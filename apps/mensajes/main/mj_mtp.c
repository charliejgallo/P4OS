/*
 * P4OS - Mensajes: MTProto 2.0 (mj_mtp.h says what and how).
 */
#include "mj_mtp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mj_crypto.h"
#include "mj_port.h"

#define APP_LAYER MJ_LAYER

/* Telegram's production key (core.telegram.org/mtproto/auth_key, and the
 * one every client carries; fingerprint 0xd09d1d85de64fd85), e = 65537. */
static const uint8_t TG_RSA_N[256] = {
    0xe8, 0xbb, 0x33, 0x05, 0xc0, 0xb5, 0x2c, 0x6c, 0xf2, 0xaf, 0xdf, 0x76, 0x37, 0x31, 0x34, 0x89, 0xe6, 0x3e,
    0x05, 0x26, 0x8e, 0x5b, 0xad, 0xb6, 0x01, 0xaf, 0x41, 0x77, 0x86, 0x47, 0x2e, 0x5f, 0x93, 0xb8, 0x54, 0x38,
    0x96, 0x8e, 0x20, 0xe6, 0x72, 0x9a, 0x30, 0x1c, 0x0a, 0xfc, 0x12, 0x1b, 0xf7, 0x15, 0x1f, 0x83, 0x44, 0x36,
    0xf7, 0xfd, 0xa6, 0x80, 0x84, 0x7a, 0x66, 0xbf, 0x64, 0xac, 0xce, 0xc7, 0x8e, 0xe2, 0x1c, 0x0b, 0x31, 0x6f,
    0x0e, 0xda, 0xfe, 0x2f, 0x41, 0x90, 0x8d, 0xa7, 0xbd, 0x1f, 0x4a, 0x51, 0x07, 0x63, 0x8e, 0xeb, 0x67, 0x04,
    0x0a, 0xce, 0x47, 0x2a, 0x14, 0xf9, 0x0d, 0x9f, 0x7c, 0x2b, 0x7d, 0xef, 0x99, 0x68, 0x8b, 0xa3, 0x07, 0x3a,
    0xdb, 0x57, 0x50, 0xbb, 0x02, 0x96, 0x49, 0x02, 0xa3, 0x59, 0xfe, 0x74, 0x5d, 0x81, 0x70, 0xe3, 0x68, 0x76,
    0xd4, 0xfd, 0x8a, 0x5d, 0x41, 0xb2, 0xa7, 0x6c, 0xbf, 0xf9, 0xa1, 0x32, 0x67, 0xeb, 0x95, 0x80, 0xb2, 0xd0,
    0x6d, 0x10, 0x35, 0x74, 0x48, 0xd2, 0x0d, 0x9d, 0xa2, 0x19, 0x1c, 0xb5, 0xd8, 0xc9, 0x39, 0x82, 0x96, 0x1c,
    0xdf, 0xde, 0xda, 0x62, 0x9e, 0x37, 0xf1, 0xfb, 0x09, 0xa0, 0x72, 0x20, 0x27, 0x69, 0x60, 0x32, 0xfe, 0x61,
    0xed, 0x66, 0x3d, 0xb7, 0xa3, 0x7f, 0x6f, 0x26, 0x3d, 0x37, 0x0f, 0x69, 0xdb, 0x53, 0xa0, 0xdc, 0x0a, 0x17,
    0x48, 0xbd, 0xaa, 0xff, 0x62, 0x09, 0xd5, 0x64, 0x54, 0x85, 0xe6, 0xe0, 0x01, 0xd1, 0x95, 0x32, 0x55, 0x75,
    0x7e, 0x4b, 0x8e, 0x42, 0x81, 0x33, 0x47, 0xb1, 0x1d, 0xa6, 0xab, 0x50, 0x0f, 0xd0, 0xac, 0xe7, 0xe6, 0xdf,
    0xa3, 0x73, 0x61, 0x99, 0xcc, 0xaf, 0x93, 0x97, 0xed, 0x07, 0x45, 0xa4, 0x27, 0xdc, 0xfa, 0x6c, 0xd6, 0x7b,
    0xcb, 0x1a, 0xcf, 0xf3,
};

/* the data centres' addresses, IPv4 (Telegram Desktop's built-in list) */
static const char *const TG_DC_IP[MJ_DC_MAX + 1] = {
    NULL, "149.154.175.50", "149.154.167.51", "149.154.175.100", "149.154.167.91", "149.154.171.5",
};

#define RX_MAX       (2 * 1024 * 1024)   /* the largest packet accepted       */
#define PING_MS      45000
#define IDLE_CLOSE_MS 60000              /* a data centre that is not home    */
#define RESEND_MS    25000               /* sent, not even acked              */
#define GIVEUP_MS    90000               /* acked, never answered             */

typedef struct mj_req {
    struct mj_req *next;
    uint8_t *body;
    size_t n;
    int64_t msg_id;             /* 0: to send                                 */
    int64_t container;          /* the container it went in, 0 none           */
    uint32_t sent_ms, not_before_ms;
    int tries;
    bool acked;
    bool is_ping;
    mj_rpc_cb_t cb;
    void *ctx;
} mj_req_t;

typedef struct {
    int dc;
    int sock;
    int state;
    uint8_t key[256];
    uint8_t key_id[8];
    bool has_key;
    int64_t salt, session_id;
    uint32_t seq;
    bool inited;
    uint8_t *rx;
    size_t rx_n, rx_cap;
    int64_t acks[64];
    int nacks;
    uint32_t ack_ms;
    mj_req_t *reqs;
    uint32_t last_io_ms, last_ping_ms, retry_at_ms;
    int fails;
} dc_t;

struct mj_mtp {
    mj_mtp_cfg_t cfg;
    char host[64];
    uint8_t rsa_n[256];
    int64_t fp_tg, fp_own;
    dc_t dc[MJ_DC_MAX + 1];
    int home;
    int64_t clock0;             /* unix seconds when made (0: unknown)        */
    uint32_t ms0;
    int64_t off_ms;             /* what to add to our clock to get the server's */
    int64_t last_msg_id;
    char app_version[24], device[40], system[24], lang[8];
};

