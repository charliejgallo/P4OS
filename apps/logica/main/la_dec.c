/*
 * P4OS - Logic: the protocol decoders.
 *
 * Each one walks its channels' edges in the capture (la_next_edge and a
 * version of it for several channels, both skipping quiet stretches with
 * the summaries) and leaves two things:
 *
 *   annotations  a box under the channels per field: a byte, an address,
 *                an ACK, a start bit, an error, with a short text
 *   packets      a line per transaction for the table: an I2C transfer from
 *                START to STOP, a line of serial text, a CAN frame, a
 *                Modbus request or answer
 *
 * They run in the job thread after every capture, and again when their
 * settings change. Times are samples; a bit's length is a float of samples
 * (exact enough: positions are added as integers to small products).
 */
#include "la.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const la_cap_t *c;
    const la_dec_cfg_t *d;
    int row;
    float rate;
} dctx_t;

static inline int lvl(const la_cap_t *c, int ch, uint32_t i) { return (c->buf[i] >> ch) & 1; }

static void ann(dctx_t *x, uint32_t s0, uint32_t s1, int kind, const char *fmt, ...)
{
    if (LA.an.nann >= LA_ANN_MAX) {
        LA.an.full = true;
        return;
    }
    if (s1 <= s0) s1 = s0 + 1;
    la_ann_t *a = &LA.an.ann[LA.an.nann++];
    a->s0 = s0;
    a->s1 = s1;
    a->row = (uint8_t)x->row;
    a->kind = (uint8_t)kind;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->t, sizeof a->t, fmt, ap);
    va_end(ap);
}

static void pkt(dctx_t *x, uint32_t s0, uint32_t s1, bool err, const char *fmt, ...)
{
    if (LA.an.npkt >= LA_PKT_MAX) {
        LA.an.full = true;
        return;
    }
    la_pkt_t *p = &LA.an.pkt[LA.an.npkt++];
    p->s0 = s0;
    p->s1 = s1 > s0 ? s1 : s0 + 1;
    p->row = (uint8_t)x->row;
    p->err = err;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->t, sizeof p->t, fmt, ap);
    va_end(ap);
}

/* appends to a packet's text being built, cutting it at the end */
static void cat(char *s, size_t n, const char *fmt, ...)
{
    size_t l = strlen(s);
    if (l + 1 >= n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s + l, n - l, fmt, ap);
    va_end(ap);
}

/* the next sample > from where any channel of mask changes, or to */
static uint32_t next_any(const la_cap_t *c, uint8_t m, uint32_t from, uint32_t to)
{
    if (to > c->n) to = c->n;
    if (from >= to) return to;
    const uint8_t v = c->buf[from] & m;
    uint32_t i = from + 1;
    while (i < to) {
        if (!(i % LA_SUM2) && i + LA_SUM2 <= to) {
            uint32_t k = i / LA_SUM2;
            if ((c->s2[2 * k] & m) == v && (c->s2[2 * k + 1] & m) == v) {
                i += LA_SUM2;
                continue;
            }
        }
        if (!(i % LA_SUM1) && i + LA_SUM1 <= to) {
            uint32_t k = i / LA_SUM1;
            if ((c->s1[2 * k] & m) == v && (c->s1[2 * k + 1] & m) == v) {
                i += LA_SUM1;
                continue;
            }
        }
        if ((c->buf[i] & m) != v) return i;
        i++;
    }
    return to;
}

/* the next sample > from where the channel is at level v (from itself not counted), or n */
static uint32_t next_level(const la_cap_t *c, int ch, uint32_t from, int v)
{
    if (from >= c->n) return c->n;
    if (lvl(c, ch, from) == v) {
        uint32_t e = la_next_edge(c, ch, from, c->n);
        if (e >= c->n) return c->n;
        from = e;
    }
    return la_next_edge(c, ch, from, c->n);
}

static void fmt_byte(const dctx_t *x, uint32_t v, int bits, char *out, size_t n)
{
    la_fmt_value(v, bits, x->d->fmt, out, n);
}

/* ===== UART (and what rides on it: DMX, Modbus) ===== */

static const uint32_t BAUDS[] = { 300, 600, 1200, 2400, 4800, 9600, 14400, 19200, 28800, 31250, 38400, 57600,
                                  74880, 76800, 115200, 128000, 230400, 250000, 256000, 460800, 500000, 921600,
                                  1000000, 1500000, 2000000, 3000000, 4000000 };

/* the shortest pulse that shows up more than once is a bit */
static uint32_t guess_baud(const la_cap_t *c, int ch, float rate)
{
    uint32_t best = 0xFFFFFFFFu, second = 0xFFFFFFFFu;
    uint32_t pos = la_next_edge(c, ch, 0, c->n);
    for (int k = 0; k < 4000 && pos < c->n; k++) {
        uint32_t e = la_next_edge(c, ch, pos, c->n);
        if (e >= c->n) break;
        uint32_t w = e - pos;
        if (w < best) {
            second = best;
            best = w;
        } else if (w < second) {
            second = w;
        }
        pos = e;
    }
    if (second == 0xFFFFFFFFu) return 0;
    /* two close to each other: one bit; otherwise the larger (the smallest
     * was a glitch) */
    uint32_t w = (second - best) * 5 <= best ? (best + second) / 2 : second;
    if (!w) return 0;
    float est = rate / (float)w;
    uint32_t pick = 0;
    float err = 0.12f;
    for (size_t i = 0; i < sizeof BAUDS / sizeof BAUDS[0]; i++) {
        float e = (est - (float)BAUDS[i]) / (float)BAUDS[i];
        if (e < 0) e = -e;
        if (e < err) {
            err = e;
            pick = BAUDS[i];
        }
    }
    return pick ? pick : (uint32_t)(est + 0.5f);
}

typedef struct {
    uint32_t s0, s1;            /* the start bit's edge, the end of the stop bits */
    uint16_t v;
    bool ferr, perr, brk;
} uframe_t;

typedef struct {
    int ch;
    bool inv;
    float tb;                   /* samples per bit */
    int bits;
    char par;
    int stop;
} uline_t;

