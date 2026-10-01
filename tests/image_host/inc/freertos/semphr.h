#pragma once
typedef void *SemaphoreHandle_t;
typedef int StaticSemaphore_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *b) { return b; }
#define xSemaphoreTake(m, t) ((void)(m), 1)
#define xSemaphoreGive(m) ((void)(m))
