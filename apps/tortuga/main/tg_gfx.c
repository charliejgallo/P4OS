/*
 * Tortuga - the picture (see tg_gfx.h): Bastion's compositor, with
 * the screen's size a field, one core (it runs on LVGL's thread), no depth
 * and no text, and a filled quadrilateral for a cell seen in perspective.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "tg_gfx.h"

#include "tg_pak.h"

#include <math.h>
#include <string.h>

enum { IT_SPR, IT_GLOW, IT_SHADOW, IT_RECT, IT_QUAD, IT_BEAM, IT_RING, IT_DISC };

#define M565 0x07E0F81Fu
#define EXP(c) ((((uint32_t)(c)) | (((uint32_t)(c)) << 16)) & M565)

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

/* ---- set up --------------------------------------------------------------------- */

bool tg_gfx_init(tg_gfx_t *g)
{
    memset(g, 0, sizeof *g);
    g->item = (tg_item_t *)tg_alloc(sizeof(tg_item_t) * TGX_MAX_ITEMS);
    g->cur = (uint32_t *)tg_alloc(sizeof(uint32_t) * TGX_MAX_TILES);
    g->need = (uint8_t *)tg_alloc(TGX_MAX_TILES);
    g->band_cap = TGX_MAX_ITEMS * 10;
    g->band_ix = (uint16_t *)tg_alloc(sizeof(uint16_t) * (size_t)g->band_cap);
    if (!g->item || !g->cur || !g->need || !g->band_ix) {
        tg_gfx_free(g);
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
    for (int i = 0; i <= 16; i++) {
        float v = 1.0f - (float)i / 16.0f;
        g->prof[i] = (uint8_t)(v * v * 255.0f);
    }
    tg_gfx_size(g, 720, 1280, NULL);
    return true;
}

void tg_gfx_free(tg_gfx_t *g)
{
    tg_free(g->item);
    tg_free(g->cur);
    tg_free(g->need);
    tg_free(g->band_ix);
    memset(g, 0, sizeof *g);
}

void tg_gfx_size(tg_gfx_t *g, int w, int h, const uint16_t *bg)
{
    g->w = w;
    g->h = h;
    g->tw = (w + TGX_TILE - 1) / TGX_TILE;
    g->th = (h + TGX_TILE - 1) / TGX_TILE;
    if (g->tw * g->th > TGX_MAX_TILES) g->th = TGX_MAX_TILES / g->tw;
    if (g->th > TGX_MAX_TH) g->th = TGX_MAX_TH;
    g->nt = g->tw * g->th;
    g->bg = bg;
}

bool tg_sheet_bind(tg_sheet_t *s, uint8_t *data, size_t len)
{
    memset(s, 0, sizeof *s);
    if (!data || len < 8 || memcmp(data, "BSSH", 4)) return false;
    s->data = data;
    s->nframes = data[4] | (data[5] << 8);
    s->cols = data[6] | (data[7] << 8);
    s->fr = (const tg_frame_t *)(data + 8);
    if (s->cols <= 0) s->cols = 1;
    return s->nframes > 0 && 8 + (size_t)s->nframes * sizeof(tg_frame_t) <= len;
}

/* ---- the frame's list ---------------------------------------------------------------- */

void tg_gfx_begin(tg_gfx_t *g)
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

static tg_item_t *push(tg_gfx_t *g, int kind, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g->w) x1 = g->w;
    if (y1 > g->h) y1 = g->h;
    if (x1 <= x0 || y1 <= y0) return NULL;
    if (g->nitem >= TGX_MAX_ITEMS) {
        g->overflow = true;
        return NULL;
    }
    tg_item_t *it = &g->item[g->nitem++];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    it->x0 = (int16_t)x0;
    it->y0 = (int16_t)y0;
    it->x1 = (int16_t)x1;
    it->y1 = (int16_t)y1;
    return it;
}

