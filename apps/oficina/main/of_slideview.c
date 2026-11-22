/*
 * P4OS - Office: the slides screen (pptx and odp), read only.
 *
 * The deck as a column of slides at the screen's width (lying down, as
 * tall as fits), each with its number and, when asked, its speaker notes;
 * a tap on a slide presents from it: the slide alone on black, a tap on the
 * right or a swipe to the left for the next one, on the left for the one
 * before, back to leave. Every slide is drawn by the app from the deck's
 * resolved shapes: backgrounds, fills and outlines (rectangles, rounded,
 * ellipses, triangles, diamonds, arrows), lines, pictures (decoded at their
 * box's size when they come into sight), tables, and text laid out in its
 * box with its sizes, bold, colours, bullets, numbers, alignment and
 * anchor. The menu saves the deck as PDF, or its text as docx, odt or txt.
 */
#include "of_ui.h"
#include "of_doc.h"
#include "of_slides.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    int       s, k;             /* slide, shape (-1: the background) */
    uint16_t *px;
    int       w, h;             /* decoded */
    int       want_w, want_h;
    lv_image_dsc_t dsc;
    bool      failed, wanted;
} spic_t;

static struct {
    of_deck_t *dk;
    char       path[300];
    lv_obj_t  *root, *top, *title, *area, *badge;
    int32_t    W, H, ax, ay, aw, ah;
    int32_t    sy, total_h, sw, shh;
    int32_t   *ypos;            /* each item's top */
    float      vy;
    lv_timer_t *fling, *pic_timer;
    bool       notes, present;
    int        cur;
    spic_t    *pics;
    int        npics;
} V;

static void relayout(void);

/* -------------------------------------------------------------------------- */
/* Drawing helpers                                                             */
/* -------------------------------------------------------------------------- */

static lv_color_t col(uint32_t c) { return lv_color_hex(c & 0xFFFFFF); }

static void rect(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t fill, uint32_t line, int32_t lw, int32_t radius)
{
    if (x2 < x1 || y2 < y1) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = fill ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (fill) d.bg_color = col(fill);
    if (line && lw > 0) { d.border_color = col(line); d.border_width = lw; d.border_opa = LV_OPA_COVER; }
    d.radius = radius;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

static void tri(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, uint32_t c)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.color = col(c);
    d.opa = LV_OPA_COVER;
    d.p[0].x = x1; d.p[0].y = y1;
    d.p[1].x = x2; d.p[1].y = y2;
    d.p[2].x = x3; d.p[2].y = y3;
    lv_draw_triangle(layer, &d);
}

static void line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t c, int32_t w)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = col(c);
    d.width = w > 0 ? w : 1;
    d.p1.x = x1; d.p1.y = y1;
    d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(layer, &d);
}

static void label(lv_layer_t *layer, const char *s, int n, const lv_font_t *f, lv_color_t c, int32_t x, int32_t y, bool faux)
{
    char buf[400];
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = c;
    d.text = buf;
    d.text_local = 1;
    lv_area_t a = { x, y, x + 3000, y + f->line_height };
    lv_draw_label(layer, &d, &a);
    if (faux) { a.x1++; a.x2++; lv_draw_label(layer, &d, &a); }
}

/* -------------------------------------------------------------------------- */
/* Pictures                                                                    */
/* -------------------------------------------------------------------------- */

static spic_t *pic_for(int s, int k, int w, int h)
{
    for (int i = 0; i < V.npics; i++)
        if (V.pics[i].s == s && V.pics[i].k == k) {
            spic_t *p = &V.pics[i];
            /* a new size: decoded again */
            if (p->px && (abs(p->want_w - w) > 6 || abs(p->want_h - h) > 6)) {
                aos_hal_image_free(p->px);
                p->px = NULL;
                p->failed = false;
            }
            p->want_w = w;
            p->want_h = h;
            return p;
        }
    spic_t *n = of_realloc(V.pics, (size_t)(V.npics + 1) * sizeof *n);
    if (!n) return NULL;
    V.pics = n;
    spic_t *p = &V.pics[V.npics++];
    memset(p, 0, sizeof *p);
    p->s = s;
    p->k = k;
    p->want_w = w;
    p->want_h = h;
    return p;
}

