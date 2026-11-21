/*
 * P4OS - Bolsa: price series, the cache, the indicators and the text.
 *
 * A series is an array of points (time, open, high, low, close, volume)
 * in PSRAM, sorted by time. The cache keeps each symbol's series at each
 * resolution in <data>/bolsa/c/, so the app opens with the last thing it
 * saw and works with no network.
 *
 * Every number here is a float: the P4's FPU is single precision, and a
 * double means the slow software library. The epoch fits in 32 bits until
 * 2038 and is kept that way (a 64-bit integer to float is not in the
 * symbol table).
 */
#include "bs.h"

#include <time.h>
#include <sys/stat.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

static void *ps_realloc(void *p, size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    if (q) return q;
#endif
    return realloc(p, n);
}

bool bs_series_reserve(bs_series_t *s, int n)
{
    if (n <= s->cap) return true;
    int cap = s->cap ? s->cap : 64;
    while (cap < n) cap *= 2;
    bs_pt_t *p = ps_realloc(s->p, sizeof *p * (size_t)cap);
    if (!p) return false;
    s->p = p;
    s->cap = cap;
    return true;
}

void bs_series_free(bs_series_t *s)
{
    free(s->p);
    memset(s, 0, sizeof *s);
}

bool bs_series_push(bs_series_t *s, const bs_pt_t *pt)
{
    if (!bs_series_reserve(s, s->n + 1)) return false;
    s->p[s->n++] = *pt;
    return true;
}

static int cmp_t(const void *a, const void *b)
{
    int32_t x = ((const bs_pt_t *)a)->t, y = ((const bs_pt_t *)b)->t;
    return x < y ? -1 : x > y;
}

void bs_series_sort(bs_series_t *s)
{
    if (s->n < 2) return;
    /* most providers send newest first or oldest first: reversing is
     * enough, and qsort only for the odd one */
    bool asc = true, desc = true;
    for (int i = 1; i < s->n && (asc || desc); i++) {
        if (s->p[i].t < s->p[i - 1].t) asc = false;
        if (s->p[i].t > s->p[i - 1].t) desc = false;
    }
    if (desc && !asc) {
        for (int i = 0, j = s->n - 1; i < j; i++, j--) {
            bs_pt_t t = s->p[i];
            s->p[i] = s->p[j];
            s->p[j] = t;
        }
    } else if (!asc) {
        qsort(s->p, (size_t)s->n, sizeof *s->p, cmp_t);
    }
    int w = 1;
    for (int i = 1; i < s->n; i++) {
        if (s->p[i].t == s->p[w - 1].t) s->p[w - 1] = s->p[i];
        else s->p[w++] = s->p[i];
    }
    s->n = w;
}

void bs_series_copy(bs_series_t *dst, const bs_series_t *src)
{
    bs_pt_t *keep = dst->p;
    int cap = dst->cap;
    *dst = *src;
    dst->p = keep;
    dst->cap = cap;
    dst->n = 0;
    if (src->n && bs_series_reserve(dst, src->n)) {
        memcpy(dst->p, src->p, sizeof *src->p * (size_t)src->n);
        dst->n = src->n;
    }
}

int bs_series_find(const bs_series_t *s, int32_t t)
{
    int lo = 0, hi = s->n;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (s->p[m].t < t) lo = m + 1;
        else hi = m;
    }
    return lo;
}

/* ---- the cache ---- */

#define CACHE_MAGIC 0x31435342u     /* "BSC1" */

typedef struct {
    uint32_t magic;
    int32_t  n, fetched;
    uint8_t  ohlc, vol, prov, res;
} cache_hdr_t;

static void cache_path(const char *sym, int res, char *out, size_t n)
{
    /* the symbol as a file name: ^ and : are not friendly to FAT */
    char f[BS_SYM_LEN];
    size_t w = 0;
    for (const char *p = sym; *p && w + 1 < sizeof f; p++)
        f[w++] = (*p == '^') ? '_' : (*p == ':') ? '-' : *p;
    f[w] = 0;
    snprintf(out, n, "%.150s/c/%s.%d", bs_data_dir(), f, res);
}

