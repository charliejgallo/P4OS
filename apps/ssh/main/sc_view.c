/*
 * P4OS - SSH client: the terminal on the screen.
 *
 * A plain LVGL object that draws the terminal's cells itself, in its draw
 * event: per row, the runs of one background as rectangles and the runs of
 * one colour as labels (text_local, since the draw threads read the text
 * later: HANDOFF-APPS, "Trampas"). Only the rows the terminal marks as
 * changed are invalidated, from the app's timer.
 *
 * aos_mono_18 (JetBrains Mono) has ASCII, Latin-1, the box drawing and the
 * block characters; anything else is drawn as '?' so the columns stay put.
 * Bold is the bright colour, as most terminals do, the font having no bold.
 *
 * A finger dragged up and down goes through the scrollback; typing (the
 * app's job) comes back to the live screen. A tap is the app's.
 */
#include "sc.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_mono.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FG_DEFAULT 0xD8DEE9
#define BG_DEFAULT 0x0D1117

struct sc_view {
    lv_obj_t *obj;
    sc_term_t *term;
    const lv_font_t *font;
    int32_t cw, lh;
    int cols, rows;
    int back;                   /* lines up into the scrollback */
    int32_t drag_acc, drag_total;
    bool dragged;
    int cur_x, cur_y;
    void (*on_tap)(void *);
    void *tap_user;
};

/* ---- colours ---- */

static const uint32_t BASE16[16] = {
    0x1D1F21, 0xCC6666, 0x8ABE6E, 0xE6C15A, 0x5F8FD7, 0xB294BB, 0x5FB3B3, 0xC5C8C6,
    0x666666, 0xFF7B72, 0xA5D6A7, 0xFFE082, 0x82AAFF, 0xD7A9E3, 0x89DDFF, 0xFFFFFF,
};

static uint32_t xterm_rgb(int i)
{
    if (i < 16) return BASE16[i];
    if (i < 232) {
        i -= 16;
        static const uint8_t L[6] = { 0, 95, 135, 175, 215, 255 };
        return (uint32_t)L[i / 36] << 16 | (uint32_t)L[i / 6 % 6] << 8 | L[i % 6];
    }
    uint32_t g = (uint32_t)(8 + (i - 232) * 10);
    return g << 16 | g << 8 | g;
}

static uint32_t mix(uint32_t a, uint32_t b)
{
    return ((a >> 1) & 0x7F7F7F) + ((b >> 1) & 0x7F7F7F);
}

/* the colours a cell is drawn with */
static void colours(const sc_cell_t *c, uint32_t *fg, uint32_t *bg)
{
    uint16_t f = c->fg;
    if ((c->attr & SC_A_BOLD) && f < 8) f = (uint16_t)(f + 8);
    uint32_t F = f == SC_DEF ? FG_DEFAULT : xterm_rgb(f);
    uint32_t B = c->bg == SC_DEF ? BG_DEFAULT : xterm_rgb(c->bg);
    if (c->attr & SC_A_DIM) F = mix(F, B);
    if (c->attr & SC_A_REV) {
        uint32_t t = F;
        F = B;
        B = t;
    }
    if (c->attr & SC_A_INVIS) F = B;
    *fg = F;
    *bg = B;
}

/* what aos_mono_18 has a glyph for */
static bool glyph_ok(uint32_t c)
{
    return (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xFF) || (c >= 0x2022 && c <= 0x2026) ||
           (c >= 0x2500 && c <= 0x259F);
}

static size_t put_utf8(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F));
    return 3;
}

/* ---- drawing ---- */

static void draw_rect(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t rgb, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(rgb);
    d.bg_opa = opa;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

static void draw_text(sc_view_t *v, lv_layer_t *layer, int32_t x, int32_t y, const char *txt, uint32_t rgb)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = v->font;
    d.color = lv_color_hex(rgb);
    d.text = txt;
    d.text_local = 1;
    d.flag = LV_TEXT_FLAG_EXPAND;
    lv_area_t a = { x, y, x + 4096, y + v->lh - 1 };
    lv_draw_label(layer, &d, &a);
}

