/*
 * DERIVA - the picture (see ast_gfx.h). The list, the hashes, the two
 * cores and the turned strips are Metegol's (apps/metegol/main/mg_gfx.c,
 * itself Bastion's); what is new is the neon stroke and a screen that may
 * be upright or lying down.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "ast_gfx.h"

#if defined(AST_HARNESS)
/* tools/: the two the compositor needs, from the tool itself */
void aos_hal_log(const char *tag, const char *fmt, ...);
bool aos_hal_worker_split(void (*fn)(void *arg, int part), void *arg);
#else
#include "aos_hal.h"
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN) && !defined(AST_HARNESS)
#include "esp_heap_caps.h"
#endif

enum { IT_LINE, IT_GLOW, IT_RING, IT_DISC, IT_RECT };

#define M565 0x07E0F81Fu
#define EXP(c) ((((uint32_t)(c)) | (((uint32_t)(c)) << 16)) & M565)
#define GLOW_R 8.0f         /* a stroke's halo reaches this far, times its width */
#define PIECE  40.0f        /* longer strokes go in pieces                        */

/* ---- memory ---------------------------------------------------------------------------- */

static size_t s_used;

void *ast_malloc(size_t n)
{
    size_t total = n + 16;
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN) && !defined(AST_HARNESS)
    /* PSRAM or nothing: internal RAM is the system's */
    uint32_t *b = (uint32_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    uint32_t *b = (uint32_t *)malloc(total);
#endif
    if (!b) return NULL;
    b[0] = (uint32_t)n;
    b[1] = 0xA57EA57Eu;
    s_used += n;
    return (uint8_t *)b + 16;
}

void *ast_calloc(size_t n)
{
    void *p = ast_malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void ast_free(void *p)
{
    if (!p) return;
    uint32_t *b = (uint32_t *)((uint8_t *)p - 16);
    if (b[1] == 0xA57EA57Eu) {
        s_used -= b[0];
        b[1] = 0;
    }
    free(b);
}

size_t ast_mem_used(void)
{
    return s_used;
}

static inline int rnd(float v)
{
    return (int)floorf(v + 0.5f);
}

/* ---- pixels ---------------------------------------------------------------------------- */

static inline uint16_t blend5(uint16_t bg, uint32_t fg_e, uint32_t a5)
{
    uint32_t b = EXP(bg);
    b = (b + (((fg_e - b) * a5) >> 5)) & M565;
    return (uint16_t)(b | (b >> 16));
}

static inline uint16_t add_sat(uint16_t bg, uint32_t add_e)
{
    uint32_t s = EXP(bg) + add_e;
    uint32_t o = s & 0x08010020u;
    s = (s | (o - (o >> 5))) & M565;
    return (uint16_t)(s | (s >> 16));
}

/* ---- set up ---------------------------------------------------------------------------- */

bool ast_gfx_init(ast_gfx_t *g)
{
    memset(g, 0, sizeof *g);
    g->item = (ast_item_t *)ast_malloc(sizeof(ast_item_t) * AST_MAX_ITEMS);
    g->cur = (uint32_t *)ast_malloc(sizeof(uint32_t) * AST_NT);
    g->need = (uint8_t *)ast_malloc(AST_NT);
    g->band_cap = AST_MAX_ITEMS * 6;
    g->band_ix = (uint16_t *)ast_malloc(sizeof(uint16_t) * (size_t)g->band_cap);
    for (int i = 0; i < 2; i++) g->strip[i] = (uint16_t *)ast_malloc((size_t)AST_WMAX * AST_TILE * 2);
    if (!g->item || !g->cur || !g->need || !g->band_ix || !g->strip[0] || !g->strip[1]) {
        ast_gfx_free(g);
        return false;
    }
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            float dx = (x - 31.5f) / 32.0f, dy = (y - 31.5f) / 32.0f;
            float d = dx * dx + dy * dy;
            float v = d >= 1.0f ? 0.0f : (1.0f - d);
            v = v * v * (0.55f + 0.45f * v);
            g->glow[y * 64 + x] = (uint8_t)(v * 255.0f);
        }
    }
    /* a stroke across: a white-hot core about 3 px wide and a halo that
     * fades over 7; the two never add past full */
    for (int i = 0; i < 256; i++) {
        /* i is 4 d^2 with d in widths: up to 8 */
        float d = sqrtf((float)i / 4.0f);
        float c = 1.0f - (d - 0.7f) / 1.1f;
        c = c < 0 ? 0 : c > 1 ? 1 : c;
        c = c * c * (3.0f - 2.0f * c);
        float h = expf(-d / 2.6f) * (1.0f - d / 8.0f);
        if (h < 0) h = 0;
        float cc = 0.6f * c, hh = 0.62f * h;
        if (cc + hh > 1.0f) hh = 1.0f - cc;
        g->lut_c[i] = (uint8_t)(cc * 255.0f + 0.5f);
        g->lut_h[i] = (uint8_t)(hh * 255.0f + 0.5f);
    }
    ast_gfx_size(g, 720, 1280);
    return true;
}

