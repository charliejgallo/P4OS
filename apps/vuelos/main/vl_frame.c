/*
 * VUELOS - a frame: the screen cut out of the best map buffer, and on top
 * the rings around the place, the airports, the aircraft with their trails,
 * the card of the picked one and the buttons; then the whole of it to the
 * panel.
 *
 * The cut and the way to the panel are Mapas' (apps/mapas/main/mp_frame.c
 * explains why a whole frame and not a blit with a stride): upright, the
 * frame is drawn straight into the panel's free buffer and flipped to;
 * turned, it is cut into a->frame and the PPA turns it; without either (the
 * simulator), it goes through the LVGL canvas under everything.
 *
 * What is drawn here changes every frame (the aircraft move), so none of it
 * goes into the map buffers: those are the worker's, and only the map.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "vuelos.h"

#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PAD         24
#define BTN_R       38
#define BTN_HIT     50
#define PILL_H      72
#define PI_F        3.14159265f

#define INK     0xE8EAEE
#define DIM     0x9AA1AD
#define CARD    0x1A1E26
#define RING    0x5D7E9C
#define HOME    0x7FC8FF
#define RED     0xFF5A4E

/* ---------------------------------------------------------------------------
 * The layout
 * ------------------------------------------------------------------------- */

void vl_layout(app_t *a)
{
    lay_t *L = &a->L;
    int sw = a->sw, sh = a->sh;
    L->sw = sw;
    L->sh = sh;
    L->land = sw > sh;
    L->menu = (disc_t){ sw - PAD - BTN_R, PAD + PILL_H / 2, BTN_R };
    L->list = (disc_t){ L->menu.x - 2 * BTN_R - 16, L->menu.y, BTN_R };
    L->px0 = PAD;
    L->py0 = PAD;
    L->py1 = PAD + PILL_H;
    L->px1 = L->list.x - BTN_R - 16;
    if (L->px1 - L->px0 > 620) L->px1 = L->px0 + 620;

    /* the card of the picked aircraft: across the bottom upright, a column
     * on the left lying down */
    /* its height is what it holds: the name, what it is, the route, the
     * figures in rows of three (two lying down) and a last line */
    int ls = a->fonts[Q_FULL][VL_FONT_S].ok ? a->fonts[Q_FULL][VL_FONT_S].line_h : 20;
    int lm = a->fonts[Q_FULL][VL_FONT_M].ok ? a->fonts[Q_FULL][VL_FONT_M].line_h : 25;
    int ll = a->fonts[Q_FULL][VL_FONT_L].ok ? a->fonts[Q_FULL][VL_FONT_L].line_h : 30;
    int lb = a->fbig.ok ? a->fbig.line_h : 40;
    int rows = L->land ? 3 : 2;
    int ch = 22 + lb + 2 + lm + 10 + lm + 16 + rows * (ls + ll + 18) + 4 + ls + 24;
    if (L->land) {
        L->card_x = PAD;
        L->card_y = L->py1 + 16;
        L->card_w = 470;
        L->card_h = ch;
    } else {
        L->card_w = sw - 2 * PAD;
        L->card_h = ch;
        L->card_x = PAD;
        L->card_y = sh - L->card_h - 56;
    }
    L->card_close = (disc_t){ L->card_x + L->card_w - 46, L->card_y + 46, 30 };
    L->card_follow = (disc_t){ L->card_close.x - 76, L->card_y + 46, 30 };

    /* + - and the house at the right, above the card when it is up */
    int bottom = sh - 150;
    if (!L->land && a->sel >= 0) bottom = L->card_y - BTN_R - 24;
    L->zout = (disc_t){ sw - PAD - BTN_R, bottom, BTN_R };
    L->zin = (disc_t){ sw - PAD - BTN_R, bottom - 2 * BTN_R - 20, BTN_R };
    L->home = (disc_t){ sw - PAD - BTN_R, bottom - 4 * BTN_R - 56, BTN_R };
    L->scale_x = PAD + 6;
    L->scale_y = sh - 78;
    if (L->land && a->sel >= 0) L->scale_x = L->card_x + L->card_w + 24;
    L->attr_y = sh - 46;
}

bool vl_hit_disc(const disc_t *d, float x, float y)
{
    return fabsf(x - (float)d->x) < d->r + 12 && fabsf(y - (float)d->y) < d->r + 12;
}

bool vl_hit_card(const app_t *a, float x, float y)
{
    const lay_t *L = &a->L;
    return x >= L->card_x && x < L->card_x + L->card_w && y >= L->card_y && y < L->card_y + L->card_h;
}

