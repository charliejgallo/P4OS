/*
 * ARKANOS - drawing
 *
 * Two buffers and one rule: 'bg' holds everything that does not change between
 * frames (sky, stars, walls, bricks) and 'fb' holds the frame on screen.
 * Before drawing, the loop restores from bg the rectangles we dirtied on the
 * previous frame; afterwards, everything that moves is drawn into fb and
 * records its rectangle. Only those rectangles are upscaled and invalidated.
 *
 * From that comes this file's unusual requirement: ANY rectangle of the
 * background has to be repaintable, because when a brick breaks whatever was
 * behind it has to be reconstructed. That is why the background is a function
 * of the rectangle -a gradient computed per row, stars stored in a table- and
 * not a drawing made once and then forgotten.
 */
#include "arkanos.h"
#include "aos_i18n.h"

#include <string.h>

/* --------------------------------------------------------------------------
 * Geometry (see arkanos.h)
 * -------------------------------------------------------------------------- */

ak_geo_t ak_geo;

void ak_geo_set(int landscape)
{
    ak_geo_t *o = &ak_geo;

    memset(o, 0, sizeof(*o));
    if (landscape) {
        ak_canvas_w  = 426;
        ak_canvas_h  = 240;
        o->land      = 1;
        o->ax0       = (426 - 240) / 2;         /*  93: a panel on each side */
        o->fy0       = AK_CEIL;                 /*   8 */
        o->by0       = o->fy0 + 12;
        o->pad_y     = 214;
        o->death_y   = 228;
        o->floor_y   = 240;
        o->banner_y  = 150;
        /* the top of the right panel, where the button is drawn, with margin */
        o->pause     = (ak_rect_t){ (int16_t)(o->ax0 + 240), 0, 426, 58 };
        o->speed_pct = 100;
    } else {
        ak_canvas_w  = 240;
        ak_canvas_h  = 426;
        o->ax0       = 0;
        o->fy0       = AK_HUD_H + AK_CEIL;      /*  30 */
        /* three rows of air over the wall: the ball rattles up there, which
         * is half the fun of breaking through */
        o->by0       = o->fy0 + 30;
        o->pad_y     = 346;
        o->death_y   = 360;
        o->floor_y   = 366;                     /* the deck: 60 px, 180 on screen */
        o->banner_y  = 250;
        /* the whole score strip: at the top, far from the thumb */
        o->pause     = (ak_rect_t){ 0, 0, 240, AK_HUD_H };
        o->speed_pct = 125;
    }
    o->ax1 = (int16_t)(o->ax0 + 240);
    o->fx0 = (int16_t)(o->ax0 + AK_WALL);
    o->fx1 = (int16_t)(o->ax1 - AK_WALL);
    o->bx0 = o->fx0;
}

void ak_brick_box(int row, int col, int *x, int *y)
{
    *x = AK_BRICK_X0 + col * AK_BRICK_W;
    *y = AK_BRICK_Y0 + row * AK_BRICK_H;
}

/* --------------------------------------------------------------------------
 * Background
 * -------------------------------------------------------------------------- */

/* The pause button, drawn in the background: it never changes. */
static void pause_box(int *x, int *y, int *w, int *h)
{
    if (ak_geo.land) {
        *w = 48;
        *h = 30;
        *x = ak_geo.ax1 + (AK_W - ak_geo.ax1 - *w) / 2;
        *y = 14;
    } else {
        *w = 22;
        *h = 16;
        *x = AK_W - *w - 4;
        *y = 3;
    }
}

static void pause_paint(ak_buf_t *b, uint16_t acc)
{
    int x, y, w, h;
    pause_box(&x, &y, &w, &h);
    ak_round(b, x, y, w, h, 2, ak_tone(acc, -6));
    ak_round(b, x + 1, y + 1, w - 2, h - 2, 1, ak_tone(acc, -11));
    ak_hline(b, x + 2, y + 1, w - 4, ak_tone(acc, -3));
    /* the two bars of the sign */
    int bw = h / 5, bh = h - 6, gap = bw + 1;
    int bx = x + w / 2 - gap / 2 - bw, by = y + 3;
    ak_rect(b, bx, by, bw, bh, ak_rgb(0xE8F6FF));
    ak_rect(b, bx + bw + gap, by, bw, bh, ak_rgb(0xE8F6FF));
}

/* The deck under the field (portrait): the thumb's place. A rail with a notch
 * under each column of bricks; the carriage that runs on it is drawn with
 * what moves (draw_carriage). */