/* ---------------------------------------------------------------- small */

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int64_t rd64(const uint8_t *p)
{
    return (int64_t)((uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32);
}

static void le64(uint8_t *p, int64_t v)
{
    le32(p, (uint32_t)(uint64_t)v);
    le32(p + 4, (uint32_t)((uint64_t)v >> 32));
}

int mj_err_num(const char *msg, const char *prefix)
{
    if (!msg) return -1;
    size_t n = strlen(prefix);
    if (strncmp(msg, prefix, n)) return -1;
    const char *p = msg + n;
    if (*p < '0' || *p > '9') return -1;
    return atoi(p);
}

static int64_t fingerprint(const uint8_t n[256])
{
    /* SHA-1 of the key as TL (two strings: n and e), its last 8 bytes */
    mj_buf_t b = {0};
    mj_buf_bytes(&b, n, 256);
    static const uint8_t e[3] = {1, 0, 1};
    mj_buf_bytes(&b, e, 3);
    uint8_t h[20];
    mj_sha1(b.p, b.n, h);
    mj_buf_free(&b);
    return rd64(h + 12);
}

/* ---------------------------------------------------------------- clock */

static int64_t server_ms(mj_mtp_t *m)
{
    int64_t local = m->clock0 * 1000 + (int64_t)(uint32_t)(mj_now_ms() - m->ms0);
    return local + m->off_ms;
}

int64_t mj_mtp_server_time(mj_mtp_t *m)
{
    return server_ms(m) / 1000;
}

static void learn_time(mj_mtp_t *m, int64_t server_unix)
{
    int64_t local = m->clock0 * 1000 + (int64_t)(uint32_t)(mj_now_ms() - m->ms0);
    m->off_ms = server_unix * 1000 - local;
}

static int64_t new_msg_id(mj_mtp_t *m)
{
    int64_t ms = server_ms(m);
    uint64_t sec = (uint64_t)(ms / 1000);
    uint32_t frac = (uint32_t)(((uint64_t)(ms % 1000) << 32) / 1000);
    int64_t id = (int64_t)(sec << 32 | (frac & ~3u));
    if (id <= m->last_msg_id) id = m->last_msg_id + 4;
    m->last_msg_id = id;
    return id;
}

/* ------------------------------------------------------------ the socket */

static void dc_state(mj_mtp_t *m, dc_t *d, int st)
{
    if (d->state == st) return;
    d->state = st;
    if (m->cfg.on_state) m->cfg.on_state(m->cfg.ud, d->dc, st);
}

static void dc_close(mj_mtp_t *m, dc_t *d)
{
    if (d->sock > 0) mj_tcp_close(d->sock);
    d->sock = 0;
    d->rx_n = 0;
    /* what was sent and not answered goes again on the next connection */
    for (mj_req_t *r = d->reqs; r; r = r->next) {
        r->msg_id = 0;
        r->container = 0;
        r->acked = false;
    }
    d->nacks = 0;
    dc_state(m, d, MJ_NET_DOWN);
}

static void dc_host(mj_mtp_t *m, int dc, char *host, size_t hn, int *port)
{
    if (m->host[0]) {
        snprintf(host, hn, "%s", m->host);
        *port = m->cfg.port + dc - 1;
    } else {
        snprintf(host, hn, "%s", TG_DC_IP[dc]);
        *port = 443;
    }
}

static bool dc_connect(mj_mtp_t *m, dc_t *d)
{
    char host[64];
    int port;
    dc_host(m, d->dc, host, sizeof host, &port);
    dc_state(m, d, MJ_NET_CONNECTING);
    int s = mj_tcp_connect(host, port, 8000);
    if (s <= 0) {
        d->fails++;
        uint32_t back = d->fails > 5 ? 30000 : 1000u << d->fails;
        d->retry_at_ms = mj_now_ms() + back;
        mj_log("mtp: dc%d: cannot connect (%d), again in %u s", d->dc, s, (unsigned)(back / 1000));
        dc_state(m, d, MJ_NET_DOWN);
        return false;
    }
    /* the transport's tag: intermediate */
    static const uint8_t tag[4] = {0xee, 0xee, 0xee, 0xee};
    if (mj_tcp_send(s, tag, 4, 5000) != 4) {
        mj_tcp_close(s);
        d->fails++;
        d->retry_at_ms = mj_now_ms() + 2000;
        dc_state(m, d, MJ_NET_DOWN);
        return false;
    }
    d->sock = s;
    d->rx_n = 0;
    d->last_io_ms = d->last_ping_ms = mj_now_ms();
    return true;
}

static bool send_packet(mj_mtp_t *m, dc_t *d, const uint8_t *p, size_t n)
{
    uint8_t h[4];
    le32(h, (uint32_t)n);
    if (mj_tcp_send(d->sock, h, 4, 10000) != 4 || mj_tcp_send(d->sock, p, (int)n, 20000) != (int)n) {
        mj_log("mtp: dc%d: send failed", d->dc);
        dc_close(m, d);
        return false;
    }
    d->last_io_ms = mj_now_ms();
    return true;
}

/* Reads what is there into rx. -1 closed, 0 nothing, 1 something. */
static int rx_fill(mj_mtp_t *m, dc_t *d, int wait_ms)
{
    if (d->rx_cap - d->rx_n < 16384) {
        size_t cap = d->rx_cap ? d->rx_cap * 2 : 65536;
        if (cap > RX_MAX + 16384) cap = RX_MAX + 16384;
        if (cap > d->rx_cap) {
            uint8_t *p = (uint8_t *)mj_realloc(d->rx, cap);
            if (!p) return -1;
            d->rx = p;
            d->rx_cap = cap;
        }
    }
    size_t room = d->rx_cap - d->rx_n;
    if (!room) return -1;
    int r = mj_tcp_recv(d->sock, d->rx + d->rx_n, (int)room, wait_ms);
    if (r < 0) {
        mj_log("mtp: dc%d: connection closed", d->dc);
        dc_close(m, d);
        return -1;
    }
    if (r == 0) return 0;
    d->rx_n += (size_t)r;
    d->last_io_ms = mj_now_ms();
    return 1;
}

/* A whole packet in rx: its length, 0 not yet. -1 broken. */
static long rx_packet(dc_t *d)
{
    if (d->rx_n < 4) return 0;
    uint32_t n = rd32(d->rx);
    if (n > RX_MAX) return -1;
    if (d->rx_n < 4 + (size_t)n) return 0;
    return (long)n;
}

static void rx_drop(dc_t *d, size_t n)
{
    memmove(d->rx, d->rx + 4 + n, d->rx_n - 4 - n);
    d->rx_n -= 4 + n;
}

/* ------------------------------------------------------------ handshake */

static bool hs_send(mj_mtp_t *m, dc_t *d, const tl_obj_t *q)
{
    mj_buf_t b = {0};
    uint8_t h[20];
    memset(h, 0, 8);
    le64(h + 8, new_msg_id(m));
    mj_buf_put(&b, h, 20);
    if (!tl_write(&b, q)) {
        mj_buf_free(&b);
        return false;
    }
    le32(b.p + 16, (uint32_t)(b.n - 20));
    bool ok = !b.oom && send_packet(m, d, b.p, b.n);
    mj_buf_free(&b);
    return ok;
}

static tl_obj_t *hs_recv(mj_mtp_t *m, dc_t *d, mj_arena_t *a, uint32_t expect)
{
    uint32_t t0 = mj_now_ms();
    for (;;) {
        long n = rx_packet(d);
        if (n < 0) return NULL;
        if (n > 0) {
            const uint8_t *p = d->rx + 4;
            if (n == 4) {
                mj_log("mtp: dc%d: the server says %d during the handshake", d->dc, (int)rd32(p));
                rx_drop(d, (size_t)n);
                return NULL;
            }
            tl_obj_t *o = n > 20 ? tl_read_obj(a, p + 20, (size_t)n - 20, NULL) : NULL;
            rx_drop(d, (size_t)n);
            if (!o) return NULL;
            if (expect && o->id != expect && tl_ctors[o->ci].result != tl_ctors[tl_ctor_index(expect)].result) {
                mj_log("mtp: dc%d: handshake got %s", d->dc, tl_name(o->id));
                return NULL;
            }
            return o;
        }
        if ((uint32_t)(mj_now_ms() - t0) > 15000) {
            mj_log("mtp: dc%d: the handshake timed out", d->dc);
            return NULL;
        }
        if (m->cfg.stop && *m->cfg.stop) return NULL;
        if (rx_fill(m, d, 200) < 0) return NULL;
    }
}

static void be_min(uint32_t v, uint8_t *out, uint32_t *n)
{
    uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    int i = 0;
    while (i < 3 && t[i] == 0) i++;
    *n = (uint32_t)(4 - i);
    memcpy(out, t + i, *n);
}

/* RSA_PAD (core.telegram.org/mtproto/auth_key, "4.1"). */
static bool rsa_pad(const uint8_t *data, size_t dn, const uint8_t n[256], uint8_t out[256])
{
    static const uint8_t e[3] = {1, 0, 1};
    uint8_t dwp[192], dwh[224], kae[256], tk[32], zero[32] = {0}, h[32];
    if (dn > 144) return false;
    for (int tries = 0; tries < 20; tries++) {
        memcpy(dwp, data, dn);
        mj_random(dwp + dn, 192 - dn);
        for (int i = 0; i < 192; i++) dwh[i] = dwp[191 - i];
        mj_random(tk, 32);
        mj_sha256_2(tk, 32, dwp, 192, dwh + 192);
        mj_ige_encrypt(tk, zero, dwh, 224);
        mj_sha256(dwh, 224, h);
        for (int i = 0; i < 32; i++) kae[i] = tk[i] ^ h[i];
        memcpy(kae + 32, dwh, 224);
        if (memcmp(kae, n, 256) >= 0) continue;     /* must be below the modulus */
        return mj_modexp_be(out, 256, kae, 256, e, 3, n, 256);
    }
    return false;
}

static void tmp_aes(const uint8_t nn[32], const uint8_t sn[16], uint8_t key[32], uint8_t iv[32])
{
    uint8_t a[20], b[20], c[20];
    mj_sha1_2(nn, 32, sn, 16, a);
    mj_sha1_2(sn, 16, nn, 32, b);
    mj_sha1_2(nn, 32, nn, 32, c);
    memcpy(key, a, 20);
    memcpy(key + 20, b, 12);
    memcpy(iv, b + 12, 8);
    memcpy(iv + 8, c, 20);
    memcpy(iv + 28, nn, 4);
}

static bool make_key(mj_mtp_t *m, dc_t *d)
{
    mj_arena_t a = {0};
    bool ok = false;
    uint32_t t0 = mj_now_ms();
    uint8_t nonce[16], snonce[16], nn[32], key[32], iv[32];
    uint8_t *ga = NULL, *gb = (uint8_t *)mj_malloc(256), *b = (uint8_t *)mj_malloc(256),
            *ak = (uint8_t *)mj_malloc(256), *buf = (uint8_t *)mj_malloc(1024);
    mj_buf_t w = {0};
    if (!gb || !b || !ak || !buf) goto out;
    dc_state(m, d, MJ_NET_KEY);
    mj_log("mtp: dc%d: making an auth key", d->dc);

    /* 1. req_pq_multi */
    mj_random(nonce, 16);
    tl_obj_t *q = tl_new(&a, TLC_req_pq_multi);
    tl_set_b(q, TLN_nonce, nonce, 16);
    if (!hs_send(m, d, q)) goto out;
    tl_obj_t *res = hs_recv(m, d, &a, TLC_resPQ);
    uint32_t len;
    if (!res || memcmp(tl_b(res, TLN_nonce, &len), nonce, 16)) goto out;
    memcpy(snonce, tl_b(res, TLN_server_nonce, &len), 16);
    const uint8_t *pqb = tl_b(res, TLN_pq, &len);
    if (!pqb || len > 8) goto out;
    uint64_t pq = 0;
    for (uint32_t i = 0; i < len; i++) pq = pq << 8 | pqb[i];
    tl_vec_t *fps = tl_v(res, TLN_server_public_key_fingerprints);
    const uint8_t *rsa = NULL;
    int64_t fp = 0;
    for (uint32_t i = 0; i < tl_vn(fps); i++) {
        int64_t f = tl_vl(fps, i);
        if (m->host[0] && f == m->fp_own) rsa = m->rsa_n, fp = f;
        else if (!m->host[0] && f == m->fp_tg) rsa = TG_RSA_N, fp = f;
    }
    if (!rsa) {
        mj_log("mtp: dc%d: none of the server's keys is ours", d->dc);
        goto out;
    }

    /* 2. pq, and the inner data under RSA */
    uint32_t p, qq;
    if (!mj_factor_pq(pq, &p, &qq)) {
        mj_log("mtp: dc%d: pq did not split", d->dc);
        goto out;
    }
    uint8_t pb[4], qb[4];
    uint32_t pn, qn;
    be_min(p, pb, &pn);
    be_min(qq, qb, &qn);
    mj_random(nn, 32);
    tl_obj_t *inner = tl_new(&a, TLC_p_q_inner_data_dc);
    tl_set_b(inner, TLN_pq, pqb, len);
    tl_set_b(inner, TLN_p, pb, pn);
    tl_set_b(inner, TLN_q, qb, qn);
    tl_set_b(inner, TLN_nonce, nonce, 16);
    tl_set_b(inner, TLN_server_nonce, snonce, 16);
    tl_set_b(inner, TLN_new_nonce, nn, 32);
    tl_set_i(inner, TLN_dc, d->dc);
    if (!tl_write(&w, inner)) goto out;
    uint8_t enc[256];
    if (!rsa_pad(w.p, w.n, rsa, enc)) goto out;
    mj_buf_free(&w);
    q = tl_new(&a, TLC_req_DH_params);
    tl_set_b(q, TLN_nonce, nonce, 16);
    tl_set_b(q, TLN_server_nonce, snonce, 16);
    tl_set_b(q, TLN_p, pb, pn);
    tl_set_b(q, TLN_q, qb, qn);
    tl_set_l(q, TLN_public_key_fingerprint, fp);
    tl_set_b(q, TLN_encrypted_data, enc, 256);
    if (!hs_send(m, d, q)) goto out;
    res = hs_recv(m, d, &a, TLC_server_DH_params_ok);
    if (!res || res->id != TLC_server_DH_params_ok || memcmp(tl_b(res, TLN_nonce, &len), nonce, 16) ||
        memcmp(tl_b(res, TLN_server_nonce, &len), snonce, 16))
        goto out;

    /* 3. the server's half of Diffie-Hellman */
    const uint8_t *ea = tl_b(res, TLN_encrypted_answer, &len);
    if (!ea || len % 16 || len < 64 || len > 1024) goto out;
    memcpy(buf, ea, len);
    tmp_aes(nn, snonce, key, iv);
    mj_ige_decrypt(key, iv, buf, len);
    size_t used = 0;
    tl_obj_t *sdh = tl_read_obj(&a, buf + 20, len - 20, &used);
    uint8_t h[20];
    if (!sdh || sdh->id != TLC_server_DH_inner_data) goto out;
    mj_sha1(buf + 20, used, h);
    if (memcmp(h, buf, 20) || memcmp(tl_b(sdh, TLN_nonce, &len), nonce, 16)) {
        mj_log("mtp: dc%d: the DH answer's hash is wrong", d->dc);
        goto out;
    }
    int g = tl_i(sdh, TLN_g);
    uint32_t pl, gal;
    const uint8_t *prime = tl_b(sdh, TLN_dh_prime, &pl);
    const uint8_t *gap = tl_b(sdh, TLN_g_a, &gal);
    if (pl != 256 || !gap || gal > 256 || !mj_dh_check(g, prime, gap, gal)) {
        mj_log("mtp: dc%d: the server's DH parameters fail the checks", d->dc);
        goto out;
    }
    ga = (uint8_t *)mj_calloc(256);
    if (!ga) goto out;
    memcpy(ga + 256 - gal, gap, gal);
    learn_time(m, tl_i(sdh, TLN_server_time));

    /* 4. ours, and the key */
    int64_t retry = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t gg = (uint8_t)g;
        do {
            mj_random(b, 256);
            if (!mj_modexp_be(gb, 256, &gg, 1, b, 256, MJ_DH_PRIME, 256)) goto out;
        } while (!mj_dh_check(g, MJ_DH_PRIME, gb, 256));
        tl_obj_t *ci = tl_new(&a, TLC_client_DH_inner_data);
        tl_set_b(ci, TLN_nonce, nonce, 16);
        tl_set_b(ci, TLN_server_nonce, snonce, 16);
        tl_set_l(ci, TLN_retry_id, retry);
        tl_set_b(ci, TLN_g_b, gb, 256);
        mj_buf_t cw = {0};
        uint8_t z[20] = {0};
        mj_buf_put(&cw, z, 20);
        if (!tl_write(&cw, ci)) {
            mj_buf_free(&cw);
            goto out;
        }
        mj_sha1(cw.p + 20, cw.n - 20, cw.p);
        uint8_t pad[16];
        mj_random(pad, 16);
        mj_buf_put(&cw, pad, (16 - cw.n % 16) % 16);
        if (cw.oom) {
            mj_buf_free(&cw);
            goto out;
        }
        mj_ige_encrypt(key, iv, cw.p, cw.n);
        q = tl_new(&a, TLC_set_client_DH_params);
        tl_set_b(q, TLN_nonce, nonce, 16);
        tl_set_b(q, TLN_server_nonce, snonce, 16);
        tl_set_b(q, TLN_encrypted_data, cw.p, (uint32_t)cw.n);
        bool sent = hs_send(m, d, q);
        mj_buf_free(&cw);
        if (!sent) goto out;
        if (!mj_modexp_be(ak, 256, ga, 256, b, 256, MJ_DH_PRIME, 256)) goto out;
        res = hs_recv(m, d, &a, TLC_dh_gen_ok);
        if (!res) goto out;
        uint8_t akh[20], nh[41];
        mj_sha1(ak, 256, akh);
        memcpy(nh, nn, 32);
        memcpy(nh + 33, akh, 8);
        if (res->id == TLC_dh_gen_ok) {
            nh[32] = 1;
            mj_sha1(nh, 41, h);
            if (memcmp(h + 4, tl_b(res, TLN_new_nonce_hash1, &len), 16)) {
                mj_log("mtp: dc%d: new_nonce_hash1 is wrong", d->dc);
                goto out;
            }
            memcpy(d->key, ak, 256);
            memcpy(d->key_id, akh + 12, 8);
            d->has_key = true;
            uint8_t s8[8];
            for (int i = 0; i < 8; i++) s8[i] = nn[i] ^ snonce[i];
            d->salt = rd64(s8);
            ok = true;
            break;
        }
        if (res->id == TLC_dh_gen_retry) {
            retry = rd64(akh);
            continue;
        }
        mj_log("mtp: dc%d: dh_gen_fail", d->dc);
        goto out;
    }
    if (ok) {
        mj_log("mtp: dc%d: auth key made in %u ms", d->dc, (unsigned)(mj_now_ms() - t0));
        if (m->cfg.on_key) m->cfg.on_key(m->cfg.ud, d->dc, d->key);
    }
out:
    mj_buf_free(&w);
    if (ak) memset(ak, 0, 256);
    if (b) memset(b, 0, 256);
    mj_free(ga);
    mj_free(gb);
    mj_free(b);
    mj_free(ak);
    mj_free(buf);
    mj_arena_free(&a);
    if (!ok) {
        d->fails++;
        d->retry_at_ms = mj_now_ms() + (d->fails > 4 ? 30000u : 2000u * (uint32_t)d->fails);
        dc_close(m, d);
    }
    return ok;
}

