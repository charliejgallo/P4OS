/*
 * CIELO - drawing into an RGB565 buffer (see cl_draw.h).
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "cl_draw.h"
#include "cl_mem.h"

#include <math.h>
#include <string.h>

/* fminf and fmaxf are not in the firmware's symbol table */
static inline float mn(float a, float b) { return a < b ? a : b; }
static inline float mx(float a, float b) { return a > b ? a : b; }

static inline uint16_t blend(uint16_t v, uint32_t rgb, int al)
{
    int r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
    r += (((int)(rgb >> 16) - r) * al) >> 8;
    g += ((((int)(rgb >> 8) & 255) - g) * al) >> 8;
    b += (((int)rgb & 255) - b) * al >> 8;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint16_t add(uint16_t v, uint32_t rgb, int amt)
{
    int r = ((v >> 11) << 3) + ((int)(rgb >> 16) * amt >> 8);
    int g = (((v >> 5) & 63) << 2) + ((int)((rgb >> 8) & 255) * amt >> 8);
    int b = ((v & 31) << 3) + ((int)(rgb & 255) * amt >> 8);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline void fill16(uint16_t *d, int n, uint16_t c)
{
    if (n <= 0) return;
    if ((uintptr_t)d & 2) {
        *d++ = c;
        n--;
    }
    uint32_t c2 = (uint32_t)c | (uint32_t)c << 16;
    uint32_t *w = (uint32_t *)d;
    int n2 = n >> 1, i = 0;
    for (; i + 4 <= n2; i += 4) {
        w[i] = c2;
        w[i + 1] = c2;
        w[i + 2] = c2;
        w[i + 3] = c2;
    }
    for (; i < n2; i++) w[i] = c2;
    if (n & 1) d[n - 1] = c;
}

void cl_fb_plain(cl_fb_t *fb, uint16_t *px, int w, int h)
{
    memset(fb, 0, sizeof *fb);
    fb->px = px;
    fb->w = w;
    fb->h = h;
    fb->sx = 1;
    fb->sy = w;
    cl_fb_clip(fb, 0, 0, w, h);
}

void cl_fb_panel(cl_fb_t *fb, uint16_t *px, int pw, int ph, int rot)
{
    memset(fb, 0, sizeof *fb);
    fb->px = px;
    switch (rot) {
    case 90:
        fb->w = ph, fb->h = pw, fb->off = pw - 1, fb->sx = pw, fb->sy = -1;
        break;
    case 270:
        fb->w = ph, fb->h = pw, fb->off = (ph - 1) * pw, fb->sx = -pw, fb->sy = 1;
        break;
    case 180:
        fb->w = pw, fb->h = ph, fb->off = pw * ph - 1, fb->sx = -1, fb->sy = -pw;
        break;
    default:
        fb->w = pw, fb->h = ph, fb->off = 0, fb->sx = 1, fb->sy = pw;
        break;
    }
    cl_fb_clip(fb, 0, 0, fb->w, fb->h);
}

void cl_fb_clip(cl_fb_t *fb, int x0, int y0, int x1, int y1)
{
    fb->cx0 = x0 < 0 ? 0 : x0;
    fb->cy0 = y0 < 0 ? 0 : y0;
    fb->cx1 = x1 > fb->w ? fb->w : x1;
    fb->cy1 = y1 > fb->h ? fb->h : y1;
}

uint32_t cl_tint(const cl_fb_t *fb, uint32_t rgb)
{
    if (!fb->night) return rgb;
    int r = (int)(rgb >> 16), g = (int)(rgb >> 8) & 255, b = (int)rgb & 255;
    int l = (r * 77 + g * 150 + b * 29) >> 8;
    int rr = l * 5 / 4;
    if (rr > 255) rr = 255;
    return (uint32_t)rr << 16 | (uint32_t)(l / 10) << 8;
}

uint32_t cl_mix(uint32_t a, uint32_t b, float t)
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    uint32_t o = 0;
    for (int s = 0; s <= 16; s += 8) {
        int x = (int)(a >> s) & 255, y = (int)(b >> s) & 255;
        o |= (uint32_t)(x + (int)((float)(y - x) * t)) << s;
    }
    return o;
}

void cl_fill_rect(cl_fb_t *fb, int x, int y, int w, int h, uint32_t rgb)
{
    int x0 = x < fb->cx0 ? fb->cx0 : x, x1 = x + w > fb->cx1 ? fb->cx1 : x + w;
    int y0 = y < fb->cy0 ? fb->cy0 : y, y1 = y + h > fb->cy1 ? fb->cy1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    uint16_t c = cl_565(cl_tint(fb, rgb));
    if (fb->sx == 1) {
        for (int yy = y0; yy < y1; yy++) fill16(cl_px(fb, x0, yy), x1 - x0, c);
    } else if (fb->sy == 1) {
        for (int xx = x0; xx < x1; xx++) fill16(cl_px(fb, xx, y0), y1 - y0, c);
    } else if (fb->sy == -1) {
        for (int xx = x0; xx < x1; xx++) fill16(cl_px(fb, xx, y1 - 1), y1 - y0, c);
    } else if (fb->sx == -1) {
        for (int yy = y0; yy < y1; yy++) fill16(cl_px(fb, x1 - 1, yy), x1 - x0, c);
    }
}

void cl_span(cl_fb_t *fb, int y, float xa, float xb, uint32_t rgb, int alpha)
{
    if (y < fb->cy0 || y >= fb->cy1 || xb <= xa) return;
    if (xa < (float)fb->cx0) xa = (float)fb->cx0;
    if (xb > (float)fb->cx1) xb = (float)fb->cx1;
    if (xb <= xa) return;
    rgb = cl_tint(fb, rgb);
    uint16_t c = cl_565(rgb);
    int ia = (int)xa, ib = (int)xb;
    if (ia == ib) {
        int al = (int)((xb - xa) * (float)alpha);
        if (al > 0 && ia < fb->cx1) *cl_px(fb, ia, y) = blend(*cl_px(fb, ia, y), rgb, al);
        return;
    }
    int al = (int)(((float)(ia + 1) - xa) * (float)alpha);
    uint16_t *p = cl_px(fb, ia, y);
    if (al > 0) *p = al >= 250 ? c : blend(*p, rgb, al);
    if (alpha >= 255 && fb->sx == 1) {
        fill16(p + 1, ib - ia - 1, c);
    } else {
        uint16_t *q = p;
        for (int x = ia + 1; x < ib; x++) {
            q += fb->sx;
            *q = alpha >= 255 ? c : blend(*q, rgb, alpha);
        }
    }
    if (ib < fb->cx1) {
        al = (int)((xb - (float)ib) * (float)alpha);
        uint16_t *e = cl_px(fb, ib, y);
        if (al > 0) *e = blend(*e, rgb, al);
    }
}

void cl_blend_px(cl_fb_t *fb, int x, int y, uint32_t rgb, int alpha)
{
    if (x < fb->cx0 || x >= fb->cx1 || y < fb->cy0 || y >= fb->cy1 || alpha <= 0) return;
    uint16_t *p = cl_px(fb, x, y);
    rgb = cl_tint(fb, rgb);
    *p = alpha >= 255 ? cl_565(rgb) : blend(*p, rgb, alpha);
}

void cl_add_px(cl_fb_t *fb, int x, int y, uint32_t rgb, int amount)
{
    if (x < fb->cx0 || x >= fb->cx1 || y < fb->cy0 || y >= fb->cy1 || amount <= 0) return;
    uint16_t *p = cl_px(fb, x, y);
    *p = add(*p, cl_tint(fb, rgb), amount);
}

void cl_disc(cl_fb_t *fb, float cx, float cy, float r, uint32_t rgb, int alpha)
{
    if (alpha <= 0) return;
    if (r < 0.7f) {
        /* a point: its brightness spread over the four pixels it touches */
        alpha = (int)((float)alpha * r / 0.7f);
        float fx = cx - 0.5f, fy = cy - 0.5f;
        int ix = (int)floorf(fx), iy = (int)floorf(fy);
        float ax = fx - (float)ix, ay = fy - (float)iy;
        rgb = cl_tint(fb, rgb);
        const float w[4] = { (1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay };
        for (int k = 0; k < 4; k++) {
            int x = ix + (k & 1), y = iy + (k >> 1);
            int al = (int)(w[k] * (float)alpha * 1.6f);
            if (al > 255) al = 255;
            if (al < 6 || x < fb->cx0 || x >= fb->cx1 || y < fb->cy0 || y >= fb->cy1) continue;
            uint16_t *p = cl_px(fb, x, y);
            *p = blend(*p, rgb, al);
        }
        return;
    }
    rgb = cl_tint(fb, rgb);
    uint16_t c = cl_565(rgb);
    const float R = r + 0.5f, R2 = R * R, ri = r - 0.5f, ri2 = ri > 0 ? ri * ri : -1;
    int y0 = (int)floorf(cy - R), y1 = (int)ceilf(cy + R);
    int x0 = (int)floorf(cx - R), x1 = (int)ceilf(cx + R);
    if (y0 < fb->cy0) y0 = fb->cy0;
    if (y1 > fb->cy1) y1 = fb->cy1;
    if (x0 < fb->cx0) x0 = fb->cx0;
    if (x1 > fb->cx1) x1 = fb->cx1;
    for (int y = y0; y < y1; y++) {
        float dy = (float)y + 0.5f - cy;
        uint16_t *p = cl_px(fb, x0, y);
        for (int x = x0; x < x1; x++, p += fb->sx) {
            float dx = (float)x + 0.5f - cx, d2 = dx * dx + dy * dy;
            if (d2 >= R2) continue;
            if (d2 <= ri2) {
                *p = alpha >= 255 ? c : blend(*p, rgb, alpha);
            } else {
                int al = (int)((R - sqrtf(d2)) * (float)alpha);
                if (al > 0) *p = blend(*p, rgb, al > 255 ? 255 : al);
            }
        }
    }
}

void cl_glow(cl_fb_t *fb, float cx, float cy, float r, uint32_t rgb, int amount)
{
    if (r < 1 || amount <= 0) return;
    rgb = cl_tint(fb, rgb);
    int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    int x0 = (int)floorf(cx - r), x1 = (int)ceilf(cx + r);
    if (y0 < fb->cy0) y0 = fb->cy0;
    if (y1 > fb->cy1) y1 = fb->cy1;
    if (x0 < fb->cx0) x0 = fb->cx0;
    if (x1 > fb->cx1) x1 = fb->cx1;
    float inv = 1.0f / (r * r);
    for (int y = y0; y < y1; y++) {
        float dy = (float)y + 0.5f - cy;
        uint16_t *p = cl_px(fb, x0, y);
        for (int x = x0; x < x1; x++, p += fb->sx) {
            float dx = (float)x + 0.5f - cx, q = 1.0f - (dx * dx + dy * dy) * inv;
            if (q <= 0) continue;
            int a = (int)(q * q * (float)amount);
            if (a > 0) *p = add(*p, rgb, a);
        }
    }
}

void cl_ring(cl_fb_t *fb, float cx, float cy, float r, float w, uint32_t rgb, int alpha)
{
    rgb = cl_tint(fb, rgb);
    float ro = r + w * 0.5f, rin = r - w * 0.5f;
    int y0 = (int)floorf(cy - ro - 1), y1 = (int)ceilf(cy + ro + 1);
    int x0 = (int)floorf(cx - ro - 1), x1 = (int)ceilf(cx + ro + 1);
    if (y0 < fb->cy0) y0 = fb->cy0;
    if (y1 > fb->cy1) y1 = fb->cy1;
    if (x0 < fb->cx0) x0 = fb->cx0;
    if (x1 > fb->cx1) x1 = fb->cx1;
    for (int y = y0; y < y1; y++) {
        float dy = (float)y + 0.5f - cy;
        uint16_t *p = cl_px(fb, x0, y);
        for (int x = x0; x < x1; x++, p += fb->sx) {
            float dx = (float)x + 0.5f - cx, d2 = dx * dx + dy * dy;
            if (d2 > (ro + 0.5f) * (ro + 0.5f) || (rin > 0.5f && d2 < (rin - 0.5f) * (rin - 0.5f))) continue;
            float d = sqrtf(d2);
            float cov = mn(ro + 0.5f - d, d - rin + 0.5f);
            if (cov > 1) cov = 1;
            if (cov <= 0) continue;
            *p = blend(*p, rgb, (int)(cov * (float)alpha));
        }
    }
}

void cl_line(cl_fb_t *fb, float x0, float y0, float x1, float y1, float w, uint32_t rgb, int alpha)
{
    if (alpha <= 0) return;
    if (w < 1.0f) {
        alpha = (int)((float)alpha * w);
        w = 1.0f;
    }
    /* quick reject against the clip */
    float r = w * 0.5f + 1;
    if ((x0 < fb->cx0 - r && x1 < fb->cx0 - r) || (x0 > fb->cx1 + r && x1 > fb->cx1 + r) ||
        (y0 < fb->cy0 - r && y1 < fb->cy0 - r) || (y0 > fb->cy1 + r && y1 > fb->cy1 + r))
        return;
    rgb = cl_tint(fb, rgb);
    uint16_t c = cl_565(rgb);
    /* Wu's way: one step per pixel along the long axis, and across it the
     * pixels the band covers, each by its share (vl_draw.c's seg_thin) */
    float dx = x1 - x0, dy = y1 - y0;
    bool xmajor = fabsf(dx) >= fabsf(dy);
    if (!xmajor) {
        float t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
        t = dx; dx = dy; dy = t;
    }
    if (x1 < x0) {
        float t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        dx = -dx;
        dy = -dy;
    }
    if (dx < 1e-3f) return;
    const float slope = dy / dx, hv = w * 0.5f * sqrtf(1.0f + slope * slope);
    int m0 = xmajor ? fb->cx0 : fb->cy0, m1 = xmajor ? fb->cx1 : fb->cy1;
    int n0 = xmajor ? fb->cy0 : fb->cx0, n1 = xmajor ? fb->cy1 : fb->cx1;
    int a = (int)ceilf(x0 - 0.5f), b = (int)ceilf(x1 - 0.5f);
    if (a < m0) a = m0;
    if (b > m1) b = m1;
    for (int i = a; i < b; i++) {
        float cc = y0 + ((float)i + 0.5f - x0) * slope;
        float lo = cc - hv, hi = cc + hv;
        int j0 = (int)floorf(lo), j1 = (int)floorf(hi);
        if (j0 < n0) j0 = n0;
        if (j1 >= n1) j1 = n1 - 1;
        for (int j = j0; j <= j1; j++) {
            float top = (float)j > lo ? (float)j : lo, bot = (float)(j + 1) < hi ? (float)(j + 1) : hi;
            int al = (int)((bot - top) * (float)alpha);
            if (al <= 4) continue;
            uint16_t *p = xmajor ? cl_px(fb, i, j) : cl_px(fb, j, i);
            *p = al >= 250 ? c : blend(*p, rgb, al);
        }
    }
}

void cl_rbox(cl_fb_t *fb, int x, int y, int w, int h, int r, uint32_t rgb, int alpha)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int yy = y; yy < y + h; yy++) {
        float inset = 0;
        float d = -1;
        if (yy < y + r) d = (float)(y + r) - ((float)yy + 0.5f);
        else if (yy >= y + h - r) d = ((float)yy + 0.5f) - (float)(y + h - r);
        if (d > 0) inset = (float)r - sqrtf(mx(0.0f, (float)(r * r) - d * d));
        cl_span(fb, yy, (float)x + inset, (float)(x + w) - inset, rgb, alpha);
    }
}