bool bs_cache_save(const char *sym, int res, const bs_series_t *s)
{
    char p[200], tmp[210];
    cache_path(sym, res, p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    cache_hdr_t h = { CACHE_MAGIC, s->n, s->fetched, s->ohlc, s->vol, s->prov, (uint8_t)res };
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 &&
              (s->n == 0 || fwrite(s->p, sizeof *s->p, (size_t)s->n, f) == (size_t)s->n);
    fclose(f);
    if (!ok) { remove(tmp); return false; }
    remove(p);
    return rename(tmp, p) == 0;
}

bool bs_cache_load(const char *sym, int res, bs_series_t *s)
{
    char p[200];
    cache_path(sym, res, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    cache_hdr_t h;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == CACHE_MAGIC && h.n >= 0 && h.n < 200000 &&
              bs_series_reserve(s, h.n) && (h.n == 0 || fread(s->p, sizeof *s->p, (size_t)h.n, f) == (size_t)h.n);
    fclose(f);
    if (!ok) return false;
    s->n = h.n;
    s->fetched = h.fetched;
    s->fetched_ms = 0;
    s->ohlc = h.ohlc;
    s->vol = h.vol;
    s->prov = h.prov;
    return true;
}

/* The quotes: quotes.bin for the app, quotes.txt for the page */
typedef struct {
    char sym[BS_SYM_LEN];
    bs_quote_t q;
} qrec_t;

bool bs_quotes_save(void)
{
    char p[200], t[200];
    snprintf(p, sizeof p, "%.150s/quotes.bin", bs_data_dir());
    snprintf(t, sizeof t, "%.150s/quotes.txt", bs_data_dir());
    FILE *f = fopen(p, "wb");
    FILE *g = fopen(t, "w");
    if (g) fputs("# sym\tprice\tprev\ttime\tsource (written by the app)\n", g);
    for (int i = 0; i < bs_items_count(); i++) {
        bs_item_t *it = bs_item_at(i);
        bs_quote_t q;
        if (!bs_item_quote(it, &q)) continue;
        if (f) {
            qrec_t r;
            memset(&r, 0, sizeof r);
            bs_scpy(r.sym, sizeof r.sym, it->sym);
            r.q = it->q;
            if (it->q.ok) fwrite(&r, sizeof r, 1, f);
        }
        if (g) {
            char a[32], b[32];
            /* plain decimal points for the page */
            snprintf(a, sizeof a, "%ld.%04ld", (long)(int32_t)q.price,
                     (long)(int32_t)roundf((q.price - (float)(int32_t)q.price) * 10000.0f));
            snprintf(b, sizeof b, "%ld.%04ld", (long)(int32_t)q.prev,
                     (long)(int32_t)roundf((q.prev - (float)(int32_t)q.prev) * 10000.0f));
            fprintf(g, "%s\t%s\t%s\t%ld\t%s\n", it->sym, a, (q.has & BS_Q_PREV) ? b : "",
                    (long)(q.t ? q.t : q.fetched), bs_prov_name((bs_prov_t)q.prov));
        }
    }
    if (f) fclose(f);
    if (g) fclose(g);
    return f != NULL;
}

void bs_quotes_load(void)
{
    char p[200];
    snprintf(p, sizeof p, "%.150s/quotes.bin", bs_data_dir());
    FILE *f = fopen(p, "rb");
    if (!f) return;
    qrec_t r;
    while (fread(&r, sizeof r, 1, f) == 1) {
        r.sym[BS_SYM_LEN - 1] = 0;
        bs_item_t *it = bs_item_find(r.sym);
        if (it && !it->q.ok) {
            it->q = r.q;
            it->q.fetched_ms = 0;
        }
    }
    fclose(f);
}

/* ---- indicators ---- */

void bs_sma(const bs_pt_t *p, int cnt, int n, float *out)
{
    float sum = 0;
    for (int i = 0; i < cnt; i++) {
        sum += p[i].c;
        if (i >= n) sum -= p[i - n].c;
        out[i] = i >= n - 1 ? sum / (float)n : NAN;
    }
}

void bs_rsi(const bs_pt_t *p, int cnt, int n, float *out)
{
    float up = 0, dn = 0;
    for (int i = 0; i < cnt; i++) {
        out[i] = NAN;
        if (i == 0) continue;
        float d = p[i].c - p[i - 1].c;
        float u = d > 0 ? d : 0, w = d < 0 ? -d : 0;
        if (i <= n) {
            up += u;
            dn += w;
            if (i == n) { up /= (float)n; dn /= (float)n; }
            else continue;
        } else {
            up = (up * (float)(n - 1) + u) / (float)n;
            dn = (dn * (float)(n - 1) + w) / (float)n;
        }
        out[i] = dn <= 0 ? 100.0f : 100.0f - 100.0f / (1.0f + up / dn);
    }
}

void bs_quote_from_series(const bs_series_t *s, bs_quote_t *q, bool intraday_today)
{
    (void)intraday_today;
    memset(q, 0, sizeof *q);
    if (!s || s->n < 1) return;
    const bs_pt_t *last = &s->p[s->n - 1];
    q->ok = true;
    q->price = last->c;
    q->t = last->t;
    q->fetched = s->fetched;
    q->fetched_ms = s->fetched_ms;
    q->prov = s->prov;
    if (s->n >= 2) {
        q->prev = s->p[s->n - 2].c;
        q->has |= BS_Q_PREV;
    }
    if (s->ohlc) {
        q->open = last->o;
        q->high = last->h;
        q->low = last->l;
        q->has |= BS_Q_OHL;
    }
    if (s->vol) {
        q->vol = last->v;
        q->has |= BS_Q_VOL;
    }
    int32_t from = last->t - 365 * 86400;
    int i0 = bs_series_find(s, from);
    if (s->n - i0 >= 20) {
        float lo = INFINITY, hi = -INFINITY;
        for (int i = i0; i < s->n; i++) {
            float l = s->ohlc ? s->p[i].l : s->p[i].c, h = s->ohlc ? s->p[i].h : s->p[i].c;
            if (l < lo) lo = l;
            if (h > hi) hi = h;
        }
        q->lo52 = lo;
        q->hi52 = hi;
        q->has |= BS_Q_52W;
    }
}

/* ---- dates ---- */

int32_t bs_days(int y, int m, int d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    int32_t yoe = y - era * 400;
    int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void bs_civil(int32_t z, int *y, int *m, int *d)
{
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    int32_t doe = z - era * 146097;
    int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t yy = yoe + era * 400;
    int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int32_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

int32_t bs_now(void)
{
    if (!aos_hal_time_is_valid()) return 0;
    return (int32_t)time(NULL);
}

static bool lang_en(void)
{
    const char *l = aos_i18n_current();
    return l && !strcmp(l, "en");
}

char bs_dec_sep(void) { return lang_en() ? '.' : ','; }

static void local_tm(int32_t t, struct tm *out)
{
    time_t tt = (time_t)t;
    localtime_r(&tt, out);
}

void bs_fmt_date(int32_t t, bool time_too, char *out, size_t n)
{
    struct tm tm;
    local_tm(t, &tm);
    if (lang_en()) {
        if (time_too) snprintf(out, n, "%02d/%02d %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        else snprintf(out, n, "%02d/%02d/%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year % 100);
    } else {
        if (time_too) snprintf(out, n, "%02d/%02d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
        else snprintf(out, n, "%02d/%02d/%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100);
    }
}

void bs_fmt_hm(int32_t t, char *out, size_t n)
{
    struct tm tm;
    local_tm(t, &tm);
    snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void bs_fmt_ago(int32_t t, char *out, size_t n)
{
    int32_t now = bs_now();
    out[0] = 0;
    if (!t || !now) return;
    int32_t s = now - t;
    if (s < 0) s = 0;
    if (s < 90) snprintf(out, n, "%s", _("recién"));
    else if (s < 5400) snprintf(out, n, _("hace %d min"), (int)(s / 60));
    else if (s < 2 * 86400) snprintf(out, n, _("hace %d h"), (int)(s / 3600));
    else snprintf(out, n, _("hace %d días"), (int)(s / 86400));
}

/* ---- numbers ---- */

/* v >= 0 with 'dec' decimals and thousands separators, into out */
static void fmt_pos(float v, int dec, char *out, size_t n)
{
    bool en = lang_en();
    char th = en ? ',' : '.', ds = en ? '.' : ',';
    if (v >= 4.0e9f) {          /* not a price: a volume or a market cap */
        bs_fmt_big(v, out, n);
        return;
    }
    float scale = 1;
    for (int i = 0; i < dec; i++) scale *= 10;
    uint32_t ip = (uint32_t)v;
    float fr = v - (float)ip;
    uint32_t f = (uint32_t)roundf(fr * scale);
    if (f >= (uint32_t)scale) { ip++; f = 0; }
    char digits[16];
    snprintf(digits, sizeof digits, "%lu", (unsigned long)ip);
    size_t len = strlen(digits), w = 0;
    for (size_t i = 0; i < len && w + 2 < n; i++) {
        out[w++] = digits[i];
        size_t left = len - 1 - i;
        if (left && left % 3 == 0) out[w++] = th;
    }
    if (dec > 0 && w + 2 + (size_t)dec < n) {
        out[w++] = ds;
        /* the decimals by hand: zero-padded, 'dec' digits */
        for (int i = dec - 1; i >= 0; i--) {
            out[w + (size_t)i] = (char)('0' + f % 10);
            f /= 10;
        }
        w += (size_t)dec;
    }
    out[w] = 0;
}

void bs_fmt_num(float v, int dec, char *out, size_t n)
{
    if (isnan(v)) { bs_scpy(out, n, "--"); return; }
    if (v < 0) {
        if (n < 2) return;
        out[0] = '-';
        fmt_pos(-v, dec, out + 1, n - 1);
    } else {
        fmt_pos(v, dec, out, n);
    }
}

static int price_dec(float v, const bs_info_t *in)
{
    float a = fabsf(v);
    if (in && in->kind == BS_K_RISK) return 0;
    if (a >= 100000) return 0;
    if (a >= 1) return 2;
    if (a >= 0.01f) return 4;
    return 6;
}

void bs_fmt_price(float v, const bs_info_t *in, char *out, size_t n)
{
    bs_fmt_num(v, price_dec(v, in), out, n);
}

void bs_fmt_chg(float chg, const bs_info_t *in, char *out, size_t n)
{
    char b[32];
    /* a change is shown with the cents of a price, not with the four
     * decimals a small number would get */
    float a = fabsf(chg);
    int dec = in && in->kind == BS_K_RISK ? 0 : a >= 100000 ? 0 : a >= 0.01f ? 2 : price_dec(chg, in);
    bs_fmt_num(a, dec, b, sizeof b);
    snprintf(out, n, "%s%s", chg > 0 ? "+" : chg < 0 ? "-" : "", b);
}

void bs_fmt_pct(float pct, char *out, size_t n)
{
    char b[24];
    bs_fmt_num(fabsf(pct), 2, b, sizeof b);
    snprintf(out, n, "%s%s %%", pct > 0 ? "+" : pct < 0 ? "-" : "", b);
}

void bs_fmt_big(float v, char *out, size_t n)
{
    char b[24];
    float a = fabsf(v);
    if (a >= 1e12f) { bs_fmt_num(v / 1e12f, 2, b, sizeof b); snprintf(out, n, _("%s B"), b); }
    else if (a >= 1e9f) { bs_fmt_num(v / 1e9f, 2, b, sizeof b); snprintf(out, n, _("%s mil M"), b); }
    else if (a >= 1e6f) { bs_fmt_num(v / 1e6f, 2, b, sizeof b); snprintf(out, n, _("%s M"), b); }
    else if (a >= 1e4f) { bs_fmt_num(v / 1e3f, 1, b, sizeof b); snprintf(out, n, _("%s mil"), b); }
    else bs_fmt_num(v, 0, out, n);
}
