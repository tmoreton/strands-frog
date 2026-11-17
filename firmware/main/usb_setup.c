#include "usb_setup.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"
#include "tusb.h"

#define ETHERNET_HEADER_BYTES 14
#define ETHERNET_MAX_BYTES (1500 + ETHERNET_HEADER_BYTES)
#define USB_TX_LIMIT 4
#define USB_RECONNECT_MS 100

static const char *TAG = "usb_setup";
static esp_netif_t *usb_interface;
static atomic_bool usb_ready;
static atomic_uint queued_transmits;

static void restore_serial_phy(void)
{
    // ESP32-S3's RTC USB mux can survive a CPU-only software restart.
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
}

static void stop_usb_for_restart(void)
{
    if (atomic_exchange(&usb_ready, false)) {
        tud_disconnect();
        // Let the host observe the detach before the serial controller takes over.
        esp_rom_delay_us(20000);
    }
    restore_serial_phy();
}

static bool valid_frame(size_t length)
{
    return length >= ETHERNET_HEADER_BYTES && length <= ETHERNET_MAX_BYTES;
}

static void free_receive_frame(void *driver, void *buffer)
{
    (void)driver;
    free(buffer);
}

static esp_err_t receive_frame(void *buffer, uint16_t length, void *context)
{
    if (!atomic_load(&usb_ready) || !valid_frame(length)) {
        return ESP_ERR_INVALID_STATE;
    }
    // TinyUSB reuses the RX buffer when this callback returns. esp-netif's
    // Ethernet input owns the copy, including when it drops the packet.
    void *copy = malloc(length);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, length);
    return esp_netif_receive(context, copy, length, NULL);
}

static void free_transmit_frame(void *buffer, void *context)
{
    (void)context;
    free(buffer);
    atomic_fetch_sub(&queued_transmits, 1);
}

static esp_err_t transmit_frame(void *driver, void *buffer, size_t length)
{
    (void)driver;
    if (!atomic_load(&usb_ready)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!valid_frame(length)) {
        return ESP_ERR_INVALID_SIZE;
    }
    // The lwIP output buffer may be freed on return. Bound the copies waiting
    // for the USB task so a slow host does not hold up Wi-Fi or exhaust memory.
    if (atomic_fetch_add(&queued_transmits, 1) >= USB_TX_LIMIT) {
        atomic_fetch_sub(&queued_transmits, 1);
        return ESP_ERR_NO_MEM;
    }
    void *copy = malloc(length);
    if (copy == NULL) {
        atomic_fetch_sub(&queued_transmits, 1);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, length);
    esp_err_t err = tinyusb_net_send_async(copy, (uint16_t)length, copy);
    if (err != ESP_OK) {
        free_transmit_frame(copy, NULL);
    }
    return err;
}

esp_err_t usb_setup_start(esp_netif_t **netif)
{
    if (netif == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *netif = usb_interface;
    if (usb_interface != NULL) {
        return ESP_OK;
    }

    tinyusb_net_config_t usb_config = {
        .on_recv_callback = receive_frame,
        .free_tx_buffer = free_transmit_frame,
    };
    esp_err_t err = esp_read_mac(usb_config.mac_addr, ESP_MAC_ETH);
    if (err != ESP_OK) {
        return err;
    }
    const esp_netif_ip_info_t ip = {
        .ip.addr = ESP_IP4TOADDR(192, 168, 7, 1),
        .netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0),
    };
    esp_netif_inherent_config_t inherent = {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .if_key = "FROG_USB_NCM",
        .if_desc = "usb",
        .ip_info = &ip,
    };
    // NCM advertises the MAC the host uses for its virtual adapter. Give the
    // Frog's side of the Ethernet link a different locally administered MAC.
    memcpy(inherent.mac, usb_config.mac_addr, sizeof(inherent.mac));
    inherent.mac[0] |= 0x02;
    inherent.mac[5] ^= 0x01;
    const esp_netif_driver_ifconfig_t driver = {
        .handle = &usb_ready,
        .transmit = transmit_frame,
        .driver_free_rx_buffer = free_receive_frame,
    };
    const esp_netif_config_t config = {
        .base = &inherent,
        .driver = &driver,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    esp_netif_t *interface = esp_netif_new(&config);
    if (interface == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t offer = 0;
    err = esp_netif_dhcps_option(interface, ESP_NETIF_OP_SET,
                                ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &offer, sizeof(offer));
    if (err == ESP_OK) {
        err = esp_netif_dhcps_option(interface, ESP_NETIF_OP_SET,
                                    ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    }
    if (err != ESP_OK) {
        goto destroy_netif;
    }

    // Get DHCP ready before the host receives NCM's first link-up notification.
    esp_netif_action_start(interface, NULL, 0, NULL);
    esp_netif_dhcp_status_t status;
    err = esp_netif_dhcps_get_status(interface, &status);
    if (err != ESP_OK || status != ESP_NETIF_DHCP_STARTED) {
        if (err == ESP_OK) {
            err = ESP_ERR_ESP_NETIF_DHCPS_START_FAILED;
        }
        goto stop_netif;
    }

    usb_config.user_context = interface;
    err = esp_register_shutdown_handler(stop_usb_for_restart);
    if (err != ESP_OK) {
        goto stop_netif;
    }
    const tinyusb_config_t tinyusb = TINYUSB_DEFAULT_CONFIG();
    err = tinyusb_driver_install(&tinyusb);
    if (err != ESP_OK) {
        goto restore_phy;
    }
    // The driver installs the default descriptors before the NET class adds
    // the MAC string. Reconnect after the class and descriptors are ready.
    tud_disconnect();
    err = tinyusb_net_init(&usb_config);
    if (err != ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(tinyusb_driver_uninstall());
        goto restore_phy;
    }
    vTaskDelay(pdMS_TO_TICKS(USB_RECONNECT_MS));
    atomic_store(&usb_ready, true);
    tud_connect();
    usb_interface = interface;
    *netif = interface;
    ESP_LOGI(TAG, "USB Ethernet setup ready at 192.168.7.1");
    return ESP_OK;

restore_phy:
    esp_unregister_shutdown_handler(stop_usb_for_restart);
    restore_serial_phy();
stop_netif:
    esp_netif_action_stop(interface, NULL, 0, NULL);
destroy_netif:
    esp_netif_destroy(interface);
    return err;
}
