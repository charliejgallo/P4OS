/*
 * P4OS - Office: the document screen (docx and odt).
 *
 * The page is laid out and drawn by the app: each paragraph's runs in the
 * font of their size and style (the pack's Inter regular, bold, italic and
 * bold italic; of_fonts.c), lines on a common baseline, headings, list
 * bullets and numbers, alignment, tables with their grid, pictures (decoded
 * by the HAL the first time they come into sight), footnote numbers raised.
 * A paragraph's lines are kept until it changes; the positions of all of
 * them are worked out again in one cheap pass.
 *
 * Editing: a tap places the caret and brings the keyboard, a double tap
 * selects a word, a long press and a drag select; typing, Enter (a new
 * paragraph like this one) and Backspace (joining two) within a body or a
 * table cell. The bar over the keyboard: bold, italic, underline,
 * strikethrough, the paragraph's style (body text, three headings), its
 * alignment. A locked paragraph (tracked changes, content controls) shows a
 * mark at its side and says why when touched. Undo keeps copies of the
 * paragraphs an edit touched. A USB or Bluetooth keyboard: arrows with
 * Shift, Home, End, Ctrl+B/I/U, Ctrl+Z/Y/C/X/V/A/S/F.
 */
#include "of_ui.h"
#include "of_pdf.h"
#include "of_doc.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NZOOM 5
static const float ZPT[NZOOM] = { 1.5f, 1.8f, 2.1f, 2.5f, 3.0f };    /* pixels per point */

#define UNDO_MAX 100

typedef struct {
    int32_t  a, b;              /* bytes of the paragraph */
    int32_t  y, h, asc;         /* from the paragraph's top */
    int32_t  x;                 /* alignment offset */
    int32_t  w;                 /* the text's width */
} line_t;

typedef struct {
    line_t  *l;
    int      n, cap;
    bool     valid;
    int32_t  x, w;              /* the column it is laid out in (page coordinates) */
    int32_t  y, h;              /* page coordinates */
    int32_t  indent;            /* text start, after the label */
} play_t;

typedef struct {
    int      first;
    int      nbefore, nafter;
    of_para_t **before, **after;
    int      cpara, cpos;
} uentry_t;

typedef struct {                /* a decoded picture */
    int        obj;
    uint16_t  *px;
    int        w, h;
    bool       failed, wanted;
    lv_image_dsc_t dsc;
} pic_t;

/* a table, laid out */
typedef struct {
    int32_t  y0, y1;
    int32_t  colx[65];
    int32_t *rowy;              /* nrows + 1 */
    int      nrows, ncols;
} tlay_t;

static struct {
    of_doc_t  *d;
    char       path[300];
    lv_obj_t  *root, *top, *title, *area, *kb, *fmt, *b_undo, *b_redo, *b_kb, *badge;
    lv_obj_t  *fb[4];
    int32_t    W, H, ax, ay, aw, ah;
    int        z;
    play_t    *pl;
    int        npl;
    tlay_t    *tl;
    int        ntl;
    int32_t    page_x, page_w, total_h;
    int32_t    sy;
    float      vy;
    lv_timer_t *fling, *pic_timer;
    float      pinch;
    /* caret and selection */
    int        cp, cpos, ap, apos;
    int32_t    want_x;
    bool       selecting;
    uint16_t   typing;          /* a style for the next letters, 0xFFFF none */
    /* undo */
    uentry_t   undo[UNDO_MAX], redo[UNDO_MAX];
    int        nundo, nredo;
    bool       typing_group;
    uint32_t   last_type_ms;
    bool       modified;
    pic_t     *pics;
    int        npics;
    char       find[128];
} D;

static void relayout(void);
static void refresh(void);
static void layout_all(void);
static void kb_show(bool on);
static void invalidate_all(void);

/* -------------------------------------------------------------------------- */
/* Fonts of a run                                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    const lv_font_t *f;
    bool faux, slant;
    int32_t px;
    of_rstyle_t e;
} runfont_t;

static void run_font(const of_para_t *p, uint16_t rs, runfont_t *r)
{
    of_doc_effective(D.d, p, rs, &r->e);
    float pt = r->e.half_pt / 2.0f;
    if (pt < 4) pt = 11;
    if (r->e.sup) pt *= 0.65f;
    r->px = (int32_t)(pt * ZPT[D.z] + 0.5f);
    int st = (r->e.b == OF_TRI_ON ? 1 : 0) | (r->e.i == OF_TRI_ON ? 2 : 0);
    r->f = ofd_font(st, r->px, &r->faux, &r->slant);
}

static int32_t glyph_w(const lv_font_t *f, uint32_t cp, uint32_t next)
{
    if (cp == '\t') return lv_font_get_glyph_width(f, ' ', 0) * 4;
    return lv_font_get_glyph_width(f, cp, next);
}

static bool is_obj(const of_para_t *p, int i)
{
    return i >= 0 && i + 3 <= p->len && !memcmp(p->txt + i, OF_OBJ_CHAR, 3);
}

/* -------------------------------------------------------------------------- */
/* Pictures                                                                    */
/* -------------------------------------------------------------------------- */

static pic_t *pic_of(int obj)
{
    for (int i = 0; i < D.npics; i++)
        if (D.pics[i].obj == obj) return &D.pics[i];
    pic_t *n = of_realloc(D.pics, (size_t)(D.npics + 1) * sizeof *n);
    if (!n) return NULL;
    D.pics = n;
    memset(&D.pics[D.npics], 0, sizeof *n);
    D.pics[D.npics].obj = obj;
    return &D.pics[D.npics++];
}

/* the size a picture takes on the page */
static void obj_size(const of_obj_t *o, int32_t maxw, int32_t *w, int32_t *h)
{
    int32_t ww = (int32_t)(o->w * ZPT[D.z]), hh = (int32_t)(o->h * ZPT[D.z]);
    if (ww < 8) ww = 8;
    if (hh < 8) hh = 8;
    if (ww > maxw) { hh = hh * maxw / ww; ww = maxw; }
    *w = ww;
    *h = hh;
}

/* Decoded at the box's size exactly (enlarged if small: the HAL never
 * enlarges when only fitting, and a bitmap scaled up by the draw is clipped). */
static void pic_decode(pic_t *pc)
{
    of_obj_t *o = &D.d->obj[pc->obj];
    if (!o->target) { pc->failed = true; return; }
    if (o->file) {
        /* a picture on the card (a picture opened alone, one an RTF carried) */
        const char *fx = of_ext(o->target);
        if (strcmp(fx, "png") && strcmp(fx, "jpg") && strcmp(fx, "jpeg") && strcmp(fx, "bmp")) { pc->failed = true; return; }
        int32_t w, h;
        obj_size(o, D.page_w, &w, &h);
        int ow = 0, oh = 0;
        pc->px = aos_hal_image_decode(o->target, 0, 0, (int)w, (int)h, true, &ow, &oh, NULL, NULL);
        if (!pc->px) { pc->failed = true; return; }
        pc->w = ow;
        pc->h = oh;
        memset(&pc->dsc, 0, sizeof pc->dsc);
        pc->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        pc->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        pc->dsc.header.w = (uint32_t)ow;
        pc->dsc.header.h = (uint32_t)oh;
        pc->dsc.header.stride = (uint32_t)ow * 2;
        pc->dsc.data = (const uint8_t *)pc->px;
        pc->dsc.data_size = (uint32_t)(ow * oh * 2);
        return;
    }
    int i = of_zip_find(&D.d->zip, o->target);
    if (i < 0) { pc->failed = true; return; }
    const of_zentry_t *e = &D.d->zip.e[i];
    const char *ext = of_ext(o->target);
    if (strcmp(ext, "png") && strcmp(ext, "jpg") && strcmp(ext, "jpeg") && strcmp(ext, "bmp")) { pc->failed = true; return; }
    int32_t w, h;
    obj_size(o, D.page_w, &w, &h);
    int ow = 0, oh = 0;
    if (e->method == 0) {
        /* stored: decoded straight from the document */
        pc->px = aos_hal_image_decode(D.path, e->data, e->csize, (int)w, (int)h, true, &ow, &oh, NULL, NULL);
    } else {
        size_t n;
        uint8_t *data = of_zip_read(&D.d->zip, i, &n);
        if (data) {
            char tmp[200];
            snprintf(tmp, sizeof tmp, "%s/oficina/pic.%s", aos_hal_path_data(), ext);
            if (of_file_write(tmp, data, n)) pc->px = aos_hal_image_decode(tmp, 0, 0, (int)w, (int)h, true, &ow, &oh, NULL, NULL);
            remove(tmp);
            of_free(data);
        }
    }
    if (!pc->px) { pc->failed = true; return; }
    pc->w = ow;
    pc->h = oh;
    memset(&pc->dsc, 0, sizeof pc->dsc);
    pc->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    pc->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    pc->dsc.header.w = (uint32_t)ow;
    pc->dsc.header.h = (uint32_t)oh;
    pc->dsc.header.stride = (uint32_t)ow * 2;
    pc->dsc.data = (const uint8_t *)pc->px;
    pc->dsc.data_size = (uint32_t)(ow * oh * 2);
}

static void pics_free(void)
{
    for (int i = 0; i < D.npics; i++)
        if (D.pics[i].px) aos_hal_image_free(D.pics[i].px);
    of_free(D.pics);
    D.pics = NULL;
    D.npics = 0;
}

/* one wanted picture a turn: decoding takes tens of milliseconds */
static void pic_timer_cb(lv_timer_t *t)
{
    (void)t;
    for (int i = 0; i < D.npics; i++) {
        pic_t *pc = &D.pics[i];
        if (pc->wanted && !pc->px && !pc->failed) {
            pic_decode(pc);
            pc->wanted = false;
            if (D.area) lv_obj_invalidate(D.area);
            return;
        }
    }
}

