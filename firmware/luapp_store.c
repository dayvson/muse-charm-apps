#include "luapp_store.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "luapp_store";

#define ROOT "/luapps"
#define LABEL "luapps"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static luapp_meta_t s_index[LUAPP_MAX_APPS];   /* under s_mux */
static int s_count;
static uint32_t s_version;
static bool s_mounted;

/* File operations come from the Lua task and, for uploads, Muse's command
 * task: one at a time (FAT here has max_files = 3 and no locking of its own). */
static StaticSemaphore_t s_fs_buf;
static SemaphoreHandle_t s_fs;
static portMUX_TYPE s_fs_init = portMUX_INITIALIZER_UNLOCKED;

static void fs_lock(void)
{
    taskENTER_CRITICAL(&s_fs_init);
    if (!s_fs) s_fs = xSemaphoreCreateRecursiveMutexStatic(&s_fs_buf);
    taskEXIT_CRITICAL(&s_fs_init);
    xSemaphoreTakeRecursive(s_fs, portMAX_DELAY);
}

static void fs_unlock(void) { xSemaphoreGiveRecursive(s_fs); }

static void path(char *out, size_t len, const char *id, const char *ext)
{
    snprintf(out, len, ROOT "/%s.%s", id, ext);
}

bool luapp_id_valid(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n == 0 || n > LUAPP_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        if (!(islower((unsigned char)id[i]) || isdigit((unsigned char)id[i]) || id[i] == '_')) return false;
    }
    return true;
}

bool luapp_name_valid(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n > LUAPP_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        if (name[i] < 0x20 || name[i] > 0x7e) return false;
    }
    return true;
}

static char *read_file(const char *p, size_t max, size_t *len)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = NULL;
    if (n >= 0 && (size_t)n <= max) {
        buf = heap_caps_malloc((size_t)n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
            buf[n] = 0;
            if (len) *len = (size_t)n;
        } else {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    return buf;
}

static bool write_file(const char *p, const char *data, size_t len, const char *mode)
{
    FILE *f = fopen(p, mode);
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    return ok;
}

static bool read_meta(const char *id, luapp_meta_t *m)
{
    char p[48];
    memset(m, 0, sizeof(*m));
    strlcpy(m->id, id, sizeof(m->id));
    strlcpy(m->name, id, sizeof(m->name));
    m->color = 0x27d3a2;
    path(p, sizeof(p), id, "meta");
    char *meta = read_file(p, 128, NULL);
    if (meta) {
        char *nl = strchr(meta, '\n');
        if (nl) {
            *nl = 0;
            if (luapp_name_valid(meta)) strlcpy(m->name, meta, sizeof(m->name));
            m->color = (uint32_t)strtoul(nl + 1, NULL, 0) & 0xffffff;
        }
        free(meta);
    }
    path(p, sizeof(p), id, "lua");
    struct stat st;
    if (stat(p, &st) != 0) return false;
    m->size = (uint32_t)st.st_size;
    return true;
}

static void rescan(void)
{
    luapp_meta_t found[LUAPP_MAX_APPS];
    int n = 0;
    DIR *d = opendir(ROOT);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < LUAPP_MAX_APPS) {
            char id[LUAPP_ID_MAX + 8];
            strlcpy(id, e->d_name, sizeof(id));
            char *dot = strrchr(id, '.');
            if (!dot || strcasecmp(dot, ".lua")) continue;
            *dot = 0;
            for (char *c = id; *c; c++) *c = (char)tolower((unsigned char)*c);   /* FAT may report upper case */
            if (luapp_id_valid(id) && read_meta(id, &found[n])) n++;
        }
        closedir(d);
    }
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_index, found, sizeof(found[0]) * n);
    s_count = n;
    s_version++;
    taskEXIT_CRITICAL(&s_mux);
}

static bool fs_mount(void)
{
    if (s_mounted) return true;
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 3,
        .allocation_unit_size = 4096,
    };
    wl_handle_t wl;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(ROOT, LABEL, &cfg, &wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount: %s (is the luapps partition in the table?)", esp_err_to_name(err));
        return false;
    }
    s_mounted = true;
    rescan();
    ESP_LOGI(TAG, "mounted " ROOT ": %d app(s)", s_count);
    return true;
}

bool luapp_store_mounted(void) { return s_mounted; }

