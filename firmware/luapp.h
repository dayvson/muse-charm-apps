#pragma once

/*
 * The Lua app container: apps Muse writes in Lua, installed over Link without
 * a firmware update, run one at a time in the Apps tile. See luapp_vm.c (the
 * Lua task and the API), luapp_ui.c (launcher), luapp_store.c (storage) and
 * luapp_api.md (the reference Muse reads).
 */

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "lvgl.h"
#include "muse_apps.h"

/* luapp_vm.c */
void luapp_vm_start(void);
void luapp_vm_run(const char *id);
/* From LVGL callbacks: widget >= 0 a button, -1 the background (x, y from
 * the centre), -2 a long-press (leave the app). */
void luapp_vm_tap(int widget, uint32_t gen, int x, int y);
uint32_t luapp_vm_gen(void);
void luapp_set_reply(muse_apps_reply_fn fn);
cJSON *luapp_command(const char *command, const cJSON *params, const char *request_id, uint64_t generation);

/* luapp_ui.c (UI task to build; the rest under the display lock) */
void luapp_ui_build(lv_obj_t *tile, int w, int h);
lv_obj_t *luapp_ui_app_root(void);
void luapp_ui_show_app(void);
void luapp_ui_show_launcher(void);
void luapp_ui_show_error(const char *app_name, const char *error);
/* The launcher icon for an app: an icon file (luapp_image.h), copied, or
 * NULL to go back to its initials. false if it isn't one or there's no room. */
bool luapp_ui_set_icon(const char *id, const uint8_t *file);
