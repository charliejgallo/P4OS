/*
 * P4OS - VNC viewer: the remote screen, and the view of it.
 *
 * The remote screen is one RGB565 picture in PSRAM, written by the decoders
 * (vnc_rfb.c) as rectangles arrive. Its budget is FB_BUDGET: a Retina Mac
 * sends 2880 x 1800 (10 MB here) and a 5K iMac 5120 x 2880 (29 MB, more than
 * the board has), so a screen over budget is kept at a half or a quarter.
 * The decoders do not know: every write below drops the pixels in between,
 * and what changed is noted in kept pixels for the view.
 *
 * The view is what the canvas shows: the screen's size, at any scale, at
 * any place of the remote screen. Scaling down averages a few samples per
 * pixel (up to 3 x 3), so text shrunk to fit stays readable instead of
 * breaking up; scaling up takes the nearest pixel, sharp as a remote screen
 * should look. The per-column samples are worked out once per view.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")      /* the .so is built with -Os: the pixel loops want O2 */
#endif
#include "vnc.h"

#include "aos_hal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

#define FB_BUDGET   (9u * 1024 * 1024)
#define MAX_K       3
#define BG          0x0000

void *vnc_psram(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n ? n : 1);
#else
    return malloc(n ? n : 1);
#endif
}

/* ---- the remote screen ---------------------------------------------------- */

bool vnc_fb_alloc(vnc_fb_t *fb, int rw, int rh)
{
    vnc_fb_free(fb);
    for (int shift = 0; shift <= 2; shift++) {
        int w = (rw + (1 << shift) - 1) >> shift, h = (rh + (1 << shift) - 1) >> shift;
        size_t bytes = (size_t)w * h * 2;
        if (bytes > FB_BUDGET && shift < 2) {
            continue;
        }
        fb->px = vnc_psram(bytes);
        if (!fb->px) {
            continue;
        }
        memset(fb->px, 0, bytes);
        fb->zx = 0;
        fb->zy = 0;
        fb->rw = rw;
        fb->rh = rh;
        fb->w = w;
        fb->h = h;
        fb->shift = shift;
        fb->dx0 = 0;
        fb->dy0 = 0;
        fb->dx1 = w;
        fb->dy1 = h;
        return true;
    }
    return false;
}

void vnc_fb_free(vnc_fb_t *fb)
{
    free(fb->px);
    memset(fb, 0, sizeof *fb);
}

/* The kept columns or rows [*a, *b) that the server's [x, x + n) covers. */
static bool span(int x, int n, int shift, int limit, int *a, int *b)
{
    int m = (1 << shift) - 1;
    int lo = (x + m) >> shift, hi = (x + n + m) >> shift;
    if (lo < 0) lo = 0;
    if (hi > limit) hi = limit;
    *a = lo;
    *b = hi;
    return lo < hi;
}

static void mark(vnc_fb_t *fb, int x0, int y0, int x1, int y1)
{
    if (fb->dx0 >= fb->dx1) {
        fb->dx0 = x0;
        fb->dy0 = y0;
        fb->dx1 = x1;
        fb->dy1 = y1;
        return;
    }
    if (x0 < fb->dx0) fb->dx0 = x0;
    if (y0 < fb->dy0) fb->dy0 = y0;
    if (x1 > fb->dx1) fb->dx1 = x1;
    if (y1 > fb->dy1) fb->dy1 = y1;
}

void vnc_fb_fill(vnc_fb_t *fb, int x, int y, int w, int h, uint16_t c)
{
    int x0, x1, y0, y1;
    x -= fb->zx;
    y -= fb->zy;
    if (!fb->px || !span(x, w, fb->shift, fb->w, &x0, &x1) || !span(y, h, fb->shift, fb->h, &y0, &y1)) {
        return;
    }
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *d = fb->px + (size_t)yy * fb->w;
        for (int xx = x0; xx < x1; xx++) d[xx] = c;
    }
    mark(fb, x0, y0, x1, y1);
}

void vnc_fb_row(vnc_fb_t *fb, int x, int y, int w, const uint16_t *src)
{
    int x0, x1, sh = fb->shift;
    x -= fb->zx;
    y -= fb->zy;
    if (!fb->px || y < 0 || (y >> sh) >= fb->h || (y & ((1 << sh) - 1)) ||
        !span(x, w, sh, fb->w, &x0, &x1)) {
        return;
    }
    int yy = y >> sh;
    uint16_t *d = fb->px + (size_t)yy * fb->w;
    if (!sh) {
        memcpy(d + x0, src + (x0 - x), (size_t)(x1 - x0) * 2);
    } else {
        for (int xx = x0; xx < x1; xx++) d[xx] = src[(xx << sh) - x];
    }
    mark(fb, x0, yy, x1, yy + 1);
}

