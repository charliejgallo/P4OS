/*
 * HOCKEY - the picture (see hk_gfx.h). The list, the hashes and the
 * drawing of each thing are Bastion's (apps/bastion/main/bs_gfx.c) by way
 * of Metegol's; what is new is the text turned for the far player.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "hk_gfx.h"

#include "hk_pak.h"

#include "aos_hal.h"

#include <math.h>
#include <string.h>

enum { IT_SPR, IT_GLOW, IT_SHADOW, IT_RECT, IT_TEXT, IT_RING, IT_DISC };

#define M565 0x07E0F81Fu
#define EXP(c) ((((uint32_t)(c)) | (((uint32_t)(c)) << 16)) & M565)
/* the row of y in the target */
#define ROW(fb, y) ((fb) + (size_t)(y) * HK_W)

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

bool hk_gfx_init(hk_gfx_t *g)
{
    memset(g, 0, sizeof *g);
    g->item = (hk_item_t *)hk_malloc(sizeof(hk_item_t) * HK_MAX_ITEMS);
    g->text = (char *)hk_malloc(HK_TEXT_ARENA);
    g->cur = (uint32_t *)hk_malloc(sizeof(uint32_t) * HK_NT);
    g->need = (uint8_t *)hk_malloc(HK_NT);
    g->band_cap = HK_MAX_ITEMS * 8;
    g->band_ix = (uint16_t *)hk_malloc(sizeof(uint16_t) * (size_t)g->band_cap);
    if (!g->item || !g->text || !g->cur || !g->need || !g->band_ix) {
        hk_gfx_free(g);
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
    return true;
}

void hk_gfx_free(hk_gfx_t *g)
{
    hk_free(g->item);
    hk_free(g->text);
    hk_free(g->cur);
    hk_free(g->need);
    hk_free(g->band_ix);
    memset(g, 0, sizeof *g);
}

void hk_gfx_world(hk_gfx_t *g, const uint16_t *bg)
{
    g->bg = bg;
}

static uint32_t s_sheet_ids = 1;

bool hk_sheet_bind(hk_sheet_t *s, uint8_t *data, size_t len)
{
    memset(s, 0, sizeof *s);
    if (!data || len < 8 || memcmp(data, "BSSH", 4)) return false;
    s->data = data;
    s->nframes = data[4] | (data[5] << 8);
    s->cols = data[6] | (data[7] << 8);
    s->fr = (const hk_frame_t *)(data + 8);
    s->id = s_sheet_ids++;
    if (s->cols <= 0) s->cols = 1;
    return s->nframes > 0 && 8 + (size_t)s->nframes * sizeof(hk_frame_t) <= len;
}

uint8_t *hk_sheet_encode(const hk_rgba_frame_t *fr, int n, int cols, size_t *len)
{
    /* first the size: the worst case of every row is one op a pixel */
    size_t total = 8 + 12 * (size_t)n;
    for (int f = 0; f < n; f++) total += (size_t)fr[f].h * 4 + (size_t)fr[f].w * fr[f].h * 6 + (size_t)fr[f].h * 4 + 4;
    uint8_t *b = (uint8_t *)hk_malloc(total);
    if (!b) return NULL;
    memcpy(b, "BSSH", 4);
    b[4] = (uint8_t)n;
    b[5] = (uint8_t)(n >> 8);
    b[6] = (uint8_t)cols;
    b[7] = (uint8_t)(cols >> 8);
    size_t at = 8 + 12 * (size_t)n;
    for (int f = 0; f < n; f++) {
        const hk_rgba_frame_t *F = &fr[f];
        hk_frame_t *t = (hk_frame_t *)(b + 8 + 12 * (size_t)f);
        /* the box of what shows */
        int x0 = F->w, x1 = -1, y0 = F->h, y1 = -1;
        for (int y = 0; y < F->h; y++) {
            for (int x = 0; x < F->w; x++) {
                if (F->al[y * F->w + x] >= 8) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
            }
        }
        if (x1 < 0) {
            memset(t, 0, sizeof *t);
            continue;
        }
        int w = x1 - x0 + 1, h = y1 - y0 + 1;
        t->ox = (int16_t)(x0 - F->ox);
        t->oy = (int16_t)(y0 - F->oy);
        t->w = (uint16_t)w;
        t->h = (uint16_t)h;
        t->off = (uint32_t)at;
        uint32_t *rows = (uint32_t *)(b + at);
        size_t op = at + 4 * (size_t)h;
        for (int y = 0; y < h; y++) {
            rows[y] = (uint32_t)op;
            const uint8_t *al = F->al + (size_t)(y + y0) * F->w + x0;
            const uint16_t *px = F->px + (size_t)(y + y0) * F->w + x0;
            int x = 0;
            while (x < w) {
                int k = al[x] < 8 ? 0 : al[x] >= 248 ? 1 : 2;
                int e = x + 1;
                while (e < w && e - x < 0x3FFF) {
                    int k2 = al[e] < 8 ? 0 : al[e] >= 248 ? 1 : 2;
                    if (k2 != k) break;
                    e++;
                }
                int cnt = e - x;
                uint16_t *o = (uint16_t *)(b + op);
                if (k == 0) {
                    if (e == w) break;
                    *o = (uint16_t)((1 << 14) | cnt);
                    op += 2;
                } else if (k == 1) {
                    *o++ = (uint16_t)((2 << 14) | cnt);
                    memcpy(o, px + x, (size_t)cnt * 2);
                    op += 2 + (size_t)cnt * 2;
                } else {
                    *o++ = (uint16_t)((3 << 14) | cnt);
                    for (int i = 0; i < cnt; i++) {
                        o[i * 2] = px[x + i];
                        o[i * 2 + 1] = al[x + i];
                    }
                    op += 2 + (size_t)cnt * 4;
                }
                x = e;
            }
            *(uint16_t *)(b + op) = 0;
            op += 2;
        }
        if (op & 2) {
            *(uint16_t *)(b + op) = 0;
            op += 2;
        }
        at = op;
    }
    if (len) *len = at;
    return b;
}

bool hk_font_bind(hk_font_t *f, uint8_t *data, size_t len)
{
    memset(f, 0, sizeof *f);
    if (!data || len < 16 || memcmp(data, "BSFN", 4)) return false;
    const uint16_t *h = (const uint16_t *)(data + 4);
    f->data = data;
    f->n = h[0];
    f->line_h = h[1];
    f->base = h[2];
    f->aw = h[3];
    f->ah = h[4];
    f->g = (const hk_glyph_t *)(data + 16);
    f->atlas = data + 16 + (size_t)f->n * sizeof(hk_glyph_t);
    for (int i = 0; i < 256; i++) f->idx[i] = -1;
    for (int i = 0; i < f->n; i++) {
        if (f->g[i].cp < 256) f->idx[f->g[i].cp] = (int16_t)i;
    }
    return 16 + (size_t)f->n * sizeof(hk_glyph_t) + (size_t)f->aw * f->ah <= len;
}

/* the next character of a UTF-8 string, as Latin-1 ('?' for the rest) */
static int next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++;
    if (c >= 0xC0 && c < 0xE0 && (*p & 0xC0) == 0x80) {
        c = ((c & 0x1F) << 6) | (*p++ & 0x3F);
    } else if (c >= 0xE0) {
        while ((*p & 0xC0) == 0x80) p++;
        c = '?';
    }
    *s = (const char *)p;
    return c < 256 ? c : '?';
}

