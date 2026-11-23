/*
 * Images and canvases for Lua apps.
 *
 * Images (gfx.image, gfx.sprite) live in PSRAM, counted against GFX_CAP with
 * the canvases. They're reference counted: the Lua handle, each ui.image
 * showing one and each canvas op drawing one hold a reference, and one
 * nothing holds is freed under the display lock after the callback.
 *
 * Canvases record drawing ops during a callback (no lock, no LVGL) and draw
 * them in one go after it, through LVGL's draw API on the canvas's RGB565
 * buffer: anti-aliased shapes, text in the screen's fonts, and images,
 * scaled and rotated.
 */

#include <math.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "luapp_image.h"
#include "luapp_priv.h"
#include "muse_apps_priv.h"

#define GFX_CAP (1536 * 1024)      /* images + canvases, per app, when the board can spare it */
#define PSRAM_RESERVE MUSE_APPS_PSRAM_RESERVE
#define MAX_DIM 466
#define MAX_CANVAS 2
#define MAX_OPS 1500
#define ARENA_MAX 8192
#define IMAGE_MT "luapp.image"

/* LVGL could hold on to a freed image through its caches, which this build
 * turns off; freeing images as they go relies on that. */
#if CONFIG_LV_CACHE_DEF_SIZE != 0 || CONFIG_LV_IMAGE_HEADER_CACHE_DEF_CNT != 0
#error "luapp_gfx.c frees images LVGL may cache: drop them from the cache first"
#endif

static const char *TAG = "luapp_gfx";

static size_t s_bytes;          /* images + canvases in use */
static luapp_img_t *s_dead;     /* refs 0, to free under the lock */

/* ---- memory ---- */

/* How much more the app may have right now: up to GFX_CAP, and never into
 * the PSRAM the rest of the gadget (Muse's voice above all) needs. */
static size_t gfx_room(void)
{
    size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t spare = free > PSRAM_RESERVE ? free - PSRAM_RESERVE : 0;
    size_t cap = GFX_CAP > s_bytes ? GFX_CAP - s_bytes : 0;
    return spare < cap ? spare : cap;
}

static void *gfx_alloc(size_t n)
{
    if (n > gfx_room()) return NULL;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < n) return NULL;
    void *p = heap_caps_aligned_alloc(4, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) s_bytes += n;
    return p;
}

size_t luapp_gfx_bytes(void) { return s_bytes; }

static void gfx_free(void *p, size_t n)
{
    if (!p) return;
    heap_caps_free(p);
    s_bytes -= n;
}

static void *px_alloc(size_t n, void *ud)
{
    (void)ud;
    return gfx_alloc(n);
}

/* ---- images ---- */

void luapp_img_ref(luapp_img_t *img)
{
    if (img) img->refs++;
}

void luapp_img_unref(luapp_img_t *img)
{
    if (img && --img->refs == 0) {
        img->next = s_dead;
        s_dead = img;
    }
}

void luapp_gfx_collect(void)
{
    if (!s_dead || !luapp_lock()) return;
    while (s_dead) {
        luapp_img_t *img = s_dead;
        s_dead = img->next;
        gfx_free((void *)img->dsc.data, img->bytes);
        heap_caps_free(img);
    }
    luapp_unlock();
}

typedef struct {
    luapp_img_t *img;   /* NULL once freed */
} ibox_t;

luapp_img_t *luapp_gfx_check_image(lua_State *L, int idx)
{
    ibox_t *b = luaL_checkudata(L, idx, IMAGE_MT);
    if (!b->img) luaL_error(L, "the image was freed");
    return b->img;
}

static int img_gc(lua_State *L)
{
    ibox_t *b = luaL_checkudata(L, 1, IMAGE_MT);
    luapp_img_unref(b->img);
    b->img = NULL;
    return 0;
}

static int img_index(lua_State *L)
{
    ibox_t *b = luaL_checkudata(L, 1, IMAGE_MT);
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "free")) {
        lua_pushcfunction(L, img_gc);
        return 1;
    }
    if (!b->img) return luaL_error(L, "the image was freed");
    if (!strcmp(k, "w")) lua_pushinteger(L, (lua_Integer)b->img->dsc.header.w * b->img->scale);
    else if (!strcmp(k, "h")) lua_pushinteger(L, (lua_Integer)b->img->dsc.header.h * b->img->scale);
    else lua_pushnil(L);
    return 1;
}

/* Wraps decoded pixels in an image and its Lua handle (on the stack). */
static void push_image(lua_State *L, ibox_t *b, luapp_pixels_t *px)
{
    luapp_img_t *img = heap_caps_calloc(1, sizeof(*img), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!img) {
        gfx_free(px->px, px->bytes);
        luaL_error(L, "out of memory");
    }
    img->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    img->dsc.header.cf = px->alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    img->dsc.header.w = px->w;
    img->dsc.header.h = px->h;
    img->dsc.header.stride = (uint32_t)px->w * 2;
    img->dsc.data = px->px;
    img->dsc.data_size = (uint32_t)px->bytes;
    img->bytes = px->bytes;
    img->refs = 1;
    img->scale = 1;
    b->img = img;
}

/* Allocation failed for lack of room: collect garbage (dropping images the
 * app no longer holds) and say whether a retry might work. */
static bool reclaim(lua_State *L)
{
    size_t before = s_bytes;
    lua_gc(L, LUA_GCCOLLECT);
    luapp_gfx_collect();
    return s_bytes < before;
}

static ibox_t *new_box(lua_State *L)
{
    ibox_t *b = lua_newuserdatauv(L, sizeof(*b), 0);
    b->img = NULL;
    luaL_setmetatable(L, IMAGE_MT);
    return b;
}

static int l_image(lua_State *L)
{
    size_t n;
    const char *data = luaL_checklstring(L, 1, &n);
    ibox_t *b = new_box(L);
    for (int attempt = 0; attempt < 2; attempt++) {
        luapp_pixels_t px;
        const char *err = NULL;
        if (luapp_img_decode((const uint8_t *)data, n, MAX_DIM, px_alloc, NULL, &px, &err)) {
            push_image(L, b, &px);
            return 1;
        }
        gfx_free(px.px, px.bytes);
        if (attempt == 0 && px.px == NULL && err && strstr(err, "memory is full") && reclaim(L)) continue;
        return luaL_error(L, "gfx.image: %s", err ? err : "can't decode");
    }
    return 0;
}

