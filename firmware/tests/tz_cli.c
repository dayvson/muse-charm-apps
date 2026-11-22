/* Host harness: lines of "<posix> <utc>" in, "<offset> <dst>" out ("ERR" if the rule doesn't parse). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_tz.h"
#include "muse_zones.h"

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "find")) {
        for (int i = 2; i < argc; i++) {
            const muse_zone_t *z = muse_zone_find(argv[i]);
            printf("%s -> %s\n", argv[i], z ? z->code : "(none)");
        }
        return 0;
    }
    char tzs[256];
    long long utc;
    while (scanf("%255s %lld", tzs, &utc) == 2) {
        muse_tz_t tz;
        if (!muse_tz_parse(tzs, &tz)) { puts("ERR"); continue; }
        bool dst;
        int off = muse_tz_offset(&tz, utc, &dst);
        muse_civil_t c;
        muse_tz_civil(utc + off, &c);
        printf("%d %d %04d-%02d-%02d %02d:%02d:%02d %d\n", off, dst, c.year, c.month, c.day, c.hour, c.min, c.sec, c.wday);
    }
    return 0;
}
