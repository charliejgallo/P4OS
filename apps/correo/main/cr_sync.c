/*
 * P4OS - Correo: the network thread (cr.h).
 *
 * One thread for every account, so nothing here needs a lock of its own:
 * each account has one IMAP connection, which sits in IDLE on the INBOX
 * when there is nothing to do, and the thread goes round them polling each
 * socket for a few tens of milliseconds. A job from the screens takes the
 * account out of IDLE (DONE), selects what it needs, does its part and lets
 * the loop put IDLE back.
 *
 * Sync is by UID and incremental. A folder opened for the first time gets
 * its newest CR_WINDOW headers (by sequence number, which needs no search);
 * after that only what is above the highest UID known, the flags that
 * changed (CHANGEDSINCE with CONDSTORE, else the flags of the known range)
 * and the UIDs that went (a UID SEARCH of the known range). UIDVALIDITY
 * changing throws the folder's copy away. The headers live in the model
 * (cr_lock) and on the card (cr_store.c), so the lists show at once, also
 * offline.
 *
 * Timeouts are short and sliced (cr_net.c), so destroy() can wait for the
 * thread: the loader unloads the .so right after it.
 */
#include "cr.h"

#include "aos_hal.h"
#include "aos_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define LINE_MAX_B   (256 * 1024)
#define LIT_MAX      12
#define IDLE_RENEW_S (25 * 60)
#define POLL_MS      25
#define STATUS_S     300
#define NOIDLE_S     120
#define CMD_MS       30000

/* ---- shared with the screens -------------------------------------------------------------------- */

cr_model_t CR;
static void      *s_mx;
static void      *s_qmx;
static cr_job_t  *s_jobs, *s_done;
static cr_event_t s_ev[16];
static int        s_ev_n;
static volatile bool s_stop, s_running, s_reload, s_hidden;
static volatile int  s_kick = -1;
static uint32_t   s_ver_dirty;

void cr_lock(void)
{
    if (s_mx) aos_hal_mutex_lock(s_mx);
}

void cr_unlock(void)
{
    if (s_mx) aos_hal_mutex_unlock(s_mx);
}

void cr_touch(void)
{
    CR.ver++;
}

static void qlock(void)
{
    if (s_qmx) aos_hal_mutex_lock(s_qmx);
}

static void qunlock(void)
{
    if (s_qmx) aos_hal_mutex_unlock(s_qmx);
}

cr_job_t *cr_job_new(int kind)
{
    cr_job_t *j = cr_alloc(sizeof *j);
    if (j) {
        j->kind = kind;
        j->acct = j->folder = -1;
    }
    return j;
}

void cr_job_free(cr_job_t *j)
{
    if (!j) return;
    if (j->out) {
        if (j->kind == CR_J_OPEN) cr_msg_free(j->out);
        else if (j->kind == CR_J_IMAGE) aos_hal_image_free(j->out);
        else cr_free(j->out);
    }
    cr_free(j->in);
    cr_out_free(j->out_msg);
    cr_free(j->cfg);
    memset(j->pass, 0, sizeof j->pass);
    cr_free(j);
}

void cr_job_put(cr_job_t *j)
{
    if (!j) return;
    qlock();
    cr_job_t **p = &s_jobs;
    while (*p) p = &(*p)->next;
    j->next = NULL;
    *p = j;
    qunlock();
}

cr_job_t *cr_job_done(void)
{
    qlock();
    cr_job_t *j = s_done;
    if (j) s_done = j->next;
    qunlock();
    if (j) j->next = NULL;
    return j;
}

static void job_finish(cr_job_t *j)
{
    qlock();
    cr_job_t **p = &s_done;
    while (*p) p = &(*p)->next;
    j->next = NULL;
    *p = j;
    qunlock();
}

/* the next job for this account (or for none, acct < 0, if any is -1) */
static cr_job_t *job_take(int acct)
{
    qlock();
    cr_job_t **p = &s_jobs;
    while (*p && (*p)->acct != acct) p = &(*p)->next;
    cr_job_t *j = *p;
    if (j) *p = j->next;
    qunlock();
    return j;
}

static bool job_waiting(int acct)
{
    qlock();
    cr_job_t *p = s_jobs;
    while (p && p->acct != acct) p = p->next;
    qunlock();
    return p != NULL;
}

void cr_event_put(const cr_event_t *e)
{
    qlock();
    if (s_ev_n < 16) s_ev[s_ev_n++] = *e;
    qunlock();
}

bool cr_event_take(cr_event_t *e)
{
    qlock();
    bool ok = s_ev_n > 0;
    if (ok) {
        *e = s_ev[0];
        memmove(s_ev, s_ev + 1, sizeof s_ev[0] * (size_t)(s_ev_n - 1));
        s_ev_n--;
    }
    qunlock();
    return ok;
}

/* ---- the model's folders ----------------------------------------------------------------------- */

int cr_folder_special(const cr_acct_t *a, int sp)
{
    for (int i = 0; i < a->nf; i++) {
        if (a->f[i].special == sp && !a->f[i].gone) return i;
    }
    return -1;
}

int cr_folder_find(const cr_acct_t *a, const char *raw)
{
    for (int i = 0; i < a->nf; i++) {
        if (!strcmp(a->f[i].raw, raw)) return i;
    }
    return -1;
}

static int hdr_index(const cr_folder_t *f, uint32_t uid)
{
    int lo = 0, hi = f->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->h[mid].uid == uid) return mid;
        if (f->h[mid].uid < uid) lo = mid + 1;
        else hi = mid - 1;
    }
    return -(lo + 1);
}

cr_hdr_t *cr_folder_hdr(cr_folder_t *f, uint32_t uid)
{
    int i = hdr_index(f, uid);
    return i >= 0 ? &f->h[i] : NULL;
}

void cr_folder_put(cr_folder_t *f, const cr_hdr_t *h)
{
    int i = hdr_index(f, h->uid);
    if (i >= 0) {
        f->h[i] = *h;
        f->dirty = true;
        return;
    }
    i = -i - 1;
    if (f->n == f->cap) {
        int c = f->cap ? f->cap * 2 : 128;
        cr_hdr_t *nh = cr_realloc(f->h, sizeof(cr_hdr_t) * (size_t)c);
        if (!nh) return;
        f->h = nh;
        f->cap = c;
    }
    memmove(&f->h[i + 1], &f->h[i], sizeof(cr_hdr_t) * (size_t)(f->n - i));
    f->h[i] = *h;
    f->n++;
    f->dirty = true;
}

void cr_folder_drop(cr_folder_t *f, uint32_t uid)
{
    int i = hdr_index(f, uid);
    if (i < 0) return;
    memmove(&f->h[i], &f->h[i + 1], sizeof(cr_hdr_t) * (size_t)(f->n - i - 1));
    f->n--;
    f->dirty = true;
}

/* ---- per account, the thread's own ------------------------------------------------------------- */

typedef struct {
    cr_conn_t *c;
    int        tagn;
    char       tag[16];
    bool       cap_idle, cap_move, cap_uidplus, cap_condstore, cap_litplus, cap_sasl_ir, cap_plain, cap_special;
    int        sel;                 /* selected folder, -1 none */
    bool       sel_ro;
    bool       idling;
    uint32_t   idle_since;
    uint64_t   retry_at;
    int        fails;
    bool       dirty;               /* the server said something changed */
    uint32_t   last_status, last_poll;
    bool       first_sync_done;
    bool       inbox_had_cache;
    char       last[300];           /* the last tagged answer's text */
    /* what SELECT said */
    uint32_t   s_exists, s_uidvalidity, s_uidnext;
    char       s_modseq[24];
    bool       s_nomodseq;
} wk_t;

static wk_t     W[CR_ACCT_MAX];
static char    *s_line;             /* LINE_MAX_B, the thread's */
static uint32_t s_gen;

typedef struct {
    char  *t;
    size_t n;
    char  *lit[LIT_MAX];
    size_t litn[LIT_MAX];
    int    nlit;
    int    next;                    /* the parser's next literal */
} resp_t;

static void resp_free(resp_t *r)
{
    for (int i = 0; i < r->nlit; i++) cr_free(r->lit[i]);
    cr_free(r->t);
    memset(r, 0, sizeof *r);
}

static void set_state(int ai, int st, int kind, const char *why)
{
    cr_lock();
    if (ai < CR.na && CR.gen == s_gen) {
        cr_acct_t *a = &CR.a[ai];
        if (a->state != st || (why && strcmp(a->err, why))) {
            a->state = st;
            a->err_kind = kind;
            cr_copy(a->err, sizeof a->err, why ? why : "");
            cr_touch();
        }
    }
    cr_unlock();
    cr_event_t e = {CR_EV_STATE, ai, st, ""};
    cr_event_put(&e);
}

static void wk_close(int ai)
{
    wk_t *w = &W[ai];
    if (w->c && cr_conn_is_open(w->c)) {
        if (w->idling) cr_conn_puts(w->c, "DONE\r\n");
        char t[32];
        snprintf(t, sizeof t, "Z%d LOGOUT\r\n", ++w->tagn);
        cr_conn_puts(w->c, t);
    }
    cr_conn_free(w->c);
    w->c = NULL;
    w->idling = false;
    w->sel = -1;
}

/* A response: a line and its literals, read to the end. 0, -2 nothing
 * within the timeout, -1 the connection is gone. */
static int read_resp(wk_t *w, resp_t *r, int timeout_ms)
{
    memset(r, 0, sizeof *r);
    cr_buf_t b = {0};
    int k = cr_conn_line(w->c, s_line, LINE_MAX_B, timeout_ms);
    if (k < 0) return k;
    for (int guard = 0; guard < 64; guard++) {
        cr_buf_add(&b, s_line, (size_t)k);
        /* a literal at the end: {n} */
        size_t n = (size_t)k;
        if (n >= 3 && s_line[n - 1] == '}') {
            size_t o = n - 2;
            while (o > 0 && s_line[o] >= '0' && s_line[o] <= '9') o--;
            if (s_line[o] == '{' && o < n - 2) {
                long len = strtol(s_line + o + 1, NULL, 10);
                char *lit = cr_alloc((size_t)len + 1);
                if (!lit || len < 0) {
                    cr_buf_free(&b);
                    return -1;
                }
                if (cr_conn_read(w->c, lit, (int)len, CMD_MS) != len) {
                    cr_free(lit);
                    cr_buf_free(&b);
                    return -1;
                }
                if (r->nlit < LIT_MAX) {
                    r->lit[r->nlit] = lit;
                    r->litn[r->nlit] = (size_t)len;
                    r->nlit++;
                } else {
                    cr_free(lit);
                }
                k = cr_conn_line(w->c, s_line, LINE_MAX_B, CMD_MS);
                if (k < 0) {
                    cr_buf_free(&b);
                    return -1;
                }
                continue;
            }
        }
        break;
    }
    r->t = cr_buf_take(&b);
    r->n = strlen(r->t);
    return 0;
}

/* ---- the response's little language ------------------------------------------------------------ */

