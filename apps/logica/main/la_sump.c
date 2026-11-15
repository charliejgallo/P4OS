/*
 * P4OS - Logic: analysers that speak SUMP over a serial port.
 *
 * SUMP is the protocol of the Open Bench Logic Sniffer, which many small
 * analysers copied because PulseView ("Openbench Logic Sniffer & SUMP
 * compatibles") and sigrok-cli already speak it: the app's own firmware
 * for an ESP32 (firmware/ in this app), an OLS, analysers built on a Pico
 * or an FPGA. The device captures into its own memory and sends it all
 * when the capture ends: the depth is the device's, the speed too.
 *
 * Commands are one byte, or five (the byte and a 32-bit value, little end
 * first) when bit 7 is set:
 *
 *   0x00 reset (sent five times: a long command half sent is flushed)
 *   0x01 arm      0x02 id ("1ALS")      0x04 metadata
 *   0xC0 / C1 / C2  stage 0's trigger mask, values and configuration
 *                   (bit 27: start the capture; only stage 0 is used)
 *   0x80 divider: rate = 100 MHz / (divider + 1)
 *   0x81 read and delay counts, in fours less one, 16 bits each
 *   0x82 flags: bits 2..5 switch channel groups off (0x38: only 0..7,
 *        one byte per sample)
 *   0x83 / 0x84 delay and read counts in fours, 32 bits (for more than
 *        256K samples; the app's firmware and some others)
 *   0xF0 (the app's firmware only, by its name "P4OS..."): rising and
 *        falling edge masks for the trigger, which SUMP lacks
 *
 * The samples come back newest first; the trigger is at read - delay.
 * Metadata is (key, value) pairs ending in 0: keys 0x00-0x1F carry a
 * string, 0x20-0x3F a 32-bit number (big end first), 0x40-0x5F a byte.
 */
#include "la.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OWNER "Lógica"
#define SUMP_CLOCK 100000000u

