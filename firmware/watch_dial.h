#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "watch_assets.h"

/* The I8 dial as the screen shows it: w*h RGB565 pixels (host tests; the board
 * hands the image to LVGL as is). False if it's malformed. */
bool watch_dial_rgb565(const watch_indexed_t *d, uint16_t *out);
