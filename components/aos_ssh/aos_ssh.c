/*
 * P4OS - the SSH server: the listener, one thread per connection, and the
 * three layers of SSH-2 a console needs.
 *
 *   transport (RFC 4253)    version lines, KEXINIT, curve25519-sha256,
 *                           NEWKEYS, AES-GCM or ChaCha20-Poly1305 packets; the client may
 *                           exchange keys again at any time. "Strict KEX"
 *                           (OpenSSH's answer to Terrapin) when the client
 *                           offers it: sequence numbers start over at every
 *                           NEWKEYS and nothing but key exchange is allowed
 *                           before the first.
 *   authentication (4252)   none, password, publickey, as aos_access says
 *                           for the network the connection came in on
 *   connection (4254)       up to two session channels: pty-req, shell,
 *                           exec, window-change. No port forwarding, no
 *                           agent, no X11, no SFTP yet.
 *
 * Everything of a connection happens in its own thread (PSRAM stack): it
 * reads packets as they come and, between them, lets each channel's shell
 * run and sends what it wrote as far as the client's window allows.
 * Memory per connection: two packet buffers of ~35 KB and the shells' own,
 * all PSRAM. Three connections at most.
 */
#include "aos_ssh.h"
#include "aos_ssh_int.h"
#include "aos_access.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include "mbedtls/base64.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

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

#define TAG "ssh"

#define MAX_CONN      3
#define MAX_CH        2
#define PKT_MAX       35000         /* the most a packet may be (RFC 4253, 6.1) */
#define IN_CAP        (PKT_MAX + 4 + 16 + 64)
#define OUT_CAP       (PKT_MAX + 4 + 16 + 64)
#define WIN_SIZE      (128 * 1024)  /* what the client may send before we ask for more */
#define OUR_MAXPKT    32768
#define DATA_CHUNK    16384         /* channel data per packet, at most */
#define GRACE_MS      120000        /* to log in, as OpenSSH's LoginGraceTime */
#define AUTH_TRIES    6
#define ASK_TIMEOUT_S 60

enum {
    MSG_DISCONNECT = 1, MSG_IGNORE, MSG_UNIMPLEMENTED, MSG_DEBUG, MSG_SERVICE_REQUEST, MSG_SERVICE_ACCEPT,
    MSG_EXT_INFO,
    MSG_KEXINIT = 20, MSG_NEWKEYS,
    MSG_KEX_ECDH_INIT = 30, MSG_KEX_ECDH_REPLY,
    MSG_USERAUTH_REQUEST = 50, MSG_USERAUTH_FAILURE, MSG_USERAUTH_SUCCESS, MSG_USERAUTH_BANNER,
    MSG_USERAUTH_PK_OK = 60,
    MSG_GLOBAL_REQUEST = 80, MSG_REQUEST_SUCCESS, MSG_REQUEST_FAILURE,
    MSG_CHANNEL_OPEN = 90, MSG_CHANNEL_OPEN_CONFIRMATION, MSG_CHANNEL_OPEN_FAILURE, MSG_CHANNEL_WINDOW_ADJUST,
    MSG_CHANNEL_DATA, MSG_CHANNEL_EXTENDED_DATA, MSG_CHANNEL_EOF, MSG_CHANNEL_CLOSE, MSG_CHANNEL_REQUEST,
    MSG_CHANNEL_SUCCESS, MSG_CHANNEL_FAILURE,
};

/* reasons for MSG_DISCONNECT (RFC 4250, 4.2.2) */
enum { DIS_PROTOCOL = 2, DIS_KEX_FAILED = 3, DIS_MAC = 5, DIS_SERVICE = 7, DIS_BY_APP = 11, DIS_NO_AUTH = 14 };

static const char KEX_ALGS[] = "curve25519-sha256,curve25519-sha256@libssh.org,ext-info-s,kex-strict-s-v00@openssh.com";
static const char HOSTKEY_ALGS[] = "ssh-ed25519";
/* the client's order decides: OpenSSH puts ChaCha20 first, the board's
 * client app speaks only that one */
static const char CIPHERS[] = "aes128-gcm@openssh.com,aes256-gcm@openssh.com,chacha20-poly1305@openssh.com";
/* with an AEAD cipher the MAC is not used; some clients want a match all the same */
static const char MACS[] = "hmac-sha2-256";

typedef struct {
    bool used;
    uint32_t peer_id, peer_win, peer_maxpkt;
    uint32_t our_win, consumed;
    bool started, close_sent;
    bool pty;
    int cols, rows;
    char term[32];
    sh_t *sh;
} chan_t;

typedef struct conn {
    int fd;
    char from[48], user[32];
    aos_zone_t zone;
    unsigned gen;
    uint32_t t0, since_s;
    bool dead;

    char v_c[256];
    uint8_t *i_c, *i_s;
    size_t i_c_len, i_s_len;
    bool in_kex;                /* our KEXINIT sent, client's NEWKEYS not yet */
    bool got_kexinit;
    bool first_kex, strict, ext_info, ignore_next;
    char enc_c2s[32], enc_s2c[32];  /* the ciphers chosen, by name */
    uint8_t sid[32];
    bool have_sid;
    ssh_cipher_t *rx, *tx, *rx_next;
    uint32_t seq_rx, seq_tx;

    uint8_t *in, *out;
    size_t in_len;

    bool userauth, authed, asked;
    const char *how;
    int tries;
    chan_t ch[MAX_CH];
} conn_t;

static void *s_mx;
static conn_t *s_conns[MAX_CONN];
static int s_port;

/* ---- sending packets ---- */

/* the payload goes after the length and the padding length */
static wbuf_t pk_begin(conn_t *c, uint8_t type)
{
    wbuf_t b = { c->out + 5, 0, PKT_MAX - 64, false };
    wb_byte(&b, type);
    return b;
}

static bool send_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static bool pk_send(conn_t *c, wbuf_t *b)
{
    if (c->dead) return false;
    if (b->bad) {
        aos_hal_log(TAG, "%s: a message did not fit, dropped", c->from);
        return false;
    }
    /* GCM: the length is not encrypted and does not count for the block;
     * in clear (before the first NEWKEYS) everything does, in blocks of 8 */
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
        if (!ssh_cipher_seal(c->tx, o, L, c->seq_tx)) {
            c->dead = true;
            return false;
        }
        total += 16;
    }
    c->seq_tx++;
    if (!send_all(c->fd, o, total)) {
        c->dead = true;
        return false;
    }
    return true;
}

