/*
 * P4OS - The board's Telegram bot (aos_bot.h says what it is for).
 *
 * Two threads, each with its own HTTP/1.1 connection kept open to the Bot
 * API (TLS through the HAL's aos_tls_*):
 *   - "bot_in" asks getUpdates with a long poll (25 s): Telegram answers as
 *     soon as someone writes, so a command takes a round trip and not a
 *     polling period. Each update is handled there: who wrote, which
 *     command, and what to answer goes to the outbox.
 *   - "bot_out" empties the outbox (messages, photos, answers to buttons)
 *     and runs the rules once a second.
 * A single connection would make every alert wait for the long poll to end.
 *
 * Everything shared lives in B (PSRAM), under one mutex; the threads hold it
 * only to copy in and out, never across the network.
 *
 * Over the wire, nothing of the board but what a linked chat asks for: the
 * token is in the URL of every request (that is the Bot API), always over
 * https:// unless the pref "bot_api" points at a test server.
 */
#include "aos_bot.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"
#include "aos_zigbee.h"
#include "aos_notif_internal.h"

#include "cJSON.h"

#include <ctype.h>
#include <stdarg.h>
#include <strings.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the HAL's connection, declared here without its header (it pulls in
 * mbedtls's), as the apps that speak a protocol of their own do */
void *aos_tls_new(void);
void  aos_tls_free(void *c);
int   aos_tls_open(void *c, const char *host, int port, unsigned flags, int timeout_ms, char *why, size_t why_n);
bool  aos_tls_send(void *c, const void *buf, int len, int timeout_ms);
int   aos_tls_recv(void *c, void *buf, int max, int timeout_ms);
void  aos_tls_close(void *c);
#define TLS_ENCRYPT 0x01u
#define TLS_NODELAY 0x02u
#define TLS_TIMEOUT (-2)

#define TAG "bot"
#define API_DEFAULT  "https://api.telegram.org"
#define POLL_S       25
#define RESP_MAX     (256 * 1024)
#define OUT_MAX      16
#define LOG_MAX      40
#define CODE_S       600
#define MARKUP_MAX   2048
#define TEXT_MAX     1500
#define NOTIF_APP    "Bot"

#if defined(AOS_SIM)
static void *big_calloc(size_t n) { return calloc(1, n); }
#else
#include "esp_heap_caps.h"
static void *big_calloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}
#endif

/* ------------------------------------------------------------------------ */
/* The state                                                                 */
/* ------------------------------------------------------------------------ */

enum { J_TEXT = 0, J_SHOT, J_ANSWER, J_COMMANDS };
typedef struct {
    uint8_t kind;
    int64_t chat;                   /* 0: every linked chat */
    char    text[TEXT_MAX];         /* the message, a photo's caption, an answer's toast */
    char    markup[MARKUP_MAX];     /* reply_markup JSON, "" none */
    char    cb_id[64];              /* J_ANSWER: the callback query */
} job_t;

typedef struct {
    float    last;
    bool     known;
    uint32_t fired_ms;
    uint32_t event_ms;
    int32_t  time_key;
} rule_state_t;

typedef struct {
    void    *tls;
    bool     open;
} conn_t;

static struct {
    void *mx;
    uint32_t version;
    /* settings */
    bool enabled;
    char token[96];
    uint32_t token_gen;
    uint32_t cmds;
    aos_bot_chat_t chats[AOS_BOT_CHATS_MAX];
    int nchats;
    aos_bot_rule_t rules[AOS_BOT_RULES_MAX];
    int nrules;
    uint32_t rules_gen;
    /* what is going on */
    aos_bot_state_t state;
    char username[40];
    char why[100];
    uint32_t received, sent;
    uint32_t code, code_until;
    bool commands_set;
    bool boot_pending;
    /* the outbox (s_out, made when the bot first starts) */
    int out_r, out_n;
    /* the log */
    char log[LOG_MAX][96];
    int log_w, log_n;
    /* the chats told how to link, not to tell them again for a while */
    int64_t told[8];
    uint32_t told_ms[8];
} *B;

/* made when the bot is first switched on, not before: ~150 KB of PSRAM
 * (the outbox, two copies of the Zigbee table, two stacks) that a board
 * without a bot does not pay */
static job_t *s_out;                            /* OUT_MAX */
static aos_zigbee_dev_t *s_zb_in, *s_zb_out;   /* AOS_ZB_DEVICES_MAX each, PSRAM */
static bool s_started;
static rule_state_t s_rs[AOS_BOT_RULES_MAX];
static uint32_t s_rs_gen = UINT32_MAX;
static uint32_t s_notif_uid;
static bool s_notif_seen;
static uint32_t s_notif_uid_next = 0xB0700000u;

static void lock(void) { aos_hal_mutex_lock(B->mx); }
static void unlock(void) { aos_hal_mutex_unlock(B->mx); }
static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

static void scpy(char *d, size_t n, const char *s)
{
    if (!n) return;
    size_t i = 0;
    for (; s && s[i] && i + 1 < n; i++) d[i] = s[i];
    d[i] = 0;
}

static bool ci_contains(const char *hay, const char *needle)
{
    if (!needle || !needle[0]) return true;
    size_t ln = strlen(needle);
    for (const char *p = hay; p && *p; p++) {
        size_t k = 0;
        while (k < ln && p[k] && tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == ln) return true;
    }
    return false;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    aos_hal_log(TAG, "%s", line);
    if (!B) return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    lock();
    char *d = B->log[B->log_w % LOG_MAX];
    if (tm.tm_year > 120) snprintf(d, 96, "%02d:%02d %.80s", tm.tm_hour % 24, tm.tm_min % 60, line);
    else scpy(d, 96, line);
    B->log_w++;
    if (B->log_n < LOG_MAX) B->log_n++;
    B->version++;
    unlock();
}

static void set_state(aos_bot_state_t st, const char *why)
{
    lock();
    if (B->state != st || (why && strcmp(B->why, why))) B->version++;
    B->state = st;
    if (why) scpy(B->why, sizeof B->why, why);
    unlock();
}

/* ------------------------------------------------------------------------ */
/* Saving                                                                    */
/* ------------------------------------------------------------------------ */

static void chats_save(void)        /* under the lock */
{
    char buf[AOS_BOT_CHATS_MAX * 72] = "";
    size_t o = 0;
    for (int i = 0; i < B->nchats; i++) {
        char name[48];
        scpy(name, sizeof name, B->chats[i].name);
        for (char *p = name; *p; p++) if (*p == ';' || *p == '=') *p = ' ';
        o += snprintf(buf + o, sizeof buf - o, "%lld=%s;", (long long)B->chats[i].id, name);
        if (o >= sizeof buf) break;
    }
    aos_hal_pref_set_str("bot_chats", buf);
}

static void chats_load(void)
{
    char buf[AOS_BOT_CHATS_MAX * 72] = "";
    aos_hal_pref_get_str("bot_chats", buf, sizeof buf);
    B->nchats = 0;
    for (char *p = buf; *p && B->nchats < AOS_BOT_CHATS_MAX;) {
        char *semi = strchr(p, ';');
        if (semi) *semi = 0;
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = 0;
            B->chats[B->nchats].id = strtoll(p, NULL, 10);
            scpy(B->chats[B->nchats].name, sizeof B->chats[0].name, eq + 1);
            if (B->chats[B->nchats].id) B->nchats++;
        }
        if (!semi) break;
        p = semi + 1;
    }
}

/* A rule as one line: fields split by the unit separator, the text last
 * (it may hold anything but that). */
static void rule_line(const aos_bot_rule_t *r, char *out, size_t n)
{
    char ie[17], ae[17];
    for (int i = 0; i < 8; i++) {
        snprintf(ie + 2 * i, 3, "%02x", r->ieee[i]);
        snprintf(ae + 2 * i, 3, "%02x", r->act_ieee[i]);
    }
    snprintf(out, n, "%d\x1f%d\x1f%s\x1f%d\x1f%d\x1f%g\x1f%d\x1f%d\x1f%s\x1f%d\x1f%s\x1f%s\x1f%d\x1f%s",
             r->on, r->trig, ie, r->var, r->op, (double)r->value, r->at_min, r->days, r->filter,
             r->act, ae, r->ir, r->gap_s, r->text);
}

static void hex8(const char *s, uint8_t out[8])
{
    memset(out, 0, 8);
    for (int i = 0; i < 8 && s[2 * i] && s[2 * i + 1]; i++) {
        char h[3] = { s[2 * i], s[2 * i + 1], 0 };
        out[i] = (uint8_t)strtoul(h, NULL, 16);
    }
}

static bool rule_parse(char *line, aos_bot_rule_t *r)
{
    char *f[14];
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < 14; p++)
        if (*p == '\x1f') { *p = 0; f[n++] = p + 1; }
    if (n < 14) return false;
    memset(r, 0, sizeof *r);
    r->on = atoi(f[0]) != 0;
    r->trig = (uint8_t)atoi(f[1]);
    hex8(f[2], r->ieee);
    r->var = (uint8_t)atoi(f[3]);
    r->op = (uint8_t)atoi(f[4]);
    r->value = strtof(f[5], NULL);
    r->at_min = (uint16_t)atoi(f[6]);
    r->days = (uint8_t)atoi(f[7]);
    scpy(r->filter, sizeof r->filter, f[8]);
    r->act = (uint8_t)atoi(f[9]);
    hex8(f[10], r->act_ieee);
    scpy(r->ir, sizeof r->ir, f[11]);
    r->gap_s = (uint16_t)atoi(f[12]);
    scpy(r->text, sizeof r->text, f[13]);
    return r->trig < AOS_BOT_T_N && r->act < AOS_BOT_A_N && r->var < AOS_BOT_V_N && r->op < AOS_BOT_OP_N;
}

static void rules_save(void)        /* under the lock */
{
    char key[24], line[400];
    for (int i = 0; i < B->nrules; i++) {
        snprintf(key, sizeof key, "bot_r%d", i);
        rule_line(&B->rules[i], line, sizeof line);
        aos_hal_pref_set_str(key, line);
    }
    for (int i = B->nrules; i < AOS_BOT_RULES_MAX; i++) {
        snprintf(key, sizeof key, "bot_r%d", i);
        aos_hal_pref_erase(key);
    }
    aos_hal_pref_set_i32("bot_rn", B->nrules);
}