void ast_gfx_free(ast_gfx_t *g)
{
    ast_free(g->item);
    ast_free(g->cur);
    ast_free(g->need);
    ast_free(g->band_ix);
    for (int i = 0; i < 2; i++) ast_free(g->strip[i]);
    memset(g, 0, sizeof *g);
}

void ast_gfx_size(ast_gfx_t *g, int w, int h)
{
    g->W = w;
    g->H = h;
    g->TW = w / AST_TILE;
    g->TH = h / AST_TILE;
}

void ast_gfx_world(ast_gfx_t *g, const uint16_t *bg)
{
    g->bg = bg;
}

/* ---- the list ------------------------------------------------------------------------------ */

void ast_gfx_begin(ast_gfx_t *g)
{
    g->nitem = 0;
    g->overflow = false;
}

static uint32_t mix(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 0x01000193u;
    h ^= h >> 15;
    return h;
}

void ast_gfx_clip(ast_gfx_t *g, bool on, int x0, int y0, int x1, int y1)
{
    g->clip_on = on;
    /* off keeps the rectangle: the things already listed are drawn with it */
    if (!on) return;
    g->clip.x0 = (int16_t)x0;
    g->clip.y0 = (int16_t)y0;
    g->clip.x1 = (int16_t)x1;
    g->clip.y1 = (int16_t)y1;
}

#define KIND_CLIP 0x80

static ast_item_t *push(ast_gfx_t *g, int kind, int x0, int y0, int x1, int y1)
{
    if (g->clip_on) {
        if (x0 < g->clip.x0) x0 = g->clip.x0;
        if (y0 < g->clip.y0) y0 = g->clip.y0;
        if (x1 > g->clip.x1) x1 = g->clip.x1;
        if (y1 > g->clip.y1) y1 = g->clip.y1;
        kind |= KIND_CLIP;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g->W) x1 = g->W;
    if (y1 > g->H) y1 = g->H;
    if (x1 <= x0 || y1 <= y0) return NULL;
    if (g->nitem >= AST_MAX_ITEMS) {
        g->overflow = true;
        return NULL;
    }
    ast_item_t *it = &g->item[g->nitem++];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    it->x0 = (int16_t)x0;
    it->y0 = (int16_t)y0;
    it->x1 = (int16_t)x1;
    it->y1 = (int16_t)y1;
    return it;
}

static void seal(ast_item_t *it)
{
    uint32_t h = 0x811C9DC5u;
    h = mix(h, (uint32_t)it->kind | ((uint32_t)it->flags << 8) | ((uint32_t)it->a << 16) | ((uint32_t)it->aux << 24));
    h = mix(h, (uint32_t)(uint16_t)it->p[0] | ((uint32_t)(uint16_t)it->p[1] << 16));
    h = mix(h, (uint32_t)(uint16_t)it->p[2] | ((uint32_t)(uint16_t)it->p[3] << 16));
    h = mix(h, (uint32_t)(uint16_t)it->p[4] | ((uint32_t)(uint16_t)it->p[5] << 16));
    h = mix(h, (uint32_t)it->color | ((uint32_t)it->color2 << 16));
    it->hash = h ? h : 1;
}

static uint16_t hot_of(uint32_t rgb)
{
    /* the core: the colour well on its way to white */
    uint32_t r = (rgb >> 16) & 255, gg = (rgb >> 8) & 255, b = rgb & 255;
    r += (255 - r) * 3 / 5;
    gg += (255 - gg) * 3 / 5;
    b += (255 - b) * 3 / 5;
    return ast_rgb((r << 16) | (gg << 8) | b);
}