static void disconnect(conn_t *c, uint32_t reason, const char *why)
{
    if (c->dead) return;
    aos_hal_log(TAG, "%s: closing: %s", c->from, why);
    wbuf_t b = pk_begin(c, MSG_DISCONNECT);
    wb_u32(&b, reason);
    wb_cstr(&b, why);
    wb_cstr(&b, "");
    pk_send(c, &b);
    c->dead = true;
}

static void banner(conn_t *c, const char *text)
{
    wbuf_t b = pk_begin(c, MSG_USERAUTH_BANNER);
    char t[300];
    snprintf(t, sizeof t, "%s\r\n", text);
    wb_cstr(&b, t);
    wb_cstr(&b, "");
    pk_send(c, &b);
}

/* ---- key exchange ---- */

/* our version line without its CRLF; the exchange hash takes it as sent */
static void our_version(char *out, size_t n)
{
    snprintf(out, n, "SSH-2.0-P4OS_%s", aos_hal_firmware_version());
    for (char *p = out + 13; *p; p++)
        if (*p == ' ' || *p == '-' || *p < 0x21 || *p > 0x7e) *p = '_';
}

static bool send_kexinit(conn_t *c)
{
    wbuf_t b = pk_begin(c, MSG_KEXINIT);
    uint8_t cookie[16];
    ssh_random(cookie, sizeof cookie);
    wb_bytes(&b, cookie, 16);
    wb_cstr(&b, KEX_ALGS);
    wb_cstr(&b, HOSTKEY_ALGS);
    wb_cstr(&b, CIPHERS);
    wb_cstr(&b, CIPHERS);
    wb_cstr(&b, MACS);
    wb_cstr(&b, MACS);
    wb_cstr(&b, "none");
    wb_cstr(&b, "none");
    wb_cstr(&b, "");
    wb_cstr(&b, "");
    wb_byte(&b, 0);             /* no guessed packet follows */
    wb_u32(&b, 0);
    if (b.bad) return false;
    free(c->i_s);
    c->i_s = malloc(b.len);
    if (!c->i_s) return false;
    memcpy(c->i_s, b.p, b.len);
    c->i_s_len = b.len;
    c->in_kex = true;
    return pk_send(c, &b);
}

/* The first name in the client's list that is also in ours (RFC 4253,
 * 7.1: the client's order decides). Copied into out; false if none. */
static bool choose(const uint8_t *list, size_t n, const char *ours, char *out, size_t on)
{
    size_t i = 0;
    while (list && i < n) {
        size_t j = i;
        while (j < n && list[j] != ',') j++;
        if (j > i && j - i < on) {
            memcpy(out, list + i, j - i);
            out[j - i] = 0;
            if (namelist_has((const uint8_t *)ours, strlen(ours), out) && strcmp(out, "ext-info-s") &&
                strcmp(out, "kex-strict-s-v00@openssh.com"))
                return true;
        }
        i = j + 1;
    }
    out[0] = 0;
    return false;
}

static bool first_is(const uint8_t *list, size_t n, const char *name)
{
    size_t l = strlen(name);
    return n >= l && !memcmp(list, name, l) && (n == l || list[l] == ',');
}

static void on_kexinit(conn_t *c, const uint8_t *pay, size_t n, uint32_t seq)
{
    if (c->got_kexinit) {
        disconnect(c, DIS_PROTOCOL, "KEXINIT twice");
        return;
    }
    rbuf_t r = { pay, n, 1 + 16, false };
    const uint8_t *l[10];
    size_t ln[10];
    for (int i = 0; i < 10; i++) l[i] = rb_string(&r, &ln[i]);
    bool follows = rb_bool(&r);
    rb_u32(&r);
    if (r.bad || n < 17) {
        disconnect(c, DIS_PROTOCOL, "bad KEXINIT");
        return;
    }
    if (c->first_kex) {
        c->strict = namelist_has(l[0], ln[0], "kex-strict-c-v00@openssh.com");
        c->ext_info = namelist_has(l[0], ln[0], "ext-info-c");
        if (c->strict && seq != 0) {
            disconnect(c, DIS_PROTOCOL, "strict KEX: KEXINIT was not the first packet");
            return;
        }
    }
    char kex[40], hk[24], comp[8];
    char *e1 = c->enc_c2s, *e2 = c->enc_s2c;
    if (!choose(l[0], ln[0], KEX_ALGS, kex, sizeof kex) || !choose(l[1], ln[1], HOSTKEY_ALGS, hk, sizeof hk) ||
        !choose(l[2], ln[2], CIPHERS, e1, sizeof c->enc_c2s) || !choose(l[3], ln[3], CIPHERS, e2, sizeof c->enc_s2c) ||
        !choose(l[6], ln[6], "none", comp, sizeof comp) || !choose(l[7], ln[7], "none", comp, sizeof comp)) {
        aos_hal_log(TAG, "%s: no algorithm in common (the board: %s / %s / %s)", c->from, KEX_ALGS, HOSTKEY_ALGS, CIPHERS);
        disconnect(c, DIS_KEX_FAILED, "no algorithm in common");
        return;
    }
    /* a guess that did not come true: its packet is skipped (RFC 4253, 7) */
    c->ignore_next = follows && !(first_is(l[0], ln[0], kex) && first_is(l[1], ln[1], hk));
    free(c->i_c);
    c->i_c = malloc(n);
    if (!c->i_c) {
        c->dead = true;
        return;
    }
    memcpy(c->i_c, pay, n);
    c->i_c_len = n;
    c->got_kexinit = true;
    if (!c->in_kex) send_kexinit(c);    /* the client started a new exchange */
}

/* HASH(K || H || letter || session_id), lengthened as RFC 4253, 7.2 says */
static void derive(conn_t *c, const uint8_t *kmp, size_t kn, const uint8_t h[32], char letter, uint8_t *out,
                   size_t need)
{
    uint8_t k1[32], k2[32];
    ssh_sha256_ctx_t *x = ssh_sha256_new();
    ssh_sha256_add(x, kmp, kn);
    ssh_sha256_add(x, h, 32);
    ssh_sha256_add(x, &letter, 1);
    ssh_sha256_add(x, c->sid, 32);
    ssh_sha256_end(x, k1);
    memcpy(out, k1, need < 32 ? need : 32);
    if (need > 32) {
        x = ssh_sha256_new();
        ssh_sha256_add(x, kmp, kn);
        ssh_sha256_add(x, h, 32);
        ssh_sha256_add(x, k1, 32);
        ssh_sha256_end(x, k2);
        memcpy(out + 32, k2, need - 32);
    }
    memset(k1, 0, sizeof k1);
    memset(k2, 0, sizeof k2);
}

