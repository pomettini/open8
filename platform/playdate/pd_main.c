/** @file pd_main.c
 *
 *  Playdate backend entry point for open8.
 *
 *  Boots embedded test carts, runs the z8lua VM, threshold-blits the PICO-8
 *  framebuffer to the 1-bit display, and reports update/draw/blit timing.
 *  Expensive probes are separately gated by OPEN8_PROFILE_LOAD and
 *  OPEN8_PROFILE_TOOLS so production builds do not pay their overhead.
 *
 *  SPDX-License-Identifier: MIT
 **/
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pd_api.h"

#include "core.h"        /* core_pd_init / core_pd_boot_cart / core_pd_update / core_pd_draw */
#include "memory.h"      /* pico8_ram */
#include "auxiliary.h"   /* color_lookup */
#include "audio.h"       /* audio_render */
#include "profile.h"     /* opt-in coarse API counters */
#include "pdshim.h"

/* Declared in api.c / lvm.c. Not via their headers: those pull z8lua's lua.h,
 * whose lua_State typedef collides with the Playdate SDK's pd_api_lua.h. */
#ifdef OPEN8_PROFILE_TOOLS
extern int open8_profile_skip_fill;
#endif
#ifdef OPEN8_PROFILE_LOAD
extern uint32_t open8_vm_instr_count;   /* bytecode ops executed (load characterizer) */
extern uint32_t open8_vm_ccall_count;   /* C-function calls executed */
#endif

#include "SDL3/SDL.h"    /* SDL_GAMEPAD_BUTTON_* (so the button mapping stays in sync) */

#include "generated/cart_celeste.h"
#include "generated/cart_jelpi.h"
#include "generated/cart_racer.h"
#include "generated/cart_picross.h"

/* Test matrix (docs/playdate-port.md): real game / fill-heavy / VM-heavy / light.
 * Switchable at runtime via the system menu so we can record a split per cart.
 * Populated at init because the xxd `*_len` symbols are runtime globals. */
typedef struct { const char* name; const unsigned char* data; unsigned int len; } cart_entry;
#define NUM_CARTS 4
static cart_entry  g_carts[NUM_CARTS];
static const char* g_cart_titles[NUM_CARTS];
static int         g_cart_index = 0;

static void init_cart_registry(void)
{
    g_carts[0] = (cart_entry){ "celeste", celeste_cart, celeste_cart_len };
    g_carts[1] = (cart_entry){ "jelpi",   jelpi_cart,   jelpi_cart_len };
    g_carts[2] = (cart_entry){ "racer",   racer_cart,   racer_cart_len };
    g_carts[3] = (cart_entry){ "picross", picross_cart, picross_cart_len };
    for (int i = 0; i < NUM_CARTS; i++) g_cart_titles[i] = g_carts[i].name;
}

/* PICO-8's 128x128 is nearest-neighbour upscaled to a SCALE_W x SCALE_H square,
 * centred in the 400x240 frame, with ordered (Bayer 4x4) dithering so the 16
 * PICO-8 colours render as brightness patterns on the 1-bit display. DEST_X is a
 * multiple of 8 so each output byte is exactly 8 packed pixels with no straddle. */
#define SRC_W         128
#define SRC_H         128
#define SCALE_W       240          /* 1.875x; fills the full 240px height */
#define SCALE_H       240
#define DEST_X        80            /* byte-aligned: 80/8 = 10, centred in 400 */
#define DEST_Y        0             /* (240 - 240) / 2 */
#define DEST_BYTE_X   (DEST_X / 8)  /* 10 */
#define DEST_BYTES    (SCALE_W / 8) /* 30 */

#define FB_BASE       0x6000
#define DISP_PALETTE  0x5f10

static PlaydateAPI* g_pd  = NULL;
static LCDFont*     g_font = NULL;
static int          g_booted = 0;

/* dither[final colour 0..15][bayer cell 0..15] -> 1-bit pixel. Built from the 16
 * fixed PICO-8 colour luminances and keyed by the post-display-palette colour,
 * so it stays correct under pal(). Nearest-neighbour scale tables map dest->src. */
static uint8_t g_dither[16][16];
static uint8_t g_sx[SCALE_W];
static uint8_t g_sy[SCALE_H];

