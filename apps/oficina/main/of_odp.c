/*
 * P4OS - Office: Impress (.odp, .otp), read.
 *
 * content.xml's draw:page elements in order; each frame, shape, line,
 * picture and table at its svg:x/y/width/height. Text styles come from the
 * presentation style of the frame (the master's "title", "outline1".."9",
 * "subtitle", "notes"), the paragraph's and the span's styles, each with its
 * parents; graphic styles give fills and strokes; the drawing-page style
 * (or the master page's) the background. The master page's own shapes are
 * drawn under the slide's (its placeholders are only frames). Speaker notes
 * from presentation:notes.
 */
#include "of_slides.h"
#include "of_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern const char *(*of_tr)(const char *es);
#define _(s) (of_tr ? of_tr(s) : (s))

/* lengths: "2.5cm" "12mm" "1in" "18pt" "100px" to points */
static float len_pt(const char *v)
{
    char *e;
    float x = strtof(v, &e);
    while (*e == ' ') e++;
    if (!strncmp(e, "cm", 2)) return x * 28.3465f;
    if (!strncmp(e, "mm", 2)) return x * 2.83465f;
    if (!strncmp(e, "in", 2)) return x * 72;
    if (!strncmp(e, "px", 2)) return x * 0.75f;
    if (!strncmp(e, "pc", 2)) return x * 12;
    return x;
}

static float alen(const xn_t *n, const char *a, float def)
{
    char v[40];
    return xn_attr(n, a, v, sizeof v) && v[0] ? len_pt(v) : def;
}

static uint32_t hexcol(const char *v)
{
    if (v[0] != '#') return 0;
    return (uint32_t)strtoul(v + 1, NULL, 16) | 0x1000000u;
}

/* -------------------------------------------------------------------------- */
/* Styles                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct { const xn_t *n; char name[80], family[24]; } st_t;

typedef struct {
    of_deck_t *dk;
    xn_t      *content, *styles;
    char      *csrc, *ssrc;
    st_t      *st;
    int        nst;
    const xn_t **lists;         /* text:list-style */
    int        nlists;
    char       master_name[80];
} odp_t;

static void collect(odp_t *O, const xn_t *n)
{
    for (const xn_t *c = n ? n->kid : NULL; c; c = c->next) {
        if (!strcmp(c->name, "style:style") || !strcmp(c->name, "style:presentation-page-layout")) {
            st_t *s = of_realloc(O->st, (size_t)(O->nst + 1) * sizeof *s);
            if (!s) return;
            O->st = s;
            st_t *x = &O->st[O->nst++];
            x->n = c;
            xn_attr(c, "style:name", x->name, sizeof x->name);
            xn_attr(c, "style:family", x->family, sizeof x->family);
        } else if (!strcmp(c->name, "text:list-style")) {
            const xn_t **l = of_realloc(O->lists, (size_t)(O->nlists + 1) * sizeof *l);
            if (!l) return;
            O->lists = l;
            O->lists[O->nlists++] = c;
        }
    }
}

static const xn_t *style(const odp_t *O, const char *name, const char *family)
{
    if (!name || !name[0]) return NULL;
    /* automatic ones of content.xml first (collected first) */
    for (int i = 0; i < O->nst; i++)
        if (!strcmp(O->st[i].name, name) && (!family || !strcmp(O->st[i].family, family))) return O->st[i].n;
    return NULL;
}

typedef struct {
    float    size;              /* 0 unset */
    float    pct;               /* a size relative to the parent's */
    int8_t   b, i, u, s, sup;
    uint32_t color;
    int8_t   algn;
    float    marL, indent, before, after;
    bool     has_marL;
    /* graphic */
    int8_t   fill_none, stroke_none;
    uint32_t fill, stroke;
    float    stroke_w;
    int8_t   valign;
    float    pl, pt, pr, pb;
    bool     has_pad;
} props_t;

static void props_clear(props_t *p)
{
    memset(p, 0, sizeof *p);
    p->b = p->i = p->u = p->s = p->sup = p->algn = p->fill_none = p->stroke_none = p->valign = -1;
}