static void hash_string(ssh_sha256_ctx_t *x, const void *d, size_t n)
{
    uint8_t l[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    ssh_sha256_add(x, l, 4);
    ssh_sha256_add(x, d, n);
}

static void on_ecdh_init(conn_t *c, const uint8_t *pay, size_t n)
{
    rbuf_t r = { pay, n, 1, false };
    size_t ql;
    const uint8_t *qc = rb_string(&r, &ql);
    if (!c->got_kexinit || r.bad || ql != 32) {
        disconnect(c, DIS_PROTOCOL, "bad KEX_ECDH_INIT");
        return;
    }
    uint8_t seed[32], pub[32], esk[32], epk[32], k[32];
    if (!aos_access_ssh_host_key(seed, pub)) {
        disconnect(c, DIS_KEX_FAILED, "the board has no host key");
        return;
    }
    ssh_x25519_keypair(esk, epk);
    bool ok = ssh_x25519(k, esk, qc);
    memset(esk, 0, sizeof esk);
    if (!ok) {
        memset(seed, 0, sizeof seed);
        disconnect(c, DIS_KEX_FAILED, "bad curve25519 point");
        return;
    }
    /* K as an mpint: the 32 bytes read as a big-endian number (RFC 8731, 3) */
    uint8_t kmp[4 + 33];
    wbuf_t kb = { kmp, 0, sizeof kmp, false };
    wb_mpint(&kb, k, 32);
    memset(k, 0, sizeof k);

    uint8_t ks[4 + 11 + 4 + 32];
    wbuf_t kw = { ks, 0, sizeof ks, false };
    wb_cstr(&kw, "ssh-ed25519");
    wb_string(&kw, pub, 32);

    uint8_t h[32];
    ssh_sha256_ctx_t *x = ssh_sha256_new();
    hash_string(x, c->v_c, strlen(c->v_c));
    char vs[80];
    our_version(vs, sizeof vs);
    hash_string(x, vs, strlen(vs));
    hash_string(x, c->i_c, c->i_c_len);
    hash_string(x, c->i_s, c->i_s_len);
    hash_string(x, ks, kw.len);
    hash_string(x, qc, 32);
    hash_string(x, epk, 32);
    ssh_sha256_add(x, kmp, kb.len);
    ssh_sha256_end(x, h);
    if (!c->have_sid) {
        memcpy(c->sid, h, 32);
        c->have_sid = true;
    }

    uint8_t iv_c[12] = { 0 }, iv_s[12] = { 0 }, key_c[64], key_s[64];
    if (ssh_cipher_iv_len(c->enc_c2s)) derive(c, kmp, kb.len, h, 'A', iv_c, 12);
    if (ssh_cipher_iv_len(c->enc_s2c)) derive(c, kmp, kb.len, h, 'B', iv_s, 12);
    derive(c, kmp, kb.len, h, 'C', key_c, (size_t)ssh_cipher_key_len(c->enc_c2s));
    derive(c, kmp, kb.len, h, 'D', key_s, (size_t)ssh_cipher_key_len(c->enc_s2c));
    memset(kmp, 0, sizeof kmp);
    ssh_cipher_free(c->rx_next);
    c->rx_next = ssh_cipher_new(c->enc_c2s, key_c, iv_c);
    ssh_cipher_t *tx_next = ssh_cipher_new(c->enc_s2c, key_s, iv_s);
    memset(key_c, 0, sizeof key_c);
    memset(key_s, 0, sizeof key_s);
    if (!c->rx_next || !tx_next) {
        ssh_cipher_free(tx_next);
        memset(seed, 0, sizeof seed);
        disconnect(c, DIS_KEX_FAILED, "out of memory");
        return;
    }

    wbuf_t b = pk_begin(c, MSG_KEX_ECDH_REPLY);
    wb_string(&b, ks, kw.len);
    wb_string(&b, epk, 32);
    ssh_host_sign(&b, seed, pub, h, 32);
    memset(seed, 0, sizeof seed);
    pk_send(c, &b);

    b = pk_begin(c, MSG_NEWKEYS);
    pk_send(c, &b);
    ssh_cipher_free(c->tx);
    c->tx = tx_next;
    if (c->strict) c->seq_tx = 0;

    /* which signatures the board takes for publickey: without it an OpenSSH
     * client with an RSA key would offer only ssh-rsa, SHA-1 (RFC 8308) */
    if (c->first_kex && c->ext_info) {
        b = pk_begin(c, MSG_EXT_INFO);
        wb_u32(&b, 1);
        wb_cstr(&b, "server-sig-algs");
        wb_cstr(&b, SSH_SIG_ALGS);
        pk_send(c, &b);
    }
}

static void on_newkeys(conn_t *c)
{
    if (!c->rx_next) {
        disconnect(c, DIS_PROTOCOL, "NEWKEYS before the exchange");
        return;
    }
    ssh_cipher_free(c->rx);
    c->rx = c->rx_next;
    c->rx_next = NULL;
    if (c->strict) c->seq_rx = 0;
    c->in_kex = c->got_kexinit = false;
    c->first_kex = false;
    free(c->i_c);
    free(c->i_s);
    c->i_c = c->i_s = NULL;
}

/* ---- authentication ---- */

static aos_access_t need_now(conn_t *c) { return aos_access_need(c->zone); }

static void auth_ok(conn_t *c, const char *how)
{
    wbuf_t b = pk_begin(c, MSG_USERAUTH_SUCCESS);
    pk_send(c, &b);
    c->authed = true;
    c->how = how;
    c->since_s = (uint32_t)(aos_hal_uptime_ms() / 1000);
    aos_hal_log(TAG, "%s: %s logged in (%s)", c->from, c->user, how);
}

static void auth_fail(conn_t *c, bool counts)
{
    if (counts && ++c->tries >= AUTH_TRIES) {
        disconnect(c, DIS_NO_AUTH, "too many tries");
        return;
    }
    wbuf_t b = pk_begin(c, MSG_USERAUTH_FAILURE);
    wb_cstr(&b, "publickey,password");
    wb_byte(&b, 0);
    pk_send(c, &b);
}

static bool alg_known(const char *alg)
{
    return !strcmp(alg, "ssh-ed25519") || !strcmp(alg, "ecdsa-sha2-nistp256") || !strcmp(alg, "rsa-sha2-256") ||
           !strcmp(alg, "rsa-sha2-512");
}

/* the key's type name inside its blob ("ssh-rsa" for both RSA signatures) */
static bool blob_type(const uint8_t *blob, size_t n, char *out, size_t on)
{
    rbuf_t r = { blob, n, 0, false };
    return rb_cstr(&r, out, on) && !r.bad;
}

static bool alg_fits(const char *alg, const char *type)
{
    if (!strncmp(alg, "rsa-sha2-", 9)) return !strcmp(type, "ssh-rsa");
    return !strcmp(alg, type);
}

static void auth_publickey(conn_t *c, rbuf_t *r, const uint8_t *user, size_t un, const char *service)
{
    bool has_sig = rb_bool(r);
    char alg[32];
    rb_cstr(r, alg, sizeof alg);
    size_t bn, sn = 0;
    const uint8_t *blob = rb_string(r, &bn), *sig = has_sig ? rb_string(r, &sn) : NULL;
    char type[32], kind[24];
    if (r->bad || !alg_known(alg) || !blob_type(blob, bn, type, sizeof type) || !alg_fits(alg, type) ||
        !ssh_key_describe(blob, bn, kind, sizeof kind)) {
        auth_fail(c, has_sig);
        return;
    }
    char *b64 = malloc(1600);
    size_t bl = 0;
    if (!b64 || mbedtls_base64_encode((unsigned char *)b64, 1600, &bl, blob, bn)) {
        free(b64);
        auth_fail(c, false);
        return;
    }
    b64[bl] = 0;
    bool known = aos_access_ssh_key_find(type, b64) >= 0;
    bool open = need_now(c) == AOS_ACCESS_OPEN;

    if (!has_sig) {
        /* "would this key do?": yes for one on the list, and for the first
         * unknown one, which may then ask the board */
        if (known || open || !c->asked) {
            wbuf_t b = pk_begin(c, MSG_USERAUTH_PK_OK);
            wb_cstr(&b, alg);
            wb_string(&b, blob, bn);
            pk_send(c, &b);
        } else auth_fail(c, false);
        free(b64);
        return;
    }

    /* what the client signed (RFC 4252, 7) */
    size_t mcap = bn + 256 + un + strlen(service);
    wbuf_t m = { malloc(mcap), 0, mcap, false };
    if (!m.p) {
        free(b64);
        auth_fail(c, false);
        return;
    }
    wb_string(&m, c->sid, 32);
    wb_byte(&m, MSG_USERAUTH_REQUEST);
    wb_string(&m, user, un);
    wb_cstr(&m, service);
    wb_cstr(&m, "publickey");
    wb_byte(&m, 1);
    wb_cstr(&m, alg);
    wb_string(&m, blob, bn);
    bool good = !m.bad && ssh_key_verify(alg, blob, bn, sig, sn, m.p, m.len);
    free(m.p);
    if (!good) {
        aos_hal_log(TAG, "%s: a %s signature that does not check", c->from, kind);
        free(b64);
        auth_fail(c, true);
        return;
    }
    if (known || open) {
        free(b64);
        auth_ok(c, known ? N_("clave") : N_("abierta"));
        return;
    }
    if (c->asked) {
        free(b64);
        auth_fail(c, true);
        return;
    }
    c->asked = true;
    aos_access_ssh_ask_t q = { 0 };
    snprintf(q.type, sizeof q.type, "%s", kind);
    snprintf(q.comment, sizeof q.comment, "%s", c->user);
    snprintf(q.from, sizeof q.from, "%s", c->from);
    size_t ll = strlen(type) + bl + strlen(c->user) + strlen(c->from) + 4;
    char *line = malloc(ll);
    if (line) snprintf(line, ll, "%s %s %s@%s", type, b64, c->user, c->from);
    free(b64);
    if (!line) {
        auth_fail(c, false);
        return;
    }
    aos_access_ssh_key_fingerprint(line, q.fp, sizeof q.fp);
    char msg[200];
    snprintf(msg, sizeof msg, _("Esta clave (%s %s) no está en la lista de la placa. Mirá su pantalla: tenés %d segundos para permitirla."),
             q.type, q.fp, ASK_TIMEOUT_S);
    banner(c, msg);
    int a = aos_access_ssh_ask(&q, line, ASK_TIMEOUT_S);
    free(line);
    if (a == 1) {
        auth_ok(c, N_("clave"));
        return;
    }
    banner(c, a < 0 ? _("La placa está preguntando por otra clave; probá de nuevo en un rato.")
                    : _("La placa no permitió la clave."));
    auth_fail(c, true);
}

static void on_userauth(conn_t *c, const uint8_t *pay, size_t n)
{
    if (c->authed) return;              /* RFC 4252, 5.1: ignored after success */
    rbuf_t r = { pay, n, 1, false };
    char service[32], method[32];
    size_t un;
    const uint8_t *user = rb_string(&r, &un);
    /* kept for the log and `who`, printable; the signature takes it as it came */
    size_t cn = un < sizeof c->user - 1 ? un : sizeof c->user - 1;
    for (size_t i = 0; i < cn; i++) c->user[i] = user[i] < 0x20 || user[i] == 0x7f ? '?' : (char)user[i];
    c->user[cn] = 0;
    rb_cstr(&r, service, sizeof service);
    rb_cstr(&r, method, sizeof method);
    if (r.bad || strcmp(service, "ssh-connection")) {
        disconnect(c, DIS_SERVICE, "only ssh-connection");
        return;
    }
    aos_access_t need = need_now(c);
    if (need == AOS_ACCESS_CLOSED) {
        disconnect(c, DIS_BY_APP, "closed on this network");
        return;
    }
    if (!strcmp(method, "publickey")) {
        auth_publickey(c, &r, user, un, service);
        return;
    }
    if (need == AOS_ACCESS_OPEN) {
        /* the portal opens without a password here, and so does this */
        auth_ok(c, N_("abierta"));
        return;
    }
    if (!strcmp(method, "password")) {
        bool change = rb_bool(&r);
        char pw[130];
        rb_cstr(&r, pw, sizeof pw);
        int wait_s = 0;
        bool ok = !r.bad && !change && aos_access_check_password(pw, &wait_s);
        memset(pw, 0, sizeof pw);
        if (ok) {
            auth_ok(c, N_("contraseña"));
            return;
        }
        if (wait_s > 0) {
            char msg[120];
            snprintf(msg, sizeof msg, _("Demasiados intentos: esperá %d s."), wait_s);
            banner(c, msg);
        } else aos_hal_log(TAG, "%s: wrong password for %s", c->from, c->user);
        auth_fail(c, true);
        return;
    }
    auth_fail(c, false);                /* none, keyboard-interactive...: what there is */
}

/* ---- channels ---- */

static chan_t *chan_of(conn_t *c, rbuf_t *r)
{
    uint32_t id = rb_u32(r);
    if (r->bad || id >= MAX_CH || !c->ch[id].used) return NULL;
    return &c->ch[id];
}

static void chan_reply(conn_t *c, chan_t *ch, bool ok)
{
    wbuf_t b = pk_begin(c, ok ? MSG_CHANNEL_SUCCESS : MSG_CHANNEL_FAILURE);
    wb_u32(&b, ch->peer_id);
    pk_send(c, &b);
}

static void on_channel_open(conn_t *c, const uint8_t *pay, size_t n)
{
    rbuf_t r = { pay, n, 1, false };
    char type[32];
    rb_cstr(&r, type, sizeof type);
    uint32_t peer = rb_u32(&r), win = rb_u32(&r), maxpkt = rb_u32(&r);
    if (r.bad) {
        disconnect(c, DIS_PROTOCOL, "bad CHANNEL_OPEN");
        return;
    }
    int id = -1;
    for (int i = 0; i < MAX_CH && id < 0; i++)
        if (!c->ch[i].used) id = i;
    if (strcmp(type, "session") || id < 0 || maxpkt < 64) {
        wbuf_t b = pk_begin(c, MSG_CHANNEL_OPEN_FAILURE);
        wb_u32(&b, peer);
        wb_u32(&b, id < 0 ? 4 : 1);     /* resource shortage, administratively prohibited */
        wb_cstr(&b, id < 0 ? "too many channels" : "only session channels");
        wb_cstr(&b, "");
        pk_send(c, &b);
        if (strcmp(type, "session")) aos_hal_log(TAG, "%s: refused a \"%s\" channel", c->from, type);
        return;
    }
    chan_t *ch = &c->ch[id];
    memset(ch, 0, sizeof *ch);
    ch->used = true;
    ch->peer_id = peer;
    ch->peer_win = win;
    ch->peer_maxpkt = maxpkt;
    ch->our_win = WIN_SIZE;
    ch->cols = 80;
    ch->rows = 24;
    wbuf_t b = pk_begin(c, MSG_CHANNEL_OPEN_CONFIRMATION);
    wb_u32(&b, peer);
    wb_u32(&b, (uint32_t)id);
    wb_u32(&b, WIN_SIZE);
    wb_u32(&b, OUR_MAXPKT);
    pk_send(c, &b);
}

static sh_t *chan_shell(conn_t *c, chan_t *ch)
{
    sh_opts_t o = { c->user, c->from, ch->pty, ch->cols, ch->rows, ch->term };
    return sh_new(&o);
}

static void on_channel_request(conn_t *c, const uint8_t *pay, size_t n)
{
    rbuf_t r = { pay, n, 1, false };
    chan_t *ch = chan_of(c, &r);
    char type[40];
    rb_cstr(&r, type, sizeof type);
    bool want = rb_bool(&r);
    if (!ch || r.bad) return;
    bool ok = false;
    if (!strcmp(type, "pty-req")) {
        rb_cstr(&r, ch->term, sizeof ch->term);
        uint32_t cols = rb_u32(&r), rows = rb_u32(&r);
        if (!r.bad && !ch->started) {
            ch->pty = true;
            ch->cols = cols >= 10 && cols <= 1000 ? (int)cols : 80;
            ch->rows = rows >= 2 && rows <= 1000 ? (int)rows : 24;
            ok = true;
        }
    } else if (!strcmp(type, "shell") || !strcmp(type, "exec")) {
        char *cmd = NULL;
        if (type[0] == 'e') {
            size_t cl;
            const uint8_t *cp = rb_string(&r, &cl);
            cmd = cp && cl < 4096 ? malloc(cl + 1) : NULL;
            if (cmd) {
                memcpy(cmd, cp, cl);
                cmd[cl] = 0;
            }
        }
        if (!ch->started && (type[0] == 's' || cmd) && (ch->sh = chan_shell(c, ch))) {
            ch->started = true;
            ok = true;
            if (cmd) {
                aos_hal_log(TAG, "%s: exec \"%.60s\"", c->from, cmd);
                sh_start_exec(ch->sh, cmd);
            } else sh_start_shell(ch->sh);
        }
        free(cmd);
    } else if (!strcmp(type, "window-change")) {
        uint32_t cols = rb_u32(&r), rows = rb_u32(&r);
        if (!r.bad && cols >= 10 && cols <= 1000 && rows >= 2 && rows <= 1000) {
            ch->cols = (int)cols;
            ch->rows = (int)rows;
            if (ch->sh) sh_resize(ch->sh, ch->cols, ch->rows);
        }
        ok = true;
    } else if (!strcmp(type, "subsystem")) {
        char sub[24];
        rb_cstr(&r, sub, sizeof sub);
        aos_hal_log(TAG, "%s: subsystem \"%s\" refused (not yet)", c->from, sub);
    }
    if (want) chan_reply(c, ch, ok);
}

static void on_channel_data(conn_t *c, const uint8_t *pay, size_t n, bool extended)
{
    rbuf_t r = { pay, n, 1, false };
    chan_t *ch = chan_of(c, &r);
    if (extended) rb_u32(&r);
    size_t dl;
    const uint8_t *d = rb_string(&r, &dl);
    if (!ch || r.bad) return;
    if (dl > ch->our_win) {
        disconnect(c, DIS_PROTOCOL, "data past the window");
        return;
    }
    ch->our_win -= (uint32_t)dl;
    ch->consumed += (uint32_t)dl;
    if (!extended && ch->sh) sh_input(ch->sh, d, dl);
    if (ch->consumed >= WIN_SIZE / 2) {
        wbuf_t b = pk_begin(c, MSG_CHANNEL_WINDOW_ADJUST);
        wb_u32(&b, ch->peer_id);
        wb_u32(&b, ch->consumed);
        pk_send(c, &b);
        ch->our_win += ch->consumed;
        ch->consumed = 0;
    }
}

static void chan_free(conn_t *c, chan_t *ch)
{
    (void)c;
    if (ch->sh) sh_free(ch->sh);
    memset(ch, 0, sizeof *ch);
}

static void on_channel_misc(conn_t *c, uint8_t type, const uint8_t *pay, size_t n)
{
    rbuf_t r = { pay, n, 1, false };
    chan_t *ch = chan_of(c, &r);
    if (!ch) return;
    if (type == MSG_CHANNEL_WINDOW_ADJUST) {
        uint32_t add = rb_u32(&r);
        uint64_t w = (uint64_t)ch->peer_win + add;
        ch->peer_win = w > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)w;
    } else if (type == MSG_CHANNEL_EOF) {
        if (ch->sh) sh_eof(ch->sh);
    } else if (type == MSG_CHANNEL_CLOSE) {
        if (!ch->close_sent) {
            wbuf_t b = pk_begin(c, MSG_CHANNEL_CLOSE);
            wb_u32(&b, ch->peer_id);
            pk_send(c, &b);
        }
        chan_free(c, ch);
    }
}

