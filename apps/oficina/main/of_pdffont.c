/*
 * P4OS - Office: the fonts a PDF carries, read for their outlines.
 *
 * TrueType (glyf, composite glyphs, cmaps 0, 4, 6 and 12), CFF (bare, as
 * FontFile3, or inside OpenType; CID-keyed with FDArray and FDSelect too;
 * Type 2 charstrings with subroutines, hints skipped, flex, seac) and
 * Type 1 (eexec, its Subrs and CharStrings, flex through othersubrs, seac).
 * Every offset read from the file is checked against its length: a font
 * that lies gives no outline, never a read outside it.
 */
#include "of_pdfd.h"
#include "of_pdfi.h"
#include "of_pdfnames.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Paths                                                                       */
/* -------------------------------------------------------------------------- */

static bool path_room(of_path_t *p, int ops, int pts)
{
    if (p->oom) return false;
    if (p->nop + ops > p->capop) {
        int nc = p->capop ? p->capop * 2 : 64;
        while (nc < p->nop + ops) nc *= 2;
        uint8_t *n = of_realloc(p->op, (size_t)nc);
        if (!n) { p->oom = true; return false; }
        p->op = n;
        p->capop = nc;
    }
    if (p->npt + pts > p->cappt) {
        int nc = p->cappt ? p->cappt * 2 : 128;
        while (nc < p->npt + pts) nc *= 2;
        of_pt_t *n = of_realloc(p->pt, (size_t)nc * sizeof *n);
        if (!n) { p->oom = true; return false; }
        p->pt = n;
        p->cappt = nc;
    }
    return true;
}

void of_path_reset(of_path_t *p) { p->nop = p->npt = 0; p->oom = false; }
void of_path_free(of_path_t *p) { of_free(p->op); of_free(p->pt); memset(p, 0, sizeof *p); }

void of_path_move(of_path_t *p, float x, float y)
{
    /* a move after a move: the last one counts */
    if (p->nop && p->op[p->nop - 1] == 'M') { p->pt[p->npt - 1] = (of_pt_t){ x, y }; return; }
    if (!path_room(p, 1, 1)) return;
    p->op[p->nop++] = 'M';
    p->pt[p->npt++] = (of_pt_t){ x, y };
}

void of_path_line(of_path_t *p, float x, float y)
{
    if (!p->nop) of_path_move(p, x, y);
    if (!path_room(p, 1, 1)) return;
    p->op[p->nop++] = 'L';
    p->pt[p->npt++] = (of_pt_t){ x, y };
}

void of_path_quad(of_path_t *p, float cx, float cy, float x, float y)
{
    if (!p->nop) of_path_move(p, cx, cy);
    if (!path_room(p, 1, 2)) return;
    p->op[p->nop++] = 'Q';
    p->pt[p->npt++] = (of_pt_t){ cx, cy };
    p->pt[p->npt++] = (of_pt_t){ x, y };
}

void of_path_cubic(of_path_t *p, float x1, float y1, float x2, float y2, float x3, float y3)
{
    if (!p->nop) of_path_move(p, x1, y1);
    if (!path_room(p, 1, 3)) return;
    p->op[p->nop++] = 'C';
    p->pt[p->npt++] = (of_pt_t){ x1, y1 };
    p->pt[p->npt++] = (of_pt_t){ x2, y2 };
    p->pt[p->npt++] = (of_pt_t){ x3, y3 };
}

void of_path_close(of_path_t *p)
{
    if (!p->nop || p->op[p->nop - 1] == 'Z') return;
    if (!path_room(p, 1, 0)) return;
    p->op[p->nop++] = 'Z';
}

/* -------------------------------------------------------------------------- */
/* Reading the file, checked                                                   */
/* -------------------------------------------------------------------------- */

static inline uint32_t u8at(const of_fprog_t *F, uint32_t o) { return o < F->n ? F->data[o] : 0; }
static inline uint32_t u16at(const of_fprog_t *F, uint32_t o) { return (u8at(F, o) << 8) | u8at(F, o + 1); }
static inline int32_t s16at(const of_fprog_t *F, uint32_t o) { return (int16_t)u16at(F, o); }
static inline uint32_t u32at(const of_fprog_t *F, uint32_t o) { return (u16at(F, o) << 16) | u16at(F, o + 2); }
static inline bool in(const of_fprog_t *F, uint32_t o, uint32_t len) { return o <= F->n && len <= F->n - o; }

/* numbers from the file as integers, never out of range */
static inline uint32_t du(float v) { return v > 0 && v < 4.0e9f ? (uint32_t)v : 0; }
static inline int si(float v) { return v > -1.0e9f && v < 1.0e9f ? (int)v : 0; }

/* the outline under construction, through a matrix */
typedef struct {
    of_path_t *out;
    of_mtx_t   m;
    bool       open;
} pen_t;

static void pen_move(pen_t *P, float x, float y)
{
    if (P->open) of_path_close(P->out);
    of_path_move(P->out, P->m.a * x + P->m.c * y + P->m.e, P->m.b * x + P->m.d * y + P->m.f);
    P->open = true;
}

static void pen_line(pen_t *P, float x, float y)
{
    of_path_line(P->out, P->m.a * x + P->m.c * y + P->m.e, P->m.b * x + P->m.d * y + P->m.f);
}

static void pen_quad(pen_t *P, float cx, float cy, float x, float y)
{
    const of_mtx_t *m = &P->m;
    of_path_quad(P->out, m->a * cx + m->c * cy + m->e, m->b * cx + m->d * cy + m->f, m->a * x + m->c * y + m->e, m->b * x + m->d * y + m->f);
}

static void pen_cubic(pen_t *P, float x1, float y1, float x2, float y2, float x3, float y3)
{
    const of_mtx_t *m = &P->m;
    of_path_cubic(P->out, m->a * x1 + m->c * y1 + m->e, m->b * x1 + m->d * y1 + m->f, m->a * x2 + m->c * y2 + m->e,
                  m->b * x2 + m->d * y2 + m->f, m->a * x3 + m->c * y3 + m->e, m->b * x3 + m->d * y3 + m->f);
}

static void pen_end(pen_t *P)
{
    if (P->open) of_path_close(P->out);
    P->open = false;
}

/* -------------------------------------------------------------------------- */
/* Names and Unicode                                                           */
/* -------------------------------------------------------------------------- */

static int gname_cmp(const void *a, const void *b) { return strcmp(((const of_gname_t *)a)->name, ((const of_gname_t *)b)->name); }
static int guni_cmp(const void *a, const void *b)
{
    const of_guni_t *x = a, *y = b;
    if (x->cp != y->cp) return x->cp < y->cp ? -1 : 1;
    return x->gid - y->gid;
}

static const char *std_name(int sid) { return sid >= 0 && sid < 391 ? OF_CFF_STD + OF_CFF_STD_OFF[sid] : ""; }

/* byuni from byname, both sorted */
static void index_names(of_fprog_t *F)
{
    if (!F->nbyname) return;
    F->byuni = of_alloc((size_t)F->nbyname * sizeof *F->byuni);
    for (int i = 0; F->byuni && i < F->nbyname; i++) {
        uint32_t cp = of_pdf_glyph_cp(F->byname[i].name);
        if (cp) F->byuni[F->nbyuni++] = (of_guni_t){ cp, F->byname[i].gid };
    }
    if (F->nbyuni) qsort(F->byuni, (size_t)F->nbyuni, sizeof *F->byuni, guni_cmp);
    qsort(F->byname, (size_t)F->nbyname, sizeof *F->byname, gname_cmp);
}

int of_fprog_by_name(const of_fprog_t *F, const char *name)
{
    if (!name || !F->nbyname) return -1;
    int lo = 0, hi = F->nbyname - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2, c = strcmp(F->byname[m].name, name);
        if (!c) return F->byname[m].gid;
        if (c < 0) lo = m + 1;
        else hi = m - 1;
    }
    return -1;
}

