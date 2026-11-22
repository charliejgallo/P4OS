/*
 * SOLITARIO - the art. See so_art.h.
 *
 * The suits and the stroke font are the method of Blackjack's art
 * (apps/blackjack/main/bj_art.c), redone for any size: there the card is
 * drawn in the watch's units at an integer scale, here every measure is a
 * fraction of the card's width, because a solitaire needs cards of whatever
 * width seven or ten columns leave.
 */
#include "so_art.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --- palette ------------------------------------------------------------------ */

#define C_PAPER_T   0xFFFFFF
#define C_PAPER_B   0xF2EEE5
#define C_EDGE      0x9E978B
#define C_RED       0xD0142C
#define C_BLACK     0x17171C
#define C_BLUE4     0x1F5BC9
#define C_GREEN4    0x13803A
#define C_GOLD      0xE2B84E
#define C_GOLD_D    0xA27A1C

/* fminf/fmaxf are not in the firmware's symbol table; these are enough */
static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float smooth(float t)
{
    return t * t * (3.f - 2.f * t);
}

/* --- images and pixels ------------------------------------------------------- */

so_img_t so_img_new(int w, int h)
{
    so_img_t m = { NULL, w, h };
    m.px = (uint32_t *)malloc((size_t)w * h * 4);
    if (m.px) {
        memset(m.px, 0, (size_t)w * h * 4);
    }
    return m;
}

void so_img_free(so_img_t *img)
{
    free(img->px);
    img->px = NULL;
}

void so_img_clear(so_img_t *img)
{
    memset(img->px, 0, (size_t)img->w * img->h * 4);
}

void so_plot(so_img_t *m, int x, int y, uint32_t rgb, int a)
{
    if (a <= 0 || x < 0 || y < 0 || x >= m->w || y >= m->h) {
        return;
    }
    uint32_t *p  = &m->px[y * m->w + x];
    uint32_t  d  = *p;
    int       da = (int)(d >> 24);
    if (a >= 255 || da == 0) {
        *p = ((uint32_t)(a > 255 ? 255 : a) << 24) | (rgb & 0xFFFFFF);
        return;
    }
    int k  = da * (255 - a) / 255;
    int oa = a + k;
    int r = ((int)((rgb >> 16) & 0xFF) * a + (int)((d >> 16) & 0xFF) * k) / oa;
    int g = ((int)((rgb >> 8) & 0xFF) * a + (int)((d >> 8) & 0xFF) * k) / oa;
    int b = ((int)(rgb & 0xFF) * a + (int)(d & 0xFF) * k) / oa;
    *p = ((uint32_t)oa << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t)
{
    t = clampf(t, 0.f, 1.f);
    int r = (int)(((a >> 16) & 0xFF) + (((int)((b >> 16) & 0xFF) - (int)((a >> 16) & 0xFF)) * t));
    int g = (int)(((a >> 8) & 0xFF) + (((int)((b >> 8) & 0xFF) - (int)((a >> 8) & 0xFF)) * t));
    int c = (int)((a & 0xFF) + (((int)(b & 0xFF) - (int)(a & 0xFF)) * t));
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)c;
}

