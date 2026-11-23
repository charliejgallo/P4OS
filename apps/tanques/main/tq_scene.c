/*
 * TANQUES - the two views (see tq_scene.h)
 */
#include "tq_scene.h"
#include "tq_art.h"
#include "tq_mem.h"

#include "aos_hal.h"

#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#define MAXPX     (720 * 1280)
#define FP_NEAR   0.3f
#define WMARGIN   48

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static struct {
    /* the world: the battle camera's picture of the whole map */
    uint16_t *wpx, *wz;             /* at half the battle view's scale */
    int ww, wh;                     /* the battle view's size of it (even) */
    int hw, hh;                     /* what is kept: half that */
    float ox, oy;                   /* image position of the world's origin */
    float dfar;                     /* depth at nearness 0 */
    tq_v3_t F, R, U;
    bool all_dirty;
    tq_vbox_t dirty;
    bool have_dirty;
    float *wv;                      /* per vertex: image x, y, nearness */
    /* first person */
    uint16_t *fpx, *fz;             /* half size */
    float *cv;                      /* per vertex: x, y, q (projected), zc */
    uint8_t *code;                  /* per vertex: off-frustum bits */
    uint32_t *fcol;                 /* per vertex: fogged colour */
} S;

static bool (*s_split)(void (*fn)(void *arg, int part), void *arg);

void tq_scene_set_split(bool (*split)(void (*fn)(void *arg, int part), void *arg))
{
    s_split = split;
}

static void run(void (*fn)(void *, int), void *arg)
{
    if (s_split && s_split(fn, arg)) return;
    fn(arg, 0);
    fn(arg, 1);
}

static inline uint32_t now_us(void)
{
    return (uint32_t)aos_hal_uptime_us();
}

/* --------------------------------------------------------------------------
 * The world's projection
 * -------------------------------------------------------------------------- */

static inline void wproj(tq_v3_t p, float *x, float *y, float *q)
{
    *x = TQ_TOP_S * tq_dot(p, S.R) + S.ox;
    *y = -TQ_TOP_S * tq_dot(p, S.U) + S.oy;
    *q = (S.dfar - tq_dot(p, S.F)) * TQ_QS;
}

bool tq_scene_init(void)
{
    tq_top_axes(&S.F, &S.R, &S.U);
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, d1 = -1e9f;
    for (int k = 0; k < 8; k++) {
        tq_v3_t p = tq_v3(k & 1 ? TQ_MAP_MAX : 0, k & 2 ? TQ_MAP_MAX : 0, k & 4 ? 8.0f : TQ_BASE_Z);
        float x = TQ_TOP_S * tq_dot(p, S.R), y = -TQ_TOP_S * tq_dot(p, S.U), d = tq_dot(p, S.F);
        x0 = fminf(x0, x); x1 = fmaxf(x1, x);
        y0 = fminf(y0, y); y1 = fmaxf(y1, y);
        d1 = fmaxf(d1, d);
    }
    S.ww = (int)(x1 - x0) + 2 * WMARGIN;
    S.wh = (int)(y1 - y0) + 2 * WMARGIN;
    if (S.ww < 1280) S.ww = 1280;
    if (S.wh < 1280) S.wh = 1280;
    S.ww = (S.ww + 1) & ~1;
    S.wh = (S.wh + 1) & ~1;
    S.hw = S.ww / 2;
    S.hh = S.wh / 2;
    S.ox = (S.ww - (x1 - x0)) * 0.5f - x0;
    S.oy = (S.wh - (y1 - y0)) * 0.5f - y0;
    S.dfar = d1 + 2.0f;
    /* the world at half the view's scale: the ground is smooth, the
     * sprites over it stay sharp, and the 36 m map takes 2.3 MB, not 9.3 */
    size_t n = (size_t)S.hw * S.hh;
    S.wpx = (uint16_t *)tq_malloc(n * 2);
    S.wz = (uint16_t *)tq_malloc(n * 2);
    S.wv = (float *)tq_malloc(sizeof(float) * 3 * TQ_NVMAX * TQ_NVMAX);
    S.fpx = (uint16_t *)tq_malloc(MAXPX / 4 * 2);
    S.fz = (uint16_t *)tq_malloc(MAXPX / 4 * 2);
    S.cv = (float *)tq_malloc(sizeof(float) * 4 * TQ_NVMAX * TQ_NVMAX);
    S.code = (uint8_t *)tq_malloc(TQ_NVMAX * TQ_NVMAX);
    S.fcol = (uint32_t *)tq_malloc(4 * TQ_NVMAX * TQ_NVMAX);
    if (!S.wpx || !S.wz || !S.wv || !S.fpx || !S.fz || !S.cv || !S.code || !S.fcol) {
        tq_scene_free();
        return false;
    }
    S.all_dirty = true;
    tq_gfx_init();
    return true;
}

void tq_scene_free(void)
{
    tq_free(S.wpx);
    tq_free(S.wz);
    tq_free(S.wv);
    tq_free(S.fpx);
    tq_free(S.fz);
    tq_free(S.cv);
    tq_free(S.code);
    tq_free(S.fcol);
    memset(&S, 0, sizeof S);
}

size_t tq_scene_bytes(void)
{
    return (size_t)S.hw * S.hh * 4 + MAXPX + sizeof(float) * 7 * TQ_NVMAX * TQ_NVMAX + 5 * TQ_NVMAX * TQ_NVMAX;
}

void tq_scene_world_all(void)
{
    S.all_dirty = true;
}

void tq_scene_world_dirty(tq_vbox_t b)
{
    if (b.x1 < b.x0) return;
    if (!S.have_dirty) {
        S.dirty = b;
        S.have_dirty = true;
        return;
    }
    if (b.x0 < S.dirty.x0) S.dirty.x0 = b.x0;
    if (b.y0 < S.dirty.y0) S.dirty.y0 = b.y0;
    if (b.x1 > S.dirty.x1) S.dirty.x1 = b.x1;
    if (b.y1 > S.dirty.y1) S.dirty.y1 = b.y1;
}

/* --------------------------------------------------------------------------
 * Drawing the world
 * -------------------------------------------------------------------------- */

static inline void vcol(uint32_t c, tq_vtx_t *v)
{
    v->r = tq_clampf((float)((c >> 16) & 255), 1, 254);
    v->g = tq_clampf((float)((c >> 8) & 255), 1, 254);
    v->b = tq_clampf((float)(c & 255), 1, 254);
}

static void world_vertex(const tq_terrain_t *t, int i, int j, tq_vtx_t *v)
{
    const float *w = &S.wv[(j * t->nv + i) * 3];
    v->x = w[0] * 0.5f;
    v->y = w[1] * 0.5f;
    v->q = w[2];
    vcol(t->col[j * t->nv + i], v);
    v->u = i * TQ_CELL * 16.0f;
    v->v = j * TQ_CELL * 16.0f;
}

typedef struct {
    const tq_terrain_t *t;
    tq_tgt_t tgt;
} wjob_t;