static void draw_row(sc_view_t *v, lv_layer_t *layer, const sc_cell_t *r, int y, int cols, int32_t ox, int32_t oy,
                     int curx)
{
    int32_t py = oy + y * v->lh;
    /* backgrounds, in runs */
    for (int x = 0; x < cols;) {
        uint32_t fg, bg;
        colours(&r[x], &fg, &bg);
        int e = x + 1;
        for (; e < cols; e++) {
            uint32_t f2, b2;
            colours(&r[e], &f2, &b2);
            if (b2 != bg) break;
        }
        if (bg != BG_DEFAULT) draw_rect(layer, ox + x * v->cw, py, ox + e * v->cw - 1, py + v->lh - 1, bg, LV_OPA_COVER);
        x = e;
    }
    /* the cursor's block, under its character */
    if (curx >= 0 && curx < cols) {
        int32_t cx = ox + curx * v->cw;
        draw_rect(layer, cx, py, cx + v->cw - 1, py + v->lh - 1, FG_DEFAULT, LV_OPA_80);
    }
    /* text, in runs of one colour; blanks end a run, so each label starts
     * on its own column */
    char buf[SC_COLS_MAX * 3 + 1];
    for (int x = 0; x < cols;) {
        uint32_t c = r[x].ch;
        if (!c || c == ' ') {
            x++;
            continue;
        }
        uint32_t fg, bg;
        colours(&r[x], &fg, &bg);
        if (x == curx) fg = BG_DEFAULT;
        size_t n = 0;
        int e = x;
        for (; e < cols; e++) {
            uint32_t ce = r[e].ch;
            if (!ce || ce == ' ') break;
            uint32_t f2, b2;
            colours(&r[e], &f2, &b2);
            if (e == curx) f2 = BG_DEFAULT;
            if (f2 != fg || (r[e].attr & SC_A_UNDER) != (r[x].attr & SC_A_UNDER)) break;
            n += put_utf8(glyph_ok(ce) ? ce : '?', buf + n);
        }
        buf[n] = 0;
        draw_text(v, layer, ox + x * v->cw, py, buf, fg);
        if (r[x].attr & SC_A_UNDER)
            draw_rect(layer, ox + x * v->cw, py + v->lh - 2, ox + e * v->cw - 1, py + v->lh - 2, fg, LV_OPA_COVER);
        x = e;
    }
}

static void draw_cb(lv_event_t *e)
{
    sc_view_t *v = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t co;
    lv_obj_get_coords(v->obj, &co);
    const lv_area_t *clip = &layer->_clip_area;
    sc_term_lock(v->term);
    int cx, cy, tc, tr;
    bool cur = sc_term_cursor(v->term, &cx, &cy) && v->back == 0;
    sc_term_size(v->term, &tc, &tr);
    int cols = v->cols < tc ? v->cols : tc, rows = v->rows < tr ? v->rows : tr;
    for (int y = 0; y < rows; y++) {
        int32_t py = co.y1 + y * v->lh;
        if (py + v->lh <= clip->y1 || py > clip->y2) continue;
        const sc_cell_t *r = sc_term_row(v->term, y, v->back);
        if (r) draw_row(v, layer, r, y, cols, co.x1, co.y1, cur && y == cy ? cx : -1);
    }
    sc_term_unlock(v->term);
}

static void invalidate_row(sc_view_t *v, int y)
{
    if (y < 0 || y >= v->rows) return;
    lv_area_t co;
    lv_obj_get_coords(v->obj, &co);
    lv_area_t a = { co.x1, co.y1 + y * v->lh, co.x2, co.y1 + (y + 1) * v->lh - 1 };
    lv_obj_invalidate_area(v->obj, &a);
}