/* Let each shell run and send what it wrote; when one has ended, its exit
 * status, EOF and CLOSE. Whether one of them wants to run again at once. */
static bool pump(conn_t *c)
{
    bool busy = false;
    for (int i = 0; i < MAX_CH && !c->dead; i++) {
        chan_t *ch = &c->ch[i];
        if (!ch->used || !ch->sh || ch->close_sent) continue;
        sh_set_blocked(ch->sh, c->in_kex || ch->peer_win == 0);
        int st = sh_poll(ch->sh);
        bool alive = st != SH_DONE;
        const uint8_t *p;
        size_t k;
        while (!c->in_kex && !c->dead && ch->peer_win && (k = sh_out_peek(ch->sh, &p))) {
            uint32_t m = (uint32_t)k;
            if (m > ch->peer_win) m = ch->peer_win;
            if (m > ch->peer_maxpkt) m = ch->peer_maxpkt;
            if (m > DATA_CHUNK) m = DATA_CHUNK;
            wbuf_t b = pk_begin(c, MSG_CHANNEL_DATA);
            wb_u32(&b, ch->peer_id);
            wb_string(&b, p, m);
            if (!pk_send(c, &b)) break;
            sh_out_take(ch->sh, m);
            ch->peer_win -= m;
        }
        if (!alive && !c->in_kex && !sh_out_peek(ch->sh, &p)) {
            wbuf_t b = pk_begin(c, MSG_CHANNEL_REQUEST);
            wb_u32(&b, ch->peer_id);
            wb_cstr(&b, "exit-status");
            wb_byte(&b, 0);
            wb_u32(&b, (uint32_t)sh_exit_status(ch->sh));
            pk_send(c, &b);
            b = pk_begin(c, MSG_CHANNEL_EOF);
            wb_u32(&b, ch->peer_id);
            pk_send(c, &b);
            b = pk_begin(c, MSG_CHANNEL_CLOSE);
            wb_u32(&b, ch->peer_id);
            pk_send(c, &b);
            ch->close_sent = true;
        } else if (st == SH_BUSY && ch->peer_win && !c->in_kex) busy = true;
    }
    return busy;
}

