/*
 * Marcador - pixels.
 *
 * Everything the scenes show is drawn here, by the app, when a scene is
 * built: no pack, no picture from anywhere. Two kinds of buffer:
 *
 *   mr_img_t  ARGB8888 with straight alpha, the format of an lv_image: a
 *             match, a bean, a lamp, a card. Drawn once, shown by as many
 *             objects as need it.
 *   mr_cv_t   RGB565, opaque, under an lv_canvas: the felt, the table, the
 *             slate. What changes on it (chalk) is written in place and the
 *             changed rectangle invalidated.
 *
 * The textures come from value noise. Noise per pixel is too slow for a
 * full screen on the board (a few octaves at 1.6 M pixels is seconds), so
 * the slow part is computed on a coarse grid (mr_field_t, one sample every
 * few pixels) and read back with bilinear interpolation, and the fine grain
 * on top is an integer hash per pixel, which costs a handful of operations.
 *
 * The strokes are the digits as centre lines (mr_glyph): one drawing that
 * the counter's wheels print, the flip cards paint and the chalk writes.
 */
#include "mr_app.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline float fminf_(float a, float b) { return a < b ? a : b; }
static inline float fmaxf_(float a, float b) { return a > b ? a : b; }

void *mr_alloc(size_t n)
{
    /* 1 KB and up lands in PSRAM by itself (docs/MEMORY.md) */
    void *p = malloc(n < 1024 ? 1024 : n);
    if (p) memset(p, 0, n);
    return p;
}

void mr_free(void *p)
{
    free(p);
}

/* --- images ---------------------------------------------------------------------- */

bool mr_img_new(mr_img_t *im, int w, int h)
{
    memset(im, 0, sizeof *im);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    im->px = (uint32_t *)mr_alloc((size_t)w * h * 4);
    if (!im->px) return false;
    im->w = w;
    im->h = h;
    im->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    im->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    im->dsc.header.w = (uint32_t)w;
    im->dsc.header.h = (uint32_t)h;
    im->dsc.header.stride = (uint32_t)w * 4;
    im->dsc.data = (const uint8_t *)im->px;
    im->dsc.data_size = (uint32_t)w * h * 4;
    return true;
}

void mr_img_free(mr_img_t *im)
{
    if (im->px) {
        lv_image_cache_drop(&im->dsc);
        mr_free(im->px);
    }
    memset(im, 0, sizeof *im);
}

void mr_img_clear(mr_img_t *im)
{
    if (im->px) memset(im->px, 0, (size_t)im->w * im->h * 4);
    if (im->px) lv_image_cache_drop(&im->dsc);
}

