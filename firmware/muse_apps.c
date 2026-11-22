/*
 * Apps beside Muse: the tiles either side of the avatar (muse_apps_ui.h), the
 * commands Muse drives them with, and the wall clock they need (SNTP, which
 * the SDK doesn't start; Muse's own code only ever needed uptime).
 */

#include <stdatomic.h>
#include <stdlib.h>
#include <sys/time.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "muse_apps.h"
#include "muse_apps_priv.h"
#include "muse_apps_ui.h"
#include "luapp.h"
#include "muse_tz.h"
#include "esp_timer.h"
#include "muse_state.h"
#include "muse_voice.h"
#if CONFIG_MUSE_HATCH
#include "muse_chat.h"
#endif
#include "muse_wifi.h"

static const char *TAG = "muse_apps";

/* The app tiles (MUSE_APP_*), then Muse's own. */
#define APP_MUSE MUSE_APP_TILES
#define APP_COUNT (MUSE_APP_TILES + 1)
static const char *const APP_NAMES[APP_COUNT] = {[MUSE_APP_LUAPPS] = "apps", [MUSE_APP_WIND] = "wind",
                                                 [MUSE_APP_STOCKS] = "stocks", [MUSE_APP_WATCH] = "watch",
                                                 [APP_MUSE] = "muse"};
static atomic_int s_active = APP_MUSE;   /* the tile on screen, for other tasks */

_Static_assert(MUSE_APP_TILES == MUSE_APPS_COUNT, "muse_apps_ui.h and muse_apps_priv.h disagree");

static lv_obj_t *s_tv, *s_tiles[APP_COUNT];
static atomic_int s_show = -1;   /* app.show's request, for the UI task */
static bool s_sntp;
static atomic_bool s_ntp_synced;

static void on_time_sync(struct timeval *tv)
{
    atomic_store(&s_ntp_synced, true);
    ESP_LOGI(TAG, "SNTP: clock synced (%lld)", (long long)tv->tv_sec);
}

void muse_apps_clock_hint(const char *date, size_t len)
{
    if (!date || atomic_load(&s_ntp_synced)) return;   /* SNTP is better than a whole second */
    int64_t at = muse_http_date(date, len);
    if (at < 0) return;
    struct timeval now;
    gettimeofday(&now, NULL);
    /* The header drops the fraction: aim for the middle of its second. Leave
     * a clock within 2 s alone, so replies don't make the hands twitch. */
    int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
    int64_t at_ms = at * 1000 + 500;
    if (llabs(at_ms - now_ms) < 2000) return;
    struct timeval tv = {.tv_sec = (time_t)(at_ms / 1000), .tv_usec = (suseconds_t)(at_ms % 1000) * 1000};
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "clock set from Muse's reply (%.29s)%s", date, now.tv_sec < 1704067200 ? "" : ", corrected");
}

static void start_sntp(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.wait_for_sync = false;   /* nobody waits: the watch polls the clock */
    cfg.sync_cb = on_time_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    s_sntp = true;
    ESP_LOGI(TAG, "SNTP: %s", err == ESP_OK ? "started (pool.ntp.org)" : esp_err_to_name(err));
}

static void apps_tick(lv_timer_t *timer)
{
    (void)timer;
    int show = atomic_exchange(&s_show, -1);
    if (show >= 0 && show < APP_COUNT && s_tv) {
        if (muse_state_asleep()) muse_state_set_asleep(false);
        muse_state_poke();
        lv_tileview_set_tile(s_tv, s_tiles[show], LV_ANIM_ON);
    }
    if (!s_sntp && muse_wifi_connected()) start_sntp();
    if (s_tv) {
        lv_obj_t *active = lv_tileview_get_tile_active(s_tv);
        int a = -1;
        for (int i = 0; i < APP_COUNT; i++) {
            if (s_tiles[i] == active) a = i;
        }
        atomic_store(&s_active, a);
    }
}