static void pic_decode(spic_t *p)
{
    const sl_slide_t *s = &V.dk->s[p->s];
    const char *name = p->k < 0 ? s->bg_pic : s->sh[p->k].pic;
    int ow0 = 0, oh0 = 0;
    if (name && V.dk->pic_files) {
        /* a file of the card (a .ppt's pictures, in the cache) */
        p->px = aos_hal_image_decode(name, 0, 0, p->want_w, p->want_h, true, &ow0, &oh0, NULL, NULL);
        if (!p->px) { p->failed = true; return; }
        p->w = ow0;
        p->h = oh0;
        memset(&p->dsc, 0, sizeof p->dsc);
        p->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        p->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        p->dsc.header.w = (uint32_t)ow0;
        p->dsc.header.h = (uint32_t)oh0;
        p->dsc.header.stride = (uint32_t)ow0 * 2;
        p->dsc.data = (const uint8_t *)p->px;
        p->dsc.data_size = (uint32_t)(ow0 * oh0 * 2);
        return;
    }
    int i = name ? of_zip_find(&V.dk->zip, name) : -1;
    if (i < 0) { p->failed = true; return; }
    const of_zentry_t *e = &V.dk->zip.e[i];
    int ow = 0, oh = 0;
    if (e->method == 0) {
        p->px = aos_hal_image_decode(V.path, e->data, e->csize, p->want_w, p->want_h, true, &ow, &oh, NULL, NULL);
    } else {
        size_t n;
        uint8_t *d = of_zip_read(&V.dk->zip, i, &n);
        if (d) {
            const char *ext = n > 4 && !memcmp(d, "\x89PNG", 4) ? "png" : n > 2 && d[0] == 0xFF && d[1] == 0xD8 ? "jpg" : "bmp";
            char tmp[200];
            snprintf(tmp, sizeof tmp, "%s/oficina/pic.%s", aos_hal_path_data(), ext);
            if (of_file_write(tmp, d, n)) p->px = aos_hal_image_decode(tmp, 0, 0, p->want_w, p->want_h, true, &ow, &oh, NULL, NULL);
            remove(tmp);
            of_free(d);
        }
    }
    if (!p->px) { p->failed = true; return; }
    p->w = ow;
    p->h = oh;
    memset(&p->dsc, 0, sizeof p->dsc);
    p->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    p->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    p->dsc.header.w = (uint32_t)ow;
    p->dsc.header.h = (uint32_t)oh;
    p->dsc.header.stride = (uint32_t)ow * 2;
    p->dsc.data = (const uint8_t *)p->px;
    p->dsc.data_size = (uint32_t)(ow * oh * 2);
}

static void pics_free(void)
{
    for (int i = 0; i < V.npics; i++) if (V.pics[i].px) aos_hal_image_free(V.pics[i].px);
    of_free(V.pics);
    V.pics = NULL;
    V.npics = 0;
}

/* one picture a turn; the far ones let go */
static void pic_timer_cb(lv_timer_t *t)
{
    (void)t;
    int first = V.dk->ns, last = -1;
    if (V.present) first = last = V.cur;
    else
        for (int s = 0; s < V.dk->ns; s++) {
            int32_t y0 = V.ypos[s] - V.sy, y1 = V.ypos[s + 1] - V.sy;
            if (y1 >= 0 && y0 <= V.ah) { if (s < first) first = s; last = s; }
        }
    int held = 0;
    for (int i = 0; i < V.npics; i++) {
        spic_t *p = &V.pics[i];
        if (p->px && (p->s < first - 1 || p->s > last + 1)) { aos_hal_image_free(p->px); p->px = NULL; }
        if (p->px) held++;
    }
    for (int i = 0; i < V.npics; i++) {
        spic_t *p = &V.pics[i];
        if (p->wanted && !p->px && !p->failed) {
            p->wanted = false;
            pic_decode(p);
            if (V.area) lv_obj_invalidate(V.area);
            return;
        }
    }
    (void)held;
}

static void draw_pic(lv_layer_t *layer, int s, int k, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (w < 2 || h < 2) return;
    spic_t *p = pic_for(s, k, w, h);
    if (p && p->px) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = &p->dsc;
        lv_area_t a = { x, y, x + p->w - 1, y + p->h - 1 };
        lv_draw_image(layer, &d, &a);
    } else {
        rect(layer, x, y, x + w - 1, y + h - 1, 0x1D8D8D8, 0, 0, 0);
        if (p && !p->failed) p->wanted = true;
    }
}

/* -------------------------------------------------------------------------- */
/* Text in a box                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    const sl_run_t *r;
    const char *s;
    int n;
    int32_t w;
    const lv_font_t *f;
    bool faux;
    bool space;                 /* a space before it */
} seg_t;

static const lv_font_t *run_font(const sl_run_t *r, float scale, bool *faux)
{
    int px = (int)(r->size * scale * (r->sup ? 0.65f : 1) + 0.5f);
    if (px < 8) px = 8;
    bool fx = false, sl = false;
    const lv_font_t *f = ofd_font((r->b ? 1 : 0) | (r->i ? 2 : 0), px, &fx, &sl);
    if (faux) *faux = fx;
    return f;
}

