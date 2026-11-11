/* P4OS simulator - Ethernet (aos_eth.h). The simulated board has no W5500 on its header, so
 * switching it on says that; with P4_SIM_ETH=1 it has one, with a cable in and an address, for the
 * pages that show it (P4_SIM_ETH=nolink: the module and no cable). */
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

bool aos_eth_yield(const char *port, const char *to) { (void)port; (void)to; return false; }
void aos_eth_port_free(const char *port) { (void)port; }
void aos_eth_on_pause(aos_eth_notice_t cb) { (void)cb; }
