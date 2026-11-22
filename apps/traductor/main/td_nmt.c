/*
 * TRADUCTOR - the neural translator (see td_nmt.h)
 *
 * The maths, as Marian does it for these models:
 *   embedding  x = E[w] * sqrt(D) + sinusoid(pos)   (first half sin, then cos)
 *   encoder    x = LN(x + Att(x, x));  x = LN(x + W2 relu(W1 x))
 *   decoder    the step's input is E[prev] * sqrt(D) + sinusoid(t), 0 at t = 0
 *              SSRU: f = sigmoid(Wf y + bf); c = f c + (1 - f) W y; y = LN(y + relu(c))
 *              y = LN(y + Att(y, enc));  y = LN(y + W2 relu(W1 y))
 *   output     logits = E y + b over the shortlist only
 * LN is a layer norm with eps 1e-9. A matrix is int8, each output's inputs in
 * a row, times its scale.
 */
#include "td_nmt.h"
#include "td_spm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENC   6
#define MAX_DEC   2
#define MAX_SRC   160       /* pieces a segment; longer ones are cut          */
#define MAX_HEADS 8

typedef struct {
    const int8_t *q;
    float s, sa;
    int out, in;
} m8_t;

typedef struct {
    m8_t aq, ak, av, ao, f1, f2;
    const float *abq, *abk, *abv, *abo, *als, *alb, *fb1, *fb2, *fls, *flb;
} enc_t;

typedef struct {
    m8_t r, rf, cq, ck, cv, co, f1, f2;
    const float *rbf, *rls, *rlb, *cbq, *cbk, *cbv, *cbo, *cls, *clb, *fb1, *fb2, *fls, *flb;
} dec_t;

struct td_nmt {
    int D, H, F, V;
    enc_t enc[MAX_ENC];
    dec_t dec[MAX_DEC];
    m8_t emb;
    const float *ob;
    td_spm_t spm;
    /* shortlist */
    int lex_first, lex_nsrc;
    const uint32_t *lex_off;
    const uint16_t *lex_ids;
    uint32_t lex_nids;
    /* the embedding rows from the card, when not in memory */
    td_pak_t *pak;
    uint32_t emb_off;
    int emb_mode;
    /* what was loaded */
    void *bufs[160];
    int nbufs;
    size_t bytes;
    /* options */
    bool int8;
    td_nmt_par_fn par;
    void *par_ctx;
    volatile bool *cancel;
    uint32_t (*clock)(void);
    uint64_t macs;
    bool oom;
    /* scratch for one segment */
    uint8_t *mark;          /* V bytes: candidate set                        */
};

static int8_t *s_xq;            /* scratch for the int8 activations         */
static size_t s_xq_len;

/* ---------------------------------------------------------------- loading */

static const void *grab(td_nmt_t *m, td_pak_t *p, const char *dir, const char *name,
                        uint32_t type, const td_entry_t **out)
{
    char full[TD_NAME_LEN + 8];
    snprintf(full, sizeof full, "%s.%s", dir, name);
    const td_entry_t *e = td_pak_find(p, full);
    if (!e || e->type != type) return NULL;
    if (m->nbufs >= (int)(sizeof m->bufs / sizeof m->bufs[0])) return NULL;
    void *b = td_pak_load(p, e);
    if (!b) {
        m->oom = true;
        return NULL;
    }
    m->bufs[m->nbufs++] = b;
    m->bytes += e->len;
    if (out) *out = e;
    return b;
}

static bool mat(td_nmt_t *m, td_pak_t *p, const char *dir, const char *name, m8_t *w, int out, int in)
{
    const td_entry_t *e;
    w->q = (const int8_t *)grab(m, p, dir, name, TD_T_MAT8, &e);
    if (!w->q || e->rows != out || e->cols != in || e->len != (uint32_t)out * in) return false;
    w->s = e->scale;
    w->sa = e->scale_a;
    w->out = out;
    w->in = in;
    return true;
}

