/*
 * P4OS - Luces: the universes out over the network, as Art-Net.
 *
 * Art-Net carries DMX in UDP (port 6454): an ArtDmx packet is a header
 * ("Art-Net", the op code 0x5000, the protocol version 14, a sequence
 * number, the port-address) and the 512 levels of one universe. Lighting
 * consoles, nodes that turn it back into DMX on an XLR, LED controllers and
 * the PC programs all speak it. Luces sends every universe that has a
 * fixture patched, 40 times a second, to the broadcast address of the
 * network or to one node; universe 1 of the patch goes out with
 * port-address "base", universe 2 with base + 1, and so on.
 *
 * A thread of its own: a send on the Wi-Fi may wait a few milliseconds for
 * the radio, and LVGL's thread should not. It copies the levels without a
 * lock: a packet with half of one frame and half of the next is what a
 * fader moving looks like anyway.
 */
#include "lz_ui.h"

#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

lz_net_t LZN;

static volatile bool s_stop, s_running;

static void net_main(void *arg)
{
    (void)arg;
    int h = 0;
    uint8_t seq = 0;
    static uint8_t pkt[18 + 512];
    while (!s_stop) {
        if (!LZN.on) {
            if (h > 0) { aos_hal_tcp_close(h); h = 0; }
            aos_hal_sleep_ms(100);
            continue;
        }
        if (h <= 0) {
            h = aos_hal_udp_open(0, NULL);
            if (h <= 0) {
                snprintf(LZN.err, sizeof LZN.err, "%s", "no UDP socket");
                h = 0;
                aos_hal_sleep_ms(1000);
                continue;
            }
            LZN.err[0] = 0;
        }
        seq = seq == 255 ? 1 : seq + 1;         /* 0 means "not sequenced" */
        for (int u = 0; u < LZ_UNIV; u++) {
            if (!(LZ.used_univ >> u & 1)) continue;
            uint16_t pa = (uint16_t)((LZN.base + u) & 0x7FFF);
            memcpy(pkt, "Art-Net", 8);
            pkt[8] = 0x00; pkt[9] = 0x50;       /* OpDmx, little-endian */
            pkt[10] = 0; pkt[11] = 14;          /* protocol version, big-endian */
            pkt[12] = seq;
            pkt[13] = 0;                        /* physical input */
            pkt[14] = (uint8_t)(pa & 0xFF);     /* SubUni */
            pkt[15] = (uint8_t)(pa >> 8);       /* Net */
            pkt[16] = 512 >> 8; pkt[17] = 512 & 0xFF;
            memcpy(pkt + 18, LZ.uni[u] + 1, 512);
            const char *dest = LZN.dest[0] ? LZN.dest : "255.255.255.255";
            if (aos_hal_udp_send(h, dest, 6454, pkt, sizeof pkt) == (int)sizeof pkt) LZN.packets++;
            else snprintf(LZN.err, sizeof LZN.err, "%s", "send failed");
        }
        aos_hal_sleep_ms(25);
    }
    if (h > 0) aos_hal_tcp_close(h);
    s_running = false;
}

void lz_net_start(void)
{
    if (s_running) return;
    s_stop = false;
    s_running = true;
    if (!aos_hal_thread_start("luces_net", net_main, NULL, 4096, 3)) s_running = false;
}

void lz_net_stop(void)
{
    s_stop = true;
    for (int i = 0; i < 60 && s_running; i++) aos_hal_sleep_ms(10);
}
