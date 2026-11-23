/*
 * Screenshots for Muse (luapp.admin action screenshot): the screen as a PNG,
 * served in base64 pages.
 *
 * A whole-screen snapshot would take a 434 KB buffer, which a running app
 * with a big canvas leaves no room for. Instead the screen is drawn 32 rows
 * at a time into a ~30 KB strip, the way lv_snapshot draws, and each strip
 * goes straight into the ROM's deflate as PNG rows: ~200 KB at the peak
 * (most of it the compressor), plus the PNG. The display lock is held
 * throughout, so the picture is one moment (the screen pauses ~1 s).
 *
 * Lua task (it has the stack for drawing).
 */

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "lvgl_private.h"
#include "mbedtls/base64.h"
#include "miniz.h"
#include "luapp_priv.h"
#include "muse_apps_priv.h"

static const char *TAG = "luapp_shot";

#define STRIP_ROWS 32
#define PAGE_RAW (12 * 1024)              /* base64: 16 KB a reply */
#define PNG_MAX (512 * 1024)
#define KEEP_US (60 * 1000000LL)          /* an unfetched picture is dropped after this */
#define DEFLATE_FLAGS (TDEFL_WRITE_ZLIB_HEADER | TDEFL_GREEDY_PARSING_FLAG | 64)

static struct {
    uint8_t *png;
    size_t len, cap;
    int w, h, scale;
    int64_t at;
} S;

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    bool full;
} out_t;

static bool grow(out_t *o, size_t need)
{
    if (o->len + need <= o->cap) return true;
    size_t cap = o->cap ? o->cap : 32 * 1024;
    while (cap < o->len + need) cap *= 2;
    if (cap > PNG_MAX) {
        o->full = true;
        return false;
    }
    uint8_t *b = heap_caps_realloc(o->buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) {
        o->full = true;
        return false;
    }
    o->buf = b;
    o->cap = cap;
    return true;
}

static void put(out_t *o, const void *p, size_t n)
{
    if (!grow(o, n)) return;
    memcpy(o->buf + o->len, p, n);
    o->len += n;
}

static void put32(out_t *o, uint32_t v)
{
    uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v};
    put(o, b, 4);
}

static mz_bool on_deflate(const void *p, int len, void *user)
{
    out_t *o = user;
    put(o, p, (size_t)len);
    return !o->full;
}

static void chunk(out_t *o, const char *type, const uint8_t *data, size_t n)
{
    put32(o, (uint32_t)n);
    size_t at = o->len;
    put(o, type, 4);
    if (n) put(o, data, n);
    if (o->full) return;
    put32(o, esp_rom_crc32_le(0, o->buf + at, (uint32_t)(n + 4)));
}

/* Draws rows [y0, y0 + rows) of the screen into buf (the way lv_snapshot does). */
static void draw_strip(lv_obj_t *scr, lv_draw_buf_t *buf, int32_t y0, int32_t rows)
{
    lv_draw_buf_clear(buf, NULL);
    lv_area_t area;
    lv_obj_get_coords(scr, &area);
    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = buf;
    layer.buf_area.x1 = area.x1;
    layer.buf_area.x2 = area.x1 + (int32_t)buf->header.w - 1;
    layer.buf_area.y1 = area.y1 + y0;
    layer.buf_area.y2 = area.y1 + y0 + rows - 1;
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer._clip_area = layer.buf_area;
    layer.phy_clip_area = layer.buf_area;
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_CREATED, &layer);
    lv_display_t *disp = lv_obj_get_display(scr), *disp_old = lv_refr_get_disp_refreshing();
    lv_layer_t *head_old = disp->layer_head;
    disp->layer_head = &layer;
    lv_refr_set_disp_refreshing(disp);
    lv_obj_redraw(&layer, scr);
    lv_obj_t *top = lv_display_get_layer_top(disp);
    if (top) lv_obj_redraw(&layer, top);   /* hints and captions over everything */
    layer.all_tasks_added = true;
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }
    disp->layer_head = head_old;
    lv_refr_set_disp_refreshing(disp_old);
    lv_draw_unit_send_event(NULL, LV_EVENT_SCREEN_LOAD_START, &layer);
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_DELETED, &layer);
}

static inline void rgb(uint16_t c, uint8_t *o)
{
    uint8_t r = c >> 11, g = (c >> 5) & 0x3f, b = c & 0x1f;
    o[0] = (uint8_t)(r << 3 | r >> 2);
    o[1] = (uint8_t)(g << 2 | g >> 4);
    o[2] = (uint8_t)(b << 3 | b >> 2);
}

void luapp_shot_drop(void)
{
    free(S.png);
    memset(&S, 0, sizeof(S));
}

void luapp_shot_expire(void)
{
    if (S.png && esp_timer_get_time() - S.at > KEEP_US) luapp_shot_drop();
}

