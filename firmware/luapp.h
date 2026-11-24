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
#include "luapp_store.h"   /* LUAPP_ID_MAX */

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
/* Icons are kept for a few pages only (the one showing and the next). The ids
 * on them that have no icon loaded yet, up to cap; the Lua task loads those. */
#define LUAPP_UI_WANT_MAX 12
int luapp_ui_icons_wanted(char ids[][LUAPP_ID_MAX + 1], int cap);
/* luapp_vm.c: load the launcher's wanted icons soon (any task). */
void luapp_vm_want_icons(void);
