/*
 * P4OS - Buscaminas (from AmoledOS)
 *
 * Five boards, now rectangular because the screen is: 8x11 with 12 mines,
 * 10x14 with 22, 12x16 with 34, 14x19 with 50 and 16x22 with 72 (the last
 * one about the density of the classic expert board). On the watch the two
 * big boards did not fit a finger and had a pinch zoom; on the 5" panel even
 * the 16x22 gets 42 px cells in portrait, more than the watch's 12x12 ever
 * had, so the zoom is gone and every board is drawn 1:1.
 *
 * Screen, portrait (720x1204 under the status bar):
 *
 *   header card    mines left | new game | time and record
 *   board          as big as the cell that fits both ways allows
 *   toolbar card   difficulty picker | Cavar / Bandera
 *                  (replaced by the result and "Jugar otra" when it ends)
 *
 * Landscape puts the three cards in a column on the left and the board on
 * the right. The board is TURNED rather than reflowed (see to_disp()): the
 * same game goes on and it keeps its shape - a board that is taller than wide
 * in portrait is wider than tall in landscape, which is what fills the glass.
 *
 * The board is ONE lv_canvas drawn with LVGL's primitives, a decision taken
 * on the watch, where 350 objects would have exhausted internal RAM; here it
 * is still the cheapest way to draw 352 cells. There is no frame timer: only
 * the cells that changed are redrawn. The only timer is the clock's.
 *
 * The game lives in statics (G) that survive the app being destroyed and
 * created again - which is what the runtime does when the screen turns - and
 * the clock stops while the app is not in front.
 */
#include "aos_app.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Boards -------------------------------------------------------------- */

#define LEVELS      5
#define MAX_CELLS   (16 * 22)

/* Columns x rows as seen in portrait. */
static const uint8_t LEVEL_C[LEVELS]     = { 8, 10, 12, 14, 16 };
static const uint8_t LEVEL_R[LEVELS]     = { 11, 14, 16, 19, 22 };
static const uint8_t LEVEL_MINES[LEVELS] = { 12, 22, 34, 50, 72 };
static const char *const LEVEL_NAME[LEVELS] = {
    N_("Fácil"), N_("Medio"), N_("Difícil"), N_("Experto"), N_("Maestro"),
};

#define ST_HIDDEN   0
#define ST_OPEN     1
#define ST_FLAG     2

typedef enum { GAME_READY = 0, GAME_RUN, GAME_LOST, GAME_WON } phase_t;

/* Classic number colours. Index 0 is unused. */
static const uint32_t NUM_COLOR[9] = {
    0x000000, 0x0A84FF, 0x30D158, 0xFF453A, 0xBF5AF2,
    0xFF9F0A, 0x40C8E0, 0xFFFFFF, 0x8E8E93,
};

#define C_TILE_HI   0x4A4A4E    /* hidden tile, top of its gradient */
#define C_TILE_LO   0x343436    /* ... and bottom                   */
#define C_OPEN      0x161618    /* opened cell: a flat field        */
#define C_BAD_FLAG  0x7A4A10    /* a flag where there was no mine   */
#define C_MINE_BG   0x8E1F18

/* ---- Layout --------------------------------------------------------------- */

#define MARGIN      16
#define GAP         12
#define HEAD_H      112
#define TOOL_H      104
#define SIDE_W      312         /* landscape: the column of cards */
#define CTRL_H      80

/* ---- The game: survives destroy/create ------------------------------------ */

static struct {
    bool     inited;
    int      level;
    int      cols, rows, mines;
    uint8_t  mine[MAX_CELLS];
    uint8_t  adj[MAX_CELLS];
    uint8_t  state[MAX_CELLS];
    int      opened, flags;
    int      boom;              /* the cell that exploded, -1 if none */
    phase_t  phase;
    bool     flag_mode;
    bool     xray;              /* MINES_XRAY=1: draws the hidden mines */
    bool     record;            /* the win that just happened beat the record */

    uint32_t start_ms;
    uint32_t elapsed_s;
    bool     paused;            /* the app is not in front: the clock stops */
    uint32_t run_ms;            /* ... and this is how far it had got */
    int      best[LEVELS];

    uint32_t rng;
    /* Stack for the cascading reveal, here and not on the LVGL task's. */
    int16_t  stack[MAX_CELLS];
} G;

/* ---- The UI: rebuilt on every create -------------------------------------- */

static struct {
    aos_app_t  *self;
    lv_obj_t   *root;
    lv_obj_t   *canvas;
    lv_obj_t   *touch;
    lv_obj_t   *lbl_mines, *lbl_time, *lbl_best;
    lv_obj_t   *btn_new;
    lv_obj_t   *controls, *result;
    lv_obj_t   *lbl_level, *lbl_level_dim;
    lv_obj_t   *seg_dig, *seg_flag;
    lv_obj_t   *lbl_res_title, *lbl_res_sub;
    lv_obj_t   *picker;
    lv_timer_t *timer;

    uint16_t *buf;              /* RGB565, PSRAM, big enough for the area */
    size_t    buf_cap;          /* in pixels */

    int  rot;                   /* 0/90/180/270: how the board is turned */
    int  bx, by, bw, bh;        /* the area the board may take, in root px */
    int  dcols, drows;          /* the board as displayed */
    int  cell;

    bool dirty;
    int  dx0, dy0, dx1, dy1;    /* display cells to redraw, inclusive */
} U;

/* -------------------------------------------------------------------------- */

static uint32_t rnd(void)
{
    uint32_t x = G.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    G.rng = x;
    return x;
}