static bool vec(td_nmt_t *m, td_pak_t *p, const char *dir, const char *name, const float **v, int n)
{
    const td_entry_t *e;
    *v = (const float *)grab(m, p, dir, name, TD_T_VECF, &e);
    return *v && e->len == (uint32_t)n * 4;
}

void td_nmt_free(td_nmt_t *m)
{
    if (!m) return;
    for (int i = 0; i < m->nbufs; i++) td_free(m->bufs[i]);
    td_spm_free(&m->spm);
    td_free(m->mark);
    free(m);
    td_free(s_xq);
    s_xq = NULL;
    s_xq_len = 0;
}

size_t td_nmt_bytes(const td_nmt_t *m)
{
    return m ? m->bytes : 0;
}

td_nmt_t *td_nmt_load(td_pak_t *p, const char *dir, int emb_mode, const char **err)
{
    static const char *const no_model = "no model";
    static const char *const no_mem = "no memory";
    char nm[16];
    td_nmt_t *m = (td_nmt_t *)calloc(1, sizeof *m);
    if (!m) {
        if (err) *err = no_mem;
        return NULL;
    }
    char full[TD_NAME_LEN + 8];
    snprintf(full, sizeof full, "%s.emb", dir);
    const td_entry_t *ee = td_pak_find(p, full);
    if (!ee || ee->type != TD_T_MAT8) {
        free(m);
        if (err) *err = no_model;
        return NULL;
    }
    m->V = ee->rows;
    m->D = ee->cols;
    m->H = MAX_HEADS;
    m->F = 1536;
    const int D = m->D, F = m->F, V = m->V;
    if (D != 256 || V <= 0) goto bad;
    for (int l = 0; l < MAX_ENC; l++) {
        enc_t *L = &m->enc[l];
#define E(n) (snprintf(nm, sizeof nm, "e%d." n, l + 1), nm)
        if (!mat(m, p, dir, E("aq"), &L->aq, D, D) || !mat(m, p, dir, E("ak"), &L->ak, D, D) ||
            !mat(m, p, dir, E("av"), &L->av, D, D) || !mat(m, p, dir, E("ao"), &L->ao, D, D) ||
            !mat(m, p, dir, E("f1"), &L->f1, F, D) || !mat(m, p, dir, E("f2"), &L->f2, D, F) ||
            !vec(m, p, dir, E("abq"), &L->abq, D) || !vec(m, p, dir, E("abk"), &L->abk, D) ||
            !vec(m, p, dir, E("abv"), &L->abv, D) || !vec(m, p, dir, E("abo"), &L->abo, D) ||
            !vec(m, p, dir, E("als"), &L->als, D) || !vec(m, p, dir, E("alb"), &L->alb, D) ||
            !vec(m, p, dir, E("fb1"), &L->fb1, F) || !vec(m, p, dir, E("fb2"), &L->fb2, D) ||
            !vec(m, p, dir, E("fls"), &L->fls, D) || !vec(m, p, dir, E("flb"), &L->flb, D))
            goto bad;
#undef E
    }
    for (int l = 0; l < MAX_DEC; l++) {
        dec_t *L = &m->dec[l];
#define E(n) (snprintf(nm, sizeof nm, "d%d." n, l + 1), nm)
        if (!mat(m, p, dir, E("r"), &L->r, D, D) || !mat(m, p, dir, E("rf"), &L->rf, D, D) ||
            !mat(m, p, dir, E("cq"), &L->cq, D, D) || !mat(m, p, dir, E("ck"), &L->ck, D, D) ||
            !mat(m, p, dir, E("cv"), &L->cv, D, D) || !mat(m, p, dir, E("co"), &L->co, D, D) ||
            !mat(m, p, dir, E("f1"), &L->f1, F, D) || !mat(m, p, dir, E("f2"), &L->f2, D, F) ||
            !vec(m, p, dir, E("rbf"), &L->rbf, D) || !vec(m, p, dir, E("rls"), &L->rls, D) ||
            !vec(m, p, dir, E("rlb"), &L->rlb, D) ||
            !vec(m, p, dir, E("cbq"), &L->cbq, D) || !vec(m, p, dir, E("cbk"), &L->cbk, D) ||
            !vec(m, p, dir, E("cbv"), &L->cbv, D) || !vec(m, p, dir, E("cbo"), &L->cbo, D) ||
            !vec(m, p, dir, E("cls"), &L->cls, D) || !vec(m, p, dir, E("clb"), &L->clb, D) ||
            !vec(m, p, dir, E("fb1"), &L->fb1, F) || !vec(m, p, dir, E("fb2"), &L->fb2, D) ||
            !vec(m, p, dir, E("fls"), &L->fls, D) || !vec(m, p, dir, E("flb"), &L->flb, D))
            goto bad;
#undef E
    }
    if (!vec(m, p, dir, "ob", &m->ob, V)) goto bad;
    m->emb.s = ee->scale;
    m->emb.sa = ee->scale_a;
    m->emb.out = V;
    m->emb.in = D;
    m->emb_mode = emb_mode;
    m->pak = p;
    m->emb_off = ee->off;
    if (emb_mode == TD_NMT_EMB_RAM) {
        m->emb.q = (const int8_t *)grab(m, p, dir, "emb", TD_T_MAT8, NULL);
        if (!m->emb.q) goto bad;
    }
    const td_entry_t *e;
    const uint8_t *spm = (const uint8_t *)grab(m, p, dir, "spm", TD_T_SPM, &e);
    if (!spm && !m->oom) {
        /* both directions share it: the pack keeps it under the other one */
        char rev[5] = {dir[2], dir[3], dir[0], dir[1], 0};
        spm = (const uint8_t *)grab(m, p, rev, "spm", TD_T_SPM, &e);
    }
    if (!spm || td_spm_init(&m->spm, spm, e->len)) goto bad;
    if (m->spm.n != V) goto bad;
    const uint8_t *lex = (const uint8_t *)grab(m, p, dir, "lex", TD_T_LEX, &e);
    if (!lex || e->len < 8) goto bad;
    uint16_t first;
    uint32_t nsrc;
    memcpy(&first, lex, 2);
    memcpy(&nsrc, lex + 4, 4);
    if (e->len < 8 + (nsrc + 1) * 4) goto bad;
    m->lex_first = first;
    m->lex_nsrc = (int)nsrc;
    m->lex_off = (const uint32_t *)(lex + 8);
    m->lex_ids = (const uint16_t *)(lex + 8 + (nsrc + 1) * 4);
    m->lex_nids = (e->len - 8 - (nsrc + 1) * 4) / 2;
    m->mark = (uint8_t *)td_malloc((size_t)V);
    if (!m->mark) {
        m->oom = true;
        goto bad;
    }
    return m;
bad:
    if (err) *err = m->oom ? no_mem : no_model;
    td_nmt_free(m);
    return NULL;
}

