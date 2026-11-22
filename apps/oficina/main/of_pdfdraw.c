/*
 * P4OS - Office: a PDF page drawn as it is printed.
 *
 * The page's content stream is run with its graphics state (matrices,
 * colours in every colour space the format has, lines with their caps,
 * joins and dashes, clipping paths, alpha), and painted into an RGB565
 * picture at any scale: paths are flattened and filled with anti-aliasing
 * (five rows of samples per pixel, each crossed exactly; rectangles
 * exactly), strokes become polygons, text is drawn with the outlines of the
 * fonts the file carries (of_pdffont.c; glyphs cached by size), or, for the
 * fonts it names and does not carry, with the app's own (a callback, so
 * the painter knows nothing of LVGL). Pictures: samples of any depth and
 * colour space, JPEG through the system's decoder, masks, soft masks and
 * colour keys, shrunk while read to the size they are shown at. Axial and
 * radial shadings, tiling patterns, Type 3 fonts, form XObjects, inline
 * pictures, and the annotations' appearances. What it does not do: the
 * mesh shadings, soft masks of groups, blend modes, JPEG 2000, JBIG2.
 */
#include "of_pdfd.h"
#include "of_pdfi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint16_t *aos_hal_image_decode(const char *path, uint32_t offset, uint32_t size, int max_w, int max_h, bool fill,
                               int *out_w, int *out_h, int *src_w, int *src_h);
void      aos_hal_image_free(void *px);
bool      of_cache_file(const char *name, const void *data, size_t n, char *out, size_t len);

#define SUB        5            /* rows of samples per pixel */
#define SUB_GLYPH  8
#define MAX_GS     48
#define MAX_DEPTH  10
#define GC_SLOTS   2048
#define GC_BYTES   (3u * 1024 * 1024)
#define IMG_SLOTS  12

static of_pdf_fallback_fn s_fallback;

void of_pdfr_fallback(of_pdf_fallback_fn fn) { s_fallback = fn; }

static inline float fnum(const pv_t *v, float def) { return (float)pv_num(v, def); }
/* a number from the file as an integer, never out of range */
static inline int si(float v) { return v > -1.0e9f && v < 1.0e9f ? (int)v : 0; }
static inline int pvi(const pv_t *v, double def) { double d = pv_num(v, def); return d > -1.0e9 && d < 1.0e9 ? (int)d : 0; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* -------------------------------------------------------------------------- */
/* Functions (types 0, 2, 3, 4)                                                */
/* -------------------------------------------------------------------------- */

enum { PS_NUM = 0, PS_JZ, PS_J, PS_ADD, PS_SUB, PS_MUL, PS_DIV, PS_IDIV, PS_MOD, PS_NEG, PS_ABS, PS_CEIL, PS_FLOOR,
       PS_ROUND, PS_TRUNC, PS_SQRT, PS_SIN, PS_COS, PS_ATAN, PS_EXP, PS_LN, PS_LOG, PS_CVI, PS_CVR, PS_EQ, PS_NE, PS_GT,
       PS_GE, PS_LT, PS_LE, PS_AND, PS_OR, PS_NOT, PS_XOR, PS_SHIFT, PS_TRUE, PS_FALSE, PS_POP, PS_EXCH, PS_DUP,
       PS_COPY, PS_INDEX, PS_ROLL, PS_NOP };

static const char *const PS_NAMES[] = { "", "", "", "add", "sub", "mul", "div", "idiv", "mod", "neg", "abs", "ceiling",
    "floor", "round", "truncate", "sqrt", "sin", "cos", "atan", "exp", "ln", "log", "cvi", "cvr", "eq", "ne", "gt", "ge",
    "lt", "le", "and", "or", "not", "xor", "bitshift", "true", "false", "pop", "exch", "dup", "copy", "index", "roll" };

typedef struct { uint8_t op; float v; int jmp; } psop_t;

typedef struct fn_s fn_t;
struct fn_s {
    const pv_t *key;
    int     type, m, n;
    float   dom[4], rng[16];
    bool    has_rng;
    /* 0 */
    int     size[2], bps;
    float   enc[4], dec[16];
    uint8_t *smp;
    size_t  nsmp;
    /* 2 */
    float   c0[8], c1[8], N;
    /* 3 */
    fn_t   *sub[16];
    int     nsub;
    float   bounds[16], encode[34];
    /* 4 */
    psop_t *code;
    int     ncode;
    fn_t   *next;
};

typedef struct { fn_t *f[8]; int nf; } fnset_t;

typedef struct csi_s csi_t;

struct of_pdfr {
    pdf_t   *P;
    char     path[300];
    pref_t  *pages;
    int      npages;
    fonts_t  fonts;
    struct rfont **rf;
    int      nrf;
    fn_t    *fns;
    csi_t   *css;
    uint32_t file_id;
    /* glyphs, by size */
    struct gce { const void *rf; int gid; int32_t k[6]; int16_t w, h, l, t; uint8_t *a; } *gc;
    int      ngc;
    size_t   gc_bytes;
    /* pictures of the page drawn last */
    struct img *img[IMG_SLOTS];
    int      nimg;
    int      img_page;
};

static void fn_free(fn_t *f)
{
    while (f) {
        fn_t *n = f->next;
        of_free(f->smp);
        of_free(f->code);
        of_free(f);
        f = n;
    }
}

static int ps_compile(lx_t *L, psop_t **code, int *n, int *cap, int depth)
{
    /* up to the "}" that closes this block */
    for (;;) {
        pv_t v;
        if (!pv_parse(NULL, L, &v)) return -1;
        if (*n + 2 > *cap) {
            int nc = *cap ? *cap * 2 : 64;
            psop_t *x = of_realloc(*code, (size_t)nc * sizeof *x);
            if (!x) { pv_free(&v); return -1; }
            *code = x;
            *cap = nc;
        }
        if (v.t == PV_INT || v.t == PV_REAL) { (*code)[(*n)++] = (psop_t){ PS_NUM, fnum(&v, 0), 0 }; continue; }
        if (v.t != PV_OP) { pv_free(&v); continue; }
        const char *o = v.s.s;
        if (!strcmp(o, "}")) { pv_free(&v); return 0; }
        if (!strcmp(o, "{")) {
            pv_free(&v);
            if (depth > 16) return -1;
            /* { A } if  /  { A } { B } ifelse */
            int jz = (*n)++;
            if (ps_compile(L, code, n, cap, depth + 1) < 0) return -1;
            size_t save = L->p;
            pv_t w;
            if (!pv_parse(NULL, L, &w)) return -1;
            if (w.t == PV_OP && !strcmp(w.s.s, "if")) {
                (*code)[jz] = (psop_t){ PS_JZ, 0, *n };
            } else if (w.t == PV_OP && !strcmp(w.s.s, "{")) {
                int j = (*n)++;
                (*code)[jz] = (psop_t){ PS_JZ, 0, *n };
                if (ps_compile(L, code, n, cap, depth + 1) < 0) { pv_free(&w); return -1; }
                (*code)[j] = (psop_t){ PS_J, 0, *n };
                pv_t e;
                if (pv_parse(NULL, L, &e)) pv_free(&e);     /* "ifelse" */
            } else {
                L->p = save;
                (*code)[jz] = (psop_t){ PS_NOP, 0, 0 };
            }
            pv_free(&w);
            continue;
        }
        uint8_t op = PS_NOP;
        for (int k = PS_ADD; k < PS_NOP; k++) if (!strcmp(o, PS_NAMES[k])) { op = (uint8_t)k; break; }
        pv_free(&v);
        (*code)[(*n)++] = (psop_t){ op, 0, 0 };
    }
}

static void ps_run(const fn_t *f, float *st, int *sp)
{
    int n = *sp;
    for (int pc = 0, guard = 0; pc < f->ncode && guard < 100000; pc++, guard++) {
        const psop_t *o = &f->code[pc];
        float a = n > 0 ? st[n - 1] : 0, b = n > 1 ? st[n - 2] : 0;
        switch (o->op) {
        case PS_NUM: if (n < 100) st[n++] = o->v; break;
        case PS_JZ: if (n > 0) n--; if (a == 0) pc = o->jmp - 1; break;
        case PS_J: pc = o->jmp - 1; break;
#define BIN(expr) if (n >= 2) { st[n - 2] = (expr); n--; } break;
        case PS_ADD: BIN(b + a)
        case PS_SUB: BIN(b - a)
        case PS_MUL: BIN(b * a)
        case PS_DIV: BIN(a != 0 ? b / a : 0)
        case PS_IDIV: BIN(si(a) ? (float)(si(b) / si(a)) : 0)
        case PS_MOD: BIN(si(a) ? (float)(si(b) % si(a)) : 0)
        case PS_EQ: BIN(b == a)
        case PS_NE: BIN(b != a)
        case PS_GT: BIN(b > a)
        case PS_GE: BIN(b >= a)
        case PS_LT: BIN(b < a)
        case PS_LE: BIN(b <= a)
        case PS_AND: BIN((float)(si(b) & si(a)))
        case PS_OR: BIN((float)(si(b) | si(a)))
        case PS_XOR: BIN((float)(si(b) ^ si(a)))
        case PS_SHIFT: BIN(a >= 0 ? (float)(int32_t)((uint32_t)si(b) << (si(a) & 31)) : (float)(si(b) >> (-si(a) & 31)))
        case PS_ATAN: BIN(atan2f(b, a) * 57.29578f < 0 ? atan2f(b, a) * 57.29578f + 360 : atan2f(b, a) * 57.29578f)
        case PS_EXP: BIN(b > 0 || a == (float)si(a) ? powf(b, a) : 0)
#undef BIN
#define UN(expr) if (n >= 1) st[n - 1] = (expr); break;
        case PS_NEG: UN(-a)
        case PS_ABS: UN(fabsf(a))
        case PS_CEIL: UN(ceilf(a))
        case PS_FLOOR: UN(floorf(a))
        case PS_ROUND: UN(floorf(a + 0.5f))
        case PS_TRUNC: UN((float)si(a))
        case PS_SQRT: UN(a > 0 ? sqrtf(a) : 0)
        case PS_SIN: UN(sinf(a * 0.0174533f))
        case PS_COS: UN(cosf(a * 0.0174533f))
        case PS_LN: UN(a > 0 ? logf(a) : 0)
        case PS_LOG: UN(a > 0 ? log10f(a) : 0)
        case PS_CVI: UN((float)si(a))
        case PS_CVR: UN(a)
        case PS_NOT: UN(a == 0 ? 1.0f : a == 1 ? 0.0f : (float)~si(a))
#undef UN
        case PS_TRUE: if (n < 100) st[n++] = 1; break;
        case PS_FALSE: if (n < 100) st[n++] = 0; break;
        case PS_POP: if (n) n--; break;
        case PS_EXCH: if (n >= 2) { st[n - 1] = b; st[n - 2] = a; } break;
        case PS_DUP: if (n && n < 100) { st[n] = a; n++; } break;
        case PS_COPY: {
            int k = si(a);
            if (n) n--;
            if (k > 0 && k <= n && n + k < 100) { for (int i = 0; i < k; i++) st[n + i] = st[n - k + i]; n += k; }
            break;
        }
        case PS_INDEX: { int k = si(a); if (n && k >= 0 && k < n - 1) st[n - 1] = st[n - 2 - k]; break; }
        case PS_ROLL: {
            int j = si(a), k = si(b);
            if (n >= 2) n -= 2;
            if (k > 0 && k <= n) {
                j %= k;
                if (j < 0) j += k;
                float t[100];
                for (int i = 0; i < k; i++) t[(i + j) % k] = st[n - k + i];
                memcpy(st + n - k, t, (size_t)k * sizeof *t);
            }
            break;
        }
        default: break;
        }
    }
    *sp = n;
}

static fn_t *fn_get(of_pdfr_t *R, const pv_t *ref, int depth);

static void fn_eval(const fn_t *f, const float *in, float *out)
{
    float x[2];
    for (int i = 0; i < f->m && i < 2; i++) x[i] = clampf(in[i], f->dom[2 * i], f->dom[2 * i + 1]);
    switch (f->type) {
    case 0: {
        /* linear between the samples (first input), nearest for a second one */
        int sz0 = f->size[0] > 0 ? f->size[0] : 1;
        float e = f->enc[0] + (x[0] - f->dom[0]) * (f->enc[1] - f->enc[0]) / (f->dom[1] - f->dom[0] != 0 ? f->dom[1] - f->dom[0] : 1);
        e = clampf(e, 0, (float)(sz0 - 1));
        int i0 = (int)e, i1 = i0 + 1 < sz0 ? i0 + 1 : i0;
        float t = e - (float)i0;
        int row = 0;
        if (f->m >= 2 && f->size[1] > 0) {
            float e2 = f->enc[2] + (x[1] - f->dom[2]) * (f->enc[3] - f->enc[2]) / (f->dom[3] - f->dom[2] != 0 ? f->dom[3] - f->dom[2] : 1);
            row = (int)(clampf(e2, 0, (float)(f->size[1] - 1)) + 0.5f) * sz0;
        }
        float mx = (float)((1u << f->bps) - 1);
        for (int j = 0; j < f->n; j++) {
            float s[2];
            for (int k = 0; k < 2; k++) {
                size_t idx = ((size_t)(row + (k ? i1 : i0)) * (size_t)f->n + (size_t)j) * (size_t)f->bps;
                uint32_t v = 0;
                if (f->bps == 8) v = idx / 8 < f->nsmp ? f->smp[idx / 8] : 0;
                else if (f->bps == 16) v = idx / 8 + 1 < f->nsmp ? (uint32_t)f->smp[idx / 8] << 8 | f->smp[idx / 8 + 1] : 0;
                else if (f->bps < 8) v = idx / 8 < f->nsmp ? (uint32_t)(f->smp[idx / 8] >> (8 - f->bps - (int)(idx % 8))) & ((1u << f->bps) - 1) : 0;
                else if (f->bps == 32) v = idx / 8 + 3 < f->nsmp ? (uint32_t)f->smp[idx / 8] << 24 | (uint32_t)f->smp[idx / 8 + 1] << 16 : 0;
                s[k] = f->dec[2 * j] + (float)v / (f->bps == 32 ? 4294967295.0f : mx) * (f->dec[2 * j + 1] - f->dec[2 * j]);
            }
            out[j] = s[0] + (s[1] - s[0]) * t;
        }
        break;
    }
    case 2: {
        float p = f->N == 1 ? x[0] : (x[0] > 0 ? powf(x[0], f->N) : 0);
        for (int j = 0; j < f->n; j++) out[j] = f->c0[j] + p * (f->c1[j] - f->c0[j]);
        break;
    }
    case 3: {
        int k = 0;
        while (k < f->nsub - 1 && x[0] >= f->bounds[k]) k++;
        float lo = k ? f->bounds[k - 1] : f->dom[0], hi = k < f->nsub - 1 ? f->bounds[k] : f->dom[1];
        float t = hi - lo != 0 ? (x[0] - lo) / (hi - lo) : 0;
        float e = f->encode[2 * k] + t * (f->encode[2 * k + 1] - f->encode[2 * k]);
        if (k < f->nsub && f->sub[k]) fn_eval(f->sub[k], &e, out);
        else for (int j = 0; j < f->n; j++) out[j] = 0;
        break;
    }
    case 4: {
        float st[104];
        int sp = 0;
        for (int i = 0; i < f->m && i < 4; i++) st[sp++] = in[i];
        ps_run(f, st, &sp);
        for (int j = 0; j < f->n; j++) out[j] = j < sp ? st[sp - f->n + j >= 0 ? sp - f->n + j : 0] : 0;
        break;
    }
    default:
        for (int j = 0; j < f->n; j++) out[j] = 0;
    }
    if (f->has_rng) for (int j = 0; j < f->n && j < 8; j++) out[j] = clampf(out[j], f->rng[2 * j], f->rng[2 * j + 1]);
}

static void arr_f(const pv_t *a, float *out, int max, int *got)
{
    int k = 0;
    for (int i = 0; a && a->t == PV_ARR && i < a->a.n && k < max; i++) out[k++] = fnum(&a->a.v[i], 0);
    if (got) *got = k;
}

static fn_t *fn_get(of_pdfr_t *R, const pv_t *ref, int depth)
{
    const pv_t *v = pdf_res(R->P, ref);
    if (!v || depth > 4 || (v->t != PV_DICT && v->t != PV_STREAM)) return NULL;
    for (fn_t *f = R->fns; f; f = f->next) if (f->key == v) return f;
    fn_t *f = of_alloc(sizeof *f);
    if (!f) return NULL;
    f->key = v;
    f->type = pvi(pdf_get(R->P, v, "FunctionType"), -1);
    int nd = 0, nr = 0;
    arr_f(pdf_get(R->P, v, "Domain"), f->dom, 4, &nd);
    if (nd < 2) { f->dom[0] = 0; f->dom[1] = 1; nd = 2; }
    f->m = nd / 2;
    arr_f(pdf_get(R->P, v, "Range"), f->rng, 16, &nr);
    f->has_rng = nr >= 2;
    f->n = nr / 2;
    if (f->type == 0) {
        int ns = 0;
        float sz[2] = { 0, 0 };
        arr_f(pdf_get(R->P, v, "Size"), sz, 2, &ns);
        f->size[0] = (int)sz[0];
        f->size[1] = (int)sz[1];
        f->bps = pvi(pdf_get(R->P, v, "BitsPerSample"), 8);
        int ne = 0, ndc = 0;
        arr_f(pdf_get(R->P, v, "Encode"), f->enc, 4, &ne);
        if (ne < 2) { f->enc[0] = 0; f->enc[1] = (float)(f->size[0] - 1); f->enc[2] = 0; f->enc[3] = (float)(f->size[1] - 1); }
        arr_f(pdf_get(R->P, v, "Decode"), f->dec, 16, &ndc);
        if (ndc < 2 * f->n) memcpy(f->dec, f->rng, sizeof f->dec);
        if (v->t == PV_STREAM) f->smp = pdf_stream(R->P, v, ref && ref->t == PV_REF ? ref->ref.num : 0, &f->nsmp, NULL);
        if (!f->smp || f->size[0] <= 0 || (f->bps != 1 && f->bps != 2 && f->bps != 4 && f->bps != 8 && f->bps != 16 && f->bps != 32)) f->type = -1;
    } else if (f->type == 2) {
        int a = 0, b = 0;
        arr_f(pdf_get(R->P, v, "C0"), f->c0, 8, &a);
        arr_f(pdf_get(R->P, v, "C1"), f->c1, 8, &b);
        if (!a) { f->c0[0] = 0; a = 1; }
        if (!b) { f->c1[0] = 1; b = 1; }
        f->n = a < b ? a : b;
        f->N = fnum(pdf_get(R->P, v, "N"), 1);
    } else if (f->type == 3) {
        const pv_t *fs = pdf_get(R->P, v, "Functions");
        for (int i = 0; fs && fs->t == PV_ARR && i < fs->a.n && i < 16; i++) f->sub[f->nsub++] = fn_get(R, &fs->a.v[i], depth + 1);
        arr_f(pdf_get(R->P, v, "Bounds"), f->bounds, 16, NULL);
        int ne = 0;
        arr_f(pdf_get(R->P, v, "Encode"), f->encode, 34, &ne);
        for (int k = ne / 2; k < f->nsub; k++) { f->encode[2 * k] = 0; f->encode[2 * k + 1] = 1; }
        if (!f->n && f->nsub && f->sub[0]) f->n = f->sub[0]->n;
    } else if (f->type == 4 && v->t == PV_STREAM) {
        size_t n;
        uint8_t *d = pdf_stream(R->P, v, ref && ref->t == PV_REF ? ref->ref.num : 0, &n, NULL);
        if (d) {
            lx_t L = { d, n, 0 };
            pv_t o;
            /* the opening brace */
            if (pv_parse(NULL, &L, &o)) {
                pv_free(&o);
                int cap = 0;
                if (ps_compile(&L, &f->code, &f->ncode, &cap, 0) < 0 && !f->ncode) f->type = -1;
            }
            of_free(d);
        }
    }
    if (f->n > 8) f->n = 8;
    if (f->n <= 0) f->n = 1;
    f->next = R->fns;
    R->fns = f;
    return f;
}

/* /Function: one, or an array of one-output functions */
static void fnset_get(of_pdfr_t *R, const pv_t *ref, fnset_t *S)
{
    S->nf = 0;
    const pv_t *v = pdf_res(R->P, ref);
    if (v && v->t == PV_ARR) {
        for (int i = 0; i < v->a.n && S->nf < 8; i++) { fn_t *f = fn_get(R, &v->a.v[i], 0); if (f) S->f[S->nf++] = f; }
    } else if (v) {
        fn_t *f = fn_get(R, ref, 0);
        if (f) S->f[S->nf++] = f;
    }
}

static int fnset_eval(const fnset_t *S, const float *in, float *out)
{
    int k = 0;
    for (int i = 0; i < S->nf; i++) {
        float o[8];
        fn_eval(S->f[i], in, o);
        for (int j = 0; j < S->f[i]->n && k < 8; j++) out[k++] = o[j];
    }
    return k;
}

/* -------------------------------------------------------------------------- */
/* Colour spaces                                                               */
/* -------------------------------------------------------------------------- */

enum { CS_GRAY = 0, CS_RGB, CS_CMYK, CS_LAB, CS_IDX, CS_SEP, CS_PAT };

struct csi_s {
    const pv_t *key;
    int      kind, n;
    /* Indexed: the table as RGB */
    uint8_t *lut;
    int      hival;
    /* Separation, DeviceN: the tint function into the alternate */
    fnset_t  fn;
    csi_t   *alt;
    /* Lab */
    float    wp[3], range[4];
    /* Pattern: its underlying space */
    csi_t   *under;
    csi_t   *next;
};

static csi_t CS_DEV_GRAY = { .kind = CS_GRAY, .n = 1 }, CS_DEV_RGB = { .kind = CS_RGB, .n = 3 },
             CS_DEV_CMYK = { .kind = CS_CMYK, .n = 4 }, CS_DEV_PAT = { .kind = CS_PAT, .n = 0 };

static void to_rgb(const csi_t *cs, const float *c, float out[3]);

static void lab_rgb(const csi_t *cs, const float *c, float out[3])
{
    float L = c[0], a = c[1], b = c[2];
    float fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
#define G(t) ((t) > 6.0f / 29 ? (t) * (t) * (t) : 3 * (6.0f / 29) * (6.0f / 29) * ((t) - 4.0f / 29))
    float X = cs->wp[0] * G(fx), Y = cs->wp[1] * G(fy), Z = cs->wp[2] * G(fz);
#undef G
    float r = 3.1339f * X - 1.6169f * Y - 0.4906f * Z;
    float g = -0.9788f * X + 1.9161f * Y + 0.0335f * Z;
    float bl = 0.0719f * X - 0.2290f * Y + 1.4052f * Z;
    float v[3] = { r, g, bl };
    for (int i = 0; i < 3; i++) {
        float x = clampf(v[i], 0, 1);
        out[i] = x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1 / 2.4f) - 0.055f;
    }
}