static void seal(tg_item_t *it, uint32_t extra)
{
    uint32_t h = 0x811C9DC5u;
    h = mix(h, (uint32_t)it->kind | ((uint32_t)it->flags << 8) | ((uint32_t)it->a << 16) | ((uint32_t)it->aux << 24));
    h = mix(h, (uint32_t)(uint16_t)it->x | ((uint32_t)(uint16_t)it->y << 16));
    h = mix(h, (uint32_t)(uint16_t)it->p0 | ((uint32_t)(uint16_t)it->p1 << 16));
    h = mix(h, (uint32_t)(uint16_t)it->p2 | ((uint32_t)(uint16_t)it->p3 << 16));
    h = mix(h, (uint32_t)it->color | ((uint32_t)it->z << 16));
    h = mix(h, extra);
    it->hash = h ? h : 1;
}

void tg_spr(tg_gfx_t *g, const tg_sheet_t *s, int frame, int x, int y, int flags, int alpha)
{
    if (!s || !s->data || alpha <= 0) return;
    if (frame < 0) frame = 0;
    if (frame >= s->nframes) frame %= s->nframes;
    const tg_frame_t *fr = &s->fr[frame];
    if (!fr->w || !fr->h) return;
    tg_item_t *it = push(g, IT_SPR, x + fr->ox, y + fr->oy, x + fr->ox + fr->w, y + fr->oy + fr->h);
    if (!it) return;
    it->flags = (uint8_t)flags;
    it->a = (uint8_t)(alpha > 255 ? 255 : alpha);
    it->x = (int16_t)(x + fr->ox);
    it->y = (int16_t)(y + fr->oy);
    it->p0 = (int16_t)fr->w;
    it->p1 = (int16_t)fr->h;
    it->p3 = (int16_t)frame;
    it->p = s;
    /* the frame's bytes: the same sheet loaded twice may land at the same
     * address with other pixels, so the hash takes both */
    seal(it, (uint32_t)(uintptr_t)(s->data + fr->off) ^ ((uint32_t)fr->off * 2654435761u));
}

void tg_glow(tg_gfx_t *g, int x, int y, int r, uint16_t color, int k)
{
    if (r < 1 || k <= 0) return;
    tg_item_t *it = push(g, IT_GLOW, x - r, y - r, x + r, y + r);
    if (!it) return;
    it->x = (int16_t)x;
    it->y = (int16_t)y;
    it->p0 = (int16_t)r;
    it->color = color;
    it->a = (uint8_t)(k > 255 ? 255 : k);
    seal(it, 0);
}

void tg_shadow(tg_gfx_t *g, int x, int y, int rx, int ry, int a)
{
    if (rx < 2 || ry < 1 || a <= 0) return;
    tg_item_t *it = push(g, IT_SHADOW, x - rx, y - ry, x + rx, y + ry);
    if (!it) return;
    it->x = (int16_t)x;
    it->y = (int16_t)y;
    it->p0 = (int16_t)rx;
    it->p1 = (int16_t)ry;
    it->a = (uint8_t)(a > 255 ? 255 : a);
    seal(it, 0);
}

void tg_rect(tg_gfx_t *g, int x, int y, int w, int h, uint16_t color, int a, int radius)
{
    if (a <= 0) return;
    tg_item_t *it = push(g, IT_RECT, x, y, x + w, y + h);
    if (!it) return;
    it->x = (int16_t)x;
    it->y = (int16_t)y;
    it->p0 = (int16_t)w;
    it->p1 = (int16_t)h;
    it->p2 = (int16_t)radius;
    it->color = color;
    it->a = (uint8_t)(a > 255 ? 255 : a);
    seal(it, 0);
}

void tg_quad(tg_gfx_t *g, const int16_t *xy, uint16_t color, int a)
{
    if (a <= 0) return;
    int x0 = xy[0], x1 = xy[0], y0 = xy[1], y1 = xy[1];
    for (int i = 1; i < 4; i++) {
        if (xy[2 * i] < x0) x0 = xy[2 * i];
        if (xy[2 * i] > x1) x1 = xy[2 * i];
        if (xy[2 * i + 1] < y0) y0 = xy[2 * i + 1];
        if (xy[2 * i + 1] > y1) y1 = xy[2 * i + 1];
    }
    tg_item_t *it = push(g, IT_QUAD, x0, y0, x1 + 1, y1 + 1);
    if (!it) return;
    it->color = color;
    it->a = (uint8_t)(a > 255 ? 255 : a);
    /* the four corners fill x, y, p0..p3, z and the pointer's slot */
    it->x = xy[0];
    it->y = xy[1];
    it->p0 = xy[2];
    it->p1 = xy[3];
    it->p2 = xy[4];
    it->p3 = xy[5];
    it->z = (uint16_t)xy[6];
    it->p = (const void *)(uintptr_t)(uint16_t)xy[7];
    seal(it, (uint32_t)(uint16_t)xy[7] | ((uint32_t)(uint16_t)xy[6] << 16));
}

