#include "board_ui.h"

#include <stdatomic.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_memory_utils.h"
#include "frog_sprites.h"
#include "long_transcript.h"
#include "lvgl.h"

#define LCD_BUFFER_LINES 8
#define AUDIO_FRAME_MS 35
#define AUDIO_OPEN_LEVEL 12
#define AUDIO_CLOSE_LEVEL 7
#define MAX_LISTEN_LIFT 12
#define FROG_HOME_Y 85
#define FROG_COMPACT_Y 38
#define FROG_RECORDING_Y 145
#define FROG_ACCESSORY_Y 32
#define HOME_SWIPE_PX 54
#define HOME_TAP_SLOP_PX 18
#define HOME_TAP_MS 400
#define BATTERY_UNAVAILABLE 101

static const char *TAG = "board_ui";

typedef struct {
    frog_sprite_t sprite;
    int8_t x;
    int8_t y;
    uint16_t ms;
} frog_frame_t;

typedef struct {
    const frog_frame_t *frames;
    size_t count;
    size_t loop_from;
} frog_motion_t;

typedef enum {
    FROG_AUDIO_NONE,
    FROG_AUDIO_LISTENING,
    FROG_AUDIO_SPEAKING,
} frog_audio_mode_t;

#define MOTION(sequence, repeat_from) { sequence, sizeof(sequence) / sizeof((sequence)[0]), repeat_from }

static const frog_frame_t idle_frames[] = {
    {FROG_IDLE, 0, 0, 1280}, {FROG_BLINK, 0, 0, 110}, {FROG_IDLE, 0, 0, 930},
    {FROG_CROUCH, 0, 2, 110}, {FROG_HOP, 0, -2, 60}, {FROG_HOP, 0, -10, 110},
    {FROG_HOP, 0, -3, 65}, {FROG_CROUCH, 0, 2, 100}, {FROG_IDLE, 0, 0, 1400},
};

static const frog_frame_t ready_frames[] = {
    {FROG_IDLE, 0, 0, 480},
    {FROG_WAVE_LIFT, 0, 0, 82}, {FROG_WAVE_IN, 0, 0, 80},
    {FROG_WAVE_MID, 0, 0, 64}, {FROG_WAVE_OUT, 0, 0, 104},
    {FROG_WAVE_MID, 0, 0, 64}, {FROG_WAVE_IN, 0, 0, 100},
    {FROG_WAVE_MID, 0, 0, 64}, {FROG_WAVE_OUT, 0, 0, 104},
    {FROG_WAVE_MID, 0, 0, 64}, {FROG_WAVE_IN, 0, 0, 80},
    {FROG_WAVE_LIFT, 0, 0, 82},
    {FROG_IDLE, 0, 0, 1220}, {FROG_BLINK, 0, 0, 110}, {FROG_IDLE, 0, 0, 3300},
};

static const frog_frame_t tap_frames[] = {
    {FROG_CROUCH, 0, 3, 75}, {FROG_HOP, 0, -8, 65}, {FROG_HOP, 0, -24, 125},
    {FROG_HOP, 0, -10, 65}, {FROG_CROUCH, 0, 3, 80}, {FROG_WINK, 0, 0, 270},
};
static const frog_motion_t tap_motion = MOTION(tap_frames, 0);

static const frog_frame_t waiting_frames[] = {
    {FROG_THINK_LEFT, -2, 0, 550}, {FROG_IDLE, 0, 0, 140},
    {FROG_THINK_RIGHT, 2, 0, 550}, {FROG_BLINK, 0, 0, 100},
};

static const frog_frame_t release_frames[] = {
    {FROG_LISTEN, 0, -4, 480}, {FROG_CROUCH, 0, 1, 150},
};

static const frog_frame_t reply_frames[] = {
    {FROG_HAPPY, 0, 0, 260}, {FROG_CROUCH, 0, 2, 90}, {FROG_HOP, 2, -10, 155},
    {FROG_CROUCH, 0, 2, 90}, {FROG_WINK, 0, -1, 210},
    {FROG_HAPPY, 0, 0, 2350}, {FROG_WINK, 0, 0, 170},
};

static const frog_frame_t worried_frames[] = {
    {FROG_WORRIED, 0, 0, 950}, {FROG_WORRIED, -2, 2, 120},
    {FROG_WORRIED, 2, 2, 120}, {FROG_WORRIED, 0, 0, 1600},
};

typedef struct {
    const char *title;
    const char *hint;
    uint32_t accent;
    frog_motion_t motion;
} state_style_t;

