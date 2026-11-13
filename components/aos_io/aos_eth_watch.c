/*
 * P4OS - Ethernet: looking at the traffic that reaches the port (aos_eth.h).
 *
 * Two things over the frames a capture hands it, one at a time:
 *
 *   counting    how much is sent to everyone, to a group, to another
 *               station; how much is ARP, DHCP, IPv4, IPv6; and how many
 *               broadcasts a second, now and at their worst.
 *   noticing    what a network should not have: two DHCP servers answering,
 *               one address claimed by two stations, a broadcast storm.
 *
 * and the file a capture leaves, in the pcap format every analyser opens
 * (Wireshark, tcpdump).
 *
 * As the rest of this driver's readers, it believes no length in a frame.
 * One state, no lock: the caller keeps its calls apart.
 */
#include "aos_eth.h"

#include <stdio.h>
#include <string.h>

#define ARP_SEEN   48           /* addresses remembered with who claimed them */
#define STORM_RATE 300          /* broadcasts a second that are a storm... */
#define STORM_SECS 3            /* ...when they go on for this long */

static struct {
    aos_eth_watch_t w;
    uint32_t sec;               /* the second being counted */
    uint32_t bcast_sec;         /* broadcasts in it */
    int      storm_run;         /* seconds in a row over STORM_RATE */
    struct { uint8_t ip[4], mac[6]; } arp[ARP_SEEN];
    int      arp_n, arp_next;
} W;

void aos_eth_watch_reset(void) { memset(&W, 0, sizeof W); }

static void second_turn(uint32_t sec)
{
    if (sec == W.sec) return;
    /* the second that ended, and the silent ones between it and now */
    W.w.bcast_now = sec == W.sec + 1 ? W.bcast_sec : 0;
    if (W.bcast_sec > W.w.bcast_peak) W.w.bcast_peak = W.bcast_sec;
    W.storm_run = W.bcast_sec >= STORM_RATE && sec == W.sec + 1 ? W.storm_run + 1 : 0;
    if (W.storm_run >= STORM_SECS) W.w.storm = true;
    W.sec = sec;
    W.bcast_sec = 0;
}

static void dhcp_server(const uint8_t *ip)
{
    char t[16];
    snprintf(t, sizeof t, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    for (int i = 0; i < W.w.dhcp_n; i++) if (!strcmp(W.w.dhcp_srv[i], t)) return;
    if (W.w.dhcp_n < 3) snprintf(W.w.dhcp_srv[W.w.dhcp_n++], sizeof W.w.dhcp_srv[0], "%s", t);
}

/* A DHCP server's answer: who it says it is (option 54), or where it came from. */
static void dhcp_reply(const uint8_t *p, size_t len, const uint8_t *src_ip)
{
    if (len < 240 || p[0] != 2 || p[236] != 0x63 || p[237] != 0x82 || p[238] != 0x53 || p[239] != 0x63) return;
    const uint8_t *id = src_ip;
    p += 240;
    len -= 240;
    while (len >= 1 && p[0] != 255) {
        if (p[0] == 0) { p++; len--; continue; }
        if (len < 2 || (size_t)p[1] + 2 > len) break;
        if (p[0] == 54 && p[1] == 4) id = p + 2;
        len -= (size_t)p[1] + 2;
        p += (size_t)p[1] + 2;
    }
    if (id[0] | id[1] | id[2] | id[3]) dhcp_server(id);
}

/* An ARP's sender: the address it says is its own. The same address from another MAC is two
 * stations with one address (or one that changed its card: the screen says "two", and who). */
static void arp_claim(const uint8_t *ip, const uint8_t *mac)
{
    if (!(ip[0] | ip[1] | ip[2] | ip[3])) return;           /* a probe: no address yet */
    for (int i = 0; i < W.arp_n; i++) {
        if (memcmp(W.arp[i].ip, ip, 4)) continue;
        if (memcmp(W.arp[i].mac, mac, 6) && !W.w.dup) {
            W.w.dup = true;
            snprintf(W.w.dup_ip, sizeof W.w.dup_ip, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            memcpy(W.w.dup_a, W.arp[i].mac, 6);
            memcpy(W.w.dup_b, mac, 6);
        }
        return;
    }
    int at = W.arp_n < ARP_SEEN ? W.arp_n++ : (W.arp_next = (W.arp_next + 1) % ARP_SEEN);
    memcpy(W.arp[at].ip, ip, 4);
    memcpy(W.arp[at].mac, mac, 6);
}

void aos_eth_watch_note(const uint8_t *f, size_t len, const uint8_t *self, uint32_t now_ms)
{
    if (!f || len < 14) return;
    second_turn(now_ms / 1000);
    W.w.frames++;
    W.w.bytes += (uint32_t)len;
    static const uint8_t ALL[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (!memcmp(f, ALL, 6)) { W.w.bcast++; W.bcast_sec++; }
    else if (f[0] & 1) W.w.mcast++;
    else if (self && memcmp(f, self, 6)) W.w.others++;
    size_t at = 12;
    if (len >= 18 && f[at] == 0x81 && f[at + 1] == 0x00) at += 4;
    unsigned type = f[at] << 8 | f[at + 1];
    at += 2;
    if (type == 0x0806) {
        W.w.arp++;
        if (len >= at + 28) arp_claim(f + at + 14, f + at + 8);
    } else if (type == 0x0800) {
        W.w.ipv4++;
        if (len >= at + 20) {
            size_t ihl = (size_t)(f[at] & 15) * 4;
            if (f[at + 9] == 17 && ihl >= 20 && len >= at + ihl + 8) {
                const uint8_t *u = f + at + ihl;
                unsigned sport = u[0] << 8 | u[1], dport = u[2] << 8 | u[3];
                if (sport == 67 || dport == 67) W.w.dhcp++;
                if (sport == 67 && dport == 68) dhcp_reply(u + 8, len - at - ihl - 8, f + at + 12);
            }
        }
    } else if (type == 0x86DD) {
        W.w.ipv6++;
    } else {
        W.w.other++;
    }
}

void aos_eth_watch_get(aos_eth_watch_t *out, uint32_t now_ms)
{
    second_turn(now_ms / 1000);
    *out = W.w;
}

/* ---- the file ---- */

static bool put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };     /* little endian */
    return fwrite(b, 1, 4, f) == 4;
}

bool aos_eth_pcap_header(FILE *f)
{
    /* the magic, version 2.4, no time zone, no accuracy, the longest frame kept, Ethernet */
    return put32(f, 0xA1B2C3D4u) && put32(f, 0x00040002u) && put32(f, 0) && put32(f, 0) && put32(f, 1518) && put32(f, 1);
}

bool aos_eth_pcap_record(FILE *f, const uint8_t *frame, uint32_t kept, uint32_t whole, uint32_t sec, uint32_t usec)
{
    return put32(f, sec) && put32(f, usec) && put32(f, kept) && put32(f, whole) && fwrite(frame, 1, kept, f) == kept;
}
