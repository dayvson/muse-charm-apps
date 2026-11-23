/*
 * The board and the rest of the gadget, for Lua apps: device.* (battery,
 * Wi-Fi, Muse, volume), app.keep_awake / app.brightness, mic.level, motion.*
 * (the QMI8658 IMU), fs.* (the app's own files), apps.* (the built-in apps'
 * data), muse.ask (a question to Muse as a typed turn), app.alarm (wake the
 * board and open the app at a time), ui.on_button (the bottom button), and
 * JSON <-> Lua for luapp.send.
 *
 * Lua task, except the hooks marked otherwise: those set a flag and wake the
 * task, and luapp_dev_tick delivers them between callbacks.
 */

#include <math.h>
#include "esp_attr.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lauxlib.h"
#include "lua.h"
#include "luapp.h"
#include "luapp_priv.h"
#include "luapp_store.h"
#include "muse_apps_priv.h"
#include "muse_chat.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_voice.h"
#include "muse_wifi.h"
#if __has_include("bsp/esp-bsp.h")
#include "bsp/esp-bsp.h"
#define HAVE_BSP_I2C 1
#endif

static const char *TAG = "luapp_dev";

#define CLOCK_VALID 1704067200
#define POKE_US (5 * 1000000LL)
#define ASK_MAX 2000              /* question */
#define ASK_REPLY_MAX (6 * 1024)  /* answer, cut beyond */
#define ASK_TIMEOUT_US (120 * 1000000LL)
#define ASK_GAP_US (5 * 1000000LL)
#define ASK_PER_RUN 30
#define ALARMS_MAX 16
#define ALARMS_PER_APP 4
#define ALARM_TAG_MAX 23
#define APPS_CACHE_US (2 * 1000000LL)
#define JSON_DEPTH 12

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static int64_t now_us(void) { return esp_timer_get_time(); }

static int64_t wall_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

/* Per run (reset by luapp_dev_open and luapp_dev_stop). Task-only state, in
 * PSRAM like the rest of this file's tables: internal RAM is what Muse's link needs. */
EXT_RAM_BSS_ATTR static struct {
    lua_State *L;
    bool keep_awake;
    int64_t poked;
    bool mic_on;
    int button_ref, alarm_ref, message_ref;
    int ask_ref;
    int64_t ask_last;
    int asks;
} D;

/* ---- JSON <-> Lua ---- */

void luapp_push_cjson(lua_State *L, const cJSON *v, int depth)
{
    luaL_checkstack(L, 4, "json");
    if (!v || cJSON_IsNull(v) || depth > JSON_DEPTH) {
        lua_pushnil(L);
    } else if (cJSON_IsBool(v)) {
        lua_pushboolean(L, cJSON_IsTrue(v));
    } else if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        if (d == floor(d) && fabs(d) < 9e15) lua_pushinteger(L, (lua_Integer)d);
        else lua_pushnumber(L, d);
    } else if (cJSON_IsString(v)) {
        lua_pushstring(L, v->valuestring ? v->valuestring : "");
    } else if (cJSON_IsArray(v)) {
        lua_createtable(L, cJSON_GetArraySize(v), 0);
        int i = 1;
        for (const cJSON *c = v->child; c; c = c->next) {
            luapp_push_cjson(L, c, depth + 1);
            lua_rawseti(L, -2, i++);
        }
    } else if (cJSON_IsObject(v)) {
        lua_newtable(L);
        for (const cJSON *c = v->child; c; c = c->next) {
            if (!c->string) continue;
            luapp_push_cjson(L, c, depth + 1);
            lua_setfield(L, -2, c->string);
        }
    } else {
        lua_pushnil(L);
    }
}

