#pragma once

#include "lua.h"

/* Sets the global `json` = {decode = ..., encode = ...}. */
void luapp_json_open(lua_State *L);
