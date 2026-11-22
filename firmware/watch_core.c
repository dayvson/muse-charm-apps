#include "watch_core.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "muse_zones.h"

static const char *const WDAY[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static void set_from_zone(watch_zone_t *z, const muse_zone_t *mz)
{
    memset(z, 0, sizeof(*z));
    strlcpy(z->label, mz->code, sizeof(z->label));
    strlcpy(z->zone, mz->iana, sizeof(z->zone));
    strlcpy(z->tz, mz->tz, sizeof(z->tz));
}

void watch_cfg_default(watch_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->magic = WATCH_CFG_MAGIC;
    set_from_zone(&c->local, muse_zone_find("MIA"));   /* where the user lives */
    set_from_zone(&c->work, muse_zone_find("SFO"));    /* the zone they follow */
}

static bool zone_valid(const watch_zone_t *z)
{
    muse_tz_t tz;
    return memchr(z->label, 0, sizeof(z->label)) && z->label[0] &&
           memchr(z->zone, 0, sizeof(z->zone)) &&
           memchr(z->tz, 0, sizeof(z->tz)) && muse_tz_parse(z->tz, &tz);
}

bool watch_cfg_valid(const watch_cfg_t *c)
{
    return c->magic == WATCH_CFG_MAGIC && zone_valid(&c->work) && zone_valid(&c->local);
}

static const char *str_param(const cJSON *params, const char *key, bool *wrong_type)
{
    const cJSON *item = params ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
    if (!item || cJSON_IsNull(item)) return NULL;
    if (!cJSON_IsString(item) || !item->valuestring) {
        *wrong_type = true;
        return NULL;
    }
    return item->valuestring[0] ? item->valuestring : NULL;
}

static bool clean_label(const char *in, char out[WATCH_LABEL_MAX + 1])
{
    size_t n = strlen(in);
    if (n == 0 || n > WATCH_LABEL_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)in[i])) return false;
        out[i] = (char)toupper((unsigned char)in[i]);
    }
    out[n] = 0;
    return true;
}

static void known_codes(char *out, size_t len)
{
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < muse_zone_count && o + 6 < len; i++) {
        o += snprintf(out + o, len - o, "%s%s", i ? " " : "", muse_zones[i].code);
    }
}

bool watch_cfg_set_zone(watch_cfg_t *c, const cJSON *params, char *err, size_t err_len)
{
    bool wrong_type = false;
    const char *slot = str_param(params, "slot", &wrong_type);
    const char *zone = str_param(params, "zone", &wrong_type);
    const char *tz = str_param(params, "tz", &wrong_type);
    const char *label = str_param(params, "label", &wrong_type);
    if (wrong_type) {
        snprintf(err, err_len, "slot, zone, tz and label must be strings");
        return false;
    }
    watch_zone_t *target;
    if (slot && !strcasecmp(slot, "work")) {
        target = &c->work;
    } else if (slot && !strcasecmp(slot, "local")) {
        target = &c->local;
    } else {
        snprintf(err, err_len, "slot is required: \"local\" (where the user is) or \"work\" (the zone they follow)");
        return false;
    }

    watch_zone_t z = *target;
    const muse_zone_t *mz = zone ? muse_zone_find(zone) : NULL;
    if (mz) {
        set_from_zone(&z, mz);
    } else if (tz) {
        muse_tz_t parsed;
        if (!muse_tz_parse(tz, &parsed)) {
            snprintf(err, err_len, "tz \"%.70s\" isn't a POSIX TZ rule (e.g. \"IST-2IDT,M3.4.4/26,M10.5.0\")", tz);
            return false;
        }
        if (!label) {
            snprintf(err, err_len, "label (up to %d letters) is required with tz", WATCH_LABEL_MAX);
            return false;
        }
        memset(&z, 0, sizeof(z));
        strlcpy(z.tz, tz, sizeof(z.tz));
        if (zone && strlen(zone) <= WATCH_ZONE_MAX) strlcpy(z.zone, zone, sizeof(z.zone));
    } else if (zone) {
        char codes[400];
        known_codes(codes, sizeof(codes));
        snprintf(err, err_len,
                 "unknown zone \"%.40s\". Built in: %s (also their city or IANA names). "
                 "For anywhere else pass tz, a POSIX TZ rule (the last line of /usr/share/zoneinfo/<Area/City>), and label",
                 zone, codes);
        return false;
    } else if (!label) {
        snprintf(err, err_len, "pass zone (e.g. \"TLV\"), or tz and label, or a new label");
        return false;
    }
    if (label && !clean_label(label, z.label)) {
        snprintf(err, err_len, "label must be 1 to %d letters or digits", WATCH_LABEL_MAX);
        return false;
    }
    *target = z;
    return true;
}

void watch_cfg_swap(watch_cfg_t *c)
{
    watch_zone_t t = c->work;
    c->work = c->local;
    c->local = t;
}

static cJSON *zone_json(const watch_zone_t *z, int64_t now)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "label", z->label);
    if (z->zone[0]) cJSON_AddStringToObject(o, "zone", z->zone);
    cJSON_AddStringToObject(o, "tz", z->tz);
    muse_tz_t tz;
    if (now >= WATCH_CLOCK_VALID && muse_tz_parse(z->tz, &tz)) {
        bool dst;
        int32_t off = muse_tz_offset(&tz, now, &dst);
        muse_civil_t t;
        muse_tz_civil(now + off, &t);
        char buf[16];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.year, t.month, t.day);
        cJSON_AddStringToObject(o, "date", buf);
        snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.min);
        cJSON_AddStringToObject(o, "time", buf);
        cJSON_AddStringToObject(o, "weekday", WDAY[t.wday]);
        int a = off < 0 ? -off : off;
        snprintf(buf, sizeof(buf), "%c%02d:%02d", off < 0 ? '-' : '+', a / 3600, a / 60 % 60);
        cJSON_AddStringToObject(o, "utc_offset", buf);
        cJSON_AddBoolToObject(o, "dst", dst);
        cJSON_AddBoolToObject(o, "daytime", watch_daytime(t.hour));
    }
    return o;
}

cJSON *watch_cfg_json(const watch_cfg_t *c, int64_t now)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "work", zone_json(&c->work, now));
    cJSON_AddItemToObject(o, "local", zone_json(&c->local, now));
    cJSON_AddBoolToObject(o, "clock_synced", now >= WATCH_CLOCK_VALID);
    return o;
}
