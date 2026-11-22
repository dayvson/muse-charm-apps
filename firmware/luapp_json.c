/*
 * json.decode / json.encode for Lua apps, straight between text and Lua
 * values (in the app's own PSRAM heap; no cJSON tree in internal RAM).
 * JSON null decodes to nil (so it drops out of tables); arrays become
 * sequences. Pure Lua C API: tests/ runs it on the host.
 */

#include "luapp_json.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"

#define MAX_DEPTH 32

typedef struct {
    lua_State *L;
    const char *start, *p, *end;
    int depth;
} dec_t;

static void skip_ws(dec_t *d)
{
    while (d->p < d->end && (*d->p == ' ' || *d->p == '\t' || *d->p == '\n' || *d->p == '\r')) d->p++;
}

static int fail(dec_t *d, const char *what)
{
    return luaL_error(d->L, "json: %s at byte %d", what, (int)(d->p - d->start));
}

static void decode_value(dec_t *d);

static unsigned hex4(dec_t *d)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        if (d->p >= d->end) fail(d, "short \\u escape");
        char c = *d->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else fail(d, "bad \\u escape");
    }
    return v;
}

static void decode_string(dec_t *d)
{
    luaL_Buffer b;
    d->p++;   /* opening quote */
    luaL_buffinit(d->L, &b);
    for (;;) {
        if (d->p >= d->end) fail(d, "unterminated string");
        char c = *d->p++;
        if (c == '"') break;
        if ((unsigned char)c < 0x20) fail(d, "control character in string");
        if (c != '\\') {
            luaL_addchar(&b, c);
            continue;
        }
        if (d->p >= d->end) fail(d, "unterminated escape");
        c = *d->p++;
        switch (c) {
        case '"': case '\\': case '/': luaL_addchar(&b, c); break;
        case 'b': luaL_addchar(&b, '\b'); break;
        case 'f': luaL_addchar(&b, '\f'); break;
        case 'n': luaL_addchar(&b, '\n'); break;
        case 'r': luaL_addchar(&b, '\r'); break;
        case 't': luaL_addchar(&b, '\t'); break;
        case 'u': {
            unsigned cp = hex4(d);
            if (cp >= 0xd800 && cp < 0xdc00 && d->end - d->p >= 6 && d->p[0] == '\\' && d->p[1] == 'u') {
                d->p += 2;
                unsigned lo = hex4(d);
                cp = (lo >= 0xdc00 && lo < 0xe000) ? 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00) : 0xfffd;
            } else if (cp >= 0xd800 && cp < 0xe000) {
                cp = 0xfffd;
            }
            char u[8];
            int n = 0;
            if (cp < 0x80) u[n++] = (char)cp;
            else if (cp < 0x800) { u[n++] = (char)(0xc0 | cp >> 6); u[n++] = (char)(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { u[n++] = (char)(0xe0 | cp >> 12); u[n++] = (char)(0x80 | ((cp >> 6) & 63)); u[n++] = (char)(0x80 | (cp & 63)); }
            else { u[n++] = (char)(0xf0 | cp >> 18); u[n++] = (char)(0x80 | ((cp >> 12) & 63)); u[n++] = (char)(0x80 | ((cp >> 6) & 63)); u[n++] = (char)(0x80 | (cp & 63)); }
            luaL_addlstring(&b, u, (size_t)n);
            break;
        }
        default: fail(d, "bad escape");
        }
    }
    luaL_pushresult(&b);
}

static void decode_number(dec_t *d)
{
    char buf[64];
    size_t n = 0;
    while (d->p < d->end && n < sizeof(buf) - 1 && strchr("+-0123456789.eE", *d->p)) buf[n++] = *d->p++;
    buf[n] = 0;
    if (n == 0 || !lua_stringtonumber(d->L, buf)) fail(d, "bad number");
}

static void literal(dec_t *d, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(d->end - d->p) < n || memcmp(d->p, word, n)) fail(d, "unexpected character");
    d->p += n;
}

static void decode_value(dec_t *d)
{
    skip_ws(d);
    if (d->p >= d->end) fail(d, "unexpected end");
    luaL_checkstack(d->L, 4, "json nesting");
    switch (*d->p) {
    case '{': {
        if (++d->depth > MAX_DEPTH) fail(d, "nested too deep");
        d->p++;
        lua_newtable(d->L);
        skip_ws(d);
        if (d->p < d->end && *d->p == '}') { d->p++; d->depth--; return; }
        for (;;) {
            skip_ws(d);
            if (d->p >= d->end || *d->p != '"') fail(d, "expected a key");
            decode_string(d);
            skip_ws(d);
            if (d->p >= d->end || *d->p++ != ':') fail(d, "expected ':'");
            decode_value(d);
            lua_rawset(d->L, -3);
            skip_ws(d);
            if (d->p < d->end && *d->p == ',') { d->p++; continue; }
            if (d->p < d->end && *d->p == '}') { d->p++; break; }
            fail(d, "expected ',' or '}'");
        }
        d->depth--;
        return;
    }
    case '[': {
        if (++d->depth > MAX_DEPTH) fail(d, "nested too deep");
        d->p++;
        lua_newtable(d->L);
        skip_ws(d);
        if (d->p < d->end && *d->p == ']') { d->p++; d->depth--; return; }
        lua_Integer i = 1;
        for (;;) {
            decode_value(d);
            lua_rawseti(d->L, -2, i++);
            skip_ws(d);
            if (d->p < d->end && *d->p == ',') { d->p++; continue; }
            if (d->p < d->end && *d->p == ']') { d->p++; break; }
            fail(d, "expected ',' or ']'");
        }
        d->depth--;
        return;
    }
    case '"': decode_string(d); return;
    case 't': literal(d, "true"); lua_pushboolean(d->L, 1); return;
    case 'f': literal(d, "false"); lua_pushboolean(d->L, 0); return;
    case 'n': literal(d, "null"); lua_pushnil(d->L); return;
    default: decode_number(d); return;
    }
}