static int l_sprite(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    int t = 1;
    /* rows: the table's array part, or its rows field */
    int rows_idx = t;
    if (lua_getfield(L, t, "rows") == LUA_TTABLE) {
        rows_idx = lua_gettop(L);
    } else {
        lua_pop(L, 1);
        lua_pushvalue(L, t);
        rows_idx = lua_gettop(L);
    }
    int nrows = (int)lua_rawlen(L, rows_idx);
    if (nrows < 1 || nrows > MAX_DIM) return luaL_error(L, "gfx.sprite: rows must be 1 to %d strings", MAX_DIM);
    const char **rows = lua_newuserdatauv(L, sizeof(char *) * (size_t)nrows, 0);
    size_t *lens = lua_newuserdatauv(L, sizeof(size_t) * (size_t)nrows, 0);
    for (int i = 0; i < nrows; i++) {
        lua_rawgeti(L, rows_idx, i + 1);
        if (!lua_isstring(L, -1)) return luaL_error(L, "gfx.sprite: row %d isn't a string", i + 1);
        rows[i] = lua_tolstring(L, -1, &lens[i]);
        lua_pop(L, 1);   /* still held by the rows table */
    }
    uint32_t pal[256];
    bool has[256];
    memset(has, 0, sizeof(has));
    if (lua_getfield(L, t, "palette") != LUA_TTABLE) return luaL_error(L, "gfx.sprite needs a palette");
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_type(L, -2) != LUA_TSTRING || lua_rawlen(L, -2) != 1 || !lua_isinteger(L, -1)) {
            return luaL_error(L, "gfx.sprite: palette maps one character to a colour, like [\"#\"]=0xFF0000");
        }
        unsigned char c = (unsigned char)lua_tostring(L, -2)[0];
        pal[c] = (uint32_t)lua_tointeger(L, -1) & 0xffffff;
        has[c] = true;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "scale");
    int scale = (int)luaL_optinteger(L, -1, 1);
    lua_pop(L, 1);
    if (scale < 1 || scale > 32) return luaL_error(L, "gfx.sprite: scale is 1 to 32");
    ibox_t *b = new_box(L);
    for (int attempt = 0; attempt < 2; attempt++) {
        luapp_pixels_t px;
        const char *err = NULL;
        /* 1x pixels, enlarged when drawn: scale x scale times less memory. */
        if (luapp_img_sprite(rows, lens, nrows, pal, has, 1, MAX_DIM / scale, px_alloc, NULL, &px, &err)) {
            push_image(L, b, &px);
            b->img->scale = (uint8_t)scale;
            return 1;
        }
        if (attempt == 0 && err && strstr(err, "memory is full") && reclaim(L)) continue;
        return luaL_error(L, "gfx.sprite: %s", err);
    }
    return 0;
}

/* used, available (what it could have in all right now), largest (the biggest
 * single image or canvas that fits right now: PSRAM is fragmented, and each
 * needs one unbroken block). */
static int l_memory(lua_State *L)
{
    size_t room = gfx_room(), block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    lua_pushinteger(L, (lua_Integer)s_bytes);
    lua_pushinteger(L, (lua_Integer)(s_bytes + room));
    lua_pushinteger(L, (lua_Integer)(room < block ? room : block));
    return 3;
}

/* ---- canvases ---- */

enum { OP_CLEAR, OP_RECT, OP_CIRCLE, OP_LINE, OP_TRI, OP_ARC, OP_TEXT, OP_IMAGE, OP_GRAD, OP_POLY, OP_TILES, OP_SPRITES };

#define POLY_MAX 32       /* polygon vertices */
#define TILES_MAX 4096    /* tilemap cells */
#define BATCH_MAX 256     /* c:sprites positions */

typedef struct {
    uint8_t op, size, align, fill, opa;
    int16_t a, b, c, d, e, f;
    int16_t width, radius, scale, angle;
    uint16_t text;      /* OP_TEXT, OP_POLY, OP_TILES, OP_SPRITES: offset of its data in the arena */
    uint32_t color;
    luapp_img_t *img;
} cop_t;

/* Up to RECTS areas of a canvas, merged as they come (LVGL itself keeps 32
 * invalid areas, then redraws the whole screen). */
#define RECTS 8
typedef struct {
    int n;
    lv_area_t r[RECTS];
} rects_t;

typedef struct {
    bool used;
    lv_obj_t *obj;
    int zoom;
    /* c:partial(true): only what changed is sent to the screen. pvalid: prev
     * holds everything drawn since the last clear (of pcolor); pfull: some of
     * it had no bounds. */
    bool partial, pvalid, pfull;
    uint32_t pcolor;
    rects_t prev;
    lv_draw_buf_t db;
    uint8_t *buf;
    size_t bytes;
    int w, h;
    uint32_t color;
    cop_t *ops;
    int nops;
    char *arena;
    int arena_len;
} canvas_t;

EXT_RAM_BSS_ATTR static canvas_t s_cv[MAX_CANVAS];

static unsigned kb(size_t n) { return (unsigned)((n + 512) / 1024); }

/* Why a w x h canvas (n bytes) didn't fit, with the numbers an app (or Muse
 * writing it) needs to fix it. Call with the failed slot already cleared. */
static const char *canvas_no_room(int w, int h, size_t n)
{
    EXT_RAM_BSS_ATTR static char msg[224];
    size_t cv = 0;
    for (int i = 0; i < MAX_CANVAS; i++) {
        if (s_cv[i].used) cv += s_cv[i].bytes;
    }
    if (s_bytes + n > GFX_CAP) {
        snprintf(msg, sizeof(msg),
                 "a %dx%d canvas needs %u KB, but %u of the app's %u KB is in use (images %u KB, canvases %u KB): "
                 "use zoom=2 (a quarter of the memory), a smaller canvas, or free images",
                 w, h, kb(n), kb(s_bytes), kb(GFX_CAP), kb(s_bytes - cv), kb(cv));
    } else {
        size_t room = gfx_room(), block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        snprintf(msg, sizeof(msg),
                 "a %dx%d canvas needs %u KB, but the board can spare only %u KB more right now (the app has images "
                 "%u KB, canvases %u KB): use zoom=2 (a quarter of the memory), a smaller canvas, or free images",
                 w, h, kb(n), kb(room < block ? room : block), kb(s_bytes - cv), kb(cv));
    }
    return msg;
}

