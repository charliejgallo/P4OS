/*
 * Marcador - the abacus.
 *
 * A school abacus per player: a walnut frame, three (or four) steel rods
 * for the hundreds, tens and units, ten lacquered beads on each. The beads
 * pushed to the right are the ones that count, so the score is read the way
 * a child reads it: three on the hundreds, two on the tens, seven on the
 * units.
 *
 * A bead is a solid thing on a rod: drag one and it pushes the ones in its
 * way; let go and the loose ones run to whichever side they are nearer,
 * stopping with the clack of wood on wood. The reading of the beads is the
 * score; ten on the units are carried, by the app, to one on the tens (all
 * ten run back home and one ten moves over), as a teacher would.
 *
 * Tap a frame for a point, hold it to take one back.
 */
#include "mr_app.h"

#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define NB       10
#define MAXR     4
#define HOLD_MS  520

typedef struct {
    float     x, tx;        /* along the rod (u), and where it is going     */
    lv_obj_t *o;
    bool      moving;
} bead_t;

typedef struct {
    int      c;             /* the rod's line across (y lying, x standing)  */
    bead_t   b[NB];
    int      held;          /* the bead a finger has, or -1                 */
    int      finger;
    float    grab;          /* finger u minus bead u                        */
    uint32_t last_clack;
} rod_t;

typedef struct {
    int       x, y, w, h;   /* the frame                                    */
    int       u0;           /* the screen coordinate of u = 0               */
    float     L;            /* how far the beads run                        */
    int       nr;
    rod_t     r[MAXR];
    lv_obj_t *score;
} frm_t;

typedef struct {
    mr_cv_t   bg;
    mr_img_t  bead[MAXR][2];
    float     bw, bh;
    bool      vert;         /* standing rods (upright screen)               */
    int       n;
    frm_t     f[4];
    struct {
        int  fr, rod;
        bool held;
    } fing[MR_FINGERS];
} aba_t;

#define ST ((aba_t *)s->st)

static const uint32_t ROD_COL[MAXR] = { 0x2F7FD0, 0x2E9E58, 0xE8B021, 0xD2402E };

/* --- drawing ---------------------------------------------------------------------------- */

static void bead_draw(mr_img_t *im, float bw, float bh, uint32_t col, uint32_t seed, bool vert)
{
    float cx = im->w * 0.5f - 2, cy = im->h * 0.5f - 3;
    float a = bw * 0.5f, b = bh * 0.5f;
    const float lx = -0.5f, ly = -0.6f, lz = 0.62f;
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            float u = (x + 0.5f - cx) / a, v = (y + 0.5f - cy) / b;
            float d = u * u + v * v;
            if (d > 1.08f) continue;
            float cov = mr_clampf((1.0f - sqrtf(d)) * a + 0.5f, 0, 1);
            float nz = sqrtf(mr_clampf(1 - d, 0, 1)), nx = u, ny = v;
            float dif = mr_clampf(nx * lx + ny * ly + nz * lz, 0, 1);
            float hx = lx, hy = ly, hz = lz + 1, hl = sqrtf(hx * hx + hy * hy + hz * hz);
            float spec = powf(mr_clampf((nx * hx + ny * hy + nz * hz) / hl, 0, 1), 40.0f);
            /* the wood under the lacquer: faint rings round the hole */
            float ring = 0.5f + 0.5f * sinf(sqrtf(vert ? u * u + v * v * 0.3f : u * u * 0.3f + v * v) * 22.0f + mr_vnoise(u * 3, v * 3, seed) * 4);
            uint32_t c = mr_rgb_scale(col, (0.35f + 0.80f * dif) * (0.94f + 0.08f * ring));
            /* the rim where the hole is, left and right */
            if (!vert && fabsf(v) < 0.16f && fabsf(u) > 0.9f) c = mr_rgb_scale(c, 0.5f);
            if (vert && fabsf(u) < 0.16f && fabsf(v) > 0.9f) c = mr_rgb_scale(c, 0.5f);
            c = mr_rgb_mix(c, 0xFFFFFF, spec * 0.85f);
            mr_img_over(im, x, y, c, cov);
        }
    mr_img_shadow(im, 3, 5, 4, 0.45f);
}

