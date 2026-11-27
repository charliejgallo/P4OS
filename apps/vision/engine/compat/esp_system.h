#pragma once
#include "esp_err.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
static inline void esp_fill_random(void *buf, size_t len) { unsigned char *b = (unsigned char *)buf; for (size_t i = 0; i < len; i++) b[i] = (unsigned char)rand(); }
