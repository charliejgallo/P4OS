/*
 * CHATARRA - pixel art engine (see ch_pixel.h)
 *
 * Copied from arkanos by way of Claude Jump, with the names changed. On
 * P4OS every primitive learned the scale (ch_pixel.h, "logical and physical
 * pixels") and the watch's hand upscaler went away: the OS scales the canvas.
 */
#include "ch_pixel.h"

#include <string.h>

/* --------------------------------------------------------------------------
 * Buffer and clip
 * -------------------------------------------------------------------------- */

void ch_buf_escala(ch_buf_t *b, uint16_t *px, int lw, int lh, int stride, int s)
{
    if (s < 1) s = 1;
    b->px = px;
    b->s  = (int16_t)s;
    b->ox = 0;
    b->oy = 0;
    b->lw = (int16_t)lw;
    b->lh = (int16_t)lh;
    b->w  = (int16_t)(lw * s);
    b->h  = (int16_t)(lh * s);
    b->stride = (int16_t)stride;
    ch_clip_none(b);
}

void ch_buf_init(ch_buf_t *b, uint16_t *px, int w, int h)
{
    ch_buf_escala(b, px, w, h, w, 1);
}

void ch_clip(ch_buf_t *b, int x0, int y0, int x1, int y1)
{
    x0 = ch_fx(b, x0); x1 = ch_fx(b, x1);
    y0 = ch_fy(b, y0); y1 = ch_fy(b, y1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > b->w) x1 = b->w;
    if (y1 > b->h) y1 = b->h;
    b->cx0 = (int16_t)x0;
    b->cy0 = (int16_t)y0;
    b->cx1 = (int16_t)(x1 > x0 ? x1 : x0);
    b->cy1 = (int16_t)(y1 > y0 ? y1 : y0);
}

void ch_clip_none(ch_buf_t *b)
{
    b->cx0 = 0;
    b->cy0 = 0;
    b->cx1 = b->w;
    b->cy1 = b->h;
}

ch_buf_t ch_nativo(const ch_buf_t *b)
{
    ch_buf_t n = *b;
    n.s  = 1;
    n.ox = 0;
    n.oy = 0;
    n.lw = b->w;
    n.lh = b->h;
    return n;
}

uint16_t ch_mix(uint16_t a, uint16_t b, int f)
{
    if (f <= 0) return a;
    if (f >= 16) return b;

    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;

    int r  = ar + (br - ar) * f / 16;
    int g  = ag + (bg - ag) * f / 16;
    int bl = ab + (bb - ab) * f / 16;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

uint16_t ch_tone(uint16_t c, int f)
{
    return f < 0 ? ch_mix(c, 0x0000, -f) : ch_mix(c, 0xFFFF, f);
}

/* --------------------------------------------------------------------------
 * Dirty rectangles
 *
 * The list is deliberately short. Every rectangle costs twice: once on
 * restoring and once on presenting, and LVGL also builds a draw area per
 * invalidation. Twenty rectangles of ten pixels come out more expensive than
 * one of two hundred, so on adding, something to merge with is looked for
 * first.
 *
 * The merge criterion is the wasted area: it is accepted if the rectangle
 * enclosing both adds no more than CH_JOIN_SLACK pixels to the sum of the two
 * separately.
 *
 * The cap also has a reason and it is not memory: LVGL stores the invalid
 * areas in a buffer of LV_INV_BUF_SIZE (32 by default), and when it fills up
 * it throws the list away and stores THE WHOLE SCREEN. Which means
 * overshooting does not cost a little more: it costs a full repaint.
 * -------------------------------------------------------------------------- */

#define CH_JOIN_SLACK   380

static inline int rect_area(const ch_rect_t *r)
{
    return (r->x1 - r->x0) * (r->y1 - r->y0);
}

/* how much 'a' grows if it swallows 'b', in pixels */
static int join_cost(const ch_rect_t *a, const ch_rect_t *b)
{
    int x0 = a->x0 < b->x0 ? a->x0 : b->x0;
    int y0 = a->y0 < b->y0 ? a->y0 : b->y0;
    int x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    int y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    return (x1 - x0) * (y1 - y0) - rect_area(a) - rect_area(b);
}

static void join_into(ch_rect_t *a, const ch_rect_t *b)
{
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
}

void ch_dirty_init(ch_dirty_t *d, int lw, int lh)
{
    d->lw = (int16_t)lw;
    d->lh = (int16_t)lh;
    ch_dirty_reset(d);
}

void ch_dirty_reset(ch_dirty_t *d)
{
    d->n = 0;
    d->all = false;
}

void ch_dirty_all(ch_dirty_t *d)
{
    d->n = 0;
    d->all = true;
}

void ch_dirty_add(ch_dirty_t *d, int x, int y, int w, int h)
{
    if (d->all || w <= 0 || h <= 0) {
        return;
    }
    ch_rect_t n;
    n.x0 = (int16_t)(x < 0 ? 0 : x);
    n.y0 = (int16_t)(y < 0 ? 0 : y);
    n.x1 = (int16_t)(x + w > d->lw ? d->lw : x + w);
    n.y1 = (int16_t)(y + h > d->lh ? d->lh : y + h);
    if (n.x1 <= n.x0 || n.y1 <= n.y0) {
        return;
    }

    int best = -1, best_cost = 0;
    for (int i = 0; i < d->n; i++) {
        int cost = join_cost(&d->r[i], &n);
        if (best < 0 || cost < best_cost) {
            best = i;
            best_cost = cost;
        }
    }

    /* If there is room in the list, it only merges when it comes cheap. If
     * there is not, it merges anyway with the cheapest: losing a rectangle is
     * worse than repainting too much. */
    if (best >= 0 && (best_cost <= CH_JOIN_SLACK || d->n >= CH_MAX_DIRTY)) {
        join_into(&d->r[best], &n);
        return;
    }
    d->r[d->n++] = n;
}

void ch_dirty_join(ch_dirty_t *dst, const ch_dirty_t *src)
{
    if (dst->all) {
        return;
    }
    if (src->all) {
        ch_dirty_all(dst);
        return;
    }
    for (int i = 0; i < src->n; i++) {
        const ch_rect_t *r = &src->r[i];
        ch_dirty_add(dst, r->x0, r->y0, r->x1 - r->x0, r->y1 - r->y0);
    }
}

int ch_dirty_area(const ch_dirty_t *d)
{
    if (d->all) {
        return d->lw * d->lh;
    }
    int total = 0;
    for (int i = 0; i < d->n; i++) {
        total += rect_area(&d->r[i]);
    }
    return total;
}

void ch_restore(ch_buf_t *dst, const ch_buf_t *src, const ch_rect_t *r)
{
    int x0 = ch_fx(dst, r->x0), x1 = ch_fx(dst, r->x1);
    int y0 = ch_fy(dst, r->y0), y1 = ch_fy(dst, r->y1);

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dst->w) x1 = dst->w;
    if (y1 > dst->h) y1 = dst->h;
    if (x1 <= x0) {
        return;
    }
    for (int y = y0; y < y1; y++) {
        memcpy(dst->px + (size_t)y * dst->stride + x0,
               src->px + (size_t)y * src->stride + x0,
               (size_t)(x1 - x0) * sizeof(uint16_t));
    }
}

