/*
 * SUDOKU - the board: one lv_canvas, drawn cell by cell (Buscaminas'
 * decision: 81 objects would cost more than they draw), only the cells that
 * changed. A selection lights its row, column, box and every cell with the
 * same digit, so a new selection redraws the whole board: 81 cells, a
 * handful of primitives each.
 *
 * Under each cell goes the theme's texture, copied straight into the
 * canvas buffer before LVGL draws on top: the lines (thin inside a box,
 * thick between boxes), the highlights as translucent fills, Killer's
 * dashed cages with their sums, and the digit - the givens in the theme's
 * own drawn glyphs from the pack, the player's in the system font, the
 * notes small in a 3x3 (or 3x2) grid. A hint draws its pattern on top, and
 * a chain its links: solid where one of the two must be true, dashed where
 * they cannot both be.
 *
 * The theme preview draws a fixed demo position through the same code.
 */
#include "sd_app.h"
#include "sd_pak.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sd_board_t sd_B;

/* what the board shows: the game, or the preview's demo */
typedef struct {
    const sd_shape_t *s;
    const uint8_t    *given, *val, *sol;
    const uint16_t   *notes;
    int               sel, digit;
} view_t;

static view_t V;

/* The demo for the preview: a real puzzle, part played, with notes. */
static const uint8_t DEMO_GIVEN[81] = {
    5, 3, 0, 0, 7, 0, 0, 0, 0, 6, 0, 0, 1, 9, 5, 0, 0, 0, 0, 9, 8, 0, 0, 0, 0, 6, 0,
    8, 0, 0, 0, 6, 0, 0, 0, 3, 4, 0, 0, 8, 0, 3, 0, 0, 1, 7, 0, 0, 0, 2, 0, 0, 0, 6,
    0, 6, 0, 0, 0, 0, 2, 8, 0, 0, 0, 0, 4, 1, 9, 0, 0, 5, 0, 0, 0, 0, 8, 0, 0, 7, 9,
};
static const uint8_t DEMO_SOL[81] = {
    5, 3, 4, 6, 7, 8, 9, 1, 2, 6, 7, 2, 1, 9, 5, 3, 4, 8, 1, 9, 8, 3, 4, 2, 5, 6, 7,
    8, 5, 9, 7, 6, 1, 4, 2, 3, 4, 2, 6, 8, 5, 3, 7, 9, 1, 7, 1, 3, 9, 2, 4, 8, 5, 6,
    9, 6, 1, 5, 3, 7, 2, 8, 4, 2, 8, 7, 4, 1, 9, 6, 3, 5, 3, 4, 5, 2, 8, 6, 1, 7, 9,
};
static uint8_t  demo_val[81];
static uint16_t demo_notes[81];
static sd_shape_t demo_shape;

static void demo_setup(void)
{
    sd_shape_init(&demo_shape, 9, false);
    memcpy(demo_val, DEMO_GIVEN, 81);
    static const uint8_t played[] = { 2, 10, 11, 18, 21, 28, 29, 37, 47, 48, 57, 58 };
    for (size_t i = 0; i < sizeof played; i++) demo_val[played[i]] = DEMO_SOL[played[i]];
    memset(demo_notes, 0, sizeof demo_notes);
    static const struct { uint8_t c; uint16_t m; } notes[] = {
        { 3, (1 << 2) | (1 << 6) }, { 5, (1 << 2) | (1 << 8) }, { 22, (1 << 2) | (1 << 4) },
        { 30, (1 << 5) | (1 << 7) }, { 33, (1 << 4) | (1 << 9) }, { 40, (1 << 5) },
        { 41, (1 << 5) | (1 << 7) }, { 66, (1 << 2) | (1 << 3) | (1 << 7) }, { 70, (1 << 3) | (1 << 6) },
    };
    for (size_t i = 0; i < sizeof notes / sizeof notes[0]; i++) demo_notes[notes[i].c] = notes[i].m;
}

