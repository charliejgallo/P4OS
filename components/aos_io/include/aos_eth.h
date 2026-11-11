/*
 * P4OS - Ethernet over a wire: a W5500 on an SPI port of the header
 * (docs/plan/ETHERNET.md). The board has no Ethernet of its own, and the
 * module may not be there: it is off until switched on, and switching it on
 * with nothing connected says so and gives up.
 *
 * Where the chip is comes from modules.txt, "module w5500 spi.a" (its cs=
 * and rst= when they are not the port's); with no such line it is looked
 * for on spi.a with the port's own chip select. The SPI port is opened
 * through aos_io_spi_* under the owner "Ethernet", so the pin arbiter and
 * every app see it taken.
 *
 * While it has a link and an address the wire is the way out: its route
 * goes before the Wi-Fi's, and pulling the cable gives the route back.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AOS_ETH_OWNER       "Ethernet"
#define AOS_ETH_DEFAULT_HZ  8000000u    /* jumper wires on the header; a board with short tracks takes more */

typedef struct {
    bool     enabled;           /* the preference: it starts at boot */
    bool     running;           /* the driver is up: the chip answered */
    bool     link;              /* a cable with someone at the other end */
    int      speed_mbps;        /* 10 or 100; 0 with no link */
    bool     full_duplex;
    char     ip[16], mask[16], gw[16];      /* "" until DHCP answers */
    uint8_t  mac[6];
    char     port[16];          /* "spi.a" */
    int      cs;                /* the chip select's GPIO */
    uint32_t spi_hz;            /* as the hardware makes it */
    uint32_t link_s;            /* seconds the link has been up */
    uint32_t links;             /* times the link came up since it started */
    char     err[80];           /* why it is not running, in Spanish; "" when it is */
} aos_eth_info_t;

/* Reads the preference and starts it if it is on. Once, at boot. */
void aos_eth_boot(void);
/* Sets the preference and starts or stops it now. false when it was asked to
 * start and could not: aos_eth_info() says why. */
bool aos_eth_enable(bool on);
/* The SPI clock, kept as a preference; it takes effect at the next start. */
void aos_eth_set_clock(uint32_t hz);
void aos_eth_info(aos_eth_info_t *out);

#ifdef __cplusplus
}
#endif
