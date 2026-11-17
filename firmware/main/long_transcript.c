#include "long_transcript.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "voice_client.h"

#define SAMPLE_RATE 16000
#define READ_FRAMES (SAMPLE_RATE / 50)
#define MIN_FRAMES (SAMPLE_RATE / 4)
#define CHUNK_SECONDS 10
#define CHUNK_FRAMES (SAMPLE_RATE * CHUNK_SECONDS)
#define LONG_RECORDING_SECONDS (60 * 60)
#define MAX_CHUNKS (LONG_RECORDING_SECONDS / CHUNK_SECONDS)
#define SLOT_COUNT 4
#define ENTRIES_PER_PAGE 4
#define TEXT_CAPACITY 2049
#define STAMP_BYTES 8 // "[00:00]\n"
#define RECORD_CAPACITY (STAMP_BYTES + TEXT_CAPACITY)
#define TRANSCRIPT_FLASH_BUDGET (360 * 1024)

// The uploader writes to NVS. Its stack can use PSRAM only when flash writes
// leave the cache enabled, as they do with PSRAM XIP and an unshared SPI1 bus.
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_SPIRAM_FETCH_INSTRUCTIONS && \
    CONFIG_SPIRAM_RODATA && !CONFIG_SPI_FLASH_SHARE_SPI1_BUS
#define UPLOAD_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define UPLOAD_STACK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

static const char *TAG = "long_transcript";
// Retain the storage namespace used by existing firmware.
static const char *NAMESPACE = "frog_meeting";
static atomic_bool stop_requested;

typedef struct {
    char riff[4];
    uint32_t riff_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
} wav_header_t;

#define WAV_SLOT_BYTES (sizeof(wav_header_t) + CHUNK_FRAMES * sizeof(int16_t))

_Static_assert(sizeof(wav_header_t) == 44, "WAV header must be 44 bytes");
_Static_assert(CHUNK_FRAMES % READ_FRAMES == 0, "I2S reads must fit WAV slots");
_Static_assert(LONG_TRANSCRIPT_PAGE_CAPACITY >= ENTRIES_PER_PAGE * (RECORD_CAPACITY + 2),
               "Transcript page must fit four complete entries");

typedef struct {
    uint8_t *wav;
    unsigned index;
    unsigned frames;
} audio_chunk_t;

typedef struct {
    QueueHandle_t available;
    QueueHandle_t pending;
    SemaphoreHandle_t finished;
    nvs_handle_t store;
    char *record;
    unsigned records;
    unsigned segments;
    unsigned gaps;
    size_t flash_used;
    esp_err_t storage_error;
} transcript_upload_t;

void long_transcript_prepare(void)
{
    atomic_store(&stop_requested, false);
}

void long_transcript_request_stop(void)
{
    atomic_store(&stop_requested, true);
}

static void record_key(char key[16], unsigned index)
{
    snprintf(key, 16, "entry%03u", index);
}