void tg_beam(tg_gfx_t *g, int x0, int y0, int x1, int y1, int halfw, uint16_t color, int k)
{
    if (halfw < 1 || k <= 0) return;
    int ax = x0 < x1 ? x0 : x1, bx = x0 < x1 ? x1 : x0;
    int ay = y0 < y1 ? y0 : y1, by = y0 < y1 ? y1 : y0;
    int dx = bx - ax, dy = by - ay;
    int pad = halfw + (dx > dy ? (dy * halfw) / (dx ? dx : 1) : (dx * halfw) / (dy ? dy : 1)) + 1;
    tg_item_t *it = push(g, IT_BEAM, ax - pad, ay - pad, bx + pad + 1, by + pad + 1);
    if (!it) return;
    it->x = (int16_t)x0;
    it->y = (int16_t)y0;
    it->p0 = (int16_t)x1;
    it->p1 = (int16_t)y1;
    it->p2 = (int16_t)halfw;
    it->color = color;
    it->a = (uint8_t)(k > 255 ? 255 : k);
    seal(it, 0);
}

void tg_ring(tg_gfx_t *g, int cx, int cy, int rx, int ry, int thick, uint16_t color, int k, int flags)
{
    if (rx < 2 || ry < 2 || k <= 0) return;
    tg_item_t *it = push(g, IT_RING, cx - rx, cy - ry, cx + rx + 1, cy + ry + 1);
    if (!it) return;
    it->x = (int16_t)cx;
    it->y = (int16_t)cy;
    it->p0 = (int16_t)rx;
    it->p1 = (int16_t)ry;
    it->p2 = (int16_t)(thick < 1 ? 1 : thick);
    it->color = color;
    it->flags = (uint8_t)flags;
    it->a = (uint8_t)(k > 255 ? 255 : k);
    seal(it, 0);
}

void tg_disc(tg_gfx_t *g, int cx, int cy, int rx, int ry, uint16_t color, int a)
{
    if (rx < 1 || ry < 1 || a <= 0) return;
    tg_item_t *it = push(g, IT_DISC, cx - rx, cy - ry, cx + rx + 1, cy + ry + 1);
    if (!it) return;
    it->x = (int16_t)cx;
    it->y = (int16_t)cy;
    it->p0 = (int16_t)rx;
    it->p1 = (int16_t)ry;
    it->color = color;
    it->a = (uint8_t)(a > 255 ? 255 : a);
    seal(it, 0);
}

/* ---- drawing one thing, clipped --------------------------------------------------------- */

