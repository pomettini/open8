#include <stdint.h>

#include "pd_api.h"

#ifdef OPEN8_VM_DTCM_EXEC

#define OPEN8_DTCM_FIRMWARE_FLOOR ((uintptr_t)0x200074d0u)
#define OPEN8_DTCM_POOL_TOP        ((uintptr_t)0x20008a00u)
#define OPEN8_DTCM_GUARD_BYTES     ((uintptr_t)16u)
#define OPEN8_DTCM_ALIGN           ((uintptr_t)32u)

static const uint32_t g_guard_words[4] = {
    0x4f384c4fu, 0x57475541u, 0x52444454u, 0x434d564du
};

static PlaydateAPI* g_pd;
static volatile uint32_t* g_guard_low;
static volatile uint32_t* g_guard_high;
static int g_active;
static int g_fallback_reported;

#if defined(__arm__)
extern uintptr_t open8_vm_hot_start_address(void);
extern uintptr_t open8_vm_hot_end_address(void);
extern uintptr_t open8_vm_execute_source_address(void);
extern uintptr_t open8_vm_gettable_source_address(void);
extern uintptr_t open8_vm_settable_source_address(void);
extern void open8_vm_use_relocated(uintptr_t execute_address,
                                   uintptr_t gettable_address,
                                   uintptr_t settable_address);
extern void open8_vm_use_original(void);

static void write_guard(volatile uint32_t* guard)
{
    unsigned int i;
    for (i = 0; i < 4; ++i)
    {
        guard[i] = g_guard_words[i];
    }
}

static int guard_matches(volatile const uint32_t* guard)
{
    unsigned int i;
    for (i = 0; i < 4; ++i)
    {
        if (guard[i] != g_guard_words[i])
        {
            return 0;
        }
    }
    return 1;
}
#endif

int open8_dtcm_exec_init(PlaydateAPI* pd)
{
#if defined(__arm__)
    const uintptr_t source_start = open8_vm_hot_start_address();
    const uintptr_t source_end = open8_vm_hot_end_address();
    const uintptr_t source_size = source_end - source_start;
    const uintptr_t source_execute = open8_vm_execute_source_address();
    const uintptr_t source_gettable = open8_vm_gettable_source_address();
    const uintptr_t source_settable = open8_vm_settable_source_address();
    const uintptr_t execute_code = source_execute & ~(uintptr_t)1u;
    const uintptr_t gettable_code = source_gettable & ~(uintptr_t)1u;
    const uintptr_t settable_code = source_settable & ~(uintptr_t)1u;
    const uintptr_t destination =
        (OPEN8_DTCM_POOL_TOP - source_size) & ~(OPEN8_DTCM_ALIGN - 1u);
    const uintptr_t destination_execute =
        destination + (execute_code - source_start) +
        (source_execute & (uintptr_t)1u);
    const uintptr_t destination_gettable =
        destination + (gettable_code - source_start) +
        (source_gettable & (uintptr_t)1u);
    const uintptr_t destination_settable =
        destination + (settable_code - source_start) +
        (source_settable & (uintptr_t)1u);
    const uint32_t* src;
    volatile uint32_t* dst;
    uintptr_t words;
    uintptr_t i;
    int copy_ok = 1;

    g_pd = pd;
    g_active = 0;
    g_fallback_reported = 0;

    if (source_end <= source_start ||
        (source_size & (uintptr_t)3u) != 0 ||
        execute_code < source_start || execute_code >= source_end ||
        gettable_code < source_start || gettable_code >= source_end ||
        settable_code < source_start || settable_code >= source_end ||
        destination < OPEN8_DTCM_FIRMWARE_FLOOR + OPEN8_DTCM_GUARD_BYTES)
    {
        pd->system->logToConsole(
            "open8: DTCM VM disabled: invalid layout src=%08lx..%08lx size=%lu dst=%08lx entries=%08lx/%08lx/%08lx",
            (unsigned long)source_start,
            (unsigned long)source_end,
            (unsigned long)source_size,
            (unsigned long)destination,
            (unsigned long)source_execute,
            (unsigned long)source_gettable,
            (unsigned long)source_settable);
        return 0;
    }

    g_guard_low =
        (volatile uint32_t*)(destination - OPEN8_DTCM_GUARD_BYTES);
    g_guard_high = (volatile uint32_t*)OPEN8_DTCM_POOL_TOP;
    write_guard(g_guard_low);
    write_guard(g_guard_high);

    src = (const uint32_t*)source_start;
    dst = (volatile uint32_t*)destination;
    words = source_size / sizeof(uint32_t);
    for (i = 0; i < words; ++i)
    {
        dst[i] = src[i];
    }

    for (i = 0; i < words; ++i)
    {
        if (dst[i] != src[i])
        {
            copy_ok = 0;
            break;
        }
    }

    if (!copy_ok || !guard_matches(g_guard_low) ||
        !guard_matches(g_guard_high))
    {
        pd->system->logToConsole(
            "open8: DTCM VM disabled: copy/guard verification failed word=%lu",
            (unsigned long)i);
        return 0;
    }

    pd->system->clearICache();
    open8_vm_use_relocated(destination_execute,
                           destination_gettable,
                           destination_settable);
    g_active = 1;
    pd->system->logToConsole(
        "open8: DTCM VM+table active src=%08lx..%08lx dst=%08lx..%08lx size=%lu entries=%08lx/%08lx/%08lx guards=%08lx/%08lx",
        (unsigned long)source_start,
        (unsigned long)source_end,
        (unsigned long)destination,
        (unsigned long)(destination + source_size),
        (unsigned long)source_size,
        (unsigned long)destination_execute,
        (unsigned long)destination_gettable,
        (unsigned long)destination_settable,
        (unsigned long)g_guard_low,
        (unsigned long)g_guard_high);
    return 1;
#else
    (void)pd;
    return 0;
#endif
}

void open8_dtcm_exec_disable(void)
{
#if defined(__arm__)
    if (g_active)
    {
        open8_vm_use_original();
        g_active = 0;
    }
#endif
}

int open8_dtcm_exec_check(void)
{
#if defined(__arm__)
    if (g_active &&
        (!guard_matches(g_guard_low) || !guard_matches(g_guard_high)))
    {
        open8_vm_use_original();
        g_active = 0;
        if (!g_fallback_reported)
        {
            g_fallback_reported = 1;
            g_pd->system->logToConsole(
                "open8: DTCM VM guard touched; fell back to source VM low_ok=%d high_ok=%d",
                guard_matches(g_guard_low), guard_matches(g_guard_high));
        }
    }
#endif
    return g_active;
}

#endif
