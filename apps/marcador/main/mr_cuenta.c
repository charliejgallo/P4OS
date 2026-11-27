/*
 * Marcador - hand tally counters.
 *
 * The little chrome counter that clicks once each time the lever is pushed:
 * a round steel body with a finger ring under it, four number wheels behind
 * a window, the plunger on top and a knurled knob on the side that turns
 * the wheels back to nought. One to four of them on a leather desk pad,
 * each under its own lamp, each with its owner's name on a strip of
 * embossed label tape.
 *
 * The body is drawn as polished metal: every pixel's normal reflects a
 * studio (a bright softbox above, a dark band at the horizon, the floor
 * below), so the rim gets the hard bands chrome has and the slightly domed
 * face a soft fall from light to dark. The wheels are real moving strips:
 * each is a column of the digits 0..9 and 0 again, seen through a clip the
 * size of the window, under a shading that rounds them into cylinders.
 * Every wheel follows its number on a spring, so a click snaps the units
 * round with a little overshoot and a carry kicks the tens a moment later,
 * the way the pawl drags them in the real thing.
 *
 * Fingers: press the counter (anywhere on it) and it counts, as the real
 * one counts on the way down; the plunger rides the finger. Hold the knob
 * to reset: the wheels whirr forward to nought. The small minus under the
 * label takes one back. Each counter answers its own finger.
 */
#include "mr_app.h"

#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define NW       4          /* wheels                                        */
#define KNOB_FR  4
#define RESET_MS 650

typedef struct {
    float x, y;             /* the body's centre                            */
    lv_obj_t *body, *plunger, *knob, *over, *tag, *tagl;
    lv_obj_t *clip[NW], *strip[NW];
    float pos[NW], vel[NW], base[NW], tgt[NW];
    float delay[NW];        /* the carry waits a moment                     */
    float press;            /* 0 up .. 1 down                               */
    bool  down;
    int   finger;
    float knob_t;           /* the knob's turn, in frames                   */
    bool  spinning;
} ctr_t;

typedef struct {
    mr_cv_t  bg;
    mr_img_t body, plunger, knob[KNOB_FR], strip, over;
    float    D;
    int      wx, wy, ww, wh;    /* the window, from the body's top left      */
    int      cw;                /* one wheel                                 */
    int      bw, bh;            /* the body image                            */
    int      n;
    ctr_t    c[4];
    struct {
        int      who;
        bool     knob;
        uint32_t t0;
        bool     fired;
    } f[MR_FINGERS];
} cnt_t;

#define ST ((cnt_t *)s->st)

static void mr_cuenta_minus(mr_scene_t *s, int who);

/* --- metal --------------------------------------------------------------------- */

/* what a polished surface with normal (nx, ny, nz) shows of the studio */
static float studio(float nx, float ny, float nz)
{
    float ry = 2.0f * nz * ny, rx = 2.0f * nz * nx;
    float v;
    if (ry < -0.55f) v = 0.92f - 0.25f * mr_smooth(-0.55f, -1.0f, ry);
    else if (ry < -0.1f) v = 0.55f + 0.37f * mr_smooth(-0.1f, -0.55f, ry);
    else if (ry < 0.18f) v = 0.20f + 0.35f * mr_smooth(0.18f, -0.1f, ry);
    else v = 0.32f + 0.12f * mr_smooth(0.18f, 0.8f, ry);
    v += 0.30f * expf(-((rx - 0.55f) * (rx - 0.55f)) / 0.02f) * (ry < 0.2f ? 1.0f : 0.4f);
    v += 0.12f * expf(-((rx + 0.6f) * (rx + 0.6f)) / 0.05f);
    return v;
}

static uint32_t chrome(float v)
{
    return mr_rgb_mix(0x1A1D22, 0xF4F8FC, mr_clampf(v, 0, 1.1f));
}