static esp_err_t start_transcript(nvs_handle_t store)
{
    // Invalidate the prior header before erasing its entries. An interrupted replace
    // cannot expose a mix of old and new recordings.
    esp_err_t err = nvs_erase_key(store, "started");
    if (err == ESP_OK) {
        err = nvs_commit(store);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(store);
    if (err != ESP_OK) {
        return err;
    }
    time_t now = time(NULL);
    err = nvs_set_i64(store, "started", now > 0 ? (int64_t)now : 0);
    return err == ESP_OK ? nvs_commit(store) : err;
}

static bool save_record(transcript_upload_t *upload, unsigned chunk_index)
{
    unsigned seconds = chunk_index * CHUNK_SECONDS;
    char stamp[STAMP_BYTES + 1];
    snprintf(stamp, sizeof(stamp), "[%02u:%02u]\n", (seconds / 60) % 60, seconds % 60);
    memcpy(upload->record, stamp, STAMP_BYTES);

    char key[16];
    record_key(key, upload->records);
    size_t bytes = strlen(upload->record) + 1;
    // Budget 32-byte NVS entries, including overhead for blobs split across flash pages.
    size_t flash_cost = ((bytes + 31) / 32 + 4) * 32;
    esp_err_t err = ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    if (upload->flash_used + flash_cost <= TRANSCRIPT_FLASH_BUDGET) {
        err = nvs_set_blob(upload->store, key, upload->record, bytes);
    }
    if (err == ESP_OK) {
        err = nvs_commit(upload->store);
    }
    if (err != ESP_OK) {
        upload->storage_error = err;
        long_transcript_request_stop();
        ESP_LOGE(TAG, "Couldn't save a transcript entry: %s", esp_err_to_name(err));
        return false;
    }
    upload->flash_used += flash_cost;
    ++upload->records;
    return true;
}

static void save_gap(transcript_upload_t *upload, unsigned first, unsigned count)
{
    upload->gaps += count;
    if (upload->storage_error == ESP_OK) {
        snprintf(upload->record + STAMP_BYTES, TEXT_CAPACITY, "Audio unavailable (%u chunk%s).",
                 count, count == 1 ? "" : "s");
        save_record(upload, first);
    }
}

static void upload_worker(void *context)
{
    transcript_upload_t *upload = context;
    unsigned next_index = 0;
    char *text = upload->record + STAMP_BYTES;
    for (;;) {
        audio_chunk_t chunk;
        xQueueReceive(upload->pending, &chunk, portMAX_DELAY);
        if (chunk.index > next_index) {
            save_gap(upload, next_index, chunk.index - next_index);
        }
        if (chunk.wav == NULL) {
            break;
        }
        next_index = chunk.index + 1;
        esp_err_t err = upload->storage_error;
        if (err == ESP_OK) {
            text[0] = '\0';
            err = voice_client_transcribe(chunk.wav,
                                          sizeof(wav_header_t) + chunk.frames * sizeof(int16_t),
                                          text, TEXT_CAPACITY);
            text[TEXT_CAPACITY - 1] = '\0';
        }
        xQueueSend(upload->available, &chunk.wav, 0);

        if (err != ESP_OK) {
            save_gap(upload, chunk.index, 1);
        } else if (text[0] != '\0') {
            if (save_record(upload, chunk.index)) {
                ++upload->segments;
            } else {
                ++upload->gaps;
            }
        }
    }
    xSemaphoreGive(upload->finished);
    // The recorder deletes this task and frees its stack after the handoff.
    vTaskSuspend(NULL);
}

static void submit_audio(transcript_upload_t *upload, uint8_t *wav, unsigned index, unsigned frames)
{
    if (wav == NULL) {
        // The next queued index, or the end marker, reports the missing window.
        return;
    }
    uint32_t bytes = frames * sizeof(int16_t);
    const wav_header_t header = {
        .riff = "RIFF",
        .riff_size = sizeof(header) - 8 + bytes,
        .wave = "WAVE",
        .fmt = "fmt ",
        .fmt_size = 16,
        .format = 1,
        .channels = 1,
        .sample_rate = SAMPLE_RATE,
        .byte_rate = SAMPLE_RATE * sizeof(int16_t),
        .block_align = sizeof(int16_t),
        .bits_per_sample = 16,
        .data = "data",
        .data_size = bytes,
    };
    memcpy(wav, &header, sizeof(header));
    const audio_chunk_t chunk = {.wav = wav, .index = index, .frames = frames};
    // Each pending chunk owns a slot, so the queue always has room.
    xQueueSend(upload->pending, &chunk, 0);
}

static unsigned capture_audio(transcript_upload_t *upload, esp_codec_dev_handle_t microphone,
                              long_transcript_result_t *result)
{
    int64_t start = esp_timer_get_time();
    int64_t deadline = start + LONG_RECORDING_SECONDS * INT64_C(1000000);
    int16_t stereo[READ_FRAMES * 2];
    uint8_t *wav = NULL;
    unsigned frames = 0;
    unsigned index = 0;
    while (index < MAX_CHUNKS && !atomic_load(&stop_requested) && esp_timer_get_time() < deadline) {
        if (frames == 0) {
            xQueueReceive(upload->available, &wav, 0);
        }
        int err = esp_codec_dev_read(microphone, stereo, sizeof(stereo));
        if (atomic_load(&stop_requested)) {
            break; // Discard the read that straddled Stop.
        }
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Long recording microphone read failed: %d", err);
            result->error = ESP_FAIL;
            break;
        }
        int16_t *mono = wav == NULL ? NULL : (int16_t *)(wav + sizeof(wav_header_t)) + frames;
        unsigned magnitude = 0;
        for (unsigned i = 0; i < READ_FRAMES; ++i) {
            int sample = (stereo[2 * i] + stereo[2 * i + 1]) / 2;
            magnitude += (unsigned)abs(sample);
            if (mono != NULL) {
                mono[i] = (int16_t)sample;
            }
        }
        unsigned mean = magnitude / READ_FRAMES;
        unsigned signal = mean > 36 ? mean - 36 : 0;
        board_ui_set_audio_level(100 * signal / (signal + 240));
        frames += READ_FRAMES;
        if (frames == CHUNK_FRAMES) {
            submit_audio(upload, wav, index++, frames);
            wav = NULL;
            frames = 0;
        }
    }

    result->duration_seconds = (unsigned)((esp_timer_get_time() - start) / 1000000);
    if (result->duration_seconds > LONG_RECORDING_SECONDS) {
        result->duration_seconds = LONG_RECORDING_SECONDS;
    }
    if (frames >= MIN_FRAMES) {
        submit_audio(upload, wav, index++, frames);
    }
    return index;
}

