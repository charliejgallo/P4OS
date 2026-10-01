/*
 * MONSTER HOP - the HUD (see mh_hud.h)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "mh_hud.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fminf/fmaxf are not in the firmware's symbol table */
static inline float mn(float a, float b) { return a < b ? a : b; }
static inline float mx(float a, float b) { return a > b ? a : b; }
static inline float ab(float a) { return a < 0 ? -a : a; }

#define GOLD    0xFFC83A
#define GOLD_D  0x6A5020
#define RED     0xFF4A5A
#define WHITE   0xFFFFFF

static void act_free(void);

void mh_hud_free(mh_hud_t *h)
{
    for (int i = 0; i < 12; i++) {
        free(h->dig[i].a);
        free(h->sdig[i].a);
    }
    for (int i = 0; i < MSG_N; i++) free(h->msg[i].a);
    free(h->title.a);
    memset(h, 0, sizeof(*h));
    act_free();
}

void mh_hud_events(mh_hud_state_t *s, uint32_t ev, float dt)
{
    if (s->msg >= 0) {
        s->msg_t += dt;
        if (s->msg_t > 1.8f) s->msg = -1;
    }
    if (s->key_flash > 0) s->key_flash -= dt;
    if (s->coin_flash > 0) s->coin_flash -= dt;
    int m = -1;
    if (ev & EV_KEY) { m = MSG_KEY; s->key_flash = 0.8f; }
    if (ev & EV_OPEN) m = MSG_OPEN;
    if (ev & EV_CHECK) m = MSG_CHECK;
    if (ev & EV_TIMEUP) m = MSG_TIMEUP;
    if (ev & EV_LIFE) m = MSG_LIFE;
    if (ev & EV_TIME) m = MSG_TIME;
    if (ev & EV_LOW_TIME) m = MSG_LOW;
    if (ev & (EV_COIN | EV_CHEST)) s->coin_flash = 0.4f;
    if (m >= 0) {
        s->msg = m;
        s->msg_t = 0;
    }
}

/* ---- little drawings ---- */

/* every size in screen pixels times the art's scale (2 with the desktop's
 * HD art); the masks of text come already scaled (monsterhop.c) */
#define P MH_PX

static void outline_text(mh_img_t *im, const mh_mask_t *m, int x, int y, uint16_t c, int alpha)
{
    uint16_t k = mh_hex(0x000000);
    int oa = alpha * 3 >> 2;
    mh_mask_draw(im, m, x - P, y, k, oa);
    mh_mask_draw(im, m, x + P, y, k, oa);
    mh_mask_draw(im, m, x, y - P, k, oa);
    mh_mask_draw(im, m, x, y + P, k, oa);
    mh_mask_draw(im, m, x, y + 2 * P, k, oa);
    mh_mask_draw(im, m, x, y, c, alpha);
}

/* a string of digits and ':' '/' with the digit masks; returns its width */
static int number_w(const mh_mask_t *d, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        int i = *s >= '0' && *s <= '9' ? *s - '0' : *s == ':' ? 10 : *s == '/' ? 11 : -1;
        if (i >= 0 && d[i].a) w += d[i].w - P;
    }
    return w;
}
static void number(mh_img_t *im, const mh_mask_t *d, const char *s, int x, int y, uint16_t c)
{
    for (; *s; s++) {
        int i = *s >= '0' && *s <= '9' ? *s - '0' : *s == ':' ? 10 : *s == '/' ? 11 : -1;
        if (i < 0 || !d[i].a) continue;
        outline_text(im, &d[i], x, y, c, 255);
        x += d[i].w - P;
    }
}

/* a key: a ring and a toothed shaft, 22 x 12 px */
/* who: 0 not taken, 1 taken here, 2 taken by the other watch */
static void key_icon(mh_img_t *im, int x, int y, int who, float glow)
{
    bool have = who != 0;
    uint16_t c = mh_hex(who == 2 ? 0x9AD8FF : have ? GOLD : GOLD_D);
    uint16_t k = mh_hex(0x1A1208);
    int a = 255;
    if (have && glow > 0) {
        mh_disc(im, (x + 11 * P) * 16, (y + 6 * P) * 16, (int)(16 * (9 + glow * 6) * P), mh_hex(0xFFE8A0),
                (int)(glow * 120));
    }
    mh_disc(im, (x + 5 * P) * 16, (y + 6 * P) * 16, 6 * 16 * P, k, a);
    mh_disc(im, (x + 5 * P) * 16, (y + 6 * P) * 16, 5 * 16 * P, c, a);
    mh_disc(im, (x + 5 * P) * 16, (y + 6 * P) * 16, 2 * 16 * P, k, a);
    mh_rect(im, x + 9 * P, y + 4 * P, 12 * P, 4 * P, k);
    mh_rect(im, x + 9 * P, y + 5 * P, 12 * P, 2 * P, c);
    mh_rect(im, x + 16 * P, y + 7 * P, 2 * P, 4 * P, c);
    mh_rect(im, x + 19 * P, y + 7 * P, 2 * P, 3 * P, c);
}