/* ---- dispatch ---- */

static bool kex_msg(uint8_t t) { return t == MSG_KEXINIT || t == MSG_NEWKEYS || t == MSG_KEX_ECDH_INIT; }

static void dispatch(conn_t *c, const uint8_t *pay, size_t n, uint32_t seq)
{
    uint8_t t = pay[0];
    if (c->ignore_next && t >= 30 && t <= 49) {
        c->ignore_next = false;
        return;
    }
    /* strict KEX: before the first NEWKEYS, only the exchange itself */
    if (c->first_kex && c->strict && !kex_msg(t)) {
        disconnect(c, DIS_PROTOCOL, "strict KEX: unexpected message during the exchange");
        return;
    }
    /* the client sent KEXINIT: until its NEWKEYS, only transport messages */
    if (c->got_kexinit && t >= 50) {
        disconnect(c, DIS_PROTOCOL, "message during a key exchange");
        return;
    }
    if (!c->have_sid && t > MSG_DEBUG && !kex_msg(t)) {
        disconnect(c, DIS_PROTOCOL, "message before the key exchange");
        return;
    }
    switch (t) {
    case MSG_DISCONNECT:
        c->dead = true;
        return;
    case MSG_IGNORE:
    case MSG_DEBUG:
    case MSG_UNIMPLEMENTED:
    case MSG_EXT_INFO:                  /* the client's own (RFC 8308): nothing in it is for us */
        return;
    case MSG_KEXINIT: on_kexinit(c, pay, n, seq); return;
    case MSG_KEX_ECDH_INIT: on_ecdh_init(c, pay, n); return;
    case MSG_NEWKEYS: on_newkeys(c); return;
    case MSG_SERVICE_REQUEST: {
        rbuf_t r = { pay, n, 1, false };
        char name[32];
        rb_cstr(&r, name, sizeof name);
        if (strcmp(name, "ssh-userauth")) {
            disconnect(c, DIS_SERVICE, "only ssh-userauth");
            return;
        }
        c->userauth = true;
        wbuf_t b = pk_begin(c, MSG_SERVICE_ACCEPT);
        wb_cstr(&b, name);
        pk_send(c, &b);
        return;
    }
    case MSG_USERAUTH_REQUEST:
        if (!c->userauth) disconnect(c, DIS_PROTOCOL, "userauth before its service");
        else on_userauth(c, pay, n);
        return;
    default:
        break;
    }
    if (!c->authed) {
        char why[40];
        snprintf(why, sizeof why, "not logged in (message %u)", t);
        disconnect(c, DIS_PROTOCOL, why);
        return;
    }
    switch (t) {
    case MSG_GLOBAL_REQUEST: {
        rbuf_t r = { pay, n, 1, false };
        size_t l;
        rb_string(&r, &l);
        if (rb_bool(&r)) {
            wbuf_t b = pk_begin(c, MSG_REQUEST_FAILURE);
            pk_send(c, &b);
        }
        return;
    }
    case MSG_REQUEST_SUCCESS:
    case MSG_REQUEST_FAILURE:
    case MSG_CHANNEL_SUCCESS:
    case MSG_CHANNEL_FAILURE:
        return;
    case MSG_CHANNEL_OPEN: on_channel_open(c, pay, n); return;
    case MSG_CHANNEL_REQUEST: on_channel_request(c, pay, n); return;
    case MSG_CHANNEL_DATA: on_channel_data(c, pay, n, false); return;
    case MSG_CHANNEL_EXTENDED_DATA: on_channel_data(c, pay, n, true); return;
    case MSG_CHANNEL_WINDOW_ADJUST:
    case MSG_CHANNEL_EOF:
    case MSG_CHANNEL_CLOSE: on_channel_misc(c, t, pay, n); return;
    default: {
        wbuf_t b = pk_begin(c, MSG_UNIMPLEMENTED);
        wb_u32(&b, seq);
        pk_send(c, &b);
        return;
    }
    }
}

