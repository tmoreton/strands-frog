#include "frog_sprites.h"

extern const uint8_t frog_frame_pixels[];
extern const uint8_t frog_theme_overlay_pixels[];
extern const uint8_t frog_astronaut_pixels[];

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

#define ARGB_BYTES(width, height) ((width) * (height) * sizeof(uint32_t))
#define THEME_OVERLAY_BYTES ARGB_BYTES(FROG_THEME_OVERLAY_WIDTH, FROG_THEME_OVERLAY_HEIGHT)
#define HELMET_BYTES ARGB_BYTES(FROG_SPRITE_WIDTH, FROG_SPRITE_HEIGHT)

#define ARGB_IMAGE(pixels, width, height) { \
    .header.magic = LV_IMAGE_HEADER_MAGIC, \
    .header.cf = LV_COLOR_FORMAT_ARGB8888, \
    .header.w = (width), \
    .header.h = (height), \
    .header.stride = (width) * sizeof(uint32_t), \
    .data_size = ARGB_BYTES(width, height), \
    .data = (pixels), \
}

const lv_image_dsc_t frog_theme_overlays[2] = {
    ARGB_IMAGE(frog_theme_overlay_pixels, FROG_THEME_OVERLAY_WIDTH, FROG_THEME_OVERLAY_HEIGHT),
    ARGB_IMAGE(frog_theme_overlay_pixels + THEME_OVERLAY_BYTES,
               FROG_THEME_OVERLAY_WIDTH, FROG_THEME_OVERLAY_HEIGHT),
};

const lv_image_dsc_t frog_astronaut_helmet =
    ARGB_IMAGE(frog_astronaut_pixels, FROG_SPRITE_WIDTH, FROG_SPRITE_HEIGHT);

const lv_image_dsc_t frog_orbit_star =
    ARGB_IMAGE(frog_astronaut_pixels + HELMET_BYTES, FROG_ORBIT_STAR_WIDTH, FROG_ORBIT_STAR_HEIGHT);