static const state_style_t styles[] = {
    [BOARD_UI_SETUP] = {"Wi-Fi setup", "Connect over USB to set up Wi-Fi.", 0xFFD08A, MOTION(idle_frames, 0)},
    [BOARD_UI_CONNECTING] = {"Joining Wi-Fi", "", 0xC0C9C4, MOTION(waiting_frames, 0)},
    [BOARD_UI_READY] = {.title = "", .hint = "", .motion = MOTION(ready_frames, 0)},
    [BOARD_UI_LISTENING] = {.title = "Listening", .hint = "Release to send", .accent = 0x80E1AF},
    [BOARD_UI_RELEASE] = {"Release to send", "Recording limit reached", 0x80E1AF, MOTION(release_frames, 0)},
    [BOARD_UI_THINKING] = {"Thinking", "", 0x80E1AF, MOTION(waiting_frames, 0)},
    [BOARD_UI_REPLY] = {.title = "", .hint = "", .motion = MOTION(reply_frames, 5)},
    [BOARD_UI_ERROR] = {"Try again", "", 0xFFD08A, MOTION(worried_frames, 1)},
};

static esp_lcd_panel_handle_t display_panel;
static bool display_sleeping;
static lv_obj_t *frog;
static lv_obj_t *frog_accessory;
static lv_timer_t *frog_timer;
static const frog_motion_t *active_motion;
static size_t frame_index;
static int32_t frog_y;
static atomic_uint audio_level;
static frog_audio_mode_t frog_audio_mode;
static unsigned audio_envelope;
static bool audio_active;
static lv_obj_t *status_label;
static lv_obj_t *body;
static lv_obj_t *detail_label;
static unsigned battery_percent = BATTERY_UNAVAILABLE;
static bool battery_charging;
static int active_state = -1;
static bool voice_speaking;

typedef enum {
    FROG_THEME_CLASSIC,
    FROG_THEME_WIZARD,
    FROG_THEME_GARDEN,
    FROG_THEME_COUNT,
} frog_theme_t;

static frog_theme_t frog_theme = FROG_THEME_CLASSIC;
static struct {
    lv_point_t origin;
    uint32_t started_at;
    bool active;
    bool moved;
} home_touch;

typedef enum {
    PANEL_HOME,
    PANEL_SETTINGS,
    PANEL_RECORDING,
    PANEL_FINISHING,
    PANEL_TRANSCRIPT,
    PANEL_ERROR,
} panel_view_t;

static struct {
    unsigned percent;
    lv_obj_t *button;
} volume_options[] = {{.percent = 0}, {.percent = 30}, {.percent = 55}, {.percent = 80}};
static panel_view_t panel_view;
static lv_obj_t *panel;
static lv_obj_t *panel_title;
static lv_obj_t *panel_hint;
static lv_obj_t *panel_home;
static lv_obj_t *settings_root;
static lv_obj_t *settings_pages;
static lv_obj_t *settings_battery;
static lv_obj_t *settings_battery_status;
static lv_obj_t *settings_dots[3];
static lv_obj_t *volume_hint;
static lv_obj_t *wifi_hint;
static lv_obj_t *settings_wifi;
static unsigned selected_volume;
static esp_err_t (*save_volume_handler)(unsigned);
static esp_err_t (*open_wifi_handler)(void);
static lv_obj_t *transcript_scroll;
static lv_obj_t *transcript_text;
static lv_obj_t *transcript_prev;
static lv_obj_t *transcript_next;
static char *transcript_page_text;
static unsigned transcript_page;
static unsigned transcript_pages;

static void round_panel_area(lv_area_t *area, void *user_data)
{
    (void)user_data;
    // The CO5300 QSPI panel expects even start coordinates and even dimensions.
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

esp_err_t board_ui_start_display(void)
{
    const esp_lv_adapter_config_t adapter = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&adapter), TAG, "Initialize LVGL adapter");

    const bsp_display_config_t panel_config = {
        .max_transfer_sz = BSP_LCD_H_RES * LCD_BUFFER_LINES * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(bsp_display_new(&panel_config, &display_panel, &io), TAG, "Initialize LCD panel");

    // A small internal buffer lets SPI DMA send pixels directly after Wi-Fi starts.
    const esp_lv_adapter_display_config_t display_config = {
        .panel = display_panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = LCD_BUFFER_LINES,
            .use_psram = false,
            .require_double_buffer = false,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *display = esp_lv_adapter_register_display(&display_config);
    ESP_RETURN_ON_FALSE(display, ESP_FAIL, TAG, "Register LCD display");
    const lv_draw_buf_t *buffer = lv_display_get_buf_active(display);
    ESP_RETURN_ON_FALSE(buffer && esp_ptr_dma_capable(buffer->data), ESP_ERR_NOT_SUPPORTED,
                        TAG, "LCD draw buffer is not SPI DMA capable");
    ESP_RETURN_ON_ERROR(esp_lv_adapter_set_area_rounder_cb(display, round_panel_area, NULL), TAG,
                        "Set LCD area alignment");

    const bsp_display_cfg_t touch_orientation = {
        .touch_flags = {.mirror_x = 1, .mirror_y = 1},
    };
    esp_lcd_touch_handle_t touch;
    ESP_RETURN_ON_ERROR(bsp_touch_new(&touch_orientation, &touch), TAG, "Initialize LCD touch");
    const esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, touch);
    ESP_RETURN_ON_FALSE(esp_lv_adapter_register_touch(&touch_config), ESP_FAIL, TAG, "Register LCD touch");
    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "Initialize LCD brightness");
    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), TAG, "Start LVGL adapter");
    ESP_LOGI(TAG, "LCD DMA buffer ready (%d lines)", LCD_BUFFER_LINES);
    return ESP_OK;
}