void cl_phase_disc(cl_fb_t *fb, float cx, float cy, float r, float k, float ang, uint32_t lit, uint32_t dark)
{
    lit = cl_tint(fb, lit);
    dark = cl_tint(fb, dark);
    /* in the disc's own frame, u towards the Sun: the lit part is u > 0 on
     * the limb's half, and the terminator an ellipse of half-width
     * |2k - 1| r, on the Sun's side when more than half is lit */
    float cs = cosf(ang), sn = sinf(ang), e = 2 * k - 1;
    const float R = r + 0.5f;
    int y0 = (int)floorf(cy - R), y1 = (int)ceilf(cy + R);
    int x0 = (int)floorf(cx - R), x1 = (int)ceilf(cx + R);
    if (y0 < fb->cy0) y0 = fb->cy0;
    if (y1 > fb->cy1) y1 = fb->cy1;
    if (x0 < fb->cx0) x0 = fb->cx0;
    if (x1 > fb->cx1) x1 = fb->cx1;
    for (int y = y0; y < y1; y++) {
        float dy = (float)y + 0.5f - cy;
        uint16_t *p = cl_px(fb, x0, y);
        for (int x = x0; x < x1; x++, p += fb->sx) {
            float dx = (float)x + 0.5f - cx, d2 = dx * dx + dy * dy;
            if (d2 >= R * R) continue;
            float edge = R - sqrtf(d2);
            if (edge > 1) edge = 1;
            float u = dx * cs + dy * sn, v = -dx * sn + dy * cs;
            float hw = sqrtf(mx(0.0f, r * r - v * v));     /* the limb at this v */
            float t = e * hw;                                  /* the terminator's u */
            /* lit where u > -t, softened over a pixel */
            float l = u + t + 0.5f;
            if (l < 0) l = 0;
            if (l > 1) l = 1;
            uint32_t col = cl_mix(dark, lit, l);
            *p = blend(*p, col, (int)(edge * 255));
        }
    }
}