/* The map's sides: earth strata from the edge down to the floor */
static void world_sides(const tq_terrain_t *t, const tq_tgt_t *g)
{
    for (int side = 0; side < 4; side++) {
        float shade = side == 0 ? 0.95f : (side == 1 ? 0.72f : 0.55f);
        int N = t->n;
        for (int k = 0; k < N; k++) {
            int ia, ja, ib, jb;
            switch (side) {
            case 0: ia = k; ja = 0; ib = k + 1; jb = 0; break;                       /* south */
            case 1: ia = N; ja = k; ib = N; jb = k + 1; break;                       /* east */
            case 2: ia = k; ja = N; ib = k + 1; jb = N; break;                       /* north */
            default: ia = 0; ja = k; ib = 0; jb = k + 1; break;                      /* west */
            }
            /* in bands, so the strata show */
            float za = tq_ter_vh(t, ia, ja), zb = tq_ter_vh(t, ib, jb);
            for (int b = 0; b < 4; b++) {
                float fa0 = b / 4.0f, fa1 = (b + 1) / 4.0f;
                tq_vtx_t v[4];
                float zs[4] = { za + (TQ_BASE_Z - za) * fa0, zb + (TQ_BASE_Z - zb) * fa0,
                                zb + (TQ_BASE_Z - zb) * fa1, za + (TQ_BASE_Z - za) * fa1 };
                int ii[4] = { ia, ib, ib, ia }, jj[4] = { ja, jb, jb, ja };
                for (int q = 0; q < 4; q++) {
                    tq_v3_t p = tq_v3(ii[q] * TQ_CELL, jj[q] * TQ_CELL, zs[q]);
                    wproj(p, &v[q].x, &v[q].y, &v[q].q);
                    v[q].x *= 0.5f;
                    v[q].y *= 0.5f;
                    vcol(tq_ter_side_colour(t, zs[q], shade * (q < 2 && b == 0 ? 1.08f : 1.0f)), &v[q]);
                    v[q].u = (ii[q] + jj[q]) * TQ_CELL * 16.0f;
                    v[q].v = zs[q] * 16.0f;
                }
                tq_tri(g, &v[0], &v[1], &v[2], TQ_TRI_DETAIL);
                tq_tri(g, &v[0], &v[2], &v[3], TQ_TRI_DETAIL);
            }
        }
    }
}

static void world_part(void *arg, int part)
{
    wjob_t *j = (wjob_t *)arg;
    tq_tgt_t g = j->tgt;
    int mid = (g.cy0 + g.cy1) / 2;
    if (part == 0) g.cy1 = mid;
    else g.cy0 = mid;
    if (g.cy0 >= g.cy1) return;
    /* the background: a soft sky behind the diorama */
    for (int y = g.cy0; y < g.cy1; y++) {
        float f = (float)y / S.hh;
        uint32_t r = (uint32_t)(150 - 70 * f), gg = (uint32_t)(196 - 72 * f), b = (uint32_t)(226 - 60 * f);
        uint16_t c = tq_565((r << 16) | (gg << 8) | b);
        uint16_t *p = S.wpx + (size_t)y * S.hw, *z = S.wz + (size_t)y * S.hw;
        for (int x = g.cx0; x < g.cx1; x++) {
            p[x] = c;
            z[x] = 0;
        }
    }
    const tq_terrain_t *t = j->t;
    int N = t->n, NV = t->nv;
    for (int cj = 0; cj < N; cj++) {
        for (int ci = 0; ci < N; ci++) {
            const float *a = &S.wv[(cj * NV + ci) * 3], *c = &S.wv[((cj + 1) * NV + ci + 1) * 3];
            const float *b = &S.wv[(cj * NV + ci + 1) * 3], *d = &S.wv[((cj + 1) * NV + ci) * 3];
            float minx = fminf(fminf(a[0], b[0]), fminf(c[0], d[0])), maxx = fmaxf(fmaxf(a[0], b[0]), fmaxf(c[0], d[0]));
            float miny = fminf(fminf(a[1], b[1]), fminf(c[1], d[1])), maxy = fmaxf(fmaxf(a[1], b[1]), fmaxf(c[1], d[1]));
            if (maxx * 0.5f < g.cx0 || minx * 0.5f >= g.cx1 || maxy * 0.5f < g.cy0 || miny * 0.5f >= g.cy1) continue;
            tq_vtx_t v00, v10, v11, v01;
            world_vertex(t, ci, cj, &v00);
            world_vertex(t, ci + 1, cj, &v10);
            world_vertex(t, ci + 1, cj + 1, &v11);
            world_vertex(t, ci, cj + 1, &v01);
            tq_tri(&g, &v00, &v10, &v11, TQ_TRI_DETAIL);
            tq_tri(&g, &v00, &v11, &v01, TQ_TRI_DETAIL);
        }
    }
    world_sides(t, &g);
}

static void world_draw(const tq_terrain_t *t, int x0, int y0, int x1, int y1)
{
    for (int j = 0; j < t->nv; j++)
        for (int i = 0; i < t->nv; i++) {
            float *w = &S.wv[(j * t->nv + i) * 3];
            wproj(tq_v3(i * TQ_CELL, j * TQ_CELL, tq_ter_vh(t, i, j)), &w[0], &w[1], &w[2]);
        }
    wjob_t job;
    job.t = t;
    job.tgt.px = S.wpx;
    job.tgt.z = S.wz;
    /* the box comes in the view's pixels; the world is kept at half */
    x0 = x0 < 0 ? 0 : x0 / 2;
    y0 = y0 < 0 ? 0 : y0 / 2;
    x1 = (x1 + 1) / 2;
    y1 = (y1 + 1) / 2;
    job.tgt.w = S.hw;
    job.tgt.h = S.hh;
    job.tgt.stride = S.hw;
    job.tgt.cx0 = x0;
    job.tgt.cy0 = y0;
    job.tgt.cx1 = x1 > S.hw ? S.hw : x1;
    job.tgt.cy1 = y1 > S.hh ? S.hh : y1;
    if (job.tgt.cx0 >= job.tgt.cx1 || job.tgt.cy0 >= job.tgt.cy1) return;
    run(world_part, &job);
}

static void world_update(const tq_terrain_t *t, tq_prof_t *prof)
{
    uint32_t t0 = now_us();
    if (S.all_dirty) {
        world_draw(t, 0, 0, S.ww, S.wh);
        S.all_dirty = false;
        S.have_dirty = false;
    } else if (S.have_dirty) {
        /* the box's columns, from the floor to above the highest hill */
        tq_vbox_t b = S.dirty;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int k = 0; k < 8; k++) {
            int i = k & 1 ? b.x1 + 1 : b.x0 - 1, j = k & 2 ? b.y1 + 1 : b.y0 - 1;
            tq_v3_t p = tq_v3(i * TQ_CELL, j * TQ_CELL, k & 4 ? t->hmax + 1.5f : TQ_BASE_Z);
            float x, y, q;
            wproj(p, &x, &y, &q);
            x0 = fminf(x0, x); x1 = fmaxf(x1, x);
            y0 = fminf(y0, y); y1 = fmaxf(y1, y);
        }
        world_draw(t, (int)x0 - 2, (int)y0 - 2, (int)x1 + 3, (int)y1 + 3);
        S.have_dirty = false;
    } else {
        return;
    }
    if (prof) prof->world_us += now_us() - t0;
}

