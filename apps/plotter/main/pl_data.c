/*
 * P4OS - Monitor serie: the samples.
 *
 * Two rings in PSRAM, allocated once when the app opens:
 *
 *   rows      every parsed line: its time and a value per series (NAN where
 *             the line had none). 2 MB, ~58 000 rows: three minutes of the
 *             most a 115200 line carries (~330 lines of 35 bytes a second),
 *             hours at the pace of a sensor.
 *   buckets   one per second: the low, the high, the sum and the count of
 *             each series. 3.25 MB, ~8 hours whatever the pace.
 *
 * The chart asks for columns over a window: from the rows when the window
 * is inside them and not too crowded, from the buckets otherwise. Each
 * column keeps the low, the high and the last value of each series, so a
 * thousand samples squeezed into one pixel still show their spikes.
 */
#include "pl.h"

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

#define RAW_BYTES (2u * 1024 * 1024)
#define BKT_BYTES (3328u * 1024)
#define BKT_MS    1000u
#define RAW_SCAN_MAX 60000u    /* more rows than this in a window: buckets */

pl_data_t PD;

static const uint32_t PALETTE[PL_MAXS] = {
    0x40C8E0, 0xFF9F0A, 0x30D158, 0xFF375F, 0xFFD60A, 0xBF5AF2, 0x0A84FF, 0xFF453A,
};

void *pl_ps_alloc(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) return p;
#endif
    return malloc(n);
}