#define AK_RAIL_DY      30      /* the rail's centre, from the top of the deck */

static void deck_paint(ak_buf_t *b, uint16_t acc)
{
    int y0 = AK_FLOOR_Y;
    int rail = y0 + AK_RAIL_DY;

    ak_vgrad(b, 0, y0, AK_W, AK_H - 1, ak_tone(acc, -11), ak_tone(acc, -14));
    ak_hline(b, 0, y0, AK_W, ak_tone(acc, -3));
    ak_hline(b, 0, y0 + 1, AK_W, ak_tone(acc, -13));

    /* the groove */
    ak_round(b, 14, rail - 4, AK_W - 28, 9, 2, ak_tone(acc, -15));
    ak_hline(b, 16, rail - 4, AK_W - 32, ak_rgb(0x000000));
    ak_hline(b, 16, rail + 4, AK_W - 32, ak_tone(acc, -9));
    for (int c = 0; c < AK_COLS; c++) {
        int x = AK_BRICK_X0 + c * AK_BRICK_W + AK_BRICK_W / 2;
        ak_vline(b, x, rail - 9, 3, ak_tone(acc, -6));
        ak_vline(b, x, rail + 7, 3, ak_tone(acc, -6));
    }
    /* arrows at the ends: this is dragged sideways */
    for (int i = 0; i < 4; i++) {
        ak_vline(b, 4 + i, rail - i, 1 + 2 * i, ak_tone(acc, -4));
        ak_vline(b, AK_W - 5 - i, rail - i, 1 + 2 * i, ak_tone(acc, -4));
    }
}

/* Paints sky, stars and structure inside whatever clip the background buffer
 * already has set. */
static void bg_paint_clipped(ak_t *g)
{
    const ak_level_t *lv = ak_level_get(g->level);
    ak_buf_t *b = &g->bg;
    uint16_t top = ak_rgb(lv->sky_top);
    uint16_t bot = ak_rgb(lv->sky_bot);
    uint16_t acc = ak_rgb(lv->accent);
    int ax0 = ak_geo.ax0, ax1 = ak_geo.ax1;
    int fy0 = AK_FIELD_Y0, fl = AK_FLOOR_Y;

    /* Gradient: one line per row of the clip, not of the screen. ak_mix has
     * 17 steps, which over 426 rows are bands of 25; a 2x2 ordered dither
     * between neighbouring steps gives it four times as many, and a function
     * of (x, y) is still repaintable rectangle by rectangle. */
    static const uint8_t bayer[2][2] = { { 0, 2 }, { 3, 1 } };
    for (int y = b->cy0; y < b->cy1; y++) {
        int f4 = y * 64 / (AK_H - 1);
        uint16_t c0 = ak_mix(top, bot, f4 >> 2);
        int frac = f4 & 3;
        if (!frac) {
            ak_hline(b, b->cx0, y, b->cx1 - b->cx0, c0);
            continue;
        }
        uint16_t c1 = ak_mix(top, bot, (f4 >> 2) + 1);
        for (int x = b->cx0; x < b->cx1; x++) {
            ak_px(b, x, y, bayer[y & 1][x & 1] < frac ? c1 : c0);
        }
    }

    for (int i = 0; i < g->nstars; i++) {
        uint16_t c = ak_mix(ak_mix(top, bot, g->star_y[i] * 16 / (AK_H - 1)),
                            0xFFFF, g->star_b[i]);
        ak_px(b, g->star_x[i], g->star_y[i], c);
    }

    /* the score: a strip on top, or a panel on each side of the arena */
    uint16_t panel = ak_rgb(0x05060C);
    if (ak_geo.land) {
        ak_rect(b, 0, 0, ax0, AK_H, panel);
        ak_rect(b, ax1, 0, AK_W - ax1, AK_H, panel);
        ak_vline(b, ax0 - 2, 0, AK_H, ak_tone(acc, -8));
        ak_vline(b, ax1 + 1, 0, AK_H, ak_tone(acc, -8));
    } else {
        ak_rect(b, 0, 0, AK_W, AK_HUD_H, panel);
        ak_hline(b, 0, AK_HUD_H - 1, AK_W, ak_tone(acc, -8));
    }
    pause_paint(b, acc);

    /* ceiling and walls: a strip of metal with a light edge facing inwards, a
     * sheen along it and joints with rivets, which is what gives the field
     * its scale */
    uint16_t metal  = ak_tone(acc, -10);
    uint16_t brillo = ak_tone(acc, -6);
    uint16_t claro  = ak_tone(acc, -2);
    uint16_t oscuro = ak_tone(acc, -13);

    ak_rect(b, ax0, fy0 - AK_CEIL, ax1 - ax0, AK_CEIL, metal);
    ak_rect(b, ax0, fy0, AK_WALL, fl - fy0, metal);
    ak_rect(b, AK_FIELD_X1, fy0, AK_WALL, fl - fy0, metal);

    ak_hline(b, ax0, fy0 - AK_CEIL + 2, ax1 - ax0, brillo);
    ak_vline(b, ax0 + 3, fy0, fl - fy0, brillo);
    ak_vline(b, ax1 - 4, fy0, fl - fy0, brillo);
    ak_hline(b, ax0, fy0 - 1, ax1 - ax0, claro);
    ak_vline(b, AK_FIELD_X0 - 1, fy0, fl - fy0, claro);
    ak_vline(b, AK_FIELD_X1, fy0, fl - fy0, claro);

    for (int y = fy0 + 24; y < fl - 4; y += 32) {
        ak_hline(b, ax0, y, AK_WALL - 1, oscuro);
        ak_hline(b, AK_FIELD_X1 + 1, y, AK_WALL - 1, oscuro);
        ak_px(b, ax0 + 5, y + 4, oscuro);
        ak_px(b, ax1 - 6, y + 4, oscuro);
    }
    for (int c = 0; c <= AK_COLS; c++) {
        int x = AK_BRICK_X0 + c * AK_BRICK_W;
        ak_vline(b, x, fy0 - AK_CEIL, AK_CEIL - 1, oscuro);
        if (c < AK_COLS) {
            ak_px(b, x + AK_BRICK_W / 2, fy0 - AK_CEIL / 2 - 1, oscuro);
        }
    }

    if (!ak_geo.land) {
        deck_paint(b, acc);
    }
}

