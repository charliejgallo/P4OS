/*
 * PLANEO - memory (see vu_mem.h)
 */
#include "vu_mem.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN) && !defined(VU_BENCH)
#include "esp_heap_caps.h"
#define VU_BOARD 1
#endif

#define HDR 16          /* keeps the block 16-byte aligned */

static int32_t s_used, s_peak;

void *vu_malloc(size_t n)
{
    size_t total = (n ? n : 1) + HDR;
#ifdef VU_BOARD
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

void *vu_calloc(size_t n)
{
    void *p = vu_malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void vu_free(void *p)
{
    if (!p) return;
    uint8_t *b = (uint8_t *)p - HDR;
    __atomic_sub_fetch(&s_used, (int32_t)*(uint32_t *)b, __ATOMIC_RELAXED);
#ifdef VU_BOARD
    heap_caps_free(b);
#else
    free(b);
#endif
}

void *vu_malloc_fast(size_t n)
{
#ifdef VU_BOARD
    return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
#else
    return malloc(n);
#endif
}

void vu_free_fast(void *p)
{
#ifdef VU_BOARD
    heap_caps_free(p);
#else
    free(p);
#endif
}

size_t vu_mem_used(void)
{
    return (size_t)s_used;
}

size_t vu_mem_peak(void)
{
    return (size_t)s_peak;
}
