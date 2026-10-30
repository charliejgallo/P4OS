/*
 * P4OS - Programmer: the board as the computer's debug probe (F5,
 * docs/plan/PROGRAMADOR.md). A job that holds a port's SWD pins, as the SWD
 * family does, and puts the USB in PROBE mode: a CMSIS-DAP v2 interface on
 * the OTG connector (aos_usb_p4.c). OpenOCD, pyOCD and probe-rs find it with
 * no driver and debug through it - breakpoints, stepping, the variables, and
 * their own flash algorithms for chips the Programador does not know.
 *
 * This file is the probe's firmware: the CMSIS-DAP commands, SWD only, as
 * Arm's reference implementation (CMSIS-DAP 2.1, DAP.c) does them, posted AP
 * reads and value matching included, on aos_io's SWD wire - the same
 * calibrated bit-bang as the SWD family's, so the simulator's emulated
 * targets (sim/swd_sim.c) answer it too. Not free-dap, which the plan named:
 * it drives the pins itself, and that would leave the simulator and the
 * calibration out.
 *
 * Not here: JTAG, SWO, the UART bridge and the atomic queue
 * (DAP_QueueCommands); DAP_Info says so and the debuggers do without them.
 * DAP_ExecuteCommands is answered anyway. A TARGETSEL write (multidrop: the
 * RP2040) has no ACK; the debuggers send it as a DAP_SWD_Sequence, which is
 * here.
 *
 * One command per packet, answered in order: the computer sends up to
 * DAP_PACKETS before it waits for the first answer (the queue in
 * aos_usb_p4.c holds as many).
 */
#include "aos_flasher_priv.h"
#include "aos_i18n.h"
#include "aos_hal.h"
#include "aos_io.h"

#include <string.h>

#define DAP_PACKETS     4
#define DAP_PACKET_MAX  512
#define KHZ_DEFAULT     1000

/* commands */
enum {
    ID_INFO = 0x00, ID_HOST_STATUS = 0x01, ID_CONNECT = 0x02, ID_DISCONNECT = 0x03,
    ID_TRANSFER_CONFIGURE = 0x04, ID_TRANSFER = 0x05, ID_TRANSFER_BLOCK = 0x06, ID_TRANSFER_ABORT = 0x07,
    ID_WRITE_ABORT = 0x08, ID_DELAY = 0x09, ID_RESET_TARGET = 0x0A,
    ID_SWJ_PINS = 0x10, ID_SWJ_CLOCK = 0x11, ID_SWJ_SEQUENCE = 0x12, ID_SWD_CONFIGURE = 0x13,
    ID_SWD_SEQUENCE = 0x1D, ID_EXECUTE_COMMANDS = 0x7F, ID_INVALID = 0xFF,
};
#define DAP_OK          0x00
#define DAP_ERROR       0xFF

/* a transfer request's bits, and its response's */
#define T_APnDP         0x01
#define T_RnW           0x02
#define T_MATCH_VALUE   0x10
#define T_MATCH_MASK    0x20
#define T_TIMESTAMP     0x80
#define T_OK            0x01
#define T_WAIT          0x02
#define T_ERROR         0x08
#define T_MISMATCH      0x10
#define DP_RDBUFF       0x0C

typedef struct {
    aos_io_swd_t *w;
    int      packet;
    uint8_t  idle;              /* idle cycles after each transfer */
    uint16_t wait_retry, match_retry;
    uint32_t match_mask;
    uint32_t khz;
    bool     connected;
    bool     reset_low;         /* the debugger holds NRST */
    uint32_t cmds, xfers;
    uint8_t  in[DAP_PACKET_MAX], out[DAP_PACKET_MAX];
} dap_t;

/* ---- the request and the response, bounded ---- */

typedef struct { const uint8_t *p; int n; } rd_t;
typedef struct { uint8_t *p; int n, cap; } wr_t;

static uint8_t get8(rd_t *r)
{
    if (r->n < 1) { r->n = -1; return 0; }
    r->n--;
    return *r->p++;
}
static uint16_t get16(rd_t *r) { uint16_t v = get8(r); return (uint16_t)(v | get8(r) << 8); }
static uint32_t get32(rd_t *r) { uint32_t v = get16(r); return v | (uint32_t)get16(r) << 16; }