typedef struct {
    const char *p, *e;
    resp_t     *r;
} tok_t;

static void skip_sp(tok_t *t)
{
    while (t->p < t->e && *t->p == ' ') t->p++;
}

/* An atom, number, quoted string or literal, into out. NIL is "". */
static bool tok_str(tok_t *t, char *out, size_t on, const char **lit, size_t *litn)
{
    skip_sp(t);
    if (lit) *lit = NULL;
    if (t->p >= t->e) return false;
    if (*t->p == '"') {
        t->p++;
        size_t k = 0;
        while (t->p < t->e && *t->p != '"') {
            if (*t->p == '\\' && t->p + 1 < t->e) t->p++;
            if (k + 1 < on) out[k++] = *t->p;
            t->p++;
        }
        if (t->p < t->e) t->p++;
        if (on) out[k] = '\0';
        return true;
    }
    if (*t->p == '{') {
        const char *c = memchr(t->p, '}', (size_t)(t->e - t->p));
        t->p = c ? c + 1 : t->e;
        if (t->r->next < t->r->nlit) {
            int i = t->r->next++;
            if (lit) {
                *lit = t->r->lit[i];
                *litn = t->r->litn[i];
            }
            if (on) cr_ncopy(out, on, t->r->lit[i], t->r->litn[i]);
        } else if (on) {
            out[0] = '\0';
        }
        return true;
    }
    size_t k = 0;
    int depth = 0;
    while (t->p < t->e && (depth || (*t->p != ' ' && *t->p != ')' && *t->p != '('))) {
        if (*t->p == '[') depth++;
        if (*t->p == ']') depth--;
        if (k + 1 < on) out[k++] = *t->p;
        t->p++;
    }
    if (on) out[k] = '\0';
    if (k == 3 && !strncasecmp(out, "NIL", 3)) out[0] = '\0';
    return k > 0;
}

/* A parenthesised list, its raw text (without the brackets). */
static bool tok_list(tok_t *t, const char **s, size_t *n)
{
    skip_sp(t);
    if (t->p >= t->e || *t->p != '(') return false;
    int depth = 0;
    const char *st = t->p + 1;
    bool q = false;
    for (; t->p < t->e; t->p++) {
        char c = *t->p;
        if (q) {
            if (c == '\\') t->p++;
            else if (c == '"') q = false;
            continue;
        }
        if (c == '"') q = true;
        else if (c == '(') depth++;
        else if (c == ')' && --depth == 0) {
            *s = st;
            *n = (size_t)(t->p - st);
            t->p++;
            return true;
        } else if (c == '{') {
            /* a literal inside a list (rare): skip it */
            const char *cc = memchr(t->p, '}', (size_t)(t->e - t->p));
            if (cc) t->p = cc;
            if (t->r->next < t->r->nlit) t->r->next++;
        }
    }
    return false;
}

static uint16_t flags_of(const char *s, size_t n)
{
    uint16_t f = 0;
    if (cr_imem(s, n, "\\Seen")) f |= CR_F_SEEN;
    if (cr_imem(s, n, "\\Answered")) f |= CR_F_ANSWERED;
    if (cr_imem(s, n, "\\Flagged")) f |= CR_F_FLAGGED;
    if (cr_imem(s, n, "\\Deleted")) f |= CR_F_DELETED;
    if (cr_imem(s, n, "\\Draft")) f |= CR_F_DRAFT;
    if (cr_imem(s, n, "$Forwarded")) f |= CR_F_FORWARDED;
    return f;
}

/* "[CODE value]" in a text */
static bool resp_code(const char *t, const char *code, char *out, size_t on)
{
    char pat[40];
    snprintf(pat, sizeof pat, "[%s", code);
    const char *p = cr_istr(t, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    size_t k = 0;
    while (*p && *p != ']' && k + 1 < on) out[k++] = *p++;
    out[k] = '\0';
    return true;
}

static void caps_parse(wk_t *w, const char *s)
{
    w->cap_idle = cr_istr(s, " IDLE") != NULL;
    w->cap_move = cr_istr(s, " MOVE") != NULL;
    w->cap_uidplus = cr_istr(s, "UIDPLUS") != NULL;
    w->cap_condstore = cr_istr(s, "CONDSTORE") != NULL || cr_istr(s, "QRESYNC") != NULL;
    w->cap_litplus = cr_istr(s, "LITERAL+") != NULL || cr_istr(s, "LITERAL-") != NULL;
    w->cap_sasl_ir = cr_istr(s, "SASL-IR") != NULL;
    w->cap_plain = cr_istr(s, "AUTH=PLAIN") != NULL;
    w->cap_special = cr_istr(s, "SPECIAL-USE") != NULL;
}

typedef void (*on_resp_t)(int ai, resp_t *r, void *ctx);

/* What every untagged response may say, whoever asked. */
static void untagged_common(int ai, resp_t *r)
{
    wk_t *w = &W[ai];
    const char *t = r->t + 2;
    char v[40];
    if (cr_istarts(t, "CAPABILITY ")) caps_parse(w, t + 10);
    else if (cr_istarts(t, "OK ")) {
        char caps[1024] = " ";
        if (resp_code(t, "CAPABILITY", caps + 1, sizeof caps - 1)) caps_parse(w, caps);
        if (resp_code(t, "UIDVALIDITY", v, sizeof v)) w->s_uidvalidity = (uint32_t)strtoul(v, NULL, 10);
        if (resp_code(t, "UIDNEXT", v, sizeof v)) w->s_uidnext = (uint32_t)strtoul(v, NULL, 10);
        if (resp_code(t, "HIGHESTMODSEQ", v, sizeof v)) cr_copy(w->s_modseq, sizeof w->s_modseq, v);
        if (cr_istr(t, "[NOMODSEQ]")) w->s_nomodseq = true;
    } else if (t[0] >= '0' && t[0] <= '9') {
        const char *sp = strchr(t, ' ');
        if (sp && cr_istarts(sp + 1, "EXISTS")) {
            uint32_t n = (uint32_t)strtoul(t, NULL, 10);
            if (n != w->s_exists) w->dirty = true;
            w->s_exists = n;
        } else if (sp && (cr_istarts(sp + 1, "EXPUNGE") || cr_istarts(sp + 1, "FETCH"))) {
            w->dirty = true;
            if (cr_istarts(sp + 1, "EXPUNGE") && w->s_exists) w->s_exists--;
        }
    } else if (cr_istarts(t, "VANISHED")) {
        w->dirty = true;
    }
}

/* Sends a command (the tag in front, CRLF after) and reads to its tagged
 * answer. 0 OK, 1 NO, 2 BAD, -1 the connection. */
static int cmd_send(int ai, const char *text)
{
    wk_t *w = &W[ai];
    snprintf(w->tag, sizeof w->tag, "A%d", ++w->tagn);
    char head[24];
    snprintf(head, sizeof head, "%s ", w->tag);
    if (!cr_conn_puts(w->c, head) || !cr_conn_puts(w->c, text) || !cr_conn_puts(w->c, "\r\n")) return -1;
    return 0;
}

static int cmd_wait(int ai, on_resp_t on, void *ctx, int timeout_ms)
{
    wk_t *w = &W[ai];
    for (;;) {
        resp_t r;
        int k = read_resp(w, &r, timeout_ms);
        if (k == -2) {
            cr_copy(w->last, sizeof w->last, "the server did not answer in time");
            wk_close(ai);
            return -1;
        }
        if (k < 0) {
            cr_copy(w->last, sizeof w->last, cr_conn_why(w->c));
            return -1;
        }
        size_t tl = strlen(w->tag);
        if (!strncmp(r.t, w->tag, tl) && r.t[tl] == ' ') {
            const char *s = r.t + tl + 1;
            int rc = cr_istarts(s, "OK") ? 0 : cr_istarts(s, "NO") ? 1 : 2;
            const char *txt = strchr(s, ' ');
            cr_copy(w->last, sizeof w->last, txt ? txt + 1 : s);
            char v[40];
            if (resp_code(s, "UIDNEXT", v, sizeof v)) w->s_uidnext = (uint32_t)strtoul(v, NULL, 10);
            char caps[1024] = " ";
            if (resp_code(s, "CAPABILITY", caps + 1, sizeof caps - 1)) caps_parse(w, caps);
            if (on && rc == 0 && cr_istr(s, "[APPENDUID")) on(ai, &r, ctx);
            resp_free(&r);
            return rc;
        }
        if (r.t[0] == '*' && r.t[1] == ' ') {
            untagged_common(ai, &r);
            if (cr_istarts(r.t + 2, "BYE")) cr_copy(w->last, sizeof w->last, r.t + 6);
            if (on) on(ai, &r, ctx);
        }
        resp_free(&r);
    }
}

static int cmd(int ai, on_resp_t on, void *ctx, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int cmd(int ai, on_resp_t on, void *ctx, const char *fmt, ...)
{
    char text[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (cmd_send(ai, text) < 0) return -1;
    return cmd_wait(ai, on, ctx, CMD_MS);
}

/* A literal in the middle of a command: with LITERAL+ at once, else after
 * the server's "+". */
static bool send_literal(int ai, const void *d, size_t n)
{
    wk_t *w = &W[ai];
    char h[24];
    snprintf(h, sizeof h, "{%u%s}\r\n", (unsigned)n, w->cap_litplus ? "+" : "");
    if (!cr_conn_puts(w->c, h)) return false;
    if (!w->cap_litplus) {
        for (;;) {
            int k = cr_conn_line(w->c, s_line, LINE_MAX_B, CMD_MS);
            if (k < 0) return false;
            if (s_line[0] == '+') break;
            if (s_line[0] != '*') return false;      /* refused: the tag answered */
        }
    }
    return cr_conn_send(w->c, d, (int)n);
}

/* An IMAP string: quoted if it can be, a literal if not. */
static bool send_astring(int ai, const char *s)
{
    bool plain = true;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p >= 0x80 || *p < 32) plain = false;
    }
    if (!plain) return send_literal(ai, s, strlen(s));
    cr_buf_t b = {0};
    cr_buf_add(&b, "\"", 1);
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') cr_buf_add(&b, "\\", 1);
        cr_buf_add(&b, p, 1);
    }
    cr_buf_add(&b, "\"", 1);
    bool ok = cr_conn_send(W[ai].c, b.p, (int)b.n);
    memset(b.p, 0, b.n);
    cr_buf_free(&b);
    return ok;
}

/* "quoted" for a folder name inside a printf'd command */
static void quoted(char *out, size_t n, const char *s)
{
    size_t k = 0;
    if (k + 1 < n) out[k++] = '"';
    for (; *s && k + 3 < n; s++) {
        if (*s == '"' || *s == '\\') out[k++] = '\\';
        out[k++] = *s;
    }
    out[k++] = '"';
    out[k] = '\0';
}

/* ---- connecting -------------------------------------------------------------------------------- */

static void cfg_copy(int ai, cr_acct_cfg_t *cfg)
{
    cr_lock();
    *cfg = CR.a[ai].cfg;
    cr_unlock();
}