void mr_img_over(mr_img_t *im, int x, int y, uint32_t rgb, float a)
{
    if ((unsigned)x >= (unsigned)im->w || (unsigned)y >= (unsigned)im->h || a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    uint32_t *p = &im->px[y * im->w + x];
    uint32_t d = *p;
    float da = (float)(d >> 24) / 255.0f;
    float oa = a + da * (1.0f - a);
    if (oa <= 0.0f) return;
    float kd = da * (1.0f - a) / oa, ks = a / oa;
    int r = (int)(((rgb >> 16) & 255) * ks + ((d >> 16) & 255) * kd + 0.5f);
    int g = (int)(((rgb >> 8) & 255) * ks + ((d >> 8) & 255) * kd + 0.5f);
    int b = (int)((rgb & 255) * ks + (d & 255) * kd + 0.5f);
    int A = (int)(oa * 255.0f + 0.5f);
    *p = ((uint32_t)A << 24) | ((uint32_t)(r > 255 ? 255 : r) << 16) | ((uint32_t)(g > 255 ? 255 : g) << 8) |
         (uint32_t)(b > 255 ? 255 : b);
}

/* a box blur of a float plane, twice each way: close enough to a gaussian */
static void blur_plane(float *s, float *t, int w, int h, int blur)
{
    for (int pass = 0; pass < 2 && blur > 0; pass++) {
        float k = 1.0f / (float)(2 * blur + 1);
        for (int y = 0; y < h; y++) {
            float acc = 0;
            for (int x = -blur; x <= blur; x++) acc += (x >= 0 && x < w) ? s[y * w + x] : 0;
            for (int x = 0; x < w; x++) {
                t[y * w + x] = acc * k;
                int xo = x - blur, xi = x + blur + 1;
                if (xo >= 0) acc -= s[y * w + xo];
                if (xi < w) acc += s[y * w + xi];
            }
        }
        for (int x = 0; x < w; x++) {
            float acc = 0;
            for (int y = -blur; y <= blur; y++) acc += (y >= 0 && y < h) ? t[y * w + x] : 0;
            for (int y = 0; y < h; y++) {
                s[y * w + x] = acc * k;
                int yo = y - blur, yi = y + blur + 1;
                if (yo >= 0) acc -= t[yo * w + x];
                if (yi < h) acc += t[yi * w + x];
            }
        }
    }
}

void mr_img_blur(mr_img_t *im, int blur)
{
    int n = im->w * im->h;
    float *s = (float *)mr_alloc((size_t)n * sizeof(float));
    float *t = (float *)mr_alloc((size_t)n * sizeof(float));
    if (s && t) {
        for (int i = 0; i < n; i++) s[i] = (float)(im->px[i] >> 24);
        blur_plane(s, t, im->w, im->h, blur);
        for (int i = 0; i < n; i++) {
            int al = (int)(s[i] + 0.5f);
            im->px[i] = ((uint32_t)(al > 255 ? 255 : al) << 24) | (im->px[i] & 0xFFFFFF);
        }
    }
    mr_free(s);
    mr_free(t);
}

void mr_img_shadow(mr_img_t *im, int dx, int dy, int blur, float a)
{
    int w = im->w, h = im->h, n = w * h;
    float *s = (float *)mr_alloc((size_t)n * sizeof(float));
    float *t = (float *)mr_alloc((size_t)n * sizeof(float));
    if (!s || !t) {
        mr_free(s);
        mr_free(t);
        return;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = x - dx, sy = y - dy;
            s[y * w + x] = (sx >= 0 && sy >= 0 && sx < w && sy < h) ? (float)(im->px[sy * w + sx] >> 24) / 255.0f : 0;
        }
    }
    blur_plane(s, t, w, h, blur);
    /* the image over its shadow */
    for (int i = 0; i < n; i++) {
        float sa = s[i] * a;
        uint32_t d = im->px[i];
        float da = (float)(d >> 24) / 255.0f;
        float oa = da + sa * (1.0f - da);
        if (oa <= 0.001f) {
            im->px[i] = 0;
            continue;
        }
        float k = da / oa;
        int r = (int)(((d >> 16) & 255) * k), g = (int)(((d >> 8) & 255) * k), b = (int)((d & 255) * k);
        im->px[i] = ((uint32_t)(oa * 255.0f + 0.5f) << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    mr_free(s);
    mr_free(t);
}

/* --- canvases -------------------------------------------------------------------- */

bool mr_cv_new(mr_cv_t *cv, int w, int h)
{
    memset(cv, 0, sizeof *cv);
    cv->px = (uint16_t *)mr_alloc((size_t)w * h * 2);
    if (!cv->px) return false;
    cv->w = w;
    cv->h = h;
    return true;
}

void mr_cv_free(mr_cv_t *cv)
{
    mr_free(cv->px);
    memset(cv, 0, sizeof *cv);
}

void mr_cv_mix(mr_cv_t *cv, int x, int y, uint32_t rgb, float a)
{
    if ((unsigned)x >= (unsigned)cv->w || (unsigned)y >= (unsigned)cv->h || a <= 0.0f) return;
    uint16_t *p = &cv->px[y * cv->w + x];
    if (a >= 1.0f) {
        *p = mr_565(rgb);
        return;
    }
    *p = mr_565(mr_rgb_mix(mr_888(*p), rgb, a));
}

void mr_cv_put_img(mr_cv_t *cv, const mr_img_t *im, int x0, int y0, float a)
{
    for (int y = 0; y < im->h; y++) {
        int cy = y0 + y;
        if ((unsigned)cy >= (unsigned)cv->h) continue;
        for (int x = 0; x < im->w; x++) {
            uint32_t s = im->px[y * im->w + x];
            if (!(s >> 24)) continue;
            mr_cv_mix(cv, x0 + x, cy, s & 0xFFFFFF, (float)(s >> 24) / 255.0f * a);
        }
    }
}

void mr_cv_copy(mr_cv_t *dst, const mr_cv_t *src)
{
    if (dst->w == src->w && dst->h == src->h) memcpy(dst->px, src->px, (size_t)src->w * src->h * 2);
}

lv_obj_t *mr_cv_obj(lv_obj_t *parent, mr_cv_t *cv)
{
    lv_obj_t *c = lv_canvas_create(parent);
    lv_canvas_set_buffer(c, cv->px, cv->w, cv->h, LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(c, 0, 0);
    return c;
}

/* --- colour ------------------------------------------------------------------------ */

uint32_t mr_rgb_mix(uint32_t a, uint32_t b, float t)
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    int r = (int)(((a >> 16) & 255) + ((float)((b >> 16) & 255) - ((a >> 16) & 255)) * t);
    int g = (int)(((a >> 8) & 255) + ((float)((b >> 8) & 255) - ((a >> 8) & 255)) * t);
    int bl = (int)((a & 255) + ((float)(b & 255) - (a & 255)) * t);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

uint32_t mr_rgb_scale(uint32_t c, float k)
{
    int r = (int)(((c >> 16) & 255) * k), g = (int)(((c >> 8) & 255) * k), b = (int)((c & 255) * k);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* --- noise ------------------------------------------------------------------------- */

uint32_t mr_hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

float mr_rnd(uint32_t *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return (float)(mr_hash(*seed) >> 8) / 16777216.0f;
}

float mr_hashf(int x, int y, uint32_t salt)
{
    return (float)(mr_hash((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ salt * 83492791u) >> 8) /
           16777216.0f;
}

float mr_vnoise(float x, float y, uint32_t salt)
{
    float fx = floorf(x), fy = floorf(y);
    int ix = (int)fx, iy = (int)fy;
    float tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    float a = mr_hashf(ix, iy, salt), b = mr_hashf(ix + 1, iy, salt);
    float c = mr_hashf(ix, iy + 1, salt), d = mr_hashf(ix + 1, iy + 1, salt);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

float mr_fbm(float x, float y, int oct, uint32_t salt)
{
    float s = 0, amp = 0.5f, norm = 0;
    for (int i = 0; i < oct; i++) {
        s += mr_vnoise(x, y, salt + (uint32_t)i * 101u) * amp;
        norm += amp;
        x = x * 2.03f + 17.1f;
        y = y * 2.03f + 5.3f;
        amp *= 0.5f;
    }
    return s / norm;
}

bool mr_field_new(mr_field_t *f, int w, int h, int step, float scale, int oct, uint32_t salt)
{
    f->step = step;
    f->gw = w / step + 2;
    f->gh = h / step + 2;
    f->v = (float *)mr_alloc((size_t)f->gw * f->gh * sizeof(float));
    if (!f->v) return false;
    for (int y = 0; y < f->gh; y++) {
        for (int x = 0; x < f->gw; x++) {
            f->v[y * f->gw + x] = mr_fbm((float)(x * step) * scale, (float)(y * step) * scale, oct, salt);
        }
    }
    return true;
}

float mr_field_at(const mr_field_t *f, int x, int y)
{
    int gx = x / f->step, gy = y / f->step;
    if (gx < 0) gx = 0;
    if (gy < 0) gy = 0;
    if (gx > f->gw - 2) gx = f->gw - 2;
    if (gy > f->gh - 2) gy = f->gh - 2;
    float tx = (float)(x - gx * f->step) / (float)f->step, ty = (float)(y - gy * f->step) / (float)f->step;
    const float *r0 = &f->v[gy * f->gw + gx], *r1 = r0 + f->gw;
    float a = r0[0] + (r0[1] - r0[0]) * tx, b = r1[0] + (r1[1] - r1[0]) * tx;
    return a + (b - a) * ty;
}

void mr_field_free(mr_field_t *f)
{
    mr_free(f->v);
    f->v = NULL;
}

/* --- strokes ----------------------------------------------------------------------- */

#define MAXS 3
typedef struct {
    int         ns;
    mr_stroke_t s[MAXS];
} glyph_t;

enum { G_DIG = 0, G_MINUS = 10, G_PLUS, G_SLASH, G_COLON, G_A, G_D, G_N };
static glyph_t G[G_N];
static bool g_ready;

static mr_stroke_t *st_new(glyph_t *g)
{
    mr_stroke_t *s = &g->s[g->ns++];
    s->n = 0;
    return s;
}

static void pt(mr_stroke_t *s, float x, float y)
{
    if (s->n < MR_STROKE_PTS) {
        s->x[s->n] = x;
        s->y[s->n] = y;
        s->n++;
    }
}

static void arc(mr_stroke_t *s, float cx, float cy, float rx, float ry, float a0, float a1, int n)
{
    for (int i = 0; i <= n; i++) {
        float a = (a0 + (a1 - a0) * (float)i / (float)n) * 3.14159265f / 180.0f;
        pt(s, cx + rx * cosf(a), cy + ry * sinf(a));
    }
}

static void glyphs_build(void)
{
    glyph_t *g;
    mr_stroke_t *s;
    memset(G, 0, sizeof G);

    g = &G[0];
    arc(st_new(g), 0.5f, 0.5f, 0.40f, 0.48f, -90, 270, 30);

    g = &G[1];
    s = st_new(g);
    pt(s, 0.20f, 0.22f);
    pt(s, 0.56f, 0.02f);
    pt(s, 0.56f, 1.0f);

    g = &G[2];
    s = st_new(g);
    arc(s, 0.5f, 0.29f, 0.38f, 0.27f, 195, 400, 18);
    pt(s, 0.08f, 1.0f);
    pt(s, 0.94f, 1.0f);

    g = &G[3];
    s = st_new(g);
    arc(s, 0.5f, 0.265f, 0.36f, 0.245f, 205, 450, 16);
    arc(s, 0.5f, 0.745f, 0.40f, 0.255f, 270, 520, 18);

    g = &G[4];
    s = st_new(g);
    pt(s, 0.72f, 1.0f);
    pt(s, 0.72f, 0.0f);
    pt(s, 0.06f, 0.68f);
    pt(s, 0.96f, 0.68f);

    g = &G[5];
    s = st_new(g);
    pt(s, 0.86f, 0.02f);
    pt(s, 0.24f, 0.02f);
    pt(s, 0.17f, 0.45f);
    arc(s, 0.5f, 0.68f, 0.40f, 0.31f, 222, 520, 22);

    g = &G[6];
    s = st_new(g);
    arc(s, 0.56f, 0.60f, 0.46f, 0.58f, 295, 172, 12);
    arc(st_new(g), 0.5f, 0.69f, 0.39f, 0.30f, 0, 360, 26);

    g = &G[7];
    s = st_new(g);
    pt(s, 0.06f, 0.02f);
    pt(s, 0.94f, 0.02f);
    pt(s, 0.38f, 1.0f);

    g = &G[8];
    arc(st_new(g), 0.5f, 0.26f, 0.33f, 0.24f, 0, 360, 24);
    arc(st_new(g), 0.5f, 0.735f, 0.39f, 0.265f, 0, 360, 26);

    /* nine: the six turned over */
    g = &G[9];
    for (int i = 0; i < G[6].ns; i++) {
        mr_stroke_t *d = st_new(g);
        for (int k = 0; k < G[6].s[i].n; k++) pt(d, 1.0f - G[6].s[i].x[k], 1.0f - G[6].s[i].y[k]);
    }

    g = &G[G_MINUS];
    s = st_new(g);
    pt(s, 0.14f, 0.56f);
    pt(s, 0.86f, 0.56f);

    g = &G[G_PLUS];
    s = st_new(g);
    pt(s, 0.12f, 0.56f);
    pt(s, 0.88f, 0.56f);
    s = st_new(g);
    pt(s, 0.5f, 0.22f);
    pt(s, 0.5f, 0.90f);

    g = &G[G_SLASH];
    s = st_new(g);
    pt(s, 0.82f, 0.0f);
    pt(s, 0.18f, 1.0f);

    g = &G[G_COLON];
    s = st_new(g);
    pt(s, 0.5f, 0.30f);
    pt(s, 0.5f, 0.30f);
    s = st_new(g);
    pt(s, 0.5f, 0.78f);
    pt(s, 0.5f, 0.78f);

    g = &G[G_A];
    s = st_new(g);
    pt(s, 0.04f, 1.0f);
    pt(s, 0.5f, 0.0f);
    pt(s, 0.96f, 1.0f);
    s = st_new(g);
    pt(s, 0.22f, 0.64f);
    pt(s, 0.78f, 0.64f);

    g = &G[G_D];
    s = st_new(g);
    pt(s, 0.12f, 1.0f);
    pt(s, 0.12f, 0.0f);
    pt(s, 0.45f, 0.0f);
    arc(s, 0.45f, 0.5f, 0.45f, 0.5f, 270, 450, 16);
    pt(s, 0.12f, 1.0f);

    g_ready = true;
}

int mr_glyph(int ch, const mr_stroke_t **out)
{
    if (!g_ready) glyphs_build();
    int k = -1;
    if (ch >= '0' && ch <= '9') k = ch - '0';
    else if (ch == '-') k = G_MINUS;
    else if (ch == '+') k = G_PLUS;
    else if (ch == '/') k = G_SLASH;
    else if (ch == ':') k = G_COLON;
    else if (ch == 'A') k = G_A;
    else if (ch == 'D') k = G_D;
    if (k < 0) return 0;
    *out = G[k].s;
    return G[k].ns;
}

float mr_seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float l2 = vx * vx + vy * vy;
    float t = l2 > 1e-6f ? (wx * vx + wy * vy) / l2 : 0.0f;
    t = mr_clampf(t, 0.0f, 1.0f);
    float dx = wx - vx * t, dy = wy - vy * t;
    return sqrtf(dx * dx + dy * dy);
}

void mr_glyph_img(mr_img_t *im, int ch, float x, float y, float w, float h, float lw, uint32_t rgb)
{
    const mr_stroke_t *s;
    int ns = mr_glyph(ch, &s);
    if (!ns) return;
    float r = lw * 0.5f;
    int x0 = (int)floorf(x - r - 1), y0 = (int)floorf(y - r - 1);
    int x1 = (int)ceilf(x + w + r + 1), y1 = (int)ceilf(y + h + r + 1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > im->w) x1 = im->w;
    if (y1 > im->h) y1 = im->h;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            float fx = (float)px + 0.5f, fy = (float)py + 0.5f, d = 1e9f;
            for (int i = 0; i < ns; i++) {
                for (int k = 0; k + 1 < s[i].n; k++) {
                    float e = mr_seg_dist(fx, fy, x + s[i].x[k] * w, y + s[i].y[k] * h, x + s[i].x[k + 1] * w,
                                          y + s[i].y[k + 1] * h);
                    if (e < d) d = e;
                }
            }
            float c = mr_clampf(r + 0.5f - d, 0.0f, 1.0f);
            if (c > 0) mr_img_over(im, px, py, rgb, c);
        }
    }
}

float mr_text_w(const char *s, float h)
{
    float w = 0;
    for (; *s; s++) w += (*s == '1' ? 0.56f : *s == ':' ? 0.34f : 0.70f) * h;
    return w;
}

void mr_text_img(mr_img_t *im, const char *s, float x, float y, float h, float lw, uint32_t rgb)
{
    for (; *s; s++) {
        float adv = (*s == '1' ? 0.56f : *s == ':' ? 0.34f : 0.70f) * h;
        float gw = 0.56f * h;
        float gx = x + (adv - gw) * 0.5f - (*s == '1' ? 0.05f * h : 0.0f);
        mr_glyph_img(im, *s, gx, y, gw, h, lw, rgb);
        x += adv;
    }
}

/* --- small LVGL helpers -------------------------------------------------------------- */

void mr_obj_plain(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *mr_img_obj(lv_obj_t *parent, mr_img_t *im)
{
    lv_obj_t *o = lv_image_create(parent);
    lv_image_set_src(o, &im->dsc);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *mr_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt ? txt : "");
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/* --- shapes into an image ---------------------------------------------------------- */

void mr_img_disc(mr_img_t *im, float cx, float cy, float r, uint32_t rgb, float a)
{
    int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
    int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float c = mr_clampf(r + 0.5f - sqrtf(dx * dx + dy * dy), 0, 1);
            if (c > 0) mr_img_over(im, x, y, rgb, c * a);
        }
    }
}

void mr_img_capsule(mr_img_t *im, float ax, float ay, float bx, float by, float r, uint32_t rgb, float a)
{
    int x0 = (int)floorf(fminf_(ax, bx) - r - 1), x1 = (int)ceilf(fmaxf_(ax, bx) + r + 1);
    int y0 = (int)floorf(fminf_(ay, by) - r - 1), y1 = (int)ceilf(fmaxf_(ay, by) + r + 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float d = mr_seg_dist((float)x + 0.5f, (float)y + 0.5f, ax, ay, bx, by);
            float c = mr_clampf(r + 0.5f - d, 0, 1);
            if (c > 0) mr_img_over(im, x, y, rgb, c * a);
        }
    }
}

float mr_rrect_sd(float px, float py, float x, float y, float w, float h, float rad)
{
    float cx = x + w * 0.5f, cy = y + h * 0.5f;
    float qx = fabsf(px - cx) - (w * 0.5f - rad), qy = fabsf(py - cy) - (h * 0.5f - rad);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - rad;
}

void mr_img_rrect(mr_img_t *im, float x, float y, float w, float h, float rad, uint32_t rgb, float a)
{
    for (int py = (int)floorf(y) - 1; py <= (int)ceilf(y + h); py++) {
        for (int px = (int)floorf(x) - 1; px <= (int)ceilf(x + w); px++) {
            float d = mr_rrect_sd((float)px + 0.5f, (float)py + 0.5f, x, y, w, h, rad);
            float c = mr_clampf(0.5f - d, 0, 1);
            if (c > 0) mr_img_over(im, px, py, rgb, c * a);
        }
    }
}

void mr_img_vgrad(mr_img_t *im, uint32_t top, uint32_t bot)
{
    for (int y = 0; y < im->h; y++) {
        uint32_t c = mr_rgb_mix(top, bot, (float)y / (float)(im->h > 1 ? im->h - 1 : 1));
        for (int x = 0; x < im->w; x++) im->px[y * im->w + x] = 0xFF000000u | c;
    }
}