/* ------------------------------------------------------- sealed messages */

static void new_session(mj_mtp_t *m, dc_t *d)
{
    mj_random(&d->session_id, 8);
    d->seq = 0;
    d->inited = false;
    (void)m;
}

static void derive(const uint8_t *ak, const uint8_t *msg_key, int x, uint8_t key[32], uint8_t iv[32])
{
    uint8_t a[32], b[32];
    mj_sha256_2(msg_key, 16, ak + x, 36, a);
    mj_sha256_2(ak + 40 + x, 36, msg_key, 16, b);
    memcpy(key, a, 8);
    memcpy(key + 8, b + 8, 16);
    memcpy(key + 24, a + 24, 8);
    memcpy(iv, b, 8);
    memcpy(iv + 8, a + 8, 16);
    memcpy(iv + 24, b + 24, 8);
}

/* Seals one message (msg_id, seqno, body) and sends it. */
static bool send_sealed(mj_mtp_t *m, dc_t *d, int64_t msg_id, uint32_t seqno, const uint8_t *body, size_t n)
{
    size_t pn = 32 + n;
    size_t pad = 12 + (16 - (pn + 12) % 16) % 16;
    uint8_t rnd;
    mj_random(&rnd, 1);
    pad += 16 * (rnd & 3);
    size_t total = 24 + pn + pad;
    uint8_t *p = (uint8_t *)mj_malloc(total);
    if (!p) return false;
    uint8_t *pt = p + 24;
    le64(pt, d->salt);
    le64(pt + 8, d->session_id);
    le64(pt + 16, msg_id);
    le32(pt + 24, seqno);
    le32(pt + 28, (uint32_t)n);
    memcpy(pt + 32, body, n);
    mj_random(pt + pn, pad);
    uint8_t big[32], key[32], iv[32];
    mj_sha256_2(d->key + 88, 32, pt, pn + pad, big);
    memcpy(p, d->key_id, 8);
    memcpy(p + 8, big + 8, 16);
    derive(d->key, big + 8, 0, key, iv);
    mj_ige_encrypt(key, iv, pt, pn + pad);
    bool ok = send_packet(m, d, p, total);
    mj_free(p);
    return ok;
}