static void view_load(void)
{
    if (sd_B.demo) {
        V.s = &demo_shape;
        V.given = DEMO_GIVEN;
        V.val = demo_val;
        V.sol = DEMO_SOL;
        V.notes = demo_notes;
        V.sel = 40;
        V.digit = 5;
    } else {
        V.s = &sd_shape;
        V.given = sd_G.puz.given;
        V.val = sd_G.val;
        V.sol = sd_G.puz.sol;
        V.notes = sd_G.notes;
        V.sel = sd_G.sel;
        V.digit = sd_G.digit;
    }
}

/* ---- primitives -------------------------------------------------------------- */

static void fill(lv_layer_t *l, int x1, int y1, int x2, int y2, uint32_t c, lv_opa_t opa, int r)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(c);
    d.bg_opa = opa;
    d.radius = r;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(l, &d, &a);
}

static void line(lv_layer_t *l, int x1, int y1, int x2, int y2, int w, uint32_t c, lv_opa_t opa, int dash)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.width = w;
    d.color = lv_color_hex(c);
    d.opa = opa;
    if (dash) {
        d.dash_width = dash;
        d.dash_gap = dash;
    }
    lv_draw_line(l, &d);
}

static void text(lv_layer_t *l, int x1, int y1, int x2, int y2, const char *t, const lv_font_t *f, uint32_t c,
                 bool local)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = t;
    d.text_local = local;
    d.font = f;
    d.color = lv_color_hex(c);
    d.align = LV_TEXT_ALIGN_CENTER;
    int lh = lv_font_get_line_height(f);
    lv_area_t a = { x1, (y1 + y2 + 1 - lh) / 2, x2, (y1 + y2 + 1 - lh) / 2 + lh };
    lv_draw_label(l, &d, &a);
}

static const char *const DIGIT[10] = { "", "1", "2", "3", "4", "5", "6", "7", "8", "9" };

static const lv_font_t *digit_font(int cell)
{
    if (cell >= 96) return &aos_inter_64;
    return &aos_inter_48;
}

static const lv_font_t *note_font(int cell)
{
    if (cell >= 96) return &aos_inter_28;
    if (cell >= 70) return &aos_inter_20;
    return &aos_inter_16;
}

/* ---- geometry ------------------------------------------------------------------- */

static int cage(int c)
{
    return V.s->cage_of[c];
}

static bool same_cage(int a, int b)
{
    return cage(a) != 0xFF && cage(a) == cage(b);
}

/* where the note of digit d sits in a cell */
static void note_pos(int c, int d, int *cx, int *cy)
{
    int n = V.s->n, cell = sd_B.cell;
    int cols = 3, rows = n == 9 ? 3 : 2;
    int top = V.s->ncages ? cell / 6 : 0;
    int k = d - 1;
    int sw = (cell - 4) / cols, sh = (cell - 4 - top) / rows;
    *cx = (c % n) * cell + 2 + (k % cols) * sw + sw / 2;
    *cy = (c / n) * cell + 2 + top + (k / cols) * sh + sh / 2;
}

static bool in_hint(int c)
{
    const sd_step_t *h = sd_B.hint;
    if (!h) return false;
    for (int k = 0; k < h->ncells; k++)
        if (h->cells[k] == c) return true;
    return false;
}

static uint16_t hint_elims(int c)
{
    const sd_step_t *h = sd_B.hint;
    uint16_t m = 0;
    if (!h) return 0;
    for (int k = 0; k < h->nelim; k++)
        if (h->elim_cell[k] == c) m |= (uint16_t)(1u << h->elim_digit[k]);
    return m;
}

/* ---- a cell ---------------------------------------------------------------------- */

