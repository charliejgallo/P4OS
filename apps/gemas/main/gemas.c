/*
 * P4OS - gemas (from AmoledOS)
 *
 * A match-three game of the Bejeweled family: two neighbouring jewels are
 * swapped and anything left in a line of three or more breaks, what is above
 * falls and sometimes that makes another line by itself. Lining up four or
 * more leaves a special jewel.
 *
 * It builds two ways from the same source:
 *
 *   .so for the board               cd apps/gemas && idf.py so
 *   built-in app of the simulator   the simulator builds it (AOS_SIM_BUILTIN)
 *
 * How it is put together inside:
 *
 *   gm_art    draws the jewels in code, once, into RGB565A8 sprites
 *   gm_board  the rules: lines, specials, falling, shuffling
 *   gm_fx     sparks, ripples, beams and panels
 *   gm_snd    the melodies, which come out of the single tone there is
 *
 * And what is left here is what joins them: one view per cell (an LVGL image
 * that moves, scales and rotates) and a state machine carrying the swap, the
 * break, the fall and the cascade along.
 *
 * There is no canvas: each jewel is an LVGL object, so only what moves is
 * repainted. In a game where half the screen is still most of the time, that
 * is far cheaper than redrawing everything every frame - and on a 720x1280
 * panel drawn in software, much more so than it was on the watch.
 *
 * What the port changed:
 *
 *   - The layout comes from the root, not from constants. Upright the board
 *     is 8 x 88 px, edge to edge across the width and low, where the thumb
 *     is, and the HUD fills everything above it; lying down it is 8 x 84 px
 *     on the left and the HUD is a column on the right. The jewels are
 *     RENDERED at that size (not scaled), so they stay as sharp as on the
 *     watch.
 *   - It is played by touch alone: the board has no buttons and no motion
 *     sensor, so pausing is the round button in the HUD.
 *   - The game lives in a static: turning the screen destroys and re-creates
 *     the app, and the board, the score and the level come back paused.
 *   - Everything the watch measured in its 44 px cell (gravity, the drag
 *     threshold, sparks, waves) is scaled by the cell.
 *   - A USB gamepad plays it too: the d-pad walks a yellow frame over the
 *     jewels (it shows on the first press and hides at the next touch), A
 *     takes the jewel under it - the same white ring as a tap - and then a
 *     direction swaps it with that neighbour (or A on it again, or B, lets
 *     it go). START pauses and resumes; the menu, the pause and the game
 *     over go through aos_pad_menu, with START on their main button and B
 *     as "Seguir" in the pause.
 */
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"
#include "aos_theme.h"
#include "aos_fonts.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"

#include "gm_art.h"
#include "gm_board.h"
#include "gm_fx.h"
#include "gm_snd.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Measurements
 * -------------------------------------------------------------------------- */

#define FRAME_MS        33              /* 30 frames per second */
#define FX16            16              /* positions in 1/16 of a pixel */

/* The watch's pixels (a 44 px cell) in this layout's: gravity, thresholds,
 * shake. */
#define SC(v)           ((int)(v) * a->cell / GM_CELL_REF)

/* Layout, in px. The system's edge strips are what bound the game: a drag
 * that starts in the top 48 px (40 lying down) pulls the notifications, and
 * one in the bottom 36 px goes home (aos_ui.c, gesture_begin). The board and
 * the HUD keep clear of both; the frame may overlap them, it takes no touch. */
#define PAD             24              /* AOS_UI_PAD: the screen's margin      */
#define PAD_BOARD_P     8               /* board to the side edges, upright     */
#define EDGE_TOP        56              /* below the notifications' strip, with air */
#define EDGE_BOTTOM     40              /* above the home strip, with air       */
#define BTN_H           120             /* the big buttons                      */
#define ROW_H           104             /* chips and small buttons              */
#define PAUSE_D         112             /* the HUD's round pause button         */
#define DLG_W           640             /* pause and game over cards            */
#define DLG_IN          48              /* the card's inner margin              */
#define HUD_MIN         420             /* the least the HUD needs upright      */
#define HUD_BAR_H       20              /* the level's bar                      */
#define HUD_TIME_H      32              /* the clock's bar, time attack         */

#define TIME_MAX        10000           /* the time bar, in thousandths */
#define COMBO_MAX       9

#define KEY_HI_RELAX    "gem_hi_relax"
#define KEY_HI_TIME     "gem_hi_time"
#define KEY_MODE        "gem_mode"
#define KEY_DIFF        "gem_diff"
#define KEY_SFX         "gem_sfx"

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

typedef enum {
    ST_MENU = 0,
    ST_FALL,        /* something is falling (the initial deal too) */
    ST_IDLE,        /* the board is waiting for the player */
    ST_SWAP,        /* two jewels changing places */
    ST_UNSWAP,      /* ... and coming back, because they formed nothing */
    ST_POP,         /* the jewels breaking */
    ST_SHUFFLE,     /* no moves left: it shuffles */
    ST_OVER,
    ST_PAUSE,
} state_t;

/* How a jewel breaks. It changes the animation, the sound and the sparks. */
typedef enum {
    POP_NORMAL = 0, /* line of three: it grows and fades      */
    POP_SUCK,       /* line of four or more: it goes to the centre */
    POP_BURN,       /* a flame caught it                      */
    POP_BEAM,       /* a star caught it                       */
    POP_WIPE,       /* the hypercube took it                  */
} pop_kind_t;

typedef struct {
    lv_obj_t *img;              /* the jewel */
    lv_obj_t *badge;            /* flame or star, on top */
    int16_t   x, y;             /* current position in 1/16 px, inside the board */
    int16_t   vy;               /* fall, 1/16 px per frame */
    int16_t   sx, sy;           /* origin of the swap, in 1/16 px */
    uint8_t   pop, pop0;        /* frames of breaking: 0 = whole */
    uint8_t   delay;            /* wait before it starts breaking */
    uint8_t   kind;             /* pop_kind_t */
    uint8_t   squash;           /* frames of bounce on landing */
    uint8_t   birth;            /* frames of a special jewel appearing */
    int8_t    pr, pc;           /* where it goes if a special swallows it */
    int8_t    type;
    uint8_t   special;
    bool      alive;
} cell_view_t;

typedef struct {
    const char *name;
    uint8_t  colors;
    uint16_t drain;         /* thousandths of bar per frame, level 1 */
    uint16_t drain_step;    /* how much it goes up per level          */
    uint16_t gain;          /* thousandths each broken jewel gives back */
} diff_t;

/* The balance of the time-attack mode comes from a single sum: the bar is
 * 10,000 thousandths, the drain is per frame (30 a second) and each broken
 * jewel gives back 'gain'. In NORMAL level 1 that is 180 thousandths a second,
 * that is, 55 seconds of a full bar, and a line of three gives back some 4
 * seconds: time to think, but not to get distracted. */
static const diff_t s_diffs[3] = {
    { N_("Fácil"),    6,  4, 1, 300 },
    { N_("Normal"),   7,  6, 2, 240 },
    { N_("Difícil"),  7,  9, 3, 180 },
};

/*
 * The whole app is ONE static, split in two halves:
 *
 *   - the game, first, survives create/destroy: turning the screen makes the
 *     runtime rebuild the app in the new size, and the board, the score and
 *     the level must come back as they were;
 *   - the view, from 'v' on, is zeroed on every create: LVGL objects, the
 *     sprites (rendered at the size of the layout) and the input.
 *
 * 'live' says there is a game worth coming back to. SALIR and going back to
 * the menu clear it, so opening the app again after leaving on purpose shows
 * the menu, as on the watch.
 */
typedef struct {
    /* --- game (survives) --- */
    gm_board_t  b;
    bool        live;
    const char *over_why;       /* the game over title, if it is over */
    state_t     state;
    state_t     resume;         /* where to go back to from the pause */
    int         phase;          /* frames the phase has left */
    int         phase0;
    int         combo;
    uint32_t    score;
    uint32_t    hiscore;
    int         level;
    int         level_pts;      /* points accumulated towards the level */
    int         level_need;
    int         timebar;        /* 0..TIME_MAX, time-attack mode only */
    int         mode;           /* 0 relaxed, 1 time attack */
    int         diff;
    int         idle_frames;    /* for the hint */
    int         hint[4];
    bool        hint_on;
    int         tick_warn;      /* so the time warning is not repeated */

    /* the cell the finger moved: that is where the special jewel appears */
    uint8_t     mark[GM_N][GM_N];   /* what is breaking right now */
    int8_t      swap_r1, swap_c1, swap_r2, swap_c2;

    /* --- view (rebuilt on every create; must stay first of its half) --- */
    cell_view_t v[GM_N][GM_N];
    int8_t      sel_r, sel_c;   /* current selection, -1 if none */

    /* layout */
    int         cell;           /* px per cell: 88 upright, 84 lying down */
    int         board_px;
    int         bx, by;         /* the board's corner in the root */
    int         bar_w;          /* width of the HUD's bars */
    int         hud_h;          /* the HUD's height: what is left beside the board */
    int         score_y;        /* the score's line, for the 144 px digits */
    bool        land;

    gm_art_t    art;
    gm_fx_t     fx;
    lv_obj_t   *root;
    lv_obj_t   *board;          /* the board's container, it shakes */
    lv_obj_t   *bg;
    lv_obj_t   *sel_ring;
    lv_obj_t   *touch;
    lv_timer_t *timer;

    /* HUD */
    lv_obj_t   *hud;
    lv_obj_t   *lbl_score;
    lv_obj_t   *lbl_level;
    lv_obj_t   *lbl_mode;
    lv_obj_t   *lbl_hud_best;
    lv_obj_t   *lbl_next;
    lv_obj_t   *lbl_next_cap;
    lv_obj_t   *bar_level;
    lv_obj_t   *bar_level_fill;
    lv_obj_t   *bar_time;
    lv_obj_t   *bar_time_fill;
    lv_obj_t   *btn_pause;
    uint32_t    last_score;     /* what the HUD shows, to only redraw changes */
    int         last_level, last_lvl_w, last_time_w;

    /* panels */
    lv_obj_t   *menu;
    lv_obj_t   *chip_mode;
    lv_obj_t   *chip_diff;
    lv_obj_t   *chip_sfx;
    lv_obj_t   *lbl_best;
    lv_obj_t   *pause;
    lv_obj_t   *chip_sfx2;
    lv_obj_t   *over;
    lv_obj_t   *lbl_over_title;
    lv_obj_t   *lbl_over_score;
    lv_obj_t   *lbl_over_info;

    /* input */
    bool        pressing;
    int16_t     press_x, press_y;
    int8_t      press_r, press_c;
    bool        drag_done;
    bool        want_exit;
    bool        exiting;        /* SALIR: back() must let the runtime close */

    /* gamepad */
    aos_pad_t      pad;
    aos_pad_menu_t pmenu;       /* the buttons of the panel showing */
    lv_obj_t      *pm_menu[4], *pm_pause[4], *pm_over[3];
    lv_obj_t      *pad_ring;    /* the pad's frame over the board */
    bool           cur_on;
    int8_t         cur_r, cur_c;
#ifdef AOS_SIM_BUILTIN
    int8_t      test_mv[4];     /* move served up by GEMAS_TEST, -1 = none */
#endif
} app_t;

static app_t s_app;

/* --------------------------------------------------------------------------
 * Utilities
 * -------------------------------------------------------------------------- */

static inline int cell_home_x(const app_t *a, int c) { return c * a->cell * FX16; }
static inline int cell_home_y(const app_t *a, int r) { return r * a->cell * FX16; }

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Points with a thousands separator: "12.480". It reads far better at a
 * glance, which is all you look at while playing. */
static void format_score(char *out, size_t len, uint32_t value)
{
    if (value < 1000) {
        snprintf(out, len, "%u", (unsigned)value);
    } else if (value < 1000000) {
        snprintf(out, len, "%u.%03u", (unsigned)(value / 1000),
                 (unsigned)(value % 1000));
    } else {
        snprintf(out, len, "%u.%03u.%03u", (unsigned)(value / 1000000),
                 (unsigned)((value / 1000) % 1000), (unsigned)(value % 1000));
    }
}

/* --------------------------------------------------------------------------
 * A cell's view
 *
 * Each jewel is a cell-sized LVGL image with its sprite. The specials also carry a
 * badge (the flame or the star), which is another sibling image and not a
 * child: scaling an object with children forces LVGL to draw the whole subtree
 * in a separate layer, whereas scaling a lone image is a multiplication inside
 * the same blit. With twenty jewels breaking at once the difference shows.
 * -------------------------------------------------------------------------- */