static void piece(ast_gfx_t *g, float ax, float ay, float bx, float by, uint16_t c, uint16_t hot, float w, int k, int caps)
{
    float r = GLOW_R * w + 1.0f;
    float mnx = ax < bx ? ax : bx, mxx = ax < bx ? bx : ax;
    float mny = ay < by ? ay : by, mxy = ay < by ? by : ay;
    ast_item_t *it = push(g, IT_LINE, (int)floorf(mnx - r), (int)floorf(mny - r), (int)ceilf(mxx + r) + 1,
                          (int)ceilf(mxy + r) + 1);
    if (!it) return;
    it->flags = (uint8_t)caps;
    it->a = (uint8_t)k;
    int wq = (int)(w * 16.0f + 0.5f);
    it->aux = (uint8_t)(wq < 4 ? 4 : wq > 255 ? 255 : wq);
    it->p[0] = (int16_t)rnd(ax * 8.0f);
    it->p[1] = (int16_t)rnd(ay * 8.0f);
    it->p[2] = (int16_t)rnd(bx * 8.0f);
    it->p[3] = (int16_t)rnd(by * 8.0f);
    it->color = c;
    it->color2 = hot;
    seal(it);
}

static void line_c(ast_gfx_t *g, float ax, float ay, float bx, float by, uint16_t c, uint16_t hot, float w, int k, int caps)
{
    if (k <= 0) return;
    if (k > 255) k = 255;
    /* wholly off the screen: nothing (the halo may still reach in) */
    float r = GLOW_R * w + 1.0f;
    if ((ax < -r && bx < -r) || (ay < -r && by < -r) || (ax > g->W + r && bx > g->W + r) ||
        (ay > g->H + r && by > g->H + r))
        return;
    float dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy);
    int n = len > PIECE ? (int)ceilf(len / (PIECE * 0.9f)) : 1;
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) {
        float t0 = (float)i / (float)n, t1 = (float)(i + 1) / (float)n;
        int cp = 0;
        if (i == 0) cp |= caps & AST_F_CAP0;
        if (i == n - 1) cp |= caps & AST_F_CAP1;
        piece(g, ax + dx * t0, ay + dy * t0, ax + dx * t1, ay + dy * t1, c, hot, w, k, cp);
    }
}

void ast_line(ast_gfx_t *g, float ax, float ay, float bx, float by, uint32_t rgb, float w, int k, int caps)
{
    line_c(g, ax, ay, bx, by, ast_rgb(rgb), hot_of(rgb), w, k, caps);
}

void ast_poly(ast_gfx_t *g, const float *xy, int n, uint32_t rgb, float w, int k)
{
    uint16_t c = ast_rgb(rgb), hot = hot_of(rgb);
    for (int i = 0; i < n; i++) {
        int j = i + 1 == n ? 0 : i + 1;
        line_c(g, xy[i * 2], xy[i * 2 + 1], xy[j * 2], xy[j * 2 + 1], c, hot, w, k, AST_F_CAP0);
    }
}

void ast_glow(ast_gfx_t *g, float x, float y, int r, uint32_t rgb, int k)
{
    if (r < 1 || k <= 0) return;
    int xi = (int)rnd(x), yi = (int)rnd(y);
    ast_item_t *it = push(g, IT_GLOW, xi - r, yi - r, xi + r + 1, yi + r + 1);
    if (!it) return;
    it->p[0] = (int16_t)xi;
    it->p[1] = (int16_t)yi;
    it->p[2] = (int16_t)r;
    it->color = ast_rgb(rgb);
    it->a = (uint8_t)(k > 255 ? 255 : k);
    seal(it);
}

void ast_ring(ast_gfx_t *g, int cx, int cy, int rx, int ry, int thick, uint32_t rgb, int a, int flags)
{
    if (rx < 1 || ry < 1 || a <= 0) return;
    ast_item_t *it = push(g, IT_RING, cx - rx, cy - ry, cx + rx + 1, cy + ry + 1);
    if (!it) return;
    it->p[0] = (int16_t)cx;
    it->p[1] = (int16_t)cy;
    it->p[2] = (int16_t)rx;
    it->p[3] = (int16_t)ry;
    it->p[4] = (int16_t)thick;
    it->color = ast_rgb(rgb);
    it->a = (uint8_t)(a > 255 ? 255 : a);
    it->flags = (uint8_t)flags;
    seal(it);
}

