/*
 * P4OS - an SSH client in the firmware and SFTP over it (aos_sshc.h).
 *
 * The same transport the server speaks (aos_ssh.c), from the other end, and
 * written to block: connect, exchange keys, check the server's key against
 * the one that was kept, log in with the board's Ed25519 key, open one
 * session channel, ask for the "sftp" subsystem, and then one SFTP request
 * at a time, each waited for.
 *
 * Every wait has an end: STEP_MS for a packet that is due, and 'cancel' is
 * looked at ten times a second. A server that goes quiet fails the call; it
 * does not hang the thread.
 *
 * Memory (PSRAM first): a connection holds three packet buffers of ~35 KB
 * and the channel's 64 KB of received data, all PSRAM, and nothing in
 * internal RAM but the caller's stack. The frames here are small on
 * purpose: the paths and the packets are in those buffers.
 */
#include "aos_sshc.h"
#include "aos_ssh_int.h"
#include "aos_hal.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "mbedtls/base64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void *big_calloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}
#else
static void *big_calloc(size_t n) { return calloc(1, n); }
#endif

#define TAG         "sshc"
#define PKT_MAX     35000
#define BUF_CAP     (PKT_MAX + 4 + 16 + 64)
#define WIN         (64 * 1024)     /* what the server may send before we ask for more: SFTP's replies are small */
#define MAXPKT      32768
#define CONNECT_MS  8000
#define STEP_MS     20000           /* a packet that is due */
#define FX_CHUNK    16384           /* a file goes up in pieces of this */
#define FX_MAX      (34 * 1024)     /* an SFTP packet of ours, or of the server's, whole */

static const char V_C[] = "SSH-2.0-P4OS_sftp";
static const char KEX_ALGS[] = "curve25519-sha256,curve25519-sha256@libssh.org,ext-info-c,kex-strict-c-v00@openssh.com";
static const char KEX_AGAIN[] = "curve25519-sha256,curve25519-sha256@libssh.org";
static const char HOSTKEY_ALGS[] = "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256";
/* the P4's AES engine first; ChaCha20 for a server that has no AES-GCM */
static const char CIPHERS[] = "aes256-gcm@openssh.com,aes128-gcm@openssh.com,chacha20-poly1305@openssh.com";

enum {
    MSG_DISCONNECT = 1, MSG_IGNORE, MSG_UNIMPLEMENTED, MSG_DEBUG, MSG_SERVICE_REQUEST, MSG_SERVICE_ACCEPT,
    MSG_EXT_INFO,
    MSG_KEXINIT = 20, MSG_NEWKEYS,
    MSG_KEX_ECDH_INIT = 30, MSG_KEX_ECDH_REPLY,
    MSG_USERAUTH_REQUEST = 50, MSG_USERAUTH_FAILURE, MSG_USERAUTH_SUCCESS, MSG_USERAUTH_BANNER,
    MSG_GLOBAL_REQUEST = 80, MSG_REQUEST_SUCCESS, MSG_REQUEST_FAILURE,
    MSG_CHANNEL_OPEN = 90, MSG_CHANNEL_OPEN_CONFIRMATION, MSG_CHANNEL_OPEN_FAILURE, MSG_CHANNEL_WINDOW_ADJUST,
    MSG_CHANNEL_DATA, MSG_CHANNEL_EXTENDED_DATA, MSG_CHANNEL_EOF, MSG_CHANNEL_CLOSE, MSG_CHANNEL_REQUEST,
    MSG_CHANNEL_SUCCESS, MSG_CHANNEL_FAILURE,
};
enum {
    FXP_INIT = 1, FXP_VERSION, FXP_OPEN, FXP_CLOSE, FXP_READ, FXP_WRITE, FXP_LSTAT, FXP_FSTAT, FXP_SETSTAT, FXP_FSETSTAT,
    FXP_OPENDIR, FXP_READDIR, FXP_REMOVE, FXP_MKDIR, FXP_RMDIR, FXP_REALPATH, FXP_STAT, FXP_RENAME,
    FXP_STATUS = 101, FXP_HANDLE, FXP_DATA, FXP_NAME, FXP_ATTRS, FXP_EXTENDED = 200,
};
enum { FX_OK = 0, FX_EOF, FX_NO_SUCH_FILE };
enum { ATTR_SIZE = 1, ATTR_UIDGID = 2, ATTR_PERMISSIONS = 4, ATTR_ACMODTIME = 8 };

struct aos_sshc {
    int fd;
    volatile bool *cancel;
    char user[64];
    int err;
    char why[220];

    uint8_t *in, *out, *pkt;
    size_t in_len, pkt_len;
    char v_s[256];
    uint8_t *i_c, *i_s;
    size_t i_c_len, i_s_len;
    bool first_kex, strict, have_sid, ignore_next, live;
    uint8_t sid[32];
    char hk_alg[24];
    char *hostkey;              /* base64 of the blob the server showed */
    char fp[80];
    ssh_cipher_t *rx, *tx;
    uint32_t seq_rx, seq_tx;

    /* the one channel */
    bool ch_open, ch_eof, ch_closed;
    uint32_t peer, peer_win, peer_max, consumed;
    uint8_t *rx_q;              /* channel data not yet read: WIN at most */
    size_t rx_len;

    /* SFTP */
    uint8_t *fx;                /* a request being written, then its reply */
    size_t fx_len;
    uint32_t fx_id;
    bool posix_rename;
};

/* ---- how it ends ---- */

static int fail(aos_sshc_t *c, int err, const char *fmt, const char *arg)
{
    if (!c->err) {
        c->err = err;
        snprintf(c->why, sizeof c->why, fmt, arg ? arg : "");
        aos_hal_log(TAG, "%s", c->why);
    }
    return err;
}

static bool stopped(aos_sshc_t *c)
{
    if (c->cancel && *c->cancel) fail(c, AOS_SSHC_E_STOPPED, "stopped", NULL);
    return c->err != 0;
}

/* ---- packets ---- */

static wbuf_t pk_begin(aos_sshc_t *c, uint8_t type)
{
    wbuf_t b = { c->out + 5, 0, PKT_MAX - 64, false };
    wb_byte(&b, type);
    return b;
}

