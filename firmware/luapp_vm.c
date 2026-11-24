/*
 * The Lua app host: one task (64 KB PSRAM stack) owns the running app's Lua
 * state and everything that touches it. LVGL callbacks (taps), the apps'
 * fetch task (HTTP replies) and Muse's commands only post messages here.
 *
 * Limits keep a buggy app from hurting the gadget: 384 KB of Lua heap (in
 * PSRAM), 250 ms per callback (1 s for the script's first run), 96 widgets,
 * 24 timers, 4 queued HTTP requests of up to 32 KB each, 768 KB of images
 * and canvases (luapp_gfx.c). Any error stops the app, shows it on screen,
 * and is kept for luapp.admin logs. Sound: luapp_sound.c.
 *
 * Widget calls never raise a Lua error while holding the display lock:
 * arguments are read into a props_t first, then applied under the lock.
 */

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"

#include "lauxlib.h"
#include "lua.h"

#include "apps_net.h"
#include "luapp.h"
#include "luapp_priv.h"
#include "luapp_sandbox.h"
#include "luapp_store.h"
#include "muse_apps_priv.h"
#include "muse_board.h"
#include "muse_state.h"
#include "muse_tz.h"
#include "muse_voice.h"
#include "muse_wifi.h"

static const char *TAG = "luapp";

#define TASK_STACK (64 * 1024)
#define MEM_MAX (384 * 1024)
#define CHECK_MEM_MAX (256 * 1024)
#define CALL_MS 250
#define START_MS 1000
#define MAX_W 96
#define MAX_TIMERS 24
#define MAX_HTTP 4
#define MAX_POINTS 128
#define TEXT_MAX 512
#define URL_MAX 300
#define HTTP_BODY_MAX (32 * 1024)
#define LOG_MAX 3072
#define ERR_MAX 600
#define PIECE_MAX 8192
#define KV_FLUSH_US (10 * 1000000LL)
#define CLOCK_VALID 1704067200

typedef enum {
    M_INSTALL, M_RUN, M_STOP, M_REMOVE, M_SOURCE, M_TAP, M_HTTP, M_TOUCH, M_CHANGE, M_ANIM,
    M_SEND, M_SHOT, M_WIDGETS,
} mtype_t;

typedef struct {
    uint8_t type;
    char id[LUAPP_ID_MAX + 1];
    char name[LUAPP_NAME_MAX + 1];
    uint32_t color;
    bool has_name, has_color, append, done, run;
    char *data;   /* install piece or HTTP body: PSRAM, owned by the message */
    size_t len;
    int status;
    int widget;   /* -1: the background */
    uint8_t touch;   /* M_TOUCH: 0 down, 1 move, 2 up */
    uint32_t gen;
    int x, y;
    uint32_t http_id;
    size_t offset;
    bool has_req;
    char req_id[64];
    uint64_t sgen;
} msg_t;

typedef enum {
    W_LABEL, W_BOX, W_BUTTON, W_ARC, W_BAR, W_LINE, W_IMAGE, W_CANVAS,
    W_ROLLER, W_SLIDER, W_SWITCH, W_LIST, W_SCROLL,
} wkind_t;

typedef struct {
    lv_obj_t *obj, *lbl;
    lv_point_precise_t *pts;
    luapp_img_t *img;   /* W_IMAGE: the image shown (a reference) */
    int tap_ref, touch_ref;
    int change_ref;     /* roller, slider, switch: on_change; list: on_select */
    int anim_ref;       /* ui.animate's on_done */
    int8_t parent;      /* the scroll it's in, or -1 (the screen) */
    int16_t x, y;
    int16_t scale;      /* W_IMAGE: the app's scale in percent (0: 100) */
    int8_t canvas;      /* W_CANVAS: its slot in luapp_gfx.c */
    uint8_t kind;
    bool used;
} widget_t;

typedef struct {
    int id, ref, period_ms;
    int64_t due_us;
} vtimer_t;

typedef struct {
    uint32_t id;
    int ref;
    char url[URL_MAX + 1];
    char method[8];
    char *headers, *body;   /* PSRAM or NULL, owned here until the request goes */
    size_t body_len;
} vhttp_t;

typedef struct {
    lua_State *L;
    char id[LUAPP_ID_MAX + 1];
    char name[LUAPP_NAME_MAX + 1];
    uint32_t gen;               /* bumps per run: drops taps and replies meant for an earlier one */
    widget_t w[MAX_W];
    vtimer_t t[MAX_TIMERS];
    int next_timer;
    int bg_ref;
    int kv_ref;
    bool kv_dirty;
    int64_t kv_due;
    bool exit_req;
    int64_t deadline;
    int limit_ms;
    size_t mem, check_mem;
    vhttp_t http[MAX_HTTP];
    int http_n;
    bool http_busy;
    uint32_t http_seq;
    int icon_calls;
    int64_t away_since;         /* off the Apps screen since (0: on it) */
    int swipe_ref;              /* ui.on_swipe */
    int64_t timers_ok_at;       /* frame pacing: no timers before this */
} vm_t;

EXT_RAM_BSS_ATTR static vm_t V;

/* Stopping jumps the queue: the long-press (UI task) or Muse's stop sets this,
 * the Lua hook ends a running callback at once, and the task stops the app
 * before anything else. */
enum { STOP_NONE, STOP_LONG_PRESS, STOP_MUSE, STOP_REPLACED };
static const char *const STOP_WHY[] = {NULL, "long-press", "Muse asked", "replaced"};
static atomic_int s_stop_req;
static atomic_uint s_live_gen;   /* V.gen, for the fetch task: a request for a stopped app isn't made */

/* The last unload, for luapp.admin list (under s_log_mux). */
typedef struct {
    char id[LUAPP_ID_MAX + 1];
    char why[24];
    unsigned lua_kb, gfx_kb, freed_kb, leftover_kb, psram_kb, largest_kb;
} last_stop_t;
EXT_RAM_BSS_ATTR static last_stop_t s_last_stop;

/* Muse's luapp.admin stop, answered once the app is unloaded (stop_app), or
 * after STOP_WAIT_MS by the watcher: an error, or with force a reboot. */
#define STOP_WAIT_MS 3000
typedef struct {
    bool pending, force;
    char req_id[64];
    uint64_t sgen;
    char id[LUAPP_ID_MAX + 1];
} pend_stop_t;
EXT_RAM_BSS_ATTR static pend_stop_t s_pend_stop;   /* under s_log_mux */

/* A forced stop's reboot leaves the app's id here (RTC memory survives a
 * software reset); the next boot reports it in luapp.admin list. */
#define FORCED_MAGIC 0x4c464f52u
typedef struct {
    uint32_t magic;
    char id[LUAPP_ID_MAX + 1];
} forced_t;
RTC_NOINIT_ATTR static forced_t s_forced_rtc;
static char s_forced_id[LUAPP_ID_MAX + 1];   /* this boot followed a forced stop of it */

#define MSG_BURST 8                    /* messages handled before each round of timers */
#define MIN_IDLE_US 5000               /* frame pacing: at least this between rounds... */
#define AWAY_US (30 * 1000000LL)       /* unload an app this long off the Apps screen */
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static muse_apps_reply_fn s_reply;

/* Logs of the last app run, for luapp.admin logs (any task, under s_log_mux). */
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static char s_log[LOG_MAX + 1];
EXT_RAM_BSS_ATTR static char s_error[ERR_MAX + 1];
static size_t s_log_len;
static char s_log_id[LUAPP_ID_MAX + 1];
static char s_running[LUAPP_ID_MAX + 1];   /* "" when none */

/* The HTTP request the fetch task is on (under s_log_mux too: tiny). */
EXT_RAM_BSS_ATTR static char s_http_url[URL_MAX + 1];
static char s_http_method[8];
static char *s_http_headers, *s_http_body;   /* handed to the fetch task with the URL */
static size_t s_http_body_len;
static uint32_t s_http_gen;

static int64_t now_us(void) { return esp_timer_get_time(); }

/* ---- logs ---- */

static void log_append(const char *s, size_t n)
{
    taskENTER_CRITICAL(&s_log_mux);
    if (n > LOG_MAX) {
        s += n - LOG_MAX;
        n = LOG_MAX;
    }
    if (s_log_len + n > LOG_MAX) {
        size_t drop = s_log_len + n - LOG_MAX;
        memmove(s_log, s_log + drop, s_log_len - drop);
        s_log_len -= drop;
    }
    memcpy(s_log + s_log_len, s, n);
    s_log_len += n;
    s_log[s_log_len] = 0;
    taskEXIT_CRITICAL(&s_log_mux);
}

static void set_error(const char *msg)
{
    taskENTER_CRITICAL(&s_log_mux);
    strlcpy(s_error, msg ? msg : "", sizeof(s_error));
    taskEXIT_CRITICAL(&s_log_mux);
}

static void start_logs(const char *id)
{
    taskENTER_CRITICAL(&s_log_mux);
    s_log_len = 0;
    s_log[0] = 0;
    s_error[0] = 0;
    strlcpy(s_log_id, id, sizeof(s_log_id));
    taskEXIT_CRITICAL(&s_log_mux);
}

static void set_running(const char *id)
{
    taskENTER_CRITICAL(&s_log_mux);
    strlcpy(s_running, id ? id : "", sizeof(s_running));
    taskEXIT_CRITICAL(&s_log_mux);
}

/* ---- replies ---- */

static void reply(const msg_t *m, cJSON *result)
{
    if (m->has_req && m->sgen == 0 && !strcmp(m->req_id, "console")) {
        /* The USB console's request (muse_apps_console): it has no session, so
         * the result is printed like the console's other answers. */
        char *text = result ? cJSON_PrintUnformatted(result) : NULL;
        printf("@apps %s\n", text ? text : "{}");
        fflush(stdout);
        cJSON_free(text);
        cJSON_Delete(result);
        return;
    }
    if (m->has_req && s_reply) {
        s_reply(m->sgen, m->req_id, result);
    } else {
        cJSON_Delete(result);
    }
}

/* ---- memory and time limits ---- */

static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    size_t *used = ud, cap = used == &V.mem ? MEM_MAX : CHECK_MEM_MAX;
    if (!ptr) osize = 0;
    if (nsize == 0) {
        heap_caps_free(ptr);
        *used -= osize;
        return NULL;
    }
    if (*used - osize + nsize > cap) return NULL;
    void *q = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (q) *used = *used - osize + nsize;
    return q;
}

static void hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (atomic_load(&s_stop_req)) luaL_error(L, "stopped");   /* call() unloads it quietly */
    if (now_us() > V.deadline) luaL_error(L, "the app ran over its %d ms limit", V.limit_ms);
}

static int msgh(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(error object)", 1);
    return 1;
}

static void stop_app(const char *error, const char *why);

/* Calls the function under its nargs arguments; false (and the app stopped)
 * on error. With nres > 0 the results stay on the stack and app.exit waits
 * for after_call(). */
static bool call_n(int nargs, int nres, int limit_ms);
static bool call(int nargs, int limit_ms) { return call_n(nargs, 0, limit_ms); }

static void after_call(void)
{
    luapp_gfx_flush();   /* draw what the canvases recorded */
    if (V.exit_req) stop_app(NULL, "app.exit");
}

static bool call_n(int nargs, int nres, int limit_ms)
{
    lua_State *L = V.L;
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, msgh);
    lua_insert(L, base);
    V.limit_ms = limit_ms;
    V.deadline = now_us() + (int64_t)limit_ms * 1000;
    int64_t t0 = now_us();
    int rc = lua_pcall(L, nargs, nres, base);
    luapp_perf_lua(now_us() - t0);
    lua_remove(L, base);
    if (rc != LUA_OK) {
        int req = atomic_exchange(&s_stop_req, STOP_NONE);
        if (req) {   /* a long-press or Muse's stop cut it short: not an error */
            lua_pop(L, 1);
            stop_app(NULL, STOP_WHY[req]);
            return false;
        }
        char err[ERR_MAX + 1];
        strlcpy(err, rc == LUA_ERRMEM ? "out of memory (384 KB)" : lua_tostring(L, -1), sizeof(err));
        lua_pop(L, 1);
        stop_app(err, "error");
        return false;
    }
    if (nres == 0) after_call();
    return true;
}

/* ---- the screen ---- */

bool luapp_lock(void) { return muse_board->display_lock(2000); }
void luapp_unlock(void) { muse_board->display_unlock(); }
static bool lock(void) { return luapp_lock(); }
static void unlock(void) { luapp_unlock(); }

const lv_font_t *luapp_font(int size)
{
    if (size >= 40) {
#if CONFIG_LV_FONT_MONTSERRAT_48
        return &lv_font_montserrat_48;
#else
        return &lv_font_montserrat_28;
#endif
    }
    if (size >= 24) return &lv_font_montserrat_28;
    if (size >= 18) return &lv_font_montserrat_20;
    if (size >= 15) return &lv_font_montserrat_16;
    return &lv_font_montserrat_14;
}

enum {
    P_X = 1 << 0, P_Y = 1 << 1, P_W = 1 << 2, P_H = 1 << 3, P_TEXT = 1 << 4, P_SIZE = 1 << 5, P_COLOR = 1 << 6,
    P_BG = 1 << 7, P_RADIUS = 1 << 8, P_BORDER = 1 << 9, P_BORDER_COLOR = 1 << 10, P_OPACITY = 1 << 11,
    P_ALIGN = 1 << 12, P_VALUE = 1 << 13, P_MIN = 1 << 14, P_MAX = 1 << 15, P_R = 1 << 16, P_WIDTH = 1 << 17,
    P_START = 1 << 18, P_SWEEP = 1 << 19, P_POINTS = 1 << 20, P_TEXT_COLOR = 1 << 21, P_TAP = 1 << 22,
    P_IMG = 1 << 23, P_SCALE = 1 << 24, P_ANGLE = 1 << 25, P_TOUCH = 1 << 26, P_ZOOM = 1 << 27,
};
#define P_OPTIONS (1ULL << 28)   /* roller options, list items */
#define P_CHANGE (1ULL << 29)    /* on_change / on_select */
#define P_PARENT (1ULL << 30)    /* in a ui.scroll */
#define P_ROWS (1ULL << 31)      /* roller: visible rows */
#define OPTS_MAX 4096            /* joined options or items */

typedef struct {
    uint64_t has;
    int x, y, w, h, size, radius, border, opacity, align, r, width, start, sweep;
    uint32_t color, bg, border_color, text_color;
    int value, min, max;
    int scale, angle, zoom, rows;
    int parent;                /* widget index of the scroll it's in */
    char *opts;                /* PSRAM, "\n"-joined; freed by the caller */
    int nopts;
    luapp_img_t *img;          /* held by the arguments; referenced when applied */
    char text[TEXT_MAX + 1];
    lv_point_precise_t *pts;   /* PSRAM; ownership passes to the widget */
    int npts;
} props_t;

