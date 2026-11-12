/* P4OS simulator - Ethernet (aos_eth.h). The simulated board has no W5500 on its header, so
 * switching it on says that; with P4_SIM_ETH=1 it has one, with a cable in and an address, for the
 * pages that show it (P4_SIM_ETH=nolink: the module and no cable).
 *
 * The cable goes to a made-up switch that announces itself three seconds after someone listens,
 * by LLDP (P4_SIM_ETH=cdp: by CDP; P4_SIM_ETH=quiet: a switch that says nothing). The announcement
 * is a real frame, built here and read by the same code as on the board (aos_eth_announce.c).
 *
 * And somebody talks on the wire, for the tool that finds a device (aos_eth_stations.c), also
 * with real frames. On the made-up network: its router, a PC that looks for a name by NetBIOS and
 * a printer that answers by mDNS. With P4_SIM_ETH=device there is no network: the cable goes
 * straight to a camera that asks for an address by DHCP, gets none, settles on 192.168.1.64,
 * says so by ARP and says hello by WS-Discovery; it answers when asked for that address. */
#include "aos_eth.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static aos_eth_info_t s_info;
static bool s_read;

static void apply(bool on)
{
    const char *e = getenv("P4_SIM_ETH");
    s_info.enabled = on;
    s_info.mode = AOS_ETH_CLIENT;
    s_info.running = s_info.link = false;
    s_info.ip[0] = s_info.mask[0] = s_info.gw[0] = s_info.err[0] = 0;
    s_info.speed_mbps = 0;
    snprintf(s_info.port, sizeof s_info.port, "spi.a");
    s_info.cs = 49;
    if (on && e && e[0] && strcmp(e, "0")) {
        static const uint8_t MAC[6] = { 0x02, 0x50, 0x34, 0x4F, 0x53, 0x01 };
        memcpy(s_info.mac, MAC, 6);
        s_info.running = true;
        if (!s_info.spi_hz) s_info.spi_hz = AOS_ETH_DEFAULT_HZ;
        if (!strcmp(e, "device")) {             /* a device and no DHCP server: a link and no address */
            s_info.link = true;
            s_info.speed_mbps = 100;
            s_info.full_duplex = true;
        } else if (strcmp(e, "nolink")) {
            s_info.link = true;
            s_info.speed_mbps = 100;
            s_info.full_duplex = true;
            snprintf(s_info.ip, sizeof s_info.ip, "10.0.0.50");
            snprintf(s_info.mask, sizeof s_info.mask, "255.255.255.0");
            snprintf(s_info.gw, sizeof s_info.gw, "10.0.0.1");
        }
    } else if (on) {
        snprintf(s_info.err, sizeof s_info.err, "no hay un W5500 que conteste en el puerto");
    }
    aos_hal_net_wired_set(s_info.ip[0] != 0, s_info.ip);
}

static void read_once(void)
{
    if (s_read) return;
    s_read = true;
    int32_t on = 0, hz = 0;
    if (aos_hal_pref_get_i32("eth_hz", &hz) && hz > 0) s_info.spi_hz = (uint32_t)hz;
    apply(aos_hal_pref_get_i32("eth", &on) && on);
}

void aos_eth_boot(void) { read_once(); }

bool aos_eth_enable(bool on)
{
    s_read = true;
    aos_hal_pref_set_i32("eth", on ? 1 : 0);
    apply(on);
    return !on || s_info.running;
}

void aos_eth_set_clock(uint32_t hz)
{
    aos_hal_pref_set_i32("eth_hz", (int32_t)hz);
    s_info.spi_hz = hz;
}

void aos_eth_info(aos_eth_info_t *out)
{
    read_once();
    *out = s_info;
}

void aos_eth_dhcp(aos_eth_dhcp_t *out)
{
    read_once();
    memset(out, 0, sizeof *out);
    if (!s_info.ip[0]) return;
    out->valid = true;
    snprintf(out->server, sizeof out->server, "10.0.0.1");
    snprintf(out->dns1, sizeof out->dns1, "10.0.0.1");
    snprintf(out->dns2, sizeof out->dns2, "10.0.0.2");
    out->lease_s = 86400;
    out->age_s = (uint32_t)(aos_hal_uptime_ms() / 1000);
}

/* ---- the made-up switch ---- */

static bool s_listen;
static uint64_t s_listen_at, s_listen_seen;

#define B(s) s, sizeof(s) - 1            /* a string's bytes and how many, the NULs inside it too */

