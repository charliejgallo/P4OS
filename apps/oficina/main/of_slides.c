/*
 * P4OS - Office: the deck (of_slides.h): loading by format, freeing, and
 * the deck as a document (its text, for reading, copying and saving as
 * docx, odt or txt).
 */
#include "of_slides.h"
#include "of_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char *(*of_tr)(const char *es);
#define _(s) (of_tr ? of_tr(s) : (s))

sl_shape_t *sl_add_shape(sl_slide_t *s)
{
    sl_shape_t *n = of_realloc(s->sh, (size_t)(s->nsh + 1) * sizeof *n);
    if (!n) return NULL;
    s->sh = n;
    sl_shape_t *sh = &s->sh[s->nsh++];
    memset(sh, 0, sizeof *sh);
    sh->tx.l = sh->tx.r = 7.2f;
    sh->tx.t = sh->tx.b = 3.6f;
    sh->tx.wrap = true;
    return sh;
}

sl_para_t *sl_add_para(sl_text_t *t)
{
    sl_para_t *n = of_realloc(t->p, (size_t)(t->np + 1) * sizeof *n);
    if (!n) return NULL;
    t->p = n;
    sl_para_t *p = &t->p[t->np++];
    memset(p, 0, sizeof *p);
    p->line = 1;
    p->start = 1;
    p->empty_size = 18;
    return p;
}

sl_run_t *sl_add_run(sl_para_t *p, const char *text, size_t n)
{
    sl_run_t *r = of_realloc(p->r, (size_t)(p->nr + 1) * sizeof *r);
    if (!r) return NULL;
    p->r = r;
    sl_run_t *x = &p->r[p->nr++];
    memset(x, 0, sizeof *x);
    x->t = of_alloc(n + 1);
    if (x->t) memcpy(x->t, text, n);
    x->size = 18;
    return x;
}

void sl_number(sl_text_t *t)
{
    int count[10] = { 0 };
    for (int i = 0; i < t->np; i++) {
        sl_para_t *p = &t->p[i];
        int lv = p->level < 10 ? p->level : 9;
        for (int k = lv + 1; k < 10; k++) count[k] = 0;
        if (!p->autonum) { count[lv] = 0; continue; }
        if (!count[lv]) count[lv] = p->start > 0 ? p->start : 1;
        else count[lv]++;
        p->number = count[lv];
    }
}

void sl_text_free(sl_text_t *t)
{
    for (int i = 0; i < t->np; i++) {
        for (int k = 0; k < t->p[i].nr; k++) { of_free(t->p[i].r[k].t); of_free(t->p[i].r[k].link); }
        of_free(t->p[i].r);
    }
    of_free(t->p);
    t->p = NULL;
    t->np = 0;
}

void of_deck_free(of_deck_t *dk)
{
    if (!dk) return;
    for (int i = 0; i < dk->ns; i++) {
        sl_slide_t *s = &dk->s[i];
        for (int k = 0; k < s->nsh; k++) {
            sl_shape_t *sh = &s->sh[k];
            sl_text_free(&sh->tx);
            for (int c = 0; c < sh->ncells; c++) sl_text_free(&sh->cells[c].tx);
            of_free(sh->cells);
            of_free(sh->colw);
            of_free(sh->rowh);
            of_free(sh->pic);
            of_free(sh->label);
        }
        of_free(s->sh);
        of_free(s->bg_pic);
        of_free(s->notes);
        of_free(s->title);
    }
    of_free(dk->s);
    of_zip_close(&dk->zip);
    of_free(dk->file);
    of_free(dk);
}

bool of_is_deck_ext(const char *e)
{
    return !strcmp(e, "pptx") || !strcmp(e, "ppsx") || !strcmp(e, "potx") || !strcmp(e, "pptm") || !strcmp(e, "odp") || !strcmp(e, "otp") ||
           !strcmp(e, "ppt") || !strcmp(e, "pps") || !strcmp(e, "pot");
}

uint8_t *of_deck_part(const of_deck_t *dk, const char *name, size_t *len)
{
    if (dk->pic_files) return of_file_read(name, len, 32u * 1024 * 1024);
    int i = of_zip_find(&dk->zip, name);
    return i < 0 ? NULL : of_zip_read(&dk->zip, i, len);
}

