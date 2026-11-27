/*
 * P4OS - Correo: the connection and SMTP (cr.h).
 *
 * The connection is the HAL's (aos_tls.c, the same Cartero and Impresoras
 * 3D use), declared here rather than included because its header pulls in
 * mbedtls's. TLS from the first byte is aos_tls_open with AOS_TLS_ENCRYPT
 * and verifies the server's certificate against the board's store, always:
 * a certificate that cannot be verified does not connect, and the reason is
 * said (cr_conn_why).
 *
 * STARTTLS (IMAP on 143, SMTP on 587) needs the TLS handshake started on a
 * socket that already spoke in the clear, which the HAL does not offer to
 * apps yet: aos_tls_open negotiates at connect or never. The protocol side
 * is here, behind CR_HAVE_STARTTLS, waiting for a call like
 *
 *     int aos_tls_starttls(aos_tls_t *c, const char *host, int timeout_ms,
 *                          char *why, size_t why_n);
 *
 * (the TLS half of tls_attempt() in aos_tls.c, on c->fd). Until then an
 * account that asks for STARTTLS is refused with CR_E_STARTTLS, and never
 * falls back to the clear: a password must not go out unencrypted because
 * the board could not do better.
 */
#include "cr.h"

#include "aos_hal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef CR_HAVE_STARTTLS
#define CR_HAVE_STARTTLS 0
#endif

typedef struct cr_tls cr_tls_t;
cr_tls_t *aos_tls_new(void);
void      aos_tls_free(cr_tls_t *c);
int       aos_tls_open(cr_tls_t *c, const char *host, int port, unsigned flags, int timeout_ms, char *why,
                       size_t why_n);
bool      aos_tls_send(cr_tls_t *c, const void *buf, int len, int timeout_ms);
int       aos_tls_recv(cr_tls_t *c, void *buf, int max, int timeout_ms);
void      aos_tls_close(cr_tls_t *c);
#if CR_HAVE_STARTTLS
int aos_tls_starttls(cr_tls_t *c, const char *host, int timeout_ms, char *why, size_t why_n);
#endif
#define TLS_ENCRYPT 0x01u
#define TLS_NODELAY 0x02u
#define TLS_TIMEOUT (-2)

/* aos_hal.h's AOS_HTTP_ERR_* that aos_tls_open returns */
#define E_DNS      (-1)
#define E_CONNECT  (-2)
#define E_SIN_HORA (-7)
#define E_TLS      (-8)

#define RBUF 16384
#define SLICE_MS 250

volatile bool cr_net_abort;

struct cr_conn {
    cr_tls_t *t;
    bool      open;
    int       r, w;
    int       err;
    char      why[200];
    char      buf[RBUF];
};

cr_conn_t *cr_conn_new(void)
{
    return cr_alloc(sizeof(cr_conn_t));
}

void cr_conn_close(cr_conn_t *c)
{
    if (!c) return;
    if (c->t) aos_tls_free(c->t);            /* closes too */
    c->t = NULL;
    c->open = false;
    c->r = c->w = 0;
}

void cr_conn_free(cr_conn_t *c)
{
    if (!c) return;
    cr_conn_close(c);
    cr_free(c);
}

bool cr_conn_is_open(const cr_conn_t *c)
{
    return c && c->open;
}

const char *cr_conn_why(const cr_conn_t *c)
{
    return c ? c->why : "";
}

int cr_conn_err(const cr_conn_t *c)
{
    return c ? c->err : CR_E_NET;
}

bool cr_conn_pending(cr_conn_t *c)
{
    return c && c->w > c->r;
}

static int kind_of(int rc)
{
    if (rc == E_SIN_HORA) return CR_E_CLOCK;
    if (rc == E_TLS) return CR_E_TLS;
    return CR_E_NET;
}

int cr_conn_open(cr_conn_t *c, const char *host, int port, int sec, int timeout_ms)
{
    cr_conn_close(c);
    c->why[0] = '\0';
    c->err = CR_E_NONE;
    c->t = aos_tls_new();
    if (!c->t) {
        cr_copy(c->why, sizeof c->why, "out of memory");
        c->err = CR_E_NET;
        return -1;
    }
    unsigned fl = TLS_NODELAY | (sec == CR_SEC_TLS ? TLS_ENCRYPT : 0);
#if !CR_HAVE_STARTTLS
    if (sec == CR_SEC_STARTTLS) {
        cr_copy(c->why, sizeof c->why, "STARTTLS is not available in this firmware");
        c->err = CR_E_STARTTLS;
        aos_tls_free(c->t);
        c->t = NULL;
        return -1;
    }
#endif
    int rc = aos_tls_open(c->t, host, port, fl, timeout_ms, c->why, sizeof c->why);
    if (rc != 0) {
        c->err = kind_of(rc);
        if (!c->why[0]) snprintf(c->why, sizeof c->why, "cannot connect to %s:%d", host, port);
        aos_tls_free(c->t);
        c->t = NULL;
        return -1;
    }
    c->open = true;
    c->r = c->w = 0;
    return 0;
}