bool board_ui_set_sleeping(bool sleeping)
{
    if (display_panel == NULL) {
        return false;
    }
    if (sleeping == display_sleeping) {
        return true;
    }

    if (sleeping) {
        esp_err_t err = esp_lv_adapter_pause(500);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Couldn't pause display updates: %s", esp_err_to_name(err));
            return false;
        }
        // The panel IO drains any queued pixel transfers before sending the off command.
        err = esp_lcd_panel_disp_on_off(display_panel, false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Couldn't turn off the display: %s", esp_err_to_name(err));
            err = esp_lv_adapter_resume();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Couldn't restore display updates: %s", esp_err_to_name(err));
            }
            return false;
        }
    } else {
        esp_err_t err = esp_lcd_panel_disp_on_off(display_panel, true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Couldn't turn on the display: %s", esp_err_to_name(err));
            return false;
        }
        err = esp_lv_adapter_resume();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Couldn't resume display updates: %s", esp_err_to_name(err));
            err = esp_lcd_panel_disp_on_off(display_panel, false);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Couldn't restore the sleeping display: %s", esp_err_to_name(err));
            }
            return false;
        }
    }

    display_sleeping = sleeping;
    return true;
}

void board_ui_prepare_standby(void)
{
    esp_err_t err = esp_lcd_panel_disp_sleep(display_panel, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Couldn't put the display controller in standby: %s", esp_err_to_name(err));
    }
}

static void update_battery_page(void)
{
    if (battery_percent == BATTERY_UNAVAILABLE) {
        lv_label_set_text_static(settings_battery, "--");
        lv_label_set_text_static(settings_battery_status, "Unavailable");
    } else {
        lv_label_set_text_fmt(settings_battery, "%u%%", battery_percent);
        lv_label_set_text_static(settings_battery_status, battery_charging ? "Charging" : "");
    }
}

void board_ui_set_battery(unsigned percent, bool charging)
{
    if (bsp_display_lock(250) != ESP_OK) {
        return;
    }
    if (percent > 100) {
        percent = BATTERY_UNAVAILABLE;
        charging = false;
    }
    if (percent != battery_percent || charging != battery_charging) {
        battery_percent = percent;
        battery_charging = charging;
        if (panel_view == PANEL_SETTINGS) {
            update_battery_page();
        }
    }
    bsp_display_unlock();
}

void board_ui_set_audio_level(unsigned level)
{
    atomic_store_explicit(&audio_level, level > 100 ? 100 : level, memory_order_relaxed);
}

static void show_frog_sprite(frog_sprite_t pose, int32_t x, int32_t y)
{
    const lv_image_dsc_t *sprite = &frog_sprites[pose];
    if (lv_image_get_src(frog) != sprite) {
        lv_image_set_src(frog, sprite);
    }
    lv_obj_set_pos(frog, x, y);
    int32_t accessory_y = y - FROG_ACCESSORY_Y;
    if (frog_theme == FROG_THEME_WIZARD) {
        // Keep the hat tip on the round panel during the compact reply hop.
        accessory_y = LV_MAX(accessory_y, 6);
    }
    lv_obj_set_pos(frog_accessory, x, accessory_y);
}

static void show_frog_frame(void)
{
    const frog_frame_t *frame = &active_motion->frames[frame_index];
    show_frog_sprite(frame->sprite, frame->x, frog_y + frame->y);
    if (frog_timer != NULL) {
        lv_timer_set_period(frog_timer, frame->ms);
    }
}

