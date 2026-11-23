/*
 * The wind tracker tile: a spot (tap its name to cycle), the wind now (arrow
 * pointing downwind, knots coloured on the wind scale, gusts and direction,
 * a kite rating), and a chart for today, tomorrow (hourly, 6 AM-9 PM) or
 * the week (daily max), with the best session window underneath. Layout
 * matches firmware/tools/render_wind.py.
 *
 * Spots: Crandon Park and Stiltsville (Key Biscayne, Miami), plus one Muse
 * sets with wind.set_spot ("check Virginia Key"), kept in NVS. Forecasts come
 * from Open-Meteo on the sea grid cell, fetched on the apps' fetch task while
 * the tile is up (muse_apps_net_ok()), every 15 minutes at most.
 */

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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "apps_net.h"
#include "muse_apps_priv.h"
#include "wind_assets.h"
#include "wind_data.h"

static const char *TAG = "wind";

#define SPOTS 3                 /* two built in, one Muse sets */
#define CUSTOM 2
#define STALE_US (15 * 60 * 1000000LL)
#define RETRY_US (60 * 1000000LL)
#define NVS_NS "museapps"
#define NVS_KEY "wind"
#define CLOCK_VALID 1704067200   /* 2024: before this the clock isn't set */

/* Geometry (screen coordinates on the 466 px circle). */
#define NAME_Y 62
#define DOTS_Y 86
#define RING_X 160
#define RING_Y 140
#define RING_R 44
#define NUM_X 218
#define NUM_Y 128
#define SUB_Y 176
#define BADGE_Y 212
#define CHART_X 53
#define CHART_Y 236
#define CHART_W 360
#define CHART_H 120
#define CHIP_Y 390
#define CHIP_W 92
#define CHIP_H 30
#define CHIP_GAP 8
#define FOOT_Y 430
#define BAR_BASE 84             /* chart rows, canvas coordinates */
#define BAR_TOP 20
#define ARROW_Y 95
#define LABEL_Y 111

#define TEAL 0x27d3a2
#define DIM 0x8e8e93
#define RING_COLOR 0x1e2f3a
#define TICK_COLOR 0x46596a
#define DOT_OFF 0x3a3a3c

#if CONFIG_LV_FONT_MONTSERRAT_48
#define FONT_BIG (&lv_font_montserrat_48)
#else
#define FONT_BIG (&lv_font_montserrat_28)
#endif

enum { VIEW_TODAY, VIEW_TMRW, VIEW_WEEK, VIEWS };
static const char *const VIEW_LABELS[VIEWS] = {"TODAY", "TMRW", "WEEK"};
static const char *const VIEW_NAMES[VIEWS] = {"today", "tomorrow", "week"};

typedef struct {
    char name[WIND_NAME_MAX + 1];
    double lat, lon;
} spot_t;

typedef struct {
    uint32_t magic;
    spot_t spot;
    bool set;
} custom_cfg_t;
#define CUSTOM_MAGIC 0x57494e31u   /* "WIN1" */

typedef struct {
    bool ok, failed, pending;
    uint32_t version;
    int64_t fetched_us, tried_us;
} info_t;

/* Shared with the fetch task and commands: under s_lock. */
EXT_RAM_BSS_ATTR static wind_fc_t s_fc[SPOTS];
EXT_RAM_BSS_ATTR static info_t s_info[SPOTS];
static spot_t s_spots[SPOTS] = {
    {"Crandon Park", 25.7120, -80.1535},
    {"Stiltsville", 25.6529, -80.1754},
};
static bool s_custom_set;
static uint32_t s_spot_gen[SPOTS];   /* a changed spot drops results fetched for the old one */
static uint32_t s_custom_version;    /* saved to NVS by the UI task */
static SemaphoreHandle_t s_lock;
static atomic_int s_req_spot = -1, s_req_view = -1;

typedef struct {
    lv_obj_t *name, *dots[SPOTS], *arrow, *num, *kn, *sub, *badge, *badge_lbl, *chart, *chips[VIEWS],
        *chip_lbl[VIEWS], *foot;
    lv_draw_buf_t *chart_buf;
    lv_image_dsc_t dsc_arrow, dsc_kite;
    wind_fc_t cur;
    int sel, view;
    int shown_sel, shown_view;
    uint32_t shown_version, saved_custom;
    bool shown_ok, shown_custom;
    int64_t shown_hour;
} wind_ui_t;