/* the next character from *pos on; false at the end of the capture */
static bool uart_next(const la_cap_t *c, const uline_t *u, uint32_t *pos, uframe_t *f)
{
    const int idle = u->inv ? 0 : 1;
    const int nb = 1 + u->bits + (u->par != 'N') + u->stop;
    for (;;) {
        uint32_t s = next_level(c, u->ch, *pos, !idle);
        if (s >= c->n) return false;
        uint32_t end = s + (uint32_t)((float)nb * u->tb);
        if (end >= c->n) return false;
        uint32_t mid = s + (uint32_t)(0.5f * u->tb);
        if ((lvl(c, u->ch, mid) ^ u->inv) != 0) {      /* a glitch, not a start bit */
            *pos = mid;
            continue;
        }
        memset(f, 0, sizeof *f);
        f->s0 = s;
        int ones = 0, all0 = 1;
        for (int i = 0; i < u->bits; i++) {
            int b = lvl(c, u->ch, s + (uint32_t)((1.5f + (float)i) * u->tb)) ^ u->inv;
            f->v |= (uint16_t)(b << i);
            ones += b;
            if (b) all0 = 0;
        }
        int k = 1 + u->bits;
        if (u->par != 'N') {
            int p = lvl(c, u->ch, s + (uint32_t)((0.5f + (float)k) * u->tb)) ^ u->inv;
            if (p) all0 = 0;
            int want = u->par == 'E' ? (ones & 1) : !(ones & 1);
            f->perr = p != want;
            k++;
        }
        for (int i = 0; i < u->stop; i++, k++) {
            int b = lvl(c, u->ch, s + (uint32_t)((0.5f + (float)k) * u->tb)) ^ u->inv;
            if (!b) f->ferr = true;
        }
        f->s1 = end;
        if (f->ferr && all0) {
            /* a break: the line held low past the character */
            f->brk = true;
            uint32_t r = next_level(c, u->ch, s, idle);
            f->s1 = r;
            *pos = r;
        } else {
            /* go on from the middle of the last stop bit */
            *pos = s + (uint32_t)(((float)nb - 0.5f) * u->tb);
        }
        return true;
    }
}

static uint32_t uart_baud(dctx_t *x, const la_dec_cfg_t *d, bool *guessed)
{
    *guessed = false;
    if (d->baud) return d->baud;
    *guessed = true;
    return guess_baud(x->c, d->ch[0], x->rate);
}

static void dec_uart(dctx_t *x)
{
    const la_dec_cfg_t *d = x->d;
    bool guessed;
    uint32_t baud = uart_baud(x, d, &guessed);
    if (!baud || x->rate / (float)baud < 2.5f) {
        snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], "%s", baud ? _("UART: muestreo muy lento para la velocidad") : _("UART: no hay datos para medir la velocidad"));
        return;
    }
    snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], "UART %u %d%c%d%s", (unsigned)baud, d->bits, d->parity,
             d->stop, guessed ? _(" (adivinada)") : "");
    uline_t u = { d->ch[0], d->inv, x->rate / (float)baud, d->bits, d->parity, d->stop };
    uint32_t pos = 0;
    uframe_t f;
    char line[86] = "";
    uint32_t l0 = 0, l1 = 0;
    bool lerr = false;
    const float gap = 20.0f * u.tb * (float)(2 + d->bits);
    while (uart_next(x->c, &u, &pos, &f)) {
        if (line[0] && (float)(int32_t)(f.s0 - l1) > gap) {          /* the next start can come before the stop bit's end */
            pkt(x, l0, l1, lerr, "%s", line);
            line[0] = 0;
        }
        if (f.brk) {
            ann(x, f.s0, f.s1, LA_K_CTRL, "Break");
            continue;
        }
        char v[16];
        fmt_byte(x, f.v, d->bits, v, sizeof v);
        bool err = f.ferr || f.perr;
        ann(x, f.s0, f.s1, err ? LA_K_ERR : LA_K_DATA, "%s%s", v, f.ferr ? "!" : f.perr ? "?" : "");
        if (!line[0]) {
            l0 = f.s0;
            lerr = false;
        }
        l1 = f.s1;
        lerr |= err;
        if (d->fmt == LA_FMT_ASCII) {
            if (f.v >= 32 && f.v < 127) cat(line, sizeof line, "%c", (char)f.v);
            else if (f.v != '\n' && f.v != '\r') cat(line, sizeof line, "\\x%02X", (unsigned)f.v);
        } else {
            cat(line, sizeof line, "%s%s", line[0] ? " " : "", v);
        }
        if (f.v == '\n' || strlen(line) > 70) {
            pkt(x, l0, l1, lerr, "%s", line[0] ? line : "\\n");
            line[0] = 0;
        }
    }
    if (line[0]) pkt(x, l0, l1, lerr, "%s", line);
}

/* ===== DMX512: UART at 250 kbit/s, 8N2, frames opened by a break ===== */

static void dec_dmx(dctx_t *x)
{
    const la_dec_cfg_t *d = x->d;
    if (x->rate < 1000000) {
        snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], "%s", _("DMX: hace falta muestrear a 1 MHz o más"));
        return;
    }
    snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], "DMX512");
    uline_t u = { d->ch[0], d->inv, x->rate / 250000.0f, 8, 'N', 2 };
    uint32_t pos = 0, f0 = 0, last = 0;
    int slot = -1;
    char line[86] = "";
    uframe_t f;
    while (uart_next(x->c, &u, &pos, &f)) {
        if (f.brk) {
            if (slot > 0) pkt(x, f0, last, false, _("%d canales: %s"), slot - 1 < 0 ? 0 : slot - 1, line);
            float us = (float)(f.s1 - f.s0) / x->rate * 1e6f;
            ann(x, f.s0, f.s1, us < 88 ? LA_K_ERR : LA_K_CTRL, us < 88 ? "Break corto" : "Break");
            slot = 0;
            f0 = f.s0;
            line[0] = 0;
            continue;
        }
        if (slot < 0) continue;     /* not inside a frame yet */
        if (slot == 0) ann(x, f.s0, f.s1, f.v ? LA_K_ADDR : LA_K_START, "SC %02X", (unsigned)f.v);
        else {
            ann(x, f.s0, f.s1, f.ferr ? LA_K_ERR : LA_K_DATA, "%d: %u", slot, (unsigned)f.v);
            if (slot <= 24) cat(line, sizeof line, "%s%u", slot > 1 ? " " : "", (unsigned)f.v);
            else if (slot == 25) cat(line, sizeof line, " ...");
        }
        slot++;
        last = f.s1;
        if (slot > 513) slot = -1;
    }
    if (slot > 0) pkt(x, f0, last, false, _("%d canales: %s"), slot - 1, line);
}

/* ===== Modbus RTU: UART characters, frames split by 3.5 characters of silence ===== */

