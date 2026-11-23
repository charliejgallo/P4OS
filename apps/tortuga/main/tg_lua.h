/*
 * Tortuga - a Lua program run against the world (tg_lua.c): a fresh state,
 * the orders as functions, then the world is finished.
 */
#pragma once

#include "tg_core.h"

void tg_lua_run(const char *src, tg_world_t *w);
