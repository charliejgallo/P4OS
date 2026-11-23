/*
 * P4OS - Mi casa: the server's certificate, and when it ends.
 *
 * The HAL's TLS (aos_tls.c) verifies a certificate and says yes or no, but
 * does not hand it to the app, and mbedtls is not in the symbol table. So
 * the certificate is read the way a network scanner does: over a bare
 * socket (aos_tls_open without AOS_TLS_ENCRYPT), a TLS 1.2 ClientHello of
 * our own, and the server's answer read up to its Certificate message,
 * which TLS 1.2 sends in the clear. Then the connection is dropped: no
 * handshake is finished and nothing is verified here; whether the server
 * can be trusted is still the real check's business (ec_check.c).
 *
 * A server that speaks TLS 1.3 only refuses the hello; its days stay
 * unknown (a firmware call that hands the peer's certificate over would
 * fix it: a request in HANDOFF-NUEVAS-APPS.md).
 *
 * From the certificate: notAfter, the subject's CN, the first DNS name of
 * the subjectAltName (and whether it has IP addresses), and whether it was
 * issued by itself. The DER is kept on request: "trust this certificate"
 * saves it as the only authority for that service (aos_tls_open_trust).
 */
#include "ec.h"

#include <stdlib.h>

typedef struct ec_tls ec_tls_t;
ec_tls_t *aos_tls_new(void);
void aos_tls_free(ec_tls_t *c);
int  aos_tls_open(ec_tls_t *c, const char *host, int port, unsigned flags, int timeout_ms, char *why, size_t why_n);
bool aos_tls_send(ec_tls_t *c, const void *buf, int len, int timeout_ms);
int  aos_tls_recv(ec_tls_t *c, void *buf, int max, int timeout_ms);
#define TLS_NODELAY  0x02u
#define TLS_TIMEOUT  (-2)

#define HS_MAX   (48 * 1024)

/* ---- the hello ----------------------------------------------------------------------------------- */

static const uint16_t SUITES[] = {
    0xC02B, 0xC02F, 0xC02C, 0xC030, 0xCCA9, 0xCCA8, 0xC009, 0xC013, 0xC00A, 0xC014,
    0x009E, 0x009F, 0x009C, 0x009D, 0x002F, 0x0035, 0x00FF,
};
static const uint16_t SIGALGS[] = {
    0x0403, 0x0503, 0x0603, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601, 0x0203, 0x0201,
};

static uint8_t *put16(uint8_t *p, unsigned v)
{
    *p++ = (uint8_t)(v >> 8);
    *p++ = (uint8_t)v;
    return p;
}

static bool is_ip(const char *h)
{
    for (; *h; h++)
        if (!((*h >= '0' && *h <= '9') || *h == '.' || *h == ':')) return false;
    return true;
}