int cr_conn_starttls(cr_conn_t *c, const char *host, int timeout_ms)
{
#if CR_HAVE_STARTTLS
    /* nothing may be left over from the clear: an attacker could have put it there */
    c->r = c->w = 0;
    int rc = aos_tls_starttls(c->t, host, timeout_ms, c->why, sizeof c->why);
    if (rc != 0) {
        c->err = kind_of(rc);
        cr_conn_close(c);
        return -1;
    }
    return 0;
#else
    (void)host;
    (void)timeout_ms;
    cr_copy(c->why, sizeof c->why, "STARTTLS is not available in this firmware");
    c->err = CR_E_STARTTLS;
    cr_conn_close(c);
    return -1;
#endif
}

static int fill(cr_conn_t *c, int timeout_ms)
{
    if (!c->open) return -1;
    if (c->r > 0 && c->r == c->w) c->r = c->w = 0;
    if (c->w == RBUF) {
        if (c->r == 0) return -3;                     /* full: the caller reads */
        memmove(c->buf, c->buf + c->r, (size_t)(c->w - c->r));
        c->w -= c->r;
        c->r = 0;
    }
    int k = aos_tls_recv(c->t, c->buf + c->w, RBUF - c->w, timeout_ms);
    if (k == TLS_TIMEOUT) return -2;
    if (k <= 0) {
        cr_copy(c->why, sizeof c->why, k == 0 ? "the server closed the connection" : "the connection broke");
        c->err = CR_E_NET;
        cr_conn_close(c);
        return -1;
    }
    c->w += k;
    return k;
}

/* One line without its CRLF. A line longer than 'max' is cut (the rest is
 * read and dropped), which no IMAP server's responses need: literals carry
 * anything long. */
int cr_conn_line(cr_conn_t *c, char *out, int max, int timeout_ms)
{
    uint64_t deadline = aos_hal_uptime_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    int k = 0;
    for (;;) {
        for (int i = c->r; i < c->w; i++) {
            if (c->buf[i] == '\n') {
                int l = i - c->r;
                int cl = l;
                if (cl && c->buf[c->r + cl - 1] == '\r') cl--;
                int cp = cl < max - 1 - k ? cl : max - 1 - k;
                if (cp > 0) memcpy(out + k, c->buf + c->r, (size_t)cp);
                k += cp > 0 ? cp : 0;
                out[k] = '\0';
                c->r = i + 1;
                return k;
            }
        }
        /* no newline yet: if the buffer is full, take what is there */
        if (c->w - c->r >= RBUF - 1) {
            int cp = c->w - c->r < max - 1 - k ? c->w - c->r : max - 1 - k;
            if (cp > 0) {
                memcpy(out + k, c->buf + c->r, (size_t)cp);
                k += cp;
            }
            c->r = c->w = 0;
        }
        if (cr_net_abort) return -1;
        uint64_t now = aos_hal_uptime_ms();
        int left = now >= deadline ? 0 : (int)(deadline - now);
        if (left > SLICE_MS) left = SLICE_MS;
        int f = fill(c, left > 0 ? left : 1);
        if (f == -1) return -1;
        if (f == -2) {
            if (aos_hal_uptime_ms() >= deadline) {
                if (k) {                          /* half a line: keep it for later */
                    /* it was consumed; give it back to the caller as a line */
                    out[k] = '\0';
                    return k;
                }
                return -2;
            }
        }
    }
}

int cr_conn_read(cr_conn_t *c, void *out, int n, int timeout_ms)
{
    uint8_t *o = out;
    int got = 0;
    uint64_t deadline = aos_hal_uptime_ms() + (uint64_t)timeout_ms;
    while (got < n) {
        if (c->w > c->r) {
            int take = c->w - c->r < n - got ? c->w - c->r : n - got;
            memcpy(o + got, c->buf + c->r, (size_t)take);
            c->r += take;
            got += take;
            continue;
        }
        if (cr_net_abort) return -1;
        uint64_t now = aos_hal_uptime_ms();
        if (now >= deadline) return -2;
        int left = (int)(deadline - now);
        int f = fill(c, left > SLICE_MS ? SLICE_MS : left);
        if (f == -1) return -1;
    }
    return got;
}