static int32_t width_of(const lv_font_t *f, const char *s, int n)
{
    int32_t w = 0;
    for (int i = 0; i < n;) {
        int l;
        uint32_t cp = of_utf8_get(s + i, &l);
        if (l < 1) l = 1;
        w += lv_font_get_glyph_width(f, cp, 0);
        i += l;
    }
    return w;
}

static const char *num_label(const sl_para_t *p, char *buf, size_t len)
{
    if (p->bullet[0]) return p->bullet;
    if (!p->autonum) return "";
    int n = p->number > 0 ? p->number : 1;
    if (p->autonum == 2) snprintf(buf, len, "%c.", 'a' + (n - 1) % 26);
    else if (p->autonum == 3) snprintf(buf, len, "%c.", 'A' + (n - 1) % 26);
    else if (p->autonum == 6) snprintf(buf, len, "%d)", n);
    else snprintf(buf, len, "%d.", n);
    return buf;
}

/* Lays out (and draws, when layer) a text body in the box; returns its height in px. */
static int32_t text_box(lv_layer_t *layer, const sl_text_t *t, int32_t bx, int32_t by, int32_t bw, int32_t bh, float scale)
{
    int32_t pad_l = (int32_t)(t->l * scale), pad_r = (int32_t)(t->r * scale), pad_t = (int32_t)(t->t * scale), pad_b = (int32_t)(t->b * scale);
    int32_t inner = bw - pad_l - pad_r;
    if (inner < 8) inner = 8;
    int32_t y = 0;
    int32_t top = by + pad_t;
    if (layer) {
        int32_t total = text_box(NULL, t, bx, by, bw, bh, scale);
        int32_t room = bh - pad_t - pad_b;
        if (t->anchor == 1) top += (room - total) / 2;
        else if (t->anchor == 2) top += room - total;
    }
    seg_t seg[96];
    for (int pi = 0; pi < t->np; pi++) {
        const sl_para_t *p = &t->p[pi];
        if (pi) y += (int32_t)(p->before * scale);
        int32_t marg = (int32_t)(p->margin * scale), ind = (int32_t)(p->indent * scale);
        int32_t width = inner - marg;
        if (width < 8) width = 8;
        bool first = true;
        /* the paragraph's words */
        int ri = 0, off = 0;
        bool space = false;
        bool done = !p->nr;
        if (done) {
            /* an empty paragraph: its own height */
            bool fx;
            sl_run_t r0 = { .size = p->empty_size };
            const lv_font_t *f = run_font(&r0, scale, &fx);
            y += (int32_t)(f->line_height * (p->line > 0.3f ? p->line : 1));
        }
        while (!done) {
            int ns = 0;
            int32_t x = 0, asc = 0, desc = 0;
            bool brk = false;
            while (ri < p->nr && ns < 96) {
                const sl_run_t *r = &p->r[ri];
                const char *s = r->t ? r->t : "";
                int len = (int)strlen(s);
                if (off >= len) { ri++; off = 0; continue; }
                char c = s[off];
                if (c == ' ' || c == '\t') { space = true; off++; continue; }
                if (c == '\n' || c == '\v') { off++; brk = true; space = false; break; }
                int e = off;
                while (e < len && s[e] != ' ' && s[e] != '\t' && s[e] != '\n' && s[e] != '\v') e++;
                bool fx;
                const lv_font_t *f = run_font(r, scale, &fx);
                int32_t w = width_of(f, s + off, e - off);
                int32_t sp = space && ns ? lv_font_get_glyph_width(f, ' ', 0) : 0;
                if (ns && x + sp + w > width) break;
                seg[ns] = (seg_t){ r, s + off, e - off, w, f, fx, space && ns > 0 };
                x += sp + w;
                int32_t a = f->line_height - f->base_line, dd = f->base_line;
                if (a > asc) asc = a;
                if (dd > desc) desc = dd;
                ns++;
                space = false;
                off = e;
            }
            if (ri >= p->nr) done = true;
            if (!ns && !brk && !done) {
                /* a word wider than the box: it goes alone */
                const sl_run_t *r = &p->r[ri];
                const char *s = r->t ? r->t : "";
                int e = off;
                while (s[e] && s[e] != ' ' && s[e] != '\n') e++;
                bool fx;
                const lv_font_t *f = run_font(r, scale, &fx);
                seg[0] = (seg_t){ r, s + off, e - off, width_of(f, s + off, e - off), f, fx, false };
                x = seg[0].w;
                asc = f->line_height - f->base_line;
                desc = f->base_line;
                ns = 1;
                off = e;
            }
            if (!asc) {
                bool fx;
                sl_run_t r0 = { .size = p->nr ? p->r[0].size : p->empty_size };
                const lv_font_t *f = run_font(&r0, scale, &fx);
                asc = f->line_height - f->base_line;
                desc = f->base_line;
            }
            int32_t lh = (int32_t)((asc + desc) * (p->line > 0.3f ? p->line : 1));
            if (layer && ns) {
                int32_t lx = bx + pad_l + marg;
                if (p->align == OF_AL_CENTER) lx += (width - x) / 2;
                else if (p->align == OF_AL_RIGHT) lx += width - x;
                int32_t ly = top + y;
                if (first) {
                    char nb[12];
                    const char *lab = num_label(p, nb, sizeof nb);
                    if (lab[0]) {
                        int32_t bxx = bx + pad_l + marg + ind;
                        if (bxx < bx + pad_l) bxx = bx + pad_l;
                        const lv_font_t *f = seg[0].f;
                        label(layer, lab, (int)strlen(lab), f, col(p->bullet_color ? p->bullet_color : 0x1000000), bxx,
                              ly + asc - (f->line_height - f->base_line), false);
                        /* small slides: the text is not let onto the bullet */
                        int32_t end = bxx + width_of(f, lab, (int)strlen(lab)) + lv_font_get_glyph_width(f, ' ', 0);
                        if (p->align == OF_AL_LEFT && lx < end) lx = end;
                    }
                }
                int32_t cx = lx;
                for (int k = 0; k < ns; k++) {
                    const seg_t *g = &seg[k];
                    if (g->space) cx += lv_font_get_glyph_width(g->f, ' ', 0);
                    int32_t gy = ly + asc - (g->f->line_height - g->f->base_line);
                    if (g->r->sup == 1) gy -= asc / 3;
                    else if (g->r->sup == 2) gy += desc / 2;
                    lv_color_t c = col(g->r->color ? g->r->color : 0x1000000);
                    label(layer, g->s, g->n, g->f, c, cx, gy, g->faux);
                    if (g->r->u) {
                        /* the underline goes under the space too, within a run */
                        int32_t ux = g->space && k && seg[k - 1].r == g->r ? cx - lv_font_get_glyph_width(g->f, ' ', 0) : cx;
                        rect(layer, ux, ly + asc + 1, cx + g->w - 1, ly + asc + 1 + (g->f->line_height > 30 ? 2 : 1), g->r->color ? g->r->color : 0x1000000, 0, 0, 0);
                    }
                    if (g->r->s) rect(layer, cx, ly + asc - asc / 3, cx + g->w - 1, ly + asc - asc / 3 + 1, g->r->color ? g->r->color : 0x1000000, 0, 0, 0);
                    cx += g->w;
                }
            }
            y += lh;
            first = false;
            if (brk && ri >= p->nr) done = true;
        }
        y += (int32_t)(p->after * scale);
    }
    return y + pad_t + pad_b;
}