static int int_field(lua_State *L, int t, const char *k, uint64_t bit, uint64_t *has, int lo, int hi)
{
    int v = 0;
    if (lua_getfield(L, t, k) != LUA_TNIL) {
        if (!lua_isnumber(L, -1)) luaL_error(L, "%s must be a number", k);
        lua_Number n = lua_tonumber(L, -1);
        v = (int)(n < lo ? lo : n > hi ? hi : n);
        *has |= bit;
    }
    lua_pop(L, 1);
    return v;
}

static int parent_arg(lua_State *L, int i);

static void read_props(lua_State *L, int t, props_t *p)
{
    luaL_checktype(L, t, LUA_TTABLE);
    memset(p, 0, sizeof(*p));
    p->x = int_field(L, t, "x", P_X, &p->has, -1000, 1000);
    p->y = int_field(L, t, "y", P_Y, &p->has, -1000, 1000);
    p->w = int_field(L, t, "w", P_W, &p->has, 0, 466);
    p->h = int_field(L, t, "h", P_H, &p->has, 0, 466);
    p->size = int_field(L, t, "size", P_SIZE, &p->has, 8, 64);
    p->radius = int_field(L, t, "radius", P_RADIUS, &p->has, 0, 1000);
    p->border = int_field(L, t, "border", P_BORDER, &p->has, 0, 40);
    p->opacity = int_field(L, t, "opacity", P_OPACITY, &p->has, 0, 255);
    p->r = int_field(L, t, "r", P_R, &p->has, 1, 233);
    p->width = int_field(L, t, "width", P_WIDTH, &p->has, 1, 100);
    p->start = int_field(L, t, "start", P_START, &p->has, -720, 720);
    p->sweep = int_field(L, t, "sweep", P_SWEEP, &p->has, 1, 360);
    p->value = int_field(L, t, "value", P_VALUE, &p->has, -1000000, 1000000);
    p->min = int_field(L, t, "min", P_MIN, &p->has, -1000000, 1000000);
    p->max = int_field(L, t, "max", P_MAX, &p->has, -1000000, 1000000);
    p->color = (uint32_t)int_field(L, t, "color", P_COLOR, &p->has, 0, 0xffffff);
    p->bg = (uint32_t)int_field(L, t, "bg", P_BG, &p->has, 0, 0xffffff);
    p->border_color = (uint32_t)int_field(L, t, "border_color", P_BORDER_COLOR, &p->has, 0, 0xffffff);
    p->text_color = (uint32_t)int_field(L, t, "text_color", P_TEXT_COLOR, &p->has, 0, 0xffffff);
    p->scale = int_field(L, t, "scale", P_SCALE, &p->has, 1, 1600);
    p->angle = int_field(L, t, "angle", P_ANGLE, &p->has, -3600, 3600);
    p->zoom = int_field(L, t, "zoom", P_ZOOM, &p->has, 1, 4);
    if (lua_getfield(L, t, "img") != LUA_TNIL) {
        p->img = luapp_gfx_check_image(L, -1);
        p->has |= P_IMG;
    }
    lua_pop(L, 1);
    if (lua_getfield(L, t, "on_touch") != LUA_TNIL) {
        luaL_checktype(L, -1, LUA_TFUNCTION);
        p->has |= P_TOUCH;
    }
    lua_pop(L, 1);
    if (lua_getfield(L, t, "text") != LUA_TNIL) {
        size_t n;
        const char *s = luaL_tolstring(L, -1, &n);
        strlcpy(p->text, s, sizeof(p->text));
        lua_pop(L, 1);
        p->has |= P_TEXT;
    }
    lua_pop(L, 1);
    if (lua_getfield(L, t, "align") != LUA_TNIL) {
        const char *a = luaL_checkstring(L, -1);
        p->align = !strcmp(a, "left") ? 1 : !strcmp(a, "right") ? 2 : 0;
        p->has |= P_ALIGN;
    }
    lua_pop(L, 1);
    if (lua_getfield(L, t, "on_tap") != LUA_TNIL) {
        luaL_checktype(L, -1, LUA_TFUNCTION);
        p->has |= P_TAP;
    }
    lua_pop(L, 1);
    p->rows = int_field(L, t, "rows", P_ROWS, &p->has, 1, 9);
    if (lua_getfield(L, t, "on_change") != LUA_TNIL || (lua_pop(L, 1), lua_getfield(L, t, "on_select")) != LUA_TNIL) {
        luaL_checktype(L, -1, LUA_TFUNCTION);
        p->has |= P_CHANGE;
    }
    lua_pop(L, 1);
    if (lua_getfield(L, t, "parent") != LUA_TNIL) {
        p->parent = parent_arg(L, -1);
        p->has |= P_PARENT;
    }
    lua_pop(L, 1);
    /* Allocated last (with the points): nothing below can raise once they're held. */
    const char *okey = lua_getfield(L, t, "options") != LUA_TNIL ? "options" : (lua_pop(L, 1), lua_getfield(L, t, "items")) != LUA_TNIL ? "items" : NULL;
    if (okey) {
        luaL_checktype(L, -1, LUA_TTABLE);
        int n = (int)lua_rawlen(L, -1);
        if (n < 1 || n > 100) luaL_error(L, "%s: 1 to 100 strings", okey);
        size_t total = 0;
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);
            size_t len;
            if (!lua_isstring(L, -1)) luaL_error(L, "%s must be strings", okey);
            lua_tolstring(L, -1, &len);
            total += (len > 64 ? 64 : len) + 1;
            lua_pop(L, 1);
        }
        if (total > OPTS_MAX) luaL_error(L, "%s: too much text (%d bytes at most)", okey, OPTS_MAX);
        p->opts = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p->opts) luaL_error(L, "out of memory");
        size_t o = 0;
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);
            size_t len;
            const char *s = lua_tolstring(L, -1, &len);
            if (len > 64) len = 64;
            for (size_t k = 0; k < len; k++) p->opts[o++] = s[k] == '\n' ? ' ' : s[k];
            p->opts[o++] = i < n ? '\n' : 0;
            lua_pop(L, 1);
        }
        p->opts[o] = 0;
        p->nopts = n;
        p->has |= P_OPTIONS;
    }
    lua_pop(L, 1);
    /* Points last: nothing below can raise an error once they're allocated. */
    if (lua_getfield(L, t, "points") != LUA_TNIL) {
        luaL_checktype(L, -1, LUA_TTABLE);
        int n = (int)lua_rawlen(L, -1);
        if (n < 4 || n % 2 || n / 2 > MAX_POINTS) {
            heap_caps_free(p->opts);
            luaL_error(L, "points: 2 to %d x,y pairs", MAX_POINTS);
        }
        double xy[2 * MAX_POINTS];
        for (int i = 0; i < n; i++) {
            lua_rawgeti(L, -1, i + 1);
            if (!lua_isnumber(L, -1)) {
                heap_caps_free(p->opts);
                luaL_error(L, "points must be numbers");
            }
            xy[i] = lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        p->pts = heap_caps_malloc(sizeof(lv_point_precise_t) * (n / 2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p->pts) {
            heap_caps_free(p->opts);
            luaL_error(L, "out of memory");
        }
        for (int i = 0; i < n / 2; i++) {
            p->pts[i].x = (int32_t)lround(xy[2 * i]) + 233;   /* centre-relative to the tile's corner */
            p->pts[i].y = (int32_t)lround(xy[2 * i + 1]) + 233;
        }
        p->npts = n / 2;
        p->has |= P_POINTS;
    }
    lua_pop(L, 1);
}

static void on_button(lv_event_t *e)
{
    uintptr_t v = (uintptr_t)lv_event_get_user_data(e);
    luapp_vm_tap((int)(v & 0x7f), (uint32_t)(v >> 8), 0, 0);
}

static msg_t *new_msg(uint8_t type);
static bool post(msg_t *m);

/* A roller, slider or switch changed, or a list item was tapped (LVGL task).
 * Coalesced like drags: the latest value waits, one message at a time. */
static atomic_int s_chg_val[MAX_W];
static atomic_bool s_chg_pending[MAX_W];

static void on_change(lv_event_t *e)
{
    uintptr_t v = (uintptr_t)lv_event_get_user_data(e);
    int idx = (int)(v & 0x7f);
    lv_obj_t *o = lv_event_get_current_target(e);
    int val;
    if (lv_obj_check_type(o, &lv_roller_class)) val = (int)lv_roller_get_selected(o) + 1;
    else if (lv_obj_check_type(o, &lv_slider_class)) val = (int)lv_slider_get_value(o);
    else if (lv_obj_check_type(o, &lv_switch_class)) val = lv_obj_has_state(o, LV_STATE_CHECKED);
    else val = (int)(intptr_t)lv_obj_get_user_data(o);   /* a list item: its number */
    atomic_store(&s_chg_val[idx], val);
    if (atomic_exchange(&s_chg_pending[idx], true)) return;
    msg_t *m = new_msg(M_CHANGE);
    if (!m) {
        atomic_store(&s_chg_pending[idx], false);
        return;
    }
    m->widget = idx;
    m->gen = (uint32_t)(v >> 8) & 0xffffff;
    if (!post(m)) atomic_store(&s_chg_pending[idx], false);
}

/* ui.animate's on_done (LVGL task). */
static void on_anim_done(lv_anim_t *a)
{
    uintptr_t v = (uintptr_t)lv_anim_get_user_data(a);
    msg_t *m = new_msg(M_ANIM);
    if (!m) return;
    m->widget = (int)(v & 0x7f);
    m->gen = (uint32_t)(v >> 8) & 0xffffff;
    post(m);
}

/* Canvas touches (LVGL task). A drag's moves are coalesced: one message in
 * the queue at a time, carrying the latest point. */
static atomic_int s_mv_x, s_mv_y;
static atomic_bool s_mv_queued;
static uint32_t s_press_tick;
static lv_point_t s_press_pt, s_last_pt;
static bool s_press_exited;

static void post_touch(int widget, uint32_t gen, int kind, int x, int y)
{
    msg_t *m = new_msg(M_TOUCH);
    if (!m) {
        if (kind == 1) atomic_store(&s_mv_queued, false);
        return;
    }
    m->widget = widget;
    m->gen = gen & 0xffffff;
    m->touch = (uint8_t)kind;
    m->x = x;
    m->y = y;
    if (!post(m) && kind == 1) atomic_store(&s_mv_queued, false);
}

static void on_canvas(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    uintptr_t v = (uintptr_t)lv_event_get_user_data(e);
    int idx = (int)(v & 0x7f);
    bool touch = (v & 0x80) != 0;
    uint32_t gen = (uint32_t)(v >> 8);
    lv_obj_t *o = lv_event_get_current_target(e);
    lv_point_t p = {0, 0};
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_get_point(in, &p);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    /* In canvas pixels: a zoomed canvas is shown scale/256 times its size. */
    int32_t sc = lv_image_get_scale_x(o);
    if (sc < 256) sc = 256;
    int x = (int)((p.x - a.x1) * 256 / sc), y = (int)((p.y - a.y1) * 256 / sc);
    switch (code) {
    case LV_EVENT_PRESSED:
        s_press_tick = lv_tick_get();
        s_press_pt = s_last_pt = p;
        s_press_exited = false;
        if (touch) post_touch(idx, gen, 0, x, y);
        break;
    case LV_EVENT_PRESSING:
        if (!touch) break;
        if (p.x != s_last_pt.x || p.y != s_last_pt.y) {   /* a still finger isn't a move */
            s_last_pt = p;
            atomic_store(&s_mv_x, x);
            atomic_store(&s_mv_y, y);
            if (!atomic_exchange(&s_mv_queued, true)) post_touch(idx, gen, 1, x, y);
        }
        /* Drags don't swipe away from a touch canvas, so leaving is a still
         * hold of 1.5 s. */
        if (!s_press_exited && lv_tick_elaps(s_press_tick) >= 1500 && LV_ABS(p.x - s_press_pt.x) < 20 &&
            LV_ABS(p.y - s_press_pt.y) < 20) {
            s_press_exited = true;
            luapp_vm_tap(-2, gen, 0, 0);
        }
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (touch) post_touch(idx, gen, 2, x, y);
        break;
    case LV_EVENT_SHORT_CLICKED:
        luapp_vm_tap(idx, gen, x, y);
        break;
    default:
        break;
    }
}

/* Under the display lock; no Lua calls. */
static void apply(widget_t *w, props_t *p, bool create)
{
    lv_obj_t *o = w->obj;
    if (p->has & (P_X | P_Y)) {
        if (p->has & P_X) w->x = (int16_t)p->x;
        if (p->has & P_Y) w->y = (int16_t)p->y;
        if (w->kind != W_LINE) lv_obj_align(o, LV_ALIGN_CENTER, w->x, w->y);
    }
    switch (w->kind) {
    case W_LABEL:
        if (p->has & P_TEXT) lv_label_set_text(o, p->text);
        if ((p->has & P_SIZE) || create) lv_obj_set_style_text_font(o, luapp_font((p->has & P_SIZE) ? p->size : 16), 0);
        if ((p->has & P_COLOR) || create) lv_obj_set_style_text_color(o, lv_color_hex((p->has & P_COLOR) ? p->color : 0xffffff), 0);
        if (p->has & P_W) {
            lv_obj_set_width(o, p->w);
            lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_WRAP);
        }
        if ((p->has & P_ALIGN) || create) {
            static const lv_text_align_t A[] = {LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_RIGHT};
            lv_obj_set_style_text_align(o, A[p->align], 0);
        }
        break;
    case W_BOX:
    case W_BUTTON:
        if (p->has & (P_W | P_H)) {
            lv_obj_set_size(o, (p->has & P_W) ? p->w : lv_obj_get_width(o), (p->has & P_H) ? p->h : lv_obj_get_height(o));
        }
        if ((p->has & P_COLOR) || create) {
            lv_obj_set_style_bg_color(o, lv_color_hex((p->has & P_COLOR) ? p->color : (w->kind == W_BUTTON ? 0x2c2c2e : 0x1c1c1e)), 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        }
        if (p->has & P_OPACITY) lv_obj_set_style_bg_opa(o, (lv_opa_t)p->opacity, 0);
        if (p->has & P_RADIUS) lv_obj_set_style_radius(o, p->radius, 0);
        else if (create && w->kind == W_BUTTON) lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        if (p->has & P_BORDER) lv_obj_set_style_border_width(o, p->border, 0);
        if (p->has & P_BORDER_COLOR) lv_obj_set_style_border_color(o, lv_color_hex(p->border_color), 0);
        if (w->kind == W_BUTTON) {
            if (p->has & P_TEXT) lv_label_set_text(w->lbl, p->text);
            if ((p->has & P_SIZE) || create) lv_obj_set_style_text_font(w->lbl, luapp_font((p->has & P_SIZE) ? p->size : 16), 0);
            if ((p->has & P_TEXT_COLOR) || create)
                lv_obj_set_style_text_color(w->lbl, lv_color_hex((p->has & P_TEXT_COLOR) ? p->text_color : 0xffffff), 0);
        }
        break;
    case W_ARC:
        if (p->has & P_R) lv_obj_set_size(o, 2 * p->r, 2 * p->r);
        if ((p->has & P_WIDTH) || create) {
            int wd = (p->has & P_WIDTH) ? p->width : 8;
            lv_obj_set_style_arc_width(o, wd, LV_PART_MAIN);
            lv_obj_set_style_arc_width(o, wd, LV_PART_INDICATOR);
        }
        if ((p->has & P_COLOR) || create) lv_obj_set_style_arc_color(o, lv_color_hex((p->has & P_COLOR) ? p->color : 0x27d3a2), LV_PART_INDICATOR);
        if ((p->has & P_BG) || create) lv_obj_set_style_arc_color(o, lv_color_hex((p->has & P_BG) ? p->bg : 0x2c2c2e), LV_PART_MAIN);
        if ((p->has & (P_START | P_SWEEP)) || create) {
            lv_arc_set_rotation(o, (((p->has & P_START) ? p->start : 0) + 270 + 720) % 360);
            lv_arc_set_bg_angles(o, 0, (p->has & P_SWEEP) ? p->sweep : 360);
        }
        if ((p->has & (P_MIN | P_MAX)) || create) {
            lv_arc_set_range(o, (p->has & P_MIN) ? p->min : 0, (p->has & P_MAX) ? p->max : 100);
        }
        if ((p->has & P_VALUE) || create) lv_arc_set_value(o, (p->has & P_VALUE) ? p->value : 0);
        break;
    case W_BAR:
        if (p->has & (P_W | P_H)) {
            lv_obj_set_size(o, (p->has & P_W) ? p->w : lv_obj_get_width(o), (p->has & P_H) ? p->h : lv_obj_get_height(o));
        }
        if ((p->has & P_COLOR) || create) lv_obj_set_style_bg_color(o, lv_color_hex((p->has & P_COLOR) ? p->color : 0x27d3a2), LV_PART_INDICATOR);
        if ((p->has & P_BG) || create) {
            lv_obj_set_style_bg_color(o, lv_color_hex((p->has & P_BG) ? p->bg : 0x2c2c2e), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
        }
        if (p->has & P_RADIUS) {
            lv_obj_set_style_radius(o, p->radius, LV_PART_MAIN);
            lv_obj_set_style_radius(o, p->radius, LV_PART_INDICATOR);
        }
        if ((p->has & (P_MIN | P_MAX)) || create) lv_bar_set_range(o, (p->has & P_MIN) ? p->min : 0, (p->has & P_MAX) ? p->max : 100);
        if ((p->has & P_VALUE) || create) lv_bar_set_value(o, (p->has & P_VALUE) ? p->value : 0, LV_ANIM_OFF);
        break;
    case W_LINE:
        if (p->has & P_POINTS) {
            lv_line_set_points(o, p->pts, (uint32_t)p->npts);
            heap_caps_free(w->pts);   /* the line keeps a pointer to its points */
            w->pts = p->pts;
            p->pts = NULL;
        }
        if ((p->has & P_COLOR) || create) lv_obj_set_style_line_color(o, lv_color_hex((p->has & P_COLOR) ? p->color : 0xffffff), 0);
        if ((p->has & P_WIDTH) || create) {
            lv_obj_set_style_line_width(o, (p->has & P_WIDTH) ? p->width : 2, 0);
            lv_obj_set_style_line_rounded(o, true, 0);
        }
        break;
    case W_IMAGE:
        if (p->has & P_IMG) {
            lv_image_set_src(o, &p->img->dsc);
            lv_image_set_pivot(o, p->img->dsc.header.w / 2, p->img->dsc.header.h / 2);
            luapp_img_ref(p->img);
            luapp_img_unref(w->img);   /* freed after the callback, once nothing shows it */
            w->img = p->img;
        }
        if (p->has & P_SCALE) w->scale = (int16_t)p->scale;
        if ((p->has & (P_IMG | P_SCALE)) && w->img) {   /* sprites keep 1x pixels: enlarge them here */
            uint32_t s = (uint32_t)(w->scale ? w->scale : 100) * 256 * w->img->scale / 100;
            lv_image_set_scale(o, s > 65535 ? 65535 : s);
        }
        if ((p->has & P_ANGLE) || create) {
            int a = (p->has & P_ANGLE) ? p->angle : 0;
            lv_image_set_rotation(o, a * 10);
            lv_image_set_antialias(o, a != 0);
        }
        if (p->has & P_OPACITY) lv_obj_set_style_image_opa(o, (lv_opa_t)p->opacity, 0);
        break;
    case W_CANVAS:
        if (p->has & P_COLOR) luapp_canvas_set_color(w->canvas, p->color);
        break;
    case W_ROLLER:
        if (p->has & P_OPTIONS) lv_roller_set_options(o, p->opts, LV_ROLLER_MODE_NORMAL);
        if ((p->has & P_ROWS) || create) lv_roller_set_visible_row_count(o, (p->has & P_ROWS) ? (uint32_t)p->rows : 3);
        if (p->has & P_W) lv_obj_set_width(o, p->w);
        if ((p->has & P_SIZE) || create) {
            const lv_font_t *f = luapp_font((p->has & P_SIZE) ? p->size : 20);
            lv_obj_set_style_text_font(o, f, LV_PART_MAIN);
            lv_obj_set_style_text_font(o, f, LV_PART_SELECTED);
        }
        if ((p->has & P_BG) || create) {
            lv_obj_set_style_bg_color(o, lv_color_hex((p->has & P_BG) ? p->bg : 0x1c1c1e), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
        }
        if ((p->has & P_TEXT_COLOR) || create) {
            lv_obj_set_style_text_color(o, lv_color_hex((p->has & P_TEXT_COLOR) ? p->text_color : 0x8e8e93), LV_PART_MAIN);
        }
        if ((p->has & P_COLOR) || create) {
            lv_obj_set_style_bg_color(o, lv_color_hex((p->has & P_COLOR) ? p->color : 0x27d3a2), LV_PART_SELECTED);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_SELECTED);
            lv_obj_set_style_text_color(o, lv_color_black(), LV_PART_SELECTED);
        }
        if (p->has & P_RADIUS) lv_obj_set_style_radius(o, p->radius, LV_PART_MAIN);
        if (p->has & P_VALUE) lv_roller_set_selected(o, p->value > 0 ? (uint32_t)p->value - 1 : 0, create ? LV_ANIM_OFF : LV_ANIM_ON);
        break;
    case W_SLIDER:
        if ((p->has & (P_W | P_H)) || create) {
            lv_obj_set_size(o, (p->has & P_W) ? p->w : (create ? 240 : lv_obj_get_width(o)),
                            (p->has & P_H) ? p->h : (create ? 10 : lv_obj_get_height(o)));
        }
        if ((p->has & (P_MIN | P_MAX)) || create) lv_slider_set_range(o, (p->has & P_MIN) ? p->min : 0, (p->has & P_MAX) ? p->max : 100);
        if ((p->has & P_VALUE) || create) lv_slider_set_value(o, (p->has & P_VALUE) ? p->value : 0, LV_ANIM_OFF);
        if ((p->has & P_COLOR) || create) {
            lv_color_t c = lv_color_hex((p->has & P_COLOR) ? p->color : 0x27d3a2);
            lv_obj_set_style_bg_color(o, c, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(o, lv_color_white(), LV_PART_KNOB);
        }
        if ((p->has & P_BG) || create) lv_obj_set_style_bg_color(o, lv_color_hex((p->has & P_BG) ? p->bg : 0x2c2c2e), LV_PART_MAIN);
        break;
    case W_SWITCH:
        if (p->has & (P_W | P_H)) {
            lv_obj_set_size(o, (p->has & P_W) ? p->w : lv_obj_get_width(o), (p->has & P_H) ? p->h : lv_obj_get_height(o));
        } else if (create) {
            lv_obj_set_size(o, 64, 36);
        }
        if (p->has & P_VALUE) lv_obj_set_state(o, LV_STATE_CHECKED, p->value != 0);
        if ((p->has & P_COLOR) || create) {
            lv_obj_set_style_bg_color(o, lv_color_hex((p->has & P_COLOR) ? p->color : 0x27d3a2), LV_PART_INDICATOR | LV_STATE_CHECKED);
        }
        if ((p->has & P_BG) || create) lv_obj_set_style_bg_color(o, lv_color_hex((p->has & P_BG) ? p->bg : 0x3a3a3c), LV_PART_INDICATOR);
        break;
    case W_LIST:
        if (p->has & P_SIZE) w->scale = (int16_t)p->size;   /* a list keeps its items' text size here */
        if ((p->has & (P_W | P_H)) || create) {
            lv_obj_set_size(o, (p->has & P_W) ? p->w : (create ? 300 : lv_obj_get_width(o)),
                            (p->has & P_H) ? p->h : (create ? 260 : lv_obj_get_height(o)));
        }
        if ((p->has & P_BG) || create) {
            lv_obj_set_style_bg_color(o, lv_color_hex((p->has & P_BG) ? p->bg : 0x000000), 0);
            lv_obj_set_style_bg_opa(o, (p->has & P_BG) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(o, 0, 0);
            lv_obj_set_style_pad_row(o, 6, 0);
        }
        if (p->has & P_OPTIONS) {
            lv_obj_clean(o);
            void *ud = (void *)(uintptr_t)((int)(w - V.w) | (V.gen << 8));
            const lv_font_t *f = luapp_font(w->scale > 0 ? w->scale : 20);
            uint32_t bg = (p->has & P_COLOR) ? p->color : 0x1c1c1e;
            uint32_t tc = (p->has & P_TEXT_COLOR) ? p->text_color : 0xffffff;
            char *s = p->opts;
            for (int i = 1; s && i <= p->nopts; i++) {
                char *nl = strchr(s, '\n');
                if (nl) *nl = 0;
                lv_obj_t *b = lv_list_add_button(o, NULL, s);
                lv_obj_set_user_data(b, (void *)(intptr_t)i);
                lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
                lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(b, 14, 0);
                lv_obj_set_style_border_width(b, 0, 0);
                lv_obj_set_style_text_color(b, lv_color_hex(tc), 0);
                lv_obj_set_style_text_font(b, f, 0);
                lv_obj_set_style_pad_ver(b, 12, 0);
                lv_obj_add_event_cb(b, on_change, LV_EVENT_SHORT_CLICKED, ud);
                s = nl ? nl + 1 : NULL;
            }
        }
        break;
    case W_SCROLL:
        if ((p->has & (P_W | P_H)) || create) {
            lv_obj_set_size(o, (p->has & P_W) ? p->w : (create ? 360 : lv_obj_get_width(o)),
                            (p->has & P_H) ? p->h : (create ? 300 : lv_obj_get_height(o)));
        }
        if (p->has & P_COLOR) {
            lv_obj_set_style_bg_color(o, lv_color_hex(p->color), 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        }
        if (p->has & P_RADIUS) lv_obj_set_style_radius(o, p->radius, 0);
        break;
    }
}

/* ---- ui.* ---- */

#define WIDGET_MT "luapp.widget"

typedef struct {
    int idx;
    uint32_t gen;
} whandle_t;

static widget_t *check_widget(lua_State *L, int i)
{
    whandle_t *h = luaL_checkudata(L, i, WIDGET_MT);
    if (h->gen != V.gen || h->idx < 0 || h->idx >= MAX_W || !V.w[h->idx].used) luaL_error(L, "widget was deleted");
    return &V.w[h->idx];
}

static void set_ref(lua_State *L, int *ref, int t, const char *field)
{
    if (*ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, *ref);
    lua_getfield(L, t, field);
    *ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

static void set_change_ref(lua_State *L, widget_t *w, int t)
{
    set_ref(L, &w->change_ref, t, lua_getfield(L, t, "on_change") != LUA_TNIL ? "on_change" : "on_select");
    lua_pop(L, 1);
}

int luapp_canvas_arg(lua_State *L, int idx)
{
    widget_t *w = check_widget(L, idx);
    if (w->kind != W_CANVAS) luaL_error(L, "only a canvas can draw (ui.canvas)");
    return w->canvas;
}

/* A widget's parent: a ui.scroll (raises otherwise). */
static int parent_arg(lua_State *L, int i)
{
    widget_t *pw = check_widget(L, i);
    if (pw->kind != W_SCROLL) luaL_error(L, "parent must be a ui.scroll");
    return (int)(pw - V.w);
}

static int create(lua_State *L, wkind_t kind)
{
    /* The handle first: allocating it can raise, and nothing must be held then. */
    whandle_t *h = lua_newuserdatauv(L, sizeof(*h), 0);
    h->idx = -1;
    h->gen = V.gen;
    luaL_setmetatable(L, WIDGET_MT);
    props_t p;
    read_props(L, 1, &p);
    int idx = -1;
    for (int i = 0; i < MAX_W; i++) {
        if (!V.w[i].used) {
            idx = i;
            break;
        }
    }
    const char *bad = idx < 0 ? "too many widgets (max 96)"
                    : kind == W_LINE && !(p.has & P_POINTS) ? "a line needs points"
                    : kind == W_IMAGE && !(p.has & P_IMG) ? "ui.image needs img (from gfx.image or gfx.sprite)"
                    : (p.has & P_PARENT) && (kind == W_LINE || kind == W_CANVAS || kind == W_SCROLL)
                        ? "lines, canvases and scrolls can't go in a scroll"
                    : NULL;
    if (bad) {
        heap_caps_free(p.pts);
        heap_caps_free(p.opts);
        return luaL_error(L, "%s", bad);
    }
    int slot = -1;
    if (kind == W_CANVAS) {
        const char *err = "a canvas needs w and h of at least 1";
        int cw = (p.has & P_W) ? p.w : 200, chh = (p.has & P_H) ? p.h : 200, zoom = (p.has & P_ZOOM) ? p.zoom : 1;
        heap_caps_free(p.pts);   /* a canvas has no points */
        p.pts = NULL;
        heap_caps_free(p.opts);
        p.opts = NULL;
        if (cw * zoom > 466 || chh * zoom > 466) {
            return luaL_error(L, "ui.canvas: w and h times zoom must fit the 466 px screen (zoom=2: up to 233)");
        }
        if (cw >= 1 && chh >= 1) slot = luapp_canvas_new(L, cw, chh, (p.has & P_COLOR) ? p.color : 0x000000, &err);
        if (slot < 0) return luaL_error(L, "ui.canvas: %s", err);
    }
    h->idx = idx;
    widget_t *w = &V.w[idx];
    memset(w, 0, sizeof(*w));
    w->kind = (uint8_t)kind;
    w->tap_ref = LUA_NOREF;
    w->touch_ref = LUA_NOREF;
    w->change_ref = LUA_NOREF;
    w->anim_ref = LUA_NOREF;
    w->parent = (int8_t)((p.has & P_PARENT) ? p.parent : -1);
    w->canvas = (int8_t)slot;
    if (!lock()) {
        heap_caps_free(p.pts);
        heap_caps_free(p.opts);
        luapp_canvas_free(slot);
        return luaL_error(L, "the screen is busy");
    }
    lv_obj_t *root = w->parent >= 0 ? V.w[w->parent].obj : luapp_ui_app_root();
    switch (kind) {
    case W_LABEL: w->obj = lv_label_create(root); break;
    case W_ARC:
        w->obj = lv_arc_create(root);
        lv_obj_remove_style(w->obj, NULL, LV_PART_KNOB);
        lv_obj_set_size(w->obj, 80, 80);
        break;
    case W_BAR: w->obj = lv_bar_create(root); lv_obj_set_size(w->obj, 200, 12); break;
    case W_LINE: w->obj = lv_line_create(root); lv_obj_set_pos(w->obj, 0, 0); break;
    case W_IMAGE: w->obj = lv_image_create(root); break;
    case W_CANVAS:
        w->obj = lv_canvas_create(root);
        luapp_canvas_attach(slot, w->obj, (p.has & P_ZOOM) ? p.zoom : 1);
        break;
    case W_ROLLER: w->obj = lv_roller_create(root); break;
    case W_SLIDER: w->obj = lv_slider_create(root); break;
    case W_SWITCH: w->obj = lv_switch_create(root); break;
    case W_LIST: w->obj = lv_list_create(root); break;
    case W_SCROLL:
        w->obj = lv_obj_create(root);
        lv_obj_remove_style_all(w->obj);
        lv_obj_set_scroll_dir(w->obj, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(w->obj, LV_SCROLLBAR_MODE_OFF);
        break;
    default:
        w->obj = lv_obj_create(root);
        lv_obj_remove_style_all(w->obj);
        lv_obj_set_size(w->obj, kind == W_BUTTON ? 140 : 100, kind == W_BUTTON ? 48 : 100);
        break;
    }
    bool scrolls = kind == W_ROLLER || kind == W_LIST || kind == W_SCROLL;
    bool input = scrolls || kind == W_SLIDER || kind == W_SWITCH;
    if (!scrolls) lv_obj_remove_flag(w->obj, LV_OBJ_FLAG_SCROLLABLE);
    if (!input) lv_obj_remove_flag(w->obj, LV_OBJ_FLAG_CLICKABLE);
    if (kind == W_ROLLER || kind == W_SLIDER || kind == W_SWITCH) {
        /* Their drags are theirs: they don't swipe to the next screen. */
        lv_obj_remove_flag(w->obj, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_add_event_cb(w->obj, on_change, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)(idx | (V.gen << 8)));
    }
    if (kind == W_BUTTON) {
        w->lbl = lv_label_create(w->obj);
        lv_obj_center(w->lbl);
        lv_obj_add_flag(w->obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(w->obj, on_button, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)(idx | (V.gen << 8)));
    }
    if (kind == W_IMAGE && (p.has & P_TAP)) {
        lv_obj_add_flag(w->obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(w->obj, on_button, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)(idx | (V.gen << 8)));
    }
    if (kind == W_CANVAS && (p.has & (P_TAP | P_TOUCH))) {
        bool touch = (p.has & P_TOUCH) != 0;
        lv_obj_add_flag(w->obj, LV_OBJ_FLAG_CLICKABLE);
        if (touch) {
            /* Its drags are the app's: they don't swipe to the next screen. */
            lv_obj_remove_flag(w->obj, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        } else {
            lv_obj_add_flag(w->obj, LV_OBJ_FLAG_EVENT_BUBBLE);   /* a long-press still leaves the app */
        }
        void *ud = (void *)(uintptr_t)(idx | (touch ? 0x80 : 0) | (V.gen << 8));
        static const lv_event_code_t codes[] = {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED,
                                                LV_EVENT_PRESS_LOST, LV_EVENT_SHORT_CLICKED};
        for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) lv_obj_add_event_cb(w->obj, on_canvas, codes[i], ud);
    }
    if (!(p.has & (P_X | P_Y)) && kind != W_LINE) lv_obj_align(w->obj, LV_ALIGN_CENTER, 0, 0);
    apply(w, &p, true);
    unlock();
    heap_caps_free(p.pts);
    heap_caps_free(p.opts);
    w->used = true;
    atomic_store(&s_chg_pending[idx], false);
    if (p.has & P_TAP) set_ref(L, &w->tap_ref, 1, "on_tap");
    if (p.has & P_TOUCH) set_ref(L, &w->touch_ref, 1, "on_touch");
    if (p.has & P_CHANGE) set_change_ref(L, w, 1);
    return 1;
}

static int l_label(lua_State *L) { return create(L, W_LABEL); }
static int l_box(lua_State *L) { return create(L, W_BOX); }
static int l_button(lua_State *L) { return create(L, W_BUTTON); }
static int l_arc(lua_State *L) { return create(L, W_ARC); }
static int l_bar(lua_State *L) { return create(L, W_BAR); }
static int l_line(lua_State *L) { return create(L, W_LINE); }
static int l_image(lua_State *L) { return create(L, W_IMAGE); }
static int l_canvas(lua_State *L) { return create(L, W_CANVAS); }

static int w_set(lua_State *L)
{
    widget_t *w = check_widget(L, 1);
    props_t p;
    read_props(L, 2, &p);
    if (!lock()) {
        heap_caps_free(p.pts);
        heap_caps_free(p.opts);
        return luaL_error(L, "the screen is busy");
    }
    apply(w, &p, false);
    unlock();
    heap_caps_free(p.pts);
    heap_caps_free(p.opts);
    if (p.has & P_TAP) set_ref(L, &w->tap_ref, 2, "on_tap");
    if (p.has & P_TOUCH) set_ref(L, &w->touch_ref, 2, "on_touch");
    if (p.has & P_CHANGE) set_change_ref(L, w, 2);
    lua_settop(L, 1);
    return 1;
}

static int w_visible(lua_State *L, bool show)
{
    widget_t *w = check_widget(L, 1);
    if (!lock()) return luaL_error(L, "the screen is busy");
    lv_obj_set_flag(w->obj, LV_OBJ_FLAG_HIDDEN, !show);
    unlock();
    return 0;
}
static int w_hide(lua_State *L) { return w_visible(L, false); }
static int w_show(lua_State *L) { return w_visible(L, true); }

/* The widget's LVGL object must be gone (or go here): lines draw from their
 * points, canvases from their buffers, images from their pixels. */
static void free_widget(lua_State *L, widget_t *w, bool delete_obj)
{
    if (delete_obj && w->obj) lv_obj_delete(w->obj);
    heap_caps_free(w->pts);
    luapp_img_unref(w->img);
    if (w->used && w->kind == W_CANVAS) luapp_canvas_free(w->canvas);
    if (L && w->tap_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, w->tap_ref);
    if (L && w->touch_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, w->touch_ref);
    if (L && w->change_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, w->change_ref);
    if (L && w->anim_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, w->anim_ref);
    memset(w, 0, sizeof(*w));
}

/* The widgets in a scroll whose LVGL object was just deleted (theirs went
 * with it): free their slots too. */
static void free_children(lua_State *L, int parent)
{
    for (int i = 0; i < MAX_W; i++) {
        if (V.w[i].used && V.w[i].parent == parent) {
            V.w[i].obj = NULL;
            free_children(L, i);
            free_widget(L, &V.w[i], false);
        }
    }
}

static int w_delete(lua_State *L)
{
    widget_t *w = check_widget(L, 1);
    if (!lock()) return luaL_error(L, "the screen is busy");
    lv_obj_delete(w->obj);   /* and its children's, in a scroll */
    w->obj = NULL;
    unlock();
    free_children(L, (int)(w - V.w));
    free_widget(L, w, false);
    return 0;
}

static int l_clear(lua_State *L)
{
    if (!lock()) return luaL_error(L, "the screen is busy");
    for (int i = 0; i < MAX_W; i++) {   /* top level only: a scroll takes its children with it */
        if (V.w[i].used && V.w[i].obj && V.w[i].parent < 0) lv_obj_delete(V.w[i].obj);
    }
    for (int i = 0; i < MAX_W; i++) V.w[i].obj = NULL;
    unlock();
    for (int i = 0; i < MAX_W; i++) {
        if (V.w[i].used) free_widget(L, &V.w[i], false);
    }
    return 0;
}

/* ---- ui.animate ---- */

static void ax(void *o, int32_t v) { lv_obj_set_x(o, v); }
static void ay(void *o, int32_t v) { lv_obj_set_y(o, v); }
static void aopa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void aw(void *o, int32_t v) { lv_obj_set_width(o, v); }
static void ah(void *o, int32_t v) { lv_obj_set_height(o, v); }
static void arot(void *o, int32_t v) { lv_image_set_rotation(o, v); }
static void ascale(void *o, int32_t v) { lv_image_set_scale(o, (uint32_t)(v < 1 ? 1 : v)); }
static void aarc(void *o, int32_t v) { lv_arc_set_value(o, v); }
static void abar(void *o, int32_t v) { lv_bar_set_value(o, v, LV_ANIM_OFF); }
static void aslider(void *o, int32_t v) { lv_slider_set_value(o, v, LV_ANIM_OFF); }

static bool num_field(lua_State *L, int t, const char *k, double *out)
{
    bool has = lua_getfield(L, t, k) != LUA_TNIL;
    if (has) *out = luaL_checknumber(L, -1);
    lua_pop(L, 1);
    return has;
}

/* ui.animate(w, {x, y, opacity, w, h, value, angle, scale}, ms, {ease, delay,
 * ["repeat"], reverse, on_done}): LVGL animates it on its own, a frame at a
 * time, with no Lua per frame. */
static int l_animate(lua_State *L)
{
    widget_t *w = check_widget(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    int ms = (int)luaL_optinteger(L, 3, 300);
    ms = ms < 1 ? 1 : ms > 60000 ? 60000 : ms;
    lv_anim_path_cb_t path = lv_anim_path_ease_in_out;
    int delay = 0, repeat = 0;
    bool reverse = false, done = false;
    if (lua_istable(L, 4)) {
        if (lua_getfield(L, 4, "ease") != LUA_TNIL) {
            const char *e = luaL_checkstring(L, -1);
            path = !strcmp(e, "linear") ? lv_anim_path_linear : !strcmp(e, "in") ? lv_anim_path_ease_in
                 : !strcmp(e, "out") ? lv_anim_path_ease_out : !strcmp(e, "overshoot") ? lv_anim_path_overshoot
                 : !strcmp(e, "bounce") ? lv_anim_path_bounce : !strcmp(e, "step") ? lv_anim_path_step
                 : lv_anim_path_ease_in_out;
        }
        lua_pop(L, 1);
        if (lua_getfield(L, 4, "delay") != LUA_TNIL) delay = (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
        int rt = lua_getfield(L, 4, "repeat");
        if (rt == LUA_TBOOLEAN) repeat = lua_toboolean(L, -1) ? -1 : 0;
        else if (rt != LUA_TNIL) repeat = (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
        if (lua_getfield(L, 4, "reverse") != LUA_TNIL) reverse = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (lua_getfield(L, 4, "on_done") != LUA_TNIL) {
            luaL_checktype(L, -1, LUA_TFUNCTION);
            done = true;
        }
        lua_pop(L, 1);
    }
    double x = 0, y = 0, opa = 0, ww = 0, hh = 0, val = 0, ang = 0, sc = 0;
    bool hx = num_field(L, 2, "x", &x), hy = num_field(L, 2, "y", &y), ho = num_field(L, 2, "opacity", &opa);
    bool hw = num_field(L, 2, "w", &ww), hh_ = num_field(L, 2, "h", &hh), hv = num_field(L, 2, "value", &val);
    bool ha = num_field(L, 2, "angle", &ang), hs = num_field(L, 2, "scale", &sc);
    if (!lock()) return luaL_error(L, "the screen is busy");
    lv_obj_t *o = w->obj;
    int idx = (int)(w - V.w);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_duration(&a, (uint32_t)ms);
    lv_anim_set_delay(&a, (uint32_t)(delay < 0 ? 0 : delay));
    lv_anim_set_path_cb(&a, path);
    if (repeat) lv_anim_set_repeat_count(&a, repeat < 0 ? LV_ANIM_REPEAT_INFINITE : (uint32_t)repeat);
    if (reverse) lv_anim_set_reverse_duration(&a, (uint32_t)ms);
    lv_anim_set_user_data(&a, (void *)(uintptr_t)(idx | (V.gen << 8)));
    int started = 0;
#define GO(cb, from, to)                                                    \
    do {                                                                    \
        lv_anim_delete(o, cb);                                              \
        lv_anim_set_exec_cb(&a, cb);                                        \
        lv_anim_set_values(&a, (int32_t)(from), (int32_t)(to));             \
        lv_anim_set_completed_cb(&a, done && !started ? on_anim_done : NULL); \
        lv_anim_start(&a);                                                  \
        started++;                                                          \
    } while (0)
    if (hx) { GO(ax, lv_obj_get_style_x(o, 0), lround(x)); w->x = (int16_t)lround(x); }
    if (hy) { GO(ay, lv_obj_get_style_y(o, 0), lround(y)); w->y = (int16_t)lround(y); }
    if (ho) GO(aopa, lv_obj_get_style_opa(o, 0), opa < 0 ? 0 : opa > 255 ? 255 : lround(opa));
    if (hw) GO(aw, lv_obj_get_width(o), lround(ww));
    if (hh_) GO(ah, lv_obj_get_height(o), lround(hh));
    if (hv && w->kind == W_ARC) GO(aarc, lv_arc_get_value(o), lround(val));
    if (hv && w->kind == W_BAR) GO(abar, lv_bar_get_value(o), lround(val));
    if (hv && w->kind == W_SLIDER) GO(aslider, lv_slider_get_value(o), lround(val));
    if (ha && w->kind == W_IMAGE) {
        lv_image_set_antialias(o, true);
        GO(arot, lv_image_get_rotation(o), lround(ang * 10));
    }
    if (hs && w->kind == W_IMAGE && w->img) {
        w->scale = (int16_t)(sc < 1 ? 1 : sc > 1600 ? 1600 : lround(sc));
        GO(ascale, lv_image_get_scale(o), (int32_t)w->scale * 256 * w->img->scale / 100);
    }
#undef GO
    unlock();
    if (done && started) {
        if (w->anim_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, w->anim_ref);
        lua_getfield(L, 4, "on_done");
        w->anim_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

static int w_stop(lua_State *L)
{
    widget_t *w = check_widget(L, 1);
    if (!lock()) return luaL_error(L, "the screen is busy");
    lv_anim_delete(w->obj, NULL);
    unlock();
    return 0;
}

static int l_on_swipe(lua_State *L)
{
    if (V.swipe_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, V.swipe_ref);
    V.swipe_ref = LUA_NOREF;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
        V.swipe_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

static int l_roller(lua_State *L) { return create(L, W_ROLLER); }
static int l_slider(lua_State *L) { return create(L, W_SLIDER); }
static int l_switch(lua_State *L) { return create(L, W_SWITCH); }
static int l_list(lua_State *L) { return create(L, W_LIST); }
static int l_scroll(lua_State *L) { return create(L, W_SCROLL); }

static int l_background(lua_State *L)
{
    uint32_t c = (uint32_t)luaL_checkinteger(L, 1) & 0xffffff;
    if (!lock()) return luaL_error(L, "the screen is busy");
    lv_obj_set_style_bg_color(luapp_ui_app_root(), lv_color_hex(c), 0);
    unlock();
    return 0;
}

static int l_on_tap(lua_State *L)
{
    if (V.bg_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, V.bg_ref);
    V.bg_ref = LUA_NOREF;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
        V.bg_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

static int add_timer(lua_State *L, bool repeat)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ms < 16) ms = 16;   /* ~60 fps; the loop's pacing keeps Muse's share */
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (V.t[i].id == 0) {
            lua_pushvalue(L, 2);
            V.t[i].ref = luaL_ref(L, LUA_REGISTRYINDEX);
            V.t[i].id = ++V.next_timer;
            V.t[i].period_ms = repeat ? (int)ms : 0;
            V.t[i].due_us = now_us() + ms * 1000;
            lua_pushinteger(L, V.t[i].id);
            return 1;
        }
    }
    return luaL_error(L, "too many timers (max %d)", MAX_TIMERS);
}
static int l_every(lua_State *L) { return add_timer(L, true); }
static int l_after(lua_State *L) { return add_timer(L, false); }

static int l_cancel(lua_State *L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (V.t[i].id == id) {
            luaL_unref(L, LUA_REGISTRYINDEX, V.t[i].ref);
            memset(&V.t[i], 0, sizeof(V.t[i]));
        }
    }
    return 0;
}

/* ---- time.* ---- */

static double wall_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static int l_now(lua_State *L) { lua_pushnumber(L, wall_now()); return 1; }
static int l_synced(lua_State *L) { lua_pushboolean(L, wall_now() >= CLOCK_VALID); return 1; }

static int push_date(lua_State *L, bool local)
{
    int64_t t = lua_isnoneornil(L, 1) ? (int64_t)wall_now() : (int64_t)luaL_checknumber(L, 1);
    int32_t off = local ? watch_local_offset(t) : 0;
    muse_civil_t c;
    muse_tz_civil(t + off, &c);
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, c.year); lua_setfield(L, -2, "year");
    lua_pushinteger(L, c.month); lua_setfield(L, -2, "month");
    lua_pushinteger(L, c.day); lua_setfield(L, -2, "day");
    lua_pushinteger(L, c.hour); lua_setfield(L, -2, "hour");
    lua_pushinteger(L, c.min); lua_setfield(L, -2, "min");
    lua_pushinteger(L, c.sec); lua_setfield(L, -2, "sec");
    lua_pushinteger(L, c.wday + 1); lua_setfield(L, -2, "wday");
    lua_pushinteger(L, off); lua_setfield(L, -2, "utc_offset");
    return 1;
}
static int l_date(lua_State *L) { return push_date(L, true); }
static int l_utc(lua_State *L) { return push_date(L, false); }

/* ---- http.get ---- */

static void http_job(uint32_t gen);

static void http_free(vhttp_t *h)
{
    free(h->headers);
    free(h->body);
    h->headers = h->body = NULL;
}

static void http_kick(void)
{
    if (V.http_busy || V.http_n == 0) return;
    vhttp_t *h = &V.http[0];
    char *old_h, *old_b;
    taskENTER_CRITICAL(&s_log_mux);
    strlcpy(s_http_url, h->url, sizeof(s_http_url));
    strlcpy(s_http_method, h->method[0] ? h->method : "GET", sizeof(s_http_method));
    old_h = s_http_headers;   /* a job that never ran */
    old_b = s_http_body;
    s_http_headers = h->headers;
    s_http_body = h->body;
    s_http_body_len = h->body_len;
    s_http_gen = V.gen;
    taskEXIT_CRITICAL(&s_log_mux);
    h->headers = h->body = NULL;   /* the fetch task's now */
    free(old_h);
    free(old_b);
    V.http_busy = apps_net_submit(http_job, h->id);
}

#define HTTP_SEND_MAX (16 * 1024)
#define HTTP_HEADERS_MAX 1024

/* http.get(url, fn [, headers]), http.post(url, body, fn [, headers]) and
 * http.request{url=, method=, headers=, body=}, fn: body a string, or a table
 * sent as JSON (Content-Type: application/json unless headers say otherwise). */
static int http_add(lua_State *L, const char *url, const char *method, int headers_idx, int body_idx, int fn_idx)
{
    size_t n = strlen(url);
    luaL_checktype(L, fn_idx, LUA_TFUNCTION);
    if (n > URL_MAX || (strncmp(url, "https://", 8) && strncmp(url, "http://", 7))) {
        return luaL_error(L, "http: an http(s) URL of up to %d characters", URL_MAX);
    }
    if (V.http_n >= MAX_HTTP) return luaL_error(L, "too many HTTP requests waiting (max %d)", MAX_HTTP);
    luaL_Buffer hb;
    luaL_buffinit(L, &hb);
    bool has_type = false;
    if (headers_idx && lua_istable(L, headers_idx)) {
        lua_pushnil(L);
        while (lua_next(L, headers_idx)) {
            if (lua_type(L, -2) == LUA_TSTRING && (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER)) {
                const char *k = lua_tostring(L, -2), *v = lua_tostring(L, -1);
                if (strpbrk(k, "\r\n:") || strpbrk(v, "\r\n")) return luaL_error(L, "http: bad header %s", k);
                if (!strcasecmp(k, "content-type")) has_type = true;
                luaL_addstring(&hb, k);
                luaL_addstring(&hb, ": ");
                luaL_addstring(&hb, v);
                luaL_addchar(&hb, '\n');
            }
            lua_pop(L, 1);
        }
    }
    const char *body = NULL;
    size_t body_len = 0;
    char *json = NULL;
    if (body_idx && lua_istable(L, body_idx)) {
        cJSON *j = luapp_to_cjson(L, body_idx, 0);
        json = j ? cJSON_PrintUnformatted(j) : NULL;
        cJSON_Delete(j);
        if (!json) return luaL_error(L, "http: couldn't encode the body");
        body = json;
        body_len = strlen(json);
        if (!has_type) luaL_addstring(&hb, "Content-Type: application/json\n");
    } else if (body_idx && !lua_isnoneornil(L, body_idx)) {
        body = luaL_checklstring(L, body_idx, &body_len);
    }
    luaL_pushresult(&hb);
    size_t hlen;
    const char *hs = lua_tolstring(L, -1, &hlen);
    const char *err = body_len > HTTP_SEND_MAX ? "http: body over 16 KB" : hlen > HTTP_HEADERS_MAX ? "http: headers over 1 KB" : NULL;
    char *h_copy = NULL, *b_copy = NULL;
    if (!err && hlen) {
        h_copy = heap_caps_malloc(hlen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (h_copy) memcpy(h_copy, hs, hlen + 1);
        else err = "http: out of memory";
    }
    if (!err && body_len) {
        b_copy = heap_caps_malloc(body_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (b_copy) {
            memcpy(b_copy, body, body_len);
            b_copy[body_len] = 0;
        } else {
            err = "http: out of memory";
        }
    }
    free(json);
    lua_pop(L, 1);
    if (err) {
        free(h_copy);
        free(b_copy);
        return luaL_error(L, "%s", err);
    }
    vhttp_t *h = &V.http[V.http_n++];
    strlcpy(h->url, url, sizeof(h->url));
    strlcpy(h->method, method, sizeof(h->method));
    h->headers = h_copy;
    h->body = b_copy;
    h->body_len = body_len;
    h->id = ++V.http_seq;
    lua_pushvalue(L, fn_idx);
    h->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    http_kick();
    return 0;
}

static int l_http_get(lua_State *L)
{
    return http_add(L, luaL_checkstring(L, 1), "GET", lua_istable(L, 3) ? 3 : 0, 0, 2);
}

static int l_http_post(lua_State *L)
{
    return http_add(L, luaL_checkstring(L, 1), "POST", lua_istable(L, 4) ? 4 : 0, 2, 3);
}

static int l_http_request(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "url");
    const char *url = luaL_optstring(L, -1, NULL);
    if (!url) return luaL_error(L, "http.request: url is required");
    lua_getfield(L, 1, "method");
    char method[8];
    strlcpy(method, luaL_optstring(L, -1, "GET"), sizeof(method));
    for (char *c = method; *c; c++) *c = (char)toupper((unsigned char)*c);
    static const char *const OK[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"};
    bool valid = false;
    for (int i = 0; i < 6; i++) valid |= !strcmp(method, OK[i]);
    if (!valid) return luaL_error(L, "http.request: method GET, POST, PUT, PATCH, DELETE or HEAD");
    lua_getfield(L, 1, "headers");
    lua_getfield(L, 1, "body");
    int top = lua_gettop(L);
    return http_add(L, url, method, top - 1, top, 2);
}

/* On the apps' fetch task. */
static void http_job(uint32_t id)
{
    char url[URL_MAX + 1], method[8];
    char *headers, *body;
    size_t body_len;
    uint32_t gen;
    taskENTER_CRITICAL(&s_log_mux);
    strlcpy(url, s_http_url, sizeof(url));
    strlcpy(method, s_http_method, sizeof(method));
    headers = s_http_headers;
    body = s_http_body;
    body_len = s_http_body_len;
    s_http_headers = s_http_body = NULL;
    gen = s_http_gen;
    taskEXIT_CRITICAL(&s_log_mux);
    if (gen != atomic_load(&s_live_gen)) {   /* its app was unloaded: don't fetch for nobody */
        free(headers);
        free(body);
        return;
    }
    msg_t *m = heap_caps_calloc(1, sizeof(*m), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) {
        free(headers);
        free(body);
        return;
    }
    m->type = M_HTTP;
    m->http_id = id;
    m->gen = gen;
    if (!muse_wifi_connected() || muse_voice_in_call()) {
        m->status = -2;   /* not now */
    } else {
        const char *resp;
        size_t len;
        m->status = apps_net_request(method, url, headers, body, body_len, &resp, &len);
        if (m->status > 0 && resp) {
            if (len > HTTP_BODY_MAX) {
                m->status = -3;
            } else {
                m->data = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (m->data) {
                    memcpy(m->data, resp, len + 1);
                    m->len = len;
                }
            }
        }
    }
    free(headers);
    free(body);
    if (xQueueSend(s_queue, &m, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(m->data);
        free(m);
        return;
    }
    if (s_task) xTaskNotifyGive(s_task);
}

/* ---- store.* ---- */

static int l_store_get(lua_State *L)
{
    const char *k = luaL_checkstring(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, V.kv_ref);
    lua_getfield(L, -1, k);
    return 1;
}

static int l_store_set(lua_State *L)
{
    const char *k = luaL_checkstring(L, 1);
    int t = lua_type(L, 2);
    if (t != LUA_TNIL && t != LUA_TNUMBER && t != LUA_TSTRING && t != LUA_TBOOLEAN) {
        return luaL_error(L, "store.set: strings, numbers and booleans only");
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, V.kv_ref);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, k);
    if (!V.kv_dirty) V.kv_due = now_us() + KV_FLUSH_US;
    V.kv_dirty = true;
    return 0;
}

static void kv_flush(void)
{
    if (!V.L || !V.kv_dirty) return;
    V.kv_dirty = false;
    lua_State *L = V.L;
    lua_getglobal(L, "json");
    lua_getfield(L, -1, "encode");
    lua_rawgeti(L, LUA_REGISTRYINDEX, V.kv_ref);
    if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
        size_t n;
        const char *s = lua_tolstring(L, -1, &n);
        if (!luapp_store_kv_write(V.id, s, n)) log_append("[store too big to save]\n", 24);
    }
    lua_pop(L, 2);
}

/* ---- app.*, print ---- */

static int l_exit(lua_State *L)
{
    (void)L;
    V.exit_req = true;
    return 0;
}

/* app.icon(image or canvas): the app's launcher icon, kept on the board;
 * app.icon(nil) goes back to its initials. A few calls per run: each may
 * write flash. */
static int l_app_icon(lua_State *L)
{
    if (++V.icon_calls > 4) {
        if (V.icon_calls == 5) log_append("[app.icon: ignored after 4 calls per run]\n", 42);
        return 0;
    }
    int64_t t0 = now_us();
    if (lua_isnoneornil(L, 1)) {
        luapp_store_icon_remove(V.id);
        if (lock()) {
            luapp_ui_set_icon(V.id, NULL);
            unlock();
        }
        return 0;
    }
    luapp_pixels_t src;
    luapp_gfx_icon_source(L, 1, &src);
    uint8_t *file = heap_caps_malloc(LUAPP_ICON_FILE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!file) return luaL_error(L, "app.icon: out of memory");
    luapp_icon_make(&src, file);
    bool saved = luapp_store_icon_write(V.id, file, LUAPP_ICON_FILE);
    if (lock()) {
        luapp_ui_set_icon(V.id, file);
        unlock();
    }
    free(file);
    if (!saved) log_append("[app.icon: couldn't save it (storage full?)]\n", 45);
    V.deadline += now_us() - t0;   /* flash writes don't count against the app */
    return 0;
}

static int l_print(lua_State *L)
{
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_addchar(&b, '\n');
    luaL_pushresult(&b);
    size_t len;
    const char *s = lua_tolstring(L, -1, &len);
    log_append(s, len);
    ESP_LOGI(TAG, "%s: %.*s", V.id, (int)(len - 1), s);
    return 0;
}

static void open_api(lua_State *L)
{
    luaL_newmetatable(L, WIDGET_MT);
    static const luaL_Reg wm[] = {{"set", w_set}, {"hide", w_hide}, {"show", w_show}, {"delete", w_delete},
                                  {"stop", w_stop}, {NULL, NULL}};
    luaL_newlib(L, wm);
    luapp_gfx_open(L);   /* the canvas methods join these; gfx.* */
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luapp_sound_open(L);

    static const luaL_Reg ui[] = {{"label", l_label}, {"box", l_box}, {"button", l_button}, {"arc", l_arc},
                                  {"bar", l_bar}, {"line", l_line}, {"image", l_image}, {"canvas", l_canvas},
                                  {"clear", l_clear}, {"background", l_background},
                                  {"on_tap", l_on_tap}, {"every", l_every}, {"after", l_after}, {"cancel", l_cancel},
                                  {"roller", l_roller}, {"slider", l_slider}, {"switch", l_switch}, {"list", l_list},
                                  {"scroll", l_scroll}, {"animate", l_animate}, {"on_swipe", l_on_swipe},
                                  {NULL, NULL}};
    luaL_newlib(L, ui);
    luapp_dev_ui_fields(L);
    lua_setglobal(L, "ui");
    static const luaL_Reg tm[] = {{"now", l_now}, {"synced", l_synced}, {"date", l_date}, {"utc", l_utc}, {NULL, NULL}};
    luaL_newlib(L, tm);
    lua_setglobal(L, "time");
    static const luaL_Reg http[] = {{"get", l_http_get}, {"post", l_http_post}, {"request", l_http_request},
                                    {NULL, NULL}};
    luaL_newlib(L, http);
    lua_setglobal(L, "http");
    static const luaL_Reg st[] = {{"get", l_store_get}, {"set", l_store_set}, {NULL, NULL}};
    luaL_newlib(L, st);
    lua_setglobal(L, "store");
    static const luaL_Reg app[] = {{"exit", l_exit}, {"icon", l_app_icon}, {NULL, NULL}};
    luaL_newlib(L, app);
    luapp_dev_app_fields(L);
    lua_pushstring(L, V.id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, V.name);
    lua_setfield(L, -2, "name");
    lua_setglobal(L, "app");
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");
    luapp_dev_open(L);   /* device, mic, motion, fs, apps, muse */
}

/* ---- running ---- */

static void reset_widgets(lua_State *L)
{
    for (int i = 0; i < MAX_W; i++) {
        if (V.w[i].used || V.w[i].pts) free_widget(L, &V.w[i], false);
    }
}

static bool take_pending_stop(pend_stop_t *out)
{
    taskENTER_CRITICAL(&s_log_mux);
    bool had = s_pend_stop.pending;
    if (had) {
        *out = s_pend_stop;
        s_pend_stop.pending = false;
    }
    taskEXIT_CRITICAL(&s_log_mux);
    return had;
}

static cJSON *unload_json(const last_stop_t *r)
{
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "id", r->id);
    cJSON_AddStringToObject(u, "why", r->why);
    cJSON_AddNumberToObject(u, "freed_kb", r->freed_kb);
    cJSON_AddNumberToObject(u, "lua_kb", r->lua_kb);
    cJSON_AddNumberToObject(u, "graphics_kb", r->gfx_kb);
    cJSON_AddNumberToObject(u, "left_behind_kb", r->leftover_kb);
    cJSON_AddNumberToObject(u, "psram_free_kb", r->psram_kb);
    cJSON_AddNumberToObject(u, "largest_block_kb", r->largest_kb);
    return u;
}

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

/* Woken by each stop Muse asks for: after STOP_WAIT_MS, if the app still
 * hasn't unloaded (the Lua task stuck in C, say), answers with an error, or
 * with force reboots the gadget (after the answer has had time to go). One
 * long-lived task (PSRAM stack): nothing to create when it's needed. */
static TaskHandle_t s_watch;

static void stop_watch(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(STOP_WAIT_MS));
        pend_stop_t p;
        if (take_pending_stop(&p) && s_reply) {
            if (p.force) {
                s_forced_rtc.magic = FORCED_MAGIC;
                strlcpy(s_forced_rtc.id, p.id, sizeof(s_forced_rtc.id));
                ESP_LOGE(TAG, "%s didn't stop within %d ms: rebooting (forced stop)", p.id, STOP_WAIT_MS);
                cJSON *r = cJSON_CreateObject();
                cJSON_AddStringToObject(r, "id", p.id);
                cJSON_AddBoolToObject(r, "forced", true);
                cJSON_AddStringToObject(r, "result", "it didn't stop within 3 s: the gadget is rebooting (back in ~10 s)");
                s_reply(p.sgen, p.req_id, apps_ok(r));
                static esp_timer_handle_t t;
                const esp_timer_create_args_t a = {.callback = reboot_cb, .name = "luapp_reboot"};
                if (!t && esp_timer_create(&a, &t) != ESP_OK) esp_restart();
                esp_timer_start_once(t, 800 * 1000);   /* let the answer go out first */
            } else {
                ESP_LOGW(TAG, "%s didn't stop within %d ms", p.id, STOP_WAIT_MS);
                s_reply(p.sgen, p.req_id, apps_error("timeout", "the app didn't stop within 3 s; force=true reboots the gadget"));
            }
        }
    }
}

/* Stops the running app (if any) and unloads it: its Lua state and heap,
 * timers, queued requests, widgets, canvases, images and sound. With an
 * error, shows it on screen. Logs (and keeps for luapp.admin list) what it
 * freed and anything left behind. */
static void stop_app(const char *error, const char *why)
{
    bool had = V.L != NULL;
    size_t lua_bytes = V.mem, gfx_bytes = luapp_gfx_bytes();
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    luapp_sound_enable(false);
    luapp_dev_stop();
    luapp_perf_stop();   /* its overlay goes with the screen, below */
    if (V.L) {
        kv_flush();
        lua_close(V.L);   /* frees every ref with it */
        V.L = NULL;
    }
    for (int i = 0; i < V.http_n; i++) {
        V.http[i].ref = LUA_NOREF;
        http_free(&V.http[i]);
    }
    memset(V.t, 0, sizeof(V.t));
    V.http_n = 0;
    V.http_busy = false;   /* an answer in flight is dropped by its gen */
    V.gen++;
    atomic_store(&s_live_gen, V.gen);   /* the fetch task skips requests for it */
    V.exit_req = false;
    V.kv_dirty = false;
    V.away_since = 0;
    set_running(NULL);
    if (error) {
        set_error(error);
        log_append("[error] ", 8);
        log_append(error, strlen(error));
        log_append("\n", 1);
        ESP_LOGW(TAG, "%s stopped: %s", V.id, error);
    }
    if (lock()) {
        lv_obj_clean(luapp_ui_app_root());
        if (error) {
            luapp_ui_show_error(V.name, error);
        } else {
            luapp_ui_show_launcher();
        }
        unlock();
    }
    /* Only now that the objects are gone: lines draw from their points,
     * images and canvases from their pixels. */
    reset_widgets(NULL);
    luapp_gfx_reset();
    atomic_store(&s_stop_req, STOP_NONE);
    if (!had) return;
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM), left = luapp_gfx_bytes();
    last_stop_t r = {0};
    strlcpy(r.id, V.id, sizeof(r.id));
    strlcpy(r.why, why ? why : "stopped", sizeof(r.why));
    r.lua_kb = (unsigned)(lua_bytes / 1024);
    r.gfx_kb = (unsigned)(gfx_bytes / 1024);
    r.freed_kb = psram_after > psram_before ? (unsigned)((psram_after - psram_before) / 1024) : 0;
    r.leftover_kb = (unsigned)((left + 1023) / 1024);
    r.psram_kb = (unsigned)(psram_after / 1024);
    r.largest_kb = (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024);
    taskENTER_CRITICAL(&s_log_mux);
    s_last_stop = r;
    taskEXIT_CRITICAL(&s_log_mux);
    ESP_LOGI(TAG, "%s unloaded (%s): freed %u KB (Lua %u KB, graphics %u KB); PSRAM %u KB free, largest block %u KB",
             r.id, r.why, r.freed_kb, r.lua_kb, r.gfx_kb, r.psram_kb, r.largest_kb);
    if (left) ESP_LOGW(TAG, "%s left %u bytes of images behind", r.id, (unsigned)left);
    pend_stop_t p;
    if (take_pending_stop(&p) && s_reply) {   /* Muse's stop: answered now that it's gone */
        cJSON *res = cJSON_CreateObject();
        cJSON_AddItemToObject(res, "unloaded", unload_json(&r));
        s_reply(p.sgen, p.req_id, apps_ok(res));
    }
}

/* Unloads an app that's been off the Apps screen for AWAY_US (asleep on it
 * doesn't count). Its store is kept; opening it again starts it fresh. */
static void away_check(void)
{
    if (!V.L || muse_apps_on_screen(MUSE_APP_LUAPPS)) {
        V.away_since = 0;
        return;
    }
    int64_t now = now_us();
    if (!V.away_since) {
        V.away_since = now;
    } else if (now - V.away_since >= AWAY_US) {
        stop_app(NULL, "30 s off screen");
    }
}

/* Returns NULL when it's running, else why not (static or V-owned text). */
static const char *run_app(const char *id)
{
    EXT_RAM_BSS_ATTR static char err[ERR_MAX + 1];
    stop_app(NULL, "another app started");
    atomic_store(&s_stop_req, STOP_NONE);
    luapp_meta_t meta;
    if (!luapp_store_find(id, &meta)) return "no app with that id";
    size_t len;
    char *code = luapp_store_code(id, &len);
    if (!code) return "couldn't read the app";
    strlcpy(V.id, id, sizeof(V.id));
    strlcpy(V.name, meta.name, sizeof(V.name));
    start_logs(id);
    V.mem = 0;
    V.icon_calls = 0;
    V.L = luapp_sandbox_new(l_alloc, &V.mem);
    if (!V.L) {
        free(code);
        return "out of memory starting Lua";
    }
    lua_State *L = V.L;
    lua_sethook(L, hook, LUA_MASKCOUNT, 2000);
    V.bg_ref = LUA_NOREF;
    V.swipe_ref = LUA_NOREF;
    for (int i = 0; i < MAX_W; i++) atomic_store(&s_chg_pending[i], false);
    open_api(L);
    /* The app's store, from its .kv file. */
    size_t kvn;
    char *kv = luapp_store_kv_read(id, &kvn);
    lua_getglobal(L, "json");
    lua_getfield(L, -1, "decode");
    lua_remove(L, -2);
    lua_pushstring(L, kv ? kv : "{}");
    free(kv);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK || !lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    V.kv_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    luapp_sound_enable(true);
    if (lock()) {
        muse_apps_trim();   /* off to the Apps tile: the other tiles' charts make room */
        luapp_ui_show_app();
        unlock();
    }
    set_running(id);
    ESP_LOGI(TAG, "running %s (%u bytes)", id, (unsigned)len);
    char chunk[LUAPP_ID_MAX + 2];
    snprintf(chunk, sizeof(chunk), "=%s", id);
    int rc = luapp_sandbox_load(L, code, len, chunk);
    free(code);
    if (rc != LUA_OK) {
        strlcpy(err, lua_tostring(L, -1), sizeof(err));
        stop_app(err, "error");
        return err;
    }
    if (!call(0, START_MS)) {
        taskENTER_CRITICAL(&s_log_mux);
        strlcpy(err, s_error, sizeof(err));
        taskEXIT_CRITICAL(&s_log_mux);
        return err;
    }
    return NULL;
}

/* ---- for luapp_dev.c ---- */

const char *luapp_vm_app_id(void) { return V.L ? V.id : ""; }
void luapp_vm_app_name(char *out, size_t n) { strlcpy(out, V.name, n); }
void luapp_vm_forgive(int64_t us) { V.deadline += us; }
void luapp_vm_wake(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}
bool luapp_vm_call(int nargs) { return V.L && call(nargs, CALL_MS); }
bool luapp_vm_call_results(int nargs, int nres) { return V.L && call_n(nargs, nres, CALL_MS); }
void luapp_vm_after_call(void)
{
    if (V.L) after_call();
}

/* An alarm is due: wake the screen, bring up the Apps tile, open the app
 * (unless it's the one running) and call its app.on_alarm(tag). */
void luapp_vm_fire_alarm(const char *id, const char *tag)
{
    muse_state_set_asleep(false);
    muse_state_poke();
    muse_apps_request_show(MUSE_APP_LUAPPS);
    if (!V.L || strcmp(V.id, id)) {
        const char *err = run_app(id);
        if (err) {
            ESP_LOGW(TAG, "alarm %s: %s didn't start: %s", tag, id, err);
            return;
        }
    }
    luapp_dev_call_alarm(tag);
}

/* ---- messages ---- */

static cJSON *ok_payload(cJSON *p) { return apps_ok(p); }

static const char *const KIND_NAME[] = {"label", "box", "button", "arc", "bar", "line", "image", "canvas",
                                        "roller", "slider", "switch", "list", "scroll"};

/* luapp.admin widgets: what the running app has on screen. */
static cJSON *widgets_json(void)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "app", V.L ? V.id : "");
    cJSON *arr = cJSON_AddArrayToObject(p, "widgets");
    if (!V.L || !lock()) return p;
    for (int i = 0; i < MAX_W; i++) {
        widget_t *w = &V.w[i];
        if (!w->used || !w->obj) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "i", i);
        cJSON_AddStringToObject(o, "kind", w->kind < sizeof(KIND_NAME) / sizeof(KIND_NAME[0]) ? KIND_NAME[w->kind] : "?");
        lv_area_t a;
        lv_obj_get_coords(w->obj, &a);
        cJSON_AddNumberToObject(o, "x", a.x1);
        cJSON_AddNumberToObject(o, "y", a.y1);
        cJSON_AddNumberToObject(o, "w", lv_area_get_width(&a));
        cJSON_AddNumberToObject(o, "h", lv_area_get_height(&a));
        if (lv_obj_has_flag(w->obj, LV_OBJ_FLAG_HIDDEN)) cJSON_AddBoolToObject(o, "hidden", true);
        if (w->parent >= 0) cJSON_AddNumberToObject(o, "parent", w->parent);
        lv_obj_t *lbl = w->kind == W_LABEL ? w->obj : w->lbl;
        if (lbl) cJSON_AddStringToObject(o, "text", lv_label_get_text(lbl));
        switch (w->kind) {
        case W_ARC: cJSON_AddNumberToObject(o, "value", lv_arc_get_value(w->obj)); break;
        case W_BAR: cJSON_AddNumberToObject(o, "value", lv_bar_get_value(w->obj)); break;
        case W_SLIDER: cJSON_AddNumberToObject(o, "value", lv_slider_get_value(w->obj)); break;
        case W_SWITCH: cJSON_AddBoolToObject(o, "on", lv_obj_has_state(w->obj, LV_STATE_CHECKED)); break;
        case W_ROLLER: cJSON_AddNumberToObject(o, "selected", lv_roller_get_selected(w->obj) + 1); break;
        default: break;
        }
        if (w->tap_ref != LUA_NOREF) cJSON_AddBoolToObject(o, "on_tap", true);
        cJSON_AddItemToArray(arr, o);
    }
    unlock();
    return p;
}

static void handle_install(msg_t *m)
{
    if (!luapp_store_mounted()) {
        reply(m, apps_error("storage_error", "the app storage isn't mounted"));
        return;
    }
    /* Every piece, the last included, is already in <id>.tmp: luapp_command
     * wrote them on Muse's command task, so an upload never waited for us. */
    size_t len;
    char *code = luapp_store_tmp_read(m->id, &len);
    if (!code) {
        reply(m, apps_error("storage_error", "couldn't read the app back"));
        return;
    }
    /* Compile in a throwaway state: a syntax error comes back with its line. */
    V.check_mem = 0;
    lua_State *C = luapp_sandbox_new(l_alloc, &V.check_mem);
    char chunk[LUAPP_ID_MAX + 2];
    snprintf(chunk, sizeof(chunk), "=%s", m->id);
    int rc = C ? luapp_sandbox_load(C, code, len, chunk) : LUA_ERRMEM;
    free(code);
    if (rc != LUA_OK) {
        char err[ERR_MAX + 1];
        strlcpy(err, C && lua_tostring(C, -1) ? lua_tostring(C, -1) : "out of memory compiling", sizeof(err));
        if (C) lua_close(C);
        luapp_store_tmp_discard(m->id);
        reply(m, apps_error("syntax_error", err));
        return;
    }
    lua_close(C);
    luapp_meta_t old;
    bool had = luapp_store_find(m->id, &old);
    const char *name = m->has_name ? m->name : had ? old.name : m->id;
    uint32_t color = m->has_color ? m->color : had ? old.color : 0x27d3a2;
    if (strcmp(V.id, m->id) == 0 && V.L) stop_app(NULL, "reinstalled");   /* replacing the running app */
    if (!luapp_store_commit(m->id, name, color)) {
        reply(m, apps_error("storage_error", "couldn't save the app"));
        return;
    }
    ESP_LOGI(TAG, "installed %s (%u bytes)", m->id, (unsigned)len);
    if (!m->run) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "id", m->id);
        cJSON_AddNumberToObject(p, "bytes", (double)len);
        reply(m, ok_payload(p));
        return;
    }
    muse_apps_request_show(MUSE_APP_LUAPPS);
    const char *err = run_app(m->id);
    if (err) {
        char msg[ERR_MAX + 64];
        snprintf(msg, sizeof(msg), "installed, but it stopped while starting: %s", err);
        reply(m, apps_error("runtime_error", msg));
        return;
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "id", m->id);
    cJSON_AddNumberToObject(p, "bytes", (double)len);
    cJSON_AddBoolToObject(p, "running", true);
    reply(m, ok_payload(p));
}

static void handle(msg_t *m)
{
    switch (m->type) {
    case M_INSTALL:
        handle_install(m);
        break;
    case M_RUN: {
        if (m->has_req) muse_apps_request_show(MUSE_APP_LUAPPS);
        const char *err = run_app(m->id);
        if (err) {
            reply(m, apps_error("runtime_error", err));
        } else {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "running", m->id);
            reply(m, ok_payload(p));
        }
        break;
    }
    case M_STOP:
        stop_app(NULL, "Muse asked");
        reply(m, ok_payload(NULL));
        break;
    case M_REMOVE: {
        if (!strcmp(V.id, m->id) && V.L) stop_app(NULL, "removed");
        bool existed = luapp_store_remove(m->id);
        luapp_dev_alarms_forget(m->id);
        if (lock()) {
            luapp_ui_set_icon(m->id, NULL);
            if (!V.L) luapp_ui_show_launcher();
            unlock();
        }
        reply(m, existed ? ok_payload(NULL) : apps_error("not_found", "no app with that id"));
        break;
    }
    case M_SOURCE: {
        size_t len;
        char *code = luapp_store_code(m->id, &len);
        if (!code) {
            reply(m, apps_error("not_found", "no app with that id"));
            break;
        }
        size_t off = m->offset < len ? m->offset : len, n = len - off < 3000 ? len - off : 3000;
        char save = code[off + n];
        code[off + n] = 0;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "id", m->id);
        cJSON_AddNumberToObject(p, "offset", (double)off);
        cJSON_AddNumberToObject(p, "total", (double)len);
        cJSON_AddStringToObject(p, "code", code + off);
        code[off + n] = save;
        free(code);
        reply(m, ok_payload(p));
        break;
    }
    case M_TAP:
        if (!V.L || m->gen != (V.gen & 0xffffff)) {
            if (!V.L && m->widget == -2 && lock()) {   /* long-press on an error card: back to the launcher */
                luapp_ui_show_launcher();
                unlock();
            }
            break;
        }
        if (m->widget == -2) {   /* long-press: leave the app */
            stop_app(NULL, "long-press");
        } else if (m->widget == -3 || m->widget == -4) {   /* a swipe up or down */
            if (V.swipe_ref != LUA_NOREF) {
                lua_rawgeti(V.L, LUA_REGISTRYINDEX, V.swipe_ref);
                lua_pushstring(V.L, m->widget == -3 ? "up" : "down");
                call(1, CALL_MS);
            }
        } else if (m->widget == -1) {
            if (V.bg_ref != LUA_NOREF) {
                lua_rawgeti(V.L, LUA_REGISTRYINDEX, V.bg_ref);
                lua_pushinteger(V.L, m->x);
                lua_pushinteger(V.L, m->y);
                call(2, CALL_MS);
            }
        } else if (m->widget >= 0 && m->widget < MAX_W && V.w[m->widget].used && V.w[m->widget].tap_ref != LUA_NOREF) {
            lua_rawgeti(V.L, LUA_REGISTRYINDEX, V.w[m->widget].tap_ref);
            if (V.w[m->widget].kind == W_CANVAS) {   /* where on the canvas */
                lua_pushinteger(V.L, m->x);
                lua_pushinteger(V.L, m->y);
                call(2, CALL_MS);
            } else {
                call(0, CALL_MS);
            }
        }
        break;
    case M_CHANGE: {
        if (m->widget < 0 || m->widget >= MAX_W) break;
        atomic_store(&s_chg_pending[m->widget], false);   /* first: a later change posts again */
        if (!V.L || m->gen != (V.gen & 0xffffff)) break;
        widget_t *w = &V.w[m->widget];
        if (!w->used || w->change_ref == LUA_NOREF) break;
        int v = atomic_load(&s_chg_val[m->widget]);
        lua_rawgeti(V.L, LUA_REGISTRYINDEX, w->change_ref);
        if (w->kind == W_SWITCH) lua_pushboolean(V.L, v);
        else lua_pushinteger(V.L, v);
        call(1, CALL_MS);
        break;
    }
    case M_ANIM: {
        if (!V.L || m->gen != (V.gen & 0xffffff) || m->widget < 0 || m->widget >= MAX_W) break;
        widget_t *w = &V.w[m->widget];
        if (!w->used || w->anim_ref == LUA_NOREF) break;
        int ref = w->anim_ref;
        w->anim_ref = LUA_NOREF;   /* once per animate */
        lua_rawgeti(V.L, LUA_REGISTRYINDEX, ref);
        luaL_unref(V.L, LUA_REGISTRYINDEX, ref);
        call(0, CALL_MS);
        break;
    }
    case M_TOUCH: {
        if (m->touch == 1) {   /* the latest point of the drag */
            atomic_store(&s_mv_queued, false);
            m->x = atomic_load(&s_mv_x);
            m->y = atomic_load(&s_mv_y);
        }
        if (!V.L || m->gen != (V.gen & 0xffffff) || m->widget < 0 || m->widget >= MAX_W) break;
        widget_t *w = &V.w[m->widget];
        if (!w->used || w->touch_ref == LUA_NOREF) break;
        static const char *const K[] = {"down", "move", "up"};
        lua_rawgeti(V.L, LUA_REGISTRYINDEX, w->touch_ref);
        lua_pushstring(V.L, K[m->touch < 3 ? m->touch : 2]);
        lua_pushinteger(V.L, m->x);
        lua_pushinteger(V.L, m->y);
        call(3, CALL_MS);
        break;
    }
    case M_SEND: {
        cJSON *data = m->data ? cJSON_Parse(m->data) : NULL;
        const char *err;
        if (m->id[0] && (!V.L || strcmp(m->id, V.id))) {
            cJSON_Delete(data);
            reply(m, apps_error("not_running", "that app isn't running"));
            break;
        }
        cJSON *r = luapp_dev_message(data, &err);
        cJSON_Delete(data);
        if (err) {
            reply(m, apps_error(V.L ? "no_handler" : "not_running", err));
        } else {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddItemToObject(p, "reply", r);
            reply(m, ok_payload(p));
        }
        break;
    }
    case M_SHOT: {
        const char *err;
        cJSON *p = luapp_shot(m->x, m->offset, &err);
        reply(m, p ? ok_payload(p) : apps_error("unavailable", err));
        break;
    }
    case M_WIDGETS:
        reply(m, ok_payload(widgets_json()));
        break;
    case M_HTTP: {
        if (!V.L || m->gen != V.gen || V.http_n == 0 || V.http[0].id != m->http_id) break;
        vhttp_t h = V.http[0];
        http_free(&h);
        memmove(&V.http[0], &V.http[1], sizeof(V.http[0]) * (size_t)(--V.http_n));
        V.http_busy = false;
        lua_State *L = V.L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
        luaL_unref(L, LUA_REGISTRYINDEX, h.ref);
        bool ok = m->status >= 200 && m->status < 300 && m->data;
        lua_pushboolean(L, ok);
        if (m->data) {
            lua_pushlstring(L, m->data, m->len);
        } else {
            lua_pushstring(L, m->status == -1 ? "low memory, try again" : m->status == -2 ? "offline or in a call"
                              : m->status == -3 ? "response over 32 KB" : "request failed");
        }
        lua_pushinteger(L, m->status > 0 ? m->status : 0);
        if (call(3, CALL_MS)) http_kick();
        break;
    }
    }
}

