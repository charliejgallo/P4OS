/*
 * P4OS - Logic: the demo, signals made up in memory.
 *
 * Three scenes, each with the decoders that read it set up, so the app
 * can be learnt (and its decoders tried) with nothing plugged in. The
 * timings are the protocols' own, so what the decoders see is what a real
 * line would carry at the sample rate chosen: too slow a rate and they
 * fail as they would on a real bus.
 *
 *   0  serial, I2C, SPI and a PWM
 *   1  1-Wire (a DS18B20), CAN, WS2812 LEDs, an NEC remote, DMX512, and
 *      Modbus RTU on an RS485 line
 *   2  pulses to measure: a servo, 1 kHz, 38 kHz, 10 kHz, and a four-bit
 *      counter as a parallel bus
 */
#include "la.h"

#include "aos_i18n.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t *buf;
    uint32_t n;
    double   rate;
    uint8_t  idle;              /* each channel's level at rest */
} gen_t;

static void lv(gen_t *g, int ch, double t0, double t1, int v)
{
    if (t1 <= t0) return;
    uint8_t m = (uint8_t)(1u << ch);
    if (((g->idle & m) != 0) == (v != 0)) return;   /* the background has it */
    double a = t0 * g->rate, b = t1 * g->rate;
    if (a < 0) a = 0;
    if (b > g->n) b = g->n;
    uint32_t i0 = (uint32_t)(a + 0.5), i1 = (uint32_t)(b + 0.5);
    if (v) for (uint32_t i = i0; i < i1; i++) g->buf[i] |= m;
    else for (uint32_t i = i0; i < i1; i++) g->buf[i] &= (uint8_t)~m;
}

/* ---- serial ---- */

static double uart_byte(gen_t *g, int ch, double t, double baud, int v, int bits, char parity, int stop)
{
    double bt = 1.0 / baud;
    lv(g, ch, t, t + bt, 0);
    t += bt;
    int ones = 0;
    for (int i = 0; i < bits; i++) {
        int b = (v >> i) & 1;
        ones += b;
        lv(g, ch, t, t + bt, b);
        t += bt;
    }
    if (parity != 'N') {
        int p = parity == 'E' ? (ones & 1) : !(ones & 1);
        lv(g, ch, t, t + bt, p);
        t += bt;
    }
    lv(g, ch, t, t + stop * bt, 1);
    return t + stop * bt;
}

static double uart_str(gen_t *g, int ch, double t, double baud, const char *s)
{
    for (; *s; s++) t = uart_byte(g, ch, t, baud, (uint8_t)*s, 8, 'N', 1);
    return t;
}

/* ---- I2C ---- */

typedef struct { gen_t *g; int scl, sda; double t, q; int last; } i2c_t;

static void i2c_start(i2c_t *b)
{
    /* SDA falls with SCL high, then SCL falls */
    lv(b->g, b->sda, b->t + b->q, b->t + 2 * b->q, 0);
    b->last = 0;
    b->t += 2 * b->q;
}

static void i2c_bit(i2c_t *b, int v)
{
    lv(b->g, b->scl, b->t, b->t + 2 * b->q, 0);
    lv(b->g, b->sda, b->t, b->t + b->q, b->last);
    lv(b->g, b->sda, b->t + b->q, b->t + 4 * b->q, v);
    b->last = v;
    lv(b->g, b->scl, b->t + 2 * b->q, b->t + 4 * b->q, 1);
    b->t += 4 * b->q;
}

static void i2c_byte(i2c_t *b, int v, int ack)
{
    for (int i = 7; i >= 0; i--) i2c_bit(b, (v >> i) & 1);
    i2c_bit(b, !ack);
}

static void i2c_restart(i2c_t *b)
{
    lv(b->g, b->scl, b->t, b->t + b->q, 0);
    lv(b->g, b->sda, b->t, b->t + b->q, b->last);
    lv(b->g, b->sda, b->t + b->q, b->t + 3 * b->q, 1);
    b->last = 0;
    lv(b->g, b->scl, b->t + 2 * b->q, b->t + 3 * b->q, 1);
    lv(b->g, b->sda, b->t + 3 * b->q, b->t + 4 * b->q, 0);
    lv(b->g, b->scl, b->t + 3 * b->q, b->t + 4 * b->q, 1);
    b->t += 4 * b->q;
}

