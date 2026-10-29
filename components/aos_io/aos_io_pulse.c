/*
 * P4OS - Pulses, counts and SWD on the header's pins (aos_io.h): the part
 * the board and the simulator share. It checks the pins and claims them for
 * the owner; the backends (aos_io_pulse_p4.c, sim/io_pulse_sim.c) time the
 * edges, clock the bits and count.
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool take(int gpio, const char *owner, const char *what)
{
    const aos_io_pin_t *p = aos_io_pin_of_gpio(gpio);
    if (!owner || !owner[0] || !p || (p->flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD))) {
        aos_hal_log("io", "%s: GPIO%d is not a usable pin of the header", what, gpio);
        return false;
    }
    return aos_io_claim(gpio, owner);
}

aos_io_edges_t *aos_io_edges_open(int gpio, int max, const char *owner)
{
    if (max < 1 || max > AOS_IO_EDGES_MAX || !take(gpio, owner, "edges")) return NULL;
    aos_io_edges_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->gpio = (int8_t)gpio;
    e->max = max;
    snprintf(e->owner, sizeof e->owner, "%s", owner);
    if (!aos_io_be_edges_open(e)) { free(e); return NULL; }
    return e;
}

void aos_io_edges_arm(aos_io_edges_t *e) { if (e) aos_io_be_edges_arm(e); }

int aos_io_edges_wait(aos_io_edges_t *e, uint32_t idle_us, uint32_t timeout_ms)
{
    return e ? aos_io_be_edges_wait(e, idle_us, timeout_ms) : -1;
}

int aos_io_edges_get(aos_io_edges_t *e, uint32_t *t_us, uint8_t *level, int max)
{
    return e && t_us && level && max > 0 ? aos_io_be_edges_get(e, t_us, level, max) : 0;
}

void aos_io_edges_close(aos_io_edges_t *e)
{
    if (!e) return;
    aos_io_be_edges_close(e);
    free(e);
}

static bool owned(int gpio, const char *owner)
{
    const char *o = aos_io_owner(gpio);
    return owner && o && !strcmp(o, owner);
}

bool aos_io_pulse_out(int gpio, int level, uint32_t us, const char *owner)
{
    if (!owned(gpio, owner) || us > 100000) return false;
    return aos_io_be_pulse_out(gpio, level, us);
}

bool aos_io_shift_in(int data, int clk, int bits, int extra, uint32_t *out, const char *owner)
{
    if (!out || bits < 1 || bits > 32 || extra < 0 || extra > 8) return false;
    if (!owned(data, owner) || !owned(clk, owner) || data == clk) return false;
    return aos_io_be_shift_in(data, clk, bits, extra, out);
}

aos_io_count_t *aos_io_count_open(int gpio, aos_gpio_mode_t pull, bool both_edges, uint32_t debounce_us, const char *owner)
{
    if (pull == AOS_GPIO_OUTPUT || !take(gpio, owner, "count")) return NULL;
    aos_io_count_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->gpio = (int8_t)gpio;
    c->both = both_edges;
    c->debounce_us = debounce_us;
    snprintf(c->owner, sizeof c->owner, "%s", owner);
    if (!aos_io_be_count_open(c, pull)) { free(c); return NULL; }
    return c;
}

uint32_t aos_io_count_get(aos_io_count_t *c) { return c ? aos_io_be_count_get(c) : 0; }

void aos_io_count_close(aos_io_count_t *c)
{
    if (!c) return;
    aos_io_be_count_close(c);
    free(c);
}

/* ---- SWD ---- */

aos_io_swd_t *aos_io_swd_open(int swclk, int swdio, int nrst, uint32_t khz, const char *owner)
{
    if (swclk == swdio || swclk == nrst || swdio == nrst) return NULL;
    if (!take(swclk, owner, "swd")) return NULL;
    if (!take(swdio, owner, "swd")) { aos_io_release(swclk, owner); return NULL; }
    if (nrst >= 0 && !take(nrst, owner, "swd")) {
        aos_io_release(swclk, owner);
        aos_io_release(swdio, owner);
        return NULL;
    }
    aos_io_swd_t *s = calloc(1, sizeof *s);
    if (s) {
        s->clk = (int8_t)swclk;
        s->dio = (int8_t)swdio;
        s->nrst = (int8_t)nrst;
        s->khz = khz ? khz : 1000;
        snprintf(s->owner, sizeof s->owner, "%s", owner);
    }
    if (!s || !aos_io_be_swd_open(s)) {
        aos_io_release(swclk, owner);
        aos_io_release(swdio, owner);
        if (nrst >= 0) aos_io_release(nrst, owner);
        free(s);
        return NULL;
    }
    return s;
}

bool aos_io_swd_seq(aos_io_swd_t *s, const uint8_t *bits, int n)
{
    return s && bits && n > 0 && aos_io_be_swd_seq(s, bits, n);
}

int aos_io_swd_xfer(aos_io_swd_t *s, uint8_t req, uint32_t *data, bool no_ack)
{
    return s && data ? aos_io_be_swd_xfer(s, req, data, no_ack) : 7;
}

bool aos_io_swd_nrst(aos_io_swd_t *s, bool assert)
{
    return s && s->nrst >= 0 && aos_io_be_swd_nrst(s, assert);
}

void aos_io_swd_close(aos_io_swd_t *s)
{
    if (!s) return;
    aos_io_be_swd_close(s);
    aos_io_release(s->clk, s->owner);
    aos_io_release(s->dio, s->owner);
    if (s->nrst >= 0) aos_io_release(s->nrst, s->owner);
    free(s);
}
