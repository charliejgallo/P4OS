/*
 * Marcador - the green felt with matches.
 *
 * How truco is scored at a table: every point is a match laid on the felt,
 * and five make a square: the left side, the top, the right side, the bottom
 * and the diagonal across it. Fifteen are the "malas" and fifteen more the
 * "buenas" (to thirty), or any number in squares of five for other games.
 *
 * The felt is drawn once: green cloth with its nap and mottling, light from
 * above and the edges falling off into the dark, and a stitched seam
 * around. A match is a little lit object: a wooden stick, lit across its
 * round section from the top left whatever way it lies, a red head with its
 * shine, and the soft shadow it throws. Each of the five places of a square
 * has its own way of lying (heads going round the square) and three
 * slightly crooked variants, so a column of squares looks laid by hand.
 *
 * Taking a point: a match leaves the box (which rattles), flies in an arc
 * turning to its place while its shadow falls behind it, and lands with a
 * soft tick on the cloth. Giving one back is the same the other way.
 *
 * Fingers: tap a player's side to add a match, hold to take one back; the
 * small buttons do the same. Several players can tap at once.
 */
#include "mr_app.h"

#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAXM     60         /* matches shown per player (twelve squares)   */
#define VARS     3
#define FLIGHTS  8
#define FLY_S    0.42f
#define HOLD_MS  520

typedef struct {
    bool     on;
    int      p, k;          /* player, match index                         */
    bool     in;            /* to the felt (else back to the box)          */
    float    t, delay;
    float    x0, y0, a0, x1, y1, a1;
    lv_obj_t *img, *sh;
} flight_t;

typedef struct {
    float x, y, w, h;       /* the area of the squares                      */
} zone_t;

typedef struct {
    mr_cv_t   bg;
    lv_obj_t *bgo;
    mr_img_t  spr[5][VARS]; /* the five places of a square                 */
    mr_img_t  fly, flysh, box;
    float     L, T;         /* match length and thickness                  */
    float     side;         /* the square                                  */
    float     boxx, boxy;   /* the drawer's mouth, where matches come from */
    int       np;
    struct {
        int       x, y, w, h;       /* the player's part of the felt       */
        zone_t    z[2];
        int       nz;
        int       cap;              /* matches that fit                     */
        float     mx[MAXM], my[MAXM];
        uint8_t   kind[MAXM], var[MAXM];
        lv_obj_t *m[MAXM];
        int       shown;            /* matches on the felt (or landing)     */
        lv_obj_t *name, *count, *dot;
        lv_obj_t *lab[2];
    } c[4];
    flight_t  fl[FLIGHTS];
    struct {
        int      p;
        bool     held;
    } fing[MR_FINGERS];
    uint32_t  seed;
} pano_t;

#define ST ((pano_t *)s->st)

static void mr_pano_btn(mr_scene_t *s, int arg);

/* --- drawing ---------------------------------------------------------------------- */

static void felt_draw(mr_cv_t *cv, uint32_t salt)
{
    int w = cv->w, h = cv->h;
    mr_field_t mot, big;
    mr_field_new(&mot, w, h, 8, 1.0f / 90.0f, 4, salt);
    mr_field_new(&big, w, h, 16, 1.0f / 420.0f, 3, salt + 7);
    const uint32_t base = 0x1C6A3A;
    float cx = w * 0.5f, cy = h * 0.42f, R = sqrtf(cx * cx + cy * cy) * 1.05f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float m = mr_field_at(&mot, x, y), b = mr_field_at(&big, x, y);
            /* the nap: fibres a few pixels long, mostly up and down */
            uint32_t hs = mr_hash((uint32_t)x * 2654435761u ^ (uint32_t)(y / 3) * 40503u ^ salt);
            float fib = ((float)(hs & 255) / 255.0f - 0.5f) * 0.10f;
            float grain = ((float)((mr_hash((uint32_t)(x + y * w) ^ salt) >> 4) & 255) / 255.0f - 0.5f) * 0.07f;
            float dx = x - cx, dy = y - cy;
            float v = 1.0f - 0.55f * mr_smooth(0.35f, 1.0f, sqrtf(dx * dx + dy * dy) / R);
            float k = (0.86f + (m - 0.5f) * 0.20f + (b - 0.5f) * 0.18f + fib + grain) * v *
                      (1.06f - 0.12f * (float)y / (float)h);
            cv->px[y * w + x] = mr_565(mr_rgb_scale(base, k));
        }
    }
    mr_field_free(&mot);
    mr_field_free(&big);
    /* the seam: a groove and the stitches along it */
    int in = 22;
    for (int side = 0; side < 4; side++) {
        int len = side < 2 ? w - 2 * in : h - 2 * in;
        for (int i = 0; i < len; i++) {
            int x, y, nx, ny;
            if (side == 0) { x = in + i; y = in; nx = 0; ny = 1; }
            else if (side == 1) { x = in + i; y = h - in; nx = 0; ny = -1; }
            else if (side == 2) { x = in; y = in + i; nx = 1; ny = 0; }
            else { x = w - in; y = in + i; nx = -1; ny = 0; }
            mr_cv_mix(cv, x - nx, y - ny, 0x000000, 0.35f);
            mr_cv_mix(cv, x, y, 0x000000, 0.22f);
            mr_cv_mix(cv, x + nx, y + ny, 0x8FD0A0, 0.10f);
            bool stitch = (i % 14) < 8;
            if (stitch) {
                mr_cv_mix(cv, x + nx * 5, y + ny * 5, 0xC9E3B8, 0.42f);
                mr_cv_mix(cv, x + nx * 6, y + ny * 6, 0xC9E3B8, 0.30f);
                mr_cv_mix(cv, x + nx * 7, y + ny * 7, 0x000000, 0.12f);
            }
        }
    }
}

