#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "board_ui.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/rtc_io.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "long_transcript.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "voice_client.h"
#include "wifi_setup.h"

#define CONNECTED BIT0
#define RELEASED BIT1
#define TOP_HELD BIT2
#define SAMPLE_RATE 16000
#define CHUNK_FRAMES (SAMPLE_RATE / 50)
#define MAX_FRAMES (SAMPLE_RATE * 10)
#define MIN_FRAMES (SAMPLE_RATE / 4)
#define SPEECH_VISUAL_DELAY_US 55000
#define DEFAULT_SPEAKER_VOLUME 55
#define TOP_BUTTON_GPIO GPIO_NUM_3
#define SETUP_BUTTON_GPIO GPIO_NUM_0
#define BUTTON_POLL_MS 10
#define BUTTON_DEBOUNCE_SAMPLES 3
#define TOP_TAP_MS 350
#define LOWER_TAP_MS 350
#define SETUP_HOLD_MS 3000
#define BATTERY_POLL_MS 5000
#define WIFI_SETUP_TIMEOUT_US 20000000
#define AXP2101_ADDRESS 0x34
#define SECONDS_PER_DAY 86400
#define CLOCK_LIMIT 4102444800ULL  // 2100-01-01 UTC

_Static_assert(STRANDS_BUILD_EPOCH >= 1700000000 && STRANDS_BUILD_EPOCH < CLOCK_LIMIT,
               "Build epoch must be a current Unix timestamp");

static const char *TAG = "strands_button";
static const char *SETTINGS_NAMESPACE = "strands_demo";

typedef enum {
    TURN_IDLE,
    TURN_RECORDING_TOP,
    TURN_TOP_TAP_CANCELLED,
    TURN_PROCESSING,
    TURN_LONG_RECORDING,
    TURN_LONG_FINISHING,
    TURN_STANDBY,
    TURN_CONFIGURING,
} turn_state_t;

typedef struct {
    bool pressed;
    unsigned different_samples;
} button_state_t;

typedef enum {
    LOWER_IDLE,
    LOWER_FIRST_DOWN,
    LOWER_WAIT_SECOND,
    LOWER_SECOND_DOWN,
} lower_phase_t;

typedef struct {
    lower_phase_t phase;
    TickType_t changed_at;
} lower_gesture_t;

// The ESP32-S3 stores integers little-endian, as required by PCM WAV.
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

_Static_assert(sizeof(wav_header_t) == 44, "WAV header must be 44 bytes");

typedef struct {
    char ssid[33];
    char password[65];
    char reserved[160 + 65]; // Legacy URL and token slots in the NVS blob.
} wifi_settings_t;

_Static_assert(sizeof(wifi_settings_t) == 323, "Wi-Fi settings must retain the existing NVS format");

static wifi_settings_t settings;
static EventGroupHandle_t device_events;
static esp_timer_handle_t reconnect_timer;
static esp_timer_handle_t setup_fallback_timer;
static TaskHandle_t voice_task;
static atomic_int turn_state;
static atomic_uint speaker_volume = DEFAULT_SPEAKER_VOLUME;
static esp_codec_dev_handle_t microphone;
static esp_codec_dev_handle_t speaker;
static uint64_t clock_floor;
static uint64_t saved_clock_floor;

static bool start_long_recording(void);
static void stop_long_recording(void);

static bool long_recording_active(int state)
{
    return state == TURN_LONG_RECORDING || state == TURN_LONG_FINISHING;
}

static bool connected(void)
{
    return (xEventGroupGetBits(device_events) & CONNECTED) != 0;
}

static bool apply_clock_floor(void)
{
    time_t floor = (time_t)clock_floor;
    if (time(NULL) >= floor) {
        return true;
    }
    const struct timeval time_at_floor = {.tv_sec = floor};
    return settimeofday(&time_at_floor, NULL) == 0;
}

static void seed_tls_clock(void)
{
    clock_floor = STRANDS_BUILD_EPOCH;
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint64_t previous = 0;
        if (nvs_get_u64(handle, "clock_floor", &previous) == ESP_OK &&
            previous >= 1700000000 && previous < CLOCK_LIMIT) {
            saved_clock_floor = previous;
            if (previous > clock_floor) {
                clock_floor = previous;
            }
        }
        nvs_close(handle);
    }
    if (!apply_clock_floor()) {
        ESP_LOGW(TAG, "Couldn't seed the secure connection clock");
    }
}

