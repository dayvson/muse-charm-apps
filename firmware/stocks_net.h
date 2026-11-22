#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "stocks_data.h"

typedef struct {
    bool ok;              /* has data */
    bool failed;          /* the last attempt failed */
    bool pending;         /* queued or being fetched */
    uint32_t version;     /* bumped by each successful fetch */
    int64_t fetched_us;   /* esp_timer time of the data */
    int64_t tried_us;     /* and of the last attempt */
} stocks_slot_info_t;

void stocks_net_init(void);
/* Queue a fetch (starts the task the first time); false if already pending or full. */
bool stocks_net_request(int sym, int range);
stocks_slot_info_t stocks_net_info(int sym, int range);
/* Copy a slot's data (out may be NULL); false if it has none yet. */
bool stocks_net_copy(int sym, int range, stocks_series_t *out, stocks_slot_info_t *info);