void vnc_fb_rect(vnc_fb_t *fb, int x, int y, int w, int h, const uint16_t *src, int stride_px)
{
    for (int r = 0; r < h; r++) {
        if (!vnc_fb_skip_row(fb, y + r)) vnc_fb_row(fb, x, y + r, w, src + (size_t)r * stride_px);
    }
}

bool vnc_fb_copy(vnc_fb_t *fb, int sx, int sy, int dx, int dy, int w, int h)
{
    if (!fb->px) {
        return true;
    }
    sx -= fb->zx;
    sy -= fb->zy;
    dx -= fb->zx;
    dy -= fb->zy;
    /* what was copied from outside the zone was never kept */
    if (sx < 0 || sy < 0 || sx + w > fb->rw || sy + h > fb->rh) {
        return dx + w <= 0 || dy + h <= 0 || dx >= fb->rw || dy >= fb->rh;
    }
    if (dx < 0 || dy < 0) {
        /* the part that stays in the zone; the rest is not kept */
        int cx = dx < 0 ? -dx : 0, cy = dy < 0 ? -dy : 0;
        sx += cx; dx += cx; w -= cx;
        sy += cy; dy += cy; h -= cy;
        if (w <= 0 || h <= 0) return true;
    }
    int sh = fb->shift;
    /* in kept pixels; at a half or a quarter an odd offset rounds, which
     * the next update of that area puts right */
    int ksx = sx >> sh, ksy = sy >> sh, kdx = dx >> sh, kdy = dy >> sh;
    int kw = (w + (1 << sh) - 1) >> sh, kh = (h + (1 << sh) - 1) >> sh;
    if (ksx < 0 || ksy < 0 || kdx < 0 || kdy < 0) return true;
    if (ksx + kw > fb->w) kw = fb->w - ksx;
    if (kdx + kw > fb->w) kw = fb->w - kdx;
    if (ksy + kh > fb->h) kh = fb->h - ksy;
    if (kdy + kh > fb->h) kh = fb->h - kdy;
    if (kw <= 0 || kh <= 0) return true;
    if (kdy <= ksy) {
        for (int r = 0; r < kh; r++) {
            memmove(fb->px + (size_t)(kdy + r) * fb->w + kdx, fb->px + (size_t)(ksy + r) * fb->w + ksx,
                    (size_t)kw * 2);
        }
    } else {
        for (int r = kh - 1; r >= 0; r--) {
            memmove(fb->px + (size_t)(kdy + r) * fb->w + kdx, fb->px + (size_t)(ksy + r) * fb->w + ksx,
                    (size_t)kw * 2);
        }
    }
    mark(fb, kdx, kdy, kdx + kw, kdy + kh);
    return true;
}

/* ---- the view ------------------------------------------------------------- */

struct vnc_render {
    vnc_view_t v;               /* what the tables are for */
    int        shift, fw, fh;
    int        k;               /* samples per axis */
    int32_t   *xs;              /* k per view column: kept x, or -1 outside */
    int        cap;             /* columns xs has room for */
};

vnc_render_t *vnc_render_new(void)
{
    return calloc(1, sizeof(vnc_render_t));
}

void vnc_render_free(vnc_render_t *r)
{
    if (r) {
        free(r->xs);
        free(r);
    }
}

/* The kept coordinate of sample j (of k) of view pixel i along one axis. */
static int sample(float o, float scale, int i, int j, int k, int shift, int limit)
{
    float rx = o + ((float)i + ((float)j + 0.5f) / (float)k) / scale;
    if (rx < 0.0f) return -1;
    int x = (int)rx >> shift;
    return x < limit ? x : -1;
}