static int hello(uint8_t *o, const char *host)
{
    uint8_t *p = o + 9;
    *p++ = 3;
    *p++ = 3;                               /* TLS 1.2 */
    aos_hal_random(p, 32);
    p += 32;
    *p++ = 0;                               /* no session */
    p = put16(p, sizeof SUITES);
    for (size_t i = 0; i < sizeof SUITES / sizeof SUITES[0]; i++) p = put16(p, SUITES[i]);
    *p++ = 1;
    *p++ = 0;                               /* no compression */
    uint8_t *ext = p;
    p += 2;
    size_t hl = strlen(host);
    if (!is_ip(host) && hl < 200) {
        p = put16(p, 0x0000);
        p = put16(p, (unsigned)hl + 5);
        p = put16(p, (unsigned)hl + 3);
        *p++ = 0;
        p = put16(p, (unsigned)hl);
        memcpy(p, host, hl);
        p += hl;
    }
    p = put16(p, 0x000A);                   /* groups: x25519, P-256, P-384 */
    p = put16(p, 8);
    p = put16(p, 6);
    p = put16(p, 0x001D);
    p = put16(p, 0x0017);
    p = put16(p, 0x0018);
    p = put16(p, 0x000B);                   /* point formats: uncompressed */
    p = put16(p, 2);
    *p++ = 1;
    *p++ = 0;
    p = put16(p, 0x000D);                   /* signature algorithms */
    p = put16(p, sizeof SIGALGS + 2);
    p = put16(p, sizeof SIGALGS);
    for (size_t i = 0; i < sizeof SIGALGS / sizeof SIGALGS[0]; i++) p = put16(p, SIGALGS[i]);
    p = put16(p, 0x0017);                   /* extended master secret */
    p = put16(p, 0);
    p = put16(p, 0xFF01);                   /* renegotiation info */
    p = put16(p, 1);
    *p++ = 0;
    put16(ext, (unsigned)(p - ext - 2));
    int body = (int)(p - o - 9);
    o[0] = 0x16;                            /* handshake record */
    o[1] = 3;
    o[2] = 1;
    put16(o + 3, (unsigned)body + 4);
    o[5] = 1;                               /* ClientHello */
    o[6] = (uint8_t)(body >> 16);
    o[7] = (uint8_t)(body >> 8);
    o[8] = (uint8_t)body;
    return (int)(p - o);
}

/* ---- DER --------------------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;           /* the contents */
    int len;
    int tag;
    const uint8_t *whole;       /* tag and length included */
    int whole_len;
} tlv_t;

/* one element at p (n bytes left); false if it does not fit */
static bool tlv(const uint8_t *p, int n, tlv_t *t)
{
    if (n < 2) return false;
    t->tag = p[0];
    int l = p[1], h = 2;
    if (l & 0x80) {
        int nb = l & 0x7F;
        if (nb < 1 || nb > 3 || n < 2 + nb) return false;
        l = 0;
        for (int i = 0; i < nb; i++) l = (l << 8) | p[2 + i];
        h += nb;
    }
    if (l > n - h) return false;
    t->p = p + h;
    t->len = l;
    t->whole = p;
    t->whole_len = h + l;
    return true;
}

static bool next(tlv_t *in, int *off, tlv_t *out)
{
    if (*off >= in->len || !tlv(in->p + *off, in->len - *off, out)) return false;
    *off += out->whole_len;
    return true;
}