static void i2c_stop(i2c_t *b)
{
    lv(b->g, b->scl, b->t, b->t + b->q, 0);
    lv(b->g, b->sda, b->t, b->t + 3 * b->q, 0);
    b->last = 1;
    b->t += 3 * b->q;
    /* SCL is up from here, SDA rises a quarter later */
}

/* ---- SPI, mode 0, MSB first ---- */

static double spi_xfer(gen_t *g, int cs, int sck, int mosi, int miso, double t, double hz, const uint8_t *o,
                       const uint8_t *in, int n)
{
    double h = 0.5 / hz;
    lv(g, cs, t, t + h, 0);
    double t0 = t;
    t += h;
    for (int k = 0; k < n; k++)
        for (int i = 7; i >= 0; i--) {
            lv(g, mosi, t, t + 2 * h, (o[k] >> i) & 1);
            lv(g, miso, t, t + 2 * h, (in[k] >> i) & 1);
            lv(g, sck, t + h, t + 2 * h, 1);
            t += 2 * h;
        }
    lv(g, cs, t0, t + h, 0);
    return t + h;
}

/* ---- 1-Wire, standard speed ---- */

static double ow_reset(gen_t *g, int ch, double t)
{
    lv(g, ch, t, t + 480e-6, 0);
    lv(g, ch, t + 480e-6 + 35e-6, t + 480e-6 + 155e-6, 0);   /* presence */
    return t + 960e-6;
}

static double ow_write(gen_t *g, int ch, double t, int v)
{
    for (int i = 0; i < 8; i++) {
        int b = (v >> i) & 1;
        lv(g, ch, t, t + (b ? 6e-6 : 60e-6), 0);
        t += 70e-6;
    }
    return t;
}

static double ow_read(gen_t *g, int ch, double t, int v)
{
    for (int i = 0; i < 8; i++) {
        int b = (v >> i) & 1;
        lv(g, ch, t, t + (b ? 6e-6 : 30e-6), 0);
        t += 70e-6;
    }
    return t;
}

static uint8_t crc8_maxim(const uint8_t *p, int n)
{
    uint8_t crc = 0;
    while (n--) {
        uint8_t b = *p++;
        for (int i = 0; i < 8; i++) {
            uint8_t mix = (crc ^ b) & 1;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            b >>= 1;
        }
    }
    return crc;
}

/* ---- CAN ---- */

static int can_bits(uint32_t id, bool ext, const uint8_t *d, int len, uint8_t *out)
{
    uint8_t raw[160];
    int n = 0;
    raw[n++] = 0;
    if (!ext) {
        for (int i = 10; i >= 0; i--) raw[n++] = (id >> i) & 1;
        raw[n++] = 0;                       /* RTR */
        raw[n++] = 0;                       /* IDE */
        raw[n++] = 0;                       /* r0 */
    } else {
        uint32_t base = id >> 18, x = id & 0x3FFFF;
        for (int i = 10; i >= 0; i--) raw[n++] = (base >> i) & 1;
        raw[n++] = 1;                       /* SRR */
        raw[n++] = 1;                       /* IDE */
        for (int i = 17; i >= 0; i--) raw[n++] = (x >> i) & 1;
        raw[n++] = 0;                       /* RTR */
        raw[n++] = 0;                       /* r1 */
        raw[n++] = 0;                       /* r0 */
    }
    for (int i = 3; i >= 0; i--) raw[n++] = (len >> i) & 1;
    for (int k = 0; k < len; k++)
        for (int i = 7; i >= 0; i--) raw[n++] = (d[k] >> i) & 1;
    uint16_t crc = 0;
    for (int i = 0; i < n; i++) {
        int nxt = raw[i] ^ ((crc >> 14) & 1);
        crc = (uint16_t)((crc << 1) & 0x7FFF);
        if (nxt) crc ^= 0x4599;
    }
    for (int i = 14; i >= 0; i--) raw[n++] = (crc >> i) & 1;
    /* stuffing, from SOF to the end of the CRC */
    int m = 0, run = 0, last = -1;
    for (int i = 0; i < n; i++) {
        out[m++] = raw[i];
        if (raw[i] == last) run++;
        else { run = 1; last = raw[i]; }
        if (run == 5) {
            out[m++] = !last;
            last = !last;
            run = 1;
        }
    }
    out[m++] = 1;                           /* CRC delimiter */
    out[m++] = 0;                           /* ACK, from a receiver */
    out[m++] = 1;                           /* ACK delimiter */
    for (int i = 0; i < 7; i++) out[m++] = 1;
    return m;
}