static void heart_icon(mh_img_t *im, int x, int y, uint16_t c)
{
    uint16_t k = mh_hex(0x200810);
    for (int pass = 0; pass < 2; pass++) {
        uint16_t col = pass ? c : k;
        int g = pass ? 0 : 1;
        mh_disc(im, (x + 4 * P) * 16, (y + 4 * P) * 16, (4 + g) * 16 * P, col, 255);
        mh_disc(im, (x + 10 * P) * 16, (y + 4 * P) * 16, (4 + g) * 16 * P, col, 255);
        for (int r = 0; r < (8 + g) * P; r++) {
            int half = (7 + g) * P - r;
            if (half < 0) break;
            mh_rect(im, x + 7 * P - half, y + 5 * P + r, 2 * half, 1, col);
        }
    }
}

static void coin_icon(mh_img_t *im, int x, int y, float flash)
{
    mh_disc(im, (x + 7 * P) * 16, (y + 7 * P) * 16, 8 * 16 * P, mh_hex(0x3A2808), 255);
    mh_disc(im, (x + 7 * P) * 16, (y + 7 * P) * 16, 7 * 16 * P, mh_hex(flash > 0 ? 0xFFF0A0 : GOLD), 255);
    mh_disc(im, (x + 7 * P) * 16, (y + 7 * P) * 16, 4 * 16 * P, mh_hex(0xE8A020), 255);
}

/* a filled triangle pointing along (dx, dy) with its tip at (x, y), len
 * long and 2 * half wide at the base, with a dark rim */
static void tri(mh_img_t *im, float x, float y, float dx, float dy, float len, float half, uint16_t c, int alpha,
                bool rim)
{
    float n = sqrtf(dx * dx + dy * dy);
    if (n < 1e-3f) return;
    dx /= n;
    dy /= n;
    float bx = x - dx * len, by = y - dy * len;
    float px = -dy, py = dx;
    float ax = bx + px * half, ay = by + py * half, cx = bx - px * half, cy = by - py * half;
    int y0 = (int)mn(mn(y, ay), cy) - 1, y1 = (int)mx(mx(y, ay), cy) + 1;
    int x0 = (int)mn(mn(x, ax), cx) - 1, x1 = (int)mx(mx(x, ax), cx) + 1;
    if (y0 < im->cy0) y0 = im->cy0;
    if (y1 >= im->cy1) y1 = im->cy1 - 1;
    if (x0 < im->cx0) x0 = im->cx0;
    if (x1 >= im->cx1) x1 = im->cx1 - 1;
    uint16_t k = mh_hex(0x000000);
    for (int yy = y0; yy <= y1; yy++) {
        uint16_t *row = im->px + (size_t)yy * im->w;
        for (int xx = x0; xx <= x1; xx++) {
            float qx = xx + 0.5f, qy = yy + 0.5f;
            /* inside the triangle (x,y) a c, with a soft 1.5 px rim */
            float e0 = (ax - x) * (qy - y) - (ay - y) * (qx - x);
            float e1 = (cx - ax) * (qy - ay) - (cy - ay) * (qx - ax);
            float e2 = (x - cx) * (qy - cy) - (y - cy) * (qx - cx);
            bool in = (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
            if (!in) continue;
            float d0 = ab(e0) / (2 * half), d1 = ab(e1) / (2 * half), d2 = ab(e2) / len;
            float d = mn(d0, mn(d1, d2));
            row[xx] = mh_blend(row[xx], rim && d < 2.0f * P ? k : c, alpha);
        }
    }
}

static void arrow(mh_img_t *im, float x, float y, float dx, float dy, uint16_t c, int alpha)
{
    tri(im, x, y, dx, dy, 22.0f * P, 12.0f * P, c, alpha, true);
}

/* ---- the on-screen controls (P4: touch only) ---- */

/* a ring from radius r - w to r, soft on both edges */
static void ring(mh_img_t *im, int cx, int cy, int r, int w, uint16_t c, int alpha)
{
    int y0 = cy - r - 1 < im->cy0 ? im->cy0 : cy - r - 1, y1 = cy + r + 1 >= im->cy1 ? im->cy1 - 1 : cy + r + 1;
    int x0 = cx - r - 1 < im->cx0 ? im->cx0 : cx - r - 1, x1 = cx + r + 1 >= im->cx1 ? im->cx1 - 1 : cx + r + 1;
    float mid = r - w * 0.5f, hw = w * 0.5f;
    for (int y = y0; y <= y1; y++) {
        uint16_t *row = im->px + (size_t)y * im->w;
        float fy = y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float fx = x + 0.5f - cx;
            float d = sqrtf(fx * fx + fy * fy);
            float cov = hw + 0.5f - ab(d - mid);
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            row[x] = mh_blend(row[x], c, (int)(alpha * cov));
        }
    }
}