static void to_rgb(const csi_t *cs, const float *c, float out[3])
{
    switch (cs->kind) {
    case CS_GRAY: out[0] = out[1] = out[2] = clampf(c[0], 0, 1); break;
    case CS_RGB: for (int i = 0; i < 3; i++) out[i] = clampf(c[i], 0, 1); break;
    case CS_CMYK: {
        float k = clampf(c[3], 0, 1);
        for (int i = 0; i < 3; i++) out[i] = (1 - clampf(c[i], 0, 1)) * (1 - k);
        break;
    }
    case CS_LAB: lab_rgb(cs, c, out); break;
    case CS_IDX: {
        int i = (int)(c[0] + 0.5f);
        if (i < 0) i = 0;
        if (i > cs->hival) i = cs->hival;
        if (cs->lut) for (int k = 0; k < 3; k++) out[k] = cs->lut[3 * i + k] / 255.0f;
        else out[0] = out[1] = out[2] = 0;
        break;
    }
    case CS_SEP: {
        float o[8] = { 0 };
        if (cs->fn.nf && cs->alt) {
            fnset_eval(&cs->fn, c, o);
            to_rgb(cs->alt, o, out);
        } else {
            /* no function: a tint of black */
            float t = cs->n ? clampf(c[0], 0, 1) : 0;
            out[0] = out[1] = out[2] = 1 - t;
        }
        break;
    }
    default: out[0] = out[1] = out[2] = 0; break;
    }
}

static csi_t *cs_get(of_pdfr_t *R, const pv_t *v, const pv_t *res, int depth);

static csi_t *cs_named(const char *n)
{
    if (!strcmp(n, "DeviceGray") || !strcmp(n, "G") || !strcmp(n, "CalGray")) return &CS_DEV_GRAY;
    if (!strcmp(n, "DeviceRGB") || !strcmp(n, "RGB") || !strcmp(n, "CalRGB")) return &CS_DEV_RGB;
    if (!strcmp(n, "DeviceCMYK") || !strcmp(n, "CMYK")) return &CS_DEV_CMYK;
    if (!strcmp(n, "Pattern")) return &CS_DEV_PAT;
    return NULL;
}

static csi_t *cs_get(of_pdfr_t *R, const pv_t *v, const pv_t *res, int depth)
{
    pdf_t *P = R->P;
    v = pdf_res(P, v);
    if (!v || depth > 4) return &CS_DEV_GRAY;
    if (v->t == PV_NAME) {
        csi_t *d = cs_named(v->s.s);
        if (d) return d;
        /* a name in the resources */
        const pv_t *rd = pdf_get(P, res, "ColorSpace");
        const pv_t *x = pdf_get(P, rd, v->s.s);
        return x ? cs_get(R, x, res, depth + 1) : &CS_DEV_GRAY;
    }
    if (v->t != PV_ARR || !v->a.n) return &CS_DEV_GRAY;
    for (csi_t *c = R->css; c; c = c->next) if (c->key == v) return c;
    const pv_t *k = pdf_res(P, &v->a.v[0]);
    if (!k || k->t != PV_NAME) return &CS_DEV_GRAY;
    const char *kn = k->s.s;
    csi_t *d = cs_named(kn);
    if (d && strcmp(kn, "Pattern")) return d;
    csi_t *c = of_alloc(sizeof *c);
    if (!c) return &CS_DEV_GRAY;
    c->key = v;
    c->kind = CS_GRAY;
    c->n = 1;
    if (!strcmp(kn, "ICCBased") && v->a.n >= 2) {
        int n = pvi(pdf_get(P, pdf_res(P, &v->a.v[1]), "N"), 3);
        c->kind = n == 1 ? CS_GRAY : n == 4 ? CS_CMYK : CS_RGB;
        c->n = n == 1 ? 1 : n == 4 ? 4 : 3;
    } else if (!strcmp(kn, "CalRGB")) {
        c->kind = CS_RGB; c->n = 3;
    } else if (!strcmp(kn, "Lab")) {
        c->kind = CS_LAB; c->n = 3;
        const pv_t *d2 = v->a.n >= 2 ? pdf_res(P, &v->a.v[1]) : NULL;
        c->wp[0] = 0.9505f; c->wp[1] = 1; c->wp[2] = 1.089f;
        arr_f(pdf_get(P, d2, "WhitePoint"), c->wp, 3, NULL);
        c->range[0] = -100; c->range[1] = 100; c->range[2] = -100; c->range[3] = 100;
        arr_f(pdf_get(P, d2, "Range"), c->range, 4, NULL);
    } else if ((!strcmp(kn, "Indexed") || !strcmp(kn, "I")) && v->a.n >= 4) {
        c->kind = CS_IDX;
        c->n = 1;
        csi_t *base = cs_get(R, &v->a.v[1], res, depth + 1);
        c->hival = pvi(pdf_res(P, &v->a.v[2]), 255);
        if (c->hival < 0) c->hival = 0;
        if (c->hival > 255) c->hival = 255;
        const pv_t *lt = pdf_res(P, &v->a.v[3]);
        const uint8_t *tab = NULL;
        size_t tn = 0;
        uint8_t *own = NULL;
        if (lt && lt->t == PV_STR) { tab = (const uint8_t *)lt->s.s; tn = (size_t)lt->s.n; }
        else if (lt && lt->t == PV_STREAM) { own = pdf_stream(P, lt, v->a.v[3].t == PV_REF ? v->a.v[3].ref.num : 0, &tn, NULL); tab = own; }
        c->lut = of_alloc((size_t)(c->hival + 1) * 3);
        int bn = base->n > 0 && base->n <= 8 ? base->n : 1;
        for (int i = 0; c->lut && i <= c->hival; i++) {
            float in[8] = { 0 }, o[3];
            for (int j = 0; j < bn; j++) {
                size_t at = (size_t)i * (size_t)bn + (size_t)j;
                float b = at < tn ? tab[at] / 255.0f : 0;
                if (base->kind == CS_LAB) b = j == 0 ? b * 100 : base->range[2 * (j - 1)] + b * (base->range[2 * (j - 1) + 1] - base->range[2 * (j - 1)]);
                in[j] = b;
            }
            to_rgb(base, in, o);
            for (int j = 0; j < 3; j++) c->lut[3 * i + j] = (uint8_t)(o[j] * 255 + 0.5f);
        }
        of_free(own);
    } else if ((!strcmp(kn, "Separation") || !strcmp(kn, "DeviceN")) && v->a.n >= 4) {
        c->kind = CS_SEP;
        const pv_t *names = pdf_res(P, &v->a.v[1]);
        c->n = names && names->t == PV_ARR ? names->a.n : 1;
        if (c->n < 1) c->n = 1;
        if (c->n > 8) c->n = 8;
        c->alt = cs_get(R, &v->a.v[2], res, depth + 1);
        fnset_get(R, &v->a.v[3], &c->fn);
        /* "None": nothing; "All": every plate */
        if (names && names->t == PV_NAME && !strcmp(names->s.s, "All")) c->fn.nf = 0;
    } else if (!strcmp(kn, "Pattern")) {
        c->kind = CS_PAT;
        c->n = 0;
        if (v->a.n >= 2) c->under = cs_get(R, &v->a.v[1], res, depth + 1);
    }
    c->next = R->css;
    R->css = c;
    return c;
}

static void cs_initial(const csi_t *cs, float *c)
{
    for (int i = 0; i < 8; i++) c[i] = 0;
    if (cs->kind == CS_CMYK) c[3] = 1;
    else if (cs->kind == CS_SEP) for (int i = 0; i < 8; i++) c[i] = 1;
}

/* -------------------------------------------------------------------------- */
/* Fonts as the painter uses them                                              */
/* -------------------------------------------------------------------------- */

typedef struct rfont {
    const font_t *t;
    of_fprog_t prog;
    bool       has_prog, cff_cid;
    int16_t    gid[256];        /* simple fonts: -2 not looked up yet */
    uint16_t  *cid2gid;
    int        ncid2gid;
    /* Type 0: the Encoding CMap's CIDs (none: Identity) */
    struct { uint32_t lo, hi, cid; } *cr;
    int        ncr;
    struct { uint8_t len; uint32_t lo, hi; } cs[8];
    int        ncs;
    bool       cmap_ucs2;       /* a predefined Unicode CMap: the codes are Unicode */
    uint8_t    fbflags;         /* the fallback's look: FL_ bits, 8 serif */
    /* Type 3 */
    const pv_t *charprocs, *res3;
    of_mtx_t   fm3;
} rfont_t;

static void rfont_cmap(of_pdfr_t *R, rfont_t *F, const pv_t *s, int num)
{
    size_t n;
    uint8_t *d = pdf_stream(R->P, s, num, &n, NULL);
    if (!d) return;
    lx_t L = { d, n, 0 };
    pv_t v[3];
    for (;;) {
        pv_t op;
        if (!pv_parse(NULL, &L, &op)) break;
        if (op.t != PV_OP) { pv_free(&op); continue; }
        int mode = !strcmp(op.s.s, "begincodespacerange") ? 1 : !strcmp(op.s.s, "begincidchar") ? 2 : !strcmp(op.s.s, "begincidrange") ? 3 : 0;
        pv_free(&op);
        if (!mode) continue;
        int per = mode == 3 ? 3 : 2;
        for (;;) {
            int got = 0;
            bool end = false;
            for (int k = 0; k < per; k++) {
                if (!pv_parse(NULL, &L, &v[k])) { end = true; break; }
                if (v[k].t == PV_OP) { pv_free(&v[k]); end = true; break; }
                got++;
            }
            if (end) { for (int k = 0; k < got; k++) pv_free(&v[k]); break; }
            uint32_t a = 0, b = 0;
            for (int i = 0; v[0].t == PV_STR && i < v[0].s.n && i < 4; i++) a = a << 8 | (uint8_t)v[0].s.s[i];
            for (int i = 0; v[1].t == PV_STR && i < v[1].s.n && i < 4; i++) b = b << 8 | (uint8_t)v[1].s.s[i];
            if (mode == 1 && v[0].t == PV_STR && F->ncs < 8 && v[0].s.n >= 1 && v[0].s.n <= 4) {
                F->cs[F->ncs].len = (uint8_t)v[0].s.n;
                F->cs[F->ncs].lo = a;
                F->cs[F->ncs].hi = b;
                F->ncs++;
            } else if (mode >= 2 && v[0].t == PV_STR) {
                uint32_t hi = mode == 3 ? b : a, cid = (uint32_t)pv_num(&v[mode == 3 ? 2 : 1], 0);
                if (!(F->ncr & 63)) {
                    void *x = of_realloc(F->cr, (size_t)(F->ncr + 64) * sizeof *F->cr);
                    if (!x) { for (int k = 0; k < per; k++) pv_free(&v[k]); break; }
                    F->cr = x;
                }
                F->cr[F->ncr].lo = a;
                F->cr[F->ncr].hi = hi < a ? a : hi;
                F->cr[F->ncr].cid = cid;
                F->ncr++;
            }
            for (int k = 0; k < per; k++) pv_free(&v[k]);
        }
    }
    of_free(d);
}

static rfont_t *rfont_get(of_pdfr_t *R, const font_t *t)
{
    if (!t) return NULL;
    for (int i = R->nrf - 1; i >= 0; i--) if (R->rf[i]->t == t) return R->rf[i];
    rfont_t **nn = of_realloc(R->rf, (size_t)(R->nrf + 1) * sizeof *nn);
    if (!nn) return NULL;
    R->rf = nn;
    rfont_t *F = of_alloc(sizeof *F);
    if (!F) return NULL;
    R->rf[R->nrf++] = F;
    F->t = t;
    for (int i = 0; i < 256; i++) F->gid[i] = -2;
    pdf_t *P = R->P;
    const pv_t *fd = t->dict, *desc_font = fd;
    if (t->type0) {
        const pv_t *df = pdf_get(P, fd, "DescendantFonts");
        if (df && df->t == PV_ARR && df->a.n) desc_font = pdf_res(P, &df->a.v[0]);
        const pv_t *eref = pv_get(fd, "Encoding");
        const pv_t *enc = pdf_res(P, eref);
        if (enc && enc->t == PV_STREAM) rfont_cmap(R, F, enc, eref && eref->t == PV_REF ? eref->ref.num : 0);
        else if (enc && enc->t == PV_NAME && strncmp(enc->s.s, "Identity", 8) && (strstr(enc->s.s, "UCS2") || strstr(enc->s.s, "UTF16")))
            F->cmap_ucs2 = true;
        /* TrueType CIDs: their glyphs */
        const pv_t *cgref = desc_font ? pv_get(desc_font, "CIDToGIDMap") : NULL;
        const pv_t *cg = pdf_res(P, cgref);
        if (cg && cg->t == PV_STREAM) {
            size_t n;
            uint8_t *d = pdf_stream(P, cg, cgref->t == PV_REF ? cgref->ref.num : 0, &n, NULL);
            if (d) {
                F->ncid2gid = (int)(n / 2);
                F->cid2gid = of_alloc((size_t)F->ncid2gid * 2 + 2);
                for (int i = 0; F->cid2gid && i < F->ncid2gid; i++) F->cid2gid[i] = (uint16_t)(d[2 * i] << 8 | d[2 * i + 1]);
                of_free(d);
            }
        }
    }
    if (t->type3) {
        F->charprocs = pdf_get(P, fd, "CharProcs");
        F->res3 = pdf_get(P, fd, "Resources");
        float m[6] = { 0.001f, 0, 0, 0.001f, 0, 0 };
        arr_f(pdf_get(P, fd, "FontMatrix"), m, 6, NULL);
        F->fm3 = (of_mtx_t){ m[0], m[1], m[2], m[3], m[4], m[5] };
        return F;
    }
    /* the program */
    const pv_t *desc = desc_font && desc_font->t == PV_DICT ? pdf_get(P, desc_font, "FontDescriptor") : NULL;
    static const char *const FF[3] = { "FontFile", "FontFile2", "FontFile3" };
    for (int k = 0; desc && desc->t == PV_DICT && k < 3 && !F->has_prog; k++) {
        const pv_t *ref = pv_get(desc, FF[k]);
        const pv_t *s = pdf_res(P, ref);
        if (!s || s->t != PV_STREAM) continue;
        size_t n;
        uint8_t *d = pdf_stream(P, s, ref->t == PV_REF ? ref->ref.num : 0, &n, NULL);
        if (d) F->has_prog = of_fprog_load(&F->prog, d, n, k + 1);
    }
    F->cff_cid = F->has_prog && F->prog.kind == FP_CFF && F->prog.cid;
    /* the fallback's look */
    const pv_t *base = pdf_get(P, fd, "BaseFont");
    F->fbflags = t->flags;
    int fl = desc && desc->t == PV_DICT ? pvi(pdf_get(P, desc, "Flags"), 0) : 0;
    if ((fl & 2) || (base && base->t == PV_NAME && (strstr(base->s.s, "Times") || strstr(base->s.s, "Georgia") || strstr(base->s.s, "Garamond") ||
                                                   strstr(base->s.s, "Roman") || strstr(base->s.s, "Serif") || strstr(base->s.s, "Cambria") ||
                                                   strstr(base->s.s, "Minion") || strstr(base->s.s, "Palatino") || strstr(base->s.s, "Book"))))
        F->fbflags |= 8;
    if (base && base->t == PV_NAME && strstr(base->s.s, "Sans")) F->fbflags &= (uint8_t)~8;
    return F;
}

