/*
 * Marcador - the stadium board.
 *
 * The big board at the end of a field: a steel cabinet on two posts against
 * the night, the teams' names on painted plates, the goals in incandescent
 * lamps (a 5 x 7 grid of bulbs per digit), the game clock in red seven
 * segment LEDs and the period in lamps. Under it, standing in, the scorer's
 * console with its chunky keys.
 *
 * A lamp is a bulb in a black cup: off, the glass shows a faint reflection;
 * on, a white-hot filament inside an amber glow that spills onto its
 * neighbours. Bulbs are not LEDs: they light in a few tens of milliseconds
 * and fade over a couple of hundred, so a changing number glows across from
 * the old shape to the new one, and then blinks three times to be noticed,
 * with the relays clacking in the cabinet. The board is drawn into one
 * canvas, and only the digits whose lamps are moving are drawn again.
 *
 * The clock runs forward or down by the sport's periods; at the end of a
 * period it stops and the horn sounds. Tap the clock to start or stop it,
 * hold it to put this period back to the start. Tap a team's number to give
 * it a point; the console has the rest.
 */
#include "mr_app.h"

#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAXD     3          /* digits per team                              */
#define NL       35         /* lamps in a digit                              */
#define BLINKS   3

/* digits on 5 x 7, row by row, five bits each (bit 4 the left) */
static const uint8_t FONT57[10][7] = {
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
};

/* seven segments: a b c d e f g */
static const uint8_t SEG7[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

typedef struct {
    int   x, y;             /* top left of the digit's lamps                */
    float pitch;
    float lit[NL];
    int   shown;            /* the digit it shows (-1 blank)                */
    bool  dirty;
} ldig_t;

typedef struct {
    mr_cv_t   bg, base;     /* what is shown, and the board without lamps    */
    lv_obj_t *cvo;
    mr_img_t  bulb_off[2], bulb_on[2];  /* big and small                     */
    ldig_t    d[2][MAXD];
    int       nd;
    ldig_t    per;
    float     blink[2];     /* seconds of blinking left                      */
    int       target[2][MAXD];
    struct {
        int x, y, w, h;     /* the clock's digits                            */
        float dh;
    } clk;
    int       clk_shown;    /* the seconds drawn                             */
    bool      colon;
    bool      ended;
    lv_obj_t *plate[2], *plate_l[2];
    lv_obj_t *b_run;
    struct {
        int      kind;      /* 0 nothing, 1 team, 2 clock                    */
        int      team;
        uint32_t t0;
        bool     fired;
    } f[MR_FINGERS];
    int       score_x[2][2], score_y[2][2];   /* the teams' number areas     */
} est_t;

#define ST ((est_t *)s->st)

static void est_btn(mr_scene_t *s, int arg);

/* --- lamps -------------------------------------------------------------------------- */

static void bulb_draw(mr_img_t *im, float p, bool on)
{
    float cx = im->w * 0.5f, cy = im->h * 0.5f, r = p * 0.36f;
    for (int y = 0; y < im->h; y++) {
        for (int x = 0; x < im->w; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
            if (on) {
                /* the glow: amber, falling off over a lamp's pitch */
                float gl = expf(-(d * d) / (p * p * 0.30f)) * 0.75f;
                float core = mr_clampf(r + 0.5f - d, 0, 1);
                float hot = expf(-(d * d) / (r * r * 0.35f));
                uint32_t c = mr_rgb_mix(0xFF8A10, 0xFFF6D8, hot);
                float a = mr_maxf(gl, core * (0.75f + 0.25f * hot));
                if (a > 0.003f) mr_img_over(im, x, y, core > 0 ? c : 0xFF9A22, a);
            } else {
                /* the cup and the dead bulb with a sliver of reflection */
                float cup = mr_clampf(r * 1.25f + 0.5f - d, 0, 1);
                if (cup > 0) mr_img_over(im, x, y, 0x050505, cup);
                float g = mr_clampf(r + 0.5f - d, 0, 1);
                if (g > 0) {
                    float l = 0.16f + 0.10f * (-dy / r) + 0.5f * expf(-((dx + r * 0.35f) * (dx + r * 0.35f) +
                                                                          (dy + r * 0.40f) * (dy + r * 0.40f)) /
                                                                         (r * r * 0.05f));
                    mr_img_over(im, x, y, mr_rgb_scale(0x6A5A40, l * 1.6f), g);
                }
            }
        }
    }
}

/* add light: brighter, never darker */
static void cv_add(mr_cv_t *cv, int x, int y, uint32_t c, float a)
{
    if ((unsigned)x >= (unsigned)cv->w || (unsigned)y >= (unsigned)cv->h) return;
    uint16_t *p = &cv->px[y * cv->w + x];
    uint32_t d = mr_888(*p);
    int r = (int)((d >> 16) & 255) + (int)(((c >> 16) & 255) * a);
    int g = (int)((d >> 8) & 255) + (int)(((c >> 8) & 255) * a);
    int b = (int)(d & 255) + (int)((c & 255) * a);
    *p = mr_565(((uint32_t)(r > 255 ? 255 : r) << 16) | ((uint32_t)(g > 255 ? 255 : g) << 8) | (uint32_t)(b > 255 ? 255 : b));
}

static void invalidate(mr_scene_t *s, int x0, int y0, int x1, int y1)
{
    est_t *E = ST;
    lv_area_t o, a;
    lv_obj_get_coords(E->cvo, &o);
    a.x1 = o.x1 + x0;
    a.y1 = o.y1 + y0;
    a.x2 = o.x1 + x1;
    a.y2 = o.y1 + y1;
    lv_obj_invalidate_area(E->cvo, &a);
}

static void digit_draw(mr_scene_t *s, ldig_t *d, int big)
{
    est_t *E = ST;
    mr_img_t *off = &E->bulb_off[big], *on = &E->bulb_on[big];
    float p = d->pitch;
    int m = (int)(p * 0.8f);
    int x0 = d->x - m, y0 = d->y - m, x1 = d->x + (int)(5 * p) + m, y1 = d->y + (int)(7 * p) + m;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > E->bg.w) x1 = E->bg.w;
    if (y1 > E->bg.h) y1 = E->bg.h;
    for (int y = y0; y < y1; y++) memcpy(&E->bg.px[y * E->bg.w + x0], &E->base.px[y * E->bg.w + x0], (size_t)(x1 - x0) * 2);
    for (int i = 0; i < NL; i++) {
        int cx = d->x + (int)((i % 5 + 0.5f) * p), cy = d->y + (int)((i / 5 + 0.5f) * p);
        mr_cv_put_img(&E->bg, off, cx - off->w / 2, cy - off->h / 2, 1.0f);
    }
    for (int i = 0; i < NL; i++) {
        float a = d->lit[i];
        if (a < 0.01f) continue;
        int cx = d->x + (int)((i % 5 + 0.5f) * p), cy = d->y + (int)((i / 5 + 0.5f) * p);
        int ox = cx - on->w / 2, oy = cy - on->h / 2;
        for (int y = 0; y < on->h; y++) {
            int yy = oy + y;
            if (yy < y0 || yy >= y1) continue;
            for (int x = 0; x < on->w; x++) {
                int xx = ox + x;
                if (xx < x0 || xx >= x1) continue;
                uint32_t sp = on->px[y * on->w + x];
                if (!(sp >> 24)) continue;
                cv_add(&E->bg, xx, yy, sp & 0xFFFFFF, (float)(sp >> 24) / 255.0f * a);
            }
        }
    }
    invalidate(s, x0, y0, x1, y1);
    d->dirty = false;
}