static void draw_spr(const tg_gfx_t *g, uint16_t *fb, const tg_item_t *it, const tg_rect_t *c)
{
    const tg_sheet_t *s = (const tg_sheet_t *)it->p;
    const tg_frame_t *fr = &s->fr[it->p3];
    const uint32_t *row = (const uint32_t *)(s->data + fr->off);
    int W = g->w;
    int ya = it->y < c->y0 ? c->y0 : it->y;
    int yb = it->y + fr->h > c->y1 ? c->y1 : it->y + fr->h;
    int ga = it->a >> 3;
    bool solid = it->a >= 250 && !(it->flags & TGX_F_FLASH);
    bool flash = (it->flags & TGX_F_FLASH) != 0;
    for (int y = ya; y < yb; y++) {
        const uint16_t *op = (const uint16_t *)(s->data + row[y - it->y]);
        uint16_t *dst = fb + (size_t)y * W;
        int x = it->x;
        for (;;) {
            uint16_t o = *op++;
            int kind = o >> 14, n = o & 0x3FFF;
            if (kind == 0) break;
            if (kind == 1) {
                x += n;
                continue;
            }
            int a = x < c->x0 ? c->x0 : x;
            int b = x + n > c->x1 ? c->x1 : x + n;
            if (kind == 2) {
                const uint16_t *src = op - x;
                if (a < b) {
                    if (solid) {
                        memcpy(dst + a, src + a, (size_t)(b - a) * 2);
                    } else {
                        for (int px = a; px < b; px++) {
                            uint16_t col = src[px];
                            if (flash) col = blend5(col, EXP(0xFFFF), 12);
                            dst[px] = ga >= 31 ? col : blend5(dst[px], EXP(col), (uint32_t)ga);
                        }
                    }
                }
                op += n;
            } else {
                const uint16_t *src = op - 2 * x;
                for (int px = a; px < b; px++) {
                    uint16_t col = src[px * 2];
                    uint32_t al = src[px * 2 + 1];
                    if (flash) col = blend5(col, EXP(0xFFFF), 12);
                    if (!solid) al = (al * it->a) >> 8;
                    dst[px] = blend5(dst[px], EXP(col), (al + 4) >> 3);
                }
                op += n * 2;
            }
            x += n;
            if (x >= c->x1) break;
        }
    }
}

static void draw_glow(const tg_gfx_t *g, uint16_t *fb, const tg_item_t *it, const tg_rect_t *c)
{
    int r = it->p0, W = g->w;
    int xa = it->x0 < c->x0 ? c->x0 : it->x0, xb = it->x1 > c->x1 ? c->x1 : it->x1;
    int ya = it->y0 < c->y0 ? c->y0 : it->y0, yb = it->y1 > c->y1 ? c->y1 : it->y1;
    uint32_t step = (64u << 16) / (uint32_t)(2 * r);
    uint32_t ce = EXP(it->color);
    int ox = it->x - r, oy = it->y - r;
    for (int y = ya; y < yb; y++) {
        const uint8_t *t = g->glow + (((uint32_t)(y - oy) * step) >> 16) * 64;
        uint16_t *dst = fb + (size_t)y * W;
        uint32_t u = (uint32_t)(xa - ox) * step;
        for (int x = xa; x < xb; x++, u += step) {
            uint32_t a = ((uint32_t)t[u >> 16] * it->a) >> 11;
            if (a) dst[x] = add_sat(dst[x], ((ce * a) >> 5) & M565);
        }
    }
}

static void draw_shadow(const tg_gfx_t *g, uint16_t *fb, const tg_item_t *it, const tg_rect_t *c)
{
    int rx = it->p0, ry = it->p1, W = g->w;
    int ya = it->y0 < c->y0 ? c->y0 : it->y0, yb = it->y1 > c->y1 ? c->y1 : it->y1;
    for (int y = ya; y < yb; y++) {
        float dy = ((float)(y - it->y) + 0.5f) / (float)ry;
        float q = 1.0f - dy * dy;
        if (q <= 0) continue;
        int half = (int)((float)rx * sqrtf(q));
        int xa = it->x - half, xb = it->x + half;
        if (xa < c->x0) xa = c->x0;
        if (xb > c->x1) xb = c->x1;
        uint16_t *dst = fb + (size_t)y * W;
        /* soft: darker in the middle, in three steps */
        for (int x = xa; x < xb; x++) {
            float dx = ((float)(x - it->x)) / (float)rx;
            float d = dx * dx + dy * dy;
            uint32_t a = (uint32_t)((float)it->a * (1.0f - d) * (1.0f - d)) >> 3;
            if (!a) continue;
            uint32_t b = EXP(dst[x]);
            b = ((b * (32 - a)) >> 5) & M565;
            dst[x] = (uint16_t)(b | (b >> 16));
        }
    }
}