static void rfont_free(rfont_t *F)
{
    of_fprog_free(&F->prog);
    of_free(F->cid2gid);
    of_free(F->cr);
    of_free(F);
}

/* the next code of a string, by the Encoding CMap's code space when it has one */
static uint32_t rf_next(const rfont_t *F, const uint8_t *s, int n, int *i)
{
    if (F->t->type0 && F->ncs) {
        for (int len = 1; len <= 4 && *i + len <= n; len++) {
            uint32_t c = 0;
            for (int k = 0; k < len; k++) c = c << 8 | s[*i + k];
            for (int r = 0; r < F->ncs; r++)
                if (F->cs[r].len == len && c >= F->cs[r].lo && c <= F->cs[r].hi) { *i += len; return c; }
        }
    }
    return of_pdff_next(F->t, s, n, i);
}

static uint32_t first_cp(const font_t *t, uint32_t code)
{
    char buf[16];
    const char *u = of_pdff_text(t, code, buf);
    if (!u) return 0;
    int l;
    return of_utf8_get(u, &l);
}

/* the glyph of a code in the font's program, -1 if none */
static int rf_gid(rfont_t *F, uint32_t code)
{
    if (!F->has_prog) return -1;
    const font_t *t = F->t;
    const of_fprog_t *G = &F->prog;
    if (t->type0) {
        uint32_t cid = code;
        if (F->ncr) {
            cid = 0;
            for (int i = 0; i < F->ncr; i++) if (code >= F->cr[i].lo && code <= F->cr[i].hi) { cid = F->cr[i].cid + (code - F->cr[i].lo); break; }
        }
        if (F->cmap_ucs2 && G->kind == FP_TT) return of_fprog_by_uni(G, code);
        if (G->kind == FP_TT) {
            if (F->cid2gid) return (int)cid < F->ncid2gid ? F->cid2gid[cid] : -1;
            return (int)cid < G->ngl ? (int)cid : -1;
        }
        if (F->cff_cid) return of_fprog_by_cid(G, cid);
        return (int)cid < G->ngl ? (int)cid : -1;
    }
    int c = (int)(code & 255);
    if (F->gid[c] != -2) return F->gid[c];
    int g = -1;
    const char *nm = t->names[c];
    if (G->kind == FP_TT) {
        if (nm) { uint32_t u = of_pdf_glyph_cp(nm); if (u) g = of_fprog_by_uni(G, u); }
        if (g < 0 && !t->symbolic && G->c31 && t->enc[c]) g = of_fprog_by_uni(G, t->enc[c]);
        if (g < 0) g = of_fprog_by_code(G, c, t->symbolic);
        if (g < 0 && G->c31) { uint32_t u = first_cp(t, code); if (u) g = of_fprog_by_uni(G, u); }
    } else {
        if (nm) g = of_fprog_by_name(G, nm);
        if (g < 0 && t->base_named && t->enc[c]) g = of_fprog_by_uni(G, t->enc[c]);
        if (g < 0) g = of_fprog_by_code(G, c, t->symbolic);
        if (g < 0 && !t->base_named && t->enc[c] && !t->symbolic) g = of_fprog_by_uni(G, t->enc[c]);
        if (g < 0) { uint32_t u = first_cp(t, code); if (u) g = of_fprog_by_uni(G, u); }
    }
    if (g >= G->ngl) g = -1;
    F->gid[c] = (int16_t)g;
    return g;
}

/* -------------------------------------------------------------------------- */
/* The picture painted into                                                    */
/* -------------------------------------------------------------------------- */

typedef struct { int ref; uint8_t m[]; } clipm_t;

typedef struct {
    of_mtx_t ctm;
    csi_t   *cs[2];             /* 0 fill, 1 stroke */
    float    col[2][8];
    const pv_t *pat[2];
    uint8_t  rgb[2][3];
    float    alpha[2];
    float    lw, ml;
    int      cap, join;
    float    dash[16], dphase;
    int      ndash;
    rfont_t *font;
    float    fs, tc, tw, th, tl, ts;
    int      tr;
    int      cx0, cy0, cx1, cy1;
    clipm_t *mask;
} gs_t;

typedef struct { float x0, y0, x1, y1, dxdy; int dir; } edge_t;

typedef struct img {
    int       key, w, h;        /* key: the object (and the size asked) */
    int       dw, dh;
    uint16_t *px;
    uint8_t  *a;
    bool      hal, stencil, interp;
} img_t;

typedef struct dc_s dc_t;
struct dc_s {
    of_pdfr_t *R;
    uint16_t *px;
    uint8_t  *pa;               /* alpha, for a pattern's cell */
    int       w, h;
    gs_t      g[MAX_GS];
    int       ng;
    of_mtx_t  tm, tlm;
    of_path_t path;
    bool      clip_pending, clip_eo;
    float    *cov;
    edge_t   *E;
    int       ne, cape;
    of_path_t tmp;
    int       depth, t3depth;
    volatile bool *cancel;
    long      ops;
    bool      stop;
    int       page;
    of_mtx_t  base;             /* the page's space to the picture's pixels */
    bool      t3color_off;
};

static inline gs_t *G(dc_t *D) { return &D->g[D->ng - 1]; }

static inline void pt_dev(const of_mtx_t *m, float x, float y, float *ox, float *oy)
{
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}

static inline void blend565(uint16_t *d, int r, int g, int b, int a)
{
    uint16_t v = *d;
    if (a >= 255) { *d = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); return; }
    int dr = (v >> 11) << 3, dg = ((v >> 5) & 63) << 2, db = (v & 31) << 3;
    dr |= dr >> 5; dg |= dg >> 6; db |= db >> 5;
    dr += ((r - dr) * a + 127) / 255;
    dg += ((g - dg) * a + 127) / 255;
    db += ((b - db) * a + 127) / 255;
    *d = (uint16_t)((dr >> 3) << 11 | (dg >> 2) << 5 | db >> 3);
}

/* one pixel of colour (r, g, b) at coverage a (0..255) */
static inline void put(dc_t *D, int x, int y, int r, int g, int b, int a)
{
    size_t i = (size_t)y * (size_t)D->w + (size_t)x;
    if (!D->pa) { blend565(&D->px[i], r, g, b, a); return; }
    /* a cell with its own alpha: colours over what is there, alpha added */
    int da = D->pa[i];
    int oa = a + da * (255 - a) / 255;
    if (oa <= 0) return;
    uint16_t v = D->px[i];
    int dr = (v >> 11) << 3, dg = ((v >> 5) & 63) << 2, db = (v & 31) << 3;
    int k = da * (255 - a) / 255;
    int nr = (r * a + dr * k) / oa, ng = (g * a + dg * k) / oa, nb = (b * a + db * k) / oa;
    D->px[i] = (uint16_t)(((nr > 255 ? 255 : nr) >> 3) << 11 | ((ng > 255 ? 255 : ng) >> 2) << 5 | (nb > 255 ? 255 : nb) >> 3);
    D->pa[i] = (uint8_t)oa;
}

/* -------------------------------------------------------------------------- */
/* Flattening and filling                                                      */
/* -------------------------------------------------------------------------- */

static void add_edge(dc_t *D, float x0, float y0, float x1, float y1)
{
    if (y0 == y1) return;
    if (D->ne == D->cape) {
        int nc = D->cape ? D->cape * 2 : 1024;
        if (nc > 4000000) return;
        edge_t *n = of_realloc(D->E, (size_t)nc * sizeof *n);
        if (!n) return;
        D->E = n;
        D->cape = nc;
    }
    edge_t *e = &D->E[D->ne++];
    if (y0 < y1) { e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1; e->dir = 1; }
    else { e->x0 = x1; e->y0 = y1; e->x1 = x0; e->y1 = y0; e->dir = -1; }
    e->dxdy = (e->x1 - e->x0) / (e->y1 - e->y0);
}

typedef void (*seg_fn)(void *ud, float x0, float y0, float x1, float y1);
typedef void (*sub_fn)(void *ud, bool closed);

/* Every subpath as lines; curves cut until they lie within a quarter pixel. */
static void flatten(const of_path_t *p, seg_fn seg, sub_fn end, void *ud)
{
    int k = 0;
    float sx = 0, sy = 0, cx = 0, cy = 0;
    bool open = false;
    for (int i = 0; i < p->nop; i++) {
        switch (p->op[i]) {
        case 'M':
            if (open && end) end(ud, false);
            sx = cx = p->pt[k].x; sy = cy = p->pt[k].y; k++;
            open = true;
            break;
        case 'L':
            seg(ud, cx, cy, p->pt[k].x, p->pt[k].y);
            cx = p->pt[k].x; cy = p->pt[k].y; k++;
            open = true;
            break;
        case 'Q': {
            of_pt_t c = p->pt[k], e = p->pt[k + 1];
            k += 2;
            float dx = cx - 2 * c.x + e.x, dy = cy - 2 * c.y + e.y;
            float dd = sqrtf(dx * dx + dy * dy);
            int n = 1 + (int)sqrtf(dd * 2.0f);
            if (n > 64) n = 64;
            float px = cx, py = cy;
            for (int s = 1; s <= n; s++) {
                float t = (float)s / (float)n, u = 1 - t;
                float x = u * u * cx + 2 * u * t * c.x + t * t * e.x, y = u * u * cy + 2 * u * t * c.y + t * t * e.y;
                seg(ud, px, py, x, y);
                px = x; py = y;
            }
            cx = e.x; cy = e.y;
            open = true;
            break;
        }
        case 'C': {
            of_pt_t a = p->pt[k], b = p->pt[k + 1], e = p->pt[k + 2];
            k += 3;
            float d1x = cx - 2 * a.x + b.x, d1y = cy - 2 * a.y + b.y, d2x = a.x - 2 * b.x + e.x, d2y = a.y - 2 * b.y + e.y;
            float dd = sqrtf(fmaxf(d1x * d1x + d1y * d1y, d2x * d2x + d2y * d2y));
            int n = 1 + (int)sqrtf(dd * 3.0f);
            if (n > 100) n = 100;
            float px = cx, py = cy;
            for (int s = 1; s <= n; s++) {
                float t = (float)s / (float)n, u = 1 - t;
                float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
                float x = w0 * cx + w1 * a.x + w2 * b.x + w3 * e.x, y = w0 * cy + w1 * a.y + w2 * b.y + w3 * e.y;
                seg(ud, px, py, x, y);
                px = x; py = y;
            }
            cx = e.x; cy = e.y;
            open = true;
            break;
        }
        case 'Z':
            if (cx != sx || cy != sy) seg(ud, cx, cy, sx, sy);
            if (end) end(ud, true);
            cx = sx; cy = sy;
            open = false;
            break;
        }
    }
    if (open && end) end(ud, false);
}

typedef struct { dc_t *D; float sx, sy, lx, ly; bool have; } fe_t;

static void fe_seg(void *ud, float x0, float y0, float x1, float y1)
{
    fe_t *F = ud;
    if (!F->have) { F->sx = x0; F->sy = y0; F->have = true; }
    add_edge(F->D, x0, y0, x1, y1);
    F->lx = x1; F->ly = y1;
}

static void fe_end(void *ud, bool closed)
{
    fe_t *F = ud;
    /* filling closes what is open */
    if (F->have && !closed) add_edge(F->D, F->lx, F->ly, F->sx, F->sy);
    F->have = false;
}

static int edge_cmp(const void *a, const void *b)
{
    float x = ((const edge_t *)a)->y0, y = ((const edge_t *)b)->y0;
    return x < y ? -1 : x > y;
}

typedef void (*row_fn)(void *ud, int y, int x0, int x1, const float *cov);

/* The edges filled into rows of coverage, inside [cx0, cx1) x [cy0, cy1). */
static void raster(dc_t *D, bool eo, int sub, int cx0, int cy0, int cx1, int cy1, row_fn row, void *ud)
{
    if (!D->ne || cx1 <= cx0 || cy1 <= cy0) return;
    qsort(D->E, (size_t)D->ne, sizeof *D->E, edge_cmp);
    float ymin = D->E[0].y0, ymax = -1e30f;
    for (int i = 0; i < D->ne; i++) if (D->E[i].y1 > ymax) ymax = D->E[i].y1;
    int y0 = (int)floorf(ymin), y1 = (int)ceilf(ymax);
    if (y0 < cy0) y0 = cy0;
    if (y1 > cy1) y1 = cy1;
    if (y0 >= y1) return;
    float *cov = D->cov;            /* w + 2 partials, then w + 2 running deltas */
    float *acc = cov + D->w + 2;
    int *act = of_alloc((size_t)D->ne * sizeof(int));
    struct xc { float x; int d; } *xs = of_alloc((size_t)D->ne * sizeof *xs);
    if (!act || !xs) { of_free(act); of_free(xs); return; }
    int nact = 0, next = 0;
    float wsub = 1.0f / (float)sub;
    for (int y = y0; y < y1; y++) {
        int minx = cx1, maxx = cx0 - 1;
        for (int s = 0; s < sub; s++) {
            float sy = (float)y + ((float)s + 0.5f) * wsub;
            while (next < D->ne && D->E[next].y0 <= sy) act[nact++] = next++;
            int nx = 0;
            for (int k = 0; k < nact;) {
                edge_t *e = &D->E[act[k]];
                if (e->y1 <= sy) { act[k] = act[--nact]; continue; }
                if (e->y0 <= sy) xs[nx++] = (struct xc){ e->x0 + (sy - e->y0) * e->dxdy, e->dir };
                k++;
            }
            if (nx < 2) continue;
            for (int i = 1; i < nx; i++) {
                struct xc t = xs[i];
                int j = i - 1;
                while (j >= 0 && xs[j].x > t.x) { xs[j + 1] = xs[j]; j--; }
                xs[j + 1] = t;
            }
            int wind = 0;
            float xa = 0;
            for (int i = 0; i < nx; i++) {
                bool was = eo ? (wind & 1) : wind != 0;
                wind += xs[i].d;
                bool is = eo ? (wind & 1) : wind != 0;
                if (!was && is) xa = xs[i].x;
                else if (was && !is) {
                    float a = xa < (float)cx0 ? (float)cx0 : xa, b = xs[i].x > (float)cx1 ? (float)cx1 : xs[i].x;
                    if (b <= a) continue;
                    int ia = (int)a, ib = (int)b;
                    if (ia < minx) minx = ia;
                    if ((ib < cx1 ? ib : cx1 - 1) > maxx) maxx = ib < cx1 ? ib : cx1 - 1;
                    if (ia == ib) { cov[ia] += (b - a) * wsub; continue; }
                    cov[ia] += ((float)(ia + 1) - a) * wsub;
                    acc[ia + 1] += wsub;
                    acc[ib] -= wsub;
                    if (ib < cx1) cov[ib] += (b - (float)ib) * wsub;
                }
            }
        }
        if (maxx < minx) continue;
        float run = 0;
        for (int x = minx; x <= maxx; x++) { run += acc[x]; cov[x] += run; acc[x] = 0; }
        acc[maxx + 1] = 0;
        row(ud, y, minx, maxx, cov);
        for (int x = minx; x <= maxx + 1; x++) cov[x] = 0;
    }
    of_free(act);
    of_free(xs);
}

/* -------------------------------------------------------------------------- */
/* What fills: a colour, a shading, a pattern's cell                           */
/* -------------------------------------------------------------------------- */

typedef struct shade_s {
    int      type;
    csi_t   *cs;
    fnset_t  fn;
    float    coords[6], dom[2];
    bool     ext[2];
    of_mtx_t inv;               /* the picture's pixels to the shading's space */
    uint8_t  lut[256][3];
    bool     has_bg;
    uint8_t  bg[3];
    /* type 1 */
    of_mtx_t fmat;
} shade_t;

typedef struct {
    int       w, h;
    uint16_t *px;
    uint8_t  *a;
    of_mtx_t  inv;              /* the picture's pixels to the pattern's space */
    float     bx, by, xs, ys, kx, ky;
    bool      uncolored;
} tile_t;

typedef struct {
    dc_t     *D;
    int       kind;             /* 0 colour, 1 shading, 2 cell */
    int       r, g, b;
    float     alpha;
    shade_t  *sh;
    tile_t   *tl;
    const clipm_t *mask;
    const uint8_t *a8;          /* a glyph's coverage instead of cov */
} paint_t;

static bool shade_at(const shade_t *S, float X, float Y, uint8_t out[3])
{
    float x = S->inv.a * X + S->inv.c * Y + S->inv.e, y = S->inv.b * X + S->inv.d * Y + S->inv.f;
    float t;
    if (S->type == 2) {
        float dx = S->coords[2] - S->coords[0], dy = S->coords[3] - S->coords[1];
        float l2 = dx * dx + dy * dy;
        t = l2 > 0 ? ((x - S->coords[0]) * dx + (y - S->coords[1]) * dy) / l2 : 0;
    } else if (S->type == 3) {
        /* the largest s with |p - c(s)| = r(s) */
        float x0 = S->coords[0], y0 = S->coords[1], r0 = S->coords[2], x1 = S->coords[3], y1 = S->coords[4], r1 = S->coords[5];
        float cdx = x1 - x0, cdy = y1 - y0, dr = r1 - r0;
        float px = x - x0, py = y - y0;
        float a = cdx * cdx + cdy * cdy - dr * dr;
        float b = px * cdx + py * cdy + r0 * dr;
        float c = px * px + py * py - r0 * r0;
        if (fabsf(a) < 1e-6f) {
            if (fabsf(b) < 1e-9f) return false;
            t = c / (2 * b);
        } else {
            float disc = b * b - a * c;
            if (disc < 0) return false;
            float sq = sqrtf(disc);
            t = (b + sq) / a;
            if (r0 + t * dr < 0) t = (b - sq) / a;
        }
        if (r0 + t * dr < 0) return false;
    } else if (S->type == 1) {
        float u = S->fmat.a * x + S->fmat.c * y + S->fmat.e, v = S->fmat.b * x + S->fmat.d * y + S->fmat.f;
        float in[2] = { u, v }, o[8] = { 0 }, rgb[3];
        fnset_eval(&S->fn, in, o);
        to_rgb(S->cs, o, rgb);
        for (int i = 0; i < 3; i++) out[i] = (uint8_t)(rgb[i] * 255 + 0.5f);
        return true;
    } else {
        return false;
    }
    if (t < 0) { if (!S->ext[0]) { if (S->has_bg) { memcpy(out, S->bg, 3); return true; } return false; } t = 0; }
    if (t > 1) { if (!S->ext[1]) { if (S->has_bg) { memcpy(out, S->bg, 3); return true; } return false; } t = 1; }
    int i = (int)(t * 255 + 0.5f);
    memcpy(out, S->lut[i], 3);
    return true;
}

