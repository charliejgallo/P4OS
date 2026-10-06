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

/* ---- the monitors in one screen ---------------------------------------------
 *
 * A Mac with two monitors sends one screen that holds both, and what no
 * monitor covers is exact black (0). So, column by column, the black that
 * reaches in from the top and from the bottom says where the monitor over
 * that column starts and ends; a monitor shorter than its neighbour, or set
 * lower, shows as a step. A dark wallpaper only ever adds black, never
 * takes it away, so: the steps are found on a running median (a few black
 * columns of wallpaper do not move it, and a median keeps an edge where it
 * is), and each monitor's top and bottom are nearly the least black over
 * its columns (an eighth in: Tight's JPEG smears a few pixels of grey
 * into the black, which is why "black" is also anything nearly so). Two monitors of the same height side by side, aligned, leave no
 * step: nothing to see there (the server list or a zoom does it). */

#define MON_R       24      /* the median's reach, kept pixels */
#define MON_TOL     2
#define MON_MIN     64      /* the narrowest monitor, kept pixels */
#define MON_SEGS    32

static int16_t median_at(const int16_t *a, int n, int c, int16_t *tmp)
{
    int lo = c - MON_R < 0 ? 0 : c - MON_R, hi = c + MON_R >= n ? n - 1 : c + MON_R, k = 0;
    for (int i = lo; i <= hi; i++) {
        int16_t v = a[i];
        int j = k++;
        while (j > 0 && tmp[j - 1] > v) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = v;
    }
    return tmp[k / 2];
}

/* r, g and b all at most 1/31 (1/63 green... 3): ~8 of 255 */
#define MON_DARK(p) (((p) & 0xF79E) == 0)

/* the k-th smallest of a[0..n) (moves a around) */
static int16_t kth(int16_t *a, int n, int k)
{
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        int16_t piv = a[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < piv) i++;
            while (a[j] > piv) j--;
            if (i <= j) {
                int16_t t = a[i];
                a[i++] = a[j];
                a[j--] = t;
            }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else break;
    }
    return a[k];
}

typedef struct {
    int a, b;               /* [a, b) along the axis */
    int lo, hi;             /* black in from either side across it */
} mon_seg_t;

static void seg_levels(mon_seg_t *s, const int16_t *lo, const int16_t *hi, int16_t *scratch)
{
    /* a few columns in from each end: the median may put the edge one off */
    int a = s->a + 4, b = s->b - 4;
    if (b <= a) a = s->a, b = s->b;
    int n = b - a;
    memcpy(scratch, lo + a, (size_t)n * sizeof *scratch);
    s->lo = kth(scratch, n, n / 8);
    memcpy(scratch, hi + a, (size_t)n * sizeof *scratch);
    s->hi = kth(scratch, n, n / 8);
}

/* lo/hi: per column (or row), the black in from either side; m: the
 * extent across (all black: m). The monitors into seg; how many. */
static int split_axis(const int16_t *lo, const int16_t *hi, int n, int m, int16_t *mlo, int16_t *mhi,
                      int16_t *tmp, mon_seg_t *seg)
{
    if (n < 2 * MON_MIN) return 0;
    for (int c = 0; c < n; c++) {
        mlo[c] = median_at(lo, n, c, tmp);
        mhi[c] = median_at(hi, n, c, tmp);
    }
    int k = 0, s0 = 0;
    for (int c = 1; c <= n; c++) {
        if (c < n && abs(mlo[c] - mlo[c - 1]) <= MON_TOL && abs(mhi[c] - mhi[c - 1]) <= MON_TOL) continue;
        if (c - s0 < MON_MIN && k) {
            seg[k - 1].b = c;                       /* a sliver: the one before's */
        } else if (c - s0 < MON_MIN && c < n) {
            continue;                               /* a sliver at the start: the next one's */
        } else {
            if (k == MON_SEGS) return 0;            /* that many steps: not monitors, a picture */
            seg[k++] = (mon_seg_t){ s0, c, 0, 0 };
        }
        s0 = c;
    }
    /* the medians are done with: mlo is the scratch from here on */
    for (int i = 0; i < k; i++) seg_levels(&seg[i], lo, hi, mlo);
    /* neighbours on the same level are one monitor */
    int j = 0;
    for (int i = 0; i < k; i++) {
        if (j && abs(seg[i].lo - seg[j - 1].lo) <= MON_TOL && abs(seg[i].hi - seg[j - 1].hi) <= MON_TOL) {
            seg[j - 1].b = seg[i].b;
            seg_levels(&seg[j - 1], lo, hi, mlo);
        } else {
            seg[j++] = seg[i];
        }
    }
    /* all black across: the gap between two monitors that do not touch */
    int out = 0;
    for (int i = 0; i < j; i++) {
        if (seg[i].lo + seg[i].hi < m - MON_MIN) seg[out++] = seg[i];
    }
    return out >= 2 ? out : 0;
}