/* A match lying at 'ang' degrees (0: head to the right, y down), lit from
 * the top left across its round section. */
static void match_draw(mr_img_t *im, float L, float T, float ang, uint32_t seed, bool shadow)
{
    float a = ang * 3.14159265f / 180.0f, ca = cosf(a), sa = sinf(a);
    float cx = im->w * 0.5f, cy = im->h * 0.5f;
    float r = T * 0.5f, hl = L * 0.15f, hr = T * 0.70f;
    float hu = L * 0.5f - hl * 0.5f;
    const float lx = -0.45f, ly = -0.62f, lz = 0.64f;
    uint32_t sd = seed;
    float tint = 0.94f + 0.10f * mr_rnd(&sd);
    float gph = mr_rnd(&sd) * 30.0f;
    for (int y = 0; y < im->h; y++) {
        for (int x = 0; x < im->w; x++) {
            float px = (float)x + 0.5f - cx, py = (float)y + 0.5f - cy;
            float u = px * ca + py * sa, v = -px * sa + py * ca;
            /* the head: an egg at the +u end */
            float du = (u - hu) / (hl * 0.5f + 0.6f), dv = v / hr;
            float dh = du * du + dv * dv;
            if (dh <= 1.0f) {
                float edge = mr_clampf((1.0f - dh) * hr * 1.2f, 0, 1);
                float nz = sqrtf(1.0f - dh), nu = du * 0.7f, nv = dv;
                float nx = nu * ca - nv * sa, ny = nu * sa + nv * ca;
                float dif = mr_clampf(nx * lx + ny * ly + nz * lz, 0, 1);
                float spec = powf(mr_clampf(nx * -0.3f + ny * -0.5f + nz * 0.81f, 0, 1), 18.0f);
                /* the striking tip is a darker cap */
                float tip = mr_smooth(0.15f, 0.9f, du) * 0.35f;
                uint32_t c = mr_rgb_scale(0xA8301F, (0.42f + 0.75f * dif) * (1.0f - tip));
                c = mr_rgb_mix(c, 0xFFE2D0, spec * 0.75f);
                /* the grain of the paste */
                c = mr_rgb_scale(c, 0.93f + 0.14f * mr_hashf(x, y, seed));
                mr_img_over(im, x, y, c, edge);
                continue;
            }
            /* the stick: square-cut at the back end, round across */
            float half = L * 0.5f;
            if (u < -half - 1 || u > hu || fabsf(v) > r + 1) continue;
            float cov = mr_clampf(r + 0.5f - fabsf(v), 0, 1) * mr_clampf(u + half + 0.5f, 0, 1);
            if (cov <= 0) continue;
            float nv = mr_clampf(v / r, -1, 1), nz = sqrtf(1.0f - nv * nv);
            float nx = -nv * sa, ny = nv * ca;
            float dif = mr_clampf(nx * lx + ny * ly + nz * lz, 0, 1);
            /* the grain runs along the stick */
            float grain = 0.93f + 0.07f * sinf(v * 2.4f + gph + 1.3f * sinf(u * 0.07f + gph)) +
                          0.04f * (mr_hashf((int)(u * 0.5f), (int)(v * 3 + 40), seed) - 0.5f);
            uint32_t c = mr_rgb_scale(0xE6C68C, (0.50f + 0.62f * dif) * grain * tint);
            /* the cut end shows the darker end grain */
            if (u < -half + 1.5f) c = mr_rgb_scale(c, 0.78f);
            /* a little of the head's colour soaks into the stick under it */
            c = mr_rgb_mix(c, 0x7A2A18, mr_smooth(hu - hl * 0.9f, hu - hl * 0.4f, u) * 0.5f);
            mr_img_over(im, x, y, c, cov);
        }
    }
    if (shadow) mr_img_shadow(im, 3, 4, 3, 0.55f);
}