static uint32_t shade(uint32_t c, int pct)
{
    int r = (int)((c >> 16) & 0xFF) * pct / 100;
    int g = (int)((c >> 8) & 0xFF) * pct / 100;
    int b = (int)(c & 0xFF) * pct / 100;
    r = r > 255 ? 255 : r;
    g = g > 255 ? 255 : g;
    b = b > 255 ? 255 : b;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* value noise, 0..1 */
static float vnoise(float x, float y)
{
    float fx = floorf(x), fy = floorf(y);
    int   ix = (int)fx, iy = (int)fy;
    float tx = smooth(x - fx), ty = smooth(y - fy);
    float a = (float)(hash2(ix, iy) & 0xFFFF) / 65535.f;
    float b = (float)(hash2(ix + 1, iy) & 0xFFFF) / 65535.f;
    float c = (float)(hash2(ix, iy + 1) & 0xFFFF) / 65535.f;
    float d = (float)(hash2(ix + 1, iy + 1) & 0xFFFF) / 65535.f;
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

/* --- rounded rectangles ----------------------------------------------------- */

static float sd_rrect(float px, float py, float x0, float y0, float w, float h, float r)
{
    float cx = x0 + w * 0.5f, cy = y0 + h * 0.5f;
    float qx = fabsf(px - cx) - (w * 0.5f - r);
    float qy = fabsf(py - cy) - (h * 0.5f - r);
    float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    float out = sqrtf(mx * mx + my * my);
    float in  = qx > qy ? qx : qy;
    return out + (in < 0 ? in : 0) - r;
}

static void rrect_fill(so_img_t *m, float x0, float y0, float w, float h, float r,
                       uint32_t ct, uint32_t cb, int opa)
{
    int ix0 = (int)floorf(x0) - 1, iy0 = (int)floorf(y0) - 1;
    int ix1 = (int)ceilf(x0 + w) + 1, iy1 = (int)ceilf(y0 + h) + 1;
    for (int y = iy0; y < iy1; y++) {
        uint32_t c = ct == cb ? ct : lerp_rgb(ct, cb, ((float)y + 0.5f - y0) / h);
        for (int x = ix0; x < ix1; x++) {
            float d = sd_rrect((float)x + 0.5f, (float)y + 0.5f, x0, y0, w, h, r);
            float cov = clampf(0.5f - d, 0.f, 1.f);
            if (cov > 0) {
                so_plot(m, x, y, c, (int)(cov * (float)opa + 0.5f));
            }
        }
    }
}

static void rrect_stroke(so_img_t *m, float x0, float y0, float w, float h, float r,
                         float bw, uint32_t c, int opa)
{
    int ix0 = (int)floorf(x0) - 1, iy0 = (int)floorf(y0) - 1;
    int ix1 = (int)ceilf(x0 + w) + 1, iy1 = (int)ceilf(y0 + h) + 1;
    for (int y = iy0; y < iy1; y++) {
        for (int x = ix0; x < ix1; x++) {
            float d  = sd_rrect((float)x + 0.5f, (float)y + 0.5f, x0, y0, w, h, r);
            float c1 = clampf(0.5f - d, 0.f, 1.f);
            float c2 = clampf(d + bw + 0.5f, 0.f, 1.f);
            float cov = c1 < c2 ? c1 : c2;
            if (cov > 0) {
                so_plot(m, x, y, c, (int)(cov * (float)opa + 0.5f));
            }
        }
    }
}

static void rrect_shadow(so_img_t *m, float x0, float y0, float w, float h, float r,
                         float blur, int opa)
{
    int ix0 = (int)floorf(x0 - blur) - 1, iy0 = (int)floorf(y0 - blur) - 1;
    int ix1 = (int)ceilf(x0 + w + blur) + 1, iy1 = (int)ceilf(y0 + h + blur) + 1;
    for (int y = iy0; y < iy1; y++) {
        for (int x = ix0; x < ix1; x++) {
            float d = sd_rrect((float)x + 0.5f, (float)y + 0.5f, x0, y0, w, h, r);
            float t = clampf(1.f - (d + 0.5f) / blur, 0.f, 1.f);
            if (t > 0) {
                so_plot(m, x, y, 0x000000, (int)(t * t * (float)opa));
            }
        }
    }
}

/* a disc, antialiased */
static void disc(so_img_t *m, float cx, float cy, float r, uint32_t c, int opa)
{
    rrect_fill(m, cx - r, cy - r, 2 * r, 2 * r, r, c, c, opa);
}

static void ring(so_img_t *m, float cx, float cy, float r, float bw, uint32_t c, int opa)
{
    rrect_stroke(m, cx - r, cy - r, 2 * r, 2 * r, r, bw, c, opa);
}

/* --- the suits ----------------------------------------------------------------- */

static bool in_heart_raw(float nx, float ny)
{
    /* the sextic (x^2 + y^2 - 1)^3 - x^2 y^3 <= 0, y up, fitted to the box */
    float X = nx * 1.20f;
    float Y = 0.06f - 1.16f * ny;
    float a = X * X + Y * Y - 1.f;
    return a * a * a - X * X * Y * Y * Y <= 0.f;
}

static bool in_stem(float nx, float ny, float top)
{
    if (ny < top || ny > 0.98f) {
        return false;
    }
    float u  = (ny - top) / (0.98f - top);
    float hw = 0.07f + 0.36f * u * u * u;
    return fabsf(nx) <= hw;
}

static bool in_suit(int suit, float nx, float ny)
{
    switch (suit) {
    case 1:
        return in_heart_raw(nx, ny * 1.03f);
    case 2: {
        float ax = fabsf(nx) / 0.80f, ay = fabsf(ny);
        return powf(ax, 0.88f) + powf(ay, 0.88f) <= 1.f;
    }
    case 0: {
        float t = (ny + 1.f) / 0.78f - 1.f;
        if (t <= 1.f && in_heart_raw(nx * 1.02f, -t)) {
            return true;
        }
        return in_stem(nx, ny, 0.30f);
    }
    default: {
        static const float c[3][2] = { { 0.f, -0.50f }, { -0.50f, 0.12f }, { 0.50f, 0.12f } };
        const float r = 0.40f;
        for (int i = 0; i < 3; i++) {
            float dx = nx - c[i][0], dy = ny - c[i][1];
            if (dx * dx + dy * dy <= r * r) {
                return true;
            }
        }
        if (nx * nx + (ny + 0.05f) * (ny + 0.05f) <= 0.26f * 0.26f) {
            return true;
        }
        return in_stem(nx, ny, 0.10f);
    }
    }
}

bool so_suit_test(int suit, float nx, float ny)
{
    return in_suit(suit, nx, ny);
}

static const so_mask_t *suit_mask(so_art_t *a, int suit, int size)
{
    if (size < 2) {
        size = 2;
    }
    for (int i = 0; i < a->nmasks; i++) {
        if (a->masks[i].suit == suit && a->masks[i].size == size) {
            return &a->masks[i];
        }
    }
    if (a->nmasks >= SO_MAX_MASKS) {
        return NULL;
    }
    uint8_t *m = (uint8_t *)malloc((size_t)size * size);
    if (!m) {
        return NULL;
    }
    const float inv = 2.f / (float)size;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int n = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    float nx = ((float)x + ((float)sx + 0.5f) / 4.f) * inv - 1.f;
                    float ny = ((float)y + ((float)sy + 0.5f) / 4.f) * inv - 1.f;
                    n += in_suit(suit, nx, ny);
                }
            }
            m[y * size + x] = (uint8_t)(n * 255 / 16);
        }
    }
    so_mask_t *k = &a->masks[a->nmasks++];
    k->a = m;
    k->size = size;
    k->suit = suit;
    return k;
}

uint32_t so_suit_color(const so_art_t *a, int suit)
{
    if (a->deck == SO_DECK_FOUR) {
        static const uint32_t C4[4] = { C_BLACK, C_RED, C_BLUE4, C_GREEN4 };
        return C4[suit & 3];
    }
    return (suit == 1 || suit == 2) ? C_RED : C_BLACK;
}

/* a pip with its box's top-left at (x, y); 'turn' draws it upside down */
static void pip(so_art_t *a, so_img_t *m, int suit, int size, int x, int y, uint32_t col, bool turn)
{
    const so_mask_t *k = suit_mask(a, suit, size);
    if (!k) {
        return;
    }
    size = k->size;
    for (int j = 0; j < size; j++) {
        for (int i = 0; i < size; i++) {
            int sx = turn ? size - 1 - i : i;
            int sy = turn ? size - 1 - j : j;
            so_plot(m, x + i, y + j, col, k->a[sy * size + sx]);
        }
    }
}

void so_suit_draw(so_art_t *a, so_img_t *img, int suit, int size, int x, int y, uint32_t rgb)
{
    pip(a, img, suit, size, x, y, rgb, false);
}

/* --- the stroke font -------------------------------------------------------------
 * Glyphs in a 10 x 14 box, y down: M moves with the pen down, P lifts it,
 * L draws a line, A an elliptical arc from a0 to a1 degrees.
 * ------------------------------------------------------------------------------ */

enum { G_M = 1, G_L, G_A, G_P, G_E };
#define M_(x, y)                  G_M, x, y
#define L_(x, y)                  G_L, x, y
#define A_(cx, cy, rx, ry, a, b)  G_A, cx, cy, rx, ry, a, b
#define P_                        G_P
#define E_                        G_E

