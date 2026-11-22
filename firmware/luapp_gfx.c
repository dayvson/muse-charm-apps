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

enum { OP_CLEAR, OP_RECT, OP_CIRCLE, OP_LINE, OP_TRI, OP_ARC, OP_TEXT, OP_IMAGE };

typedef struct {
    uint8_t op, size, align, fill, opa;
    int16_t a, b, c, d, e, f;
    int16_t width, radius, scale, angle;
    uint16_t text;
    uint32_t color;
    luapp_img_t *img;
} cop_t;

typedef struct {
    bool used;
    lv_obj_t *obj;
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

static canvas_t s_cv[MAX_CANVAS];

static unsigned kb(size_t n) { return (unsigned)((n + 512) / 1024); }

/* Why a w x h canvas (n bytes) didn't fit, with the numbers an app (or Muse
 * writing it) needs to fix it. Call with the failed slot already cleared. */
static const char *canvas_no_room(int w, int h, size_t n)
{
    static char msg[224];
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

/* Under the lock. */
static void draw_ops(canvas_t *c)
{
    int from = 0;
    for (int i = c->nops - 1; i >= 0; i--) {
        if (c->ops[i].op == OP_CLEAR) {
            lv_canvas_fill_bg(c->obj, lv_color_hex(c->ops[i].color), LV_OPA_COVER);
            from = i + 1;
            break;
        }
    }
    if (from >= c->nops) {
        lv_obj_invalidate(c->obj);
        return;
    }
    lv_layer_t layer;
    lv_canvas_init_layer(c->obj, &layer);
    for (int i = from; i < c->nops; i++) {
        const cop_t *o = &c->ops[i];
        switch (o->op) {
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
    lv_canvas_finish_layer(c->obj, &layer);
}

static void flush_one(canvas_t *c)
{
    if (!c->nops) return;
    if (luapp_lock()) {
        draw_ops(c);
        luapp_unlock();
    }
    drop_ops(c);
}

void luapp_gfx_flush(void)
{
    for (int i = 0; i < MAX_CANVAS; i++) {
        if (s_cv[i].used) flush_one(&s_cv[i]);
    }
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
                                  {"pixel", c_pixel}, {"size", c_size}, {NULL, NULL}};
    luaL_setfuncs(L, cm, 0);   /* into the widget methods table on top */

    static const luaL_Reg g[] = {{"image", l_image}, {"sprite", l_sprite}, {"memory", l_memory}, {NULL, NULL}};
    luaL_newlib(L, g);
    lua_setglobal(L, "gfx");
}