/* --------------------------------------------------------------------------
 * The battle view
 * -------------------------------------------------------------------------- */

static void top_window(const tq_terrain_t *t, float cx, float cy, int w, int h, int *wx0, int *wy0)
{
    float x, y, q;
    wproj(tq_v3(cx, cy, tq_ter_height(t, cx, cy)), &x, &y, &q);
    int a = (int)(x - w / 2), b = (int)(y - h / 2);
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (a > S.ww - w) a = S.ww - w;
    if (b > S.wh - h) b = S.wh - h;
    *wx0 = a & ~1;                  /* even: the half-size world lines up */
    *wy0 = b & ~1;
}

void tq_scene_top_clamp(const tq_terrain_t *t, int w, int h, float *cx, float *cy)
{
    /* walk the centre back along the screen's axes until the window fits */
    for (int it = 0; it < 4; it++) {
        float x, y, q;
        float gz = tq_ter_height(t, *cx, *cy);
        wproj(tq_v3(*cx, *cy, gz), &x, &y, &q);
        float ex = 0, ey = 0;
        if (x - w / 2 < 0) ex = x - w / 2;
        if (x + w / 2 > S.ww) ex = x + w / 2 - S.ww;
        if (y - h / 2 < 0) ey = y - h / 2;
        if (y + h / 2 > S.wh) ey = y + h / 2 - S.wh;
        if (!ex && !ey) return;
        /* screen right is R; screen down is -U, which on the ground is
         * (-U projected); move the centre in the ground plane */
        float sx = ex / TQ_TOP_S, sy = -ey / TQ_TOP_S;
        float uh = sqrtf(S.U.x * S.U.x + S.U.y * S.U.y);
        *cx -= S.R.x * sx + S.U.x / (uh * uh) * sy;
        *cy -= S.R.y * sx + S.U.y / (uh * uh) * sy;
    }
}

void tq_scene_top_project(const tq_terrain_t *t, float cx, float cy, int w, int h, tq_v3_t p, float *sx, float *sy)
{
    int wx0, wy0;
    top_window(t, cx, cy, w, h, &wx0, &wy0);
    float q;
    wproj(p, sx, sy, &q);
    *sx -= wx0;
    *sy -= wy0;
}

bool tq_scene_top_pick(const tq_terrain_t *t, float cx, float cy, int w, int h, float sx, float sy,
                       float *wx, float *wy)
{
    int wx0, wy0;
    top_window(t, cx, cy, w, h, &wx0, &wy0);
    float ix = sx + wx0 - S.ox, iy = sy + wy0 - S.oy;
    /* the line of sight through that pixel: P0 + s F */
    tq_v3_t P0 = tq_v3(S.R.x * ix / TQ_TOP_S - S.U.x * iy / TQ_TOP_S, S.R.y * ix / TQ_TOP_S - S.U.y * iy / TQ_TOP_S,
                       S.R.z * ix / TQ_TOP_S - S.U.z * iy / TQ_TOP_S);
    float prev = -60;
    for (float s = -60; s < 60; s += 0.1f) {
        tq_v3_t p = tq_v3(P0.x + S.F.x * s, P0.y + S.F.y * s, P0.z + S.F.z * s);
        float M = t->n * TQ_CELL;
        if (p.x < 0 || p.y < 0 || p.x > M || p.y > M) {
            prev = s;
            continue;
        }
        if (p.z <= tq_ter_height(t, p.x, p.y)) {
            /* refine between prev and s */
            float a = prev, b = s;
            for (int k = 0; k < 8; k++) {
                float m = (a + b) * 0.5f;
                tq_v3_t q = tq_v3(P0.x + S.F.x * m, P0.y + S.F.y * m, P0.z + S.F.z * m);
                if (q.z <= tq_ter_height(t, q.x, q.y)) b = m;
                else a = m;
            }
            *wx = P0.x + S.F.x * b;
            *wy = P0.y + S.F.y * b;
            return true;
        }
        prev = s;
    }
    return false;
}

typedef struct {
    int kind;                       /* 0 sprite, 1 ball */
    float depth;
    const tq_spr_t *spr;
    const tq_lut_t *lut;
    tq_v3_t p;                      /* the anchor */
    float r;                        /* ball: metres */
    uint32_t rgb;
    int alpha;
    float shadow;                   /* top: the blob's radius under it, m (0: none) */
} item_t;

#define MAX_ITEMS 400

typedef struct {
    const tq_req_t *r;
    const tq_terrain_t *t;
    uint16_t *fb;
    int wx0, wy0;
    item_t *items;
    int n;
} topjob_t;

static int cmp_far_first(const void *a, const void *b)
{
    float da = ((const item_t *)a)->depth, db = ((const item_t *)b)->depth;
    return da < db ? 1 : (da > db ? -1 : 0);
}

static uint32_t part_rgb(const tq_part_t *q, int *alpha)
{
    float f = q->age / q->life;
    *alpha = 256;
    switch (q->kind) {
    case TQ_PART_FIRE: {
        /* white-yellow, yellow, orange, red, then soot */
        uint32_t c = f < 0.15f ? 0xFFF8D0 : (f < 0.35f ? 0xFFD24A : (f < 0.55f ? 0xFF9A30 : (f < 0.78f ? 0xE5582A : 0x5A4A44)));
        *alpha = (int)(256 * (1.0f - f * f * f));
        return c;
    }
    case TQ_PART_FLASH:
        *alpha = (int)(240 * (1.0f - f));
        return q->rgb;
    case TQ_PART_RING:
        *alpha = (int)(220 * (1.0f - f) * (1.0f - f));
        return q->rgb;
    case TQ_PART_SMOKE:
    case TQ_PART_PUFF:
        *alpha = (int)(230 * (1.0f - f));
        return q->rgb;
    default:
        if (f > 0.8f) *alpha = (int)(256 * (1.0f - f) * 5);
        return q->rgb;
    }
}

static float part_radius(const tq_part_t *q)
{
    float f = q->age / q->life;
    float e = 1.0f - (1.0f - f) * (1.0f - f);
    return q->r0 + (q->r1 - q->r0) * e;
}

