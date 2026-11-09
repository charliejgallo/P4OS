/*
 * P4OS - DMX: the board as a network node. What a console, a PC program or
 * Luces on another board sends over the network as Art-Net or sACN (E1.31)
 * for one universe comes out of the RS485 port as DMX.
 *
 *   Art-Net  UDP 6454, ArtDmx packets; the universe is the 15-bit
 *            port-address. An ArtPoll gets an ArtPollReply, so a console
 *            lists the board among its nodes.
 *   sACN     UDP 5568, multicast to 239.255.<universe high>.<universe low>;
 *            universes 1-63999. Packets with the preview bit or a stream
 *            ending are skipped.
 *
 * A thread of its own receives; it keeps the newest levels in DX.net_uni
 * and counts. The app's timer (every 25 ms on the Red tab) copies them into
 * the universe the link sends: the link and its handle stay LVGL's thread's
 * alone, as in the rest of the app.
 */
#include "dx.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <string.h>

static volatile bool s_stop, s_running;
static void *s_mx;              /* the levels between the thread and the timer; made once */

static uint16_t get16be(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void art_poll_reply(int h, const char *to_ip)
{
    static uint8_t r[239];
    memset(r, 0, sizeof r);
    memcpy(r, "Art-Net", 8);
    r[8] = 0x00; r[9] = 0x21;                   /* OpPollReply */
    const char *ip = aos_hal_net_ip();
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (ip) sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d);
    r[10] = (uint8_t)a; r[11] = (uint8_t)b; r[12] = (uint8_t)c; r[13] = (uint8_t)d;
    r[14] = 0x36; r[15] = 0x19;                 /* port 6454, little-endian */
    r[16] = 0; r[17] = 1;                       /* firmware version */
    uint16_t pa = DX.net_univ & 0x7FFF;
    r[18] = (uint8_t)(pa >> 8);                 /* NetSwitch */
    r[19] = (uint8_t)(pa >> 4 & 0x0F);          /* SubSwitch */
    r[20] = 0xFF; r[21] = 0xFF;                 /* OEM: unknown */
    r[23] = 0xD2;                               /* Status1: indicators normal, port-address set by the front panel */
    snprintf((char *)r + 26, 18, "P4OS DMX");
    snprintf((char *)r + 44, 64, "P4OS DMX node, universe %u to RS485 %s", (unsigned)pa, DX.port);
    snprintf((char *)r + 108, 64, "#0001 [%04u] %s", (unsigned)(DX.net_packets % 10000), DX.net_packets ? "receiving" : "waiting");
    r[172] = 0; r[173] = 1;                     /* one port */
    r[174] = 0x80;                              /* it outputs DMX512 */
    r[182] = DX.net_packets ? 0x80 : 0x00;      /* GoodOutput: data being sent */
    r[190] = (uint8_t)(pa & 0x0F);              /* SwOut */
    r[200] = 0x00;                              /* Style: StNode */
    aos_hal_udp_send(h, to_ip, 6454, r, sizeof r);
}

static void take_levels(const uint8_t *lv, int n, const char *from)
{
    if (n > 512) n = 512;
    if (n < 0) n = 0;
    aos_hal_mutex_lock(s_mx);
    memcpy(DX.net_uni + 1, lv, (size_t)n);
    if (n < 512) memset(DX.net_uni + 1 + n, 0, (size_t)(512 - n));
    DX.net_slots = (uint16_t)n;
    DX.net_packets++;
    DX.net_ms = (uint32_t)aos_hal_uptime_ms();
    snprintf(DX.net_from, sizeof DX.net_from, "%.23s", from);
    DX.net_fresh = true;
    aos_hal_mutex_unlock(s_mx);
}

static void net_main(void *arg)
{
    (void)arg;
    int h = 0;
    int proto = -1;
    uint16_t univ = 0;
    static uint8_t pkt[700];
    char from[32];
    while (!s_stop) {
        if (!DX.net_on) {
            if (h > 0) { aos_hal_tcp_close(h); h = 0; }
            aos_hal_sleep_ms(100);
            continue;
        }
        /* (re)open when the protocol or, for sACN, the universe changes */
        if (h > 0 && (proto != DX.net_proto || (proto == DX_NET_SACN && univ != DX.net_univ))) { aos_hal_tcp_close(h); h = 0; }
        if (h <= 0) {
            proto = DX.net_proto;
            univ = DX.net_univ;
            char group[20] = "";
            if (proto == DX_NET_SACN) snprintf(group, sizeof group, "239.255.%u.%u", (unsigned)(univ >> 8), (unsigned)(univ & 0xFF));
            h = aos_hal_udp_open(proto == DX_NET_SACN ? 5568 : 6454, group[0] ? group : NULL);
            if (h <= 0) {
                snprintf(DX.net_err, sizeof DX.net_err, _("no se pudo escuchar el puerto %u"), proto == DX_NET_SACN ? 5568u : 6454u);
                h = 0;
                aos_hal_sleep_ms(1000);
                continue;
            }
            DX.net_err[0] = 0;
        }
        int n = aos_hal_udp_recv(h, pkt, sizeof pkt, 100, from, sizeof from);
        if (n <= 0) continue;
        char *colon = strchr(from, ':');
        if (colon) *colon = 0;
        if (proto == DX_NET_ARTNET) {
            if (n < 12 || memcmp(pkt, "Art-Net", 8)) continue;
            uint16_t op = (uint16_t)(pkt[8] | pkt[9] << 8);
            if (op == 0x2000) { art_poll_reply(h, from); DX.net_polls++; continue; }
            if (op != 0x5000 || n < 18) continue;
            uint16_t pa = (uint16_t)(pkt[14] | (pkt[15] & 0x7F) << 8);
            if (pa != DX.net_univ) { DX.net_other++; continue; }
            int len = get16be(pkt + 16);
            if (len > n - 18) len = n - 18;
            take_levels(pkt + 18, len, from);
        } else {
            if (n < 126 || memcmp(pkt + 4, "ASC-E1.17\0\0\0", 12)) continue;
            if (get16be(pkt + 113) != DX.net_univ) { DX.net_other++; continue; }
            if (pkt[112] & 0x40) continue;              /* stream terminated */
            if (pkt[112] & 0x80) continue;              /* preview data: not for output */
            if (pkt[125] != 0) continue;                /* a start code other than levels */
            int count = get16be(pkt + 123) - 1;
            if (count > n - 126) count = n - 126;
            take_levels(pkt + 126, count, from);
        }
    }
    if (h > 0) aos_hal_tcp_close(h);
    s_running = false;
}

void dx_net_start(void)
{
    if (s_running) return;
    if (!s_mx) s_mx = aos_hal_mutex_create();
    s_stop = false;
    s_running = true;
    if (!aos_hal_thread_start("dmx_net", net_main, NULL, 4096, 4)) s_running = false;
}

void dx_net_stop(void)
{
    s_stop = true;
    for (int i = 0; i < 60 && s_running; i++) aos_hal_sleep_ms(10);
}

/* From the app's timer: the newest levels into what the link sends. */
bool dx_net_take(void)
{
    if (!s_mx) return false;
    aos_hal_mutex_lock(s_mx);
    bool fresh = DX.net_fresh;
    if (fresh) memcpy(DX.uni + 1, DX.net_uni + 1, 512);
    DX.net_fresh = false;
    aos_hal_mutex_unlock(s_mx);
    return fresh;
}