of_deck_t *of_deck_load(const char *path, char *err, size_t errlen)
{
    const char *x = of_ext(path);
    if (!strcmp(x, "ppt") || !strcmp(x, "pps") || !strcmp(x, "pot")) {
        extern struct of_deck_s *of_ppt_load(const char *path, char *err, size_t errlen);
        return of_ppt_load(path, err, errlen);
    }
    of_deck_t *dk = of_alloc(sizeof *dk);
    if (!dk) { snprintf(err, errlen, "memory"); return NULL; }
    size_t n;
    dk->file = of_file_read(path, &n, 96u * 1024 * 1024);
    dk->len = n;
    if (!dk->file) { snprintf(err, errlen, "read"); of_free(dk); return NULL; }
    if (of_is_encrypted_ooxml(dk->file, n)) { snprintf(err, errlen, "encrypted"); of_deck_free(dk); return NULL; }
    if (!of_zip_open(&dk->zip, dk->file, n)) { snprintf(err, errlen, "zip"); of_deck_free(dk); return NULL; }
    const char *e = of_ext(path);
    bool ok = !strcmp(e, "odp") || !strcmp(e, "otp") ? sl_odp_read(dk, err, errlen) : sl_pptx_read(dk, err, errlen);
    if (!ok) { of_deck_free(dk); return NULL; }
    for (int i = 0; i < dk->ns; i++)
        for (int k = 0; k < dk->s[i].nsh; k++) {
            sl_shape_t *sh = &dk->s[i].sh[k];
            sl_number(&sh->tx);
            for (int c = 0; c < sh->ncells; c++) sl_number(&sh->cells[c].tx);
        }
    return dk;
}

/* -------------------------------------------------------------------------- */
/* The deck as a document                                                      */
/* -------------------------------------------------------------------------- */

static int s_numid = 7000;

