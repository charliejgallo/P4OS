/*
 * P4OS - the Modbus master. The contract is in aos_modbus.h; the framing is
 * the standard's (Modbus over serial line V1.02, Modbus Messaging on TCP/IP
 * V1.0b): RTU = unit, PDU, CRC-16 (0xA001, low byte first); TCP = the MBAP
 * header (transaction, protocol 0, length, unit) and the PDU, no CRC.
 *
 * Only aos_hal.h and aos_io.h underneath: the same file in the simulator,
 * where a port can be a pty with tools/fake_modbus.py on the other end.
 */
#include "aos_modbus.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PDU_MAX 253

struct aos_mb {
    aos_mb_link_t link;
    aos_io_uart_t *uart;
    int tcp;
    uint16_t tid;
    aos_mb_stats_t st;
};

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
    }
    return c;
}

aos_mb_t *aos_mb_open(const aos_mb_link_t *link, const char *owner, char *err, size_t err_len)
{
    aos_mb_t *m = calloc(1, sizeof *m);
    if (!m) { snprintf(err, err_len, "%s", _("sin memoria")); return NULL; }
    m->link = *link;
    if (!m->link.unit) m->link.unit = 1;
    if (!m->link.retries) m->link.retries = 2;
    if (!m->link.gap_ms) m->link.gap_ms = 5;
    if (m->link.transport == AOS_MB_RTU) {
        if (!m->link.baud) m->link.baud = 9600;
        if (!m->link.timeout_ms) m->link.timeout_ms = 300;
        const aos_io_port_t *p = aos_io_port_find(m->link.port);
        bool rs485 = p && p->kind == AOS_PORT_UART && p->pins[2] >= 0;   /* a DE pin: half duplex */
        m->uart = aos_io_uart_open(m->link.port, m->link.baud, rs485, owner);
        if (!m->uart) {
            snprintf(err, err_len, _("no se pudo abrir %s (¿lo tiene otra app?)"), m->link.port);
            free(m);
            return NULL;
        }
        char par = m->link.parity ? m->link.parity : 'N';
        int stop = m->link.stop_bits ? m->link.stop_bits : 1;
        if ((par != 'N' || stop != 1) && !aos_io_uart_set_format(m->uart, par, stop)) {
            snprintf(err, err_len, _("%s no acepta 8%c%d"), m->link.port, par, stop);
            aos_io_uart_close(m->uart);
            free(m);
            return NULL;
        }
    } else {
        if (!m->link.tcp_port) m->link.tcp_port = 502;
        if (!m->link.timeout_ms) m->link.timeout_ms = 1000;
        m->tcp = aos_hal_tcp_connect(m->link.host, m->link.tcp_port, 3000);
        if (m->tcp <= 0) {
            snprintf(err, err_len, _("no contesta %s:%d"), m->link.host, m->link.tcp_port);
            free(m);
            return NULL;
        }
    }
    return m;
}

void aos_mb_close(aos_mb_t *m)
{
    if (!m) return;
    if (m->uart) aos_io_uart_close(m->uart);
    if (m->tcp > 0) aos_hal_tcp_close(m->tcp);
    free(m);
}

void aos_mb_set_unit(aos_mb_t *m, uint8_t unit) { if (m) m->link.unit = unit ? unit : 1; }
void aos_mb_stats(aos_mb_t *m, aos_mb_stats_t *out) { if (m && out) *out = m->st; }

/* Reads exactly n bytes, or fewer when the line goes quiet past the deadline. */
static int rtu_read(aos_mb_t *m, uint8_t *buf, int n, uint32_t deadline)
{
    int got = 0;
    while (got < n) {
        int left = (int)(deadline - (uint32_t)aos_hal_uptime_ms());
        if (left <= 0) break;
        int r = aos_io_uart_read(m->uart, buf + got, n - got, left < 50 ? left : 50);
        if (r < 0) return -1;
        got += r;
    }
    return got;
}

/* One request/answer. 'expect' is the length of a normal answer's PDU, or
 * -1 for "the byte count is in the answer" (reads). The answer's PDU goes
 * into rsp. Returns 0, an exception code, or AOS_MB_E_*. */
