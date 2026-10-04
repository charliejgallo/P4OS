/*
 * P4OS - Notes: the rich-text editor.
 *
 * LVGL has no editor for text with styles in it (a textarea is one font,
 * one colour), so this lays the note out itself. One object, as tall as the
 * note, inside a scrolling container; the blocks are measured glyph by glyph
 * with the font each byte's style word picks, wrapped at spaces, and drawn
 * in that object's draw event as runs of lv_draw_label, one per stretch of
 * equal style on a line. Knowing where every glyph is, a tap becomes a
 * caret position, a drag a selection, and a line its bullet.
 *
 * Measuring and drawing agree because both walk a line the same way: a run
 * is drawn by one lv_draw_label, which kerns each glyph against the next one
 * IN THE RUN and the last one against nothing, and line_x() measures it like
 * that too (nt_font has kerning; LVGL's own label does the same).
 *
 * Undo keeps whole copies of the note (nt_frag_whole): a note is a few KB,
 * and a copy per burst of typing is simpler than reversible operations and
 * cannot get out of step with them. Typing coalesces for 1.5 s or 40 bytes.
 */
#include "nt.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"

#include <string.h>

#define PAD_TOP      20
#define PAD_BOTTOM   240
#define INDENT_STEP  44
#define UNDO_MAX     60
#define UNDO_BYTES   (4 * 1024 * 1024)

enum { OP_NONE, OP_TYPE, OP_DELETE, OP_OTHER };

typedef struct {
    nt_frag_t *f;
    int        b, p;
} snap_t;

static struct {
    nt_doc_t   *doc;
    nt_ed_cbs_t cbs;
    lv_obj_t   *sc, *content, *caret, *h[2], *menu;
    lv_timer_t *blink;
    int32_t     W, pad_x;
    int         cb, cp;             /* caret */
    int         ab, ap;             /* anchor of the selection */
    bool        sel;
    bool        focus;
    uint16_t    pending;
    bool        has_pending;
    /* touch */
    lv_point_t  down;
    uint32_t    last_click;
    lv_point_t  last_pt;
    int         clicks;
    bool        dragging, drag_moved;
    int         handle;             /* which handle is held, -1 none */
    bool        long_pressed;
    /* undo */
    snap_t      undo[UNDO_MAX], redo[UNDO_MAX];
    int         nu, nr;
    int         last_op;
    uint32_t    last_ms;
    int         group;
} E;

static nt_frag_t *s_clip;

static void refresh(bool all);
static void caret_moved(void);
static void show_menu(bool show);

/* -------------------------------------------------------------------------- */
/* UTF-8                                                                       */
/* -------------------------------------------------------------------------- */

static uint32_t u8dec(const char *s, int len, int *i)
{
    unsigned char c = (unsigned char)s[*i];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (*i + n > len) n = 1;
    uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)s[*i + k] & 0x3F);
    *i += n;
    return cp;
}