/* The wrapper of a session's first request:
 * invokeWithLayer(layer, initConnection(..., query)). */
static bool wrap_init(mj_mtp_t *m, const uint8_t *body, size_t n, mj_buf_t *out)
{
    mj_arena_t a = {0};
    tl_obj_t *ic = tl_new(&a, TLC_initConnection);
    tl_set_i(ic, TLN_api_id, m->cfg.api_id);
    tl_set_s(ic, TLN_device_model, m->device);
    tl_set_s(ic, TLN_system_version, m->system);
    tl_set_s(ic, TLN_app_version, m->app_version);
    tl_set_s(ic, TLN_system_lang_code, m->lang);
    tl_set_s(ic, TLN_lang_pack, "");
    tl_set_s(ic, TLN_lang_code, m->lang);
    tl_set_raw(ic, TLN_query, body, (uint32_t)n);
    tl_obj_t *il = tl_new(&a, TLC_invokeWithLayer);
    tl_set_i(il, TLN_layer, APP_LAYER);
    tl_set_o(il, TLN_query, ic);
    bool ok = tl_write(out, il);
    mj_arena_free(&a);
    return ok && !out->oom;
}

/* Everything waiting on a data centre goes out: the requests (one message
 * each, or a container when there are more of them or acks), and the acks. */