static bool lamp_on(int digit, int i)
{
    if (digit < 0 || digit > 9) return false;
    return (FONT57[digit][i / 5] >> (4 - i % 5)) & 1;
}

/* --- the clock --------------------------------------------------------------------------- */

static void seg_draw(mr_cv_t *cv, float x, float y, float h, int seg, bool on)
{
    float w = h * 0.55f, t = h * 0.11f;
    float ax, ay, bx, by;
    switch (seg) {
    case 0: ax = x + t, ay = y; bx = x + w - t, by = y; break;
    case 1: ax = x + w, ay = y + t; bx = x + w, by = y + h * 0.5f - t; break;
    case 2: ax = x + w, ay = y + h * 0.5f + t; bx = x + w, by = y + h - t; break;
    case 3: ax = x + t, ay = y + h; bx = x + w - t, by = y + h; break;
    case 4: ax = x, ay = y + h * 0.5f + t; bx = x, by = y + h - t; break;
    case 5: ax = x, ay = y + t; bx = x, by = y + h * 0.5f - t; break;
    default: ax = x + t, ay = y + h * 0.5f; bx = x + w - t, by = y + h * 0.5f; break;
    }
    /* slanted, like the old LED digits */
    float k = 0.08f;
    ax -= (ay - y - h * 0.5f) * k;
    bx -= (by - y - h * 0.5f) * k;
    int x0 = (int)(mr_minf(ax, bx) - t * 3), x1 = (int)(mr_maxf(ax, bx) + t * 3);
    int y0 = (int)(mr_minf(ay, by) - t * 3), y1 = (int)(mr_maxf(ay, by) + t * 3);
    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            float d = mr_seg_dist(px + 0.5f, py + 0.5f, ax, ay, bx, by);
            float core = mr_clampf(t * 0.5f + 0.5f - d, 0, 1);
            if (on) {
                float glow = expf(-(d * d) / (t * t * 2.2f)) * 0.55f;
                if (glow > 0.01f) cv_add(cv, px, py, 0xFF2010, glow);
                if (core > 0) mr_cv_mix(cv, px, py, mr_rgb_mix(0xFF3A22, 0xFFD0C0, mr_clampf(1 - d / (t * 0.5f), 0, 1) * 0.6f), core);
            } else if (core > 0) {
                mr_cv_mix(cv, px, py, 0x2A0A08, core);
            }
        }
    }
}