/* What the battle view draws over the world, far first */
static int top_items(const tq_req_t *r, const tq_terrain_t *t, item_t *it)
{
    const tq_state_t *s = &r->st;
    int n = 0;
    const tq_lut_t *scen = tq_art_scenery_lut();
    for (int k = 0; k < s->ntree && n < MAX_ITEMS; k++) {
        const tq_tree_t *tr = &s->tree[k];
        item_t *i = &it[n];
        i->kind = 0;
        i->p = tq_v3(tr->x, tr->y, tq_ter_height(t, tr->x, tr->y));
        i->spr = tq_art_prop(TQ_SET_TOP, tr->alive ? tr->kind : TQ_PROP_STUMP, 0, 0);
        i->kind = 0;
        i->lut = scen;
        i->alpha = 256;
        i->shadow = tr->alive ? 1.15f : 0.4f;
        if (i->spr) n++;
    }
    for (int k = 0; k < s->nblock && n < MAX_ITEMS; k++) {
        const tq_block_t *b = &s->block[k];
        if (!b->alive) continue;
        item_t *i = &it[n];
        i->kind = 0;
        i->p = tq_v3(b->i + 0.5f, b->j + 0.5f, b->z);
        i->spr = tq_art_prop(TQ_SET_TOP, TQ_PROP_BLOCK1 + b->variant, 0, 0);
        i->kind = 0;
        i->lut = scen;
        i->alpha = 256;
        i->shadow = 0;
        if (i->spr) n++;
    }
    for (int k = 0; k < s->ntank && n < MAX_ITEMS - 2; k++) {
        const tq_tank_t *tk = &s->tank[k];
        tq_v3_t p = tq_v3(tk->x, tk->y, tq_tank_z(t, tk));
        const tq_lut_t *lut = r->lut[k];
        const tq_spr_t *hull = tq_art_tank(TQ_SET_TOP, tk->type, TQ_PART_HULL, tq_yaw_index(tk->yaw), 0);
        const tq_spr_t *tur = tq_art_tank(TQ_SET_TOP, tk->type, TQ_PART_TURRET, tq_yaw_index(tk->tyaw), 0);
        if (!hull || !lut) continue;
        memset(&it[n], 0, sizeof it[n]);
        it[n].p = p;
        it[n].spr = hull;
        it[n].lut = tk->alive ? lut : r->wreck;
        it[n].alpha = 256;
        it[n].shadow = 1.6f;
        n++;
        if (tur && tk->alive) {
            it[n] = it[n - 1];
            it[n].p.z += 0.001f;        /* the turret after its hull */
            it[n].spr = tur;
            it[n].shadow = 0;
            n++;
        }
    }
    for (int k = 0; k < TQ_MAX_SHELLS && n < MAX_ITEMS; k++) {
        const tq_shell_t *sh = &s->shell[k];
        if (!sh->live || sh->under) continue;
        item_t *i = &it[n++];
        memset(i, 0, sizeof *i);
        i->kind = 1;
        i->p = sh->p;
        i->r = 0.2f;
        i->rgb = TQ_WEAPON[sh->weapon].rgb;
        i->alpha = 256;
        i->shadow = 0.25f;
    }
    for (int k = 0; k < TQ_MAX_MINES && n < MAX_ITEMS; k++) {
        const tq_mine_t *mn = &s->mine[k];
        if (!mn->live) continue;
        item_t *i = &it[n++];
        memset(i, 0, sizeof *i);
        i->kind = 1;
        i->p = tq_v3(mn->x, mn->y, tq_ter_height(t, mn->x, mn->y) + 0.12f);
        i->r = 0.24f;
        i->rgb = ((int)(s->pulse * 4) & 1) ? 0xFF4A3A : 0x4A4E56;
        i->alpha = 256;
    }
    if (s->beam_t > 0) {
        for (int k = 0; k <= 24 && n < MAX_ITEMS; k++) {
            float f = k / 24.0f;
            item_t *i = &it[n++];
            memset(i, 0, sizeof *i);
            i->kind = 1;
            i->p = tq_v3(s->beam_a.x + (s->beam_b.x - s->beam_a.x) * f, s->beam_a.y + (s->beam_b.y - s->beam_a.y) * f,
                         s->beam_a.z + (s->beam_b.z - s->beam_a.z) * f);
            i->r = 0.16f;
            i->rgb = s->beam_rgb;
            i->alpha = (int)(512 * s->beam_t);
        }
    }
    for (int k = 0; k < s->npart && n < MAX_ITEMS; k++) {
        const tq_part_t *q = &s->part[k];
        if (q->kind == TQ_PART_RING) continue;
        item_t *i = &it[n++];
        i->kind = 1;
        i->p = q->p;
        i->r = part_radius(q);
        i->rgb = part_rgb(q, &i->alpha);
        i->shadow = q->kind == TQ_PART_CHUNK ? i->r : 0;
    }
    for (int k = 0; k < n; k++) it[k].depth = tq_dot(it[k].p, S.F);
    qsort(it, (size_t)n, sizeof(item_t), cmp_far_first);
    return n;
}

static const uint32_t REACH_RGB[4] = { 0, 0x46C8FF, 0xFFD23C, 0xFF7A3C };

