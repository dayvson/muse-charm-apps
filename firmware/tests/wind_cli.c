/* Host harness: unit checks, then each Open-Meteo file given -> what the board keeps. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wind_data.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static wind_fc_t f;

int main(int argc, char **argv)
{
    CHECK(!strcmp(wind_compass(0), "N") && !strcmp(wind_compass(112), "ESE") && !strcmp(wind_compass(355), "N") &&
          !strcmp(wind_compass(191), "S") && !strcmp(wind_compass(-1), "--"));
    CHECK(wind_rate(9.9f) == RATE_LIGHT && wind_rate(12) == RATE_MARGINAL && wind_rate(18) == RATE_GO &&
          wind_rate(28) == RATE_STRONG && wind_rate(40) == RATE_NUKING);
    char b[16];
    wind_fmt_hour(b, sizeof b, 0); CHECK(!strcmp(b, "12 AM"));
    wind_fmt_hour(b, sizeof b, 13); CHECK(!strcmp(b, "1 PM"));
    CHECK(!wind_parse("{\"hourly\":{}}", &f));
    CHECK(!wind_parse(NULL, &f));

    /* synthetic day: UTC, 24 hours from midnight; GO from 12 to 15, marginal at 9 */
    char json[8192], *p = json;
    p += snprintf(p, (size_t)(json + sizeof json - p), "{\"utc_offset_seconds\":0,\"hourly\":{\"time\":[");
    for (int h = 0; h < 48; h++) p += snprintf(p, (size_t)(json + sizeof json - p), "%s%d", h ? "," : "", 86400 * 100 + h * 3600);
    p += snprintf(p, (size_t)(json + sizeof json - p), "],\"wind_speed_10m\":[");
    for (int h = 0; h < 48; h++) p += snprintf(p, (size_t)(json + sizeof json - p), "%s%d", h ? "," : "", (h >= 12 && h <= 15) ? 18 : h == 9 ? 12 : 5);
    p += snprintf(p, (size_t)(json + sizeof json - p), "],\"wind_direction_10m\":[");
    for (int h = 0; h < 48; h++) p += snprintf(p, (size_t)(json + sizeof json - p), "%s90", h ? "," : "");
    p += snprintf(p, (size_t)(json + sizeof json - p), "],\"wind_gusts_10m\":[");
    for (int h = 0; h < 48; h++) p += snprintf(p, (size_t)(json + sizeof json - p), "%s%s", h ? "," : "", h == 3 ? "null" : "20");
    p += snprintf(p, (size_t)(json + sizeof json - p), "]},\"daily\":{\"time\":[%d,%d],\"wind_speed_10m_max\":[18,5],\"wind_gusts_10m_max\":[20,8],"
                    "\"wind_direction_10m_dominant\":[90,null]}}", 86400 * 100, 86400 * 101);
    CHECK(wind_parse(json, &f));
    CHECK(f.nh == 48 && f.nd == 2 && !f.has_now && f.hg[3] == f.hs[3] && f.dd[1] == -1);
    int h0, h1; float lo, hi;
    CHECK(wind_best_window(&f, 0, 0, &h0, &h1, &lo, &hi) && h0 == 12 && h1 == 15 && lo == 18 && hi == 18);
    CHECK(wind_best_window(&f, 0, 86400 * 100 + 14 * 3600 + 60, &h0, &h1, &lo, &hi) && h0 == 14 && h1 == 15);
    CHECK(!wind_best_window(&f, 1, 0, &h0, &h1, &lo, &hi));
    CHECK(wind_best_day(&f) == 0);
    int idx[24];
    CHECK(wind_day_hours(&f, 0, 6, 21, idx, 24) == 16 && wind_local_hour(&f, idx[0]) == 6);

    for (int i = 1; i < argc; i++) {
        FILE *fp = fopen(argv[i], "rb");
        fseek(fp, 0, SEEK_END); long n = ftell(fp); rewind(fp);
        char *buf = malloc(n + 1);
        fread(buf, 1, n, fp); buf[n] = 0; fclose(fp);
        bool ok = wind_parse(buf, &f);
        int bw = wind_best_window(&f, 1, 0, &h0, &h1, &lo, &hi);
        int nt = wind_day_hours(&f, 1, 6, 21, idx, 24);
        printf("%s ok=%d off=%d now=%.1f gust=%.1f dir=%d nh=%d nd=%d h0s=%.1f hlast=%.1f d0=%.1f dlast=%.1f g0=%.1f dd0=%d "
               "tmrw_hours=%d tmrw_first=%d best=%d bestday=%d wd0=%s\n",
               argv[i], ok, f.utc_offset, f.now_kn, f.now_gust, f.now_dir, f.nh, f.nd, f.hs[0], f.hs[f.nh - 1], f.ds[0],
               f.ds[f.nd - 1], f.dg[0], f.dd[0], nt, nt ? wind_local_hour(&f, idx[0]) : -1, bw ? h0 * 100 + h1 : -1,
               wind_best_day(&f), wind_weekday(&f, 0));
        free(buf);
    }
    fprintf(stderr, "%s\n", fails ? "FAIL" : "unit OK");
    return fails != 0;
}
