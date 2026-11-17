#include "wifi_setup.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "board_ui.h"
#include "bootloader_random.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"
#include "portal_dns.h"
#include "usb_setup.h"

#define SCAN_RESULTS 24
#define PORTAL_HOSTNAME "frog"
#define PORTAL_LOCAL_HOST PORTAL_HOSTNAME ".local"

extern const char setup_html[] asm("_binary_wifi_setup_html_start");
extern const char setup_svg_start[] asm("_binary_strands_frog_svg_start");
extern const char setup_svg_end[] asm("_binary_strands_frog_svg_end");

typedef struct {
    char ssid[33];
    bool secured;
} setup_network_t;

static const char *TAG = "wifi_setup";
static char form_nonce[25];
static char portal_host[16];
static char usb_host[16];
static char portal_url[sizeof("http:///") + sizeof(portal_host) - 1];
static esp_timer_handle_t reboot_timer;
static esp_err_t (*save_wifi)(const char *ssid, const char *password);
static portMUX_TYPE scan_lock = portMUX_INITIALIZER_UNLOCKED;
static bool scanning;
static size_t network_count;
static setup_network_t networks[SCAN_RESULTS];

static bool is_portal_host(httpd_req_t *req)
{
    char host[sizeof(portal_host) + sizeof(":80") - 1];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    char *port = strchr(host, ':');
    if (port != NULL) {
        if (strcmp(port, ":80") != 0) {
            return false;
        }
        *port = '\0';
    }
    return strcmp(host, portal_host) == 0 || (usb_host[0] != '\0' && strcmp(host, usb_host) == 0) ||
           strcasecmp(host, PORTAL_LOCAL_HOST) == 0 || strcasecmp(host, PORTAL_LOCAL_HOST ".") == 0;
}

static esp_err_t redirect_to_portal(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", portal_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // The captive network check on iOS also expects a nonempty response body.
    return httpd_resp_sendstr(req, "Open Strands Frog Wi-Fi setup.");
}

static esp_err_t redirect_unknown_page(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    if (req->method == HTTP_GET && !is_portal_host(req)) {
        return redirect_to_portal(req);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Open the setup address shown on the frog.");
}

static esp_err_t show_form(httpd_req_t *req)
{
    if (!is_portal_host(req)) {
        return redirect_to_portal(req);
    }
    ESP_LOGI(TAG, "Serving Wi-Fi setup page");
    const char marker[] = "__SETUP_NONCE__";
    const char *nonce = strstr(setup_html, marker);
    if (nonce == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Couldn't open Wi-Fi setup.");
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, setup_html, nonce - setup_html), TAG, "Send setup page");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, form_nonce), TAG, "Send setup nonce");
    ESP_RETURN_ON_ERROR(httpd_resp_sendstr_chunk(req, nonce + sizeof(marker) - 1), TAG, "Finish setup page");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t show_frog(httpd_req_t *req)
{
    if (!is_portal_host(req)) {
        return redirect_to_portal(req);
    }
    httpd_resp_set_type(req, "image/svg+xml");
    return httpd_resp_send(req, setup_svg_start, setup_svg_end - setup_svg_start);
}

static esp_err_t start_network_scan(void)
{
    portENTER_CRITICAL(&scan_lock);
    bool already_running = scanning;
    scanning = true;
    portEXIT_CRITICAL(&scan_lock);
    if (already_running) {
        return ESP_OK;
    }
    const wifi_scan_config_t config = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 40, .max = 90},
        .home_chan_dwell_time = 45,
    };
    esp_err_t err = esp_wifi_scan_start(&config, false);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&scan_lock);
        scanning = false;
        portEXIT_CRITICAL(&scan_lock);
    }
    return err;
}

static bool supported_network(wifi_auth_mode_t auth)
{
    return auth == WIFI_AUTH_OPEN || auth == WIFI_AUTH_WPA2_PSK || auth == WIFI_AUTH_WPA_WPA2_PSK ||
           auth == WIFI_AUTH_WPA3_PSK || auth == WIFI_AUTH_WPA2_WPA3_PSK;
}