/* Whole packets out of what came in, each opened and handed on. */
static void take_packets(conn_t *c)
{
    while (!c->dead && c->in_len >= 4) {
        uint8_t *q = c->in;
        bool enc = c->rx != NULL;
        uint32_t L = enc ? ssh_cipher_length(c->rx, q, c->seq_rx)
                         : (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3];
        /* in clear, at least 16 bytes with the length (RFC 4253, 6); an
         * encrypted one is whole blocks of its cipher, the length not
         * counted, and one block at least: OpenSSH's ChaCha20 sends a
         * NEWKEYS of 8 */
        uint32_t blk = enc ? (uint32_t)ssh_cipher_block(c->rx) : 8;
        if (L < (enc ? blk : 12u) || L > PKT_MAX || (enc ? L % blk : (L + 4) % 8)) {
            aos_hal_log(TAG, "%s: bad length %u (seq %u, %s, %u bytes in)", c->from, (unsigned)L, (unsigned)c->seq_rx,
                        enc ? c->enc_c2s : "clear", (unsigned)c->in_len);
            disconnect(c, DIS_PROTOCOL, "bad packet length");
            return;
        }
        size_t total = 4 + L + (enc ? 16 : 0);
        if (c->in_len < total) return;
        if (enc && !ssh_cipher_open(c->rx, q, L, c->seq_rx)) {
            disconnect(c, DIS_MAC, "corrupt packet");
            return;
        }
        uint8_t pad = q[4];
        if (pad < 4 || (size_t)pad + 1 >= L) {
            disconnect(c, DIS_PROTOCOL, "bad padding");
            return;
        }
        uint32_t seq = c->seq_rx++;
        dispatch(c, q + 5, L - 1 - pad, seq);
        memmove(c->in, c->in + total, c->in_len - total);
        c->in_len -= total;
    }
}

