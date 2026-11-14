/*
 * P4OS - Logic: a capture in memory, and what is asked of it.
 *
 * The samples are bytes, one bit per channel, in PSRAM. Two summaries go
 * with them: for every 64 samples, and for every 4096, the OR and the AND
 * of the bytes. A channel whose OR and AND bits agree did not move in that
 * block, so the view draws a column of a million samples from a few dozen
 * bytes, and an edge search skips quiet stretches 4096 samples at a time.
 */
#include "la.h"

#include "aos_hal.h"

#include <stdlib.h>
#include <string.h>

static uint32_t sum_len(uint32_t n, uint32_t per) { return ((n + per - 1) / per) * 2 + 2; }

void la_cap_free(la_cap_t *c)
{
    la_big_free(c->buf);
    la_big_free(c->s1);
    la_big_free(c->s2);
    c->buf = c->s1 = c->s2 = NULL;
    c->n = c->cap = 0;
    c->trig = -1;
}

bool la_cap_alloc(la_cap_t *c, uint32_t n)
{
    if (n < 16) n = 16;
    if (c->buf && c->cap >= n) return true;
    la_cap_free(c);
    c->buf = la_big_alloc(n);
    c->s1 = la_big_alloc(sum_len(n, LA_SUM1));
    c->s2 = la_big_alloc(sum_len(n, LA_SUM2));
    if (!c->buf || !c->s1 || !c->s2) {
        la_cap_free(c);
        return false;
    }
    c->cap = n;
    return true;
}

void la_cap_summarise(la_cap_t *c)
{
    if (!c->buf || !c->n) return;
    const uint8_t *b = c->buf;
    uint32_t nb1 = (c->n + LA_SUM1 - 1) / LA_SUM1;
    for (uint32_t k = 0; k < nb1; k++) {
        uint32_t i0 = k * LA_SUM1, i1 = i0 + LA_SUM1;
        if (i1 > c->n) i1 = c->n;
        uint8_t o = 0, a = 0xFF;
        /* four at a time where the block is whole: this runs over every
         * sample of a capture of millions after each one */
        if (i1 - i0 == LA_SUM1 && !((uintptr_t)(b + i0) & 3)) {
            const uint32_t *w = (const uint32_t *)(b + i0);
            uint32_t ow = 0, aw = 0xFFFFFFFFu;
            for (int j = 0; j < LA_SUM1 / 4; j++) {
                ow |= w[j];
                aw &= w[j];
            }
            o = (uint8_t)(ow | ow >> 8 | ow >> 16 | ow >> 24);
            a = (uint8_t)(aw & aw >> 8 & aw >> 16 & aw >> 24);
        } else {
            for (uint32_t i = i0; i < i1; i++) {
                o |= b[i];
                a &= b[i];
            }
        }
        c->s1[2 * k] = o;
        c->s1[2 * k + 1] = a;
    }
    uint32_t nb2 = (c->n + LA_SUM2 - 1) / LA_SUM2;
    const uint32_t per = LA_SUM2 / LA_SUM1;
    for (uint32_t k = 0; k < nb2; k++) {
        uint32_t j0 = k * per, j1 = j0 + per;
        if (j1 > nb1) j1 = nb1;
        uint8_t o = 0, a = 0xFF;
        for (uint32_t j = j0; j < j1; j++) {
            o |= c->s1[2 * j];
            a &= c->s1[2 * j + 1];
        }
        c->s2[2 * k] = o;
        c->s2[2 * k + 1] = a;
    }
}

void la_cap_range(const la_cap_t *c, uint32_t a, uint32_t b, uint8_t *or_, uint8_t *and_)
{
    uint8_t o = 0, an = 0xFF;
    if (b > c->n) b = c->n;
    const uint8_t *buf = c->buf;
    uint32_t i = a;
    while (i < b && (i % LA_SUM1)) {
        o |= buf[i];
        an &= buf[i];
        i++;
    }
    while (i + LA_SUM1 <= b && (i % LA_SUM2)) {
        uint32_t k = i / LA_SUM1;
        o |= c->s1[2 * k];
        an &= c->s1[2 * k + 1];
        i += LA_SUM1;
    }
    while (i + LA_SUM2 <= b) {
        uint32_t k = i / LA_SUM2;
        o |= c->s2[2 * k];
        an &= c->s2[2 * k + 1];
        i += LA_SUM2;
    }
    while (i + LA_SUM1 <= b) {
        uint32_t k = i / LA_SUM1;
        o |= c->s1[2 * k];
        an &= c->s1[2 * k + 1];
        i += LA_SUM1;
    }
    while (i < b) {
        o |= buf[i];
        an &= buf[i];
        i++;
    }
    *or_ = o;
    *and_ = an;
}

uint32_t la_next_edge(const la_cap_t *c, int ch, uint32_t from, uint32_t to)
{
    if (to > c->n) to = c->n;
    if (from >= to) return to;
    const uint8_t m = (uint8_t)(1u << ch);
    const uint8_t v = c->buf[from] & m;
    uint32_t i = from + 1;
    while (i < to) {
        if (!(i % LA_SUM2) && i + LA_SUM2 <= to) {
            uint32_t k = i / LA_SUM2;
            if ((c->s2[2 * k] & m) == v && (c->s2[2 * k + 1] & m) == v) {
                i += LA_SUM2;
                continue;
            }
        }
        if (!(i % LA_SUM1) && i + LA_SUM1 <= to) {
            uint32_t k = i / LA_SUM1;
            if ((c->s1[2 * k] & m) == v && (c->s1[2 * k + 1] & m) == v) {
                i += LA_SUM1;
                continue;
            }
        }
        if ((c->buf[i] & m) != v) return i;
        i++;
    }
    return to;
}

