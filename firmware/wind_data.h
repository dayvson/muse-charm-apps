#pragma once

/*
 * The wind tracker's data: Open-Meteo forecasts (knots, sea grid cell) parsed
 * without a JSON tree, and the kiter's view of them: a rating, a colour scale
 * and the best session window. Pure C, no ESP-IDF: tests/ runs it on real
 * responses on the host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIND_HOURS 168           /* 7 days hourly */
#define WIND_DAYS 7
#define WIND_NAME_MAX 15         /* spot names on screen */
#define WIND_DAY_FIRST_HOUR 6    /* the hourly charts: 6 AM ... */
#define WIND_DAY_LAST_HOUR 21    /* ... to 9 PM */
#define WIND_SESSION_FIRST 7     /* a session window: daylight, 7 AM ... */
#define WIND_SESSION_LAST 19     /* ... to 7 PM */

typedef struct {
    int32_t utc_offset;          /* the spot's, seconds east of UTC */
    bool has_now;
    int64_t now_t;
    float now_kn, now_gust;
    int16_t now_dir;             /* degrees the wind comes from */
    int nh;
    int64_t ht[WIND_HOURS];
    float hs[WIND_HOURS], hg[WIND_HOURS];
    int16_t hd[WIND_HOURS];
    int nd;
    int64_t dt[WIND_DAYS];       /* the spot's local midnights */
    float ds[WIND_DAYS], dg[WIND_DAYS];
    int16_t dd[WIND_DAYS];
} wind_fc_t;

typedef enum { RATE_LIGHT, RATE_MARGINAL, RATE_GO, RATE_STRONG, RATE_NUKING } wind_rating_t;

/* False unless it's a forecast with hourly and daily wind. */
bool wind_parse(const char *json, wind_fc_t *out);
int wind_url(char *out, size_t len, double lat, double lon);

/* "ESE" for the direction the wind comes from. */
const char *wind_compass(int deg);
/* The speed scale's colour (0xRRGGBB). */
uint32_t wind_color(float kn);
wind_rating_t wind_rate(float kn);
const char *wind_rate_label(wind_rating_t r);
uint32_t wind_rate_color(wind_rating_t r);

/* Indices of day `day`'s hours from `first` to `last` local (inclusive). */
int wind_day_hours(const wind_fc_t *f, int day, int first, int last, int *idx, int cap);
/* The local hour (0-23) of hourly index i. */
int wind_local_hour(const wind_fc_t *f, int i);

/* The longest daylight run on `day` at the best rating it reaches (GO, else
 * MARGINAL), from `not_before` (Unix time; 0 for the whole day). False if no
 * hour is even marginal. Hours are local, h1 inclusive. */
bool wind_best_window(const wind_fc_t *f, int day, int64_t not_before, int *h0, int *h1, float *lo, float *hi);
/* The day with the strongest kiteable wind (max sustained, capped at what's
 * still fun); -1 if none has data. */
int wind_best_day(const wind_fc_t *f);

/* "1 PM" */
void wind_fmt_hour(char *out, size_t len, int hour);
/* The 3-letter weekday of daily index d. */
const char *wind_weekday(const wind_fc_t *f, int d);