/* Takes the picture; NULL, or why not. */
static const char *take(int scale)
{
    luapp_shot_drop();
    const char *why = NULL;
    out_t o = {0};
    tdefl_compressor *d = heap_caps_calloc(1, sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *strip = NULL, *row = NULL;
    lv_draw_buf_t buf;
    if (!d || !luapp_lock()) {
        why = d ? "the screen is busy" : "not enough memory right now";
        free(d);
        return why;
    }
    lv_obj_t *scr = lv_screen_active();
    int32_t sw = lv_obj_get_width(scr), sh = lv_obj_get_height(scr);
    int w = sw / scale, h = sh / scale;
    uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)sw, LV_COLOR_FORMAT_RGB565);
    size_t strip_bytes = (size_t)stride * STRIP_ROWS;
    strip = heap_caps_aligned_alloc(64, strip_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    row = heap_caps_malloc((size_t)w * 3 + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!strip || !row || lv_draw_buf_init(&buf, (uint32_t)sw, STRIP_ROWS, LV_COLOR_FORMAT_RGB565, stride, strip,
                                           (uint32_t)strip_bytes) != LV_RESULT_OK) {
        why = "not enough memory right now";
        goto done;
    }
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    put(&o, SIG, 8);
    uint8_t ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};   /* 8-bit RGB */
    chunk(&o, "IHDR", ihdr, sizeof(ihdr));
    size_t idat_at = o.len;
    put32(&o, 0);   /* the length, once known */
    put(&o, "IDAT", 4);
    if (tdefl_init(d, on_deflate, &o, DEFLATE_FLAGS) != TDEFL_STATUS_OKAY) {
        why = "the compressor wouldn't start";
        goto done;
    }
    for (int32_t y0 = 0; y0 < sh && !o.full; y0 += STRIP_ROWS) {
        int32_t rows = sh - y0 < STRIP_ROWS ? sh - y0 : STRIP_ROWS;
        buf.header.h = (uint32_t)rows;
        draw_strip(scr, &buf, y0, rows);
        for (int32_t r = 0; r + scale <= rows && !o.full; r += scale) {
            row[0] = 0;   /* filter: none */
            const uint16_t *p0 = (const uint16_t *)(strip + (size_t)r * stride);
            if (scale == 1) {
                for (int x = 0; x < w; x++) rgb(p0[x], row + 1 + 3 * x);
            } else {   /* 2x2 average */
                const uint16_t *p1 = (const uint16_t *)(strip + (size_t)(r + 1) * stride);
                for (int x = 0; x < w; x++) {
                    uint8_t a[3], b[3], c[3], e[3];
                    rgb(p0[2 * x], a);
                    rgb(p0[2 * x + 1], b);
                    rgb(p1[2 * x], c);
                    rgb(p1[2 * x + 1], e);
                    for (int k = 0; k < 3; k++) row[1 + 3 * x + k] = (uint8_t)((a[k] + b[k] + c[k] + e[k] + 2) / 4);
                }
            }
            if (tdefl_compress_buffer(d, row, (size_t)w * 3 + 1, TDEFL_NO_FLUSH) != TDEFL_STATUS_OKAY) o.full = true;
        }
    }
    if (!o.full && tdefl_compress_buffer(d, NULL, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE) o.full = true;
    luapp_unlock();
    scr = NULL;
    if (o.full) {
        why = o.cap >= PNG_MAX ? "the picture came out over 512 KB" : "not enough memory right now";
        goto out;
    }
    {
        uint32_t n = (uint32_t)(o.len - idat_at - 8);
        o.buf[idat_at] = n >> 24;
        o.buf[idat_at + 1] = n >> 16;
        o.buf[idat_at + 2] = n >> 8;
        o.buf[idat_at + 3] = n;
        put32(&o, esp_rom_crc32_le(0, o.buf + idat_at + 4, n + 4));
        chunk(&o, "IEND", NULL, 0);
    }
    if (o.full) {
        why = "not enough memory right now";
        goto out;
    }
    S.png = heap_caps_realloc(o.buf, o.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!S.png) S.png = o.buf;
    o.buf = NULL;
    S.len = o.len;
    S.w = w;
    S.h = h;
    S.scale = scale;
    S.at = esp_timer_get_time();
    ESP_LOGI(TAG, "screenshot %dx%d: %u bytes", w, h, (unsigned)S.len);
    goto out;
done:
    luapp_unlock();
out:
    free(o.buf);
    free(d);
    free(strip);
    free(row);
    return why;
}

/* offset 0 takes a new picture; later offsets page through it. */
cJSON *luapp_shot(int scale, size_t offset, const char **err)
{
    *err = NULL;
    if (offset == 0) {
        *err = take(scale == 1 ? 1 : 2);
        if (*err) return NULL;
    } else if (!S.png) {
        *err = "no picture to page through: start again at offset 0";
        return NULL;
    }
    if (offset > S.len) offset = S.len;
    size_t n = S.len - offset < PAGE_RAW ? S.len - offset : PAGE_RAW, olen = 0;
    size_t cap = 4 * ((n + 2) / 3) + 1;
    unsigned char *b64 = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b64 || mbedtls_base64_encode(b64, cap, &olen, S.png + offset, n) != 0) {
        free(b64);
        *err = "not enough memory right now";
        return NULL;
    }
    b64[olen] = 0;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "w", S.w);
    cJSON_AddNumberToObject(p, "h", S.h);
    cJSON_AddNumberToObject(p, "bytes", (double)S.len);
    cJSON_AddNumberToObject(p, "offset", (double)offset);
    if (offset + n < S.len) cJSON_AddNumberToObject(p, "next", (double)(offset + n));
    else cJSON_AddNullToObject(p, "next");
    cJSON_AddStringToObject(p, "png_base64", (const char *)b64);
    free(b64);
    if (offset + n >= S.len) luapp_shot_drop();   /* all of it sent */
    return p;
}