/* The width an object takes in a line. */
static int32_t obj_width(const of_para_t *p, int i, int32_t maxw)
{
    of_obj_t *o = of_para_obj_at(D.d, p, i);
    if (!o) return 0;
    if (o->kind == OF_OBJ_IMAGE) {
        int32_t iw, ih;
        obj_size(o, maxw, &iw, &ih);
        return iw + 4;
    }
    if ((o->kind == OF_OBJ_TEXT || o->kind == OF_OBJ_NOTE) && o->text) {
        runfont_t rf;
        run_font(p, p->at[i], &rf);
        const lv_font_t *sf = o->kind == OF_OBJ_NOTE ? ofd_font(0, rf.px * 6 / 10 > 10 ? rf.px * 6 / 10 : 10, NULL, NULL) : rf.f;
        int32_t w = 0;
        for (const char *t = o->text; *t;) {
            int l2;
            uint32_t cp = of_utf8_get(t, &l2);
            w += lv_font_get_glyph_width(sf, cp, 0);
            t += l2;
        }
        return w;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */
/* -------------------------------------------------------------------------- */

static void line_push(play_t *pl, line_t l)
{
    if (pl->n == pl->cap) {
        int cap = pl->cap ? pl->cap * 2 : 4;
        line_t *n = of_realloc(pl->l, (size_t)cap * sizeof *n);
        if (!n) return;
        pl->l = n;
        pl->cap = cap;
    }
    pl->l[pl->n++] = l;
}

/* The lines of paragraph i, for a column of width w. */
static void layout_para(int i, int32_t w)
{
    of_para_t *p = &D.d->p[i];
    play_t *pl = &D.pl[i];
    pl->n = 0;
    pl->w = w;
    int32_t em = (int32_t)(11 * ZPT[D.z]);
    pl->indent = p->label[0] ? em * (2 + p->ilvl * 2) : em * 3 / 2 * p->indent;
    int32_t maxw = w - pl->indent;
    if (maxw < em * 2) maxw = em * 2;
    int a = 0;
    runfont_t rf;
    uint16_t cur = 0xFFFF;
    /* an empty paragraph has the height of its mark's font */
    if (!p->len) {
        run_font(p, p->mark, &rf);
        int32_t lh = rf.f->line_height;
        line_push(pl, (line_t){ 0, 0, 0, lh * 115 / 100, lh - rf.f->base_line, 0, 0 });
        pl->h = lh * 115 / 100;
        return;
    }
    while (a < p->len) {
        int32_t x = 0, asc = 0, desc = 0, h = 0;
        int last_space = -1;
        int i2 = a;
        while (i2 < p->len) {
            int32_t gw, ga, gd;
            int len;
            if (is_obj(p, i2)) {
                of_obj_t *o = of_para_obj_at(D.d, p, i2);
                len = 3;
                gw = 0;
                ga = gd = 0;
                if (o && o->kind == OF_OBJ_IMAGE) {
                    int32_t iw, ih;
                    obj_size(o, maxw, &iw, &ih);
                    gw = iw + 4;
                    ga = ih;
                } else if (o && (o->kind == OF_OBJ_TEXT || o->kind == OF_OBJ_NOTE) && o->text) {
                    if (p->at[i2] != cur) { run_font(p, p->at[i2], &rf); cur = p->at[i2]; }
                    const lv_font_t *sf = o->kind == OF_OBJ_NOTE ? ofd_font(0, rf.px * 6 / 10 > 10 ? rf.px * 6 / 10 : 10, NULL, NULL) : rf.f;
                    for (const char *t = o->text; *t;) {
                        int l2;
                        uint32_t cp = of_utf8_get(t, &l2);
                        gw += lv_font_get_glyph_width(sf, cp, 0);
                        t += l2;
                    }
                } else if (o && o->kind == OF_OBJ_BREAK) {
                    /* a page break ends the line */
                    if (i2 > a) break;
                    i2 += 3;
                    h = em;
                    break;
                }
            } else {
                if (p->at[i2] != cur) { run_font(p, p->at[i2], &rf); cur = p->at[i2]; }
                uint32_t cp = of_utf8_get(p->txt + i2, &len);
                if (cp == '\n') {
                    i2 += 1;
                    if (!asc) { asc = rf.f->line_height - rf.f->base_line; desc = rf.f->base_line; }
                    break;
                }
                gw = glyph_w(rf.f, cp, 0);
                ga = rf.f->line_height - rf.f->base_line;
                gd = rf.f->base_line;
                if (cp == ' ') last_space = i2;
            }
            if (x + gw > maxw && i2 > a) {
                if (last_space >= a && last_space + 1 < i2 && !(is_obj(p, i2) && gw > maxw / 2)) {
                    i2 = last_space + 1;
                    /* the width up to there */
                    x = -1;
                }
                break;
            }
            x += gw;
            if (ga > asc) asc = ga;
            if (gd > desc) desc = gd;
            i2 += len;
        }
        if (i2 == a) i2 = a + (is_obj(p, a) ? 3 : of_utf8_len((uint8_t)p->txt[a]));
        /* the line's own width (measured again when it broke at a space) */
        int32_t lw = 0;
        {
            uint16_t c2 = 0xFFFF;
            runfont_t r2;
            for (int k = a; k < i2;) {
                if (is_obj(p, k)) {
                    lw += obj_width(p, k, maxw);
                    k += 3;
                    continue;
                }
                if (p->at[k] != c2) { run_font(p, p->at[k], &r2); c2 = p->at[k]; }
                int l2;
                uint32_t cp = of_utf8_get(p->txt + k, &l2);
                if (cp != '\n') lw += glyph_w(r2.f, cp, 0);
                k += l2;
            }
        }
        (void)x;
        if (!asc) {
            run_font(p, p->at[a], &rf);
            asc = rf.f->line_height - rf.f->base_line;
            desc = rf.f->base_line;
        }
        int32_t lh = (asc + desc) * 115 / 100;
        if (h > lh) lh = h;
        int32_t y = pl->n ? pl->l[pl->n - 1].y + pl->l[pl->n - 1].h : 0;
        int32_t off = 0;
        int32_t trimmed = lw;
        if (p->align == OF_AL_CENTER) off = (maxw - trimmed) / 2;
        else if (p->align == OF_AL_RIGHT) off = maxw - trimmed;
        if (off < 0) off = 0;
        line_push(pl, (line_t){ a, i2, y, lh, asc, off, lw });
        a = i2;
        cur = 0xFFFF;
    }
    pl->h = pl->n ? pl->l[pl->n - 1].y + pl->l[pl->n - 1].h : 0;
}

static int32_t para_gap(const of_para_t *p)
{
    int32_t em = (int32_t)(11 * ZPT[D.z]);
    if (p->heading) return em * 6 / 10;
    if (p->cell) return em / 6;
    if (p->label[0]) return em / 5;
    return em / 2;
}

/* positions of every paragraph and table, lines laid out where needed */
static void layout_all(void)
{
    of_doc_t *d = D.d;
    if (D.npl != d->np) {
        invalidate_all();
        D.pl = of_alloc((size_t)(d->np + 1) * sizeof *D.pl);
        if (!D.pl) return;
        D.npl = d->np;
    }
    for (int t = 0; t < D.ntl; t++) of_free(D.tl[t].rowy);
    of_free(D.tl);
    D.ntl = d->ntables;
    D.tl = d->ntables ? of_alloc((size_t)d->ntables * sizeof *D.tl) : NULL;
    int32_t margin = D.page_w > 900 ? 56 : 28;
    int32_t x0 = D.page_x + margin, w = D.page_w - 2 * margin;
    int32_t y = 24;
    int i = 0;
    while (i < d->np) {
        of_para_t *p = &d->p[i];
        if (!p->cell) {
            play_t *pl = &D.pl[i];
            if (p->heading) y += (int32_t)(11 * ZPT[D.z]) / 2;
            if (!pl->valid || pl->w != w) { layout_para(i, w); pl->valid = true; }
            pl->x = x0;
            pl->y = y;
            y += pl->h + para_gap(p);
            i++;
            continue;
        }
        /* a table: its paragraphs, cell by cell */
        int t = d->cells[p->cell - 1].table;
        int j = i;
        while (j < d->np && d->p[j].cell && d->cells[d->p[j].cell - 1].table == t) j++;
        of_table_t *T = &d->tables[t];
        tlay_t *L = &D.tl[t];
        L->ncols = T->ncols > 0 ? T->ncols : 1;
        L->nrows = T->nrows > 0 ? T->nrows : 1;
        int32_t sum = 0;
        for (int c = 0; c < L->ncols; c++) sum += T->colw[c] > 0 ? T->colw[c] : 0;
        L->colx[0] = x0;
        for (int c = 0; c < L->ncols; c++) {
            int32_t cw = sum > 0 && T->colw[c] > 0 ? (int32_t)((int64_t)w * T->colw[c] / sum) : w / L->ncols;
            L->colx[c + 1] = L->colx[c] + cw;
        }
        L->colx[L->ncols] = x0 + w;
        L->rowy = of_alloc((size_t)(L->nrows + 1) * sizeof(int32_t));
        L->y0 = y;
        int32_t pad = 8;
        /* each row: its cells' heights */
        int32_t ry = y;
        int k = i;
        for (int r = 0; r < L->nrows && L->rowy; r++) {
            L->rowy[r] = ry;
            int32_t rh = (int32_t)(11 * ZPT[D.z]) + 2 * pad;
            int k2 = k;
            while (k2 < j && d->cells[d->p[k2].cell - 1].row == r) {
                int cell = d->p[k2].cell;
                const of_cell_t2 *c = &d->cells[cell - 1];
                int32_t cx = L->colx[c->col < L->ncols ? c->col : L->ncols - 1];
                int ce = c->col + c->span;
                int32_t cx2 = L->colx[ce <= L->ncols ? ce : L->ncols];
                int32_t cy = ry + pad;
                while (k2 < j && d->p[k2].cell == cell) {
                    play_t *pl = &D.pl[k2];
                    int32_t cw = cx2 - cx - 2 * pad;
                    if (cw < 20) cw = 20;
                    if (!pl->valid || pl->w != cw) { layout_para(k2, cw); pl->valid = true; }
                    pl->x = cx + pad;
                    pl->y = cy;
                    cy += pl->h + para_gap(&d->p[k2]);
                    k2++;
                }
                if (cy - ry + pad > rh) rh = cy - ry + pad;
            }
            ry += rh;
            k = k2;
        }
        if (L->rowy) L->rowy[L->nrows] = ry;
        L->y1 = ry;
        y = ry + (int32_t)(11 * ZPT[D.z]) / 2;
        i = j;
    }
    D.total_h = y + 48;
}

static void invalidate_para(int i)
{
    if (i >= 0 && i < D.npl) D.pl[i].valid = false;
}

/* everything laid out again (the paragraphs moved: their lines go too) */
static void invalidate_all(void)
{
    for (int i = 0; i < D.npl; i++) of_free(D.pl[i].l);
    of_free(D.pl);
    D.pl = NULL;
    D.npl = 0;
}

/* -------------------------------------------------------------------------- */
/* Positions                                                                   */
/* -------------------------------------------------------------------------- */

static int line_of(const play_t *pl, int pos)
{
    for (int k = 0; k < pl->n; k++)
        if (pos < pl->l[k].b || k == pl->n - 1) return k;
    return 0;
}

/* x of a position within its line, from the line's start */
static int32_t x_in_line(int pi, const line_t *L, int pos)
{
    const of_para_t *p = &D.d->p[pi];
    const play_t *pl = &D.pl[pi];
    int32_t x = 0;
    uint16_t cur = 0xFFFF;
    runfont_t rf;
    for (int k = L->a; k < pos && k < L->b;) {
        if (is_obj(p, k)) {
            x += obj_width(p, k, pl->w - pl->indent);
            k += 3;
            continue;
        }
        if (p->at[k] != cur) { run_font(p, p->at[k], &rf); cur = p->at[k]; }
        int l2;
        uint32_t cp = of_utf8_get(p->txt + k, &l2);
        if (cp != '\n') x += glyph_w(rf.f, cp, 0);
        k += l2;
    }
    return x;
}

static void caret_xy(int pi, int pos, int32_t *x, int32_t *y, int32_t *h)
{
    const play_t *pl = &D.pl[pi];
    int k = line_of(pl, pos);
    const line_t *L = &pl->l[k];
    *x = pl->x + pl->indent + L->x + x_in_line(pi, L, pos);
    *y = pl->y + L->y;
    *h = L->h;
}

/* the paragraph and position under a point of the page */
static bool hit(int32_t px, int32_t py, int *pi, int *out_pos)
{
    of_doc_t *d = D.d;
    int best = -1;
    int32_t bestd = 0x7FFFFFFF;
    for (int i = 0; i < d->np; i++) {
        const play_t *pl = &D.pl[i];
        if (px < pl->x - 12 || px > pl->x + pl->w + 12) continue;
        int32_t dy = py < pl->y ? pl->y - py : py > pl->y + pl->h ? py - pl->y - pl->h : 0;
        if (dy < bestd) { bestd = dy; best = i; }
        if (!dy) break;
    }
    if (best < 0) return false;
    const play_t *pl = &D.pl[best];
    const of_para_t *p = &d->p[best];
    int k = 0;
    while (k + 1 < pl->n && py >= pl->y + pl->l[k + 1].y) k++;
    const line_t *L = &pl->l[k];
    int32_t x = px - pl->x - pl->indent - L->x;
    int32_t acc = 0;
    int pos = L->a;
    uint16_t cur = 0xFFFF;
    runfont_t rf;
    while (pos < L->b) {
        int32_t gw;
        int len;
        if (is_obj(p, pos)) {
            gw = obj_width(p, pos, pl->w - pl->indent);
            len = 3;
        } else {
            if (p->at[pos] != cur) { run_font(p, p->at[pos], &rf); cur = p->at[pos]; }
            uint32_t cp = of_utf8_get(p->txt + pos, &len);
            if (cp == '\n') break;
            gw = glyph_w(rf.f, cp, 0);
        }
        if (acc + gw / 2 > x) break;
        acc += gw;
        pos += len;
    }
    /* a wrapped line ends before its last space */
    if (k + 1 < pl->n && pos == L->b && pos > L->a) pos--;
    *pi = best;
    *out_pos = pos;
    return true;
}

static void scroll_to_caret(void)
{
    int32_t x, y, h;
    caret_xy(D.cp, D.cpos, &x, &y, &h);
    if (y - 20 < D.sy) D.sy = y - 20;
    else if (y + h + 20 > D.sy + D.ah) D.sy = y + h + 20 - D.ah;
    int32_t maxy = D.total_h - D.ah;
    if (D.sy > maxy) D.sy = maxy;
    if (D.sy < 0) D.sy = 0;
}

static void clamp(void)
{
    int32_t maxy = D.total_h - D.ah;
    if (D.sy > maxy) D.sy = maxy;
    if (D.sy < 0) D.sy = 0;
}

/* -------------------------------------------------------------------------- */
/* Drawing                                                                     */
/* -------------------------------------------------------------------------- */

static void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, lv_opa_t opa)
{
    if (x2 < x1 || y2 < y1) return;
    lv_draw_rect_dsc_t dd;
    lv_draw_rect_dsc_init(&dd);
    dd.bg_color = c;
    dd.bg_opa = opa;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &dd, &a);
}

