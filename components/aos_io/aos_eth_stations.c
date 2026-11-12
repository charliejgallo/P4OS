/*
 * P4OS - Ethernet: who is talking on the wire (aos_eth.h).
 *
 * A device of unknown address, plugged straight into the board, gives itself
 * away as it starts: it asks for an address by DHCP with its name, announces
 * the address it settles on by ARP, looks for its neighbours by mDNS, SSDP or
 * NetBIOS, a camera says hello by WS-Discovery. All of that is sent to
 * everyone, so listening is enough. This file keeps a table of the stations
 * heard, from the frames the platform hands it (aos_eth_p4.c, sim/eth_sim.c):
 * it does not touch the network, and it believes no length in a frame.
 *
 * Whose address is it? A frame's source MAC is always the station's, but the
 * source IP of a routed packet is somebody far away. So an address is taken
 * from ARP, which speaks for the sender itself, and from IP packets sent to
 * everyone on the wire (broadcast or multicast), which no router passes on.
 *
 * One table, no lock: the caller keeps its calls apart.
 */
#include "aos_eth.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    aos_eth_station_t s;
    uint32_t last_ms, first_ms;
    bool used;
} slot_t;

static slot_t s_tab[AOS_ETH_STATIONS];

void aos_eth_station_clear(void) { memset(s_tab, 0, sizeof s_tab); }