static void wood_bar(mr_cv_t *cv, int x0, int y0, int w, int h, bool vert, uint32_t salt)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            if ((unsigned)x >= (unsigned)cv->w || (unsigned)y >= (unsigned)cv->h) continue;
            float l = vert ? (float)(y - y0) : (float)(x - x0), a = vert ? (float)(x - x0) : (float)(y - y0);
            float across = vert ? (float)w : (float)h;
            float g = sinf((a * 0.35f + mr_vnoise(l * 0.01f, a * 0.08f, salt) * 3.0f) * 3.0f);
            float k = 0.80f + 0.10f * g + (mr_hashf((int)(l * 0.2f), (int)a, salt) - 0.5f) * 0.08f;
            /* rounded: lit at the top edge, dark at the bottom */
            float t = a / across;
            k *= 0.70f + 0.55f * sinf(t * 3.14159f) * (vert ? 1.0f : 1.0f - 0.35f * t);
            if (t < 0.08f) k *= 1.15f;
            cv->px[y * cv->w + x] = mr_565(mr_rgb_scale(0x6B3A1C, k));
        }
}

static void frame_draw(aba_t *A, frm_t *F, int idx)
{
    mr_cv_t *cv = &A->bg;
    int t = (int)(A->bh * 0.42f);
    /* its shadow on the desk */
    for (int y = F->y + 10; y < F->y + F->h + 14; y++)
        for (int x = F->x + 10; x < F->x + F->w + 14; x++) mr_cv_mix(cv, x, y, 0x000000, 0.20f);
    /* the rods, behind the beads */
    for (int r = 0; r < F->nr; r++) {
        int rc = F->r[r].c;
        int len = A->vert ? F->h - t : F->w - t;
        int s0 = A->vert ? F->y + t / 2 : F->x + t / 2;
        for (int q = s0; q < s0 + len; q++)
            for (int k = -4; k <= 7; k++) {
                float v = k / 4.0f;
                int x = A->vert ? rc + k : q, y = A->vert ? q : rc + k;
                if (k > 4) mr_cv_mix(cv, x + (A->vert ? 0 : 0), y, 0x000000, k == 5 ? 0.3f : 0.15f);
                else mr_cv_mix(cv, x, y, mr_rgb_scale(0xB7BEC6, 0.55f + 0.5f * (1 - v * v) - 0.35f * v), 1.0f);
            }
    }
    wood_bar(cv, F->x, F->y, F->w, t, false, 31 + idx);
    wood_bar(cv, F->x, F->y + F->h - t, F->w, t, false, 37 + idx);
    wood_bar(cv, F->x, F->y, t, F->h, true, 41 + idx);
    wood_bar(cv, F->x + F->w - t, F->y, t, F->h, true, 43 + idx);
    /* the joints at the corners */
    for (int c = 0; c < 4; c++) {
        int cx = c & 1 ? F->x + F->w - t : F->x, cy = c & 2 ? F->y + F->h - t : F->y;
        for (int i = 0; i < t; i++) {
            mr_cv_mix(cv, cx + i, c & 2 ? cy : cy + t - 1, 0x000000, 0.35f);
        }
    }
}

static void desk_draw(mr_scene_t *s)
{
    aba_t *A = ST;
    mr_cv_t *cv = &A->bg;
    mr_field_t f;
    mr_field_new(&f, cv->w, cv->h, 8, 1.0f / 120.0f, 3, 71);
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            /* a pale painted desk, a lamp from the top left */
            float n = mr_field_at(&f, x, y), g = (mr_hashf(x, y, 72) - 0.5f) * 0.04f;
            float dx = (x - cv->w * 0.25f) / cv->w, dy = (y - cv->h * 0.2f) / cv->h;
            float light = 1.05f - 0.45f * sqrtf(dx * dx + dy * dy);
            cv->px[y * cv->w + x] = mr_565(mr_rgb_scale(0xD9CBB0, (0.92f + (n - 0.5f) * 0.10f + g) * light));
        }
    mr_field_free(&f);
    (void)s;
}