static void text(lv_layer_t *layer, const char *s, int n, const lv_font_t *f, lv_color_t c, int32_t x, int32_t y)
{
    char buf[512];
    int k = 0;
    for (int i = 0; i < n && k < (int)sizeof buf - 5; i++) {
        if (s[i] == '\t') { memcpy(buf + k, "    ", 4); k += 4; }
        else if (s[i] == '\n') continue;
        else buf[k++] = s[i];
    }
    buf[k] = 0;
    if (!k) return;
    lv_draw_label_dsc_t dd;
    lv_draw_label_dsc_init(&dd);
    dd.font = f;
    dd.color = c;
    dd.text = buf;
    dd.text_local = 1;
    lv_area_t a = { x, y, x + 4000, y + f->line_height };
    lv_draw_label(layer, &dd, &a);
}

/* a colour of the document on the page: one too close to the paper is
 * lightened (dark page) or darkened (light page), keeping its hue */
static lv_color_t ink_of(uint32_t rgb)
{
    if (!rgb) return OFT.ink;
    lv_color_t c = lv_color_hex(rgb & 0xFFFFFF);
    lv_color_t bg = OFT.paper;
    int lc = (c.red * 3 + c.green * 6 + c.blue) / 10, lb = (bg.red * 3 + bg.green * 6 + bg.blue) / 10;
    if (lc - lb < 60 && lb - lc < 60) {
        if (!(c.red > 40 || c.green > 40 || c.blue > 40) && OFT.dark) return OFT.ink;      /* black text */
        return lv_color_mix(OFT.dark ? lv_color_white() : lv_color_black(), c, 110);
    }
    return c;
}

static bool sel_has(void)
{
    return D.cp != D.ap || D.cpos != D.apos;
}

static void sel_norm(int *p1, int *o1, int *p2, int *o2)
{
    if (D.ap < D.cp || (D.ap == D.cp && D.apos <= D.cpos)) { *p1 = D.ap; *o1 = D.apos; *p2 = D.cp; *o2 = D.cpos; }
    else { *p1 = D.cp; *o1 = D.cpos; *p2 = D.ap; *o2 = D.apos; }
}

static void draw_para(lv_layer_t *layer, int pi, int32_t oy)
{
    of_para_t *p = &D.d->p[pi];
    play_t *pl = &D.pl[pi];
    int32_t top = pl->y - D.sy + oy;
    int sp1, so1, sp2, so2;
    sel_norm(&sp1, &so1, &sp2, &so2);
    bool sel = sel_has() && pi >= sp1 && pi <= sp2;
    if (p->locked) fill(layer, pl->x - 14, top, pl->x - 10, top + pl->h, OFT.dim, LV_OPA_60);
    /* the label */
    if (p->label[0] && pl->n) {
        runfont_t rf;
        run_font(p, p->len ? p->at[0] : p->mark, &rf);
        const line_t *L = &pl->l[0];
        int32_t em = (int32_t)(11 * ZPT[D.z]);
        int32_t lx = pl->x + pl->indent - em * 3 / 2;
        text(layer, p->label, (int)strlen(p->label), rf.f, OFT.ink, lx, top + L->y + L->asc - (rf.f->line_height - rf.f->base_line));
    }
    for (int k = 0; k < pl->n; k++) {
        const line_t *L = &pl->l[k];
        int32_t ly = top + L->y;
        if (ly > D.ay + D.ah || ly + L->h < D.ay) continue;
        int32_t x = pl->x + pl->indent + L->x;
        /* the selection behind it */
        if (sel) {
            int a = pi == sp1 ? so1 : 0, b = pi == sp2 ? so2 : p->len;
            if (b > L->a && a <= L->b) {
                int aa = a > L->a ? a : L->a, bb = b < L->b ? b : L->b;
                int32_t x1 = x + x_in_line(pi, L, aa), x2 = x + x_in_line(pi, L, bb);
                if (b > L->b || (pi < sp2 && k == pl->n - 1)) x2 += 8;
                fill(layer, x1, ly, x2, ly + L->h - 1, OFT.sel, LV_OPA_40);
            }
        }
        int i = L->a;
        while (i < L->b) {
            if (is_obj(p, i)) {
                int o_ix = -1;
                {
                    int kk = 0;
                    for (int q = 0; q + 2 < p->len && q < i; q++)
                        if (!memcmp(p->txt + q, OF_OBJ_CHAR, 3)) { kk++; q += 2; }
                    o_ix = kk < p->nobj ? p->obj[kk] : -1;
                }
                of_obj_t *o = o_ix >= 0 ? &D.d->obj[o_ix] : NULL;
                if (o && o->kind == OF_OBJ_IMAGE) {
                    int32_t iw, ih;
                    obj_size(o, pl->w - pl->indent, &iw, &ih);
                    int32_t iy = ly + L->asc - ih;
                    pic_t *pc = pic_of(o_ix);
                    if (pc && pc->px) {
                        /* the bitmap is at most the box: scaled up to it */
                        lv_draw_image_dsc_t id;
                        lv_draw_image_dsc_init(&id);
                        id.src = &pc->dsc;
                        int32_t sc = pc->w ? iw * 256 / pc->w : 256;
                        id.scale_x = id.scale_y = sc;
                        id.pivot.x = 0;
                        id.pivot.y = 0;
                        lv_area_t a = { x + 2, iy, x + 2 + pc->w - 1, iy + pc->h - 1 };
                        lv_draw_image(layer, &id, &a);
                    } else {
                        fill(layer, x + 2, iy, x + 2 + iw - 1, iy + ih - 1, OFT.head, LV_OPA_COVER);
                        lv_area_t a = { x + 2, iy, x + 2 + iw - 1, iy + ih - 1 };
                        ofu_draw_icon(layer, OF_IC_PALETTE, &a, OFT.dim, 3);
                        if (pc && !pc->failed && iy + ih > D.ay && iy < D.ay + D.ah) pc->wanted = true;
                    }
                    x += iw + 4;
                } else if (o && (o->kind == OF_OBJ_TEXT || o->kind == OF_OBJ_NOTE) && o->text) {
                    runfont_t rf;
                    run_font(p, p->at[i], &rf);
                    bool note = o->kind == OF_OBJ_NOTE;
                    const lv_font_t *sf = note ? ofd_font(0, rf.px * 6 / 10 > 10 ? rf.px * 6 / 10 : 10, NULL, NULL) : rf.f;
                    text(layer, o->text, (int)strlen(o->text), sf, note ? lv_color_hex(0x2F6FD0) : ink_of(rf.e.color), x,
                         ly + L->asc - (rf.f->line_height - rf.f->base_line) - (note ? 2 : 0));
                    for (const char *t = o->text; *t;) {
                        int l2;
                        uint32_t cp = of_utf8_get(t, &l2);
                        x += lv_font_get_glyph_width(sf, cp, 0);
                        t += l2;
                    }
                } else if (o && o->kind == OF_OBJ_BREAK) {
                    for (int32_t dx = pl->x; dx < pl->x + pl->w; dx += 16) fill(layer, dx, ly + L->h / 2, dx + 8, ly + L->h / 2 + 1, OFT.dim, LV_OPA_COVER);
                }
                i += 3;
                continue;
            }
            /* a run of one style */
            int j = i;
            while (j < L->b && p->at[j] == p->at[i] && !is_obj(p, j)) j++;
            runfont_t rf;
            run_font(p, p->at[i], &rf);
            int32_t w = 0;
            for (int q = i; q < j;) {
                int l2;
                uint32_t cp = of_utf8_get(p->txt + q, &l2);
                if (cp != '\n') w += glyph_w(rf.f, cp, 0);
                q += l2;
            }
            int32_t fasc = rf.f->line_height - rf.f->base_line;
            int32_t ty = ly + L->asc - fasc - (rf.e.sup == 1 ? rf.px / 2 : rf.e.sup == 2 ? -rf.px / 5 : 0);
            lv_color_t c = ink_of(rf.e.color);
            if (rf.e.hl) fill(layer, x, ly + 2, x + w, ly + L->h - 2, lv_color_hex(rf.e.hl & 0xFFFFFF), LV_OPA_70);
            text(layer, p->txt + i, j - i, rf.f, c, x, ty);
            if (rf.faux) text(layer, p->txt + i, j - i, rf.f, c, x + 1, ty);
            if (rf.e.u == OF_TRI_ON) fill(layer, x, ly + L->asc + 2, x + w, ly + L->asc + 3, c, LV_OPA_COVER);
            if (rf.e.s == OF_TRI_ON) fill(layer, x, ly + L->asc - fasc / 3, x + w, ly + L->asc - fasc / 3 + 1, c, LV_OPA_COVER);
            x += w;
            i = j;
        }
    }
}