cJSON *luapp_to_cjson(lua_State *L, int idx, int depth)
{
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN:
        return cJSON_CreateBool(lua_toboolean(L, idx));
    case LUA_TNUMBER: {
        double d = lua_tonumber(L, idx);
        return isfinite(d) ? cJSON_CreateNumber(d) : cJSON_CreateNull();
    }
    case LUA_TSTRING:
        return cJSON_CreateString(lua_tostring(L, idx));
    case LUA_TTABLE: {
        if (depth > JSON_DEPTH) return cJSON_CreateNull();
        luaL_checkstack(L, 4, "json");
        lua_Integer n = (lua_Integer)lua_rawlen(L, idx), keys = 0;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            keys++;
            lua_pop(L, 1);
        }
        if (n > 0 && keys == n) {
            cJSON *a = cJSON_CreateArray();
            for (lua_Integer i = 1; i <= n; i++) {
                lua_rawgeti(L, idx, i);
                cJSON_AddItemToArray(a, luapp_to_cjson(L, -1, depth + 1));
                lua_pop(L, 1);
            }
            return a;
        }
        cJSON *o = cJSON_CreateObject();
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            char key[32];
            const char *k = NULL;
            if (lua_type(L, -2) == LUA_TSTRING) {
                k = lua_tostring(L, -2);
            } else if (lua_type(L, -2) == LUA_TNUMBER) {
                snprintf(key, sizeof(key), "%.14g", lua_tonumber(L, -2));
                k = key;
            }
            if (k) cJSON_AddItemToObject(o, k, luapp_to_cjson(L, -1, depth + 1));
            lua_pop(L, 1);
        }
        return o;
    }
    default:
        return cJSON_CreateNull();
    }
}

/* ---- device.* ---- */

static const char *muse_state_name(void)
{
    muse_hatch_status_t st;
    muse_hatch_status(&st);
    switch (st.state) {
    case MUSE_HATCH_NOT_SET: return "not set up";
    case MUSE_HATCH_OFFLINE: return "offline";
    case MUSE_HATCH_REACHABLE: return "connected";
    case MUSE_HATCH_UNREACHABLE: return "unreachable";
    default: return "connecting";
    }
}

static int l_device_info(lua_State *L)
{
    muse_power_t p = muse_state_power();
    lua_createtable(L, 0, 12);
    if (p.battery_pct >= 0) {
        lua_pushinteger(L, p.battery_pct);
        lua_setfield(L, -2, "battery");
    }
    if (p.battery_mv > 0) {
        lua_pushinteger(L, p.battery_mv);
        lua_setfield(L, -2, "battery_mv");
    }
    lua_pushboolean(L, p.charging);
    lua_setfield(L, -2, "charging");
    lua_pushboolean(L, p.usb);
    lua_setfield(L, -2, "usb");
    lua_pushboolean(L, muse_wifi_connected());
    lua_setfield(L, -2, "wifi");
    lua_pushstring(L, muse_state_name());
    lua_setfield(L, -2, "muse");
    lua_pushboolean(L, muse_voice_in_call());
    lua_setfield(L, -2, "call");
    lua_pushinteger(L, muse_settings_volume());
    lua_setfield(L, -2, "volume");
    lua_pushboolean(L, muse_settings_speaker_on());
    lua_setfield(L, -2, "speaker");
    lua_pushinteger(L, muse_settings_brightness());
    lua_setfield(L, -2, "brightness");
    lua_pushinteger(L, (lua_Integer)(now_us() / 1000000));
    lua_setfield(L, -2, "uptime");
    lua_pushinteger(L, (lua_Integer)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    lua_setfield(L, -2, "psram_kb");
    return 1;
}

static int l_device_battery(lua_State *L)
{
    muse_power_t p = muse_state_power();
    if (p.battery_pct >= 0) lua_pushinteger(L, p.battery_pct);
    else lua_pushnil(L);
    lua_pushboolean(L, p.charging);
    return 2;
}

/* ---- app.keep_awake, app.brightness ---- */

static atomic_int s_bright = -1;   /* the app's brightness, while it's on screen; -1 none */

/* UI task (muse_ui.c's brightness poll, patch 0002). */
int muse_apps_brightness(int target)
{
    int b = atomic_load(&s_bright);
    return b >= 0 && muse_apps_on_screen(MUSE_APP_LUAPPS) ? b : target;
}

static int l_keep_awake(lua_State *L)
{
    D.keep_awake = lua_isnoneornil(L, 1) || lua_toboolean(L, 1);
    D.poked = 0;
    return 0;
}

static int l_brightness(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        atomic_store(&s_bright, -1);
    } else {
        int b = (int)luaL_checkinteger(L, 1);
        atomic_store(&s_bright, b < 10 ? 10 : b > 100 ? 100 : b);
    }
    return 0;
}

