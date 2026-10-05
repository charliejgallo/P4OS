/*
 * P4OS - Atasco (from AmoledOS)
 *
 * A sliding block puzzle in the style of Rush Hour: the red car has to get out
 * of the car park to the right, but the way is full of other cars that only
 * slide along their own axis. 25 levels out of the box, of increasing and
 * verified difficulty (see tools/at_harness.c), and adding more means adding a
 * text map to at_levels.c: there is no code per level.
 *
 * How it is put together inside:
 *
 *   at_game   the rules: parsing the map, slide range, winning. It depends on
 *             neither LVGL nor the HAL (it is bench-tested with plain 'cc').
 *   at_cars   draws each car in code: a handful of lv_obj (body, cabin,
 *             wheels, an ornament depending on the model), never bitmaps.
 *   at_levels each level's text map.
 *
 * And what is left here is what joins them: two screens (level menu and board)
 * plus the victory panel, and the drag: a transparent layer over the board
 * that follows the finger along the axis of the car touched and only on
 * release decides which cell it ended up in.
 *
 * On the 5" panel the board is 100 px a cell (the watch had 50): a two-cell
 * car is two finger widths long. Portrait stacks bar, board and an undo
 * button; landscape puts the board on the left -the exit is on its right, so
 * the car drives out towards the panel- and the controls in a card on the
 * right. The menu shows the 25 bays at once in a 5x5 grid with a "keep
 * playing" button. The game itself lives in statics (s_keep) so turning the
 * screen, which rebuilds the app, does not lose the half-solved car park.
 * New here: undo, one move at a time.
 *
 * A USB gamepad plays it as well. On the board the d-pad walks a yellow
 * frame over the cells (it shows on the first press and steps aside at the
 * next touch); A on a car takes it - the frame turns green and wraps the
 * car - and then the d-pad slides it along its axis, one cell a press, until
 * A or B lets it go: the whole slide is one move, as with a finger. B with
 * nothing taken undoes, R starts the level over and START goes to the
 * levels. The menu and the victory panel go through aos_pad_menu.
 */
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"
#include "aos_theme.h"
#include "aos_sys_glyphs.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"

#include "at_cars.h"
#include "at_game.h"
#include "at_levels.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Measurements
 * -------------------------------------------------------------------------- */

#define CELL        100
#define GAP         6
#define BOARD_PX    (AT_GRID * CELL)                  /* 600 */
#define WALL        14      /* thickness of the kerb around the lot */
#define MARGIN      16
#define BTN         88      /* round buttons, the comfortable minimum */
#define UNDO_MAX    256
#define WIN_W       560     /* the victory card */
#define WIN_H       320

/* The car park's palette. The asphalt is dark grey and not black: against the
 * system's pure black, the lot stands out by itself and the gap of the exit
 * reads without needing a border drawn around it. */
#define C_ASPHALT   0x3A3A3E
#define C_LANE      0x46464C      /* the target's lane, just a shade lighter */
#define C_WALL      0x9A9AA2      /* the kerb, lit side                       */
#define C_WALL_LO   0x6A6A72      /* ... and its shadow                       */
#define C_MARK      0xD2D2D8      /* the crosses marking the bays             */
#define C_HAZARD    0xFFC81F      /* the hazard tape of the gap               */
#define C_CURSOR    0xFFD60A      /* the gamepad's frame ...                  */
#define C_GRAB      0x30D158      /* ... and while it holds a car             */

#define KEY_UNLOCKED "atasco_unlk"

typedef enum {
    ST_MENU = 0,
    ST_PLAY,
    ST_WIN,
} at_state_t;

typedef struct {
    int8_t idx;
    int8_t from;          /* the coordinate that moves, before the move */
} at_undo_t;

/* What survives the app being built again (the screen turned, the app left
 * and came back): where the player was and the board as it stood. */
static struct {
    bool       valid;
    at_state_t state;
    int        level;
    int        moves;
    at_board_t board;
    at_undo_t  undo[UNDO_MAX];
    int        undo_n;
} s_keep;

typedef struct {
    aos_app_t *self;
    at_state_t state;
    int32_t    W, H;
    bool       land;

    /* --- game --- */
    at_board_t board;
    int        level;      /* 0-based index of the current level */
    int        moves;
    int        unlocked;   /* highest playable index (0-based); completing N unlocks N+1 */
    at_undo_t  undo[UNDO_MAX];
    int        undo_n;

    /* --- where the board sits --- */
    int  board_x, board_y;
    int  apron_end;         /* x where the asphalt beyond the exit stops */

    /* --- drag --- */
    bool dragging;
    int  drag_idx;
    int  drag_start_touch;  /* px or py at the moment of pressing */
    int  drag_start_pos;    /* col*CELL or row*CELL of the car at that moment */
    int  drag_fixed_px;     /* perpendicular coordinate, already with the half gap */
    int  drag_lo_px, drag_hi_px;
    int  drag_cur_px;
    int  drag_from;         /* the car's cell when pressed, for undo */

    /* --- menu --- */
    lv_obj_t  *menu_page;
    lv_obj_t  *lbl_progress;
    lv_obj_t  *lbl_continue;
    lv_obj_t **level_btn;
    int        level_btn_n;

    /* --- board --- */
    lv_obj_t *play_page;
    lv_obj_t *lbl_title;
    lv_obj_t *lbl_moves;
    lv_obj_t *board_area;
    lv_obj_t *lane;         /* the lane the target leaves through          */
    lv_obj_t *apron;        /* asphalt outside the gap, for the exit       */
    lv_obj_t *wall_r_top;   /* the right-hand wall, split in two by the    */
    lv_obj_t *wall_r_bot;   /* ... gap of the exit                         */
    lv_obj_t *hazard[6];    /* the yellow and black tape marking it        */
    lv_obj_t *touch;
    lv_obj_t *hint;
    lv_obj_t *btn_undo;
    lv_obj_t *car_view[AT_MAX_CARS];

    /* --- victory --- */
    lv_obj_t *win_dim;
    lv_obj_t *win_panel;
    lv_obj_t *lbl_win_title;
    lv_obj_t *lbl_win_info;
    lv_obj_t *btn_menu_w;
    lv_obj_t *btn_next;
    lv_obj_t *btn_go;       /* the menu's "keep playing" */

    /* --- gamepad --- */
    aos_pad_t      pad;
    aos_pad_menu_t pmenu;   /* the buttons of the menu or the victory panel */
    lv_timer_t    *pad_timer;
    lv_obj_t      *cursor;  /* the frame over the board */
    bool           cur_on;
    int            cur_row, cur_col;
    int            grab;    /* the car the pad holds, -1 if none */
    int            grab_from;
} at_app_t;

