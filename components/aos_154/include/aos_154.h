/*
 * P4OS - the 802.15.4 radio (Zigbee, Thread), the C6's, lent to the P4
 * (docs/plan/ZIGBEE.md).
 *
 * The C6 does the Wi-Fi and the BLE for the P4 and, since its firmware
 * 3.0.9-154.1, listens on 802.15.4 when asked: a scan of the sixteen
 * channels' energy and traffic, and every frame heard on one (a sniffer).
 * What they say to each other is in aos_154_proto.h; this is the P4's side,
 * for the screens and the portal.
 *
 * A C6 with an older firmware does not answer: aos_154_info() says
 * 'reachable' false and the screens offer the C6's update.
 *
 * A capture goes to the card as pcap (Wireshark reads it: the link type is
 * IEEE 802.15.4 TAP, so each frame keeps its channel, RSSI and LQI) in
 * <card>/zigbee/, and the last AOS_154_KEEP frames stay in PSRAM for the
 * screens, numbered from 1.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AOS_154_KEEP 4096

typedef struct {
    bool     reachable;         /* the C6 answered since the board started */
    uint8_t  proto;             /* AOS_154_PROTO of the C6's firmware */
    uint8_t  state;             /* AOS_154_IDLE, _SCANNING, _SNIFFING */
    uint8_t  channel;
    bool     guarded;           /* the C6 fell over with the radio on: it keeps it off */
    uint8_t  faults;
    int8_t   err;               /* the C6's answer to the last request, 0 when it went well */
    uint32_t frames;            /* heard on this sniff, as the C6 counts them */
    uint32_t dropped;           /* lost in the C6, its buffer full */
    uint32_t last_seq;          /* the newest frame kept here, 0 for none */
    uint32_t scan_seq;          /* goes up with every channel a scan measures */
    char     pcap[128];         /* the capture being written, "" for none */
    uint32_t pcap_frames;
    uint32_t heap_free, heap_min;   /* the C6's free memory (protocol 2), 0 when it does not say */
    uint8_t  zb, zb_channel;        /* Espressif's Zigbee stack on the C6 (protocol 3): AOS_154_ZB_* */
    uint32_t zb_heap_before, zb_heap_after;
} aos_154_info_t;

typedef struct {
    uint8_t  channel;
    int8_t   ed_max, ed_avg;    /* dBm */
    uint16_t frames, bytes;     /* heard while listening */
    uint16_t dwell_ms;
    uint32_t when_ms;           /* aos_hal_uptime_ms() when it came; 0: never measured */
} aos_154_chan_t;

typedef struct {
    uint32_t seq;
    uint64_t t_us;              /* the C6's clock */
    int8_t   rssi;
    uint8_t  lqi, channel, len;
    uint8_t  data[127];         /* the PSDU, FCS included */
} aos_154_frame_t;

/* A device heard: by its short address on a PAN, or by its long one. The
 * long one of a short comes from Zigbee's network header when it carries it. */
#define AOS_154_DEVICES 64
typedef struct {
    uint16_t pan;               /* 0xffff when the frame did not say */
    uint16_t short_addr;        /* 0xffff: known only by its long address */
    uint8_t  ext[8];            /* the long address, most significant byte first */
    bool     has_ext;
    uint8_t  channel;
    uint32_t frames;
    int8_t   rssi_last, rssi_avg;
    uint32_t first_ms, last_ms; /* aos_hal_uptime_ms() */
    char     vendor[29];        /* the radio's maker by the long address's first three
                                 * bytes (<card>/zigbee/oui.bin, tools/zigbee/gen_oui.py);
                                 * "" when unknown */
    bool     vendor_done;       /* looked up already */
} aos_154_device_t;

/* The devices heard since the board started (or since aos_154_devices_clear),
 * the most recently heard first; how many. */
int  aos_154_devices(aos_154_device_t *out, int max);
void aos_154_devices_clear(void);

/* Asks the C6 how it is; the answer comes by itself (aos_154_info). */
bool aos_154_hello(void);
/* Each channel from first to last (11..26), dwell_ms on each. */
bool aos_154_scan(int first, int last, int dwell_ms);
/* The last measure of every channel, 11..26 at [0..15]. */
void aos_154_channels(aos_154_chan_t out[16]);
/* Listens on one channel; 'pcap' writes what it hears to the card too. */
bool aos_154_sniff(int channel, bool pcap);
bool aos_154_stop(void);
/* Espressif's Zigbee stack on the C6, a measuring step (protocol 3): a
 * coordinator forming a network of its own on 'channel', closed to joining;
 * the radio is the stack's until aos_154_zigbee_stop(). */
bool aos_154_zigbee_start(int channel);
bool aos_154_zigbee_stop(void);
/* The C6's app slots (protocol 4, docs/C6.md "The slots"): a request, the
 * answer comes by itself into aos_154_slots(). Growing writes the C6's
 * partition table (two slots of 1.94 MB for the factory 1.5 MB), with its
 * checks; it is used from the next start. */
typedef struct {
    bool     known;             /* the C6 answered */
    uint8_t  layout;            /* AOS_154_SLOTS_* */
    int8_t   grow;              /* the last growing's result */
    uint32_t slot_size, running_addr;
    uint32_t seq;               /* goes up with every answer */
} aos_154_slots_info_t;
bool aos_154_slots_ask(bool grow);
void aos_154_slots(aos_154_slots_info_t *out);
/* The C6's log lines into the P4's log ("c6: ..."), on or off (protocol 3). */
bool aos_154_c6_log(bool on);
/* After the C6 kept the radio off by its guard: lets it on again. */
bool aos_154_unguard(void);
void aos_154_info(aos_154_info_t *out);
/* The frames kept, from 'from' (a seq) on, up to max; how many. */
int  aos_154_frames(uint32_t from, aos_154_frame_t *out, int max);

/* The C6 as a radio co-processor for a stack on the P4 (protocol 5): from
 * the request on, the C6's radio is the RCP's until it restarts, and spinel
 * goes both ways (aos_154_spinel_*). */
bool aos_154_rcp_start(void);
/* A test of the link: asks the RCP its version in spinel and waits up to
 * 'ms' for the answer; true with the version in 'out'. */
bool aos_154_rcp_probe(char *out, size_t n, uint32_t ms);
/* Where the RCP's spinel bytes go (a stack's transport), or NULL. Called
 * from the link's task: it must not wait. */
void aos_154_spinel_sink(void (*sink)(const uint8_t *data, size_t len));
bool aos_154_spinel_send(const uint8_t *data, size_t len);

/* ---- for the link (aos_154_link_p4.c, sim/aos_154_link_sim.c) ---- */
bool aos_154_link_send(const uint8_t *data, size_t len);
bool aos_154_link_spinel(const uint8_t *data, size_t len);
bool aos_154_link_open(void);
/* what the C6 sent, from the link's task */
void aos_154_on_msg(uint32_t id, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
