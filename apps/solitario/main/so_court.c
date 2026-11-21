/*
 * SOLITARIO - the jack, the queen and the king
 *
 * Figures of our own, not a copy of any printed deck: each is a short list
 * of shapes (ellipses, polygons, thick strokes, a suit) in a box of 100 x 70
 * units, which is HALF a card's picture, from its top edge down to the line
 * in the middle. The other half is the same list turned 180 degrees, as on
 * a real double-headed card, so the halves cannot disagree.
 *
 * Painting is per pixel, 3 x 3 samples: the last shape that holds a sample
 * gives its colour, which is the painter's order read backwards. Each shape
 * keeps its bounding box, so most tests are a comparison or two.
 *
 * The king holds a sceptre and wears the crown and an ermine collar; the
 * queen a rose, a tiara and long hair; the jack a lance with a pennant and a
 * feathered cap. The robe takes the suit's colour, and a pip of the suit is
 * sewn on the chest.
 */
#include "so_art.h"

#include <math.h>
#include <string.h>

enum { SH_ELL = 0, SH_POLY, SH_CAPS, SH_SUIT };

typedef struct {
    uint8_t  type, n;
    uint32_t color;
    float    p[16];             /* ELL cx cy rx ry; CAPS x0 y0 x1 y1 r;
                                 * POLY n points; SUIT cx cy r suit  */
    float    bx0, by0, bx1, by1;
} shape_t;

#define MAX_SHAPES 64

typedef struct {
    shape_t s[MAX_SHAPES];
    int     n;
} pic_t;

static void bbox(shape_t *s)
{
    switch (s->type) {
    case SH_ELL:
        s->bx0 = s->p[0] - s->p[2];
        s->bx1 = s->p[0] + s->p[2];
        s->by0 = s->p[1] - s->p[3];
        s->by1 = s->p[1] + s->p[3];
        break;
    case SH_CAPS: {
        float r = s->p[4];
        s->bx0 = (s->p[0] < s->p[2] ? s->p[0] : s->p[2]) - r;
        s->bx1 = (s->p[0] > s->p[2] ? s->p[0] : s->p[2]) + r;
        s->by0 = (s->p[1] < s->p[3] ? s->p[1] : s->p[3]) - r;
        s->by1 = (s->p[1] > s->p[3] ? s->p[1] : s->p[3]) + r;
        break;
    }
    case SH_SUIT:
        s->bx0 = s->p[0] - s->p[2];
        s->bx1 = s->p[0] + s->p[2];
        s->by0 = s->p[1] - s->p[2];
        s->by1 = s->p[1] + s->p[2];
        break;
    default:
        s->bx0 = s->by0 = 1e9f;
        s->bx1 = s->by1 = -1e9f;
        for (int i = 0; i < s->n; i++) {
            float x = s->p[2 * i], y = s->p[2 * i + 1];
            if (x < s->bx0) s->bx0 = x;
            if (x > s->bx1) s->bx1 = x;
            if (y < s->by0) s->by0 = y;
            if (y > s->by1) s->by1 = y;
        }
        break;
    }
}

static void ell(pic_t *q, float cx, float cy, float rx, float ry, uint32_t c)
{
    if (q->n >= MAX_SHAPES) return;
    shape_t *s = &q->s[q->n++];
    s->type = SH_ELL;
    s->color = c;
    s->p[0] = cx;
    s->p[1] = cy;
    s->p[2] = rx;
    s->p[3] = ry;
    bbox(s);
}

static void caps(pic_t *q, float x0, float y0, float x1, float y1, float r, uint32_t c)
{
    if (q->n >= MAX_SHAPES) return;
    shape_t *s = &q->s[q->n++];
    s->type = SH_CAPS;
    s->color = c;
    s->p[0] = x0;
    s->p[1] = y0;
    s->p[2] = x1;
    s->p[3] = y1;
    s->p[4] = r;
    bbox(s);
}

static void poly(pic_t *q, uint32_t c, int n, const float *pts)
{
    if (q->n >= MAX_SHAPES || n > 8) return;
    shape_t *s = &q->s[q->n++];
    s->type = SH_POLY;
    s->color = c;
    s->n = (uint8_t)n;
    memcpy(s->p, pts, (size_t)n * 2 * sizeof(float));
    bbox(s);
}

static void suitpip(pic_t *q, float cx, float cy, float r, int suit, uint32_t c)
{
    if (q->n >= MAX_SHAPES) return;
    shape_t *s = &q->s[q->n++];
    s->type = SH_SUIT;
    s->color = c;
    s->p[0] = cx;
    s->p[1] = cy;
    s->p[2] = r;
    s->p[3] = (float)suit;
    bbox(s);
}

