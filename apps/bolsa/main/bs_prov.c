/*
 * P4OS - Bolsa: one layer per provider.
 *
 * For each provider, the URL of what is wanted and the parser of what comes
 * back. Nothing here draws or keeps state: a job goes in, a URL comes out;
 * a body goes in, a series or quotes come out.
 *
 * What each one gives (checked against their documentation on 2026-11-21,
 * and tested only against apps/bolsa/tools/fake_quotes.py):
 *
 *   CoinGecko       no key. Prices of several coins in one request; a
 *                   chart of 1 day (5 min), up to 90 days (hourly) or a year
 *                   (daily, the keyless limit). Closes only, no open/high/low.
 *                   About 5 requests a minute, shared by IP; attribution
 *                   required, so the footer always names it.
 *   ArgentinaDatos  no key. The whole daily history of each dollar house,
 *                   of each currency and of the country risk, in one request.
 *   Stooq           a free key (since 2026 the CSV wants one). Daily
 *                   open/high/low/close/volume, all of it or from a date.
 *   Twelve Data     a free key: 8 credits a minute, 800 a day. Quotes with
 *                   the 52 weeks, intraday (5 min, 30 min) and daily.
 *   Alpha Vantage   a free key: 25 requests a day. Quotes, the last 100
 *                   days, and weekly for the long ranges.
 *   Finnhub         a free key: quotes only (its candles are a paid plan).
 *
 * Development: BOLSA_FAKE=http://127.0.0.1:8248 sends every provider to the
 * fake server, each under its own prefix.
 */
#include "bs.h"

#include <time.h>

/* ---- JSON, read in place ---- */

static const char *ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

const char *bs_j_find(const char *from, const char *end, const char *key)
{
    char pat[48];
    int n = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof pat || !from || from >= end) return NULL;
    for (const char *p = from; p + n <= end; p++) {
        if (*p != '"' || memcmp(p, pat, (size_t)n)) continue;
        const char *v = ws(p + n, end);
        if (v < end && *v == ':') {
            v = ws(v + 1, end);
            return v < end ? v : NULL;
        }
    }
    return NULL;
}

bool bs_j_str(const char *v, const char *end, char *out, int max)
{
    if (!v || v >= end || *v != '"') return false;
    v++;
    int w = 0;
    while (v < end && *v != '"') {
        char c = *v++;
        if (c == '\\' && v < end) {
            c = *v++;
            if (c == 'u' && v + 4 <= end) {
                /* \uXXXX: the BMP into UTF-8 */
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) {
                    char h = v[i];
                    cp = cp * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                }
                v += 4;
                if (cp < 0x80) { if (w < max - 1) out[w++] = (char)cp; }
                else if (cp < 0x800) { if (w < max - 2) { out[w++] = (char)(0xC0 | (cp >> 6)); out[w++] = (char)(0x80 | (cp & 0x3F)); } }
                else if (w < max - 3) { out[w++] = (char)(0xE0 | (cp >> 12)); out[w++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[w++] = (char)(0x80 | (cp & 0x3F)); }
                continue;
            }
            c = c == 'n' ? ' ' : c == 't' ? ' ' : c;
        }
        if (w < max - 1) out[w++] = c;
    }
    out[w] = 0;
    return v < end;
}

bool bs_j_num(const char *v, const char *end, float *out)
{
    if (!v || v >= end) return false;
    if (*v == '"') v++;
    if (v >= end || !((*v >= '0' && *v <= '9') || *v == '-' || *v == '.')) return false;
    char *e = NULL;
    float f = strtof(v, &e);
    if (e == v) return false;
    *out = f;
    return true;
}

static bool j_i64(const char *v, const char *end, int64_t *out)
{
    if (!v || v >= end) return false;
    if (*v == '"') v++;
    bool neg = false;
    if (v < end && *v == '-') { neg = true; v++; }
    if (v >= end || *v < '0' || *v > '9') return false;
    int64_t x = 0;
    while (v < end && *v >= '0' && *v <= '9') x = x * 10 + (*v++ - '0');
    *out = neg ? -x : x;
    return true;
}

const char *bs_j_obj_end(const char *p, const char *end)
{
    int depth = 0;
    bool str = false;
    for (; p < end; p++) {
        if (str) {
            if (*p == '\\') p++;
            else if (*p == '"') str = false;
            continue;
        }
        if (*p == '"') str = true;
        else if (*p == '{' || *p == '[') depth++;
        else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
    }
    return NULL;
}

const char *bs_j_skip(const char *v, const char *end)
{
    v = ws(v, end);
    if (v >= end) return NULL;
    if (*v == '{' || *v == '[') return bs_j_obj_end(v, end);
    if (*v == '"') {
        for (v++; v < end; v++) {
            if (*v == '\\') v++;
            else if (*v == '"') return v + 1;
        }
        return NULL;
    }
    while (v < end && *v != ',' && *v != '}' && *v != ']') v++;
    return v;
}

/* The next element of an array: p after '[' or after the previous one.
 * Returns its start, or NULL at the end. */
static const char *arr_next(const char *p, const char *end)
{
    p = ws(p, end);
    if (p < end && *p == ',') p = ws(p + 1, end);
    if (p >= end || *p == ']') return NULL;
    return p;
}