static uint16_t crc16_modbus(const uint8_t *p, int n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = crc & 1 ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static const char *mb_func(int f)
{
    switch (f & 0x7F) {
    case 1:  return _("leer bobinas");
    case 2:  return _("leer entradas");
    case 3:  return _("leer registros");
    case 4:  return _("leer reg. de entrada");
    case 5:  return _("escribir bobina");
    case 6:  return _("escribir registro");
    case 15: return _("escribir bobinas");
    case 16: return _("escribir registros");
    case 23: return _("leer/escribir registros");
    default: return _("función");
    }
}

static void mb_frame(dctx_t *x, const uint8_t *b, const uint32_t *s0, const uint32_t *s1, int n)
{
    if (n < 4) {
        for (int i = 0; i < n; i++) ann(x, s0[i], s1[i], LA_K_ERR, "%02X", b[i]);
        pkt(x, s0[0], s1[n - 1], true, _("trama corta (%d bytes)"), n);
        return;
    }
    uint16_t crc = crc16_modbus(b, n - 2);
    bool ok = (b[n - 2] | b[n - 1] << 8) == crc;
    ann(x, s0[0], s1[0], LA_K_ADDR, _("Esclavo %u"), b[0]);
    ann(x, s0[1], s1[1], b[1] & 0x80 ? LA_K_ERR : LA_K_CTRL, "Fn %u", b[1] & 0x7F);
    for (int i = 2; i < n - 2; i++) ann(x, s0[i], s1[i], LA_K_DATA, "%02X", b[i]);
    ann(x, s0[n - 2], s1[n - 1], ok ? LA_K_ACK : LA_K_ERR, ok ? "CRC ok" : "CRC mal");
    char t[86];
    int f = b[1];
    snprintf(t, sizeof t, "%u %s", b[0], mb_func(f));
    if (f & 0x80) {
        cat(t, sizeof t, _(": excepción %u"), n > 2 ? b[2] : 0);
    } else if ((f >= 1 && f <= 4) && n == 8) {
        cat(t, sizeof t, _(": %u desde %u"), b[4] << 8 | b[5], b[2] << 8 | b[3]);
    } else if ((f >= 1 && f <= 4) && n >= 5 && b[2] == n - 5) {
        cat(t, sizeof t, " ->");
        if (f >= 3)
            for (int i = 3; i + 1 < n - 2; i += 2) cat(t, sizeof t, " %u", b[i] << 8 | b[i + 1]);
        else
            for (int i = 3; i < n - 2; i++) cat(t, sizeof t, " %02X", b[i]);
    } else if ((f == 5 || f == 6) && n == 8) {
        cat(t, sizeof t, _(": %u en %u"), b[4] << 8 | b[5], b[2] << 8 | b[3]);
    } else if ((f == 15 || f == 16) && n == 8) {
        cat(t, sizeof t, _(": %u desde %u, hecho"), b[4] << 8 | b[5], b[2] << 8 | b[3]);
    } else if ((f == 15 || f == 16) && n > 9) {
        cat(t, sizeof t, _(": %u desde %u"), b[4] << 8 | b[5], b[2] << 8 | b[3]);
    }
    if (!ok) cat(t, sizeof t, _(" (CRC mal)"));
    pkt(x, s0[0], s1[n - 1], !ok, "%s", t);
}

static void dec_modbus(dctx_t *x)
{
    const la_dec_cfg_t *d = x->d;
    bool guessed;
    uint32_t baud = uart_baud(x, d, &guessed);
    if (!baud || x->rate / (float)baud < 2.5f) {
        snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], "%s", _("Modbus: muestreo muy lento o sin datos"));
        return;
    }
    snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], "Modbus RTU %u 8%c%d%s", (unsigned)baud, d->parity,
             d->stop, guessed ? _(" (adivinada)") : "");
    uline_t u = { d->ch[0], d->inv, x->rate / (float)baud, 8, d->parity, d->stop };
    int nb = 1 + 8 + (d->parity != 'N') + d->stop;
    float gap = baud > 19200 ? 1750e-6f * x->rate : 3.5f * (float)nb * u.tb;
    gap *= 0.8f;                /* real devices run it close */
    uint8_t b[256];
    uint32_t s0[256], s1[256];
    int n = 0;
    uint32_t pos = 0;
    uframe_t f;
    while (uart_next(x->c, &u, &pos, &f)) {
        if (n && ((float)(int32_t)(f.s0 - s1[n - 1]) > gap || n == 256)) {
            mb_frame(x, b, s0, s1, n);
            n = 0;
        }
        if (f.brk) continue;
        b[n] = (uint8_t)f.v;
        s0[n] = f.s0;
        s1[n] = f.s1;
        n++;
    }
    if (n) mb_frame(x, b, s0, s1, n);
}

/* ===== I2C ===== */

static void dec_i2c(dctx_t *x)
{
    const la_cap_t *c = x->c;
    const int scl = x->d->ch[0], sda = x->d->ch[1];
    snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], "I2C");
    const uint8_t m = (uint8_t)(1u << scl | 1u << sda);
    bool in = false;            /* inside a transfer */
    int bit = 0, byte_no = 0;
    uint32_t v = 0, b0 = 0;
    /* the ACK bit is drawn from SCL's rise to its fall, so it waits */
    bool ack_open = false;
    uint32_t ack_at = 0;
    int ack_kind = LA_K_ACK;
    const char *ack_txt = "A";
    bool perr = false;
    uint32_t p0 = 0;
    char t[86] = "";
    uint32_t pos = 0;
    while (pos < c->n) {
        uint32_t e = next_any(c, m, pos, c->n);
        if (e >= c->n) break;
        uint8_t pr = c->buf[e - 1], cu = c->buf[e];
        int scl0 = (pr >> scl) & 1, scl1 = (cu >> scl) & 1, sda0 = (pr >> sda) & 1, sda1 = (cu >> sda) & 1;
        pos = e;
        if (ack_open && (scl0 && !scl1)) {
            ann(x, ack_at, e, ack_kind, "%s", ack_txt);
            ack_open = false;
            continue;
        }
        if (scl0 && scl1 && sda0 != sda1) {
            if (ack_open) {
                ann(x, ack_at, e, ack_kind, "%s", ack_txt);
                ack_open = false;
            }
            if (!sda1) {
                /* START or repeated START */
                ann(x, e, e + 1, LA_K_START, in ? "Sr" : "S");
                if (!in) {
                    p0 = e;
                    t[0] = 0;
                    perr = false;
                } else {
                    cat(t, sizeof t, " |");
                }
                in = true;
                bit = 0;
                byte_no = 0;
                v = 0;
            } else {
                ann(x, e, e + 1, LA_K_START, "P");
                if (in) pkt(x, p0, e, perr, "%s", t[0] ? t : "S P");
                in = false;
            }
            continue;
        }
        if (!in || scl0 || !scl1) continue;
        /* SCL rises: a bit */
        if (bit < 8) {
            if (!bit) b0 = e;
            v = v << 1 | (uint32_t)sda1;
            bit++;
            continue;
        }
        /* the ACK bit */
        bool nack = sda1;
        if (!byte_no) {
            bool rd = v & 1;
            ann(x, b0, e, LA_K_ADDR, "%02X %s", (unsigned)(v >> 1), rd ? "R" : "W");
            cat(t, sizeof t, "%s%02X %s:", t[0] ? " " : "", (unsigned)(v >> 1), rd ? "R" : "W");
            if (nack) {
                perr = true;
                cat(t, sizeof t, _(" sin respuesta"));
            }
        } else {
            char s[16];
            fmt_byte(x, v, 8, s, sizeof s);
            ann(x, b0, e, LA_K_DATA, "%s", s);
            cat(t, sizeof t, " %s", s);
        }
        ack_at = e;
        ack_open = true;
        ack_kind = nack ? (byte_no ? LA_K_INFO : LA_K_ERR) : LA_K_ACK;
        ack_txt = nack ? "N" : "A";
        byte_no++;
        bit = 0;
        v = 0;
    }
    if (in && t[0]) pkt(x, p0, c->n - 1, true, "%s", t);
}