static void flush(mj_mtp_t *m, dc_t *d)
{
    if (d->sock <= 0 || !d->has_key) return;
    uint32_t now = mj_now_ms();
    mj_req_t *todo[16];
    int nt = 0;
    for (mj_req_t *r = d->reqs; r && nt < 16; r = r->next) {
        if (r->msg_id) continue;
        if (r->not_before_ms && (int32_t)(now - r->not_before_ms) < 0) continue;
        todo[nt++] = r;
        if (!d->inited) break;          /* the first one goes alone, wrapped */
    }
    bool acks_due = d->nacks && (d->nacks >= 16 || (uint32_t)(now - d->ack_ms) > 800 || nt);
    if (!nt && !acks_due) return;

    mj_buf_t ackb = {0};
    if (acks_due) {
        mj_arena_t a = {0};
        tl_obj_t *ack = tl_new(&a, TLC_msgs_ack);
        tl_vec_t *v = tl_vec_new(&a, (uint32_t)d->nacks);
        for (int i = 0; i < d->nacks; i++) tl_vset_l(v, (uint32_t)i, d->acks[i]);
        tl_set_v(ack, TLN_msg_ids, v);
        tl_write(&ackb, ack);
        mj_arena_free(&a);
        d->nacks = 0;
    }

    /* each item: its body (wrapped for the first of a session) */
    mj_buf_t items[17];
    int64_t ids[17];
    uint32_t seqs[17];
    int ni = 0;
    for (int i = 0; i < nt; i++) {
        mj_buf_t *b = &items[ni];
        memset(b, 0, sizeof *b);
        if (!d->inited) {
            if (!wrap_init(m, todo[i]->body, todo[i]->n, b)) {
                mj_buf_free(b);
                continue;
            }
            d->inited = true;
        } else {
            mj_buf_put(b, todo[i]->body, todo[i]->n);
        }
        ids[ni] = new_msg_id(m);
        seqs[ni] = d->seq++ * 2 + 1;
        todo[i]->msg_id = ids[ni];
        todo[i]->sent_ms = now;
        todo[i]->tries++;
        todo[i]->acked = false;
        todo[i]->container = 0;
        ni++;
    }
    if (ackb.n) {
        items[ni] = ackb;
        ids[ni] = new_msg_id(m);
        seqs[ni] = d->seq * 2;
        ni++;
    }
    if (ni == 1) {
        send_sealed(m, d, ids[0], seqs[0], items[0].p, items[0].n);
    } else if (ni > 1) {
        /* msg_container: msg_id, seqno, length and body of each */
        mj_buf_t c = {0};
        mj_buf_u32(&c, TL_CONTAINER);
        mj_buf_u32(&c, (uint32_t)ni);
        for (int i = 0; i < ni; i++) {
            mj_buf_u64(&c, (uint64_t)ids[i]);
            mj_buf_u32(&c, seqs[i]);
            mj_buf_u32(&c, (uint32_t)items[i].n);
            mj_buf_put(&c, items[i].p, items[i].n);
        }
        int64_t cid = new_msg_id(m);
        for (int i = 0; i < nt; i++)
            if (todo[i]->msg_id) todo[i]->container = cid;
        if (!c.oom) send_sealed(m, d, cid, d->seq * 2, c.p, c.n);
        mj_buf_free(&c);
    }
    for (int i = 0; i < ni; i++) mj_buf_free(&items[i]);
}

/* ------------------------------------------------------------- answers */

static mj_req_t *find_req(dc_t *d, int64_t msg_id, mj_req_t ***link)
{
    mj_req_t **l = &d->reqs;
    while (*l) {
        if ((*l)->msg_id == msg_id) {
            if (link) *link = l;
            return *l;
        }
        l = &(*l)->next;
    }
    return NULL;
}

static void req_free(mj_req_t *r)
{
    mj_free(r->body);
    mj_free(r);
}

