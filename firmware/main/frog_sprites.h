#pragma once

#include "lvgl.h"

#define FROG_SPRITE_WIDTH 216
#define FROG_SPRITE_HEIGHT 176
#define FROG_THEME_OVERLAY_WIDTH 168
#define FROG_THEME_OVERLAY_HEIGHT 64
#define FROG_ORBIT_STAR_WIDTH 28
#define FROG_ORBIT_STAR_HEIGHT 28

typedef enum {
    FROG_IDLE,
    FROG_BLINK,
    FROG_CROUCH,
    FROG_HOP,
    FROG_LISTEN,
    FROG_THINK_LEFT,
    FROG_THINK_RIGHT,
    FROG_HAPPY,
    FROG_WINK,
    FROG_WORRIED,
    FROG_TALK,
    FROG_WAVE_LIFT,
    FROG_WAVE_IN,
    FROG_WAVE_MID,
    FROG_WAVE_OUT,
    FROG_SPRITE_COUNT,
} frog_sprite_t;

extern const lv_image_dsc_t frog_sprites[FROG_SPRITE_COUNT];

// Wizard at index 0, Garden at index 1.
extern const lv_image_dsc_t frog_theme_overlays[2];
extern const lv_image_dsc_t frog_astronaut_helmet;
extern const lv_image_dsc_t frog_orbit_star;