static void area_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(D.area, &a);
    D.ax = a.x1;
    D.ay = a.y1;
    lv_area_t keep = layer->_clip_area, cl;
    if (!ofu_clip(&cl, &keep, &a)) return;
    layer->_clip_area = cl;
    fill(layer, cl.x1, cl.y1, cl.x2, cl.y2, OFT.bg, LV_OPA_COVER);
    int32_t oy = D.ay;
    /* the page: paper from its top to its end */
    fill(layer, D.ax + D.page_x, oy + 8 - D.sy, D.ax + D.page_x + D.page_w - 1, oy + D.total_h - 16 - D.sy, OFT.paper, LV_OPA_COVER);
    of_doc_t *d = D.d;
    /* table grids */
    for (int t = 0; t < D.ntl; t++) {
        tlay_t *L = &D.tl[t];
        if (!L->rowy || L->y1 - D.sy + oy < D.ay || L->y0 - D.sy + oy > D.ay + D.ah) continue;
        int32_t x1 = D.ax + L->colx[0], x2 = D.ax + L->colx[L->ncols];
        for (int r = 0; r <= L->nrows; r++) {
            int32_t y = L->rowy[r] - D.sy + oy;
            fill(layer, x1, y, x2, y, OFT.grid, LV_OPA_COVER);
        }
        for (int c = 0; c <= L->ncols; c++) {
            int32_t x = D.ax + L->colx[c];
            fill(layer, x, L->y0 - D.sy + oy, x, L->y1 - D.sy + oy, OFT.grid, LV_OPA_COVER);
        }
    }
    /* the paragraphs in sight */
    for (int i = 0; i < d->np; i++) {
        const play_t *pl = &D.pl[i];
        int32_t y = pl->y - D.sy + oy;
        if (y > D.ay + D.ah || y + pl->h < D.ay) continue;
        /* page coordinates are from the area's left */
        play_t *m = &D.pl[i];
        int32_t keepx = m->x;
        m->x = keepx + D.ax;
        draw_para(layer, i, oy);
        m->x = keepx;
    }
    /* the caret */
    if (!sel_has() && D.kb) {
        int32_t x, y, h;
        caret_xy(D.cp, D.cpos, &x, &y, &h);
        fill(layer, D.ax + x, oy + y - D.sy + 2, D.ax + x + 2, oy + y - D.sy + h - 3, OFT.sel, LV_OPA_COVER);
    }
    layer->_clip_area = keep;
}

/* -------------------------------------------------------------------------- */
/* Undo                                                                        */
/* -------------------------------------------------------------------------- */

static void entry_free(uentry_t *u)
{
    for (int i = 0; i < u->nbefore; i++) if (u->before && u->before[i]) { of_para_free_fields(u->before[i]); of_free(u->before[i]); }
    for (int i = 0; i < u->nafter; i++) if (u->after && u->after[i]) { of_para_free_fields(u->after[i]); of_free(u->after[i]); }
    of_free(u->before);
    of_free(u->after);
    memset(u, 0, sizeof *u);
}

/* Before an edit of paragraphs [first, first+n): copies, and how many there
 * will be after (filled in by undo_after). */
static uentry_t *undo_before(int first, int n)
{
    if (D.nundo == UNDO_MAX) {
        entry_free(&D.undo[0]);
        memmove(&D.undo[0], &D.undo[1], (UNDO_MAX - 1) * sizeof(uentry_t));
        D.nundo--;
    }
    for (int i = 0; i < D.nredo; i++) entry_free(&D.redo[i]);
    D.nredo = 0;
    uentry_t *u = &D.undo[D.nundo++];
    memset(u, 0, sizeof *u);
    u->first = first;
    u->nbefore = n;
    u->before = of_alloc((size_t)(n ? n : 1) * sizeof(of_para_t *));
    for (int i = 0; i < n && u->before; i++) u->before[i] = of_para_clone(&D.d->p[first + i]);
    u->cpara = D.cp;
    u->cpos = D.cpos;
    u->nafter = n;
    return u;
}

static void swap_in(int first, int nnow, of_para_t **with, int nwith)
{
    of_doc_t *d = D.d;
    for (int i = 0; i < nnow; i++) of_doc_remove_para(d, first);
    for (int i = 0; i < nwith; i++) {
        of_para_t *p = of_doc_insert_para(d, first + i);
        if (!p) break;
        of_para_free_fields(p);
        of_para_t *c = of_para_clone(with[i]);
        if (c) { *p = *c; of_free(c); }
        p->dirty = true;
    }
}

static void do_undo(void)
{
    if (!D.nundo) return;
    uentry_t u = D.undo[--D.nundo];
    /* what is there now, for redo */
    u.after = of_alloc((size_t)(u.nafter ? u.nafter : 1) * sizeof(of_para_t *));
    for (int i = 0; i < u.nafter && u.after; i++) u.after[i] = of_para_clone(&D.d->p[u.first + i]);
    swap_in(u.first, u.nafter, u.before, u.nbefore);
    D.redo[D.nredo++] = u;
    D.cp = D.ap = u.cpara < D.d->np ? u.cpara : D.d->np - 1;
    D.cpos = D.apos = u.cpos <= D.d->p[D.cp].len ? u.cpos : D.d->p[D.cp].len;
    D.modified = true;
    D.typing_group = false;
    of_doc_number(D.d);
    invalidate_all();
    layout_all();
    scroll_to_caret();
    refresh();
}

static void do_redo(void)
{
    if (!D.nredo) return;
    uentry_t u = D.redo[--D.nredo];
    swap_in(u.first, u.nbefore, u.after, u.nafter);
    for (int i = 0; i < u.nafter; i++) { of_para_free_fields(u.after[i]); of_free(u.after[i]); }
    of_free(u.after);
    u.after = NULL;
    D.undo[D.nundo++] = u;
    D.cp = D.ap = u.first + (u.nafter ? u.nafter - 1 : 0) < D.d->np ? u.first + (u.nafter ? u.nafter - 1 : 0) : D.d->np - 1;
    D.cpos = D.apos = D.d->p[D.cp].len;
    D.modified = true;
    D.typing_group = false;
    of_doc_number(D.d);
    invalidate_all();
    layout_all();
    scroll_to_caret();
    refresh();
}

/* -------------------------------------------------------------------------- */
/* Editing                                                                     */
/* -------------------------------------------------------------------------- */

/* A document read from another format is not edited: a copy is. */
static bool refuse_ro(void)
{
    if (!D.d->readonly) return false;
    of_app_toast(_("Sólo se lee: para editarlo, guardá una copia como docx u odt (en el menú ⋯)"));
    return true;
}

static bool refuse_locked(int pi)
{
    if (refuse_ro()) return true;
    if (pi >= 0 && pi < D.d->np && D.d->p[pi].locked) {
        of_app_toast(_("Este párrafo tiene control de cambios u objetos que no se editan acá: se guarda como está"));
        return true;
    }
    return false;
}

static void edited(int from, int to)
{
    for (int i = from; i <= to && i < D.d->np; i++) invalidate_para(i);
    D.modified = true;
    of_doc_number(D.d);
    layout_all();
}

/* Deletes the selection; false if it cannot (it crosses cells or locks). */
static bool delete_selection(bool record)
{
    if (!sel_has()) return true;
    if (refuse_ro()) return false;
    int p1, o1, p2, o2;
    sel_norm(&p1, &o1, &p2, &o2);
    for (int i = p1; i <= p2; i++) {
        if (D.d->p[i].locked) { refuse_locked(i); return false; }
        if (D.d->p[i].cell != D.d->p[p1].cell) {
            of_app_toast(_("La selección pasa de una celda a otra: se borra de a una celda"));
            return false;
        }
    }
    if (record) undo_before(p1, p2 - p1 + 1)->nafter = 1;
    if (p1 == p2) {
        of_para_delete(D.d, p1, o1, o2 - o1);
    } else {
        of_para_delete(D.d, p2, 0, o2);
        for (int i = p2 - 1; i > p1; i--) of_doc_remove_para(D.d, i);
        of_para_delete(D.d, p1, o1, D.d->p[p1].len - o1);
        of_doc_merge(D.d, p1 + 1);
    }
    D.cp = D.ap = p1;
    D.cpos = D.apos = o1;
    D.typing_group = false;
    invalidate_all();
    edited(p1, p1);
    return true;
}