int luapp_canvas_new(lua_State *L, int w, int h, uint32_t color, const char **err)
{
    int slot = -1;
    for (int i = 0; i < MAX_CANVAS; i++) {
        if (!s_cv[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        *err = "too many canvases (2 at most)";
        return -1;
    }
    canvas_t *c = &s_cv[slot];
    memset(c, 0, sizeof(*c));
    c->bytes = (size_t)w * h * 2;
    c->buf = gfx_alloc(c->bytes);
    if (!c->buf && reclaim(L)) c->buf = gfx_alloc(c->bytes);
    if (!c->buf) {
        size_t n = c->bytes;
        memset(c, 0, sizeof(*c));
        *err = canvas_no_room(w, h, n);
        return -1;
    }
    c->ops = heap_caps_malloc(sizeof(cop_t) * MAX_OPS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    c->arena = heap_caps_malloc(ARENA_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c->ops || !c->arena) {
        gfx_free(c->buf, c->bytes);
        heap_caps_free(c->ops);
        heap_caps_free(c->arena);
        memset(c, 0, sizeof(*c));
        *err = "the board is low on memory right now: try again, or use a smaller canvas";
        return -1;
    }
    c->w = w;
    c->h = h;
    c->color = color;
    lv_draw_buf_init(&c->db, (uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_RGB565, (uint32_t)w * 2, c->buf, (uint32_t)c->bytes);
    c->used = true;
    return slot;
}

void luapp_canvas_attach(int slot, lv_obj_t *obj, int zoom)
{
    canvas_t *c = &s_cv[slot];
    c->obj = obj;
    c->zoom = zoom > 1 ? zoom : 1;
    c->pvalid = false;
    lv_canvas_set_draw_buf(obj, &c->db);
    lv_canvas_fill_bg(obj, lv_color_hex(c->color), LV_OPA_COVER);
    if (zoom > 1) {
        /* Shown zoom times its size, each pixel a crisp zoom x zoom block; the
         * object covers all of it, so taps land on it everywhere. */
        lv_obj_set_size(obj, c->w * zoom, c->h * zoom);
        lv_image_set_antialias(obj, false);
        lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_STRETCH);
    }
}

void luapp_canvas_set_color(int slot, uint32_t color)
{
    if (slot >= 0 && slot < MAX_CANVAS && s_cv[slot].used) s_cv[slot].color = color;
}

static void drop_ops(canvas_t *c)
{
    for (int i = 0; i < c->nops; i++) luapp_img_unref(c->ops[i].img);
    c->nops = 0;
    c->arena_len = 0;
}

void luapp_canvas_free(int slot)
{
    if (slot < 0 || slot >= MAX_CANVAS || !s_cv[slot].used) return;
    canvas_t *c = &s_cv[slot];
    drop_ops(c);
    gfx_free(c->buf, c->bytes);
    heap_caps_free(c->ops);
    heap_caps_free(c->arena);
    memset(c, 0, sizeof(*c));
}

/* ---- direct pixel ops: polygons, tilemaps and sprite batches write the
 * RGB565 buffer themselves (exact, seamless, and far faster than an LVGL
 * draw task per tile), between LVGL's layers. ---- */

static inline uint16_t blend565(uint16_t fg, uint16_t bg, unsigned a)
{
    if (a >= 255) return fg;
    a += a >> 7;   /* 0..256 */
    unsigned r = (((fg >> 11) * a + (bg >> 11) * (256 - a)) >> 8) & 0x1f;
    unsigned g = ((((fg >> 5) & 0x3f) * a + ((bg >> 5) & 0x3f) * (256 - a)) >> 8) & 0x3f;
    unsigned b = (((fg & 0x1f) * a + (bg & 0x1f) * (256 - a)) >> 8) & 0x1f;
    return (uint16_t)(r << 11 | g << 5 | b);
}

/* Even-odd scanline fill at pixel centres: any simple or concave shape. */
static void fill_poly(canvas_t *c, const int16_t *v, int n, uint32_t color, uint8_t opa)
{
    if (!opa) return;
    uint16_t col = lv_color_to_u16(lv_color_hex(color));
    uint16_t *px = (uint16_t *)c->buf;
    int ymin = v[1], ymax = v[1];
    for (int i = 1; i < n; i++) {
        if (v[2 * i + 1] < ymin) ymin = v[2 * i + 1];
        if (v[2 * i + 1] > ymax) ymax = v[2 * i + 1];
    }
    if (ymin < 0) ymin = 0;
    if (ymax > c->h) ymax = c->h;
    float xs[POLY_MAX];
    for (int y = ymin; y < ymax; y++) {
        float yc = y + 0.5f;
        int k = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            float y0 = v[2 * j + 1], y1 = v[2 * i + 1];
            if ((y0 <= yc) != (y1 <= yc)) {
                float x = v[2 * j] + (yc - y0) * (v[2 * i] - v[2 * j]) / (y1 - y0);
                int m = k++;
                while (m > 0 && xs[m - 1] > x) {   /* insertion sort */
                    xs[m] = xs[m - 1];
                    m--;
                }
                xs[m] = x;
            }
        }
        uint16_t *row = px + (size_t)y * c->w;
        for (int i = 0; i + 1 < k; i += 2) {
            int xa = (int)ceilf(xs[i] - 0.5f), xb = (int)ceilf(xs[i + 1] - 0.5f);
            if (xa < 0) xa = 0;
            if (xb > c->w) xb = c->w;
            for (int x = xa; x < xb; x++) row[x] = opa == 255 ? col : blend565(col, row[x], opa);
        }
    }
}

/* Source rectangle (sx, sy, sw, sh) of img, in its 1x pixels, drawn with its
 * top-left at dx, dy, each pixel k x k; transparency kept. */
static void blit(canvas_t *c, const luapp_img_t *img, int sx, int sy, int sw, int sh, int dx, int dy)
{
    int k = img->scale ? img->scale : 1, iw = img->dsc.header.w;
    int x0 = dx < 0 ? 0 : dx, y0 = dy < 0 ? 0 : dy;
    int x1 = dx + sw * k, y1 = dy + sh * k;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x0 >= x1 || y0 >= y1) return;
    const uint16_t *src = (const uint16_t *)img->dsc.data;
    const uint8_t *al = img->dsc.header.cf == LV_COLOR_FORMAT_RGB565A8
                            ? img->dsc.data + (size_t)iw * img->dsc.header.h * 2 : NULL;
    uint16_t *px = (uint16_t *)c->buf;
    for (int y = y0; y < y1; y++) {
        size_t r = (size_t)(sy + (y - dy) / k) * iw;
        uint16_t *row = px + (size_t)y * c->w;
        for (int x = x0; x < x1; x++) {
            size_t q = r + sx + (x - dx) / k;
            unsigned a = al ? al[q] : 255;
            if (a == 255) row[x] = src[q];
            else if (a) row[x] = blend565(src[q], row[x], a);
        }
    }
}

static void draw_tiles(canvas_t *c, const cop_t *o)
{
    const uint8_t *map = (const uint8_t *)c->arena + o->text;
    int k = o->img->scale ? o->img->scale : 1, tw = o->c, th = o->d, cols = o->e;
    int per_row = (int)o->img->dsc.header.w / tw;
    for (int i = 0; i < o->width; i++) {
        if (!map[i]) continue;
        int t = map[i] - 1;
        blit(c, o->img, (t % per_row) * tw, (t / per_row) * th, tw, th, o->a + (i % cols) * tw * k,
             o->b + (i / cols) * th * k);
    }
}

static void draw_sprites(canvas_t *c, const cop_t *o)
{
    const int16_t *p = (const int16_t *)(c->arena + o->text);
    int w = o->img->dsc.header.w, h = o->img->dsc.header.h;
    for (int i = 0; i < o->width; i++) blit(c, o->img, 0, 0, w, h, p[2 * i], p[2 * i + 1]);
}

/* ---- partial redraw (c:partial) ---- */

static int64_t rect_size(const lv_area_t *a) { return (int64_t)(a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1); }

static lv_area_t rect_join(const lv_area_t *a, const lv_area_t *b)
{
    return (lv_area_t){LV_MIN(a->x1, b->x1), LV_MIN(a->y1, b->y1), LV_MAX(a->x2, b->x2), LV_MAX(a->y2, b->y2)};
}

static void rects_add(rects_t *s, lv_area_t r)
{
    for (int i = 0; i < s->n; i++) {   /* touching or within 8 px: one area */
        const lv_area_t *q = &s->r[i];
        if (r.x1 <= q->x2 + 8 && q->x1 <= r.x2 + 8 && r.y1 <= q->y2 + 8 && q->y1 <= r.y2 + 8) {
            s->r[i] = rect_join(q, &r);
            return;
        }
    }
    if (s->n < RECTS) {
        s->r[s->n++] = r;
        return;
    }
    int best = 0;   /* full: into the one it grows least */
    int64_t grow = INT64_MAX;
    for (int i = 0; i < s->n; i++) {
        lv_area_t j = rect_join(&s->r[i], &r);
        int64_t g = rect_size(&j) - rect_size(&s->r[i]);
        if (g < grow) grow = g, best = i;
    }
    s->r[best] = rect_join(&s->r[best], &r);
}

/* Adds x1..x2, y1..y2 (canvas pixels) clipped to the canvas, if any of it is on it. */
static void add_box(const canvas_t *c, rects_t *s, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    x1 = LV_MAX(x1, 0), y1 = LV_MAX(y1, 0), x2 = LV_MIN(x2, c->w - 1), y2 = LV_MIN(y2, c->h - 1);
    if (x1 <= x2 && y1 <= y2) rects_add(s, (lv_area_t){x1, y1, x2, y2});
}

/* Where an op can paint; false when it can't be bounded. Generous by a pixel
 * or two for antialiasing. */
static bool op_rects(const canvas_t *c, const cop_t *o, rects_t *s)
{
    switch (o->op) {
    case OP_RECT:
    case OP_GRAD:
        add_box(c, s, o->a, o->b, o->a + o->c - 1, o->b + o->d - 1);
        return true;
    case OP_CIRCLE:
        add_box(c, s, o->a - o->c - 1, o->b - o->c - 1, o->a + o->c + 1, o->b + o->c + 1);
        return true;
    case OP_ARC:
        add_box(c, s, o->a - o->c - 2, o->b - o->c - 2, o->a + o->c + 2, o->b + o->c + 2);
        return true;
    case OP_LINE: {
        int m = o->width / 2 + 2;
        add_box(c, s, LV_MIN(o->a, o->c) - m, LV_MIN(o->b, o->d) - m, LV_MAX(o->a, o->c) + m,
                LV_MAX(o->b, o->d) + m);
        return true;
    }
    case OP_TRI:
        add_box(c, s, LV_MIN(o->a, LV_MIN(o->c, o->e)) - 1, LV_MIN(o->b, LV_MIN(o->d, o->f)) - 1,
                LV_MAX(o->a, LV_MAX(o->c, o->e)) + 1, LV_MAX(o->b, LV_MAX(o->d, o->f)) + 1);
        return true;
    case OP_TEXT: {
        /* No glyph widths here: a character is at most a line height wide. */
        const lv_font_t *f = luapp_font(o->size);
        int lh = lv_font_get_line_height(f), lines = 1, n = 0, most = 0;
        for (const unsigned char *t = (const unsigned char *)c->arena + o->text; *t; t++) {
            if (*t == '\n') {
                lines++;
                most = LV_MAX(most, n);
                n = 0;
            } else if ((*t & 0xC0) != 0x80) {
                n++;
            }
        }
        most = LV_MAX(most, n);
        int w = most * lh, h = LV_MIN(lines, 8) * lh;
        int x = o->align == 1 ? o->a - w / 2 : o->align == 2 ? o->a - w : o->a;
        add_box(c, s, x - 2, o->b - 2, x + w + 2, o->b + h + 2);
        return true;
    }
    case OP_IMAGE: {
        int k = o->img->scale ? o->img->scale : 1, w = o->img->dsc.header.w, h = o->img->dsc.header.h;
        int sw = w * k * o->scale / 100 + 2, sh = h * k * o->scale / 100 + 2;
        if (o->angle) {   /* turning about its centre: within a circle round it */
            int cx = o->a + k * w / 2, cy = o->b + k * h / 2, r = (sw + sh) / 2 + 2;
            add_box(c, s, cx - r, cy - r, cx + r, cy + r);
        } else {
            add_box(c, s, o->a - 1, o->b - 1, o->a + sw, o->b + sh);
        }
        return true;
    }
    case OP_POLY: {
        const int16_t *v = (const int16_t *)(c->arena + o->text);
        int32_t x1 = v[0], y1 = v[1], x2 = v[0], y2 = v[1];
        for (int i = 1; i < o->width; i++) {
            x1 = LV_MIN(x1, v[2 * i]), x2 = LV_MAX(x2, v[2 * i]);
            y1 = LV_MIN(y1, v[2 * i + 1]), y2 = LV_MAX(y2, v[2 * i + 1]);
        }
        add_box(c, s, x1 - 1, y1 - 1, x2 + 1, y2 + 1);
        return true;
    }
    case OP_TILES: {
        int k = o->img->scale ? o->img->scale : 1, rows = (o->width + o->e - 1) / o->e;
        add_box(c, s, o->a, o->b, o->a + o->e * o->c * k - 1, o->b + rows * o->d * k - 1);
        return true;
    }
    case OP_SPRITES: {
        int k = o->img->scale ? o->img->scale : 1;
        int w = o->img->dsc.header.w * k, h = o->img->dsc.header.h * k;
        const int16_t *p = (const int16_t *)(c->arena + o->text);
        for (int i = 0; i < o->width; i++) add_box(c, s, p[2 * i], p[2 * i + 1], p[2 * i] + w - 1, p[2 * i + 1] + h - 1);
        return true;
    }
    default:
        return false;
    }
}

/* Under the lock, after drawing ops[from..]: tells LVGL what to redraw. With
 * c:partial, only where those ops painted, plus (after a clear of the same
 * colour) where everything since the previous clear did: the rest is as it was. */
static void invalidate(canvas_t *c, int from, int clear)
{
    if (c->partial) lv_display_enable_invalidation(lv_obj_get_display(c->obj), true);
    if (!c->partial) {
        lv_obj_invalidate(c->obj);
        return;
    }
    rects_t cur = {0};
    bool unbounded = false;
    for (int i = from; i < c->nops && !unbounded; i++) unbounded = !op_rects(c, &c->ops[i], &cur);
    bool full = unbounded;
    rects_t out = cur;
    if (clear >= 0) {
        uint32_t col = c->ops[clear].color;
        if (!c->pvalid || c->pfull || col != c->pcolor) full = true;
        for (int i = 0; !full && i < c->prev.n; i++) rects_add(&out, c->prev.r[i]);
        c->pvalid = true;
        c->pcolor = col;
        c->pfull = unbounded;
        c->prev = cur;
    } else {
        c->pfull |= unbounded;
        for (int i = 0; i < cur.n; i++) rects_add(&c->prev, cur.r[i]);
    }
    int64_t sum = 0;
    for (int i = 0; i < out.n; i++) sum += rect_size(&out.r[i]);
    if (full || sum * 10 >= (int64_t)c->w * c->h * 7) {   /* most of it anyway: in one go */
        lv_obj_invalidate(c->obj);
        return;
    }
    lv_area_t co;
    lv_obj_get_coords(c->obj, &co);
    int z = c->zoom;
    for (int i = 0; i < out.n; i++) {
        const lv_area_t *r = &out.r[i];
        lv_area_t a = {co.x1 + r->x1 * z, co.y1 + r->y1 * z, co.x1 + (r->x2 + 1) * z - 1, co.y1 + (r->y2 + 1) * z - 1};
        lv_obj_invalidate_area(c->obj, &a);
    }
}

/* Under the lock. */
static void draw_ops(canvas_t *c)
{
    /* LVGL's canvas calls (fill_bg, finish_layer) invalidate the whole canvas:
     * with partial, keep them from it; invalidate() turns it back on. */
    if (c->partial) lv_display_enable_invalidation(lv_obj_get_display(c->obj), false);
    int from = 0, clear = -1;
    for (int i = c->nops - 1; i >= 0; i--) {
        if (c->ops[i].op == OP_CLEAR) {
            lv_canvas_fill_bg(c->obj, lv_color_hex(c->ops[i].color), LV_OPA_COVER);
            from = i + 1;
            clear = i;
            break;
        }
    }
    if (from >= c->nops) {
        invalidate(c, from, clear);
        return;
    }
    lv_layer_t layer;
    bool open = false;   /* LVGL's layer: direct pixel ops wait for what it queued */
    for (int i = from; i < c->nops; i++) {
        const cop_t *o = &c->ops[i];
        bool direct = o->op == OP_POLY || o->op == OP_TILES || o->op == OP_SPRITES;
        if (direct && open) {
            lv_canvas_finish_layer(c->obj, &layer);
            open = false;
        } else if (!direct && !open) {
            lv_canvas_init_layer(c->obj, &layer);
            open = true;
        }
        switch (o->op) {
        case OP_POLY:
            fill_poly(c, (const int16_t *)(c->arena + o->text), o->width, o->color, o->opa);
            break;
        case OP_TILES:
            draw_tiles(c, o);
            break;
        case OP_SPRITES:
            draw_sprites(c, o);
            break;
        case OP_GRAD: {
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            lv_area_t a = {o->a, o->b, o->a + o->c - 1, o->b + o->d - 1};
            uint32_t c2 = (uint16_t)o->e | (uint32_t)(uint8_t)o->f << 16;
            d.radius = o->radius;
            d.bg_opa = o->opa;
            d.bg_color = lv_color_hex(o->color);
            d.bg_grad.dir = o->align ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_VER;
            d.bg_grad.stops_count = 2;
            d.bg_grad.stops[0] = (lv_grad_stop_t){lv_color_hex(o->color), LV_OPA_COVER, 0};
            d.bg_grad.stops[1] = (lv_grad_stop_t){lv_color_hex(c2), LV_OPA_COVER, 255};
            lv_draw_rect(&layer, &d, &a);
            break;
        }
        case OP_RECT:
        case OP_CIRCLE: {
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            lv_area_t a;
            if (o->op == OP_RECT) {
                a = (lv_area_t){o->a, o->b, o->a + o->c - 1, o->b + o->d - 1};
                d.radius = o->radius;
            } else {
                a = (lv_area_t){o->a - o->c, o->b - o->c, o->a + o->c, o->b + o->c};
                d.radius = LV_RADIUS_CIRCLE;
            }
            if (o->fill) {
                d.bg_color = lv_color_hex(o->color);
                d.bg_opa = o->opa;
            } else {
                d.bg_opa = LV_OPA_TRANSP;
                d.border_color = lv_color_hex(o->color);
                d.border_width = o->width;
                d.border_opa = o->opa;
            }
            lv_draw_rect(&layer, &d, &a);
            break;
        }
        case OP_LINE: {
            lv_draw_line_dsc_t d;
            lv_draw_line_dsc_init(&d);
            d.p1.x = o->a;
            d.p1.y = o->b;
            d.p2.x = o->c;
            d.p2.y = o->d;
            d.color = lv_color_hex(o->color);
            d.width = o->width;
            d.opa = o->opa;
            d.round_start = d.round_end = 1;
            lv_draw_line(&layer, &d);
            break;
        }
        case OP_TRI: {
            lv_draw_triangle_dsc_t d;
            lv_draw_triangle_dsc_init(&d);
            d.p[0].x = o->a;
            d.p[0].y = o->b;
            d.p[1].x = o->c;
            d.p[1].y = o->d;
            d.p[2].x = o->e;
            d.p[2].y = o->f;
            d.color = lv_color_hex(o->color);
            d.opa = o->opa;
            lv_draw_triangle(&layer, &d);
            break;
        }
        case OP_ARC: {
            lv_draw_arc_dsc_t d;
            lv_draw_arc_dsc_init(&d);
            d.center.x = o->a;
            d.center.y = o->b;
            d.radius = (uint16_t)o->c;
            d.start_angle = o->d - 90;   /* from the top, clockwise */
            d.end_angle = o->d - 90 + o->e;
            d.width = o->width;
            d.color = lv_color_hex(o->color);
            d.opa = o->opa;
            d.rounded = 1;
            lv_draw_arc(&layer, &d);
            break;
        }
        case OP_TEXT: {
            lv_draw_label_dsc_t d;
            lv_draw_label_dsc_init(&d);
            d.font = luapp_font(o->size);
            d.color = lv_color_hex(o->color);
            d.opa = o->opa;
            d.text = c->arena + o->text;
            int lh = lv_font_get_line_height(d.font);
            lv_area_t a;
            if (o->align == 1) {
                a = (lv_area_t){o->a - 1000, o->b, o->a + 1000, o->b + lh * 8};
                d.align = LV_TEXT_ALIGN_CENTER;
            } else if (o->align == 2) {
                a = (lv_area_t){o->a - 2000, o->b, o->a, o->b + lh * 8};
                d.align = LV_TEXT_ALIGN_RIGHT;
            } else {
                a = (lv_area_t){o->a, o->b, o->a + 2000, o->b + lh * 8};
                d.align = LV_TEXT_ALIGN_LEFT;
            }
            lv_draw_label(&layer, &d, &a);
            break;
        }
        case OP_IMAGE: {
            lv_draw_image_dsc_t d;
            lv_draw_image_dsc_init(&d);
            int w = o->img->dsc.header.w, h = o->img->dsc.header.h, k = o->img->scale;
            d.src = &o->img->dsc;
            d.opa = o->opa;
            d.scale_x = d.scale_y = o->scale * 256 * k / 100;
            d.rotation = o->angle * 10;
            int x = o->a, y = o->b;
            if (o->angle) {
                /* About the centre of the enlarged sprite, as if it were k times bigger. */
                d.pivot.x = w / 2;
                d.pivot.y = h / 2;
                d.antialias = 1;
                x += (k * w - w) / 2;
                y += (k * h - h) / 2;
            } else {
                d.pivot.x = 0;
                d.pivot.y = 0;
                d.antialias = 0;
            }
            lv_area_t a = {x, y, x + w - 1, y + h - 1};
            lv_draw_image(&layer, &d, &a);
            break;
        }
        default:
            break;
        }
    }
    if (open) lv_canvas_finish_layer(c->obj, &layer);
    invalidate(c, from, clear);   /* direct pixel ops don't */
}

static void flush_one(canvas_t *c)
{
    if (!c->nops) return;
    if (luapp_lock()) {
        int64_t t0 = esp_timer_get_time();
        draw_ops(c);
        luapp_perf_draw(esp_timer_get_time() - t0, false);
        luapp_unlock();
    }
    drop_ops(c);
}

void luapp_gfx_flush(void)
{
    bool frame = false;
    for (int i = 0; i < MAX_CANVAS; i++) {
        if (s_cv[i].used && s_cv[i].nops) {
            frame = true;
            flush_one(&s_cv[i]);
        }
    }
    if (frame) luapp_perf_draw(0, true);   /* one app frame, however many canvases */
    luapp_gfx_collect();
}

void luapp_gfx_reset(void)
{
    for (int i = 0; i < MAX_CANVAS; i++) luapp_canvas_free(i);
    luapp_gfx_collect();
    if (s_bytes) ESP_LOGW(TAG, "%u bytes of images still held after the app stopped", (unsigned)s_bytes);
}

/* ---- canvas methods ---- */

static int16_t coord(lua_State *L, int i)
{
    lua_Number v = luaL_checknumber(L, i);
    return (int16_t)(v < -4000 ? -4000 : v > 4000 ? 4000 : lround(v));
}

static uint32_t color_arg(lua_State *L, int i) { return (uint32_t)luaL_checkinteger(L, i) & 0xffffff; }

static int opt_field(lua_State *L, int t, const char *k, int def, int lo, int hi)
{
    if (!lua_istable(L, t)) return def;
    int v = def;
    if (lua_getfield(L, t, k) != LUA_TNIL) {
        lua_Number n = luaL_checknumber(L, -1);
        v = (int)(n < lo ? lo : n > hi ? hi : lround(n));
    }
    lua_pop(L, 1);
    return v;
}

static bool opt_bool(lua_State *L, int t, const char *k, bool def)
{
    if (!lua_istable(L, t)) return def;
    bool v = def;
    if (lua_getfield(L, t, k) != LUA_TNIL) v = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

/* A new op on the canvas at stack index 1 (drawing what's queued if full). */
static cop_t *new_op(lua_State *L, canvas_t **out, uint8_t kind)
{
    canvas_t *c = &s_cv[luapp_canvas_arg(L, 1)];
    if (c->nops >= MAX_OPS) flush_one(c);
    if (!c->obj) luaL_error(L, "canvas isn't ready");
    cop_t *o = &c->ops[c->nops++];
    memset(o, 0, sizeof(*o));
    o->op = kind;
    o->opa = 255;
    *out = c;
    return o;
}

static int c_clear(lua_State *L)
{
    canvas_t *c = &s_cv[luapp_canvas_arg(L, 1)];
    uint32_t color = lua_isnoneornil(L, 2) ? c->color : color_arg(L, 2);
    drop_ops(c);   /* everything before is painted over */
    cop_t *o = new_op(L, &c, OP_CLEAR);
    o->color = color;
    return 0;
}

static int c_rect(lua_State *L)
{
    int16_t x = coord(L, 2), y = coord(L, 3), w = coord(L, 4), h = coord(L, 5);
    uint32_t color = color_arg(L, 6);
    canvas_t *c;
    if (w <= 0 || h <= 0) return 0;
    int fill = opt_bool(L, 7, "fill", true);
    int width = opt_field(L, 7, "width", 2, 1, 100);
    int radius = opt_field(L, 7, "radius", 0, 0, 1000);
    int opa = opt_field(L, 7, "opacity", 255, 0, 255);
    cop_t *o = new_op(L, &c, OP_RECT);
    o->a = x, o->b = y, o->c = w, o->d = h;
    o->color = color, o->fill = (uint8_t)fill, o->width = (int16_t)width, o->radius = (int16_t)radius;
    o->opa = (uint8_t)opa;
    return 0;
}

static int c_circle(lua_State *L)
{
    int16_t x = coord(L, 2), y = coord(L, 3), r = coord(L, 4);
    uint32_t color = color_arg(L, 5);
    canvas_t *c;
    if (r <= 0) return 0;
    int fill = opt_bool(L, 6, "fill", true);
    int width = opt_field(L, 6, "width", 2, 1, 100);
    int opa = opt_field(L, 6, "opacity", 255, 0, 255);
    cop_t *o = new_op(L, &c, OP_CIRCLE);
    o->a = x, o->b = y, o->c = r, o->color = color, o->fill = (uint8_t)fill, o->width = (int16_t)width;
    o->opa = (uint8_t)opa;
    return 0;
}

static int c_line(lua_State *L)
{
    int16_t x1 = coord(L, 2), y1 = coord(L, 3), x2 = coord(L, 4), y2 = coord(L, 5);
    uint32_t color = color_arg(L, 6);
    int width = (int)luaL_optinteger(L, 7, 2);
    canvas_t *c;
    cop_t *o = new_op(L, &c, OP_LINE);
    o->a = x1, o->b = y1, o->c = x2, o->d = y2, o->color = color;
    o->width = (int16_t)(width < 1 ? 1 : width > 100 ? 100 : width);
    return 0;
}

static int c_triangle(lua_State *L)
{
    int16_t v[6];
    for (int i = 0; i < 6; i++) v[i] = coord(L, 2 + i);
    uint32_t color = color_arg(L, 8);
    canvas_t *c;
    cop_t *o = new_op(L, &c, OP_TRI);
    o->a = v[0], o->b = v[1], o->c = v[2], o->d = v[3], o->e = v[4], o->f = v[5], o->color = color;
    return 0;
}

static int c_arc(lua_State *L)
{
    int16_t x = coord(L, 2), y = coord(L, 3), r = coord(L, 4), start = coord(L, 5), sweep = coord(L, 6);
    uint32_t color = color_arg(L, 7);
    int width = (int)luaL_optinteger(L, 8, 4);
    canvas_t *c;
    if (r <= 0 || sweep == 0) return 0;
    cop_t *o = new_op(L, &c, OP_ARC);
    o->a = x, o->b = y, o->c = r, o->d = start, o->e = sweep, o->color = color;
    o->width = (int16_t)(width < 1 ? 1 : width > 200 ? 200 : width);
    return 0;
}

static int c_text(lua_State *L)
{
    size_t n;
    const char *s = luaL_tolstring(L, 2, &n);
    int16_t x = coord(L, 3), y = coord(L, 4);
    uint32_t color = lua_isnoneornil(L, 5) ? 0xffffff : color_arg(L, 5);
    int size = (int)luaL_optinteger(L, 6, 16);
    const char *align = luaL_optstring(L, 7, "left");
    canvas_t *c = &s_cv[luapp_canvas_arg(L, 1)];
    if (n > 400) n = 400;
    if (c->nops >= MAX_OPS || c->arena_len + (int)n + 1 > ARENA_MAX) flush_one(c);
    int off = c->arena_len;
    memcpy(c->arena + off, s, n);
    c->arena[off + n] = 0;
    c->arena_len += (int)n + 1;
    cop_t *o = new_op(L, &c, OP_TEXT);
    o->a = x, o->b = y, o->color = color, o->size = (uint8_t)(size < 8 ? 8 : size > 64 ? 64 : size);
    o->align = !strcmp(align, "center") ? 1 : !strcmp(align, "right") ? 2 : 0;
    o->text = (uint16_t)off;
    return 0;
}

static int c_image(lua_State *L)
{
    luapp_img_t *img = luapp_gfx_check_image(L, 2);
    int16_t x = coord(L, 3), y = coord(L, 4);
    int scale = opt_field(L, 5, "scale", 100, 1, 1600);
    int angle = opt_field(L, 5, "angle", 0, -3600, 3600);
    int opa = opt_field(L, 5, "opacity", 255, 0, 255);
    canvas_t *c;
    cop_t *o = new_op(L, &c, OP_IMAGE);
    o->a = x, o->b = y, o->scale = (int16_t)scale, o->angle = (int16_t)angle, o->opa = (uint8_t)opa;
    o->img = img;
    luapp_img_ref(img);
    return 0;
}

static int c_pixel(lua_State *L)
{
    int16_t x = coord(L, 2), y = coord(L, 3);
    uint32_t color = color_arg(L, 4);
    canvas_t *c;
    cop_t *o = new_op(L, &c, OP_RECT);
    o->a = x, o->b = y, o->c = 1, o->d = 1, o->color = color, o->fill = 1;
    return 0;
}

/* n bytes of the canvas's arena for an op's data, drawing what's queued
 * first if the arena or the op list is full. Returns its offset. */
static int arena_take(lua_State *L, canvas_t *c, int n, const char *what)
{
    int off = (c->arena_len + 1) & ~1;   /* int16 data */
    if (c->nops >= MAX_OPS || off + n > ARENA_MAX) {
        flush_one(c);
        off = 0;
    }
    if (n > ARENA_MAX) luaL_error(L, "%s: too much data", what);
    c->arena_len = off + n;
    return off;
}

/* c:polygon(points, color [, opacity or {opacity=}]): filled; points =
 * {x1, y1, x2, y2, ...}, 3 to 32 corners; any shape (overlaps: even-odd). */
static int c_polygon(lua_State *L)
{
    canvas_t *c = &s_cv[luapp_canvas_arg(L, 1)];
    luaL_checktype(L, 2, LUA_TTABLE);
    uint32_t color = color_arg(L, 3);
    int opa = lua_isnumber(L, 4) ? (int)luaL_checkinteger(L, 4) : opt_field(L, 4, "opacity", 255, 0, 255);
    opa = opa < 0 ? 0 : opa > 255 ? 255 : opa;
    int len = (int)lua_rawlen(L, 2);
    if (len < 6 || len > 2 * POLY_MAX || len % 2) {
        return luaL_error(L, "c:polygon: points is {x1, y1, x2, y2, ...}, 3 to %d corners", POLY_MAX);
    }
    int off = arena_take(L, c, len * 2, "c:polygon");
    int16_t *v = (int16_t *)(c->arena + off);
    for (int i = 0; i < len; i++) {
        lua_rawgeti(L, 2, i + 1);
        v[i] = coord(L, -1);
        lua_pop(L, 1);
    }
    cop_t *o = new_op(L, &c, OP_POLY);
    o->text = (uint16_t)off, o->width = (int16_t)(len / 2), o->color = color, o->opa = (uint8_t)opa;
    return 0;
}

/* c:gradient(x, y, w, h, color1, color2 [, "v" | "h" or {dir="v", radius=0, opacity=255}]) */
static int c_gradient(lua_State *L)
{
    int16_t x = coord(L, 2), y = coord(L, 3), w = coord(L, 4), h = coord(L, 5);
    uint32_t c1 = color_arg(L, 6), c2 = color_arg(L, 7);
    bool hor = false;
    if (lua_type(L, 8) == LUA_TSTRING) {
        hor = lua_tostring(L, 8)[0] == 'h';
    } else if (lua_istable(L, 8)) {
        if (lua_getfield(L, 8, "dir") == LUA_TSTRING) hor = lua_tostring(L, -1)[0] == 'h';
        lua_pop(L, 1);
    }
    int radius = opt_field(L, 8, "radius", 0, 0, 1000);
    int opa = opt_field(L, 8, "opacity", 255, 0, 255);
    canvas_t *c;
    if (w <= 0 || h <= 0) return 0;
    cop_t *o = new_op(L, &c, OP_GRAD);
    o->a = x, o->b = y, o->c = w, o->d = h, o->color = c1;
    o->e = (int16_t)(uint16_t)(c2 & 0xffff), o->f = (int16_t)(c2 >> 16);
    o->align = hor, o->radius = (int16_t)radius, o->opa = (uint8_t)opa;
    return 0;
}

/* c:tilemap(sheet, tile_w, tile_h, map, cols [, x=0, y=0]): sheet is an image
 * of tiles left to right, top to bottom (numbered from 1); map a flat list of
 * tile numbers, cols to a row, 0 = empty. Tile sizes are in drawn pixels, like
 * sheet.w (a sprite's scale enlarges every tile). One op. */
static int c_tilemap(lua_State *L)
{
    canvas_t *c = &s_cv[luapp_canvas_arg(L, 1)];
    luapp_img_t *img = luapp_gfx_check_image(L, 2);
    int k = img->scale ? img->scale : 1;
    int tw = (int)luaL_checkinteger(L, 3), th = (int)luaL_checkinteger(L, 4);
    luaL_checktype(L, 5, LUA_TTABLE);
    int cols = (int)luaL_checkinteger(L, 6);
    int16_t x = lua_isnoneornil(L, 7) ? 0 : coord(L, 7), y = lua_isnoneornil(L, 8) ? 0 : coord(L, 8);
    int iw = img->dsc.header.w, ih = img->dsc.header.h;
    if (tw < k || th < k || tw % k || th % k) {
        return luaL_error(L, "c:tilemap: tile sizes must be whole multiples of the sheet's scale (%d)", k);
    }
    tw /= k, th /= k;
    if (tw > iw || th > ih) return luaL_error(L, "c:tilemap: tiles are bigger than the sheet");
    int ntiles = (iw / tw) * (ih / th);
    int n = (int)lua_rawlen(L, 5);
    if (cols < 1 || n < 1 || n > TILES_MAX) {
        return luaL_error(L, "c:tilemap: map needs 1 to %d tile numbers and cols >= 1", TILES_MAX);
    }
    int off = arena_take(L, c, n, "c:tilemap");
    uint8_t *map = (uint8_t *)c->arena + off;
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 5, i + 1);
        lua_Integer t = lua_tointeger(L, -1);
        lua_pop(L, 1);
        if (t < 0 || t > ntiles || t > 255) {
            return luaL_error(L, "c:tilemap: map[%d] is %d, but the sheet has %d tiles (255 at most)", i + 1,
                              (int)t, ntiles);
        }
        map[i] = (uint8_t)t;
    }
    cop_t *o = new_op(L, &c, OP_TILES);
    o->a = x, o->b = y, o->c = (int16_t)tw, o->d = (int16_t)th, o->e = (int16_t)cols, o->width = (int16_t)n;
    o->text = (uint16_t)off;
    o->img = img;
    luapp_img_ref(img);
    return 0;
}