/* ===== SPI ===== */

static void dec_spi(dctx_t *x)
{
    const la_cap_t *c = x->c;
    const la_dec_cfg_t *d = x->d;
    const int clk = d->ch[0], mosi = d->ch[1], miso = d->ch[2], cs = d->ch[3];
    const int cpol = d->mode >> 1, cpha = d->mode & 1;
    const int sample_rising = (cpol ^ cpha) == 0;
    const int bits = d->bits >= 4 && d->bits <= 16 ? d->bits : 8;
    const int cs_on = d->inv ? 1 : 0;
    snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], _("SPI modo %d, %d bits%s"), d->mode, bits,
             d->lsb ? _(", LSB primero") : "");
    uint8_t m = (uint8_t)(1u << clk);
    if (cs >= 0) m |= (uint8_t)(1u << cs);
    uint32_t pos = 0, w0 = 0, f0 = 0, last_edge = 0;
    uint32_t vo = 0, vi = 0, period = 0;
    int nb = 0, words = 0;
    bool active = cs < 0;
    char to[86] = "", ti[86] = "";
    if (cs >= 0) active = lvl(c, cs, 0) == cs_on;
    if (active) f0 = 0;
    for (;;) {
        uint32_t e = next_any(c, m, pos, c->n);
        if (e >= c->n) break;
        pos = e;
        uint8_t pr = c->buf[e - 1], cu = c->buf[e];
        if (cs >= 0 && ((pr ^ cu) >> cs & 1)) {
            bool now = ((cu >> cs) & 1) == cs_on;
            if (now && !active) {
                f0 = e;
                to[0] = ti[0] = 0;
                words = 0;
                nb = 0;
                vo = vi = 0;
            } else if (!now && active) {
                if (nb) ann(x, w0, e, LA_K_ERR, _("%d bits"), nb);
                if (words) {
                    if (miso >= 0 && mosi >= 0) pkt(x, f0, e, nb != 0, "MOSI %s · MISO %s", to, ti);
                    else pkt(x, f0, e, nb != 0, "%s", mosi >= 0 ? to : ti);
                }
                nb = 0;
            }
            active = now;
        }
        if (!((pr ^ cu) >> clk & 1) || !active) continue;
        int rising = (cu >> clk) & 1;
        if (rising != sample_rising) continue;
        if (last_edge && e - last_edge < 0x7FFFFFFF) period = e - last_edge;
        last_edge = e;
        if (!nb) w0 = e;
        int bo = mosi >= 0 ? lvl(c, mosi, e) : 0, bi = miso >= 0 ? lvl(c, miso, e) : 0;
        if (d->lsb) {
            vo |= (uint32_t)bo << nb;
            vi |= (uint32_t)bi << nb;
        } else {
            vo = vo << 1 | (uint32_t)bo;
            vi = vi << 1 | (uint32_t)bi;
        }
        if (++nb == bits) {
            char so[16], si[16];
            fmt_byte(x, vo, bits, so, sizeof so);
            fmt_byte(x, vi, bits, si, sizeof si);
            uint32_t end = e + (period ? period : 1);
            if (mosi >= 0 && miso >= 0) ann(x, w0, end, LA_K_DATA, "%s/%s", so, si);
            else ann(x, w0, end, LA_K_DATA, "%s", mosi >= 0 ? so : si);
            if (!words) f0 = cs >= 0 ? f0 : w0;
            cat(to, sizeof to, "%s%s", words ? " " : "", so);
            cat(ti, sizeof ti, "%s%s", words ? " " : "", si);
            words++;
            nb = 0;
            vo = vi = 0;
            /* without a CS, a line every 16 words */
            if (cs < 0 && words == 16) {
                pkt(x, f0, end, false, "MOSI %s · MISO %s", to, ti);
                to[0] = ti[0] = 0;
                words = 0;
            }
        }
    }
    if (words && cs < 0) pkt(x, f0, last_edge, false, "MOSI %s · MISO %s", to, ti);
}

/* ===== 1-Wire ===== */

static const char *ow_rom(int v)
{
    switch (v) {
    case 0x33: return "Read ROM";
    case 0x55: return "Match ROM";
    case 0xCC: return "Skip ROM";
    case 0xF0: return "Search ROM";
    case 0xEC: return "Alarm Search";
    case 0x3C: return "Overdrive Skip";
    case 0x69: return "Overdrive Match";
    default: return NULL;
    }
}