static void remember_https_date(uint64_t date)
{
    if (date <= clock_floor) {
        return;
    }
    clock_floor = date;
    apply_clock_floor();
    if (date / SECONDS_PER_DAY <= saved_clock_floor / SECONDS_PER_DAY) {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u64(handle, "clock_floor", date);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        saved_clock_floor = date;
    } else {
        ESP_LOGW(TAG, "Couldn't persist the secure connection clock: %s", esp_err_to_name(err));
    }
}

static esp_err_t restart_into_setup(void)
{
    int expected = TURN_IDLE;
    if (!atomic_compare_exchange_strong(&turn_state, &expected, TURN_CONFIGURING)) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "setup_once", 1);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "DEMO WIFI SETUP");
        esp_restart();
    }
    ESP_LOGE(TAG, "Couldn't request Wi-Fi setup: %s", esp_err_to_name(err));
    atomic_store(&turn_state, TURN_IDLE);
    return err;
}

static esp_err_t open_wifi_setup(void)
{
    // GPIO0 is the boot strap. A touch action must also leave it released.
    if (gpio_get_level(SETUP_BUTTON_GPIO) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return restart_into_setup();
}

static bool take_setup_request(void)
{
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    uint8_t requested = 0;
    if (nvs_get_u8(handle, "setup_once", &requested) == ESP_OK) {
        if (nvs_erase_key(handle, "setup_once") == ESP_OK) {
            nvs_commit(handle);
        }
    }
    nvs_close(handle);
    return requested == 1;
}

static bool begin_recording(void)
{
    int expected = TURN_IDLE;
    if (!connected() ||
        !atomic_compare_exchange_strong(&turn_state, &expected, TURN_RECORDING_TOP)) {
        return false;
    }
    xEventGroupClearBits(device_events, RELEASED | TOP_HELD);
    xTaskNotifyGive(voice_task);
    return true;
}

static void finish_top_capture(turn_state_t next)
{
    int expected = TURN_RECORDING_TOP;
    if (atomic_compare_exchange_strong(&turn_state, &expected, next)) {
        xEventGroupSetBits(device_events, RELEASED);
    }
}

static esp_err_t read_pmu_register(i2c_master_dev_handle_t pmu, uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(pmu, &reg, 1, value, 1, 50);
}

static void set_power_key_guard(i2c_master_dev_handle_t pmu, bool held, uint8_t *restore)
{
    if (pmu == NULL) {
        return;
    }

    // AXP2101 0x22[1] is the configurable hold-to-off action. 0x10[2]
    // is the separate 16-second PMU cutoff. Never echo command bits 0x10[1:0].
    static const struct {
        uint8_t reg;
        uint8_t enable_bit;
        uint8_t command_bits;
    } controls[] = {
        {0x22, BIT(1), 0},
        {0x10, BIT(2), BIT(1) | BIT(0)},
    };

    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        uint8_t flag = BIT(i);
        if (!held && !(*restore & flag)) {
            continue;
        }

        uint8_t current;
        esp_err_t err = read_pmu_register(pmu, controls[i].reg, &current);
        if (err == ESP_OK) {
            current &= ~controls[i].command_bits;
            uint8_t updated = held ? current & ~controls[i].enable_bit
                                   : current | controls[i].enable_bit;
            if (updated != current) {
                const uint8_t write[] = {controls[i].reg, updated};
                err = i2c_master_transmit(pmu, write, sizeof(write), 50);
                if (err == ESP_OK && held) {
                    *restore |= flag;
                }
            }
            if (err == ESP_OK && !held) {
                *restore &= ~flag;
            }
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Power-key guard register 0x%02x: %s",
                     controls[i].reg, esp_err_to_name(err));
        }
    }
}

static void load_speaker_volume(void)
{
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t value;
        if (nvs_get_u8(handle, "speaker_vol", &value) == ESP_OK &&
            (value == 0 || value == 30 || value == DEFAULT_SPEAKER_VOLUME || value == 80)) {
            atomic_store(&speaker_volume, value);
        }
        nvs_close(handle);
    }
}

static esp_err_t save_speaker_volume(unsigned percent)
{
    if (percent == atomic_load(&speaker_volume)) {
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "speaker_vol", (uint8_t)percent);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        atomic_store(&speaker_volume, percent);
    } else {
        ESP_LOGW(TAG, "Couldn't save speaker volume: %s", esp_err_to_name(err));
    }
    return err;
}

