#pragma once

/*
 * A Lua state for an app Muse wrote: the base library without load, dofile,
 * loadfile or collectgarbage; string (without dump), table, math, utf8,
 * coroutine; json. No io, os, debug or package: an app reaches the device
 * only through the API the host adds (ui, time, http, store, app). Pure Lua
 * C API: tests/ runs it on the host.
 */

#include <stddef.h>

#include "lua.h"

lua_State *luapp_sandbox_new(lua_Alloc alloc, void *ud);

/* Compiles source text (never bytecode) as chunk `name`; pushes the function,
 * or an error message, like luaL_loadbufferx. */
int luapp_sandbox_load(lua_State *L, const char *code, size_t len, const char *name);