/* Connects and logs in. 0, or a CR_E_* with the reason in w->last. */
static int imap_login(int ai, const cr_acct_cfg_t *cfg, const char *pass)
{
    wk_t *w = &W[ai];
    wk_close(ai);
    w->c = cr_conn_new();
    if (!w->c) return CR_E_NET;
    w->sel = -1;
    w->tagn = 0;
    if (cr_conn_open(w->c, cfg->imap_host, cfg->imap_port, cfg->imap_sec, 10000) != 0) {
        cr_copy(w->last, sizeof w->last, cr_conn_why(w->c));
        return cr_conn_err(w->c);
    }
    resp_t r;
    if (read_resp(w, &r, 15000) != 0) {
        cr_copy(w->last, sizeof w->last, "no greeting from the server");
        return CR_E_PROTO;
    }
    if (!cr_istarts(r.t, "* OK") && !cr_istarts(r.t, "* PREAUTH")) {
        cr_copy(w->last, sizeof w->last, r.t);
        resp_free(&r);
        return CR_E_PROTO;
    }
    untagged_common(ai, &r);
    resp_free(&r);
    if (!w->cap_plain && !w->cap_idle && cmd(ai, NULL, NULL, "CAPABILITY") < 0) return CR_E_NET;
    if (cfg->imap_sec == CR_SEC_STARTTLS) {
        if (cmd(ai, NULL, NULL, "STARTTLS") != 0) return CR_E_TLS;
        if (cr_conn_starttls(w->c, cfg->imap_host, 15000) != 0) {
            cr_copy(w->last, sizeof w->last, cr_conn_why(w->c));
            return cr_conn_err(w->c);
        }
        if (cmd(ai, NULL, NULL, "CAPABILITY") != 0) return CR_E_PROTO;
    }
    const char *user = cfg->imap_user[0] ? cfg->imap_user : cfg->email;
    int rc;
    if (w->cap_plain) {
        cr_buf_t raw = {0}, b = {0};
        cr_buf_add(&raw, "", 1);
        cr_buf_str(&raw, user);
        cr_buf_add(&raw, "", 1);
        cr_buf_str(&raw, pass);
        cr_b64_enc(&b, raw.p, raw.n, 0);
        memset(raw.p, 0, raw.n);
        cr_buf_free(&raw);
        if (w->cap_sasl_ir) {
            snprintf(w->tag, sizeof w->tag, "A%d", ++w->tagn);
            char head[40];
            snprintf(head, sizeof head, "%s AUTHENTICATE PLAIN ", w->tag);
            bool ok = cr_conn_puts(w->c, head) && cr_conn_send(w->c, b.p, (int)b.n) && cr_conn_puts(w->c, "\r\n");
            rc = ok ? cmd_wait(ai, NULL, NULL, CMD_MS) : -1;
        } else {
            rc = -1;
            if (cmd_send(ai, "AUTHENTICATE PLAIN") == 0) {
                int k = cr_conn_line(w->c, s_line, LINE_MAX_B, CMD_MS);
                if (k >= 0 && s_line[0] == '+') {
                    if (cr_conn_send(w->c, b.p, (int)b.n) && cr_conn_puts(w->c, "\r\n")) rc = cmd_wait(ai, NULL, NULL, CMD_MS);
                } else if (k >= 0) {
                    cr_copy(w->last, sizeof w->last, s_line);
                    rc = 1;
                }
            }
        }
        memset(b.p, 0, b.n);
        cr_buf_free(&b);
    } else {
        snprintf(w->tag, sizeof w->tag, "A%d", ++w->tagn);
        char head[32];
        snprintf(head, sizeof head, "%s LOGIN ", w->tag);
        bool ok = cr_conn_puts(w->c, head) && send_astring(ai, user) && cr_conn_puts(w->c, " ") &&
                  send_astring(ai, pass) && cr_conn_puts(w->c, "\r\n");
        rc = ok ? cmd_wait(ai, NULL, NULL, CMD_MS) : -1;
    }
    if (rc < 0) return CR_E_NET;
    if (rc != 0) return CR_E_AUTH;
    /* some servers say their capabilities only after the login */
    if (!w->cap_idle && !w->cap_move && cmd(ai, NULL, NULL, "CAPABILITY") < 0) return CR_E_NET;
    if (w->cap_condstore) cmd(ai, NULL, NULL, "ENABLE CONDSTORE");
    return CR_E_NONE;
}

/* ---- LIST and STATUS --------------------------------------------------------------------------- */

typedef struct {
    char     raw[128];
    char     delim;
    uint8_t  special;
    bool     noselect;
} lf_t;

typedef struct {
    lf_t *l;
    int   n;
} lists_t;

static uint8_t special_by_name(const char *raw)
{
    const char *base = strrchr(raw, '/');
    const char *dot = strrchr(raw, '.');
    if (dot && (!base || dot > base)) base = dot;
    base = base ? base + 1 : raw;
    static const struct {
        const char *n;
        uint8_t sp;
    } T[] = {{"Sent", CR_SP_SENT},       {"Sent Items", CR_SP_SENT},     {"Sent Messages", CR_SP_SENT},
             {"Sent Mail", CR_SP_SENT},  {"Enviados", CR_SP_SENT},       {"Elementos enviados", CR_SP_SENT},
             {"Drafts", CR_SP_DRAFTS},   {"Borradores", CR_SP_DRAFTS},   {"Trash", CR_SP_TRASH},
             {"Deleted Items", CR_SP_TRASH}, {"Deleted Messages", CR_SP_TRASH}, {"Papelera", CR_SP_TRASH},
             {"Bin", CR_SP_TRASH},       {"Junk", CR_SP_JUNK},           {"Spam", CR_SP_JUNK},
             {"Junk E-mail", CR_SP_JUNK}, {"Correo no deseado", CR_SP_JUNK}, {"Archive", CR_SP_ARCHIVE},
             {"Archivo", CR_SP_ARCHIVE}, {"Archivados", CR_SP_ARCHIVE},  {"All Mail", CR_SP_ALL}};
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (!strcasecmp(base, T[i].n)) return T[i].sp;
    }
    return CR_SP_NONE;
}

static void on_list(int ai, resp_t *r, void *ctx)
{
    (void)ai;
    lists_t *L = ctx;
    if (!cr_istarts(r->t, "* LIST ") && !cr_istarts(r->t, "* LSUB ")) return;
    tok_t t = {r->t + 7, r->t + r->n, r, };
    const char *fl;
    size_t fn;
    if (!tok_list(&t, &fl, &fn)) return;
    char delim[8], name[256];
    tok_str(&t, delim, sizeof delim, NULL, NULL);
    if (!tok_str(&t, name, sizeof name, NULL, NULL) || !name[0]) return;
    if (L->n >= CR_FOLDER_MAX) return;
    lf_t *f = &L->l[L->n++];
    memset(f, 0, sizeof *f);
    cr_copy(f->raw, sizeof f->raw, name);
    f->delim = delim[0];
    f->noselect = cr_imem(fl, fn, "\\Noselect") || cr_imem(fl, fn, "\\NonExistent");
    if (!strcasecmp(name, "INBOX")) f->special = CR_SP_INBOX;
    else if (cr_imem(fl, fn, "\\Sent")) f->special = CR_SP_SENT;
    else if (cr_imem(fl, fn, "\\Drafts")) f->special = CR_SP_DRAFTS;
    else if (cr_imem(fl, fn, "\\Trash")) f->special = CR_SP_TRASH;
    else if (cr_imem(fl, fn, "\\Junk")) f->special = CR_SP_JUNK;
    else if (cr_imem(fl, fn, "\\Archive")) f->special = CR_SP_ARCHIVE;
    else if (cr_imem(fl, fn, "\\All")) f->special = CR_SP_ALL;
    else if (cr_imem(fl, fn, "\\Flagged")) f->special = CR_SP_FLAGGED;
    else f->special = special_by_name(name);
}

static void folder_names(cr_folder_t *f, const lf_t *l)
{
    cr_copy(f->raw, sizeof f->raw, l->raw);
    f->delim = l->delim;
    f->special = l->special;
    f->noselect = l->noselect;
    char utf[256];
    cr_mutf7_dec(utf, sizeof utf, l->raw);
    const char *base = utf;
    int depth = 0;
    if (l->delim) {
        for (const char *p = utf; *p; p++) {
            if (*p == l->delim) {
                depth++;
                base = p + 1;
            }
        }
    }
    cr_copy(f->name, sizeof f->name, base);
    f->depth = (uint8_t)(depth > 6 ? 6 : depth);
}

static void on_status(int ai, resp_t *r, void *ctx)
{
    (void)ctx;
    if (!cr_istarts(r->t, "* STATUS ")) return;
    tok_t t = {r->t + 9, r->t + r->n, r, };
    char name[256];
    if (!tok_str(&t, name, sizeof name, NULL, NULL)) return;
    const char *l;
    size_t ln;
    if (!tok_list(&t, &l, &ln)) return;
    char tmp[300];
    cr_ncopy(tmp, sizeof tmp, l, ln);
    uint32_t msgs = 0, unseen = 0, next = 0;
    bool hm = false, hu = false;
    char *p = tmp;
    while (*p) {
        while (*p == ' ') p++;
        char *k = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        uint32_t v = (uint32_t)strtoul(p, &p, 10);
        if (!strcasecmp(k, "MESSAGES")) msgs = v, hm = true;
        else if (!strcasecmp(k, "UNSEEN")) unseen = v, hu = true;
        else if (!strcasecmp(k, "UIDNEXT")) next = v;
    }
    cr_lock();
    if (CR.gen == s_gen && ai < CR.na) {
        cr_acct_t *a = &CR.a[ai];
        int fi = cr_folder_find(a, name);
        if (fi >= 0) {
            cr_folder_t *f = &a->f[fi];
            if (hm) f->exists = msgs;
            if (hu && f->unseen != unseen) {
                f->unseen = unseen;
                cr_touch();
            }
            /* something new there: its list is out of date */
            if (next && f->uidnext && next != f->uidnext) f->synced = false;
            if (f->special == CR_SP_INBOX) a->unseen = f->unseen;
        }
    }
    cr_unlock();
}

static int do_list(int ai)
{
    lists_t L = {cr_alloc(sizeof(lf_t) * CR_FOLDER_MAX), 0};
    if (!L.l) return -1;
    int rc = cmd(ai, on_list, &L, "LIST \"\" \"*\"");
    if (rc != 0) {
        cr_free(L.l);
        return rc;
    }
    cr_lock();
    if (CR.gen == s_gen) {
        cr_acct_t *a = &CR.a[ai];
        for (int i = 0; i < a->nf; i++) a->f[i].gone = true;
        bool have_inbox = false;
        for (int k = 0; k < L.n; k++) {
            if (L.l[k].special == CR_SP_INBOX) have_inbox = true;
            int fi = cr_folder_find(a, L.l[k].raw);
            if (fi < 0 && a->nf < CR_FOLDER_MAX) {
                fi = a->nf++;
                memset(&a->f[fi], 0, sizeof a->f[fi]);
            }
            if (fi < 0) continue;
            folder_names(&a->f[fi], &L.l[k]);
            a->f[fi].gone = false;
        }
        (void)have_inbox;
        cr_touch();
    }
    cr_unlock();
    cr_free(L.l);
    cr_lock();
    cr_acct_t copy_a = CR.a[ai];
    cr_unlock();
    cr_folders_save(&copy_a);
    return 0;
}