static void sleep_device(void)
{
    int expected = TURN_IDLE;
    if (!atomic_compare_exchange_strong(&turn_state, &expected, TURN_STANDBY)) {
        return;
    }

    // PWR drives GPIO3 high and BOOT drives GPIO0 low. Separate RTC wake
    // sources allow the opposite levels while keeping both buttons usable.
    esp_err_t err = esp_sleep_enable_ext0_wakeup(TOP_BUTTON_GPIO, 1);
    if (err == ESP_OK) {
        err = esp_sleep_enable_ext1_wakeup_io(1ULL << SETUP_BUTTON_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
    }
    if (err == ESP_OK) {
        err = rtc_gpio_pullup_en(SETUP_BUTTON_GPIO);
    }
    if (err == ESP_OK && board_ui_set_sleeping(true)) {
        err = esp_wifi_stop();
        if (err == ESP_OK) {
            board_ui_prepare_standby();
            ESP_LOGI(TAG, "Entering deep sleep. Tap PWR or BOOT to wake.");
            esp_deep_sleep_start();
        }
        board_ui_set_sleeping(false);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Couldn't enter deep sleep: %s", esp_err_to_name(err));
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    atomic_store(&turn_state, TURN_IDLE);
}

static bool update_button(button_state_t *button, bool sample)
{
    if (sample == button->pressed) {
        button->different_samples = 0;
    } else if (++button->different_samples >= BUTTON_DEBOUNCE_SAMPLES) {
        button->pressed = sample;
        button->different_samples = 0;
        return true;
    }
    return false;
}

static void handle_lower_key(lower_gesture_t *gesture, const button_state_t *lower,
                             bool changed, TickType_t now, bool power_guard_active)
{
    if (gesture->phase == LOWER_WAIT_SECOND &&
        now - gesture->changed_at >= pdMS_TO_TICKS(LOWER_TAP_MS)) {
        gesture->phase = LOWER_IDLE;
        board_ui_toggle_settings();
    }
    if (!changed) {
        return;
    }
    if (lower->pressed) {
        gesture->phase = gesture->phase == LOWER_WAIT_SECOND ? LOWER_SECOND_DOWN : LOWER_FIRST_DOWN;
        gesture->changed_at = now;
        return;
    }
    if (gesture->phase != LOWER_FIRST_DOWN && gesture->phase != LOWER_SECOND_DOWN) {
        return;
    }

    bool double_tap = gesture->phase == LOWER_SECOND_DOWN;
    TickType_t held = now - gesture->changed_at;
    gesture->phase = LOWER_IDLE;
    // The lower key is also GPIO0, so enter setup only after its debounced release.
    if (held >= pdMS_TO_TICKS(SETUP_HOLD_MS)) {
        if (!power_guard_active) {
            restart_into_setup();
        }
        return;
    }
    if (held > pdMS_TO_TICKS(LOWER_TAP_MS)) {
        return;
    }
    if (!double_tap) {
        gesture->phase = LOWER_WAIT_SECOND;
        gesture->changed_at = now;
    } else if (!power_guard_active) {
        sleep_device();
    }
}

static void poll_battery(i2c_master_dev_handle_t pmu, unsigned *previous, bool *was_charging)
{
    uint8_t status;
    // AXP2101 STATUS1[3] indicates a battery, STATUS2[6:5] == 01 is charging,
    // and the fuel gauge reports percentage in register A4.
    if (pmu == NULL || read_pmu_register(pmu, 0x00, &status) != ESP_OK) {
        return;
    }
    unsigned percent = 101;
    bool charging = false;
    if (status & BIT(3)) {
        uint8_t gauge;
        if (read_pmu_register(pmu, 0x01, &status) != ESP_OK ||
            read_pmu_register(pmu, 0xa4, &gauge) != ESP_OK) {
            return;
        }
        if (gauge <= 100) {
            percent = gauge;
            charging = (status & (BIT(6) | BIT(5))) == BIT(5);
        }
    }
    if (percent != *previous || charging != *was_charging) {
        *previous = percent;
        *was_charging = charging;
        board_ui_set_battery(percent, charging);
        if (percent <= 100) {
            ESP_LOGI(TAG, "Battery %u%%%s", percent, charging ? " (charging)" : "");
        }
    }
}

static void top_button_worker(void *context)
{
    i2c_master_dev_handle_t pmu = context;
    // A key held at startup doesn't become a new press.
    button_state_t top = {.pressed = gpio_get_level(TOP_BUTTON_GPIO) != 0};
    button_state_t lower = {.pressed = gpio_get_level(SETUP_BUTTON_GPIO) == 0};
    lower_gesture_t gesture = {0};
    bool consume_wake = false;
    bool owns_turn = false;
    bool top_candidate = false;
    bool top_confirmed = false;
    bool top_tap_pending = false;
    bool top_second_tap = false;
    bool top_long_recording = false;
    bool long_recording_pending = false;
    bool long_recording_screen_off = false;
    TickType_t top_pressed_at = 0;
    TickType_t top_released_at = 0;
    uint8_t restore = 0;
    TickType_t last_restore = 0;
    TickType_t last_battery = xTaskGetTickCount() - pdMS_TO_TICKS(BATTERY_POLL_MS);
    unsigned previous_battery = 101;
    bool was_charging = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        bool top_sample = gpio_get_level(TOP_BUTTON_GPIO) != 0;
        bool lower_sample = gpio_get_level(SETUP_BUTTON_GPIO) == 0;
        bool top_changed = update_button(&top, top_sample);
        bool lower_changed = update_button(&lower, lower_sample);
        bool top_down = top_sample || top.pressed;
        bool lower_down = lower_sample || lower.pressed;
        bool keys_released = !top_down && !lower_down;
        TickType_t now = xTaskGetTickCount();

        if (!top.pressed && restore && now - last_restore >= pdMS_TO_TICKS(500)) {
            set_power_key_guard(pmu, false, &restore);
            last_restore = xTaskGetTickCount();
        }
        if (consume_wake) {
            if (keys_released) {
                consume_wake = false;
            }
            continue;
        }

        int state = atomic_load(&turn_state);
        bool in_long_recording = long_recording_active(state);
        if (long_recording_screen_off) {
            bool wake_press = (top_changed && top.pressed) || (lower_changed && lower.pressed);
            if ((wake_press || !in_long_recording) && board_ui_set_sleeping(false)) {
                long_recording_screen_off = false;
                consume_wake = !keys_released;
            }
            continue;
        }

        bool idle = state == TURN_IDLE || state == TURN_TOP_TAP_CANCELLED;
        bool can_tap = idle || state == TURN_LONG_RECORDING;
        if (lower_down) {
            top_candidate = false;
            top_tap_pending = false;
            long_recording_pending = false;
        }
        if (top_tap_pending && (!can_tap || in_long_recording != top_long_recording)) {
            top_tap_pending = false;
        }
        if (long_recording_pending) {
            long_recording_pending = state == TURN_TOP_TAP_CANCELLED;
            if (state == TURN_IDLE) {
                start_long_recording();
            }
            continue;
        }
        if (top_tap_pending && keys_released && state != TURN_TOP_TAP_CANCELLED &&
            (in_long_recording || restore == 0) &&
            now - top_released_at >= pdMS_TO_TICKS(TOP_TAP_MS)) {
            top_tap_pending = false;
            if (in_long_recording) {
                long_recording_screen_off = board_ui_set_sleeping(true);
            } else {
                sleep_device();
            }
            continue;
        }

        if (top_changed && top.pressed) {
            top_candidate = can_tap && !lower_down;
            top_second_tap = top_candidate && top_tap_pending &&
                             now - top_released_at < pdMS_TO_TICKS(TOP_TAP_MS);
            top_long_recording = in_long_recording;
            top_tap_pending = false;
            top_confirmed = false;
            top_pressed_at = now;
        }
        if (top.pressed && top_candidate && !owns_turn && !top_long_recording &&
            state == TURN_IDLE && begin_recording()) {
            owns_turn = true;
            ESP_LOGI(TAG, "DEMO TOP PRESSED");
            set_power_key_guard(pmu, true, &restore);
        }
        bool held = now - top_pressed_at >= pdMS_TO_TICKS(TOP_TAP_MS);
        if (owns_turn && held && !top_confirmed) {
            top_confirmed = true;
            xEventGroupSetBits(device_events, TOP_HELD);
        }
        if (top_changed && !top.pressed) {
            if (owns_turn) {
                owns_turn = false;
                if (held) {
                    ESP_LOGI(TAG, "DEMO TOP RELEASED");
                }
                finish_top_capture(held ? TURN_PROCESSING : TURN_TOP_TAP_CANCELLED);
                set_power_key_guard(pmu, false, &restore);
                last_restore = xTaskGetTickCount();
            }
            if (top_candidate && !held && in_long_recording == top_long_recording &&
                (can_tap || state == TURN_RECORDING_TOP)) {
                if (top_second_tap) {
                    if (top_long_recording) {
                        stop_long_recording();
                    } else {
                        // The voice worker must close its provisional PTT capture first.
                        long_recording_pending = true;
                    }
                } else {
                    top_tap_pending = true;
                    top_released_at = now;
                }
            }
            top_candidate = false;
        }
        if (top_down || atomic_load(&turn_state) != TURN_IDLE) {
            gesture.phase = LOWER_IDLE;
        } else {
            handle_lower_key(&gesture, &lower, lower_changed, now, restore != 0);
        }
        if (!top_sample && !lower_sample && atomic_load(&turn_state) == TURN_IDLE &&
            now - last_battery >= pdMS_TO_TICKS(BATTERY_POLL_MS)) {
            poll_battery(pmu, &previous_battery, &was_charging);
            last_battery = xTaskGetTickCount();
        }
    }
}

static void start_top_button(void)
{
    // The 1.75C routes its top PWR key through an inverter to active-high GPIO3.
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 ||
        esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        rtc_gpio_deinit(TOP_BUTTON_GPIO);
        rtc_gpio_deinit(SETUP_BUTTON_GPIO);
    }
    const gpio_config_t input = {
        .pin_bit_mask = 1ULL << TOP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
    };
    esp_err_t err = gpio_config(&input);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Couldn't configure top button: %s", esp_err_to_name(err));
        return;
    }
    const gpio_config_t setup_input = {
        .pin_bit_mask = 1ULL << SETUP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    err = gpio_config(&setup_input);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Couldn't configure setup button: %s", esp_err_to_name(err));
        return;
    }

    const i2c_device_config_t pmu_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDRESS,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t pmu = NULL;
    err = i2c_master_bus_add_device(bsp_i2c_get_handle(), &pmu_config, &pmu);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Couldn't configure power-key guard: %s", esp_err_to_name(err));
    }
    if (xTaskCreate(top_button_worker, "top_button", 3072, pmu, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Couldn't start top button task");
        if (pmu != NULL) {
            i2c_master_bus_rm_device(pmu);
        }
    }
}

