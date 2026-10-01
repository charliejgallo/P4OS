/*
 * TURBO - the HUD (see tb_hud.h)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "tb_hud.h"
#include "tb_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Baking
 * -------------------------------------------------------------------------- */

bool tb_hud_bake(tb_sprite_t *out, const tb_mask_t *m, uint32_t top, uint32_t bottom, int outline)
{
    memset(out, 0, sizeof(*out));
    if (!m->a || m->w <= 0 || m->h <= 0) return false;
    int o = outline;
    int w = m->w + o * 2, h = m->h + o * 2;
    out->px = (uint16_t *)tb_malloc((size_t)w * h * 2);
    out->a = (uint8_t *)tb_calloc((size_t)w * h, 1);
    if (!out->px || !out->a) {
        free(out->px);
        free(out->a);
        out->px = NULL;
        out->a = NULL;
        return false;
    }
    out->w = (int16_t)w;
    out->h = (int16_t)h;
    out->ox = 0;
    out->oy = 0;
    /* the outline: the mask dilated by a disc of radius o */
    uint8_t *ol = (uint8_t *)tb_calloc((size_t)w * h, 1);
    if (ol && o > 0) {
        for (int y = 0; y < m->h; y++) {
            for (int x = 0; x < m->w; x++) {
                int a = m->a[y * m->w + x];
                if (a < 40) continue;
                for (int dy = -o; dy <= o; dy++) {
                    for (int dx = -o; dx <= o; dx++) {
                        if (dx * dx + dy * dy > o * o + o) continue;
                        uint8_t *p = &ol[(size_t)(y + o + dy) * w + x + o + dx];
                        if (*p < a) *p = (uint8_t)a;
                    }
                }
            }
        }
    }
    for (int y = 0; y < h; y++) {
        int t = h > 1 ? y * 256 / (h - 1) : 0;
        uint32_t c = tb_mix(top, bottom, t);
        int cr = (int)(c >> 16) & 255, cg = (int)(c >> 8) & 255, cb = (int)c & 255;
        for (int x = 0; x < w; x++) {
            int mx = x - o, my = y - o;
            int a = (mx >= 0 && my >= 0 && mx < m->w && my < m->h) ? m->a[my * m->w + mx] : 0;
            int oa = ol ? ol[(size_t)y * w + x] : 0;
            size_t i = (size_t)y * w + x;
            /* colour over the dark outline */
            int r = (cr * a + 10 * (255 - a)) / 255, g = (cg * a + 8 * (255 - a)) / 255, b = (cb * a + 14 * (255 - a)) / 255;
            out->px[i] = tb_rgb(r, g, b);
            out->a[i] = (uint8_t)(a > oa ? a : oa);
        }
    }
    free(ol);
    tb_sprite_runs(out);
    return true;
}

/* the pedals at twice the watch's size: brushed metal rim, a rubber (brake)
 * or aluminium (gas) face, opaque inside so only the rounded edge blends */
