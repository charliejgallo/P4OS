/* P4OS - Vision: the heap in the simulator is malloc; the capabilities are ignored. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define MALLOC_CAP_EXEC (1 << 0)
#define MALLOC_CAP_32BIT (1 << 1)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT (1 << 12)
#define MALLOC_CAP_SIMD (1 << 16)
#define MALLOC_CAP_CACHE_ALIGNED (1 << 17)
#ifdef __cplusplus
extern "C" {
#endif
static inline void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n ? n : 1); }
static inline void *heap_caps_calloc(size_t c, size_t n, uint32_t caps) { (void)caps; return calloc(c ? c : 1, n ? n : 1); }
static inline void *heap_caps_realloc(void *p, size_t n, uint32_t caps) { (void)caps; return realloc(p, n); }
static inline void *heap_caps_aligned_alloc(size_t a, size_t n, uint32_t caps)
{
    (void)caps;
    void *p = NULL;
    if (a < sizeof(void *)) a = sizeof(void *);
    return posix_memalign(&p, a, n ? n : 1) == 0 ? p : NULL;
}
static inline void *heap_caps_aligned_calloc(size_t a, size_t c, size_t n, uint32_t caps)
{
    void *p = heap_caps_aligned_alloc(a, c * n, caps);
    if (p) memset(p, 0, c * n);
    return p;
}
static inline void heap_caps_free(void *p) { free(p); }
static inline void heap_caps_aligned_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 16u << 20; }
static inline size_t heap_caps_get_total_size(uint32_t caps) { (void)caps; return 32u << 20; }
static inline size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 16u << 20; }
#ifdef __cplusplus
}
#endif
#define HEAP_IRAM_ATTR
