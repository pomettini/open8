/** @file pd_main.c
 *
 *  Playdate backend entry point for open8 (milestone 1 / Phase 0).
 *
 *  Boots an embedded 1CELESTE cart, runs the z8lua VM, threshold-blits the
 *  PICO-8 framebuffer (0x6000) to the 400x240 1-bit display at 1:1, and reports
 *  per-component frame timing (t_update / t_draw / t_blit) via an on-screen HUD
 *  and a once-per-second serial line.
 *
 *  No optimization here by design — this exists to close the measurement loop
 *  on real hardware. See docs/playdate-port.md.
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
#include "pdshim.h"

/* Declared in api.c / lvm.c. Not via their headers: those pull z8lua's lua.h,
 * whose lua_State typedef collides with the Playdate SDK's pd_api_lua.h. */
#ifdef OPEN8_PLATFORM_PLAYDATE
extern int open8_profile_skip_fill;
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

/* PICO-8 screen is 128x128; centre it in the 400x240 frame. 136 and 56 keep the
 * region byte-aligned (136 = 17*8, 128 = 16*8) so each output byte is built from
 * exactly 8 source pixels with no straddle. */
#define SCREEN_W      128
#define SCREEN_H      128
#define DEST_X        136
#define DEST_Y        56
#define DEST_BYTE_X   (DEST_X / 8)   /* 17 */
#define DEST_BYTES    (SCREEN_W / 8) /* 16 */

#define FB_BASE       0x6000
#define DISP_PALETTE  0x5f10

static PlaydateAPI* g_pd  = NULL;
static LCDFont*     g_font = NULL;
static int          g_booted = 0;

/* Final-colour (0..15) -> 1-bit. Keyed by the post-display-palette colour, so it
 * stays valid regardless of pal() remapping. Milestone 1 = plain luminance
 * threshold; ordered dithering is Phase 1. */
static uint8_t g_threshold[16];

static void build_threshold_lut(void)
{
    for (int i = 0; i < 16; i++)
    {
        uint8_t r, g, b;
        color_lookup(i, &r, &g, &b);
        /* Rec.601-ish luminance. Playdate: bit 1 = white, 0 = black. */
        uint32_t lum = (54u * r + 183u * g + 19u * b) >> 8;
        g_threshold[i] = (lum >= 128) ? 1 : 0;
    }
}

static void blit_framebuffer(void)
{
    uint8_t* frame = g_pd->graphics->getFrame();
    const uint8_t* disp = &pico8_ram[DISP_PALETTE];

    for (int py = 0; py < SCREEN_H; py++)
    {
        const uint8_t* src  = &pico8_ram[FB_BASE + (py << 6)];
        uint8_t*       drow = frame + (DEST_Y + py) * LCD_ROWSIZE + DEST_BYTE_X;

        for (int bx = 0; bx < DEST_BYTES; bx++)
        {
            uint8_t out = 0;
            /* 8 horizontal pixels = 4 source bytes (2 px each). */
            for (int k = 0; k < 4; k++)
            {
                uint8_t s  = src[(bx << 2) + k];
                uint8_t c0 = g_threshold[disp[s & 0x0F] & 0x0F]; /* even/left pixel  */
                uint8_t c1 = g_threshold[disp[s >> 4]   & 0x0F]; /* odd/right pixel  */
                out |= (uint8_t)(c0 << (7 - (k << 1)));
                out |= (uint8_t)(c1 << (6 - (k << 1)));
            }
            drow[bx] = out;
        }
    }

    g_pd->graphics->markUpdatedRows(DEST_Y, DEST_Y + SCREEN_H - 1);
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

static void skipfill_menu_cb(void* ud)
{
#ifdef OPEN8_PLATFORM_PLAYDATE
    PDMenuItem* item = (PDMenuItem*)ud;
    open8_profile_skip_fill = g_pd->system->getMenuItemValue(item);
    g_pd->system->logToConsole("open8: skip_fill = %d", open8_profile_skip_fill);
#else
    (void)ud;
#endif
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

    uint32_t ui = 0, uc = 0, di = 0, dc = 0; /* per-phase load counts */

    uint32_t t0 = pd_shim_ticks();
    poll_input();
#ifdef OPEN8_PLATFORM_PLAYDATE
    open8_vm_instr_count = 0; open8_vm_ccall_count = 0;
#endif
    core_pd_update();
#ifdef OPEN8_PLATFORM_PLAYDATE
    ui = open8_vm_instr_count; uc = open8_vm_ccall_count;
    open8_vm_instr_count = 0; open8_vm_ccall_count = 0;
#endif
    if (g_log_first) pd->system->logToConsole("open8: frame1 update ok");
    uint32_t t1 = pd_shim_ticks();
    core_pd_draw();
#ifdef OPEN8_PLATFORM_PLAYDATE
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
#ifdef OPEN8_PLATFORM_PLAYDATE
    skip = open8_profile_skip_fill;
#endif

    /* HUD in the left border (cols 0..135, clear of the 128x128 region). */
    if (g_font)
    {
        char line[64];
        pd->graphics->setFont(g_font);
        int n;
        n = snprintf(line, sizeof(line), "%s%s", g_carts[g_cart_index].name, skip ? " [nofill]" : "");
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
        pd->system->logToConsole("cart=%s nofill=%d  fps=%d  t_update=%luus  t_draw=%luus  t_blit=%luus  | upd[i=%lu c=%lu] drw[i=%lu c=%lu]",
                                 g_carts[g_cart_index].name, skip,
                                 (int)(fps + 0.5f),
                                 (unsigned long)us_update,
                                 (unsigned long)us_draw,
                                 (unsigned long)us_blit,
                                 (unsigned long)ui, (unsigned long)uc,
                                 (unsigned long)di, (unsigned long)dc);
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

        build_threshold_lut();
        pd->system->logToConsole("open8: [2] threshold lut built");

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

        /* System menu: pick a test cart, and toggle the t_draw fill probe. */
        PDMenuItem* cart_item =
            pd->system->addOptionsMenuItem("cart", g_cart_titles, NUM_CARTS, cart_menu_cb, NULL);
        pd->system->setMenuItemUserdata(cart_item, cart_item);
        PDMenuItem* skip_item =
            pd->system->addCheckmarkMenuItem("no fill", 0, skipfill_menu_cb, NULL);
        pd->system->setMenuItemUserdata(skip_item, skip_item);

        pd->display->setRefreshRate(30.0f);
        pd->system->setUpdateCallback(update, pd);
        pd->system->logToConsole("open8: [7] update callback set, entering loop");
    }

    return 0;
}