static void read_props(const xn_t *st, props_t *p)
{
    char v[64];
    for (const xn_t *c = st ? st->kid : NULL; c; c = c->next) {
        if (!strcmp(c->name, "style:text-properties")) {
            if (xn_attr(c, "fo:font-size", v, sizeof v)) {
                if (strchr(v, '%')) p->pct = strtof(v, NULL) / 100;
                else { p->size = len_pt(v); p->pct = 0; }
            }
            if (xn_attr(c, "fo:font-weight", v, sizeof v)) p->b = (int8_t)(!strcmp(v, "bold") || atoi(v) >= 600);
            if (xn_attr(c, "fo:font-style", v, sizeof v)) p->i = (int8_t)(!strcmp(v, "italic") || !strcmp(v, "oblique"));
            if (xn_attr(c, "style:text-underline-style", v, sizeof v)) p->u = (int8_t)(strcmp(v, "none") != 0);
            if (xn_attr(c, "style:text-line-through-style", v, sizeof v)) p->s = (int8_t)(strcmp(v, "none") != 0);
            if (xn_attr(c, "style:text-position", v, sizeof v)) p->sup = (int8_t)(!strncmp(v, "super", 5) || atoi(v) > 0 ? 1 : !strncmp(v, "sub", 3) || atoi(v) < 0 ? 2 : 0);
            if (xn_attr(c, "fo:color", v, sizeof v)) p->color = hexcol(v);
        } else if (!strcmp(c->name, "style:paragraph-properties")) {
            if (xn_attr(c, "fo:text-align", v, sizeof v))
                p->algn = (int8_t)(!strcmp(v, "center") ? OF_AL_CENTER : !strcmp(v, "end") || !strcmp(v, "right") ? OF_AL_RIGHT : !strcmp(v, "justify") ? OF_AL_JUSTIFY : OF_AL_LEFT);
            if (xn_attr(c, "fo:margin-left", v, sizeof v)) { p->marL = len_pt(v); p->has_marL = true; }
            if (xn_attr(c, "fo:text-indent", v, sizeof v)) p->indent = len_pt(v);
            if (xn_attr(c, "fo:margin-top", v, sizeof v)) p->before = len_pt(v);
            if (xn_attr(c, "fo:margin-bottom", v, sizeof v)) p->after = len_pt(v);
        } else if (!strcmp(c->name, "style:graphic-properties") || !strcmp(c->name, "style:drawing-page-properties")) {
            if (xn_attr(c, "draw:fill", v, sizeof v)) p->fill_none = (int8_t)(!strcmp(v, "none"));
            if (xn_attr(c, "draw:fill-color", v, sizeof v)) { p->fill = hexcol(v); if (p->fill_none < 0) p->fill_none = 0; }
            if (xn_attr(c, "draw:stroke", v, sizeof v)) p->stroke_none = (int8_t)(!strcmp(v, "none"));
            if (xn_attr(c, "svg:stroke-color", v, sizeof v)) p->stroke = hexcol(v);
            if (xn_attr(c, "svg:stroke-width", v, sizeof v)) p->stroke_w = len_pt(v);
            if (xn_attr(c, "draw:textarea-vertical-align", v, sizeof v)) p->valign = (int8_t)(!strcmp(v, "middle") ? 1 : !strcmp(v, "bottom") ? 2 : 0);
            if (xn_attr(c, "fo:padding-left", v, sizeof v)) { p->pl = len_pt(v); p->has_pad = true; }
            if (xn_attr(c, "fo:padding-top", v, sizeof v)) { p->pt = len_pt(v); p->has_pad = true; }
            if (xn_attr(c, "fo:padding-right", v, sizeof v)) { p->pr = len_pt(v); p->has_pad = true; }
            if (xn_attr(c, "fo:padding-bottom", v, sizeof v)) { p->pb = len_pt(v); p->has_pad = true; }
        } else if (!strcmp(c->name, "style:table-cell-properties")) {
            if (xn_attr(c, "fo:background-color", v, sizeof v) && v[0] == '#') { p->fill = hexcol(v); p->fill_none = 0; }
        }
    }
}