EXT_RAM_BSS_ATTR static wind_ui_t u;

/* ---- helpers ---- */

static int dy(int y) { return y - 233; }

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static void set_text(lv_obj_t *l, const char *s)
{
    if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}

static lv_obj_t *button(lv_obj_t *parent, int w, int h, int cx, int cy, lv_event_cb_t cb, intptr_t id)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(b, LV_ALIGN_CENTER, cx, dy(cy));
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)id);
    return b;
}

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

static int64_t wall_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

static uint32_t scale(uint32_t rgb, float k)
{
    return ((uint32_t)(((rgb >> 16) & 255) * k) << 16) | ((uint32_t)(((rgb >> 8) & 255) * k) << 8) |
           (uint32_t)((rgb & 255) * k);
}

/* ---- store (any task) ---- */

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static int spot_count_locked(void) { return s_custom_set ? SPOTS : CUSTOM; }

static void job(uint32_t arg)
{
    int spot = (int)(arg & 0xff);
    uint32_t gen = arg >> 8;
    lock();
    spot_t sp = s_spots[spot];
    unlock();
    char url[400];
    wind_url(url, sizeof(url), sp.lat, sp.lon);
    wind_fc_t *f = heap_caps_malloc(sizeof(*f), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const char *body;
    size_t len;
    int status = f ? apps_net_get(url, &body, &len) : 0;
    bool ok = status == 200 && wind_parse(body, f);
    int64_t now = esp_timer_get_time();
    lock();
    info_t *in = &s_info[spot];
    in->pending = false;
    in->tried_us = now;
    in->failed = !ok;
    if (ok && (s_spot_gen[spot] & 0xffffff) == gen) {
        memcpy(&s_fc[spot], f, sizeof(*f));
        in->ok = true;
        in->fetched_us = now;
        in->version++;
    }
    unlock();
    if (ok) {
        ESP_LOGI(TAG, "%s: %.0f kn %s, %d hours", sp.name, f->now_kn, wind_compass(f->now_dir), f->nh);
    } else if (status >= 0) {
        ESP_LOGW(TAG, "%s: failed (HTTP %d)", sp.name, status);
    }
    heap_caps_free(f);
}

static void request(int spot)
{
    lock();
    info_t *in = &s_info[spot];
    if (!in->pending) {
        in->pending = apps_net_submit(job, (s_spot_gen[spot] & 0xffffff) << 8 | (uint32_t)spot);
    }
    unlock();
}

static void want(int spot)
{
    int64_t now = esp_timer_get_time();
    lock();
    info_t in = s_info[spot];
    unlock();
    if (in.pending) return;
    if (in.ok && now - in.fetched_us < STALE_US) return;
    if (in.failed && now - in.tried_us < RETRY_US) return;
    request(spot);
}

static void custom_load(void)
{
    custom_cfg_t c;
    size_t len = sizeof(c);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    bool ok = nvs_get_blob(h, NVS_KEY, &c, &len) == ESP_OK && len == sizeof(c) && c.magic == CUSTOM_MAGIC && c.set &&
              memchr(c.spot.name, 0, sizeof(c.spot.name));
    nvs_close(h);
    if (!ok) return;
    lock();
    s_spots[CUSTOM] = c.spot;
    s_custom_set = true;
    unlock();
    ESP_LOGI(TAG, "Muse's spot: %s (%.4f, %.4f)", c.spot.name, c.spot.lat, c.spot.lon);
}

static void custom_save(void)
{
    custom_cfg_t c = {.magic = CUSTOM_MAGIC};
    lock();
    c.spot = s_spots[CUSTOM];
    c.set = s_custom_set;
    unlock();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &c, sizeof(c));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "couldn't save Muse's spot: %s", esp_err_to_name(err));
}

/* ---- chart ---- */

static void bar(lv_layer_t *layer, int cx, int w, int top, uint32_t color)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(color);
    r.radius = 3;
    lv_area_t a = {cx - w / 2, top, cx - w / 2 + w - 1, BAR_BASE};
    if (a.y1 > a.y2 - 1) a.y1 = a.y2 - 1;   /* a calm hour still shows a sliver */
    lv_draw_rect(layer, &r, &a);
}

