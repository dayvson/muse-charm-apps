#include "wind_data.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps_json.h"
#include "muse_tz.h"

typedef struct {
    double v[WIND_HOURS];
    bool null[WIND_HOURS];
} col_t;

static int column(const char *from, const char *key, col_t *c)
{
    return json_array(from, key, c->v, c->null, WIND_HOURS);
}

static bool parse_into(const char *json, wind_fc_t *f, col_t *t, col_t *s, col_t *g, col_t *d)
{
    memset(f, 0, sizeof(*f));
    double v;
    if (json_number(json, NULL, "utc_offset_seconds", &v)) f->utc_offset = (int32_t)v;

    const char *cur = strstr(json, "\"current\":{");
    if (cur) {
        const char *end = strchr(cur, '}');
        double sp, gu, di, tt;
        if (json_number(cur, end, "wind_speed_10m", &sp) && json_number(cur, end, "wind_direction_10m", &di)) {
            f->has_now = true;
            f->now_kn = (float)sp;
            f->now_dir = (int16_t)di;
            f->now_gust = json_number(cur, end, "wind_gusts_10m", &gu) ? (float)gu : (float)sp;
            if (json_number(cur, end, "time", &tt)) f->now_t = (int64_t)tt;
        }
    }

    const char *hourly = strstr(json, "\"hourly\":{");
    if (!hourly) return false;
    int n = column(hourly, "time", t);
    if (n < 24 || column(hourly, "wind_speed_10m", s) != n || column(hourly, "wind_direction_10m", d) != n ||
        column(hourly, "wind_gusts_10m", g) != n) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (t->null[i] || s->null[i]) continue;
        int k = f->nh++;
        f->ht[k] = (int64_t)t->v[i];
        f->hs[k] = (float)s->v[i];
        f->hg[k] = g->null[i] ? (float)s->v[i] : (float)g->v[i];
        f->hd[k] = d->null[i] ? -1 : (int16_t)d->v[i];
    }

    const char *daily = strstr(json, "\"daily\":{");
    if (!daily) return false;
    int nd = json_array(daily, "time", t->v, t->null, WIND_DAYS);
    if (nd < 1 || json_array(daily, "wind_speed_10m_max", s->v, s->null, WIND_DAYS) != nd ||
        json_array(daily, "wind_gusts_10m_max", g->v, g->null, WIND_DAYS) != nd ||
        json_array(daily, "wind_direction_10m_dominant", d->v, d->null, WIND_DAYS) != nd) {
        return false;
    }
    for (int i = 0; i < nd; i++) {
        f->dt[i] = (int64_t)t->v[i];
        f->ds[i] = s->null[i] ? NAN : (float)s->v[i];
        f->dg[i] = g->null[i] ? f->ds[i] : (float)g->v[i];
        f->dd[i] = d->null[i] ? -1 : (int16_t)d->v[i];
    }
    f->nd = nd;
    return f->nh >= 24;
}

bool wind_parse(const char *json, wind_fc_t *out)
{
    if (!json || !out) return false;
    col_t *c = malloc(4 * sizeof(col_t));   /* ~7 KB: PSRAM on the board, off the stack */
    if (!c) return false;
    bool ok = parse_into(json, out, &c[0], &c[1], &c[2], &c[3]);
    free(c);
    return ok;
}

int wind_url(char *out, size_t len, double lat, double lon)
{
    return snprintf(out, len,
                    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                    "&current=wind_speed_10m,wind_direction_10m,wind_gusts_10m"
                    "&hourly=wind_speed_10m,wind_direction_10m,wind_gusts_10m"
                    "&daily=wind_speed_10m_max,wind_gusts_10m_max,wind_direction_10m_dominant"
                    "&wind_speed_unit=kn&timezone=auto&timeformat=unixtime&forecast_days=7&cell_selection=sea",
                    lat, lon);
}