void td_nmt_set_par(td_nmt_t *m, td_nmt_par_fn fn, void *ctx)
{
    m->par = fn;
    m->par_ctx = ctx;
}

void td_nmt_set_int8(td_nmt_t *m, bool on)
{
    m->int8 = on;
}

void td_nmt_set_cancel(td_nmt_t *m, volatile bool *flag)
{
    m->cancel = flag;
}

void td_nmt_set_clock(td_nmt_t *m, uint32_t (*ms)(void))
{
    m->clock = ms;
}

static uint32_t now(td_nmt_t *m)
{
    return m->clock ? m->clock() : 0;
}

/* ---------------------------------------------------------------- the maths */

typedef struct {
    const float *x;     /* n x in, or NULL with xq                          */
    const int8_t *xq;   /* n x in, quantized with W->sa                      */
    int n;
    const m8_t *W;
    const float *b;
    float *y;           /* n x out                                           */
    bool relu;
} mm_job_t;

static void mm_part(void *arg, int part, int parts)
{
    const mm_job_t *J = (const mm_job_t *)arg;
    const m8_t *W = J->W;
    const int in = W->in, n = J->n;
    int j0 = (int)((long)W->out * part / parts), j1 = (int)((long)W->out * (part + 1) / parts);
    for (int j = j0; j < j1; j++) {
        const int8_t *w = W->q + (size_t)j * in;
        const float bj = J->b ? J->b[j] : 0.0f;
        if (J->xq) {
            const float s = W->s / W->sa;
            for (int t = 0; t < n; t++) {
                const int8_t *x = J->xq + (size_t)t * in;
                int32_t acc = 0;
                for (int i = 0; i < in; i++) acc += (int32_t)x[i] * w[i];
                float v = (float)acc * s + bj;
                J->y[(size_t)t * W->out + j] = J->relu && v < 0 ? 0 : v;
            }
            continue;
        }
        int t = 0;
        for (; t + 4 <= n; t += 4) {    /* four tokens per pass over the row */
            const float *x0 = J->x + (size_t)t * in, *x1 = x0 + in, *x2 = x1 + in, *x3 = x2 + in;
            float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            for (int i = 0; i < in; i++) {
                float wi = (float)w[i];
                a0 += x0[i] * wi;
                a1 += x1[i] * wi;
                a2 += x2[i] * wi;
                a3 += x3[i] * wi;
            }
            float v[4] = {a0 * W->s + bj, a1 * W->s + bj, a2 * W->s + bj, a3 * W->s + bj};
            for (int k = 0; k < 4; k++)
                J->y[(size_t)(t + k) * W->out + j] = J->relu && v[k] < 0 ? 0 : v[k];
        }
        for (; t < n; t++) {
            const float *x = J->x + (size_t)t * in;
            float a = 0;
            for (int i = 0; i < in; i++) a += x[i] * (float)w[i];
            float v = a * W->s + bj;
            J->y[(size_t)t * W->out + j] = J->relu && v < 0 ? 0 : v;
        }
    }
}


