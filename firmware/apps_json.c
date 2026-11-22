#include "apps_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool json_number(const char *from, const char *to, const char *key, double *out)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(from, pat);
    if (!p || (to && p >= to)) return false;
    p += strlen(pat);
    char *end;
    double v = strtod(p, &end);
    if (end == p || !isfinite(v)) return false;
    *out = v;
    return true;
}

int json_array(const char *from, const char *key, double *out, bool *nulls, int cap)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":[", key);
    const char *p = strstr(from, pat);
    if (!p) return -1;
    p += strlen(pat);
    int n = 0;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']') break;
        if (n >= cap) return -1;
        if (!strncmp(p, "null", 4)) {
            out[n] = NAN;
            nulls[n++] = true;
            p += 4;
            continue;
        }
        char *end;
        double v = strtod(p, &end);
        if (end == p) return -1;
        out[n] = v;
        nulls[n++] = !isfinite(v);
        p = end;
    }
    return *p == ']' ? n : -1;
}