static uint16_t typing_style(void)
{
    if (D.typing != 0xFFFF) return D.typing;
    of_para_t *p = &D.d->p[D.cp];
    if (D.cpos > 0) {
        int k = D.cpos - 1;
        while (k > 0 && (p->txt[k] & 0xC0) == 0x80) k--;
        /* the style of the letter before, but not an object's */
        if (!(k >= 2 && !memcmp(p->txt + k - 2, OF_OBJ_CHAR, 3))) return p->at[k];
    }
    return p->len ? p->at[0] : p->mark;
}

static void k_insert(const char *s)
{
    if (refuse_locked(D.cp)) return;
    uint32_t now = lv_tick_get();
    if (sel_has()) {
        if (!delete_selection(true)) return;
        D.typing_group = false;
    }
    if (!D.typing_group || now - D.last_type_ms > 2000 || D.nundo == 0 || D.undo[D.nundo - 1].first != D.cp ||
        D.undo[D.nundo - 1].nafter != 1 || D.undo[D.nundo - 1].nbefore != 1) {
        undo_before(D.cp, 1);
        D.typing_group = true;
    }
    D.last_type_ms = now;
    uint16_t st = typing_style();
    int n = (int)strlen(s);
    of_para_insert(D.d, D.cp, D.cpos, s, n, st);
    D.cpos += n;
    D.apos = D.cpos;
    D.typing = 0xFFFF;
    D.want_x = -1;
    edited(D.cp, D.cp);
    scroll_to_caret();
    refresh();
}