/* y = x W^T + b (relu), n rows */
static bool mm(td_nmt_t *m, const float *x, int n, const m8_t *W, const float *b, float *y, bool relu)
{
    mm_job_t J = {x, NULL, n, W, b, y, relu};
    if (m->int8 && W->sa > 0) {
        size_t need = (size_t)n * W->in;
        if (need > s_xq_len) {
            td_free(s_xq);
            s_xq = (int8_t *)td_malloc(need);
            s_xq_len = s_xq ? need : 0;
            if (!s_xq) return false;
        }
        for (size_t i = 0; i < need; i++) {
            float v = x[i] * W->sa;
            v = v > 127.f ? 127.f : v < -127.f ? -127.f : v;
            s_xq[i] = (int8_t)(v < 0 ? v - 0.5f : v + 0.5f);
        }
        J.xq = s_xq;
    }
    m->macs += (uint64_t)n * W->in * W->out;
    if (m->par && W->out * n >= 256) m->par(mm_part, &J, m->par_ctx);
    else mm_part(&J, 0, 1);
    return true;
}

static void layer_norm(float *x, int n, int D, const float *s, const float *b)
{
    for (int t = 0; t < n; t++) {
        float *r = x + (size_t)t * D;
        float mean = 0;
        for (int i = 0; i < D; i++) mean += r[i];
        mean /= (float)D;
        float var = 0;
        for (int i = 0; i < D; i++) {
            float d = r[i] - mean;
            var += d * d;
        }
        var /= (float)D;
        float inv = 1.0f / sqrtf(var + 1e-9f);
        for (int i = 0; i < D; i++) r[i] = (r[i] - mean) * inv * s[i] + b[i];
    }
}

static void add(float *x, const float *y, size_t n)
{
    for (size_t i = 0; i < n; i++) x[i] += y[i];
}

static void sinusoid(float *x, int D, int pos)
{
    const int h = D / 2;
    const float inc = logf(10000.0f) / (float)(h - 1);
    for (int i = 0; i < h; i++) {
        float v = (float)pos * expf((float)i * -inc);
        x[i] += sinf(v);
        x[h + i] += cosf(v);
    }
}