static void paint_px(paint_t *P, int x, int y, float c)
{
    dc_t *D = P->D;
    if (P->mask) c *= P->mask->m[(size_t)y * (size_t)D->w + (size_t)x] * (1.0f / 255);
    c *= P->alpha;
    if (c <= 0.004f) return;
    int a = (int)(c * 255 + 0.5f);
    if (a > 255) a = 255;
    if (P->kind == 0) { put(D, x, y, P->r, P->g, P->b, a); return; }
    if (P->kind == 1) {
        uint8_t o[3];
        if (shade_at(P->sh, (float)x + 0.5f, (float)y + 0.5f, o)) put(D, x, y, o[0], o[1], o[2], a);
        return;
    }
    tile_t *T = P->tl;
    float X = (float)x + 0.5f, Y = (float)y + 0.5f;
    float u = T->inv.a * X + T->inv.c * Y + T->inv.e - T->bx, v = T->inv.b * X + T->inv.d * Y + T->inv.f - T->by;
    u = fmodf(u, T->xs); if (u < 0) u += T->xs;
    v = fmodf(v, T->ys); if (v < 0) v += T->ys;
    int i = (int)(u * T->kx), j = (int)(v * T->ky);
    if (i < 0 || j < 0 || i >= T->w || j >= T->h) return;
    size_t k = (size_t)j * (size_t)T->w + (size_t)i;
    int ta = T->a[k];
    if (!ta) return;
    a = a * ta / 255;
    if (T->uncolored) { put(D, x, y, P->r, P->g, P->b, a); return; }
    uint16_t v16 = T->px[k];
    put(D, x, y, (v16 >> 11) << 3, ((v16 >> 5) & 63) << 2, (v16 & 31) << 3, a);
}

static void paint_row(void *ud, int y, int x0, int x1, const float *cov)
{
    paint_t *P = ud;
    for (int x = x0; x <= x1; x++) {
        float c = cov[x];
        if (c <= 0.004f) continue;
        paint_px(P, x, y, c > 1 ? 1 : c);
    }
}

/* -------------------------------------------------------------------------- */
/* Paint set-up from the state                                                 */
/* -------------------------------------------------------------------------- */

static bool pattern_paint(dc_t *D, int which, paint_t *P);
static void paint_done(paint_t *P);

static bool paint_from(dc_t *D, int which, paint_t *P)
{
    gs_t *g = G(D);
    memset(P, 0, sizeof *P);
    P->D = D;
    P->alpha = g->alpha[which];
    P->mask = g->mask;
    P->r = g->rgb[which][0];
    P->g = g->rgb[which][1];
    P->b = g->rgb[which][2];
    if (g->cs[which] && g->cs[which]->kind == CS_PAT) return pattern_paint(D, which, P);
    return true;
}

/* the clip's box, and a path's box, intersected */
static bool clip_box(dc_t *D, float bx0, float by0, float bx1, float by1, int *x0, int *y0, int *x1, int *y1)
{
    gs_t *g = G(D);
    *x0 = (int)floorf(bx0); *y0 = (int)floorf(by0); *x1 = (int)ceilf(bx1) + 1; *y1 = (int)ceilf(by1) + 1;
    if (*x0 < g->cx0) *x0 = g->cx0;
    if (*y0 < g->cy0) *y0 = g->cy0;
    if (*x1 > g->cx1) *x1 = g->cx1;
    if (*y1 > g->cy1) *y1 = g->cy1;
    return *x1 > *x0 && *y1 > *y0;
}

/* a path that is one rectangle along the axes: its corners */
static bool path_rect(const of_path_t *p, float *rx0, float *ry0, float *rx1, float *ry1)
{
    int n = p->nop;
    if (n < 4 || n > 6 || p->op[0] != 'M') return false;
    int lines = 0;
    for (int i = 1; i < n; i++) {
        if (p->op[i] == 'L') lines++;
        else if (p->op[i] != 'Z' || i != n - 1) return false;
    }
    if (lines < 3 || lines > 4 || p->npt != lines + 1) return false;
    const of_pt_t *q = p->pt;
    if (lines == 4 && (fabsf(q[4].x - q[0].x) > 0.01f || fabsf(q[4].y - q[0].y) > 0.01f)) return false;
    bool a = fabsf(q[0].y - q[1].y) < 0.01f && fabsf(q[1].x - q[2].x) < 0.01f && fabsf(q[2].y - q[3].y) < 0.01f && fabsf(q[3].x - q[0].x) < 0.01f;
    bool b = fabsf(q[0].x - q[1].x) < 0.01f && fabsf(q[1].y - q[2].y) < 0.01f && fabsf(q[2].x - q[3].x) < 0.01f && fabsf(q[3].y - q[0].y) < 0.01f;
    if (!a && !b) return false;
    *rx0 = fminf(q[0].x, q[2].x); *rx1 = fmaxf(q[0].x, q[2].x);
    *ry0 = fminf(q[0].y, q[2].y); *ry1 = fmaxf(q[0].y, q[2].y);
    return true;
}

/* a rectangle filled exactly: each pixel by how much of it is inside */
static void fill_rect(dc_t *D, float rx0, float ry0, float rx1, float ry1, paint_t *P)
{
    int x0, y0, x1, y1;
    if (!clip_box(D, rx0, ry0, rx1, ry1, &x0, &y0, &x1, &y1)) return;
    float *cov = D->cov;
    for (int x = x0; x < x1; x++) {
        float a = fmaxf((float)x, rx0), b = fminf((float)x + 1, rx1);
        cov[x] = b > a ? b - a : 0;
    }
    float *rowc = cov + D->w + 2;
    for (int y = y0; y < y1; y++) {
        float a = fmaxf((float)y, ry0), b = fminf((float)y + 1, ry1);
        float vy = b > a ? b - a : 0;
        if (vy <= 0) continue;
        for (int x = x0; x < x1; x++) rowc[x] = cov[x] * vy;
        paint_row(P, y, x0, x1 - 1, rowc);
    }
    for (int x = x0; x < x1 + 1 && x < D->w + 2; x++) { cov[x] = 0; rowc[x] = 0; }
}

static void fill_path_with(dc_t *D, const of_path_t *path, bool eo, paint_t *P)
{
    float rx0, ry0, rx1, ry1;
    if (path_rect(path, &rx0, &ry0, &rx1, &ry1)) {
        /* thin rules wider than nothing */
        if (rx1 - rx0 < 0.5f) { float m = (rx0 + rx1) / 2; rx0 = m - 0.25f; rx1 = m + 0.25f; }
        if (ry1 - ry0 < 0.5f) { float m = (ry0 + ry1) / 2; ry0 = m - 0.25f; ry1 = m + 0.25f; }
        fill_rect(D, rx0, ry0, rx1, ry1, P);
        return;
    }
    D->ne = 0;
    fe_t F = { .D = D };
    flatten(path, fe_seg, fe_end, &F);
    if (!D->ne) return;
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (int i = 0; i < D->ne; i++) {
        edge_t *e = &D->E[i];
        bx0 = fminf(bx0, fminf(e->x0, e->x1)); bx1 = fmaxf(bx1, fmaxf(e->x0, e->x1));
        by0 = fminf(by0, e->y0); by1 = fmaxf(by1, e->y1);
    }
    int x0, y0, x1, y1;
    if (!clip_box(D, bx0, by0, bx1, by1, &x0, &y0, &x1, &y1)) return;
    raster(D, eo, SUB, x0, y0, x1, y1, paint_row, P);
}

static void fill_path(dc_t *D, const of_path_t *path, bool eo)
{
    paint_t P;
    if (!paint_from(D, 0, &P)) return;
    fill_path_with(D, path, eo, &P);
    paint_done(&P);
}

/* -------------------------------------------------------------------------- */
/* Strokes, as polygons                                                        */
/* -------------------------------------------------------------------------- */

typedef struct { of_pt_t *p; int n, cap; bool closed; } poly_t;

typedef struct {
    dc_t   *D;
    poly_t *pl;
    int     npl, cappl;
    poly_t  cur;
} sk_t;

static void poly_add(poly_t *P, float x, float y)
{
    if (P->n && fabsf(P->p[P->n - 1].x - x) < 1e-4f && fabsf(P->p[P->n - 1].y - y) < 1e-4f) return;
    if (P->n == P->cap) {
        int nc = P->cap ? P->cap * 2 : 32;
        of_pt_t *n = of_realloc(P->p, (size_t)nc * sizeof *n);
        if (!n) return;
        P->p = n;
        P->cap = nc;
    }
    P->p[P->n++] = (of_pt_t){ x, y };
}

static void sk_seg(void *ud, float x0, float y0, float x1, float y1)
{
    sk_t *S = ud;
    if (!S->cur.n) poly_add(&S->cur, x0, y0);
    poly_add(&S->cur, x1, y1);
}

static void sk_end(void *ud, bool closed)
{
    sk_t *S = ud;
    if (!S->cur.n) return;
    if (S->npl == S->cappl) {
        int nc = S->cappl ? S->cappl * 2 : 16;
        poly_t *n = of_realloc(S->pl, (size_t)nc * sizeof *n);
        if (!n) { of_free(S->cur.p); memset(&S->cur, 0, sizeof S->cur); return; }
        S->pl = n;
        S->cappl = nc;
    }
    S->cur.closed = closed;
    if (closed && S->cur.n > 2 && fabsf(S->cur.p[0].x - S->cur.p[S->cur.n - 1].x) < 1e-4f && fabsf(S->cur.p[0].y - S->cur.p[S->cur.n - 1].y) < 1e-4f)
        S->cur.n--;
    S->pl[S->npl++] = S->cur;
    memset(&S->cur, 0, sizeof S->cur);
}

/* a polygon into the fill's path, always turning the same way (so all of them add up) */
static void out_poly(of_path_t *o, const of_pt_t *p, int n)
{
    if (n < 3) return;
    float area = 0;
    for (int i = 0; i < n; i++) { int j = (i + 1) % n; area += p[i].x * p[j].y - p[j].x * p[i].y; }
    if (fabsf(area) < 1e-6f) return;
    if (area > 0) { of_path_move(o, p[0].x, p[0].y); for (int i = 1; i < n; i++) of_path_line(o, p[i].x, p[i].y); }
    else { of_path_move(o, p[n - 1].x, p[n - 1].y); for (int i = n - 2; i >= 0; i--) of_path_line(o, p[i].x, p[i].y); }
    of_path_close(o);
}

static void out_circle(of_path_t *o, float cx, float cy, float r)
{
    int n = (int)(r * 2.5f) + 8;
    if (n > 64) n = 64;
    of_pt_t p[64];
    for (int i = 0; i < n; i++) {
        float a = (float)i * 6.2831853f / (float)n;
        p[i] = (of_pt_t){ cx + r * cosf(a), cy + r * sinf(a) };
    }
    out_poly(o, p, n);
}

static void stroke_poly(of_path_t *o, const poly_t *L, float hw, int cap, int join, float ml)
{
    const of_pt_t *p = L->p;
    int n = L->n;
    if (n == 1 || (n == 2 && fabsf(p[0].x - p[1].x) < 1e-4f && fabsf(p[0].y - p[1].y) < 1e-4f)) {
        /* a dot: only round and square caps show it */
        if (cap == 1) out_circle(o, p[0].x, p[0].y, hw);
        else if (cap == 2) { of_pt_t q[4] = { { p[0].x - hw, p[0].y - hw }, { p[0].x + hw, p[0].y - hw }, { p[0].x + hw, p[0].y + hw }, { p[0].x - hw, p[0].y + hw } }; out_poly(o, q, 4); }
        return;
    }
    int segs = L->closed ? n : n - 1;
    for (int i = 0; i < segs; i++) {
        of_pt_t a = p[i], b = p[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy);
        if (l < 1e-5f) continue;
        float ux = dx / l, uy = dy / l, nx = -uy * hw, ny = ux * hw;
        float ex0 = 0, ex1 = 0;
        if (!L->closed && cap == 2) { if (i == 0) ex0 = hw; if (i == segs - 1) ex1 = hw; }
        of_pt_t q[4] = { { a.x - ux * ex0 + nx, a.y - uy * ex0 + ny }, { b.x + ux * ex1 + nx, b.y + uy * ex1 + ny },
                         { b.x + ux * ex1 - nx, b.y + uy * ex1 - ny }, { a.x - ux * ex0 - nx, a.y - uy * ex0 - ny } };
        out_poly(o, q, 4);
    }
    /* joins */
    int first = L->closed ? 0 : 1, last = L->closed ? n : n - 1;
    for (int i = first; i < last; i++) {
        of_pt_t v = p[i], a = p[(i - 1 + n) % n], b = p[(i + 1) % n];
        float d0x = v.x - a.x, d0y = v.y - a.y, d1x = b.x - v.x, d1y = b.y - v.y;
        float l0 = sqrtf(d0x * d0x + d0y * d0y), l1 = sqrtf(d1x * d1x + d1y * d1y);
        if (l0 < 1e-5f || l1 < 1e-5f) continue;
        d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
        float cr = d0x * d1y - d0y * d1x, dot = d0x * d1x + d0y * d1y;
        if (fabsf(cr) < 1e-4f && dot > 0) continue;
        if (join == 1 || hw < 1.0f) { out_circle(o, v.x, v.y, hw); continue; }
        /* the outer side */
        float s = cr > 0 ? -1.0f : 1.0f;
        of_pt_t o0 = { v.x - d0y * hw * s, v.y + d0x * hw * s }, o1 = { v.x - d1y * hw * s, v.y + d1x * hw * s };
        float cosh = sqrtf(fmaxf((1 + dot) / 2, 0));
        if (join == 0 && cosh > 1e-4f && 1 / cosh <= ml) {
            float mx = (d1x - d0x), my = (d1y - d0y);
            float ml2 = sqrtf(mx * mx + my * my);
            if (ml2 > 1e-5f) {
                mx = -mx / ml2; my = -my / ml2;
                float len = hw / cosh;
                of_pt_t m = { v.x + mx * len, v.y + my * len };
                of_pt_t q[4] = { v, o0, m, o1 };
                out_poly(o, q, 4);
                continue;
            }
        }
        of_pt_t q[3] = { v, o0, o1 };
        out_poly(o, q, 3);
    }
    if (!L->closed && cap == 1) { out_circle(o, p[0].x, p[0].y, hw); out_circle(o, p[n - 1].x, p[n - 1].y, hw); }
}

/* the polylines cut by the dash pattern */
static void dash_polys(sk_t *S, const float *dash, int nd, float phase)
{
    float total = 0;
    for (int i = 0; i < nd; i++) total += dash[i];
    if (total <= 0.01f) return;
    int npl = S->npl;
    poly_t *src = S->pl;
    S->pl = NULL;
    S->npl = S->cappl = 0;
    for (int k = 0; k < npl; k++) {
        poly_t *L = &src[k];
        int di = 0;
        float left = 0, ph = fmodf(phase, total);
        if (ph < 0) ph += total;
        while (ph >= dash[di]) { ph -= dash[di]; di = (di + 1) % nd; }
        left = dash[di] - ph;
        bool on = !(di & 1);
        int segs = L->closed ? L->n : L->n - 1;
        if (on) poly_add(&S->cur, L->p[0].x, L->p[0].y);
        for (int i = 0; i < segs; i++) {
            of_pt_t a = L->p[i], b = L->p[(i + 1) % L->n];
            float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy), at = 0;
            for (int guard = 0; l - at > left && guard < 100000; guard++) {
                at += left;
                float x = a.x + dx * at / l, y = a.y + dy * at / l;
                if (on) { poly_add(&S->cur, x, y); sk_end(S, false); }
                else poly_add(&S->cur, x, y);
                on = !on;
                di = (di + 1) % nd;
                left = dash[di];
            }
            left -= l - at;
            if (on) poly_add(&S->cur, b.x, b.y);
        }
        if (on) sk_end(S, false);
        else { of_free(S->cur.p); memset(&S->cur, 0, sizeof S->cur); }
        of_free(L->p);
    }
    of_free(src);
}

static float dev_scale(const of_mtx_t *m) { return sqrtf(fabsf(m->a * m->d - m->b * m->c)); }

static void stroke_path_with(dc_t *D, const of_path_t *path, paint_t *P)
{
    gs_t *g = G(D);
    float sc = dev_scale(&g->ctm);
    float lw = g->lw * sc;
    if (lw < 1.0f) lw = 1.0f;
    sk_t S = { .D = D };
    flatten(path, sk_seg, sk_end, &S);
    if (S.cur.n) sk_end(&S, false);
    if (g->ndash) {
        float d[16];
        bool any = false;
        for (int i = 0; i < g->ndash; i++) { d[i] = g->dash[i] * sc; if (d[i] > 0) any = true; }
        if (any) dash_polys(&S, d, g->ndash, g->dphase * sc);
    }
    of_path_t *o = &D->tmp;
    of_path_reset(o);
    for (int i = 0; i < S.npl; i++) {
        stroke_poly(o, &S.pl[i], lw / 2, g->cap, g->join, g->ml);
        of_free(S.pl[i].p);
    }
    of_free(S.pl);
    of_free(S.cur.p);
    if (!o->nop) return;
    /* the polygons, every one turning the same way: filled as one */
    D->ne = 0;
    fe_t F = { .D = D };
    flatten(o, fe_seg, fe_end, &F);
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (int i = 0; i < D->ne; i++) {
        edge_t *e = &D->E[i];
        bx0 = fminf(bx0, fminf(e->x0, e->x1)); bx1 = fmaxf(bx1, fmaxf(e->x0, e->x1));
        by0 = fminf(by0, e->y0); by1 = fmaxf(by1, e->y1);
    }
    int x0, y0, x1, y1;
    if (D->ne && clip_box(D, bx0, by0, bx1, by1, &x0, &y0, &x1, &y1)) raster(D, false, SUB, x0, y0, x1, y1, paint_row, P);
}