int hk_text_width(const hk_font_t *f, const char *s)
{
    int w = 0;
    if (!f->data) return 0;
    while (*s) {
        int i = f->idx[next_cp(&s)];
        if (i >= 0) w += f->g[i].adv;
    }
    return w;
}

/* ---- the frame's list ---------------------------------------------------------------- */

void hk_gfx_begin(hk_gfx_t *g)
{
    g->nitem = 0;
    g->ntext = 0;
    g->overflow = false;
}

static uint32_t mix(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 0x01000193u;
    h ^= h >> 15;
    return h;
}

static hk_item_t *push(hk_gfx_t *g, int kind, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > HK_W) x1 = HK_W;
    if (y1 > HK_H) y1 = HK_H;
    if (x1 <= x0 || y1 <= y0) return NULL;
    if (g->nitem >= HK_MAX_ITEMS) {
        g->overflow = true;
        return NULL;
    }
    hk_item_t *it = &g->item[g->nitem++];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    it->x0 = (int16_t)x0;
    it->y0 = (int16_t)y0;
    it->x1 = (int16_t)x1;
    it->y1 = (int16_t)y1;
    return it;
}

static void seal(hk_item_t *it)
{
    uint32_t h = 0x811C9DC5u;
    h = mix(h, (uint32_t)it->kind | ((uint32_t)it->flags << 8) | ((uint32_t)it->a << 16) | ((uint32_t)it->aux << 24));
    h = mix(h, (uint32_t)(uint16_t)it->x | ((uint32_t)(uint16_t)it->y << 16));
    h = mix(h, (uint32_t)(uint16_t)it->p0 | ((uint32_t)(uint16_t)it->p1 << 16));
    h = mix(h, (uint32_t)(uint16_t)it->p2 | ((uint32_t)(uint16_t)it->p3 << 16));
    h = mix(h, (uint32_t)it->color);
    if (it->kind == IT_TEXT) {
        for (const char *s = (const char *)it->p; *s; s++) h = mix(h, (uint8_t)*s);
        h = mix(h, (uint32_t)it->hash);
    } else {
        h = mix(h, (uint32_t)(uintptr_t)it->p);
    }
    it->hash = h ? h : 1;
}

