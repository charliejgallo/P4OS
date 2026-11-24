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
static aos_154_device_t *s_dev;         /* AOS_154_DEVICES, PSRAM */
static int32_t  s_rssi_acc[AOS_154_DEVICES];
static int s_ndev;
static aos_154_slots_info_t s_slots;

/* the RCP's spinel: to a stack's transport, and a little of it kept for the probe */
static void (*volatile s_spinel_sink)(const uint8_t *, size_t);
static uint8_t s_probe[512];
static volatile size_t s_probe_n;

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

bool aos_154_zigbee_start(int channel)
{
    if (channel < AOS_154_CH_FIRST || channel > AOS_154_CH_LAST) return false;
    pcap_stop();
    uint8_t b[] = { AOS_154_OP_ZB_START, (uint8_t)channel };
    return cmd(b, sizeof b);
}

bool aos_154_zigbee_stop(void)
{
    uint8_t b[] = { AOS_154_OP_ZB_STOP };
    return cmd(b, sizeof b);
}

bool aos_154_slots_ask(bool grow)
{
    uint8_t b[] = { grow ? AOS_154_OP_SLOTS_GROW : AOS_154_OP_SLOTS };
    return cmd(b, sizeof b);
}

void aos_154_slots(aos_154_slots_info_t *out)
{
    if (!s_lock) { memset(out, 0, sizeof *out); return; }
    lock();
    *out = s_slots;
    unlock();
}

bool aos_154_rcp_start(void)
{
    uint8_t b[] = { AOS_154_OP_RCP_START };
    return cmd(b, sizeof b);
}

void aos_154_spinel_sink(void (*sink)(const uint8_t *, size_t))
{
    s_spinel_sink = sink;
}

bool aos_154_spinel_send(const uint8_t *data, size_t len)
{
    return open_link() && aos_154_link_spinel(data, len);
}

/* HDLC-lite, as spinel frames it: 0x7E, the bytes with 0x7E/0x7D/0x11/0x13
 * escaped (0x7D, byte ^ 0x20), the FCS-16 (PPP's) complemented, 0x7E */
static uint16_t fcs16(uint16_t f, uint8_t b)
{
    f ^= b;
    for (int i = 0; i < 8; i++) f = (f & 1) ? (uint16_t)((f >> 1) ^ 0x8408) : (uint16_t)(f >> 1);
    return f;
}

static size_t hdlc_put(uint8_t *o, uint8_t b)
{
    if (b == 0x7E || b == 0x7D || b == 0x11 || b == 0x13 || b == 0xF8) {
        o[0] = 0x7D;
        o[1] = b ^ 0x20;
        return 2;
    }
    o[0] = b;
    return 1;
}

bool aos_154_rcp_probe(char *out, size_t n, uint32_t ms)
{
    /* PROP_VALUE_GET (2) of PROP_NCP_VERSION (2), header 0x81 (IID 0, TID 1) */
    static const uint8_t ask[] = { 0x81, 0x02, 0x02 };
    uint8_t f[16];
    size_t k = 0;
    uint16_t fcs = 0xFFFF;
    f[k++] = 0x7E;
    for (size_t i = 0; i < sizeof ask; i++) { fcs = fcs16(fcs, ask[i]); k += hdlc_put(f + k, ask[i]); }
    fcs ^= 0xFFFF;
    k += hdlc_put(f + k, (uint8_t)fcs);
    k += hdlc_put(f + k, (uint8_t)(fcs >> 8));
    f[k++] = 0x7E;
    s_probe_n = 0;
    if (!aos_154_spinel_send(f, k)) return false;
    /* the answer: a frame with header 0x81, PROP_VALUE_IS (6), PROP_NCP_VERSION (2), a string */
    for (uint32_t t = 0; t < ms; t += 20) {
        aos_hal_sleep_ms(20);
        uint8_t d[512];
        size_t m = 0;
        bool esc = false;
        for (size_t i = 0; i < s_probe_n && m < sizeof d; i++) {
            uint8_t b = s_probe[i];
            if (b == 0x7E) {
                if (m >= 5 && d[0] == 0x81 && d[1] == 0x06 && d[2] == 0x02) {
                    size_t len = m - 2 - 3;             /* without the FCS and the header */
                    if (len >= n) len = n - 1;
                    memcpy(out, d + 3, len);
                    out[len] = '\0';
                    return true;
                }
                m = 0;
                esc = false;
            } else if (b == 0x7D) esc = true;
            else {
                d[m++] = esc ? b ^ 0x20 : b;
                esc = false;
            }
        }
    }
    return false;
}

