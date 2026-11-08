/*
 * P4OS - SSH client: the session.
 *
 * SSH-2 as a client, in a thread of the app's own (aos_hal_thread_start,
 * PSRAM stack) over one aos_hal_tcp_* handle:
 *
 *   transport   curve25519-sha256; the host key ssh-ed25519 first, else
 *               ecdsa-sha2-nistp256 or RSA with SHA-2 (checked by the
 *               firmware's ssh_key_verify); chacha20-poly1305@openssh.com,
 *               which every OpenSSH since 6.5 has; strict KEX when the
 *               server offers it. The server may exchange keys again
 *               whenever it likes.
 *   host key    known_hosts in the app's data folder: a new one is shown
 *               with its fingerprint and the user says yes or no; a changed
 *               one is refused unless the user forgets the old one.
 *   login       the board's Ed25519 key, if the server takes publickey;
 *               then the password (asked on the screen, never kept), by
 *               "password" or "keyboard-interactive", whichever the server
 *               offers (a Mac and PAM want the second).
 *   channel     one session: pty-req (xterm-256color, the view's size),
 *               shell, window-change; and a second one, when asked, that
 *               puts the board's key in ~/.ssh/authorized_keys.
 *
 * The UI talks to it through the struct's fields under its mutex: the
 * state, the questions and their answers, the bytes typed, a new size.
 * Texts for the screen are kept as their Spanish keys and translated by the
 * UI (_() belongs to the app in front, and to LVGL's thread).
 */
#include "sc.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ssh_chacha.h"
#include "aos_ssh_wire.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PKT_MAX      35000
#define BUF_CAP      (PKT_MAX + 4 + 16 + 64)
#define WIN          (256 * 1024)
#define MAXPKT       32768
#define TXQ_CAP      8192
#define CONNECT_MS   6000
#define ASK_MS       120000

static const char V_C[] = "SSH-2.0-P4OS_ssh";
static const char KEX_ALGS[] = "curve25519-sha256,curve25519-sha256@libssh.org,ext-info-c,kex-strict-c-v00@openssh.com";
static const char HOSTKEY_ALGS[] = "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256";
static const char CIPHER[] = "chacha20-poly1305@openssh.com";

enum {
    MSG_DISCONNECT = 1, MSG_IGNORE, MSG_UNIMPLEMENTED, MSG_DEBUG, MSG_SERVICE_REQUEST, MSG_SERVICE_ACCEPT,
    MSG_EXT_INFO,
    MSG_KEXINIT = 20, MSG_NEWKEYS,
    MSG_KEX_ECDH_INIT = 30, MSG_KEX_ECDH_REPLY,
    MSG_USERAUTH_REQUEST = 50, MSG_USERAUTH_FAILURE, MSG_USERAUTH_SUCCESS, MSG_USERAUTH_BANNER,
    MSG_USERAUTH_INFO_REQUEST = 60, MSG_USERAUTH_INFO_RESPONSE,
    MSG_GLOBAL_REQUEST = 80, MSG_REQUEST_SUCCESS, MSG_REQUEST_FAILURE,
    MSG_CHANNEL_OPEN = 90, MSG_CHANNEL_OPEN_CONFIRMATION, MSG_CHANNEL_OPEN_FAILURE, MSG_CHANNEL_WINDOW_ADJUST,
    MSG_CHANNEL_DATA, MSG_CHANNEL_EXTENDED_DATA, MSG_CHANNEL_EOF, MSG_CHANNEL_CLOSE, MSG_CHANNEL_REQUEST,
    MSG_CHANNEL_SUCCESS, MSG_CHANNEL_FAILURE,
};

typedef struct {
    bool open, closing;
    uint32_t peer, peer_win, peer_max, consumed;
} chan_t;

struct sc_sess {
    sc_host_t host;
    sc_term_t *term;
    void *mx;
    volatile bool stop, done;

    /* what the UI sees, under mx */
    sc_state_t state;
    const char *why;            /* a key for _() with at most one %s */
    char why_arg[120];
    uint32_t ask_seq;           /* one more for every question */
    sc_hostkey_q_t hk;
    int hk_answer;              /* -1 waiting, 0 no, 1 yes */
    char pw_prompt[96];
    bool pw_retry;              /* the last one did not do */
    char pw[128];
    int pw_answer;              /* -1 waiting, 0 cancelled, 1 given */
    uint8_t txq[TXQ_CAP];
    size_t txn;
    int want_cols, want_rows;
    bool want_resize, want_install;
    const char *note;           /* a line for the terminal, a key for _() */
    bool key_used;

    /* the thread's own */
    int fd;
    uint8_t *in, *out, *pkt;
    size_t in_len, pkt_len;
    char v_s[256];
    uint8_t *i_c, *i_s;
    size_t i_c_len, i_s_len;
    bool first_kex, strict, have_sid, ignore_next;
    uint8_t sid[32];
    uint8_t *hk_blob;
    size_t hk_len;
    char hk_alg[24];
    ssh_chachapoly_t rx, tx;
    bool rx_on, tx_on;
    uint32_t seq_rx, seq_tx;
    chan_t ch[2];               /* 0 the shell, 1 the key's installation */
    int cols, rows;
};

static void lock(sc_sess_t *s) { aos_hal_mutex_lock(s->mx); }
static void unlock(sc_sess_t *s) { aos_hal_mutex_unlock(s->mx); }

static void set_state(sc_sess_t *s, sc_state_t st)
{
    lock(s);
    s->state = st;
    unlock(s);
}

