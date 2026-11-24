/*
 * P4OS - what the P4 and the C6 say to each other about the 802.15.4 radio
 * (docs/plan/ZIGBEE.md). The C6's side is c6/main/radio154.c, the P4's
 * components/aos_154/aos_154.c; both include this file, so a change here is
 * a change of both firmwares and of AOS_154_PROTO.
 *
 * They travel on esp_hosted's peer_data channel: a message is an id and up
 * to 8166 bytes. Every number is little-endian.
 *
 * P4 -> C6, AOS_154_MSG_CMD: one byte, the operation, and its arguments.
 *
 *   HELLO                          the C6 answers with STATUS
 *   SCAN  u8 first, u8 last,       each channel from first to last (11..26):
 *         u16 dwell_ms             energy measured while listening for
 *                                  dwell_ms, one SCAN line per channel, then
 *                                  a STATUS with the radio off again
 *   SNIFF u8 channel               every frame heard on it, in FRAMES batches,
 *                                  until STOP
 *   STOP                           the radio off; a STATUS
 *   UNGUARD                        lets the radio on again after the guard
 *                                  took it off (below)
 *
 * C6 -> P4:
 *
 *   STATUS  aos_154_status_t
 *   SCAN    aos_154_scan_t, one channel
 *   FRAMES  records one after another, each aos_154_rec_t and then its
 *           frame: the PSDU as the radio gave it (len bytes). The radio has
 *           checked the FCS and put the RSSI and the LQI in its two bytes;
 *           the P4 writes the FCS back (aos_154.c, fix_fcs)
 *
 * The guard: the radio is the one new thing in the C6, and a C6 that falls
 * over at every start leaves the board with no Wi-Fi, with no way back but
 * a cable on J7 (docs/C6.md). So the radio is off at every start and only
 * the P4 turns it on; if the C6 restarts twice in a row by a fault while it
 * was on, it stays off (STATUS says "guarded") until UNGUARD.
 */
#pragma once

#include <stdint.h>

#define AOS_154_PROTO       1

#define AOS_154_MSG_CMD     0x15400001u
#define AOS_154_MSG_STATUS  0x15400081u
#define AOS_154_MSG_SCAN    0x15400082u
#define AOS_154_MSG_FRAMES  0x15400083u

enum { AOS_154_OP_HELLO = 1, AOS_154_OP_SCAN, AOS_154_OP_SNIFF, AOS_154_OP_STOP, AOS_154_OP_UNGUARD };

enum { AOS_154_IDLE = 0, AOS_154_SCANNING, AOS_154_SNIFFING };

#define AOS_154_CH_FIRST    11
#define AOS_154_CH_LAST     26

typedef struct __attribute__((packed)) {
    uint8_t  proto;             /* AOS_154_PROTO */
    uint8_t  state;             /* AOS_154_IDLE... */
    uint8_t  channel;           /* the one in use, 0 when idle */
    uint8_t  guarded;           /* the guard keeps the radio off */
    uint32_t frames;            /* heard since SNIFF */
    uint32_t dropped;           /* heard and not sent: the C6's buffer was full */
    uint32_t uptime_s;          /* the C6's */
    int8_t   err;               /* the last operation's, 0 when it went well */
    uint8_t  faults;            /* restarts by a fault with the radio on, in a row */
    uint16_t reserved;
} aos_154_status_t;

typedef struct __attribute__((packed)) {
    uint8_t  channel;
    int8_t   ed_max;            /* dBm: the energy measured, the highest */
    int8_t   ed_avg;            /* and the average */
    uint8_t  samples;
    uint16_t frames;            /* heard while listening */
    uint16_t bytes;
    uint16_t dwell_ms;
    uint16_t reserved;
} aos_154_scan_t;

typedef struct __attribute__((packed)) {
    uint8_t  len;               /* the frame's bytes that follow */
    int8_t   rssi;
    uint8_t  lqi;
    uint8_t  channel;
    uint32_t t_lo;              /* the C6's clock when it was heard, microseconds */
    uint32_t t_hi;
} aos_154_rec_t;