static bool room(const wr_t *w, int n) { return w->n + n <= w->cap; }
static void put8(wr_t *w, uint8_t v) { if (room(w, 1)) w->p[w->n++] = v; }
static void put32(wr_t *w, uint32_t v) { for (int i = 0; i < 4; i++) put8(w, (uint8_t)(v >> 8 * i)); }

/* ---- the wire ---- */

static void idle_cycles(dap_t *d)
{
    /* the wire already ends each packet with two */
    static const uint8_t ZERO[32] = { 0 };
    if (d->idle > 2) aos_io_swd_seq(d->w, ZERO, d->idle - 2);
}

/* One SWD transfer from a DAP request's low bits (APnDP, RnW, A2, A3), with
 * the WAITs retried; returns the DAP's ACK bits (T_OK, T_WAIT, 0x04 FAULT,
 * 0x07 no answer) or T_ERROR for a bad parity. */
static uint8_t swd(dap_t *d, uint8_t request, uint32_t *data)
{
    uint8_t a = request & 0x0F;
    uint8_t par = (uint8_t)((a ^ a >> 1 ^ a >> 2 ^ a >> 3) & 1);
    uint8_t req = (uint8_t)(0x81 | a << 1 | par << 5);
    uint32_t scratch = 0;
    if (!data) data = &scratch;
    int ack;
    uint32_t retry = d->wait_retry;
    do {
        ack = aos_io_swd_xfer(d->w, req, data, false);
        d->xfers++;
        idle_cycles(d);
    } while (ack == 2 && retry--);
    return ack == -1 ? T_ERROR : (uint8_t)ack;
}

/* ---- DAP_Info ---- */

static void info(dap_t *d, rd_t *r, wr_t *w)
{
    uint8_t id = get8(r);
    const char *str = NULL;
    switch (id) {
    case 0x01: str = "P4OS"; break;                     /* vendor */
    case 0x02: str = "P4OS CMSIS-DAP"; break;           /* product */
    case 0x04: str = "2.1.1"; break;                    /* the protocol's version */
    case 0x09: str = "1.0"; break;                      /* this firmware's */
    case 0xF0: put8(w, 1); put8(w, 0x01); return;       /* capabilities: SWD only */
    case 0xFE: put8(w, 1); put8(w, DAP_PACKETS); return;
    case 0xFF: put8(w, 2); put8(w, (uint8_t)d->packet); put8(w, (uint8_t)(d->packet >> 8)); return;
    default: break;                                     /* the rest: not said */
    }
    if (!str) { put8(w, 0); return; }
    int n = (int)strlen(str) + 1;
    put8(w, (uint8_t)n);
    for (int i = 0; i < n; i++) put8(w, (uint8_t)str[i]);
}

/* ---- DAP_Transfer: Arm's DAP_SWD_Transfer, with its posted reads ---- */