static void make_pedal(tb_sprite_t *s, bool gas, bool pressed)
{
    int w = gas ? 152 : 208, h = gas ? 200 : 160;
    s->w = (int16_t)w;
    s->h = (int16_t)h;
    s->ox = 0;
    s->oy = 0;
    s->px = (uint16_t *)tb_malloc((size_t)w * h * 2);
    s->a = (uint8_t *)tb_calloc((size_t)w * h, 1);
    if (!s->px || !s->a) return;
    int r = 28;
    int inset = pressed ? 6 : 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int xx = x - inset, yy = y - inset, ww = w - inset * 2, hh = h - inset * 2;
            float cov = 1.0f;
            if (xx < 0 || yy < 0 || xx >= ww || yy >= hh) continue;
            float qx = 0, qy = 0;
            if (xx < r) qx = (float)(r - xx) - 0.5f;
            else if (xx >= ww - r) qx = (float)(xx - (ww - r)) + 0.5f;
            if (yy < r) qy = (float)(r - yy) - 0.5f;
            else if (yy >= hh - r) qy = (float)(yy - (hh - r)) + 0.5f;
            if (qx > 0 && qy > 0) {
                cov = (float)r - sqrtf(qx * qx + qy * qy) + 0.5f;
                if (cov <= 0) continue;
                if (cov > 1) cov = 1;
            }
            int edge = xx < 10 || yy < 10 || xx >= ww - 10 || yy >= hh - 10;
            int v;
            if (edge) {
                v = 170 - yy * 60 / hh;
            } else if (gas) {
                /* aluminium with rows of oval holes */
                v = 150 + (xx * 20 / ww) - (yy * 30 / hh);
                int cx = (xx - 24) % 26, cy = (yy - 20) % 32;
                if (xx > 20 && xx < ww - 20 && yy > 16 && yy < hh - 16 && cx >= 0 && cx < 14 && cy >= 0 && cy < 20) {
                    /* a soft hole: darker in the middle */
                    v = 40;
                }
            } else {
                /* rubber with horizontal ridges */
                v = 60 + ((yy / 12) & 1 ? 26 : 0) - (yy * 20 / hh);
            }
            if (pressed) v = v * 3 / 4 + 30;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            size_t i = (size_t)y * w + x;
            s->px[i] = gas ? tb_rgb(v, v, v + 8 > 255 ? 255 : v + 8) : tb_rgb(v, v, v);
            s->a[i] = (uint8_t)(cov * 255.0f);
        }
    }
    tb_sprite_runs(s);
}

/* a rounded key with an arrow, see-through, brighter when pressed */
static void make_arrow(tb_sprite_t *s, bool right, bool pressed)
{
    int w = 150, h = 170, r = 30;
    s->w = (int16_t)w;
    s->h = (int16_t)h;
    s->ox = s->oy = 0;
    s->px = (uint16_t *)tb_malloc((size_t)w * h * 2);
    s->a = (uint8_t *)tb_calloc((size_t)w * h, 1);
    if (!s->px || !s->a) return;
    /* the triangle: tip at 70 % across, base at 30 % */
    float tx = right ? w * 0.70f : w * 0.30f, bx = right ? w * 0.34f : w * 0.66f;
    float ty = h * 0.5f, hb = h * 0.26f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float qx = 0, qy = 0, cov = 1.0f;
            if (x < r) qx = (float)(r - x) - 0.5f;
            else if (x >= w - r) qx = (float)(x - (w - r)) + 0.5f;
            if (y < r) qy = (float)(r - y) - 0.5f;
            else if (y >= h - r) qy = (float)(y - (h - r)) + 0.5f;
            if (qx > 0 && qy > 0) {
                cov = (float)r - sqrtf(qx * qx + qy * qy) + 0.5f;
                if (cov <= 0) continue;
                if (cov > 1) cov = 1;
            }
            /* inside the triangle: between the base and the tip, within the
             * two slanted edges (1 px of antialiasing) */
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float u = right ? (px - bx) / (tx - bx) : (bx - px) / (bx - tx);     /* 0 base .. 1 tip */
            float half = hb * (1.0f - u);
            float in = 0;
            if (u >= 0 && u <= 1) {
                float d = half - fabsf(py - ty);
                in = d > 1 ? 1 : (d < 0 ? 0 : d);
                float db = right ? px - bx : bx - px;
                if (db < 1) in *= db < 0 ? 0 : db;
            }
            int bg = pressed ? 110 : 40;
            int v = (int)((float)bg + (255.0f - (float)bg) * in);
            size_t i = (size_t)y * w + x;
            bool edge = x < 4 || y < 4 || x >= w - 4 || y >= h - 4;
            s->px[i] = pressed ? tb_rgb(v, v * 9 / 10, v * 6 / 10) : tb_rgb(v, v, v);
            if (edge && in == 0) s->px[i] = pressed ? tb_rgb(255, 180, 60) : tb_rgb(200, 204, 210);
            s->a[i] = (uint8_t)(cov * (in > 0 || edge ? 235.0f : (pressed ? 170.0f : 120.0f)));
        }
    }
}