static size_t tlv(uint8_t *p, unsigned type, const void *v, size_t n)
{
    p[0] = (uint8_t)(type << 1 | (n >> 8 & 1));
    p[1] = (uint8_t)n;
    memcpy(p + 2, v, n);
    return n + 2;
}

static size_t lldp_frame(uint8_t *f)
{
    static const uint8_t HEAD[14] = { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E, 0x00, 0x1B, 0x54, 0xAA, 0x10, 0x07, 0x88, 0xCC };
    memcpy(f, HEAD, 14);
    size_t n = 14;
    n += tlv(f + n, 1, B("\x04\x00\x1B\x54\xAA\x10\x00"));          /* chassis: a MAC */
    n += tlv(f + n, 2, B("\x05Gi1/0/7"));                              /* port: its name */
    n += tlv(f + n, 3, B("\x00\x78"));                                 /* 120 s */
    n += tlv(f + n, 4, B("Banco 2, puesto 7"));
    n += tlv(f + n, 5, B("sw-taller"));
    n += tlv(f + n, 6, B("Conmutador de ejemplo 24G PoE, firmware 2.1 (simulador)"));
    n += tlv(f + n, 8, B("\x05\x01\x0A\x00\x00\x02\x02\x00\x00\x00\x01\x00"));  /* managed at 10.0.0.2 */
    n += tlv(f + n, 127, B("\x00\x80\xC2\x01\x00\x14"));              /* untagged VLAN 20 */
    n += tlv(f + n, 127, B("\x00\x12\x0F\x02\x07\x01\x00"));          /* a power source, switched on */
    n += tlv(f + n, 0, B(""));
    return n;
}

static size_t cdp_tlv(uint8_t *p, unsigned type, const void *v, size_t n)
{
    p[0] = (uint8_t)(type >> 8);
    p[1] = (uint8_t)type;
    p[2] = (uint8_t)((n + 4) >> 8);
    p[3] = (uint8_t)(n + 4);
    memcpy(p + 4, v, n);
    return n + 4;
}

static size_t cdp_frame(uint8_t *f)
{
    static const uint8_t HEAD[26] = { 0x01, 0x00, 0x0C, 0xCC, 0xCC, 0xCC, 0x00, 0x1B, 0x54, 0xAA, 0x10, 0x07, 0x00, 0x00,
                                      0xAA, 0xAA, 0x03, 0x00, 0x00, 0x0C, 0x20, 0x00, 0x02, 0xB4, 0x00, 0x00 };
    memcpy(f, HEAD, 26);
    size_t n = 26;
    n += cdp_tlv(f + n, 0x0001, B("sw-taller.ejemplo"));
    n += cdp_tlv(f + n, 0x0003, B("FastEthernet0/7"));
    n += cdp_tlv(f + n, 0x0006, B("Conmutador de ejemplo 2960 (simulador)"));
    n += cdp_tlv(f + n, 0x000A, B("\x00\x14"));
    return n;
}

/* ---- the stations on the made-up wire ---- */

static const uint8_t ALL[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t SELF[6] = { 0x02, 0x50, 0x34, 0x4F, 0x53, 0x01 };
static unsigned s_said;                 /* which of the made-up frames went out already */
static bool s_asked_cam, s_asked_router;

static size_t arp_frame(uint8_t *f, const uint8_t *src, const uint8_t *dst, int op, const uint8_t *spa, const uint8_t *tpa)
{
    memset(f, 0, 60);
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    static const uint8_t HEAD[9] = { 0x08, 0x06, 0x00, 0x01, 0x08, 0x00, 6, 4, 0x00 };
    memcpy(f + 12, HEAD, 9);
    f[21] = (uint8_t)op;
    memcpy(f + 22, src, 6);
    memcpy(f + 28, spa, 4);
    if (op == 2) memcpy(f + 32, dst, 6);
    memcpy(f + 38, tpa, 4);
    return 60;
}

static size_t udp_frame(uint8_t *f, const uint8_t *src, const uint8_t *dst, const uint8_t *sip, const uint8_t *dip,
                        unsigned sport, unsigned dport, const void *payload, size_t n)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    f[12] = 0x08;
    f[13] = 0x00;
    uint8_t *ip = f + 14;
    memset(ip, 0, 28);
    ip[0] = 0x45;
    ip[2] = (uint8_t)((28 + n) >> 8);
    ip[3] = (uint8_t)(28 + n);
    ip[8] = 64;
    ip[9] = 17;
    memcpy(ip + 12, sip, 4);
    memcpy(ip + 16, dip, 4);
    uint8_t *u = ip + 20;
    u[0] = (uint8_t)(sport >> 8);
    u[1] = (uint8_t)sport;
    u[2] = (uint8_t)(dport >> 8);
    u[3] = (uint8_t)dport;
    u[4] = (uint8_t)((8 + n) >> 8);
    u[5] = (uint8_t)(8 + n);
    memcpy(u + 8, payload, n);
    return 42 + n;
}