static inline int idx(int x, int y) { return y * G.cols + x; }
static inline bool inside(int x, int y)
{
    return x >= 0 && x < G.cols && y >= 0 && y < G.rows;
}

static void fmt_time(char *out, size_t n, uint32_t s)
{
    snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

/* --------------------------------------------------------------------------
 * The board, turned
 *
 * The game always thinks in portrait columns and rows. On screen the board
 * is turned with the glass, so that when the device is physically rotated the
 * cells stay where they were under the finger (the P4 HAL turns the image by
 * 'rotation' degrees). Turning and not mirroring: a mirrored board is the
 * same game, but the pattern one was reading would jump around.
 * -------------------------------------------------------------------------- */

static void to_disp(int c, int r, int *dc, int *dr)
{
    switch (U.rot) {
    case 90:  *dc = r;              *dr = G.cols - 1 - c; break;
    case 180: *dc = G.cols - 1 - c; *dr = G.rows - 1 - r; break;
    case 270: *dc = G.rows - 1 - r; *dr = c;              break;
    default:  *dc = c;              *dr = r;              break;
    }
}

static void to_logic(int dc, int dr, int *c, int *r)
{
    switch (U.rot) {
    case 90:  *r = dc;              *c = G.cols - 1 - dr; break;
    case 180: *c = G.cols - 1 - dc; *r = G.rows - 1 - dr; break;
    case 270: *r = G.rows - 1 - dc; *c = dr;              break;
    default:  *c = dc;              *r = dr;              break;
    }
}

/* -------------------------------------------------------------------------- */
/* Redrawing by rectangle of (display) cells                                   */

static void dirty_add(int x0, int y0, int x1, int y1)
{
    if (!U.dirty) {
        U.dirty = true;
        U.dx0 = x0; U.dy0 = y0; U.dx1 = x1; U.dy1 = y1;
        return;
    }
    if (x0 < U.dx0) U.dx0 = x0;
    if (y0 < U.dy0) U.dy0 = y0;
    if (x1 > U.dx1) U.dx1 = x1;
    if (y1 > U.dy1) U.dy1 = y1;
}

static void dirty_cell(int x, int y)
{
    int dc, dr;
    to_disp(x, y, &dc, &dr);
    dirty_add(dc, dr, dc, dr);
}

static void dirty_all(void)
{
    dirty_add(0, 0, U.dcols - 1, U.drows - 1);
}

/* -------------------------------------------------------------------------- */
/* Drawing a cell                                                              */

static void fill(lv_layer_t *layer, const lv_area_t *area, uint32_t color,
                 int32_t radius)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa   = LV_OPA_COVER;
    dsc.radius   = radius;
    lv_draw_rect(layer, &dsc, area);
}

static void fill_grad(lv_layer_t *layer, const lv_area_t *area, uint32_t top,
                      uint32_t bottom, int32_t radius)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(top);
    dsc.bg_opa   = LV_OPA_COVER;
    dsc.bg_grad.dir = LV_GRAD_DIR_VER;
    dsc.bg_grad.stops_count = 2;
    dsc.bg_grad.stops[0].color = lv_color_hex(top);
    dsc.bg_grad.stops[0].opa   = LV_OPA_COVER;
    dsc.bg_grad.stops[0].frac  = 0;
    dsc.bg_grad.stops[1].color = lv_color_hex(bottom);
    dsc.bg_grad.stops[1].opa   = LV_OPA_COVER;
    dsc.bg_grad.stops[1].frac  = 255;
    dsc.radius   = radius;
    lv_draw_rect(layer, &dsc, area);
}

static void line(lv_layer_t *layer, int x1, int y1, int x2, int y2, int w,
                 uint32_t color)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.p1.x = x1; dsc.p1.y = y1;
    dsc.p2.x = x2; dsc.p2.y = y2;
    dsc.width = w;
    dsc.color = lv_color_hex(color);
    dsc.opa = LV_OPA_COVER;
    dsc.round_start = 1;
    dsc.round_end = 1;
    lv_draw_line(layer, &dsc);
}

/* Text centred on both axes inside 'box'. lv_draw_label centres horizontally
 * with 'align' but starts at the top, so the vertical centring is by hand. */
static void centered_text(lv_layer_t *layer, const lv_area_t *box,
                          const char *text, const lv_font_t *font,
                          uint32_t color)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text  = text;
    dsc.font  = font;
    dsc.color = lv_color_hex(color);
    dsc.align = LV_TEXT_ALIGN_CENTER;
    dsc.opa   = LV_OPA_COVER;

    int32_t lh = lv_font_get_line_height(font);
    lv_area_t a = *box;
    a.y1 = (box->y1 + box->y2 + 1 - lh) / 2;
    a.y2 = a.y1 + lh;
    lv_draw_label(layer, &dsc, &a);
}

static const lv_font_t *cell_font(void)
{
    if (U.cell >= 76) return aos_font_large;
    if (U.cell >= 54) return aos_font_title;
    return aos_font_body;
}

static void draw_mine(lv_layer_t *layer, const lv_area_t *box)
{
    int cx = (box->x1 + box->x2) / 2;
    int cy = (box->y1 + box->y2) / 2;
    int r  = U.cell / 5;
    int len = U.cell * 3 / 10;
    int th  = LV_MAX(2, U.cell / 12);
    int dg  = len * 7 / 10;

    line(layer, cx - len, cy, cx + len, cy, th, 0x000000);
    line(layer, cx, cy - len, cx, cy + len, th, 0x000000);
    line(layer, cx - dg, cy - dg, cx + dg, cy + dg, th, 0x000000);
    line(layer, cx - dg, cy + dg, cx + dg, cy - dg, th, 0x000000);

    lv_area_t body = { cx - r, cy - r, cx + r, cy + r };
    fill(layer, &body, 0x000000, LV_RADIUS_CIRCLE);

    /* the glint that makes it round */
    int g = LV_MAX(2, r / 3);
    lv_area_t glint = { cx - r / 2 - g / 2, cy - r / 2 - g / 2,
                        cx - r / 2 + g / 2, cy - r / 2 + g / 2 };
    fill(layer, &glint, 0xD0D0D4, LV_RADIUS_CIRCLE);
}

