/*
 * BASTION - the picture (see bs_gfx.h)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "bs_gfx.h"

#include "bs_pak.h"

#include "aos_hal.h"

#include <math.h>
#include <string.h>

enum { IT_SPR, IT_GLOW, IT_SHADOW, IT_RECT, IT_TEXT, IT_BEAM, IT_RING, IT_DISC };

#define M565 0x07E0F81Fu
#define EXP(c) ((((uint32_t)(c)) | (((uint32_t)(c)) << 16)) & M565)
#define Z_BIAS 380          /* a third of a metre: a sprite is never hidden by the ground it stands on */
#define Z_ROW_UP 34         /* depth units a row above the foot (a metre of height is 24 rows, 0.8 m nearer) */
#define Z_ROW_DOWN 19       /* and a row below it, along the ground */

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

bool bs_gfx_init(bs_gfx_t *g)
{
    memset(g, 0, sizeof *g);
    g->item = (bs_item_t *)bs_malloc(sizeof(bs_item_t) * BS_MAX_ITEMS);
    g->text = (char *)bs_malloc(BS_TEXT_ARENA);
    g->cur = (uint32_t *)bs_malloc(sizeof(uint32_t) * BS_NT);
    g->need = (uint8_t *)bs_malloc(BS_NT);
    g->zmin = (uint16_t *)bs_malloc(sizeof(uint16_t) * BS_NT);
    g->band_cap = BS_MAX_ITEMS * 8;
    g->band_ix = (uint16_t *)bs_malloc(sizeof(uint16_t) * (size_t)g->band_cap);
    if (!g->item || !g->text || !g->cur || !g->need || !g->zmin || !g->band_ix) {
        bs_gfx_free(g);
        return false;
    }
    /* one soft light, scaled to whatever radius is asked */
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
    memset(g->zmin, 0xFF, sizeof(uint16_t) * BS_NT);
    return true;
}

void bs_gfx_free(bs_gfx_t *g)
{
    bs_free(g->item);
    bs_free(g->text);
    bs_free(g->cur);
    bs_free(g->need);
    bs_free(g->zmin);
    bs_free(g->band_ix);
    memset(g, 0, sizeof *g);
}

void bs_gfx_world(bs_gfx_t *g, const uint16_t *bg, const uint16_t *depth)
{
    g->bg = bg;
    g->depth = depth;
    if (!depth) {
        memset(g->zmin, 0xFF, sizeof(uint16_t) * BS_NT);
        return;
    }
    for (int ty = 0; ty < BS_TH; ty++) {
        for (int tx = 0; tx < BS_TW; tx++) {
            uint16_t m = 0xFFFF;
            for (int y = 0; y < BS_TILE; y++) {
                const uint16_t *r = depth + (size_t)(ty * BS_TILE + y) * BS_W + tx * BS_TILE;
                for (int x = 0; x < BS_TILE; x++) {
                    if (r[x] < m) m = r[x];
                }
            }
            g->zmin[ty * BS_TW + tx] = m;
        }
    }
}

bool bs_sheet_bind(bs_sheet_t *s, uint8_t *data, size_t len)
{
    memset(s, 0, sizeof *s);
    if (!data || len < 8 || memcmp(data, "BSSH", 4)) return false;
    s->data = data;
    s->nframes = data[4] | (data[5] << 8);
    s->cols = data[6] | (data[7] << 8);
    s->fr = (const bs_frame_t *)(data + 8);
    if (s->cols <= 0) s->cols = 1;
    return s->nframes > 0 && 8 + (size_t)s->nframes * sizeof(bs_frame_t) <= len;
}

