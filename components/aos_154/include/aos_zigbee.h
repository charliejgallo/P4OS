/*
 * P4OS - Espressif's Zigbee stack on the P4, with the C6 as its radio
 * (aos_zigbee_p4.c, docs/plan/ZIGBEE.md stage 3): a coordinator with a
 * network of its own on one channel, closed until asked to let devices in
 * (aos_zigbee_devices.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { AOS_ZB_OFF = 0, AOS_ZB_STARTING, AOS_ZB_FORMED, AOS_ZB_FAILED };

typedef struct {
    uint8_t  state;             /* AOS_ZB_*: the board's own network (OFF while emulating) */
    uint8_t  channel;
    uint32_t to_c6, from_c6;    /* spinel bytes through the link */
    bool     emu;               /* the stack is emulating a device instead: aos_zigbee_emu_info */
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
       AOS_ZB_CL_OCC = 1 << 4, AOS_ZB_CL_IAS = 1 << 5, AOS_ZB_CL_LEVEL = 1 << 6,
       AOS_ZB_CL_TUYA = 1 << 7,     /* Tuya's 0xEF00: datapoints */
       AOS_ZB_CL_IR = 1 << 8 };     /* Tuya's IR blaster (0xED00 + 0xE004, "Zosung") */
#define AOS_ZB_DPS 12               /* Tuya datapoints kept per device */

/* One Tuya datapoint, the last value it reported (0xEF00) */
typedef struct {
    uint8_t  dp, type, len;         /* type: 0 raw, 1 bool, 2 value, 3 string, 4 enum, 5 bitmap */
    int32_t  v;                     /* bool, value, enum, bitmap */
    uint8_t  raw[8];                /* raw and string: the first bytes */
    uint32_t ms;
} aos_zigbee_dp_t;
/* the controls a device can have, each on one endpoint (aos_zigbee_dev_t.ctl_ep) */
enum { AOS_ZB_CTL_COVER = 0, AOS_ZB_CTL_THERM, AOS_ZB_CTL_COLOR, AOS_ZB_CTL_LOCK, AOS_ZB_CTL_FAN, AOS_ZB_CTL_N };
#define AOS_ZB_MEAS 12              /* the measured values kept per device */

/* One value the board knows how to read (aos_zigbee_fmt.c's table): the
 * raw value as it came (integers sign-extended to 32 bits, a float as its
 * bits). */
typedef struct {
    uint16_t cluster, attr;
    uint8_t  ep, type;
    int32_t  v;
} aos_zigbee_meas_t;
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
    char     name[33];          /* the user's for it ("" none): aos_zigbee_set_name */
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
    /* the rest of what it measures (aos_zigbee_meas_text names it), and
     * the endpoint of each control it has (0: none) */
    uint8_t  nmeas;
    aos_zigbee_meas_t meas[AOS_ZB_MEAS];
    uint8_t  ctl_ep[AOS_ZB_CTL_N];
    /* the last thing it did itself: a button, a remote's command */
    char     event[40];
    uint32_t event_ms;
    /* Tuya's datapoints */
    uint8_t  ndp;
    aos_zigbee_dp_t dps[AOS_ZB_DPS];
    /* and every attribute it said, raw, the last of each (any device) */
    uint8_t  nattr;
    aos_zigbee_attr_t attr[AOS_ZB_ATTRS];
} aos_zigbee_dev_t;

bool aos_zigbee_permit(int seconds);
/* the same, open on the board alone (the routers closed): a device that
 * does not finish joining through a router has to join the board itself */
bool aos_zigbee_permit_board(int seconds);
/* the same with the link key exchange required of whoever joins (a Zigbee
 * 3.0 device that will not finish joining without its own key) */
bool aos_zigbee_permit_strict(int seconds, bool board);
void aos_zigbee_static_kn(bool on);     /* measuring: the next strict opening with the static Request Key alone */
/* asks a device to leave the network, and forgets it */
bool aos_zigbee_remove(const uint8_t ieee[8]);
/* an output of a device (its On/Off cluster on endpoint ep): 0 off, 1 on,
 * 2 toggle. Its new state comes back in the device's values. */