static void react_to_audio(void)
{
    unsigned level = atomic_load_explicit(&audio_level, memory_order_relaxed);
    // Rise quickly on a syllable and decay more gently through short pauses.
    if (level > audio_envelope) {
        audio_envelope = (audio_envelope + 2 * level + 2) / 3;
    } else {
        audio_envelope = (audio_envelope + level) / 2;
    }
    audio_active = audio_envelope >= (audio_active ? AUDIO_CLOSE_LEVEL : AUDIO_OPEN_LEVEL);

    frog_sprite_t pose = FROG_IDLE;
    int32_t lift = 0;
    if (audio_active && frog_audio_mode == FROG_AUDIO_LISTENING) {
        pose = FROG_LISTEN;
        lift = audio_envelope / 4;
        if (lift > MAX_LISTEN_LIFT) {
            lift = MAX_LISTEN_LIFT;
        }
    } else if (audio_active && frog_audio_mode == FROG_AUDIO_SPEAKING) {
        pose = audio_envelope >= 46 ? FROG_TALK : FROG_LISTEN;
    }
    show_frog_sprite(pose, 0, frog_y - lift);
}

static void next_frog_frame(lv_timer_t *timer)
{
    (void)timer;
    if (frog_audio_mode != FROG_AUDIO_NONE) {
        react_to_audio();
        return;
    }
    if (++frame_index == active_motion->count) {
        if (active_motion == &tap_motion) {
            active_motion = &styles[BOARD_UI_READY].motion;
        }
        frame_index = active_motion->loop_from;
    }
    show_frog_frame();
}

static void animate_frog(const frog_motion_t *motion, int32_t y)
{
    frog_audio_mode = FROG_AUDIO_NONE;
    active_motion = motion;
    frog_y = y;
    frame_index = 0;
    show_frog_frame();
    if (frog_timer != NULL) {
        lv_timer_reset(frog_timer);
        lv_timer_resume(frog_timer);
    }
}

static void cycle_frog_theme(int direction)
{
    frog_theme = (frog_theme + FROG_THEME_COUNT + direction) % FROG_THEME_COUNT;
    if (frog_theme != FROG_THEME_CLASSIC) {
        lv_image_set_src(frog_accessory, &frog_theme_overlays[frog_theme - FROG_THEME_WIZARD]);
    }
    lv_obj_set_flag(frog_accessory, LV_OBJ_FLAG_HIDDEN, frog_theme == FROG_THEME_CLASSIC);
}

static void on_home_touch(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESS_LOST || panel_view != PANEL_HOME || active_state != BOARD_UI_READY) {
        home_touch.active = false;
        return;
    }
    lv_indev_t *touch = lv_event_get_indev(event);
    if (touch == NULL || lv_indev_get_type(touch) != LV_INDEV_TYPE_POINTER) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(touch, &point);
    if (code == LV_EVENT_PRESSED) {
        home_touch.origin = point;
        home_touch.started_at = lv_tick_get();
        home_touch.active = true;
        home_touch.moved = false;
        return;
    }
    if (!home_touch.active) {
        return;
    }

    int32_t dx = point.x - home_touch.origin.x;
    int32_t dy = point.y - home_touch.origin.y;
    home_touch.moved |= LV_ABS(dx) > HOME_TAP_SLOP_PX || LV_ABS(dy) > HOME_TAP_SLOP_PX;
    if (code == LV_EVENT_RELEASED) {
        home_touch.active = false;
        if (LV_ABS(dx) >= HOME_SWIPE_PX && LV_ABS(dx) > LV_ABS(dy)) {
            cycle_frog_theme(dx < 0 ? 1 : -1);
        } else if (!home_touch.moved && lv_tick_elaps(home_touch.started_at) < HOME_TAP_MS) {
            animate_frog(&tap_motion, frog_y);
        }
    }
}

static void animate_audio(frog_audio_mode_t mode, int32_t y)
{
    frog_audio_mode = mode;
    frog_y = y;
    audio_envelope = 0;
    audio_active = false;
    show_frog_sprite(FROG_IDLE, 0, frog_y);
    if (frog_timer != NULL) {
        lv_timer_set_period(frog_timer, AUDIO_FRAME_MS);
        lv_timer_reset(frog_timer);
        lv_timer_resume(frog_timer);
    }
}

static void animate_voice(void)
{
    if (active_state < 0) {
        return;
    }
    if (active_state == BOARD_UI_READY) {
        int32_t height = lv_display_get_vertical_resolution(lv_obj_get_display(frog));
        animate_frog(&styles[active_state].motion, (height - FROG_SPRITE_HEIGHT) / 2);
    } else if (active_state == BOARD_UI_LISTENING) {
        animate_audio(FROG_AUDIO_LISTENING, FROG_HOME_Y);
    } else if (active_state == BOARD_UI_REPLY && voice_speaking) {
        animate_audio(FROG_AUDIO_SPEAKING, FROG_COMPACT_Y);
    } else {
        bool compact = active_state == BOARD_UI_REPLY || active_state == BOARD_UI_ERROR;
        animate_frog(&styles[active_state].motion, compact ? FROG_COMPACT_Y : FROG_HOME_Y);
    }
}