/* the rim and the hub of a sports wheel (the spokes are drawn each frame,
 * turned): a leather rim shaded from above, a dark hub with a bright ring */
#define WHEEL_R     150
#define WHEEL_RIN   122
#define WHEEL_HUB   36
static void make_wheel(tb_sprite_t *s)
{
    int d = WHEEL_R * 2 + 2;
    s->w = s->h = (int16_t)d;
    s->ox = s->oy = (int16_t)(d / 2);
    s->px = (uint16_t *)tb_malloc((size_t)d * d * 2);
    s->a = (uint8_t *)tb_calloc((size_t)d * d, 1);
    if (!s->px || !s->a) return;
    float c = (float)d * 0.5f;
    for (int y = 0; y < d; y++) {
        for (int x = 0; x < d; x++) {
            float dx = (float)x + 0.5f - c, dy = (float)y + 0.5f - c;
            float rr = sqrtf(dx * dx + dy * dy);
            size_t i = (size_t)y * d + x;
            float co = (float)WHEEL_R - rr + 0.5f, ci = rr - (float)WHEEL_RIN + 0.5f;
            if (co > 0 && ci > 0) {
                float cov = co < ci ? co : ci;
                if (cov > 1) cov = 1;
                /* round in section: bright on the top of the tube */
                float t = (rr - (float)WHEEL_RIN) / (float)(WHEEL_R - WHEEL_RIN);     /* 0 in .. 1 out */
                float sec = 1.0f - fabsf(t - 0.45f) * 1.8f;
                float up = -dy / rr;                                                     /* 1 at the top */
                int v = 26 + (int)(sec * 34.0f) + (int)(up * 22.0f);
                if (v < 8) v = 8;
                s->px[i] = tb_rgb(v, v, v + 4);
                s->a[i] = (uint8_t)(cov * 255.0f);
            }
            float ch = (float)WHEEL_HUB - rr + 0.5f;
            if (ch > 0) {
                float cov = ch > 1 ? 1 : ch;
                int v = rr > WHEEL_HUB - 6 ? 150 - (int)(dy * 2.0f) : 44 - (int)(dy * 0.6f);
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                s->px[i] = tb_rgb(v, v, v + 6 > 255 ? 255 : v + 6);
                s->a[i] = (uint8_t)(cov * 255.0f);
            }
        }
    }
    tb_sprite_runs(s);
}

static void make_pause(tb_sprite_t *s)
{
    int d = 68;
    s->w = s->h = (int16_t)d;
    s->ox = s->oy = 0;
    s->px = (uint16_t *)tb_malloc((size_t)d * d * 2);
    s->a = (uint8_t *)tb_calloc((size_t)d * d, 1);
    if (!s->px || !s->a) return;
    float c = (d - 1) * 0.5f;
    for (int y = 0; y < d; y++) {
        for (int x = 0; x < d; x++) {
            float dx = x - c, dy = y - c;
            float cov = (float)d * 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            bool bar = (y >= 20 && y < 48) && ((x >= 22 && x < 30) || (x >= 38 && x < 46));
            size_t i = (size_t)y * d + x;
            s->px[i] = bar ? tb_rgb(255, 255, 255) : tb_rgb(0, 0, 0);
            s->a[i] = (uint8_t)(cov * (bar ? 255.0f : 120.0f));
        }
    }
}

void tb_hud_make_controls(tb_hud_t *h)
{
    make_pedal(&h->pedal[0][0], false, false);
    make_pedal(&h->pedal[0][1], false, true);
    make_pedal(&h->pedal[1][0], true, false);
    make_pedal(&h->pedal[1][1], true, true);
    make_arrow(&h->arrow[0][0], false, false);
    make_arrow(&h->arrow[0][1], false, true);
    make_arrow(&h->arrow[1][0], true, false);
    make_arrow(&h->arrow[1][1], true, true);
    make_wheel(&h->wheel);
    make_pause(&h->pause);
}

