/*
 * P4OS - The HAL's connection, plain or TLS (aos_tls.h). Shared between the
 * board and the simulator.
 *
 * It was born inside aos_http.c (2026-09-03, "TLS" in that file's header) as
 * conn_t, and moved out here on 2026-09-28 when the WebSocket client needed
 * wss:// for Home Assistant behind Nabu Casa or a reverse proxy. Moving it
 * was preferred to giving aos_ws.c a TLS of its own: the trust store, the
 * "no valid time" check, the error translation and the session cache are
 * exactly the same problem, and two copies would drift apart on the board.
 *
 * Why it can be one file for both platforms: mbedtls 3.x is the same API on
 * both sides (the IDF ships 3.6.x, Homebrew 3.4.1), so setup, handshake,
 * read, write and close are common code. The ONLY thing that differs is
 * where the trusted certificate store comes from -the IDF's compiled-in
 * bundle against the Mac's /etc/ssl/cert.pem- and that is ONE function,
 * trust_attach(). Nothing else here knows which platform it runs on, except
 * where the struct itself is allocated (aos_tls_new).
 *
 * What was measured and settles the design (aos_http.c's header and
 * docs/HANDOFF-HAL-RED-MIC.md, section 3.1):
 *
 *  - With CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC a handshake costs 3.5 KB of
 *    internal RAM instead of 34.6. The deepest stack point is verifying the
 *    chain: 4432 bytes in the worst case seen, so a task that opens a TLS
 *    connection needs ~7 KB of stack (aos_http.c's request task has 7168).
 *  - A full handshake is 1.6-1.8 s on the board and a resumed one 0.6, hence
 *    the session cache below.
 *  - With no valid time the certificate cannot be verified. That is checked
 *    BEFORE spending the handshake, and afterwards the BADCERT_FUTURE bit is
 *    translated, which is the only thing distinguishing "the board is in the
 *    past" from "I do not know that CA": both give the same return code.
 */
#include "aos_tls.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "mbedtls/net_sockets.h"

#ifndef AOS_SIM
  #include "esp_heap_caps.h"
  #include "esp_crt_bundle.h"
#endif

/* How many hosts remember their TLS session. Two is enough: the real pattern
 * is one or two servers (the weather, Home Assistant) talked to again and
 * again, not a browser. */
#define TLS_SESSIONS    2

/* WANT_READ/WANT_WRITE on a BLOCKING socket does not mean "not yet, try again
 * right away": the socket has a timeout, so getting here means it has already
 * gone by without a byte. Retrying without a limit would be an infinite loop
 * against a server that has gone quiet. A write gets ONE more round in case
 * the expiry fell right on a split record, and then it gives up. (A read
 * reports AOS_TLS_TIMEOUT instead and the caller decides.) */
#define TLS_REINTENTOS  1

#define WHY(...) do { if (why && why_n) snprintf(why, why_n, __VA_ARGS__); } while (0)

/* ==========================================================================
 * The session cache. mbedtls_ssl_set_session() and mbedtls_ssl_get_session()
 * deep-copy, so the stored entry can be shared between tasks as long as
 * nobody frees it at the same time; that is what s_lock is for.
 *
 * The lock is created on first use by whichever thread gets there first (the
 * LVGL task for an HTTP request, Home Assistant's service thread for a
 * WebSocket), so the creation itself is a compare-and-swap. The loser's
 * mutex is leaked (the HAL has no call to delete one): at most once a boot.
 * ========================================================================== */
typedef struct {
    char                host[96];
    mbedtls_ssl_session sess;
    bool                valid;
} tls_cache_t;

static tls_cache_t s_sess[TLS_SESSIONS];
static void       *s_lock;

