/* P4OS - Vision: the FreeRTOS types esp-dl's headers name, for the simulator (one core, no tasks). */
#pragma once
#include <stdint.h>
#include "esp_heap_caps.h"
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1
#define tskIDLE_PRIORITY 0
#define pdMS_TO_TICKS(ms) (ms)