static const float GL_A[]  = { M_(0.6f, 14.f), L_(5.f, 0.2f), L_(9.4f, 14.f), M_(2.3f, 9.4f), L_(7.7f, 9.4f), E_ };
static const float GL_2[]  = { P_, A_(5.f, 4.3f, 4.2f, 4.1f, 190.f, 395.f), L_(0.8f, 13.8f), L_(9.6f, 13.8f), E_ };
static const float GL_3[]  = { P_, A_(5.f, 3.7f, 3.9f, 3.5f, 200.f, 450.f), P_, A_(5.f, 10.3f, 4.4f, 3.6f, 270.f, 515.f), E_ };
static const float GL_4[]  = { M_(7.3f, 14.f), L_(7.3f, 0.2f), L_(0.5f, 9.8f), L_(9.8f, 9.8f), E_ };
static const float GL_5[]  = { M_(9.f, 0.3f), L_(2.1f, 0.3f), L_(1.5f, 6.4f), A_(5.0f, 9.5f, 4.4f, 4.3f, 226.f, 505.f), E_ };
static const float GL_6[]  = { P_, A_(5.f, 9.6f, 4.3f, 4.3f, 0.f, 360.f), P_, A_(9.4f, 9.6f, 8.7f, 9.4f, 253.f, 180.f), E_ };
static const float GL_7[]  = { M_(0.5f, 0.3f), L_(9.5f, 0.3f), L_(3.7f, 14.f), E_ };
static const float GL_8[]  = { P_, A_(5.f, 3.6f, 3.6f, 3.4f, 0.f, 360.f), P_, A_(5.f, 10.3f, 4.3f, 3.7f, 0.f, 360.f), E_ };
static const float GL_9[]  = { P_, A_(5.f, 4.4f, 4.3f, 4.3f, 0.f, 360.f), P_, A_(0.6f, 4.4f, 8.7f, 9.4f, 73.f, 0.f), E_ };
static const float GL_10[] = { M_(0.2f, 2.8f), L_(2.8f, 0.2f), L_(2.8f, 14.f),
                               P_, A_(8.9f, 7.f, 3.5f, 6.8f, 0.f, 360.f), E_ };
static const float GL_J[]  = { M_(8.2f, 0.2f), L_(8.2f, 9.6f), A_(4.7f, 9.6f, 3.5f, 4.2f, 0.f, 165.f), E_ };
static const float GL_Q[]  = { P_, A_(5.f, 7.f, 4.4f, 6.8f, 0.f, 360.f), M_(5.9f, 9.9f), L_(9.8f, 14.4f), E_ };
static const float GL_K[]  = { M_(1.f, 0.2f), L_(1.f, 14.f), M_(9.3f, 0.2f), L_(1.2f, 8.7f), M_(4.3f, 5.6f), L_(9.6f, 14.f), E_ };

static const float *const RANK_GLYPH[13] = {
    GL_A, GL_2, GL_3, GL_4, GL_5, GL_6, GL_7, GL_8, GL_9, GL_10, GL_J, GL_Q, GL_K,
};

#define MAX_SEGS 96
typedef struct { float x0, y0, x1, y1; } seg_t;

static int glyph_segs(const float *g, seg_t *out)
{
    int   n = 0;
    float px = 0, py = 0;
    bool  down = false;
    while (*g != G_E && n < MAX_SEGS) {
        int op = (int)*g++;
        if (op == G_M) {
            px = g[0];
            py = g[1];
            g += 2;
            down = true;
        } else if (op == G_P) {
            down = false;
        } else if (op == G_L) {
            if (down) {
                out[n++] = (seg_t){ px, py, g[0], g[1] };
            }
            px = g[0];
            py = g[1];
            g += 2;
            down = true;
        } else if (op == G_A) {
            float cx = g[0], cy = g[1], rx = g[2], ry = g[3], a0 = g[4], a1 = g[5];
            g += 6;
            int steps = (int)(fabsf(a1 - a0) / 12.f) + 2;
            for (int i = 0; i <= steps && n < MAX_SEGS; i++) {
                float a = (a0 + (a1 - a0) * (float)i / (float)steps) * 3.14159265f / 180.f;
                float x = cx + rx * cosf(a), y = cy + ry * sinf(a);
                if (down) {
                    out[n++] = (seg_t){ px, py, x, y };
                }
                px = x;
                py = y;
                down = true;
            }
        }
    }
    return n;
}

static float seg_dist2(const seg_t *s, float x, float y)
{
    float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
    float l2 = dx * dx + dy * dy;
    float t  = l2 > 0 ? ((x - s->x0) * dx + (y - s->y0) * dy) / l2 : 0;
    t = clampf(t, 0.f, 1.f);
    float ex = s->x0 + dx * t - x, ey = s->y0 + dy * t - y;
    return ex * ex + ey * ey;
}

/* glyph 'g' with its box's top-left at (x, y) in pixels, 'h' tall,
 * horizontal scale 'sx', stroke 'sw' wide */
static void glyph(so_img_t *m, const float *g, float x, float y, float h, float sx, float sw,
                  uint32_t color)
{
    seg_t seg[MAX_SEGS];
    int   n = glyph_segs(g, seg);
    float k = h / 14.f;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        seg[i].x0 = x + seg[i].x0 * k * sx;
        seg[i].x1 = x + seg[i].x1 * k * sx;
        seg[i].y0 = y + seg[i].y0 * k;
        seg[i].y1 = y + seg[i].y1 * k;
        minx = minf(minx, minf(seg[i].x0, seg[i].x1));
        maxx = maxf(maxx, maxf(seg[i].x0, seg[i].x1));
        miny = minf(miny, minf(seg[i].y0, seg[i].y1));
        maxy = maxf(maxy, maxf(seg[i].y0, seg[i].y1));
    }
    float hw = sw * 0.5f;
    int   x0 = (int)floorf(minx - hw - 1), x1 = (int)ceilf(maxx + hw + 1);
    int   y0 = (int)floorf(miny - hw - 1), y1 = (int)ceilf(maxy + hw + 1);
    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            float cx = (float)px + 0.5f, cy = (float)py + 0.5f;
            float best = 1e9f;
            for (int i = 0; i < n; i++) {
                float d2 = seg_dist2(&seg[i], cx, cy);
                if (d2 < best) {
                    best = d2;
                }
            }
            float cov = clampf(hw + 0.5f - sqrtf(best), 0.f, 1.f);
            if (cov > 0) {
                so_plot(m, px, py, color, (int)(cov * 255.f + 0.5f));
            }
        }
    }
}

/* the width a rank's glyph takes at height h */
static float rank_w(int rank, float h, float *sx)
{
    *sx = rank == 9 ? 0.70f : 0.84f;
    return (rank == 9 ? 12.4f : 10.f) * (h / 14.f) * *sx;
}

/* --- setup ---------------------------------------------------------------------- */

int so_card_h_for(int cw)
{
    return (cw * 7 + 2) / 5;
}

static float idx_h(const so_art_t *a)
{
    return (float)a->cw * (a->deck == SO_DECK_LARGE ? 0.34f : 0.25f);
}