/* a style with its parents applied first; over what p already has */
static void apply(const odp_t *O, const char *name, const char *family, props_t *p, int depth)
{
    const xn_t *s = style(O, name, family);
    if (!s || depth > 8) return;
    char parent[80];
    if (xn_attr(s, "style:parent-style-name", parent, sizeof parent)) apply(O, parent, family, p, depth + 1);
    props_t own;
    props_clear(&own);
    read_props(s, &own);
    if (own.pct) { float base = p->size ? p->size : 18; p->size = base * own.pct; }
    else if (own.size) p->size = own.size;
    if (own.b >= 0) p->b = own.b;
    if (own.i >= 0) p->i = own.i;
    if (own.u >= 0) p->u = own.u;
    if (own.s >= 0) p->s = own.s;
    if (own.sup >= 0) p->sup = own.sup;
    if (own.color) p->color = own.color;
    if (own.algn >= 0) p->algn = own.algn;
    if (own.has_marL) { p->marL = own.marL; p->has_marL = true; }
    if (own.indent) p->indent = own.indent;
    if (own.before) p->before = own.before;
    if (own.after) p->after = own.after;
    if (own.fill_none >= 0) p->fill_none = own.fill_none;
    if (own.fill) p->fill = own.fill;
    if (own.stroke_none >= 0) p->stroke_none = own.stroke_none;
    if (own.stroke) p->stroke = own.stroke;
    if (own.stroke_w) p->stroke_w = own.stroke_w;
    if (own.valign >= 0) p->valign = own.valign;
    if (own.has_pad) { p->pl = own.pl; p->pt = own.pt; p->pr = own.pr; p->pb = own.pb; p->has_pad = true; }
}

/* -------------------------------------------------------------------------- */
/* Text                                                                        */
/* -------------------------------------------------------------------------- */

typedef struct {
    odp_t      *O;
    sl_text_t  *tx;
    props_t     base;           /* the frame's (presentation style) */
    char        cls[24];        /* presentation:class */
    const xn_t *list_style;     /* the list's text:list-style */
    int         level;
    bool        in_list;
} tctx_t;

static void runs(tctx_t *T, sl_para_t *p, const xn_t *n, const props_t *pp, const char *link, int depth)
{
    if (depth > 12) return;
    for (const xn_t *c = n->kid; c; c = c->next) {
        if (c->text) {
            /* white space in ODF text collapses */
            of_sb_t t = { 0 };
            bool sp = p->nr && p->r[p->nr - 1].t && p->r[p->nr - 1].t[0] && p->r[p->nr - 1].t[strlen(p->r[p->nr - 1].t) - 1] == ' ';
            for (const char *s = c->text; *s; s++) {
                bool w = *s == ' ' || *s == '\n' || *s == '\t' || *s == '\r';
                if (w) { if (!sp && (t.n || p->nr)) of_sb_putc(&t, ' '); sp = true; }
                else { of_sb_putc(&t, *s); sp = false; }
            }
            if (!t.n) { of_sb_free(&t); continue; }
            sl_run_t *r = sl_add_run(p, t.s, t.n);
            of_sb_free(&t);
            if (!r) return;
            r->size = pp->size ? pp->size : 18;
            r->b = pp->b > 0; r->i = pp->i > 0; r->u = pp->u > 0 || link; r->s = pp->s > 0;
            r->sup = (uint8_t)(pp->sup > 0 ? pp->sup : 0);
            r->color = pp->color ? pp->color : link ? 0x10563C1 : 0x1000000;
            if (link) r->link = of_strdup(link);
            continue;
        }
        if (!strcmp(c->name, "text:span")) {
            props_t sp = *pp;
            char sn[80];
            if (xn_attr(c, "text:style-name", sn, sizeof sn)) apply(T->O, sn, "text", &sp, 0);
            runs(T, p, c, &sp, link, depth + 1);
        } else if (!strcmp(c->name, "text:a")) {
            char href[300] = "";
            xn_attr(c, "xlink:href", href, sizeof href);
            runs(T, p, c, pp, href[0] ? href : link, depth + 1);
        } else if (!strcmp(c->name, "text:s")) {
            int k = (int)xn_num(c, "text:c", 1);
            char sp[33];
            if (k > 32) k = 32;
            memset(sp, ' ', (size_t)k);
            sl_run_t *r = sl_add_run(p, sp, (size_t)k);
            if (r) { r->size = pp->size ? pp->size : 18; r->color = pp->color ? pp->color : 0x1000000; }
        } else if (!strcmp(c->name, "text:tab")) {
            sl_run_t *r = sl_add_run(p, "    ", 4);
            if (r) { r->size = pp->size ? pp->size : 18; r->color = 0x1000000; }
        } else if (!strcmp(c->name, "text:line-break")) {
            sl_run_t *r = sl_add_run(p, "\n", 1);
            if (r) { r->size = pp->size ? pp->size : 18; r->color = 0x1000000; }
        } else if (!strcmp(c->name, "text:page-number")) {
            runs(T, p, c, pp, link, depth + 1);
        } else if (strcmp(c->name, "text:note") && strcmp(c->name, "office:annotation")) {
            runs(T, p, c, pp, link, depth + 1);
        }
    }
}