/* -------------------------------------------------------------------------- */
/* A slide                                                                     */
/* -------------------------------------------------------------------------- */

static void draw_slide(lv_layer_t *layer, int si, int32_t ox, int32_t oy, float scale)
{
    const sl_slide_t *s = &V.dk->s[si];
    int32_t sw = (int32_t)(V.dk->w * scale), sh = (int32_t)(V.dk->h * scale);
    lv_area_t keep = layer->_clip_area, sa = { ox, oy, ox + sw - 1, oy + sh - 1 }, cl;
    if (!ofu_clip(&cl, &keep, &sa)) return;
    layer->_clip_area = cl;
    rect(layer, ox, oy, ox + sw - 1, oy + sh - 1, s->bg ? s->bg : 0x1FFFFFF, 0, 0, 0);
    if (s->bg_pic) draw_pic(layer, si, -1, ox, oy, sw, sh);
    for (int k = 0; k < s->nsh; k++) {
        const sl_shape_t *h = &s->sh[k];
        int32_t x = ox + (int32_t)(h->x * scale), y = oy + (int32_t)(h->y * scale);
        int32_t w = (int32_t)(h->w * scale), hh = (int32_t)(h->h * scale);
        int32_t lw = h->line ? (int32_t)(h->line_w * scale + 0.5f) : 0;
        if (h->line && lw < 1) lw = 1;
        if (h->kind == SL_LINE) {
            int32_t x1 = x, y1 = y, x2 = x + w, y2 = y + hh;
            if (h->flip_h) { int32_t t = x1; x1 = x2; x2 = t; }
            if (h->flip_v) { int32_t t = y1; y1 = y2; y2 = t; }
            line(layer, x1, y1, x2, y2, h->line ? h->line : 0x1000000, lw);
            continue;
        }
        if (h->kind == SL_PIC && h->pic) { draw_pic(layer, si, k, x, y, w, hh); continue; }
        if (h->kind == SL_TABLE) {
            float tw = 0;
            for (int c = 0; c < h->cols; c++) tw += h->colw ? h->colw[c] : 0;
            float cs = tw > 0 ? h->w / tw : 1;
            int32_t ry = y;
            for (int r = 0; r < h->rows; r++) {
                int32_t rh = (int32_t)((h->rowh ? h->rowh[r] : 20) * scale);
                /* a row grows with its text */
                for (int c = 0; c < h->ncells; c++) {
                    const sl_cell_t *cl2 = &h->cells[c];
                    if (cl2->row != r || cl2->hidden) continue;
                    float cw = 0;
                    for (int q = 0; q < cl2->cspan && cl2->col + q < h->cols; q++) cw += h->colw[cl2->col + q] * cs;
                    int32_t th = text_box(NULL, &cl2->tx, 0, 0, (int32_t)(cw * scale), 0, scale);
                    if (th > rh) rh = th;
                }
                for (int c = 0; c < h->ncells; c++) {
                    const sl_cell_t *cl2 = &h->cells[c];
                    if (cl2->row != r || cl2->hidden) continue;
                    float cx = 0, cw = 0;
                    for (int q = 0; q < cl2->col && q < h->cols; q++) cx += h->colw[q] * cs;
                    for (int q = 0; q < cl2->cspan && cl2->col + q < h->cols; q++) cw += h->colw[cl2->col + q] * cs;
                    int32_t x1 = x + (int32_t)(cx * scale), x2 = x + (int32_t)((cx + cw) * scale) - 1;
                    rect(layer, x1, ry, x2, ry + rh - 1, cl2->fill, 0x1A0A0A0, 1, 0);
                    text_box(layer, &cl2->tx, x1, ry, x2 - x1 + 1, rh, scale);
                }
                ry += rh;
            }
            continue;
        }
        /* a shape */
        if (h->fill || h->line) {
            switch (h->geom) {
            case SG_ELLIPSE: rect(layer, x, y, x + w - 1, y + hh - 1, h->fill, h->line, lw, LV_RADIUS_CIRCLE); break;
            case SG_ROUND: rect(layer, x, y, x + w - 1, y + hh - 1, h->fill, h->line, lw, (w < hh ? w : hh) / 6); break;
            case SG_TRIANGLE: tri(layer, x, y + hh, x + w, y + hh, x + w / 2, y, h->fill ? h->fill : h->line); break;
            case SG_DIAMOND:
                tri(layer, x, y + hh / 2, x + w / 2, y, x + w, y + hh / 2, h->fill ? h->fill : h->line);
                tri(layer, x, y + hh / 2, x + w / 2, y + hh, x + w, y + hh / 2, h->fill ? h->fill : h->line);
                break;
            case SG_ARROW: {
                int32_t hd = w * 3 / 10;
                rect(layer, x, y + hh / 4, x + w - hd, y + hh * 3 / 4, h->fill ? h->fill : h->line, 0, 0, 0);
                tri(layer, x + w - hd, y, x + w, y + hh / 2, x + w - hd, y + hh, h->fill ? h->fill : h->line);
                break;
            }
            default: rect(layer, x, y, x + w - 1, y + hh - 1, h->fill, h->line, lw, 0); break;
            }
        }
        if (h->tx.np) text_box(layer, &h->tx, x, y, w, hh, scale);
    }
    if (s->hidden && !V.present) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(0x000000);
        d.bg_opa = LV_OPA_40;
        lv_draw_rect(layer, &d, &sa);
    }
    layer->_clip_area = keep;
}

