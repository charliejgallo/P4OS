/*
 * Malla - the stream links: serial and TCP, and the framing they share.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * Both carry the API as a byte stream with each protobuf framed as
 * 0x94 0xC3 <len hi> <len lo> <len bytes>. A frame longer than 512 is not a
 * frame: the framer goes back to hunting, which is also how it finds its
 * feet after joining a stream in the middle. Over serial the node prints its
 * debug log on the same port when that is on, so whatever comes between
 * frames is gathered into lines and handed to the session, which keeps the
 * last ones for the connection's screen.
 *
 * Serial is aos_io's UART: the header's ports (uart.a...) and the USB host's
 * serial devices (usb0, usb1: CDC-ACM, CH34x, CP210x, FTDI), all the same
 * call. Opening a USB port raises DTR and RTS, as a computer's terminal does;
 * on an ESP32 board with the usual two-transistor reset that is "run", and
 * an nRF52 or an RP2040 on its own USB starts talking once DTR is up.
 * Before the first frame the client sends 32 bytes of 0xC3, as Meshtastic's
 * own clients do: a node whose serial port was in its text console mode
 * takes that as "the API from here on".
 */
#include "msh_link.h"
#include "msh_util.h"

#include "aos_hal.h"
#include "aos_io.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the framing ---- */

void msh_framer_reset(msh_framer_t *f)
{
    f->state = 0;
    f->want = f->have = 0;
    f->text_len = 0;
}

static void text_byte(msh_framer_t *f, uint8_t c, void (*on_text)(void *, const char *), void *ctx)
{
    if (c == '\r') return;
    if (c == '\n' || f->text_len >= (int)sizeof f->text - 1) {
        f->text[f->text_len] = 0;
        if (f->text_len && on_text) on_text(ctx, f->text);
        f->text_len = 0;
        if (c == '\n') return;
    }
    /* the log carries ANSI colours: keep only what prints */
    if (c >= 0x20 || c == '\t') f->text[f->text_len++] = (char)c;
}

size_t msh_framer_feed(msh_framer_t *f, const uint8_t *in, size_t len, int *got,
                       void (*on_text)(void *, const char *), void *ctx)
{
    *got = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t c = in[i++];
        switch (f->state) {
        case 0:
            if (c == 0x94) f->state = 1;
            else text_byte(f, c, on_text, ctx);
            break;
        case 1:
            if (c == 0xC3) f->state = 2;
            else {
                text_byte(f, 0x94, on_text, ctx);
                f->state = c == 0x94 ? 1 : 0;
                if (c != 0x94) text_byte(f, c, on_text, ctx);
            }
            break;
        case 2:
            f->want = (uint16_t)(c << 8);
            f->state = 3;
            break;
        case 3:
            f->want |= c;
            f->have = 0;
            if (f->want > MSH_FRAME_MAX) {
                f->state = 0;           /* not a frame: hunt again */
            } else if (f->want == 0) {
                f->state = 0;           /* an empty protobuf: nothing to hand over */
            } else {
                f->state = 4;
            }
            break;
        case 4:
            f->buf[f->have++] = c;
            if (f->have == f->want) {
                f->state = 0;
                *got = f->want;
                return i;
            }
            break;
        }
    }
    return i;
}

void msh_frame_header(uint8_t hdr[4], int len)
{
    hdr[0] = 0x94;
    hdr[1] = 0xC3;
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)len;
}

/* ---- what both stream links keep ---- */

typedef struct {
    aos_io_uart_t *uart;            /* serial */
    int            fd;              /* TCP handle, 0 none */
    msh_framer_t   fr;
    uint8_t        rx[1024];
    size_t         rx_len, rx_pos;
    bool           woke;
} stream_t;

static stream_t *stream_of(msh_link_t *l)
{
    if (!l->priv) {
        l->priv = msh_alloc(sizeof(stream_t));
        if (l->priv) msh_framer_reset(&((stream_t *)l->priv)->fr);
    }
    return (stream_t *)l->priv;
}

static void on_text(void *ctx, const char *line)
{
    msh_link_t *l = (msh_link_t *)ctx;
    if (l->log_line) l->log_line(l->log_ctx, line);
}

/* Takes what is buffered and, when it runs out, reads more with rd(). */
static int stream_recv(msh_link_t *l, uint8_t *buf, int max, int timeout_ms,
                       int (*rd)(stream_t *s, uint8_t *b, int n, int ms))
{
    stream_t *s = (stream_t *)l->priv;
    uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
    for (;;) {
        if (s->rx_pos < s->rx_len) {
            int got = 0;
            s->rx_pos += msh_framer_feed(&s->fr, s->rx + s->rx_pos, s->rx_len - s->rx_pos, &got, on_text, l);
            if (got > 0) {
                if (got > max) continue;        /* cannot be: max is MSH_FRAME_MAX */
                memcpy(buf, s->fr.buf, (size_t)got);
                return got;
            }
            continue;
        }
        int left = timeout_ms - (int)((uint32_t)aos_hal_uptime_ms() - t0);
        if (left < 0) return 0;
        int n = rd(s, s->rx, sizeof s->rx, left > 0 ? left : 1);
        if (n < 0) return -1;
        if (n == 0) return 0;
        s->rx_len = (size_t)n;
        s->rx_pos = 0;
    }
}