static int guni_find(const of_guni_t *a, int n, uint32_t cp)
{
    int lo = 0, hi = n - 1, best = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (a[m].cp == cp) { best = a[m].gid; hi = m - 1; }
        else if (a[m].cp < cp) lo = m + 1;
        else hi = m - 1;
    }
    return best;
}

/* -------------------------------------------------------------------------- */
/* TrueType                                                                    */
/* -------------------------------------------------------------------------- */

static uint32_t sfnt_table(const of_fprog_t *F, const char *tag, uint32_t *len)
{
    uint32_t nt = u16at(F, 4);
    for (uint32_t i = 0; i < nt && i < 256; i++) {
        uint32_t r = 12 + 16 * i;
        if (!in(F, r, 16)) break;
        if (!memcmp(F->data + r, tag, 4)) {
            uint32_t o = u32at(F, r + 8), l = u32at(F, r + 12);
            if (!in(F, o, l)) l = o < F->n ? (uint32_t)F->n - o : 0;
            if (len) *len = l;
            return l ? o : 0;
        }
    }
    return 0;
}

static int cmap_lookup(const of_fprog_t *F, uint32_t t, uint32_t c)
{
    if (!t) return -1;
    uint32_t fmt = u16at(F, t);
    if (fmt == 0) {
        if (c > 255) return -1;
        int g = (int)u8at(F, t + 6 + c);
        return g ? g : -1;
    }
    if (fmt == 4) {
        uint32_t seg2 = u16at(F, t + 6);
        uint32_t ends = t + 14, starts = ends + seg2 + 2, deltas = starts + seg2, ranges = deltas + seg2;
        if (!in(F, t, 16 + 4 * seg2) || c > 0xFFFF) return -1;
        for (uint32_t i = 0; i < seg2 / 2; i++) {
            uint32_t end = u16at(F, ends + 2 * i);
            if (c > end) continue;
            uint32_t start = u16at(F, starts + 2 * i);
            if (c < start) return -1;
            uint32_t delta = u16at(F, deltas + 2 * i), ro = u16at(F, ranges + 2 * i);
            if (!ro) { int g = (int)((c + delta) & 0xFFFF); return g ? g : -1; }
            uint32_t ga = ranges + 2 * i + ro + 2 * (c - start);
            if (!in(F, ga, 2)) return -1;
            uint32_t g = u16at(F, ga);
            if (!g) return -1;
            g = (g + delta) & 0xFFFF;
            return g ? (int)g : -1;
        }
        return -1;
    }
    if (fmt == 6) {
        uint32_t first = u16at(F, t + 6), cnt = u16at(F, t + 8);
        if (c < first || c - first >= cnt) return -1;
        int g = (int)u16at(F, t + 10 + 2 * (c - first));
        return g ? g : -1;
    }
    if (fmt == 12) {
        uint32_t ng = u32at(F, t + 12);
        if (!in(F, t + 16, ng * 12) || ng > 1000000) return -1;
        uint32_t lo = 0, hi = ng;
        while (lo < hi) {
            uint32_t m = (lo + hi) / 2, g = t + 16 + 12 * m;
            uint32_t s = u32at(F, g), e = u32at(F, g + 4);
            if (c < s) hi = m;
            else if (c > e) lo = m + 1;
            else { int r = (int)(u32at(F, g + 8) + (c - s)); return r ? r : -1; }
        }
    }
    return -1;
}

static bool tt_load(of_fprog_t *F)
{
    uint32_t hl = 0, ml = 0, cl = 0;
    uint32_t head = sfnt_table(F, "head", &hl);
    uint32_t maxp = sfnt_table(F, "maxp", &ml);
    F->glyf = sfnt_table(F, "glyf", &F->glyf_len);
    F->loca = sfnt_table(F, "loca", &F->loca_len);
    if (!head || hl < 54) return false;
    uint32_t upem = u16at(F, head + 18);
    if (upem < 16 || upem > 16384) upem = 1000;
    F->fm = (of_mtx_t){ 1.0f / (float)upem, 0, 0, 1.0f / (float)upem, 0, 0 };
    F->loca32 = u16at(F, head + 50) != 0;
    F->ngl = maxp && ml >= 6 ? (int)u16at(F, maxp + 4) : 0;
    int byloca = F->loca ? (int)(F->loca_len / (F->loca32 ? 4 : 2)) - 1 : 0;
    if (!F->ngl || F->ngl > byloca) F->ngl = byloca;
    uint32_t hh = 0, hhl = 0;
    hh = sfnt_table(F, "hhea", &hhl);
    F->hmtx = sfnt_table(F, "hmtx", NULL);
    F->nhm = hh && hhl >= 36 ? u16at(F, hh + 34) : 0;
    uint32_t cmap = sfnt_table(F, "cmap", &cl);
    if (cmap) {
        uint32_t nt = u16at(F, cmap + 2);
        for (uint32_t i = 0; i < nt && i < 64; i++) {
            uint32_t r = cmap + 4 + 8 * i;
            if (!in(F, r, 8)) break;
            uint32_t pid = u16at(F, r), eid = u16at(F, r + 2), off = u32at(F, r + 4);
            if (off >= cl) continue;
            uint32_t t = cmap + off;
            if (pid == 3 && (eid == 1 || eid == 10) && (!F->c31 || eid == 1)) F->c31 = t;
            else if (pid == 3 && eid == 0) F->c30 = t;
            else if (pid == 1 && eid == 0) F->c10 = t;
            else if (pid == 0) F->c0 = t;
        }
    }
    if (!F->c31) F->c31 = F->c0;
    F->kind = FP_TT;
    return F->glyf && F->loca && F->ngl > 0;
}