static void draw_flag(lv_layer_t *layer, const lv_area_t *box)
{
    int cx = (box->x1 + box->x2) / 2 + U.cell / 10;
    int cy = (box->y1 + box->y2) / 2;
    int h  = U.cell * 3 / 10;
    int th = LV_MAX(2, U.cell / 14);

    lv_area_t pole = { cx - th / 2, cy - h, cx - th / 2 + th, cy + h };
    fill(layer, &pole, 0xE0E0E0, 0);

    /* the pennant, pointing left off the top of the pole */
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = lv_color_hex(0xFF453A);
    tri.opa   = LV_OPA_COVER;
    tri.p[0].x = cx - th / 2;               tri.p[0].y = cy - h;
    tri.p[1].x = cx - th / 2;               tri.p[1].y = cy - h + h * 11 / 10;
    tri.p[2].x = cx - th / 2 - U.cell * 7 / 20; tri.p[2].y = cy - h + h * 11 / 20;
    lv_draw_triangle(layer, &tri);

    /* a two-step base, wide enough that the flag never reads as a "1" */
    int bw = U.cell / 4, bh = LV_MAX(2, th * 3 / 4);
    lv_area_t step = { cx - bw * 3 / 5, cy + h - 2 * bh, cx + bw * 3 / 5, cy + h - bh };
    fill(layer, &step, 0xE0E0E0, 0);
    lv_area_t foot = { cx - bw, cy + h - bh, cx + bw, cy + h };
    fill(layer, &foot, 0xE0E0E0, LV_MAX(1, bh / 2));
}

static void draw_cell(lv_layer_t *layer, int dc, int dr)
{
    lv_area_t box = {
        .x1 = dc * U.cell,
        .y1 = dr * U.cell,
    };
    box.x2 = box.x1 + U.cell - 1;
    box.y2 = box.y1 + U.cell - 1;

    int x, y;
    to_logic(dc, dr, &x, &y);
    int i = idx(x, y);

    /* a little air between hidden tiles: the board reads without grid lines */
    int inset = LV_MAX(1, U.cell / 24);
    lv_area_t tile = { box.x1 + inset, box.y1 + inset,
                       box.x2 - inset, box.y2 - inset };
    int radius = LV_MAX(3, U.cell / 6);

    bool reveal_mines = (G.phase == GAME_LOST) || G.xray;

    if (G.state[i] == ST_FLAG) {
        if (reveal_mines && !G.mine[i]) {
            fill(layer, &tile, C_BAD_FLAG, radius);
        } else {
            fill_grad(layer, &tile, C_TILE_HI, C_TILE_LO, radius);
        }
        draw_flag(layer, &box);
        return;
    }

    if (G.state[i] == ST_HIDDEN) {
        if (reveal_mines && G.mine[i]) {
            fill(layer, &tile, C_MINE_BG, radius);
            draw_mine(layer, &box);
        } else {
            fill_grad(layer, &tile, C_TILE_HI, C_TILE_LO, radius);
        }
        return;
    }

    /* revealed */
    if (G.mine[i]) {
        fill(layer, &tile, i == G.boom ? 0xFF453A : C_MINE_BG, radius);
        draw_mine(layer, &box);
        return;
    }

    /* An opened cell fills its whole box with no radius, so an opened region
     * reads as one flat field and not as a mosaic of tiles. */
    fill(layer, &box, C_OPEN, 0);
    if (G.adj[i] > 0) {
        /* Static literals: lv_draw_label() keeps THE POINTER until the layer
         * is finished, so a buffer on this stack would be gone by then. */
        static const char *const DIGIT[9] = {
            "", "1", "2", "3", "4", "5", "6", "7", "8",
        };
        centered_text(layer, &box, DIGIT[G.adj[i]], cell_font(),
                      NUM_COLOR[G.adj[i]]);
    }
}

static void present(void)
{
    if (!U.dirty || !U.canvas || !U.buf) {
        return;
    }
    U.dirty = false;

    lv_layer_t layer;
    lv_canvas_init_layer(U.canvas, &layer);

    /* Black under the whole dirty rectangle first: the tiles leave gaps and
     * those must not keep the previous board's pixels. */
    lv_area_t bg = {
        .x1 = U.dx0 * U.cell,
        .y1 = U.dy0 * U.cell,
        .x2 = (U.dx1 + 1) * U.cell - 1,
        .y2 = (U.dy1 + 1) * U.cell - 1,
    };
    fill(&layer, &bg, 0x000000, 0);

    for (int y = U.dy0; y <= U.dy1; y++) {
        for (int x = U.dx0; x <= U.dx1; x++) {
            draw_cell(&layer, x, y);
        }
    }
    lv_canvas_finish_layer(U.canvas, &layer);
}

/* -------------------------------------------------------------------------- */
/* Header and toolbar                                                          */