static void init_audio(void)
{
    // Allocate DMA at boot in the shared stereo format so opening either codec doesn't
    // need to grow the buffers after Wi-Fi has taken internal memory.
    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    esp_err_t result = bsp_audio_init(&i2s_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Couldn't reserve audio DMA: %s", esp_err_to_name(result));
        return;
    }
    microphone = bsp_audio_codec_microphone_init();
    if (microphone != NULL) {
        esp_codec_dev_set_in_gain(microphone, 24.0f);
        ESP_LOGI(TAG, "Microphone ready");
    }
    speaker = bsp_audio_codec_speaker_init();
    if (speaker != NULL) {
        ESP_LOGI(TAG, "Speaker ready");
    } else {
        ESP_LOGW(TAG, "Speaker initialization failed");
    }
}

static int open_audio_codec(esp_codec_dev_handle_t codec)
{
    esp_codec_dev_sample_info_t format = {
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel = 2,
    };
    return esp_codec_dev_open(codec, &format);
}

static unsigned audio_level(unsigned magnitude, size_t frames, unsigned noise_floor, unsigned knee)
{
    unsigned mean = magnitude / frames;
    if (mean <= noise_floor) {
        return 0;
    }
    // A soft knee keeps quiet syllables visible without maxing out on every peak.
    unsigned signal = mean - noise_floor;
    return 100 * signal / (signal + knee);
}