/* Takes the request out of the list and gives it its answer. */
static void complete(mj_mtp_t *m, dc_t *d, mj_req_t **link, mj_rpc_t *res)
{
    mj_req_t *r = *link;
    *link = r->next;
    res->dc = d->dc;
    if (r->cb) r->cb(r->ctx, res);
    req_free(r);
}

/* Sends again the request with this msg_id, or every one of a container. */
static void resend(dc_t *d, int64_t msg_id, uint32_t delay_ms)
{
    for (mj_req_t *r = d->reqs; r; r = r->next)
        if (r->msg_id == msg_id || (r->container && r->container == msg_id)) {
            r->msg_id = 0;
            r->container = 0;
            r->not_before_ms = delay_ms ? mj_now_ms() + delay_ms : 0;
        }
}

static void handle_body(mj_mtp_t *m, dc_t *d, int64_t msg_id, const uint8_t *p, size_t n, int depth);

static void on_rpc_result(mj_mtp_t *m, dc_t *d, const uint8_t *p, size_t n)
{
    if (n < 12) return;
    int64_t req_id = rd64(p);
    mj_req_t **link = NULL;
    mj_req_t *r = find_req(d, req_id, &link);
    if (!r) {
        mj_log("mtp: dc%d: an answer to nothing we asked (%08x)", d->dc, (unsigned)rd32(p + 8));
        return;
    }
    mj_arena_t a = {0};
    mj_rpc_t res = {0};
    res.arena = &a;
    if (!tl_read(&a, p + 8, n - 8, &res.val, NULL)) {
        res.code = -1;
        res.msg = "UNREADABLE_ANSWER";
        complete(m, d, link, &res);
        mj_arena_free(&a);
        return;
    }
    tl_obj_t *o = res.val.kind == TLK_OBJ ? res.val.u.o : NULL;
    if (o && o->id == TLC_rpc_error) {
        res.code = tl_i(o, TLN_error_code);
        res.msg = tl_s(o, TLN_error_message);
        int w = mj_err_num(res.msg, "FLOOD_WAIT_");
        if (w < 0) w = mj_err_num(res.msg, "FLOOD_PREMIUM_WAIT_");
        if (res.code == 420 && w >= 0 && w <= 60 && r->tries < 4) {
            mj_log("mtp: dc%d: flood wait %d s, then again", d->dc, w);
            r->msg_id = 0;
            r->not_before_ms = mj_now_ms() + (uint32_t)w * 1000 + 300;
            mj_arena_free(&a);
            return;
        }
        if (res.code >= 500 && r->tries < 3) {
            r->msg_id = 0;
            r->not_before_ms = mj_now_ms() + 1500;
            mj_arena_free(&a);
            return;
        }
        if (res.code != 303 && res.code != 401 && strcmp(res.msg, "SESSION_PASSWORD_NEEDED"))
            mj_log("mtp: dc%d: rpc error %d %s", d->dc, res.code, res.msg);
    }
    complete(m, d, link, &res);
    mj_arena_free(&a);
}

static bool is_updates(uint32_t id)
{
    return id == TLC_updatesTooLong || id == TLC_updateShortMessage || id == TLC_updateShortChatMessage ||
           id == TLC_updateShort || id == TLC_updatesCombined || id == TLC_updates || id == TLC_updateShortSentMessage;
}

static void handle_body(mj_mtp_t *m, dc_t *d, int64_t msg_id, const uint8_t *p, size_t n, int depth)
{
    if (n < 4 || depth > 3) return;
    uint32_t id = rd32(p);
    if (id == TL_CONTAINER) {
        if (n < 8) return;
        uint32_t cnt = rd32(p + 4);
        size_t off = 8;
        for (uint32_t i = 0; i < cnt && off + 16 <= n; i++) {
            int64_t mid = rd64(p + off);
            uint32_t seqno = rd32(p + off + 8);
            uint32_t len = rd32(p + off + 12);
            off += 16;
            if (off + len > n) break;
            if ((seqno & 1) && d->nacks < 64) {
                if (!d->nacks) d->ack_ms = mj_now_ms();
                d->acks[d->nacks++] = mid;
            }
            handle_body(m, d, mid, p + off, len, depth + 1);
            off += len;
        }
        return;
    }
    if (id == TL_RPC_RESULT) {
        on_rpc_result(m, d, p + 4, n - 4);
        return;
    }
    mj_arena_t a = {0};
    if (id == TL_GZIP_PACKED) {
        /* unpacked into the arena and handled again */
        tl_val_t v;
        size_t on = 0;
        uint32_t len = 0;
        /* the packed string starts after the id */
        const uint8_t *s = p + 4;
        if (n < 8) return;
        size_t head = 1;
        len = s[0];
        if (len == 254) {
            len = (uint32_t)s[1] | (uint32_t)s[2] << 8 | (uint32_t)s[3] << 16;
            head = 4;
        }
        if (4 + head + len > n) return;
        uint8_t *u = tl_gunzip(&a, s + head, len, &on);
        (void)v;
        if (u) handle_body(m, d, msg_id, u, on, depth + 1);
        mj_arena_free(&a);
        return;
    }
    tl_val_t v;
    if (!tl_read(&a, p, n, &v, NULL) || v.kind != TLK_OBJ) {
        mj_arena_free(&a);
        return;
    }
    tl_obj_t *o = v.u.o;
    switch (o->id) {
    case TLC_msgs_ack: {
        tl_vec_t *ids = tl_v(o, TLN_msg_ids);
        for (uint32_t i = 0; i < tl_vn(ids); i++) {
            mj_req_t *r = find_req(d, tl_vl(ids, i), NULL);
            if (r) r->acked = true;
        }
        break;
    }
    case TLC_bad_server_salt:
        d->salt = tl_l(o, TLN_new_server_salt);
        resend(d, tl_l(o, TLN_bad_msg_id), 0);
        break;
    case TLC_bad_msg_notification: {
        int code = tl_i(o, TLN_error_code);
        int64_t bad = tl_l(o, TLN_bad_msg_id);
        mj_log("mtp: dc%d: bad_msg_notification %d", d->dc, code);
        if (code == 16 || code == 17) {
            /* our clock is off: the server's is in its own msg_id */
            learn_time(m, (int64_t)((uint64_t)msg_id >> 32));
            m->last_msg_id = 0;
        } else if (code == 32 || code == 33) {
            new_session(m, d);
            for (mj_req_t *r = d->reqs; r; r = r->next) r->msg_id = 0;
        }
        resend(d, bad, 0);
        break;
    }
    case TLC_new_session_created:
        d->salt = tl_l(o, TLN_server_salt);
        if (m->cfg.on_new_session) m->cfg.on_new_session(m->cfg.ud, d->dc);
        break;
    case TLC_pong: {
        mj_req_t **link = NULL;
        mj_req_t *r = find_req(d, tl_l(o, TLN_msg_id), &link);
        if (r) {
            mj_rpc_t res = {0};
            res.arena = &a;
            res.val = v;
            complete(m, d, link, &res);
        }
        break;
    }
    case TLC_msg_detailed_info:
    case TLC_msg_new_detailed_info:
        if (d->nacks < 64) {
            if (!d->nacks) d->ack_ms = mj_now_ms();
            d->acks[d->nacks++] = tl_l(o, TLN_answer_msg_id);
        }
        break;
    case TLC_future_salts:
    case TLC_msgs_state_info:
    case TLC_msgs_all_info:
    case TLC_destroy_session_ok:
    case TLC_destroy_session_none:
        break;
    default:
        if (is_updates(o->id)) {
            if (m->cfg.on_updates) m->cfg.on_updates(m->cfg.ud, d->dc, o, &a);
        } else {
            mj_log("mtp: dc%d: not handled: %s", d->dc, tl_name(o->id));
        }
        break;
    }
    mj_arena_free(&a);
}

