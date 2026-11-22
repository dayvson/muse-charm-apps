/*
 * The dual-time pocket watch: a pre-rendered dial (watch_assets.c, drawn by
 * firmware/tools/render_watch.py), LVGL-rotated hands, and two zones Muse sets
 * with watch.* commands. Local time drives the main hands, the date and the
 * small seconds; work time the copper skeleton hand.
 *
 * Threads: commands arrive on the Noise session task and only swap the
 * settings under s_mux; the UI task's timer redraws and saves them to NVS
 * (as the settings screen does), so the session task never touches flash.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#include "muse_apps_priv.h"
#include "watch_assets.h"
#include "watch_core.h"

static const char *TAG = "watch";

#define NVS_NS "museapps"
#define NVS_KEY "watch"

/* Layout, as render_watch.py draws the dial (offsets from its centre). */
#define DIAL_PX 466
#define WIN_X 112          /* WORK/LOCAL windows */
#define ICON_DX (-35)      /* sun/moon centre, from its window's */
#define CODE_DX 12         /* city code centre, from its window's */
#define DATE_DY (-90)
#define SUB_DY 100         /* small seconds */
#define IVORY 0xe8e2d2
#define COPPER 0xec8a5c
#define ERR_MAX 640        /* watch.set_zone's longest error lists every built-in code */

enum { LOCAL, WORK };

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static watch_cfg_t s_cfg;   /* under s_mux */
static uint32_t s_version;                     /* under s_mux; bumped by each change */
static atomic_bool s_loaded;

static const char *const WDAY[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

typedef struct {
    lv_obj_t *date, *code[2], *icon[2];
    lv_obj_t *sec, *work, *hour, *min;
    lv_image_dsc_t dial, dsc_sec, dsc_work, dsc_hour, dsc_min, dsc_cap, dsc_sun, dsc_moon;
    muse_tz_t tz[2];
    uint32_t version, saved;
    int64_t shown_sec;
    int8_t day[2];      /* -1 unknown */
    bool synced;
} watch_ui_t;

EXT_RAM_BSS_ATTR static watch_ui_t u;

/* ---- settings, shared with the session task ---- */

static void cfg_get(watch_cfg_t *out, uint32_t *version)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_cfg;
    if (version) *version = s_version;
    taskEXIT_CRITICAL(&s_mux);
}

static void cfg_put(const watch_cfg_t *in)
{
    taskENTER_CRITICAL(&s_mux);
    s_cfg = *in;
    s_version++;
    taskEXIT_CRITICAL(&s_mux);
}

static void cfg_load(void)
{
    watch_cfg_t c;
    size_t len = sizeof(c);
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, NVS_KEY, &c, &len) == ESP_OK && len == sizeof(c) && watch_cfg_valid(&c);
        nvs_close(h);
    }
    if (!ok) watch_cfg_default(&c);
    cfg_put(&c);
    ESP_LOGI(TAG, "zones: work %s (%s), local %s (%s)%s", c.work.label, c.work.tz, c.local.label, c.local.tz,
             ok ? "" : ", defaults");
}

static void cfg_save(const watch_cfg_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, c, sizeof(*c));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "couldn't save the zones: %s", esp_err_to_name(err));
}

/* ---- drawing ---- */

static void argb_dsc(lv_image_dsc_t *d, const watch_image_t *a)
{
    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_ARGB8888;
    d->header.w = a->w;
    d->header.h = a->h;
    d->header.stride = a->w * 4;
    d->data_size = (uint32_t)a->w * a->h * 4;
    d->data = a->px;
}

static lv_obj_t *hand(lv_obj_t *tile, lv_image_dsc_t *dsc, const watch_image_t *a, int cx, int cy)
{
    argb_dsc(dsc, a);
    lv_obj_t *img = lv_image_create(tile);
    lv_image_set_src(img, dsc);
    lv_image_set_pivot(img, a->pivot_x, a->pivot_y);
    lv_image_set_antialias(img, true);
    lv_obj_set_pos(img, cx - a->pivot_x, cy - a->pivot_y);
    return img;
}