static const char *record_voice(uint8_t *wav, size_t *wav_bytes)
{
    const char *too_short = "Hold the top button a little longer while you speak.";
    if (atomic_load(&turn_state) != TURN_RECORDING_TOP) {
        return too_short;
    }

    if (microphone == NULL) {
        return "Couldn't start the microphone.";
    }
    if (open_audio_codec(microphone) != ESP_CODEC_DEV_OK) {
        esp_codec_dev_close(microphone);
        return "Couldn't open the microphone.";
    }

    ESP_LOGI(TAG, "DEMO VOICE RECORDING");
    // The ES7210 exposes both microphones as interleaved 16-bit I2S slots.
    int16_t stereo[CHUNK_FRAMES * 2];
    int16_t *mono = (int16_t *)(wav + sizeof(wav_header_t));
    size_t frames = 0;
    int peak = 0;
    int read_result = ESP_CODEC_DEV_OK;
    bool listening = false;
    while (frames < MAX_FRAMES && atomic_load(&turn_state) == TURN_RECORDING_TOP) {
        read_result = esp_codec_dev_read(microphone, stereo, sizeof(stereo));
        if (read_result != ESP_CODEC_DEV_OK) {
            break;
        }
        unsigned magnitude = 0;
        for (size_t i = 0; i < CHUNK_FRAMES; ++i) {
            int sample = (stereo[2 * i] + stereo[2 * i + 1]) / 2;
            mono[frames++] = (int16_t)sample;
            int amplitude = abs(sample);
            magnitude += (unsigned)amplitude;
            if (amplitude > peak) {
                peak = amplitude;
            }
        }
        if (!listening && (xEventGroupGetBits(device_events) & TOP_HELD)) {
            board_ui_show(BOARD_UI_LISTENING, NULL);
            listening = true;
        }
        board_ui_set_audio_level(audio_level(magnitude, CHUNK_FRAMES, 36, 240));
    }
    esp_codec_dev_close(microphone);
    board_ui_set_audio_level(0);

    ESP_LOGI(TAG, "Captured %u ms of audio, peak %d",
             (unsigned)(frames * 1000 / SAMPLE_RATE), peak);
    if (read_result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Microphone read failed (%d)", read_result);
        return "Microphone read failed. Try again.";
    }
    if (frames < MIN_FRAMES) {
        return too_short;
    }
    if (peak < 16) {
        return "No voice picked up. Try speaking closer.";
    }

    const uint32_t pcm_bytes = frames * sizeof(*mono);
    const wav_header_t header = {
        .riff = "RIFF",
        .riff_size = sizeof(header) - 8 + pcm_bytes,
        .wave = "WAVE",
        .fmt = "fmt ",
        .fmt_size = 16,
        .format = 1,
        .channels = 1,
        .sample_rate = SAMPLE_RATE,
        .byte_rate = SAMPLE_RATE * sizeof(*mono),
        .block_align = sizeof(*mono),
        .bits_per_sample = 16,
        .data = "data",
        .data_size = pcm_bytes,
    };
    memcpy(wav, &header, sizeof(header));
    *wav_bytes = sizeof(header) + pcm_bytes;
    return NULL;
}