static const xn_t *list_level(const xn_t *ls, int level)
{
    for (const xn_t *c = ls ? ls->kid : NULL; c; c = c->next)
        if ((int)xn_num(c, "text:level", 1) == level + 1) return c;
    return NULL;
}

static void paragraph(tctx_t *T, const xn_t *pn)
{
    sl_para_t *p = sl_add_para(T->tx);
    if (!p) return;
    props_t pp = T->base;
    /* an outline's level has its own style: "Default-outline2" */
    if (!strcmp(T->cls, "outline") && T->level > 0 && T->O->master_name[0]) {
        char nm[100];
        snprintf(nm, sizeof nm, "%.80s-outline%d", T->O->master_name, T->level + 1);
        apply(T->O, nm, "presentation", &pp, 0);
    }
    char sn[80];
    if (xn_attr(pn, "text:style-name", sn, sizeof sn)) apply(T->O, sn, "paragraph", &pp, 0);
    p->level = (uint8_t)(T->level < 9 ? T->level : 8);
    p->align = (uint8_t)(pp.algn >= 0 ? pp.algn : OF_AL_LEFT);
    p->before = pp.before;
    p->after = pp.after;
    p->margin = pp.has_marL ? pp.marL : T->in_list ? 18.0f * (T->level + 1) : 0;
    p->indent = pp.indent ? pp.indent : T->in_list ? -14 : 0;
    p->empty_size = pp.size ? pp.size : 18;
    if (T->in_list) {
        const xn_t *lv = list_level(T->list_style, T->level);
        char v[16] = "";
        /* where the label and the text go: the level's properties */
        const xn_t *lp = lv ? xn_kid(lv, "list-level-properties") : NULL;
        const xn_t *la = lp ? xn_kid(lp, "list-level-label-alignment") : NULL;
        if (la && xn_attr(la, "fo:margin-left", v, sizeof v)) {
            p->margin = len_pt(v);
            p->indent = xn_attr(la, "fo:text-indent", v, sizeof v) ? len_pt(v) : -p->margin;
        } else if (lp && (xn_attr(lp, "text:space-before", v, sizeof v) || xn_attr(lp, "text:min-label-width", v, sizeof v))) {
            float before = xn_attr(lp, "text:space-before", v, sizeof v) ? len_pt(v) : 0;
            float label = xn_attr(lp, "text:min-label-width", v, sizeof v) ? len_pt(v) : 18;
            p->margin = before + label;
            p->indent = -label;
        } else if (!pp.has_marL) {
            p->margin = 24.0f * (T->level + 1);
            p->indent = -20;
        }
        v[0] = 0;
        if (lv && !strcmp(lv->name, "text:list-level-style-number")) {
            xn_attr(lv, "style:num-format", v, sizeof v);
            p->autonum = !strcmp(v, "a") ? 2 : !strcmp(v, "A") ? 3 : !strcmp(v, "i") ? 4 : !strcmp(v, "I") ? 5 : v[0] ? 1 : 0;
            p->start = (int)xn_num(lv, "text:start-value", 1);
        } else if (lv && xn_attr(lv, "text:bullet-char", v, sizeof v) && v[0]) {
            /* symbol fonts' private bullets: the usual one */
            int l;
            uint32_t cp = of_utf8_get(v, &l);
            snprintf(p->bullet, sizeof p->bullet, "%.7s", cp >= 0xE000 && cp < 0xF900 ? "\xE2\x80\xA2" : v);
        } else if (!lv || strcmp(lv->name, "text:list-level-style-image")) {
            snprintf(p->bullet, sizeof p->bullet, "%s", "\xE2\x80\xA2");
        }
    }
    runs(T, p, pn, &pp, NULL, 0);
    p->bullet_color = p->nr ? p->r[0].color : 0x1000000;
}