static bool hit(const shape_t *s, float x, float y)
{
    if (x < s->bx0 || x > s->bx1 || y < s->by0 || y > s->by1) {
        return false;
    }
    switch (s->type) {
    case SH_ELL: {
        float dx = (x - s->p[0]) / s->p[2], dy = (y - s->p[1]) / s->p[3];
        return dx * dx + dy * dy <= 1.f;
    }
    case SH_CAPS: {
        float ax = s->p[0], ay = s->p[1], bx = s->p[2] - ax, by = s->p[3] - ay;
        float l2 = bx * bx + by * by;
        float t = l2 > 0 ? ((x - ax) * bx + (y - ay) * by) / l2 : 0;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        float ex = ax + bx * t - x, ey = ay + by * t - y;
        return ex * ex + ey * ey <= s->p[4] * s->p[4];
    }
    case SH_SUIT:
        return so_suit_test((int)s->p[3], (x - s->p[0]) / s->p[2], (y - s->p[1]) / s->p[2]);
    default: {
        bool in = false;
        for (int i = 0, j = s->n - 1; i < s->n; j = i++) {
            float xi = s->p[2 * i], yi = s->p[2 * i + 1], xj = s->p[2 * j], yj = s->p[2 * j + 1];
            if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
                in = !in;
            }
        }
        return in;
    }
    }
}

/* --- the figures ------------------------------------------------------------------- */

#define GOLD    0xE2B84E
#define GOLD_D  0xA27A1C
#define SKIN    0xF3CDA6
#define SKIN_D  0xD9A47C
#define WHITE   0xFFFDF8
#define INK     0x2A2230
#define LIPS    0xB4474C
#define CREAM   0xFFF6E3

typedef struct { uint32_t robe, lining, accent, hair; } pal_t;

static pal_t palette(int suit, int rank, bool four)
{
    static const pal_t P[4] = {
        { 0x2B4180, 0x1A2752, 0xC8342F, 0 },    /* spades: royal blue  */
        { 0xB4232F, 0x76121D, 0x2B4180, 0 },    /* hearts: crimson     */
        { 0x7B3B93, 0x4C2160, 0xE2B84E, 0 },    /* diamonds: purple    */
        { 0x2C7A47, 0x1A4E2D, 0xC8342F, 0 },    /* clubs: green        */
    };
    pal_t p = P[suit & 3];
    if (four && suit == 2) {
        p.robe = 0x2463C8;
        p.lining = 0x163E80;
    }
    static const uint32_t HAIR[3][4] = {
        { 0x5B3A1E, 0x7A4A22, 0x2E2018, 0x8A5A2A },     /* jacks  */
        { 0xC98F2E, 0x5A3218, 0xB0702A, 0x2E2018 },     /* queens */
        { 0xE9E4DA, 0xCFC8BC, 0xE0D8C8, 0xBDB5A8 },     /* kings  */
    };
    p.hair = HAIR[rank - 10][suit & 3];
    return p;
}

static void face(pic_t *q, uint32_t hair, bool brows_dark)
{
    ell(q, 50, 50, 5, 6, SKIN_D);                    /* the neck  */
    ell(q, 50, 31, 11, 13, SKIN);                    /* the head  */
    ell(q, 45.5f, 30, 1.7f, 1.3f, INK);              /* the eyes  */
    ell(q, 54.5f, 30, 1.7f, 1.3f, INK);
    caps(q, 43, 26.5f, 47.5f, 26, 0.7f, brows_dark ? INK : hair);
    caps(q, 52.5f, 26, 57, 26.5f, 0.7f, brows_dark ? INK : hair);
    caps(q, 50, 31, 49, 35.5f, 0.75f, SKIN_D);       /* the nose  */
    ell(q, 44, 35, 2.6f, 1.6f, 0xF2B4A4);            /* cheeks    */
    ell(q, 56, 35, 2.6f, 1.6f, 0xF2B4A4);
}

