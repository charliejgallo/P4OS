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
 *
 * The SPI port is not lost to the bench. The sensor service reads a chip of
 * its own on the same port with Ethernet running (a thermocouple: two bytes
 * a second). An app that opens the port - Bus, EEPROM, a radio, a card
 * reader - gets it whole: Ethernet stops for as long as the app has the
 * port, says so, and comes back by itself a few seconds after the port was
 * last let go (an app opens and closes it for every transfer). A long
 * or slow transfer on shared wires would starve it anyway, and a bench tool
 * must not have to wonder.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <stddef.h>

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
    bool     paused;            /* stopped while an app has its SPI port; back when the port is free */
    char     paused_by[24];     /* that app, as it named itself to the pin arbiter */
} aos_eth_info_t;

/* Reads the preference and starts it if it is on. Once, at boot. */
void aos_eth_boot(void);
/* Sets the preference and starts or stops it now. false when it was asked to
 * start and could not: aos_eth_info() says why. */
bool aos_eth_enable(bool on);
/* The SPI clock, kept as a preference; it takes effect at the next start. */
void aos_eth_set_clock(uint32_t hz);
void aos_eth_info(aos_eth_info_t *out);

/* ---- for the tools that test a network port (Red, Cable) ---- */

/* What the DHCP server gave with the address. */
typedef struct {
    bool     valid;             /* the wire has an address */
    char     server[16];        /* who gave it; "" when it was not DHCP */
    char     dns1[16], dns2[16];
    uint32_t lease_s;           /* for how long it was given */
    uint32_t age_s;             /* how long ago */
} aos_eth_dhcp_t;
void aos_eth_dhcp(aos_eth_dhcp_t *out);

/* The switch at the other end of the cable, as it announces itself: LLDP,
 * or Cisco's CDP. A switch that is not managed says nothing. */
typedef struct {
    char     proto[8];          /* "LLDP" or "CDP" */
    char     system[64];        /* the switch's name */
    char     port[48];          /* this port, as the switch calls it */
    char     port_desc[64];     /* and as whoever set it up described it */
    char     descr[96];         /* the model and its software */
    char     chassis[32];       /* its MAC or identifier */
    char     mgmt[40];          /* where it is managed from */
    int      vlan;              /* the port's untagged VLAN; 0 not said */
    int      poe;               /* 1 the port offers power, 0 it does not, -1 not said */
    uint32_t ttl_s;             /* for how long the switch says this holds */
    uint32_t age_s;             /* since the last announcement */
    uint32_t heard;             /* announcements since listening began */
} aos_eth_neighbor_t;

/* Listening for the announcement. The chip then passes on every frame on
 * the wire, not only the board's, so it is on only while a tool asks: call
 * it again every second or two, it switches itself off five seconds after
 * the last call. Turning it on forgets what was heard before. */
void aos_eth_listen(bool on);
/* false: nothing heard since listening began. */
bool aos_eth_neighbor(aos_eth_neighbor_t *out);
/* Reads a whole Ethernet frame: true when it is an announcement, with what
 * it says (age_s and heard left at 0). aos_eth_announce.c. */
bool aos_eth_parse_announce(const uint8_t *frame, size_t len, aos_eth_neighbor_t *out);

/* For the SPI service (aos_io.c). An app asks for a port: if Ethernet is on
 * it, it stops and lets it go (true), and if it was already waiting for that
 * port, it goes on waiting. And a port has nobody on it any more: a paused
 * Ethernet counts a few seconds from here and starts again. */
bool aos_eth_yield(const char *port, const char *to);
void aos_eth_port_free(const char *port);
/* Told when Ethernet pauses for an app and when it is back, to say so on the
 * screen. It runs in the thread that opened or closed the port. */
typedef void (*aos_eth_notice_t)(bool paused, const char *port, const char *by);
void aos_eth_on_pause(aos_eth_notice_t cb);

#ifdef __cplusplus
}
#endif
