/*
 * P4OS - Modbus: the protocol (mb.h says what the app is).
 *
 * CRC, the request PDUs, the lengths an RTU frame may have (what lets the
 * monitor split a stream where the silences were lost), and the one-line
 * meaning of a frame for the Traffic tab.
 */
#include "mb.h"

#include "aos_i18n.h"

#include <stdio.h>
#include <string.h>

uint16_t mb_crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
    }
    return c;
}

bool mb_crc_ok(const uint8_t *f, int len)
{
    if (len < 4) return false;
    uint16_t c = mb_crc16(f, (size_t)len - 2);
    return f[len - 2] == (uint8_t)c && f[len - 1] == (uint8_t)(c >> 8);
}

void mb_crc_put(uint8_t *f, int len)
{
    uint16_t c = mb_crc16(f, (size_t)len);
    f[len] = (uint8_t)c;
    f[len + 1] = (uint8_t)(c >> 8);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

int mb_build_pdu(const mb_mreq_t *q, uint8_t *o)
{
    o[0] = q->fc;
    switch (q->fc) {
    case 1: case 2:
        if (!q->n || q->n > 2000) return -1;
        put16(o + 1, q->addr); put16(o + 3, q->n);
        return 5;
    case 3: case 4:
        if (!q->n || q->n > 125) return -1;
        put16(o + 1, q->addr); put16(o + 3, q->n);
        return 5;
    case 5:
        put16(o + 1, q->addr); put16(o + 3, q->wv[0] ? 0xFF00 : 0);
        return 5;
    case 6:
        put16(o + 1, q->addr); put16(o + 3, q->wv[0]);
        return 5;
    case 8:                                     /* diagnostics 00: return query data */
        put16(o + 1, 0); put16(o + 3, q->wv[0]);
        return 5;
    case 15: {
        int n = q->wn ? q->wn : 1;
        if (n > 123 * 16) return -1;
        int bytes = (n + 7) / 8;
        put16(o + 1, q->addr); put16(o + 3, (uint16_t)n);
        o[5] = (uint8_t)bytes;
        memset(o + 6, 0, (size_t)bytes);
        for (int i = 0; i < n && i < 123; i++) if (q->wv[i]) o[6 + i / 8] |= (uint8_t)(1 << (i % 8));
        return 6 + bytes;
    }
    case 16: {
        int n = q->wn ? q->wn : 1;
        if (n > 123) return -1;
        put16(o + 1, q->addr); put16(o + 3, (uint16_t)n);
        o[5] = (uint8_t)(2 * n);
        for (int i = 0; i < n; i++) put16(o + 6 + 2 * i, q->wv[i]);
        return 6 + 2 * n;
    }
    case 17:
        return 1;
    case 43:                                    /* read device identification, basic */
        o[1] = 14; o[2] = 1; o[3] = 0;
        return 4;
    default:
        return -1;
    }
}

/* FC 43/14's answer has no length of its own: walk its objects. */
static int len_43(const uint8_t *f, int have)
{
    /* unit 43 14 code conformity more next count, then id len data... */
    if (have < 8) return 0;
    int p = 8, cnt = f[7];
    for (int i = 0; i < cnt; i++) {
        if (have < p + 2) return 0;
        p += 2 + f[p + 1];
        if (p > MB_MAXF) return -1;
    }
    return p + 2;
}

int mb_rtu_lengths(const uint8_t *f, int have, int *out)
{
    if (have < 2) return 0;
    int fc = f[1], k = 0;
    if (fc & 0x80) { out[k++] = 5; return k; }
    switch (fc) {
    case 1: case 2: case 3: case 4:
        if (have < 3) return 0;
        out[k++] = 8;                           /* the request */
        out[k++] = 5 + f[2];                    /* the answer, with its byte count */
        return k;
    case 5: case 6: case 8:
        out[k++] = 8;
        return k;
    case 15: case 16:
        out[k++] = 8;                           /* the answer */
        if (have < 7) return k;                 /* try it; the request needs [6] */
        out[k++] = 9 + f[6];
        return k;
    case 17:
        out[k++] = 4;
        if (have >= 3) out[k++] = 5 + f[2];
        return k;
    case 43:
        out[k++] = 7;
        if (have >= 3 && f[2] == 14) {
            int l = len_43(f, have);
            if (l > 0) out[k++] = l;
        }
        return k;
    default:
        return 0;
    }
}

int mb_rtu_answer_len(const uint8_t *q)
{
    switch (q[0]) {
    case 5: case 6: case 8: case 15: case 16: return 8;
    default: return -1;
    }
}

const char *mb_fc_name(int fc)
{
    switch (fc & 0x7F) {
    case 1: return _("leer coils");
    case 2: return _("leer entradas");
    case 3: return _("leer holding");
    case 4: return _("leer input");
    case 5: return _("escribir coil");
    case 6: return _("escribir registro");
    case 8: return _("diagnóstico");
    case 15: return _("escribir coils");
    case 16: return _("escribir registros");
    case 17: return _("identificarse");
    case 43: return _("identificación");
    default: return _("función desconocida");
    }
}

static const char *exc_name(int code)
{
    switch (code) {
    case 1: return _("función no soportada");
    case 2: return _("dirección inválida");
    case 3: return _("valor inválido");
    case 4: return _("falla del esclavo");
    case 5: return _("aceptado, sigue trabajando");
    case 6: return _("esclavo ocupado");
    case 10: return _("gateway sin camino");
    case 11: return _("el equipo tras el gateway no contesta");
    default: return "?";
    }
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

void mb_describe(const uint8_t *f, int len, bool is_req, char *o, size_t n)
{
    if (len < 2) { snprintf(o, n, "%s", _("ruido")); return; }
    int u = f[0], fc = f[1];
    if (fc & 0x80) {
        int code = len > 2 ? f[2] : 0;
        snprintf(o, n, "U%d %s: %s %02X, %s", u, mb_fc_name(fc), _("excepción"), code, exc_name(code));
        return;
    }
    const char *name = mb_fc_name(fc);
    switch (fc) {
    case 1: case 2: case 3: case 4:
        if (is_req && len >= 6) snprintf(o, n, "U%d %s %u..%u (%u)", u, name, get16(f + 2), get16(f + 2) + get16(f + 4) - 1, get16(f + 4));
        else if (len >= 3) {
            int bc = f[2];
            if (fc >= 3 && bc >= 2) {
                char vals[64] = "";
                size_t p = 0;
                for (int i = 0; i < bc / 2 && i < 6 && 3 + 2 * i + 1 < len; i++)
                    p += (size_t)snprintf(vals + p, sizeof vals - p, "%s%u", i ? " " : "", get16(f + 3 + 2 * i));
                snprintf(o, n, "U%d %s: %d %s: %s%s", u, name, bc / 2, _("valores"), vals, bc / 2 > 6 ? " …" : "");
            } else snprintf(o, n, "U%d %s: %d bytes", u, name, bc);
        } else snprintf(o, n, "U%d %s", u, name);
        return;
    case 5:
        if (len >= 6) snprintf(o, n, "U%d %s %u = %s", u, name, get16(f + 2), get16(f + 4) == 0xFF00 ? "ON" : get16(f + 4) ? "?" : "OFF");
        else snprintf(o, n, "U%d %s", u, name);
        return;
    case 6:
        if (len >= 6) snprintf(o, n, "U%d %s %u = %u (0x%04X)", u, name, get16(f + 2), get16(f + 4), get16(f + 4));
        else snprintf(o, n, "U%d %s", u, name);
        return;
    case 8:
        if (len >= 6) snprintf(o, n, "U%d %s %u, %s 0x%04X", u, name, get16(f + 2), _("dato"), get16(f + 4));
        else snprintf(o, n, "U%d %s", u, name);
        return;
    case 15: case 16:
        if (len >= 6) snprintf(o, n, "U%d %s %u..%u (%u)", u, name, get16(f + 2), get16(f + 2) + get16(f + 4) - 1, get16(f + 4));
        else snprintf(o, n, "U%d %s", u, name);
        return;
    default:
        snprintf(o, n, "U%d %s", u, name);
        return;
    }
}

const char *mb_strerror(int rc)
{
    switch (rc) {
    case 0: return "OK";
    case MB_E_TIMEOUT: return _("sin respuesta");
    case MB_E_CRC: return _("respuesta dañada (CRC)");
    case MB_E_FRAME: return _("respuesta que no corresponde");
    case MB_E_IO: return _("falló el puerto o la conexión");
    case MB_E_ARG: return _("pedido inválido");
    default: return exc_name(rc);
    }
}

uint32_t mb_char_us(const mb_cfg_t *c, float chars)
{
    uint32_t baud = c->baud ? c->baud : 9600;
    int bits = 1 + 8 + (c->par == 'E' || c->par == 'O' ? 1 : 0) + (c->stop == 2 ? 2 : 1);
    return (uint32_t)(chars * (float)bits * 1e6f / (float)baud);
}
