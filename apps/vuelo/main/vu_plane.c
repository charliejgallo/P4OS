/*
 * PLANEO - the plane (see vu_plane.h)
 *
 * The model, in metres, x forward, y right, z up: a round fuselage of
 * eight-sided rings with a spinner, a bubble canopy, low wings with a
 * little dihedral and light tips, the tailplane and the fin. Each
 * triangle keeps its colour slot (body, wings, trim, glass, dark) and its
 * outward normal; a frame turns them, lights them by the world's sun,
 * drops the ones facing away and sorts the rest far to near.
 */
#include "vu_plane.h"

#include <math.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#define MAXV 140
#define MAXT 200

enum { C_BODY = 0, C_WING, C_TRIM, C_GLASS, C_DARK, C_SPIN };

typedef struct {
    uint8_t a, b, c, col;
    float n[3];
} mtri_t;

static float  s_v[MAXV][3];
static int    s_nv;
static mtri_t s_t[MAXT];
static int    s_nt;
static bool   s_built;

static int vtx(float x, float y, float z)
{
    if (s_nv >= MAXV) return MAXV - 1;
    s_v[s_nv][0] = x;
    s_v[s_nv][1] = y;
    s_v[s_nv][2] = z;
    return s_nv++;
}

/* A triangle facing along 'out' (its winding fixed to match) */
static void tri(int a, int b, int c, int col, float ox, float oy, float oz)
{
    if (s_nt >= MAXT) return;
    float e1[3], e2[3], n[3];
    for (int i = 0; i < 3; i++) {
        e1[i] = s_v[b][i] - s_v[a][i];
        e2[i] = s_v[c][i] - s_v[a][i];
    }
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l < 1e-6f) return;
    if (n[0] * ox + n[1] * oy + n[2] * oz < 0) {
        int t = b;
        b = c;
        c = t;
        n[0] = -n[0];
        n[1] = -n[1];
        n[2] = -n[2];
    }
    mtri_t *t = &s_t[s_nt++];
    t->a = (uint8_t)a;
    t->b = (uint8_t)b;
    t->c = (uint8_t)c;
    t->col = (uint8_t)col;
    for (int i = 0; i < 3; i++) t->n[i] = n[i] / l;
}

static void quad(int a, int b, int c, int d, int col, float ox, float oy, float oz)
{
    tri(a, b, c, col, ox, oy, oz);
    tri(a, c, d, col, ox, oy, oz);
}

/* A thin slab from a root edge to a tip edge: the wings and the tailplane.
 * le/te: leading and trailing x; y: the span; z: the chord line's height;
 * t: half the thickness */
static void slab(float le0, float te0, float y0, float z0, float le1, float te1, float y1, float z1, float t, int col)
{
    int a = vtx(le0, y0, z0 + t), b = vtx(te0, y0, z0 + t * 0.3f), c = vtx(te1, y1, z1 + t * 0.3f),
        d = vtx(le1, y1, z1 + t);
    int e = vtx(le0, y0, z0 - t * 0.6f), f = vtx(te0, y0, z0 - t * 0.2f), g = vtx(te1, y1, z1 - t * 0.2f),
        h = vtx(le1, y1, z1 - t * 0.6f);
    float side = y1 > y0 ? 1.0f : -1.0f;
    quad(a, b, c, d, col, 0, 0, 1);         /* top */
    quad(e, f, g, h, col, 0, 0, -1);        /* bottom */
    quad(a, d, h, e, col, 1, 0, 0);         /* the leading edge */
    quad(b, c, g, f, col, -1, 0, 0);        /* the trailing edge */
    quad(d, c, g, h, col, 0, side, 0);      /* the tip */
}