static void draw_rect(const tg_gfx_t *g, uint16_t *fb, const tg_item_t *it, const tg_rect_t *c)
{
    int w = it->p0, h = it->p1, rad = it->p2, W = g->w;
    int ya = it->y < c->y0 ? c->y0 : it->y, yb = it->y + h > c->y1 ? c->y1 : it->y + h;
    uint32_t ce = EXP(it->color);
    uint32_t a5 = (uint32_t)(it->a + 4) >> 3;
    for (int y = ya; y < yb; y++) {
        int inset = 0;
        int ry = y - it->y;
        int k = ry < rad ? rad - ry : (ry >= h - rad ? ry - (h - rad) + 1 : 0);
        if (k > 0) inset = rad - (int)sqrtf((float)(rad * rad - k * k) + 0.5f);
        int xa = it->x + inset, xb = it->x + w - inset;
        if (xa < c->x0) xa = c->x0;
        if (xb > c->x1) xb = c->x1;
        uint16_t *dst = fb + (size_t)y * W;
        if (a5 >= 31) {
            for (int x = xa; x < xb; x++) dst[x] = it->color;
        } else {
            for (int x = xa; x < xb; x++) dst[x] = blend5(dst[x], ce, a5);
        }
    }
}

static void draw_quad(const tg_gfx_t *g, uint16_t *fb, const tg_item_t *it, const tg_rect_t *c)
{
    float px[4] = { it->x, it->p0, it->p2, (float)(int16_t)it->z };
    float py[4] = { it->y, it->p1, it->p3, (float)(int16_t)(uint16_t)(uintptr_t)it->p };
    int W = g->w;
    uint32_t ce = EXP(it->color);
    uint32_t a5 = (uint32_t)(it->a + 4) >> 3;
    int ya = it->y0 < c->y0 ? c->y0 : it->y0, yb = it->y1 > c->y1 ? c->y1 : it->y1;
    for (int y = ya; y < yb; y++) {
        float yc = (float)y + 0.5f, lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) & 3;
            float y0 = py[i], y1 = py[j];
            if ((yc < y0) == (yc < y1)) continue;
            float x = px[i] + (yc - y0) * (px[j] - px[i]) / (y1 - y0);
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        if (hi < lo) continue;
        int xa = (int)(lo + 0.5f), xb = (int)(hi + 0.5f);
        if (xa < c->x0) xa = c->x0;
        if (xb > c->x1) xb = c->x1;
        uint16_t *dst = fb + (size_t)y * W;
        for (int x = xa; x < xb; x++) dst[x] = a5 >= 31 ? it->color : blend5(dst[x], ce, a5);
    }
}

static void draw_beam(const tg_gfx_t *g, uint16_t *fb, const tg_item_t *it, const tg_rect_t *c)
{
    int x0 = it->x, y0 = it->y, x1 = it->p0, y1 = it->p1, hw = it->p2, W = g->w;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    uint32_t ce = EXP(it->color);
    if (ady >= adx) {
        if (ady == 0) return;
        if (y0 > y1) {
            int t = x0; x0 = x1; x1 = t;
            t = y0; y0 = y1; y1 = t;
            dx = -dx;
            dy = -dy;
        }
        int half = hw + (adx * hw) / (2 * ady) + 1;
        int32_t slope = (dx * 65536) / dy;
        int ya = y0 < c->y0 ? c->y0 : y0, yb = y1 + 1 > c->y1 ? c->y1 : y1 + 1;
        for (int y = ya; y < yb; y++) {
            int xc = x0 + (((y - y0) * slope) >> 16);
            int xa = xc - half, xb = xc + half + 1;
            if (xa < c->x0) xa = c->x0;
            if (xb > c->x1) xb = c->x1;
            uint16_t *dst = fb + (size_t)y * W;
            for (int x = xa; x < xb; x++) {
                int d = x - xc;
                if (d < 0) d = -d;
                uint32_t a = ((uint32_t)g->prof[d * 16 / (half + 1)] * it->a) >> 11;
                if (a) dst[x] = add_sat(dst[x], ((ce * a) >> 5) & M565);
            }
        }
    } else {
        if (x0 > x1) {
            int t = x0; x0 = x1; x1 = t;
            t = y0; y0 = y1; y1 = t;
            dx = -dx;
            dy = -dy;
        }
        int half = hw + (ady * hw) / (2 * adx) + 1;
        int32_t slope = (dy * 65536) / dx;
        int xa = x0 < c->x0 ? c->x0 : x0, xb = x1 + 1 > c->x1 ? c->x1 : x1 + 1;
        for (int x = xa; x < xb; x++) {
            int yc = y0 + (((x - x0) * slope) >> 16);
            int ya = yc - half, yb = yc + half + 1;
            if (ya < c->y0) ya = c->y0;
            if (yb > c->y1) yb = c->y1;
            for (int y = ya; y < yb; y++) {
                int d = y - yc;
                if (d < 0) d = -d;
                uint32_t a = ((uint32_t)g->prof[d * 16 / (half + 1)] * it->a) >> 11;
                uint16_t *dst = fb + (size_t)y * W + x;
                if (a) *dst = add_sat(*dst, ((ce * a) >> 5) & M565);
            }
        }
    }
}