void sc_view_refresh(sc_view_t *v)
{
    uint32_t d[(SC_ROWS_MAX + 31) / 32];
    sc_term_lock(v->term);
    bool any = sc_term_take_dirty(v->term, d);
    int cx, cy;
    sc_term_cursor(v->term, &cx, &cy);
    sc_term_unlock(v->term);
    if (v->back) {
        if (any) lv_obj_invalidate(v->obj);     /* what is above moves under the finger */
        return;
    }
    if (cy != v->cur_y || cx != v->cur_x) {
        invalidate_row(v, v->cur_y);
        invalidate_row(v, cy);
        v->cur_x = cx;
        v->cur_y = cy;
    }
    if (!any) return;
    for (int y = 0; y < v->rows; y++)
        if (d[y >> 5] & (1u << (y & 31))) invalidate_row(v, y);
}

void sc_view_to_bottom(sc_view_t *v)
{
    if (!v->back) return;
    v->back = 0;
    lv_obj_invalidate(v->obj);
}

/* ---- the finger ---- */

static void press_cb(lv_event_t *e)
{
    sc_view_t *v = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        v->drag_acc = v->drag_total = 0;
        v->dragged = false;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        lv_point_t vec;
        lv_indev_get_vect(lv_indev_active(), &vec);
        v->drag_acc += vec.y;
        v->drag_total += vec.y < 0 ? -vec.y : vec.y;
        if (v->drag_total > 12) v->dragged = true;
        int lines = v->drag_acc / v->lh;
        if (!lines) return;
        v->drag_acc -= lines * v->lh;
        sc_term_lock(v->term);
        int max = sc_term_scrollback(v->term);
        sc_term_unlock(v->term);
        int b = v->back + lines;         /* a finger going down shows what was above */
        if (b < 0) b = 0;
        if (b > max) b = max;
        if (b != v->back) {
            v->back = b;
            lv_obj_invalidate(v->obj);
        }
        return;
    }
    if (code == LV_EVENT_CLICKED && !v->dragged && v->on_tap) v->on_tap(v->tap_user);
}

/* ---- life ---- */

sc_view_t *sc_view_create(lv_obj_t *parent, sc_term_t *term)
{
    sc_view_t *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->term = term;
    v->font = &aos_mono_18;
    v->cw = lv_font_get_glyph_width(v->font, 'M', 0);
    v->lh = lv_font_get_line_height(v->font);
    if (v->cw <= 0) v->cw = 11;
    v->cur_x = v->cur_y = -1;
    v->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(v->obj);
    lv_obj_set_style_bg_color(v->obj, lv_color_hex(BG_DEFAULT), 0);
    lv_obj_set_style_bg_opa(v->obj, LV_OPA_COVER, 0);
    lv_obj_add_flag(v->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(v->obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(v->obj, draw_cb, LV_EVENT_DRAW_MAIN, v);
    lv_obj_add_event_cb(v->obj, press_cb, LV_EVENT_PRESSED, v);
    lv_obj_add_event_cb(v->obj, press_cb, LV_EVENT_PRESSING, v);
    lv_obj_add_event_cb(v->obj, press_cb, LV_EVENT_CLICKED, v);
    return v;
}

void sc_view_on_tap(sc_view_t *v, void (*cb)(void *), void *user)
{
    v->on_tap = cb;
    v->tap_user = user;
}

void sc_view_delete(sc_view_t *v)
{
    if (!v) return;
    if (v->obj) lv_obj_delete(v->obj);
    free(v);
}

lv_obj_t *sc_view_obj(sc_view_t *v) { return v->obj; }

void sc_view_set_size(sc_view_t *v, int32_t w, int32_t h, int *cols, int *rows)
{
    lv_obj_set_size(v->obj, w, h);
    int c = (int)(w / v->cw), r = (int)(h / v->lh);
    if (c > SC_COLS_MAX) c = SC_COLS_MAX;
    if (r > SC_ROWS_MAX) r = SC_ROWS_MAX;
    if (c < 10) c = 10;
    if (r < 3) r = 3;
    v->cols = c;
    v->rows = r;
    v->back = 0;
    *cols = c;
    *rows = r;
    lv_obj_invalidate(v->obj);
}
