/*
 * DIBUJO - painting. See dib_paint.h.
 */
#include "dib_paint.h"

#include <math.h>
#include <string.h>

const dib_brush_def_t dib_brush_defs[DIB_BR_COUNT] = {
    /*               hard  flow  spacing max    size  opacity */
    [DIB_BR_PENCIL] = { 1.0f, 1.0f,  0.10f, false,  4.0f, 1.0f },
    [DIB_BR_SOFT]   = { 0.0f, 0.55f, 0.08f, false, 32.0f, 1.0f },
    [DIB_BR_AIR]    = { 0.0f, 0.07f, 0.12f, false, 64.0f, 1.0f },
    [DIB_BR_MARKER] = { 0.85f, 1.0f, 0.08f, true,  20.0f, 0.5f },
    [DIB_BR_ERASER] = { 0.9f, 1.0f,  0.10f, false, 32.0f, 1.0f },
};

#define AIR_MS_PER_DAB  22.0f

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* The dab's strength at distance dist from its centre, 0..1. */
static inline float profile(float dist, float r, float hard)
{
    if (hard >= 0.99f) return clampf(r - dist + 0.5f, 0.0f, 1.0f);
    float inner = r * hard;
    if (dist <= inner) return 1.0f;
    if (dist >= r) return 0.0f;
    float t = (dist - inner) / (r - inner);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

static void dab_one(dib_stroke_t *s, float cx, float cy)
{
    dib_doc_t *d = s->doc;
    const dib_brush_def_t *def = &dib_brush_defs[s->br.kind];
    float r = s->br.size * 0.5f;
    if (r < 0.5f) r = 0.5f;
    dib_rect_t b = { (int)floorf(cx - r - 1), (int)floorf(cy - r - 1),
                     (int)ceilf(cx + r + 1), (int)ceilf(cy + r + 1) };
    dib_rect_clip(&b, d->w, d->h);
    if (dib_rect_empty(&b)) return;

    const bool erase = s->br.kind == DIB_BR_ERASER;
    const unsigned op = (unsigned)(clampf(s->br.opacity, 0.0f, 1.0f) * 255.0f + 0.5f);
    const float flow = def->flow * 65535.0f;
    const float r2 = (r + 1.0f) * (r + 1.0f);
    dib_layer_t *L = &d->layers[s->layer];

    for (int ty = b.y0 / DIB_TILE; ty <= (b.y1 - 1) / DIB_TILE; ty++) {
        for (int tx = b.x0 / DIB_TILE; tx <= (b.x1 - 1) / DIB_TILE; tx++) {
            int t = ty * d->tw + tx;
            if (!dib_hist_save(d, s->hist, s->layer, t)) return;
            bool found;
            const dib_px_t *before = dib_hist_before(d, s->hist, s->layer, t, &found);
            if (erase && !before) continue;             /* nothing to erase there */
            dib_px_t *tile = dib_tile_get(d, L->tiles, t);
            if (!tile) return;
            if (!s->mask[t]) {
                s->mask[t] = dib_calloc(DIB_TILE_PX * sizeof(uint16_t));
                if (!s->mask[t]) {
                    d->oom = true;
                    return;
                }
            }
            uint16_t *m = s->mask[t];
            int x0 = tx * DIB_TILE, y0 = ty * DIB_TILE;
            int ax = b.x0 > x0 ? b.x0 : x0, bx = b.x1 < x0 + DIB_TILE ? b.x1 : x0 + DIB_TILE;
            int ay = b.y0 > y0 ? b.y0 : y0, by = b.y1 < y0 + DIB_TILE ? b.y1 : y0 + DIB_TILE;
            for (int y = ay; y < by; y++) {
                float dy = (float)y + 0.5f - cy;
                for (int x = ax; x < bx; x++) {
                    float dx = (float)x + 0.5f - cx;
                    float dd = dx * dx + dy * dy;
                    if (dd > r2) continue;
                    float a = profile(sqrtf(dd), r, def->hardness);
                    if (a <= 0.0f) continue;
                    int i = (y - y0) * DIB_TILE + (x - x0);
                    unsigned mv = m[i], nm;
                    if (def->max_mode) {
                        nm = (unsigned)(a * flow);
                        if (nm <= mv) continue;
                    } else {
                        nm = mv + (unsigned)((65535.0f - (float)mv) * a * def->flow + 0.5f);
                        if (nm > 65535) nm = 65535;
                        if (nm == mv) continue;
                    }
                    m[i] = (uint16_t)nm;
                    unsigned sa = (nm * op + 32767) / 65535;
                    dib_px_t bp = before ? before[i] : 0;
                    if (erase) {
                        unsigned na = (DIB_A(bp) * (255 - sa) + 127) / 255;
                        tile[i] = na ? ((bp & 0x00FFFFFF) | ((dib_px_t)na << 24)) : 0;
                    } else {
                        tile[i] = dib_over(bp, s->br.color, sa);
                    }
                }
            }
        }
    }
    s->any = true;
    dib_rect_union(&s->dirty, &b);
}

static void dab(dib_stroke_t *s, float x, float y)
{
    float W = (float)s->doc->w, H = (float)s->doc->h;
    dab_one(s, x, y);
    if (s->br.sym == DIB_SYM_V || s->br.sym == DIB_SYM_4) dab_one(s, W - x, y);
    if (s->br.sym == DIB_SYM_H || s->br.sym == DIB_SYM_4) dab_one(s, x, H - y);
    if (s->br.sym == DIB_SYM_4) dab_one(s, W - x, H - y);
}

static float step_of(const dib_stroke_t *s)
{
    float st = s->br.size * dib_brush_defs[s->br.kind].spacing;
    return st < 0.5f ? 0.5f : st;
}

/* Dabs from the last one towards (x, y), one every step. */
static void walk_to(dib_stroke_t *s, float x, float y)
{
    float step = step_of(s);
    float dx = x - s->lx, dy = y - s->ly;
    float len = sqrtf(dx * dx + dy * dy);
    while (len >= step) {
        s->lx += dx / len * step;
        s->ly += dy / len * step;
        dab(s, s->lx, s->ly);
        dx = x - s->lx;
        dy = y - s->ly;
        len = sqrtf(dx * dx + dy * dy);
    }
}

bool dib_stroke_begin(dib_stroke_t *s, dib_doc_t *d, const dib_brush_t *br, float x, float y)
{
    memset(s, 0, sizeof(*s));
    s->doc = d;
    s->br = *br;
    if (s->br.kind < 0 || s->br.kind >= DIB_BR_COUNT) s->br.kind = DIB_BR_PENCIL;
    s->br.color |= 0xFF000000u;
    s->layer = d->active;
    s->mask = dib_calloc(sizeof(uint16_t *) * d->ntiles);
    s->hist = dib_hist_begin(d);
    if (!s->mask || !s->hist) {
        dib_free(s->mask);
        if (s->hist) dib_hist_abort(d, s->hist);
        d->oom = true;
        return false;
    }
    s->fx = s->sx = s->lx = x;
    s->fy = s->sy = s->ly = y;
    s->active = true;
    dab(s, x, y);
    return true;
}

static void smooth_step(dib_stroke_t *s)
{
    float k = 1.0f - clampf(s->br.smooth, 0.0f, 1.0f) * 0.88f;
    s->sx += (s->fx - s->sx) * k;
    s->sy += (s->fy - s->sy) * k;
    walk_to(s, s->sx, s->sy);
}

void dib_stroke_to(dib_stroke_t *s, float x, float y)
{
    if (!s->active) return;
    s->fx = x;
    s->fy = y;
    smooth_step(s);
}

void dib_stroke_hold(dib_stroke_t *s, float ms)
{
    if (!s->active) return;
    smooth_step(s);
    if (s->br.kind == DIB_BR_AIR) {
        /* a still airbrush keeps spraying where it is */
        int n = (int)(ms / AIR_MS_PER_DAB + 0.5f);
        for (int i = 0; i < n && i < 8; i++) dab(s, s->lx, s->ly);
    }
}

static void free_masks(dib_stroke_t *s)
{
    if (!s->mask) return;
    for (int t = 0; t < s->doc->ntiles; t++) dib_free(s->mask[t]);
    dib_free(s->mask);
    s->mask = NULL;
}

void dib_stroke_end(dib_stroke_t *s)
{
    if (!s->active) return;
    walk_to(s, s->fx, s->fy);
    dib_doc_t *d = s->doc;
    /* an eraser can leave whole tiles empty: they go */
    for (int t = 0; t < d->ntiles; t++) {
        if (s->mask[t] && dib_tile_empty(d->layers[s->layer].tiles[t])) {
            dib_tile_release(d, &d->layers[s->layer].tiles[t]);
        }
    }
    free_masks(s);
    dib_hist_end(d, s->hist);
    s->hist = NULL;
    s->active = false;
}

void dib_stroke_cancel(dib_stroke_t *s)
{
    if (!s->active) return;
    dib_doc_t *d = s->doc;
    for (int t = 0; t < d->ntiles; t++) {
        if (s->mask[t]) {
            int tx = t % d->tw, ty = t / d->tw;
            dib_rect_t r = { tx * DIB_TILE, ty * DIB_TILE, (tx + 1) * DIB_TILE, (ty + 1) * DIB_TILE };
            dib_rect_union(&s->dirty, &r);
        }
    }
    free_masks(s);
    dib_hist_abort(d, s->hist);
    s->hist = NULL;
    s->active = false;
}

bool dib_stroke_take_dirty(dib_stroke_t *s, dib_rect_t *r)
{
    if (dib_rect_empty(&s->dirty)) return false;
    *r = s->dirty;
    s->dirty = (dib_rect_t){ 0, 0, 0, 0 };
    return true;
}

/* --------------------------------------------------------------------------
 * The fill
 * -------------------------------------------------------------------------- */

static inline bool similar(dib_px_t a, dib_px_t b, int tol)
{
    int aa = (int)DIB_A(a), ba = (int)DIB_A(b);
    if (aa <= tol / 4 && ba <= tol / 4) return true;    /* both (almost) transparent */
    int d = aa - ba;
    if (d < 0) d = -d;
    if (d > tol) return false;
    int dr = (int)DIB_R(a) - (int)DIB_R(b), dg = (int)DIB_G(a) - (int)DIB_G(b), db = (int)DIB_B(a) - (int)DIB_B(b);
    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    return dr <= tol && dg <= tol && db <= tol;
}

typedef struct {
    int32_t *v;
    int      n, cap;
} istack_t;

static bool push3(istack_t *s, int y, int xl, int xr)
{
    if (s->n + 3 > s->cap) {
        int cap = s->cap ? s->cap * 2 : 3 * 1024;
        int32_t *v = dib_alloc(sizeof(int32_t) * cap);
        if (!v) return false;
        if (s->n) memcpy(v, s->v, sizeof(int32_t) * s->n);
        dib_free(s->v);
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n++] = y;
    s->v[s->n++] = xl;
    s->v[s->n++] = xr;
    return true;
}

bool dib_fill(dib_doc_t *d, int x, int y, dib_px_t color, float opacity, int tolerance,
              bool all_layers, dib_rect_t *changed)
{
    if (x < 0 || y < 0 || x >= d->w || y >= d->h) return false;
    const int W = d->w, H = d->h;
    dib_px_t target = dib_pick(d, x, y, all_layers);
    color |= 0xFF000000u;
    unsigned op = (unsigned)(clampf(opacity, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (!all_layers && target == color && op == 255) return false;

    uint8_t *mask = dib_calloc((size_t)W * H);
    if (!mask) {
        d->oom = true;
        return false;
    }
    istack_t st = { 0 };
    bool ok = push3(&st, y, x, x);
    dib_rect_t box = { x, y, x + 1, y + 1 };
#define INSIDE(px, py) (!mask[(size_t)(py) * W + (px)] && similar(dib_pick(d, (px), (py), all_layers), target, tolerance))
    while (ok && st.n) {
        int xr = st.v[--st.n], xl = st.v[--st.n], yy = st.v[--st.n];
        (void)xr;
        if (!INSIDE(xl, yy)) continue;
        int a = xl, b = xl;
        while (a > 0 && INSIDE(a - 1, yy)) a--;
        while (b < W - 1 && INSIDE(b + 1, yy)) b++;
        memset(mask + (size_t)yy * W + a, 1, (size_t)(b - a + 1));
        if (a < box.x0) box.x0 = a;
        if (b + 1 > box.x1) box.x1 = b + 1;
        if (yy < box.y0) box.y0 = yy;
        if (yy + 1 > box.y1) box.y1 = yy + 1;
        for (int ny = yy - 1; ny <= yy + 1 && ok; ny += 2) {
            if (ny < 0 || ny >= H) continue;
            bool in = false;
            for (int k = a; k <= b; k++) {
                bool here = INSIDE(k, ny);
                if (here && !in) ok = push3(&st, ny, k, k);
                in = here;
            }
        }
    }
#undef INSIDE
    dib_free(st.v);
    if (!ok) {
        d->oom = true;
        dib_free(mask);
        return false;
    }

    /* one pixel more, over the soft edge of whatever stopped the fill */
    dib_rect_t g = { box.x0 - 1, box.y0 - 1, box.x1 + 1, box.y1 + 1 };
    dib_rect_clip(&g, W, H);
    for (int yy = g.y0; yy < g.y1; yy++) {
        uint8_t *row = mask + (size_t)yy * W;
        for (int xx = g.x0; xx < g.x1; xx++) {
            if (row[xx]) continue;
            if ((xx > 0 && row[xx - 1] == 1) || (xx < W - 1 && row[xx + 1] == 1) ||
                (yy > 0 && row[xx - W] == 1) || (yy < H - 1 && row[xx + W] == 1)) {
                row[xx] = 2;
            }
        }
    }

    dib_hent_t *h = dib_hist_begin(d);
    int la = d->active;
    for (int ty = g.y0 / DIB_TILE; h && ty <= (g.y1 - 1) / DIB_TILE; ty++) {
        for (int tx = g.x0 / DIB_TILE; tx <= (g.x1 - 1) / DIB_TILE; tx++) {
            int t = ty * d->tw + tx, x0 = tx * DIB_TILE, y0 = ty * DIB_TILE;
            int ex = x0 + DIB_TILE < W ? x0 + DIB_TILE : W, ey = y0 + DIB_TILE < H ? y0 + DIB_TILE : H;
            bool any = false;
            for (int yy = y0; yy < ey && !any; yy++) {
                for (int xx = x0; xx < ex; xx++) {
                    if (mask[(size_t)yy * W + xx]) { any = true; break; }
                }
            }
            if (!any) continue;
            if (!dib_hist_save(d, h, la, t)) goto out;
            dib_px_t *tile = dib_tile_get(d, d->layers[la].tiles, t);
            if (!tile) goto out;
            for (int yy = y0; yy < ey; yy++) {
                for (int xx = x0; xx < ex; xx++) {
                    if (mask[(size_t)yy * W + xx]) {
                        dib_px_t *p = &tile[(yy - y0) * DIB_TILE + xx - x0];
                        *p = dib_over(*p, color, op);
                    }
                }
            }
        }
    }
out:
    dib_hist_end(d, h);
    dib_free(mask);
    if (changed) dib_rect_union(changed, &g);
    return true;
}