/* ---- serial ---- */

static bool serial_open(msh_link_t *l, char *err, size_t err_len)
{
    stream_t *s = stream_of(l);
    if (!s) { snprintf(err, err_len, "%s", N_("Sin memoria")); return false; }
    s->uart = aos_io_uart_open(l->t.port, l->t.baud ? l->t.baud : 115200, false, "Malla");
    if (!s->uart) {
        snprintf(err, err_len, "%s", N_("No se pudo abrir el puerto"));
        return false;
    }
    s->rx_len = s->rx_pos = 0;
    s->woke = false;
    msh_framer_reset(&s->fr);
    return true;
}

static int serial_rd(stream_t *s, uint8_t *b, int n, int ms)
{
    int r = aos_io_uart_read(s->uart, b, n, ms);
    return r < 0 ? -1 : r;
}

static int serial_recv(msh_link_t *l, uint8_t *buf, int max, int timeout_ms, char *err, size_t err_len)
{
    int r = stream_recv(l, buf, max, timeout_ms, serial_rd);
    if (r < 0) snprintf(err, err_len, "%s", N_("Se desconectó el puerto"));
    return r;
}

static bool serial_send(msh_link_t *l, const uint8_t *buf, int len)
{
    stream_t *s = (stream_t *)l->priv;
    if (!s || !s->uart) return false;
    if (!s->woke) {
        uint8_t wake[32];
        memset(wake, 0xC3, sizeof wake);
        aos_io_uart_write(s->uart, wake, sizeof wake);
        aos_hal_sleep_ms(100);
        s->woke = true;
    }
    uint8_t hdr[4];
    msh_frame_header(hdr, len);
    return aos_io_uart_write(s->uart, hdr, 4) == 4 && aos_io_uart_write(s->uart, buf, len) == len;
}

static void serial_close(msh_link_t *l)
{
    stream_t *s = (stream_t *)l->priv;
    if (s && s->uart) {
        aos_io_uart_close(s->uart);
        s->uart = NULL;
    }
}

const msh_link_ops_t msh_link_serial_ops = { serial_open, serial_recv, serial_send, serial_close };

/* ---- TCP ---- */

static bool tcp_open(msh_link_t *l, char *err, size_t err_len)
{
    stream_t *s = stream_of(l);
    if (!s) { snprintf(err, err_len, "%s", N_("Sin memoria")); return false; }
    int fd = aos_hal_tcp_connect(l->t.host, l->t.tcp_port > 0 ? l->t.tcp_port : 4403, 6000);
    if (fd <= 0) {
        snprintf(err, err_len, "%s", fd == AOS_TCP_ERR_DNS ? N_("No se encontró la dirección")
                                   : fd == AOS_TCP_ERR_SLOTS ? N_("No quedan conexiones libres")
                                   : N_("El nodo no contesta"));
        return false;
    }
    s->fd = fd;
    s->rx_len = s->rx_pos = 0;
    msh_framer_reset(&s->fr);
    return true;
}

static int tcp_rd(stream_t *s, uint8_t *b, int n, int ms)
{
    int r = aos_hal_tcp_recv(s->fd, b, n, ms);
    if (r == AOS_TCP_ERR_TIMEOUT) return 0;
    return r < 0 ? -1 : r;
}

static int tcp_recv(msh_link_t *l, uint8_t *buf, int max, int timeout_ms, char *err, size_t err_len)
{
    int r = stream_recv(l, buf, max, timeout_ms, tcp_rd);
    if (r < 0) snprintf(err, err_len, "%s", N_("El nodo cerró la conexión"));
    return r;
}

static bool tcp_send(msh_link_t *l, const uint8_t *buf, int len)
{
    stream_t *s = (stream_t *)l->priv;
    if (!s || s->fd <= 0) return false;
    uint8_t out[4 + MSH_FRAME_MAX];
    if (len > MSH_FRAME_MAX) return false;
    msh_frame_header(out, len);
    memcpy(out + 4, buf, (size_t)len);
    return aos_hal_tcp_send(s->fd, out, len + 4, 3000) == len + 4;
}

static void tcp_close(msh_link_t *l)
{
    stream_t *s = (stream_t *)l->priv;
    if (s && s->fd > 0) {
        aos_hal_tcp_close(s->fd);
        s->fd = 0;
    }
}

const msh_link_ops_t msh_link_tcp_ops = { tcp_open, tcp_recv, tcp_send, tcp_close };

/* ---- a link for a target ---- */

msh_link_t *msh_link_new(const msh_target_t *t)
{
    const msh_link_ops_t *ops = t->kind == MSH_LINK_SERIAL ? &msh_link_serial_ops
                              : t->kind == MSH_LINK_TCP    ? &msh_link_tcp_ops
                              : t->kind == MSH_LINK_BLE    ? &msh_link_ble_ops : NULL;
    if (!ops) return NULL;
    msh_link_t *l = (msh_link_t *)msh_alloc(sizeof *l);
    if (!l) return NULL;
    l->ops = ops;
    l->t = *t;
    l->note = "";
    return l;
}

void msh_link_free(msh_link_t *l)
{
    if (!l) return;
    l->ops->close(l);
    free(l->priv);
    free(l);
}