static inline void span(uint16_t *dst, int xa, int xb, const tg_rect_t *c, uint32_t ce, uint32_t a5, bool add, uint16_t color)
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

static void draw_ring(const tg_gfx_t *g, uint16_t *fb, const tg_item_t *it, const tg_rect_t *c, bool fill)
{
    int rx = it->p0, ry = it->p1, th = it->p2, W = g->w;
    int ya = it->y0 < c->y0 ? c->y0 : it->y0, yb = it->y1 > c->y1 ? c->y1 : it->y1;
    uint32_t ce = EXP(it->color);
    uint32_t a5 = (uint32_t)(it->a + 4) >> 3;
    bool add = (it->flags & TGX_F_ADD) != 0;
    int rxi = rx - th, ryi = ry - (th * ry + rx / 2) / rx;
    if (ryi < 1) ryi = 1;
    for (int y = ya; y < yb; y++) {
        float d = ((float)(y - it->y)) / (float)ry;
        float q = 1.0f - d * d;
        if (q < 0) continue;
        int xo = (int)((float)rx * sqrtf(q) + 0.5f);
        uint16_t *dst = fb + (size_t)y * W;
        if (fill) {
            span(dst, it->x - xo, it->x + xo + 1, c, ce, a5, add, it->color);
            continue;
        }
        int xi = -1;
        if (rxi > 0) {
            float di = ((float)(y - it->y)) / (float)ryi;
            float qi = 1.0f - di * di;
            if (qi > 0) xi = (int)((float)rxi * sqrtf(qi) + 0.5f);
        }
        if (xi < 0) {
            span(dst, it->x - xo, it->x + xo + 1, c, ce, a5, add, it->color);
        } else {
            span(dst, it->x - xo, it->x - xi, c, ce, a5, add, it->color);
            span(dst, it->x + xi + 1, it->x + xo + 1, c, ce, a5, add, it->color);
        }
    }
}

static void draw_item(const tg_gfx_t *g, uint16_t *fb, const tg_item_t *it, const tg_rect_t *c)
{
    switch (it->kind) {
    case IT_SPR:    draw_spr(g, fb, it, c); break;
    case IT_GLOW:   draw_glow(g, fb, it, c); break;
    case IT_SHADOW: draw_shadow(g, fb, it, c); break;
    case IT_RECT:   draw_rect(g, fb, it, c); break;
    case IT_QUAD:   draw_quad(g, fb, it, c); break;
    case IT_BEAM:   draw_beam(g, fb, it, c); break;
    case IT_RING:   draw_ring(g, fb, it, c, false); break;
    case IT_DISC:   draw_ring(g, fb, it, c, true); break;
    default: break;
    }
}

/* ---- the frame ------------------------------------------------------------------------- */

