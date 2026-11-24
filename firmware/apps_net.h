#pragma once

/*
 * One fetch task for every app: jobs run one at a time on a low-priority task
 * with a PSRAM stack, so at most one HTTPS connection (whose socket and
 * buffers take internal RAM) is ever open for the apps. It stays open for 20 s
 * after a fetch, so the next one to the same host skips the TCP and TLS
 * handshakes; later ones resume the TLS session.
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

/* Any method (GET, POST, PUT, PATCH, DELETE, HEAD), with extra headers
 * ("Name: value" lines separated by \n, or NULL) and a body (or NULL). */
int apps_net_request(const char *method, const char *url, const char *headers, const char *data, size_t data_len,
                     const char **body, size_t *len);