static bool pk_send(aos_sshc_t *c, wbuf_t *b)
{
    if (c->err) return false;
    if (b->bad) { fail(c, AOS_SSHC_E_PROTOCOL, "a message did not fit", NULL); return false; }
    size_t block = c->tx ? (size_t)ssh_cipher_block(c->tx) : 8, head = c->tx ? 1 : 5;
    size_t pad = block - (head + b->len) % block;
    if (pad < 4) pad += block;
    size_t L = 1 + b->len + pad;
    uint8_t *o = c->out;
    o[0] = (uint8_t)(L >> 24);
    o[1] = (uint8_t)(L >> 16);
    o[2] = (uint8_t)(L >> 8);
    o[3] = (uint8_t)L;
    o[4] = (uint8_t)pad;
    ssh_random(o + 5 + b->len, pad);
    size_t total = 4 + L;
    if (c->tx) {
        if (!ssh_cipher_seal(c->tx, o, L, c->seq_tx)) { fail(c, AOS_SSHC_E_PROTOCOL, "a packet could not be sealed", NULL); return false; }
        total += 16;
    }
    c->seq_tx++;
    if (aos_hal_tcp_send(c->fd, o, (int)total, STEP_MS) != (int)total) {
        fail(c, AOS_SSHC_E_IO, "the connection was cut", NULL);
        return false;
    }
    return true;
}

/* A whole packet into c->pkt: 1, 0 if none came in timeout_ms, -1 when the
 * connection is over (fail() said why). */
static int pk_recv(aos_sshc_t *c, int timeout_ms)
{
    uint64_t until = aos_hal_uptime_ms() + (uint64_t)timeout_ms;
    for (;;) {
        if (c->in_len >= 4) {
            uint8_t *q = c->in;
            bool enc = c->rx != NULL;
            uint32_t L = enc ? ssh_cipher_length(c->rx, q, c->seq_rx)
                             : (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3];
            uint32_t blk = enc ? (uint32_t)ssh_cipher_block(c->rx) : 8;
            if (L < (enc ? blk : 12u) || L > PKT_MAX || (enc ? L % blk : (L + 4) % 8)) {
                fail(c, AOS_SSHC_E_PROTOCOL, "the server sent something that is not SSH", NULL);
                return -1;
            }
            size_t total = 4 + L + (enc ? 16 : 0);
            if (c->in_len >= total) {
                if (enc && !ssh_cipher_open(c->rx, q, L, c->seq_rx)) {
                    fail(c, AOS_SSHC_E_PROTOCOL, "a packet came damaged", NULL);
                    return -1;
                }
                uint8_t pad = q[4];
                if (pad < 4 || (size_t)pad + 1 >= L) {
                    fail(c, AOS_SSHC_E_PROTOCOL, "the server sent something that is not SSH", NULL);
                    return -1;
                }
                c->pkt_len = L - 1 - pad;
                memcpy(c->pkt, q + 5, c->pkt_len);
                c->seq_rx++;
                memmove(c->in, c->in + total, c->in_len - total);
                c->in_len -= total;
                return 1;
            }
        }
        if (stopped(c)) return -1;
        uint64_t now = aos_hal_uptime_ms();
        if (now >= until) return 0;
        int wait = (int)(until - now);
        int k = aos_hal_tcp_recv(c->fd, c->in + c->in_len, (int)(BUF_CAP - c->in_len), wait > 100 ? 100 : wait);
        if (k < 0) {
            fail(c, AOS_SSHC_E_IO, "the server closed the connection", NULL);
            return -1;
        }
        c->in_len += (size_t)k;
    }
}

static bool send_kexinit(aos_sshc_t *c);
static bool kex(aos_sshc_t *c);

/* The next packet that matters to whoever waits: its type, 0 when none came
 * in timeout_ms, -1 when the connection is over. What is the transport's or
 * the channel's own business is dealt with here: a new key exchange, the
 * windows, the data that came. */
static int next(aos_sshc_t *c, int timeout_ms)
{
    for (;;) {
        int r = pk_recv(c, timeout_ms);
        if (r <= 0) return r;
        uint8_t t = c->pkt[0];
        rbuf_t rb = { c->pkt, c->pkt_len, 1, false };
        switch (t) {
        case MSG_IGNORE: case MSG_DEBUG: case MSG_UNIMPLEMENTED: case MSG_EXT_INFO:
            /* strict KEX: nothing but the exchange before the first NEWKEYS */
            if (c->first_kex && c->strict && !c->have_sid) {
                fail(c, AOS_SSHC_E_PROTOCOL, "the server sent something that is not SSH", NULL);
                return -1;
            }
            continue;
        case MSG_DISCONNECT: {
            char why[100];
            rb_u32(&rb);
            rb_cstr(&rb, why, sizeof why);
            fail(c, AOS_SSHC_E_IO, "the server closed: %.80s", why);
            return -1;
        }
        case MSG_USERAUTH_BANNER:
            continue;
        case MSG_GLOBAL_REQUEST: {
            char name[48];
            rb_cstr(&rb, name, sizeof name);
            if (rb_bool(&rb) && c->live) {
                wbuf_t b = pk_begin(c, MSG_REQUEST_FAILURE);
                pk_send(c, &b);
            }
            continue;
        }
        case MSG_KEXINIT:
            if (!c->live) return t;             /* the first one: the caller is waiting for it */
            if (!send_kexinit(c) || !kex(c)) return -1;
            continue;
        case MSG_CHANNEL_WINDOW_ADJUST: {
            rb_u32(&rb);
            uint64_t w = (uint64_t)c->peer_win + rb_u32(&rb);
            c->peer_win = w > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)w;
            return t;
        }
        case MSG_CHANNEL_DATA:
        case MSG_CHANNEL_EXTENDED_DATA: {
            rb_u32(&rb);
            if (t == MSG_CHANNEL_EXTENDED_DATA) rb_u32(&rb);
            size_t n;
            const uint8_t *d = rb_string(&rb, &n);
            if (rb.bad || c->rx_len + n > WIN) {
                fail(c, AOS_SSHC_E_PROTOCOL, "the server sent more than it was told it could", NULL);
                return -1;
            }
            if (t == MSG_CHANNEL_EXTENDED_DATA) { c->consumed += (uint32_t)n; continue; }     /* its stderr: not kept */
            memcpy(c->rx_q + c->rx_len, d, n);
            c->rx_len += n;
            return t;
        }
        case MSG_CHANNEL_EOF:   c->ch_eof = true; return t;
        case MSG_CHANNEL_CLOSE: c->ch_closed = true; return t;
        case MSG_CHANNEL_REQUEST:
            continue;                           /* exit-status and the like */
        default:
            return t;
        }
    }
}