static const char *ow_func(int v)
{
    switch (v) {
    case 0x44: return "Convert T";
    case 0xBE: return "Read Scratchpad";
    case 0x4E: return "Write Scratchpad";
    case 0x48: return "Copy Scratchpad";
    case 0xB8: return "Recall E2";
    case 0xB4: return "Read Power";
    case 0xF0: return "Read Memory";
    case 0x0F: return "Write Scratchpad";
    case 0xAA: return "Read Scratchpad";
    case 0x55: return "Copy Scratchpad";
    default: return NULL;
    }
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

static void ow_flush(dctx_t *x, uint32_t p0, uint32_t p1, const uint8_t *b, int n, bool presence)
{
    if (!n && !presence) return;
    char t[86];
    snprintf(t, sizeof t, "%s", presence ? "Reset" : _("Reset sin presencia"));
    int i = 0;
    bool err = !presence;
    if (n) {
        const char *r = ow_rom(b[0]);
        cat(t, sizeof t, " · %s", r ? r : "?");
        i = 1;
        if ((b[0] == 0x33 || b[0] == 0x55) && n >= 9) {
            cat(t, sizeof t, " %02X", b[1]);
            for (int k = 7; k >= 2; k--) cat(t, sizeof t, "%02X", b[k]);
            bool ok = crc8_maxim(b + 1, 7) == b[8];
            cat(t, sizeof t, ok ? "" : _(" CRC mal"));
            err |= !ok;
            i = 9;
        }
    }
    if (i < n) {
        const char *f = ow_func(b[i]);
        if (f) cat(t, sizeof t, " · %s", f);
        else cat(t, sizeof t, " · %02X", b[i]);
        if (b[i] == 0xBE && n - i - 1 >= 9) {
            const uint8_t *sp = b + i + 1;
            bool ok = crc8_maxim(sp, 8) == sp[8];
            int16_t raw = (int16_t)(sp[0] | sp[1] << 8);
            cat(t, sizeof t, _(" · %.2f °C (DS18B20), CRC %s"), (double)((float)raw / 16.0f), ok ? "ok" : _("mal"));
            err |= !ok;
        } else {
            for (int k = i + 1; k < n && k < i + 12; k++) cat(t, sizeof t, " %02X", b[k]);
        }
    }
    pkt(x, p0, p1, err, "%s", t);
}

static void dec_1wire(dctx_t *x)
{
    const la_cap_t *c = x->c;
    const int ch = x->d->ch[0];
    if (x->rate < 200000) {
        snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], "%s", _("1-Wire: hace falta muestrear a 200 kHz o más"));
        return;
    }
    snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], "1-Wire");
    const float us = x->rate / 1e6f;
    uint32_t pos = 0, b0 = 0, p0 = 0, p1 = 0;
    int nb = 0, v = 0, n = 0;
    uint8_t bytes[48];
    bool open = false, presence = false;
    for (;;) {
        uint32_t f = next_level(c, ch, pos, 0);
        if (f >= c->n) break;
        uint32_t r = next_level(c, ch, f, 1);
        if (r >= c->n) break;
        float w = (float)(r - f) / us;
        pos = r;
        if (w > 380) {
            if (open) ow_flush(x, p0, p1, bytes, n, presence);
            ann(x, f, r, LA_K_CTRL, "Reset");
            /* the presence pulse: low again within 15-60 µs, for 60-240 */
            uint32_t pf = next_level(c, ch, r, 0);
            presence = false;
            if (pf < c->n && (float)(pf - r) / us < 70) {
                uint32_t pr = next_level(c, ch, pf, 1);
                float pw = (float)(pr - pf) / us;
                if (pw >= 40 && pw <= 300) {
                    presence = true;
                    ann(x, pf, pr, LA_K_ACK, _("Presencia"));
                    pos = pr;
                }
            }
            if (!presence) ann(x, r, r + (uint32_t)(60 * us), LA_K_ERR, _("Nadie"));
            open = true;
            p0 = f;
            p1 = pos;
            n = 0;
            nb = 0;
            v = 0;
            continue;
        }
        if (!open || w > 120) continue;
        if (!nb) b0 = f;
        v |= (w < 15 ? 1 : 0) << nb;
        if (++nb == 8) {
            uint32_t end = f + (uint32_t)(65 * us);
            const char *nm = n == 0 ? ow_rom(v) : NULL;
            if (!nm && n > 0 && n < (int)sizeof bytes) {
                bool after_rom = (bytes[0] == 0xCC && n == 1) || ((bytes[0] == 0x55) && n == 9);
                if (after_rom) nm = ow_func(v);
            }
            if (nm) ann(x, b0, end, n == 0 ? LA_K_ADDR : LA_K_CTRL, "%02X %s", v, nm);
            else {
                char s[16];
                fmt_byte(x, (uint32_t)v, 8, s, sizeof s);
                ann(x, b0, end, LA_K_DATA, "%s", s);
            }
            if (n < (int)sizeof bytes) bytes[n++] = (uint8_t)v;
            p1 = end;
            nb = 0;
            v = 0;
        }
    }
    if (open) ow_flush(x, p0, p1, bytes, n, presence);
}

/* ===== CAN ===== */