void vp_build(void)
{
    if (s_built) return;
    s_nv = s_nt = 0;
    /* the fuselage: stations x, half width, half height, centre height */
    static const float ST[][4] = {
        { 2.82f, 0.30f, 0.30f, 0.00f }, /* the spinner's base */
        { 2.66f, 0.56f, 0.58f, 0.00f }, /* the cowl */
        { 1.55f, 0.68f, 0.72f, 0.05f },
        { 0.15f, 0.64f, 0.72f, 0.08f }, /* the cockpit */
        { -1.40f, 0.44f, 0.52f, 0.16f },
        { -3.30f, 0.13f, 0.17f, 0.34f }, /* the tail */
    };
    static const uint8_t SEG_COL[] = { C_TRIM, C_TRIM, C_BODY, C_BODY, C_BODY };
    enum { NS = sizeof ST / sizeof ST[0], RING = 8 };
    int ring[NS][RING];
    for (int s = 0; s < NS; s++) {
        for (int k = 0; k < RING; k++) {
            float a = (float)k * 6.2831853f / RING + 0.3927f;
            ring[s][k] = vtx(ST[s][0], ST[s][1] * cosf(a), ST[s][3] + ST[s][2] * sinf(a));
        }
    }
    for (int s = 0; s + 1 < NS; s++) {
        for (int k = 0; k < RING; k++) {
            int k2 = (k + 1) % RING;
            float a = ((float)k + 0.5f) * 6.2831853f / RING + 0.3927f;
            int col = SEG_COL[s];
            /* a trim stripe along the sides of the body */
            if (col == C_BODY && (k == 0 || k == 3) && s >= 2) col = C_TRIM;
            quad(ring[s][k], ring[s][k2], ring[s + 1][k2], ring[s + 1][k], col, 0, cosf(a), sinf(a));
        }
    }
    /* the spinner's cone and the tail's end */
    int tip = vtx(3.20f, 0, 0);
    for (int k = 0; k < RING; k++) {
        float a = ((float)k + 0.5f) * 6.2831853f / RING + 0.3927f;
        tri(ring[0][k], ring[0][(k + 1) % RING], tip, C_SPIN, 1.0f, cosf(a), sinf(a));
    }
    int tail = vtx(-3.52f, 0, 0.36f);
    for (int k = 0; k < RING; k++) {
        float a = ((float)k + 0.5f) * 6.2831853f / RING + 0.3927f;
        tri(ring[NS - 1][k], ring[NS - 1][(k + 1) % RING], tail, C_TRIM, -1.0f, cosf(a), sinf(a));
    }
    /* the canopy: a low bubble over the cockpit */
    {
        int f0 = vtx(1.00f, 0, 0.70f), top = vtx(0.05f, 0, 1.22f), back = vtx(-0.95f, 0, 0.76f);
        int l0 = vtx(0.35f, -0.44f, 0.80f), r0 = vtx(0.35f, 0.44f, 0.80f);
        int l1 = vtx(-0.45f, -0.40f, 0.82f), r1 = vtx(-0.45f, 0.40f, 0.82f);
        tri(f0, l0, top, C_GLASS, 0.6f, -0.4f, 0.7f);
        tri(f0, top, r0, C_GLASS, 0.6f, 0.4f, 0.7f);
        tri(l0, l1, top, C_GLASS, 0, -1, 0.6f);
        tri(r0, top, r1, C_GLASS, 0, 1, 0.6f);
        tri(l1, back, top, C_GLASS, -0.5f, -0.4f, 0.8f);
        tri(r1, top, back, C_GLASS, -0.5f, 0.4f, 0.8f);
    }
    /* the wings: red inside, light tips; a little dihedral */
    for (int side = -1; side <= 1; side += 2) {
        float sd = (float)side;
        slab(0.80f, -1.05f, 0.50f * sd, -0.36f, 0.55f, -0.80f, 3.15f * sd, -0.13f, 0.11f, C_WING);
        slab(0.55f, -0.80f, 3.15f * sd, -0.13f, 0.32f, -0.52f, 4.35f * sd, -0.03f, 0.08f, C_TRIM);
        /* the tailplane */
        slab(-2.70f, -3.40f, 0.12f * sd, 0.32f, -3.00f, -3.42f, 1.70f * sd, 0.34f, 0.05f, C_WING);
    }
    /* the fin */
    {
        float x0 = -2.55f, x1 = -3.50f, x2 = -3.15f, x3 = -3.58f, zb = 0.45f, zt = 1.62f, t = 0.05f;
        int a = vtx(x0, -t, zb), b = vtx(x1, -t, zb), c = vtx(x3, -t * 0.5f, zt), d = vtx(x2, -t * 0.5f, zt);
        int e = vtx(x0, t, zb), f = vtx(x1, t, zb), g = vtx(x3, t * 0.5f, zt), h = vtx(x2, t * 0.5f, zt);
        quad(a, b, c, d, C_WING, 0, -1, 0);
        quad(e, f, g, h, C_WING, 0, 1, 0);
        quad(a, d, h, e, C_WING, 1, 0, 0.3f);
        quad(d, c, g, h, C_TRIM, 0, 0, 1);
    }
    /* the wheels under the wings, as dark wedges */
    for (int side = -1; side <= 1; side += 2) {
        float y = 1.25f * (float)side;
        int a = vtx(0.55f, y, -0.40f), b = vtx(0.10f, y, -0.40f), c = vtx(0.38f, y, -1.05f);
        int d = vtx(0.55f, y + 0.12f * (float)side, -0.40f), e = vtx(0.38f, y + 0.12f * (float)side, -1.05f);
        tri(a, b, c, C_DARK, 0, -(float)side, 0);
        tri(a, c, b, C_DARK, 0, (float)side, 0);
        quad(a, d, e, c, C_DARK, 1, 0, 0);
    }
    s_built = true;
}