static void scan_finished(void *context, esp_event_base_t base, int32_t event, void *data)
{
    (void)context;
    (void)base;
    (void)event;
    const wifi_event_sta_scan_done_t *result = data;
    wifi_ap_record_t *records = result->status == 0 ? calloc(SCAN_RESULTS, sizeof(*records)) : NULL;
    uint16_t count = SCAN_RESULTS;
    bool complete = records != NULL && esp_wifi_scan_get_ap_records(&count, records) == ESP_OK;
    if (!complete) {
        esp_wifi_clear_ap_list();
    }

    portENTER_CRITICAL(&scan_lock);
    if (complete) {
        network_count = 0;
        // ESP-IDF returns strongest first. Keep one entry for each SSID.
        for (size_t i = 0; i < count; ++i) {
            const char *ssid = (const char *)records[i].ssid;
            if (!ssid[0] || !supported_network(records[i].authmode)) {
                continue;
            }
            size_t j = 0;
            while (j < network_count && strcmp(networks[j].ssid, ssid) != 0) {
                ++j;
            }
            if (j == network_count) {
                memcpy(networks[j].ssid, ssid, 32);
                networks[j].ssid[32] = '\0';
                networks[j].secured = records[i].authmode != WIFI_AUTH_OPEN;
                ++network_count;
            }
        }
    }
    size_t found = network_count;
    scanning = false;
    portEXIT_CRITICAL(&scan_lock);
    free(records);
    if (complete) {
        ESP_LOGI(TAG, "Setup Wi-Fi scan found %u networks", (unsigned)found);
    } else {
        ESP_LOGW(TAG, "Setup Wi-Fi scan did not complete");
    }
}

static esp_err_t show_networks(httpd_req_t *req)
{
    if (!is_portal_host(req)) {
        return redirect_to_portal(req);
    }
    setup_network_t snapshot[SCAN_RESULTS];
    portENTER_CRITICAL(&scan_lock);
    size_t count = network_count;
    bool running = scanning;
    memcpy(snapshot, networks, count * sizeof(*snapshot));
    portEXIT_CRITICAL(&scan_lock);

    char *json = NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(root, "networks");
    bool ok = list && cJSON_AddBoolToObject(root, "scanning", running);
    for (size_t i = 0; ok && i < count; ++i) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL || !cJSON_AddItemToArray(list, entry)) {
            cJSON_Delete(entry);
            ok = false;
        } else {
            ok = cJSON_AddStringToObject(entry, "ssid", snapshot[i].ssid) &&
                 cJSON_AddBoolToObject(entry, "secured", snapshot[i].secured);
        }
    }
    if (ok) {
        json = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Couldn't list Wi-Fi networks.");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t scan_again(httpd_req_t *req)
{
    if (!is_portal_host(req) || req->content_len != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi scan request.");
    }
    if (start_network_scan() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Couldn't scan for Wi-Fi.");
    }
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, NULL, 0);
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = tolower((unsigned char)ch);
    return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

static bool decode_field(const char *encoded, char *field, size_t capacity)
{
    size_t length = 0;
    while (*encoded) {
        unsigned char byte = *encoded++;
        if (byte == '+') {
            byte = ' ';
        } else if (byte == '%') {
            if (!encoded[0] || !encoded[1]) {
                return false;
            }
            int high = hex_digit(encoded[0]);
            int low = hex_digit(encoded[1]);
            if (high < 0 || low < 0) {
                return false;
            }
            byte = (high << 4) | low;
            encoded += 2;
        }
        if (byte < 32 || byte == 127 || length + 1 >= capacity) {
            return false;
        }
        field[length++] = byte;
    }
    field[length] = '\0';
    return true;
}

static bool parse_form(char *form, char ssid[33], char password[65])
{
    bool valid_nonce = false;
    char *remaining;
    for (char *field = strtok_r(form, "&", &remaining); field != NULL;
         field = strtok_r(NULL, "&", &remaining)) {
        char *value = strchr(field, '=');
        if (value == NULL) {
            return false;
        }
        *value++ = '\0';
        if (strcmp(field, "nonce") == 0) {
            valid_nonce = strcmp(value, form_nonce) == 0;
        } else if (strcmp(field, "ssid") == 0) {
            if (!decode_field(value, ssid, 33)) {
                return false;
            }
        } else if (strcmp(field, "password") == 0 && !decode_field(value, password, 65)) {
            return false;
        }
    }
    size_t password_length = strlen(password);
    return valid_nonce && ssid[0] && (password_length == 0 ||
        (password_length >= 8 && (password_length < 64 ||
         strspn(password, "0123456789abcdefABCDEF") == 64)));
}

static esp_err_t save_form(httpd_req_t *req)
{
    if (!is_portal_host(req)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Open the setup address shown on the frog.");
    }
    char form[384];
    size_t length = req->content_len;
    if (length == 0 || length >= sizeof(form)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi form.");
    }
    for (size_t offset = 0; offset < length;) {
        int received = httpd_req_recv(req, form + offset, length - offset);
        if (received <= 0) {
            return ESP_FAIL;
        }
        offset += received;
    }
    form[length] = '\0';
    char ssid[33] = {0};
    char password[65] = {0};
    if (!parse_form(form, ssid, password)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                  "Check the network name and password, then reload the setup page.");
    }
    if (save_wifi(ssid, password) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Couldn't save Wi-Fi settings.");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ESP_LOGI(TAG, "Wi-Fi settings saved; restarting");
    esp_err_t result = httpd_resp_sendstr(req, "Wi-Fi credentials saved. The frog will wave once it joins Wi-Fi.");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_start_once(reboot_timer, 900000));
    return result;
}

