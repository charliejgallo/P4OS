/*
 * DIBUJO - shapes. See dib_raster.h.
 */
#include "dib_raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI_F    3.14159265f
#define SUBROWS 4

/* --------------------------------------------------------------------------
 * Paths
 * -------------------------------------------------------------------------- */

void dib_path_init(dib_path_t *p)
{
    memset(p, 0, sizeof(*p));
}

void dib_path_free(dib_path_t *p)
{
    dib_free(p->pts);
    dib_free(p->start);
    memset(p, 0, sizeof(*p));
}

void dib_path_reset(dib_path_t *p)
{
    p->n = p->nc = 0;
    p->oom = false;
}

static bool grow(void **buf, int *cap, int need, size_t elem)
{
    if (need <= *cap) return true;
    int c = *cap ? *cap : 64;
    while (c < need) c *= 2;
    void *n = dib_alloc(elem * c);
    if (!n) return false;
    if (*buf) memcpy(n, *buf, elem * *cap);
    dib_free(*buf);
    *buf = n;
    *cap = c;
    return true;
}

void dib_path_move(dib_path_t *p, float x, float y)
{
    if (!grow((void **)&p->start, &p->ccap, p->nc + 1, sizeof(int))) {
        p->oom = true;
        return;
    }
    p->start[p->nc++] = p->n;
    dib_path_line(p, x, y);
}

void dib_path_line(dib_path_t *p, float x, float y)
{
    if (!p->nc || !grow((void **)&p->pts, &p->cap, p->n + 1, sizeof(dib_pt_t))) {
        p->oom = true;
        return;
    }
    p->pts[p->n++] = (dib_pt_t){ x, y };
}

/* The points of the last contour, reversed: the other winding. */
static void reverse_last(dib_path_t *p)
{
    if (!p->nc) return;
    int a = p->start[p->nc - 1], b = p->n - 1;
    while (a < b) {
        dib_pt_t t = p->pts[a]; p->pts[a] = p->pts[b]; p->pts[b] = t;
        a++; b--;
    }
}

/* Twice the signed area of the last contour; > 0 is clockwise on a screen
 * (y down). */
static float area_last(const dib_path_t *p)
{
    if (!p->nc) return 0;
    int a = p->start[p->nc - 1];
    float s = 0;
    for (int i = a; i < p->n; i++) {
        const dib_pt_t *u = &p->pts[i], *v = &p->pts[i + 1 < p->n ? i + 1 : a];
        s += u->x * v->y - v->x * u->y;
    }
    return s;
}

static void make_cw(dib_path_t *p)
{
    if (area_last(p) < 0) reverse_last(p);
}

static int arc_steps(float r)
{
    int n = (int)(r * 0.5f) + 12;
    return n > 160 ? 160 : n;
}

void dib_path_ellipse(dib_path_t *p, float cx, float cy, float rx, float ry, float ang, bool rev)
{
    rx = fabsf(rx);
    ry = fabsf(ry);
    int n = arc_steps(rx > ry ? rx : ry) * 2;
    float c = cosf(ang), s = sinf(ang);
    for (int i = 0; i < n; i++) {
        float t = 2.0f * PI_F * (float)i / (float)n;
        float lx = rx * cosf(t), ly = ry * sinf(t);
        float x = cx + lx * c - ly * s, y = cy + lx * s + ly * c;
        if (i == 0) dib_path_move(p, x, y);
        else dib_path_line(p, x, y);
    }
    make_cw(p);
    if (rev) reverse_last(p);
}

void dib_path_circle(dib_path_t *p, float cx, float cy, float r)
{
    dib_path_ellipse(p, cx, cy, r, r, 0, false);
}

