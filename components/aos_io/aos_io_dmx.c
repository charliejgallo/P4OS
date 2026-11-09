/*
 * P4OS - DMX512 on a UART port of the header (aos_io.h): the part the board
 * and the simulator share. It finds the port, claims its pins for the
 * owner, keeps the universe to send and the rate; the backends
 * (aos_io_dmx_p4.c, sim/io_dmx_sim.c) move the bytes.
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* one lock for every handle: the HAL has no way to free a mutex, and a
 * handle a minute does not need one of its own */
static void *s_mx;

static void release(const aos_io_port_t *p, const char *owner, int upto)
{
    for (int k = 0; k < upto && k < 3; k++) if (p->pins[k] >= 0) aos_io_release(p->pins[k], owner);
}

aos_io_dmx_t *aos_io_dmx_open(const char *port, aos_dmx_dir_t dir, const char *owner)
{
    if (!owner || !owner[0] || dir > AOS_DMX_RECEIVE) return NULL;
    const aos_io_port_t *p = aos_io_port_find(port);
    if (!p || p->kind != AOS_PORT_UART) { aos_hal_log("io", "DMX: no UART port %s", port ? port : "?"); return NULL; }
    /* TX, RX and DE (if the port has one) */
    for (int k = 0; k < 3; k++) {
        if (p->pins[k] < 0) continue;
        if (!aos_io_claim(p->pins[k], owner)) {
            aos_hal_log("io", "DMX: GPIO%d is %s's", p->pins[k], aos_io_owner(p->pins[k]) ? aos_io_owner(p->pins[k]) : "taken");
            release(p, owner, k);
            return NULL;
        }
    }
    aos_io_dmx_t *d = calloc(1, sizeof *d);
    if (!d) { release(p, owner, 3); return NULL; }
    d->port = p;
    d->dir = dir;
    snprintf(d->owner, sizeof d->owner, "%s", owner);
    d->cfg = (aos_dmx_cfg_t){ .slots = 512, .break_us = 176, .mab_us = 16, .period_ms = 25, .start_code = 0 };
    if (!s_mx) s_mx = aos_hal_mutex_create();
    d->mx = s_mx;
    d->hz_ms = (uint32_t)aos_hal_uptime_ms();
    if (!d->mx || !aos_io_be_dmx_open(d)) {
        release(p, owner, 3);
        free(d);
        return NULL;
    }
    aos_hal_log("io", "DMX %s on %s (TX%d RX%d DE%d)", dir == AOS_DMX_SEND ? "send" : "receive", p->name, p->pins[0], p->pins[1], p->pins[2]);
    return d;
}

bool aos_io_dmx_config(aos_io_dmx_t *d, const aos_dmx_cfg_t *c)
{
    if (!d || !c) return false;
    aos_dmx_cfg_t n = *c;
    if (!n.slots || n.slots > 512) n.slots = 512;
    if (!n.break_us) n.break_us = 176;
    if (n.break_us < 88) n.break_us = 88;
    if (n.break_us > 2000) n.break_us = 2000;
    if (!n.mab_us) n.mab_us = 16;
    if (n.mab_us < 8) n.mab_us = 8;
    if (n.mab_us > 1000) n.mab_us = 1000;
    if (!n.period_ms) n.period_ms = 25;
    if (n.period_ms > 1000) n.period_ms = 1000;
    aos_hal_mutex_lock(d->mx);
    d->cfg = n;
    d->tx[0] = n.start_code;
    aos_hal_mutex_unlock(d->mx);
    return true;
}

void aos_io_dmx_write(aos_io_dmx_t *d, int first, const uint8_t *v, int n)
{
    if (!d || !v || first < 1 || first > 512 || n <= 0) return;
    if (first + n - 1 > 512) n = 512 - first + 1;
    aos_hal_mutex_lock(d->mx);
    memcpy(d->tx + first, v, (size_t)n);
    aos_hal_mutex_unlock(d->mx);
}

int aos_io_dmx_read(aos_io_dmx_t *d, uint8_t *out, int max, aos_dmx_stats_t *st)
{
    if (!d) return 0;
    aos_dmx_stats_t s;
    memset(&s, 0, sizeof s);
    int n = aos_io_be_dmx_read(d, out, max, &s);
    /* the rate, over about a second */
    uint32_t now = (uint32_t)aos_hal_uptime_ms(), dt = now - d->hz_ms;
    if (dt >= 1000) {
        d->hz = (float)(s.packets - d->hz_packets) * 1000.0f / (float)dt;
        d->hz_packets = s.packets;
        d->hz_ms = now;
    }
    s.hz = s.age_ms > 2000 ? 0.0f : d->hz;
    if (st) *st = s;
    return n;
}

int aos_io_dmx_rdm(aos_io_dmx_t *d, const uint8_t *req, int len, uint8_t *out, int max, int timeout_ms)
{
    if (!d || d->dir != AOS_DMX_SEND || !req || len < 3 || len > 257 || !out || max <= 0) return -1;
    return aos_io_be_dmx_rdm(d, req, len, out, max, timeout_ms > 0 ? timeout_ms : 3);
}

void aos_io_dmx_close(aos_io_dmx_t *d)
{
    if (!d) return;
    aos_io_be_dmx_close(d);
    release(d->port, d->owner, 3);
    free(d);
}