/* --------------------------------------------------------------------------
 * Primitives
 *
 * The physical workhorse is fill(): a clipped rectangle of canvas pixels.
 * Everything that keeps its blocks goes through it; everything drawn at the
 * physical resolution goes through span().
 * -------------------------------------------------------------------------- */

static void fill(ch_buf_t *b, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < b->cx0) x0 = b->cx0;
    if (y0 < b->cy0) y0 = b->cy0;
    if (x1 > b->cx1) x1 = b->cx1;
    if (y1 > b->cy1) y1 = b->cy1;
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    /* Two pixels per write. The canvas lives in PSRAM, where every short
     * write hurts. */
    uint32_t pair = ((uint32_t)c << 16) | c;

    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = &b->px[(size_t)yy * b->stride];
        int xx = x0;

        if ((xx & 1) && xx < x1) {
            row[xx++] = c;
        }
        int pairs = (x1 - xx) / 2;
        if (!((uintptr_t)(row + xx) & 3)) {
            uint32_t *p32 = (uint32_t *)(void *)&row[xx];
            for (int i = 0; i < pairs; i++) {
                p32[i] = pair;
            }
            xx += pairs * 2;
        }
        while (xx < x1) {
            row[xx++] = c;
        }
    }
}

/* One physical row, clipped. */
static inline void span(ch_buf_t *b, int x0, int x1, int y, uint16_t c)
{
    fill(b, x0, y, x1, y + 1, c);
}

void ch_px(ch_buf_t *b, int x, int y, uint16_t c)
{
    int X = ch_fx(b, x), Y = ch_fy(b, y);
    fill(b, X, Y, X + b->s, Y + b->s, c);
}

void ch_rect(ch_buf_t *b, int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    int X = ch_fx(b, x), Y = ch_fy(b, y);
    fill(b, X, Y, X + w * b->s, Y + h * b->s, c);
}

void ch_fill(ch_buf_t *b, uint16_t c)
{
    fill(b, 0, 0, b->w, b->h, c);
}

void ch_hline(ch_buf_t *b, int x, int y, int len, uint16_t c)
{
    ch_rect(b, x, y, len, 1, c);
}

void ch_vline(ch_buf_t *b, int x, int y, int len, uint16_t c)
{
    ch_rect(b, x, y, 1, len, c);
}

void ch_frame(ch_buf_t *b, int x, int y, int w, int h, uint16_t c)
{
    ch_hline(b, x, y, w, c);
    ch_hline(b, x, y + h - 1, w, c);
    ch_vline(b, x, y, h, c);
    ch_vline(b, x + w - 1, y, h, c);
}

void ch_line(ch_buf_t *b, int x0, int y0, int x1, int y1, uint16_t c)
{
    int s = b->s;
    int X0 = ch_fx(b, x0), Y0 = ch_fy(b, y0);
    int X1 = ch_fx(b, x1), Y1 = ch_fy(b, y1);
    int dx = X1 - X0, dy = Y1 - Y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;

    if (steps == 0) {
        fill(b, X0, Y0, X0 + s, Y0 + s, c);
        return;
    }
    for (int i = 0; i <= steps; i++) {
        int X = X0 + dx * i / steps, Y = Y0 + dy * i / steps;
        fill(b, X, Y, X + s, Y + s, c);
    }
}

/* The circles are drawn with doubled coordinates, so the centre can be the
 * middle of a logical pixel and the radius can end half a unit out: that is
 * exactly where the watch's discs ended, only now the edge is smooth. */
typedef struct { int cx2, cy2, r2; } circ_t;

static circ_t circ(const ch_buf_t *b, int cx, int cy, int r)
{
    circ_t k;
    k.cx2 = 2 * ch_fx(b, cx) + b->s;
    k.cy2 = 2 * ch_fy(b, cy) + b->s;
    k.r2  = (2 * r + 1) * b->s;
    return k;
}

/* Half the chord at physical row y, in physical pixels (doubled), or -1. */
static int chord(const circ_t *k, int y)
{
    int d = 2 * y + 1 - k->cy2;
    int q = k->r2 * k->r2 - d * d;
    return q < 0 ? -1 : ch_isqrt(q);
}

void ch_disc(ch_buf_t *b, int cx, int cy, int r, uint16_t c)
{
    if (r < 0) {
        return;
    }
    circ_t k = circ(b, cx, cy, r);
    int ya = (k.cy2 - k.r2) / 2, yb = (k.cy2 + k.r2) / 2;
    if (ya < b->cy0) ya = b->cy0;
    if (yb >= b->cy1) yb = b->cy1 - 1;
    for (int y = ya; y <= yb; y++) {
        int h = chord(&k, y);
        if (h < 0) continue;
        span(b, (k.cx2 - h + 1) / 2, (k.cx2 + h + 1) / 2, y, c);
    }
}