static void reboot(void *context)
{
    (void)context;
    esp_restart();
}

static void log_phone_connection(void *context, esp_event_base_t base, int32_t event, void *data)
{
    (void)context;
    (void)data;
    if (base == IP_EVENT && event == IP_EVENT_AP_STAIPASSIGNED) {
        ESP_LOGI(TAG, "Setup network assigned a client address");
    } else if (base == WIFI_EVENT && event == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Setup network client joined");
    } else if (base == WIFI_EVENT && event == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "Setup network client left");
    }
}

static esp_err_t start_portal_discovery(esp_netif_t *usb)
{
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "Start portal discovery");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(PORTAL_HOSTNAME), TAG, "Set portal name");
    if (usb != NULL) {
        ESP_RETURN_ON_ERROR(mdns_register_netif(usb), TAG, "Register USB for portal discovery");
        ESP_RETURN_ON_ERROR(mdns_netif_action(usb, MDNS_EVENT_ENABLE_IP4), TAG, "Advertise portal over USB");
    }
    return ESP_OK;
}

esp_err_t wifi_setup_start(esp_err_t (*save)(const char *ssid, const char *password))
{
    save_wifi = save;
    // Setup runs without audio/ADC. Seed the password before the Wi-Fi radio starts.
    uint8_t random[8];
    bootloader_random_enable();
    esp_fill_random(random, sizeof(random));
    bootloader_random_disable();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Initialize network");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Start event loop");
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(ap, ESP_FAIL, TAG, "Create setup access point");
    esp_netif_ip_info_t ip;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(ap, &ip), TAG, "Read setup network address");
    ESP_RETURN_ON_FALSE(ip.ip.addr != 0, ESP_FAIL, TAG, "Setup network has no address");
    snprintf(portal_host, sizeof(portal_host), IPSTR, IP2STR(&ip.ip));
    snprintf(portal_url, sizeof(portal_url), "http://%s/", portal_host);

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                                    log_phone_connection, NULL), TAG, "Watch setup joins");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                                    log_phone_connection, NULL), TAG, "Watch setup leaves");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                                    log_phone_connection, NULL), TAG, "Watch setup leases");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                                    scan_finished, NULL), TAG, "Watch network scans");
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Set temporary Wi-Fi storage");

    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "Read setup address");
    wifi_config_t config = {.ap = {.channel = 1, .max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK}};
    snprintf((char *)config.ap.ssid, sizeof(config.ap.ssid), "Strands-Frog-%02X%02X", mac[4], mac[5]);
    static const char alphabet[] = "23456789abcdefghijkmnpqrstuvwxyz";
    _Static_assert(sizeof(alphabet) == 33, "Use 32 distinct setup password characters");
    for (size_t i = 0; i < sizeof(random); ++i) {
        config.ap.password[i] = alphabet[random[i] & 31];
    }
    // Station mode is enabled only for scanning here; it never connects upstream.
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "Set setup and scan mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG, "Configure access point");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Start setup Wi-Fi");
    snprintf(form_nonce, sizeof(form_nonce), "%08lX%08lX%08lX", (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random());

    esp_netif_t *usb = NULL;
    esp_err_t usb_result = usb_setup_start(&usb);
    esp_netif_ip_info_t usb_ip;
    if (usb_result == ESP_OK) {
        usb_result = esp_netif_get_ip_info(usb, &usb_ip);
    }
    if (usb_result == ESP_OK) {
        snprintf(usb_host, sizeof(usb_host), IPSTR, IP2STR(&usb_ip.ip));
    } else {
        ESP_LOGW(TAG, "USB setup address unavailable: %s", esp_err_to_name(usb_result));
    }

    const esp_timer_create_args_t timer_config = {.callback = reboot, .name = "setup_reboot"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_config, &reboot_timer), TAG, "Create restart timer");
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    // Leave room for HTTPD's three internal sockets, the DNS socket, and DHCP.
    server_config.max_open_sockets = 5;
    server_config.lru_purge_enable = true;
    server_config.recv_wait_timeout = 3;
    server_config.stack_size = 6144;
    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &server_config), TAG, "Start setup page");
    const httpd_uri_t form = {.uri = "/", .method = HTTP_GET, .handler = show_form};
    const httpd_uri_t frog = {.uri = "/frog.svg", .method = HTTP_GET, .handler = show_frog};
    const httpd_uri_t list = {.uri = "/networks", .method = HTTP_GET, .handler = show_networks};
    const httpd_uri_t scan = {.uri = "/scan", .method = HTTP_POST, .handler = scan_again};
    const httpd_uri_t submit = {.uri = "/save", .method = HTTP_POST, .handler = save_form};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &form), TAG, "Register setup page");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &frog), TAG, "Register frog logo");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &list), TAG, "Register network list");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &scan), TAG, "Register network scan");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &submit), TAG, "Register setup form");
    ESP_RETURN_ON_ERROR(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_unknown_page),
                        TAG, "Register setup redirect");
    // The default DHCP configuration advertises the SoftAP's address as DNS.
    // Keep the direct IP form available even if DNS couldn't start.
    ESP_ERROR_CHECK_WITHOUT_ABORT(portal_dns_start(ip.ip.addr));

    bool named = start_portal_discovery(usb) == ESP_OK;
    const char *connection = named && usb_host[0] ? "USB-C or join" : "Join";
    char instructions[105];
    snprintf(instructions, sizeof(instructions), "%s %.17s\nPassword %.8s\nOpen %s", connection,
             (const char *)config.ap.ssid, (const char *)config.ap.password,
             named ? "http://" PORTAL_LOCAL_HOST "/" : portal_url);
    board_ui_show(BOARD_UI_SETUP, instructions);
    ESP_LOGI(TAG, "Wi-Fi setup ready");
    ESP_ERROR_CHECK_WITHOUT_ABORT(start_network_scan());
    return ESP_OK;
}