static void spr_free(tb_sprite_t *s)
{
    tb_sprite_free(s);
}

void tb_hud_free(tb_hud_t *h)
{
    for (int i = 0; i < TB_GLYPHS; i++) {
        spr_free(&h->big[i]);
        spr_free(&h->mid[i]);
        spr_free(&h->sml[i]);
    }
    for (int i = 0; i < TX_N; i++) spr_free(&h->word[i]);
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            spr_free(&h->pedal[i][j]);
            spr_free(&h->arrow[i][j]);
        }
    }
    spr_free(&h->wheel);
    spr_free(&h->pause);
    h->ok = false;
}

static uint32_t sb(const tb_sprite_t *s)
{
    uint32_t n = (uint32_t)(s->w > 0 ? s->w : 0) * (uint32_t)(s->h > 0 ? s->h : 0);
    return (s->px ? n * 2 : 0) + (s->a ? n : 0) + (s->run ? (uint32_t)s->h * 8 : 0);
}

uint32_t tb_hud_bytes(const tb_hud_t *h)
{
    uint32_t n = 0;
    for (int i = 0; i < TB_GLYPHS; i++) n += sb(&h->big[i]) + sb(&h->mid[i]) + sb(&h->sml[i]);
    for (int i = 0; i < TX_N; i++) n += sb(&h->word[i]);
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) n += sb(&h->pedal[i][j]) + sb(&h->arrow[i][j]);
    }
    return n + sb(&h->wheel) + sb(&h->pause);
}

