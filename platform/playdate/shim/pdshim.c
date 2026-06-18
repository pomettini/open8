/** @file pdshim.c
 *
 *  Implementation of the SDL3 compatibility shim for the Playdate port.
 *  See platform/playdate/shim/SDL3/SDL.h for the rationale.
 *
 *  SPDX-License-Identifier: MIT
 **/
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "SDL3/SDL.h"
#include "pdshim.h"

static PlaydateAPI* pd = NULL;
static volatile uint32_t g_buttons = 0;

/* ---- lifecycle / glue -------------------------------------------------- */

void pd_shim_init(PlaydateAPI* p)
{
    pd = p;
}

void pd_shim_set_buttons(uint32_t mask)
{
    g_buttons = mask;
}

uint32_t pd_shim_ticks(void)
{
    /* Microseconds on both device and simulator. Raw DWT access faults because
     * Playdate game code is unprivileged, so getElapsedTime() is the supported
     * high-resolution monotonic timer for the profiling splits. */
    return pd ? (uint32_t)(pd->system->getElapsedTime() * 1000000.0f) : 0;
}

int pd_shim_ticks_are_cycles(void)
{
    return 0; /* always microseconds for now */
}

/* ---- logging / errors / strings --------------------------------------- */

void SDL_Log(const char* fmt, ...)
{
    if (!pd) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pd->system->logToConsole("%s", buf);
}

const char* SDL_GetError(void) { return ""; }

int SDL_asprintf(char** strp, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (len < 0) { *strp = NULL; return -1; }

    *strp = (char*)malloc((size_t)len + 1);
    if (!*strp) return -1;

    va_start(ap, fmt);
    vsnprintf(*strp, (size_t)len + 1, fmt, ap);
    va_end(ap);
    return len;
}

const char* SDL_GetBasePath(void) { return "/"; }

/* ---- timing ------------------------------------------------------------ */

Uint64 SDL_GetTicks(void)
{
    return pd ? (Uint64)pd->system->getCurrentTimeMilliseconds() : 0;
}

Uint64 SDL_GetPerformanceCounter(void) { return (Uint64)pd_shim_ticks(); }
void   SDL_Delay(Uint32 ms)            { (void)ms; }
void   SDL_DelayNS(Uint64 ns)          { (void)ns; }

/* ---- textures (real malloc-backed buffers) ----------------------------- */

SDL_Texture* SDL_CreateTexture(SDL_Renderer* r, SDL_PixelFormat fmt, SDL_TextureAccess access, int w, int h)
{
    (void)r; (void)access;
    int bpp = SDL_BYTESPERPIXEL(fmt);
    if (bpp <= 0) bpp = 4;

    SDL_Texture* t = (SDL_Texture*)calloc(1, sizeof(SDL_Texture));
    if (!t) return NULL;
    t->w = w; t->h = h; t->bpp = bpp; t->pitch = w * bpp;
    t->pixels = (uint8_t*)calloc(1, (size_t)t->pitch * (size_t)h);
    if (!t->pixels) { free(t); return NULL; }
    return t;
}

void SDL_DestroyTexture(SDL_Texture* t)
{
    if (!t) return;
    free(t->pixels);
    free(t);
}

bool SDL_LockTexture(SDL_Texture* t, const SDL_Rect* rect, void** pixels, int* pitch)
{
    (void)rect;
    if (!t) return false;
    *pixels = t->pixels;
    *pitch  = t->pitch;
    return true;
}

void SDL_UnlockTexture(SDL_Texture* t) { (void)t; }

bool SDL_UpdateTexture(SDL_Texture* t, const SDL_Rect* rect, const void* pixels, int pitch)
{
    (void)rect;
    if (!t || !pixels) return false;
    const uint8_t* src = (const uint8_t*)pixels;
    int copy = pitch < t->pitch ? pitch : t->pitch;
    for (int y = 0; y < t->h; y++)
        memcpy(t->pixels + (size_t)y * t->pitch, src + (size_t)y * pitch, (size_t)copy);
    return true;
}

bool SDL_SetTextureBlendMode(SDL_Texture* t, SDL_BlendMode mode) { (void)t; (void)mode; return true; }
bool SDL_SetTextureScaleMode(SDL_Texture* t, SDL_ScaleMode mode) { (void)t; (void)mode; return true; }

/* ---- rendering (no-ops; pd_main owns the real framebuffer) ------------- */

bool SDL_RenderClear(SDL_Renderer* r) { (void)r; return true; }
bool SDL_RenderTexture(SDL_Renderer* r, SDL_Texture* t, const SDL_FRect* s, const SDL_FRect* d) { (void)r; (void)t; (void)s; (void)d; return true; }
void SDL_RenderPresent(SDL_Renderer* r) { (void)r; }
bool SDL_SetRenderDrawColor(SDL_Renderer* r, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) { (void)r; (void)cr; (void)cg; (void)cb; (void)ca; return true; }

bool SDL_GetRenderOutputSize(SDL_Renderer* r, int* w, int* h) { (void)r; if (w) *w = 400; if (h) *h = 240; return true; }
SDL_Window* SDL_GetRenderWindow(SDL_Renderer* r) { (void)r; return (SDL_Window*)1; }
bool SDL_GetWindowSize(SDL_Window* win, int* w, int* h) { (void)win; if (w) *w = 400; if (h) *h = 240; return true; }

SDL_PropertiesID SDL_GetRendererProperties(SDL_Renderer* r) { (void)r; return 0; }
void* SDL_GetPointerProperty(SDL_PropertiesID props, const char* name, void* def) { (void)props; (void)name; return def; }

const SDL_PixelFormatDetails* SDL_GetPixelFormatDetails(SDL_PixelFormat fmt)
{
    static SDL_PixelFormatDetails details;
    details.format = fmt;
    details.bytes_per_pixel = SDL_BYTESPERPIXEL(fmt);
    return &details;
}

Uint32 SDL_MapRGB(const SDL_PixelFormatDetails* details, const SDL_Palette* pal, Uint8 r, Uint8 g, Uint8 b)
{
    (void)details; (void)pal;
    return 0xFF000000u | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
}

/* ---- input ------------------------------------------------------------- */

SDL_Gamepad* SDL_GetGamepadFromPlayerIndex(int index) { return index == 0 ? (SDL_Gamepad*)1 : NULL; }
SDL_Gamepad* SDL_GetGamepadFromID(SDL_JoystickID id) { (void)id; return (SDL_Gamepad*)1; }
const char*  SDL_GetGamepadName(SDL_Gamepad* gp) { (void)gp; return "Playdate"; }
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID id) { (void)id; return (SDL_Gamepad*)1; }
void         SDL_CloseGamepad(SDL_Gamepad* gp) { (void)gp; }

bool SDL_GetGamepadButton(SDL_Gamepad* gp, SDL_GamepadButton button)
{
    if (!gp) return false;
    return (g_buttons >> (uint32_t)button) & 1u;
}

const bool* SDL_GetKeyboardState(int* numkeys)
{
    static const bool keys[SDL_SHIM_SCANCODE_COUNT] = { false };
    if (numkeys) *numkeys = SDL_SHIM_SCANCODE_COUNT;
    return keys;
}

SDL_Finger** SDL_GetTouchFingers(SDL_JoystickID touchID, int* count)
{
    (void)touchID;
    if (count) *count = 0;
    return NULL;
}

bool SDL_EnumerateDirectory(const char* path, SDL_EnumerateDirectoryCallback cb, void* userdata)
{
    (void)path; (void)cb; (void)userdata;
    return false; /* Current Playdate test carts are embedded. */
}