/* -------------------------------------------------------------------------- */
/* The column                                                                  */
/* -------------------------------------------------------------------------- */

static int32_t notes_h(int si)
{
    const char *n = V.dk->s[si].notes;
    if (!V.notes || !n || !n[0]) return 0;
    lv_point_t sz;
    lv_text_get_size(&sz, n, ofu_font(0, OF_SZ_S), 0, 4, V.sw - 24, LV_TEXT_FLAG_NONE);
    return sz.y + 28;
}

static void layout(void)
{
    of_free(V.ypos);
    V.ypos = of_alloc((size_t)(V.dk->ns + 1) * sizeof(int32_t));
    if (!V.ypos) return;
    bool land = V.W > V.H;
    V.sw = V.aw - 32;
    if (land) {
        int32_t by_h = (int32_t)((V.ah - 70) * V.dk->w / V.dk->h);
        if (by_h < V.sw) V.sw = by_h;
    }
    V.shh = (int32_t)(V.sw * V.dk->h / V.dk->w);
    int32_t y = 12;
    for (int i = 0; i < V.dk->ns; i++) {
        V.ypos[i] = y;
        y += 40 + V.shh + notes_h(i) + 20;
    }
    V.ypos[V.dk->ns] = y;
    V.total_h = y + 20;
}

static void clamp(void)
{
    int32_t max = V.total_h - V.ah;
    if (V.sy > max) V.sy = max;
    if (V.sy < 0) V.sy = 0;
}