static int u8prev(const char *s, int i)
{
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static int u8next(const char *s, int len, int i)
{
    if (i >= len) return len;
    u8dec(s, len, &i);
    return i;
}

/* -------------------------------------------------------------------------- */
/* Styles and metrics                                                          */
/* -------------------------------------------------------------------------- */

static bool heading(int t)
{
    return t == BT_TITLE || t == BT_H1 || t == BT_H2 || t == BT_H3;
}

static int base_size(const nt_blk_t *b)
{
    switch (b->type) {
    case BT_TITLE: case BT_H1: return NT_SZ_XL;
    case BT_H2: return NT_SZ_L;
    default: return NT_SZ_M;
    }
}

/* What a byte's style word becomes once its block has its say. */
static uint16_t eff(const nt_blk_t *b, uint16_t a)
{
    if (b->type == BT_TITLE) return (uint16_t)(A_B | (NT_SZ_XL << A_SZ_SHIFT));
    if (heading(b->type)) a = (uint16_t)((a & ~A_SZ_MASK) | (base_size(b) << A_SZ_SHIFT) | A_B);
    if (b->type == BT_QUOTE) a |= A_I;
    if (b->type == BT_CHECK && b->checked) a |= A_S;
    return a;
}

static const lv_font_t *font_of(uint16_t a)
{
    return nt_font(((a & A_B) ? NT_ST_BOLD : 0) | ((a & A_I) ? NT_ST_ITALIC : 0), A_SZ(a));
}

static int32_t asc_of(const lv_font_t *f)
{
    return f->line_height - f->base_line;
}

static bool is_list(int t)
{
    return t == BT_BULLET || t == BT_NUM || t == BT_CHECK;
}

static int32_t gutter(const nt_blk_t *b)
{
    int px = nt_font_px(base_size(b));
    switch (b->type) {
    case BT_BULLET: case BT_CHECK: return px * 2;
    case BT_NUM: return px * 2 + px / 3;
    case BT_QUOTE: return 30;
    default: return 0;
    }
}

static int32_t text_x(const nt_blk_t *b)
{
    if (b->type == BT_TITLE || b->type == BT_RULE) return E.pad_x;
    return E.pad_x + b->indent * INDENT_STEP + gutter(b);
}

static int32_t avail(const nt_blk_t *b)
{
    int32_t w = E.W - text_x(b) - E.pad_x;
    return w < 60 ? 60 : w;
}

static uint32_t peek(const nt_blk_t *b, int i)
{
    int k = i;
    return k < b->len ? u8dec(b->txt, b->len, &k) : 0;
}

/* Width of the glyph at i, kerned against the next one when that one is in
 * the same run and before 'end'. */
static int32_t glyph_w(const nt_blk_t *b, int i, int end, int *next)
{
    int j = i;
    uint32_t cp = u8dec(b->txt, b->len, &j);
    uint16_t a = eff(b, b->at[i]);
    uint32_t nx = (j < end && eff(b, b->at[j]) == a) ? peek(b, j) : 0;
    *next = j;
    if (cp == '\t') cp = ' ';
    return (int32_t)lv_font_get_glyph_width(font_of(a), cp, nx);
}

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */
/* -------------------------------------------------------------------------- */

static void add_line(nt_blk_t *b, int start, int end)
{
    if (b->nlines + 1 > b->lcap) {
        int cap = b->lcap ? b->lcap * 2 : 4;
        nt_line_t *q = nt_realloc(b->lines, cap * sizeof(nt_line_t));
        if (!q) return;
        b->lines = q;
        b->lcap = cap;
    }
    int32_t asc = 0, desc = 0;
    uint16_t last = 0xFFFF;
    for (int i = start; i < end; i++) {
        uint16_t a = eff(b, b->at[i]);
        if (a == last) continue;
        last = a;
        const lv_font_t *f = font_of(a);
        if (asc_of(f) > asc) asc = asc_of(f);
        if (f->base_line > desc) desc = f->base_line;
    }
    if (start == end) {
        uint16_t a = eff(b, b->len ? b->at[start < b->len ? start : b->len - 1] : 0);
        if (b == &E.doc->b[E.cb] && E.has_pending && b->len == 0) a = eff(b, E.pending);
        const lv_font_t *f = font_of(a);
        asc = asc_of(f);
        desc = f->base_line;
    }
    nt_line_t *l = &b->lines[b->nlines++];
    l->start = start;
    l->end = end;
    l->asc = asc;
    l->h = asc + desc + (asc + desc) / 7;
    l->y = b->nlines > 1 ? b->lines[b->nlines - 2].y + b->lines[b->nlines - 2].h : 0;
}

static void layout_block(nt_blk_t *b)
{
    b->nlines = 0;
    if (b->type == BT_RULE) {
        b->h = 36;
        add_line(b, 0, 0);
        return;
    }
    int32_t aw = avail(b);
    int start = 0;
    for (;;) {
        int32_t x = 0;
        int i = start, brk = -1, end = b->len;
        while (i < b->len) {
            int j;
            int32_t w = glyph_w(b, i, b->len, &j);
            if (b->txt[i] == ' ') {     /* spaces hang past the edge */
                x += w;
                i = j;
                brk = i;
                continue;
            }
            if (x + w > aw && i > start) {
                end = brk > start ? brk : i;
                break;
            }
            x += w;
            i = j;
        }
        add_line(b, start, end);
        if (end >= b->len) break;
        start = end;
    }
    b->h = 0;
    for (int k = 0; k < b->nlines; k++) b->h += b->lines[k].h;
}

static int32_t space_before(int i)
{
    const nt_blk_t *b = &E.doc->b[i];
    const nt_blk_t *p = i > 0 ? &E.doc->b[i - 1] : NULL;
    if (!p) return 0;
    switch (b->type) {
    case BT_H1: return p->type == BT_TITLE ? 4 : 26;
    case BT_H2: return p->type == BT_TITLE ? 4 : 20;
    case BT_H3: return p->type == BT_TITLE ? 4 : 14;
    case BT_RULE: return 4;
    case BT_QUOTE: return p->type == BT_QUOTE ? 0 : 8;
    default: break;
    }
    if (is_list(b->type)) return is_list(p->type) ? 2 : 8;
    if (p->type == BT_TITLE) return 6;
    return is_list(p->type) || p->type == BT_QUOTE ? 8 : 4;
}

static void position_all(void)
{
    int32_t y = PAD_TOP;
    for (int i = 0; i < E.doc->n; i++) {
        nt_blk_t *b = &E.doc->b[i];
        y += space_before(i);
        b->y = y;
        y += b->h;
        if (b->type == BT_TITLE) y += 12;
    }
    lv_obj_set_height(E.content, y + PAD_BOTTOM);
}

static void layout_all(void)
{
    for (int i = 0; i < E.doc->n; i++) layout_block(&E.doc->b[i]);
    nt_doc_number(E.doc);
    position_all();
}

/* x of byte 'pos' on line 'l' of b, from the text's left edge */
static int32_t line_x(const nt_blk_t *b, const nt_line_t *l, int pos)
{
    int32_t x = 0;
    int i = l->start;
    while (i < pos && i < l->end) {
        int j;
        x += glyph_w(b, i, l->end, &j);
        i = j;
    }
    return x;
}

static int line_of(const nt_blk_t *b, int pos)
{
    for (int k = 0; k < b->nlines; k++) {
        if (pos < b->lines[k].end || k == b->nlines - 1) return k;
        if (pos == b->lines[k].end && b->lines[k].end == b->lines[k].start) return k;
    }
    return 0;
}

/* The byte nearest to x on line k. */
static int hit_line(const nt_blk_t *b, int k, int32_t x)
{
    const nt_line_t *l = &b->lines[k];
    int32_t cx = 0;
    int i = l->start;
    while (i < l->end) {
        int j;
        int32_t w = glyph_w(b, i, l->end, &j);
        if (x < cx + w / 2) return i;
        cx += w;
        i = j;
    }
    /* past the end of a wrapped line: before its hanging space, or the
     * caret would show on the next line */
    if (k < b->nlines - 1 && l->end > l->start && b->txt[l->end - 1] == ' ') return l->end - 1;
    return l->end;
}

/* Content coordinates -> (block, byte). */
static void hit(int32_t x, int32_t y, int *pb, int *pp)
{
    nt_doc_t *d = E.doc;
    int bi = d->n - 1;
    for (int i = 0; i < d->n; i++) {
        if (y < d->b[i].y + d->b[i].h + 4) { bi = i; break; }
    }
    nt_blk_t *b = &d->b[bi];
    int k = 0;
    while (k < b->nlines - 1 && y >= b->y + b->lines[k + 1].y) k++;
    *pb = bi;
    *pp = hit_line(b, k, x - text_x(b));
}

static void caret_rect(int bi, int pos, int32_t *x, int32_t *y, int32_t *h)
{
    nt_blk_t *b = &E.doc->b[bi];
    int k = line_of(b, pos);
    const nt_line_t *l = &b->lines[k];
    uint16_t a;
    if (E.has_pending && !E.sel) a = eff(b, E.pending);
    else if (pos > l->start) a = eff(b, b->at[u8prev(b->txt, pos)]);
    else if (pos < b->len) a = eff(b, b->at[pos]);
    else a = eff(b, 0);
    const lv_font_t *f = font_of(a);
    *x = text_x(b) + line_x(b, l, pos);
    *h = f->line_height;
    *y = b->y + l->y + l->asc - asc_of(f);
    if (b->type == BT_RULE) { *x = E.pad_x; *y = b->y + 4; *h = 28; }
}

/* -------------------------------------------------------------------------- */
/* Drawing                                                                     */
/* -------------------------------------------------------------------------- */

static void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                 lv_color_t c, lv_opa_t opa, int32_t r)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = opa;
    d.radius = r;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

static void sel_order(int *b0, int *p0, int *b1, int *p1)
{
    if (E.ab < E.cb || (E.ab == E.cb && E.ap <= E.cp)) {
        *b0 = E.ab; *p0 = E.ap; *b1 = E.cb; *p1 = E.cp;
    } else {
        *b0 = E.cb; *p0 = E.cp; *b1 = E.ab; *p1 = E.ap;
    }
}

static void draw_marker(lv_layer_t *layer, const nt_blk_t *b, int32_t ox, int32_t oy)
{
    if (!b->nlines) return;
    const nt_line_t *l = &b->lines[0];
    int px = nt_font_px(base_size(b));
    int32_t gx = ox + E.pad_x + b->indent * INDENT_STEP;
    int32_t base = oy + b->y + l->y + l->asc;
    int32_t mid = base - px * 36 / 100;
    lv_color_t c = NT.dim;
    if (b->len) c = nt_fg_color(A_FG(b->at[0]));
    if (b->type == BT_BULLET) {
        lv_color_t bc = A_FG(b->len ? b->at[0] : 0) ? c : lv_color_mix(NT.text, NT.dim, 200);
        nt_draw_bullet(layer, b->style, gx + px * 4 / 5, mid, px * 21 / 100, bc);
    } else if (b->type == BT_NUM) {
        char lab[24];
        nt_num_label(b->style, b->num, lab, sizeof lab);
        const lv_font_t *f = nt_font(0, base_size(b));
        int32_t w = nt_text_width(lab, f);
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.font = f;
        d.color = A_FG(b->len ? b->at[0] : 0) ? c : NT.dim;
        d.text = lab;
        d.text_local = 1;
        int32_t x2 = ox + text_x(b) - px / 3;
        lv_area_t a = { x2 - w, base - asc_of(f), x2 + 2, base - asc_of(f) + f->line_height };
        lv_draw_label(layer, &d, &a);
    } else if (b->type == BT_CHECK) {
        int32_t s = px * 92 / 100;
        lv_area_t a = { gx + px / 4, mid - s / 2, gx + px / 4 + s, mid + s / 2 };
        nt_draw_checkbox(layer, &a, b->checked, false, NT.dim, NT.accent);
        if (b->style) {
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            d.bg_color = nt_prio_color(b->style);
            d.radius = LV_RADIUS_CIRCLE;
            lv_area_t dot = { a.x2 - 4, a.y1 - 6, a.x2 + 6, a.y1 + 4 };
            lv_draw_rect(layer, &d, &dot);
        }
    } else if (b->type == BT_QUOTE) {
        fill(layer, gx + 4, oy + b->y - 2, gx + 9, oy + b->y + b->h + 2, NT.accent, LV_OPA_COVER, 3);
    }
}