void so_art_setup(so_art_t *a, int cw, int ch, int deck, int flags)
{
    (void)flags;
    so_art_free(a);
    a->cw = cw;
    a->ch = ch;
    a->pad = cw / 18 < 3 ? 3 : cw / 18;
    a->cv_w = cw + 2 * a->pad;
    a->cv_h = ch + 2 * a->pad;
    a->deck = deck;
    float top = (float)cw * 0.055f;
    a->strip = (int)(top + idx_h(a) + (float)cw * 0.06f + 0.5f);
}

void so_art_free(so_art_t *a)
{
    for (int i = 0; i < a->nmasks; i++) {
        free(a->masks[i].a);
    }
    a->nmasks = 0;
}

/* --- a face ------------------------------------------------------------------------ */

typedef struct { float t; int8_t col; bool turn; } spot_t;

/* t: 0 the top row of the field, 1 the bottom one; col -1, 0, 1 */
static const spot_t SP2[]  = { { 0, 0, 0 }, { 1, 0, 1 } };
static const spot_t SP3[]  = { { 0, 0, 0 }, { .5f, 0, 0 }, { 1, 0, 1 } };
static const spot_t SP4[]  = { { 0, -1, 0 }, { 0, 1, 0 }, { 1, -1, 1 }, { 1, 1, 1 } };
static const spot_t SP5[]  = { { 0, -1, 0 }, { 0, 1, 0 }, { .5f, 0, 0 }, { 1, -1, 1 }, { 1, 1, 1 } };
static const spot_t SP6[]  = { { 0, -1, 0 }, { 0, 1, 0 }, { .5f, -1, 0 }, { .5f, 1, 0 },
                               { 1, -1, 1 }, { 1, 1, 1 } };
static const spot_t SP7[]  = { { 0, -1, 0 }, { 0, 1, 0 }, { .25f, 0, 0 }, { .5f, -1, 0 },
                               { .5f, 1, 0 }, { 1, -1, 1 }, { 1, 1, 1 } };
static const spot_t SP8[]  = { { 0, -1, 0 }, { 0, 1, 0 }, { .25f, 0, 0 }, { .5f, -1, 0 },
                               { .5f, 1, 0 }, { .75f, 0, 1 }, { 1, -1, 1 }, { 1, 1, 1 } };
static const spot_t SP9[]  = { { 0, -1, 0 }, { 0, 1, 0 }, { 1.f / 3, -1, 0 }, { 1.f / 3, 1, 0 },
                               { .5f, 0, 0 }, { 2.f / 3, -1, 1 }, { 2.f / 3, 1, 1 },
                               { 1, -1, 1 }, { 1, 1, 1 } };
static const spot_t SP10[] = { { 0, -1, 0 }, { 0, 1, 0 }, { 1.f / 6, 0, 0 }, { 1.f / 3, -1, 0 },
                               { 1.f / 3, 1, 0 }, { 2.f / 3, -1, 1 }, { 2.f / 3, 1, 1 },
                               { 5.f / 6, 0, 1 }, { 1, -1, 1 }, { 1, 1, 1 } };