static void rules_load(void)
{
    int32_t n = 0;
    aos_hal_pref_get_i32("bot_rn", &n);
    B->nrules = 0;
    char key[24], line[400];
    for (int i = 0; i < n && i < AOS_BOT_RULES_MAX; i++) {
        snprintf(key, sizeof key, "bot_r%d", i);
        line[0] = 0;
        if (aos_hal_pref_get_str(key, line, sizeof line) && rule_parse(line, &B->rules[B->nrules])) B->nrules++;
    }
}

/* ------------------------------------------------------------------------ */
/* HTTP over the HAL's connection                                            */
/* ------------------------------------------------------------------------ */

typedef struct { bool https; char host[80]; int port; } api_t;

static void api_get(api_t *a)
{
    char url[128] = "";
    aos_hal_pref_get_str("bot_api", url, sizeof url);
    if (!url[0]) scpy(url, sizeof url, API_DEFAULT);
    const char *p = url;
    a->https = strncmp(p, "http://", 7) != 0;
    if (!strncmp(p, "https://", 8)) p += 8;
    else if (!strncmp(p, "http://", 7)) p += 7;
    size_t k = 0;
    while (p[k] && p[k] != ':' && p[k] != '/' && k + 1 < sizeof a->host) { a->host[k] = p[k]; k++; }
    a->host[k] = 0;
    a->port = p[k] == ':' ? atoi(p + k + 1) : (a->https ? 443 : 80);
}

static void conn_close(conn_t *c)
{
    if (c->tls) aos_tls_close(c->tls);
    c->open = false;
}

static bool conn_open(conn_t *c, const api_t *a, char *why, size_t wn)
{
    if (c->open) return true;
    if (!c->tls) c->tls = aos_tls_new();
    if (!c->tls) { scpy(why, wn, _("No hay memoria")); return false; }
    int rc = aos_tls_open(c->tls, a->host, a->port, (a->https ? TLS_ENCRYPT : 0) | TLS_NODELAY, 10000, why, wn);
    if (rc != 0) {
        if (!why[0]) snprintf(why, wn, _("No pude conectar con %.60s"), a->host);
        return false;
    }
    c->open = true;
    return true;
}

typedef struct {
    int status;                 /* HTTP's, or < 0: the connection failed */
    char *body;                 /* NUL-terminated, PSRAM, the caller frees */
    int len;
} resp_t;

static int find_hdr_end(const char *b, int n)
{
    for (int i = 3; i < n; i++)
        if (b[i - 3] == '\r' && b[i - 2] == '\n' && b[i - 1] == '\r' && b[i] == '\n') return i + 1;
    return -1;
}

static const char *hdr_val(const char *h, int hlen, const char *name)
{
    size_t ln = strlen(name);
    for (int i = 0; i + (int)ln < hlen; i++) {
        if ((i == 0 || h[i - 1] == '\n') && !strncasecmp(h + i, name, ln) && h[i + ln] == ':') {
            const char *v = h + i + ln + 1;
            while (*v == ' ') v++;
            return v;
        }
    }
    return NULL;
}

/* One request on the kept connection; it opens it when it has to and closes
 * it on any failure, so the next one starts clean. */
/* The long poll's way out: getUpdates waits up to 25 s, and a new token or
 * the switch going off should not wait that long. */
static volatile uint32_t s_in_gen;
static bool in_cancelled(void)
{
    lock();
    bool c = !B->enabled || B->token_gen != s_in_gen;
    unlock();
    return c;
}

static resp_t http_call(conn_t *c, const api_t *a, const char *token, const char *method,
                        const char *ctype, const void *body, int blen, int timeout_ms, char *why, size_t wn,
                        bool (*cancel)(void))
{
    resp_t r = { .status = -1 };
    why[0] = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        bool reused = c->open;
        if (!conn_open(c, a, why, wn)) return r;
        char head[512];
        int hn = snprintf(head, sizeof head,
                          "POST /bot%s/%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: P4OS\r\nAccept: application/json\r\n"
                          "Connection: keep-alive\r\nContent-Type: %s\r\nContent-Length: %d\r\n\r\n",
                          token, method, a->host, ctype ? ctype : "application/json", blen);
        if (hn <= 0 || hn >= (int)sizeof head) { scpy(why, wn, "request too long"); return r; }
        bool ok = aos_tls_send(c->tls, head, hn, 10000) && (blen == 0 || aos_tls_send(c->tls, body, blen, 20000));
        if (!ok) {
            conn_close(c);
            if (reused) continue;   /* the server closed the idle one: once more, fresh */
            scpy(why, wn, _("Se cortó la conexión"));
            return r;
        }
        /* the head, then the body as it says */
        int cap = 4096, n = 0, hend = -1;
        char *buf = big_calloc((size_t)cap + 1);
        if (!buf) { conn_close(c); scpy(why, wn, _("No hay memoria")); return r; }
        uint32_t t0 = now_ms();
        while (hend < 0) {
            if (n >= cap) break;
            int k = aos_tls_recv(c->tls, buf + n, cap - n, 1000);
            if (k == TLS_TIMEOUT) {
                if (now_ms() - t0 > (uint32_t)timeout_ms || (cancel && cancel())) break;
                continue;
            }
            if (k <= 0) break;
            n += k;
            hend = find_hdr_end(buf, n);
        }
        if (hend < 0) {
            free(buf);
            conn_close(c);
            if (cancel && cancel()) return r;
            if (reused && n == 0) continue;
            scpy(why, wn, _("Telegram no contestó"));
            return r;
        }
        r.status = atoi(buf + 9);
        const char *cl = hdr_val(buf, hend, "Content-Length");
        const char *te = hdr_val(buf, hend, "Transfer-Encoding");
        const char *cn = hdr_val(buf, hend, "Connection");
        bool chunked = te && !strncasecmp(te, "chunked", 7);
        bool close_after = cn && !strncasecmp(cn, "close", 5);
        int want = cl ? atoi(cl) : -1;
        if (want > RESP_MAX) want = RESP_MAX;
        /* the body, moved to the front */
        int have = n - hend;
        memmove(buf, buf + hend, (size_t)have);
        int total = chunked ? RESP_MAX : (want >= 0 ? want : RESP_MAX);
        if (total + 1 > cap) {
            char *nb = big_calloc((size_t)(chunked || want < 0 ? 64 * 1024 : total) + 1);
            if (!nb) { free(buf); conn_close(c); r.status = -1; scpy(why, wn, _("No hay memoria")); return r; }
            memcpy(nb, buf, (size_t)have);
            free(buf);
            buf = nb;
            cap = chunked || want < 0 ? 64 * 1024 : total;
        }
        bool done = !chunked && want >= 0 && have >= want;
        while (!done) {
            if (have >= cap) {
                if (cap >= RESP_MAX) break;
                int nc = cap * 2 > RESP_MAX ? RESP_MAX : cap * 2;
                char *nb = big_calloc((size_t)nc + 1);
                if (!nb) break;
                memcpy(nb, buf, (size_t)have);
                free(buf);
                buf = nb;
                cap = nc;
            }
            int k = aos_tls_recv(c->tls, buf + have, cap - have, 1000);
            if (k == TLS_TIMEOUT) {
                if (now_ms() - t0 > (uint32_t)timeout_ms + 5000) break;
                continue;
            }
            if (k <= 0) { close_after = true; done = want < 0 && !chunked; break; }
            have += k;
            if (!chunked && want >= 0 && have >= want) done = true;
            if (chunked && have >= 5 && !memcmp(buf + have - 5, "0\r\n\r\n", 5)) done = true;
        }
        if (!done) {
            free(buf);
            conn_close(c);
            r.status = -1;
            scpy(why, wn, _("La respuesta llegó cortada"));
            return r;
        }
        if (chunked) {
            /* undone in place: size line, data, CRLF... */
            int rd = 0, wr = 0;
            while (rd < have) {
                long sz = strtol(buf + rd, NULL, 16);
                char *nl = memchr(buf + rd, '\n', (size_t)(have - rd));
                if (!nl || sz <= 0) break;
                rd = (int)(nl - buf) + 1;
                if (rd + sz > have) sz = have - rd;
                memmove(buf + wr, buf + rd, (size_t)sz);
                wr += (int)sz;
                rd += (int)sz + 2;
            }
            have = wr;
        }
        buf[have] = 0;
        r.body = buf;
        r.len = have;
        if (close_after) conn_close(c);
        return r;
    }
    return r;
}

/* A JSON method; the answer's "result", parsed (the caller deletes the
 * root it gets back). status: HTTP's or -1. */
static cJSON *api_json_x(conn_t *c, const api_t *a, const char *token, const char *method, cJSON *args,
                         int timeout_ms, int *status, char *why, size_t wn, bool (*cancel)(void))
{
    char *js = args ? cJSON_PrintUnformatted(args) : NULL;
    resp_t r = http_call(c, a, token, method, "application/json", js ? js : "{}", js ? (int)strlen(js) : 2,
                         timeout_ms, why, wn, cancel);
    if (js) cJSON_free(js);
    if (status) *status = r.status;
    if (!r.body) return NULL;
    cJSON *root = cJSON_Parse(r.body);
    free(r.body);
    if (!root) { scpy(why, wn, _("Telegram contestó algo que no entiendo")); return NULL; }
    cJSON *okj = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsTrue(okj)) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "description");
        if (cJSON_IsString(d)) scpy(why, wn, d->valuestring);
    }
    return root;
}

static cJSON *api_json(conn_t *c, const api_t *a, const char *token, const char *method, cJSON *args,
                       int timeout_ms, int *status, char *why, size_t wn)
{
    return api_json_x(c, a, token, method, args, timeout_ms, status, why, wn, NULL);
}

/* ------------------------------------------------------------------------ */
/* The outbox                                                                */
/* ------------------------------------------------------------------------ */