/* a thick stroke from (x0, y0) to (x1, y1), stamped with discs */
static void stroke(mh_img_t *im, float x0, float y0, float x1, float y1, float r, uint16_t c)
{
    float dx = x1 - x0, dy = y1 - y0;
    int n = (int)(sqrtf(dx * dx + dy * dy) / (r * 0.5f)) + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        mh_disc(im, (int)((x0 + dx * t) * 16), (int)((y0 + dy * t) * 16), (int)(r * 16), c, 255);
    }
}

/* the grid's four ways on the screen, the projection's axes (+Y, +X, -Y, -X) */
static const float s_axis[4][2] = { { 0.4299f, -0.9029f }, { 0.9738f, 0.2272f }, { -0.4299f, 0.9029f },
                                    { -0.9738f, -0.2272f } };

/* the action: a gold ring with a double chevron, "up and over" */
static void draw_action(mh_img_t *im, int cx, int cy, int r, bool lit)
{
    uint16_t gold = mh_hex(GOLD), white = mh_hex(WHITE), dark = mh_hex(0x0C0814);
    mh_disc(im, cx * 16, cy * 16, r * 16, lit ? mh_hex(0x6A4A10) : dark, lit ? 200 : 120);
    ring(im, cx, cy, r, 5, gold, lit ? 255 : 210);
    uint16_t ic = lit ? mh_hex(0xFFF0B0) : white;
    float w = r * 0.36f, h = r * 0.2f, th = r * 0.075f;
    for (int k = 0; k < 2; k++) {
        float y = cy - r * 0.02f + (k ? r * 0.26f : -r * 0.1f);
        stroke(im, cx - w, y + h, cx, y - h, th, ic);
        stroke(im, cx, y - h, cx + w, y + h, th, ic);
    }
}

/* The action button, drawn once per look (size, lit) as colour + alpha and
 * then only blended: drawing it anew cost ~10 ms a frame on the P4 (a disc,
 * a ring and 56 stamped discs, a square root a pixel). It is made by drawing
 * it over black and over white: the difference is what shows through. Made
 * by mh_hud_prepare() on one core, read by both. */
static struct {
    int       r, side;
    bool      lit;
    uint16_t *c;
    uint8_t  *a;
} s_act;

static void act_free(void)
{
    free(s_act.c);
    free(s_act.a);
    memset(&s_act, 0, sizeof s_act);
}

