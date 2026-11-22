/*
 * The stock tracker's fetches: (symbol, range) jobs on the apps' fetch task
 * (apps_net.c) get the chart from Yahoo Finance, parse it in place and store
 * it for the UI. The response, the parse and the store are all PSRAM.
 */

#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "apps_net.h"
#include "stocks_net.h"

static const char *TAG = "stocks";

typedef struct {
    stocks_series_t s;
    stocks_slot_info_t info;
} slot_t;

EXT_RAM_BSS_ATTR static slot_t s_slots[STOCKS_COUNT][RANGE_COUNT];
static SemaphoreHandle_t s_lock;

/* A job on the apps' fetch task: arg = symbol << 8 | range. */
static void job(uint32_t arg)
{
    int sym = (int)(arg >> 8), range = (int)(arg & 0xff);
    stocks_series_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char url[160];
    stocks_url(url, sizeof(url), stocks_symbols[sym], (stocks_range_t)range);
    const char *body;
    size_t len;
    int64_t t0 = esp_timer_get_time();
    int status = s ? apps_net_get(url, &body, &len) : 0;
    bool ok = status == 200 && stocks_parse(body, s, STOCKS_MAX_POINTS);
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot_t *slot = &s_slots[sym][range];
    slot->info.pending = false;
    slot->info.tried_us = now;
    slot->info.failed = !ok;
    if (ok) {
        memcpy(&slot->s, s, sizeof(*s));
        slot->info.ok = true;
        slot->info.fetched_us = now;
        slot->info.version++;
    }
    xSemaphoreGive(s_lock);
    if (ok) {
        ESP_LOGI(TAG, "%s %s: $%.2f, %d points, %d ms", stocks_symbols[sym], stocks_ranges[range].label, s->price,
                 s->n, (int)((now - t0) / 1000));
    } else if (status >= 0) {
        ESP_LOGW(TAG, "%s %s: failed (HTTP %d)", stocks_symbols[sym], stocks_ranges[range].label, status);
    }
    heap_caps_free(s);
}

void stocks_net_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

bool stocks_net_request(int sym, int range)
{
    if (sym < 0 || sym >= STOCKS_COUNT || range < 0 || range >= RANGE_COUNT || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot_t *slot = &s_slots[sym][range];
    bool queued = false;
    if (!slot->info.pending) {
        queued = apps_net_submit(job, (uint32_t)sym << 8 | (uint32_t)range);
        slot->info.pending = queued;
    }
    xSemaphoreGive(s_lock);
    return queued;
}

stocks_slot_info_t stocks_net_info(int sym, int range)
{
    stocks_slot_info_t info = {0};
    if (!s_lock) return info;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    info = s_slots[sym][range].info;
    xSemaphoreGive(s_lock);
    return info;
}

bool stocks_net_copy(int sym, int range, stocks_series_t *out, stocks_slot_info_t *info)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot_t *slot = &s_slots[sym][range];
    bool ok = slot->info.ok;
    if (ok && out) memcpy(out, &slot->s, sizeof(*out));
    if (info) *info = slot->info;
    xSemaphoreGive(s_lock);
    return ok;
}