static uint16_t hue_px(uint16_t c, const int32_t *m)
{
    int r = (c >> 11) << 3, gg = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
    int r2 = (r * m[0] + gg * m[1] + b * m[2]) >> 12;
    int g2 = (r * m[3] + gg * m[4] + b * m[5]) >> 12;
    int b2 = (r * m[6] + gg * m[7] + b * m[8]) >> 12;
    if (r2 < 0) r2 = 0;
    if (r2 > 255) r2 = 255;
    if (g2 < 0) g2 = 0;
    if (g2 > 255) g2 = 255;
    if (b2 < 0) b2 = 0;
    if (b2 > 255) b2 = 255;
    return (uint16_t)(((r2 >> 3) << 11) | ((g2 >> 2) << 5) | (b2 >> 3));
}

void bs_sheet_hue(bs_sheet_t *s, int degrees)
{
    if (!s->data || degrees == 0) return;
    /* a turn about the grey axis: greys stay grey, colours go round */
    float a = (float)degrees * 3.14159265f / 180.0f;
    float c = cosf(a), sn = sinf(a);
    float k = (1.0f - c) / 3.0f, q = sn * 0.57735027f;
    float mf[9] = { c + k, k - q, k + q, k + q, c + k, k - q, k - q, k + q, c + k };
    int32_t m[9];
    for (int i = 0; i < 9; i++) m[i] = (int32_t)(mf[i] * 4096.0f);
    for (int f = 0; f < s->nframes; f++) {
        const bs_frame_t *fr = &s->fr[f];
        const uint32_t *row = (const uint32_t *)(s->data + fr->off);
        for (int y = 0; y < fr->h; y++) {
            uint16_t *op = (uint16_t *)(s->data + row[y]);
            for (;;) {
                uint16_t o = *op++;
                int kind = o >> 14, n = o & 0x3FFF;
                if (kind == 0) break;
                if (kind == 2) {
                    for (int i = 0; i < n; i++) op[i] = hue_px(op[i], m);
                    op += n;
                } else if (kind == 3) {
                    for (int i = 0; i < n; i++) op[i * 2] = hue_px(op[i * 2], m);
                    op += n * 2;
                }
            }
        }
    }
}

bool bs_font_bind(bs_font_t *f, uint8_t *data, size_t len)
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
    f->g = (const bs_glyph_t *)(data + 16);
    f->atlas = data + 16 + (size_t)f->n * sizeof(bs_glyph_t);
    for (int i = 0; i < 256; i++) f->idx[i] = -1;
    for (int i = 0; i < f->n; i++) {
        if (f->g[i].cp < 256) f->idx[f->g[i].cp] = (int16_t)i;
    }
    return 16 + (size_t)f->n * sizeof(bs_glyph_t) + (size_t)f->aw * f->ah <= len;
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

int bs_text_width(const bs_font_t *f, const char *s)
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

void bs_gfx_begin(bs_gfx_t *g)
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

static bs_item_t *push(bs_gfx_t *g, int kind, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > BS_W) x1 = BS_W;
    if (y1 > BS_H) y1 = BS_H;
    if (x1 <= x0 || y1 <= y0) return NULL;
    if (g->nitem >= BS_MAX_ITEMS) {
        g->overflow = true;
        return NULL;
    }
    bs_item_t *it = &g->item[g->nitem++];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    it->x0 = (int16_t)x0;
    it->y0 = (int16_t)y0;
    it->x1 = (int16_t)x1;
    it->y1 = (int16_t)y1;
    return it;
}

static void seal(bs_item_t *it)
{
    uint32_t h = 0x811C9DC5u;
    h = mix(h, (uint32_t)it->kind | ((uint32_t)it->flags << 8) | ((uint32_t)it->a << 16) | ((uint32_t)it->aux << 24));
    h = mix(h, (uint32_t)(uint16_t)it->x | ((uint32_t)(uint16_t)it->y << 16));
    h = mix(h, (uint32_t)(uint16_t)it->p0 | ((uint32_t)(uint16_t)it->p1 << 16));
    h = mix(h, (uint32_t)(uint16_t)it->p2 | ((uint32_t)(uint16_t)it->p3 << 16));
    h = mix(h, (uint32_t)it->color | ((uint32_t)it->z << 16));
    h = mix(h, (uint32_t)(uint16_t)it->foot);
    if (it->kind == IT_TEXT) {
        for (const char *s = (const char *)it->p; *s; s++) h = mix(h, (uint8_t)*s);
        h = mix(h, (uint32_t)it->hash);
    } else {
        h = mix(h, (uint32_t)(uintptr_t)it->p);
    }
    it->hash = h ? h : 1;
}