void ch_ring(ch_buf_t *b, int cx, int cy, int r, uint16_t c)
{
    if (r <= 0) {
        ch_px(b, cx, cy, c);
        return;
    }
    /* The band between r - 1/2 and r + 1/2 units: one unit thick, as the
     * watch's midpoint circle was, and round at any scale. */
    circ_t out = circ(b, cx, cy, r);
    circ_t in  = circ(b, cx, cy, r - 1);
    int ya = (out.cy2 - out.r2) / 2, yb = (out.cy2 + out.r2) / 2;
    if (ya < b->cy0) ya = b->cy0;
    if (yb >= b->cy1) yb = b->cy1 - 1;
    for (int y = ya; y <= yb; y++) {
        int ho = chord(&out, y), hi = chord(&in, y);
        if (ho < 0) continue;
        int xa = (out.cx2 - ho + 1) / 2, xb = (out.cx2 + ho + 1) / 2;
        if (hi < 0) {
            span(b, xa, xb, y, c);
        } else {
            span(b, xa, (in.cx2 - hi + 1) / 2, y, c);
            span(b, (in.cx2 + hi + 1) / 2, xb, y, c);
        }
    }
}

void ch_ellipse(ch_buf_t *b, int cx, int cy, int rx, int ry, uint16_t c)
{
    if (rx <= 0 || ry <= 0) {
        return;
    }
    /* Doubled coordinates again, and the chord of a circle squashed by
     * ry/rx: x^2/rx^2 + y^2/ry^2 <= 1, in integers. */
    int s = b->s;
    int cx2 = 2 * ch_fx(b, cx) + s, cy2 = 2 * ch_fy(b, cy) + s;
    int RX = (2 * rx + 1) * s, RY = (2 * ry + 1) * s;
    int ya = (cy2 - RY) / 2, yb = (cy2 + RY) / 2;
    if (ya < b->cy0) ya = b->cy0;
    if (yb >= b->cy1) yb = b->cy1 - 1;
    for (int y = ya; y <= yb; y++) {
        int d = 2 * y + 1 - cy2;
        int q = RY * RY - d * d;
        if (q < 0) continue;
        int h = ch_isqrt(q) * RX / RY;
        span(b, (cx2 - h + 1) / 2, (cx2 + h + 1) / 2, y, c);
    }
}

void ch_round(ch_buf_t *b, int x, int y, int w, int h, int cut, uint16_t c)
{
    int s = b->s;
    int X = ch_fx(b, x), Y = ch_fy(b, y);
    int W = w * s, H = h * s, C = cut * s;

    if (W <= 0 || H <= 0) {
        return;
    }
    if (C * 2 > H) C = H / 2;
    for (int yy = 0; yy < H; yy++) {
        int inset = 0;
        if (yy < C) {
            inset = C - yy;
        } else if (yy >= H - C) {
            inset = C - (H - 1 - yy);
        }
        span(b, X + inset, X + W - inset, Y + yy, c);
    }
}

void ch_vgrad(ch_buf_t *b, int x, int y0, int w, int y1, uint16_t top, uint16_t bot)
{
    if (y1 < y0) {
        return;
    }
    /* Row by PHYSICAL row: twice the steps the watch had, for nothing. The
     * blend is done in 1/64 and snapped to ch_mix's sixteenths by pairs of
     * dithered rows, so a tall sky does not come out in bands. */
    int X0 = ch_fx(b, x), X1 = ch_fx(b, x + w);
    int Y0 = ch_fy(b, y0), Y1 = ch_fy(b, y1 + 1);
    int span_y = Y1 - Y0 - 1;
    for (int y = Y0; y < Y1; y++) {
        int f64 = span_y > 0 ? (y - Y0) * 64 / span_y : 0;
        int f = f64 >> 2;
        if ((f64 & 3) >= 2 && (y & 1)) f++;
        span(b, X0, X1, y, ch_mix(top, bot, f));
    }
}

void ch_shade(ch_buf_t *b, int x, int y, int w, int h, int f)
{
    int x0 = ch_fx(b, x), y0 = ch_fy(b, y);
    int x1 = ch_fx(b, x + w), y1 = ch_fy(b, y + h);
    if (x0 < b->cx0) x0 = b->cx0;
    if (y0 < b->cy0) y0 = b->cy0;
    if (x1 > b->cx1) x1 = b->cx1;
    if (y1 > b->cy1) y1 = b->cy1;
    uint16_t target = f < 0 ? 0x0000 : 0xFFFF;
    int amount = f < 0 ? -f : f;

    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = &b->px[(size_t)yy * b->stride];
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = ch_mix(row[xx], target, amount);
        }
    }
}

/* The same, but towards a COLOUR instead of black or white. It is what gives
 * each zone its air: a thin wash of the zone's colour over the finished
 * background, once, when the room is built.
 *
 * `f` IS OUT OF SIXTEEN, not out of 255 - that is ch_mix()'s scale, and at 16
 * it returns the target colour and nothing else. Asking for 20 does not give
 * a strong wash, it gives a flat rectangle of paint; two or three is a wash. */
void ch_tint(ch_buf_t *b, int x, int y, int w, int h, uint16_t c, int f)
{
    int x0 = ch_fx(b, x), y0 = ch_fy(b, y);
    int x1 = ch_fx(b, x + w), y1 = ch_fy(b, y + h);
    if (x0 < b->cx0) x0 = b->cx0;
    if (y0 < b->cy0) y0 = b->cy0;
    if (x1 > b->cx1) x1 = b->cx1;
    if (y1 > b->cy1) y1 = b->cy1;

    if (f <= 0) return;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = &b->px[(size_t)yy * b->stride];
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = ch_mix(row[xx], c, f);
        }
    }
}

void ch_glow(ch_buf_t *b, int cx, int cy, int r, uint16_t c, int f)
{
    if (r <= 0) {
        return;
    }
    circ_t k = circ(b, cx, cy, r);
    int R2 = k.r2 * k.r2;
    /* The falloff is f*(1 - d/R2). Dividing by R2 on every pixel is
     * expensive: the reciprocal is computed once, in 1/65536, and after that
     * it is a multiplication - which cannot overflow, because d never passes
     * R2 and so d * inv never passes f << 16. No 64-bit arithmetic: on the
     * P4 that is a libgcc call per pixel. */
    int32_t inv = (int32_t)(((uint32_t)f << 16) / (uint32_t)(R2 ? R2 : 1));
    int ya = (k.cy2 - k.r2) / 2, yb = (k.cy2 + k.r2) / 2;
    if (ya < b->cy0) ya = b->cy0;
    if (yb >= b->cy1) yb = b->cy1 - 1;

    for (int y = ya; y <= yb; y++) {
        int h = chord(&k, y);
        if (h < 0) continue;
        int dy = 2 * y + 1 - k.cy2;
        int x0 = (k.cx2 - h + 1) / 2, x1 = (k.cx2 + h + 1) / 2;
        if (x0 < b->cx0) x0 = b->cx0;
        if (x1 > b->cx1) x1 = b->cx1;
        uint16_t *row = &b->px[(size_t)y * b->stride];
        for (int xx = x0; xx < x1; xx++) {
            int dx = 2 * xx + 1 - k.cx2;
            int d = dx * dx + dy * dy;
            row[xx] = ch_mix(row[xx], c, f - (int)(((int32_t)d * inv) >> 16));
        }
    }
}