static void copy_texture(int c)
{
    int n = V.s->n, cell = sd_B.cell, px = sd_B.stride;
    int x0 = (c % n) * cell, y0 = (c / n) * cell;
    const sd_theme_t *t = sd_theme();
    uint16_t *dst = sd_B.buf;
    if (sd_A.tex && sd_A.theme == (int)(t - SD_THEME)) {
        int off = (SD_TEX - sd_B.px) / 2;
        if (off < 0) off = 0;
        for (int y = 0; y < cell; y++) {
            int sy = off + y0 + y;
            if (sy >= SD_TEX) sy = SD_TEX - 1;
            memcpy(dst + (size_t)(y0 + y) * px + x0, sd_A.tex + (size_t)sy * SD_TEX + off + x0, (size_t)cell * 2);
        }
    } else {
        lv_color_t lc = lv_color_hex(t->board);
        uint16_t v = (uint16_t)(((lc.red >> 3) << 11) | ((lc.green >> 2) << 5) | (lc.blue >> 3));
        for (int y = 0; y < cell; y++) {
            uint16_t *row = dst + (size_t)(y0 + y) * px + x0;
            for (int x = 0; x < cell; x++) row[x] = v;
        }
    }
}

static void draw_glyph(lv_layer_t *l, int x0, int y0, int cell, int d)
{
    const sd_theme_t *t = sd_theme();
    bool six = V.s->n == 6;
    const uint8_t *sheet = six ? sd_A.g6 : sd_A.g9;
    int gw = six ? sd_A.g6w : sd_A.g9w;
    if (sheet && gw > 0 && gw <= cell && sd_A.theme == (int)(t - SD_THEME)) {
        static lv_image_dsc_t dsc[2][10];
        lv_image_dsc_t *g = &dsc[six][d];
        memset(g, 0, sizeof *g);
        g->header.magic = LV_IMAGE_HEADER_MAGIC;
        g->header.cf = LV_COLOR_FORMAT_RGB565A8;
        g->header.w = (uint32_t)gw;
        g->header.h = (uint32_t)gw;
        g->header.stride = (uint32_t)gw * 2;
        g->data_size = (uint32_t)(gw * gw * 3);
        g->data = sheet + (size_t)(d - 1) * g->data_size;
        lv_draw_image_dsc_t id;
        lv_draw_image_dsc_init(&id);
        id.src = g;
        int ox = x0 + (cell - gw) / 2, oy = y0 + (cell - gw) / 2;
        lv_area_t a = { ox, oy, ox + gw - 1, oy + gw - 1 };
        lv_draw_image(l, &id, &a);
        return;
    }
    text(l, x0, y0, x0 + cell - 1, y0 + cell - 1, DIGIT[d], digit_font(cell), t->given, false);
}