long_transcript_result_t long_transcript_run(esp_codec_dev_handle_t microphone, void (*on_finishing)(void))
{
    long_transcript_result_t result = {0};
    if (atomic_load(&stop_requested)) {
        return result;
    }
    if (microphone == NULL) {
        result.error = ESP_ERR_INVALID_STATE;
        return result;
    }

    transcript_upload_t upload = {
        .available = xQueueCreate(SLOT_COUNT, sizeof(uint8_t *)),
        .pending = xQueueCreate(SLOT_COUNT + 1, sizeof(audio_chunk_t)),
        .finished = xSemaphoreCreateBinary(),
    };
    uint8_t *memory = heap_caps_malloc(SLOT_COUNT * WAV_SLOT_BYTES + RECORD_CAPACITY,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (upload.available == NULL || upload.pending == NULL || upload.finished == NULL ||
        memory == NULL) {
        result.error = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    for (unsigned i = 0; i < SLOT_COUNT; ++i) {
        uint8_t *slot = memory + i * WAV_SLOT_BYTES;
        xQueueSend(upload.available, &slot, 0);
    }
    upload.record = (char *)(memory + SLOT_COUNT * WAV_SLOT_BYTES);

    result.error = nvs_open(NAMESPACE, NVS_READWRITE, &upload.store);
    if (result.error != ESP_OK) {
        goto cleanup;
    }
    if (atomic_load(&stop_requested)) {
        goto close_store;
    }
    esp_codec_dev_sample_info_t format = {
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel = 2,
    };
    if (esp_codec_dev_open(microphone, &format) != ESP_CODEC_DEV_OK) {
        esp_codec_dev_close(microphone);
        result.error = ESP_FAIL;
        goto close_store;
    }
    TaskHandle_t worker;
    if (xTaskCreateWithCaps(upload_worker, "transcript_upload", 8192, &upload, 4, &worker,
                           UPLOAD_STACK_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "Couldn't allocate the transcript upload task");
        esp_codec_dev_close(microphone);
        result.error = ESP_ERR_NO_MEM;
        goto close_store;
    }

    bool started = false;
    unsigned chunks = 0;
    if (!atomic_load(&stop_requested)) {
        result.error = start_transcript(upload.store);
        if (result.error == ESP_OK) {
            started = true;
            chunks = capture_audio(&upload, microphone, &result);
        }
    }
    esp_codec_dev_close(microphone);
    board_ui_set_audio_level(0);
    if (on_finishing != NULL) {
        on_finishing();
    }
    const audio_chunk_t end = {.index = chunks};
    xQueueSend(upload.pending, &end, 0);
    xSemaphoreTake(upload.finished, portMAX_DELAY);
    vTaskDeleteWithCaps(worker);

    result.segments = upload.segments;
    result.gaps = upload.gaps;
    if (result.error == ESP_OK) {
        result.error = upload.storage_error;
    }
    if (started) {
        result.complete = result.error == ESP_OK && result.gaps == 0;
        esp_err_t err = nvs_set_u8(upload.store, "complete", result.complete);
        if (err == ESP_OK) {
            err = nvs_commit(upload.store);
        }
        if (err != ESP_OK) {
            result.complete = false;
            if (result.error == ESP_OK) {
                result.error = err;
            }
        }
    }

close_store:
    nvs_close(upload.store);
cleanup:
    if (upload.available != NULL) {
        vQueueDelete(upload.available);
    }
    if (upload.pending != NULL) {
        vQueueDelete(upload.pending);
    }
    if (upload.finished != NULL) {
        vSemaphoreDelete(upload.finished);
    }
    free(memory);
    return result;
}

static esp_err_t open_transcript(nvs_handle_t *handle)
{
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, handle);
    if (err == ESP_OK) {
        int64_t started;
        err = nvs_get_i64(*handle, "started", &started);
        if (err != ESP_OK) {
            nvs_close(*handle);
        }
    }
    return err;
}

bool long_transcript_complete(void)
{
    nvs_handle_t handle;
    uint8_t complete = 0;
    if (open_transcript(&handle) == ESP_OK) {
        nvs_get_u8(handle, "complete", &complete);
        nvs_close(handle);
    }
    return complete == 1;
}

unsigned long_transcript_page_count(void)
{
    nvs_handle_t handle;
    if (open_transcript(&handle) != ESP_OK) {
        return 0;
    }
    // Entries are committed in order. A power cut leaves a readable prefix.
    unsigned low = 0;
    unsigned high = MAX_CHUNKS;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        char key[16];
        record_key(key, middle);
        size_t bytes = 0;
        if (nvs_get_blob(handle, key, NULL, &bytes) == ESP_OK) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    nvs_close(handle);
    return (low + ENTRIES_PER_PAGE - 1) / ENTRIES_PER_PAGE;
}

esp_err_t long_transcript_read_page(unsigned page, char *buffer, size_t size)
{
    if (buffer == NULL || size == 0 || page >= MAX_CHUNKS / ENTRIES_PER_PAGE) {
        return ESP_ERR_INVALID_ARG;
    }
    buffer[0] = '\0';
    nvs_handle_t handle;
    esp_err_t err = open_transcript(&handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t used = 0;
    for (unsigned i = 0; i < ENTRIES_PER_PAGE; ++i) {
        char key[16];
        record_key(key, page * ENTRIES_PER_PAGE + i);
        size_t bytes = 0;
        err = nvs_get_blob(handle, key, NULL, &bytes);
        if (err != ESP_OK) {
            if (err == ESP_ERR_NVS_NOT_FOUND && used != 0) {
                err = ESP_OK;
            }
            break;
        }
        size_t offset = used == 0 ? 0 : used + 2;
        if (offset >= size || bytes > size - offset || bytes > RECORD_CAPACITY || bytes <= STAMP_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = nvs_get_blob(handle, key, buffer + offset, &bytes);
        if (err != ESP_OK) {
            break;
        }
        if (buffer[offset + bytes - 1] != '\0') {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (used != 0) {
            memcpy(buffer + used, "\n\n", 2);
        }
        used = offset + bytes - 1;
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        buffer[0] = '\0';
    }
    return err;
}
