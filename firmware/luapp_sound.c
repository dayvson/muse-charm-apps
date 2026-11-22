/*
 * sound.* for Lua apps, on luapp_synth.c. Muse's voice task pulls the mix
 * while it's idle (muse_apps_sound_pull), so Muse's own audio always comes
 * first; app sound stops when the app does, when it's off screen, and when
 * the screen sleeps. Sound held up by a call is dropped, not played late.
 */

#include <stdatomic.h>
#include <string.h>

#include "esp_timer.h"

#include "luapp_priv.h"
#include "luapp_synth.h"
#include "muse_apps.h"
#include "muse_apps_priv.h"
#include "muse_state.h"

static atomic_bool s_enabled;
static int64_t s_last_pull;

void luapp_sound_enable(bool on)
{
    static bool inited;
    if (!inited) {
        synth_init();
        inited = true;
    }
    atomic_store(&s_enabled, on);
    synth_stop(-1);
}

bool muse_apps_sound_pull(int16_t *out, size_t frames)
{
    int64_t now = esp_timer_get_time();
    int64_t gap = now - s_last_pull;
    s_last_pull = now;
    if (!atomic_load(&s_enabled)) return false;
    if (!muse_apps_on_screen(MUSE_APP_LUAPPS) || muse_state_asleep()) {
        if (synth_busy(-1)) synth_stop(-1);
        return false;
    }
    /* The voice task was away (a call, a note, a reply): what queued up then
     * is stale. 20 ms chunks normally, so 250 ms is a long way off. */
    if (gap > 250000 && synth_busy(-1)) {
        synth_stop(-1);
        return false;
    }
    return synth_render(out, frames);
}

/* ---- Lua ---- */

typedef struct {
    int wave, vol, ch, bpm, to;
    bool fade, loop;
} sopts_t;

static int opt_int(lua_State *L, int t, const char *k, int def, int lo, int hi)
{
    int v = def;
    if (lua_getfield(L, t, k) != LUA_TNIL) {
        lua_Number n = luaL_checknumber(L, -1);
        v = (int)(n < lo ? lo : n > hi ? hi : n);
    }
    lua_pop(L, 1);
    return v;
}

static void read_opts(lua_State *L, int t, sopts_t *o)
{
    if (lua_isnoneornil(L, t)) return;
    luaL_checktype(L, t, LUA_TTABLE);
    if (lua_getfield(L, t, "wave") != LUA_TNIL) {
        int w = synth_wave(luaL_checkstring(L, -1));
        if (w < 0) luaL_error(L, "wave: square, pulse, triangle, saw, sine or noise");
        o->wave = w;
    }
    lua_pop(L, 1);
    o->vol = opt_int(L, t, "volume", o->vol, 0, 100);
    o->ch = opt_int(L, t, "ch", o->ch, 1, SYNTH_CH);
    o->bpm = opt_int(L, t, "bpm", o->bpm, 20, 600);
    o->to = opt_int(L, t, "to", o->to, 0, 7900);
    if (lua_getfield(L, t, "fade") != LUA_TNIL) o->fade = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (lua_getfield(L, t, "loop") != LUA_TNIL) o->loop = lua_toboolean(L, -1);
    lua_pop(L, 1);
}

static int l_tone(lua_State *L)
{
    lua_Number f = luaL_checknumber(L, 1);
    lua_Number ms = luaL_checknumber(L, 2);
    sopts_t o = {.wave = WAVE_SQUARE, .vol = 50, .ch = 3, .bpm = 120, .to = -1};
    read_opts(L, 3, &o);
    int f0 = (int)(f < 20 ? 20 : f > 7900 ? 7900 : f);
    synth_note_t n = {
        .f0 = (uint16_t)f0,
        .f1 = (uint16_t)(o.to >= 20 ? o.to : f0),
        .ms = (uint16_t)(ms < 10 ? 10 : ms > 10000 ? 10000 : ms),
        .wave = (uint8_t)o.wave,
        .vol = (uint8_t)o.vol,
        .fade = o.fade,
    };
    synth_play(o.ch - 1, &n, 1, o.loop);
    return 0;
}

static int l_play(lua_State *L)
{
    const char *s = luaL_checkstring(L, 1);
    sopts_t o = {.wave = WAVE_SQUARE, .vol = 40, .ch = 1, .bpm = 120};
    read_opts(L, 2, &o);
    synth_note_t *notes = lua_newuserdatauv(L, sizeof(synth_note_t) * SYNTH_NOTES, 0);   /* scratch, GC'd */
    char err[120];
    int n = synth_melody(s, o.bpm, o.wave, o.vol, notes, SYNTH_NOTES, err, sizeof(err));
    if (n < 0) return luaL_error(L, "%s", err);
    synth_play(o.ch - 1, notes, n, o.loop);
    return 0;
}

static int l_effect(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    sopts_t o = {.wave = WAVE_SQUARE, .vol = 60, .ch = 4, .bpm = 120};
    read_opts(L, 2, &o);
    synth_note_t notes[8];
    int n = synth_effect(name, o.vol, notes, 8);
    if (n < 0) return luaL_error(L, "sound.effect: unknown \"%s\" (%s)", name, synth_effect_names);
    synth_play(o.ch - 1, notes, n, o.loop);
    return 0;
}

static int l_stop(lua_State *L)
{
    int ch = (int)luaL_optinteger(L, 1, 0);
    synth_stop(ch >= 1 && ch <= SYNTH_CH ? ch - 1 : -1);
    return 0;
}

static int l_busy(lua_State *L)
{
    int ch = (int)luaL_optinteger(L, 1, 0);
    lua_pushboolean(L, synth_busy(ch >= 1 && ch <= SYNTH_CH ? ch - 1 : -1));
    return 1;
}

void luapp_sound_open(lua_State *L)
{
    static const luaL_Reg fns[] = {{"tone", l_tone}, {"play", l_play}, {"effect", l_effect},
                                   {"stop", l_stop}, {"busy", l_busy}, {NULL, NULL}};
    luaL_newlib(L, fns);
    lua_setglobal(L, "sound");
}
