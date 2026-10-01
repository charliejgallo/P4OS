/*
 * MILA - a frame (see ml_render.h)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "ml_render.h"

#include <stdlib.h>
#include <string.h>

#define BIAS 2          /* a sprite may sit this far "inside" the ground */

void ml_dlist_clear(ml_dlist_t *l)
{
    l->n = 0;
}

ml_draw_t *ml_dlist_add(ml_dlist_t *l)
{
    if (l->n >= ML_MAX_DRAW) return NULL;
    ml_draw_t *d = &l->d[l->n++];
    memset(d, 0, sizeof(*d));
    d->alpha = 255;
    return d;
}

static int cmp_draw(const void *pa, const void *pb)
{
    const ml_draw_t *a = (const ml_draw_t *)pa, *b = (const ml_draw_t *)pb;
    /* shadows under everything */
    int sa = a->fmt == ML_PX_PLANE, sb = b->fmt == ML_PX_PLANE;
    if (sa != sb) return sb - sa;
    if (a->d != b->d) return (int)b->d - (int)a->d;
    return (int)a->prio - (int)b->prio;
}

void ml_dlist_sort(ml_dlist_t *l)
{
    qsort(l->d, (size_t)l->n, sizeof(ml_draw_t), cmp_draw);
}

/* ---- the sprites ---- */

static inline const uint16_t *cache_row(const ml_world_t *w, int lpy)
{
    return w->cd + (size_t)ml_chwrap(lpy) * ML_CW;
}

static void draw_lid(const ml_world_t *w, ml_img_t *im, const ml_draw_t *e, int cam_x, int cam_y)
{
    const ml_spr_t *s = e->s;
    const ml_lut_t *lut = e->lut;
    int lx0 = e->x - s->ax, ly0 = e->y - s->ay;         /* LP of the frame's (0,0) */
    int sx = lx0 - cam_x, sy = ly0 - cam_y;
    int r0 = im->cy0 - sy, r1 = im->cy1 - sy;
    if (r0 < 0) r0 = 0;
    if (r1 > s->h) r1 = s->h;
    int ga = e->alpha;
    for (int r = r0; r < r1; r++) {
        int x0, x1;
        const uint8_t *p = ml_spr_row(s, r, &x0, &x1);
        int c0 = sx + x0, c1 = sx + x1;
        if (c0 < im->cx0) { p += (size_t)(im->cx0 - c0) * 3; c0 = im->cx0; }
        if (c1 > im->cx1) c1 = im->cx1;
        if (c0 >= c1) continue;
        uint16_t *dst = im->px + (size_t)(sy + r) * im->w;
        const uint16_t *cd = cache_row(w, ly0 + r);
        int ci = ml_cwrap(c0 + cam_x);         /* the cache column, wrapping */
        for (int x = c0; x < c1; x++, p += 3, ci = ci + 1 == ML_CW ? 0 : ci + 1) {
            int ida = p[0];
            int a = (ida & 15) * 17;
            if (!a) continue;
            int sd = e->d + p[2] - 128;
            if (ga < 255) a = a * ga >> 8;
            if (sd < (int)cd[ci] + BIAS || (e->flags & DR_NOZ)) {
                dst[x] = ml_blend(dst[x], lut->c[ida >> 4][p[1] >> 2], a);
            } else if (e->flags & DR_XRAY) {
                dst[x] = ml_blend(dst[x], e->xray, a * 3 >> 3);
            }
        }
    }
}

static void draw_col(const ml_world_t *w, ml_img_t *im, const ml_draw_t *e, int cam_x, int cam_y)
{
    const ml_spr_t *s = e->s;
    int lx0 = e->x - s->ax, ly0 = e->y - s->ay;
    int sx = lx0 - cam_x, sy = ly0 - cam_y;
    int r0 = im->cy0 - sy, r1 = im->cy1 - sy;
    if (r0 < 0) r0 = 0;
    if (r1 > s->h) r1 = s->h;
    int ga = e->alpha;
    bool add = (e->flags & DR_ADD) != 0, noz = (e->flags & DR_NOZ) != 0;
    for (int r = r0; r < r1; r++) {
        int x0, x1;
        const uint8_t *p = ml_spr_row(s, r, &x0, &x1);
        int c0 = sx + x0, c1 = sx + x1;
        if (c0 < im->cx0) { p += (size_t)(im->cx0 - c0) * 4; c0 = im->cx0; }
        if (c1 > im->cx1) c1 = im->cx1;
        if (c0 >= c1) continue;
        uint16_t *dst = im->px + (size_t)(sy + r) * im->w;
        const uint16_t *cd = cache_row(w, ly0 + r);
        int ci = ml_cwrap(c0 + cam_x);         /* the cache column, wrapping */
        for (int x = c0; x < c1; x++, p += 4, ci = ci + 1 == ML_CW ? 0 : ci + 1) {
            int a = p[2];
            if (!a || p[3] == ML_Z_EMPTY) continue;
            int sd = e->d + p[3] - 128;
            if (ga < 255) a = a * ga >> 8;
            if (!noz && sd >= (int)cd[ci] + BIAS) {
                /* hidden: a faint silhouette shows through (Mila) */
                if (e->flags & DR_XRAY) dst[x] = ml_blend(dst[x], e->xray, a * 3 >> 3);
                continue;
            }
            uint16_t c = (uint16_t)(p[0] | (p[1] << 8));
            if (add) dst[x] = ml_add(dst[x], ml_scale(c, a));
            else dst[x] = ml_blend(dst[x], c, a);
        }
    }
}

