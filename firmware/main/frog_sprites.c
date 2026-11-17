#include "frog_sprites.h"

extern const uint8_t frog_frame_pixels[];
extern const uint8_t frog_theme_overlay_pixels[];

#define FRAME_BYTES (FROG_SPRITE_WIDTH * FROG_SPRITE_HEIGHT * sizeof(uint16_t))

#define SPRITE(pose) [pose] = { \
    .header.magic = LV_IMAGE_HEADER_MAGIC, \
    .header.cf = LV_COLOR_FORMAT_RGB565, \
    .header.w = FROG_SPRITE_WIDTH, \
    .header.h = FROG_SPRITE_HEIGHT, \
    .header.stride = FROG_SPRITE_WIDTH * sizeof(uint16_t), \
    .data_size = FRAME_BYTES, \
    .data = frog_frame_pixels + (pose) * FRAME_BYTES, \
}

const lv_image_dsc_t frog_sprites[FROG_SPRITE_COUNT] = {
    SPRITE(FROG_IDLE),
    SPRITE(FROG_BLINK),
    SPRITE(FROG_CROUCH),
    SPRITE(FROG_HOP),
    SPRITE(FROG_LISTEN),
    SPRITE(FROG_THINK_LEFT),
    SPRITE(FROG_THINK_RIGHT),
    SPRITE(FROG_HAPPY),
    SPRITE(FROG_WINK),
    SPRITE(FROG_WORRIED),
    SPRITE(FROG_TALK),
    SPRITE(FROG_WAVE_LIFT),
    SPRITE(FROG_WAVE_IN),
    SPRITE(FROG_WAVE_MID),
    SPRITE(FROG_WAVE_OUT),
};

#define OVERLAY_BYTES (FROG_THEME_OVERLAY_WIDTH * FROG_THEME_OVERLAY_HEIGHT * sizeof(uint32_t))

#define OVERLAY(index) [index] = { \
    .header.magic = LV_IMAGE_HEADER_MAGIC, \
    .header.cf = LV_COLOR_FORMAT_ARGB8888, \
    .header.w = FROG_THEME_OVERLAY_WIDTH, \
    .header.h = FROG_THEME_OVERLAY_HEIGHT, \
    .header.stride = FROG_THEME_OVERLAY_WIDTH * sizeof(uint32_t), \
    .data_size = OVERLAY_BYTES, \
    .data = frog_theme_overlay_pixels + (index) * OVERLAY_BYTES, \
}

const lv_image_dsc_t frog_theme_overlays[2] = {
    OVERLAY(0),
    OVERLAY(1),
};
