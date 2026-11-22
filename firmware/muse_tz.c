#include "muse_tz.h"

#include <ctype.h>
#include <string.h>

/* ---- calendar (proleptic Gregorian, days since 1970-01-01) ---- */

static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

static bool leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int month_days(int y, int m)
{
    static const int n[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : n[m - 1];
}

static int64_t floor_div(int64_t a, int64_t b)
{
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)));
}

void muse_tz_civil(int64_t t, muse_civil_t *out)
{
    int64_t days = floor_div(t, 86400);
    int64_t secs = t - days * 86400;
    civil_from_days(days, &out->year, &out->month, &out->day);
    out->hour = (int)(secs / 3600);
    out->min = (int)(secs / 60 % 60);
    out->sec = (int)(secs % 60);
    out->wday = (int)((days % 7 + 11) % 7);   /* 1970-01-01 was a Thursday */
}

/* ---- parsing ---- */

static bool parse_name(const char **p)
{
    const char *s = *p;
    int n = 0;
    if (*s == '<') {
        s++;
        while (*s && *s != '>') {
            if (!isalnum((unsigned char)*s) && *s != '+' && *s != '-') return false;
            s++, n++;
        }
        if (*s != '>' || n < 3) return false;
        *p = s + 1;
        return true;
    }
    while (isalpha((unsigned char)*s)) s++, n++;
    if (n < 3) return false;
    *p = s;
    return true;
}

/* [+-]hh[:mm[:ss]]; `max_h` bounds the hours. */
static bool parse_hms(const char **p, int max_h, int32_t *out)
{
    const char *s = *p;
    int sign = 1;
    if (*s == '+' || *s == '-') sign = *s++ == '-' ? -1 : 1;
    int part[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) return false;
        int v = 0, digits = 0;
        while (isdigit((unsigned char)*s) && digits < 3) v = v * 10 + (*s++ - '0'), digits++;
        if (isdigit((unsigned char)*s)) return false;
        part[i] = v;
        if (*s != ':') break;
        s++;
    }
    if (part[0] > max_h || part[1] > 59 || part[2] > 59) return false;
    *out = sign * (part[0] * 3600 + part[1] * 60 + part[2]);
    *p = s;
    return true;
}

static bool parse_int(const char **p, int lo, int hi, int16_t *out)
{
    const char *s = *p;
    if (!isdigit((unsigned char)*s)) return false;
    int v = 0;
    while (isdigit((unsigned char)*s)) {
        v = v * 10 + (*s++ - '0');
        if (v > hi) return false;
    }
    if (v < lo) return false;
    *out = (int16_t)v;
    *p = s;
    return true;
}

static bool parse_rule(const char **p, muse_tz_rule_t *r)
{
    const char *s = *p;
    memset(r, 0, sizeof(*r));
    if (*s == 'J') {
        s++;
        r->kind = 'J';
        if (!parse_int(&s, 1, 365, &r->a)) return false;
    } else if (*s == 'M') {
        s++;
        r->kind = 'M';
        if (!parse_int(&s, 1, 12, &r->a) || *s++ != '.') return false;
        if (!parse_int(&s, 1, 5, &r->b) || *s++ != '.') return false;
        if (!parse_int(&s, 0, 6, &r->c)) return false;
    } else {
        r->kind = 'D';
        if (!parse_int(&s, 0, 365, &r->a)) return false;
    }
    r->secs = 2 * 3600;
    if (*s == '/') {
        s++;
        if (!parse_hms(&s, 167, &r->secs)) return false;
    }
    *p = s;
    return true;
}

bool muse_tz_parse(const char *s, muse_tz_t *tz)
{
    if (!s || !*s || strlen(s) > MUSE_TZ_MAX) return false;
    muse_tz_t t = {0};
    int32_t west;
    if (!parse_name(&s) || !parse_hms(&s, 24, &west)) return false;
    t.std_off = -west;
    if (*s) {
        if (!parse_name(&s)) return false;
        t.has_dst = true;
        t.dst_off = t.std_off + 3600;
        if (*s && *s != ',') {
            if (!parse_hms(&s, 24, &west)) return false;
            t.dst_off = -west;
        }
        if (*s == ',') {
            s++;
            if (!parse_rule(&s, &t.start) || *s++ != ',' || !parse_rule(&s, &t.end)) return false;
        } else {
            /* No rule: the US one, as glibc assumes. */
            t.start = (muse_tz_rule_t){'M', 3, 2, 0, 7200};
            t.end = (muse_tz_rule_t){'M', 11, 1, 0, 7200};
        }
    }
    if (*s) return false;
    *tz = t;
    return true;
}

/* ---- conversion ---- */

/* Local midnight (as Unix-style seconds) of the rule's day in `year`, plus its time. */
static int64_t rule_local(const muse_tz_rule_t *r, int year)
{
    int64_t jan1 = days_from_civil(year, 1, 1);
    int64_t day;
    switch (r->kind) {
    case 'J':
        day = jan1 + r->a - 1 + (leap(year) && r->a >= 60);
        break;
    case 'D':
        day = jan1 + r->a;
        break;
    default: {
        int64_t first = days_from_civil(year, r->a, 1);
        int wday = (int)((first % 7 + 11) % 7);
        int mday = 1 + (r->c - wday + 7) % 7 + (r->b - 1) * 7;
        while (mday > month_days(year, r->a)) mday -= 7;
        day = first + mday - 1;
        break;
    }
    }
    return day * 86400 + r->secs;
}

static bool dst_in_year(const muse_tz_t *tz, int64_t utc, int year)
{
    int64_t start = rule_local(&tz->start, year) - tz->std_off;   /* starts in standard time */
    int64_t end = rule_local(&tz->end, year) - tz->dst_off;       /* ends in daylight time */
    if (start < end) return utc >= start && utc < end;
    return !(utc >= end && utc < start);                         /* southern hemisphere */
}

int32_t muse_tz_offset(const muse_tz_t *tz, int64_t utc, bool *is_dst)
{
    bool dst = false;
    if (tz->has_dst) {
        muse_civil_t c;
        muse_tz_civil(utc + tz->std_off, &c);
        dst = dst_in_year(tz, utc, c.year);
    }
    if (is_dst) *is_dst = dst;
    return dst ? tz->dst_off : tz->std_off;
}

/* ---- HTTP dates ---- */

static int two_or_four(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

int64_t muse_http_date(const char *s, size_t len)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if (!s || len != 29 || s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' ||
        s[19] != ':' || s[22] != ':' || memcmp(s + 25, " GMT", 4) != 0) {
        return -1;
    }
    int day = two_or_four(s + 5, 2), year = two_or_four(s + 12, 4);
    int hh = two_or_four(s + 17, 2), mm = two_or_four(s + 20, 2), ss = two_or_four(s + 23, 2);
    int mon = -1;
    for (int i = 0; i < 12; i++) {
        if (!memcmp(s + 8, months + 3 * i, 3)) mon = i + 1;
    }
    if (mon < 0 || year < 2024 || year > 2200 || day < 1 || day > month_days(year, mon) || hh < 0 || hh > 23 ||
        mm < 0 || mm > 59 || ss < 0 || ss > 60) {
        return -1;
    }
    return days_from_civil(year, mon, day) * 86400 + hh * 3600 + mm * 60 + ss;
}