static void bg_paint(ak_t *g, int x, int y, int w, int h)
{
    ak_clip(&g->bg, x, y, x + w, y + h);
    bg_paint_clipped(g);
    ak_clip_none(&g->bg);
}

/* A brick: a block with a light edge at the top and on the left, shadow at the
 * bottom and on the right, and a dark outline so two neighbouring bricks do
 * not merge. */
static void brick_paint(ak_t *g, int row, int col)
{
    int kind = g->grid[row][col];
    if (kind == BK_NONE) {
        return;
    }
    const ak_brick_def_t *d = ak_brick(kind);
    ak_buf_t *b = &g->bg;
    int x, y;
    ak_brick_box(row, col, &x, &y);

    uint16_t base = ak_rgb(d->color);
    /* The multi-hit ones fade: it is the only clue as to how much they have
     * left, and it reads at a glance better than any little number. */
    if (d->hp > 1 && g->hp[row][col] < d->hp) {
        base = ak_tone(base, -(int)(d->hp - g->hp[row][col]) * 4);
    }

    ak_rect(b, x, y, AK_BRICK_W, AK_BRICK_H, base);
    ak_hline(b, x + 1, y, AK_BRICK_W - 2, ak_tone(base, 5));
    ak_vline(b, x, y + 1, AK_BRICK_H - 2, ak_tone(base, 3));
    ak_hline(b, x + 1, y + AK_BRICK_H - 1, AK_BRICK_W - 2, ak_tone(base, -6));
    ak_vline(b, x + AK_BRICK_W - 1, y + 1, AK_BRICK_H - 2, ak_tone(base, -5));
    ak_px(b, x, y, ak_tone(base, -8));
    ak_px(b, x + AK_BRICK_W - 1, y, ak_tone(base, -8));
    ak_px(b, x, y + AK_BRICK_H - 1, ak_tone(base, -8));
    ak_px(b, x + AK_BRICK_W - 1, y + AK_BRICK_H - 1, ak_tone(base, -8));

    if (kind == BK_STEEL) {
        /* diagonal sheen: it reads as metal and not as "one more colour" */
        for (int i = 0; i < 4; i++) {
            ak_line(b, x + 3 + i * 4, y + AK_BRICK_H - 2, x + 6 + i * 4, y + 1,
                    ak_tone(base, 6));
        }
    } else if (kind == BK_BOMB) {
        ak_disc(b, x + AK_BRICK_W / 2, y + AK_BRICK_H / 2, 3, ak_rgb(0xFFE45E));
        ak_disc(b, x + AK_BRICK_W / 2, y + AK_BRICK_H / 2, 1, ak_rgb(0xFFFFFF));
    } else if (kind == BK_MYST) {
        ak_text(b, x + AK_BRICK_W / 2 - 2, y + 2, "?", ak_rgb(0x05060C));
    } else if (d->hp > 1) {
        /* cracks: one per hit taken */
        int golpes = d->hp - g->hp[row][col];
        for (int i = 0; i < golpes && i < 3; i++) {
            int gx = x + 4 + i * 6;
            ak_line(b, gx, y + 2, gx + 2, y + AK_BRICK_H - 3, ak_tone(base, -9));
        }
    }
}