void dib_path_rrect(dib_path_t *p, float cx, float cy, float w, float h, float r, float ang, bool rev)
{
    w = fabsf(w);
    h = fabsf(h);
    float m = (w < h ? w : h) * 0.5f;
    if (r > m) r = m;
    if (r < 0) r = 0;
    float c = cosf(ang), s = sinf(ang);
    /* the four corner centres, clockwise from the top left */
    const float ccx[4] = { -w / 2 + r, w / 2 - r, w / 2 - r, -w / 2 + r };
    const float ccy[4] = { -h / 2 + r, -h / 2 + r, h / 2 - r, h / 2 - r };
    int steps = r > 0.5f ? arc_steps(r) / 2 + 2 : 0;
    bool first = true;
    for (int k = 0; k < 4; k++) {
        float a0 = PI_F + (float)k * PI_F * 0.5f;
        for (int i = 0; i <= steps; i++) {
            float t = a0 + (steps ? (float)i / (float)steps : 0.0f) * PI_F * 0.5f;
            float lx = ccx[k] + r * cosf(t), ly = ccy[k] + r * sinf(t);
            float x = cx + lx * c - ly * s, y = cy + lx * s + ly * c;
            if (first) {
                dib_path_move(p, x, y);
                first = false;
            } else {
                dib_path_line(p, x, y);
            }
        }
    }
    make_cw(p);
    if (rev) reverse_last(p);
}

void dib_path_polyline(dib_path_t *p, const dib_pt_t *pts, int n, bool closed, float width)
{
    float hw = width * 0.5f;
    if (hw < 0.5f) hw = 0.5f;
    int segs = closed ? n : n - 1;
    for (int i = 0; i < segs; i++) {
        dib_pt_t a = pts[i], b = pts[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-3f) continue;
        float nx = -dy / len * hw, ny = dx / len * hw;
        dib_path_move(p, a.x + nx, a.y + ny);
        dib_path_line(p, b.x + nx, b.y + ny);
        dib_path_line(p, b.x - nx, b.y - ny);
        dib_path_line(p, a.x - nx, a.y - ny);
        make_cw(p);
    }
    for (int i = 0; i < n; i++) dib_path_circle(p, pts[i].x, pts[i].y, hw);
}

/* --------------------------------------------------------------------------
 * The rasterizer
 * -------------------------------------------------------------------------- */

typedef struct {
    float y0, y1, x0, dxdy;
    int   dir;
} edge_t;

typedef struct {
    float x;
    int   dir;
} cross_t;