/* --------------------------------------------------------------------------
 * A frame
 * -------------------------------------------------------------------------- */

static inline uint16_t shade(uint32_t rgb, float k, uint32_t amb)
{
    float r = (float)((rgb >> 16) & 255) * k + (float)((amb >> 16) & 255) * 0.18f;
    float g = (float)((rgb >> 8) & 255) * k + (float)((amb >> 8) & 255) * 0.18f;
    float b = (float)(rgb & 255) * k + (float)(amb & 255) * 0.18f;
    int R = (int)r, G = (int)g, B = (int)b;
    if (R > 255) R = 255;
    if (G > 255) G = 255;
    if (B > 255) B = 255;
    return (uint16_t)(((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3));
}

static void add_tri(vp_draw_t *d, const float *x, const float *y, float depth, uint16_t col, int alpha)
{
    if (d->n >= VP_MAXT) return;
    vp_tri_t *t = &d->t[d->n++];
    for (int i = 0; i < 3; i++) {
        t->x[i] = x[i];
        t->y[i] = y[i];
    }
    t->depth = depth;
    t->col = col;
    t->alpha = (uint8_t)alpha;
}

void vp_prepare(vp_draw_t *d, const vp_pose_t *pose, const vr_proj_t *p, const float sun[3], uint32_t amb_rgb)
{
    vp_build();
    d->n = 0;
    d->disc = false;
    float sx[MAXV], sy[MAXV], sz[MAXV];
    bool ok[MAXV];
    float wv[MAXV][3];
    for (int i = 0; i < s_nv; i++) {
        const float *v = s_v[i];
        for (int k = 0; k < 3; k++) wv[i][k] = pose->p[k] + pose->f[k] * v[0] + pose->r[k] * v[1] + pose->u[k] * v[2];
        ok[i] = vr_project(p, wv[i][0], wv[i][1], wv[i][2], &sx[i], &sy[i], &sz[i]);
    }
    /* the shadow first: the plane's cross squashed onto the ground below it */
    float above = pose->p[2] - pose->ground;
    if (above < 140.0f && above > 0) {
        int dark = (int)(14.0f * (1.0f - above / 140.0f));
        float g = pose->ground + 0.3f;
        /* along the ground: the plane's forward and right flattened */
        float fx = pose->f[0], fy = pose->f[1], rx = pose->r[0], ry = pose->r[1];
        float spread = 1.0f + above * 0.004f;
        const float Q[2][4][2] = { { { 3.0f, 0.5f }, { -3.4f, 0.3f }, { -3.4f, -0.3f }, { 3.0f, -0.5f } },
                                   { { 0.7f, 4.3f }, { -1.0f, 4.3f }, { -1.0f, -4.3f }, { 0.7f, -4.3f } } };
        for (int q = 0; q < 2; q++) {
            float qx[4], qy[4];
            bool all = true;
            for (int k = 0; k < 4; k++) {
                float a = Q[q][k][0] * spread, b = Q[q][k][1] * spread;
                float wx = pose->p[0] + fx * a + rx * b, wy = pose->p[1] + fy * a + ry * b;
                all &= vr_project(p, wx, wy, g, &qx[k], &qy[k], NULL);
            }
            if (!all) continue;
            float x1[3] = { qx[0], qx[1], qx[2] }, y1[3] = { qy[0], qy[1], qy[2] };
            float x2[3] = { qx[0], qx[2], qx[3] }, y2[3] = { qy[0], qy[2], qy[3] };
            add_tri(d, x1, y1, 1e9f, 0, dark);
            add_tri(d, x2, y2, 1e9f, 0, dark);
        }
    }
    int first = d->n;
    float eye[3] = { p->cx, p->cy, p->cz };
    for (int i = 0; i < s_nt; i++) {
        const mtri_t *t = &s_t[i];
        if (!ok[t->a] || !ok[t->b] || !ok[t->c]) continue;
        float n[3];
        for (int k = 0; k < 3; k++) n[k] = pose->f[k] * t->n[0] + pose->r[k] * t->n[1] + pose->u[k] * t->n[2];
        /* facing away from the eye: hidden */
        float to[3];
        for (int k = 0; k < 3; k++) to[k] = wv[t->a][k] - eye[k];
        if (to[0] > VW_SIZE * 0.5f) to[0] -= VW_SIZE;
        if (to[0] < -VW_SIZE * 0.5f) to[0] += VW_SIZE;
        if (to[1] > VW_SIZE * 0.5f) to[1] -= VW_SIZE;
        if (to[1] < -VW_SIZE * 0.5f) to[1] += VW_SIZE;
        if (n[0] * to[0] + n[1] * to[1] + n[2] * to[2] >= 0) continue;
        float dif = n[0] * sun[0] + n[1] * sun[1] + n[2] * sun[2];
        if (dif < 0) dif = 0;
        float k = 0.42f + 0.62f * dif;
        uint32_t rgb;
        switch (t->col) {
        case C_BODY: rgb = pose->paint[0]; break;
        case C_WING: rgb = pose->paint[1]; break;
        case C_TRIM: rgb = pose->paint[2]; break;
        case C_GLASS:
            rgb = 0x2A3E58;
            k += 0.5f * powf(dif, 6.0f);    /* a glint */
            break;
        case C_SPIN: rgb = 0xD8D8D0; break;
        default: rgb = 0x2A2A2E; break;
        }
        float x[3] = { sx[t->a], sx[t->b], sx[t->c] }, y[3] = { sy[t->a], sy[t->b], sy[t->c] };
        float dep = (sz[t->a] + sz[t->b] + sz[t->c]) * (1.0f / 3.0f);
        add_tri(d, x, y, dep, shade(rgb, k, amb_rgb), 32);
    }
    /* far first (insertion sort: a hundred triangles, nearly in order frame to frame) */
    for (int i = first + 1; i < d->n; i++) {
        vp_tri_t t = d->t[i];
        int j = i - 1;
        while (j >= first && d->t[j].depth < t.depth) {
            d->t[j + 1] = d->t[j];
            j--;
        }
        d->t[j + 1] = t;
    }
    /* the propeller's disc, in front of the spinner */
    if (pose->prop > 0.05f) {
        float c[3], a[3], b[3];
        for (int k = 0; k < 3; k++) {
            c[k] = pose->p[k] + pose->f[k] * 3.05f;
            a[k] = c[k] + pose->r[k] * 1.25f;
            b[k] = c[k] + pose->u[k] * 1.25f;
        }
        float cx, cy, ax, ay, bx, by;
        if (vr_project(p, c[0], c[1], c[2], &cx, &cy, NULL) && vr_project(p, a[0], a[1], a[2], &ax, &ay, NULL) &&
            vr_project(p, b[0], b[1], b[2], &bx, &by, NULL)) {
            d->disc = true;
            d->dcx = cx;
            d->dcy = cy;
            d->dax = ax - cx;
            d->day = ay - cy;
            d->dbx = bx - cx;
            d->dby = by - cy;
        }
    }
}

void vp_fill(uint16_t *out, int W, int y0, int y1, const float *xs, const float *ys, uint16_t col, int alpha)
{
    float ax = xs[0], ay = ys[0], bx = xs[1], by = ys[1], cx = xs[2], cy = ys[2], t;
    if (ay > by) {
        t = ax, ax = bx, bx = t;
        t = ay, ay = by, by = t;
    }
    if (by > cy) {
        t = bx, bx = cx, cx = t;
        t = by, by = cy, cy = t;
    }
    if (ay > by) {
        t = ax, ax = bx, bx = t;
        t = ay, ay = by, by = t;
    }
    if (cy - ay < 1e-4f) return;
    int ys0 = (int)ceilf(ay - 0.5f), ys1 = (int)ceilf(cy - 0.5f);
    if (ys0 < y0) ys0 = y0;
    if (ys1 > y1) ys1 = y1;
    float il = 1.0f / (cy - ay);
    float iu = by - ay > 1e-4f ? 1.0f / (by - ay) : 0, id = cy - by > 1e-4f ? 1.0f / (cy - by) : 0;
    for (int y = ys0; y < ys1; y++) {
        float py = (float)y + 0.5f;
        float xl = ax + (cx - ax) * (py - ay) * il;
        float xr = py < by ? ax + (bx - ax) * (py - ay) * iu : bx + (cx - bx) * (py - by) * id;
        if (xl > xr) t = xl, xl = xr, xr = t;
        int xa = (int)ceilf(xl - 0.5f), xb = (int)ceilf(xr - 0.5f);
        if (xa < 0) xa = 0;
        if (xb > W) xb = W;
        uint16_t *o = out + (size_t)y * W;
        if (alpha >= 32)
            for (int x = xa; x < xb; x++) o[x] = col;
        else
            for (int x = xa; x < xb; x++) o[x] = vr_blend(o[x], col, alpha);
    }
}

void vp_draw(const vp_draw_t *d, uint16_t *out, int W, int H, int y0, int y1)
{
    if (y1 > H) y1 = H;
    for (int i = 0; i < d->n; i++) {
        const vp_tri_t *t = &d->t[i];
        vp_fill(out, W, y0, y1, t->x, t->y, t->col, t->alpha);
    }
    if (d->disc) {
        /* the blur: an ellipse from its two half axes, a light veil with a darker rim */
        float det = d->dax * d->dby - d->day * d->dbx;
        if (fabsf(det) < 0.5f) return;
        float id = 1.0f / det;
        float ext = sqrtf(d->dax * d->dax + d->dbx * d->dbx) + 1, eyt = sqrtf(d->day * d->day + d->dby * d->dby) + 1;
        int ya = (int)(d->dcy - eyt), yb = (int)(d->dcy + eyt) + 1;
        int xa = (int)(d->dcx - ext), xb = (int)(d->dcx + ext) + 1;
        if (ya < y0) ya = y0;
        if (yb > y1) yb = y1;
        if (xa < 0) xa = 0;
        if (xb > W) xb = W;
        for (int y = ya; y < yb; y++) {
            uint16_t *o = out + (size_t)y * W;
            float py = (float)y + 0.5f - d->dcy;
            for (int x = xa; x < xb; x++) {
                float px = (float)x + 0.5f - d->dcx;
                /* the point in the disc's own axes */
                float a = (px * d->dby - py * d->dbx) * id, b = (py * d->dax - px * d->day) * id;
                float r2 = a * a + b * b;
                if (r2 > 1) continue;
                int al = r2 > 0.82f ? 9 : 5;
                o[x] = vr_blend(o[x], r2 > 0.82f ? 0x3186 : 0xC618, al);
            }
        }
    }
}