static void draw_cell(lv_layer_t *l, int c)
{
    const sd_shape_t *s = V.s;
    const sd_theme_t *t = sd_theme();
    int n = s->n, cell = sd_B.cell;
    int r = c / n, col = c % n;
    int x0 = col * cell, y0 = r * cell, x1 = x0 + cell - 1, y1 = y0 + cell - 1;
    int v = V.val[c];
    bool given = V.given[c] != 0;
    bool wrong = !sd_B.demo && v && !given && v != V.sol[c];

    /* highlights */
    if (s->diag && (r == col || r + col == n - 1)) fill(l, x0, y0, x1, y1, t->peer, 70, 0);
    int sel = V.sel;
    if (sel >= 0 && sel < s->ncells && !sd_B.hint) {
        bool peer = sd_D.hl_peers && c != sel &&
                    (c / n == sel / n || c % n == sel % n || s->cell_unit[c][2] == s->cell_unit[sel][2]);
        if (peer) fill(l, x0, y0, x1, y1, t->peer, t->peer_opa, 0);
    }
    if (sd_D.hl_same && V.digit && v == V.digit && !sd_B.hint) fill(l, x0, y0, x1, y1, t->same, t->same_opa, 0);
    if (c == sel && !sd_B.hint) fill(l, x0 + 2, y0 + 2, x1 - 2, y1 - 2, t->sel, 170, cell / 10);
    if (wrong && sd_D.mark_errors) fill(l, x0, y0, x1, y1, t->err, 60, 0);
    if (sd_B.hint) {
        const sd_step_t *h = sd_B.hint;
        if (h->cell == c) fill(l, x0 + 2, y0 + 2, x1 - 2, y1 - 2, t->hint, 190, cell / 10);
        else if (in_hint(c)) fill(l, x0, y0, x1, y1, t->hint, 100, 0);
        else if (hint_elims(c)) fill(l, x0, y0, x1, y1, t->err, 55, 0);
        else if (h->unit != 0xFF && h->unit < s->nunits) {
            for (int k = 0; k < s->unit_len[h->unit]; k++)
                if (s->unit_cell[h->unit][k] == c) fill(l, x0, y0, x1, y1, t->hint, 45, 0);
        }
    }

    /* the lines: each cell draws its own half of every edge */
    int thin = cell >= 96 ? 2 : 1, thick = cell >= 96 ? 3 : 2, outer = 3;
    bool bl = col % s->bw == 0, bt = r % s->bh == 0, br = (col + 1) % s->bw == 0, bb = (r + 1) % s->bh == 0;
    int wl = col == 0 ? outer * 2 : bl ? thick : thin;
    int wt = r == 0 ? outer * 2 : bt ? thick : thin;
    int wr = col == n - 1 ? outer * 2 : br ? thick : thin;
    int wb = r == n - 1 ? outer * 2 : bb ? thick : thin;
    fill(l, x0, y0, x0 + wl - 1, y1, bl ? t->box : t->line, bl ? 255 : t->line_opa, 0);
    fill(l, x1 - wr + 1, y0, x1, y1, br ? t->box : t->line, br ? 255 : t->line_opa, 0);
    fill(l, x0, y0, x1, y0 + wt - 1, bt ? t->box : t->line, bt ? 255 : t->line_opa, 0);
    fill(l, x0, y1 - wb + 1, x1, y1, bb ? t->box : t->line, bb ? 255 : t->line_opa, 0);

    /* Killer: the cage's edge, dashed, a little inside */
    if (s->ncages && cage(c) != 0xFF) {
        int in = cell >= 96 ? 7 : 5, dw = cell >= 96 ? 6 : 4;
        bool up = r == 0 || !same_cage(c, c - n), dn = r == n - 1 || !same_cage(c, c + n);
        bool lf = col == 0 || !same_cage(c, c - 1), rt = col == n - 1 || !same_cage(c, c + 1);
        int ax = lf ? x0 + in : x0, bx = rt ? x1 - in : x1 + 1;
        int ay = up ? y0 + in : y0, by = dn ? y1 - in : y1 + 1;
        if (up) line(l, ax, y0 + in, bx, y0 + in, 2, t->cage, 230, dw);
        if (dn) line(l, ax, y1 - in, bx, y1 - in, 2, t->cage, 230, dw);
        if (lf) line(l, x0 + in, ay, x0 + in, by, 2, t->cage, 230, dw);
        if (rt) line(l, x1 - in, ay, x1 - in, by, 2, t->cage, 230, dw);
        /* the sum, on the cage's first cell */
        int g = cage(c);
        bool first = true;
        for (int k = 0; k < c; k++)
            if (cage(k) == g) {
                first = false;
                break;
            }
        if (first) {
            char buf[8];
            snprintf(buf, sizeof buf, "%d", s->cage_sum[g]);
            const lv_font_t *f = cell >= 96 ? &aos_inter_20 : &aos_inter_14;
            int tw = (int)strlen(buf) * (cell >= 96 ? 12 : 9) + 4, th = lv_font_get_line_height(f);
            fill(l, x0 + in - 2, y0 + in - 2, x0 + in + tw, y0 + in + th - 4, t->board, 230, 3);
            lv_draw_label_dsc_t d;
            lv_draw_label_dsc_init(&d);
            d.text = buf;
            d.text_local = 1;
            d.font = f;
            d.color = lv_color_hex(t->cage);
            lv_area_t a = { x0 + in, y0 + in - 3, x0 + in + tw + 20, y0 + in + th };
            lv_draw_label(l, &d, &a);
        }
    }

    /* the digit, or the notes */
    if (v) {
        if (given) draw_glyph(l, x0, y0, cell, v);
        else text(l, x0, y0, x1, y1, DIGIT[v], digit_font(cell), wrong && sd_D.mark_errors ? t->err : t->user, false);
    } else {
        uint16_t m = V.notes[c], el = hint_elims(c);
        if (sd_B.hint && el) m |= el;
        const lv_font_t *f = note_font(cell);
        int half = cell / 7;
        for (int d = 1; d <= n; d++) {
            if (!(m & (1u << d))) continue;
            int cx, cy;
            note_pos(c, d, &cx, &cy);
            uint32_t col_n = t->note;
            if (el & (1u << d)) {
                col_n = t->err;
            } else if (sd_D.hl_same && V.digit == d && !sd_B.hint) {
                fill(l, cx - half, cy - half, cx + half, cy + half, t->same, 220, LV_RADIUS_CIRCLE);
                col_n = t->user;
            }
            text(l, cx - half - 4, cy - half, cx + half + 4, cy + half, DIGIT[d], f, col_n, false);
            if (el & (1u << d)) line(l, cx - half, cy + half, cx + half, cy - half, 2, t->err, 255, 0);
        }
    }

    if (sd_B.cursor && c == V.sel && !sd_B.demo) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = lv_color_hex(0xFFD60A);
        d.border_opa = LV_OPA_COVER;
        d.border_width = 4;
        d.radius = cell / 8;
        lv_area_t a = { x0 + 1, y0 + 1, x1 - 1, y1 - 1 };
        lv_draw_rect(l, &d, &a);
    }
}

