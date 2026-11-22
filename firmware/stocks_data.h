#pragma once

/*
 * The stock tracker's data: Yahoo Finance chart responses parsed without a
 * JSON tree (cJSON's many small nodes would land in internal RAM, which is
 * scarce with live voice). Pure C, no ESP-IDF: tests/ runs it on real
 * responses on the host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STOCKS_COUNT 3
#define STOCKS_MAX_POINTS 280          /* the chart is 360 px wide */
#define STOCKS_SYMBOL_MAX 7

typedef enum { RANGE_1D, RANGE_1W, RANGE_1M, RANGE_1Y, RANGE_5Y, RANGE_COUNT } stocks_range_t;

typedef struct {
    const char *label;       /* "1W" on the chip, and in commands */
    const char *range;       /* Yahoo's range= */
    const char *interval;    /* and interval= */
    const char *period;      /* "Past week", beside the change */
    int stale_open_s;        /* refetch after this while the market's open */
    int stale_closed_s;      /* and while it's closed */
} stocks_range_info_t;

extern const stocks_range_info_t stocks_ranges[RANGE_COUNT];
extern const char *const stocks_symbols[STOCKS_COUNT];

typedef struct {
    double price;            /* regularMarketPrice */
    double base;             /* the close before the range: what the change is measured from */
    double prev_close;       /* the previous session's close (today's change) */
    int64_t market_time;     /* of price */
    int64_t session_start;   /* today's regular session (meaningful on 1D) */
    int64_t session_end;
    int32_t gmtoffset;       /* the exchange's, seconds east of UTC */
    uint16_t n;
    float close[STOCKS_MAX_POINTS];
    int64_t t[STOCKS_MAX_POINTS];
} stocks_series_t;

/* Parses a chart response (NUL-terminated). Keeps at most `max_points`
 * points, evenly thinned, always including the last. False if it isn't a
 * chart with a price and at least two points. */
bool stocks_parse(const char *json, stocks_series_t *out, int max_points);

/* The request path for a symbol and range. */
int stocks_url(char *out, size_t len, const char *symbol, stocks_range_t range);

/* "1W" (any case) to its range; -1 if none. */
int stocks_range_find(const char *label);
/* A tracked symbol's index (any case); -1 if it isn't one. */
int stocks_symbol_find(const char *symbol);

/* "$1,234.56" */
void stocks_fmt_price(char *out, size_t len, double v);
/* "+12.34 (+1.02%)" */
void stocks_fmt_change(char *out, size_t len, double now, double base);