static lv_obj_t *make_image(app_t *a)
{
    lv_obj_t *img = lv_image_create(a->board);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(img, a->cell, a->cell);
    lv_image_set_pivot(img, a->cell / 2, a->cell / 2);
    lv_image_set_antialias(img, true);
    return img;
}

static void view_badge(app_t *a, cell_view_t *v)
{
    const lv_image_dsc_t *src = NULL;
    if (v->special == GM_SP_FLAME) {
        src = &a->art.flame.dsc;
    } else if (v->special == GM_SP_STAR) {
        src = &a->art.star.dsc;
    }

    if (!src) {
        if (v->badge) {
            lv_obj_add_flag(v->badge, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (!v->badge) {
        v->badge = make_image(a);
    }
    lv_image_set_src(v->badge, src);
    lv_obj_remove_flag(v->badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(v->badge);
}

static void view_sync(app_t *a, int r, int c)
{
    cell_view_t *v = &a->v[r][c];
    v->type    = a->b.c[r][c].type;
    v->special = a->b.c[r][c].special;

    const lv_image_dsc_t *src = (v->special == GM_SP_HYPER)
                              ? &a->art.hyper.dsc
                              : &a->art.gem[clampi(v->type, 0, GM_TYPES - 1)].dsc;
    lv_image_set_src(v->img, src);
    lv_obj_remove_flag(v->img, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_scale(v->img, 256);
    lv_image_set_rotation(v->img, 0);
    lv_obj_set_style_image_opa(v->img, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor_opa(v->img, LV_OPA_TRANSP, 0);
    v->alive = true;
    v->pop = 0;
    view_badge(a, v);
}

static void view_place(cell_view_t *v)
{
    int px = v->x / FX16;
    int py = v->y / FX16;
    lv_obj_set_pos(v->img, px, py);
    if (v->badge && !lv_obj_has_flag(v->badge, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_pos(v->badge, px, py);
    }
}

static void view_scale(cell_view_t *v, int sx, int sy)
{
    lv_image_set_scale_x(v->img, sx);
    lv_image_set_scale_y(v->img, sy);
    if (v->badge && !lv_obj_has_flag(v->badge, LV_OBJ_FLAG_HIDDEN)) {
        lv_image_set_scale_x(v->badge, sx);
        lv_image_set_scale_y(v->badge, sy);
    }
}

static void view_hide(cell_view_t *v)
{
    lv_obj_add_flag(v->img, LV_OBJ_FLAG_HIDDEN);
    if (v->badge) {
        lv_obj_add_flag(v->badge, LV_OBJ_FLAG_HIDDEN);
    }
    v->alive = false;
}

/* Centre of a cell in screen coordinates, which is what the sparks and the
 * panels want (they live outside the board's container). */
static void cell_center(const app_t *a, int r, int c, int *x, int *y)
{
    *x = a->bx + c * a->cell + a->cell / 2;
    *y = a->by + r * a->cell + a->cell / 2;
}

/* The middle of the board, where the announcements go. */
static int board_cx(const app_t *a) { return a->bx + a->board_px / 2; }
static int board_cy(const app_t *a) { return a->by + a->board_px / 2; }


/* --------------------------------------------------------------------------
 * Score, levels and time
 * -------------------------------------------------------------------------- */

/* Width of an ASCII string in a font, without the private lv_text API. */
static int text_width(const lv_font_t *font, const char *txt)
{
    int w = 0;
    for (const char *p = txt; *p; p++) {
        w += lv_font_get_glyph_width(font, (uint8_t)p[0], (uint8_t)p[1]);
    }
    return w;
}

static void hud_refresh(app_t *a)
{
    char buf[56], num[24], num2[24];     /* "%s / %s": two of num and the 3 between */

    /* the score in 144 px digits while it fits the column, in 96 after:
     * 1.234.567 does not fit the HUD column lying down */
    format_score(buf, sizeof(buf), a->score);
    const lv_font_t *big = &aos_inter_num_144;
    if (text_width(big, buf) > a->bar_w) {
        big = &aos_inter_num_96;
    }
    if (lv_obj_get_style_text_font(a->lbl_score, 0) != big) {
        lv_obj_set_style_text_font(a->lbl_score, big, 0);
        /* the smaller one sits on the same middle line */
        lv_obj_set_y(a->lbl_score, a->score_y + (big == &aos_inter_num_144 ? 0 : 20));
    }
    lv_label_set_text(a->lbl_score, buf);

    snprintf(buf, sizeof(buf), _("Nivel %d"), a->level);
    lv_label_set_text(a->lbl_level, buf);

    format_score(num, sizeof(num), a->hiscore);
    snprintf(buf, sizeof(buf), _("Mejor %s"), num);
    lv_label_set_text(a->lbl_hud_best, buf);

    /* what is left to the next level, which is what the bar is about */
    format_score(num, sizeof(num), (uint32_t)clampi(a->level_pts, 0, a->level_need));
    format_score(num2, sizeof(num2), (uint32_t)a->level_need);
    snprintf(buf, sizeof(buf), "%s / %s", num, num2);
    lv_label_set_text(a->lbl_next, buf);

    int w = a->level_need ? (a->bar_w * a->level_pts / a->level_need) : 0;
    lv_obj_set_width(a->bar_level_fill, clampi(w, 0, a->bar_w));

    if (a->mode == 1) {
        int tw = a->bar_w * a->timebar / TIME_MAX;
        lv_obj_set_width(a->bar_time_fill, clampi(tw, 0, a->bar_w));
        /* from green to red as it runs out */
        uint32_t col = (a->timebar > TIME_MAX / 2) ? 0x30D158
                     : (a->timebar > TIME_MAX / 5) ? 0xFFD60A : 0xFF453A;
        lv_obj_set_style_bg_color(a->bar_time_fill, lv_color_hex(col), 0);
    }
}

static void level_up(app_t *a)
{
    a->level++;
    a->level_pts -= a->level_need;
    a->level_need = 900 + a->level * 550;
    a->timebar = TIME_MAX;
    a->tick_warn = 0;

    gm_snd_play(GM_SFX_LEVELUP, 0);
    gm_fx_flash(&a->fx, 90, 8);

    char buf[32];
    snprintf(buf, sizeof(buf), _("Nivel %d"), a->level);
    gm_fx_text(&a->fx, board_cx(a), board_cy(a) - a->cell / 2, buf,
               0xFFD60A, true);
}

static void add_score(app_t *a, int pts)
{
    if (pts <= 0) {
        return;
    }
    a->score += (uint32_t)pts;
    if (a->score > a->hiscore) {
        a->hiscore = a->score;
    }
    a->level_pts += pts;
    while (a->level_pts >= a->level_need) {
        level_up(a);
    }
}

/* --------------------------------------------------------------------------
 * Breaking the jewels
 * -------------------------------------------------------------------------- */

static const uint8_t s_pop_frames[] = {
    [POP_NORMAL] = 9,
    [POP_SUCK]   = 13,
    [POP_BURN]   = 12,
    [POP_BEAM]   = 10,
    [POP_WIPE]   = 14,
};

static void set_pop(app_t *a, int r, int c, int kind, int delay, int pr, int pc)
{
    cell_view_t *v = &a->v[r][c];
    if (!v->alive) {
        return;
    }
    v->kind  = (uint8_t)kind;
    v->pop0  = s_pop_frames[kind];
    v->pop   = v->pop0;
    v->delay = (uint8_t)delay;
    v->pr    = (int8_t)pr;
    v->pc    = (int8_t)pc;
    v->vy    = 0;
}

static int pop_total(const app_t *a)
{
    int worst = 0;
    for (int r = 0; r < GM_N; r++) {
        for (int c = 0; c < GM_N; c++) {
            const cell_view_t *v = &a->v[r][c];
            if (v->pop) {
                int total = v->delay + v->pop;
                if (total > worst) {
                    worst = total;
                }
            }
        }
    }
    return worst;
}

/* Effects of a special detonating. It is done here and not in gm_board because
 * it is pure decoration: the rules have already decided which cells go. */
static void special_effects(app_t *a, int r, int c, int special)
{
    int x, y;
    cell_center(a, r, c, &x, &y);

    if (special == GM_SP_FLAME) {
        gm_fx_ring(&a->fx, x, y, 0xFF8A14, 12, 74, 12, 6);
        gm_fx_burst(&a->fx, x, y, 0xFF6A00, 10, 46, false);
        gm_fx_burst(&a->fx, x, y, 0xFFD54A, 6, 30, false);
        gm_fx_shake(&a->fx, 8);
        gm_snd_play(GM_SFX_FLAME, 0);
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                int nr = r + dr, nc = c + dc;
                if (nr >= 0 && nr < GM_N && nc >= 0 && nc < GM_N &&
                    a->mark[nr][nc]) {
                    set_pop(a, nr, nc, POP_BURN, 0, r, c);
                }
            }
        }
    } else if (special == GM_SP_STAR) {
        gm_fx_beam(&a->fx, x, y, true, 0x5AD8FF, 9);
        gm_fx_beam(&a->fx, x, y, false, 0x5AD8FF, 9);
        gm_fx_burst(&a->fx, x, y, 0xFFFFFF, 8, 52, false);
        gm_fx_shake(&a->fx, 5);
        gm_snd_play(GM_SFX_STAR, 0);
        for (int i = 0; i < GM_N; i++) {
            if (a->mark[r][i]) {
                set_pop(a, r, i, POP_BEAM, (i > c ? i - c : c - i), r, c);
            }
            if (a->mark[i][c]) {
                set_pop(a, i, c, POP_BEAM, (i > r ? i - r : r - i), r, c);
            }
        }
    } else if (special == GM_SP_HYPER) {
        gm_fx_ring(&a->fx, x, y, 0xFFFFFF, 10, 200, 16, 8);
        gm_fx_flash(&a->fx, 70, 7);
        gm_fx_shake(&a->fx, 10);
        gm_snd_play(GM_SFX_HYPER, 0);
        for (int rr = 0; rr < GM_N; rr++) {
            for (int cc = 0; cc < GM_N; cc++) {
                if (!a->mark[rr][cc]) {
                    continue;
                }
                int dr = rr > r ? rr - r : r - rr;
                int dc = cc > c ? cc - c : c - cc;
                set_pop(a, rr, cc, POP_WIPE, (dr + dc), r, c);
            }
        }
    }
}

/* Starts the breaking of everything marked and hands out the points. */
static void begin_pop(app_t *a, int group_cells, int group_points,
                      int specials_made)
{
    int gems = gm_mark_count(a->mark);

    /* the ones the explosions took score too, more weakly */
    int pts = group_points + 40 * (gems - group_cells);
    int mult = clampi(a->combo, 1, COMBO_MAX);
    pts = pts * mult + specials_made;

    add_score(a, pts);

    if (a->mode == 1) {
        a->timebar = clampi(a->timebar + gems * s_diffs[a->diff].gain,
                            0, TIME_MAX);
    }

    a->state  = ST_POP;
    a->phase0 = pop_total(a) + 2;
    a->phase  = a->phase0;
}

/* Looks for completed lines and starts the breaking phase. If there are none,
 * the cascade is over. Returns true if something breaks. */
static bool resolve_step(app_t *a)
{
    gm_group_t g[GM_MAX_GROUPS];
    int ng = gm_find_groups(&a->b, g, GM_MAX_GROUPS, a->swap_r2, a->swap_c2);
    if (ng == 0) {
        return false;
    }

    a->combo++;
    memset(a->mark, 0, sizeof(a->mark));

    int group_cells = 0, group_points = 0, specials_made = 0;

    for (int i = 0; i < ng; i++) {
        for (int k = 0; k < g[i].n; k++) {
            a->mark[g[i].cell[k] / GM_N][g[i].cell[k] % GM_N] = 1;
        }
        group_cells += g[i].n;
        group_points += 40 * g[i].n + 30 * (g[i].n - 3);
    }

    /* The specials that were in the line detonate and take more cells with
     * them. Mind the order: expand first and place the new jewels afterwards,
     * or the newly created one would explode on the spot. */
    gm_expand_specials(&a->b, a->mark);

    for (int r = 0; r < GM_N; r++) {
        for (int c = 0; c < GM_N; c++) {
            if (a->mark[r][c]) {
                set_pop(a, r, c, POP_NORMAL, 0, r, c);
            }
        }
    }

    /* lines of four or more are pulled towards where the special will be born */
    for (int i = 0; i < ng; i++) {
        if (g[i].special == GM_SP_NONE) {
            continue;
        }
        for (int k = 0; k < g[i].n; k++) {
            int r = g[i].cell[k] / GM_N, c = g[i].cell[k] % GM_N;
            if (a->mark[r][c]) {
                set_pop(a, r, c, POP_SUCK, 0, g[i].pr, g[i].pc);
            }
        }
    }

    /* explosions: they override the break type of whatever they reach */
    for (int r = 0; r < GM_N; r++) {
        for (int c = 0; c < GM_N; c++) {
            if (a->mark[r][c] && a->b.c[r][c].special != GM_SP_NONE) {
                special_effects(a, r, c, a->b.c[r][c].special);
            }
        }
    }

    /* now, at last, the new jewels stay in their cell */
    for (int i = 0; i < ng; i++) {
        int x, y;
        cell_center(a, g[i].pr, g[i].pc, &x, &y);

        if (g[i].special != GM_SP_NONE) {
            a->b.c[g[i].pr][g[i].pc].type    = g[i].type;
            a->b.c[g[i].pr][g[i].pc].special = g[i].special;
            a->mark[g[i].pr][g[i].pc] = 0;
            a->v[g[i].pr][g[i].pc].pop = 0;

            specials_made += (g[i].special == GM_SP_FLAME) ? 150
                           : (g[i].special == GM_SP_STAR)  ? 300 : 500;

            gm_fx_ring(&a->fx, x, y, gm_art_color_light(g[i].type), 8, 46, 10, 4);
            gm_snd_play(g[i].special == GM_SP_FLAME ? GM_SFX_MATCH4
                                                    : GM_SFX_MATCH5, 0);
        } else {
            gm_snd_play(GM_SFX_MATCH, a->combo);
        }

        char buf[16];
        int mult = clampi(a->combo, 1, COMBO_MAX);
        snprintf(buf, sizeof(buf), "+%d", (40 * g[i].n + 30 * (g[i].n - 3)) * mult);
        gm_fx_text(&a->fx, x, y, buf, gm_art_color_light(g[i].type), false);

        for (int k = 0; k < g[i].n; k++) {
            int cx, cy;
            cell_center(a, g[i].cell[k] / GM_N, g[i].cell[k] % GM_N, &cx, &cy);
            gm_fx_burst(&a->fx, cx, cy, gm_art_color_light(g[i].type),
                        g[i].special != GM_SP_NONE ? 3 : 5, 34, true);
        }
    }

    if (a->combo >= 2) {
        char buf[32];
        snprintf(buf, sizeof(buf), _("Cadena x%d"), clampi(a->combo, 1, COMBO_MAX));
        gm_fx_text(&a->fx, board_cx(a), a->by + a->cell, buf, 0xFFD60A, true);
        gm_fx_shake(&a->fx, 4);
    }

    begin_pop(a, group_cells, group_points, specials_made);
    return true;
}

/* The hypercube forms no lines: it is activated by swapping it with a jewel,
 * and it takes every jewel of that colour. Two hypercubes together clear the
 * board. */
static void activate_hyper(app_t *a, int hr, int hc, int tr, int tc)
{
    a->combo++;
    memset(a->mark, 0, sizeof(a->mark));

    /* the hypercube itself is taken out of the equation before expanding:
     * otherwise gm_expand_specials would detonate it again and it would take
     * its own colour too */
    a->b.c[hr][hc].special = GM_SP_NONE;

    if (a->b.c[tr][tc].special == GM_SP_HYPER) {
        for (int r = 0; r < GM_N; r++) {
            for (int c = 0; c < GM_N; c++) {
                a->mark[r][c] = 1;
            }
        }
        gm_fx_flash(&a->fx, 140, 10);
    } else {
        gm_mark_color(&a->b, a->mark, a->b.c[tr][tc].type);
    }
    a->mark[hr][hc] = 1;

    gm_expand_specials(&a->b, a->mark);
    int gems = gm_mark_count(a->mark);

    for (int r = 0; r < GM_N; r++) {
        for (int c = 0; c < GM_N; c++) {
            if (a->mark[r][c]) {
                int dr = r > hr ? r - hr : hr - r;
                int dc = c > hc ? c - hc : hc - c;
                set_pop(a, r, c, POP_WIPE, dr + dc, hr, hc);
            }
        }
    }

    int x, y;
    cell_center(a, hr, hc, &x, &y);
    gm_fx_ring(&a->fx, x, y, 0xFFFFFF, 10, 230, 18, 9);
    gm_fx_burst(&a->fx, x, y, 0xFFFFFF, 12, 60, false);
    gm_fx_shake(&a->fx, 10);
    gm_snd_play(GM_SFX_HYPER, 0);

    begin_pop(a, gems, 55 * gems, 0);
}

/* --------------------------------------------------------------------------
 * The fall
 *
 * gm_collapse leaves the board already resolved and says, for each cell, which
 * row whatever is now there came from. With that, the views travel with their
 * LVGL object (so the jewel coming down is the same one that was above, and
 * not a new object with the same drawing) and the new jewels recycle the
 * objects of those that broke.
 * -------------------------------------------------------------------------- */

static void do_collapse(app_t *a)
{
    gm_fall_t fall;
    gm_collapse(&a->b, a->mark, &fall);

    for (int c = 0; c < GM_N; c++) {
        cell_view_t old[GM_N];
        bool used[GM_N];
        for (int r = 0; r < GM_N; r++) {
            old[r] = a->v[r][c];
            used[r] = false;
        }

        for (int r = 0; r < GM_N; r++) {
            int from = fall.from[r][c];
            if (from >= 0) {
                a->v[r][c] = old[from];
                used[from] = true;
            }
        }

        int spare = 0;
        for (int r = 0; r < GM_N; r++) {
            if (fall.from[r][c] >= 0) {
                continue;
            }
            while (spare < GM_N && used[spare]) {
                spare++;
            }
            if (spare >= GM_N) {
                break;      /* cannot happen: as many broke as are born */
            }
            used[spare] = true;

            cell_view_t *v = &a->v[r][c];
            *v = old[spare];
            v->x  = (int16_t)cell_home_x(a, c);
            v->y  = (int16_t)(-(fall.born[r][c] + 1) * a->cell * FX16 / 2);
            v->vy = 0;
        }

        for (int r = 0; r < GM_N; r++) {
            cell_view_t *v = &a->v[r][c];
            v->pop = 0;
            v->delay = 0;
            v->squash = 0;
            v->x = (int16_t)cell_home_x(a, c);

            bool nueva = (v->type != a->b.c[r][c].type ||
                          v->special != a->b.c[r][c].special ||
                          !v->alive);
            if (nueva) {
                view_sync(a, r, c);
                if (a->b.c[r][c].special != GM_SP_NONE && v->y >= 0) {
                    v->birth = 10;      /* the freshly created special bounces */
                }
            }
            view_scale(v, 256, 256);
            lv_image_set_rotation(v->img, 0);
            lv_obj_set_style_image_recolor_opa(v->img, LV_OPA_TRANSP, 0);
            view_place(v);
        }
    }

#ifdef AOS_SIM_BUILTIN
    if (getenv("GEMAS_TRACE")) {
        for (int r = 0; r < GM_N; r++) {
            for (int c = 0; c < GM_N; c++) {
                if (a->b.c[r][c].special != GM_SP_NONE) {
                    printf("[gemas] especial %d en %d,%d (vista badge=%p vis=%d)\n",
                           a->b.c[r][c].special, r, c, (void *)a->v[r][c].badge,
                           a->v[r][c].badge ? !lv_obj_has_flag(a->v[r][c].badge, LV_OBJ_FLAG_HIDDEN) : -1);
                }
            }
        }
    }
#endif
    a->state = ST_FALL;
}

/* The cascade is over: back to waiting for the player. */
static void cascade_end(app_t *a)
{
    a->combo = 0;
    a->swap_r2 = a->swap_c2 = -1;

    if (!gm_has_move(&a->b)) {
        a->state = ST_SHUFFLE;
        a->phase0 = 26;
        a->phase  = a->phase0;
        gm_snd_play(GM_SFX_SHUFFLE, 0);
        gm_fx_text(&a->fx, board_cx(a), board_cy(a),
                   _("Sin jugadas"), 0xFFD60A, true);
        return;
    }

    a->state = ST_IDLE;
    a->idle_frames = 0;
}

/* --------------------------------------------------------------------------
 * One frame of each phase
 * -------------------------------------------------------------------------- */

static void try_swap(app_t *a, int r1, int c1, int r2, int c2);
static void game_start(app_t *a);
static void pause_open(app_t *a);
static void pad_step(app_t *a);

/* easing: 0..256 -> 0..256 with a start and a stop */
static int ease(int t)
{
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    return (t * t * (768 - 2 * t)) / (256 * 256);
}

static void step_swap(app_t *a)
{
    cell_view_t *v1 = &a->v[a->swap_r1][a->swap_c1];
    cell_view_t *v2 = &a->v[a->swap_r2][a->swap_c2];

    a->phase--;
    int t = ease((a->phase0 - a->phase) * 256 / a->phase0);

    int h1x = cell_home_x(a, a->swap_c1), h1y = cell_home_y(a, a->swap_r1);
    int h2x = cell_home_x(a, a->swap_c2), h2y = cell_home_y(a, a->swap_r2);

    v1->x = (int16_t)(h1x + (h2x - h1x) * t / 256);
    v1->y = (int16_t)(h1y + (h2y - h1y) * t / 256);
    v2->x = (int16_t)(h2x + (h1x - h2x) * t / 256);
    v2->y = (int16_t)(h2y + (h1y - h2y) * t / 256);

    /* the one travelling forwards passes over the top and grows a little */
    int bump = 256 + (t < 128 ? t : 256 - t) / 4;
    view_scale(v1, bump, bump);
    view_place(v1);
    view_place(v2);

    if (a->phase > 0) {
        return;
    }

    view_scale(v1, 256, 256);

    /* the swap only takes effect now, both on the board and in the views: that
     * way each jewel goes on being the same LVGL object */
    gm_cell_t tmp = a->b.c[a->swap_r1][a->swap_c1];
    a->b.c[a->swap_r1][a->swap_c1] = a->b.c[a->swap_r2][a->swap_c2];
    a->b.c[a->swap_r2][a->swap_c2] = tmp;

    cell_view_t vtmp = *v1;
    *v1 = *v2;
    *v2 = vtmp;
    v1->x = (int16_t)h1x; v1->y = (int16_t)h1y;
    v2->x = (int16_t)h2x; v2->y = (int16_t)h2y;
    view_place(v1);
    view_place(v2);

    if (a->state == ST_UNSWAP) {
        a->state = ST_IDLE;
        a->idle_frames = 0;
        return;
    }

    /* the hypercube is activated by swapping it, not by lining it up */
    if (a->b.c[a->swap_r2][a->swap_c2].special == GM_SP_HYPER) {
        activate_hyper(a, a->swap_r2, a->swap_c2, a->swap_r1, a->swap_c1);
        return;
    }
    if (a->b.c[a->swap_r1][a->swap_c1].special == GM_SP_HYPER) {
        activate_hyper(a, a->swap_r1, a->swap_c1, a->swap_r2, a->swap_c2);
        return;
    }

    if (!resolve_step(a)) {
        /* it formed nothing: it comes back by itself and sounds like a no */
        int8_t r = a->swap_r1, c = a->swap_c1;
        a->swap_r1 = a->swap_r2; a->swap_c1 = a->swap_c2;
        a->swap_r2 = r;          a->swap_c2 = c;
        a->state  = ST_UNSWAP;
        a->phase0 = 7;
        a->phase  = a->phase0;
        gm_snd_play(GM_SFX_DENY, 0);
    }
}

static void step_pop(app_t *a)
{
    for (int r = 0; r < GM_N; r++) {
        for (int c = 0; c < GM_N; c++) {
            cell_view_t *v = &a->v[r][c];
            if (v->pop == 0) {
                continue;
            }
            if (v->delay > 0) {
                v->delay--;
                continue;
            }

            v->pop--;
            int p = v->pop * 256 / v->pop0;      /* 256 at the start, 0 at the end */

            switch (v->kind) {
            case POP_SUCK: {
                int tx = cell_home_x(a, v->pc), ty = cell_home_y(a, v->pr);
                v->x = (int16_t)(v->x + (tx - v->x) * 5 / 16);
                v->y = (int16_t)(v->y + (ty - v->y) * 5 / 16);
                int s = 40 + p * 216 / 256;
                view_scale(v, s, s);
                break;
            }
            case POP_BURN:
                lv_obj_set_style_image_recolor(v->img, lv_color_hex(0xFFE8B0), 0);
                lv_obj_set_style_image_recolor_opa(v->img,
                    (lv_opa_t)(255 - p * 255 / 256), 0);
                {
                    int s = 256 + (256 - p) * 120 / 256;
                    view_scale(v, s, s);
                }
                break;

            case POP_BEAM: {
                int s = 60 + p * 196 / 256;
                view_scale(v, 256 + (256 - p) / 2, s);
                lv_obj_set_style_image_recolor(v->img, lv_color_hex(0x9AE8FF), 0);
                lv_obj_set_style_image_recolor_opa(v->img,
                    (lv_opa_t)(255 - p * 255 / 256), 0);
                break;
            }
            case POP_WIPE: {
                int s = 30 + p * 226 / 256;
                view_scale(v, s, s);
                lv_image_set_rotation(v->img, (v->pop0 - v->pop) * 320);
                break;
            }
            case POP_NORMAL:
            default: {
                int s = 256 + (256 - p) * 170 / 256;
                view_scale(v, s, s);
                break;
            }
            }

            lv_obj_set_style_image_opa(v->img, (lv_opa_t)(p > 255 ? 255 : p), 0);
            view_place(v);

            if (v->pop == 0) {
                view_hide(v);
            }
        }
    }

    a->phase--;
    if (a->phase <= 0) {
        do_collapse(a);
    }
}

static void step_fall(app_t *a)
{
    bool moving = false;

    for (int c = 0; c < GM_N; c++) {
        for (int r = 0; r < GM_N; r++) {
            cell_view_t *v = &a->v[r][c];
            int home = cell_home_y(a, r);

            if (v->y < home) {
                /* the watch's gravity, in proportion to the cell: the
                 * fall takes the same time, it just covers more pixels */
                v->vy = (int16_t)(v->vy + SC(34));
                if (v->vy > SC(340)) {
                    v->vy = (int16_t)SC(340);
                }
                v->y = (int16_t)(v->y + v->vy);
                if (v->y >= home) {
                    v->y = (int16_t)home;
                    v->squash = (uint8_t)(v->vy > SC(140) ? 4 : 2);
                    v->vy = 0;
                }
                moving = true;
                view_place(v);
            } else if (v->squash > 0) {
                /* squashing on landing, just barely: overdoing it makes the
                 * jewel spill out of its cell and overlap its neighbour */
                v->squash--;
                int k = v->squash * 9;
                view_scale(v, 256 + k, 256 - k);
                moving = true;
            } else if (v->birth > 0) {
                v->birth--;
                int k = v->birth * 10;
                view_scale(v, 256 + k, 256 + k);
                moving = true;
            }
        }
    }

    if (moving) {
        return;
    }

    if (!resolve_step(a)) {
        cascade_end(a);
    }
}

static void step_shuffle(app_t *a)
{
    a->phase--;

    int half = a->phase0 / 2;
    if (a->phase == half) {
        gm_board_shuffle(&a->b);
        for (int r = 0; r < GM_N; r++) {
            for (int c = 0; c < GM_N; c++) {
                view_sync(a, r, c);
            }
        }
    }

    /* They go out and come back on. It could rotate them, but rotating or
     * scaling are the same expensive LVGL path (drawing into a separate layer
     * and transforming it) and here it would be all sixty-four at once;
     * opacity, by contrast, is one more blend in the same blit. */
    int t = (a->phase > half) ? (a->phase - half) * 255 / half
                              : (half - a->phase) * 255 / half;
    lv_opa_t opa = (lv_opa_t)clampi(255 - t, 25, 255);
    for (int r = 0; r < GM_N; r++) {
        for (int c = 0; c < GM_N; c++) {
            lv_obj_set_style_image_opa(a->v[r][c].img, opa, 0);
            if (a->v[r][c].badge &&
                !lv_obj_has_flag(a->v[r][c].badge, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_set_style_image_opa(a->v[r][c].badge, opa, 0);
            }
        }
    }

    if (a->phase <= 0) {
        for (int r = 0; r < GM_N; r++) {
            for (int c = 0; c < GM_N; c++) {
                lv_obj_set_style_image_opa(a->v[r][c].img, LV_OPA_COVER, 0);
                if (a->v[r][c].badge) {
                    lv_obj_set_style_image_opa(a->v[r][c].badge, LV_OPA_COVER, 0);
                }
            }
        }
        a->state = ST_IDLE;
        a->idle_frames = 0;
    }
}

/* If the player sits and stares, a move is pointed out to them. */
static void step_idle(app_t *a)
{
    a->idle_frames++;

#ifdef AOS_SIM_BUILTIN
    /* the move served up by GEMAS_TEST plays itself after a second, which is
     * plenty of time to start taking screenshots */
    if (a->test_mv[0] >= 0 && a->idle_frames > 30) {
        int8_t mv[4] = { a->test_mv[0], a->test_mv[1], a->test_mv[2], a->test_mv[3] };
        a->test_mv[0] = -1;
        try_swap(a, mv[0], mv[1], mv[2], mv[3]);
        return;
    }

    /* GEMAS_AUTO=1 makes the game play itself. It is not a game mode: it is
     * the only convenient way of watching a thousand cascades in a row while
     * tuning the animations, and of leaving it running for a good while to see
     * that it neither hangs nor runs out of moves. */
    if (getenv("GEMAS_AUTO") && a->idle_frames > 8) {
        uint8_t mv[4];
        if (gm_find_move(&a->b, mv)) {
            try_swap(a, mv[0], mv[1], mv[2], mv[3]);
            return;
        }
    }
#endif

    if (a->idle_frames == 200) {
        uint8_t mv[4];
        if (gm_find_move(&a->b, mv)) {
            a->hint[0] = mv[0]; a->hint[1] = mv[1];
            a->hint[2] = mv[2]; a->hint[3] = mv[3];
            a->hint_on = true;
        }
    }

    if (!a->hint_on) {
        return;
    }

    /* two jewels pulsing together */
    int t = a->idle_frames % 40;
    int k = (t < 20) ? t : 40 - t;
    int s = 256 + k * 4;
    for (int i = 0; i < 2; i++) {
        cell_view_t *v = &a->v[a->hint[i * 2]][a->hint[i * 2 + 1]];
        view_scale(v, s, s);
    }
}

static void hint_clear(app_t *a)
{
    if (!a->hint_on) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        view_scale(&a->v[a->hint[i * 2]][a->hint[i * 2 + 1]], 256, 256);
    }
    a->hint_on = false;
}

/* --------------------------------------------------------------------------
 * Time, game over and the loop
 * -------------------------------------------------------------------------- */

static void panel_show(app_t *a, lv_obj_t *panel);
static void panel_hide_all(app_t *a);
static void over_refresh(app_t *a, const char *title);
static int  over_score_y(void);
static void hud_place(app_t *a);

static void game_over(app_t *a, const char *why)
{
    a->state = ST_OVER;
    a->over_why = why;
    a->sel_r = a->sel_c = -1;
    lv_obj_add_flag(a->sel_ring, LV_OBJ_FLAG_HIDDEN);
    gm_snd_play(GM_SFX_GAMEOVER, 0);
    gm_fx_flash(&a->fx, 120, 10);
    over_refresh(a, why);
    panel_show(a, a->over);

    aos_hal_pref_set_i32(a->mode == 1 ? KEY_HI_TIME : KEY_HI_RELAX,
                         (int32_t)a->hiscore);
}

static void step_time(app_t *a)
{
    if (a->mode != 1) {
        return;
    }

    const diff_t *d = &s_diffs[a->diff];
    a->timebar -= d->drain + d->drain_step * (a->level - 1);

    if (a->timebar <= 0) {
        a->timebar = 0;
        game_over(a, _("Se acabó el tiempo"));
        return;
    }

    /* warning: one tick a second when less than a fifth is left */
    if (a->timebar < TIME_MAX / 5) {
        if (--a->tick_warn <= 0) {
            a->tick_warn = 30;
            gm_snd_play(GM_SFX_TICK, 0);
        }
    } else {
        a->tick_warn = 0;
    }
}

static void apply_shake(app_t *a)
{
    int dx = 0, dy = 0;
    if (a->fx.shake > 0) {
        int amp = SC(3);
        dx = (int)(gm_rnd(&a->b) % (uint32_t)(2 * amp + 1)) - amp;
        dy = (int)(gm_rnd(&a->b) % (uint32_t)(2 * amp + 1)) - amp;
    } else if (lv_obj_get_x(a->board) == a->bx && lv_obj_get_y(a->board) == a->by) {
        return;     /* still: nothing to move */
    }
    lv_obj_set_pos(a->board, a->bx + dx, a->by + dy);
}

/* The score's texts are only rewritten when they change: every
 * lv_label_set_text dirties its area and sends it to be repainted. */
static void hud_tick(app_t *a)
{
    if (a->score != a->last_score || a->level != a->last_level) {
        a->last_score = a->score;
        a->last_level = a->level;
        hud_refresh(a);
        a->last_lvl_w = a->last_time_w = -1;
        return;
    }

    int w = a->level_need ? (a->bar_w * a->level_pts / a->level_need) : 0;
    w = clampi(w, 0, a->bar_w);
    if (w != a->last_lvl_w) {
        a->last_lvl_w = w;
        lv_obj_set_width(a->bar_level_fill, w);
    }

    if (a->mode == 1) {
        int tw = clampi(a->bar_w * a->timebar / TIME_MAX, 0, a->bar_w);
        if (tw != a->last_time_w) {
            a->last_time_w = tw;
            lv_obj_set_width(a->bar_time_fill, tw);
            uint32_t col = (a->timebar > TIME_MAX / 2) ? 0x30D158
                         : (a->timebar > TIME_MAX / 5) ? 0xFFD60A : 0xFF453A;
            lv_obj_set_style_bg_color(a->bar_time_fill, lv_color_hex(col), 0);
        }
    }
}

static void frame(lv_timer_t *timer)
{
    app_t *a = (app_t *)lv_timer_get_user_data(timer);

    if (a->want_exit) {
        /* aos_ui_back() destroys the app: after this 'a' no longer exists */
        a->want_exit = false;
        aos_ui_back();
        return;
    }

    gm_snd_tick();
    pad_step(a);
    if (a->want_exit) {
        return;                 /* "Salir" from the pad: the next frame leaves */
    }

#ifdef AOS_SIM_BUILTIN
    if (getenv("GEMAS_TRACE")) {
        static int n;
        printf("[gemas] cuadro %d estado=%d fase=%d combo=%d\n",
               n++, (int)a->state, a->phase, a->combo);
    }
#endif

    switch (a->state) {
    case ST_SWAP:
    case ST_UNSWAP:  step_swap(a);    break;
    case ST_POP:     step_pop(a);     break;
    case ST_FALL:    step_fall(a);    break;
    case ST_SHUFFLE: step_shuffle(a); break;
    case ST_IDLE:    step_idle(a);    break;
    default: break;
    }

    if (a->state != ST_MENU && a->state != ST_PAUSE && a->state != ST_OVER) {
        step_time(a);
    }

    /* the selection ring pulses so it can be seen that it is held */
    if (a->sel_r >= 0) {
        /* narrowed to 32 bits before dividing: dividing a uint64_t drags in __udivdi3 */
        uint32_t ms = (uint32_t)aos_hal_uptime_ms();
        int t = (int)((ms / 60u) % 20u);
        int k = (t < 10) ? t : 20 - t;
        lv_obj_set_style_border_opa(a->sel_ring, (lv_opa_t)(150 + k * 10), 0);
    }

    gm_fx_step(&a->fx);
    apply_shake(a);
    hud_tick(a);
}

/* --------------------------------------------------------------------------
 * Input
 *
 * It can be played the two ways anybody expects: tapping a jewel and then its
 * neighbour, or dragging a jewel sideways. The drag is resolved at a quarter
 * of a cell (22 px here), well before the 50 px LVGL needs to call it a
 * gesture, so they never clash.
 * -------------------------------------------------------------------------- */

static void sel_show(app_t *a, int r, int c)
{
    a->sel_r = (int8_t)r;
    a->sel_c = (int8_t)c;
    lv_obj_set_pos(a->sel_ring, c * a->cell, r * a->cell);
    lv_obj_remove_flag(a->sel_ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(a->sel_ring);
}

static void sel_clear(app_t *a)
{
    a->sel_r = a->sel_c = -1;
    lv_obj_add_flag(a->sel_ring, LV_OBJ_FLAG_HIDDEN);
}

static void try_swap(app_t *a, int r1, int c1, int r2, int c2)
{
    if (a->state != ST_IDLE) {
        return;
    }
    if (r2 < 0 || r2 >= GM_N || c2 < 0 || c2 >= GM_N) {
        return;
    }

    hint_clear(a);
    sel_clear(a);

    a->swap_r1 = (int8_t)r1; a->swap_c1 = (int8_t)c1;
    a->swap_r2 = (int8_t)r2; a->swap_c2 = (int8_t)c2;
    a->state  = ST_SWAP;
    a->phase0 = 8;
    a->phase  = a->phase0;
    gm_snd_play(GM_SFX_SWAP, 0);
}

static bool point_to_cell(const app_t *a, int px, int py, int *r, int *c)
{
    int x = px - a->bx;
    int y = py - a->by;
    if (x < 0 || y < 0 || x >= a->board_px || y >= a->board_px) {
        return false;
    }
    *c = x / a->cell;
    *r = y / a->cell;
    return true;
}

static void touch_event(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        a->pressing = false;
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t area;
    lv_obj_get_coords(a->touch, &area);
    int px = p.x - area.x1;
    int py = p.y - area.y1;

    if (code == LV_EVENT_PRESSED) {
        int r, c;
        a->pressing  = false;
        a->drag_done = false;
        /* a finger again: the pad's frame steps aside */
        a->cur_on = false;
        lv_obj_add_flag(a->pad_ring, LV_OBJ_FLAG_HIDDEN);
        if (!point_to_cell(a, px, py, &r, &c) || a->state != ST_IDLE) {
            return;
        }
        a->pressing = true;
        a->press_x  = (int16_t)px;
        a->press_y  = (int16_t)py;
        a->press_r  = (int8_t)r;
        a->press_c  = (int8_t)c;
        hint_clear(a);

        if (a->sel_r >= 0) {
            int dr = r - a->sel_r, dc = c - a->sel_c;
            int adr = dr < 0 ? -dr : dr;
            int adc = dc < 0 ? -dc : dc;
            if (adr + adc == 1) {
                try_swap(a, a->sel_r, a->sel_c, r, c);
                return;
            }
            if (adr + adc == 0) {
                sel_clear(a);           /* tapping the same one releases it */
                return;
            }
        }
        sel_show(a, r, c);
        gm_snd_play(GM_SFX_SELECT, 0);
        return;
    }

    /* LV_EVENT_PRESSING: dragging a jewel sideways */
    if (!a->pressing || a->drag_done || a->state != ST_IDLE) {
        return;
    }
    int dx = px - a->press_x;
    int dy = py - a->press_y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (adx < a->cell / 4 && ady < a->cell / 4) {
        return;
    }

    a->drag_done = true;
    int r2 = a->press_r, c2 = a->press_c;
    if (adx > ady) {
        c2 += (dx > 0) ? 1 : -1;
    } else {
        r2 += (dy > 0) ? 1 : -1;
    }
    try_swap(a, a->press_r, a->press_c, r2, c2);
}

/* The gamepad: the frame sits on the pad's cell, unless that jewel is taken,
 * when the white ring alone says where it is. */
static void pad_ring_show(app_t *a)
{
    bool show = a->cur_on && a->sel_r < 0 && a->state != ST_MENU &&
                a->state != ST_PAUSE && a->state != ST_OVER;
    if (!show) {
        lv_obj_add_flag(a->pad_ring, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_pos(a->pad_ring, a->cur_c * a->cell, a->cur_r * a->cell);
    lv_obj_remove_flag(a->pad_ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(a->pad_ring);
}

static void pad_board(app_t *a)
{
    const aos_pad_t *p = &a->pad;
    uint32_t dirs = p->repeat & AOS_PAD_DIRS;
    bool btn_a = aos_pad_pressed(p, AOS_PAD_A);
    bool btn_b = aos_pad_pressed(p, AOS_PAD_B);
    if (!dirs && !btn_a && !btn_b) {
        return;
    }
    if (!a->cur_on) {
        /* the first press only shows where the frame is */
        a->cur_on = true;
        pad_ring_show(a);
        return;
    }
    int dr = (dirs & AOS_PAD_DOWN) ? 1 : (dirs & AOS_PAD_UP) ? -1 : 0;
    int dc = dr ? 0 : (dirs & AOS_PAD_RIGHT) ? 1 : (dirs & AOS_PAD_LEFT) ? -1 : 0;

    if (a->sel_r >= 0) {
        /* a jewel taken: B or A lets it go, a direction swaps it there */
        if (btn_a || btn_b) {
            sel_clear(a);
        } else if (dr || dc) {
            int r2 = a->sel_r + dr, c2 = a->sel_c + dc;
            if (r2 >= 0 && r2 < GM_N && c2 >= 0 && c2 < GM_N && a->state == ST_IDLE) {
                a->cur_r = (int8_t)r2;
                a->cur_c = (int8_t)c2;
                try_swap(a, a->sel_r, a->sel_c, r2, c2);
            }
        }
        pad_ring_show(a);
        return;
    }
    if (btn_a) {
        if (a->state == ST_IDLE) {
            hint_clear(a);
            sel_show(a, a->cur_r, a->cur_c);
            gm_snd_play(GM_SFX_SELECT, 0);
        }
    } else if (dr || dc) {
        int r = a->cur_r + dr, c = a->cur_c + dc;
        if (r >= 0 && r < GM_N && c >= 0 && c < GM_N) {
            a->cur_r = (int8_t)r;
            a->cur_c = (int8_t)c;
        }
    }
    pad_ring_show(a);
}

/* Once a frame. */
static void pad_step(app_t *a)
{
    aos_pad_update(&a->pad, lv_tick_get());
    if (!a->pad.pressed && !a->pad.repeat) {
        return;
    }
    bool start = aos_pad_pressed(&a->pad, AOS_PAD_START);
    switch (a->state) {
    case ST_MENU:
    case ST_OVER:
        if (start) {
            /* "Jugar" / "Otra vez" */
            game_start(a);
        } else {
            aos_pad_menu_step(&a->pmenu, &a->pad);
        }
        break;
    case ST_PAUSE:
        if (start || aos_pad_pressed(&a->pad, AOS_PAD_B)) {
            a->state = a->resume;
            panel_hide_all(a);
            pad_ring_show(a);
        } else {
            aos_pad_menu_step(&a->pmenu, &a->pad);
        }
        break;
    default:
        if (start) {
            pause_open(a);
            pad_ring_show(a);
        } else {
            pad_board(a);
        }
        break;
    }
}

/* With AOS_APP_FLAG_NO_SWIPE the back gesture is handled by the app. While
 * playing it does not count: dragging is how the jewels are moved, and the
 * way out of a game is the pause button. */
static void touch_gesture(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();

    if (!indev || lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) {
        return;
    }
    if (a->state != ST_MENU) {
        return;
    }
    lv_indev_wait_release(indev);
    a->want_exit = true;
}

/* --------------------------------------------------------------------------
 * Panels and buttons
 *
 * The game is all images, but the menus are text to read and touch: there
 * LVGL's vector typography and the usual large buttons are the right choice.
 * P4OS: iOS-like cards on black, every target at least 88 px tall.
 * -------------------------------------------------------------------------- */

static lv_obj_t *make_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int x, int y,
                             int w, int h, uint32_t color, const lv_font_t *font,
                             lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = make_box(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = make_label(btn, text, font, lv_color_hex(0xFFFFFF));
    lv_obj_center(label);
    return btn;
}

/* A settings row: the name on the left, dim, and the value on the right in
 * its colour. Tapping anywhere on it moves to the next value. */
static lv_obj_t *make_chip(lv_obj_t *parent, const char *key, int x, int y,
                           int w, uint32_t bg, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *chip = make_box(parent);
    lv_obj_set_size(chip, w, ROW_H);
    lv_obj_set_pos(chip, x, y);
    lv_obj_set_style_bg_color(chip, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(chip, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(chip, AOS_UI_RADIUS, 0);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(chip, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *k = make_label(chip, key, aos_font_title, AOS_C_DIM);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 36, 0);

    lv_obj_t *v = make_label(chip, "", aos_font_title, AOS_C_TEXT);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -36, 0);
    return chip;
}

static void chip_set(lv_obj_t *chip, const char *text, uint32_t color)
{
    lv_obj_t *label = lv_obj_get_child(chip, 1);
    if (label) {
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    }
}

/* A dialog: a scrim over the whole app (it also swallows the touches meant
 * for the board) and a card in the middle. Returns the card; the panel that
 * is shown and hidden is its parent, the scrim. */
static lv_obj_t *make_dialog(app_t *a, int w, int h, uint32_t border)
{
    lv_obj_t *scrim = make_box(a->root);
    lv_obj_set_size(scrim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_60, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *p = make_box(scrim);
    lv_obj_set_size(p, w, h);
    /* over the board, whichever way the screen is */
    lv_obj_set_pos(p, a->bx + (a->board_px - w) / 2, a->by + (a->board_px - h) / 2);
    lv_obj_set_style_bg_color(p, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 36, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(border), 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    return p;
}

static lv_obj_t *make_text(lv_obj_t *parent, const char *text,
                           const lv_font_t *font, uint32_t color, int y)
{
    lv_obj_t *label = make_label(parent, text, font, lv_color_hex(color));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_y(label, y);
    return label;
}

static void panel_hide_all(app_t *a)
{
    aos_pad_menu_clear(&a->pmenu);
    lv_obj_t *const panels[] = { a->menu, a->pause, a->over };
    for (unsigned i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
        if (panels[i]) {
            lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void panel_show(app_t *a, lv_obj_t *panel)
{
    panel_hide_all(a);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(panel);
    if (a->pad_ring) {
        lv_obj_add_flag(a->pad_ring, LV_OBJ_FLAG_HIDDEN);
    }

    /* the pad goes through its buttons, from the main one */
    if (panel == a->menu) {
        aos_pad_menu_set(&a->pmenu, a->pm_menu, 4, 3);
    } else if (panel == a->pause) {
        aos_pad_menu_set(&a->pmenu, a->pm_pause, 4, 0);
    } else if (panel == a->over) {
        aos_pad_menu_set(&a->pmenu, a->pm_over, 3, 0);
    }
}

/* --------------------------------------------------------------------------
 * Menu, pause and game over
 * -------------------------------------------------------------------------- */

static void prefs_save(app_t *a)
{
    aos_hal_pref_set_i32(KEY_MODE, a->mode);
    aos_hal_pref_set_i32(KEY_DIFF, a->diff);
    aos_hal_pref_set_i32(KEY_SFX, gm_snd_enabled() ? 1 : 0);
}

static const char *mode_name(int mode)
{
    return mode == 1 ? _("Contrarreloj") : _("Sin apuro");
}

static void sfx_chip_refresh(lv_obj_t *chip)
{
    chip_set(chip, gm_snd_enabled() ? _("Sí") : _("No"),
             gm_snd_enabled() ? 0x0A84FF : 0x8E8E93);
}

static void menu_refresh(app_t *a)
{
    chip_set(a->chip_mode, mode_name(a->mode),
             a->mode == 1 ? 0xFF9F0A : 0x30D158);
    chip_set(a->chip_diff, _(s_diffs[a->diff].name),
             a->diff == 0 ? 0x30D158 : (a->diff == 1 ? 0x0A84FF : 0xFF453A));
    sfx_chip_refresh(a->chip_sfx);

    int32_t best = 0;
    aos_hal_pref_get_i32(a->mode == 1 ? KEY_HI_TIME : KEY_HI_RELAX, &best);
    char buf[48], num[24];
    format_score(num, sizeof(num), (uint32_t)(best > 0 ? best : 0));
    snprintf(buf, sizeof(buf), _("Mejor %s"), num);
    lv_label_set_text(a->lbl_best, buf);
}

static void over_refresh(app_t *a, const char *title)
{
    lv_label_set_text(a->lbl_over_title, title);

    char buf[96], num[24], best[24];
    int32_t hi = 0;
    aos_hal_pref_get_i32(a->mode == 1 ? KEY_HI_TIME : KEY_HI_RELAX, &hi);
    format_score(num, sizeof(num), a->score);
    format_score(best, sizeof(best),
                 (uint32_t)(hi > (int32_t)a->score ? hi : (int32_t)a->score));
    /* the same digits as the HUD while they fit the card, a size down after */
    const lv_font_t *big = &aos_inter_num_144;
    if (text_width(big, num) > DLG_W - 2 * DLG_IN) {
        big = &aos_inter_num_96;
    }
    lv_obj_set_style_text_font(a->lbl_over_score, big, 0);
    lv_obj_set_y(a->lbl_over_score, over_score_y() + (big == &aos_inter_num_144 ? 0 : 20));
    lv_label_set_text(a->lbl_over_score, num);
    snprintf(buf, sizeof(buf), _("Nivel %d  ·  Mejor %s"), a->level, best);
    lv_label_set_text(a->lbl_over_info, buf);
}

/* The mode line of the HUD, and which bars it carries. */
static void hud_mode_refresh(app_t *a)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s  ·  %s", mode_name(a->mode),
             _(s_diffs[a->diff].name));
    lv_label_set_text(a->lbl_mode, buf);
    if (a->mode == 1) {
        lv_obj_remove_flag(a->bar_time, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a->bar_time, LV_OBJ_FLAG_HIDDEN);
    }
    hud_place(a);           /* with or without the clock, the bars move */
}

/* Puts every view in its cell, whole and still, from the board. */
static void views_home(app_t *a)
{
    for (int r = 0; r < GM_N; r++) {
        for (int c = 0; c < GM_N; c++) {
            cell_view_t *v = &a->v[r][c];
            v->x = (int16_t)cell_home_x(a, c);
            v->y = (int16_t)cell_home_y(a, r);
            v->vy = 0;
            v->pop = 0;
            v->delay = 0;
            v->squash = 0;
            v->birth = 0;
            view_sync(a, r, c);
            view_scale(v, 256, 256);
            view_place(v);
        }
    }
}

static void game_show(app_t *a)
{
    lv_obj_remove_flag(a->hud, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(a->board, LV_OBJ_FLAG_HIDDEN);
    hud_mode_refresh(a);
    a->last_score = 0xFFFFFFFFu;        /* the next frame rewrites the HUD */
    hud_refresh(a);
}

static void game_start(app_t *a)
{
    gm_board_init(&a->b, (uint32_t)aos_hal_uptime_ms() ^ 0xA5A5u,
                  s_diffs[a->diff].colors);
    gm_board_fill(&a->b);

    a->live       = true;
    a->over_why   = NULL;
    a->score      = 0;
    a->level      = 1;
    a->level_pts  = 0;
    a->level_need = 1450;
    a->timebar    = TIME_MAX;
    a->combo      = 0;
    a->tick_warn  = 0;
    a->hint_on    = false;
    a->swap_r2    = -1;
    a->swap_c2    = -1;

    int32_t hi = 0;
    aos_hal_pref_get_i32(a->mode == 1 ? KEY_HI_TIME : KEY_HI_RELAX, &hi);
    a->hiscore = (uint32_t)(hi > 0 ? hi : 0);

    sel_clear(a);
    gm_fx_clear(&a->fx);
    panel_hide_all(a);

    /* the initial deal is the same fall as always: each column starts a little
     * higher than the one beside it and the board fills itself */
    for (int r = 0; r < GM_N; r++) {
        for (int c = 0; c < GM_N; c++) {
            cell_view_t *v = &a->v[r][c];
            v->x = (int16_t)cell_home_x(a, c);
            v->y = (int16_t)(-(GM_N - r + (c % 3)) * a->cell * FX16 / 2);
            v->vy = 0;
            v->pop = 0;
            v->delay = 0;
            v->squash = 0;
            v->birth = 0;
            view_sync(a, r, c);
            view_scale(v, 256, 256);
            lv_image_set_rotation(v->img, 0);
            view_place(v);
        }
    }

    game_show(a);
    gm_snd_play(GM_SFX_START, 0);
    a->state = ST_FALL;
    pad_ring_show(a);
}

static void go_menu(app_t *a)
{
    a->state = ST_MENU;
    a->live  = false;
    sel_clear(a);
    gm_fx_clear(&a->fx);
    menu_refresh(a);
    panel_show(a, a->menu);
}

static void cb_play(lv_event_t *e)     { game_start((app_t *)lv_event_get_user_data(e)); }
static void cb_menu(lv_event_t *e)     { go_menu((app_t *)lv_event_get_user_data(e)); }

static void cb_exit(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->live = false;            /* leaving on purpose: next time, the menu */
    a->exiting = true;
    a->want_exit = true;        /* leaving here would destroy the app inside its own callback */
}

static void cb_mode(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->mode = a->mode ? 0 : 1;
    prefs_save(a);
    menu_refresh(a);
    gm_snd_play(GM_SFX_SELECT, 0);
}

static void cb_diff(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->diff = (a->diff + 1) % 3;
    prefs_save(a);
    menu_refresh(a);
    gm_snd_play(GM_SFX_SELECT, 0);
}

static void cb_sfx(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    gm_snd_enable(!gm_snd_enabled());
    prefs_save(a);
    menu_refresh(a);
    sfx_chip_refresh(a->chip_sfx2);
    gm_snd_play(GM_SFX_SELECT, 0);
}

static void pause_open(app_t *a)
{
    if (a->state == ST_MENU || a->state == ST_OVER || a->state == ST_PAUSE) {
        return;
    }
    a->resume = a->state;
    a->state  = ST_PAUSE;
    sfx_chip_refresh(a->chip_sfx2);
    panel_show(a, a->pause);
}

static void cb_pause(lv_event_t *e)  { pause_open((app_t *)lv_event_get_user_data(e)); }

static void cb_resume(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->state = a->resume;
    panel_hide_all(a);
    pad_ring_show(a);
}

static void cb_retry(lv_event_t *e)  { game_start((app_t *)lv_event_get_user_data(e)); }

/* --------------------------------------------------------------------------
 * Building the screen
 * -------------------------------------------------------------------------- */

/* Where everything goes. Upright: the board edge to edge across the width,
 * low, where the thumb reaches, and the HUD filling the space above it.
 * Lying down: the board on the left filling the height and the HUD as a
 * column on the right. */
static void layout(app_t *a, lv_obj_t *root)
{
    int w = (int)lv_obj_get_width(root);
    int h = (int)lv_obj_get_height(root);

    a->land = w > h;
    if (a->land) {
        /* centred in the height: it overlaps the two edge strips by a few
         * pixels of the outer rows, which beats a smaller board */
        a->cell = (h - 2 * PAD) / GM_N;
        a->board_px = a->cell * GM_N;
        a->bx = PAD;
        a->by = (h - a->board_px) / 2;
    } else {
        a->cell = (w - 2 * PAD_BOARD_P) / GM_N;
        /* a root shorter than the screen still leaves the HUD its room */
        if (a->cell * GM_N > h - HUD_MIN - EDGE_TOP - EDGE_BOTTOM) {
            a->cell = (h - HUD_MIN - EDGE_TOP - EDGE_BOTTOM) / GM_N;
        }
        a->board_px = a->cell * GM_N;
        a->bx = (w - a->board_px) / 2;
        a->by = h - a->board_px - EDGE_BOTTOM;
    }
}

/* Stacks blocks of the given heights down [y0, y1): equal gaps between them,
 * as wide as the room allows up to 'gmax', and the whole centred in what is
 * left. It is how the HUD and the menu fill the screen whichever way it is
 * turned without a table of positions per orientation. */
static void stack(const int *hs, int *ys, int n, int y0, int y1, int gmin, int gmax)
{
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += hs[i];
    }
    int gap = (n > 1) ? (y1 - y0 - sum) / (n - 1) : 0;
    gap = clampi(gap, gmin, gmax);
    int y = y0 + (y1 - y0 - sum - gap * (n - 1)) / 2;
    if (y < y0) {
        y = y0;
    }
    for (int i = 0; i < n; i++) {
        ys[i] = y;
        y += hs[i] + gap;
    }
}

static int line_h(const lv_font_t *font)
{
    return (int)lv_font_get_line_height(font);
}

static void pause_icon(lv_obj_t *parent, int size)
{
    /* two bars, drawn: the glyph would depend on the font carrying it */
    int bw = size / 9, bh = size * 3 / 8;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *bar = make_box(parent);
        lv_obj_set_size(bar, bw, bh);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, bw / 2, 0);
        lv_obj_align(bar, LV_ALIGN_CENTER, (i ? 1 : -1) * bw, 0);
    }
}

/* The HUD in three groups: the level beside the pause button at the top,
 * the score in the middle and the bars at the bottom, next to the board.
 * Whatever the board leaves goes between them, so upright the HUD reaches
 * from the top strip down to the board, and without the clock the level's
 * bar drops to the board instead of leaving a hole. */
static void hud_place(app_t *a)
{
    int hw = a->bar_w;
    int lh_level = line_h(aos_font_large);
    int lh_mode  = line_h(aos_font_body);
    int lh_best  = line_h(aos_font_title);
    int lh_cap   = line_h(aos_font_body);

    int top_h = PAUSE_D;
    if (lh_level + lh_mode > top_h) {
        top_h = lh_level + lh_mode;
    }
    int score_h = line_h(&aos_inter_num_144) + lh_best;
    int bars_h  = lh_cap + 12 + HUD_BAR_H + (a->mode == 1 ? 20 + HUD_TIME_H : 0);

    int hs[3] = { top_h, score_h, bars_h }, ys[3];
    stack(hs, ys, 3, 0, a->hud_h, 12, 72);

    int ty = ys[0] + (top_h - lh_level - lh_mode) / 2;
    lv_obj_set_pos(a->lbl_level, 0, ty);
    lv_obj_set_pos(a->lbl_mode, 0, ty + lh_level);
    lv_obj_set_pos(a->btn_pause, hw - PAUSE_D, ys[0] + (top_h - PAUSE_D) / 2);

    a->score_y = ys[1];
    bool small = lv_obj_get_style_text_font(a->lbl_score, 0) != &aos_inter_num_144;
    lv_obj_set_y(a->lbl_score, a->score_y + (small ? 20 : 0));
    lv_obj_set_y(a->lbl_hud_best, ys[1] + line_h(&aos_inter_num_144));

    int y = ys[2];
    lv_obj_set_y(a->lbl_next_cap, y);
    lv_obj_set_y(a->lbl_next, y);
    y += lh_cap + 12;
    lv_obj_set_y(a->bar_level, y);
    y += HUD_BAR_H + 20;
    lv_obj_set_y(a->bar_time, y);
}

static void build_hud(app_t *a, lv_obj_t *root)
{
    int w = (int)lv_obj_get_width(root);
    int h = (int)lv_obj_get_height(root);
    int hx, hy, hw, hh;

    if (a->land) {
        hx = a->bx + a->board_px + 48;
        hw = w - PAD - 16 - hx;
        hy = EDGE_TOP;
        hh = h - EDGE_TOP - EDGE_BOTTOM;
    } else {
        hx = a->bx + 16;
        hw = a->board_px - 32;
        hy = EDGE_TOP;          /* below the top strip, which pulls the notifications */
        hh = a->by - 28 - hy;
    }
    a->bar_w = hw;
    a->hud_h = hh;

    a->hud = make_box(root);
    lv_obj_set_size(a->hud, hw, hh);
    lv_obj_set_pos(a->hud, hx, hy);
    lv_obj_add_flag(a->hud, LV_OBJ_FLAG_HIDDEN);

    /* top row: the level and the mode on the left, pause on the right */
    a->lbl_level = make_label(a->hud, "", aos_font_large, AOS_C_TEXT);
    a->lbl_mode = make_label(a->hud, "", aos_font_body, AOS_C_DIM);

    a->btn_pause = make_box(a->hud);
    lv_obj_set_size(a->btn_pause, PAUSE_D, PAUSE_D);
    lv_obj_set_style_bg_color(a->btn_pause, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(a->btn_pause, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(a->btn_pause, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(a->btn_pause, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(a->btn_pause, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(a->btn_pause, 16);
    lv_obj_add_event_cb(a->btn_pause, cb_pause, LV_EVENT_CLICKED, a);
    pause_icon(a->btn_pause, PAUSE_D);

    /* the score, big, in the middle */
    a->lbl_score = make_label(a->hud, "0", &aos_inter_num_144, AOS_C_TEXT);
    lv_obj_set_style_text_align(a->lbl_score, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(a->lbl_score, hw);
    lv_label_set_long_mode(a->lbl_score, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_x(a->lbl_score, 0);

    a->lbl_hud_best = make_label(a->hud, "", aos_font_title, AOS_C_DIM);
    lv_obj_set_style_text_align(a->lbl_hud_best, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(a->lbl_hud_best, hw);
    lv_obj_set_x(a->lbl_hud_best, 0);

    /* the bars: the level's progress, and the clock in time attack */
    a->lbl_next_cap = make_label(a->hud, _("Próximo nivel"), aos_font_body, AOS_C_DIM);
    lv_obj_set_x(a->lbl_next_cap, 0);
    a->lbl_next = make_label(a->hud, "", aos_font_body, AOS_C_DIM);
    lv_obj_set_style_text_align(a->lbl_next, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(a->lbl_next, hw / 2);
    lv_obj_set_x(a->lbl_next, hw / 2);

    struct { lv_obj_t **bar, **fill; int h; uint32_t color; } bars[] = {
        { &a->bar_level, &a->bar_level_fill, HUD_BAR_H,  0x0A84FF },
        { &a->bar_time,  &a->bar_time_fill,  HUD_TIME_H, 0x30D158 },
    };
    for (unsigned i = 0; i < sizeof(bars) / sizeof(bars[0]); i++) {
        lv_obj_t *bar = make_box(a->hud);
        lv_obj_set_size(bar, hw, bars[i].h);
        lv_obj_set_x(bar, 0);
        lv_obj_set_style_bg_color(bar, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, bars[i].h / 2, 0);

        lv_obj_t *fill = make_box(bar);
        lv_obj_set_size(fill, 0, bars[i].h);
        lv_obj_set_pos(fill, 0, 0);
        lv_obj_set_style_bg_color(fill, lv_color_hex(bars[i].color), 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(fill, bars[i].h / 2, 0);

        *bars[i].bar = bar;
        *bars[i].fill = fill;
    }
    hud_place(a);
}

/* How the specials are made. It is the one thing about the game that cannot
 * be guessed by looking at it, and upright the menu has the room. */
static const struct {
    int8_t      gem;
    uint8_t     special;
    const char *how;
    const char *what;
} s_legend[] = {
    { 0, GM_SP_FLAME, N_("Cuatro en línea"), N_("Llama: explota alrededor") },
    { 2, GM_SP_STAR,  N_("En L o en T"),     N_("Estrella: fila y columna") },
    { 6, GM_SP_HYPER, N_("Cinco en línea"),  N_("Hipercubo: todo un color") },
};
#define LEGEND_N    ((int)(sizeof(s_legend) / sizeof(s_legend[0])))
#define LEGEND_PAD  12

static lv_obj_t *menu_image(lv_obj_t *parent, const lv_image_dsc_t *src,
                            int size, int x, int y)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_set_size(img, size, size);
    lv_obj_set_pos(img, x, y);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

static int legend_height(const app_t *a)
{
    return LEGEND_N * (a->cell + 8) + 2 * LEGEND_PAD;
}

/* A card with one row per special: the jewel as it looks on the board, what
 * makes it and what it does. */
static void build_legend(app_t *a, lv_obj_t *parent, int x, int y, int w)
{
    int row = a->cell + 8;
    lv_obj_t *card = make_box(parent);
    lv_obj_set_size(card, w, legend_height(a));
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, AOS_UI_RADIUS, 0);

    int lh1 = line_h(aos_font_body), lh2 = line_h(aos_font_small);
    int tx = 24 + a->cell + 20;
    for (int i = 0; i < LEGEND_N; i++) {
        int ry = LEGEND_PAD + i * row;
        const lv_image_dsc_t *src = (s_legend[i].special == GM_SP_HYPER)
                                  ? &a->art.hyper.dsc : &a->art.gem[s_legend[i].gem].dsc;
        menu_image(card, src, a->cell, 24, ry + 4);
        if (s_legend[i].special == GM_SP_FLAME) {
            menu_image(card, &a->art.flame.dsc, a->cell, 24, ry + 4);
        } else if (s_legend[i].special == GM_SP_STAR) {
            menu_image(card, &a->art.star.dsc, a->cell, 24, ry + 4);
        }

        int ty = ry + (row - lh1 - lh2) / 2;
        lv_obj_t *how = make_label(card, _(s_legend[i].how), aos_font_body, AOS_C_TEXT);
        lv_obj_set_pos(how, tx, ty);
        lv_obj_t *what = make_label(card, _(s_legend[i].what), aos_font_small, AOS_C_DIM);
        lv_obj_set_pos(what, tx, ty + lh1);
        /* a longer translation is cut, it never runs off the card */
        lv_obj_set_width(how, w - tx - 24);
        lv_obj_set_width(what, w - tx - 24);
        lv_label_set_long_mode(how, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_long_mode(what, LV_LABEL_LONG_MODE_DOTS);
    }
}

static void build_menu(app_t *a, lv_obj_t *root)
{
    int w = (int)lv_obj_get_width(root);
    int h = (int)lv_obj_get_height(root);

    lv_obj_t *p = make_box(root);
    lv_obj_set_size(p, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(p, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p, touch_gesture, LV_EVENT_GESTURE, a);
    a->menu = p;

    /* Five blocks: the title, a sample of the jewels, the specials, the
     * settings and the play button. All in one column upright, filling the
     * height; lying down the first three on the left and the other two on
     * the right. */
    int lh_huge = line_h(aos_font_huge), lh_body = line_h(aos_font_body);
    enum { B_TITLE, B_GEMS, B_LEGEND, B_SET, B_PLAY, B_N };
    int hs[B_N], ys[B_N], xs[B_N], ws[B_N];
    hs[B_TITLE]  = lh_huge + 4 + lh_body;
    hs[B_GEMS]   = a->cell;
    hs[B_LEGEND] = legend_height(a);
    hs[B_SET]    = 3 * ROW_H + 2 * 16;
    hs[B_PLAY]   = BTN_H + 24 + lh_body;

    int y0 = EDGE_TOP, y1 = h - EDGE_BOTTOM;
    if (a->land) {
        int cw = w / 2 - 72;
        stack(hs, ys, 3, y0, y1, 24, 56);
        stack(hs + B_SET, ys + B_SET, 2, y0, y1, 40, 56);
        for (int i = 0; i < B_N; i++) {
            xs[i] = (i < B_SET) ? 48 : w / 2 + 24;
            ws[i] = cw;
        }
    } else {
        stack(hs, ys, B_N, y0 + 16, y1, 24, 64);
        for (int i = 0; i < B_N; i++) {
            xs[i] = 48;
            ws[i] = w - 96;
        }
    }

    lv_obj_t *col = make_box(p);
    lv_obj_set_size(col, ws[B_TITLE], hs[B_TITLE]);
    lv_obj_set_pos(col, xs[B_TITLE], ys[B_TITLE]);
    make_text(col, _("Gemas"), aos_font_huge, 0xFFFFFF, 0);
    make_text(col, _("Alineá tres o más"), aos_font_body, 0x8E8E93, lh_huge + 4);

    /* a sample of the seven jewels: it is what best says what this is about.
     * Spread over the column, and a hair closer than a cell if it is narrow
     * (the jewel fills the cell but for its air) */
    int cw = ws[B_GEMS];
    int step = (cw - a->cell) / (GM_TYPES - 1);
    if (step > a->cell + 8) {
        step = a->cell + 8;
    }
    int gx = xs[B_GEMS] + (cw - (GM_TYPES - 1) * step - a->cell) / 2;
    for (int i = 0; i < GM_TYPES; i++) {
        menu_image(p, &a->art.gem[i].dsc, a->cell, gx + i * step, ys[B_GEMS]);
    }

    build_legend(a, p, xs[B_LEGEND], ys[B_LEGEND], ws[B_LEGEND]);

    lv_obj_t *set = make_box(p);
    lv_obj_set_size(set, ws[B_SET], hs[B_SET]);
    lv_obj_set_pos(set, xs[B_SET], ys[B_SET]);
    int sw = ws[B_SET];
    a->chip_mode = make_chip(set, _("Modo"),       0, 0,                 sw, 0x1C1C1E, cb_mode, a);
    a->chip_diff = make_chip(set, _("Dificultad"), 0, ROW_H + 16,        sw, 0x1C1C1E, cb_diff, a);
    a->chip_sfx  = make_chip(set, _("Sonido"),     0, 2 * (ROW_H + 16),  sw, 0x1C1C1E, cb_sfx,  a);

    lv_obj_t *play = make_box(p);
    lv_obj_set_size(play, ws[B_PLAY], hs[B_PLAY]);
    lv_obj_set_pos(play, xs[B_PLAY], ys[B_PLAY]);
    a->pm_menu[0] = a->chip_mode;
    a->pm_menu[1] = a->chip_diff;
    a->pm_menu[2] = a->chip_sfx;
    a->pm_menu[3] = make_button(play, _("Jugar"), 0, 0, ws[B_PLAY], BTN_H, 0x30D158,
                                aos_font_large, cb_play, a);
    a->lbl_best = make_text(play, "", aos_font_body, 0x8E8E93, BTN_H + 24);
}

static void build_pause(app_t *a)
{
    int iw = DLG_W - 2 * DLG_IN;
    int hb = (iw - 16) / 2;
    int y = 36 + line_h(aos_font_huge) + 36;
    lv_obj_t *p = make_dialog(a, DLG_W, y + BTN_H + 16 + ROW_H + 16 + ROW_H + DLG_IN,
                              0x3A3A46);
    a->pause = lv_obj_get_parent(p);

    make_text(p, _("Pausa"), aos_font_huge, 0xFFFFFF, 36);
    a->pm_pause[0] = make_button(p, _("Seguir"), DLG_IN, y, iw, BTN_H, 0x30D158,
                                 aos_font_large, cb_resume, a);
    y += BTN_H + 16;
    a->chip_sfx2 = make_chip(p, _("Sonido"), DLG_IN, y, iw, 0x2C2C2E, cb_sfx, a);
    a->pm_pause[1] = a->chip_sfx2;
    y += ROW_H + 16;
    a->pm_pause[2] = make_button(p, _("Menú"),  DLG_IN, y, hb, ROW_H, 0x0A84FF,
                                 aos_font_title, cb_menu, a);
    a->pm_pause[3] = make_button(p, _("Salir"), DLG_IN + hb + 16, y, hb, ROW_H, 0xFF453A,
                                 aos_font_title, cb_exit, a);
}

static int over_score_y(void)
{
    return 36 + line_h(aos_font_title) + 8;
}

static void build_over(app_t *a)
{
    int iw = DLG_W - 2 * DLG_IN;
    int hb = (iw - 16) / 2;
    int y_info = over_score_y() + line_h(&aos_inter_num_144) + 4;
    int y_btn = y_info + line_h(aos_font_body) + 36;
    lv_obj_t *p = make_dialog(a, DLG_W, y_btn + BTN_H + 16 + ROW_H + DLG_IN, 0xFF453A);
    a->over = lv_obj_get_parent(p);

    a->lbl_over_title = make_text(p, "", aos_font_title, 0xFF6A5A, 36);
    a->lbl_over_score = make_text(p, "", &aos_inter_num_144, 0xFFFFFF, over_score_y());
    a->lbl_over_info  = make_text(p, "", aos_font_body, 0x8E8E93, y_info);

    a->pm_over[0] = make_button(p, _("Otra vez"), DLG_IN, y_btn, iw, BTN_H, 0x30D158,
                                aos_font_large, cb_retry, a);
    int y = y_btn + BTN_H + 16;
    a->pm_over[1] = make_button(p, _("Menú"),  DLG_IN, y, hb, ROW_H, 0x0A84FF,
                                aos_font_title, cb_menu, a);
    a->pm_over[2] = make_button(p, _("Salir"), DLG_IN + hb + 16, y, hb, ROW_H, 0xFF453A,
                                aos_font_title, cb_exit, a);
}

/* --------------------------------------------------------------------------
 * Coming back after the screen turned
 *
 * The runtime rebuilt the app in the new size and the game was left in some
 * phase of an animation whose views no longer exist. Each phase is brought to
 * a point from which the ordinary loop can carry on with the views in their
 * cells, and the game comes back paused: the person turned the screen, and
 * had not asked for the clock to run meanwhile.
 * -------------------------------------------------------------------------- */

static state_t settle(app_t *a, state_t st)
{
    switch (st) {
    case ST_SWAP:
        /* the swap had not reached the board yet */
        return ST_IDLE;
    case ST_UNSWAP: {
        /* it had: the jewels were on their way back */
        gm_cell_t tmp = a->b.c[a->swap_r1][a->swap_c1];
        a->b.c[a->swap_r1][a->swap_c1] = a->b.c[a->swap_r2][a->swap_c2];
        a->b.c[a->swap_r2][a->swap_c2] = tmp;
        return ST_IDLE;
    }
    case ST_SHUFFLE:
        if (!gm_has_move(&a->b)) {
            gm_board_shuffle(&a->b);
        }
        return ST_IDLE;
    case ST_POP:
        /* the marks survived with the board: they fall now (below) */
        return ST_POP;
    case ST_FALL:
    case ST_IDLE:
        return st;
    default:
        return ST_IDLE;
    }
}

static void game_restore(app_t *a)
{
    state_t st = (a->state == ST_PAUSE) ? a->resume : a->state;
    bool over = (a->state == ST_OVER);

    if (!over) {
        st = settle(a, st);
    }
    views_home(a);
    game_show(a);

    if (over) {
        over_refresh(a, a->over_why ? a->over_why : "");
        panel_show(a, a->over);
        return;
    }
    if (st == ST_POP) {
        do_collapse(a);         /* leaves ST_FALL, with the new jewels above */
        st = ST_FALL;
    }
    a->state = st;
    a->idle_frames = 0;
    a->hint_on = false;
    pause_open(a);
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static void prefs_load(app_t *a)
{
    int32_t v = 0;
    if (aos_hal_pref_get_i32(KEY_MODE, &v)) {
        a->mode = (v == 1) ? 1 : 0;
    }
    if (aos_hal_pref_get_i32(KEY_DIFF, &v)) {
        a->diff = clampi((int)v, 0, 2);
    }
    if (aos_hal_pref_get_i32(KEY_SFX, &v)) {
        gm_snd_enable(v != 0);
    }
}

static void *gemas_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;

    app_t *a = &s_app;
    /* the view half starts from zero; the game half is kept (see app_t) */
    memset(&a->v, 0, sizeof(app_t) - offsetof(app_t, v));
    a->root  = root;
    a->sel_r = a->sel_c = -1;
    layout(a, root);

    /* The sprites add up to some 250 KB and go through malloc(), not through
     * lv_malloc(): LVGL's pool is internal RAM, and with
     * CONFIG_SPIRAM_USE_MALLOC this lands in PSRAM, which is where it
     * belongs. */
    if (!gm_art_init(&a->art, a->cell)) {
        return NULL;
    }

    gm_snd_init();
    if (!a->live) {
        prefs_load(a);          /* mid-game, the mode is the game's */
    } else {
        int32_t v = 0;
        if (aos_hal_pref_get_i32(KEY_SFX, &v)) {
            gm_snd_enable(v != 0);
        }
    }

    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* the board's frame */
    lv_obj_t *frame_obj = make_box(root);
    lv_obj_set_size(frame_obj, a->board_px + 12, a->board_px + 12);
    lv_obj_set_pos(frame_obj, a->bx - 6, a->by - 6);
    lv_obj_set_style_radius(frame_obj, 22, 0);
    lv_obj_set_style_border_width(frame_obj, 3, 0);
    lv_obj_set_style_border_color(frame_obj, lv_color_hex(0x2A3350), 0);

    /* the board: a container of its own so it can be shaken as a whole, and it
     * also clips the jewels that are still falling in from above */
    a->board = make_box(root);
    lv_obj_set_size(a->board, a->board_px, a->board_px);
    lv_obj_set_pos(a->board, a->bx, a->by);
    lv_obj_add_flag(a->board, LV_OBJ_FLAG_HIDDEN);

    a->bg = lv_image_create(a->board);
    lv_image_set_src(a->bg, &a->art.tile.dsc);
    lv_obj_set_size(a->bg, a->board_px, a->board_px);
    lv_obj_set_pos(a->bg, 0, 0);
    lv_image_set_inner_align(a->bg, LV_IMAGE_ALIGN_TILE);
    lv_obj_remove_flag(a->bg, LV_OBJ_FLAG_CLICKABLE);

    for (int r = 0; r < GM_N; r++) {
        for (int c = 0; c < GM_N; c++) {
            a->v[r][c].img = make_image(a);
            a->v[r][c].type = 0;
            a->v[r][c].special = GM_SP_NONE;
            lv_obj_add_flag(a->v[r][c].img, LV_OBJ_FLAG_HIDDEN);
        }
    }

    a->sel_ring = make_box(a->board);
    lv_obj_set_size(a->sel_ring, a->cell, a->cell);
    lv_obj_set_style_radius(a->sel_ring, a->cell / 4, 0);
    lv_obj_set_style_border_width(a->sel_ring, 5, 0);
    lv_obj_set_style_border_color(a->sel_ring, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_flag(a->sel_ring, LV_OBJ_FLAG_HIDDEN);

    a->pad_ring = make_box(a->board);
    lv_obj_set_size(a->pad_ring, a->cell, a->cell);
    lv_obj_set_style_radius(a->pad_ring, a->cell / 4, 0);
    lv_obj_set_style_border_width(a->pad_ring, 5, 0);
    lv_obj_set_style_border_color(a->pad_ring, lv_color_hex(0xFFD60A), 0);
    lv_obj_add_flag(a->pad_ring, LV_OBJ_FLAG_HIDDEN);
    a->cur_r = a->cur_c = GM_N / 2;

    gm_fx_init(&a->fx, root, a->cell * 256 / GM_CELL_REF);
    gm_fx_set_area(&a->fx, a->bx, a->by, a->board_px, a->board_px, 0);

    /* touch layer: it covers the screen, but only does anything over the board */
    a->touch = make_box(root);
    lv_obj_set_size(a->touch, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(a->touch, 0, 0);
    lv_obj_add_flag(a->touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->touch, touch_event, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->touch, touch_event, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(a->touch, touch_event, LV_EVENT_RELEASED, a);
    lv_obj_add_event_cb(a->touch, touch_event, LV_EVENT_PRESS_LOST, a);
    lv_obj_add_event_cb(a->touch, touch_gesture, LV_EVENT_GESTURE, a);

    build_hud(a, root);
    build_menu(a, root);
    build_pause(a);
    build_over(a);

    if (a->live && a->state != ST_MENU) {
        game_restore(a);
    } else {
        go_menu(a);
    }

#ifdef AOS_SIM_BUILTIN
    /* Shortcuts for looking at an animation without having to provoke it by
     * playing:
     *
     *   GEMAS_TEST=4     starts with a line of four served up
     *   GEMAS_TEST=5     one of five
     *   GEMAS_TEST=L     an L (leaves a star)
     *   GEMAS_TEST=H     a hypercube beside a pile of its colour
     *   GEMAS_TIME=1     time-attack mode
     *
     * Only on the first create: after a rotation the game carries on.
     */
    static bool s_tested;
    a->test_mv[0] = -1;
    const char *test = s_tested ? NULL : getenv("GEMAS_TEST");
    const char *timed = s_tested ? NULL : getenv("GEMAS_TIME");
    s_tested = true;
    if (timed) {
        a->mode = 1;
    }
    if (test) {
        game_start(a);

        /* a background with no completed lines: two neighbouring cells never match */
        for (int r = 0; r < GM_N; r++) {
            for (int c = 0; c < GM_N; c++) {
                a->b.c[r][c].type = (int8_t)((r + 2 * c) % a->b.ncolors);
                a->b.c[r][c].special = GM_SP_NONE;
            }
        }

        /* in every case the move is dropping the jewel from (3,3) to (4,3) */
        a->b.c[3][3].type = 0;
        a->test_mv[0] = 3; a->test_mv[1] = 3;
        a->test_mv[2] = 4; a->test_mv[3] = 3;

        switch (test[0]) {
        case '4':   /* line of four: leaves a flame */
            a->b.c[4][1].type = 0; a->b.c[4][2].type = 0; a->b.c[4][4].type = 0;
            break;
        case '5':   /* line of five: leaves a hypercube */
            a->b.c[4][1].type = 0; a->b.c[4][2].type = 0;
            a->b.c[4][4].type = 0; a->b.c[4][5].type = 0;
            break;
        case 'L':   /* cross: leaves a star */
            a->b.c[4][2].type = 0; a->b.c[4][4].type = 0;
            a->b.c[5][3].type = 0; a->b.c[6][3].type = 0;
            break;
        case 'F':   /* a flame already made, to watch it detonate */
            a->b.c[4][1].type = 0; a->b.c[4][2].type = 0;
            a->b.c[4][2].special = GM_SP_FLAME;
            break;
        case 'S':   /* a star already made */
            a->b.c[4][1].type = 0; a->b.c[4][2].type = 0;
            a->b.c[4][2].special = GM_SP_STAR;
            break;
        case 'H':   /* hypercube: swapped with its neighbour, it takes that colour */
            a->b.c[3][3].special = GM_SP_HYPER;
            break;
        default:
            break;
        }

        views_home(a);
        a->state = ST_IDLE;
        a->idle_frames = 0;
    } else if (timed && atoi(timed) > 1) {
        /* GEMAS_TIME=400 starts with the bar nearly empty: it is the quick way
         * to reach the game over panel without playing for a minute */
        game_start(a);
        a->timebar = atoi(timed);
    } else if (timed) {
        menu_refresh(a);        /* the chip has to reflect the mode */
    }

    /* GEMAS_SCORE=1234567 starts a game with that score: the layout audit of
     * the HUD with a long number */
    const char *score = getenv("GEMAS_SCORE");
    if (score && a->state == ST_MENU) {
        game_start(a);
        a->score = a->hiscore = (uint32_t)atoi(score);
    }
#endif

    int frame_ms = FRAME_MS;
#ifdef AOS_SIM_BUILTIN
    /* GEMAS_SLOW=120 lengthens the frame: useful for watching an animation
     * calmly, or for taking screenshots of it without it getting away */
    if (getenv("GEMAS_SLOW")) {
        frame_ms = atoi(getenv("GEMAS_SLOW"));
        if (frame_ms < 10) {
            frame_ms = FRAME_MS;
        }
    }
#endif
    aos_pad_reset(&a->pad, lv_tick_get());
    a->timer = lv_timer_create(frame, frame_ms, a);
    return a;
}

static void gemas_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return;
    }
    if (a->timer) {
        lv_timer_delete(a->timer);
        a->timer = NULL;
    }
    if (a->hiscore > 0) {
        int32_t hi = 0;
        aos_hal_pref_get_i32(a->mode == 1 ? KEY_HI_TIME : KEY_HI_RELAX, &hi);
        if ((int32_t)a->hiscore > hi) {
            aos_hal_pref_set_i32(a->mode == 1 ? KEY_HI_TIME : KEY_HI_RELAX,
                                 (int32_t)a->hiscore);
        }
    }
    gm_snd_stop();

    /* The objects go HERE, before the sprites they point at: the runtime
     * deletes the root only after destroy(), and anything drawn in between
     * would read freed pixels. */
    if (a->root) {
        lv_obj_clean(a->root);
        a->root = NULL;
    }
    gm_art_free(&a->art);
}

/* Leaving in the middle of a game should not cost the board: it pauses. */
static void gemas_hide(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a) {
        pause_open(a);
    }
}

static bool gemas_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return false;
    }
    if (a->exiting || a->state == ST_MENU) {
        return false;           /* let the runtime close the app */
    }
    if (a->state == ST_PAUSE || a->state == ST_OVER) {
        return true;            /* the panels already have their own buttons */
    }
    pause_open(a);
    return true;
}

static bool gemas_init(aos_app_t *app)
{
    app->desc.id      = "demo.gemas";
    app->desc.name    = "Gemas";
    app->desc.icon    = LV_SYMBOL_SHUFFLE;
    app->desc.icon_vec = AOS_ICON_GEM;
    app->desc.color_a = 0xE81E32;
    app->desc.color_b = 0x2E7BFF;
    app->desc.order   = 147;
    /* No orientation flag: both. The game survives the turn (see app_t). */
    app->desc.flags   = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN |
                        AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;

    app->create  = gemas_create;
    app->destroy = gemas_destroy;
    app->hide    = gemas_hide;
    app->back    = gemas_back;
    return true;
}

AOS_APP_ENTRY(gemas_init);
