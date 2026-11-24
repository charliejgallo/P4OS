/* P4OS - Espressif's Zigbee stack on the C6, on request (zigbee_test.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { ZB_OFF = 0, ZB_STARTING, ZB_FORMED, ZB_FAILED };

bool     zigbee_test_start(uint8_t channel);
void     zigbee_test_stop(void);
bool     zigbee_test_running(void);
uint8_t  zigbee_test_state(void);
uint32_t zigbee_test_heap_before(void);
uint32_t zigbee_test_heap_after(void);
