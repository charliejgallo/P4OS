/*
 * P4OS - Espressif's Zigbee stack on the P4, with the C6 as its radio
 * (aos_zigbee_p4.c, docs/plan/ZIGBEE.md stage 3): a coordinator with a
 * network of its own on one channel, closed until asked to let devices in
 * (aos_zigbee_devices.c).
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

/* Devices joining: the network open for some seconds (0 closes it, at most
 * 254), and what is known of those that joined since the start. */
#define AOS_ZB_DEVICES_MAX 32
enum { AOS_ZB_DEV_EPS = 0, AOS_ZB_DEV_BASIC, AOS_ZB_DEV_DONE, AOS_ZB_DEV_FAILED, AOS_ZB_DEV_LEFT };
typedef struct {
    uint8_t  ieee[8];           /* little-endian, as on the air */
    uint16_t nwk;
    uint8_t  cap;               /* the announce's capability: bit 1 router, bit 3 on when idle */
    uint8_t  step;              /* AOS_ZB_DEV_*: how far its interview went */
    uint8_t  ep, eps;           /* the first endpoint, how many */
    uint8_t  power;             /* Basic's power source (0x01 mains, 0x03 battery...) */
    int8_t   rssi;
    uint32_t seen_ms;           /* aos_hal_uptime_ms() of its last announce */
    char     manuf[33], model[33];
} aos_zigbee_dev_t;

bool aos_zigbee_permit(int seconds);
int  aos_zigbee_permit_left(void);
int  aos_zigbee_devices(aos_zigbee_dev_t *out, int max);

#ifdef __cplusplus
}
#endif
