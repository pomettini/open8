/** @file api.h
 *
 *  A portable PICO-8 emulator written in C.
 *
 *  Copyright (c) 2025-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef API_H
#define API_H

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdbool.h>

#include "z8lua/fix32.h"
#include "z8lua/lauxlib.h"
#include "z8lua/lua.h"

typedef struct
{
    int x, y, w, h;
    uint8_t bit;

} touch_region;

extern fix32_t seconds_since_start;

// Frame timing information (set by core.c each frame) used by stat(1).
extern uint32_t pico8_frame_start;
extern uint32_t pico8_frame_ms;

#ifdef OPEN8_PROFILE_TOOLS
// Profiling toggle: when non-zero, the dominant blitters (cls/spr/sspr/map/
// rectfill/circfill/line/pset) early-return. Set from pd_main to split t_draw
// into C-side fill vs VM call overhead. See docs/playdate-port.md.
extern int open8_profile_skip_fill;
#endif

void init_api(lua_State* L);
void update_input(SDL_Renderer* renderer);
void update_time(void);
bool point_in_touch_region(float x, float y, const touch_region* r);
const touch_region* get_touch_regions(int* count);

#endif // API_H
