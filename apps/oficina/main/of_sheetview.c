/*
 * P4OS - Office: the spreadsheet screen.
 *
 *   - a bar: back, the file's name, undo, redo, save, the menu;
 *   - the formula bar: the cell's name (tap: go to a cell) and what it holds
 *     (tap: edit it; while a formula is being typed, tapping a cell or
 *     dragging over a range types its reference);
 *   - the grid, drawn by the app: the column letters and row numbers, the
 *     panes the file keeps frozen, merged cells, fills, colours and bold of
 *     the file, text that runs over empty neighbours, "###" for a number
 *     that does not fit. One finger scrolls (with momentum), a tap selects,
 *     a double tap edits, a long press and a drag selects a range, two
 *     fingers zoom, and dragging the edge of a column's letter resizes it;
 *   - the sheets as tabs, and the sum, average and count of the selection.
 *
 * A USB or Bluetooth keyboard works as in a desktop spreadsheet: arrows
 * (with Shift to select), Enter and Tab move after typing, typing replaces
 * the cell, F2 edits it, Delete clears, Ctrl+Z / Y / C / X / V / S / F.
 */
#include "of_ui.h"
#include "of_pdf.h"
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NZOOM 6
static const lv_font_t *const ZF[NZOOM] = { &aos_inter_16, &aos_inter_20, &aos_inter_24, &aos_inter_28, &aos_inter_32, &aos_inter_36 };
static const int ZP[NZOOM] = { 16, 20, 24, 28, 32, 36 };

#define UNDO_MAX 200

typedef struct {
    int      sheet;
    int32_t  r, c;
    char    *before, *after;        /* sources; NULL = empty */
    uint32_t style_before, style_after;
} uchange_t;

typedef struct {
    uchange_t *ch;
    int        n;
} uentry_t;

static struct {
    of_book_t *b;
    char       path[300];
    int        si;              /* the sheet shown */
    lv_obj_t  *root, *top, *title, *fbar, *fref, *ftext, *ta, *grid, *tabs, *status, *kb;
    lv_obj_t  *b_undo, *b_redo, *b_save;
    int32_t    W, H, gx, gy, gw, gh;
    int        z;
    /* geometry caches, at the current zoom */
    int32_t   *cx, ncx;         /* cx[i] = x of column i, i <= ncx */
    int32_t   *ry, nry;
    int32_t    defw, defh, hw, hh;
    int32_t    sx, sy;          /* scroll of the moving pane */
    /* selection */
    int32_t    ar, ac, cr, cc;
    bool       editing, ref_mode;
    int        ref_at;          /* where in the text the inserted reference starts */
    /* gestures */
    int        drag_mode;       /* 0 scroll, 1 select, 2 resize column, 3 reference */
    int32_t    resize_col, resize_w0;
    float      resize_x0;
    float      vx, vy;
    lv_timer_t *fling;
    float      pinch_acc;
    bool       modified;
    /* undo */
    uentry_t   undo[UNDO_MAX], redo[UNDO_MAX];
    int        nundo, nredo;
    uentry_t   cur;             /* being built */
    /* the internal clipboard: where it was copied from */
    char      *clip;
    int32_t    clip_r, clip_c;
    int        clip_sheet;
    /* filter */
    bool       filtered;
} V;

static void relayout(void);
static void refresh(void);
static void sel_changed(void);
static void edit_begin(const char *initial, bool replace);
static void edit_commit(int move_r, int move_c);
static void edit_cancel(void);

static of_sheet_t *SH(void) { return &V.b->sh[V.si]; }

static int32_t text_w(const char *s, const lv_font_t *f)
{
    int32_t w = 0;
    while (*s) {
        int l;
        uint32_t cp = of_utf8_get(s, &l);
        int ln;
        uint32_t next = s[l] ? of_utf8_get(s + l, &ln) : 0;
        w += lv_font_get_glyph_width(f, cp, next);
        s += l;
    }
    return w;
}

/* -------------------------------------------------------------------------- */
/* Geometry                                                                    */
/* -------------------------------------------------------------------------- */

static int32_t col_px(int c)
{
    float w = of_sheet_colw(SH(), c);
    if (w < 0.1f) return 0;
    return (int32_t)(w * ZP[V.z] * 0.52f) + ZP[V.z] / 3;
}

static int32_t row_px(int32_t r)
{
    of_row_t *row = of_row_get(SH(), r);
    if (row && row->hidden) return 0;
    if (row && row->height > 0) {
        if (row->height < 0.1f) return 0;
        int32_t h = (int32_t)(row->height * V.defh / (SH()->def_rowh > 0 ? SH()->def_rowh : 15.0f));
        return h < V.defh / 2 ? V.defh / 2 : h;
    }
    return V.defh;
}

static void geometry(void)
{
    of_sheet_t *s = SH();
    int32_t mr, mc;
    of_sheet_extent(s, &mr, &mc);
    V.defh = ZP[V.z] * 155 / 100;
    V.defw = (int32_t)((s->def_colw > 0 ? s->def_colw : 10.7f) * ZP[V.z] * 0.52f) + ZP[V.z] / 3;
    int ncx = (mc > 0 ? mc : 0) + 64;
    if (ncx < s->ncolw + 8) ncx = s->ncolw + 8;
    if (ncx > 16384) ncx = 16384;
    of_free(V.cx);
    V.cx = of_alloc((size_t)(ncx + 1) * sizeof(int32_t));
    V.ncx = ncx;
    for (int i = 0; i < ncx; i++) V.cx[i + 1] = V.cx[i] + col_px(i);
    int nry = (mr > 0 ? mr : 0) + 200;
    if (nry > 1048576) nry = 1048576;
    of_free(V.ry);
    V.ry = of_alloc((size_t)(nry + 1) * sizeof(int32_t));
    V.nry = nry;
    for (int32_t i = 0; i < nry; i++) V.ry[i + 1] = V.ry[i] + row_px(i);
    /* the row numbers' column fits the biggest one in sight */
    int digits = 1;
    for (int32_t n = nry; n >= 10; n /= 10) digits++;
    if (digits < 3) digits = 3;
    V.hw = ZP[V.z] * (digits * 6 + 8) / 10;
    V.hh = V.defh;
}

static int32_t colx(int32_t c)
{
    if (c <= V.ncx) return V.cx[c];
    return V.cx[V.ncx] + (c - V.ncx) * V.defw;
}

static int32_t rowy(int32_t r)
{
    if (r <= V.nry) return V.ry[r];
    return V.ry[V.nry] + (r - V.nry) * V.defh;
}