int vl_frame_reserve(const app_t *a, const back_t *b, int16_t (*out)[4], int max)
{
    const lay_t *L = &a->L;
    int sr[4][4] = {
        { L->px0 - 6, L->py0 - 6, L->menu.x + BTN_R + 6, L->py1 + 6 },
        { L->home.x - BTN_R - 6, L->home.y - BTN_R - 6, L->zout.x + BTN_R + 6, L->zout.y + BTN_R + 6 },
        { L->scale_x - 6, L->scale_y - 30, L->scale_x + 260, L->scale_y + 8 },
        { PAD, L->attr_y - 4, L->sw - PAD, L->attr_y + 26 },
    };
    int n = 0;
    for (int i = 0; i < 4 && n < max; i++) {
        float ox = b->w * 0.5f - a->sw * 0.5f * b->res, oy = b->h * 0.5f - a->sh * 0.5f * b->res;
        out[n][0] = (int16_t)(ox + sr[i][0] * b->res);
        out[n][1] = (int16_t)(oy + sr[i][1] * b->res);
        out[n][2] = (int16_t)(ox + sr[i][2] * b->res);
        out[n][3] = (int16_t)(oy + sr[i][3] * b->res);
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * The cut (Mapas')
 * ------------------------------------------------------------------------- */

typedef struct {
    const back_t *b;
    float bxc, byc;
    float inv;
    bool  exact;
} cut_t;

static void cut_of(const app_t *a, const back_t *b, cut_t *c)
{
    const vl_view_t *v = &a->view;
    float s = vl_px_per_unit(b->v.z) * b->res;
    c->b = b;
    c->bxc = b->w * 0.5f + (float)(int32_t)(v->cx - b->v.cx) * s;
    c->byc = b->h * 0.5f + (float)(int32_t)(v->cy - b->v.cy) * s;
    c->inv = b->res * exp2f(b->v.z - v->z);
    c->exact = fabsf(c->inv - 1.0f) < 0.002f;
}

static float coverage(const app_t *a, const cut_t *c)
{
    float hw = a->sw * 0.5f * c->inv, hh = a->sh * 0.5f * c->inv;
    float x0 = c->bxc - hw, x1 = c->bxc + hw, y0 = c->byc - hh, y1 = c->byc + hh;
    float ix = (x1 < c->b->w ? x1 : c->b->w) - (x0 > 0 ? x0 : 0);
    float iy = (y1 < c->b->h ? y1 : c->b->h) - (y0 > 0 ? y0 : 0);
    if (ix <= 0 || iy <= 0) return 0;
    return ix * iy / ((x1 - x0) * (y1 - y0));
}

static bool choose(app_t *a, cut_t *out)
{
    bool have = false;
    float best_cov = 0, best_mag = 0;
    for (int q = 0; q < Q_N; q++) {
        int f = a->pair[q].front;
        if (f < 0) continue;
        back_t *b = &a->pair[q].b[f];
        if (b->geo != a->geo) continue;
        cut_t c;
        cut_of(a, b, &c);
        float cov = coverage(a, &c);
        float mag = fabsf(log2f(c.inv));
        bool full = cov > 0.9999f;
        bool better = !have || (full && best_cov <= 0.9999f) ||
                      (full && best_cov > 0.9999f && mag < best_mag - 0.01f) ||
                      (!full && best_cov <= 0.9999f && cov > best_cov + 0.001f);
        if (better) {
            *out = c;
            best_cov = cov;
            best_mag = mag;
            have = true;
        }
    }
    return have;
}

static int16_t s_col[AOS_PANEL_H];

static void cut_frame(app_t *a, const cut_t *c, uint16_t *dst)
{
    const int sw = a->sw, sh = a->sh;
    const uint16_t bg = vl_565(VL_BG);
    if (!c) {
        for (size_t i = 0; i < (size_t)sw * sh; i++) dst[i] = bg;
        return;
    }
    const back_t *b = c->b;
    if (c->exact) {
        int ox = (int)floorf(c->bxc - sw * 0.5f + 0.5f), oy = (int)floorf(c->byc - sh * 0.5f + 0.5f);
        int x0 = ox < 0 ? -ox : 0, x1 = ox + sw > b->w ? b->w - ox : sw;
        if (x1 < x0) x1 = x0;
        for (int y = 0; y < sh; y++) {
            uint16_t *d = dst + (size_t)y * sw;
            int by = oy + y;
            if (by < 0 || by >= b->h) {
                for (int x = 0; x < sw; x++) d[x] = bg;
                continue;
            }
            const uint16_t *srow = b->px + (size_t)by * b->w;
            for (int x = 0; x < x0; x++) d[x] = bg;
            if (x1 > x0) memcpy(d + x0, srow + ox + x0, (size_t)(x1 - x0) * 2);
            for (int x = x1; x < sw; x++) d[x] = bg;
        }
        return;
    }
    for (int x = 0; x < sw; x++) {
        int bx = (int)floorf(c->bxc + ((float)x + 0.5f - sw * 0.5f) * c->inv);
        s_col[x] = (int16_t)(bx < 0 || bx >= b->w ? -1 : bx);
    }
    int prev = -2;
    for (int y = 0; y < sh; y++) {
        uint16_t *d = dst + (size_t)y * sw;
        int by = (int)floorf(c->byc + ((float)y + 0.5f - sh * 0.5f) * c->inv);
        if (by < 0 || by >= b->h) by = -1;
        if (by == prev && y > 0) {
            memcpy(d, d - sw, (size_t)sw * 2);
            continue;
        }
        prev = by;
        if (by < 0) {
            for (int x = 0; x < sw; x++) d[x] = bg;
            continue;
        }
        const uint16_t *srow = b->px + (size_t)by * b->w;
        for (int x = 0; x < sw; x++) d[x] = s_col[x] < 0 ? bg : srow[s_col[x]];
    }
}

/* ---------------------------------------------------------------------------
 * Shapes
 * ------------------------------------------------------------------------- */

static void pill(vl_fb_t *fb, float x0, float y0, float x1, float y1, uint32_t rgb, int alpha)
{
    float h = y1 - y0, r = h * 0.5f, yc = (y0 + y1) * 0.5f;
    float l[4] = { x0 + r, yc, x1 - r, yc };
    vl_polyline(fb, l, 2, h, rgb, alpha);
}

/* a rounded rectangle, blended */
static void rrect(vl_fb_t *fb, int x, int y, int w, int h, int r, uint32_t rgb, int alpha)
{
    int y0 = y < fb->cy0 ? fb->cy0 : y, y1 = y + h > fb->cy1 ? fb->cy1 : y + h;
    int x0 = x < fb->cx0 ? fb->cx0 : x, x1 = x + w > fb->cx1 ? fb->cx1 : x + w;
    for (int j = y0; j < y1; j++) {
        float dy = j < y + r ? (float)(y + r - j) - 0.5f : j >= y + h - r ? (float)(j - (y + h - r)) + 0.5f : 0;
        for (int i = x0; i < x1; i++) {
            float dx = i < x + r ? (float)(x + r - i) - 0.5f : i >= x + w - r ? (float)(i - (x + w - r)) + 0.5f : 0;
            int al = alpha;
            if (dx > 0 && dy > 0) {
                float d = sqrtf(dx * dx + dy * dy) - (float)r;
                if (d >= 0.5f) continue;
                if (d > -0.5f) al = (int)((0.5f - d) * (float)alpha);
            }
            vl_blend_px(fb, i, j, rgb, al);
        }
    }
}

static void button(vl_fb_t *fb, const disc_t *d, bool on)
{
    vl_disc(fb, (float)d->x, (float)d->y, d->r + 2.0f, 0x000000, 140);
    vl_disc(fb, (float)d->x, (float)d->y, (float)d->r, on ? 0x2F6FB0 : CARD, 238);
}

static void circle(vl_fb_t *fb, float cx, float cy, float r, float w, uint32_t rgb, int alpha)
{
    int n = (int)(r * 0.6f);
    if (n < 24) n = 24;
    if (n > 256) n = 256;
    float xy[2 * 257];
    for (int i = 0; i <= n; i++) {
        float t = (float)i / (float)n * 2.0f * PI_F;
        xy[2 * i] = cx + r * cosf(t);
        xy[2 * i + 1] = cy + r * sinf(t);
    }
    vl_polyline(fb, xy, n + 1, w, rgb, alpha);
}

static void fit(const vl_font_t *f, const char *s, int w, char *out, size_t n)
{
    snprintf(out, n, "%s", s);
    if (vl_text_width(f, out) <= w) return;
    size_t l = strlen(out);
    while (l > 0) {
        l--;
        while (l > 0 && ((unsigned char)out[l] & 0xC0) == 0x80) l--;
        if (l + 4 > n) continue;
        memcpy(out + l, "...", 4);
        if (vl_text_width(f, out) <= w) return;
    }
}

/* ---------------------------------------------------------------------------
 * Colour by altitude: warm low, cold high, as radar pages do
 * ------------------------------------------------------------------------- */

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    int r = (int)(((a >> 16) & 255) + (((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t));
    int g = (int)(((a >> 8) & 255) + (((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t));
    int bl = (int)((a & 255) + (((int)(b & 255) - (int)(a & 255)) * t));
    return (uint32_t)(r << 16 | g << 8 | bl);
}

static uint32_t alt_color_ft(int32_t ft)
{
    static const struct { int32_t ft; uint32_t rgb; } T[] = {
        { 0, 0xFF7A45 }, { 2000, 0xFFA43A }, { 6000, 0xF2D43D }, { 10000, 0xA5E05A },
        { 20000, 0x45D6C4 }, { 30000, 0x5AA8FF }, { 40000, 0xB98BFF },
    };
    const int n = (int)(sizeof T / sizeof T[0]);
    if (ft <= T[0].ft) return T[0].rgb;
    for (int i = 1; i < n; i++)
        if (ft < T[i].ft) return mix(T[i - 1].rgb, T[i].rgb, (float)(ft - T[i - 1].ft) / (float)(T[i].ft - T[i - 1].ft));
    return T[n - 1].rgb;
}

uint32_t vl_alt_color(const vl_ac_t *ac)
{
    if (ac->ground) return 0x8A8F99;
    if (!ac->has_alt) return 0xC8CCD4;
    return alt_color_ft(ac->alt_ft);
}

/* ---------------------------------------------------------------------------
 * The aircraft's shapes: outlines in pixels, nose up (north), turned by the
 * track
 * ------------------------------------------------------------------------- */

static const float SH_JET[] = {
    0, -15, 2, -12.5f, 2.2f, -4, 14, 3, 14, 5.5f, 2.2f, 2.5f, 1.8f, 10, 6, 13.2f, 6, 15, 0, 13.6f,
    -6, 15, -6, 13.2f, -1.8f, 10, -2.2f, 2.5f, -14, 5.5f, -14, 3, -2.2f, -4, -2, -12.5f,
};
static const float SH_PROP[] = {
    0, -12, 1.8f, -10, 1.8f, -4.5f, 12.5f, -3.5f, 12.5f, -0.8f, 1.8f, 0.2f, 1.3f, 8, 5, 9.6f, 5, 11.4f,
    0, 10.6f, -5, 11.4f, -5, 9.6f, -1.3f, 8, -1.8f, 0.2f, -12.5f, -0.8f, -12.5f, -3.5f, -1.8f, -4.5f, -1.8f, -10,
};
static const float SH_LIGHT[] = {
    0, -9, 1.4f, -7.5f, 1.4f, -6, 10, -5.5f, 10, -3.5f, 1.3f, -3, 1, 6, 4, 7, 4, 8.5f, 0, 8,
    -4, 8.5f, -4, 7, -1, 6, -1.3f, -3, -10, -3.5f, -10, -5.5f, -1.4f, -6, -1.4f, -7.5f,
};
static const float SH_GLIDER[] = {
    0, -9, 1, -7, 1, -3, 16, -2.6f, 16, -1.3f, 1, -0.8f, 0.6f, 8, 3.6f, 9, 3.6f, 10.2f, -3.6f, 10.2f,
    -3.6f, 9, -0.6f, 8, -1, -0.8f, -16, -1.3f, -16, -2.6f, -1, -3, -1, -7,
};
static const float SH_HELI[] = {
    0, -7, 3.5f, -5, 4, 0, 2.5f, 4, 1, 5, 0.8f, 13, 3.5f, 13.5f, 3.5f, 15, -3.5f, 15, -3.5f, 13.5f, -0.8f, 13,
    -1, 5, -2.5f, 4, -4, 0, -3.5f, -5,
};

static void shape(vl_fb_t *fb, const float *pts, int n, float cx, float cy, float ang, float scale, uint32_t rgb,
                  bool halo)
{
    float xy[2 * 24];
    float c = cosf(ang), s = sinf(ang);
    for (int i = 0; i < n; i++) {
        float lx = pts[2 * i] * scale, ly = pts[2 * i + 1] * scale;
        xy[2 * i] = cx + lx * c - ly * s;
        xy[2 * i + 1] = cy + lx * s + ly * c;
    }
    if (halo) {
        xy[2 * n] = xy[0];
        xy[2 * n + 1] = xy[1];
        vl_polyline(fb, xy, n + 1, 3.4f, 0x000000, 170);
    }
    uint32_t rn = (uint32_t)n;
    vl_fill_poly(fb, xy, &rn, 1, rgb);
}

static void aircraft_icon(vl_fb_t *fb, const vl_ac_t *ac, float x, float y, uint32_t rgb)
{
    float ang = (ac->has_trk ? ac->trk : 0) * PI_F / 180.0f;
    switch (ac->kind) {
    case K_HEAVY: shape(fb, SH_JET, 18, x, y, ang, 1.3f, rgb, true); break;
    case K_PROP: shape(fb, SH_PROP, 18, x, y, ang, 1.0f, rgb, true); break;
    case K_LIGHT: shape(fb, SH_LIGHT, 18, x, y, ang, 1.0f, rgb, true); break;
    case K_GLIDER: shape(fb, SH_GLIDER, 18, x, y, ang, 1.0f, rgb, true); break;
    case K_HELI:
        circle(fb, x, y - 0, 12.5f, 3.6f, 0x000000, 120);
        circle(fb, x, y, 12.5f, 1.8f, rgb, 210);
        shape(fb, SH_HELI, 16, x, y, ang, 1.0f, rgb, true);
        break;
    case K_BALLOON:
        vl_disc(fb, x, y - 3, 10.5f, 0x000000, 170);
        vl_disc(fb, x, y - 3, 9, rgb, 255);
        vl_disc(fb, x, y + 9.5f, 3.5f, 0x000000, 170);
        vl_disc(fb, x, y + 9.5f, 2.4f, rgb, 255);
        break;
    case K_DRONE: {
        float l1[4] = { x - 8, y - 8, x + 8, y + 8 }, l2[4] = { x - 8, y + 8, x + 8, y - 8 };
        vl_polyline(fb, l1, 2, 5.0f, 0x000000, 160);
        vl_polyline(fb, l2, 2, 5.0f, 0x000000, 160);
        vl_polyline(fb, l1, 2, 2.6f, rgb, 255);
        vl_polyline(fb, l2, 2, 2.6f, rgb, 255);
        for (int i = 0; i < 4; i++) vl_disc(fb, x + (i & 1 ? 8 : -8), y + (i & 2 ? 8 : -8), 3.4f, rgb, 255);
        break;
    }
    case K_GROUND:
        vl_disc(fb, x, y, 6.5f, 0x000000, 170);
        vl_disc(fb, x, y, 5, rgb, 255);
        break;
    default: shape(fb, SH_JET, 18, x, y, ang, 1.0f, rgb, true); break;
    }
}

/* ---------------------------------------------------------------------------
 * The rings, the place, the airports
 * ------------------------------------------------------------------------- */

static void rings(app_t *a, vl_fb_t *fb)
{
    const vl_font_t *fs = &a->fonts[Q_FULL][VL_FONT_S];
    float px, py;
    vl_to_screen(a, a->place_x, a->place_y, &px, &py);
    float mpp = vl_metres_per_px(&a->view);
    bool nm = a->conf.units == UNITS_AVIATION;
    float unit = nm ? 1852.0f : 1000.0f;
    static const float STEP[] = { 1, 2, 5, 10, 20, 25, 50, 100, 200, 500 };
    float step = 0;
    for (size_t i = 0; i < sizeof STEP / sizeof STEP[0]; i++) {
        if (STEP[i] * unit / mpp >= 120.0f) {
            step = STEP[i];
            break;
        }
    }
    if (step <= 0) return;
    float far = sqrtf((float)(a->sw * a->sw + a->sh * a->sh)) + fabsf(px - a->sw * 0.5f) + fabsf(py - a->sh * 0.5f);
    float rpx = step * unit / mpp;
    for (int k = 1; k <= 12 && rpx * k < far; k++) {
        float r = rpx * k;
        circle(fb, px, py, r, 1.4f, RING, 120);
        if (!fs->ok) continue;
        char t[16];
        snprintf(t, sizeof t, "%g %s", (double)(step * k), nm ? "NM" : "km");
        float lx = px + r * 0.7071f + 4, ly = py - r * 0.7071f - fs->line_h - 2;
        if (lx > 0 && lx < a->sw - 60 && ly > a->L.py1 && ly < a->sh - 40)
            vl_text(fb, fs, (int)lx, (int)ly, t, 0x8FA9C2, VL_BG, true);
    }
    /* the radius asked for, a little brighter */
    float rq = (float)a->conf.radius_km * 1000.0f / mpp;
    if (rq < far) circle(fb, px, py, rq, 2.0f, 0x7F9FBF, 150);
    /* the place: a dot in a ring */
    vl_disc(fb, px, py, 9, 0x000000, 150);
    vl_disc(fb, px, py, 6.5f, HOME, 255);
    circle(fb, px, py, 15, 2.2f, HOME, 200);
}

static void airports(app_t *a, vl_fb_t *fb)
{
    if (!a->nap) return;
    float z = a->view.z;
    if (z < 5.5f) return;
    const vl_font_t *fs = &a->fonts[Q_FULL][VL_FONT_S];
    float s = vl_px_per_unit(z);
    uint32_t hw = (uint32_t)((a->sw * 0.5f + 40) / s), hh = (uint32_t)((a->sh * 0.5f + 40) / s);
    float lon0, lat0, lon1, lat1;
    vl_world_to_lonlat(a->view.cx - hw, a->view.cy + hh, &lon0, &lat0);     /* south-west */
    vl_world_to_lonlat(a->view.cx + hw, a->view.cy - hh, &lon1, &lat1);     /* north-east */
    int32_t la0 = (int32_t)(lat0 * 1e5f), la1 = (int32_t)(lat1 * 1e5f);
    int32_t lo0 = (int32_t)(lon0 * 1e5f), lo1 = (int32_t)(lon1 * 1e5f);
    bool wrap = lo0 > lo1;                  /* the screen crosses 180 degrees */
    /* the first at or above the south edge, by halving */
    int lo = 0, hi = a->nap;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (a->ap[m].lat < la0) lo = m + 1;
        else hi = m;
    }
    int drawn = 0;
    for (int i = lo; i < a->nap && a->ap[i].lat <= la1 && drawn < 120; i++) {
        const vl_airport_t *ap = &a->ap[i];
        static const float MINZ[] = { 5.5f, 7.5f, 9.5f, 11.0f };
        if (z < MINZ[ap->kind & 3]) continue;
        /* the pack holds the whole world: the latitude band is everything
         * round the globe, so the longitude goes first, before any maths */
        if (wrap ? ap->lon < lo0 && ap->lon > lo1 : ap->lon < lo0 || ap->lon > lo1) continue;
        uint32_t wx, wy;
        vl_lonlat_to_world((float)ap->lon / 1e5f, (float)ap->lat / 1e5f, &wx, &wy);
        float x, y;
        vl_to_screen(a, wx, wy, &x, &y);
        if (x < -20 || x > a->sw + 20 || y < -20 || y > a->sh + 20) continue;
        float r = ap->kind == AP_LARGE ? 7 : ap->kind == AP_MEDIUM ? 5.5f : 4;
        vl_disc(fb, x, y, r + 2, 0x000000, 150);
        vl_disc(fb, x, y, r, 0xB3BBC9, 230);
        vl_disc(fb, x, y, r * 0.45f, 0x2A303B, 255);
        if (fs->ok && (z >= 8.5f || ap->kind == AP_LARGE)) {
            const char *code = ap->iata[0] ? ap->iata : ap->icao;
            vl_text(fb, fs, (int)(x + r + 6), (int)(y - fs->line_h / 2), code, 0xA6AFBD, VL_BG, true);
        }
        drawn++;
    }
}

/* ---------------------------------------------------------------------------
 * The aircraft
 * ------------------------------------------------------------------------- */

typedef struct {
    float x, y;
    bool  on;
} spot_t;

static spot_t s_spot[VL_MAX_AC];

static void trail(app_t *a, vl_fb_t *fb, const vl_ac_t *ac, float x, float y, uint32_t now, bool sel)
{
    int n = ac->ntrail;
    if (!n) return;
    uint32_t keep = sel ? 0xFFFFFFFFu : (uint32_t)a->conf.trail_min * 60000u;
    if (!keep) return;
    float px = x, py = y;
    int32_t palt = ac->ground ? 0 : ac->has_alt ? ac->alt_ft : INT32_MIN;
    /* from the newest back */
    for (int k = 0; k < n; k++) {
        const trail_pt_t *p = &ac->trail[(ac->trail_i + VL_TRAIL - 1 - k) % VL_TRAIL];
        uint32_t age = now - p->t;
        if (age > keep) break;
        float qx, qy;
        vl_to_screen(a, p->x, p->y, &qx, &qy);
        float seg[4] = { qx, qy, px, py };
        int32_t alt = palt != INT32_MIN ? palt : p->alt;
        uint32_t rgb = alt == INT32_MIN ? 0xC8CCD4 : ac->ground ? 0x8A8F99 : alt_color_ft(alt);
        float f = sel ? 1.0f : 1.0f - (float)age / (float)keep;
        int al = (int)(40 + 170 * f);
        if (!(qx < -50 && px < -50) && !(qx > a->sw + 50 && px > a->sw + 50) && !(qy < -50 && py < -50) &&
            !(qy > a->sh + 50 && py > a->sh + 50))
            vl_polyline(fb, seg, 2, sel ? 3.2f : 2.2f, rgb, al);
        px = qx;
        py = qy;
        palt = p->alt;
    }
}

static bool label_free(float (*boxes)[4], int n, float x0, float y0, float x1, float y1)
{
    for (int i = 0; i < n; i++)
        if (x0 < boxes[i][2] && x1 > boxes[i][0] && y0 < boxes[i][3] && y1 > boxes[i][1]) return false;
    return true;
}

static void draw_aircraft(app_t *a, vl_fb_t *fb, uint32_t now)
{
    const vl_font_t *fs = &a->fonts[Q_FULL][VL_FONT_S];
    float fastest = 0;
    float s = vl_px_per_unit(a->view.z);
    int visible = 0;
    for (int i = 0; i < a->nac; i++) {
        vl_ac_t *ac = &a->ac[i];
        s_spot[i].on = false;
        if (!ac->used || vl_ac_stale(ac, now)) continue;
        uint32_t wx, wy;
        vl_ac_pos(a, ac, now, &wx, &wy);
        float x, y;
        vl_to_screen(a, wx, wy, &x, &y);
        s_spot[i].x = x;
        s_spot[i].y = y;
        bool near = x > -400 && x < a->sw + 400 && y > -400 && y < a->sh + 400;
        if (!near) continue;
        s_spot[i].on = true;
        if (x > -20 && x < a->sw + 20 && y > -20 && y < a->sh + 20) {
            visible++;
            float v = sqrtf(ac->vx * ac->vx + ac->vy * ac->vy) * s;
            if (v > fastest) fastest = v;
        }
    }
    a->ac_speed_px = fastest;

    /* the trails under everything */
    for (int i = 0; i < a->nac; i++)
        if (s_spot[i].on && i != a->sel) trail(a, fb, &a->ac[i], s_spot[i].x, s_spot[i].y, now, false);
    if (a->sel >= 0 && s_spot[a->sel].on) trail(a, fb, &a->ac[a->sel], s_spot[a->sel].x, s_spot[a->sel].y, now, true);

    /* the icons, the lower first (the higher fly over them) */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < a->nac; i++) {
            const vl_ac_t *ac = &a->ac[i];
            if (!s_spot[i].on || i == a->sel) continue;
            bool high = ac->has_alt && !ac->ground && ac->alt_ft >= 10000;
            if (high != (pass == 1)) continue;
            float x = s_spot[i].x, y = s_spot[i].y;
            if (x < -30 || x > a->sw + 30 || y < -30 || y > a->sh + 30) continue;
            bool emerg = ac->has_sq && (ac->sq == 7500 || ac->sq == 7600 || ac->sq == 7700);
            if (emerg) {
                int al = 120 + (int)(100 * sinf((float)(now % 1000) / 1000.0f * 2 * PI_F));
                circle(fb, x, y, 22, 3.0f, RED, al);
            }
            aircraft_icon(fb, ac, x, y, vl_alt_color(ac));
        }
    }
    if (a->sel >= 0 && s_spot[a->sel].on) {
        const vl_ac_t *ac = &a->ac[a->sel];
        float x = s_spot[a->sel].x, y = s_spot[a->sel].y;
        vl_disc(fb, x, y, 26, 0xFFFFFF, 40);
        circle(fb, x, y, 26, 2.4f, 0xFFFFFF, 220);
        aircraft_icon(fb, ac, x, y, vl_alt_color(ac));
    }

    /* the labels: the callsign under each one, and its altitude closer in,
     * where they do not run into each other */
    if (!fs->ok) return;
    static float boxes[VL_MAX_AC][4];
    int nb = 0;
    bool two = a->view.z >= 9.0f;
    bool all = a->conf.labels && visible <= 80;
    for (int k = -1; k < a->nac; k++) {
        int i = k < 0 ? a->sel : k;
        if (i < 0 || (k >= 0 && i == a->sel) || !s_spot[i].on) continue;
        if (!all && i != a->sel) continue;
        const vl_ac_t *ac = &a->ac[i];
        float x = s_spot[i].x, y = s_spot[i].y;
        if (x < 0 || x > a->sw || y < 0 || y > a->sh) continue;
        const char *name = ac->flight[0] ? ac->flight : ac->reg[0] ? ac->reg : NULL;
        char alt[24] = "";
        if (two || i == a->sel) vl_fmt_alt(a, ac, alt, sizeof alt);
        if (!name && !alt[0]) continue;
        int w1 = name ? vl_text_width(fs, name) : 0, w2 = alt[0] ? vl_text_width(fs, alt) : 0;
        int w = w1 > w2 ? w1 : w2, h = fs->line_h * ((name ? 1 : 0) + (alt[0] ? 1 : 0));
        float x0 = x - w * 0.5f, y0 = y + 20, x1 = x0 + w, y1 = y0 + h;
        if (i != a->sel && !label_free(boxes, nb, x0 - 4, y0, x1 + 4, y1)) continue;
        if (nb < VL_MAX_AC) {
            boxes[nb][0] = x0 - 4;
            boxes[nb][1] = y0;
            boxes[nb][2] = x1 + 4;
            boxes[nb][3] = y1;
            nb++;
        }
        int ty = (int)y0;
        if (name) {
            vl_text(fb, fs, (int)(x - w1 * 0.5f), ty, name, i == a->sel ? 0xFFFFFF : 0xDDE1E8, VL_BG, true);
            ty += fs->line_h;
        }
        if (alt[0]) vl_text(fb, fs, (int)(x - w2 * 0.5f), ty, alt, vl_alt_color(ac), VL_BG, true);
    }
}

/* ---------------------------------------------------------------------------
 * The card of the picked aircraft
 * ------------------------------------------------------------------------- */

static const char *kind_name(const vl_ac_t *ac)
{
    switch (ac->kind) {
    case K_HEAVY: return _("de fuselaje ancho");
    case K_PROP: return _("regional");
    case K_LIGHT: return _("liviano");
    case K_HELI: return _("helicóptero");
    case K_GLIDER: return _("planeador");
    case K_BALLOON: return _("globo");
    case K_DRONE: return _("dron");
    case K_GROUND: return _("vehículo");
    default: return _("de línea");
    }
}

static void field(app_t *a, vl_fb_t *fb, int x, int y, int w, const char *label, const char *value, uint32_t rgb)
{
    const vl_font_t *fs = &a->fonts[Q_FULL][VL_FONT_S], *fl = &a->fonts[Q_FULL][VL_FONT_L];
    char t[48];
    fit(fs, label, w - 8, t, sizeof t);
    vl_text(fb, fs, x, y, t, DIM, CARD, false);
    fit(fl, value, w - 8, t, sizeof t);
    vl_text(fb, fl, x, y + fs->line_h + 2, t, rgb, CARD, false);
}

static void card(app_t *a, vl_fb_t *fb, uint32_t now)
{
    const lay_t *L = &a->L;
    const vl_ac_t *ac = &a->ac[a->sel];
    const vl_font_t *fs = &a->fonts[Q_FULL][VL_FONT_S], *fm = &a->fonts[Q_FULL][VL_FONT_M];
    const vl_font_t *fb32 = &a->fbig;
    if (!fs->ok || !fm->ok || !fb32->ok) return;
    bool emerg = ac->has_sq && (ac->sq == 7500 || ac->sq == 7600 || ac->sq == 7700);
    int x = L->card_x, y = L->card_y, w = L->card_w, h = L->card_h;
    rrect(fb, x - 2, y - 2, w + 4, h + 4, 26, emerg ? RED : 0x000000, emerg ? 255 : 150);
    rrect(fb, x, y, w, h, 24, CARD, 240);

    /* the buttons: follow it, and let it go */
    button(fb, &L->card_follow, a->follow);
    circle(fb, (float)L->card_follow.x, (float)L->card_follow.y, 12, 2.6f, INK, 255);
    vl_disc(fb, (float)L->card_follow.x, (float)L->card_follow.y, 4.5f, INK, 255);
    float l1[4] = { (float)L->card_follow.x, L->card_follow.y - 19.0f, (float)L->card_follow.x, L->card_follow.y - 12.0f };
    float l2[4] = { (float)L->card_follow.x, L->card_follow.y + 12.0f, (float)L->card_follow.x, L->card_follow.y + 19.0f };
    float l3[4] = { L->card_follow.x - 19.0f, (float)L->card_follow.y, L->card_follow.x - 12.0f, (float)L->card_follow.y };
    float l4[4] = { L->card_follow.x + 12.0f, (float)L->card_follow.y, L->card_follow.x + 19.0f, (float)L->card_follow.y };
    vl_polyline(fb, l1, 2, 2.6f, INK, 255);
    vl_polyline(fb, l2, 2, 2.6f, INK, 255);
    vl_polyline(fb, l3, 2, 2.6f, INK, 255);
    vl_polyline(fb, l4, 2, 2.6f, INK, 255);
    button(fb, &L->card_close, false);
    float cx = (float)L->card_close.x, cy = (float)L->card_close.y;
    float c1[4] = { cx - 10, cy - 10, cx + 10, cy + 10 }, c2[4] = { cx - 10, cy + 10, cx + 10, cy - 10 };
    vl_polyline(fb, c1, 2, 3.0f, INK, 255);
    vl_polyline(fb, c2, 2, 3.0f, INK, 255);

    /* the name, and what it is */
    int tx = x + 28, ty = y + 22, tw = L->card_follow.x - 40 - tx;
    char t[96], u[64];
    const char *name = ac->flight[0] ? ac->flight : ac->reg[0] ? ac->reg : NULL;
    if (!name) {
        snprintf(u, sizeof u, "%06X", (unsigned)(ac->hex & 0xFFFFFF));
        name = u;
    }
    fit(fb32, name, tw, t, sizeof t);
    vl_text(fb, fb32, tx, ty, t, INK, CARD, false);
    ty += fb32->line_h + 2;
    char what[64];
    snprintf(what, sizeof what, "%s%s%s%s%s", ac->reg[0] && ac->flight[0] ? ac->reg : "",
             ac->reg[0] && ac->flight[0] ? " · " : "", ac->type[0] ? ac->type : "", ac->type[0] ? " · " : "",
             kind_name(ac));
    fit(fm, what, L->card_follow.x - 40 - tx, t, sizeof t);
    vl_text(fb, fm, tx, ty, t, DIM, CARD, false);
    ty += fm->line_h + 10;

    /* the route, when the service knows it */
    const vl_route_t *r = vl_route_of(a, ac);
    if (r && r->state == 1) {
        char ft[40], tt[40];
        snprintf(ft, sizeof ft, "%s%s%s", r->from, r->from_city[0] ? " " : "", r->from_city);
        snprintf(tt, sizeof tt, "%s%s%s", r->to, r->to_city[0] ? " " : "", r->to_city);
        /* from, an arrow, to: each half the width */
        int half = (w - 56 - 56) / 2;
        fit(fm, ft, half, t, sizeof t);
        int fw = vl_text_width(fm, t);
        vl_text(fb, fm, tx, ty, t, 0xD9E6F5, CARD, false);
        float ay = ty + fm->line_h * 0.5f + 1, ax0 = (float)(tx + fw + 14), ax1 = ax0 + 28;
        float shaft[4] = { ax0, ay, ax1, ay }, head[6] = { ax1 - 8, ay - 7, ax1, ay, ax1 - 8, ay + 7 };
        vl_polyline(fb, shaft, 2, 2.4f, 0x9FB8D6, 255);
        vl_polyline(fb, head, 3, 2.4f, 0x9FB8D6, 255);
        fit(fm, tt, half, t, sizeof t);
        vl_text(fb, fm, (int)ax1 + 14, ty, t, 0xD9E6F5, CARD, false);
    } else if (r && r->state == 0) {
        vl_text(fb, fm, tx, ty, _("Buscando la ruta..."), DIM, CARD, false);
    } else if (a->conf.routes && a->conf.src != SRC_LOCAL && ac->flight[0] && !ac->ground) {
        vl_text(fb, fm, tx, ty, r ? _("Ruta desconocida") : "", DIM, CARD, false);
    }
    ty += fm->line_h + 16;

    /* the figures: three columns upright, two lying down */
    char alt[24], gs[24], vr[24], trk[16], sq[16], dist[40];
    vl_fmt_alt(a, ac, alt, sizeof alt);
    vl_fmt_speed(a, ac, gs, sizeof gs);
    vl_fmt_vrate(a, ac, vr, sizeof vr);
    if (ac->has_trk) snprintf(trk, sizeof trk, "%03d°", (int)roundf(ac->trk) % 360);
    else snprintf(trk, sizeof trk, "-");
    if (ac->has_sq) snprintf(sq, sizeof sq, "%04u", (unsigned)ac->sq);
    else snprintf(sq, sizeof sq, "-");
    char dd[24];
    vl_fmt_dist(a, ac->dist_km, dd, sizeof dd);
    static const char *const DIR[] = { N_("N"), N_("NE"), N_("E"), N_("SE"), N_("S"), N_("SO"), N_("O"), N_("NO") };
    snprintf(dist, sizeof dist, "%s %s", dd, _(DIR[(int)((ac->brg + 22.5f) / 45.0f) & 7]));
    const char *lab[6] = { _("Altitud"), _("Velocidad"), _("Vel. vertical"), _("Rumbo"), _("Squawk"), _("Distancia") };
    const char *val[6] = { alt, gs, vr, trk, sq, dist };
    int cols = L->land ? 2 : 3;
    int cw = (w - 56) / cols, rh = fs->line_h + a->fonts[Q_FULL][VL_FONT_L].line_h + 18;
    for (int i = 0; i < 6; i++) {
        int c = i % cols, row = i / cols;
        uint32_t rgb = i == 0 ? vl_alt_color(ac) : i == 4 && emerg ? RED : INK;
        field(a, fb, tx + c * cw, ty + row * rh, cw, lab[i], val[i], rgb);
    }
    ty += rh * ((6 + cols - 1) / cols) + 4;

    /* when it was seen, and its address */
    uint32_t ago = (now - ac->fix_ms) / 1000;
    if (emerg) {
        snprintf(u, sizeof u, "%s", ac->sq == 7700 ? _("Squawk de emergencia") : ac->sq == 7600 ? _("Squawk de falla de radio")
                                                                                    : _("Squawk de interferencia ilícita"));
        vl_text(fb, fs, tx, ty, u, RED, CARD, false);
    } else {
        snprintf(u, sizeof u, _("visto hace %u s · %06X%s"), (unsigned)ago, (unsigned)(ac->hex & 0xFFFFFF),
                 ac->mil ? _(" · militar") : "");
        if (ty + fs->line_h < y + h) vl_text(fb, fs, tx, ty, u, DIM, CARD, false);
    }
}

/* ---------------------------------------------------------------------------
 * The bar, the buttons, the scale
 * ------------------------------------------------------------------------- */

void vl_status_text(app_t *a, char *out, size_t n, uint32_t *rgb)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    *rgb = INK;
    bool net = aos_hal_net_state() == AOS_NET_CONNECTED;
    int e = a->feed_err;
    if (!net) {
        snprintf(out, n, "%s", _("Sin conexión"));
        *rgb = 0xFFD27A;
    } else if (a->conf.src == SRC_LOCAL && !a->conf.url[0]) {
        snprintf(out, n, "%s", _("Falta la dirección del receptor"));
        *rgb = 0xFFD27A;
    } else if (e && !a->feed_ok_ms) {
        if (e == 429) snprintf(out, n, "%s", _("Cupo de pedidos agotado"));
        else if (e == AOS_HTTP_ERR_SIN_HORA) snprintf(out, n, "%s", _("Esperando la hora"));
        else if (e == -200) snprintf(out, n, "%s", _("Respuesta inesperada"));
        else if (e == 401 || e == 403) snprintf(out, n, "%s", _("Acceso rechazado"));
        else if (e == AOS_HTTP_ERR_MEM) snprintf(out, n, "%s", _("Sin memoria"));
        else if (e == AOS_HTTP_ERR_RECV) snprintf(out, n, "%s", _("Sin respuesta"));
        else snprintf(out, n, _("Error %d"), e);
        *rgb = 0xFFD27A;
    } else if (!a->feed_ok_ms) {
        snprintf(out, n, "%s", _("Buscando aviones..."));
        *rgb = DIM;
    } else {
        uint32_t ago = (now - a->feed_ok_ms) / 1000;
        char cnt[32];
        if (a->feed_count == 1) snprintf(cnt, sizeof cnt, "%s", _("1 avión"));
        else snprintf(cnt, sizeof cnt, _("%d aviones"), a->feed_count);
        snprintf(out, n, "%s · %s · %u s", cnt, vl_src_name(vl_src_now(a)), (unsigned)ago);
        if (e) *rgb = 0xFFD27A;
    }
}

static void draw_bar(app_t *a, vl_fb_t *fb)
{
    const lay_t *L = &a->L;
    const vl_font_t *fs = &a->fonts[Q_FULL][VL_FONT_S], *fm = &a->fonts[Q_FULL][VL_FONT_M];
    pill(fb, L->px0 - 2, L->py0 - 2, L->px1 + 2, L->py1 + 2, 0x000000, 140);
    pill(fb, L->px0, L->py0, L->px1, L->py1, CARD, 238);
    /* a small aircraft at its start */
    shape(fb, SH_JET, 18, L->px0 + 38.0f, (L->py0 + L->py1) * 0.5f, PI_F / 4, 1.0f, 0xFFC94A, false);
    if (fm->ok) {
        char st[96], t[96];
        uint32_t rgb;
        vl_status_text(a, st, sizeof st, &rgb);
        fit(fm, st, L->px1 - L->px0 - 96, t, sizeof t);
        vl_text(fb, fm, L->px0 + 70, (L->py0 + L->py1 - fm->line_h) / 2, t, rgb, CARD, false);
    }

    /* the list: three lines; the menu: a gear-like sun */
    button(fb, &L->list, false);
    for (int i = -1; i <= 1; i++) {
        float l[4] = { L->list.x - 10.0f, L->list.y + i * 9.0f, L->list.x + 14.0f, L->list.y + i * 9.0f };
        vl_polyline(fb, l, 2, 3.2f, INK, 255);
        vl_disc(fb, L->list.x - 16.0f, L->list.y + i * 9.0f, 2.2f, INK, 255);
    }
    button(fb, &L->menu, false);
    for (int i = 0; i < 8; i++) {
        float t = (float)i * PI_F / 4;
        float l[4] = { L->menu.x + 10 * cosf(t), L->menu.y + 10 * sinf(t), L->menu.x + 17 * cosf(t), L->menu.y + 17 * sinf(t) };
        vl_polyline(fb, l, 2, 4.4f, INK, 255);
    }
    circle(fb, (float)L->menu.x, (float)L->menu.y, 10, 3.6f, INK, 255);

    button(fb, &L->home, false);
    float hx = (float)L->home.x, hy = (float)L->home.y;
    float roof[6] = { hx - 15, hy - 1, hx, hy - 14, hx + 15, hy - 1 };
    vl_polyline(fb, roof, 3, 3.4f, INK, 255);
    float wall[8] = { hx - 10, hy - 4, hx - 10, hy + 12, hx + 10, hy + 12, hx + 10, hy - 4 };
    vl_polyline(fb, wall, 4, 3.0f, INK, 255);
    button(fb, &L->zin, false);
    float h[4] = { L->zin.x - 14.0f, (float)L->zin.y, L->zin.x + 14.0f, (float)L->zin.y };
    float vv[4] = { (float)L->zin.x, L->zin.y - 14.0f, (float)L->zin.x, L->zin.y + 14.0f };
    vl_polyline(fb, h, 2, 4.0f, INK, 255);
    vl_polyline(fb, vv, 2, 4.0f, INK, 255);
    button(fb, &L->zout, false);
    float m[4] = { L->zout.x - 14.0f, (float)L->zout.y, L->zout.x + 14.0f, (float)L->zout.y };
    vl_polyline(fb, m, 2, 4.0f, INK, 255);

    if (!fs->ok) return;
    const int lh = fs->line_h;
    float mpp = vl_metres_per_px(&a->view);
    static const float NICE[] = { 1, 2, 5 };
    float best = 0;
    for (float dec = 1; dec < 1e7f && !best; dec *= 10) {
        for (int i = 0; i < 3; i++) {
            if (NICE[i] * dec / mpp >= 80.0f) {
                best = NICE[i] * dec;
                break;
            }
        }
    }
    if (best > 0 && !(L->land && a->sel >= 0 && L->scale_x + 200 > L->zout.x - BTN_R)) {
        char txt[16];
        int spx = (int)(best / mpp);
        if (best >= 1000) snprintf(txt, sizeof txt, "%d km", (int)(best / 1000));
        else snprintf(txt, sizeof txt, "%d m", (int)best);
        float x0 = (float)L->scale_x, yb = (float)L->scale_y;
        float bar[6] = { x0, yb - 8, x0, yb, x0 + spx, yb };
        float end[4] = { x0 + spx, yb, x0 + spx, yb - 8 };
        if (!(a->sel >= 0 && !L->land)) {
            vl_polyline(fb, bar, 3, 5.0f, VL_BG, 200);
            vl_polyline(fb, end, 2, 5.0f, VL_BG, 200);
            vl_polyline(fb, bar, 3, 2.2f, 0xD8DCE4, 255);
            vl_polyline(fb, end, 2, 2.2f, 0xD8DCE4, 255);
            vl_text(fb, fs, (int)x0 + spx + 8, (int)yb - lh + 4, txt, 0xD8DCE4, VL_BG, true);
        }
    }
    /* the attribution the licences ask for */
    char attr[96];
    snprintf(attr, sizeof attr, "© OpenStreetMap © OpenFreeMap · %s", vl_src_name(vl_src_now(a)));
    int aw = vl_text_width(fs, attr);
    int ax = L->land && a->sel >= 0 ? L->card_x + L->card_w + 24 : (a->sw - aw) / 2;
    vl_text(fb, fs, ax, L->attr_y, attr, 0x8A919E, VL_BG, true);
}

static void draw_overlay(app_t *a, vl_fb_t *fb)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    rings(a, fb);
    airports(a, fb);
    draw_aircraft(a, fb, now);
    draw_bar(a, fb);
    if (a->sel >= 0) card(a, fb, now);
}

/* ---------------------------------------------------------------------------
 * To the panel (Mapas')
 * ------------------------------------------------------------------------- */

static void compose(app_t *a, uint16_t *dst)
{
    cut_t c;
    bool have = choose(a, &c);
    if (have) {
        a->composing = (back_t *)c.b;
        __sync_synchronize();
        if (!choose(a, &c)) have = false;
        a->composing = have ? (back_t *)c.b : NULL;
    }
    cut_frame(a, have ? &c : NULL, dst);
    a->composing = NULL;
    vl_fb_t fb = { dst, a->sw, a->sh, 0, 0, a->sw, a->sh };
    vl_layout(a);
    draw_overlay(a, &fb);
}

enum { PRESENT_NONE = 0, PRESENT_LVGL, PRESENT_BLIT, PRESENT_FLIP_TURNED, PRESENT_FLIP };

static void present_log(app_t *a, int how)
{
    if (a->present == how) return;
    a->present = how;
    static const char *const WHAT[] = {
        "", "through LVGL's canvas", "blitted into the buffer on screen",
        "turned into the panel's free buffer by the PPA and flipped to",
        "cut straight into the panel's free buffer and flipped to",
    };
    aos_hal_log("vuelos", "frames: %s", WHAT[how]);
}

void vl_frame_push(app_t *a)
{
    bool may = a->use_blit && !a->over_bits && (a->blit_ok || a->blit_fails < 3);
    uint16_t *back = may ? aos_hal_display_back() : NULL;
    bool direct = back && aos_hal_display_get_rotation() == 0 && a->sw == AOS_PANEL_W && a->sh == AOS_PANEL_H;
    compose(a, direct ? back : a->frame);

    int how = PRESENT_LVGL;
    if (direct) {
        if (aos_hal_display_flip(back)) {
            how = PRESENT_FLIP;
            a->flipped = back;
            a->frame_synced = false;
        } else {
            compose(a, a->frame);
            back = NULL;
        }
    }
    if (how == PRESENT_LVGL) a->frame_synced = true;
    if (how == PRESENT_LVGL && may) {
        if (back && aos_hal_display_blit_into(back, 0, 0, a->sw, a->sh, a->frame) && aos_hal_display_flip(back)) {
            how = PRESENT_FLIP_TURNED;
            a->flipped = back;
        } else if (aos_hal_display_blit_scaled(0, 0, a->sw, a->sh, a->frame, 1, false)) {
            how = PRESENT_BLIT;
            a->flipped = NULL;
        } else if (++a->blit_fails == 3 && !a->blit_ok) {
            aos_hal_log("vuelos", "no direct blit here: the frames go through LVGL");
        }
    }
    if (how == PRESENT_LVGL) {
        a->flipped = NULL;
        lv_obj_invalidate(a->canvas);
    } else {
        a->blit_ok = true;
    }
    present_log(a, how);
    a->pushed_ms = (uint32_t)aos_hal_uptime_ms();
}

bool vl_frame_sync(app_t *a, uint32_t now)
{
    if (a->frame_synced) return false;
    bool touched = false;
    if (a->flipped && !a->over_bits && aos_hal_display_back_age(a->flipped, NULL, &touched) && touched) return true;
    if (now - a->pushed_ms > SYNC_MS) {
        compose(a, a->frame);
        a->frame_synced = true;
    }
    return false;
}