static bool out_push(const job_t *j)
{
    if (!B) return false;
    lock();
    bool ok = s_out && B->out_n < OUT_MAX;
    if (ok) {
        s_out[(B->out_r + B->out_n) % OUT_MAX] = *j;
        B->out_n++;
    }
    unlock();
    if (!ok) logf_("%s", _("La cola de salida está llena: se perdió un mensaje"));
    return ok;
}

static job_t s_j;                   /* a job being built (bot_in) */
static job_t s_jo;                  /* the one being sent (bot_out) */

static void say(int64_t chat, const char *text, const char *markup)
{
    memset(&s_j, 0, sizeof s_j);
    s_j.kind = J_TEXT;
    s_j.chat = chat;
    scpy(s_j.text, sizeof s_j.text, text);
    scpy(s_j.markup, sizeof s_j.markup, markup ? markup : "");
    out_push(&s_j);
}

/* ------------------------------------------------------------------------ */
/* What the board knows, in words                                            */
/* ------------------------------------------------------------------------ */

static void ieee_hex(const uint8_t ieee[8], char out[17])
{
    for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", ieee[7 - i]);
}

static void dev_name(const aos_zigbee_dev_t *d, char *out, size_t n)
{
    if (d->name[0]) scpy(out, n, d->name);
    else if (d->model[0]) scpy(out, n, d->model);
    else {
        char h[17];
        ieee_hex(d->ieee, h);
        snprintf(out, n, "0x%s", h + 10);
    }
}

static int zb_snapshot(aos_zigbee_dev_t *buf)
{
    if (!buf) return 0;
    return aos_zigbee_devices(buf, AOS_ZB_DEVICES_MAX);
}

/* the endpoint of its first output, bit its index in eps_list; 0 none */
static uint8_t first_out(const aos_zigbee_dev_t *d, int *bit)
{
    for (int i = 0; i < d->neps && i < 4; i++)
        if (d->onoff_eps & (1u << i)) { if (bit) *bit = i; return d->eps_list[i]; }
    return 0;
}

static bool var_of(const aos_zigbee_dev_t *d, int var, float *v)
{
    int bit = 0;
    switch (var) {
    case AOS_BOT_V_ON:
        if (!first_out(d, &bit) || !(d->onoff_known & (1u << bit))) return false;
        *v = (d->onoff >> bit) & 1;
        return true;
    case AOS_BOT_V_ALARM:
        if (!d->zone_known) return false;
        *v = d->zone_status & 1;
        return true;
    case AOS_BOT_V_OCC:
        if (!(d->clusters & AOS_ZB_CL_OCC) || d->occupancy == 0xFF) return false;
        *v = d->occupancy & 1;
        return true;
    case AOS_BOT_V_TEMP:
        if (d->temp == INT16_MIN) return false;
        *v = d->temp / 100.0f;
        return true;
    case AOS_BOT_V_HUM:
        if (d->hum == 0xFFFF) return false;
        *v = d->hum / 100.0f;
        return true;
    case AOS_BOT_V_BAT:
        if (d->battery == 0xFF) return false;
        *v = d->battery / 2.0f;
        return true;
    case AOS_BOT_V_LEVEL:
        if (!d->level_ep || d->level == 0xFF) return false;
        *v = roundf(d->level * 100.0f / 254.0f);
        return true;
    }
    return false;
}

static void var_text(const aos_zigbee_dev_t *d, int var, float v, char *out, size_t n)
{
    switch (var) {
    case AOS_BOT_V_ON: scpy(out, n, v != 0 ? _("prendido") : _("apagado")); return;
    case AOS_BOT_V_ALARM:
        if (d && d->zone_type == 0x0015) scpy(out, n, v != 0 ? _("abierta") : _("cerrada"));
        else if (d && d->zone_type == 0x000d) scpy(out, n, v != 0 ? _("hay movimiento") : _("sin movimiento"));
        else if (d && d->zone_type == 0x002a) scpy(out, n, v != 0 ? _("hay agua") : _("seco"));
        else scpy(out, n, v != 0 ? _("alarma") : _("normal"));
        return;
    case AOS_BOT_V_OCC: scpy(out, n, v != 0 ? _("hay alguien") : _("no hay nadie")); return;
    case AOS_BOT_V_TEMP: snprintf(out, n, "%.1f °C", (double)v); return;
    case AOS_BOT_V_HUM: snprintf(out, n, "%.0f %%", (double)v); return;
    case AOS_BOT_V_BAT: snprintf(out, n, "%.0f %%", (double)v); return;
    case AOS_BOT_V_LEVEL: snprintf(out, n, "%.0f %%", (double)v); return;
    }
    scpy(out, n, d ? d->event : "");
}

static void dev_line(const aos_zigbee_dev_t *d, char *out, size_t n)
{
    char name[40], v[32];
    dev_name(d, name, sizeof name);
    size_t o = (size_t)snprintf(out, n, "• %s", name);
    static const int order[] = { AOS_BOT_V_ON, AOS_BOT_V_ALARM, AOS_BOT_V_OCC, AOS_BOT_V_TEMP,
                                 AOS_BOT_V_HUM, AOS_BOT_V_LEVEL, AOS_BOT_V_BAT };
    bool first = true;
    for (size_t i = 0; i < sizeof order / sizeof order[0] && o < n; i++) {
        float f;
        if (!var_of(d, order[i], &f)) continue;
        var_text(d, order[i], f, v, sizeof v);
        o += (size_t)snprintf(out + o, n - o, "%s%s%s", first ? ": " : ", ",
                              order[i] == AOS_BOT_V_BAT ? _("batería ") : "", v);
        first = false;
    }
    if (d->heard_ms == 0 && o < n) snprintf(out + o, n - o, " %s", _("(sin noticias)"));
}

static void time_text(char *out, size_t n)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year > 120) snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
    else scpy(out, n, "--:--");
}

static void state_text(char *out, size_t n)
{
    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    uint32_t up = (uint32_t)(aos_hal_uptime_ms() / 1000);
    const char *app = aos_ui_current_app();
    const aos_app_t *fa = app ? aos_ui_app_find(app) : NULL;
    if (fa && fa->desc.name) app = fa->desc.name;
    char hora[8];
    time_text(hora, sizeof hora);
    size_t o = (size_t)snprintf(out, n, "📟 %s · P4OS %s\n", aos_hal_device_name(), aos_hal_firmware_version());
    o += (size_t)snprintf(out + o, n - o, _("Hora: %s · encendida hace %ud %02uh %02um\n"), hora,
                          (unsigned)(up / 86400), (unsigned)(up / 3600 % 24), (unsigned)(up / 60 % 60));
    if (aos_hal_net_state() == AOS_NET_CONNECTED)
        o += (size_t)snprintf(out + o, n - o, _("Red: %s, %d dBm\n"), aos_hal_net_wired() ? _("cable") : "Wi-Fi",
                              aos_hal_net_rssi());
    o += (size_t)snprintf(out + o, n - o, _("Memoria libre: %u KB interna, %u KB PSRAM\n"),
                          (unsigned)(fi / 1024), (unsigned)(fp / 1024));
    o += (size_t)snprintf(out + o, n - o, _("En pantalla: %s\n"), app ? app : _("el inicio"));
    int nd = zb_snapshot(s_zb_in);
    if (nd > 0 && o < n) o += (size_t)snprintf(out + o, n - o, _("Zigbee: %d dispositivos\n"), nd);
    aos_battery_t bt;
    if (aos_hal_battery_read(&bt) && o < n) snprintf(out + o, n - o, _("Batería: %d %%\n"), bt.percent);
}

/* ------------------------------------------------------------------------ */
/* Rules in words                                                            */
/* ------------------------------------------------------------------------ */

static const char *var_word(int v)
{
    switch (v) {
    case AOS_BOT_V_ON: return _("prendido");
    case AOS_BOT_V_ALARM: return _("alarma (abierta, movimiento, agua)");
    case AOS_BOT_V_OCC: return _("presencia");
    case AOS_BOT_V_TEMP: return _("temperatura");
    case AOS_BOT_V_HUM: return _("humedad");
    case AOS_BOT_V_BAT: return _("batería");
    case AOS_BOT_V_LEVEL: return _("nivel");
    default: return _("un evento (botón, control)");
    }
}

static void zb_name_of(const uint8_t ieee[8], char *out, size_t n, aos_zigbee_dev_t *buf)
{
    int nd = zb_snapshot(buf);
    for (int i = 0; i < nd; i++)
        if (!memcmp(buf[i].ieee, ieee, 8)) { dev_name(&buf[i], out, n); return; }
    char h[17];
    ieee_hex(ieee, h);
    snprintf(out, n, "0x%s", h + 10);
}