static void top_part(void *arg, int part)
{
    topjob_t *j = (topjob_t *)arg;
    const tq_req_t *r = j->r;
    const tq_state_t *s = &r->st;
    const tq_terrain_t *t = j->t;
    int W = r->w, H = r->h;
    tq_tgt_t g = { j->fb, NULL, W, H, W, 0, 0, W, H };
    if (part == 0) g.cy1 = H / 2;
    else g.cy0 = H / 2;
    /* the window of the world */
    /* the window of the world, doubled */
    tq_up2_window(j->fb, W, W, S.wpx, S.hw, S.hh, S.hw, j->wx0 / 2, j->wy0 / 2, g.cy0, g.cy1);
    const uint16_t *zt = S.wz + (size_t)(j->wy0 / 2) * S.hw + j->wx0 / 2;
    int zs = S.hw;
    float ox = -(float)j->wx0, oy = -(float)j->wy0;

    /* the reach: tiles on the ground */
    if (s->show_reach) {
        for (int c = 0; c < s->grid * s->grid; c++) {
            int cls = s->reach[c];
            if (!cls) continue;
            float x0 = c % s->grid + 0.07f, y0 = c / s->grid + 0.07f, x1 = x0 + 0.86f, y1 = y0 + 0.86f;
            float xy[8], q[4];
            float px[4] = { x0, x1, x1, x0 }, py[4] = { y0, y0, y1, y1 };
            bool any = false;
            for (int k = 0; k < 4; k++) {
                tq_v3_t p = tq_v3(px[k], py[k], tq_ter_height(t, px[k], py[k]) + 0.03f);
                wproj(p, &xy[2 * k], &xy[2 * k + 1], &q[k]);
                xy[2 * k] += ox;
                xy[2 * k + 1] += oy;
                if (xy[2 * k + 1] >= g.cy0 - 40 && xy[2 * k + 1] < g.cy1 + 40) any = true;
            }
            if (any) tq_tint_quad(&g, xy, q, REACH_RGB[cls], 96, zt, zs, 60);
        }
    }
    /* the active tank's ring */
    {
        const tq_tank_t *tk = &s->tank[s->cur];
        float x, y, q;
        wproj(tq_v3(tk->x, tk->y, tq_tank_z(t, tk)), &x, &y, &q);
        float rr = (1.9f + 0.15f * sinf(s->pulse * 2 * TQ_PI)) * TQ_TOP_S;
        tq_ring(&g, x + ox, y + oy, rr, rr * sinf(TQ_TOP_ELEV * TQ_DEG), 4, 0xFFFFFF, 170);
    }
    /* the path */
    if (s->npath >= 2) {
        for (int k = 0; k + 1 < s->npath; k++) {
            float ax = s->path[k][0], ay = s->path[k][1], bx = s->path[k + 1][0], by = s->path[k + 1][1];
            float len = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
            int n = (int)(len / 0.55f) + 1;
            for (int i = (k ? 0 : 1); i <= n; i++) {
                float f = (float)i / n, x = ax + (bx - ax) * f, y = ay + (by - ay) * f;
                float sx, sy, q;
                wproj(tq_v3(x, y, tq_ter_height(t, x, y) + 0.05f), &sx, &sy, &q);
                tq_disc(&g, sx + ox, sy + oy, 5.5f, 0x203040, 140);
                tq_disc(&g, sx + ox, sy + oy, 3.8f, 0xFFFFFF, 256);
            }
        }
        float gx = s->path[s->npath - 1][0], gy = s->path[s->npath - 1][1], sx, sy, q;
        wproj(tq_v3(gx, gy, tq_ter_height(t, gx, gy) + 0.05f), &sx, &sy, &q);
        tq_ring(&g, sx + ox, sy + oy, 22, 22 * 0.82f, 5, 0xFFFFFF, 230);
    }
    /* the blasts' rings along the ground */
    for (int k = 0; k < s->npart; k++) {
        const tq_part_t *q = &s->part[k];
        if (q->kind != TQ_PART_RING) continue;
        int al;
        uint32_t c = part_rgb(q, &al);
        float sx, sy, qq, rr = part_radius(q) * TQ_TOP_S;
        wproj(q->p, &sx, &sy, &qq);
        tq_ring(&g, sx + ox, sy + oy, rr, rr * sinf(TQ_TOP_ELEV * TQ_DEG), 7, c, al);
    }
    /* shadows on the ground: away from the sun */
    tq_v3_t L = tq_sun();
    float lh = sqrtf(L.x * L.x + L.y * L.y);
    for (int k = 0; k < j->n; k++) {
        const item_t *i = &j->items[k];
        float rad = i->shadow, hgt;
        if (rad <= 0) continue;
        if (i->kind == 0) {
            hgt = 0.6f;
        } else {
            hgt = i->p.z - tq_ter_height(t, i->p.x, i->p.y);
            if (hgt > 6) continue;
        }
        float gz = i->kind == 0 ? i->p.z : tq_ter_height(t, i->p.x, i->p.y);
        tq_v3_t sp = tq_v3(i->p.x - L.x / lh * hgt * 0.9f, i->p.y - L.y / lh * hgt * 0.9f, gz);
        float sx, sy, q;
        wproj(sp, &sx, &sy, &q);
        tq_shadow(&g, sx + ox, sy + oy, rad * TQ_TOP_S, rad * TQ_TOP_S * 0.8f, 150);
    }
    /* the things */
    for (int k = 0; k < j->n; k++) {
        const item_t *i = &j->items[k];
        float sx, sy, q;
        wproj(i->p, &sx, &sy, &q);
        if (i->kind == 0) {
            /* a sprite without depth (the tanks from above) is tested as if
             * its front were nearer: the ground just in front of the anchor
             * would hide its tracks otherwise */
            int qa = (int)q + (i->spr->Z ? 0 : (int)(1.6f * TQ_QS));
            tq_spr_draw(&g, i->spr, i->lut, (int)floorf(sx + ox + 0.5f), (int)floorf(sy + oy + 0.5f), qa, TQ_QZS,
                        i->alpha, zt, zs, false);
        } else {
            tq_ball(&g, sx + ox, sy + oy, i->r * TQ_TOP_S, i->rgb, i->alpha, (int)q, zt, zs);
        }
    }
    /* the hit points over the tanks */
    if (r->hp_bars) {
        for (int k = 0; k < s->ntank; k++) {
            const tq_tank_t *tk = &s->tank[k];
            if (!tk->alive) continue;
            float sx, sy, q;
            wproj(tq_v3(tk->x, tk->y, tq_tank_z(t, tk) + 2.6f), &sx, &sy, &q);
            int x = (int)(sx + ox) - 34, y = (int)(sy + oy) - 20;
            tq_rect(&g, x - 2, y - 2, 72, 12, 0x1A1A22, 200);
            int fill = 68 * tk->hp / tk->hpmax;
            uint32_t c = tk->hp > 50 ? 0x5ED16A : (tk->hp > 25 ? 0xFFC83C : 0xFF5A46);
            if (tk->hit_flash > 0 && ((int)(tk->hit_flash * 12) & 1)) c = 0xFFFFFF;
            tq_rect(&g, x, y, fill, 8, c, 256);
            tq_rect(&g, x, y, 68, 2, r->rgb[k], 256);
            if (tk->shield > 0) tq_rect(&g, x, y + 8, 68 * (tk->shield > 60 ? 60 : tk->shield) / 60, 3, 0x46C8FF, 256);
        }
    }
}

static void render_top(const tq_req_t *r, const tq_terrain_t *t, uint16_t *fb, tq_prof_t *prof)
{
    world_update(t, prof);
    static item_t items[MAX_ITEMS];
    topjob_t j;
    j.r = r;
    j.t = t;
    j.fb = fb;
    top_window(t, r->cx, r->cy, r->w, r->h, &j.wx0, &j.wy0);
    uint32_t t0 = now_us();
    j.items = items;
    j.n = top_items(r, t, items);
    tq_gfx_zshift(1);
    run(top_part, &j);
    tq_gfx_zshift(0);
    if (prof) {
        prof->things_us += now_us() - t0;
        prof->sprites = j.n;
    }
}

/* --------------------------------------------------------------------------
 * The gunner's view
 * -------------------------------------------------------------------------- */

void tq_scene_fp_frame(int w, int h, float *cx, float *cy, float *f)
{
    *cx = w * 0.5f;
    *cy = h * (h > w ? 0.46f : 0.42f);
    *f = TQ_FP_F;
}

typedef struct {
    const tq_req_t *r;
    const tq_terrain_t *t;
    uint16_t *fb;
    int fw, fh;
    float cx, cy, f;                /* half size */
    tq_v3_t Fw, Rw, Uw;
    item_t *items;
    int n;
    int tris[2];
    float yhz;                      /* the horizon's row */
    uint32_t fog, sky_top, sky_low, far, near;
    float fog_near, fog_max;
} fpjob_t;


static inline uint32_t mix_rgb(uint32_t a, uint32_t b, float f)
{
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return ((uint32_t)(ar + (br - ar) * f) << 16) | ((uint32_t)(ag + (bg - ag) * f) << 8) | (uint32_t)(ab + (bb - ab) * f);
}

static void fp_vertices(fpjob_t *j)
{
    const tq_terrain_t *t = j->t;
    tq_v3_t e = j->r->eye;
    float xmax = j->cx / j->f * 1.1f, ymax = j->cy / j->f * 1.6f;
    for (int jj = 0; jj < t->nv; jj++) {
        for (int ii = 0; ii < t->nv; ii++) {
            int k = jj * t->nv + ii;
            tq_v3_t d = tq_v3(ii * TQ_CELL - e.x, jj * TQ_CELL - e.y, t->h[k] - e.z);
            float xc = tq_dot(d, j->Rw), yc = tq_dot(d, j->Uw), zc = tq_dot(d, j->Fw);
            float *o = &S.cv[k * 4];
            uint8_t code = 0;
            if (zc < FP_NEAR) code |= 1;
            if (xc < -xmax * zc - 1) code |= 2;
            if (xc > xmax * zc + 1) code |= 4;
            if (yc > ymax * zc + 1) code |= 8;
            if (yc < -ymax * zc * 1.4f - 1) code |= 16;
            S.code[k] = code;
            o[0] = xc;
            o[1] = yc;
            o[2] = zc;
            float dist = sqrtf(d.x * d.x + d.y * d.y);
            S.fcol[k] = mix_rgb(t->col[k], j->fog, tq_clampf((dist - j->fog_near) / 40.0f, 0, j->fog_max));
        }
    }
}