/* What the made-up stations say, each frame once, as the time since listening began goes by. */
static void stations_talk(uint32_t now)
{
    const char *e = getenv("P4_SIM_ETH");
    uint32_t t = now - (uint32_t)s_listen_at;
    static uint8_t f[700];
    size_t n;
    if (e && !strcmp(e, "device")) {
        static const uint8_t CAM[6] = { 0x02, 0xCA, 0x4D, 0x7C, 0x2A, 0x11 }, NONE[4] = { 0, 0, 0, 0 }, BCAST[4] = { 255, 255, 255, 255 };
        static const uint8_t IP[4] = { 192, 168, 1, 64 }, WSD[4] = { 239, 255, 255, 250 }, WSD_MAC[6] = { 0x01, 0x00, 0x5E, 0x7F, 0xFF, 0xFA };
        if (t >= 1500 && !(s_said & 1)) {       /* a DHCP discover: its name, what it runs, the address it would like */
            s_said |= 1;
            static uint8_t b[300];
            memset(b, 0, sizeof b);
            b[0] = 1;
            b[1] = 1;
            b[2] = 6;
            memcpy(b + 28, CAM, 6);
            memcpy(b + 236, "\x63\x82\x53\x63", 4);
            size_t k = 240;
            memcpy(b + k, "\x35\x01\x01", 3); k += 3;
            memcpy(b + k, "\x0C\x0Aipcam-7c2a", 12); k += 12;
            memcpy(b + k, "\x3C\x0Cudhcp 1.24.2", 14); k += 14;
            memcpy(b + k, "\x32\x04\xC0\xA8\x01\x40", 6); k += 6;
            b[k++] = 255;
            n = udp_frame(f, CAM, ALL, NONE, BCAST, 68, 67, b, k);
            aos_eth_station_note(f, n, SELF, now);
        }
        if (t >= 4000 && !(s_said & 2)) {       /* nobody answered: its factory address, announced */
            s_said |= 2;
            n = arp_frame(f, CAM, ALL, 1, IP, IP);
            aos_eth_station_note(f, n, SELF, now);
        }
        if (t >= 5500 && !(s_said & 4)) {
            s_said |= 4;
            n = udp_frame(f, CAM, WSD_MAC, IP, WSD, 3702, 3702, B("<Hello/>"));
            aos_eth_station_note(f, n, SELF, now);
        }
        if (t >= 6500 && !(s_said & 16)) {      /* and announces itself by SSDP, with where its description is */
            s_said |= 16;
            static const uint8_t SSDP[4] = { 239, 255, 255, 250 };
            n = udp_frame(f, CAM, WSD_MAC, IP, SSDP, 1900, 1900,
                          B("NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nNT: upnp:rootdevice\r\n"
                            "SERVER: Linux/3.10 UPnP/1.0 camara-ejemplo/1.4\r\nLOCATION: http://192.168.1.64:49152/desc.xml\r\n\r\n"));
            aos_eth_station_note(f, n, SELF, now);
        }
        if (s_asked_cam && !(s_said & 8)) {
            s_said |= 8;
            n = arp_frame(f, CAM, SELF, 2, IP, NONE);
            aos_eth_station_note(f, n, SELF, now);
        }
        return;
    }
    static const uint8_t ROUTER[6] = { 0x02, 0x10, 0x00, 0x00, 0x00, 0x01 }, PC[6] = { 0x02, 0x10, 0x00, 0x00, 0x00, 0x17 };
    static const uint8_t PRN[6] = { 0x02, 0x10, 0x00, 0x00, 0x00, 0x1F };
    static const uint8_t R_IP[4] = { 10, 0, 0, 1 }, PC_IP[4] = { 10, 0, 0, 23 }, PRN_IP[4] = { 10, 0, 0, 31 }, NET_ALL[4] = { 10, 0, 0, 255 };
    static const uint8_t MDNS[4] = { 224, 0, 0, 251 }, MDNS_MAC[6] = { 0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB }, ME[4] = { 10, 0, 0, 50 };
    if (t >= 800 && !(s_said & 1)) {            /* the router looks for somebody */
        s_said |= 1;
        n = arp_frame(f, ROUTER, ALL, 1, R_IP, PC_IP);
        aos_eth_station_note(f, n, SELF, now);
    }
    if (t >= 2200 && !(s_said & 2)) {           /* a PC asks for a NetBIOS name, and gives its own away */
        s_said |= 2;
        uint8_t q[50] = { 0x12, 0x34, 0x01, 0x10, 0, 1, 0, 0, 0, 0, 0, 0, 32 };
        const char *name = "PC-TALLER      ";
        for (int i = 0; i < 16; i++) {
            uint8_t c = i < 15 ? (uint8_t)name[i] : 0;
            q[13 + 2 * i] = (uint8_t)('A' + (c >> 4));
            q[14 + 2 * i] = (uint8_t)('A' + (c & 15));
        }
        q[46] = 0x00;
        q[47] = 0x20;
        q[49] = 0x01;
        n = udp_frame(f, PC, ALL, PC_IP, NET_ALL, 137, 137, q, sizeof q);
        aos_eth_station_note(f, n, SELF, now);
    }
    if (t >= 3500 && !(s_said & 4)) {           /* a printer answers an mDNS question with its name */
        s_said |= 4;
        n = udp_frame(f, PRN, MDNS_MAC, PRN_IP, MDNS, 5353, 5353,
                      B("\x00\x00\x84\x00\x00\x00\x00\x01\x00\x00\x00\x00\x09impresora\x05local\x00\x00\x01\x80\x01\x00\x00\x00\x78\x00\x04\x0A\x00\x00\x1F"));
        aos_eth_station_note(f, n, SELF, now);
    }
    if (s_asked_router && !(s_said & 8)) {
        s_said |= 8;
        n = arp_frame(f, ROUTER, SELF, 2, R_IP, ME);
        aos_eth_station_note(f, n, SELF, now);
    }
}