void bs_spr(bs_gfx_t *g, const bs_sheet_t *s, int frame, int x, int y, int flags, int alpha, uint16_t z, int foot)
{
    if (!s || !s->data || alpha <= 0) return;
    if (frame < 0) frame = 0;
    if (frame >= s->nframes) frame %= s->nframes;
    const bs_frame_t *fr = &s->fr[frame];
    if (!fr->w || !fr->h) return;
    bs_item_t *it = push(g, IT_SPR, x + fr->ox, y + fr->oy, x + fr->ox + fr->w, y + fr->oy + fr->h);
    if (!it) return;
    it->flags = (uint8_t)flags;
    it->a = (uint8_t)(alpha > 255 ? 255 : alpha);
    it->x = (int16_t)(x + fr->ox);
    it->y = (int16_t)(y + fr->oy);
    it->z = z;
    it->foot = (int16_t)foot;
    it->p = s->data + fr->off;
    it->p0 = (int16_t)fr->w;
    it->p1 = (int16_t)fr->h;
    /* the rows are found from the sheet's start */
    it->p2 = 0;
    seal(it);
    /* the frame's own bytes are fixed, but two sheets may be loaded at the
     * same address in different battles: nothing else to hash */
    it->p = s;
    it->p3 = (int16_t)frame;
}

void bs_glow(bs_gfx_t *g, int x, int y, int r, uint16_t color, int k)
{
    if (r < 1 || k <= 0) return;
    bs_item_t *it = push(g, IT_GLOW, x - r, y - r, x + r, y + r);
    if (!it) return;
    it->x = (int16_t)x;
    it->y = (int16_t)y;
    it->p0 = (int16_t)r;
    it->color = color;
    it->a = (uint8_t)(k > 255 ? 255 : k);
    seal(it);
}

void bs_shadow(bs_gfx_t *g, int x, int y, int rx, int ry, int a)
{
    if (rx < 2 || ry < 1 || a <= 0) return;
    bs_item_t *it = push(g, IT_SHADOW, x - rx, y - ry, x + rx, y + ry);
    if (!it) return;
    it->x = (int16_t)x;
    it->y = (int16_t)y;
    it->p0 = (int16_t)rx;
    it->p1 = (int16_t)ry;
    it->a = (uint8_t)(a > 255 ? 255 : a);
    seal(it);
}