static void body_draw(cnt_t *C)
{
    mr_img_t *im = &C->body;
    float D = C->D, R = D * 0.5f;
    float cx = im->w * 0.5f, cy = R + 10;
    /* the finger ring, under the body */
    float ry = cy + R * 1.08f, ro = R * 0.40f, ri = R * 0.25f;
    for (int y = 0; y < im->h; y++) {
        for (int x = 0; x < im->w; x++) {
            float px = x + 0.5f - cx, py = y + 0.5f - ry;
            float d = sqrtf(px * px + py * py);
            float mid = (ro + ri) * 0.5f, tr = (ro - ri) * 0.5f;
            float t = (d - mid) / tr;
            if (fabsf(t) > 1.05f) continue;
            float cov = mr_clampf((1.0f - fabsf(t)) * tr, 0, 1);
            float s = t, nz = sqrtf(mr_clampf(1 - s * s, 0, 1));
            float nx = px / (d + 1e-3f) * s, ny = py / (d + 1e-3f) * s;
            mr_img_over(im, x, y, chrome(studio(nx, ny, nz) * 0.9f), cov);
        }
    }
    /* the neck between ring and body */
    mr_img_rrect(im, cx - R * 0.16f, cy + R * 0.7f, R * 0.32f, R * 0.30f, 4, 0x8E959E, 1.0f);
    /* the body */
    for (int y = 0; y < im->h; y++) {
        for (int x = 0; x < im->w; x++) {
            float px = x + 0.5f - cx, py = y + 0.5f - cy;
            float r = sqrtf(px * px + py * py);
            if (r > R + 1) continue;
            float cov = mr_clampf(R + 0.5f - r, 0, 1);
            float u = r / R;
            uint32_t c;
            if (u > 0.86f) {
                /* the rounded rim */
                float t = (u - 0.86f) / 0.14f, th = t * 1.5707963f;
                float s = sinf(th), cz = cosf(th);
                float nx = px / (r + 1e-3f) * s, ny = py / (r + 1e-3f) * s;
                c = chrome(studio(nx, ny, cz));
            } else {
                /* the face: a very shallow dome, brushed round the centre */
                float nx = px / R * 0.22f, ny = py / R * 0.22f, nz = sqrtf(1 - nx * nx - ny * ny);
                float v = studio(nx, ny, nz) * 0.55f + 0.40f - 0.10f * (py / R);
                float ang = atan2f(py, px);
                float brush = (mr_hashf((int)(ang * 900.0f), (int)(r * 0.5f), 77) - 0.5f) * 0.06f +
                              0.03f * sinf(r * 1.7f);
                c = chrome(v + brush);
                /* a groove where the face meets the rim */
                if (u > 0.83f) c = mr_rgb_scale(c, 0.55f);
            }
            mr_img_over(im, x, y, c, cov);
        }
    }
    /* the window's recess */
    float wx = C->wx, wy = C->wy, ww = C->ww, wh = C->wh;
    mr_img_rrect(im, wx - 8, wy - 8, ww + 16, wh + 16, 10, 0x5A6068, 1.0f);
    mr_img_rrect(im, wx - 6, wy - 6, ww + 12, wh + 12, 9, 0xC8CED6, 1.0f);
    mr_img_rrect(im, wx - 3, wy - 3, ww + 6, wh + 6, 6, 0x24272C, 1.0f);
    mr_img_rrect(im, wx, wy, ww, wh, 4, 0x0B0C0E, 1.0f);
    /* rivets at the sides of the window */
    for (int k = 0; k < 2; k++) {
        float rx2 = k ? wx + ww + R * 0.16f : wx - R * 0.16f, ry2 = wy + wh * 0.5f;
        mr_img_disc(im, rx2, ry2, R * 0.055f, 0x50565E, 1.0f);
        mr_img_disc(im, rx2 - 1, ry2 - 1, R * 0.042f, 0xD9DEE4, 1.0f);
        mr_img_disc(im, rx2 - R * 0.015f, ry2 - R * 0.015f, R * 0.016f, 0xFFFFFF, 0.8f);
    }
    /* a stamped ring of "graduations" under the window, like the maker's mark */
    for (int k = 0; k < 24; k++) {
        float a = 3.14159f * (0.18f + 0.64f * (float)k / 23.0f);
        float x0 = cx + cosf(a) * R * 0.62f, y0 = cy + sinf(a) * R * 0.62f;
        float x1 = cx + cosf(a) * R * (k % 4 ? 0.68f : 0.71f), y1 = cy + sinf(a) * R * (k % 4 ? 0.68f : 0.71f);
        mr_img_capsule(im, x0, y0, x1, y1, 1.0f, 0x3A3F46, 0.55f);
    }
    mr_img_shadow(im, 6, 12, 10, 0.55f);
}