static int clock_shown(const mr_game_t *g)
{
    int per_ms = g->opt[MR_EST_MIN] * 60000;
    int ms = g->clock_ms;
    if (ms > per_ms) ms = per_ms;
    int show = g->opt[MR_EST_DOWN] ? per_ms - ms : ms;
    if (g->opt[MR_EST_DOWN]) show += 999;    /* a countdown shows the second it is in */
    return show / 1000;
}

static void clock_draw(mr_scene_t *s)
{
    est_t *E = ST;
    mr_game_t *g = s->g;
    int x0 = E->clk.x - 16, y0 = E->clk.y - 16, x1 = E->clk.x + E->clk.w + 16, y1 = E->clk.y + E->clk.h + 16;
    for (int y = y0; y < y1; y++) memcpy(&E->bg.px[y * E->bg.w + x0], &E->base.px[y * E->bg.w + x0], (size_t)(x1 - x0) * 2);
    int sec = clock_shown(g);
    int mm = sec / 60, ss = sec % 60;
    int dig[4] = { mm / 10 % 10, mm % 10, ss / 10, ss % 10 };
    float h = E->clk.dh, dw = h * 0.55f, gap = h * 0.28f, cg = h * 0.42f;
    float x = E->clk.x + h * 0.06f;
    for (int i = 0; i < 4; i++) {
        for (int sg = 0; sg < 7; sg++) seg_draw(&E->bg, x, E->clk.y, h, sg, (SEG7[dig[i]] >> sg) & 1);
        x += dw + gap;
        if (i == 1) {
            float cx = x - gap * 0.5f + cg * 0.5f - h * 0.05f;
            bool on = E->colon || !g->running;
            for (int k = 0; k < 2; k++) {
                float cy = E->clk.y + h * (k ? 0.70f : 0.30f);
                for (int py = (int)(cy - 10); py <= (int)(cy + 10); py++)
                    for (int px = (int)(cx - 10); px <= (int)(cx + 10); px++) {
                        float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
                        float core = mr_clampf(h * 0.055f + 0.5f - d, 0, 1);
                        if (on && d < 10) cv_add(&E->bg, px, py, 0xFF2010, expf(-d * d / 18.0f) * 0.5f);
                        if (core > 0) mr_cv_mix(&E->bg, px, py, on ? 0xFF5A40 : 0x2A0A08, core);
                    }
            }
            x += cg;
        }
    }
    invalidate(s, x0, y0, x1, y1);
}

/* --- the board ----------------------------------------------------------------------------- */

static void steel(mr_cv_t *cv, int x0, int y0, int w, int h, uint32_t c, float rough)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            if ((unsigned)x >= (unsigned)cv->w || (unsigned)y >= (unsigned)cv->h) continue;
            float n = (mr_hashf(x / 2, y, 21) - 0.5f) * rough + (mr_vnoise(x / 40.0f, y / 40.0f, 23) - 0.5f) * 0.12f;
            cv->px[y * cv->w + x] = mr_565(mr_rgb_scale(c, 1.0f + n - 0.18f * (float)(y - y0) / h));
        }
}

static void bolt(mr_cv_t *cv, float cx, float cy, float r)
{
    for (int y = (int)(cy - r - 2); y <= (int)(cy + r + 3); y++)
        for (int x = (int)(cx - r - 2); x <= (int)(cx + r + 3); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
            float sd = sqrtf((dx - 2) * (dx - 2) + (dy - 2) * (dy - 2));
            if (sd < r) mr_cv_mix(cv, x, y, 0, 0.4f);
            float c = mr_clampf(r + 0.5f - d, 0, 1);
            if (c > 0) mr_cv_mix(cv, x, y, mr_rgb_scale(0x8A9098, 0.7f + 0.6f * (-dx - dy) / (2 * r)), c);
        }
}