static bool tt_glyph(const of_fprog_t *F, int gid, of_mtx_t m, pen_t *P, int depth)
{
    if (gid < 0 || gid >= F->ngl || depth > 8) return false;
    uint32_t o0, o1;
    if (F->loca32) { o0 = u32at(F, F->loca + 4 * (uint32_t)gid); o1 = u32at(F, F->loca + 4 * (uint32_t)gid + 4); }
    else { o0 = 2 * u16at(F, F->loca + 2 * (uint32_t)gid); o1 = 2 * u16at(F, F->loca + 2 * (uint32_t)gid + 2); }
    if (o1 <= o0) return true;                      /* a space */
    if (o1 > F->glyf_len) return false;
    uint32_t g = F->glyf + o0, gend = F->glyf + o1;
    int nc = s16at(F, g);
    if (nc >= 0) {
        uint32_t ends = g + 10;
        if (!nc || !in(F, ends, (uint32_t)nc * 2)) return true;
        int npts = (int)u16at(F, ends + 2 * (uint32_t)(nc - 1)) + 1;
        if (npts <= 0 || npts > 20000) return false;
        uint32_t ins = u16at(F, ends + 2 * (uint32_t)nc);
        uint32_t fp = ends + 2 * (uint32_t)nc + 2 + ins;
        uint8_t *fl = of_alloc((size_t)npts);
        float *xs = of_alloc((size_t)npts * sizeof(float)), *ys = of_alloc((size_t)npts * sizeof(float));
        bool ok = fl && xs && ys;
        /* flags, with repeats */
        for (int i = 0; ok && i < npts;) {
            if (fp >= gend) { ok = false; break; }
            uint8_t f = (uint8_t)u8at(F, fp++);
            fl[i++] = f;
            if (f & 8) {
                int r = (int)u8at(F, fp++);
                while (r-- > 0 && i < npts) fl[i++] = f;
            }
        }
        int32_t v = 0;
        for (int i = 0; ok && i < npts; i++) {
            uint8_t f = fl[i];
            if (f & 2) { int d = (int)u8at(F, fp++); v += f & 16 ? d : -d; }
            else if (!(f & 16)) { v += s16at(F, fp); fp += 2; }
            xs[i] = (float)v;
        }
        v = 0;
        for (int i = 0; ok && i < npts; i++) {
            uint8_t f = fl[i];
            if (f & 4) { int d = (int)u8at(F, fp++); v += f & 32 ? d : -d; }
            else if (!(f & 32)) { v += s16at(F, fp); fp += 2; }
            ys[i] = (float)v;
        }
        if (ok && fp > gend + 1) ok = false;
        pen_t Q = { P->out, of_mtx_mul(m, P->m), false };
        int s = 0;
        for (int c = 0; ok && c < nc; c++) {
            int e = (int)u16at(F, ends + 2 * (uint32_t)c);
            if (e < s || e >= npts) break;
            int n = e - s + 1, first = -1;
            for (int k = 0; k < n; k++) if (fl[s + k] & 1) { first = k; break; }
            float sx, sy;
            int begin, count;
            if (first >= 0) { sx = xs[s + first]; sy = ys[s + first]; begin = first + 1; count = n - 1; }
            else { sx = (xs[s] + xs[e]) / 2; sy = (ys[s] + ys[e]) / 2; begin = 0; count = n; }
            pen_move(&Q, sx, sy);
            bool have_c = false;
            float cx = 0, cy = 0;
            for (int j = 0; j < count; j++) {
                int i = s + (begin + j) % n;
                float x = xs[i], y = ys[i];
                if (fl[i] & 1) {
                    if (have_c) pen_quad(&Q, cx, cy, x, y);
                    else pen_line(&Q, x, y);
                    have_c = false;
                } else {
                    if (have_c) pen_quad(&Q, cx, cy, (cx + x) / 2, (cy + y) / 2);
                    cx = x; cy = y;
                    have_c = true;
                }
            }
            if (have_c) pen_quad(&Q, cx, cy, sx, sy);
            else pen_line(&Q, sx, sy);
            s = e + 1;
        }
        pen_end(&Q);
        of_free(fl); of_free(xs); of_free(ys);
        return ok;
    }
    /* composite */
    uint32_t p = g + 10;
    for (int guard = 0; guard < 64; guard++) {
        if (!in(F, p, 4) || p >= gend) break;
        uint32_t flags = u16at(F, p), sub = u16at(F, p + 2);
        p += 4;
        float dx, dy;
        if (flags & 1) { dx = (float)s16at(F, p); dy = (float)s16at(F, p + 2); p += 4; }
        else { dx = (float)(int8_t)u8at(F, p); dy = (float)(int8_t)u8at(F, p + 1); p += 2; }
        if (!(flags & 2)) dx = dy = 0;              /* matched points: not followed */
        of_mtx_t c = { 1, 0, 0, 1, dx, dy };
        if (flags & 8) { c.a = c.d = (float)s16at(F, p) / 16384.0f; p += 2; }
        else if (flags & 0x40) { c.a = (float)s16at(F, p) / 16384.0f; c.d = (float)s16at(F, p + 2) / 16384.0f; p += 4; }
        else if (flags & 0x80) {
            c.a = (float)s16at(F, p) / 16384.0f; c.b = (float)s16at(F, p + 2) / 16384.0f;
            c.c = (float)s16at(F, p + 4) / 16384.0f; c.d = (float)s16at(F, p + 6) / 16384.0f;
            p += 8;
        }
        tt_glyph(F, (int)sub, of_mtx_mul(c, m), P, depth + 1);
        if (!(flags & 0x20)) break;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* CFF                                                                         */
/* -------------------------------------------------------------------------- */

static bool cff_index(const of_fprog_t *F, uint32_t at, of_cffix_t *x)
{
    memset(x, 0, sizeof *x);
    uint32_t base = F->cff, lim = F->cff + F->cffn;
    if (at < base || at + 2 > lim) return false;
    x->count = u16at(F, at);
    if (!x->count) { x->end = at + 2; return true; }
    x->offsize = u8at(F, at + 2);
    if (x->offsize < 1 || x->offsize > 4) return false;
    x->offs = at + 3;
    uint64_t dpos = (uint64_t)x->offs + (uint64_t)(x->count + 1) * x->offsize - 1;
    if (dpos > lim) return false;
    x->data = (uint32_t)dpos;
    /* the last offset: where it ends */
    uint32_t lo = 0;
    for (uint32_t k = 0; k < x->offsize; k++) lo = lo << 8 | u8at(F, x->offs + x->count * x->offsize + k);
    if ((uint64_t)x->data + lo > lim) return false;
    x->end = x->data + lo;
    return true;
}

static bool cff_get(const of_fprog_t *F, const of_cffix_t *x, uint32_t i, uint32_t *s, uint32_t *e)
{
    if (i >= x->count) return false;
    uint32_t a = 0, b = 0;
    for (uint32_t k = 0; k < x->offsize; k++) {
        a = a << 8 | u8at(F, x->offs + i * x->offsize + k);
        b = b << 8 | u8at(F, x->offs + (i + 1) * x->offsize + k);
    }
    if (a < 1 || b < a || x->data + b > x->end) return false;
    *s = x->data + a;
    *e = x->data + b;
    return true;
}

typedef struct { float v[48]; int n; } dargs_t;

/* a DICT's operators; cb gets each with its operands */
static void cff_dict(const of_fprog_t *F, uint32_t s, uint32_t e, void (*cb)(void *ud, int op, const dargs_t *a), void *ud)
{
    dargs_t a = { .n = 0 };
    uint32_t p = s;
    if (e > F->n) e = (uint32_t)F->n;
    while (p < e) {
        int b = (int)F->data[p];
        if (b <= 21) {
            int op = b;
            p++;
            if (b == 12) { op = 0x100 | (int)u8at(F, p); p++; }
            cb(ud, op, &a);
            a.n = 0;
            continue;
        }
        float v = 0;
        if (b == 28) { v = (float)s16at(F, p + 1); p += 3; }
        else if (b == 29) { v = (float)(int32_t)u32at(F, p + 1); p += 5; }
        else if (b == 30) {
            /* a real: nibbles */
            char buf[40];
            int k = 0;
            bool done = false;
            p++;
            while (p < e && !done) {
                int by = F->data[p++];
                for (int h = 0; h < 2 && !done; h++) {
                    int nb = h ? by & 15 : by >> 4;
                    if (k > 36) { done = true; break; }
                    if (nb <= 9) buf[k++] = (char)('0' + nb);
                    else if (nb == 0xA) buf[k++] = '.';
                    else if (nb == 0xB) buf[k++] = 'E';
                    else if (nb == 0xC) { buf[k++] = 'E'; buf[k++] = '-'; }
                    else if (nb == 0xE) buf[k++] = '-';
                    else if (nb == 0xF) done = true;
                }
            }
            buf[k] = 0;
            v = strtof(buf, NULL);
        } else if (b >= 32 && b <= 246) { v = (float)(b - 139); p++; }
        else if (b >= 247 && b <= 250) { v = (float)((b - 247) * 256 + (int)u8at(F, p + 1) + 108); p += 2; }
        else if (b >= 251 && b <= 254) { v = (float)(-(b - 251) * 256 - (int)u8at(F, p + 1) - 108); p += 2; }
        else { p++; continue; }
        if (a.n < 48) a.v[a.n++] = v;
    }
}

typedef struct {
    uint32_t charset, encoding, charstrings, priv_size, priv_off, fdarray, fdselect, subrs;
    bool     cid, has_fm;
    float    fm[6];
} topd_t;

static void top_cb(void *ud, int op, const dargs_t *a)
{
    topd_t *T = ud;
    float last = a->n ? a->v[a->n - 1] : 0;
    switch (op) {
    case 15: T->charset = du(last); break;
    case 16: T->encoding = du(last); break;
    case 17: T->charstrings = du(last); break;
    case 18: if (a->n >= 2) { T->priv_size = du(a->v[a->n - 2]); T->priv_off = du(last); } break;
    case 19: T->subrs = du(last); break;
    case 0x100 | 30: T->cid = true; break;
    case 0x100 | 36: T->fdarray = du(last); break;
    case 0x100 | 37: T->fdselect = du(last); break;
    case 0x100 | 7:
        if (a->n >= 6) { for (int i = 0; i < 6; i++) T->fm[i] = a->v[a->n - 6 + i]; T->has_fm = true; }
        break;
    default: break;
    }
}

/* a Private DICT's local subroutines */
static void cff_private(const of_fprog_t *F, uint32_t size, uint32_t off, of_cffix_t *subrs)
{
    memset(subrs, 0, sizeof *subrs);
    uint32_t ps = F->cff + off;
    if (!size || off >= F->cffn || size > F->cffn - off) return;
    topd_t P = { 0 };
    cff_dict(F, ps, ps + size, top_cb, &P);
    if (P.subrs) cff_index(F, ps + P.subrs, subrs);
}

static const char *cff_sid_name(const of_fprog_t *F, const of_cffix_t *strings, uint32_t sid, char *buf, size_t bl)
{
    if (sid < 391) return std_name((int)sid);
    uint32_t s, e;
    if (!cff_get(F, strings, sid - 391, &s, &e)) return "";
    size_t l = e - s;
    if (l >= bl) l = bl - 1;
    memcpy(buf, F->data + s, l);
    buf[l] = 0;
    return buf;
}

static bool cff_load(of_fprog_t *F, uint32_t base, uint32_t len)
{
    F->cff = base;
    F->cffn = len;
    F->kind = FP_CFF;
    if (len < 4) return false;
    uint32_t hdr = u8at(F, base + 2);
    of_cffix_t names, tops, strings;
    if (!cff_index(F, base + hdr, &names) || !cff_index(F, names.end, &tops) || !cff_index(F, tops.end, &strings) ||
        !cff_index(F, strings.end, &F->gsubr))
        return false;
    uint32_t ts, te;
    if (!cff_get(F, &tops, 0, &ts, &te)) return false;
    topd_t T = { 0 };
    T.fm[0] = T.fm[3] = 0.001f;
    cff_dict(F, ts, te, top_cb, &T);
    F->fm = (of_mtx_t){ T.fm[0], T.fm[1], T.fm[2], T.fm[3], T.fm[4], T.fm[5] };
    if (!T.has_fm || (fabsf(F->fm.a) < 1e-6f && fabsf(F->fm.b) < 1e-6f)) F->fm = (of_mtx_t){ 0.001f, 0, 0, 0.001f, 0, 0 };
    if (!T.charstrings || !cff_index(F, base + T.charstrings, &F->cs) || !F->cs.count) return false;
    F->ngl = (int)F->cs.count;
    F->cid = T.cid;
    cff_private(F, T.priv_size, T.priv_off, &F->lsubr);
    if (T.fdarray) {
        of_cffix_t fda;
        if (cff_index(F, base + T.fdarray, &fda)) {
            for (uint32_t i = 0; i < fda.count && i < 16; i++) {
                uint32_t s, e;
                if (!cff_get(F, &fda, i, &s, &e)) break;
                topd_t D = { 0 };
                cff_dict(F, s, e, top_cb, &D);
                cff_private(F, D.priv_size, D.priv_off, &F->fdsub[i]);
                F->nfd = (int)i + 1;
            }
        }
        if (T.fdselect && T.fdselect < len) F->fdselect = base + T.fdselect;
    }
    /* the charset: each glyph's name (SID) or CID */
    F->gsid = of_alloc((size_t)F->ngl * sizeof *F->gsid);
    if (!F->gsid) return false;
    if (T.charset > 2 && T.charset < len) {
        uint32_t p = base + T.charset;
        int fmt = (int)u8at(F, p++);
        int g = 1;
        if (fmt == 0) {
            for (; g < F->ngl; g++, p += 2) F->gsid[g] = (uint16_t)u16at(F, p);
        } else if (fmt == 1 || fmt == 2) {
            while (g < F->ngl && p < base + len) {
                uint32_t first = u16at(F, p);
                uint32_t left = fmt == 1 ? u8at(F, p + 2) : u16at(F, p + 2);
                p += fmt == 1 ? 3 : 4;
                for (uint32_t k = 0; k <= left && g < F->ngl; k++) F->gsid[g++] = (uint16_t)(first + k);
            }
        }
    } else {
        for (int g = 0; g < F->ngl; g++) F->gsid[g] = (uint16_t)(g < 229 ? g : 0);
    }
    if (F->cid) {
        F->bycid = of_alloc((size_t)F->ngl * sizeof *F->bycid);
        for (int g = 0; F->bycid && g < F->ngl; g++) F->bycid[F->nbycid++] = (of_guni_t){ F->gsid[g], g };
        if (F->nbycid) qsort(F->bycid, (size_t)F->nbycid, sizeof *F->bycid, guni_cmp);
    } else {
        /* the names, in a pool */
        size_t need = 0;
        char buf[64];
        for (int g = 0; g < F->ngl; g++) need += strlen(cff_sid_name(F, &strings, F->gsid[g], buf, sizeof buf)) + 1;
        F->pool = of_alloc(need + 1);
        F->byname = of_alloc((size_t)F->ngl * sizeof *F->byname);
        if (F->pool && F->byname) {
            size_t at = 0;
            for (int g = 0; g < F->ngl; g++) {
                const char *nm = cff_sid_name(F, &strings, F->gsid[g], buf, sizeof buf);
                size_t l = strlen(nm);
                memcpy(F->pool + at, nm, l + 1);
                F->byname[F->nbyname++] = (of_gname_t){ F->pool + at, g };
                at += l + 1;
            }
        }
        index_names(F);
        /* the built-in encoding */
        for (int c = 0; c < 256; c++) F->benc[c] = -1;
        if (T.encoding == 0) {
            F->benc_std = true;
            for (int c = 0; c < 256; c++) if (OF_STD_ENC[c]) F->benc[c] = (int16_t)of_fprog_by_name(F, std_name(OF_STD_ENC[c]));
        } else if (T.encoding > 1 && T.encoding < len) {
            uint32_t p = base + T.encoding;
            int fmt = (int)u8at(F, p++);
            if ((fmt & 0x7F) == 0) {
                int nc = (int)u8at(F, p++);
                for (int i = 0; i < nc && i + 1 < F->ngl; i++) F->benc[u8at(F, p++)] = (int16_t)(i + 1);
            } else if ((fmt & 0x7F) == 1) {
                int nr = (int)u8at(F, p++), g = 1;
                for (int r = 0; r < nr; r++) {
                    int first = (int)u8at(F, p), left = (int)u8at(F, p + 1);
                    p += 2;
                    for (int k = 0; k <= left && g < F->ngl; k++) if (first + k < 256) F->benc[first + k] = (int16_t)g++;
                }
            }
            if (fmt & 0x80) {
                int ns = (int)u8at(F, p++);
                for (int i = 0; i < ns; i++, p += 3) {
                    int code = (int)u8at(F, p);
                    uint32_t sid = u16at(F, p + 1);
                    for (int g = 0; g < F->ngl; g++) if (F->gsid[g] == sid) { F->benc[code] = (int16_t)g; break; }
                }
            }
        }
    }
    return true;
}

static int subr_bias(uint32_t n) { return n < 1240 ? 107 : n < 33900 ? 1131 : 32768; }

typedef struct {
    const of_fprog_t *F;
    pen_t   *P;
    float    st[48];
    int      sp, nstems;
    float    x, y;
    bool     width_done, ended;
    const of_cffix_t *lsub;
    float    trans[32];
    int      depth;             /* seac's glyphs in glyphs */
} t2_t;

static void t2_run(t2_t *T, uint32_t s, uint32_t e, int depth);
static void cff_glyph(const of_fprog_t *F, int gid, of_mtx_t gm, of_path_t *out, int depth);

static void t2_seac(t2_t *T, float adx, float ady, int bchar, int achar)
{
    if (bchar < 0 || bchar > 255 || achar < 0 || achar > 255) return;
    int bg = of_fprog_by_name(T->F, std_name(OF_STD_ENC[bchar])), ag = of_fprog_by_name(T->F, std_name(OF_STD_ENC[achar]));
    pen_t *P = T->P;
    if (bg >= 0) cff_glyph(T->F, bg, P->m, P->out, T->depth + 1);
    if (ag >= 0) cff_glyph(T->F, ag, of_mtx_mul((of_mtx_t){ 1, 0, 0, 1, adx, ady }, P->m), P->out, T->depth + 1);
}

static void t2_curve(t2_t *T, float dx1, float dy1, float dx2, float dy2, float dx3, float dy3)
{
    float x1 = T->x + dx1, y1 = T->y + dy1, x2 = x1 + dx2, y2 = y1 + dy2;
    T->x = x2 + dx3;
    T->y = y2 + dy3;
    pen_cubic(T->P, x1, y1, x2, y2, T->x, T->y);
}

static void t2_run(t2_t *T, uint32_t s, uint32_t e, int depth)
{
    const of_fprog_t *F = T->F;
    if (depth > 10) return;
    uint32_t p = s;
    float *st = T->st;
    while (p < e && !T->ended) {
        int b = F->data[p];
        if (b >= 32 || b == 28) {
            float v;
            if (b == 28) { v = (float)s16at(F, p + 1); p += 3; }
            else if (b <= 246) { v = (float)(b - 139); p++; }
            else if (b <= 250) { v = (float)((b - 247) * 256 + (int)u8at(F, p + 1) + 108); p += 2; }
            else if (b <= 254) { v = (float)(-(b - 251) * 256 - (int)u8at(F, p + 1) - 108); p += 2; }
            else { v = (float)(int32_t)u32at(F, p + 1) / 65536.0f; p += 5; }
            if (T->sp < 48) st[T->sp++] = v;
            continue;
        }
        p++;
        int n = T->sp, i = 0;
        switch (b) {
        case 1: case 3: case 18: case 23:
            if ((n & 1) && !T->width_done) i = 1;
            T->nstems += (n - i) / 2;
            T->width_done = true;
            T->sp = 0;
            break;
        case 19: case 20:
            if ((n & 1) && !T->width_done) i = 1;
            T->nstems += (n - i) / 2;
            T->width_done = true;
            p += (uint32_t)(T->nstems + 7) / 8;
            T->sp = 0;
            break;
        case 21:
            if (n > 2 && !T->width_done) i = 1;
            T->width_done = true;
            if (n - i >= 2) { T->x += st[i]; T->y += st[i + 1]; }
            pen_move(T->P, T->x, T->y);
            T->sp = 0;
            break;
        case 22: case 4:
            if (n > 1 && !T->width_done) i = 1;
            T->width_done = true;
            if (n - i >= 1) { if (b == 22) T->x += st[i]; else T->y += st[i]; }
            pen_move(T->P, T->x, T->y);
            T->sp = 0;
            break;
        case 5:
            for (; i + 1 < n; i += 2) { T->x += st[i]; T->y += st[i + 1]; pen_line(T->P, T->x, T->y); }
            T->sp = 0;
            break;
        case 6: case 7: {
            bool h = b == 6;
            for (; i < n; i++, h = !h) {
                if (h) T->x += st[i];
                else T->y += st[i];
                pen_line(T->P, T->x, T->y);
            }
            T->sp = 0;
            break;
        }
        case 8:
            for (; i + 5 < n; i += 6) t2_curve(T, st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
            T->sp = 0;
            break;
        case 24:
            for (; i + 5 < n - 2; i += 6) t2_curve(T, st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
            if (i + 1 < n) { T->x += st[i]; T->y += st[i + 1]; pen_line(T->P, T->x, T->y); }
            T->sp = 0;
            break;
        case 25:
            for (; i + 1 < n - 6; i += 2) { T->x += st[i]; T->y += st[i + 1]; pen_line(T->P, T->x, T->y); }
            if (i + 5 < n) t2_curve(T, st[i], st[i + 1], st[i + 2], st[i + 3], st[i + 4], st[i + 5]);
            T->sp = 0;
            break;
        case 26: {
            float dx1 = 0;
            if (n & 1) dx1 = st[i++];
            for (; i + 3 < n; i += 4) { t2_curve(T, dx1, st[i], st[i + 1], st[i + 2], 0, st[i + 3]); dx1 = 0; }
            T->sp = 0;
            break;
        }
        case 27: {
            float dy1 = 0;
            if (n & 1) dy1 = st[i++];
            for (; i + 3 < n; i += 4) { t2_curve(T, st[i], dy1, st[i + 1], st[i + 2], st[i + 3], 0); dy1 = 0; }
            T->sp = 0;
            break;
        }
        case 30: case 31: {
            bool h = b == 31;
            while (i + 3 < n) {
                float last = (n - i == 5) ? st[i + 4] : 0;
                if (h) t2_curve(T, st[i], 0, st[i + 1], st[i + 2], last, st[i + 3]);
                else t2_curve(T, 0, st[i], st[i + 1], st[i + 2], st[i + 3], last);
                i += (n - i == 5) ? 5 : 4;
                h = !h;
            }
            T->sp = 0;
            break;
        }
        case 10: case 29: {
            if (!n) break;
            const of_cffix_t *x = b == 10 ? T->lsub : &F->gsubr;
            if (!x || !x->count) { T->sp = 0; break; }
            int k = si(st[--T->sp]) + subr_bias(x->count);
            uint32_t ss, se;
            if (k >= 0 && cff_get(F, x, (uint32_t)k, &ss, &se)) t2_run(T, ss, se, depth + 1);
            break;
        }
        case 11: return;
        case 14:
            if (n >= 4 && T->depth < 2) {
                int k = n - 4;
                pen_end(T->P);
                t2_seac(T, st[k], st[k + 1], si(st[k + 2]), si(st[k + 3]));
            }
            pen_end(T->P);
            T->ended = true;
            return;
        case 12: {
            int b2 = (int)u8at(F, p++);
            float *a = st;
            switch (b2) {
            case 35:
                if (n >= 12) { t2_curve(T, a[0], a[1], a[2], a[3], a[4], a[5]); t2_curve(T, a[6], a[7], a[8], a[9], a[10], a[11]); }
                T->sp = 0;
                break;
            case 34:
                if (n >= 7) {
                    float y0 = T->y;
                    t2_curve(T, a[0], 0, a[1], a[2], a[3], 0);
                    t2_curve(T, a[4], 0, a[5], y0 - T->y, a[6], 0);
                }
                T->sp = 0;
                break;
            case 36:
                if (n >= 9) {
                    float y0 = T->y;
                    t2_curve(T, a[0], a[1], a[2], a[3], a[4], 0);
                    t2_curve(T, a[5], 0, a[6], a[7], a[8], y0 - (T->y + a[7]));
                }
                T->sp = 0;
                break;
            case 37:
                if (n >= 11) {
                    float dx = 0, dy = 0;
                    for (int k = 0; k < 10; k += 2) { dx += a[k]; dy += a[k + 1]; }
                    float x0 = T->x, y0 = T->y;
                    t2_curve(T, a[0], a[1], a[2], a[3], a[4], a[5]);
                    float lx, ly;
                    if (fabsf(dx) > fabsf(dy)) { lx = a[10]; ly = y0 - (T->y + a[7] + a[9]); }
                    else { lx = x0 - (T->x + a[6] + a[8]); ly = a[10]; }
                    t2_curve(T, a[6], a[7], a[8], a[9], lx, ly);
                }
                T->sp = 0;
                break;
            case 9: if (n >= 1) a[n - 1] = fabsf(a[n - 1]); break;
            case 10: if (n >= 2) { a[n - 2] += a[n - 1]; T->sp--; } break;
            case 11: if (n >= 2) { a[n - 2] -= a[n - 1]; T->sp--; } break;
            case 12: if (n >= 2) { a[n - 2] = a[n - 1] != 0 ? a[n - 2] / a[n - 1] : 0; T->sp--; } break;
            case 14: if (n >= 1) a[n - 1] = -a[n - 1]; break;
            case 18: if (n >= 1) T->sp--; break;
            case 24: if (n >= 2) { a[n - 2] *= a[n - 1]; T->sp--; } break;
            case 26: if (n >= 1) a[n - 1] = a[n - 1] > 0 ? sqrtf(a[n - 1]) : 0; break;
            case 27: if (n >= 1 && n < 48) { a[n] = a[n - 1]; T->sp++; } break;
            case 28: if (n >= 2) { float t = a[n - 1]; a[n - 1] = a[n - 2]; a[n - 2] = t; } break;
            case 20: if (n >= 2) { int k = si(a[n - 1]); if (k >= 0 && k < 32) T->trans[k] = a[n - 2]; T->sp -= 2; } break;
            case 21: if (n >= 1) { int k = si(a[n - 1]); a[n - 1] = k >= 0 && k < 32 ? T->trans[k] : 0; } break;
            default: T->sp = 0; break;
            }
            break;
        }
        default:
            T->sp = 0;
            break;
        }
    }
}

static int fd_of(const of_fprog_t *F, int gid);

static void cff_glyph(const of_fprog_t *F, int gid, of_mtx_t gm, of_path_t *out, int depth)
{
    uint32_t s, e;
    if (!cff_get(F, &F->cs, (uint32_t)gid, &s, &e)) return;
    pen_t P = { out, gm, false };
    t2_t T = { .F = F, .P = &P, .depth = depth };
    int fd = F->nfd ? fd_of(F, gid) : -1;
    T.lsub = fd >= 0 && fd < F->nfd ? &F->fdsub[fd] : &F->lsubr;
    t2_run(&T, s, e, 0);
    pen_end(&P);
}

static int fd_of(const of_fprog_t *F, int gid)
{
    if (!F->fdselect) return 0;
    uint32_t p = F->fdselect;
    int fmt = (int)u8at(F, p);
    if (fmt == 0) return (int)u8at(F, p + 1 + (uint32_t)gid);
    if (fmt == 3) {
        uint32_t nr = u16at(F, p + 1);
        for (uint32_t r = 0; r < nr && r < 65536; r++) {
            uint32_t q = p + 3 + 3 * r;
            uint32_t first = u16at(F, q), next = u16at(F, q + 3);
            if ((uint32_t)gid >= first && (uint32_t)gid < next) return (int)u8at(F, q + 2);
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Type 1                                                                      */
/* -------------------------------------------------------------------------- */

static void t1_decrypt(uint8_t *d, size_t n, uint16_t r)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = d[i];
        d[i] = (uint8_t)(c ^ (r >> 8));
        r = (uint16_t)((c + r) * 52845u + 22719u);
    }
}

static bool is_sp(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0; }
static bool is_hex(uint8_t c) { return (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
static int hexv(uint8_t c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

typedef struct { const uint8_t *d; size_t n, p; } tk_t;

/* the next token: a name ("/x" without the slash), a number or a word; false at the end */
static bool t1_tok(tk_t *T, char *out, size_t cap, bool *is_name)
{
    while (T->p < T->n && is_sp(T->d[T->p])) T->p++;
    if (T->p >= T->n) return false;
    *is_name = false;
    if (T->d[T->p] == '/') { *is_name = true; T->p++; }
    size_t k = 0;
    if (T->p < T->n && strchr("[]{}", T->d[T->p]) && !*is_name) { out[k++] = (char)T->d[T->p++]; out[k] = 0; return true; }
    while (T->p < T->n && !is_sp(T->d[T->p]) && !strchr("/[]{}()<>", T->d[T->p])) {
        if (k + 1 < cap) out[k++] = (char)T->d[T->p];
        T->p++;
    }
    if (!k && T->p < T->n && !*is_name) T->p++;    /* a delimiter of no interest */
    out[k] = 0;
    return true;
}

static const uint8_t *find_mem(const uint8_t *h, size_t n, const char *s)
{
    size_t l = strlen(s);
    for (size_t i = 0; i + l <= n; i++) if (h[i] == (uint8_t)s[0] && !memcmp(h + i, s, l)) return h + i;
    return NULL;
}

static bool t1_load(of_fprog_t *F)
{
    F->kind = FP_T1;
    /* a PFB's segment headers, if they came along */
    if (F->n > 6 && F->data[0] == 0x80 && F->data[1] == 1) {
        uint8_t *o = of_alloc(F->n);
        size_t on = 0, p = 0;
        if (!o) return false;
        while (p + 6 <= F->n && F->data[p] == 0x80 && (F->data[p + 1] == 1 || F->data[p + 1] == 2)) {
            uint32_t l = (uint32_t)F->data[p + 2] | (uint32_t)F->data[p + 3] << 8 | (uint32_t)F->data[p + 4] << 16 | (uint32_t)F->data[p + 5] << 24;
            p += 6;
            if (l > F->n - p) l = (uint32_t)(F->n - p);
            memcpy(o + on, F->data + p, l);
            on += l;
            p += l;
        }
        of_free(F->data);
        F->data = o;
        F->n = on;
    }
    const uint8_t *ee = find_mem(F->data, F->n, "eexec");
    if (!ee) return false;
    size_t clear = (size_t)(ee - F->data);
    /* the matrix and the encoding, in the clear part */
    F->fm = (of_mtx_t){ 0.001f, 0, 0, 0.001f, 0, 0 };
    const uint8_t *fmp = find_mem(F->data, clear, "/FontMatrix");
    if (fmp) {
        tk_t T = { fmp + 11, clear - (size_t)(fmp + 11 - F->data), 0 };
        char t[64];
        bool nm;
        float v[6];
        int k = 0;
        while (k < 6 && t1_tok(&T, t, sizeof t, &nm)) {
            if (t[0] == '[' || !t[0]) continue;
            if (t[0] == ']') break;
            v[k++] = strtof(t, NULL);
        }
        if (k == 6 && (fabsf(v[0]) > 1e-6f || fabsf(v[1]) > 1e-6f)) F->fm = (of_mtx_t){ v[0], v[1], v[2], v[3], v[4], v[5] };
    }
    char *enc_names[256] = { 0 };
    bool std = true;
    const uint8_t *ep = find_mem(F->data, clear, "/Encoding");
    char *encpool = NULL;
    if (ep) {
        tk_t T = { ep + 9, clear - (size_t)(ep + 9 - F->data), 0 };
        char t[64];
        bool nm;
        if (t1_tok(&T, t, sizeof t, &nm) && strcmp(t, "StandardEncoding")) {
            std = false;
            encpool = of_alloc(256 * 32);
            /* dup <code> /<name> put, up to "def" */
            int code = -1, used = 0;
            char prev[64] = "";
            while (encpool && t1_tok(&T, t, sizeof t, &nm)) {
                if (!nm && !strcmp(t, "def")) break;
                if (nm && code >= 0 && code < 256 && !strcmp(prev, "dupnum") && used < 256) {
                    char *slot = encpool + 32 * used++;
                    snprintf(slot, 32, "%.31s", t);
                    enc_names[code] = slot;
                }
                if (!nm && !strcmp(prev, "dup")) { code = (int)strtol(t, NULL, 10); snprintf(prev, sizeof prev, "dupnum"); continue; }
                snprintf(prev, sizeof prev, "%s", nm ? "/" : t);
            }
        }
    }
    /* the encrypted part: binary or hex */
    size_t p = clear + 5;
    while (p < F->n && is_sp(F->data[p])) p++;
    if (p + 4 > F->n) { of_free(encpool); return false; }
    uint8_t *priv;
    size_t pn = 0;
    bool hex = is_hex(F->data[p]) && is_hex(F->data[p + 1]) && is_hex(F->data[p + 2]) && is_hex(F->data[p + 3]);
    priv = of_alloc(F->n - p + 1);
    if (!priv) { of_free(encpool); return false; }
    if (hex) {
        int hi = -1;
        for (size_t i = p; i < F->n; i++) {
            uint8_t c = F->data[i];
            if (is_sp(c)) continue;
            if (!is_hex(c)) break;
            if (hi < 0) hi = hexv(c);
            else { priv[pn++] = (uint8_t)(hi << 4 | hexv(c)); hi = -1; }
        }
    } else {
        pn = F->n - p;
        memcpy(priv, F->data + p, pn);
    }
    t1_decrypt(priv, pn, 55665);
    /* the private part replaces the file: offsets below are into it */
    of_free(F->data);
    F->data = priv;
    F->n = pn;
    int leniv = 4;
    const uint8_t *lv = find_mem(priv, pn, "/lenIV");
    if (lv) leniv = (int)strtol((const char *)lv + 6, NULL, 10);
    if (leniv < -1 || leniv > 16) leniv = 4;
    /* Subrs: dup i n RD <n bytes> NP */
    const uint8_t *sp = find_mem(priv, pn, "/Subrs");
    char t[64];
    bool nm;
    if (sp) {
        tk_t T = { priv, pn, (size_t)(sp - priv) + 6 };
        if (t1_tok(&T, t, sizeof t, &nm)) {
            int cnt = (int)strtol(t, NULL, 10);
            if (cnt > 0 && cnt < 65536) F->subr = of_alloc((size_t)cnt * sizeof *F->subr);
            if (F->subr) F->nsubr = cnt;
            for (int guard = 0; F->subr && guard < cnt * 8 + 8; guard++) {
                size_t save = T.p;
                if (!t1_tok(&T, t, sizeof t, &nm)) break;
                if (nm && !strcmp(t, "CharStrings")) { T.p = save; break; }
                if (strcmp(t, "dup")) continue;
                char a[32], b[32], rd[8];
                if (!t1_tok(&T, a, sizeof a, &nm) || !t1_tok(&T, b, sizeof b, &nm) || !t1_tok(&T, rd, sizeof rd, &nm)) break;
                long idx = strtol(a, NULL, 10), len = strtol(b, NULL, 10);
                T.p++;                                  /* the one space */
                if (len < 0 || (size_t)len > pn - T.p) break;
                if (idx >= 0 && idx < cnt) {
                    t1_decrypt(priv + T.p, (size_t)len, 4330);
                    int skip = leniv < 0 ? 0 : leniv;
                    if (len >= skip) { F->subr[idx].off = (uint32_t)(T.p + (size_t)skip); F->subr[idx].len = (uint32_t)(len - skip); }
                }
                T.p += (size_t)len;
            }
        }
    }
    /* CharStrings: /name n RD <n bytes> ND */
    const uint8_t *cp = find_mem(priv, pn, "/CharStrings");
    if (!cp) { of_free(encpool); return false; }
    tk_t T = { priv, pn, (size_t)(cp - priv) + 12 };
    int cnt = 0;
    if (t1_tok(&T, t, sizeof t, &nm)) cnt = (int)strtol(t, NULL, 10);
    if (cnt <= 0 || cnt > 65535) cnt = 4096;
    F->byname = of_alloc((size_t)cnt * sizeof *F->byname);
    F->t1cs = of_alloc((size_t)cnt * sizeof *F->t1cs);
    F->pool = of_alloc((size_t)cnt * 32);
    if (!F->byname || !F->t1cs || !F->pool) { of_free(encpool); return false; }
    while (F->ngl < cnt && t1_tok(&T, t, sizeof t, &nm)) {
        if (!nm) { if (!strcmp(t, "end")) break; continue; }
        char a[32], rd[8];
        bool n2;
        size_t save = T.p;
        if (!t1_tok(&T, a, sizeof a, &n2) || n2) { T.p = save; continue; }
        char *e;
        long len = strtol(a, &e, 10);
        if (*e) { T.p = save; continue; }
        if (!t1_tok(&T, rd, sizeof rd, &n2)) break;
        T.p++;
        if (len < 0 || (size_t)len > pn - T.p) break;
        t1_decrypt(priv + T.p, (size_t)len, 4330);
        int skip = leniv < 0 ? 0 : leniv;
        char *slot = F->pool + 32 * F->ngl;
        snprintf(slot, 32, "%.31s", t);
        F->byname[F->nbyname++] = (of_gname_t){ slot, F->ngl };
        F->t1cs[F->ngl].off = (uint32_t)(T.p + (size_t)(len >= skip ? skip : 0));
        F->t1cs[F->ngl].len = (uint32_t)(len >= skip ? len - skip : 0);
        F->ngl++;
        T.p += (size_t)len;
    }
    index_names(F);
    for (int c = 0; c < 256; c++) {
        const char *n = std ? (OF_STD_ENC[c] ? std_name(OF_STD_ENC[c]) : NULL) : enc_names[c];
        F->benc[c] = (int16_t)(n ? of_fprog_by_name(F, n) : -1);
    }
    F->benc_std = std;
    of_free(encpool);
    return F->ngl > 0;
}

typedef struct {
    const of_fprog_t *F;
    pen_t   *P;
    float    st[32];
    int      sp;
    float    ps[16];
    int      nps;
    float    x, y, sbx;
    bool     flex, ended;
    of_pt_t  fp[8];
    int      nfp;
} t1s_t;

static void t1_run(t1s_t *T, uint32_t s, uint32_t len, int depth);

static void t1_glyph(const of_fprog_t *F, int gid, of_mtx_t m, of_path_t *out)
{
    if (gid < 0 || gid >= F->ngl) return;
    pen_t P = { out, m, false };
    t1s_t T = { .F = F, .P = &P };
    t1_run(&T, F->t1cs[gid].off, F->t1cs[gid].len, 0);
    pen_end(&P);
}

static void t1_run(t1s_t *T, uint32_t s, uint32_t len, int depth)
{
    const of_fprog_t *F = T->F;
    if (depth > 10 || s > F->n || len > F->n - s) return;
    uint32_t p = s, e = s + len;
    float *st = T->st;
    while (p < e && !T->ended) {
        int v = F->data[p++];
        if (v >= 32) {
            float x;
            if (v <= 246) x = (float)(v - 139);
            else if (v <= 250) x = (float)((v - 247) * 256 + (int)u8at(F, p++) + 108);
            else if (v <= 254) x = (float)(-(v - 251) * 256 - (int)u8at(F, p++) - 108);
            else { x = (float)(int32_t)u32at(F, p); p += 4; }
            if (T->sp < 32) st[T->sp++] = x;
            continue;
        }
        int n = T->sp;
        switch (v) {
        case 13:            /* hsbw */
            if (n >= 2) { T->sbx = st[0]; T->x = st[0]; T->y = 0; }
            T->sp = 0;
            break;
        case 21:
            if (n >= 2) {
                T->x += st[n - 2]; T->y += st[n - 1];
                if (!T->flex) pen_move(T->P, T->x, T->y);
            }
            T->sp = 0;
            break;
        case 22: if (n >= 1) { T->x += st[n - 1]; if (!T->flex) pen_move(T->P, T->x, T->y); } T->sp = 0; break;
        case 4: if (n >= 1) { T->y += st[n - 1]; if (!T->flex) pen_move(T->P, T->x, T->y); } T->sp = 0; break;
        case 5: if (n >= 2) { T->x += st[0]; T->y += st[1]; pen_line(T->P, T->x, T->y); } T->sp = 0; break;
        case 6: if (n >= 1) { T->x += st[0]; pen_line(T->P, T->x, T->y); } T->sp = 0; break;
        case 7: if (n >= 1) { T->y += st[0]; pen_line(T->P, T->x, T->y); } T->sp = 0; break;
        case 8:
            if (n >= 6) {
                float x1 = T->x + st[0], y1 = T->y + st[1], x2 = x1 + st[2], y2 = y1 + st[3];
                T->x = x2 + st[4]; T->y = y2 + st[5];
                pen_cubic(T->P, x1, y1, x2, y2, T->x, T->y);
            }
            T->sp = 0;
            break;
        case 30:
            if (n >= 4) {
                float x1 = T->x, y1 = T->y + st[0], x2 = x1 + st[1], y2 = y1 + st[2];
                T->x = x2 + st[3]; T->y = y2;
                pen_cubic(T->P, x1, y1, x2, y2, T->x, T->y);
            }
            T->sp = 0;
            break;
        case 31:
            if (n >= 4) {
                float x1 = T->x + st[0], y1 = T->y, x2 = x1 + st[1], y2 = y1 + st[2];
                T->x = x2; T->y = y2 + st[3];
                pen_cubic(T->P, x1, y1, x2, y2, T->x, T->y);
            }
            T->sp = 0;
            break;
        case 9: of_path_close(T->P->out); T->sp = 0; break;
        case 10:
            if (n >= 1) {
                int k = si(st[--T->sp]);
                if (k >= 0 && k < F->nsubr && F->subr[k].len) t1_run(T, F->subr[k].off, F->subr[k].len, depth + 1);
            }
            break;
        case 11: return;
        case 14: pen_end(T->P); T->ended = true; return;
        case 1: case 3: T->sp = 0; break;
        case 12: {
            int w = (int)u8at(F, p++);
            switch (w) {
            case 7:         /* sbw */
                if (n >= 4) { T->sbx = st[0]; T->x = st[0]; T->y = st[1]; }
                T->sp = 0;
                break;
            case 6:         /* seac */
                if (n >= 5) {
                    float asb = st[0], adx = st[1], ady = st[2];
                    int bc = si(st[3]), ac = si(st[4]);
                    pen_end(T->P);
                    if (bc >= 0 && bc < 256 && ac >= 0 && ac < 256) {
                        int bg = of_fprog_by_name(F, std_name(OF_STD_ENC[bc])), ag = of_fprog_by_name(F, std_name(OF_STD_ENC[ac]));
                        if (bg >= 0 && depth < 4) t1_glyph(F, bg, T->P->m, T->P->out);
                        if (ag >= 0 && depth < 4)
                            t1_glyph(F, ag, of_mtx_mul((of_mtx_t){ 1, 0, 0, 1, adx - asb, ady }, T->P->m), T->P->out);
                    }
                    T->ended = true;
                    return;
                }
                T->sp = 0;
                break;
            case 12: if (n >= 2) { st[n - 2] = st[n - 1] != 0 ? st[n - 2] / st[n - 1] : 0; T->sp--; } break;
            case 16: {      /* callothersubr */
                if (n < 2) { T->sp = 0; break; }
                int oth = si(st[n - 1]), cnt = si(st[n - 2]);
                T->sp -= 2;
                if (cnt < 0 || cnt > T->sp) cnt = T->sp;
                float args[16];
                int na = cnt > 16 ? 16 : cnt;
                for (int k = 0; k < na; k++) args[k] = st[T->sp - cnt + k];
                T->sp -= cnt;
                T->nps = 0;
                if (oth == 1) { T->flex = true; T->nfp = 0; }
                else if (oth == 2) { if (T->flex && T->nfp < 8) T->fp[T->nfp++] = (of_pt_t){ T->x, T->y }; }
                else if (oth == 0) {
                    if (T->flex && T->nfp >= 7) {
                        pen_cubic(T->P, T->fp[1].x, T->fp[1].y, T->fp[2].x, T->fp[2].y, T->fp[3].x, T->fp[3].y);
                        pen_cubic(T->P, T->fp[4].x, T->fp[4].y, T->fp[5].x, T->fp[5].y, T->fp[6].x, T->fp[6].y);
                        T->x = T->fp[6].x; T->y = T->fp[6].y;
                    }
                    T->flex = false;
                    T->ps[0] = T->y; T->ps[1] = T->x;
                    T->nps = 2;
                } else {
                    /* the others' arguments come back, the last first */
                    for (int k = na - 1; k >= 0 && T->nps < 16; k--) T->ps[T->nps++] = args[k];
                    if (oth == 3) { T->nps = 1; T->ps[0] = 3; }
                }
                break;
            }
            case 17:        /* pop */
                if (T->sp < 32) st[T->sp++] = T->nps > 0 ? T->ps[--T->nps] : 0;
                break;
            case 33:        /* setcurrentpoint */
                if (n >= 2) { T->x = st[n - 2]; T->y = st[n - 1]; }
                T->sp = 0;
                break;
            default: T->sp = 0; break;
            }
            break;
        }
        default: T->sp = 0; break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* The public face                                                             */
/* -------------------------------------------------------------------------- */

bool of_fprog_load(of_fprog_t *F, uint8_t *data, size_t n, int kind)
{
    memset(F, 0, sizeof *F);
    F->data = data;
    F->n = n;
    for (int c = 0; c < 256; c++) F->benc[c] = -1;
    if (!data || n < 16) return false;
    bool ok = false;
    uint32_t tag = u32at(F, 0);
    bool sfnt = tag == 0x00010000 || tag == 0x74727565 /* true */ || tag == 0x4F54544F /* OTTO */;
    if (sfnt) {
        uint32_t cl = 0, cff = sfnt_table(F, "CFF ", &cl);
        if (cff) ok = cff_load(F, cff, cl);
        else ok = tt_load(F);
    } else if (kind == 3 || (data[0] == 1 && data[1] == 0 && data[2] >= 4)) {
        ok = cff_load(F, 0, (uint32_t)n);
    } else if (kind == 1 || data[0] == '%' || data[0] == 0x80) {
        ok = t1_load(F);
    } else if (kind == 2) {
        ok = tt_load(F);
    }
    if (!ok) { of_fprog_free(F); F->kind = FP_NONE; }
    return ok;
}

void of_fprog_free(of_fprog_t *F)
{
    of_free(F->data);
    of_free(F->gsid);
    of_free(F->subr);
    of_free(F->byname);
    of_free(F->byuni);
    of_free(F->bycid);
    of_free(F->pool);
    of_free(F->t1cs);
    memset(F, 0, sizeof *F);
}

int of_fprog_by_uni(const of_fprog_t *F, uint32_t cp)
{
    if (F->kind == FP_TT) {
        int g = cmap_lookup(F, F->c31, cp);
        return g;
    }
    return F->nbyuni ? guni_find(F->byuni, F->nbyuni, cp) : -1;
}

int of_fprog_by_code(const of_fprog_t *F, int code, bool symbolic)
{
    if (code < 0) return -1;
    if (F->kind == FP_TT) {
        int g = -1;
        if (F->c30) {
            g = cmap_lookup(F, F->c30, (uint32_t)code);
            for (int hi = 0xF000; g < 0 && hi <= 0xF200; hi += 0x100) g = cmap_lookup(F, F->c30, (uint32_t)(hi + code));
        }
        if (g < 0 && F->c10) g = cmap_lookup(F, F->c10, (uint32_t)code);
        if (g < 0 && symbolic && F->c31) g = cmap_lookup(F, F->c31, (uint32_t)code);
        if (g < 0 && !F->c30 && !F->c10 && !F->c31) g = code < F->ngl ? code : -1;   /* no cmap: the code is the glyph */
        return g;
    }
    return code < 256 ? F->benc[code] : -1;
}

int of_fprog_by_cid(const of_fprog_t *F, uint32_t cid)
{
    if (F->kind == FP_CFF && F->cid) return guni_find(F->bycid, F->nbycid, cid);
    return (int)cid < F->ngl ? (int)cid : -1;
}

float of_fprog_advance(const of_fprog_t *F, int gid)
{
    if (F->kind != FP_TT || !F->hmtx || !F->nhm || gid < 0) return 0;
    uint32_t i = (uint32_t)gid < F->nhm ? (uint32_t)gid : F->nhm - 1;
    return (float)u16at(F, F->hmtx + 4 * i) * F->fm.a;
}

bool of_fprog_outline(const of_fprog_t *F, int gid, of_mtx_t m, of_path_t *out)
{
    if (gid < 0 || gid >= F->ngl) return false;
    of_mtx_t gm = of_mtx_mul(F->fm, m);
    if (F->kind == FP_TT) {
        pen_t P = { out, gm, false };
        return tt_glyph(F, gid, (of_mtx_t){ 1, 0, 0, 1, 0, 0 }, &P, 0);
    }
    if (F->kind == FP_CFF) {
        cff_glyph(F, gid, gm, out, 0);
        return true;
    }
    if (F->kind == FP_T1) {
        t1_glyph(F, gid, gm, out);
        return true;
    }
    return false;
}