static void do_status(int ai, bool all)
{
    wk_t *w = &W[ai];
    char names[CR_FOLDER_MAX][130];
    int n = 0;
    cr_lock();
    if (CR.gen == s_gen) {
        cr_acct_t *a = &CR.a[ai];
        for (int i = 0; i < a->nf && n < CR_FOLDER_MAX; i++) {
            if (a->f[i].noselect || a->f[i].gone || i == w->sel) continue;
            if (!all && a->f[i].special != CR_SP_INBOX && a->f[i].special != CR_SP_DRAFTS &&
                a->f[i].special != CR_SP_JUNK && !a->f[i].unseen)
                continue;
            quoted(names[n++], sizeof names[0], a->f[i].raw);
        }
    }
    cr_unlock();
    for (int i = 0; i < n && !s_stop; i++) {
        if (cmd(ai, on_status, NULL, "STATUS %s (MESSAGES UNSEEN UIDNEXT)", names[i]) < 0) return;
    }
    w->last_status = cr_now() ? cr_now() : (uint32_t)(aos_hal_uptime_ms() / 1000);
}

/* ---- selecting and syncing --------------------------------------------------------------------- */

static int select_folder(int ai, int fi, bool force)
{
    wk_t *w = &W[ai];
    if (w->sel == fi && !force) return 0;
    char raw[260];
    cr_lock();
    quoted(raw, sizeof raw, CR.a[ai].f[fi].raw);
    cr_unlock();
    w->s_exists = w->s_uidvalidity = w->s_uidnext = 0;
    w->s_modseq[0] = '\0';
    w->s_nomodseq = false;
    int rc = cmd(ai, NULL, NULL, "SELECT %s%s", raw, w->cap_condstore ? " (CONDSTORE)" : "");
    w->sel = rc == 0 ? fi : -1;
    /* what SELECT itself said (EXISTS) is not news */
    w->dirty = false;
    return rc;
}

typedef struct {
    cr_hdr_t *h;
    int       n, cap;
    int       ai, fi;
    char      modseq[24];
} hdrs_t;

static void hdrs_add(hdrs_t *H, const cr_hdr_t *h)
{
    if (H->n == H->cap) {
        int c = H->cap ? H->cap * 2 : 64;
        cr_hdr_t *nh = cr_realloc(H->h, sizeof(cr_hdr_t) * (size_t)c);
        if (!nh) return;
        H->h = nh;
        H->cap = c;
    }
    H->h[H->n++] = *h;
}

/* * n FETCH (UID .. FLAGS (..) RFC822.SIZE .. INTERNALDATE ".." BODY[HEADER.FIELDS (..)] {n}) */
static void on_fetch_hdr(int ai, resp_t *r, void *ctx)
{
    (void)ai;
    hdrs_t *H = ctx;
    const char *p = r->t + 2;
    while (*p >= '0' && *p <= '9') p++;
    if (!cr_istarts(p, " FETCH (")) return;
    tok_t t = {p + 8, r->t + r->n, r, };
    cr_hdr_t h;
    memset(&h, 0, sizeof h);
    h.acct = (uint8_t)H->ai;
    h.folder = (uint8_t)H->fi;
    bool have_fields = false;
    char key[200];
    while (t.p < t.e && *t.p != ')') {
        if (!tok_str(&t, key, sizeof key, NULL, NULL)) break;
        if (!strcasecmp(key, "UID")) {
            char v[24];
            tok_str(&t, v, sizeof v, NULL, NULL);
            h.uid = (uint32_t)strtoul(v, NULL, 10);
        } else if (!strcasecmp(key, "FLAGS")) {
            const char *l;
            size_t ln;
            if (tok_list(&t, &l, &ln)) h.flags |= flags_of(l, ln);
        } else if (!strcasecmp(key, "RFC822.SIZE")) {
            char v[24];
            tok_str(&t, v, sizeof v, NULL, NULL);
            h.size = (uint32_t)strtoul(v, NULL, 10);
        } else if (!strcasecmp(key, "INTERNALDATE")) {
            char v[64];
            tok_str(&t, v, sizeof v, NULL, NULL);
            if (!h.date) h.date = cr_idate_parse(v);
        } else if (!strcasecmp(key, "MODSEQ")) {
            const char *l;
            size_t ln;
            tok_list(&t, &l, &ln);
        } else if (cr_istarts(key, "BODY[")) {
            const char *lit = NULL;
            size_t litn = 0;
            tok_str(&t, NULL, 0, &lit, &litn);
            if (lit) {
                uint32_t idate = h.date;
                h.date = 0;
                cr_hdr_from_fields(&h, lit, litn);
                if (!h.date) h.date = idate;
                have_fields = true;
            }
        } else {
            /* something not asked for: its value is skipped */
            skip_sp(&t);
            const char *l;
            size_t ln;
            if (t.p < t.e && *t.p == '(') tok_list(&t, &l, &ln);
            else tok_str(&t, NULL, 0, NULL, NULL);
        }
    }
    if (!h.uid) return;
    if (!have_fields) h.mid_h = 0xFFFFFFFFu;        /* only flags: merged, not inserted */
    hdrs_add(H, &h);
}

#define HDR_ITEMS                                                                                            \
    "(UID FLAGS RFC822.SIZE INTERNALDATE BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE MESSAGE-ID IN-REPLY-TO " \
    "REFERENCES CONTENT-TYPE)])"

/* Merges what a FETCH brought into the folder; returns the new unseen
 * messages, for the notice. */
static int merge(int ai, int fi, hdrs_t *H, bool notify_ok, cr_event_t *ev)
{
    int fresh = 0;
    cr_lock();
    if (CR.gen != s_gen || ai >= CR.na || fi >= CR.a[ai].nf) {
        cr_unlock();
        return 0;
    }
    cr_acct_t *a = &CR.a[ai];
    cr_folder_t *f = &a->f[fi];
    for (int i = 0; i < H->n; i++) {
        cr_hdr_t *h = &H->h[i];
        cr_hdr_t *old = cr_folder_hdr(f, h->uid);
        if (h->mid_h == 0xFFFFFFFFu) {
            /* flags only */
            if (old && (old->flags & 0x3F) != (h->flags & 0x3F)) {
                old->flags = (uint16_t)((old->flags & ~0x3F) | (h->flags & 0x3F));
                f->dirty = true;
            }
            continue;
        }
        if (!old && !(h->flags & CR_F_SEEN) && notify_ok) {
            fresh++;
            if (ev) {
                ev->kind = CR_EV_NEW_MAIL;
                ev->acct = ai;
                snprintf(ev->text, sizeof ev->text, "%s\n%s", h->from, h->subject);
            }
        }
        cr_folder_put(f, h);
        /* contacts: who writes, and to whom one writes */
        if (f->special != CR_SP_SENT && f->special != CR_SP_JUNK && h->from_addr[0]) {
            cr_contacts_add(h->from, h->from_addr, h->date);
        }
    }
    uint32_t un = 0;
    for (int i = 0; i < f->n; i++) {
        if (!(f->h[i].flags & CR_F_SEEN)) un++;
    }
    if (f->all || un > f->unseen) f->unseen = un;
    if (f->special == CR_SP_INBOX) a->unseen = f->unseen;
    cr_touch();
    cr_unlock();
    return fresh;
}