void hk_spr(hk_gfx_t *g, const hk_sheet_t *s, int frame, int x, int y, int flags, int alpha)
{
    if (!s || !s->data || alpha <= 0) return;
    if (frame < 0) frame = 0;
    if (frame >= s->nframes) frame %= s->nframes;
    const hk_frame_t *fr = &s->fr[frame];
    if (!fr->w || !fr->h) return;
    hk_item_t *it = push(g, IT_SPR, x + fr->ox, y + fr->oy, x + fr->ox + fr->w, y + fr->oy + fr->h);
    if (!it) return;
    it->flags = (uint8_t)flags;
    it->a = (uint8_t)(alpha > 255 ? 255 : alpha);
    it->x = (int16_t)(x + fr->ox);
    it->y = (int16_t)(y + fr->oy);
    it->p0 = (int16_t)fr->w;
    it->p1 = (int16_t)fr->h;
    it->p2 = (int16_t)frame;
    it->p = (const void *)(uintptr_t)s->id;
    seal(it);
    it->p = s;
    it->p3 = (int16_t)frame;
}

void hk_glow(hk_gfx_t *g, int x, int y, int r, uint16_t color, int k)
{
    if (r < 1 || k <= 0) return;
    hk_item_t *it = push(g, IT_GLOW, x - r, y - r, x + r, y + r);
    if (!it) return;
    it->x = (int16_t)x;
    it->y = (int16_t)y;
    it->p0 = (int16_t)r;
    it->color = color;
    it->a = (uint8_t)(k > 255 ? 255 : k);
    seal(it);
}

void hk_shadow(hk_gfx_t *g, int x, int y, int rx, int ry, int a)
{
    if (rx < 2 || ry < 1 || a <= 0) return;
    hk_item_t *it = push(g, IT_SHADOW, x - rx, y - ry, x + rx, y + ry);
    if (!it) return;
    it->x = (int16_t)x;
    it->y = (int16_t)y;
    it->p0 = (int16_t)rx;
    it->p1 = (int16_t)ry;
    it->a = (uint8_t)(a > 255 ? 255 : a);
    seal(it);
}

void hk_rect(hk_gfx_t *g, int x, int y, int w, int h, uint16_t color, int a, int radius)
{
    if (a <= 0) return;
    hk_item_t *it = push(g, IT_RECT, x, y, x + w, y + h);
    if (!it) return;
    it->x = (int16_t)x;
    it->y = (int16_t)y;
    it->p0 = (int16_t)w;
    it->p1 = (int16_t)h;
    it->p2 = (int16_t)radius;
    it->color = color;
    it->a = (uint8_t)(a > 255 ? 255 : a);
    seal(it);
}

