#pragma once

/*
 * POSIX TZ rules ("PST8PDT,M3.2.0,M11.1.0", "<+03>-3", "IST-2IDT,M3.4.4/26,M10.5.0")
 * without libc's single global TZ: the watch converts two zones at once, from
 * the UI task, while the Noise session task validates new ones. Pure C, no
 * ESP-IDF, so tests/ runs it on the host against Python's zoneinfo.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MUSE_TZ_MAX 63   /* longest rule accepted (and stored) */

typedef struct {
    char kind;          /* 'J' Julian 1-365 (no Feb 29), 'D' zero-based 0-365, 'M' month.week.day */
    int16_t a, b, c;    /* J/D: a = day; M: a = month, b = week (5 = last), c = weekday (0 = Sunday) */
    int32_t secs;       /* local time of day of the change, may be negative or past 24 h */
} muse_tz_rule_t;

typedef struct {
    int32_t std_off;    /* seconds east of UTC */
    int32_t dst_off;
    bool has_dst;
    muse_tz_rule_t start, end;
} muse_tz_t;

typedef struct {
    int year, month, day;          /* month 1-12, day 1-31 */
    int hour, min, sec;
    int wday;                      /* 0 = Sunday */
} muse_civil_t;

/* True if `s` is a whole valid rule; `tz` is filled only then. */
bool muse_tz_parse(const char *s, muse_tz_t *tz);

/* Seconds east of UTC at `utc` (Unix time), and whether that's daylight time. */
int32_t muse_tz_offset(const muse_tz_t *tz, int64_t utc, bool *is_dst);

/* An HTTP Date header ("Sun, 06 Nov 1994 08:49:37 GMT") to Unix time; -1 if malformed. */
int64_t muse_http_date(const char *s, size_t len);

/* Unix time (already shifted by an offset) to a calendar date and time. */
void muse_tz_civil(int64_t t, muse_civil_t *out);