static int32_t col_at_x(int32_t x)        /* x in sheet coordinates */
{
    if (x < 0) return 0;
    if (x >= V.cx[V.ncx]) return V.ncx + (x - V.cx[V.ncx]) / (V.defw ? V.defw : 1);
    int lo = 0, hi = V.ncx - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (V.cx[mid] <= x) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

static int32_t row_at_y(int32_t y)
{
    if (y < 0) return 0;
    if (y >= V.ry[V.nry]) {
        int32_t r = V.nry + (y - V.ry[V.nry]) / (V.defh ? V.defh : 1);
        return r > 1048575 ? 1048575 : r;
    }
    int32_t lo = 0, hi = V.nry - 1;
    while (lo < hi) {
        int32_t mid = (lo + hi + 1) / 2;
        if (V.ry[mid] <= y) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

static int32_t fr(void) { return SH()->freeze_r; }
static int32_t fc(void) { return SH()->freeze_c; }

/* screen position of a column's left edge, a row's top */
static int32_t scr_x(int32_t c)
{
    int32_t x = colx(c);
    if (c >= fc()) x -= V.sx;
    return V.gx + V.hw + x;
}

static int32_t scr_y(int32_t r)
{
    int32_t y = rowy(r);
    if (r >= fr()) y -= V.sy;
    return V.gy + V.hh + y;
}

static void clamp_scroll(void)
{
    int32_t mr, mc;
    of_sheet_extent(SH(), &mr, &mc);
    int32_t maxx = colx((mc > 0 ? mc : 0) + 30) - colx(fc()) - (V.gw - V.hw - colx(fc()));
    int32_t maxy = rowy((mr > 0 ? mr : 0) + 60) - rowy(fr()) - (V.gh - V.hh - rowy(fr()));
    if (maxx < 0) maxx = 0;
    if (maxy < 0) maxy = 0;
    if (V.sx > maxx) V.sx = maxx;
    if (V.sy > maxy) V.sy = maxy;
    if (V.sx < 0) V.sx = 0;
    if (V.sy < 0) V.sy = 0;
}

/* the cell under a point of the screen; false over the headers */
static bool cell_at(float px, float py, int32_t *r, int32_t *c)
{
    int32_t x = (int32_t)px - V.gx - V.hw, y = (int32_t)py - V.gy - V.hh;
    if (x < 0 || y < 0) return false;
    int32_t sx = x < colx(fc()) ? x : x + V.sx;
    int32_t sy = y < rowy(fr()) ? y : y + V.sy;
    *c = col_at_x(sx);
    *r = row_at_y(sy);
    return true;
}

static void scroll_to_cell(int32_t r, int32_t c)
{
    int32_t paneW = V.gw - V.hw - colx(fc()), paneH = V.gh - V.hh - rowy(fr());
    if (c >= fc()) {
        int32_t x0 = colx(c) - colx(fc()), x1 = colx(c + 1) - colx(fc());
        if (x0 < V.sx) V.sx = x0;
        else if (x1 > V.sx + paneW) V.sx = x1 - paneW;
    }
    if (r >= fr()) {
        int32_t y0 = rowy(r) - rowy(fr()), y1 = rowy(r + 1) - rowy(fr());
        if (y0 < V.sy) V.sy = y0;
        else if (y1 > V.sy + paneH) V.sy = y1 - paneH;
    }
    clamp_scroll();
}

static const of_range_t *merge_of(int32_t r, int32_t c)
{
    of_sheet_t *s = SH();
    for (int i = 0; i < s->nmerges; i++) {
        const of_range_t *m = &s->merges[i];
        if ((uint32_t)r >= m->r1 && (uint32_t)r <= m->r2 && (uint32_t)c >= m->c1 && (uint32_t)c <= m->c2) return m;
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Drawing                                                                     */
/* -------------------------------------------------------------------------- */

static void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, lv_opa_t opa)
{
    if (x2 < x1 || y2 < y1) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = opa;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

static void frame(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, int32_t w)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = c;
    d.border_width = w;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

static void label(lv_layer_t *layer, const char *s, const lv_font_t *f, lv_color_t c, lv_area_t *a, lv_text_align_t al)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = c;
    d.text = s;
    d.text_local = 1;
    d.align = al;
    lv_area_t t = *a;
    int32_t lh = f->line_height;
    t.y1 = (a->y1 + a->y2 + 1) / 2 - lh / 2;
    t.y2 = t.y1 + lh;
    lv_draw_label(layer, &d, &t);
}

static bool cell_has_text(int32_t r, int32_t c)
{
    of_cell_t *x = of_cell_get(SH(), r, c);
    return x && (x->v.t != OV_EMPTY);
}

static lv_color_t readable(uint32_t rgb, lv_color_t bg)
{
    /* a file's dark text on the dark theme's dark paper: use the ink */
    lv_color_t c = lv_color_hex(rgb & 0xFFFFFF);
    int lc = (c.red * 3 + c.green * 6 + c.blue) / 10, lb = (bg.red * 3 + bg.green * 6 + bg.blue) / 10;
    if (lc - lb < 60 && lb - lc < 60) return OFT.ink;
    return c;
}

/* The cells of rows [r0, r1] x cols [c0, c1] that fall in 'clip' */
static void draw_cells(lv_layer_t *layer, const lv_area_t *clip, int32_t r0, int32_t r1, int32_t c0, int32_t c1)
{
    lv_area_t keep = layer->_clip_area;
    lv_area_t cl;
    if (!ofu_clip(&cl, &keep, clip)) return;
    layer->_clip_area = cl;
    of_sheet_t *s = SH();
    const lv_font_t *f = ZF[V.z];
    fill(layer, cl.x1, cl.y1, cl.x2, cl.y2, OFT.paper, LV_OPA_COVER);
    /* fills */
    for (int32_t r = r0; r <= r1; r++) {
        of_row_t *row = of_row_get(s, r);
        if (!row || row->hidden) continue;
        int32_t y1 = scr_y(r), y2 = scr_y(r + 1) - 1;
        for (int k = 0; k < row->n; k++) {
            of_cell_t *c = &row->c[k];
            if (c->col < c0 || c->col > c1 || c->style >= (uint32_t)V.b->nxf) continue;
            uint32_t fl = V.b->xf[c->style].fill;
            if (!fl) continue;
            const of_range_t *m = merge_of(r, c->col);
            int32_t x1 = scr_x(c->col), x2 = scr_x(c->col + 1) - 1, yy2 = y2;
            if (m) { x2 = scr_x((int32_t)m->c2 + 1) - 1; yy2 = scr_y((int32_t)m->r2 + 1) - 1; }
            fill(layer, x1, y1, x2, yy2, lv_color_hex(fl & 0xFFFFFF), LV_OPA_COVER);
        }
    }
    /* the grid */
    for (int32_t c = c0; c <= c1 + 1; c++) {
        int32_t x = scr_x(c) - 1;
        fill(layer, x, cl.y1, x, cl.y2, OFT.grid, LV_OPA_COVER);
    }
    for (int32_t r = r0; r <= r1 + 1; r++) {
        int32_t y = scr_y(r) - 1;
        fill(layer, cl.x1, y, cl.x2, y, OFT.grid, LV_OPA_COVER);
    }
    /* merged areas over their inner lines */
    for (int i = 0; i < s->nmerges; i++) {
        const of_range_t *m = &s->merges[i];
        if ((int32_t)m->r2 < r0 || (int32_t)m->r1 > r1 || (int32_t)m->c2 < c0 || (int32_t)m->c1 > c1) continue;
        of_cell_t *tl = of_cell_get(s, (int32_t)m->r1, (int32_t)m->c1);
        uint32_t fl = tl && tl->style < (uint32_t)V.b->nxf ? V.b->xf[tl->style].fill : 0;
        fill(layer, scr_x((int32_t)m->c1), scr_y((int32_t)m->r1), scr_x((int32_t)m->c2 + 1) - 2, scr_y((int32_t)m->r2 + 1) - 2,
             fl ? lv_color_hex(fl & 0xFFFFFF) : OFT.paper, LV_OPA_COVER);
    }
    /* the text */
    int32_t padx = ZP[V.z] / 4;
    for (int32_t r = r0; r <= r1; r++) {
        of_row_t *row = of_row_get(s, r);
        if (!row || row->hidden) continue;
        int32_t y1 = scr_y(r), y2 = scr_y(r + 1) - 2;
        if (y2 <= y1) continue;
        for (int k = 0; k < row->n; k++) {
            of_cell_t *c = &row->c[k];
            if (c->col > c1) break;
            if (c->v.t == OV_EMPTY) continue;
            const of_range_t *m = merge_of(r, c->col);
            if (m && ((int32_t)m->r1 != r || (int32_t)m->c1 != c->col)) continue;
            char buf[256];
            bool right;
            of_cell_display(V.b, c, buf, sizeof buf, &right);
            if (!buf[0]) continue;
            const of_xf_t *xf = c->style < (uint32_t)V.b->nxf ? &V.b->xf[c->style] : NULL;
            int32_t x1 = scr_x(c->col), x2 = scr_x(c->col + 1) - 2, yy2 = y2;
            if (m) { x2 = scr_x((int32_t)m->c2 + 1) - 2; yy2 = scr_y((int32_t)m->r2 + 1) - 2; }
            /* a left-aligned text runs over empty cells to its right; it may
             * start in a column left of the clip */
            int32_t tw = text_w(buf, f);
            int al = xf && xf->halign ? xf->halign : (c->v.t == OV_STR ? 1 : (c->v.t == OV_NUM ? 3 : 2));
            if (c->col < c0) {
                if (al != 1 || x1 + padx + tw < cl.x1) continue;
            }
            lv_area_t a = { x1 + padx, y1, x2 - padx, yy2 };
            if (tw > a.x2 - a.x1) {
                if (c->v.t == OV_NUM && !m) {
                    snprintf(buf, sizeof buf, "###");
                } else if (al == 1 && !m) {
                    int32_t cc = c->col + 1;
                    while (a.x1 + tw > a.x2 && cc < c->col + 40 && !cell_has_text(r, cc)) {
                        a.x2 = scr_x(cc + 1) - 2 - padx;
                        cc++;
                    }
                }
            }
            lv_color_t ink = OFT.ink;
            lv_color_t bg = xf && xf->fill ? lv_color_hex(xf->fill & 0xFFFFFF) : OFT.paper;
            if (xf && xf->color) ink = readable(xf->color, bg);
            else if (xf && xf->fill) ink = (bg.red * 3 + bg.green * 6 + bg.blue) / 10 > 128 ? lv_color_hex(0x1C1C1E) : lv_color_white();
            if (c->v.t == OV_ERR) ink = OFT.danger;
            if (c->v.t == OV_NUM && c->v.n < 0 && xf && V.b->fmt == OF_FMT_XLSX) {
                const char *code = of_numfmt_code(V.b, xf->numfmt);
                if (code && strstr(code, "[Red]")) ink = lv_color_hex(0xE0301E);
            }
            lv_text_align_t ta = al == 3 ? LV_TEXT_ALIGN_RIGHT : al == 2 ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT;
            lv_area_t tclip;
            lv_area_t cell_clip = { a.x1 - padx, y1, a.x2 + padx, yy2 };
            if (!ofu_clip(&tclip, &cl, &cell_clip)) continue;
            layer->_clip_area = tclip;
            label(layer, buf, f, ink, &a, ta);
            if (xf && xf->bold) {
                lv_area_t b2 = a;
                b2.x1++;
                b2.x2++;
                label(layer, buf, f, ink, &b2, ta);
            }
            if (xf && xf->underline) {
                int32_t ux1 = ta == LV_TEXT_ALIGN_RIGHT ? a.x2 - tw : ta == LV_TEXT_ALIGN_CENTER ? (a.x1 + a.x2 - tw) / 2 : a.x1;
                int32_t uy = (a.y1 + a.y2) / 2 + f->line_height / 2 - 2;
                fill(layer, ux1, uy, ux1 + tw, uy + 1, ink, LV_OPA_COVER);
            }
            layer->_clip_area = cl;
        }
    }
    /* the selection */
    int32_t sr1 = LV_MIN(V.ar, V.cr), sr2 = LV_MAX(V.ar, V.cr), sc1 = LV_MIN(V.ac, V.cc), sc2 = LV_MAX(V.ac, V.cc);
    if (sr2 >= r0 && sr1 <= r1 + 1 && sc2 >= c0 && sc1 <= c1 + 1) {
        int32_t x1 = scr_x(sc1), y1 = scr_y(sr1), x2 = scr_x(sc2 + 1) - 1, y2 = scr_y(sr2 + 1) - 1;
        bool range = sr1 != sr2 || sc1 != sc2;
        if (range) {
            fill(layer, x1, y1, x2, y2, OFT.sel, LV_OPA_20);
            frame(layer, x1 - 1, y1 - 1, x2 + 1, y2 + 1, OFT.sel, 3);
        }
        /* the active cell */
        const of_range_t *m = merge_of(V.cr, V.cc);
        int32_t ax1 = scr_x(m ? (int32_t)m->c1 : V.cc), ay1 = scr_y(m ? (int32_t)m->r1 : V.cr);
        int32_t ax2 = scr_x((m ? (int32_t)m->c2 : V.cc) + 1) - 1, ay2 = scr_y((m ? (int32_t)m->r2 : V.cr) + 1) - 1;
        if (range) frame(layer, ax1, ay1, ax2, ay2, OFT.sel, 2);
        else frame(layer, ax1 - 1, ay1 - 1, ax2 + 1, ay2 + 1, OFT.sel, 4);
    }
    layer->_clip_area = keep;
}

static void draw_headers(lv_layer_t *layer, int32_t r0, int32_t r1, int32_t c0, int32_t c1, bool cols, const lv_area_t *clip)
{
    lv_area_t keep = layer->_clip_area, cl;
    if (!ofu_clip(&cl, &keep, clip)) return;
    layer->_clip_area = cl;
    fill(layer, cl.x1, cl.y1, cl.x2, cl.y2, OFT.head, LV_OPA_COVER);
    const lv_font_t *f = ZF[V.z > 0 ? V.z - 1 : 0];
    int32_t sr1 = LV_MIN(V.ar, V.cr), sr2 = LV_MAX(V.ar, V.cr), sc1 = LV_MIN(V.ac, V.cc), sc2 = LV_MAX(V.ac, V.cc);
    if (cols) {
        for (int32_t c = c0; c <= c1; c++) {
            int32_t x1 = scr_x(c), x2 = scr_x(c + 1) - 1;
            if (x2 <= x1) continue;
            bool on = c >= sc1 && c <= sc2;
            if (on) fill(layer, x1, cl.y1, x2, cl.y2, OFT.sel, LV_OPA_30);
            fill(layer, x2, cl.y1, x2, cl.y2, OFT.grid, LV_OPA_COVER);
            char n[8];
            of_col_name(c, n);
            lv_area_t a = { x1, cl.y1, x2, cl.y2 };
            label(layer, n, f, on ? OFT.sel : OFT.dim, &a, LV_TEXT_ALIGN_CENTER);
        }
        fill(layer, cl.x1, cl.y2, cl.x2, cl.y2, OFT.grid, LV_OPA_COVER);
    } else {
        for (int32_t r = r0; r <= r1; r++) {
            int32_t y1 = scr_y(r), y2 = scr_y(r + 1) - 1;
            if (y2 <= y1) continue;
            bool on = r >= sr1 && r <= sr2;
            if (on) fill(layer, cl.x1, y1, cl.x2, y2, OFT.sel, LV_OPA_30);
            fill(layer, cl.x1, y2, cl.x2, y2, OFT.grid, LV_OPA_COVER);
            char n[12];
            snprintf(n, sizeof n, "%ld", (long)r + 1);
            lv_area_t a = { cl.x1, y1, cl.x2 - 4, y2 };
            label(layer, n, f, on ? OFT.sel : OFT.dim, &a, LV_TEXT_ALIGN_CENTER);
        }
        fill(layer, cl.x2, cl.y1, cl.x2, cl.y2, OFT.grid, LV_OPA_COVER);
    }
    layer->_clip_area = keep;
}

/* The grid's corner on the screen: drawing and the gestures work in screen
 * coordinates, and the app's root starts under the status bar. */
static void sync_origin(void)
{
    lv_area_t a;
    lv_obj_get_coords(V.grid, &a);
    V.gx = a.x1;
    V.gy = a.y1;
}

static void grid_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    sync_origin();
    int32_t X0 = V.gx, Y0 = V.gy, X1 = V.gx + V.gw - 1, Y1 = V.gy + V.gh - 1;
    int32_t px = V.gx + V.hw + colx(fc()), py = V.gy + V.hh + rowy(fr());
    /* what is in sight in the moving pane */
    int32_t c0 = col_at_x(colx(fc()) + V.sx), c1 = col_at_x(colx(fc()) + V.sx + (X1 - px));
    int32_t r0 = row_at_y(rowy(fr()) + V.sy), r1 = row_at_y(rowy(fr()) + V.sy + (Y1 - py));
    /* text that starts to the left of the pane and runs into it */
    int32_t c0t = c0 > 8 ? c0 - 8 : 0;
    if (c0t < fc()) c0t = fc();
    lv_area_t a;
    a = (lv_area_t){ px, py, X1, Y1 };
    draw_cells(layer, &a, r0, r1, c0t, c1);
    if (fc()) { a = (lv_area_t){ X0 + V.hw, py, px - 1, Y1 }; draw_cells(layer, &a, r0, r1, 0, fc() - 1); }
    if (fr()) { a = (lv_area_t){ px, Y0 + V.hh, X1, py - 1 }; draw_cells(layer, &a, 0, fr() - 1, c0t, c1); }
    if (fr() && fc()) { a = (lv_area_t){ X0 + V.hw, Y0 + V.hh, px - 1, py - 1 }; draw_cells(layer, &a, 0, fr() - 1, 0, fc() - 1); }
    /* headers */
    a = (lv_area_t){ px, Y0, X1, Y0 + V.hh - 1 };
    draw_headers(layer, 0, 0, c0, c1, true, &a);
    if (fc()) { a = (lv_area_t){ X0 + V.hw, Y0, px - 1, Y0 + V.hh - 1 }; draw_headers(layer, 0, 0, 0, fc() - 1, true, &a); }
    a = (lv_area_t){ X0, py, X0 + V.hw - 1, Y1 };
    draw_headers(layer, r0, r1, 0, 0, false, &a);
    if (fr()) { a = (lv_area_t){ X0, Y0 + V.hh, X0 + V.hw - 1, py - 1 }; draw_headers(layer, 0, fr() - 1, 0, 0, false, &a); }
    fill(layer, X0, Y0, X0 + V.hw - 1, Y0 + V.hh - 1, OFT.head, LV_OPA_COVER);
    /* the frozen panes' edges */
    if (fc()) fill(layer, px - 2, Y0, px - 1, Y1, OFT.dim, LV_OPA_COVER);
    if (fr()) fill(layer, X0, py - 2, X1, py - 1, OFT.dim, LV_OPA_COVER);
}

/* -------------------------------------------------------------------------- */
/* Undo                                                                        */
/* -------------------------------------------------------------------------- */

static char *source_of(int sheet, int32_t r, int32_t c, uint32_t *style)
{
    of_cell_t *x = of_cell_get(&V.b->sh[sheet], r, c);
    *style = x ? x->style : 0;
    if (!x || (x->v.t == OV_EMPTY && !x->f)) return NULL;
    char buf[1024];
    of_cell_source(V.b, x, buf, sizeof buf);
    return of_strdup(buf);
}

static void entry_free(uentry_t *u)
{
    for (int i = 0; i < u->n; i++) {
        of_free(u->ch[i].before);
        of_free(u->ch[i].after);
    }
    of_free(u->ch);
    u->ch = NULL;
    u->n = 0;
}

/* Before changing a cell: what it was. */
static void undo_note(int32_t r, int32_t c)
{
    uchange_t *n = of_realloc(V.cur.ch, (size_t)(V.cur.n + 1) * sizeof *n);
    if (!n) return;
    V.cur.ch = n;
    uchange_t *u = &V.cur.ch[V.cur.n++];
    memset(u, 0, sizeof *u);
    u->sheet = V.si;
    u->r = r;
    u->c = c;
    u->before = source_of(V.si, r, c, &u->style_before);
}

/* After: what it is now, and the step goes on the stack. */
static void undo_close(void)
{
    if (!V.cur.n) return;
    for (int i = 0; i < V.cur.n; i++)
        V.cur.ch[i].after = source_of(V.cur.ch[i].sheet, V.cur.ch[i].r, V.cur.ch[i].c, &V.cur.ch[i].style_after);
    if (V.nundo == UNDO_MAX) {
        entry_free(&V.undo[0]);
        memmove(&V.undo[0], &V.undo[1], (UNDO_MAX - 1) * sizeof(uentry_t));
        V.nundo--;
    }
    V.undo[V.nundo++] = V.cur;
    memset(&V.cur, 0, sizeof V.cur);
    for (int i = 0; i < V.nredo; i++) entry_free(&V.redo[i]);
    V.nredo = 0;
    V.modified = true;
}

static void apply(uentry_t *u, bool back)
{
    for (int i = back ? u->n - 1 : 0; back ? i >= 0 : i < u->n; i += back ? -1 : 1) {
        uchange_t *ch = &u->ch[i];
        of_sheet_t *s = &V.b->sh[ch->sheet];
        const char *src = back ? ch->before : ch->after;
        of_cell_input(V.b, s, ch->r, ch->c, src ? src : "");
        of_cell_t *x = of_cell_get(s, ch->r, ch->c);
        if (x) x->style = back ? ch->style_before : ch->style_after;
    }
    of_book_recalc(V.b);
    V.modified = true;
}

static void do_undo(void)
{
    if (V.editing) edit_cancel();
    if (!V.nundo) return;
    uentry_t u = V.undo[--V.nundo];
    apply(&u, true);
    V.redo[V.nredo++] = u;
    if (u.n) {
        if (u.ch[0].sheet != V.si) { V.si = u.ch[0].sheet; relayout(); }
        V.ar = V.cr = u.ch[0].r;
        V.ac = V.cc = u.ch[0].c;
        scroll_to_cell(V.cr, V.cc);
    }
    geometry();
    refresh();
}

static void do_redo(void)
{
    if (V.editing) edit_cancel();
    if (!V.nredo) return;
    uentry_t u = V.redo[--V.nredo];
    apply(&u, false);
    V.undo[V.nundo++] = u;
    if (u.n) {
        if (u.ch[0].sheet != V.si) { V.si = u.ch[0].sheet; relayout(); }
        V.ar = V.cr = u.ch[0].r;
        V.ac = V.cc = u.ch[0].c;
        scroll_to_cell(V.cr, V.cc);
    }
    geometry();
    refresh();
}

/* -------------------------------------------------------------------------- */
/* Changing cells                                                              */
/* -------------------------------------------------------------------------- */

static bool locked(int32_t r, int32_t c)
{
    of_cell_t *x = of_cell_get(SH(), r, c);
    if (x && (x->flags & OC_ARRAY)) {
        of_app_toast(_("Esa celda es parte de una fórmula matricial: se cambia en la computadora"));
        return true;
    }
    return false;
}

static void set_cell(int32_t r, int32_t c, const char *text)
{
    undo_note(r, c);
    of_cell_input(V.b, SH(), r, c, text);
}

static void sel_range(int32_t *r1, int32_t *c1, int32_t *r2, int32_t *c2)
{
    *r1 = LV_MIN(V.ar, V.cr);
    *r2 = LV_MAX(V.ar, V.cr);
    *c1 = LV_MIN(V.ac, V.cc);
    *c2 = LV_MAX(V.ac, V.cc);
}

static void clear_selection_cells(void)
{
    int32_t r1, c1, r2, c2;
    sel_range(&r1, &c1, &r2, &c2);
    of_sheet_t *s = SH();
    for (int i = 0; i < s->nrows; i++) {
        of_row_t *row = &s->rows[i];
        if (row->row < r1 || row->row > r2) continue;
        for (int k = 0; k < row->n; k++) {
            int32_t c = row->c[k].col;
            if (c < c1 || c > c2) continue;
            if (row->c[k].v.t == OV_EMPTY && !row->c[k].f) continue;
            if (row->c[k].flags & OC_ARRAY) continue;
            int32_t rr = row->row;
            set_cell(rr, c, "");
            row = &s->rows[i];          /* the cells may have moved */
        }
    }
    undo_close();
    of_book_recalc(V.b);
    refresh();
}

/* -------------------------------------------------------------------------- */
/* Clipboard: the selection as tab-separated sources                           */
/* -------------------------------------------------------------------------- */

static void do_copy(bool cut)
{
    int32_t r1, c1, r2, c2;
    sel_range(&r1, &c1, &r2, &c2);
    if ((int64_t)(r2 - r1 + 1) * (c2 - c1 + 1) > 200000) { of_app_toast(_("Es demasiado para copiar de una vez")); return; }
    of_sb_t sb = { 0 };
    for (int32_t r = r1; r <= r2; r++) {
        for (int32_t c = c1; c <= c2; c++) {
            if (c > c1) of_sb_putc(&sb, '\t');
            of_cell_t *x = of_cell_get(SH(), r, c);
            if (!x) continue;
            char buf[1024];
            of_cell_source(V.b, x, buf, sizeof buf);
            for (char *p = buf; *p; p++)
                if (*p == '\t' || *p == '\n') *p = ' ';
            of_sb_puts(&sb, buf);
        }
        of_sb_putc(&sb, '\n');
    }
    if (sb.oom) { of_sb_free(&sb); return; }
    aos_ui_clip_set(sb.s ? sb.s : "");
    of_free(V.clip);
    V.clip = of_sb_take(&sb);
    V.clip_r = r1;
    V.clip_c = c1;
    V.clip_sheet = V.si;
    if (cut) clear_selection_cells();
    of_app_toast(cut ? _("Cortado") : _("Copiado"));
}

static void do_paste(void)
{
    size_t cap = 256 * 1024;
    char *buf = of_alloc(cap);
    if (!buf) return;
    size_t n = aos_ui_clip_get(buf, cap);
    if (!n) { of_free(buf); of_app_toast(_("No hay nada para pegar")); return; }
    bool ours = V.clip && !strcmp(V.clip, buf);
    int32_t r = V.cr;
    const char *p = buf;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        int32_t cc = V.cc;
        const char *q = p;
        while (q <= p + ll) {
            const char *tab = memchr(q, '\t', (size_t)(p + ll - q));
            size_t fl = tab ? (size_t)(tab - q) : (size_t)(p + ll - q);
            char *field = of_strndup(q, fl);
            if (field && fl && field[fl - 1] == '\r') field[fl - 1] = 0;
            if (field && !locked(r, cc)) {
                /* a formula copied here moves with the cell */
                if (ours && field[0] == '=') {
                    char *moved = of_formula_shift(field + 1, V.b, V.si, V.cr - V.clip_r, V.cc - V.clip_c);
                    if (moved) {
                        char *f2 = of_alloc(strlen(moved) + 2);
                        if (f2) { f2[0] = '='; strcpy(f2 + 1, moved); set_cell(r, cc, f2); of_free(f2); }
                        of_free(moved);
                    } else {
                        set_cell(r, cc, field);
                    }
                } else {
                    set_cell(r, cc, field);
                }
            }
            of_free(field);
            cc++;
            if (!tab) break;
            q = tab + 1;
        }
        r++;
        if (!eol) break;
        p = eol + 1;
        if (!*p) break;
    }
    of_free(buf);
    undo_close();
    of_book_recalc(V.b);
    geometry();
    refresh();
}

/* -------------------------------------------------------------------------- */
/* The formula bar and editing                                                 */
/* -------------------------------------------------------------------------- */

static void update_fbar(void)
{
    if (!V.fref) return;
    char ref[24];
    int32_t r1, c1, r2, c2;
    sel_range(&r1, &c1, &r2, &c2);
    if (r1 != r2 || c1 != c2) {
        char a[12], b2[12];
        of_ref_name(r1, c1, a);
        of_ref_name(r2, c2, b2);
        snprintf(ref, sizeof ref, "%s:%s", a, b2);
    } else {
        of_ref_name(V.cr, V.cc, ref);
    }
    lv_label_set_text(lv_obj_get_child(V.fref, 0), ref);
    if (!V.editing) {
        char src[1024];
        of_cell_source(V.b, of_cell_get(SH(), V.cr, V.cc), src, sizeof src);
        lv_label_set_text(V.ftext, src);
    }
}

typedef struct { double sum; long count, nums; } stat_t;

static void update_status(void)
{
    if (!V.status) return;
    int32_t r1, c1, r2, c2;
    sel_range(&r1, &c1, &r2, &c2);
    if (r1 == r2 && c1 == c2) {
        lv_label_set_text(V.status, "");
        return;
    }
    stat_t st = { 0 };
    of_sheet_t *s = SH();
    for (int i = 0; i < s->nrows; i++) {
        of_row_t *row = &s->rows[i];
        if (row->row < r1 || row->row > r2 || row->hidden) continue;
        for (int k = 0; k < row->n; k++) {
            of_cell_t *c = &row->c[k];
            if (c->col < c1 || c->col > c2 || c->v.t == OV_EMPTY) continue;
            st.count++;
            if (c->v.t == OV_NUM) { st.sum += c->v.n; st.nums++; }
        }
    }
    char a[48], b[48], t[200];
    bool right;
    of_val_t vs = { .t = OV_NUM, .n = st.sum }, va = { .t = OV_NUM, .n = st.nums ? st.sum / (double)st.nums : 0 };
    of_numfmt_format("#,##0.##", &vs, false, a, sizeof a, &right);
    of_numfmt_format("#,##0.##", &va, false, b, sizeof b, &right);
    if (st.nums) snprintf(t, sizeof t, _("Suma %s  ·  Prom. %s"), a, b);
    else snprintf(t, sizeof t, _("%ld celdas"), st.count);
    lv_label_set_text(V.status, t);
}

static void refresh(void)
{
    if (V.grid) lv_obj_invalidate(V.grid);
    update_fbar();
    update_status();
    if (V.b_undo) {
        if (V.nundo) lv_obj_remove_state(V.b_undo, LV_STATE_DISABLED);
        else lv_obj_add_state(V.b_undo, LV_STATE_DISABLED);
    }
    if (V.b_redo) {
        if (V.nredo) lv_obj_remove_state(V.b_redo, LV_STATE_DISABLED);
        else lv_obj_add_state(V.b_redo, LV_STATE_DISABLED);
    }
    if (V.b_save) ofu_icon_color(V.b_save, V.modified ? OFT.accent : OFT.dim);
}

static void sel_changed(void)
{
    scroll_to_cell(V.cr, V.cc);
    refresh();
}

static void move_sel(int dr, int dc, bool extend)
{
    int32_t r = V.cr + dr, c = V.cc + dc;
    /* skip rows the filter hides */
    while (dr && r >= 0 && r < 1048575 && row_px(r) == 0) r += dr > 0 ? 1 : -1;
    if (r < 0) r = 0;
    if (c < 0) c = 0;
    if (r > 1048575) r = 1048575;
    if (c > 16383) c = 16383;
    V.cr = r;
    V.cc = c;
    if (!extend) { V.ar = r; V.ac = c; }
    sel_changed();
}

/* the keyboard's target while a cell is edited: the textarea, with Enter,
 * Tab and Esc the spreadsheet's */
static void ta_insert(const char *s) { if (V.ta) lv_textarea_add_text(V.ta, s); }
static void ta_bksp(void) { if (V.ta) lv_textarea_delete_char(V.ta); }
static void ta_enter(void) { edit_commit(1, 0); }
static void ta_move(int dx)
{
    if (!V.ta) return;
    for (; dx < 0; dx++) lv_textarea_cursor_left(V.ta);
    for (; dx > 0; dx--) lv_textarea_cursor_right(V.ta);
}
static bool ta_caps(void) { return false; }

static bool ta_key(uint32_t k, uint8_t mods)
{
    if (k == AOS_KEY_ENTER) { edit_commit((mods & OF_MOD_SHIFT) ? -1 : 1, 0); return true; }
    if (k == AOS_KEY_NEXT) { edit_commit(0, (mods & OF_MOD_SHIFT) ? -1 : 1); return true; }
    if (k == AOS_KEY_PREV) { edit_commit(0, -1); return true; }
    if (k == AOS_KEY_ESC) { edit_cancel(); return true; }
    if (k == AOS_KEY_UP || k == AOS_KEY_DOWN) { edit_commit(k == AOS_KEY_UP ? -1 : 1, 0); return true; }
    if (k == AOS_KEY_DEL) { if (V.ta) lv_textarea_delete_char_forward(V.ta); return true; }
    if (k == AOS_KEY_HOME) { if (V.ta) lv_textarea_set_cursor_pos(V.ta, 0); return true; }
    if (k == AOS_KEY_END) { if (V.ta) lv_textarea_set_cursor_pos(V.ta, LV_TEXTAREA_CURSOR_LAST); return true; }
    return false;
}

static const ofu_kb_target_t TA_TARGET = { ta_insert, ta_bksp, ta_enter, ta_move, ta_caps, ta_key };

static void show_kb(bool on)
{
    if (on && !V.kb) {
        bool land = V.W > V.H;
        int32_t kbh = land ? 300 : 420;
        V.kb = ofu_kb_create(V.root, V.W, kbh);
        lv_obj_align(V.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    } else if (!on && V.kb) {
        lv_obj_delete(V.kb);
        V.kb = NULL;
    }
    relayout();
}

static void edit_begin(const char *initial, bool replace)
{
    if (locked(V.cr, V.cc)) return;
    V.editing = true;
    V.ref_mode = false;
    lv_obj_add_flag(V.ftext, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(V.ta, LV_OBJ_FLAG_HIDDEN);
    char src[1024] = "";
    if (!replace) of_cell_source(V.b, of_cell_get(SH(), V.cr, V.cc), src, sizeof src);
    if (initial) snprintf(src + strlen(src), sizeof src - strlen(src), "%s", initial);
    lv_textarea_set_text(V.ta, src);
    lv_textarea_set_cursor_pos(V.ta, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_state(V.ta, LV_STATE_FOCUSED);
    ofu_kb_attach(&TA_TARGET);
    show_kb(true);
    /* a number, or a column of numbers: the numbers' page first */
    {
        of_cell_t *x = of_cell_get(SH(), V.cr, V.cc);
        of_cell_t *up = V.cr ? of_cell_get(SH(), V.cr - 1, V.cc) : NULL;
        bool num = (x && x->v.t == OV_NUM && !x->f) || (!x && up && up->v.t == OV_NUM);
        if (num && (!initial || (initial[0] >= '0' && initial[0] <= '9'))) ofu_kb_numeric(true);
    }
    /* the keyboard took room: keep the cell in sight */
    scroll_to_cell(V.cr, V.cc);
    refresh();
}

static void edit_end(void)
{
    V.editing = false;
    V.ref_mode = false;
    lv_obj_add_flag(V.ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(V.ftext, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(V.ta, LV_STATE_FOCUSED);
    show_kb(false);
}

static bool grid_key(uint32_t k, uint8_t mods);
static void grid_target(void);

static void edit_commit(int move_r, int move_c)
{
    if (!V.editing) return;
    const char *t = lv_textarea_get_text(V.ta);
    char *text = of_strdup(t);
    /* a formula that does not read is not stored as an error: it stays to fix */
    if (text && text[0] == '=' && text[1]) {
        bool unk = false;
        of_node_t *n = of_formula_parse(text + 1, V.si, V.b, &unk);
        if (!n) {
            of_free(text);
            of_app_toast(_("La fórmula tiene un error"));
            return;
        }
        of_formula_free(n);
        if (unk) of_app_toast(_("La fórmula usa una función que la app no tiene"));
    }
    edit_end();
    char src[1024];
    of_cell_source(V.b, of_cell_get(SH(), V.cr, V.cc), src, sizeof src);
    if (text && strcmp(src, text) != 0) {
        set_cell(V.cr, V.cc, text);
        undo_close();
        of_book_recalc(V.b);
        geometry();
    }
    of_free(text);
    grid_target();
    if (move_r || move_c) move_sel(move_r, move_c, false);
    else refresh();
}

static void edit_cancel(void)
{
    if (!V.editing) return;
    edit_end();
    grid_target();
    refresh();
}

/* While a formula is typed, a tapped cell becomes its reference. */
static bool wants_ref(void)
{
    if (!V.editing || !V.ta) return false;
    const char *t = lv_textarea_get_text(V.ta);
    if (t[0] != '=') return false;
    uint32_t pos = lv_textarea_get_cursor_pos(V.ta);
    /* after an operator, a parenthesis or a separator, or replacing the
     * reference just inserted */
    if (V.ref_mode) return true;
    if (pos == 0) return false;
    const char *p = t;
    uint32_t i = 0;
    char prev = 0;
    while (*p && i < pos) {
        prev = *p;
        p += of_utf8_len((uint8_t)*p);
        i++;
    }
    return strchr("=+-*/^&(,;:<>", prev) != NULL;
}

static void put_ref(int32_t r1, int32_t c1, int32_t r2, int32_t c2)
{
    char ref[32], a[12], b2[12];
    of_ref_name(r1, c1, a);
    of_ref_name(r2, c2, b2);
    if (r1 == r2 && c1 == c2) snprintf(ref, sizeof ref, "%s", a);
    else snprintf(ref, sizeof ref, "%s:%s", a, b2);
    if (V.ref_mode) {
        /* replace the one inserted before */
        while ((int)lv_textarea_get_cursor_pos(V.ta) > V.ref_at) lv_textarea_delete_char(V.ta);
    } else {
        V.ref_at = (int)lv_textarea_get_cursor_pos(V.ta);
    }
    lv_textarea_add_text(V.ta, ref);
    V.ref_mode = true;
}

/* -------------------------------------------------------------------------- */
/* Gestures                                                                    */
/* -------------------------------------------------------------------------- */

static void fling_cb(lv_timer_t *t)
{
    (void)t;
    V.sx -= (int32_t)(V.vx / 60);
    V.sy -= (int32_t)(V.vy / 60);
    V.vx *= 0.92f;
    V.vy *= 0.92f;
    int32_t ox = V.sx, oy = V.sy;
    clamp_scroll();
    if (ox != V.sx) V.vx = 0;
    if (oy != V.sy) V.vy = 0;
    lv_obj_invalidate(V.grid);
    if (V.vx * V.vx + V.vy * V.vy < 400) {
        lv_timer_delete(V.fling);
        V.fling = NULL;
    }
}

static void stop_fling(void)
{
    if (V.fling) {
        lv_timer_delete(V.fling);
        V.fling = NULL;
    }
}

static void zoom(int dz, float px, float py)
{
    int nz = V.z + dz;
    if (nz < 0 || nz >= NZOOM) return;
    /* keep the cell under the fingers where it is */
    int32_t r, c;
    bool at = cell_at(px, py, &r, &c);
    int32_t ox = 0, oy = 0;
    if (at) {
        ox = (int32_t)px - scr_x(c);
        oy = (int32_t)py - scr_y(r);
    }
    int oz = V.z;
    V.z = nz;
    geometry();
    if (at) {
        ox = ox * ZP[nz] / ZP[oz];
        oy = oy * ZP[nz] / ZP[oz];
        if (c >= fc()) V.sx = colx(c) - colx(fc()) - ((int32_t)px - V.gx - V.hw - colx(fc()) - ox);
        if (r >= fr()) V.sy = rowy(r) - rowy(fr()) - ((int32_t)py - V.gy - V.hh - rowy(fr()) - oy);
    }
    clamp_scroll();
    aos_hal_pref_set_i32("of_zoom", V.z);
    lv_obj_invalidate(V.grid);
}

static int32_t col_edge_at(float px)
{
    /* a column's right edge within reach of the finger, in the letters' row */
    int32_t x = (int32_t)px;
    int32_t c0 = col_at_x(x - V.gx - V.hw < colx(fc()) ? x - V.gx - V.hw : x - V.gx - V.hw + V.sx);
    for (int32_t c = c0 > 0 ? c0 - 1 : 0; c <= c0 + 1; c++) {
        int32_t ex = scr_x(c + 1);
        if (x > ex - 22 && x < ex + 22) return c;
    }
    return -1;
}

static void gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    (void)user;
    if (ofu_sheet_open()) return;
    sync_origin();
    int32_t r, c;
    switch (ev->type) {
    case AOS_GESTURE_TAP:
        stop_fling();
        if (ev->y < V.gy + V.hh && ev->x > V.gx + V.hw) {
            /* a column: all of it */
            if (cell_at(ev->x, V.gy + V.hh + 1, &r, &c)) {
                int32_t mr, mc;
                of_sheet_extent(SH(), &mr, &mc);
                V.ac = V.cc = c;
                V.ar = 0;
                V.cr = mr > 0 ? mr : 0;
                refresh();
            }
            return;
        }
        if (ev->x < V.gx + V.hw && ev->y > V.gy + V.hh) {
            if (cell_at(V.gx + V.hw + 1, ev->y, &r, &c)) {
                int32_t mr, mc;
                of_sheet_extent(SH(), &mr, &mc);
                V.ar = V.cr = r;
                V.ac = 0;
                V.cc = mc > 0 ? mc : 0;
                refresh();
            }
            return;
        }
        if (!cell_at(ev->x, ev->y, &r, &c)) return;
        if (wants_ref()) {
            put_ref(r, c, r, c);
            V.drag_mode = 3;
            V.ar = r;
            V.ac = c;
            return;
        }
        if (V.editing) edit_commit(0, 0);
        {
            const of_range_t *m = merge_of(r, c);
            if (m) { r = (int32_t)m->r1; c = (int32_t)m->c1; }
        }
        if (r == V.cr && c == V.cc && V.ar == V.cr && V.ac == V.cc && !V.editing) {
            edit_begin(NULL, false);        /* a second tap on the same cell edits it */
            return;
        }
        V.ar = V.cr = r;
        V.ac = V.cc = c;
        refresh();
        break;
    case AOS_GESTURE_DOUBLE_TAP:
        if (cell_at(ev->x, ev->y, &r, &c) && !V.editing) {
            V.ar = V.cr = r;
            V.ac = V.cc = c;
            edit_begin(NULL, false);
        }
        break;
    case AOS_GESTURE_LONG_PRESS:
        if (cell_at(ev->x, ev->y, &r, &c)) {
            if (wants_ref()) { V.ar = r; V.ac = c; put_ref(r, c, r, c); V.drag_mode = 3; return; }
            V.ar = V.cr = r;
            V.ac = V.cc = c;
            V.drag_mode = 1;
            refresh();
        }
        break;
    case AOS_GESTURE_DRAG_BEGIN:
        stop_fling();
        V.drag_mode = 0;
        if (ev->after_long) { V.drag_mode = wants_ref() ? 3 : 1; break; }
        if (ev->y < V.gy + V.hh && ev->x > V.gx + V.hw) {
            int32_t ce = col_edge_at(ev->x);
            if (ce >= 0) {
                V.drag_mode = 2;
                V.resize_col = ce;
                V.resize_w0 = scr_x(ce + 1) - scr_x(ce);
                V.resize_x0 = ev->x;
                break;
            }
        }
        if (wants_ref() && cell_at(ev->x, ev->y, &r, &c)) {
            V.drag_mode = 3;
            V.ar = r;
            V.ac = c;
            put_ref(r, c, r, c);
        }
        break;
    case AOS_GESTURE_DRAG:
        if (V.drag_mode == 1 || V.drag_mode == 3) {
            if (cell_at(ev->x, ev->y, &r, &c)) {
                if (V.drag_mode == 1) {
                    V.cr = r;
                    V.cc = c;
                    refresh();
                } else {
                    put_ref(LV_MIN(V.ar, r), LV_MIN(V.ac, c), LV_MAX(V.ar, r), LV_MAX(V.ac, c));
                }
            }
            break;
        }
        if (V.drag_mode == 2) {
            int32_t w = V.resize_w0 + (int32_t)(ev->x - V.resize_x0);
            if (w < 8) w = 8;
            float chars = (float)(w - ZP[V.z] / 3) / (ZP[V.z] * 0.52f);
            if (chars < 0.5f) chars = 0.5f;
            of_sheet_set_colw(SH(), V.resize_col, chars);
            V.modified = true;
            geometry();
            lv_obj_invalidate(V.grid);
            break;
        }
        V.sx -= (int32_t)ev->dx;
        V.sy -= (int32_t)ev->dy;
        clamp_scroll();
        lv_obj_invalidate(V.grid);
        break;
    case AOS_GESTURE_DRAG_END:
        if (V.drag_mode == 0 && (ev->vx * ev->vx + ev->vy * ev->vy) > 90000) {
            V.vx = ev->vx;
            V.vy = ev->vy;
            if (!V.fling) V.fling = lv_timer_create(fling_cb, 16, NULL);
        }
        if (V.drag_mode == 2) refresh();
        V.drag_mode = 0;
        break;
    case AOS_GESTURE_PINCH_BEGIN:
        stop_fling();
        V.pinch_acc = 1;
        break;
    case AOS_GESTURE_PINCH:
        V.pinch_acc *= ev->scale;
        if (V.pinch_acc > 1.18f) { zoom(1, ev->x, ev->y); V.pinch_acc = 1; }
        else if (V.pinch_acc < 0.85f) { zoom(-1, ev->x, ev->y); V.pinch_acc = 1; }
        else if (ev->dx || ev->dy) {
            V.sx -= (int32_t)ev->dx;
            V.sy -= (int32_t)ev->dy;
            clamp_scroll();
            lv_obj_invalidate(V.grid);
        }
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* The keyboard on the grid                                                    */
/* -------------------------------------------------------------------------- */

static void find_open(void);
static void menu_open(void);
bool of_sheet_save(void);

static void g_insert(const char *s)
{
    /* typing on the grid starts editing, replacing what was there */
    edit_begin(s, true);
}

static void g_bksp(void)
{
    edit_begin(NULL, true);
}

static void g_enter(void) { move_sel(1, 0, false); }
static void g_move(int dx) { move_sel(0, dx, false); }
static bool g_caps(void) { return false; }

static bool grid_key(uint32_t k, uint8_t mods)
{
    bool shift = mods & OF_MOD_SHIFT;
    bool cmd = mods & OF_MOD_CMD;
    if (cmd) {
        switch (k | 0x20) {
        case 'z': if (shift) do_redo(); else do_undo(); return true;
        case 'y': do_redo(); return true;
        case 'c': do_copy(false); return true;
        case 'x': do_copy(true); return true;
        case 'v': do_paste(); return true;
        case 's': of_sheet_save(); return true;
        case 'f': find_open(); return true;
        case 'a': {
            int32_t mr, mc;
            of_sheet_extent(SH(), &mr, &mc);
            V.ar = 0; V.ac = 0; V.cr = mr > 0 ? mr : 0; V.cc = mc > 0 ? mc : 0;
            refresh();
            return true;
        }
        default: break;
        }
        if (k == AOS_KEY_UP || k == AOS_KEY_DOWN || k == AOS_KEY_LEFT || k == AOS_KEY_RIGHT) {
            /* to the edge of the data */
            int32_t mr, mc;
            of_sheet_extent(SH(), &mr, &mc);
            if (k == AOS_KEY_UP) V.cr = 0;
            else if (k == AOS_KEY_DOWN) V.cr = mr > 0 ? mr : 0;
            else if (k == AOS_KEY_LEFT) V.cc = 0;
            else V.cc = mc > 0 ? mc : 0;
            if (!shift) { V.ar = V.cr; V.ac = V.cc; }
            sel_changed();
            return true;
        }
        return true;
    }
    switch (k) {
    case AOS_KEY_UP: move_sel(-1, 0, shift); return true;
    case AOS_KEY_DOWN: move_sel(1, 0, shift); return true;
    case AOS_KEY_LEFT: move_sel(0, -1, shift); return true;
    case AOS_KEY_RIGHT: move_sel(0, 1, shift); return true;
    case AOS_KEY_ENTER: move_sel(shift ? -1 : 1, 0, false); return true;
    case AOS_KEY_NEXT: move_sel(0, shift ? -1 : 1, false); return true;
    case AOS_KEY_PREV: move_sel(0, -1, false); return true;
    case AOS_KEY_HOME: V.cc = 0; if (!shift) { V.ac = 0; V.ar = V.cr; } sel_changed(); return true;
    case AOS_KEY_DEL: case AOS_KEY_BACKSPACE: clear_selection_cells(); return true;
    case AOS_KEY_ESC: V.ar = V.cr; V.ac = V.cc; refresh(); return true;
    default: break;
    }
    return false;
}

static const ofu_kb_target_t GRID_TARGET = { g_insert, g_bksp, g_enter, g_move, g_caps, grid_key };

static void grid_target(void)
{
    ofu_kb_attach(&GRID_TARGET);
}

/* -------------------------------------------------------------------------- */
/* Menus                                                                       */
/* -------------------------------------------------------------------------- */

static const char *const FMT_CODES[] = {
    "General", "0", "#,##0", "0.00", "#,##0.00", "\"$\"\\ #,##0.00", "0%", "0.00%", "dd/mm/yyyy", "dd/mm/yyyy hh:mm", "hh:mm", "@" };

static void apply_format(int i)
{
    if (V.b->fmt == OF_FMT_CSV || V.b->fmt == OF_FMT_TSV) {
        of_app_toast(_("Un CSV no guarda formatos: guardalo como .xlsx o .ods"));
        return;
    }
    if (V.b->fmt == OF_FMT_ODS) {
        of_app_toast(_("En los .ods los formatos se cambian en la computadora por ahora"));
        return;
    }
    const char *code = FMT_CODES[i];
    if (!of_locale_comma()) {
        if (i == 8) code = "m/d/yyyy";
        if (i == 9) code = "m/d/yyyy h:mm";
    }
    int32_t r1, c1, r2, c2;
    sel_range(&r1, &c1, &r2, &c2);
    if ((int64_t)(r2 - r1 + 1) * (c2 - c1 + 1) > 100000) return;
    for (int32_t r = r1; r <= r2; r++)
        for (int32_t c = c1; c <= c2; c++) {
            of_cell_t *x = of_cell_get(SH(), r, c);
            if (!x && i == 0) continue;
            undo_note(r, c);
            x = of_cell_make(SH(), r, c);
            if (!x) continue;
            x->style = i == 0 ? (x->style < (uint32_t)V.b->nxf ? of_book_xf_with_numfmt(V.b, x->style, "General") : 0)
                              : of_book_xf_with_numfmt(V.b, x->style, code);
            x->flags |= OC_STYLED;
            SH()->dirty = true;
            of_row_t *row = of_row_get(SH(), r);
            if (row) row->dirty = 1;
        }
    /* the undo step holds the style before and after */
    undo_close();
    refresh();
}

static void fmt_pick(int i, void *ud) { (void)ud; apply_format(i); }

static void fmt_open(void)
{
    char ex[12][48];
    static const char *const NAMES[] = { N_("General"), N_("Entero"), N_("Entero con miles"), N_("Dos decimales"),
                                         N_("Miles y dos decimales"), N_("Moneda"), N_("Porcentaje"),
                                         N_("Porcentaje con decimales"), N_("Fecha"), N_("Fecha y hora"), N_("Hora"),
                                         N_("Texto") };
    ofu_item_t items[12];
    of_val_t v = { .t = OV_NUM, .n = 1234.5 };
    of_val_t d = { .t = OV_NUM, .n = 46348.5 };
    for (int i = 0; i < 12; i++) {
        char sample[32];
        bool right;
        of_numfmt_format(FMT_CODES[i], i >= 8 && i <= 10 ? &d : (i == 6 || i == 7) ? &(of_val_t){ .t = OV_NUM, .n = 0.155 } : &v,
                         false, sample, sizeof sample, &right);
        snprintf(ex[i], sizeof ex[i], "%s   %s", _(NAMES[i]), sample);
        items[i] = (ofu_item_t){ ex[i], -1, false, false };
    }
    ofu_sheet(_("Formato de número"), items, 12, fmt_pick, NULL);
}

static void sort_sel(bool desc)
{
    int32_t r1, c1, r2, c2;
    sel_range(&r1, &c1, &r2, &c2);
    int32_t mr, mc;
    of_sheet_extent(SH(), &mr, &mc);
    if (r1 == r2 && c1 == c2) {
        /* one cell: the block of data around it, sorted by its column, the
         * first row taken as titles when it is text over numbers */
        r1 = 0;
        r2 = mr;
        c1 = 0;
        c2 = mc;
        of_cell_t *h = of_cell_get(SH(), 0, V.cc), *d = of_cell_get(SH(), 1, V.cc);
        if (h && h->v.t == OV_STR && (!d || d->v.t != OV_STR || V.cr > 0)) r1 = 1;
    }
    if (r2 > mr) r2 = mr;
    if (c2 > mc) c2 = mc;
    if (r2 <= r1) return;
    if ((int64_t)(r2 - r1 + 1) * (c2 - c1 + 1) > 200000) { of_app_toast(_("Es demasiado para ordenar en la placa")); return; }
    for (int32_t r = r1; r <= r2; r++)
        for (int32_t c = c1; c <= c2; c++)
            if (of_cell_get(SH(), r, c)) {
                if (locked(r, c)) return;
                undo_note(r, c);
            }
    of_sheet_sort(V.b, SH(), r1, r2, c1, c2, V.cc, desc);
    /* the cells that were empty and are not now */
    for (int32_t r = r1; r <= r2; r++)
        for (int32_t c = c1; c <= c2; c++) {
            bool noted = false;
            for (int i = 0; i < V.cur.n && !noted; i++) noted = V.cur.ch[i].r == r && V.cur.ch[i].c == c;
            if (!noted && of_cell_get(SH(), r, c)) {
                uchange_t *n = of_realloc(V.cur.ch, (size_t)(V.cur.n + 1) * sizeof *n);
                if (n) {
                    V.cur.ch = n;
                    memset(&V.cur.ch[V.cur.n], 0, sizeof *n);
                    V.cur.ch[V.cur.n].sheet = V.si;
                    V.cur.ch[V.cur.n].r = r;
                    V.cur.ch[V.cur.n].c = c;
                    V.cur.n++;
                }
            }
        }
    undo_close();
    geometry();
    refresh();
}

static void filter_toggle(void)
{
    of_sheet_t *s = SH();
    if (V.filtered) {
        for (int i = 0; i < s->nrows; i++) s->rows[i].hidden = 0;
        V.filtered = false;
        geometry();
        refresh();
        return;
    }
    /* the rows whose cell in this column is like the selected one stay */
    of_cell_t *key = of_cell_get(s, V.cr, V.cc);
    char want[256];
    bool right;
    of_cell_display(V.b, key, want, sizeof want, &right);
    int32_t mr, mc;
    of_sheet_extent(s, &mr, &mc);
    int32_t first = 1;              /* the title row stays */
    int hidden = 0;
    for (int32_t r = first; r <= mr; r++) {
        of_row_t *row = of_row_make(s, r);
        if (!row) continue;
        of_cell_t *c = of_cell_get(s, r, V.cc);
        char t[256];
        of_cell_display(V.b, c, t, sizeof t, &right);
        row = of_row_get(s, r);
        if (row) row->hidden = strcmp(t, want) != 0;
        if (row && row->hidden) hidden++;
    }
    V.filtered = true;
    geometry();
    clamp_scroll();
    refresh();
    char msg[96];
    snprintf(msg, sizeof msg, _("Filtro: «%.40s» (%d filas escondidas)"), want, hidden);
    of_app_toast(msg);
}

static void autofit(void)
{
    of_sheet_t *s = SH();
    int32_t c1 = LV_MIN(V.ac, V.cc), c2 = LV_MAX(V.ac, V.cc);
    const lv_font_t *f = ZF[V.z];
    for (int32_t c = c1; c <= c2 && c < 16384; c++) {
        int32_t best = 0;
        for (int i = 0; i < s->nrows; i++) {
            of_cell_t *x = of_cell_get(s, s->rows[i].row, c);
            if (!x || x->v.t == OV_EMPTY) continue;
            char t[256];
            bool right;
            of_cell_display(V.b, x, t, sizeof t, &right);
            int32_t w = text_w(t, f);
            if (w > best) best = w;
        }
        if (!best) continue;
        float chars = (float)(best + ZP[V.z] / 2) / (ZP[V.z] * 0.52f);
        of_sheet_set_colw(s, c, chars < 2 ? 2 : chars > 80 ? 80 : chars);
    }
    V.modified = true;
    geometry();
    refresh();
}

static void freeze_here(void)
{
    of_sheet_t *s = SH();
    if (s->freeze_r || s->freeze_c) {
        s->freeze_r = s->freeze_c = 0;
    } else {
        s->freeze_r = V.cr;
        s->freeze_c = V.cc;
        if (!s->freeze_r && !s->freeze_c) s->freeze_r = 1;
    }
    if (V.b->fmt == OF_FMT_XLSX && V.b->zip.n)
        of_app_toast(_("Inmovilizar se ve en la placa; en el archivo queda como estaba"));
    V.sx = V.sy = 0;
    geometry();
    refresh();
}

static void goto_done(const char *text, void *ud)
{
    (void)ud;
    int32_t r, c;
    int used;
    const char *t = text;
    const char *bang = strchr(t, '!');
    if (bang) {
        for (int i = 0; i < V.b->nsh; i++)
            if ((int)strlen(V.b->sh[i].name) == bang - t && !strncasecmp(V.b->sh[i].name, t, (size_t)(bang - t))) {
                V.si = i;
                relayout();
            }
        t = bang + 1;
    }
    if (!of_parse_ref(t, &r, &c, &used)) { of_app_toast(_("Esa no es una celda (por ejemplo, B12)")); return; }
    V.ar = V.cr = r;
    V.ac = V.cc = c;
    sel_changed();
}

/* find: the next cell whose shown text has it */
static char s_find[128];

static void find_next(void)
{
    if (!s_find[0]) return;
    of_sheet_t *s = SH();
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s->nrows; i++) {
            of_row_t *row = &s->rows[i];
            for (int k = 0; k < row->n; k++) {
                of_cell_t *c = &row->c[k];
                bool after = row->row > V.cr || (row->row == V.cr && c->col > V.cc);
                if (pass == 0 && !after) continue;
                char t[512];
                bool right;
                of_cell_display(V.b, c, t, sizeof t, &right);
                /* case-insensitive, ASCII */
                size_t nl = strlen(s_find);
                bool hit = false;
                for (char *p = t; *p && !hit; p++) hit = !strncasecmp(p, s_find, nl);
                if (!hit && c->f) hit = strstr(c->f, s_find) != NULL;
                if (hit) {
                    V.ar = V.cr = row->row;
                    V.ac = V.cc = c->col;
                    sel_changed();
                    return;
                }
            }
        }
    }
    of_app_toast(_("No está en esta hoja"));
}

static void find_done(const char *text, void *ud)
{
    (void)ud;
    snprintf(s_find, sizeof s_find, "%s", text);
    find_next();
}

static void find_open(void)
{
    ofu_prompt(_("Buscar en la hoja"), s_find, _("Buscar"), find_done, NULL);
}

/* save as */
static int s_as_fmt;
#define AS_PDF 99

static void save_as_done(const char *text, void *ud)
{
    (void)ud;
    static const char *const EXT[] = { "", "csv", "tsv", "xlsx", "ods" };
    if (!text[0] || strchr(text, '/')) return;
    char dir[300];
    snprintf(dir, sizeof dir, "%s", V.path);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0;
    char p[320];
    const char *ext = s_as_fmt == AS_PDF ? "pdf" : EXT[s_as_fmt];
    bool has = !strcasecmp(of_ext(text), ext);
    snprintf(p, sizeof p, "%.200s/%.100s%s%s", dir, text, has ? "" : ".", has ? "" : ext);
    if (of_file_exists(p)) { of_app_toast(_("Ya hay un archivo con ese nombre")); return; }
    char err[64];
    if (s_as_fmt == AS_PDF) {
        /* every sheet, as tables on A4 */
        if (!of_pdf_write_book(V.b, -1, p, of_basename(V.path), err, sizeof err)) { of_app_toast(_("No se pudo guardar")); return; }
        of_app_toast(_("Guardado como PDF"));
        return;
    }
    if (!of_book_save(V.b, s_as_fmt, V.si, p, err, sizeof err)) { of_app_toast(_("No se pudo guardar")); return; }
    int lose = of_book_save_losses(V.b, s_as_fmt);
    of_recent_add(p);
    if (lose & OF_LOSE_SHEETS) of_app_toast(_("Guardado. El CSV tiene sólo la hoja que se veía"));
    else if (lose & OF_LOSE_FORMULAS) of_app_toast(_("Guardado. En el CSV las fórmulas quedaron como valores"));
    else of_app_toast(_("Guardado"));
    /* the book stays as it was: the open file is still the original */
}

static void save_as_pick(int i, void *ud)
{
    (void)ud;
    static const int FMTS[] = { OF_FMT_XLSX, OF_FMT_ODS, OF_FMT_CSV, AS_PDF };
    s_as_fmt = FMTS[i];
    char base[128];
    snprintf(base, sizeof base, "%s", of_basename(V.path));
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = 0;
    ofu_prompt(_("Guardar una copia como"), base, _("Guardar"), save_as_done, NULL);
}

static void save_as_open(void)
{
    ofu_item_t items[4] = {
        { _("Planilla de Excel (.xlsx)"), OF_IC_SHEET, false, V.b->fmt == OF_FMT_XLSX },
        { _("Planilla de OpenDocument (.ods)"), OF_IC_SHEET, false, V.b->fmt == OF_FMT_ODS },
        { _("Datos separados por comas (.csv)"), OF_IC_DOC, false, V.b->fmt == OF_FMT_CSV },
        { _("PDF para imprimir o mandar (.pdf)"), OF_IC_DOC, false, false },
    };
    ofu_sheet(_("Guardar una copia como"), items, 4, save_as_pick, NULL);
}

static void help_open(void)
{
    int lang = of_locale_get_lang();
    of_sb_t sb = { 0 };
    int n = of_formula_fn_count();
    for (int i = 0; i < n; i++) {
        if (i) of_sb_puts(&sb, ", ");
        of_sb_puts(&sb, of_formula_fn_name(i, lang));
    }
    int32_t cw;
    lv_obj_t *card = ofu_sheet_custom(_("Fórmulas que la app calcula"), &cw);
    lv_obj_t *t = ofu_text(card, sb.s ? sb.s : "", ofu_font(0, OF_SZ_S), OFT.text);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, cw - 24);
    lv_obj_set_style_pad_all(t, 12, 0);
    lv_obj_t *t2 = ofu_text(card, _("Se escriben con = adelante, y con ; o , entre los argumentos. En el archivo quedan con los nombres en inglés, como los guarda Excel. Las que la app no tiene conservan el valor que traía el archivo."),
                            ofu_font(0, OF_SZ_XS), OFT.dim);
    lv_label_set_long_mode(t2, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t2, cw - 24);
    lv_obj_set_style_pad_all(t2, 12, 0);
    of_sb_free(&sb);
}

static void sheet_rename_done(const char *text, void *ud)
{
    (void)ud;
    if (!text[0]) return;
    if (V.b->fmt == OF_FMT_XLSX && V.b->zip.n) {
        of_app_toast(_("Cambiar el nombre de una hoja de un .xlsx todavía no se puede"));
        return;
    }
    of_free(SH()->name);
    SH()->name = of_strdup(text);
    SH()->dirty = true;
    V.modified = true;
    relayout();
}

static void menu_pick(int i, void *ud)
{
    (void)ud;
    switch (i) {
    case 0: of_sheet_save(); break;
    case 1: save_as_open(); break;
    case 2: fmt_open(); break;
    case 3: sort_sel(false); break;
    case 4: sort_sel(true); break;
    case 5: filter_toggle(); break;
    case 6: autofit(); break;
    case 7: freeze_here(); break;
    case 8: do_copy(false); break;
    case 9: do_copy(true); break;
    case 10: do_paste(); break;
    case 11: clear_selection_cells(); break;
    case 12: ofu_prompt(_("Ir a la celda"), "", _("Ir"), goto_done, NULL); break;
    case 13: find_open(); break;
    case 14: help_open(); break;
    case 15: {
        char base[128];
        snprintf(base, sizeof base, "%s", SH()->name ? SH()->name : "");
        ofu_prompt(_("Nombre de la hoja"), base, _("Cambiar"), sheet_rename_done, NULL);
        break;
    }
    }
}

static void menu_open(void)
{
    if (V.editing) edit_commit(0, 0);
    ofu_item_t items[16] = {
        { _("Guardar"), OF_IC_SAVE, false, false },
        { _("Guardar una copia como..."), OF_IC_COPY, false, false },
        { _("Formato de número..."), OF_IC_TYPE, false, false },
        { _("Ordenar de menor a mayor"), OF_IC_SORT, false, false },
        { _("Ordenar de mayor a menor"), OF_IC_SORT, false, false },
        { V.filtered ? _("Sacar el filtro") : _("Filtrar por este valor"), OF_IC_FILTER, false, V.filtered },
        { _("Ancho de columna justo"), OF_IC_HANDLE, false, false },
        { (SH()->freeze_r || SH()->freeze_c) ? _("Liberar filas y columnas") : _("Inmovilizar hasta acá"), OF_IC_PIN, false,
          SH()->freeze_r || SH()->freeze_c },
        { _("Copiar"), OF_IC_COPY, false, false },
        { _("Cortar"), OF_IC_COPY, false, false },
        { _("Pegar"), OF_IC_COPY, false, false },
        { _("Borrar el contenido"), OF_IC_TRASH, true, false },
        { _("Ir a la celda..."), OF_IC_SEARCH, false, false },
        { _("Buscar..."), OF_IC_SEARCH, false, false },
        { _("Fórmulas que calcula"), OF_IC_INFO, false, false },
        { _("Cambiar el nombre de la hoja"), OF_IC_TYPE, false, false },
    };
    ofu_sheet(NULL, items, 16, menu_pick, NULL);
}

/* -------------------------------------------------------------------------- */
/* Saving                                                                      */
/* -------------------------------------------------------------------------- */

static void save_as_open(void);

bool of_sheet_save(void)
{
    if (V.editing) edit_commit(0, 0);
    if (V.b->fmt == OF_FMT_XLS) {
        /* the old binary is read, not written: a copy */
        of_app_toast(_("Un .xls no se puede guardar: se guarda una copia como xlsx u ods"));
        save_as_open();
        return false;
    }
    char err[64] = "";
    int fmt = V.b->fmt;
    if (fmt == OF_FMT_NONE) fmt = of_fmt_from_ext(of_ext(V.path));
    if (!of_book_save(V.b, fmt, V.si, V.path, err, sizeof err)) {
        of_app_toast(_("No se pudo guardar"));
        return false;
    }
    V.modified = false;
    int lose = of_book_save_losses(V.b, fmt);
    if (lose & OF_LOSE_FORMULAS) of_app_toast(_("Guardado. En el CSV las fórmulas quedaron como valores"));
    else if (V.b->any_unknown_f) of_app_toast(_("Guardado. Hay fórmulas que la app no calcula: el programa de la computadora las recalcula al abrir"));
    else of_app_toast(_("Guardado"));
    refresh();
    return true;
}

/* The system is closing the app with changes: they go to a copy next to the
 * file, never over it without asking. */
bool of_sheet_save_recovery(void)
{
    if (!V.b) return false;
    char p[340], err[64];
    const char *e = of_ext(V.path);
    int bl = (int)strlen(V.path) - (e[0] ? (int)strlen(e) + 1 : 0);
    bool xls = V.b->fmt == OF_FMT_XLS;
    snprintf(p, sizeof p, "%.*s (sin guardar).%s", bl > 280 ? 280 : bl, V.path, xls ? "xlsx" : e[0] ? e : "xlsx");
    int fmt = xls ? OF_FMT_XLSX : V.b->fmt ? V.b->fmt : of_fmt_from_ext(e);
    return of_book_save(V.b, fmt, V.si, p, err, sizeof err);
}

bool of_sheet_modified(void)
{
    return V.b && V.modified;
}

void of_sheet_tick(void)
{
}

/* -------------------------------------------------------------------------- */
/* Building the screen                                                         */
/* -------------------------------------------------------------------------- */

static void back_cb(lv_event_t *e)
{
    (void)e;
    if (V.editing) { edit_commit(0, 0); return; }
    of_app_back_from_doc();
}

static void undo_cb(lv_event_t *e) { (void)e; do_undo(); }
static void redo_cb(lv_event_t *e) { (void)e; do_redo(); }
static void save_cb(lv_event_t *e) { (void)e; of_sheet_save(); }
static void more_cb(lv_event_t *e) { (void)e; menu_open(); }
static void fref_cb(lv_event_t *e) { (void)e; if (!V.editing) ofu_prompt(_("Ir a la celda"), "", _("Ir"), goto_done, NULL); }
static void ftext_cb(lv_event_t *e) { (void)e; if (!V.editing) edit_begin(NULL, false); }
static void ok_cb(lv_event_t *e) { (void)e; edit_commit(0, 0); }

static void tab_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (V.editing) edit_commit(0, 0);
    if (i == V.si) return;
    V.si = i;
    V.sx = V.sy = 0;
    V.ar = V.cr = V.ac = V.cc = 0;
    if (V.filtered) {
        for (int s = 0; s < V.b->nsh; s++)
            for (int r = 0; r < V.b->sh[s].nrows; r++) V.b->sh[s].rows[r].hidden = 0;
        V.filtered = false;
    }
    relayout();
}

/* Lays everything out for the current size, keyboard and sheet. */
static void relayout(void)
{
    if (!V.root) return;
    V.W = lv_obj_get_width(V.root);
    V.H = lv_obj_get_height(V.root);
    bool land = V.W > V.H;
    int32_t top_h = land ? 76 : 88, fbar_h = land ? 66 : 76, tabs_h = land ? 60 : 68;
    int32_t kbh = V.kb ? lv_obj_get_height(V.kb) : 0;
    if (land && V.kb) tabs_h = 0;                   /* lying down, the keyboard needs the room */
    lv_obj_set_pos(V.top, 0, 0);
    lv_obj_set_size(V.top, V.W, top_h);
    lv_obj_set_pos(V.fbar, 0, top_h);
    lv_obj_set_size(V.fbar, V.W, fbar_h);
    int32_t gy = top_h + fbar_h;
    V.gw = V.W;
    V.gh = V.H - gy - tabs_h - kbh;
    lv_obj_set_pos(V.grid, 0, gy);
    lv_obj_set_size(V.grid, V.gw, V.gh);
    lv_obj_update_layout(V.grid);
    sync_origin();
    if (tabs_h) {
        lv_obj_remove_flag(V.tabs, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(V.tabs, 0, V.H - tabs_h - kbh);
        lv_obj_set_size(V.tabs, V.W, tabs_h);
    } else {
        lv_obj_add_flag(V.tabs, LV_OBJ_FLAG_HIDDEN);
    }
    /* the sheet tabs */
    lv_obj_t *row = lv_obj_get_child(V.tabs, 0);
    lv_obj_clean(row);
    for (int i = 0; i < V.b->nsh; i++) {
        if (V.b->sh[i].state == 2) continue;        /* a chart sheet */
        lv_obj_t *p = ofu_pill(row, V.b->sh[i].name ? V.b->sh[i].name : "?", tab_cb, (void *)(intptr_t)i);
        lv_obj_set_height(p, 52);
        if (i == V.si) lv_obj_add_state(p, LV_STATE_CHECKED);
        if (V.b->sh[i].state == 1) lv_obj_set_style_text_opa(p, LV_OPA_50, 0);
    }
    int32_t sw = land ? V.W / 2 : V.W * 45 / 100;
    lv_obj_set_width(row, V.W - sw - 24);
    lv_obj_set_width(V.status, sw);
    lv_obj_align(V.status, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_width(V.title, V.W - 4 * 84 - 100);
    geometry();
    clamp_scroll();
    refresh();
}

bool of_sheet_open(lv_obj_t *parent, const char *path, of_book_t *b)
{
    if (b) {
        /* a new book */
        of_sheet_close_view();
        memset(&V, 0, sizeof V);
        V.b = b;
        snprintf(V.path, sizeof V.path, "%s", path);
        int32_t z = 1;
        aos_hal_pref_get_i32("of_zoom", &z);
        V.z = z >= 0 && z < NZOOM ? (int)z : 1;
        for (int i = 0; i < b->nsh; i++)
            if (b->sh[i].state == 0) { V.si = i; break; }
        if (b->any_unknown_f)
            of_app_toast(_("Hay fórmulas con funciones que la app no tiene: se ve el valor que trae el archivo"));
    } else if (!V.b) {
        return false;
    }
    stop_fling();
    V.root = parent;
    V.kb = NULL;
    V.editing = false;
    ofu_ui_root = parent;
    lv_obj_set_style_bg_color(parent, OFT.bg, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* the bar */
    V.top = ofu_box(parent, 10, 10);
    lv_obj_set_style_bg_color(V.top, OFT.surface, 0);
    lv_obj_set_style_bg_opa(V.top, LV_OPA_COVER, 0);
    lv_obj_t *bk = ofu_icon_btn(V.top, OF_IC_BACK, 84, back_cb, NULL);
    lv_obj_align(bk, LV_ALIGN_LEFT_MID, 4, 0);
    V.title = ofu_text(V.top, of_basename(V.path), ofu_font(OF_ST_BOLD, OF_SZ_M), OFT.text);
    lv_label_set_long_mode(V.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(V.title, LV_ALIGN_LEFT_MID, 92, 0);
    lv_obj_t *mo = ofu_icon_btn(V.top, OF_IC_MORE, 84, more_cb, NULL);
    lv_obj_align(mo, LV_ALIGN_RIGHT_MID, -4, 0);
    V.b_save = ofu_icon_btn(V.top, OF_IC_SAVE, 84, save_cb, NULL);
    lv_obj_align(V.b_save, LV_ALIGN_RIGHT_MID, -88, 0);
    V.b_redo = ofu_icon_btn(V.top, OF_IC_REDO, 84, redo_cb, NULL);
    lv_obj_align(V.b_redo, LV_ALIGN_RIGHT_MID, -172, 0);
    V.b_undo = ofu_icon_btn(V.top, OF_IC_UNDO, 84, undo_cb, NULL);
    lv_obj_align(V.b_undo, LV_ALIGN_RIGHT_MID, -256, 0);

    /* the formula bar */
    V.fbar = ofu_box(parent, 10, 10);
    lv_obj_set_style_bg_color(V.fbar, OFT.surface, 0);
    lv_obj_set_style_bg_opa(V.fbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(V.fbar, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(V.fbar, OFT.hair, 0);
    lv_obj_set_style_border_width(V.fbar, 1, 0);
    V.fref = lv_obj_create(V.fbar);
    lv_obj_remove_style_all(V.fref);
    lv_obj_set_size(V.fref, 150, 56);
    lv_obj_set_style_radius(V.fref, 14, 0);
    lv_obj_set_style_bg_color(V.fref, OFT.surface2, 0);
    lv_obj_set_style_bg_opa(V.fref, LV_OPA_COVER, 0);
    lv_obj_add_flag(V.fref, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(V.fref, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(V.fref, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *rl = ofu_text(V.fref, "A1", ofu_font(OF_ST_BOLD, OF_SZ_S), OFT.text);
    lv_label_set_long_mode(rl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(rl, 136);
    lv_obj_set_style_text_align(rl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(V.fref, fref_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fx = ofu_text(V.fbar, "fx", ofu_font(0, OF_SZ_S), OFT.dim);
    lv_obj_align(fx, LV_ALIGN_LEFT_MID, 176, 0);
    int32_t fw = lv_obj_get_width(parent) - 230 - 84;
    V.ftext = ofu_text(V.fbar, "", ofu_font(0, OF_SZ_M), OFT.text);
    lv_label_set_long_mode(V.ftext, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(V.ftext, fw, 40);
    lv_obj_align(V.ftext, LV_ALIGN_LEFT_MID, 220, 0);
    lv_obj_add_flag(V.ftext, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(V.ftext, 18);
    lv_obj_add_event_cb(V.ftext, ftext_cb, LV_EVENT_CLICKED, NULL);
    V.ta = lv_textarea_create(V.fbar);
    lv_textarea_set_one_line(V.ta, true);
    lv_obj_set_size(V.ta, fw, 60);
    lv_obj_align(V.ta, LV_ALIGN_LEFT_MID, 214, 0);
    lv_obj_set_style_text_font(V.ta, ofu_font(0, OF_SZ_M), 0);
    lv_obj_set_style_bg_color(V.ta, OFT.surface2, 0);
    lv_obj_set_style_text_color(V.ta, OFT.text, 0);
    lv_obj_set_style_border_width(V.ta, 0, 0);
    lv_obj_set_style_radius(V.ta, 12, 0);
    lv_obj_set_style_pad_ver(V.ta, 8, 0);
    lv_obj_add_flag(V.ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *ok = ofu_icon_btn(V.fbar, OF_IC_CHECK, 72, ok_cb, NULL);
    lv_obj_align(ok, LV_ALIGN_RIGHT_MID, -6, 0);
    ofu_icon_color(ok, OFT.accent);

    /* the grid */
    V.grid = ofu_box(parent, 10, 10);
    lv_obj_add_flag(V.grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(V.grid, grid_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    aos_gesture_attach(V.grid, 0, gesture_cb, NULL);

    /* the tabs and the status */
    V.tabs = ofu_box(parent, 10, 10);
    lv_obj_set_style_bg_color(V.tabs, OFT.surface, 0);
    lv_obj_set_style_bg_opa(V.tabs, LV_OPA_COVER, 0);
    lv_obj_t *row = lv_obj_create(V.tabs);
    lv_obj_remove_style_all(row);
    lv_obj_set_height(row, 68);
    lv_obj_align(row, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    V.status = ofu_text(V.tabs, "", ofu_font(0, OF_SZ_XS), OFT.dim);
    lv_label_set_long_mode(V.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(V.status, aos_inter_20.line_height);
    lv_obj_set_style_text_align(V.status, LV_TEXT_ALIGN_RIGHT, 0);

    grid_target();
    relayout();
    return true;
}

bool of_sheet_back(void)
{
    if (V.editing) { edit_cancel(); return true; }
    if (V.ar != V.cr || V.ac != V.cc) { V.ar = V.cr; V.ac = V.cc; refresh(); return true; }
    return false;
}

void of_sheet_close_view(void)
{
    stop_fling();
    for (int i = 0; i < V.nundo; i++) entry_free(&V.undo[i]);
    for (int i = 0; i < V.nredo; i++) entry_free(&V.redo[i]);
    entry_free(&V.cur);
    of_free(V.cx);
    of_free(V.ry);
    of_free(V.clip);
    if (V.b) of_book_free(V.b);
    memset(&V, 0, sizeof V);
}