static void board_draw(mr_scene_t *s, int bx, int by, int bw, int bh, int cy0)
{
    est_t *E = ST;
    mr_cv_t *cv = &E->bg;
    /* the night, with the floodlights' haze high up */
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            float t = (float)y / cv->h;
            uint32_t c = mr_rgb_mix(0x0E1A30, 0x03050A, mr_smooth(0.0f, 0.8f, t));
            float hz = 0;
            for (int k = 0; k < 2; k++) {
                float fx = k ? cv->w * 1.05f : -cv->w * 0.05f, fy = s->top - 40.0f;
                float dx = (x - fx) / (cv->w * 0.6f), dy = (y - fy) / (cv->h * 0.35f);
                hz += expf(-(dx * dx + dy * dy)) * 0.35f;
            }
            c = mr_rgb_mix(c, 0x9FB4D8, hz);
            float n = (mr_hashf(x, y, 3) - 0.5f) * 0.04f;
            cv->px[y * cv->w + x] = mr_565(mr_rgb_scale(c, 1 + n));
        }
    /* the posts */
    for (int k = 0; k < 2; k++) {
        int px = bx + (k ? bw * 3 / 4 : bw / 4) - 18;
        steel(cv, px, by + bh, 36, cy0 - by - bh + 40, 0x2A2E34, 0.10f);
        for (int y = by + bh; y < cy0 + 40 && y < cv->h; y++) {
            mr_cv_mix(cv, px, y, 0xFFFFFF, 0.08f);
            mr_cv_mix(cv, px + 35, y, 0x000000, 0.4f);
        }
    }
    /* the cabinet: painted steel, a lip all round, bolts */
    steel(cv, bx, by, bw, bh, 0x23272D, 0.08f);
    for (int i = 0; i < 6; i++) {
        for (int x = bx; x < bx + bw; x++) {
            mr_cv_mix(cv, x, by + i, 0xFFFFFF, 0.10f - i * 0.015f);
            mr_cv_mix(cv, x, by + bh - 1 - i, 0x000000, 0.35f - i * 0.05f);
        }
        for (int y = by; y < by + bh; y++) {
            mr_cv_mix(cv, bx + i, y, 0xFFFFFF, 0.06f - i * 0.01f);
            mr_cv_mix(cv, bx + bw - 1 - i, y, 0x000000, 0.3f - i * 0.04f);
        }
    }
    float br = bw > 900 ? 7 : 5.5f;
    for (int i = 0; i < 9; i++) {
        float x = bx + 14 + (bw - 28) * i / 8.0f;
        bolt(cv, x, by + 13, br);
        bolt(cv, x, by + bh - 14, br);
    }
}

static void panel(mr_cv_t *cv, int x, int y, int w, int h)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) {
            float n = (mr_hashf(xx, yy, 9) - 0.5f) * 0.05f;
            /* louvres: faint horizontal slats in the black */
            float sl = ((yy - y) % 12 < 2) ? 0.6f : 1.0f;
            mr_cv_mix(cv, xx, yy, mr_rgb_scale(0x0D0E10, (1 + n) * sl), 1.0f);
        }
    for (int i = 0; i < 3; i++) {
        for (int xx = x; xx < x + w; xx++) {
            mr_cv_mix(cv, xx, y + i, 0x000000, 0.6f);
            mr_cv_mix(cv, xx, y + h - 1 - i, 0xFFFFFF, 0.05f);
        }
    }
}

static void console_draw(mr_scene_t *s, int y0)
{
    est_t *E = ST;
    mr_cv_t *cv = &E->bg;
    int x0 = 10, w = cv->w - 20, h = cv->h - y0 - 8;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            float t = (float)(y - y0) / h;
            float n = (mr_hashf(x, y, 77) - 0.5f) * 0.06f;
            float edge = (y - y0 < 4) ? 1.5f : 1.0f;
            mr_cv_mix(cv, x, y, mr_rgb_scale(0x3A3F47, (0.95f - 0.25f * t + n) * edge), 1.0f);
        }
    for (int x = x0; x < x0 + w; x++)
        for (int i = 0; i < 10; i++) mr_cv_mix(cv, x, y0 - i, 0x000000, 0.25f - i * 0.025f);
    (void)s;
}