/* the end, with why (a key for _(); arg for its %s) */
static void fail(sc_sess_t *s, const char *why, const char *arg)
{
    lock(s);
    if (s->state != SC_ST_CLOSED) {
        s->why = why;
        sc_copy(s->why_arg, sizeof s->why_arg, arg ? arg : "");
        s->state = SC_ST_CLOSED;
    }
    unlock(s);
    aos_hal_log(SC_TAG, "%s: %s %s", s->host.host, why, arg ? arg : "");
}

static bool closed(sc_sess_t *s)
{
    lock(s);
    bool c = s->state == SC_ST_CLOSED;
    unlock(s);
    return c || s->stop;
}

/* ---- packets ---- */

static wbuf_t pk_begin(sc_sess_t *s, uint8_t type)
{
    wbuf_t b = { s->out + 5, 0, PKT_MAX - 64, false };
    wb_byte(&b, type);
    return b;
}

static bool pk_send(sc_sess_t *s, wbuf_t *b)
{
    if (b->bad || s->fd <= 0) return false;
    /* blocks of 8; with ChaCha20 the length is not counted */
    size_t head = s->tx_on ? 1 : 5, pad = 8 - (head + b->len) % 8;
    if (pad < 4) pad += 8;
    size_t L = 1 + b->len + pad;
    uint8_t *o = s->out;
    o[0] = (uint8_t)(L >> 24);
    o[1] = (uint8_t)(L >> 16);
    o[2] = (uint8_t)(L >> 8);
    o[3] = (uint8_t)L;
    o[4] = (uint8_t)pad;
    aos_hal_random(o + 5 + b->len, pad);
    size_t total = 4 + L;
    if (s->tx_on) {
        ssh_chachapoly_seal(&s->tx, o, L, s->seq_tx);
        total += 16;
    }
    s->seq_tx++;
    if (aos_hal_tcp_send(s->fd, o, (int)total, 10000) != (int)total) {
        fail(s, N_("Se cortó la conexión."), NULL);
        return false;
    }
    return true;
}

/* A whole packet into s->pkt: 1, 0 if none came in timeout_ms, -1 when
 * the session is over (fail() said why). */
static int pk_recv(sc_sess_t *s, int timeout_ms)
{
    for (;;) {
        if (s->in_len >= 4) {
            uint8_t *q = s->in;
            uint32_t L = s->rx_on ? ssh_chachapoly_length(&s->rx, q, s->seq_rx)
                                  : (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3];
            if (L < (s->rx_on ? 8u : 12u) || L > PKT_MAX || (s->rx_on ? L % 8 : (L + 4) % 8)) {
                fail(s, N_("El equipo mandó algo que no es SSH."), NULL);
                return -1;
            }
            size_t total = 4 + L + (s->rx_on ? 16 : 0);
            if (s->in_len >= total) {
                if (s->rx_on && !ssh_chachapoly_open(&s->rx, q, L, s->seq_rx)) {
                    fail(s, N_("Un paquete llegó dañado."), NULL);
                    return -1;
                }
                uint8_t pad = q[4];
                if (pad < 4 || (size_t)pad + 1 >= L) {
                    fail(s, N_("El equipo mandó algo que no es SSH."), NULL);
                    return -1;
                }
                s->pkt_len = L - 1 - pad;
                memcpy(s->pkt, q + 5, s->pkt_len);
                s->seq_rx++;
                memmove(s->in, s->in + total, s->in_len - total);
                s->in_len -= total;
                return 1;
            }
        }
        if (s->stop) return -1;
        int k = aos_hal_tcp_recv(s->fd, s->in + s->in_len, (int)(BUF_CAP - s->in_len), timeout_ms);
        if (k < 0) {
            fail(s, N_("El equipo cerró la conexión."), NULL);
            return -1;
        }
        if (k == 0) return 0;
        s->in_len += (size_t)k;
    }
}

/* the next packet that matters, waiting as long as it takes (but stop) */
static int pk_wait(sc_sess_t *s)
{
    for (;;) {
        int r = pk_recv(s, 100);
        if (r < 0) return -1;
        if (!r) continue;
        uint8_t t = s->pkt[0];
        if (t == MSG_IGNORE || t == MSG_DEBUG || t == MSG_UNIMPLEMENTED || t == MSG_EXT_INFO) {
            if (s->first_kex && s->strict && !s->have_sid) {
                fail(s, N_("El equipo mandó algo que no es SSH."), NULL);
                return -1;
            }
            continue;
        }
        if (t == MSG_DISCONNECT) {
            rbuf_t r2 = { s->pkt, s->pkt_len, 5, false };
            char why[120];
            rb_cstr(&r2, why, sizeof why);
            fail(s, N_("El equipo cortó: %s"), why);
            return -1;
        }
        if (t == MSG_USERAUTH_BANNER && s->term) {
            rbuf_t r2 = { s->pkt, s->pkt_len, 1, false };
            size_t n;
            const uint8_t *msg = rb_string(&r2, &n);
            if (msg) {
                sc_term_lock(s->term);
                for (size_t i = 0; i < n; i++) {
                    if (msg[i] == '\n') sc_term_feed(s->term, (const uint8_t *)"\r", 1);
                    sc_term_feed(s->term, msg + i, 1);
                }
                sc_term_unlock(s->term);
            }
            continue;
        }
        return 1;
    }
}

/* ---- key exchange ---- */