static void area_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(V.area, &a);
    V.ax = a.x1;
    V.ay = a.y1;
    lv_area_t keep = layer->_clip_area, cl;
    if (!ofu_clip(&cl, &keep, &a)) return;
    layer->_clip_area = cl;
    if (V.present) {
        rect(layer, cl.x1, cl.y1, cl.x2, cl.y2, 0x1000000, 0, 0, 0);
        float sc = (float)V.aw / V.dk->w;
        if (V.dk->h * sc > V.ah) sc = (float)V.ah / V.dk->h;
        int32_t sw = (int32_t)(V.dk->w * sc), sh = (int32_t)(V.dk->h * sc);
        draw_slide(layer, V.cur, V.ax + (V.aw - sw) / 2, V.ay + (V.ah - sh) / 2, sc);
        char n[24];
        snprintf(n, sizeof n, "%d / %d", V.cur + 1, V.dk->ns);
        label(layer, n, (int)strlen(n), ofu_font(0, OF_SZ_XS), lv_color_hex(0x707070), V.ax + V.aw - 110, V.ay + V.ah - 30, false);
        layer->_clip_area = keep;
        return;
    }
    {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = OFT.bg;
        lv_draw_rect(layer, &d, &cl);
    }
    float sc = (float)V.sw / V.dk->w;
    int32_t x0 = V.ax + (V.aw - V.sw) / 2;
    for (int i = 0; i < V.dk->ns; i++) {
        int32_t y = V.ay + V.ypos[i] - V.sy;
        if (y > V.ay + V.ah || V.ay + V.ypos[i + 1] - V.sy < V.ay) continue;
        char n[64];
        snprintf(n, sizeof n, "%d%s", i + 1, V.dk->s[i].hidden ? _("  ·  oculta") : "");
        label(layer, n, (int)strlen(n), ofu_font(OF_ST_BOLD, OF_SZ_S), OFT.dim, x0, y + 6, false);
        draw_slide(layer, i, x0, y + 40, sc);
        /* a thin frame, for white slides on a white page */
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = OFT.dim;
        d.border_opa = LV_OPA_40;
        d.border_width = 1;
        lv_area_t fa = { x0 - 1, y + 39, x0 + V.sw, y + 40 + V.shh };
        lv_draw_rect(layer, &d, &fa);
        int32_t nh = notes_h(i);
        if (nh) {
            lv_draw_rect_dsc_t nd;
            lv_draw_rect_dsc_init(&nd);
            nd.bg_color = OFT.surface;
            nd.radius = 10;
            lv_area_t na = { x0, y + 40 + V.shh + 8, x0 + V.sw - 1, y + 40 + V.shh + nh - 4 };
            lv_draw_rect(layer, &nd, &na);
            lv_draw_label_dsc_t ld;
            lv_draw_label_dsc_init(&ld);
            ld.font = ofu_font(0, OF_SZ_S);
            ld.color = OFT.text;
            ld.text = V.dk->s[i].notes;
            ld.line_space = 4;
            lv_area_t la = { x0 + 12, y + 40 + V.shh + 18, x0 + V.sw - 12, y + 40 + V.shh + nh };
            lv_draw_label(layer, &ld, &la);
        }
    }
    layer->_clip_area = keep;
}

/* -------------------------------------------------------------------------- */
/* Gestures                                                                    */
/* -------------------------------------------------------------------------- */

static void fling_cb(lv_timer_t *t)
{
    (void)t;
    V.sy -= (int32_t)(V.vy / 60);
    V.vy *= 0.93f;
    int32_t o = V.sy;
    clamp();
    if (o != V.sy) V.vy = 0;
    lv_obj_invalidate(V.area);
    if (V.vy * V.vy < 400) { lv_timer_delete(V.fling); V.fling = NULL; }
}

static void stop_fling(void)
{
    if (V.fling) { lv_timer_delete(V.fling); V.fling = NULL; }
}

static int next_shown(int from, int dir)
{
    for (int i = from + dir; i >= 0 && i < V.dk->ns; i += dir)
        if (!V.dk->s[i].hidden) return i;
    return from;
}

static void present(bool on, int from)
{
    V.present = on;
    if (on) V.cur = from;
    else if (V.ypos) { V.sy = V.ypos[V.cur]; }
    relayout();
}

static void step(int dir)
{
    int n = next_shown(V.cur, dir);
    if (n == V.cur) { of_app_toast(dir > 0 ? _("Es la última") : _("Es la primera")); return; }
    V.cur = n;
    lv_obj_invalidate(V.area);
}