int aos_eth_stations(aos_eth_station_t *out, int max)
{
    uint64_t now = aos_hal_uptime_ms();
    if (!s_listen || !s_info.link || now - s_listen_seen > 5000) return 0;
    stations_talk((uint32_t)now);
    return aos_eth_station_list(out, max, (uint32_t)now);
}

bool aos_eth_arp_ask(uint32_t ip)
{
    if (!s_info.link) return false;
    if (ip == 0xC0A80140u) s_asked_cam = true;          /* 192.168.1.64 */
    if (ip == 0x0A000001u) s_asked_router = true;       /* 10.0.0.1 */
    return true;
}

void aos_eth_listen(bool on)
{
    if (on && !s_listen) {
        aos_eth_station_clear();
        s_said = 0;
        s_asked_cam = s_asked_router = false;
    }
    if (on && !s_listen) s_listen_at = aos_hal_uptime_ms();
    s_listen = on;
    s_listen_seen = aos_hal_uptime_ms();
}

bool aos_eth_neighbor(aos_eth_neighbor_t *out)
{
    memset(out, 0, sizeof *out);
    const char *e = getenv("P4_SIM_ETH");
    uint64_t now = aos_hal_uptime_ms();
    if (s_listen && now - s_listen_seen > 5000) s_listen = false;      /* as on the board: it goes off by itself */
    if (!s_listen || !s_info.link || !e || !strcmp(e, "quiet") || now - s_listen_at < 3000) return false;
    uint8_t f[512];
    size_t n = !strcmp(e, "cdp") ? cdp_frame(f) : lldp_frame(f);
    if (!aos_eth_parse_announce(f, n, out)) return false;
    uint32_t every = !strcmp(e, "cdp") ? 60 : 30, since = (uint32_t)((now - s_listen_at - 3000) / 1000);
    out->age_s = since % every;
    out->heard = 1 + since / every;
    return true;
}

/* The bench network: the made-up wire takes the address it is given, and stops being "the network". */
bool aos_eth_bench(aos_eth_mode_t mode, const char *ip)
{
    read_once();
    if (mode == AOS_ETH_SERVER && (!ip || !ip[0])) ip = AOS_ETH_BENCH_IP;
    if (mode != AOS_ETH_CLIENT && (!ip || !ip[0])) return false;
    s_info.mode = mode;
    if (mode == AOS_ETH_CLIENT) {
        apply(s_info.enabled);
        return true;
    }
    if (s_info.running) {
        snprintf(s_info.ip, sizeof s_info.ip, "%s", ip);
        snprintf(s_info.mask, sizeof s_info.mask, "255.255.255.0");
        s_info.gw[0] = 0;
    }
    aos_hal_net_wired_set(false, NULL);
    return true;
}

bool aos_eth_yield(const char *port, const char *to) { (void)port; (void)to; return false; }
void aos_eth_port_free(const char *port) { (void)port; }
void aos_eth_on_pause(aos_eth_notice_t cb) { (void)cb; }
