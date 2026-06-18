# open8 on Playdate — Postmortem

A retrospective on porting open8 (a portable PICO-8 player) to the Panic
Playdate, and the performance investigation that followed. Companion to the
running log in [playdate-port.md](playdate-port.md).

## Goal

Run a PICO-8 player on the Playdate (STM32F746 Cortex-M7 @168 MHz, 16 KB I/D
cache, 400×240 1-bit display) at "full native speed for as many carts as
possible," reusing open8's z8lua VM + graphics API rather than writing a player
from scratch.

## Outcome

A working player: boots `.p8.png` carts, runs the z8lua VM, renders to the 1-bit
display, takes input, and now has audio and persistent-data support. **The
performance ceiling is a hard hardware wall** — light carts hold 30 fps; complex
carts (Celeste, Jelpi) sit at ~6–13 fps — and that wall was identified, proven,
and shown to be unmovable from software. The display and input paths are
complete; the lasting engineering asset is an **always-on measurement harness**
that turned every performance question into a single device experiment.

## What shipped

- **Playdate backend** (`platform/playdate/`): an SDL3 compatibility shim lets
  open8's SDL-free core (api.c, core.c, memory.c, z8lua, …) compile unchanged;
  the root SDL build is untouched. Input routes through the gamepad shim so
  `btn`/`btnp` work as-is.
- **1-bit display**: native threshold blit of the 4bpp framebuffer (`0x6000`) to
  the 400×240 frame at 1:1, ~3.7 ms/frame and rock-stable.
- **Embedded test carts** (Celeste/Jelpi/Racer/Picross) switchable via the
  system menu, sidestepping on-device file I/O and pdc's PNG conversion.
- **Profiler**: per-component frame timing (update / draw / blit) + a load
  characterizer (bytecode + C-call counts) on an on-device HUD and serial.
- **Correctness**: `cartdata`/`dget`/`dset` (fixes Racer's nil crash); a
  PICO-8 audio synth (4 channels, standard waveforms, SFX + basic music).

## The investigation — how the bottleneck was found

The hardware brief predicted the wall would be "non-obvious… D-cache misses on
the working data set, not the interpreter hot loop." That turned out to be
exactly right, but it took five experiments to prove it and, crucially, to rule
out the tempting cheap fixes. Each was built behind a compile flag, validated for
correctness, device-measured, and kept or reverted on the number.

| # | Hypothesis | Experiment | Result |
|---|---|---|---|
| 0 | — | Boot + per-component timing | Baseline: draw 46 ms, update 35 ms (spiky), blit 3.7 ms |
| 1 | Fill-rate / blit | skip-fill probe (C blit vs VM split) | **Blit is ~3.7 ms flat; VM dominates.** Not the blitter. |
| 2 | Interpreter dispatch | computed-goto threading | **Regressed** (I-cache bloat). Dispatch isn't the cost. |
| 3 | Bytecode volume | instruction + C-call counters | Counts tiny (~0.5–4 ms of real interpretation). Not volume. |
| 4 | Garbage collector | GC-off toggle | **No change at all.** Not GC. |
| 5 | Heap placement | arena allocator (locality) | **No change.** Not placement. |

### The verdict

By elimination plus two positive signals, the cost is **memory latency on the
cart's data working set**:

- Per-bytecode-instruction wall time is ~1,800–2,900 cycles where the op itself
  is ~10–30 cycles — the rest is stalls.
- **Per-instruction cost scales with heap size** (Celeste's 114 KB heap → ~17
  µs/instr; Jelpi's 51 KB → ~11 µs/instr). The smoking gun for cache-miss-bound
  execution.

The cart's tables/objects (51–114 KB) far exceed the 16 KB D-cache, live in
external RAM, and are accessed by pointer-chasing the *cart* defines — none of
which we can change. DTCM has only ~8 KB free, so the heap can't be relocated to
fast memory, and the MPU/cacheability isn't game-controllable.

## What worked

- **Measure-don't-assume, behind flags.** Every optimization was a falsifiable
  experiment with a revert path. This is what kept a wrong hypothesis (GC) from
  becoming wasted days — it cost one toggle and one serial read.
- **Host differential testing for risky VM changes.** Before trusting the
  computed-goto rewrite and the custom allocator on hardware, we compiled z8lua
  standalone and diffed output against the stock build under heavy churn.
  Byte-identical → correctness settled before the device round-trip.
- **The SDL shim** kept the upstream SDL build and ~10 existing platforms intact
  while the Playdate backend reused the exact same core.
- **The display strategy** was right on the first try and never needed revisiting
  (blit stayed ~3.7 ms through every experiment).

## What didn't (and why it was still valuable)

- **Computed-goto VM dispatch — regressed.** Token threading bloats the
  interpreter past the I-cache; on Cortex-M that penalty isn't offset because
  dispatch wasn't the bottleneck. Lesson: the same architecture that punished
  this would punish a packed-TValue (unaligned byte access) — so we *didn't*
  attempt the footprint-shrink surgery that the "deep VM work" instinct
  suggested. A measured null result steered us away from a predictable second
  regression.
- **GC tuning — no effect.** The intuitive culprit (Celeste's 800 ms hitches
  *looked* like collections) was wrong; stopping the collector changed nothing
  and the hitches persisted. Cheap to disprove.
- **Arena allocator — no effect.** Object placement doesn't matter when the
  access *pattern* is the problem.
- **"Use the Playdate's built-in Lua"** (a good question) — blocked three ways:
  no runtime source loading in the C API, wrong (non-fixed-point) semantics and
  syntax, and it targets interpreter *code* when the cost is *data*. It would
  optimize the part that's already fast.

## Lessons

1. **Trust the cache prediction, but prove it by elimination.** "It's probably
   D-cache" was correct on day one, yet the value was in *ruling out* blit,
   dispatch, bytecode, GC, and placement — because each is a plausible cheap win
   someone would otherwise chase.
2. **On Cortex-M, code size and unaligned access are first-class costs.** Two
   classic desktop wins (computed-goto, packed structs) are net losses here.
3. **A null result is a result.** Four of five experiments "failed" and together
   they're the proof of the conclusion.
4. **Build the instrument first.** The profiler/harness paid for itself many
   times over and outlives the port.
5. **Interpreter performance on cache-bound workloads is a data-layout problem,
   not an interpreter problem.** The carts' own data structures set the ceiling.

## Current state & realistic envelope

- Boots and runs PICO-8 carts; display, input, audio, persistent data working.
- 30 fps for light carts; ~6–13 fps for heavy ones (Celeste, Jelpi). This is the
  hardware ceiling, not a missing optimization.
- Audio is a functional core (SFX + basic music); tempo/mix constants and the
  fancier instruments/effects likely want on-device tuning.

## If someone picks this up

- **Accept the envelope and broaden compatibility**: implement remaining stubs
  (`menuitem`, music fades/effects), a real cart browser via `pd->file`, on-disk
  `cartdata` persistence.
- **Audio polish**: tune tempo/pitch/mix on-device; finish the note effects
  (slide/vibrato/arp) and tilted-saw/organ/phaser instruments; double-buffer the
  synth state to remove the game/audio-thread race.
- **Only if you must chase fps**: the sole remaining lever is shrinking the cart
  *data* working set inside z8lua (a parallel-tag / SoA value representation to
  beat the 8 B TValue), which is high-risk surgery against the NaN-trick layout
  with an uncertain payoff that likely still exceeds the 16 KB cache. The honest
  expectation is diminishing returns.

The harness makes all of these measurable. Start by reading
[playdate-port.md](playdate-port.md) — it has every experiment, including the
failures and the reasons.