static void *cache_lock(void)
{
    void *m = __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
    if (!m) {
        void *mine = aos_hal_mutex_create();
        void *none = NULL;
        if (__atomic_compare_exchange_n(&s_lock, &none, mine, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            m = mine;
        } else {
            m = none;                   /* somebody else won; ours is leaked once */
        }
    }
    aos_hal_mutex_lock(m);
    return m;
}

/* Copies the host's stored session into the context, if there is one. */
static void session_restore(aos_tls_t *c, const char *host)
{
    void *m = cache_lock();
    for (int i = 0; i < TLS_SESSIONS; i++) {
        if (s_sess[i].valid && strcmp(s_sess[i].host, host) == 0) {
            mbedtls_ssl_set_session(&c->ssl, &s_sess[i].sess);
            break;
        }
    }
    aos_hal_mutex_unlock(m);
}

/* Stores the freshly negotiated session. If there is no free slot it
 * overwrites the first one: with two entries it is not worth tracking which
 * was used last. */
static void session_save(aos_tls_t *c, const char *host)
{
    mbedtls_ssl_session tmp;
    mbedtls_ssl_session_init(&tmp);
    if (mbedtls_ssl_get_session(&c->ssl, &tmp) != 0) {
        mbedtls_ssl_session_free(&tmp);
        return;
    }

    void *m = cache_lock();
    int libre = -1;
    for (int i = 0; i < TLS_SESSIONS; i++) {
        if (s_sess[i].valid && strcmp(s_sess[i].host, host) == 0) { libre = i; break; }
        if (!s_sess[i].valid && libre < 0)                          libre = i;
    }
    if (libre < 0) {
        libre = 0;
    }
    if (s_sess[libre].valid) {
        mbedtls_ssl_session_free(&s_sess[libre].sess);
    }
    s_sess[libre].sess  = tmp;          /* ownership is transferred */
    s_sess[libre].valid = true;
    snprintf(s_sess[libre].host, sizeof(s_sess[libre].host), "%s", host);
    aos_hal_mutex_unlock(m);
}

/* --------------------------------------------------------------------------
 * THE ONLY DIFFERENCE BETWEEN THE TWO PLATFORMS, and that is why it sits alone
 * in a function of its own rather than scattered through the file: where the
 * list of trusted authorities comes from.
 *
 * On the board it is the bundle ESP-IDF compiles into the binary (CMN profile:
 * measured, it validates the same servers as FULL and takes 48 KB less). On
 * the Mac it is the system's file, which is already kept up to date without
 * anybody maintaining it, plus -simulator only- the PEM file named by
 * P4_SIM_EXTRA_CA, so a throwaway CA (tools/fake_ha.py --tls) can be trusted
 * for a local test without touching the system store.
 * -------------------------------------------------------------------------- */
static bool trust_attach(aos_tls_t *c, char *why, size_t why_n)
{
#ifdef AOS_SIM
    if (mbedtls_x509_crt_parse_file(&c->ca, "/etc/ssl/cert.pem") < 0) {
        WHY("cannot read /etc/ssl/cert.pem");
        return false;
    }
    const char *extra = getenv("P4_SIM_EXTRA_CA");
    if (extra && extra[0]) {
        int rc = mbedtls_x509_crt_parse_file(&c->ca, extra);
        if (rc != 0) {
            /* Loud, not fatal: the system store is still there. */
            aos_hal_log("tls", "P4_SIM_EXTRA_CA=%s: %s (-0x%04x)", extra,
                        rc < 0 ? "not a PEM certificate" : "some certificates skipped",
                        (unsigned)(rc < 0 ? -rc : rc));
        }
    }
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    return true;
#else
    if (esp_crt_bundle_attach(&c->conf) != 0) {
        WHY("cannot attach the certificate bundle");
        return false;
    }
    return true;
#endif
}

/* ==========================================================================
 * The socket
 * ========================================================================== */

/* Sets SO_RCVTIMEO or SO_SNDTIMEO only when it changes. Never 0: for a socket
 * 0 means "wait forever". */
static void set_timeout(aos_tls_t *c, bool rcv, int ms)
{
    if (ms < 1) {
        ms = 1;
    }
    int *cur = rcv ? &c->rcv_ms : &c->snd_ms;
    if (*cur == ms) {
        return;
    }
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(c->fd, SOL_SOCKET, rcv ? SO_RCVTIMEO : SO_SNDTIMEO, &tv, sizeof(tv));
    *cur = ms;
}

/* Connects with a time limit. SO_RCVTIMEO does not cover connect, so it has
 * to be set non-blocking and waited on with select. */
static int sock_connect(const char *host, int port, int timeout_ms)
{
    char service[8];
    snprintf(service, sizeof(service), "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;        /* the board does not always have IPv6 */
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) {
        return AOS_HTTP_ERR_DNS;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return AOS_HTTP_ERR_CONNECT;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc != 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return AOS_HTTP_ERR_CONNECT;
        }
        fd_set w;
        FD_ZERO(&w);
        FD_SET(fd, &w);
        struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
        if (select(fd + 1, NULL, &w, NULL, &tv) <= 0) {
            close(fd);
            return AOS_HTTP_ERR_CONNECT;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            close(fd);
            return AOS_HTTP_ERR_CONNECT;
        }
    }

    fcntl(fd, F_SETFL, flags);          /* back to blocking, now with timeouts */
#ifdef SO_NOSIGPIPE
    /* macOS: a peer that closes while we write must be an error, not a
     * SIGPIPE that takes the whole simulator down. lwIP has no signals. */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;
}

