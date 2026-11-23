/*
 * TANQUES - pixels (see tq_gfx.h)
 *
 * The triangles take their attributes from the plane through the three
 * corners (one set of gradients per triangle, the value at the first pixel
 * centre of each span), then step them in 16.16 fixed point: the nearness,
 * the colour and, for the ground, the grain texture's coordinates.
 */
#include "tq_gfx.h"

#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

static uint8_t s_grain[64 * 64];
static int s_zsh;                   /* the depth to test against is this much smaller (log2) */

void tq_gfx_zshift(int shift)
{
    s_zsh = shift;
}

/* ceil(v - 0.5): the first pixel centre at or after v */
static inline int px_start(float v)
{
    float x = v - 0.5f;
    int i = (int)x;
    return i + (x > (float)i);
}

static inline int mini(int a, int b) { return a < b ? a : b; }
static inline int maxi(int a, int b) { return a > b ? a : b; }

void tq_gfx_init(void)
{
    /* a few octaves of hashed noise, wrapping at 64 */
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            float v = 0, amp = 0.5f;
            for (int o = 0; o < 4; o++) {
                int p = 8 << o;                 /* cells per side: 8, 16, 32, 64 */
                float fx = x * p / 64.0f, fy = y * p / 64.0f;
                int ix = (int)fx, iy = (int)fy;
                float tx = fx - ix, ty = fy - iy;
                uint32_t h00 = (uint32_t)(((ix) % p) * 73856093u ^ ((iy) % p) * 19349663u ^ (uint32_t)o * 83492791u);
                uint32_t h10 = (uint32_t)(((ix + 1) % p) * 73856093u ^ ((iy) % p) * 19349663u ^ (uint32_t)o * 83492791u);
                uint32_t h01 = (uint32_t)(((ix) % p) * 73856093u ^ ((iy + 1) % p) * 19349663u ^ (uint32_t)o * 83492791u);
                uint32_t h11 = (uint32_t)(((ix + 1) % p) * 73856093u ^ ((iy + 1) % p) * 19349663u ^ (uint32_t)o * 83492791u);
                float a = ((h00 * 2654435761u) >> 16 & 0xFF) / 255.0f, b = ((h10 * 2654435761u) >> 16 & 0xFF) / 255.0f;
                float c = ((h01 * 2654435761u) >> 16 & 0xFF) / 255.0f, d = ((h11 * 2654435761u) >> 16 & 0xFF) / 255.0f;
                tx = tx * tx * (3 - 2 * tx);
                ty = ty * ty * (3 - 2 * ty);
                v += amp * (a + (b - a) * tx + (c - a) * ty + (a - b - c + d) * tx * ty);
                amp *= 0.5f;
            }
            float m = 214.0f + 42.0f * tq_clampf(v * 1.07f, 0, 1);
            s_grain[y * 64 + x] = (uint8_t)(m > 255 ? 255 : m);
        }
    }
}

/* --------------------------------------------------------------------------
 * Triangles
 * -------------------------------------------------------------------------- */

typedef struct {
    float dx, dy;
} grad_t;

void tq_tri(const tq_tgt_t *t, const tq_vtx_t *a, const tq_vtx_t *b, const tq_vtx_t *c, int flags)
{
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area > -1e-3f && area < 1e-3f) return;
    if ((flags & TQ_TRI_CULL) && area < 0) return;
    float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x));
    float miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
    if (maxx < t->cx0 || minx >= t->cx1 || maxy < t->cy0 || miny >= t->cy1) return;
    int ys = maxi(px_start(miny), t->cy0), ye = mini(px_start(maxy), t->cy1);
    if (ys >= ye) return;

    float inv = 1.0f / area;
    float e1x = b->x - a->x, e1y = b->y - a->y, e2x = c->x - a->x, e2y = c->y - a->y;
#define GRAD(f, g) do { float d1 = b->f - a->f, d2 = c->f - a->f; \
        (g).dx = (d1 * e2y - d2 * e1y) * inv; (g).dy = (d2 * e1x - d1 * e2x) * inv; } while (0)
    grad_t gq, gr, gg, gb, gu = { 0, 0 }, gv = { 0, 0 };
    GRAD(q, gq);
    GRAD(r, gr);
    GRAD(g, gg);
    GRAD(b, gb);
    bool detail = flags & TQ_TRI_DETAIL;
    if (detail) {
        GRAD(u, gu);
        GRAD(v, gv);
    }