static void plunger_draw(cnt_t *C)
{
    mr_img_t *im = &C->plunger;
    float D = C->D;
    float cx = im->w * 0.5f;
    float sw = D * 0.13f, cw = D * 0.30f, chh = D * 0.10f;
    /* the stem: a polished rod, lit across */
    for (int y = (int)chh; y < im->h; y++) {
        for (int x = 0; x < im->w; x++) {
            float u = (x + 0.5f - cx) / (sw * 0.5f);
            if (fabsf(u) > 1.02f) continue;
            float nz = sqrtf(mr_clampf(1 - u * u, 0, 1));
            mr_img_over(im, x, y, chrome(studio(u, 0, nz) * 0.95f), mr_clampf((1.02f - fabsf(u)) * sw * 0.5f, 0, 1));
        }
    }
    /* the cap: a short cylinder with a domed top */
    for (int y = 0; y < (int)(chh + 4); y++) {
        for (int x = 0; x < im->w; x++) {
            float u = (x + 0.5f - cx) / (cw * 0.5f);
            if (fabsf(u) > 1.02f) continue;
            float top = chh * 0.30f * (1.0f - u * u);
            float yy = y + 0.5f;
            float edge = mr_clampf((1.02f - fabsf(u)) * cw * 0.5f, 0, 1);
            float nz = sqrtf(mr_clampf(1 - u * u, 0, 1));
            if (yy < chh * 0.32f - top + chh * 0.02f) continue;
            float v;
            if (yy < chh * 0.36f) v = studio(u * 0.3f, -0.8f, 0.6f);   /* the top catches the softbox */
            else v = studio(u, 0.05f, nz);
            uint32_t c = chrome(v);
            if (yy > chh - 2) c = mr_rgb_scale(c, 0.6f);
            mr_img_over(im, x, y, c, edge);
        }
    }
}

static void knob_draw(cnt_t *C, mr_img_t *im, int phase)
{
    float w = im->w, h = im->h;
    for (int y = 0; y < (int)h; y++) {
        for (int x = 0; x < (int)w; x++) {
            /* a cylinder lying on its side, axis horizontal */
            float v = (y + 0.5f - h * 0.5f) / (h * 0.42f);
            float xx = x + 0.5f;
            if (fabsf(v) > 1.02f || xx > w - 2) continue;
            float cov = mr_clampf((1.02f - fabsf(v)) * h * 0.42f, 0, 1);
            float nz = sqrtf(mr_clampf(1 - v * v, 0, 1));
            float l = studio(0.1f, v, nz) * 0.85f;
            /* knurling: ridges round the cylinder, moving as it turns */
            float ang = asinf(mr_clampf(v, -1, 1)) + (float)phase * 0.39f;
            float ridge = 0.5f + 0.5f * sinf(ang * 14.0f);
            l *= 0.70f + 0.45f * ridge;
            if (xx < 5) l *= 0.6f;      /* the shaft side, in the body's shade */
            mr_img_over(im, x, y, chrome(l), cov);
        }
    }
    (void)C;
}