/* mbedtls talks to the socket through these two. A timeout arrives as EAGAIN
 * and is translated to WANT_READ/WANT_WRITE: the handshake loop retries it
 * against its deadline, a read reports AOS_TLS_TIMEOUT. */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int n = send(*(int *)ctx, buf, len, 0);
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE
                                                         : MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int n = recv(*(int *)ctx, buf, len, 0);
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ
                                                         : MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return n;
}

/* The verification flags in words. The return code alone says nothing
 * ("-0x2700, verify failed" for all of them); these are what a person needs
 * to fix it. */
static void why_verify(uint32_t flags, const char *host, char *why, size_t why_n)
{
    if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) {
        WHY("the certificate is not for %s", host);
    } else if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) {
        WHY("certificate for %s is not signed by a trusted authority", host);
    } else if (flags & MBEDTLS_X509_BADCERT_EXPIRED) {
        WHY("the certificate for %s has expired", host);
    } else if (flags & MBEDTLS_X509_BADCERT_REVOKED) {
        WHY("the certificate for %s is revoked", host);
    } else if (flags) {
        WHY("certificate for %s rejected (flags 0x%lx)", host, (unsigned long)flags);
    }
}

/* ==========================================================================
 * The four calls
 * ========================================================================== */

void aos_tls_close(aos_tls_t *c)
{
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_entropy_free(&c->ent);
        c->tls = false;
    }
    mbedtls_x509_crt_free(&c->ca);
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