static int transact_once(aos_mb_t *m, const uint8_t *pdu, int plen, uint8_t *rsp, int expect)
{
    uint8_t f[PDU_MAX + 10];
    uint8_t fc = pdu[0];
    uint32_t t0;
    m->st.requests++;
    aos_hal_sleep_ms(m->link.gap_ms);
    if (m->uart) {
        uint8_t junk[64];
        while (aos_io_uart_read(m->uart, junk, sizeof junk, 0) > 0) {}   /* a late answer to the last one */
        f[0] = m->link.unit;
        memcpy(f + 1, pdu, (size_t)plen);
        uint16_t c = crc16(f, (size_t)plen + 1);
        f[plen + 1] = (uint8_t)c;
        f[plen + 2] = (uint8_t)(c >> 8);
        t0 = (uint32_t)aos_hal_uptime_ms();
        if (aos_io_uart_write(m->uart, f, plen + 3) != plen + 3) return AOS_MB_E_IO;
        uint32_t deadline = t0 + m->link.timeout_ms;
        /* unit, function, and either the exception code or the byte count / first byte */
        int n = rtu_read(m, f, 3, deadline);
        if (n < 0) return AOS_MB_E_IO;
        if (n < 3) { m->st.timeouts++; return AOS_MB_E_TIMEOUT; }
        int total;
        if (f[1] == (fc | 0x80)) total = 5;
        else if (f[1] != fc) total = -1;
        else if (expect < 0) total = 3 + f[2] + 2;
        else total = 1 + expect + 2;
        if (total < 0 || f[0] != m->link.unit) {
            aos_hal_sleep_ms(20);           /* not ours: let the rest of it arrive, drain it next time */
            return AOS_MB_E_FRAME;
        }
        n = rtu_read(m, f + 3, total - 3, deadline + 50);
        if (n < 0) return AOS_MB_E_IO;
        if (n < total - 3) { m->st.timeouts++; return AOS_MB_E_TIMEOUT; }
        uint16_t got = (uint16_t)(f[total - 2] | f[total - 1] << 8);
        if (crc16(f, (size_t)total - 2) != got) { m->st.crc_errors++; return AOS_MB_E_CRC; }
        memcpy(rsp, f + 1, (size_t)total - 3);
    } else {
        uint16_t tid = ++m->tid;
        f[0] = (uint8_t)(tid >> 8); f[1] = (uint8_t)tid;
        f[2] = 0; f[3] = 0;
        f[4] = (uint8_t)((plen + 1) >> 8); f[5] = (uint8_t)(plen + 1);
        f[6] = m->link.unit;
        memcpy(f + 7, pdu, (size_t)plen);
        t0 = (uint32_t)aos_hal_uptime_ms();
        if (aos_hal_tcp_send(m->tcp, f, plen + 7, 2000) != plen + 7) return AOS_MB_E_IO;
        for (;;) {
            /* MBAP header, then its length */
            int got = 0;
            while (got < 7) {
                int left = (int)(t0 + m->link.timeout_ms - (uint32_t)aos_hal_uptime_ms());
                if (left <= 0) { m->st.timeouts++; return AOS_MB_E_TIMEOUT; }
                int r = aos_hal_tcp_recv(m->tcp, f + got, 7 - got, left);
                if (r < 0) return AOS_MB_E_IO;
                got += r;
            }
            int len = (f[4] << 8 | f[5]) - 1;
            if (len < 2 || len > PDU_MAX) return AOS_MB_E_FRAME;
            got = 0;
            while (got < len) {
                int r = aos_hal_tcp_recv(m->tcp, f + 7 + got, len - got, 1000);
                if (r <= 0) return r < 0 ? AOS_MB_E_IO : AOS_MB_E_TIMEOUT;
                got += r;
            }
            if ((f[0] << 8 | f[1]) != tid) continue;        /* an old answer: skip it */
            memcpy(rsp, f + 7, (size_t)len);
            break;
        }
    }
    m->st.last_ms = (uint32_t)aos_hal_uptime_ms() - t0;
    m->st.answers++;
    if (rsp[0] == (fc | 0x80)) { m->st.exceptions++; return rsp[1] ? rsp[1] : 4; }
    if (rsp[0] != fc) return AOS_MB_E_FRAME;
    return 0;
}

static int transact(aos_mb_t *m, const uint8_t *pdu, int plen, uint8_t *rsp, int expect)
{
    if (!m) return AOS_MB_E_ARG;
    int rc = AOS_MB_E_TIMEOUT;
    for (int i = 0; i <= m->link.retries; i++) {
        rc = transact_once(m, pdu, plen, rsp, expect);
        if (rc != AOS_MB_E_TIMEOUT && rc != AOS_MB_E_CRC && rc != AOS_MB_E_FRAME) break;
    }
    return rc;
}