/* the chain of a hint, over everything */
static void draw_chain(lv_layer_t *l)
{
    const sd_step_t *h = sd_B.hint;
    if (!h || h->nchain < 2) return;
    const sd_theme_t *t = sd_theme();
    for (int k = 0; k + 1 < h->nchain; k++) {
        int ax, ay, bx, by;
        note_pos(h->chain[k] >> 4, h->chain[k] & 15, &ax, &ay);
        note_pos(h->chain[k + 1] >> 4, h->chain[k + 1] & 15, &bx, &by);
        /* links alternate: strong (one is true) from an "off" node, weak after */
        bool strong = (k % 2) == 0;
        line(l, ax, ay, bx, by, 3, strong ? t->hint : t->err, 230, strong ? 0 : 6);
    }
    for (int k = 0; k < h->nchain; k++) {
        int cx, cy;
        note_pos(h->chain[k] >> 4, h->chain[k] & 15, &cx, &cy);
        int r = sd_B.cell / 8;
        fill(l, cx - r, cy - r, cx + r, cy + r, k % 2 ? t->hint : t->board, 255, LV_RADIUS_CIRCLE);
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = lv_color_hex(t->hint);
        d.border_width = 2;
        d.border_opa = 255;
        d.radius = LV_RADIUS_CIRCLE;
        lv_area_t a = { cx - r, cy - r, cx + r, cy + r };
        lv_draw_rect(l, &d, &a);
        text(l, cx - r, cy - r, cx + r, cy + r, DIGIT[h->chain[k] & 15], &aos_inter_16, k % 2 ? t->board : t->user,
             false);
    }
}

/* ---- the canvas ------------------------------------------------------------------- */

void sd_board_all(void)
{
    memset(sd_B.dmask, 1, sizeof sd_B.dmask);
    sd_B.dirty = true;
}

void sd_board_cell(int c)
{
    if (c >= 0 && c < SD_MAXC) {
        sd_B.dmask[c] = 1;
        sd_B.dirty = true;
    }
}