static inline void fp_proj(const fpjob_t *j, float xc, float yc, float zc, tq_vtx_t *v)
{
    float iz = 1.0f / zc;
    v->x = j->cx + j->f * xc * iz;
    v->y = j->cy - j->f * yc * iz;
    v->q = TQ_QK * iz;
}

/* A triangle of the ground, cut at the near plane if it crosses it */
static int fp_tri(const fpjob_t *j, const tq_tgt_t *g, int a, int b, int c)
{
    uint8_t ca = S.code[a], cb = S.code[b], cc = S.code[c];
    if (ca & cb & cc) return 0;
    int idx[3] = { a, b, c };
    tq_vtx_t v[3];
    float cam[3][3];
    for (int k = 0; k < 3; k++) {
        const float *o = &S.cv[idx[k] * 4];
        cam[k][0] = o[0];
        cam[k][1] = o[1];
        cam[k][2] = o[2];
        vcol(S.fcol[idx[k]], &v[k]);
        int ii = idx[k] % j->t->nv, jj = idx[k] / j->t->nv;
        v[k].u = ii * TQ_CELL * 16.0f;
        v[k].v = jj * TQ_CELL * 16.0f;
    }
    if (!((ca | cb | cc) & 1)) {
        for (int k = 0; k < 3; k++) fp_proj(j, cam[k][0], cam[k][1], cam[k][2], &v[k]);
        tq_tri(g, &v[0], &v[1], &v[2], TQ_TRI_DETAIL | TQ_TRI_CULL);
        return 1;
    }
    /* clip against zc = near: up to four corners */
    tq_vtx_t out[4];
    float oc[4][3];
    int n = 0;
    for (int k = 0; k < 3; k++) {
        int l = (k + 1) % 3;
        bool ink = cam[k][2] >= FP_NEAR, inl = cam[l][2] >= FP_NEAR;
        if (ink) {
            out[n] = v[k];
            memcpy(oc[n], cam[k], sizeof oc[n]);
            n++;
        }
        if (ink != inl) {
            float f = (FP_NEAR - cam[k][2]) / (cam[l][2] - cam[k][2]);
            tq_vtx_t m;
            m.r = v[k].r + (v[l].r - v[k].r) * f;
            m.g = v[k].g + (v[l].g - v[k].g) * f;
            m.b = v[k].b + (v[l].b - v[k].b) * f;
            m.u = v[k].u + (v[l].u - v[k].u) * f;
            m.v = v[k].v + (v[l].v - v[k].v) * f;
            out[n] = m;
            for (int q = 0; q < 3; q++) oc[n][q] = cam[k][q] + (cam[l][q] - cam[k][q]) * f;
            n++;
        }
    }
    if (n < 3) return 0;
    for (int k = 0; k < n; k++) fp_proj(j, oc[k][0], oc[k][1], oc[k][2], &out[k]);
    for (int k = 1; k + 1 < n; k++) tq_tri(g, &out[0], &out[k], &out[k + 1], TQ_TRI_DETAIL | TQ_TRI_CULL);
    return n - 2;
}

/* Far hills all round: a height for an azimuth */
static float hills(float az, int layer)
{
    if (layer == 0) return 0.045f + 0.025f * sinf(3 * az + 1.0f) + 0.016f * sinf(7 * az + 2.0f) + 0.008f * sinf(17 * az);
    return 0.022f + 0.018f * sinf(5 * az + 3.0f) + 0.010f * sinf(11 * az + 0.5f) + 0.006f * sinf(23 * az + 1);
}

static void fp_sky(const fpjob_t *j, const tq_tgt_t *g)
{
    int W = j->fw;
    float yhz = j->yhz;
    for (int y = g->cy0; y < g->cy1; y++) {
        uint16_t *p = g->px + (size_t)y * W, *z = g->z + (size_t)y * W;
        uint16_t c;
        if (y < yhz) {
            float f = tq_clampf((yhz - y) / (j->f * 0.9f), 0, 1);
            c = tq_565(mix_rgb(j->sky_low, j->sky_top, f));
        } else {
            float f = tq_clampf((y - yhz) / (j->f * 0.5f), 0, 1);
            c = tq_565(mix_rgb(j->far, j->near, f));
        }
        for (int x = 0; x < W; x++) {
            p[x] = c;
            z[x] = 0;
        }
    }
    /* the hills, column by column: two ranges, the nearer greener */
    float phi = j->r->yaw;
    uint16_t far = tq_565(0xA4BDD0), nearc = tq_565(0x8DB394);
    for (int x = 0; x < W; x++) {
        float az = phi - atan2f((float)x + 0.5f - j->cx, j->f);
        for (int layer = 0; layer < 2; layer++) {
            float top = yhz - j->f * hills(az, layer);
            int y0 = (int)top, y1 = (int)yhz + 1;
            if (y0 < g->cy0) y0 = g->cy0;
            if (y1 > g->cy1) y1 = g->cy1;
            uint16_t c = layer ? nearc : far;
            for (int y = y0; y < y1; y++) g->px[(size_t)y * W + x] = c;
        }
    }
}