static void show_speech_level(const int16_t *pcm, size_t frames, int64_t visual_start)
{
    int64_t elapsed = esp_timer_get_time() - visual_start;
    if (visual_start == 0 || elapsed < 0) {
        return;
    }
    size_t offset = (size_t)(elapsed * SAMPLE_RATE / 1000000);
    if (offset >= frames) {
        board_ui_set_audio_level(0);
        return;
    }
    size_t count = frames - offset < CHUNK_FRAMES ? frames - offset : CHUNK_FRAMES;
    unsigned magnitude = 0;
    for (size_t i = 0; i < count; ++i) {
        magnitude += (unsigned)abs((int)pcm[offset + i]);
    }
    board_ui_set_audio_level(audio_level(magnitude, count, 80, 620));
}

static bool play_speech(const int16_t *mono, size_t frames, unsigned volume)
{
    if (open_audio_codec(speaker) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(speaker, volume) != ESP_CODEC_DEV_OK) {
        esp_codec_dev_close(speaker);
        return false;
    }

    board_ui_set_audio_level(0);
    board_ui_set_speaking(true);
    ESP_LOGI(TAG, "DEMO SPEECH START");
    int16_t stereo[CHUNK_FRAMES * 2];
    int result = ESP_CODEC_DEV_OK;
    int64_t visual_start = 0;
    for (size_t written = 0; written < frames;) {
        size_t chunk = frames - written > CHUNK_FRAMES ? CHUNK_FRAMES : frames - written;
        for (size_t i = 0; i < chunk; ++i) {
            stereo[2 * i] = stereo[2 * i + 1] = mono[written + i];
        }
        result = esp_codec_dev_write(speaker, stereo, chunk * 2 * sizeof(*stereo));
        if (result != ESP_CODEC_DEV_OK) {
            break;
        }
        if (written == 0) {
            // Writes feed the 90 ms DMA ring ahead of the speaker. Allow for the
            // first write and the next display refresh when estimating audible time.
            visual_start = esp_timer_get_time() + SPEECH_VISUAL_DELAY_US;
        }
        written += chunk;
        show_speech_level(mono, frames, visual_start);
    }
    // The BSP's six 240-frame DMA buffers can hold up to 90 ms of queued sound.
    for (unsigned elapsed = 0; elapsed < 120; elapsed += 20) {
        show_speech_level(mono, frames, visual_start);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_codec_dev_close(speaker);
    board_ui_set_audio_level(0);
    board_ui_set_speaking(false);
    return result == ESP_CODEC_DEV_OK;
}

static bool start_long_recording(void)
{
    int expected = TURN_IDLE;
    if (microphone == NULL || voice_task == NULL || !connected() ||
        !atomic_compare_exchange_strong(&turn_state, &expected, TURN_LONG_RECORDING)) {
        return false;
    }
    long_transcript_prepare();
    xTaskNotifyGive(voice_task);
    return true;
}

static void stop_long_recording(void)
{
    int expected = TURN_LONG_RECORDING;
    if (atomic_compare_exchange_strong(&turn_state, &expected, TURN_LONG_FINISHING)) {
        long_transcript_request_stop();
    }
}

static void finish_long_recording(void)
{
    atomic_store(&turn_state, TURN_LONG_FINISHING);
    board_ui_long_recording_finishing();
}

static void run_long_recording(void)
{
    board_ui_long_recording_started();
    if (!apply_clock_floor()) {
        board_ui_long_recording_finished("Couldn't set the clock for transcription.");
        return;
    }
    long_transcript_result_t result = long_transcript_run(microphone, finish_long_recording);
    ESP_LOGI(TAG, "Long recording finished: %u seconds, %u segments, %u gaps, %s",
             result.duration_seconds, result.segments, result.gaps, esp_err_to_name(result.error));
    if (result.error != ESP_OK && result.segments == 0) {
        board_ui_long_recording_finished("Couldn't save the transcript.");
    } else if (result.segments == 0 && result.gaps != 0) {
        board_ui_long_recording_finished("Couldn't transcribe the recording. Check Wi-Fi and try again.");
    } else if (result.segments == 0) {
        board_ui_long_recording_finished("No speech was transcribed.");
    } else {
        board_ui_long_recording_finished(NULL);
    }
}

static void voice_worker(void *context)
{
    (void)context;
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 0) {
            continue;
        }
        if (long_recording_active(atomic_load(&turn_state))) {
            run_long_recording();
            atomic_store(&turn_state, TURN_IDLE);
            continue;
        }
        board_ui_set_audio_level(0);
        size_t wav_bytes = 0;
        uint8_t *wav = heap_caps_malloc(sizeof(wav_header_t) + MAX_FRAMES * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const char *problem = wav == NULL ? "Couldn't allocate the recording buffer."
                                          : record_voice(wav, &wav_bytes);
        if (problem != NULL) {
            // Classify the press even if the codec failed before the key was released.
            xEventGroupWaitBits(device_events, TOP_HELD | RELEASED, pdFALSE, pdFALSE, portMAX_DELAY);
        }
        if (atomic_load(&turn_state) == TURN_TOP_TAP_CANCELLED) {
            free(wav);
            atomic_store(&turn_state, TURN_IDLE);
            continue;
        }
        if (problem == NULL && atomic_load(&turn_state) == TURN_RECORDING_TOP) {
            board_ui_show(BOARD_UI_RELEASE, NULL);
            xEventGroupWaitBits(device_events, RELEASED, pdFALSE, pdTRUE, portMAX_DELAY);
        }
        finish_top_capture(TURN_PROCESSING);

        unsigned volume = atomic_load(&speaker_volume);
        bool want_speech = volume != 0 && speaker != NULL;
        voice_response_t response = {0};
        if (problem == NULL && connected()) {
            board_ui_show(BOARD_UI_THINKING, NULL);
            if (apply_clock_floor()) {
                voice_client_request(wav, wav_bytes, want_speech, &response);
                remember_https_date(response.https_date);
            } else {
                problem = "Couldn't set the clock for a secure connection.";
            }
        }
        free(wav);

        bool ok = response.reply != NULL;
        if (ok) {
            board_ui_show(BOARD_UI_REPLY, response.reply);
            if (response.stream != NULL) {
                size_t frames = 0;
                int16_t *pcm = voice_client_read_speech(&response, &frames);
                bool played = pcm != NULL && play_speech(pcm, frames, volume);
                free(pcm);
                ESP_LOGI(TAG, "DEMO SPEECH %s", played ? "OK" : "ERROR");
            }
        } else {
            if (!connected()) {
                problem = "Wi-Fi disconnected. Try again once it's back.";
            } else if (problem == NULL) {
                problem = response.error != NULL ? response.error
                                                 : "Couldn't reach the voice service. Try again.";
            }
            board_ui_show(BOARD_UI_ERROR, problem);
        }
        voice_client_close(&response);
        atomic_store(&turn_state, TURN_IDLE);
        ESP_LOGI(TAG, "DEMO VOICE %s", ok ? "OK" : "ERROR");
    }
}