static void on_search(int ai, resp_t *r, void *ctx)
{
    (void)ai;
    if (!cr_istarts(r->t, "* SEARCH")) return;
    cr_buf_t *b = ctx;
    const char *p = r->t + 8;
    while (*p) {
        while (*p == ' ') p++;
        if (*p < '0' || *p > '9') break;
        uint32_t u = (uint32_t)strtoul(p, (char **)&p, 10);
        cr_buf_add(b, &u, sizeof u);
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void save_folder(int ai, int fi)
{
    cr_lock();
    if (CR.gen != s_gen) {
        cr_unlock();
        return;
    }
    cr_acct_t *a = &CR.a[ai];
    cr_folder_t *f = &a->f[fi];
    bool dirty = f->dirty;
    f->dirty = false;
    /* a copy to write without holding the lock */
    cr_folder_t copy = *f;
    copy.h = NULL;
    if (dirty && f->n) {
        copy.h = cr_alloc(sizeof(cr_hdr_t) * (size_t)f->n);
        if (copy.h) memcpy(copy.h, f->h, sizeof(cr_hdr_t) * (size_t)f->n);
    }
    cr_acct_t acopy = *a;
    cr_unlock();
    if (dirty) {
        cr_idx_save(&acopy, &copy);
        cr_contacts_save();
    }
    cr_free(copy.h);
}

/* The folder brought up to date with the server. */
static int sync_folder(int ai, int fi, bool notify)
{
    wk_t *w = &W[ai];
    int rc = select_folder(ai, fi, true);
    if (rc != 0) return rc;
    hdrs_t H = {0};
    H.ai = ai;
    H.fi = fi;
    uint32_t known_max = 0, known_min = 0;
    int known_n = 0;
    char modseq[24];
    bool reset = false;
    cr_lock();
    cr_acct_t *a = &CR.a[ai];
    cr_folder_t *f = &a->f[fi];
    if (!f->loaded) {
        cr_acct_t ac = *a;
        cr_unlock();
        cr_folder_t tmp = {0};
        cr_copy(tmp.raw, sizeof tmp.raw, ac.f[fi].raw);
        cr_idx_load(&ac, &tmp);
        cr_lock();
        f = &CR.a[ai].f[fi];
        if (!f->loaded) {
            cr_free(f->h);
            f->h = tmp.h;
            f->n = tmp.n;
            f->cap = tmp.cap;
            f->uidvalidity = tmp.uidvalidity;
            f->uidnext = tmp.uidnext;
            f->all = tmp.all;
            cr_copy(f->modseq, sizeof f->modseq, tmp.modseq);
            f->loaded = true;
        } else {
            cr_free(tmp.h);
        }
    }
    if (f->uidvalidity && w->s_uidvalidity && f->uidvalidity != w->s_uidvalidity) {
        reset = true;
        f->n = 0;
        f->modseq[0] = '\0';
        f->all = false;
        f->dirty = true;
    }
    if (f->n) {
        known_max = f->h[f->n - 1].uid;
        known_min = f->h[0].uid;
        known_n = f->n;
    }
    cr_copy(modseq, sizeof modseq, f->modseq);
    bool first_ever = !f->n && !f->uidvalidity;
    bool is_inbox = f->special == CR_SP_INBOX;
    cr_unlock();
    if (reset) {
        cr_lock();
        cr_acct_t ac = CR.a[ai];
        cr_folder_t fc = CR.a[ai].f[fi];
        cr_unlock();
        cr_cache_forget(&ac, &fc);
    }

    int fresh = 0;
    cr_event_t ev = {0};
    uint32_t exists = w->s_exists;
    if (!known_n) {
        if (exists) {
            uint32_t from = exists > CR_WINDOW ? exists - CR_WINDOW + 1 : 1;
            rc = cmd(ai, on_fetch_hdr, &H, "FETCH %u:%u " HDR_ITEMS, (unsigned)from, (unsigned)exists);
            if (rc < 0) goto out;
            cr_lock();
            CR.a[ai].f[fi].all = from == 1;
            cr_unlock();
        } else {
            cr_lock();
            CR.a[ai].f[fi].all = true;
            cr_unlock();
        }
        /* the first time ever, nothing is news; after a reset neither */
        fresh = merge(ai, fi, &H, false, NULL);
    } else {
        /* the new ones */
        if (!w->s_uidnext || w->s_uidnext > known_max + 1) {
            rc = cmd(ai, on_fetch_hdr, &H, "UID FETCH %u:* " HDR_ITEMS, (unsigned)(known_max + 1));
            if (rc < 0) goto out;
            /* "n:*" brings the last one even when it is below n */
            int k = 0;
            for (int i = 0; i < H.n; i++) {
                if (H.h[i].uid > known_max) H.h[k++] = H.h[i];
            }
            H.n = k;
            fresh = merge(ai, fi, &H, notify && is_inbox, &ev);
            H.n = 0;
        }
        /* the flags that changed */
        if (w->cap_condstore && modseq[0] && w->s_modseq[0] && !w->s_nomodseq) {
            if (strcmp(modseq, w->s_modseq)) {
                rc = cmd(ai, on_fetch_hdr, &H, "UID FETCH %u:* (UID FLAGS) (CHANGEDSINCE %s)", (unsigned)known_min,
                         modseq);
            }
        } else {
            rc = cmd(ai, on_fetch_hdr, &H, "UID FETCH %u:* (UID FLAGS)", (unsigned)known_min);
        }
        if (rc < 0) goto out;
        merge(ai, fi, &H, false, NULL);
        H.n = 0;
        /* the ones that went: only if the count says so */
        cr_lock();
        int have = CR.a[ai].f[fi].n;
        bool all = CR.a[ai].f[fi].all;
        cr_unlock();
        if (!all || (uint32_t)have != exists) {
            cr_buf_t b = {0};
            rc = cmd(ai, on_search, &b, "UID SEARCH UID %u:*", (unsigned)known_min);
            if (rc < 0) {
                cr_buf_free(&b);
                goto out;
            }
            uint32_t *u = (uint32_t *)b.p;
            size_t nu = b.n / sizeof(uint32_t);
            if (nu > 1) qsort(u, nu, sizeof *u, cmp_u32);
            cr_lock();
            cr_folder_t *ff = &CR.a[ai].f[fi];
            for (int i = ff->n - 1; i >= 0; i--) {
                uint32_t id = ff->h[i].uid;
                bool found = false;
                size_t lo = 0, hi = nu;
                while (lo < hi) {
                    size_t mid = (lo + hi) / 2;
                    if (u[mid] == id) {
                        found = true;
                        break;
                    }
                    if (u[mid] < id) lo = mid + 1;
                    else hi = mid;
                }
                if (!found) cr_folder_drop(ff, id);
            }
            cr_touch();
            cr_unlock();
            cr_buf_free(&b);
        }
    }
    cr_lock();
    f = &CR.a[ai].f[fi];
    f->uidvalidity = w->s_uidvalidity;
    if (w->s_uidnext) f->uidnext = w->s_uidnext;
    f->exists = exists;
    if (w->s_modseq[0]) cr_copy(f->modseq, sizeof f->modseq, w->s_modseq);
    f->synced = true;
    f->dirty = true;
    uint32_t un = 0;
    for (int i = 0; i < f->n; i++) {
        if (!(f->h[i].flags & CR_F_SEEN)) un++;
    }
    if (f->all) f->unseen = un;
    if (f->special == CR_SP_INBOX) CR.a[ai].unseen = f->unseen;
    cr_touch();
    cr_unlock();
    save_folder(ai, fi);
    if (fresh && !first_ever) {
        ev.n = fresh;
        ev.kind = CR_EV_NEW_MAIL;
        ev.acct = ai;
        cr_event_put(&ev);
    }
    rc = 0;
out:
    cr_free(H.h);
    w->dirty = false;
    return rc;
}

/* ---- jobs -------------------------------------------------------------------------------------- */

typedef struct {
    char    *body;
    size_t   n;
    uint32_t size;
    uint16_t flags;
    bool     got;
} body_t;

static void on_fetch_body(int ai, resp_t *r, void *ctx)
{
    (void)ai;
    body_t *B = ctx;
    const char *p = r->t + 2;
    while (*p >= '0' && *p <= '9') p++;
    if (!cr_istarts(p, " FETCH (")) return;
    tok_t t = {p + 8, r->t + r->n, r, };
    char key[200];
    while (t.p < t.e && *t.p != ')') {
        if (!tok_str(&t, key, sizeof key, NULL, NULL)) break;
        if (!strcasecmp(key, "RFC822.SIZE")) {
            char v[24];
            tok_str(&t, v, sizeof v, NULL, NULL);
            B->size = (uint32_t)strtoul(v, NULL, 10);
        } else if (!strcasecmp(key, "FLAGS")) {
            const char *l;
            size_t ln;
            if (tok_list(&t, &l, &ln)) B->flags = flags_of(l, ln);
        } else if (cr_istarts(key, "BODY[")) {
            const char *lit = NULL;
            size_t litn = 0;
            tok_str(&t, NULL, 0, &lit, &litn);
            if (lit && !B->got) {
                /* the literal is the response's: take it over */
                for (int i = 0; i < r->nlit; i++) {
                    if (r->lit[i] == lit) {
                        B->body = r->lit[i];
                        B->n = r->litn[i];
                        r->lit[i] = NULL;
                        B->got = true;
                    }
                }
            }
        } else {
            skip_sp(&t);
            const char *l;
            size_t ln;
            if (t.p < t.e && *t.p == '(') tok_list(&t, &l, &ln);
            else tok_str(&t, NULL, 0, NULL, NULL);
        }
    }
}

static bool ensure(int ai, cr_job_t *j);

static void job_open(int ai, cr_job_t *j)
{
    char path[300];
    cr_lock();
    cr_acct_t ac = CR.a[ai];
    cr_folder_t fc = CR.a[ai].f[j->folder];
    cr_hdr_t *hp = cr_folder_hdr(&CR.a[ai].f[j->folder], j->uid);
    uint16_t flags = hp ? hp->flags : 0;
    uint32_t size = hp ? hp->size : 0;
    cr_unlock();
    cr_cache_path(path, sizeof path, &ac, &fc, j->uid);
    size_t n = 0;
    char *raw = cr_file_read(path, &n, 0);
    bool partial = false;
    if (!raw) {
        char ppath[310];
        snprintf(ppath, sizeof ppath, "%s.part", path);
        /* a part file is only kept as long as the message is that big */
        raw = cr_file_read(ppath, &n, 0);
        partial = raw != NULL;
    }
    if (!raw) {
        if (!ensure(ai, j)) return;
        if (select_folder(ai, j->folder, false) != 0) {
            j->ok = 0;
            cr_copy(j->why, sizeof j->why, W[ai].last);
            return;
        }
        body_t B = {0};
        int rc = cmd(ai, on_fetch_body, &B, "UID FETCH %u (RFC822.SIZE FLAGS BODY.PEEK[]<0.%u>)", (unsigned)j->uid,
                     (unsigned)CR_BODY_MAX);
        if (rc != 0 || !B.got) {
            cr_free(B.body);
            j->ok = 0;
            cr_copy(j->why, sizeof j->why, rc == 0 ? "the message is no longer on the server" : W[ai].last);
            return;
        }
        raw = B.body;
        n = B.n;
        size = B.size;
        flags = B.flags;
        partial = B.size > n;
        char dir[300];
        cr_copy(dir, sizeof dir, path);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = '\0';
        cr_mkdirs(dir);
        if (partial) {
            char ppath[310];
            snprintf(ppath, sizeof ppath, "%s.part", path);
            cr_file_write(ppath, raw, n);
        } else {
            cr_file_write(path, raw, n);
        }
        cr_cache_add((uint32_t)n);
        uint32_t cap = (uint32_t)CR.cache_mb * 1024u * 1024u;
        if (cr_cache_used() > cap) cr_cache_trim(cap);
    }
    cr_msg_t *m = cr_mime_parse(raw, n, partial);
    if (!m) {
        j->ok = 0;
        cr_copy(j->why, sizeof j->why, "out of memory");
        return;
    }
    m->acct = ai;
    m->folder = j->folder;
    m->uid = j->uid;
    m->full_size = size;
    m->flags = flags;
    j->out = m;
    j->ok = 1;
    /* read now: on the server too (the open does not wait for it) */
    if (!(flags & CR_F_SEEN) && !(j->arg & 1)) {
        cr_lock();
        cr_hdr_t *h = cr_folder_hdr(&CR.a[ai].f[j->folder], j->uid);
        if (h) {
            h->flags |= CR_F_SEEN;
            cr_folder_t *f = &CR.a[ai].f[j->folder];
            if (f->unseen) f->unseen--;
            if (f->special == CR_SP_INBOX) CR.a[ai].unseen = f->unseen;
            f->dirty = true;
            cr_touch();
        }
        cr_unlock();
        m->flags |= CR_F_SEEN;
        cr_job_t *k = cr_job_new(CR_J_FLAGS);
        if (k) {
            k->acct = ai;
            k->folder = j->folder;
            k->uid = j->uid;
            k->arg = CR_F_SEEN;
            k->token = 0;
            cr_job_put(k);
        }
    }
}

static const char *flag_names(uint16_t f, char *out, size_t n)
{
    out[0] = '\0';
    size_t k = 0;
    static const struct {
        uint16_t f;
        const char *s;
    } T[] = {{CR_F_SEEN, "\\Seen"}, {CR_F_ANSWERED, "\\Answered"}, {CR_F_FLAGGED, "\\Flagged"},
             {CR_F_DELETED, "\\Deleted"}, {CR_F_DRAFT, "\\Draft"}, {CR_F_FORWARDED, "$Forwarded"}};
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (f & T[i].f) k += (size_t)snprintf(out + k, n - k, "%s%s", k ? " " : "", T[i].s);
        if (k >= n) break;
    }
    return out;
}

static void job_flags(int ai, cr_job_t *j)
{
    if (!ensure(ai, j)) return;
    if (select_folder(ai, j->folder, false) != 0) {
        j->ok = 0;
        cr_copy(j->why, sizeof j->why, W[ai].last);
        return;
    }
    char f[120];
    int rc = 0;
    if (j->arg) rc = cmd(ai, NULL, NULL, "UID STORE %u +FLAGS.SILENT (%s)", (unsigned)j->uid, flag_names((uint16_t)j->arg, f, sizeof f));
    if (rc == 0 && j->arg2)
        rc = cmd(ai, NULL, NULL, "UID STORE %u -FLAGS.SILENT (%s)", (unsigned)j->uid, flag_names((uint16_t)j->arg2, f, sizeof f));
    j->ok = rc == 0;
    if (rc) cr_copy(j->why, sizeof j->why, W[ai].last);
}

static int expunge_uid(int ai, uint32_t uid)
{
    wk_t *w = &W[ai];
    int rc = cmd(ai, NULL, NULL, "UID STORE %u +FLAGS.SILENT (\\Deleted)", (unsigned)uid);
    if (rc) return rc;
    return w->cap_uidplus ? cmd(ai, NULL, NULL, "UID EXPUNGE %u", (unsigned)uid) : cmd(ai, NULL, NULL, "EXPUNGE");
}

static int move_uid(int ai, int src, uint32_t uid, int dst)
{
    wk_t *w = &W[ai];
    if (select_folder(ai, src, false) != 0) return 1;
    char raw[260];
    cr_lock();
    quoted(raw, sizeof raw, CR.a[ai].f[dst].raw);
    cr_unlock();
    int rc;
    if (w->cap_move) {
        rc = cmd(ai, NULL, NULL, "UID MOVE %u %s", (unsigned)uid, raw);
    } else {
        rc = cmd(ai, NULL, NULL, "UID COPY %u %s", (unsigned)uid, raw);
        if (rc == 0) rc = expunge_uid(ai, uid);
    }
    if (rc == 0) {
        cr_lock();
        cr_folder_t *f = &CR.a[ai].f[src];
        cr_folder_drop(f, uid);
        CR.a[ai].f[dst].synced = false;
        cr_touch();
        cr_unlock();
        save_folder(ai, src);
    }
    return rc;
}

static void job_move(int ai, cr_job_t *j)
{
    if (!ensure(ai, j)) return;
    int rc = move_uid(ai, j->folder, j->uid, j->arg);
    j->ok = rc == 0;
    if (rc) cr_copy(j->why, sizeof j->why, W[ai].last);
}

static void job_delete(int ai, cr_job_t *j)
{
    if (!ensure(ai, j)) return;
    cr_lock();
    int trash = cr_folder_special(&CR.a[ai], CR_SP_TRASH);
    cr_unlock();
    int rc;
    if (trash >= 0 && trash != j->folder) {
        rc = move_uid(ai, j->folder, j->uid, trash);
    } else {
        rc = select_folder(ai, j->folder, false);
        if (rc == 0) rc = expunge_uid(ai, j->uid);
        if (rc == 0) {
            cr_lock();
            cr_folder_drop(&CR.a[ai].f[j->folder], j->uid);
            cr_touch();
            cr_unlock();
            save_folder(ai, j->folder);
        }
    }
    j->ok = rc == 0;
    if (rc) cr_copy(j->why, sizeof j->why, W[ai].last);
}

static void job_more(int ai, cr_job_t *j)
{
    if (!ensure(ai, j)) return;
    wk_t *w = &W[ai];
    if (select_folder(ai, j->folder, true) != 0) {
        j->ok = 0;
        cr_copy(j->why, sizeof j->why, w->last);
        return;
    }
    cr_lock();
    cr_folder_t *f = &CR.a[ai].f[j->folder];
    uint32_t low = f->n ? f->h[0].uid : 0xFFFFFFFFu;
    uint32_t have = (uint32_t)f->n;
    cr_unlock();
    uint32_t exists = w->s_exists;
    hdrs_t H = {0};
    H.ai = ai;
    H.fi = j->folder;
    int rc = 0;
    bool all = true;
    if (exists > have) {
        uint32_t top = exists - have;
        uint32_t from = top > CR_MORE ? top - CR_MORE + 1 : 1;
        all = from == 1;
        rc = cmd(ai, on_fetch_hdr, &H, "FETCH %u:%u " HDR_ITEMS, (unsigned)from, (unsigned)top);
        int k = 0;
        for (int i = 0; i < H.n; i++) {
            if (H.h[i].uid < low) H.h[k++] = H.h[i];
        }
        H.n = k;
    }
    if (rc == 0) {
        merge(ai, j->folder, &H, false, NULL);
        cr_lock();
        CR.a[ai].f[j->folder].all = all;
        cr_touch();
        cr_unlock();
        save_folder(ai, j->folder);
    }
    cr_free(H.h);
    j->ok = rc == 0;
    if (rc) cr_copy(j->why, sizeof j->why, w->last);
}

/* UID SEARCH [CHARSET UTF-8] TEXT <string>, the string as a literal when
 * it is not ASCII. 'charset' false for servers that refuse the argument. */
static int search_text(int ai, const char *q, bool charset, cr_buf_t *out)
{
    wk_t *w = &W[ai];
    snprintf(w->tag, sizeof w->tag, "A%d", ++w->tagn);
    char head[64];
    snprintf(head, sizeof head, "%s UID SEARCH %sTEXT ", w->tag, charset ? "CHARSET UTF-8 " : "");
    bool ok = cr_conn_puts(w->c, head) && send_astring(ai, q) && cr_conn_puts(w->c, "\r\n");
    return ok ? cmd_wait(ai, on_search, out, 60000) : -1;
}

static void job_search(int ai, cr_job_t *j)
{
    if (!ensure(ai, j)) return;
    wk_t *w = &W[ai];
    if (select_folder(ai, j->folder, false) != 0) {
        j->ok = 0;
        cr_copy(j->why, sizeof j->why, w->last);
        return;
    }
    cr_buf_t b = {0};
    int rc = search_text(ai, j->text, true, &b);
    if (rc == 1 || rc == 2) {
        b.n = 0;
        rc = search_text(ai, j->text, false, &b);
    }
    if (rc != 0) {
        cr_buf_free(&b);
        j->ok = 0;
        cr_copy(j->why, sizeof j->why, w->last);
        return;
    }
    uint32_t *u = (uint32_t *)b.p;
    int nu = (int)(b.n / sizeof(uint32_t));
    if (nu > 1) qsort(u, (size_t)nu, sizeof *u, cmp_u32);
    j->n = 0;
    if (nu) {
        /* the newest hundred */
        int from = nu > 100 ? nu - 100 : 0;
        cr_buf_t set = {0};
        cr_buf_str(&set, "UID FETCH ");
        for (int i = from; i < nu; i++) cr_buf_printf(&set, "%s%u", i > from ? "," : "", (unsigned)u[i]);
        cr_buf_str(&set, " " HDR_ITEMS);
        hdrs_t H = {0};
        H.ai = ai;
        H.fi = j->folder;
        if (!set.oom && cmd_send(ai, set.p) == 0) cmd_wait(ai, on_fetch_hdr, &H, 60000);
        cr_buf_free(&set);
        j->out = H.h;
        j->n = H.n;
    }
    cr_buf_free(&b);
    j->ok = 1;
}

/* APPEND a message to a folder; the new UID (0 unknown). */
typedef struct {
    uint32_t uid;
} appended_t;

static void on_append(int ai, resp_t *r, void *ctx)
{
    (void)ai;
    appended_t *A = ctx;
    char v[60];
    if (resp_code(r->t, "APPENDUID", v, sizeof v)) {
        const char *sp = strchr(v, ' ');
        if (sp) A->uid = (uint32_t)strtoul(sp + 1, NULL, 10);
    }
}

static int append_msg(int ai, int fi, const char *flags, const char *msg, size_t n, uint32_t *uid)
{
    wk_t *w = &W[ai];
    char raw[260];
    cr_lock();
    quoted(raw, sizeof raw, CR.a[ai].f[fi].raw);
    cr_unlock();
    snprintf(w->tag, sizeof w->tag, "A%d", ++w->tagn);
    char head[400];
    snprintf(head, sizeof head, "%s APPEND %s (%s) ", w->tag, raw, flags);
    if (!cr_conn_puts(w->c, head)) return -1;
    if (!send_literal(ai, msg, n)) return -1;
    if (!cr_conn_puts(w->c, "\r\n")) return -1;
    appended_t A = {0};
    int rc = cmd_wait(ai, on_append, &A, 60000);
    if (uid) *uid = A.uid;
    cr_lock();
    CR.a[ai].f[fi].synced = false;
    cr_unlock();
    return rc;
}

static void job_send(int ai, cr_job_t *j, bool draft)
{
    cr_out_t *o = j->out_msg;
    cr_acct_cfg_t cfg;
    cfg_copy(ai, &cfg);
    size_t n = 0;
    char msgid[200];
    char *msg = cr_mime_build(o, &cfg, &n, msgid, sizeof msgid);
    if (!msg) {
        j->ok = 0;
        cr_copy(j->why, sizeof j->why, "the message does not fit in memory");
        return;
    }
    char pass[128] = "";
    cr_pass_get(cfg.id, pass, sizeof pass);
    if (!draft) {
        /* the recipients: To, Cc and Bcc */
        cr_addr_t *ad = cr_alloc(sizeof(cr_addr_t) * 80);
        const char *rc[80];
        int nr = 0;
        if (ad) {
            nr += cr_addr_parse(o->to, ad + nr, 80 - nr);
            nr += cr_addr_parse(o->cc, ad + nr, 80 - nr);
            nr += cr_addr_parse(o->bcc, ad + nr, 80 - nr);
            for (int i = 0; i < nr; i++) rc[i] = ad[i].addr;
        }
        int e = nr ? cr_smtp_send(&cfg, pass, cfg.email, rc, nr, msg, n, j->why, sizeof j->why) : CR_E_PROTO;
        if (!nr) cr_copy(j->why, sizeof j->why, "no recipients");
        if (e == CR_E_NONE) {
            for (int i = 0; i < nr; i++) cr_contacts_add(ad[i].name, ad[i].addr, cr_now());
            cr_contacts_save();
        }
        cr_free(ad);
        j->err_kind = e;
        if (e != CR_E_NONE) {
            memset(pass, 0, sizeof pass);
            cr_free(msg);
            j->ok = 0;
            return;
        }
        j->ok = 1;
        cr_event_t ev = {CR_EV_SENT, ai, 0, ""};
        cr_event_put(&ev);
    }
    memset(pass, 0, sizeof pass);
    /* the server's side: Sent, the original's flag, the draft gone */
    if (ensure(ai, j)) {
        cr_lock();
        int sent = cr_folder_special(&CR.a[ai], CR_SP_SENT);
        int drafts = cr_folder_special(&CR.a[ai], CR_SP_DRAFTS);
        cr_unlock();
        if (draft) {
            if (drafts < 0) {
                j->ok = 0;
                cr_copy(j->why, sizeof j->why, "the account has no Drafts folder");
            } else {
                uint32_t uid = 0;
                int rc = append_msg(ai, drafts, "\\Draft \\Seen", msg, n, &uid);
                j->ok = rc == 0;
                j->n = (int)uid;
                if (rc) cr_copy(j->why, sizeof j->why, W[ai].last);
            }
        } else if (sent >= 0 && cfg.save_sent) {
            append_msg(ai, sent, "\\Seen", msg, n, NULL);
        }
        if (o->reply_uid && o->reply_folder >= 0 && !draft) {
            if (select_folder(ai, o->reply_folder, false) == 0) {
                cmd(ai, NULL, NULL, "UID STORE %u +FLAGS.SILENT (%s)", (unsigned)o->reply_uid,
                    o->forward ? "$Forwarded" : "\\Answered");
                cr_lock();
                cr_hdr_t *h = cr_folder_hdr(&CR.a[ai].f[o->reply_folder], o->reply_uid);
                if (h) h->flags |= o->forward ? CR_F_FORWARDED : CR_F_ANSWERED;
                cr_touch();
                cr_unlock();
            }
        }
        if (o->draft_uid && o->draft_folder >= 0 && (j->ok || !draft)) {
            if (select_folder(ai, o->draft_folder, false) == 0 && expunge_uid(ai, o->draft_uid) == 0) {
                cr_lock();
                cr_folder_drop(&CR.a[ai].f[o->draft_folder], o->draft_uid);
                cr_touch();
                cr_unlock();
            }
        }
    }
    cr_free(msg);
}

/* Streams a part straight to a file, decoded: an attachment bigger than
 * what the open message holds. */
static void job_part(int ai, cr_job_t *j)
{
    if (!ensure(ai, j)) return;
    wk_t *w = &W[ai];
    if (select_folder(ai, j->folder, false) != 0) {
        j->ok = 0;
        cr_copy(j->why, sizeof j->why, w->last);
        return;
    }
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.part", j->path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        j->ok = 0;
        cr_copy(j->why, sizeof j->why, "cannot write to the card");
        return;
    }
    char c[120];
    snprintf(c, sizeof c, "UID FETCH %u (BODY.PEEK[%.40s])", (unsigned)j->uid, j->text);
    if (cmd_send(ai, c) < 0) {
        fclose(f);
        unlink(tmp);
        j->ok = 0;
        cr_copy(j->why, sizeof j->why, cr_conn_why(w->c));
        return;
    }
    int rc = -1;
    uint32_t total = 0;
    uint8_t *chunk = cr_alloc(8192);
    uint8_t *dec = cr_alloc(8192);
    cr_b64s_t bs = {0};
    char qp_carry[4] = "";
    int qpc = 0;
    for (;;) {
        int k = cr_conn_line(w->c, s_line, LINE_MAX_B, CMD_MS);
        if (k < 0) break;
        size_t tl = strlen(w->tag);
        if (!strncmp(s_line, w->tag, tl) && s_line[tl] == ' ') {
            rc = cr_istarts(s_line + tl + 1, "OK") ? 0 : 1;
            cr_copy(w->last, sizeof w->last, s_line + tl + 1);
            break;
        }
        size_t n = (size_t)k;
        if (n >= 3 && s_line[n - 1] == '}' && chunk && dec) {
            char *o = strrchr(s_line, '{');
            long len = o ? strtol(o + 1, NULL, 10) : 0;
            if (len > CR_PART_MAX) {
                cr_copy(w->last, sizeof w->last, "the attachment is too big for the board");
                wk_close(ai);
                break;
            }
            while (len > 0) {
                int take = len > 8192 ? 8192 : (int)len;
                if (cr_conn_read(w->c, chunk, take, CMD_MS) != take) {
                    len = -1;
                    break;
                }
                len -= take;
                size_t dn;
                if (j->arg == CR_ENC_B64) {
                    dn = cr_b64_stream(&bs, dec, (const char *)chunk, (size_t)take);
                } else if (j->arg == CR_ENC_QP) {
                    /* an escape split between two pieces waits for the next */
                    char *buf = cr_alloc((size_t)take + 4);
                    memcpy(buf, qp_carry, (size_t)qpc);
                    memcpy(buf + qpc, chunk, (size_t)take);
                    size_t bn = (size_t)take + (size_t)qpc;
                    qpc = 0;
                    for (int q = 1; q <= 2 && bn >= (size_t)q; q++) {
                        if (buf[bn - (size_t)q] == '=') {
                            qpc = q;
                            memcpy(qp_carry, buf + bn - (size_t)q, (size_t)q);
                            bn -= (size_t)q;
                            break;
                        }
                    }
                    dn = cr_qp_dec((char *)dec, buf, bn, false);
                    cr_free(buf);
                } else {
                    memcpy(dec, chunk, (size_t)take);
                    dn = (size_t)take;
                }
                fwrite(dec, 1, dn, f);
                total += (uint32_t)dn;
            }
            if (len < 0) break;
        }
    }
    cr_free(chunk);
    cr_free(dec);
    fclose(f);
    if (rc == 0 && total) {
        unlink(j->path);
        rename(tmp, j->path);
        j->ok = 1;
        j->n = (int)total;
    } else {
        unlink(tmp);
        j->ok = 0;
        cr_copy(j->why, sizeof j->why, rc == 0 ? "the server sent nothing" : w->last);
    }
}