/* ---- the connection ---- */

/* the version line, after any lines a server may send first (RFC 4253,
 * 4.2); a client sends none, but they cost nothing to skip */
static bool read_version(conn_t *c)
{
    for (int lines = 0; lines < 20;) {
        uint8_t *nl = memchr(c->in, '\n', c->in_len);
        if (nl) {
            size_t l = (size_t)(nl - c->in);
            size_t ll = l && c->in[l - 1] == '\r' ? l - 1 : l;
            bool ver = ll >= 8 && !memcmp(c->in, "SSH-", 4);
            if (ver) {
                if (ll >= sizeof c->v_c || (memcmp(c->in, "SSH-2.0-", 8) && memcmp(c->in, "SSH-1.99-", 9))) return false;
                memcpy(c->v_c, c->in, ll);
                c->v_c[ll] = 0;
            }
            memmove(c->in, nl + 1, c->in_len - l - 1);
            c->in_len -= l + 1;
            if (ver) return true;
            lines++;
            continue;
        }
        if (c->in_len >= 255) return false;
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(c->fd, &rs);
        struct timeval tv = { 10, 0 };
        if (select(c->fd + 1, &rs, NULL, NULL, &tv) <= 0) return false;
        ssize_t k = recv(c->fd, c->in + c->in_len, IN_CAP - c->in_len, 0);
        if (k <= 0) return false;
        c->in_len += (size_t)k;
    }
    return false;
}

static void conn_run(conn_t *c)
{
    char vs[84];
    our_version(vs, sizeof vs - 2);
    strcat(vs, "\r\n");
    if (!send_all(c->fd, (const uint8_t *)vs, strlen(vs)) || !read_version(c)) {
        aos_hal_log(TAG, "%s: no SSH version line", c->from);
        return;
    }
    aos_hal_log(TAG, "%s: %s", c->from, c->v_c);
    if (!send_kexinit(c)) return;
    take_packets(c);            /* what came with the version line (its KEXINIT, often) */

    uint32_t last_check = 0;
    bool busy = false;
    while (!c->dead) {
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        if (now - last_check >= 1000) {
            last_check = now;
            aos_access_t need = need_now(c);
            if (aos_access_ssh_generation() != c->gen || !aos_access_ssh()) {
                disconnect(c, DIS_BY_APP, "closed from the board");
                break;
            }
            if (need == AOS_ACCESS_CLOSED || (c->authed && need == AOS_ACCESS_LOGIN && !strcmp(c->how, "abierta"))) {
                disconnect(c, DIS_BY_APP, "the board's rules changed");
                break;
            }
            if (!c->authed && now - c->t0 > GRACE_MS) {
                disconnect(c, DIS_BY_APP, "took too long to log in");
                break;
            }
        }
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(c->fd, &rs);
        bool chans = false;
        for (int i = 0; i < MAX_CH; i++) chans |= c->ch[i].used;
        struct timeval tv = { 0, busy ? 0 : chans ? 50000 : 500000 };
        int sel = select(c->fd + 1, &rs, NULL, NULL, &tv);
        if (sel < 0 && errno != EINTR) break;
        if (sel > 0 && FD_ISSET(c->fd, &rs)) {
            ssize_t k = recv(c->fd, c->in + c->in_len, IN_CAP - c->in_len, 0);
            if (k <= 0) break;
            c->in_len += (size_t)k;
            take_packets(c);
        }
        busy = c->authed && !c->dead ? pump(c) : false;
    }
}

