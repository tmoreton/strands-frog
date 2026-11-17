#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_client.h"

typedef struct {
    const char *reply;
    const char *transcript;
    const char *error;
    uint64_t https_date;
    struct cJSON *metadata;
    esp_http_client_handle_t stream;
    int64_t speech_bytes;
    int status;
    bool binary;
} voice_response_t;

// Pass a zero-initialized response. Text remains valid until voice_client_close().
void voice_client_request(const uint8_t *wav, size_t wav_bytes, bool want_speech,
                          voice_response_t *response);

// A silent clip is a successful request with an empty transcript.
esp_err_t voice_client_transcribe(const uint8_t *wav, size_t wav_bytes, char *text, size_t capacity);

// Read the optional PCM into PSRAM and close the stream. The caller owns the samples.
int16_t *voice_client_read_speech(voice_response_t *response, size_t *frames);
void voice_client_close(voice_response_t *response);
