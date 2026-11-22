/* Host harness: parse each Yahoo chart file given; print what the board would keep. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stocks_data.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv)
{
    char b[64];
    stocks_fmt_price(b, sizeof b, 560.84); CHECK(!strcmp(b, "$560.84"));
    stocks_fmt_price(b, sizeof b, 1234567.005); CHECK(!strcmp(b, "$1,234,567.01") || !strcmp(b, "$1,234,567.00"));
    stocks_fmt_price(b, sizeof b, 0.5); CHECK(!strcmp(b, "$0.50"));
    stocks_fmt_change(b, sizeof b, 560.84, 596.22); CHECK(!strcmp(b, "-35.38 (-5.93%)"));
    stocks_fmt_change(b, sizeof b, 110, 100); CHECK(!strcmp(b, "+10.00 (+10.00%)"));
    stocks_fmt_change(b, sizeof b, 100.001, 100); CHECK(!strcmp(b, "+0.00 (+0.00%)"));
    CHECK(stocks_range_find("1w") == RANGE_1W && stocks_range_find("5Y") == RANGE_5Y && stocks_range_find("2D") == -1);
    CHECK(stocks_symbol_find("meta") == 0 && stocks_symbol_find("SPY") == 2 && stocks_symbol_find("AAPL") == -1);
    stocks_url(b, sizeof b, "META", RANGE_1D);
    static stocks_series_t s;
    CHECK(!stocks_parse("{\"chart\":{\"result\":null,\"error\":{\"code\":\"Not Found\"}}}", &s, 100));
    CHECK(!stocks_parse("{\"meta\":{\"regularMarketPrice\":1},\"timestamp\":[1,2],\"indicators\":{\"quote\":[{\"close\":[null,null]}]}}", &s, 100));
    CHECK(stocks_parse("{\"meta\":{\"regularMarketPrice\":5},\"timestamp\":[1,2,3],\"indicators\":{\"quote\":[{\"close\":[1,null,3]}]}}", &s, 100));
    CHECK(s.n == 2 && s.t[1] == 3 && s.close[1] == 3 && s.base == 1);
    CHECK(!stocks_parse("{\"meta\":{\"regularMarketPrice\":5},\"timestamp\":[1,2,3", &s, 100));

    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
        char *buf = malloc(n + 1);
        fread(buf, 1, n, f); buf[n] = 0; fclose(f);
        bool ok = stocks_parse(buf, &s, STOCKS_MAX_POINTS);
        printf("%s ok=%d price=%.2f base=%.2f prev=%.2f n=%d first=%.2f last=%.2f t0=%lld tn=%lld ss=%lld se=%lld off=%d\n",
               argv[i], ok, s.price, s.base, s.prev_close, s.n, s.close[0], s.close[s.n - 1],
               (long long)s.t[0], (long long)s.t[s.n - 1], (long long)s.session_start, (long long)s.session_end, s.gmtoffset);
        free(buf);
    }
    fprintf(stderr, "%s\n", fails ? "FAIL" : "unit OK");
    return fails != 0;
}