static void strip_draw(cnt_t *C)
{
    mr_img_t *im = &C->strip;
    int cw = C->cw, ch = C->wh;
    for (int i = 0; i < im->w * im->h; i++) im->px[i] = 0xFF17181B;
    for (int d = 0; d <= 10; d++) {
        float gh = ch * 0.64f, gw = gh * 0.52f;
        float x = (cw - gw) * 0.5f, y = d * ch + (ch - gh) * 0.5f;
        mr_glyph_img(im, '0' + d % 10, x, y, gw, gh, gh * 0.12f, 0xF1F0EA);
    }
    /* the edges of a wheel, darker where the next one is */
    for (int y = 0; y < im->h; y++) {
        for (int x = 0; x < 3; x++) {
            mr_img_over(im, x, y, 0x000000, 0.5f - x * 0.15f);
            mr_img_over(im, cw - 1 - x, y, 0x000000, 0.5f - x * 0.15f);
        }
    }
}

static void over_draw(cnt_t *C)
{
    mr_img_t *im = &C->over;
    int w = im->w, h = im->h;
    for (int y = 0; y < h; y++) {
        float v = (y + 0.5f) / h * 2.0f - 1.0f;
        /* round wheels: dark top and bottom */
        float a = powf(fabsf(v), 2.2f) * 0.92f;
        for (int x = 0; x < w; x++) mr_img_over(im, x, y, 0x000000, a);
    }
    /* the window's plastic: a slanted reflection */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float t = (x * 0.35f + y) / (float)h;
            float a = 0.18f * expf(-((t - 0.32f) * (t - 0.32f)) / 0.006f) + 0.06f * (1.0f - (float)y / h);
            mr_img_over(im, x, y, 0xFFFFFF, a);
        }
    }
    /* the dividers between wheels */
    for (int k = 1; k < NW; k++) {
        int x = k * C->cw;
        for (int y = 0; y < h; y++) {
            mr_img_over(im, x - 1, y, 0x000000, 0.7f);
            mr_img_over(im, x, y, 0x000000, 0.7f);
        }
    }
}

static void desk_draw(mr_scene_t *s)
{
    cnt_t *C = ST;
    mr_cv_t *cv = &C->bg;
    mr_field_t f;
    mr_field_new(&f, cv->w, cv->h, 4, 1.0f / 6.0f, 2, 41);
    for (int y = 0; y < cv->h; y++) {
        for (int x = 0; x < cv->w; x++) {
            float n = mr_field_at(&f, x, y);
            /* pebbled leather: the little hills catch light at their tops */
            float pebble = mr_smooth(0.35f, 0.7f, n);
            float k = 0.78f + 0.20f * pebble + (mr_hashf(x, y, 5) - 0.5f) * 0.05f;
            float light = 0.0f;
            for (int i = 0; i < C->n; i++) {
                float dx = (x - C->c[i].x) / (C->D * 1.1f), dy = (y - C->c[i].y) / (C->D * 1.2f);
                light += expf(-(dx * dx + dy * dy) * 1.3f);
            }
            k *= 0.42f + 0.62f * mr_clampf(light, 0, 1.2f);
            cv->px[y * cv->w + x] = mr_565(mr_rgb_scale(0x4A3426, k));
        }
    }
    mr_field_free(&f);
    /* the pad's stitched edge */
    int in = 26;
    for (int x = in; x < cv->w - in; x++)
        for (int k = 0; k < 2; k++) {
            int y = k ? cv->h - in : in;
            mr_cv_mix(cv, x, y, 0x000000, 0.3f);
            if (x % 12 < 7) mr_cv_mix(cv, x, y + (k ? -6 : 6), 0xD8B48A, 0.35f);
        }
    for (int y = in; y < cv->h - in; y++)
        for (int k = 0; k < 2; k++) {
            int x = k ? cv->w - in : in;
            mr_cv_mix(cv, x, y, 0x000000, 0.3f);
            if (y % 12 < 7) mr_cv_mix(cv, x + (k ? -6 : 6), y, 0xD8B48A, 0.35f);
        }
}

