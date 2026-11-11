/* P4OS simulator - Ethernet (aos_eth.h): the simulated board has no W5500 on its header yet. */
#include "aos_eth.h"
#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

static aos_eth_info_t s_info;

void aos_eth_boot(void)
{
    int32_t on = 0;
    s_info.enabled = aos_hal_pref_get_i32("eth", &on) && on;
    if (s_info.enabled) aos_eth_enable(true);
}

bool aos_eth_enable(bool on)
{
    s_info.enabled = on;
    aos_hal_pref_set_i32("eth", on ? 1 : 0);
    snprintf(s_info.port, sizeof s_info.port, "spi.a");
    snprintf(s_info.err, sizeof s_info.err, "%s", on ? "no hay un W5500 que conteste en el puerto" : "");
    return !on;
}

void aos_eth_set_clock(uint32_t hz) { aos_hal_pref_set_i32("eth_hz", (int32_t)hz); }
void aos_eth_info(aos_eth_info_t *out) { *out = s_info; }