void ast_disc(ast_gfx_t *g, int cx, int cy, int rx, int ry, uint32_t rgb, int a, int flags)
{
    if (rx < 1 || ry < 1 || a <= 0) return;
    ast_item_t *it = push(g, IT_DISC, cx - rx, cy - ry, cx + rx + 1, cy + ry + 1);
    if (!it) return;
    it->p[0] = (int16_t)cx;
    it->p[1] = (int16_t)cy;
    it->p[2] = (int16_t)rx;
    it->p[3] = (int16_t)ry;
    it->color = ast_rgb(rgb);
    it->a = (uint8_t)(a > 255 ? 255 : a);
    it->flags = (uint8_t)flags;
    seal(it);
}

void ast_rect(ast_gfx_t *g, int x, int y, int w, int h, uint32_t rgb, int a, int flags)
{
    if (a <= 0) return;
    ast_item_t *it = push(g, IT_RECT, x, y, x + w, y + h);
    if (!it) return;
    it->color = ast_rgb(rgb);
    it->a = (uint8_t)(a > 255 ? 255 : a);
    it->flags = (uint8_t)flags;
    seal(it);
}

/* ---- drawing, clipped to c ------------------------------------------------------------------ */

#define ROW(fb, y) ((fb) + (size_t)(y) * (size_t)g->W)

static inline void stroke_px(uint16_t *p, const ast_gfx_t *g, float d2, float inv_w2, uint32_t ce, uint32_t he, uint32_t k)
{
    int i = (int)(d2 * inv_w2 * 4.0f);
    if (i > 255) return;
    uint32_t ah = ((uint32_t)g->lut_h[i] * k) >> 11;    /* 0..31 */
    uint32_t ac = ((uint32_t)g->lut_c[i] * k) >> 11;
    if (!(ah | ac)) return;
    *p = add_sat(*p, (((ce * ah) >> 5) & M565) + (((he * ac) >> 5) & M565));
}

static void draw_line(const ast_gfx_t *g, uint16_t *fb, const ast_item_t *it, const ast_rect_t *c)
{
    int xa = it->x0 < c->x0 ? c->x0 : it->x0, xb = it->x1 > c->x1 ? c->x1 : it->x1;
    int ya = it->y0 < c->y0 ? c->y0 : it->y0, yb = it->y1 > c->y1 ? c->y1 : it->y1;
    if (xa >= xb || ya >= yb) return;
    float ax = it->p[0] * 0.125f, ay = it->p[1] * 0.125f;
    float dx = it->p[2] * 0.125f - ax, dy = it->p[3] * 0.125f - ay;
    float l2 = dx * dx + dy * dy;
    float inv = l2 > 1e-4f ? 1.0f / l2 : 0.0f;
    float w = (float)it->aux / 16.0f;
    float inv_w2 = 1.0f / (w * w);
    float R = GLOW_R * w;
    bool cap0 = (it->flags & AST_F_CAP0) != 0, cap1 = (it->flags & AST_F_CAP1) != 0;
    uint32_t ce = EXP(it->color), he = EXP(it->color2), k = it->a;
    if (l2 <= 1e-4f) {
        if (!cap0 && !cap1) return;
        /* a point: a dot */
        for (int y = ya; y < yb; y++) {
            uint16_t *row = ROW(fb, y);
            float qy = (float)y + 0.5f - ay;
            for (int x = xa; x < xb; x++) {
                float qx = (float)x + 0.5f - ax;
                stroke_px(row + x, g, qx * qx + qy * qy, inv_w2, ce, he, k);
            }
        }
        return;
    }
    float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (ady >= adx) {
        /* steep: along the rows, a span of the strip's width around its middle */
        float sx = dx / dy;
        float hw = R * sqrtf(1.0f + sx * sx) + 1.5f;
        for (int y = ya; y < yb; y++) {
            float py = (float)y + 0.5f;
            float t = (py - ay) / dy;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float xc = ax + t * dx;
            int x0 = (int)floorf(xc - hw), x1 = (int)ceilf(xc + hw);
            if (x0 < xa) x0 = xa;
            if (x1 > xb) x1 = xb;
            if (x0 >= x1) continue;
            uint16_t *row = ROW(fb, y);
            float qy = py - ay;
            for (int x = x0; x < x1; x++) {
                float qx = (float)x + 0.5f - ax;
                float tt = (qx * dx + qy * dy) * inv;
                float d2;
                if (tt < 0) {
                    if (!cap0) continue;
                    d2 = qx * qx + qy * qy;
                } else if (tt > 1) {
                    if (!cap1) continue;
                    float ex = qx - dx, ey = qy - dy;
                    d2 = ex * ex + ey * ey;
                } else {
                    float cr = qx * dy - qy * dx;
                    d2 = cr * cr * inv;
                }
                stroke_px(row + x, g, d2, inv_w2, ce, he, k);
            }
        }
    } else {
        /* flat: along the columns */
        float sy = dy / dx;
        float hh = R * sqrtf(1.0f + sy * sy) + 1.5f;
        for (int x = xa; x < xb; x++) {
            float px = (float)x + 0.5f;
            float t = (px - ax) / dx;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float yc = ay + t * dy;
            int y0 = (int)floorf(yc - hh), y1 = (int)ceilf(yc + hh);
            if (y0 < ya) y0 = ya;
            if (y1 > yb) y1 = yb;
            float qx = px - ax;
            for (int y = y0; y < y1; y++) {
                float qy = (float)y + 0.5f - ay;
                float tt = (qx * dx + qy * dy) * inv;
                float d2;
                if (tt < 0) {
                    if (!cap0) continue;
                    d2 = qx * qx + qy * qy;
                } else if (tt > 1) {
                    if (!cap1) continue;
                    float ex = qx - dx, ey = qy - dy;
                    d2 = ex * ex + ey * ey;
                } else {
                    float cr = qx * dy - qy * dx;
                    d2 = cr * cr * inv;
                }
                stroke_px(ROW(fb, y) + x, g, d2, inv_w2, ce, he, k);
            }
        }
    }
}

