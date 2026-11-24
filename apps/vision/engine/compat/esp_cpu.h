#pragma once
#include <stdint.h>
#include "esp_timer.h"
static inline uint32_t esp_cpu_get_cycle_count(void) { return (uint32_t)(esp_timer_get_time() * 360); }