static void job_test(cr_job_t *j)
{
    /* a slot of its own, after the real accounts */
    int ai = CR_ACCT_MAX - 1;
    wk_t saved = W[ai];
    memset(&W[ai], 0, sizeof W[ai]);
    W[ai].sel = -1;
    int e = imap_login(ai, j->cfg, j->pass);
    if (e != CR_E_NONE) {
        j->ok = 0;
        j->err_kind = e;
        snprintf(j->why, sizeof j->why, "IMAP: %.180s", W[ai].last);
        wk_close(ai);
        W[ai] = saved;
        return;
    }
    wk_close(ai);
    W[ai] = saved;
    char why[200];
    e = cr_smtp_test(j->cfg, j->pass, why, sizeof why);
    if (e != CR_E_NONE) {
        j->ok = 0;
        j->err_kind = e;
        snprintf(j->why, sizeof j->why, "SMTP: %.180s", why);
        return;
    }
    j->ok = 1;
}

/* A picture of a message, decoded by the HAL: it reads files, so the
 * bytes go to the card first. */
static void job_image(cr_job_t *j)
{
    char path[200];
    snprintf(path, sizeof path, "%s/img-%lu.tmp", cr_dir(), (unsigned long)(aos_hal_uptime_ms() & 0xFFFFFF));
    cr_mkdirs(cr_dir());
    if (!j->in || !cr_file_write(path, j->in, j->in_n)) {
        j->ok = 0;
        return;
    }
    int w = 0, h = 0;
    uint16_t *px = aos_hal_image_decode(path, 0, 0, j->arg, j->arg2, false, &w, &h, NULL, NULL);
    unlink(path);
    j->out = px;
    j->n = w | (h << 16);
    j->ok = px != NULL;
}

