/*
 * P4OS - Modbus: the bus thread (mb.h says who does what).
 *
 * It owns the link while the UI wants it open and plays the chosen role on
 * it. All three roles read the line the same way (rx_pump and rx_frame):
 *
 *   The UART driver hands bytes over in bursts - when its FIFO fills, or
 *   some character times after the line goes quiet - so the silences the
 *   standard frames RTU with (3.5 characters) cannot be measured from here
 *   to the character. Frames are split by their own contents instead: from
 *   the function code, the lengths a frame may have (a request, an answer,
 *   an exception), and the one whose CRC matches wins. A silence long enough
 *   for the driver to have delivered everything (14 characters and 2 ms,
 *   plus the bytes a length still expects) ends what is left as one damaged
 *   frame, or as noise when it is too short to be a frame. Between frames
 *   that do not match, the next offset where a good frame starts is looked
 *   for, so one bad frame does not swallow the good one behind it.
 *
 * Times (the silence before a frame, an answer's delay) are therefore when
 * the bytes reached the app, not when they were on the wire: close, a few
 * character times late. The Traffic tab says so.
 */
#include "mb.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_io.h"

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OWNER "Modbus"

mb_shared_t MB;

static void *s_mx;
static volatile bool s_stop, s_done, s_started;

void mb_lock(void) { aos_hal_mutex_lock(s_mx); }
void mb_unlock(void) { aos_hal_mutex_unlock(s_mx); }