static int fp_items(const tq_req_t *r, const tq_terrain_t *t, fpjob_t *j, item_t *it)
{
    const tq_state_t *s = &r->st;
    int n = 0;
    const tq_lut_t *scen = tq_art_scenery_lut();
    for (int k = 0; k < s->ntree && n < MAX_ITEMS; k++) {
        const tq_tree_t *tr = &s->tree[k];
        it[n].kind = 0;
        it[n].p = tq_v3(tr->x, tr->y, tq_ter_height(t, tr->x, tr->y));
        it[n].spr = NULL;
        it[n].r = tr->alive ? tr->kind : TQ_PROP_STUMP;
        it[n].lut = scen;
        it[n].alpha = 256;
        it[n].rgb = 0;
        n++;
    }
    for (int k = 0; k < s->nblock && n < MAX_ITEMS; k++) {
        const tq_block_t *b = &s->block[k];
        if (!b->alive) continue;
        it[n].kind = 0;
        it[n].p = tq_v3(b->i + 0.5f, b->j + 0.5f, b->z);
        it[n].spr = NULL;
        it[n].r = TQ_PROP_BLOCK1 + b->variant;
        it[n].lut = scen;
        it[n].alpha = 256;
        it[n].rgb = 1;
        n++;
    }
    for (int k = 0; k < s->ntank && n < MAX_ITEMS - 1; k++) {
        if (k == r->me) continue;
        const tq_tank_t *tk = &s->tank[k];
        it[n].kind = 2;
        it[n].p = tq_v3(tk->x, tk->y, tq_tank_z(t, tk));
        it[n].lut = tk->alive ? r->lut[k] : r->wreck;
        it[n].alpha = 256;
        it[n].r = (float)k;
        if (it[n].lut) n++;
    }
    for (int k = 0; k < TQ_MAX_SHELLS && n < MAX_ITEMS; k++) {
        const tq_shell_t *sh = &s->shell[k];
        if (!sh->live || sh->under) continue;
        it[n].kind = 1;
        it[n].p = sh->p;
        it[n].r = 0.2f;
        it[n].rgb = TQ_WEAPON[sh->weapon].rgb;
        it[n].alpha = 256;
        n++;
    }
    for (int k = 0; k < TQ_MAX_MINES && n < MAX_ITEMS; k++) {
        const tq_mine_t *mn = &s->mine[k];
        if (!mn->live) continue;
        it[n].kind = 1;
        it[n].p = tq_v3(mn->x, mn->y, tq_ter_height(t, mn->x, mn->y) + 0.12f);
        it[n].r = 0.24f;
        it[n].rgb = ((int)(s->pulse * 4) & 1) ? 0xFF4A3A : 0x4A4E56;
        it[n].alpha = 256;
        n++;
    }
    if (s->beam_t > 0) {
        for (int k = 0; k <= 40 && n < MAX_ITEMS; k++) {
            float f = k / 40.0f;
            it[n].kind = 1;
            it[n].p = tq_v3(s->beam_a.x + (s->beam_b.x - s->beam_a.x) * f, s->beam_a.y + (s->beam_b.y - s->beam_a.y) * f,
                            s->beam_a.z + (s->beam_b.z - s->beam_a.z) * f);
            it[n].r = 0.12f;
            it[n].rgb = s->beam_rgb;
            it[n].alpha = (int)(512 * s->beam_t);
            n++;
        }
    }
    for (int k = 0; k < s->npart && n < MAX_ITEMS; k++) {
        const tq_part_t *q = &s->part[k];
        if (q->kind == TQ_PART_RING) continue;
        it[n].kind = 1;
        it[n].p = q->p;
        it[n].r = part_radius(q);
        it[n].rgb = part_rgb(q, &it[n].alpha);
        n++;
    }
    int m = 0;
    for (int k = 0; k < n; k++) {
        tq_v3_t d = tq_v3(it[k].p.x - r->eye.x, it[k].p.y - r->eye.y, it[k].p.z - r->eye.z);
        float zc = tq_dot(d, j->Fw);
        if (zc < 0.6f || zc > 70) continue;
        it[m] = it[k];
        it[m].depth = zc;
        m++;
    }
    qsort(it, (size_t)m, sizeof(item_t), cmp_far_first);
    return m;
}

static void fp_project_pt(const fpjob_t *j, tq_v3_t p, float *sx, float *sy, float *zc)
{
    tq_v3_t d = tq_v3(p.x - j->r->eye.x, p.y - j->r->eye.y, p.z - j->r->eye.z);
    *zc = tq_dot(d, j->Fw);
    float iz = 1.0f / (*zc > 0.05f ? *zc : 0.05f);
    *sx = j->cx + j->f * tq_dot(d, j->Rw) * iz;
    *sy = j->cy - j->f * tq_dot(d, j->Uw) * iz;
}

static void fp_sprite(const fpjob_t *j, const tq_tgt_t *g, const tq_spr_t *s0, int set_mip_from, int prop, int part,
                      int yaw_idx, const tq_lut_t *lut, float sx, float sy, float zc, int alpha)
{
    float scale = j->f / zc / TQ_LOW_S;
    int mip = scale < 0.3f ? 2 : (scale < 0.6f ? 1 : 0);
    const tq_spr_t *s;
    if (set_mip_from >= 100) s = tq_art_tank(TQ_SET_LOW, set_mip_from - 100, part, yaw_idx, mip);
    else s = tq_art_prop(TQ_SET_LOW, prop, yaw_idx, mip);
    if (!s) s = s0;
    if (!s) return;
    float sc = scale * (float)(1 << mip);
    float q0 = TQ_QK / zc;
    float qs = TQ_QK / (zc * zc) / 32.0f * 256.0f;
    tq_spr_scaled(g, s, lut, sx, sy, sc, (int)q0, (int)(qs + 0.5f), alpha);
}

static void fp_part(void *arg, int part)
{
    fpjob_t *j = (fpjob_t *)arg;
    const tq_req_t *r = j->r;
    const tq_state_t *s = &r->st;
    tq_tgt_t g = { S.fpx, S.fz, j->fw, j->fh, j->fw, 0, 0, j->fw, j->fh };
    if (part == 0) g.cy1 = j->fh / 2;
    else g.cy0 = j->fh / 2;
    fp_sky(j, &g);
    /* the ground */
    int tris = 0;
    int N = j->t->n, NV = j->t->nv;
    for (int cj = 0; cj < N; cj++) {
        for (int ci = 0; ci < N; ci++) {
            int a = cj * NV + ci, b = a + 1, c = a + NV + 1, d = a + NV;
            tris += fp_tri(j, &g, a, b, c);
            tris += fp_tri(j, &g, a, c, d);
        }
    }
    j->tris[part] = tris;
    /* the things, far first */
    for (int k = 0; k < j->n; k++) {
        const item_t *i = &j->items[k];
        float sx, sy, zc;
        fp_project_pt(j, i->p, &sx, &sy, &zc);
        if (i->kind == 0) {
            int prop = (int)i->r;
            int yi = 0;
            if (prop >= TQ_PROP_BLOCK1) {
                float rel = -r->yaw;
                rel = fmodf(rel, TQ_PI / 2);
                if (rel < 0) rel += TQ_PI / 2;
                yi = (int)(rel / (11.25f * TQ_DEG) + 0.5f) & 7;
            }
            float rad = prop == TQ_PROP_STUMP ? 0.4f : (prop >= TQ_PROP_BLOCK1 ? 0.75f : 1.2f);
            tq_shadow(&g, sx + j->f * rad * 0.35f / zc, sy, j->f * rad / zc, j->f * rad / zc * 0.25f, 110);
            fp_sprite(j, &g, NULL, 1, prop, 0, yi, i->lut, sx, sy, zc, i->alpha);
        } else if (i->kind == 2) {
            const tq_tank_t *tk = &s->tank[(int)i->r];
            tq_shadow(&g, sx + j->f * 0.5f / zc, sy, j->f * 1.7f / zc, j->f * 1.7f / zc * 0.25f, 150);
            int hy = tq_yaw_index(tk->yaw - r->yaw), ty = tq_yaw_index(tk->tyaw - r->yaw);
            fp_sprite(j, &g, NULL, 100 + tk->type, 0, TQ_PART_HULL, hy, i->lut, sx, sy, zc, 256);
            if (tk->alive) fp_sprite(j, &g, NULL, 100 + tk->type, 0, TQ_PART_TURRET, ty, i->lut, sx, sy, zc, 256);
            if (r->hp_bars && tk->alive) {
                float bx, by, bz;
                fp_project_pt(j, tq_v3(i->p.x, i->p.y, i->p.z + 2.6f), &bx, &by, &bz);
                int wbar = 44;
                tq_rect(&g, (int)bx - wbar / 2 - 1, (int)by - 1, wbar + 2, 7, 0x1A1A22, 200);
                uint32_t c = tk->hp > 50 ? 0x5ED16A : (tk->hp > 25 ? 0xFFC83C : 0xFF5A46);
                tq_rect(&g, (int)bx - wbar / 2, (int)by, wbar * tk->hp / tk->hpmax, 5, c, 256);
            }
        } else {
            tq_ball(&g, sx, sy, j->f * i->r / zc, i->rgb, i->alpha, (int)(TQ_QK / zc), S.fz, j->fw);
        }
    }
    /* the aim's guide */
    if (s->narc > 0 && r->me == s->cur) {
        for (int k = 0; k < s->narc; k++) {
            float sx, sy, zc;
            fp_project_pt(j, s->arc[k], &sx, &sy, &zc);
            if (zc < 0.8f) continue;
            float rr = tq_clampf(j->f * 0.07f / zc, 1.2f, 4.5f);
            int fade = s->arc_full ? 240 : 240 - 150 * k / (s->narc > 1 ? s->narc : 1);
            tq_ball(&g, sx, sy, rr, 0xFFFFFF, fade, (int)(TQ_QK / zc) + 40, S.fz, j->fw);
        }
        if (s->arc_full && s->arc_landed) {
            float sx, sy, zc;
            fp_project_pt(j, s->arc_end, &sx, &sy, &zc);
            if (zc > 1) {
                float rr = j->f * TQ_WEAPON[tq_weapon_of(&s->tank[s->cur])].radius / zc;
                float k = tq_clampf((r->eye.z - s->arc_end.z) / zc, 0.12f, 1.0f);
                tq_ring(&g, sx, sy, rr, rr * k, 2.5f, 0xFF5A46, 230);
                tq_ring(&g, sx, sy, rr * 0.35f, rr * 0.35f * k, 2.0f, 0xFFFFFF, 230);
            }
        }
    }
    /* the turret and the barrel, nearest of all */
    const tq_tank_t *me = &s->tank[r->me];
    const tq_lut_t *lut = r->lut[r->me];
    int pi = (int)floorf((me->pitch / TQ_DEG + 5.0f) / 5.0f + 0.5f);
    const tq_spr_t *bar = tq_art_barrel(me->type, pi), *roof = tq_art_roof(me->type);
    int ax = (int)floorf(j->cx + 0.5f), ay = (int)floorf(j->cy + 0.5f);
    if (bar) tq_spr_draw(&g, bar, lut, ax, ay, TQ_QMAX - 128, 1, 256, S.fz, j->fw, true);
    if (roof) tq_spr_draw(&g, roof, lut, ax, ay, TQ_QMAX - 128, 1, 256, S.fz, j->fw, true);
}

