#include "voice_client.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "cloud_settings.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#define REPLY_CAPACITY 4096
#define SPEECH_MAX_BYTES (16000 * 2 * 20)
#define SECONDS_PER_DAY 86400
#define UPLOAD_CHUNK_BYTES 2048
#define UPLOAD_TIMEOUT_MS 9000
#define TRANSCRIBE_RESPONSE_TIMEOUT_MS 16000
#define VOICE_RESPONSE_TIMEOUT_MS 32000

_Static_assert(sizeof(STRANDS_DEVICE_TOKEN) > 1, "Set the device token in main/cloud_settings.h");

static const char *TAG = "voice_client";

static int two_digits(const char *value)
{
    if (value[0] < '0' || value[0] > '9' || value[1] < '0' || value[1] > '9') {
        return -1;
    }
    return (value[0] - '0') * 10 + value[1] - '0';
}

static uint64_t parse_https_date(const char *value)
{
    // HTTP IMF-fixdate is a fixed-length, timezone-independent UTC timestamp.
    if (strlen(value) != 29 || memcmp(value + 3, ", ", 2) != 0 ||
        value[7] != ' ' || value[11] != ' ' || value[16] != ' ' ||
        value[19] != ':' || value[22] != ':' || memcmp(value + 25, " GMT", 4) != 0) {
        return 0;
    }
    int day = two_digits(value + 5);
    int century = two_digits(value + 12);
    int year_in_century = two_digits(value + 14);
    int hour = two_digits(value + 17);
    int minute = two_digits(value + 20);
    int second = two_digits(value + 23);
    if (century != 20 || year_in_century < 0 || day < 1 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return 0;
    }
    int year = 2000 + year_in_century;
    bool leap = year % 4 == 0;  // All accepted years are 2000 through 2099.
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    // Days since 1970 through 1999, plus prior full years since 2000.
    unsigned days = 10957 + 365 * year_in_century + (year_in_century + 3) / 4;
    for (size_t month = 0; month < sizeof(month_days) / sizeof(month_days[0]); ++month) {
        int length = month_days[month] + (month == 1 && leap);
        if (memcmp(value + 8, months + month * 3, 3) == 0) {
            if (day > length) {
                return 0;
            }
            days += day - 1;
            static const char weekdays[] = "SunMonTueWedThuFriSat";
            if (memcmp(value, weekdays + ((days + 4) % 7) * 3, 3) != 0) {
                return 0;
            }
            return (uint64_t)days * SECONDS_PER_DAY + hour * 3600 + minute * 60 + second;
        }
        days += length;
    }
    return 0;
}

static esp_err_t voice_header(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
        voice_response_t *response = event->user_data;
        if (strcasecmp(event->header_key, "Content-Type") == 0) {
            response->binary = strcasecmp(event->header_value, "application/octet-stream") == 0;
        } else if (strcasecmp(event->header_key, "Date") == 0) {
            response->https_date = parse_https_date(event->header_value);
        }
    }
    return ESP_OK;
}

static cJSON *read_reply_metadata(esp_http_client_handle_t client, int64_t length)
{
    if (length < 0 || length > REPLY_CAPACITY) {
        return NULL;
    }
    // With chunked JSON, the transfer has no advertised length.
    int limit = length == 0 ? REPLY_CAPACITY : (int)length;
    char *json = malloc(limit + 1);
    if (json == NULL) {
        return NULL;
    }
    int received = esp_http_client_read_response(client, json, limit);
    cJSON *payload = NULL;
    if (received > 0 &&
        (length == 0 ? esp_http_client_is_complete_data_received(client) : received == limit)) {
        json[received] = '\0';
        payload = cJSON_ParseWithOpts(json, NULL, true);
    }
    free(json);
    return payload;
}

static const char *metadata_text(const cJSON *metadata, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(metadata, key);
    return cJSON_IsString(item) && item->valuestring[0] != '\0' ? item->valuestring : NULL;
}

static cJSON *read_voice_reply(esp_http_client_handle_t client, int64_t length, int64_t *speech_bytes)
{
    // The body is a four-byte little-endian JSON length, then JSON, then optional PCM.
    bool chunked = esp_http_client_is_chunked_response(client);
    uint8_t prefix[4];
    if ((!chunked && length < (int64_t)sizeof(prefix)) ||
        esp_http_client_read_response(client, (char *)prefix, sizeof(prefix)) != sizeof(prefix)) {
        return NULL;
    }
    uint32_t json_bytes = (uint32_t)prefix[0] | (uint32_t)prefix[1] << 8 |
                          (uint32_t)prefix[2] << 16 | (uint32_t)prefix[3] << 24;
    if (json_bytes == 0 || (!chunked && json_bytes > length - (int64_t)sizeof(prefix))) {
        return NULL;
    }
    cJSON *metadata = read_reply_metadata(client, json_bytes);
    // ESP-IDF can still have unread body bytes after it has parsed the final chunk.
    *speech_bytes = chunked ? -1 : length - (int64_t)sizeof(prefix) - json_bytes;
    return metadata;
}

