/** @file arena_alloc.h
 *
 *  Locality-improving Lua allocator (Phase 2 experiment, OPEN8_ARENA_ALLOC).
 *
 *  Small Lua objects (the numerous, scattered, cache-relevant ones) are served
 *  from one contiguous region via size-classed free lists, so same-size objects
 *  cluster and reused slots stay cache-warm. Large blocks (stack, big arrays)
 *  fall through to the system allocator. Targets the proven cause (D-cache
 *  misses on the heap working set) without the unaligned-access penalty a packed
 *  TValue would incur on Cortex-M7. See docs/playdate-port.md.
 *
 *  No platform deps: the backing block comes from malloc (which the Playdate SDK
 *  routes to pd->system->realloc on device), so this also builds/host-tests
 *  standalone.
 *
 *  SPDX-License-Identifier: MIT
 **/
#ifndef OPEN8_ARENA_ALLOC_H
#define OPEN8_ARENA_ALLOC_H

#include <stddef.h>

/* Reset the arena (bump pointer + free lists). Call before each lua_newstate;
 * the previous VM must already be closed. Lazily allocates the backing block. */
void arena_reset(void);

/* lua_Alloc-compatible allocator. */
void* arena_alloc(void* ud, void* ptr, size_t osize, size_t nsize);

#endif /* OPEN8_ARENA_ALLOC_H */