int tg_gfx_end(tg_gfx_t *g, uint16_t *fb, uint32_t *shown, bool full)
{
    int TW = g->tw, TH = g->th, W = g->w;
    memset(g->cur, 0, sizeof(uint32_t) * (size_t)g->nt);
    int counts[TGX_MAX_TH];
    memset(counts, 0, sizeof counts);
    for (int i = 0; i < g->nitem; i++) {
        const tg_item_t *it = &g->item[i];
        int tx0 = it->x0 / TGX_TILE, tx1 = (it->x1 - 1) / TGX_TILE;
        int ty0 = it->y0 / TGX_TILE, ty1 = (it->y1 - 1) / TGX_TILE;
        if (ty1 >= TH) ty1 = TH - 1;
        if (tx1 >= TW) tx1 = TW - 1;
        for (int ty = ty0; ty <= ty1; ty++) {
            uint32_t *h = g->cur + ty * TW;
            for (int tx = tx0; tx <= tx1; tx++) h[tx] = mix(h[tx], it->hash);
            counts[ty]++;
        }
    }
    int at = 0;
    for (int ty = 0; ty < TH; ty++) {
        g->band_at[ty] = at;
        at += counts[ty];
    }
    g->band_at[TH] = at;
    if (at > g->band_cap) return 0;
    int fill[TGX_MAX_TH];
    memcpy(fill, g->band_at, sizeof(int) * (size_t)TH);
    for (int i = 0; i < g->nitem; i++) {
        const tg_item_t *it = &g->item[i];
        int ty0 = it->y0 / TGX_TILE, ty1 = (it->y1 - 1) / TGX_TILE;
        if (ty1 >= TH) ty1 = TH - 1;
        for (int ty = ty0; ty <= ty1; ty++) g->band_ix[fill[ty]++] = (uint16_t)i;
    }
    int x0 = TW, x1 = -1, y0 = TH, y1 = -1, drawn = 0;
    for (int ty = 0; ty < TH; ty++) {
        for (int tx = 0; tx < TW; tx++) {
            int t = ty * TW + tx;
            bool d = full || g->cur[t] != shown[t];
            g->need[t] = d;
            if (!d) continue;
            drawn++;
            if (tx < x0) x0 = tx;
            if (tx > x1) x1 = tx;
            if (ty < y0) y0 = ty;
            y1 = ty;
        }
    }
    g->drawn_tiles = drawn;
    if (y1 < 0) {
        g->dirty.x0 = g->dirty.x1 = g->dirty.y0 = g->dirty.y1 = 0;
        return 0;
    }
    g->dirty.x0 = (int16_t)(x0 * TGX_TILE);
    g->dirty.x1 = (int16_t)((x1 + 1) * TGX_TILE > g->w ? g->w : (x1 + 1) * TGX_TILE);
    g->dirty.y0 = (int16_t)(y0 * TGX_TILE);
    g->dirty.y1 = (int16_t)((y1 + 1) * TGX_TILE > g->h ? g->h : (y1 + 1) * TGX_TILE);
    for (int ty = y0; ty <= y1; ty++) {
        const uint8_t *need = g->need + ty * TW;
        const uint16_t *ix = g->band_ix + g->band_at[ty];
        int n = g->band_at[ty + 1] - g->band_at[ty];
        for (int tx = 0; tx < TW;) {
            if (!need[tx]) {
                tx++;
                continue;
            }
            int t0 = tx;
            while (tx < TW && need[tx]) tx++;
            tg_rect_t c = { (int16_t)(t0 * TGX_TILE), (int16_t)(ty * TGX_TILE),
                            (int16_t)(tx * TGX_TILE > g->w ? g->w : tx * TGX_TILE),
                            (int16_t)((ty + 1) * TGX_TILE > g->h ? g->h : (ty + 1) * TGX_TILE) };
            size_t bytes = (size_t)(c.x1 - c.x0) * 2;
            for (int y = c.y0; y < c.y1; y++) {
                if (g->bg) memcpy(fb + (size_t)y * W + c.x0, g->bg + (size_t)y * W + c.x0, bytes);
                else memset(fb + (size_t)y * W + c.x0, 0, bytes);
            }
            for (int i = 0; i < n; i++) {
                const tg_item_t *it = &g->item[ix[i]];
                if (it->x1 <= c.x0 || it->x0 >= c.x1) continue;
                draw_item(g, fb, it, &c);
            }
        }
    }
    memcpy(shown, g->cur, sizeof(uint32_t) * (size_t)g->nt);
    return drawn;
}