static void put_long(aos_io_uart_t *u, uint8_t cmd, uint32_t v)
{
    uint8_t b[5] = { cmd, (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    aos_io_uart_write(u, b, 5);
}

static void put_reset(aos_io_uart_t *u)
{
    static const uint8_t z[5] = { 0 };
    aos_io_uart_write(u, z, 5);
}

static void drain(aos_io_uart_t *u)
{
    uint8_t b[64];
    for (int i = 0; i < 40 && aos_io_uart_read(u, b, sizeof b, 30) > 0; i++) {}
}

/* reads exactly n bytes, or fewer if nothing comes for timeout_ms */
static int read_n(aos_io_uart_t *u, uint8_t *p, int n, int timeout_ms)
{
    int got = 0, idle = 0;
    while (got < n && idle < timeout_ms) {
        int r = aos_io_uart_read(u, p + got, n - got, 20);
        if (r > 0) {
            got += r;
            idle = 0;
        } else {
            if (r < 0) break;
            idle += 20;
        }
    }
    return got;
}

typedef struct {
    char name[40];
    uint32_t probes, mem, max_rate, proto;
} sump_meta_t;

static void meta_read(aos_io_uart_t *u, sump_meta_t *m)
{
    memset(m, 0, sizeof *m);
    uint8_t c = 0x04;
    aos_io_uart_write(u, &c, 1);
    for (int guard = 0; guard < 64; guard++) {
        uint8_t key;
        if (read_n(u, &key, 1, 300) != 1 || key == 0) return;
        int type = key >> 5;
        if (type == 0) {
            char s[64];
            int k = 0;
            uint8_t ch;
            while (read_n(u, &ch, 1, 300) == 1 && ch) {
                if (k < (int)sizeof s - 1) s[k++] = (char)ch;
            }
            s[k] = 0;
            if (key == 0x01) snprintf(m->name, sizeof m->name, "%.39s", s);
        } else if (type == 1) {
            uint8_t b[4];
            if (read_n(u, b, 4, 300) != 4) return;
            uint32_t v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
            if (key == 0x20) m->probes = v;
            else if (key == 0x21) m->mem = v;
            else if (key == 0x23) m->max_rate = v;
            else if (key == 0x24) m->proto = v;
        } else if (type == 2) {
            uint8_t b;
            if (read_n(u, &b, 1, 300) != 1) return;
            if (key == 0x40) m->probes = b;
            else if (key == 0x41) m->proto = b;
        } else {
            return;
        }
    }
}

/* resets the device and asks who it is; a chip that the open restarted
 * (an ESP32 on its own USB) needs a moment, so it asks a few times */
static bool hello(aos_io_uart_t *u)
{
    for (int tries = 0; tries < 6; tries++) {
        put_reset(u);
        aos_hal_sleep_ms(tries ? 150 : 30);
        drain(u);
        uint8_t c = 0x02, id[4];
        aos_io_uart_write(u, &c, 1);
        if (read_n(u, id, 4, 250) == 4 && !memcmp(id, "1ALS", 4)) return true;
    }
    return false;
}

static aos_io_uart_t *open_port(const char *port, uint32_t baud)
{
    aos_io_uart_t *u = aos_io_uart_open(port, baud ? baud : 115200, false, OWNER);
    if (!u) return NULL;
    /* an Espressif USB-Serial-JTAG decodes DTR/RTS as reset and boot:
     * both down is "run" */
    if (aos_io_uart_is_usb_jtag(u)) aos_io_uart_dtr_rts(u, false, false);
    return u;
}

static void fill_dev(la_dev_t *o, const char *port, uint32_t baud, const sump_meta_t *m)
{
    memset(o, 0, sizeof *o);
    o->drv = LA_DRV_SUMP;
    snprintf(o->port, sizeof o->port, "%s", port);
    o->baud = baud;
    o->channels = m->probes ? (uint8_t)(m->probes > 8 ? 8 : m->probes) : 8;
    o->max_rate = m->max_rate ? m->max_rate : SUMP_CLOCK;
    o->max_samples = m->mem ? m->mem : 24576;
    snprintf(o->name, sizeof o->name, "%s", m->name[0] ? m->name : "SUMP / OLS");
    char r[16];
    la_fmt_rate(o->max_rate, r, sizeof r);
    char s[16];
    la_fmt_count(o->max_samples, s, sizeof s);
    if (!strncmp(port, "usb", 3))
        snprintf(o->detail, sizeof o->detail, _("SUMP en %s · hasta %s, %s muestras"), port, r, s);
    else
        snprintf(o->detail, sizeof o->detail, _("SUMP en %s a %u · hasta %s, %s muestras"), port, (unsigned)baud, r, s);
}

bool la_sump_probe(const char *port, uint32_t baud, la_dev_t *out)
{
    aos_io_uart_t *u = open_port(port, baud);
    if (!u) return false;
    bool ok = hello(u);
    if (ok) {
        sump_meta_t m;
        meta_read(u, &m);
        fill_dev(out, port, baud, &m);
        aos_hal_log("logica", "sump on %s: \"%s\", %u probes, %u bytes, %u Hz", port, m.name, (unsigned)m.probes,
                    (unsigned)m.mem, (unsigned)m.max_rate);
    }
    aos_io_uart_close(u);
    return ok;
}

/* what answered last, with what its metadata said, for the next listing */
static void remember(const la_dev_t *d)
{
    aos_hal_pref_set_str("la_sport", d->port);
    aos_hal_pref_set_i32("la_sbaud", (int32_t)d->baud);
    aos_hal_pref_set_str("la_sname", d->name);
    aos_hal_pref_set_i32("la_srate", (int32_t)d->max_rate);
    aos_hal_pref_set_i32("la_smem", (int32_t)d->max_samples);
}

/* the USB serial ports, asked only when the user says so (talking to a
 * port sends bytes to whatever is there), plus the one that answered last
 * time, remembered without asking */
int la_sump_find(la_dev_t *out, int max, bool probe)
{
    int k = 0;
    char port[16] = "";
    int32_t baud = 921600;
    char key_port[] = "la_sport";
    {
        char v[16] = "";
        if (aos_hal_pref_get_str(key_port, v, sizeof v)) snprintf(port, sizeof port, "%s", v);
        aos_hal_pref_get_i32("la_sbaud", &baud);
    }
    if (probe) {
        int n = aos_hal_usb_serial_count();
        for (int i = 0; i < n && i < 4 && k < max; i++) {
            char p[8];
            snprintf(p, sizeof p, "usb%d", i);
            if (la_sump_probe(p, 921600, &out[k])) {
                remember(&out[k]);
                k++;
            }
        }
        return k;
    }
    if (port[0] && k < max) {
        sump_meta_t m;
        memset(&m, 0, sizeof m);
        char nm[40] = "";
        if (aos_hal_pref_get_str("la_sname", nm, sizeof nm)) snprintf(m.name, sizeof m.name, "%s", nm);
        int32_t v;
        if (aos_hal_pref_get_i32("la_srate", &v)) m.max_rate = (uint32_t)v;
        if (aos_hal_pref_get_i32("la_smem", &v)) m.mem = (uint32_t)v;
        fill_dev(&out[k], port, (uint32_t)baud, &m);
        k++;
    }
    return k;
}

const char *la_sump_capture(const la_dev_t *dev, const la_cfg_t *cfg, la_cap_t *c)
{
    la_lock();
    LA.state = LA_ST_CONNECT;
    la_unlock();
    aos_io_uart_t *u = open_port(dev->port, dev->baud);
    if (!u) return _("No se pudo abrir el puerto serie");
    if (!hello(u)) {
        aos_io_uart_close(u);
        return _("El analizador SUMP no contesta");
    }
    sump_meta_t m;
    meta_read(u, &m);
    la_dev_t d;
    fill_dev(&d, dev->port, dev->baud, &m);
    remember(&d);
    const bool ours = !strncmp(m.name, "P4OS", 4);

    uint32_t rate = cfg->rate > d.max_rate ? d.max_rate : cfg->rate;
    uint32_t div = SUMP_CLOCK / rate;
    if (div < 1) div = 1;
    rate = SUMP_CLOCK / div;
    uint32_t n = cfg->samples;
    if (d.max_samples && n > d.max_samples) n = d.max_samples;
    n &= ~3u;
    if (n < 4) n = 4;
    /* what the device can wait for: an edge that plain SUMP cannot say
     * leaves no condition, and the capture is not a triggered one */
    bool trig = false;
    for (int ch = 0; ch < LA_NCH; ch++)
        if (cfg->trig[ch] != LA_T_X && (cfg->trig[ch] != LA_T_EDGE || ours)) trig = true;
    uint32_t pre = trig ? (n * cfg->pre / 100) & ~3u : 0;
    uint32_t delay = n - pre;
    if (delay < 4) delay = 4;

    uint8_t mask = 0, val = 0, rise = 0, fall = 0, edge = 0;
    for (int ch = 0; ch < LA_NCH; ch++) {
        uint8_t b = (uint8_t)(1u << ch);
        switch (cfg->trig[ch]) {
        case LA_T_0:    mask |= b; break;
        case LA_T_1:    mask |= b; val |= b; break;
        /* plain SUMP has levels only: an edge waits for the level after it */
        case LA_T_RISE: if (ours) rise |= b; else { mask |= b; val |= b; } break;
        case LA_T_FALL: if (ours) fall |= b; else mask |= b; break;
        /* either edge: the app's firmware 0.2 on; nothing plain SUMP can say */
        case LA_T_EDGE: if (ours) edge |= b; break;
        default: break;
        }
    }
    put_long(u, 0xC0, mask);
    put_long(u, 0xC1, val);
    put_long(u, 0xC2, 0x08000000u);         /* stage 0, level 0, start */
    if (ours) put_long(u, 0xF0, (uint32_t)rise | (uint32_t)fall << 8 | (uint32_t)edge << 16);
    put_long(u, 0x80, div - 1);
    uint32_t rc = n / 4 - 1, dc = delay / 4 - 1;
    put_long(u, 0x81, (rc & 0xFFFF) | (dc & 0xFFFF) << 16);
    if (n > 262144) {
        put_long(u, 0x83, delay / 4 - 1);
        put_long(u, 0x84, n / 4 - 1);
    }
    put_long(u, 0x82, 0x38);
    uint8_t run = 0x01;
    aos_io_uart_write(u, &run, 1);
    aos_hal_log("logica", "sump: %u Hz (div %u), %u samples, %u after the trigger %02x/%02x %02x %02x",
                (unsigned)rate, (unsigned)div, (unsigned)n, (unsigned)delay, mask, val, rise, fall);

    if (!la_cap_alloc(c, n)) {
        put_reset(u);
        aos_io_uart_close(u);
        return _("No hay memoria para tantas muestras");
    }
    c->n = 0;
    c->trig = -1;
    c->rate = rate;
    c->nch = d.channels;
    c->lost = 0;
    c->stalled = false;
    la_lock();
    LA.state = trig ? LA_ST_WAIT : LA_ST_CAPTURE;
    LA.prog = 0;
    la_unlock();

    uint32_t got = 0, idle = 0;
    /* once data flows it has to keep flowing; before, the trigger may take
     * as long as it takes */
    const char *why = NULL;
    while (got < n) {
        if (LA.stop) {
            put_reset(u);
            why = _("Detenida");
            break;
        }
        int r = aos_io_uart_read(u, c->buf + got, (int)(n - got), 200);
        if (r < 0) {
            why = _("Se cortó el puerto serie");
            break;
        }
        if (r == 0) {
            if (got) idle += 200;
            if (idle >= 5000) {
                why = _("El analizador dejó de mandar muestras");
                break;
            }
            continue;
        }
        if (!got) {
            la_lock();
            LA.state = LA_ST_CAPTURE;
            la_unlock();
        }
        idle = 0;
        got += (uint32_t)r;
        la_lock();
        LA.prog = got;
        la_unlock();
    }
    aos_io_uart_close(u);
    if (why) return why;
    /* newest first: turn it round */
    for (uint32_t i = 0, j = n - 1; i < j; i++, j--) {
        uint8_t t = c->buf[i];
        c->buf[i] = c->buf[j];
        c->buf[j] = t;
    }
    c->n = n;
    c->trig = trig ? (int32_t)(n - delay) : -1;
    if (d.channels < 8) {
        uint8_t keep = (uint8_t)((1u << d.channels) - 1);
        for (uint32_t i = 0; i < n; i++) c->buf[i] &= keep;
    }
    snprintf(c->src, sizeof c->src, "%s", d.name);
    return NULL;
}
