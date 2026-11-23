/*
 * TANQUES - memory: everything big goes to PSRAM, and is counted so the log
 * can say what the game holds.
 */
#pragma once

#include <stddef.h>

void  *tq_malloc(size_t n);
void  *tq_calloc(size_t n);
void   tq_free(void *p);
size_t tq_mem_used(void);
size_t tq_mem_peak(void);