/* A sealed packet from the server. */
static void on_packet(mj_mtp_t *m, dc_t *d, uint8_t *p, size_t n)
{
    if (n == 4) {
        int32_t e = (int32_t)rd32(p);
        mj_log("mtp: dc%d: transport error %d", d->dc, (int)e);
        if (e == -404) {
            /* the server does not know our key: a new one */
            d->has_key = false;
            if (m->cfg.on_key) m->cfg.on_key(m->cfg.ud, d->dc, NULL);
            new_session(m, d);
        }
        dc_close(m, d);
        return;
    }
    if (n < 24 + 32 || (n - 24) % 16 || memcmp(p, d->key_id, 8)) {
        mj_log("mtp: dc%d: a packet that is not ours (%u bytes)", d->dc, (unsigned)n);
        return;
    }
    uint8_t key[32], iv[32], big[32];
    derive(d->key, p + 8, 8, key, iv);
    uint8_t *pt = p + 24;
    size_t pn = n - 24;
    mj_ige_decrypt(key, iv, pt, pn);
    mj_sha256_2(d->key + 96, 32, pt, pn, big);
    if (memcmp(big + 8, p + 8, 16)) {
        mj_log("mtp: dc%d: msg_key does not match", d->dc);
        return;
    }
    if (rd64(pt + 8) != d->session_id) {
        mj_log("mtp: dc%d: a message of another session", d->dc);
        return;
    }
    int64_t msg_id = rd64(pt + 16);
    uint32_t seqno = rd32(pt + 24);
    uint32_t len = rd32(pt + 28);
    if (len > pn - 32 || pn - 32 - len < 12 || pn - 32 - len > 1024) {
        mj_log("mtp: dc%d: a message with a bad length", d->dc);
        return;
    }
    if ((seqno & 1) && d->nacks < 64) {
        if (!d->nacks) d->ack_ms = mj_now_ms();
        d->acks[d->nacks++] = msg_id;
    }
    handle_body(m, d, msg_id, pt + 32, len, 0);
}

/* ------------------------------------------------------------- the loop */

static dc_t *dcp(mj_mtp_t *m, int dc)
{
    if (dc < 1 || dc > MJ_DC_MAX) return NULL;
    return &m->dc[dc];
}

mj_mtp_t *mj_mtp_new(const mj_mtp_cfg_t *cfg)
{
    mj_mtp_t *m = (mj_mtp_t *)mj_calloc(sizeof *m);
    if (!m) return NULL;
    m->cfg = *cfg;
    snprintf(m->device, sizeof m->device, "%s", cfg->device_model ? cfg->device_model : "P4OS");
    snprintf(m->system, sizeof m->system, "%s", cfg->system_version ? cfg->system_version : "P4OS");
    snprintf(m->app_version, sizeof m->app_version, "%s", cfg->app_version ? cfg->app_version : "1.0");
    snprintf(m->lang, sizeof m->lang, "%s", cfg->lang_code ? cfg->lang_code : "en");
    if (cfg->host && cfg->host[0]) {
        snprintf(m->host, sizeof m->host, "%s", cfg->host);
        if (cfg->rsa_n) {
            memcpy(m->rsa_n, cfg->rsa_n, 256);
            m->fp_own = fingerprint(m->rsa_n);
        }
    }
    m->cfg.host = NULL;
    m->cfg.rsa_n = NULL;
    m->fp_tg = fingerprint(TG_RSA_N);
    m->clock0 = mj_unixtime();
    m->ms0 = mj_now_ms();
    for (int i = 1; i <= MJ_DC_MAX; i++) {
        m->dc[i].dc = i;
        new_session(m, &m->dc[i]);
    }
    return m;
}

void mj_mtp_free(mj_mtp_t *m)
{
    if (!m) return;
    for (int i = 1; i <= MJ_DC_MAX; i++) {
        dc_t *d = &m->dc[i];
        if (d->sock > 0) mj_tcp_close(d->sock);
        d->sock = 0;
        while (d->reqs) {
            mj_req_t **l = &d->reqs;
            mj_rpc_t res = {0};
            res.code = -1;
            res.msg = "CLOSED";
            complete(m, d, l, &res);
        }
        mj_free(d->rx);
        memset(d->key, 0, sizeof d->key);
    }
    mj_free(m);
}

void mj_mtp_set_api_id(mj_mtp_t *m, int32_t api_id)
{
    if (m->cfg.api_id != api_id) {
        m->cfg.api_id = api_id;
        /* the next request of every session says it again */
        for (int i = 1; i <= MJ_DC_MAX; i++) m->dc[i].inited = false;
    }
}

void mj_mtp_set_key(mj_mtp_t *m, int dc, const uint8_t *key)
{
    dc_t *d = dcp(m, dc);
    if (!d) return;
    memcpy(d->key, key, 256);
    uint8_t h[20];
    mj_sha1(key, 256, h);
    memcpy(d->key_id, h + 12, 8);
    d->has_key = true;
    d->salt = 0;                /* the server corrects it with bad_server_salt */
}

bool mj_mtp_has_key(mj_mtp_t *m, int dc)
{
    dc_t *d = dcp(m, dc);
    return d && d->has_key;
}

void mj_mtp_drop_key(mj_mtp_t *m, int dc)
{
    dc_t *d = dcp(m, dc);
    if (!d) return;
    d->has_key = false;
    memset(d->key, 0, sizeof d->key);
    new_session(m, d);
    dc_close(m, d);
}

