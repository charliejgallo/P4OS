/*
 * VISOR 3D - the rasteriser (see v3_raster.h)
 *
 * The plainest thing that looks right: every vertex transformed once per
 * frame, every triangle filled scanline by scanline with its depth
 * interpolated along the span and tested against a 16-bit z-buffer, one
 * colour per triangle (flat shading, which is also what makes a low-poly
 * model read as faceted rather than broken).
 *
 * Both sides of a face are lit (|n.z|): STL files come with windings in
 * every state, and a model that vanishes because its triangles face "the
 * wrong way" is worse than paying to fill the back faces the z-buffer then
 * hides. The light is a headlight (from the camera) plus a little from
 * above the screen, so the shape reads from any angle without a light to
 * aim.
 *
 * P4OS: the same code as the watch's, in floats (the P4 has a
 * single-precision FPU; a double here would be soft-float), writing LVGL's
 * byte order, with the twist (roll) after the two turns, clearing only
 * what the previous frame drew, and split across the two cores.
 */
#include "v3_raster.h"
#include "aos_hal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#define CAM_D       3.0f                /* camera distance, in model radii */
#define FILL        0.72f               /* of half the smaller side, at scale 1 */
#define BASE_RGB    0xA8B8D0            /* the model when the file has no colour */

/* ceil(v - 0.5) = the first pixel centre at or after v, without a call into
 * libm (ceilf was a function on the watch's core, and it ran twice per
 * scanline of every one of 12 000 triangles). Valid for |v| < 16384. */
static inline int px_start(float v)
{
    float x = v - 0.5f;
    int i = (int)x;                     /* truncates: ceil for x < 0 */
    return i + (x > (float)i);
}

static inline uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

bool v3_scratch_alloc(v3_scratch_t *s, int nv)
{
    s->xyz = malloc((size_t)nv * 3 * sizeof(float));
    s->nv = nv;
    if (!s->xyz) {
        v3_scratch_free(s);
        return false;
    }
    return true;
}

void v3_scratch_free(v3_scratch_t *s)
{
    free(s->xyz);
    memset(s, 0, sizeof *s);
}

/* One triangle, filled. x, y in pixels; z in 0..65535 (0 = nearest). */
static void fill_tri(uint16_t *fb, uint16_t *zb, int w, int h,
                     float x0, float y0, float z0, float x1, float y1, float z1,
                     float x2, float y2, float z2, uint16_t c)
{
    /* sort by y: 0 top */
    if (y1 < y0) { float t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; t = z0; z0 = z1; z1 = t; }
    if (y2 < y0) { float t; t = x0; x0 = x2; x2 = t; t = y0; y0 = y2; y2 = t; t = z0; z0 = z2; z2 = t; }
    if (y2 < y1) { float t; t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; t = z1; z1 = z2; z2 = t; }
    if (y2 - y0 < 1e-4f) return;

    int ys = px_start(y0), ye = px_start(y2);
    if (ys < 0) ys = 0;
    if (ye > h) ye = h;
    float inv02 = 1.0f / (y2 - y0);
    float inv01 = (y1 - y0) > 1e-4f ? 1.0f / (y1 - y0) : 0;
    float inv12 = (y2 - y1) > 1e-4f ? 1.0f / (y2 - y1) : 0;

    for (int y = ys; y < ye; y++) {
        float py = (float)y + 0.5f;
        float t = (py - y0) * inv02;
        float xa = x0 + (x2 - x0) * t, za = z0 + (z2 - z0) * t;
        float xb, zb2;
        if (py < y1) {
            float u = (py - y0) * inv01;
            xb = x0 + (x1 - x0) * u;
            zb2 = z0 + (z1 - z0) * u;
        } else {
            float u = (py - y1) * inv12;
            xb = x1 + (x2 - x1) * u;
            zb2 = z1 + (z2 - z1) * u;
        }
        if (xa > xb) { float q = xa; xa = xb; xb = q; q = za; za = zb2; zb2 = q; }
        int xs = px_start(xa), xe = px_start(xb);
        if (xs < 0) xs = 0;
        if (xe > w) xe = w;
        if (xs >= xe) continue;
        float dz = (xb - xa) > 1e-4f ? (zb2 - za) / (xb - xa) : 0;
        float z = za + ((float)xs + 0.5f - xa) * dz;
        uint16_t *pc = fb + (size_t)y * w;
        uint16_t *pz = zb + (size_t)y * w;
        for (int x = xs; x < xe; x++, z += dz) {
            int zi = (int)z;
            if (zi < 0) zi = 0;
            if (zi < pz[x]) {
                pz[x] = (uint16_t)zi;
                pc[x] = c;
            }
        }
    }
}

