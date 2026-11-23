/* How fast the screen and the running Lua app go, over the last second:
 * gfx.stats(), gfx.show_stats(on) and luapp.admin action perf.
 *
 * The screen side comes from LVGL's render events (LVGL task): frames redrawn,
 * how long each took to render and send, and how much of the screen it was.
 * The app side from the Lua task: time in callbacks, and time drawing canvases. */
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "lvgl_private.h"   /* the display's invalid areas */
#include "luapp.h"
#include "luapp_priv.h"

#define WINDOW_US 1000000

typedef struct {
    uint32_t frames, app_frames, calls;
    int64_t frame_us, lua_us, draw_us;
    uint64_t px;
} acc_t;

typedef struct {
    float fps, frame_ms, kb, app_fps, draw_ms, lua_ms, calls, busy;
    bool valid;
} stats_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static acc_t s_acc;               /* under s_mux */
EXT_RAM_BSS_ATTR static stats_t s_last;            /* under s_mux */
EXT_RAM_BSS_ATTR static int64_t s_since;             /* Lua task */
EXT_RAM_BSS_ATTR static bool s_hooked;               /* Lua task */
EXT_RAM_BSS_ATTR static int64_t s_render_t0;         /* LVGL task */
EXT_RAM_BSS_ATTR static uint64_t s_render_px;        /* LVGL task */
EXT_RAM_BSS_ATTR static lv_obj_t *s_label;           /* the overlay; under the display lock */
EXT_RAM_BSS_ATTR static bool s_overlay;              /* Lua task */

static int64_t now_us(void) { return esp_timer_get_time(); }

static void on_display(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RENDER_START) {
        lv_display_t *d = lv_event_get_current_target(e);
        uint64_t px = 0;
        for (uint32_t i = 0; d && i < d->inv_p; i++) {
            if (!d->inv_area_joined[i]) px += (uint64_t)lv_area_get_size(&d->inv_areas[i]);
        }
        s_render_px = px;
        s_render_t0 = now_us();
    } else if (code == LV_EVENT_RENDER_READY && s_render_t0) {
        int64_t dt = now_us() - s_render_t0;
        s_render_t0 = 0;
        taskENTER_CRITICAL(&s_mux);
        s_acc.frames++;
        s_acc.frame_us += dt;
        s_acc.px += s_render_px;
        taskEXIT_CRITICAL(&s_mux);
    }
}