static double can_frame(gen_t *g, int ch, double t, double rate, uint32_t id, bool ext, const uint8_t *d, int len)
{
    uint8_t bits[200];
    int n = can_bits(id, ext, d, len, bits);
    double bt = 1.0 / rate;
    for (int i = 0; i < n; i++) {
        lv(g, ch, t, t + bt, bits[i]);
        t += bt;
    }
    return t + 3 * bt;
}

/* ---- WS2812 ---- */

static double ws_led(gen_t *g, int ch, double t, uint32_t rgb)
{
    uint32_t grb = ((rgb >> 8) & 0xFF) << 16 | ((rgb >> 16) & 0xFF) << 8 | (rgb & 0xFF);
    for (int i = 23; i >= 0; i--) {
        bool one = (grb >> i) & 1;
        double hi = one ? 0.8e-6 : 0.4e-6;
        lv(g, ch, t, t + hi, 1);
        lv(g, ch, t + hi, t + 1.25e-6, 0);
        t += 1.25e-6;
    }
    return t;
}

/* ---- NEC, as a receiver's output (low while the carrier is there) ---- */

static double nec_frame(gen_t *g, int ch, double t, int addr, int cmd)
{
    lv(g, ch, t, t + 9e-3, 0);
    t += 9e-3 + 4.5e-3;
    uint32_t v = (uint32_t)addr | (uint32_t)(~addr & 0xFF) << 8 | (uint32_t)cmd << 16 | (uint32_t)(~cmd & 0xFF) << 24;
    for (int i = 0; i < 32; i++) {
        lv(g, ch, t, t + 562.5e-6, 0);
        t += 562.5e-6 + ((v >> i) & 1 ? 1687.5e-6 : 562.5e-6);
    }
    lv(g, ch, t, t + 562.5e-6, 0);
    return t + 562.5e-6;
}