static void dec_can(dctx_t *x)
{
    const la_cap_t *c = x->c;
    const int ch = x->d->ch[0];
    uint32_t br = x->d->baud ? x->d->baud : 500000;
    const float tb = x->rate / (float)br;
    if (tb < 4) {
        snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], _("CAN: muestreo muy lento para %u kbit/s"), (unsigned)(br / 1000));
        return;
    }
    snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], "CAN %u kbit/s", (unsigned)(br / 1000));
    uint32_t pos = 0;
    while (pos < c->n) {
        uint32_t sof = next_level(c, ch, pos, 0);
        if (sof >= c->n) break;
        /* read the frame, destuffing, resynchronising on falling edges */
        uint32_t sync = sof;
        int since = 0;
        uint8_t bits[160];
        uint32_t at[160];
        int nbits = 0, run = 0, last = -1;
        bool stuff_err = false, ended = false;
        uint32_t s = sof;
        int need = 19;              /* grows as the header says */
        bool ide = false, rtr = false;
        int dlc = 0;
        while (nbits < need) {
            /* an edge where this bit begins: resynchronise there */
            uint32_t win0 = sync + (uint32_t)(((float)since - 0.3f) * tb);
            uint32_t e = la_next_edge(c, ch, win0 > sync ? win0 : sync, c->n);
            if (since && e < sync + (uint32_t)(((float)since + 0.5f) * tb) && e > sync) {
                sync = e;
                since = 0;
            }
            s = sync + (uint32_t)(((float)since + 0.7f) * tb);
            since++;
            if (s >= c->n) {
                ended = true;
                break;
            }
            int b = lvl(c, ch, s);
            if (run == 5) {
                if (b == last) {
                    stuff_err = true;
                    break;
                }
                run = 1;
                last = b;
                continue;
            }
            if (b == last) run++;
            else {
                run = 1;
                last = b;
            }
            at[nbits] = sync + (uint32_t)(((float)since - 1.0f) * tb);
            bits[nbits++] = (uint8_t)b;
            if (nbits == 14) {
                ide = bits[13];
                if (!ide) {
                    rtr = bits[12];
                    need = 19;
                } else {
                    need = 39;
                }
            }
            if (ide && nbits == 33) rtr = bits[32];
            int dl0 = ide ? 35 : 15;
            if (nbits == dl0 + 4) {
                dlc = bits[dl0] << 3 | bits[dl0 + 1] << 2 | bits[dl0 + 2] << 1 | bits[dl0 + 3];
                int nd = rtr ? 0 : (dlc > 8 ? 8 : dlc);
                need = dl0 + 4 + nd * 8 + 15;
            }
        }
        if (ended) break;
        if (stuff_err) {
            ann(x, sof, s, LA_K_ERR, _("Error de relleno"));
            pkt(x, sof, s, true, _("Trama rota (relleno)"));
            /* wait for the bus to rest: 11 recessive bits */
            pos = s;
            for (;;) {
                uint32_t r = next_level(c, ch, pos, 1);
                uint32_t f = next_level(c, ch, r, 0);
                pos = f;
                if (f >= c->n || (float)(f - r) >= 10.5f * tb) break;
            }
            if (pos < c->n) pos -= 1;
            continue;
        }
        /* the fields */
        uint32_t id = 0;
        for (int i = 1; i <= 11; i++) id = id << 1 | bits[i];
        int dl0;
        if (ide) {
            for (int i = 14; i <= 31; i++) id = id << 1 | bits[i];
            dl0 = 35;
        } else {
            dl0 = 15;
        }
        int nd = rtr ? 0 : (dlc > 8 ? 8 : dlc);
        uint16_t crc = 0;
        int crc_at = dl0 + 4 + nd * 8;
        for (int i = 0; i < crc_at; i++) {
            int nx = bits[i] ^ ((crc >> 14) & 1);
            crc = (uint16_t)((crc << 1) & 0x7FFF);
            if (nx) crc ^= 0x4599;
        }
        uint16_t got = 0;
        for (int i = 0; i < 15; i++) got = (uint16_t)(got << 1 | bits[crc_at + i]);
        bool ok = got == crc;
        uint32_t endb = at[crc_at + 14] + (uint32_t)tb;
        ann(x, at[0], at[1], LA_K_START, "SOF");
        int id_end = ide ? 32 : 12;
        ann(x, at[1], at[id_end], LA_K_ADDR, ide ? "ID %08X" : "ID %03X", (unsigned)id);
        ann(x, at[id_end], at[dl0], LA_K_CTRL, rtr ? "RTR" : (ide ? "Ext" : "Std"));
        ann(x, at[dl0], at[dl0 + 4 > nbits - 1 ? nbits - 1 : dl0 + 4], LA_K_CTRL, "DLC %d", dlc);
        char t[86];
        snprintf(t, sizeof t, ide ? "%08X [%d]" : "%03X [%d]", (unsigned)id, dlc);
        if (rtr) cat(t, sizeof t, " RTR");
        for (int k = 0; k < nd; k++) {
            int b0 = dl0 + 4 + k * 8;
            int v = 0;
            for (int i = 0; i < 8; i++) v = v << 1 | bits[b0 + i];
            char sv[16];
            fmt_byte(x, (uint32_t)v, 8, sv, sizeof sv);
            ann(x, at[b0], at[b0 + 7] + (uint32_t)tb, LA_K_DATA, "%s", sv);
            cat(t, sizeof t, " %s", sv);
        }
        ann(x, at[crc_at], endb, ok ? LA_K_ACK : LA_K_ERR, ok ? "CRC ok" : "CRC mal");
        /* after the CRC, no stuffing: delimiter, ACK slot, delimiter, EOF */
        uint32_t ack_s = endb + (uint32_t)(1.7f * tb);
        bool acked = ack_s < c->n && !lvl(c, ch, ack_s);
        ann(x, endb + (uint32_t)tb, endb + (uint32_t)(2 * tb), acked ? LA_K_ACK : LA_K_ERR, acked ? "ACK" : "NAK");
        if (!ok) cat(t, sizeof t, _(" (CRC mal)"));
        if (!acked) cat(t, sizeof t, _(" sin ACK"));
        pkt(x, sof, endb + (uint32_t)(10 * tb), !ok || !acked, "%s", t);
        pos = endb + (uint32_t)(9.5f * tb);
    }
}

/* ===== WS2812 ===== */

static void dec_ws2812(dctx_t *x)
{
    const la_cap_t *c = x->c;
    const int ch = x->d->ch[0];
    if (x->rate < 4000000) {
        snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], "%s", _("WS2812: hace falta muestrear a 4 MHz o más"));
        return;
    }
    snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], "WS2812 (GRB)");
    const float us = x->rate / 1e6f;
    uint32_t pos = 0, l0 = 0, f0 = 0, last = 0;
    int nb = 0, leds = 0;
    uint32_t v = 0;
    char t[86] = "";
    for (;;) {
        uint32_t r = next_level(c, ch, pos, 1);
        if (r >= c->n) break;
        uint32_t f = next_level(c, ch, r, 0);
        if (f >= c->n) break;
        if (leds && (float)(r - last) / us > 40) {
            pkt(x, f0, last, nb != 0, _("%d LEDs: %s"), leds, t);
            leds = 0;
            nb = 0;
            t[0] = 0;
        }
        float h = (float)(f - r) / us;
        if (!nb) l0 = r;
        if (!leds && !nb) f0 = r;
        v = v << 1 | (h > 0.6f ? 1u : 0u);
        last = f;
        pos = f;
        if (++nb == 24) {
            uint32_t rgb = ((v >> 8) & 0xFF) << 16 | ((v >> 16) & 0xFF) << 8 | (v & 0xFF);
            uint32_t end = r + (uint32_t)(1.25f * us);
            ann(x, l0, end, LA_K_DATA, "#%06X", (unsigned)rgb);
            if (leds < 8) cat(t, sizeof t, "%s#%06X", leds ? " " : "", (unsigned)rgb);
            else if (leds == 8) cat(t, sizeof t, " ...");
            leds++;
            nb = 0;
            v = 0;
            last = end;
        }
    }
    if (leds) pkt(x, f0, last, nb != 0, _("%d LEDs: %s"), leds, t);
}

/* ===== NEC infrared, from a receiver's output (low = carrier) ===== */

