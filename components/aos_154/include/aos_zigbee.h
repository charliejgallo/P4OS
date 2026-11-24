/*
 * P4OS - Espressif's Zigbee stack on the P4, with the C6 as its radio
 * (aos_zigbee_p4.c, docs/plan/ZIGBEE.md stage 3). For now a measuring
 * step: a coordinator forming a network of its own on one channel, closed.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { AOS_ZB_OFF = 0, AOS_ZB_STARTING, AOS_ZB_FORMED, AOS_ZB_FAILED };

typedef struct {
    uint8_t  state;             /* AOS_ZB_* */
    uint8_t  channel;
    uint32_t to_c6, from_c6;    /* spinel bytes through the link */
} aos_zigbee_info_t;

/* The C6 becomes the radio (until it restarts) and the stack starts here. */
bool aos_zigbee_start(int channel);
void aos_zigbee_info(aos_zigbee_info_t *out);

#ifdef __cplusplus
}
#endif