static lv_obj_t *label(lv_obj_t *tile, const lv_font_t *font, uint32_t color, int dx, int dy)
{
    lv_obj_t *l = lv_label_create(tile);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_CENTER, dx, dy);
    return l;
}

static void set_text(lv_obj_t *l, const char *s)
{
    if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}

static void set_day(int which, int day)
{
    if (u.day[which] == day) return;
    u.day[which] = (int8_t)day;
    if (day < 0) {
        lv_obj_add_flag(u.icon[which], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_image_set_src(u.icon[which], day ? &u.dsc_sun : &u.dsc_moon);
    lv_obj_remove_flag(u.icon[which], LV_OBJ_FLAG_HIDDEN);
}

/* LVGL rotation is in tenths of a degree, clockwise. */
static int32_t hour_angle(const muse_civil_t *t)
{
    return ((t->hour % 12) * 3600 + t->min * 60 + t->sec) / 12;
}

static void draw(int64_t now)
{
    muse_civil_t l, h;
    bool synced = now >= WATCH_CLOCK_VALID;
    if (synced) {
        muse_tz_civil(now + muse_tz_offset(&u.tz[LOCAL], now, NULL), &l);
        muse_tz_civil(now + muse_tz_offset(&u.tz[WORK], now, NULL), &h);
        char buf[12];
        snprintf(buf, sizeof(buf), "%s %d", WDAY[l.wday], l.day);
        set_text(u.date, buf);
        set_day(LOCAL, watch_daytime(l.hour));
        set_day(WORK, watch_daytime(h.hour));
    } else {
        /* Until SNTP answers: the hands at rest, as in a shop window. */
        l = (muse_civil_t){.hour = 10, .min = 9, .sec = 0};
        h = (muse_civil_t){.hour = 1, .min = 9, .sec = 0};
        set_text(u.date, "SYNC");
        set_day(LOCAL, -1);
        set_day(WORK, -1);
    }
    if (synced != u.synced) {
        u.synced = synced;
        if (synced) ESP_LOGI(TAG, "clock set: local %02d:%02d:%02d", l.hour, l.min, l.sec);
    }
    lv_image_set_rotation(u.sec, l.sec * 60);
    lv_image_set_rotation(u.min, l.min * 60 + l.sec);
    lv_image_set_rotation(u.hour, hour_angle(&l));
    lv_image_set_rotation(u.work, hour_angle(&h));
}

static void apply_cfg(const watch_cfg_t *c)
{
    muse_tz_parse(c->local.tz, &u.tz[LOCAL]);   /* validated on the way in */
    muse_tz_parse(c->work.tz, &u.tz[WORK]);
    set_text(u.code[LOCAL], c->local.label);
    set_text(u.code[WORK], c->work.label);
    u.day[LOCAL] = u.day[WORK] = -2;           /* re-pick the icons */
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    watch_cfg_t c;
    uint32_t v;
    cfg_get(&c, &v);
    if (v != u.version) {
        u.version = v;
        apply_cfg(&c);
        u.shown_sec = -1;
        if (v != u.saved) {
            cfg_save(&c);
            u.saved = v;
        }
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec != u.shown_sec) {
        u.shown_sec = tv.tv_sec;
        draw(tv.tv_sec);
    }
}

void watch_build(lv_obj_t *tile, int w, int h)
{
    cfg_load();
    cfg_get(&(watch_cfg_t){0}, &u.saved);      /* what's loaded needn't be written back */

    lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(tile, 0, 0);
    int cx = w / 2, cy = h / 2;

    /* 256 colours, drawn by LVGL from flash a row at a time: no copy in RAM. */
    u.dial.header.magic = LV_IMAGE_HEADER_MAGIC;
    u.dial.header.cf = LV_COLOR_FORMAT_I8;
    u.dial.header.w = watch_dial.w;
    u.dial.header.h = watch_dial.h;
    u.dial.header.stride = watch_dial.w;
    u.dial.data_size = watch_dial.size;
    u.dial.data = watch_dial.data;
    lv_obj_t *dial = lv_image_create(tile);
    lv_image_set_src(dial, &u.dial);
    lv_obj_align(dial, LV_ALIGN_CENTER, 0, 0);

    u.date = label(tile, &lv_font_montserrat_16, IVORY, 0, DATE_DY);
    u.code[LOCAL] = label(tile, &lv_font_montserrat_20, IVORY, -WIN_X + CODE_DX, 0);
    u.code[WORK] = label(tile, &lv_font_montserrat_20, COPPER, WIN_X + CODE_DX, 0);
    argb_dsc(&u.dsc_sun, &watch_sun);
    argb_dsc(&u.dsc_moon, &watch_moon);
    for (int i = 0; i < 2; i++) {
        u.icon[i] = lv_image_create(tile);
        lv_image_set_src(u.icon[i], &u.dsc_moon);
        lv_obj_align(u.icon[i], LV_ALIGN_CENTER, (i == WORK ? WIN_X : -WIN_X) + ICON_DX, 0);
        lv_obj_add_flag(u.icon[i], LV_OBJ_FLAG_HIDDEN);
        u.day[i] = -1;
    }

    /* Bottom to top: small seconds, work, hour, minute, then the cap. */
    u.sec = hand(tile, &u.dsc_sec, &watch_second, cx, cy + SUB_DY);
    u.work = hand(tile, &u.dsc_work, &watch_work, cx, cy);
    u.hour = hand(tile, &u.dsc_hour, &watch_hour, cx, cy);
    u.min = hand(tile, &u.dsc_min, &watch_minute, cx, cy);
    argb_dsc(&u.dsc_cap, &watch_cap);
    lv_obj_t *cap = lv_image_create(tile);
    lv_image_set_src(cap, &u.dsc_cap);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, 0);

    u.version = 0;
    u.shown_sec = -1;
    atomic_store(&s_loaded, true);
    tick(NULL);
    lv_timer_create(tick, 200, NULL);
}

int32_t watch_local_offset(int64_t utc)
{
    watch_cfg_t c;
    cfg_get(&c, NULL);
    muse_tz_t tz;
    return muse_tz_parse(c.local.tz, &tz) ? muse_tz_offset(&tz, utc, NULL) : 0;
}

/* ---- commands (Noise session task) ---- */

static int64_t now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

cJSON *watch_command(const char *command, const cJSON *params)
{
    if (!atomic_load(&s_loaded)) return apps_error("unavailable", "the watch is starting up");
    watch_cfg_t c;
    cfg_get(&c, NULL);
    if (!strcmp(command, "watch.get")) {
        return apps_ok(watch_cfg_json(&c, now_s()));
    }
    if (!strcmp(command, "watch.swap")) {
        watch_cfg_swap(&c);
        cfg_put(&c);
        ESP_LOGI(TAG, "swapped: work %s, local %s", c.work.label, c.local.label);
        return apps_ok(watch_cfg_json(&c, now_s()));
    }
    if (!strcmp(command, "watch.set_zone")) {
        char *err = malloc(ERR_MAX);
        if (!err) return apps_error("out_of_memory", "no memory for the request");
        bool ok = watch_cfg_set_zone(&c, params, err, ERR_MAX);
        cJSON *fail = ok ? NULL : apps_error("invalid_params", err);
        free(err);
        if (fail) return fail;
        cfg_put(&c);
        ESP_LOGI(TAG, "zones: work %s (%s), local %s (%s)", c.work.label, c.work.tz, c.local.label, c.local.tz);
        return apps_ok(watch_cfg_json(&c, now_s()));
    }
    return apps_error("unsupported", "unknown watch command");
}
