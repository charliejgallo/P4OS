/*
 * TANQUES - memory (see tq_mem.h)
 */
#include "tq_mem.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN) && !defined(TQ_BENCH)
#include "esp_heap_caps.h"
#endif

#define HDR 16          /* keeps the block 16-byte aligned */

static int32_t s_used, s_peak;

void *tq_malloc(size_t n)
{
    size_t total = (n ? n : 1) + HDR;
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN) && !defined(TQ_BENCH)
    uint8_t *p = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    uint8_t *p = (uint8_t *)malloc(total);
#endif
    if (!p) return NULL;
    *(uint32_t *)p = (uint32_t)n;
    int32_t u = __atomic_add_fetch(&s_used, (int32_t)n, __ATOMIC_RELAXED);
    if (u > s_peak) s_peak = u;
    return p + HDR;
}

void *tq_calloc(size_t n)
{
    void *p = tq_malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void tq_free(void *p)
{
    if (!p) return;
    uint8_t *b = (uint8_t *)p - HDR;
    __atomic_sub_fetch(&s_used, (int32_t)*(uint32_t *)b, __ATOMIC_RELAXED);
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN) && !defined(TQ_BENCH)
    heap_caps_free(b);
#else
    free(b);
#endif
}

size_t tq_mem_used(void)
{
    return (size_t)s_used;
}

size_t tq_mem_peak(void)
{
    return (size_t)s_peak;
}