/* "2026-11-20" or "2026-11-20 14:35:00": UTC epoch. Daily bars get the hour
 * a session closes (20:00 UTC), so the local date is the bar's date. */
static int32_t parse_dt(const char *s, bool *has_time)
{
    int y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0;
    int k = sscanf(s, "%4d-%2d-%2d%*[ T]%2d:%2d:%2d", &y, &mo, &d, &hh, &mi, &ss);
    if (k < 3 || y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    if (has_time) *has_time = k >= 5;
    if (k < 5) { hh = 20; mi = 0; ss = 0; }
    return bs_days(y, mo, d) * 86400 + hh * 3600 + mi * 60 + ss;
}

/* The next comma-separated token of p into out; returns where the next one
 * starts, or NULL past the end (strtok is not in the symbol table). */
static const char *tok(const char *p, char *out, size_t n)
{
    if (!p || !*p) return NULL;
    size_t w = 0;
    while (*p && *p != ',') {
        if (w + 1 < n) out[w++] = *p;
        p++;
    }
    out[w] = 0;
    return *p == ',' ? p + 1 : p;
}

/* ---- where each provider lives ---- */

static const char *base(bs_prov_t p, char *buf, size_t n)
{
    static const char *REAL[BS_P_COUNT] = {
        "", "https://api.coingecko.com", "https://api.argentinadatos.com", "https://stooq.com",
        "https://api.twelvedata.com", "https://www.alphavantage.co", "https://finnhub.io" };
    static const char *FAKE[BS_P_COUNT] = { "", "coingecko", "argdatos", "stooq", "twelve", "alpha", "finnhub" };
    const char *fake = getenv("BOLSA_FAKE");       /* development switch */
    if (fake && fake[0]) snprintf(buf, n, "%.100s/%s", fake, FAKE[p]);
    else snprintf(buf, n, "%s", REAL[p]);
    return buf;
}

static const char *key(bs_prov_t p) { return bs_cfg()->key[p]; }

bool bs_prov_can(bs_prov_t p, int res)
{
    switch (p) {
    case BS_P_COINGECKO: return res != BS_R_FULL;     /* a year is the keyless limit */
    case BS_P_ARGDATOS:  return res == BS_R_FULL;     /* it is all or nothing */
    case BS_P_STOOQ:     return res == BS_R_YEAR || res == BS_R_FULL;
    case BS_P_TWELVE:    return true;
    case BS_P_ALPHA:     return res == BS_R_YEAR || res == BS_R_FULL;
    default:             return false;
    }
}

bool bs_prov_has_ohlc(bs_prov_t p, int res)
{
    (void)res;
    return p == BS_P_STOOQ || p == BS_P_TWELVE || p == BS_P_ALPHA;
}

/* The provider's own name for a symbol */
static void remote_sym(bs_prov_t p, const char *sym, char *out, size_t n)
{
    static const struct { const char *sym, *stooq, *twelve; } IDX[] = {
        { "^GSPC", "^spx", "SPX" }, { "^DJI", "^dji", "DJI" }, { "^IXIC", "^ndq", "IXIC" },
        { "^MERV", "^mrv", "IMV" }, { "^BVSP", "^bvp", "IBOV" }, { "^FTSE", "^ukx", "UKX" },
        { "^GDAXI", "^dax", "DAX" }, { "^N225", "^nkx", "N225" },
    };
    for (size_t i = 0; i < sizeof IDX / sizeof IDX[0]; i++) {
        if (strcmp(IDX[i].sym, sym)) continue;
        bs_scpy(out, n, p == BS_P_STOOQ ? IDX[i].stooq : p == BS_P_TWELVE ? IDX[i].twelve : sym);
        return;
    }
    size_t L = strlen(sym);
    bool ba = L > 3 && !strcmp(sym + L - 3, ".BA");
    if (p == BS_P_STOOQ) {
        /* aapl.us; Buenos Aires as .ar (not checked: the live CSV wants a key) */
        char low[BS_SYM_LEN];
        size_t w = 0;
        for (size_t i = 0; i < (ba ? L - 3 : L) && w + 1 < sizeof low; i++)
            low[w++] = (sym[i] >= 'A' && sym[i] <= 'Z') ? (char)(sym[i] + 32) : sym[i];
        low[w] = 0;
        snprintf(out, n, "%s.%s", low, ba ? "ar" : "us");
    } else if (p == BS_P_TWELVE && ba) {
        snprintf(out, n, "%.*s:BCBA", (int)(L - 3), sym);
    } else {
        bs_scpy(out, n, sym);
    }
}

/* URL-safe: the symbols are [A-Z0-9.^:-], and ^ must be escaped */
static void url_sym(const char *in, char *out, size_t n)
{
    size_t w = 0;
    for (const char *p = in; *p && w + 4 < n; p++) {
        if (*p == '^') { out[w++] = '%'; out[w++] = '5'; out[w++] = 'E'; }
        else out[w++] = *p;
    }
    out[w] = 0;
}

static void ymd(int32_t t, char *out, size_t n)
{
    int y, m, d;
    bs_civil(t / 86400, &y, &m, &d);
    snprintf(out, n, "%04d%02d%02d", y, m, d);
}

bool bs_prov_url(const bs_job_t *j, char *url, size_t n, int *max_bytes)
{
    char b[128], rs[48], us[64];
    bs_prov_t p = (bs_prov_t)j->prov;
    base(p, b, sizeof b);
    url[0] = 0;
    *max_bytes = 64 * 1024;

    if (j->kind == BS_J_SEARCH) {
        char q[96];
        size_t w = 0;
        for (const char *s = j->query; *s && w + 4 < sizeof q; s++) {
            unsigned char c = (unsigned char)*s;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')
                q[w++] = (char)c;
            else { snprintf(q + w, sizeof q - w, "%%%02X", c); w += 3; }
        }
        q[w] = 0;
        switch (p) {
        case BS_P_COINGECKO: snprintf(url, n, "%s/api/v3/search?query=%s", b, q); break;
        case BS_P_TWELVE: snprintf(url, n, "%s/symbol_search?symbol=%s&outputsize=12&apikey=%s", b, q, key(p)); break;
        case BS_P_FINNHUB: snprintf(url, n, "%s/api/v1/search?q=%s&token=%s", b, q, key(p)); break;
        case BS_P_ALPHA: snprintf(url, n, "%s/query?function=SYMBOL_SEARCH&keywords=%s&apikey=%s", b, q, key(p)); break;
        default: return false;
        }
        return true;
    }

    if (j->kind == BS_J_QUOTES) {
        if (p == BS_P_COINGECKO) {
            char ids[256];
            size_t w = 0;
            ids[0] = 0;
            char s[BS_SYM_LEN];
            for (const char *it = tok(j->syms, s, sizeof s); it; it = tok(it, s, sizeof s)) {
                bs_info_t in;
                bs_sym_info(s, &in);
                if (in.kind != BS_K_CRYPTO || !in.remote) continue;
                w += (size_t)snprintf(ids + w, sizeof ids - w, "%s%s", w ? "," : "", in.remote);
                if (w >= sizeof ids) return false;
            }
            snprintf(url, n, "%s/api/v3/simple/price?ids=%s&vs_currencies=usd&include_24hr_change=true"
                     "&include_24hr_vol=true&include_last_updated_at=true", b, ids);
            return true;
        }
        if (p == BS_P_TWELVE) {
            char list[256];
            size_t w = 0;
            list[0] = 0;
            char s[BS_SYM_LEN];
            for (const char *it = tok(j->syms, s, sizeof s); it; it = tok(it, s, sizeof s)) {
                remote_sym(p, s, rs, sizeof rs);
                url_sym(rs, us, sizeof us);
                w += (size_t)snprintf(list + w, sizeof list - w, "%s%s", w ? "," : "", us);
                if (w >= sizeof list) return false;
            }
            snprintf(url, n, "%s/quote?symbol=%s&apikey=%s", b, list, key(p));
            return true;
        }
        remote_sym(p, j->sym, rs, sizeof rs);
        url_sym(rs, us, sizeof us);
        if (p == BS_P_FINNHUB) { snprintf(url, n, "%s/api/v1/quote?symbol=%s&token=%s", b, us, key(p)); return true; }
        if (p == BS_P_ALPHA) { snprintf(url, n, "%s/query?function=GLOBAL_QUOTE&symbol=%s&apikey=%s", b, us, key(p)); return true; }
        if (p == BS_P_ARGDATOS) {
            bs_info_t in;
            bs_sym_info(j->sym, &in);
            if (in.kind != BS_K_RISK) return false;
            snprintf(url, n, "%s/v1/finanzas/indices/riesgo-pais/ultimo", b);
            return true;
        }
        return false;
    }

    /* a series */
    bs_info_t in;
    bs_sym_info(j->sym, &in);
    int res = j->res;
    if (!bs_prov_can(p, res)) return false;
    switch (p) {
    case BS_P_COINGECKO:
        if (!in.remote) return false;
        snprintf(url, n, "%s/api/v3/coins/%s/market_chart?vs_currency=usd&days=%s", b, in.remote,
                 res == BS_R_DAY ? "1" : res == BS_R_WEEK ? "5" : "365");
        *max_bytes = res == BS_R_YEAR ? 96 * 1024 : 128 * 1024;
        return true;
    case BS_P_ARGDATOS:
        if (in.kind == BS_K_DOLAR) snprintf(url, n, "%s/v1/cotizaciones/dolares/%s", b, in.remote);
        else if (in.kind == BS_K_FX) snprintf(url, n, "%s/v1/cotizaciones/%s", b, in.remote);
        else if (in.kind == BS_K_RISK) snprintf(url, n, "%s/v1/finanzas/indices/riesgo-pais", b);
        else return false;
        *max_bytes = 1536 * 1024;
        return true;
    case BS_P_STOOQ: {
        remote_sym(p, j->sym, rs, sizeof rs);
        url_sym(rs, us, sizeof us);
        if (res == BS_R_YEAR) {
            char d1[16];
            int32_t now = bs_now();
            ymd((now ? now : 1790000000) - 400 * 86400, d1, sizeof d1);
            snprintf(url, n, "%s/q/d/l/?s=%s&i=d&d1=%s&apikey=%s", b, us, d1, key(p));
            *max_bytes = 48 * 1024;
        } else {
            snprintf(url, n, "%s/q/d/l/?s=%s&i=d&apikey=%s", b, us, key(p));
            *max_bytes = 1024 * 1024;
        }
        return true;
    }
    case BS_P_TWELVE: {
        remote_sym(p, j->sym, rs, sizeof rs);
        url_sym(rs, us, sizeof us);
        static const char *IV[BS_R_COUNT] = { "5min", "30min", "1day", "1day" };
        static const int SZ[BS_R_COUNT] = { 96, 70, 270, 5000 };
        snprintf(url, n, "%s/time_series?symbol=%s&interval=%s&outputsize=%d&timezone=UTC&apikey=%s",
                 b, us, IV[res], SZ[res], key(p));
        *max_bytes = res == BS_R_FULL ? 900 * 1024 : 64 * 1024;
        return true;
    }
    case BS_P_ALPHA:
        remote_sym(p, j->sym, rs, sizeof rs);
        url_sym(rs, us, sizeof us);
        snprintf(url, n, "%s/query?function=%s&symbol=%s&apikey=%s", b,
                 res == BS_R_YEAR ? "TIME_SERIES_DAILY" : "TIME_SERIES_WEEKLY", us, key(p));
        *max_bytes = res == BS_R_FULL ? 512 * 1024 : 48 * 1024;
        return true;
    default:
        return false;
    }
}

/* ---- errors in the body ---- */

static int twelve_err(const char *body, const char *end)
{
    const char *st = bs_j_find(body, end, "status");
    char s[16];
    if (!st || !bs_j_str(st, end, s, sizeof s) || strcmp(s, "error")) return 0;
    float code = 0;
    bs_j_num(bs_j_find(body, end, "code"), end, &code);
    int c = (int)code;
    return c == 401 ? BS_E_KEY : c == 429 ? BS_E_LIMIT : c == 404 || c == 400 ? BS_E_NOSYM : c == 403 ? BS_E_PLAN : BS_E_HTTP;
}

static int alpha_err(const char *body, const char *end)
{
    char m[200];
    const char *v;
    if ((v = bs_j_find(body, end, "Error Message"))) {
        bs_j_str(v, end, m, sizeof m);
        return strstr(m, "apikey") ? BS_E_KEY : BS_E_NOSYM;
    }
    if ((v = bs_j_find(body, end, "Information")) || (v = bs_j_find(body, end, "Note"))) {
        bs_j_str(v, end, m, sizeof m);
        return strstr(m, "premium") && !strstr(m, "rate limit") ? BS_E_PLAN : BS_E_LIMIT;
    }
    return 0;
}

/* ---- series ---- */

static int parse_cg_chart(const char *body, const char *end, bs_series_t *out)
{
    const char *pr = bs_j_find(body, end, "prices");
    if (!pr || *pr != '[') {
        return bs_j_find(body, end, "error_code") || bs_j_find(body, end, "status") ? BS_E_LIMIT : BS_E_PARSE;
    }
    const char *pe = bs_j_obj_end(pr, end);
    const char *vo = bs_j_find(body, end, "total_volumes");
    const char *ve = vo && *vo == '[' ? bs_j_obj_end(vo, end) : NULL;
    const char *vp = ve ? vo + 1 : NULL;
    for (const char *e = arr_next(pr + 1, pe); e; e = arr_next(e, pe)) {
        if (*e != '[') return BS_E_PARSE;
        const char *ee = bs_j_obj_end(e, pe);
        int64_t ms = 0;
        float price = 0, vol = 0;
        const char *x = ws(e + 1, ee);
        if (!j_i64(x, ee, &ms)) return BS_E_PARSE;
        const char *comma = strchr(x, ',');
        if (!comma || comma > ee || !bs_j_num(ws(comma + 1, ee), ee, &price)) { e = ee; continue; }
        if (vp) {
            const char *vel = arr_next(vp, ve);
            if (vel && *vel == '[') {
                const char *vee = bs_j_obj_end(vel, ve);
                const char *vc = strchr(vel, ',');
                if (vc && vc < vee) bs_j_num(ws(vc + 1, vee), vee, &vol);
                vp = vee;
            } else {
                vp = NULL;
            }
        }
        bs_pt_t pt = { (int32_t)(ms / 1000), price, price, price, price, vol };
        if (!bs_series_push(out, &pt)) return BS_E_PARSE;
        e = ee;
    }
    out->ohlc = false;
    out->vol = false;   /* CoinGecko's are a rolling 24 h, not a bar's */
    return out->n ? 0 : BS_E_EMPTY;
}

static int parse_ad(const char *body, const char *end, bs_series_t *out)
{
    const char *a = ws(body, end);
    if (a >= end || *a != '[') return BS_E_PARSE;
    const char *ae = bs_j_obj_end(a, end);
    if (!ae) return BS_E_PARSE;
    for (const char *e = arr_next(a + 1, ae); e; e = arr_next(e, ae)) {
        if (*e != '{') return BS_E_PARSE;
        const char *oe = bs_j_obj_end(e, ae);
        char f[24];
        float v = NAN;
        if (bs_j_str(bs_j_find(e, oe, "fecha"), oe, f, sizeof f)) {
            if (!bs_j_num(bs_j_find(e, oe, "venta"), oe, &v) &&
                !bs_j_num(bs_j_find(e, oe, "valor"), oe, &v))
                bs_j_num(bs_j_find(e, oe, "compra"), oe, &v);
            int32_t t = parse_dt(f, NULL);
            if (t && !isnan(v)) {
                /* noon in Buenos Aires: the local date is the day's */
                t = t - 20 * 3600 + 15 * 3600;
                bs_pt_t pt = { t, v, v, v, v, 0 };
                if (!bs_series_push(out, &pt)) return BS_E_PARSE;
            }
        }
        e = oe;
    }
    out->ohlc = out->vol = false;
    return out->n ? 0 : BS_E_EMPTY;
}

static int parse_stooq(const char *body, const char *end, bs_series_t *out)
{
    if (strstr(body, "apikey") || strstr(body, "Access denied")) return BS_E_KEY;
    if (strstr(body, "Exceeded")) return BS_E_LIMIT;
    if (!strncmp(body, "No data", 7)) return BS_E_NOSYM;
    if (strncmp(body, "Date,", 5)) return BS_E_PARSE;
    const char *p = strchr(body, '\n');
    bool vol = false;
    while (p && p < end) {
        p++;
        if (p >= end) break;
        char line[128];
        int w = 0;
        while (p < end && *p != '\n' && w < (int)sizeof line - 1) line[w++] = *p++;
        line[w] = 0;
        if (w < 10) continue;
        int32_t t = parse_dt(line, NULL);
        float o, h, l, c, v = 0;
        char *s = strchr(line, ',');
        if (!t || !s) continue;
        char *e;
        o = strtof(s + 1, &e); if (*e != ',') continue;
        h = strtof(e + 1, &e); if (*e != ',') continue;
        l = strtof(e + 1, &e); if (*e != ',') continue;
        c = strtof(e + 1, &e);
        if (*e == ',') { v = strtof(e + 1, &e); vol |= v > 0; }
        bs_pt_t pt = { t, o, h, l, c, v };
        if (!bs_series_push(out, &pt)) return BS_E_PARSE;
    }
    out->ohlc = true;
    out->vol = vol;
    return out->n ? 0 : BS_E_EMPTY;
}

static int parse_twelve_series(const char *body, const char *end, bs_series_t *out)
{
    int err = twelve_err(body, end);
    if (err) return err;
    const char *vals = bs_j_find(body, end, "values");
    if (!vals || *vals != '[') return BS_E_PARSE;
    const char *ve = bs_j_obj_end(vals, end);
    bool vol = false;
    for (const char *e = arr_next(vals + 1, ve); e; e = arr_next(e, ve)) {
        if (*e != '{') return BS_E_PARSE;
        const char *oe = bs_j_obj_end(e, ve);
        char dt[32];
        bs_pt_t pt = { 0, NAN, NAN, NAN, NAN, 0 };
        if (bs_j_str(bs_j_find(e, oe, "datetime"), oe, dt, sizeof dt)) pt.t = parse_dt(dt, NULL);
        bs_j_num(bs_j_find(e, oe, "open"), oe, &pt.o);
        bs_j_num(bs_j_find(e, oe, "high"), oe, &pt.h);
        bs_j_num(bs_j_find(e, oe, "low"), oe, &pt.l);
        bs_j_num(bs_j_find(e, oe, "close"), oe, &pt.c);
        if (bs_j_num(bs_j_find(e, oe, "volume"), oe, &pt.v)) vol |= pt.v > 0;
        if (pt.t && !isnan(pt.c)) {
            if (isnan(pt.o)) pt.o = pt.c;
            if (isnan(pt.h)) pt.h = pt.c;
            if (isnan(pt.l)) pt.l = pt.c;
            if (!bs_series_push(out, &pt)) return BS_E_PARSE;
        }
        e = oe;
    }
    out->ohlc = true;
    out->vol = vol;
    return out->n ? 0 : BS_E_EMPTY;
}

static int parse_alpha_series(const char *body, const char *end, bs_series_t *out)
{
    int err = alpha_err(body, end);
    if (err) return err;
    const char *ts = bs_j_find(body, end, "Time Series (Daily)");
    if (!ts) ts = bs_j_find(body, end, "Weekly Time Series");
    if (!ts || *ts != '{') return BS_E_PARSE;
    const char *te = bs_j_obj_end(ts, end);
    bool vol = false;
    for (const char *p = ts + 1; p && p < te;) {
        p = ws(p, te);
        if (p < te && *p == ',') p = ws(p + 1, te);
        if (p >= te || *p != '"') break;
        char d[16];
        if (!bs_j_str(p, te, d, sizeof d)) break;
        const char *colon = strchr(p + 1, ':');
        if (!colon) break;
        const char *o = ws(colon + 1, te);
        if (*o != '{') break;
        const char *oe = bs_j_obj_end(o, te);
        bs_pt_t pt = { parse_dt(d, NULL), NAN, NAN, NAN, NAN, 0 };
        bs_j_num(bs_j_find(o, oe, "1. open"), oe, &pt.o);
        bs_j_num(bs_j_find(o, oe, "2. high"), oe, &pt.h);
        bs_j_num(bs_j_find(o, oe, "3. low"), oe, &pt.l);
        bs_j_num(bs_j_find(o, oe, "4. close"), oe, &pt.c);
        if (bs_j_num(bs_j_find(o, oe, "5. volume"), oe, &pt.v)) vol |= pt.v > 0;
        if (pt.t && !isnan(pt.c) && !isnan(pt.o) && !isnan(pt.h) && !isnan(pt.l))
            if (!bs_series_push(out, &pt)) return BS_E_PARSE;
        p = oe;
    }
    out->ohlc = true;
    out->vol = vol;
    return out->n ? 0 : BS_E_EMPTY;
}

int bs_prov_parse_series(const bs_job_t *j, const char *body, int len, bs_series_t *out)
{
    const char *end = body + len;
    out->n = 0;
    int r;
    switch (j->prov) {
    case BS_P_COINGECKO: r = parse_cg_chart(body, end, out); break;
    case BS_P_ARGDATOS:  r = parse_ad(body, end, out); break;
    case BS_P_STOOQ:     r = parse_stooq(body, end, out); break;
    case BS_P_TWELVE:    r = parse_twelve_series(body, end, out); break;
    case BS_P_ALPHA:     r = parse_alpha_series(body, end, out); break;
    default:             r = BS_E_PARSE; break;
    }
    if (r) return r;
    bs_series_sort(out);
    out->prov = j->prov;
    return 0;
}

/* ---- quotes ---- */

static int parse_cg_quotes(const bs_job_t *j, const char *body, const char *end)
{
    if (bs_j_find(body, end, "error_code")) return BS_E_LIMIT;
    if (*ws(body, end) != '{') return BS_E_PARSE;
    int got = 0;
    char s[BS_SYM_LEN];
    for (const char *it = tok(j->syms, s, sizeof s); it; it = tok(it, s, sizeof s)) {
        bs_info_t in;
        bs_sym_info(s, &in);
        if (!in.remote) continue;
        const char *o = bs_j_find(body, end, in.remote);
        if (!o || *o != '{') continue;
        const char *oe = bs_j_obj_end(o, end);
        bs_quote_t q;
        memset(&q, 0, sizeof q);
        if (!bs_j_num(bs_j_find(o, oe, "usd"), oe, &q.price)) continue;
        float chg;
        if (bs_j_num(bs_j_find(o, oe, "usd_24h_change"), oe, &chg) && chg > -100) {
            q.prev = q.price / (1.0f + chg / 100.0f);
            q.has |= BS_Q_PREV;
        }
        if (bs_j_num(bs_j_find(o, oe, "usd_24h_vol"), oe, &q.vol)) q.has |= BS_Q_VOL;
        int64_t up;
        if (j_i64(bs_j_find(o, oe, "last_updated_at"), oe, &up)) q.t = (int32_t)up;
        q.ok = true;
        q.prov = BS_P_COINGECKO;
        bs_item_quote_set(s, &q);
        got++;
    }
    return got ? 0 : BS_E_EMPTY;
}

static bool twelve_one(const char *sym, const char *o, const char *oe)
{
    if (twelve_err(o, oe)) return false;
    /* the 52 weeks are an object of their own with its own "low"/"high":
     * the top-level ones are looked for before it */
    const char *w52 = bs_j_find(o, oe, "fifty_two_week");
    const char *top = w52 ? w52 : oe;
    bs_quote_t q;
    memset(&q, 0, sizeof q);
    if (!bs_j_num(bs_j_find(o, top, "close"), top, &q.price)) return false;
    if (bs_j_num(bs_j_find(o, top, "previous_close"), top, &q.prev)) q.has |= BS_Q_PREV;
    if (bs_j_num(bs_j_find(o, top, "open"), top, &q.open) &&
        bs_j_num(bs_j_find(o, top, "high"), top, &q.high) &&
        bs_j_num(bs_j_find(o, top, "low"), top, &q.low)) q.has |= BS_Q_OHL;
    if (bs_j_num(bs_j_find(o, top, "volume"), top, &q.vol)) q.has |= BS_Q_VOL;
    if (w52 && *w52 == '{') {
        const char *we = bs_j_obj_end(w52, oe);
        if (bs_j_num(bs_j_find(w52, we, "low"), we, &q.lo52) && bs_j_num(bs_j_find(w52, we, "high"), we, &q.hi52))
            q.has |= BS_Q_52W;
    }
    int64_t ts;
    if (j_i64(bs_j_find(o, oe, "timestamp"), oe, &ts)) q.t = (int32_t)ts;
    char nm[BS_NAME_LEN];
    if (bs_j_str(bs_j_find(o, oe, "name"), oe, nm, sizeof nm) && nm[0] && !bs_names_get(sym))
        bs_names_put(sym, nm);
    q.ok = true;
    q.prov = BS_P_TWELVE;
    bs_item_quote_set(sym, &q);
    return true;
}

static int parse_twelve_quotes(const bs_job_t *j, const char *body, const char *end)
{
    int n = 0, got = 0, err = 0;
    for (const char *c = j->syms; *c; c++) n += *c == ',';
    if (n == 0) {
        /* one symbol: the object itself */
        err = twelve_err(body, end);
        if (err) return err;
        return twelve_one(j->syms, body, end) ? 0 : BS_E_PARSE;
    }
    char s[BS_SYM_LEN];
    for (const char *it = tok(j->syms, s, sizeof s); it; it = tok(it, s, sizeof s)) {
        char rs[48];
        remote_sym(BS_P_TWELVE, s, rs, sizeof rs);
        const char *o = bs_j_find(body, end, rs);
        if (!o || *o != '{') continue;
        const char *oe = bs_j_obj_end(o, end);
        int e = twelve_err(o, oe);
        if (e) { err = e; continue; }
        got += twelve_one(s, o, oe);
    }
    if (!got) {
        int e = twelve_err(body, end);
        return e ? e : err ? err : BS_E_PARSE;
    }
    return 0;
}

static int parse_finnhub_quote(const bs_job_t *j, const char *body, const char *end)
{
    if (bs_j_find(body, end, "error")) {
        char m[96];
        bs_j_str(bs_j_find(body, end, "error"), end, m, sizeof m);
        return strstr(m, "limit") ? BS_E_LIMIT : strstr(m, "access") ? BS_E_PLAN : BS_E_KEY;
    }
    bs_quote_t q;
    memset(&q, 0, sizeof q);
    if (!bs_j_num(bs_j_find(body, end, "c"), end, &q.price)) return BS_E_PARSE;
    if (q.price == 0) return BS_E_NOSYM;     /* its answer for a symbol it does not know */
    if (bs_j_num(bs_j_find(body, end, "pc"), end, &q.prev) && q.prev > 0) q.has |= BS_Q_PREV;
    if (bs_j_num(bs_j_find(body, end, "o"), end, &q.open) && bs_j_num(bs_j_find(body, end, "h"), end, &q.high) &&
        bs_j_num(bs_j_find(body, end, "l"), end, &q.low) && q.high > 0) q.has |= BS_Q_OHL;
    int64_t t;
    if (j_i64(bs_j_find(body, end, "t"), end, &t)) q.t = (int32_t)t;
    q.ok = true;
    q.prov = BS_P_FINNHUB;
    bs_item_quote_set(j->sym, &q);
    return 0;
}

static int parse_alpha_quote(const bs_job_t *j, const char *body, const char *end)
{
    int err = alpha_err(body, end);
    if (err) return err;
    const char *g = bs_j_find(body, end, "Global Quote");
    if (!g || *g != '{') return BS_E_PARSE;
    const char *ge = bs_j_obj_end(g, end);
    bs_quote_t q;
    memset(&q, 0, sizeof q);
    if (!bs_j_num(bs_j_find(g, ge, "05. price"), ge, &q.price)) return BS_E_NOSYM;
    if (bs_j_num(bs_j_find(g, ge, "08. previous close"), ge, &q.prev)) q.has |= BS_Q_PREV;
    if (bs_j_num(bs_j_find(g, ge, "02. open"), ge, &q.open) && bs_j_num(bs_j_find(g, ge, "03. high"), ge, &q.high) &&
        bs_j_num(bs_j_find(g, ge, "04. low"), ge, &q.low)) q.has |= BS_Q_OHL;
    if (bs_j_num(bs_j_find(g, ge, "06. volume"), ge, &q.vol)) q.has |= BS_Q_VOL;
    char d[16];
    if (bs_j_str(bs_j_find(g, ge, "07. latest trading day"), ge, d, sizeof d)) q.t = parse_dt(d, NULL);
    q.ok = true;
    q.prov = BS_P_ALPHA;
    bs_item_quote_set(j->sym, &q);
    return 0;
}

static int parse_ad_quote(const bs_job_t *j, const char *body, const char *end)
{
    float v;
    char f[24];
    if (!bs_j_num(bs_j_find(body, end, "valor"), end, &v)) return BS_E_PARSE;
    bs_item_t *it = bs_item_find(j->sym);
    bs_quote_t q;
    memset(&q, 0, sizeof q);
    /* the latest value only: the day before comes from the series */
    if (it && it->s[BS_R_FULL].n >= 2) {
        bs_quote_from_series(&it->s[BS_R_FULL], &q, false);
        const bs_pt_t *last = &it->s[BS_R_FULL].p[it->s[BS_R_FULL].n - 1];
        int32_t t = bs_j_str(bs_j_find(body, end, "fecha"), end, f, sizeof f) ? parse_dt(f, NULL) - 5 * 3600 : 0;
        if (t > last->t + 3600) q.prev = last->c;   /* a new day after the series */
    }
    q.price = v;
    q.ok = true;
    q.prov = BS_P_ARGDATOS;
    if (bs_j_str(bs_j_find(body, end, "fecha"), end, f, sizeof f)) q.t = parse_dt(f, NULL) - 5 * 3600;
    bs_item_quote_set(j->sym, &q);
    return 0;
}

int bs_prov_parse_quotes(const bs_job_t *j, const char *body, int len)
{
    const char *end = body + len;
    switch (j->prov) {
    case BS_P_COINGECKO: return parse_cg_quotes(j, body, end);
    case BS_P_TWELVE:    return parse_twelve_quotes(j, body, end);
    case BS_P_FINNHUB:   return parse_finnhub_quote(j, body, end);
    case BS_P_ALPHA:     return parse_alpha_quote(j, body, end);
    case BS_P_ARGDATOS:  return parse_ad_quote(j, body, end);
    default:             return BS_E_PARSE;
    }
}

/* ---- search ---- */

static void hit(bs_hit_t *out, int max, int *n, const char *sym, const char *name, const char *where)
{
    if (*n >= max || !sym[0]) return;
    for (int i = 0; i < *n; i++)
        if (!strcmp(out[i].sym, sym)) return;
    bs_scpy(out[*n].sym, sizeof out[0].sym, sym);
    bs_scpy(out[*n].name, sizeof out[0].name, name);
    bs_scpy(out[*n].where, sizeof out[0].where, where);
    (*n)++;
}

/* A provider's symbol and exchange as one of ours: Buenos Aires is ".BA",
 * the United States as is; anything else is left out (the app does not know
 * how each provider spells the other markets). */
static bool canon(const char *sym, const char *exch, char *out, size_t n)
{
    char s[BS_SYM_LEN];
    if (!bs_sym_clean(sym, s, sizeof s)) return false;
    bool ba = exch && (!strcmp(exch, "BCBA") || !strcmp(exch, "Argentina") || !strcmp(exch, "XBUE"));
    size_t L = strlen(s);
    if (L > 3 && !strcmp(s + L - 3, ".BA")) ba = true, s[L - 3] = 0;
    bool us = !exch || !exch[0] || !strcmp(exch, "NYSE") || !strcmp(exch, "NASDAQ") || !strcmp(exch, "United States") ||
              !strcmp(exch, "AMEX") || !strcmp(exch, "ARCA") || !strcmp(exch, "BATS");
    if (strchr(s, '.') && !ba) return false;
    if (ba) snprintf(out, n, "%s.BA", s);
    else if (us) bs_scpy(out, n, s);
    else return false;
    return true;
}

int bs_prov_parse_search(const bs_job_t *j, const char *body, int len, bs_hit_t *out, int max, int *n)
{
    const char *end = body + len;
    *n = 0;
    char sym[40], name[BS_NAME_LEN], ex[32], c[BS_SYM_LEN];
    if (j->prov == BS_P_COINGECKO) {
        const char *a = bs_j_find(body, end, "coins");
        if (!a || *a != '[') return bs_j_find(body, end, "error_code") ? BS_E_LIMIT : BS_E_PARSE;
        const char *ae = bs_j_obj_end(a, end);
        for (const char *e = arr_next(a + 1, ae); e && *e == '{'; e = arr_next(e, ae)) {
            const char *oe = bs_j_obj_end(e, ae);
            char id[40];
            if (bs_j_str(bs_j_find(e, oe, "id"), oe, id, sizeof id) &&
                bs_j_str(bs_j_find(e, oe, "symbol"), oe, sym, sizeof sym) &&
                bs_j_str(bs_j_find(e, oe, "name"), oe, name, sizeof name)) {
                char cs[BS_SYM_LEN];
                snprintf(cs, sizeof cs, "cg:%.20s", id);
                for (char *p = sym; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
                hit(out, max, n, cs, name, sym);
            }
            e = oe;
        }
        return 0;
    }
    if (j->prov == BS_P_TWELVE) {
        int err = twelve_err(body, end);
        if (err) return err;
        const char *a = bs_j_find(body, end, "data");
        if (!a || *a != '[') return BS_E_PARSE;
        const char *ae = bs_j_obj_end(a, end);
        for (const char *e = arr_next(a + 1, ae); e && *e == '{'; e = arr_next(e, ae)) {
            const char *oe = bs_j_obj_end(e, ae);
            if (bs_j_str(bs_j_find(e, oe, "symbol"), oe, sym, sizeof sym) &&
                bs_j_str(bs_j_find(e, oe, "instrument_name"), oe, name, sizeof name) &&
                bs_j_str(bs_j_find(e, oe, "exchange"), oe, ex, sizeof ex) && canon(sym, ex, c, sizeof c))
                hit(out, max, n, c, name, ex);
            e = oe;
        }
        return 0;
    }
    if (j->prov == BS_P_FINNHUB) {
        if (bs_j_find(body, end, "error")) return BS_E_KEY;
        const char *a = bs_j_find(body, end, "result");
        if (!a || *a != '[') return BS_E_PARSE;
        const char *ae = bs_j_obj_end(a, end);
        for (const char *e = arr_next(a + 1, ae); e && *e == '{'; e = arr_next(e, ae)) {
            const char *oe = bs_j_obj_end(e, ae);
            if (bs_j_str(bs_j_find(e, oe, "symbol"), oe, sym, sizeof sym) &&
                bs_j_str(bs_j_find(e, oe, "description"), oe, name, sizeof name) && canon(sym, NULL, c, sizeof c))
                hit(out, max, n, c, name, strstr(c, ".BA") ? "BCBA" : "");
            e = oe;
        }
        return 0;
    }
    if (j->prov == BS_P_ALPHA) {
        int err = alpha_err(body, end);
        if (err) return err;
        const char *a = bs_j_find(body, end, "bestMatches");
        if (!a || *a != '[') return BS_E_PARSE;
        const char *ae = bs_j_obj_end(a, end);
        for (const char *e = arr_next(a + 1, ae); e && *e == '{'; e = arr_next(e, ae)) {
            const char *oe = bs_j_obj_end(e, ae);
            if (bs_j_str(bs_j_find(e, oe, "1. symbol"), oe, sym, sizeof sym) &&
                bs_j_str(bs_j_find(e, oe, "2. name"), oe, name, sizeof name) &&
                bs_j_str(bs_j_find(e, oe, "4. region"), oe, ex, sizeof ex) && canon(sym, ex, c, sizeof c))
                hit(out, max, n, c, name, ex);
            e = oe;
        }
        return 0;
    }
    return BS_E_PARSE;
}
