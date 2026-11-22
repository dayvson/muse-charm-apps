#include "stocks_data.h"

#include "apps_json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *const stocks_symbols[STOCKS_COUNT] = {"META", "SHOP", "SPY"};

const stocks_range_info_t stocks_ranges[RANGE_COUNT] = {
    [RANGE_1D] = {"1D", "1d", "5m", "Today", 60, 15 * 60},
    [RANGE_1W] = {"1W", "5d", "15m", "Past week", 5 * 60, 30 * 60},
    [RANGE_1M] = {"1M", "1mo", "60m", "Past month", 15 * 60, 60 * 60},
    [RANGE_1Y] = {"1Y", "1y", "1d", "Past year", 60 * 60, 6 * 3600},
    [RANGE_5Y] = {"5Y", "5y", "1wk", "Past 5 years", 6 * 3600, 12 * 3600},
};

#define RAW_CAP 600   /* our longest (5Y weekly) has ~262; more is refused, not truncated */

typedef struct {
    double ts[RAW_CAP], cl[RAW_CAP];
    bool ts_null[RAW_CAP], cl_null[RAW_CAP];
    int idx[RAW_CAP];
} scratch_t;   /* ~17 KB: malloc puts it in PSRAM on the board, off the task's stack */

static bool parse_into(const char *json, stocks_series_t *s, int max_points, scratch_t *w)
{
    const char *meta = strstr(json, "\"meta\":{");
    if (!meta) return false;
    const char *meta_end = strstr(meta, "\"timestamp\":[");
    if (!meta_end) return false;

    memset(s, 0, sizeof(*s));
    double v;
    if (!json_number(meta, meta_end, "regularMarketPrice", &s->price) || s->price <= 0) return false;
    if (json_number(meta, meta_end, "chartPreviousClose", &v) && v > 0) s->base = v;
    if (json_number(meta, meta_end, "previousClose", &v) && v > 0) s->prev_close = v;
    if (json_number(meta, meta_end, "regularMarketTime", &v)) s->market_time = (int64_t)v;
    if (json_number(meta, meta_end, "gmtoffset", &v)) s->gmtoffset = (int32_t)v;
    const char *ctp = strstr(meta, "\"currentTradingPeriod\":");
    const char *reg = ctp && ctp < meta_end ? strstr(ctp, "\"regular\":{") : NULL;
    if (reg && reg < meta_end) {
        const char *reg_end = strchr(reg, '}');
        if (json_number(reg, reg_end, "start", &v)) s->session_start = (int64_t)v;
        if (json_number(reg, reg_end, "end", &v)) s->session_end = (int64_t)v;
    }

    int nt = json_array(meta_end, "timestamp", w->ts, w->ts_null, RAW_CAP);
    const char *quote = strstr(meta_end, "\"quote\":[");
    int nc = quote ? json_array(quote, "close", w->cl, w->cl_null, RAW_CAP) : -1;
    if (nt < 2 || nc < 2) return false;
    int n = nt < nc ? nt : nc;

    /* Points with a price, then evenly thinned to max_points (first and last kept). */
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!w->ts_null[i] && !w->cl_null[i] && w->cl[i] > 0) w->idx[k++] = i;
    }
    if (k < 2) return false;
    int keep = k < max_points ? k : max_points;
    for (int j = 0; j < keep; j++) {
        int i = w->idx[(int)((int64_t)j * (k - 1) / (keep - 1))];
        s->t[j] = (int64_t)w->ts[i];
        s->close[j] = (float)w->cl[i];
    }
    s->n = (uint16_t)keep;
    if (s->base <= 0) s->base = s->close[0];
    if (s->prev_close <= 0) s->prev_close = s->base;
    return true;
}

bool stocks_parse(const char *json, stocks_series_t *out, int max_points)
{
    if (!json || !out || max_points < 2 || max_points > STOCKS_MAX_POINTS) return false;
    scratch_t *w = malloc(sizeof(*w));
    if (!w) return false;
    bool ok = parse_into(json, out, max_points, w);
    free(w);
    return ok;
}

int stocks_url(char *out, size_t len, const char *symbol, stocks_range_t range)
{
    const stocks_range_info_t *r = &stocks_ranges[range];
    return snprintf(out, len, "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=%s&interval=%s&includePrePost=false",
                    symbol, r->range, r->interval);
}

int stocks_range_find(const char *label)
{
    for (int i = 0; label && i < RANGE_COUNT; i++) {
        if (!strcasecmp(label, stocks_ranges[i].label)) return i;
    }
    return -1;
}

int stocks_symbol_find(const char *symbol)
{
    for (int i = 0; symbol && i < STOCKS_COUNT; i++) {
        if (!strcasecmp(symbol, stocks_symbols[i])) return i;
    }
    return -1;
}

void stocks_fmt_price(char *out, size_t len, double v)
{
    char digits[32];
    long long cents = llround(fabs(v) * 100);
    snprintf(digits, sizeof(digits), "%lld", cents / 100);
    /* thousands separators */
    char grouped[40];
    int dl = (int)strlen(digits), g = 0;
    for (int i = 0; i < dl; i++) {
        if (i && (dl - i) % 3 == 0) grouped[g++] = ',';
        grouped[g++] = digits[i];
    }
    grouped[g] = 0;
    snprintf(out, len, "%s$%s.%02lld", v < 0 ? "-" : "", grouped, cents % 100);
}

void stocks_fmt_change(char *out, size_t len, double now, double base)
{
    double d = now - base;
    double pct = base > 0 ? d / base * 100 : 0;
    /* Round first so "-0.00" never shows. */
    if (fabs(d) < 0.005) d = 0;
    if (fabs(pct) < 0.005) pct = 0;
    snprintf(out, len, "%s%.2f (%s%.2f%%)", d >= 0 ? "+" : "-", fabs(d), pct >= 0 ? "+" : "-", fabs(pct));
}
