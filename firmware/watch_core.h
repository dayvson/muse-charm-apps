#pragma once

/*
 * The watch's settings and its commands' logic, without LVGL or FreeRTOS, so
 * tests/ runs it on the host: two zones (work, local), each a dial label, an
 * optional IANA name and a POSIX TZ rule.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "muse_tz.h"

#define WATCH_LABEL_MAX 4
#define WATCH_ZONE_MAX 39
#define WATCH_CLOCK_VALID 1704067200   /* 2024-01-01: before this the clock isn't set */

typedef struct {
    char label[WATCH_LABEL_MAX + 1];   /* "TLV" */
    char zone[WATCH_ZONE_MAX + 1];     /* "Asia/Jerusalem", or "" for a rule Muse gave */
    char tz[MUSE_TZ_MAX + 1];          /* POSIX rule */
} watch_zone_t;

typedef struct {
    uint32_t magic;                    /* WATCH_CFG_MAGIC when stored */
    watch_zone_t work, local;
} watch_cfg_t;

#define WATCH_CFG_MAGIC 0x57415434u    /* "WAT4": local + work (was home), defaults MIA + SFO */

void watch_cfg_default(watch_cfg_t *c);
/* Magic right and both rules parse (a stored blob from NVS). */
bool watch_cfg_valid(const watch_cfg_t *c);

/* watch.set_zone's params applied to *c. False with a message in err (and *c
 * untouched) when they're wrong. */
bool watch_cfg_set_zone(watch_cfg_t *c, const cJSON *params, char *err, size_t err_len);
void watch_cfg_swap(watch_cfg_t *c);

/* {work: {...}, local: {...}, clock_synced}: each zone's label, zone, tz and,
 * once the clock is set, its date, time, weekday, UTC offset, dst, daytime. */
cJSON *watch_cfg_json(const watch_cfg_t *c, int64_t now);

/* 06:00-17:59 counts as day on the dial's sun/moon. */
static inline bool watch_daytime(int hour)
{
    return hour >= 6 && hour < 18;
}