static void stroke_path(dc_t *D, const of_path_t *path)
{
    paint_t P;
    if (!paint_from(D, 1, &P)) return;
    stroke_path_with(D, path, &P);
    paint_done(&P);
}

/* -------------------------------------------------------------------------- */
/* Clipping                                                                    */
/* -------------------------------------------------------------------------- */

static void mask_unref(clipm_t *m) { if (m && --m->ref <= 0) of_free(m); }

typedef struct { dc_t *D; uint8_t *m; } cr_t;

static void clip_row(void *ud, int y, int x0, int x1, const float *cov)
{
    cr_t *C = ud;
    uint8_t *r = C->m + (size_t)y * (size_t)C->D->w;
    for (int x = x0; x <= x1; x++) {
        float c = cov[x];
        r[x] = (uint8_t)(c >= 1 ? 255 : c <= 0 ? 0 : (int)(c * 255 + 0.5f));
    }
}

static void clip_path(dc_t *D, const of_path_t *path, bool eo)
{
    gs_t *g = G(D);
    float rx0, ry0, rx1, ry1;
    if (path_rect(path, &rx0, &ry0, &rx1, &ry1) && !g->mask) {
        /* a rectangle: the box alone, to whole pixels */
        int x0 = (int)floorf(rx0 + 0.5f), y0 = (int)floorf(ry0 + 0.5f), x1 = (int)floorf(rx1 + 0.5f), y1 = (int)floorf(ry1 + 0.5f);
        if (x0 > g->cx0) g->cx0 = x0;
        if (y0 > g->cy0) g->cy0 = y0;
        if (x1 < g->cx1) g->cx1 = x1;
        if (y1 < g->cy1) g->cy1 = y1;
        if (g->cx1 < g->cx0) g->cx1 = g->cx0;
        if (g->cy1 < g->cy0) g->cy1 = g->cy0;
        return;
    }
    D->ne = 0;
    fe_t F = { .D = D };
    flatten(path, fe_seg, fe_end, &F);
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (int i = 0; i < D->ne; i++) {
        edge_t *e = &D->E[i];
        bx0 = fminf(bx0, fminf(e->x0, e->x1)); bx1 = fmaxf(bx1, fmaxf(e->x0, e->x1));
        by0 = fminf(by0, e->y0); by1 = fmaxf(by1, e->y1);
    }
    int x0, y0, x1, y1;
    if (!D->ne || !clip_box(D, bx0, by0, bx1, by1, &x0, &y0, &x1, &y1)) { g->cx1 = g->cx0; g->cy1 = g->cy0; return; }
    clipm_t *m = of_alloc(sizeof *m + (size_t)D->w * (size_t)D->h);
    if (!m) { g->cx0 = x0; g->cy0 = y0; g->cx1 = x1; g->cy1 = y1; return; }
    m->ref = 1;
    cr_t C = { D, m->m };
    raster(D, eo, SUB, x0, y0, x1, y1, clip_row, &C);
    if (g->mask) {
        for (int y = y0; y < y1; y++) {
            uint8_t *a = m->m + (size_t)y * (size_t)D->w, *b = g->mask->m + (size_t)y * (size_t)D->w;
            for (int x = x0; x < x1; x++) a[x] = (uint8_t)(a[x] * b[x] / 255);
        }
        mask_unref(g->mask);
    }
    g->mask = m;
    g->cx0 = x0; g->cy0 = y0; g->cx1 = x1; g->cy1 = y1;
}

/* -------------------------------------------------------------------------- */
/* Shadings and patterns                                                       */
/* -------------------------------------------------------------------------- */

static bool mtx_inv(of_mtx_t m, of_mtx_t *o)
{
    float det = m.a * m.d - m.b * m.c;
    if (fabsf(det) < 1e-12f) return false;
    float id = 1 / det;
    o->a = m.d * id; o->b = -m.b * id; o->c = -m.c * id; o->d = m.a * id;
    o->e = -(m.e * o->a + m.f * o->c);
    o->f = -(m.e * o->b + m.f * o->d);
    return true;
}

static shade_t *shade_new(dc_t *D, const pv_t *sh, const pv_t *res, of_mtx_t to_dev)
{
    pdf_t *P = D->R->P;
    sh = pdf_res(P, sh);
    if (!sh || (sh->t != PV_DICT && sh->t != PV_STREAM)) return NULL;
    int type = pvi(pdf_get(P, sh, "ShadingType"), 0);
    if (type < 1 || type > 3) return NULL;
    shade_t *S = of_alloc(sizeof *S);
    if (!S) return NULL;
    S->type = type;
    S->cs = cs_get(D->R, pv_get(sh, "ColorSpace"), res, 0);
    fnset_get(D->R, pv_get(sh, "Function"), &S->fn);
    if (!mtx_inv(to_dev, &S->inv)) { of_free(S); return NULL; }
    arr_f(pdf_get(P, sh, "Coords"), S->coords, 6, NULL);
    S->dom[0] = 0; S->dom[1] = 1;
    float dm[4] = { 0, 1, 0, 1 };
    arr_f(pdf_get(P, sh, "Domain"), dm, 4, NULL);
    S->dom[0] = dm[0]; S->dom[1] = dm[1];
    const pv_t *ex = pdf_get(P, sh, "Extend");
    if (ex && ex->t == PV_ARR && ex->a.n >= 2) {
        S->ext[0] = ex->a.v[0].t == PV_BOOL && ex->a.v[0].b;
        S->ext[1] = ex->a.v[1].t == PV_BOOL && ex->a.v[1].b;
    }
    const pv_t *bg = pdf_get(P, sh, "Background");
    if (bg && bg->t == PV_ARR) {
        float c[8] = { 0 }, rgb[3];
        arr_f(bg, c, 8, NULL);
        to_rgb(S->cs, c, rgb);
        for (int i = 0; i < 3; i++) S->bg[i] = (uint8_t)(rgb[i] * 255 + 0.5f);
        S->has_bg = true;
    }
    if (type == 1) {
        float m[6] = { 1, 0, 0, 1, 0, 0 };
        arr_f(pdf_get(P, sh, "Matrix"), m, 6, NULL);
        of_mtx_t fm = { m[0], m[1], m[2], m[3], m[4], m[5] };
        /* the function's own space: the shading space through the inverse of Matrix */
        if (!mtx_inv(fm, &S->fmat)) S->fmat = (of_mtx_t){ 1, 0, 0, 1, 0, 0 };
        if (!S->fn.nf) { of_free(S); return NULL; }
        return S;
    }
    /* the colours along t, ahead */
    for (int i = 0; i < 256; i++) {
        float t = S->dom[0] + (S->dom[1] - S->dom[0]) * (float)i / 255.0f, o[8] = { 0 }, rgb[3];
        if (S->fn.nf) fnset_eval(&S->fn, &t, o);
        else o[0] = t;
        to_rgb(S->cs, o, rgb);
        for (int k = 0; k < 3; k++) S->lut[i][k] = (uint8_t)(clampf(rgb[k], 0, 1) * 255 + 0.5f);
    }
    return S;
}

static void run_stream(dc_t *D, const uint8_t *d, size_t n, const pv_t *res);

/* a tiling pattern's cell, painted once at the scale it is shown */
static tile_t *tile_new(dc_t *D, const pv_t *pat, int num, of_mtx_t to_dev, bool uncolored, const uint8_t rgb[3])
{
    pdf_t *P = D->R->P;
    float bb[4] = { 0, 0, 1, 1 };
    arr_f(pdf_get(P, pat, "BBox"), bb, 4, NULL);
    float xs = fabsf(fnum(pdf_get(P, pat, "XStep"), bb[2] - bb[0])), ys = fabsf(fnum(pdf_get(P, pat, "YStep"), bb[3] - bb[1]));
    if (xs < 1e-3f || ys < 1e-3f) return NULL;
    float kx = sqrtf(to_dev.a * to_dev.a + to_dev.b * to_dev.b), ky = sqrtf(to_dev.c * to_dev.c + to_dev.d * to_dev.d);
    int w = (int)ceilf(xs * kx), h = (int)ceilf(ys * ky);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    /* not bigger than needed, nor than 512 a side */
    if (w > 512) { kx *= 512.0f / (float)w; w = 512; }
    if (h > 512) { ky *= 512.0f / (float)h; h = 512; }
    tile_t *T = of_alloc(sizeof *T);
    if (!T) return NULL;
    T->w = w; T->h = h;
    T->px = of_alloc((size_t)w * (size_t)h * 2);
    T->a = of_alloc((size_t)w * (size_t)h);
    if (!T->px || !T->a || !mtx_inv(to_dev, &T->inv)) { of_free(T->px); of_free(T->a); of_free(T); return NULL; }
    T->bx = bb[0]; T->by = bb[1]; T->xs = xs; T->ys = ys;
    T->kx = (float)w / xs; T->ky = (float)h / ys;
    T->uncolored = uncolored;
    size_t n;
    uint8_t *d = pdf_stream(P, pat, num, &n, NULL);
    if (d && D->depth < MAX_DEPTH) {
        dc_t *C = of_alloc(sizeof *C);
        if (C) {
            C->R = D->R;
            C->px = T->px; C->pa = T->a; C->w = w; C->h = h;
            C->cov = of_alloc(((size_t)w + 4) * 2 * sizeof(float));
            C->cancel = D->cancel;
            C->depth = D->depth + 1;
            C->ng = 1;
            gs_t *g = &C->g[0];
            memset(g, 0, sizeof *g);
            g->ctm = (of_mtx_t){ T->kx, 0, 0, T->ky, -bb[0] * T->kx, -bb[1] * T->ky };
            C->base = g->ctm;
            g->cs[0] = g->cs[1] = &CS_DEV_GRAY;
            g->alpha[0] = g->alpha[1] = 1;
            g->lw = 1; g->ml = 10; g->th = 1;
            g->cx1 = w; g->cy1 = h;
            if (uncolored) for (int k = 0; k < 2; k++) memcpy(g->rgb[k], rgb, 3);
            C->t3color_off = uncolored;
            if (C->cov) run_stream(C, d, n, pdf_get(P, pat, "Resources"));
            while (C->ng > 0) { mask_unref(C->g[C->ng - 1].mask); C->ng--; }
            of_path_free(&C->path);
            of_path_free(&C->tmp);
            of_free(C->E);
            of_free(C->cov);
            of_free(C);
        }
    }
    of_free(d);
    return T;
}

static bool pattern_paint(dc_t *D, int which, paint_t *P)
{
    gs_t *g = G(D);
    const pv_t *pref = g->pat[which];
    pdf_t *Pd = D->R->P;
    const pv_t *pat = pdf_res(Pd, pref);
    if (!pat) return false;
    float m[6] = { 1, 0, 0, 1, 0, 0 };
    arr_f(pdf_get(Pd, pat, "Matrix"), m, 6, NULL);
    of_mtx_t to_dev = of_mtx_mul((of_mtx_t){ m[0], m[1], m[2], m[3], m[4], m[5] }, D->base);
    int type = pvi(pdf_get(Pd, pat, "PatternType"), 1);
    if (type == 2) {
        P->sh = shade_new(D, pv_get(pat, "Shading"), NULL, to_dev);
        if (!P->sh) return false;
        P->kind = 1;
        return true;
    }
    if (pat->t != PV_STREAM) return false;
    bool unc = pv_num(pdf_get(Pd, pat, "PaintType"), 1) == 2;
    if (unc) {
        csi_t *u = g->cs[which]->under ? g->cs[which]->under : &CS_DEV_GRAY;
        float rgb[3];
        to_rgb(u, g->col[which], rgb);
        P->r = (int)(rgb[0] * 255 + 0.5f); P->g = (int)(rgb[1] * 255 + 0.5f); P->b = (int)(rgb[2] * 255 + 0.5f);
    }
    uint8_t c3[3] = { (uint8_t)P->r, (uint8_t)P->g, (uint8_t)P->b };
    P->tl = tile_new(D, pat, pref && pref->t == PV_REF ? pref->ref.num : 0, to_dev, unc, c3);
    if (!P->tl) {
        /* without its cell: a light grey, so the shape still shows */
        P->kind = 0;
        P->r = P->g = P->b = 200;
        return true;
    }
    P->kind = 2;
    return true;
}

static void paint_done(paint_t *P)
{
    of_free(P->sh);
    if (P->tl) { of_free(P->tl->px); of_free(P->tl->a); of_free(P->tl); }
    P->sh = NULL;
    P->tl = NULL;
}

/* "sh": the shading over all of the clip */
static void op_sh(dc_t *D, const pv_t *res, const char *name)
{
    const pv_t *sd = pdf_get(D->R->P, res, "Shading");
    paint_t P = { .D = D, .alpha = G(D)->alpha[0], .mask = G(D)->mask, .kind = 1 };
    P.sh = shade_new(D, pv_get(sd, name), res, G(D)->ctm);
    if (!P.sh) return;
    gs_t *g = G(D);
    float *cov = D->cov;
    for (int x = g->cx0; x < g->cx1; x++) cov[x] = 1;
    for (int y = g->cy0; y < g->cy1; y++) paint_row(&P, y, g->cx0, g->cx1 - 1, cov);
    for (int x = g->cx0; x < g->cx1; x++) cov[x] = 0;
    paint_done(&P);
}

/* -------------------------------------------------------------------------- */
/* Pictures                                                                    */
/* -------------------------------------------------------------------------- */

static void img_free(img_t *I)
{
    if (!I) return;
    if (I->hal) aos_hal_image_free(I->px);
    else of_free(I->px);
    of_free(I->a);
    of_free(I);
}

/* the abbreviations of inline pictures */
static const pv_t *iget(pdf_t *P, const pv_t *d, const char *full, const char *abbr)
{
    const pv_t *v = pdf_get(P, d, full);
    return v ? v : abbr ? pdf_get(P, d, abbr) : NULL;
}

/* samples into RGB565 (and alpha for stencils), shrunk by f while read */
static img_t *img_raw(dc_t *D, const pv_t *im, const uint8_t *d, size_t n, int w, int h, int f, const pv_t *res, bool stencil)
{
    pdf_t *P = D->R->P;
    int bpc = stencil ? 1 : pvi(iget(P, im, "BitsPerComponent", "BPC"), 8);
    csi_t *cs = stencil ? &CS_DEV_GRAY : cs_get(D->R, iget(P, im, "ColorSpace", "CS"), res, 0);
    int nc = cs->kind == CS_IDX ? 1 : cs->n > 0 ? cs->n : 1;
    if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) return NULL;
    size_t rowbytes = ((size_t)w * (size_t)nc * (size_t)bpc + 7) / 8;
    if (n < rowbytes) return NULL;
    int hh = (int)(n / rowbytes) < h ? (int)(n / rowbytes) : h;      /* short data: what there is */
    float dec[16];
    int ndec = 0;
    arr_f(iget(P, im, "Decode", "D"), dec, 16, &ndec);
    float mx = (float)((1 << (bpc > 8 ? 8 : bpc)) - 1);
    for (int k = ndec / 2; k < nc && k < 8; k++) {
        dec[2 * k] = 0;
        dec[2 * k + 1] = cs->kind == CS_IDX ? mx : cs->kind == CS_LAB ? (k == 0 ? 100 : cs->range[2 * (k - 1) + 1]) : 1;
        if (cs->kind == CS_LAB && k > 0) dec[2 * k] = cs->range[2 * (k - 1)];
    }
    /* a table for one component of 8 bits or less */
    uint8_t lut[256][3];
    bool use_lut = nc == 1 && bpc <= 8;
    if (use_lut) {
        for (int v = 0; v <= (int)mx; v++) {
            float c[8] = { dec[0] + (float)v / mx * (dec[1] - dec[0]) }, rgb[3];
            to_rgb(cs, c, rgb);
            for (int k = 0; k < 3; k++) lut[v][k] = (uint8_t)(clampf(rgb[k], 0, 1) * 255 + 0.5f);
        }
    }
    /* colour key masking */
    const pv_t *mk = pdf_get(P, im, "Mask");
    int key[16], nkey = 0;
    if (mk && mk->t == PV_ARR) for (int i = 0; i < mk->a.n && i < 16; i++) key[nkey++] = pvi(&mk->a.v[i], 0);
    int ow = w / f, oh = hh / f;
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    img_t *I = of_alloc(sizeof *I);
    if (!I) return NULL;
    I->w = ow; I->h = oh;
    I->px = stencil ? NULL : of_alloc((size_t)ow * (size_t)oh * 2);
    if (stencil || nkey) I->a = of_alloc((size_t)ow * (size_t)oh);
    if ((!stencil && !I->px) || ((stencil || nkey) && !I->a)) { img_free(I); return NULL; }
    I->stencil = stencil;
    bool inv_stencil = stencil && ndec >= 2 && dec[0] > 0.5f;
    for (int oy = 0; oy < oh; oy++) {
        if (D->cancel && *D->cancel) break;
        for (int ox = 0; ox < ow; ox++) {
            int sr = 0, sg = 0, sb = 0, sa = 0, cnt = 0;
            for (int yy = oy * f; yy < oy * f + f && yy < hh; yy++) {
                const uint8_t *row = d + (size_t)yy * rowbytes;
                for (int xx = ox * f; xx < ox * f + f && xx < w; xx++) {
                    int v[8];
                    for (int k = 0; k < nc && k < 8; k++) {
                        size_t bit = ((size_t)xx * (size_t)nc + (size_t)k) * (size_t)bpc;
                        if (bpc == 8) v[k] = row[bit / 8];
                        else if (bpc == 16) v[k] = row[bit / 8];        /* the high byte */
                        else v[k] = (row[bit / 8] >> (8 - bpc - (int)(bit % 8))) & ((1 << bpc) - 1);
                    }
                    cnt++;
                    if (stencil) {
                        bool paint = inv_stencil ? v[0] == 1 : v[0] == 0;
                        sa += paint ? 255 : 0;
                        continue;
                    }
                    if (nkey) {
                        bool in = true;
                        for (int k = 0; k < nc && 2 * k + 1 < nkey; k++) if (v[k] < key[2 * k] || v[k] > key[2 * k + 1]) in = false;
                        sa += in ? 0 : 255;
                    }
                    if (use_lut) { sr += lut[v[0]][0]; sg += lut[v[0]][1]; sb += lut[v[0]][2]; continue; }
                    float c[8], rgb[3];
                    float m8 = bpc == 16 ? 255.0f : mx;
                    for (int k = 0; k < nc && k < 8; k++) c[k] = dec[2 * k] + (float)v[k] / m8 * (dec[2 * k + 1] - dec[2 * k]);
                    if (cs->kind == CS_RGB && bpc == 8 && !ndec) { sr += v[0]; sg += v[1]; sb += v[2]; continue; }
                    to_rgb(cs, c, rgb);
                    sr += (int)(rgb[0] * 255 + 0.5f); sg += (int)(rgb[1] * 255 + 0.5f); sb += (int)(rgb[2] * 255 + 0.5f);
                }
            }
            if (!cnt) cnt = 1;
            size_t at = (size_t)oy * (size_t)ow + (size_t)ox;
            if (I->a) I->a[at] = (uint8_t)(sa / cnt);
            if (I->px) {
                int r = sr / cnt, g = sg / cnt, b = sb / cnt;
                I->px[at] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
            }
        }
    }
    return I;
}