/* ---- key exchange ---- */

static bool send_kexinit(aos_sshc_t *c)
{
    wbuf_t b = pk_begin(c, MSG_KEXINIT);
    uint8_t cookie[16];
    ssh_random(cookie, sizeof cookie);
    wb_bytes(&b, cookie, 16);
    wb_cstr(&b, c->first_kex ? KEX_ALGS : KEX_AGAIN);
    wb_cstr(&b, HOSTKEY_ALGS);
    wb_cstr(&b, CIPHERS);
    wb_cstr(&b, CIPHERS);
    wb_cstr(&b, "hmac-sha2-256");
    wb_cstr(&b, "hmac-sha2-256");
    wb_cstr(&b, "none");
    wb_cstr(&b, "none");
    wb_cstr(&b, "");
    wb_cstr(&b, "");
    wb_byte(&b, 0);
    wb_u32(&b, 0);
    if (b.bad) return false;
    free(c->i_c);
    c->i_c = malloc(b.len);
    if (!c->i_c) { fail(c, AOS_SSHC_E_MEMORY, "no memory", NULL); return false; }
    memcpy(c->i_c, b.p, b.len);
    c->i_c_len = b.len;
    return pk_send(c, &b);
}

/* the first of ours (in our order) that the server has */
static bool pick(const uint8_t *theirs, size_t n, const char *ours, char *out, size_t on)
{
    const char *p = ours;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l < on) {
            memcpy(out, p, l);
            out[l] = 0;
            if (namelist_has(theirs, n, out)) return true;
        }
        if (!e) break;
        p = e + 1;
    }
    return false;
}