bool aos_zigbee_onoff(const uint8_t ieee[8], uint8_t ep, int cmd);
/* a dimmer's level (Level Control, Move to Level with On/Off), 0-254 */
bool aos_zigbee_level(const uint8_t ieee[8], uint8_t ep, int level);
/* any cluster-specific command (payload up to 14 bytes), and any attribute
 * written (value up to 11 bytes), on one endpoint; then read_attr of the
 * same cluster read back (0xFFFF: nothing) */
bool aos_zigbee_command(const uint8_t ieee[8], uint8_t ep, uint16_t cluster, uint8_t cmd, const void *data, int len,
                        uint16_t read_attr);
bool aos_zigbee_write(const uint8_t ieee[8], uint8_t ep, uint16_t cluster, uint16_t attr, uint8_t type,
                      const void *value, int len);

/* a Tuya datapoint set (0xEF00 dataRequest): type 1 bool, 2 value, 4 enum */
bool aos_zigbee_tuya_dp(const uint8_t ieee[8], uint8_t dp, uint8_t type, int32_t value);

/* The IR blaster (one transfer at a time, any blaster): learning a code
 * from a remote, and sending one. A code is the device's own, in base64
 * (as Zigbee2MQTT shows it: "learned_ir_code"). */
enum { AOS_ZB_IR_IDLE = 0, AOS_ZB_IR_LEARNING, AOS_ZB_IR_RECEIVING, AOS_ZB_IR_LEARNED, AOS_ZB_IR_SENDING, AOS_ZB_IR_SENT,
       AOS_ZB_IR_FAILED };
bool aos_zigbee_ir_learn(const uint8_t ieee[8], bool start);
bool aos_zigbee_ir_send(const uint8_t ieee[8], const char *code_b64);
/* the state, whose it is, and the last code learned (base64, "" if none) */
int  aos_zigbee_ir_state(uint8_t ieee[8], char *code, size_t n);

/* A blaster's code is Tuya's: base64 of FastLZ-packed durations, 16-bit
 * little-endian microseconds, mark first. To durations (how many, -1 if
 * not such a code) and back (the text's length, -1 if it does not fit). */
int  aos_zigbee_ir_decode(const char *b64, uint32_t *out, int max);
int  aos_zigbee_ir_encode(const uint32_t *d, int n, char *b64, size_t size);
/* sending durations (any remote's, SmartIR's library) at a carrier in Hz
 * (0: 38 kHz); as aos_zigbee_ir_send, the progress in aos_zigbee_ir_state */
bool aos_zigbee_ir_send_raw(const uint8_t ieee[8], const uint32_t *d, int n, uint32_t freq);
/* the network's IR blasters: how many, their IEEEs and names, up to max */
int  aos_zigbee_ir_blasters(uint8_t (*ieee)[8], char (*name)[33], int max);

/* Codes kept by name, on the card (zigbee/ir.txt), each with its
 * blaster: the last one learned saved, the list, one sent, one forgotten. */
#define AOS_ZB_IR_NAME 32
typedef struct { uint8_t ieee[8]; char name[AOS_ZB_IR_NAME + 1]; } aos_zigbee_ir_code_t;
bool aos_zigbee_ir_save(const uint8_t ieee[8], const char *name);
int  aos_zigbee_ir_codes(aos_zigbee_ir_code_t *out, int max);
bool aos_zigbee_ir_send_saved(int i);
bool aos_zigbee_ir_forget(int i);

/* What a measured value means, for the screen and the portal
 * (aos_zigbee_fmt.c): whether the board knows (cluster, attr), and the
 * value as text with its unit ("21.5 °C", "430 ppm") and its name
 * ("CO2"). Multipliers and divisors the device gave are applied. */
bool aos_zigbee_meas_known(uint16_t cluster, uint16_t attr);
bool aos_zigbee_meas_hidden(uint16_t cluster, uint16_t attr);    /* a multiplier, a divisor */
bool aos_zigbee_meas_text(const aos_zigbee_dev_t *d, const aos_zigbee_meas_t *m, const char **name, char *val,
                          size_t n);