static uint64_t now_us(void) { return aos_hal_uptime_us(); }
static uint32_t now_ms(void) { return (uint32_t)(aos_hal_uptime_us() / 1000); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* ---- the link ---- */

static aos_io_uart_t *s_u;
static int s_tcp;
static int s_listen, s_client;  /* the server: the slave over TCP, the gateway */
static mb_role_t s_open_role;   /* what the link was opened for: TCP is a client or a server */
static uint16_t s_tid;
static mb_cfg_t s_cfg;          /* what is open */
static bool s_open;

static void server_close(void)
{
    if (s_client > 0) aos_hal_tcp_close(s_client);
    if (s_listen > 0) aos_hal_tcp_close(s_listen);
    s_client = s_listen = 0;
    mb_lock();
    MB.tcp_peer[0] = 0;
    MB.seq++;
    mb_unlock();
}

static void link_close(void)
{
    if (s_u) aos_io_uart_close(s_u);
    if (s_tcp > 0) aos_hal_tcp_close(s_tcp);
    server_close();
    s_u = NULL;
    s_tcp = 0;
    s_open = false;
}

static bool server_open(uint16_t port, char *err, size_t n)
{
    if (s_listen > 0) return true;
    s_listen = aos_hal_tcp_listen(port);
    if (s_listen <= 0) {
        s_listen = 0;
        snprintf(err, n, _("no se pudo escuchar en el puerto %u (¿lo usa otro?)"), port);
        return false;
    }
    return true;
}

static bool link_open(const mb_cfg_t *c, mb_role_t role, char *err, size_t n, char *desc, size_t dn)
{
    s_cfg = *c;
    s_open_role = role;
    if (c->tr == MB_TR_TCP && role == MB_ROLE_SLAVE) {
        uint16_t port = c->tport ? c->tport : 502;
        if (!server_open(port, err, n)) return false;
        snprintf(desc, dn, _("Modbus TCP, escuchando en el puerto %u"), port);
        s_open = true;
        return true;
    }
    if (c->tr == MB_TR_RTU) {
        const aos_io_port_t *p = aos_io_port_find(c->port);
        bool rs485 = p && p->kind == AOS_PORT_UART && p->pins[2] >= 0;   /* a DE pin: half duplex */
        s_u = aos_io_uart_open(c->port, c->baud ? c->baud : 9600, rs485, OWNER);
        if (!s_u) {
            snprintf(err, n, _("no se pudo abrir %s (¿lo tiene otra app?)"), c->port);
            return false;
        }
        char par = c->par ? c->par : 'N';
        int stop = c->stop ? c->stop : 1;
        if ((par != 'N' || stop != 1) && !aos_io_uart_set_format(s_u, par, stop)) {
            snprintf(err, n, _("%s no acepta 8%c%d"), c->port, par, stop);
            link_close();
            return false;
        }
        snprintf(desc, dn, "%s%s", aos_io_uart_desc(s_u), rs485 ? " · RS485" : "");
    } else {
        s_tcp = aos_hal_tcp_connect(c->host, c->tport ? c->tport : 502, 3000);
        if (s_tcp <= 0) {
            s_tcp = 0;
            snprintf(err, n, _("no contesta %s:%d"), c->host, c->tport ? c->tport : 502);
            return false;
        }
        snprintf(desc, dn, "%s:%d", c->host, c->tport ? c->tport : 502);
    }
    s_open = true;
    return true;
}

/* ---- the log ---- */

static uint64_t s_prev_end;     /* when the last frame ended (arrived) */

static void log_put(const uint8_t *d, int len, uint8_t fl, uint8_t note, uint64_t t_us, uint32_t gap_us, uint32_t rt_ms)
{
    if (!MB.log) return;
    if (len > MB_MAXF) len = MB_MAXF;
    mb_lock();
    mb_log_t *e = &MB.log[MB.log_seq % MB_LOGN];
    e->t_us = t_us;
    e->gap_us = gap_us;
    e->len = (uint16_t)(len > 0 ? len : 0);
    e->fl = fl;
    e->note = note;
    e->rt_ms = (uint16_t)(rt_ms > 65535 ? 65535 : rt_ms);
    if (len > 0) memcpy(e->d, d, (size_t)len);
    MB.log_seq++;
    MB.seq++;
    mb_unlock();
}

static void log_note(uint8_t note, uint8_t extra_fl, const uint8_t *d, int len)
{
    log_put(d, len, (uint8_t)(MB_LF_NOTE | extra_fl), note, now_us(), 0, 0);
}

bool mb_log_get(uint32_t k, mb_log_t *out)
{
    bool ok = false;
    mb_lock();
    if (MB.log && k < MB.log_seq && MB.log_seq - k <= MB_LOGN) {
        *out = MB.log[k % MB_LOGN];
        ok = true;
    }
    mb_unlock();
    return ok;
}

void mb_bus_clear_log(void)
{
    mb_lock();
    MB.log_seq = 0;
    MB.t0_us = now_us();
    MB.seq++;
    mb_unlock();
}

void mb_bus_clear_stats(void)
{
    mb_lock();
    MB.m_req = MB.m_ans = MB.m_tmo = MB.m_crc = MB.m_exc = 0;
    MB.m_rt_last = MB.m_rt_sum = MB.m_rt_max = 0;
    MB.s_req = MB.s_mine = MB.s_other = MB.s_bcast = MB.s_crc = MB.s_exc = 0;
    memset(MB.units, 0, sizeof MB.units);
    MB.nunits = 0;
    MB.mon_frames = MB.mon_bytes = MB.mon_crc = MB.mon_noise = 0;
    MB.mon_parity = MB.mon_frame = 0;
    MB.gw_req = MB.gw_ok = MB.gw_fail = 0;
    MB.seq++;
    mb_unlock();
}

/* ---- reading the line ---- */

static uint8_t s_rx[2 * MB_MAXF];
static int s_rxn;
static uint64_t s_rx_t0, s_rx_last;     /* the first and the last arrival of what is in s_rx */

static int rx_pump(int wait_ms)
{
    if (!s_u) return -1;
    if (s_rxn >= (int)sizeof s_rx) return 0;
    int r = aos_io_uart_read(s_u, s_rx + s_rxn, (int)sizeof s_rx - s_rxn, wait_ms);
    if (r > 0) {
        uint64_t t = now_us();
        if (!s_rxn) s_rx_t0 = t;
        s_rxn += r;
        s_rx_last = t;
    }
    return r;
}

static void rx_consume(int n)
{
    memmove(s_rx, s_rx + n, (size_t)(s_rxn - n));
    s_rxn -= n;
    s_rx_t0 = s_rx_last;            /* the rest arrived with the last burst, at best */
}

/* A good frame (its CRC matches one of its possible lengths) at the front:
 * its length, or 0. *pending: a length still waits for bytes (how many). */
static int good_at(const uint8_t *f, int have, int *pending)
{
    int L[3], k = mb_rtu_lengths(f, have, L), miss = 0;
    for (int i = 0; i < k; i++) {
        if (L[i] < 4 || L[i] > MB_MAXF) continue;
        if (L[i] <= have) { if (mb_crc_ok(f, L[i])) return L[i]; }
        else if (L[i] - have > miss) miss = L[i] - have;
    }
    if (pending) *pending = miss;
    return 0;
}

/* Takes one frame off the front of what has been read. Returns its length
 * (0: none yet), with *fl MB_LF_CRC or MB_LF_NOISE for a bad one and the
 * time it arrived in *t, the silence before it in *gap. */
static int rx_frame(uint8_t *out, uint8_t *fl, uint64_t *t, uint32_t *gap)
{
    if (!s_rxn) return 0;
    int miss = 0;
    int take = good_at(s_rx, s_rxn, &miss);
    *fl = 0;
    if (!take) {
        uint32_t quiet = (uint32_t)(now_us() - s_rx_last);
        uint32_t need = mb_char_us(&s_cfg, 14.0f) + 2000 + (miss ? mb_char_us(&s_cfg, (float)miss) : 0);
        if (quiet < need && s_rxn < (int)sizeof s_rx - MB_MAXF / 2) return 0;
        /* the line went quiet with no good frame at the front: up to the
         * next offset where one starts, it is damage */
        take = s_rxn;
        for (int j = 1; j + 4 <= s_rxn; j++) if (good_at(s_rx + j, s_rxn - j, NULL)) { take = j; break; }
        if (take > MB_MAXF) take = MB_MAXF;
        /* a function this file does not know the lengths of, whole: good */
        *fl = mb_crc_ok(s_rx, take) ? 0 : take >= 4 ? MB_LF_CRC : MB_LF_NOISE;
    }
    memcpy(out, s_rx, (size_t)take);
    *t = s_rx_t0;
    *gap = s_prev_end && s_rx_t0 > s_prev_end ? (uint32_t)(s_rx_t0 - s_prev_end) : 0;
    s_prev_end = s_rx_last;
    rx_consume(take);
    return take;
}

static void rx_drain_to_log(void)
{
    /* whatever came in between (a late answer, somebody else on the bus) */
    while (rx_pump(0) > 0) {}
    uint8_t f[MB_MAXF], fl;
    uint64_t t;
    uint32_t gap;
    int n;
    s_rx_last = 0;                  /* old enough: end it now */
    while ((n = rx_frame(f, &fl, &t, &gap)) > 0) log_put(f, n, fl, fl ? MB_NOTE_NONE : MB_NOTE_LATE, t, gap, 0);
}

/* ---- the profile, as the thread keeps it ---- */

static mb_prof_t s_prof;
static uint32_t s_prof_gen;
static bool s_prof_on;
static mb_block_t s_blk[32];
static int s_nblk, s_blk_of[MB_PTS];
static bool s_prof_fresh;               /* the slave has not written its values yet */

static void prof_take(void)
{
    mb_lock();
    if (MB.prof_gen != s_prof_gen || MB.prof_on != s_prof_on) {
        s_prof_gen = MB.prof_gen;
        s_prof_on = MB.prof_on;
        s_prof = MB.prof;
        s_nblk = s_prof_on ? mb_prof_blocks(&s_prof, s_blk, 32, s_blk_of) : 0;
        s_prof_fresh = true;
        for (int i = 0; i < MB_PTS; i++) { MB.pv[i] = 0; MB.prc[i] = MB_E_TIMEOUT; }
        MB.pv_seq++;
    }
    mb_unlock();
}

/* ---- master ---- */

static void m_count(int rc, uint32_t rt)
{
    mb_lock();
    MB.m_req++;
    if (rc == MB_E_TIMEOUT) MB.m_tmo++;
    else if (rc == MB_E_CRC) MB.m_crc++;
    else if (rc >= 0) {
        MB.m_ans++;
        if (rc > 0) MB.m_exc++;
        MB.m_rt_last = rt;
        MB.m_rt_sum += rt;
        if (rt > MB.m_rt_max) MB.m_rt_max = rt;
    }
    MB.seq++;
    mb_unlock();
}

/* One RTU request and its answer. The answer's frame (unit, PDU, CRC) goes
 * to rsp. Returns 0, an exception code or MB_E_*; *rt the delay in ms. */
static int rtu_transact(const uint8_t *f, int len, uint8_t *rsp, int *rlen, uint16_t timeout_ms, uint32_t *rt, bool quiet)
{
    uint8_t unit = f[0], fc = f[1];
    if (!quiet) rx_drain_to_log();
    else { while (rx_pump(0) > 0) {} s_rxn = 0; }
    uint64_t t_tx = now_us();
    if (aos_io_uart_write(s_u, f, len) != len) return MB_E_IO;
    uint64_t tx_end = t_tx + mb_char_us(&s_cfg, (float)len);
    if (!quiet) log_put(f, len, MB_LF_TX | MB_LF_REQ, 0, t_tx, s_prev_end && t_tx > s_prev_end ? (uint32_t)(t_tx - s_prev_end) : 0, 0);
    s_prev_end = tx_end;
    if (unit == 0) {                    /* broadcast: nobody answers */
        aos_hal_sleep_ms(timeout_ms < 100 ? timeout_ms : 100);
        *rlen = 0;
        return 0;
    }
    uint64_t deadline = tx_end + (uint64_t)timeout_ms * 1000;
    for (;;) {
        uint64_t t;
        uint32_t gap;
        uint8_t fl;
        int n = rx_frame(rsp, &fl, &t, &gap);
        if (n > 0) {
            /* from the end of the request to the start of the answer */
            uint64_t t_start = t - mb_char_us(&s_cfg, (float)n);
            uint32_t ms = t_start > tx_end ? (uint32_t)((t_start - tx_end) / 1000) : 0;
            if (fl) {
                if (!quiet) log_put(rsp, n, fl | MB_LF_RSP, 0, t, gap, ms);
                if (fl & MB_LF_CRC) { *rt = ms; return MB_E_CRC; }
                continue;
            }
            if (n == len && !memcmp(rsp, f, (size_t)len) && fc != 5 && fc != 6 && fc != 8) {
                /* our own request back: a transceiver that hears itself */
                if (!quiet) log_put(rsp, n, MB_LF_REQ, MB_NOTE_OTHER_UNIT, t, gap, 0);
                continue;
            }
            bool ours = rsp[0] == unit && (rsp[1] & 0x7F) == fc;
            if (!quiet) log_put(rsp, n, (uint8_t)(MB_LF_RSP | ((rsp[1] & 0x80) ? MB_LF_EXC : 0)), ours ? 0 : MB_NOTE_OTHER_UNIT, t, gap, ms);
            if (!ours) continue;
            *rlen = n;
            *rt = ms;
            return (rsp[1] & 0x80) ? (rsp[2] ? rsp[2] : 4) : 0;
        }
        int64_t left = (int64_t)(deadline - now_us());
        if (left <= 0) {
            if (s_rxn) {            /* something came, never whole */
                n = s_rxn > MB_MAXF ? MB_MAXF : s_rxn;
                if (!quiet) log_put(s_rx, n, MB_LF_CRC | MB_LF_RSP, 0, s_rx_t0, 0, 0);
                s_rxn = 0;
                return MB_E_CRC;
            }
            if (!quiet) log_note(MB_NOTE_TIMEOUT, 0, f, 2);
            return MB_E_TIMEOUT;
        }
        int w = (int)(left / 1000);
        if (rx_pump(w < 1 ? 1 : w > 5 ? 5 : w) < 0) return MB_E_IO;
    }
}

static int tcp_transact(const uint8_t *pdu, int plen, uint8_t unit, uint8_t *rsp, int *rlen, uint16_t timeout_ms, uint32_t *rt)
{
    uint8_t f[MB_MAXF];
    uint16_t tid = ++s_tid;
    put16(f, tid);
    put16(f + 2, 0);
    put16(f + 4, (uint16_t)(plen + 1));
    f[6] = unit;
    memcpy(f + 7, pdu, (size_t)plen);
    uint64_t t0 = now_us();
    if (aos_hal_tcp_send(s_tcp, f, plen + 7, 2000) != plen + 7) return MB_E_IO;
    log_put(f, plen + 7, MB_LF_TX | MB_LF_REQ | MB_LF_TCP, 0, t0, 0, 0);
    for (;;) {
        int got = 0;
        while (got < 7) {
            int left = (int)(timeout_ms - (now_us() - t0) / 1000);
            if (left <= 0) { log_note(MB_NOTE_TIMEOUT, MB_LF_TCP, f, 8); return MB_E_TIMEOUT; }
            int r = aos_hal_tcp_recv(s_tcp, rsp + got, 7 - got, left);
            if (r < 0) return MB_E_IO;
            got += r;
        }
        int len = get16(rsp + 4) - 1;
        if (len < 2 || len + 7 > MB_MAXF) return MB_E_FRAME;
        got = 0;
        while (got < len) {
            int r = aos_hal_tcp_recv(s_tcp, rsp + 7 + got, len - got, 1000);
            if (r <= 0) return r < 0 ? MB_E_IO : MB_E_TIMEOUT;
            got += r;
        }
        uint64_t t = now_us();
        *rt = (uint32_t)((t - t0) / 1000);
        bool exc = rsp[7] & 0x80;
        log_put(rsp, len + 7, (uint8_t)(MB_LF_RSP | MB_LF_TCP | (exc ? MB_LF_EXC : 0)), 0, t, 0, *rt);
        if (get16(rsp) != tid) continue;            /* an old answer */
        *rlen = len + 7;
        return exc ? (rsp[8] ? rsp[8] : 4) : (rsp[7] == pdu[0] ? 0 : MB_E_FRAME);
    }
}

/* Does a request; the answer's PDU (no unit, no CRC) is copied to pdu_out. */
static int m_do(const mb_mreq_t *q, uint8_t *pdu_out, int *pdu_len, uint32_t *rt, bool count)
{
    uint8_t f[MB_MAXF], r[MB_MAXF];
    int rl = 0, rc;
    int pl = mb_build_pdu(q, f + 1);
    if (pl < 0) return MB_E_ARG;
    *rt = 0;
    aos_hal_sleep_ms(s_cfg.gap_ms);
    if (s_cfg.tr == MB_TR_TCP) {
        rc = tcp_transact(f + 1, pl, q->unit, r, &rl, s_cfg.timeout_ms ? s_cfg.timeout_ms : 1000, rt);
        if (rc >= 0 && rl > 7) { memcpy(pdu_out, r + 7, (size_t)(rl - 7)); *pdu_len = rl - 7; }
    } else {
        f[0] = q->unit;
        mb_crc_put(f, pl + 1);
        rc = rtu_transact(f, pl + 3, r, &rl, s_cfg.timeout_ms ? s_cfg.timeout_ms : 300, rt, false);
        if (rc >= 0 && rl > 3) { memcpy(pdu_out, r + 1, (size_t)(rl - 3)); *pdu_len = rl - 3; }
        else *pdu_len = 0;
    }
    if (count) m_count(rc, *rt);
    return rc;
}

/* What FC 17 and 43 say, as text. */
static void m_text_of(const uint8_t *p, int len, char *o, size_t n)
{
    o[0] = 0;
    if (p[0] == 17 && len >= 2) {
        int bc = p[1];
        size_t k = (size_t)snprintf(o, n, "%s:", _("Id"));
        for (int i = 0; i < bc && 2 + i < len && k + 4 < n; i++) k += (size_t)snprintf(o + k, n - k, " %02X", p[2 + i]);
        if (k + 8 < n) k += (size_t)snprintf(o + k, n - k, "  \"");
        for (int i = 0; i < bc && 2 + i < len && k + 3 < n; i++) o[k++] = p[2 + i] >= 32 && p[2 + i] < 127 ? (char)p[2 + i] : '.';
        if (k + 2 < n) { o[k++] = '"'; o[k] = 0; }
    } else if (p[0] == 43 && len >= 7) {
        static const char *const NAME[] = { N_("Fabricante"), N_("Producto"), N_("Versión") };
        int cnt = p[6], q = 7;
        size_t k = 0;
        for (int i = 0; i < cnt && q + 2 <= len; i++) {
            int id = p[q], l = p[q + 1];
            char v[64];
            int m = l < 63 ? l : 63;
            if (q + 2 + m > len) break;
            for (int j = 0; j < m; j++) v[j] = p[q + 2 + j] >= 32 && p[q + 2 + j] < 127 ? (char)p[q + 2 + j] : '.';
            v[m] = 0;
            if (k + 4 < n) k += (size_t)snprintf(o + k, n - k, "%s%s: %s", k ? " · " : "", id < 3 ? aos_tr(NAME[id]) : "?", v);
            q += 2 + l;
        }
    }
}

static void m_request(const mb_mreq_t *q)
{
    uint8_t p[MB_MAXF];
    int pl = 0;
    uint32_t rt;
    int rc = m_do(q, p, &pl, &rt, true);
    mb_lock();
    MB.m_rc = rc;
    MB.m_got_fc = q->fc;
    MB.m_got_addr = q->addr;
    if (!rc && pl >= 2) {
        if (q->fc <= 2) {
            int n = q->n;
            for (int i = 0; i < n && 2 + i / 8 < pl; i++) MB.m_val[i] = (p[2 + i / 8] >> (i % 8)) & 1;
            MB.m_got_n = (uint16_t)n;
        } else if (q->fc <= 4) {
            int n = p[1] / 2;
            for (int i = 0; i < n && 3 + 2 * i < pl; i++) MB.m_val[i] = get16(p + 2 + 2 * i);
            MB.m_got_n = (uint16_t)n;
        } else if (q->fc == 8) {
            MB.m_got_n = 0;
            snprintf(MB.m_text, sizeof MB.m_text, "%s", pl >= 5 && get16(p + 3) == q->wv[0] ? _("El eco volvió igual.") : _("El eco volvió distinto."));
        } else if (q->fc == 17 || q->fc == 43) {
            MB.m_got_n = 0;
            m_text_of(p, pl, MB.m_text, sizeof MB.m_text);
        } else MB.m_got_n = 0;
    }
    MB.m_seq++;
    MB.seq++;
    mb_unlock();
}

static const uint32_t SCAN_BAUDS[] = { 9600, 19200, 38400, 57600, 115200, 4800, 2400, 1200 };
static const struct { char par; uint8_t stop; } SCAN_FMT[] = { { 'N', 1 }, { 'E', 1 }, { 'O', 1 }, { 'N', 2 } };

static bool tool_stopped(void)
{
    mb_lock();
    bool s = MB.tool_stop || !MB.want_open || s_stop;
    mb_unlock();
    return s;
}

static void tool_progress(int pos)
{
    mb_lock();
    MB.tool_pos = pos;
    MB.tool_seq++;
    MB.seq++;
    mb_unlock();
}

/* Who is on the bus: a short read of holding 0 to every unit. An answer or
 * an exception says somebody is there; a damaged answer too, probably. */
static void tool_scan(void)
{
    mb_lock();
    memset(MB.found, 0, sizeof MB.found);
    MB.tool_total = 247;
    mb_unlock();
    uint16_t tmo = (uint16_t)(mb_char_us(&s_cfg, 30.0f) / 1000 + 40);
    for (int u = 1; u <= 247 && !tool_stopped(); u++) {
        uint8_t f[8] = { (uint8_t)u, 3, 0, 0, 0, 1 }, r[MB_MAXF];
        mb_crc_put(f, 6);
        int rl;
        uint32_t rt;
        int rc = rtu_transact(f, 8, r, &rl, tmo, &rt, true);
        if (rc != MB_E_TIMEOUT) {
            mb_lock();
            MB.found[u] = rc == 0 ? 1 : rc > 0 ? 2 : 3;
            mb_unlock();
            log_put(f, 8, MB_LF_TX | MB_LF_REQ, 0, now_us(), 0, 0);
            if (rc >= 0) log_put(r, rl, (uint8_t)(MB_LF_RSP | (rc ? MB_LF_EXC : 0)), 0, now_us(), 0, rt);
        }
        tool_progress(u);
        aos_hal_sleep_ms(s_cfg.gap_ms ? s_cfg.gap_ms : 5);
    }
}

/* At what speed and format a unit answers: every pair, a read of holding 0.
 * At the right speed several formats answer (a UART hands over a byte with
 * a bad parity too), so the line's errors while the answer comes in tell
 * them apart (aos_io_uart_errors): the right format reads it clean, a wrong
 * one sees bad parity or stop bits in about half the bytes. The clean ones
 * go first in the list. */
static void tool_baud(uint8_t unit)
{
    int nb = (int)(sizeof SCAN_BAUDS / sizeof SCAN_BAUDS[0]), nf = (int)(sizeof SCAN_FMT / sizeof SCAN_FMT[0]);
    char clean[8][24], dirty[8][24];
    int nc = 0, nd = 0;
    mb_lock();
    MB.nbauds_found = 0;
    MB.nbauds_clean = 0;
    MB.tool_total = nb * nf;
    mb_unlock();
    mb_cfg_t keep = s_cfg;
    int pos = 0;
    for (int b = 0; b < nb && !tool_stopped(); b++) {
        for (int k = 0; k < nf && !tool_stopped(); k++, pos++) {
            aos_io_uart_set_baud(s_u, SCAN_BAUDS[b]);
            aos_io_uart_set_format(s_u, SCAN_FMT[k].par, SCAN_FMT[k].stop);
            s_cfg.baud = SCAN_BAUDS[b];
            s_cfg.par = SCAN_FMT[k].par;
            s_cfg.stop = SCAN_FMT[k].stop;
            aos_hal_sleep_ms(20);
            uint8_t f[8] = { unit, 3, 0, 0, 0, 1 }, r[MB_MAXF];
            mb_crc_put(f, 6);
            int rl, rc = MB_E_TIMEOUT;
            uint32_t rt;
            uint16_t tmo = (uint16_t)(mb_char_us(&s_cfg, 30.0f) / 1000 + 80);
            aos_uart_errors_t le;
            for (int tries = 0; tries < 2 && rc == MB_E_TIMEOUT; tries++) {
                aos_io_uart_errors(s_u, NULL, true);
                rc = rtu_transact(f, 8, r, &rl, tmo, &rt, true);
            }
            bool counted = aos_io_uart_errors(s_u, &le, true);
            if (rc >= 0) {
                unsigned bad = le.parity + le.frame;
                if (counted && !bad && nc < 8)
                    snprintf(clean[nc++], sizeof clean[0], "%u 8%c%u", (unsigned)SCAN_BAUDS[b], SCAN_FMT[k].par, SCAN_FMT[k].stop);
                else if (nd < 8)
                    snprintf(dirty[nd++], sizeof dirty[0], counted ? "%u 8%c%u · %u err" : "%u 8%c%u", (unsigned)SCAN_BAUDS[b],
                             SCAN_FMT[k].par, SCAN_FMT[k].stop, bad);
                mb_lock();
                MB.nbauds_found = 0;
                for (int i = 0; i < nc && MB.nbauds_found < 8; i++) memcpy(MB.bauds_found[MB.nbauds_found++], clean[i], sizeof clean[0]);
                MB.nbauds_clean = MB.nbauds_found;
                for (int i = 0; i < nd && MB.nbauds_found < 8; i++) memcpy(MB.bauds_found[MB.nbauds_found++], dirty[i], sizeof dirty[0]);
                mb_unlock();
                log_put(r, rl, (uint8_t)(MB_LF_RSP | (rc ? MB_LF_EXC : 0)), 0, now_us(), 0, rt);
            }
            tool_progress(pos + 1);
        }
    }
    aos_io_uart_set_baud(s_u, keep.baud ? keep.baud : 9600);
    aos_io_uart_set_format(s_u, keep.par ? keep.par : 'N', keep.stop ? keep.stop : 1);
    s_cfg = keep;
    aos_hal_sleep_ms(20);
    while (rx_pump(0) > 0) {}
    s_rxn = 0;
}

static uint32_t s_m_last;

/* One round of a profile's reads: every block, its points decoded. */
static void prof_poll(void)
{
    for (int b = 0; b < s_nblk; b++) {
        mb_lock();
        bool go_on = MB.want_open && MB.role == MB_ROLE_MASTER && MB.prof_gen == s_prof_gen && !s_stop;
        mb_unlock();
        if (!go_on) return;
        mb_mreq_t q;
        memset(&q, 0, sizeof q);
        q.unit = s_prof.unit;
        q.fc = s_blk[b].table;
        q.addr = s_blk[b].addr;
        q.n = s_blk[b].n;
        uint8_t p[MB_MAXF];
        int pl = 0;
        uint32_t rt;
        int rc = m_do(&q, p, &pl, &rt, true);
        static uint16_t raw[1000];
        if (!rc) {
            if (q.fc <= 2) for (int i = 0; i < q.n && 2 + i / 8 < pl; i++) raw[i] = (p[2 + i / 8] >> (i % 8)) & 1;
            else for (int i = 0; i < q.n && 3 + 2 * i < pl; i++) raw[i] = get16(p + 2 + 2 * i);
        }
        mb_lock();
        for (int i = 0; i < s_prof.n; i++) {
            if (s_blk_of[i] != b) continue;
            MB.prc[i] = rc;
            if (!rc) MB.pv[i] = mb_pt_decode(&s_prof.pt[i], raw + (s_prof.pt[i].addr - q.addr));
        }
        MB.m_rc = rc;
        MB.m_rt_last = rt;
        MB.m_seq++;
        MB.pv_seq++;
        MB.seq++;
        mb_unlock();
    }
}

static void gateway_step(void);
static bool server_open(uint16_t port, char *err, size_t n);
static void server_close(void);

static void master_step(void)
{
    prof_take();
    /* the gateway, only on a bus: TCP clients asking the RTU units */
    if (s_cfg.tr == MB_TR_RTU) {
        mb_lock();
        bool gw = MB.gw_on;
        uint16_t gp = MB.gw_port ? MB.gw_port : 502;
        mb_unlock();
        if (gw && s_listen <= 0) {
            char e[64] = "";
            bool ok = server_open(gp, e, sizeof e);
            mb_lock();
            snprintf(MB.tcp_err, sizeof MB.tcp_err, "%s", e);
            if (!ok) MB.gw_on = false;
            MB.seq++;
            mb_unlock();
        } else if (!gw && s_listen > 0) server_close();
        if (s_listen > 0) gateway_step();
    }
    mb_lock();
    int tool = MB.tool;
    mb_mreq_t w;
    bool have_w = MB.m_wqn > 0;
    if (have_w) {
        w = MB.m_wq[0];
        memmove(MB.m_wq, MB.m_wq + 1, (size_t)(MB.m_wqn - 1) * sizeof MB.m_wq[0]);
        MB.m_wqn--;
    }
    mb_mreq_t q = MB.mreq;
    bool go = MB.m_once || (MB.m_live && now_ms() - s_m_last >= MB.m_period_ms);
    MB.m_once = false;
    mb_unlock();
    if (have_w) {
        uint8_t p[MB_MAXF];
        int pl;
        uint32_t rt;
        int rc = m_do(&w, p, &pl, &rt, true);
        mb_lock();
        MB.m_rc = rc;
        MB.m_seq++;
        mb_unlock();
        if (!rc) { mb_lock(); MB.m_once = true; mb_unlock(); }     /* show the effect */
        return;
    }
    if (tool) {
        if (s_cfg.tr == MB_TR_RTU) {
            if (tool == MB_TOOL_SCAN) tool_scan();
            else tool_baud(q.unit ? q.unit : 1);
        }
        mb_lock();
        MB.tool = MB_TOOL_NONE;
        MB.tool_stop = false;
        MB.tool_seq++;
        MB.seq++;
        mb_unlock();
        return;
    }
    if (s_prof_on) {
        /* a profile: its reads, over and over at its pace */
        if (now_ms() - s_m_last >= s_prof.poll_ms) {
            s_m_last = now_ms();
            prof_poll();
            return;
        }
    } else if (go) {
        s_m_last = now_ms();
        m_request(&q);
        return;
    }
    /* idle: still show what goes by (another master, a late answer) */
    if (s_u) {
        rx_pump(10);
        uint8_t f[MB_MAXF], fl;
        uint64_t t;
        uint32_t gap;
        int n;
        while ((n = rx_frame(f, &fl, &t, &gap)) > 0) log_put(f, n, fl, fl ? 0 : MB_NOTE_LATE, t, gap, 0);
    } else aos_hal_sleep_ms(10);
}

/* ---- the server: Modbus TCP for clients on the network ----
 *
 * One client at a time (a second one waits in the listen queue until the
 * first goes): a PLC, a SCADA, Modbus Poll on a PC. An ADU is the MBAP
 * header (transaction, protocol 0, length, unit) and the PDU; the answer
 * goes back with the same header. Polled from the thread's loop, so the
 * slave and the gateway never block the rest of it for long. */

static uint8_t s_tin[MB_MAXF + 8];
static int s_tinn;

/* A whole request from the client, or 0 (nothing yet) or -1 (it left). */
static int server_take(uint8_t *adu)
{
    if (s_listen <= 0) return -1;
    if (s_client <= 0) {
        char peer[24] = "";
        int c = aos_hal_tcp_accept(s_listen, 5, peer, sizeof peer);
        if (c <= 0) return 0;
        s_client = c;
        s_tinn = 0;
        mb_lock();
        snprintf(MB.tcp_peer, sizeof MB.tcp_peer, "%s", peer);
        MB.seq++;
        mb_unlock();
        log_note(MB_NOTE_OPEN, MB_LF_TCP, NULL, 0);
    }
    int r = aos_hal_tcp_recv(s_client, s_tin + s_tinn, (int)sizeof s_tin - s_tinn, 5);
    if (r < 0) {
        aos_hal_tcp_close(s_client);
        s_client = 0;
        mb_lock();
        MB.tcp_peer[0] = 0;
        MB.seq++;
        mb_unlock();
        log_note(MB_NOTE_CLOSE, MB_LF_TCP, NULL, 0);
        return -1;
    }
    s_tinn += r;
    if (s_tinn < 7) return 0;
    int len = get16(s_tin + 4) + 6;
    if (get16(s_tin + 2) != 0 || len < 8 || len > MB_MAXF) { s_tinn = 0; return 0; }    /* not Modbus: dropped */
    if (s_tinn < len) return 0;
    memcpy(adu, s_tin, (size_t)len);
    memmove(s_tin, s_tin + len, (size_t)(s_tinn - len));
    s_tinn -= len;
    return len;
}

/* Sends an answer PDU (pdu[0] = function) for the request adu. */
static void server_reply(const uint8_t *adu, const uint8_t *pdu, int plen, uint32_t rt)
{
    uint8_t o[MB_MAXF + 8];
    memcpy(o, adu, 4);
    put16(o + 4, (uint16_t)(plen + 1));
    o[6] = adu[6];
    memcpy(o + 7, pdu, (size_t)plen);
    if (s_client > 0) aos_hal_tcp_send(s_client, o, plen + 7, 1000);
    bool exc = pdu[0] & 0x80;
    log_put(o, plen + 7, (uint8_t)(MB_LF_TX | MB_LF_RSP | MB_LF_TCP | (exc ? MB_LF_EXC : 0)), 0, now_us(), 0, rt);
}

/* The gateway: a TCP client's request goes out on the bus as RTU, and the
 * answer goes back. While it waits for the bus nothing else is asked. */
static void gateway_step(void)
{
    uint8_t adu[MB_MAXF + 8];
    int n;
    while ((n = server_take(adu)) > 0) {
        uint64_t t = now_us();
        log_put(adu, n, MB_LF_REQ | MB_LF_TCP, 0, t, 0, 0);
        uint8_t f[MB_MAXF], r[MB_MAXF];
        int fl = n - 6;                 /* unit + PDU */
        memcpy(f, adu + 6, (size_t)fl);
        mb_crc_put(f, fl);
        int rl = 0;
        uint32_t rt = 0;
        aos_hal_sleep_ms(s_cfg.gap_ms);
        int rc = rtu_transact(f, fl + 2, r, &rl, s_cfg.timeout_ms ? s_cfg.timeout_ms : 1000, &rt, false);
        mb_lock();
        MB.gw_req++;
        if (rc >= 0) MB.gw_ok++; else MB.gw_fail++;
        MB.seq++;
        mb_unlock();
        if (f[0] == 0) continue;                    /* a broadcast: nobody answers */
        if (rc >= 0 && rl >= 4) server_reply(adu, r + 1, rl - 3, rt);
        else {
            /* 0x0B: the gateway's target did not answer (a bad CRC counts) */
            uint8_t e[2] = { (uint8_t)(adu[7] | 0x80), 0x0B };
            server_reply(adu, e, 2, rt);
        }
    }
}

/* ---- slave ---- */

static uint32_t s_anim_ms, s_anim_t0, s_rng = 0x2545F491;

/* rand() is not among the firmware's exports */
static uint32_t rng(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

static void put_point(const mb_point_t *pt, double v)
{
    uint16_t r[2];
    mb_pt_encode(pt, v, r);
    int w = mb_pt_words(pt);
    if ((uint32_t)pt->addr + (uint32_t)w > MB_SPACE) return;
    for (int k = 0; k < w; k++) {
        switch (pt->table) {
        case MB_TAB_COIL: MB.s_coil[pt->addr] = r[0] != 0; break;
        case MB_TAB_DI: MB.s_di[pt->addr] = r[0] != 0; break;
        case MB_TAB_HR: MB.s_hr[pt->addr + k] = r[k]; break;
        default: MB.s_ir[pt->addr + k] = r[k]; break;
        }
    }
}

void mb_bus_point_put(const mb_point_t *pt, double v)
{
    mb_lock();
    if (MB.s_ir) put_point(pt, v);
    MB.s_seq++;
    mb_unlock();
}

static double get_point(const mb_point_t *pt)
{
    uint16_t r[2] = { 0, 0 };
    int w = mb_pt_words(pt);
    if ((uint32_t)pt->addr + (uint32_t)w > MB_SPACE) return 0;
    for (int k = 0; k < w; k++) {
        switch (pt->table) {
        case MB_TAB_COIL: r[0] = MB.s_coil[pt->addr]; break;
        case MB_TAB_DI: r[0] = MB.s_di[pt->addr]; break;
        case MB_TAB_HR: r[k] = MB.s_hr[pt->addr + k]; break;
        default: r[k] = MB.s_ir[pt->addr + k]; break;
        }
    }
    return mb_pt_decode(pt, r);
}

/* The slave plays its profile: the values where they start, then each
 * simulated one moving; what a master writes stays. */
static void slave_prof(float t, float dt)
{
    if (s_prof_fresh) {
        for (int i = 0; i < s_prof.n; i++) put_point(&s_prof.pt[i], s_prof.pt[i].value);
        s_prof_fresh = false;
    }
    for (int i = 0; i < s_prof.n; i++) {
        const mb_point_t *pt = &s_prof.pt[i];
        if (pt->sim) put_point(pt, mb_pt_sim(pt, get_point(pt), t, dt));
        MB.pv[i] = get_point(pt);
        MB.prc[i] = (uint32_t)pt->addr + (uint32_t)mb_pt_words(pt) > MB_SPACE ? MB_E_ARG : 0;
    }
    MB.pv_seq++;
}

static void slave_anim(void)
{
    uint32_t t = now_ms();
    if (t - s_anim_ms < 100) return;
    float dt = s_anim_ms ? (float)(t - s_anim_ms) / 1000.0f : 0.1f;
    s_anim_ms = t;
    if (!s_anim_t0) s_anim_t0 = t;
    prof_take();
    mb_lock();
    if (s_prof_on && MB.s_ir) slave_prof((float)(t - s_anim_t0) / 1000.0f, dt);
    else if (MB.s_anim && MB.s_ir) {
        float s = (float)(t - s_anim_t0) / 1000.0f;
        MB.s_ir[0] = (uint16_t)(s);                                     /* seconds */
        MB.s_ir[1] = (uint16_t)(int16_t)(sinf(s * 0.5f) * 1000.0f);     /* a sine, x1000 */
        MB.s_ir[2] = (uint16_t)(235 + (int)(sinf(s / 40.0f) * 15.0f));  /* 23.5 C x10, drifting */
        MB.s_ir[3] = (uint16_t)(rng() & 0x3FF);                        /* noise */
        uint32_t f = (uint32_t)(s * 10.0f);                             /* a 32-bit counter in 4-5 */
        MB.s_ir[4] = (uint16_t)(f >> 16);
        MB.s_ir[5] = (uint16_t)f;
        float fv = 230.0f + sinf(s / 7.0f) * 3.0f;                      /* a float in 6-7, ABCD */
        uint32_t fw;
        memcpy(&fw, &fv, 4);
        MB.s_ir[6] = (uint16_t)(fw >> 16);
        MB.s_ir[7] = (uint16_t)fw;
        MB.s_di[0] = (uint8_t)(((t - s_anim_t0) / 2000) & 1);
        MB.s_di[1] = (uint8_t)(((t - s_anim_t0) / 5000) & 1);
    }
    mb_unlock();
}

/* The answer PDU to a request (unit + PDU, no CRC): its length. Takes the
 * lock for the tables. */
static int slave_answer(const uint8_t *q, int qlen, uint8_t *o)
{
    int fc = q[1];
    uint16_t a = qlen >= 4 ? get16(q + 2) : 0, n = qlen >= 6 ? get16(q + 4) : 0;
    int ex = 0, len = 0;
    o[0] = (uint8_t)fc;
    mb_lock();
    switch (fc) {
    case 1: case 2: {
        if (n < 1 || n > 2000) { ex = 3; break; }
        if ((uint32_t)a + n > MB_SPACE) { ex = 2; break; }
        const uint8_t *src = fc == 1 ? MB.s_coil : MB.s_di;
        int bytes = (n + 7) / 8;
        o[1] = (uint8_t)bytes;
        memset(o + 2, 0, (size_t)bytes);
        for (int i = 0; i < n; i++) if (src[a + i]) o[2 + i / 8] |= (uint8_t)(1 << (i % 8));
        len = 2 + bytes;
        break;
    }
    case 3: case 4: {
        if (n < 1 || n > 125) { ex = 3; break; }
        if ((uint32_t)a + n > MB_SPACE) { ex = 2; break; }
        const uint16_t *src = fc == 3 ? MB.s_hr : MB.s_ir;
        o[1] = (uint8_t)(2 * n);
        for (int i = 0; i < n; i++) put16(o + 2 + 2 * i, src[a + i]);
        len = 2 + 2 * n;
        break;
    }
    case 5:
        if (n != 0xFF00 && n != 0) { ex = 3; break; }
        if (a >= MB_SPACE) { ex = 2; break; }
        MB.s_coil[a] = n ? 1 : 0;
        memcpy(o, q + 1, 5);
        len = 5;
        break;
    case 6:
        if (a >= MB_SPACE) { ex = 2; break; }
        MB.s_hr[a] = n;
        memcpy(o, q + 1, 5);
        len = 5;
        break;
    case 8:
        if (a != 0) { ex = 1; break; }      /* only 00: return query data */
        memcpy(o, q + 1, 5);
        len = 5;
        break;
    case 15: case 16: {
        int bc = qlen >= 7 ? q[6] : 0;
        if (fc == 15 ? (n < 1 || n > 1968 || bc != (n + 7) / 8) : (n < 1 || n > 123 || bc != 2 * n)) { ex = 3; break; }
        if ((uint32_t)a + n > MB_SPACE) { ex = 2; break; }
        if (qlen < 7 + bc) { ex = 3; break; }
        for (int i = 0; i < n; i++) {
            if (fc == 15) MB.s_coil[a + i] = (q[7 + i / 8] >> (i % 8)) & 1;
            else MB.s_hr[a + i] = get16(q + 7 + 2 * i);
        }
        memcpy(o, q + 1, 5);
        len = 5;
        break;
    }
    case 17: {
        static const char ID[] = "P4OS";
        o[1] = (uint8_t)(sizeof ID - 1 + 1);
        memcpy(o + 2, ID, sizeof ID - 1);
        o[2 + sizeof ID - 1] = 0xFF;            /* run indicator: on */
        len = 2 + (int)sizeof ID;
        break;
    }
    case 43: {
        if (qlen < 5 || q[2] != 14) { ex = 1; break; }
        static const char *const OBJ[3] = { "P4OS", "Modbus slave", "1.0" };
        o[1] = 14; o[2] = 1; o[3] = 0x01; o[4] = 0; o[5] = 0; o[6] = 3;
        int p = 7;
        for (int i = 0; i < 3; i++) {
            int l = (int)strlen(OBJ[i]);
            o[p++] = (uint8_t)i;
            o[p++] = (uint8_t)l;
            memcpy(o + p, OBJ[i], (size_t)l);
            p += l;
        }
        len = p;
        break;
    }
    default:
        ex = 1;
    }
    mb_unlock();
    if (ex) {
        o[0] = (uint8_t)(fc | 0x80);
        o[1] = (uint8_t)ex;
        len = 2;
    }
    return len;
}

static void slave_step(void)
{
    slave_anim();
    if (rx_pump(5) < 0) { aos_hal_sleep_ms(10); return; }
    uint8_t f[MB_MAXF], fl;
    uint64_t t;
    uint32_t gap;
    int n;
    while ((n = rx_frame(f, &fl, &t, &gap)) > 0) {
        mb_lock();
        uint8_t unit = MB.s_unit ? MB.s_unit : 1;
        int fault = MB.s_fault;
        uint16_t delay = MB.s_delay_ms;
        MB.s_req++;
        if (fl) MB.s_crc++;
        mb_unlock();
        log_put(f, n, (uint8_t)(fl | MB_LF_REQ), 0, t, gap, 0);
        if (fl) continue;
        bool bc = f[0] == 0;
        if (f[0] != unit && !bc) {
            mb_lock(); MB.s_other++; MB.seq++; mb_unlock();
            continue;
        }
        uint8_t o[MB_MAXF];
        int ol = slave_answer(f, n - 2, o + 1);
        char what[96];
        mb_describe(f, n - 2, true, what, sizeof what);
        mb_lock();
        if (bc) MB.s_bcast++; else MB.s_mine++;
        snprintf(MB.s_last, sizeof MB.s_last, "%s", what);
        MB.s_seq++;
        MB.seq++;
        mb_unlock();
        if (bc) continue;                       /* a broadcast is never answered */
        if (fault == MB_FAULT_SILENT) { log_note(MB_NOTE_SILENT, 0, f, 2); continue; }
        if (fault == MB_FAULT_BUSY || fault == MB_FAULT_FAIL) {
            o[1] = (uint8_t)(f[1] | 0x80);
            o[2] = fault == MB_FAULT_BUSY ? 6 : 4;
            ol = 2;
        }
        o[0] = unit;
        mb_crc_put(o, ol + 1);
        int total = ol + 3;
        if (fault == MB_FAULT_CRC) o[total - 1] ^= 0x5A;
        /* the turnaround: the master has to be listening already */
        uint32_t wait = mb_char_us(&s_cfg, 3.5f) / 1000 + 2;
        if (fault == MB_FAULT_SLOW) wait += delay;
        aos_hal_sleep_ms(wait);
        uint64_t tw = now_us();
        aos_io_uart_write(s_u, o, total);
        uint32_t rt = (uint32_t)((tw - t) / 1000);
        bool exc = o[1] & 0x80;
        if (exc) { mb_lock(); MB.s_exc++; mb_unlock(); }
        log_put(o, total, (uint8_t)(MB_LF_TX | MB_LF_RSP | (exc ? MB_LF_EXC : 0) | (fault == MB_FAULT_CRC ? MB_LF_CRC : 0)), 0, tw, 0, rt);
        s_prev_end = tw + mb_char_us(&s_cfg, (float)total);
    }
}

/* The slave over Modbus TCP: the same tables and faults, a client on the
 * network instead of a master on the bus. A request to another unit is
 * not answered, as on a bus (unit 0 and 255 are taken as this one: many
 * clients send them to a device that is not behind a gateway). */
static void slave_tcp_step(void)
{
    slave_anim();
    uint8_t adu[MB_MAXF + 8];
    int n;
    while ((n = server_take(adu)) > 0) {
        uint64_t t = now_us();
        log_put(adu, n, MB_LF_REQ | MB_LF_TCP, 0, t, 0, 0);
        mb_lock();
        uint8_t unit = MB.s_unit ? MB.s_unit : 1;
        int fault = MB.s_fault;
        uint16_t delay = MB.s_delay_ms;
        MB.s_req++;
        mb_unlock();
        uint8_t u = adu[6];
        if (u != unit && u != 0 && u != 0xFF) {
            mb_lock(); MB.s_other++; MB.seq++; mb_unlock();
            continue;
        }
        uint8_t o[MB_MAXF];
        int ol = slave_answer(adu + 6, n - 6, o);
        char what[96];
        mb_describe(adu + 6, n - 6, true, what, sizeof what);
        mb_lock();
        MB.s_mine++;
        snprintf(MB.s_last, sizeof MB.s_last, "%s", what);
        MB.s_seq++;
        MB.seq++;
        mb_unlock();
        if (fault == MB_FAULT_SILENT) { log_note(MB_NOTE_SILENT, MB_LF_TCP, adu + 6, 2); continue; }
        if (fault == MB_FAULT_BUSY || fault == MB_FAULT_FAIL) {
            o[0] = (uint8_t)(adu[7] | 0x80);
            o[1] = fault == MB_FAULT_BUSY ? 6 : 4;
            ol = 2;
        }
        if (fault == MB_FAULT_SLOW) aos_hal_sleep_ms(delay);
        /* TCP has no CRC: the broken-answer fault answers another transaction */
        if (fault == MB_FAULT_CRC) adu[1] ^= 0x5A;
        if (o[0] & 0x80) { mb_lock(); MB.s_exc++; mb_unlock(); }
        server_reply(adu, o, ol, (uint32_t)((now_us() - t) / 1000));
    }
}

/* ---- monitor ---- */

static struct { bool on; uint8_t unit, fc; uint16_t addr, n; uint64_t t; } s_pend;

static mb_unit_t *unit_row(uint8_t u)
{
    for (int i = 0; i < MB.nunits; i++) if (MB.units[i].unit == u) return &MB.units[i];
    if (MB.nunits >= MB_UNITS) return NULL;
    mb_unit_t *r = &MB.units[MB.nunits++];
    memset(r, 0, sizeof *r);
    r->unit = u;
    return r;
}

static void pend_expire(uint64_t now, bool force)
{
    if (!s_pend.on) return;
    if (!force && now - s_pend.t < 1000000) return;     /* a second: nobody waits that long */
    mb_lock();
    mb_unit_t *r = unit_row(s_pend.unit);
    if (r) r->timeouts++;
    MB.seq++;
    mb_unlock();
    uint8_t d[2] = { s_pend.unit, s_pend.fc };
    log_put(d, 2, MB_LF_NOTE, MB_NOTE_TIMEOUT, force ? now : s_pend.t + 1000000, 0, 0);
    s_pend.on = false;
}

static void monitor_step(void)
{
    if (rx_pump(5) < 0) { aos_hal_sleep_ms(10); return; }
    aos_uart_errors_t le;
    if (s_u && aos_io_uart_errors(s_u, &le, true) && (le.parity || le.frame)) {
        mb_lock();
        MB.mon_parity += le.parity;
        MB.mon_frame += le.frame;
        MB.seq++;
        mb_unlock();
    }
    uint8_t f[MB_MAXF], fl;
    uint64_t t;
    uint32_t gap;
    int n;
    while ((n = rx_frame(f, &fl, &t, &gap)) > 0) {
        mb_lock();
        MB.mon_frames++;
        MB.mon_bytes += (uint32_t)n;
        if (fl & MB_LF_CRC) MB.mon_crc++;
        if (fl & MB_LF_NOISE) MB.mon_noise += (uint32_t)n;
        /* a damaged frame that starts with the unit asked: its answer, broken */
        bool broken_answer = (fl & MB_LF_CRC) && s_pend.on && f[0] == s_pend.unit;
        if (broken_answer) {
            mb_unit_t *r = unit_row(s_pend.unit);
            if (r) r->crc++;
            s_pend.on = false;
        }
        mb_unlock();
        if (fl) { log_put(f, n, (uint8_t)(fl | (broken_answer ? MB_LF_RSP : 0)), 0, t, gap, 0); continue; }
        uint8_t u = f[0], fc = f[1] & 0x7F;
        bool is_rsp = s_pend.on && s_pend.unit == u && s_pend.fc == fc;
        if (is_rsp) {
            /* both arrived late by about the same; the answer's own length is not its delay */
            uint64_t t_start = t - mb_char_us(&s_cfg, (float)n);
            uint32_t rt = t_start > s_pend.t ? (uint32_t)((t_start - s_pend.t) / 1000) : 0;
            bool exc = f[1] & 0x80;
            mb_lock();
            mb_unit_t *r = unit_row(u);
            if (r) {
                r->rsp++;
                if (exc) r->exc++;
                r->rt_sum += rt;
                r->rt_n++;
                if (rt > r->rt_max) r->rt_max = rt;
            }
            MB.seq++;
            mb_unlock();
            log_put(f, n, (uint8_t)(MB_LF_RSP | (exc ? MB_LF_EXC : 0)), 0, t, gap, rt);
            s_pend.on = false;
            continue;
        }
        pend_expire(t, true);           /* a new question: the last one got nothing */
        log_put(f, n, MB_LF_REQ, 0, t, gap, 0);
        mb_lock();
        mb_unit_t *r = unit_row(u);
        if (r) {
            r->req++;
            r->last_fc = fc;
            r->last_addr = n >= 6 ? get16(f + 2) : 0;
            r->last_n = n >= 8 ? get16(f + 4) : 0;
            r->last_ms = (uint32_t)(t / 1000);
        }
        MB.seq++;
        mb_unlock();
        if (u != 0) {
            s_pend.on = true;
            s_pend.unit = u;
            s_pend.fc = fc;
            s_pend.t = t;
        }
    }
    pend_expire(now_us(), false);
}

/* ---- the thread ---- */

static void bus_main(void *arg)
{
    (void)arg;
    mb_role_t last_role = MB_ROLE_COUNT;
    while (!s_stop) {
        mb_lock();
        bool want = MB.want_open;
        mb_cfg_t cfg = MB.cfg;
        mb_role_t role = MB.role;
        mb_unlock();
        if (s_open && (!want || memcmp(&cfg, &s_cfg, sizeof cfg))) {
            link_close();
            log_note(MB_NOTE_CLOSE, 0, NULL, 0);
            mb_lock();
            MB.open = false;
            for (int i = 0; i < MB_PTS; i++) MB.prc[i] = MB_E_TIMEOUT;     /* the values are no longer live */
            MB.pv_seq++;
            MB.seq++;
            mb_unlock();
        }
        if (!want) { aos_hal_sleep_ms(50); continue; }
        if (!s_open) {
            char err[96] = "", desc[64] = "";
            if (cfg.tr == MB_TR_TCP && role == MB_ROLE_MONITOR) {
                mb_lock();
                snprintf(MB.err, sizeof MB.err, "%s", _("El monitor escucha un bus RS485: por TCP no hay nada que escuchar."));
                MB.seq++;
                mb_unlock();
                aos_hal_sleep_ms(200);
                continue;
            }
            bool ok = link_open(&cfg, role, err, sizeof err, desc, sizeof desc);
            mb_lock();
            MB.open = ok;
            snprintf(MB.err, sizeof MB.err, "%s", ok ? "" : err);
            snprintf(MB.desc, sizeof MB.desc, "%s", desc);
            MB.seq++;
            mb_unlock();
            if (!ok) {
                for (int i = 0; i < 20 && !s_stop; i++) aos_hal_sleep_ms(50);
                continue;
            }
            s_rxn = 0;
            s_prev_end = 0;
            s_pend.on = false;
            log_note(MB_NOTE_OPEN, 0, NULL, 0);
            last_role = MB_ROLE_COUNT;
        }
        if (role != last_role) {
            last_role = role;
            s_pend.on = false;
            if (cfg.tr == MB_TR_TCP && (role == MB_ROLE_SLAVE) != (s_open_role == MB_ROLE_SLAVE)) {
                /* a client and a server are different links */
                link_close();
                mb_lock(); MB.open = false; MB.seq++; mb_unlock();
                continue;
            }
        }
        switch (role) {
        case MB_ROLE_MASTER: master_step(); break;
        case MB_ROLE_SLAVE: if (s_cfg.tr == MB_TR_TCP) slave_tcp_step(); else slave_step(); break;
        default: monitor_step(); break;
        }
    }
    link_close();
    s_done = true;
}

/* PSRAM: the log alone is a quarter of a megabyte */
static void *big_alloc(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) memset(p, 0, n);
    return p;
#else
    return calloc(1, n);
#endif
}

static void big_free(void *p)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    heap_caps_free(p);
#else
    free(p);
#endif
}