static void refresh_mode(void)
{
    if (!U.seg_dig) return;
    lv_obj_set_style_bg_opa(U.seg_dig, G.flag_mode ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(U.seg_flag, G.flag_mode ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void refresh_info(void)
{
    if (!U.lbl_mines) return;
    char buf[48];

    snprintf(buf, sizeof(buf), "%d", G.mines - G.flags);
    lv_label_set_text(U.lbl_mines, buf);

    fmt_time(buf, sizeof(buf), G.elapsed_s);
    lv_label_set_text(U.lbl_time, buf);

    if (G.best[G.level] > 0) {
        char t[16];
        fmt_time(t, sizeof(t), (uint32_t)G.best[G.level]);
        snprintf(buf, sizeof(buf), _("Récord %s"), t);
        lv_label_set_text(U.lbl_best, buf);
    } else {
        lv_label_set_text(U.lbl_best, _("Sin récord"));
    }

    lv_label_set_text(U.lbl_level, _(LEVEL_NAME[G.level]));
    snprintf(buf, sizeof(buf), "%d\xC3\x97%d", G.cols, G.rows);
    lv_label_set_text(U.lbl_level_dim, buf);

    /* the result replaces the controls when the game is over */
    bool over = G.phase == GAME_WON || G.phase == GAME_LOST;
    if (over) {
        lv_obj_add_flag(U.controls, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(U.result, LV_OBJ_FLAG_HIDDEN);
        if (G.phase == GAME_WON) {
            lv_label_set_text(U.lbl_res_title, _("¡Ganaste!"));
            lv_obj_set_style_text_color(U.lbl_res_title, AOS_C_GREEN, 0);
            char t[16];
            fmt_time(t, sizeof(t), G.elapsed_s);
            snprintf(buf, sizeof(buf), G.record ? _("%s, nuevo récord") : "%s", t);
            lv_label_set_text(U.lbl_res_sub, buf);
        } else {
            lv_label_set_text(U.lbl_res_title, _("¡Boom!"));
            lv_obj_set_style_text_color(U.lbl_res_title, AOS_C_RED, 0);
            lv_label_set_text(U.lbl_res_sub, _("Pisaste una mina"));
        }
    } else {
        lv_obj_remove_flag(U.controls, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.result, LV_OBJ_FLAG_HIDDEN);
    }

    lv_color_t nc = G.phase == GAME_WON ? AOS_C_GREEN
                  : G.phase == GAME_LOST ? AOS_C_RED : AOS_C_CARD2;
    lv_obj_set_style_bg_color(U.btn_new, nc, 0);
}

/* -------------------------------------------------------------------------- */
/* Rules                                                                       */

static void place_mines(int safe)
{
    int total = G.cols * G.rows;
    int sx = safe % G.cols, sy = safe / G.cols;

    memset(G.mine, 0, sizeof(G.mine));
    int placed = 0;
    while (placed < G.mines) {
        int i = (int)(rnd() % (uint32_t)total);
        if (G.mine[i]) {
            continue;
        }
        /* The first cell and its eight neighbours are left clear: the first
         * touch always opens a region, as it has since Windows 3.1. */
        int x = i % G.cols, y = i / G.cols;
        if (x >= sx - 1 && x <= sx + 1 && y >= sy - 1 && y <= sy + 1) {
            continue;
        }
        G.mine[i] = 1;
        placed++;
    }

    for (int y = 0; y < G.rows; y++) {
        for (int x = 0; x < G.cols; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if ((dx || dy) && inside(x + dx, y + dy) &&
                        G.mine[idx(x + dx, y + dy)]) {
                        n++;
                    }
                }
            }
            G.adj[idx(x, y)] = (uint8_t)n;
        }
    }
}

static void check_win(void)
{
    if (G.phase != GAME_RUN) {
        return;
    }
    if (G.opened != G.cols * G.rows - G.mines) {
        return;
    }
    G.phase  = GAME_WON;
    G.elapsed_s = (lv_tick_get() - G.start_ms) / 1000u;

    int t = (int)G.elapsed_s;
    if (t < 1) t = 1;
    G.record = false;
    if (G.best[G.level] == 0 || t < G.best[G.level]) {
        G.record = true;
        G.best[G.level] = t;
        char key[32];
        snprintf(key, sizeof(key), "mn_best%d", G.level);
        aos_hal_pref_set_i32(key, t);
    }

    /* the remaining mines flag themselves, which is the reward */
    for (int i = 0; i < G.cols * G.rows; i++) {
        if (G.mine[i] && G.state[i] != ST_FLAG) {
            G.state[i] = ST_FLAG;
            G.flags++;
        }
    }
    dirty_all();
    aos_hal_beep(880, 60);
    aos_hal_beep(1320, 120);
}

/* Cascading reveal, with an explicit stack. A cell is marked on PUSHING, so
 * each one enters exactly once and MAX_CELLS is an exact bound. */
static void open_cell(int x, int y)
{
    int first = idx(x, y);
    if (G.state[first] != ST_HIDDEN) {
        return;
    }

    int top = 0;
    G.state[first] = ST_OPEN;
    G.opened++;
    dirty_cell(x, y);
    G.stack[top++] = (int16_t)first;

    while (top > 0) {
        int i  = G.stack[--top];
        int cx = i % G.cols, cy = i / G.cols;

        if (G.mine[i]) {        /* it can only be the first */
            G.boom   = i;
            G.phase  = GAME_LOST;
                    dirty_all();
            aos_hal_beep(140, 350);
            return;
        }
        if (G.adj[i] != 0) {
            continue;
        }
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if ((!dx && !dy) || !inside(cx + dx, cy + dy)) {
                    continue;
                }
                int j = idx(cx + dx, cy + dy);
                if (G.state[j] != ST_HIDDEN) {
                    continue;   /* the flags are honoured */
                }
                G.state[j] = ST_OPEN;
                G.opened++;
                dirty_cell(cx + dx, cy + dy);
                G.stack[top++] = (int16_t)j;
            }
        }
    }
    check_win();
}