static int64_t run_timers(void)
{
    int64_t next = INT64_MAX;
    if (!V.L) return next;
    bool live = muse_apps_on_screen(MUSE_APP_LUAPPS) && !muse_state_asleep();
    int64_t now = now_us();
    for (int i = 0; i < MAX_TIMERS && V.L; i++) {
        vtimer_t *t = &V.t[i];
        if (!t->id) continue;
        if (t->due_us <= now) {
            if (!live) {   /* paused off screen: catch up when back, without a burst */
                t->due_us = now + (t->period_ms ? t->period_ms : 50) * 1000LL;
            } else {
                int ref = t->ref;
                if (t->period_ms) {
                    t->due_us += t->period_ms * 1000LL;
                    if (t->due_us <= now) t->due_us = now + t->period_ms * 1000LL;
                    lua_rawgeti(V.L, LUA_REGISTRYINDEX, ref);
                } else {
                    lua_rawgeti(V.L, LUA_REGISTRYINDEX, ref);
                    luaL_unref(V.L, LUA_REGISTRYINDEX, ref);
                    memset(t, 0, sizeof(*t));
                }
                if (!call(0, CALL_MS)) return INT64_MAX;
                now = now_us();
                if (uxQueueMessagesWaiting(s_queue) || atomic_load(&s_stop_req)) return now;   /* messages first */
            }
        }
        if (V.L && t->id && t->due_us < next) next = t->due_us;
    }
    if (V.L && V.kv_dirty) {
        if (now >= V.kv_due) kv_flush();
        else if (V.kv_due < next) next = V.kv_due;
    }
    return next;
}