/* the shadow of a match in the air, on its own: a blurred dark bar */
static void match_shadow_draw(mr_img_t *im, float L, float T)
{
    float cx = im->w * 0.5f, cy = im->h * 0.5f;
    mr_img_capsule(im, cx - L * 0.5f, cy, cx + L * 0.5f, cy, T * 0.6f, 0x000000, 0.42f);
    mr_img_blur(im, 4);
}

/* the box of matches, from above: the sleeve with its label and striking
 * strips, the drawer pushed half out with the heads in a row */
static void box_draw(mr_img_t *im, float L)
{
    float W = L * 1.12f, H = L * 0.62f;
    float x0 = (im->w - W * 1.38f) * 0.5f + 4, y0 = (im->h - H) * 0.5f - 4;
    float dx = W * 0.40f;  /* how far the drawer is out, to the right */
    /* the drawer: cardboard tray with matches */
    float tx = x0 + dx, ty = y0 + H * 0.06f, tw = W, th = H * 0.88f;
    mr_img_rrect(im, tx, ty, tw, th, 4, 0xC9A774, 1.0f);
    mr_img_rrect(im, tx + 5, ty + 5, tw - 10, th - 10, 3, 0xB89263, 1.0f);
    int rows = 6;
    for (int i = 0; i < rows; i++) {
        float my = ty + 10 + (th - 20) * ((float)i + 0.5f) / (float)rows;
        float jit = (float)((mr_hash((uint32_t)i * 77u) & 7)) - 3.5f;
        float mx1 = tx + tw - 8 - jit;
        mr_img_capsule(im, tx + 10, my, mx1 - L * 0.08f, my, L * 0.036f, 0xE0BE84, 1.0f);
        mr_img_capsule(im, tx + 10, my + 1.5f, mx1 - L * 0.08f, my + 1.5f, L * 0.02f, 0x8E6E44, 0.35f);
        mr_img_disc(im, mx1 - L * 0.06f, my, L * 0.040f, 0x9E2B1C, 1.0f);
        mr_img_disc(im, mx1 - L * 0.07f, my - L * 0.014f, L * 0.012f, 0xF0B5A0, 0.6f);
    }
    /* the sleeve */
    mr_img_rrect(im, x0, y0, W, H, 5, 0xEFE6CE, 1.0f);
    /* striking strips on the long sides */
    for (int sd = 0; sd < 2; sd++) {
        float sy = sd ? y0 + H - H * 0.11f : y0;
        mr_img_rrect(im, x0, sy, W, H * 0.11f, 3, 0x5A3B26, 1.0f);
        for (int k = 0; k < 500; k++) {
            uint32_t hs = mr_hash((uint32_t)k * 31u + (uint32_t)sd * 977u);
            float gx = x0 + 3 + (float)(hs % 1000) / 1000.0f * (W - 6);
            float gy = sy + 2 + (float)((hs >> 10) % 100) / 100.0f * (H * 0.11f - 4);
            mr_img_over(im, (int)gx, (int)gy, (hs >> 20) & 1 ? 0x8A6A4E : 0x2E1C12, 0.8f);
        }
    }
    /* the label: a band, a flame in a ring, little lines of "text" */
    float lx = x0 + W * 0.08f, ly = y0 + H * 0.17f, lw = W * 0.84f, lh = H * 0.66f;
    mr_img_rrect(im, lx, ly, lw, lh, 4, 0x1F4E8C, 1.0f);
    mr_img_rrect(im, lx + 4, ly + 4, lw - 8, lh - 8, 3, 0xF3EBD5, 1.0f);
    mr_img_rrect(im, lx + 4, ly + lh * 0.40f, lw - 8, lh * 0.22f, 0, 0xC8392B, 1.0f);
    float fcx = lx + lw * 0.22f, fcy = ly + lh * 0.5f, fr = lh * 0.34f;
    mr_img_disc(im, fcx, fcy, fr, 0x1F4E8C, 1.0f);
    mr_img_disc(im, fcx, fcy, fr - 3, 0xF3EBD5, 1.0f);
    mr_img_disc(im, fcx, fcy + fr * 0.25f, fr * 0.45f, 0xF2A33A, 1.0f);
    mr_img_disc(im, fcx, fcy - fr * 0.05f, fr * 0.30f, 0xF2A33A, 1.0f);
    mr_img_disc(im, fcx, fcy - fr * 0.30f, fr * 0.16f, 0xF2A33A, 1.0f);
    mr_img_disc(im, fcx, fcy + fr * 0.28f, fr * 0.22f, 0xFFE07A, 1.0f);
    for (int i = 0; i < 3; i++) {
        float yy = ly + lh * (i == 1 ? 0.51f : i == 0 ? 0.24f : 0.78f);
        float ww = lw * (i == 1 ? 0.48f : 0.40f);
        mr_img_capsule(im, lx + lw * 0.42f, yy, lx + lw * 0.42f + ww, yy, i == 1 ? 3.5f : 2.0f,
                       i == 1 ? 0xF3EBD5 : 0x1F4E8C, 0.9f);
    }
    /* the sleeve's edge where the drawer comes out */
    mr_img_capsule(im, x0 + W - 1, y0 + 2, x0 + W - 1, y0 + H - 2, 1.5f, 0x7A6A50, 0.7f);
    mr_img_shadow(im, 5, 7, 6, 0.55f);
}