static void build_display_tables(void)
{
    /* Standard recursive 4x4 Bayer matrix (same ordered-dither family as Nofrendo). */
    static const uint8_t bayer[16] = {
        0,  8,  2, 10,
       12,  4, 14,  6,
        3, 11,  1,  9,
       15,  7, 13,  5
    };
    for (int c = 0; c < 16; c++)
    {
        uint8_t r, g, b;
        color_lookup(c, &r, &g, &b);
        /* Rec.601-ish luminance 0..255. Playdate: bit 1 = white. */
        int lum = (int)((54u * r + 183u * g + 19u * b) >> 8);
        for (int k = 0; k < 16; k++)
        {
            int threshold = bayer[k] * 16 + 8; /* 8, 24, ... 248 */
            g_dither[c][k] = (lum > threshold) ? 1 : 0;
        }
    }
    for (int d = 0; d < SCALE_W; d++) g_sx[d] = (uint8_t)(d * SRC_W / SCALE_W);
    for (int d = 0; d < SCALE_H; d++) g_sy[d] = (uint8_t)(d * SRC_H / SCALE_H);
}

static void blit_framebuffer(void)
{
    uint8_t* frame = g_pd->graphics->getFrame();
    const uint8_t* disp = &pico8_ram[DISP_PALETTE];

    for (int dy = 0; dy < SCALE_H; dy++)
    {
        const uint8_t* src  = &pico8_ram[FB_BASE + ((uint16_t)g_sy[dy] << 6)];
        uint8_t*       drow = frame + (DEST_Y + dy) * LCD_ROWSIZE + DEST_BYTE_X;
        int            bayer_row = (dy & 3) << 2;

        for (int bx = 0; bx < DEST_BYTES; bx++)
        {
            uint8_t out = 0;
            int dx0 = bx << 3;
            for (int k = 0; k < 8; k++)
            {
                int     dx  = dx0 + k;
                uint8_t sx  = g_sx[dx];
                uint8_t s   = src[sx >> 1];
                uint8_t nib = (sx & 1) ? (s >> 4) : (s & 0x0F);
                uint8_t col = disp[nib] & 0x0F;                 /* post-display-palette colour */
                uint8_t bit = g_dither[col][bayer_row + (dx & 3)];
                out |= (uint8_t)(bit << (7 - k));
            }
            drow[bx] = out;
        }
    }

    g_pd->graphics->markUpdatedRows(DEST_Y, DEST_Y + SCALE_H - 1);
}

/* Playdate sound source: mono synth -> left buffer. Runs on the audio thread. */
static int audio_cb(void* ctx, int16_t* left, int16_t* right, int len)
{
    (void)ctx; (void)right;
    return audio_render(left, len);
}

static void poll_input(void)
{
    PDButtons cur = 0;
    g_pd->system->getButtonState(&cur, NULL, NULL);

    uint32_t m = 0;
    if (cur & kButtonLeft)  m |= 1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    if (cur & kButtonRight) m |= 1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    if (cur & kButtonUp)    m |= 1u << SDL_GAMEPAD_BUTTON_DPAD_UP;
    if (cur & kButtonDown)  m |= 1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    if (cur & kButtonA)     m |= 1u << SDL_GAMEPAD_BUTTON_EAST;  /* PICO-8 O (primary) */
    if (cur & kButtonB)     m |= 1u << SDL_GAMEPAD_BUTTON_SOUTH; /* PICO-8 X            */
    pd_shim_set_buttons(m);
}

/* Convert a profiler delta (cycles on device, microseconds in the sim) to us. */
static uint32_t to_us(uint32_t delta)
{
    return pd_shim_ticks_are_cycles() ? (delta / 168u) : delta;
}

static int g_log_first = 1; /* reset on each cart (re)boot to re-log frame1 */
static int g_gc_off = 0;    /* GC diagnostic: 1 = Lua collector stopped */

static void boot_cart(int idx)
{
    if (idx < 0 || idx >= NUM_CARTS) return;
    g_booted = 0;
    g_cart_index = idx;
    g_pd->system->logToConsole("open8: booting '%s' (%u bytes)...",
                               g_carts[idx].name, g_carts[idx].len);
    if (core_pd_boot_cart(g_carts[idx].data, (long)g_carts[idx].len))
    {
        g_booted = 1;
        g_log_first = 1;
        core_pd_set_gc(!g_gc_off); /* re-apply: a fresh VM starts with GC on */
        g_pd->system->logToConsole("open8: '%s' booted OK", g_carts[idx].name);
    }
    else
    {
        g_pd->system->logToConsole("open8: '%s' boot FAILED", g_carts[idx].name);
    }
}

static void cart_menu_cb(void* ud)
{
    PDMenuItem* item = (PDMenuItem*)ud;
    boot_cart(g_pd->system->getMenuItemValue(item));
}

#ifdef OPEN8_PROFILE_TOOLS
static void skipfill_menu_cb(void* ud)
{
    PDMenuItem* item = (PDMenuItem*)ud;
    open8_profile_skip_fill = g_pd->system->getMenuItemValue(item);
    g_pd->system->logToConsole("open8: skip_fill = %d", open8_profile_skip_fill);
}
#endif