static void robe(pic_t *q, const pal_t *p, int suit)
{
    /* the shoulders, edged in gold, and the sleeves */
    const float tr[] = { 15, 71, 17, 57, 27, 47.5f, 40, 44.5f, 60, 44.5f, 73, 47.5f, 83, 57, 85, 71 };
    poly(q, GOLD, 8, tr);
    const float sh[] = { 17.5f, 71, 19.5f, 58, 28.5f, 49.5f, 40.5f, 46.5f, 59.5f, 46.5f, 71.5f, 49.5f,
                         80.5f, 58, 82.5f, 71 };
    poly(q, p->robe, 8, sh);
    caps(q, 27, 52, 22, 71, 1.0f, p->lining);
    caps(q, 73, 52, 78, 71, 1.0f, p->lining);
    const float v[] = { 37, 50, 63, 50, 50, 72 };
    poly(q, p->lining, 3, v);
    caps(q, 37, 50, 50, 72, 1.1f, GOLD);
    caps(q, 63, 50, 50, 72, 1.1f, GOLD);
    suitpip(q, 50, 61, 5.f, suit, WHITE);
}

static void king(pic_t *q, int suit, const pal_t *p)
{
    ell(q, 50, 30, 14, 15, p->hair);                 /* hair behind */
    robe(q, p, suit);
    /* ermine */
    ell(q, 50, 51, 21, 7, WHITE);
    for (int i = 0; i < 5; i++) {
        ell(q, 33.f + (float)i * 8.5f, 51.f + (i & 1 ? 1.5f : -0.5f), 0.9f, 1.6f, INK);
    }
    face(q, p->hair, false);
    /* the beard and the moustache */
    const float b[] = { 39, 33, 61, 33, 59, 44, 50, 52, 41, 44 };
    poly(q, p->hair, 5, b);
    caps(q, 44, 37.5f, 50, 36.5f, 1.3f, p->hair);
    caps(q, 50, 36.5f, 56, 37.5f, 1.3f, p->hair);
    caps(q, 47.5f, 40, 52.5f, 40, 0.8f, LIPS);
    /* the crown */
    const float cr[] = { 36.5f, 15, 39.5f, 2, 44, 10, 50, 0, 56, 10, 60.5f, 2, 63.5f, 15 };
    poly(q, GOLD, 7, cr);
    const float band[] = { 36.5f, 14, 63.5f, 14, 63.5f, 20, 36.5f, 20 };
    poly(q, GOLD_D, 4, band);
    ell(q, 50, 17, 2.2f, 2.2f, 0xC8263A);
    ell(q, 42, 17, 1.6f, 1.6f, 0x2B6FD8);
    ell(q, 58, 17, 1.6f, 1.6f, 0x2B6FD8);
    ell(q, 39.5f, 2.5f, 1.4f, 1.4f, WHITE);
    ell(q, 50, 1, 1.6f, 1.6f, WHITE);
    ell(q, 60.5f, 2.5f, 1.4f, 1.4f, WHITE);
    /* the sceptre in his hand */
    caps(q, 78, 12, 75, 68, 1.7f, GOLD_D);
    ell(q, 78.3f, 8, 4.2f, 4.2f, GOLD);
    caps(q, 78.3f, 2, 78.3f, 6, 0.8f, GOLD);
    caps(q, 76.3f, 3.8f, 80.3f, 3.8f, 0.8f, GOLD);
    ell(q, 75.5f, 58, 4.3f, 4, SKIN);
}

static void queen(pic_t *q, int suit, const pal_t *p)
{
    const float hl[] = { 36, 28, 64, 28, 68, 58, 58, 54, 42, 54, 32, 58 };
    poly(q, p->hair, 6, hl);                         /* long hair  */
    ell(q, 50, 30, 15, 16, p->hair);
    robe(q, p, suit);
    ell(q, 50, 50, 16, 5, WHITE);                    /* the ruff   */
    for (int i = 0; i < 6; i++) {
        ell(q, 37.f + (float)i * 5.2f, 53.5f, 2.2f, 1.6f, WHITE);
    }
    face(q, p->hair, true);
    caps(q, 47.5f, 39.5f, 52.5f, 39.5f, 0.9f, LIPS);
    /* the fringe */
    const float fr[] = { 38, 27, 42, 18, 58, 18, 62, 27, 55, 22, 45, 22 };
    poly(q, p->hair, 6, fr);
    /* the tiara */
    const float t[] = { 39, 19, 42, 12, 46, 16, 50, 9, 54, 16, 58, 12, 61, 19 };
    poly(q, GOLD, 7, t);
    ell(q, 50, 15, 1.7f, 1.7f, 0xC8263A);
    ell(q, 42, 14, 1.1f, 1.1f, WHITE);
    ell(q, 58, 14, 1.1f, 1.1f, WHITE);
    ell(q, 38.5f, 34, 1.3f, 1.3f, GOLD);             /* earrings   */
    ell(q, 61.5f, 34, 1.3f, 1.3f, GOLD);
    /* the rose */
    caps(q, 23, 30, 27, 64, 1.0f, 0x2E7A34);
    ell(q, 20, 42, 3.5f, 1.6f, 0x3C9442);
    ell(q, 28, 48, 3.5f, 1.6f, 0x3C9442);
    ell(q, 22.5f, 26, 6, 5.5f, 0xC2263A);
    ell(q, 22.5f, 25.5f, 3.4f, 3, 0x8E1426);
    ell(q, 21.5f, 25, 1.4f, 1.2f, 0xE8506A);
    ell(q, 27, 59, 4.3f, 4, SKIN);
}