static void init_panel(lv_obj_t *screen);
static void init_settings(void);
static void show_voice_state(board_ui_state_t state, const char *detail);

static bool long_recording_in_progress(void)
{
    return panel_view == PANEL_RECORDING || panel_view == PANEL_FINISHING;
}

static void update_home_text_visibility(void)
{
    bool hidden = panel_view != PANEL_HOME || active_state == BOARD_UI_READY;
    lv_obj_set_flag(status_label, LV_OBJ_FLAG_HIDDEN, hidden);
    lv_obj_set_flag(body, LV_OBJ_FLAG_HIDDEN, hidden);
}

static void show_panel(panel_view_t view)
{
    if (panel_view == view) {
        return;
    }
    if (panel == NULL) {
        init_panel(lv_screen_active());
    }
    home_touch.active = false;
    panel_view = view;
    bool home = view == PANEL_HOME;
    bool settings = view == PANEL_SETTINGS;
    bool transcript = view == PANEL_TRANSCRIPT;
    bool error = view == PANEL_ERROR;
    bool recording = view == PANEL_RECORDING;
    bool working = long_recording_in_progress();
    if (settings && settings_root == NULL) {
        init_settings();
    }
    lv_obj_set_flag(panel, LV_OBJ_FLAG_HIDDEN, home);
    update_home_text_visibility();
    bool frog_hidden = !home && !working;
    lv_obj_set_flag(frog, LV_OBJ_FLAG_HIDDEN, frog_hidden);
    lv_obj_set_flag(frog_accessory, LV_OBJ_FLAG_HIDDEN, frog_hidden || frog_theme == FROG_THEME_CLASSIC);

    if (!transcript && transcript_page_text != NULL) {
        lv_label_set_text_static(transcript_text, "");
        heap_caps_free(transcript_page_text);
        transcript_page_text = NULL;
    }
    if (home) {
        animate_voice();
        return;
    }

    if (settings_root != NULL) {
        lv_obj_set_flag(settings_root, LV_OBJ_FLAG_HIDDEN, !settings);
    }
    lv_obj_set_flag(panel_title, LV_OBJ_FLAG_HIDDEN, settings);
    lv_obj_set_flag(transcript_scroll, LV_OBJ_FLAG_HIDDEN, !transcript && !error);
    lv_obj_set_flag(panel_hint, LV_OBJ_FLAG_HIDDEN, settings || error);
    lv_obj_set_flag(panel_home, LV_OBJ_FLAG_HIDDEN, !transcript && !error);
    lv_obj_set_flag(transcript_prev, LV_OBJ_FLAG_HIDDEN, !transcript);
    lv_obj_set_flag(transcript_next, LV_OBJ_FLAG_HIDDEN, !transcript);
    lv_obj_set_y(panel_title, transcript ? 55 : 73);
    lv_obj_set_y(panel_hint, transcript ? 91 : 112);
    lv_obj_set_style_text_color(panel_title, lv_color_hex(recording ? 0xFFD08A : 0xE4F2E9), 0);

    if (recording) {
        lv_label_set_text_static(panel_title, "Long recording");
        lv_label_set_text_static(panel_hint, "Double-tap top to stop.");
        animate_audio(FROG_AUDIO_LISTENING, FROG_RECORDING_Y);
    } else if (working) {
        lv_label_set_text_static(panel_title, "Finishing...");
        lv_label_set_text_static(panel_hint, "Transcribing and saving");
        animate_frog(&styles[BOARD_UI_THINKING].motion, FROG_RECORDING_Y);
    } else if (frog_timer != NULL) {
        lv_timer_pause(frog_timer);
    }
}

static void return_home(void)
{
    if (active_state == BOARD_UI_REPLY) {
        show_voice_state(BOARD_UI_READY, NULL);
    }
    show_panel(PANEL_HOME);
}

static void show_recording_error(const char *error)
{
    show_panel(PANEL_ERROR);
    lv_label_set_text_static(panel_title, "Long recording");
    lv_label_set_text(transcript_text, error);
    lv_obj_update_layout(transcript_scroll);
    lv_obj_scroll_to_y(transcript_scroll, 0, LV_ANIM_OFF);
}