/* ctx (nq x D) = softmax(q k^T / sqrt(dh)) v, per head; k, v are nk x D */
static void attend(const float *q, int nq, const float *k, const float *v, int nk, int D, int H,
                   float *ctx, float *sc)
{
    const int dh = D / H;
    const float scale = 1.0f / sqrtf((float)dh);
    for (int t = 0; t < nq; t++) {
        for (int h = 0; h < H; h++) {
            const float *qh = q + (size_t)t * D + h * dh;
            float mx = -1e30f;
            for (int s = 0; s < nk; s++) {
                const float *kh = k + (size_t)s * D + h * dh;
                float a = 0;
                for (int i = 0; i < dh; i++) a += qh[i] * kh[i];
                a *= scale;
                sc[s] = a;
                if (a > mx) mx = a;
            }
            float sum = 0;
            for (int s = 0; s < nk; s++) {
                sc[s] = expf(sc[s] - mx);
                sum += sc[s];
            }
            float inv = 1.0f / sum;
            float *o = ctx + (size_t)t * D + h * dh;
            for (int i = 0; i < dh; i++) o[i] = 0;
            for (int s = 0; s < nk; s++) {
                const float *vh = v + (size_t)s * D + h * dh;
                float w = sc[s] * inv;
                for (int i = 0; i < dh; i++) o[i] += w * vh[i];
            }
        }
    }
}

static bool emb_row(td_nmt_t *m, int id, int8_t *dst)
{
    if (m->emb.q) {
        memcpy(dst, m->emb.q + (size_t)id * m->D, (size_t)m->D);
        return true;
    }
    return td_pak_read(m->pak, m->emb_off + (uint32_t)id * (uint32_t)m->D, dst, (uint32_t)m->D);
}