/* a soft mask (or a stencil /Mask) at the picture's size, as alpha */
static void img_smask(dc_t *D, img_t *I, const pv_t *sref, bool stencil_mask)
{
    pdf_t *P = D->R->P;
    const pv_t *sm = pdf_res(P, sref);
    if (!sm || sm->t != PV_STREAM) return;
    int w = pvi(pdf_get(P, sm, "Width"), 0), h = pvi(pdf_get(P, sm, "Height"), 0);
    if (w <= 0 || h <= 0 || (int64_t)w * h > 40000000) return;
    const char *imf = NULL;
    size_t n;
    uint8_t *d = pdf_stream(P, sm, sref->t == PV_REF ? sref->ref.num : 0, &n, &imf);
    if (!d) return;
    img_t *M = NULL;
    if (!imf) {
        int f = 1;
        while (w / (f + 1) >= I->w && h / (f + 1) >= I->h) f++;
        M = img_raw(D, sm, d, n, w, h, f, NULL, stencil_mask);
    }
    of_free(d);
    if (!M) return;
    uint8_t *a = of_alloc((size_t)I->w * (size_t)I->h);
    if (a) {
        for (int y = 0; y < I->h; y++)
            for (int x = 0; x < I->w; x++) {
                int sx = x * M->w / I->w, sy = y * M->h / I->h;
                size_t si = (size_t)sy * (size_t)M->w + (size_t)sx;
                int v;
                if (stencil_mask) v = M->a[si];
                else { uint16_t p = M->px[si]; v = (((p >> 11) << 3) + (((p >> 5) & 63) << 2) + ((p & 31) << 3)) / 3; }
                size_t di = (size_t)y * (size_t)I->w + (size_t)x;
                a[di] = (uint8_t)(I->a ? I->a[di] * v / 255 : v);
            }
        of_free(I->a);
        I->a = a;
    }
    img_free(M);
}

static img_t *img_get(dc_t *D, const pv_t *im, int num, const uint8_t *inl, size_t inl_n, const pv_t *res, float dw, float dh)
{
    of_pdfr_t *R = D->R;
    pdf_t *P = R->P;
    int w = pvi(iget(P, im, "Width", "W"), 0), h = pvi(iget(P, im, "Height", "H"), 0);
    if (w <= 0 || h <= 0 || (int64_t)w * h > 60000000) return NULL;
    /* shrunk while read: no more than 1.5 times the pixels it covers */
    int f = 1;
    while (w / (f + 1) >= dw * 1.5f && h / (f + 1) >= dh * 1.5f && f < 64) f++;
    int key = num > 0 ? num * 64 + f : 0;
    if (key)
        for (int i = 0; i < R->nimg; i++) if (R->img[i]->key == key) return R->img[i];
    const pv_t *imk = iget(P, im, "ImageMask", "IM");
    bool stencil = imk && imk->t == PV_BOOL && imk->b;
    img_t *I = NULL;
    const char *imf = NULL;
    uint8_t *d = NULL;
    size_t n = 0;
    if (inl) {
        uint8_t *cp = of_alloc(inl_n + 1);
        if (cp) {
            memcpy(cp, inl, inl_n);
            d = pdf_decode(P, cp, inl_n, pdf_res(P, iget(P, im, "Filter", "F")), pdf_res(P, iget(P, im, "DecodeParms", "DP")), &n, &imf);
        }
    } else {
        d = pdf_stream(P, im, num, &n, &imf);
    }
    if (d && imf && !strcmp(imf, "DCTDecode") && !stencil) {
        /* JPEG: the system's decoder, from the file when it is there as it is */
        int ow = 0, oh = 0;
        uint16_t *px = NULL;
        int mw = w / f, mh = h / f;
        const pv_t *flt = pdf_res(P, pv_get(im, "Filter"));
        bool plain = !inl && !P->crypt && flt && (flt->t == PV_NAME || (flt->t == PV_ARR && flt->a.n == 1));
        if (plain) px = aos_hal_image_decode(R->path, (uint32_t)im->soff, (uint32_t)im->slen, mw, mh, false, &ow, &oh, NULL, NULL);
        if (!px) {
            char name[64], path[200];
            snprintf(name, sizeof name, "pdr%08lx_%d.jpg", (unsigned long)R->file_id, num);
            if (of_cache_file(name, d, n, path, sizeof path)) px = aos_hal_image_decode(path, 0, 0, mw, mh, false, &ow, &oh, NULL, NULL);
        }
        if (px && ow > 0 && oh > 0) {
            I = of_alloc(sizeof *I);
            if (I) { I->px = px; I->w = ow; I->h = oh; I->hal = true; }
            else aos_hal_image_free(px);
        }
    } else if (d && !imf) {
        I = img_raw(D, im, d, n, w, h, f, res, stencil);
    }
    of_free(d);
    if (!I) return NULL;
    if (!stencil) {
        const pv_t *sm = pv_get(im, "SMask");
        const pv_t *mk = pdf_res(P, pv_get(im, "Mask"));
        if (sm && pdf_res(P, sm) && pdf_res(P, sm)->t == PV_STREAM) img_smask(D, I, sm, false);
        else if (mk && mk->t == PV_STREAM) img_smask(D, I, pv_get(im, "Mask"), true);
    }
    const pv_t *ip = iget(P, im, "Interpolate", "I");
    I->interp = ip && ip->t == PV_BOOL && ip->b;
    I->key = key;
    if (key) {
        if (R->nimg == IMG_SLOTS) { img_free(R->img[0]); memmove(R->img, R->img + 1, (IMG_SLOTS - 1) * sizeof *R->img); R->nimg--; }
        R->img[R->nimg++] = I;
    }
    return I;
}

/* the picture over the unit square of the current matrix */
static void draw_img(dc_t *D, const img_t *I)
{
    gs_t *g = G(D);
    of_mtx_t m = g->ctm, inv;
    if (!mtx_inv(m, &inv)) return;
    float xs[4], ys[4];
    pt_dev(&m, 0, 0, &xs[0], &ys[0]); pt_dev(&m, 1, 0, &xs[1], &ys[1]); pt_dev(&m, 0, 1, &xs[2], &ys[2]); pt_dev(&m, 1, 1, &xs[3], &ys[3]);
    float bx0 = fminf(fminf(xs[0], xs[1]), fminf(xs[2], xs[3])), bx1 = fmaxf(fmaxf(xs[0], xs[1]), fmaxf(xs[2], xs[3]));
    float by0 = fminf(fminf(ys[0], ys[1]), fminf(ys[2], ys[3])), by1 = fmaxf(fmaxf(ys[0], ys[1]), fmaxf(ys[2], ys[3]));
    int x0, y0, x1, y1;
    if (!clip_box(D, bx0, by0, bx1, by1, &x0, &y0, &x1, &y1)) return;
    paint_t Pt;
    if (I->stencil && !paint_from(D, 0, &Pt)) return;
    /* enlarged a little: between the samples; much (an icon, a pattern of dots): as squares */
    float enl = sqrtf((bx1 - bx0) * (by1 - by0) / ((float)I->w * (float)I->h));
    bool smooth = enl > 1.2f && (enl < 3.0f || I->interp);
    float alpha = g->alpha[0];
    const clipm_t *mask = g->mask;
    for (int y = y0; y < y1; y++) {
        if ((y & 31) == 0 && D->cancel && *D->cancel) break;
        for (int x = x0; x < x1; x++) {
            float X = (float)x + 0.5f, Y = (float)y + 0.5f;
            float u = inv.a * X + inv.c * Y + inv.e, v = inv.b * X + inv.d * Y + inv.f;
            if (u < 0 || u >= 1 || v <= 0 || v > 1) continue;
            float fx = u * (float)I->w, fy = (1 - v) * (float)I->h;
            int r, gg, b, a = 255;
            if (smooth) {
                fx -= 0.5f; fy -= 0.5f;
                int ix = (int)floorf(fx), iy = (int)floorf(fy);
                float tx = fx - (float)ix, ty = fy - (float)iy;
                int ix1 = ix + 1, iy1 = iy + 1;
                if (ix < 0) ix = 0;
                if (iy < 0) iy = 0;
                if (ix1 >= I->w) ix1 = I->w - 1;
                if (iy1 >= I->h) iy1 = I->h - 1;
                if (ix >= I->w) ix = I->w - 1;
                if (iy >= I->h) iy = I->h - 1;
                size_t i00 = (size_t)iy * (size_t)I->w + (size_t)ix, i10 = (size_t)iy * (size_t)I->w + (size_t)ix1;
                size_t i01 = (size_t)iy1 * (size_t)I->w + (size_t)ix, i11 = (size_t)iy1 * (size_t)I->w + (size_t)ix1;
                float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty), w01 = (1 - tx) * ty, w11 = tx * ty;
                if (I->px) {
                    uint16_t p[4] = { I->px[i00], I->px[i10], I->px[i01], I->px[i11] };
                    float ws[4] = { w00, w10, w01, w11 }, fr = 0, fg = 0, fb = 0;
                    for (int k = 0; k < 4; k++) {
                        fr += ws[k] * (float)((p[k] >> 11) << 3);
                        fg += ws[k] * (float)(((p[k] >> 5) & 63) << 2);
                        fb += ws[k] * (float)((p[k] & 31) << 3);
                    }
                    r = (int)fr; gg = (int)fg; b = (int)fb;
                } else {
                    r = gg = b = 0;
                }
                if (I->a) a = (int)(w00 * I->a[i00] + w10 * I->a[i10] + w01 * I->a[i01] + w11 * I->a[i11]);
            } else {
                int ix = (int)fx, iy = (int)fy;
                if (ix >= I->w) ix = I->w - 1;
                if (iy >= I->h) iy = I->h - 1;
                size_t i = (size_t)iy * (size_t)I->w + (size_t)ix;
                if (I->px) { uint16_t p = I->px[i]; r = (p >> 11) << 3; gg = ((p >> 5) & 63) << 2; b = (p & 31) << 3; }
                else r = gg = b = 0;
                if (I->a) a = I->a[i];
            }
            if (I->stencil) {
                if (a) paint_px(&Pt, x, y, (float)a / 255.0f);
                continue;
            }
            float c = (float)a / 255.0f * alpha;
            if (mask) c *= mask->m[(size_t)y * (size_t)D->w + (size_t)x] * (1.0f / 255);
            if (c > 0.004f) put(D, x, y, r, gg, b, (int)(c * 255 + 0.5f));
        }
    }
    if (I->stencil) paint_done(&Pt);
}

static void draw_xobj_img(dc_t *D, const pv_t *im, int num, const uint8_t *inl, size_t inl_n, const pv_t *res)
{
    of_mtx_t m = G(D)->ctm;
    float dw = sqrtf(m.a * m.a + m.b * m.b), dh = sqrtf(m.c * m.c + m.d * m.d);
    if (dw < 0.3f && dh < 0.3f) return;
    img_t *I = img_get(D, im, num, inl, inl_n, res, dw, dh);
    if (!I) {
        /* a picture the app cannot read: its place, in grey */
        const pv_t *imk = iget(D->R->P, im, "ImageMask", "IM");
        if (imk && imk->t == PV_BOOL && imk->b) return;
        of_path_t *p = &D->tmp;
        of_path_reset(p);
        float x, y;
        pt_dev(&m, 0, 0, &x, &y); of_path_move(p, x, y);
        pt_dev(&m, 1, 0, &x, &y); of_path_line(p, x, y);
        pt_dev(&m, 1, 1, &x, &y); of_path_line(p, x, y);
        pt_dev(&m, 0, 1, &x, &y); of_path_line(p, x, y);
        of_path_close(p);
        paint_t P = { .D = D, .alpha = 1, .mask = G(D)->mask, .r = 225, .g = 225, .b = 225 };
        fill_path_with(D, p, false, &P);
        return;
    }
    draw_img(D, I);
    if (!I->key) img_free(I);
}

/* -------------------------------------------------------------------------- */
/* Text                                                                        */
/* -------------------------------------------------------------------------- */

typedef struct { uint8_t *a; int w, h; } gbm_t;

static void gbm_row(void *ud, int y, int x0, int x1, const float *cov)
{
    gbm_t *B = ud;
    if (y < 0 || y >= B->h) return;
    for (int x = x0; x <= x1; x++) {
        if (x < 0 || x >= B->w) continue;
        float c = cov[x];
        B->a[(size_t)y * (size_t)B->w + (size_t)x] = (uint8_t)(c >= 1 ? 255 : c <= 0 ? 0 : (int)(c * 255 + 0.5f));
    }
}

static void gc_clear(of_pdfr_t *R)
{
    for (int i = 0; i < GC_SLOTS && R->gc; i++) of_free(R->gc[i].a);
    if (R->gc) memset(R->gc, 0, GC_SLOTS * sizeof *R->gc);
    R->ngc = 0;
    R->gc_bytes = 0;
}

/* a glyph's coverage at the matrix's size and the position's quarter pixel */
static struct gce *glyph_bitmap(dc_t *D, rfont_t *F, int gid, of_mtx_t m, float *ox, float *oy)
{
    of_pdfr_t *R = D->R;
    if (!R->gc) R->gc = of_alloc(GC_SLOTS * sizeof *R->gc);
    if (!R->gc) return NULL;
    float fx = floorf(m.e), fy = floorf(m.f);
    int qx = (int)((m.e - fx) * 4), qy = (int)((m.f - fy) * 4);
    int32_t k[6] = { (int32_t)lroundf(m.a * 256), (int32_t)lroundf(m.b * 256), (int32_t)lroundf(m.c * 256), (int32_t)lroundf(m.d * 256), qx, qy };
    uint32_t hsh = (uint32_t)gid * 2654435761u ^ (uint32_t)(uintptr_t)F;
    for (int i = 0; i < 6; i++) hsh = hsh * 31 + (uint32_t)k[i];
    int slot = (int)(hsh % GC_SLOTS);
    for (int probe = 0; probe < 16; probe++) {
        struct gce *e = &R->gc[(slot + probe) % GC_SLOTS];
        if (!e->rf) break;
        if (e->rf == F && e->gid == gid && !memcmp(e->k, k, sizeof k)) { *ox = fx; *oy = fy; return e; }
    }
    if (R->ngc > GC_SLOTS * 3 / 4 || R->gc_bytes > GC_BYTES) gc_clear(R);
    /* the outline, at the quarter pixel */
    of_mtx_t gm = { m.a, m.b, m.c, m.d, (float)qx / 4.0f, (float)qy / 4.0f };
    of_path_t *p = &D->tmp;
    of_path_reset(p);
    of_fprog_outline(&F->prog, gid, gm, p);
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (int i = 0; i < p->npt; i++) {
        bx0 = fminf(bx0, p->pt[i].x); bx1 = fmaxf(bx1, p->pt[i].x);
        by0 = fminf(by0, p->pt[i].y); by1 = fmaxf(by1, p->pt[i].y);
    }
    struct gce E = { .rf = F, .gid = gid };
    memcpy(E.k, k, sizeof k);
    if (p->npt && bx1 > bx0 && by1 > by0) {
        int l = (int)floorf(bx0), t = (int)floorf(by0), w = (int)ceilf(bx1) - l + 1, h = (int)ceilf(by1) - t + 1;
        if (w > 600 || h > 600 || w > D->w) return NULL;
        for (int i = 0; i < p->npt; i++) { p->pt[i].x -= (float)l; p->pt[i].y -= (float)t; }
        E.a = of_alloc((size_t)w * (size_t)h);
        if (!E.a) return NULL;
        E.w = (int16_t)w; E.h = (int16_t)h; E.l = (int16_t)l; E.t = (int16_t)t;
        D->ne = 0;
        fe_t FE = { .D = D };
        flatten(p, fe_seg, fe_end, &FE);
        gbm_t B = { E.a, w, h };
        raster(D, false, SUB_GLYPH, 0, 0, w, h, gbm_row, &B);
    }
    for (int probe = 0; probe < 16; probe++) {
        struct gce *e = &R->gc[(slot + probe) % GC_SLOTS];
        if (e->rf) continue;
        *e = E;
        R->ngc++;
        R->gc_bytes += (size_t)E.w * (size_t)E.h + 64;
        *ox = fx; *oy = fy;
        return e;
    }
    of_free(E.a);
    return NULL;
}

static void blit_glyph(dc_t *D, const struct gce *e, float ox, float oy, paint_t *P)
{
    if (!e->a) return;
    gs_t *g = G(D);
    int x0 = (int)ox + e->l, y0 = (int)oy + e->t;
    for (int j = 0; j < e->h; j++) {
        int y = y0 + j;
        if (y < g->cy0 || y >= g->cy1) continue;
        const uint8_t *r = e->a + (size_t)j * (size_t)e->w;
        for (int i = 0; i < e->w; i++) {
            int x = x0 + i;
            if (!r[i] || x < g->cx0 || x >= g->cx1) continue;
            paint_px(P, x, y, (float)r[i] * (1.0f / 255));
        }
    }
}

