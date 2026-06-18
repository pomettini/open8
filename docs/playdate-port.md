# open8 → Panic Playdate port

Living log for porting open8 (this repo's portable PICO-8 player) to the
Playdate (STM32F746, Cortex-M7 @168 MHz, 400×240 1-bit display).

**This file documents experiments that FAIL as well as those that succeed.**
Every optimization is gated behind a compile flag and judged by the always-on
profiler (see "Measurement"). Do not delete a failed experiment's notes — the
"why it didn't work" is the most valuable part.

---

## Why open8 is the port base

open8 already *is* the player: z8lua VM (fixed-point, `fix32_t = int32_t`,
16.16 — no float in the number type), a PICO-8 graphics API that draws into a
4bpp framebuffer at `pico8_ram[0x6000]`, and a lexaloffle cart loader, all
behind a thin SDL3 seam. So this is a **backend port + M7 optimization**, not a
from-scratch build.

| Component | File(s) | State |
|---|---|---|
| Lua VM | `src/z8lua/` | done, fixed-point |
| Graphics API | `src/api.c` | done, writes 4bpp `0x6000` |
| FB→display blit | `src/memory.c` `update_from_virtual_memory` | SDL-coupled; replaced on Playdate |
| Cart loader / decompress | `src/lexaloffle/`, `src/core.c` | done; PNG decode via vendored `stb_image` (SDL-free) |
| Audio synth | `src/api.c` `pico8_sfx/_music` | **stubbed `TO_BE_DONE`** — green-field |
| Platform seam | `src/main.c`, `src/app.c` | SDL3; replaced by `platform/playdate/` |

## Hardware constraints (do not rediscover)

- STM32F746 Cortex-M7 @168 MHz, 16 KB I-cache + 16 KB D-cache (4-way).
- ITCM is MPU write-protected by the OS → unusable for relocated code. Hot code
  relies on the 16 KB I-cache; keep dispatch + blit inner loops small.
- DTCM 64 KB but only ~8 KB free in practice. Note the framebuffer is exactly
  8 KB — relocating it (or the dither LUTs) into DTCM is a *measured* candidate,
  not an assumption.
- 400×240 1-bit display. PICO-8 is 128×128, 16 colors.
- Prior (Nofrendo): the wall was **D-cache misses on the working set**, not the
  interpreter hot loop. Expect the same shape here.

## Bottleneck hypothesis (to be confirmed by measurement, NOT assumed)

- Framebuffer is 8 KB (`0x6000`); sprite sheet is another 8 KB (`0x0000`). A
  sprite/map blit touches both ≈16 KB ≈ the whole D-cache → eviction pressure
  on fill-heavy carts.
- z8lua `TValue` ≈ 8 B; Lua stack/table/hash traffic over a heap >16 KB →
  D-cache thrash on VM-heavy carts. fix32 multiply is a 1-cycle `SMULL`, so
  *arithmetic* is cheap; dispatch + memory dominate.
- New per-frame cost the SDL build never paid: converting the 8 KB indexed FB
  to 1-bit every frame.

## Measurement (always-on, low overhead, split by component)

Device: DWT cycle counter (`DWT->CYCCNT`, 168 MHz, ~free). Simulator: fall back
to `pd->system->getElapsedTime()` (DWT is meaningless off-device). Splits:
`t_update` (Lua `_update`), `t_draw` (Lua `_draw` + graphics API), `t_blit`
(0x6000 → 1-bit → Playdate frame), later `t_audio`. Surfaced two ways behind
the always-compiled `OPEN8_PROFILE` flag: an on-device HUD (toggle button) in
the 400×240 border, and a once-per-second serial line via `logToConsole`.
Budget: 5.6 M cycles @30 fps, 2.8 M @60 fps.

## Display & audio strategy

- **Scale:** start 1:1 centered (128×128 at x=136,y=56 in 400×240). No integer
  scale fits (2× = 256 > 240). A ~1.875× height-fill scaler is a later flagged
  experiment.
- **16→1 bit:** milestone 1 uses a plain luminance **threshold** (so we can see
  output and measure). Ordered Bayer 4×4 dithering, computed from the
  *post-display-palette* color, is Phase 1/2.
- **Audio:** stubbed today; port FAKE-08's synth later (Phase 4). SFX-only mono
  first, then the music tracker. `t_audio` hook wired from the start.

## Test carts

| Profile | Cart | Rationale |
|---|---|---|
| Light / control | tiny `cls+spr+print` + `7PICROSS` | cheap baseline |
| Fill-rate heavy | `3JELPI` | scrolling map + many sprites (FB↔spritesheet wall) |
| VM heavy | `4RACER` | per-frame pseudo-3D/trig |
| Real-game target | `1CELESTE` | honest "is it playable" |

(Profiles are hypotheses; Phase 1 measurement confirms/reclassifies them.)

---

## Phased plan

- **Phase 0 — bring-up + instrumentation.** Backend boots a cart, runs VM,
  threshold-blits `0x6000`, profiler shows splits. No optimization.
- **Phase 1 — correct measured baseline.** Full API parity, Bayer dither, input;
  record baseline fps/splits per test cart.
- **Phase 2 — attack measured #1 cost (likely blit/fill).** Flags e.g.
  `OPEN8_DITHER_LUT`, `OPEN8_FB_DTCM`. Target: 3JELPI 30 fps.
- **Phase 3 — VM / data-cache**, data-driven. Target: 4RACER + Celeste 30 fps.
- **Phase 4 — audio.** SFX→music, no fps regression.
- **Phase 5 — polish / optional 1.875× scaler.**

---

## Log

### 2026-06-18 — Milestone 1 (Phase 0 start): boot + measure

Goal: smallest provable form — Playdate backend compiles under
`OPEN8_PLATFORM_PLAYDATE`, boots `1CELESTE`, runs the VM, threshold-blits
`0x6000` to screen at 1:1, prints `t_update / t_draw / t_blit / fps` over serial.
Ugly output is fine; dithering and speed are explicitly out of scope.

Architecture decisions for this milestone:

- **SDL shim header** (`platform/playdate/shim/SDL3/SDL.h`): lets `api.c`,
  `core.c`, `memory.c`, `p8scii.c`, `auxiliary.c` compile unchanged. The root
  CMake / SDL build is untouched (everything is behind the flag / a separate
  build dir). Most shimmed symbols are trivial libc wrappers; textures are
  backed by plain malloc buffers; render/audio calls are no-ops.
- **Input via the gamepad shim:** Playdate buttons are exposed through
  `SDL_GetGamepadButton`, so the existing `update_input()` runs unchanged and
  `btn`/`btnp` (incl. the static `btn_held_frames` repeat logic) keep working.
- **Embedded cart:** `1CELESTE.PNG` is compiled in as a byte array and fed to
  `stbi_load_from_memory`. This sidesteps on-device file I/O *and* pdc's
  PNG→.pdi asset conversion. A real cart browser / file I/O is a later phase.
- **Native blit:** read `0x6000`, apply display palette (`0x5f10`), luminance
  threshold → Playdate 1bpp frame, 1:1 centered. open8's
  `update_from_virtual_memory` (SDL screen texture) is **not** used on Playdate.
- **Reuse without duplication:** `core.c` gains three small guarded entry points
  (`core_pd_init`, `core_pd_boot_cart`, `core_pd_frame`) that live alongside the
  existing statics, plus `load_cart` is split so the cart can be loaded from a
  memory buffer instead of only a file.

Build target this milestone: **simulator** (clang dylib) — fastest path to
validate the whole port path. Device (armgcc) bring-up — DWT profiler counter,
verifying the SDK allocator handles the Lua/stb heap — is the immediate
follow-up; the CMake supports both.

Build: `cd platform/playdate && PLAYDATE_SDK_PATH=… cmake -B build -G "Unix
Makefiles" && cmake --build build`. Produces `Source/pdex.dylib` (eventHandler
exported, verified with `nm`) and `open8.pdx` via pdc. The upstream SDL build is
unaffected (verified: `src/core.c` still compiles under the SDL `compile_commands.json`).

Status: **simulator build green and bundled.** Open `open8.pdx` in the Playdate
Simulator (or sideload to device) to record the first baseline fps / split
numbers — fill in the table below once observed.

| cart | fps | t_update (us) | t_draw (us) | t_blit (us) |
|---|---|---|---|---|
| 1CELESTE | _tbd_ | _tbd_ | _tbd_ | _tbd_ |

Immediate follow-ups (not part of milestone 1):
- **Device (armgcc) link:** `load_cart`/`init_core` reference `fopen`/`fread`
  (unused on Playdate — we boot from the embedded array). They compile but may
  pull unresolved newlib syscalls when linking the device ELF; stub or
  `#ifdef`-exclude the file path on device. Simulator links fine (host libc).
- Verify the DWT cycle counter on real hardware (sim uses microseconds).

#### Experiments that failed
_(none yet)_

#### Notes / gotchas discovered
- pdc converts `.png` assets to `.pdi`, which would destroy the steganographic
  cart bytes — another reason the cart is embedded as a C array, not bundled.
- Input is routed through the SDL *gamepad* shim (not keyboard/touch) so the
  existing `update_input()` and its static `btn_held_frames` (btnp repeat) work
  unchanged. Mapping: Playdate A → PICO-8 O, B → X, D-pad → D-pad.
- The SDL screen-texture blit (`update_from_virtual_memory`) is compiled but
  never called on Playdate; `pd_main.c` blits `0x6000` directly. Dead path to
  remove in a later cleanup phase.
