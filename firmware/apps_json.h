#pragma once

/*
 * Reading the few fields the apps need straight out of a JSON response,
 * without building a cJSON tree (its many small nodes would land in internal
 * RAM). The body must be NUL-terminated. Pure C, tested on the host.
 */

#include <stdbool.h>

/* The number after "key": in [from, to) (to may be NULL); false if absent or null. */
bool json_number(const char *from, const char *to, const char *key, double *out);

/* Parses "key":[n, n, null, ...] found after `from`. A null becomes NaN and
 * nulls[i] true. Returns the count, or -1 if absent, malformed or over cap. */
int json_array(const char *from, const char *key, double *out, bool *nulls, int cap);