const char *wind_compass(int deg)
{
    static const char *const P[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    if (deg < 0) return "--";
    return P[((deg % 360) * 16 + 180) / 360 % 16];
}

uint32_t wind_color(float kn)
{
    if (kn < 6) return 0x4a6b8a;    /* slate: flat */
    if (kn < 10) return 0x2eb8e6;   /* sky */
    if (kn < 14) return 0x27d3a2;   /* teal */
    if (kn < 18) return 0x7be04a;   /* green */
    if (kn < 22) return 0xf5d547;   /* yellow */
    if (kn < 27) return 0xff9f2e;   /* orange */
    if (kn < 33) return 0xff4d4d;   /* red */
    return 0xd35cff;                /* purple */
}

wind_rating_t wind_rate(float kn)
{
    if (kn < 11) return RATE_LIGHT;
    if (kn < 14) return RATE_MARGINAL;
    if (kn < 25) return RATE_GO;
    if (kn < 32) return RATE_STRONG;
    return RATE_NUKING;
}

const char *wind_rate_label(wind_rating_t r)
{
    static const char *const L[] = {"TOO LIGHT", "MARGINAL", "GO KITE!", "HOLD ON!", "NUKING"};
    return L[r];
}

uint32_t wind_rate_color(wind_rating_t r)
{
    static const uint32_t C[] = {0x4a6b8a, 0x27d3a2, 0x7be04a, 0xff9f2e, 0xd35cff};
    return C[r];
}

int wind_local_hour(const wind_fc_t *f, int i)
{
    muse_civil_t c;
    muse_tz_civil(f->ht[i] + f->utc_offset, &c);
    return c.hour;
}

int wind_day_hours(const wind_fc_t *f, int day, int first, int last, int *idx, int cap)
{
    if (day < 0 || day >= f->nd) return 0;
    int64_t d0 = f->dt[day], d1 = d0 + 86400;
    int n = 0;
    for (int i = 0; i < f->nh && n < cap; i++) {
        if (f->ht[i] < d0 || f->ht[i] >= d1) continue;
        int h = wind_local_hour(f, i);
        if (h >= first && h <= last) idx[n++] = i;
    }
    return n;
}

bool wind_best_window(const wind_fc_t *f, int day, int64_t not_before, int *h0, int *h1, float *lo, float *hi)
{
    int idx[24];
    int n = wind_day_hours(f, day, WIND_SESSION_FIRST, WIND_SESSION_LAST, idx, 24);
    for (wind_rating_t want = RATE_GO; want >= RATE_MARGINAL; want--) {
        int best_len = 0, best_start = -1, run = 0;
        for (int k = 0; k <= n; k++) {
            bool ok = k < n && f->ht[idx[k]] + 3600 > not_before && wind_rate(f->hs[idx[k]]) >= want &&
                      wind_rate(f->hs[idx[k]]) <= RATE_STRONG;
            if (ok) {
                run++;
            } else {
                if (run > best_len) {
                    best_len = run;
                    best_start = k - run;
                }
                run = 0;
            }
        }
        if (best_len > 0) {
            *h0 = wind_local_hour(f, idx[best_start]);
            *h1 = wind_local_hour(f, idx[best_start + best_len - 1]);
            *lo = *hi = f->hs[idx[best_start]];
            for (int k = best_start; k < best_start + best_len; k++) {
                *lo = fminf(*lo, f->hs[idx[k]]);
                *hi = fmaxf(*hi, f->hs[idx[k]]);
            }
            return true;
        }
    }
    return false;
}

int wind_best_day(const wind_fc_t *f)
{
    int best = -1;
    float score = -1;
    for (int d = 0; d < f->nd; d++) {
        if (isnan(f->ds[d])) continue;
        /* Stronger is better up to "hold on"; nuking days don't win. */
        float s = f->ds[d] < 32 ? f->ds[d] : 0;
        if (s > score) {
            score = s;
            best = d;
        }
    }
    return best;
}

void wind_fmt_hour(char *out, size_t len, int hour)
{
    int h12 = hour % 12 ? hour % 12 : 12;
    snprintf(out, len, "%d %s", h12, hour < 12 ? "AM" : "PM");
}

const char *wind_weekday(const wind_fc_t *f, int d)
{
    static const char *const WD[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    muse_civil_t c;
    muse_tz_civil(f->dt[d] + f->utc_offset, &c);
    return WD[c.wday];
}