static int prev_pos(const of_para_t *p, int pos)
{
    if (pos <= 0) return 0;
    if (pos >= 3 && !memcmp(p->txt + pos - 3, OF_OBJ_CHAR, 3)) return pos - 3;
    pos--;
    while (pos > 0 && (p->txt[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

static int next_pos(const of_para_t *p, int pos)
{
    if (pos >= p->len) return p->len;
    if (is_obj(p, pos)) return pos + 3;
    return pos + of_utf8_len((uint8_t)p->txt[pos]);
}

static void k_bksp(void)
{
    D.typing_group = false;
    if (sel_has()) { delete_selection(true); scroll_to_caret(); refresh(); return; }
    if (refuse_locked(D.cp)) return;
    of_para_t *p = &D.d->p[D.cp];
    if (D.cpos > 0) {
        int a = prev_pos(p, D.cpos);
        undo_before(D.cp, 1);
        of_para_delete(D.d, D.cp, a, D.cpos - a);
        D.cpos = D.apos = a;
        edited(D.cp, D.cp);
    } else if (of_doc_can_merge(D.d, D.cp)) {
        int at = D.d->p[D.cp - 1].len;
        undo_before(D.cp - 1, 2)->nafter = 1;
        of_doc_merge(D.d, D.cp);
        D.cp = D.ap = D.cp - 1;
        D.cpos = D.apos = at;
        invalidate_all();
        edited(D.cp, D.cp);
    } else if (D.cp > 0 && D.d->p[D.cp - 1].locked) {
        refuse_locked(D.cp - 1);
    }
    D.want_x = -1;
    scroll_to_caret();
    refresh();
}

static void k_enter(void)
{
    D.typing_group = false;
    if (sel_has() && !delete_selection(true)) return;
    if (refuse_locked(D.cp)) return;
    undo_before(D.cp, 1)->nafter = 2;
    if (!of_doc_split(D.d, D.cp, D.cpos)) return;
    D.cp = D.ap = D.cp + 1;
    D.cpos = D.apos = 0;
    invalidate_all();
    edited(D.cp - 1, D.cp);
    scroll_to_caret();
    refresh();
}

static void move_to(int pi, int pos, bool extend)
{
    D.cp = pi;
    D.cpos = pos;
    if (!extend) { D.ap = pi; D.apos = pos; }
    D.typing = 0xFFFF;
    D.typing_group = false;
    scroll_to_caret();
    refresh();
}

static void k_move(int dx)
{
    of_para_t *p = &D.d->p[D.cp];
    int pi = D.cp, pos = D.cpos;
    for (; dx < 0; dx++) {
        if (pos > 0) pos = prev_pos(p, pos);
        else if (pi > 0) { pi--; p = &D.d->p[pi]; pos = p->len; }
    }
    for (; dx > 0; dx--) {
        if (pos < p->len) pos = next_pos(p, pos);
        else if (pi + 1 < D.d->np) { pi++; p = &D.d->p[pi]; pos = 0; }
    }
    D.want_x = -1;
    move_to(pi, pos, false);
}

static bool k_caps(void)
{
    of_para_t *p = &D.d->p[D.cp];
    int k = D.cpos;
    while (k > 0 && p->txt[k - 1] == ' ') k--;
    if (!k) return true;
    char c = p->txt[k - 1];
    return (c == '.' || c == '?' || c == '!') && k < D.cpos;
}

static void vmove(int dir, bool extend)
{
    int32_t x, y, h;
    caret_xy(D.cp, D.cpos, &x, &y, &h);
    if (D.want_x < 0) D.want_x = x;
    int pi, pos;
    int32_t ty = dir < 0 ? y - 4 : y + h + 4;
    if (hit(D.want_x, ty, &pi, &pos)) {
        int32_t keep = D.want_x;
        move_to(pi, pos, extend);
        D.want_x = keep;
    }
}

/* formatting */
static void toggle(uint8_t bit)
{
    if (refuse_ro()) return;
    of_rstyle_t cur;
    of_para_t *p = &D.d->p[D.cp];
    int pos = D.cpos > 0 ? prev_pos(p, D.cpos) : 0;
    int p1, o1, p2, o2;
    sel_norm(&p1, &o1, &p2, &o2);
    if (sel_has()) { p = &D.d->p[p1]; pos = o1; }
    of_doc_effective(D.d, p, p->len ? p->at[pos < p->len ? pos : p->len - 1] : p->mark, &cur);
    uint8_t on = bit == OF_RS_B ? cur.b : bit == OF_RS_I ? cur.i : bit == OF_RS_U ? cur.u : cur.s;
    uint8_t want = on == OF_TRI_ON ? OF_TRI_OFF : OF_TRI_ON;
    of_rstyle_t v = { .b = want, .i = want, .u = want, .s = want };
    if (!sel_has()) {
        /* for what is typed next */
        of_rstyle_t s = D.d->rs[typing_style()];
        if (bit == OF_RS_B) s.b = want;
        if (bit == OF_RS_I) s.i = want;
        if (bit == OF_RS_U) s.u = want;
        if (bit == OF_RS_S) s.s = want;
        s.mask |= bit;
        s.raw = s.raw ? of_strdup(s.raw) : NULL;
        D.typing = (uint16_t)of_doc_style(D.d, &s);
        of_free(s.raw);
        refresh();
        return;
    }
    for (int i = p1; i <= p2; i++) if (D.d->p[i].locked) { refuse_locked(i); return; }
    undo_before(p1, p2 - p1 + 1);
    for (int i = p1; i <= p2; i++) {
        int a = i == p1 ? o1 : 0, b = i == p2 ? o2 : D.d->p[i].len;
        of_para_restyle(D.d, i, a, b - a, bit, &v);
    }
    D.typing_group = false;
    edited(p1, p2);
    refresh();
}

static void set_heading(int level)
{
    if (refuse_ro()) return;
    int p1, o1, p2, o2;
    sel_norm(&p1, &o1, &p2, &o2);
    for (int i = p1; i <= p2; i++) if (D.d->p[i].locked) { refuse_locked(i); return; }
    undo_before(p1, p2 - p1 + 1);
    for (int i = p1; i <= p2; i++) {
        of_para_t *p = &D.d->p[i];
        p->heading = (uint8_t)level;
        if (!level) p->pstyle = D.d->ps_normal;
        else {
            p->pstyle = -1;
            for (int s = 0; s < D.d->nps; s++)
                if (D.d->ps[s].heading == level) { p->pstyle = s; break; }
        }
        p->pmask |= OF_PP_STYLE;
        p->dirty = true;
    }
    D.typing_group = false;
    edited(p1, p2);
    refresh();
}

static void set_align(int al)
{
    if (refuse_ro()) return;
    int p1, o1, p2, o2;
    sel_norm(&p1, &o1, &p2, &o2);
    for (int i = p1; i <= p2; i++) if (D.d->p[i].locked) { refuse_locked(i); return; }
    undo_before(p1, p2 - p1 + 1);
    for (int i = p1; i <= p2; i++) {
        D.d->p[i].align = (uint8_t)al;
        D.d->p[i].pmask |= OF_PP_ALIGN;
        D.d->p[i].dirty = true;
    }
    edited(p1, p2);
    refresh();
}

/* clipboard: plain text */
static void copy(bool cut)
{
    if (!sel_has()) return;
    int p1, o1, p2, o2;
    sel_norm(&p1, &o1, &p2, &o2);
    of_sb_t sb = { 0 };
    for (int i = p1; i <= p2; i++) {
        of_para_t *p = &D.d->p[i];
        int a = i == p1 ? o1 : 0, b = i == p2 ? o2 : p->len;
        for (int k = a; k < b; k++) {
            if (is_obj(p, k)) { k += 2; continue; }
            of_sb_putc(&sb, p->txt[k]);
        }
        if (i < p2) of_sb_putc(&sb, '\n');
    }
    if (sb.n < 256 * 1024) aos_ui_clip_set(sb.s ? sb.s : "");
    of_sb_free(&sb);
    if (cut) { delete_selection(true); refresh(); }
}

static void paste(void)
{
    char *buf = of_alloc(64 * 1024);
    if (!buf) return;
    size_t n = aos_ui_clip_get(buf, 64 * 1024);
    if (!n) { of_free(buf); return; }
    if (refuse_locked(D.cp)) { of_free(buf); return; }
    if (sel_has() && !delete_selection(true)) { of_free(buf); return; }
    /* line by line: each line end is a new paragraph */
    undo_before(D.cp, 1);
    int start = D.cp, lines = 0;
    char *s = buf;
    while (*s) {
        char *e = strchr(s, '\n');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l && s[l - 1] == '\r') l--;
        uint16_t st = typing_style();
        of_para_insert(D.d, D.cp, D.cpos, s, (int)l, st);
        D.cpos += (int)l;
        if (!e) break;
        of_doc_split(D.d, D.cp, D.cpos);
        D.cp++;
        D.cpos = 0;
        lines++;
        s = e + 1;
    }
    D.undo[D.nundo - 1].nafter = lines + 1;
    D.ap = D.cp;
    D.apos = D.cpos;
    of_free(buf);
    invalidate_all();
    edited(start, D.cp);
    scroll_to_caret();
    refresh();
}

static void select_word(int pi, int pos)
{
    of_para_t *p = &D.d->p[pi];
    int a = pos, b = pos;
    while (a > 0 && p->txt[a - 1] != ' ' && p->txt[a - 1] != '\t' && !(a >= 3 && !memcmp(p->txt + a - 3, OF_OBJ_CHAR, 3))) a--;
    while (b < p->len && p->txt[b] != ' ' && p->txt[b] != '\t' && !is_obj(p, b)) b++;
    D.ap = D.cp = pi;
    D.apos = a;
    D.cpos = b;
}

/* -------------------------------------------------------------------------- */
/* Find                                                                        */
/* -------------------------------------------------------------------------- */

static void find_next(void)
{
    if (!D.find[0]) return;
    size_t nl = strlen(D.find);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = pass ? 0 : D.cp; i < D.d->np; i++) {
            of_para_t *p = &D.d->p[i];
            int from = !pass && i == D.cp ? D.cpos : 0;
            for (int k = from; k + (int)nl <= p->len; k++)
                if (!strncasecmp(p->txt + k, D.find, nl)) {
                    D.ap = D.cp = i;
                    D.apos = k;
                    D.cpos = k + (int)nl;
                    scroll_to_caret();
                    refresh();
                    return;
                }
        }
    }
    of_app_toast(_("No está"));
}

static void find_done(const char *text, void *ud)
{
    (void)ud;
    snprintf(D.find, sizeof D.find, "%s", text);
    find_next();
}

/* -------------------------------------------------------------------------- */
/* Keyboard target                                                             */
/* -------------------------------------------------------------------------- */

static bool k_key(uint32_t k, uint8_t mods)
{
    bool shift = mods & OF_MOD_SHIFT, cmd = mods & OF_MOD_CMD;
    if (cmd) {
        switch (k | 0x20) {
        case 'b': toggle(OF_RS_B); return true;
        case 'i': toggle(OF_RS_I); return true;
        case 'u': toggle(OF_RS_U); return true;
        case 'z': if (shift) do_redo(); else do_undo(); return true;
        case 'y': do_redo(); return true;
        case 'c': copy(false); return true;
        case 'x': copy(true); return true;
        case 'v': paste(); return true;
        case 's': of_docv_save(); return true;
        case 'f': ofu_prompt(_("Buscar en el documento"), D.find, _("Buscar"), find_done, NULL); return true;
        case 'g': find_next(); return true;
        case 'a':
            D.ap = 0; D.apos = 0; D.cp = D.d->np - 1; D.cpos = D.d->p[D.cp].len;
            refresh();
            return true;
        default: return true;
        }
    }
    switch (k) {
    case AOS_KEY_LEFT: {
        int pi = D.cp, pos = D.cpos;
        if (pos > 0) pos = prev_pos(&D.d->p[pi], pos);
        else if (pi > 0) { pi--; pos = D.d->p[pi].len; }
        D.want_x = -1;
        move_to(pi, pos, shift);
        return true;
    }
    case AOS_KEY_RIGHT: {
        int pi = D.cp, pos = D.cpos;
        if (pos < D.d->p[pi].len) pos = next_pos(&D.d->p[pi], pos);
        else if (pi + 1 < D.d->np) { pi++; pos = 0; }
        D.want_x = -1;
        move_to(pi, pos, shift);
        return true;
    }
    case AOS_KEY_UP: vmove(-1, shift); return true;
    case AOS_KEY_DOWN: vmove(1, shift); return true;
    case AOS_KEY_HOME: case AOS_KEY_END: {
        const play_t *pl = &D.pl[D.cp];
        const line_t *L = &pl->l[line_of(pl, D.cpos)];
        int pos = k == AOS_KEY_HOME ? L->a : L->b;
        if (k == AOS_KEY_END && L->b < D.d->p[D.cp].len && pos > L->a) pos = prev_pos(&D.d->p[D.cp], pos);
        move_to(D.cp, pos, shift);
        return true;
    }
    case AOS_KEY_NEXT: k_insert("\t"); return true;
    case AOS_KEY_DEL:
        if (sel_has()) { delete_selection(true); refresh(); return true; }
        if (D.cpos < D.d->p[D.cp].len) {
            if (refuse_locked(D.cp)) return true;
            undo_before(D.cp, 1);
            of_para_delete(D.d, D.cp, D.cpos, next_pos(&D.d->p[D.cp], D.cpos) - D.cpos);
            edited(D.cp, D.cp);
        } else if (D.cp + 1 < D.d->np && of_doc_can_merge(D.d, D.cp + 1)) {
            undo_before(D.cp, 2)->nafter = 1;
            of_doc_merge(D.d, D.cp + 1);
            invalidate_all();
            edited(D.cp, D.cp);
        }
        refresh();
        return true;
    case AOS_KEY_ESC:
        if (sel_has()) { D.ap = D.cp; D.apos = D.cpos; refresh(); }
        else kb_show(false);
        return true;
    default: break;
    }
    return false;
}

static const ofu_kb_target_t TARGET = { k_insert, k_bksp, k_enter, k_move, k_caps, k_key };

/* -------------------------------------------------------------------------- */
/* Gestures                                                                    */
/* -------------------------------------------------------------------------- */

static void fling_cb(lv_timer_t *t)
{
    (void)t;
    D.sy -= (int32_t)(D.vy / 60);
    D.vy *= 0.93f;
    int32_t o = D.sy;
    clamp();
    if (o != D.sy) D.vy = 0;
    lv_obj_invalidate(D.area);
    if (D.vy * D.vy < 400) { lv_timer_delete(D.fling); D.fling = NULL; }
}

static void stop_fling(void)
{
    if (D.fling) { lv_timer_delete(D.fling); D.fling = NULL; }
}

static void set_zoom(int z)
{
    if (z < 0 || z >= NZOOM || z == D.z) return;
    /* the paragraph at the top stays at the top */
    int top = 0;
    for (int i = 0; i < D.d->np; i++)
        if (D.pl[i].y + D.pl[i].h > D.sy) { top = i; break; }
    int32_t off = D.sy - D.pl[top].y;
    float ratio = ZPT[z] / ZPT[D.z];
    D.z = z;
    invalidate_all();
    layout_all();
    D.sy = D.pl[top].y + (int32_t)(off * ratio);
    clamp();
    aos_hal_pref_set_i32("of_dzoom", D.z);
    lv_obj_invalidate(D.area);
}

static void gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    (void)user;
    if (ofu_sheet_open()) return;
    lv_area_t a;
    lv_obj_get_coords(D.area, &a);
    int32_t px = (int32_t)ev->x - a.x1, py = (int32_t)ev->y - a.y1 + D.sy;
    int pi, pos;
    switch (ev->type) {
    case AOS_GESTURE_TAP:
        stop_fling();
        if (!hit(px, py, &pi, &pos)) break;
        D.want_x = -1;
        move_to(pi, pos, false);
        if (D.d->readonly) {
            of_para_t *lp = &D.d->p[pi];
            int at = pos < lp->len ? pos : pos - 1;
            int lk = at >= 0 && lp->len ? D.d->rs[lp->at[at]].link : -1;
            if (lk >= 0 && lk < D.d->nlinks && of_link_is_url(D.d->links[lk])) of_app_toast(D.d->links[lk]);
        } else if (D.d->p[pi].locked) {
            refuse_locked(pi);
        } else if (!D.kb) {
            kb_show(true);
        }
        break;
    case AOS_GESTURE_DOUBLE_TAP:
        if (hit(px, py, &pi, &pos)) { select_word(pi, pos); refresh(); }
        break;
    case AOS_GESTURE_LONG_PRESS:
        if (hit(px, py, &pi, &pos)) { select_word(pi, pos); D.selecting = true; refresh(); }
        break;
    case AOS_GESTURE_DRAG_BEGIN:
        stop_fling();
        D.selecting = ev->after_long;
        break;
    case AOS_GESTURE_DRAG:
        if (D.selecting) {
            if (hit(px, py, &pi, &pos)) { D.cp = pi; D.cpos = pos; refresh(); }
            break;
        }
        D.sy -= (int32_t)ev->dy;
        clamp();
        lv_obj_invalidate(D.area);
        break;
    case AOS_GESTURE_DRAG_END:
        if (!D.selecting && ev->vy * ev->vy > 90000) {
            D.vy = ev->vy;
            if (!D.fling) D.fling = lv_timer_create(fling_cb, 16, NULL);
        }
        D.selecting = false;
        break;
    case AOS_GESTURE_PINCH_BEGIN:
        stop_fling();
        D.pinch = 1;
        break;
    case AOS_GESTURE_PINCH:
        D.pinch *= ev->scale;
        if (D.pinch > 1.18f) { set_zoom(D.z + 1); D.pinch = 1; }
        else if (D.pinch < 0.85f) { set_zoom(D.z - 1); D.pinch = 1; }
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* Menus                                                                       */
/* -------------------------------------------------------------------------- */

static void style_pick(int i, void *ud) { (void)ud; set_heading(i); }
static void align_pick(int i, void *ud) { (void)ud; set_align(i); }

static void style_open(void)
{
    ofu_item_t items[4] = {
        { _("Texto normal"), OF_IC_DOC, false, D.d->p[D.cp].heading == 0 },
        { _("Título 1"), OF_IC_TYPE, false, D.d->p[D.cp].heading == 1 },
        { _("Título 2"), OF_IC_TYPE, false, D.d->p[D.cp].heading == 2 },
        { _("Título 3"), OF_IC_TYPE, false, D.d->p[D.cp].heading == 3 },
    };
    ofu_sheet(_("Estilo del párrafo"), items, 4, style_pick, NULL);
}

static void align_open(void)
{
    int al = D.d->p[D.cp].align;
    ofu_item_t items[4] = {
        { _("A la izquierda"), OF_IC_LIST, false, al == 0 },
        { _("Centrado"), OF_IC_LIST, false, al == 1 },
        { _("A la derecha"), OF_IC_LIST, false, al == 2 },
        { _("Justificado"), OF_IC_LIST, false, al == 3 },
    };
    ofu_sheet(_("Alineación"), items, 4, align_pick, NULL);
}

static int s_as_fmt;

static void save_as_done(const char *text, void *ud)
{
    (void)ud;
    static const char *const EXT[] = { "", "docx", "odt", "txt", "", "pdf" };
    if (!text[0] || strchr(text, '/')) return;
    char dir[300];
    snprintf(dir, sizeof dir, "%s", D.path);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0;
    char p[320];
    bool has = !strcasecmp(of_ext(text), EXT[s_as_fmt]);
    snprintf(p, sizeof p, "%.200s/%.100s%s%s", dir, text, has ? "" : ".", has ? "" : EXT[s_as_fmt]);
    if (of_file_exists(p)) { of_app_toast(_("Ya hay un archivo con ese nombre")); return; }
    char err[64];
    if (s_as_fmt == 5) {
        char title[128];
        snprintf(title, sizeof title, "%s", of_basename(D.path));
        char *dot = strrchr(title, '.');
        if (dot && dot != title) *dot = 0;
        if (!of_pdf_write_doc(D.d, p, title, false, false, err, sizeof err)) { of_app_toast(_("No se pudo guardar")); return; }
        of_app_toast(_("Guardado como PDF"));
        return;
    }
    if (!of_doc_save(D.d, s_as_fmt, p, err, sizeof err)) { of_app_toast(_("No se pudo guardar")); return; }
    of_recent_add(p);
    of_app_toast(s_as_fmt == D.d->fmt || s_as_fmt == OF_DOC_TEXT || D.d->readonly ? _("Guardado")
                 : _("Guardado. En la copia no va lo que la app no entiende"));
    /* a copy of a read-only document opens, to be edited */
    if (D.d->readonly && s_as_fmt != OF_DOC_TEXT) of_app_open(p);
}

static void save_as_pick(int i, void *ud)
{
    (void)ud;
    static const int F[] = { OF_DOC_DOCX, OF_DOC_ODT, OF_DOC_TEXT, 5 };
    s_as_fmt = F[i];
    char base[128];
    snprintf(base, sizeof base, "%s", of_basename(D.path));
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = 0;
    ofu_prompt(_("Guardar una copia como"), base, _("Guardar"), save_as_done, NULL);
}

static void info_open(void)
{
    char *t = of_doc_plain(D.d);
    int words = 0, chars = 0;
    bool in = false;
    for (const char *c = t ? t : ""; *c; c++) {
        if (((unsigned char)*c & 0xC0) != 0x80) chars++;
        bool sp = *c == ' ' || *c == '\n' || *c == '\t';
        if (!sp && !in) words++;
        in = !sp;
    }
    of_free(t);
    char msg[300];
    int ml = snprintf(msg, sizeof msg, _("%d párrafos, %d palabras, %d caracteres."), D.d->np, words, chars);
    if (D.d->readonly && ml > 0 && ml < (int)sizeof msg) {
        snprintf(msg + ml, sizeof msg - (size_t)ml, _(" Leído de %s: se ve y se copia; para editarlo, se guarda una copia como docx u odt."),
                 D.d->kind[0] ? D.d->kind : "?");
    }
    ofu_confirm(of_basename(D.path), D.d->any_locked ? _("Algunos párrafos (marcados al costado) tienen control de cambios u otros objetos: se ven, y se guardan como estaban.") : msg,
                _("Listo"), false, NULL, NULL);
}

/* the headings, to jump to */
static int *s_toc;
static int s_ntoc;

static void toc_pick(int i, void *ud)
{
    (void)ud;
    if (i < 0 || i >= s_ntoc) return;
    int pi = s_toc[i];
    D.ap = D.cp = pi;
    D.apos = D.cpos = 0;
    D.sy = D.pl && pi < D.npl ? D.pl[pi].y - 24 : 0;
    clamp();
    refresh();
}

static void toc_open(void)
{
    of_free(s_toc);
    s_toc = NULL;
    s_ntoc = 0;
    for (int i = 0; i < D.d->np; i++)
        if (D.d->p[i].heading && D.d->p[i].heading <= 3 && !D.d->p[i].cell && D.d->p[i].len) s_ntoc++;
    if (!s_ntoc) { of_app_toast(_("El documento no tiene títulos")); return; }
    if (s_ntoc > 200) s_ntoc = 200;
    s_toc = of_alloc((size_t)s_ntoc * sizeof *s_toc);
    ofu_item_t *items = of_alloc((size_t)s_ntoc * sizeof *items);
    char (*lab)[96] = of_alloc((size_t)s_ntoc * sizeof *lab);
    if (!s_toc || !items || !lab) { of_free(items); of_free(lab); s_ntoc = 0; return; }
    int k = 0;
    for (int i = 0; i < D.d->np && k < s_ntoc; i++) {
        of_para_t *p = &D.d->p[i];
        if (!p->heading || p->heading > 3 || p->cell || !p->len) continue;
        /* indented by level, the objects and line breaks left out */
        int w = 0;
        for (int l = 1; l < p->heading && w < 8; l++) { memcpy(lab[k] + w, "   ", 3); w += 3; }
        for (int c = 0; c < p->len && w < 90; c++) {
            if (c + 2 < p->len && !memcmp(p->txt + c, OF_OBJ_CHAR, 3)) { c += 2; continue; }
            char ch = p->txt[c] == '\n' ? ' ' : p->txt[c];
            /* not a cut UTF-8 sequence at the end */
            if (w >= 86 && ((unsigned char)ch & 0xC0) == 0xC0) break;
            lab[k][w++] = ch;
        }
        while (w && ((unsigned char)lab[k][w - 1] & 0xC0) == 0xC0) w--;
        lab[k][w] = 0;
        items[k] = (ofu_item_t){ lab[k], -1, false, false };
        s_toc[k] = i;
        k++;
    }
    /* the one the caret is in, ticked */
    for (int j = 0; j < k; j++) items[j].checked = s_toc[j] <= D.cp && (j + 1 >= k || s_toc[j + 1] > D.cp);
    ofu_sheet(_("Índice"), items, k, toc_pick, NULL);
    /* the sheet copies what it shows */
    of_free(items);
    of_free(lab);
}

enum { M_SAVE, M_SAVE_AS, M_TOC, M_FIND, M_BIGGER, M_SMALLER, M_ALL, M_COPY, M_PASTE, M_AS_TEXT, M_INFO, M_PAGES };
static int s_menu[12];

static void menu_pick(int i, void *ud)
{
    (void)ud;
    switch (i >= 0 && i < 12 ? s_menu[i] : -1) {
    case M_SAVE: of_docv_save(); break;
    case M_SAVE_AS: {
        ofu_item_t items[4] = {
            { _("Documento de Word (.docx)"), OF_IC_DOC, false, D.d->fmt == OF_DOC_DOCX },
            { _("Documento de OpenDocument (.odt)"), OF_IC_DOC, false, D.d->fmt == OF_DOC_ODT },
            { _("Texto (.txt)"), OF_IC_DOC, false, false },
            { _("PDF para imprimir o mandar (.pdf)"), OF_IC_DOC, false, false },
        };
        /* a PDF read is not written back as PDF */
        ofu_sheet(_("Guardar una copia como"), items, strcmp(D.d->kind, "PDF") ? 4 : 3, save_as_pick, NULL);
        break;
    }
    case M_TOC: toc_open(); break;
    case M_FIND: ofu_prompt(_("Buscar en el documento"), D.find, _("Buscar"), find_done, NULL); break;
    case M_BIGGER: set_zoom(D.z + 1); break;
    case M_SMALLER: set_zoom(D.z - 1); break;
    case M_ALL: D.ap = 0; D.apos = 0; D.cp = D.d->np - 1; D.cpos = D.d->p[D.cp].len; refresh(); break;
    case M_COPY: copy(false); break;
    case M_PASTE: paste(); break;
    case M_AS_TEXT: {
        char p[300];
        snprintf(p, sizeof p, "%s", D.path);
        of_app_open_as(p, OF_OPEN_TEXT);
        break;
    }
    case M_INFO: info_open(); break;
    case M_PAGES: {
        char p[300];
        snprintf(p, sizeof p, "%s", D.path);
        of_app_pdf_mode(p, true);
        break;
    }
    }
}

static void menu_open(void)
{
    ofu_item_t items[12];
    int n = 0;
    bool ro = D.d->readonly;
#define ADD(id, label, icon) do { s_menu[n] = (id); items[n++] = (ofu_item_t){ (label), (icon), false, false }; } while (0)
    if (!strcmp(D.d->kind, "PDF")) ADD(M_PAGES, _("Ver las páginas como se imprimen"), OF_IC_DOC);
    if (!ro) ADD(M_SAVE, _("Guardar"), OF_IC_SAVE);
    ADD(M_SAVE_AS, _("Guardar una copia como..."), OF_IC_COPY);
    ADD(M_TOC, _("Índice"), OF_IC_LIST);
    ADD(M_FIND, _("Buscar..."), OF_IC_SEARCH);
    ADD(M_BIGGER, _("Letra más grande"), OF_IC_ZOOM_IN);
    ADD(M_SMALLER, _("Letra más chica"), OF_IC_ZOOM_OUT);
    ADD(M_ALL, _("Seleccionar todo"), OF_IC_CHECKBOX);
    ADD(M_COPY, _("Copiar"), OF_IC_COPY);
    if (!ro) ADD(M_PASTE, _("Pegar"), OF_IC_COPY);
    if (ro && of_app_can_text(D.path)) ADD(M_AS_TEXT, _("Editar como texto"), OF_IC_TYPE);
    ADD(M_INFO, _("Información"), OF_IC_INFO);
#undef ADD
    ofu_sheet(NULL, items, n, menu_pick, NULL);
}

/* -------------------------------------------------------------------------- */
/* The screen                                                                  */
/* -------------------------------------------------------------------------- */

bool of_docv_save(void)
{
    if (D.d->readonly) return true;
    char err[64] = "";
    if (!of_doc_save(D.d, D.d->fmt, D.path, err, sizeof err)) {
        of_app_toast(_("No se pudo guardar"));
        return false;
    }
    D.modified = false;
    of_app_toast(_("Guardado"));
    refresh();
    return true;
}

bool of_docv_save_recovery(void)
{
    if (!D.d || D.d->readonly) return false;
    char p[340], err[32];
    const char *e = of_ext(D.path);
    int bl = (int)strlen(D.path) - (e[0] ? (int)strlen(e) + 1 : 0);
    snprintf(p, sizeof p, "%.*s (sin guardar).%s", bl > 280 ? 280 : bl, D.path, e[0] ? e : "docx");
    return of_doc_save(D.d, D.d->fmt, p, err, sizeof err);
}

bool of_docv_modified(void)
{
    return D.d && D.modified;
}

static void refresh(void)
{
    if (D.area) lv_obj_invalidate(D.area);
    if (D.b_undo) { if (D.nundo) lv_obj_remove_state(D.b_undo, LV_STATE_DISABLED); else lv_obj_add_state(D.b_undo, LV_STATE_DISABLED); }
    if (D.b_redo) { if (D.nredo) lv_obj_remove_state(D.b_redo, LV_STATE_DISABLED); else lv_obj_add_state(D.b_redo, LV_STATE_DISABLED); }
    if (D.title) {
        char t[160];
        snprintf(t, sizeof t, "%s%s", D.modified ? "\xE2\x80\xA2 " : "", of_basename(D.path));
        lv_label_set_text(D.title, t);
    }
    /* the format bar shows what applies at the caret */
    if (D.fmt && D.d) {
        of_para_t *p = &D.d->p[D.cp];
        of_rstyle_t e;
        uint16_t st = D.typing != 0xFFFF ? D.typing : (p->len ? p->at[D.cpos > 0 ? prev_pos(p, D.cpos) : 0] : p->mark);
        of_doc_effective(D.d, p, st, &e);
        uint8_t on[4] = { e.b, e.i, e.u, e.s };
        for (int i = 0; i < 4; i++) {
            if (!D.fb[i]) continue;
            if (on[i] == OF_TRI_ON) lv_obj_add_state(D.fb[i], LV_STATE_CHECKED);
            else lv_obj_remove_state(D.fb[i], LV_STATE_CHECKED);
        }
    }
}

static void fmt_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    static const uint8_t BIT[4] = { OF_RS_B, OF_RS_I, OF_RS_U, OF_RS_S };
    if (i < 4) toggle(BIT[i]);
    else if (i == 4) style_open();
    else if (i == 5) align_open();
}

static lv_obj_t *fmt_btn(lv_obj_t *row, const char *label, int i, const lv_font_t *f)
{
    lv_obj_t *b = lv_obj_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, i < 4 ? 72 : LV_SIZE_CONTENT, 56);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, OFT.surface2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, OFT.accent, LV_STATE_CHECKED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_t *l = ofu_text(b, label, f, OFT.text);
    lv_obj_center(l);
    if (i == 2) lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_UNDERLINE, 0);
    if (i == 3) lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_STRIKETHROUGH, 0);
    lv_obj_add_event_cb(b, fmt_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    return b;
}