int luapp_store_list(luapp_meta_t *out, int cap)
{
    taskENTER_CRITICAL(&s_mux);
    int n = s_count < cap ? s_count : cap;
    memcpy(out, s_index, sizeof(out[0]) * n);
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

bool luapp_store_find(const char *id, luapp_meta_t *out)
{
    bool found = false;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_count && !found; i++) {
        if (!strcmp(s_index[i].id, id)) {
            *out = s_index[i];
            found = true;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return found;
}

uint32_t luapp_store_version(void)
{
    taskENTER_CRITICAL(&s_mux);
    uint32_t v = s_version;
    taskEXIT_CRITICAL(&s_mux);
    return v;
}

static bool fs_tmp_write(const char *id, const char *data, size_t len, bool append, size_t *total)
{
    char p[48];
    path(p, sizeof(p), id, "tmp");
    if (!write_file(p, data, len, append ? "ab" : "wb")) return false;
    struct stat st;
    if (stat(p, &st) != 0) return false;
    *total = (size_t)st.st_size;
    return true;
}

static char *fs_tmp_read(const char *id, size_t *len)
{
    char p[48];
    path(p, sizeof(p), id, "tmp");
    return read_file(p, LUAPP_CODE_MAX, len);
}

static void fs_tmp_discard(const char *id)
{
    char p[48];
    path(p, sizeof(p), id, "tmp");
    remove(p);
}

static bool fs_commit(const char *id, const char *name, uint32_t color)
{
    char tmp[48], lua[48], meta[48], m[64];
    path(tmp, sizeof(tmp), id, "tmp");
    path(lua, sizeof(lua), id, "lua");
    path(meta, sizeof(meta), id, "meta");
    remove(lua);
    if (rename(tmp, lua) != 0) return false;
    int n = snprintf(m, sizeof(m), "%s\n0x%06lx\n", name, (unsigned long)(color & 0xffffff));
    write_file(meta, m, (size_t)n, "wb");
    rescan();
    return true;
}

static char *fs_code(const char *id, size_t *len)
{
    char p[48];
    path(p, sizeof(p), id, "lua");
    return read_file(p, LUAPP_CODE_MAX, len);
}

static bool fs_remove(const char *id)
{
    static const char *const exts[] = {"lua", "meta", "kv", "tmp", "ico"};
    char p[48];
    path(p, sizeof(p), id, "lua");
    bool existed = remove(p) == 0;
    for (int i = 1; i < 5; i++) {
        path(p, sizeof(p), id, exts[i]);
        remove(p);
    }
    rescan();
    return existed;
}

static char *fs_kv_read(const char *id, size_t *len)
{
    char p[48];
    path(p, sizeof(p), id, "kv");
    return read_file(p, LUAPP_KV_MAX, len);
}

static bool fs_kv_write(const char *id, const char *json, size_t len)
{
    if (len > LUAPP_KV_MAX) return false;
    char p[48];
    path(p, sizeof(p), id, "kv");
    return write_file(p, json, len, "wb");
}

static bool fs_icon_read(const char *id, uint8_t *buf, size_t n)
{
    char p[48];
    path(p, sizeof(p), id, "ico");
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    bool ok = fread(buf, 1, n, f) == n && fgetc(f) == EOF;
    fclose(f);
    return ok;
}

static bool fs_icon_write(const char *id, const uint8_t *buf, size_t n)
{
    char p[48];
    path(p, sizeof(p), id, "ico");
    FILE *f = fopen(p, "rb");
    if (f) {
        uint8_t chunk[256];
        size_t off = 0, got;
        bool same = true;
        while (same && (got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            same = off + got <= n && !memcmp(chunk, buf + off, got);
            off += got;
        }
        fclose(f);
        if (same && off == n) return true;
    }
    return write_file(p, (const char *)buf, n, "wb");
}

static void fs_icon_remove(const char *id)
{
    char p[48];
    path(p, sizeof(p), id, "ico");
    remove(p);
}

/* ---- the public file operations, one at a time ---- */

#define LOCKED(type, call) \
    do {                   \
        fs_lock();         \
        type r_ = call;    \
        fs_unlock();       \
        return r_;         \
    } while (0)

bool luapp_store_mount(void) { LOCKED(bool, fs_mount()); }
bool luapp_store_tmp_write(const char *id, const char *data, size_t len, bool append, size_t *total)
{
    LOCKED(bool, fs_tmp_write(id, data, len, append, total));
}
char *luapp_store_tmp_read(const char *id, size_t *len) { LOCKED(char *, fs_tmp_read(id, len)); }
void luapp_store_tmp_discard(const char *id)
{
    fs_lock();
    fs_tmp_discard(id);
    fs_unlock();
}
bool luapp_store_commit(const char *id, const char *name, uint32_t color) { LOCKED(bool, fs_commit(id, name, color)); }
char *luapp_store_code(const char *id, size_t *len) { LOCKED(char *, fs_code(id, len)); }
bool luapp_store_remove(const char *id) { LOCKED(bool, fs_remove(id)); }
char *luapp_store_kv_read(const char *id, size_t *len) { LOCKED(char *, fs_kv_read(id, len)); }
bool luapp_store_kv_write(const char *id, const char *json, size_t len) { LOCKED(bool, fs_kv_write(id, json, len)); }
bool luapp_store_icon_read(const char *id, uint8_t *buf, size_t n) { LOCKED(bool, fs_icon_read(id, buf, n)); }
bool luapp_store_icon_write(const char *id, const uint8_t *buf, size_t n) { LOCKED(bool, fs_icon_write(id, buf, n)); }
void luapp_store_icon_remove(const char *id)
{
    fs_lock();
    fs_icon_remove(id);
    fs_unlock();
}