static void hash_string(ssh_sha256_ctx_t *x, const void *d, size_t n)
{
    uint8_t l[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    ssh_sha256_add(x, l, 4);
    ssh_sha256_add(x, d, n);
}

/* HASH(K || H || letter || session_id), lengthened as RFC 4253, 7.2 says */
static void derive(aos_sshc_t *c, const uint8_t *kmp, size_t kn, const uint8_t h[32], char letter, uint8_t out[64])
{
    ssh_sha256_ctx_t *x = ssh_sha256_new();
    ssh_sha256_add(x, kmp, kn);
    ssh_sha256_add(x, h, 32);
    ssh_sha256_add(x, &letter, 1);
    ssh_sha256_add(x, c->sid, 32);
    ssh_sha256_end(x, out);
    x = ssh_sha256_new();
    ssh_sha256_add(x, kmp, kn);
    ssh_sha256_add(x, h, 32);
    ssh_sha256_add(x, out, 32);
    ssh_sha256_end(x, out + 32);
}

static char *b64_of(const uint8_t *d, size_t n)
{
    size_t cap = (n + 2) / 3 * 4 + 1, got = 0;
    char *out = malloc(cap);
    if (!out || mbedtls_base64_encode((unsigned char *)out, cap, &got, d, n) != 0) { free(out); return NULL; }
    out[got] = 0;
    return out;
}

static void fingerprint(const uint8_t *blob, size_t n, char out[80])
{
    char kind[24] = "", b64[48] = "";
    uint8_t h[32];
    size_t got = 0;
    ssh_key_describe(blob, n, kind, sizeof kind);
    ssh_sha256(blob, n, h);
    mbedtls_base64_encode((unsigned char *)b64, sizeof b64, &got, h, 32);
    b64[got] = 0;
    while (got && b64[got - 1] == '=') b64[--got] = 0;     /* as ssh-keygen prints it */
    snprintf(out, 80, "%.20s SHA256:%s", kind, b64);
}

/* The exchange, from the server's KEXINIT in c->pkt (ours already sent).
 * 'kept' is the key the caller pinned, or NULL. */
static bool kex_with(aos_sshc_t *c, const char *kept)
{
    free(c->i_s);
    c->i_s = malloc(c->pkt_len);
    if (!c->i_s) { fail(c, AOS_SSHC_E_MEMORY, "no memory", NULL); return false; }
    memcpy(c->i_s, c->pkt, c->pkt_len);
    c->i_s_len = c->pkt_len;
    rbuf_t r = { c->i_s, c->i_s_len, 1 + 16, false };
    const uint8_t *l[10];
    size_t ln[10];
    for (int i = 0; i < 10; i++) l[i] = rb_string(&r, &ln[i]);
    bool follows = rb_bool(&r);
    char kexn[40], enc_c[40], enc_s[40], comp[8];
    if (r.bad || !pick(l[0], ln[0], KEX_AGAIN, kexn, sizeof kexn)) {
        fail(c, AOS_SSHC_E_PROTOCOL, "the server has no curve25519: no key exchange in common", NULL);
        return false;
    }
    if (!pick(l[1], ln[1], HOSTKEY_ALGS, c->hk_alg, sizeof c->hk_alg)) {
        fail(c, AOS_SSHC_E_PROTOCOL, "the server has no kind of host key the board knows", NULL);
        return false;
    }
    if (!pick(l[2], ln[2], CIPHERS, enc_c, sizeof enc_c) || !pick(l[3], ln[3], CIPHERS, enc_s, sizeof enc_s)) {
        fail(c, AOS_SSHC_E_PROTOCOL, "the server has neither AES-GCM nor ChaCha20: no cipher in common", NULL);
        return false;
    }
    if (!pick(l[6], ln[6], "none", comp, sizeof comp) || !pick(l[7], ln[7], "none", comp, sizeof comp)) {
        fail(c, AOS_SSHC_E_PROTOCOL, "the server only takes compression", NULL);
        return false;
    }
    if (c->first_kex) c->strict = namelist_has(l[0], ln[0], "kex-strict-s-v00@openssh.com");
    c->ignore_next = follows && !(ln[0] >= strlen(kexn) && !memcmp(l[0], kexn, strlen(kexn)));

    uint8_t esk[32], epk[32];
    ssh_x25519_keypair(esk, epk);
    wbuf_t b = pk_begin(c, MSG_KEX_ECDH_INIT);
    wb_string(&b, epk, 32);
    if (!pk_send(c, &b)) return false;

    /* c->live off while this runs: a KEXINIT now would be a server gone wrong */
    bool was_live = c->live;
    c->live = false;
    int t;
    for (;;) {
        t = next(c, STEP_MS);
        if (t <= 0) { if (!t) fail(c, AOS_SSHC_E_IO, "the server stopped answering", NULL); return false; }
        if (c->ignore_next && t >= 30 && t <= 49) { c->ignore_next = false; continue; }
        break;
    }
    if (t != MSG_KEX_ECDH_REPLY) {
        fail(c, AOS_SSHC_E_PROTOCOL, "the server sent something that is not SSH", NULL);
        return false;
    }
    r = (rbuf_t){ c->pkt, c->pkt_len, 1, false };
    size_t ksn, qsn, sgn;
    const uint8_t *ks = rb_string(&r, &ksn), *qs = rb_string(&r, &qsn), *sg = rb_string(&r, &sgn);
    uint8_t k[32];
    if (r.bad || qsn != 32 || !ssh_x25519(k, esk, qs)) {
        fail(c, AOS_SSHC_E_PROTOCOL, "the server sent something that is not SSH", NULL);
        return false;
    }
    crypto_wipe(esk, sizeof esk);
    uint8_t kmp[4 + 33];
    wbuf_t kb = { kmp, 0, sizeof kmp, false };
    wb_mpint(&kb, k, 32);
    crypto_wipe(k, sizeof k);

    uint8_t h[32];
    ssh_sha256_ctx_t *x = ssh_sha256_new();
    hash_string(x, V_C, strlen(V_C));
    hash_string(x, c->v_s, strlen(c->v_s));
    hash_string(x, c->i_c, c->i_c_len);
    hash_string(x, c->i_s, c->i_s_len);
    hash_string(x, ks, ksn);
    hash_string(x, epk, 32);
    hash_string(x, qs, 32);
    ssh_sha256_add(x, kmp, kb.len);
    ssh_sha256_end(x, h);

    if (!ssh_key_verify(c->hk_alg, ks, ksn, sg, sgn, h, 32)) {
        fail(c, AOS_SSHC_E_HOSTKEY, "the server's signature does not match its key", NULL);
        return false;
    }
    char *shown = b64_of(ks, ksn);
    if (!shown) { fail(c, AOS_SSHC_E_MEMORY, "no memory", NULL); return false; }
    if (c->first_kex) {
        c->hostkey = shown;
        fingerprint(ks, ksn, c->fp);
        /* before anything of ours is sent: the key that was kept, or none */
        if (kept && kept[0] && strcmp(kept, shown)) {
            fail(c, AOS_SSHC_E_HOSTKEY, "the server's key is not the one that was kept (now %.80s)", c->fp);
            return false;
        }
    } else {
        bool same = !strcmp(shown, c->hostkey);
        free(shown);
        if (!same) { fail(c, AOS_SSHC_E_HOSTKEY, "the server's key changed in the middle of the session", NULL); return false; }
    }
    if (!c->have_sid) {
        memcpy(c->sid, h, 32);
        c->have_sid = true;
    }
    uint8_t iv_c[64], iv_s[64], key_c[64], key_s[64];
    derive(c, kmp, kb.len, h, 'A', iv_c);
    derive(c, kmp, kb.len, h, 'B', iv_s);
    derive(c, kmp, kb.len, h, 'C', key_c);
    derive(c, kmp, kb.len, h, 'D', key_s);
    crypto_wipe(kmp, sizeof kmp);
    ssh_cipher_t *tx = ssh_cipher_new(enc_c, key_c, iv_c), *rx = ssh_cipher_new(enc_s, key_s, iv_s);
    crypto_wipe(key_c, sizeof key_c);
    crypto_wipe(key_s, sizeof key_s);
    if (!tx || !rx) {
        ssh_cipher_free(tx);
        ssh_cipher_free(rx);
        fail(c, AOS_SSHC_E_MEMORY, "no memory", NULL);
        return false;
    }

    b = pk_begin(c, MSG_NEWKEYS);
    if (!pk_send(c, &b)) { ssh_cipher_free(tx); ssh_cipher_free(rx); return false; }
    ssh_cipher_free(c->tx);
    c->tx = tx;
    if (c->strict) c->seq_tx = 0;

    t = next(c, STEP_MS);
    if (t != MSG_NEWKEYS) {
        ssh_cipher_free(rx);
        if (t >= 0) fail(c, t ? AOS_SSHC_E_PROTOCOL : AOS_SSHC_E_IO, "the server did not finish the key exchange", NULL);
        return false;
    }
    ssh_cipher_free(c->rx);
    c->rx = rx;
    if (c->strict) c->seq_rx = 0;
    c->first_kex = false;
    c->live = was_live;
    return true;
}

static bool kex(aos_sshc_t *c) { return kex_with(c, NULL); }

/* ---- logging in ---- */

static void auth_head(wbuf_t *b, aos_sshc_t *c, const char *method)
{
    wb_cstr(b, c->user);
    wb_cstr(b, "ssh-connection");
    wb_cstr(b, method);
}

/* 1 in, 0 refused (the methods it would take in 'methods'), -1 over */
static int auth_reply(aos_sshc_t *c, char *methods, size_t mn)
{
    int t = next(c, STEP_MS);
    if (t == MSG_USERAUTH_SUCCESS) return 1;
    if (t == MSG_USERAUTH_FAILURE) {
        rbuf_t r = { c->pkt, c->pkt_len, 1, false };
        rb_cstr(&r, methods, mn);
        return 0;
    }
    if (t >= 0) fail(c, t ? AOS_SSHC_E_PROTOCOL : AOS_SSHC_E_IO, "the server did not answer the login", NULL);
    return -1;
}

static bool login(aos_sshc_t *c, const uint8_t seed_in[32])
{
    wbuf_t b = pk_begin(c, MSG_SERVICE_REQUEST);
    wb_cstr(&b, "ssh-userauth");
    if (!pk_send(c, &b)) return false;
    int t = next(c, STEP_MS);
    if (t != MSG_SERVICE_ACCEPT) {
        if (t >= 0) fail(c, t ? AOS_SSHC_E_PROTOCOL : AOS_SSHC_E_IO, "the server did not take the login service", NULL);
        return false;
    }
    char methods[160] = "";
    b = pk_begin(c, MSG_USERAUTH_REQUEST);
    auth_head(&b, c, "none");
    if (!pk_send(c, &b)) return false;
    int r = auth_reply(c, methods, sizeof methods);
    if (r) return r > 0;                /* a server that asks for nothing: in */
    if (!namelist_has((const uint8_t *)methods, strlen(methods), "publickey")) {
        fail(c, AOS_SSHC_E_AUTH, "the server does not take keys (it offers: %.80s)", methods);
        return false;
    }

    uint8_t seed[32], sk[64], pub[32], sig[64], blob[4 + 11 + 4 + 32];
    memcpy(seed, seed_in, 32);
    crypto_ed25519_key_pair(sk, pub, seed);     /* wipes its copy of the seed */
    wbuf_t kb = { blob, 0, sizeof blob, false };
    wb_cstr(&kb, "ssh-ed25519");
    wb_string(&kb, pub, 32);
    /* what is signed (RFC 4252, 7): the session id, then the request. Built
     * in the reply buffer, which is idle now. */
    wbuf_t mb = { c->pkt, 0, 1024, false };
    wb_string(&mb, c->sid, 32);
    wb_byte(&mb, MSG_USERAUTH_REQUEST);
    auth_head(&mb, c, "publickey");
    wb_byte(&mb, 1);
    wb_cstr(&mb, "ssh-ed25519");
    wb_string(&mb, blob, kb.len);
    crypto_ed25519_sign(sig, sk, c->pkt, mb.len);
    crypto_wipe(sk, sizeof sk);
    b = pk_begin(c, MSG_USERAUTH_REQUEST);
    auth_head(&b, c, "publickey");
    wb_byte(&b, 1);
    wb_cstr(&b, "ssh-ed25519");
    wb_string(&b, blob, kb.len);
    wb_u32(&b, 4 + 11 + 4 + 64);
    wb_cstr(&b, "ssh-ed25519");
    wb_string(&b, sig, 64);
    if (!pk_send(c, &b)) return false;
    r = auth_reply(c, methods, sizeof methods);
    if (!r) fail(c, AOS_SSHC_E_AUTH, "the server does not let the board's key in for %.60s", c->user);
    return r > 0;
}

/* ---- the connection ---- */

static bool read_version(aos_sshc_t *c)
{
    for (int lines = 0; lines < 30;) {
        uint8_t *nl = memchr(c->in, '\n', c->in_len);
        if (nl) {
            size_t l = (size_t)(nl - c->in), ll = l && c->in[l - 1] == '\r' ? l - 1 : l;
            bool ver = ll >= 4 && !memcmp(c->in, "SSH-", 4);
            if (ver) {
                if (ll >= sizeof c->v_s || (memcmp(c->in, "SSH-2.0-", 8) && memcmp(c->in, "SSH-1.99-", 9))) return false;
                memcpy(c->v_s, c->in, ll);
                c->v_s[ll] = 0;
            }
            memmove(c->in, nl + 1, c->in_len - l - 1);
            c->in_len -= l + 1;
            if (ver) return true;
            lines++;
            continue;
        }
        if (c->in_len >= 255 || stopped(c)) return false;
        int k = aos_hal_tcp_recv(c->fd, c->in + c->in_len, (int)(BUF_CAP - c->in_len), CONNECT_MS);
        if (k <= 0) return false;
        c->in_len += (size_t)k;
    }
    return false;
}

aos_sshc_t *aos_sshc_open(const aos_sshc_opts_t *o, int *err, char *why, size_t why_len, char *seen)
{
    aos_sshc_t *c = big_calloc(sizeof *c);
    if (seen) seen[0] = 0;
    if (c) {
        c->in = big_calloc(BUF_CAP);
        c->out = big_calloc(BUF_CAP);
        c->pkt = big_calloc(BUF_CAP);
        c->rx_q = big_calloc(WIN);
        c->fx = big_calloc(FX_MAX);
    }
    if (!c || !c->in || !c->out || !c->pkt || !c->rx_q || !c->fx) {
        if (err) *err = AOS_SSHC_E_MEMORY;
        if (why) snprintf(why, why_len, "no memory");
        aos_sshc_close(c);
        return NULL;
    }
    c->cancel = o->cancel;
    c->first_kex = true;
    snprintf(c->user, sizeof c->user, "%s", o->user ? o->user : "");
    int port = o->port > 0 ? o->port : 22;
    if (!o->host || !o->host[0] || !c->user[0] || !o->seed) fail(c, AOS_SSHC_E_CONNECT, "no server, or no user, to connect to", NULL);
    else {
        c->fd = aos_hal_tcp_connect(o->host, port, CONNECT_MS);
        if (c->fd <= 0) fail(c, AOS_SSHC_E_CONNECT, c->fd == AOS_TCP_ERR_DNS ? "%.80s was not found" : "%.80s does not answer", o->host);
    }
    if (!c->err) {
        char vc[40];
        snprintf(vc, sizeof vc, "%s\r\n", V_C);
        if (aos_hal_tcp_send(c->fd, vc, (int)strlen(vc), CONNECT_MS) != (int)strlen(vc) || !read_version(c))
            fail(c, AOS_SSHC_E_PROTOCOL, "%.80s did not answer as an SSH server", o->host);
    }
    if (!c->err && send_kexinit(c)) {
        int t = next(c, STEP_MS);
        if (t != MSG_KEXINIT) { if (t >= 0) fail(c, t ? AOS_SSHC_E_PROTOCOL : AOS_SSHC_E_IO, "the server did not start the key exchange", NULL); }
        else if (kex_with(c, o->hostkey) && login(c, o->seed)) {
            c->live = true;
            aos_hal_log(TAG, "%s: in as %s (%s, %s)", o->host, c->user, c->v_s, c->fp);
        }
    }
    if (seen && c->hostkey) snprintf(seen, 600, "%s", c->hostkey);
    if (c->err) {
        if (err) *err = c->err;
        if (why) snprintf(why, why_len, "%s", c->why);
        aos_sshc_close(c);
        return NULL;
    }
    if (err) *err = AOS_SSHC_OK;
    return c;
}

const char *aos_sshc_hostkey(const aos_sshc_t *c) { return c && c->hostkey ? c->hostkey : ""; }
const char *aos_sshc_fingerprint(const aos_sshc_t *c) { return c ? c->fp : ""; }
const char *aos_sshc_why(const aos_sshc_t *c) { return c ? c->why : ""; }

bool aos_sshc_fingerprint_of(const char *hostkey, char *out, size_t n)
{
    uint8_t blob[600];
    char fp[80];
    size_t len = 0;
    if (!hostkey || !hostkey[0] || mbedtls_base64_decode(blob, sizeof blob, &len, (const unsigned char *)hostkey, strlen(hostkey)) != 0) return false;
    fingerprint(blob, len, fp);
    snprintf(out, n, "%s", fp);
    return true;
}

void aos_sshc_close(aos_sshc_t *c)
{
    if (!c) return;
    if (c->fd > 0) {
        if (c->live && !c->err && c->ch_open && !c->ch_closed) {
            wbuf_t b = pk_begin(c, MSG_CHANNEL_CLOSE);
            wb_u32(&b, c->peer);
            pk_send(c, &b);
        }
        if (c->live && !c->err) {
            wbuf_t b = pk_begin(c, MSG_DISCONNECT);
            wb_u32(&b, 11);                     /* by application */
            wb_cstr(&b, "done");
            wb_cstr(&b, "");
            pk_send(c, &b);
        }
        aos_hal_tcp_close(c->fd);
    }
    ssh_cipher_free(c->rx);
    ssh_cipher_free(c->tx);
    free(c->i_c); free(c->i_s); free(c->hostkey);
    free(c->in); free(c->out); free(c->pkt); free(c->rx_q); free(c->fx);
    free(c);
}

/* ---- the channel ---- */

static bool ch_write(aos_sshc_t *c, const uint8_t *d, size_t n)
{
    while (n && !c->err) {
        if (c->ch_closed) { fail(c, AOS_SSHC_E_IO, "the server closed the session", NULL); break; }
        if (!c->peer_win) {
            int t = next(c, STEP_MS);
            if (!t) fail(c, AOS_SSHC_E_IO, "the server stopped taking data", NULL);
            continue;
        }
        uint32_t m = n > FX_CHUNK ? FX_CHUNK : (uint32_t)n;
        if (m > c->peer_win) m = c->peer_win;
        if (m > c->peer_max) m = c->peer_max;
        wbuf_t b = pk_begin(c, MSG_CHANNEL_DATA);
        wb_u32(&b, c->peer);
        wb_string(&b, d, m);
        if (!pk_send(c, &b)) break;
        c->peer_win -= m;
        d += m;
        n -= m;
    }
    return !c->err;
}

/* 'n' bytes of what the server sent on the channel, waited for. */
static bool ch_read(aos_sshc_t *c, uint8_t *out, size_t n)
{
    while (c->rx_len < n && !c->err) {
        if (c->ch_eof || c->ch_closed) { fail(c, AOS_SSHC_E_IO, "the server closed the session", NULL); break; }
        int t = next(c, STEP_MS);
        if (!t) fail(c, AOS_SSHC_E_IO, "the server stopped answering", NULL);
    }
    if (c->err) return false;
    memcpy(out, c->rx_q, n);
    memmove(c->rx_q, c->rx_q + n, c->rx_len - n);
    c->rx_len -= n;
    c->consumed += (uint32_t)n;
    if (c->consumed >= WIN / 2) {
        wbuf_t b = pk_begin(c, MSG_CHANNEL_WINDOW_ADJUST);
        wb_u32(&b, c->peer);
        wb_u32(&b, c->consumed);
        pk_send(c, &b);
        c->consumed = 0;
    }
    return !c->err;
}

/* ---- SFTP ---- */

static wbuf_t fx_begin(aos_sshc_t *c, uint8_t type)
{
    wbuf_t b = { c->fx + 4, 0, FX_MAX - 4, false };
    wb_byte(&b, type);
    if (type != FXP_INIT) wb_u32(&b, ++c->fx_id);
    return b;
}

static void wb_u64(wbuf_t *b, uint64_t v) { wb_u32(b, (uint32_t)(v >> 32)); wb_u32(b, (uint32_t)v); }

/* The request goes, its reply comes into c->fx: the reply's type, or 0. */
static int fx_call(aos_sshc_t *c, wbuf_t *b)
{
    if (c->err) return 0;
    if (b->bad) { fail(c, AOS_SSHC_E_REMOTE, "a path too long to send", NULL); return 0; }
    uint32_t n = (uint32_t)b->len, want = c->fx_id;
    bool init = c->fx[4] == FXP_INIT;
    c->fx[0] = (uint8_t)(n >> 24); c->fx[1] = (uint8_t)(n >> 16); c->fx[2] = (uint8_t)(n >> 8); c->fx[3] = (uint8_t)n;
    if (!ch_write(c, c->fx, 4 + n)) return 0;
    uint8_t len[4];
    if (!ch_read(c, len, 4)) return 0;
    n = (uint32_t)len[0] << 24 | (uint32_t)len[1] << 16 | (uint32_t)len[2] << 8 | len[3];
    if (n < 1 || n > FX_MAX || !ch_read(c, c->fx, n)) {
        fail(c, AOS_SSHC_E_PROTOCOL, "what the server answered is not SFTP", NULL);
        return 0;
    }
    c->fx_len = n;
    if (!init) {
        rbuf_t r = { c->fx, n, 1, false };
        if (rb_u32(&r) != want) { fail(c, AOS_SSHC_E_PROTOCOL, "the server answered another request", NULL); return 0; }
    }
    return c->fx[0];
}

/* A reply that is a STATUS: its code, or -1 (not a status, or no reply). */
static int fx_status(aos_sshc_t *c, int type)
{
    if (type != FXP_STATUS) {
        if (type && !c->err) fail(c, AOS_SSHC_E_PROTOCOL, "the server answered something else", NULL);
        return -1;
    }
    rbuf_t r = { c->fx, c->fx_len, 5, false };
    return (int)rb_u32(&r);
}

/* Attributes in a reply: read past them, keeping the size and the date. */
static void rb_attrs(rbuf_t *r, uint64_t *size, uint32_t *mtime, bool *dir)
{
    uint32_t flags = rb_u32(r);
    if (size) *size = 0;
    if (mtime) *mtime = 0;
    if (dir) *dir = false;
    if (flags & ATTR_SIZE) { uint64_t hi = rb_u32(r); uint64_t v = hi << 32 | rb_u32(r); if (size) *size = v; }
    if (flags & ATTR_UIDGID) { rb_u32(r); rb_u32(r); }
    if (flags & ATTR_PERMISSIONS) { uint32_t p = rb_u32(r); if (dir) *dir = (p & 0170000) == 0040000; }
    if (flags & ATTR_ACMODTIME) { rb_u32(r); uint32_t m = rb_u32(r); if (mtime) *mtime = m; }
    if (flags & 0x80000000u) {
        uint32_t n = rb_u32(r);
        size_t l;
        for (uint32_t i = 0; i < n && !r->bad; i++) { rb_string(r, &l); rb_string(r, &l); }
    }
}

int aos_sshc_sftp(aos_sshc_t *c)
{
    if (c->err) return c->err;
    wbuf_t b = pk_begin(c, MSG_CHANNEL_OPEN);
    wb_cstr(&b, "session");
    wb_u32(&b, 0);
    wb_u32(&b, WIN);
    wb_u32(&b, MAXPKT);
    if (!pk_send(c, &b)) return c->err;
    int t = next(c, STEP_MS);
    if (t != MSG_CHANNEL_OPEN_CONFIRMATION) {
        if (t >= 0) fail(c, t ? AOS_SSHC_E_REMOTE : AOS_SSHC_E_IO, "the server did not open a session", NULL);
        return c->err;
    }
    rbuf_t r = { c->pkt, c->pkt_len, 1, false };
    rb_u32(&r);
    c->peer = rb_u32(&r);
    c->peer_win = rb_u32(&r);
    c->peer_max = rb_u32(&r);
    if (r.bad || c->peer_max < 64) return fail(c, AOS_SSHC_E_PROTOCOL, "the server sent something that is not SSH", NULL);
    c->ch_open = true;

    b = pk_begin(c, MSG_CHANNEL_REQUEST);
    wb_u32(&b, c->peer);
    wb_cstr(&b, "subsystem");
    wb_byte(&b, 1);
    wb_cstr(&b, "sftp");
    if (!pk_send(c, &b)) return c->err;
    do t = next(c, STEP_MS); while (t == MSG_CHANNEL_WINDOW_ADJUST);
    if (t != MSG_CHANNEL_SUCCESS) {
        if (t >= 0) fail(c, t ? AOS_SSHC_E_REMOTE : AOS_SSHC_E_IO, "the server has no SFTP for this user", NULL);
        return c->err;
    }

    wbuf_t f = fx_begin(c, FXP_INIT);
    wb_u32(&f, 3);
    if (fx_call(c, &f) != FXP_VERSION) {
        if (!c->err) fail(c, AOS_SSHC_E_PROTOCOL, "what the server answered is not SFTP", NULL);
        return c->err;
    }
    r = (rbuf_t){ c->fx, c->fx_len, 1, false };
    rb_u32(&r);
    while (!r.bad && r.off < r.len) {       /* what else it knows */
        char name[48], data[16];
        rb_cstr(&r, name, sizeof name);
        rb_cstr(&r, data, sizeof data);
        if (!r.bad && !strcmp(name, "posix-rename@openssh.com")) c->posix_rename = true;
    }
    return AOS_SSHC_OK;
}

int aos_sshc_stat(aos_sshc_t *c, const char *path, bool *there, uint64_t *size)
{
    if (there) *there = false;
    wbuf_t f = fx_begin(c, FXP_STAT);
    wb_cstr(&f, path);
    int t = fx_call(c, &f);
    if (t == FXP_ATTRS) {
        rbuf_t r = { c->fx, c->fx_len, 5, false };
        rb_attrs(&r, size, NULL, NULL);
        if (there) *there = true;
        return AOS_SSHC_OK;
    }
    int code = fx_status(c, t);
    if (code < 0) return c->err;
    return code == FX_NO_SUCH_FILE ? AOS_SSHC_OK : fail(c, AOS_SSHC_E_REMOTE, "the server would not look at %.80s", path);
}

int aos_sshc_mkdirs(aos_sshc_t *c, const char *path)
{
    size_t n = strlen(path);
    char *part = malloc(n + 1);
    if (!part) return fail(c, AOS_SSHC_E_MEMORY, "no memory", NULL);
    /* each folder on the way, the ones that are there refusing; what
     * matters is whether the last one is there at the end */
    for (size_t i = 1; i <= n && !c->err; i++) {
        if (path[i] != '/' && path[i] != 0) continue;
        memcpy(part, path, i);
        part[i] = 0;
        if (part[i - 1] == '/') continue;
        wbuf_t f = fx_begin(c, FXP_MKDIR);
        wb_cstr(&f, part);
        wb_u32(&f, 0);
        fx_status(c, fx_call(c, &f));
    }
    free(part);
    if (c->err) return c->err;
    bool there = false;
    aos_sshc_stat(c, path, &there, NULL);
    if (!c->err && !there) fail(c, AOS_SSHC_E_REMOTE, "the folder %.80s could not be made on the server", path);
    return c->err;
}

int aos_sshc_remove(aos_sshc_t *c, const char *path)
{
    wbuf_t f = fx_begin(c, FXP_REMOVE);
    wb_cstr(&f, path);
    int code = fx_status(c, fx_call(c, &f));
    if (code < 0) return c->err;
    return code == FX_OK || code == FX_NO_SUCH_FILE ? AOS_SSHC_OK : AOS_SSHC_E_REMOTE;
}

int aos_sshc_put(aos_sshc_t *c, const char *local, const char *remote)
{
    if (c->err) return c->err;
    size_t rl = strlen(remote);
    char *tmp = malloc(rl + 8);
    uint8_t *buf = big_calloc(FX_CHUNK);
    FILE *in = fopen(local, "rb");
    uint8_t handle[64];
    size_t hl = 0;
    bool opened = false;
    if (!tmp || !buf) fail(c, AOS_SSHC_E_MEMORY, "no memory", NULL);
    else if (!in) fail(c, AOS_SSHC_E_LOCAL, "%.80s could not be read", local);
    else {
        snprintf(tmp, rl + 8, "%s.part", remote);
        wbuf_t f = fx_begin(c, FXP_OPEN);
        wb_cstr(&f, tmp);
        wb_u32(&f, 0x02 | 0x08 | 0x10);         /* write, create, truncate */
        wb_u32(&f, 0);
        int t = fx_call(c, &f);
        if (t == FXP_HANDLE) {
            rbuf_t r = { c->fx, c->fx_len, 5, false };
            size_t n;
            const uint8_t *h = rb_string(&r, &n);
            if (h && n && n <= sizeof handle) { memcpy(handle, h, n); hl = n; opened = true; }
        }
        if (!opened && !c->err) fail(c, AOS_SSHC_E_REMOTE, "the server would not take %.80s (does the folder exist, can it be written?)", remote);
    }
    uint64_t off = 0;
    size_t got;
    while (opened && !c->err && (got = fread(buf, 1, FX_CHUNK, in)) > 0) {
        wbuf_t f = fx_begin(c, FXP_WRITE);
        wb_string(&f, handle, hl);
        wb_u64(&f, off);
        wb_string(&f, buf, got);
        int code = fx_status(c, fx_call(c, &f));
        if (code > 0) fail(c, AOS_SSHC_E_REMOTE, "the server stopped taking %.80s (is it full?)", remote);
        off += got;
    }
    if (opened && !c->err && ferror(in)) fail(c, AOS_SSHC_E_LOCAL, "%.80s could not be read to its end", local);
    if (opened) {
        /* closed whatever happened, when the connection still stands */
        int had = c->err;
        if (had == AOS_SSHC_E_LOCAL || had == AOS_SSHC_E_REMOTE) c->err = 0;
        wbuf_t f = fx_begin(c, FXP_CLOSE);
        wb_string(&f, handle, hl);
        int code = fx_status(c, fx_call(c, &f));
        if (!had && code > 0) fail(c, AOS_SSHC_E_REMOTE, "the server could not finish %.80s", remote);
        if (had) {
            if (!c->err) aos_sshc_remove(c, tmp);
            c->err = had;
        }
    }
    if (opened && !c->err) {
        /* over the name: the server's own rename-over when it has one */
        int code;
        if (c->posix_rename) {
            wbuf_t f = fx_begin(c, FXP_EXTENDED);
            wb_cstr(&f, "posix-rename@openssh.com");
            wb_cstr(&f, tmp);
            wb_cstr(&f, remote);
            code = fx_status(c, fx_call(c, &f));
        } else {
            aos_sshc_remove(c, remote);
            wbuf_t f = fx_begin(c, FXP_RENAME);
            wb_cstr(&f, tmp);
            wb_cstr(&f, remote);
            code = fx_status(c, fx_call(c, &f));
        }
        if (code > 0) fail(c, AOS_SSHC_E_REMOTE, "%.80s went up but could not be given its name", remote);
    }
    if (in) fclose(in);
    free(tmp);
    free(buf);
    return c->err;
}

int aos_sshc_list(aos_sshc_t *c, const char *path, aos_sshc_name_t each, void *ctx)
{
    uint8_t handle[64];
    size_t hl = 0;
    wbuf_t f = fx_begin(c, FXP_OPENDIR);
    wb_cstr(&f, path);
    int t = fx_call(c, &f);
    if (t != FXP_HANDLE) {
        if (!c->err) return AOS_SSHC_E_REMOTE;      /* not there: nothing to list, and the connection stands */
        return c->err;
    }
    rbuf_t r = { c->fx, c->fx_len, 5, false };
    size_t n;
    const uint8_t *h = rb_string(&r, &n);
    if (!h || !n || n > sizeof handle) return fail(c, AOS_SSHC_E_PROTOCOL, "what the server answered is not SFTP", NULL);
    memcpy(handle, h, n);
    hl = n;
    for (int rounds = 0; rounds < 2000 && !c->err; rounds++) {
        f = fx_begin(c, FXP_READDIR);
        wb_string(&f, handle, hl);
        t = fx_call(c, &f);
        if (t != FXP_NAME) break;               /* EOF, or the end of what it will say */
        r = (rbuf_t){ c->fx, c->fx_len, 5, false };
        uint32_t count = rb_u32(&r);
        for (uint32_t i = 0; i < count && !r.bad; i++) {
            char name[256];
            size_t ll;
            uint64_t size;
            uint32_t mtime;
            bool dir;
            rb_cstr(&r, name, sizeof name);
            rb_string(&r, &ll);
            rb_attrs(&r, &size, &mtime, &dir);
            if (!r.bad && strcmp(name, ".") && strcmp(name, "..") && each) each(ctx, name, dir, size, mtime);
        }
    }
    if (!c->err) {
        f = fx_begin(c, FXP_CLOSE);
        wb_string(&f, handle, hl);
        fx_status(c, fx_call(c, &f));
    }
    return c->err;
}

/* ---- the key this side logs in with ---- */

bool aos_sshc_key_line(const uint8_t seed_in[32], const char *comment, char *out, size_t n)
{
    uint8_t seed[32], sk[64], pub[32];
    uint8_t blob[4 + 11 + 4 + 32] = { 0, 0, 0, 11, 's', 's', 'h', '-', 'e', 'd', '2', '5', '5', '1', '9', 0, 0, 0, 32 };
    char b64[80];
    size_t got = 0;
    memcpy(seed, seed_in, 32);
    crypto_ed25519_key_pair(sk, pub, seed);
    crypto_wipe(sk, sizeof sk);
    memcpy(blob + 19, pub, 32);
    if (mbedtls_base64_encode((unsigned char *)b64, sizeof b64, &got, blob, sizeof blob) != 0) return false;
    b64[got] = 0;
    snprintf(out, n, "ssh-ed25519 %s %s", b64, comment ? comment : "p4os");
    return true;
}
