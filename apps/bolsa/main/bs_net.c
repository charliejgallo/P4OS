/*
 * P4OS - Bolsa: the queue of requests.
 *
 * Every request goes through here: one at a time per provider and two at
 * most in all (the HAL has three slots and the rest of the system uses
 * them too). Each provider has its own pace, because the free plans count:
 *
 *   CoinGecko       ~5 a minute, shared by IP          one every 13 s
 *   Twelve Data     8 credits a minute (a symbol each) 7.6 s per credit
 *   Alpha Vantage   25 a day                           one every 15 s
 *   Finnhub         60 a minute                        one a second
 *   Stooq, ArgentinaDatos                              one a second or so
 *
 * When a provider says "too many", it is left alone for a while (a minute,
 * or an hour for the daily limits) and the footer says so. When it says the
 * key is wrong, nothing more goes to it until the configuration changes.
 * The queue is looked at from the tick; nothing blocks.
 */
#include "bs.h"

#define QMAX        48
#define TIMEOUT_MS  30000

void (*bs_on_data)(const char *sym, int what);

typedef struct {
    bs_job_t job;
    bool     used;
} slot_t;

static slot_t   s_q[QMAX];
static struct {
    int      req;                       /* HAL id, 0 = idle */
    bs_job_t job;
    uint32_t start_ms;
    uint32_t next_ms;                   /* not before this */
    uint32_t limited_until;
    bool     bad_key;
    int      last_err;
} s_p[BS_P_COUNT];
static char           s_last_err[96];
static bs_search_cb_t s_search_cb;
static bs_hit_t       s_hits[16];

static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

static int gap_ms(bs_prov_t p, int credits)
{
    /* against the fake server the plans' pace only slows the tests down */
    const char *fake = getenv("BOLSA_FAKE");
    int div = fake && fake[0] ? 10 : 1;
    switch (p) {
    case BS_P_COINGECKO: return 13000 / div;
    case BS_P_TWELVE:    return 7600 * (credits < 1 ? 1 : credits) / div;
    case BS_P_ALPHA:     return 15000 / div;
    case BS_P_FINNHUB:   return 1100 / div;
    case BS_P_STOOQ:     return 1500 / div;
    default:             return 800 / div;
    }
}

static int credits(const bs_job_t *j)
{
    if (j->kind != BS_J_QUOTES || j->prov != BS_P_TWELVE) return 1;
    int n = 1;
    for (const char *c = j->syms; *c; c++) n += *c == ',';
    return n;
}

void bs_net_init(void)
{
    memset(s_q, 0, sizeof s_q);
    for (int i = 0; i < BS_P_COUNT; i++) {
        s_p[i].req = 0;
        s_p[i].bad_key = false;
        /* the pace and the limits survive the app being built again */
    }
    s_search_cb = NULL;
}

void bs_net_stop(void)
{
    for (int i = 0; i < BS_P_COUNT; i++) {
        if (s_p[i].req > 0) {
            /* always released, in flight or not: the HAL's task cleans up */
            aos_hal_http_release(s_p[i].req);
            s_p[i].req = 0;
        }
    }
    memset(s_q, 0, sizeof s_q);
    s_search_cb = NULL;
}

static bool same(const bs_job_t *a, const bs_job_t *b)
{
    return a->kind == b->kind && a->prov == b->prov && a->res == b->res && !strcmp(a->sym, b->sym) &&
           !strcmp(a->syms, b->syms) && !strcmp(a->query, b->query);
}

bool bs_net_queue(const bs_job_t *j, bool front)
{
    if (j->prov <= BS_P_NONE || j->prov >= BS_P_COUNT || s_p[j->prov].bad_key) return false;
    if (s_p[j->prov].req > 0 && same(&s_p[j->prov].job, j)) return true;
    int freei = -1;
    for (int i = 0; i < QMAX; i++) {
        if (s_q[i].used && same(&s_q[i].job, j)) {
            if (!front) return true;
            s_q[i].used = false;        /* moved to the front below */
            freei = i;
            break;
        }
        if (!s_q[i].used && freei < 0) freei = i;
    }
    if (freei < 0) return false;
    if (front) {
        /* the front: shift everything down one */
        int last = QMAX - 1;
        for (int i = 0; i < QMAX; i++) if (!s_q[i].used) { last = i; break; }
        for (int i = last; i > 0; i--) s_q[i] = s_q[i - 1];
        freei = 0;
    }
    s_q[freei].used = true;
    s_q[freei].job = *j;
    return true;
}