static void retry_wifi(void *context)
{
    (void)context;
    if (atomic_load(&turn_state) == TURN_STANDBY) {
        return;
    }
    esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi retry failed: %s", esp_err_to_name(result));
    }
}

static void wifi_setup_timeout(void *context)
{
    (void)context;
    if (connected()) {
        return;
    }
    if (gpio_get_level(TOP_BUTTON_GPIO) == 0 && gpio_get_level(SETUP_BUTTON_GPIO) != 0) {
        restart_into_setup();
    }
    esp_timer_start_once(setup_fallback_timer, 2000000);
}

static void network_event(void *context, esp_event_base_t base, int32_t id, void *data)
{
    (void)context;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        retry_wifi(NULL);
    } else if (atomic_load(&turn_state) == TURN_STANDBY) {
        return;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGI(TAG, "Wi-Fi disconnected (reason %u)", (unsigned)event->reason);
        xEventGroupClearBits(device_events, CONNECTED);
        if (atomic_load(&turn_state) == TURN_IDLE) {
            board_ui_show(BOARD_UI_CONNECTING, NULL);
        }
        esp_timer_start_once(reconnect_timer, 2000000);
        esp_timer_start_once(setup_fallback_timer, WIFI_SETUP_TIMEOUT_US);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(device_events, CONNECTED);
        esp_timer_stop(setup_fallback_timer);
        esp_err_t err = esp_netif_sntp_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Clock synchronization start failed: %s", esp_err_to_name(err));
        }
        if (atomic_load(&turn_state) == TURN_IDLE) {
            board_ui_show(BOARD_UI_READY, NULL);
        }
        ESP_LOGI(TAG, "DEMO WIFI READY");
    }
}