static void draw_plane(const ml_world_t *w, ml_img_t *im, const ml_draw_t *e, int cam_x, int cam_y)
{
    const ml_spr_t *s = e->s;
    int lx0 = e->x - s->ax, ly0 = e->y - s->ay;
    int sx = lx0 - cam_x, sy = ly0 - cam_y;
    int r0 = im->cy0 - sy, r1 = im->cy1 - sy;
    if (r0 < 0) r0 = 0;
    if (r1 > s->h) r1 = s->h;
    int k = e->alpha;
    for (int r = r0; r < r1; r++) {
        int x0, x1;
        const uint8_t *p = ml_spr_row(s, r, &x0, &x1);
        int c0 = sx + x0, c1 = sx + x1;
        if (c0 < im->cx0) { p += im->cx0 - c0; c0 = im->cx0; }
        if (c1 > im->cx1) c1 = im->cx1;
        if (c0 >= c1) continue;
        uint16_t *dst = im->px + (size_t)(sy + r) * im->w;
        const uint16_t *cd = cache_row(w, ly0 + r);
        int dp = e->d + ml_iround((float)(ly0 + r - e->y) * w->dplane);
        int ci = ml_cwrap(c0 + cam_x);         /* the cache column, wrapping */
        for (int x = c0; x < c1; x++, p++, ci = ci + 1 == ML_CW ? 0 : ci + 1) {
            int v = *p;
            if (!v) continue;
            int dz = (int)cd[ci] - dp;
            if (dz < -4 || dz > 4) continue;
            dst[x] = ml_darken(dst[x], 256 - (v * k >> 8));
        }
    }
}

void ml_render_band(const ml_world_t *w, ml_img_t *im, int cam_x, int cam_y, int y0, int y1,
                    const ml_dlist_t *l)
{
    /* the background: rows of the cache, wrapping, over the clip's columns */
    int xa = im->cx0, n = im->cx1 - im->cx0;
    if (n <= 0) return;
    if (w->still_w) {
        /* a backdrop: its box, black around (the cache does not wrap) */
        int l0 = cam_x + xa, l1 = l0 + n;           /* LP columns of the clip */
        int a0 = l0 < 0 ? 0 : l0, a1 = l1 > w->still_w ? w->still_w : l1;
        for (int y = y0; y < y1; y++) {
            uint16_t *dst = im->px + (size_t)y * im->w + xa;
            int ly = cam_y + y;
            if (ly < 0 || ly >= w->still_h || a0 >= a1) {
                memset(dst, 0, (size_t)n * 2);
                continue;
            }
            if (a0 > l0) memset(dst, 0, (size_t)(a0 - l0) * 2);
            memcpy(dst + (a0 - l0), w->cc + (size_t)ly * ML_CW + a0, (size_t)(a1 - a0) * 2);
            if (l1 > a1) memset(dst + (a1 - l0), 0, (size_t)(l1 - a1) * 2);
        }
        goto sprites;
    }
    int cx = ml_cwrap(cam_x + xa);
    int n1 = ML_CW - cx;
    if (n1 > n) n1 = n;
    for (int y = y0; y < y1; y++) {
        const uint16_t *src = w->cc + (size_t)ml_chwrap(cam_y + y) * ML_CW;
        uint16_t *dst = im->px + (size_t)y * im->w + xa;
        memcpy(dst, src + cx, (size_t)n1 * 2);
        if (n1 < n) memcpy(dst + n1, src, (size_t)(n - n1) * 2);
    }
sprites:
    for (int i = 0; i < l->n; i++) {
        const ml_draw_t *e = &l->d[i];
        const ml_spr_t *s = e->s;
        if (!s) continue;
        int top = e->y - s->ay - cam_y;
        if (top >= y1 || top + s->h <= y0) continue;
        /* and across */
        int left = e->x - s->ax - cam_x;
        if (left >= im->cx1 || left + s->w <= im->cx0) continue;
        switch (e->fmt) {
        case ML_PX_LID: draw_lid(w, im, e, cam_x, cam_y); break;
        case ML_PX_COL: draw_col(w, im, e, cam_x, cam_y); break;
        case ML_PX_PLANE: draw_plane(w, im, e, cam_x, cam_y); break;
        default: break;
        }
    }
}