int vnc_fb_monitors(const vnc_fb_t *fb, vnc_rect_t *out, int max)
{
    int w = fb->w, h = fb->h;
    if (!fb->px || w < 2 * MON_MIN || h < 2 * MON_MIN) return 0;
    int n = w > h ? w : h;
    int16_t *buf = vnc_psram((size_t)(6 * n + 2 * MON_R + 2) * sizeof(int16_t));
    mon_seg_t *seg = vnc_psram(MON_SEGS * sizeof *seg);
    if (!buf || !seg) {
        free(buf);
        free(seg);
        return 0;
    }
    int16_t *ctop = buf, *cbot = buf + n, *rl = buf + 2 * n, *rr = buf + 3 * n;
    int16_t *mlo = buf + 4 * n, *mhi = buf + 5 * n, *tmp = buf + 6 * n;
    /* one pass in memory order (PSRAM): first and last lit pixel of every
     * column and every row */
    for (int x = 0; x < w; x++) {
        ctop[x] = -1;
        cbot[x] = -1;
    }
    for (int y = 0; y < h; y++) {
        const uint16_t *p = fb->px + (size_t)y * w;
        int first = -1, last = -1;
        for (int x = 0; x < w; x++) {
            if (MON_DARK(p[x])) continue;
            if (first < 0) first = x;
            last = x;
            if (ctop[x] < 0) ctop[x] = (int16_t)y;
            cbot[x] = (int16_t)y;
        }
        rl[y] = (int16_t)(first < 0 ? w : first);
        rr[y] = (int16_t)(first < 0 ? w : w - 1 - last);
    }
    for (int x = 0; x < w; x++) {
        if (ctop[x] < 0) {
            ctop[x] = (int16_t)h;
            cbot[x] = (int16_t)h;
        } else {
            cbot[x] = (int16_t)(h - 1 - cbot[x]);
        }
    }
    int sh = fb->shift, k = split_axis(ctop, cbot, w, h, mlo, mhi, tmp, seg);
    bool cols = k > 0;
    if (!cols) k = split_axis(rl, rr, h, w, mlo, mhi, tmp, seg);     /* one above the other */
    if (k > max) k = max;
    for (int i = 0; i < k; i++) {
        const mon_seg_t *s = &seg[i];
        int a0 = s->a, a1 = s->b, b0 = s->lo, b1 = (cols ? h : w) - s->hi;
        int x0 = cols ? a0 : b0, x1 = cols ? a1 : b1, y0 = cols ? b0 : a0, y1 = cols ? b1 : a1;
        x0 <<= sh, x1 <<= sh, y0 <<= sh, y1 <<= sh;
        if (x1 > fb->rw) x1 = fb->rw;
        if (y1 > fb->rh) y1 = fb->rh;
        out[i] = (vnc_rect_t){ fb->zx + x0, fb->zy + y0, x1 - x0, y1 - y0 };
    }
    free(buf);
    free(seg);
    return k;
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