bool cr_conn_send(cr_conn_t *c, const void *d, int n)
{
    if (!c->open) return false;
    if (!aos_tls_send(c->t, d, n, 20000)) {
        cr_copy(c->why, sizeof c->why, "could not send to the server");
        c->err = CR_E_NET;
        cr_conn_close(c);
        return false;
    }
    return true;
}

bool cr_conn_puts(cr_conn_t *c, const char *s)
{
    return cr_conn_send(c, s, (int)strlen(s));
}

/* ---- SMTP -------------------------------------------------------------------------------------- */

typedef struct {
    cr_conn_t *c;
    char       line[1024];
    char       text[600];      /* the last reply's text, all its lines */
    bool       auth_plain, auth_login, starttls, size, eightbit;
} smtp_t;

/* A reply: its code, and its lines' text in s->text. */
static int smtp_reply(smtp_t *s, int timeout_ms)
{
    s->text[0] = '\0';
    size_t tn = 0;
    for (int guard = 0; guard < 100; guard++) {
        int k = cr_conn_line(s->c, s->line, sizeof s->line, timeout_ms);
        if (k < 0) return -1;
        if (k < 3) continue;
        int code = atoi(s->line);
        const char *t = k > 4 ? s->line + 4 : "";
        if (s->line[3] == '-' || s->line[3] == ' ') {
            /* capabilities of an EHLO */
            if (cr_istarts(t, "AUTH")) {
                if (cr_istr(t, "PLAIN")) s->auth_plain = true;
                if (cr_istr(t, "LOGIN")) s->auth_login = true;
            }
            if (cr_istarts(t, "STARTTLS")) s->starttls = true;
            if (cr_istarts(t, "8BITMIME")) s->eightbit = true;
            size_t l = strlen(t);
            if (tn + l + 2 < sizeof s->text) {
                if (tn) s->text[tn++] = ' ';
                memcpy(s->text + tn, t, l);
                tn += l;
                s->text[tn] = '\0';
            }
        }
        if (s->line[3] != '-') return code;
    }
    return -1;
}

static int smtp_cmd(smtp_t *s, const char *cmd, int timeout_ms)
{
    if (!cr_conn_puts(s->c, cmd) || !cr_conn_puts(s->c, "\r\n")) return -1;
    return smtp_reply(s, timeout_ms);
}

static void smtp_why(smtp_t *s, char *why, size_t wn, const char *what, int code)
{
    if (code < 0) snprintf(why, wn, "%s: %s", what, cr_conn_why(s->c));
    else snprintf(why, wn, "%s: %d %.150s", what, code, s->text);
}

/* Connects, says hello, encrypts if the account asks it, logs in. 0, or
 * one of CR_E_* with 'why' said. */
static int smtp_open(smtp_t *s, const cr_acct_cfg_t *cfg, const char *pass, char *why, size_t wn)
{
    memset(s, 0, sizeof *s);
    s->c = cr_conn_new();
    if (!s->c) return CR_E_NET;
    if (cr_conn_open(s->c, cfg->smtp_host, cfg->smtp_port, cfg->smtp_sec, 15000) != 0) {
        snprintf(why, wn, "%s", cr_conn_why(s->c));
        return cr_conn_err(s->c);
    }
    int code = smtp_reply(s, 15000);
    if (code != 220) {
        smtp_why(s, why, wn, "greeting", code);
        return CR_E_PROTO;
    }
    char hello[160];
    const char *dom = strchr(cfg->email, '@');
    snprintf(hello, sizeof hello, "EHLO %.100s", dom ? dom + 1 : "p4os.local");
    code = smtp_cmd(s, hello, 15000);
    if (code != 250) {
        smtp_why(s, why, wn, "EHLO", code);
        return CR_E_PROTO;
    }
    if (cfg->smtp_sec == CR_SEC_STARTTLS) {
        if (!s->starttls) {
            snprintf(why, wn, "the server does not offer STARTTLS");
            return CR_E_TLS;
        }
        code = smtp_cmd(s, "STARTTLS", 15000);
        if (code != 220) {
            smtp_why(s, why, wn, "STARTTLS", code);
            return CR_E_TLS;
        }
        if (cr_conn_starttls(s->c, cfg->smtp_host, 15000) != 0) {
            snprintf(why, wn, "%s", cr_conn_why(s->c));
            return cr_conn_err(s->c);
        }
        s->auth_plain = s->auth_login = false;
        code = smtp_cmd(s, hello, 15000);
        if (code != 250) {
            smtp_why(s, why, wn, "EHLO", code);
            return CR_E_PROTO;
        }
    }
    const char *user = cfg->smtp_user[0] ? cfg->smtp_user : cfg->email;
    if (s->auth_plain || !s->auth_login) {
        cr_buf_t raw = {0}, b = {0};
        cr_buf_add(&raw, "", 1);
        cr_buf_str(&raw, user);
        cr_buf_add(&raw, "", 1);
        cr_buf_str(&raw, pass);
        cr_buf_str(&b, "AUTH PLAIN ");
        cr_b64_enc(&b, raw.p, raw.n, 0);
        code = smtp_cmd(s, b.p, 20000);
        memset(raw.p, 0, raw.n);
        memset(b.p, 0, b.n);
        cr_buf_free(&raw);
        cr_buf_free(&b);
    } else {
        code = smtp_cmd(s, "AUTH LOGIN", 15000);
        if (code == 334) {
            cr_buf_t b = {0};
            cr_b64_enc(&b, user, strlen(user), 0);
            code = smtp_cmd(s, b.p, 15000);
            cr_buf_free(&b);
            if (code == 334) {
                cr_b64_enc(&b, pass, strlen(pass), 0);
                code = smtp_cmd(s, b.p, 20000);
                memset(b.p, 0, b.n);
                cr_buf_free(&b);
            }
        }
    }
    if (code != 235) {
        smtp_why(s, why, wn, "login", code);
        return code >= 500 && code < 600 ? CR_E_AUTH : CR_E_PROTO;
    }
    return CR_E_NONE;
}

