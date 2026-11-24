#include "apps_net.h"

#include <string.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "apps_net";

#define RESPONSE_MAX (64 * 1024)   /* the largest (stocks 5Y weekly) is ~30 KB */
#define TASK_STACK (16 * 1024)     /* TLS handshake; in PSRAM */
/* TLS (mbedTLS) and lwIP keep their buffers in PSRAM; what a fetch takes from
 * internal RAM is small pieces (the client's 1 KB and 512 B buffers, the
 * socket's bookkeeping), none near 4 KB. Leave Muse's sessions room: skip a
 * fetch below these. (Idle with Muse connected and Lua apps built in, the
 * board sits around 23 KB free, 7 KB largest block, and live calls work.) */
#define MIN_INTERNAL_FREE (16 * 1024)
#define MIN_INTERNAL_BLOCK (4 * 1024)

/* One client, kept between fetches: a fetch to the same host within IDLE_MS
 * reuses its connection (no TCP or TLS handshake), and a later one resumes the
 * TLS session (session tickets) instead of a full handshake. All on the fetch
 * task, so no locking. */
#define IDLE_MS 20000             /* close the connection after this long unused */
#define DROP_MS (5 * 60 * 1000)   /* and free the client after this long */
#define HEADERS_MAX 8

typedef struct {
    apps_job_fn fn;
    uint32_t arg;
} job_t;

/* In PSRAM like the rest: internal RAM is what Muse's link needs. */
EXT_RAM_BSS_ATTR static esp_http_client_handle_t s_client;
EXT_RAM_BSS_ATTR static bool s_open;                   /* s_client may hold a connection */
EXT_RAM_BSS_ATTR static char s_hdr[HEADERS_MAX][48];   /* the last fetch's extra headers, removed before the next */

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static char *s_body;
static size_t s_len;
static bool s_overflow;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
        if (s_len + e->data_len < RESPONSE_MAX) {
            memcpy(s_body + s_len, e->data, e->data_len);
            s_len += e->data_len;
        } else {
            s_overflow = true;
        }
    }
    return ESP_OK;
}

int apps_net_get(const char *url, const char **body, size_t *len)
{
    return apps_net_request("GET", url, NULL, NULL, 0, body, len);
}