int hk_text(hk_gfx_t *g, const hk_font_t *f, int x, int y, const char *s, uint16_t color, int flags)
{
    if (!f || !f->data || !s || !*s) return 0;
    int w = hk_text_width(f, s);
    if (flags & HK_F_CENTER) x -= w / 2;
    else if (flags & HK_F_RIGHT) x -= w;
    size_t n = strlen(s) + 1;
    if (g->ntext + (int)n > HK_TEXT_ARENA) return w;
    hk_item_t *it = push(g, IT_TEXT, x - 3, y - 3, x + w + 4, y + f->line_h + 4);
    if (!it) return w;
    char *copy = g->text + g->ntext;
    memcpy(copy, s, n);
    g->ntext += (int)n;
    it->x = (int16_t)x;
    it->y = (int16_t)y;
    it->color = color;
    it->flags = (uint8_t)flags;
    it->p = copy;
    it->hash = (uint32_t)(uintptr_t)f;
    it->a = 255;
    seal(it);
    it->p0 = (int16_t)(copy - g->text);
    it->p1 = (int16_t)w;
    it->p = f;
    return w;
}

void hk_ring(hk_gfx_t *g, int cx, int cy, int rx, int ry, int thick, uint16_t color, int k, int flags)
{
    if (rx < 2 || ry < 2 || k <= 0) return;
    hk_item_t *it = push(g, IT_RING, cx - rx, cy - ry, cx + rx + 1, cy + ry + 1);
    if (!it) return;
    it->x = (int16_t)cx;
    it->y = (int16_t)cy;
    it->p0 = (int16_t)rx;
    it->p1 = (int16_t)ry;
    it->p2 = (int16_t)(thick < 1 ? 1 : thick);
    it->color = color;
    it->flags = (uint8_t)flags;
    it->a = (uint8_t)(k > 255 ? 255 : k);
    seal(it);
}

void hk_disc(hk_gfx_t *g, int cx, int cy, int rx, int ry, uint16_t color, int a)
{
    if (rx < 1 || ry < 1 || a <= 0) return;
    hk_item_t *it = push(g, IT_DISC, cx - rx, cy - ry, cx + rx + 1, cy + ry + 1);
    if (!it) return;
    it->x = (int16_t)cx;
    it->y = (int16_t)cy;
    it->p0 = (int16_t)rx;
    it->p1 = (int16_t)ry;
    it->color = color;
    it->a = (uint8_t)(a > 255 ? 255 : a);
    seal(it);
}

/* ---- drawing one thing, clipped --------------------------------------------------------- */