void aos_bot_rule_text(const aos_bot_rule_t *r, char *out, size_t n)
{
    char when[120] = "", what[90] = "", dn[40] = "";
    /* the app's thread: a snapshot of its own (small, on the heap) */
    aos_zigbee_dev_t *buf = big_calloc(sizeof(aos_zigbee_dev_t) * AOS_ZB_DEVICES_MAX);
    switch (r->trig) {
    case AOS_BOT_T_ZIGBEE:
        zb_name_of(r->ieee, dn, sizeof dn, buf);
        if (r->var == AOS_BOT_V_EVENT || r->op == AOS_BOT_OP_CHANGE)
            snprintf(when, sizeof when, _("Cuando cambia %s de %s"), var_word(r->var), dn);
        else if (r->op == AOS_BOT_OP_ABOVE)
            snprintf(when, sizeof when, _("Cuando %s de %s pasa de %g"), var_word(r->var), dn, (double)r->value);
        else if (r->op == AOS_BOT_OP_BELOW)
            snprintf(when, sizeof when, _("Cuando %s de %s baja de %g"), var_word(r->var), dn, (double)r->value);
        else if (r->var == AOS_BOT_V_ON || r->var == AOS_BOT_V_ALARM || r->var == AOS_BOT_V_OCC)
            snprintf(when, sizeof when, _("Cuando %s de %s es %s"), var_word(r->var), dn,
                     r->value != 0 ? _("sí") : _("no"));
        else
            snprintf(when, sizeof when, _("Cuando %s de %s es %g"), var_word(r->var), dn, (double)r->value);
        break;
    case AOS_BOT_T_TIME: {
        static const char *const D[] = { N_("do"), N_("lu"), N_("ma"), N_("mi"), N_("ju"), N_("vi"), N_("sá") };
        char days[40] = "";
        if ((r->days & 0x7F) == 0x7F) scpy(days, sizeof days, _("todos los días"));
        else for (int i = 0; i < 7; i++)
            if (r->days & (1u << i)) {
                size_t l = strlen(days);
                snprintf(days + l, sizeof days - l, "%s%s", l ? " " : "", aos_tr(D[i]));
            }
        snprintf(when, sizeof when, _("A las %02d:%02d, %s"), r->at_min / 60, r->at_min % 60, days);
        break;
    }
    case AOS_BOT_T_BOOT: scpy(when, sizeof when, _("Cuando arranca la placa")); break;
    default:
        if (r->filter[0]) snprintf(when, sizeof when, _("Cuando llega un aviso de \"%s\""), r->filter);
        else scpy(when, sizeof when, _("Cuando llega un aviso"));
    }
    switch (r->act) {
    case AOS_BOT_A_MESSAGE: scpy(what, sizeof what, _("mandar un mensaje")); break;
    case AOS_BOT_A_SHOT: scpy(what, sizeof what, _("mandar una captura")); break;
    case AOS_BOT_A_ZB_ON: case AOS_BOT_A_ZB_OFF: case AOS_BOT_A_ZB_TOGGLE:
        zb_name_of(r->act_ieee, dn, sizeof dn, buf);
        snprintf(what, sizeof what, r->act == AOS_BOT_A_ZB_ON ? _("prender %s") :
                                    r->act == AOS_BOT_A_ZB_OFF ? _("apagar %s") : _("cambiar %s"), dn);
        break;
    default: snprintf(what, sizeof what, _("mandar el código IR \"%s\""), r->ir);
    }
    free(buf);
    snprintf(out, n, "%s: %s", when, what);
}

/* ------------------------------------------------------------------------ */
/* Markup                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct { cJSON *root, *rows, *row; } kb_t;

static void kb_begin(kb_t *k)
{
    k->root = cJSON_CreateObject();
    k->rows = cJSON_AddArrayToObject(k->root, "inline_keyboard");
    k->row = NULL;
}

static void kb_row(kb_t *k) { k->row = cJSON_CreateArray(); cJSON_AddItemToArray(k->rows, k->row); }

static void kb_btn(kb_t *k, const char *text, const char *data)
{
    if (!k->row) kb_row(k);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "text", text);
    cJSON_AddStringToObject(b, "callback_data", data);
    cJSON_AddItemToArray(k->row, b);
}

static void kb_end(kb_t *k, char *out, size_t n)
{
    char *js = cJSON_PrintUnformatted(k->root);
    scpy(out, n, js && strlen(js) < n ? js : "");
    if (js) cJSON_free(js);
    cJSON_Delete(k->root);
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                  */
/* ------------------------------------------------------------------------ */

static bool linked(int64_t chat)
{
    lock();
    bool ok = false;
    for (int i = 0; i < B->nchats && !ok; i++) ok = B->chats[i].id == chat;
    unlock();
    return ok;
}

static bool cmd_on(uint32_t bit)
{
    lock();
    bool on = (B->cmds & bit) != 0;
    unlock();
    return on;
}

static char s_txt[TEXT_MAX];
static char s_mk[MARKUP_MAX];

static void help(int64_t chat)
{
    size_t o = (size_t)snprintf(s_txt, sizeof s_txt, _("Soy %s. Esto sé hacer:\n"), aos_hal_device_name());
    struct { uint32_t bit; const char *line; } L[] = {
        { AOS_BOT_CMD_STATE, N_("/estado: cómo está la placa") },
        { AOS_BOT_CMD_SHOT, N_("/captura: lo que muestra la pantalla") },
        { AOS_BOT_CMD_ZIGBEE, N_("/zigbee: los dispositivos, con botones") },
        { AOS_BOT_CMD_ZIGBEE, N_("/prender <nombre>, /apagar <nombre>") },
        { AOS_BOT_CMD_IR, N_("/ir: los códigos IR guardados") },
        { AOS_BOT_CMD_NOTICE, N_("/aviso <texto>: un aviso en la pantalla") },
        { AOS_BOT_CMD_RULES, N_("/reglas: las reglas, para prenderlas y apagarlas") },
    };
    for (size_t i = 0; i < sizeof L / sizeof L[0] && o < sizeof s_txt; i++)
        if (cmd_on(L[i].bit)) o += (size_t)snprintf(s_txt + o, sizeof s_txt - o, "%s\n", aos_tr(L[i].line));
    say(chat, s_txt, NULL);
}

static void zigbee_list(int64_t chat)
{
    int nd = zb_snapshot(s_zb_in);
    if (nd <= 0) { say(chat, _("No hay dispositivos Zigbee (o la red Zigbee no está andando)."), NULL); return; }
    size_t o = (size_t)snprintf(s_txt, sizeof s_txt, "%s\n", _("Dispositivos Zigbee:"));
    kb_t k;
    kb_begin(&k);
    int nb = 0;
    for (int i = 0; i < nd && o < sizeof s_txt - 100; i++) {
        char line[160];
        dev_line(&s_zb_in[i], line, sizeof line);
        o += (size_t)snprintf(s_txt + o, sizeof s_txt - o, "%s\n", line);
        uint8_t ep = first_out(&s_zb_in[i], NULL);
        if (ep && nb < 12) {
            char name[40], lab[64], data[40], h[17];
            dev_name(&s_zb_in[i], name, sizeof name);
            ieee_hex(s_zb_in[i].ieee, h);
            kb_row(&k);
            snprintf(lab, sizeof lab, "💡 %s", name);
            snprintf(data, sizeof data, "z1:%s:%u", h, ep);
            kb_btn(&k, lab, data);
            snprintf(data, sizeof data, "z0:%s:%u", h, ep);
            kb_btn(&k, _("Apagar"), data);
            nb++;
        }
    }
    kb_end(&k, s_mk, sizeof s_mk);
    say(chat, s_txt, nb ? s_mk : NULL);
}

static const aos_zigbee_dev_t *zb_find(const char *name, int nd)
{
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nd; i++) {
            char dn[40];
            dev_name(&s_zb_in[i], dn, sizeof dn);
            if (!first_out(&s_zb_in[i], NULL)) continue;
            if (pass == 0 ? !strcasecmp(dn, name) : ci_contains(dn, name)) return &s_zb_in[i];
        }
    return NULL;
}

static void zigbee_switch(int64_t chat, const char *arg, int cmd)
{
    while (*arg == ' ') arg++;
    if (!*arg) { say(chat, cmd ? _("¿Qué prendo? /prender <nombre>") : _("¿Qué apago? /apagar <nombre>"), NULL); return; }
    int nd = zb_snapshot(s_zb_in);
    const aos_zigbee_dev_t *d = zb_find(arg, nd);
    if (!d) { snprintf(s_txt, sizeof s_txt, _("No encuentro \"%s\" entre los que se prenden y apagan."), arg); say(chat, s_txt, NULL); return; }
    char dn[40];
    dev_name(d, dn, sizeof dn);
    bool ok = aos_zigbee_onoff(d->ieee, first_out(d, NULL), cmd);
    snprintf(s_txt, sizeof s_txt, ok ? (cmd ? _("Prendo %s.") : _("Apago %s.")) : _("No pude mandarle la orden a %s."), dn);
    logf_("%s", s_txt);
    say(chat, s_txt, NULL);
}

static aos_zigbee_ir_code_t s_ir[24];

static void ir_list(int64_t chat, const char *arg)
{
    while (*arg == ' ') arg++;
    int n = aos_zigbee_ir_codes(s_ir, 24);
    if (*arg) {
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < n; i++)
                if (pass == 0 ? !strcasecmp(s_ir[i].name, arg) : ci_contains(s_ir[i].name, arg)) {
                    bool ok = aos_zigbee_ir_send_saved(i);
                    snprintf(s_txt, sizeof s_txt, ok ? _("Mando \"%s\".") : _("No pude mandar \"%s\"."), s_ir[i].name);
                    logf_("%s", s_txt);
                    say(chat, s_txt, NULL);
                    return;
                }
        snprintf(s_txt, sizeof s_txt, _("No hay un código IR que se llame \"%s\"."), arg);
        say(chat, s_txt, NULL);
        return;
    }
    if (n <= 0) { say(chat, _("No hay códigos IR guardados. Se aprenden en la app Zigbee, con un emisor IR."), NULL); return; }
    kb_t k;
    kb_begin(&k);
    for (int i = 0; i < n && i < 24; i++) {
        if (i % 3 == 0) kb_row(&k);
        char data[16];
        snprintf(data, sizeof data, "ir:%d", i);
        kb_btn(&k, s_ir[i].name, data);
    }
    kb_end(&k, s_mk, sizeof s_mk);
    say(chat, _("Los códigos IR guardados:"), s_mk);
}

static void rules_list(int64_t chat)
{
    lock();
    int n = B->nrules;
    static aos_bot_rule_t r[AOS_BOT_RULES_MAX];
    memcpy(r, B->rules, sizeof r[0] * (size_t)n);
    unlock();
    if (!n) { say(chat, _("No hay reglas. Se arman en la app Bot de la placa o en su página del portal."), NULL); return; }
    size_t o = (size_t)snprintf(s_txt, sizeof s_txt, "%s\n", _("Las reglas:"));
    kb_t k;
    kb_begin(&k);
    for (int i = 0; i < n && o < sizeof s_txt - 10; i++) {
        char line[300];
        aos_bot_rule_text(&r[i], line, sizeof line);
        o += (size_t)snprintf(s_txt + o, sizeof s_txt - o, "%d. %s %s\n", i + 1, r[i].on ? "✅" : "⏸", line);
        char lab[32], data[16];
        snprintf(lab, sizeof lab, r[i].on ? _("Pausar %d") : _("Activar %d"), i + 1);
        snprintf(data, sizeof data, "rl:%d", i);
        if (i % 4 == 0) kb_row(&k);
        kb_btn(&k, lab, data);
    }
    kb_end(&k, s_mk, sizeof s_mk);
    say(chat, s_txt, s_mk);
}

