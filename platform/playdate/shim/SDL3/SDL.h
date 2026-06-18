/** @file SDL.h  (Playdate shim)
 *
 *  Minimal SDL3 compatibility shim for the open8 Playdate port.
 *
 *  This header is placed on the include path ONLY for the Playdate build
 *  (OPEN8_PLATFORM_PLAYDATE). It lets api.c / core.c / memory.c / p8scii.c /
 *  auxiliary.c compile unchanged: most symbols are thin libc wrappers, textures
 *  are backed by plain malloc buffers, and render/audio calls are no-ops. The
 *  real on-screen output and input come from platform/playdate/pd_main.c.
 *
 *  The upstream SDL build is unaffected — it never sees this file.
 *
 *  SPDX-License-Identifier: MIT
 **/
#ifndef OPEN8_SDL_SHIM_H
#define OPEN8_SDL_SHIM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Integer typedefs ------------------------------------------------- */
typedef int8_t   Sint8;
typedef uint8_t  Uint8;
typedef int16_t  Sint16;
typedef uint16_t Uint16;
typedef int32_t  Sint32;
typedef uint32_t Uint32;
typedef int64_t  Sint64;
typedef uint64_t Uint64;

/* ---- Opaque platform handles ------------------------------------------ */
typedef struct SDL_Window   SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Gamepad  SDL_Gamepad;
typedef struct SDL_Palette  SDL_Palette;
typedef Uint32 SDL_JoystickID;
typedef Uint32 SDL_PropertiesID;

/* Texture is backed by a real pixel buffer so Lock/Update/Create behave. */
typedef struct SDL_Texture
{
    int      w, h;
    int      pitch;
    int      bpp;
    uint8_t* pixels;
} SDL_Texture;

typedef struct SDL_FRect { float x, y, w, h; } SDL_FRect;
typedef struct SDL_Rect  { int   x, y, w, h; } SDL_Rect;
typedef struct SDL_Finger { SDL_JoystickID id; float x, y, pressure; } SDL_Finger;

/* ---- Pixel formats ----------------------------------------------------- */
/* The low byte encodes bytes-per-pixel so SDL_BYTESPERPIXEL works. */
typedef enum SDL_PixelFormat
{
    SDL_PIXELFORMAT_UNKNOWN = 0,
    SDL_PIXELFORMAT_RGBA32  = 4
} SDL_PixelFormat;

typedef struct SDL_PixelFormatDetails { SDL_PixelFormat format; int bytes_per_pixel; } SDL_PixelFormatDetails;

#define SDL_BYTESPERPIXEL(x)        ((int)(x))
#define SDL_ISPIXELFORMAT_PACKED(x) (1)

typedef enum SDL_TextureAccess
{
    SDL_TEXTUREACCESS_STATIC = 0,
    SDL_TEXTUREACCESS_STREAMING,
    SDL_TEXTUREACCESS_TARGET
} SDL_TextureAccess;

typedef enum SDL_ScaleMode { SDL_SCALEMODE_NEAREST = 0, SDL_SCALEMODE_LINEAR } SDL_ScaleMode;
typedef enum SDL_BlendMode { SDL_BLENDMODE_NONE = 0, SDL_BLENDMODE_BLEND } SDL_BlendMode;

/* ---- Events (only fields referenced by core.c handle_events) ----------- */
typedef enum SDL_EventType
{
    SDL_EVENT_QUIT = 0x100,
    SDL_EVENT_WINDOW_RESIZED,
    SDL_EVENT_KEY_DOWN,
    SDL_EVENT_MOUSE_BUTTON_DOWN,
    SDL_EVENT_MOUSE_BUTTON_UP,
    SDL_EVENT_FINGER_DOWN,
    SDL_EVENT_FINGER_UP,
    SDL_EVENT_GAMEPAD_ADDED,
    SDL_EVENT_GAMEPAD_REMOVED,
    SDL_EVENT_GAMEPAD_BUTTON_DOWN
} SDL_EventType;

typedef struct { Uint32 type; Uint32 key; bool repeat; } SDL_KeyboardEvent;
typedef struct { Uint32 type; int button; float x, y; } SDL_MouseButtonEvent;
typedef struct { Uint32 type; SDL_JoystickID which; Uint8 button; } SDL_GamepadButtonEvent;
typedef struct { Uint32 type; SDL_JoystickID which; } SDL_GamepadDeviceEvent;
typedef struct { Uint32 type; float x, y; } SDL_TouchFingerEvent;

typedef union SDL_Event
{
    Uint32                 type;
    SDL_KeyboardEvent      key;
    SDL_MouseButtonEvent   button;
    SDL_GamepadButtonEvent gbutton;
    SDL_GamepadDeviceEvent gdevice;
    SDL_TouchFingerEvent   tfinger;
} SDL_Event;

#define SDL_BUTTON_LEFT 1

/* ---- Gamepad button ids (must match pd_main button mask) --------------- */
typedef enum SDL_GamepadButton
{
    SDL_GAMEPAD_BUTTON_SOUTH = 0,
    SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT
} SDL_GamepadButton;

/* ---- Scancodes (keyboard path returns an all-zero state array) --------- */
typedef enum SDL_Scancode
{
    SDL_SCANCODE_LEFT = 1, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
    SDL_SCANCODE_Z, SDL_SCANCODE_Y, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V,
    SDL_SCANCODE_5, SDL_SCANCODE_7, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_7,
    SDL_SHIM_SCANCODE_COUNT
} SDL_Scancode;

