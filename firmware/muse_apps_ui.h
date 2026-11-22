#pragma once

#include "lvgl.h"

/* App tiles in screen order, leaving out Muse's: MUSE_APPS_LEFT of them left
 * of Muse (wind, stocks, then the watch, one swipe right of Muse), then the
 * Lua apps right of Muse, before settings (one swipe left of Muse).
 * Wind | Stocks | Watch | Muse | Apps | Settings. */
#define MUSE_APPS_COUNT 4
#define MUSE_APPS_LEFT 3

/* From the UI's build, under the display lock: fills the app tiles and
 * remembers the tileview so app.show can switch tiles. */
void muse_apps_build(lv_obj_t *tileview, lv_obj_t *const tiles[MUSE_APPS_COUNT], lv_obj_t *face_tile, int w, int h);