int aos_tls_open(aos_tls_t *c, const char *host, int port, unsigned flags,
                 int timeout_ms, char *why, size_t why_n)
{
    bool tls = (flags & AOS_TLS_ENCRYPT) != 0;
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    mbedtls_x509_crt_init(&c->ca);
    WHY("%s", "");
    if (timeout_ms < 1) {
        timeout_ms = 1;
    }

    /* The time first, and BEFORE spending the handshake. A certificate is
     * verified against the clock: with no time there is no way to know whether
     * it is still valid, and the error coming out of the handshake does not
     * explain it (see below). */
    if (tls && !aos_hal_time_is_valid()) {
        WHY("no valid time yet: a certificate cannot be checked");
        return AOS_HTTP_ERR_SIN_HORA;
    }

    int fd = sock_connect(host, port, timeout_ms);
    if (fd < 0) {
        if (fd == AOS_HTTP_ERR_DNS) {
            WHY("cannot resolve %s", host);
        } else {
            WHY("cannot connect to %s:%d", host, port);
        }
        return fd;                     /* already an AOS_HTTP_ERR_* */
    }
    c->fd = fd;
    set_timeout(c, true, timeout_ms);
    set_timeout(c, false, timeout_ms);
    if (flags & AOS_TLS_NODELAY) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    if (!tls) {
        return 0;
    }

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_entropy_init(&c->ent);
    mbedtls_ctr_drbg_init(&c->drbg);
    c->tls = true;

    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->ent,
                              (const unsigned char *)"aos", 3) != 0 ||
        mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        WHY("TLS setup failed");
        aos_tls_close(c);
        return AOS_HTTP_ERR_TLS;
    }

    /* VERIFY_REQUIRED and not OPTIONAL: if the chain cannot be verified, the
     * connection does not go out. A TLS that accepts any certificate gives the
     * same feeling of security with none of the properties. */
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);

    if (!trust_attach(c, why, why_n)) {
        aos_tls_close(c);
        return AOS_HTTP_ERR_TLS;
    }

    /* set_hostname is mandatory, and for two different reasons: it sends SNI
     * (without it, a server with several sites answers with the wrong
     * certificate) and it is against this name that the certificate's is
     * compared. An IP address is compared the same way, against the
     * certificate's names. */
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0 ||
        mbedtls_ssl_set_hostname(&c->ssl, host) != 0) {
        WHY("TLS setup failed");
        aos_tls_close(c);
        return AOS_HTTP_ERR_TLS;
    }

    session_restore(c, host);
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);

    uint64_t limite = aos_hal_uptime_ms() + (uint64_t)timeout_ms;
    int rc;
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            break;
        }
        if (aos_hal_uptime_ms() > limite) {
            WHY("TLS handshake with %s timed out", host);
            aos_tls_close(c);
            return AOS_HTTP_ERR_TLS;
        }
    }

    if (rc != 0) {
        /* The return code is not enough to know what happened: "the board is
         * in the past" and "I do not know that CA" are both -0x2700. What
         * separates them is in the verification flags, and with the clock
         * wrong BOTH come back set at once -if the board believes it is 2015,
         * the bundle's root is not valid yet either-, so BADCERT_FUTURE is
         * looked at first. */
        uint32_t vflags = mbedtls_ssl_get_verify_result(&c->ssl);
        aos_tls_close(c);
        if (vflags != (uint32_t)-1 && (vflags & MBEDTLS_X509_BADCERT_FUTURE)) {
            WHY("the clock is behind the certificate for %s", host);
            return AOS_HTTP_ERR_SIN_HORA;
        }
        if (vflags != (uint32_t)-1 && vflags) {
            why_verify(vflags, host, why, why_n);
        } else {
            WHY("TLS handshake with %s failed (-0x%04x)", host, (unsigned)-rc);
        }
        return AOS_HTTP_ERR_TLS;
    }

    session_save(c, host);
    return 0;
}

bool aos_tls_send(aos_tls_t *c, const void *data, int len, int timeout_ms)
{
    const unsigned char *buf = data;
    if (c->fd < 0) {
        return false;
    }
    set_timeout(c, false, timeout_ms);
    int sent = 0;
    int reintentos = 0;
    while (sent < len) {
        if (!c->tls) {
            int w = send(c->fd, buf + sent, len - sent, 0);
            if (w <= 0) {
                return false;
            }
            sent += w;
            continue;
        }
        int w = mbedtls_ssl_write(&c->ssl, buf + sent, len - sent);
        if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (++reintentos > TLS_REINTENTOS) {
                return false;
            }
            continue;
        }
        if (w <= 0) {
            return false;
        }
        reintentos = 0;
        sent += w;
    }
    return true;
}

int aos_tls_recv(aos_tls_t *c, void *buf, int max, int timeout_ms)
{
    if (c->fd < 0) {
        return -1;
    }
    set_timeout(c, true, timeout_ms);
    if (!c->tls) {
        int n = recv(c->fd, buf, max, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return AOS_TLS_TIMEOUT;
        }
        return n < 0 ? -1 : n;
    }
    /* A partial record left by a timeout stays inside mbedtls and the next
     * call resumes it: that is how mbedtls's non-blocking reads work. */
    int r = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, max);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return AOS_TLS_TIMEOUT;
    }
    /* An announced close is a legitimate ending, just like the bare socket's
     * EOF: with HTTP/1.0 that is how every response ends. */
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;
    }
    return r < 0 ? -1 : r;
}

aos_tls_t *aos_tls_new(void)
{
#ifdef AOS_SIM
    aos_tls_t *c = calloc(1, sizeof(*c));
#else
    /* PSRAM: ~2 KB of contexts with no reason to come out of internal RAM;
     * mbedtls's own buffers are there already (EXTERNAL_MEM_ALLOC). */
    aos_tls_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) {
        c = calloc(1, sizeof(*c));
    }
#endif
    if (c) {
        c->fd = -1;
    }
    return c;
}

void aos_tls_free(aos_tls_t *c)
{
    if (c) {
        aos_tls_close(c);
        free(c);
    }
}