/* ---------------------------------------------------------------------------
 * Text
 * ------------------------------------------------------------------------- */

static int utf8_put(char *o, uint32_t cp)
{
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

uint32_t cl_utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = *p;
    if (!c) return 0;
    if (c < 0x80) {
        *s += 1;
        return c;
    }
    int n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    uint32_t cp = n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s += i;
            return '?';
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += n;
    return n == 1 ? '?' : cp;
}

static const cl_glyph_t *glyph(const cl_font_t *f, uint32_t cp)
{
    if (cp >= CL_GLYPH_FIRST && cp <= CL_GLYPH_LAST) {
        const cl_glyph_t *g = &f->g[cp - CL_GLYPH_FIRST];
        if (g->a || g->adv) return g;
    } else {
        for (int i = 0; i < f->nx; i++)
            if (f->xcp[i] == cp) return &f->x[i];
        if (cp == 0x2014 || cp == 0x2013) return &f->g['-' - CL_GLYPH_FIRST];
    }
    return &f->g['?' - CL_GLYPH_FIRST];
}

/* one glyph drawn into the canvas, its masks copied out */
typedef struct {
    uint32_t cp;
    const lv_font_t *font;
    int adv;
} todo_t;

bool cl_font_bake(cl_font_t *f, const lv_font_t *font, const lv_font_t *sym, const uint32_t *extra, int nextra)
{
    memset(f, 0, sizeof *f);
    if (nextra > CL_EXTRA) nextra = CL_EXTRA;
    if (!sym) nextra = 0;
    int total = CL_GLYPHS + nextra;
    todo_t *td = (todo_t *)cl_malloc(sizeof(todo_t) * (size_t)total);
    if (!td) return false;
    int lh = lv_font_get_line_height(font), lh2 = sym ? lv_font_get_line_height(sym) : 0;
    int maxadv = 1;
    for (int i = 0; i < total; i++) {
        uint32_t cp = i < CL_GLYPHS ? (uint32_t)(CL_GLYPH_FIRST + i) : extra[i - CL_GLYPHS];
        const lv_font_t *fn = i < CL_GLYPHS ? font : sym;
        td[i].cp = cp;
        td[i].font = fn;
        td[i].adv = (i < CL_GLYPHS && cp >= 0x7F && cp < 0xA0) ? 0 : (int)lv_font_get_glyph_width(fn, cp, 0);
        if (td[i].adv > maxadv) maxadv = td[i].adv;
    }
    if (lh2 > lh) lh = lh2;
    const int pad = CL_HALO + 1;
    const int cw = maxadv + 2 * pad + 2, ch = lh + 2 * CL_HALO;
    const int cols = 16, rows = (total + cols - 1) / cols;
    const int W = cols * cw, H = rows * ch;

    lv_draw_buf_t *db = lv_draw_buf_create((uint32_t)W, (uint32_t)H, LV_COLOR_FORMAT_L8, 0);
    if (!db) {
        cl_free(td);
        return false;
    }
    char(*txt)[5] = (char(*)[5])cl_malloc((size_t)total * 5);
    if (!txt) {
        lv_draw_buf_destroy(db);
        cl_free(td);
        return false;
    }
    lv_obj_t *cv = lv_canvas_create(lv_layer_top());
    lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(cv, db);
    lv_canvas_fill_bg(cv, lv_color_hex(0x000000), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    for (int i = 0; i < total; i++) {
        if (td[i].adv <= 0) continue;
        int n = utf8_put(txt[i], td[i].cp);
        txt[i][n] = 0;
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.color = lv_color_hex(0xFFFFFF);
        d.font = td[i].font;
        d.text = txt[i];
        int fl = lv_font_get_line_height(td[i].font);
        int x = (i % cols) * cw + pad, y = (i / cols) * ch + CL_HALO + (lh - fl) / 2;
        lv_area_t area = { x, y, x + cw - pad - 1, y + fl - 1 };
        lv_draw_label(&layer, &d, &area);
    }
    lv_canvas_finish_layer(cv, &layer);

    size_t bytes = 0;
    for (int i = 0; i < total; i++) {
        if (td[i].adv <= 0) continue;
        int w = td[i].adv + 2 * pad + 1;
        bytes += (size_t)(w > cw ? cw : w) * ch * 2;
    }
    f->atlas = (uint8_t *)cl_malloc(bytes ? bytes : 1);
    bool ok = f->atlas != NULL;
    size_t at = 0;
    for (int i = 0; i < total && ok; i++) {
        cl_glyph_t *g = i < CL_GLYPHS ? &f->g[i] : &f->x[i - CL_GLYPHS];
        if (i >= CL_GLYPHS) f->xcp[i - CL_GLYPHS] = td[i].cp;
        g->adv = (int16_t)td[i].adv;
        if (td[i].adv <= 0) continue;
        int w = td[i].adv + 2 * pad + 1, h = ch;
        if (w > cw) w = cw;
        g->w = (int16_t)w;
        g->h = (int16_t)h;
        g->a = f->atlas + at;
        at += (size_t)w * h * 2;
        g->halo = g->a + (size_t)w * h;
        int ox = (i % cols) * cw, oy = (i / cols) * ch;
        for (int y = 0; y < h; y++)
            memcpy(g->a + (size_t)y * w, db->data + (size_t)(oy + y) * db->header.stride + ox, (size_t)w);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int m = 0;
                for (int dy = -CL_HALO; dy <= CL_HALO; dy++) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    for (int dx = -CL_HALO; dx <= CL_HALO; dx++) {
                        int xx = x + dx;
                        if (xx < 0 || xx >= w || dx * dx + dy * dy > CL_HALO * CL_HALO + 1) continue;
                        int v = g->a[yy * w + xx];
                        if (v > m) m = v;
                    }
                }
                g->halo[y * w + x] = (uint8_t)m;
            }
        }
    }
    f->nx = nextra;
    lv_obj_delete(cv);
    lv_draw_buf_destroy(db);
    cl_free(txt);
    cl_free(td);
    f->line_h = lh;
    f->pad = pad;
    f->top = CL_HALO;
    f->ok = ok;
    if (!ok) cl_font_free(f);
    return ok;
}