int bs_net_pending(void)
{
    int n = 0;
    for (int i = 0; i < QMAX; i++) n += s_q[i].used;
    for (int i = 0; i < BS_P_COUNT; i++) n += s_p[i].req > 0;
    return n;
}

bool bs_net_busy(void)
{
    for (int i = 0; i < BS_P_COUNT; i++)
        if (s_p[i].req > 0) return true;
    return false;
}

const char *bs_net_last_error(void) { return s_last_err; }

bool bs_net_limited(bs_prov_t p, char *why, size_t n)
{
    if (p <= BS_P_NONE || p >= BS_P_COUNT) return false;
    if (s_p[p].bad_key) {
        snprintf(why, n, _("%s rechazó la clave"), bs_prov_name(p));
        return true;
    }
    if ((int32_t)(s_p[p].limited_until - now_ms()) > 0) {
        snprintf(why, n, _("%s pidió esperar"), bs_prov_name(p));
        return true;
    }
    return false;
}

static void fail(const bs_job_t *j, int err)
{
    bs_prov_t p = (bs_prov_t)j->prov;
    s_p[p].last_err = err;
    const char *what = err == BS_E_KEY ? _("clave rechazada") : err == BS_E_LIMIT ? _("límite del plan gratis") :
                       err == BS_E_NOSYM ? _("no conoce el símbolo") : err == BS_E_PLAN ? _("no está en el plan") :
                       err == BS_E_EMPTY ? _("sin datos") : err == BS_E_PARSE ? _("respuesta rara") : _("sin respuesta");
    snprintf(s_last_err, sizeof s_last_err, "%s: %s", bs_prov_name(p), what);
    aos_hal_log("bolsa", "%s %s %s res %d: error %d", bs_prov_name(p),
                j->kind == BS_J_QUOTES ? "quotes" : j->kind == BS_J_SEARCH ? "search" : "series",
                j->kind == BS_J_QUOTES ? j->syms : j->sym, j->res, err);
    if (err == BS_E_LIMIT) {
        uint32_t wait = p == BS_P_ALPHA || p == BS_P_STOOQ ? 3600000u : 65000u;
        s_p[p].limited_until = now_ms() + wait;
    } else if (err == BS_E_KEY) {
        s_p[p].bad_key = true;
        for (int i = 0; i < QMAX; i++)
            if (s_q[i].used && s_q[i].job.prov == p) s_q[i].used = false;
    }
    if (j->kind == BS_J_SEARCH) {
        if (s_search_cb) s_search_cb(s_hits, 0, err);
        s_search_cb = NULL;
        return;
    }
    /* the item remembers it, for its row and its detail */
    char first[BS_SYM_LEN];
    const char *src = j->kind == BS_J_QUOTES ? j->syms : j->sym;
    size_t k = 0;
    while (src[k] && src[k] != ',' && k + 1 < sizeof first) { first[k] = src[k]; k++; }
    first[k] = 0;
    for (const char *s = src; s && *s;) {
        char one[BS_SYM_LEN];
        size_t w = 0;
        while (*s && *s != ',') { if (w + 1 < sizeof one) one[w++] = *s; s++; }
        one[w] = 0;
        if (*s == ',') s++;
        bs_item_t *it = bs_item_find(one);
        if (it) {
            it->err = err;
            it->err_prov = (uint8_t)p;
            it->err_ms = now_ms();
        }
    }
    if (bs_on_data) bs_on_data(first, j->kind == BS_J_QUOTES ? BS_D_QUOTE : j->res);
}