void mh_hud_prepare(const mh_hud_state_t *s)
{
    if (!s->ctl) return;
    int r = s->act_r;
    if (s_act.c && s_act.r == r && s_act.lit == s->act_lit) return;
    int side = 2 * r + 8;
    free(s_act.c);
    free(s_act.a);
    s_act.c = NULL;
    s_act.a = NULL;
    uint16_t *b = (uint16_t *)malloc((size_t)side * side * 2), *wt = (uint16_t *)malloc((size_t)side * side * 2);
    s_act.c = (uint16_t *)malloc((size_t)side * side * 2);
    s_act.a = (uint8_t *)malloc((size_t)side * side);
    if (!b || !wt || !s_act.c || !s_act.a) {
        free(b); free(wt); free(s_act.c); free(s_act.a);
        s_act.c = NULL;
        s_act.a = NULL;
        return;
    }
    memset(b, 0, (size_t)side * side * 2);
    memset(wt, 0xFF, (size_t)side * side * 2);
    mh_img_t im;
    mh_img_init(&im, b, side, side);
    draw_action(&im, side / 2, side / 2, r, s->act_lit);
    mh_img_init(&im, wt, side, side);
    draw_action(&im, side / 2, side / 2, r, s->act_lit);
    for (int i = 0; i < side * side; i++) {
        int br, bg, bb, wr, wg, wb;
        mh_unpack(b[i], &br, &bg, &bb);
        mh_unpack(wt[i], &wr, &wg, &wb);
        int al = 255 - ((wr - br) + (wg - bg) + (wb - bb)) / 3;
        if (al < 0) al = 0;
        if (al > 255) al = 255;
        s_act.a[i] = (uint8_t)al;
        if (al < 4) { s_act.c[i] = 0; continue; }
        int rr = br * 255 / al, gg = bg * 255 / al, bl = bb * 255 / al;
        s_act.c[i] = mh_rgb(rr > 255 ? 255 : rr, gg > 255 ? 255 : gg, bl > 255 ? 255 : bl);
    }
    free(b);
    free(wt);
    s_act.r = r;
    s_act.side = side;
    s_act.lit = s->act_lit;
}

static void controls(mh_img_t *im, const mh_hud_state_t *s)
{
    uint16_t gold = mh_hex(GOLD), white = mh_hex(WHITE), dark = mh_hex(0x0C0814);
    int r = s->act_r, cx = s->act_x, cy = s->act_y;
    if (cy + r + 4 > im->cy0 && cy - r - 4 < im->cy1) {
        if (s_act.c && s_act.r == r && s_act.lit == s->act_lit) {
            int side = s_act.side, x0 = cx - side / 2, y0 = cy - side / 2;
            int ya = y0 < im->cy0 ? im->cy0 : y0, yb = y0 + side > im->cy1 ? im->cy1 : y0 + side;
            int xa = x0 < im->cx0 ? im->cx0 : x0, xb = x0 + side > im->cx1 ? im->cx1 : x0 + side;
            for (int y = ya; y < yb; y++) {
                uint16_t *row = im->px + (size_t)y * im->w;
                const uint16_t *sc = s_act.c + (size_t)(y - y0) * side - x0;
                const uint8_t *sa = s_act.a + (size_t)(y - y0) * side - x0;
                for (int x = xa; x < xb; x++)
                    if (sa[x]) row[x] = mh_blend(row[x], sc[x], sa[x]);
            }
        } else {
            draw_action(im, cx, cy, r, s->act_lit);
        }
    }
    /* the arrows: one per way of the grid, along its axis on the screen */
    if (s->pad) {
        int pr = s->pad_r, px = s->pad_x, py = s->pad_y;
        if (py + pr + 2 > im->cy0 && py - pr - 2 < im->cy1) {
            mh_disc(im, px * 16, py * 16, pr * 16, dark, 90);
            ring(im, px, py, pr, 3, white, 90);
            for (int d = 0; d < 4; d++) {
                float ax = s_axis[d][0], ay = s_axis[d][1];
                bool lit = s->pad_lit == d;
                tri(im, px + ax * pr * 0.9f, py + ay * pr * 0.9f, ax, ay, pr * 0.46f, pr * 0.3f,
                    lit ? gold : white, lit ? 240 : 150, false);
            }
        }
    }
}