static int dig(const uint8_t *p, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static bool der_time(const tlv_t *t, int64_t *out)
{
    const uint8_t *s = t->p;
    int y, k;
    if (t->tag == 0x17 && t->len >= 12) {          /* UTCTime YYMMDDHHMMSSZ */
        y = dig(s, 2);
        if (y < 0) return false;
        y += y < 50 ? 2000 : 1900;
        k = 2;
    } else if (t->tag == 0x18 && t->len >= 14) {   /* GeneralizedTime YYYYMMDDHHMMSSZ */
        y = dig(s, 4);
        k = 4;
    } else {
        return false;
    }
    int mo = dig(s + k, 2), d = dig(s + k + 2, 2), h = dig(s + k + 4, 2), mi = dig(s + k + 6, 2),
        se = dig(s + k + 8, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || mi < 0 || se < 0) return false;
    *out = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    return true;
}

static void copy_str(char *out, size_t n, const uint8_t *p, int len)
{
    size_t k = 0;
    for (int i = 0; i < len && k + 1 < n; i++) out[k++] = p[i] >= 0x20 && p[i] < 0x7F ? (char)p[i] : '?';
    out[k] = '\0';
}

/* the CN of a Name */
static void name_cn(const tlv_t *name, char *out, size_t n)
{
    static const uint8_t CN[] = { 0x55, 0x04, 0x03 };
    int o1 = 0;
    tlv_t set, seq, oid, val;
    while (next((tlv_t *)name, &o1, &set)) {
        int o2 = 0;
        while (next(&set, &o2, &seq)) {
            int o3 = 0;
            if (!next(&seq, &o3, &oid) || !next(&seq, &o3, &val)) continue;
            if (oid.tag == 0x06 && oid.len == 3 && !memcmp(oid.p, CN, 3)) {
                copy_str(out, n, val.p, val.len);
                return;
            }
        }
    }
}

static void san(const tlv_t *exts, ec_cert_t *c)
{
    static const uint8_t SAN[] = { 0x55, 0x1D, 0x11 };
    int o1 = 0;
    tlv_t ext, oid, v, names, gn;
    while (next((tlv_t *)exts, &o1, &ext)) {
        int o2 = 0;
        if (!next(&ext, &o2, &oid) || oid.tag != 0x06 || oid.len != 3 || memcmp(oid.p, SAN, 3)) continue;
        if (!next(&ext, &o2, &v)) continue;
        if (v.tag == 0x01 && !next(&ext, &o2, &v)) continue;   /* critical */
        if (v.tag != 0x04 || !tlv(v.p, v.len, &names) || names.tag != 0x30) continue;
        int o3 = 0;
        while (next(&names, &o3, &gn)) {
            if (gn.tag == 0x82 && !c->dns[0]) copy_str(c->dns, sizeof c->dns, gn.p, gn.len);
            if (gn.tag == 0x87) c->san_ip = true;
        }
        return;
    }
}

static bool parse_cert(const uint8_t *der, int len, ec_cert_t *c)
{
    tlv_t cert, tbs, x, issuer, validity, subject, t1, t2;
    if (!tlv(der, len, &cert) || cert.tag != 0x30) return false;
    int o = 0;
    if (!next(&cert, &o, &tbs) || tbs.tag != 0x30) return false;
    o = 0;
    if (!next(&tbs, &o, &x)) return false;
    if (x.tag == 0xA0 && !next(&tbs, &o, &x)) return false;     /* version, then the serial */
    if (!next(&tbs, &o, &x)) return false;                       /* signature algorithm */
    if (!next(&tbs, &o, &issuer) || !next(&tbs, &o, &validity) || !next(&tbs, &o, &subject)) return false;
    int ov = 0;
    if (!next(&validity, &ov, &t1) || !next(&validity, &ov, &t2) || !der_time(&t2, &c->not_after)) return false;
    c->self = issuer.whole_len == subject.whole_len && !memcmp(issuer.whole, subject.whole, (size_t)issuer.whole_len);
    name_cn(&subject, c->name, sizeof c->name);
    if (!next(&tbs, &o, &x)) return true;                        /* the public key */
    while (next(&tbs, &o, &x)) {
        if (x.tag == 0xA3) {
            tlv_t exts;
            if (tlv(x.p, x.len, &exts) && exts.tag == 0x30) san(&exts, c);
        }
    }
    return true;
}

/* ---- the exchange ------------------------------------------------------------------------------- */

bool ec_cert_fetch(const char *host, int port, int timeout_ms, bool keep_der, ec_cert_t *out)
{
    memset(out, 0, sizeof *out);
    ec_tls_t *c = aos_tls_new();
    uint8_t *hs = ec_psram(HS_MAX);
    uint8_t *rec = ec_psram(16 * 1024 + 5);
    if (!c || !hs || !rec) {
        if (c) aos_tls_free(c);
        free(hs);
        free(rec);
        return false;
    }
    bool ok = false;
    if (aos_tls_open(c, host, port, TLS_NODELAY, timeout_ms, NULL, 0) != 0) goto done;
    int n = hello(rec, host);
    if (!aos_tls_send(c, rec, n, timeout_ms)) goto done;
    int hs_len = 0, rl = 0;
    uint32_t deadline = (uint32_t)aos_hal_uptime_ms() + (uint32_t)timeout_ms;
    for (;;) {
        if ((int32_t)((uint32_t)aos_hal_uptime_ms() - deadline) >= 0) goto done;
        /* a whole record */
        int want = rl < 5 ? 5 : 5 + ((rec[3] << 8) | rec[4]);
        if (want > 16 * 1024 + 5) goto done;
        if (rl < want) {
            int got = aos_tls_recv(c, rec + rl, want - rl, 200);
            if (got == TLS_TIMEOUT) continue;
            if (got <= 0) goto done;
            rl += got;
            continue;
        }
        if (rec[0] != 0x16) goto done;      /* an alert, or not TLS at all */
        int plen = rl - 5;
        if (hs_len + plen > HS_MAX) goto done;
        memcpy(hs + hs_len, rec + 5, (size_t)plen);
        hs_len += plen;
        rl = 0;
        /* the messages so far */
        int off = 0;
        while (off + 4 <= hs_len) {
            int mt = hs[off];
            int ml = (hs[off + 1] << 16) | (hs[off + 2] << 8) | hs[off + 3];
            if (off + 4 + ml > hs_len) break;
            if (mt == 11) {                 /* Certificate */
                const uint8_t *m = hs + off + 4;
                if (ml < 6) goto done;
                int cl = (m[3] << 16) | (m[4] << 8) | m[5];
                if (cl <= 0 || 6 + cl > ml) goto done;
                ok = parse_cert(m + 6, cl, out);
                if (ok) {
                    out->ok = true;
                    if (keep_der) {
                        out->der = malloc((size_t)cl);
                        if (out->der) {
                            memcpy(out->der, m + 6, (size_t)cl);
                            out->der_len = cl;
                        }
                    }
                }
                goto done;
            }
            if (mt == 14) goto done;        /* ServerHelloDone with no certificate */
            off += 4 + ml;
        }
    }
done:
    aos_tls_free(c);
    free(hs);
    free(rec);
    return ok;
}

/* ---- trusting one -------------------------------------------------------------------------------- */

char *ec_cert_pem(const uint8_t *der, int len)
{
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int b64 = (len + 2) / 3 * 4;
    char *o = malloc((size_t)b64 + b64 / 64 + 80);
    if (!o) return NULL;
    size_t cap = (size_t)b64 + (size_t)b64 / 64 + 80;
    int k = snprintf(o, cap, "-----BEGIN CERTIFICATE-----\n");
    int col = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)der[i] << 16;
        if (i + 1 < len) v |= (uint32_t)der[i + 1] << 8;
        if (i + 2 < len) v |= der[i + 2];
        o[k++] = B64[(v >> 18) & 63];
        o[k++] = B64[(v >> 12) & 63];
        o[k++] = i + 1 < len ? B64[(v >> 6) & 63] : '=';
        o[k++] = i + 2 < len ? B64[v & 63] : '=';
        col += 4;
        if (col == 64) {
            o[k++] = '\n';
            col = 0;
        }
    }
    if (col) o[k++] = '\n';
    snprintf(o + k, cap - (size_t)k, "-----END CERTIFICATE-----\n");
    return o;
}

void ec_cert_pin_path(uint32_t key, char *out, size_t n)
{
    snprintf(out, n, "%s/estado_%08lx.pem", aos_hal_path_data(), (unsigned long)key);
}

bool ec_cert_trust(int i)
{
    if (i < 0 || i >= ec_app.cfg.n) return false;
    ec_lock();
    int len = 0;
    const uint8_t *der = ec_rt_der(i, &len);
    char *pem = der ? ec_cert_pem(der, len) : NULL;
    char name[96];
    ec_copy(name, sizeof name, ec_rt(i)->cert_check);
    ec_unlock();
    if (!pem) return false;
    char path[200];
    ec_cert_pin_path(ec_svc_key(&ec_app.cfg.s[i]), path, sizeof path);
    FILE *f = fopen(path, "w");
    bool ok = false;
    if (f) {
        fprintf(f, "name=%s\n%s", name, pem);
        ok = !ferror(f);
        fclose(f);
    }
    free(pem);
    return ok;
}