static void smtp_close(smtp_t *s)
{
    if (s->c && cr_conn_is_open(s->c)) {
        cr_conn_puts(s->c, "QUIT\r\n");
        smtp_reply(s, 2000);
    }
    cr_conn_free(s->c);
    s->c = NULL;
}

int cr_smtp_test(const cr_acct_cfg_t *cfg, const char *pass, char *why, size_t wn)
{
    smtp_t s;
    int e = smtp_open(&s, cfg, pass, why, wn);
    smtp_close(&s);
    return e;
}

int cr_smtp_send(const cr_acct_cfg_t *cfg, const char *pass, const char *from, const char **rcpt, int nrcpt,
                 const char *msg, size_t n, char *why, size_t wn)
{
    smtp_t s;
    int e = smtp_open(&s, cfg, pass, why, wn);
    if (e) {
        smtp_close(&s);
        return e;
    }
    char cmd[200];
    snprintf(cmd, sizeof cmd, "MAIL FROM:<%.150s>%s", from, s.eightbit ? " BODY=8BITMIME" : "");
    int code = smtp_cmd(&s, cmd, 20000);
    if (code != 250) {
        smtp_why(&s, why, wn, "MAIL FROM", code);
        smtp_close(&s);
        return CR_E_PROTO;
    }
    int ok = 0;
    for (int i = 0; i < nrcpt; i++) {
        snprintf(cmd, sizeof cmd, "RCPT TO:<%.150s>", rcpt[i]);
        code = smtp_cmd(&s, cmd, 20000);
        if (code == 250 || code == 251) {
            ok++;
        } else {
            char what[140];
            snprintf(what, sizeof what, "%.120s", rcpt[i]);
            smtp_why(&s, why, wn, what, code);
            smtp_close(&s);
            return CR_E_PROTO;
        }
    }
    if (!ok) {
        snprintf(why, wn, "no recipients");
        smtp_close(&s);
        return CR_E_PROTO;
    }
    code = smtp_cmd(&s, "DATA", 20000);
    if (code != 354) {
        smtp_why(&s, why, wn, "DATA", code);
        smtp_close(&s);
        return CR_E_PROTO;
    }
    /* the message with every line that starts with a dot doubled, in
     * pieces of up to 4 KB */
    char out[4200];
    int k = 0;
    bool bol = true;
    for (size_t i = 0; i < n; i++) {
        if (bol && msg[i] == '.') out[k++] = '.';
        out[k++] = msg[i];
        bol = msg[i] == '\n';
        if (k >= 4096) {
            if (!cr_conn_send(s.c, out, k)) break;
            k = 0;
        }
    }
    if (!bol) {
        out[k++] = '\r';
        out[k++] = '\n';
    }
    memcpy(out + k, ".\r\n", 3);
    k += 3;
    if (!cr_conn_send(s.c, out, k)) {
        smtp_why(&s, why, wn, "sending", -1);
        smtp_close(&s);
        return CR_E_NET;
    }
    code = smtp_reply(&s, 60000);
    if (code != 250) {
        smtp_why(&s, why, wn, "message", code);
        smtp_close(&s);
        return CR_E_PROTO;
    }
    smtp_close(&s);
    return CR_E_NONE;
}