void mh_hud_draw(const mh_hud_t *h, mh_img_t *im, const mh_game_t *g, const mh_hud_state_t *s,
                 const mh_world_t *w, int cam_x, int cam_y)
{
    /* the band: skip what does not touch it */
    int by0 = im->cy0, by1 = im->cy1;
    if (by0 < 64 * P) {
        /* a dark strip for legibility */
        for (int y = by0; y < by1 && y < 40 * P; y++) {
            uint16_t *row = im->px + (size_t)y * im->w;
            int k = 256 - (40 * P - y) * 4 / P;
            for (int x = im->cx0; x < im->cx1; x++) row[x] = mh_darken(row[x], k);
        }
        /* pause */
        if (s->pause_icon) {
            mh_rrect(im, 12 * P, 10 * P, 6 * P, 20 * P, 2 * P, mh_hex(0xFFFFFF), 200);
            mh_rrect(im, 22 * P, 10 * P, 6 * P, 20 * P, 2 * P, mh_hex(0xFFFFFF), 200);
        }
        /* keys */
        int kx = 44 * P;
        if (g->link) {
            /* a race: this watch's keys in gold first, the other's in blue */
            int mine = g->my_keys, theirs = g->keys - g->my_keys;
            for (int i = 0; i < MH_KEYS; i++)
                key_icon(im, kx + i * 25 * P, 8 * P, i < mine ? 1 : i < mine + theirs ? 2 : 0, i < mine ? s->key_flash : 0);
        } else {
            for (int i = 0; i < MH_KEYS; i++) key_icon(im, kx + i * 25 * P, 8 * P, i < g->keys, s->key_flash);
            /* lives */
            int lx = 44 * P;
            for (int i = 0; i < g->lives && i < 6; i++) heart_icon(im, lx + i * 17 * P, 26 * P, mh_hex(RED));
        }
        /* the clock, top right; the coins under it */
        char b[16];
        if (g->timer) {
            int t = (int)ceilf(g->time_left);
            if (t < 0) t = 0;
            snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
            int ww = number_w(h->dig, b);
            bool low = t <= 10 && ((int)(g->t * 4) & 1);
            number(im, h->dig, b, MH_W - 14 * P - ww, 4 * P, mh_hex(low ? 0xFF6060 : WHITE));
        }
        snprintf(b, sizeof b, "%d", g->coins);
        int cw = number_w(h->sdig, b);
        int cy = (g->timer ? 34 : 8) * P;
        coin_icon(im, MH_W - 36 * P - cw, cy + P, s->coin_flash);
        number(im, h->sdig, b, MH_W - 14 * P - cw, cy, mh_hex(s->coin_flash > 0 ? 0xFFF0A0 : WHITE));
    }
    /* the arrow towards the nearest key, when it is off the screen */
    float tx, ty;
    if (g->state == GS_PLAY && mh_game_target(g, &tx, &ty)) {
        int tz = 0;
        int ix = (int)tx, iy = (int)ty;
        if (mh_in(g->lv, ix, iy)) tz = mh_cell(g->lv, ix, iy)->h;
        float sx = mh_lpx(w, tx, ty) - cam_x, sy = mh_lpy(w, tx, ty, tz * MH_FLOOR_M) - cam_y - 20 * P;
        bool off = sx < 0 || sx > MH_W || sy < 50 * P || sy > MH_H;
        if (off) {
            float cx = MH_W / 2.0f, cy = MH_H / 2.0f + 20 * P;
            float dx = sx - cx, dy = sy - cy;
            /* clamp the ray to the screen, inset */
            float kx = dx > 0 ? (MH_W - 26 * P - cx) / dx : dx < 0 ? (26 * P - cx) / dx : 1e9f;
            float ky = dy > 0 ? (MH_H - 26 * P - cy) / dy : dy < 0 ? (70 * P - cy) / dy : 1e9f;
            float k = mn(kx, ky);
            float ex = cx + dx * k, ey = cy + dy * k;
            float pulse = 0.75f + 0.25f * sinf(g->t * 6.0f);
            if (ey + 24 * P > by0 && ey - 24 * P < by1)
                arrow(im, ex, ey, dx, dy, mh_hex(g->exit_open ? 0x6AF07A : GOLD), (int)(230 * pulse));
        }
    }
    /* the banner */
    if (s->msg >= 0 && s->msg < MSG_N && h->msg[s->msg].a) {
        const mh_mask_t *m = &h->msg[s->msg];
        float t = s->msg_t;
        int a = t < 0.15f ? (int)(t / 0.15f * 255) : t > 1.4f ? (int)((1.8f - t) / 0.4f * 255) : 255;
        if (a < 0) a = 0;
        int y = (120 - (int)(t < 0.15f ? (0.15f - t) * 80 : 0)) * P;
        if (y + m->h > by0 && y < by1) {
            uint32_t col = s->msg == MSG_TIMEUP || s->msg == MSG_LOW ? 0xFF7070 :
                           s->msg == MSG_OPEN ? 0x8AF59A : 0xFFE070;
            outline_text(im, m, (MH_W - m->w) / 2, y, mh_hex(col), a);
        }
    }
    if (s->show_title && h->title.a) {
        const mh_mask_t *m = &h->title;
        int y = 70 * P;
        if (y + m->h > by0 && y < by1) outline_text(im, m, (MH_W - m->w) / 2, y, mh_hex(0xFFFFFF), 255);
    }
    if (s->ctl) controls(im, s);
}
