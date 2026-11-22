#pragma once

/*
 * One fetch task for every app: jobs run one at a time on a low-priority task
 * with a PSRAM stack, so at most one HTTPS connection (whose socket and
 * buffers take internal RAM) is ever open for the apps.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*apps_job_fn)(uint32_t arg);

/* Queue a job (starts the task the first time). */
bool apps_net_submit(apps_job_fn fn, uint32_t arg);

/* From a job: GET `url` into the task's PSRAM buffer, NUL-terminated.
 * Returns the HTTP status, 0 if it failed, or -1 if skipped because internal
 * RAM is too low to open a connection without crowding Muse's sessions. */
int apps_net_get(const char *url, const char **body, size_t *len);