static void toggle_flag(int x, int y)
{
    int i = idx(x, y);
    if (G.state[i] == ST_OPEN || G.phase == GAME_LOST || G.phase == GAME_WON) {
        return;
    }
    if (G.state[i] == ST_FLAG) {
        G.state[i] = ST_HIDDEN;
        G.flags--;
        aos_hal_beep(520, 25);
    } else {
        G.state[i] = ST_FLAG;
        G.flags++;
        aos_hal_beep(980, 25);
    }
    dirty_cell(x, y);
    refresh_info();
}

/* Touching a revealed number with as many flags around it as it says opens
 * the remaining neighbours: the classic "chord". */
static void chord(int x, int y)
{
    int i = idx(x, y);
    if (G.adj[i] == 0) {
        return;
    }
    int flagged = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if ((dx || dy) && inside(x + dx, y + dy) &&
                G.state[idx(x + dx, y + dy)] == ST_FLAG) {
                flagged++;
            }
        }
    }
    if (flagged != G.adj[i]) {
        return;
    }
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if ((dx || dy) && inside(x + dx, y + dy) &&
                G.state[idx(x + dx, y + dy)] == ST_HIDDEN) {
                open_cell(x + dx, y + dy);
                if (G.phase == GAME_LOST) {
                    return;
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Board geometry                                                              */

/* The largest cell that fits the area both ways, and the canvas centred in
 * it. Called on every new game (the board changes size) and on create. */
static void board_layout(void)
{
    if (!U.canvas || !U.buf) {
        return;
    }
    bool land = U.rot == 90 || U.rot == 270;
    U.dcols = land ? G.rows : G.cols;
    U.drows = land ? G.cols : G.rows;
    int cw = U.bw / U.dcols, ch = U.bh / U.drows;
    U.cell = cw < ch ? cw : ch;

    int w = U.dcols * U.cell, h = U.drows * U.cell;
    lv_canvas_set_buffer(U.canvas, U.buf, w, h, LV_COLOR_FORMAT_RGB565);
    memset(U.buf, 0, (size_t)w * h * 2);
    lv_obj_set_size(U.canvas, w, h);
    lv_obj_set_pos(U.canvas, U.bx + (U.bw - w) / 2, U.by + (U.bh - h) / 2);
    U.dirty = false;
    dirty_all();
}

static void new_game(void)
{
    G.cols  = LEVEL_C[G.level];
    G.rows  = LEVEL_R[G.level];
    G.mines = LEVEL_MINES[G.level];
    G.opened = 0;
    G.flags  = 0;
    G.boom   = -1;
    G.phase  = GAME_READY;
    G.record = false;
    G.paused = false;
    G.elapsed_s = 0;
    G.start_ms  = lv_tick_get();
    memset(G.mine, 0, sizeof(G.mine));
    memset(G.adj, 0, sizeof(G.adj));
    memset(G.state, ST_HIDDEN, sizeof(G.state));
    G.inited = true;

    board_layout();
    refresh_info();
}

/* The clock does not run while the app is away. */
static void clock_pause(void)
{
    if (G.phase == GAME_RUN && !G.paused) {
        G.run_ms = lv_tick_get() - G.start_ms;
        G.paused = true;
    }
}

static void clock_resume(void)
{
    if (G.paused) {
        G.start_ms = lv_tick_get() - G.run_ms;
        G.paused = false;
    }
}

/* -------------------------------------------------------------------------- */
/* Input                                                                       */

static bool touch_cell(float px, float py, int *out_x, int *out_y)
{
    if (!U.canvas || U.cell <= 0) {
        return false;
    }
    lv_area_t co;
    lv_obj_get_coords(U.canvas, &co);
    int bx = (int)px - co.x1, by = (int)py - co.y1;
    if (bx < 0 || by < 0) {
        return false;
    }
    int dc = bx / U.cell, dr = by / U.cell;
    if (dc >= U.dcols || dr >= U.drows) {
        return false;
    }
    to_logic(dc, dr, out_x, out_y);
    return inside(*out_x, *out_y);
}

static void tap(float px, float py)
{
    /* A finished board stays still to be looked at: where the other mines
     * were, which flag was wrong. The watch restarted on the next touch after
     * a moment of deafness; here "Jugar otra" sits right under it. */
    if (G.phase == GAME_LOST || G.phase == GAME_WON) {
        return;
    }
    int x, y;
    if (!touch_cell(px, py, &x, &y)) {
        return;
    }
    if (G.flag_mode) {
        /* Flagging does not start the game: the first dig must stay safe. */
        toggle_flag(x, y);
        present();
        return;
    }

    if (G.phase == GAME_READY) {
        place_mines(idx(x, y));
        if (G.xray) {
            dirty_all();        /* the mines did not exist a moment ago */
        }
        G.phase    = GAME_RUN;
        G.start_ms = lv_tick_get();
    }

    int i = idx(x, y);
    if (G.state[i] == ST_FLAG) {
        return;                 /* a flagged cell is not dug with one tap */
    }
    if (G.state[i] == ST_OPEN) {
        chord(x, y);
    } else {
        open_cell(x, y);
    }
    refresh_info();
    present();
}

/* Tap digs (or flags, in flag mode), a long press always flags.
 *
 * With LVGL's own events and not aos_gesture: there is no drag or pinch any
 * more, and SHORT_CLICKED already tells a tap from a long press. The press
 * point is kept because the release one is where the finger ended up; a
 * touch that wandered more than a third of a cell is not a tap. */
static lv_point_t s_press;

static void touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_press = p;
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        int lim = LV_MAX(16, U.cell / 3);
        if (LV_ABS(p.x - s_press.x) > lim || LV_ABS(p.y - s_press.y) > lim) {
            return;
        }
        tap((float)s_press.x, (float)s_press.y);
    } else if (code == LV_EVENT_LONG_PRESSED) {
        int x, y;
        if (touch_cell((float)s_press.x, (float)s_press.y, &x, &y) &&
            (G.phase == GAME_READY || G.phase == GAME_RUN)) {
            toggle_flag(x, y);
            present();
        }
    }
}

