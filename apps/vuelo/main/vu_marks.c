/*
 * PLANEO - the marks (see vu_marks.h)
 */
#include "vu_marks.h"
#include "vu_plane.h"

#include <math.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#define SEGS 20

static inline float wrapd(float d)
{
    if (d > VW_SIZE * 0.5f) d -= VW_SIZE;
    if (d < -VW_SIZE * 0.5f) d += VW_SIZE;
    return d;
}

static void add(vm_draw_t *d, const float *x, const float *y, uint16_t col, int alpha)
{
    if (d->n >= VM_MAXT) return;
    vm_tri_t *t = &d->t[d->n++];
    memcpy(t->x, x, sizeof t->x);
    memcpy(t->y, y, sizeof t->y);
    t->col = col;
    t->alpha = (uint8_t)alpha;
}

/* the eye sees the point: no ground in between (a few samples) */
static bool seen(const vw_world_t *w, const vr_proj_t *p, const float *q)
{
    float dx = wrapd(q[0] - p->cx), dy = wrapd(q[1] - p->cy), dz = q[2] - p->cz;
    for (int i = 1; i < 10; i++) {
        float t = (float)i / 10.0f;
        float h = vw_height(w, vw_wrap(p->cx + dx * t), vw_wrap(p->cy + dy * t));
        if (h > p->cz + dz * t + 1.0f) return false;
    }
    return true;
}

static void ring(vm_draw_t *d, const vr_proj_t *p, const vg_pt_t *c, float R, float wdt, uint16_t col, int alpha)
{
    /* the ring's plane: a and b across its facing */
    float n[3] = { c->n[0], c->n[1], c->n[2] };
    float a[3] = { n[1], -n[0], 0 };
    float la = sqrtf(a[0] * a[0] + a[1] * a[1]);
    if (la < 1e-3f) {
        a[0] = 1;
        a[1] = 0;
        la = 1;
    }
    a[0] /= la;
    a[1] /= la;
    float b[3] = { a[1] * n[2] - a[2] * n[1], a[2] * n[0] - a[0] * n[2], a[0] * n[1] - a[1] * n[0] };
    float ix[SEGS + 1], iy[SEGS + 1], ox[SEGS + 1], oy[SEGS + 1];
    bool ok[SEGS + 1];
    for (int i = 0; i <= SEGS; i++) {
        float t = (float)i * 6.2831853f / SEGS;
        float ct = cosf(t), st = sinf(t);
        float q[3], r[3];
        for (int k = 0; k < 3; k++) {
            float v = a[k] * ct + b[k] * st;
            q[k] = c->p[k] + v * (R - wdt);
            r[k] = c->p[k] + v * (R + wdt);
        }
        ok[i] = vr_project(p, q[0], q[1], q[2], &ix[i], &iy[i], NULL) &&
                vr_project(p, r[0], r[1], r[2], &ox[i], &oy[i], NULL);
    }
    for (int i = 0; i < SEGS; i++) {
        if (!ok[i] || !ok[i + 1]) continue;
        /* the lower half a little darker: it reads as a hoop, not a line */
        uint16_t cc = i >= SEGS / 2 ? vr_blend(col, 0, 6) : col;
        float x1[3] = { ix[i], ox[i], ox[i + 1] }, y1[3] = { iy[i], oy[i], oy[i + 1] };
        float x2[3] = { ix[i], ox[i + 1], ix[i + 1] }, y2[3] = { iy[i], oy[i + 1], iy[i + 1] };
        add(d, x1, y1, cc, alpha);
        add(d, x2, y2, cc, alpha);
    }
}

