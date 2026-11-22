/* Host test: watch.set_zone/swap/get logic and the dial decoder. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "watch_core.h"
#include "watch_dial.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static bool set(watch_cfg_t *c, const char *json, char *err)
{
    cJSON *p = cJSON_Parse(json);
    bool ok = watch_cfg_set_zone(c, p, err, 640);
    cJSON_Delete(p);
    return ok;
}

int main(int argc, char **argv)
{
    char err[640];
    watch_cfg_t c;
    watch_cfg_default(&c);
    CHECK(watch_cfg_valid(&c));
    CHECK(!strcmp(c.local.label, "MIA") && !strcmp(c.local.zone, "America/New_York") && !strcmp(c.work.label, "SFO"));

    /* HTTP Date headers, for the clock fallback */
    CHECK(muse_http_date("Sat, 21 Nov 2026 19:30:00 GMT", 29) == 1795289400);
    CHECK(muse_http_date("Sun, 29 Feb 2032 00:00:01 GMT", 29) == 1961625601);
    CHECK(muse_http_date("Sat, 21 Nov 2026 19:30:00 UTC", 29) == -1);
    CHECK(muse_http_date("Sat, 31 Nov 2026 19:30:00 GMT", 29) == -1);
    CHECK(muse_http_date("Sat, 21 Xyz 2026 19:30:00 GMT", 29) == -1);
    CHECK(muse_http_date("Sat, 21 Nov 2026 24:30:00 GMT", 29) == -1);
    CHECK(muse_http_date("Sat, 21 Nov 1999 19:30:00 GMT", 29) == -1);
    CHECK(muse_http_date("Sat, 21 Nov 2026 19:30:00 GMT", 28) == -1);
    CHECK(muse_http_date(NULL, 0) == -1);

    /* the user's example: "switch the second time zone to TLV" */
    CHECK(set(&c, "{\"slot\":\"local\",\"zone\":\"TLV\"}", err));
    CHECK(!strcmp(c.local.label, "TLV") && !strcmp(c.local.zone, "Asia/Jerusalem"));
    CHECK(set(&c, "{\"slot\":\"WORK\",\"zone\":\"Tel Aviv\",\"label\":\"tlv2\"}", err));
    CHECK(!strcmp(c.work.label, "TLV2"));
    CHECK(set(&c, "{\"slot\":\"work\",\"zone\":\"america/new_york\"}", err));
    CHECK(!strcmp(c.work.label, "ET"));
    /* relabel only */
    CHECK(set(&c, "{\"slot\":\"local\",\"label\":\"tav\"}", err));
    CHECK(!strcmp(c.local.label, "TAV") && !strcmp(c.local.zone, "Asia/Jerusalem"));
    /* custom rule */
    CHECK(set(&c, "{\"slot\":\"local\",\"zone\":\"Asia/Kathmandu\",\"tz\":\"<+0545>-5:45\",\"label\":\"KTM\"}", err));
    CHECK(!strcmp(c.local.zone, "Asia/Kathmandu") && !strcmp(c.local.tz, "<+0545>-5:45"));
    CHECK(watch_cfg_valid(&c));

    watch_cfg_t before = c;
    CHECK(!set(&c, "{\"zone\":\"TLV\"}", err) && strstr(err, "slot"));
    CHECK(!set(&c, "{\"slot\":\"away\",\"zone\":\"TLV\"}", err));
    CHECK(!set(&c, "{\"slot\":\"home\",\"zone\":\"TLV\"}", err) && strstr(err, "work"));
    CHECK(!set(&c, "{\"slot\":\"local\",\"zone\":\"Atlantis\"}", err) && strstr(err, "TLV") && strstr(err, "tz"));
    CHECK(!set(&c, "{\"slot\":\"local\",\"tz\":\"garbage\",\"label\":\"X\"}", err));
    CHECK(!set(&c, "{\"slot\":\"local\",\"tz\":\"<+04>-4\"}", err) && strstr(err, "label"));
    CHECK(!set(&c, "{\"slot\":\"local\",\"zone\":\"TLV\",\"label\":\"TOOLONG\"}", err));
    CHECK(!set(&c, "{\"slot\":\"local\",\"zone\":\"TLV\",\"label\":\"T-V\"}", err));
    CHECK(!set(&c, "{\"slot\":\"local\",\"zone\":5}", err));
    CHECK(!set(&c, "{\"slot\":\"local\"}", err));
    CHECK(!watch_cfg_set_zone(&c, NULL, err, sizeof(err)));
    CHECK(!memcmp(&before, &c, sizeof(c)));   /* failures leave it alone */

    watch_cfg_default(&c);
    watch_cfg_swap(&c);
    CHECK(!strcmp(c.work.label, "MIA") && !strcmp(c.local.label, "SFO"));

    /* 2026-11-21 19:30:00 UTC: 14:30 in Miami (EST), 11:30 in San Francisco (PST) */
    watch_cfg_default(&c);
    cJSON *j = watch_cfg_json(&c, 1795289400);
    char *s = cJSON_PrintUnformatted(j);
    printf("%s\n", s);
    CHECK(strstr(s, "\"work\":{\"label\":\"SFO\",\"zone\":\"America/Los_Angeles\",\"tz\":\"PST8PDT,M3.2.0,M11.1.0\","
                    "\"date\":\"2026-11-21\",\"time\":\"11:30\",\"weekday\":\"Sat\",\"utc_offset\":\"-08:00\","
                    "\"dst\":false,\"daytime\":true}"));
    CHECK(strstr(s, "\"local\":{\"label\":\"MIA\"") && strstr(s, "\"time\":\"14:30\"") &&
          strstr(s, "\"utc_offset\":\"-05:00\"") && strstr(s, "\"clock_synced\":true"));
    free(s);
    cJSON_Delete(j);
    j = watch_cfg_json(&c, 1000);
    s = cJSON_PrintUnformatted(j);
    CHECK(!strstr(s, "time\"") && strstr(s, "\"clock_synced\":false"));
    free(s);
    cJSON_Delete(j);

    /* stored blob validation */
    watch_cfg_default(&c);
    c.magic = 0;
    CHECK(!watch_cfg_valid(&c));
    watch_cfg_default(&c);
    memset(c.work.tz, 'A', sizeof(c.work.tz));
    CHECK(!watch_cfg_valid(&c));

    /* dial: the I8 image's pixels, dumped as PPM for the Python side to compare */
    size_t n = (size_t)watch_dial.w * watch_dial.h;
    uint16_t *px = malloc(n * 2);
    watch_indexed_t cut = watch_dial;
    cut.size -= 1;
    CHECK(!watch_dial_rgb565(&cut, px));
    CHECK(watch_dial_rgb565(&watch_dial, px));
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        fprintf(f, "P6 %d %d 255\n", watch_dial.w, watch_dial.h);
        for (size_t i = 0; i < n; i++) {
            uint16_t v = px[i];
            unsigned char rgb[3] = {(unsigned char)((v >> 11) << 3), (unsigned char)(((v >> 5) & 63) << 2),
                                    (unsigned char)((v & 31) << 3)};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    free(px);
    printf("%s\n", fails ? "FAIL" : "OK");
    return fails != 0;
}