static void text_at(lv_layer_t *layer, int cx, int cy, const char *s, uint32_t color)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = &lv_font_montserrat_14;
    d.color = lv_color_hex(color);
    d.align = LV_TEXT_ALIGN_CENTER;
    d.text = s;
    d.text_local = 1;   /* copied: s is on the stack */
    lv_area_t a = {cx - 24, cy - 8, cx + 24, cy + 9};
    lv_draw_label(layer, &d, &a);
}

/* A small triangle pointing downwind; deg is where the wind comes from. */
static void small_arrow(lv_layer_t *layer, int cx, int cy, int deg, uint32_t color)
{
    if (deg < 0) return;
    float a = (float)(deg + 180) * (float)M_PI / 180.0f;
    float ux = sinf(a), uy = -cosf(a), px = -uy, py = ux;
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = lv_color_hex(color);
    t.opa = LV_OPA_COVER;
    t.p[0].x = (int32_t)lroundf(cx + ux * 6);
    t.p[0].y = (int32_t)lroundf(cy + uy * 6);
    t.p[1].x = (int32_t)lroundf(cx - ux * 5 + px * 4.5f);
    t.p[1].y = (int32_t)lroundf(cy - uy * 5 + py * 4.5f);
    t.p[2].x = (int32_t)lroundf(cx - ux * 5 - px * 4.5f);
    t.p[2].y = (int32_t)lroundf(cy - uy * 5 - py * 4.5f);
    lv_draw_triangle(layer, &t);
}

/* The chart's colours vary (bars by strength, white numbers, grey arrows), so
 * it stays RGB565, kept only near the tile. */
#define CHART_BYTES (CHART_W * CHART_H * 2)

void wind_trim(void)
{
    if (!u.chart_buf || muse_apps_near(MUSE_APP_WIND)) return;
    lv_image_set_src(u.chart, NULL);
    lv_draw_buf_destroy(u.chart_buf);
    u.chart_buf = NULL;
}

/* True when the buffer was just made: the chart needs drawing. Not out of
 * voice's reserve. */
static bool chart_keep(void)
{
    if (!muse_apps_near(MUSE_APP_WIND)) {
        wind_trim();
        return false;
    }
    if (u.chart_buf || !u.chart) return false;
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < CHART_BYTES + MUSE_APPS_PSRAM_RESERVE) return false;
    u.chart_buf = lv_draw_buf_create(CHART_W, CHART_H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (!u.chart_buf) return false;
    lv_canvas_set_draw_buf(u.chart, u.chart_buf);
    return true;
}

static void draw_chart(const wind_fc_t *f, bool have)
{
    lv_draw_buf_t *db = u.chart_buf;
    if (!db) return;   /* away from the tile: drawn when it's back */
    for (int y = 0; y < CHART_H; y++) memset(db->data + y * db->header.stride, 0, CHART_W * 2);
    if (!have) {
        lv_obj_invalidate(u.chart);
        return;
    }
    float sp[24], gu[24];
    int dir[24], n = 0, bw;
    bool past[24] = {false}, number[24] = {false};
    char labels[24][6] = {{0}};
    int64_t now = wall_now();
    if (u.view == VIEW_WEEK) {
        for (int d = 0; d < f->nd && n < 24; d++, n++) {
            sp[n] = isnan(f->ds[d]) ? 0 : f->ds[d];
            gu[n] = isnan(f->dg[d]) ? sp[n] : f->dg[d];
            dir[n] = f->dd[d];
            number[n] = true;
            strlcpy(labels[n], wind_weekday(f, d), sizeof(labels[n]));
        }
        bw = 26;
    } else {
        int idx[24];
        n = wind_day_hours(f, u.view == VIEW_TODAY ? 0 : 1, WIND_DAY_FIRST_HOUR, WIND_DAY_LAST_HOUR, idx, 24);
        for (int k = 0; k < n; k++) {
            int i = idx[k], hr = wind_local_hour(f, i);
            sp[k] = f->hs[i];
            gu[k] = f->hg[i];
            dir[k] = f->hd[i];
            past[k] = u.view == VIEW_TODAY && now >= CLOCK_VALID && f->ht[i] + 3600 <= now;
            number[k] = hr % 2 == 0;
            if (hr % 3 == 0) snprintf(labels[k], sizeof(labels[k]), "%d%c", hr % 12 ? hr % 12 : 12, hr < 12 ? 'a' : 'p');
        }
        bw = 14;
    }
    if (n == 0) {
        lv_obj_invalidate(u.chart);
        return;
    }
    float top = 25;
    for (int k = 0; k < n; k++) top = fmaxf(top, gu[k]);
    float slot = (float)CHART_W / n;

    lv_layer_t layer;
    lv_canvas_init_layer(u.chart, &layer);
    for (int k = 0; k < n; k++) {
        int cx = (int)lroundf(slot * (k + 0.5f));
        uint32_t col = wind_color(sp[k]);
        if (past[k]) col = scale(col, 0.4f);
        int y = BAR_BASE - (int)lroundf(sp[k] / top * (BAR_BASE - BAR_TOP));
        int gy = BAR_BASE - (int)lroundf(gu[k] / top * (BAR_BASE - BAR_TOP));
        bar(&layer, cx, bw, gy, scale(col, 0.32f));   /* gusts: the bar's colour, faint, above it */
        bar(&layer, cx, bw, y, col);
        if (number[k]) {
            char b[8];
            snprintf(b, sizeof(b), "%d", (int)lroundf(sp[k]));
            text_at(&layer, cx, (gy < y ? gy : y) - 10, b, past[k] ? 0x646464 : 0xffffff);
            small_arrow(&layer, cx, ARROW_Y, dir[k], past[k] ? 0x505050 : 0xc8c8c8);
        }
        if (labels[k][0]) text_at(&layer, cx, LABEL_Y, labels[k], DIM);
    }
    lv_canvas_finish_layer(u.chart, &layer);
    lv_obj_invalidate(u.chart);
}