static void text_into(of_bld_t *b, const sl_text_t *t, bool skip_title_of, const char *title)
{
    s_numid++;
    for (int i = 0; i < t->np; i++) {
        const sl_para_t *p = &t->p[i];
        bool any = false;
        for (int k = 0; k < p->nr; k++) if (p->r[k].t && p->r[k].t[0]) any = true;
        if (!any) continue;
        b->list = p->bullet[0] ? OF_LIST_BULLET : p->autonum ? OF_LIST_NUMBER : 0;
        b->ilvl = p->level;
        b->numid = s_numid;
        b->cur = -1;
        of_bld_para(b);
        for (int k = 0; k < p->nr; k++) {
            const sl_run_t *r = &p->r[k];
            b->st = (of_rstyle_t){ .link = -1 };
            if (r->b) b->st.b = OF_TRI_ON;
            if (r->i) b->st.i = OF_TRI_ON;
            if (r->u) b->st.u = OF_TRI_ON;
            if (r->s) b->st.s = OF_TRI_ON;
            b->st.sup = r->sup;
            /* light colours read badly on paper: only the dark and the bright ones */
            uint32_t c = r->color & 0xFFFFFF;
            int lum = (int)(((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10;
            if (r->color && lum < 200 && lum > 20) b->st.color = r->color;
            if (r->link) of_bld_link(b, r->link);
            const char *s = r->t ? r->t : "";
            /* a line break inside a run */
            const char *nl;
            while ((nl = strchr(s, '\n'))) {
                of_bld_text(b, s, (size_t)(nl - s));
                if (b->cur >= 0) of_para_append(b->d, &b->d->p[b->cur], "\n", 1, (uint16_t)of_doc_style(b->d, &b->st));
                b->space = false;
                s = nl + 1;
            }
            of_bld_text(b, s, strlen(s));
            if (r->link) of_bld_link(b, NULL);
        }
        b->st = (of_rstyle_t){ .link = -1 };
    }
    b->list = 0;
    b->ilvl = 0;
    (void)skip_title_of;
    (void)title;
}

/* the shapes in reading order: top to bottom, then left to right */
static int s_cmp_slide;
static const of_deck_t *s_cmp_deck;
static int shape_cmp(const void *a, const void *b)
{
    const sl_shape_t *x = &s_cmp_deck->s[s_cmp_slide].sh[*(const int *)a], *y = &s_cmp_deck->s[s_cmp_slide].sh[*(const int *)b];
    float dy = x->y - y->y;
    if (dy < -10 || dy > 10) return dy < 0 ? -1 : 1;
    return x->x < y->x ? -1 : x->x > y->x;
}

struct of_doc_s *of_deck_to_doc(const of_deck_t *dk, bool notes)
{
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    if (!d) return NULL;
    of_bld_t b;
    of_bld_init(&b, d);
    for (int i = 0; i < dk->ns; i++) {
        const sl_slide_t *s = &dk->s[i];
        if (i) of_bld_break(&b);
        b.heading = 2;
        b.cur = -1;
        of_bld_para(&b);
        char h[300];
        const char *word = _("Diapositiva");
        if (s->title && s->title[0]) snprintf(h, sizeof h, "%d. %.280s", i + 1, s->title);
        else snprintf(h, sizeof h, "%s %d", word, i + 1);
        for (char *q = h; *q; q++) if (*q == '\n' || *q == '\v') *q = ' ';
        of_bld_text(&b, h, strlen(h));
        b.heading = 0;
        int *ord = of_alloc((size_t)(s->nsh + 1) * sizeof(int));
        int n = 0;
        for (int k = 0; k < s->nsh; k++) if (s->sh[k].placeholder || s->sh[k].kind != SL_TEXT || s->sh[k].tx.np) ord[n++] = k;
        s_cmp_slide = i;
        s_cmp_deck = dk;
        if (ord) qsort(ord, (size_t)n, sizeof(int), shape_cmp);
        for (int q = 0; ord && q < n; q++) {
            const sl_shape_t *sh = &s->sh[ord[q]];
            if (!sh->placeholder) continue;         /* the master's decoration is not content */
            if (sh->kind == SL_TEXT) {
                /* the title is already the heading */
                bool is_title = s->title && sh->tx.np && sh->tx.p[0].nr && sh->tx.p[0].r[0].t && !strncmp(s->title, sh->tx.p[0].r[0].t, strlen(sh->tx.p[0].r[0].t)) && q < 2;
                if (is_title) continue;
                text_into(&b, &sh->tx, false, NULL);
            } else if (sh->kind == SL_PIC && sh->pic) {
                b.align = OF_AL_CENTER;
                b.cur = -1;
                of_bld_para(&b);
                float w = sh->w, hh = sh->h;
                if (w > 400) { hh = hh * 400 / w; w = 400; }
                of_bld_image(&b, sh->pic, dk->pic_files, (int)w, (int)hh);
                b.align = 0;
                b.cur = -1;
            } else if (sh->kind == SL_TABLE && sh->cells) {
                of_bld_table(&b);
                int t = d->ntables - 1;
                for (int c = 0; c < sh->cols && c < 64; c++) d->tables[t].colw[c] = (int32_t)(sh->colw ? sh->colw[c] : 100);
                for (int r = 0; r < sh->rows; r++) {
                    of_bld_row(&b);
                    for (int c = 0; c < sh->ncells; c++) {
                        const sl_cell_t *cl = &sh->cells[c];
                        if (cl->row != r || cl->hidden) continue;
                        of_bld_cell(&b, cl->cspan);
                        for (int pi = 0; pi < cl->tx.np; pi++) {
                            if (pi) { b.cur = -1; of_bld_para(&b); }
                            for (int k = 0; k < cl->tx.p[pi].nr; k++) {
                                b.st = (of_rstyle_t){ .link = -1 };
                                if (cl->tx.p[pi].r[k].b) b.st.b = OF_TRI_ON;
                                const char *t2 = cl->tx.p[pi].r[k].t ? cl->tx.p[pi].r[k].t : "";
                                of_bld_text(&b, t2, strlen(t2));
                            }
                        }
                        b.st = (of_rstyle_t){ .link = -1 };
                    }
                }
                of_bld_table_end(&b);
                b.cur = -1;
            } else if (sh->label) {
                b.cur = -1;
                of_bld_para(&b);
                b.st.color = 0x1808080;
                char l[80];
                snprintf(l, sizeof l, "[%s]", sh->label);
                of_bld_text(&b, l, strlen(l));
                b.st.color = 0;
            }
        }
        of_free(ord);
        if (notes && s->notes && s->notes[0]) {
            b.cur = -1;
            b.indent = 1;
            of_bld_para(&b);
            b.st = (of_rstyle_t){ .link = -1, .i = OF_TRI_ON, .color = 0x1606060 };
            const char *lab = _("Notas: ");
            of_bld_text(&b, lab, strlen(lab));
            const char *p = s->notes;
            while (*p) {
                const char *nl = strchr(p, '\n');
                size_t l = nl ? (size_t)(nl - p) : strlen(p);
                of_bld_text(&b, p, l);
                if (!nl) break;
                if (b.cur >= 0) of_para_append(d, &d->p[b.cur], "\n", 1, (uint16_t)of_doc_style(d, &b.st));
                b.space = false;
                p = nl + 1;
            }
            b.st = (of_rstyle_t){ .link = -1 };
            b.indent = 0;
        }
    }
    of_bld_done(&b);
    /* the pictures are parts of the deck's package: the document carries it */
    d->file = dk->len ? of_alloc(dk->len + 1) : NULL;
    if (d->file) {
        memcpy(d->file, dk->file, dk->len);
        d->file_len = dk->len;
        if (!of_zip_open(&d->zip, d->file, d->file_len)) memset(&d->zip, 0, sizeof d->zip);
    }
    snprintf(d->kind, sizeof d->kind, "%s", dk->kind);
    return d;
}

/* -------------------------------------------------------------------------- */
/* A small XML tree (the readers walk it up and down)                          */
/* -------------------------------------------------------------------------- */

static xn_t *xn_new(xn_t *parent, const char *name, int nl)
{
    xn_t *n = of_alloc(sizeof *n);
    if (!n) return NULL;
    n->name = of_alloc((size_t)nl + 1);
    if (n->name) memcpy(n->name, name, (size_t)nl);
    const char *c = n->name ? strchr(n->name, ':') : NULL;
    n->local = c ? c + 1 : n->name ? n->name : "";
    n->parent = parent;
    if (parent) {
        if (parent->last) parent->last->next = n;
        else parent->kid = n;
        parent->last = n;
    }
    return n;
}

xn_t *xn_parse(const char *src, size_t len)
{
    xn_t *root = xn_new(NULL, "#root", 5);
    if (!root) return NULL;
    xn_t *cur = root;
    of_xml_t X;
    of_xml_init(&X, src, len);
    int t, depth = 0;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_OPEN || t == OX_EMPTY) {
            xn_t *n = xn_new(cur, X.name, X.name_len);
            if (!n) break;
            n->attrs = X.attrs;
            n->alen = X.attrs_len;
            if (t == OX_OPEN && depth < 200) { cur = n; depth++; }
        } else if (t == OX_CLOSE) {
            if (cur->parent) { cur = cur->parent; depth--; }
        } else if (t == OX_TEXT) {
            xn_t *n = xn_new(cur, "#text", 5);
            if (!n) break;
            of_sb_t sb = { 0 };
            of_xml_unescape(X.src + X.start, X.end - X.start, &sb);
            n->text = sb.s ? sb.s : of_alloc(1);
        }
    }
    return root;
}

void xn_free(xn_t *n)
{
    while (n) {
        xn_t *next = n->next;
        xn_free(n->kid);
        of_free(n->name);
        of_free(n->text);
        of_free(n);
        n = next;
    }
}

xn_t *xn_kid(const xn_t *n, const char *local)
{
    if (!n) return NULL;
    for (xn_t *c = n->kid; c; c = c->next)
        if (!strcmp(c->local, local)) return c;
    return NULL;
}

xn_t *xn_path(const xn_t *n, const char *path)
{
    /* "spPr/xfrm/off" */
    char seg[48];
    while (n && *path) {
        const char *sl = strchr(path, '/');
        size_t l = sl ? (size_t)(sl - path) : strlen(path);
        if (l >= sizeof seg) return NULL;
        memcpy(seg, path, l);
        seg[l] = 0;
        n = xn_kid(n, seg);
        path += l + (sl ? 1 : 0);
    }
    return (xn_t *)n;
}

bool xn_attr(const xn_t *n, const char *name, char *out, size_t len)
{
    if (!n || !n->attrs) return false;
    const char *v;
    int vl;
    if (!of_xml_attr_raw(n->attrs, n->alen, name, &v, &vl)) return false;
    of_sb_t sb = { 0 };
    of_xml_unescape(v, (size_t)vl, &sb);
    snprintf(out, len, "%s", sb.s ? sb.s : "");
    of_sb_free(&sb);
    return true;
}

double xn_num(const xn_t *n, const char *name, double def)
{
    char v[40];
    if (!xn_attr(n, name, v, sizeof v) || !v[0]) return def;
    return strtod(v, NULL);
}

void xn_text(const xn_t *n, of_sb_t *sb)
{
    for (const xn_t *c = n ? n->kid : NULL; c; c = c->next) {
        if (c->text) of_sb_puts(sb, c->text);
        else xn_text(c, sb);
    }
}
