/*
 * P4OS - Mi casa: the checkers.
 *
 * Two threads (aos_hal_thread_start, PSRAM stacks) share the list: each
 * takes the service whose turn came first, tries it with a copy of its
 * settings and writes what it found under the lock. Two, so that a service
 * that does not answer (a whole timeout) does not hold up the rest.
 *
 * HTTP and HTTPS go over the HAL's connection (aos_tls.c, as Cartero and
 * Impresoras 3D do), not aos_hal_http_*: that one does not say how long the
 * connection took apart from the answer, and has three slots for the whole
 * system. A request here is one GET with "Connection: close"; the latency
 * is from the request leaving to the first byte of the answer, and the time
 * to connect (DNS, TCP and the TLS handshake, 1.6-1.8 s on the board and
 * mostly the board's own work) is kept apart. A TCP check is the connect
 * alone, and its latency is that.
 *
 * A service is "down" after cfg.fails failures in a row; after the first
 * one it is tried again sooner (RETRY_S) to tell a hiccup from an outage.
 * While the board has no network nothing is tried and nothing counts.
 */
#include "ec.h"

#include <stdlib.h>

/* from the HAL's aos_tls.h, which is not included: it pulls in mbedtls's */
typedef struct ec_tls ec_tls_t;
ec_tls_t *aos_tls_new(void);
void aos_tls_free(ec_tls_t *c);
int  aos_tls_open(ec_tls_t *c, const char *host, int port, unsigned flags, int timeout_ms, char *why, size_t why_n);
int  aos_tls_open_trust(ec_tls_t *c, const char *host, int port, unsigned flags, const char *ca_pem, size_t ca_len,
                        const char *name, int timeout_ms, char *why, size_t why_n);
bool aos_tls_send(ec_tls_t *c, const void *buf, int len, int timeout_ms);
int  aos_tls_recv(ec_tls_t *c, void *buf, int max, int timeout_ms);
#define TLS_ENCRYPT  0x01u
#define TLS_NODELAY  0x02u
#define TLS_TIMEOUT  (-2)

#define THREADS      2
#define STACK        (24 * 1024)
#define PRIO         3
#define SLICE_MS     200
#define RETRY_S      15
#define HDR_MAX      (8 * 1024)
#define BODY_MAX     (64 * 1024)
#define CERT_EVERY_MS   (6u * 3600u * 1000u)
#define CERT_RETRY_MS   (30u * 60u * 1000u)
#define EVENTS       16

static struct {
    void      *mx;
    ec_svc_t   svc[EC_MAX];
    ec_rt_t    rt[EC_MAX];
    uint8_t   *der[EC_MAX];
    int        der_len[EC_MAX];
    int        n;
    int        fails;
    volatile bool stop;
    volatile int  running;
    uint32_t   seq;
    ec_event_t ev[EVENTS];
    int        ev_head, ev_n;
} S;

void ec_lock(void) { if (S.mx) aos_hal_mutex_lock(S.mx); }
void ec_unlock(void) { if (S.mx) aos_hal_mutex_unlock(S.mx); }
const ec_rt_t *ec_rt(int i) { return &S.rt[i >= 0 && i < EC_MAX ? i : 0]; }
uint32_t ec_seq(void) { return S.seq; }

const uint8_t *ec_rt_der(int i, int *len)
{
    if (i < 0 || i >= EC_MAX || !S.der[i]) return NULL;
    *len = S.der_len[i];
    return S.der[i];
}

static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }
static bool same_target(const ec_svc_t *a, const ec_svc_t *b);

bool ec_net_ok(void) { return aos_hal_net_state() == AOS_NET_CONNECTED; }

int ec_count(int state)
{
    int c = 0;
    for (int i = 0; i < S.n; i++) c += S.rt[i].state == state;
    return c;
}

static void push_event(int i, int kind, int err, int code, int32_t n)
{
    ec_event_t *e = &S.ev[(S.ev_head + S.ev_n) % EVENTS];
    if (S.ev_n == EVENTS) S.ev_head = (S.ev_head + 1) % EVENTS;
    else S.ev_n++;
    memset(e, 0, sizeof *e);
    e->i = i;
    ec_copy(e->name, sizeof e->name, S.svc[i].name);
    e->kind = kind;
    e->err = err;
    e->code = code;
    e->n = n;
}

