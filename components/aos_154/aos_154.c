/*
 * P4OS - the 802.15.4 radio, the P4's side (aos_154.h, docs/plan/ZIGBEE.md).
 *
 * The link (aos_154_link_p4.c on the board, sim/aos_154_link_sim.c in the
 * simulator) hands over what the C6 says from its own task, which on the
 * board is esp_hosted's: aos_154_on_msg() only copies into memory and
 * returns. The capture to the card is a task of its own that writes the new
 * frames every 200 ms.
 */
#include "aos_154.h"
#include "aos_154_proto.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define TAG          "154"
#define LINKTYPE_TAP 283                /* IEEE 802.15.4 with a TAP header: channel, RSSI, LQI */
#define TAP_LEN      36

static void *s_lock;
static bool s_open;
static aos_154_info_t s_info;
static aos_154_chan_t s_chan[16];
static aos_154_frame_t *s_ring;         /* AOS_154_KEEP of them, PSRAM (malloc puts them there) */
static uint32_t s_seq;                  /* the newest frame's */

/* the capture */
static FILE *s_pcap;
static bool  s_pcap_run;
static uint32_t s_pcap_seq;             /* written up to here */
static time_t   s_pcap_t0;              /* the wall clock at the first frame ... */
static uint64_t s_pcap_us0;             /* ... and the C6's */

static void lock(void)   { aos_hal_mutex_lock(s_lock); }
static void unlock(void) { aos_hal_mutex_unlock(s_lock); }

static bool open_link(void)
{
    if (!s_lock) s_lock = aos_hal_mutex_create();
    if (!s_open) s_open = aos_154_link_open();
    return s_open;
}

static bool cmd(const uint8_t *b, size_t n)
{
    return open_link() && aos_154_link_send(b, n);
}

bool aos_154_hello(void)
{
    uint8_t b[] = { AOS_154_OP_HELLO };
    return cmd(b, sizeof b);
}

bool aos_154_scan(int first, int last, int dwell_ms)
{
    if (first < AOS_154_CH_FIRST || last > AOS_154_CH_LAST || first > last || dwell_ms < 20 || dwell_ms > 2000) return false;
    uint8_t b[] = { AOS_154_OP_SCAN, (uint8_t)first, (uint8_t)last, (uint8_t)dwell_ms, (uint8_t)(dwell_ms >> 8) };
    return cmd(b, sizeof b);
}

void aos_154_channels(aos_154_chan_t out[16])
{
    if (!s_lock) { memset(out, 0, sizeof(aos_154_chan_t) * 16); return; }
    lock();
    memcpy(out, s_chan, sizeof s_chan);
    unlock();
}

/* ---- the capture ---------------------------------------------------------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

static void pcap_record(FILE *f, const aos_154_frame_t *fr)
{
    if (!s_pcap_us0) {
        s_pcap_us0 = fr->t_us;
        s_pcap_t0 = aos_hal_time_is_valid() ? time(NULL) : 0;
    }
    uint64_t us = (uint64_t)s_pcap_t0 * 1000000u + (fr->t_us - s_pcap_us0);
    uint8_t h[16 + TAP_LEN];
    put32(h, (uint32_t)(us / 1000000u));
    put32(h + 4, (uint32_t)(us % 1000000u));
    put32(h + 8, TAP_LEN + fr->len);
    put32(h + 12, TAP_LEN + fr->len);
    uint8_t *t = h + 16;
    memset(t, 0, TAP_LEN);
    put16(t + 2, TAP_LEN);              /* version 0, reserved, the header's length */
    put16(t + 4, 0);  put16(t + 6, 1);  t[8] = 1;               /* FCS type: 16-bit CRC */
    put16(t + 12, 1); put16(t + 14, 4);                          /* RSS, a float in dBm */
    float rssi = fr->rssi;
    memcpy(t + 16, &rssi, 4);
    put16(t + 20, 3); put16(t + 22, 3); put16(t + 24, fr->channel); t[26] = 0;  /* channel, page 0 */
    put16(t + 28, 10); put16(t + 30, 1); t[32] = fr->lqi;       /* LQI */
    fwrite(h, 1, sizeof h, f);
    fwrite(fr->data, 1, fr->len, f);
}