static uint16_t crc16_modbus(const uint8_t *p, int n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = crc & 1 ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static double modbus_frame(gen_t *g, int ch, double t, const uint8_t *p, int n)
{
    uint8_t f[64];
    memcpy(f, p, (size_t)n);
    uint16_t crc = crc16_modbus(f, n);
    f[n++] = (uint8_t)crc;
    f[n++] = (uint8_t)(crc >> 8);
    for (int i = 0; i < n; i++) t = uart_byte(g, ch, t, 9600, f[i], 8, 'N', 1);
    return t;
}

static void square(gen_t *g, int ch, double t0, double t1, double hz, double high)
{
    double p = 1.0 / hz;
    for (double t = t0; t < t1; t += p) lv(g, ch, t, t + high, 1);
}

/* ---- the scenes ---- */

static const char *const SCENE[] = { N_("Serie, I2C y SPI"), N_("1-Wire, CAN, LEDs, IR, DMX y Modbus"),
                                     N_("Pulsos y un bus paralelo") };

int la_demo_scenes(void) { return 3; }
const char *la_demo_scene_name(int i) { return i >= 0 && i < 3 ? _(SCENE[i]) : ""; }

static void scene0(gen_t *g, double span)
{
    g->idle = 0x0F;                         /* UART, I2C and CS high; SCK, MOSI, MISO, PWM low */
    memset(g->buf, g->idle, g->n);
    for (double t = 1e-3; t < span; t += 20e-3) uart_str(g, 0, t, 115200, "Hola desde P4OS!\r\n");
    for (double t = 5e-3; t < span; t += 20e-3) {
        i2c_t b = { g, 1, 2, t, 2.5e-6, 1 };   /* 100 kHz */
        i2c_start(&b);
        i2c_byte(&b, 0x50 << 1, 1);
        i2c_byte(&b, 0x00, 1);
        i2c_byte(&b, 0x10, 1);
        i2c_byte(&b, 'O', 1);
        i2c_byte(&b, 'K', 1);
        i2c_stop(&b);
        b.t += 40e-6;
        i2c_start(&b);
        i2c_byte(&b, 0x68 << 1, 1);
        i2c_byte(&b, 0x75, 1);
        i2c_restart(&b);
        i2c_byte(&b, 0x68 << 1 | 1, 1);
        i2c_byte(&b, 0x68, 0);              /* the master does not ack the last */
        i2c_stop(&b);
        b.t += 40e-6;
        i2c_start(&b);
        i2c_byte(&b, 0x3C << 1, 0);         /* nobody there */
        i2c_stop(&b);
    }
    for (double t = 10e-3; t < span; t += 20e-3) {
        static const uint8_t o[4] = { 0x9F, 0, 0, 0 }, in[4] = { 0xFF, 0xEF, 0x40, 0x18 };
        double e = spi_xfer(g, 3, 4, 5, 6, t, 1e6, o, in, 4);
        static const uint8_t o2[6] = { 0x03, 0x00, 0x10, 0x00, 0, 0 }, in2[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 'P', '4' };
        spi_xfer(g, 3, 4, 5, 6, e + 5e-6, 1e6, o2, in2, 6);
    }
    square(g, 7, 0, span, 1000, 0.25e-3);
}

static void scene1(gen_t *g, double span)
{
    g->idle = 0x01 | 0x02 | 0x08 | 0x10 | 0x20;   /* 1-Wire, CAN, IR, DMX, RS485 at rest */
    memset(g->buf, g->idle, g->n);
    /* a DS18B20 at 25.0625 degrees */
    for (double t = 0.5e-3; t < span; t += 60e-3) {
        double u = ow_reset(g, 0, t);
        u = ow_write(g, 0, u, 0xCC);
        u = ow_write(g, 0, u, 0x44);
        u = ow_reset(g, 0, u + 1e-3);
        u = ow_write(g, 0, u, 0xCC);
        u = ow_write(g, 0, u, 0xBE);
        uint8_t sp[9] = { 0x91, 0x01, 0x4B, 0x46, 0x7F, 0xFF, 0x0F, 0x10, 0 };
        sp[8] = crc8_maxim(sp, 8);
        for (int i = 0; i < 9; i++) u = ow_read(g, 0, u, sp[i]);
    }
    for (double t = 1e-3; t < span; t += 10e-3) {
        static const uint8_t d1[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
        static const uint8_t d2[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
        double u = can_frame(g, 1, t, 500000, 0x123, false, d1, 4);
        can_frame(g, 1, u + 20e-6, 500000, 0x18FEF100, true, d2, 8);
    }
    static const uint32_t col[4] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFA000 };
    for (double t = 0.2e-3; t < span; t += 5e-3) {
        double u = t;
        for (int i = 0; i < 4; i++) u = ws_led(g, 2, u, col[i]);
    }
    if (span > 25e-3) nec_frame(g, 3, 15e-3, 0x04, 0x08);
    for (double t = 3e-3; t < span; t += 25e-3) {
        lv(g, 4, t, t + 120e-6, 0);         /* break */
        double u = t + 132e-6;              /* mark after break */
        u = uart_byte(g, 4, u, 250000, 0x00, 8, 'N', 2);
        for (int s = 0; s < 16; s++) u = uart_byte(g, 4, u, 250000, (s * 16 + 15) & 0xFF, 8, 'N', 2);
    }
    for (double t = 2e-3; t < span; t += 50e-3) {
        static const uint8_t req[6] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02 };
        static const uint8_t ans[7] = { 0x01, 0x03, 0x04, 0x00, 0xE6, 0x01, 0x2C };
        double u = modbus_frame(g, 5, t, req, 6);
        modbus_frame(g, 5, u + 6e-3, ans, 7);
    }
    square(g, 6, 0, span, 50, 10e-3);
    square(g, 7, 0, span, 100000, 5e-6);
}

static void scene2(gen_t *g, double span)
{
    g->idle = 0;
    memset(g->buf, 0, g->n);
    square(g, 0, 0, span, 50, 1.5e-3);
    square(g, 1, 0, span, 1000, 0.25e-3);
    square(g, 2, 0, span, 38000, 1.0 / 38000 / 3);
    square(g, 3, 0, span, 10000, 50e-6);
    int k = 0;
    for (double t = 0; t < span; t += 10e-6, k++)
        for (int b = 0; b < 4; b++) lv(g, 4 + b, t, t + 10e-6, (k >> b) & 1);
}

const char *la_demo_capture(const la_cfg_t *cfg, la_cap_t *c)
{
    la_lock();
    LA.state = LA_ST_CAPTURE;
    la_unlock();
    uint32_t n = cfg->samples;
    if (n > 8u * 1024 * 1024) n = 8u * 1024 * 1024;
    if (!la_cap_alloc(c, n)) return _("No hay memoria para tantas muestras");
    gen_t g = { c->buf, n, (double)cfg->rate, 0 };
    double span = (double)n / (double)cfg->rate;
    switch (cfg->demo) {
    case 1: scene1(&g, span); break;
    case 2: scene2(&g, span); break;
    default: scene0(&g, span); break;
    }
    c->n = n;
    c->rate = cfg->rate;
    c->nch = 8;
    c->lost = 0;
    c->stalled = false;
    c->trig = -1;
    if (la_trig_any(cfg->trig)) {
        uint32_t from = (uint32_t)((uint64_t)n * cfg->pre / 100);
        c->trig = la_trig_find(cfg->trig, c->buf, n, from ? from : 0, c->buf[0]);
        if (c->trig < 0) c->trig = la_trig_find(cfg->trig, c->buf, n, 0, c->buf[0]);
    }
    la_lock();
    LA.prog = n;
    la_unlock();
    snprintf(c->src, sizeof c->src, "Demo: %s", la_demo_scene_name(cfg->demo));
    return NULL;
}

void la_demo_decoders(int scene)
{
    static const char *const N0[8] = { "TX", "SCL", "SDA", "CS", "SCK", "MOSI", "MISO", "PWM" };
    static const char *const N1[8] = { "1-Wire", "CAN", "WS2812", "IR", "DMX", "RS485", "LED", "CLK" };
    static const char *const N2[8] = { "Servo", "1 kHz", "38 kHz", "10 kHz", "D0", "D1", "D2", "D3" };
    const char *const *nm = scene == 1 ? N1 : scene == 2 ? N2 : N0;
    for (int i = 0; i < LA_NCH; i++) {
        snprintf(LA.chname[i], sizeof LA.chname[i], "%s", nm[i]);
        LA.chshow[i] = true;
    }
    for (int i = 0; i < LA_DEC_MAX; i++) {
        la_dec_defaults(&LA.dec[i], LA_DEC_UART);
        LA.dec[i].on = false;
    }
    la_dec_cfg_t *d = LA.dec;
    if (scene == 0) {
        la_dec_defaults(&d[0], LA_DEC_UART);
        d[0].ch[0] = 0;
        d[0].fmt = LA_FMT_ASCII;
        la_dec_defaults(&d[1], LA_DEC_I2C);
        d[1].ch[0] = 1;
        d[1].ch[1] = 2;
        la_dec_defaults(&d[2], LA_DEC_SPI);
        d[2].ch[0] = 4;
        d[2].ch[1] = 5;
        d[2].ch[2] = 6;
        d[2].ch[3] = 3;
    } else if (scene == 1) {
        la_dec_defaults(&d[0], LA_DEC_1WIRE);
        d[0].ch[0] = 0;
        la_dec_defaults(&d[1], LA_DEC_CAN);
        d[1].ch[0] = 1;
        d[1].baud = 500000;
        la_dec_defaults(&d[2], LA_DEC_WS2812);
        d[2].ch[0] = 2;
        la_dec_defaults(&d[3], LA_DEC_NEC);
        d[3].ch[0] = 3;
        la_dec_defaults(&d[4], LA_DEC_DMX);
        d[4].ch[0] = 4;
        la_dec_defaults(&d[5], LA_DEC_MODBUS);
        d[5].ch[0] = 5;
        d[5].baud = 9600;
    } else {
        la_dec_defaults(&d[0], LA_DEC_PAR);
        for (int i = 0; i < LA_DEC_PINS; i++) d[0].ch[i] = -1;
        for (int b = 0; b < 4; b++) d[0].ch[1 + b] = (int8_t)(4 + b);
    }
}