static void dec_nec(dctx_t *x)
{
    const la_cap_t *c = x->c;
    const int ch = x->d->ch[0];
    const int act = x->d->inv ? 1 : 0;      /* the level while the carrier is on */
    snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], "NEC");
    const float ms = x->rate / 1e3f;
    uint32_t pos = 0;
    for (;;) {
        uint32_t a = next_level(c, ch, pos, act);
        if (a >= c->n) break;
        uint32_t b = next_level(c, ch, a, !act);
        if (b >= c->n) break;
        float mark = (float)(b - a) / ms;
        pos = b;
        if (mark < 7 || mark > 11) continue;
        uint32_t d = next_level(c, ch, b, act);
        if (d >= c->n) break;
        float space = (float)(d - b) / ms;
        if (space > 1.7f && space < 2.8f) {
            ann(x, a, d, LA_K_CTRL, _("Repetir"));
            pkt(x, a, d, false, _("repetición"));
            pos = d;
            continue;
        }
        if (space < 3.5f || space > 5.5f) continue;
        ann(x, a, d, LA_K_START, _("Inicio"));
        uint32_t v = 0, t = d, b0 = d;
        int nb = 0;
        bool bad = false;
        while (nb < 32) {
            uint32_t m1 = next_level(c, ch, t, !act);
            uint32_t m2 = next_level(c, ch, m1, act);
            if (m2 >= c->n) {
                bad = true;
                break;
            }
            float sp = (float)(m2 - m1) / ms;
            if (sp > 2.5f) {
                bad = true;
                break;
            }
            if (!(nb % 8)) b0 = t;
            v |= (sp > 1.1f ? 1u : 0u) << nb;
            nb++;
            if (!(nb % 8)) {
                static const char *const L[4] = { "Dir", "~Dir", "Cmd", "~Cmd" };
                ann(x, b0, m2, nb == 8 ? LA_K_ADDR : nb == 24 ? LA_K_DATA : LA_K_INFO, "%s %02X", L[nb / 8 - 1],
                    (unsigned)((v >> (nb - 8)) & 0xFF));
            }
            t = m2;
        }
        pos = t;
        if (bad) {
            pkt(x, a, t, true, _("trama NEC incompleta"));
            continue;
        }
        uint8_t ad = v & 0xFF, an_ = (v >> 8) & 0xFF, cm = (v >> 16) & 0xFF, cn = (v >> 24) & 0xFF;
        bool cmd_ok = (uint8_t)~cm == cn;
        if ((uint8_t)~ad == an_)
            pkt(x, a, t, !cmd_ok, _("NEC dirección %02X, comando %02X%s"), ad, cm, cmd_ok ? "" : _(" (no cierra)"));
        else
            pkt(x, a, t, !cmd_ok, _("NEC extendido, dirección %04X, comando %02X%s"), (unsigned)(v & 0xFFFF), cm,
                cmd_ok ? "" : _(" (no cierra)"));
    }
}

/* ===== a parallel bus ===== */

static void dec_par(dctx_t *x)
{
    const la_cap_t *c = x->c;
    const la_dec_cfg_t *d = x->d;
    int clk = d->ch[0], nb = 0;
    uint8_t m = 0;
    int pins[8];
    for (int i = 1; i < LA_DEC_PINS; i++)
        if (d->ch[i] >= 0) {
            pins[nb++] = d->ch[i];
            m |= (uint8_t)(1u << d->ch[i]);
        }
    if (!nb) return;
    snprintf(LA.an.summary[x->row], sizeof LA.an.summary[0], clk >= 0 ? _("Paralelo, %d bits con reloj") : _("Paralelo, %d bits"), nb);
    uint32_t pos = 0;
    if (clk >= 0) {
        uint32_t prev = 0;
        bool have = false;
        uint32_t pv = 0;
        for (;;) {
            uint32_t r = next_level(c, clk, pos, 1);
            if (r >= c->n) break;
            uint32_t v = 0;
            for (int i = 0; i < nb; i++) v |= (uint32_t)lvl(c, pins[i], r) << i;
            if (have) {
                char s[16];
                fmt_byte(x, pv, nb, s, sizeof s);
                ann(x, prev, r, LA_K_DATA, "%s", s);
            }
            prev = r;
            pv = v;
            have = true;
            pos = r;
        }
        return;
    }
    uint32_t s0 = 0;
    for (;;) {
        uint32_t e = next_any(c, m, pos, c->n);
        uint32_t v = 0;
        for (int i = 0; i < nb; i++) v |= (uint32_t)lvl(c, pins[i], s0) << i;
        /* lines rarely change at the very same sample: a value held for a
         * couple of samples is the bus moving, not a value */
        if (e - s0 > 2 || e >= c->n) {
            char s[16];
            fmt_byte(x, v, nb, s, sizeof s);
            ann(x, s0, e, LA_K_DATA, "%s", s);
        }
        if (e >= c->n) break;
        s0 = e;
        pos = e;
    }
}

/* ===== the table of decoders ===== */

static const char *const NAMES[LA_DEC_COUNT] = { "UART", "I2C", "SPI", "1-Wire", "CAN", "WS2812", "NEC IR",
                                                 "DMX512", "Modbus RTU", N_("Paralelo") };

const char *la_dec_name(int type) { return type >= 0 && type < LA_DEC_COUNT ? _(NAMES[type]) : "?"; }

int la_dec_npins(int type)
{
    switch (type) {
    case LA_DEC_I2C: return 2;
    case LA_DEC_SPI: return 4;
    case LA_DEC_PAR: return 9;
    default: return 1;
    }
}

const char *la_dec_pin_name(int type, int pin)
{
    static const char *const SPI[4] = { "CLK", "MOSI", "MISO", "CS" };
    static const char *const PAR[9] = { N_("Reloj"), "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7" };
    switch (type) {
    case LA_DEC_UART: return "RX/TX";
    case LA_DEC_I2C: return pin ? "SDA" : "SCL";
    case LA_DEC_SPI: return SPI[pin & 3];
    case LA_DEC_1WIRE: return "DQ";
    case LA_DEC_CAN: return "RX";
    case LA_DEC_WS2812: return "DIN";
    case LA_DEC_NEC: return "IR";
    case LA_DEC_DMX: return "DMX";
    case LA_DEC_MODBUS: return "A/B (RO)";
    case LA_DEC_PAR: return pin < 9 ? _(PAR[pin]) : "?";
    default: return "?";
    }
}

bool la_dec_pin_needed(int type, int pin)
{
    switch (type) {
    case LA_DEC_I2C: return true;
    case LA_DEC_SPI: return pin == 0;
    case LA_DEC_PAR: return pin == 1;
    default: return pin == 0;
    }
}