static void job_empty(int ai, cr_job_t *j)
{
    if (!ensure(ai, j)) return;
    int rc = select_folder(ai, j->folder, true);
    if (rc == 0 && W[ai].s_exists) rc = cmd(ai, NULL, NULL, "STORE 1:* +FLAGS.SILENT (\\Deleted)");
    if (rc == 0) rc = cmd(ai, NULL, NULL, "EXPUNGE");
    if (rc == 0) {
        cr_lock();
        CR.a[ai].f[j->folder].n = 0;
        CR.a[ai].f[j->folder].unseen = 0;
        CR.a[ai].f[j->folder].dirty = true;
        cr_touch();
        cr_unlock();
        save_folder(ai, j->folder);
    }
    j->ok = rc == 0;
    if (rc) cr_copy(j->why, sizeof j->why, W[ai].last);
}

/* ---- the account's connection, kept up ---------------------------------------------------------- */

static const char *kind_word(int e)
{
    (void)e;
    return "";
}

static int connect_account(int ai)
{
    wk_t *w = &W[ai];
    cr_acct_cfg_t cfg;
    cfg_copy(ai, &cfg);
    char pass[128] = "";
    if (!cr_pass_get(cfg.id, pass, sizeof pass) || !pass[0]) {
        set_state(ai, CR_ST_NO_PASS, CR_E_NOPASS, "");
        w->retry_at = aos_hal_uptime_ms() + 3600000;
        return CR_E_NOPASS;
    }
    set_state(ai, CR_ST_CONNECTING, CR_E_NONE, "");
    int e = imap_login(ai, &cfg, pass);
    memset(pass, 0, sizeof pass);
    if (e != CR_E_NONE) {
        aos_hal_log(CR_TAG, "%s: %s", cfg.email, w->last);
        set_state(ai, CR_ST_ERROR, e, w->last);
        wk_close(ai);
        static const int BACK[] = {5, 15, 60, 300, 900};
        int b = BACK[w->fails < 4 ? w->fails : 4];
        if (e == CR_E_AUTH || e == CR_E_STARTTLS) b = 3600;    /* typing the password again is the fix */
        w->fails++;
        w->retry_at = aos_hal_uptime_ms() + (uint64_t)b * 1000u;
        return e;
    }
    w->fails = 0;
    (void)kind_word;
    aos_hal_log(CR_TAG, "%s: connected (idle %d move %d condstore %d)", cfg.email, w->cap_idle, w->cap_move,
                w->cap_condstore);
    if (do_list(ai) < 0) return CR_E_NET;
    cr_lock();
    int inbox = cr_folder_special(&CR.a[ai], CR_SP_INBOX);
    int sent = cr_folder_special(&CR.a[ai], CR_SP_SENT);
    cr_unlock();
    if (inbox >= 0 && sync_folder(ai, inbox, w->first_sync_done || w->inbox_had_cache) < 0) return CR_E_NET;
    w->first_sync_done = true;
    if (sent >= 0) sync_folder(ai, sent, false);
    do_status(ai, true);
    set_state(ai, CR_ST_ONLINE, CR_E_NONE, "");
    return CR_E_NONE;
}

/* The account online for a job, or the job answered with why not. */
static bool ensure(int ai, cr_job_t *j)
{
    wk_t *w = &W[ai];
    if (w->c && cr_conn_is_open(w->c)) {
        if (w->idling) {
            cr_conn_puts(w->c, "DONE\r\n");
            w->idling = false;
            if (cmd_wait(ai, NULL, NULL, 10000) < 0) wk_close(ai);
        }
        if (w->c && cr_conn_is_open(w->c)) return true;
    }
    int e = connect_account(ai);
    if (e == CR_E_NONE) return true;
    j->ok = 0;
    j->err_kind = e;
    cr_copy(j->why, sizeof j->why, w->last[0] ? w->last : "not connected");
    return false;
}