/* Under the display lock, once: from the Lua task when it starts. */
void luapp_perf_hook(void)
{
    if (s_hooked) return;
    lv_display_t *d = lv_display_get_default();
    if (!d) return;
    lv_display_add_event_cb(d, on_display, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(d, on_display, LV_EVENT_RENDER_READY, NULL);
    s_hooked = true;
    s_since = now_us();
}

void luapp_perf_lua(int64_t us)
{
    taskENTER_CRITICAL(&s_mux);
    s_acc.calls++;
    s_acc.lua_us += us;
    taskEXIT_CRITICAL(&s_mux);
}

void luapp_perf_draw(int64_t us, bool frame)
{
    taskENTER_CRITICAL(&s_mux);
    s_acc.draw_us += us;
    if (frame) s_acc.app_frames++;
    taskEXIT_CRITICAL(&s_mux);
}

static float per(int64_t total_us, uint32_t n) { return n ? (float)total_us / n / 1000.0f : 0; }

static void overlay_update(const stats_t *s)
{
    if (!s_label) return;
    char t[96];
    snprintf(t, sizeof(t), "%.0f fps  %.1f ms  %.0f KB\nLua %.1f ms  draw %.1f ms", s->fps, s->frame_ms, s->kb,
             s->lua_ms, s->draw_ms);
    lv_label_set_text(s_label, t);
    lv_obj_move_foreground(s_label);
}

/* From the Lua task's loop: closes the window each second. */
void luapp_perf_tick(void)
{
    if (!s_hooked) return;
    int64_t now = now_us(), span = now - s_since;
    if (span < WINDOW_US) return;
    s_since = now;
    taskENTER_CRITICAL(&s_mux);
    acc_t a = s_acc;
    memset(&s_acc, 0, sizeof(s_acc));
    float secs = span / 1e6f;
    s_last = (stats_t){
        .fps = a.frames / secs,
        .frame_ms = per(a.frame_us, a.frames),
        .kb = a.frames ? (float)a.px * 2 / 1024 / a.frames : 0,   /* RGB565 */
        .app_fps = a.app_frames / secs,
        .draw_ms = per(a.draw_us, a.app_frames),
        .lua_ms = per(a.lua_us, a.calls),
        .calls = a.calls / secs,
        .busy = (float)(a.lua_us + a.draw_us) * 100 / span,
        .valid = true,
    };
    stats_t s = s_last;
    taskEXIT_CRITICAL(&s_mux);
    if (s_overlay && luapp_lock()) {
        overlay_update(&s);
        luapp_unlock();
    }
}

static stats_t snapshot(void)
{
    taskENTER_CRITICAL(&s_mux);
    stats_t s = s_last;
    taskEXIT_CRITICAL(&s_mux);
    return s;
}

static float r1(float v) { return (float)(int)(v * 10 + (v >= 0 ? 0.5f : -0.5f)) / 10; }

#define FIELDS(X)                                                                                                     \
    X(fps) X(frame_ms) X(kb) X(app_fps) X(draw_ms) X(lua_ms) X(calls) X(busy)

cJSON *luapp_perf_json(void)
{
    stats_t s = snapshot();
    cJSON *o = cJSON_CreateObject();
#define ADD(f) cJSON_AddNumberToObject(o, #f, r1(s.f));
    FIELDS(ADD)
#undef ADD
    if (!s.valid) cJSON_AddStringToObject(o, "note", "no Lua app has run since boot");
    return o;
}

/* gfx.stats(): the same table, plus the app's Lua memory. */
static int l_stats(lua_State *L)
{
    stats_t s = snapshot();
    lua_createtable(L, 0, 9);
#define SET(f)                         \
    lua_pushnumber(L, r1(s.f));        \
    lua_setfield(L, -2, #f);
    FIELDS(SET)
#undef SET
    lua_pushinteger(L, lua_gc(L, LUA_GCCOUNT, 0));
    lua_setfield(L, -2, "lua_kb");
    return 1;
}

/* gfx.show_stats(on): the numbers at the top of the app's screen, each second. */
static int l_show_stats(lua_State *L)
{
    bool on = lua_toboolean(L, 1);
    if (on == s_overlay) return 0;
    s_overlay = on;
    if (!luapp_lock()) return 0;
    if (on && !s_label) {
        s_label = lv_label_create(luapp_ui_app_root());
        lv_obj_set_style_text_font(s_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(s_label, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_label, LV_OPA_70, 0);
        lv_obj_set_style_pad_hor(s_label, 8, 0);
        lv_obj_set_style_pad_ver(s_label, 3, 0);
        lv_obj_set_style_radius(s_label, 8, 0);
        lv_obj_remove_flag(s_label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_label, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
        lv_obj_align(s_label, LV_ALIGN_TOP_MID, 0, 36);
        stats_t s = snapshot();
        overlay_update(&s);
    } else if (!on && s_label) {
        lv_obj_delete(s_label);
        s_label = NULL;
    }
    luapp_unlock();
    return 0;
}

/* Adds stats and show_stats to the gfx table on top of the stack. */
void luapp_perf_gfx_fields(lua_State *L)
{
    static const luaL_Reg f[] = {{"stats", l_stats}, {"show_stats", l_show_stats}, {NULL, NULL}};
    luaL_setfuncs(L, f, 0);
}

/* The app stopped: its screen is about to be cleaned, the overlay with it. */
void luapp_perf_stop(void)
{
    s_overlay = false;
    s_label = NULL;
}