#undef GRAD
    int32_t dq = (int32_t)(gq.dx * 65536.0f), dr = (int32_t)(gr.dx * 65536.0f);
    int32_t dg = (int32_t)(gg.dx * 65536.0f), db = (int32_t)(gb.dx * 65536.0f);
    int32_t du = (int32_t)(gu.dx * 65536.0f), dv = (int32_t)(gv.dx * 65536.0f);

    /* the edges, by y */
    const tq_vtx_t *p0 = a, *p1 = b, *p2 = c, *tmp;
    if (p1->y < p0->y) { tmp = p0; p0 = p1; p1 = tmp; }
    if (p2->y < p0->y) { tmp = p0; p0 = p2; p2 = tmp; }
    if (p2->y < p1->y) { tmp = p1; p1 = p2; p2 = tmp; }
    float inv02 = 1.0f / (p2->y - p0->y);
    float inv01 = (p1->y - p0->y) > 1e-5f ? 1.0f / (p1->y - p0->y) : 0;
    float inv12 = (p2->y - p1->y) > 1e-5f ? 1.0f / (p2->y - p1->y) : 0;

    const int stride = t->stride;
    for (int y = ys; y < ye; y++) {
        float py = (float)y + 0.5f;
        float xa = p0->x + (p2->x - p0->x) * (py - p0->y) * inv02;
        float xb = py < p1->y ? p0->x + (p1->x - p0->x) * (py - p0->y) * inv01
                              : p1->x + (p2->x - p1->x) * (py - p1->y) * inv12;
        if (xa > xb) { float s = xa; xa = xb; xb = s; }
        int xs = maxi(px_start(xa), t->cx0), xe = mini(px_start(xb), t->cx1);
        if (xs >= xe) continue;
        float fx = (float)xs + 0.5f - a->x, fy = py - a->y;
        float q0 = a->q + gq.dx * fx + gq.dy * fy;
        q0 = tq_clampf(q0, 0, (float)TQ_QMAX);
        int32_t qi = (int32_t)(q0 * 65536.0f);
        int32_t ri = (int32_t)(tq_clampf(a->r + gr.dx * fx + gr.dy * fy, 0, 255) * 65536.0f);
        int32_t gi = (int32_t)(tq_clampf(a->g + gg.dx * fx + gg.dy * fy, 0, 255) * 65536.0f);
        int32_t bi = (int32_t)(tq_clampf(a->b + gb.dx * fx + gb.dy * fy, 0, 255) * 65536.0f);
        uint16_t *pc = t->px + (size_t)y * stride;
        uint16_t *pz = t->z + (size_t)y * stride;
        if (!detail) {
            for (int x = xs; x < xe; x++) {
                int zq = qi >> 16;
                if (zq > pz[x]) {
                    pz[x] = (uint16_t)zq;
                    pc[x] = (uint16_t)(((ri >> 8) & 0xF800) | ((gi >> 13) & 0x07E0) | ((bi >> 19) & 0x1F));
                }
                qi += dq;
                ri += dr;
                gi += dg;
                bi += db;
            }
        } else {
            int32_t ui = (int32_t)((a->u + gu.dx * fx + gu.dy * fy) * 65536.0f);
            int32_t vi = (int32_t)((a->v + gv.dx * fx + gv.dy * fy) * 65536.0f);
            for (int x = xs; x < xe; x++) {
                int zq = qi >> 16;
                if (zq > pz[x]) {
                    pz[x] = (uint16_t)zq;
                    uint32_t m = s_grain[((vi >> 10) & 0xFC0) | ((ui >> 16) & 63)];
                    uint32_t r8 = ((uint32_t)(ri >> 16) * m) >> 8;
                    uint32_t g8 = ((uint32_t)(gi >> 16) * m) >> 8;
                    uint32_t b8 = ((uint32_t)(bi >> 16) * m) >> 8;
                    pc[x] = (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
                }
                qi += dq;
                ri += dr;
                gi += dg;
                bi += db;
                ui += du;
                vi += dv;
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Sprites
 * -------------------------------------------------------------------------- */

void tq_lut_build(tq_lut_t *lut, const uint32_t rgb[16])
{
    for (int id = 0; id < 16; id++) {
        float c[3] = { ((rgb[id] >> 16) & 255) / 255.0f, ((rgb[id] >> 8) & 255) / 255.0f, (rgb[id] & 255) / 255.0f };
        for (int L = 0; L < 256; L++) {
            float tt = L / 200.0f;
            float lo = tt < 1 ? tt : 1;
            float hi = tq_clampf(tt - 1, 0, 1);
            float o[3];
            for (int k = 0; k < 3; k++) {
                o[k] = c[k] * lo;
                if (k == 2) o[k] += 0.05f * (1 - lo) * lo;
                o[k] = o[k] + (1 - o[k]) * hi * 1.4f;
                o[k] = tq_clampf(o[k], 0, 1);
            }
            uint32_t r = (uint32_t)(o[0] * 255 + 0.5f), g = (uint32_t)(o[1] * 255 + 0.5f), b = (uint32_t)(o[2] * 255 + 0.5f);
            lut->c[id * 256 + L] = tq_565((r << 16) | (g << 8) | b);
        }
    }
}

void tq_spr_draw(const tq_tgt_t *t, const tq_spr_t *s, const tq_lut_t *lut, int x, int y,
                 int q0, int qs, int alpha, const uint16_t *zt, int zstride, bool zwrite)
{
    int x0 = x - s->ax, y0 = y - s->ay;
    int i0 = maxi(0, t->cx0 - x0), i1 = mini(s->w, t->cx1 - x0);
    int j0 = maxi(0, t->cy0 - y0), j1 = mini(s->h, t->cy1 - y0);
    if (i0 >= i1 || j0 >= j1) return;
    int qb = q0 + 128 * qs;
    for (int j = j0; j < j1; j++) {
        const tq_row_t *rw = &s->rows[j];
        int a0 = maxi(i0, rw->x0), a1 = mini(i1, rw->x0 + rw->len);
        if (a0 >= a1) continue;
        int dy = y0 + j;
        int k0 = (int)rw->off - rw->x0;         /* plane index = k0 + i */
        uint16_t *pc = t->px + (size_t)dy * t->stride + x0;
        const uint16_t *pz = zt ? zt + (size_t)(dy >> s_zsh) * zstride : NULL;
        uint16_t *pw = zwrite ? t->z + (size_t)dy * t->stride + x0 : NULL;
        const uint8_t *A = s->A + k0, *Z = s->Z ? s->Z + k0 : NULL;
        for (int i = a0; i < a1; i++) {
            int a = A[i];
            if (!a) continue;
            int q = Z ? qb - Z[i] * qs : q0;
            if (pz && q <= pz[(x0 + i) >> s_zsh]) continue;
            uint16_t c = tq_spr_px(s, lut, k0 + i);
            int aa = (a * alpha) >> 8;
            if (aa >= 250) pc[i] = c;
            else pc[i] = tq_blend(pc[i], c, (aa + 4) >> 3);
            if (pw && a >= 160) pw[i] = (uint16_t)(q < 0 ? 0 : (q > TQ_QMAX ? TQ_QMAX : q));
        }
    }
}

void tq_spr_scaled(const tq_tgt_t *t, const tq_spr_t *s, const tq_lut_t *lut, float x, float y,
                   float scale, int q0, int qs256, int alpha)
{
    if (scale <= 0.01f) return;
    float inv = 1.0f / scale;
    float left = x - s->ax * scale, top = y - s->ay * scale;
    int dx0 = maxi(px_start(left), t->cx0), dx1 = mini(px_start(left + s->w * scale), t->cx1);
    int dy0 = maxi(px_start(top), t->cy0), dy1 = mini(px_start(top + s->h * scale), t->cy1);
    if (dx0 >= dx1 || dy0 >= dy1) return;
    int32_t step = (int32_t)(inv * 65536.0f);
    int qb = q0 * 256 + 128 * qs256;
    for (int dy = dy0; dy < dy1; dy++) {
        int sy = (int)(((float)dy + 0.5f - top) * inv);
        if (sy < 0 || sy >= s->h) continue;
        const tq_row_t *rw = &s->rows[sy];
        if (!rw->len) continue;
        /* only the destination columns over the row's run */
        float rl = left + rw->x0 * scale, rr = left + (rw->x0 + rw->len) * scale;
        int ax0 = maxi(dx0, px_start(rl)), ax1 = mini(dx1, px_start(rr));
        if (ax0 >= ax1) continue;
        int k0 = (int)rw->off - rw->x0;
        uint16_t *pc = t->px + (size_t)dy * t->stride;
        uint16_t *pz = t->z + (size_t)dy * t->stride;
        int32_t sx16 = (int32_t)(((float)ax0 + 0.5f - left) * inv * 65536.0f);
        for (int dx = ax0; dx < ax1; dx++, sx16 += step) {
            int sx = sx16 >> 16;
            if (sx < rw->x0 || sx >= rw->x0 + rw->len) continue;
            int k = k0 + sx;
            int a = s->A[k];
            if (!a) continue;
            int q = s->Z ? (qb - s->Z[k] * qs256) >> 8 : q0;
            if (q <= pz[dx]) continue;
            uint16_t c = tq_spr_px(s, lut, k);
            int aa = (a * alpha) >> 8;
            if (aa >= 250) pc[dx] = c;
            else pc[dx] = tq_blend(pc[dx], c, (aa + 4) >> 3);
            if (a >= 160) pz[dx] = (uint16_t)(q > TQ_QMAX ? TQ_QMAX : q);
        }
    }
}

/* --------------------------------------------------------------------------
 * Puffs, shadows, marks
 * -------------------------------------------------------------------------- */

void tq_ball(const tq_tgt_t *t, float x, float y, float r, uint32_t rgb, int alpha, int q,
             const uint16_t *zt, int zstride)
{
    if (r < 0.5f || alpha <= 0) return;
    int x0 = maxi((int)floorf(x - r), t->cx0), x1 = mini((int)ceilf(x + r) + 1, t->cx1);
    int y0 = maxi((int)floorf(y - r), t->cy0), y1 = mini((int)ceilf(y + r) + 1, t->cy1);
    float cr = ((rgb >> 16) & 255), cg = ((rgb >> 8) & 255), cb = (rgb & 255);
    float ir = 1.0f / r;
    for (int py = y0; py < y1; py++) {
        float dy = ((float)py + 0.5f - y) * ir;
        uint16_t *pc = t->px + (size_t)py * t->stride;
        const uint16_t *pz = zt ? zt + (size_t)(py >> s_zsh) * zstride : NULL;
        for (int px = x0; px < x1; px++) {
            float dx = ((float)px + 0.5f - x) * ir;
            float d2 = dx * dx + dy * dy;
            if (d2 >= 1) continue;
            if (pz && q <= pz[px >> s_zsh]) continue;
            float nz = sqrtf(1 - d2);
            float sh = 0.50f + 0.62f * tq_clampf(-0.42f * dx - 0.52f * dy + 0.74f * nz, 0, 1);
            float edge = tq_clampf((1 - sqrtf(d2)) * r, 0, 1);
            uint32_t R = (uint32_t)tq_clampf(cr * sh, 0, 255), G = (uint32_t)tq_clampf(cg * sh, 0, 255),
                     B = (uint32_t)tq_clampf(cb * sh, 0, 255);
            int a = (int)(edge * alpha) >> 3;
            if (a >= 32) pc[px] = tq_565((R << 16) | (G << 8) | B);
            else if (a > 0) pc[px] = tq_blend(pc[px], tq_565((R << 16) | (G << 8) | B), a);
        }
    }
}

void tq_shadow(const tq_tgt_t *t, float x, float y, float rx, float ry, int strength)
{
    int x0 = maxi((int)floorf(x - rx), t->cx0), x1 = mini((int)ceilf(x + rx) + 1, t->cx1);
    int y0 = maxi((int)floorf(y - ry), t->cy0), y1 = mini((int)ceilf(y + ry) + 1, t->cy1);
    float irx = 1.0f / rx, iry = 1.0f / ry;
    for (int py = y0; py < y1; py++) {
        float dy = ((float)py + 0.5f - y) * iry;
        uint16_t *pc = t->px + (size_t)py * t->stride;
        for (int px = x0; px < x1; px++) {
            float dx = ((float)px + 0.5f - x) * irx;
            float d2 = dx * dx + dy * dy;
            if (d2 >= 1) continue;
            float k = (1 - d2);
            int a = (int)(k * k * (float)strength) >> 3;
            if (a > 0) pc[px] = tq_blend(pc[px], 0x0841, a > 32 ? 32 : a);
        }
    }
}

void tq_disc(const tq_tgt_t *t, float x, float y, float r, uint32_t rgb, int alpha)
{
    int x0 = maxi((int)floorf(x - r), t->cx0), x1 = mini((int)ceilf(x + r) + 1, t->cx1);
    int y0 = maxi((int)floorf(y - r), t->cy0), y1 = mini((int)ceilf(y + r) + 1, t->cy1);
    uint16_t c = tq_565(rgb);
    for (int py = y0; py < y1; py++) {
        float dy = (float)py + 0.5f - y;
        uint16_t *pc = t->px + (size_t)py * t->stride;
        for (int px = x0; px < x1; px++) {
            float dx = (float)px + 0.5f - x;
            float e = tq_clampf(r - sqrtf(dx * dx + dy * dy) + 0.5f, 0, 1);
            int a = (int)(e * alpha) >> 3;
            if (a >= 32) pc[px] = c;
            else if (a > 0) pc[px] = tq_blend(pc[px], c, a);
        }
    }
}

void tq_ring(const tq_tgt_t *t, float x, float y, float rx, float ry, float width, uint32_t rgb, int alpha)
{
    float ex = rx + width, ey = ry + width;
    int x0 = maxi((int)floorf(x - ex), t->cx0), x1 = mini((int)ceilf(x + ex) + 1, t->cx1);
    int y0 = maxi((int)floorf(y - ey), t->cy0), y1 = mini((int)ceilf(y + ey) + 1, t->cy1);
    uint16_t c = tq_565(rgb);
    float k = ry / rx;
    for (int py = y0; py < y1; py++) {
        float dy = ((float)py + 0.5f - y) / k;
        uint16_t *pc = t->px + (size_t)py * t->stride;
        for (int px = x0; px < x1; px++) {
            float dx = (float)px + 0.5f - x;
            float d = fabsf(sqrtf(dx * dx + dy * dy) - rx);
            float e = tq_clampf(width * 0.5f - d * (k < 1 ? k : 1) + 0.5f, 0, 1);
            int a = (int)(e * alpha) >> 3;
            if (a >= 32) pc[px] = c;
            else if (a > 0) pc[px] = tq_blend(pc[px], c, a);
        }
    }
}

static void tint_tri(const tq_tgt_t *t, const float *p0, const float *p1, const float *p2,
                     float q0, float q1, float q2, uint16_t c, int a32, const uint16_t *zt, int zstride, int bias)
{
    float area = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
    if (fabsf(area) < 1e-3f) return;
    float inv = 1.0f / area;
    float e1x = p1[0] - p0[0], e1y = p1[1] - p0[1], e2x = p2[0] - p0[0], e2y = p2[1] - p0[1];
    float gdx = ((q1 - q0) * e2y - (q2 - q0) * e1y) * inv, gdy = ((q2 - q0) * e1x - (q1 - q0) * e2x) * inv;
    const float *v[3] = { p0, p1, p2 };
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2 - i; j++)
            if (v[j][1] > v[j + 1][1]) { const float *s = v[j]; v[j] = v[j + 1]; v[j + 1] = s; }
    int ys = maxi(px_start(v[0][1]), t->cy0), ye = mini(px_start(v[2][1]), t->cy1);
    float d02 = v[2][1] - v[0][1];
    if (d02 < 1e-5f) return;
    for (int y = ys; y < ye; y++) {
        float py = (float)y + 0.5f;
        float xa = v[0][0] + (v[2][0] - v[0][0]) * (py - v[0][1]) / d02;
        float xb;
        if (py < v[1][1]) {
            float d = v[1][1] - v[0][1];
            xb = d > 1e-5f ? v[0][0] + (v[1][0] - v[0][0]) * (py - v[0][1]) / d : v[1][0];
        } else {
            float d = v[2][1] - v[1][1];
            xb = d > 1e-5f ? v[1][0] + (v[2][0] - v[1][0]) * (py - v[1][1]) / d : v[1][0];
        }
        if (xa > xb) { float s = xa; xa = xb; xb = s; }
        int xs = maxi(px_start(xa), t->cx0), xe = mini(px_start(xb), t->cx1);
        uint16_t *pc = t->px + (size_t)y * t->stride;
        const uint16_t *pz = zt + (size_t)(y >> s_zsh) * zstride;
        float q = q0 + gdx * ((float)xs + 0.5f - p0[0]) + gdy * (py - p0[1]);
        for (int x = xs; x < xe; x++, q += gdx) {
            if ((int)q + bias < pz[x >> s_zsh]) continue;
            pc[x] = tq_blend(pc[x], c, a32);
        }
    }
}

void tq_tint_quad(const tq_tgt_t *t, const float xy[8], const float q[4], uint32_t rgb, int alpha,
                  const uint16_t *zt, int zstride, int bias)
{
    uint16_t c = tq_565(rgb);
    int a32 = alpha >> 3;
    tint_tri(t, &xy[0], &xy[2], &xy[4], q[0], q[1], q[2], c, a32, zt, zstride, bias);
    tint_tri(t, &xy[0], &xy[4], &xy[6], q[0], q[2], q[3], c, a32, zt, zstride, bias);
}

void tq_rect(const tq_tgt_t *t, int x, int y, int w, int h, uint32_t rgb, int alpha)
{
    int x0 = maxi(x, t->cx0), x1 = mini(x + w, t->cx1), y0 = maxi(y, t->cy0), y1 = mini(y + h, t->cy1);
    uint16_t c = tq_565(rgb);
    int a = alpha >> 3;
    for (int py = y0; py < y1; py++) {
        uint16_t *pc = t->px + (size_t)py * t->stride;
        for (int px = x0; px < x1; px++) pc[px] = a >= 32 ? c : tq_blend(pc[px], c, a);
    }
}

/* --------------------------------------------------------------------------
 * Doubling
 * -------------------------------------------------------------------------- */

static inline uint32_t spread(uint16_t c)
{
    return (c | ((uint32_t)c << 16)) & 0x07E0F81Fu;
}

void tq_up2_rect(uint16_t *dst, int dstride, const uint16_t *src, int sw, int sh, int sstride, int sx0, int sy0,
                 int X0, int X1, int y0, int y1)
{
    X0 &= ~1;
    for (int Y = y0; Y < y1; Y++) {
        /* output row Y samples source rows sa (weight 3) and sb (weight 1) */
        int sa = sy0 + (Y >> 1), sb = (Y & 1) ? sa + 1 : sa - 1;
        if (sa >= sh) sa = sh - 1;
        if (sb < 0) sb = 0;
        if (sb >= sh) sb = sh - 1;
        const uint16_t *ra = src + (size_t)sa * sstride, *rb = src + (size_t)sb * sstride;
        uint16_t *o = dst + (size_t)Y * dstride;
        int c0 = sx0 + X0 / 2;
        int cp = c0 > 0 ? c0 - 1 : c0;
        uint32_t pa = spread(ra[cp]) * 3 + spread(rb[cp]);
        for (int X = X0; X < X1; X += 2) {
            int c = sx0 + (X >> 1);
            if (c >= sw) c = sw - 1;
            int cn = c + 1 < sw ? c + 1 : c;
            uint32_t ca = spread(ra[c]) * 3 + spread(rb[c]);
            uint32_t nn = spread(ra[cn]) * 3 + spread(rb[cn]);
            uint32_t l = ((ca * 3 + pa) >> 4) & 0x07E0F81Fu;
            uint32_t r = ((ca * 3 + nn) >> 4) & 0x07E0F81Fu;
            o[X] = (uint16_t)(l | (l >> 16));
            if (X + 1 < X1) o[X + 1] = (uint16_t)(r | (r >> 16));
            pa = ca;
        }
    }
}

void tq_up2_window(uint16_t *dst, int dstride, int W, const uint16_t *src, int sw, int sh, int sstride, int sx0,
                   int sy0, int y0, int y1)
{
    tq_up2_rect(dst, dstride, src, sw, sh, sstride, sx0, sy0, 0, W, y0, y1);
}

void tq_up2(uint16_t *dst, int dstride, const uint16_t *src, int w, int h, int y0, int y1)
{
    tq_up2_window(dst, dstride, 2 * w, src, w, h, w, 0, 0, y0, y1);
}