static void notice(int64_t chat, const char *from, const char *arg)
{
    while (*arg == ' ') arg++;
    if (!*arg) { say(chat, _("¿Qué aviso? /aviso <texto>"), NULL); return; }
    aos_notif_t n = { 0 };
    n.uid = s_notif_uid_next++;
    n.category = AOS_NOTIF_SOCIAL;
    scpy(n.app, sizeof n.app, NOTIF_APP);
    scpy(n.title, sizeof n.title, from);
    scpy(n.message, sizeof n.message, arg);
    n.when = time(NULL);
    n.important = true;
    aos_notif_push(&n);
    say(chat, _("Listo, está en la pantalla."), NULL);
}

static void link_chat(int64_t chat, const char *who, const char *arg)
{
    while (*arg == ' ') arg++;
    uint32_t code = (uint32_t)strtoul(arg, NULL, 10);
    lock();
    bool ok = B->code && code == B->code && (int32_t)(B->code_until - now_ms()) > 0;
    bool full = B->nchats >= AOS_BOT_CHATS_MAX;
    if (ok && !full) {
        bool dup = false;
        for (int i = 0; i < B->nchats; i++) dup |= B->chats[i].id == chat;
        if (!dup) {
            B->chats[B->nchats].id = chat;
            scpy(B->chats[B->nchats].name, sizeof B->chats[0].name, who);
            B->nchats++;
            chats_save();
        }
        B->code = 0;
        B->version++;
    }
    unlock();
    if (ok && full) { say(chat, _("Ya hay ocho chats vinculados: olvidá alguno en la app Bot."), NULL); return; }
    if (!ok) { say(chat, _("Ese código no sirve. Abrí la app Bot en la placa, tocá Vincular un chat y mandame el número que muestra."), NULL); return; }
    logf_(_("Chat vinculado: %s"), who);
    snprintf(s_txt, sizeof s_txt, _("Vinculado. Hola, %s."), who);
    say(chat, s_txt, NULL);
    help(chat);
}

static void tell_how_to_link(int64_t chat)
{
    uint32_t now = now_ms();
    lock();
    int slot = 0;
    bool told = false;
    for (int i = 0; i < 8; i++) {
        if (B->told[i] == chat && now - B->told_ms[i] < 600000) told = true;
        if (B->told_ms[i] < B->told_ms[slot]) slot = i;
    }
    if (!told) { B->told[slot] = chat; B->told_ms[slot] = now ? now : 1; }
    unlock();
    if (!told)
        say(chat, _("Este chat no está vinculado con la placa. En la placa: app Bot, Vincular un chat; después mandame /vincular y el número que muestra."), NULL);
}

static void handle_command(int64_t chat, const char *who, const char *text)
{
    char cmd[24];
    size_t k = 0;
    const char *p = text + 1;
    while (*p && *p != ' ' && *p != '@' && k + 1 < sizeof cmd) cmd[k++] = (char)tolower((unsigned char)*p++);
    cmd[k] = 0;
    while (*p && *p != ' ') p++;        /* "@NameBot" */
    const char *arg = p;
    if (!strcmp(cmd, "vincular") || !strcmp(cmd, "link") || (!strcmp(cmd, "start") && *arg)) {
        if (linked(chat)) { say(chat, _("Este chat ya está vinculado."), NULL); return; }
        link_chat(chat, who, arg);
        return;
    }
    if (!linked(chat)) { tell_how_to_link(chat); return; }
    logf_(_("%s pidió /%s"), who, cmd);
    if (!strcmp(cmd, "start") || !strcmp(cmd, "ayuda") || !strcmp(cmd, "help")) help(chat);
    else if ((!strcmp(cmd, "estado") || !strcmp(cmd, "status")) && cmd_on(AOS_BOT_CMD_STATE)) {
        state_text(s_txt, sizeof s_txt);
        say(chat, s_txt, NULL);
    } else if ((!strcmp(cmd, "captura") || !strcmp(cmd, "screenshot")) && cmd_on(AOS_BOT_CMD_SHOT)) {
        memset(&s_j, 0, sizeof s_j);
        s_j.kind = J_SHOT;
        s_j.chat = chat;
        out_push(&s_j);
    } else if (!strcmp(cmd, "zigbee") && cmd_on(AOS_BOT_CMD_ZIGBEE)) zigbee_list(chat);
    else if ((!strcmp(cmd, "prender") || !strcmp(cmd, "on")) && cmd_on(AOS_BOT_CMD_ZIGBEE)) zigbee_switch(chat, arg, 1);
    else if ((!strcmp(cmd, "apagar") || !strcmp(cmd, "off")) && cmd_on(AOS_BOT_CMD_ZIGBEE)) zigbee_switch(chat, arg, 0);
    else if (!strcmp(cmd, "ir") && cmd_on(AOS_BOT_CMD_IR)) ir_list(chat, arg);
    else if ((!strcmp(cmd, "aviso") || !strcmp(cmd, "notice")) && cmd_on(AOS_BOT_CMD_NOTICE)) notice(chat, who, arg);
    else if ((!strcmp(cmd, "reglas") || !strcmp(cmd, "rules")) && cmd_on(AOS_BOT_CMD_RULES)) rules_list(chat);
    else say(chat, _("No conozco ese comando (o está apagado en la app Bot). /ayuda dice cuáles hay."), NULL);
}

static void answer(const char *cb_id, const char *text)
{
    memset(&s_j, 0, sizeof s_j);
    s_j.kind = J_ANSWER;
    scpy(s_j.cb_id, sizeof s_j.cb_id, cb_id);
    scpy(s_j.text, sizeof s_j.text, text);
    out_push(&s_j);
}

static void handle_callback(int64_t chat, const char *who, const char *id, const char *data)
{
    if (!linked(chat)) { answer(id, _("Este chat no está vinculado")); return; }
    char t[120];
    if ((data[0] == 'z') && (data[1] == '0' || data[1] == '1') && data[2] == ':' && cmd_on(AOS_BOT_CMD_ZIGBEE)) {
        uint8_t ieee[8];
        char h[17];
        scpy(h, sizeof h, data + 3);
        for (int i = 0; i < 8; i++) {
            char b[3] = { h[2 * i], h[2 * i + 1], 0 };
            ieee[7 - i] = (uint8_t)strtoul(b, NULL, 16);
        }
        const char *c = strchr(data + 3, ':');
        int ep = c ? atoi(c + 1) : 1;
        bool ok = aos_zigbee_onoff(ieee, (uint8_t)ep, data[1] - '0');
        snprintf(t, sizeof t, "%s", ok ? (data[1] == '1' ? _("Prendido") : _("Apagado")) : _("No pude mandar la orden"));
        logf_(_("%s tocó %s"), who, data[1] == '1' ? _("prender") : _("apagar"));
        answer(id, t);
    } else if (!strncmp(data, "ir:", 3) && cmd_on(AOS_BOT_CMD_IR)) {
        int i = atoi(data + 3);
        int n = aos_zigbee_ir_codes(s_ir, 24);
        bool ok = i >= 0 && i < n && aos_zigbee_ir_send_saved(i);
        snprintf(t, sizeof t, ok ? _("Mando \"%s\"") : _("No pude mandarlo"), i >= 0 && i < n ? s_ir[i].name : "");
        logf_("%s", t);
        answer(id, t);
    } else if (!strncmp(data, "rl:", 3) && cmd_on(AOS_BOT_CMD_RULES)) {
        int i = atoi(data + 3);
        lock();
        bool ok = i >= 0 && i < B->nrules;
        bool on = false;
        if (ok) {
            B->rules[i].on = !B->rules[i].on;
            on = B->rules[i].on;
            rules_save();
            B->rules_gen++;
            B->version++;
        }
        unlock();
        snprintf(t, sizeof t, ok ? (on ? _("Regla %d activada") : _("Regla %d en pausa")) : _("Esa regla ya no está"), i + 1);
        answer(id, t);
    } else {
        answer(id, _("Eso ya no está"));
    }
}

static void handle_update(const cJSON *u)
{
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(u, "message");
    const cJSON *cb = cJSON_GetObjectItemCaseSensitive(u, "callback_query");
    const cJSON *from = NULL, *chat = NULL;
    if (msg) {
        from = cJSON_GetObjectItemCaseSensitive(msg, "from");
        chat = cJSON_GetObjectItemCaseSensitive(msg, "chat");
    } else if (cb) {
        from = cJSON_GetObjectItemCaseSensitive(cb, "from");
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(cb, "message");
        chat = m ? cJSON_GetObjectItemCaseSensitive(m, "chat") : NULL;
    }
    const cJSON *cid = chat ? cJSON_GetObjectItemCaseSensitive(chat, "id") : NULL;
    if (!cJSON_IsNumber(cid)) return;
    int64_t id = (int64_t)cid->valuedouble;
    char who[48] = "?";
    const cJSON *fn = from ? cJSON_GetObjectItemCaseSensitive(from, "first_name") : NULL;
    const cJSON *un = from ? cJSON_GetObjectItemCaseSensitive(from, "username") : NULL;
    if (cJSON_IsString(fn)) scpy(who, sizeof who, fn->valuestring);
    else if (cJSON_IsString(un)) scpy(who, sizeof who, un->valuestring);
    lock();
    B->received++;
    B->version++;
    unlock();
    if (msg) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(msg, "text");
        if (!cJSON_IsString(t)) return;
        if (t->valuestring[0] == '/') handle_command(id, who, t->valuestring);
        else if (!linked(id)) tell_how_to_link(id);
        else say(id, _("Hablo con comandos: /ayuda dice cuáles."), NULL);
    } else {
        const cJSON *cbid = cJSON_GetObjectItemCaseSensitive(cb, "id");
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(cb, "data");
        if (cJSON_IsString(cbid) && cJSON_IsString(data)) handle_callback(id, who, cbid->valuestring, data->valuestring);
    }
}

/* ------------------------------------------------------------------------ */
/* bot_in: getUpdates                                                        */
/* ------------------------------------------------------------------------ */