static int edge_cmp(const void *a, const void *b)
{
    float d = ((const edge_t *)a)->y0 - ((const edge_t *)b)->y0;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

bool dib_raster_fill(const dib_path_t *p, dib_rect_t clip, dib_span_fn fn, void *user)
{
    if (p->oom || p->n < 3) return false;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < p->n; i++) {
        if (p->pts[i].x < minx) minx = p->pts[i].x;
        if (p->pts[i].x > maxx) maxx = p->pts[i].x;
        if (p->pts[i].y < miny) miny = p->pts[i].y;
        if (p->pts[i].y > maxy) maxy = p->pts[i].y;
    }
    dib_rect_t b = { (int)floorf(minx), (int)floorf(miny), (int)ceilf(maxx) + 1, (int)ceilf(maxy) + 1 };
    if (b.x0 < clip.x0) b.x0 = clip.x0;
    if (b.y0 < clip.y0) b.y0 = clip.y0;
    if (b.x1 > clip.x1) b.x1 = clip.x1;
    if (b.y1 > clip.y1) b.y1 = clip.y1;
    if (dib_rect_empty(&b)) return true;

    edge_t *e = dib_alloc(sizeof(edge_t) * p->n);
    int *act = dib_alloc(sizeof(int) * p->n);
    cross_t *cr = dib_alloc(sizeof(cross_t) * p->n);
    int bw = b.x1 - b.x0;
    float *acc = dib_alloc(sizeof(float) * (bw + 2));
    float *dif = dib_alloc(sizeof(float) * (bw + 2));
    uint8_t *cov = dib_alloc(bw + 2);
    bool ok = e && act && cr && acc && dif && cov;
    int ne = 0;
    for (int c = 0; ok && c < p->nc; c++) {
        int a = p->start[c], z = c + 1 < p->nc ? p->start[c + 1] : p->n;
        for (int i = a; i < z; i++) {
            dib_pt_t u = p->pts[i], v = p->pts[i + 1 < z ? i + 1 : a];
            if (u.y == v.y) continue;
            edge_t *ed = &e[ne++];
            if (u.y < v.y) {
                ed->y0 = u.y; ed->y1 = v.y; ed->x0 = u.x; ed->dir = 1;
            } else {
                ed->y0 = v.y; ed->y1 = u.y; ed->x0 = v.x; ed->dir = -1;
            }
            ed->dxdy = (v.x - u.x) / (v.y - u.y);
        }
    }
    if (ok) qsort(e, ne, sizeof(edge_t), edge_cmp);

    int next = 0, nact = 0;
    const float w = 1.0f / SUBROWS;
    for (int y = b.y0; ok && y < b.y1; y++) {
        memset(acc, 0, sizeof(float) * (bw + 2));
        memset(dif, 0, sizeof(float) * (bw + 2));
        bool any = false;
        for (int s = 0; s < SUBROWS; s++) {
            float sy = (float)y + ((float)s + 0.5f) * w;
            while (next < ne && e[next].y0 <= sy) act[nact++] = next++;
            int nc = 0;
            for (int k = 0; k < nact;) {
                edge_t *ed = &e[act[k]];
                if (ed->y1 <= sy) {
                    act[k] = act[--nact];
                    continue;
                }
                if (ed->y0 <= sy) {
                    cross_t c = { ed->x0 + (sy - ed->y0) * ed->dxdy, ed->dir };
                    int j = nc++;
                    while (j > 0 && cr[j - 1].x > c.x) {
                        cr[j] = cr[j - 1];
                        j--;
                    }
                    cr[j] = c;
                }
                k++;
            }
            int wind = 0;
            float xa = 0;
            for (int k = 0; k < nc; k++) {
                int was = wind;
                wind += cr[k].dir;
                if (!was && wind) {
                    xa = cr[k].x;
                } else if (was && !wind) {
                    float a = xa - (float)b.x0, z = cr[k].x - (float)b.x0;
                    if (a < 0) a = 0;
                    if (z > (float)bw) z = (float)bw;
                    if (z <= a) continue;
                    int ia = (int)a, iz = (int)z;
                    if (ia == iz) {
                        acc[ia] += (z - a) * w;
                    } else {
                        acc[ia] += ((float)(ia + 1) - a) * w;
                        dif[ia + 1] += w;
                        dif[iz] -= w;
                        acc[iz] += (z - (float)iz) * w;
                    }
                    any = true;
                }
            }
        }
        if (!any) continue;
        float run = 0;
        int first = -1, last = -1;
        for (int i = 0; i < bw; i++) {
            run += dif[i];
            float v = (acc[i] + run) * 255.0f + 0.5f;
            uint8_t c = v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)v;
            cov[i] = c;
            if (c) {
                if (first < 0) first = i;
                last = i;
            }
        }
        if (first >= 0) fn(user, y, b.x0 + first, last - first + 1, cov + first);
    }
    dib_free(e);
    dib_free(act);
    dib_free(cr);
    dib_free(acc);
    dib_free(dif);
    dib_free(cov);
    return ok;
}

/* --------------------------------------------------------------------------
 * The floating object
 * -------------------------------------------------------------------------- */

static dib_pt_t to_doc(const dib_obj_t *o, float lx, float ly)
{
    float c = cosf(o->ang), s = sinf(o->ang);
    return (dib_pt_t){ o->cx + lx * c - ly * s, o->cy + lx * s + ly * c };
}