static void done(int p)
{
    bs_job_t *j = &s_p[p].job;
    int id = s_p[p].req;
    aos_http_state_t st = aos_hal_http_state(id);
    if (st == AOS_HTTP_BUSY) {
        if ((int32_t)(now_ms() - s_p[p].start_ms) > TIMEOUT_MS) {
            aos_hal_http_release(id);
            s_p[p].req = 0;
            fail(j, BS_E_HTTP);
        }
        return;
    }
    s_p[p].req = 0;
    if (st != AOS_HTTP_DONE) {
        int why = aos_hal_http_status(id);
        aos_hal_http_release(id);
        snprintf(s_last_err, sizeof s_last_err, _("%s: sin conexión (%d)"), bs_prov_name((bs_prov_t)p), why);
        fail(j, BS_E_HTTP);
        return;
    }
    int code = aos_hal_http_status(id);
    const char *body = aos_hal_http_body(id);
    int len = aos_hal_http_len(id);
    uint32_t t0 = now_ms();
    int err = 0;
    if (!body || len <= 0) {
        err = code == 429 ? BS_E_LIMIT : BS_E_EMPTY;
    } else if (code == 429) {
        err = BS_E_LIMIT;
    } else if (code == 401 || code == 403) {
        err = p == BS_P_FINNHUB && code == 403 ? BS_E_PLAN : BS_E_KEY;
    } else if (code == 404 && p != BS_P_TWELVE) {
        err = BS_E_NOSYM;
    } else if (code >= 400) {
        err = BS_E_HTTP;
    } else if (j->kind == BS_J_QUOTES) {
        err = bs_prov_parse_quotes(j, body, len);
        if (!err && bs_on_data) bs_on_data(NULL, BS_D_QUOTE);     /* every row of the batch */
    } else if (j->kind == BS_J_SEARCH) {
        int n = 0;
        err = bs_prov_parse_search(j, body, len, s_hits, (int)(sizeof s_hits / sizeof s_hits[0]), &n);
        if (!err && s_search_cb) {
            bs_search_cb_t cb = s_search_cb;
            s_search_cb = NULL;
            cb(s_hits, n, 0);
        }
    } else {
        bs_series_t s;
        memset(&s, 0, sizeof s);
        err = bs_prov_parse_series(j, body, len, &s);
        if (!err) {
            bs_item_t *it = bs_item(j->sym);
            int res = j->res;
            s.fetched = bs_now();
            s.fetched_ms = now_ms() | 1;
            if (it) {
                bs_series_free(&it->s[res]);
                it->s[res] = s;
                it->loaded[res] = true;
                it->err = 0;
                bs_cache_save(j->sym, res, &it->s[res]);
                if (bs_on_data) bs_on_data(j->sym, res);
            } else {
                bs_series_free(&s);
            }
        } else {
            bs_series_free(&s);
        }
    }
    aos_hal_log("bolsa", "%s %s %s res %d: HTTP %d, %d bytes, parsed in %lu ms%s",
                bs_prov_name((bs_prov_t)p), j->kind == BS_J_QUOTES ? "quotes" : j->kind == BS_J_SEARCH ? "search" : "series",
                j->kind == BS_J_QUOTES ? j->syms : j->kind == BS_J_SEARCH ? j->query : j->sym, j->res, code, len,
                (unsigned long)(now_ms() - t0), err ? " (error)" : "");
    aos_hal_http_release(id);
    if (err) fail(j, err);
    else s_p[p].last_err = 0;
}

void bs_net_poll(void)
{
    for (int p = 1; p < BS_P_COUNT; p++)
        if (s_p[p].req > 0) done(p);

    int inflight = 0;
    for (int p = 1; p < BS_P_COUNT; p++) inflight += s_p[p].req > 0;
    if (aos_hal_net_state() != AOS_NET_CONNECTED) return;

    uint32_t t = now_ms();
    for (int i = 0; i < QMAX && inflight < 2; i++) {
        if (!s_q[i].used) continue;
        bs_job_t *j = &s_q[i].job;
        int p = j->prov;
        if (s_p[p].req > 0 || (int32_t)(s_p[p].next_ms - t) > 0 || (int32_t)(s_p[p].limited_until - t) > 0)
            continue;
        if (s_p[p].bad_key) { s_q[i].used = false; continue; }
        char url[512];
        int max = 0;
        if (!bs_prov_url(j, url, sizeof url, &max)) {
            s_q[i].used = false;
            continue;
        }
        int id = aos_hal_http_get(url, max);
        if (id <= 0) break;         /* no slot now: next tick */
        s_p[p].req = id;
        s_p[p].job = *j;
        s_p[p].start_ms = t;
        s_p[p].next_ms = t + (uint32_t)gap_ms((bs_prov_t)p, credits(j));
        s_q[i].used = false;
        inflight++;
    }
}

void bs_net_search(const char *q, bs_prov_t p, bs_search_cb_t cb)
{
    bs_job_t j;
    memset(&j, 0, sizeof j);
    j.kind = BS_J_SEARCH;
    j.prov = (uint8_t)p;
    bs_scpy(j.query, sizeof j.query, q);
    /* only the latest search matters */
    for (int i = 0; i < QMAX; i++)
        if (s_q[i].used && s_q[i].job.kind == BS_J_SEARCH) s_q[i].used = false;
    s_search_cb = cb;
    if (!bs_net_queue(&j, true)) {
        s_search_cb = NULL;
        if (cb) cb(s_hits, 0, BS_E_KEY);
    }
}
