/** @file core.h
 *
 *  A portable PICO-8 emulator written in C.
 *
 *  Copyright (c) 2025-2026, Michael Fitzmayer. All rights reserved.
 *  SPDX-License-Identifier: MIT
 *
 **/

#ifndef CORE_H
#define CORE_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define CART_WIDTH 160
#define CART_HEIGHT 205
#define CART_DATA_SIZE 0x8020

#define MAX_CODE_SIZE 65536

typedef struct cart
{
    SDL_Texture* image;

    uint8_t cart_data[0x8020];
    uint8_t* code;
    uint32_t code_size;

    bool is_corrupt;

} cart_t;

typedef enum state
{
    STATE_MENU,
    STATE_EMULATOR

} state_t;

// Touch button state (when SDL_HINT_MOUSE_TOUCH_EVENTS is used)
extern uint8_t touch_button_state;

void handle_resize(SDL_Renderer *renderer);

bool init_core(SDL_Renderer* renderer);
void destroy_core(void);
bool handle_events(SDL_Renderer* renderer, SDL_Event* event);
bool iterate_core(SDL_Renderer* renderer);

#ifdef OPEN8_PLATFORM_PLAYDATE
// Playdate backend entry points (implemented in core.c). See pd_main.c.
bool core_pd_init(void);
bool core_pd_boot_cart(const uint8_t* data, long size);
void core_pd_update(void);
void core_pd_draw(void);
void core_pd_set_gc(int on);
#endif

#endif // CORE_H