static void gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    (void)user;
    if (ofu_sheet_open()) return;
    if (V.present) {
        if (ev->type == AOS_GESTURE_TAP) step(ev->x - V.ax < V.aw / 3 ? -1 : 1);
        else if (ev->type == AOS_GESTURE_DRAG_END && (ev->vx * ev->vx > 90000)) step(ev->vx < 0 ? 1 : -1);
        return;
    }
    switch (ev->type) {
    case AOS_GESTURE_TAP: {
        stop_fling();
        int32_t py = (int32_t)ev->y - V.ay + V.sy;
        for (int i = 0; i < V.dk->ns; i++)
            if (py >= V.ypos[i] + 40 && py < V.ypos[i] + 40 + V.shh) { present(true, i); break; }
        break;
    }
    case AOS_GESTURE_DRAG_BEGIN: stop_fling(); break;
    case AOS_GESTURE_DRAG:
        V.sy -= (int32_t)ev->dy;
        clamp();
        lv_obj_invalidate(V.area);
        break;
    case AOS_GESTURE_DRAG_END:
        if (ev->vy * ev->vy > 90000) {
            V.vy = ev->vy;
            if (!V.fling) V.fling = lv_timer_create(fling_cb, 16, NULL);
        }
        break;
    default: break;
    }
}

/* -------------------------------------------------------------------------- */
/* Menus                                                                       */
/* -------------------------------------------------------------------------- */

static int s_as;

static void save_as_done(const char *text, void *ud)
{
    (void)ud;
    static const char *const EXT[] = { "pdf", "docx", "odt", "txt" };
    if (!text[0] || strchr(text, '/')) return;
    char dir[300];
    snprintf(dir, sizeof dir, "%s", V.path);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0;
    char p[320];
    bool has = !strcasecmp(of_ext(text), EXT[s_as]);
    snprintf(p, sizeof p, "%.200s/%.100s%s%s", dir, text, has ? "" : ".", has ? "" : EXT[s_as]);
    if (of_file_exists(p)) { of_app_toast(_("Ya hay un archivo con ese nombre")); return; }
    char err[64];
    bool ok;
    if (s_as == 0) {
        char title[128];
        snprintf(title, sizeof title, "%s", of_basename(V.path));
        char *dot = strrchr(title, '.');
        if (dot && dot != title) *dot = 0;
        ok = of_pdf_write_deck(V.dk, p, title, false, err, sizeof err);
    } else {
        of_doc_t *d = of_deck_to_doc(V.dk, true);
        ok = d && of_doc_save(d, s_as == 1 ? OF_DOC_DOCX : s_as == 2 ? OF_DOC_ODT : OF_DOC_TEXT, p, err, sizeof err);
        of_doc_free(d);
    }
    if (!ok) { of_app_toast(_("No se pudo guardar")); return; }
    of_recent_add(p);
    of_app_toast(s_as == 0 ? _("Guardado como PDF") : _("Guardado: el texto de las diapositivas y sus notas"));
}

static void save_as_pick(int i, void *ud)
{
    (void)ud;
    s_as = i;
    char base[128];
    snprintf(base, sizeof base, "%s", of_basename(V.path));
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = 0;
    ofu_prompt(_("Guardar una copia como"), base, _("Guardar"), save_as_done, NULL);
}

static void goto_done(const char *text, void *ud)
{
    (void)ud;
    int n = atoi(text);
    if (n < 1 || n > V.dk->ns) { of_app_toast(_("No hay una diapositiva con ese número")); return; }
    V.sy = V.ypos[n - 1];
    clamp();
    lv_obj_invalidate(V.area);
}

static void info_open(void)
{
    int hidden = 0, notes = 0;
    for (int i = 0; i < V.dk->ns; i++) { if (V.dk->s[i].hidden) hidden++; if (V.dk->s[i].notes) notes++; }
    char msg[240];
    snprintf(msg, sizeof msg, _("%d diapositivas (%d ocultas, %d con notas), de %.1f × %.1f cm. Se ven y se presentan; para editarlas hace falta PowerPoint o Impress."),
             V.dk->ns, hidden, notes, V.dk->w / 28.3465, V.dk->h / 28.3465);
    ofu_confirm(of_basename(V.path), msg, _("Listo"), false, NULL, NULL);
}

static void menu_pick(int i, void *ud)
{
    (void)ud;
    switch (i) {
    case 0: {
        /* from the slide at the top of the screen */
        int from = 0;
        for (int k = 0; k < V.dk->ns; k++) if (V.ypos[k] + V.shh / 2 <= V.sy + 40) from = k + 1 < V.dk->ns ? k + 1 : k;
        if (V.dk->s[from].hidden) from = next_shown(from, 1);
        present(true, from);
        break;
    }
    case 1:
        V.notes = !V.notes;
        aos_hal_pref_set_i32("of_snotes", V.notes);
        layout();
        clamp();
        lv_obj_invalidate(V.area);
        break;
    case 2: ofu_prompt(_("Ir a la diapositiva"), "", _("Ir"), goto_done, NULL); break;
    case 3: {
        ofu_item_t items[4] = {
            { _("PDF, una página por diapositiva (.pdf)"), OF_IC_DOC, false, false },
            { _("El texto, como documento de Word (.docx)"), OF_IC_DOC, false, false },
            { _("El texto, como OpenDocument (.odt)"), OF_IC_DOC, false, false },
            { _("El texto (.txt)"), OF_IC_DOC, false, false },
        };
        ofu_sheet(_("Guardar una copia como"), items, 4, save_as_pick, NULL);
        break;
    }
    case 4: info_open(); break;
    }
}