/* --- build ------------------------------------------------------------------------ */

static void place_ctr(mr_scene_t *s, ctr_t *c, int who)
{
    cnt_t *C = ST;
    mr_game_t *g = s->g;
    float D = C->D, R = D * 0.5f;
    int bx = (int)(c->x - C->bw * 0.5f), by = (int)(c->y - R - 10);
    float travel = D * 0.07f;
    c->plunger = mr_img_obj(s->obj, &C->plunger);
    lv_obj_set_pos(c->plunger, (int)(c->x - C->plunger.w * 0.5f), (int)(c->y - R - D * 0.21f));
    (void)travel;
    c->knob = mr_img_obj(s->obj, &C->knob[0]);
    lv_obj_set_pos(c->knob, (int)(c->x + R * 0.78f), (int)(c->y - R * 0.55f - C->knob[0].h * 0.5f));
    c->body = mr_img_obj(s->obj, &C->body);
    lv_obj_set_pos(c->body, bx, by);
    for (int k = 0; k < NW; k++) {
        c->clip[k] = lv_obj_create(s->obj);
        mr_obj_plain(c->clip[k]);
        lv_obj_set_size(c->clip[k], C->cw, C->wh);
        lv_obj_set_pos(c->clip[k], bx + C->wx + k * C->cw, by + C->wy);
        c->strip[k] = mr_img_obj(c->clip[k], &C->strip);
        lv_obj_set_pos(c->strip[k], 0, 0);
    }
    c->over = mr_img_obj(s->obj, &C->over);
    lv_obj_set_pos(c->over, bx + C->wx, by + C->wy);
    /* the label tape: the player's colour, white raised letters */
    c->tag = lv_obj_create(s->obj);
    mr_obj_plain(c->tag);
    lv_obj_set_style_bg_color(c->tag, lv_color_hex(mr_rgb_scale(g->p[who].color, 0.78f)), 0);
    lv_obj_set_style_bg_opa(c->tag, 255, 0);
    lv_obj_set_style_bg_grad_color(c->tag, lv_color_hex(mr_rgb_scale(g->p[who].color, 0.55f)), 0);
    lv_obj_set_style_bg_grad_dir(c->tag, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(c->tag, 4, 0);
    lv_obj_set_style_shadow_width(c->tag, 10, 0);
    lv_obj_set_style_shadow_offset_y(c->tag, 4, 0);
    lv_obj_set_style_shadow_opa(c->tag, 150, 0);
    lv_obj_set_style_pad_hor(c->tag, 18, 0);
    lv_obj_set_style_pad_ver(c->tag, 6, 0);
    lv_obj_set_size(c->tag, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_transform_rotation(c->tag, (who & 1) ? 12 : -15, 0);
    char up[MR_NAME * 2];
    size_t k = 0;
    for (const char *p = g->p[who].name; *p && k < sizeof up - 1; p++) up[k++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    up[k] = 0;
    c->tagl = mr_label(c->tag, D > 300 ? aos_font_body : aos_font_small, 0xFAFAFA, up);
    lv_obj_set_style_text_letter_space(c->tagl, 3, 0);
    lv_obj_set_style_text_opa(c->tagl, 235, 0);
    lv_obj_update_layout(c->tag);
    int tw = lv_obj_get_width(c->tag);
    lv_obj_set_pos(c->tag, (int)(c->x - tw * 0.5f), (int)(c->y + R * 1.58f));
    int bsz = D > 300 ? 64 : 52;
    mr_rbtn(s, (int)(c->x + R * 0.75f), (int)(c->y + R * 1.05f), bsz, bsz, LV_SYMBOL_MINUS, 0x2A1E16, 0xE8D8C4,
            mr_cuenta_minus, who);
}

static void build(mr_scene_t *s)
{
    cnt_t *C = (cnt_t *)mr_alloc(sizeof(cnt_t));
    s->st = C;
    if (!C) return;
    mr_game_t *g = s->g;
    C->n = g->np;
    int top = s->top, W = s->w, H = s->h - top;
    int cols = 1, rows = 1;
    if (C->n == 2) {
        cols = s->land ? 2 : 1;
        rows = s->land ? 1 : 2;
    } else if (C->n == 3) {
        cols = s->land ? 3 : 2;
        rows = s->land ? 1 : 2;
    } else if (C->n == 4) {
        cols = s->land ? 4 : 2;
        rows = s->land ? 1 : 2;
    }
    float cellw = (float)W / cols, cellh = (float)H / rows;
    /* a counter needs about 1.0 D wide (plus the knob) and 2.1 D high */
    float D = mr_minf(cellw / 1.45f, (cellh - 70) / 1.62f);
    if (D > 420) D = 420;
    D = floorf(D / 2) * 2;
    C->D = D;
    for (int i = 0; i < C->n; i++) {
        int cx = i % cols, cy = i / cols;
        if (C->n == 3 && !s->land && i == 2) cx = 0, cy = 1;
        float x = cellw * (cx + 0.5f), y = top + cellh * (cy + 0.5f) - D * 0.06f - 24;
        if (C->n == 3 && !s->land && i == 2) x = W * 0.5f;
        C->c[i].x = x - D * 0.06f;
        C->c[i].y = y;
        C->c[i].finger = -1;
    }
    float R = D * 0.5f;
    C->ww = (int)(D * 0.62f) / NW * NW;
    C->cw = C->ww / NW;
    C->wh = (int)(D * 0.25f);
    C->bw = (int)(D + 40);
    C->bh = (int)(D * 1.48f + 30);
    C->wx = (C->bw - C->ww) / 2;
    C->wy = (int)(R + 10 - C->wh * 0.72f);
    if (!mr_cv_new(&C->bg, s->w, s->h)) return;
    desk_draw(s);
    mr_cv_obj(s->obj, &C->bg);
    mr_img_new(&C->body, C->bw, C->bh);
    body_draw(C);
    mr_img_new(&C->plunger, (int)(D * 0.34f), (int)(D * 0.30f));
    plunger_draw(C);
    for (int k = 0; k < KNOB_FR; k++) {
        mr_img_new(&C->knob[k], (int)(D * 0.13f), (int)(D * 0.20f));
        knob_draw(C, &C->knob[k], k);
    }
    mr_img_new(&C->strip, C->cw, C->wh * 11);
    strip_draw(C);
    mr_img_new(&C->over, C->ww, C->wh);
    over_draw(C);
    for (int i = 0; i < C->n; i++) place_ctr(s, &C->c[i], i);
    for (int i = 0; i < MR_FINGERS; i++) C->f[i].who = -1;
}

static void sfree(mr_scene_t *s)
{
    cnt_t *C = ST;
    if (!C) return;
    mr_cv_free(&C->bg);
    mr_img_free(&C->body);
    mr_img_free(&C->plunger);
    for (int k = 0; k < KNOB_FR; k++) mr_img_free(&C->knob[k]);
    mr_img_free(&C->strip);
    mr_img_free(&C->over);
}

/* --- the wheels ---------------------------------------------------------------------- */

static void wheels_target(mr_scene_t *s, int i, bool snap)
{
    cnt_t *C = ST;
    ctr_t *c = &C->c[i];
    int32_t v = s->g->p[i].score;
    if (v < 0) v = 0;
    int32_t p10 = 1;
    for (int k = NW - 1; k >= 0; k--) {
        /* wheel NW-1 is the units */
        float t = c->base[k] + (float)(v / p10);
        if (t != c->tgt[k]) {
            float prev = c->tgt[k];
            c->tgt[k] = t;
            /* a carry: the higher wheel moves after the one below has gone round */
            c->delay[k] = (k < NW - 1 && fabsf(t - prev) < 2.0f) ? 0.045f * (float)(NW - 1 - k) : 0.0f;
        }
        if (snap) {
            c->pos[k] = c->tgt[k];
            c->vel[k] = 0;
        }
        p10 *= 10;
    }
}

static void reset_wheels(mr_scene_t *s, int i)
{
    /* the knob rolls every wheel forward to its next nought */
    cnt_t *C = ST;
    ctr_t *c = &C->c[i];
    for (int k = 0; k < NW; k++) {
        float next = ceilf((c->pos[k] - 0.02f) / 10.0f) * 10.0f;
        if (next < c->pos[k] - 0.01f) next += 10;
        c->base[k] = next;
        c->tgt[k] = next;
        c->delay[k] = 0.02f * (float)k;
    }
    c->spinning = true;
    (void)s;
}

static void wheel_show(cnt_t *C, ctr_t *c, int k)
{
    float d = fmodf(c->pos[k], 10.0f);
    if (d < 0) d += 10.0f;
    lv_obj_set_pos(c->strip[k], 0, (int)(-d * C->wh));
}

static void frame(mr_scene_t *s, float dt)
{
    cnt_t *C = ST;
    if (!C) return;
    for (int i = 0; i < C->n; i++) {
        ctr_t *c = &C->c[i];
        /* the plunger follows the finger */
        float want = c->down ? 1.0f : 0.0f;
        float np = c->press + (want - c->press) * mr_clampf(dt * (c->down ? 40.0f : 22.0f), 0, 1);
        if (fabsf(np - c->press) > 0.002f) {
            c->press = np;
            float R = C->D * 0.5f;
            int dy = (int)(np * C->D * 0.07f);
            lv_obj_set_pos(c->plunger, (int)(c->x - C->plunger.w * 0.5f), (int)(c->y - R - C->D * 0.21f) + dy);
        }
        bool moving = false;
        for (int k = 0; k < NW; k++) {
            if (c->delay[k] > 0) {
                c->delay[k] -= dt;
                moving = true;
                continue;
            }
            float x = c->tgt[k] - c->pos[k];
            if (fabsf(x) < 0.001f && fabsf(c->vel[k]) < 0.01f) {
                if (c->pos[k] != c->tgt[k]) {
                    c->pos[k] = c->tgt[k];
                    wheel_show(C, c, k);
                }
                continue;
            }
            moving = true;
            /* a fast spin (reset) runs at a steady whirr, a click springs */
            if (fabsf(x) > 1.5f) {
                float v = 26.0f * (x > 0 ? 1 : -1);
                c->pos[k] += v * dt;
                if ((x > 0 && c->pos[k] > c->tgt[k] - 1.0f) || (x < 0 && c->pos[k] < c->tgt[k] + 1.0f)) c->vel[k] = v * 0.4f;
            } else {
                float kk = 900.0f, damp = 2.0f * sqrtf(kk) * 0.42f;
                int steps = 4;
                float h = dt / steps;
                for (int st = 0; st < steps; st++) {
                    float a = kk * (c->tgt[k] - c->pos[k]) - damp * c->vel[k];
                    c->vel[k] += a * h;
                    c->pos[k] += c->vel[k] * h;
                }
            }
            wheel_show(C, c, k);
        }
        if (c->spinning) {
            c->knob_t += dt * 22.0f;
            lv_image_set_src(c->knob, &C->knob[((int)c->knob_t) % KNOB_FR].dsc);
            if (!moving) {
                c->spinning = false;
                mr_snd_play(MR_S_CLICK_UP, 0.6f, 0.8f);
            }
        }
        /* at rest, the numbers are brought back near zero */
        if (!moving) {
            for (int k = 0; k < NW; k++) {
                if (c->pos[k] >= 10.0f) {
                    float sh = floorf(c->pos[k] / 10.0f) * 10.0f;
                    if (c->base[k] >= sh) {
                        c->pos[k] -= sh;
                        c->tgt[k] -= sh;
                        c->base[k] -= sh;
                    }
                }
            }
        }
    }
    /* the knob is held to reset */
    for (int f = 0; f < MR_FINGERS; f++) {
        if (C->f[f].who < 0 || !C->f[f].knob || C->f[f].fired) continue;
        if (!mr_A->fing[f].id) continue;
        if (mr_ms() - C->f[f].t0 > RESET_MS) {
            C->f[f].fired = true;
            int i = C->f[f].who;
            if (s->g->p[i].score) {
                reset_wheels(s, i);
                mr_snd_play(MR_S_RESET, 0.8f, 1.0f);
                mr_add(s->g, i, -(int)s->g->p[i].score);
            }
        }
    }
}

static void sync(mr_scene_t *s, int why, int who)
{
    cnt_t *C = ST;
    if (!C) return;
    for (int i = 0; i < C->n; i++) {
        if (why == MR_SYNC_RESET && s->g->p[i].score == 0) {
            reset_wheels(s, i);
            continue;
        }
        wheels_target(s, i, why == MR_SYNC_LOAD);
        if (why == MR_SYNC_LOAD)
            for (int k = 0; k < NW; k++) wheel_show(C, &C->c[i], k);
    }
    (void)who;
}

/* --- fingers -------------------------------------------------------------------------- */

static void mr_cuenta_minus(mr_scene_t *s, int who)
{
    mr_add(s->g, who, -1);
    mr_snd_play(MR_S_CLICK_UP, 0.5f, 0.7f);
}

static void down(mr_scene_t *s, int f, float x, float y)
{
    cnt_t *C = ST;
    if (!C) return;
    float R = C->D * 0.5f;
    for (int i = 0; i < C->n; i++) {
        ctr_t *c = &C->c[i];
        /* the knob first: it sticks out of the right side */
        float kx = c->x + R * 0.95f, ky = c->y - R * 0.55f;
        if (fabsf(x - kx) < C->D * 0.16f && fabsf(y - ky) < C->D * 0.16f) {
            C->f[f].who = i;
            C->f[f].knob = true;
            C->f[f].t0 = mr_ms();
            C->f[f].fired = false;
            c->spinning = true;      /* it turns under the finger while held */
            return;
        }
        float dx = x - c->x, dy = y - c->y;
        bool on_body = dx * dx + dy * dy < (R * 1.1f) * (R * 1.1f);
        bool on_top = fabsf(dx) < C->D * 0.2f && y < c->y - R * 0.6f && y > c->y - R - C->D * 0.3f;
        bool on_ring = fabsf(dx) < R * 0.5f && y > c->y && y < c->y + R * 1.6f;
        if (on_body || on_top || on_ring) {
            if (c->down) return;    /* one finger per counter */
            C->f[f].who = i;
            C->f[f].knob = false;
            c->down = true;
            c->finger = f;
            mr_snd_play(MR_S_CLICK, 0.9f, 0.96f + 0.08f * (float)(i) / 4.0f);
            mr_add(s->g, i, 1);
            return;
        }
    }
}

static void up(mr_scene_t *s, int f, float x, float y)
{
    (void)x;
    (void)y;
    cnt_t *C = ST;
    if (!C) return;
    int i = C->f[f].who;
    C->f[f].who = -1;
    if (i < 0) return;
    ctr_t *c = &C->c[i];
    if (C->f[f].knob) {
        if (!C->f[f].fired) c->spinning = false;
        return;
    }
    if (c->finger == f) {
        c->down = false;
        c->finger = -1;
        mr_snd_play(MR_S_CLICK_UP, 0.7f, 1.0f);
    }
}

const mr_scene_ops_t mr_cuenta_ops = { build, sfree, sync, frame, down, NULL, up, NULL };
