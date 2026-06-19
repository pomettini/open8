/** @file tests.c
 *
 *  A portable PICO-8 emulator written in C.
 *
 *  Copyright (c) 2025-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#include <SDL3/SDL.h>
#include <stdlib.h>
#include "z8lua/lua.h"
#include "z8lua/lualib.h"
#include "api.h"

static uint8_t ram[32768];

static int lcf_results(lua_State* L)
{
    lua_Number value = luaL_checknumber(L, 1);
    lua_pushnumber(L, value);
    lua_pushnumber(L, value + fix32_value(1, 0));
    lua_pushnumber(L, value + fix32_value(2, 0));
    return 3;
}

static int lcf_nested(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_call(L, 1, 1);
    lua_pushnumber(L, lua_tonumber(L, -1) + fix32_value(1, 0));
    return 1;
}

static int lcf_grow_stack(lua_State* L)
{
    int i;
    luaL_checkany(L, 1);
    if (!lua_checkstack(L, 256))
    {
        return luaL_error(L, "could not grow stack");
    }
    for (i = 0; i < 256; i++)
    {
        lua_pushinteger(L, i);
    }
    return 1;
}

static int lcf_error(lua_State* L)
{
    return luaL_error(L, "expected light-C test error");
}

static void register_lcf_tests(lua_State* L)
{
    lua_pushcfunction(L, lcf_results);
    lua_setglobal(L, "lcf_results");
    lua_pushcfunction(L, lcf_nested);
    lua_setglobal(L, "lcf_nested");
    lua_pushcfunction(L, lcf_grow_stack);
    lua_setglobal(L, "lcf_grow_stack");
    lua_pushcfunction(L, lcf_error);
    lua_setglobal(L, "lcf_error");
}

int main()
{
    lua_State* vm = luaL_newstate();
    if (!vm)
    {
        SDL_Log("Couldn't create Lua state.");
        return EXIT_FAILURE;
    }
    lua_setpico8memory(vm, ram);
    luaL_openlibs(vm);
    init_api(vm);
    register_lcf_tests(vm);

    if (luaL_loadfile(vm, "tests.lua") || lua_pcall(vm, 0, 1, 0))
    {
        SDL_Log("Lua error: %s", lua_tostring(vm, -1));
        lua_pop(vm, 1);
        return EXIT_FAILURE;
    }

    if (lua_isnumber(vm, -1))
    {
        int result = lua_tointeger(vm, -1);
        if (result)
        {
            lua_pop(vm, 1);
            lua_close(vm);
            return EXIT_FAILURE;
        }
    }
    else
    {
        SDL_Log("Lua script did not return a number.");
    }

    lua_pop(vm, 1);

#ifdef OPEN8_VM_LCF_FAST_TEST_COUNTER
    {
        extern uint32_t open8_vm_lcf_fast_count;
        if (open8_vm_lcf_fast_count == 0)
        {
            SDL_Log("Light-C fast path was enabled but never executed.");
            lua_close(vm);
            return EXIT_FAILURE;
        }
        SDL_Log("Light-C fast path executed %u times.",
                (unsigned int)open8_vm_lcf_fast_count);
    }
#endif

    lua_close(vm);

    return EXIT_SUCCESS;
}