static bool settings_copy(char *token, size_t tn, uint32_t *gen)
{
    lock();
    bool on = B->enabled;
    scpy(token, tn, B->token);
    *gen = B->token_gen;
    unlock();
    return on;
}

static void in_thread(void *arg)
{
    (void)arg;
    static conn_t c;
    static char token[96], why[100];
    uint32_t gen = 0, refused_gen = UINT32_MAX, online_gen = UINT32_MAX;
    int backoff = 0;
    char offs[24] = "";
    aos_hal_pref_get_str("bot_offset", offs, sizeof offs);
    long long offset = strtoll(offs, NULL, 10);
    for (;;) {
        bool on = settings_copy(token, sizeof token, &gen);
        if (!on) { conn_close(&c); set_state(AOS_BOT_OFF, ""); online_gen = UINT32_MAX; aos_hal_sleep_ms(500); continue; }
        if (!token[0]) { conn_close(&c); set_state(AOS_BOT_NO_TOKEN, ""); aos_hal_sleep_ms(500); continue; }
        if (gen == refused_gen) { aos_hal_sleep_ms(500); continue; }
        if (aos_hal_net_state() != AOS_NET_CONNECTED) {
            conn_close(&c);
            set_state(AOS_BOT_WAITING_NET, "");
            aos_hal_sleep_ms(1000);
            continue;
        }
        api_t a;
        api_get(&a);
        int st = 0;
        if (online_gen != gen) {
            set_state(AOS_BOT_CONNECTING, NULL);
            cJSON *me = api_json(&c, &a, token, "getMe", NULL, 15000, &st, why, sizeof why);
            const cJSON *res = me ? cJSON_GetObjectItemCaseSensitive(me, "result") : NULL;
            const cJSON *un = res ? cJSON_GetObjectItemCaseSensitive(res, "username") : NULL;
            if (st == 401 || st == 404) {
                cJSON_Delete(me);
                refused_gen = gen;
                set_state(AOS_BOT_BAD_TOKEN, _("Telegram no reconoce el token"));
                logf_("%s", _("Telegram no reconoce el token"));
                continue;
            }
            if (st != 200 || !cJSON_IsString(un)) {
                cJSON_Delete(me);
                set_state(AOS_BOT_ERROR, why[0] ? why : _("No pude hablar con Telegram"));
                backoff = backoff ? (backoff * 2 > 60000 ? 60000 : backoff * 2) : 2000;
                aos_hal_sleep_ms((uint32_t)backoff);
                continue;
            }
            lock();
            scpy(B->username, sizeof B->username, un->valuestring);
            B->commands_set = false;
            B->version++;
            unlock();
            logf_(_("En línea como @%s"), un->valuestring);
            cJSON_Delete(me);
            online_gen = gen;
            set_state(AOS_BOT_ONLINE, "");
        }
        cJSON *args = cJSON_CreateObject();
        cJSON_AddNumberToObject(args, "offset", (double)offset);
        cJSON_AddNumberToObject(args, "timeout", POLL_S);
        cJSON *al = cJSON_AddArrayToObject(args, "allowed_updates");
        cJSON_AddItemToArray(al, cJSON_CreateString("message"));
        cJSON_AddItemToArray(al, cJSON_CreateString("callback_query"));
        s_in_gen = gen;
        cJSON *root = api_json_x(&c, &a, token, "getUpdates", args, (POLL_S + 10) * 1000, &st, why, sizeof why,
                                 in_cancelled);
        cJSON_Delete(args);
        if (!root && in_cancelled()) { conn_close(&c); continue; }
        if (st == 401) { cJSON_Delete(root); refused_gen = gen; online_gen = UINT32_MAX; set_state(AOS_BOT_BAD_TOKEN, _("Telegram no reconoce el token")); continue; }
        if (st == 409) {
            cJSON_Delete(root);
            set_state(AOS_BOT_ERROR, _("Otro programa usa este bot (un webhook u otro getUpdates)"));
            aos_hal_sleep_ms(10000);
            continue;
        }
        if (st != 200 || !root) {
            cJSON_Delete(root);
            set_state(AOS_BOT_ERROR, why[0] ? why : _("No pude hablar con Telegram"));
            online_gen = UINT32_MAX;
            backoff = backoff ? (backoff * 2 > 60000 ? 60000 : backoff * 2) : 2000;
            aos_hal_sleep_ms((uint32_t)backoff);
            continue;
        }
        backoff = 0;
        set_state(AOS_BOT_ONLINE, "");
        const cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "result"), *u;
        long long before = offset;
        cJSON_ArrayForEach(u, res) {
            const cJSON *uid = cJSON_GetObjectItemCaseSensitive(u, "update_id");
            if (cJSON_IsNumber(uid) && (long long)uid->valuedouble >= offset) offset = (long long)uid->valuedouble + 1;
            /* a setting changed under us (the token, off): not for this one */
            uint32_t g2;
            if (!settings_copy(token, sizeof token, &g2) || g2 != gen) break;
            handle_update(u);
        }
        cJSON_Delete(root);
        if (offset != before) {
            /* kept, so a restart does not do the last commands again */
            snprintf(offs, sizeof offs, "%lld", offset);
            aos_hal_pref_set_str("bot_offset", offs);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* bot_out: the outbox and the rules                                         */
/* ------------------------------------------------------------------------ */

/* The screen, as a JPEG on the card; its bytes in *out (PSRAM). */
static bool shot_jpeg(uint8_t **out, int *len)
{
    const char *sd = aos_hal_path_sd_root();
    if (!sd) return false;
    char path[96];
    snprintf(path, sizeof path, "%s/bot/captura.jpg", sd);
    aos_ui_snapshot_t s;
    bool ok = aos_ui_request_snapshot(true);
    aos_snapshot_state_t st = AOS_SNAPSHOT_PENDING;
    for (int i = 0; ok && i < 60 && (st = aos_ui_snapshot_peek(&s)) == AOS_SNAPSHOT_PENDING; i++) aos_hal_sleep_ms(50);
    if (!ok || st != AOS_SNAPSHOT_READY) {
        if (st == AOS_SNAPSHOT_READY || st == AOS_SNAPSHOT_FAILED) aos_ui_snapshot_release();
        return false;
    }
    ok = aos_hal_jpeg_save(path, (const uint16_t *)s.data, s.w, s.h, (int)(s.stride / 2), 82);
    aos_ui_snapshot_release();
    if (!ok) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 && n < 4 * 1024 * 1024 ? big_calloc((size_t)n) : NULL;
    ok = b && fread(b, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { free(b); return false; }
    *out = b;
    *len = (int)n;
    return true;
}

static bool send_photo(conn_t *c, const api_t *a, const char *token, int64_t chat, const uint8_t *jpg, int jn,
                       const char *caption, char *why, size_t wn)
{
    static const char *const BND = "p4osbot7f3a9";
    char head[1024];
    int hn = snprintf(head, sizeof head,
                      "--%s\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n%lld\r\n"
                      "--%s\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n%.600s\r\n"
                      "--%s\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"captura.jpg\"\r\n"
                      "Content-Type: image/jpeg\r\n\r\n",
                      BND, (long long)chat, BND, caption ? caption : "", BND);
    char tail[40];
    int tn = snprintf(tail, sizeof tail, "\r\n--%s--\r\n", BND);
    int total = hn + jn + tn;
    uint8_t *body = big_calloc((size_t)total);
    if (!body) { scpy(why, wn, _("No hay memoria")); return false; }
    memcpy(body, head, (size_t)hn);
    memcpy(body + hn, jpg, (size_t)jn);
    memcpy(body + hn + jn, tail, (size_t)tn);
    char ct[64];
    snprintf(ct, sizeof ct, "multipart/form-data; boundary=%s", BND);
    resp_t r = http_call(c, a, token, "sendPhoto", ct, body, total, 30000, why, wn, NULL);
    free(body);
    bool ok = r.status == 200;
    if (!ok && r.body && !why[0]) scpy(why, wn, r.body);
    free(r.body);
    return ok;
}

static int chats_copy(int64_t *ids, int max)
{
    lock();
    int n = B->nchats < max ? B->nchats : max;
    for (int i = 0; i < n; i++) ids[i] = B->chats[i].id;
    unlock();
    return n;
}

static void set_commands(conn_t *c, const api_t *a, const char *token, char *why, size_t wn)
{
    /* the menu in the board's language: every name works in both */
    struct { uint32_t bit; const char *cmd, *en, *desc; } L[] = {
        { 0, "ayuda", "help", N_("Qué sé hacer") },
        { AOS_BOT_CMD_STATE, "estado", "status", N_("Cómo está la placa") },
        { AOS_BOT_CMD_SHOT, "captura", "screenshot", N_("Lo que muestra la pantalla") },
        { AOS_BOT_CMD_ZIGBEE, "zigbee", "zigbee", N_("Los dispositivos Zigbee") },
        { AOS_BOT_CMD_IR, "ir", "ir", N_("Los códigos IR guardados") },
        { AOS_BOT_CMD_NOTICE, "aviso", "notice", N_("Un aviso en la pantalla") },
        { AOS_BOT_CMD_RULES, "reglas", "rules", N_("Las reglas") },
    };
    const char *lang = aos_i18n_current();
    bool es = !lang || !strncmp(lang, "es", 2);
    cJSON *args = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(args, "commands");
    for (size_t i = 0; i < sizeof L / sizeof L[0]; i++) {
        if (L[i].bit && !cmd_on(L[i].bit)) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "command", es ? L[i].cmd : L[i].en);
        cJSON_AddStringToObject(o, "description", aos_tr(L[i].desc));
        cJSON_AddItemToArray(arr, o);
    }
    int st = 0;
    cJSON *r = api_json(c, a, token, "setMyCommands", args, 15000, &st, why, wn);
    cJSON_Delete(args);
    cJSON_Delete(r);
    if (st == 200) {
        lock();
        B->commands_set = true;
        unlock();
    }
}

static bool run_job(conn_t *c, const api_t *a, const char *token, job_t *j, char *why, size_t wn)
{
    int64_t ids[AOS_BOT_CHATS_MAX];
    int n = 1;
    if (j->chat) ids[0] = j->chat;
    else n = chats_copy(ids, AOS_BOT_CHATS_MAX);
    if (j->kind == J_ANSWER) {
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "callback_query_id", j->cb_id);
        if (j->text[0]) cJSON_AddStringToObject(args, "text", j->text);
        int st = 0;
        cJSON *r = api_json(c, a, token, "answerCallbackQuery", args, 15000, &st, why, wn);
        cJSON_Delete(args);
        cJSON_Delete(r);
        return st >= 0;
    }
    if (j->kind == J_SHOT) {
        uint8_t *jpg = NULL;
        int jn = 0;
        if (!shot_jpeg(&jpg, &jn)) {
            /* said instead, to the same chats (j is the thread's copy) */
            j->kind = J_TEXT;
            j->markup[0] = 0;
            scpy(j->text, sizeof j->text, _("No pude sacar la captura (hace falta la tarjeta)."));
            return run_job(c, a, token, j, why, wn);
        }
        bool ok = true;
        for (int i = 0; i < n; i++) {
            ok &= send_photo(c, a, token, ids[i], jpg, jn, j->text, why, wn);
            if (ok) { lock(); B->sent++; B->version++; unlock(); }
        }
        free(jpg);
        return ok || why[0];
    }
    bool ok = true;
    for (int i = 0; i < n; i++) {
        cJSON *args = cJSON_CreateObject();
        cJSON_AddNumberToObject(args, "chat_id", (double)ids[i]);
        cJSON_AddStringToObject(args, "text", j->text);
        if (j->markup[0]) {
            cJSON *mk = cJSON_Parse(j->markup);
            if (mk) cJSON_AddItemToObject(args, "reply_markup", mk);
        }
        int st = 0;
        cJSON *r = api_json(c, a, token, "sendMessage", args, 15000, &st, why, wn);
        cJSON_Delete(args);
        cJSON_Delete(r);
        if (st == 200) { lock(); B->sent++; B->version++; unlock(); }
        else if (st < 0) ok = false;        /* the network: tried again; Telegram's refusal is not */
        else logf_(_("Telegram no aceptó un mensaje: %.50s"), why);
    }
    return ok;
}

/* -- the rules -- */

static void expand(const char *tpl, const char *dev, const char *val, const char *app, const char *title,
                   const char *text, char *out, size_t n)
{
    char hora[8];
    time_text(hora, sizeof hora);
    struct { const char *k, *v; } V[] = {
        { "{dispositivo}", dev }, { "{valor}", val }, { "{hora}", hora }, { "{placa}", aos_hal_device_name() },
        { "{app}", app }, { "{titulo}", title }, { "{texto}", text },
        /* the same in English, for whoever reads the board in English */
        { "{device}", dev }, { "{value}", val }, { "{time}", hora }, { "{board}", aos_hal_device_name() },
        { "{title}", title }, { "{text}", text },
    };
    size_t o = 0;
    for (const char *p = tpl; *p && o + 1 < n;) {
        bool hit = false;
        if (*p == '{')
            for (size_t i = 0; i < sizeof V / sizeof V[0] && !hit; i++) {
                size_t kl = strlen(V[i].k);
                if (!strncmp(p, V[i].k, kl)) {
                    o += (size_t)snprintf(out + o, n - o, "%s", V[i].v ? V[i].v : "");
                    if (o >= n) o = n - 1;
                    p += kl;
                    hit = true;
                }
            }
        if (!hit) out[o++] = *p++;
    }
    out[o] = 0;
}

static void act(const aos_bot_rule_t *r, const char *msg)
{
    switch (r->act) {
    case AOS_BOT_A_MESSAGE: {
        memset(&s_jo, 0, sizeof s_jo);
        s_jo.kind = J_TEXT;
        scpy(s_jo.text, sizeof s_jo.text, msg);
        out_push(&s_jo);
        break;
    }
    case AOS_BOT_A_SHOT:
        memset(&s_jo, 0, sizeof s_jo);
        s_jo.kind = J_SHOT;
        scpy(s_jo.text, sizeof s_jo.text, msg);
        out_push(&s_jo);
        break;
    case AOS_BOT_A_ZB_ON: case AOS_BOT_A_ZB_OFF: case AOS_BOT_A_ZB_TOGGLE: {
        int nd = zb_snapshot(s_zb_out);
        for (int i = 0; i < nd; i++)
            if (!memcmp(s_zb_out[i].ieee, r->act_ieee, 8)) {
                uint8_t ep = first_out(&s_zb_out[i], NULL);
                if (ep) aos_zigbee_onoff(r->act_ieee, ep, r->act == AOS_BOT_A_ZB_ON ? 1 : r->act == AOS_BOT_A_ZB_OFF ? 0 : 2);
            }
        break;
    }
    default: {
        static aos_zigbee_ir_code_t ir[24];
        int n = aos_zigbee_ir_codes(ir, 24);
        for (int i = 0; i < n; i++)
            if (!strcasecmp(ir[i].name, r->ir)) { aos_zigbee_ir_send_saved(i); break; }
    }
    }
}

static void fire(int i, const aos_bot_rule_t *r, const char *dev, const char *val, const char *app,
                 const char *title, const char *text)
{
    uint32_t now = now_ms();
    uint32_t gap = (r->gap_s ? r->gap_s : 30) * 1000u;
    if (s_rs[i].fired_ms && now - s_rs[i].fired_ms < gap) return;
    s_rs[i].fired_ms = now ? now : 1;
    char msg[TEXT_MAX];
    const char *tpl = r->text;
    if (!tpl[0]) {
        switch (r->trig) {
        case AOS_BOT_T_ZIGBEE: tpl = "🔔 {dispositivo}: {valor}"; break;
        case AOS_BOT_T_TIME: tpl = "⏰ {placa}, {hora}"; break;
        case AOS_BOT_T_BOOT: tpl = _("✅ {placa} arrancó"); break;
        default: tpl = "📩 {app}: {titulo}\n{texto}";
        }
    }
    expand(tpl, dev, val, app, title, text, msg, sizeof msg);
    char line[300];
    aos_bot_rule_text(r, line, sizeof line);
    logf_(_("Regla %d: %.70s"), i + 1, line);
    act(r, msg);
}

static void rules_tick(bool online)
{
    static aos_bot_rule_t R[AOS_BOT_RULES_MAX];
    lock();
    int n = B->nrules;
    memcpy(R, B->rules, sizeof R[0] * (size_t)n);
    uint32_t gen = B->rules_gen;
    bool boot = B->boot_pending && online;
    if (boot) B->boot_pending = false;
    unlock();
    if (gen != s_rs_gen) { memset(s_rs, 0, sizeof s_rs); s_rs_gen = gen; }
    int nd = -1;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    /* an hour's rule fires as the clock goes into its minute: not for the
     * minute the board started (or got the time) in, so a restart at 8:00
     * does not say "8:00" twice */
    static int32_t last_key = -1;
    int32_t key_now = tm.tm_year > 120 ? tm.tm_yday * 1440 + tm.tm_hour * 60 + tm.tm_min : -1;
    bool new_minute = key_now >= 0 && last_key >= 0 && key_now != last_key;
    last_key = key_now;
    /* the notifications that came since the last look */
    static aos_notif_t fresh[6];
    int nfresh = 0;
    int nn = aos_hal_notif_count();
    if (nn > 0) {
        aos_notif_t x;
        for (int k = 0; k < nn && nfresh < 6; k++) {
            if (!aos_hal_notif_at(k, &x)) break;
            if (s_notif_seen && x.uid == s_notif_uid) break;
            if (!s_notif_seen) break;
            if (!x.pre_existing && strcmp(x.app, NOTIF_APP)) fresh[nfresh++] = x;
        }
        if (aos_hal_notif_at(0, &x)) s_notif_uid = x.uid;
        s_notif_seen = true;
    } else {
        s_notif_seen = true;
        s_notif_uid = 0;
    }
    for (int i = 0; i < n; i++) {
        const aos_bot_rule_t *r = &R[i];
        if (!r->on) continue;
        if (r->trig == AOS_BOT_T_ZIGBEE) {
            if (nd < 0) nd = zb_snapshot(s_zb_out);
            const aos_zigbee_dev_t *d = NULL;
            for (int k = 0; k < nd && !d; k++) if (!memcmp(s_zb_out[k].ieee, r->ieee, 8)) d = &s_zb_out[k];
            if (!d) continue;
            char dn[40], vt[40];
            dev_name(d, dn, sizeof dn);
            if (r->var == AOS_BOT_V_EVENT) {
                if (s_rs[i].known && d->event_ms != s_rs[i].event_ms && d->event[0]) fire(i, r, dn, d->event, "", "", "");
                s_rs[i].event_ms = d->event_ms;
                s_rs[i].known = true;
                continue;
            }
            float v;
            if (!var_of(d, r->var, &v)) continue;
            bool was = s_rs[i].known;
            float prev = s_rs[i].last;
            bool hit = false;
            if (was) switch (r->op) {
                case AOS_BOT_OP_CHANGE: hit = v != prev; break;
                case AOS_BOT_OP_ABOVE: hit = prev <= r->value && v > r->value; break;
                case AOS_BOT_OP_BELOW: hit = prev >= r->value && v < r->value; break;
                default: hit = fabsf(v - r->value) < 0.05f && fabsf(prev - r->value) >= 0.05f;
            }
            s_rs[i].last = v;
            s_rs[i].known = true;
            if (hit) {
                var_text(d, r->var, v, vt, sizeof vt);
                fire(i, r, dn, vt, "", "", "");
            }
        } else if (r->trig == AOS_BOT_T_TIME) {
            if (!new_minute) continue;
            if (tm.tm_hour * 60 + tm.tm_min == r->at_min && (r->days & (1u << tm.tm_wday)) && key_now != s_rs[i].time_key) {
                s_rs[i].time_key = key_now;
                char hora[8];
                time_text(hora, sizeof hora);
                fire(i, r, "", hora, "", "", "");
            }
        } else if (r->trig == AOS_BOT_T_BOOT) {
            if (boot) fire(i, r, "", "", "", "", "");
        } else {
            for (int k = nfresh - 1; k >= 0; k--)
                if (ci_contains(fresh[k].app, r->filter) || ci_contains(fresh[k].title, r->filter)) {
                    s_rs[i].fired_ms = 0;       /* each notification is its own */
                    fire(i, r, "", "", fresh[k].app, fresh[k].title, fresh[k].message);
                }
        }
    }
}

static void out_thread(void *arg)
{
    (void)arg;
    static conn_t c;
    static char token[96], why[100];
    uint32_t last_rules = 0;
    for (;;) {
        uint32_t gen;
        bool on = settings_copy(token, sizeof token, &gen);
        lock();
        bool online = B->state == AOS_BOT_ONLINE;
        bool cmds = B->commands_set;
        unlock();
        uint32_t now = now_ms();
        if (on && now - last_rules >= 1000) {
            last_rules = now;
            rules_tick(online);
        }
        if (!on || !online) {
            conn_close(&c);
            /* what waits for an offline bot is dropped after a while: an
             * alert two hours late is worse than none */
            lock();
            if (!on) B->out_n = 0;
            unlock();
            aos_hal_sleep_ms(300);
            continue;
        }
        api_t a;
        api_get(&a);
        if (!cmds) set_commands(&c, &a, token, why, sizeof why);
        lock();
        bool any = B->out_n > 0;
        if (any) s_jo = s_out[B->out_r];
        unlock();
        if (!any) { aos_hal_sleep_ms(100); continue; }
        static job_t j;
        j = s_jo;
        if (run_job(&c, &a, token, &j, why, sizeof why)) {
            lock();
            B->out_r = (B->out_r + 1) % OUT_MAX;
            B->out_n--;
            unlock();
        } else {
            logf_(_("No pude mandar: %.60s"), why[0] ? why : "?");
            aos_hal_sleep_ms(3000);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* The API                                                                   */
/* ------------------------------------------------------------------------ */

/* The threads and their memory, the first time the bot is on. */
static void start(void)
{
    if (s_started) return;
    if (!s_out) s_out = big_calloc(sizeof(job_t) * OUT_MAX);
    if (!s_zb_in) s_zb_in = big_calloc(sizeof(aos_zigbee_dev_t) * AOS_ZB_DEVICES_MAX);
    if (!s_zb_out) s_zb_out = big_calloc(sizeof(aos_zigbee_dev_t) * AOS_ZB_DEVICES_MAX);
    if (!s_out || !s_zb_in || !s_zb_out) { aos_hal_log(TAG, "no memory to start"); return; }
    /* the TLS handshake checks a certificate chain: ~7 KB of stack (PSRAM) */
    if (!aos_hal_thread_start("bot_in", in_thread, NULL, 10240, 3)) return;
    if (!aos_hal_thread_start("bot_out", out_thread, NULL, 10240, 3)) return;
    s_started = true;
}

void aos_bot_boot(void)
{
    if (B) return;
    B = big_calloc(sizeof *B);
    if (!B) { aos_hal_log(TAG, "no memory"); return; }
    B->mx = aos_hal_mutex_create();
    int32_t v = 0;
    B->enabled = aos_hal_pref_get_i32("bot_on", &v) && v;
    aos_hal_pref_get_str("sec_bot_token", B->token, sizeof B->token);
    v = AOS_BOT_CMD_ALL;
    aos_hal_pref_get_i32("bot_cmds", &v);
    B->cmds = (uint32_t)v;
    chats_load();
    rules_load();
    B->boot_pending = B->enabled;     /* "when the board starts": a bot switched on later did not see it */
    if (B->enabled) start();
}

void aos_bot_info(aos_bot_info_t *out)
{
    memset(out, 0, sizeof *out);
    if (!B) return;
    lock();
    out->state = B->state;
    scpy(out->username, sizeof out->username, B->username);
    scpy(out->why, sizeof out->why, B->why);
    out->received = B->received;
    out->sent = B->sent;
    out->chats = B->nchats;
    int32_t left = (int32_t)(B->code_until - now_ms());
    if (B->code && left > 0) {
        out->code = B->code;
        out->code_left_s = (uint32_t)left / 1000;
    }
    unlock();
}

bool aos_bot_enabled(void) { return B && B->enabled; }

void aos_bot_enable(bool on)
{
    if (!B) return;
    lock();
    B->enabled = on;
    B->token_gen++;
    B->version++;
    unlock();
    aos_hal_pref_set_i32("bot_on", on);
    if (on) start();
    logf_("%s", on ? _("Bot prendido") : _("Bot apagado"));
}

bool aos_bot_has_token(void) { return B && B->token[0]; }

bool aos_bot_set_token(const char *token)
{
    if (!B || !token) return false;
    char t[96];
    size_t k = 0;
    for (const char *p = token; *p && k + 1 < sizeof t; p++)
        if (!isspace((unsigned char)*p)) t[k++] = *p;
    t[k] = 0;
    /* "123456789:AA..." : digits, a colon, then letters, digits, - and _ */
    const char *colon = strchr(t, ':');
    bool ok = !t[0];
    if (colon && colon > t && strlen(colon + 1) >= 20) {
        ok = true;
        for (const char *p = t; p < colon; p++) ok &= isdigit((unsigned char)*p) != 0;
        for (const char *p = colon + 1; *p; p++) ok &= isalnum((unsigned char)*p) || *p == '-' || *p == '_';
    }
    if (!ok) return false;
    lock();
    scpy(B->token, sizeof B->token, t);
    B->token_gen++;
    B->username[0] = 0;
    B->version++;
    unlock();
    if (t[0]) aos_hal_pref_set_str("sec_bot_token", t);
    else aos_hal_pref_erase("sec_bot_token");
    aos_hal_pref_erase("bot_offset");
    logf_("%s", t[0] ? _("Token nuevo guardado") : _("Token borrado"));
    return true;
}

uint32_t aos_bot_link_code(void)
{
    if (!B) return 0;
    uint32_t r;
    aos_hal_random(&r, sizeof r);
    lock();
    B->code = 100000 + r % 900000;
    B->code_until = now_ms() + CODE_S * 1000u;
    B->version++;
    uint32_t c = B->code;
    unlock();
    return c;
}

void aos_bot_link_cancel(void)
{
    if (!B) return;
    lock();
    B->code = 0;
    B->version++;
    unlock();
}

int aos_bot_chats(aos_bot_chat_t *out, int max)
{
    if (!B) return 0;
    lock();
    int n = B->nchats < max ? B->nchats : max;
    if (out) memcpy(out, B->chats, sizeof out[0] * (size_t)n);
    unlock();
    return n;
}

bool aos_bot_chat_forget(int64_t id)
{
    if (!B) return false;
    lock();
    int k = 0;
    for (int i = 0; i < B->nchats; i++) if (B->chats[i].id != id) B->chats[k++] = B->chats[i];
    bool hit = k != B->nchats;
    B->nchats = k;
    if (hit) { chats_save(); B->version++; }
    unlock();
    return hit;
}

uint32_t aos_bot_cmds(void) { return B ? B->cmds : 0; }

void aos_bot_set_cmds(uint32_t mask)
{
    if (!B) return;
    lock();
    B->cmds = mask & AOS_BOT_CMD_ALL;
    B->commands_set = false;        /* Telegram's menu, again */
    B->version++;
    unlock();
    aos_hal_pref_set_i32("bot_cmds", (int32_t)(mask & AOS_BOT_CMD_ALL));
}

int aos_bot_rules(aos_bot_rule_t *out, int max)
{
    if (!B) return 0;
    lock();
    int n = B->nrules < max ? B->nrules : max;
    if (out) memcpy(out, B->rules, sizeof out[0] * (size_t)n);
    unlock();
    return n;
}

bool aos_bot_rule_set(int i, const aos_bot_rule_t *r)
{
    if (!B || !r || r->trig >= AOS_BOT_T_N || r->act >= AOS_BOT_A_N || r->var >= AOS_BOT_V_N || r->op >= AOS_BOT_OP_N)
        return false;
    lock();
    bool ok = i < 0 ? B->nrules < AOS_BOT_RULES_MAX : i < B->nrules;
    if (ok) {
        if (i < 0) i = B->nrules++;
        B->rules[i] = *r;
        B->rules[i].text[sizeof r->text - 1] = 0;
        for (char *p = B->rules[i].text; *p; p++) if (*p == '\x1f') *p = ' ';
        for (char *p = B->rules[i].filter; *p; p++) if (*p == '\x1f') *p = ' ';
        for (char *p = B->rules[i].ir; *p; p++) if (*p == '\x1f') *p = ' ';
        rules_save();
        B->rules_gen++;
        B->version++;
    }
    unlock();
    return ok;
}

bool aos_bot_rule_delete(int i)
{
    if (!B) return false;
    lock();
    bool ok = i >= 0 && i < B->nrules;
    if (ok) {
        memmove(&B->rules[i], &B->rules[i + 1], sizeof B->rules[0] * (size_t)(B->nrules - i - 1));
        B->nrules--;
        rules_save();
        B->rules_gen++;
        B->version++;
    }
    unlock();
    return ok;
}

bool aos_bot_send(const char *text)
{
    if (!B || !text || !text[0]) return false;
    lock();
    bool ok = B->enabled && (B->cmds & AOS_BOT_CMD_APPS) && B->nchats > 0;
    unlock();
    if (!ok) return false;
    static job_t j;
    static void *mx;
    if (!mx) mx = aos_hal_mutex_create();
    aos_hal_mutex_lock(mx);
    memset(&j, 0, sizeof j);
    j.kind = J_TEXT;
    scpy(j.text, sizeof j.text, text);
    ok = out_push(&j);
    aos_hal_mutex_unlock(mx);
    return ok;
}

int aos_bot_log(char (*lines)[96], int max)
{
    if (!B) return 0;
    lock();
    int n = B->log_n < max ? B->log_n : max;
    for (int i = 0; i < n; i++) scpy(lines[i], 96, B->log[(B->log_w - n + i + LOG_MAX) % LOG_MAX]);
    unlock();
    return n;
}

uint32_t aos_bot_version(void) { return B ? B->version : 0; }
