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
 * 254), and what is known of those that joined since the start, with what
 * they said (aos_zigbee_devices.c). */
#define AOS_ZB_DEVICES_MAX 32
#define AOS_ZB_CLUSTERS    16           /* the clusters a device serves, kept */
#define AOS_ZB_ATTRS       16           /* the last attributes it said, kept raw */
enum { AOS_ZB_DEV_EPS = 0, AOS_ZB_DEV_BASIC, AOS_ZB_DEV_DONE, AOS_ZB_DEV_FAILED, AOS_ZB_DEV_LEFT,
       AOS_ZB_DEV_REMOVED };       /* removed: not listed, its slot kept until the next start */
/* the clusters the board reads into values (aos_zigbee_dev_t.clusters) */
enum { AOS_ZB_CL_POWER = 1 << 0, AOS_ZB_CL_ONOFF = 1 << 1, AOS_ZB_CL_TEMP = 1 << 2, AOS_ZB_CL_HUM = 1 << 3,
       AOS_ZB_CL_OCC = 1 << 4, AOS_ZB_CL_IAS = 1 << 5, AOS_ZB_CL_LEVEL = 1 << 6 };
typedef struct {
    uint16_t cluster, attr;
    uint8_t  ep, type, len;     /* ZCL type; len of v (a string: its first bytes) */
    uint8_t  v[12];
    uint32_t ms;                /* aos_hal_uptime_ms() when said */
} aos_zigbee_attr_t;
typedef struct {
    uint8_t  ieee[8];           /* little-endian, as on the air */
    uint16_t nwk;
    uint8_t  cap;               /* the announce's capability: bit 1 router, bit 3 on when idle */
    uint8_t  step;              /* AOS_ZB_DEV_*: how far its interview went */
    uint8_t  setup;             /* setup requests still to go, 0 when done */
    uint8_t  ep, eps;           /* the first endpoint, how many */
    uint8_t  neps, eps_list[4]; /* the endpoints (the first four) */
    uint8_t  power;             /* Basic's power source (0x01 mains, 0x03 battery...) */
    int8_t   rssi;
    uint32_t seen_ms;           /* aos_hal_uptime_ms() of its last announce */
    uint32_t heard_ms;          /* and of the last frame from it; 0: no news since the start */
    char     manuf[33], model[33];
    /* the clusters its endpoints serve: ep << 16 | cluster */
    uint8_t  ncl;
    uint32_t cl[AOS_ZB_CLUSTERS];
    /* what the board reads into values; unknown: INT16_MIN, 0xFFFF, 0xFF */
    uint16_t clusters;          /* AOS_ZB_CL_* */
    int16_t  temp;              /* 0.01 degC */
    uint16_t hum;               /* 0.01 % */
    uint8_t  battery;           /* 0.5 % */
    uint8_t  voltage;           /* 0.1 V */
    uint8_t  onoff, onoff_known;        /* a bit per endpoint, eps_list's order */
    uint8_t  onoff_eps;         /* the endpoints with an On/Off cluster, same bits */
    uint8_t  level_ep;          /* the endpoint with Level Control (a dimmer), 0: none */
    uint8_t  level;             /* its CurrentLevel, 0-254; 0xFF unknown */
    uint8_t  occupancy;
    uint16_t zone_type;         /* IAS: 0x0015 contact, 0x000d motion, 0x002a water, ... */
    uint16_t zone_status;       /* IAS: bit 0 alarm (open, motion, water), bit 3 battery low */
    uint8_t  zone_known;
    /* and every attribute it said, raw, the last of each (any device) */
    uint8_t  nattr;
    aos_zigbee_attr_t attr[AOS_ZB_ATTRS];
} aos_zigbee_dev_t;

bool aos_zigbee_permit(int seconds);
/* asks a device to leave the network, and forgets it */
bool aos_zigbee_remove(const uint8_t ieee[8]);
/* an output of a device (its On/Off cluster on endpoint ep): 0 off, 1 on,
 * 2 toggle. Its new state comes back in the device's values. */
bool aos_zigbee_onoff(const uint8_t ieee[8], uint8_t ep, int cmd);
/* a dimmer's level (Level Control, Move to Level with On/Off), 0-254 */
bool aos_zigbee_level(const uint8_t ieee[8], uint8_t ep, int level);
int  aos_zigbee_permit_left(void);
int  aos_zigbee_devices(aos_zigbee_dev_t *out, int max);

#ifdef __cplusplus
}
#endif