static bool queue(mj_mtp_t *m, int dc, uint8_t *body, size_t n, mj_rpc_cb_t cb, void *ctx, bool ping)
{
    dc_t *d = dcp(m, dc);
    mj_req_t *r = d ? (mj_req_t *)mj_calloc(sizeof *r) : NULL;
    if (!r) {
        mj_free(body);
        return false;
    }
    r->body = body;
    r->n = n;
    r->cb = cb;
    r->ctx = ctx;
    r->is_ping = ping;
    mj_req_t **l = &d->reqs;
    while (*l) l = &(*l)->next;
    *l = r;
    return true;
}

bool mj_mtp_call(mj_mtp_t *m, int dc, const tl_obj_t *q, mj_rpc_cb_t cb, void *ctx)
{
    mj_buf_t b = {0};
    if (!tl_write(&b, q) || b.oom) {
        mj_buf_free(&b);
        return false;
    }
    return queue(m, dc, b.p, b.n, cb, ctx, false);
}

bool mj_mtp_call_raw(mj_mtp_t *m, int dc, const uint8_t *body, size_t n, mj_rpc_cb_t cb, void *ctx)
{
    uint8_t *c = (uint8_t *)mj_malloc(n);
    if (!c) return false;
    memcpy(c, body, n);
    return queue(m, dc, c, n, cb, ctx, false);
}

int mj_mtp_pending(mj_mtp_t *m, int dc)
{
    dc_t *d = dcp(m, dc);
    int n = 0;
    if (d)
        for (mj_req_t *r = d->reqs; r; r = r->next)
            if (!r->is_ping) n++;
    return n;
}

void mj_mtp_cancel(mj_mtp_t *m, void *ctx)
{
    for (int i = 1; i <= MJ_DC_MAX; i++) {
        dc_t *d = &m->dc[i];
        mj_req_t **l = &d->reqs;
        while (*l) {
            if ((*l)->ctx == ctx && (*l)->cb) {
                mj_rpc_t res = {0};
                res.code = -1;
                res.msg = "CANCELLED";
                complete(m, d, l, &res);
            } else {
                l = &(*l)->next;
            }
        }
    }
}

void mj_mtp_set_home(mj_mtp_t *m, int dc)
{
    m->home = dc;
}

int mj_mtp_state(mj_mtp_t *m, int dc)
{
    dc_t *d = dcp(m, dc);
    return d ? d->state : MJ_NET_DOWN;
}

void mj_mtp_disconnect(mj_mtp_t *m)
{
    for (int i = 1; i <= MJ_DC_MAX; i++)
        if (m->dc[i].sock > 0) dc_close(m, &m->dc[i]);
}

static void ping(mj_mtp_t *m, dc_t *d)
{
    mj_arena_t a = {0};
    tl_obj_t *q = tl_new(&a, TLC_ping_delay_disconnect);
    int64_t id;
    mj_random(&id, 8);
    tl_set_l(q, TLN_ping_id, id);
    tl_set_i(q, TLN_disconnect_delay, 75);
    mj_buf_t b = {0};
    if (tl_write(&b, q) && !b.oom) queue(m, d->dc, b.p, b.n, NULL, NULL, true);
    else mj_buf_free(&b);
    mj_arena_free(&a);
    d->last_ping_ms = mj_now_ms();
}

static void timers(mj_mtp_t *m, dc_t *d)
{
    uint32_t now = mj_now_ms();
    mj_req_t **l = &d->reqs;
    while (*l) {
        mj_req_t *r = *l;
        if (r->msg_id && !r->acked && (uint32_t)(now - r->sent_ms) > RESEND_MS) {
            if (r->is_ping || r->tries >= 3) {
                if (r->is_ping) {
                    /* a ping nobody answered: the connection is dead */
                    mj_log("mtp: dc%d: no pong, reconnecting", d->dc);
                    *l = r->next;
                    req_free(r);
                    dc_close(m, d);
                    return;
                }
                mj_rpc_t res = {0};
                res.code = -1;
                res.msg = "TIMEOUT";
                complete(m, d, l, &res);
                continue;
            }
            r->msg_id = 0;
        } else if (r->msg_id && r->acked && (uint32_t)(now - r->sent_ms) > GIVEUP_MS) {
            mj_rpc_t res = {0};
            res.code = -1;
            res.msg = "TIMEOUT";
            complete(m, d, l, &res);
            continue;
        }
        l = &r->next;
    }
    if (d->dc == m->home && d->sock > 0 && (uint32_t)(now - d->last_ping_ms) > PING_MS) ping(m, d);
}

void mj_mtp_poll(mj_mtp_t *m, int wait_ms)
{
    uint32_t now = mj_now_ms();
    int live = 0;
    for (int i = 1; i <= MJ_DC_MAX; i++) {
        dc_t *d = &m->dc[i];
        bool wanted = d->reqs != NULL || i == m->home;
        if (!wanted) {
            if (d->sock > 0 && (uint32_t)(now - d->last_io_ms) > IDLE_CLOSE_MS) {
                mj_log("mtp: dc%d: idle, closed", i);
                dc_close(m, d);
            }
            continue;
        }
        if (d->sock <= 0) {
            if (d->retry_at_ms && (int32_t)(now - d->retry_at_ms) < 0) continue;
            if (!dc_connect(m, d)) continue;
        }
        if (!d->has_key) {
            if (!make_key(m, d)) continue;
            new_session(m, d);
        }
        if (d->state != MJ_NET_UP) {
            d->fails = 0;
            d->retry_at_ms = 0;
            dc_state(m, d, MJ_NET_UP);
        }
        flush(m, d);
        live++;
    }
    /* read: the time is shared among the open connections */
    int each = live ? wait_ms / live : wait_ms;
    if (each < 5) each = 5;
    bool any = false;
    for (int i = 1; i <= MJ_DC_MAX; i++) {
        dc_t *d = &m->dc[i];
        if (d->sock <= 0 || !d->has_key) continue;
        any = true;
        if (rx_fill(m, d, each) < 0) continue;
        for (;;) {
            long n = rx_packet(d);
            if (n < 0) {
                mj_log("mtp: dc%d: a packet too long, reconnecting", i);
                dc_close(m, d);
                break;
            }
            if (n == 0) break;
            /* the packet is handled in place, then dropped */
            on_packet(m, d, d->rx + 4, (size_t)n);
            if (d->sock <= 0) break;
            rx_drop(d, (size_t)n);
        }
        if (d->sock > 0) {
            timers(m, d);
            flush(m, d);
        }
    }
    if (!any && wait_ms > 0) {
        /* nothing open: the loop still waits, not spins */
        uint32_t t0 = mj_now_ms();
        while ((uint32_t)(mj_now_ms() - t0) < (uint32_t)wait_ms) {
            if (m->cfg.stop && *m->cfg.stop) break;
            mj_sleep_ms(20);
        }
    }
}
