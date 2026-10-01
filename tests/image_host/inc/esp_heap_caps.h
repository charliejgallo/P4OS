#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void *heap_caps_malloc(size_t n, int c) { (void)c; return malloc(n); }
static inline void heap_caps_free(void *p) { free(p); }
