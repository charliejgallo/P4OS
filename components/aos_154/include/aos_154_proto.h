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
 *   ZB_START u8 channel            (protocol 3, a measuring step) Espressif's
 *                                  Zigbee stack as a coordinator forming a
 *                                  network of its own on that channel, closed
 *                                  to joining; the radio is the stack's
 *                                  until ZB_STOP
 *   ZB_STOP
 *   SLOTS                          (protocol 4) the C6 answers with SLOTS: its
 *                                  partition table and where it runs
 *   SLOTS_GROW                     writes the repository's partition table (two
 *                                  app slots of 1.94 MB) over the factory one
 *                                  (1.5 MB), with c6/main/slots.c's checks;
 *                                  a SLOTS answer, used from the next start
 *   RCP_START                      (protocol 5) the C6 becomes a radio
 *                                  co-processor for a stack on the P4: the IDF's
 *                                  OpenThread RCP, its spinel (HDLC-framed) in
 *                                  SPINEL messages both ways, until it restarts
 *   LOG u8 on                      (protocol 3) the C6's log lines to the P4,
 *                                  as LOG messages, while on: its own
 *                                  serial port is on J7, out of reach
 *
 * C6 -> P4:
 *
 *   STATUS  aos_154_status_t
 *   SCAN    aos_154_scan_t, one channel
 *   LOG     one or more log lines of the C6, text
 *   SLOTS   aos_154_slots_t
 *   SPINEL  bytes of the RCP's spinel stream, as they come (either way: the
 *           P4's go as AOS_154_MSG_SPINEL_IN)
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

#define AOS_154_PROTO       5           /* 2: the status says the C6's free memory;
                                           3: Espressif's Zigbee stack on request;
                                           4: the C6's app slots (not the radio's, but
                                              this is the P4's channel to the C6);
                                           5: the C6 as an RCP, spinel both ways */

#define AOS_154_MSG_CMD     0x15400001u
#define AOS_154_MSG_SPINEL_IN 0x15400002u       /* P4 -> C6: spinel for the RCP */
#define AOS_154_MSG_STATUS  0x15400081u
#define AOS_154_MSG_SCAN    0x15400082u
#define AOS_154_MSG_FRAMES  0x15400083u
#define AOS_154_MSG_LOG     0x15400084u
#define AOS_154_MSG_SLOTS   0x15400085u
#define AOS_154_MSG_SPINEL  0x15400086u

enum { AOS_154_OP_HELLO = 1, AOS_154_OP_SCAN, AOS_154_OP_SNIFF, AOS_154_OP_STOP, AOS_154_OP_UNGUARD,
       AOS_154_OP_ZB_START, AOS_154_OP_ZB_STOP, AOS_154_OP_LOG, AOS_154_OP_SLOTS, AOS_154_OP_SLOTS_GROW,
       AOS_154_OP_RCP_START };

/* the table on the C6's flash (protocol 4) */
enum { AOS_154_SLOTS_UNKNOWN = 0, AOS_154_SLOTS_FACTORY, AOS_154_SLOTS_GROWN, AOS_154_SLOTS_OTHER };

typedef struct __attribute__((packed)) {
    uint8_t  layout;            /* AOS_154_SLOTS_*: the table on the flash now */
    int8_t   grow;              /* SLOTS_GROW's result: 1 written, 0 already; below 0 slots.h */
    uint16_t reserved;
    uint32_t slot_size;         /* ota_0's as this start read the table (the layout in use) */
    uint32_t running_addr;      /* where the running image is: 0x10000 is ota_0 */
} aos_154_slots_t;

enum { AOS_154_IDLE = 0, AOS_154_SCANNING, AOS_154_SNIFFING, AOS_154_ZIGBEE, AOS_154_RCP };

/* the stack's state in the status (protocol 3) */
enum { AOS_154_ZB_OFF = 0, AOS_154_ZB_STARTING, AOS_154_ZB_FORMED, AOS_154_ZB_FAILED };

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
    uint8_t  zb;                /* AOS_154_ZB_* (protocol 3; 0 before) */
    uint8_t  zb_channel;
    /* since protocol 2 (a protocol 1 status ends above: AOS_154_STATUS_V1) */
    uint32_t heap_free;         /* the C6's free internal memory, bytes */
    uint32_t heap_min;          /* the least it had since it started */
    /* since protocol 3 */
    uint32_t zb_heap_before;    /* free memory when the stack was asked for */
    uint32_t zb_heap_after;     /* and when its network was formed */
} aos_154_status_t;

#define AOS_154_STATUS_V1   20          /* the bytes of a protocol 1 status */

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