/* one segment of at most MAX_SRC pieces */
static bool translate_ids(td_nmt_t *m, const int32_t *src, int n, int32_t *out, int *nout, int maxout,
                          td_nmt_stats_t *st)
{
    const int D = m->D, F = m->F, H = m->H;
    bool ok = false;
    float *x = (float *)td_malloc((size_t)n * D * 4);
    float *q = (float *)td_malloc((size_t)n * D * 4);
    float *k = (float *)td_malloc((size_t)n * D * 4);
    float *v = (float *)td_malloc((size_t)n * D * 4);
    float *hb = (float *)td_malloc((size_t)n * F * 4);
    float *kv = (float *)td_malloc((size_t)MAX_DEC * 2 * n * D * 4);
    float *sc = (float *)td_malloc((size_t)(n + 1) * 4);
    int8_t *row = (int8_t *)td_malloc((size_t)D);
    int32_t *cand = NULL;
    int8_t *cemb = NULL;
    float *logit = NULL;
    if (!x || !q || !k || !v || !hb || !kv || !sc || !row) goto out;

    /* --- encoder */
    uint32_t t0 = now(m);
    const float sq = sqrtf((float)D);
    for (int t = 0; t < n; t++) {
        if (!emb_row(m, src[t], row)) goto out;
        float *r = x + (size_t)t * D;
        for (int i = 0; i < D; i++) r[i] = (float)row[i] * m->emb.s * sq;
        sinusoid(r, D, t);
    }
    for (int l = 0; l < MAX_ENC; l++) {
        const enc_t *L = &m->enc[l];
        if (!mm(m, x, n, &L->aq, L->abq, q, false) || !mm(m, x, n, &L->ak, L->abk, k, false) ||
            !mm(m, x, n, &L->av, L->abv, v, false))
            goto out;
        attend(q, n, k, v, n, D, H, hb, sc);
        if (!mm(m, hb, n, &L->ao, L->abo, q, false)) goto out;
        add(x, q, (size_t)n * D);
        layer_norm(x, n, D, L->als, L->alb);
        if (!mm(m, x, n, &L->f1, L->fb1, hb, true) || !mm(m, hb, n, &L->f2, L->fb2, q, false)) goto out;
        add(x, q, (size_t)n * D);
        layer_norm(x, n, D, L->fls, L->flb);
    }
    for (int l = 0; l < MAX_DEC; l++) {
        const dec_t *L = &m->dec[l];
        float *K = kv + (size_t)(2 * l) * n * D, *Vv = kv + (size_t)(2 * l + 1) * n * D;
        if (!mm(m, x, n, &L->ck, L->cbk, K, false) || !mm(m, x, n, &L->cv, L->cbv, Vv, false)) goto out;
    }
    uint32_t t1 = now(m);

    /* --- the shortlist: the first pieces, each source piece's list, </s> */
    memset(m->mark, 0, (size_t)m->V);
    for (int i = 0; i < m->lex_first && i < m->V; i++) m->mark[i] = 1;
    m->mark[m->spm.eos] = 1;
    for (int t = 0; t < n; t++) {
        int w = src[t];
        if (w < 0 || w >= m->lex_nsrc) continue;
        for (uint32_t i = m->lex_off[w]; i < m->lex_off[w + 1] && i < m->lex_nids; i++)
            m->mark[m->lex_ids[i]] = 1;
    }
    int nc = 0;
    for (int i = 0; i < m->V; i++) nc += m->mark[i];
    cand = (int32_t *)td_malloc((size_t)nc * 4);
    cemb = (int8_t *)td_malloc((size_t)nc * D);
    logit = (float *)td_malloc((size_t)nc * 4);
    if (!cand || !cemb || !logit) goto out;
    nc = 0;
    for (int i = 0; i < m->V; i++)
        if (m->mark[i]) cand[nc++] = i;
    for (int c = 0; c < nc; c++)        /* in order: from the card, reads go forwards */
        if (!emb_row(m, cand[c], cemb + (size_t)c * D)) goto out;
    m8_t out_w = {cemb, m->emb.s, m->emb.sa, nc, D};
    if (st) st->cand = (uint32_t)nc;

    /* --- decoder, greedy */
    float cell[MAX_DEC][256];
    memset(cell, 0, sizeof cell);
    float *y = x, *t1b = q, *t2b = k;   /* the encoder's buffers are free now */
    int prev = -1, prev_c = 0, cnt = 0;
    for (int step = 0; step < maxout; step++) {
        if (m->cancel && *m->cancel) goto out;
        if (prev < 0) {
            memset(y, 0, (size_t)D * 4);
        } else {
            const int8_t *pr = cemb + (size_t)prev_c * D;   /* prev came from the shortlist */
            for (int i = 0; i < D; i++) y[i] = (float)pr[i] * m->emb.s * sq;
        }
        sinusoid(y, D, step);
        for (int l = 0; l < MAX_DEC; l++) {
            const dec_t *L = &m->dec[l];
            if (!mm(m, y, 1, &L->r, NULL, t1b, false) || !mm(m, y, 1, &L->rf, L->rbf, t2b, false)) goto out;
            for (int i = 0; i < D; i++) {
                float f = 1.0f / (1.0f + expf(-t2b[i]));
                cell[l][i] = f * cell[l][i] + (1.0f - f) * t1b[i];
                y[i] += cell[l][i] > 0 ? cell[l][i] : 0;
            }
            layer_norm(y, 1, D, L->rls, L->rlb);
            const float *K = kv + (size_t)(2 * l) * n * D, *Vv = kv + (size_t)(2 * l + 1) * n * D;
            if (!mm(m, y, 1, &L->cq, L->cbq, t1b, false)) goto out;
            attend(t1b, 1, K, Vv, n, D, H, t2b, sc);
            if (!mm(m, t2b, 1, &L->co, L->cbo, t1b, false)) goto out;
            add(y, t1b, (size_t)D);
            layer_norm(y, 1, D, L->cls, L->clb);
            if (!mm(m, y, 1, &L->f1, L->fb1, hb, true) || !mm(m, hb, 1, &L->f2, L->fb2, t1b, false)) goto out;
            add(y, t1b, (size_t)D);
            layer_norm(y, 1, D, L->fls, L->flb);
        }
        if (!mm(m, y, 1, &out_w, NULL, logit, false)) goto out;
        int best = 0, best_c = 0;
        float bv = -1e30f;
        for (int c = 0; c < nc; c++) {
            float lv = logit[c] + m->ob[cand[c]];
            if (lv > bv) {
                bv = lv;
                best = cand[c];
                best_c = c;
            }
        }
        if (best == m->spm.eos) break;
        if (cnt < maxout) out[cnt++] = best;
        prev = best;
        prev_c = best_c;
    }
    *nout = cnt;
    ok = true;
    if (st) {
        uint32_t t2 = now(m);
        st->ms_enc += t1 - t0;
        st->ms_dec += t2 - t1;
    }
out:
    td_free(x);
    td_free(q);
    td_free(k);
    td_free(v);
    td_free(hb);
    td_free(kv);
    td_free(sc);
    td_free(row);
    td_free(cand);
    td_free(cemb);
    td_free(logit);
    return ok;
}