int apps_net_request(const char *method, const char *url, const char *headers, const char *data, size_t data_len,
                     const char **body, size_t *len)
{
    *body = NULL;
    *len = 0;
    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t int_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (int_free < MIN_INTERNAL_FREE || int_block < MIN_INTERNAL_BLOCK) {
        ESP_LOGW(TAG, "skipped: internal RAM %uK/%uK", (unsigned)(int_free / 1024), (unsigned)(int_block / 1024));
        return -1;
    }
    if (!s_body) s_body = heap_caps_malloc(RESPONSE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   /* retried each fetch */
    if (!s_body) {
        ESP_LOGW(TAG, "skipped: no PSRAM for the response");
        return -1;
    }
    s_len = 0;
    s_overflow = false;
    if (!s_client) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 12000,
            .buffer_size = 1024,      /* internal RAM: keep them small */
            .buffer_size_tx = 512,
            .user_agent = "Mozilla/5.0",
            .event_handler = on_http,
            .crt_bundle_attach = esp_crt_bundle_attach,
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
            .save_client_session = true,
#endif
        };
        s_client = esp_http_client_init(&cfg);
        if (!s_client) return 0;
        memset(s_hdr, 0, sizeof(s_hdr));
    } else if (esp_http_client_set_url(s_client, url) != ESP_OK) {   /* closes it if the host changed */
        return 0;
    }
    esp_http_client_handle_t c = s_client;
    static const struct { const char *name; esp_http_client_method_t m; } METHODS[] = {
        {"GET", HTTP_METHOD_GET}, {"POST", HTTP_METHOD_POST}, {"PUT", HTTP_METHOD_PUT},
        {"PATCH", HTTP_METHOD_PATCH}, {"DELETE", HTTP_METHOD_DELETE}, {"HEAD", HTTP_METHOD_HEAD},
    };
    esp_http_client_set_method(c, HTTP_METHOD_GET);
    for (size_t i = 0; method && i < sizeof(METHODS) / sizeof(METHODS[0]); i++) {
        if (!strcmp(method, METHODS[i].name)) esp_http_client_set_method(c, METHODS[i].m);
    }
    for (int i = 0; i < HEADERS_MAX && s_hdr[i][0]; i++) {
        esp_http_client_delete_header(c, s_hdr[i]);
        s_hdr[i][0] = 0;
    }
    /* "Name: value" lines, separated by \n. */
    int nh = 0;
    for (const char *h = headers; h && *h;) {
        const char *eol = strchr(h, '\n'), *end = eol ? eol : h + strlen(h);
        const char *colon = memchr(h, ':', (size_t)(end - h));
        if (colon && colon > h && end - h < 512 && colon - h < (int)sizeof(s_hdr[0]) && nh < HEADERS_MAX) {
            char line[512];
            memcpy(line, h, (size_t)(end - h));
            line[end - h] = 0;
            line[colon - h] = 0;
            const char *v = line + (colon - h) + 1;
            while (*v == ' ') v++;
            esp_http_client_set_header(c, line, v);
            strlcpy(s_hdr[nh++], line, sizeof(s_hdr[0]));
        }
        h = eol ? eol + 1 : end;
    }
    esp_http_client_set_post_field(c, data && data_len ? data : NULL, data && data_len ? (int)data_len : 0);
    bool reused = s_open;
    esp_err_t err = esp_http_client_perform(c);
    if (err != ESP_OK && reused && s_len == 0) {
        /* The server closed the kept connection while it sat idle: nothing was
         * answered, so it's safe to send once more on a fresh one. */
        ESP_LOGI(TAG, "kept connection was closed (%s): reconnecting", esp_err_to_name(err));
        esp_http_client_close(c);
        s_overflow = false;
        err = esp_http_client_perform(c);
    }
    int status = esp_http_client_get_status_code(c);
    if (err != ESP_OK) {
        esp_http_client_close(c);
        s_open = false;
    } else {
        s_open = true;   /* the client closes it itself on "Connection: close" */
    }
    s_body[s_len] = 0;
    ESP_LOGI(TAG, "%s %d, %u bytes (internal RAM %uK/%uK before, %uK after, min %uK)",
             method ? method : "GET", status, (unsigned)s_len, (unsigned)(int_free / 1024), (unsigned)(int_block / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
    if (err != ESP_OK || s_overflow) return 0;
    *body = s_body;
    *len = s_len;
    return status;
}

static void worker(void *arg)
{
    (void)arg;
    s_body = heap_caps_malloc(RESPONSE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (;;) {
        job_t j;
        TickType_t wait = !s_client ? portMAX_DELAY : pdMS_TO_TICKS(s_open ? IDLE_MS : DROP_MS - IDLE_MS);
        if (xQueueReceive(s_queue, &j, wait) != pdTRUE) {
            if (s_open) {          /* unused for IDLE_MS: give back the socket and TLS buffers */
                esp_http_client_close(s_client);
                s_open = false;
            } else if (s_client) { /* and the client itself (and its session ticket) later */
                esp_http_client_cleanup(s_client);
                s_client = NULL;
            }
            continue;
        }
        j.fn(j.arg);
    }
}

bool apps_net_submit(apps_job_fn fn, uint32_t arg)
{
    if (!s_queue) s_queue = xQueueCreate(24, sizeof(job_t));
    if (!s_queue) return false;
    if (!s_task && xTaskCreatePinnedToCoreWithCaps(worker, "apps_net", TASK_STACK, NULL, 3, &s_task, tskNO_AFFINITY,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "couldn't start the fetch task");
        return false;
    }
    job_t j = {fn, arg};
    return xQueueSend(s_queue, &j, 0) == pdTRUE;
}
