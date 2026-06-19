#include <stdint.h>

#ifdef OPEN8_VM_DTCM_WATERMARK

#define OPEN8_DTCM_POOL_TOP       ((uintptr_t)0x20008a00u)
#define OPEN8_DTCM_WATERMARK_WORD ((uint32_t)0xa5a55a5au)
#define OPEN8_DTCM_STACK_RESERVE  ((uintptr_t)512u)

static uintptr_t g_watermark_end = OPEN8_DTCM_POOL_TOP;

void open8_dtcm_watermark_init(void)
{
    uintptr_t sp;
    volatile uint32_t* dst;
    volatile uint32_t* end;

#if defined(__arm__)
    __asm__ volatile ("mov %0, sp" : "=r" (sp));
#else
    return;
#endif

    if (sp <= OPEN8_DTCM_POOL_TOP + OPEN8_DTCM_STACK_RESERVE)
    {
        return;
    }

    g_watermark_end = (sp - OPEN8_DTCM_STACK_RESERVE) & ~(uintptr_t)3u;
    dst = (volatile uint32_t*)OPEN8_DTCM_POOL_TOP;
    end = (volatile uint32_t*)g_watermark_end;
    while (dst < end)
    {
        *dst++ = OPEN8_DTCM_WATERMARK_WORD;
    }
}

uintptr_t open8_dtcm_watermark_low(void)
{
    volatile const uint32_t* src =
        (volatile const uint32_t*)OPEN8_DTCM_POOL_TOP;
    volatile const uint32_t* end =
        (volatile const uint32_t*)g_watermark_end;

    while (src < end && *src == OPEN8_DTCM_WATERMARK_WORD)
    {
        src++;
    }

    return (uintptr_t)src;
}

uintptr_t open8_dtcm_watermark_end(void)
{
    return g_watermark_end;
}

#endif