static void put_text(char *out, size_t n, const uint8_t *p, size_t len)
{
    size_t k = 0;
    for (size_t i = 0; i < len && k + 1 < n; i++) {
        uint8_t c = p[i];
        if (c == 0) break;
        out[k++] = c >= 32 && c < 127 ? (char)c : '.';
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

static void put_ip(char *out, size_t n, const uint8_t *p)
{
    snprintf(out, n, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
}

static slot_t *slot_for(const uint8_t *mac, uint32_t now)
{
    slot_t *oldest = &s_tab[0];
    for (int i = 0; i < AOS_ETH_STATIONS; i++) {
        if (s_tab[i].used && !memcmp(s_tab[i].s.mac, mac, 6)) return &s_tab[i];
        if (!s_tab[i].used) { oldest = &s_tab[i]; break; }
        if ((int32_t)(s_tab[i].last_ms - oldest->last_ms) < 0) oldest = &s_tab[i];
    }
    memset(oldest, 0, sizeof *oldest);          /* a free one, or the one silent the longest */
    oldest->used = true;
    memcpy(oldest->s.mac, mac, 6);
    oldest->last_ms = oldest->first_ms = now;
    return oldest;
}

/* The options of a DHCP request: the name it goes by, what it says it is, the address it wants. */
static void dhcp_options(aos_eth_station_t *s, const uint8_t *p, size_t len)
{
    if (len < 240 || p[236] != 0x63 || p[237] != 0x82 || p[238] != 0x53 || p[239] != 0x63) return;
    p += 240;
    len -= 240;
    while (len >= 1 && p[0] != 255) {
        if (p[0] == 0) { p++; len--; continue; }           /* padding */
        if (len < 2 || (size_t)p[1] + 2 > len) return;
        const uint8_t *v = p + 2;
        size_t n = p[1];
        if (p[0] == 12 && n) put_text(s->name, sizeof s->name, v, n);
        else if (p[0] == 60 && n) put_text(s->vendor, sizeof s->vendor, v, n);
        else if (p[0] == 50 && n == 4) put_ip(s->wants, sizeof s->wants, v);
        p += n + 2;
        len -= n + 2;
    }
}

/* A NetBIOS name as it goes on the wire: 32 letters, two for each of its 16 bytes. */
static void nbns_name(aos_eth_station_t *s, const uint8_t *p, size_t len)
{
    if (len < 12 + 34 || p[12] != 32 || s->name[0]) return;
    char name[16];
    for (int i = 0; i < 15; i++) {
        uint8_t a = p[13 + 2 * i], b = p[14 + 2 * i];
        if (a < 'A' || a > 'P' || b < 'A' || b > 'P') return;
        name[i] = (char)((a - 'A') << 4 | (b - 'A'));
    }
    name[15] = 0;
    if (name[0] == '*' || (uint8_t)name[0] < 33) return;   /* a question for everybody, not a name */
    put_text(s->name, sizeof s->name, (const uint8_t *)name, 15);
}

/* An mDNS answer's first name, when it is a host's own: "camara.local". */
static void mdns_name(aos_eth_station_t *s, const uint8_t *p, size_t len)
{
    if (len < 13 || !(p[2] & 0x80) || s->name[0]) return;  /* a question says what is looked for, not who asks */
    char name[sizeof s->name];
    size_t k = 0, at = 12;
    while (at < len && p[at] && p[at] < 64) {
        size_t n = p[at];
        if (at + 1 + n > len) return;
        if (p[at + 1] == '_') return;                       /* a service, not a host */
        if (k && k + 1 < sizeof name) name[k++] = '.';
        for (size_t i = 0; i < n && k + 1 < sizeof name; i++) {
            uint8_t c = p[at + 1 + i];
            name[k++] = c >= 33 && c < 127 ? (char)c : '.';
        }
        at += 1 + n;
    }
    name[k] = 0;
    if (k > 6 && !strcmp(name + k - 6, ".local")) snprintf(s->name, sizeof s->name, "%s", name);
}

/* A line of an SSDP announcement, "SERVER: ..." or "LOCATION: ...": its value, when out is still empty. */
static void ssdp_line(char *out, size_t n, const char *key, const uint8_t *p, size_t len)
{
    size_t kn = strlen(key);
    if (out[0]) return;
    for (size_t i = 0; i + kn + 1 < len; i++) {
        if (i && p[i - 1] != '\n') continue;
        size_t k = 0;
        while (k < kn && (p[i + k] | 32) == key[k]) k++;
        if (k < kn || p[i + kn] != ':') continue;
        size_t a = i + kn + 1, b;
        while (a < len && p[a] == ' ') a++;
        for (b = a; b < len && p[b] != '\r' && p[b] != '\n'; b++) {}
        put_text(out, n, p + a, b - a);
        return;
    }
}

void aos_eth_station_note(const uint8_t *f, size_t len, const uint8_t *self, uint32_t now_ms)
{
    if (!f || len < 14 || (f[6] & 1) || (self && !memcmp(f + 6, self, 6))) return;     /* no real sender, or us */
    static const uint8_t ZERO[6];
    if (!memcmp(f + 6, ZERO, 6)) return;
    slot_t *t = slot_for(f + 6, now_ms);
    aos_eth_station_t *s = &t->s;
    t->last_ms = now_ms;
    s->frames++;
    bool to_all = f[0] & 1;
    size_t at = 12;
    if (len >= 18 && f[at] == 0x81 && f[at + 1] == 0x00) at += 4;      /* a VLAN tag */
    unsigned type = f[at] << 8 | f[at + 1];
    at += 2;
    if (type == 0x0806 && len >= at + 28) {
        s->said |= AOS_ETH_SAID_ARP;
        const uint8_t *spa = f + at + 14;
        if (spa[0] | spa[1] | spa[2] | spa[3]) put_ip(s->ip, sizeof s->ip, spa);
        if (f[at + 7] == 2 && self && !memcmp(f + at + 18, self, 6)) s->answered = true;   /* a reply, to us */
    } else if (type == 0x0800 && len >= at + 20) {
        size_t ihl = (size_t)(f[at] & 15) * 4;
        const uint8_t *src = f + at + 12;
        if (to_all && (src[0] | src[1] | src[2] | src[3])) put_ip(s->ip, sizeof s->ip, src);
        if (f[at + 9] == 17 && ihl >= 20 && len >= at + ihl + 8) {
            const uint8_t *u = f + at + ihl;
            unsigned sport = u[0] << 8 | u[1], dport = u[2] << 8 | u[3];
            const uint8_t *p = u + 8;
            size_t n = len - at - ihl - 8;
            if (dport == 67 && sport == 68) {
                s->said |= AOS_ETH_SAID_DHCP;
                s->wants_dhcp = true;
                dhcp_options(s, p, n);
            } else if (dport == 5353) {
                s->said |= AOS_ETH_SAID_MDNS;
                mdns_name(s, p, n);
            } else if (dport == 1900) {
                s->said |= AOS_ETH_SAID_SSDP;
                ssdp_line(s->vendor, sizeof s->vendor, "server", p, n);
                ssdp_line(s->location, sizeof s->location, "location", p, n);
            } else if (dport == 137) {
                s->said |= AOS_ETH_SAID_NBNS;
                nbns_name(s, p, n);
            } else if (dport == 3702) {
                s->said |= AOS_ETH_SAID_WSD;
            } else if (dport == 5355) {
                s->said |= AOS_ETH_SAID_LLMNR;
            } else {
                s->said |= AOS_ETH_SAID_OTHER;
            }
        } else {
            s->said |= AOS_ETH_SAID_OTHER;
        }
    } else if (type == 0x86DD) {
        s->said |= AOS_ETH_SAID_IPV6;
    } else if (type == 0x88CC || type <= 1500) {            /* LLDP; or a length, as spanning tree and CDP use */
        s->said |= AOS_ETH_SAID_SWITCH;
    } else {
        s->said |= AOS_ETH_SAID_OTHER;
    }
}

int aos_eth_station_list(aos_eth_station_t *out, int max, uint32_t now_ms)
{
    int n = 0;
    for (int i = 0; i < AOS_ETH_STATIONS && n < max; i++) {
        if (!s_tab[i].used) continue;
        out[n] = s_tab[i].s;
        out[n].age_s = (now_ms - s_tab[i].last_ms) / 1000;
        out[n].since_s = (now_ms - s_tab[i].first_ms) / 1000;
        n++;
    }
    return n;
}