static bool send_kexinit(sc_sess_t *s)
{
    wbuf_t b = pk_begin(s, MSG_KEXINIT);
    uint8_t cookie[16];
    aos_hal_random(cookie, sizeof cookie);
    wb_bytes(&b, cookie, 16);
    wb_cstr(&b, s->first_kex ? KEX_ALGS : "curve25519-sha256,curve25519-sha256@libssh.org");
    wb_cstr(&b, HOSTKEY_ALGS);
    wb_cstr(&b, CIPHER);
    wb_cstr(&b, CIPHER);
    wb_cstr(&b, "hmac-sha2-256");
    wb_cstr(&b, "hmac-sha2-256");
    wb_cstr(&b, "none");
    wb_cstr(&b, "none");
    wb_cstr(&b, "");
    wb_cstr(&b, "");
    wb_byte(&b, 0);
    wb_u32(&b, 0);
    if (b.bad) return false;
    free(s->i_c);
    s->i_c = malloc(b.len);
    if (!s->i_c) return false;
    memcpy(s->i_c, b.p, b.len);
    s->i_c_len = b.len;
    return pk_send(s, &b);
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

static void hash_string(aos_hal_sha256_ctx_t *x, const void *d, size_t n)
{
    uint8_t l[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    aos_hal_sha256_update(x, l, 4);
    aos_hal_sha256_update(x, d, n);
}

static void derive(sc_sess_t *s, const uint8_t *kmp, size_t kn, const uint8_t h[32], char letter, uint8_t out[64])
{
    aos_hal_sha256_ctx_t *x = aos_hal_sha256_begin();
    aos_hal_sha256_update(x, kmp, kn);
    aos_hal_sha256_update(x, h, 32);
    aos_hal_sha256_update(x, &letter, 1);
    aos_hal_sha256_update(x, s->sid, 32);
    aos_hal_sha256_end(x, out);
    x = aos_hal_sha256_begin();
    aos_hal_sha256_update(x, kmp, kn);
    aos_hal_sha256_update(x, h, 32);
    aos_hal_sha256_update(x, out, 32);
    aos_hal_sha256_end(x, out + 32);
}

/* What the user says about a host key not yet known (or changed). */
static bool ask_hostkey(sc_sess_t *s, int known)
{
    lock(s);
    ssh_key_describe(s->hk_blob, s->hk_len, s->hk.type, sizeof s->hk.type);
    sc_fingerprint(s->hk_blob, s->hk_len, s->hk.fp, sizeof s->hk.fp);
    s->hk.known = known;
    s->hk_answer = -1;
    s->ask_seq++;
    s->state = SC_ST_ASK_HOSTKEY;
    unlock(s);
    for (int t = 0; t < ASK_MS / 100; t++) {
        lock(s);
        int a = s->hk_answer;
        unlock(s);
        if (a >= 0 || s->stop) {
            set_state(s, SC_ST_CONNECTING);
            return a == 1;
        }
        aos_hal_sleep_ms(100);
    }
    return false;
}

/* The exchange, from the server's KEXINIT in s->pkt (ours already sent). */
static bool kex(sc_sess_t *s)
{
    free(s->i_s);
    s->i_s = malloc(s->pkt_len);
    if (!s->i_s) return false;
    memcpy(s->i_s, s->pkt, s->pkt_len);
    s->i_s_len = s->pkt_len;
    rbuf_t r = { s->i_s, s->i_s_len, 1 + 16, false };
    const uint8_t *l[10];
    size_t ln[10];
    for (int i = 0; i < 10; i++) l[i] = rb_string(&r, &ln[i]);
    bool follows = rb_bool(&r);
    char kexn[40], enc[40], comp[8];
    if (r.bad || !pick(l[0], ln[0], "curve25519-sha256,curve25519-sha256@libssh.org", kexn, sizeof kexn)) {
        fail(s, N_("El equipo no tiene curve25519 (ningún algoritmo en común)."), NULL);
        return false;
    }
    if (!pick(l[1], ln[1], HOSTKEY_ALGS, s->hk_alg, sizeof s->hk_alg)) {
        fail(s, N_("El equipo no tiene un tipo de clave que la placa conozca."), NULL);
        return false;
    }
    if (!pick(l[2], ln[2], CIPHER, enc, sizeof enc) || !pick(l[3], ln[3], CIPHER, enc, sizeof enc)) {
        fail(s, N_("El equipo no tiene chacha20-poly1305 (ningún cifrado en común)."), NULL);
        return false;
    }
    if (!pick(l[6], ln[6], "none", comp, sizeof comp) || !pick(l[7], ln[7], "none", comp, sizeof comp)) {
        fail(s, N_("El equipo sólo acepta compresión."), NULL);
        return false;
    }
    if (s->first_kex) s->strict = namelist_has(l[0], ln[0], "kex-strict-s-v00@openssh.com");
    s->ignore_next = follows && !(ln[0] >= strlen(kexn) && !memcmp(l[0], kexn, strlen(kexn)));

    uint8_t esk[32], epk[32];
    aos_hal_random(esk, sizeof esk);
    crypto_x25519_public_key(epk, esk);
    wbuf_t b = pk_begin(s, MSG_KEX_ECDH_INIT);
    wb_string(&b, epk, 32);
    if (!pk_send(s, &b)) return false;

    for (;;) {
        if (pk_wait(s) < 0) return false;
        if (s->ignore_next && s->pkt[0] >= 30 && s->pkt[0] <= 49) {
            s->ignore_next = false;
            continue;
        }
        break;
    }
    if (s->pkt[0] != MSG_KEX_ECDH_REPLY) {
        fail(s, N_("El equipo mandó algo que no es SSH."), NULL);
        return false;
    }
    r = (rbuf_t){ s->pkt, s->pkt_len, 1, false };
    size_t ksn, qsn, sgn;
    const uint8_t *ks = rb_string(&r, &ksn), *qs = rb_string(&r, &qsn), *sg = rb_string(&r, &sgn);
    uint8_t k[32];
    if (r.bad || qsn != 32) {
        fail(s, N_("El equipo mandó algo que no es SSH."), NULL);
        return false;
    }
    crypto_x25519(k, esk, qs);
    crypto_wipe(esk, sizeof esk);
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= k[i];
    if (!acc) {
        fail(s, N_("El equipo mandó algo que no es SSH."), NULL);
        return false;
    }
    uint8_t kmp[4 + 33];
    wbuf_t kb = { kmp, 0, sizeof kmp, false };
    wb_mpint(&kb, k, 32);
    crypto_wipe(k, sizeof k);

    uint8_t h[32];
    aos_hal_sha256_ctx_t *x = aos_hal_sha256_begin();
    hash_string(x, V_C, strlen(V_C));
    hash_string(x, s->v_s, strlen(s->v_s));
    hash_string(x, s->i_c, s->i_c_len);
    hash_string(x, s->i_s, s->i_s_len);
    hash_string(x, ks, ksn);
    hash_string(x, epk, 32);
    hash_string(x, qs, 32);
    aos_hal_sha256_update(x, kmp, kb.len);
    aos_hal_sha256_end(x, h);

    if (!ssh_key_verify(s->hk_alg, ks, ksn, sg, sgn, h, 32)) {
        fail(s, N_("La firma del equipo no coincide con su clave."), NULL);
        return false;
    }
    if (s->first_kex) {
        free(s->hk_blob);
        s->hk_blob = malloc(ksn);
        if (!s->hk_blob) return false;
        memcpy(s->hk_blob, ks, ksn);
        s->hk_len = ksn;
        int known = sc_known_check(s->host.host, s->host.port, ks, ksn);
        if (known != 1) {
            if (!ask_hostkey(s, known)) {
                fail(s, known < 0 ? N_("La clave del equipo cambió: no se conectó.") : N_("No se aceptó la clave del equipo."), NULL);
                return false;
            }
            sc_known_add(s->host.host, s->host.port, ks, ksn);
        }
    } else if (ksn != s->hk_len || memcmp(ks, s->hk_blob, ksn)) {
        fail(s, N_("La clave del equipo cambió en medio de la sesión."), NULL);
        return false;
    }
    if (!s->have_sid) {
        memcpy(s->sid, h, 32);
        s->have_sid = true;
    }
    uint8_t kc[64], kd[64];
    derive(s, kmp, kb.len, h, 'C', kc);
    derive(s, kmp, kb.len, h, 'D', kd);
    crypto_wipe(kmp, sizeof kmp);

    b = pk_begin(s, MSG_NEWKEYS);
    if (!pk_send(s, &b)) return false;
    ssh_chachapoly_init(&s->tx, kc);
    s->tx_on = true;
    if (s->strict) s->seq_tx = 0;
    crypto_wipe(kc, sizeof kc);

    if (pk_wait(s) < 0) return false;
    if (s->pkt[0] != MSG_NEWKEYS) {
        fail(s, N_("El equipo mandó algo que no es SSH."), NULL);
        return false;
    }
    ssh_chachapoly_init(&s->rx, kd);
    s->rx_on = true;
    if (s->strict) s->seq_rx = 0;
    crypto_wipe(kd, sizeof kd);
    s->first_kex = false;
    return true;
}

/* ---- logging in ---- */

static int auth_reply(sc_sess_t *s, char *methods, size_t mn)
{
    for (;;) {
        if (pk_wait(s) < 0) return -1;
        uint8_t t = s->pkt[0];
        if (t == MSG_USERAUTH_SUCCESS) return 1;
        if (t == MSG_USERAUTH_FAILURE) {
            rbuf_t r = { s->pkt, s->pkt_len, 1, false };
            rb_cstr(&r, methods, mn);
            return 0;
        }
        if (t == MSG_USERAUTH_INFO_REQUEST) return 2;   /* keyboard-interactive's */
        if (t == MSG_GLOBAL_REQUEST || t == MSG_USERAUTH_BANNER) continue;
        fail(s, N_("El equipo mandó algo que no es SSH."), NULL);
        return -1;
    }
}

static void auth_head(wbuf_t *b, sc_sess_t *s, const char *method)
{
    wb_cstr(b, s->host.user);
    wb_cstr(b, "ssh-connection");
    wb_cstr(b, method);
}

static bool try_key(sc_sess_t *s)
{
    uint8_t sk[64], pub[32];
    if (!sc_key(sk, pub)) return false;
    uint8_t blob[4 + 11 + 4 + 32];
    wbuf_t kb = { blob, 0, sizeof blob, false };
    wb_cstr(&kb, "ssh-ed25519");
    wb_string(&kb, pub, 32);
    /* what is signed (RFC 4252, 7): the session id, then the request */
    size_t cap = 512 + strlen(s->host.user);
    uint8_t *m = malloc(cap);
    if (!m) {
        crypto_wipe(sk, sizeof sk);
        return false;
    }
    wbuf_t mb = { m, 0, cap, false };
    wb_string(&mb, s->sid, 32);
    wb_byte(&mb, MSG_USERAUTH_REQUEST);
    auth_head(&mb, s, "publickey");
    wb_byte(&mb, 1);
    wb_cstr(&mb, "ssh-ed25519");
    wb_string(&mb, blob, kb.len);
    uint8_t sig[64];
    crypto_ed25519_sign(sig, sk, m, mb.len);
    crypto_wipe(sk, sizeof sk);
    free(m);
    wbuf_t b = pk_begin(s, MSG_USERAUTH_REQUEST);
    auth_head(&b, s, "publickey");
    wb_byte(&b, 1);
    wb_cstr(&b, "ssh-ed25519");
    wb_string(&b, blob, kb.len);
    wb_u32(&b, 4 + 11 + 4 + 64);
    wb_cstr(&b, "ssh-ed25519");
    wb_string(&b, sig, 64);
    return pk_send(s, &b);
}

/* The password from the screen: true with it in s->pw (the caller wipes it). */
static bool ask_password(sc_sess_t *s, const char *prompt, bool retry)
{
    lock(s);
    sc_copy(s->pw_prompt, sizeof s->pw_prompt, prompt ? prompt : "");
    s->pw_retry = retry;
    s->pw_answer = -1;
    s->ask_seq++;
    s->state = SC_ST_ASK_PASSWORD;
    unlock(s);
    for (int t = 0; t < ASK_MS / 100; t++) {
        lock(s);
        int a = s->pw_answer;
        unlock(s);
        if (a >= 0 || s->stop) {
            set_state(s, SC_ST_CONNECTING);
            return a == 1;
        }
        aos_hal_sleep_ms(100);
    }
    return false;
}

/* keyboard-interactive's questions, in s->pkt; the password for those that
 * do not echo, the screen for any other */
static bool answer_info(sc_sess_t *s, bool *used_pw)
{
    rbuf_t r = { s->pkt, s->pkt_len, 1, false };
    size_t n;
    rb_string(&r, &n);          /* name */
    rb_string(&r, &n);          /* instruction */
    rb_string(&r, &n);          /* language */
    uint32_t count = rb_u32(&r);
    if (r.bad || count > 8) return false;
    wbuf_t b = pk_begin(s, MSG_USERAUTH_INFO_RESPONSE);
    wb_u32(&b, count);
    for (uint32_t i = 0; i < count; i++) {
        char prompt[96];
        rb_cstr(&r, prompt, sizeof prompt);
        bool echo = rb_bool(&r);
        (void)echo;
        if (*used_pw || !s->pw[0]) {
            /* a second question (a code, a new password): asked as such */
            if (!ask_password(s, prompt, false)) return false;
        }
        *used_pw = true;
        wb_cstr(&b, s->pw);
    }
    bool ok = pk_send(s, &b);
    crypto_wipe(s->pw, sizeof s->pw);
    return ok;
}

static bool login(sc_sess_t *s)
{
    wbuf_t b = pk_begin(s, MSG_SERVICE_REQUEST);
    wb_cstr(&b, "ssh-userauth");
    if (!pk_send(s, &b)) return false;
    for (;;) {
        if (pk_wait(s) < 0) return false;
        if (s->pkt[0] == MSG_SERVICE_ACCEPT) break;
        if (s->pkt[0] != MSG_GLOBAL_REQUEST) {
            fail(s, N_("El equipo mandó algo que no es SSH."), NULL);
            return false;
        }
    }
    char methods[160] = "";
    b = pk_begin(s, MSG_USERAUTH_REQUEST);
    auth_head(&b, s, "none");
    if (!pk_send(s, &b)) return false;
    int r = auth_reply(s, methods, sizeof methods);
    if (r == 1) return true;
    if (r < 0) return false;

    if (namelist_has((const uint8_t *)methods, strlen(methods), "publickey")) {
        if (!try_key(s)) return false;
        r = auth_reply(s, methods, sizeof methods);
        if (r == 1) {
            lock(s);
            s->key_used = true;
            unlock(s);
            aos_hal_log(SC_TAG, "%s: in with the board's key", s->host.host);
            return true;
        }
        if (r < 0) return false;
    }
    bool pw = namelist_has((const uint8_t *)methods, strlen(methods), "password");
    bool ki = namelist_has((const uint8_t *)methods, strlen(methods), "keyboard-interactive");
    if (!pw && !ki) {
        fail(s, N_("El equipo no acepta la clave de la placa ni contraseña (%s)."), methods);
        return false;
    }
    for (int tries = 0; tries < 3; tries++) {
        char prompt[96];
        snprintf(prompt, sizeof prompt, "%.39s@%.55s", s->host.user, s->host.host);
        if (!ask_password(s, prompt, tries > 0)) {
            fail(s, N_("No se entró."), NULL);
            return false;
        }
        b = pk_begin(s, MSG_USERAUTH_REQUEST);
        if (pw) {
            auth_head(&b, s, "password");
            wb_byte(&b, 0);
            wb_cstr(&b, s->pw);
            crypto_wipe(s->pw, sizeof s->pw);
            if (!pk_send(s, &b)) return false;
            r = auth_reply(s, methods, sizeof methods);
        } else {
            auth_head(&b, s, "keyboard-interactive");
            wb_cstr(&b, "");
            wb_cstr(&b, "");
            if (!pk_send(s, &b)) return false;
            bool used = false;
            while ((r = auth_reply(s, methods, sizeof methods)) == 2)
                if (!answer_info(s, &used)) {
                    fail(s, N_("No se entró."), NULL);
                    return false;
                }
            crypto_wipe(s->pw, sizeof s->pw);
        }
        if (r == 1) return true;
        if (r < 0) return false;
    }
    fail(s, N_("La contraseña no anduvo tres veces."), NULL);
    return false;
}

/* ---- the channels ---- */

static bool open_channel(sc_sess_t *s, uint32_t id)
{
    wbuf_t b = pk_begin(s, MSG_CHANNEL_OPEN);
    wb_cstr(&b, "session");
    wb_u32(&b, id);
    wb_u32(&b, WIN);
    wb_u32(&b, MAXPKT);
    return pk_send(s, &b);
}

static bool chan_request(sc_sess_t *s, int i, const char *type, bool want, wbuf_t *b)
{
    *b = pk_begin(s, MSG_CHANNEL_REQUEST);
    wb_u32(b, s->ch[i].peer);
    wb_cstr(b, type);
    wb_byte(b, want);
    return true;
}

static bool start_shell(sc_sess_t *s)
{
    if (!open_channel(s, 0)) return false;
    for (;;) {
        if (pk_wait(s) < 0) return false;
        uint8_t t = s->pkt[0];
        rbuf_t r = { s->pkt, s->pkt_len, 1, false };
        if (t == MSG_GLOBAL_REQUEST) {
            size_t n;
            rb_string(&r, &n);
            if (rb_bool(&r)) {
                wbuf_t b = pk_begin(s, MSG_REQUEST_FAILURE);
                if (!pk_send(s, &b)) return false;
            }
            continue;
        }
        if (t == MSG_CHANNEL_OPEN_FAILURE) {
            fail(s, N_("El equipo no abrió una sesión."), NULL);
            return false;
        }
        if (t != MSG_CHANNEL_OPEN_CONFIRMATION) continue;
        if (rb_u32(&r) != 0) continue;
        s->ch[0].peer = rb_u32(&r);
        s->ch[0].peer_win = rb_u32(&r);
        s->ch[0].peer_max = rb_u32(&r);
        s->ch[0].open = true;
        break;
    }
    wbuf_t b;
    chan_request(s, 0, "pty-req", false, &b);
    wb_cstr(&b, "xterm-256color");
    wb_u32(&b, (uint32_t)s->cols);
    wb_u32(&b, (uint32_t)s->rows);
    wb_u32(&b, 0);
    wb_u32(&b, 0);
    /* the terminal's modes: Backspace is DEL, and the input is UTF-8 */
    uint8_t modes[] = { 3, 0, 0, 0, 127, 42, 0, 0, 0, 1, 0 };
    wb_string(&b, modes, sizeof modes);
    if (!pk_send(s, &b)) return false;
    chan_request(s, 0, "env", false, &b);
    wb_cstr(&b, "LANG");
    wb_cstr(&b, "en_US.UTF-8");
    if (!pk_send(s, &b)) return false;
    chan_request(s, 0, "shell", true, &b);
    return pk_send(s, &b);
}

static void send_data(sc_sess_t *s, int i, const uint8_t *d, size_t n)
{
    chan_t *c = &s->ch[i];
    while (n && c->peer_win && !closed(s)) {
        size_t m = n;
        if (m > c->peer_win) m = c->peer_win;
        if (m > c->peer_max) m = c->peer_max;
        if (m > 16384) m = 16384;
        wbuf_t b = pk_begin(s, MSG_CHANNEL_DATA);
        wb_u32(&b, c->peer);
        wb_string(&b, d, m);
        if (!pk_send(s, &b)) return;
        c->peer_win -= (uint32_t)m;
        d += m;
        n -= m;
    }
}

static void install_command(sc_sess_t *s)
{
    char line[200];
    if (!sc_key_line(line, sizeof line)) return;
    /* the base64 part, to see whether it is there already */
    char b64[100] = "";
    const char *sp = strchr(line, ' ');
    if (sp) {
        const char *e = strchr(sp + 1, ' ');
        size_t l = e ? (size_t)(e - sp - 1) : strlen(sp + 1);
        if (l < sizeof b64) {
            memcpy(b64, sp + 1, l);
            b64[l] = 0;
        }
    }
    char *cmd = malloc(800);
    if (!cmd) return;
    snprintf(cmd, 800,
             "umask 077; mkdir -p ~/.ssh && touch ~/.ssh/authorized_keys && "
             "{ grep -qF '%s' ~/.ssh/authorized_keys || echo '%s' >> ~/.ssh/authorized_keys; }",
             b64, line);
    wbuf_t b;
    chan_request(s, 1, "exec", true, &b);
    wb_cstr(&b, cmd);
    free(cmd);
    pk_send(s, &b);
}

static void note(sc_sess_t *s, const char *key)
{
    lock(s);
    s->note = key;
    unlock(s);
}

/* One packet of the live session. */
static void live_packet(sc_sess_t *s)
{
    uint8_t t = s->pkt[0];
    rbuf_t r = { s->pkt, s->pkt_len, 1, false };
    if (t == MSG_KEXINIT) {
        if (send_kexinit(s)) kex(s);
        return;
    }
    if (t == MSG_GLOBAL_REQUEST) {
        size_t n;
        rb_string(&r, &n);
        if (rb_bool(&r)) {
            wbuf_t b = pk_begin(s, MSG_REQUEST_FAILURE);
            pk_send(s, &b);
        }
        return;
    }
    if (t == MSG_CHANNEL_OPEN_CONFIRMATION) {
        uint32_t id = rb_u32(&r);
        if (id != 1) return;
        s->ch[1].peer = rb_u32(&r);
        s->ch[1].peer_win = rb_u32(&r);
        s->ch[1].peer_max = rb_u32(&r);
        s->ch[1].open = true;
        install_command(s);
        return;
    }
    if (t == MSG_CHANNEL_OPEN_FAILURE) {
        if (rb_u32(&r) == 1) note(s, N_("El equipo no abrió un segundo canal para instalar la clave."));
        return;
    }
    if (t < MSG_CHANNEL_WINDOW_ADJUST || t > MSG_CHANNEL_FAILURE) return;
    uint32_t id = rb_u32(&r);
    if (id > 1 || !s->ch[id].open) return;
    chan_t *c = &s->ch[id];
    switch (t) {
    case MSG_CHANNEL_WINDOW_ADJUST: {
        uint64_t w = (uint64_t)c->peer_win + rb_u32(&r);
        c->peer_win = w > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)w;
        break;
    }
    case MSG_CHANNEL_DATA:
    case MSG_CHANNEL_EXTENDED_DATA: {
        if (t == MSG_CHANNEL_EXTENDED_DATA) rb_u32(&r);
        size_t n;
        const uint8_t *d = rb_string(&r, &n);
        if (!d) break;
        if (id == 0) {
            sc_term_lock(s->term);
            sc_term_feed(s->term, d, n);
            sc_term_unlock(s->term);
        }
        c->consumed += (uint32_t)n;
        if (c->consumed >= WIN / 2) {
            wbuf_t b = pk_begin(s, MSG_CHANNEL_WINDOW_ADJUST);
            wb_u32(&b, c->peer);
            wb_u32(&b, c->consumed);
            pk_send(s, &b);
            c->consumed = 0;
        }
        break;
    }
    case MSG_CHANNEL_REQUEST: {
        char type[32];
        rb_cstr(&r, type, sizeof type);
        bool want = rb_bool(&r);
        if (id == 1 && !strcmp(type, "exit-status"))
            note(s, rb_u32(&r) == 0 ? N_("La clave de la placa quedó en ~/.ssh/authorized_keys: la próxima vez entra sin contraseña.")
                                    : N_("No se pudo instalar la clave (¿no es un Unix?)."));
        if (want) {
            wbuf_t b = pk_begin(s, MSG_CHANNEL_FAILURE);
            wb_u32(&b, c->peer);
            pk_send(s, &b);
        }
        break;
    }
    case MSG_CHANNEL_FAILURE:
        if (id == 1) note(s, N_("El equipo no dejó correr el comando para instalar la clave."));
        else fail(s, N_("El equipo no abrió una consola."), NULL);
        break;
    case MSG_CHANNEL_EOF:
        break;
    case MSG_CHANNEL_CLOSE:
        if (!c->closing) {
            wbuf_t b = pk_begin(s, MSG_CHANNEL_CLOSE);
            wb_u32(&b, c->peer);
            pk_send(s, &b);
        }
        c->open = false;
        if (id == 0) fail(s, N_("La sesión terminó."), NULL);
        break;
    default:
        break;
    }
}

static void live(sc_sess_t *s)
{
    set_state(s, SC_ST_LIVE);
    aos_hal_log(SC_TAG, "%s: live (%s, %s)", s->host.host, s->hk_alg, s->strict ? "strict kex" : "kex");
    uint8_t *tx = malloc(TXQ_CAP);
    if (!tx) return;
    while (!closed(s)) {
        /* what was typed, the terminal's answers, a new size, the key */
        lock(s);
        size_t n = s->txn;
        memcpy(tx, s->txq, n);
        s->txn = 0;
        bool resize = s->want_resize, install = s->want_install;
        int cols = s->want_cols, rows = s->want_rows;
        s->want_resize = s->want_install = false;
        unlock(s);
        sc_term_lock(s->term);
        n += sc_term_take_reply(s->term, tx + n, TXQ_CAP - n);
        sc_term_unlock(s->term);
        if (n) send_data(s, 0, tx, n);
        if (resize) {
            s->cols = cols;
            s->rows = rows;
            wbuf_t b;
            chan_request(s, 0, "window-change", false, &b);
            wb_u32(&b, (uint32_t)cols);
            wb_u32(&b, (uint32_t)rows);
            wb_u32(&b, 0);
            wb_u32(&b, 0);
            pk_send(s, &b);
        }
        if (install && !s->ch[1].open) open_channel(s, 1);
        int r = pk_recv(s, 30);
        while (r > 0 && !closed(s)) {
            if (s->pkt[0] == MSG_DISCONNECT) {
                rbuf_t r2 = { s->pkt, s->pkt_len, 5, false };
                char why[120];
                rb_cstr(&r2, why, sizeof why);
                fail(s, N_("El equipo cortó: %s"), why);
                break;
            }
            live_packet(s);
            r = pk_recv(s, 0);
        }
    }
    free(tx);
}

static void sess_thread(void *arg)
{
    sc_sess_t *s = arg;
    s->fd = aos_hal_tcp_connect(s->host.host, s->host.port, CONNECT_MS);
    if (s->fd <= 0) {
        fail(s, s->fd == AOS_TCP_ERR_DNS ? N_("No se encontró %s.") : N_("No se pudo conectar con %s."), s->host.host);
        s->done = true;
        return;
    }
    char vc[40];
    snprintf(vc, sizeof vc, "%s\r\n", V_C);
    bool ok = aos_hal_tcp_send(s->fd, vc, (int)strlen(vc), 5000) == (int)strlen(vc);
    /* the server's version line, after any lines it says first */
    for (int lines = 0; ok && lines < 30;) {
        uint8_t *nl = memchr(s->in, '\n', s->in_len);
        if (nl) {
            size_t l = (size_t)(nl - s->in), ll = l && s->in[l - 1] == '\r' ? l - 1 : l;
            bool ver = ll >= 4 && !memcmp(s->in, "SSH-", 4);
            if (ver) {
                if (ll >= sizeof s->v_s || (memcmp(s->in, "SSH-2.0-", 8) && memcmp(s->in, "SSH-1.99-", 9))) ok = false;
                else {
                    memcpy(s->v_s, s->in, ll);
                    s->v_s[ll] = 0;
                }
            }
            memmove(s->in, nl + 1, s->in_len - l - 1);
            s->in_len -= l + 1;
            if (ver) break;
            lines++;
            continue;
        }
        if (s->in_len >= 255 || s->stop) ok = false;
        int k = ok ? aos_hal_tcp_recv(s->fd, s->in + s->in_len, (int)(BUF_CAP - s->in_len), 8000) : -1;
        if (k <= 0) ok = false;
        else s->in_len += (size_t)k;
    }
    if (!ok || !s->v_s[0]) fail(s, N_("%s no contestó como un servidor SSH."), s->host.host);
    else aos_hal_log(SC_TAG, "%s: %s", s->host.host, s->v_s);

    if (!closed(s) && send_kexinit(s) && pk_wait(s) > 0) {
        if (s->pkt[0] != MSG_KEXINIT) fail(s, N_("El equipo mandó algo que no es SSH."), NULL);
        else if (kex(s) && login(s) && start_shell(s)) live(s);
    }
    if (s->stop && s->fd > 0 && s->tx_on) {
        wbuf_t b = pk_begin(s, MSG_DISCONNECT);
        wb_u32(&b, 11);
        wb_cstr(&b, "closed on the board");
        wb_cstr(&b, "");
        pk_send(s, &b);
    }
    fail(s, N_("La sesión terminó."), NULL);
    aos_hal_tcp_close(s->fd);
    s->fd = 0;
    s->done = true;
}

/* ---- the UI's side ---- */

sc_sess_t *sc_sess_start(const sc_host_t *h, sc_term_t *term)
{
    sc_sess_t *s = sc_psram(sizeof *s);
    if (!s) return NULL;
    s->host = *h;
    s->term = term;
    s->mx = aos_hal_mutex_create();
    s->in = sc_psram(BUF_CAP);
    s->out = sc_psram(BUF_CAP);
    s->pkt = sc_psram(BUF_CAP);
    s->first_kex = true;
    s->state = SC_ST_CONNECTING;
    sc_term_lock(term);
    sc_term_size(term, &s->cols, &s->rows);
    sc_term_unlock(term);
    if (!s->mx || !s->in || !s->out || !s->pkt || !aos_hal_thread_start("sshc", sess_thread, s, 16384, 3)) {
        free(s->in);
        free(s->out);
        free(s->pkt);
        free(s);
        return NULL;
    }
    return s;
}

void sc_sess_stop(sc_sess_t *s)
{
    if (!s) return;
    s->stop = true;
    /* the thread runs code of this .so: it must be out before the app goes
     * (HANDOFF-APPS, "Trampas"); every wait in it is short */
    for (int i = 0; i < 170 && !s->done; i++) aos_hal_sleep_ms(50);   /* the connect's 6 s at most */
    if (!s->done) {
        aos_hal_log(SC_TAG, "the session's thread did not stop; its memory is left");
        return;
    }
    free(s->in);
    free(s->out);
    free(s->pkt);
    free(s->i_c);
    free(s->i_s);
    free(s->hk_blob);
    ssh_chachapoly_wipe(&s->rx);
    ssh_chachapoly_wipe(&s->tx);
    crypto_wipe(s->pw, sizeof s->pw);
    free(s);
}

sc_state_t sc_sess_state(sc_sess_t *s, char *detail, size_t n)
{
    lock(s);
    sc_state_t st = s->state;
    if (detail && n) {
        if (s->why) snprintf(detail, n, _(s->why), s->why_arg);
        else detail[0] = 0;
    }
    unlock(s);
    return st;
}

uint32_t sc_sess_ask_seq(sc_sess_t *s)
{
    lock(s);
    uint32_t n = s->ask_seq;
    unlock(s);
    return n;
}

void sc_sess_hostkey(sc_sess_t *s, sc_hostkey_q_t *q)
{
    lock(s);
    *q = s->hk;
    unlock(s);
}

void sc_sess_answer_hostkey(sc_sess_t *s, bool yes)
{
    lock(s);
    s->hk_answer = yes;
    unlock(s);
}

void sc_sess_password_prompt(sc_sess_t *s, char *out, size_t n, bool *retry)
{
    lock(s);
    sc_copy(out, n, s->pw_prompt);
    if (retry) *retry = s->pw_retry;
    unlock(s);
}

void sc_sess_answer_password(sc_sess_t *s, const char *pw)
{
    lock(s);
    if (pw) sc_copy(s->pw, sizeof s->pw, pw);
    s->pw_answer = pw ? 1 : 0;
    unlock(s);
}

void sc_sess_send(sc_sess_t *s, const void *d, size_t n)
{
    lock(s);
    if (s->txn + n <= TXQ_CAP) {
        memcpy(s->txq + s->txn, d, n);
        s->txn += n;
    }
    unlock(s);
}

void sc_sess_resize(sc_sess_t *s, int cols, int rows)
{
    lock(s);
    s->want_cols = cols;
    s->want_rows = rows;
    s->want_resize = true;
    unlock(s);
}

void sc_sess_install_key(sc_sess_t *s)
{
    lock(s);
    s->want_install = true;
    unlock(s);
}

bool sc_sess_key_used(sc_sess_t *s)
{
    lock(s);
    bool k = s->key_used;
    unlock(s);
    return k;
}

const char *sc_sess_take_note(sc_sess_t *s)
{
    lock(s);
    const char *n = s->note;
    s->note = NULL;
    unlock(s);
    return n;
}