/* ---- what changed ---- */

void ml_dmg_clear(ml_dmg_t *d)
{
    memset(d, 0, sizeof(*d));
}

void ml_dmg_full(ml_dmg_t *d)
{
    d->full = true;
}

void ml_dmg_rect(ml_dmg_t *d, int x, int y, int w, int h)
{
    if (d->full || w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ML_W) x1 = ML_W;
    if (y1 > ML_H) y1 = ML_H;
    if (x0 >= x1 || y0 >= y1) return;
    int tx0 = x0 / ML_TILE, tx1 = (x1 - 1) / ML_TILE;
    int ty0 = y0 / ML_TILE, ty1 = (y1 - 1) / ML_TILE;
    uint64_t bits = (tx1 >= 63 ? ~0ull : ((1ull << (tx1 + 1)) - 1)) & ~((1ull << tx0) - 1);
    for (int ty = ty0; ty <= ty1 && ty < ML_TMAX; ty++) d->row[ty] |= bits;
    d->any = true;
}

void ml_dmg_or(ml_dmg_t *d, const ml_dmg_t *o)
{
    if (o->full) d->full = true;
    if (d->full) return;
    if (!o->any) return;
    for (int i = 0; i < ML_TMAX; i++) d->row[i] |= o->row[i];
    d->any = true;
}

bool ml_dmg_span(const ml_dmg_t *d, int y0, int y1, int *x0, int *x1)
{
    if (d->full) {
        *x0 = 0;
        *x1 = ML_W;
        return true;
    }
    if (!d->any || y1 <= y0) return false;
    uint64_t m = 0;
    for (int ty = y0 / ML_TILE; ty <= (y1 - 1) / ML_TILE && ty < ML_TMAX; ty++) m |= d->row[ty];
    if (!m) return false;
    int a = ml_bit_lo(m), b = ml_bit_hi(m);
    *x0 = a * ML_TILE;
    *x1 = (b + 1) * ML_TILE;
    if (*x1 > ML_W) *x1 = ML_W;
    return true;
}

int ml_dmg_count(const ml_dmg_t *d, int *of)
{
    int tw = (ML_W + ML_TILE - 1) / ML_TILE, th = (ML_H + ML_TILE - 1) / ML_TILE;
    *of = tw * th;
    if (d->full) return tw * th;
    int n = 0;
    for (int i = 0; i < th && i < ML_TMAX; i++) n += ml_bit_count(d->row[i]);
    return n;
}

static bool same(const ml_draw_t *a, const ml_draw_t *b)
{
    return a->s == b->s && a->x == b->x && a->y == b->y && a->d == b->d && a->fmt == b->fmt &&
           a->alpha == b->alpha && a->flags == b->flags && a->prio == b->prio && a->lut == b->lut;
}

static void box(ml_dmg_t *d, const ml_draw_t *e, int cam_x, int cam_y)
{
    const ml_spr_t *s = e->s;
    if (!s) return;
    ml_dmg_rect(d, e->x - s->ax - cam_x, e->y - s->ay - cam_y, s->w, s->h);
}

void ml_dlist_damage(const ml_dlist_t *prev, const ml_dlist_t *cur, int cam_x, int cam_y, ml_dmg_t *d)
{
    /* both lists are sorted by depth, so a thing that did not change is
     * usually at the same index: look there first */
    for (int i = 0; i < cur->n; i++) {
        const ml_draw_t *e = &cur->d[i];
        bool found = i < prev->n && same(e, &prev->d[i]);
        for (int j = 0; j < prev->n && !found; j++) found = same(e, &prev->d[j]);
        if (!found) box(d, e, cam_x, cam_y);
    }
    for (int j = 0; j < prev->n; j++) {
        const ml_draw_t *e = &prev->d[j];
        bool found = j < cur->n && same(e, &cur->d[j]);
        for (int i = 0; i < cur->n && !found; i++) found = same(e, &cur->d[i]);
        if (!found) box(d, e, cam_x, cam_y);
    }
}