void cl_font_free(cl_font_t *f)
{
    cl_free(f->atlas);
    f->atlas = NULL;
    for (int i = 0; i < CL_GLYPHS; i++) f->g[i].a = f->g[i].halo = NULL;
    for (int i = 0; i < CL_EXTRA; i++) f->x[i].a = f->x[i].halo = NULL;
    f->ok = false;
}

int cl_text_width(const cl_font_t *f, const char *s)
{
    if (!f->ok) return 0;
    int w = 0;
    uint32_t cp;
    while ((cp = cl_utf8_next(&s)) != 0) w += glyph(f, cp)->adv;
    return w;
}

static void mask(cl_fb_t *fb, const uint8_t *m, int w, int h, int x0, int y0, uint32_t rgb)
{
    uint16_t c = cl_565(rgb);
    for (int y = 0; y < h; y++) {
        int yy = y0 + y;
        if (yy < fb->cy0 || yy >= fb->cy1) continue;
        const uint8_t *mr = m + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int xx = x0 + x;
            if (!mr[x] || xx < fb->cx0 || xx >= fb->cx1) continue;
            uint16_t *p = cl_px(fb, xx, yy);
            *p = mr[x] >= 250 ? c : blend(*p, rgb, mr[x]);
        }
    }
}

void cl_text(cl_fb_t *fb, const cl_font_t *f, int x, int y, const char *s, uint32_t rgb, uint32_t halo_rgb, bool halo)
{
    if (!f->ok) return;
    rgb = cl_tint(fb, rgb);
    halo_rgb = cl_tint(fb, halo_rgb);
    /* the whole line's box against the clip first */
    if (y + f->line_h + CL_HALO < fb->cy0 || y - CL_HALO > fb->cy1) return;
    const int pad = f->pad;
    for (int pass = halo ? 0 : 1; pass < 2; pass++) {
        const char *p = s;
        int pen = x;
        uint32_t cp;
        while ((cp = cl_utf8_next(&p)) != 0) {
            const cl_glyph_t *g = glyph(f, cp);
            if (g->a && pen + g->w > fb->cx0 && pen - pad < fb->cx1)
                mask(fb, pass ? g->a : g->halo, g->w, g->h, pen - pad, y - f->top, pass ? rgb : halo_rgb);
            pen += g->adv;
        }
    }
}

void cl_text_c(cl_fb_t *fb, const cl_font_t *f, int cx, int y, const char *s, uint32_t rgb, uint32_t halo_rgb,
               bool halo)
{
    cl_text(fb, f, cx - cl_text_width(f, s) / 2, y, s, rgb, halo_rgb, halo);
}