static void text_of(tctx_t *T, const xn_t *n, int depth)
{
    if (depth > 10) return;
    for (const xn_t *c = n->kid; c; c = c->next) {
        if (!strcmp(c->name, "text:p") || !strcmp(c->name, "text:h")) paragraph(T, c);
        else if (!strcmp(c->name, "text:list")) {
            tctx_t L = *T;
            char sn[80];
            if (xn_attr(c, "text:style-name", sn, sizeof sn))
                for (int i = 0; i < T->O->nlists; i++) {
                    char nm[80] = "";
                    xn_attr(T->O->lists[i], "style:name", nm, sizeof nm);
                    if (!strcmp(nm, sn)) L.list_style = T->O->lists[i];
                }
            L.level = T->in_list ? T->level + 1 : 0;
            L.in_list = true;
            for (const xn_t *it = c->kid; it; it = it->next)
                if (!strcmp(it->name, "text:list-item") || !strcmp(it->name, "text:list-header")) text_of(&L, it, depth + 1);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Shapes                                                                      */
/* -------------------------------------------------------------------------- */

static void frame_props(odp_t *O, const xn_t *n, props_t *pp, char *cls, size_t clen)
{
    props_clear(pp);
    pp->size = 18;
    cls[0] = 0;
    xn_attr(n, "presentation:class", cls, clen);
    /* the master's style for this class, then the frame's own */
    char nm[100], sn[80];
    if (cls[0] && O->master_name[0]) {
        snprintf(nm, sizeof nm, "%.80s-%s", O->master_name, !strcmp(cls, "outline") ? "outline1" : cls);
        apply(O, nm, "presentation", pp, 0);
    }
    if (xn_attr(n, "presentation:style-name", sn, sizeof sn)) apply(O, sn, "presentation", pp, 0);
    if (xn_attr(n, "draw:style-name", sn, sizeof sn)) apply(O, sn, "graphic", pp, 0);
    if (xn_attr(n, "draw:text-style-name", sn, sizeof sn)) apply(O, sn, "paragraph", pp, 0);
}

static void geometry(const xn_t *n, sl_shape_t *sh)
{
    sh->x = alen(n, "svg:x", 0);
    sh->y = alen(n, "svg:y", 0);
    sh->w = alen(n, "svg:width", 0);
    sh->h = alen(n, "svg:height", 0);
    char tr[200];
    if (xn_attr(n, "draw:transform", tr, sizeof tr)) {
        /* "rotate (0.5) translate (2cm 3cm)": the place */
        const char *t = strstr(tr, "translate");
        if (t && (t = strchr(t, '('))) {
            char a[40], b[40];
            if (sscanf(t + 1, "%39s %39[^)]", a, b) == 2) { sh->x = len_pt(a); sh->y = len_pt(b); }
        }
        const char *r = strstr(tr, "rotate");
        if (r && (r = strchr(r, '('))) sh->rot = -strtof(r + 1, NULL) * 57.29578f;
    }
}

static void fill_shape(odp_t *O, const xn_t *n, sl_slide_t *s, bool own, int depth);

static void shape_text(odp_t *O, const xn_t *n, sl_shape_t *sh, const props_t *pp, const char *cls)
{
    tctx_t T = { .O = O, .tx = &sh->tx, .base = *pp };
    snprintf(T.cls, sizeof T.cls, "%s", cls);
    text_of(&T, n, 0);
    sh->tx.anchor = (uint8_t)(pp->valign >= 0 ? pp->valign : 0);
    if (pp->has_pad) { sh->tx.l = pp->pl; sh->tx.t = pp->pt; sh->tx.r = pp->pr; sh->tx.b = pp->pb; }
}

static void table(odp_t *O, const xn_t *t, sl_shape_t *sh)
{
    sh->kind = SL_TABLE;
    for (const xn_t *c = t->kid; c; c = c->next) {
        if (!strcmp(c->name, "table:table-column")) {
            int rep = (int)xn_num(c, "table:number-columns-repeated", 1);
            for (int k = 0; k < rep && sh->cols < 64; k++) {
                float *n = of_realloc(sh->colw, (size_t)(sh->cols + 1) * sizeof *n);
                if (!n) return;
                sh->colw = n;
                sh->colw[sh->cols++] = 0;
            }
        }
    }
    for (const xn_t *r = t->kid; r; r = r->next) {
        if (strcmp(r->name, "table:table-row")) continue;
        float *n = of_realloc(sh->rowh, (size_t)(sh->rows + 1) * sizeof *n);
        if (!n) return;
        sh->rowh = n;
        int row = sh->rows++;
        sh->rowh[row] = 0;
        int col = 0;
        for (const xn_t *c = r->kid; c; c = c->next) {
            bool covered = !strcmp(c->name, "table:covered-table-cell");
            if (strcmp(c->name, "table:table-cell") && !covered) continue;
            sl_cell_t *nc = of_realloc(sh->cells, (size_t)(sh->ncells + 1) * sizeof *nc);
            if (!nc) return;
            sh->cells = nc;
            sl_cell_t *cl = &sh->cells[sh->ncells++];
            memset(cl, 0, sizeof *cl);
            cl->row = row;
            cl->col = col;
            cl->cspan = (int)xn_num(c, "table:number-columns-spanned", 1);
            cl->rspan = (int)xn_num(c, "table:number-rows-spanned", 1);
            cl->hidden = covered;
            cl->tx.l = cl->tx.r = 5;
            cl->tx.t = cl->tx.b = 3;
            cl->tx.wrap = true;
            props_t pp;
            props_clear(&pp);
            pp.size = 18;
            char sn[80];
            if (xn_attr(c, "table:style-name", sn, sizeof sn)) apply(O, sn, "table-cell", &pp, 0);
            if (pp.fill_none == 0 && pp.fill) cl->fill = pp.fill;
            tctx_t T = { .O = O, .tx = &cl->tx, .base = pp };
            text_of(&T, c, 0);
            col++;
        }
        if (col > sh->cols) sh->cols = col;
    }
    /* columns without widths share the frame's */
    if (sh->cols) {
        float *n = of_realloc(sh->colw, (size_t)sh->cols * sizeof *n);
        if (n) sh->colw = n;
        for (int i = 0; i < sh->cols; i++) sh->colw[i] = sh->w / sh->cols;
    }
    for (int i = 0; i < sh->rows; i++) sh->rowh[i] = sh->h / (sh->rows ? sh->rows : 1);
}

static void fill_shape(odp_t *O, const xn_t *n, sl_slide_t *s, bool own, int depth)
{
    if (depth > 10) return;
    const char *nm = n->name;
    if (!strcmp(nm, "draw:g")) {
        for (const xn_t *c = n->kid; c; c = c->next) fill_shape(O, c, s, own, depth + 1);
        return;
    }
    bool frame = !strcmp(nm, "draw:frame"), rect = !strcmp(nm, "draw:rect"), ell = !strcmp(nm, "draw:ellipse") || !strcmp(nm, "draw:circle");
    bool cust = !strcmp(nm, "draw:custom-shape"), line = !strcmp(nm, "draw:line") || !strcmp(nm, "draw:connector");
    bool poly = !strcmp(nm, "draw:polygon") || !strcmp(nm, "draw:path") || !strcmp(nm, "draw:polyline");
    if (!frame && !rect && !ell && !cust && !line && !poly) return;
    char cls[24];
    props_t pp;
    frame_props(O, n, &pp, cls, sizeof cls);
    char v[16];
    /* a master's placeholder, or an empty one on a slide: not drawn */
    if (cls[0] && (!own || (xn_attr(n, "presentation:placeholder", v, sizeof v) && !strcmp(v, "true")))) return;
    sl_shape_t *sh = sl_add_shape(s);
    if (!sh) return;
    sh->placeholder = own;
    geometry(n, sh);
    if (line) {
        float x1 = alen(n, "svg:x1", 0), y1 = alen(n, "svg:y1", 0), x2 = alen(n, "svg:x2", 0), y2 = alen(n, "svg:y2", 0);
        sh->kind = SL_LINE;
        sh->x = x1 < x2 ? x1 : x2;
        sh->y = y1 < y2 ? y1 : y2;
        sh->w = x1 < x2 ? x2 - x1 : x1 - x2;
        sh->h = y1 < y2 ? y2 - y1 : y1 - y2;
        sh->flip_v = (x2 > x1) != (y2 > y1);
        sh->line = pp.stroke_none == 1 ? 0 : pp.stroke ? pp.stroke : 0x1000000;
        sh->line_w = pp.stroke_w > 0 ? pp.stroke_w : 1;
        return;
    }
    if (frame) {
        const xn_t *img = xn_kid(n, "image"), *tb = xn_kid(n, "text-box"), *tbl = xn_kid(n, "table"), *obj = xn_kid(n, "object");
        if (tbl) { table(O, tbl, sh); return; }
        if (img && !tb) {
            char href[260] = "";
            xn_attr(img, "xlink:href", href, sizeof href);
            if (href[0] && strncmp(href, "http", 4)) {
                sh->kind = SL_PIC;
                sh->pic = of_strdup(href[0] == '.' && href[1] == '/' ? href + 2 : href);
                return;
            }
        }
        if (obj) {
            sh->kind = SL_TEXT;
            sh->geom = SG_RECT;
            sh->line = 0x1A0A0A0;
            sh->line_w = 0.75f;
            sh->label = of_strdup(_("Objeto"));
            sh->tx.anchor = 1;
            sl_para_t *p = sl_add_para(&sh->tx);
            if (p) {
                p->align = OF_AL_CENTER;
                sl_run_t *r = sl_add_run(p, sh->label, strlen(sh->label));
                if (r) { r->color = 0x1808080; r->size = 14; r->i = 1; }
            }
            return;
        }
        sh->kind = SL_TEXT;
        if (pp.fill_none == 0 && pp.fill) { sh->fill = pp.fill; sh->geom = SG_RECT; }
        if (pp.stroke_none == 0 && pp.stroke) { sh->line = pp.stroke; sh->line_w = pp.stroke_w > 0 ? pp.stroke_w : 0.75f; sh->geom = SG_RECT; }
        if (tb) shape_text(O, tb, sh, &pp, cls);
        if (!strcmp(cls, "title") && !s->title && sh->tx.np) {
            of_sb_t t = { 0 };
            for (int i = 0; i < sh->tx.np; i++) for (int r = 0; r < sh->tx.p[i].nr; r++) of_sb_puts(&t, sh->tx.p[i].r[r].t);
            s->title = of_sb_take(&t);
        }
        if (!sh->tx.np && !sh->fill && !sh->line) s->nsh--;
        return;
    }
    /* shapes: fill and stroke from the graphic style (filled and outlined unless it says not) */
    sh->kind = SL_TEXT;
    sh->geom = ell ? SG_ELLIPSE : SG_RECT;
    if (cust) {
        const xn_t *eg = xn_kid(n, "enhanced-geometry");
        char t[48] = "";
        xn_attr(eg, "draw:type", t, sizeof t);
        sh->geom = strstr(t, "ellipse") || strstr(t, "circle") ? SG_ELLIPSE : strstr(t, "round") ? SG_ROUND : strstr(t, "triangle") ? SG_TRIANGLE
                 : strstr(t, "diamond") ? SG_DIAMOND : strstr(t, "arrow") ? SG_ARROW : SG_RECT;
    }
    sh->fill = pp.fill_none == 1 ? 0 : pp.fill ? pp.fill : 0x1729FCF;
    sh->line = pp.stroke_none == 1 ? 0 : pp.stroke ? pp.stroke : 0x13465A4;
    sh->line_w = pp.stroke_w > 0 ? pp.stroke_w : 0.75f;
    shape_text(O, n, sh, &pp, cls);
    sh->tx.anchor = (uint8_t)(pp.valign >= 0 ? pp.valign : 1);
    for (int i = 0; i < sh->tx.np; i++) if (pp.algn < 0) sh->tx.p[i].align = OF_AL_CENTER;
}

static uint32_t page_bg(odp_t *O, const char *style_name)
{
    props_t pp;
    props_clear(&pp);
    apply(O, style_name, "drawing-page", &pp, 0);
    if (pp.fill_none == 1) return 0;
    return pp.fill;
}

bool sl_odp_read(of_deck_t *dk, char *err, size_t errlen)
{
    odp_t O = { .dk = dk };
    snprintf(dk->kind, sizeof dk->kind, "ODP");
    size_t cn, sn = 0;
    O.csrc = (char *)of_deck_part(dk, "content.xml", &cn);
    if (!O.csrc) { snprintf(err, errlen, "format"); return false; }
    O.ssrc = (char *)of_deck_part(dk, "styles.xml", &sn);
    O.content = xn_parse(O.csrc, cn);
    O.styles = O.ssrc ? xn_parse(O.ssrc, sn) : NULL;
    xn_t *croot = xn_kid(O.content, "document-content");
    xn_t *sroot = O.styles ? xn_kid(O.styles, "document-styles") : NULL;
    collect(&O, xn_kid(croot, "automatic-styles"));
    collect(&O, xn_kid(sroot, "automatic-styles"));
    collect(&O, xn_kid(sroot, "styles"));
    xn_t *masters = xn_kid(sroot, "master-styles");
    /* the page size: the first master's page layout */
    dk->w = 792;
    dk->h = 612;
    xn_t *pres = xn_path(croot, "body/presentation");
    for (xn_t *pg = pres ? pres->kid : NULL; pg; pg = pg->next) {
        if (strcmp(pg->name, "draw:page")) continue;
        sl_slide_t *ns = of_realloc(dk->s, (size_t)(dk->ns + 1) * sizeof *ns);
        if (!ns) break;
        dk->s = ns;
        sl_slide_t *s = &dk->s[dk->ns++];
        memset(s, 0, sizeof *s);
        char mname[80] = "", st[80] = "";
        xn_attr(pg, "draw:master-page-name", mname, sizeof mname);
        snprintf(O.master_name, sizeof O.master_name, "%s", mname);
        /* the master page: its size, its background, its shapes */
        const xn_t *mp = NULL;
        for (xn_t *m = masters ? masters->kid : NULL; m; m = m->next) {
            char nm[80] = "";
            if (strcmp(m->name, "style:master-page") || !xn_attr(m, "style:name", nm, sizeof nm)) continue;
            if (!mp || !strcmp(nm, mname)) mp = m;
            if (!strcmp(nm, mname)) break;
        }
        if (mp && dk->ns == 1) {
            char pl[80] = "";
            xn_attr(mp, "style:page-layout-name", pl, sizeof pl);
            const xn_t *pls = NULL;
            for (xn_t *a = xn_kid(sroot, "automatic-styles") ? xn_kid(sroot, "automatic-styles")->kid : NULL; a; a = a->next) {
                char nm[80] = "";
                if (!strcmp(a->name, "style:page-layout") && xn_attr(a, "style:name", nm, sizeof nm) && !strcmp(nm, pl)) pls = a;
            }
            const xn_t *pp = pls ? xn_kid(pls, "page-layout-properties") : NULL;
            if (pp) { dk->w = alen(pp, "fo:page-width", dk->w); dk->h = alen(pp, "fo:page-height", dk->h); }
        }
        if (xn_attr(pg, "draw:style-name", st, sizeof st)) s->bg = page_bg(&O, st);
        if (!s->bg && mp && xn_attr(mp, "draw:style-name", st, sizeof st)) s->bg = page_bg(&O, st);
        if (!s->bg) s->bg = 0x1FFFFFF;
        for (const xn_t *c = mp ? mp->kid : NULL; c; c = c->next) fill_shape(&O, c, s, false, 0);
        for (const xn_t *c = pg->kid; c; c = c->next) {
            if (!strcmp(c->name, "presentation:notes")) {
                of_sb_t sb = { 0 };
                for (const xn_t *f = c->kid; f; f = f->next) {
                    char cls[24] = "";
                    if (strcmp(f->name, "draw:frame") || !xn_attr(f, "presentation:class", cls, sizeof cls) || strcmp(cls, "notes")) continue;
                    const xn_t *tb = xn_kid(f, "text-box");
                    for (const xn_t *p = tb ? tb->kid : NULL; p; p = p->next) {
                        of_sb_t line = { 0 };
                        xn_text(p, &line);
                        if (line.n) { if (sb.n) of_sb_putc(&sb, '\n'); of_sb_put(&sb, line.s, line.n); }
                        of_sb_free(&line);
                    }
                }
                s->notes = sb.n ? of_sb_take(&sb) : (of_sb_free(&sb), NULL);
                continue;
            }
            fill_shape(&O, c, s, true, 0);
        }
        char vis[16];
        if (xn_attr(pg, "presentation:visibility", vis, sizeof vis) && !strcmp(vis, "hidden")) s->hidden = true;
    }
    of_free(O.st);
    of_free(O.lists);
    xn_free(O.content);
    xn_free(O.styles);
    of_free(O.csrc);
    of_free(O.ssrc);
    if (!dk->ns) { snprintf(err, errlen, "empty"); return false; }
    return true;
}