/* ---- the rest of the screen ---- */

static void update_foot(const wind_fc_t *f, bool have, info_t in)
{
    char b[48];
    if (!have) {
        snprintf(b, sizeof(b), "%s", in.failed && !in.pending ? "Can't load the forecast" : "Loading forecast...");
    } else if (u.view == VIEW_WEEK) {
        int d = wind_best_day(f);
        if (d >= 0 && wind_rate(f->ds[d]) >= RATE_MARGINAL) {
            snprintf(b, sizeof(b), "Best day: %s • %.0f kn", wind_weekday(f, d), f->ds[d]);
        } else {
            snprintf(b, sizeof(b), "A light week: no kiteable day");
        }
    } else {
        int h0, h1;
        float lo, hi;
        int64_t from = u.view == VIEW_TODAY ? wall_now() : 0;
        if (wind_best_window(f, u.view == VIEW_TODAY ? 0 : 1, from, &h0, &h1, &lo, &hi)) {
            char a[8], z[8];
            wind_fmt_hour(a, sizeof(a), h0);
            wind_fmt_hour(z, sizeof(z), h1 + 1);
            if ((int)lroundf(lo) == (int)lroundf(hi)) {
                snprintf(b, sizeof(b), "Best %s-%s • %.0f kn", a, z, hi);
            } else {
                snprintf(b, sizeof(b), "Best %s-%s • %.0f-%.0f kn", a, z, lo, hi);
            }
        } else {
            snprintf(b, sizeof(b), "No kiteable wind 7 AM-7 PM");
        }
    }
    set_text(u.foot, b);
}