/* ---- the trigger ---- */

typedef struct { uint8_t lm, lv, rise, fall, edge; } trig_masks_t;

static void trig_masks(const uint8_t *trig, trig_masks_t *t)
{
    memset(t, 0, sizeof *t);
    for (int ch = 0; ch < LA_NCH; ch++) {
        uint8_t m = (uint8_t)(1u << ch);
        switch (trig[ch]) {
        case LA_T_0:    t->lm |= m; break;
        case LA_T_1:    t->lm |= m; t->lv |= m; break;
        case LA_T_RISE: t->rise |= m; break;
        case LA_T_FALL: t->fall |= m; break;
        case LA_T_EDGE: t->edge |= m; break;
        default: break;
        }
    }
}

bool la_trig_any(const uint8_t *trig)
{
    for (int ch = 0; ch < LA_NCH; ch++)
        if (trig[ch] != LA_T_X) return true;
    return false;
}

int32_t la_trig_find(const uint8_t *trig, const uint8_t *buf, uint32_t n, uint32_t from, uint8_t prev)
{
    trig_masks_t t;
    trig_masks(trig, &t);
    uint8_t p = from ? buf[from - 1] : prev;
    if (!t.rise && !t.fall && !t.edge) {
        for (uint32_t i = from; i < n; i++)
            if ((buf[i] & t.lm) == t.lv) return (int32_t)i;
        return -1;
    }
    for (uint32_t i = from; i < n; i++) {
        uint8_t s = buf[i];
        if ((s & t.lm) == t.lv && (t.rise & ~p & s) == t.rise && (t.fall & p & ~s) == t.fall &&
            (t.edge & (p ^ s)) == t.edge)
            return (int32_t)i;
        p = s;
    }
    return -1;
}

static void reverse(uint8_t *a, uint32_t n)
{
    if (n < 2) return;
    uint8_t *e = a + n - 1;
    while (a < e) {
        uint8_t t = *a;
        *a++ = *e;
        *e-- = t;
    }
}

void la_rotate(uint8_t *buf, uint32_t n, uint32_t first)
{
    if (!first || first >= n) return;
    reverse(buf, first);
    reverse(buf + first, n - first);
    reverse(buf, n);
}

uint32_t la_max_samples(void)
{
    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    /* what is free plus what the capture holds now (it is given back
     * first), less room for the system, the annotations and the summaries */
    uint64_t room = (uint64_t)fp + LA.cap.cap;
    /* 3 MB stay for the system (measured on the board: 15.7 MB of PSRAM
     * free with the app open, 6 MB of margin left captures at 5M) */
    uint64_t keep = 3u * 1024 * 1024;
    /* an FX2 streams through a ring of the host's (la_fx2.c) */
    if (LA.ndevs && LA.devs[LA.dev].drv == LA_DRV_FX2) keep += 2u * 1024 * 1024;
    if (!LA.an.ann) keep += (uint64_t)LA_ANN_MAX * sizeof(la_ann_t) + (uint64_t)LA_PKT_MAX * sizeof(la_pkt_t);
    if (room <= keep) return 0;
    uint64_t n = (room - keep) * 32 / 33;
    if (n > 24u * 1024 * 1024) n = 24u * 1024 * 1024;
    return (uint32_t)n;
}

/* ---- measurements ---- */

void la_measure(const la_cap_t *c, int ch, uint32_t a, uint32_t b, la_meas_t *m)
{
    memset(m, 0, sizeof *m);
    if (!c->buf || b > c->n) b = c->n;
    if (!c->buf || a + 1 >= b || !c->rate) return;
    const float ts = 1.0f / (float)c->rate;
    const uint8_t mask = (uint8_t)(1u << ch);
    bool v = c->buf[a] & mask;
    uint32_t pos = a, high = 0;
    int64_t first_rise = -1, last_rise = -1, last_fall = -1;
    uint32_t nhi = 0, nlo = 0;
    float hi_sum = 0, lo_sum = 0;
    for (;;) {
        uint32_t e = la_next_edge(c, ch, pos, b);
        if (v) high += e - pos;
        if (e >= b) break;
        v = !v;
        if (v) {
            m->rises++;
            if (first_rise < 0) first_rise = e;
            if (last_fall >= 0) {
                float w = (float)(e - (uint32_t)last_fall) * ts;
                if (!nlo || w < m->lo_min) m->lo_min = w;
                if (w > m->lo_max) m->lo_max = w;
                lo_sum += w;
                nlo++;
            }
            last_rise = e;
        } else {
            m->falls++;
            if (last_rise >= 0) {
                float w = (float)(e - (uint32_t)last_rise) * ts;
                if (!nhi || w < m->hi_min) m->hi_min = w;
                if (w > m->hi_max) m->hi_max = w;
                hi_sum += w;
                nhi++;
            }
            last_fall = e;
        }
        pos = e;
    }
    m->high_frac = (float)high / (float)(b - a);
    if (nhi) m->hi_avg = hi_sum / (float)nhi;
    if (nlo) m->lo_avg = lo_sum / (float)nlo;
    if (m->rises >= 2) {
        float span = (float)((uint32_t)last_rise - (uint32_t)first_rise) * ts;
        if (span > 0) m->freq = (float)(m->rises - 1) / span;
        if (m->hi_avg > 0 && m->freq > 0) {
            m->duty = m->hi_avg * m->freq;
            if (m->duty > 1) m->duty = 1;
        }
    }
}