static void show_transcript_page(unsigned page)
{
    if (page >= transcript_pages) {
        show_recording_error("Couldn't open the transcript.");
        return;
    }
    if (transcript_page_text == NULL) {
        transcript_page_text = heap_caps_malloc(LONG_TRANSCRIPT_PAGE_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (transcript_page_text == NULL) {
        show_recording_error("Not enough memory to open the transcript.");
        return;
    }
    lv_label_set_text_static(transcript_text, "");
    if (long_transcript_read_page(page, transcript_page_text, LONG_TRANSCRIPT_PAGE_CAPACITY) != ESP_OK) {
        show_recording_error("Couldn't read the transcript.");
        return;
    }
    transcript_page = page;
    if (panel_view != PANEL_TRANSCRIPT) {
        show_panel(PANEL_TRANSCRIPT);
        lv_label_set_text_static(panel_title, long_transcript_complete() ? "Transcript" : "Partial transcript");
    }
    lv_label_set_text_fmt(panel_hint, "Page %u of %u", page + 1, transcript_pages);
    lv_obj_set_state(transcript_prev, LV_STATE_DISABLED, page == 0);
    lv_obj_set_state(transcript_next, LV_STATE_DISABLED, page + 1 >= transcript_pages);
    // The label borrows this PSRAM buffer until switching pages or leaving the view.
    lv_label_set_text_static(transcript_text, transcript_page_text);
    lv_obj_update_layout(transcript_scroll);
    lv_obj_scroll_to_y(transcript_scroll, 0, LV_ANIM_OFF);
}

static void refresh_volume(void)
{
    for (size_t i = 0; i < sizeof(volume_options) / sizeof(volume_options[0]); ++i) {
        lv_obj_set_state(volume_options[i].button, LV_STATE_CHECKED, volume_options[i].percent == selected_volume);
    }
    lv_label_set_text_static(volume_hint, "0% shows text only");
}

static void volume_clicked(lv_event_t *event)
{
    const unsigned *volume = lv_event_get_user_data(event);
    if (save_volume_handler(*volume) == ESP_OK) {
        selected_volume = *volume;
        refresh_volume();
    } else {
        lv_label_set_text_static(volume_hint, "Couldn't save volume");
    }
}

static void panel_button_clicked(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_target_obj(event);
    if (button == settings_wifi) {
        if (open_wifi_handler() != ESP_OK) {
            lv_label_set_text_static(wifi_hint, "Try Wi-Fi setup again.");
        }
    } else if (button == panel_home) {
        return_home();
    } else if (button == transcript_prev && transcript_page > 0) {
        show_transcript_page(transcript_page - 1);
    } else if (button == transcript_next && transcript_page + 1 < transcript_pages) {
        show_transcript_page(transcript_page + 1);
    }
}

static lv_obj_t *new_panel_button(lv_obj_t *parent, const char *title, int width, int height, int x, int y)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, width, height);
    lv_obj_align(button, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x37674B), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x14231A), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x254532), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_hex(0xCDEDD8), 0);
    lv_obj_set_style_text_color(button, lv_color_hex(0x66756B), LV_STATE_DISABLED);
    lv_obj_set_style_text_font(button, width < 100 ? &lv_font_montserrat_14 : &lv_font_montserrat_18, 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text_static(label, title);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *new_settings_label(lv_obj_t *parent, const char *text,
                                    const lv_font_t *font, uint32_t color, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_static(label, text);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static void update_settings_dots(void)
{
    lv_obj_t *active = lv_tileview_get_tile_active(settings_pages);
    for (size_t i = 0; i < sizeof(settings_dots) / sizeof(settings_dots[0]); ++i) {
        bool selected = lv_obj_get_child(settings_pages, i) == active;
        lv_obj_set_style_bg_color(settings_dots[i], lv_color_hex(selected ? 0xCDEDD8 : 0x53645A), 0);
    }
}

static void settings_page_changed(lv_event_t *event)
{
    (void)event;
    update_settings_dots();
}

static void init_settings(void)
{
    settings_root = lv_obj_create(panel);
    lv_obj_remove_style_all(settings_root);
    lv_obj_set_size(settings_root, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(settings_root, LV_OBJ_FLAG_SCROLLABLE);

    settings_pages = lv_tileview_create(settings_root);
    lv_obj_remove_style_all(settings_pages);
    lv_obj_set_size(settings_pages, lv_pct(100), lv_pct(100));
    lv_obj_set_scrollbar_mode(settings_pages, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *battery_page = lv_tileview_add_tile(settings_pages, 0, 0, LV_DIR_HOR);
    lv_obj_t *volume_page = lv_tileview_add_tile(settings_pages, 1, 0, LV_DIR_HOR);
    lv_obj_t *wifi_page = lv_tileview_add_tile(settings_pages, 2, 0, LV_DIR_HOR);
    lv_obj_t *pages[] = {battery_page, volume_page, wifi_page};
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        // Keep the tileview's local styles for page size and position.
        lv_obj_remove_theme(pages[i], LV_PART_ANY | LV_STATE_ANY);
        lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    new_settings_label(battery_page, "Battery", &lv_font_montserrat_28, 0xE4F2E9, 148);
    settings_battery = new_settings_label(battery_page, "", &lv_font_montserrat_48, 0xE4F2E9, 0);
    lv_obj_center(settings_battery);
    settings_battery_status = new_settings_label(battery_page, "", &lv_font_montserrat_18, 0xB3BFB8, 281);

    new_settings_label(volume_page, "Volume", &lv_font_montserrat_28, 0xE4F2E9, 148);
    volume_hint = new_settings_label(volume_page, "", &lv_font_montserrat_18, 0xB3BFB8, 325);
    for (size_t i = 0; i < sizeof(volume_options) / sizeof(volume_options[0]); ++i) {
        lv_obj_t *button = new_panel_button(volume_page, "", 106, 52, i % 2 ? 58 : -58, 195 + (int)(i / 2) * 62);
        volume_options[i].button = button;
        lv_label_set_text_fmt(lv_obj_get_child(button, 0), "%u%%", volume_options[i].percent);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x305C43), LV_STATE_CHECKED);
        lv_obj_set_style_border_color(button, lv_color_hex(0x80E1AF), LV_STATE_CHECKED);
        lv_obj_add_event_cb(button, volume_clicked, LV_EVENT_CLICKED, &volume_options[i].percent);
    }
    new_settings_label(wifi_page, "Wi-Fi setup", &lv_font_montserrat_28, 0xE4F2E9, 148);
    settings_wifi = new_panel_button(wifi_page, "Open setup", 192, 52, 0, 220);
    lv_obj_add_event_cb(settings_wifi, panel_button_clicked, LV_EVENT_CLICKED, NULL);
    wifi_hint = new_settings_label(wifi_page, "", &lv_font_montserrat_18, 0xB3BFB8, 288);

    new_settings_label(settings_root, "Settings", &lv_font_montserrat_22, 0xE4F2E9, 73);
    for (size_t i = 0; i < sizeof(settings_dots) / sizeof(settings_dots[0]); ++i) {
        lv_obj_t *dot = lv_obj_create(settings_root);
        settings_dots[i] = dot;
        lv_obj_remove_style_all(dot);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_TOP_MID, ((int)i - 1) * 16, 366);
    }
    lv_obj_add_event_cb(settings_pages, settings_page_changed, LV_EVENT_VALUE_CHANGED, NULL);
}

void board_ui_toggle_settings(void)
{
    if (bsp_display_lock(UINT32_MAX) != ESP_OK) {
        return;
    }
    if (panel_view == PANEL_HOME) {
        show_panel(PANEL_SETTINGS);
        update_battery_page();
        refresh_volume();
        lv_label_set_text_static(wifi_hint, "");
        lv_tileview_set_tile_by_index(settings_pages, 0, 0, LV_ANIM_OFF);
        update_settings_dots();
    } else if (!long_recording_in_progress()) {
        return_home();
    }
    bsp_display_unlock();
}

static void init_panel(lv_obj_t *screen)
{
    panel = lv_obj_create(screen);
    // Keep the frog above the recording panel.
    lv_obj_move_to_index(panel, 0);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

    panel_title = lv_label_create(panel);
    lv_obj_set_style_text_font(panel_title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(panel_title, lv_color_hex(0xE4F2E9), 0);
    lv_obj_align(panel_title, LV_ALIGN_TOP_MID, 0, 73);
    panel_hint = lv_label_create(panel);
    lv_obj_set_width(panel_hint, 298);
    lv_obj_set_style_text_font(panel_hint, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(panel_hint, lv_color_hex(0xB3BFB8), 0);
    lv_obj_set_style_text_align(panel_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(panel_hint, LV_ALIGN_TOP_MID, 0, 112);

    transcript_scroll = lv_obj_create(panel);
    lv_obj_remove_style_all(transcript_scroll);
    lv_obj_set_size(transcript_scroll, 300, 218);
    lv_obj_align(transcript_scroll, LV_ALIGN_TOP_MID, 0, 126);
    lv_obj_set_style_pad_hor(transcript_scroll, 8, 0);
    lv_obj_set_scroll_dir(transcript_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(transcript_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(transcript_scroll, lv_color_hex(0x53645A), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(transcript_scroll, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(transcript_scroll, 3, LV_PART_SCROLLBAR);
    transcript_text = lv_label_create(transcript_scroll);
    lv_obj_set_width(transcript_text, lv_pct(100));
    lv_label_set_long_mode(transcript_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(transcript_text, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(transcript_text, lv_color_hex(0xF0F6F2), 0);
    lv_obj_set_style_text_line_space(transcript_text, 5, 0);

    panel_home = new_panel_button(panel, "Home", 86, 48, 0, 356);
    transcript_prev = new_panel_button(panel, "Prev", 86, 48, -105, 356);
    transcript_next = new_panel_button(panel, "Next", 86, 48, 105, 356);
    lv_obj_t *actions[] = {panel_home, transcript_prev, transcript_next};
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        lv_obj_add_event_cb(actions[i], panel_button_clicked, LV_EVENT_CLICKED, NULL);
    }
}

void board_ui_long_recording_started(void)
{
    if (bsp_display_lock(UINT32_MAX) != ESP_OK) {
        return;
    }
    show_panel(PANEL_RECORDING);
    bsp_display_unlock();
}

void board_ui_long_recording_finishing(void)
{
    if (bsp_display_lock(UINT32_MAX) != ESP_OK) {
        return;
    }
    if (panel_view == PANEL_RECORDING) {
        show_panel(PANEL_FINISHING);
    }
    bsp_display_unlock();
}

void board_ui_long_recording_finished(const char *error)
{
    if (bsp_display_lock(UINT32_MAX) != ESP_OK) {
        return;
    }
    if (long_recording_in_progress()) {
        transcript_pages = long_transcript_page_count();
        if (error != NULL) {
            show_recording_error(error);
        } else {
            show_transcript_page(0);
        }
    }
    bsp_display_unlock();
}

void board_ui_set_speaking(bool speaking)
{
    if (bsp_display_lock(UINT32_MAX) != ESP_OK) {
        return;
    }
    if (active_state == BOARD_UI_REPLY) {
        voice_speaking = speaking;
        if (panel_view == PANEL_HOME) {
            animate_voice();
        }
    }
    bsp_display_unlock();
}

void board_ui_init(unsigned volume, esp_err_t (*save_volume)(unsigned), esp_err_t (*open_wifi)(void))
{
    bsp_display_lock(UINT32_MAX);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, on_home_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(screen, on_home_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(screen, on_home_touch, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(screen, on_home_touch, LV_EVENT_PRESS_LOST, NULL);
    selected_volume = volume;
    save_volume_handler = save_volume;
    open_wifi_handler = open_wifi;

    frog = lv_image_create(screen);
    lv_obj_set_align(frog, LV_ALIGN_TOP_MID);
    frog_accessory = lv_image_create(screen);
    lv_obj_set_align(frog_accessory, LV_ALIGN_TOP_MID);
    lv_obj_remove_flag(frog_accessory, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(frog_accessory, LV_OBJ_FLAG_HIDDEN);
    frog_timer = lv_timer_create(next_frog_frame, 1000, NULL);
    if (frog_timer != NULL) {
        lv_timer_pause(frog_timer);
    }

    status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_22, 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 266);

    body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 312, 90);
    lv_obj_set_style_pad_hor(body, 8, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x53645A), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(body, 3, LV_PART_SCROLLBAR);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 303);

    detail_label = lv_label_create(body);
    lv_obj_set_width(detail_label, lv_pct(100));
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(detail_label, 5, 0);

    bsp_display_unlock();
    board_ui_show(BOARD_UI_CONNECTING, NULL);
}

static void show_voice_state(board_ui_state_t state, const char *detail)
{
    const state_style_t *style = &styles[state];
    bool show_reply = state == BOARD_UI_REPLY;
    bool compact = show_reply || state == BOARD_UI_ERROR;
    if ((int)state != active_state) {
        home_touch.active = false;
        active_state = state;
        update_home_text_visibility();
        if (!show_reply) {
            voice_speaking = false;
        }
        // Keep all four text-area corners within the round panel's visible area.
        lv_obj_set_y(status_label, compact ? 214 : 266);
        int body_y = show_reply ? 214 : compact ? 247 : 303;
        lv_obj_set_y(body, body_y);
        lv_obj_set_height(body, 393 - body_y);
        lv_obj_set_scrollbar_mode(body, compact ? LV_SCROLLBAR_MODE_AUTO : LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_text_font(detail_label, compact ? &lv_font_montserrat_20 : &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(detail_label, lv_color_hex(compact ? 0xF0F6F2 : 0xB3BFB8), 0);
        lv_obj_set_style_text_align(detail_label, show_reply ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text_static(status_label, style->title);
        lv_obj_set_style_text_color(status_label, lv_color_hex(style->accent), 0);
        if (panel_view == PANEL_HOME) {
            animate_voice();
        }
    }

    bool voice_turn = state == BOARD_UI_LISTENING || state == BOARD_UI_RELEASE ||
                      state == BOARD_UI_THINKING || state == BOARD_UI_REPLY || state == BOARD_UI_ERROR;
    if (voice_turn && !long_recording_in_progress()) {
        show_panel(PANEL_HOME);
    }

    lv_label_set_text(detail_label, detail == NULL ? style->hint : detail);
    lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
}

void board_ui_show(board_ui_state_t state, const char *detail)
{
    if (bsp_display_lock(UINT32_MAX) != ESP_OK) {
        return;
    }
    show_voice_state(state, detail);
    bsp_display_unlock();
}