/* a glyph the file does not carry: the app's own, mapped through the matrix */
static void fallback_glyph(dc_t *D, rfont_t *F, uint32_t cp, of_mtx_t m, float adv_pdf, paint_t *P)
{
    if (!s_fallback || cp <= 32 || cp == 0xA0) return;
    float px = sqrtf(m.c * m.c + m.d * m.d);         /* the em, in pixels, upright */
    if (px < 1.5f || px > 1200) return;
    of_fbglyph_t gl;
    if (!s_fallback(cp, px, F ? F->fbflags : 0, &gl) || !gl.a8 || gl.w <= 0 || gl.h <= 0) return;
    /* the bitmap's pixels to text space (1 = an em), squeezed to the width the file says */
    float k = 1.0f / gl.px;
    float sx = k;
    if (gl.adv > 0 && adv_pdf > 0) {
        float r = adv_pdf / (gl.adv * k);
        sx = k * clampf(r, 0.7f, 1.15f);
    }
    /* bitmap (bx, by), y down from the top at gl.top above the baseline */
    of_mtx_t b2t = { sx, 0, 0, -k, (float)gl.left * sx, (float)gl.top * k };
    of_mtx_t b2d = of_mtx_mul(b2t, m), inv;
    if (!mtx_inv(b2d, &inv)) return;
    float xs[4], ys[4];
    pt_dev(&b2d, 0, 0, &xs[0], &ys[0]); pt_dev(&b2d, (float)gl.w, 0, &xs[1], &ys[1]);
    pt_dev(&b2d, 0, (float)gl.h, &xs[2], &ys[2]); pt_dev(&b2d, (float)gl.w, (float)gl.h, &xs[3], &ys[3]);
    float bx0 = fminf(fminf(xs[0], xs[1]), fminf(xs[2], xs[3])), bx1 = fmaxf(fmaxf(xs[0], xs[1]), fmaxf(xs[2], xs[3]));
    float by0 = fminf(fminf(ys[0], ys[1]), fminf(ys[2], ys[3])), by1 = fmaxf(fmaxf(ys[0], ys[1]), fmaxf(ys[2], ys[3]));
    int x0, y0, x1, y1;
    if (!clip_box(D, bx0, by0, bx1, by1, &x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float X = (float)x + 0.5f, Y = (float)y + 0.5f;
            float u = inv.a * X + inv.c * Y + inv.e - 0.5f, v = inv.b * X + inv.d * Y + inv.f - 0.5f;
            if (u < -1 || v < -1 || u >= (float)gl.w || v >= (float)gl.h) continue;
            int iu = (int)floorf(u), iv = (int)floorf(v);
            float tu = u - (float)iu, tv = v - (float)iv;
            float s = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    int qx = iu + dx, qy = iv + dy;
                    if (qx < 0 || qy < 0 || qx >= gl.w || qy >= gl.h) continue;
                    float wgt = (dx ? tu : 1 - tu) * (dy ? tv : 1 - tv);
                    s += wgt * gl.a8[(size_t)qy * (size_t)gl.w + (size_t)qx];
                }
            if (s > 1) paint_px(P, x, y, s * (1.0f / 255));
        }
}

static void gs_push(dc_t *D);
static void gs_pop(dc_t *D, int floor_ng);

uint8_t *of_pdf_glyph_a8(const of_fprog_t *F, int gid, float px, int *w, int *h, int *left, int *top)
{
    dc_t *D = of_alloc(sizeof *D);
    if (!D) return NULL;
    uint8_t *a = NULL;
    of_fprog_outline(F, gid, (of_mtx_t){ px, 0, 0, -px, 0, 0 }, &D->tmp);
    of_path_t *p = &D->tmp;
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (int i = 0; i < p->npt; i++) {
        bx0 = fminf(bx0, p->pt[i].x); bx1 = fmaxf(bx1, p->pt[i].x);
        by0 = fminf(by0, p->pt[i].y); by1 = fmaxf(by1, p->pt[i].y);
    }
    if (p->npt && bx1 > bx0 && by1 > by0 && bx1 - bx0 < 2000 && by1 - by0 < 2000) {
        int l = (int)floorf(bx0), t = (int)floorf(by0), gw = (int)ceilf(bx1) - l + 1, gh = (int)ceilf(by1) - t + 1;
        for (int i = 0; i < p->npt; i++) { p->pt[i].x -= (float)l; p->pt[i].y -= (float)t; }
        D->w = gw;
        D->cov = of_alloc(((size_t)gw + 4) * 2 * sizeof(float));
        a = D->cov ? of_alloc((size_t)gw * (size_t)gh) : NULL;
        if (a) {
            fe_t FE = { .D = D };
            flatten(p, fe_seg, fe_end, &FE);
            gbm_t B = { a, gw, gh };
            raster(D, false, SUB_GLYPH, 0, 0, gw, gh, gbm_row, &B);
            *w = gw; *h = gh; *left = l; *top = -t;
        }
    }
    of_path_free(&D->tmp);
    of_free(D->E);
    of_free(D->cov);
    of_free(D);
    return a;
}

static void run_charproc(dc_t *D, rfont_t *F, uint32_t code, of_mtx_t trm, const pv_t *res)
{
    if (D->t3depth > 2 || !F->charprocs) return;
    const font_t *t = F->t;
    const char *name = t->names[code & 255];
    char buf[16];
    if (!name) {
        /* the standard name of the encoding's character */
        const char *u = of_pdff_text(t, code, buf);
        if (!u || u[1]) return;
        snprintf(buf, sizeof buf, "%c", u[0]);
        name = buf;
    }
    const pv_t *ref = pv_get(pdf_res(D->R->P, F->charprocs), name);
    const pv_t *s = pdf_res(D->R->P, ref);
    if (!s || s->t != PV_STREAM) return;
    size_t n;
    uint8_t *d = pdf_stream(D->R->P, s, ref->t == PV_REF ? ref->ref.num : 0, &n, NULL);
    if (!d) return;
    if (D->ng < MAX_GS) {
        int floor_ng = D->ng;
        gs_push(D);
        G(D)->ctm = of_mtx_mul(F->fm3, trm);
        of_mtx_t tm = D->tm, tlm = D->tlm;
        of_path_t saved = D->path;
        bool off = D->t3color_off;
        D->path = (of_path_t){ 0 };
        D->t3depth++;
        run_stream(D, d, n, F->res3 ? F->res3 : res);
        D->t3depth--;
        of_path_free(&D->path);
        D->path = saved;
        D->t3color_off = off;
        D->tm = tm;
        D->tlm = tlm;
        while (D->ng > floor_ng) gs_pop(D, floor_ng);
    }
    of_free(d);
}

static void show(dc_t *D, const pv_t *str, const pv_t *res)
{
    gs_t *g = G(D);
    rfont_t *F = g->font;
    if (!F || str->t != PV_STR) return;
    const font_t *t = F->t;
    const uint8_t *s = (const uint8_t *)str->s.s;
    int n = str->s.n;
    int mode = g->tr;
    bool fill = mode == 0 || mode == 2 || mode == 4 || mode == 6;
    bool strk = mode == 1 || mode == 2 || mode == 5 || mode == 6;
    paint_t P = { 0 }, PS = { 0 };
    bool havep = fill && paint_from(D, 0, &P);
    bool haves = strk && paint_from(D, 1, &PS);
    for (int i = 0; i < n && !D->stop;) {
        bool one = !t->type0;
        uint32_t code = rf_next(F, s, n, &i);
        char buf[16];
        const char *txt = of_pdff_text(t, code, buf);
        float w = of_pdff_width(t, code, txt);
        float w0 = t->type3 ? w * (float)t->fm_a : w / 1000.0f;
        of_mtx_t tsm = { g->fs * g->th, 0, 0, g->fs, 0, g->ts };
        of_mtx_t trm = of_mtx_mul(of_mtx_mul(tsm, D->tm), g->ctm);
        if (mode != 3 && mode != 7) {
            if (t->type3) {
                run_charproc(D, F, code, trm, res);
                g = G(D);
            } else {
                int gid = rf_gid(F, code);
                if (gid > 0) {
                    if (fill && havep) {
                        float ox, oy;
                        struct gce *e = glyph_bitmap(D, F, gid, trm, &ox, &oy);
                        if (e) blit_glyph(D, e, ox, oy, &P);
                        else {
                            of_path_t *p = &D->tmp;
                            of_path_reset(p);
                            of_fprog_outline(&F->prog, gid, trm, p);
                            fill_path_with(D, p, false, &P);
                        }
                    }
                    if (strk && haves) {
                        of_path_t gp = { 0 };
                        of_fprog_outline(&F->prog, gid, trm, &gp);
                        stroke_path_with(D, &gp, &PS);
                        of_path_free(&gp);
                    }
                } else if (txt && (fill || strk)) {
                    int l;
                    uint32_t cp = of_utf8_get(txt, &l);
                    fallback_glyph(D, F, cp, trm, w0, fill ? &P : &PS);
                }
            }
        }
        float tx = (w0 * g->fs + g->tc + (one && code == 32 ? g->tw : 0)) * g->th;
        D->tm.e += tx * D->tm.a;
        D->tm.f += tx * D->tm.b;
    }
    if (havep) paint_done(&P);
    if (haves) paint_done(&PS);
}

/* -------------------------------------------------------------------------- */
/* The content stream                                                          */
/* -------------------------------------------------------------------------- */

static void gs_push(dc_t *D)
{
    if (D->ng >= MAX_GS) { D->ng++; return; }       /* counted, not kept */
    D->g[D->ng] = D->g[D->ng - 1];
    if (D->g[D->ng].mask) D->g[D->ng].mask->ref++;
    D->ng++;
}

static void gs_pop(dc_t *D, int floor_ng)
{
    if (D->ng <= floor_ng) return;
    if (D->ng > MAX_GS) { D->ng--; return; }
    mask_unref(D->g[D->ng - 1].mask);
    D->ng--;
}

static void set_color(dc_t *D, int which, const float *c, int nc)
{
    gs_t *g = G(D);
    if (D->t3color_off) return;
    csi_t *cs = g->cs[which];
    for (int i = 0; i < 8; i++) g->col[which][i] = i < nc ? c[i] : 0;
    if (cs->kind == CS_PAT) return;
    float rgb[3];
    to_rgb(cs, g->col[which], rgb);
    for (int i = 0; i < 3; i++) g->rgb[which][i] = (uint8_t)(clampf(rgb[i], 0, 1) * 255 + 0.5f);
}

static void set_cs(dc_t *D, int which, csi_t *cs)
{
    gs_t *g = G(D);
    if (D->t3color_off) return;
    g->cs[which] = cs;
    g->pat[which] = NULL;
    float c[8];
    cs_initial(cs, c);
    set_color(D, which, c, 8);
}

static void ext_gstate(dc_t *D, const pv_t *res, const char *name)
{
    pdf_t *P = D->R->P;
    const pv_t *eg = pdf_get(P, pdf_get(P, res, "ExtGState"), name);
    if (!eg || eg->t != PV_DICT) return;
    gs_t *g = G(D);
    for (int i = 0; i < eg->d.n; i++) {
        const char *k = eg->d.k[i];
        const pv_t *v = pdf_res(P, &eg->d.v[i]);
        if (!strcmp(k, "LW")) g->lw = fnum(v, g->lw);
        else if (!strcmp(k, "LC")) g->cap = pvi(v, 0);
        else if (!strcmp(k, "LJ")) g->join = pvi(v, 0);
        else if (!strcmp(k, "ML")) g->ml = fnum(v, 10);
        else if (!strcmp(k, "CA")) g->alpha[1] = clampf(fnum(v, 1), 0, 1);
        else if (!strcmp(k, "ca")) g->alpha[0] = clampf(fnum(v, 1), 0, 1);
        else if (!strcmp(k, "D") && v && v->t == PV_ARR && v->a.n >= 2) {
            const pv_t *a = pdf_res(P, &v->a.v[0]);
            g->ndash = 0;
            for (int j = 0; a && a->t == PV_ARR && j < a->a.n && j < 16; j++) g->dash[g->ndash++] = fnum(&a->a.v[j], 0);
            g->dphase = fnum(&v->a.v[1], 0);
        } else if (!strcmp(k, "Font") && v && v->t == PV_ARR && v->a.n >= 2) {
            g->font = rfont_get(D->R, of_pdff_load(P, &D->R->fonts, &v->a.v[0]));
            g->fs = fnum(&v->a.v[1], 12);
        }
    }
}

static void run_form(dc_t *D, const pv_t *x, int num, const pv_t *res, const of_mtx_t *extra)
{
    pdf_t *P = D->R->P;
    if (D->depth >= MAX_DEPTH) return;
    size_t len;
    uint8_t *d = pdf_stream(P, x, num, &len, NULL);
    if (!d) return;
    int floor_ng = D->ng;
    gs_push(D);
    gs_t *g = G(D);
    if (extra) g->ctm = of_mtx_mul(*extra, g->ctm);
    float m[6] = { 1, 0, 0, 1, 0, 0 };
    arr_f(pdf_get(P, x, "Matrix"), m, 6, NULL);
    g->ctm = of_mtx_mul((of_mtx_t){ m[0], m[1], m[2], m[3], m[4], m[5] }, g->ctm);
    /* the form's box clips it */
    float bb[4];
    int nb = 0;
    arr_f(pdf_get(P, x, "BBox"), bb, 4, &nb);
    if (nb == 4) {
        of_path_t *p = &D->tmp;
        of_path_reset(p);
        float px, py;
        pt_dev(&g->ctm, bb[0], bb[1], &px, &py); of_path_move(p, px, py);
        pt_dev(&g->ctm, bb[2], bb[1], &px, &py); of_path_line(p, px, py);
        pt_dev(&g->ctm, bb[2], bb[3], &px, &py); of_path_line(p, px, py);
        pt_dev(&g->ctm, bb[0], bb[3], &px, &py); of_path_line(p, px, py);
        of_path_close(p);
        clip_path(D, p, false);
    }
    const pv_t *fr = pdf_get(P, x, "Resources");
    of_mtx_t tm = D->tm, tlm = D->tlm;
    of_path_t saved = D->path;
    D->path = (of_path_t){ 0 };
    D->depth++;
    run_stream(D, d, len, fr && fr->t == PV_DICT ? fr : res);
    D->depth--;
    of_path_free(&D->path);
    D->path = saved;
    D->tm = tm;
    D->tlm = tlm;
    while (D->ng > floor_ng) gs_pop(D, floor_ng);
    of_free(d);
}

static void op_do(dc_t *D, const pv_t *res, const char *name)
{
    pdf_t *P = D->R->P;
    const pv_t *xd = pdf_get(P, res, "XObject");
    const pv_t *ref = pv_get(pdf_res(P, xd), name);
    const pv_t *x = pdf_res(P, ref);
    if (!x || x->t != PV_STREAM) return;
    int num = ref && ref->t == PV_REF ? ref->ref.num : 0;
    const pv_t *st = pdf_get(P, x, "Subtype");
    if (pv_is_name(st, "Image")) { draw_xobj_img(D, x, num, NULL, 0, res); return; }
    if (pv_is_name(st, "Form")) run_form(D, x, num, res, NULL);
}

/* "BI ... ID data EI": the dictionary into a value of its own, then the picture */
static void inline_image(dc_t *D, lx_t *L, const pv_t *res)
{
    pv_t dict = { .t = PV_DICT };
    int cap = 0;
    for (int guard = 0; guard < 64; guard++) {
        pv_t k;
        if (!pv_parse(NULL, L, &k)) break;
        if (k.t == PV_OP && !strcmp(k.s.s, "ID")) { pv_free(&k); break; }
        if (k.t != PV_NAME) { pv_free(&k); continue; }
        pv_t v;
        if (!pv_parse(NULL, L, &v)) { pv_free(&k); break; }
        if (dict.d.n == cap) {
            cap = cap ? cap * 2 : 8;
            char **nk = of_realloc(dict.d.k, (size_t)cap * sizeof *nk);
            pv_t *nv = of_realloc(dict.d.v, (size_t)cap * sizeof *nv);
            if (nk) dict.d.k = nk;
            if (nv) dict.d.v = nv;
            if (!nk || !nv) { pv_free(&k); pv_free(&v); break; }
        }
        dict.d.k[dict.d.n] = k.s.s;     /* the name's text, taken */
        dict.d.v[dict.d.n] = v;
        dict.d.n++;
    }
    if (L->p < L->n) L->p++;            /* the one white space after ID */
    size_t start = L->p;
    /* the data's end: "EI" between white spaces (the length, when the filter is plain, says it better) */
    while (L->p + 2 <= L->n && !(L->b[L->p] == 'E' && L->b[L->p + 1] == 'I' && (L->p == 0 || L->b[L->p - 1] <= ' ') &&
                                 (L->p + 2 == L->n || L->b[L->p + 2] <= ' ')))
        L->p++;
    size_t end = L->p;
    while (end > start && (L->b[end - 1] == '\n' || L->b[end - 1] == '\r')) end--;
    L->p = L->p + 2 <= L->n ? L->p + 2 : L->n;
    if (end > start) draw_xobj_img(D, &dict, 0, L->b + start, end - start, res);
    pv_free(&dict);
}

static void annots(dc_t *D, const pv_t *page)
{
    pdf_t *P = D->R->P;
    const pv_t *an = pdf_get(P, page, "Annots");
    for (int i = 0; an && an->t == PV_ARR && i < an->a.n && i < 500; i++) {
        const pv_t *a = pdf_res(P, &an->a.v[i]);
        if (!a || a->t != PV_DICT) continue;
        int fl = pvi(pdf_get(P, a, "F"), 0);
        if (fl & (2 | 32)) continue;
        if (pv_is_name(pdf_get(P, a, "Subtype"), "Popup")) continue;
        const pv_t *ap = pdf_get(P, a, "AP");
        const pv_t *nref = ap ? pv_get(pdf_res(P, ap), "N") : NULL;
        const pv_t *n = pdf_res(P, nref);
        if (n && n->t == PV_DICT) {
            /* states: the one it is in */
            const pv_t *as = pdf_get(P, a, "AS");
            nref = as && as->t == PV_NAME ? pv_get(n, as->s.s) : NULL;
            n = pdf_res(P, nref);
        }
        if (!n || n->t != PV_STREAM) continue;
        float r[4];
        int nr = 0;
        arr_f(pdf_get(P, a, "Rect"), r, 4, &nr);
        if (nr != 4) continue;
        float bb[4] = { 0, 0, 1, 1 }, m[6] = { 1, 0, 0, 1, 0, 0 };
        arr_f(pdf_get(P, n, "BBox"), bb, 4, NULL);
        arr_f(pdf_get(P, n, "Matrix"), m, 6, NULL);
        of_mtx_t fm = { m[0], m[1], m[2], m[3], m[4], m[5] };
        float xs[4], ys[4];
        pt_dev(&fm, bb[0], bb[1], &xs[0], &ys[0]); pt_dev(&fm, bb[2], bb[1], &xs[1], &ys[1]);
        pt_dev(&fm, bb[0], bb[3], &xs[2], &ys[2]); pt_dev(&fm, bb[2], bb[3], &xs[3], &ys[3]);
        float tx0 = fminf(fminf(xs[0], xs[1]), fminf(xs[2], xs[3])), tx1 = fmaxf(fmaxf(xs[0], xs[1]), fmaxf(xs[2], xs[3]));
        float ty0 = fminf(fminf(ys[0], ys[1]), fminf(ys[2], ys[3])), ty1 = fmaxf(fmaxf(ys[0], ys[1]), fmaxf(ys[2], ys[3]));
        if (tx1 - tx0 < 1e-3f || ty1 - ty0 < 1e-3f) continue;
        float rx0 = fminf(r[0], r[2]), rx1 = fmaxf(r[0], r[2]), ry0 = fminf(r[1], r[3]), ry1 = fmaxf(r[1], r[3]);
        float sx = (rx1 - rx0) / (tx1 - tx0), sy = (ry1 - ry0) / (ty1 - ty0);
        of_mtx_t A = { sx, 0, 0, sy, rx0 - tx0 * sx, ry0 - ty0 * sy };
        run_form(D, n, nref && nref->t == PV_REF ? nref->ref.num : 0, NULL, &A);
    }
}

