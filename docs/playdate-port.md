# open8 → Panic Playdate port

Living log for porting open8 (this repo's portable PICO-8 player) to the
Playdate (STM32F746, Cortex-M7 @168 MHz, 400×240 1-bit display).

**This file documents experiments that FAIL as well as those that succeed.**
Risky or intrusive optimizations are gated behind compile flags and judged by
the measurement harness (see "Measurement"). Do not delete a failed
experiment's notes — the "why it didn't work" is the most valuable part.

> **Major correction, 2026-06-18:** every device performance result through
> Phase 2 experiment #4 was collected from a CMake build with an empty
> `CMAKE_BUILD_TYPE`, therefore with **no `-O` optimization flag**. Those
> measurements remain useful as experiment history, but they do not establish a
> hardware ceiling or prove a cache-bound bottleneck. The corrected Release
> build raises Celeste gameplay from 10–13 fps to 17–19 fps and reduces the
> final display blit from ~3.7 ms to ~1.2 ms. Release is now the default.

---

## Current project status

### Goal

Run PICO-8 carts on the Playdate's STM32F746 Cortex-M7 at native speed where
possible, while preserving PICO-8 fixed-point semantics and reusing open8's
z8lua VM and graphics API.

### Current outcome

The port boots embedded `.p8.png` carts, runs z8lua, renders to the Playdate's
1-bit display, handles input, synthesizes audio, and supports in-memory
`cartdata`/`dget`/`dset`.

The earlier 6–13 fps “hardware ceiling” was an invalid conclusion from an
unoptimized device build. Correcting the build raised Celeste gameplay to
17–19 fps, and the later mutation-safe `foreach()` stack snapshot reached about
20–21 fps before the full-height scaler was enabled. The project remains an
active optimization effort toward 30 fps.

Current implementation:

- Playdate backend and SDL compatibility shim under `platform/playdate/`;
- four embedded test carts: Celeste, Jelpi, Racer, and Picross;
- centered 240×240 nearest-neighbor output with Bayer 4×4 dithering;
- Playdate input and a platform-independent PICO-8 audio synth;
- on-device update/draw/blit timing plus opt-in profiling controls;
- packed two-pixel sprite processing for common aligned, unflipped draws;
- mutation-safe `all()` and `foreach()` snapshots optimized for lower traffic;
- compact 4 KiB 8→15 scaler LUT, validated on device at ~1.94 ms;
- guarded runtime relocation of the compact Lua interpreter core into executable
  DTCM, validated on device without canary fallback.

Current bounded costs from the latest valid captures:

- DTCM VM+table Celeste update: **15.65 ms median** in stable gameplay;
- DTCM VM+table Celeste full draw: **13.16 ms median**;
- Celeste no-fill draw: roughly 14.1 ms with the packed sprite path;
- original 240×240 per-pixel scaler: 10.60 ms;
- first 32 KiB scaler LUT: 23.3–23.9 ms, rejected;
- compact 4 KiB scaler LUT: **~2 ms median**, accepted;
- DTCM VM+table per-sample summed frame: **30.97 ms median / 28 fps**;
- 70% of captured gameplay samples fit the 33.3 ms frame budget;
- 30 fps frame budget: 33.3 ms.

### Corrected production configuration

The Playdate build defaults single-config generators to Release. Expensive
diagnostics are opt-in:

- `OPEN8_PROFILE_LOAD=OFF`: no writes on every opcode/C call;
- `OPEN8_PROFILE_TOOLS=OFF`: no skip-fill branch in production blitters;
- `OPEN8_PROFILE_API=OFF`: no coarse API counter writes;
- `OPEN8_ARENA_ALLOCATOR=OFF`: no experimental 4 MB arena;
- `OPEN8_VM_GOTO=OFF`: compiler-generated switch dispatch remains faster.

Static ARM comparison that exposed the invalid baseline:

| property | unoptimized baseline | corrected Release |
|---|---:|---:|
| ELF text | 237,776 B | 186,240 B |
| `luaV_execute` | 8,052 B | 5,548 B |
| emitted `fix32_*` helpers | 90 | 0 |

The Playdate SDK appends `-O2`, making it the effective optimization level.

### Conclusions that remain valid

- Raw DWT register access faults because Playdate game code is unprivileged.
- Raw ITCM is not writable, but code relocation into executable DTCM has now
  been proven on this exact device and SDK by the vecx port. It is a constrained
  experimental path, not a general-purpose code segment.
- Playdate's built-in Lua cannot load arbitrary cart source at runtime and does
  not implement PICO-8 syntax or 16.16 fixed-point semantics.
- Computed-goto dispatch regresses under both the historical `-O0` build and the
  corrected Release build; it is retired.
- GC-off does not improve steady timing and does not remove the large Celeste
  workload spikes.
- The arena allocator produced no useful result in the historical build and is
  not a current target.
- The `foreach()` stack snapshot is a normal-frame win, but does not remove the
  cart's occasional 258-call workload bursts.
- Failed experiments remain documented because their device behavior constrains
  future work—especially the Cortex-M7's sensitivity to large lookup tables.

### Working rules

- Device output is supplied manually by the tester. Never read or attach to the
  Playdate serial console from the development host.
- Deployment ends after copying and verifying the `.pdx`. Never invoke
  `pdutil run` or otherwise launch the game automatically.
- Validate semantic changes with the host suite before device deployment.
- Keep experimental optimizations behind flags or isolate them with a measured
  A/B whenever practical.

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
| Audio synth | `src/audio.c`, `src/api.c` | working core: SFX + basic music |
| Platform seam | `src/main.c`, `src/app.c` | SDL3; replaced by `platform/playdate/` |

## Hardware constraints (do not rediscover)

- STM32F746 Cortex-M7 @168 MHz, 16 KB I-cache + 16 KB D-cache (4-way).
- Raw ITCM is MPU write-protected. Executable code can nevertheless be copied
  into a carefully chosen unused DTCM/stack gap at runtime. On this exact device
  and SDK, vecx measured a contiguous safe window of roughly 5 KB; placement is
  firmware-, stack-, and project-specific and requires canaries/probes.
- DTCM is 64 KB but mostly occupied by firmware state and the game stack. Do
  not treat it as a normal allocator or move cache-resident data there blindly.
- 400×240 1-bit display. PICO-8 is 128×128, 16 colors.
- Prior (Nofrendo): D-cache misses were the wall there. Treat that only as a
  hypothesis here; the first open8 investigation accidentally benchmarked `-O0`.

## Bottleneck hypothesis (to be confirmed by measurement, NOT assumed)

- Framebuffer is 8 KB (`0x6000`); sprite sheet is another 8 KB (`0x0000`). A
  sprite/map blit touches both ≈16 KB ≈ the whole D-cache → eviction pressure
  on fill-heavy carts.
- z8lua `TValue` ≈ 8 B; Lua stack/table/hash traffic over a heap >16 KB →
  D-cache thrash on VM-heavy carts. fix32 multiply is a 1-cycle `SMULL`, so
  *arithmetic* is cheap; dispatch + memory dominate.
- New per-frame cost the SDL build never paid: converting the 8 KB indexed FB
  to 1-bit every frame.

## Measurement

Device and simulator use `pd->system->getElapsedTime()` in microseconds. Raw DWT
access is unavailable to unprivileged Playdate game code and faults on device.
Splits:
`t_update` (Lua `_update`), `t_draw` (Lua `_draw` + graphics API), `t_blit`
(0x6000 → 1-bit → Playdate frame). The HUD and once-per-second serial timing
remain in production. Expensive probes are opt-in:

- `OPEN8_PROFILE_TOOLS=ON`: skip-fill control for splitting VM and pixel fill.
- `OPEN8_PROFILE_LOAD=ON`: per-opcode and per-C-call counters; this deliberately
  perturbs timing and is for counts only.
- `OPEN8_ARENA_ALLOCATOR=ON`: historical allocator experiment.

Budget: 33.3 ms/frame at 30 fps.

## Display & audio strategy

- **Scale:** start 1:1 centered (128×128 at x=136,y=56 in 400×240). No integer
  scale fits (2× = 256 > 240). A ~1.875× height-fill scaler is a later flagged
  experiment.
- **16→1 bit:** milestone 1 uses a plain luminance **threshold** (so we can see
  output and measure). Ordered Bayer 4×4 dithering, computed from the
  *post-display-palette* color, is Phase 1/2.
- **Audio:** a platform-independent PICO-8 synth now provides SFX and basic
  music through a Playdate mono source callback.

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

Current implementation note: Phase 5's scaler is now active. The 128×128
framebuffer is rendered as a centered 240×240 image (1.875×) with Bayer 4×4
dithering. Historical 1:1 and threshold-blit sections below describe the
earlier milestones, not current HEAD.

## Build profiles

The Playdate CMake project defaults single-config generators to `Release`.
Never benchmark a build until the generated `flags.make` contains `-O2` or
better.

Production device build:

```sh
cmake -S platform/playdate -B platform/playdate/build-release \
  -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake"
cmake --build platform/playdate/build-release
```

Corrected skip-fill experiment:

```sh
cmake -S platform/playdate -B platform/playdate/build-profile \
  -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPEN8_PROFILE_TOOLS=ON
cmake --build platform/playdate/build-profile
```

Keep `OPEN8_PROFILE_LOAD=OFF` while timing; its global counter increments occur
for every Lua instruction and C call.

Device logs are captured manually by the tester. Do not attach to or read the
Playdate serial console from the development host; analyze only logs supplied
by the tester.

Device deployment is also manual after copying: push/copy the `.pdx` to the
Playdate, then stop. Never invoke `pdutil run` or otherwise launch the game
automatically; the tester launches it on the device.

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

Status: **DONE — running on device (DVT1, SDK 3.0.6), baseline captured.**
1CELESTE boots and is playable; serial profiler confirmed.

Baseline (device, µs via getElapsedTime; refresh capped at 30):

| cart | phase | fps | t_update | t_draw | t_blit |
|---|---|---|---|---|---|
| 1CELESTE | title/menu | 30 | ~1.5 ms | ~24.5 ms | ~3.7 ms |
| 1CELESTE | gameplay | 10–13 | ~35 ms (erratic, spikes to 50–780 ms) | ~45–50 ms | ~3.7 ms |

#### What the baseline says (Phase 0 findings)

1. **`t_draw` dominates** — ~24 ms on the title, ~45–50 ms in gameplay. This is
   the #1 target. It is `_draw` = Lua draw calls + the graphics API blitting into
   `0x6000`. Celeste's `map()` + per-object `spr()` is exactly the
   spritesheet↔framebuffer fill path the bottleneck hypothesis predicted. Needs a
   finer split (C-blit vs Lua-call overhead) before optimizing — see next.
2. **`t_update` is erratic with huge spikes** (656 µs … 780 ms on one frame) —
   the signature of **Lua GC pauses** / per-frame object allocation in Celeste.
   The 780 ms frame is almost certainly room/level init. Secondary target; GC
   tuning (pause/stepmul, or manual stepping) is a cheap candidate.
3. **`t_blit` is small and rock-stable (~3.7 ms, ±5%)** — the 1-bit conversion is
   **not** the wall. Confirms the 8 KB framebuffer stays cache-resident. Do not
   optimize the blit yet (it is ~11% of the 33 ms budget). Prior refined:
   fill-rate shows up inside `t_draw` (the API blit), not the final 1-bit pass.

Net: at 30 fps the title *just* fits (~29.7 ms); gameplay is ~2.5–3× over budget,
split roughly t_draw : t_update : t_blit ≈ 46 : 35 : 4 ms.

Device bring-up notes (resolved):
- **Device (armgcc) link** pulled unresolved newlib syscalls (`_read`/`_write`/
  `_open`/...) via `fopen`/stb's file path. Fixed with `shim/pd_syscalls.c`
  (no-op stubs, device-only build). The functions are never called — we boot
  from the embedded array.
#### FAILED EXPERIMENT: enabling DWT from game code → instant device crash
First device build poked raw DWT/DEMCR registers in `pd_shim_init`
(`DEMCR |= TRCENA; DWT_CTRL |= CYCCNTENA`). **It crashed immediately on hardware**
(simulator was fine — it never ran that path). Cause: Playdate runs game code
*unprivileged*, and the OS MPU protects the debug/SCS region, so the write faults.
This is the same unprivileged MPU regime that blocks raw ITCM writes and debug
register access. It does not prevent execution from a proven writable DTCM gap.
**Lesson: a Cortex-M7 cycle counter is not freely available to Playdate games
the way it is in bare-metal firmware.** Replaced with `getElapsedTime()` (µs)
on both targets. A future cycle-accurate profiler would need a sanctioned SDK
path, not raw registers.

Next step (Phase 1, data-driven): **split `t_draw`** into time spent inside the
C graphics API (spr/map/rectfill blitting `0x6000`) vs. Lua/VM call overhead, via
a cycle accumulator in api.c. That decides whether Phase 2 targets the blit/
fill path (cache) or the VM. Also: bundle 3JELPI (fill), 4RACER (VM), 7PICROSS
(light) as embedded carts to get the comparison the test matrix wants.

#### Experiments that failed
- **DWT cycle counter from game code** → instant crash (MPU/unprivileged). See
  the device bring-up notes above. Now using `getElapsedTime()`.

#### Notes / gotchas discovered
- pdc converts `.png` assets to `.pdi`, which would destroy the steganographic
  cart bytes — another reason the cart is embedded as a C array, not bundled.
- Input is routed through the SDL *gamepad* shim (not keyboard/touch) so the
  existing `update_input()` and its static `btn_held_frames` (btnp repeat) work
  unchanged. Mapping: Playdate A → PICO-8 O, B → X, D-pad → D-pad.
- The SDL screen-texture blit (`update_from_virtual_memory`) is compiled but
  never called on Playdate; `pd_main.c` blits `0x6000` directly. Dead path to
  remove in a later cleanup phase.

### 2026-06-18 — Phase 1: per-cart `t_draw` split (skip-fill probe)

Added `open8_profile_skip_fill` (api.c): the dominant blitters (cls/spr/sspr/map/
rectfill/circfill/line/pset) early-return, so the VM still issues every draw call
but no pixels are written. `t_draw(full) − t_draw(skip)` = C-side fill cost;
`t_draw(skip)` = VM time spent interpreting `_draw`. Toggled live via a system
menu item; 4 carts switchable via another. Measured on device, gameplay:

| cart | fps | t_update | t_draw full | t_draw VM-only | ⇒ fill | t_blit |
|---|---|---|---|---|---|---|
| celeste | 13 | ~27.5 ms (+GC spikes to 850 ms) | ~43 ms | ~27.5 ms | **~15.5 ms** | 3.6 ms |
| jelpi   | 6  | **~142 ms** | ~22 ms | ~2.8 ms | **~19 ms** | 3.7 ms |
| picross | 13 | ~9.4 ms | ~63 ms | ~52.5 ms* | ~11 ms* | 3.6 ms |
| racer   | — | — | broken | — | — | 3.7 ms |

\* picross under-counts fill: it leans on `print()` (grid numbers), which is not
in the skip set (cursor side-effects), so its print fill lands in the "VM-only"
column.

#### Findings — the prior is overturned

1. **The VM is the wall, not the blitter.** Per frame, VM time (update+draw) is
   celeste ~55 ms, jelpi ~145 ms, picross ~62 ms — i.e. ~75–98% of the frame.
   The C pixel-fill is a *secondary* ~11–19 ms and never dominates any cart.
2. **`t_blit` is ~3.6–3.7 ms and dead flat across all four carts** — definitively
   not worth touching. (Strongest possible confirmation of the Phase 0 read.)
3. **Fill cost is ~constant (~15 ms) regardless of cart** — it tracks screen
   coverage (each fills ~the whole 128×128), not cart complexity. A blitter
   rewrite buys ~15 ms; real but not the headline.
4. **Two distinct VM costs:** steady interpretation (jelpi `_update` 142 ms is a
   5× outlier → a pathological hot path worth bisecting; picross/celeste `_draw`
   tens of ms) **and** GC pauses (celeste `_update` spikes to 850 ms periodically).

#### Phase 2 target (decided by data): the z8lua VM

Blit is out; fill is a later ~15 ms cleanup. First VM experiments, each behind a
flag and device-measured:
- **Interpreter dispatch:** check whether z8lua uses computed-goto / jump-table
  dispatch (`LUAI_USE_*`); enabling it under device GCC is a known ~10–20% win.
- **GC tuning:** kill the celeste `_update` 850 ms spikes via incremental GC
  params (pause/stepmul) or per-frame stepping.
- **Jelpi `_update` 142 ms outlier:** bisect/read its update to find the
  pathological pattern (likely a tight Lua loop or a slow fixed-point/API path);
  a 5× outlier usually hides one fixable hot spot that generalizes.
- Suspected root cause (per Nofrendo prior): D-cache misses on Lua table/object
  data. Confirm indirectly via the above experiments.

#### Correctness bugs found (not perf)
- **racer crashes**: `_draw:1608 arithmetic on nil 'tmp'`. Root cause:
  `cartdata`/`dget` are stubbed (`TO_BE_DONE`) and return nil → used in math.
  Fix: implement `dget`/`dset`/`cartdata` (return 0 / persistent store). Also the
  per-call `Error calling _draw` log spams serial and inflates its own timing —
  rate-limit VM error logging.
- `menuitem`, `music`, `sfx`, `cartdata`, `dget` all log "not yet implemented".

### 2026-06-18 — Phase 2 experiment #1: computed-goto VM dispatch

`OPEN8_VM_GOTO` (lvm.c): replaces the interpreter's `switch(op)` with a
token-threaded computed `goto` — each opcode handler ends with its own
fetch+indirect-jump, so the branch predictor sees many distinct dispatch sites
instead of one. Jump table uses designated initialisers (`[OP_X]=&&L_OP_X`) so a
missing opcode is a compile error, not a silent mis-dispatch; all 53 z8lua
opcodes (incl. PICO-8 PEEK/PEEK2/PEEK4/LSHR/ROTL/ROTR) verified present. SDL
build keeps the `switch` path. Enabled for the Playdate build via CMake.

**Correctness gate (host):** built z8lua standalone both ways and ran a
deterministic opcode workout (numeric/generic for, recursion, calls, array+hash
tables, closures/upvalues, concat+format, comparisons, methods, bitwise, peek) —
**byte-identical output** switch vs goto. So the rewrite is behaviour-preserving.

Expectation / diagnostic value: if dispatch was a real cost we should see
`t_update`/`t_draw` (the VM portions) drop ~10–20%; `t_blit` and the C-fill
delta should be unchanged. **If the win is small, that itself is the finding** —
it confirms the VM is bound by *data*-cache misses (the Nofrendo prior), not
dispatch, and Phase 2 pivots to Lua data layout / GC rather than the interpreter.

Compare against the switch baseline already recorded above:
celeste gameplay t_update ~27.5 ms / t_draw ~43 ms; jelpi t_update ~142 ms;
picross t_draw ~63 ms.

**RESULT — FAILED EXPERIMENT (regressed). Disabled.**

| cart | switch | computed-goto | Δ |
|---|---|---|---|
| celeste `t_update` | ~27.5 ms | ~28.8 ms | +5% |
| celeste `t_draw`   | ~43 ms   | ~46.4 ms | +8% |
| jelpi `t_update`   | ~142 ms  | ~155 ms  | +9% |
| jelpi `t_draw`     | ~22 ms   | ~22.5 ms | ~0 |

Computed-goto made every VM number *worse*. Interpretation: **dispatch is not the
bottleneck — the VM is memory-bound** (the Nofrendo prior, confirmed). Token
threading inlines fetch+dispatch at ~50 sites, bloating `luaV_execute` well past
the 16 KB I-cache; with no dispatch win to offset the added I-cache misses, it's a
net loss. `t_blit` stayed flat ~3.7 ms throughout, as predicted.

This is a *useful* null result: it rules out the interpreter loop and points
Phase 2 at **data/instruction cache and memory traffic**, not the dispatch
mechanism. Code kept behind `OPEN8_VM_GOTO` (disabled in CMake) to reproduce.

#### Reframed Phase 2 targets (post computed-goto)
- The wall is the VM's *memory* behaviour. Two sub-questions to separate
  "carts simply do a lot" from "carts hit a slow per-instruction cost":
  - **Characterise the load:** count bytecode instructions + C-API calls per
    frame (behind the profile flag). If jelpi executes ~5× the bytecode of
    celeste, its 155 ms is algorithmic (heavy cart), not a fixable hot spot; if
    similar bytecode but 5× time, it's cache/per-op cost.
  - **GC:** celeste's `t_update` 900 ms spikes are GC hitches (room init) — tune
    `LUA_GCSETPAUSE/STEPMUL` or step per frame. Smooths spikes; won't move
    jelpi's steady 155 ms.
- Lua object/table memory layout (reduce pointer-chasing / TValue traffic) is
  the deeper, harder lever the data points to — pursue only once the load
  characterisation says per-op cost (not raw volume) is the problem.

### 2026-06-18 — Phase 2 experiment #2: load characterizer (counts/frame)

Per-frame bytecode-instruction and C-call counts (read+reset around update/draw;
counter inflates *timing* in this build — read counts, not times):

| cart | upd instrs | upd C-calls | drw instrs | drw C-calls |
|---|---|---|---|---|
| celeste (title)    | 49     | 3   | 981  | 91  |
| celeste (gameplay) | ~1 621 | ~108 | ~1 625 | ~120 |
| celeste (room load)| 17 409 | 534 | 1 657 | 121 |
| jelpi              | ~13 634 | ~2 103 | 158 | 20 |
| racer (errors)     | 30 | 3 | 9 342 | 445 |

**The decisive finding: bytecode execution is cheap.** Even jelpi's 13.6k
instrs/frame ≈ 4 ms at 50 cyc/op; celeste's 1.6k ≈ 0.5 ms. Yet clean frames are
27 ms / 155 ms. So **~95% of VM time is NOT interpretation** — it's GC + the
C-call/allocator path. This *explains why computed-goto regressed*: there is
almost no dispatch to optimise.

- The 775 ms celeste frame (17k instrs) is a **full GC** on room load.
- Steady celeste = ~0.5 ms bytecode inside ~27 ms → **GC stepping + allocator
  churn dominate**. (Celeste holds 114 KB Lua heap, allocates objects/particles
  every frame.)
- jelpi does ~8× the instrs and ~20× the C-calls of celeste but the same cost
  shape — **algorithmically heavier, not a hot path**. No targeted fix.

#### Phase 2 target (now firmly indicated): GC + allocator, not the interpreter

The allocator is `pd->system->realloc` (a general-purpose heap); GC alloc/free
churn likely pays that cost thousands of times/frame.

Next experiment — **GC-off diagnostic** (cheap, decisive): a menu toggle calling
`lua_gc(L, LUA_GCSTOP/RESTART)`. If update time collapses with GC stopped → GC
is the wall → then tune it (generational mode, or incremental pause/stepmul, or
a per-frame step budget; possibly pool allocations to dodge `pdrealloc`). If it
*doesn't* collapse → the cost is the C-call/allocator path itself, investigate
that. Either way the next number is the answer.

### 2026-06-18 — Phase 2 experiment #3: GC-off diagnostic — GC RULED OUT

`gc off` menu toggle (`lua_gc(GCSTOP/RESTART)` via `core_pd_set_gc`). Device:

| cart | t_update gcoff=0 | t_update gcoff=1 |
|---|---|---|
| celeste | ~27.4 ms | **~27.4 ms** (identical) |
| jelpi   | ~149 ms  | **~147 ms** (noise) |

Stopping the collector changed **nothing**, and the 785 ms celeste room-load
spike **persists with GC off** — so even that hitch is not a collection. **GC is
not the bottleneck.** Hypothesis wrong; cheap to find out.

### Historical `-O0` conclusion — D-cache wall (later invalidated)

At the time, elimination plus two apparent signals suggested memory latency on
the Lua working set. This interpretation is retained as experiment history, but
the missing compiler optimization flag invalidates the cycle-cost assumptions:

- **Per-unit wall-time ≫ cycle cost:** 11–17 µs per bytecode instruction
  (~1800–2900 cycles @168 MHz) where the op itself is ~10–30 cycles. The
  difference is memory stalls.
- **Per-instruction cost scales with heap size** — the smoking gun:
  celeste (114 KB heap) ~17 µs/instr; jelpi (51 KB heap) ~11 µs/instr. Bigger
  working set → worse cache → slower per op. Exactly cache-miss-bound behaviour.
- TValue is 8 B; tables/stack are arrays of 8 B slots with hash-part pointer
  indirection. Working sets of 51–114 KB ≫ the 16 KB D-cache → thrash. The heap
  is in main RAM (DTCM has only ~8 KB free — can't hold it).

This was treated as the final diagnosis. It is no longer the current diagnosis:
the same source compiled as Release substantially outperformed this baseline.
The following were the options considered at the time:

1. **Shrink z8lua's memory footprint** (pack TValue toward 4 B; reduce table/
   object overhead) → smaller working set → better cache residency. Deep VM
   surgery, risky, payoff uncertain but directly targets the proven cause.
2. **Accept the envelope:** complex carts (celeste, jelpi) ~6–13 fps; lighter
   carts hit 30. Ship what's playable, document the ceiling.
3. **Faster memory for the hot heap** — not available (DTCM ~8 KB free; can't
   relocate a 50–114 KB heap; MPU/cacheability not game-controllable).

`t_blit` stayed ~3.7 ms flat across every experiment — the display path is done.

### 2026-06-18 — Phase 2 experiment #4: arena allocator — NULL result

`OPEN8_ARENA_ALLOC` (arena_alloc.c): small Lua objects served from one
contiguous region via size-classed free lists (same-size objects cluster, reused
slots stay warm); large blocks (stack/big arrays) fall through to system malloc.
Host-validated byte-identical to the stock allocator through 40 rounds of table
churn + full GCs.

| cart | stock t_update | arena t_update |
|---|---|---|
| celeste | ~27.4 ms | ~26.9 ms (≈ noise) |
| jelpi   | ~149–155 ms | ~152–156 ms (no change) |

The arena did not help this `-O0` binary. That result must be repeated against
Release before drawing conclusions about object placement or cache behavior.

### Q: use the Playdate's built-in Lua interpreter instead of z8lua?

Asked, and worth recording. **No — blocked three ways, and it wouldn't help:**

1. **No runtime source loading.** `pd->lua` exposes only `addFunction`,
   `registerClass`, `callFunction` (by name), and arg marshalling — **no
   `loadstring`/`dostring`/`loadbuffer`**. Playdate Lua is precompiled by pdc to
   `.pdz`; there's no path to compile arbitrary Lua text at runtime from C. PICO-8
   carts arrive as Lua *source* that must be compiled at runtime.
2. **Wrong semantics + syntax.** PICO-8 is 16.16 fixed-point with specific
   overflow/wrap, plus custom syntax (`\`, `+=`, `?`, `!=`, fixed-point literals,
   `& | ^^ << >> >>> <<> >><`, `@ % $` peek ops). z8lua is a *fork* with a custom
   lexer/parser and a fixed-point number type precisely for this. Stock Lua 5.4
   (double/int) would fail to parse most carts and miscompute the rest.
3. **It still targets an incompatible layer.** Even after retracting the cache
   diagnosis, the runtime source-loading and PICO-8 semantic incompatibilities
   remain decisive blockers.

Even build-time transpilation (PICO-8 → native Lua `.pdz`, dodging #1) still
hits #2: language and numeric semantics would need a substantial compatibility
layer. It is not the next performance path.

### Historical Phase 2 verdict — retracted

The “unchangeable hardware wall” verdict was wrong because the baseline binary
was unoptimized. All dispatch, GC, allocator, skip-fill, and load-characterizer
results above must be regarded as `-O0` results until repeated under Release.

### 2026-06-18 — Wrap-up: correctness + audio

- **Racer fixed.** `cartdata`/`dget`/`dset` implemented in-memory over the
  persistent-data region (0x5e00, 64 fixed-point slots). `dget` now returns 0
  for unset slots instead of nil, so the `_draw:1608 arithmetic on nil` crash is
  gone and the per-frame error spam with it. On-disk persistence keyed by the
  cartdata id is not implemented (RAM only).
- **Audio implemented** (`src/audio.c`, platform-independent): PICO-8 synth —
  4 channels, the standard waveforms, pitch/volume, per-SFX speed + internal
  loop, and basic music pattern sequencing. `sfx()`/`music()` in api.c drive it;
  `audio_reset()` runs on cart load. Output: a Playdate `addSource` mono callback
  pulls `audio_render()` at 44.1 kHz. Compiled into both the Playdate and SDL
  builds (SDL output not yet wired — synth runs, device stays silent as before).
  Scope is a working core: tilted-saw/organ/phaser instruments and the
  slide/vibrato/arp note effects are approximated/stubbed, and tempo/mix
  constants may want on-device tuning. The game/audio-thread share of pico8_ram +
  channel state is an accepted minor race for now.

### 2026-06-18 — Critical correction: the device baseline was `-O0`

The generated device cache and flags exposed the missing experiment:

- `CMAKE_BUILD_TYPE:STRING=` was empty.
- `CMAKE_C_FLAGS` was empty.
- The generated device `C_FLAGS` contained Cortex-M7/ABI/debug flags but no
  `-O1`, `-O2`, `-O3`, or `-Os`.

Therefore every performance result above was measured from an unoptimized
binary. This explains the implausible 1,800–2,900 “cycles per bytecode
instruction”: that quotient mixed bytecode, thousands of Lua↔C calls, graphics
work, out-of-line `static inline` helpers, and `-O0` stack traffic. It was not a
measurement of cache-miss latency.

Static comparison, same ARM GCC toolchain:

| property | original empty build type | corrected Release |
|---|---:|---:|
| ELF text | 237,776 B | 186,240 B |
| `luaV_execute` | 8,052 B | 5,548 B |
| emitted `fix32_*` helper functions | 90 | 0 |

The SDK appends `-O2` after CMake's Release flags, so `-O2` is the effective
optimization level. Inlining removes the numerous fixed-point helper calls and
shrinks the VM/API hot code enough to materially improve I-cache behavior.

The corrected production profile also:

- compiles opcode/C-call counters out unless `OPEN8_PROFILE_LOAD=ON`;
- compiles skip-fill branches/menu code out unless `OPEN8_PROFILE_TOOLS=ON`;
- disables the null-result 4 MB arena unless `OPEN8_ARENA_ALLOCATOR=ON`;
- pre-sizes the snapshot table used by `all()` and caches its iterator length;
- fixes the host test target to include the audio implementation.

All host tests pass, including sparse `all()` and mutation-during-iteration
coverage.

#### Corrected device result

Measured on the same Playdate after installing and launching the Release build:

| cart | phase | fps | t_update | t_draw | t_blit |
|---|---|---:|---:|---:|---:|
| Celeste | title/menu | 30 | ~1.2–1.5 ms | ~12–13 ms | ~1.2–1.3 ms |
| Celeste | gameplay | **17–19** | ~14–23 ms | ~26–30 ms | ~1.2 ms |

This is a large real-device win over 10–13 fps gameplay, and it decisively
retracts the “6–13 fps hardware ceiling.” The remaining Celeste frame is roughly
48–52 ms; reaching 30 fps requires removing another ~15–19 ms, about a further
1.5× improvement rather than the previous apparent 2.5–3× gap.

#### Corrected Release skip-fill result

The tester captured a full → no-fill → full sequence during Celeste gameplay.
Obvious transition/outlier frames were excluded. Median values:

| mode | t_update | t_draw | t_blit | measured fps | summed frame |
|---|---:|---:|---:|---:|---:|
| full, first run | 25.7 ms | 28.0 ms | 1.17 ms | 16 | 54.4 ms |
| no-fill | 21.4 ms | **18.3 ms** | 1.27 ms | **23** | **42.0 ms** |
| full, restored | 27.5 ms | 29.3 ms | 1.17 ms | 16 | 57.9 ms |

The graphics fill path is worth approximately **10–11 ms/frame**, a substantial
target, but removing it completely still leaves about 42 ms/frame. Graphics
optimization alone therefore cannot reach the 33.3 ms budget. The next win must
combine a faster graphics path with roughly another 9 ms reduction in VM/API
work.

#### Coarse API counter result

The supplied capture had `skip_fill = 1` enabled before gameplay and did not
switch it off, so these are no-fill measurements. Excluding the three large
transition/update spikes, 26 gameplay samples produced:

| metric | median |
|---|---:|
| update | 29.1 ms |
| draw | 16.7 ms |
| blit | 1.15 ms |
| fps | 20 |
| update `foreach()` | 1 call / 4.5 copied items |
| draw `foreach()` | 6 calls / 56.5 copied items |
| draw graphics calls | 64 total: 52 primitives, 7.5 `spr`, 3 `map` |

`all()` was never called. The initial suspicion was therefore wrong:
Celeste's hot mutation-safe iterator is `foreach()`. Three update spikes of
332–358 ms each coincided exactly with 258 `foreach()` calls copying about
3,077 items. Normal update frames use only one small `foreach()`, while draw
consistently allocates and fills six snapshot tables.

#### Next experiment

Build Release with both coarse controls enabled:

```sh
cmake -S platform/playdate -B platform/playdate/build-profile \
  -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPEN8_PROFILE_TOOLS=ON \
  -DOPEN8_PROFILE_API=ON \
  -DOPEN8_PROFILE_LOAD=OFF \
  -DOPEN8_ARENA_ALLOCATOR=OFF
cmake --build platform/playdate/build-profile
```

`OPEN8_PROFILE_API` records selected counters separately for update and draw:
`all()` calls/items copied, `add`/`del`/shift work, `foreach`, and graphics calls
split into primitives, print, `spr`, `sspr`, and `map`. It adds only coarse API
increments—no per-opcode VM writes.

The next targeted change is a cheaper mutation-safe `foreach()` snapshot,
followed by an A/B device build. Keep the graphics work as the parallel target:
normal draw frames make about 52 primitive calls plus 5–13 sprite calls.

Implemented for the next device build: `foreach()` now keeps its compact
snapshot directly on the current Lua C-call stack. The values remain GC-rooted
while callbacks execute, preserving the previous mutation-safe behavior while
removing one temporary table allocation plus all `rawseti`/`rawgeti` traffic per
call. The host suite includes a callback that deletes and adds source elements
while verifying that the original values are still visited.

#### foreach() snapshot optimization — device A/B result

Production Release with the C-stack `foreach()` snapshot, Celeste gameplay
(medians, title frames excluded):

| metric | Release baseline | + foreach opt | Δ |
|---|---:|---:|---:|
| t_draw  | ~28 ms   | ~22.5 ms | −5.5 ms |
| t_update| ~25.7 ms | ~23 ms   | −2.7 ms |
| t_blit  | ~1.2 ms  | ~1.2 ms  | — |
| frame   | ~54 ms   | ~47 ms   | −7 ms |
| fps     | 16–19    | ~20–21   | +3–4 |

Removing the six per-draw-frame snapshot allocations took ~5.5 ms off draw. In
that A/B capture, the 330–358 ms `foreach`-storm update spikes were absent and
the worst `t_update` was 33.6 ms. A later scaler capture did contain two more
bursts: 306.9 ms and 262.5 ms, each again coinciding with 258 `foreach()` calls
and about 3,072–3,077 items. Therefore the stack snapshot improves each call and
normal frame throughput, but does **not** eliminate the cart's genuine 258-call
burst workload. Remaining gap to 30 fps (33.3 ms): ~14 ms off a ~47 ms
pre-scaler frame, split roughly evenly between update and draw.

Next levers: re-test computed-goto at the corrected Release baseline (its `-O0`
regression premise — interpreter past I-cache — may have flipped now that
`luaV_execute` is 5,548 B), and a faster graphics fill path.

#### Computed-goto re-test at Release — still regresses, RETIRED

Re-ran the threaded-dispatch A/B against the corrected Release + foreach baseline
(`-DOPEN8_VM_GOTO=ON`, now a proper CMake option; host differential test
re-confirmed switch≡goto byte-identical at current lvm.c):

| dispatch | t_update (median) | t_draw | fps |
|---|---:|---:|---:|
| switch (baseline) | ~23 ms   | ~22.5 ms | ~20–21 |
| computed-goto     | ~28.5 ms | ~22 ms   | ~18–19 |

Still ~5 ms worse on update, ~2 fps down (the goto run also had fresh 60/250 ms
spikes). The `-O2` premise flip (luaV_execute 8052→5548 B) did **not** rescue it.
The `-O0` conclusion was directionally correct for the right reason: **dispatch
isn't the bottleneck**, so threading is pure overhead, and the compiler's
`switch`→jump-table beats a hand-rolled computed goto on Cortex-M7. Caveat: the
two captures are different play sessions (object load varies), but the direction
matches both the `-O0` result and the theory. **Retired for good**; flag stays
OFF. The lvm.c code remains behind `OPEN8_VM_GOTO` purely to reproduce.

Standing optimization picture (production = switch + foreach opt, ~20–21 fps,
~47 ms frame): the ~14 ms gap to 30 fps splits ~23 ms update / ~22.5 ms draw.
The graphics fill path (~10 ms of draw, per the skip-fill split) is the next
bounded target; the residual VM/update cost is the genuinely memory-bound part.

#### 240×240 Bayer scaler + packed sprite fast path — first baseline

Current HEAD adds two graphics changes after the measurements above:

- `draw_sprite_n()` processes two aligned, opaque pixels per iteration for
  unflipped sprites at even destination X, covering the common `map()` case;
- the final display pass now scales 128×128 to a centered 240×240 image and
  applies Bayer 4×4 dithering.

The first supplied capture with the scaler enabled did not contain the requested
no-fill phase: there is no `skip_fill` transition, and the system menu instead
switches from Celeste to Jelpi and Racer. The Celeste gameplay portion is still
enough to establish the display-pass cost:

| metric | median |
|---|---:|
| t_update | 26.45 ms |
| t_draw | 20.63 ms |
| t_blit | **10.60 ms** |
| summed frame | 56.03 ms |
| measured fps | 16–17 |

The old centered 128×128 conversion was ~1.2 ms, so scaling and dithering
240×240 destination pixels added roughly **9.4 ms/frame**. This makes the display
pass a bounded, high-value target.

The first 15:8 expansion attempt was a clear regression. Its 32 KiB table used
four `uint16_t[256]` contribution tables for every Bayer X/Y phase. Device
medians from the controlled capture:

| mode | t_update | t_draw | t_blit | summed frame | fps |
|---|---:|---:|---:|---:|---:|
| full, first | 26.77 ms | 20.52 ms | **23.94 ms** | 70.43 ms | 14 |
| no-fill | 28.63 ms | **14.14 ms** | **23.70 ms** | 65.49 ms | 15 |
| full, restored | 28.65 ms | 20.26 ms | **23.34 ms** | 72.22 ms | 14 |

The table more than doubled display time versus the 10.60 ms per-pixel scaler.
The data-dependent 32 KiB working set is a poor fit for the Cortex-M7 cache.
The no-fill split does show that C-side rendering now costs about **6.3 ms**,
down from the earlier 10–11 ms; the packed sprite path is likely contributing,
though the captures are not a strict sprite-only A/B.

The accepted successor uses a compact form of the same 15:8 idea. The four
source bytes in a group all start on the same Bayer phase, so their expanded
four-bit values can share one table and be shifted while composing the result.
This removes redundant shifted copies and reduces the palette-aware LUT from
32 KiB to **4 KiB**, while retaining four table reads per 15 output pixels. It
is rebuilt only when the 16-byte display palette changes, preserving `pal()`.
Benchmark lines always include `nofill=` and `gcoff=`.

#### Compact 4 KiB scaler LUT — device result, ACCEPTED

The supplied Celeste capture ran normal full rendering throughout. Excluding
title/startup and unusual gameplay samples, 55 stable gameplay samples produced:

| metric | median |
|---|---:|
| t_update | 28.76 ms |
| t_draw | 20.01 ms |
| t_blit | **1.94 ms** |
| summed frame | 50.47 ms |
| measured fps | 19 |

Gameplay `t_blit` had a 10th–90th percentile range of roughly 1.84–2.80 ms.
Compared with the original 240×240 per-pixel scaler's 10.60 ms median, the
compact LUT saves **8.66 ms/frame (about 82%)**. It is also about 12× faster
than the rejected 32 KiB table while using one eighth of its storage.

This restores nearly the old 128×128 blit's cost while retaining the 240×240
height-filling image and Bayer dithering. Keep the compact LUT as the production
display path. The remaining normal Celeste frame is now dominated by update
(~29 ms) and draw (~20 ms), not final display conversion.

#### Clean production baseline with compact scaler

All profiling controls and counters were compiled out, and the production ELF
was checked for the absence of their symbols. Stable Celeste gameplay:

| build | update | draw | blit | summed frame | fps |
|---|---:|---:|---:|---:|---:|
| profiling build | 28.76 ms | 20.01 ms | 1.94 ms | 50.47 ms | 19 |
| clean production | **27.44 ms** | **19.92 ms** | 2.17 ms | **49.73 ms** | 19 |

The production build saves only about 0.74 ms/frame overall. Coarse API
instrumentation was therefore not materially distorting the normal-frame
conclusion. Warm title/menu samples remain at 30 fps (~1.19 ms update, ~9.73 ms
draw, ~1.30 ms blit).

The remaining gap to 30 fps is about **16.4 ms/frame**. Final conversion is no
longer a useful target. Graphics fill accounts for roughly 6 ms of draw, while
the residual update plus no-fill draw is still about 41.5 ms.

#### Light-C builtin call fast path — device result, REJECTED

Celeste's earlier load characterization counted roughly 108 C calls in update
and 120 in draw per gameplay frame. PICO-8 API functions are registered with
`lua_pushcfunction`, making them z8lua `LUA_TLCF` light C functions. Every call
currently takes the general `luaD_precall()` path: stack check, `CallInfo`
setup, GC-debt check, hook check, C invocation, and generic result movement.

The A/B candidate added a guarded VM fast path for the common case:

- the callee is a light C function;
- no call hooks are active;
- the stack already has `LUA_MINSTACK` headroom;
- a reusable `CallInfo` is available;
- otherwise fall back to the existing general path.

This attacks overhead shared by update and draw and leaves C closures, yielding,
stack growth, hooks, and error paths unchanged.

Implemented behind `OPEN8_VM_LCF_FAST`: `OP_CALL` and `OP_TAILCALL` try the
shortcut only for `LUA_TLCF` callees when hooks are disabled, GC debt is
non-positive, `LUA_MINSTACK` headroom already exists, and the next `CallInfo`
has already been allocated. Otherwise they fall through to `luaD_precall()`.
The shortcut still creates a complete active call frame and uses
`luaD_poscall()`, so nested Lua calls, stack relocation, errors, and result
movement retain the normal machinery.

Targeted host tests cover multiple results, nested Lua callbacks, forced stack
growth, direct and nested errors through `pcall`, debug call hooks (which force
fallback), and repeated light-C built-ins. The complete host suite passes with
the option enabled, and a test-only counter recorded 2,235 shortcut executions.

The clean device A/B package was built on 2026-06-19 with
`OPEN8_VM_LCF_FAST=ON` and all profiling, arena, and computed-goto options off.
The ARM text section grew by only 128 bytes, and packaged `pdex.bin` grew by
123 bytes. Its startup marker ends in `compact 4K LUT + LCF fast`.

The package was byte-verified after copying to the Playdate, then the device was
safely ejected without launching the game or attaching to its console. Device
capture, supplied manually, confirmed the `LCF fast` marker. For 55 stable
gameplay samples:

| build | update | draw | blit | summed frame | fps |
|---|---:|---:|---:|---:|---:|
| clean production baseline | 27.44 ms | 19.92 ms | 2.17 ms | 49.73 ms | 19 |
| light-C fast path | 29.30 ms | 20.13 ms | 2.13 ms | 51.70 ms | 19 |

Scene variance prevents treating the roughly 2 ms regression as exact, but the
optimization clearly did not produce the required gain. Keep the implementation
behind its default-off flag for reference; do not ship it.

#### Compact VM core — device result, REJECTED in external memory

The Playdate developer thread “Dirty Optimization Secrets” emphasizes that a
smaller hot core can beat more aggressively optimized code because external
memory misses and the small instruction cache dominate. It recommends `-Os`,
contiguous hot code, and 32-byte cache-line alignment.

`OPEN8_VM_COMPACT` applies GCC's `optimize("Os")` only to `luaV_execute` and
aligns that function to 32 bytes, leaving the rest of open8 at Release `-O2`.
In the final linked ARM build, this reduces `luaV_execute` from **5,548 bytes to
3,708 bytes (−33%)** and places it at a 32-byte-aligned address. The full
`lvm.c` object falls from 8,347 bytes to 7,429 bytes, total ELF text falls by
872 bytes, and packaged `pdex.bin` falls by 774 bytes.

The normal `-O2` interpreter was already 32-byte aligned, so this A/B primarily
tested code-size optimization. The manually supplied device capture contained
30 stable gameplay samples:

| build | update | draw | blit | summed frame | fps-equivalent |
|---|---:|---:|---:|---:|---:|
| clean production baseline | 27.44 ms | 19.92 ms | 2.17 ms | 49.73 ms | 20.1 |
| compact VM | 27.93 ms | 21.98 ms | 2.08 ms | 53.73 ms | 18.6 |

The compact build is about **4.0 ms/frame slower**, with the largest median loss
inside draw. Reject `OPEN8_VM_COMPACT` as an external-memory production option.
Keep it default-off because its 3,708-byte interpreter may still be useful as
the source image for a DTCM relocation experiment; vecx likewise observed that
a compact fallback core could regress in slow memory before winning from TCM.

#### Next bounded experiment: DTCM relocation preflight

Do not immediately execute relocated VM code. First build a non-executing
preflight that reports, through logs supplied manually by the tester:

- current shallow-stack frame address during initialization;
- compact VM source start/end and exact byte size;
- proposed DTCM pool bottom/top;
- safety distance from the current frame;
- whether the proposed pool lies inside the vecx-proven device window.

No writes or execution occur in this preflight. Once its addresses are confirmed,
the execution experiment can use the proven vecx method:

- collect the marked input section inside the normal `.text` output section so
  Playdate applies relocations;
- copy with a manual cacheable-source/volatile-destination word loop, never
  `memcpy`;
- place it only in a probed DTCM/stack gap with safety canaries;
- compile relocated outbound calls with `-mlong-calls -fno-lto`;
- flush I-cache and call through a Thumb-bit-adjusted pointer;
- re-check final size, because long-call veneers may exceed the roughly 5 KB
  measured safe window.

Implemented behind `OPEN8_VM_DTCM_PREFLIGHT`. It implies the compact VM build,
compiles `lvm.c` with `-mlong-calls -fno-lto`, and uses a local linker map that
collects `.text.open8_vm_hot` inside the normal `.text` output section. Static
verification shows:

- `luaV_execute`: 3,896 bytes with long calls;
- exact linker-bracketed copy span: **3,904 bytes**;
- source section remains part of `.text`, with relocations in `.rel.text`;
- proposed pool: `0x20007ac0–0x20008a00`;
- distance above vecx's known firmware-data floor `0x200074d0`: 1,520 bytes.

The preflight only reads addresses and tracks the lowest stack pointer observed
at `luaV_execute` entry. It logs `dtcm_preflight vmsp_min=... margin=...` once
per second. Require a comfortably positive margin across ordinary gameplay and
room transitions before enabling any DTCM copy.

#### DTCM address and stack safety — device result, PASSED

The manually supplied capture matched the linked image exactly:

- source: `0x600013c0–0x60002300`, 3,904 bytes;
- proposed pool: `0x20007ac0–0x20008a00`;
- initialization frame: `0x20009b80`;
- lowest observed `luaV_execute` entry stack pointer:
  `0x200098c8`;
- measured entry-to-pool margin: **3,784 bytes**;
- proposed pool bottom remains 1,520 bytes above vecx's measured firmware-data
  floor.

The entry margin stayed constant through sustained gameplay and a large update
spike. An entry-point sample does not include stack consumed by helper functions
after the VM begins executing, so this was followed by a stack watermark pass.

A GCC `-fstack-usage` audit reports 64 bytes for `luaV_execute`; common VM and
graphics helpers are mostly 16–176 bytes. Some string/library paths can exceed
1 KB, so the measured 3.8 KB gap is likely adequate but should be verified
dynamically before placing executable code there.

`OPEN8_VM_DTCM_WATERMARK` is the final non-executing safety pass. After cart
boot, it fills only the unused stack gap above `0x20008a00`, leaving 512 bytes
below the then-current stack pointer. Gameplay naturally overwrites the pattern;
the once-per-second `dtcm_watermark` line reports the true lowest touched
address and remaining margin. It still performs no code relocation or execution.

The manually supplied watermark capture reached a deepest touched address of
`0x20009570`, leaving **2,928 bytes** between the measured stack low-water mark
and the proposed pool top at `0x20008a00`. The result held through sustained
Celeste gameplay and exceptional 283.780 ms and 345.096 ms update spikes.
Earlier stable readings were `0x20009580` (2,944 bytes) and
`0x200095b8` (3,000 bytes). This closes the dynamic stack-safety gate for the
current device, firmware, SDK, and build layout.

#### Guarded executable DTCM VM core — device result, ACCEPTED

The first `OPEN8_VM_DTCM_EXEC` configuration performed the executable-core test.
It kept the original interpreter in external memory and routed public
`luaV_execute()` calls through a function pointer. After the initial cart had
booted, the build:

- copies only the compact, long-call `luaV_execute` implementation with a
  cacheable-source/volatile-destination 32-bit loop;
- verifies every copied word before enabling it;
- writes 16-byte canaries immediately below and above the reserved code area;
- clears the instruction cache and installs a Thumb-adjusted DTCM entry pointer;
- checks both canaries before VM work, after update, and after draw;
- immediately restores the original source interpreter and logs once if either
  canary changes.

The linked ARM image was statically verified before deployment:

- source implementation: `0x000013c0–0x000022e8`, 3,880 bytes;
- exact linker-bracketed copy span: `0x000013c0–0x000022f0`,
  **3,888 bytes**;
- DTCM destination: `0x20007ac0–0x200089f0`;
- low/high canaries: `0x20007ab0` and `0x20008a00`;
- runtime Thumb entry: `0x20007ac1`;
- all 36 interpreter fixups remain in the normal `.rel.text` relocation stream;
- outbound calls use copied literal addresses plus `blx`, so they remain valid
  after moving the core;
- the low canary remains 1,504 bytes above vecx's measured firmware-data floor,
  while the high canary remains 2,928 bytes below the measured stack low-water
  mark.

The startup marker is `compact 4K LUT + DTCM VM`. The manually supplied device
capture confirmed exact activation:

```text
open8: DTCM VM active src=600013c0..600022f0
  dst=20007ac0..200089f0 size=3888 entry=20007ac1
  guards=20007ab0/20008a00
```

No `disabled` or `guard touched` line appeared. The copied source range, DTCM
destination, size, Thumb entry, and canaries all matched the linked image.

Excluding the four title/menu samples, the capture contains 39 Celeste gameplay
samples:

| metric | DTCM VM median | 10th–90th percentile |
|---|---:|---:|
| measured fps | **23** | 21.8–26.0 |
| update | **21.13 ms** | 11.69–27.30 ms |
| draw | **18.04 ms** | 15.32–21.42 ms |
| blit | 2.42 ms | 2.11–3.68 ms |
| per-sample summed frame | **40.90 ms** | 32.16–47.39 ms |

The sum of the three independent component medians is 41.60 ms. Using that
same comparison method as earlier captures:

| build | update | draw | blit | component-median sum |
|---|---:|---:|---:|---:|
| clean production baseline | 27.44 ms | 19.92 ms | 2.17 ms | 49.73 ms |
| compact VM in external memory | 27.93 ms | 21.98 ms | 2.08 ms | 53.73 ms |
| compact VM in DTCM | **21.13 ms** | **18.04 ms** | 2.42 ms | **41.60 ms** |

Against the clean production baseline, executable DTCM saves about
**8.14 ms/frame (16%)** and raises observed gameplay from roughly 19 fps to a
23 fps median. Against the same compact core left in external memory, relocation
saves about **12.13 ms/frame (23%)**. The title/menu phase also falls to roughly
0.60 ms update and 7.5–7.8 ms draw after warm-up.

This is the largest accepted CPU-side optimization in the current port. It also
corrects the earlier interpretation that interpreter instruction fetch was too
small to matter: dispatch style still is not the answer, but placing the compact
dispatch core in zero-wait executable memory is decisively valuable.

The remaining median gap to a 33.3 ms frame is roughly **7.6–8.3 ms**.

#### Guarded DTCM VM plus table helpers — device result, ACCEPTED

The next bounded build extends the copied block with `luaV_gettable` and
`luaV_settable`. They serve five table-related bytecode handlers plus public API
table access. Their public symbols remain small external-memory trampolines
through independent function pointers, so no literal-pool rewriting is needed:
activation installs the two helper entries before the VM entry, while fallback
restores the source VM entry first and then both helpers.

The full host suite passes through the new indirect helper entries. Static ARM
verification shows:

- source block: `0x000013c0–0x00002530`, **4,464 bytes**;
- compact helper bodies:
  - `luaV_gettable`: 268 bytes at `0x000013c0`;
  - `luaV_settable`: 276 bytes at `0x000014e0`;
  - compact VM core: 3,880 bytes at `0x00001600`;
- DTCM copy: `0x20007880–0x200089f0`;
- DTCM entries:
  - VM: `0x20007ac1`;
  - gettable: `0x20007881`;
  - settable: `0x200079a1`;
- low/high canaries: `0x20007870` and `0x20008a00`;
- all 56 block fixups remain in the normal `.rel.text` stream;
- every helper outbound call is a relocated literal load plus `blx`;
- the low canary remains **928 bytes** above the known firmware-data floor;
- the high canary preserves the full **2,928-byte** measured stack margin.

The package marker is `compact 4K LUT + DTCM VM+table`. The manually supplied
capture confirmed exact activation:

```text
open8: DTCM VM+table active src=600013c0..60002530
  dst=20007880..200089f0 size=4464
  entries=20007ac1/20007881/200079a1
  guards=20007870/20008a00
```

No `disabled` or `guard touched` line appeared. Excluding the four title/menu
samples, 40 Celeste gameplay samples produced:

| metric | VM+table DTCM median | 10th–90th percentile |
|---|---:|---:|
| measured fps | **28** | 26–30 |
| update | **15.65 ms** | 9.11–21.38 ms |
| draw | **13.16 ms** | 12.04–15.69 ms |
| blit | **1.97 ms** | 1.86–2.64 ms |
| per-sample summed frame | **30.97 ms** | 27.23–37.91 ms |

The component-median sum is 30.77 ms. Twenty-eight of 40 samples (**70%**) fit
inside the 33.3 ms frame budget. The displayed FPS reaches 30 repeatedly and has
a median of 28, so the project has reached the 30 fps target on median CPU frame
work; the remaining problem is consistency during heavier gameplay frames.

Compared with the accepted core-only DTCM capture:

| build | update | draw | blit | per-sample frame | fps |
|---|---:|---:|---:|---:|---:|
| DTCM VM core | 21.13 ms | 18.04 ms | 2.42 ms | 40.90 ms | 23 |
| DTCM VM + table helpers | **15.65 ms** | **13.16 ms** | **1.97 ms** | **30.97 ms** | **28** |

The helper bundle saves another **9.92 ms/frame (24%)** in this capture,
including about 5.49 ms in update and 4.89 ms in draw. Some scene variance is
unavoidable, but the magnitude, simultaneous update/draw improvement, repeated
30 fps samples, and tester report that it “plays very good” make this a clear
accept rather than noise.

Against the earlier clean production baseline's 49.73 ms component-median sum,
the current 30.77 ms sum is about **18.96 ms/frame faster (38%)**.

Do not grow the DTCM block further without a new lower-memory probe: only
928 bytes remain above the vecx-observed firmware-data floor. The next
performance work should return to the measured C graphics-fill path, aiming to
remove enough of its remaining cost to keep the 90th-percentile frame below
33.3 ms. The target is no longer higher peak FPS; it is stable 30 fps.

This VM+table package was copied to the Playdate, byte-verified, and safely
ejected on 2026-06-19. It was not launched automatically and the host did not
attach to or read the device console.

The earlier core-only executable-DTCM package was likewise copied,
byte-verified, and safely ejected on 2026-06-19 without automatic launch or
host console access.

Do not move the Lua heap or compact scaler LUT to DTCM merely because it is
faster memory: vecx measured a regression when cache-resident hot data gained a
runtime pointer indirection. Code relocation is the supported hypothesis.

The complete host suite passes with `OPEN8_VM_COMPACT=ON`. The clean device
package was built with the marker `compact 4K LUT + compact VM`, with light-C,
profiling, arena, and computed-goto options all off.

Operational note: if `pdutil ... datadisk` returns successfully but no PLAYDATE
volume appears, check whether the Playdate Simulator app owns the USB port and
close it before retrying. Do not open or read the serial stream while checking.

For any later skip-fill capture, use one repeatable Celeste gameplay scene:

1. Run full rendering for 15–20 seconds.
2. Enable `no fill` and run for 15–20 seconds.
3. Disable `no fill` and run the same scene for another 15–20 seconds.
4. Supply the `cart=`, `api_u`, and `api_d` lines manually. Confirm that the
   `cart=` lines show `nofill=0`, then `nofill=1`, then `nofill=0`.

Do not attach to the device console from the development host, and do not launch
the game automatically after deployment.