static const spot_t *const SPOTS[11] = { NULL, NULL, SP2, SP3, SP4, SP5, SP6, SP7, SP8, SP9, SP10 };
static const uint8_t NSPOTS[11] = { 0, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

static void card_body(so_art_t *a, so_img_t *m, uint32_t top, uint32_t bot)
{
    float p = (float)a->pad, w = (float)a->cw, h = (float)a->ch;
    float r = maxf(4.f, w * 0.07f);
    rrect_shadow(m, p + w * 0.008f, p + w * 0.02f, w, h, r, p * 0.95f, 110);
    rrect_fill(m, p, p, w, h, r, top, bot, 255);
    rrect_stroke(m, p, p, w, h, r, maxf(1.f, w * 0.011f), C_EDGE, 255);
}

void so_card_draw(so_art_t *a, so_img_t *m, int suit, int rank)
{
    so_img_clear(m);
    card_body(a, m, C_PAPER_T, C_PAPER_B);
    uint32_t col = so_suit_color(a, suit);
    float p = (float)a->pad, w = (float)a->cw, h = (float)a->ch;

    /* the index: the rank on the left, its suit on the right, both large,
     * so a column fanned down to its strip still reads */
    float ih = idx_h(a), sx;
    float iw = rank_w(rank, ih, &sx);
    float ix = p + w * 0.075f, iy = p + w * 0.055f;
    if (rank == 9) {
        ix -= w * 0.02f;
    }
    glyph(m, RANK_GLYPH[rank], ix, iy, ih, sx, ih * (a->deck == SO_DECK_LARGE ? 0.17f : 0.15f), col);
    (void)iw;
    int ps = (int)(ih * 0.92f + 0.5f);
    pip(a, m, suit, ps, (int)(p + w - w * 0.07f - (float)ps + 0.5f), (int)(iy + (ih - (float)ps) * 0.5f + 0.5f),
        col, false);

    /* the field below the strip */
    float fy0 = p + (float)a->strip, fy1 = p + h - w * 0.06f;
    float fx0 = p + w * 0.10f, fx1 = p + w - w * 0.10f;
    float fw = fx1 - fx0, fh = fy1 - fy0;
    float cx = (fx0 + fx1) * 0.5f, cy = (fy0 + fy1) * 0.5f;

    if (rank >= 10) {
        so_court_draw(m, suit, rank, (int)(p + w * 0.15f + 0.5f), (int)fy0,
                      (int)(w * 0.70f + 0.5f), (int)(fy1 - fy0 + 0.5f), a->deck == SO_DECK_FOUR);
        return;
    }
    if (rank == 0 || a->deck == SO_DECK_LARGE) {
        /* one big pip; the ace of spades gets its flourish */
        float s = minf(fw, fh) * (rank == 0 ? 0.70f : 0.62f);
        if (rank == 0 && suit == 0) {
            s *= 0.86f;
            ring(m, cx, cy, s * 0.80f, maxf(1.f, w * 0.014f), C_GOLD_D, 255);
            ring(m, cx, cy, s * 0.70f, maxf(1.f, w * 0.008f), C_GOLD_D, 150);
        }
        int is = (int)(s + 0.5f);
        pip(a, m, suit, is, (int)(cx - (float)is * 0.5f + 0.5f), (int)(cy - (float)is * 0.5f + 0.5f), col,
            false);
        return;
    }
    int n = rank + 1;
    float ps2 = w * (n >= 9 ? 0.18f : 0.20f);
    int   isz = (int)(ps2 + 0.5f);
    float colx[3] = { p + w * 0.30f, cx, p + w * 0.70f };
    for (int i = 0; i < NSPOTS[n]; i++) {
        const spot_t *s = &SPOTS[n][i];
        float px = colx[s->col + 1];
        float py = fy0 + (float)isz * 0.5f + s->t * (fh - (float)isz);
        pip(a, m, suit, isz, (int)(px - (float)isz * 0.5f + 0.5f), (int)(py - (float)isz * 0.5f + 0.5f), col,
            s->turn);
    }
}

/* --- backs ------------------------------------------------------------------------- */

/* one pixel of the printed field, in field coordinates (0..fw, 0..fh) */
static uint32_t back_px(int b, float x, float y, float fw, float fh, float u)
{
    float cx = fw * 0.5f, cy = fh * 0.5f;
    float dx = x - cx, dy = y - cy;
    switch (b) {
    case 0:
    case 1: {   /* a lattice of diagonals, lighter where they cross */
        static const uint32_t B[2][4] = { { 0x9A1A2A, 0xC03446, 0x6E0F1C, 0xF0C8CE },
                                          { 0x1E3F8A, 0x3A62B8, 0x122A60, 0xC8D6F0 } };
        int ux = (int)(x / (u * 1.1f)), uy = (int)(y / (u * 1.1f));
        int pu = (ux + uy) & 7, pv = (ux - uy + 4096) & 7;
        if (pu == 0 && pv == 0) return B[b][3];
        if (pu == 0 || pv == 0) return B[b][1];
        if (pu == 4 && pv == 4) return B[b][2];
        return B[b][0];
    }
    case 2: {   /* deco: a sunburst behind nested frames */
        float a = atan2f(dy, dx) * 24.f / 3.14159265f;
        bool ray = ((int)floorf(a) & 1) != 0;
        uint32_t c = ray ? 0x0F4A4F : 0x0B3A3E;
        float d = maxf(fabsf(dx) / fw, fabsf(dy) / fh) * 2.f;     /* 0 centre, 1 edge */
        float k = fmodf(d * 9.f, 1.f);
        if (d > 0.28f && k < 0.12f) c = lerp_rgb(c, C_GOLD, 0.75f);
        return lerp_rgb(c, 0x062224, d * 0.6f);
    }
    case 3: {   /* night: stars on deep blue */
        uint32_t c = lerp_rgb(0x1A2A66, 0x080C26, (y / fh) * 0.9f);
        int gx = (int)(x / (u * 3.f)), gy = (int)(y / (u * 3.f));
        uint32_t h = hash2(gx, gy);
        if ((h & 15) == 0) {
            float sx2 = ((float)gx + 0.25f + (float)((h >> 8) & 127) / 254.f) * u * 3.f;
            float sy2 = ((float)gy + 0.25f + (float)((h >> 16) & 127) / 254.f) * u * 3.f;
            float r = (float)(1 + ((h >> 4) & 1)) * u * 0.45f;
            float dd = sqrtf((x - sx2) * (x - sx2) + (y - sy2) * (y - sy2));
            if (dd < r * 2.2f) c = lerp_rgb(c, 0xFFF4D0, clampf(1.f - dd / (r * 2.2f), 0.f, 1.f));
        }
        return c;
    }
    case 4: {   /* waves */
        float w = sinf(x / (u * 3.2f)) * u * 1.6f;
        float k = fmodf((y + w) / (u * 4.5f) + 100.f, 3.f);
        uint32_t c = k < 1.f ? 0x0E6E86 : (k < 2.f ? 0x13879E : 0x0A5468);
        float e = fmodf((y + w) / (u * 4.5f) + 100.f, 1.f);
        if (e < 0.14f) c = lerp_rgb(c, 0xD8F4F6, 0.55f);
        return c;
    }
    case 5: {   /* tartan: bands across and down, a twill over them */
        float sx = fmodf(x / u + 400.f, 24.f), sy = fmodf(y / u + 400.f, 24.f);
        uint32_t base = 0x1A5232;
        uint32_t hx = sx < 6.f ? 0x1F2E5E : (sx < 7.5f ? 0xC9A227 : (sx > 16.f && sx < 20.f ? 0xA0202A : base));
        uint32_t hy = sy < 6.f ? 0x1F2E5E : (sy < 7.5f ? 0xC9A227 : (sy > 16.f && sy < 20.f ? 0xA0202A : base));
        bool twill = ((int)((x + y) / maxf(1.f, u * 0.7f)) & 1) != 0;
        return lerp_rgb(twill ? hx : hy, twill ? hy : hx, 0.35f);
    }
    case 6: {   /* harlequin */
        float s = u * 7.f;
        float gx = x / s, gy = y / (s * 1.4f);
        float fx = gx - floorf(gx) - 0.5f, fy = gy - floorf(gy) - 0.5f;
        bool in = fabsf(fx) + fabsf(fy) < 0.5f;
        int par = ((int)floorf(gx) + (int)floorf(gy)) & 1;
        uint32_t c = in ? (par ? 0x5B2A86 : 0x7A3FA8) : (par ? 0x3C1A5C : 0x46206A);
        float e = fabsf(fabsf(fx) + fabsf(fy) - 0.5f);
        if (e < 0.03f) c = lerp_rgb(c, C_GOLD, 0.7f);
        return c;
    }
    default: {  /* gold on black: frames and corner fans */
        uint32_t c = 0x15130F;
        float d = maxf(fabsf(dx) / (fw * 0.5f), fabsf(dy) / (fh * 0.5f));
        float k = fmodf(d * 7.f, 1.f);
        if (d > 0.45f && k > 0.82f) c = lerp_rgb(c, C_GOLD, 0.8f);
        float ang = atan2f(fabsf(dy), fabsf(dx));
        if (d > 0.45f && fmodf(ang * 12.f / 3.14159265f, 1.f) < 0.08f) c = lerp_rgb(c, C_GOLD_D, 0.6f);
        return c;
    }
    }
}

void so_back_draw(so_art_t *a, so_img_t *m, int b)
{
    so_img_clear(m);
    card_body(a, m, C_PAPER_T, C_PAPER_B);
    float p = (float)a->pad, w = (float)a->cw, h = (float)a->ch;
    float u = w / 76.f;
    float mg = w * 0.065f, r = maxf(2.f, w * 0.045f);
    float fx = p + mg, fy = p + mg, fw = w - 2 * mg, fh = h - 2 * mg;
    for (int y = (int)fy - 1; y <= (int)(fy + fh) + 1; y++) {
        for (int x = (int)fx - 1; x <= (int)(fx + fw) + 1; x++) {
            float d = sd_rrect((float)x + 0.5f, (float)y + 0.5f, fx, fy, fw, fh, r);
            float cov = clampf(0.5f - d, 0.f, 1.f);
            if (cov > 0) {
                /* 2 x 2 samples: the patterns have hard edges */
                int rr = 0, gg = 0, bb = 0;
                for (int s = 0; s < 4; s++) {
                    uint32_t c = back_px(b, (float)x - fx + 0.25f + 0.5f * (float)(s & 1),
                                         (float)y - fy + 0.25f + 0.5f * (float)(s >> 1), fw, fh, u);
                    rr += (int)((c >> 16) & 255);
                    gg += (int)((c >> 8) & 255);
                    bb += (int)(c & 255);
                }
                so_plot(m, x, y, ((uint32_t)(rr / 4) << 16) | ((uint32_t)(gg / 4) << 8) | (uint32_t)(bb / 4),
                        (int)(cov * 255.f));
            }
        }
    }
    float cx = p + w * 0.5f, cy = p + h * 0.5f;
    switch (b) {
    case 0:
    case 1: {
        uint32_t dk = b ? 0x0E1F48 : 0x5A0B17;
        rrect_stroke(m, fx + u * 3, fy + u * 3, fw - u * 6, fh - u * 6, u * 2, u, b ? 0xD6E0F4 : 0xF3D7DB, 200);
        rrect_fill(m, cx - u * 17, cy - u * 21, u * 34, u * 42, u * 17, dk, shade(dk, 70), 255);
        rrect_stroke(m, cx - u * 17, cy - u * 21, u * 34, u * 42, u * 17, u * 1.6f, C_GOLD, 255);
        /* a rosette of four petals */
        for (int i = 0; i < 4; i++) {
            float ax = (i & 1 ? 1.f : -1.f) * u * 5.5f * (i < 2 ? 1.f : 0.f);
            float ay = (i & 1 ? 1.f : -1.f) * u * 7.f * (i < 2 ? 0.f : 1.f);
            disc(m, cx + ax, cy + ay, u * 5.f, C_GOLD, 255);
        }
        disc(m, cx, cy, u * 3.6f, dk, 255);
        disc(m, cx, cy, u * 2.2f, C_GOLD, 255);
        break;
    }
    case 2:
        rrect_fill(m, cx - u * 9, cy - u * 9, u * 18, u * 18, u * 2, C_GOLD, C_GOLD_D, 255);
        rrect_fill(m, cx - u * 6, cy - u * 6, u * 12, u * 12, u * 1.5f, 0x0B3A3E, 0x062224, 255);
        disc(m, cx, cy, u * 2.5f, C_GOLD, 255);
        break;
    case 3: {   /* the moon: a disc with a bite out of it */
        float R = u * 11.f;
        for (int y = (int)(cy - R) - 1; y <= (int)(cy + R) + 1; y++) {
            for (int x = (int)(cx - R) - 1; x <= (int)(cx + R) + 1; x++) {
                float px = (float)x + 0.5f - cx, py = (float)y + 0.5f - cy;
                float d1 = R - sqrtf(px * px + py * py);
                float qx = px - R * 0.42f, qy = py + R * 0.18f;
                float d2 = sqrtf(qx * qx + qy * qy) - R * 0.86f;
                float cov = clampf(minf(d1, d2) + 0.5f, 0.f, 1.f);
                if (cov > 0) so_plot(m, x, y, 0xFFE9A8, (int)(cov * 255.f));
            }
        }
        break;
    }
    case 4:
        rrect_fill(m, cx - u * 13, cy - u * 13, u * 26, u * 26, u * 13, 0xF4FBFB, 0xD8EEF0, 255);
        ring(m, cx, cy, u * 13, u * 1.2f, 0x0A5468, 255);
        for (int i = 0; i < 3; i++) {
            float yy = cy - u * 4 + (float)i * u * 4;
            for (int x = (int)(cx - u * 9); x <= (int)(cx + u * 9); x++) {
                float wy = yy + sinf(((float)x - cx) / (u * 2.f)) * u * 1.3f;
                disc(m, (float)x, wy, u * 0.8f, 0x0E6E86, 255);
            }
        }
        break;
    case 5:
        break;
    case 6:
        rrect_stroke(m, fx + u * 2, fy + u * 2, fw - u * 4, fh - u * 4, u * 1.5f, u * 1.2f, C_GOLD, 220);
        break;
    default: {
        rrect_stroke(m, fx + u * 2.5f, fy + u * 2.5f, fw - u * 5, fh - u * 5, u * 2, u * 0.9f, C_GOLD, 255);
        disc(m, cx, cy, u * 10.f, 0x15130F, 255);
        ring(m, cx, cy, u * 10.f, u * 1.4f, C_GOLD, 255);
        for (int i = 0; i < 4; i++) {
            float ang = (float)i * 3.14159265f / 2.f;
            disc(m, cx + cosf(ang) * u * 4.5f, cy + sinf(ang) * u * 4.5f, u * 3.6f, C_GOLD, 255);
        }
        disc(m, cx, cy, u * 2.6f, 0x15130F, 255);
        break;
    }
    }
}

/* --- empty piles --------------------------------------------------------------------- */

void so_slot_draw(so_art_t *a, so_img_t *m, int kind)
{
    so_img_clear(m);
    float p = (float)a->pad, w = (float)a->cw, h = (float)a->ch;
    float r = maxf(4.f, w * 0.07f);
    rrect_fill(m, p, p, w, h, r, 0x000000, 0x000000, 46);
    rrect_stroke(m, p, p, w, h, r, maxf(1.5f, w * 0.022f), 0xFFFFFF, 80);
    float cx = p + w * 0.5f, cy = p + h * 0.5f;
    uint32_t c = 0xFFFFFF;
    int opa = 70;
    if (kind == SO_SLOT_HOME || kind == SO_SLOT_KING) {
        float gh = w * 0.42f, sx;
        float gw = rank_w(kind == SO_SLOT_HOME ? 0 : 12, gh, &sx);
        so_img_t t = so_img_new(a->cv_w, a->cv_h);
        if (t.px) {
            glyph(&t, RANK_GLYPH[kind == SO_SLOT_HOME ? 0 : 12], cx - gw * 0.5f, cy - gh * 0.5f, gh, sx,
                  gh * 0.12f, c);
            for (int i = 0; i < t.w * t.h; i++) {
                int al = (int)(t.px[i] >> 24);
                if (al) so_plot(m, i % t.w, i / t.w, c, al * opa / 255);
            }
            so_img_free(&t);
        }
    } else if (kind == SO_SLOT_STOCK || kind == SO_SLOT_DONE) {
        /* a ring with an arrow head: it turns over again; spent, a bar across */
        float R = w * 0.22f, bw = maxf(2.f, w * 0.05f);
        for (int y = (int)(cy - R - bw) - 1; y <= (int)(cy + R + bw) + 1; y++) {
            for (int x = (int)(cx - R - bw) - 1; x <= (int)(cx + R + bw) + 1; x++) {
                float px = (float)x + 0.5f - cx, py = (float)y + 0.5f - cy;
                float d = fabsf(sqrtf(px * px + py * py) - R) - bw * 0.5f;
                float ang = atan2f(py, px);
                if (kind == SO_SLOT_STOCK && ang > -1.2f && ang < -0.3f) continue;
                float cov = clampf(0.5f - d, 0.f, 1.f);
                if (cov > 0) so_plot(m, x, y, c, (int)(cov * (float)opa * 1.4f));
            }
        }
        if (kind == SO_SLOT_STOCK) {
            /* the arrow head at the gap's end, pointing round */
            float ax = cx + cosf(-0.3f) * R, ay = cy + sinf(-0.3f) * R;
            for (int y = (int)(ay - bw * 3); y <= (int)(ay + bw * 3); y++) {
                for (int x = (int)(ax - bw * 3); x <= (int)(ax + bw * 3); x++) {
                    float px = (float)x + 0.5f - ax, py = (float)y + 0.5f - ay;
                    /* a triangle pointing up-left, along the circle */
                    float u2 = px * 0.8f + py * 0.6f, v2 = -px * 0.6f + py * 0.8f;
                    if (v2 < bw * 2.f && v2 > -bw * 0.6f && fabsf(u2) < (bw * 2.f - v2) * 0.9f) {
                        so_plot(m, x, y, c, opa * 14 / 10);
                    }
                }
            }
        } else {
            for (int y = (int)(cy - R); y <= (int)(cy + R); y++) {
                for (int x = (int)(cx - R); x <= (int)(cx + R); x++) {
                    float px = (float)x + 0.5f - cx, py = (float)y + 0.5f - cy;
                    float d = fabsf(px + py) * 0.7071f - bw * 0.5f;
                    if (px * px + py * py < R * R) {
                        float cov = clampf(0.5f - d, 0.f, 1.f);
                        if (cov > 0) so_plot(m, x, y, c, (int)(cov * (float)opa * 1.4f));
                    }
                }
            }
        }
    }
}

/* --- felts ------------------------------------------------------------------------------ */

static uint32_t felt_px(int f, int x, int y, int w, int h)
{
    float fx = (float)x, fy = (float)y, W = (float)w, H = (float)h;
    float dx = (fx - W * 0.5f) / (W * 0.75f + H * 0.25f), dy = (fy - H * 0.42f) / (H * 0.7f);
    float d = sqrtf(dx * dx + dy * dy);
    int grain = (int)(hash2(x, y) & 7) - 3;
    uint32_t c;
    switch (f) {
    default:
    case 0: c = lerp_rgb(0x2A8A55, 0x0B3D22, d * 1.25f); break;
    case 1: c = lerp_rgb(0x2D62AE, 0x0B1F44, d * 1.25f); break;
    case 2: c = lerp_rgb(0x9C2A33, 0x37090F, d * 1.25f); break;
    case 3: {   /* boards running down, with their grain */
        float pw = W / 6.f;
        int   pl = (int)(fx / pw);
        float lx = fx - (float)pl * pw;
        uint32_t ph = hash2(pl, 77);
        float ofs = (float)(ph & 1023);
        float n = vnoise(fx * 0.02f + (float)pl * 3.f, (fy + ofs) * 0.004f) * 6.f;
        float g = sinf(fx * 0.11f + n * 2.2f + (float)(ph & 7));
        g = g * 0.5f + 0.5f;
        float fine = vnoise(fx * 0.5f, fy * 0.03f + ofs);
        uint32_t lt = (ph & 8) ? 0xA8703F : 0x9A6436, dk = 0x5E3A1D;
        c = lerp_rgb(lt, dk, g * 0.55f + fine * 0.25f);
        c = lerp_rgb(c, 0x2A170A, d * 0.55f);
        if (lx < 1.5f) c = shade(c, 55);
        else if (lx < 3.f) c = shade(c, 112);
        break;
    }
    case 4: {   /* leather, a stitched seam around */
        float n1 = vnoise(fx * 0.35f, fy * 0.35f), n2 = vnoise(fx * 0.06f, fy * 0.06f);
        c = lerp_rgb(0x7A4A28, 0x2E180A, d * 1.1f + (n1 - 0.5f) * 0.18f + (n2 - 0.5f) * 0.2f);
        float e = minf(minf(fx, W - 1 - fx), minf(fy, H - 1 - fy));
        if (e > 14.f && e < 17.f) {
            float along = (e == fx || e == W - 1 - fx) ? fy : fx;
            if (fmodf(along, 16.f) < 10.f) c = 0xD9B98A;
            else c = shade(c, 70);
        } else if (e >= 17.f && e < 19.f) {
            c = shade(c, 78);
        } else if (e < 3.f) {
            c = shade(c, 60);
        }
        break;
    }
    case 5: {   /* night: stars over a faint nebula */
        c = lerp_rgb(0x182660, 0x04060F, fy / H * 0.85f + d * 0.25f);
        float n = vnoise(fx * 0.006f + 3.f, fy * 0.006f);
        c = lerp_rgb(c, 0x4A2466, clampf((n - 0.55f) * 1.6f, 0.f, 0.45f));
        int gx = x / 9, gy = y / 9;
        uint32_t hh = hash2(gx, gy);
        if ((hh & 31) < 3) {
            float sx = (float)gx * 9.f + 1.f + (float)((hh >> 8) & 7);
            float sy = (float)gy * 9.f + 1.f + (float)((hh >> 12) & 7);
            float r = (hh & 32) ? 1.6f : 0.9f;
            float dd = sqrtf((fx - sx) * (fx - sx) + (fy - sy) * (fy - sy));
            if (dd < r * 2.f) c = lerp_rgb(c, 0xFFF6DA, clampf(1.f - dd / (r * 2.f), 0.f, 1.f));
        }
        grain = 0;
        break;
    }
    case 6: {   /* the sea seen from above: caustics */
        float a1 = sinf(fx * 0.045f + sinf(fy * 0.031f) * 2.f);
        float a2 = sinf(fy * 0.052f + sinf(fx * 0.027f) * 2.3f);
        float k = fabsf(a1 + a2);
        k = k > 1.6f ? (k - 1.6f) * 2.5f : 0.f;
        c = lerp_rgb(0x0F6878, 0x04262E, d * 1.2f);
        c = lerp_rgb(c, 0x7FD8DE, clampf(k, 0.f, 1.f) * 0.35f);
        break;
    }
    case 7: {   /* damask: the same flower tone on tone, rows offset */
        c = lerp_rgb(0x6A1A30, 0x2A0712, d * 1.2f);
        float tw = 96.f, th = 128.f;
        int   row = (int)floorf(fy / th);
        float ox = (row & 1) ? tw * 0.5f : 0.f;
        float u = fmodf(fx + ox, tw) - tw * 0.5f, v = fmodf(fy, th) - th * 0.5f;
        float ang = atan2f(v, u), rr = sqrtf(u * u + v * v);
        float petal = 22.f + 12.f * cosf(ang * 4.f) + 5.f * cosf(ang * 8.f);
        bool in = rr < petal || (fabsf(u) < 3.f && fabsf(v) < 52.f) || (fabsf(v) < 3.f && fabsf(u) < 40.f);
        bool core = rr < 7.f;
        if (in && !core) c = lerp_rgb(c, 0xB04A64, 0.16f);
        break;
    }
    }
    int r = (int)((c >> 16) & 0xFF) + grain, g = (int)((c >> 8) & 0xFF) + grain, b = (int)(c & 0xFF) + grain;
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

uint32_t so_felt_tone(int f)
{
    static const uint32_t T[SO_NFELTS] = { 0x0F4A2A, 0x10285A, 0x4A0F16, 0x4A2E17,
                                           0x3B2111, 0x0C1236, 0x08404C, 0x3E0E1E };
    return T[f < 0 || f >= SO_NFELTS ? 0 : f];
}

void so_felt_draw(so_img_t *img, int f, int ox, int oy)
{
    (void)ox;
    (void)oy;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            img->px[y * img->w + x] = 0xFF000000u | felt_px(f, x, y, img->w, img->h);
        }
    }
}