void pl_scpy(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    size_t i = 0;
    if (src)
        for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

bool pl_has_nocase(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return true;
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return true;
    return false;
}

void pl_fmt(char *out, size_t cap, float v, int dec)
{
    if (PL_ISNAN(v)) { snprintf(out, cap, "-"); return; }
    float a = fabsf(v);
    if (a >= 1e7f || (a > 0 && a < 1e-4f && dec < 5)) { snprintf(out, cap, "%.3g", (double)v); return; }
    if (dec < 0) dec = 0;
    if (dec > 6) dec = 6;
    snprintf(out, cap, "%.*f", dec, (double)v);
}

bool pl_data_init(void)
{
    if (PD.raw) return true;
    PD.raw_cap = RAW_BYTES / sizeof(pl_row_t);
    PD.bk_cap = BKT_BYTES / sizeof(pl_bkt_t);
    PD.raw = pl_ps_alloc(PD.raw_cap * sizeof(pl_row_t));
    PD.bk = pl_ps_alloc(PD.bk_cap * sizeof(pl_bkt_t));
    if (!PD.raw || !PD.bk) {
        /* half, then nothing */
        free(PD.raw);
        free(PD.bk);
        PD.raw_cap /= 4;
        PD.bk_cap /= 4;
        PD.raw = pl_ps_alloc(PD.raw_cap * sizeof(pl_row_t));
        PD.bk = pl_ps_alloc(PD.bk_cap * sizeof(pl_bkt_t));
        if (!PD.raw || !PD.bk) {
            free(PD.raw);
            free(PD.bk);
            PD.raw = NULL;
            PD.bk = NULL;
            return false;
        }
    }
    aos_hal_log("plotter", "%u rows, %u seconds", (unsigned)PD.raw_cap, (unsigned)PD.bk_cap);
    pl_data_clear();
    return true;
}

void pl_data_free(void)
{
    free(PD.raw);
    free(PD.bk);
    memset(&PD, 0, sizeof PD);
}

void pl_data_clear(void)
{
    PD.raw_n = PD.raw_head = 0;
    PD.bk_n = PD.bk_head = 0;
    PD.cur_open = false;
    PD.rows = 0;
    /* the series stay (their names, colours and alarms), without values */
    for (int i = 0; i < PD.ns; i++) {
        pl_series_t *s = &PD.s[i];
        s->has = false;
        s->last = NAN;
        s->count = 0;
        s->dt = 0;
        s->state = 0;
    }
    PD.gen++;
}

int pl_series_get(const char *name, const char *unit)
{
    for (int i = 0; i < PD.ns; i++)
        if (!strcmp(PD.s[i].name, name)) {
            if (unit && unit[0] && !PD.s[i].unit[0]) pl_scpy(PD.s[i].unit, PL_UNIT, unit);
            return i;
        }
    if (PD.ns >= PL_MAXS) return -1;
    pl_series_t *s = &PD.s[PD.ns];
    memset(s, 0, sizeof *s);
    pl_scpy(s->name, PL_NAME, name);
    pl_scpy(s->unit, PL_UNIT, unit);
    s->col = PALETTE[PD.ns];
    s->last = NAN;
    s->dec = -1;
    pl_cfg_alarm_apply(s);
    PD.gen++;
    return PD.ns++;
}

static void bkt_push(void)
{
    if (!PD.cur_open) return;
    PD.bk[PD.bk_head] = PD.cur;
    PD.bk_head = (PD.bk_head + 1) % PD.bk_cap;
    if (PD.bk_n < PD.bk_cap) PD.bk_n++;
    PD.cur_open = false;
}

void pl_data_add(uint32_t t, const pl_parsed_t *p, pl_row_t *row)
{
    pl_row_t r;
    r.t = t;
    for (int i = 0; i < PL_MAXS; i++) r.v[i] = NAN;
    for (int k = 0; k < p->n; k++) {
        int i = pl_series_get(p->name[k], p->unit[k]);
        if (i < 0) continue;
        r.v[i] = p->v[k];
        pl_series_t *s = &PD.s[i];
        /* the decimals: the most seen lately, so 26.50 does not flicker to 26.5 */
        if (s->count == 0 || p->dec[k] > s->dec || s->count % 256 == 0) s->dec = p->dec[k];
        if (s->has && t > s->last_t) {
            float d = (float)(t - s->last_t);
            s->dt = s->dt <= 0 ? d : s->dt * 0.9f + d * 0.1f;
        }
        s->last = p->v[k];
        s->last_t = t;
        s->has = true;
        s->count++;
    }
    if (row) *row = r;
    if (!PD.raw) return;
    /* a clock that went back (a restarted simulator): start again */
    if (PD.raw_n && t < pl_data_last_t()) pl_data_clear();
    PD.raw[PD.raw_head] = r;
    PD.raw_head = (PD.raw_head + 1) % PD.raw_cap;
    if (PD.raw_n < PD.raw_cap) PD.raw_n++;
    PD.rows++;
    /* the second's bucket */
    uint32_t bt = t - t % BKT_MS;
    if (PD.cur_open && PD.cur.t != bt) bkt_push();
    if (!PD.cur_open) {
        memset(&PD.cur, 0, sizeof PD.cur);
        PD.cur.t = bt;
        PD.cur_open = true;
    }
    for (int i = 0; i < PL_MAXS; i++) {
        float v = r.v[i];
        if (PL_ISNAN(v)) continue;
        if (!PD.cur.n[i] || v < PD.cur.mn[i]) PD.cur.mn[i] = v;
        if (!PD.cur.n[i] || v > PD.cur.mx[i]) PD.cur.mx[i] = v;
        PD.cur.sum[i] += v;
        if (PD.cur.n[i] < 65535) PD.cur.n[i]++;
    }
}

static const pl_row_t *row_at(uint32_t i)
{
    return &PD.raw[(PD.raw_head + PD.raw_cap - PD.raw_n + i) % PD.raw_cap];
}

static const pl_bkt_t *bkt_at(uint32_t i)
{
    if (i == PD.bk_n) return &PD.cur;
    return &PD.bk[(PD.bk_head + PD.bk_cap - PD.bk_n + i) % PD.bk_cap];
}

uint32_t pl_data_first_t(void)
{
    if (PD.bk_n) return bkt_at(0)->t;
    return PD.raw_n ? row_at(0)->t : 0;
}

uint32_t pl_data_last_t(void)
{
    return PD.raw_n ? row_at(PD.raw_n - 1)->t : 0;
}

/* the first row at or after t */
static uint32_t raw_lower(uint32_t t)
{
    uint32_t lo = 0, hi = PD.raw_n;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (row_at(m)->t < t) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static uint32_t bkt_lower(uint32_t t)
{
    uint32_t n = PD.bk_n + (PD.cur_open ? 1 : 0);
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (bkt_at(m)->t + BKT_MS <= t) lo = m + 1;
        else hi = m;
    }
    return lo;
}

bool pl_cols_alloc(pl_cols_t *c, int cap)
{
    if (c->cap >= cap && c->mn) return true;
    pl_cols_free(c);
    size_t n = (size_t)cap * PL_MAXS;
    c->mn = pl_ps_alloc(n * sizeof(float));
    c->mx = pl_ps_alloc(n * sizeof(float));
    c->last = pl_ps_alloc(n * sizeof(float));
    c->lt = pl_ps_alloc(n * sizeof(uint32_t));
    if (!c->mn || !c->mx || !c->last || !c->lt) { pl_cols_free(c); return false; }
    c->cap = cap;
    return true;
}

void pl_cols_free(pl_cols_t *c)
{
    free(c->mn);
    free(c->mx);
    free(c->last);
    free(c->lt);
    c->mn = c->mx = c->last = NULL;
    c->lt = NULL;
    c->cap = 0;
}

static inline void put(pl_cols_t *c, int s, int col, float mn, float mx, float last, uint32_t t)
{
    int k = s * c->cap + col;
    if (PL_ISNAN(c->last[k])) {
        c->mn[k] = mn;
        c->mx[k] = mx;
    } else {
        if (mn < c->mn[k]) c->mn[k] = mn;
        if (mx > c->mx[k]) c->mx[k] = mx;
    }
    c->last[k] = last;
    c->lt[k] = t;
}

void pl_data_columns(pl_cols_t *c, uint32_t t0, uint32_t t1, int ncols)
{
    if (ncols > c->cap) ncols = c->cap;
    if (ncols < 1 || t1 <= t0) { c->ncols = 0; return; }
    c->ncols = ncols;
    c->t0 = t0;
    c->t1 = t1;
    for (int s = 0; s < PL_MAXS; s++) {
        for (int k = 0; k < ncols; k++) c->last[s * c->cap + k] = NAN;
        c->smin[s] = c->smax[s] = c->slast[s] = NAN;
        c->ssum[s] = 0;
        c->sn[s] = 0;
        c->slast_t[s] = 0;
    }
    if (!PD.raw) return;
    float span = (float)(t1 - t0);
    uint32_t i0 = 0, i1 = 0;
    bool use_raw = PD.raw_n && row_at(0)->t <= t0;
    if (PD.raw_n && !use_raw) {
        /* the rows started inside the window, and nothing older was ever
         * dropped: the rows have it all */
        use_raw = PD.raw_n == PD.rows;
    }
    if (use_raw) {
        i0 = raw_lower(t0);
        i1 = raw_lower(t1 + 1);
        if (i1 - i0 > RAW_SCAN_MAX) use_raw = false;
    }
    c->buckets = !use_raw;
    if (use_raw) {
        for (uint32_t i = i0; i < i1; i++) {
            const pl_row_t *r = row_at(i);
            int col = (int)((float)(r->t - t0) * (float)ncols / span);
            if (col >= ncols) col = ncols - 1;
            for (int s = 0; s < PD.ns; s++) {
                float v = r->v[s];
                if (PL_ISNAN(v)) continue;
                put(c, s, col, v, v, v, r->t);
                if (!c->sn[s] || v < c->smin[s]) c->smin[s] = v;
                if (!c->sn[s] || v > c->smax[s]) c->smax[s] = v;
                c->ssum[s] += v;
                c->sn[s]++;
                c->slast[s] = v;
                c->slast_t[s] = r->t;
            }
        }
        return;
    }
    uint32_t n = PD.bk_n + (PD.cur_open ? 1 : 0);
    for (uint32_t i = bkt_lower(t0); i < n; i++) {
        const pl_bkt_t *b = bkt_at(i);
        if (b->t > t1) break;
        uint32_t tm = b->t + BKT_MS / 2;
        int col = tm < t0 ? 0 : (int)((float)(tm - t0) * (float)ncols / span);
        if (col >= ncols) col = ncols - 1;
        for (int s = 0; s < PD.ns; s++) {
            if (!b->n[s]) continue;
            float avg = b->sum[s] / (float)b->n[s];
            put(c, s, col, b->mn[s], b->mx[s], avg, tm);
            if (!c->sn[s] || b->mn[s] < c->smin[s]) c->smin[s] = b->mn[s];
            if (!c->sn[s] || b->mx[s] > c->smax[s]) c->smax[s] = b->mx[s];
            c->ssum[s] += b->sum[s];
            c->sn[s] += b->n[s];
            c->slast[s] = avg;
            c->slast_t[s] = tm;
        }
    }
    /* the very last value is the last sample, not a second's average */
    for (int s = 0; s < PD.ns; s++)
        if (c->sn[s] && PD.s[s].has && PD.s[s].last_t >= t0 && PD.s[s].last_t <= t1) {
            c->slast[s] = PD.s[s].last;
            c->slast_t[s] = PD.s[s].last_t;
        }
}

int pl_data_rows(uint32_t t0, uint32_t t1, int max, pl_row_cb_t cb, void *ud)
{
    if (!PD.raw_n || max < 1) return 0;
    uint32_t i0 = raw_lower(t0), i1 = raw_lower(t1 + 1);
    if (i1 <= i0) return 0;
    uint32_t n = i1 - i0;
    int out = 0;
    if (n <= (uint32_t)max) {
        for (uint32_t i = i0; i < i1; i++, out++) cb(row_at(i), ud);
    } else {
        for (int k = 0; k < max; k++, out++) cb(row_at(i0 + (uint32_t)((float)k * (float)n / (float)max)), ud);
    }
    return out;
}