static bool prepare(vnc_render_t *r, const vnc_fb_t *fb, const vnc_view_t *v)
{
    if (r->xs && r->v.gen == v->gen && r->v.vw == v->vw && r->shift == fb->shift && r->fw == fb->w &&
        r->fh == fb->h && r->v.scale == v->scale && r->v.ox == v->ox && r->v.oy == v->oy) {
        return true;
    }
    float foot = 1.0f / (v->scale * (float)(1 << fb->shift));     /* kept pixels per view pixel */
    int k = foot <= 1.05f ? 1 : (int)ceilf(foot - 0.05f);
    if (k > MAX_K) k = MAX_K;
    int need = v->vw * k;
    if (need > r->cap) {
        free(r->xs);
        r->xs = malloc((size_t)need * sizeof(int32_t));
        r->cap = r->xs ? need : 0;
        if (!r->xs) return false;
    }
    for (int i = 0; i < v->vw; i++) {
        for (int j = 0; j < k; j++) r->xs[i * k + j] = sample(v->ox, v->scale, i, j, k, fb->shift, fb->w);
    }
    r->v = *v;
    r->k = k;
    r->shift = fb->shift;
    r->fw = fb->w;
    r->fh = fb->h;
    return true;
}

void vnc_render(vnc_render_t *r, const vnc_fb_t *fb, const vnc_view_t *v,
                uint16_t *dst, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > v->vw) x1 = v->vw;
    if (y1 > v->vh) y1 = v->vh;
    if (x0 >= x1 || y0 >= y1) return;
    if (!fb->px || !prepare(r, fb, v)) {
        for (int y = y0; y < y1; y++) {
            uint16_t *d = dst + (size_t)y * v->vw;
            for (int x = x0; x < x1; x++) d[x] = BG;
        }
        return;
    }
    const int k = r->k;
    for (int y = y0; y < y1; y++) {
        uint16_t *d = dst + (size_t)y * v->vw;
        if (k == 1) {
            int sy = sample(v->oy, v->scale, y, 0, 1, fb->shift, fb->h);
            if (sy < 0) {
                for (int x = x0; x < x1; x++) d[x] = BG;
                continue;
            }
            const uint16_t *s = fb->px + (size_t)sy * fb->w;
            const int32_t *xs = r->xs;
            for (int x = x0; x < x1; x++) {
                int sx = xs[x];
                d[x] = sx >= 0 ? s[sx] : BG;
            }
            continue;
        }
        const uint16_t *rows[MAX_K];
        int nrows = 0;
        for (int j = 0; j < k; j++) {
            int sy = sample(v->oy, v->scale, y, j, k, fb->shift, fb->h);
            if (sy >= 0) rows[nrows++] = fb->px + (size_t)sy * fb->w;
        }
        if (!nrows) {
            for (int x = x0; x < x1; x++) d[x] = BG;
            continue;
        }
        for (int x = x0; x < x1; x++) {
            const int32_t *xs = r->xs + x * k;
            uint32_t rs = 0, gs = 0, bs = 0, n = 0;
            for (int a = 0; a < nrows; a++) {
                const uint16_t *s = rows[a];
                for (int b = 0; b < k; b++) {
                    if (xs[b] < 0) continue;
                    uint16_t p = s[xs[b]];
                    rs += p >> 11;
                    gs += (p >> 5) & 63;
                    bs += p & 31;
                    n++;
                }
            }
            if (!n) {
                d[x] = BG;
                continue;
            }
            /* a whole footprint is 4 or 9 samples: no division in the common case */
            if (n == 4) {
                rs >>= 2; gs >>= 2; bs >>= 2;
            } else if (n == 9) {
                rs = rs * 7282 >> 16; gs = gs * 7282 >> 16; bs = bs * 7282 >> 16;
            } else {
                rs /= n; gs /= n; bs /= n;
            }
            d[x] = (uint16_t)(rs << 11 | gs << 5 | bs);
        }
    }
}

bool vnc_view_rect(const vnc_fb_t *fb, const vnc_view_t *v, int x0, int y0, int x1, int y1,
                   int *vx0, int *vy0, int *vx1, int *vy1)
{
    float d = (float)(1 << fb->shift);
    int a = (int)floorf(((float)x0 * d - v->ox) * v->scale) - 1;
    int b = (int)floorf(((float)y0 * d - v->oy) * v->scale) - 1;
    int c = (int)ceilf(((float)x1 * d - v->ox) * v->scale) + 1;
    int e = (int)ceilf(((float)y1 * d - v->oy) * v->scale) + 1;
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (c > v->vw) c = v->vw;
    if (e > v->vh) e = v->vh;
    if (a >= c || b >= e) return false;
    *vx0 = a;
    *vy0 = b;
    *vx1 = c;
    *vy1 = e;
    return true;
}
