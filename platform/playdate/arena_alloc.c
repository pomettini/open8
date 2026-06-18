/** @file arena_alloc.c
 *
 *  See arena_alloc.h. lua_Alloc contract:
 *    ptr==NULL          -> allocate nsize (osize is a type tag, ignore)
 *    nsize==0           -> free ptr (osize = its real size)
 *    otherwise          -> realloc ptr from osize to nsize
 *
 *  SPDX-License-Identifier: MIT
 **/
#include <stdlib.h>
#include <string.h>

#include "arena_alloc.h"

#define ARENA_SIZE  (4u * 1024u * 1024u) /* small-object region; large go to malloc */
#define SMALL_MAX   512                  /* bytes; above this -> system allocator   */
#define ALIGN       16                   /* >= 8 (TValue needs double alignment)    */
#define NCLASS      (SMALL_MAX / ALIGN)   /* 32 size classes                         */

static char*  g_base;
static size_t g_off;
static size_t g_size;
static void*  g_free[NCLASS + 1];        /* free-list head per class (1..NCLASS)    */

static int   class_of(size_t n) { return (int)((n + (ALIGN - 1)) / ALIGN); } /* 1..NCLASS */
static size_t class_bytes(int c) { return (size_t)c * ALIGN; }
static int   is_small(size_t n) { return n != 0 && n <= SMALL_MAX; }

void arena_reset(void)
{
    if (!g_base)
    {
        g_base = (char*)malloc(ARENA_SIZE);
        g_size = g_base ? ARENA_SIZE : 0;
    }
    g_off = 0;
    memset(g_free, 0, sizeof(g_free));
}

static void* small_alloc(size_t n)
{
    int c = class_of(n);
    if (g_free[c])                       /* reuse a recently-freed same-size slot */
    {
        void* p = g_free[c];
        g_free[c] = *(void**)p;
        return p;
    }
    size_t sz = class_bytes(c);
    if (g_base && g_off + sz <= g_size)  /* bump from the contiguous region */
    {
        void* p = g_base + g_off;
        g_off += sz;
        return p;
    }
    return malloc(sz);                   /* arena full: graceful fallback */
}

static void small_free(void* p, size_t osize)
{
    int c = class_of(osize);
    *(void**)p = g_free[c];              /* push onto the class free list */
    g_free[c] = p;
}

void* arena_alloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
    (void)ud;

    if (nsize == 0)                      /* free */
    {
        if (ptr)
        {
            if (is_small(osize)) small_free(ptr, osize);
            else                 free(ptr);
        }
        return NULL;
    }

    if (ptr == NULL)                     /* new allocation */
    {
        return is_small(nsize) ? small_alloc(nsize) : malloc(nsize);
    }

    /* realloc */
    int old_small = is_small(osize);
    int new_small = is_small(nsize);

    if (!old_small && !new_small)
        return realloc(ptr, nsize);      /* both large: native realloc */

    if (old_small && new_small && class_of(osize) == class_of(nsize))
        return ptr;                      /* same size class: no-op */

    void* np = new_small ? small_alloc(nsize) : malloc(nsize);
    if (!np) return NULL;
    memcpy(np, ptr, osize < nsize ? osize : nsize);
    if (old_small) small_free(ptr, osize);
    else           free(ptr);
    return np;
}