void ak_bg_brick(ak_t *g, int row, int col)
{
    int x, y;
    ak_brick_box(row, col, &x, &y);
    bg_paint(g, x, y, AK_BRICK_W, AK_BRICK_H);
    ak_clip(&g->bg, x, y, x + AK_BRICK_W, y + AK_BRICK_H);
    brick_paint(g, row, col);
    ak_clip_none(&g->bg);
    ak_dirty_add(&g->d_bg, x, y, AK_BRICK_W, AK_BRICK_H);
}

void ak_bg_build(ak_t *g)
{
    const ak_level_t *lv = ak_level_get(g->level);

    /* the maps give the stars for the watch's 176x206 field: as many more as
     * the field here is bigger, so the sky is as dense */
    int area = (AK_FIELD_X1 - AK_FIELD_X0) * (AK_FLOOR_Y - AK_FIELD_Y0);
    int n = lv->stars * area / (176 * 206);
    g->nstars = (uint8_t)(n > AK_MAX_STARS ? AK_MAX_STARS : n);
    for (int i = 0; i < g->nstars; i++) {
        g->star_x[i] = (int16_t)ak_rnd_range(g, AK_FIELD_X0 + 1, AK_FIELD_X1 - 2);
        g->star_y[i] = (int16_t)ak_rnd_range(g, AK_FIELD_Y0 + 1, AK_FLOOR_Y - 2);
        g->star_b[i] = (uint8_t)ak_rnd_range(g, 3, 13);
    }

    ak_clip_none(&g->bg);
    bg_paint_clipped(g);
    for (int r = 0; r < AK_ROWS; r++) {
        for (int c = 0; c < AK_COLS; c++) {
            brick_paint(g, r, c);
        }
    }

    /* the frame starts as a copy of the background */
    memcpy(g->fb.px, g->bg.px, (size_t)AK_W * AK_H * sizeof(uint16_t));
    ak_dirty_all(&g->d_bg);
}

/* --------------------------------------------------------------------------
 * Score
 *
 * It only repaints when a number changes: it restores its rectangles from the
 * background, writes over them and records them. They do not enter the next
 * frame's list because nothing moves there (everything else is kept inside
 * the arena), so what is left written still holds.
 *
 * Portrait: score, level, lives and the pause sign in the strip on top.
 * Landscape: score and record on the left panel; level and lives on the
 * right one, under the pause button.
 * -------------------------------------------------------------------------- */

static void hud_lives(ak_buf_t *b, int x0, int y, int n, int dir)
{
    /* little paddles, which is what is lost */
    for (int i = 0; i < n; i++) {
        int x = x0 + dir * i * 10;
        ak_rect(b, x, y, 8, 3, ak_rgb(0x4A9DF5));
        ak_hline(b, x + 1, y, 6, ak_rgb(0xBFE9FF));
    }
}

static void hud_text2(ak_buf_t *b, int cx, int y, const char *s, uint16_t c)
{
    int x = cx - ak_text_wk(s, 2) / 2;
    ak_text_k(b, x + 1, y + 1, s, ak_rgb(0x000000), 2);
    ak_text_k(b, x, y, s, c, 2);
}