static void pcap_task(void *arg)
{
    (void)arg;
    static aos_154_frame_t batch[32];
    for (;;) {
        aos_hal_sleep_ms(200);
        lock();
        bool run = s_pcap_run;
        FILE *f = s_pcap;
        unlock();
        for (;;) {
            int n = aos_154_frames(s_pcap_seq + 1, batch, 32);
            if (n <= 0 || !f) break;
            for (int i = 0; i < n; i++) pcap_record(f, &batch[i]);
            s_pcap_seq = batch[n - 1].seq;
            lock();
            s_info.pcap_frames += (uint32_t)n;
            unlock();
        }
        if (f) fflush(f);
        if (!run) break;
    }
    lock();
    if (s_pcap) fclose(s_pcap);
    s_pcap = NULL;
    unlock();
}

static bool pcap_start(void)
{
    const char *root = aos_hal_path_sd_root();
    if (!root) return false;
    char dir[64], path[128];
    snprintf(dir, sizeof dir, "%s/zigbee", root);
    mkdir(dir, 0777);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (aos_hal_time_is_valid())
        snprintf(path, sizeof path, "%s/captura-%04d%02d%02d-%02d%02d%02d.pcap", dir, tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    else snprintf(path, sizeof path, "%s/captura-%u.pcap", dir, (unsigned)aos_hal_uptime_ms());
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint8_t g[24];
    put32(g, 0xa1b2c3d4);               /* microseconds */
    put16(g + 4, 2); put16(g + 6, 4);
    put32(g + 8, 0); put32(g + 12, 0);
    put32(g + 16, 65535); put32(g + 20, LINKTYPE_TAP);
    fwrite(g, 1, sizeof g, f);
    lock();
    s_pcap = f;
    s_pcap_run = true;
    s_pcap_seq = s_seq;                 /* from what comes next */
    s_pcap_us0 = 0;
    s_info.pcap_frames = 0;
    snprintf(s_info.pcap, sizeof s_info.pcap, "%s", path);
    unlock();
    if (!aos_hal_thread_start("aos_154_pcap", pcap_task, NULL, 4096, 2)) {
        lock();
        fclose(s_pcap);
        s_pcap = NULL;
        s_pcap_run = false;
        s_info.pcap[0] = '\0';
        unlock();
        return false;
    }
    aos_hal_log(TAG, "capturing to %s", path);
    return true;
}

/* The task writes what is left and closes the file; this waits for it (up
 * to a second), so a new capture never meets the old task. */
static void pcap_stop(void)
{
    if (!s_lock) return;
    lock();
    s_pcap_run = false;
    unlock();
    for (int i = 0; i < 20; i++) {
        lock();
        bool open = s_pcap != NULL;
        unlock();
        if (!open) return;
        aos_hal_sleep_ms(50);
    }
}

bool aos_154_sniff(int channel, bool pcap)
{
    if (channel < AOS_154_CH_FIRST || channel > AOS_154_CH_LAST || !open_link()) return false;
    lock();
    if (!s_ring) s_ring = malloc(sizeof(aos_154_frame_t) * AOS_154_KEEP);
    bool ok = s_ring != NULL;
    unlock();
    if (!ok) return false;
    pcap_stop();
    if (pcap && !pcap_start()) aos_hal_log(TAG, "could not open a capture on the card");
    uint8_t b[] = { AOS_154_OP_SNIFF, (uint8_t)channel };
    return cmd(b, sizeof b);
}

bool aos_154_stop(void)
{
    pcap_stop();
    uint8_t b[] = { AOS_154_OP_STOP };
    return cmd(b, sizeof b);
}

bool aos_154_unguard(void)
{
    uint8_t b[] = { AOS_154_OP_UNGUARD };
    return cmd(b, sizeof b);
}

void aos_154_info(aos_154_info_t *out)
{
    if (!s_lock) { memset(out, 0, sizeof *out); return; }
    lock();
    *out = s_info;
    out->last_seq = s_seq;
    if (!s_pcap) out->pcap[0] = '\0';
    unlock();
}

int aos_154_frames(uint32_t from, aos_154_frame_t *out, int max)
{
    if (!s_lock) return 0;
    lock();
    int n = 0;
    if (s_ring && s_seq) {
        uint32_t oldest = s_seq >= AOS_154_KEEP ? s_seq - AOS_154_KEEP + 1 : 1;
        if (from < oldest) from = oldest;
        for (uint32_t q = from; q <= s_seq && n < max; q++) out[n++] = s_ring[(q - 1) % AOS_154_KEEP];
    }
    unlock();
    return n;
}

/* ---- what the C6 says ----------------------------------------------------- */

/* The 802.15.4 FCS: CRC-16/KERMIT, little-endian. The C6's radio checks it
 * and hands the frame over with the RSSI and the LQI in its place (the IDF's
 * esp_ieee802154_dev.c: "crc is not written to rx buffer"); a capture needs
 * it back, or Wireshark calls every frame bad. */
static void fix_fcs(uint8_t *f, int len)
{
    if (len < 3) return;
    uint16_t c = 0;
    for (int i = 0; i < len - 2; i++) {
        c ^= f[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
    }
    f[len - 2] = (uint8_t)c;
    f[len - 1] = (uint8_t)(c >> 8);
}

void aos_154_on_msg(uint32_t id, const uint8_t *d, size_t n)
{
    if (!s_lock) return;
    lock();
    if (id == AOS_154_MSG_STATUS && n >= sizeof(aos_154_status_t)) {
        aos_154_status_t st;
        memcpy(&st, d, sizeof st);
        bool first = !s_info.reachable;
        s_info.reachable = true;
        s_info.proto = st.proto;
        s_info.state = st.state;
        s_info.channel = st.channel;
        s_info.guarded = st.guarded;
        s_info.faults = st.faults;
        s_info.err = st.err;
        s_info.frames = st.frames;
        s_info.dropped = st.dropped;
        if (first) aos_hal_log(TAG, "the C6 answers: protocol %u, up %u s%s", st.proto, (unsigned)st.uptime_s,
                               st.guarded ? ", the radio guarded off" : "");
    } else if (id == AOS_154_MSG_SCAN && n >= sizeof(aos_154_scan_t)) {
        aos_154_scan_t r;
        memcpy(&r, d, sizeof r);
        if (r.channel >= AOS_154_CH_FIRST && r.channel <= AOS_154_CH_LAST) {
            aos_154_chan_t *c = &s_chan[r.channel - AOS_154_CH_FIRST];
            c->channel = r.channel;
            c->ed_max = r.ed_max;
            c->ed_avg = r.ed_avg;
            c->frames = r.frames;
            c->bytes = r.bytes;
            c->dwell_ms = r.dwell_ms;
            c->when_ms = (uint32_t)aos_hal_uptime_ms() | 1;
            s_info.scan_seq++;
            s_info.reachable = true;
            s_info.state = AOS_154_SCANNING;
            s_info.channel = r.channel;
        }
    } else if (id == AOS_154_MSG_FRAMES && s_ring) {
        size_t at = 0;
        while (at + sizeof(aos_154_rec_t) <= n) {
            aos_154_rec_t rec;
            memcpy(&rec, d + at, sizeof rec);
            at += sizeof rec;
            if (rec.len > 127 || at + rec.len > n) break;
            aos_154_frame_t *f = &s_ring[s_seq % AOS_154_KEEP];
            f->seq = ++s_seq;
            f->t_us = (uint64_t)rec.t_hi << 32 | rec.t_lo;
            f->rssi = rec.rssi;
            f->lqi = rec.lqi;
            f->channel = rec.channel;
            f->len = rec.len;
            memcpy(f->data, d + at, rec.len);
            fix_fcs(f->data, rec.len);
            at += rec.len;
        }
    }
    unlock();
}