static void beam(vm_draw_t *d, const vr_proj_t *p, const vg_pt_t *c, uint16_t col, int alpha, float time)
{
    float bx, by, tx, ty, dep;
    if (!vr_project(p, c->p[0], c->p[1], c->p[2], &bx, &by, &dep)) return;
    if (!vr_project(p, c->p[0], c->p[1], c->p[2] + 160.0f, &tx, &ty, NULL)) return;
    float half = p->f * VG_BEAM_R * 0.45f / dep;
    if (half < 1.5f) half = 1.5f;
    /* across the column on the screen */
    float ax = ty - by, ay = -(tx - bx);
    float l = sqrtf(ax * ax + ay * ay);
    if (l < 1e-3f) return;
    ax = ax / l * half;
    ay = ay / l * half;
    float pulse = 0.75f + 0.25f * sinf(time * 4.0f);
    int al = (int)((float)alpha * pulse);
    float x1[3] = { bx - ax, bx + ax, tx + ax }, y1[3] = { by - ay, by + ay, ty + ay };
    float x2[3] = { bx - ax, tx + ax, tx - ax }, y2[3] = { by - ay, ty + ay, ty - ay };
    add(d, x1, y1, col, al);
    add(d, x2, y2, col, al);
    /* a brighter core */
    float cx1[3] = { bx - ax * 0.35f, bx + ax * 0.35f, tx + ax * 0.35f }, cy1[3] = { by - ay * 0.35f, by + ay * 0.35f, ty + ay * 0.35f };
    float cx2[3] = { bx - ax * 0.35f, tx + ax * 0.35f, tx - ax * 0.35f }, cy2[3] = { by - ay * 0.35f, ty + ay * 0.35f, ty - ay * 0.35f };
    add(d, cx1, cy1, 0xFFFF, al / 2);
    add(d, cx2, cy2, 0xFFFF, al / 2);
    /* and the spot on the ground */
    vg_pt_t g = *c;
    g.n[0] = 0;
    g.n[1] = 0;
    g.n[2] = 1;
    g.p[2] += 0.6f;
    /* a ring lying flat: its facing straight up */
    float R = VG_BEAM_R;
    float ix[SEGS + 1], iy[SEGS + 1], ox[SEGS + 1], oy[SEGS + 1];
    bool ok[SEGS + 1];
    for (int i = 0; i <= SEGS; i++) {
        float t = (float)i * 6.2831853f / SEGS;
        float ct = cosf(t), st = sinf(t);
        ok[i] = vr_project(p, g.p[0] + ct * (R - 2.5f), g.p[1] + st * (R - 2.5f), g.p[2], &ix[i], &iy[i], NULL) &&
                vr_project(p, g.p[0] + ct * R, g.p[1] + st * R, g.p[2], &ox[i], &oy[i], NULL);
    }
    for (int i = 0; i < SEGS; i++) {
        if (!ok[i] || !ok[i + 1]) continue;
        float x1b[3] = { ix[i], ox[i], ox[i + 1] }, y1b[3] = { iy[i], oy[i], oy[i + 1] };
        float x2b[3] = { ix[i], ox[i + 1], ix[i + 1] }, y2b[3] = { iy[i], oy[i + 1], iy[i + 1] };
        add(d, x1b, y1b, col, 24);
        add(d, x2b, y2b, col, 24);
    }
}

void vm_prepare(vm_draw_t *d, const vg_game_t *g, const vw_world_t *w, const vr_proj_t *p, float time)
{
    d->n = 0;
    d->target = false;
    if (g->kind == VG_FREE || g->next >= g->npt) return;
    uint16_t gold = vr_565(0xFFC83C), white = vr_565(0xF4F8FF);
    uint16_t green = vr_565(0x4CFF7A), orange = vr_565(0xFF9A2E);
    if (g->kind == VG_DELIVER) {
        const vg_pt_t *c = &g->pt[g->next];
        /* the column is tall: seen over the hills, so never hidden */
        beam(d, p, c, g->carrying ? orange : green, 16, time);
    } else {
        /* far first: the third, the second, then the next */
        for (int k = 2; k >= 0; k--) {
            int i = g->next + k;
            if (i >= g->npt) continue;
            const vg_pt_t *c = &g->pt[i];
            if (!seen(w, p, c->p)) continue;
            bool last = i == g->npt - 1;
            uint16_t col = k == 0 ? (last ? vr_565(0x7CFF8C) : gold) : white;
            ring(d, p, c, VG_RING_R, k == 0 ? 1.1f : 0.7f, col, k == 0 ? 30 : 10);
        }
    }
    /* where the next one is */
    const vg_pt_t *c = &g->pt[g->next];
    float z = g->kind == VG_DELIVER ? c->p[2] + 20.0f : c->p[2];
    float dx = wrapd(c->p[0] - p->cx), dy = wrapd(c->p[1] - p->cy), dz = z - p->cz;
    d->dist = sqrtf(dx * dx + dy * dy + dz * dz);
    d->target = true;
    float sx, sy;
    d->on_screen = vr_project(p, c->p[0], c->p[1], z, &sx, &sy, NULL) && sx >= 0 && sy >= 0 && sx < (float)p->W &&
                   sy < (float)p->H;
    d->tx = sx;
    d->ty = sy;
    /* the direction on the frame, also when it is behind */
    float fwd = dx * p->fx + dy * p->fy, lat = dx * p->rx + dy * p->ry;
    float ux = lat, uy = -dz;
    if (fwd < 0) {
        /* behind: point to the side it is closer to turning */
        ux = lat >= 0 ? 1.0f : -1.0f;
        uy = 0;
    }
    float ox = ux * p->c - uy * p->s, oy = ux * p->s + uy * p->c;
    float l = sqrtf(ox * ox + oy * oy);
    if (l < 1e-4f) l = 1;
    d->dir_x = ox / l;
    d->dir_y = oy / l;
}

void vm_draw(const vm_draw_t *d, uint16_t *out, int W, int H, int y0, int y1)
{
    if (y1 > H) y1 = H;
    for (int i = 0; i < d->n; i++) {
        const vm_tri_t *t = &d->t[i];
        vp_fill(out, W, y0, y1, t->x, t->y, t->col, t->alpha);
    }
}
