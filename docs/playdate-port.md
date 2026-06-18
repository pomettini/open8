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
This is the same MPU regime as the ITCM write-protection. **Lesson: a Cortex-M7
cycle counter is not freely available to Playdate games the way it is in bare-
metal firmware.** Replaced with `getElapsedTime()` (µs) on both targets. A future
cycle-accurate profiler would need a sanctioned SDK path, not raw registers.

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

### CONCLUSION — the wall is D-cache misses on the Lua heap (the predicted one)

By elimination (blit, dispatch, bytecode volume, GC all ruled out) plus two
positive signals, the bottleneck is **memory latency on the Lua working set**:

- **Per-unit wall-time ≫ cycle cost:** 11–17 µs per bytecode instruction
  (~1800–2900 cycles @168 MHz) where the op itself is ~10–30 cycles. The
  difference is memory stalls.
- **Per-instruction cost scales with heap size** — the smoking gun:
  celeste (114 KB heap) ~17 µs/instr; jelpi (51 KB heap) ~11 µs/instr. Bigger
  working set → worse cache → slower per op. Exactly cache-miss-bound behaviour.
- TValue is 8 B; tables/stack are arrays of 8 B slots with hash-part pointer
  indirection. Working sets of 51–114 KB ≫ the 16 KB D-cache → thrash. The heap
  is in main RAM (DTCM has only ~8 KB free — can't hold it).

This is precisely the bottleneck called out up front. The cheap levers are now
exhausted; remaining options are deep and uncertain:

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

Object *placement* is not the lever. The misses are inherent to the *access
pattern* — the cart's own pointer-chasing over tables/objects — which an
allocator can't change. Instruction/C-call counts identical (the allocator
doesn't change what runs). **Memory levers now exhausted:** dispatch (regressed),
bytecode (cheap), GC (no effect), footprint (packing → predicted Cortex-M
regression), placement (this, null). The wall is the cart's data working set
(51–114 KB) ≫ 16 KB D-cache, in external RAM, accessed by cart-defined pointer
chasing — unchangeable from our side.

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
3. **It targets the wrong layer.** The bottleneck is D-cache misses on the cart's
   *data*, which lives in the same external RAM with the same access pattern no
   matter which interpreter runs it. "Already in memory" concerns interpreter
   *code* — and the load characterizer proved interpreter code/dispatch is the
   cheap part. A faster/resident interpreter optimises what's already fast.

Even build-time transpilation (PICO-8 → native Lua `.pdz`, dodging #1) still hits
#2 (fixed-point) and #3 (same data wall). Not a path.

### Phase 2 verdict

The non-obvious bottleneck called out on day one is confirmed and proven
unchangeable from software: **cart data working set ≫ D-cache, external RAM,
cart-defined access pattern.** Performance envelope: light carts 30 fps; complex
carts (celeste, jelpi) ~6–13 fps. Display path done; measurement harness is the
durable asset. Recommended pivot: correctness/completeness (fix racer's
dget/cartdata, implement audio) over further perf chasing.

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

See [POSTMORTEM.md](POSTMORTEM.md) for the full retrospective.