static void transfer(dap_t *d, rd_t *r, wr_t *w)
{
    get8(r);                                    /* the DAP index: one DAP */
    int count = get8(r);
    int head = w->n;
    put8(w, 0);
    put8(w, 0);
    int done = 0;
    uint8_t resp = 0;
    bool post_read = false, check_write = false;
    uint32_t data;
    for (; count > 0; count--) {
        uint8_t q = get8(r);
        if (r->n < 0) break;
        if (q & T_RnW) {
            if (post_read) {
                /* the AP read posted before: an AP read without a match
                 * brings it and posts its own, anything else asks RDBUFF */
                if ((q & (T_APnDP | T_MATCH_VALUE)) == T_APnDP) {
                    resp = swd(d, q, &data);
                } else {
                    resp = swd(d, DP_RDBUFF | T_RnW, &data);
                    post_read = false;
                }
                if (resp != T_OK) break;
                if (!room(w, 4)) { resp = T_ERROR; break; }
                put32(w, data);
            }
            if (q & T_MATCH_VALUE) {
                uint32_t match = get32(r);
                if (q & T_APnDP) {
                    resp = swd(d, q, NULL);
                    if (resp != T_OK) break;
                }
                uint32_t retry = d->match_retry;
                do {
                    resp = swd(d, q, &data);
                    if (resp != T_OK) break;
                } while ((data & d->match_mask) != match && retry--);
                if (resp == T_OK && (data & d->match_mask) != match) resp |= T_MISMATCH;
                if (resp != T_OK) break;
            } else if (q & T_APnDP) {
                if (!post_read) {
                    resp = swd(d, q, NULL);
                    if (resp != T_OK) break;
                    post_read = true;
                }
            } else {
                resp = swd(d, q, &data);
                if (resp != T_OK) break;
                if (!room(w, 4)) { resp = T_ERROR; break; }
                put32(w, data);
            }
            check_write = false;
        } else {
            if (post_read) {
                resp = swd(d, DP_RDBUFF | T_RnW, &data);
                if (resp != T_OK) break;
                if (!room(w, 4)) { resp = T_ERROR; break; }
                put32(w, data);
                post_read = false;
            }
            data = get32(r);
            if (q & T_MATCH_MASK) {
                d->match_mask = data;
                resp = T_OK;
            } else {
                resp = swd(d, q, &data);
                if (resp != T_OK) break;
                check_write = true;
            }
        }
        done++;
    }
    if (resp == T_OK) {
        if (post_read) {
            resp = swd(d, DP_RDBUFF | T_RnW, &data);
            if (resp == T_OK) {
                if (room(w, 4)) put32(w, data);
                else resp = T_ERROR;
            }
        } else if (check_write) {
            resp = swd(d, DP_RDBUFF | T_RnW, NULL);
        }
    }
    w->p[head] = (uint8_t)done;
    w->p[head + 1] = resp;
}

/* ---- DAP_TransferBlock: one register, many times ---- */

static void transfer_block(dap_t *d, rd_t *r, wr_t *w)
{
    get8(r);
    int count = get16(r);
    uint8_t q = get8(r);
    int head = w->n;
    put8(w, 0);
    put8(w, 0);
    put8(w, 0);
    int done = 0;
    uint8_t resp = 0;
    uint32_t data;
    if (count && r->n >= 0) {
        if (q & T_RnW) {
            if (q & T_APnDP) resp = swd(d, q, NULL);        /* the first one posted */
            if (!(q & T_APnDP) || resp == T_OK) {
                while (count--) {
                    /* the last AP read's value is in RDBUFF */
                    uint8_t qq = !count && (q & T_APnDP) ? DP_RDBUFF | T_RnW : q;
                    if (!room(w, 4)) { resp = T_ERROR; break; }
                    resp = swd(d, qq, &data);
                    if (resp != T_OK) break;
                    put32(w, data);
                    done++;
                }
            }
        } else {
            while (count--) {
                data = get32(r);
                if (r->n < 0) { resp = T_ERROR; break; }
                resp = swd(d, q, &data);
                if (resp != T_OK) break;
                done++;
            }
            if (resp == T_OK) resp = swd(d, DP_RDBUFF | T_RnW, NULL);     /* the last write landed */
        }
    }
    w->p[head] = (uint8_t)done;
    w->p[head + 1] = (uint8_t)(done >> 8);
    w->p[head + 2] = resp;
}

/* ---- the rest ---- */

static void delay_us(uint32_t us)
{
    uint64_t end = aos_hal_uptime_us() + us;
    while (aos_hal_uptime_us() < end) {}
}

static void swj_pins(dap_t *d, rd_t *r, wr_t *w)
{
    uint8_t out = get8(r), sel = get8(r);
    get32(r);                   /* the wait: what is read back is what was asked */
    /* SWCLK and SWDIO are the wire's; only nRESET is the debugger's to move */
    if (sel & 0x80) {
        bool low = !(out & 0x80);
        if (aos_io_swd_nrst(d->w, low)) d->reset_low = low;
    }
    put8(w, (uint8_t)(0x03 | (d->reset_low ? 0 : 0x80)));
}