static void draw_spr(uint16_t *fb, const hk_item_t *it, const hk_rect_t *c)
{
    const hk_sheet_t *s = (const hk_sheet_t *)it->p;
    const hk_frame_t *fr = &s->fr[it->p3];
    const uint32_t *row = (const uint32_t *)(s->data + fr->off);
    int ya = it->y < c->y0 ? c->y0 : it->y;
    int yb = it->y + fr->h > c->y1 ? c->y1 : it->y + fr->h;
    int ga = it->a >> 3;
    bool solid = it->a >= 250 && !(it->flags & HK_F_FLASH);
    bool flash = (it->flags & HK_F_FLASH) != 0;
    for (int y = ya; y < yb; y++) {
        const uint16_t *op = (const uint16_t *)(s->data + row[y - it->y]);
        uint16_t *dst = ROW(fb, y);
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
                            if (flash) col = blend5(col, EXP(0xFFFF), 20);
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
                    if (flash) col = blend5(col, EXP(0xFFFF), 20);
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

static void draw_glow(const hk_gfx_t *g, uint16_t *fb, const hk_item_t *it, const hk_rect_t *c)
{
    int r = it->p0;
    int xa = it->x0 < c->x0 ? c->x0 : it->x0, xb = it->x1 > c->x1 ? c->x1 : it->x1;
    int ya = it->y0 < c->y0 ? c->y0 : it->y0, yb = it->y1 > c->y1 ? c->y1 : it->y1;
    uint32_t step = (64u << 16) / (uint32_t)(2 * r);
    uint32_t ce = EXP(it->color);
    int ox = it->x - r, oy = it->y - r;
    for (int y = ya; y < yb; y++) {
        const uint8_t *t = g->glow + (((uint32_t)(y - oy) * step) >> 16) * 64;
        uint16_t *dst = ROW(fb, y);
        uint32_t u = (uint32_t)(xa - ox) * step;
        for (int x = xa; x < xb; x++, u += step) {
            uint32_t a = ((uint32_t)t[u >> 16] * it->a) >> 11;      /* 0..31 */
            if (a) dst[x] = add_sat(dst[x], ((ce * a) >> 5) & M565);
        }
    }
}

static void draw_shadow(uint16_t *fb, const hk_item_t *it, const hk_rect_t *c)
{
    int rx = it->p0, ry = it->p1;
    int ya = it->y0 < c->y0 ? c->y0 : it->y0, yb = it->y1 > c->y1 ? c->y1 : it->y1;
    for (int y = ya; y < yb; y++) {
        float dy = ((float)(y - it->y) + 0.5f) / (float)ry;
        float q = 1.0f - dy * dy;
        if (q <= 0) continue;
        int half = (int)((float)rx * sqrtf(q));
        int xa = it->x - half, xb = it->x + half;
        if (xa < c->x0) xa = c->x0;
        if (xb > c->x1) xb = c->x1;
        uint16_t *dst = ROW(fb, y);
        /* darker in the middle: two steps are enough at this size */
        int a_in = it->a >> 3, a_out = it->a >> 4;
        int inner = half * 2 / 3;
        for (int x = xa; x < xb; x++) {
            int d = x - it->x;
            if (d < 0) d = -d;
            uint32_t a = (uint32_t)(d < inner && q > 0.35f ? a_in : a_out);
            uint32_t b = EXP(dst[x]);
            b = ((b * (32 - a)) >> 5) & M565;
            dst[x] = (uint16_t)(b | (b >> 16));
        }
    }
}

static void draw_rect(uint16_t *fb, const hk_item_t *it, const hk_rect_t *c)
{
    int w = it->p0, h = it->p1, rad = it->p2;
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
        uint16_t *dst = ROW(fb, y);
        if (a5 >= 31) {
            for (int x = xa; x < xb; x++) dst[x] = it->color;
        } else {
            for (int x = xa; x < xb; x++) dst[x] = blend5(dst[x], ce, a5);
        }
    }
}

static void glyphs(const hk_gfx_t *g, uint16_t *fb, const hk_item_t *it, const hk_rect_t *c, int dx, int dy, uint16_t color)
{
    const hk_font_t *f = (const hk_font_t *)it->p;
    const char *s = g->text + it->p0;
    bool rot = (it->flags & HK_F_ROT) != 0;
    /* turned, a glyph's box is mirrored inside the text's own box */
    int bx0 = it->x, bx1 = it->x + it->p1, by0 = it->y, by1 = it->y + f->line_h;
    if (rot) {
        dx = -dx;
        dy = -dy;
    }
    int x = it->x + dx, y0 = it->y + dy;
    uint32_t ce = EXP(color);
    while (*s) {
        int i = f->idx[next_cp(&s)];
        if (i < 0) continue;
        const hk_glyph_t *gl = &f->g[i];
        int gx = x + gl->ox, gy = y0 + gl->oy;
        x += gl->adv;
        if (rot) {
            gx = bx0 + bx1 - (gx + gl->w);
            gy = by0 + by1 - (gy + gl->h);
        } else if (gx >= c->x1) {
            break;
        }
        if (gx >= c->x1 || gx + gl->w <= c->x0 || gy >= c->y1 || gy + gl->h <= c->y0) continue;
        int xa = gx < c->x0 ? c->x0 : gx, xb = gx + gl->w > c->x1 ? c->x1 : gx + gl->w;
        int ya = gy < c->y0 ? c->y0 : gy, yb = gy + gl->h > c->y1 ? c->y1 : gy + gl->h;
        for (int y = ya; y < yb; y++) {
            uint16_t *dst = ROW(fb, y);
            if (!rot) {
                const uint8_t *src = f->atlas + (size_t)(gl->y + y - gy) * f->aw + gl->x - gx;
                for (int px = xa; px < xb; px++) {
                    uint32_t a = src[px];
                    if (a >= 248) dst[px] = color;
                    else if (a >= 8) dst[px] = blend5(dst[px], ce, a >> 3);
                }
            } else {
                /* row y of the turned glyph is its row h-1-(y-gy), read backwards */
                const uint8_t *src = f->atlas + (size_t)(gl->y + gl->h - 1 - (y - gy)) * f->aw + gl->x + gl->w - 1 + gx;
                for (int px = xa; px < xb; px++) {
                    uint32_t a = src[-px];
                    if (a >= 248) dst[px] = color;
                    else if (a >= 8) dst[px] = blend5(dst[px], ce, a >> 3);
                }
            }
        }
    }
}

static void draw_text(const hk_gfx_t *g, uint16_t *fb, const hk_item_t *it, const hk_rect_t *c)
{
    if (it->flags & HK_F_SHADOW) glyphs(g, fb, it, c, 2, 2, 0x0000);
    glyphs(g, fb, it, c, 0, 0, it->color);
}

static inline void span(uint16_t *dst, int xa, int xb, const hk_rect_t *c, uint32_t ce, uint32_t a5, bool add, uint16_t color)
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

static void draw_ring(uint16_t *fb, const hk_item_t *it, const hk_rect_t *c, bool fill)
{
    int rx = it->p0, ry = it->p1, th = it->p2;
    int ya = it->y0 < c->y0 ? c->y0 : it->y0, yb = it->y1 > c->y1 ? c->y1 : it->y1;
    uint32_t ce = EXP(it->color);
    uint32_t a5 = (uint32_t)(it->a + 4) >> 3;
    bool add = (it->flags & HK_F_ADD) != 0;
    int rxi = rx - th, ryi = ry - (th * ry + rx / 2) / rx;
    if (ryi < 1) ryi = 1;
    for (int y = ya; y < yb; y++) {
        float d = ((float)(y - it->y)) / (float)ry;
        float q = 1.0f - d * d;
        if (q < 0) continue;
        int xo = (int)((float)rx * sqrtf(q) + 0.5f);
        uint16_t *dst = ROW(fb, y);
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

static void draw_item(const hk_gfx_t *g, uint16_t *fb, const hk_item_t *it, const hk_rect_t *c)
{
    switch (it->kind) {
    case IT_SPR:    draw_spr(fb, it, c); break;
    case IT_GLOW:   draw_glow(g, fb, it, c); break;
    case IT_SHADOW: draw_shadow(fb, it, c); break;
    case IT_RECT:   draw_rect(fb, it, c); break;
    case IT_TEXT:   draw_text(g, fb, it, c); break;
    case IT_RING:   draw_ring(fb, it, c, false); break;
    case IT_DISC:   draw_ring(fb, it, c, true); break;
    default: break;
    }
}

/* ---- the frame ------------------------------------------------------------------------- */

static void band(const hk_gfx_t *g, int ty)
{
    const uint8_t *need = g->need + ty * HK_TW;
    const uint16_t *ix = g->band_ix + g->band_at[ty];
    int n = g->band_at[ty + 1] - g->band_at[ty];
    int y0 = ty * HK_TILE;
    uint16_t *fb = g->fb;
    for (int tx = 0; tx < HK_TW;) {
        if (!need[tx]) {
            tx++;
            continue;
        }
        int t0 = tx;
        while (tx < HK_TW && need[tx]) tx++;
        hk_rect_t c = { (int16_t)(t0 * HK_TILE), (int16_t)y0, (int16_t)(tx * HK_TILE), (int16_t)(y0 + HK_TILE) };
        size_t bytes = (size_t)(c.x1 - c.x0) * 2;
        for (int y = c.y0; y < c.y1; y++) {
            if (!g->bg) {
                memset(ROW(fb, y) + c.x0, 0, bytes);
            } else if (!g->bg_dx && !g->bg_dy) {
                memcpy(ROW(fb, y) + c.x0, g->bg + (size_t)y * HK_W + c.x0, bytes);
            } else {
                int sy = y - g->bg_dy;
                sy = sy < 0 ? 0 : sy >= HK_H ? HK_H - 1 : sy;
                const uint16_t *src = g->bg + (size_t)sy * HK_W;
                uint16_t *dst = ROW(fb, y);
                for (int x = c.x0; x < c.x1; x++) {
                    int sx = x - g->bg_dx;
                    dst[x] = src[sx < 0 ? 0 : sx >= HK_W ? HK_W - 1 : sx];
                }
            }
        }
        for (int i = 0; i < n; i++) {
            const hk_item_t *it = &g->item[ix[i]];
            if (it->x1 <= c.x0 || it->x0 >= c.x1) continue;
            draw_item(g, fb, it, &c);
        }
    }
}

static void half(void *arg, int part)
{
    const hk_gfx_t *g = (const hk_gfx_t *)arg;
    int a = part ? g->split : 0, b = part ? HK_TH : g->split;
    for (int ty = a; ty < b; ty++) band(g, ty);
}

int hk_gfx_end(hk_gfx_t *g, uint16_t *fb, uint32_t *shown, bool full)
{
    memset(g->cur, 0, sizeof(uint32_t) * HK_NT);
    int counts[HK_TH];
    memset(counts, 0, sizeof counts);
    for (int i = 0; i < g->nitem; i++) {
        const hk_item_t *it = &g->item[i];
        int tx0 = it->x0 / HK_TILE, tx1 = (it->x1 - 1) / HK_TILE;
        int ty0 = it->y0 / HK_TILE, ty1 = (it->y1 - 1) / HK_TILE;
        for (int ty = ty0; ty <= ty1; ty++) {
            uint32_t *h = g->cur + ty * HK_TW;
            uint32_t v = it->hash;
            for (int tx = tx0; tx <= tx1; tx++) h[tx] = mix(h[tx], v);
            counts[ty]++;
        }
    }
    if (g->salt) {
        for (int t = 0; t < HK_NT; t++) g->cur[t] = mix(g->cur[t], g->salt);
    }
    int at = 0;
    for (int ty = 0; ty < HK_TH; ty++) {
        g->band_at[ty] = at;
        at += counts[ty];
    }
    g->band_at[HK_TH] = at;
    if (at > g->band_cap) {
        aos_hal_log("hockey", "gfx: %d band entries, %d fit", at, g->band_cap);
        return 0;
    }
    int fill[HK_TH];
    memcpy(fill, g->band_at, sizeof fill);
    for (int i = 0; i < g->nitem; i++) {
        const hk_item_t *it = &g->item[i];
        int ty0 = it->y0 / HK_TILE, ty1 = (it->y1 - 1) / HK_TILE;
        for (int ty = ty0; ty <= ty1; ty++) g->band_ix[fill[ty]++] = (uint16_t)i;
    }
    int total = 0, drawn = 0;
    int per[HK_TH];
    for (int ty = 0; ty < HK_TH; ty++) {
        int n = 0;
        for (int tx = 0; tx < HK_TW; tx++) {
            int t = ty * HK_TW + tx;
            bool d = full || g->cur[t] != shown[t];
            g->need[t] = d;
            n += d;
        }
        per[ty] = n ? n * 2 + counts[ty] : 0;
        total += per[ty];
        drawn += n;
    }
    g->drawn_tiles = drawn;
    if (!drawn) return 0;
    int acc = 0;
    g->split = HK_TH;
    for (int ty = 0; ty < HK_TH; ty++) {
        acc += per[ty];
        if (acc * 2 >= total) {
            g->split = ty + 1;
            break;
        }
    }
    g->fb = fb;
    if (g->one_core || total < 60 || g->split >= HK_TH || !aos_hal_worker_split(half, g)) {
        int s = g->split;
        g->split = HK_TH;
        half(g, 0);
        g->split = s;
    }
    memcpy(shown, g->cur, sizeof(uint32_t) * HK_NT);
    return drawn;
}