void ch_wave(ch_buf_t *b, int cx, int cy, int r, int thick, uint16_t c, int f)
{
    if (r <= 0 || thick <= 0) {
        return;
    }
    int s = b->s;
    int CX = ch_fx(b, cx) + s / 2, CY = ch_fy(b, cy) + s / 2;
    int ro = (r + thick) * s, ri = r * s;
    int outer = ro * ro, inner = ri * ri;

    for (int dy = -ro; dy <= ro; dy++) {
        int yy = CY + dy;
        if (yy < b->cy0 || yy >= b->cy1) {
            continue;
        }
        uint16_t *row = &b->px[(size_t)yy * b->stride];
        int dy2 = dy * dy;
        for (int dx = -ro; dx <= ro; dx++) {
            int xx = CX + dx;
            if (xx < b->cx0 || xx >= b->cx1) {
                continue;
            }
            int d = dx * dx + dy2;
            if (d < inner || d > outer) {
                continue;
            }
            row[xx] = ch_mix(row[xx], c, f);
        }
    }
}

/* --------------------------------------------------------------------------
 * Integer trigonometry
 * -------------------------------------------------------------------------- */

static const uint8_t ch_sin_q[65] = {
      0,   6,  13,  19,  25,  31,  37,  44,  50,  56,  62,  68,  74,  80,  86,
     92,  98, 103, 109, 115, 120, 126, 131, 136, 142, 147, 152, 157, 162, 167,
    171, 176, 180, 185, 189, 193, 197, 201, 205, 208, 212, 215, 219, 222, 225,
    228, 231, 233, 236, 238, 240, 242, 244, 246, 247, 249, 250, 251, 252, 253,
    254, 254, 255, 255, 255
};

int ch_sin(int brad)
{
    brad &= 0xFF;
    if (brad <= 64)  return  ch_sin_q[brad];
    if (brad <= 128) return  ch_sin_q[128 - brad];
    if (brad <= 192) return -ch_sin_q[brad - 128];
    return -ch_sin_q[256 - brad];
}

int ch_cos(int brad)
{
    return ch_sin(brad + 64);
}

int ch_isqrt(int v)
{
    if (v <= 0) {
        return 0;
    }
    int r = v, prev;
    do {
        prev = r;
        r = (r + v / r) / 2;
    } while (r < prev);
    return prev;
}

/* --------------------------------------------------------------------------
 * THE UPSCALER
 *
 * Every sprite of this game is pixel art drawn for 8 and then 12 px cells,
 * and here a cell is 24, 36 or 48 canvas pixels. Blowing each pixel up into
 * a square would be the watch's look at twice the size; this does three
 * things on the way, all of them per SOURCE pixel and all of them
 * deterministic (the background is restored by rectangles, so the same
 * sprite must come out the same every time it is drawn):
 *
 *  1. THE CORNERS. EPX's rule (Scale2x): where two neighbours of a pixel
 *     agree with each other and not with the other two, the corner between
 *     them takes their colour. At x2 each corner is one physical pixel; at x3
 *     and x4 it is a small triangle, so a staircase becomes a 45-degree edge
 *     instead of bigger stairs. Transparent counts as a colour, so the
 *     silhouettes round off too.
 *
 *  2. THE BEVEL. A block whose upper neighbour is darker (the mortar above a
 *     brick, the outline above a roof) gets its top physical row lit; one
 *     whose lower neighbour is darker gets its bottom row shaded, and the
 *     sides the same, softer. The art already has its dark lines; this makes
 *     the blocks between them stand up.
 *
 *  3. THE GRAIN, only on ground tiles: one physical pixel in eight a
 *     sixteenth lighter or darker, keyed on the physical coordinate. Flat
 *     colour at 48 px a cell reads as plastic; this reads as ground.
 *
 * At s = 1 (the watch's scale, which nothing here uses) it is a plain blit.
 * -------------------------------------------------------------------------- */

static inline int luma(uint16_t c)
{
    return ((c >> 11) & 0x1F) * 614 / 64 + ((c >> 5) & 0x3F) * 601 / 128 +
           (c & 0x1F) * 117 / 64;
}

#define HD_UMBRAL   9           /* luma steps that count as "darker"         */

static inline uint16_t src_at(const uint16_t *src, int sw, int sh, int x, int y,
                              uint16_t propio, bool baldosa)
{
    if (x < 0 || y < 0 || x >= sw || y >= sh) return baldosa ? propio : 0;
    return src[y * sw + x];
}