void dib_obj_corners(const dib_obj_t *o, dib_pt_t out[4])
{
    float hw = o->w * 0.5f, hh = o->h * 0.5f;
    out[0] = to_doc(o, -hw, -hh);
    out[1] = to_doc(o, hw, -hh);
    out[2] = to_doc(o, hw, hh);
    out[3] = to_doc(o, -hw, hh);
}

void dib_obj_ends(const dib_obj_t *o, dib_pt_t *a, dib_pt_t *b)
{
    *a = to_doc(o, -o->w * 0.5f, 0);
    *b = to_doc(o, o->w * 0.5f, 0);
}

void dib_obj_set_ends(dib_obj_t *o, dib_pt_t a, dib_pt_t b)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    o->cx = (a.x + b.x) * 0.5f;
    o->cy = (a.y + b.y) * 0.5f;
    o->w = sqrtf(dx * dx + dy * dy);
    o->h = 0;
    o->ang = atan2f(dy, dx);
}

static float head_len(const dib_obj_t *o)
{
    float hl = o->stroke_w * 4.0f;
    return hl < 16.0f ? 16.0f : hl;
}

dib_rect_t dib_obj_bounds(const dib_obj_t *o)
{
    dib_pt_t c[4];
    dib_obj_corners(o, c);
    float minx = c[0].x, maxx = c[0].x, miny = c[0].y, maxy = c[0].y;
    for (int i = 1; i < 4; i++) {
        if (c[i].x < minx) minx = c[i].x;
        if (c[i].x > maxx) maxx = c[i].x;
        if (c[i].y < miny) miny = c[i].y;
        if (c[i].y > maxy) maxy = c[i].y;
    }
    float m = 2.0f;
    if (o->kind != DIB_OBJ_BITMAP) m += o->stroke_w;
    if (o->kind == DIB_OBJ_ARROW) m += head_len(o);
    return (dib_rect_t){ (int)floorf(minx - m), (int)floorf(miny - m), (int)ceilf(maxx + m), (int)ceilf(maxy + m) };
}

bool dib_obj_hit(const dib_obj_t *o, float x, float y, float margin)
{
    float c = cosf(-o->ang), s = sinf(-o->ang);
    float dx = x - o->cx, dy = y - o->cy;
    float lx = dx * c - dy * s, ly = dx * s + dy * c;
    if (o->kind == DIB_OBJ_LINE || o->kind == DIB_OBJ_ARROW) {
        float hw = o->w * 0.5f, m = o->stroke_w * 0.5f + margin;
        return lx >= -hw - m && lx <= hw + m && ly >= -m && ly <= m;
    }
    return fabsf(lx) <= fabsf(o->w) * 0.5f + margin && fabsf(ly) <= fabsf(o->h) * 0.5f + margin;
}

typedef struct {
    dib_doc_t *d;
    dib_px_t   color;
} span_ctx_t;

static void span_flt(void *user, int y, int x0, int n, const uint8_t *cov)
{
    span_ctx_t *c = user;
    dib_doc_t *d = c->d;
    int ty = y / DIB_TILE;
    for (int i = 0; i < n; i++) {
        if (!cov[i]) continue;
        int x = x0 + i;
        dib_px_t *t = dib_flt_tile(d, ty * d->tw + x / DIB_TILE);
        if (!t) return;
        dib_px_t *p = &t[(y % DIB_TILE) * DIB_TILE + x % DIB_TILE];
        *p = dib_over(*p, c->color, cov[i]);
    }
}

static void fill_path(dib_doc_t *d, const dib_path_t *p, dib_px_t color, dib_rect_t clip)
{
    if (!(color >> 24)) return;
    span_ctx_t c = { d, color };
    dib_raster_fill(p, clip, span_flt, &c);
}

static inline dib_px_t bmp_at(const dib_obj_t *o, int x, int y)
{
    if (x < 0 || y < 0 || x >= o->bw || y >= o->bh) return 0;
    return o->bmp[y * o->bw + x];
}