static void jack(pic_t *q, int suit, const pal_t *p)
{
    ell(q, 50, 29, 13, 13.5f, p->hair);
    robe(q, p, suit);
    const float col[] = { 38, 47, 62, 47, 58, 55, 42, 55 };
    poly(q, WHITE, 4, col);                          /* the collar */
    face(q, p->hair, true);
    caps(q, 47.5f, 40, 52.5f, 39.5f, 0.8f, LIPS);
    const float fr[] = { 39, 26, 43, 19, 60, 19, 61, 26, 52, 22 };
    poly(q, p->hair, 5, fr);
    /* the cap, and its feather */
    ell(q, 48, 18, 16, 6.5f, p->accent);
    const float brim[] = { 33, 20, 63, 20, 63, 23, 33, 23 };
    poly(q, GOLD_D, 4, brim);
    const float fe[] = { 58, 17, 66, 9, 78, 1, 74, 9, 64, 17 };
    poly(q, WHITE, 5, fe);
    caps(q, 60, 17, 77, 2, 0.5f, GOLD_D);
    /* the lance with its pennant */
    caps(q, 81, 6, 77, 68, 1.4f, 0x8A5A2B);
    const float tip[] = { 81.5f, -2, 84, 7, 81, 9, 79, 7 };
    poly(q, 0xC8CED6, 4, tip);
    const float pen[] = { 80.5f, 10, 96, 14, 80, 19 };
    poly(q, p->accent, 3, pen);
    ell(q, 77.5f, 58, 4.3f, 4, SKIN);
}

void so_court_draw(so_img_t *img, int suit, int rank, int x, int y, int w, int h, bool four)
{
    static pic_t q;             /* 64 shapes: not on the caller's stack */
    q.n = 0;
    pal_t p = palette(suit, rank, four);
    if (rank == 12) {
        king(&q, suit, &p);
    } else if (rank == 11) {
        queen(&q, suit, &p);
    } else {
        jack(&q, suit, &p);
    }

    /* the half picture's units into pixels: uniform, centred */
    float half = (float)h * 0.5f;
    float s = (float)w / 100.f;
    if (70.f * s > half) s = half / 70.f;
    float ox = (float)x + ((float)w - 100.f * s) * 0.5f;
    float midy = (float)y + half;
    float oy = midy - 70.f * s;

    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            int r = 0, g = 0, b = 0, n = 0;
            for (int sy = 0; sy < 3; sy++) {
                for (int sx = 0; sx < 3; sx++) {
                    float fx = (float)px + ((float)sx + 0.5f) / 3.f;
                    float fy = (float)py + ((float)sy + 0.5f) / 3.f;
                    float ux, uy;
                    if (fy < midy) {
                        ux = (fx - ox) / s;
                        uy = (fy - oy) / s;
                    } else {                    /* the other half, turned */
                        ux = 100.f - (fx - ox) / s;
                        uy = 70.f - (fy - midy) / s;
                    }
                    uint32_t c = CREAM;
                    for (int i = q.n - 1; i >= 0; i--) {
                        if (hit(&q.s[i], ux, uy)) {
                            c = q.s[i].color;
                            break;
                        }
                    }
                    r += (int)((c >> 16) & 255);
                    g += (int)((c >> 8) & 255);
                    b += (int)(c & 255);
                    n++;
                }
            }
            so_plot(img, px, py, ((uint32_t)(r / n) << 16) | ((uint32_t)(g / n) << 8) | (uint32_t)(b / n), 255);
        }
    }
    /* the line between the halves, and the frame */
    for (int px = x; px < x + w; px++) {
        so_plot(img, px, (int)midy, GOLD_D, 200);
    }
    for (int px = x; px < x + w; px++) {
        so_plot(img, px, y, GOLD_D, 255);
        so_plot(img, px, y + h - 1, GOLD_D, 255);
    }
    for (int py = y; py < y + h; py++) {
        so_plot(img, x, py, GOLD_D, 255);
        so_plot(img, x + w - 1, py, GOLD_D, 255);
    }
}