int aos_mb_read(aos_mb_t *m, int fc, uint16_t addr, uint16_t count, uint16_t *out)
{
    bool bits = fc == AOS_MB_COILS || fc == AOS_MB_DISCRETE;
    if (fc < 1 || fc > 4 || !count || count > (bits ? 2000 : 125)) return AOS_MB_E_ARG;
    uint8_t q[5] = { (uint8_t)fc, (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(count >> 8), (uint8_t)count };
    uint8_t r[PDU_MAX + 2];
    int rc = transact(m, q, 5, r, -1);
    if (rc) return rc;
    int bytes = r[1];
    if (bits) {
        if (bytes < (count + 7) / 8) return AOS_MB_E_FRAME;
        for (int i = 0; i < count; i++) out[i] = (r[2 + i / 8] >> (i % 8)) & 1;
    } else {
        if (bytes != count * 2) return AOS_MB_E_FRAME;
        for (int i = 0; i < count; i++) out[i] = (uint16_t)(r[2 + 2 * i] << 8 | r[3 + 2 * i]);
    }
    return 0;
}

int aos_mb_write_coil(aos_mb_t *m, uint16_t addr, bool on)
{
    uint8_t q[5] = { 5, (uint8_t)(addr >> 8), (uint8_t)addr, on ? 0xFF : 0, 0 }, r[PDU_MAX + 2];
    return transact(m, q, 5, r, 5);
}

int aos_mb_write_reg(aos_mb_t *m, uint16_t addr, uint16_t v)
{
    uint8_t q[5] = { 6, (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(v >> 8), (uint8_t)v }, r[PDU_MAX + 2];
    return transact(m, q, 5, r, 5);
}

int aos_mb_write_regs(aos_mb_t *m, uint16_t addr, uint16_t n, const uint16_t *v)
{
    if (!n || n > 123) return AOS_MB_E_ARG;
    uint8_t q[PDU_MAX], r[PDU_MAX + 2];
    q[0] = 16; q[1] = (uint8_t)(addr >> 8); q[2] = (uint8_t)addr; q[3] = (uint8_t)(n >> 8); q[4] = (uint8_t)n;
    q[5] = (uint8_t)(n * 2);
    for (int i = 0; i < n; i++) { q[6 + 2 * i] = (uint8_t)(v[i] >> 8); q[7 + 2 * i] = (uint8_t)v[i]; }
    return transact(m, q, 6 + n * 2, r, 5);
}

int aos_mb_write_coils(aos_mb_t *m, uint16_t addr, uint16_t n, const uint8_t *bits)
{
    if (!n || n > 1968) return AOS_MB_E_ARG;
    uint8_t q[PDU_MAX], r[PDU_MAX + 2];
    int bytes = (n + 7) / 8;
    q[0] = 15; q[1] = (uint8_t)(addr >> 8); q[2] = (uint8_t)addr; q[3] = (uint8_t)(n >> 8); q[4] = (uint8_t)n;
    q[5] = (uint8_t)bytes;
    memset(q + 6, 0, (size_t)bytes);
    for (int i = 0; i < n; i++) if (bits[i]) q[6 + i / 8] |= (uint8_t)(1 << (i % 8));
    return transact(m, q, 6 + bytes, r, 5);
}

const char *aos_mb_strerror(int rc)
{
    switch (rc) {
    case 0: return "ok";
    case AOS_MB_E_TIMEOUT: return _("sin respuesta");
    case AOS_MB_E_CRC: return _("respuesta dañada (CRC)");
    case AOS_MB_E_FRAME: return _("respuesta que no corresponde");
    case AOS_MB_E_IO: return _("falló el puerto o la conexión");
    case AOS_MB_E_ARG: return _("pedido inválido");
    case 1: return _("función no soportada (01)");
    case 2: return _("dirección inválida (02)");
    case 3: return _("valor inválido (03)");
    case 4: return _("falla del esclavo (04)");
    case 6: return _("esclavo ocupado (06)");
    case 10: return _("gateway sin camino (0A)");
    case 11: return _("el equipo tras el gateway no contesta (0B)");
    default: return _("excepción Modbus");
    }
}