static void idle_start(int ai)
{
    wk_t *w = &W[ai];
    cr_lock();
    int inbox = CR.gen == s_gen ? cr_folder_special(&CR.a[ai], CR_SP_INBOX) : -1;
    cr_unlock();
    if (inbox < 0) return;
    if (select_folder(ai, inbox, false) != 0) return;
    if (cmd_send(ai, "IDLE") < 0) return;
    int k = cr_conn_line(w->c, s_line, LINE_MAX_B, 10000);
    if (k < 0 || s_line[0] != '+') {
        /* refused: the tag's answer is that line, or comes after */
        if (k >= 0 && s_line[0] == '*') cmd_wait(ai, NULL, NULL, 5000);
        w->cap_idle = false;
        return;
    }
    w->idling = true;
    w->idle_since = (uint32_t)(aos_hal_uptime_ms() / 1000);
    set_state(ai, CR_ST_IDLE, CR_E_NONE, "");
}

/* While in IDLE: anything the server says. */
static void idle_poll(int ai)
{
    wk_t *w = &W[ai];
    for (int guard = 0; guard < 50; guard++) {
        resp_t r;
        int k = read_resp(w, &r, guard ? 1 : POLL_MS);
        if (k == -2) break;
        if (k < 0) {
            wk_close(ai);
            set_state(ai, CR_ST_ERROR, CR_E_NET, "the connection broke");
            w->retry_at = aos_hal_uptime_ms() + 3000;
            return;
        }
        if (r.t[0] == '*') untagged_common(ai, &r);
        resp_free(&r);
        if (!cr_conn_pending(w->c)) break;
    }
    uint32_t now = (uint32_t)(aos_hal_uptime_ms() / 1000);
    if (w->dirty || now - w->idle_since > IDLE_RENEW_S) {
        cr_conn_puts(w->c, "DONE\r\n");
        w->idling = false;
        if (cmd_wait(ai, NULL, NULL, 10000) < 0) {
            wk_close(ai);
            return;
        }
        if (w->dirty) {
            cr_lock();
            int inbox = cr_folder_special(&CR.a[ai], CR_SP_INBOX);
            cr_unlock();
            if (inbox >= 0) sync_folder(ai, inbox, true);
        }
    }
}

static void run_job(int ai, cr_job_t *j)
{
    bool valid;
    cr_lock();
    valid = CR.gen == s_gen && ai < CR.na && (j->folder < 0 || j->folder < CR.a[ai].nf);
    cr_unlock();
    if (!valid) {
        j->ok = 0;
        cr_copy(j->why, sizeof j->why, "the account changed");
        return;
    }
    switch (j->kind) {
    case CR_J_OPEN: job_open(ai, j); break;
    case CR_J_FLAGS: job_flags(ai, j); break;
    case CR_J_MOVE: job_move(ai, j); break;
    case CR_J_DELETE: job_delete(ai, j); break;
    case CR_J_SYNC:
        if (ensure(ai, j)) {
            int rc = sync_folder(ai, j->folder, j->folder == cr_folder_special(&CR.a[ai], CR_SP_INBOX));
            j->ok = rc == 0;
            if (rc) cr_copy(j->why, sizeof j->why, W[ai].last);
            if (j->arg) do_status(ai, true);
        }
        break;
    case CR_J_MORE: job_more(ai, j); break;
    case CR_J_SEARCH: job_search(ai, j); break;
    case CR_J_SEND: job_send(ai, j, false); break;
    case CR_J_DRAFT: job_send(ai, j, true); break;
    case CR_J_PART: job_part(ai, j); break;
    case CR_J_EMPTY: job_empty(ai, j); break;
    case CR_J_RECONNECT:
        wk_close(ai);
        W[ai].retry_at = 0;
        W[ai].fails = 0;
        j->ok = connect_account(ai) == CR_E_NONE;
        if (!j->ok) cr_copy(j->why, sizeof j->why, W[ai].last);
        break;
    default: j->ok = 0; break;
    }
}

/* ---- the thread -------------------------------------------------------------------------------- */

static void load_cached(void)
{
    /* the folders and the INBOX's headers from the card, before any network */
    cr_lock();
    int na = CR.na;
    cr_unlock();
    for (int ai = 0; ai < na && !s_stop; ai++) {
        cr_lock();
        cr_acct_t ac = CR.a[ai];
        cr_unlock();
        cr_acct_t tmp = ac;
        tmp.f = cr_alloc(sizeof(cr_folder_t) * CR_FOLDER_MAX);
        if (!tmp.f) continue;
        tmp.nf = 0;
        cr_folders_load(&tmp);
        for (int i = 0; i < tmp.nf; i++) {
            if (tmp.f[i].special == CR_SP_INBOX || tmp.f[i].special == CR_SP_SENT) cr_idx_load(&tmp, &tmp.f[i]);
        }
        cr_lock();
        if (CR.gen == s_gen && ai < CR.na && CR.a[ai].nf == 0) {
            memcpy(CR.a[ai].f, tmp.f, sizeof(cr_folder_t) * (size_t)tmp.nf);
            CR.a[ai].nf = tmp.nf;
            int inbox = cr_folder_special(&CR.a[ai], CR_SP_INBOX);
            if (inbox >= 0) {
                CR.a[ai].unseen = CR.a[ai].f[inbox].unseen;
                W[ai].inbox_had_cache = CR.a[ai].f[inbox].n > 0;
            }
            cr_touch();
        } else {
            for (int i = 0; i < tmp.nf; i++) cr_free(tmp.f[i].h);
        }
        cr_unlock();
        cr_free(tmp.f);
    }
}

static void thread_main(void *arg)
{
    (void)arg;
    s_line = cr_alloc(LINE_MAX_B + 1);
    cr_contacts_load();
    cr_lock();
    s_gen = CR.gen;
    cr_unlock();
    for (int i = 0; i < CR_ACCT_MAX; i++) {
        memset(&W[i], 0, sizeof W[i]);
        W[i].sel = -1;
    }
    load_cached();
    {
        uint32_t cap = (uint32_t)CR.cache_mb * 1024u * 1024u;
        cr_cache_trim(cap);
    }
    while (!s_stop && s_line) {
        /* accounts changed: everything again */
        cr_lock();
        bool regen = CR.gen != s_gen;
        int na = CR.na;
        cr_unlock();
        if (regen || s_reload) {
            for (int i = 0; i < CR_ACCT_MAX; i++) wk_close(i);
            for (int i = 0; i < CR_ACCT_MAX; i++) {
                memset(&W[i], 0, sizeof W[i]);
                W[i].sel = -1;
            }
            cr_lock();
            s_gen = CR.gen;
            cr_unlock();
            s_reload = false;
            load_cached();
            continue;
        }
        /* jobs that need no account */
        cr_job_t *j = job_take(-1);
        if (j) {
            if (j->kind == CR_J_TEST) job_test(j);
            else if (j->kind == CR_J_IMAGE) job_image(j);
            job_finish(j);
            continue;
        }
        bool net = aos_hal_net_state() == AOS_NET_CONNECTED;
        bool paused = s_hidden && !CR.notify;
        int busy = 0;
        int kick = s_kick;
        if (kick >= 0) {
            s_kick = -1;
            if (kick < CR_ACCT_MAX) {
                W[kick].retry_at = 0;
                W[kick].fails = 0;
            }
        }
        for (int ai = 0; ai < na && !s_stop; ai++) {
            wk_t *w = &W[ai];
            j = job_take(ai);
            if (j) {
                busy++;
                if (!net) {
                    j->ok = 0;
                    j->err_kind = CR_E_NET;
                    cr_copy(j->why, sizeof j->why, "no network");
                    /* what the card has can still open */
                    if (j->kind == CR_J_OPEN) {
                        j->ok = 0;
                        job_open(ai, j);
                    }
                } else {
                    run_job(ai, j);
                }
                job_finish(j);
                continue;
            }
            if (!net || paused) {
                if (w->c) {
                    wk_close(ai);
                    set_state(ai, CR_ST_OFF, CR_E_NONE, "");
                }
                continue;
            }
            if (!w->c || !cr_conn_is_open(w->c)) {
                if (aos_hal_uptime_ms() >= w->retry_at) {
                    connect_account(ai);
                    busy++;
                }
                continue;
            }
            if (w->idling) {
                idle_poll(ai);
                continue;
            }
            /* online and not idling: what was left over, then IDLE */
            uint32_t now = cr_now() ? cr_now() : (uint32_t)(aos_hal_uptime_ms() / 1000);
            if (w->dirty) {
                cr_lock();
                int inbox = cr_folder_special(&CR.a[ai], CR_SP_INBOX);
                cr_unlock();
                if (inbox >= 0) sync_folder(ai, inbox, true);
                busy++;
                continue;
            }
            if (now - w->last_status > STATUS_S) {
                do_status(ai, false);
                busy++;
                continue;
            }
            if (w->cap_idle) {
                if (!job_waiting(ai)) idle_start(ai);
            } else if (now - w->last_poll > NOIDLE_S) {
                /* no IDLE: look now and then */
                w->last_poll = now;
                cr_lock();
                int inbox = cr_folder_special(&CR.a[ai], CR_SP_INBOX);
                cr_unlock();
                if (inbox >= 0) sync_folder(ai, inbox, true);
                set_state(ai, CR_ST_ONLINE, CR_E_NONE, "");
            }
        }
        if (!busy) aos_hal_sleep_ms(!net || paused || !na ? 300 : 10);
    }
    for (int i = 0; i < CR_ACCT_MAX; i++) wk_close(i);
    /* jobs nobody will run: answered, so the screens free them */
    cr_job_t *j;
    qlock();
    j = s_jobs;
    s_jobs = NULL;
    qunlock();
    while (j) {
        cr_job_t *n = j->next;
        j->ok = 0;
        cr_copy(j->why, sizeof j->why, "closing");
        job_finish(j);
        j = n;
    }
    cr_free(s_line);
    s_line = NULL;
    s_running = false;
}

void cr_sync_start(void)
{
    if (s_running) return;
    if (!s_mx) s_mx = aos_hal_mutex_create();
    if (!s_qmx) s_qmx = aos_hal_mutex_create();
    s_stop = false;
    cr_net_abort = false;
    s_running = true;
    if (!aos_hal_thread_start("correo", thread_main, NULL, 24 * 1024, 3)) {
        s_running = false;
        aos_hal_log(CR_TAG, "cannot start the network thread");
    }
}

void cr_sync_stop(void)
{
    s_stop = true;
    cr_net_abort = true;
    for (int i = 0; i < 600 && s_running; i++) aos_hal_sleep_ms(20);
    if (s_running) aos_hal_log(CR_TAG, "the network thread did not stop in time");
    /* what was finished and never taken */
    cr_job_t *j;
    while ((j = cr_job_done())) cr_job_free(j);
    s_ev_n = 0;
}

bool cr_sync_running(void)
{
    return s_running;
}

void cr_sync_kick(int acct)
{
    s_kick = acct;
}

void cr_sync_reload(void)
{
    s_reload = true;
}

void cr_sync_bg(bool hidden)
{
    s_hidden = hidden;
    (void)s_ver_dirty;
}
