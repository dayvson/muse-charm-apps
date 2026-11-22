#include "watch_dial.h"

bool watch_dial_rgb565(const watch_indexed_t *d, uint16_t *out)
{
    const size_t n = (size_t)d->w * d->h;
    if (d->size != WATCH_DIAL_PALETTE_BYTES + n) return false;
    uint16_t pal[256];
    for (int i = 0; i < 256; i++) {
        const uint8_t *c = d->data + i * 4;   /* B, G, R, A */
        pal[i] = (uint16_t)(((c[2] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[0] >> 3));
    }
    const uint8_t *idx = d->data + WATCH_DIAL_PALETTE_BYTES;
    for (size_t i = 0; i < n; i++) out[i] = pal[idx[i]];
    return true;
}