bool aos_154_c6_log(bool on)
{
    uint8_t b[] = { AOS_154_OP_LOG, on };
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

/* ---- the devices heard ---------------------------------------------------- */

static aos_154_device_t *dev_find(uint16_t pan, uint16_t sh, const uint8_t *ext)
{
    if (!s_dev) {
        s_dev = calloc(AOS_154_DEVICES, sizeof *s_dev);
        if (!s_dev) return NULL;
    }
    for (int i = 0; i < s_ndev; i++) {
        aos_154_device_t *d = &s_dev[i];
        if (ext && d->has_ext && !memcmp(d->ext, ext, 8)) return d;
        if (!ext && sh != 0xffff && d->short_addr == sh && (d->pan == pan || pan == 0xffff)) return d;
    }
    int at = s_ndev;
    if (at == AOS_154_DEVICES) {        /* full: the one heard longest ago goes */
        at = 0;
        for (int i = 1; i < s_ndev; i++) if ((int32_t)(s_dev[i].last_ms - s_dev[at].last_ms) < 0) at = i;
    } else s_ndev++;
    aos_154_device_t *d = &s_dev[at];
    memset(d, 0, sizeof *d);
    s_rssi_acc[at] = 0;
    d->pan = pan;
    d->short_addr = ext ? 0xffff : sh;
    if (ext) { memcpy(d->ext, ext, 8); d->has_ext = true; }
    d->first_ms = (uint32_t)aos_hal_uptime_ms();
    return d;
}

static void dev_heard(aos_154_device_t *d, const aos_154_frame_t *f)
{
    int i = (int)(d - s_dev);
    d->frames++;
    d->channel = f->channel;
    d->rssi_last = f->rssi;
    s_rssi_acc[i] += f->rssi;
    d->rssi_avg = (int8_t)(s_rssi_acc[i] / (int32_t)d->frames);
    d->last_ms = (uint32_t)aos_hal_uptime_ms();
}

/* The sender of a frame, from its MAC header, and on a Zigbee data frame the
 * long address its network header may carry for the short one. */
static void dev_note(const aos_154_frame_t *fr)
{
    const uint8_t *f = fr->data;
    int len = fr->len - 2;
    if (len < 3) return;
    uint16_t fcf = f[0] | f[1] << 8;
    int type = fcf & 7, comp = fcf >> 6 & 1, dm = fcf >> 10 & 3, sm = fcf >> 14 & 3;
    if (type == 2 || !sm) return;       /* an acknowledgement says nobody */
    int at = 3;
    uint16_t pan = 0xffff;
    if (dm) {
        pan = f[at] | f[at + 1] << 8;
        at += 2 + (dm == 3 ? 8 : 2);
    }
    if (!comp) {
        if (at + 2 > len) return;
        pan = f[at] | f[at + 1] << 8;
        at += 2;
    }
    if (at + (sm == 3 ? 8 : 2) > len) return;
    aos_154_device_t *d;
    if (sm == 3) {
        uint8_t ext[8];
        for (int i = 0; i < 8; i++) ext[i] = f[at + 7 - i];
        at += 8;
        d = dev_find(pan, 0xffff, ext);
    } else {
        uint16_t sh = f[at] | f[at + 1] << 8;
        at += 2;
        d = dev_find(pan, sh, NULL);
        /* Zigbee's network header: frame control, destination, source,
         * radius, sequence, then the long destination and source when its
         * bits 11 and 12 say so */
        if (d && type == 1 && !(fcf >> 3 & 1) && at + 8 <= len && ((f[at] >> 2) & 0x0f) == 2) {
            uint16_t nf = f[at] | f[at + 1] << 8;
            uint16_t nsrc = f[at + 4] | f[at + 5] << 8;
            int x = at + 8 + (nf >> 11 & 1 ? 8 : 0);
            if (nsrc == sh && nf >> 12 & 1 && x + 8 <= len && !d->has_ext) {
                for (int i = 0; i < 8; i++) d->ext[i] = f[x + 7 - i];
                d->has_ext = true;
            }
        }
    }
    if (d) {
        if (d->pan == 0xffff) d->pan = pan;
        dev_heard(d, fr);
    }
}

/* The maker of a long address, from <card>/zigbee/oui.bin: "AOUI", u32
 * count, then records of 32 bytes sorted by prefix (u8 prefix[3], u8 len,
 * char name[28]). A binary search with fseek; nothing kept in memory. */
static bool vendor_of(const uint8_t ext[8], char *out, size_t n)
{
    const char *root = aos_hal_path_sd_root();
    if (!root) return false;
    char path[96];
    snprintf(path, sizeof path, "%s/zigbee/oui.bin", root);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[8], r[32];
    bool ok = false;
    if (fread(h, 1, 8, f) == 8 && !memcmp(h, "AOUI", 4)) {
        uint32_t lo = 0, hi = (uint32_t)h[4] | (uint32_t)h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
        while (lo < hi && !ok) {
            uint32_t mid = (lo + hi) / 2;
            if (fseek(f, 8 + (long)mid * 32, SEEK_SET) || fread(r, 1, 32, f) != 32) break;
            int c = memcmp(r, ext, 3);
            if (c < 0) lo = mid + 1;
            else if (c > 0) hi = mid;
            else {
                size_t len = r[3] > 28 ? 28 : r[3];
                if (len >= n) len = n - 1;
                memcpy(out, r + 4, len);
                out[len] = '\0';
                ok = true;
            }
        }
    }
    fclose(f);
    return ok;
}

int aos_154_devices(aos_154_device_t *out, int max)
{
    if (!s_lock) return 0;
    lock();
    int n = 0;
    if (s_dev)
        for (int i = 0; i < s_ndev && n < max; i++) out[n++] = s_dev[i];
    unlock();
    /* the makers of the long addresses learnt since the last look, once each,
     * out of the lock (it reads the card) */
    for (int i = 0; i < n; i++) {
        if (!out[i].has_ext || out[i].vendor_done) continue;
        if (!vendor_of(out[i].ext, out[i].vendor, sizeof out[i].vendor)) out[i].vendor[0] = '\0';
        out[i].vendor_done = true;
        lock();
        for (int k = 0; k < s_ndev; k++)
            if (s_dev[k].has_ext && !memcmp(s_dev[k].ext, out[i].ext, 8)) {
                memcpy(s_dev[k].vendor, out[i].vendor, sizeof out[i].vendor);
                s_dev[k].vendor_done = true;
            }
        unlock();
    }
    /* the most recently heard first */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && (int32_t)(out[j].last_ms - out[j - 1].last_ms) > 0; j--) {
            aos_154_device_t t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}

void aos_154_devices_clear(void)
{
    if (!s_lock) return;
    lock();
    s_ndev = 0;
    unlock();
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

/* The C6's log lines, into the P4's log as "c6: ..." */
static void c6_log(const uint8_t *d, size_t n)
{
    char line[200];
    size_t at = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || d[i] == '\n' || at == sizeof line - 1) {
            line[at] = '\0';
            /* the C6's colour codes and its "I (1234) tag:" stay as they come */
            if (at) aos_hal_log("c6", "%s", line);
            at = 0;
            if (i < n && d[i] != '\n') line[at++] = (char)d[i];
        } else if (d[i] != '\r') line[at++] = (char)d[i];
    }
}