/* The saved icons of the launcher's pages near the one showing (it asks with
 * luapp_vm_want_icons when that changes), into the launcher. */
static atomic_bool s_icons_wanted;

void luapp_vm_want_icons(void)
{
    atomic_store(&s_icons_wanted, true);
    luapp_vm_wake();
}

static void load_icons(void)
{
    char ids[LUAPP_UI_WANT_MAX][LUAPP_ID_MAX + 1];
    int n = 0, loaded = 0;
    if (lock()) {
        n = luapp_ui_icons_wanted(ids, LUAPP_UI_WANT_MAX);
        unlock();
    }
    uint8_t *buf = n ? heap_caps_malloc(LUAPP_ICON_FILE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    for (int i = 0; buf && i < n; i++) {
        if (luapp_store_icon_read(ids[i], buf, LUAPP_ICON_FILE) && lock()) {
            loaded += luapp_ui_set_icon(ids[i], buf);
            unlock();
        }
    }
    free(buf);
    if (loaded) ESP_LOGI(TAG, "launcher: %d icon(s) loaded", loaded);
}

static void task(void *arg)
{
    (void)arg;
    luapp_store_mount();
    load_icons();
    if (lock()) {
        luapp_perf_hook();
        unlock();
    }
    for (;;) {
        /* 1. A long-press or Muse's stop: unload before anything else. */
        int req = atomic_exchange(&s_stop_req, STOP_NONE);
        if (req && V.L) stop_app(NULL, STOP_WHY[req]);
        /* 2. Messages (Muse's commands, taps, replies) before frames: a busy
         *    animation never keeps them waiting behind its timers. */
        msg_t *m = NULL;
        for (int i = 0; i < MSG_BURST && xQueueReceive(s_queue, &m, 0) == pdTRUE; i++) {
            if (!m) continue;
            handle(m);
            free(m->data);
            free(m);
        }
        /* 3. Off the Apps screen for 30 s: unload. Then what the board's hooks
         *    flagged (muse.ask's answer, the button, alarms) and keep-awake. */
        away_check();
        if (atomic_exchange(&s_icons_wanted, false)) load_icons();
        int64_t dev_next = luapp_dev_tick();
        luapp_shot_expire();
        luapp_perf_tick();
        /* 4. Timers, paced: never back to back. After a round that took T, wait
         *    at least max(5 ms, T/3), so the screen, Muse's link and voice keep
         *    a quarter of the time, and a late frame is skipped, not caught up. */
        int64_t now = now_us(), next = INT64_MAX;
        if (now >= V.timers_ok_at) {
            next = run_timers();
            int64_t busy = now_us() - now;
            if (busy > 1000) V.timers_ok_at = now_us() + (busy / 3 > MIN_IDLE_US ? busy / 3 : MIN_IDLE_US);
        } else {
            next = V.timers_ok_at;
        }
        if (next < V.timers_ok_at) next = V.timers_ok_at;
        if (dev_next < next) next = dev_next;
        int64_t wait_us = next == INT64_MAX ? 1000000 : next - now_us();
        if (wait_us < 0) wait_us = 0;
        if (wait_us > 1000000) wait_us = 1000000;   /* re-check visibility each second */
        /* Sleep until then, or until a message or a stop arrives: every post
         * and every stop request notifies the task (counted, so one landing
         * between the check and the wait still wakes it). */
        if (atomic_load(&s_stop_req) || uxQueueMessagesWaiting(s_queue)) continue;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((wait_us + 999) / 1000));   /* round up: no spinning */
    }
}