typedef struct {
    uint16_t *fb;
    int W, H, fw, fh;
} upjob_t;

static void up_part(void *arg, int part)
{
    upjob_t *u = (upjob_t *)arg;
    int y0 = part ? u->H / 2 : 0, y1 = part ? u->H : u->H / 2;
    tq_up2(u->fb, u->W, S.fpx, u->fw, u->fh, y0, y1);
}

static void render_fp(const tq_req_t *r, const tq_terrain_t *t, uint16_t *fb, tq_prof_t *prof)
{
    static item_t items[MAX_ITEMS];
    fpjob_t j;
    memset(&j, 0, sizeof j);
    j.r = r;
    j.t = t;
    j.fb = fb;
    j.fw = r->w / 2;
    j.fh = r->h / 2;
    float cx, cy, f;
    tq_scene_fp_frame(r->w, r->h, &cx, &cy, &f);
    j.cx = cx / 2;
    j.cy = cy / 2;
    j.f = f / 2;
    float cp = cosf(r->pitch), sp = sinf(r->pitch), cyw = cosf(r->yaw), syw = sinf(r->yaw);
    j.Fw = tq_v3(cp * cyw, cp * syw, sp);
    j.Rw = tq_v3(syw, -cyw, 0);
    j.Uw = tq_v3(j.Rw.y * j.Fw.z - j.Rw.z * j.Fw.y, j.Rw.z * j.Fw.x - j.Rw.x * j.Fw.z, j.Rw.x * j.Fw.y - j.Rw.y * j.Fw.x);
    j.yhz = j.cy + j.f * tanf(r->pitch);
    j.sky_top = tq_ter_far_colour(t->biome, 0);
    j.sky_low = tq_ter_far_colour(t->biome, 1);
    j.far = tq_ter_far_colour(t->biome, 2);
    j.near = mix_rgb(j.far, 0x203020, 0.25f);
    j.fog = j.sky_low;
    bool fog = r->st.weather == TQ_WX_FOG;
    j.fog_near = fog ? 4.0f : 16.0f;
    j.fog_max = fog ? 0.85f : 0.55f;
    if (fog) j.far = j.sky_low;
    uint32_t t0 = now_us();
    fp_vertices(&j);
    j.items = items;
    j.n = fp_items(r, t, &j, items);
    uint32_t t1 = now_us();
    run(fp_part, &j);
    uint32_t t2 = now_us();
    upjob_t u = { fb, r->w, r->h, j.fw, j.fh };
    run(up_part, &u);
    uint32_t t3 = now_us();
    if (prof) {
        prof->base_us += t2 - t0;
        prof->things_us += t1 - t0;
        prof->up_us += t3 - t2;
        prof->tris = j.tris[0] + j.tris[1];
        prof->sprites = j.n;
    }
}

/* The hangar: a backdrop, a turntable, the tank */
static void show_part(void *arg, int part)
{
    const tq_req_t *r = ((const void **)arg)[0];
    uint16_t *fb = ((void **)arg)[1];
    int W = r->w, H = r->h;
    tq_tgt_t g = { fb, NULL, W, H, W, 0, part ? H / 2 : 0, W, part ? H : H / 2 };
    for (int y = g.cy0; y < g.cy1; y++) {
        float f = (float)y / H;
        uint16_t c = tq_565(mix_rgb(0x34404E, 0x141A22, f));
        uint16_t *p = fb + (size_t)y * W;
        for (int x = 0; x < W; x++) p[x] = c;
    }
    float cx = r->show_x, cy = r->show_y;
    tq_ring(&g, cx, cy + 6, 270, 70, 10, 0x56677A, 200);
    tq_shadow(&g, cx, cy + 4, 250, 62, 120);
    tq_shadow(&g, cx + 20, cy, 190, 40, 200);
    const tq_spr_t *spr = tq_art_show(r->show_type, r->show_frame);
    if (spr && r->lut[0]) tq_spr_draw(&g, spr, r->lut[0], (int)cx, (int)cy, 0, 0, 256, NULL, 0, false);
}

void tq_scene_render(const tq_req_t *r, const tq_terrain_t *t, uint16_t *fb, tq_prof_t *prof)
{
    uint32_t t0 = now_us();
    if (r->mode == TQ_VIEW_SHOW) {
        const void *arg[2] = { r, fb };
        run(show_part, (void *)arg);
    } else if (r->mode == TQ_VIEW_FP) render_fp(r, t, fb, prof);
    else render_top(r, t, fb, prof);
    if (prof) prof->total_us += now_us() - t0;
}