void sd_board_present(void)
{
    if (!sd_B.dirty || !sd_B.canvas || !sd_B.buf) return;
    sd_B.dirty = false;
    view_load();
    int nc = V.s->ncells;
    if (sd_B.hint && sd_B.hint->nchain) memset(sd_B.dmask, 1, sizeof sd_B.dmask);
    for (int c = 0; c < nc; c++)
        if (sd_B.dmask[c]) copy_texture(c);
    lv_layer_t layer;
    lv_canvas_init_layer(sd_B.canvas, &layer);
    for (int c = 0; c < nc; c++)
        if (sd_B.dmask[c]) draw_cell(&layer, c);
    draw_chain(&layer);
    lv_canvas_finish_layer(sd_B.canvas, &layer);
    memset(sd_B.dmask, 0, sizeof sd_B.dmask);

}

static void touch_cb(lv_event_t *e)
{
    if (sd_B.demo || sd_B.paused || !sd_G.active || sd_G.won) return;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t co;
    lv_obj_get_coords(sd_B.canvas, &co);
    int x = p.x - co.x1, y = p.y - co.y1;
    int n = sd_shape.n;
    if (x < 0 || y < 0 || x >= sd_B.px || y >= sd_B.px) return;
    int c = (y / sd_B.cell) * n + x / sd_B.cell;
    (void)e;
    sd_B.cursor = false;
    sd_G.sel = (int8_t)c;
    if (sd_G.val[c]) sd_G.digit = sd_G.val[c];
    sd_board_all();
    sd_board_present();
    sd_ui_refresh_game();
}

void sd_board_create(lv_obj_t *parent, int x, int y, int max_px)
{
    sd_board_destroy();
    if (!demo_shape.n) demo_setup();
    view_load();
    int n = V.s->n;
    sd_B.cell = max_px / n;
    sd_B.px = sd_B.cell * n;
    sd_B.x = x + (max_px - sd_B.px) / 2;
    sd_B.y = y + (max_px - sd_B.px) / 2;
    /* LVGL may want its rows aligned (the board's PPA does): the texture is
     * copied into the buffer by rows, so the stride is LVGL's, not px * 2 */
    sd_B.stride = (int)(lv_draw_buf_width_to_stride((uint32_t)sd_B.px, LV_COLOR_FORMAT_RGB565) / 2);
    sd_B.buf = (uint16_t *)sd_alloc((size_t)sd_B.stride * sd_B.px * 2);
    if (!sd_B.buf) {
        sd_ui_toast(_("Sin memoria"));
        return;
    }
    sd_B.canvas = lv_canvas_create(parent);
    lv_obj_set_style_radius(sd_B.canvas, 0, 0);
    lv_obj_remove_flag(sd_B.canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_set_buffer(sd_B.canvas, sd_B.buf, sd_B.px, sd_B.px, LV_COLOR_FORMAT_RGB565);
    /* the glyphs' descriptors are reused with other data: nothing stale */
    lv_image_cache_drop(NULL);
    lv_obj_set_pos(sd_B.canvas, sd_B.x, sd_B.y);
    lv_obj_set_size(sd_B.canvas, sd_B.px, sd_B.px);

    sd_B.touch = lv_obj_create(parent);
    lv_obj_remove_style_all(sd_B.touch);
    lv_obj_set_pos(sd_B.touch, sd_B.x, sd_B.y);
    lv_obj_set_size(sd_B.touch, sd_B.px, sd_B.px);
    lv_obj_add_flag(sd_B.touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sd_B.touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(sd_B.touch, touch_cb, LV_EVENT_PRESSED, NULL);
    sd_board_all();
    sd_board_present();
}

void sd_board_destroy(void)
{
    /* the objects go with their parent's clean; only the buffer is ours */
    if (sd_B.canvas && lv_obj_is_valid(sd_B.canvas)) lv_obj_delete(sd_B.canvas);
    if (sd_B.touch && lv_obj_is_valid(sd_B.touch)) lv_obj_delete(sd_B.touch);
    sd_free(sd_B.buf);
    bool demo = sd_B.demo;
    memset(&sd_B, 0, sizeof sd_B);
    sd_B.demo = demo;
}

void sd_board_demo(bool on)
{
    sd_B.demo = on;
    if (!demo_shape.n) demo_setup();
}