/* --------------------------------------------------------------------------
 * UI helpers: lv_obj by hand, no border, no scrolling, large radii
 * -------------------------------------------------------------------------- */

static lv_obj_t *panel_base(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

/* Ornament: a plain rectangle that does NOT receive touches. In LVGL 9 every
 * lv_obj is born clickable, so without taking the flag off, every mark on the
 * floor would eat the touch meant for the drag layer. */
static lv_obj_t *decor(lv_obj_t *parent, uint32_t color, int32_t radius)
{
    lv_obj_t *o = panel_base(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

/* A stretch of the kerb around the lot. The gradient gives it the volume of
 * concrete without costing a layer. */
static lv_obj_t *wall(lv_obj_t *parent)
{
    lv_obj_t *w = decor(parent, C_WALL, 4);
    lv_obj_set_style_bg_grad_color(w, lv_color_hex(C_WALL_LO), 0);
    lv_obj_set_style_bg_grad_dir(w, LV_GRAD_DIR_VER, 0);
    return w;
}

static lv_obj_t *text(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                      lv_color_t color)
{
    lv_obj_t *l = aos_label(parent, txt, font, color);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *symbol,
                           lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = panel_base(parent);
    lv_obj_set_size(b, BTN, BTN);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x48484A), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *lbl = text(b, symbol, &aos_sym_44, AOS_C_TEXT);
    lv_obj_center(lbl);
    return b;
}

static lv_obj_t *text_btn(lv_obj_t *parent, const char *txt, lv_color_t bg,
                          lv_color_t fg, int32_t w, lv_event_cb_t cb,
                          void *user_data)
{
    lv_obj_t *b = panel_base(parent);
    lv_obj_set_size(b, w, BTN);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *lbl = text(b, txt, aos_font_body, fg);
    lv_obj_center(lbl);
    return b;
}

/* --------------------------------------------------------------------------
 * Board: placing and (re)creating the cars
 * -------------------------------------------------------------------------- */

static void place_car_view(at_app_t *a, int i)
{
    const at_car_t *c = &a->board.cars[i];
    lv_obj_set_pos(a->car_view[i], c->col * CELL + GAP / 2, c->row * CELL + GAP / 2);
}

static void clear_car_views(at_app_t *a)
{
    for (int i = 0; i < AT_MAX_CARS; i++) {
        if (a->car_view[i]) {
            /* Before deleting the object its exit animation has to be removed
             * if it had one in flight: otherwise its finished callback would
             * run on a freed object. */
            lv_anim_delete(a->car_view[i], NULL);
            lv_obj_delete(a->car_view[i]);
            a->car_view[i] = NULL;
        }
    }
}

/* The exit is not always on the same row, so the gap in the wall, the lane and
 * the tape are repositioned when each level is loaded. */
static void layout_exit(at_app_t *a)
{
    const int bx    = a->board_x, by = a->board_y;
    const int row   = a->board.cars[a->board.target_idx].row;
    const int gap_y = by + row * CELL;          /* top edge of the gap */

    lv_obj_set_pos(a->lane, 0, row * CELL);
    lv_obj_set_size(a->lane, BOARD_PX, CELL);

    /* Asphalt on the other side of the gap: without this the car drives out
     * into the void. */
    lv_obj_set_pos(a->apron, bx + BOARD_PX, gap_y);
    lv_obj_set_size(a->apron, a->apron_end - (bx + BOARD_PX), CELL);

    /* The right-hand wall reaches the gap and carries on after it. */
    lv_obj_set_pos(a->wall_r_top, bx + BOARD_PX, by - WALL);
    lv_obj_set_size(a->wall_r_top, WALL, row * CELL + WALL);

    const int bot_y = gap_y + CELL;
    lv_obj_set_pos(a->wall_r_bot, bx + BOARD_PX, bot_y);
    lv_obj_set_size(a->wall_r_bot, WALL, (by + BOARD_PX + WALL) - bot_y);

    /* Three stretches of tape on either side of the gap, alternating yellow
     * and black: it is what makes it obvious where the car has to come out. */
    const int seg = 14;
    for (int k = 0; k < 3; k++) {
        lv_obj_t *up = a->hazard[k];
        lv_obj_set_style_bg_color(up, lv_color_hex((k % 2) ? 0x1C1C1E : C_HAZARD), 0);
        lv_obj_set_pos(up, bx + BOARD_PX, gap_y - (k + 1) * seg);
        lv_obj_set_size(up, WALL, seg);

        lv_obj_t *dn = a->hazard[3 + k];
        lv_obj_set_style_bg_color(dn, lv_color_hex((k % 2) ? 0x1C1C1E : C_HAZARD), 0);
        lv_obj_set_pos(dn, bx + BOARD_PX, gap_y + CELL + k * seg);
        lv_obj_set_size(dn, WALL, seg);
    }
}

/* Builds the views for whatever a->board holds right now. */
static void build_cars(at_app_t *a)
{
    clear_car_views(a);
    for (int i = 0; i < a->board.count; i++) {
        a->car_view[i] = at_car_view_create(a->board_area, &a->board.cars[i], CELL, GAP);
        place_car_view(a, i);
    }
    lv_obj_move_foreground(a->touch);
    lv_obj_move_foreground(a->cursor);
    layout_exit(a);
    a->dragging = false;
}

static void update_hud(at_app_t *a)
{
    char buf[64];
    snprintf(buf, sizeof(buf), _("Nivel %d"), a->level + 1);
    lv_label_set_text(a->lbl_title, buf);
    snprintf(buf, sizeof(buf),
             a->moves == 1 ? _("%d movimiento") : _("%d movimientos"), a->moves);
    lv_label_set_text(a->lbl_moves, buf);

    bool can = a->undo_n > 0 && a->state == ST_PLAY;
    lv_obj_set_style_bg_opa(a->btn_undo, can ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_text_opa(lv_obj_get_child(a->btn_undo, 0),
                              can ? LV_OPA_COVER : LV_OPA_50, 0);
}

static void load_level(at_app_t *a, int idx)
{
    int n = at_level_count();
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    a->level  = idx;
    a->moves  = 0;
    a->undo_n = 0;

    const at_level_t *lvl = at_level_get(idx);
    at_parse_level(lvl->rows, &a->board);
    a->grab    = -1;
    a->cur_row = a->board.cars[a->board.target_idx].row;
    a->cur_col = a->board.cars[a->board.target_idx].col;
    build_cars(a);
    lv_obj_remove_flag(a->hint, LV_OBJ_FLAG_HIDDEN);
}

/* --------------------------------------------------------------------------
 * The gamepad's frame on the board: one cell, or the whole car it holds
 * -------------------------------------------------------------------------- */

static void cursor_show(at_app_t *a)
{
    if (!a->cur_on || a->state != ST_PLAY) {
        lv_obj_add_flag(a->cursor, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int r = a->cur_row, c = a->cur_col, w = 1, h = 1;
    if (a->grab >= 0) {
        const at_car_t *car = &a->board.cars[a->grab];
        r = car->row;
        c = car->col;
        w = car->horizontal ? car->len : 1;
        h = car->horizontal ? 1 : car->len;
    }
    lv_obj_set_pos(a->cursor, c * CELL, r * CELL);
    lv_obj_set_size(a->cursor, w * CELL, h * CELL);
    lv_obj_set_style_border_color(a->cursor,
                                  lv_color_hex(a->grab >= 0 ? C_GRAB : C_CURSOR), 0);
    lv_obj_remove_flag(a->cursor, LV_OBJ_FLAG_HIDDEN);
}

/* --------------------------------------------------------------------------
 * Navigation between screens
 * -------------------------------------------------------------------------- */

static int next_to_play(const at_app_t *a)
{
    int n = at_level_count();
    return a->unlocked < n ? a->unlocked : n - 1;
}

static void refresh_menu(at_app_t *a)
{
    for (int i = 0; i < a->level_btn_n; i++) {
        lv_obj_t *btn = a->level_btn[i];
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);

        uint32_t line;      /* the painted line of the bay */
        if (i > a->unlocked) {
            line = 0x4A4A52;                /* locked      */
        } else if (i < a->unlocked) {
            line = 0x30D158;                /* completed   */
        } else {
            line = C_HAZARD;                /* the one to play */
        }
        lv_obj_set_style_border_color(btn, lv_color_hex(line), 0);
        lv_obj_set_style_border_width(btn, (i == a->unlocked) ? 5 : 3, 0);

        char buf[12];
        if (i > a->unlocked) {
            lv_obj_set_style_text_font(lbl, &aos_sym_44, 0);
            lv_label_set_text(lbl, AOS_SYM_LOCK);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x6A6A72), 0);
        } else {
            lv_obj_set_style_text_font(lbl, aos_font_title, 0);
            snprintf(buf, sizeof(buf), "%d", i + 1);
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_text_color(lbl, lv_color_hex(line), 0);
        }
    }

    int n = at_level_count();
    int done = a->unlocked < n ? a->unlocked : n;
    char buf[64];
    snprintf(buf, sizeof(buf), _("%d de %d completados"), done, n);
    lv_label_set_text(a->lbl_progress, buf);
    if (done >= n) {
        lv_label_set_text(a->lbl_continue, _("Jugar de nuevo"));
    } else {
        snprintf(buf, sizeof(buf), _("Jugar nivel %d"), next_to_play(a) + 1);
        lv_label_set_text(a->lbl_continue, buf);
    }
}

static void show_menu(at_app_t *a)
{
    a->dragging = false;
    a->state    = ST_MENU;
    lv_obj_add_flag(a->win_dim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a->win_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a->play_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(a->menu_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(a->menu_page);
    refresh_menu(a);

    /* the pad: the 25 bays and the button, starting on the button */
    lv_obj_t *items[AOS_PAD_MENU_MAX];
    int n = 0;
    for (int i = 0; i < a->level_btn_n && n < AOS_PAD_MENU_MAX - 1; i++) {
        items[n++] = a->level_btn[i];
    }
    items[n++] = a->btn_go;
    aos_pad_menu_set(&a->pmenu, items, n, n - 1);
    cursor_show(a);
}

static void show_play(at_app_t *a)
{
    a->state = ST_PLAY;
    lv_obj_add_flag(a->menu_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a->win_dim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a->win_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(a->play_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(a->play_page);
    update_hud(a);
    aos_pad_menu_clear(&a->pmenu);
    cursor_show(a);
}

static void start_level(at_app_t *a, int idx)
{
    load_level(a, idx);
    show_play(a);
}

static void show_win(at_app_t *a)
{
    if (a->level + 1 > a->unlocked) {
        a->unlocked = a->level + 1;
        if (a->unlocked > at_level_count()) {
            a->unlocked = at_level_count();
        }
        aos_hal_pref_set_i32(KEY_UNLOCKED, (int32_t)a->unlocked);
    }

    bool last = (a->level + 1 >= at_level_count());
    lv_label_set_text(a->lbl_win_title, last ? _("Estacionamiento despejado")
                                             : _("Auto afuera"));
    char buf[64];
    /* Two whole phrases and not "movimiento%s" with the "s" glued on
     * separately: that concatenation is SPANISH's plural rule put into the
     * code. This way the translator receives both complete forms. */
    snprintf(buf, sizeof(buf),
             a->moves == 1 ? _("Nivel %d completado en %d movimiento")
                           : _("Nivel %d completado en %d movimientos"),
             a->level + 1, a->moves);
    lv_label_set_text(a->lbl_win_info, buf);
    /* the last level has no "next": the menu button takes the whole row */
    /* WIN_W and not lv_obj_get_width(): right after a create the panel has
     * not been laid out yet and measures 0. */
    const int32_t pw = WIN_W;
    if (last) {
        lv_obj_add_flag(a->btn_next, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(a->btn_menu_w, pw - 64);
    } else {
        lv_obj_remove_flag(a->btn_next, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(a->btn_menu_w, (pw - 64 - 16) / 2);
    }

    a->state = ST_WIN;
    update_hud(a);
    lv_obj_remove_flag(a->win_dim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(a->win_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(a->win_dim);
    lv_obj_move_foreground(a->win_panel);

    lv_obj_t *items[] = { a->btn_menu_w, a->btn_next };
    aos_pad_menu_set(&a->pmenu, items, 2, last ? 0 : 1);
    cursor_show(a);
}

/* --------------------------------------------------------------------------
 * Drag: a transparent layer over the board. It follows the finger ONLY along
 * the axis of the car touched; the valid range is computed once, on
 * pressing, because the other cars do not move during the drag.
 *
 * Split out of touch_event() into three pure functions (they do not touch LVGL
 * except to move the car) so they can be tested from ATASCO_SELFTEST without
 * needing a real indev: see the development switch at the end of the file.
 * -------------------------------------------------------------------------- */

static void drag_press(at_app_t *a, int idx, int touch_axis)
{
    const at_car_t *c = &a->board.cars[idx];
    int lo, hi;
    at_slide_range(&a->board, idx, &lo, &hi);

    a->drag_idx         = idx;
    a->drag_lo_px       = lo * CELL;
    a->drag_hi_px       = hi * CELL;
    a->drag_from        = c->horizontal ? c->col : c->row;
    a->drag_start_pos   = a->drag_from * CELL;
    a->drag_cur_px      = a->drag_start_pos;
    a->drag_start_touch = touch_axis;
    a->drag_fixed_px    = (c->horizontal ? c->row : c->col) * CELL + GAP / 2;
    a->dragging         = true;
}

static void drag_move(at_app_t *a, int touch_axis)
{
    if (!a->dragging) {
        return;
    }
    const at_car_t *c = &a->board.cars[a->drag_idx];
    int delta = touch_axis - a->drag_start_touch;
    int pos   = a->drag_start_pos + delta;
    if (pos < a->drag_lo_px) pos = a->drag_lo_px;
    if (pos > a->drag_hi_px) pos = a->drag_hi_px;
    a->drag_cur_px = pos;

    int ox = c->horizontal ? pos + GAP / 2 : a->drag_fixed_px;
    int oy = c->horizontal ? a->drag_fixed_px : pos + GAP / 2;
    lv_obj_set_pos(a->car_view[a->drag_idx], ox, oy);
}

static void anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

static void drive_out_done(lv_anim_t *anim)
{
    at_app_t *a = lv_anim_get_user_data(anim);
    /* If the player went to the menu while the car was leaving, there is
     * nothing to celebrate: the victory panel would appear over the menu. */
    if (a->state == ST_WIN) {
        show_win(a);
        aos_hal_beep(1500, 40);
        aos_hal_beep(2000, 70);
    }
}

/* The car drives out through the gap and only then is the level declared won.
 *
 * The object's x position is animated, which LVGL resolves by moving the blit;
 * no transformations, which would be a layer. The board has
 * LV_OBJ_FLAG_OVERFLOW_VISIBLE precisely so the car goes on being drawn once
 * it has passed the edge. */
static void drive_out(at_app_t *a)
{
    lv_obj_t *car = a->car_view[a->board.target_idx];

    /* No more playing: while it is leaving, touches must not move anything. */
    a->state = ST_WIN;
    lv_obj_add_flag(a->hint, LV_OBJ_FLAG_HIDDEN);
    cursor_show(a);

    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, car);
    lv_anim_set_exec_cb(&an, anim_x_cb);
    lv_anim_set_values(&an, lv_obj_get_x(car), a->apron_end - a->board_x + 12);
    lv_anim_set_duration(&an, 480);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&an, drive_out_done);
    lv_anim_set_user_data(&an, a);
    lv_anim_start(&an);

    aos_hal_beep(900, 25);
}

/* Returns true if the car ended up in a different cell from the one it had. */
static bool drag_release(at_app_t *a)
{
    if (!a->dragging) {
        return false;
    }
    a->dragging = false;

    int  snapped = (a->drag_cur_px + CELL / 2) / CELL;
    bool moved   = at_apply_move(&a->board, a->drag_idx, snapped);
    place_car_view(a, a->drag_idx);

    if (moved) {
        a->moves++;
        if (a->undo_n == UNDO_MAX) {
            memmove(a->undo, a->undo + 1, sizeof(a->undo[0]) * (UNDO_MAX - 1));
            a->undo_n--;
        }
        a->undo[a->undo_n++] = (at_undo_t){ (int8_t)a->drag_idx, (int8_t)a->drag_from };
        update_hud(a);
        lv_obj_add_flag(a->hint, LV_OBJ_FLAG_HIDDEN);
        aos_hal_beep(1200, 18);
    }

    /* Arriving flush against the exit's wall already counts as having got out:
     * to get that far the way must have been clear. See at_target_at_exit(),
     * which explains why at_is_solved() is not enough. */
    if (a->drag_idx == a->board.target_idx && at_target_at_exit(&a->board)) {
        drive_out(a);
    }
    return moved;
}

/* The car the pad held is let go: the whole slide is one move, as a finger's
 * drag is. Only the bookkeeping, no UI (destroy uses it too). */
static bool grab_commit(at_app_t *a)
{
    int idx = a->grab;
    if (idx < 0) {
        return false;
    }
    a->grab = -1;
    const at_car_t *c = &a->board.cars[idx];
    int pos = c->horizontal ? c->col : c->row;
    a->cur_row = c->row;
    a->cur_col = c->col;
    if (pos == a->grab_from) {
        return false;
    }
    a->moves++;
    if (a->undo_n == UNDO_MAX) {
        memmove(a->undo, a->undo + 1, sizeof(a->undo[0]) * (UNDO_MAX - 1));
        a->undo_n--;
    }
    a->undo[a->undo_n++] = (at_undo_t){ (int8_t)idx, (int8_t)a->grab_from };
    return true;
}

/* ... and with the UI: getting to the exit sends the car out the same way a
 * finger does. */
static void pad_release(at_app_t *a)
{
    int idx = a->grab;
    if (grab_commit(a)) {
        update_hud(a);
        lv_obj_add_flag(a->hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (idx == a->board.target_idx && at_target_at_exit(&a->board)) {
        drive_out(a);
    }
    cursor_show(a);
}

static void touch_event(lv_event_t *event)
{
    at_app_t      *a    = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (a->state != ST_PLAY) {
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
        a->dragging = false;
        /* a finger again: the pad lets go of its car and its frame hides */
        if (a->grab >= 0 || a->cur_on) {
            a->cur_on = false;
            pad_release(a);
        }
        if (px < 0 || py < 0 || px >= BOARD_PX || py >= BOARD_PX) {
            return;
        }
        int idx = at_car_at(&a->board, py / CELL, px / CELL);
        if (idx < 0) {
            return;
        }
        const at_car_t *c = &a->board.cars[idx];
        drag_press(a, idx, c->horizontal ? px : py);
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        drag_release(a);
        return;
    }

    /* LV_EVENT_PRESSING */
    if (!a->dragging) {
        return;
    }
    const at_car_t *c = &a->board.cars[a->drag_idx];
    drag_move(a, c->horizontal ? px : py);
}

/* --------------------------------------------------------------------------
 * Button callbacks
 * -------------------------------------------------------------------------- */

static void cb_restart_level(at_app_t *a)
{
    if (a->state != ST_PLAY) {
        return;
    }
    load_level(a, a->level);
    update_hud(a);
    cursor_show(a);
}

static void cb_restart(lv_event_t *e)
{
    cb_restart_level((at_app_t *)lv_event_get_user_data(e));
}

/* A move backwards is always legal: the car came from there along a clear
 * path, and nothing else has moved since that move. */
static void undo_move(at_app_t *a)
{
    if (a->state != ST_PLAY || a->dragging) {
        return;
    }
    pad_release(a);
    if (a->state != ST_PLAY || a->undo_n == 0) {
        return;
    }
    at_undo_t u = a->undo[--a->undo_n];
    at_car_t *c = &a->board.cars[u.idx];
    if (c->horizontal) {
        c->col = u.from;
    } else {
        c->row = u.from;
    }
    place_car_view(a, u.idx);
    a->cur_row = c->row;
    a->cur_col = c->col;
    cursor_show(a);
    if (a->moves > 0) {
        a->moves--;
    }
    update_hud(a);
    aos_hal_beep(700, 18);
}

static void cb_undo(lv_event_t *e)
{
    undo_move((at_app_t *)lv_event_get_user_data(e));
}

static void cb_open_menu(lv_event_t *e)
{
    show_menu((at_app_t *)lv_event_get_user_data(e));
}

static void cb_win_next(lv_event_t *e)
{
    at_app_t *a = lv_event_get_user_data(e);
    start_level(a, a->level + 1);
}

static void cb_continue(lv_event_t *e)
{
    at_app_t *a = lv_event_get_user_data(e);
    start_level(a, a->unlocked >= at_level_count() ? 0 : next_to_play(a));
}

static void cb_level_tile(lv_event_t *e)
{
    at_app_t *a   = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx > a->unlocked) {
        aos_hal_beep(300, 35);
        return;
    }
    start_level(a, idx);
}

/* --------------------------------------------------------------------------
 * Building the screens
 * -------------------------------------------------------------------------- */

static void build_menu_page(at_app_t *a, lv_obj_t *root)
{
    const int32_t W = a->W, H = a->H;
    a->menu_page = panel_base(root);
    lv_obj_set_size(a->menu_page, W, H);
    lv_obj_set_pos(a->menu_page, 0, 0);

    /* Portrait: sign, grid, progress and the button, top to bottom.
     * Landscape: sign, progress and button on the left, the grid right. */
    const int32_t tile = a->land ? 104 : 120;
    const int32_t tgap = a->land ? 16 : 22;
    const int32_t grid = 5 * tile + 4 * tgap;
    const int32_t col_w = a->land ? 440 : W - 2 * MARGIN;

    /* The header is a strip of asphalt with the yellow line painted along the
     * bottom: the menu belongs to the same world as the board. */
    lv_obj_t *sign = decor(a->menu_page, C_ASPHALT, AOS_UI_RADIUS);
    lv_obj_set_size(sign, col_w, 160);
    lv_obj_set_pos(sign, MARGIN, MARGIN);

    lv_obj_t *title = text(sign, _("Atasco"), aos_font_large, AOS_C_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t *sub = text(sign, _("Sacá el auto rojo"), aos_font_small, AOS_C_DIM);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 86);

    const int dash = 36, step = 72;
    int ndash = (col_w - 32 + (step - dash)) / step;
    int x0 = (col_w - (ndash * step - (step - dash))) / 2;
    for (int k = 0; k < ndash; k++) {
        lv_obj_t *d = decor(sign, C_HAZARD, 3);
        lv_obj_set_size(d, dash, 6);
        lv_obj_set_pos(d, x0 + k * step, 136);
    }

    int32_t gx, gy;
    if (a->land) {
        int32_t right_x = MARGIN + col_w;
        gx = right_x + (W - right_x - grid) / 2;
        gy = (H - grid) / 2;
    } else {
        gx = (W - grid) / 2;
        gy = MARGIN + 160 + 36;
    }

    int n = at_level_count();
    a->level_btn   = lv_malloc_zeroed(sizeof(lv_obj_t *) * (size_t)n);
    a->level_btn_n = n;
    for (int i = 0; i < n; i++) {
        /* Each level is a bay: asphalt with the line painted around it. The
         * colour of that line says whether it is played, available or still
         * locked. */
        lv_obj_t *btn = panel_base(a->menu_page);
        lv_obj_set_size(btn, tile, tile);
        lv_obj_set_pos(btn, gx + (i % 5) * (tile + tgap), gy + (i / 5) * (tile + tgap));
        lv_obj_set_style_radius(btn, 22, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_ASPHALT), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x55555A), LV_STATE_PRESSED);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, cb_level_tile, LV_EVENT_CLICKED, a);

        lv_obj_t *lbl = text(btn, "", aos_font_title, AOS_C_TEXT);
        lv_obj_center(lbl);

        a->level_btn[i] = btn;
    }

    a->lbl_progress = aos_label_boxed(a->menu_page, "", aos_font_small, AOS_C_DIM,
                                      col_w, 36);
    lv_obj_remove_flag(a->lbl_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(a->lbl_progress, MARGIN,
                   a->land ? MARGIN + 160 + 24 : gy + grid + 32);

    lv_obj_t *go = text_btn(a->menu_page, "", AOS_C_ACCENT, AOS_C_TEXT,
                            a->land ? col_w : 440, cb_continue, a);
    lv_obj_set_height(go, 96);
    a->btn_go = go;
    lv_obj_set_style_radius(go, 30, 0);
    if (a->land) {
        lv_obj_set_pos(go, MARGIN, H - MARGIN - 96);
    } else {
        lv_obj_set_pos(go, (W - 440) / 2, H - 40 - 96);
    }
    a->lbl_continue = lv_obj_get_child(go, 0);
}

static void build_play_page(at_app_t *a, lv_obj_t *root)
{
    const int32_t W = a->W, H = a->H;
    a->play_page = panel_base(root);
    lv_obj_set_size(a->play_page, W, H);
    lv_obj_set_pos(a->play_page, 0, 0);
    lv_obj_add_flag(a->play_page, LV_OBJ_FLAG_HIDDEN);

    /* Where the board goes, and how far the asphalt beyond the exit runs.
     * Landscape: the board on the left and the side card after the apron.
     * Portrait: nudged left so the exit has room for its asphalt. */
    lv_obj_t *bar;              /* where the title and the two buttons live */
    int32_t   bar_w;
    const int32_t side_x = 60 + BOARD_PX + WALL + 72;
    if (a->land) {
        a->board_x   = 60;
        a->board_y   = (H - BOARD_PX) / 2;
        a->apron_end = side_x - MARGIN;
        bar_w = W - side_x - MARGIN;
        bar = decor(a->play_page, 0x1C1C1E, AOS_UI_RADIUS);
        lv_obj_set_pos(bar, side_x, MARGIN);
        lv_obj_set_size(bar, bar_w, H - 2 * MARGIN);
    } else {
        a->board_x   = (W - BOARD_PX) / 2 - 20;
        a->apron_end = W;
        const int32_t top = 12 + BTN, bottom = H - 40 - BTN;
        a->board_y   = (top + bottom) / 2 - BOARD_PX / 2 - 24;
        bar_w = W;
        bar = panel_base(a->play_page);
        lv_obj_set_pos(bar, 0, 0);
        lv_obj_set_size(bar, W, 12 + BTN + 12);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }
    const int32_t inset = a->land ? 24 : MARGIN;

    lv_obj_t *btn_menu = round_btn(bar, AOS_SYM_VIEW_GRID_OUTLINE, cb_open_menu, a);
    lv_obj_set_pos(btn_menu, inset, a->land ? inset : 12);

    lv_obj_t *btn_restart = round_btn(bar, AOS_SYM_RESTART, cb_restart, a);
    lv_obj_set_pos(btn_restart, bar_w - inset - BTN, a->land ? inset : 12);

    const int32_t title_w = bar_w - 2 * (inset + BTN + 8);
    a->lbl_title = aos_label_boxed(bar, "", aos_font_title, AOS_C_TEXT, title_w, 44);
    lv_obj_remove_flag(a->lbl_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(a->lbl_title, inset + BTN + 8, (a->land ? inset : 12) + 2);
    a->lbl_moves = aos_label_boxed(bar, "", aos_font_small, AOS_C_DIM, title_w, 32);
    lv_obj_remove_flag(a->lbl_moves, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(a->lbl_moves, inset + BTN + 8, (a->land ? inset : 12) + 50);

    /* The asphalt outside the gap. It goes BEFORE the board so it ends up
     * underneath: the target, on leaving, has to pass over it. */
    a->apron = decor(a->play_page, C_ASPHALT, 0);

    const int bx = a->board_x, by = a->board_y;
    a->board_area = panel_base(a->play_page);
    lv_obj_set_size(a->board_area, BOARD_PX, BOARD_PX);
    lv_obj_set_pos(a->board_area, bx, by);
    lv_obj_set_style_bg_color(a->board_area, lv_color_hex(C_ASPHALT), 0);
    lv_obj_set_style_bg_opa(a->board_area, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(a->board_area, 8, 0);
    /* Without this the departing car is clipped right at the board's edge. */
    lv_obj_add_flag(a->board_area, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    /* The target's lane: a strip just a shade lighter. */
    a->lane = decor(a->board_area, C_LANE, 0);

    /* The bays' crosses, at the grid's interior intersections, created ONCE. */
    for (int r = 1; r < AT_GRID; r++) {
        for (int c = 1; c < AT_GRID; c++) {
            lv_obj_t *hbar = decor(a->board_area, C_MARK, 1);
            lv_obj_set_size(hbar, 25, 3);
            lv_obj_set_pos(hbar, c * CELL - 12, r * CELL - 1);

            lv_obj_t *vbar = decor(a->board_area, C_MARK, 1);
            lv_obj_set_size(vbar, 3, 25);
            lv_obj_set_pos(vbar, c * CELL - 1, r * CELL - 12);
        }
    }

    /* The kerb around the lot: four stretches, with the right-hand one split
     * in two to leave the gap of the exit (layout_exit sizes it). */
    lv_obj_t *wall_top = wall(a->play_page);
    lv_obj_set_pos(wall_top, bx - WALL, by - WALL);
    lv_obj_set_size(wall_top, BOARD_PX + 2 * WALL, WALL);

    lv_obj_t *wall_bot = wall(a->play_page);
    lv_obj_set_pos(wall_bot, bx - WALL, by + BOARD_PX);
    lv_obj_set_size(wall_bot, BOARD_PX + 2 * WALL, WALL);

    lv_obj_t *wall_left = wall(a->play_page);
    lv_obj_set_pos(wall_left, bx - WALL, by - WALL);
    lv_obj_set_size(wall_left, WALL, BOARD_PX + 2 * WALL);

    a->wall_r_top = wall(a->play_page);
    a->wall_r_bot = wall(a->play_page);

    for (int k = 0; k < 6; k++) {
        a->hazard[k] = decor(a->play_page, C_HAZARD, 0);
    }

    a->touch = panel_base(a->board_area);
    lv_obj_set_size(a->touch, BOARD_PX, BOARD_PX);
    lv_obj_set_pos(a->touch, 0, 0);
    lv_obj_set_style_bg_opa(a->touch, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(a->touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->touch, touch_event, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->touch, touch_event, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(a->touch, touch_event, LV_EVENT_RELEASED, a);
    lv_obj_add_event_cb(a->touch, touch_event, LV_EVENT_PRESS_LOST, a);

    /* the gamepad's frame, over everything on the board and deaf to touch */
    a->cursor = panel_base(a->board_area);
    lv_obj_remove_flag(a->cursor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(a->cursor, 6, 0);
    lv_obj_set_style_border_opa(a->cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(a->cursor, 14, 0);
    lv_obj_add_flag(a->cursor, LV_OBJ_FLAG_HIDDEN);

    /* The hint and undo: under the board in portrait, in the card in
     * landscape. The side card is drawn after the board, so it also covers
     * the car as it drives off. */
    const int32_t hint_w = a->land ? bar_w - 48 : BOARD_PX;
    a->hint = text(a->land ? bar : a->play_page,
                   _("Deslizá los autos para sacar al rojo"), aos_font_small, AOS_C_DIM);
    lv_obj_set_style_text_align(a->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(a->hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(a->hint, hint_w);

    const int32_t undo_w = a->land ? bar_w - 48 : 320;
    a->btn_undo = text_btn(a->land ? bar : a->play_page, _("Deshacer"), AOS_C_CARD2,
                           AOS_C_TEXT, undo_w, cb_undo, a);
    if (a->land) {
        lv_obj_align(a->hint, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_pos(a->btn_undo, 24, H - 2 * MARGIN - 24 - BTN);
        lv_obj_move_foreground(bar);
    } else {
        lv_obj_set_pos(a->hint, bx, by + BOARD_PX + WALL + 28);
        lv_obj_set_pos(a->btn_undo, (W - undo_w) / 2, H - 40 - BTN);
    }
}

static void build_win_overlay(at_app_t *a, lv_obj_t *root)
{
    a->win_dim = panel_base(root);
    lv_obj_set_size(a->win_dim, a->W, a->H);
    lv_obj_set_pos(a->win_dim, 0, 0);
    lv_obj_set_style_bg_color(a->win_dim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(a->win_dim, LV_OPA_60, 0);
    lv_obj_add_flag(a->win_dim, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);

    const int32_t pw = WIN_W, ph = WIN_H;
    a->win_panel = panel_base(root);
    lv_obj_set_size(a->win_panel, pw, ph);
    lv_obj_align(a->win_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(a->win_panel, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(a->win_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(a->win_panel, AOS_UI_RADIUS + 8, 0);
    lv_obj_add_flag(a->win_panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);

    a->lbl_win_title = aos_label_boxed(a->win_panel, "", aos_font_title, AOS_C_GREEN,
                                       pw - 48, 48);
    lv_obj_set_pos(a->lbl_win_title, 24, 40);

    a->lbl_win_info = text(a->win_panel, "", aos_font_body, AOS_C_DIM);
    lv_obj_set_style_text_align(a->lbl_win_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(a->lbl_win_info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(a->lbl_win_info, pw - 64);
    lv_obj_set_pos(a->lbl_win_info, 32, 108);

    const int32_t bw = (pw - 64 - 16) / 2;
    a->btn_menu_w = text_btn(a->win_panel, _("Menú"), AOS_C_CARD2, AOS_C_TEXT, bw,
                             cb_open_menu, a);
    lv_obj_set_pos(a->btn_menu_w, 32, ph - 32 - BTN);
    a->btn_next = text_btn(a->win_panel, _("Siguiente"), AOS_C_GREEN,
                           lv_color_hex(0x000000), bw, cb_win_next, a);
    lv_obj_set_pos(a->btn_next, 32 + bw + 16, ph - 32 - BTN);
}

/* --------------------------------------------------------------------------
 * The gamepad, every 30 ms (the app tick comes at 5 Hz, too slow for it)
 * -------------------------------------------------------------------------- */

static void pad_play(at_app_t *a)
{
    const aos_pad_t *p = &a->pad;
    if (aos_pad_pressed(p, AOS_PAD_START)) {
        pad_release(a);
        if (a->state == ST_PLAY) {
            show_menu(a);
        }
        return;
    }
    if (aos_pad_pressed(p, AOS_PAD_R)) {
        a->grab = -1;           /* the board is about to be loaded again */
        cb_restart_level(a);
        return;
    }
    if (aos_pad_pressed(p, AOS_PAD_L)) {
        undo_move(a);
        return;
    }

    uint32_t dirs = p->repeat & AOS_PAD_DIRS;
    bool btn_a = aos_pad_pressed(p, AOS_PAD_A);
    bool btn_b = aos_pad_pressed(p, AOS_PAD_B);
    if (!dirs && !btn_a && !btn_b) {
        return;
    }
    if (!a->cur_on) {
        /* the first press only shows where the frame is */
        a->cur_on = true;
        cursor_show(a);
        return;
    }
    if (a->dragging) {
        return;                 /* a finger is on a car */
    }

    int dr = (dirs & AOS_PAD_DOWN) ? 1 : (dirs & AOS_PAD_UP) ? -1 : 0;
    int dc = (dirs & AOS_PAD_RIGHT) ? 1 : (dirs & AOS_PAD_LEFT) ? -1 : 0;

    if (a->grab >= 0) {
        if (btn_a || btn_b) {
            pad_release(a);
            aos_hal_beep(1200, 18);
            return;
        }
        /* the car only moves along its own axis */
        at_car_t *c = &a->board.cars[a->grab];
        int step = c->horizontal ? dc : dr;
        if (!step) {
            return;
        }
        int pos = (c->horizontal ? c->col : c->row) + step;
        if (at_apply_move(&a->board, a->grab, pos)) {
            place_car_view(a, a->grab);
            cursor_show(a);
            /* flush against the exit: it is out, as with a finger */
            if (a->grab == a->board.target_idx && at_target_at_exit(&a->board)) {
                pad_release(a);
            }
        } else {
            aos_hal_beep(300, 25);
        }
        return;
    }

    if (btn_b) {
        undo_move(a);
        return;
    }
    if (btn_a) {
        int idx = at_car_at(&a->board, a->cur_row, a->cur_col);
        if (idx >= 0) {
            const at_car_t *c = &a->board.cars[idx];
            a->grab      = idx;
            a->grab_from = c->horizontal ? c->col : c->row;
            cursor_show(a);
            aos_hal_beep(900, 18);
        }
        return;
    }
    int r = a->cur_row + dr, c = a->cur_col + dc;
    if (r >= 0 && r < AT_GRID && c >= 0 && c < AT_GRID) {
        a->cur_row = r;
        a->cur_col = c;
        cursor_show(a);
    }
}

static void pad_cb(lv_timer_t *timer)
{
    at_app_t *a = lv_timer_get_user_data(timer);
    aos_pad_update(&a->pad, lv_tick_get());
    if (!a->pad.pressed && !a->pad.repeat) {
        return;
    }
    switch (a->state) {
    case ST_MENU:
        if (aos_pad_pressed(&a->pad, AOS_PAD_START)) {
            lv_obj_send_event(a->btn_go, LV_EVENT_CLICKED, NULL);
        } else {
            aos_pad_menu_step(&a->pmenu, &a->pad);
        }
        break;
    case ST_WIN:
        /* B is the panel's "Menú"; START its "Siguiente", or the menu */
        if (aos_pad_pressed(&a->pad, AOS_PAD_B)) {
            if (!lv_obj_has_flag(a->win_panel, LV_OBJ_FLAG_HIDDEN)) {
                show_menu(a);
            }
        } else if (aos_pad_pressed(&a->pad, AOS_PAD_START)) {
            if (!lv_obj_has_flag(a->win_panel, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_send_event(lv_obj_has_flag(a->btn_next, LV_OBJ_FLAG_HIDDEN)
                                  ? a->btn_menu_w : a->btn_next, LV_EVENT_CLICKED, NULL);
            }
        } else {
            aos_pad_menu_step(&a->pmenu, &a->pad);
        }
        break;
    default:
        pad_play(a);
        break;
    }
}

/* --------------------------------------------------------------------------
 * The app's life cycle
 * -------------------------------------------------------------------------- */

static bool at_back(aos_app_t *self, void *inst)
{
    (void)self;
    at_app_t *a = inst;
    if (a->state == ST_PLAY || a->state == ST_WIN) {
        show_menu(a);
        return true;
    }
    return false;   /* in the menu: let the system close the app */
}

static void at_tick(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    /* Drain the touch chip's gesture even though we do not use it: if nobody
     * reads it, it stays pending and reappears as a phantom gesture on leaving
     * the app (see docs/HANDOFF-APPS.md, the traps section). */
    aos_ui_take_gesture();
}

/* --------------------------------------------------------------------------
 * Development switches (see docs/HANDOFF-APPS.md): on the board getenv()
 * always returns NULL and they do no harm.
 *
 *   ATASCO_LEVEL=N     starts straight on level N (1-based), unlocked
 *   ATASCO_AUTOWIN=1   forces the target out as soon as it loads (to see the
 *                      victory panel without solving anything)
 *   ATASCO_SELFTEST=1  runs a few drag calculations against known values and
 *                      prints them, without depending on an indev
 * -------------------------------------------------------------------------- */

static void run_selftest(at_app_t *a)
{
    bool ok = true;
    int  lo, hi, col;

    load_level(a, 0);   /* level 1: an empty board except for the target */
    at_slide_range(&a->board, a->board.target_idx, &lo, &hi);
    printf("[ATASCO_SELFTEST] nivel1 lo=%d hi=%d (esperado 0,%d)\n", lo, hi, AT_GRID);
    ok = ok && lo == 0 && hi == AT_GRID;

    /* A drag that stops short of the far edge: 4.2 cells. Flush against the
     * exit's wall counts as out (at_target_at_exit()). */
    a->state = ST_PLAY;
    drag_press(a, a->board.target_idx, CELL / 2);
    drag_move(a, CELL / 2 + CELL * 42 / 10);
    bool moved = drag_release(a);
    col = a->board.cars[a->board.target_idx].col;
    printf("[ATASCO_SELFTEST] nivel1 arrastre de 4,2 celdas: col=%d moved=%d "
           "en_salida=%d estado=%d (esperado 4,1,1,%d)\n",
           col, moved, at_target_at_exit(&a->board), a->state, ST_WIN);
    ok = ok && moved && col == 4 && at_target_at_exit(&a->board) && a->state == ST_WIN;

    load_level(a, 1);   /* level 2: a vertical blocker stops the target */
    a->state = ST_PLAY;
    at_slide_range(&a->board, a->board.target_idx, &lo, &hi);
    printf("[ATASCO_SELFTEST] nivel2 lo=%d hi=%d (esperado 0,2)\n", lo, hi);
    ok = ok && lo == 0 && hi == 2;

    drag_press(a, a->board.target_idx, 0);
    drag_move(a, 10000);
    moved = drag_release(a);
    col = a->board.cars[a->board.target_idx].col;
    printf("[ATASCO_SELFTEST] nivel2 tras chocar contra el bloqueador: col=%d "
           "solved=%d (esperado 2,0)\n", col, at_is_solved(&a->board));
    ok = ok && moved && col == 2 && !at_is_solved(&a->board);

    /* and undo takes it back where it started */
    undo_move(a);
    col = a->board.cars[a->board.target_idx].col;
    printf("[ATASCO_SELFTEST] nivel2 deshacer: col=%d moves=%d (esperado 0,0)\n",
           col, a->moves);
    ok = ok && col == 0 && a->moves == 0;

    printf("[ATASCO_SELFTEST] %s\n", ok ? "ALL OK" : "FAILED");
}

static void *at_create(aos_app_t *self, lv_obj_t *root)
{
    at_app_t *a = lv_malloc_zeroed(sizeof(at_app_t));
    if (!a) {
        return NULL;
    }
    a->self = self;
    a->grab = -1;
    a->W    = lv_obj_get_width(root);
    a->H    = lv_obj_get_height(root);
    a->land = a->W > a->H;

    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);

    build_menu_page(a, root);
    build_play_page(a, root);
    build_win_overlay(a, root);

    int32_t unlocked = 0;
    aos_hal_pref_get_i32(KEY_UNLOCKED, &unlocked);
    if (unlocked < 0) unlocked = 0;
    /* 'unlocked' == at_level_count() is a valid state: it means they have all
     * been completed, not that one is left to "unlock". */
    if (unlocked > at_level_count()) unlocked = at_level_count();
    a->unlocked = (int)unlocked;

    const char *selftest = getenv("ATASCO_SELFTEST");
    if (selftest && selftest[0]) {
        run_selftest(a);
    }

    const char *lvl_env = getenv("ATASCO_LEVEL");
    if (s_keep.valid) {
        /* Built again (the screen turned): carry on exactly where it was. */
        a->level  = s_keep.level;
        a->moves  = s_keep.moves;
        a->board  = s_keep.board;
        a->undo_n = s_keep.undo_n;
        memcpy(a->undo, s_keep.undo, sizeof(a->undo));
        a->cur_row = a->board.cars[a->board.target_idx].row;
        a->cur_col = a->board.cars[a->board.target_idx].col;
        if (s_keep.state == ST_MENU) {
            show_menu(a);
        } else {
            build_cars(a);
            if (a->moves > 0) {
                lv_obj_add_flag(a->hint, LV_OBJ_FLAG_HIDDEN);
            }
            show_play(a);
            if (s_keep.state == ST_WIN) {
                /* the car was leaving or had left: it is out */
                lv_obj_set_x(a->car_view[a->board.target_idx],
                             a->apron_end - a->board_x + 12);
                lv_obj_add_flag(a->hint, LV_OBJ_FLAG_HIDDEN);
                show_win(a);
            }
        }
    } else if (lvl_env && lvl_env[0]) {
        int want = atoi(lvl_env) - 1;
        if (want < 0) want = 0;
        if (want > at_level_count() - 1) want = at_level_count() - 1;
        if (want > a->unlocked) {
            a->unlocked = want;
        }
        start_level(a, want);
        const char *autowin = getenv("ATASCO_AUTOWIN");
        if (autowin && autowin[0]) {
            /* Out by the same path as the player -up to the wall and from
             * there the animation- so the switch tests what is really used. */
            int len = a->board.cars[a->board.target_idx].len;
            at_apply_move(&a->board, a->board.target_idx, AT_GRID - len);
            place_car_view(a, a->board.target_idx);
            if (at_target_at_exit(&a->board)) {
                drive_out(a);
            } else {
                show_win(a);    /* the way was blocked from the start */
            }
        }
    } else {
        show_menu(a);
    }
    aos_pad_reset(&a->pad, lv_tick_get());
    a->pad_timer = lv_timer_create(pad_cb, 30, a);
    return a;
}

static void at_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    at_app_t *a = inst;

    if (a->pad_timer) {
        lv_timer_delete(a->pad_timer);
        a->pad_timer = NULL;
    }
    /* a car the pad was holding: its slide counts, as a finger's would */
    grab_commit(a);

    /* What the next create picks up. */
    s_keep.valid  = true;
    s_keep.state  = a->state;
    s_keep.level  = a->level;
    s_keep.moves  = a->moves;
    s_keep.board  = a->board;
    s_keep.undo_n = a->undo_n;
    memcpy(s_keep.undo, a->undo, sizeof(s_keep.undo));
    if (a->dragging) {
        /* a car half dragged goes back to its cell: the board already has it
         * there, only its view had moved */
        a->dragging = false;
    }

    /* If the app closes while the car was leaving, the animation has to die
     * HERE: its finished callback uses the context we are about to free. */
    for (int i = 0; i < AT_MAX_CARS; i++) {
        if (a->car_view[i]) {
            lv_anim_delete(a->car_view[i], NULL);
        }
    }
    if (a->level_btn) {
        lv_free(a->level_btn);
    }
    lv_free(a);
}

static bool at_init(aos_app_t *app)
{
    app->desc.id       = "demo.atasco";
    app->desc.name     = "Atasco";
    app->desc.icon     = "A";
    app->desc.icon_vec = AOS_ICON_GAMEPAD;
    app->desc.color_a  = 0xFF453A;
    app->desc.color_b  = 0x48484A;
    app->desc.order    = 148;
    app->desc.flags    = AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;

    app->create  = at_create;
    app->destroy = at_destroy;
    app->back    = at_back;
    app->tick    = at_tick;
    return true;
}

AOS_APP_ENTRY(at_init);
