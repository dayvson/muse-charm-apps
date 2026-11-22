#include "apps_net.h"

#include <string.h>

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

typedef struct {
    apps_job_fn fn;
    uint32_t arg;
} job_t;

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
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 12000,
        .buffer_size = 1024,      /* internal RAM: keep them small */
        .buffer_size_tx = 512,
        .user_agent = "Mozilla/5.0",
        .event_handler = on_http,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return 0;
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    s_body[s_len] = 0;
    ESP_LOGI(TAG, "HTTP %d, %u bytes (internal RAM %uK/%uK before, %uK after, min %uK)", status, (unsigned)s_len,
             (unsigned)(int_free / 1024), (unsigned)(int_block / 1024),
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
        xQueueReceive(s_queue, &j, portMAX_DELAY);
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