void bs_rect(bs_gfx_t *g, int x, int y, int w, int h, uint16_t color, int a, int radius)
{
    if (a <= 0) return;
    bs_item_t *it = push(g, IT_RECT, x, y, x + w, y + h);
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

int bs_text(bs_gfx_t *g, const bs_font_t *f, int x, int y, const char *s, uint16_t color, int flags)
{
    if (!f || !f->data || !s || !*s) return 0;
    int w = bs_text_width(f, s);
    if (flags & BS_F_CENTER) x -= w / 2;
    else if (flags & BS_F_RIGHT) x -= w;
    size_t n = strlen(s) + 1;
    if (g->ntext + (int)n > BS_TEXT_ARENA) return w;
    bs_item_t *it = push(g, IT_TEXT, x - 1, y - 2, x + w + 4, y + f->line_h + 4);
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
    /* the font goes where the text was: the text is found in the arena */
    it->p = f;
    return w;
}

void bs_beam(bs_gfx_t *g, int x0, int y0, int x1, int y1, int halfw, uint16_t color, int k)
{
    if (halfw < 1 || k <= 0) return;
    int ax = x0 < x1 ? x0 : x1, bx = x0 < x1 ? x1 : x0;
    int ay = y0 < y1 ? y0 : y1, by = y0 < y1 ? y1 : y0;
    int dx = bx - ax, dy = by - ay;
    /* a slanted line is wider across a row than its own width */
    int pad = halfw + (dx > dy ? (dy * halfw) / (dx ? dx : 1) : (dx * halfw) / (dy ? dy : 1)) + 1;
    bs_item_t *it = push(g, IT_BEAM, ax - pad, ay - pad, bx + pad + 1, by + pad + 1);
    if (!it) return;
    it->x = (int16_t)x0;
    it->y = (int16_t)y0;
    it->p0 = (int16_t)x1;
    it->p1 = (int16_t)y1;
    it->p2 = (int16_t)halfw;
    it->color = color;
    it->a = (uint8_t)(k > 255 ? 255 : k);
    seal(it);
}

void bs_ring(bs_gfx_t *g, int cx, int cy, int rx, int ry, int thick, uint16_t color, int k, int flags)
{
    if (rx < 2 || ry < 2 || k <= 0) return;
    bs_item_t *it = push(g, IT_RING, cx - rx, cy - ry, cx + rx + 1, cy + ry + 1);
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

void bs_disc(bs_gfx_t *g, int cx, int cy, int rx, int ry, uint16_t color, int a)
{
    if (rx < 1 || ry < 1 || a <= 0) return;
    bs_item_t *it = push(g, IT_DISC, cx - rx, cy - ry, cx + rx + 1, cy + ry + 1);
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

static void draw_spr(const bs_gfx_t *g, uint16_t *fb, const bs_item_t *it, const bs_rect_t *c, bool ztest)
{
    const bs_sheet_t *s = (const bs_sheet_t *)it->p;
    const bs_frame_t *fr = &s->fr[it->p3];
    const uint32_t *row = (const uint32_t *)(s->data + fr->off);
    int ya = it->y < c->y0 ? c->y0 : it->y;
    int yb = it->y + fr->h > c->y1 ? c->y1 : it->y + fr->h;
    int ga = it->a >> 3;                    /* 0..31 */
    bool solid = it->a >= 250 && !(it->flags & BS_F_FLASH);
    bool flash = (it->flags & BS_F_FLASH) != 0;
    for (int y = ya; y < yb; y++) {
        const uint16_t *op = (const uint16_t *)(s->data + row[y - it->y]);
        uint16_t *dst = fb + (size_t)y * BS_W;
        const uint16_t *dep = ztest ? g->depth + (size_t)y * BS_W : NULL;
        int zr = 0;
        if (ztest) {
            int up = it->foot - y;
            zr = (int)it->z - (up >= 0 ? up * Z_ROW_UP : -up * Z_ROW_DOWN) - Z_BIAS;
        }
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
                const uint16_t *src = op - x;       /* src[px] is the pixel of column px */
                if (a < b) {
                    if (solid && !ztest) {
                        memcpy(dst + a, src + a, (size_t)(b - a) * 2);
                    } else {
                        for (int px = a; px < b; px++) {
                            if (ztest && zr > (int)dep[px]) continue;
                            uint16_t col = src[px];
                            if (flash) col = blend5(col, EXP(0xFFFF), 20);
                            dst[px] = solid || ga >= 31 ? col : blend5(dst[px], EXP(col), (uint32_t)ga);
                        }
                    }
                }
                op += n;
            } else {
                const uint16_t *src = op - 2 * x;   /* pairs */
                for (int px = a; px < b; px++) {
                    if (ztest && zr > (int)dep[px]) continue;
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

static void draw_glow(const bs_gfx_t *g, uint16_t *fb, const bs_item_t *it, const bs_rect_t *c)
{
    int r = it->p0;
    int xa = it->x0 < c->x0 ? c->x0 : it->x0, xb = it->x1 > c->x1 ? c->x1 : it->x1;
    int ya = it->y0 < c->y0 ? c->y0 : it->y0, yb = it->y1 > c->y1 ? c->y1 : it->y1;
    uint32_t step = (64u << 16) / (uint32_t)(2 * r);
    uint32_t ce = EXP(it->color);
    int ox = it->x - r, oy = it->y - r;
    for (int y = ya; y < yb; y++) {
        const uint8_t *t = g->glow + (((uint32_t)(y - oy) * step) >> 16) * 64;
        uint16_t *dst = fb + (size_t)y * BS_W;
        uint32_t u = (uint32_t)(xa - ox) * step;
        for (int x = xa; x < xb; x++, u += step) {
            uint32_t a = ((uint32_t)t[u >> 16] * it->a) >> 11;      /* 0..31 */
            if (a) dst[x] = add_sat(dst[x], ((ce * a) >> 5) & M565);
        }
    }
}

static void draw_shadow(uint16_t *fb, const bs_item_t *it, const bs_rect_t *c)
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
        uint16_t *dst = fb + (size_t)y * BS_W;
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

static void draw_rect(uint16_t *fb, const bs_item_t *it, const bs_rect_t *c)
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
        uint16_t *dst = fb + (size_t)y * BS_W;
        if (a5 >= 31) {
            for (int x = xa; x < xb; x++) dst[x] = it->color;
        } else {
            for (int x = xa; x < xb; x++) dst[x] = blend5(dst[x], ce, a5);
        }
    }
}

static void glyphs(const bs_gfx_t *g, uint16_t *fb, const bs_item_t *it, const bs_rect_t *c, int dx, int dy, uint16_t color)
{
    const bs_font_t *f = (const bs_font_t *)it->p;
    const char *s = g->text + it->p0;
    int x = it->x + dx, y0 = it->y + dy;
    uint32_t ce = EXP(color);
    while (*s) {
        int i = f->idx[next_cp(&s)];
        if (i < 0) continue;
        const bs_glyph_t *gl = &f->g[i];
        int gx = x + gl->ox, gy = y0 + gl->oy;
        x += gl->adv;
        if (gx >= c->x1) break;
        if (gx + gl->w <= c->x0 || gy >= c->y1 || gy + gl->h <= c->y0) continue;
        int xa = gx < c->x0 ? c->x0 : gx, xb = gx + gl->w > c->x1 ? c->x1 : gx + gl->w;
        int ya = gy < c->y0 ? c->y0 : gy, yb = gy + gl->h > c->y1 ? c->y1 : gy + gl->h;
        for (int y = ya; y < yb; y++) {
            const uint8_t *src = f->atlas + (size_t)(gl->y + y - gy) * f->aw + gl->x - gx;
            uint16_t *dst = fb + (size_t)y * BS_W;
            for (int px = xa; px < xb; px++) {
                uint32_t a = src[px];
                if (a >= 248) dst[px] = color;
                else if (a >= 8) dst[px] = blend5(dst[px], ce, a >> 3);
            }
        }
    }
}

static void draw_text(const bs_gfx_t *g, uint16_t *fb, const bs_item_t *it, const bs_rect_t *c)
{
    if (it->flags & BS_F_SHADOW) glyphs(g, fb, it, c, 2, 2, 0x0000);
    glyphs(g, fb, it, c, 0, 0, it->color);
}

static void draw_beam(const bs_gfx_t *g, uint16_t *fb, const bs_item_t *it, const bs_rect_t *c)
{
    int x0 = it->x, y0 = it->y, x1 = it->p0, y1 = it->p1, hw = it->p2;
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
        int32_t slope = (dx * 65536) / dy;       /* |dx| <= |dy| < 1500: 32 bits are enough */
        int ya = y0 < c->y0 ? c->y0 : y0, yb = y1 + 1 > c->y1 ? c->y1 : y1 + 1;
        for (int y = ya; y < yb; y++) {
            int xc = x0 + (((y - y0) * slope) >> 16);
            int xa = xc - half, xb = xc + half + 1;
            if (xa < c->x0) xa = c->x0;
            if (xb > c->x1) xb = c->x1;
            uint16_t *dst = fb + (size_t)y * BS_W;
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
                uint16_t *dst = fb + (size_t)y * BS_W + x;
                if (a) *dst = add_sat(*dst, ((ce * a) >> 5) & M565);
            }
        }
    }
}

static inline void span(uint16_t *dst, int xa, int xb, const bs_rect_t *c, uint32_t ce, uint32_t a5, bool add, uint16_t color)
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

static void draw_ring(uint16_t *fb, const bs_item_t *it, const bs_rect_t *c, bool fill)
{
    int rx = it->p0, ry = it->p1, th = it->p2;
    int ya = it->y0 < c->y0 ? c->y0 : it->y0, yb = it->y1 > c->y1 ? c->y1 : it->y1;
    uint32_t ce = EXP(it->color);
    uint32_t a5 = (uint32_t)(it->a + 4) >> 3;
    bool add = (it->flags & BS_F_ADD) != 0;
    int rxi = rx - th, ryi = ry - (th * ry + rx / 2) / rx;
    if (ryi < 1) ryi = 1;
    for (int y = ya; y < yb; y++) {
        float d = ((float)(y - it->y)) / (float)ry;
        float q = 1.0f - d * d;
        if (q < 0) continue;
        int xo = (int)((float)rx * sqrtf(q) + 0.5f);
        uint16_t *dst = fb + (size_t)y * BS_W;
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

static void draw_item(const bs_gfx_t *g, uint16_t *fb, const bs_item_t *it, const bs_rect_t *c, uint16_t zmin)
{
    switch (it->kind) {
    case IT_SPR:
        /* tested only where the world has something nearer than its foot */
        draw_spr(g, fb, it, c, (it->flags & BS_F_DEPTH) && g->depth && (int)it->z - Z_BIAS > (int)zmin);
        break;
    case IT_GLOW:   draw_glow(g, fb, it, c); break;
    case IT_SHADOW: draw_shadow(fb, it, c); break;
    case IT_RECT:   draw_rect(fb, it, c); break;
    case IT_TEXT:   draw_text(g, fb, it, c); break;
    case IT_BEAM:   draw_beam(g, fb, it, c); break;
    case IT_RING:   draw_ring(fb, it, c, false); break;
    case IT_DISC:   draw_ring(fb, it, c, true); break;
    default: break;
    }
}

/* ---- the frame ------------------------------------------------------------------------- */

static void band(const bs_gfx_t *g, int ty)
{
    const uint8_t *need = g->need + ty * BS_TW;
    const uint16_t *ix = g->band_ix + g->band_at[ty];
    int n = g->band_at[ty + 1] - g->band_at[ty];
    uint16_t *fb = g->fb;
    for (int tx = 0; tx < BS_TW;) {
        if (!need[tx]) {
            tx++;
            continue;
        }
        int t0 = tx;
        uint16_t zmin = 0xFFFF;
        while (tx < BS_TW && need[tx]) {
            uint16_t z = g->zmin[ty * BS_TW + tx];
            if (z < zmin) zmin = z;
            tx++;
        }
        bs_rect_t c = { (int16_t)(t0 * BS_TILE), (int16_t)(ty * BS_TILE), (int16_t)(tx * BS_TILE),
                        (int16_t)((ty + 1) * BS_TILE) };
        size_t bytes = (size_t)(c.x1 - c.x0) * 2;
        for (int y = c.y0; y < c.y1; y++) {
            if (g->bg) memcpy(fb + (size_t)y * BS_W + c.x0, g->bg + (size_t)y * BS_W + c.x0, bytes);
            else memset(fb + (size_t)y * BS_W + c.x0, 0, bytes);
        }
        for (int i = 0; i < n; i++) {
            const bs_item_t *it = &g->item[ix[i]];
            if (it->x1 <= c.x0 || it->x0 >= c.x1) continue;
            draw_item(g, fb, it, &c, zmin);
        }
    }
}

static void half(void *arg, int part)
{
    const bs_gfx_t *g = (const bs_gfx_t *)arg;
    int a = part ? g->split : 0, b = part ? BS_TH : g->split;
    for (int ty = a; ty < b; ty++) band(g, ty);
}

int bs_gfx_end(bs_gfx_t *g, uint16_t *fb, uint32_t *shown, bool full)
{
    /* each tile's hash: its things, in order */
    memset(g->cur, 0, sizeof(uint32_t) * BS_NT);
    int counts[BS_TH];
    memset(counts, 0, sizeof counts);
    for (int i = 0; i < g->nitem; i++) {
        const bs_item_t *it = &g->item[i];
        int tx0 = it->x0 / BS_TILE, tx1 = (it->x1 - 1) / BS_TILE;
        int ty0 = it->y0 / BS_TILE, ty1 = (it->y1 - 1) / BS_TILE;
        for (int ty = ty0; ty <= ty1; ty++) {
            uint32_t *h = g->cur + ty * BS_TW;
            for (int tx = tx0; tx <= tx1; tx++) h[tx] = mix(h[tx], it->hash);
            counts[ty]++;
        }
    }
    /* the things of each row of tiles, in order */
    int at = 0;
    for (int ty = 0; ty < BS_TH; ty++) {
        g->band_at[ty] = at;
        at += counts[ty];
    }
    g->band_at[BS_TH] = at;
    if (at > g->band_cap) {
        /* never seen: the list holds eight rows of tiles a thing */
        aos_hal_log("bastion", "gfx: %d band entries, %d fit", at, g->band_cap);
        return 0;
    }
    int fill[BS_TH];
    memcpy(fill, g->band_at, sizeof fill);
    for (int i = 0; i < g->nitem; i++) {
        const bs_item_t *it = &g->item[i];
        int ty0 = it->y0 / BS_TILE, ty1 = (it->y1 - 1) / BS_TILE;
        for (int ty = ty0; ty <= ty1; ty++) g->band_ix[fill[ty]++] = (uint16_t)i;
    }
    /* what the target lacks */
    int total = 0;
    int per[BS_TH];
    int x0 = BS_TW, x1 = -1, y0 = BS_TH, y1 = -1;
    for (int ty = 0; ty < BS_TH; ty++) {
        int n = 0;
        for (int tx = 0; tx < BS_TW; tx++) {
            int t = ty * BS_TW + tx;
            bool d = full || g->cur[t] != shown[t];
            g->need[t] = d;
            if (d) {
                n++;
                if (tx < x0) x0 = tx;
                if (tx > x1) x1 = tx;
            }
        }
        /* a row's cost: its tiles and the things over them */
        per[ty] = n ? n * 2 + counts[ty] : 0;
        total += per[ty];
        if (n) {
            if (ty < y0) y0 = ty;
            y1 = ty;
        }
        g->drawn_tiles = ty == 0 ? n : g->drawn_tiles + n;
    }
    g->dirty.x0 = (int16_t)(x0 * BS_TILE);
    g->dirty.x1 = (int16_t)((x1 + 1) * BS_TILE);
    g->dirty.y0 = (int16_t)(y0 * BS_TILE);
    g->dirty.y1 = (int16_t)((y1 + 1) * BS_TILE);
    if (y1 < 0) {
        g->dirty.x0 = g->dirty.x1 = g->dirty.y0 = g->dirty.y1 = 0;
        return 0;
    }
    /* two halves of about the same work */
    int acc = 0;
    g->split = BS_TH;
    for (int ty = 0; ty < BS_TH; ty++) {
        acc += per[ty];
        if (acc * 2 >= total) {
            g->split = ty + 1;
            break;
        }
    }
    g->fb = fb;
    if (g->one_core || total < 60 || g->split >= BS_TH || !aos_hal_worker_split(half, g)) {
        int s = g->split;
        g->split = BS_TH;
        half(g, 0);
        g->split = s;
    }
    memcpy(shown, g->cur, sizeof(uint32_t) * BS_NT);
    return g->drawn_tiles;
}