/* ---------------------------------------------------------------- text */

static void put(char *out, int size, int *o, const char *s, int n)
{
    for (int i = 0; i < n && *o < size - 1; i++) out[(*o)++] = s[i];
    out[*o] = 0;
}

/* a segment ends at a line's end or after . ! ? followed by a space */
static int segment_end(const char *s, int *sep)
{
    int i = 0;
    for (; s[i]; i++) {
        if (s[i] == '\n') {
            *sep = 1;
            return i;
        }
        if ((s[i] == '.' || s[i] == '!' || s[i] == '?') && (s[i + 1] == ' ' || s[i + 1] == '\t')) {
            int j = i + 1;
            while (s[j] == ' ' || s[j] == '\t') j++;
            *sep = j - i - 1;
            return i + 1;
        }
    }
    *sep = 0;
    return i;
}

bool td_nmt_translate(td_nmt_t *m, const char *text, char *out, int size, td_nmt_stats_t *st)
{
    if (size <= 0) return false;
    out[0] = 0;
    int o = 0;
    uint32_t t0 = now(m);
    uint64_t macs0 = m->macs;
    int32_t *ids = (int32_t *)malloc(MAX_SRC * 2 * sizeof(int32_t));
    int32_t *res = (int32_t *)malloc(MAX_SRC * 3 * sizeof(int32_t));
    char *seg = (char *)malloc(2048);
    char *dec = (char *)malloc(4096);
    bool ok = ids && res && seg && dec;
    const char *s = text;
    while (ok && *s) {
        int sep;
        int len = segment_end(s, &sep);
        int keep = len < 2047 ? len : 2047;
        memcpy(seg, s, (size_t)keep);
        seg[keep] = 0;
        int n = td_spm_encode(&m->spm, seg, ids, MAX_SRC * 2);
        /* the source pieces in chunks the model takes (a sentence of 300 pieces is rare) */
        for (int a = 0; a < n && ok; a += MAX_SRC - 1) {
            int cnt = n - a < MAX_SRC - 1 ? n - a : MAX_SRC - 1;
            int32_t chunk[MAX_SRC];
            memcpy(chunk, ids + a, (size_t)cnt * 4);
            chunk[cnt] = m->spm.eos;
            int nout = 0, maxout = 2 * cnt + 10;
            if (maxout > MAX_SRC * 3) maxout = MAX_SRC * 3;
            ok = translate_ids(m, chunk, cnt + 1, res, &nout, maxout, st);
            if (!ok) break;
            int dl = td_spm_decode(&m->spm, res, nout, dec, 4096);
            if (a > 0) put(out, size, &o, " ", 1);
            put(out, size, &o, dec, dl);
            if (st) {
                st->src_tokens += (uint32_t)cnt + 1;
                st->out_tokens += (uint32_t)nout;
            }
        }
        s += len;
        if (sep) {
            if (*s == '\n') put(out, size, &o, "\n", 1);
            else put(out, size, &o, " ", 1);
            s += sep;
        }
    }
    free(ids);
    free(res);
    free(seg);
    free(dec);
    if (st) {
        st->macs_m += (uint32_t)((m->macs - macs0) / 1000000u);
        st->ms_total += now(m) - t0;
    }
    return ok;
}