static void dig_cb(lv_event_t *event)
{
    (void)event;
    G.flag_mode = false;
    refresh_mode();
}

static void flag_cb(lv_event_t *event)
{
    (void)event;
    G.flag_mode = true;
    refresh_mode();
}

static void new_cb(lv_event_t *event)
{
    (void)event;
    new_game();
    present();
}

/* The clock: only when the second changed, and outside the canvas. */
static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (G.phase != GAME_RUN || G.paused) {
        return;
    }
    uint32_t s = (lv_tick_get() - G.start_ms) / 1000u;
    if (s == G.elapsed_s || s > 5999) {
        return;
    }
    G.elapsed_s = s;
    char buf[16];
    fmt_time(buf, sizeof(buf), s);
    lv_label_set_text(U.lbl_time, buf);
}

/* -------------------------------------------------------------------------- */
/* Difficulty picker                                                           */

static void picker_close(void)
{
    if (U.picker) {
        lv_obj_delete(U.picker);
        U.picker = NULL;
    }
}

static void picker_dim_cb(lv_event_t *e)
{
    /* only a touch on the dim itself, not one that bubbled from the card */
    if (lv_event_get_target(e) == U.picker) {
        picker_close();
    }
}

static void picker_pick_cb(lv_event_t *e)
{
    int lvl = (int)(intptr_t)lv_event_get_user_data(e);
    picker_close();
    if (lvl != G.level || G.phase != GAME_READY) {
        G.level = lvl;
        aos_hal_pref_set_i32("mn_level", G.level);
        new_game();
        present();
    }
}

static lv_obj_t *box(lv_obj_t *parent, lv_color_t color, int32_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text,
                       const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = aos_label(parent, text, font, color);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void level_cb(lv_event_t *event)
{
    (void)event;
    if (U.picker) {
        return;
    }
    int32_t W = lv_obj_get_width(U.root), H = lv_obj_get_height(U.root);

    U.picker = box(U.root, lv_color_black(), 0);
    lv_obj_set_size(U.picker, W, H);
    lv_obj_set_style_bg_opa(U.picker, LV_OPA_60, 0);
    lv_obj_add_flag(U.picker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.picker, picker_dim_cb, LV_EVENT_CLICKED, NULL);

    const int row_h = 88, row_gap = 8, pad = 20, title_h = 64;
    int32_t cw = W - 2 * 24;
    if (cw > 600) cw = 600;
    int32_t chh = pad + title_h + LEVELS * row_h + (LEVELS - 1) * row_gap + pad;

    lv_obj_t *card = box(U.picker, AOS_C_CARD, AOS_UI_RADIUS);
    lv_obj_set_size(card, cw, chh);
    lv_obj_center(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);    /* the dim stays shut */

    lv_obj_t *t = label(card, _("Dificultad"), aos_font_title, AOS_C_TEXT);
    lv_obj_set_pos(t, pad + 8, pad + 8);

    for (int i = 0; i < LEVELS; i++) {
        lv_obj_t *r = box(card, AOS_C_CARD2, 20);
        lv_obj_set_size(r, cw - 2 * pad, row_h);
        lv_obj_set_pos(r, pad, pad + title_h + i * (row_h + row_gap));
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
        lv_obj_add_event_cb(r, picker_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (i == G.level) {
            lv_obj_set_style_border_color(r, AOS_C_ACCENT, 0);
            lv_obj_set_style_border_width(r, 3, 0);
            lv_obj_set_style_border_opa(r, LV_OPA_COVER, 0);
        }

        lv_obj_t *n = label(r, _(LEVEL_NAME[i]), aos_font_body, AOS_C_TEXT);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 24, -14);

        char buf[48];
        snprintf(buf, sizeof(buf), _("%d\xC3\x97%d, %d minas"),
                 LEVEL_C[i], LEVEL_R[i], LEVEL_MINES[i]);
        lv_obj_t *d = label(r, buf, aos_font_caption, AOS_C_DIM);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, 24, 20);

        if (G.best[i] > 0) {
            fmt_time(buf, sizeof(buf), (uint32_t)G.best[i]);
        } else {
            snprintf(buf, sizeof(buf), "-");
        }
        lv_obj_t *b = label(r, buf, aos_font_body,
                            G.best[i] > 0 ? AOS_C_YELLOW : AOS_C_DIM);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -24, 0);
    }
}

/* -------------------------------------------------------------------------- */
/* Construction                                                                */

static lv_obj_t *card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *c = box(parent, AOS_C_CARD, AOS_UI_RADIUS);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    return c;
}