static void build(mr_scene_t *s)
{
    est_t *E = (est_t *)mr_alloc(sizeof(est_t));
    s->st = E;
    if (!E) return;
    mr_game_t *g = s->g;
    E->nd = g->opt[MR_EST_SPORT] == 1 ? 3 : 2;
    int top = s->top + 8;
    int bw = s->w - 24, bx = 12, by = top;
    bool basket = g->opt[MR_EST_SPORT] == 1;
    bool stack = !s->land;      /* upright: the teams one over the other */
    int plate_h = 66;
    float dun = E->nd * 5.0f + (E->nd - 1) * 1.6f;     /* a team's digits, in pitches */
    float p, clk_h;
    int console_h = s->land ? 150 : (basket ? 470 : 340);
    float avail = s->h - top - console_h - 40;
    if (stack) {
        clk_h = 100;
        float pw = (bw - 48 - 40) / dun;
        float ph = (avail - 24 - 2 * (plate_h + 10 + 18 + 16) - (clk_h + 32) - 40) / 14.0f;
        p = mr_minf(pw, ph);
    } else {
        float pw = ((bw - 60) * 0.5f - 20) / dun;
        float ph = (avail - 70 - 30 - 150) / 7.0f;
        p = mr_minf(pw, ph);
        if (p > 48) p = 48;
    }
    p = floorf(p);
    if (!stack) clk_h = mr_minf(110.0f, p * 2.6f);
    int team_h = (int)(plate_h + 10 + 7 * p + 18);
    int bh = stack ? (int)(24 + 2 * (team_h + 16) + clk_h + 32 + 40) : (int)(26 + plate_h + 18 + 7 * p + 22 + clk_h + 40);
    int cy0 = by + bh + (s->land ? 18 : 40);
    if (!mr_cv_new(&E->bg, s->w, s->h) || !mr_cv_new(&E->base, s->w, s->h)) return;
    board_draw(s, bx, by, bw, bh, cy0);
    console_draw(s, cy0);

    /* where each team goes: its plate, then its numbers' panel */
    int plx[2], ply[2], plw[2];
    for (int t = 0; t < 2; t++) {
        if (stack) {
            plx[t] = bx + 24;
            plw[t] = bw - 48;
            ply[t] = by + 24 + t * (team_h + 16);
        } else {
            plx[t] = t ? bx + bw / 2 + 12 : bx + 24;
            plw[t] = bw / 2 - 36;
            ply[t] = by + 24;
        }
        int pxl = plx[t], pw2 = plw[t], py = ply[t];
        panel(&E->bg, pxl, py + plate_h + 10, pw2, (int)(7 * p + 18));
        float dw = dun * p;
        int dx = pxl + (int)((pw2 - dw) * 0.5f);
        for (int k = 0; k < E->nd; k++) {
            ldig_t *d = &E->d[t][k];
            d->x = dx + (int)(k * 6.6f * p);
            d->y = py + plate_h + 19;
            d->pitch = p;
            d->shown = -2;
        }
        E->score_x[t][0] = pxl;
        E->score_x[t][1] = pxl + pw2;
        E->score_y[t][0] = py;
        E->score_y[t][1] = py + team_h;
    }
    /* the clock's black window and the period lamp */
    float cw = clk_h * (4 * 0.55f + 3 * 0.28f + 0.42f) + clk_h * 0.2f;
    float pp = clk_h / 8.0f;
    E->clk.dh = clk_h;
    E->clk.w = (int)cw;
    E->clk.h = (int)clk_h;
    E->clk.x = (int)(bx + bw * 0.5f - (cw + 70 + 5 * pp) * 0.5f);
    E->clk.y = (int)(by + bh - clk_h - 34);
    panel(&E->bg, E->clk.x - 22, E->clk.y - 16, E->clk.w + 44, E->clk.h + 32);
    E->per.pitch = pp;
    E->per.x = (int)(E->clk.x + E->clk.w + 70);
    E->per.y = (int)(E->clk.y + (clk_h - 7 * pp) * 0.5f + 10);
    E->per.shown = -2;
    panel(&E->bg, E->per.x - 12, E->per.y - 12, (int)(5 * pp + 24), (int)(7 * pp + 24));
    mr_cv_copy(&E->base, &E->bg);
    E->cvo = mr_cv_obj(s->obj, &E->bg);

    for (int k = 0; k < 2; k++) {
        float pitch = k ? pp : p;
        mr_img_new(&E->bulb_off[k], (int)(pitch * 1.0f) + 2, (int)(pitch * 1.0f) + 2);
        bulb_draw(&E->bulb_off[k], pitch, false);
        mr_img_new(&E->bulb_on[k], (int)(pitch * 2.4f), (int)(pitch * 2.4f));
        bulb_draw(&E->bulb_on[k], pitch, true);
    }

    /* painted plates with the names */
    for (int t = 0; t < 2; t++) {
        int pxl = plx[t], pw2 = plw[t];
        lv_obj_t *pl = lv_obj_create(s->obj);
        mr_obj_plain(pl);
        lv_obj_set_size(pl, pw2, plate_h);
        lv_obj_set_pos(pl, pxl, ply[t]);
        lv_obj_set_style_bg_color(pl, lv_color_hex(0x123A26), 0);
        lv_obj_set_style_bg_grad_color(pl, lv_color_hex(0x0B2418), 0);
        lv_obj_set_style_bg_grad_dir(pl, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(pl, 255, 0);
        lv_obj_set_style_border_color(pl, lv_color_hex(g->p[t].color), 0);
        lv_obj_set_style_border_width(pl, 4, 0);
        lv_obj_set_style_border_side(pl, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_radius(pl, 3, 0);
        char up[MR_NAME * 2];
        size_t k = 0;
        for (const char *c = g->p[t].name; *c && k < sizeof up - 1; c++) up[k++] = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32) : *c;
        up[k] = 0;
        lv_obj_t *l = mr_label(pl, aos_font_title, 0xF4F1E6, up);
        lv_obj_set_style_text_letter_space(l, 4, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, pw2 - 16);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        E->plate[t] = pl;
        E->plate_l[t] = l;
    }
    lv_obj_t *tl = mr_label(s->obj, aos_font_caption, 0xD8D4C8, _("TIEMPO"));
    lv_obj_set_style_text_letter_space(tl, 3, 0);
    lv_obj_update_layout(tl);
    lv_obj_set_pos(tl, (int)(E->per.x + 2.5f * pp - lv_obj_get_width(tl) * 0.5f), E->per.y - 40);

    /* the console */
    int ky = cy0 + 24;
    if (s->land) {
        /* lying down: one row, the teams at the ends and the clock in the middle */
        int bwk = 104, bhk = 78, gap = 12;
        int x = 30;
        for (int t = 0; t < 2; t++) {
            int n = basket ? 4 : 2;
            int xs = t ? s->w - 30 - n * (bwk + gap) + gap : x;
            int col = 0;
            mr_rbtn(s, xs + col++ * (bwk + gap), ky, bwk, bhk, LV_SYMBOL_MINUS, 0x5A2A24, 0xFFFFFF, est_btn, t * 10 + 0);
            mr_rbtn(s, xs + col++ * (bwk + gap), ky, bwk, bhk, "+1", 0x1F5A36, 0xFFFFFF, est_btn, t * 10 + 1);
            if (basket) {
                mr_rbtn(s, xs + col++ * (bwk + gap), ky, bwk, bhk, "+2", 0x1F5A36, 0xFFFFFF, est_btn, t * 10 + 2);
                mr_rbtn(s, xs + col++ * (bwk + gap), ky, bwk, bhk, "+3", 0x1F5A36, 0xFFFFFF, est_btn, t * 10 + 3);
            }
        }
        int cxm = s->w / 2;
        E->b_run = mr_rbtn(s, cxm - 170, ky, 104, bhk, LV_SYMBOL_PLAY, 0x2A4F80, 0xFFFFFF, est_btn, 100);
        mr_rbtn(s, cxm - 52, ky, 104, bhk, _("T+"), 0x4A4F58, 0xFFFFFF, est_btn, 101);
        mr_rbtn(s, cxm + 66, ky, 104, bhk, LV_SYMBOL_BELL, 0x80562A, 0xFFFFFF, est_btn, 102);
    } else {
        int bwk = 150, bhk = 96, gap = 14;
        for (int t = 0; t < 2; t++) {
            int xs = t ? s->w / 2 + 20 : 30;
            lv_obj_t *l = mr_label(s->obj, aos_font_small, 0xC8CCD2, g->p[t].name);
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_size(l, s->w / 2 - 50, lv_font_get_line_height(aos_font_small));
            lv_obj_set_pos(l, xs, ky);
            int yy = ky + 40;
            mr_rbtn(s, xs, yy, bwk, bhk, "+1", 0x1F5A36, 0xFFFFFF, est_btn, t * 10 + 1);
            mr_rbtn(s, xs + bwk + gap, yy, bwk, bhk, LV_SYMBOL_MINUS, 0x5A2A24, 0xFFFFFF, est_btn, t * 10 + 0);
            if (basket) {
                mr_rbtn(s, xs, yy + bhk + gap, bwk, bhk, "+2", 0x1F5A36, 0xFFFFFF, est_btn, t * 10 + 2);
                mr_rbtn(s, xs + bwk + gap, yy + bhk + gap, bwk, bhk, "+3", 0x1F5A36, 0xFFFFFF, est_btn, t * 10 + 3);
            }
        }
        int yy = s->h - 8 - 24 - bhk;
        int wk = (s->w - 60 - 2 * gap) / 3;
        E->b_run = mr_rbtn(s, 30, yy, wk, bhk, LV_SYMBOL_PLAY, 0x2A4F80, 0xFFFFFF, est_btn, 100);
        mr_rbtn(s, 30 + wk + gap, yy, wk, bhk, _("Tiempo +"), 0x4A4F58, 0xFFFFFF, est_btn, 101);
        mr_rbtn(s, 30 + 2 * (wk + gap), yy, wk, bhk, LV_SYMBOL_BELL "  ", 0x80562A, 0xFFFFFF, est_btn, 102);
    }
    E->clk_shown = -1;
}

static void sfree(mr_scene_t *s)
{
    est_t *E = ST;
    if (!E) return;
    mr_cv_free(&E->bg);
    mr_cv_free(&E->base);
    for (int k = 0; k < 2; k++) {
        mr_img_free(&E->bulb_off[k]);
        mr_img_free(&E->bulb_on[k]);
    }
}

/* --- following the model ---------------------------------------------------------------------- */

static void set_targets(mr_scene_t *s)
{
    est_t *E = ST;
    for (int t = 0; t < 2; t++) {
        int v = s->g->p[t].score;
        if (v < 0) v = 0;
        int mx = E->nd == 3 ? 999 : 99;
        if (v > mx) v = v % (mx + 1);
        for (int k = E->nd - 1; k >= 0; k--) {
            int dgt = v % 10;
            v /= 10;
            /* no leading noughts, but at least one digit */
            E->target[t][k] = (k < E->nd - 1 && v == 0 && dgt == 0) ? -1 : dgt;
        }
        /* a lone nought for zero */
        bool any = false;
        for (int k = 0; k < E->nd; k++)
            if (E->target[t][k] >= 0) any = true;
        if (!any) E->target[t][E->nd - 1] = 0;
        for (int k = 0; k < E->nd - 1; k++) {
            if (E->target[t][k] >= 0) {
                for (int j = k + 1; j < E->nd; j++)
                    if (E->target[t][j] < 0) E->target[t][j] = 0;
                break;
            }
        }
    }
}

static void run_label(mr_scene_t *s)
{
    est_t *E = ST;
    if (!E->b_run) return;
    lv_obj_t *l = lv_obj_get_child(E->b_run, 0);
    if (l) lv_label_set_text(l, s->g->running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void sync(mr_scene_t *s, int why, int who)
{
    est_t *E = ST;
    if (!E) return;
    int before[2][MAXD];
    memcpy(before, E->target, sizeof before);
    set_targets(s);
    if (why == MR_SYNC_LOAD) {
        for (int t = 0; t < 2; t++)
            for (int k = 0; k < E->nd; k++) {
                ldig_t *d = &E->d[t][k];
                for (int i = 0; i < NL; i++) d->lit[i] = lamp_on(E->target[t][k], i) ? 1.0f : 0.0f;
                d->shown = E->target[t][k];
                digit_draw(s, d, 0);
            }
        E->per.shown = -2;
    } else {
        for (int t = 0; t < 2; t++)
            if (memcmp(before[t], E->target[t], sizeof before[t])) {
                E->blink[t] = why == MR_SYNC_ADD && s->g->p[t].score > 0 && who == t ? 1.25f : 0;
                mr_snd_play(MR_S_RELAY, 0.8f, 0.95f + 0.1f * t);
            }
    }
    if (why == MR_SYNC_RESET) {
        E->ended = false;
        E->clk_shown = -1;
    }
    run_label(s);
}

static void frame(mr_scene_t *s, float dt)
{
    est_t *E = ST;
    if (!E) return;
    mr_game_t *g = s->g;
    /* the clock */
    if (g->running) {
        int per_ms = g->opt[MR_EST_MIN] * 60000;
        g->clock_ms += (int32_t)(dt * 1000.0f);
        if (g->clock_ms >= per_ms) {
            g->clock_ms = per_ms;
            g->running = false;
            E->ended = true;
            run_label(s);
            mr_snd_play(MR_S_HORN, 1.0f, 1.0f);
            E->blink[0] = E->blink[1] = 1.25f;
            mr_set_dirty();
        }
    }
    bool colon = (g->clock_ms % 1000) < 500;
    int sec = clock_shown(g);
    if (sec != E->clk_shown || colon != E->colon) {
        E->clk_shown = sec;
        E->colon = colon;
        clock_draw(s);
    }
    /* the lamps: hot quickly, cold slowly */
    for (int t = 0; t < 2; t++) {
        bool blank = false;
        if (E->blink[t] > 0) {
            E->blink[t] -= dt;
            float ph = 1.25f - E->blink[t];
            blank = ph > 0.30f && fmodf(ph - 0.30f, 0.32f) < 0.14f;
        }
        for (int k = 0; k < E->nd; k++) {
            ldig_t *d = &E->d[t][k];
            for (int i = 0; i < NL; i++) {
                float want = (!blank && lamp_on(E->target[t][k], i)) ? 1.0f : 0.0f;
                float v = d->lit[i];
                if (v == want) continue;
                float rate = want > v ? 28.0f : 9.0f;
                v += (want - v) * mr_clampf(dt * rate, 0, 1);
                if (fabsf(v - want) < 0.02f) v = want;
                d->lit[i] = v;
                d->dirty = true;
            }
            if (d->dirty) digit_draw(s, d, 0);
        }
    }
    int pd = g->period;
    if (E->per.shown != pd) {
        bool first = E->per.shown == -2;
        E->per.shown = pd;
        for (int i = 0; i < NL; i++) E->per.lit[i] = lamp_on(pd % 10, i) ? 1.0f : 0.0f;
        digit_draw(s, &E->per, 1);
        if (!first) mr_snd_play(MR_S_RELAY, 0.7f, 0.8f);
    }
    /* holding the clock puts the period back to its start */
    for (int f = 0; f < MR_FINGERS; f++) {
        if (E->f[f].kind != 2 || E->f[f].fired || !mr_A->fing[f].id || mr_A->fing[f].moved) continue;
        if (mr_ms() - E->f[f].t0 > 700) {
            E->f[f].fired = true;
            g->clock_ms = 0;
            g->running = false;
            E->ended = false;
            run_label(s);
            mr_snd_play(MR_S_RELAY, 0.8f, 0.7f);
            mr_toast(_("El reloj vuelve al principio del tiempo"));
            mr_set_dirty();
        }
    }
}

/* --- fingers ------------------------------------------------------------------------------- */

static void toggle_clock(mr_scene_t *s)
{
    est_t *E = ST;
    mr_game_t *g = s->g;
    if (!g->running && g->clock_ms >= g->opt[MR_EST_MIN] * 60000) {
        mr_toast(_("Terminó el tiempo: pasá al siguiente"));
        return;
    }
    g->running = !g->running;
    E->ended = false;
    run_label(s);
    mr_snd_play(MR_S_RELAY, 0.6f, 1.2f);
    mr_set_dirty();
}

static void est_btn(mr_scene_t *s, int arg)
{
    mr_game_t *g = s->g;
    est_t *E = ST;
    if (arg < 100) {
        int t = arg / 10, v = arg % 10;
        mr_add(g, t, v ? v : -1);
        return;
    }
    if (arg == 100) toggle_clock(s);
    else if (arg == 101) {
        int maxp = g->opt[MR_EST_PER] > 0 ? g->opt[MR_EST_PER] : 2;
        if (g->period >= maxp) {
            mr_toast(_("Ya es el último tiempo"));
            return;
        }
        g->period++;
        g->clock_ms = 0;
        g->running = false;
        E->ended = false;
        run_label(s);
        mr_set_dirty();
    } else if (arg == 102) {
        mr_snd_play(MR_S_HORN, 1.0f, 1.0f);
    }
}

static void down(mr_scene_t *s, int f, float x, float y)
{
    est_t *E = ST;
    if (!E) return;
    E->f[f].kind = 0;
    E->f[f].t0 = mr_ms();
    E->f[f].fired = false;
    if (x >= E->clk.x - 30 && x < E->clk.x + E->clk.w + 30 && y >= E->clk.y - 30 && y < E->clk.y + E->clk.h + 30) {
        E->f[f].kind = 2;
        return;
    }
    for (int t = 0; t < 2; t++)
        if (x >= E->score_x[t][0] && x < E->score_x[t][1] && y >= E->score_y[t][0] && y < E->score_y[t][1]) {
            E->f[f].kind = 1;
            E->f[f].team = t;
        }
}

static void up(mr_scene_t *s, int f, float x, float y)
{
    est_t *E = ST;
    if (!E) return;
    int k = E->f[f].kind;
    E->f[f].kind = 0;
    if (x < -1000 || mr_A->fing[f].moved || E->f[f].fired) return;
    if (k == 1) mr_add(s->g, E->f[f].team, 1);
    else if (k == 2) toggle_clock(s);
    (void)y;
}

const mr_scene_ops_t mr_estadio_ops = { build, sfree, sync, frame, down, NULL, up, NULL };