static void swj_clock(dap_t *d, rd_t *r, wr_t *w)
{
    uint32_t hz = get32(r);
    if (!hz) { put8(w, DAP_ERROR); return; }
    uint32_t khz = hz < 1000 ? 1 : hz / 1000;
    if (khz > 20000) khz = 20000;
    if (khz != d->khz) {
        fl_logf(L_INFO, _("El depurador pidió SWCLK a %u kHz"), (unsigned)khz);
        aos_io_swd_clock(d->w, khz);
        d->khz = khz;
    }
    put8(w, DAP_OK);
}

static void swj_sequence(dap_t *d, rd_t *r, wr_t *w)
{
    int n = get8(r);
    if (!n) n = 256;
    uint8_t bits[32];
    for (int i = 0; i < (n + 7) / 8; i++) bits[i] = get8(r);
    if (r->n < 0) { put8(w, DAP_ERROR); return; }
    aos_io_swd_seq(d->w, bits, n);
    put8(w, DAP_OK);
}

static void swd_sequence(dap_t *d, rd_t *r, wr_t *w)
{
    int count = get8(r);
    int head = w->n;
    put8(w, DAP_OK);
    while (count-- > 0) {
        uint8_t inf = get8(r);
        int n = inf & 0x3F;
        if (!n) n = 64;
        uint8_t bits[8] = { 0 };
        if (inf & 0x80) {
            aos_io_swd_seq_in(d->w, bits, n);
            for (int i = 0; i < (n + 7) / 8; i++) put8(w, bits[i]);
        } else {
            for (int i = 0; i < (n + 7) / 8; i++) bits[i] = get8(r);
            if (r->n < 0) break;
            aos_io_swd_seq(d->w, bits, n);
        }
    }
    if (r->n < 0) w->p[head] = DAP_ERROR;
}

/* One command from r into w; false for DAP_TransferAbort, which has no
 * response (and nothing to abort: the commands run one after the other). */
static bool command(dap_t *d, rd_t *r, wr_t *w, int depth)
{
    uint8_t id = get8(r);
    if (id == ID_TRANSFER_ABORT) return false;
    put8(w, id);
    switch (id) {
    case ID_INFO: info(d, r, w); break;
    case ID_HOST_STATUS: get8(r); get8(r); put8(w, DAP_OK); break;
    case ID_CONNECT: {
        uint8_t port = get8(r);
        bool swd_port = port == 0 || port == 1;
        if (swd_port && !d->connected) fl_logf(L_OK, "%s", _("El depurador se conectó (SWD)"));
        d->connected |= swd_port;
        put8(w, swd_port ? 1 : 0);
        break;
    }
    case ID_DISCONNECT:
        if (d->connected) fl_logf(L_INFO, "%s", _("El depurador se desconectó"));
        d->connected = false;
        put8(w, DAP_OK);
        break;
    case ID_TRANSFER_CONFIGURE:
        d->idle = get8(r);
        d->wait_retry = get16(r);
        d->match_retry = get16(r);
        put8(w, DAP_OK);
        break;
    case ID_TRANSFER: transfer(d, r, w); break;
    case ID_TRANSFER_BLOCK: transfer_block(d, r, w); break;
    case ID_WRITE_ABORT: {
        get8(r);
        uint32_t v = get32(r);
        put8(w, swd(d, 0x00, &v) == T_OK ? DAP_OK : DAP_ERROR);    /* DP ABORT */
        break;
    }
    case ID_DELAY: delay_us(get16(r)); put8(w, DAP_OK); break;
    case ID_RESET_TARGET: put8(w, DAP_OK); put8(w, 0); break;     /* no reset of its own: the debugger's */
    case ID_SWJ_PINS: swj_pins(d, r, w); break;
    case ID_SWJ_CLOCK: swj_clock(d, r, w); break;
    case ID_SWJ_SEQUENCE: swj_sequence(d, r, w); break;
    case ID_SWD_CONFIGURE: put8(w, get8(r) == 0 ? DAP_OK : DAP_ERROR); break;   /* one turnaround, no data phase */
    case ID_SWD_SEQUENCE: swd_sequence(d, r, w); break;
    case ID_EXECUTE_COMMANDS: {
        int n = get8(r);
        put8(w, (uint8_t)n);
        while (depth == 0 && n-- > 0 && r->n > 0) command(d, r, w, depth + 1);
        break;
    }
    default:
        /* JTAG, SWO, the UART, vendor commands: unknown here */
        w->p[w->n - 1] = ID_INVALID;
        r->n = 0;
        break;
    }
    return true;
}