static void draw_glow(const ast_gfx_t *g, uint16_t *fb, const ast_item_t *it, const ast_rect_t *c)
{
    int r = it->p[2];
    int xa = it->x0 < c->x0 ? c->x0 : it->x0, xb = it->x1 > c->x1 ? c->x1 : it->x1;
    int ya = it->y0 < c->y0 ? c->y0 : it->y0, yb = it->y1 > c->y1 ? c->y1 : it->y1;
    uint32_t step = (64u << 16) / (uint32_t)(2 * r + 1);
    uint32_t ce = EXP(it->color);
    int ox = it->p[0] - r, oy = it->p[1] - r;
    for (int y = ya; y < yb; y++) {
        const uint8_t *t = g->glow + (((uint32_t)(y - oy) * step) >> 16) * 64;
        uint16_t *dst = ROW(fb, y);
        uint32_t u = (uint32_t)(xa - ox) * step;
        for (int x = xa; x < xb; x++, u += step) {
            uint32_t a = ((uint32_t)t[u >> 16] * it->a) >> 11;
            if (a) dst[x] = add_sat(dst[x], ((ce * a) >> 5) & M565);
        }
    }
}

static inline void span(uint16_t *dst, int xa, int xb, const ast_rect_t *c, uint32_t ce, uint32_t a5, bool add, uint16_t color)
{
    if (xa < c->x0) xa = c->x0;
    if (xb > c->x1) xb = c->x1;
    if (add) {
        uint32_t e = ((ce * a5) >> 5) & M565;
        for (int x = xa; x < xb; x++) dst[x] = add_sat(dst[x], e);
    } else if (a5 >= 31) {
        for (int x = xa; x < xb; x++) dst[x] = color;
    } else {
        for (int x = xa; x < xb; x++) dst[x] = blend5(dst[x], ce, a5);
    }
}