void ak_draw_hud(ak_t *g)
{
    ak_buf_t *b = &g->fb;
    char score[12], nivel[12];
    uint16_t blanco = ak_rgb(0xFFFFFF), gris = ak_rgb(0x9AA3BC);

    ak_num(score, g->score, 6);
    char *p = nivel;
    *p++ = 'N';
    p = ak_num(p, (uint32_t)(g->level + 1), 1);
    if (g->lap) {
        *p++ = '-';
        p = ak_num(p, (uint32_t)(g->lap + 1), 1);
    }
    *p = '\0';

    if (!ak_geo.land) {
        ak_rect_t band = { 0, 0, AK_W, AK_HUD_H - 1 };
        ak_restore(g->fb.px, g->bg.px, &band);
        ak_clip(b, band.x0, band.y0, band.x1, band.y1);

        ak_text_k(b, 6, 4, score, blanco, 2);
        hud_text2(b, AK_W / 2 + 6, 4, nivel, gris);
        int lives = g->lives < 4 ? g->lives : 4;
        hud_lives(b, AK_W - 36, 9, lives, -1);

        ak_clip_none(b);
        ak_dirty_add(&g->d_push, band.x0, band.y0, band.x1 - band.x0, band.y1 - band.y0);
    } else {
        int lcx = ak_geo.ax0 / 2, rcx = (ak_geo.ax1 + AK_W) / 2;
        ak_rect_t left  = { 0, 0, (int16_t)(ak_geo.ax0 - 3), 84 };
        ak_rect_t right = { (int16_t)(ak_geo.ax1 + 3), 56, (int16_t)AK_W, 112 };
        char hi[16];

        ak_restore(g->fb.px, g->bg.px, &left);
        ak_restore(g->fb.px, g->bg.px, &right);
        ak_clip(b, left.x0, left.y0, left.x1, left.y1);
        hud_text2(b, lcx, 20, score, blanco);
        hi[0] = 'H';
        hi[1] = 'I';
        hi[2] = ' ';
        ak_num(hi + 3, g->hiscore, 6);
        ak_text_center(b, lcx, 46, hi, gris, ak_rgb(0x000000));
        /* the wall's name, which in portrait only the level sign shows */
        ak_text_center(b, lcx, 66, _(ak_level_get(g->level)->name),
                       ak_tone(ak_rgb(ak_level_get(g->level)->accent), 2), ak_rgb(0x000000));

        ak_clip(b, right.x0, right.y0, right.x1, right.y1);
        hud_text2(b, rcx, 62, nivel, gris);
        /* up to ten, in rows of five */
        int lives = g->lives < 10 ? g->lives : 10;
        for (int row = 0; row * 5 < lives; row++) {
            int n = lives - row * 5 < 5 ? lives - row * 5 : 5;
            hud_lives(b, rcx - (n * 10 - 2) / 2, 90 + row * 8, n, 1);
        }

        ak_clip_none(b);
        ak_dirty_add(&g->d_push, left.x0, left.y0, left.x1 - left.x0, left.y1 - left.y0);
        ak_dirty_add(&g->d_push, right.x0, right.y0, right.x1 - right.x0, right.y1 - right.y0);
    }
    g->hud_dirty = 0;
}

/* --------------------------------------------------------------------------
 * What moves
 * -------------------------------------------------------------------------- */

/* Records a dirty rectangle, clipped to the arena just like the drawing.
 *
 * The clip is not decorative: the score (the strip on top, or the side panels
 * in landscape) is NOT restored on every frame (it only repaints when a
 * number changes), so if a rectangle touched it, the next frame would restore
 * it from the background and erase the score until the next change. The blast
 * of a bomb on the top row reaches a radius of 27 px, so it does reach it. */
static void mark(ak_t *g, int x, int y, int w, int h)
{
    if (y < AK_FIELD_Y0) {
        h -= AK_FIELD_Y0 - y;
        y = AK_FIELD_Y0;
    }
    if (x < ak_geo.ax0) {
        w -= ak_geo.ax0 - x;
        x = ak_geo.ax0;
    }
    if (x + w > ak_geo.ax1) {
        w = ak_geo.ax1 - x;
    }
    ak_dirty_add(&g->d_cur, x, y, w, h);
}