/* ---- the job ---- */

static bool usb_settle(aos_hal_usb_mode_t want, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 50) {
        if (!aos_hal_usb_busy() && aos_hal_usb_mode() == want) return true;
        aos_hal_sleep_ms(50);
    }
    return false;
}

static void publish(const dap_t *d)
{
    fl_lock();
    fl_st.done = d->cmds;
    fl_st.total = d->xfers;
    fl_unlock();
}

static bool serve(dap_t *d)
{
    bool ok = true, host = false;
    uint64_t shown = 0;
    while (ok && !fl_cancel) {
        if (!aos_hal_usb_busy() && aos_hal_usb_mode() != AOS_HAL_USB_PROBE) {
            ok = fl_fail("%s", _("El USB cambió de modo (Ajustes): la sonda se cerró"));
            break;
        }
        bool now = aos_hal_usb_connected();
        if (now != host) {
            fl_logf(L_INFO, "%s", now ? _("La computadora tomó la sonda") : _("La computadora soltó la sonda"));
            host = now;
            d->packet = aos_hal_usb_probe_packet();
        }
        int n = aos_hal_usb_probe_read(d->in, DAP_PACKET_MAX, 200);
        if (n > 0) {
            rd_t r = { d->in, n };
            wr_t w = { d->out, 0, d->packet };
            d->cmds++;
            if (command(d, &r, &w, 0) && w.n) aos_hal_usb_probe_write(d->out, w.n);
        }
        /* the counters a few times a second, not at every command */
        uint64_t t = aos_hal_uptime_ms();
        if (t - shown >= 250) { publish(d); shown = t; }
    }
    publish(d);
    return ok;
}

bool fl_probe_run(void)
{
    fl_phase(AOS_FLASHER_OPENING);
    aos_hal_usb_mode_t before = aos_hal_usb_mode(), now = before;
    if (before == AOS_HAL_USB_DISK)
        return fl_fail("%s", _("El USB está en modo Disco: expulsá la tarjeta en la computadora primero"));
    /* the pendrive host lends the OTG controller and takes it back after
     * (aos_usb_p4.c): the port goes back to idle, and the host with it */
    if (before == AOS_HAL_USB_HOST) {
        fl_logf(L_INFO, "%s", _("El host de pendrives le presta el conector OTG a la sonda"));
        before = AOS_HAL_USB_CONSOLE;
    }
    /* the state and both packets in PSRAM (aos_hal_io_alloc), not the
     * internal RAM a plain malloc of this size would take */
    dap_t *d = aos_hal_io_alloc(sizeof *d);
    if (!d) return fl_fail("%s", _("Sin memoria"));
    memset(d, 0, sizeof *d);
    d->khz = J.opts.baud && J.opts.baud <= 20000 ? J.opts.baud : KHZ_DEFAULT;
    d->wait_retry = 100;
    d->match_mask = 0xFFFFFFFF;
    d->packet = 64;
    d->w = fl_swd_open(d->khz);
    bool ok = false;
    if (d->w) {
        if (!usb_settle(now, 5000) || !aos_hal_usb_mode_set(AOS_HAL_USB_PROBE) || !usb_settle(AOS_HAL_USB_PROBE, 5000)) {
            fl_fail("%s", _("El USB no pasó a modo sonda"));
        } else {
            d->packet = aos_hal_usb_probe_packet();
            fl_phase(AOS_FLASHER_PROBE);
            fl_logf(L_OK, "%s", _("Sonda CMSIS-DAP lista en el conector OTG"));
            ok = serve(d);
        }
        if (d->reset_low) aos_io_swd_nrst(d->w, false);
        aos_io_swd_close(d->w);
        if (aos_hal_usb_mode() == AOS_HAL_USB_PROBE || aos_hal_usb_busy()) {
            usb_settle(AOS_HAL_USB_PROBE, 3000);
            aos_hal_usb_mode_set(before);
        }
    }
    aos_hal_io_free(d);
    return ok;
}