/* --- geometry ----------------------------------------------------------------------------------
 *
 * A bead's place on its rod is u, from 0 at the home end to L at the far
 * one: the counted beads are those at the far end (right when the rods lie,
 * top when they stand). */

static float u_of(aba_t *A, frm_t *F, float x, float y)
{
    return A->vert ? (float)F->u0 - y : x - (float)F->u0;
}

static void bead_put(aba_t *A, frm_t *F, rod_t *R, bead_t *b)
{
    mr_img_t *im = &A->bead[0][0];
    float sx = A->vert ? (float)R->c : F->u0 + b->x, sy = A->vert ? F->u0 - b->x : (float)R->c;
    lv_obj_set_pos(b->o, (int)(sx - im->w * 0.5f + 2), (int)(sy - im->h * 0.5f + 3));
}

static void packed(aba_t *A, frm_t *F, int counted, float *out)
{
    float bw = A->bw;
    for (int j = 0; j < NB; j++) {
        if (j < NB - counted) out[j] = bw * 0.5f + j * bw;
        else out[j] = F->L - bw * 0.5f - (NB - 1 - j) * bw;
    }
}

static void build(mr_scene_t *s)
{
    aba_t *A = (aba_t *)mr_alloc(sizeof(aba_t));
    s->st = A;
    if (!A) return;
    mr_game_t *g = s->g;
    A->n = g->np;
    A->vert = !s->land;
    int nr = 3;
    for (int p = 0; p < g->np; p++)
        if (g->p[p].score > 999) nr = 4;
    if (g->target > 999) nr = 4;
    int top = s->top + 30;
    float W = s->w - 40, H = s->h - top - 40;
    /* the frames in a grid: side by side (standing rods) or one over the other
     * (lying), two by two from three players on */
    int cols, rows;
    if (A->n <= 2) {
        cols = A->vert ? A->n : 1;
        rows = A->vert ? 1 : A->n;
    } else {
        cols = 2;
        rows = 2;
    }
    float slot_w = W / cols, slot_h = (H - (rows - 1) * 24) / rows;
    float slot_along = A->vert ? slot_h - 100 : slot_w - 170;
    float slot_cross = A->vert ? slot_w - 30 : slot_h - 40;
    float pitch = (slot_cross - 40) / (nr + 0.4f);
    float bw = mr_minf((slot_along - 40) / 13.2f, pitch * 0.74f);
    if (bw > 80) bw = 80;
    A->bw = bw;
    A->bh = bw * 1.18f;
    pitch = mr_minf(pitch, A->bh * 1.55f);
    int t = (int)(A->bh * 0.42f);
    float along = bw * 13.2f + 8;
    float cross = pitch * nr + 2 * t + 16;
    for (int p = 0; p < A->n; p++) {
        frm_t *F = &A->f[p];
        int ci = p % cols, ri = p / cols;
        float sx = 20 + ci * slot_w, sy = top + ri * (slot_h + 24);
        F->nr = nr;
        if (A->vert) {
            F->w = (int)cross;
            F->h = (int)(along + 2 * t);
            F->x = (int)(sx + (slot_w - F->w) * 0.5f);
            F->y = (int)(sy + (slot_h - 90 - F->h) * 0.5f);
            F->u0 = F->y + F->h - t - 4;
            F->L = (float)(F->h - 2 * t - 8);
            for (int r = 0; r < nr; r++) F->r[r].c = (int)(F->x + t + 8 + pitch * (r + 0.5f));
        } else {
            F->w = (int)(along + 2 * t);
            F->h = (int)cross;
            F->x = (int)(sx + (slot_w - F->w) * 0.5f) + 20;
            F->y = (int)(sy + (slot_h - F->h) * 0.5f);
            F->u0 = F->x + t + 4;
            F->L = (float)(F->w - 2 * t - 8);
            for (int r = 0; r < nr; r++) F->r[r].c = (int)(F->y + t + 8 + pitch * (r + 0.5f));
        }
        for (int r = 0; r < nr; r++) F->r[r].held = -1;
    }
    if (!mr_cv_new(&A->bg, s->w, s->h)) return;
    desk_draw(s);
    for (int p = 0; p < A->n; p++) frame_draw(A, &A->f[p], p);
    mr_cv_obj(s->obj, &A->bg);
    for (int r = 0; r < MAXR; r++) {
        for (int k = 0; k < 2; k++) {
            float iw = A->vert ? A->bh : bw, ih = A->vert ? bw : A->bh;
            mr_img_new(&A->bead[r][k], (int)(iw + 12), (int)(ih + 14));
            uint32_t c = k ? mr_rgb_mix(ROD_COL[r], 0xFFFFFF, 0.28f) : ROD_COL[r];
            bead_draw(&A->bead[r][k], iw - (A->vert ? 0 : 1), ih - (A->vert ? 1 : 0), c, 101u + (uint32_t)(r * 2 + k), A->vert);
        }
    }
    static const char *const PL[4] = { "1", "10", "100", "1000" };
    for (int p = 0; p < A->n; p++) {
        frm_t *F = &A->f[p];
        for (int r = 0; r < F->nr; r++) {
            /* the highest place first: left when standing, top when lying */
            int place = F->nr - 1 - r;
            rod_t *R = &F->r[r];
            for (int j = 0; j < NB; j++) {
                bead_t *b = &R->b[j];
                b->o = mr_img_obj(s->obj, &A->bead[place][j >= 5]);
                b->x = b->tx = bw * 0.5f + j * bw;
                bead_put(A, F, R, b);
            }
            lv_obj_t *l = mr_label(s->obj, aos_font_caption, 0x4A3A28, PL[place]);
            lv_obj_update_layout(l);
            if (A->vert) lv_obj_set_pos(l, R->c - lv_obj_get_width(l) / 2, F->y + F->h + 6);
            else lv_obj_set_pos(l, F->x - lv_obj_get_width(l) - 8, R->c - 12);
        }
        /* a brass plate with the name on the top bar, the score beside */
        lv_obj_t *pl = lv_obj_create(s->obj);
        mr_obj_plain(pl);
        lv_obj_set_style_bg_color(pl, lv_color_hex(0xD8B25A), 0);
        lv_obj_set_style_bg_grad_color(pl, lv_color_hex(0x9C7A2E), 0);
        lv_obj_set_style_bg_grad_dir(pl, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(pl, 255, 0);
        lv_obj_set_style_radius(pl, 4, 0);
        lv_obj_set_style_border_width(pl, 2, 0);
        lv_obj_set_style_border_color(pl, lv_color_hex(0x6E5520), 0);
        lv_obj_set_style_pad_hor(pl, 12, 0);
        lv_obj_set_style_pad_ver(pl, 2, 0);
        lv_obj_set_size(pl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_t *nl = mr_label(pl, aos_font_small, 0x3A2A08, g->p[p].name);
        lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_update_layout(nl);
        if (lv_obj_get_width(nl) > F->w - 40) lv_obj_set_size(nl, F->w - 40, lv_font_get_line_height(aos_font_small));
        lv_obj_update_layout(pl);
        lv_obj_set_pos(pl, F->x + F->w / 2 - lv_obj_get_width(pl) / 2, F->y - lv_obj_get_height(pl) / 2 + t / 2);
        F->score = mr_label(s->obj, aos_font_large, 0x3A2412, "0");
        lv_obj_set_style_border_side(F->score, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(F->score, lv_color_hex(g->p[p].color), 0);
        lv_obj_set_style_border_width(F->score, 5, 0);
        if (A->vert || A->n > 2) {
            lv_obj_set_width(F->score, F->w);
            lv_obj_set_style_text_align(F->score, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(F->score, F->x, F->y + F->h + 36);
        } else {
            lv_obj_set_width(F->score, 120);
            lv_obj_set_pos(F->score, F->x + F->w + 14, F->y + F->h / 2 - 30);
        }
    }
    for (int i = 0; i < MR_FINGERS; i++) A->fing[i].fr = -1;
}

static void sfree(mr_scene_t *s)
{
    aba_t *A = ST;
    if (!A) return;
    mr_cv_free(&A->bg);
    for (int r = 0; r < MAXR; r++)
        for (int k = 0; k < 2; k++) mr_img_free(&A->bead[r][k]);
}

/* --- reading and following -------------------------------------------------------------------- */

static int counted(frm_t *F, rod_t *R)
{
    int n = 0;
    for (int j = 0; j < NB; j++)
        if (R->b[j].tx > F->L * 0.5f) n++;
    return n;
}

static int32_t reading(frm_t *F)
{
    int32_t v = 0;
    for (int r = 0; r < F->nr; r++) v = v * 10 + counted(F, &F->r[r]);
    return v;
}

static void reconcile(mr_scene_t *s, bool animate)
{
    aba_t *A = ST;
    for (int p = 0; p < A->n; p++) {
        frm_t *F = &A->f[p];
        int32_t v = s->g->p[p].score;
        if (v < 0) v = 0;
        int32_t mx = F->nr == 4 ? 9999 : 999;
        if (v > mx) v = mx;
        lv_label_set_text_fmt(F->score, "%ld", (long)s->g->p[p].score);
        int32_t d = v;
        for (int r = F->nr - 1; r >= 0; r--) {
            int c = d % 10;
            d /= 10;
            rod_t *R = &F->r[r];
            if (R->held >= 0) continue;
            float t[NB];
            packed(A, F, c, t);
            for (int j = 0; j < NB; j++) {
                bead_t *b = &R->b[j];
                b->tx = t[j];
                if (!animate) {
                    b->x = t[j];
                    bead_put(A, F, R, b);
                } else if (fabsf(b->x - t[j]) > 0.5f) b->moving = true;
            }
        }
    }
}

static void sync(mr_scene_t *s, int why, int who)
{
    (void)who;
    if (!ST) return;
    reconcile(s, why != MR_SYNC_LOAD);
}

static void frame(mr_scene_t *s, float dt)
{
    aba_t *A = ST;
    if (!A) return;
    for (int p = 0; p < A->n; p++) {
        frm_t *F = &A->f[p];
        for (int r = 0; r < F->nr; r++) {
            rod_t *R = &F->r[r];
            for (int j = 0; j < NB; j++) {
                bead_t *b = &R->b[j];
                if (!b->moving || j == R->held) continue;
                float dx = b->tx - b->x;
                /* a push and the rod's friction: fast at first, then it slides in */
                float sp = mr_minf(1800.0f, fabsf(dx) * 18.0f + 200.0f);
                float step = sp * dt;
                if (fabsf(dx) <= step) {
                    b->x = b->tx;
                    b->moving = false;
                    uint32_t now = mr_ms();
                    if (now - R->last_clack > 30) {
                        R->last_clack = now;
                        mr_snd_play(MR_S_BEAD, 0.7f, 0.85f + 0.08f * (float)r + 0.05f * (float)(j % 3));
                    }
                } else {
                    b->x += dx > 0 ? step : -step;
                }
                bead_put(A, F, R, b);
            }
        }
    }
    /* a frame held without moving gives a point back */
    for (int f = 0; f < MR_FINGERS; f++) {
        mr_finger_t *Fi = &mr_A->fing[f];
        if (!Fi->id || A->fing[f].fr < 0 || A->fing[f].rod >= 0 || A->fing[f].held || Fi->moved) continue;
        if (mr_ms() - Fi->t0 > HOLD_MS) {
            A->fing[f].held = true;
            mr_add(s->g, A->fing[f].fr, -1);
        }
    }
}

/* --- fingers ------------------------------------------------------------------------------------- */

/* moving bead i of a rod to u, pushing its neighbours along */
static void push(aba_t *A, frm_t *F, rod_t *R, int i, float u)
{
    float bw = A->bw, lo = bw * 0.5f + i * bw, hi = F->L - bw * 0.5f - (NB - 1 - i) * bw;
    u = mr_clampf(u, lo, hi);
    float old = R->b[i].x;
    R->b[i].x = R->b[i].tx = u;
    for (int j = i + 1; j < NB; j++)
        if (R->b[j].x < R->b[j - 1].x + bw) {
            R->b[j].x = R->b[j].tx = R->b[j - 1].x + bw;
            R->b[j].moving = false;
        }
    for (int j = i - 1; j >= 0; j--)
        if (R->b[j].x > R->b[j + 1].x - bw) {
            R->b[j].x = R->b[j].tx = R->b[j + 1].x - bw;
            R->b[j].moving = false;
        }
    for (int j = 0; j < NB; j++) bead_put(A, F, R, &R->b[j]);
    /* beads meeting: a clack */
    if (fabsf(u - old) > 1) {
        bool touch = (i + 1 < NB && R->b[i + 1].x - R->b[i].x < bw + 0.6f && u > old) ||
                     (i > 0 && R->b[i].x - R->b[i - 1].x < bw + 0.6f && u < old);
        uint32_t now = mr_ms();
        if (touch && now - R->last_clack > 120) {
            R->last_clack = now;
            mr_snd_play(MR_S_BEAD, 0.5f, 1.1f);
        }
    }
}

static void down(mr_scene_t *s, int f, float x, float y)
{
    aba_t *A = ST;
    if (!A) return;
    (void)s;
    A->fing[f].fr = -1;
    A->fing[f].rod = -1;
    A->fing[f].held = false;
    for (int p = 0; p < A->n; p++) {
        frm_t *F = &A->f[p];
        if (x < F->x - 20 || x > F->x + F->w + 20 || y < F->y - 10 || y > F->y + F->h + 10) continue;
        A->fing[f].fr = p;
        float u = u_of(A, F, x, y), c = A->vert ? x : y;
        for (int r = 0; r < F->nr; r++) {
            rod_t *R = &F->r[r];
            if (fabsf(c - R->c) > A->bh * 0.65f || R->held >= 0) continue;
            int best = -1;
            float bd = A->bw * 0.75f;
            for (int j = 0; j < NB; j++) {
                float d = fabsf(R->b[j].x - u);
                if (d < bd) {
                    bd = d;
                    best = j;
                }
            }
            if (best >= 0) {
                R->held = best;
                R->finger = f;
                R->grab = u - R->b[best].x;
                A->fing[f].rod = r;
            }
        }
        return;
    }
}

static void move(mr_scene_t *s, int f, float x, float y)
{
    aba_t *A = ST;
    (void)s;
    if (!A || A->fing[f].fr < 0 || A->fing[f].rod < 0) return;
    frm_t *F = &A->f[A->fing[f].fr];
    rod_t *R = &F->r[A->fing[f].rod];
    if (R->held < 0) return;
    push(A, F, R, R->held, u_of(A, F, x, y) - R->grab);
}

static void up(mr_scene_t *s, int f, float x, float y)
{
    aba_t *A = ST;
    if (!A) return;
    int p = A->fing[f].fr, r = A->fing[f].rod;
    bool held = A->fing[f].held;
    A->fing[f].fr = -1;
    A->fing[f].rod = -1;
    if (p < 0) return;
    frm_t *F = &A->f[p];
    if (r >= 0) {
        rod_t *R = &F->r[r];
        bool dragged = mr_A->fing[f].moved;
        int j = R->held;
        R->held = -1;
        if (j < 0) return;
        int nc;
        if (!dragged) {
            /* a tap on a bead: it and those beyond it cross to the other side */
            nc = R->b[j].x > F->L * 0.5f ? NB - j - 1 : NB - j;
        } else {
            /* let go: every bead runs to the end it is nearer */
            nc = 0;
            for (int k = 0; k < NB; k++)
                if (R->b[k].x > F->L * 0.5f) nc++;
        }
        float t[NB];
        packed(A, F, nc, t);
        for (int k = 0; k < NB; k++) {
            R->b[k].tx = t[k];
            if (fabsf(R->b[k].x - t[k]) > 0.5f) R->b[k].moving = true;
        }
        int32_t v = reading(F);
        if (v != s->g->p[p].score) mr_add(s->g, p, (int)(v - s->g->p[p].score));
        return;
    }
    if (held || x < -1000 || mr_A->fing[f].moved) return;
    (void)y;
    mr_add(s->g, p, 1);
}

const mr_scene_ops_t mr_abaco_ops = { build, sfree, sync, frame, down, move, up, NULL };