static void draw_paddle(ak_t *g)
{
    ak_buf_t *b = &g->fb;
    int w = g->pad_w;
    int x = UNFX(g->pad_x) - w / 2;
    int y = AK_PAD_Y;
    uint16_t cuerpo = ak_rgb(0x4A9DF5);
    uint16_t filo   = ak_rgb(0xBFE9FF);

    if (g->t_catch) {
        cuerpo = ak_rgb(0x4ADE80);
        filo   = ak_rgb(0xC8FFD8);
    }

    ak_round(b, x, y, w, AK_PAD_H, 1, ak_tone(cuerpo, -6));
    ak_hline(b, x + 2, y, w - 4, filo);
    ak_hline(b, x + 1, y + 1, w - 2, cuerpo);
    ak_hline(b, x + 1, y + 2, w - 2, ak_tone(cuerpo, -3));

    if (g->t_laser) {
        /* the cannons poke out: you have to see the upgrade is fitted */
        ak_rect(b, x + 1, y - 2, 3, 2, ak_rgb(0xFF4A3D));
        ak_rect(b, x + w - 4, y - 2, 3, 2, ak_rgb(0xFF4A3D));
    }
    /* Upgrades about to run out blink for the last second and a half: losing
     * the wide paddle mid-rally is an unpleasant surprise. */
    if ((g->t_wide && g->t_wide < 45 && (g->t_wide & 4)) ||
        (g->t_laser && g->t_laser < 45 && (g->t_laser & 4)) ||
        (g->t_catch && g->t_catch < 45 && (g->t_catch & 4))) {
        ak_shade(b, x, y - 2, w, AK_PAD_H + 2, 7);
    }

    mark(g, x - 1, y - 3, w + 2, AK_PAD_H + 4);
}

/* The carriage on the deck's rail, right under the paddle (portrait). It is
 * the paddle's handle: it lights while a finger is on the glass. */
static void draw_carriage(ak_t *g)
{
    ak_buf_t *b = &g->fb;
    int w = 28, h = 13;
    int x = UNFX(g->pad_x) - w / 2;
    int y = AK_FLOOR_Y + AK_RAIL_DY - h / 2;
    uint16_t cuerpo = g->t_catch ? ak_rgb(0x4ADE80) : ak_rgb(0x4A9DF5);

    if (!g->touching) {
        cuerpo = ak_tone(cuerpo, -5);
    }
    ak_round(b, x, y, w, h, 3, ak_tone(cuerpo, -7));
    ak_round(b, x + 1, y + 1, w - 2, h - 2, 2, cuerpo);
    ak_hline(b, x + 3, y + 1, w - 6, ak_tone(cuerpo, 7));
    for (int i = -1; i <= 1; i++) {
        ak_vline(b, x + w / 2 + i * 4, y + 4, h - 7, ak_tone(cuerpo, -8));
    }
    ak_dirty_add(&g->d_cur, x - 1, y - 1, w + 2, h + 2);
}

static void draw_balls(ak_t *g)
{
    ak_buf_t *b = &g->fb;

    for (int i = 0; i < AK_MAX_BALLS; i++) {
        ak_ball_t *ba = &g->ball[i];
        if (!ba->alive) {
            continue;
        }
        /* trail: three dots fading. It costs three small rectangles that
         * nearly always merge with the ball's. */
        for (int t = ba->tn - 1; t >= 0; t--) {
            int f = 9 - t * 3;
            ak_glow(b, ba->tx[t], ba->ty[t], AK_BALL_R, ak_rgb(0x7BE9FF), f);
            mark(g, ba->tx[t] - AK_BALL_R - 1, ba->ty[t] - AK_BALL_R - 1,
                 AK_BALL_R * 2 + 3, AK_BALL_R * 2 + 3);
        }

        int x = UNFX(ba->x), y = UNFX(ba->y);
        ak_disc(b, x, y, AK_BALL_R, ak_rgb(0xE8F6FF));
        ak_px(b, x - 1, y - 1, ak_rgb(0xFFFFFF));
        ak_px(b, x + 1, y + 1, ak_rgb(0x8FB8CC));
        mark(g, x - AK_BALL_R - 1, y - AK_BALL_R - 1,
             AK_BALL_R * 2 + 3, AK_BALL_R * 2 + 3);
    }
}

/* A capsule, 14x8, top-left corner at x, y. */
static void cap_sprite(ak_buf_t *b, int x, int y, int kind)
{
    const ak_cap_def_t *d = ak_cap_def(kind);
    uint16_t col = ak_rgb(d->color);
    char txt[2] = { d->letra, '\0' };

    ak_round(b, x, y, 14, 8, 2, ak_tone(col, -7));
    ak_round(b, x + 1, y + 1, 12, 6, 1, col);
    ak_hline(b, x + 3, y + 1, 8, ak_tone(col, 6));
    ak_text(b, x + 5, y + 1, txt, ak_rgb(0x05060C));
}

static void draw_caps(ak_t *g)
{
    for (int i = 0; i < AK_MAX_CAPS; i++) {
        ak_cap_t *c = &g->cap[i];
        if (!c->alive) {
            continue;
        }
        int x = UNFX(c->x) - 7;
        int y = UNFX(c->y) - 4;
        cap_sprite(&g->fb, x, y, c->kind);
        mark(g, x - 1, y - 1, 16, 10);
    }
}