static void draw_ring(const ast_gfx_t *g, uint16_t *fb, const ast_item_t *it, const ast_rect_t *c, bool fill)
{
    int cx = it->p[0], cy = it->p[1], rx = it->p[2], ry = it->p[3], th = it->p[4];
    int ya = it->y0 < c->y0 ? c->y0 : it->y0, yb = it->y1 > c->y1 ? c->y1 : it->y1;
    uint32_t ce = EXP(it->color);
    uint32_t a5 = (uint32_t)(it->a + 4) >> 3;
    bool add = (it->flags & AST_F_ADD) != 0;
    int rxi = rx - th, ryi = ry - (th * ry + rx / 2) / rx;
    if (ryi < 1) ryi = 1;
    for (int y = ya; y < yb; y++) {
        float d = ((float)(y - cy)) / (float)ry;
        float q = 1.0f - d * d;
        if (q < 0) continue;
        int xo = (int)((float)rx * sqrtf(q) + 0.5f);
        uint16_t *dst = ROW(fb, y);
        if (fill) {
            span(dst, cx - xo, cx + xo + 1, c, ce, a5, add, it->color);
            continue;
        }
        int xi = -1;
        if (rxi > 0) {
            float di = ((float)(y - cy)) / (float)ryi;
            float qi = 1.0f - di * di;
            if (qi > 0) xi = (int)((float)rxi * sqrtf(qi) + 0.5f);
        }
        if (xi < 0) {
            span(dst, cx - xo, cx + xo + 1, c, ce, a5, add, it->color);
        } else {
            span(dst, cx - xo, cx - xi, c, ce, a5, add, it->color);
            span(dst, cx + xi + 1, cx + xo + 1, c, ce, a5, add, it->color);
        }
    }
}

static void draw_rect(const ast_gfx_t *g, uint16_t *fb, const ast_item_t *it, const ast_rect_t *c)
{
    int xa = it->x0 < c->x0 ? c->x0 : it->x0, xb = it->x1 > c->x1 ? c->x1 : it->x1;
    int ya = it->y0 < c->y0 ? c->y0 : it->y0, yb = it->y1 > c->y1 ? c->y1 : it->y1;
    uint32_t ce = EXP(it->color);
    uint32_t a5 = (uint32_t)(it->a + 4) >> 3;
    bool add = (it->flags & AST_F_ADD) != 0;
    for (int y = ya; y < yb; y++) span(ROW(fb, y), xa, xb, c, ce, a5, add, it->color);
}

static void draw_item(const ast_gfx_t *g, uint16_t *fb, const ast_item_t *it, const ast_rect_t *c)
{
    ast_rect_t cc;
    if (it->kind & KIND_CLIP) {
        cc = *c;
        if (cc.x0 < g->clip.x0) cc.x0 = g->clip.x0;
        if (cc.y0 < g->clip.y0) cc.y0 = g->clip.y0;
        if (cc.x1 > g->clip.x1) cc.x1 = g->clip.x1;
        if (cc.y1 > g->clip.y1) cc.y1 = g->clip.y1;
        if (cc.x0 >= cc.x1 || cc.y0 >= cc.y1) return;
        c = &cc;
    }
    switch (it->kind & 0x7F) {
    case IT_LINE: draw_line(g, fb, it, c); break;
    case IT_GLOW: draw_glow(g, fb, it, c); break;
    case IT_RING: draw_ring(g, fb, it, c, false); break;
    case IT_DISC: draw_ring(g, fb, it, c, true); break;
    case IT_RECT: draw_rect(g, fb, it, c); break;
    default: break;
    }
}

/* ---- the frame ------------------------------------------------------------------------------- */

/* a run of tiles of the strip into the panel's buffer, turned (lying down only:
 * the screen is 1280 x 720 and the panel 720 x 1280) */
static void turn_out(const ast_gfx_t *g, const uint16_t *strip, int x0, int x1, int y0)
{
    uint16_t *fb = g->fb;
    int W = g->W, H = g->H;
    if (g->out == AST_OUT_ROT90) {
        for (int x = x0; x < x1; x++) {
            uint16_t *p = fb + (size_t)x * H + (H - 1 - y0);
            const uint16_t *s = strip + x;
            for (int j = 0; j < AST_TILE; j++) p[-j] = s[(size_t)j * W];
        }
    } else {
        for (int x = x0; x < x1; x++) {
            uint16_t *p = fb + (size_t)(W - 1 - x) * H + y0;
            const uint16_t *s = strip + x;
            for (int j = 0; j < AST_TILE; j++) p[j] = s[(size_t)j * W];
        }
    }
}