static void line(uint16_t *fb, int w, int h, int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 8192; guard++) {
        if ((unsigned)x0 < (unsigned)w && (unsigned)y0 < (unsigned)h) fb[y0 * w + x0] = c;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* A box, clamped to the target: what clear_box() may touch. */
static void box_clamp(v3_box_t *b, int w, int h)
{
    if (b->x0 < 0) b->x0 = 0;
    if (b->y0 < 0) b->y0 = 0;
    if (b->x1 > w) b->x1 = w;
    if (b->y1 > h) b->y1 = h;
}

static void clear_box(uint16_t *buf, int w, int h, v3_box_t b, uint16_t v)
{
    box_clamp(&b, w, h);
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return;
    size_t n = (size_t)(b.x1 - b.x0);
    if (b.x0 == 0 && b.x1 == w) {       /* whole rows: one run */
        n *= (size_t)(b.y1 - b.y0);
        b.y1 = b.y0 + 1;
    }
    for (int y = b.y0; y < b.y1; y++) {
        uint16_t *p = buf + (size_t)y * w + b.x0;
        if ((v >> 8) == (v & 255)) {
            memset(p, v & 255, n * 2);
            continue;
        }
        size_t i = 0;
        if (((uintptr_t)p & 2) && n) p[i++] = v;
        uint32_t v2 = (uint32_t)v | ((uint32_t)v << 16);
        uint32_t *q = (uint32_t *)(p + i);
        for (; i + 1 < n; i += 2) *q++ = v2;
        if (i < n) p[i] = v;
    }
}

/* Where a frame's time goes, in microseconds, summed until v3_prof()
 * collects it: clearing, transforming, rasterising. uptime_us is there on
 * the board and in the simulator alike, so the two can be compared. */
static uint32_t s_prof[3];

void v3_prof(uint32_t out[3])
{
    memcpy(out, s_prof, sizeof s_prof);
    memset(s_prof, 0, sizeof s_prof);
}

/* One frame's work, shared by the two halves (aos_hal_worker_split): the
 * vertices are cut in two ranges, the triangles in interleaved runs of
 * CHUNK so both halves get a like share of the big ones on screen. The two
 * write the same fb and zb; where two triangles of the two halves cover the
 * same pixel at the same instant, the depth test can let the wrong one win
 * for that pixel and that frame - on a model of 100 000 triangles, never
 * seen. */
#define CHUNK 256

typedef struct {
    const v3_mesh_t *m;
    v3_scratch_t    *s;
    uint16_t        *fb, *zb;
    int              w, h;
    bool             solid, cull;
    float            cyw, syw, cp, sp, cr, sr, f, cx, cy;
    bool             tris;              /* false: the vertices; true: the triangles */
    int              drawn[2];
    float            box[2][4];
} job_t;

/* Measured on the board with 51 590 vertices: 17 ms on one core and 16 ms
 * for each half on two - this pass is bound by PSRAM, not by the cores, and
 * splitting it only keeps it from costing more. */
static void do_vertices(job_t *j, int part, int parts)
{
    const v3_mesh_t *m = j->m;
    int i0 = (int)((int64_t)m->nv * part / parts), i1 = (int)((int64_t)m->nv * (part + 1) / parts);
    /* the job in registers: a store through o may alias it, and the job
     * lives on the other core's stack */
    const float cyw = j->cyw, syw = j->syw, cp = j->cp, sp = j->sp, cr = j->cr, sr = j->sr;
    const float f = j->f, cx = j->cx, cy = j->cy;
    const float *v = &m->v[i0 * 3];
    float *o = &j->s->xyz[i0 * 3];
    /* vertices: yaw about Y, pitch about X, then the twist in the screen's
     * plane; then the projection */
    for (int i = i0; i < i1; i++, v += 3, o += 3) {
        float x1 = cyw * v[0] + syw * v[2];
        float z1 = -syw * v[0] + cyw * v[2];
        float y2 = cp * v[1] - sp * z1;
        float z2 = sp * v[1] + cp * z1;
        float x3 = cr * x1 - sr * y2;
        float y3 = sr * x1 + cr * y2;
        float d = CAM_D - z2;               /* distance from the camera */
        float k = f / d;
        o[0] = cx + x3 * k;
        o[1] = cy - y3 * k;
        o[2] = (d - (CAM_D - 1.0f)) * (65535.0f / 2.0f);
    }
}

static void do_triangles(job_t *j, int part, int parts)
{
    const v3_mesh_t *m = j->m;
    const float *xyz = j->s->xyz;
    uint16_t *fb = j->fb, *zb = j->zb;
    int w = j->w, h = j->h;
    bool solid = j->solid, cull = j->cull;
    float cyw = j->cyw, syw = j->syw, cp = j->cp, sp = j->sp, cr = j->cr, sr = j->sr;
    int drawn = 0;
    uint32_t base = BASE_RGB;
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;   /* what was drawn */
    for (int c0 = part * CHUNK; c0 < m->nt; c0 += parts * CHUNK) {
        int c1 = c0 + CHUNK < m->nt ? c0 + CHUNK : m->nt;
        for (int t = c0; t < c1; t++) {
            uint32_t a = m->idx[t * 3], b = m->idx[t * 3 + 1], d = m->idx[t * 3 + 2];
            const float *pa = &xyz[a * 3], *pb = &xyz[b * 3], *pd = &xyz[d * 3];
            float ax = pa[0], ay = pa[1], bx = pb[0], by = pb[1];
            float dx = pd[0], dy = pd[1];
            /* off screen entirely: skip */
            float minx = ax < bx ? ax : bx, maxx = ax > bx ? ax : bx;
            if (dx < minx) minx = dx;
            if (dx > maxx) maxx = dx;
            if (maxx < 0 || minx >= w) continue;
            float miny = ay < by ? ay : by, maxy = ay > by ? ay : by;
            if (dy < miny) miny = dy;
            if (dy > maxy) maxy = dy;
            if (maxy < 0 || miny >= h) continue;
            /* A solid's back faces. Wound outwards (counter-clockwise seen from
             * outside, y up), a front face turns CLOCKWISE on a screen whose y
             * grows down - a negative cross product. The twist turns every
             * triangle the same way, so it does not change the sign. */
            if (cull && (bx - ax) * (dy - ay) - (by - ay) * (dx - ax) >= 0) continue;

            /* the face normal, turned like the vertices */
            const float *nn = &m->fn[t * 3];
            float nx1 = cyw * nn[0] + syw * nn[2];
            float nz1 = -syw * nn[0] + cyw * nn[2];
            float ny2 = cp * nn[1] - sp * nz1;
            float nz2 = sp * nn[1] + cp * nz1;
            float ny3 = sr * nx1 + cr * ny2;    /* "above" is the screen's */
            float li = 0.20f + 0.70f * fabsf(nz2) + 0.15f * (ny3 > 0 ? ny3 : 0);
            if (li > 1.0f) li = 1.0f;

            uint32_t rgb = base;
            if (m->fc) {
                uint16_t c = m->fc[t];
                rgb = ((uint32_t)((c >> 11) & 31) << 19) | ((uint32_t)((c >> 5) & 63) << 10) |
                      ((uint32_t)(c & 31) << 3);
            }
            uint32_t r = (uint32_t)(((rgb >> 16) & 255) * li);
            uint32_t g = (uint32_t)(((rgb >> 8) & 255) * li);
            uint32_t bl = (uint32_t)((rgb & 255) * li);
            uint16_t c = rgb565(r, g, bl);

            if (!solid) {
                line(fb, w, h, (int)ax, (int)ay, (int)bx, (int)by, c);
                line(fb, w, h, (int)bx, (int)by, (int)dx, (int)dy, c);
                line(fb, w, h, (int)dx, (int)dy, (int)ax, (int)ay, c);
            } else {
                fill_tri(fb, zb, w, h, ax, ay, pa[2], bx, by, pb[2], dx, dy, pd[2], c);
            }
            if (minx < bx0) bx0 = minx;
            if (maxx > bx1) bx1 = maxx;
            if (miny < by0) by0 = miny;
            if (maxy > by1) by1 = maxy;
            drawn++;
        }
    }
    j->drawn[part] = drawn;
    j->box[part][0] = bx0; j->box[part][1] = by0; j->box[part][2] = bx1; j->box[part][3] = by1;
}

static void job_half(void *arg, int part)
{
    job_t *j = arg;
    if (j->tris) do_triangles(j, part, 2);
    else do_vertices(j, part, 2);
}

/* Both halves on the two cores, or both here when there is no second core
 * to have (the simulator). */
static void run(job_t *j)
{
    if (aos_hal_worker_split(job_half, j)) return;
    job_half(j, 0);
    job_half(j, 1);
}

int v3_render(const v3_mesh_t *m, const v3_view_t *view, v3_scratch_t *s,
              uint16_t *fb, uint16_t *zb, int w, int h,
              v3_box_t *fb_dirty, v3_box_t *zb_dirty)
{
    uint64_t t0 = aos_hal_uptime_us();
    bool solid = view->mode == V3_SOLID;
    uint16_t bg = rgb565((view->bg >> 16) & 255, (view->bg >> 8) & 255, view->bg & 255);
    clear_box(fb, w, h, *fb_dirty, bg);
    if (solid) clear_box(zb, w, h, *zb_dirty, 0xFFFF);
    uint64_t t1 = aos_hal_uptime_us();

    float half = (float)(w < h ? w : h) * 0.5f;
    job_t j = {
        .m = m, .s = s, .fb = fb, .zb = zb, .w = w, .h = h,
        .solid = solid, .cull = m->closed && solid,
        .cyw = cosf(view->yaw), .syw = sinf(view->yaw),
        .cp = cosf(view->pitch), .sp = sinf(view->pitch),
        .cr = cosf(view->roll), .sr = sinf(view->roll),
        .f = FILL * half * CAM_D * view->scale,
        .cx = w * 0.5f + view->px, .cy = h * 0.5f + view->py,
    };
    run(&j);

    uint64_t t2 = aos_hal_uptime_us();
    j.tris = true;
    run(&j);
    int drawn = j.drawn[0] + j.drawn[1];
    float bx0 = j.box[0][0] < j.box[1][0] ? j.box[0][0] : j.box[1][0];
    float by0 = j.box[0][1] < j.box[1][1] ? j.box[0][1] : j.box[1][1];
    float bx1 = j.box[0][2] > j.box[1][2] ? j.box[0][2] : j.box[1][2];
    float by1 = j.box[0][3] > j.box[1][3] ? j.box[0][3] : j.box[1][3];

    /* The box this frame drew, a pixel wider each way than the triangles'
     * (lines truncate, spans round): the next frame clears exactly that. */
    v3_box_t box = { 0, 0, 0, 0 };
    if (drawn) {
        box.x0 = (int)bx0 - 1;
        box.y0 = (int)by0 - 1;
        box.x1 = (int)bx1 + 2;
        box.y1 = (int)by1 + 2;
        box_clamp(&box, w, h);
    }
    *fb_dirty = box;
    if (solid) *zb_dirty = box;

    uint64_t t3 = aos_hal_uptime_us();
    s_prof[0] += (uint32_t)(t1 - t0);
    s_prof[1] += (uint32_t)(t2 - t1);
    s_prof[2] += (uint32_t)(t3 - t2);
    return drawn;
}