/* The upgrades running, each a capsule and a bar that empties: on the deck
 * under the rail (portrait) or in the right panel under the lives
 * (landscape). They are drawn every frame while they last and recorded like
 * anything else that moves, so when one runs out its place is restored. */
static void draw_upgrades(ak_t *g)
{
    const struct { uint8_t kind; uint16_t t, t0; } up[] = {
        { CAP_WIDE,  g->t_wide,  700 },
        { CAP_LASER, g->t_laser, 700 },
        { CAP_CATCH, g->t_catch, 700 },
        { CAP_SLOW,  g->t_slow,  500 },
    };
    ak_buf_t *b = &g->fb;
    int n = 0;
    int bar = ak_geo.land ? 52 : 30;

    for (unsigned i = 0; i < sizeof(up) / sizeof(up[0]); i++) {
        if (!up[i].t) {
            continue;
        }
        int x, y;
        if (ak_geo.land) {
            x = ak_geo.ax1 + (AK_W - ak_geo.ax1 - (14 + 4 + bar)) / 2;
            y = 128 + n * 14;
        } else {
            x = 18 + n * (14 + 4 + bar + 10);
            y = AK_FLOOR_Y + 42;
        }
        uint16_t col = ak_rgb(ak_cap_def(up[i].kind)->color);
        int len = up[i].t * bar / up[i].t0;
        cap_sprite(b, x, y, up[i].kind);
        ak_rect(b, x + 18, y + 3, bar, 3, ak_rgb(0x05060C));
        ak_rect(b, x + 18, y + 3, len > 0 ? len : 1, 3, col);
        ak_dirty_add(&g->d_cur, x - 1, y - 1, 14 + 4 + bar + 2, 10);
        n++;
    }
}

static void draw_shots(ak_t *g)
{
    ak_buf_t *b = &g->fb;

    for (int i = 0; i < AK_MAX_SHOTS; i++) {
        if (!g->shot[i].alive) {
            continue;
        }
        int x = UNFX(g->shot[i].x), y = UNFX(g->shot[i].y);
        ak_rect(b, x, y - 3, 1, 6, ak_rgb(0xFFE45E));
        ak_px(b, x, y - 4, ak_rgb(0xFFFFFF));
        mark(g, x - 1, y - 5, 3, 10);
    }
}

static void draw_bits(ak_t *g)
{
    ak_buf_t *b = &g->fb;

    for (int i = 0; i < AK_MAX_BITS; i++) {
        ak_bit_t *p = &g->bit[i];
        if (!p->life) {
            continue;
        }
        int x = UNFX(p->x), y = UNFX(p->y);
        int f = p->life0 ? p->life * 16 / p->life0 : 0;
        uint16_t c = ak_mix(ak_rgb(0x05060C), p->col, f);
        ak_rect(b, x, y, 2, 2, c);
        mark(g, x - 1, y - 1, 4, 4);
    }
}

static void draw_rings(ak_t *g)
{
    ak_buf_t *b = &g->fb;

    for (int i = 0; i < AK_MAX_RINGS; i++) {
        ak_ring_t *r = &g->ring[i];
        if (!r->life) {
            continue;
        }
        int paso = r->life0 - r->life;
        int rad  = 3 + paso * 2;
        int f    = r->life * 10 / (r->life0 ? r->life0 : 1);
        ak_wave(b, r->x, r->y, rad, 2, r->col, f);
        mark(g, r->x - rad - 3, r->y - rad - 3, rad * 2 + 7, rad * 2 + 7);
    }
}

static void draw_hits(ak_t *g)
{
    ak_buf_t *b = &g->fb;

    for (int r = 0; r < AK_ROWS; r++) {
        for (int c = 0; c < AK_COLS; c++) {
            if (!g->hit[r][c]) {
                continue;
            }
            int x, y;
            ak_brick_box(r, c, &x, &y);
            ak_shade(b, x, y, AK_BRICK_W, AK_BRICK_H, g->hit[r][c] * 3);
            mark(g, x, y, AK_BRICK_W, AK_BRICK_H);
        }
    }
}