/* ---- mic.level ---- */

static int l_mic_level(lua_State *L)
{
    if (!D.mic_on) {
        muse_voice_set_monitor(true);
        D.mic_on = true;
    }
    lua_pushnumber(L, roundf(muse_voice_monitor_db() * 10) / 10);
    return 1;
}

/* ---- motion.* (QMI8658 on the shared I2C bus) ---- */

#define QMI_WHO_AM_I 0x00
#define QMI_CTRL1 0x02
#define QMI_CTRL2 0x03
#define QMI_CTRL3 0x04
#define QMI_CTRL7 0x08
#define QMI_DATA 0x35
static i2c_master_dev_handle_t s_imu;
static int8_t s_imu_state;   /* 0 untried, 1 on, -1 none */

static esp_err_t imu_wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    return i2c_master_transmit(s_imu, b, 2, 50);
}

static bool imu_start(void)
{
    if (s_imu_state) return s_imu_state > 0;
    s_imu_state = -1;
#if HAVE_BSP_I2C
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return false;
    static const uint8_t ADDRS[] = {0x6b, 0x6a};
    for (int i = 0; i < 2 && s_imu_state < 0; i++) {
        if (i2c_master_probe(bus, ADDRS[i], 50) != ESP_OK) continue;
        i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDRS[i], .scl_speed_hz = 400000};
        if (i2c_master_bus_add_device(bus, &cfg, &s_imu) != ESP_OK) continue;
        uint8_t reg = QMI_WHO_AM_I, id = 0;
        if (i2c_master_transmit_receive(s_imu, &reg, 1, &id, 1, 50) == ESP_OK && id == 0x05 &&
            imu_wr(QMI_CTRL1, 0x40) == ESP_OK &&    /* register auto-increment, little-endian */
            imu_wr(QMI_CTRL2, 0x15) == ESP_OK &&    /* accelerometer +-4 g, ~235 Hz */
            imu_wr(QMI_CTRL3, 0x55) == ESP_OK &&    /* gyroscope +-512 dps, ~235 Hz */
            imu_wr(QMI_CTRL7, 0x03) == ESP_OK) {    /* both on */
            s_imu_state = 1;
            ESP_LOGI(TAG, "QMI8658 at 0x%02x on", ADDRS[i]);
        } else {
            i2c_master_bus_rm_device(s_imu);
            s_imu = NULL;
        }
    }
#endif
    if (s_imu_state < 0) ESP_LOGW(TAG, "no motion sensor found");
    return s_imu_state > 0;
}

static void imu_stop(void)
{
    if (s_imu_state > 0 && s_imu) {
        imu_wr(QMI_CTRL7, 0x00);
        i2c_master_bus_rm_device(s_imu);
        s_imu = NULL;
        s_imu_state = 0;
    }
}

static int l_motion_read(lua_State *L)
{
    if (!imu_start()) {
        lua_pushnil(L);
        lua_pushstring(L, "no motion sensor");
        return 2;
    }
    uint8_t reg = QMI_DATA, b[12];
    if (i2c_master_transmit_receive(s_imu, &reg, 1, b, sizeof(b), 50) != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "motion sensor didn't answer");
        return 2;
    }
    for (int i = 0; i < 6; i++) {
        int16_t raw = (int16_t)(b[2 * i] | b[2 * i + 1] << 8);
        double v = i < 3 ? raw / 8192.0 : raw / 64.0;   /* g, degrees per second */
        lua_pushnumber(L, round(v * 1000) / 1000);
    }
    return 6;
}

static int l_motion_available(lua_State *L)
{
    lua_pushboolean(L, imu_start());
    return 1;
}

/* ---- fs.* ---- */

static const char *file_arg(lua_State *L, int i)
{
    const char *name = luaL_checkstring(L, i);
    if (!luapp_file_name_valid(name)) luaL_error(L, "fs: name must be 1 to 24 of a-z 0-9 _ - . (not starting with .)");
    return name;
}