/* the attributes read from a cluster the table knows, at setup */
int  aos_zigbee_meas_attrs(uint16_t cluster, uint16_t *out, int max);
int  aos_zigbee_permit_left(void);
int  aos_zigbee_devices(aos_zigbee_dev_t *out, int max);
/* Emulating a device (docs/plan/ZIGBEE-EMULAR.md, aos_zigbee_emu.c): the
 * board joins someone else's network (Home Assistant's coordinator) as a
 * router showing a device, instead of forming its own. One network at a
 * time: the board's own stays in the stack's NVS namespace "zigbee", the
 * other one in "zbemu" (aos_zigbee_nvs.c). Switching from one to the other
 * restarts the board (stage 1): the stack does not start twice in one boot.
 * Stage 1 has one kind: an on/off light on endpoint 1, and a temperature
 * and humidity sensor (with a battery) on endpoint 2. */
enum { AOS_ZBE_LIGHT_TEMP = 0, AOS_ZBE_KINDS };
enum { AOS_ZBE_OFF = 0,             /* not emulating */
       AOS_ZBE_SEARCHING,           /* looking for an open network to join */
       AOS_ZBE_JOINED,
       AOS_ZBE_LEFT,                /* it left (or was taken out of) the network */
       AOS_ZBE_FAILED };
enum { AOS_ZBE_V_TEMP = 0,          /* 0.01 degC */
       AOS_ZBE_V_HUM,               /* 0.01 % */
       AOS_ZBE_V_BATTERY,           /* % */
       AOS_ZBE_V_ON,                /* the light: 0 or 1 */
       AOS_ZBE_V_N };

typedef struct {
    uint8_t  state, kind;           /* AOS_ZBE_* */
    bool     saved;                 /* a network to go back to is kept (until it leaves) */
    uint8_t  channel;
    uint16_t pan, nwk;              /* the network's PAN, the board's short address in it */
    uint8_t  xpan[8];
    bool     tclk;                  /* the trust center gave the board its own link key */
    uint16_t tries;                 /* joining attempts so far */
    uint16_t identify_s;            /* > 0: asked to identify itself, seconds left */
    int32_t  v[AOS_ZBE_V_N];
    uint32_t joined_ms;
} aos_zigbee_emu_t;

/* one thing the coordinator asked of the board (or the board did), in words */
typedef struct {
    uint32_t ms;
    uint8_t  out;                   /* 0 asked of the board, 1 done by it */
    char     text[96];
} aos_zigbee_emu_log_t;

/* Join as kind (any network open for joining). The board's own network
 * running: false and *restart set; with restart_ok, the board restarts
 * into emulating instead (and returns only if it could not). */
bool aos_zigbee_emu_join(int kind, bool restart_ok, bool *restart);
bool aos_zigbee_emu_set(int value, int32_t v);  /* AOS_ZBE_V_*: the attribute set, reported as configured */
bool aos_zigbee_emu_report_now(void);           /* every value sent to the coordinator now */
bool aos_zigbee_emu_leave(void);                /* leaves the network (it is told), forgets it, back home */
bool aos_zigbee_emu_home(void);                 /* back to the board's own network, the other one kept */
void aos_zigbee_emu_info(aos_zigbee_emu_t *out);
/* the log from entry 'from' on (counted since the start); returns how many,
 * *next the count to ask from next time */
int  aos_zigbee_emu_log(aos_zigbee_emu_log_t *out, uint32_t from, int max, uint32_t *next);
/* at boot (main.c): the stack started on its own when a switch asked for it */
void aos_zigbee_boot(void);

/* What a Tuya device is and what its datapoints mean, from the card's
 * zigbee/tuya.txt (tools/zigbee/gen_tuya.py, Zigbee2MQTT's definitions;
 * aos_zigbee_tuya.c). false: not in it, or no file. A datapoint's name
 * and its value as text ("temperatura", "21.5 °C"); is_bool: one a tap
 * flips. */
bool aos_zigbee_ident(const aos_zigbee_dev_t *d, char *vendor, size_t vn, char *model, size_t mn, char *desc, size_t dn);
bool aos_zigbee_dp_text(const aos_zigbee_dev_t *d, const aos_zigbee_dp_t *q, char *name, size_t nn, char *val, size_t vn,
                        bool *is_bool);
/* the user's name for a device, kept with it ("" takes it away) */
bool aos_zigbee_set_name(const uint8_t ieee[8], const char *name);

#ifdef __cplusplus
}
#endif