void muse_apps_build(lv_obj_t *tileview, lv_obj_t *const tiles[MUSE_APPS_COUNT], lv_obj_t *face_tile, int w, int h)
{
    s_tv = tileview;
    for (int i = 0; i < MUSE_APP_TILES; i++) s_tiles[i] = tiles[i];
    s_tiles[APP_MUSE] = face_tile;
    watch_build(tiles[MUSE_APP_WATCH], w, h);
    stocks_build(tiles[MUSE_APP_STOCKS], w, h);
    wind_build(tiles[MUSE_APP_WIND], w, h);
    luapp_ui_build(tiles[MUSE_APP_LUAPPS], w, h);
    lv_timer_create(apps_tick, 250, NULL);
}

bool muse_apps_on_screen(int app)
{
    return atomic_load(&s_active) == app;
}

int muse_apps_target(void)
{
    int show = atomic_load(&s_show);
    return show >= 0 && show < APP_COUNT ? show : atomic_load(&s_active);
}

/* Left to right on screen: the tiles left of Muse, Muse, the rest, settings. */
static int screen_pos(int app)
{
    if (app < 0 || app >= APP_COUNT) return MUSE_APPS_COUNT + 1;   /* settings */
    if (app == APP_MUSE) return MUSE_APPS_LEFT;
    return app < MUSE_APPS_LEFT ? app : app + 1;
}

bool muse_apps_near(int app)
{
    return abs(screen_pos(muse_apps_target()) - screen_pos(app)) <= 1;
}

void muse_apps_trim(void)
{
    stocks_trim();
    wind_trim();
}

void muse_apps_set_reply(muse_apps_reply_fn fn)
{
    luapp_set_reply(fn);
}

bool muse_apps_visible(int app)
{
    return s_tv && app >= 0 && app < APP_COUNT && lv_tileview_get_tile_active(s_tv) == s_tiles[app];
}

void muse_apps_request_show(int app)
{
    atomic_store(&s_show, app);
}

#define MUSE_SETTLE_US (3 * 1000000LL)   /* Muse's connection steady this long before a fetch */

/* The apps don't need Muse: they fetch whether it's connected, offline or not
 * set up. They only stay out of the way while its connection is being made
 * (TLS to the VM, the session's first requests), and for a moment after,
 * when internal RAM is tightest. */
bool muse_apps_net_ok(void)
{
    if (!muse_wifi_connected() || muse_state_asleep() || muse_voice_in_call()) return false;
#if CONFIG_MUSE_HATCH
    static int64_t steady_since;
    static int last_state = -1;
    int64_t now = esp_timer_get_time();
    muse_hatch_status_t hs;
    muse_hatch_status(&hs);
    if ((int)hs.state != last_state || hs.state == MUSE_HATCH_TESTING) {
        last_state = (int)hs.state;
        steady_since = now;
        return false;
    }
    return now - steady_since >= MUSE_SETTLE_US;
#else
    return true;
#endif
}

/* ---- commands ---- */

static cJSON *typed(cJSON *group, const char *name, const char *type, const char *description)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", description);
    cJSON_AddItemToObject(group, name, p);
    return p;
}

static cJSON *param(cJSON *group, const char *name, const char *description)
{
    return typed(group, name, "string", description);
}

static void add(cJSON *commands, const char *name, const char *description, cJSON *required, cJSON *optional)
{
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "description", description);
    cJSON_AddItemToObject(c, "required", required ? required : cJSON_CreateObject());
    cJSON_AddItemToObject(c, "optional", optional ? optional : cJSON_CreateObject());
    cJSON_AddItemToObject(commands, name, c);
}

/* link.register is capped at 8 KB with the SDK's own commands (~2.8 KB): keep
 * these short; the device guide and luapp.admin api carry the detail. */