static const uint8_t BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

static inline uint16_t to565(uint32_t c, int x, int y)
{
    int d = BAYER[(y & 3) * 4 + (x & 3)];
    int r = (int)((c >> 16) & 0xFF) + (d >> 1);
    int g = (int)((c >> 8) & 0xFF) + (d >> 2);
    int b = (int)(c & 0xFF) + (d >> 1);
    r = r > 255 ? 255 : r;
    g = g > 255 ? 255 : g;
    b = b > 255 ? 255 : b;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void so_felt_draw565(uint16_t *dst, int w, int h, int f)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            dst[y * w + x] = to565(felt_px(f, x, y, w, h), x, y);
        }
    }
}

/* --- stamps ------------------------------------------------------------------------------ */

static inline void blend565(uint16_t *d, uint32_t s, int a)
{
    if (a <= 0) {
        return;
    }
    int r = (int)((s >> 16) & 0xFF), g = (int)((s >> 8) & 0xFF), b = (int)(s & 0xFF);
    if (a < 255) {
        uint16_t o = *d;
        int orr = ((o >> 11) & 31) << 3, og = ((o >> 5) & 63) << 2, ob = (o & 31) << 3;
        r = (r * a + orr * (255 - a)) / 255;
        g = (g * a + og * (255 - a)) / 255;
        b = (b * a + ob * (255 - a)) / 255;
    }
    *d = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void so_stamp565(uint16_t *dst, int dw, int dh, const so_img_t *src, int x, int y)
{
    int i0 = x < 0 ? -x : 0, j0 = y < 0 ? -y : 0;
    int i1 = src->w, j1 = src->h;
    if (x + i1 > dw) i1 = dw - x;
    if (y + j1 > dh) j1 = dh - y;
    for (int j = j0; j < j1; j++) {
        const uint32_t *s = &src->px[j * src->w];
        uint16_t *d = &dst[(y + j) * dw + x];
        for (int i = i0; i < i1; i++) {
            uint32_t p = s[i];
            blend565(&d[i], p, (int)(p >> 24));
        }
    }
}

void so_stamp565_rot(uint16_t *dst, int dw, int dh, const so_img_t *src, float cx, float cy,
                     float deg, float k)
{
    float a = deg * 3.14159265f / 180.f;
    float cs = cosf(a), sn = sinf(a);
    float hw = (float)src->w * 0.5f * k, hh = (float)src->h * 0.5f * k;
    float ex = fabsf(cs) * hw + fabsf(sn) * hh, ey = fabsf(sn) * hw + fabsf(cs) * hh;
    int x0 = (int)(cx - ex) - 1, x1 = (int)(cx + ex) + 1;
    int y0 = (int)(cy - ey) - 1, y1 = (int)(cy + ey) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= dw) x1 = dw - 1;
    if (y1 >= dh) y1 = dh - 1;
    float ik = 1.f / k;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f - cx, py = (float)y + 0.5f - cy;
            float u = (px * cs + py * sn) * ik + (float)src->w * 0.5f - 0.5f;
            float v = (-px * sn + py * cs) * ik + (float)src->h * 0.5f - 0.5f;
            if (u < -0.5f || v < -0.5f || u > (float)src->w - 0.5f || v > (float)src->h - 0.5f) {
                continue;
            }
            int iu = (int)floorf(u), iv = (int)floorf(v);
            float fu = u - (float)iu, fv = v - (float)iv;
            int   ra = 0, rr = 0, rg = 0, rb = 0;
            for (int q = 0; q < 4; q++) {
                int su = iu + (q & 1), sv = iv + (q >> 1);
                if (su < 0 || sv < 0 || su >= src->w || sv >= src->h) continue;
                float wq = ((q & 1) ? fu : 1.f - fu) * ((q >> 1) ? fv : 1.f - fv);
                uint32_t p = src->px[sv * src->w + su];
                int al = (int)((float)(p >> 24) * wq * 256.f);
                ra += al;
                rr += (int)((p >> 16) & 255) * al;
                rg += (int)((p >> 8) & 255) * al;
                rb += (int)(p & 255) * al;
            }
            if (ra <= 0) continue;
            uint32_t c = ((uint32_t)(rr / ra) << 16) | ((uint32_t)(rg / ra) << 8) | (uint32_t)(rb / ra);
            blend565(&dst[y * dw + x], c, ra >> 8);
        }
    }
}
