#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "lvgl.h"
#include "muse_apps_ui.h"

/* PSRAM the apps never take: Muse's voice needs it (a voice note takes ~470 KB). */
#define MUSE_APPS_PSRAM_RESERVE (512 * 1024)

static inline cJSON *apps_ok(cJSON *payload)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    if (payload) cJSON_AddItemToObject(r, "payload", payload);
    return r;
}

static inline cJSON *apps_error(const char *code, const char *message)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    cJSON_AddItemToObject(r, "error", e);
    return r;
}

/* The app tiles in muse_apps_ui.h's order: swiping right from Muse reaches
 * the watch, then the stocks and the wind; swiping left, the Lua apps. */
enum { MUSE_APP_WIND, MUSE_APP_STOCKS, MUSE_APP_WATCH, MUSE_APP_LUAPPS, MUSE_APP_TILES };
_Static_assert(MUSE_APP_LUAPPS == MUSE_APPS_LEFT, "the Lua apps are the first tile right of Muse");

/* muse_apps.c: whether an app's tile is the one on screen (UI task), and
 * app.show's request to bring one up (any task). */
bool muse_apps_visible(int app);
void muse_apps_request_show(int app);
/* UI task: whether apps may fetch now. Wi-Fi up, the screen awake, no voice
 * call, and Muse's own session connected for a while: never while it's
 * (re)connecting, when internal RAM is tightest. */
bool muse_apps_net_ok(void);
/* Any task: whether an app's tile is the one on screen (updated 4 times a second). */
bool muse_apps_on_screen(int app);
/* Any task: the tile on screen, or the one app.show is about to bring up
 * (MUSE_APP_*, MUSE_APP_TILES for Muse's own, -1 for settings). */
int muse_apps_target(void);
/* Any task: whether an app's tile is (about to be) on screen or one swipe from
 * it. Tiles keep big buffers (charts) only while it is. */
bool muse_apps_near(int app);
/* Under the display lock: drop the big buffers of tiles that aren't near (a
 * Lua app is starting and wants the room now, not at their next tick). */
void muse_apps_trim(void);

/* watch_ui.c: the watch's local zone's UTC offset at a time (any task). */
int32_t watch_local_offset(int64_t utc);

/* wind_ui.c */
void wind_build(lv_obj_t *tile, int w, int h);
void wind_trim(void);   /* under the display lock */
cJSON *wind_command(const char *command, const cJSON *params);

/* stocks_ui.c */
void stocks_build(lv_obj_t *tile, int w, int h);
void stocks_trim(void);   /* under the display lock */
cJSON *stocks_command(const char *command, const cJSON *params);

/* watch_ui.c */
void watch_build(lv_obj_t *tile, int w, int h);
cJSON *watch_command(const char *command, const cJSON *params);