static void render_bitmap(dib_doc_t *d, const dib_obj_t *o, dib_rect_t r)
{
    if (!o->bmp || o->bw <= 0 || o->bh <= 0 || fabsf(o->w) < 0.5f || fabsf(o->h) < 0.5f) return;
    /* Unturned and at its own size: pixel for pixel, no resampling. */
    if (o->ang == 0.0f && fabsf(o->w - (float)o->bw) < 0.01f && fabsf(o->h - (float)o->bh) < 0.01f) {
        int ox = (int)floorf(o->cx - (float)o->bw * 0.5f + 0.5f);
        int oy = (int)floorf(o->cy - (float)o->bh * 0.5f + 0.5f);
        for (int y = 0; y < o->bh; y++) {
            int dy = oy + y;
            if (dy < 0 || dy >= d->h) continue;
            for (int x = 0; x < o->bw; x++) {
                int dx = ox + x;
                dib_px_t s = o->bmp[y * o->bw + x];
                if (dx < 0 || dx >= d->w || !(s >> 24)) continue;
                dib_px_t *t = dib_flt_tile(d, (dy / DIB_TILE) * d->tw + dx / DIB_TILE);
                if (!t) return;
                t[(dy % DIB_TILE) * DIB_TILE + dx % DIB_TILE] = s;
            }
        }
        return;
    }
    float c = cosf(-o->ang), s = sinf(-o->ang);
    float kx = (float)o->bw / o->w, ky = (float)o->bh / o->h;
    for (int y = r.y0; y < r.y1; y++) {
        for (int x = r.x0; x < r.x1; x++) {
            float dx = (float)x + 0.5f - o->cx, dy = (float)y + 0.5f - o->cy;
            float u = (dx * c - dy * s) * kx + (float)o->bw * 0.5f - 0.5f;
            float v = (dx * s + dy * c) * ky + (float)o->bh * 0.5f - 0.5f;
            if (u < -1.0f || v < -1.0f || u > (float)o->bw || v > (float)o->bh) continue;
            int iu = (int)floorf(u), iv = (int)floorf(v);
            float fu = u - (float)iu, fv = v - (float)iv;
            dib_px_t q[4] = { bmp_at(o, iu, iv), bmp_at(o, iu + 1, iv), bmp_at(o, iu, iv + 1),
                              bmp_at(o, iu + 1, iv + 1) };
            float wq[4] = { (1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv, fu * fv };
            /* premultiplied, so a transparent neighbour does not darken the edge */
            float A = 0, R = 0, G = 0, B = 0;
            for (int k = 0; k < 4; k++) {
                float a = (float)DIB_A(q[k]) * wq[k];
                A += a;
                R += (float)DIB_R(q[k]) * a;
                G += (float)DIB_G(q[k]) * a;
                B += (float)DIB_B(q[k]) * a;
            }
            if (A < 0.5f) continue;
            dib_px_t px = DIB_ARGB((unsigned)(A + 0.5f), (unsigned)(R / A + 0.5f), (unsigned)(G / A + 0.5f),
                                   (unsigned)(B / A + 0.5f));
            dib_px_t *t = dib_flt_tile(d, (y / DIB_TILE) * d->tw + x / DIB_TILE);
            if (!t) return;
            t[(y % DIB_TILE) * DIB_TILE + x % DIB_TILE] = px;
        }
    }
}

void dib_obj_render(dib_doc_t *d, const dib_obj_t *o, dib_rect_t *changed)
{
    dib_flt_clear(d, changed);
    dib_rect_t r = dib_obj_bounds(o);
    dib_rect_clip(&r, d->w, d->h);
    if (dib_rect_empty(&r)) return;
    d->flt_box = r;
    if (changed) dib_rect_union(changed, &r);

    if (o->kind == DIB_OBJ_BITMAP) {
        render_bitmap(d, o, r);
        return;
    }
    dib_path_t p;
    dib_path_init(&p);
    float sw = o->stroke_w < 1.0f ? 1.0f : o->stroke_w;
    float aw = fabsf(o->w), ah = fabsf(o->h);

    switch (o->kind) {
    case DIB_OBJ_RECT:
        if (o->fill) {
            dib_path_rrect(&p, o->cx, o->cy, aw, ah, o->radius, o->ang, false);
            fill_path(d, &p, o->fill_c, r);
        }
        if (o->stroke) {
            dib_path_reset(&p);
            float orad = o->radius > 0 ? o->radius + sw * 0.5f : 0;
            dib_path_rrect(&p, o->cx, o->cy, aw + sw, ah + sw, orad, o->ang, false);
            if (aw > sw && ah > sw) {
                float irad = o->radius - sw * 0.5f;
                dib_path_rrect(&p, o->cx, o->cy, aw - sw, ah - sw, irad > 0 ? irad : 0, o->ang, true);
            }
            fill_path(d, &p, o->stroke_c, r);
        }
        break;
    case DIB_OBJ_ELLIPSE:
        if (o->fill) {
            dib_path_ellipse(&p, o->cx, o->cy, aw * 0.5f, ah * 0.5f, o->ang, false);
            fill_path(d, &p, o->fill_c, r);
        }
        if (o->stroke) {
            dib_path_reset(&p);
            dib_path_ellipse(&p, o->cx, o->cy, (aw + sw) * 0.5f, (ah + sw) * 0.5f, o->ang, false);
            if (aw > sw && ah > sw) {
                dib_path_ellipse(&p, o->cx, o->cy, (aw - sw) * 0.5f, (ah - sw) * 0.5f, o->ang, true);
            }
            fill_path(d, &p, o->stroke_c, r);
        }
        break;
    case DIB_OBJ_POLY: {
        dib_pt_t pts[DIB_POLY_MAX];
        for (int i = 0; i < o->npoly; i++) pts[i] = to_doc(o, o->poly[i].x * o->w, o->poly[i].y * o->h);
        if (o->fill && o->npoly >= 3) {
            dib_path_move(&p, pts[0].x, pts[0].y);
            for (int i = 1; i < o->npoly; i++) dib_path_line(&p, pts[i].x, pts[i].y);
            fill_path(d, &p, o->fill_c, r);
        }
        if (o->stroke && o->npoly >= 2) {
            dib_path_reset(&p);
            dib_path_polyline(&p, pts, o->npoly, o->npoly >= 3, sw);
            fill_path(d, &p, o->stroke_c, r);
        }
        break;
    }
    case DIB_OBJ_LINE:
    case DIB_OBJ_ARROW: {
        dib_pt_t a, b;
        dib_obj_ends(o, &a, &b);
        if (o->kind == DIB_OBJ_ARROW && o->w > 1.0f) {
            float hl = head_len(o), hw = hl * 0.8f;
            if (hl > o->w * 0.6f) {
                hl = o->w * 0.6f;
                hw = hl * 0.8f;
            }
            float ux = cosf(o->ang), uy = sinf(o->ang);
            dib_pt_t base = { b.x - ux * hl, b.y - uy * hl };
            dib_pt_t shaft[2] = { a, { b.x - ux * hl * 0.8f, b.y - uy * hl * 0.8f } };
            dib_path_polyline(&p, shaft, 2, false, sw);
            dib_path_move(&p, b.x, b.y);
            dib_path_line(&p, base.x - uy * hw, base.y + ux * hw);
            dib_path_line(&p, base.x + uy * hw, base.y - ux * hw);
            make_cw(&p);
        } else {
            dib_pt_t seg[2] = { a, b };
            dib_path_polyline(&p, seg, 2, false, sw);
        }
        fill_path(d, &p, o->stroke_c, r);
        break;
    }
    default:
        break;
    }
    dib_path_free(&p);
}

void dib_obj_free(dib_obj_t *o)
{
    dib_free(o->bmp);
    o->bmp = NULL;
    o->bw = o->bh = 0;
}
