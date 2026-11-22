/*
 * The stock tracker tile: three ticker pills (today's change), the price,
 * the change over the chosen range, a line chart and range chips. Layout
 * matches firmware/tools/preview_stocks.py.
 *
 * Fetching (stocks_net.c) only happens when it's useful and cheap: Wi-Fi up,
 * the screen awake, no voice call (which needs the internal RAM a TLS
 * connection's socket takes), and either the tile on screen or the first
 * fill of the pills after boot. Each range has its own staleness.
 */

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "muse_apps_priv.h"
#include "muse_state.h"
#include "muse_tz.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#include "stocks_net.h"


/* Geometry on the 466 px circle (screen coordinates). */
#define PILL_Y 92
#define PILL_W 100
#define PILL_H 48
#define PILL_GAP 8
#define PRICE_Y 158
#define CHANGE_Y 204
#define CHART_X 53
#define CHART_Y 232
#define CHART_W 360
#define CHART_H 124
#define CHIP_Y 388
#define CHIP_W 54
#define CHIP_H 30
#define CHIP_GAP 7
#define FOOT_Y 428

#define GREEN 0x30d158
#define RED 0xff453a
#define TEXT 0xffffff
#define DIM 0x8e8e93
#define PILL_BG 0x1c1c1e
#define PILL_LINE 0x3a3a3c
#define BASE_LINE 0x48484a

#if CONFIG_LV_FONT_MONTSERRAT_48
#define FONT_PRICE (&lv_font_montserrat_48)
#else
#define FONT_PRICE (&lv_font_montserrat_28)
#endif

typedef struct {
    lv_obj_t *pill[STOCKS_COUNT], *pill_sym[STOCKS_COUNT], *pill_pct[STOCKS_COUNT];
    lv_obj_t *price, *change, *period, *chart, *base, *chip[RANGE_COUNT], *chip_lbl[RANGE_COUNT], *foot;
    lv_draw_buf_t *chart_buf;       /* A8, tinted with the trend colour; NULL away from the tile */
    stocks_series_t cur;            /* the selected symbol and range */
    stocks_series_t day;            /* scratch: a symbol's 1D, for the pills */
    uint32_t pill_version[STOCKS_COUNT];
    uint32_t shown_version;
    int sel, range;
    int shown_sel, shown_range;
    bool shown_ok, prefetched;
    char shown_foot[48];
    int8_t pill_up[STOCKS_COUNT];   /* -1 until known */
    float ycol[CHART_W];
    float px[STOCKS_MAX_POINTS], py[STOCKS_MAX_POINTS];   /* here, not on the UI task's stack */
} stocks_ui_t;

EXT_RAM_BSS_ATTR static stocks_ui_t u;
static atomic_int s_req_sym = -1, s_req_range = -1;   /* stocks.show, for the UI task */

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

static int64_t wall_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

/* ---- chart ---- */

/* The chart is one colour at varying strength (the line, the dot, the fill
 * fading to black), so it's an A8 coverage map the canvas tints with the
 * trend's colour: half an RGB565 buffer's size, the same pixels on black. The
 * dotted base line is grey: it's a line object just behind the canvas. */
#define CHART_BYTES (CHART_W * CHART_H)

void stocks_trim(void)
{
    if (!u.chart_buf || muse_apps_near(MUSE_APP_STOCKS)) return;
    lv_image_set_src(u.chart, NULL);
    lv_draw_buf_destroy(u.chart_buf);
    u.chart_buf = NULL;
    lv_obj_add_flag(u.base, LV_OBJ_FLAG_HIDDEN);
}

/* Keeps the chart's buffer only near the tile (not out of voice's reserve).
 * True when it was just made: the chart needs drawing. */
static bool chart_keep(void)
{
    if (!muse_apps_near(MUSE_APP_STOCKS)) {
        stocks_trim();
        return false;
    }
    if (u.chart_buf || !u.chart) return false;
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < CHART_BYTES + MUSE_APPS_PSRAM_RESERVE) return false;
    u.chart_buf = lv_draw_buf_create(CHART_W, CHART_H, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    if (!u.chart_buf) return false;
    lv_canvas_set_draw_buf(u.chart, u.chart_buf);
    return true;
}