static lv_obj_t *press_btn(lv_obj_t *parent, lv_color_t color, int32_t radius,
                           lv_event_cb_t cb)
{
    lv_obj_t *b = box(parent, color, radius);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* A number with its glyph and a caption under it. 'right' mirrors it for the
 * right-hand end of the portrait header. */
static void stat_block(lv_obj_t *parent, const char *glyph, lv_color_t gcol,
                       bool right, lv_obj_t **num, lv_obj_t **cap,
                       const char *cap_text)
{
    lv_align_t al = right ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID;
    int s = right ? -1 : 1;

    lv_obj_t *g = label(parent, glyph, &aos_sym_44, gcol);
    lv_obj_align(g, al, s * 24, 0);

    *num = label(parent, "0", aos_font_large, AOS_C_TEXT);
    lv_obj_align(*num, al, s * 84, -12);

    *cap = label(parent, cap_text, aos_font_caption, AOS_C_DIM);
    lv_obj_align(*cap, al, s * 86, 30);
}

/* Level button + Cavar/Bandera, in a w x h rectangle; 'stacked' puts them one
 * over the other (landscape column) instead of side by side. */
static void build_controls(lv_obj_t *parent, int32_t w, bool stacked)
{
    U.controls = box(parent, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(U.controls, LV_OPA_TRANSP, 0);
    int32_t h = stacked ? 2 * CTRL_H + GAP : CTRL_H;
    lv_obj_set_size(U.controls, w, h);

    int32_t lw = stacked ? w : (w - GAP) / 2;
    int32_t sw = stacked ? w : w - GAP - lw;

    lv_obj_t *lb = press_btn(U.controls, AOS_C_CARD2, 22, level_cb);
    lv_obj_set_size(lb, lw, CTRL_H);
    lv_obj_set_pos(lb, 0, 0);
    U.lbl_level = label(lb, "", aos_font_body, AOS_C_TEXT);
    lv_obj_align(U.lbl_level, LV_ALIGN_LEFT_MID, 22, 0);
    U.lbl_level_dim = label(lb, "", aos_font_small, AOS_C_DIM);
    lv_obj_align(U.lbl_level_dim, LV_ALIGN_RIGHT_MID, -58, 0);
    lv_obj_t *chev = label(lb, AOS_SYM_CHEVRON_DOWN, &aos_sym_28, AOS_C_DIM);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -18, 0);

    lv_obj_t *seg = box(U.controls, AOS_C_CARD2, 22);
    lv_obj_set_size(seg, sw, CTRL_H);
    lv_obj_set_pos(seg, stacked ? 0 : lw + GAP, stacked ? CTRL_H + GAP : 0);

    int32_t half = (sw - 12) / 2;
    U.seg_dig = press_btn(seg, AOS_C_ACCENT, 17, dig_cb);
    lv_obj_set_size(U.seg_dig, half, CTRL_H - 12);
    lv_obj_set_pos(U.seg_dig, 6, 6);
    lv_obj_t *l1 = label(U.seg_dig, _("Cavar"), aos_font_body, AOS_C_TEXT);
    lv_obj_center(l1);

    U.seg_flag = press_btn(seg, AOS_C_ORANGE, 17, flag_cb);
    lv_obj_set_size(U.seg_flag, half, CTRL_H - 12);
    lv_obj_set_pos(U.seg_flag, 6 + half, 6);
    lv_obj_t *l2 = label(U.seg_flag, _("Bandera"), aos_font_body, AOS_C_TEXT);
    lv_obj_center(l2);
}

/* The end of the game, in the controls' place. */
static void build_result(lv_obj_t *parent, int32_t w, bool stacked)
{
    U.result = box(parent, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(U.result, LV_OPA_TRANSP, 0);
    int32_t h = stacked ? 2 * CTRL_H + GAP : CTRL_H;
    lv_obj_set_size(U.result, w, h);
    lv_obj_add_flag(U.result, LV_OBJ_FLAG_HIDDEN);

    U.lbl_res_title = label(U.result, "", aos_font_title, AOS_C_GREEN);
    U.lbl_res_sub   = label(U.result, "", aos_font_small, AOS_C_DIM);

    lv_obj_t *b = press_btn(U.result, AOS_C_ACCENT, 22, new_cb);
    lv_obj_t *bl = label(b, _("Jugar otra"), aos_font_body, AOS_C_TEXT);
    lv_obj_center(bl);

    if (stacked) {
        lv_obj_align(U.lbl_res_title, LV_ALIGN_TOP_LEFT, 8, 4);
        lv_obj_align(U.lbl_res_sub, LV_ALIGN_TOP_LEFT, 8, 52);
        lv_obj_set_size(b, w, CTRL_H);
        lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, 0);
    } else {
        lv_obj_align(U.lbl_res_title, LV_ALIGN_LEFT_MID, 10, -16);
        lv_obj_align(U.lbl_res_sub, LV_ALIGN_LEFT_MID, 10, 22);
        lv_obj_set_size(b, 240, CTRL_H);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static lv_obj_t *new_button(lv_obj_t *parent, int32_t w, int32_t h)
{
    U.btn_new = press_btn(parent, AOS_C_CARD2, LV_RADIUS_CIRCLE, new_cb);
    lv_obj_set_size(U.btn_new, w, h);
    lv_obj_t *g = label(U.btn_new, AOS_SYM_RESTART, &aos_sym_44, AOS_C_TEXT);
    lv_obj_center(g);
    return U.btn_new;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&U, 0, sizeof(U));
    U.self = self;
    U.root = root;

    if (!G.inited) {
        G.rng  = (uint32_t)aos_hal_uptime_ms() * 2654435761u | 1u;
        G.boom = -1;
        /* Development switch: on the board getenv() always returns NULL. */
        const char *xray = getenv("MINES_XRAY");
        G.xray = (xray && xray[0] && xray[0] != '0');
        /* MINES_SEED=N: the same boards on every run, for scripted tests. */
        const char *seed = getenv("MINES_SEED");
        if (seed && seed[0]) {
            G.rng = (uint32_t)strtoul(seed, NULL, 10) * 2654435761u | 1u;
        }

        int32_t v = 0;
        if (aos_hal_pref_get_i32("mn_level", &v) && v >= 0 && v < LEVELS) {
            G.level = (int)v;
        }
        for (int i = 0; i < LEVELS; i++) {
            char key[32];
            snprintf(key, sizeof(key), "mn_best%d", i);
            if (aos_hal_pref_get_i32(key, &v) && v > 0) {
                G.best[i] = (int)v;
            }
        }
    }

    int32_t W = lv_obj_get_width(root), H = lv_obj_get_height(root);
    bool land = W > H;
    int hr = aos_hal_display_get_rotation();
    U.rot = land ? (hr == 270 ? 270 : 90) : (hr == 180 ? 180 : 0);

    lv_obj_t *page = aos_page(root);

    if (!land) {
        lv_obj_t *head = card(page, MARGIN, 12, W - 2 * MARGIN, HEAD_H);
        lv_obj_t *dummy;
        stat_block(head, AOS_SYM_BOMB, AOS_C_RED, false, &U.lbl_mines, &dummy, _("minas"));
        stat_block(head, AOS_SYM_TIMER_OUTLINE, AOS_C_TEAL, true, &U.lbl_time, &U.lbl_best, "");
        lv_obj_center(new_button(head, 88, 88));

        U.bx = MARGIN;
        U.by = 12 + HEAD_H + GAP;
        U.bw = W - 2 * MARGIN;
        U.bh = H - 12 - TOOL_H - GAP - U.by;

        lv_obj_t *tool = card(page, MARGIN, H - 12 - TOOL_H, W - 2 * MARGIN, TOOL_H);
        int32_t inner = W - 2 * MARGIN - 24;
        build_controls(tool, inner, false);
        lv_obj_center(U.controls);
        build_result(tool, inner, false);
        lv_obj_center(U.result);
    } else {
        int32_t side_h = H - 24;
        lv_obj_t *side = card(page, 12, 12, SIDE_W, side_h);

        lv_obj_t *m = box(side, AOS_C_CARD, 0);
        lv_obj_set_size(m, SIDE_W, 110);
        lv_obj_set_pos(m, 0, 12);
        lv_obj_t *dummy;
        stat_block(m, AOS_SYM_BOMB, AOS_C_RED, false, &U.lbl_mines, &dummy, _("minas"));

        lv_obj_t *t = box(side, AOS_C_CARD, 0);
        lv_obj_set_size(t, SIDE_W, 110);
        lv_obj_set_pos(t, 0, 122);
        stat_block(t, AOS_SYM_TIMER_OUTLINE, AOS_C_TEAL, false, &U.lbl_time, &U.lbl_best, "");

        lv_obj_t *nb = new_button(side, SIDE_W - 24, CTRL_H);
        lv_obj_set_style_radius(nb, 22, 0);
        lv_obj_set_pos(nb, 12, 244);

        int32_t cw = SIDE_W - 24;
        build_controls(side, cw, true);
        lv_obj_align(U.controls, LV_ALIGN_BOTTOM_MID, 0, -12);
        build_result(side, cw, true);
        lv_obj_align(U.result, LV_ALIGN_BOTTOM_MID, 0, -12);

        U.bx = 12 + SIDE_W + GAP;
        U.by = 12;
        U.bw = W - U.bx - 12;
        U.bh = H - 24;
    }

    /* The canvas buffer is sized for the whole area once: every board and
     * both orientations of it fit inside. malloc() sends it to PSRAM. */
    U.buf_cap = (size_t)U.bw * U.bh;
    U.buf = malloc(U.buf_cap * 2);
    if (!U.buf) {
        aos_ui_toast(_("Sin memoria"), 2000);
    }

    U.canvas = lv_canvas_create(page);
    /* The theme gives everything a radius, and on a canvas that forces
     * clipping with a mask and drawing in layers. */
    lv_obj_set_style_radius(U.canvas, 0, 0);
    lv_obj_remove_flag(U.canvas, LV_OBJ_FLAG_CLICKABLE);

    /* over the board: taps and long presses */
    U.touch = box(page, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(U.touch, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(U.touch, U.bx, U.by);
    lv_obj_set_size(U.touch, U.bw, U.bh);
    lv_obj_add_flag(U.touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.touch, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(U.touch, touch_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(U.touch, touch_cb, LV_EVENT_LONG_PRESSED, NULL);

    if (!G.inited) {
        new_game();
    } else {
        board_layout();
        clock_resume();
        refresh_info();
    }
    refresh_mode();
    present();

    U.timer = lv_timer_create(tick_cb, 200, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)inst;
    clock_pause();
    if (U.timer) {
        lv_timer_delete(U.timer);
        U.timer = NULL;
    }
    /* The objects first: the canvas points at the buffer about to be freed. */
    if (self && self->root) {
        lv_obj_clean(self->root);
    }
    free(U.buf);
    memset(&U, 0, sizeof(U));
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    clock_pause();
}

static void show(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    clock_resume();
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.picker) {
        picker_close();
        return true;
    }
    return false;
}

static bool buscaminas_init(aos_app_t *app)
{
    app->desc.id       = "aos.mines";
    app->desc.name     = "Buscaminas";
    app->desc.icon     = "Bm";
    app->desc.icon_vec = AOS_ICON_MINES;
    app->desc.color_a  = 0x5A5A5E;
    app->desc.color_b  = 0x1C1C1E;
    app->desc.flags    = AOS_APP_FLAG_NONE;
    app->desc.order    = 154;

    app->create        = create;
    app->destroy       = destroy;
    app->show          = show;
    app->hide          = hide;
    app->back          = back;
    return true;
}

AOS_APP_ENTRY(buscaminas_init);
