#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef enum {
    BOARD_UI_SETUP,
    BOARD_UI_CONNECTING,
    BOARD_UI_READY,
    BOARD_UI_LISTENING,
    BOARD_UI_RELEASE,
    BOARD_UI_THINKING,
    BOARD_UI_REPLY,
    BOARD_UI_ERROR,
} board_ui_state_t;

esp_err_t board_ui_start_display(void);
void board_ui_init(unsigned volume, esp_err_t (*save_volume)(unsigned), esp_err_t (*open_wifi)(void));
void board_ui_show(board_ui_state_t state, const char *detail);
void board_ui_set_speaking(bool speaking);
// Update the 0–100 audio level without waiting for the display task.
void board_ui_set_audio_level(unsigned level);
// Values above 100 hide the battery indicator.
void board_ui_set_battery(unsigned percent, bool charging);
// Call from the button task without holding the LVGL lock.
bool board_ui_set_sleeping(bool sleeping);
// Enter controller standby after blanking the screen, before ESP32 deep sleep.
void board_ui_prepare_standby(void);
void board_ui_toggle_settings(void);
void board_ui_long_recording_started(void);
void board_ui_long_recording_finishing(void);
// Pass NULL after committing the latest transcript.
void board_ui_long_recording_finished(const char *error);