static void menu_open(void)
{
    ofu_item_t items[5] = {
        { _("Presentar"), OF_IC_CHEVRON, false, false },
        { _("Notas del orador"), OF_IC_NOTE, false, V.notes },
        { _("Ir a la diapositiva..."), OF_IC_LIST, false, false },
        { _("Guardar una copia como..."), OF_IC_COPY, false, false },
        { _("Información"), OF_IC_INFO, false, false },
    };
    ofu_sheet(NULL, items, 5, menu_pick, NULL);
}

/* -------------------------------------------------------------------------- */
/* The screen                                                                  */
/* -------------------------------------------------------------------------- */

static void relayout(void)
{
    V.W = lv_obj_get_width(V.root);
    V.H = lv_obj_get_height(V.root);
    int32_t top = V.present ? 0 : V.W > V.H ? 76 : 88;
    if (V.present) lv_obj_add_flag(V.top, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(V.top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(V.top, V.W, V.W > V.H ? 76 : 88);
    lv_obj_set_pos(V.area, 0, top);
    V.aw = V.W;
    V.ah = V.H - top;
    lv_obj_set_size(V.area, V.aw, V.ah);
    lv_obj_set_width(V.title, V.W - 2 * 84 - 200);
    layout();
    clamp();
    lv_obj_invalidate(V.area);
}

static void back_cb(lv_event_t *e) { (void)e; of_app_home(); }
static void more_cb(lv_event_t *e) { (void)e; menu_open(); }

bool of_slidev_open(lv_obj_t *parent, const char *path, of_deck_t *dk)
{
    if (dk) {
        of_slidev_close();
        memset(&V, 0, sizeof V);
        V.dk = dk;
        snprintf(V.path, sizeof V.path, "%s", path);
        int32_t n = 0;
        aos_hal_pref_get_i32("of_snotes", &n);
        V.notes = n != 0;
    } else if (!V.dk) {
        return false;
    }
    stop_fling();
    V.root = parent;
    ofu_ui_root = parent;
    lv_obj_set_style_bg_color(parent, OFT.bg, 0);
    V.top = ofu_box(parent, 10, 88);
    lv_obj_set_style_bg_color(V.top, OFT.surface, 0);
    lv_obj_set_style_bg_opa(V.top, LV_OPA_COVER, 0);
    lv_obj_t *bk = ofu_icon_btn(V.top, OF_IC_BACK, 84, back_cb, NULL);
    lv_obj_align(bk, LV_ALIGN_LEFT_MID, 4, 0);
    V.title = ofu_text(V.top, of_basename(V.path), ofu_font(OF_ST_BOLD, OF_SZ_M), OFT.text);
    lv_label_set_long_mode(V.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(V.title, ofu_font(OF_ST_BOLD, OF_SZ_M)->line_height);
    lv_obj_align(V.title, LV_ALIGN_LEFT_MID, 92, 0);
    lv_obj_t *mo = ofu_icon_btn(V.top, OF_IC_MORE, 84, more_cb, NULL);
    lv_obj_align(mo, LV_ALIGN_RIGHT_MID, -4, 0);
    V.badge = ofu_text(V.top, V.dk->kind, ofu_font(OF_ST_BOLD, OF_SZ_XS), OFT.dim);
    lv_obj_align(V.badge, LV_ALIGN_RIGHT_MID, -100, 0);
    V.area = ofu_box(parent, 10, 10);
    lv_obj_add_flag(V.area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(V.area, area_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    aos_gesture_attach(V.area, 0, gesture_cb, NULL);
    if (!V.pic_timer) V.pic_timer = lv_timer_create(pic_timer_cb, 60, NULL);
    relayout();
    return true;
}

bool of_slidev_back(void)
{
    if (V.present) { present(false, 0); return true; }
    return false;
}

void of_slidev_close(void)
{
    stop_fling();
    if (V.pic_timer) { lv_timer_delete(V.pic_timer); V.pic_timer = NULL; }
    pics_free();
    of_free(V.ypos);
    if (V.dk) of_deck_free(V.dk);
    memset(&V, 0, sizeof V);
    ofd_fonts_free();
}