static tb_rect_t rc(int x0, int y0, int x1, int y1)
{
    tb_rect_t r = { (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1 };
    return r;
}

void tb_hud_layout(tb_hud_t *h, int w, int hg, int car_y)
{
    tb_hud_lay_t *l = &h->lay;
    bool land = w > hg;
    l->w = w;
    l->h = hg;
    l->pause_x = 16;
    l->pause_y = 16;
    l->pause = rc(0, 0, 130, 130);
    l->word_y = land ? 6 : 16;
    l->clock_y = land ? 52 : 70;
    l->elapsed_y = 18;
    l->bar_y = land ? 170 : 204;
    l->bar_x0 = w / 2 - (land ? 220 : 200);
    l->bar_x1 = w / 2 + (land ? 220 : 200);
    l->speed_x = w - 110;
    l->speed_y = 70;
    l->banner_y = land ? 196 : 330;
    int wr = WHEEL_R;
    if (land) {
        l->wheel_cx = 36 + wr;
        l->wheel_cy = hg - 30 - wr;
        l->steer = rc(0, hg - 420, 440, hg);
        l->left = rc(30, hg - 200, 180, hg - 30);
        l->right = rc(200, hg - 200, 350, hg - 30);
        l->gas_x = w - 24 - 152;
        l->gas_y = hg - 24 - 200;
        l->brake_x = l->gas_x - 24 - 208;
        l->brake_y = hg - 24 - 160;
        l->brake = rc(l->brake_x - 40, hg - 420, l->gas_x - 12, hg);
        l->gas = rc(l->gas_x - 12, hg - 420, w, hg);
    } else {
        (void)car_y;
        l->wheel_cx = 24 + wr;
        l->wheel_cy = hg - 24 - wr;
        l->steer = rc(0, hg - 440, 368, hg);
        l->left = rc(20, hg - 200, 170, hg - 30);
        l->right = rc(190, hg - 200, 340, hg - 30);
        l->gas_x = w - 16 - 152;
        l->gas_y = hg - 24 - 200;
        l->brake_x = l->gas_x - 16 - 208;
        l->brake_y = hg - 24 - 160;
        l->brake = rc(368, hg - 440, l->gas_x - 8, hg);
        l->gas = rc(l->gas_x - 8, hg - 440, w, hg);
    }
}

/* --------------------------------------------------------------------------
 * Drawing
 * -------------------------------------------------------------------------- */

static int glyph(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    switch (c) {
    case ':': return 10;
    case '.': return 11;
    case '+': return 12;
    case '-': return 13;
    default: return -1;
    }
}

/* the glyphs overlap by their outline */
static int text_w(const tb_sprite_t *set, const char *s, int kern)
{
    int w = 0;
    for (; *s; s++) {
        int g = glyph(*s);
        if (g >= 0 && set[g].px) w += set[g].w - kern;
    }
    return w + kern;
}

static void text_draw(tb_img_t *im, const tb_sprite_t *set, const char *s, int x, int y, int kern)
{
    for (; *s; s++) {
        int g = glyph(*s);
        if (g < 0 || !set[g].px) continue;
        tb_sprite(im, &set[g], x, y);
        x += set[g].w - kern;
    }
}

static void text_center(tb_img_t *im, const tb_sprite_t *set, const char *s, int cx, int y, int kern)
{
    text_draw(im, set, s, cx - text_w(set, s, kern) / 2, y, kern);
}

/* nearest, integer scale: the countdown */
static void sprite_x2(tb_img_t *im, const tb_sprite_t *s, int x, int y)
{
    for (int yy = 0; yy < s->h * 2; yy++) {
        int dy = y + yy;
        if (dy < im->cy0 || dy >= im->cy1) continue;
        uint16_t *d = tb_row(im, dy);
        const uint16_t *sp = s->px + (size_t)(yy >> 1) * s->w;
        const uint8_t *sa = s->a + (size_t)(yy >> 1) * s->w;
        for (int xx = 0; xx < s->w * 2; xx++) {
            int dx = x + xx;
            if (dx < im->cx0 || dx >= im->cx1) continue;
            int a = sa[xx >> 1];
            if (a) d[dx] = a >= 250 ? sp[xx >> 1] : tb_blend(d[dx], sp[xx >> 1], a);
        }
    }
}

static void word_center(tb_img_t *im, const tb_hud_t *h, int w, int cx, int y)
{
    const tb_sprite_t *s = &h->word[w];
    if (s->px) tb_sprite(im, s, cx - s->w / 2, y);
}

static void fmt_time(char *b, int n, float t, bool tenths)
{
    if (t < 0) t = 0;
    int ds = (int)(t * 10.0f);
    int m = ds / 600, sec = (ds / 10) % 60, d = ds % 10;
    if (tenths) snprintf(b, (size_t)n, "%d:%02d.%d", m, sec, d);
    else snprintf(b, (size_t)n, "%d:%02d", m, sec);
}

void tb_hud_events(tb_hud_state_t *st, const tb_game_t *g, uint32_t ev, float dt)
{
    if (st->banner_t > 0) {
        st->banner_t -= dt;
        if (st->banner_t <= 0) {
            st->banner = -1;
            st->split_show = false;
        }
    }
    if (ev & EV_GO) {
        st->banner = TX_GO;
        st->banner_t = 1.0f;
    }
    if (ev & EV_CHECKPOINT) {
        st->banner = TX_EXTRA;
        st->banner_t = 2.2f;
        st->added = g->cp_added;
    }
    if (ev & EV_FINISH) {
        st->banner = TX_FINISH;
        st->banner_t = 99.0f;
    }
    if (ev & EV_TIMEUP) {
        st->banner = TX_TIMEUP;
        st->banner_t = 99.0f;
    }
    st->low_time = g->state == RS_RACING && g->time_left < 10.0f;
}

void tb_hud_prepare(tb_hud_state_t *st, const tb_game_t *g)
{
    st->blink = ((int)(g->elapsed * 4.0f) & 1) != 0;
    int secs = (int)ceilf(g->time_left);
    snprintf(st->t_clock, sizeof st->t_clock, "%d", secs < 0 ? 0 : secs);
    fmt_time(st->t_elapsed, sizeof st->t_elapsed, g->elapsed, true);
    snprintf(st->t_speed, sizeof st->t_speed, "%d", tb_game_kmh(g));
    snprintf(st->t_gear, sizeof st->t_gear, "%d", g->gear);
    snprintf(st->t_extra, sizeof st->t_extra, "+%d", (int)(st->added + 0.5f));
    fmt_time(st->t_final, sizeof st->t_final, g->elapsed, true);
    st->count = 0;
    if (g->state == RS_COUNTDOWN) {
        int n = 3 - (int)g->t_state;
        if (n >= 1 && n <= 3) st->count = n;
    }
    st->progress = tb_game_progress(g);
}

static bool over(const tb_img_t *im, int x0, int y0, int x1, int y1)
{
    return x1 > im->cx0 && x0 < im->cx1 && y1 > im->cy0 && y0 < im->cy1;
}

/* the wheel: three spokes turned by the steering, the rim over them, and a
 * mark at the top of the rim so the turn reads at a glance */
static void draw_wheel(const tb_hud_t *h, tb_img_t *im, int cx, int cy, float steer)
{
    if (!over(im, cx - WHEEL_R - 2, cy - WHEEL_R - 2, cx + WHEEL_R + 2, cy + WHEEL_R + 2)) return;
    float th = steer * 1.9f;            /* +-110 degrees at full lock */
    float c = cosf(th), s = sinf(th);
    static const float base[3][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 } };
    uint16_t sp = tb_rgb(78, 84, 92);
    float rin = (float)WHEEL_RIN + 4.0f;
    for (int k = 0; k < 3; k++) {
        float ux = base[k][0] * c - base[k][1] * s, uy = base[k][0] * s + base[k][1] * c;
        float wdt = k == 2 ? 16.0f : 11.0f;
        tb_capsule(im, (float)cx + ux * 20.0f, (float)cy + uy * 20.0f, (float)cx + ux * rin, (float)cy + uy * rin,
                   wdt, sp, 235);
    }
    if (h->wheel.px) tb_sprite(im, &h->wheel, cx, cy);
    /* the mark at twelve o'clock */
    float mx = s, my = -c;
    float r0 = (float)WHEEL_RIN + 2.0f, r1 = (float)WHEEL_R - 2.0f;
    tb_capsule(im, (float)cx + mx * r0, (float)cy + my * r0, (float)cx + mx * r1, (float)cy + my * r1, 6.0f,
               tb_rgb(255, 138, 30), 255);
}

