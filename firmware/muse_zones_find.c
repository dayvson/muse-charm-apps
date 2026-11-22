#include <strings.h>

#include "muse_zones.h"

const muse_zone_t *muse_zone_find(const char *what)
{
    if (!what || !*what) return 0;
    for (int i = 0; i < muse_zone_count; i++) {
        const muse_zone_t *z = &muse_zones[i];
        if (!strcasecmp(what, z->code) || !strcasecmp(what, z->name) || !strcasecmp(what, z->iana)) {
            return z;
        }
    }
    return 0;
}
