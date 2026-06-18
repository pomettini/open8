/** @file pdshim.h
 *
 *  Glue between the SDL shim and the Playdate runtime, owned by pd_main.c.
 *  SPDX-License-Identifier: MIT
 **/
#ifndef OPEN8_PDSHIM_H
#define OPEN8_PDSHIM_H

#include <stdint.h>
#include "pd_api.h"

/* Called once at kEventInit so the shim can log/alloc/time via the runtime. */
void pd_shim_init(PlaydateAPI* pd);

/* pd_main pushes the current button state each frame as a bitmask indexed by
 * SDL_GamepadButton; update_input() reads it back through SDL_GetGamepadButton. */
void pd_shim_set_buttons(uint32_t mask);

/* Monotonic cycle/microsecond counter for the profiler. On device this is the
 * DWT cycle counter (168 MHz); in the simulator it is elapsed microseconds. */
uint32_t pd_shim_ticks(void);
int      pd_shim_ticks_are_cycles(void); /* 1 = CYCCNT (device), 0 = microseconds */

#endif /* OPEN8_PDSHIM_H */
