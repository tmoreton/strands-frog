#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_codec_dev.h"
#include "esp_err.h"

#define LONG_TRANSCRIPT_PAGE_CAPACITY 8448

typedef struct {
    esp_err_t error;
    unsigned segments;
    unsigned gaps; // Number of lost audio chunks, including grouped gaps.
    unsigned duration_seconds;
    bool complete;
} long_transcript_result_t;

// Prepare before scheduling the audio worker. Stop can be requested from another task.
void long_transcript_prepare(void);
void long_transcript_request_stop(void);

// Run on the task that owns the microphone.
// on_finishing is called after the mic closes and before waiting for pending uploads.
long_transcript_result_t long_transcript_run(esp_codec_dev_handle_t microphone, void (*on_finishing)(void));

bool long_transcript_complete(void);
unsigned long_transcript_page_count(void);
// Pages are zero-based and hold up to four timestamped entries.
esp_err_t long_transcript_read_page(unsigned page, char *buffer, size_t size);