static void update(bool force)
{
    lock();
    int count = spot_count_locked();
    if (u.sel >= count) u.sel = 0;
    info_t in = s_info[u.sel];
    bool have = in.ok;
    bool changed = force || have != u.shown_ok || in.version != u.shown_version || u.sel != u.shown_sel ||
                   u.view != u.shown_view || s_custom_set != u.shown_custom;
    if (changed && have) memcpy(&u.cur, &s_fc[u.sel], sizeof(u.cur));
    char name[WIND_NAME_MAX + 1];
    strlcpy(name, s_spots[u.sel].name, sizeof(name));
    bool custom_set = s_custom_set;
    unlock();

    /* Today's chart dims hours as they pass: redraw on the hour. */
    int64_t hour = wall_now() / 3600;
    if (!changed && hour == u.shown_hour) return;
    u.shown_hour = hour;
    u.shown_ok = have;
    u.shown_version = in.version;
    u.shown_sel = u.sel;
    u.shown_view = u.view;
    u.shown_custom = custom_set;

    set_text(u.name, name);
    for (int i = 0; i < SPOTS; i++) {
        lv_obj_set_flag(u.dots[i], LV_OBJ_FLAG_HIDDEN, i >= count);
        lv_obj_set_style_bg_color(u.dots[i], lv_color_hex(i == u.sel ? TEAL : DOT_OFF), 0);
    }
    if (have && u.cur.has_now) {
        char b[40];
        snprintf(b, sizeof(b), "%d", (int)lroundf(u.cur.now_kn));
        set_text(u.num, b);
        lv_obj_set_style_text_color(u.num, lv_color_hex(wind_color(u.cur.now_kn)), 0);
        snprintf(b, sizeof(b), "gust %d • %s", (int)lroundf(u.cur.now_gust), wind_compass(u.cur.now_dir));
        set_text(u.sub, b);
        lv_image_set_rotation(u.arrow, ((u.cur.now_dir + 180) % 360) * 10);
        lv_obj_remove_flag(u.arrow, LV_OBJ_FLAG_HIDDEN);
        wind_rating_t r = wind_rate(u.cur.now_kn);
        set_text(u.badge_lbl, wind_rate_label(r));
        lv_obj_set_style_bg_color(u.badge, lv_color_hex(wind_rate_color(r)), 0);
        lv_obj_remove_flag(u.badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        set_text(u.num, "--");
        lv_obj_set_style_text_color(u.num, lv_color_hex(DIM), 0);
        set_text(u.sub, "");
        lv_obj_add_flag(u.arrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(u.badge, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < VIEWS; i++) {
        bool sel = i == u.view;
        lv_obj_set_style_bg_opa(u.chips[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(u.chip_lbl[i], lv_color_hex(sel ? 0x000000 : DIM), 0);
    }
    draw_chart(&u.cur, have);
    update_foot(&u.cur, have, in);
}

/* ---- events and timer ---- */

static void on_name(lv_event_t *e)
{
    (void)e;
    lock();
    int count = spot_count_locked();
    unlock();
    u.sel = (u.sel + 1) % count;
    update(true);
}

static void on_chip(lv_event_t *e)
{
    u.view = (int)(intptr_t)lv_event_get_user_data(e);
    update(true);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    int spot = atomic_exchange(&s_req_spot, -1), view = atomic_exchange(&s_req_view, -1);
    if (spot >= 0) u.sel = spot;
    if (view >= 0) u.view = view;
    lock();
    uint32_t cv = s_custom_version;
    unlock();
    if (cv != u.saved_custom) {
        u.saved_custom = cv;
        custom_save();
    }
    if (muse_apps_visible(MUSE_APP_WIND) && muse_apps_net_ok()) want(u.sel);
    bool fresh = chart_keep();
    update(spot >= 0 || view >= 0 || fresh);
}

void wind_build(lv_obj_t *tile, int w, int h)
{
    (void)w;
    (void)h;
    s_lock = xSemaphoreCreateMutex();
    custom_load();

    lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(tile, 0, 0);

    /* The spot's name; a tap moves to the next spot. */
    lv_obj_t *nb = button(tile, 280, 48, 0, NAME_Y + 8, on_name, 0);
    u.name = label(nb, &lv_font_montserrat_20, 0xffffff);
    lv_obj_align(u.name, LV_ALIGN_CENTER, 0, -8);
    for (int i = 0; i < SPOTS; i++) {
        lv_obj_t *d = lv_obj_create(tile);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(d, LV_ALIGN_CENTER, (i - 1) * 16, dy(DOTS_Y));
        u.dots[i] = d;
    }

    /* Direction ring with N/E/S/W ticks and the downwind arrow. */
    lv_obj_t *ring = lv_obj_create(tile);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 2 * RING_R, 2 * RING_R);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 3, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(RING_COLOR), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(ring, LV_ALIGN_CENTER, RING_X - 233, dy(RING_Y));
    static const int8_t TX[] = {0, 1, 0, -1}, TY[] = {-1, 0, 1, 0};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *t = lv_obj_create(ring);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, TX[i] ? 6 : 2, TX[i] ? 2 : 6);
        lv_obj_set_style_bg_color(t, lv_color_hex(TICK_COLOR), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_align(t, LV_ALIGN_CENTER, TX[i] * (RING_R - 7), TY[i] * (RING_R - 7));
    }
    argb_dsc(&u.dsc_arrow, &wind_arrow);
    u.arrow = lv_image_create(tile);
    lv_image_set_src(u.arrow, &u.dsc_arrow);
    lv_image_set_pivot(u.arrow, wind_arrow.pivot_x, wind_arrow.pivot_y);
    lv_image_set_antialias(u.arrow, true);
    lv_obj_set_pos(u.arrow, RING_X - wind_arrow.pivot_x, RING_Y - wind_arrow.pivot_y);

    /* Knots (coloured on the wind scale) with "kn" beside, then gusts and direction. */
    lv_obj_t *row = lv_obj_create(tile);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(row, LV_ALIGN_LEFT_MID, NUM_X, dy(NUM_Y));
    u.num = label(row, FONT_BIG, DIM);
    u.kn = label(row, &lv_font_montserrat_20, DIM);
    lv_label_set_text(u.kn, "kn");
    lv_obj_set_style_pad_bottom(u.kn, 8, 0);
    u.sub = label(tile, &lv_font_montserrat_16, DIM);
    lv_obj_align(u.sub, LV_ALIGN_LEFT_MID, NUM_X, dy(SUB_Y));

    /* The rating badge: kite icon + label, black on the rating's colour. */
    u.badge = lv_obj_create(tile);
    lv_obj_remove_style_all(u.badge);
    lv_obj_set_size(u.badge, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_radius(u.badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(u.badge, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(u.badge, 12, 0);
    lv_obj_set_flex_flow(u.badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(u.badge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(u.badge, 6, 0);
    lv_obj_remove_flag(u.badge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(u.badge, LV_ALIGN_CENTER, 0, dy(BADGE_Y));
    argb_dsc(&u.dsc_kite, &wind_kite);
    lv_obj_t *kite = lv_image_create(u.badge);
    lv_image_set_src(kite, &u.dsc_kite);
    u.badge_lbl = label(u.badge, &lv_font_montserrat_16, 0x000000);

    u.chart = lv_canvas_create(tile);   /* its buffer comes and goes with chart_keep() */
    lv_obj_set_pos(u.chart, CHART_X, CHART_Y);
    chart_keep();

    for (int i = 0; i < VIEWS; i++) {
        u.chips[i] = button(tile, CHIP_W, CHIP_H, (i - 1) * (CHIP_W + CHIP_GAP), CHIP_Y, on_chip, i);
        lv_obj_set_style_bg_color(u.chips[i], lv_color_hex(TEAL), 0);
        u.chip_lbl[i] = label(u.chips[i], &lv_font_montserrat_16, DIM);
        lv_label_set_text(u.chip_lbl[i], VIEW_LABELS[i]);
        lv_obj_center(u.chip_lbl[i]);
    }
    u.foot = label(tile, &lv_font_montserrat_14, TEAL);
    lv_obj_align(u.foot, LV_ALIGN_CENTER, 0, dy(FOOT_Y));

    u.sel = 0;
    u.view = VIEW_TODAY;
    u.shown_sel = u.shown_view = -1;
    update(true);
    lv_timer_create(tick, 500, NULL);
}

/* ---- commands (Noise session task) ---- */

static int find_spot_locked(const char *what)
{
    int count = spot_count_locked();
    for (int i = 0; what && i < count; i++) {
        if (!strncasecmp(s_spots[i].name, what, strlen(what))) return i;
    }
    return -1;
}

static int find_view(const char *what)
{
    for (int i = 0; what && i < VIEWS; i++) {
        if (!strcasecmp(what, VIEW_NAMES[i]) || !strcasecmp(what, VIEW_LABELS[i])) return i;
    }
    return -1;
}

static bool number_param(const cJSON *params, const char *key, double *out)
{
    const cJSON *v = params ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
    if (cJSON_IsNumber(v)) {
        *out = v->valuedouble;
        return true;
    }
    if (cJSON_IsString(v) && v->valuestring) {
        char *end;
        *out = strtod(v->valuestring, &end);
        return end != v->valuestring && *end == 0;
    }
    return false;
}

static const char *str_param(const cJSON *params, const char *key)
{
    const cJSON *v = params ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
    return cJSON_IsString(v) && v->valuestring && v->valuestring[0] ? v->valuestring : NULL;
}

static cJSON *spot_json_locked(int i)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", s_spots[i].name);
    cJSON_AddNumberToObject(o, "lat", s_spots[i].lat);
    cJSON_AddNumberToObject(o, "lon", s_spots[i].lon);
    const wind_fc_t *f = &s_fc[i];
    if (!s_info[i].ok || !f->has_now) {
        cJSON_AddNullToObject(o, "now");
        return o;
    }
    cJSON *now = cJSON_AddObjectToObject(o, "now");
    cJSON_AddNumberToObject(now, "knots", round(f->now_kn));
    cJSON_AddNumberToObject(now, "gust_knots", round(f->now_gust));
    cJSON_AddNumberToObject(now, "from_deg", f->now_dir);
    cJSON_AddStringToObject(now, "from", wind_compass(f->now_dir));
    cJSON_AddStringToObject(now, "rating", wind_rate_label(wind_rate(f->now_kn)));
    for (int day = 0; day < 2; day++) {
        int h0, h1;
        float lo, hi;
        char key[16], b[40], a[8], z[8];
        snprintf(key, sizeof(key), "best_%s", day ? "tomorrow" : "today");
        if (wind_best_window(f, day, day ? 0 : wall_now(), &h0, &h1, &lo, &hi)) {
            wind_fmt_hour(a, sizeof(a), h0);
            wind_fmt_hour(z, sizeof(z), h1 + 1);
            snprintf(b, sizeof(b), "%s-%s, %.0f-%.0f kn", a, z, lo, hi);
            cJSON_AddStringToObject(o, key, b);
        } else {
            cJSON_AddNullToObject(o, key);
        }
    }
    cJSON_AddNumberToObject(o, "age_min", round((esp_timer_get_time() - s_info[i].fetched_us) / 60e6));
    return o;
}

cJSON *wind_command(const char *command, const cJSON *params)
{
    if (!s_lock) return apps_error("unavailable", "the wind app is starting up");
    if (!strcmp(command, "wind.set_spot")) {
        const char *name = str_param(params, "name");
        double lat, lon;
        if (!name || strlen(name) > WIND_NAME_MAX) {
            return apps_error("invalid_params", "name is required, up to 15 characters (shown on screen)");
        }
        for (const char *c = name; *c; c++) {
            if (*c < 0x20 || *c > 0x7e) {
                return apps_error("invalid_params", "name must be plain ASCII: the screen has no accented letters");
            }
        }
        if (!number_param(params, "lat", &lat) || !number_param(params, "lon", &lon) || lat < -90 || lat > 90 ||
            lon < -180 || lon > 180) {
            return apps_error("invalid_params", "lat and lon are required, in decimal degrees");
        }
        lock();
        strlcpy(s_spots[CUSTOM].name, name, sizeof(s_spots[CUSTOM].name));
        s_spots[CUSTOM].lat = lat;
        s_spots[CUSTOM].lon = lon;
        s_custom_set = true;
        s_spot_gen[CUSTOM]++;
        s_info[CUSTOM] = (info_t){0};
        s_custom_version++;
        unlock();
        atomic_store(&s_req_spot, CUSTOM);
        muse_apps_request_show(MUSE_APP_WIND);
        ESP_LOGI(TAG, "Muse's spot: %s (%.4f, %.4f)", name, lat, lon);
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "spot", name);
        cJSON_AddStringToObject(p, "shown", "wind");
        return apps_ok(p);
    }
    if (!strcmp(command, "wind.show")) {
        const char *spot = str_param(params, "spot"), *view = str_param(params, "view");
        lock();
        int si = spot ? find_spot_locked(spot) : -1;
        unlock();
        int vi = view ? find_view(view) : -1;
        if (spot && si < 0) return apps_error("invalid_params", "unknown spot: use wind.get for the names");
        if (view && vi < 0) return apps_error("invalid_params", "view must be today, tomorrow or week");
        if (si >= 0) atomic_store(&s_req_spot, si);
        if (vi >= 0) atomic_store(&s_req_view, vi);
        muse_apps_request_show(MUSE_APP_WIND);
        return apps_ok(NULL);
    }
    if (!strcmp(command, "wind.get")) {
        cJSON *p = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(p, "spots");
        lock();
        for (int i = 0; i < spot_count_locked(); i++) cJSON_AddItemToArray(arr, spot_json_locked(i));
        unlock();
        return apps_ok(p);
    }
    return apps_error("unsupported", "unknown wind command");
}