void tb_hud_draw(const tb_hud_t *h, tb_img_t *im, const tb_game_t *g, const tb_hud_state_t *st)
{
    const tb_hud_lay_t *l = &h->lay;
    bool blink = st->blink;
    int cx = l->w / 2;

    /* the top: the clock, the elapsed time, the progress, the speed, the pause */
    if (im->cy0 < l->bar_y + 20) {
        word_center(im, h, TX_TIME, cx, l->word_y);
        if (!(st->low_time && blink)) text_center(im, h->big, st->t_clock, cx, l->clock_y, 6);
        int ew = text_w(h->sml, st->t_elapsed, 3);
        text_draw(im, h->sml, st->t_elapsed, l->w - 20 - ew, l->elapsed_y, 3);
        int bx0 = l->bar_x0, bx1 = l->bar_x1, by = l->bar_y;
        if (over(im, bx0 - 12, by - 12, bx1 + 12, by + 20)) {
            tb_rect_blend(im, bx0, by, bx1 - bx0, 8, tb_rgb(0, 0, 0), 150);
            const tb_track_t *t = g->trk;
            float fin = (float)t->cp_seg[t->ncp - 1];
            for (int i = 0; i < t->ncp; i++) {
                int x = bx0 + (int)((float)(bx1 - bx0) * (float)t->cp_seg[i] / fin);
                bool passed = i < g->next_cp;
                tb_rect(im, x - 2, by - 6, 5, 20, passed ? tb_rgb(255, 210, 40) : tb_rgb(200, 200, 200));
            }
            int px = bx0 + (int)((float)(bx1 - bx0) * st->progress);
            tb_rect(im, bx0, by, px - bx0, 8, tb_rgb(255, 180, 30));
            if (st->rival) {
                int rx = bx0 + (int)((float)(bx1 - bx0) * st->rival_prog);
                tb_disc(im, rx * 16 + 8, (by + 4) * 16, 10 * 16, tb_rgb(60, 200, 255), 255);
            }
            tb_disc(im, px * 16 + 8, (by + 4) * 16, 10 * 16, tb_rgb(255, 255, 255), 255);
        }
        /* the speed, the gear and the revs, top right */
        int sx = l->speed_x, sy = l->speed_y;
        if (over(im, sx - 170, sy, sx + 110, sy + 110)) {
            text_center(im, h->mid, st->t_speed, sx, sy, 4);
            const tb_sprite_t *kw = &h->word[TX_KMH];
            int ky = sy + (h->mid[0].h ? h->mid[0].h : 70) - 6;
            if (kw->px) tb_sprite(im, kw, sx - kw->w / 2, ky);
            int rw = (int)(g->rpm * 180.0f);
            int ry = ky + (kw->h ? kw->h : 24) + 2;
            tb_rect_blend(im, sx - 90, ry, 180, 6, tb_rgb(0, 0, 0), 140);
            tb_rect(im, sx - 90, ry, rw, 6, g->rpm > 0.9f ? tb_rgb(255, 60, 40) : tb_rgb(120, 220, 255));
            text_draw(im, h->sml, st->t_gear, sx - 90 - 10 - (h->sml[0].w ? h->sml[0].w : 24), sy + 16, 3);
        }
        if (h->pause.px) tb_sprite(im, &h->pause, l->pause_x, l->pause_y);
    }

    /* the banners, the middle */
    int by = l->banner_y;
    if (im->cy1 > by - 10 && im->cy0 < by + 230) {
        if (st->count) {
            const tb_sprite_t *s = &h->big[st->count];
            if (s->px) sprite_x2(im, s, cx - s->w, by - 20);
        } else if (st->banner >= 0) {
            const tb_sprite_t *w = &h->word[st->banner];
            word_center(im, h, st->banner, cx, by);
            int ny = by + (w->h ? w->h : 70) + 4;
            if (st->banner == TX_EXTRA) text_center(im, h->big, st->t_extra, cx, ny, 6);
            if (st->banner == TX_FINISH || st->banner == TX_TIMEUP) text_center(im, h->mid, st->t_final, cx, ny, 4);
        } else if (st->low_time && blink) {
            word_center(im, h, TX_HURRY, cx, by);
        }
    }

    if (!st->controls) return;
    /* the bottom: the steering and the pedals */
    if (st->ctl == CTL_WHEEL) {
        draw_wheel(h, im, l->wheel_cx, l->wheel_cy, st->steer);
    } else {
        const tb_sprite_t *al = &h->arrow[0][st->left ? 1 : 0], *ar = &h->arrow[1][st->right ? 1 : 0];
        if (al->px) tb_sprite(im, al, l->left.x0, l->left.y0);
        if (ar->px) tb_sprite(im, ar, l->right.x0, l->right.y0);
    }
    const tb_sprite_t *pb = &h->pedal[0][st->brake ? 1 : 0];
    const tb_sprite_t *pg = &h->pedal[1][st->gas ? 1 : 0];
    int push = 4;
    if (pb->px) tb_sprite(im, pb, l->brake_x, l->brake_y + (st->brake ? push : 0));
    if (pg->px) tb_sprite(im, pg, l->gas_x, l->gas_y + (st->gas ? push : 0));
}