/* ---- the outside ---- */

void luapp_vm_start(void)
{
    if (s_task) return;
    if (s_forced_rtc.magic == FORCED_MAGIC) {   /* no app starts on its own: just report it */
        s_forced_rtc.id[LUAPP_ID_MAX] = 0;
        strlcpy(s_forced_id, s_forced_rtc.id, sizeof(s_forced_id));
        ESP_LOGW(TAG, "booted after a forced stop of %s", s_forced_id);
    }
    s_forced_rtc.magic = 0;
    s_queue = xQueueCreate(16, sizeof(msg_t *));
    if (!s_queue) return;
    if (xTaskCreatePinnedToCoreWithCaps(task, "luapp", TASK_STACK, NULL, 3, &s_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "couldn't start the Lua task");
    }
    /* Above the Lua task, so a busy app can't keep it from answering. */
    if (xTaskCreatePinnedToCoreWithCaps(stop_watch, "luapp_stopwd", 6144, NULL, 4, &s_watch, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_watch = NULL;
    }
}

static msg_t *new_msg(uint8_t type)
{
    msg_t *m = heap_caps_calloc(1, sizeof(*m), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (m) m->type = type;
    return m;
}

static bool post(msg_t *m)
{
    if (!s_queue || xQueueSend(s_queue, &m, 0) != pdTRUE) {
        free(m->data);
        free(m);
        return false;
    }
    if (s_task) xTaskNotifyGive(s_task);   /* wake the task's paced sleep */
    return true;
}

void luapp_vm_tap(int widget, uint32_t gen, int x, int y)
{
    if (widget == -2 && s_task && (gen & 0xffffff) == (atomic_load(&s_live_gen) & 0xffffff)) {
        atomic_store(&s_stop_req, STOP_LONG_PRESS);   /* leave now, ahead of queued frames */
        xTaskNotifyGive(s_task);
    }
    msg_t *m = new_msg(M_TAP);
    if (!m) return;
    m->widget = widget;
    m->gen = gen & 0xffffff;
    m->x = x;
    m->y = y;
    post(m);
}

uint32_t luapp_vm_gen(void) { return V.gen; }

void luapp_vm_run(const char *id)
{
    msg_t *m = new_msg(M_RUN);
    if (!m) return;
    strlcpy(m->id, id, sizeof(m->id));
    post(m);
}

void luapp_set_reply(muse_apps_reply_fn fn) { s_reply = fn; }

/* ---- commands (Noise session task) ---- */

extern const char luapp_api_md[], luapp_api_gfx_md[], luapp_api_sound_md[], luapp_api_ui_md[], luapp_api_device_md[];

#define ACTIONS "api, api_ui, api_gfx, api_sound, api_device, list, logs, stop, remove, source, screenshot, widgets or perf"

static const char *str_param(const cJSON *p, const char *k)
{
    const cJSON *v = p ? cJSON_GetObjectItemCaseSensitive(p, k) : NULL;
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static bool bool_param(const cJSON *p, const char *k, bool def)
{
    const cJSON *v = p ? cJSON_GetObjectItemCaseSensitive(p, k) : NULL;
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static msg_t *req_msg(uint8_t type, const char *id, const char *request_id, uint64_t sgen)
{
    msg_t *m = new_msg(type);
    if (!m) return NULL;
    if (id) strlcpy(m->id, id, sizeof(m->id));
    if (request_id) {
        m->has_req = true;
        strlcpy(m->req_id, request_id, sizeof(m->req_id));
        m->sgen = sgen;
    }
    return m;
}

static cJSON *async_or_busy(msg_t *m)
{
    if (!m) return apps_error("out_of_memory", "no memory for the request");
    if (!post(m)) return apps_error("busy", "the Lua host is busy; try again");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "_async", true);
    return r;
}

cJSON *luapp_command(const char *command, const cJSON *params, const char *request_id, uint64_t sgen)
{
    if (!s_queue) return apps_error("unavailable", "the Lua host isn't running");
    bool can_reply = request_id && s_reply;
    const char *id = str_param(params, "id");

    if (!strcmp(command, "luapp.install")) {
        const cJSON *code = params ? cJSON_GetObjectItemCaseSensitive(params, "code") : NULL;
        if (!luapp_id_valid(id)) return apps_error("invalid_params", "id: 1 to 12 of a-z, 0-9, _");
        if (!cJSON_IsString(code) || !code->valuestring) return apps_error("invalid_params", "code (a string) is required");
        size_t n = strlen(code->valuestring);
        if (n > PIECE_MAX) return apps_error("invalid_params", "send code in pieces of up to 8000 characters (append=true)");
        const char *name = str_param(params, "name");
        if (name && !luapp_name_valid(name)) return apps_error("invalid_params", "name: 1 to 16 plain ASCII characters");
        if (!can_reply) return apps_error("unavailable", "no reply channel");
        if (!luapp_store_mounted()) return apps_error("storage_error", "the app storage isn't mounted");
        bool append = bool_param(params, "append", false), done = bool_param(params, "done", true);
        bool run = bool_param(params, "run", true);
        /* Pieces go to storage here, on Muse's command task, not through the Lua
         * task: uploading a new version never waits for a running app. */
        luapp_meta_t existing;
        if (!append && !luapp_store_find(id, &existing) && luapp_store_count() >= LUAPP_MAX_APPS) {
            return apps_error("full", "64 apps installed: remove one first (luapp.admin action remove)");
        }
        size_t total = 0;
        if (!luapp_store_tmp_write(id, code->valuestring, n, append, &total)) {
            return apps_error("storage_error", "couldn't write the app (storage full?)");
        }
        if (total > LUAPP_CODE_MAX) {
            luapp_store_tmp_discard(id);
            return apps_error("too_large", "apps are limited to 64 KB of source");
        }
        if (!done) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "id", id);
            cJSON_AddNumberToObject(p, "received", (double)total);
            return apps_ok(p);
        }
        /* The last piece: the Lua task compiles, commits and (with run) starts
         * it. Replacing the running app, or starting this one, stops the
         * running one now, ahead of whatever it's doing. */
        char running[LUAPP_ID_MAX + 1];
        taskENTER_CRITICAL(&s_log_mux);
        strlcpy(running, s_running, sizeof(running));
        taskEXIT_CRITICAL(&s_log_mux);
        msg_t *m = req_msg(M_INSTALL, id, request_id, sgen);
        if (!m) return apps_error("out_of_memory", "no memory for the request");
        m->len = total;
        m->append = append;
        m->done = true;
        m->run = run;
        if (running[0] && (run || !strcmp(running, id))) {
            atomic_store(&s_stop_req, STOP_REPLACED);
            if (s_task) xTaskNotifyGive(s_task);
        }
        if (name) {
            m->has_name = true;
            strlcpy(m->name, name, sizeof(m->name));
        }
        const cJSON *color = cJSON_GetObjectItemCaseSensitive(params, "color");
        if (cJSON_IsNumber(color)) {
            m->has_color = true;
            m->color = (uint32_t)color->valuedouble & 0xffffff;
        } else if (cJSON_IsString(color) && color->valuestring) {
            m->has_color = true;
            m->color = (uint32_t)strtoul(color->valuestring + (color->valuestring[0] == '#'), NULL, color->valuestring[0] == '#' ? 16 : 0) & 0xffffff;
        }
        return async_or_busy(m);
    }
    if (!strcmp(command, "luapp.run")) {
        if (!luapp_id_valid(id)) return apps_error("invalid_params", "id is required");
        if (!can_reply) return apps_error("unavailable", "no reply channel");
        msg_t *m = req_msg(M_RUN, id, request_id, sgen);
        if (m && s_running[0]) {   /* the running app stops now, not after its frame */
            atomic_store(&s_stop_req, STOP_REPLACED);
            if (s_task) xTaskNotifyGive(s_task);
        }
        return async_or_busy(m);
    }
    if (!strcmp(command, "luapp.send")) {
        /* To the running app's app.on_message(data); its return value comes back. */
        if (!can_reply) return apps_error("unavailable", "no reply channel");
        if (id && !luapp_id_valid(id)) return apps_error("invalid_params", "id: an app id");
        /* data: JSON text (parsed), plain text, or any JSON value. */
        const cJSON *data = params ? cJSON_GetObjectItemCaseSensitive(params, "data") : NULL;
        cJSON *parsed = cJSON_IsString(data) && data->valuestring ? cJSON_Parse(data->valuestring) : NULL;
        char *json = parsed ? cJSON_PrintUnformatted(parsed) : data ? cJSON_PrintUnformatted(data) : NULL;
        cJSON_Delete(parsed);
        if (json && strlen(json) > 8192) {
            cJSON_free(json);
            return apps_error("too_large", "data is limited to 8 KB of JSON");
        }
        msg_t *m = req_msg(M_SEND, id, request_id, sgen);
        if (m && json) {
            size_t n = strlen(json);
            m->data = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (m->data) memcpy(m->data, json, n + 1);
        }
        cJSON_free(json);
        return async_or_busy(m);
    }
    if (!strcmp(command, "luapp.admin")) {
        const char *action = str_param(params, "action");
        if (!action) return apps_error("invalid_params", "action: " ACTIONS);
        /* The reference in parts, each well inside one reply. */
        const char *doc = !strcmp(action, "api") ? luapp_api_md : !strcmp(action, "api_gfx") ? luapp_api_gfx_md
                        : !strcmp(action, "api_sound") ? luapp_api_sound_md : !strcmp(action, "api_ui") ? luapp_api_ui_md
                        : !strcmp(action, "api_device") ? luapp_api_device_md : NULL;
        if (doc) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "api", doc);
            return apps_ok(p);
        }
        if (!strcmp(action, "list")) {
            luapp_meta_t *apps = heap_caps_malloc(sizeof(luapp_meta_t) * LUAPP_MAX_APPS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!apps) return apps_error("out_of_memory", "no memory for the list");
            int n = luapp_store_list(apps, LUAPP_MAX_APPS);
            cJSON *p = cJSON_CreateObject();
            cJSON *arr = cJSON_AddArrayToObject(p, "apps");
            for (int i = 0; i < n; i++) {
                cJSON *a = cJSON_CreateObject();
                char c[10];
                snprintf(c, sizeof(c), "0x%06lx", (unsigned long)apps[i].color);
                cJSON_AddStringToObject(a, "id", apps[i].id);
                cJSON_AddStringToObject(a, "name", apps[i].name);
                cJSON_AddStringToObject(a, "color", c);
                cJSON_AddNumberToObject(a, "bytes", apps[i].size);
                cJSON_AddItemToArray(arr, a);
            }
            free(apps);
            char running[LUAPP_ID_MAX + 1];
            taskENTER_CRITICAL(&s_log_mux);
            strlcpy(running, s_running, sizeof(running));
            taskEXIT_CRITICAL(&s_log_mux);
            if (running[0]) cJSON_AddStringToObject(p, "running", running);
            else cJSON_AddNullToObject(p, "running");
            cJSON_AddBoolToObject(p, "storage", luapp_store_mounted());
            cJSON_AddNumberToObject(p, "max_apps", LUAPP_MAX_APPS);
            uint32_t total_kb = 0, free_kb = 0;
            if (luapp_store_space(&total_kb, &free_kb)) {
                cJSON_AddNumberToObject(p, "storage_kb", total_kb);
                cJSON_AddNumberToObject(p, "storage_free_kb", free_kb);
            }
            last_stop_t ls;
            taskENTER_CRITICAL(&s_log_mux);
            ls = s_last_stop;
            taskEXIT_CRITICAL(&s_log_mux);
            if (ls.id[0]) cJSON_AddItemToObject(p, "last_unload", unload_json(&ls));   /* freed, left behind */
            if (s_forced_id[0]) cJSON_AddStringToObject(p, "forced_reset", s_forced_id);   /* this boot followed one */
            return apps_ok(p);
        }
        if (!strcmp(action, "logs")) {
            char *log = heap_caps_malloc(LOG_MAX + ERR_MAX + 2 * (LUAPP_ID_MAX + 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!log) return apps_error("out_of_memory", "no memory");
            char *err = log + LOG_MAX + 1, *app_id = err + ERR_MAX + 1, *running = app_id + LUAPP_ID_MAX + 1;
            taskENTER_CRITICAL(&s_log_mux);
            memcpy(log, s_log, s_log_len + 1);
            strlcpy(err, s_error, ERR_MAX + 1);
            strlcpy(app_id, s_log_id, LUAPP_ID_MAX + 1);
            strlcpy(running, s_running, LUAPP_ID_MAX + 1);
            taskEXIT_CRITICAL(&s_log_mux);
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "app", app_id);
            cJSON_AddBoolToObject(p, "running", running[0] != 0);
            cJSON_AddStringToObject(p, "log", log);
            if (err[0]) cJSON_AddStringToObject(p, "error", err);
            else cJSON_AddNullToObject(p, "error");
            free(log);
            return apps_ok(p);
        }
        if (!strcmp(action, "stop")) {
            /* Jumps the queue: the running callback ends at once and the task
             * unloads the app before anything else, however busy it is. The
             * answer comes once it's unloaded (what it freed), or after 3 s
             * as an error; with force=true, a reboot instead. */
            bool force = bool_param(params, "force", false);
            char running[LUAPP_ID_MAX + 1];
            taskENTER_CRITICAL(&s_log_mux);
            strlcpy(running, s_running, sizeof(running));
            bool busy = s_pend_stop.pending;
            taskEXIT_CRITICAL(&s_log_mux);
            if (!running[0]) {
                cJSON *p = cJSON_CreateObject();
                cJSON_AddNullToObject(p, "running");
                return apps_ok(p);
            }
            if (busy) return apps_error("busy", "a stop is already under way; its answer is coming");
            if (can_reply) {
                taskENTER_CRITICAL(&s_log_mux);
                s_pend_stop.pending = true;
                s_pend_stop.force = force;
                strlcpy(s_pend_stop.req_id, request_id, sizeof(s_pend_stop.req_id));
                s_pend_stop.sgen = sgen;
                strlcpy(s_pend_stop.id, running, sizeof(s_pend_stop.id));
                taskEXIT_CRITICAL(&s_log_mux);
            }
            atomic_store(&s_stop_req, STOP_MUSE);
            if (s_task) xTaskNotifyGive(s_task);
            if (!can_reply) return apps_ok(NULL);
            if (s_watch) xTaskNotifyGive(s_watch);
            else ESP_LOGW(TAG, "no stop watcher: this stop can't time out or be forced");
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "_async", true);
            return r;
        }
        if (!strcmp(action, "remove")) {
            if (!luapp_id_valid(id)) return apps_error("invalid_params", "id is required");
            if (!can_reply) return apps_error("unavailable", "no reply channel");
            return async_or_busy(req_msg(M_REMOVE, id, request_id, sgen));
        }
        if (!strcmp(action, "source")) {
            if (!luapp_id_valid(id)) return apps_error("invalid_params", "id is required");
            if (!can_reply) return apps_error("unavailable", "no reply channel");
            msg_t *m = req_msg(M_SOURCE, id, request_id, sgen);
            const cJSON *off = cJSON_GetObjectItemCaseSensitive(params, "offset");
            if (m && cJSON_IsNumber(off) && off->valuedouble > 0) m->offset = (size_t)off->valuedouble;
            return async_or_busy(m);
        }
        if (!strcmp(action, "screenshot")) {
            /* The screen as a PNG, 12 KB (16 KB of base64) per reply: offset 0
             * takes it, then offset = next until next is null. */
            if (!can_reply) return apps_error("unavailable", "no reply channel");
            msg_t *m = req_msg(M_SHOT, NULL, request_id, sgen);
            const cJSON *off = cJSON_GetObjectItemCaseSensitive(params, "offset");
            if (m && cJSON_IsNumber(off) && off->valuedouble > 0) m->offset = (size_t)off->valuedouble;
            if (m) m->x = bool_param(params, "full", false) ? 1 : 2;
            return async_or_busy(m);
        }
        if (!strcmp(action, "widgets")) {
            if (!can_reply) return apps_error("unavailable", "no reply channel");
            return async_or_busy(req_msg(M_WIDGETS, NULL, request_id, sgen));
        }
        if (!strcmp(action, "perf")) {
            /* The last second: screen fps, ms and KB per frame; the app's Lua and drawing. */
            cJSON *p = luapp_perf_json();
            char id[LUAPP_ID_MAX + 1];
            taskENTER_CRITICAL(&s_log_mux);
            strlcpy(id, s_running, sizeof(id));
            taskEXIT_CRITICAL(&s_log_mux);
            if (id[0]) cJSON_AddStringToObject(p, "app", id);
            return apps_ok(p);
        }
        return apps_error("invalid_params", "action: " ACTIONS);
    }
    return apps_error("unsupported", "unknown luapp command");
}