void ch_blit_px(ch_buf_t *b, int x, int y, const uint16_t *src,
                int sw, int sh, int m, unsigned flags)
{
    int s = b->s * (m < 1 ? 1 : m);
    int X0 = ch_fx(b, x), Y0 = ch_fy(b, y);
    bool baldosa = (flags & CH_HD_BALDOSA) != 0;
    bool bisel = (flags & CH_HD_BISEL) != 0;

    if (s == 1) {
        for (int sy = 0; sy < sh; sy++) {
            int py = Y0 + sy;
            if (py < b->cy0 || py >= b->cy1) continue;
            uint16_t *row = &b->px[(size_t)py * b->stride];
            for (int sx = 0; sx < sw; sx++) {
                int px = X0 + sx;
                uint16_t c = src[sy * sw + sx];
                if (c && px >= b->cx0 && px < b->cx1) row[px] = c;
            }
        }
        return;
    }

    for (int sy = 0; sy < sh; sy++) {
        int py0 = Y0 + sy * s;
        if (py0 >= b->cy1) break;
        if (py0 + s <= b->cy0) continue;

        for (int sx = 0; sx < sw; sx++) {
            int px0 = X0 + sx * s;
            if (px0 >= b->cx1) break;
            if (px0 + s <= b->cx0) continue;

            uint16_t P = src[sy * sw + sx];
            uint16_t A = src_at(src, sw, sh, sx, sy - 1, P, baldosa);
            uint16_t B = src_at(src, sw, sh, sx + 1, sy, P, baldosa);
            uint16_t C = src_at(src, sw, sh, sx - 1, sy, P, baldosa);
            uint16_t D = src_at(src, sw, sh, sx, sy + 1, P, baldosa);

            uint16_t tl = P, tr = P, bl = P, br = P;
            if (C == A && C != D && A != B) tl = A;
            if (A == B && A != C && B != D) tr = B;
            if (D == C && D != B && C != A) bl = C;
            if (B == D && B != A && D != C) br = D;
            if (!P && !tl && !tr && !bl && !br) continue;

            /* The bevel's four edges, decided once per source pixel. */
            bool arriba = false, abajo = false, izq = false, der = false;
            uint16_t luz = P, luz2 = P, som = P, som2 = P;
            if (bisel && P) {
                int lp = luma(P);
                arriba = A && A != P && luma(A) + HD_UMBRAL < lp;
                izq    = C && C != P && luma(C) + HD_UMBRAL < lp;
                abajo  = (D && D != P && luma(D) + HD_UMBRAL < lp) ||
                         (!D && !baldosa);
                der    = B && B != P && luma(B) + HD_UMBRAL < lp;
                if (arriba) luz  = ch_tone(P, 4);
                if (izq)    luz2 = ch_tone(P, 2);
                if (abajo)  som  = ch_tone(P, -4);
                if (der)    som2 = ch_tone(P, -2);
            }

            for (int v = 0; v < s; v++) {
                int py = py0 + v;
                if (py < b->cy0 || py >= b->cy1) continue;
                uint16_t *row = &b->px[(size_t)py * b->stride];
                for (int u = 0; u < s; u++) {
                    int px = px0 + u;
                    uint16_t c;
                    if (px < b->cx0 || px >= b->cx1) continue;

                    if (2 * (u + v) < s)                       c = tl;
                    else if (2 * ((s - 1 - u) + v) < s)        c = tr;
                    else if (2 * (u + (s - 1 - v)) < s)        c = bl;
                    else if (2 * ((s - 1 - u) + (s - 1 - v)) < s) c = br;
                    else                                        c = P;
                    if (!c) continue;

                    if (c == P) {
                        if (v == 0 && arriba)          c = luz;
                        else if (v == s - 1 && abajo)  c = som;
                        else if (u == 0 && izq)        c = luz2;
                        else if (u == s - 1 && der)    c = som2;
                    }
                    if (baldosa) {
                        uint32_t hsh = (uint32_t)px * 73856093u ^
                                       (uint32_t)py * 19349663u;
                        hsh ^= hsh >> 13;
                        switch (hsh & 15) {
                        case 0: c = ch_tone(c, 1);  break;
                        case 1: c = ch_tone(c, -1); break;
                        default: break;
                        }
                    }
                    row[px] = c;
                }
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Palette and ASCII sprites
 * -------------------------------------------------------------------------- */

/* The palette is stored as a compact table and expanded ONCE into an array
 * indexed by the character.
 *
 * No colour may be 0x000000: zero is the "transparent" marker in the expanded
 * array. The near-black outline is 0x05060C precisely for that reason.
 */
static const struct { char c; uint32_t hex; } PALETA[] = {
    /* greys and blues of the metal (inherited from Claude Jump) */
    { 'k', 0x05060C }, { 'K', 0x101527 }, { 'x', 0x232B41 }, { 'd', 0x3D465F },
    { 'D', 0x606B85 }, { 'g', 0x99A3BC }, { 'G', 0xD5DCEB }, { 'w', 0xFFFFFF },
    /* vivid colours */
    { 'c', 0x7BE9FF }, { 'C', 0x18A6D8 }, { 'b', 0x4A9DF5 }, { 'B', 0x1F4FBF },
    { 'p', 0xB072F0 }, { 'P', 0x6A2FB5 }, { 'm', 0xFF6FAE }, { 'M', 0xC0246A },
    { 'r', 0xFF4A3D }, { 'R', 0xA31E1A }, { 'o', 0xFF9F0A }, { 'O', 0xC05A00 },
    { 'y', 0xFFE45E }, { 'Y', 0xE0A800 }, { 'v', 0x4ADE80 }, { 'V', 0x1E7A3C },
    { 'n', 0x2AF0C8 }, { 'N', 0x0E8A78 }, { 't', 0xD97757 }, { 'T', 0x8E4630 },
    /* earth, grass and water: what an RPG map needs */
    { 'e', 0x9BDD6E }, { 'E', 0x5FA83F },   /* light / dark grass       */
    { 'f', 0x2F7A3A }, { 'F', 0x1B4D28 },   /* foliage / dark foliage   */
    { 'h', 0xE9BE83 }, { 'H', 0xB07F45 },   /* dirt track               */
    { 'i', 0xBEC4D2 }, { 'I', 0x6E7688 },   /* light / dark stone       */
    { 'j', 0x8A5A32 }, { 'J', 0x54341C },   /* wood / dark wood         */
    { 'l', 0x63CDF2 }, { 'L', 0x2A78C8 },   /* shallow / deep water     */
    { 'q', 0xF2DFA8 }, { 'Q', 0xC9A96A },   /* sand / dark sand         */
    { 'u', 0xC06B2E }, { 'U', 0x6E3A16 },   /* rust / dark rust         */
    { 'a', 0xD8604E }, { 'A', 0x8A3227 },   /* brick / dark brick       */
    { 's', 0x2B3145 }, { 'S', 0x171B29 },   /* shadow / deep shadow     */

    /* --- the 2x art's ramps (ASSETS.md) ----------------------------------
     * Twice the pixels need more steps between light and dark, so every
     * material the world is made of gets one or two more tones, placed
     * BETWEEN the ones above so a ramp reads light to dark in order:
     *
     *   grass     1 e 2 E 3 f F 4      dirt   6 h H 5
     *   stone     7 i 8 I 9            wood   0 j J !
     *   water     $ l % L &            sand   ( q Q )
     *   rust      * u U ,              brick  - a A /
     *   skin      : h ;                metal  w G < g > D d x K k
     *   snow      w G ?                ice    [ c C ]
     *   lava      W y o O X Z          neon   ^ n N |
     *   purple    } p P {              pink   ~ m M       blue = b B `
     *   red       _ r R                spring z (a yellow-green tip)
     */
    { '1', 0xC8EE92 }, { '2', 0x7EC552 }, { '3', 0x468F3C }, { '4', 0x10321A },
    { '6', 0xF8DDB2 }, { '5', 0x7E5530 },
    { '7', 0xE4E8EF }, { '8', 0x959DAF }, { '9', 0x474E5F },
    { '0', 0xB8814F }, { '!', 0x34200F },
    { '$', 0xD5F5FF }, { '%', 0x3FA0E0 }, { '&', 0x1B4F98 },
    { '(', 0xFFF3D0 }, { ')', 0x9C7F4A },
    { '*', 0xE8965A }, { ',', 0x42200C },
    { '-', 0xF08C76 }, { '/', 0x52241C },
    { ':', 0xF4CCA4 }, { ';', 0xC8946A },
    { '<', 0xB4BED3 }, { '>', 0x7A859E },
    { '?', 0xADC1DC },
    { '[', 0xC2F8FF }, { ']', 0x0E5C86 },
    { 'W', 0xFFF1A8 }, { 'X', 0x7A2804 }, { 'Z', 0x3A1206 },
    { '^', 0xA8FFEA }, { '|', 0x06403A },
    { '}', 0xD8B4FF }, { '{', 0x3E1A72 },
    { '~', 0xFFC0DA },
    { '=', 0x9CCBFF }, { '`', 0x0F2A6E },
    { '_', 0xFF9A8A },
    { 'z', 0xE2F59A },
};

#define PAL_FIRST   32
#define PAL_LAST    127

static uint16_t s_pal[PAL_LAST - PAL_FIRST + 1];

void ch_pal_init(void)
{
    memset(s_pal, 0, sizeof(s_pal));
    for (unsigned i = 0; i < sizeof(PALETA) / sizeof(PALETA[0]); i++) {
        s_pal[(unsigned char)PALETA[i].c - PAL_FIRST] = ch_rgb(PALETA[i].hex);
    }
}

bool ch_pal(char ch, uint16_t *out)
{
    unsigned idx = (unsigned char)ch;
    uint16_t  c;

    if (idx < PAL_FIRST || idx > PAL_LAST) {
        return false;
    }
    c = s_pal[idx - PAL_FIRST];
    if (!c) {
        return false;                   /* '.' and any other: transparent */
    }
    *out = c;
    return true;
}

int ch_sprite_w(const char *const *rows)
{
    return (int)strlen(rows[0]);
}

/* The ASCII art goes through the upscaler as pixels. The biggest sprite in
 * the game is the house, 48x36; the buffer is static because it is used from
 * the LVGL task, whose stack is 12 KB on the board. */
#define SPR_MAX_W   64
#define SPR_MAX_H   48
static uint16_t s_spr[SPR_MAX_W * SPR_MAX_H];

static void blit_rows(ch_buf_t *b, int x, int y, const char *const *rows,
                      int nrows, unsigned flags)
{
    int w = ch_sprite_w(rows);

    if (w > SPR_MAX_W) w = SPR_MAX_W;
    if (nrows > SPR_MAX_H) nrows = SPR_MAX_H;
    for (int ry = 0; ry < nrows; ry++) {
        const char *row = rows[ry];
        int rx = 0;
        for (; rx < w && row[rx]; rx++) {
            uint16_t c = 0;
            if (!ch_pal(row[rx], &c)) c = 0;
            s_spr[ry * w + rx] = c;
        }
        for (; rx < w; rx++) s_spr[ry * w + rx] = 0;
    }
    ch_blit_px(b, x, y, s_spr, w, nrows, 1, flags);
}

void ch_blit(ch_buf_t *b, int x, int y, const char *const *rows, int nrows)
{
    blit_rows(b, x, y, rows, nrows, CH_HD_BISEL);
}

void ch_blit_tile(ch_buf_t *b, int x, int y, const char *const *rows, int nrows)
{
    blit_rows(b, x, y, rows, nrows, CH_HD_BISEL | CH_HD_BALDOSA);
}

/* 2x: art pixel i covers physical [X0 + i*s/2, X0 + (i+1)*s/2). */
void ch_blit2_px(ch_buf_t *b, int x, int y, const uint16_t *src, int sw, int sh)
{
    int s = b->s;
    int X0 = ch_fx(b, x), Y0 = ch_fy(b, y);

    for (int sy = 0; sy < sh; sy++) {
        int ya = Y0 + sy * s / 2, yb = Y0 + (sy + 1) * s / 2;
        if (yb <= b->cy0) continue;
        if (ya >= b->cy1) break;
        if (ya < b->cy0) ya = b->cy0;
        if (yb > b->cy1) yb = b->cy1;
        for (int sx = 0; sx < sw; sx++) {
            uint16_t c = src[sy * sw + sx];
            int xa = X0 + sx * s / 2, xb = X0 + (sx + 1) * s / 2;
            if (!c || xb <= b->cx0) continue;
            if (xa >= b->cx1) break;
            if (xa < b->cx0) xa = b->cx0;
            if (xb > b->cx1) xb = b->cx1;
            for (int py = ya; py < yb; py++) {
                uint16_t *row = &b->px[(size_t)py * b->stride];
                for (int px = xa; px < xb; px++) row[px] = c;
            }
        }
    }
}

void ch_blit2(ch_buf_t *b, int x, int y, const char *const *rows, int nrows)
{
    ch_blit2m(b, x, y, rows, nrows, 1);
}

void ch_blit2m(ch_buf_t *b, int x, int y, const char *const *rows, int nrows, int m)
{
    int s = b->s * (m < 1 ? 1 : m);
    int X0 = ch_fx(b, x), Y0 = ch_fy(b, y);

    /* Straight from the characters: the biggest 2x sprite is 96x84 and does
     * not fit the scratch buffer, and there is nothing to compute between
     * two neighbours any more. */
    for (int sy = 0; sy < nrows; sy++) {
        const char *row = rows[sy];
        int ya = Y0 + sy * s / 2, yb = Y0 + (sy + 1) * s / 2;
        if (yb <= b->cy0) continue;
        if (ya >= b->cy1) break;
        if (ya < b->cy0) ya = b->cy0;
        if (yb > b->cy1) yb = b->cy1;
        for (int sx = 0; row[sx]; sx++) {
            uint16_t c;
            int xa = X0 + sx * s / 2, xb = X0 + (sx + 1) * s / 2;
            if (!ch_pal(row[sx], &c) || xb <= b->cx0) continue;
            if (xa >= b->cx1) break;
            if (xa < b->cx0) xa = b->cx0;
            if (xb > b->cx1) xb = b->cx1;
            for (int py = ya; py < yb; py++) {
                uint16_t *r = &b->px[(size_t)py * b->stride];
                for (int px = xa; px < xb; px++) r[px] = c;
            }
        }
    }
}

void ch_blit_hd(ch_buf_t *b, int x, int y, const char *const *rows1, int n1,
                const char *const *rows2, int n2)
{
    if (rows2) ch_blit2(b, x, y, rows2, n2);
    else       ch_blit(b, x, y, rows1, n1);
}

void ch_blit_c(ch_buf_t *b, int cx, int cy, const char *const *rows, int nrows)
{
    ch_blit(b, cx - ch_sprite_w(rows) / 2, cy - nrows / 2, rows, nrows);
}

/* --------------------------------------------------------------------------
 * 5x7 font
 *
 * One column per byte, bit 0 at the top. Upper case, digits and a few signs:
 * what is used on an arcade scoreboard. Each glyph goes through the same
 * upscaler as the sprites, without the bevel: at 2x the diagonals of an A or
 * a 7 come out as diagonals.
 * -------------------------------------------------------------------------- */
#define CH_FONT_FIRST   32
#define CH_FONT_LAST    95

static const uint8_t ch_font5x7[CH_FONT_LAST - CH_FONT_FIRST + 1][CH_FW] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 },   /* ' ' */
    { 0x00, 0x00, 0x5F, 0x00, 0x00 },   /* '!' */
    { 0x00, 0x07, 0x00, 0x07, 0x00 },   /* '"' */
    { 0x14, 0x7F, 0x14, 0x7F, 0x14 },   /* '#' */
    { 0x24, 0x2A, 0x7F, 0x2A, 0x12 },   /* '$' */
    { 0x23, 0x13, 0x08, 0x64, 0x62 },   /* '%' */
    { 0x36, 0x49, 0x55, 0x22, 0x50 },   /* '&' */
    { 0x00, 0x00, 0x07, 0x00, 0x00 },   /* apostrofo */
    { 0x00, 0x1C, 0x22, 0x41, 0x00 },   /* '(' */
    { 0x00, 0x41, 0x22, 0x1C, 0x00 },   /* ')' */
    { 0x14, 0x08, 0x3E, 0x08, 0x14 },   /* '*' */
    { 0x08, 0x08, 0x3E, 0x08, 0x08 },   /* '+' */
    { 0x00, 0x50, 0x30, 0x00, 0x00 },   /* ',' */
    { 0x08, 0x08, 0x08, 0x08, 0x08 },   /* '-' */
    { 0x00, 0x60, 0x60, 0x00, 0x00 },   /* '.' */
    { 0x20, 0x10, 0x08, 0x04, 0x02 },   /* '/' */
    { 0x3E, 0x51, 0x49, 0x45, 0x3E },   /* '0' */
    { 0x00, 0x42, 0x7F, 0x40, 0x00 },   /* '1' */
    { 0x42, 0x61, 0x51, 0x49, 0x46 },   /* '2' */
    { 0x21, 0x41, 0x45, 0x4B, 0x31 },   /* '3' */
    { 0x18, 0x14, 0x12, 0x7F, 0x10 },   /* '4' */
    { 0x27, 0x45, 0x45, 0x45, 0x39 },   /* '5' */
    { 0x3C, 0x4A, 0x49, 0x49, 0x30 },   /* '6' */
    { 0x01, 0x71, 0x09, 0x05, 0x03 },   /* '7' */
    { 0x36, 0x49, 0x49, 0x49, 0x36 },   /* '8' */
    { 0x06, 0x49, 0x49, 0x29, 0x1E },   /* '9' */
    { 0x00, 0x36, 0x36, 0x00, 0x00 },   /* ':' */
    { 0x00, 0x56, 0x36, 0x00, 0x00 },   /* ';' */
    { 0x08, 0x14, 0x22, 0x41, 0x00 },   /* '<' */
    { 0x14, 0x14, 0x14, 0x14, 0x14 },   /* '=' */
    { 0x00, 0x41, 0x22, 0x14, 0x08 },   /* '>' */
    { 0x02, 0x01, 0x51, 0x09, 0x06 },   /* '?' */
    { 0x32, 0x49, 0x79, 0x41, 0x3E },   /* '@' */
    { 0x7E, 0x11, 0x11, 0x11, 0x7E },   /* 'A' */
    { 0x7F, 0x49, 0x49, 0x49, 0x36 },   /* 'B' */
    { 0x3E, 0x41, 0x41, 0x41, 0x22 },   /* 'C' */
    { 0x7F, 0x41, 0x41, 0x22, 0x1C },   /* 'D' */
    { 0x7F, 0x49, 0x49, 0x49, 0x41 },   /* 'E' */
    { 0x7F, 0x09, 0x09, 0x09, 0x01 },   /* 'F' */
    { 0x3E, 0x41, 0x49, 0x49, 0x7A },   /* 'G' */
    { 0x7F, 0x08, 0x08, 0x08, 0x7F },   /* 'H' */
    { 0x00, 0x41, 0x7F, 0x41, 0x00 },   /* 'I' */
    { 0x20, 0x40, 0x41, 0x3F, 0x01 },   /* 'J' */
    { 0x7F, 0x08, 0x14, 0x22, 0x41 },   /* 'K' */
    { 0x7F, 0x40, 0x40, 0x40, 0x40 },   /* 'L' */
    { 0x7F, 0x02, 0x0C, 0x02, 0x7F },   /* 'M' */
    { 0x7F, 0x04, 0x08, 0x10, 0x7F },   /* 'N' */
    { 0x3E, 0x41, 0x41, 0x41, 0x3E },   /* 'O' */
    { 0x7F, 0x09, 0x09, 0x09, 0x06 },   /* 'P' */
    { 0x3E, 0x41, 0x51, 0x21, 0x5E },   /* 'Q' */
    { 0x7F, 0x09, 0x19, 0x29, 0x46 },   /* 'R' */
    { 0x46, 0x49, 0x49, 0x49, 0x31 },   /* 'S' */
    { 0x01, 0x01, 0x7F, 0x01, 0x01 },   /* 'T' */
    { 0x3F, 0x40, 0x40, 0x40, 0x3F },   /* 'U' */
    { 0x1F, 0x20, 0x40, 0x20, 0x1F },   /* 'V' */
    { 0x7F, 0x20, 0x18, 0x20, 0x7F },   /* 'W' */
    { 0x63, 0x14, 0x08, 0x14, 0x63 },   /* 'X' */
    { 0x03, 0x04, 0x78, 0x04, 0x03 },   /* 'Y' */
    { 0x61, 0x51, 0x49, 0x45, 0x43 },   /* 'Z' */
    { 0x00, 0x7F, 0x41, 0x41, 0x00 },   /* '[' */
    { 0x02, 0x04, 0x08, 0x10, 0x20 },   /* backslash */
    { 0x00, 0x41, 0x41, 0x7F, 0x00 },   /* ']' */
    { 0x04, 0x02, 0x01, 0x02, 0x04 },   /* '^' */
    { 0x40, 0x40, 0x40, 0x40, 0x40 },   /* '_' */
};

int ch_text_w(const char *s)
{
    int n = (int)strlen(s);
    return n > 0 ? n * CH_FADV - 1 : 0;
}

/* THE 2x FONT (P4OS, ASSETS.md): the same 64 letters redrawn by hand at
 * 10x14 (tools/fuente2x.py), drawn 1:1 wherever a font pixel is at least two
 * physical pixels - every text of the game on the P4. */
#include "ch_fuente2x.inc"

static void glyph2(ch_buf_t *b, int x, int y, uint8_t ch, uint16_t c, int m)
{
    const uint16_t *g = ch_fuente2x[ch - CH_FONT_FIRST];
    int s = b->s * m;
    int X0 = ch_fx(b, x), Y0 = ch_fy(b, y);

    for (int r = 0; r < CH_FH * 2; r++) {
        uint16_t bits = g[r];
        int ya = Y0 + r * s / 2, yb = Y0 + (r + 1) * s / 2;
        if (!bits || yb <= b->cy0 || ya >= b->cy1) continue;
        if (ya < b->cy0) ya = b->cy0;
        if (yb > b->cy1) yb = b->cy1;
        for (int k = 0; k < CH_FW * 2; k++) {
            if (!(bits & (1u << (CH_FW * 2 - 1 - k)))) continue;
            int xa = X0 + k * s / 2, xb = X0 + (k + 1) * s / 2;
            if (xb <= b->cx0 || xa >= b->cx1) continue;
            if (xa < b->cx0) xa = b->cx0;
            if (xb > b->cx1) xb = b->cx1;
            for (int yy = ya; yy < yb; yy++) {
                uint16_t *row = &b->px[(size_t)yy * b->stride];
                for (int xx = xa; xx < xb; xx++) row[xx] = c;
            }
        }
    }
}

static void glyph(ch_buf_t *b, int x, int y, uint8_t ch, uint16_t c, int m)
{
    uint16_t px[CH_FW * CH_FH];

    if (ch >= 'a' && ch <= 'z') {
        ch = (uint8_t)(ch - 32);            /* the font is upper case only */
    }
    if (ch < CH_FONT_FIRST || ch > CH_FONT_LAST || ch == ' ') {
        return;
    }
    if (b->s * m >= 2) {
        glyph2(b, x, y, ch, c, m);
        return;
    }
    const uint8_t *cols = ch_font5x7[ch - CH_FONT_FIRST];
    for (int cy = 0; cy < CH_FH; cy++) {
        for (int cx = 0; cx < CH_FW; cx++) {
            px[cy * CH_FW + cx] = (cols[cx] & (1u << cy)) ? c : 0;
        }
    }
    ch_blit_px(b, x, y, px, CH_FW, CH_FH, m, 0);
}

void ch_text(ch_buf_t *b, int x, int y, const char *s, uint16_t c)
{
    for (; *s; s++, x += CH_FADV) {
        glyph(b, x, y, (uint8_t)*s, c, 1);
    }
}

void ch_text_sh(ch_buf_t *b, int x, int y, const char *s, uint16_t c, uint16_t sh)
{
    ch_text(b, x + 1, y + 1, s, sh);
    ch_text(b, x, y, s, c);
}

void ch_text_center(ch_buf_t *b, int cx, int y, const char *s, uint16_t c, uint16_t sh)
{
    ch_text_sh(b, cx - ch_text_w(s) / 2, y, s, c, sh);
}

void ch_text_big(ch_buf_t *b, int x, int y, const char *s, uint16_t c,
                 uint16_t sh, int m)
{
    if (m < 1) m = 1;
    if (sh) {
        const char *p = s;
        for (int xx = x + 1; *p; p++, xx += CH_FADV * m) {
            glyph(b, xx, y + 1, (uint8_t)*p, sh, m);
        }
    }
    for (; *s; s++, x += CH_FADV * m) {
        glyph(b, x, y, (uint8_t)*s, c, m);
    }
}

char *ch_num(char *dst, uint32_t v, int min_digits)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < (int)sizeof(tmp));
    while (n < min_digits && n < (int)sizeof(tmp)) {
        tmp[n++] = '0';
    }
    for (int i = 0; i < n; i++) {
        dst[i] = tmp[n - 1 - i];
    }
    dst[n] = '\0';
    return dst + n;
}