static void gc_menu_cb(void* ud)
{
    PDMenuItem* item = (PDMenuItem*)ud;
    g_gc_off = g_pd->system->getMenuItemValue(item);
    core_pd_set_gc(!g_gc_off);
    g_pd->system->logToConsole("open8: gc_off = %d", g_gc_off);
}

static int update(void* userdata)
{
    PlaydateAPI* pd = (PlaydateAPI*)userdata;

    if (!g_booted)
    {
        g_pd->graphics->clear(kColorBlack);
        return 1;
    }

    if (g_log_first) pd->system->logToConsole("open8: frame1 begin");

#ifdef OPEN8_PROFILE_LOAD
    uint32_t ui = 0, uc = 0, di = 0, dc = 0; /* per-phase load counts */
#endif
#ifdef OPEN8_PROFILE_API
    open8_api_profile api_update = {0};
    open8_api_profile api_draw = {0};
#endif

    uint32_t t0 = pd_shim_ticks();
    poll_input();
#ifdef OPEN8_PROFILE_LOAD
    open8_vm_instr_count = 0; open8_vm_ccall_count = 0;
#endif
#ifdef OPEN8_PROFILE_API
    open8_profile_api_reset();
#endif
    core_pd_update();
#ifdef OPEN8_PROFILE_API
    api_update = open8_profile_api;
    open8_profile_api_reset();
#endif
#ifdef OPEN8_PROFILE_LOAD
    ui = open8_vm_instr_count; uc = open8_vm_ccall_count;
    open8_vm_instr_count = 0; open8_vm_ccall_count = 0;
#endif
    if (g_log_first) pd->system->logToConsole("open8: frame1 update ok");
    uint32_t t1 = pd_shim_ticks();
    core_pd_draw();
#ifdef OPEN8_PROFILE_API
    api_draw = open8_profile_api;
#endif
#ifdef OPEN8_PROFILE_LOAD
    di = open8_vm_instr_count; dc = open8_vm_ccall_count;
#endif
    if (g_log_first) pd->system->logToConsole("open8: frame1 draw ok");
    uint32_t t2 = pd_shim_ticks();

    pd->graphics->clear(kColorBlack);
    blit_framebuffer();
    if (g_log_first) { pd->system->logToConsole("open8: frame1 blit ok"); g_log_first = 0; }
    uint32_t t3 = pd_shim_ticks();

    uint32_t us_update = to_us(t1 - t0);
    uint32_t us_draw   = to_us(t2 - t1);
    uint32_t us_blit   = to_us(t3 - t2);
    float    fps       = pd->display->getFPS();

    int skip = 0;
#ifdef OPEN8_PROFILE_TOOLS
    skip = open8_profile_skip_fill;
#endif

    /* HUD in the left border (cols 0..135, clear of the 128x128 region). */
    if (g_font)
    {
        char line[64];
        pd->graphics->setFont(g_font);
        int n;
        n = snprintf(line, sizeof(line), "%s%s%s", g_carts[g_cart_index].name,
                     skip ? " [nofill]" : "", g_gc_off ? " [gc-off]" : "");
        pd->graphics->drawText(line, n, kASCIIEncoding, 4, 4);
        n = snprintf(line, sizeof(line), "fps %2d", (int)(fps + 0.5f));
        pd->graphics->drawText(line, n, kASCIIEncoding, 4, 24);
        n = snprintf(line, sizeof(line), "upd %lu", (unsigned long)us_update);
        pd->graphics->drawText(line, n, kASCIIEncoding, 4, 44);
        n = snprintf(line, sizeof(line), "drw %lu", (unsigned long)us_draw);
        pd->graphics->drawText(line, n, kASCIIEncoding, 4, 64);
        n = snprintf(line, sizeof(line), "blt %lu", (unsigned long)us_blit);
        pd->graphics->drawText(line, n, kASCIIEncoding, 4, 84);
    }

    /* Serial trace once per second. */
    static uint32_t last_log_ms = 0;
    uint32_t now_ms = pd->system->getCurrentTimeMilliseconds();
    if (now_ms - last_log_ms >= 1000)
    {
        last_log_ms = now_ms;
#ifdef OPEN8_PROFILE_LOAD
        pd->system->logToConsole("cart=%s nofill=%d gcoff=%d  fps=%d  t_update=%luus  t_draw=%luus  t_blit=%luus  | upd[i=%lu c=%lu] drw[i=%lu c=%lu]",
                                 g_carts[g_cart_index].name, skip, g_gc_off,
                                 (int)(fps + 0.5f),
                                 (unsigned long)us_update,
                                 (unsigned long)us_draw,
                                 (unsigned long)us_blit,
                                 (unsigned long)ui, (unsigned long)uc,
                                 (unsigned long)di, (unsigned long)dc);
#else
        pd->system->logToConsole("cart=%s  fps=%d  t_update=%luus  t_draw=%luus  t_blit=%luus",
                                 g_carts[g_cart_index].name,
                                 (int)(fps + 0.5f),
                                 (unsigned long)us_update,
                                 (unsigned long)us_draw,
                                 (unsigned long)us_blit);
#endif
#ifdef OPEN8_PROFILE_API
        pd->system->logToConsole(
            "api_u tbl[all=%lu items=%lu add=%lu del=%lu shifts=%lu foreach=%lu items=%lu] gfx[all=%lu prim=%lu print=%lu spr=%lu sspr=%lu map=%lu cells=%lu]",
            (unsigned long)api_update.all_calls,
            (unsigned long)api_update.all_items,
            (unsigned long)api_update.add_calls,
            (unsigned long)api_update.del_calls,
            (unsigned long)api_update.del_shifts,
            (unsigned long)api_update.foreach_calls,
            (unsigned long)api_update.foreach_items,
            (unsigned long)api_update.draw_calls,
            (unsigned long)api_update.primitive_calls,
            (unsigned long)api_update.print_calls,
            (unsigned long)api_update.spr_calls,
            (unsigned long)api_update.sspr_calls,
            (unsigned long)api_update.map_calls,
            (unsigned long)api_update.map_cells);
        pd->system->logToConsole(
            "api_d tbl[all=%lu items=%lu add=%lu del=%lu shifts=%lu foreach=%lu items=%lu] gfx[all=%lu prim=%lu print=%lu spr=%lu sspr=%lu map=%lu cells=%lu]",
            (unsigned long)api_draw.all_calls,
            (unsigned long)api_draw.all_items,
            (unsigned long)api_draw.add_calls,
            (unsigned long)api_draw.del_calls,
            (unsigned long)api_draw.del_shifts,
            (unsigned long)api_draw.foreach_calls,
            (unsigned long)api_draw.foreach_items,
            (unsigned long)api_draw.draw_calls,
            (unsigned long)api_draw.primitive_calls,
            (unsigned long)api_draw.print_calls,
            (unsigned long)api_draw.spr_calls,
            (unsigned long)api_draw.sspr_calls,
            (unsigned long)api_draw.map_calls,
            (unsigned long)api_draw.map_cells);
#endif
    }

    return 1;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg)
{
    (void)arg;

    if (event == kEventInit)
    {
        g_pd = pd;
        pd_shim_init(pd);
        init_cart_registry();
        pd->system->logToConsole("open8: [1] init, shim ready");

        build_display_tables();
        pd->system->logToConsole("open8: [2] display tables built (200x200 + dither)");

        const char* err = NULL;
        g_font = pd->graphics->loadFont("/System/Fonts/Asheville-Sans-14-Bold.pft", &err);
        pd->system->logToConsole("open8: [3] font = %p (%s)", (void*)g_font, err ? err : "ok");

        pd->system->logToConsole("open8: [4] core_pd_init...");
        if (!core_pd_init())
        {
            pd->system->logToConsole("open8: core_pd_init() FAILED");
        }
        else
        {
            pd->system->logToConsole("open8: [5] booting first cart...");
            boot_cart(0);
        }

        /* System menu: pick a test cart; diagnostic probes are opt-in builds. */
        PDMenuItem* cart_item =
            pd->system->addOptionsMenuItem("cart", g_cart_titles, NUM_CARTS, cart_menu_cb, NULL);
        pd->system->setMenuItemUserdata(cart_item, cart_item);
#ifdef OPEN8_PROFILE_TOOLS
        PDMenuItem* skip_item =
            pd->system->addCheckmarkMenuItem("no fill", 0, skipfill_menu_cb, NULL);
        pd->system->setMenuItemUserdata(skip_item, skip_item);
#endif
        PDMenuItem* gc_item =
            pd->system->addCheckmarkMenuItem("gc off", 0, gc_menu_cb, NULL);
        pd->system->setMenuItemUserdata(gc_item, gc_item);

        /* PICO-8 audio: a mono sound source pulling from the synth. */
        pd->sound->addSource(audio_cb, NULL, 0);

        pd->display->setRefreshRate(30.0f);
        pd->system->setUpdateCallback(update, pd);
        pd->system->logToConsole("open8: [7] update callback set, entering loop");
    }

    return 0;
}