static void post_audio(const uint8_t *wav, size_t wav_bytes, bool want_speech,
                       bool transcribe_only, voice_response_t *response)
{
    const esp_http_client_config_t config = {
        .url = STRANDS_CLOUD_URL,
        .method = HTTP_METHOD_POST,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .timeout_ms = UPLOAD_TIMEOUT_MS,
        .event_handler = voice_header,
        .user_data = response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGW(TAG, "Couldn't initialize the HTTPS client");
        return;
    }

    const char *stage = "request headers";
    int status = 0;
    esp_err_t err = esp_http_client_set_header(client, "Authorization", "Bearer " STRANDS_DEVICE_TOKEN);
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "Content-Type", "audio/wav");
    }
    if (err == ESP_OK && transcribe_only) {
        err = esp_http_client_set_header(client, "X-Strands-Mode", "transcribe");
    } else if (err == ESP_OK && !want_speech) {
        err = esp_http_client_set_header(client, "X-Strands-TTS", "off");
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice request failed at %s: %s", stage, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    stage = "HTTPS open";
    err = esp_http_client_open(client, (int)wav_bytes);
    if (err != ESP_OK) {
        goto failed;
    }
    stage = "WAV upload";
    int64_t upload_started = esp_timer_get_time();
    // Let the transport check socket readiness between small TLS writes.
    for (size_t offset = 0; offset < wav_bytes;) {
        size_t remaining = wav_bytes - offset;
        int chunk = remaining < UPLOAD_CHUNK_BYTES ? (int)remaining : UPLOAD_CHUNK_BYTES;
        if (esp_http_client_write(client, (const char *)wav + offset, chunk) != chunk) {
            err = ESP_ERR_HTTP_WRITE_DATA;
            goto failed;
        }
        offset += chunk;
    }
    if (!transcribe_only) {
        ESP_LOGI(TAG, "Uploaded %zu bytes in %lld ms", wav_bytes,
                 (long long)((esp_timer_get_time() - upload_started) / 1000));
    }
    stage = "response headers";
    err = esp_http_client_set_timeout_ms(client, transcribe_only ? TRANSCRIBE_RESPONSE_TIMEOUT_MS
                                                                : VOICE_RESPONSE_TIMEOUT_MS);
    if (err != ESP_OK) {
        goto failed;
    }
    int64_t length = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    if (length < 0) {
        response->https_date = 0;
        err = length == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_HTTP_EAGAIN : ESP_FAIL;
        goto failed;
    }
    response->status = status;

    if (transcribe_only && (status == 200 || status == 422)) {
        if (status == 200 && !response->binary) {
            response->metadata = read_reply_metadata(client, length);
            response->transcript = metadata_text(response->metadata, "transcript");
        }
        esp_http_client_cleanup(client);
        return;
    }

    if (status != 200) {
        response->https_date = 0;
        response->metadata = read_reply_metadata(client, length);
        response->error = metadata_text(response->metadata, "error");
        ESP_LOGW(TAG, "Voice service returned HTTP %d", status);
        esp_http_client_cleanup(client);
        return;
    }

    stage = "reply framing";
    if (response->binary) {
        response->metadata = read_voice_reply(client, length, &response->speech_bytes);
        response->reply = metadata_text(response->metadata, "reply");
        response->transcript = metadata_text(response->metadata, "transcript");
    }
    if (response->reply == NULL) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto failed;
    }
    if (want_speech && response->speech_bytes != 0) {
        response->stream = client;
    } else {
        esp_http_client_cleanup(client);
    }
    return;

failed:
    {
        int socket_errno = esp_http_client_get_errno(client);
        int tls_code = 0;
        int tls_flags = 0;
        esp_err_t tls_err = esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
        ESP_LOGW(TAG, "Voice request failed at %s: %s (HTTP %d, TLS %s, code %d, flags 0x%x, errno %d)",
                 stage, esp_err_to_name(err), status, esp_err_to_name(tls_err), tls_code,
                 (unsigned)tls_flags, socket_errno);
        esp_http_client_cleanup(client);
    }
}

void voice_client_request(const uint8_t *wav, size_t wav_bytes, bool want_speech,
                          voice_response_t *response)
{
    post_audio(wav, wav_bytes, want_speech, false, response);
}

esp_err_t voice_client_transcribe(const uint8_t *wav, size_t wav_bytes, char *text, size_t capacity)
{
    if (text == NULL || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    text[0] = '\0';
    voice_response_t response = {0};
    post_audio(wav, wav_bytes, false, true, &response);
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    if (response.status == 422) {
        result = ESP_OK;
    } else if (response.transcript != NULL) {
        size_t length = strlen(response.transcript);
        if (length < capacity) {
            memcpy(text, response.transcript, length + 1);
            result = ESP_OK;
        } else {
            result = ESP_ERR_INVALID_SIZE;
        }
    }
    voice_client_close(&response);
    return result;
}

static int16_t *read_speech_pcm(esp_http_client_handle_t client, int64_t bytes, size_t *frames)
{
    bool chunked = bytes < 0;
    if ((!chunked && (bytes == 0 || bytes % sizeof(int16_t) != 0)) ||
        bytes > SPEECH_MAX_BYTES) {
        return NULL;
    }
    int capacity = chunked ? SPEECH_MAX_BYTES : (int)bytes;
    int16_t *pcm = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        return NULL;
    }
    int count = esp_http_client_read_response(client, (char *)pcm, capacity);
    if (chunked && count == capacity && !esp_http_client_is_complete_data_received(client)) {
        char extra;
        // Consume a possible final chunk marker, while rejecting extra audio.
        if (esp_http_client_read(client, &extra, 1) > 0) {
            free(pcm);
            return NULL;
        }
    }
    bool complete = chunked ? esp_http_client_is_complete_data_received(client) : count == capacity;
    if (!complete || count <= 0 || count % sizeof(*pcm) != 0) {
        free(pcm);
        return NULL;
    }
    *frames = count / sizeof(*pcm);
    return pcm;
}

int16_t *voice_client_read_speech(voice_response_t *response, size_t *frames)
{
    int16_t *pcm = read_speech_pcm(response->stream, response->speech_bytes, frames);
    esp_http_client_cleanup(response->stream);
    response->stream = NULL;
    return pcm;
}

void voice_client_close(voice_response_t *response)
{
    if (response->stream != NULL) {
        esp_http_client_cleanup(response->stream);
    }
    cJSON_Delete(response->metadata);
}