static int l_decode(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    dec_t d = {L, s, s, s + len, 0};
    decode_value(&d);
    skip_ws(&d);
    if (d.p != d.end) fail(&d, "trailing characters");
    return 1;
}

/* ---- encode ---- */

static void encode_value(lua_State *L, luaL_Buffer *b, int idx, int depth);

static void encode_string(luaL_Buffer *b, const char *s, size_t n)
{
    luaL_addchar(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': luaL_addstring(b, "\\\""); break;
        case '\\': luaL_addstring(b, "\\\\"); break;
        case '\n': luaL_addstring(b, "\\n"); break;
        case '\r': luaL_addstring(b, "\\r"); break;
        case '\t': luaL_addstring(b, "\\t"); break;
        default:
            if (c < 0x20) {
                char e[8];
                snprintf(e, sizeof(e), "\\u%04x", c);
                luaL_addstring(b, e);
            } else {
                luaL_addchar(b, (char)c);
            }
        }
    }
    luaL_addchar(b, '"');
}

/* A table is an array if its keys are exactly 1..n (n > 0), or it's empty. */
static lua_Integer array_length(lua_State *L, int idx)
{
    lua_Integer n = 0, count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);
        if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 1) {
            lua_pop(L, 1);
            return -1;
        }
        lua_Integer k = lua_tointeger(L, -1);
        if (k > n) n = k;
        count++;
    }
    return n == count ? n : -1;
}

static void encode_value(lua_State *L, luaL_Buffer *b, int idx, int depth)
{
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL: luaL_addstring(b, "null"); break;
    case LUA_TBOOLEAN: luaL_addstring(b, lua_toboolean(L, idx) ? "true" : "false"); break;
    case LUA_TNUMBER: {
        char n[40];
        if (lua_isinteger(L, idx)) {
            snprintf(n, sizeof(n), LUA_INTEGER_FMT, (LUAI_UACINT)lua_tointeger(L, idx));
        } else {
            double v = lua_tonumber(L, idx);
            if (!isfinite(v)) luaL_error(L, "json: can't encode inf or nan");
            snprintf(n, sizeof(n), "%.14g", v);
        }
        luaL_addstring(b, n);
        break;
    }
    case LUA_TSTRING: {
        size_t n;
        const char *s = lua_tolstring(L, idx, &n);
        encode_string(b, s, n);
        break;
    }
    case LUA_TTABLE: {
        if (depth > MAX_DEPTH) luaL_error(L, "json: nested too deep (a cycle?)");
        luaL_checkstack(L, 4, "json nesting");
        lua_Integer n = array_length(L, idx);
        if (n >= 0) {
            luaL_addchar(b, '[');
            for (lua_Integer i = 1; i <= n; i++) {
                if (i > 1) luaL_addchar(b, ',');
                lua_rawgeti(L, idx, i);
                encode_value(L, b, -1, depth + 1);
                lua_pop(L, 1);
            }
            luaL_addchar(b, ']');
        } else {
            bool first = true;
            luaL_addchar(b, '{');
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                if (lua_type(L, -2) != LUA_TSTRING && lua_type(L, -2) != LUA_TNUMBER) {
                    luaL_error(L, "json: keys must be strings or numbers");
                }
                if (!first) luaL_addchar(b, ',');
                first = false;
                lua_pushvalue(L, -2);
                size_t kn;
                const char *k = lua_tolstring(L, -1, &kn);   /* a copy: tolstring mustn't change the real key */
                encode_string(b, k, kn);
                lua_pop(L, 1);
                luaL_addchar(b, ':');
                encode_value(L, b, -1, depth + 1);
                lua_pop(L, 1);
            }
            luaL_addchar(b, '}');
        }
        break;
    }
    default: luaL_error(L, "json: can't encode a %s", luaL_typename(L, idx));
    }
}

static int l_encode(lua_State *L)
{
    luaL_checkany(L, 1);
    lua_settop(L, 1);
    /* The buffer lives on the stack under the value; encode with the value's absolute index. */
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    encode_value(L, &b, 1, 0);
    luaL_pushresult(&b);
    return 1;
}

void luapp_json_open(lua_State *L)
{
    static const luaL_Reg fns[] = {{"decode", l_decode}, {"encode", l_encode}, {NULL, NULL}};
    luaL_newlib(L, fns);
    lua_setglobal(L, "json");
}
