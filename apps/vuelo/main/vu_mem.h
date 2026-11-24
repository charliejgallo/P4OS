/*
 * PLANEO - memory: everything big goes to PSRAM, and is counted so the log
 * can say what the game holds.
 */
#pragma once

#include <stddef.h>

void  *vu_malloc(size_t n);
void  *vu_calloc(size_t n);
void   vu_free(void *p);
/* Internal RAM able to do DMA (never the slow LP SRAM); NULL if there is
 * none to spare. Not counted. */
void  *vu_malloc_fast(size_t n);
void   vu_free_fast(void *p);
size_t vu_mem_used(void);
size_t vu_mem_peak(void);