static void start_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() != NULL ? ESP_OK : ESP_FAIL);
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.cloudflare.com");
    config.start = false;
    config.wait_for_sync = false;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL));
    const esp_timer_create_args_t timer_args = {.callback = retry_wifi, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &reconnect_timer));
    const esp_timer_create_args_t setup_timer_args = {
        .callback = wifi_setup_timeout, .name = "wifi_setup_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&setup_timer_args, &setup_fallback_timer));

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, settings.ssid, strlen(settings.ssid));
    memcpy(wifi_config.sta.password, settings.password, strlen(settings.password));
    wifi_config.sta.threshold.authmode = settings.password[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err == ESP_ERR_WIFI_PASSWORD || err == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "The Wi-Fi driver rejected the saved credentials");
        restart_into_setup();
        board_ui_show(BOARD_UI_ERROR, "Couldn't restart into Wi-Fi setup.");
        return;
    }
    ESP_ERROR_CHECK(err);
    board_ui_show(BOARD_UI_CONNECTING, NULL);
    ESP_ERROR_CHECK(esp_timer_start_once(setup_fallback_timer, WIFI_SETUP_TIMEOUT_US));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static esp_err_t commit_settings(const wifi_settings_t *values)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, "settings", values, sizeof(*values));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static bool valid_wifi(const wifi_settings_t *values)
{
    size_t ssid_length = strnlen(values->ssid, sizeof(values->ssid));
    size_t password_length = strnlen(values->password, sizeof(values->password));
    return ssid_length > 0 && ssid_length < sizeof(values->ssid) &&
           (password_length == 0 ||
            (password_length >= 8 && password_length < 64) ||
            (password_length == 64 &&
             strspn(values->password, "0123456789abcdefABCDEF") == 64));
}

static bool load_settings(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    size_t size = sizeof(settings);
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, "settings", &settings, &size);
        nvs_close(handle);
    }
    return err == ESP_OK && size == sizeof(settings) && valid_wifi(&settings);
}

static esp_err_t save_network_settings(const char *ssid, const char *password)
{
    wifi_settings_t next = {0};
    size_t ssid_length = strnlen(ssid, sizeof(next.ssid));
    size_t password_length = strnlen(password, sizeof(next.password));
    if (ssid_length >= sizeof(next.ssid) || password_length >= sizeof(next.password)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(next.ssid, ssid, ssid_length);
    memcpy(next.password, password, password_length);
    return valid_wifi(&next) ? commit_settings(&next) : ESP_ERR_INVALID_ARG;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    load_speaker_volume();

    device_events = xEventGroupCreate();
    ESP_ERROR_CHECK(device_events != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(board_ui_start_display());
    board_ui_init(atomic_load(&speaker_volume), save_speaker_volume, open_wifi_setup);
    bool configured = load_settings();
    if (take_setup_request() || !configured) {
        esp_err_t err = wifi_setup_start(save_network_settings);
        if (err != ESP_OK) {
            board_ui_show(BOARD_UI_ERROR, "Couldn't start the Wi-Fi setup network.");
            ESP_LOGE(TAG, "Wi-Fi setup failed: %s", esp_err_to_name(err));
        }
        return;
    }

    init_audio();
    ESP_ERROR_CHECK(xTaskCreate(voice_worker, "voice_worker", 8192, NULL, 4, &voice_task) ==
                            pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    seed_tls_clock();
    start_wifi();
    start_top_button();
}
