#pragma once

#include <stdint.h>

/* The dial: an LVGL I8 image, 256 palette entries (B, G, R, A), then w*h
 * one-byte indices. LVGL draws it from flash a row at a time. */
#define WATCH_DIAL_PALETTE_BYTES (256 * 4)
typedef struct {
    uint16_t w, h;
    uint32_t size;   /* WATCH_DIAL_PALETTE_BYTES + w * h */
    const uint8_t *data;
} watch_indexed_t;

/* A hand or icon: straight-alpha ARGB8888 (B, G, R, A), pivot in image pixels. */
typedef struct {
    uint16_t w, h;
    uint16_t pivot_x, pivot_y;
    const uint8_t *px;
} watch_image_t;

extern const watch_indexed_t watch_dial;
extern const watch_image_t watch_minute, watch_hour, watch_work, watch_second;
extern const watch_image_t watch_cap, watch_sun, watch_moon;
