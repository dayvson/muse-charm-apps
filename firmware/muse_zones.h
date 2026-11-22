#pragma once

/* Built-in time zones: what Muse can name without a POSIX TZ string. */
typedef struct {
    const char *code;   /* shown on the dial, up to 4 characters: "TLV" */
    const char *iana;   /* "Asia/Jerusalem" */
    const char *name;   /* "Tel Aviv" */
    const char *tz;     /* POSIX TZ rule: "IST-2IDT,M3.4.4/26,M10.5.0" */
} muse_zone_t;

extern const muse_zone_t muse_zones[];
extern const int muse_zone_count;

/* By code, city name or IANA name, ignoring case; NULL when unknown. */
const muse_zone_t *muse_zone_find(const char *what);
