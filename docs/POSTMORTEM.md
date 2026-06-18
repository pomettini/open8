# open8 on Playdate — Corrected postmortem

A retrospective on porting open8, a portable PICO-8 player, to the Panic
Playdate. The chronological experiment record lives in
[playdate-port.md](playdate-port.md).

## Goal

Run PICO-8 carts on the Playdate's STM32F746 Cortex-M7 at native speed where
possible, while preserving PICO-8's fixed-point semantics and reusing open8's
z8lua VM and graphics API.

## Outcome

The port boots embedded `.p8.png` carts, runs z8lua, renders to the 1-bit
display, handles input, synthesizes audio, and supports in-memory cart data.

The original postmortem incorrectly declared a 6–13 fps hardware ceiling. The
device build had an empty `CMAKE_BUILD_TYPE` and no compiler optimization flag.
After correcting the build and removing production profiling overhead, Celeste
gameplay rose from 10–13 fps to 17–19 fps; its title holds 30 fps.

This is not yet the 30 fps target, but it changes the problem from an alleged
hardware impossibility into an active optimization task with roughly another
1.5× needed for Celeste.

## What shipped

- A Playdate backend and SDL compatibility shim under `platform/playdate/`.
- A native centered 128×128, 4bpp-to-1bpp display path.
- Four embedded test carts: Celeste, Jelpi, Racer, and Picross.
- On-device update/draw/blit timing through the supported elapsed-time API.
- Opt-in skip-fill and VM load-characterization probes.
- Input, basic SFX/music synthesis, and in-memory `cartdata`/`dget`/`dset`.

## The invalid baseline

The documented device command configured a single-config Unix Makefiles build
without `-DCMAKE_BUILD_TYPE=Release`. The generated cache and flags showed:

```text
CMAKE_BUILD_TYPE:STRING=
CMAKE_C_FLAGS:STRING=
C_FLAGS = ... -mcpu=cortex-m7 ...    # no -O option
```

Consequences included:

- ninety out-of-line copies of nominally `static inline` fixed-point helpers;
- excessive stack loads/stores around Lua and C API calls;
- a 31% larger `luaV_execute`;
- inflated graphics loops and table operations;
- misleading comparisons between interpreter work, C calls, and wall time.

The earlier “1,800–2,900 cycles per bytecode instruction” calculation was not a
cache-latency measurement. It divided total phase time—including C calls,
graphics work, allocation and profiler writes—by bytecode count in an `-O0`
binary.

## Corrected production build

The Playdate project now defaults single-config generators to Release and makes
expensive diagnostics opt-in:

- `OPEN8_PROFILE_LOAD=OFF`: no global write on every opcode/C call.
- `OPEN8_PROFILE_TOOLS=OFF`: no skip-fill branch in every major drawing call.
- `OPEN8_ARENA_ALLOCATOR=OFF`: no experimental 4 MB arena.

`all()` also pre-sizes its mutation-safe snapshot and caches the snapshot length
inside the iterator. The complete host suite passes, including mutation during
`all()` iteration.

Static ARM comparison:

| property | unoptimized baseline | corrected Release |
|---|---:|---:|
| ELF text | 237,776 B | 186,240 B |
| `luaV_execute` | 8,052 B | 5,548 B |
| emitted `fix32_*` helpers | 90 | 0 |

The Playdate SDK appends `-O2`, making it the effective optimization level.

## Corrected device measurements

| cart | phase | fps | update | draw | final blit |
|---|---|---:|---:|---:|---:|
| Celeste | title/menu | 30 | ~1.2–1.5 ms | ~12–13 ms | ~1.2–1.3 ms |
| Celeste | gameplay | **17–19** | ~14–23 ms | ~26–30 ms | ~1.2 ms |

The aggregate Release build includes compiler optimization, production probes
being compiled out, the stock allocator, and the `all()` improvement. Their
individual contributions have not yet been isolated, so the gain should not be
attributed to one sub-change without another device experiment.

## What remains valid from the first investigation

- Raw DWT register access faults because Playdate game code is unprivileged.
- The final framebuffer conversion is stable and no longer a leading cost.
- The built-in Playdate Lua runtime cannot load arbitrary cart source and does
  not implement PICO-8's fixed-point language semantics.
- Keeping experiments behind flags and validating VM changes on the host is
  still the right workflow.

## What must be retested

Computed-goto dispatch, GC-off, arena allocation, skip-fill ratios, and the
load-characterizer conclusions were all measured against `-O0`. Their relative
numbers remain historical observations, not reliable architectural conclusions.
In particular, the prior claim that cart-defined D-cache misses formed an
unmovable wall is retracted.

## Next step

Re-run only the coarse fill split against Release:

```sh
cmake -S platform/playdate -B platform/playdate/build-profile \
  -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$PLAYDATE_SDK_PATH/C_API/buildsupport/arm.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPEN8_PROFILE_TOOLS=ON \
  -DOPEN8_PROFILE_LOAD=OFF \
  -DOPEN8_ARENA_ALLOCATOR=OFF
cmake --build platform/playdate/build-profile
```

Measure full and no-fill frames for all four carts:

- If Celeste's no-fill total is at or below 33 ms, graphics is the path to 30
  fps: optimize `map()`/`spr()` and packed framebuffer writes.
- If no-fill remains well above budget, add coarse timers/counters around API
  categories such as table iteration, math, input and drawing. Avoid per-opcode
  writes while collecting timing.
- `all()` is the first API-level suspect because Celeste repeatedly iterates
  object lists and the current mutation-safe semantics still require a snapshot.

Only after this corrected split should computed-goto, GC behavior, or allocator
placement be reconsidered.
