#pragma once

/*
 * Images for Lua apps, decoded to what LVGL draws directly: RGB565 when
 * opaque, RGB565A8 (the RGB565 plane, then one alpha byte per pixel) when
 * any pixel is transparent.
 *
 * - PNG: every colour type and bit depth, not interlaced. Inflate is the
 *   ESP32-S3's ROM tinfl (zlib on the host), streamed a row at a time.
 * - JPEG: baseline, through the ROM's TJpgDec (device only), scaled down by
 *   2, 4 or 8 to fit max_dim.
 * - Sprites: rows of characters and a palette (pixel art); unknown
 *   characters are transparent.
 *
 * No LVGL here: the host tests build it as is.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t w, h;
    bool alpha;      /* RGB565A8: w*h*2 bytes of colour, then w*h of alpha */
    uint8_t *px;     /* from the caller's alloc */
    size_t bytes;
} luapp_pixels_t;

/* Allocates the decoded pixels; NULL when over the caller's budget. */
typedef void *(*luapp_px_alloc_fn)(size_t n, void *ud);

/* PNG or JPEG bytes, or base64 of either (optionally a data: URL).
 * false with *err (static text) when it can't. */
bool luapp_img_decode(const uint8_t *data, size_t len, int max_dim, luapp_px_alloc_fn alloc, void *ud,
                      luapp_pixels_t *out, const char **err);

/* rows[i] of lens[i] characters; has[c] marks the characters in the palette,
 * pal[c] their 0xRRGGBB. Each pixel becomes scale x scale. */
bool luapp_img_sprite(const char *const *rows, const size_t *lens, int nrows, const uint32_t *pal,
                      const bool *has, int scale, int max_dim, luapp_px_alloc_fn alloc, void *ud,
                      luapp_pixels_t *out, const char **err);

/* Standard or URL-safe base64; whitespace ignored. Returns the bytes written
 * to out (room for n * 3 / 4 + 3), or (size_t)-1 on a bad character. */
size_t luapp_base64_decode(const char *in, size_t n, uint8_t *out);

/* Launcher icons: 96 x 96 RGB565A8, clipped to a circle. Saved as a file of
 * LUAPP_ICON_FILE bytes: "LIC", a flags byte (LUAPP_ICON_FILLS), the pixels. */
#define LUAPP_ICON 96
#define LUAPP_ICON_PX (LUAPP_ICON * LUAPP_ICON * 3)
#define LUAPP_ICON_FILE (4 + LUAPP_ICON_PX)
#define LUAPP_ICON_FILLS 1   /* opaque: covers the app's colour */

/* An icon file from an image (src->px as luapp_img_decode leaves it). Opaque
 * images fill the circle, cropped to it; ones with transparent pixels fit
 * inside it at 72 px, over the app's colour, enlarged by whole multiples so
 * pixel art stays crisp. Enlarging is nearest-pixel, shrinking averages. */
void luapp_icon_make(const luapp_pixels_t *src, uint8_t out[LUAPP_ICON_FILE]);

static inline uint16_t luapp_rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}
