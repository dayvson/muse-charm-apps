#include "luapp_sandbox.h"

#include "lauxlib.h"
#include "lualib.h"
#include "luapp_json.h"

static int panic(lua_State *L)
{
    (void)L;
    return 0;   /* the host only calls Lua under pcall; nothing should get here */
}

lua_State *luapp_sandbox_new(lua_Alloc alloc, void *ud)
{
    lua_State *L = lua_newstate(alloc, ud);
    if (!L) return NULL;
    lua_atpanic(L, panic);
    static const luaL_Reg libs[] = {
        {LUA_GNAME, luaopen_base},       {LUA_STRLIBNAME, luaopen_string}, {LUA_TABLIBNAME, luaopen_table},
        {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},  {LUA_COLIBNAME, luaopen_coroutine},
        {NULL, NULL},
    };
    for (const luaL_Reg *l = libs; l->func; l++) {
        luaL_requiref(L, l->name, l->func, 1);
        lua_pop(L, 1);
    }
    /* Nothing that reads files or loads code (bytecode can crash the VM). */
    static const char *const unsafe[] = {"dofile", "loadfile", "load", "collectgarbage", NULL};
    for (const char *const *n = unsafe; *n; n++) {
        lua_pushnil(L);
        lua_setglobal(L, *n);
    }
    lua_getglobal(L, LUA_STRLIBNAME);
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    luapp_json_open(L);
    return L;
}

int luapp_sandbox_load(lua_State *L, const char *code, size_t len, const char *name)
{
    return luaL_loadbufferx(L, code, len, name, "t");
}
