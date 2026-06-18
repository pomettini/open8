/** @file profile.h
 *
 *  Lightweight opt-in counters for Playdate performance experiments.
 *
 *  SPDX-License-Identifier: MIT
 **/

#ifndef OPEN8_PROFILE_H
#define OPEN8_PROFILE_H

#include <stdint.h>

typedef struct
{
    uint32_t all_calls;
    uint32_t all_items;
    uint32_t foreach_calls;
    uint32_t foreach_items;
    uint32_t add_calls;
    uint32_t del_calls;
    uint32_t del_shifts;

    uint32_t draw_calls;
    uint32_t primitive_calls;
    uint32_t print_calls;
    uint32_t spr_calls;
    uint32_t sspr_calls;
    uint32_t map_calls;
    uint32_t map_cells;
} open8_api_profile;

#ifdef OPEN8_PROFILE_API
extern open8_api_profile open8_profile_api;
void open8_profile_api_reset(void);
#endif

#endif /* OPEN8_PROFILE_H */