void aos_154_on_msg(uint32_t id, const uint8_t *d, size_t n)
{
    if (id == AOS_154_MSG_LOG) { c6_log(d, n); return; }
    if (id == AOS_154_MSG_SPINEL) {
        void (*sink)(const uint8_t *, size_t) = s_spinel_sink;
        if (sink) sink(d, n);
        size_t at = s_probe_n;
        size_t k = n < sizeof s_probe - at ? n : sizeof s_probe - at;
        memcpy(s_probe + at, d, k);
        s_probe_n = at + k;
        return;
    }
    if (!s_lock) return;
    lock();
    if (id == AOS_154_MSG_SLOTS && n >= sizeof(aos_154_slots_t)) {
        aos_154_slots_t r;
        memcpy(&r, d, sizeof r);
        s_slots.known = true;
        s_slots.layout = r.layout;
        s_slots.grow = r.grow;
        s_slots.slot_size = r.slot_size;
        s_slots.running_addr = r.running_addr;
        s_slots.seq++;
        aos_hal_log(TAG, "the C6's slots: table %u, %u KB in use, running at 0x%x%s", r.layout,
                    (unsigned)(r.slot_size / 1024), (unsigned)r.running_addr,
                    r.grow == 1 ? "; the new table is written, from the next start" : "");
    }
    if (id == AOS_154_MSG_STATUS && n >= AOS_154_STATUS_V1) {
        aos_154_status_t st = { 0 };    /* a protocol 1 C6 leaves the newer fields at zero */
        memcpy(&st, d, n < sizeof st ? n : sizeof st);
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
        s_info.heap_free = st.heap_free;
        s_info.heap_min = st.heap_min;
        s_info.zb = st.zb;
        s_info.zb_channel = st.zb_channel;
        s_info.zb_heap_before = st.zb_heap_before;
        s_info.zb_heap_after = st.zb_heap_after;
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
            dev_note(f);
            at += rec.len;
        }
    }
    unlock();
}