bool mb_bus_init(void)
{
    if (s_started) return true;
    if (!s_mx) s_mx = aos_hal_mutex_create();
    if (!MB.log) MB.log = big_alloc(sizeof(mb_log_t) * MB_LOGN);
    if (!MB.s_coil) MB.s_coil = big_alloc(MB_SPACE);
    if (!MB.s_di) MB.s_di = big_alloc(MB_SPACE);
    if (!MB.s_hr) MB.s_hr = big_alloc(MB_SPACE * sizeof(uint16_t));
    if (!MB.s_ir) MB.s_ir = big_alloc(MB_SPACE * sizeof(uint16_t));
    if (!MB.log || !MB.s_coil || !MB.s_di || !MB.s_hr || !MB.s_ir) return false;
    MB.t0_us = now_us();
    s_stop = s_done = false;
    if (!aos_hal_thread_start("modbus", bus_main, NULL, 8192, 4)) return false;
    s_started = true;
    return true;
}

void mb_bus_deinit(void)
{
    if (s_started) {
        s_stop = true;
        for (int i = 0; i < 400 && !s_done; i++) aos_hal_sleep_ms(10);
        if (!s_done) {
            /* still inside the port: better leak than free under its feet */
            aos_hal_log("modbus", "the bus thread did not stop");
            return;
        }
        s_started = false;
    }
    mb_lock();
    MB.open = false;
    mb_unlock();
    big_free(MB.log);
    big_free(MB.s_coil);
    big_free(MB.s_di);
    big_free(MB.s_hr);
    big_free(MB.s_ir);
    MB.log = NULL;
    MB.s_coil = MB.s_di = NULL;
    MB.s_hr = MB.s_ir = NULL;
}