/* --- layout ------------------------------------------------------------------------- */

static void grid_fit(float w, float h, int n, float gap, int *cols, int *rows, float *side)
{
    float best = 0;
    for (int c = 1; c <= n; c++) {
        int r = (n + c - 1) / c;
        float sw = (w - gap * (float)(c + 1)) / (float)c, sh = (h - gap * (float)(r + 1)) / (float)r;
        float sd = sw < sh ? sw : sh;
        if (sd > best) {
            best = sd;
            *cols = c;
            *rows = r;
        }
    }
    *side = best;
}

/* where every match of player p goes */
static void place(mr_scene_t *s, int p, float side_cap)
{
    pano_t *P = ST;
    mr_game_t *g = s->g;
    int target = g->target > 0 ? g->target : 30;
    int nsq = (target + 4) / 5;
    if (nsq > MAXM / 5) nsq = MAXM / 5;
    bool split = g->opt[MR_PANO_SPLIT] && nsq >= 2;
    int x = P->c[p].x, y = P->c[p].y + 112, w = P->c[p].w, h = P->c[p].h - 112 - 76;
    P->c[p].nz = split ? 2 : 1;
    int per[2] = { split ? (nsq + 1) / 2 : nsq, split ? nsq / 2 : 0 };
    bool side_by_side = split && w > h * 0.9f;
    float lab = split ? 34.0f : 0.0f;
    for (int z = 0; z < P->c[p].nz; z++) {
        zone_t *Z = &P->c[p].z[z];
        if (!split) *Z = (zone_t){ (float)x, (float)y, (float)w, (float)h };
        else if (side_by_side) *Z = (zone_t){ x + z * w * 0.5f, (float)y + lab, w * 0.5f, h - lab };
        else *Z = (zone_t){ (float)x, y + z * h * 0.5f + lab, (float)w, h * 0.5f - lab };
    }
    int k = 0;
    for (int z = 0; z < P->c[p].nz; z++) {
        zone_t *Z = &P->c[p].z[z];
        int cols = 1, rows = 1;
        float sd;
        grid_fit(Z->w, Z->h, per[z], side_cap * 0.28f, &cols, &rows, &sd);
        if (sd > side_cap) sd = side_cap;
        float gap = sd * 0.30f;
        float gw = cols * sd + (cols - 1) * gap, gh = rows * sd + (rows - 1) * gap;
        float ox = Z->x + (Z->w - gw) * 0.5f, oy = Z->y + (Z->h - gh) * 0.5f;
        for (int q = 0; q < per[z]; q++) {
            int cx_i = q % cols, cy_i = q / cols;
            float cx = ox + cx_i * (sd + gap) + sd * 0.5f, cy = oy + cy_i * (sd + gap) + sd * 0.5f;
            static const float DX[5] = { -0.5f, 0, 0.5f, 0, 0 }, DY[5] = { 0, -0.5f, 0, 0.5f, 0 };
            for (int m = 0; m < 5 && k < MAXM; m++, k++) {
                uint32_t hs = mr_hash((uint32_t)(p * 1000 + k) * 2654435761u);
                P->c[p].mx[k] = cx + DX[m] * sd + (float)((int)(hs & 7) - 3) * 0.6f;
                P->c[p].my[k] = cy + DY[m] * sd + (float)((int)((hs >> 3) & 7) - 3) * 0.6f;
                P->c[p].kind[k] = (uint8_t)m;
                P->c[p].var[k] = (uint8_t)((hs >> 8) % VARS);
            }
        }
        if (split && P->c[p].lab[z]) {
            lv_obj_set_pos(P->c[p].lab[z], (int)(Z->x + (Z->w - gw) * 0.5f), (int)(Z->y - lab + 2));
        }
    }
    P->c[p].cap = k;
}