static void draw_block(lv_layer_t *layer, int bi, int32_t ox, int32_t oy,
                       int b0, int p0, int b1, int p1)
{
    nt_blk_t *b = &E.doc->b[bi];
    if (b->type == BT_RULE) {
        int32_t y = oy + b->y + b->h / 2;
        fill(layer, ox + E.pad_x, y - 1, ox + E.W - E.pad_x, y + 1, NT.hair, LV_OPA_COVER, 1);
        if (E.sel && bi >= b0 && bi <= b1 && !(bi == b1 && p1 == 0))
            fill(layer, ox + E.pad_x, oy + b->y, ox + E.W - E.pad_x, oy + b->y + b->h, NT.sel, LV_OPA_30, 4);
        return;
    }
    draw_marker(layer, b, ox, oy);
    int32_t tx = ox + text_x(b);
    for (int k = 0; k < b->nlines; k++) {
        const nt_line_t *l = &b->lines[k];
        int32_t ly = oy + b->y + l->y;
        int32_t base = ly + l->asc;
        /* selection behind the text */
        if (E.sel && bi >= b0 && bi <= b1) {
            int s = bi == b0 ? p0 : 0, e = bi == b1 ? p1 : b->len;
            if (s < l->end || (l->start == l->end && s <= l->start)) {
                int ls = s > l->start ? s : l->start;
                int le = e < l->end ? e : l->end;
                if (le >= ls && !(e <= l->start && l->end > l->start)) {
                    int32_t x1 = tx + line_x(b, l, ls);
                    int32_t x2 = tx + line_x(b, l, le);
                    if (bi != b1 || e > l->end || (k < b->nlines - 1 && e >= l->end)) x2 += 10;
                    if (x2 > x1) fill(layer, x1, ly, x2, ly + l->h, NT.sel, LV_OPA_40, 2);
                }
            }
        }
        /* runs */
        int i = l->start;
        int32_t x = 0;
        while (i < l->end) {
            uint16_t a = eff(b, b->at[i]);
            int j = i;
            int32_t w = 0;
            while (j < l->end && eff(b, b->at[j]) == a) {
                int n;
                w += glyph_w(b, j, l->end, &n);
                j = n;
            }
            const lv_font_t *f = font_of(a);
            int32_t top = base - asc_of(f);
            if (A_HL(a)) fill(layer, tx + x - 1, ly + 1, tx + x + w + 1, ly + l->asc + (l->h - l->asc) * 2 / 3,
                              nt_hl_color(A_HL(a)), LV_OPA_COVER, 4);
            lv_draw_label_dsc_t d;
            lv_draw_label_dsc_init(&d);
            d.font = f;
            d.color = (b->type == BT_CHECK && b->checked) ? NT.dim :
                      b->type == BT_QUOTE && !A_FG(a) ? lv_color_mix(NT.text, NT.dim, 160) :
                      nt_fg_color(A_FG(a));
            d.text = b->txt + i;
            d.text_length = (uint32_t)(j - i);
            d.text_local = 1;
            d.flag = LV_TEXT_FLAG_EXPAND;
            d.decor = (lv_text_decor_t)(((a & A_U) ? LV_TEXT_DECOR_UNDERLINE : 0) |
                                        ((a & A_S) ? LV_TEXT_DECOR_STRIKETHROUGH : 0));
            lv_area_t ar = { tx + x, top, tx + x + w + 64, top + f->line_height };
            lv_draw_label(layer, &d, &ar);
            if (nt_font_faux_bold(((a & A_B) ? NT_ST_BOLD : 0) | ((a & A_I) ? NT_ST_ITALIC : 0), A_SZ(a))) {
                ar.x1++;
                ar.x2++;
                d.decor = 0;
                lv_draw_label(layer, &d, &ar);
            }
            x += w;
            i = j;
        }
    }
    /* the title's placeholder */
    if (bi == 0 && b->len == 0) {
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.font = font_of(eff(b, 0));
        d.color = NT.hair;
        d.text = _("Título");
        d.text_local = 1;
        lv_area_t ar = { tx, oy + b->y, tx + E.W, oy + b->y + d.font->line_height };
        lv_draw_label(layer, &d, &ar);
    }
}

static void draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t co;
    lv_obj_get_coords(E.content, &co);
    int32_t top = layer->_clip_area.y1 - co.y1, bot = layer->_clip_area.y2 - co.y1;
    int b0 = 0, p0 = 0, b1 = 0, p1 = 0;
    if (E.sel) sel_order(&b0, &p0, &b1, &p1);
    for (int i = 0; i < E.doc->n; i++) {
        const nt_blk_t *b = &E.doc->b[i];
        if (b->y + b->h + 12 < top) continue;
        if (b->y - 12 > bot) break;
        draw_block(layer, i, co.x1, co.y1, b0, p0, b1, p1);
    }
    /* an empty note's hint, under the title */
    if (E.doc->n == 2 && E.doc->b[1].len == 0 && E.doc->b[1].type == BT_PARA && !(E.focus && E.cb == 1)) {
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.font = nt_font(0, NT_SZ_M);
        d.color = NT.hair;
        d.text = _("Escribí acá");
        d.text_local = 1;
        const nt_blk_t *b = &E.doc->b[1];
        lv_area_t ar = { co.x1 + text_x(b), co.y1 + b->y, co.x2, co.y1 + b->y + d.font->line_height };
        lv_draw_label(layer, &d, &ar);
    }
}

/* -------------------------------------------------------------------------- */
/* Caret, handles, the little menu                                             */
/* -------------------------------------------------------------------------- */