static void run_stream(dc_t *D, const uint8_t *d, size_t n, const pv_t *res)
{
    pdf_t *P = D->R->P;
    lx_t L = { d, n, 0 };
    pv_t op[32];
    int nop = 0;
    int floor_ng = D->ng;
    for (;;) {
        if (D->stop) break;
        if ((++D->ops & 255) == 0 && D->cancel && *D->cancel) { D->stop = true; break; }
        pv_t v;
        if (!pv_parse(NULL, &L, &v)) break;
        if (v.t != PV_OP) {
            if (nop == 32) { pv_free(&op[0]); memmove(op, op + 1, 31 * sizeof op[0]); nop--; }
            op[nop++] = v;
            continue;
        }
        const char *o = v.s.s;
        gs_t *g = G(D);
        /* the last k operands, as numbers */
#define AR(k, i) fnum(&op[nop - (k) + (i)], 0)
        of_path_t *path = &D->path;
        switch (o[0]) {
        case 'q': if (!o[1]) gs_push(D); break;
        case 'Q': if (!o[1]) gs_pop(D, floor_ng); break;
        case 'c':
            if (!strcmp(o, "cm") && nop >= 6) {
                of_mtx_t m = { AR(6, 0), AR(6, 1), AR(6, 2), AR(6, 3), AR(6, 4), AR(6, 5) };
                g->ctm = of_mtx_mul(m, g->ctm);
            } else if (!o[1] && nop >= 6) {
                float x1, y1, x2, y2, x3, y3;
                pt_dev(&g->ctm, AR(6, 0), AR(6, 1), &x1, &y1);
                pt_dev(&g->ctm, AR(6, 2), AR(6, 3), &x2, &y2);
                pt_dev(&g->ctm, AR(6, 4), AR(6, 5), &x3, &y3);
                of_path_cubic(path, x1, y1, x2, y2, x3, y3);
            } else if (!strcmp(o, "cs") && nop >= 1) set_cs(D, 0, cs_get(D->R, &op[nop - 1], res, 0));
            break;
        case 'C': if (!strcmp(o, "CS") && nop >= 1) set_cs(D, 1, cs_get(D->R, &op[nop - 1], res, 0)); break;
        case 'v':
            if (!o[1] && nop >= 4 && path->npt) {
                of_pt_t c = path->pt[path->npt - 1];
                float x2, y2, x3, y3;
                pt_dev(&g->ctm, AR(4, 0), AR(4, 1), &x2, &y2);
                pt_dev(&g->ctm, AR(4, 2), AR(4, 3), &x3, &y3);
                of_path_cubic(path, c.x, c.y, x2, y2, x3, y3);
            }
            break;
        case 'y':
            if (!o[1] && nop >= 4) {
                float x1, y1, x3, y3;
                pt_dev(&g->ctm, AR(4, 0), AR(4, 1), &x1, &y1);
                pt_dev(&g->ctm, AR(4, 2), AR(4, 3), &x3, &y3);
                of_path_cubic(path, x1, y1, x3, y3, x3, y3);
            }
            break;
        case 'm': if (!o[1] && nop >= 2) { float x, y; pt_dev(&g->ctm, AR(2, 0), AR(2, 1), &x, &y); of_path_move(path, x, y); } break;
        case 'l': if (!o[1] && nop >= 2) { float x, y; pt_dev(&g->ctm, AR(2, 0), AR(2, 1), &x, &y); of_path_line(path, x, y); } break;
        case 'h': if (!o[1]) of_path_close(path); break;
        case 'r':
            if (!strcmp(o, "re") && nop >= 4) {
                float x = AR(4, 0), y = AR(4, 1), w = AR(4, 2), h = AR(4, 3), px, py;
                pt_dev(&g->ctm, x, y, &px, &py); of_path_move(path, px, py);
                pt_dev(&g->ctm, x + w, y, &px, &py); of_path_line(path, px, py);
                pt_dev(&g->ctm, x + w, y + h, &px, &py); of_path_line(path, px, py);
                pt_dev(&g->ctm, x, y + h, &px, &py); of_path_line(path, px, py);
                of_path_close(path);
            } else if (!strcmp(o, "rg") && nop >= 3) {
                if (!D->t3color_off) { g->cs[0] = &CS_DEV_RGB; float c[3] = { AR(3, 0), AR(3, 1), AR(3, 2) }; set_color(D, 0, c, 3); }
            } else if (!strcmp(o, "ri")) {
            }
            break;
        case 'R': if (!strcmp(o, "RG") && nop >= 3 && !D->t3color_off) { g->cs[1] = &CS_DEV_RGB; float c[3] = { AR(3, 0), AR(3, 1), AR(3, 2) }; set_color(D, 1, c, 3); } break;
        case 'g':
            if (!o[1] && nop >= 1 && !D->t3color_off) { g->cs[0] = &CS_DEV_GRAY; float c = AR(1, 0); set_color(D, 0, &c, 1); }
            else if (!strcmp(o, "gs") && nop >= 1 && op[nop - 1].t == PV_NAME) ext_gstate(D, res, op[nop - 1].s.s);
            break;
        case 'G': if (!o[1] && nop >= 1 && !D->t3color_off) { g->cs[1] = &CS_DEV_GRAY; float c = AR(1, 0); set_color(D, 1, &c, 1); } break;
        case 'k': if (!o[1] && nop >= 4 && !D->t3color_off) { g->cs[0] = &CS_DEV_CMYK; float c[4] = { AR(4, 0), AR(4, 1), AR(4, 2), AR(4, 3) }; set_color(D, 0, c, 4); } break;
        case 'K': if (!o[1] && nop >= 4 && !D->t3color_off) { g->cs[1] = &CS_DEV_CMYK; float c[4] = { AR(4, 0), AR(4, 1), AR(4, 2), AR(4, 3) }; set_color(D, 1, c, 4); } break;
        case 's': case 'S':
            if (!strcmp(o, "sc") || !strcmp(o, "scn") || !strcmp(o, "SC") || !strcmp(o, "SCN")) {
                int which = o[0] == 'S' ? 1 : 0;
                if (D->t3color_off) break;
                float c[8];
                int k = 0;
                for (int i = 0; i < nop && k < 8; i++) if (op[i].t == PV_INT || op[i].t == PV_REAL) c[k++] = fnum(&op[i], 0);
                if (g->cs[which]->kind == CS_PAT) {
                    if (nop >= 1 && op[nop - 1].t == PV_NAME) g->pat[which] = pv_get(pdf_res(P, pdf_get(P, res, "Pattern")), op[nop - 1].s.s);
                    for (int i = 0; i < 8; i++) g->col[which][i] = i < k ? c[i] : 0;
                } else {
                    set_color(D, which, c, k);
                }
            } else if (!strcmp(o, "sh") && nop >= 1 && op[nop - 1].t == PV_NAME) {
                op_sh(D, res, op[nop - 1].s.s);
            } else if (!o[1] || (o[0] == 's' && !o[1])) {
                /* S: stroke; s: close and stroke */
                if (o[0] == 's') of_path_close(path);
                stroke_path(D, path);
                goto paint_end;
            }
            break;
        case 'f': case 'F':
            if (!o[1] || (o[1] == '*' && !o[2])) { fill_path(D, path, o[1] == '*'); goto paint_end; }
            break;
        case 'B': case 'b':
            if (!strcmp(o, "BT")) { D->tm = D->tlm = (of_mtx_t){ 1, 0, 0, 1, 0, 0 }; break; }
            if (!strcmp(o, "BI")) { inline_image(D, &L, res); break; }
            if (!o[1] || (o[1] == '*' && !o[2])) {
                if (o[0] == 'b') of_path_close(path);
                fill_path(D, path, o[1] == '*');
                stroke_path(D, path);
                goto paint_end;
            }
            break;
        case 'n':
            if (!o[1]) goto paint_end;
            break;
        case 'W':
            if (!o[1] || (o[1] == '*' && !o[2])) { D->clip_pending = true; D->clip_eo = o[1] == '*'; }
            break;
        case 'w': if (!o[1] && nop >= 1) g->lw = AR(1, 0); break;
        case 'J': if (!o[1] && nop >= 1) g->cap = si(AR(1, 0)); break;
        case 'j': if (!o[1] && nop >= 1) g->join = si(AR(1, 0)); break;
        case 'M': if (!o[1] && nop >= 1) g->ml = AR(1, 0); break;
        case 'd':
            if (!o[1] && nop >= 2 && op[nop - 2].t == PV_ARR) {
                const pv_t *ar = &op[nop - 2];
                g->ndash = 0;
                for (int i = 0; i < ar->a.n && i < 16; i++) g->dash[g->ndash++] = fnum(&ar->a.v[i], 0);
                g->dphase = AR(1, 0);
            } else if ((!strcmp(o, "d0") || !strcmp(o, "d1"))) {
                if (o[1] == '1') D->t3color_off = true;
            }
            break;
        case 'T':
            switch (o[1]) {
            case 'f':
                if (nop >= 2 && op[nop - 2].t == PV_NAME) {
                    const pv_t *fonts = pdf_get(P, res, "Font");
                    g->font = rfont_get(D->R, of_pdff_load(P, &D->R->fonts, pv_get(pdf_res(P, fonts), op[nop - 2].s.s)));
                    g->fs = AR(1, 0);
                }
                break;
            case 'c': if (nop >= 1) g->tc = AR(1, 0); break;
            case 'w': if (nop >= 1) g->tw = AR(1, 0); break;
            case 'z': g->th = (nop >= 1 ? AR(1, 0) : 100) / 100.0f; break;
            case 'L': if (nop >= 1) g->tl = AR(1, 0); break;
            case 'r': if (nop >= 1) g->tr = si(AR(1, 0)); break;
            case 's': if (nop >= 1) g->ts = AR(1, 0); break;
            case 'd': case 'D':
                if (nop >= 2) {
                    float tx = AR(2, 0), ty = AR(2, 1);
                    if (o[1] == 'D') g->tl = -ty;
                    D->tlm = of_mtx_mul((of_mtx_t){ 1, 0, 0, 1, tx, ty }, D->tlm);
                    D->tm = D->tlm;
                }
                break;
            case 'm':
                if (nop >= 6) {
                    D->tlm = (of_mtx_t){ AR(6, 0), AR(6, 1), AR(6, 2), AR(6, 3), AR(6, 4), AR(6, 5) };
                    D->tm = D->tlm;
                }
                break;
            case '*':
                D->tlm = of_mtx_mul((of_mtx_t){ 1, 0, 0, 1, 0, -g->tl }, D->tlm);
                D->tm = D->tlm;
                break;
            case 'j': if (nop >= 1) show(D, &op[nop - 1], res); break;
            case 'J':
                if (nop >= 1 && op[nop - 1].t == PV_ARR) {
                    const pv_t *ar = &op[nop - 1];
                    for (int i = 0; i < ar->a.n && !D->stop; i++) {
                        if (ar->a.v[i].t == PV_STR) show(D, &ar->a.v[i], res);
                        else {
                            gs_t *gg = G(D);
                            float tx = -fnum(&ar->a.v[i], 0) / 1000.0f * gg->fs * gg->th;
                            D->tm.e += tx * D->tm.a;
                            D->tm.f += tx * D->tm.b;
                        }
                    }
                }
                break;
            }
            break;
        case '\'': case '"':
            D->tlm = of_mtx_mul((of_mtx_t){ 1, 0, 0, 1, 0, -g->tl }, D->tlm);
            D->tm = D->tlm;
            if (o[0] == '"' && nop >= 3) { g->tw = AR(3, 0); g->tc = AR(3, 1); }
            if (nop >= 1) show(D, &op[nop - 1], res);
            break;
        case 'D': if (!strcmp(o, "Do") && nop >= 1 && op[nop - 1].t == PV_NAME) op_do(D, res, op[nop - 1].s.s); break;
        default: break;
        }
        goto next;
    paint_end:
        if (D->clip_pending) { clip_path(D, path, D->clip_eo); D->clip_pending = false; }
        of_path_reset(path);
    next:
#undef AR
        pv_free(&v);
        for (int i = 0; i < nop; i++) pv_free(&op[i]);
        nop = 0;
    }
    for (int i = 0; i < nop; i++) pv_free(&op[i]);
    while (D->ng > floor_ng) gs_pop(D, floor_ng);
}

/* -------------------------------------------------------------------------- */
/* The document                                                                */
/* -------------------------------------------------------------------------- */

of_pdfr_t *of_pdfr_open(const char *path, const char *password, char *err, size_t errlen)
{
    pdf_t *P = pdf_open(path, password, err, errlen);
    if (!P) return NULL;
    of_pdfr_t *R = of_alloc(sizeof *R);
    if (!R) { pdf_close(P); snprintf(err, errlen, "memory"); return NULL; }
    R->P = P;
    snprintf(R->path, sizeof R->path, "%s", path);
    R->npages = of_pdf_pages(P, &R->pages);
    R->file_id = of_crc32(0, path, strlen(path)) ^ (uint32_t)P->len;
    R->img_page = -1;
    if (!R->npages) { of_pdfr_close(R); snprintf(err, errlen, "damaged"); return NULL; }
    return R;
}

void of_pdfr_close(of_pdfr_t *R)
{
    if (!R) return;
    for (int i = 0; i < R->nrf; i++) rfont_free(R->rf[i]);
    of_free(R->rf);
    of_pdff_free(&R->fonts);
    fn_free(R->fns);
    for (csi_t *c = R->css; c;) { csi_t *n = c->next; of_free(c->lut); of_free(c); c = n; }
    gc_clear(R);
    of_free(R->gc);
    for (int i = 0; i < R->nimg; i++) img_free(R->img[i]);
    of_free(R->pages);
    pdf_close(R->P);
    of_free(R);
}

int of_pdfr_count(const of_pdfr_t *R) { return R ? R->npages : 0; }

void of_pdfr_size(const of_pdfr_t *R, int page, float *w, float *h)
{
    *w = 612; *h = 792;
    if (!R || page < 0 || page >= R->npages) return;
    const pref_t *p = &R->pages[page];
    float bw = (float)(p->box[2] - p->box[0]), bh = (float)(p->box[3] - p->box[1]);
    if (bw < 1) bw = 612;
    if (bh < 1) bh = 792;
    bool side = p->rot == 90 || p->rot == 270;
    *w = side ? bh : bw;
    *h = side ? bw : bh;
}

bool of_pdfr_draw(of_pdfr_t *R, int page, float scale, int ox, int oy, uint16_t *px, int w, int h, volatile bool *cancel)
{
    if (!R || page < 0 || page >= R->npages || w <= 0 || h <= 0) return false;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) px[i] = 0xFFFF;
    if (R->img_page != page) {
        for (int i = 0; i < R->nimg; i++) img_free(R->img[i]);
        R->nimg = 0;
        R->img_page = page;
    }
    dc_t *D = of_alloc(sizeof *D);
    if (!D) return false;
    D->R = R;
    D->px = px;
    D->w = w;
    D->h = h;
    D->cancel = cancel;
    D->page = page;
    D->cov = of_alloc(((size_t)w + 4) * 2 * sizeof(float));
    if (!D->cov) { of_free(D); return false; }
    const pref_t *pr = &R->pages[page];
    float s = scale, bx0 = (float)pr->box[0], by0 = (float)pr->box[1], bx1 = (float)pr->box[2], by1 = (float)pr->box[3];
    of_mtx_t b;
    switch (pr->rot) {
    case 90:  b = (of_mtx_t){ 0, s, s, 0, -by0 * s - (float)ox, -bx0 * s - (float)oy }; break;
    case 180: b = (of_mtx_t){ -s, 0, 0, s, bx1 * s - (float)ox, -by0 * s - (float)oy }; break;
    case 270: b = (of_mtx_t){ 0, -s, -s, 0, by1 * s - (float)ox, bx1 * s - (float)oy }; break;
    default:  b = (of_mtx_t){ s, 0, 0, -s, -bx0 * s - (float)ox, by1 * s - (float)oy }; break;
    }
    D->base = b;
    D->ng = 1;
    gs_t *g = &D->g[0];
    g->ctm = b;
    g->cs[0] = g->cs[1] = &CS_DEV_GRAY;
    g->alpha[0] = g->alpha[1] = 1;
    g->lw = 1;
    g->ml = 10;
    g->th = 1;
    g->cx0 = 0; g->cy0 = 0; g->cx1 = w; g->cy1 = h;
    /* the page's box clips it */
    {
        float x0, y0, x1, y1;
        pt_dev(&b, bx0, by0, &x0, &y0);
        pt_dev(&b, bx1, by1, &x1, &y1);
        int ix0 = (int)floorf(fminf(x0, x1) + 0.5f), iy0 = (int)floorf(fminf(y0, y1) + 0.5f);
        int ix1 = (int)floorf(fmaxf(x0, x1) + 0.5f), iy1 = (int)floorf(fmaxf(y0, y1) + 0.5f);
        if (ix0 > 0) g->cx0 = ix0;
        if (iy0 > 0) g->cy0 = iy0;
        if (ix1 < w) g->cx1 = ix1;
        if (iy1 < h) g->cy1 = iy1;
    }
    const pv_t *cref = pv_get(pr->page, "Contents");
    const pv_t *c = pdf_res(R->P, cref);
    of_sb_t all = { 0 };
    if (c && c->t == PV_STREAM) {
        size_t n;
        uint8_t *d = pdf_stream(R->P, c, cref && cref->t == PV_REF ? cref->ref.num : 0, &n, NULL);
        if (d) { of_sb_put(&all, (const char *)d, n); of_free(d); }
    } else if (c && c->t == PV_ARR) {
        for (int i = 0; i < c->a.n; i++) {
            const pv_t *st = pdf_res(R->P, &c->a.v[i]);
            size_t n;
            uint8_t *d = st ? pdf_stream(R->P, st, c->a.v[i].t == PV_REF ? c->a.v[i].ref.num : 0, &n, NULL) : NULL;
            if (d) { of_sb_put(&all, (const char *)d, n); of_sb_putc(&all, '\n'); of_free(d); }
        }
    }
    if (all.n) run_stream(D, (const uint8_t *)all.s, all.n, pr->res);
    of_sb_free(&all);
    if (!D->stop) annots(D, pr->page);
    bool ok = !D->stop;
    while (D->ng > 0) { if (D->ng <= MAX_GS) mask_unref(D->g[D->ng - 1].mask); D->ng--; }
    of_path_free(&D->path);
    of_path_free(&D->tmp);
    of_free(D->E);
    of_free(D->cov);
    of_free(D);
    return ok;
}