static const float KIND_ANG[5] = { -90, 0, 90, 180, -45 };

static void sprites_make(pano_t *P)
{
    float L = P->L, T = P->T;
    for (int k = 0; k < 5; k++) {
        for (int v = 0; v < VARS; v++) {
            int sz = (int)(L + 24);
            if (k == 0 || k == 2) mr_img_new(&P->spr[k][v], (int)(T * 3 + 18), sz);
            else if (k == 1 || k == 3) mr_img_new(&P->spr[k][v], sz, (int)(T * 3 + 18));
            else mr_img_new(&P->spr[k][v], (int)(L * 0.75f + 26), (int)(L * 0.75f + 26));
            float jit = (v == 0 ? 0.0f : v == 1 ? 2.2f : -2.6f);
            match_draw(&P->spr[k][v], L, T, KIND_ANG[k] + jit, 0x1234u + (uint32_t)(k * 7 + v), true);
        }
    }
    int fs = (int)(L + 30);
    mr_img_new(&P->fly, fs, (int)(T * 3 + 10));
    match_draw(&P->fly, L, T, 0, 0x777u, false);
    mr_img_new(&P->flysh, fs, (int)(T * 4 + 20));
    match_shadow_draw(&P->flysh, L, T);
    int bw = (int)(L * 1.12f * 1.42f + 30), bh = (int)(L * 0.62f + 34);
    mr_img_new(&P->box, bw, bh);
    box_draw(&P->box, L);
}

static void obj_center(lv_obj_t *o, const mr_img_t *im, float x, float y)
{
    lv_obj_set_pos(o, (int)(x - im->w * 0.5f), (int)(y - im->h * 0.5f));
}

static void count_text(mr_scene_t *s, int p)
{
    pano_t *P = ST;
    mr_game_t *g = s->g;
    int sc = g->p[p].score;
    char buf[64];
    int half = g->target / 2;
    if (g->opt[MR_PANO_SPLIT] && g->target >= 10) {
        if (sc <= half) snprintf(buf, sizeof buf, "%d  %s", sc, _("malas"));
        else snprintf(buf, sizeof buf, "%d  %s  (%d)", sc, _("buenas"), sc - half);
    } else {
        snprintf(buf, sizeof buf, "%d", sc);
    }
    lv_label_set_text(P->c[p].count, buf);
}