static void draw_chart(const stocks_series_t *s, bool have, uint32_t color)
{
    lv_draw_buf_t *db = u.chart_buf;
    if (!db) return;   /* away from the tile: drawn when it's back */
    uint8_t *data = db->data;
    uint32_t stride = db->header.stride;
    for (int y = 0; y < CHART_H; y++) memset(data + y * stride, 0, CHART_W);
    lv_obj_set_style_image_recolor(u.chart, lv_color_hex(color), 0);
    lv_obj_add_flag(u.base, LV_OBJ_FLAG_HIDDEN);
    if (!have || s->n < 2) {
        lv_obj_invalidate(u.chart);
        return;
    }

    double lo = s->base, hi = s->base;
    for (int i = 0; i < s->n; i++) {
        lo = fmin(lo, s->close[i]);
        hi = fmax(hi, s->close[i]);
    }
    double pad = (hi - lo) * 0.08;
    if (pad <= 0) pad = 1;
    lo -= pad;
    hi += pad;

    /* x: today by the clock across the session (a morning fills the left);
     * longer ranges by index, so nights and weekends don't leave gaps. */
    bool by_time = u.range == RANGE_1D && s->session_end > s->session_start;
    float *px = u.px, *py = u.py;
    for (int i = 0; i < s->n; i++) {
        float x = by_time ? (float)(s->t[i] - s->session_start) / (float)(s->session_end - s->session_start)
                          : (float)i / (float)(s->n - 1);
        if (x < 0) x = 0;
        if (x > 1) x = 1;
        px[i] = x * (CHART_W - 1);
        py[i] = (float)((hi - s->close[i]) / (hi - lo) * (CHART_H - 1));
    }

    /* Fill: the line's colour fading to black at the bottom. */
    for (int x = 0; x < CHART_W; x++) u.ycol[x] = -1;
    for (int i = 0; i + 1 < s->n; i++) {
        int x0 = (int)ceilf(px[i]), x1 = (int)floorf(px[i + 1]);
        for (int x = x0; x <= x1 && x < CHART_W; x++) {
            float t = px[i + 1] > px[i] ? (x - px[i]) / (px[i + 1] - px[i]) : 0;
            u.ycol[x] = py[i] + (py[i + 1] - py[i]) * t;
        }
    }
    for (int x = 0; x < CHART_W; x++) {
        float y = u.ycol[x];
        if (y < 0) continue;
        for (int r = (int)ceilf(y); r < CHART_H; r++) {
            float k = 0.28f * (1.0f - (r - y) / (CHART_H - y + 1));
            data[r * stride + x] = (uint8_t)lroundf(k * 255);
        }
    }

    /* The range's base (previous close on 1D): dotted, under the price line. */
    int by = (int)lroundf((float)((hi - s->base) / (hi - lo) * (CHART_H - 1)));
    if (by >= 0 && by < CHART_H) {
        lv_obj_set_pos(u.base, CHART_X, CHART_Y + by);
        lv_obj_remove_flag(u.base, LV_OBJ_FLAG_HIDDEN);
    }

    lv_layer_t layer;
    lv_canvas_init_layer(u.chart, &layer);
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = lv_color_hex(color);
    ld.width = 3;
    ld.round_start = 1;
    ld.round_end = 1;
    for (int i = 0; i + 1 < s->n; i++) {
        ld.p1.x = (int32_t)lroundf(px[i]);
        ld.p1.y = (int32_t)lroundf(py[i]);
        ld.p2.x = (int32_t)lroundf(px[i + 1]);
        ld.p2.y = (int32_t)lroundf(py[i + 1]);
        lv_draw_line(&layer, &ld);
    }
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = lv_color_hex(color);
    dot.radius = LV_RADIUS_CIRCLE;
    int lx = (int)lroundf(px[s->n - 1]), ly = (int)lroundf(py[s->n - 1]);
    lv_area_t a = {lx - 4, ly - 4, lx + 4, ly + 4};
    lv_draw_rect(&layer, &dot, &a);
    lv_canvas_finish_layer(u.chart, &layer);
    lv_obj_invalidate(u.chart);
}