static void conn_thread(void *arg)
{
    conn_t *c = arg;
    conn_run(c);
    aos_hal_mutex_lock(s_mx);
    for (int i = 0; i < MAX_CONN; i++)
        if (s_conns[i] == c) s_conns[i] = NULL;
    aos_hal_mutex_unlock(s_mx);
    if (c->authed) aos_hal_log(TAG, "%s: %s logged out", c->from, c->user);
    for (int i = 0; i < MAX_CH; i++)
        if (c->ch[i].used) chan_free(c, &c->ch[i]);
    close(c->fd);
    ssh_cipher_free(c->rx);
    ssh_cipher_free(c->tx);
    ssh_cipher_free(c->rx_next);
    free(c->i_c);
    free(c->i_s);
    free(c->in);
    free(c->out);
    memset(c, 0, sizeof *c);
    free(c);
}

/* which of aos_access' zones the connection reached the board in: the
 * address it came to decides, as for the portal */
static aos_zone_t zone_of(int s)
{
#ifdef AOS_SIM
    (void)s;
    const char *e = getenv("P4_SIM_ZONE");
    return e && !strcmp(e, "away") ? AOS_ZONE_AWAY : e && !strcmp(e, "home") ? AOS_ZONE_HOME : AOS_ZONE_CABLE;
#else
    struct sockaddr_in me;
    socklen_t l = sizeof me;
    char ip[16] = "";
    if (getsockname(s, (struct sockaddr *)&me, &l) == 0) inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip);
    if (!strcmp(ip, "192.168.7.1")) return AOS_ZONE_CABLE;
    if (!strcmp(ip, aos_hal_net_ap_ip())) return AOS_ZONE_AP;
    return aos_access_here_trusted() ? AOS_ZONE_HOME : AOS_ZONE_AWAY;
#endif
}

static void accept_one(int ls)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int fd = accept(ls, (struct sockaddr *)&a, &al);
    if (fd < 0) return;
    char from[48];
    inet_ntop(AF_INET, &a.sin_addr, from, sizeof from);
    aos_zone_t z = zone_of(fd);
    if (aos_access_need(z) == AOS_ACCESS_CLOSED) {
        aos_hal_log(TAG, "%s: refused, SSH is closed on this network (the portal's rules)", from);
        close(fd);
        return;
    }
    int slot = -1;
    aos_hal_mutex_lock(s_mx);
    for (int i = 0; i < MAX_CONN && slot < 0; i++)
        if (!s_conns[i]) slot = i;
    aos_hal_mutex_unlock(s_mx);
    if (slot < 0) {
        aos_hal_log(TAG, "%s: refused, %d connections already", from, MAX_CONN);
        close(fd);
        return;
    }
    conn_t *c = big_calloc(sizeof *c);
    if (c) {
        c->in = big_calloc(IN_CAP);
        c->out = big_calloc(OUT_CAP);
    }
    if (!c || !c->in || !c->out) {
        if (c) {
            free(c->in);
            free(c->out);
        }
        free(c);
        close(fd);
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);     /* each key typed goes at once */
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#ifdef TCP_KEEPIDLE
    int idle = 60, intvl = 15, cnt = 4;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#endif
    struct timeval st = { 15, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof st);
    c->fd = fd;
    snprintf(c->from, sizeof c->from, "%s", from);
    c->zone = z;
    c->gen = aos_access_ssh_generation();
    c->t0 = (uint32_t)aos_hal_uptime_ms();
    c->first_kex = true;
    aos_hal_mutex_lock(s_mx);
    s_conns[slot] = c;
    aos_hal_mutex_unlock(s_mx);
    /* the stack: Ed25519 and X25519 want a few KB, the commands a few more */
    if (!aos_hal_thread_start("ssh", conn_thread, c, 20480, 3)) {
        aos_hal_mutex_lock(s_mx);
        s_conns[slot] = NULL;
        aos_hal_mutex_unlock(s_mx);
        free(c->in);
        free(c->out);
        free(c);
        close(fd);
    }
}

/* Listens while SSH is switched on, and stops listening when it is off. */
static void listen_thread(void *arg)
{
    (void)arg;
    int ls = -1;
    bool told = false;
    for (;;) {
        if (!aos_access_ssh()) {
            if (ls >= 0) {
                close(ls);
                ls = -1;
                aos_hal_log(TAG, "stopped listening");
            }
            aos_hal_sleep_ms(1000);
            continue;
        }
        if (ls < 0) {
            ls = socket(AF_INET, SOCK_STREAM, 0);
            int one = 1;
            setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            struct sockaddr_in a = { 0 };
            a.sin_family = AF_INET;
            a.sin_port = htons((uint16_t)s_port);
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 2) < 0) {
                if (!told) aos_hal_log(TAG, "cannot listen on port %d: %s", s_port, strerror(errno));
                told = true;
                if (ls >= 0) close(ls);
                ls = -1;
                aos_hal_sleep_ms(5000);
                continue;
            }
            told = false;
            /* the host key now, not with the first client: it is written to
             * the preferences, and its fingerprint is what Settings shows */
            uint8_t seed[32], pub[32];
            aos_access_ssh_host_key(seed, pub);
            memset(seed, 0, sizeof seed);
            char fp[60];
            aos_access_ssh_host_fingerprint(fp, sizeof fp);
            aos_hal_log(TAG, "listening on port %d, host key ED25519 %s", s_port, fp);
        }
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(ls, &rs);
        struct timeval tv = { 1, 0 };
        if (select(ls + 1, &rs, NULL, NULL, &tv) > 0) accept_one(ls);
    }
}

bool aos_ssh_start(int port)
{
    if (s_mx || port <= 0) return s_mx != NULL;
    s_mx = aos_hal_mutex_create();
    s_port = port;
    return aos_hal_thread_start("ssh_listen", listen_thread, NULL, 6144, 3);
}

int aos_ssh_sessions(void)
{
    ssh_who_t w[MAX_CONN];
    return ssh_who(w, MAX_CONN);
}

int ssh_who(ssh_who_t *out, int max)
{
    int n = 0;
    if (!s_mx) return 0;
    aos_hal_mutex_lock(s_mx);
    for (int i = 0; i < MAX_CONN && n < max; i++) {
        conn_t *c = s_conns[i];
        if (!c || !c->authed) continue;
        snprintf(out[n].user, sizeof out[n].user, "%s", c->user);
        snprintf(out[n].from, sizeof out[n].from, "%s", c->from);
        out[n].since_s = c->since_s;
        out[n].how = c->how;
        n++;
    }
    aos_hal_mutex_unlock(s_mx);
    return n;
}
