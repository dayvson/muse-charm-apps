#pragma once

/*
 * Inside the Lua host: what luapp_vm.c (widgets, the task), luapp_gfx.c
 * (images and canvases) and luapp_sound.c share. Lua task only.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lauxlib.h"
#include "lua.h"
#include "luapp_image.h"
#include "lvgl.h"

/* The display lock (2 s timeout). */
bool luapp_lock(void);
void luapp_unlock(void);
const lv_font_t *luapp_font(int size);

/* ---- luapp_gfx.c ---- */

typedef struct luapp_img luapp_img_t;
struct luapp_img {
    lv_image_dsc_t dsc;
    int refs;             /* the Lua handle, widgets showing it, canvas ops drawing it */
    size_t bytes;
    uint8_t scale;        /* shown enlarged this many times (sprites keep 1x pixels) */
    luapp_img_t *next;    /* on the free list once refs reaches 0 */
};

/* gfx.*, the image metatable, and the canvas methods into the widget
 * methods table, which is on top of the stack. */
void luapp_gfx_open(lua_State *L);
luapp_img_t *luapp_gfx_check_image(lua_State *L, int idx);
void luapp_img_ref(luapp_img_t *img);
void luapp_img_unref(luapp_img_t *img);   /* freed by luapp_gfx_collect */

/* A canvas's drawing surface, allocated without the lock held (it may
 * collect garbage). Returns its slot, or -1 with *err. Then, under the
 * lock, attach it to its lv_canvas. */
int luapp_canvas_new(lua_State *L, int w, int h, uint32_t color, const char **err);
void luapp_canvas_attach(int slot, lv_obj_t *obj, int zoom);   /* zoom: shown 1-4x, crisp */
void luapp_canvas_set_color(int slot, uint32_t color);
/* After its object is deleted. */
void luapp_canvas_free(int slot);
/* Draws what the canvases recorded during a callback (takes the lock). */
void luapp_gfx_flush(void);
/* Frees images nothing uses any more (takes the lock if there are any). */
void luapp_gfx_collect(void);
/* The app stopped and its objects are gone: drop everything. */
void luapp_gfx_reset(void);
/* Images and canvases held now (bytes). */
size_t luapp_gfx_bytes(void);

/* app.icon's argument, an image or a canvas (drawn first): its pixels,
 * still owned by it. Raises if it's neither. */
void luapp_gfx_icon_source(lua_State *L, int idx, luapp_pixels_t *out);

/* ---- luapp_vm.c, for the canvas methods ---- */

/* The canvas slot of the widget at idx; raises if it isn't a canvas. */
int luapp_canvas_arg(lua_State *L, int idx);

/* ---- luapp_sound.c ---- */

void luapp_sound_open(lua_State *L);
/* The app started (true) or stopped (false): sound stops with it. */
void luapp_sound_enable(bool on);