/* c:sprites(img, {x1, y1, x2, y2, ...}): the image (at its scale, no
 * rotation) at up to 256 top-left positions, in one op. */
static int c_sprites(lua_State *L)
{
    canvas_t *c = &s_cv[luapp_canvas_arg(L, 1)];
    luapp_img_t *img = luapp_gfx_check_image(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    int len = (int)lua_rawlen(L, 3);
    if (len % 2 || len > 2 * BATCH_MAX) {
        return luaL_error(L, "c:sprites: positions are {x1, y1, x2, y2, ...}, %d at most", BATCH_MAX);
    }
    if (!len) return 0;
    int off = arena_take(L, c, len * 2, "c:sprites");
    int16_t *p = (int16_t *)(c->arena + off);
    for (int i = 0; i < len; i++) {
        lua_rawgeti(L, 3, i + 1);
        p[i] = coord(L, -1);
        lua_pop(L, 1);
    }
    cop_t *o = new_op(L, &c, OP_SPRITES);
    o->width = (int16_t)(len / 2), o->text = (uint16_t)off;
    o->img = img;
    luapp_img_ref(img);
    return 0;
}

/* c:partial(on): send the screen only what changed (see invalidate). */
static int c_partial(lua_State *L)
{
    canvas_t *c = &s_cv[luapp_canvas_arg(L, 1)];
    c->partial = lua_toboolean(L, 2);
    c->pvalid = false;   /* the next clear redraws it all */
    return 0;
}

static int c_size(lua_State *L)
{
    canvas_t *c = &s_cv[luapp_canvas_arg(L, 1)];
    lua_pushinteger(L, c->w);
    lua_pushinteger(L, c->h);
    return 2;
}

void luapp_gfx_icon_source(lua_State *L, int idx, luapp_pixels_t *out)
{
    ibox_t *b = luaL_testudata(L, idx, IMAGE_MT);
    if (b) {
        if (!b->img) luaL_error(L, "the image was freed");
        out->w = (uint16_t)b->img->dsc.header.w;
        out->h = (uint16_t)b->img->dsc.header.h;
        out->alpha = b->img->dsc.header.cf == LV_COLOR_FORMAT_RGB565A8;
        out->px = (uint8_t *)b->img->dsc.data;
        out->bytes = b->img->bytes;
        return;
    }
    if (!lua_isuserdata(L, idx)) luaL_error(L, "app.icon takes an image (gfx.sprite, gfx.image) or a canvas");
    canvas_t *c = &s_cv[luapp_canvas_arg(L, idx)];
    flush_one(c);   /* what it was told to draw so far */
    out->w = (uint16_t)c->w;
    out->h = (uint16_t)c->h;
    out->alpha = false;
    out->px = c->buf;
    out->bytes = c->bytes;
}

void luapp_gfx_open(lua_State *L)
{
    luaL_newmetatable(L, IMAGE_MT);
    lua_pushcfunction(L, img_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, img_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    static const luaL_Reg cm[] = {{"clear", c_clear}, {"rect", c_rect}, {"circle", c_circle}, {"line", c_line},
                                  {"triangle", c_triangle}, {"arc", c_arc}, {"text", c_text}, {"image", c_image},
                                  {"pixel", c_pixel}, {"size", c_size}, {"polygon", c_polygon},
                                  {"gradient", c_gradient}, {"tilemap", c_tilemap}, {"sprites", c_sprites},
                                  {"partial", c_partial}, {NULL, NULL}};
    luaL_setfuncs(L, cm, 0);   /* into the widget methods table on top */

    static const luaL_Reg g[] = {{"image", l_image}, {"sprite", l_sprite}, {"memory", l_memory}, {NULL, NULL}};
    luaL_newlib(L, g);
    luapp_perf_gfx_fields(L);   /* gfx.stats, gfx.show_stats */
    lua_setglobal(L, "gfx");
}
