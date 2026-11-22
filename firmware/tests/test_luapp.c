/* Host test: the Lua sandbox, the json module, the memory cap. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "luapp_sandbox.h"

static size_t used, cap = 256 * 1024;

static void *alloc(void *ud, void *p, size_t osize, size_t nsize)
{
    (void)ud;
    if (!p) osize = 0;
    if (nsize == 0) {
        free(p);
        used -= osize;
        return NULL;
    }
    if (used - osize + nsize > cap) return NULL;
    void *q = realloc(p, nsize);
    if (q) used = used - osize + nsize;
    return q;
}

static const char *TESTS =
    "local function check(c, m) if not c then error(m, 2) end end\n"
    "check(io == nil and os == nil and debug == nil and package == nil and require == nil, 'unsafe libs')\n"
    "check(load == nil and dofile == nil and loadfile == nil and collectgarbage == nil, 'unsafe base')\n"
    "check(string.dump == nil, 'string.dump')\n"
    "check(string.format('%d', 3) == '3' and math.floor(2.5) == 2 and utf8.char(233) == '\\195\\169', 'libs work')\n"
    "local t = json.decode('{\"a\":[1,2.5,\"x\\\\n\\\\u00e9\\\\ud83d\\\\ude00\"],\"b\":true,\"c\":null,\"d\":{}}')\n"
    "check(t.a[1] == 1 and math.type(t.a[1]) == 'integer' and t.a[2] == 2.5 and t.b == true and t.c == nil, 'decode')\n"
    "check(t.a[3] == 'x\\n\\195\\169\\240\\159\\152\\128', 'decode escapes')\n"
    "check(json.encode({1,2,3}) == '[1,2,3]' and json.encode({}) == '[]' and json.encode('a\"b') == '\"a\\\\\"b\"', 'encode')\n"
    "local r = json.decode(json.encode({x = {y = {1, 'two', false}}, n = 1.5}))\n"
    "check(r.x.y[2] == 'two' and r.x.y[3] == false and r.n == 1.5, 'roundtrip')\n"
    "for _, bad in ipairs({'{', '[1,]', '{\"a\" 1}', 'nul', '\"abc', '1 2', '[' .. string.rep('[', 40)}) do\n"
    "  check(not pcall(json.decode, bad), 'accepted bad json: ' .. bad)\n"
    "end\n"
    "local cyc = {}; cyc.self = cyc\n"
    "check(not pcall(json.encode, cyc), 'cycle')\n"
    "check(not pcall(json.encode, {f = print}), 'function')\n"
    "return 'ok'\n";

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    lua_State *L = luapp_sandbox_new(alloc, NULL);
    CHECK(L);
    int rc = luapp_sandbox_load(L, TESTS, strlen(TESTS), "=tests");
    if (rc == LUA_OK) rc = lua_pcall(L, 0, 1, 0);
    if (rc != LUA_OK) printf("FAIL lua: %s\n", lua_tostring(L, -1)), fails++;
    lua_settop(L, 0);

    /* Bytecode is refused even though the VM could load it. */
    CHECK(luapp_sandbox_load(L, "\x1bLua\x54", 5, "=bin") != LUA_OK);
    lua_settop(L, 0);
    /* A syntax error comes back with its line. */
    CHECK(luapp_sandbox_load(L, "x = 1\nif then\n", 13, "=app") == LUA_ERRSYNTAX);
    CHECK(strstr(lua_tostring(L, -1), "app:2:") != NULL);
    lua_settop(L, 0);
    /* Running out of the cap is a catchable error, not a crash. */
    const char *hog = "local t = {} for i = 1, 1e7 do t[i] = string.rep('x', 100) .. i end";
    CHECK(luapp_sandbox_load(L, hog, strlen(hog), "=hog") == LUA_OK);
    CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRMEM);
    lua_settop(L, 0);
    lua_close(L);
    CHECK(used == 0);
    printf("%s\n", fails ? "FAIL" : "lua sandbox + json OK");
    return fails != 0;
}