/* ---- Keycodes (only used by handle_events, which Playdate never calls) - */
enum
{
    SDLK_ESCAPE = 0x1B, SDLK_SPACE = 0x20, SDLK_EQUALS = 0x3D,
    SDLK_C = 0x63, SDLK_V = 0x76, SDLK_X = 0x78, SDLK_Y = 0x79, SDLK_Z = 0x7A,
    SDLK_LEFT = 0x4000004F, SDLK_RIGHT, SDLK_SOFTLEFT, SDLK_SELECT
};

/* ---- Directory enumeration -------------------------------------------- */
typedef enum SDL_EnumerationResult { SDL_ENUM_CONTINUE = 0, SDL_ENUM_SUCCESS, SDL_ENUM_FAILURE } SDL_EnumerationResult;
typedef SDL_EnumerationResult (*SDL_EnumerateDirectoryCallback)(void* userdata, const char* dirname, const char* fname);

/* ---- Misc macros ------------------------------------------------------- */
#define SDL_arraysize(a) (sizeof(a) / sizeof((a)[0]))
#define SDL_NS_PER_US    1000ULL
#define SDL_PRIu32       "u"
#define SDL_HINT_MOUSE_TOUCH_EVENTS "SDL_MOUSE_TOUCH_EVENTS"
#define SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER "SDL.renderer.texture_formats"

/* ---- libc-backed helpers ---------------------------------------------- */
#define SDL_malloc(sz)         malloc(sz)
#define SDL_calloc(n, sz)      calloc((n), (sz))
#define SDL_realloc(p, sz)     realloc((p), (sz))
#define SDL_free(p)            free(p)
#define SDL_memset(d, c, n)    memset((d), (c), (n))
#define SDL_memcpy(d, s, n)    memcpy((d), (s), (n))
#define SDL_memcmp(a, b, n)    memcmp((a), (b), (n))
#define SDL_strstr(a, b)       strstr((a), (b))
#define SDL_strtod(s, e)       strtod((s), (e))

/* ---- Functions implemented in pdshim.c -------------------------------- */
void        SDL_Log(const char* fmt, ...);
const char* SDL_GetError(void);
int         SDL_asprintf(char** strp, const char* fmt, ...);
const char* SDL_GetBasePath(void);

Uint64      SDL_GetTicks(void);
Uint64      SDL_GetPerformanceCounter(void);
void        SDL_Delay(Uint32 ms);
void        SDL_DelayNS(Uint64 ns);

/* Textures backed by malloc buffers. */
SDL_Texture* SDL_CreateTexture(SDL_Renderer* r, SDL_PixelFormat fmt, SDL_TextureAccess access, int w, int h);
void         SDL_DestroyTexture(SDL_Texture* t);
bool         SDL_LockTexture(SDL_Texture* t, const SDL_Rect* rect, void** pixels, int* pitch);
void         SDL_UnlockTexture(SDL_Texture* t);
bool         SDL_UpdateTexture(SDL_Texture* t, const SDL_Rect* rect, const void* pixels, int pitch);
bool         SDL_SetTextureBlendMode(SDL_Texture* t, SDL_BlendMode mode);
bool         SDL_SetTextureScaleMode(SDL_Texture* t, SDL_ScaleMode mode);

/* Rendering — no-ops on Playdate (pd_main owns the framebuffer). */
bool        SDL_RenderClear(SDL_Renderer* r);
bool        SDL_RenderTexture(SDL_Renderer* r, SDL_Texture* t, const SDL_FRect* src, const SDL_FRect* dst);
void        SDL_RenderPresent(SDL_Renderer* r);
bool        SDL_SetRenderDrawColor(SDL_Renderer* r, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca);
bool        SDL_GetRenderOutputSize(SDL_Renderer* r, int* w, int* h);
SDL_Window* SDL_GetRenderWindow(SDL_Renderer* r);
bool        SDL_GetWindowSize(SDL_Window* win, int* w, int* h);

SDL_PropertiesID            SDL_GetRendererProperties(SDL_Renderer* r);
void*                       SDL_GetPointerProperty(SDL_PropertiesID props, const char* name, void* def);
const SDL_PixelFormatDetails* SDL_GetPixelFormatDetails(SDL_PixelFormat fmt);
Uint32                      SDL_MapRGB(const SDL_PixelFormatDetails* details, const SDL_Palette* pal, Uint8 r, Uint8 g, Uint8 b);

/* Input. */
SDL_Gamepad*  SDL_GetGamepadFromPlayerIndex(int index);
SDL_Gamepad*  SDL_GetGamepadFromID(SDL_JoystickID id);
bool          SDL_GetGamepadButton(SDL_Gamepad* gp, SDL_GamepadButton button);
const char*   SDL_GetGamepadName(SDL_Gamepad* gp);
SDL_Gamepad*  SDL_OpenGamepad(SDL_JoystickID id);
void          SDL_CloseGamepad(SDL_Gamepad* gp);
const bool*   SDL_GetKeyboardState(int* numkeys);
SDL_Finger**  SDL_GetTouchFingers(SDL_JoystickID touchID, int* count);

bool SDL_EnumerateDirectory(const char* path, SDL_EnumerateDirectoryCallback cb, void* userdata);

#endif /* OPEN8_SDL_SHIM_H */