static void band(const ast_gfx_t *g, int ty, uint16_t *strip)
{
    const uint8_t *need = g->need + ty * g->TW;
    const uint16_t *ix = g->band_ix + g->band_at[ty];
    int n = g->band_at[ty + 1] - g->band_at[ty];
    bool turned = g->out != AST_OUT_SCREEN;
    int y0 = ty * AST_TILE;
    uint16_t *fb = turned ? strip - (ptrdiff_t)y0 * g->W : g->fb;
    for (int tx = 0; tx < g->TW;) {
        if (!need[tx]) {
            tx++;
            continue;
        }
        int t0 = tx;
        while (tx < g->TW && need[tx]) tx++;
        ast_rect_t c = { (int16_t)(t0 * AST_TILE), (int16_t)y0, (int16_t)(tx * AST_TILE), (int16_t)(y0 + AST_TILE) };
        size_t bytes = (size_t)(c.x1 - c.x0) * 2;
        for (int y = c.y0; y < c.y1; y++) {
            if (!g->bg) memset(ROW(fb, y) + c.x0, 0, bytes);
            else memcpy(ROW(fb, y) + c.x0, g->bg + (size_t)y * g->W + c.x0, bytes);
        }
        for (int i = 0; i < n; i++) {
            const ast_item_t *it = &g->item[ix[i]];
            if (it->x1 <= c.x0 || it->x0 >= c.x1) continue;
            draw_item(g, fb, it, &c);
        }
        if (turned) turn_out(g, strip, c.x0, c.x1, y0);
    }
}

static void half(void *arg, int part)
{
    const ast_gfx_t *g = (const ast_gfx_t *)arg;
    int a = part ? g->split : 0, b = part ? g->TH : g->split;
    for (int ty = a; ty < b; ty++) band(g, ty, g->strip[part ? 1 : 0]);
}

int ast_gfx_end(ast_gfx_t *g, uint16_t *fb, int out, uint32_t *shown, bool full)
{
    int TW = g->TW, TH = g->TH, NT = TW * TH;
    memset(g->cur, 0, sizeof(uint32_t) * (size_t)NT);
    int counts[AST_THMAX];
    memset(counts, 0, sizeof counts);
    for (int i = 0; i < g->nitem; i++) {
        const ast_item_t *it = &g->item[i];
        int tx0 = it->x0 / AST_TILE, tx1 = (it->x1 - 1) / AST_TILE;
        int ty0 = it->y0 / AST_TILE, ty1 = (it->y1 - 1) / AST_TILE;
        for (int ty = ty0; ty <= ty1; ty++) {
            uint32_t *h = g->cur + ty * TW;
            for (int tx = tx0; tx <= tx1; tx++) h[tx] = mix(h[tx], it->hash);
            counts[ty]++;
        }
    }
    if (g->salt) {
        for (int t = 0; t < NT; t++) g->cur[t] = mix(g->cur[t], g->salt);
    }
    int at = 0;
    for (int ty = 0; ty < TH; ty++) {
        g->band_at[ty] = at;
        at += counts[ty];
    }
    g->band_at[TH] = at;
    if (at > g->band_cap) {
        aos_hal_log("asteroides", "gfx: %d band entries, %d fit", at, g->band_cap);
        return 0;
    }
    int fill[AST_THMAX];
    memcpy(fill, g->band_at, sizeof(int) * (size_t)TH);
    for (int i = 0; i < g->nitem; i++) {
        const ast_item_t *it = &g->item[i];
        int ty0 = it->y0 / AST_TILE, ty1 = (it->y1 - 1) / AST_TILE;
        for (int ty = ty0; ty <= ty1; ty++) g->band_ix[fill[ty]++] = (uint16_t)i;
    }
    int total = 0, drawn = 0;
    int per[AST_THMAX];
    for (int ty = 0; ty < TH; ty++) {
        int n = 0;
        for (int tx = 0; tx < TW; tx++) {
            int t = ty * TW + tx;
            bool d = full || g->cur[t] != shown[t];
            g->need[t] = d;
            n += d;
        }
        per[ty] = n ? n * 2 + counts[ty] * 3 : 0;
        total += per[ty];
        drawn += n;
    }
    g->drawn_tiles = drawn;
    if (!drawn) return 0;
    int acc = 0;
    g->split = TH;
    for (int ty = 0; ty < TH; ty++) {
        acc += per[ty];
        if (acc * 2 >= total) {
            g->split = ty + 1;
            break;
        }
    }
    g->fb = fb;
    g->out = out;
    if (g->one_core || total < 60 || g->split >= TH || !aos_hal_worker_split(half, g)) {
        int s = g->split;
        g->split = TH;
        half(g, 0);
        g->split = s;
    }
    memcpy(shown, g->cur, sizeof(uint32_t) * (size_t)NT);
    return drawn;
}