static int fs_write(lua_State *L, bool append)
{
    const char *name = file_arg(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    int64_t t0 = now_us();
    const char *err = luapp_store_file_write(luapp_vm_app_id(), name, data, len, append);
    luapp_vm_forgive(now_us() - t0);   /* flash writes don't count against the app */
    if (err) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_fs_write(lua_State *L) { return fs_write(L, false); }
static int l_fs_append(lua_State *L) { return fs_write(L, true); }

static int l_fs_read(lua_State *L)
{
    const char *name = file_arg(L, 1);
    size_t len = 0;
    int64_t t0 = now_us();
    char *data = luapp_store_file_read(luapp_vm_app_id(), name, &len);
    luapp_vm_forgive(now_us() - t0);
    if (!data) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, data, len);
    free(data);
    return 1;
}

static int l_fs_remove(lua_State *L)
{
    lua_pushboolean(L, luapp_store_file_remove(luapp_vm_app_id(), file_arg(L, 1)));
    return 1;
}

static int l_fs_list(lua_State *L)
{
    luapp_file_t *files = heap_caps_malloc(sizeof(luapp_file_t) * LUAPP_FILES_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!files) return luaL_error(L, "fs.list: out of memory");
    size_t total = 0;
    int n = luapp_store_file_list(luapp_vm_app_id(), files, LUAPP_FILES_MAX, &total);
    if (n > LUAPP_FILES_MAX) n = LUAPP_FILES_MAX;
    lua_createtable(L, 0, n);
    for (int i = 0; i < n; i++) {
        lua_pushinteger(L, files[i].size);
        lua_setfield(L, -2, files[i].name);
    }
    free(files);
    lua_pushinteger(L, (lua_Integer)total);
    lua_pushinteger(L, LUAPP_FILES_QUOTA);
    return 3;
}

/* ---- apps.* (the built-in apps' data, as their Link commands give it) ---- */

EXT_RAM_BSS_ATTR static struct {
    int64_t at;
    char *json;
} s_apps_cache[3];

static int apps_get(lua_State *L, int which)
{
    static const char *const CMD[] = {"stocks.get", "wind.get", "watch.get"};
    int64_t now = now_us();
    if (!s_apps_cache[which].json || now - s_apps_cache[which].at > APPS_CACHE_US) {
        cJSON *r = which == 0 ? stocks_command(CMD[0], NULL) : which == 1 ? wind_command(CMD[1], NULL)
                                                                         : watch_command(CMD[2], NULL);
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(r, "payload");
        char *s = payload ? cJSON_PrintUnformatted(payload) : NULL;
        cJSON_Delete(r);
        if (!s) {
            lua_pushnil(L);
            return 1;
        }
        free(s_apps_cache[which].json);
        s_apps_cache[which].json = s;
        s_apps_cache[which].at = now;
    }
    cJSON *v = cJSON_Parse(s_apps_cache[which].json);
    luapp_push_cjson(L, v, 0);
    cJSON_Delete(v);
    return 1;
}

static int l_apps_stocks(lua_State *L) { return apps_get(L, 0); }
static int l_apps_wind(lua_State *L) { return apps_get(L, 1); }
static int l_apps_watch(lua_State *L) { return apps_get(L, 2); }

/* ---- muse.ask: a typed turn; the reply comes through the chat console hook ---- */

enum { ASK_IDLE, ASK_WAITING, ASK_DONE, ASK_FAILED };
EXT_RAM_BSS_ATTR static struct {
    int state;           /* under s_mux */
    uint32_t gen;
    char *buf;           /* PSRAM, ASK_REPLY_MAX + 1 */
    size_t len, msg_start;
    int64_t started;
    char err[48];
} s_ask;

static void ask_append(const char *t)
{
    size_t n = strlen(t);
    if (s_ask.len + n > ASK_REPLY_MAX) n = ASK_REPLY_MAX - s_ask.len;
    memcpy(s_ask.buf + s_ask.len, t, n);
    s_ask.len += n;
    s_ask.buf[s_ask.len] = 0;
}

/* Muse's chat task (muse_hatch_console, patch 0002): every "@chat" line. */
void muse_apps_chat_line(const char *type, const char *text)
{
    bool wake = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_ask.state == ASK_WAITING && s_ask.buf) {
        if (!strcmp(type, "text") && text) {
            ask_append(text);
        } else if (!strcmp(type, "final") && text) {   /* the whole message: replaces its pieces */
            s_ask.len = s_ask.msg_start;
            s_ask.buf[s_ask.len] = 0;
            ask_append(text);
        } else if (!strcmp(type, "message_done")) {
            if (s_ask.len > s_ask.msg_start) ask_append("\n\n");
            s_ask.msg_start = s_ask.len;
        } else if (!strcmp(type, "done")) {
            s_ask.state = ASK_DONE;
            wake = true;
        } else if (!strcmp(type, "error")) {
            s_ask.state = ASK_FAILED;
            strlcpy(s_ask.err, text ? text : "failed", sizeof(s_ask.err));
            wake = true;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (wake) luapp_vm_wake();
}

static int l_muse_ask(lua_State *L)
{
    size_t n;
    const char *q = luaL_checklstring(L, 1, &n);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (n == 0 || n > ASK_MAX) return luaL_error(L, "muse.ask: 1 to %d characters", ASK_MAX);
    const char *why = NULL;
    int64_t now = now_us();
    muse_hatch_status_t st;
    muse_hatch_status(&st);
    taskENTER_CRITICAL(&s_mux);
    int state = s_ask.state;
    taskEXIT_CRITICAL(&s_mux);
    if (state == ASK_WAITING || D.ask_ref != LUA_NOREF) why = "one question at a time";
    else if (D.asks >= ASK_PER_RUN) why = "too many questions this run";
    else if (D.ask_last && now - D.ask_last < ASK_GAP_US) why = "wait 5 s between questions";
    else if (st.state == MUSE_HATCH_NOT_SET) why = "Muse isn't set up";
    else if (!muse_wifi_connected()) why = "offline";
    else if (muse_voice_in_call()) why = "in a call";
    if (why) {   /* answered at once, through the callback */
        lua_pushvalue(L, 2);
        lua_pushnil(L);
        lua_pushstring(L, why);
        lua_call(L, 2, 0);
        return 0;
    }
    if (!s_ask.buf) s_ask.buf = heap_caps_malloc(ASK_REPLY_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char name[LUAPP_NAME_MAX + 1];
    luapp_vm_app_name(name, sizeof(name));
    size_t cap = n + 128;
    char *text = malloc(cap);   /* muse_hatch_text_turn frees it */
    if (!s_ask.buf || !text) {
        free(text);
        return luaL_error(L, "muse.ask: out of memory");
    }
    snprintf(text, cap, "[From the %s app on the gadget; your reply is shown on its small round screen] %s", name, q);
    lua_pushvalue(L, 2);
    D.ask_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    D.ask_last = now;
    D.asks++;
    taskENTER_CRITICAL(&s_mux);
    s_ask.state = ASK_WAITING;
    s_ask.gen = luapp_vm_gen();
    s_ask.len = s_ask.msg_start = 0;
    s_ask.buf[0] = 0;
    s_ask.err[0] = 0;
    s_ask.started = now;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "%s asks Muse (%u chars)", name, (unsigned)n);
    muse_hatch_text_turn(text);
    return 0;
}

static int l_muse_status(lua_State *L)
{
    lua_pushstring(L, muse_state_name());
    return 1;
}

/* ---- app.alarm (kept in _alarms; checked by the Lua task each second) ---- */

typedef struct {
    char id[LUAPP_ID_MAX + 1];
    char tag[ALARM_TAG_MAX + 1];
    int64_t at;   /* Unix time */
} alarm_t;

EXT_RAM_BSS_ATTR static alarm_t s_alarms[ALARMS_MAX];
static int s_nalarms;
static bool s_alarms_loaded;

static void alarms_save(void)
{
    if (!luapp_store_sys_write("_alarms", (const char *)s_alarms, sizeof(alarm_t) * (size_t)s_nalarms)) {
        ESP_LOGW(TAG, "couldn't save the alarms");
    }
}

static void alarms_load(void)
{
    if (s_alarms_loaded) return;
    s_alarms_loaded = true;
    size_t len = 0;
    char *d = luapp_store_sys_read("_alarms", sizeof(s_alarms), &len);
    if (d && len % sizeof(alarm_t) == 0) {
        memcpy(s_alarms, d, len);
        s_nalarms = (int)(len / sizeof(alarm_t));
        for (int i = 0; i < s_nalarms; i++) {
            s_alarms[i].id[LUAPP_ID_MAX] = 0;
            s_alarms[i].tag[ALARM_TAG_MAX] = 0;
        }
        if (s_nalarms) ESP_LOGI(TAG, "%d alarm(s) set", s_nalarms);
    }
    free(d);
}

static void alarm_drop(int i)
{
    memmove(&s_alarms[i], &s_alarms[i + 1], sizeof(alarm_t) * (size_t)(s_nalarms - i - 1));
    s_nalarms--;
}

void luapp_dev_alarms_forget(const char *id)
{
    alarms_load();
    bool changed = false;
    for (int i = s_nalarms - 1; i >= 0; i--) {
        if (!strcmp(s_alarms[i].id, id)) {
            alarm_drop(i);
            changed = true;
        }
    }
    if (changed) alarms_save();
}

static int l_alarm(lua_State *L)
{
    double when = luaL_checknumber(L, 1);
    const char *tag = luaL_optstring(L, 2, "alarm");
    if (strlen(tag) > ALARM_TAG_MAX) return luaL_error(L, "app.alarm: tag up to %d characters", ALARM_TAG_MAX);
    int64_t now = wall_s();
    if (now < CLOCK_VALID) return luaL_error(L, "app.alarm: the clock isn't set yet (time.synced())");
    int64_t at = when < 1e9 ? now + (int64_t)ceil(when) : (int64_t)when;
    if (at <= now) at = now + 1;
    if (at > now + 30LL * 86400) return luaL_error(L, "app.alarm: up to 30 days ahead");
    alarms_load();
    const char *id = luapp_vm_app_id();
    int mine = 0, slot = -1;
    for (int i = 0; i < s_nalarms; i++) {
        if (strcmp(s_alarms[i].id, id)) continue;
        mine++;
        if (!strcmp(s_alarms[i].tag, tag)) slot = i;
    }
    if (slot < 0) {
        if (mine >= ALARMS_PER_APP) return luaL_error(L, "app.alarm: %d per app", ALARMS_PER_APP);
        if (s_nalarms >= ALARMS_MAX) return luaL_error(L, "app.alarm: the board's %d alarms are taken", ALARMS_MAX);
        slot = s_nalarms++;
        strlcpy(s_alarms[slot].id, id, sizeof(s_alarms[slot].id));
        strlcpy(s_alarms[slot].tag, tag, sizeof(s_alarms[slot].tag));
    }
    s_alarms[slot].at = at;
    alarms_save();
    lua_pushinteger(L, (lua_Integer)at);
    return 1;
}

static int l_alarm_cancel(lua_State *L)
{
    const char *tag = luaL_optstring(L, 1, NULL);
    const char *id = luapp_vm_app_id();
    alarms_load();
    bool changed = false;
    for (int i = s_nalarms - 1; i >= 0; i--) {
        if (!strcmp(s_alarms[i].id, id) && (!tag || !strcmp(s_alarms[i].tag, tag))) {
            alarm_drop(i);
            changed = true;
        }
    }
    if (changed) alarms_save();
    return 0;
}

static int l_alarms(lua_State *L)
{
    alarms_load();
    const char *id = luapp_vm_app_id();
    lua_newtable(L);
    for (int i = 0; i < s_nalarms; i++) {
        if (strcmp(s_alarms[i].id, id)) continue;
        lua_pushinteger(L, (lua_Integer)s_alarms[i].at);
        lua_setfield(L, -2, s_alarms[i].tag);
    }
    return 1;
}

static int set_handler(lua_State *L, int *ref)
{
    if (*ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, *ref);
    *ref = LUA_NOREF;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
        *ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

static int l_on_alarm(lua_State *L) { return set_handler(L, &D.alarm_ref); }
static int l_on_message(lua_State *L) { return set_handler(L, &D.message_ref); }

/* ---- ui.on_button: the bottom button's short press, while the app is on screen ---- */

static atomic_bool s_button_wanted, s_button_pressed;

static int l_on_button(lua_State *L)
{
    set_handler(L, &D.button_ref);
    atomic_store(&s_button_wanted, D.button_ref != LUA_NOREF);
    return 0;
}

/* Input task (muse_input.c's aux button, patch 0002): a short press, the
 * screen awake. True if a Lua app on screen takes it instead of sleeping. */
bool muse_apps_aux_press(void)
{
    if (!atomic_load(&s_button_wanted) || !muse_apps_on_screen(MUSE_APP_LUAPPS)) return false;
    atomic_store(&s_button_pressed, true);
    muse_state_poke();
    luapp_vm_wake();
    return true;
}

/* ---- the Lua side ---- */

static void add_funcs(lua_State *L, const luaL_Reg *r)
{
    for (; r->name; r++) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
}

void luapp_dev_open(lua_State *L)
{
    memset(&D, 0, sizeof(D));
    D.L = L;
    D.button_ref = D.alarm_ref = D.message_ref = D.ask_ref = LUA_NOREF;
    atomic_store(&s_button_wanted, false);
    atomic_store(&s_button_pressed, false);
    atomic_store(&s_bright, -1);

    static const luaL_Reg dev[] = {{"info", l_device_info}, {"battery", l_device_battery}, {NULL, NULL}};
    luaL_newlib(L, dev);
    lua_setglobal(L, "device");
    static const luaL_Reg mic[] = {{"level", l_mic_level}, {NULL, NULL}};
    luaL_newlib(L, mic);
    lua_setglobal(L, "mic");
    static const luaL_Reg mo[] = {{"read", l_motion_read}, {"available", l_motion_available}, {NULL, NULL}};
    luaL_newlib(L, mo);
    lua_setglobal(L, "motion");
    static const luaL_Reg fs[] = {{"write", l_fs_write}, {"append", l_fs_append}, {"read", l_fs_read},
                                  {"remove", l_fs_remove}, {"list", l_fs_list}, {NULL, NULL}};
    luaL_newlib(L, fs);
    lua_setglobal(L, "fs");
    static const luaL_Reg apps[] = {{"stocks", l_apps_stocks}, {"wind", l_apps_wind}, {"watch", l_apps_watch},
                                    {NULL, NULL}};
    luaL_newlib(L, apps);
    lua_setglobal(L, "apps");
    static const luaL_Reg muse[] = {{"ask", l_muse_ask}, {"status", l_muse_status}, {NULL, NULL}};
    luaL_newlib(L, muse);
    lua_setglobal(L, "muse");
}

void luapp_dev_app_fields(lua_State *L)
{
    static const luaL_Reg app[] = {{"keep_awake", l_keep_awake}, {"brightness", l_brightness}, {"alarm", l_alarm},
                                   {"cancel_alarm", l_alarm_cancel}, {"alarms", l_alarms}, {"on_alarm", l_on_alarm},
                                   {"on_message", l_on_message}, {NULL, NULL}};
    add_funcs(L, app);
}

void luapp_dev_ui_fields(lua_State *L)
{
    static const luaL_Reg ui[] = {{"on_button", l_on_button}, {NULL, NULL}};
    add_funcs(L, ui);
}

void luapp_dev_stop(void)
{
    atomic_store(&s_button_wanted, false);
    atomic_store(&s_button_pressed, false);
    atomic_store(&s_bright, -1);
    if (D.mic_on) muse_voice_set_monitor(false);
    imu_stop();
    taskENTER_CRITICAL(&s_mux);
    bool asking = s_ask.state == ASK_WAITING;
    s_ask.state = ASK_IDLE;
    taskEXIT_CRITICAL(&s_mux);
    if (asking) muse_hatch_text_cancel();   /* nobody's left to show the answer */
    for (int i = 0; i < 3; i++) {
        free(s_apps_cache[i].json);
        s_apps_cache[i].json = NULL;
    }
    memset(&D, 0, sizeof(D));
    D.button_ref = D.alarm_ref = D.message_ref = D.ask_ref = LUA_NOREF;
}

bool luapp_dev_has_alarm_handler(void) { return D.L && D.alarm_ref != LUA_NOREF; }

void luapp_dev_call_alarm(const char *tag)
{
    if (!D.L || D.alarm_ref == LUA_NOREF) return;
    lua_rawgeti(D.L, LUA_REGISTRYINDEX, D.alarm_ref);
    lua_pushstring(D.L, tag);
    luapp_vm_call(1);
}

cJSON *luapp_dev_message(const cJSON *data, const char **err)
{
    *err = NULL;
    if (!D.L) {
        *err = "no app is running";
        return NULL;
    }
    if (D.message_ref == LUA_NOREF) {
        *err = "the app has no app.on_message handler";
        return NULL;
    }
    lua_State *L = D.L;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, D.message_ref);
    luapp_push_cjson(L, data, 0);
    if (!luapp_vm_call_results(1, 1)) {   /* the app stopped: L is gone */
        *err = "the handler failed and the app stopped (see luapp.admin logs)";
        return NULL;
    }
    cJSON *r = luapp_to_cjson(L, -1, 0);
    lua_settop(L, top);
    luapp_vm_after_call();   /* may stop it (app.exit) */
    return r;
}

/* Between callbacks, on the Lua task: what the hooks flagged, keep-awake, and
 * the alarms. Returns when it next needs to run (esp_timer time), or INT64_MAX. */
int64_t luapp_dev_tick(void)
{
    int64_t now = now_us(), next = INT64_MAX;
    alarms_load();
    /* Alarms: the earliest due (and none during a call), one per tick. */
    int64_t wall = wall_s();
    if (wall >= CLOCK_VALID && s_nalarms && !muse_voice_in_call()) {
        int due = -1;
        for (int i = 0; i < s_nalarms; i++) {
            if (s_alarms[i].at <= wall && (due < 0 || s_alarms[i].at < s_alarms[due].at)) due = i;
        }
        if (due >= 0) {
            alarm_t a = s_alarms[due];
            alarm_drop(due);
            alarms_save();
            ESP_LOGI(TAG, "alarm %s for %s", a.tag, a.id);
            luapp_vm_fire_alarm(a.id, a.tag);
            now = now_us();
        }
    }
    if (!D.L) return next;
    /* muse.ask's answer. */
    taskENTER_CRITICAL(&s_mux);
    int state = s_ask.state;
    bool timeout = state == ASK_WAITING && now - s_ask.started > ASK_TIMEOUT_US;
    bool mine = s_ask.gen == luapp_vm_gen();
    if (state == ASK_DONE || state == ASK_FAILED || timeout) s_ask.state = ASK_IDLE;
    taskEXIT_CRITICAL(&s_mux);
    if (timeout) muse_hatch_text_cancel();
    if ((state == ASK_DONE || state == ASK_FAILED || timeout) && mine && D.ask_ref != LUA_NOREF) {
        int ref = D.ask_ref;
        D.ask_ref = LUA_NOREF;
        lua_rawgeti(D.L, LUA_REGISTRYINDEX, ref);
        luaL_unref(D.L, LUA_REGISTRYINDEX, ref);
        if (state == ASK_DONE) {
            size_t n = s_ask.len;
            while (n && (s_ask.buf[n - 1] == '\n' || s_ask.buf[n - 1] == ' ')) n--;
            lua_pushlstring(D.L, s_ask.buf, n);
            lua_pushnil(D.L);
        } else {
            lua_pushnil(D.L);
            lua_pushstring(D.L, timeout ? "no answer in 2 minutes" : s_ask.err);
        }
        if (!luapp_vm_call(2) || !D.L) return INT64_MAX;
        now = now_us();
    } else if (state == ASK_WAITING) {
        int64_t t = s_ask.started + ASK_TIMEOUT_US;
        if (t < next) next = t;
    }
    /* The bottom button. */
    if (atomic_exchange(&s_button_pressed, false) && D.button_ref != LUA_NOREF) {
        lua_rawgeti(D.L, LUA_REGISTRYINDEX, D.button_ref);
        if (!luapp_vm_call(0) || !D.L) return INT64_MAX;
        now = now_us();
    }
    /* Keep the screen on while the app is on it (asleep stays asleep). */
    if (D.keep_awake && muse_apps_on_screen(MUSE_APP_LUAPPS) && !muse_state_asleep()) {
        if (now - D.poked >= POKE_US) {
            muse_state_poke();
            D.poked = now;
        }
        if (D.poked + POKE_US < next) next = D.poked + POKE_US;
    }
    return next;
}