static void kb_show(bool on)
{
    if (on && !D.kb) {
        int32_t kbh = D.W > D.H ? 300 : 420;
        D.kb = ofu_kb_create(D.root, D.W, kbh);
        lv_obj_align(D.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        /* the format bar over it */
        D.fmt = ofu_box(D.root, D.W, 72);
        lv_obj_set_style_bg_color(D.fmt, OFT.surface, 0);
        lv_obj_set_style_bg_opa(D.fmt, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(D.fmt, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(D.fmt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(D.fmt, 8, 0);
        lv_obj_set_style_pad_hor(D.fmt, 10, 0);
        lv_obj_set_scroll_dir(D.fmt, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(D.fmt, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(D.fmt, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        bool f1, f2;
        D.fb[0] = fmt_btn(D.fmt, "B", 0, ofd_font(1, 28, &f1, &f2));
        D.fb[1] = fmt_btn(D.fmt, "I", 1, ofd_font(2, 28, &f1, &f2));
        D.fb[2] = fmt_btn(D.fmt, "U", 2, ofu_font(0, OF_SZ_M));
        D.fb[3] = fmt_btn(D.fmt, "S", 3, ofu_font(0, OF_SZ_M));
        fmt_btn(D.fmt, _("Estilo"), 4, ofu_font(0, OF_SZ_S));
        fmt_btn(D.fmt, _("Alinear"), 5, ofu_font(0, OF_SZ_S));
        lv_obj_align_to(D.fmt, D.kb, LV_ALIGN_OUT_TOP_MID, 0, 0);
        ofu_kb_attach(&TARGET);
    } else if (!on && D.kb) {
        lv_obj_delete(D.kb);
        lv_obj_delete(D.fmt);
        D.kb = D.fmt = NULL;
        memset(D.fb, 0, sizeof D.fb);
    }
    if (D.b_kb) ofu_icon_set(D.b_kb, D.kb ? OF_IC_KB_HIDE : OF_IC_KEYBOARD);
    relayout();
    if (on) scroll_to_caret();
}

static void relayout(void)
{
    D.W = lv_obj_get_width(D.root);
    D.H = lv_obj_get_height(D.root);
    int32_t top = D.W > D.H ? 76 : 88;
    int32_t kbh = D.kb ? lv_obj_get_height(D.kb) + 72 : 0;
    lv_obj_set_size(D.top, D.W, top);
    lv_obj_set_pos(D.area, 0, top);
    D.aw = D.W;
    D.ah = D.H - top - kbh;
    lv_obj_set_size(D.area, D.aw, D.ah);
    lv_obj_set_width(D.title, D.d && D.d->readonly ? D.W - 2 * 84 - 200 : D.W - 5 * 84 - 100);
    /* the page: the screen's width upright, a column lying down */
    D.page_w = D.W > D.H ? (D.W * 70 / 100) : D.W - 16;
    D.page_x = (D.W - D.page_w) / 2;
    layout_all();
    clamp();
    refresh();
}

static void back_cb(lv_event_t *e) { (void)e; of_app_back_from_doc(); }
static void undo_cb(lv_event_t *e) { (void)e; do_undo(); }
static void redo_cb(lv_event_t *e) { (void)e; do_redo(); }
static void more_cb(lv_event_t *e) { (void)e; menu_open(); }
static void kbb_cb(lv_event_t *e) { (void)e; kb_show(!D.kb); }

bool of_docv_open(lv_obj_t *parent, const char *path, struct of_doc_s *d)
{
    if (d) {
        of_docv_close();
        memset(&D, 0, sizeof D);
        D.d = d;
        snprintf(D.path, sizeof D.path, "%s", path);
        int32_t z = 1;
        aos_hal_pref_get_i32("of_dzoom", &z);
        D.z = z >= 0 && z < NZOOM ? (int)z : 1;
        D.typing = 0xFFFF;
        D.want_x = -1;
        if (d->any_locked)
            of_app_toast(_("Algunos párrafos tienen control de cambios u objetos que no se editan acá: se ven y se guardan como están"));
        else if (d->note)
            of_app_toast(d->note);
    } else if (!D.d) {
        return false;
    }
    stop_fling();
    D.root = parent;
    D.kb = D.fmt = NULL;
    memset(D.fb, 0, sizeof D.fb);
    ofu_ui_root = parent;
    lv_obj_set_style_bg_color(parent, OFT.bg, 0);
    D.top = ofu_box(parent, 10, 88);
    lv_obj_set_style_bg_color(D.top, OFT.surface, 0);
    lv_obj_set_style_bg_opa(D.top, LV_OPA_COVER, 0);
    lv_obj_t *bk = ofu_icon_btn(D.top, OF_IC_BACK, 84, back_cb, NULL);
    lv_obj_align(bk, LV_ALIGN_LEFT_MID, 4, 0);
    D.title = ofu_text(D.top, "", ofu_font(OF_ST_BOLD, OF_SZ_M), OFT.text);
    lv_label_set_long_mode(D.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(D.title, ofu_font(OF_ST_BOLD, OF_SZ_M)->line_height);
    lv_obj_align(D.title, LV_ALIGN_LEFT_MID, 92, 0);
    lv_obj_t *mo = ofu_icon_btn(D.top, OF_IC_MORE, 84, more_cb, NULL);
    lv_obj_align(mo, LV_ALIGN_RIGHT_MID, -4, 0);
    if (!D.d->readonly) {
        D.b_kb = ofu_icon_btn(D.top, OF_IC_KEYBOARD, 84, kbb_cb, NULL);
        lv_obj_align(D.b_kb, LV_ALIGN_RIGHT_MID, -88, 0);
        D.b_redo = ofu_icon_btn(D.top, OF_IC_REDO, 84, redo_cb, NULL);
        lv_obj_align(D.b_redo, LV_ALIGN_RIGHT_MID, -172, 0);
        D.b_undo = ofu_icon_btn(D.top, OF_IC_UNDO, 84, undo_cb, NULL);
        lv_obj_align(D.b_undo, LV_ALIGN_RIGHT_MID, -256, 0);
    } else {
        D.b_kb = D.b_redo = D.b_undo = NULL;
        /* what it was read from, beside the menu */
        D.badge = ofu_text(D.top, D.d->kind, ofu_font(OF_ST_BOLD, OF_SZ_XS), OFT.dim);
        lv_obj_align(D.badge, LV_ALIGN_RIGHT_MID, -100, 0);
    }
    D.area = ofu_box(parent, 10, 10);
    lv_obj_add_flag(D.area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(D.area, area_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    aos_gesture_attach(D.area, 0, gesture_cb, NULL);
    if (!D.pic_timer) D.pic_timer = lv_timer_create(pic_timer_cb, 60, NULL);
    invalidate_all();
    ofu_kb_attach(&TARGET);
    relayout();
    return true;
}

bool of_docv_back(void)
{
    if (D.kb) { kb_show(false); return true; }
    if (sel_has()) { D.ap = D.cp; D.apos = D.cpos; refresh(); return true; }
    return false;
}

void of_docv_close(void)
{
    stop_fling();
    if (D.pic_timer) { lv_timer_delete(D.pic_timer); D.pic_timer = NULL; }
    for (int i = 0; i < D.nundo; i++) entry_free(&D.undo[i]);
    for (int i = 0; i < D.nredo; i++) entry_free(&D.redo[i]);
    for (int i = 0; i < D.npl; i++) of_free(D.pl[i].l);
    of_free(D.pl);
    for (int t = 0; t < D.ntl; t++) of_free(D.tl[t].rowy);
    of_free(D.tl);
    pics_free();
    if (D.d) of_doc_free(D.d);
    memset(&D, 0, sizeof D);
    ofd_fonts_free();
}