void la_dec_defaults(la_dec_cfg_t *d, int type)
{
    memset(d, 0, sizeof *d);
    d->type = (uint8_t)type;
    d->on = true;
    for (int i = 0; i < LA_DEC_PINS; i++) d->ch[i] = -1;
    d->bits = 8;
    d->parity = 'N';
    d->stop = 1;
    d->fmt = LA_FMT_HEX;
    switch (type) {
    case LA_DEC_I2C: d->ch[0] = 0; d->ch[1] = 1; break;
    case LA_DEC_SPI: for (int i = 0; i < 4; i++) d->ch[i] = (int8_t)i; break;
    case LA_DEC_CAN: d->baud = 500000; d->ch[0] = 0; break;
    case LA_DEC_DMX: d->baud = 250000; d->stop = 2; d->ch[0] = 0; d->fmt = LA_FMT_DEC; break;
    case LA_DEC_MODBUS: d->baud = 9600; d->ch[0] = 0; break;
    case LA_DEC_UART: d->fmt = LA_FMT_ASCII; d->ch[0] = 0; break;
    case LA_DEC_PAR: for (int i = 0; i < 8; i++) d->ch[1 + i] = (int8_t)i; break;
    default: d->ch[0] = 0; break;
    }
}

void la_dec_describe(const la_dec_cfg_t *d, char *out, size_t n)
{
    char pins[48] = "";
    int np = la_dec_npins(d->type);
    for (int i = 0; i < np; i++)
        if (d->ch[i] >= 0) cat(pins, sizeof pins, "%s%s %d", pins[0] ? ", " : "", la_dec_pin_name(d->type, i), d->ch[i]);
    switch (d->type) {
    case LA_DEC_UART:
    case LA_DEC_MODBUS:
        if (d->baud) snprintf(out, n, "%s %u %d%c%d · %s", la_dec_name(d->type), (unsigned)d->baud, d->bits, d->parity, d->stop, pins);
        else snprintf(out, n, "%s, %s · %s", la_dec_name(d->type), _("velocidad sola"), pins);
        break;
    case LA_DEC_CAN:
        snprintf(out, n, "CAN %u kbit/s · %s", (unsigned)(d->baud / 1000), pins);
        break;
    case LA_DEC_SPI:
        snprintf(out, n, _("SPI modo %d · %s"), d->mode, pins);
        break;
    default:
        snprintf(out, n, "%s · %s", la_dec_name(d->type), pins);
        break;
    }
}

bool la_anns_alloc(void)
{
    if (!LA.an.ann) LA.an.ann = la_big_alloc((size_t)LA_ANN_MAX * sizeof(la_ann_t));
    if (!LA.an.pkt) LA.an.pkt = la_big_alloc((size_t)LA_PKT_MAX * sizeof(la_pkt_t));
    return LA.an.ann && LA.an.pkt;
}

void la_anns_free(void)
{
    la_big_free(LA.an.ann);
    la_big_free(LA.an.pkt);
    LA.an.ann = NULL;
    LA.an.pkt = NULL;
    LA.an.nann = LA.an.npkt = 0;
}

static int pkt_cmp(const void *a, const void *b)
{
    const la_pkt_t *x = a, *y = b;
    return x->s0 < y->s0 ? -1 : x->s0 > y->s0;
}

static int ann_cmp(const void *a, const void *b)
{
    const la_ann_t *x = a, *y = b;
    return x->s0 < y->s0 ? -1 : x->s0 > y->s0;
}

void la_decode_all(void)
{
    LA.an.nann = LA.an.npkt = 0;
    LA.an.full = false;
    memset(LA.an.row_first, 0, sizeof LA.an.row_first);
    memset(LA.an.row_n, 0, sizeof LA.an.row_n);
    memset(LA.an.summary, 0, sizeof LA.an.summary);
    if (!LA.cap.buf || !LA.cap.n || !la_anns_alloc()) {
        LA.an.seq++;
        return;
    }
    la_lock();
    LA.state = LA_ST_DECODE;
    la_unlock();
    for (int r = 0; r < LA_DEC_MAX; r++) {
        const la_dec_cfg_t *d = &LA.dec[r];
        LA.an.row_first[r] = LA.an.nann;
        if (!d->on || LA.stop) continue;
        bool ok = true;
        for (int i = 0; i < la_dec_npins(d->type); i++) {
            if (d->ch[i] >= LA_NCH) ok = false;
            if (la_dec_pin_needed(d->type, i) && d->ch[i] < 0) ok = false;
        }
        if (!ok) {
            snprintf(LA.an.summary[r], sizeof LA.an.summary[0], "%s: %s", la_dec_name(d->type), _("faltan canales"));
            continue;
        }
        dctx_t x = { &LA.cap, d, r, (float)LA.cap.rate };
        switch (d->type) {
        case LA_DEC_UART:   dec_uart(&x); break;
        case LA_DEC_I2C:    dec_i2c(&x); break;
        case LA_DEC_SPI:    dec_spi(&x); break;
        case LA_DEC_1WIRE:  dec_1wire(&x); break;
        case LA_DEC_CAN:    dec_can(&x); break;
        case LA_DEC_WS2812: dec_ws2812(&x); break;
        case LA_DEC_NEC:    dec_nec(&x); break;
        case LA_DEC_DMX:    dec_dmx(&x); break;
        case LA_DEC_MODBUS: dec_modbus(&x); break;
        case LA_DEC_PAR:    dec_par(&x); break;
        default: break;
        }
        LA.an.row_n[r] = LA.an.nann - LA.an.row_first[r];
        /* the I2C ACK trick and the resynchronised CAN fields can leave a
         * row slightly out of order */
        if (LA.an.row_n[r] > 1)
            qsort(LA.an.ann + LA.an.row_first[r], LA.an.row_n[r], sizeof(la_ann_t), ann_cmp);
    }
    if (LA.an.npkt > 1) qsort(LA.an.pkt, LA.an.npkt, sizeof(la_pkt_t), pkt_cmp);
    LA.an.seq++;
    aos_hal_log("logica", "decoded: %u annotations, %u packets%s", (unsigned)LA.an.nann, (unsigned)LA.an.npkt,
                LA.an.full ? " (full)" : "");
}

uint32_t la_ann_first(int row, uint32_t s)
{
    uint32_t lo = LA.an.row_first[row], hi = lo + LA.an.row_n[row];
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (LA.an.ann[mid].s1 <= s) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