/* ---- the rest of the screen ---- */

static void fmt_clock(char *out, size_t len, int64_t t, int32_t gmtoffset, bool weekday)
{
    static const char *const WD[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    muse_civil_t c;
    muse_tz_civil(t + gmtoffset, &c);
    int h12 = c.hour % 12 ? c.hour % 12 : 12;
    if (weekday) {
        snprintf(out, len, "%s %d:%02d %s ET", WD[c.wday], h12, c.min, c.hour < 12 ? "AM" : "PM");
    } else {
        snprintf(out, len, "%d:%02d %s ET", h12, c.min, c.hour < 12 ? "AM" : "PM");
    }
}

static void update_pills(void)
{
    for (int i = 0; i < STOCKS_COUNT; i++) {
        stocks_slot_info_t info = stocks_net_info(i, RANGE_1D);
        bool sel = i == u.sel;
        if (info.ok && info.version != u.pill_version[i] && stocks_net_copy(i, RANGE_1D, &u.day, NULL)) {
            u.pill_version[i] = info.version;
            double pct = (u.day.price - u.day.prev_close) / u.day.prev_close * 100;
            char b[16];
            snprintf(b, sizeof(b), "%s%.2f%%", pct >= 0 ? "+" : "-", fabs(pct));
            set_text(u.pill_pct[i], b);
            u.pill_up[i] = pct >= 0;
            lv_obj_set_style_text_color(u.pill_pct[i], lv_color_hex(u.pill_up[i] ? GREEN : RED), 0);
        }
        /* The selected pill's border follows its day. */
        uint32_t trend = u.pill_up[i] < 0 ? PILL_LINE : u.pill_up[i] ? GREEN : RED;
        lv_obj_set_style_bg_opa(u.pill[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(u.pill[i], lv_color_hex(sel ? trend : PILL_LINE), 0);
        lv_obj_set_style_border_width(u.pill[i], sel ? 2 : 1, 0);
        lv_obj_set_style_text_color(u.pill_sym[i], lv_color_hex(sel ? TEXT : DIM), 0);
    }
}

static void update_main(bool force)
{
    stocks_slot_info_t info;
    bool have = stocks_net_copy(u.sel, u.range, NULL, &info);
    if (!force && have == u.shown_ok && info.version == u.shown_version && u.sel == u.shown_sel &&
        u.range == u.shown_range) {
        return;
    }
    if (have) stocks_net_copy(u.sel, u.range, &u.cur, &info);
    u.shown_ok = have;
    u.shown_version = info.version;
    u.shown_sel = u.sel;
    u.shown_range = u.range;

    char b[48];
    bool up = !have || u.cur.price >= u.cur.base;
    uint32_t color = up ? GREEN : RED;
    if (have) {
        stocks_fmt_price(b, sizeof(b), u.cur.price);
        set_text(u.price, b);
        stocks_fmt_change(b, sizeof(b), u.cur.price, u.cur.base);
        set_text(u.change, b);
    } else {
        set_text(u.price, "--");
        set_text(u.change, "");
    }
    lv_obj_set_style_text_color(u.change, lv_color_hex(color), 0);
    set_text(u.period, stocks_ranges[u.range].period);
    for (int i = 0; i < RANGE_COUNT; i++) {
        bool sel = i == u.range;
        lv_obj_set_style_bg_color(u.chip[i], lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(u.chip[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(u.chip_lbl[i], lv_color_hex(sel ? 0x000000 : DIM), 0);
    }
    draw_chart(&u.cur, have, color);
    update_pills();
}

static void update_foot(void)
{
    stocks_slot_info_t info = stocks_net_info(u.sel, u.range);
    char b[48];
    if (!info.ok) {
        snprintf(b, sizeof(b), "%s", info.failed && !info.pending ? "Can't load prices" : "Loading...");
    } else {
        int64_t now = wall_now();
        bool open = now >= u.cur.session_start && now < u.cur.session_end;
        char clock[24];
        fmt_clock(clock, sizeof(clock), u.cur.market_time, u.cur.gmtoffset, !open);
        snprintf(b, sizeof(b), "%s %s", open ? "Updated" : "At close •", clock);
    }
    if (strcmp(b, u.shown_foot)) {
        strlcpy(u.shown_foot, b, sizeof(u.shown_foot));
        set_text(u.foot, b);
    }
}

/* ---- fetching ---- */

static bool market_open(void)
{
    stocks_slot_info_t info;
    if (!stocks_net_copy(0, RANGE_1D, &u.day, &info)) return false;
    int64_t now = wall_now();
    return now >= u.day.session_start && now < u.day.session_end;
}

static void want(int sym, int range, bool open, int64_t now_us)
{
    stocks_slot_info_t i = stocks_net_info(sym, range);
    if (i.pending) return;
    int64_t stale = (int64_t)(open ? stocks_ranges[range].stale_open_s : stocks_ranges[range].stale_closed_s) * 1000000;
    if (i.ok && now_us - i.fetched_us < stale) return;
    if (i.failed && now_us - i.tried_us < 30 * 1000000LL) return;   /* back off */
    stocks_net_request(sym, range);
}

static void fetch_policy(void)
{
    if (!muse_apps_net_ok()) return;
    bool visible = muse_apps_visible(MUSE_APP_STOCKS);
    if (!visible && u.prefetched) return;
    int64_t now_us = esp_timer_get_time();
    bool open = market_open();
    if (visible) want(u.sel, u.range, open, now_us);
    for (int i = 0; i < STOCKS_COUNT; i++) want(i, RANGE_1D, open, now_us);
    u.prefetched = true;
}

/* ---- events ---- */

static void on_pill(lv_event_t *e)
{
    u.sel = (int)(intptr_t)lv_event_get_user_data(e);
    update_main(true);
    update_foot();
}

static void on_chip(lv_event_t *e)
{
    u.range = (int)(intptr_t)lv_event_get_user_data(e);
    update_main(true);
    update_foot();
}

static void tick(lv_timer_t *t)
{
    (void)t;
    int sym = atomic_exchange(&s_req_sym, -1), range = atomic_exchange(&s_req_range, -1);
    if (sym >= 0) u.sel = sym;
    if (range >= 0) u.range = range;
    fetch_policy();
    bool fresh = chart_keep();
    update_main(sym >= 0 || range >= 0 || fresh);
    update_pills();
    update_foot();
}

void stocks_build(lv_obj_t *tile, int w, int h)
{
    (void)w;
    (void)h;
    stocks_net_init();
    lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(tile, 0, 0);

    for (int i = 0; i < STOCKS_COUNT; i++) {
        lv_obj_t *p = button(tile, PILL_W, PILL_H, (i - 1) * (PILL_W + PILL_GAP), PILL_Y, on_pill, i);
        lv_obj_set_style_bg_color(p, lv_color_hex(PILL_BG), 0);
        lv_obj_set_style_border_color(p, lv_color_hex(PILL_LINE), 0);
        lv_obj_set_style_border_width(p, 1, 0);
        u.pill[i] = p;
        u.pill_sym[i] = label(p, &lv_font_montserrat_16, DIM);
        lv_label_set_text(u.pill_sym[i], stocks_symbols[i]);
        lv_obj_align(u.pill_sym[i], LV_ALIGN_CENTER, 0, -9);
        u.pill_pct[i] = label(p, &lv_font_montserrat_14, DIM);
        lv_label_set_text(u.pill_pct[i], "--");
        lv_obj_align(u.pill_pct[i], LV_ALIGN_CENTER, 0, 11);
    }

    u.price = label(tile, FONT_PRICE, TEXT);
    lv_obj_align(u.price, LV_ALIGN_CENTER, 0, dy(PRICE_Y));
    lv_obj_t *row = lv_obj_create(tile);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, dy(CHANGE_Y));
    u.change = label(row, &lv_font_montserrat_20, GREEN);
    u.period = label(row, &lv_font_montserrat_14, DIM);

    static const lv_point_precise_t base_pts[] = {{0, 0}, {CHART_W - 1, 0}};
    u.base = lv_line_create(tile);   /* before the chart: under it */
    lv_line_set_points(u.base, base_pts, 2);
    lv_obj_set_style_line_color(u.base, lv_color_hex(BASE_LINE), 0);
    lv_obj_set_style_line_width(u.base, 1, 0);
    lv_obj_set_style_line_dash_width(u.base, 2, 0);
    lv_obj_set_style_line_dash_gap(u.base, 4, 0);
    lv_obj_add_flag(u.base, LV_OBJ_FLAG_HIDDEN);
    u.chart = lv_canvas_create(tile);   /* its buffer comes and goes with chart_keep() */
    lv_obj_set_pos(u.chart, CHART_X, CHART_Y);
    lv_obj_set_style_image_recolor_opa(u.chart, LV_OPA_COVER, 0);
    chart_keep();

    for (int i = 0; i < RANGE_COUNT; i++) {
        u.chip[i] = button(tile, CHIP_W, CHIP_H, (i - 2) * (CHIP_W + CHIP_GAP), CHIP_Y, on_chip, i);
        u.chip_lbl[i] = label(u.chip[i], &lv_font_montserrat_16, DIM);
        lv_label_set_text(u.chip_lbl[i], stocks_ranges[i].label);
        lv_obj_center(u.chip_lbl[i]);
    }
    u.foot = label(tile, &lv_font_montserrat_14, DIM);
    lv_obj_align(u.foot, LV_ALIGN_CENTER, 0, dy(FOOT_Y));

    u.sel = 0;
    u.range = RANGE_1D;
    memset(u.pill_up, -1, sizeof(u.pill_up));
    u.shown_sel = u.shown_range = -1;
    update_main(true);
    update_foot();
    lv_timer_create(tick, 500, NULL);
}

/* ---- commands (Noise session task) ---- */

static cJSON *quote_json(int sym)
{
    stocks_series_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "symbol", stocks_symbols[sym]);
    if (s && stocks_net_copy(sym, RANGE_1D, s, NULL)) {
        cJSON_AddNumberToObject(o, "price", round(s->price * 100) / 100);
        cJSON_AddNumberToObject(o, "previous_close", round(s->prev_close * 100) / 100);
        cJSON_AddNumberToObject(o, "change_pct", round((s->price - s->prev_close) / s->prev_close * 10000) / 100);
        cJSON_AddNumberToObject(o, "as_of", (double)s->market_time);
    } else {
        cJSON_AddNullToObject(o, "price");
    }
    heap_caps_free(s);
    return o;
}

cJSON *stocks_command(const char *command, const cJSON *params)
{
    if (!strcmp(command, "stocks.show")) {
        const cJSON *sym = params ? cJSON_GetObjectItemCaseSensitive(params, "symbol") : NULL;
        const cJSON *rng = params ? cJSON_GetObjectItemCaseSensitive(params, "range") : NULL;
        int si = -1, ri = -1;
        if (sym && !cJSON_IsNull(sym)) {
            si = cJSON_IsString(sym) ? stocks_symbol_find(sym->valuestring) : -1;
            if (si < 0) return apps_error("invalid_params", "symbol must be META, SHOP or SPY");
        }
        if (rng && !cJSON_IsNull(rng)) {
            ri = cJSON_IsString(rng) ? stocks_range_find(rng->valuestring) : -1;
            if (ri < 0) return apps_error("invalid_params", "range must be 1D, 1W, 1M, 1Y or 5Y");
        }
        if (si >= 0) atomic_store(&s_req_sym, si);
        if (ri >= 0) atomic_store(&s_req_range, ri);
        muse_apps_request_show(MUSE_APP_STOCKS);
        cJSON *p = cJSON_CreateObject();
        if (si >= 0) cJSON_AddStringToObject(p, "symbol", stocks_symbols[si]);
        if (ri >= 0) cJSON_AddStringToObject(p, "range", stocks_ranges[ri].label);
        cJSON_AddStringToObject(p, "shown", "stocks");
        return apps_ok(p);
    }
    if (!strcmp(command, "stocks.get")) {
        cJSON *p = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(p, "quotes");
        for (int i = 0; i < STOCKS_COUNT; i++) cJSON_AddItemToArray(arr, quote_json(i));
        return apps_ok(p);
    }
    return apps_error("unsupported", "unknown stocks command");
}