static void blink_cb(lv_timer_t *t)
{
    (void)t;
    if (!E.caret) return;
    bool vis = !lv_obj_has_flag(E.caret, LV_OBJ_FLAG_HIDDEN);
    if (!E.focus || E.sel) {
        if (vis) lv_obj_add_flag(E.caret, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (vis) lv_obj_add_flag(E.caret, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(E.caret, LV_OBJ_FLAG_HIDDEN);
}

static void place_handles(void)
{
    for (int k = 0; k < 2; k++) {
        if (!E.h[k]) continue;
        if (!E.sel) { lv_obj_add_flag(E.h[k], LV_OBJ_FLAG_HIDDEN); continue; }
        int b0, p0, b1, p1;
        sel_order(&b0, &p0, &b1, &p1);
        int32_t x, y, h;
        caret_rect(k ? b1 : b0, k ? p1 : p0, &x, &y, &h);
        /* the knob below the end, above the start (the hit box is 56 x 72) */
        lv_obj_set_pos(E.h[k], x - 28, k ? y : y + h - 72);
        lv_obj_remove_flag(E.h[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(E.h[k], -1);
    }
}

static void place_caret(void)
{
    if (!E.caret) return;
    int32_t x, y, h;
    caret_rect(E.cb, E.cp, &x, &y, &h);
    lv_obj_set_pos(E.caret, x - 1, y);
    lv_obj_set_size(E.caret, 3, h);
    if (E.focus && !E.sel) lv_obj_remove_flag(E.caret, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(E.caret, LV_OBJ_FLAG_HIDDEN);
    if (E.blink) lv_timer_reset(E.blink);
    place_handles();
}

static void handle_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    bool end = o == E.h[1];
    int32_t cx = a.x1 + 28;
    /* stem along the text, knob beyond it */
    if (end) {
        fill(layer, cx - 1, a.y1, cx + 1, a.y1 + 40, NT.sel, LV_OPA_COVER, 0);
        fill(layer, cx - 11, a.y1 + 38, cx + 11, a.y1 + 60, NT.sel, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    } else {
        fill(layer, cx - 11, a.y2 - 60, cx + 11, a.y2 - 38, NT.sel, LV_OPA_COVER, LV_RADIUS_CIRCLE);
        fill(layer, cx - 1, a.y2 - 40, cx + 1, a.y2, NT.sel, LV_OPA_COVER, 0);
    }
}

static void to_content(lv_point_t *p)
{
    lv_area_t co;
    lv_obj_get_coords(E.content, &co);
    p->x -= co.x1;
    p->y -= co.y1;
}

static void autoscroll(const lv_point_t *abs)
{
    lv_area_t sa;
    lv_obj_get_coords(E.sc, &sa);
    if (abs->y < sa.y1 + 70) lv_obj_scroll_by_bounded(E.sc, 0, 24, LV_ANIM_OFF);
    else if (abs->y > sa.y2 - 70) lv_obj_scroll_by_bounded(E.sc, 0, -24, LV_ANIM_OFF);
}

static void handle_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *o = lv_event_get_target(e);
    int k = o == E.h[1];
    if (code == LV_EVENT_PRESSED) {
        E.handle = k;
        show_menu(false);
        lv_obj_remove_flag(E.sc, LV_OBJ_FLAG_SCROLLABLE);
        /* the held end becomes the caret, the other the anchor */
        int b0, p0, b1, p1;
        sel_order(&b0, &p0, &b1, &p1);
        if (k) { E.ab = b0; E.ap = p0; E.cb = b1; E.cp = p1; }
        else   { E.ab = b1; E.ap = p1; E.cb = b0; E.cp = p0; }
    } else if (code == LV_EVENT_PRESSING && E.handle >= 0) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        autoscroll(&p);
        /* the finger is on the knob, below (end) or above (start) the text */
        p.y += k ? -52 : 52;
        to_content(&p);
        int bi, pos;
        hit(p.x, p.y, &bi, &pos);
        if (bi != E.cb || pos != E.cp) {
            if (bi == E.ab && pos == E.ap) return;     /* keep at least one character */
            E.cb = bi;
            E.cp = pos;
            lv_obj_invalidate(E.content);
            place_caret();
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        E.handle = -1;
        lv_obj_add_flag(E.sc, LV_OBJ_FLAG_SCROLLABLE);
        caret_moved();
        show_menu(true);
    }
}

static lv_obj_t *make_handle(void)
{
    lv_obj_t *h = lv_obj_create(E.content);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, 56, 72);
    lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(h, handle_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(h, handle_cb, LV_EVENT_ALL, NULL);
    return h;
}

enum { M_CUT, M_COPY, M_PASTE, M_SELECT, M_ALL, M_FORMAT };

static void menu_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    show_menu(false);
    switch (which) {
    case M_CUT:   nt_ed_copy(true); break;
    case M_COPY:  nt_ed_copy(false); aos_ui_toast(_("Copiado"), 1000); break;
    case M_PASTE: nt_ed_paste(); break;
    case M_ALL:   nt_ed_select_all(); show_menu(true); break;
    case M_SELECT: {
        nt_blk_t *b = &E.doc->b[E.cb];
        int s = E.cp, t = E.cp;
        while (s > 0 && b->txt[u8prev(b->txt, s)] != ' ') s = u8prev(b->txt, s);
        while (t < b->len && b->txt[t] != ' ') t = u8next(b->txt, b->len, t);
        if (s == t) t = u8next(b->txt, b->len, t);
        E.ab = E.cb; E.ap = s; E.cp = t; E.sel = t > s;
        lv_obj_invalidate(E.content);
        caret_moved();
        show_menu(E.sel);
        break;
    }
    default: break;
    }
}

static void menu_add(const char *label, int which)
{
    lv_obj_t *b = lv_button_create(E.menu);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, 64);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_bg_color(b, NT.dark ? lv_color_hex(0x48484A) : lv_color_hex(0xD1D1D6), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, nt_font(0, NT_SZ_S), 0);
    lv_obj_set_style_text_color(l, NT.text, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, menu_cb, LV_EVENT_CLICKED, (void *)(intptr_t)which);
}

static void show_menu(bool show)
{
    if (E.menu) { lv_obj_delete(E.menu); E.menu = NULL; }
    if (!show || !E.content) return;
    E.menu = lv_obj_create(E.content);
    lv_obj_remove_style_all(E.menu);
    lv_obj_set_size(E.menu, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_bg_color(E.menu, NT.dark ? lv_color_hex(0x2C2C2E) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(E.menu, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(E.menu, 16, 0);
    lv_obj_set_style_clip_corner(E.menu, true, 0);
    lv_obj_set_style_shadow_width(E.menu, 30, 0);
    lv_obj_set_style_shadow_opa(E.menu, LV_OPA_40, 0);
    lv_obj_set_flex_flow(E.menu, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(E.menu, LV_OBJ_FLAG_SCROLLABLE);
    if (E.sel) {
        menu_add(_("Cortar"), M_CUT);
        menu_add(_("Copiar"), M_COPY);
        if (s_clip) menu_add(_("Pegar"), M_PASTE);
        menu_add(_("Todo"), M_ALL);
    } else {
        menu_add(_("Seleccionar"), M_SELECT);
        menu_add(_("Todo"), M_ALL);
        if (s_clip) menu_add(_("Pegar"), M_PASTE);
    }
    lv_obj_update_layout(E.menu);
    int32_t mw = lv_obj_get_width(E.menu);
    int b0 = E.cb, p0 = E.cp, b1, p1;
    if (E.sel) sel_order(&b0, &p0, &b1, &p1);
    int32_t x, y, h;
    caret_rect(b0, p0, &x, &y, &h);
    int32_t mx = x - mw / 2;
    if (mx < 12) mx = 12;
    if (mx + mw > E.W - 12) mx = E.W - 12 - mw;
    int32_t my = y - 64 - (E.sel ? 70 : 14);
    if (my < lv_obj_get_scroll_y(E.sc) + 4) my = y + h + (E.sel ? 76 : 14);
    lv_obj_set_pos(E.menu, mx, my);
}

/* -------------------------------------------------------------------------- */
/* After a change                                                              */
/* -------------------------------------------------------------------------- */

static void refresh(bool all)
{
    if (!E.content) return;
    if (all) layout_all();
    else {
        nt_doc_number(E.doc);
        position_all();
    }
    lv_obj_invalidate(E.content);
    place_caret();
}

static void relayout_block(int bi)
{
    layout_block(&E.doc->b[bi]);
    refresh(false);
}

static void caret_moved(void)
{
    place_caret();
    if (E.cbs.caret) E.cbs.caret();
    nt_kb_refresh_caps();
}

static void changed(void)
{
    if (E.cbs.changed) E.cbs.changed();
}

void nt_ed_scroll_to_caret(void)
{
    if (!E.sc) return;
    lv_obj_update_layout(E.sc);
    int32_t x, y, h;
    caret_rect(E.cb, E.cp, &x, &y, &h);
    int32_t top = lv_obj_get_scroll_y(E.sc);
    int32_t vh = lv_obj_get_height(E.sc);
    if (y - 24 < top) lv_obj_scroll_to_y(E.sc, y - 24 > 0 ? y - 24 : 0, LV_ANIM_OFF);
    else if (y + h + 32 > top + vh) lv_obj_scroll_to_y(E.sc, y + h + 32 - vh, LV_ANIM_OFF);
}

/* -------------------------------------------------------------------------- */
/* Undo                                                                        */
/* -------------------------------------------------------------------------- */

static void snap_free(snap_t *s)
{
    nt_frag_free(s->f);
    s->f = NULL;
}

static void drop_redo(void)
{
    for (int i = 0; i < E.nr; i++) snap_free(&E.redo[i]);
    E.nr = 0;
}

static void push_undo(int op)
{
    uint32_t now = lv_tick_get();
    if (op == OP_TYPE && E.last_op == OP_TYPE && now - E.last_ms < 1500 && E.group < 40) {
        E.last_ms = now;
        E.group++;
        return;
    }
    if (op == OP_DELETE && E.last_op == OP_DELETE && now - E.last_ms < 1500) {
        E.last_ms = now;
        return;
    }
    E.last_op = op;
    E.last_ms = now;
    E.group = 0;
    size_t total = 0;
    for (int i = 0; i < E.nu; i++) total += nt_frag_size(E.undo[i].f);
    while (E.nu > 0 && (E.nu >= UNDO_MAX || total > UNDO_BYTES)) {
        total -= nt_frag_size(E.undo[0].f);
        snap_free(&E.undo[0]);
        memmove(&E.undo[0], &E.undo[1], (E.nu - 1) * sizeof(snap_t));
        E.nu--;
    }
    nt_frag_t *f = nt_frag_whole(E.doc);
    if (!f) return;
    E.undo[E.nu].f = f;
    E.undo[E.nu].b = E.cb;
    E.undo[E.nu].p = E.cp;
    E.nu++;
    drop_redo();
}

static void restore(snap_t *from, snap_t *to_stack, int *to_n)
{
    nt_frag_t *cur = nt_frag_whole(E.doc);
    if (cur && *to_n < UNDO_MAX) {
        to_stack[*to_n].f = cur;
        to_stack[*to_n].b = E.cb;
        to_stack[*to_n].p = E.cp;
        (*to_n)++;
    } else {
        nt_frag_free(cur);
    }
    nt_frag_restore(E.doc, from->f);
    E.cb = from->b < E.doc->n ? from->b : E.doc->n - 1;
    E.cp = from->p <= E.doc->b[E.cb].len ? from->p : E.doc->b[E.cb].len;
    snap_free(from);
    E.sel = false;
    E.has_pending = false;
    E.last_op = OP_NONE;
    refresh(true);
    caret_moved();
    nt_ed_scroll_to_caret();
    changed();
}

bool nt_ed_undo(void)
{
    if (!E.nu) return false;
    E.nu--;
    restore(&E.undo[E.nu], E.redo, &E.nr);
    return true;
}

bool nt_ed_redo(void)
{
    if (!E.nr) return false;
    E.nr--;
    restore(&E.redo[E.nr], E.undo, &E.nu);
    return true;
}

bool nt_ed_can_undo(void) { return E.nu > 0; }
bool nt_ed_can_redo(void) { return E.nr > 0; }

void nt_ed_reset_history(void)
{
    for (int i = 0; i < E.nu; i++) snap_free(&E.undo[i]);
    E.nu = 0;
    drop_redo();
    E.last_op = OP_NONE;
}

/* -------------------------------------------------------------------------- */
/* Editing                                                                     */
/* -------------------------------------------------------------------------- */

static uint16_t typing_attr(void)
{
    nt_blk_t *b = &E.doc->b[E.cb];
    if (b->type == BT_TITLE) return 0;
    if (E.has_pending) return E.pending;
    if (E.cp > 0) return b->at[u8prev(b->txt, E.cp)];
    if (b->len > 0) return b->at[0];
    return 0;
}

static void delete_selection(void)
{
    if (!E.sel) return;
    int b0, p0, b1, p1;
    sel_order(&b0, &p0, &b1, &p1);
    nt_doc_t *d = E.doc;
    if (b0 == b1) {
        nt_blk_delete(&d->b[b0], p0, p1 - p0);
    } else if (b0 == 0) {
        nt_blk_delete(&d->b[0], p0, d->b[0].len - p0);
        nt_blk_delete(&d->b[b1], 0, p1);
        for (int i = b1 - 1; i >= 1; i--) nt_doc_remove(d, i);
        if (d->b[1].type == BT_RULE) d->b[1].type = BT_PARA;
    } else {
        nt_blk_t *first = &d->b[b0];
        nt_blk_delete(first, p0, first->len - p0);
        nt_blk_t *last = &d->b[b1];
        nt_blk_insert(first, first->len, last->txt + p1, last->len - p1, 0);
        memcpy(first->at + p0, last->at + p1, (last->len - p1) * sizeof(uint16_t));
        for (int i = b1; i > b0; i--) nt_doc_remove(d, i);
        if (first->type == BT_RULE) first->type = BT_PARA;
    }
    if (d->n == 1) nt_doc_insert(d, 1, BT_PARA);
    E.cb = b0;
    E.cp = p0;
    E.sel = false;
    layout_all();
}

/* "- " at the start of a paragraph makes a list, and so on. */
static void shortcuts(void)
{
    nt_blk_t *b = &E.doc->b[E.cb];
    if (b->type != BT_PARA || E.cp > 4) return;
    struct { const char *mark; int type, style; } m[] = {
        { "- ", BT_BULLET, NT_BS_DISC }, { "* ", BT_BULLET, NT_BS_DISC },
        { "1. ", BT_NUM, NT_NS_DEC }, { "a) ", BT_NUM, NT_NS_ALPHA }, { "i. ", BT_NUM, NT_NS_ROMAN },
        { "[] ", BT_CHECK, 0 }, { "[ ] ", BT_CHECK, 0 },
        { "# ", BT_H1, 0 }, { "## ", BT_H2, 0 }, { "### ", BT_H3, 0 }, { "> ", BT_QUOTE, 0 },
    };
    for (size_t k = 0; k < sizeof m / sizeof m[0]; k++) {
        int l = (int)strlen(m[k].mark);
        if (E.cp == l && strncmp(b->txt, m[k].mark, l) == 0) {
            push_undo(OP_OTHER);
            nt_blk_delete(b, 0, l);
            b->type = (uint8_t)m[k].type;
            b->style = (uint8_t)m[k].style;
            E.cp = 0;
            E.last_op = OP_NONE;
            return;
        }
    }
}

static void ed_insert(const char *s)
{
    if (!E.doc) return;
    int n = (int)strlen(s);
    if (!n) return;
    show_menu(false);
    push_undo(E.sel ? OP_OTHER : OP_TYPE);
    if (E.sel) delete_selection();
    nt_blk_t *b = &E.doc->b[E.cb];
    if (b->type == BT_RULE) {
        nt_doc_insert(E.doc, E.cb + 1, BT_PARA);
        E.cb++;
        E.cp = 0;
        b = &E.doc->b[E.cb];
        layout_block(b);
    }
    uint16_t a = typing_attr();
    nt_blk_insert(b, E.cp, s, n, a);
    E.cp += n;
    E.has_pending = false;
    shortcuts();
    relayout_block(E.cb);
    caret_moved();
    nt_ed_scroll_to_caret();
    changed();
}

static void ed_backspace(void)
{
    if (!E.doc) return;
    show_menu(false);
    nt_doc_t *d = E.doc;
    if (E.sel) {
        push_undo(OP_OTHER);
        delete_selection();
        refresh(false);
        caret_moved();
        changed();
        return;
    }
    nt_blk_t *b = &d->b[E.cb];
    if (E.cp > 0) {
        push_undo(OP_DELETE);
        int p = u8prev(b->txt, E.cp);
        nt_blk_delete(b, p, E.cp - p);
        E.cp = p;
        relayout_block(E.cb);
    } else if (E.cb > 0) {
        push_undo(OP_OTHER);
        if (b->type == BT_RULE) {
            nt_doc_remove(d, E.cb);
            E.cb--;
            E.cp = d->b[E.cb].len;
        } else if (b->type != BT_PARA) {
            b->type = BT_PARA;
            b->style = 0;
            b->checked = 0;
        } else if (b->indent > 0) {
            b->indent--;
        } else {
            nt_blk_t *p = &d->b[E.cb - 1];
            if (p->type == BT_RULE) {
                nt_doc_remove(d, E.cb - 1);
                E.cb--;
            } else {
                int at = p->len;
                if (p->type == BT_TITLE) {
                    nt_blk_insert(p, at, b->txt, b->len, 0);
                } else {
                    nt_blk_insert(p, at, b->txt, b->len, 0);
                    memcpy(p->at + at, b->at, b->len * sizeof(uint16_t));
                }
                nt_doc_remove(d, E.cb);
                E.cb--;
                E.cp = at;
                if (d->n == 1) nt_doc_insert(d, 1, BT_PARA);
            }
        }
        layout_all();
        refresh(false);
    } else {
        return;
    }
    caret_moved();
    nt_ed_scroll_to_caret();
    changed();
}

static void ed_enter(void)
{
    if (!E.doc) return;
    show_menu(false);
    push_undo(OP_OTHER);
    E.last_op = OP_NONE;
    if (E.sel) delete_selection();
    nt_doc_t *d = E.doc;
    nt_blk_t *b = &d->b[E.cb];
    uint16_t keep = typing_attr();

    if ((is_list(b->type) || b->type == BT_QUOTE) && b->len == 0) {
        /* an empty item ends the list (a level at a time) */
        if (b->indent > 0) b->indent--;
        else { b->type = BT_PARA; b->style = 0; b->checked = 0; }
    } else if (b->type == BT_PARA && b->len == 3 && strcmp(b->txt, "---") == 0) {
        b->type = BT_RULE;
        nt_blk_set(b, "", 0);
        nt_doc_insert(d, E.cb + 1, BT_PARA);
        E.cb++;
        E.cp = 0;
    } else if (b->type == BT_RULE) {
        nt_doc_insert(d, E.cb + 1, BT_PARA);
        E.cb++;
        E.cp = 0;
    } else if (E.cp == 0 && b->len > 0 && b->type != BT_TITLE) {
        /* at the start of a line: an empty one above, the same kind */
        nt_blk_t *nb = nt_doc_insert(d, E.cb, heading(b->type) ? BT_PARA : b->type);
        b = &d->b[E.cb + 1];
        if (nb) {
            nb->style = b->type == BT_CHECK ? 0 : b->style;
            nb->indent = b->indent;
        }
        E.cb++;
        E.cp = 0;
    } else {
        int type = b->type;
        if (heading(type)) type = BT_PARA;
        nt_blk_t *nb = nt_doc_insert(d, E.cb + 1, type);
        b = &d->b[E.cb];
        if (nb) {
            nb->style = type == BT_CHECK ? 0 : b->style;
            nb->indent = b->type == BT_TITLE ? 0 : b->indent;
            if (type == BT_CHECK) nb->style = 0;
            nt_blk_insert(nb, 0, b->txt + E.cp, b->len - E.cp, 0);
            if (b->type != BT_TITLE) memcpy(nb->at, b->at + E.cp, (b->len - E.cp) * sizeof(uint16_t));
            nt_blk_delete(b, E.cp, b->len - E.cp);
            E.cb++;
            E.cp = 0;
            if (b->type != BT_TITLE && keep) { E.pending = keep; E.has_pending = true; }
        }
    }
    layout_all();
    refresh(false);
    caret_moved();
    nt_ed_scroll_to_caret();
    changed();
}

static void ed_move(int dx)
{
    if (!E.doc) return;
    show_menu(false);
    E.sel = false;
    E.has_pending = false;
    nt_doc_t *d = E.doc;
    while (dx < 0) {
        if (E.cp > 0) E.cp = u8prev(d->b[E.cb].txt, E.cp);
        else if (E.cb > 0) { E.cb--; E.cp = d->b[E.cb].len; }
        dx++;
    }
    while (dx > 0) {
        nt_blk_t *b = &d->b[E.cb];
        if (E.cp < b->len) E.cp = u8next(b->txt, b->len, E.cp);
        else if (E.cb < d->n - 1) { E.cb++; E.cp = 0; }
        dx--;
    }
    lv_obj_invalidate(E.content);
    caret_moved();
    nt_ed_scroll_to_caret();
}

static bool ed_caps(void)
{
    if (!E.doc || E.sel) return false;
    nt_blk_t *b = &E.doc->b[E.cb];
    if (b->type == BT_RULE) return false;
    int p = E.cp;
    while (p > 0 && b->txt[p - 1] == ' ') p--;
    if (p == 0) return true;
    if (p == E.cp) {
        /* right after an opening ¿ or ¡ */
        return p >= 2 && (unsigned char)b->txt[p - 2] == 0xC2 &&
               ((unsigned char)b->txt[p - 1] == 0xBF || (unsigned char)b->txt[p - 1] == 0xA1);
    }
    char c = b->txt[p - 1];
    return c == '.' || c == '?' || c == '!';
}

static const nt_kb_target_t s_target = { ed_insert, ed_backspace, ed_enter, ed_move, ed_caps };

const nt_kb_target_t *nt_ed_target(void)
{
    return &s_target;
}

/* -------------------------------------------------------------------------- */
/* Touch                                                                       */
/* -------------------------------------------------------------------------- */

static void select_word_at(int bi, int pos)
{
    nt_blk_t *b = &E.doc->b[bi];
    int s = pos, t = pos;
    while (s > 0 && b->txt[u8prev(b->txt, s)] != ' ') s = u8prev(b->txt, s);
    while (t < b->len && b->txt[t] != ' ') t = u8next(b->txt, b->len, t);
    if (s == t && t < b->len) t = u8next(b->txt, b->len, t);
    E.ab = bi; E.ap = s; E.cb = bi; E.cp = t;
    E.sel = t > s || b->type == BT_RULE;
    if (b->type == BT_RULE) { E.ap = 0; E.cp = 0; E.ab = bi; E.cb = bi < E.doc->n - 1 ? bi + 1 : bi; E.sel = E.cb != bi; }
}

static bool on_checkbox(int bi, const lv_point_t *p)
{
    nt_blk_t *b = &E.doc->b[bi];
    if (b->type != BT_CHECK || !b->nlines) return false;
    int32_t gx = E.pad_x + b->indent * INDENT_STEP;
    return p->x >= gx - 10 && p->x < text_x(b) && p->y >= b->y - 8 && p->y < b->y + b->lines[0].h + 8;
}

static void content_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);
    lv_point_t abs = p;
    to_content(&p);

    if (code == LV_EVENT_PRESSED) {
        E.down = p;
        E.long_pressed = false;
        if (E.menu) show_menu(false);
    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (lv_indev_get_scroll_obj(in)) return;
        int bi, pos;
        hit(p.x, p.y, &bi, &pos);
        E.long_pressed = true;
        select_word_at(bi, pos);
        E.dragging = true;
        E.drag_moved = false;
        lv_obj_remove_flag(E.sc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_invalidate(E.content);
        caret_moved();
        if (!E.focus && E.cbs.focus) E.cbs.focus();
    } else if (code == LV_EVENT_PRESSING && E.dragging) {
        /* the word stays selected until the finger actually moves */
        if (!E.drag_moved && LV_ABS(p.x - E.down.x) < 16 && LV_ABS(p.y - E.down.y) < 16) return;
        E.drag_moved = true;
        autoscroll(&abs);
        int bi, pos;
        hit(p.x, p.y, &bi, &pos);
        if (bi != E.cb || pos != E.cp) {
            E.cb = bi;
            E.cp = pos;
            E.sel = !(E.cb == E.ab && E.cp == E.ap);
            lv_obj_invalidate(E.content);
            place_caret();
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (E.dragging) {
            E.dragging = false;
            lv_obj_add_flag(E.sc, LV_OBJ_FLAG_SCROLLABLE);
            caret_moved();
            show_menu(true);
        }
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        if (E.long_pressed) return;
        int bi, pos;
        hit(p.x, p.y, &bi, &pos);
        uint32_t now = lv_tick_get();
        bool near = LV_ABS(p.x - E.last_pt.x) < 40 && LV_ABS(p.y - E.last_pt.y) < 40;
        E.clicks = (near && now - E.last_click < 380) ? E.clicks + 1 : 1;
        E.last_click = now;
        E.last_pt = p;

        if (E.clicks == 1 && on_checkbox(bi, &p)) {
            push_undo(OP_OTHER);
            E.last_op = OP_NONE;
            E.doc->b[bi].checked ^= 1;
            relayout_block(bi);
            changed();
            aos_hal_beep(E.doc->b[bi].checked ? 1800 : 1200, 15);
            return;
        }
        bool was_here = E.focus && !E.sel && bi == E.cb && pos == E.cp;
        if (E.clicks == 2) {
            select_word_at(bi, pos);
            show_menu(E.sel);
        } else if (E.clicks >= 3) {
            E.ab = bi; E.ap = 0; E.cb = bi; E.cp = E.doc->b[bi].len;
            E.sel = E.cp > 0;
            show_menu(E.sel);
        } else {
            E.sel = false;
            E.cb = bi;
            E.cp = pos;
            E.has_pending = false;
        }
        lv_obj_invalidate(E.content);
        if (!E.focus && E.cbs.focus) E.cbs.focus();
        caret_moved();
        if (E.clicks == 1 && was_here) show_menu(true);
    }
}

static void sc_scroll_cb(lv_event_t *e)
{
    (void)e;
    if (E.menu && !E.sel) show_menu(false);
}

/* -------------------------------------------------------------------------- */
/* Formatting                                                                  */
/* -------------------------------------------------------------------------- */

uint16_t nt_ed_attr(void)
{
    if (!E.doc) return 0;
    if (!E.sel) return E.doc->b[E.cb].type == BT_TITLE ? 0 : typing_attr();
    int b0, p0, b1, p1;
    sel_order(&b0, &p0, &b1, &p1);
    uint16_t common = 0xFFFF, first = 0;
    bool any = false;
    for (int i = b0; i <= b1; i++) {
        nt_blk_t *b = &E.doc->b[i];
        if (b->type == BT_TITLE) continue;
        int s = i == b0 ? p0 : 0, t = i == b1 ? p1 : b->len;
        for (int k = s; k < t; k++) {
            if (!any) first = b->at[k];
            common &= b->at[k];
            any = true;
        }
    }
    if (!any) return 0;
    return (uint16_t)((common & (A_B | A_I | A_U | A_S)) | (first & (A_SZ_MASK | A_FG_MASK | A_HL_MASK)));
}

int nt_ed_block_type(int *style)
{
    if (!E.doc) return BT_PARA;
    nt_blk_t *b = &E.doc->b[E.cb];
    if (style) *style = b->style;
    return b->type;
}

/* Rewrites the style words of the selection: clear 'mask', set 'bits'. */
static void apply(uint16_t mask, uint16_t bits)
{
    int b0, p0, b1, p1;
    sel_order(&b0, &p0, &b1, &p1);
    push_undo(OP_OTHER);
    E.last_op = OP_NONE;
    for (int i = b0; i <= b1; i++) {
        nt_blk_t *b = &E.doc->b[i];
        if (b->type == BT_TITLE) continue;
        int s = i == b0 ? p0 : 0, t = i == b1 ? p1 : b->len;
        for (int k = s; k < t; k++) b->at[k] = (uint16_t)((b->at[k] & ~mask) | bits);
    }
    layout_all();
    refresh(false);
    changed();
}

static void set_field(uint16_t mask, uint16_t bits)
{
    if (!E.doc) return;
    if (E.sel) { apply(mask, bits); }
    else {
        E.pending = (uint16_t)((typing_attr() & ~mask) | bits);
        E.has_pending = true;
        layout_block(&E.doc->b[E.cb]);
        refresh(false);
    }
    if (E.cbs.caret) E.cbs.caret();
}

void nt_ed_toggle(uint16_t bit)
{
    uint16_t cur = nt_ed_attr();
    set_field(bit, (cur & bit) ? 0 : bit);
}

void nt_ed_set_size(int size) { set_field(A_SZ_MASK, (uint16_t)((size & 3) << A_SZ_SHIFT)); }
void nt_ed_set_fg(int fg)     { set_field(A_FG_MASK, (uint16_t)((fg & 15) << A_FG_SHIFT)); }
void nt_ed_set_hl(int hl)     { set_field(A_HL_MASK, (uint16_t)((hl & 7) << A_HL_SHIFT)); }
void nt_ed_clear_format(void) { set_field(0xFFFF, 0); }

static void block_range(int *b0, int *b1)
{
    if (E.sel) {
        int p0, p1;
        sel_order(b0, &p0, b1, &p1);
        if (*b1 > *b0 && p1 == 0) (*b1)--;
    } else {
        *b0 = *b1 = E.cb;
    }
    if (*b0 < 1) *b0 = 1;
}

void nt_ed_set_block(int type, int style)
{
    if (!E.doc) return;
    int b0, b1;
    block_range(&b0, &b1);
    if (b1 < b0) return;
    nt_blk_t *first = &E.doc->b[b0];
    /* the same thing again takes it off */
    bool off = first->type == type && (type != BT_BULLET && type != BT_NUM ? true : first->style == style);
    push_undo(OP_OTHER);
    E.last_op = OP_NONE;
    for (int i = b0; i <= b1; i++) {
        nt_blk_t *b = &E.doc->b[i];
        if (b->type == BT_RULE) continue;
        b->type = (uint8_t)(off ? BT_PARA : type);
        b->style = (uint8_t)(off || type == BT_CHECK ? 0 : style);
        b->checked = 0;
        if (heading(b->type)) b->indent = 0;
    }
    layout_all();
    refresh(false);
    caret_moved();
    changed();
}

void nt_ed_indent(int delta)
{
    if (!E.doc) return;
    int b0, b1;
    block_range(&b0, &b1);
    push_undo(OP_OTHER);
    E.last_op = OP_NONE;
    for (int i = b0; i <= b1; i++) {
        nt_blk_t *b = &E.doc->b[i];
        if (b->type == BT_RULE || heading(b->type)) continue;
        int v = b->indent + delta;
        b->indent = (uint8_t)(v < 0 ? 0 : v > NT_MAX_INDENT ? NT_MAX_INDENT : v);
    }
    layout_all();
    refresh(false);
    caret_moved();
    changed();
}

void nt_ed_toggle_check_block(void)
{
    nt_ed_set_block(BT_CHECK, 0);
}

/* -------------------------------------------------------------------------- */
/* Selection and clipboard                                                     */
/* -------------------------------------------------------------------------- */

bool nt_ed_has_selection(void) { return E.sel; }

void nt_ed_select_all(void)
{
    if (!E.doc) return;
    E.ab = 1;
    E.ap = 0;
    E.cb = E.doc->n - 1;
    E.cp = E.doc->b[E.cb].len;
    E.sel = !(E.cb == 1 && E.cp == 0);
    lv_obj_invalidate(E.content);
    caret_moved();
}

void nt_ed_clear_selection(void)
{
    if (!E.sel) return;
    E.sel = false;
    show_menu(false);
    lv_obj_invalidate(E.content);
    caret_moved();
}

void nt_ed_copy(bool cut)
{
    if (!E.sel) return;
    int b0, p0, b1, p1;
    sel_order(&b0, &p0, &b1, &p1);
    nt_frag_free(s_clip);
    s_clip = nt_frag_copy(E.doc, b0, p0, b1, p1);
    if (cut) {
        push_undo(OP_OTHER);
        E.last_op = OP_NONE;
        delete_selection();
        refresh(false);
        caret_moved();
        changed();
    }
}

void nt_ed_paste(void)
{
    if (!s_clip || !E.doc) return;
    push_undo(OP_OTHER);
    E.last_op = OP_NONE;
    if (E.sel) delete_selection();
    if (E.cb == 0) {
        /* into the title: the first piece, as plain text */
        int len;
        const char *t = nt_frag_text(s_clip, 0, &len);
        nt_blk_insert(&E.doc->b[0], E.cp, t, len, 0);
        E.cp += len;
    } else {
        nt_frag_paste(E.doc, s_clip, &E.cb, &E.cp);
    }
    layout_all();
    refresh(false);
    caret_moved();
    nt_ed_scroll_to_caret();
    changed();
}

bool nt_ed_clip_full(void)
{
    return s_clip != NULL;
}

void nt_ed_clip_free(void)
{
    nt_frag_free(s_clip);
    s_clip = NULL;
}

/* -------------------------------------------------------------------------- */
/* The object                                                                  */
/* -------------------------------------------------------------------------- */

static void content_delete_cb(lv_event_t *e)
{
    (void)e;
    if (E.blink) { lv_timer_delete(E.blink); E.blink = NULL; }
    E.content = NULL;
    E.sc = NULL;
    E.caret = NULL;
    E.h[0] = E.h[1] = NULL;
    E.menu = NULL;
}

lv_obj_t *nt_ed_create(lv_obj_t *parent, nt_doc_t *doc, const nt_ed_cbs_t *cbs)
{
    bool same = E.doc == doc;
    E.doc = doc;
    E.cbs = *cbs;
    E.handle = -1;
    E.dragging = false;
    E.menu = NULL;
    if (!same) {
        E.cb = doc->n > 1 ? 1 : 0;
        E.cp = 0;
        E.sel = false;
        E.has_pending = false;
        nt_ed_reset_history();
    }
    if (E.cb >= doc->n) { E.cb = doc->n - 1; E.cp = 0; }
    if (E.cp > doc->b[E.cb].len) E.cp = doc->b[E.cb].len;

    E.sc = lv_obj_create(parent);
    lv_obj_remove_style_all(E.sc);
    lv_obj_set_width(E.sc, lv_pct(100));
    lv_obj_set_flex_grow(E.sc, 1);
    lv_obj_set_scroll_dir(E.sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(E.sc, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(E.sc, NT.dim, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(E.sc, sc_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);

    E.content = lv_obj_create(E.sc);
    lv_obj_remove_style_all(E.content);
    lv_obj_set_width(E.content, lv_pct(100));
    lv_obj_add_flag(E.content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(E.content, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_flag(E.content, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_event_cb(E.content, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(E.content, content_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(E.content, content_delete_cb, LV_EVENT_DELETE, NULL);

    E.caret = lv_obj_create(E.content);
    lv_obj_remove_style_all(E.caret);
    lv_obj_set_style_bg_color(E.caret, NT.accent, 0);
    lv_obj_set_style_bg_opa(E.caret, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(E.caret, 2, 0);
    lv_obj_remove_flag(E.caret, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(E.caret, LV_OBJ_FLAG_HIDDEN);
    E.h[0] = make_handle();
    E.h[1] = make_handle();
    E.blink = lv_timer_create(blink_cb, 530, NULL);
    nt_ed_relayout();
    return E.sc;
}

void nt_ed_relayout(void)
{
    if (!E.sc) return;
    lv_obj_update_layout(lv_obj_get_parent(E.sc));
    E.W = lv_obj_get_content_width(E.sc);
    if (E.W < 200) E.W = lv_obj_get_width(lv_obj_get_parent(E.sc));
    E.pad_x = E.W > 900 ? (E.W - 860) / 2 : 32;     /* lying down, a column that reads */
    refresh(true);
}

void nt_ed_destroyed(void)
{
    show_menu(false);
    if (E.blink) { lv_timer_delete(E.blink); E.blink = NULL; }
    E.sc = E.content = E.caret = NULL;
    E.h[0] = E.h[1] = NULL;
}

void nt_ed_forget(void)
{
    nt_ed_reset_history();
    E.doc = NULL;
}

void nt_ed_set_focus(bool focus)
{
    E.focus = focus;
    if (!focus) { E.sel = false; show_menu(false); if (E.content) lv_obj_invalidate(E.content); }
    place_caret();
}

void nt_ed_place(int blk, int pos)
{
    if (!E.doc) return;
    E.cb = blk < E.doc->n ? blk : E.doc->n - 1;
    E.cp = pos <= E.doc->b[E.cb].len ? pos : E.doc->b[E.cb].len;
    E.sel = false;
    caret_moved();
}

void nt_ed_caret_pos(int *blk, int *pos)
{
    *blk = E.cb;
    *pos = E.cp;
}

void nt_ed_set_scroll(int32_t y)
{
    if (!E.sc) return;
    lv_obj_update_layout(E.sc);
    lv_obj_scroll_to_y(E.sc, y, LV_ANIM_OFF);
}

int32_t nt_ed_get_scroll(void)
{
    return E.sc ? lv_obj_get_scroll_y(E.sc) : 0;
}