static void draw_pops(ak_t *g)
{
    ak_buf_t *b = &g->fb;

    for (int i = 0; i < AK_MAX_POPS; i++) {
        ak_pop_t *p = &g->pop[i];
        if (!p->life) {
            continue;
        }
        int w = ak_text_w(p->txt);
        uint16_t c = p->life < 8 ? ak_mix(ak_rgb(0x05060C), p->col, p->life * 2)
                                 : p->col;
        ak_text_sh(b, p->x - w / 2, p->y, p->txt, c, ak_rgb(0x05060C));
        mark(g, p->x - w / 2 - 1, p->y - 1, w + 3, AK_CH_H + 3);
    }
}

/* The "NIVEL 3" panel and company. It is large and stays for a while, but it
 * is still one more rectangle: it is only restored when it goes. */
static void draw_banner(ak_t *g)
{
    const char *linea1 = NULL;
    const char *linea2 = NULL;
    char buf[16];
    uint16_t col = ak_rgb(0xFFE45E);

    if (g->state == ST_READY) {
        /* -4 leaves room for the level number and the terminator. */
        const char *pre = _("NIVEL ");
        int i = 0;
        while (pre[i] && i < (int)sizeof(buf) - 4) {
            buf[i] = pre[i];
            i++;
        }
        ak_num(buf + i, (uint32_t)(g->level + 1), 1);
        linea1 = buf;
        linea2 = _(ak_level_get(g->level)->name);
    } else if (g->state == ST_CLEAR) {
        linea1 = _("SUPERADO");
        col = ak_rgb(0x4ADE80);
    } else if (g->state == ST_LOST) {
        linea1 = _("OTRA VEZ");
        col = ak_rgb(0xFF4A3D);
    } else {
        return;
    }

    int w1 = ak_text_wk(linea1, 2);
    int w2 = linea2 ? ak_text_w(linea2) : 0;
    int w  = (w1 > w2 ? w1 : w2) + 20;
    int h  = linea2 ? 34 : 24;
    int cx = AK_FIELD_CX;
    int x  = cx - w / 2;
    int y  = ak_geo.banner_y;

    ak_buf_t *b = &g->fb;
    ak_round(b, x, y, w, h, 3, ak_rgb(0x05060C));
    ak_shade(b, x, y, w, h, -2);
    ak_text_k(b, cx - w1 / 2 + 1, y + 6, linea1, ak_rgb(0x000000), 2);
    ak_text_k(b, cx - w1 / 2, y + 5, linea1, col, 2);
    if (linea2) {
        ak_text_center(b, cx, y + 23, linea2, ak_rgb(0x9AA3BC), ak_rgb(0x000000));
    }
    mark(g, x - 1, y - 1, w + 2, h + 2);
}

/* Frames per second and pixels pushed. It is the number to watch on the board:
 * the second one says how much work the dirty list saved. */
static void draw_fps(ak_t *g)
{
    char buf[20];
    char *p = buf;

    p = ak_num(p, (uint32_t)(g->fps10 / 10), 1);
    *p++ = '.';
    p = ak_num(p, (uint32_t)(g->fps10 % 10), 1);
    *p++ = ' ';
    p = ak_num(p, (uint32_t)g->last_area, 1);
    *p++ = '%';
    *p = '\0';

    ak_text_sh(&g->fb, AK_FIELD_X0 + 2, AK_FLOOR_Y - 9, buf, ak_rgb(0x4ADE80),
               ak_rgb(0x000000));
    mark(g, AK_FIELD_X0 + 1, AK_FLOOR_Y - 10, ak_text_w(buf) + 3, AK_CH_H + 3);
}

void ak_draw_movers(ak_t *g)
{
    ak_dirty_reset(&g->d_cur);

    /* Everything clipped to the field: if a particle escaped into the score it
     * would leave rubbish there until the next score change, because that
     * strip is not restored on every frame. */
    ak_clip(&g->fb, AK_FIELD_X0, AK_FIELD_Y0, AK_FIELD_X1, AK_FLOOR_Y);

    draw_hits(g);
    draw_rings(g);
    draw_caps(g);
    draw_bits(g);
    draw_shots(g);
    draw_balls(g);
    draw_paddle(g);
    draw_pops(g);
    draw_banner(g);
    if (g->show_fps) {
        draw_fps(g);
    }

    if (!ak_geo.land) {
        ak_clip(&g->fb, 0, AK_FLOOR_Y + 2, AK_W, AK_H);
        draw_carriage(g);
    } else {
        ak_clip(&g->fb, ak_geo.ax1 + 3, 124, AK_W, AK_H);
    }
    draw_upgrades(g);
    ak_clip_none(&g->fb);
}