void muse_apps_register_commands(cJSON *commands)
{
    cJSON *req = cJSON_CreateObject(), *opt = cJSON_CreateObject();
    param(req, "slot", "local (where the user is) or work (the zone they follow).");
    param(opt, "zone", "City code, city or IANA name: TLV, Tel Aviv, Asia/Jerusalem.");
    param(opt, "tz", "POSIX TZ rule for a place not built in; needs label.");
    param(opt, "label", "Up to 4 letters shown on the dial.");
    add(commands, "watch.set_zone", "Set a zone on the dual-time watch. Returns both zones' times.", req, opt);
    add(commands, "watch.swap", "Swap the watch's local and work zones.", NULL, NULL);
    add(commands, "watch.get", "The watch's zones, their times, and whether the clock is synced.", NULL, NULL);

    req = cJSON_CreateObject();
    param(req, "app", "apps, wind, stocks, watch or muse.");
    add(commands, "app.show", "Bring an app to the screen, waking it.", req, NULL);

    opt = cJSON_CreateObject();
    param(opt, "symbol", "META, SHOP or SPY.");
    param(opt, "range", "1D, 1W, 1M, 1Y or 5Y.");
    add(commands, "stocks.show", "Show the stock tracker on a symbol and range.", NULL, opt);
    add(commands, "stocks.get", "Latest quotes for META, SHOP and SPY.", NULL, NULL);

    req = cJSON_CreateObject();
    param(req, "name", "Shown on screen; up to 15 ASCII characters.");
    typed(req, "lat", "number", "Latitude (look the place up).");
    typed(req, "lon", "number", "Longitude.");
    add(commands, "wind.set_spot", "Show the kite forecast for another place: the wind app's third spot.", req, NULL);
    opt = cJSON_CreateObject();
    param(opt, "spot", "Crandon, Stiltsville or the set spot's name.");
    param(opt, "view", "today, tomorrow or week.");
    add(commands, "wind.show", "Show the wind app on a spot and view.", NULL, opt);
    add(commands, "wind.get", "Each spot's wind now and best session windows.", NULL, NULL);

    req = cJSON_CreateObject();
    opt = cJSON_CreateObject();
    param(req, "id", "a-z, 0-9, _ (up to 12).");
    param(req, "code", "Lua source, or a piece of it.");
    param(opt, "name", "Launcher name, up to 16 ASCII characters.");
    param(opt, "color", "Launcher colour, 0xRRGGBB.");
    typed(opt, "append", "boolean", "Add to the pieces sent so far.");
    typed(opt, "done", "boolean", "Last piece (default true).");
    typed(opt, "run", "boolean", "Run it after installing (default true).");
    add(commands, "luapp.install",
        "Install or update a Lua app for this screen; read luapp.admin action api first. Over 8000 characters: "
        "send pieces with done=false, then append=true. Compiles, saves and runs it; errors come back with lines.",
        req, opt);
    req = cJSON_CreateObject();
    param(req, "id", "The app's id.");
    add(commands, "luapp.run", "Open an installed Lua app; returns its start-up error, if any.", req, NULL);
    req = cJSON_CreateObject();
    opt = cJSON_CreateObject();
    param(req, "action", "api, api_gfx, api_sound (the Lua API), list, logs, stop, remove or source.");
    param(opt, "id", "For remove and source.");
    typed(opt, "offset", "integer", "For source: where to read from.");
    typed(opt, "force", "boolean", "For stop: reboot if it won't stop in 3 s.");
    add(commands, "luapp.admin", "Manage Lua apps.", req, opt);
}

bool muse_apps_handles(const char *command)
{
    return !strncmp(command, "watch.", 6) || !strncmp(command, "stocks.", 7) || !strncmp(command, "wind.", 5) ||
           !strncmp(command, "luapp.", 6) || !strcmp(command, "app.show");
}

cJSON *muse_apps_command(const char *command, const cJSON *params, const char *request_id, uint64_t generation)
{
    if (!strncmp(command, "luapp.", 6)) return luapp_command(command, params, request_id, generation);
    if (!strcmp(command, "app.show")) {
        const cJSON *app = params ? cJSON_GetObjectItemCaseSensitive(params, "app") : NULL;
        if (!s_tv) return apps_error("unavailable", "the screen isn't up");
        for (int i = 0; cJSON_IsString(app) && app->valuestring && i < APP_COUNT; i++) {
            if (!strcasecmp(app->valuestring, APP_NAMES[i])) {
                atomic_store(&s_show, i);
                cJSON *p = cJSON_CreateObject();
                cJSON_AddStringToObject(p, "shown", APP_NAMES[i]);
                return apps_ok(p);
            }
        }
        return apps_error("invalid_params", "app must be apps, wind, stocks, watch or muse");
    }
    if (!strncmp(command, "wind.", 5)) return wind_command(command, params);
    if (!strncmp(command, "stocks.", 7)) return stocks_command(command, params);
    return watch_command(command, params);
}
