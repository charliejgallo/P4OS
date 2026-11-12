/* P4OS simulator - Ethernet (aos_eth.h). The simulated board has no W5500 on its header, so
 * switching it on says that; with P4_SIM_ETH=1 it has one, with a cable in and an address, for the
 * pages that show it (P4_SIM_ETH=nolink: the module and no cable).
 *
 * The cable goes to a made-up switch that announces itself three seconds after someone listens,
 * by LLDP (P4_SIM_ETH=cdp: by CDP; P4_SIM_ETH=quiet: a switch that says nothing). The announcement
 * is a real frame, built here and read by the same code as on the board (aos_eth_announce.c). */
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
        if (strcmp(e, "nolink")) {
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

void aos_eth_listen(bool on)
{
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

bool aos_eth_yield(const char *port, const char *to) { (void)port; (void)to; return false; }
void aos_eth_port_free(const char *port) { (void)port; }
void aos_eth_on_pause(aos_eth_notice_t cb) { (void)cb; }