static void build(mr_scene_t *s)
{
    pano_t *P = (pano_t *)mr_alloc(sizeof(pano_t));
    s->st = P;
    if (!P) return;
    mr_game_t *g = s->g;
    P->np = g->np;
    P->seed = 99;
    /* where the players sit */
    int top = s->top + 4, boxh = s->land ? 0 : 150;
    int W = s->w, H = s->h - top - boxh;
    int cols = P->np, rows = 1;
    if (!s->land && P->np == 4) {
        cols = 2;
        rows = 2;
    }
    int bx_strip = s->land ? 0 : 0;
    (void)bx_strip;
    int cw = (W - 24) / cols, ch = (H - 8) / rows;
    for (int p = 0; p < P->np; p++) {
        P->c[p].x = 12 + (p % cols) * cw;
        P->c[p].y = top + (p / cols) * ch;
        P->c[p].w = cw;
        P->c[p].h = ch;
    }
    if (s->land) {
        /* lying down the box sits at the bottom of the middle; the columns
         * leave it the room */
        for (int p = 0; p < P->np; p++) P->c[p].h = H - 8;
    }
    /* the size of a square: what the tightest column allows, the same for all */
    float side = 1e9f;
    int target = g->target > 0 ? g->target : 30;
    int nsq = (target + 4) / 5;
    if (nsq > MAXM / 5) nsq = MAXM / 5;
    for (int p = 0; p < P->np; p++) {
        int cols2, rows2;
        float sd;
        bool split = g->opt[MR_PANO_SPLIT] && nsq >= 2;
        float w = P->c[p].w, h = P->c[p].h - 112 - 76 - (split ? 68 : 0);
        if (split && w > h * 0.9f) {
            grid_fit(w * 0.5f, h, (nsq + 1) / 2, 30, &cols2, &rows2, &sd);
        } else if (split) {
            grid_fit(w, h * 0.5f, (nsq + 1) / 2, 30, &cols2, &rows2, &sd);
        } else {
            grid_fit(w, h, nsq, 30, &cols2, &rows2, &sd);
        }
        if (sd < side) side = sd;
    }
    if (side > 170) side = 170;
    if (side < 40) side = 40;
    P->side = side;
    P->L = side * 0.98f;
    P->T = P->L * 0.082f;
    if (P->T < 4) P->T = 4;

    if (!mr_cv_new(&P->bg, s->w, s->h)) return;
    felt_draw(&P->bg, 0xF317u);
    /* a faint seam between the players */
    for (int p = 1; p < P->np; p++) {
        if (!s->land && P->np == 4 && p == 2) {
            int y = P->c[2].y - 4;
            for (int x = 40; x < s->w - 40; x++) mr_cv_mix(&P->bg, x, y, 0x000000, 0.18f);
            continue;
        }
        if (P->c[p].x == P->c[0].x) continue;
        int x = P->c[p].x;
        for (int y = P->c[p].y + 20; y < P->c[p].y + P->c[p].h - 20; y++) {
            mr_cv_mix(&P->bg, x - 1, y, 0x000000, 0.20f);
            mr_cv_mix(&P->bg, x, y, 0x9FD8AE, 0.06f);
        }
    }
    P->bgo = mr_cv_obj(s->obj, &P->bg);
    sprites_make(P);

    /* the box: under the squares, between the players */
    lv_obj_t *bo = mr_img_obj(s->obj, &P->box);
    float bxc = s->w * 0.5f, byc = s->land ? s->h - P->box.h * 0.5f - 18 : s->h - boxh * 0.5f - 6;
    if (s->land && P->np == 2) {
        bxc = s->w * 0.5f;
        byc = s->h - P->box.h * 0.5f - 10;
    }
    obj_center(bo, &P->box, bxc, byc);
    P->boxx = bxc + P->L * 0.45f;
    P->boxy = byc;

    for (int p = 0; p < P->np; p++) {
        /* name and count, then the squares under them */
        P->c[p].dot = lv_obj_create(s->obj);
        mr_obj_plain(P->c[p].dot);
        lv_obj_set_size(P->c[p].dot, 22, 22);
        lv_obj_set_style_radius(P->c[p].dot, 11, 0);
        lv_obj_set_style_bg_color(P->c[p].dot, lv_color_hex(g->p[p].color), 0);
        lv_obj_set_style_bg_opa(P->c[p].dot, 255, 0);
        lv_obj_set_style_border_color(P->c[p].dot, lv_color_hex(0xF3EBD5), 0);
        lv_obj_set_style_border_width(P->c[p].dot, 2, 0);
        P->c[p].name = mr_label(s->obj, aos_font_title, 0xF3EBD5, g->p[p].name);
        lv_label_set_long_mode(P->c[p].name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(P->c[p].name, P->c[p].w - 80, lv_font_get_line_height(aos_font_title));
        lv_obj_set_style_text_align(P->c[p].name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(P->c[p].name, P->c[p].x + 50, P->c[p].y + 14);
        lv_obj_set_pos(P->c[p].dot, P->c[p].x + 34, P->c[p].y + 26);
        P->c[p].count = mr_label(s->obj, aos_font_body, 0xCFE6C8, "");
        lv_obj_set_width(P->c[p].count, P->c[p].w);
        lv_obj_set_style_text_align(P->c[p].count, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(P->c[p].count, P->c[p].x, P->c[p].y + 66);
        if (g->opt[MR_PANO_SPLIT]) {
            for (int z = 0; z < 2; z++) {
                P->c[p].lab[z] = mr_label(s->obj, aos_font_caption, 0xBFDDB8, z ? _("BUENAS") : _("MALAS"));
                lv_obj_set_style_text_letter_space(P->c[p].lab[z], 3, 0);
                lv_obj_set_style_opa(P->c[p].lab[z], 170, 0);
            }
        }
        place(s, p, side);
        for (int k = 0; k < P->c[p].cap; k++) {
            mr_img_t *im = &P->spr[P->c[p].kind[k]][P->c[p].var[k]];
            P->c[p].m[k] = mr_img_obj(s->obj, im);
            obj_center(P->c[p].m[k], im, P->c[p].mx[k], P->c[p].my[k]);
            lv_obj_add_flag(P->c[p].m[k], LV_OBJ_FLAG_HIDDEN);
        }
        /* the little buttons at the bottom of the column */
        int by = P->c[p].y + P->c[p].h - 70;
        int bxm = P->c[p].x + P->c[p].w / 2;
        if (!s->land && P->np <= 3 && p < P->np && rows == 1) by = P->c[p].y + P->c[p].h - 70;
        mr_rbtn(s, bxm - 116, by, 104, 60, LV_SYMBOL_MINUS, 0x0E3A20, 0xF3EBD5, mr_pano_btn, -(p + 1));
        mr_rbtn(s, bxm + 12, by, 104, 60, LV_SYMBOL_PLUS, 0x0E3A20, 0xF3EBD5, mr_pano_btn, p + 1);
    }
    for (int i = 0; i < FLIGHTS; i++) {
        P->fl[i].sh = mr_img_obj(s->obj, &P->flysh);
        P->fl[i].img = mr_img_obj(s->obj, &P->fly);
        lv_image_set_pivot(P->fl[i].img, P->fly.w / 2, P->fly.h / 2);
        lv_image_set_pivot(P->fl[i].sh, P->flysh.w / 2, P->flysh.h / 2);
        lv_obj_add_flag(P->fl[i].img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(P->fl[i].sh, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < MR_FINGERS; i++) P->fing[i].p = -1;
}

static void sfree(mr_scene_t *s)
{
    pano_t *P = ST;
    if (!P) return;
    mr_cv_free(&P->bg);
    for (int k = 0; k < 5; k++)
        for (int v = 0; v < VARS; v++) mr_img_free(&P->spr[k][v]);
    mr_img_free(&P->fly);
    mr_img_free(&P->flysh);
    mr_img_free(&P->box);
}

/* --- motion -------------------------------------------------------------------------- */

static float ease(float t)
{
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

static float ang_to(float a0, float a1)
{
    float d = fmodf(a1 - a0 + 540.0f, 360.0f) - 180.0f;
    return a0 + d;
}

static void launch(mr_scene_t *s, int p, int k, bool in, float delay)
{
    pano_t *P = ST;
    flight_t *f = NULL;
    for (int i = 0; i < FLIGHTS; i++)
        if (!P->fl[i].on) {
            f = &P->fl[i];
            break;
        }
    if (!f) {
        /* too many at once: it just appears or goes */
        if (in) lv_obj_remove_flag(P->c[p].m[k], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(P->c[p].m[k], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    f->on = true;
    f->p = p;
    f->k = k;
    f->in = in;
    f->t = 0;
    f->delay = delay;
    float bx = P->boxx + (mr_rnd(&P->seed) - 0.5f) * 8, by = P->boxy + (mr_rnd(&P->seed) - 0.5f) * P->L * 0.3f;
    float jit = P->c[p].var[k] == 0 ? 0.0f : P->c[p].var[k] == 1 ? 2.2f : -2.6f;
    float mx = P->c[p].mx[k], my = P->c[p].my[k];
    /* the sprite's centre is the image's: the stick's middle */
    if (in) {
        f->x0 = bx; f->y0 = by; f->a0 = 0;
        f->x1 = mx; f->y1 = my; f->a1 = ang_to(0, KIND_ANG[P->c[p].kind[k]] + jit);
    } else {
        f->x0 = mx; f->y0 = my; f->a0 = KIND_ANG[P->c[p].kind[k]] + jit;
        f->x1 = bx; f->y1 = by; f->a1 = ang_to(f->a0, 0);
        lv_obj_add_flag(P->c[p].m[k], LV_OBJ_FLAG_HIDDEN);
    }
    if (delay <= 0) {
        mr_snd_play(in ? MR_S_BOX : MR_S_MATCH_OFF, 0.7f, 0.95f + 0.1f * mr_rnd(&P->seed));
    }
}

static void frame(mr_scene_t *s, float dt)
{
    pano_t *P = ST;
    if (!P) return;
    for (int i = 0; i < FLIGHTS; i++) {
        flight_t *f = &P->fl[i];
        if (!f->on) continue;
        if (f->delay > 0) {
            f->delay -= dt;
            if (f->delay <= 0)
                mr_snd_play(f->in ? MR_S_BOX : MR_S_MATCH_OFF, 0.6f, 0.95f + 0.1f * mr_rnd(&P->seed));
            continue;
        }
        f->t += dt / FLY_S;
        if (f->t >= 1.0f) {
            f->on = false;
            lv_obj_add_flag(f->img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(f->sh, LV_OBJ_FLAG_HIDDEN);
            if (f->in) {
                lv_obj_remove_flag(P->c[f->p].m[f->k], LV_OBJ_FLAG_HIDDEN);
                mr_snd_play(MR_S_MATCH_ON, 0.9f, 0.9f + 0.2f * mr_rnd(&P->seed));
            } else {
                mr_snd_play(MR_S_BEAN_SOFT, 0.3f, 0.7f);
            }
            continue;
        }
        float e = ease(f->t), hgt = sinf(3.14159f * f->t);
        float x = f->x0 + (f->x1 - f->x0) * e, y = f->y0 + (f->y1 - f->y0) * e - hgt * P->L * 0.45f;
        float a = f->a0 + (f->a1 - f->a0) * mr_smooth(0.0f, 0.85f, f->t);
        int sc = 256 + (int)(hgt * 46);
        lv_obj_remove_flag(f->img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(f->sh, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_rotation(f->img, (int)(a * 10));
        lv_image_set_rotation(f->sh, (int)(a * 10));
        lv_image_set_scale(f->img, (uint32_t)sc);
        lv_image_set_scale(f->sh, (uint32_t)(256 + hgt * 70));
        obj_center(f->img, &P->fly, x, y);
        /* the shadow stays on the cloth, further off as the match rises */
        float gy = f->y0 + (f->y1 - f->y0) * e;
        obj_center(f->sh, &P->flysh, x + 4 + hgt * 26, gy + 5 + hgt * 34);
        lv_obj_set_style_opa(f->sh, (lv_opa_t)(255 - hgt * 120), 0);
    }
}

/* the felt follows the score */
static void reconcile(mr_scene_t *s, bool animate)
{
    pano_t *P = ST;
    mr_game_t *g = s->g;
    for (int p = 0; p < P->np; p++) {
        int want = g->p[p].score;
        if (want > P->c[p].cap) want = P->c[p].cap;
        if (want < 0) want = 0;
        float d = 0;
        while (P->c[p].shown < want) {
            int k = P->c[p].shown++;
            if (animate) {
                launch(s, p, k, true, d);
                d += 0.11f;
            } else lv_obj_remove_flag(P->c[p].m[k], LV_OBJ_FLAG_HIDDEN);
        }
        d = 0;
        while (P->c[p].shown > want) {
            int k = --P->c[p].shown;
            /* a match on its way in that is called back just goes */
            for (int i = 0; i < FLIGHTS; i++)
                if (P->fl[i].on && P->fl[i].in && P->fl[i].p == p && P->fl[i].k == k) {
                    P->fl[i].on = false;
                    lv_obj_add_flag(P->fl[i].img, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(P->fl[i].sh, LV_OBJ_FLAG_HIDDEN);
                }
            if (animate) {
                launch(s, p, k, false, d);
                d += 0.09f;
            } else lv_obj_add_flag(P->c[p].m[k], LV_OBJ_FLAG_HIDDEN);
        }
        count_text(s, p);
    }
}

static void sync(mr_scene_t *s, int why, int who)
{
    (void)who;
    if (!ST) return;
    reconcile(s, why != MR_SYNC_LOAD);
}

/* --- fingers ------------------------------------------------------------------------- */

static int who_at(mr_scene_t *s, float x, float y)
{
    pano_t *P = ST;
    for (int p = 0; p < P->np; p++)
        if (x >= P->c[p].x && x < P->c[p].x + P->c[p].w && y >= P->c[p].y && y < P->c[p].y + P->c[p].h) return p;
    return -1;
}

static void mr_pano_btn(mr_scene_t *s, int arg)
{
    if (arg > 0) mr_add(s->g, arg - 1, 1);
    else mr_add(s->g, -arg - 1, -1);
}

static void down(mr_scene_t *s, int f, float x, float y)
{
    pano_t *P = ST;
    if (!P) return;
    P->fing[f].p = who_at(s, x, y);
    P->fing[f].held = false;
}

static void tick(mr_scene_t *s)
{
    (void)s;
}

static void fr(mr_scene_t *s, float dt)
{
    pano_t *P = ST;
    if (!P) return;
    frame(s, dt);
    /* a finger held still takes a match back */
    for (int i = 0; i < MR_FINGERS; i++) {
        mr_finger_t *f = &mr_A->fing[i];
        if (!f->id || P->fing[i].p < 0 || P->fing[i].held || f->moved) continue;
        if (mr_ms() - f->t0 > HOLD_MS) {
            P->fing[i].held = true;
            mr_add(s->g, P->fing[i].p, -1);
        }
    }
}

static void up(mr_scene_t *s, int f, float x, float y)
{
    pano_t *P = ST;
    if (!P) return;
    int p = P->fing[f].p;
    P->fing[f].p = -1;
    if (p < 0 || P->fing[f].held || x < -1000) return;
    if (mr_A->fing[f].moved) return;
    if (who_at(s, x, y) == p) mr_add(s->g, p, 1);
}

const mr_scene_ops_t mr_pano_ops = { build, sfree, sync, fr, down, NULL, up, tick };