bool ec_event_pop(ec_event_t *e)
{
    ec_lock();
    bool got = S.ev_n > 0;
    if (got) {
        *e = S.ev[S.ev_head];
        S.ev_head = (S.ev_head + 1) % EVENTS;
        S.ev_n--;
    }
    ec_unlock();
    return got;
}

/* ---- one HTTP request ------------------------------------------------------------------------ */

typedef struct {
    bool ok;
    int  err, code, ms, conn_ms;
    char why[96];
} res_t;

static int us_to_ms(uint64_t a, uint64_t b)
{
    uint32_t d = (uint32_t)(b - a);         /* microseconds: a check is far below an hour */
    return (int)(d / 1000u);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* case-insensitive search in a buffer that may hold NULs */
static bool ci_find(const char *hay, int n, const char *needle)
{
    int m = (int)strlen(needle);
    if (!m) return true;
    for (int i = 0; i + m <= n; i++) {
        int k = 0;
        while (k < m && lower((unsigned char)hay[i + k]) == lower((unsigned char)needle[k])) k++;
        if (k == m) return true;
    }
    return false;
}

/* the value of a header, "" if absent; hdr ends at the blank line */
static void header(const char *hdr, const char *name, char *out, size_t n)
{
    out[0] = '\0';
    size_t nl = strlen(name);
    for (const char *p = strstr(hdr, "\r\n"); p && p[2] && !(p[2] == '\r' && p[3] == '\n'); p = strstr(p + 2, "\r\n")) {
        const char *l = p + 2;
        if (!strncasecmp(l, name, nl) && l[nl] == ':') {
            l += nl + 1;
            while (*l == ' ') l++;
            size_t k = 0;
            while (l[k] && l[k] != '\r' && k + 1 < n) { out[k] = l[k]; k++; }
            out[k] = '\0';
            return;
        }
    }
}

/* chunked transfer coding undone in place; returns the new length */
static int dechunk(char *b, int n)
{
    int r = 0, w = 0;
    while (r < n) {
        char *e;
        long sz = strtol(b + r, &e, 16);
        if (e == b + r) break;
        const char *crlf = strstr(e, "\r\n");
        if (!crlf) break;
        r = (int)(crlf - b) + 2;
        if (sz <= 0) break;
        if (sz > n - r) sz = n - r;
        memmove(b + w, b + r, (size_t)sz);
        w += (int)sz;
        r += (int)sz + 2;
    }
    b[w] = '\0';
    return w;
}

static char *load_pin(uint32_t key, const char **pem, char *name, size_t nn)
{
    char path[200];
    ec_cert_pin_path(key, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16 * 1024) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    name[0] = '\0';
    if (!strncmp(buf, "name=", 5)) {
        const char *e = buf + 5 + strcspn(buf + 5, "\r\n");
        size_t l = (size_t)(e - buf - 5);
        if (l >= nn) l = nn - 1;
        memcpy(name, buf + 5, l);
        name[l] = '\0';
    }
    *pem = strstr(buf, "-----BEGIN");
    if (!*pem) {
        free(buf);
        return NULL;
    }
    return buf;
}

static int open_err(int rc, int took, int timeout)
{
    switch (rc) {
    case AOS_HTTP_ERR_DNS: return EC_E_DNS;
    case AOS_HTTP_ERR_SIN_HORA: return EC_E_CLOCK;
    case AOS_HTTP_ERR_TLS: return EC_E_TLS;
    case AOS_HTTP_ERR_MEM: return EC_E_MEM;
    default: return took >= timeout - 100 ? EC_E_TIMEOUT : EC_E_REFUSED;
    }
}

static void check_http(const ec_svc_t *s, res_t *r)
{
    char host[EC_HOST], path[EC_URL];
    int port;
    bool tls;
    if (!ec_url_split(s->url, host, sizeof host, &port, path, sizeof path, &tls)) {
        r->err = EC_E_URL;
        return;
    }
    ec_tls_t *c = aos_tls_new();
    char *buf = c ? ec_psram(BODY_MAX + HDR_MAX + 1) : NULL;
    if (!c || !buf) {
        if (c) aos_tls_free(c);
        r->err = EC_E_MEM;
        return;
    }
    uint64_t t0 = aos_hal_uptime_us();
    int rc;
    char *pin = NULL;
    const char *pem = NULL;
    char pin_name[96];
    if (tls && s->trust && (pin = load_pin(ec_svc_key(s), &pem, pin_name, sizeof pin_name)) != NULL) {
        rc = aos_tls_open_trust(c, host, port, TLS_NODELAY, pem, strlen(pem) + 1, pin_name[0] ? pin_name : NULL,
                                s->timeout_ms, r->why, sizeof r->why);
        free(pin);
    } else {
        rc = aos_tls_open(c, host, port, (tls ? TLS_ENCRYPT : 0) | TLS_NODELAY, s->timeout_ms, r->why, sizeof r->why);
    }
    uint64_t t1 = aos_hal_uptime_us();
    r->conn_ms = us_to_ms(t0, t1);
    if (rc != 0) {
        r->err = open_err(rc, r->conn_ms, s->timeout_ms);
        aos_tls_free(c);
        free(buf);
        return;
    }
    r->why[0] = '\0';
    char req[EC_URL + EC_HOST + 160];
    bool def = port == (tls ? 443 : 80);
    int qn = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\nHost: %s%s%.0d\r\nUser-Agent: P4OS-Estado/1\r\nAccept: */*\r\n"
                      "Connection: close\r\n\r\n",
                      path, host, def ? "" : ":", def ? 0 : port);
    if (qn >= (int)sizeof req) qn = (int)sizeof req - 1;
    uint64_t ts = aos_hal_uptime_us();
    if (!aos_tls_send(c, req, qn, s->timeout_ms)) {
        r->err = EC_E_CLOSED;
        aos_tls_free(c);
        free(buf);
        return;
    }
    /* the head */
    int len = 0, hend = -1;
    uint64_t first = 0;
    uint32_t deadline = now_ms() + (uint32_t)s->timeout_ms;
    bool closed = false;
    while (hend < 0 && !S.stop) {
        if ((int32_t)(now_ms() - deadline) >= 0) break;
        int got = aos_tls_recv(c, buf + len, HDR_MAX - len, SLICE_MS);
        if (got == TLS_TIMEOUT) continue;
        if (got <= 0) {
            closed = true;
            break;
        }
        if (!first) first = aos_hal_uptime_us();
        len += got;
        buf[len] = '\0';
        char *e = strstr(buf, "\r\n\r\n");
        if (e) hend = (int)(e - buf) + 4;
        else if (len >= HDR_MAX) break;
    }
    if (first) r->ms = us_to_ms(ts, first);
    if (hend < 0) {
        r->err = !first ? (closed ? EC_E_CLOSED : EC_E_TIMEOUT) : (len >= HDR_MAX ? EC_E_PROTO : EC_E_TIMEOUT);
        if (S.stop) r->err = EC_E_TIMEOUT;
        aos_tls_free(c);
        free(buf);
        return;
    }
    int code = 0;
    if (strncmp(buf, "HTTP/1.", 7) || sscanf(buf + 8, " %d", &code) != 1 || code < 100 || code > 999) {
        r->err = EC_E_PROTO;
        aos_tls_free(c);
        free(buf);
        return;
    }
    r->code = code;
    if (!ec_expect_ok(s->expect, code)) {
        r->err = EC_E_CODE;
        aos_tls_free(c);
        free(buf);
        return;
    }
    if (!s->word[0]) {
        r->ok = true;
        aos_tls_free(c);
        free(buf);
        return;
    }
    /* the body, until the word is in it, or it ends */
    char v[40];
    buf[hend - 2] = '\0';                   /* the head alone, for header() */
    header(buf, "Content-Length", v, sizeof v);
    long clen = v[0] ? strtol(v, NULL, 10) : -1;
    header(buf, "Transfer-Encoding", v, sizeof v);
    bool chunked = ci_find(v, (int)strlen(v), "chunked");
    buf[hend - 2] = '\r';
    char *body = buf + hend;
    int blen = len - hend;
    memmove(buf, body, (size_t)blen);
    body = buf;
    body[blen] = '\0';
    bool found = ci_find(body, blen, s->word);
    while (!found && !closed && !S.stop && blen < BODY_MAX) {
        if (clen >= 0 && blen >= clen) break;
        if (chunked && blen >= 5 && !memcmp(body + blen - 5, "0\r\n\r\n", 5)) break;
        if ((int32_t)(now_ms() - deadline) >= 0) break;
        int got = aos_tls_recv(c, body + blen, BODY_MAX - blen, SLICE_MS);
        if (got == TLS_TIMEOUT) continue;
        if (got <= 0) break;
        int from = blen > EC_WORD ? blen - EC_WORD : 0;
        blen += got;
        body[blen] = '\0';
        found = ci_find(body + from, blen - from, s->word);
    }
    if (!found && chunked) {
        blen = dechunk(body, blen);
        found = ci_find(body, blen, s->word);
    }
    aos_tls_free(c);
    free(buf);
    if (found) r->ok = true;
    else r->err = EC_E_WORD;
}

static void check_tcp(const ec_svc_t *s, res_t *r)
{
    if (!s->host[0] || s->port <= 0) {
        r->err = EC_E_URL;
        return;
    }
    ec_tls_t *c = aos_tls_new();
    if (!c) {
        r->err = EC_E_MEM;
        return;
    }
    uint64_t t0 = aos_hal_uptime_us();
    int rc = aos_tls_open(c, s->host, s->port, TLS_NODELAY, s->timeout_ms, r->why, sizeof r->why);
    uint64_t t1 = aos_hal_uptime_us();
    aos_tls_free(c);
    r->conn_ms = us_to_ms(t0, t1);
    if (rc != 0) {
        r->err = open_err(rc, r->conn_ms, s->timeout_ms);
        return;
    }
    r->why[0] = '\0';
    r->ms = r->conn_ms;
    r->ok = true;
}

/* ---- the certificate --------------------------------------------------------------------------- */

/* The certificate notices given, by service: one when it first runs short,
 * then at a week, three days, one day and the end; not again because the
 * service was edited (the state starts over) while the app stays open. */
static struct { uint32_t key; int days; } s_cert_said[EC_MAX * 2];

static bool cert_worth(uint32_t key, int days)
{
    int at = -1, free_at = -1;
    for (int k = 0; k < EC_MAX * 2; k++) {
        if (s_cert_said[k].key == key) at = k;
        else if (!s_cert_said[k].key && free_at < 0) free_at = k;
    }
    if (at < 0) {
        at = free_at >= 0 ? free_at : (int)(key % (EC_MAX * 2));
        s_cert_said[at].key = key;
        s_cert_said[at].days = days;
        return true;
    }
    int was = s_cert_said[at].days;
    s_cert_said[at].days = days;
    if (days >= was) return false;
    static const int MARKS[] = { 7, 3, 1, 0 };
    for (size_t m = 0; m < sizeof MARKS / sizeof MARKS[0]; m++)
        if (was > MARKS[m] && days <= MARKS[m]) return true;
    return days < 0 && was >= 0;
}

static void cert_look(int i, const ec_svc_t *s, bool keep)
{
    char host[EC_HOST], path[8];
    int port;
    bool tls;
    if (!ec_url_split(s->url, host, sizeof host, &port, path, sizeof path, &tls) || !tls) return;
    ec_cert_t ct;
    bool ok = ec_cert_fetch(host, port, s->timeout_ms, keep, &ct);
    uint32_t now = now_ms(), epoch = ec_now_epoch();
    ec_lock();
    if (i < S.n && same_target(&S.svc[i], s)) {
        ec_rt_t *rt = &S.rt[i];
        int was = rt->cert_state;
        if (ok && ct.ok && epoch) {
            int64_t left = ct.not_after - (int64_t)epoch;
            rt->cert_days = (int)(left >= 0 ? left / 86400 : -((-left + 86399) / 86400));
            rt->cert_state = left < 0 ? EC_CERT_EXPIRED : rt->cert_days < s->cert_days ? EC_CERT_SOON : EC_CERT_OK;
            rt->cert_self = ct.self;
            ec_copy(rt->cert_name, sizeof rt->cert_name, ct.dns[0] ? ct.dns : ct.name);
            ec_copy(rt->cert_check, sizeof rt->cert_check, ct.dns[0] ? ct.dns : ct.san_ip ? host : ct.name);
            rt->cert_due_ms = now + CERT_EVERY_MS;
            if (keep && ct.der) {
                free(S.der[i]);
                S.der[i] = ct.der;
                S.der_len[i] = ct.der_len;
                ct.der = NULL;
            }
            (void)was;
            if ((rt->cert_state == EC_CERT_SOON || rt->cert_state == EC_CERT_EXPIRED) && s->cert_days > 0 &&
                cert_worth(ec_svc_key(s), rt->cert_days)) {
                push_event(i, EC_EV_CERT, 0, 0, rt->cert_days);
                char d[32];
                snprintf(d, sizeof d, "%d", rt->cert_days);
                ec_hist_log(s->name, "cert", d, -1);
            }
        } else {
            rt->cert_state = EC_CERT_UNKNOWN;
            rt->cert_due_ms = now + CERT_RETRY_MS;
        }
        rt->seq++;
        S.seq++;
    }
    ec_unlock();
    free(ct.der);
}

/* ---- the threads --------------------------------------------------------------------------------- */

static void apply(int i, const ec_svc_t *s, const res_t *r)
{
    ec_rt_t *rt = &S.rt[i];
    uint32_t now = now_ms();
    rt->busy = false;
    rt->checked_ms = now ? now : 1;
    rt->ms = r->ok ? r->ms : -1;
    rt->conn_ms = r->conn_ms;
    rt->code = r->code;
    rt->err = r->ok ? EC_E_NONE : r->err;
    ec_copy(rt->why, sizeof rt->why, r->why);
    int was = rt->state;
    bool was_up = was == EC_ST_UP || was == EC_ST_SLOW;
    if (r->ok) {
        rt->fails_row = 0;
        rt->state = r->ms > s->slow_ms ? EC_ST_SLOW : EC_ST_UP;
        if (!was_up) {
            /* an outage still open in the history is closed even when the state
             * started again from nothing (the service was edited meanwhile) */
            int32_t closed = ec_hist_outage(i, false, 0);
            uint32_t down = was == EC_ST_DOWN ? (now - rt->since_ms) / 1000u : closed > 0 ? (uint32_t)closed : 0;
            if (was == EC_ST_DOWN) push_event(i, EC_EV_UP, 0, 0, (int32_t)down);
            if (was == EC_ST_DOWN || closed >= 0) {
                char d[24];
                snprintf(d, sizeof d, "%lu", (unsigned long)down);
                ec_hist_log(s->name, "up", d, r->ms);
            }
            rt->since_ms = now;
            rt->since_epoch = ec_now_epoch();
        }
        rt->due_ms = now + (uint32_t)s->every_s * 1000u;
    } else {
        rt->fails_row++;
        if (rt->fails_row >= S.fails && was != EC_ST_DOWN) {
            rt->state = EC_ST_DOWN;
            rt->since_ms = now;
            rt->since_epoch = ec_now_epoch();
            if (was_up) push_event(i, EC_EV_DOWN, rt->err, r->code, 0);
            char d[24];
            if (rt->err == EC_E_CODE) snprintf(d, sizeof d, "%d", r->code);
            else d[0] = '\0';
            ec_hist_log(s->name, "down", rt->err == EC_E_CODE ? d : ec_err_word(rt->err), -1);
            ec_hist_outage(i, true, rt->err);
        }
        uint32_t every = rt->state == EC_ST_DOWN ? (uint32_t)s->every_s : (uint32_t)(s->every_s < RETRY_S ? s->every_s : RETRY_S);
        rt->due_ms = now + every * 1000u;
    }
    ec_hist_add(i, r->ok, r->ms);
    rt->seq++;
    S.seq++;
}

static bool same_target(const ec_svc_t *a, const ec_svc_t *b)
{
    return !strcmp(a->name, b->name) && a->kind == b->kind && !strcmp(a->url, b->url) &&
           !strcmp(a->host, b->host) && a->port == b->port && a->trust == b->trust;
}

static int pick(void)
{
    uint32_t now = now_ms();
    int best = -1;
    int32_t best_late = INT32_MIN;
    for (int i = 0; i < S.n; i++) {
        ec_rt_t *rt = &S.rt[i];
        if (rt->busy || S.svc[i].paused) continue;
        int32_t late = (int32_t)(now - rt->due_ms);
        if (rt->now) late = INT32_MAX;
        if (late < 0) continue;
        if (late > best_late) {
            best_late = late;
            best = i;
        }
    }
    return best;
}

static void thread_fn(void *arg)
{
    int me = (int)(intptr_t)arg;
    uint32_t saved = now_ms();
    while (!S.stop) {
        if (!ec_net_ok()) {
            aos_hal_sleep_ms(500);
            continue;
        }
        ec_lock();
        int i = pick();
        ec_svc_t s;
        bool cert = false, keep = false;
        if (i >= 0) {
            ec_rt_t *rt = &S.rt[i];
            rt->busy = true;
            rt->now = false;
            s = S.svc[i];
            cert = s.kind == EC_HTTP && !strncasecmp(s.url, "https://", 8) &&
                   (int32_t)(now_ms() - rt->cert_due_ms) >= 0;
        }
        ec_unlock();
        if (i < 0) {
            if (me == 0 && now_ms() - saved > 60000) {
                saved = now_ms();
                ec_hist_save_if(false);
            }
            aos_hal_sleep_ms(200);
            continue;
        }
        res_t r;
        memset(&r, 0, sizeof r);
        r.ms = -1;
        if (s.kind == EC_TCP) check_tcp(&s, &r);
        else check_http(&s, &r);
        if (S.stop) break;
        ec_lock();
        /* the list may have changed under us: the result goes where its service is now */
        int at = -1;
        for (int k = 0; k < S.n; k++)
            if (same_target(&S.svc[k], &s)) { at = k; break; }
        if (at >= 0 && S.svc[at].paused) {
            S.rt[at].busy = false;
            at = -1;
        } else if (at >= 0) {
            apply(at, &S.svc[at], &r);
        }
        /* a refused certificate is fetched at once, to show whose it is and trust it */
        if (at >= 0 && r.err == EC_E_TLS && !S.der[at]) {
            cert = true;
            keep = true;
        }
        ec_unlock();
        if (at >= 0 && cert) cert_look(at, &s, keep);
    }
    ec_lock();
    S.running--;
    ec_unlock();
}

/* ---- the list ------------------------------------------------------------------------------------- */

bool ec_core_init(void)
{
    void *mx = S.mx;
    memset(&S, 0, sizeof S);
    S.mx = mx ? mx : aos_hal_mutex_create();
    S.fails = 2;
    return S.mx != NULL;
}

void ec_core_sync(const ec_cfg_t *c)
{
    static ec_rt_t old_rt[EC_MAX];
    static ec_svc_t old_svc[EC_MAX];
    static uint8_t *old_der[EC_MAX];
    static int old_len[EC_MAX];
    ec_lock();
    int old_n = S.n;
    memcpy(old_rt, S.rt, sizeof old_rt);
    memcpy(old_svc, S.svc, sizeof old_svc);
    memcpy(old_der, S.der, sizeof old_der);
    memcpy(old_len, S.der_len, sizeof old_len);
    memset(S.der, 0, sizeof S.der);
    uint32_t now = now_ms();
    S.n = c->n;
    S.fails = c->fails > 0 ? c->fails : 2;
    for (int i = 0; i < c->n; i++) {
        const ec_svc_t *s = &c->s[i];
        uint32_t key = ec_svc_key(s);
        int was = -1;
        for (int k = 0; k < old_n; k++)
            if (old_svc[k].name[0] && ec_svc_key(&old_svc[k]) == key) { was = k; break; }
        ec_rt_t *rt = &S.rt[i];
        bool same = was >= 0 && same_target(&old_svc[was], s);
        if (was >= 0 && same) {
            *rt = old_rt[was];
            S.der[i] = old_der[was];
            S.der_len[i] = old_len[was];
            old_der[was] = NULL;
            if (strcmp(old_svc[was].expect, s->expect) || strcmp(old_svc[was].word, s->word)) rt->now = true;
        } else {
            memset(rt, 0, sizeof *rt);
            rt->ms = rt->conn_ms = -1;
            rt->now = true;
            rt->since_ms = now;
        }
        if (s->paused) rt->state = EC_ST_PAUSED;
        else if (rt->state == EC_ST_PAUSED) {
            rt->state = EC_ST_UNKNOWN;
            rt->fails_row = 0;
            rt->now = true;
            rt->since_ms = now;
        }
        rt->seq++;
        S.svc[i] = *s;
    }
    for (int i = c->n; i < EC_MAX; i++) {
        memset(&S.svc[i], 0, sizeof S.svc[i]);
        memset(&S.rt[i], 0, sizeof S.rt[i]);
    }
    for (int k = 0; k < EC_MAX; k++) free(old_der[k]);
    ec_hist_sync(c);
    S.seq++;
    bool start = S.running == 0 && !S.stop;
    ec_unlock();
    if (start && c->n > 0) {
        for (int t = 0; t < THREADS; t++) {
            ec_lock();
            S.running++;
            ec_unlock();
            if (!aos_hal_thread_start(t ? "estado2" : "estado1", thread_fn, (void *)(intptr_t)t, STACK, PRIO)) {
                ec_lock();
                S.running--;
                ec_unlock();
                aos_hal_log(EC_TAG, "could not start checker %d", t);
            }
        }
    }
}

void ec_core_check_now(int i)
{
    ec_lock();
    for (int k = 0; k < S.n; k++)
        if (i < 0 || k == i) {
            S.rt[k].now = true;
            S.rt[k].cert_due_ms = now_ms();
        }
    ec_unlock();
}

void ec_core_stop(uint32_t wait_ms)
{
    S.stop = true;
    uint32_t t0 = now_ms();
    while (S.running > 0 && now_ms() - t0 < wait_ms) aos_hal_sleep_ms(20);
    if (S.running > 0) aos_hal_log(EC_TAG, "%d checkers still running after %lu ms", S.running, (unsigned long)wait_ms);
    for (int k = 0; k < EC_MAX; k++) {
        free(S.der[k]);
        S.der[k] = NULL;
    }
}

/* ---- words ------------------------------------------------------------------------------------- */

const char *ec_err_text(int err)
{
    static const char *const T[EC_E_N] = {
        [EC_E_NONE] = "",
        [EC_E_DNS] = N_("No se encuentra el nombre"),
        [EC_E_REFUSED] = N_("Rechaza la conexión"),
        [EC_E_TIMEOUT] = N_("No contesta"),
        [EC_E_TLS] = N_("Certificado no válido"),
        [EC_E_CLOCK] = N_("La placa todavía no tiene hora"),
        [EC_E_CODE] = N_("Contestó otro código"),
        [EC_E_WORD] = N_("Falta la palabra"),
        [EC_E_PROTO] = N_("No contesta HTTP"),
        [EC_E_CLOSED] = N_("Cortó la conexión"),
        [EC_E_MEM] = N_("Sin memoria"),
        [EC_E_URL] = N_("La dirección no sirve"),
    };
    return err > 0 && err < EC_E_N ? _(T[err]) : "";
}

const char *ec_err_word(int err)
{
    static const char *const W[EC_E_N] = {
        "", "dns", "refused", "timeout", "tls", "clock", "code", "word", "proto", "closed", "mem", "url",
    };
    return err >= 0 && err < EC_E_N ? W[err] : "";
}

lv_color_t ec_state_color(int state)
{
    switch (state) {
    case EC_ST_UP: return AOS_C_GREEN;
    case EC_ST_SLOW: return AOS_C_YELLOW;
    case EC_ST_DOWN: return AOS_C_RED;
    default: return AOS_C_DIM;
    }
}

const char *ec_state_name(int state)
{
    switch (state) {
    case EC_ST_UP: return _("Anda");
    case EC_ST_SLOW: return _("Lento");
    case EC_ST_DOWN: return _("Caído");
    case EC_ST_PAUSED: return _("En pausa");
    default: return _("Probando");
    }
}
