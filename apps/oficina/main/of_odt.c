/*
 * P4OS - Office: ODT (OpenDocument text).
 *
 * Reading: content.xml (or a flat .fodt), its automatic styles and the named
 * ones of styles.xml: what is bold, italic, coloured, its size, what makes a
 * heading, the alignment, and the list styles (bullet or number per level).
 * Every <text:p> and <text:h>, in the body, in lists, in sections and in
 * table cells, becomes a paragraph; nested spans become one run style that
 * keeps the names of all of them. Frames (pictures), notes, fields,
 * bookmarks and comments are objects with their XML; tracked changes and
 * indexes lock their paragraph.
 *
 * Saving into the same file rewrites content.xml around the edited
 * paragraphs only; every other part goes out as it was. An edited
 * paragraph's text is written inside its original spans, and what the
 * person changed (bold, italic...) goes in one more span inside, with an
 * automatic style that says only that: the original styles stay whole. A
 * paragraph split inside a list becomes a new list item.
 */
#include "of_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ODT_MAX (48u * 1024 * 1024)

/* -------------------------------------------------------------------------- */
/* Styles                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *name;
    int   fmt[10];                  /* per level: 0 bullet, 1..5 as of_doc.h */
    char  bullet[10][8];
} lstyle_t;

typedef struct {
    of_doc_t *d;
    char    **pnames;               /* parent names, by d->ps index */
    lstyle_t *ls;
    int       nls;
    int       footnote;
    size_t    item_open, item_end;  /* the <text:list-item> just opened */
    bool      item_closed;
} rd_t;

static int ps_find(const of_doc_t *d, const char *name)
{
    for (int i = 0; i < d->nps; i++)
        if (d->ps[i].id && !strcmp(d->ps[i].id, name)) return i;
    return -1;
}

static uint16_t size_half_pt(const char *v, uint16_t base)
{
    char *end;
    float x = strtof(v, &end);
    if (*end == '%') return (uint16_t)(base * x / 100);
    if (!strncmp(end, "pt", 2)) return (uint16_t)(x * 2 + 0.5f);
    if (!strncmp(end, "cm", 2)) return (uint16_t)(x * 56.69f + 0.5f);
    if (!strncmp(end, "in", 2)) return (uint16_t)(x * 144 + 0.5f);
    return (uint16_t)(x * 2 + 0.5f);
}

static uint32_t css_color(const char *v)
{
    if (v[0] != '#' || strlen(v) < 7) return 0;
    return (uint32_t)strtoul(v + 1, NULL, 16) | 0x1000000u;
}

static void read_props(of_xml_t *X, of_pstyle_t *s, of_rstyle_t *rs, uint16_t base)
{
    char v[64];
    if (of_xml_attr(X, "fo:font-weight", v, sizeof v)) {
        uint8_t t = (!strcmp(v, "bold") || atoi(v) >= 600) ? OF_TRI_ON : OF_TRI_OFF;
        if (s) s->b = t;
        if (rs) rs->b = t;
    }
    if (of_xml_attr(X, "fo:font-style", v, sizeof v)) {
        uint8_t t = (!strcmp(v, "italic") || !strcmp(v, "oblique")) ? OF_TRI_ON : OF_TRI_OFF;
        if (s) s->i = t;
        if (rs) rs->i = t;
    }
    if (of_xml_attr(X, "style:text-underline-style", v, sizeof v)) {
        uint8_t t = strcmp(v, "none") ? OF_TRI_ON : OF_TRI_OFF;
        if (s) s->u = t;
        if (rs) rs->u = t;
    }
    if (of_xml_attr(X, "style:text-line-through-style", v, sizeof v)) {
        uint8_t t = strcmp(v, "none") ? OF_TRI_ON : OF_TRI_OFF;
        if (s) s->s = t;
        if (rs) rs->s = t;
    }
    if (of_xml_attr(X, "fo:color", v, sizeof v)) {
        uint32_t c = css_color(v);
        if (s) s->color = c;
        if (rs) rs->color = c;
    }
    if (of_xml_attr(X, "fo:font-size", v, sizeof v)) {
        uint16_t h = size_half_pt(v, base);
        if (s) s->half_pt = h;
        if (rs) rs->half_pt = h;
    }
    if (of_xml_attr(X, "fo:background-color", v, sizeof v) && rs && strcmp(v, "transparent")) rs->hl = css_color(v);
    if (of_xml_attr(X, "style:text-position", v, sizeof v) && rs)
        rs->sup = !strncmp(v, "super", 5) || (v[0] != '-' && atoi(v) > 0) ? 1 : (!strncmp(v, "sub", 3) || v[0] == '-') ? 2 : 0;
    if (of_xml_attr(X, "fo:text-align", v, sizeof v) && s)
        s->align = !strcmp(v, "center") ? OF_AL_CENTER : (!strcmp(v, "end") || !strcmp(v, "right")) ? OF_AL_RIGHT
                 : !strcmp(v, "justify") ? OF_AL_JUSTIFY : OF_AL_LEFT;
}

static int num_code(const char *f)
{
    if (!f || !f[0]) return 1;
    if (!strcmp(f, "a")) return 2;
    if (!strcmp(f, "A")) return 3;
    if (!strcmp(f, "i")) return 4;
    if (!strcmp(f, "I")) return 5;
    return 1;
}

/* the styles of a part (content.xml's automatic ones, styles.xml's named ones) */
static void read_styles(rd_t *R, const char *src, size_t len)
{
    of_doc_t *d = R->d;
    of_xml_t X;
    of_xml_init(&X, src, len);
    int t;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_OPEN && of_xml_is(&X, "office:body")) return;
        if ((t == OX_OPEN || t == OX_EMPTY) && of_xml_is(&X, "style:default-style")) {
            char fam[24] = "";
            of_xml_attr(&X, "style:family", fam, sizeof fam);
            if (t == OX_EMPTY || strcmp(fam, "paragraph")) { if (t == OX_OPEN) of_xml_skip(&X); continue; }
            int dd = 1, tt;
            while (dd > 0 && (tt = of_xml_next(&X)) != OX_END && tt != OX_ERROR) {
                if (tt == OX_CLOSE) { dd--; continue; }
                if ((tt == OX_EMPTY || tt == OX_OPEN) && of_xml_is(&X, "style:text-properties")) {
                    char v[32];
                    if (of_xml_attr(&X, "fo:font-size", v, sizeof v)) d->def_half_pt = size_half_pt(v, 24);
                }
                if (tt == OX_OPEN) dd++;
            }
            continue;
        }
        if ((t == OX_OPEN || t == OX_EMPTY) && of_xml_is(&X, "style:style")) {
            char fam[24] = "", name[96] = "", parent[96] = "", lvl[8] = "";
            of_xml_attr(&X, "style:family", fam, sizeof fam);
            of_xml_attr(&X, "style:name", name, sizeof name);
            of_xml_attr(&X, "style:parent-style-name", parent, sizeof parent);
            of_xml_attr(&X, "style:default-outline-level", lvl, sizeof lvl);
            if (strcmp(fam, "paragraph") && strcmp(fam, "text")) { if (t == OX_OPEN) of_xml_skip(&X); continue; }
            of_pstyle_t s = { .parent = -1 };
            s.id = of_strdup(name);
            char dn[96] = "";
            if (of_xml_attr(&X, "style:display-name", dn, sizeof dn)) s.name = of_strdup(dn);
            else s.name = of_strdup(name);
            s.heading = (uint8_t)atoi(lvl);
            if (t == OX_OPEN) {
                int dd = 1, tt;
                while (dd > 0 && (tt = of_xml_next(&X)) != OX_END && tt != OX_ERROR) {
                    if (tt == OX_CLOSE) { dd--; continue; }
                    if ((tt == OX_EMPTY || tt == OX_OPEN) &&
                        (of_xml_is(&X, "style:text-properties") || of_xml_is(&X, "style:paragraph-properties")))
                        read_props(&X, &s, NULL, d->def_half_pt);
                    if (tt == OX_OPEN) dd++;
                }
            }
            of_pstyle_t *n = of_realloc(d->ps, (size_t)(d->nps + 1) * sizeof *n);
            char **pn = of_realloc(R->pnames, (size_t)(d->nps + 1) * sizeof *pn);
            if (!n || !pn) { if (n) d->ps = n; if (pn) R->pnames = pn; of_free(s.id); of_free(s.name); return; }
            d->ps = n;
            R->pnames = pn;
            R->pnames[d->nps] = of_strdup(parent);
            if (!strcmp(name, "Standard")) d->ps_normal = d->nps;
            d->ps[d->nps++] = s;
            continue;
        }
        if (t == OX_OPEN && of_xml_is(&X, "text:list-style")) {
            lstyle_t L;
            memset(&L, 0, sizeof L);
            char name[96] = "";
            of_xml_attr(&X, "style:name", name, sizeof name);
            L.name = of_strdup(name);
            for (int k = 0; k < 10; k++) L.fmt[k] = 0;
            int dd = 1, tt;
            while (dd > 0 && (tt = of_xml_next(&X)) != OX_END && tt != OX_ERROR) {
                if (tt == OX_CLOSE) { dd--; continue; }
                char v[16];
                if ((tt == OX_OPEN || tt == OX_EMPTY) &&
                    (of_xml_is(&X, "text:list-level-style-number") || of_xml_is(&X, "text:list-level-style-bullet"))) {
                    int lv = of_xml_attr(&X, "text:level", v, sizeof v) ? atoi(v) - 1 : 0;
                    if (lv >= 0 && lv < 10) {
                        if (of_xml_is(&X, "text:list-level-style-bullet")) {
                            L.fmt[lv] = 0;
                            of_xml_attr(&X, "text:bullet-char", L.bullet[lv], sizeof L.bullet[lv]);
                        } else {
                            char f[8] = "1";
                            of_xml_attr(&X, "style:num-format", f, sizeof f);
                            L.fmt[lv] = num_code(f);
                        }
                    }
                }
                if (tt == OX_OPEN) dd++;
            }
            lstyle_t *n = of_realloc(R->ls, (size_t)(R->nls + 1) * sizeof *n);
            if (n) { R->ls = n; R->ls[R->nls++] = L; }
            else of_free(L.name);
        }
    }
}

static int list_index(rd_t *R, const char *name)
{
    for (int i = 0; i < R->nls; i++)
        if (!strcmp(R->ls[i].name, name)) return i;
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Paragraphs                                                                  */
/* -------------------------------------------------------------------------- */

/* the run style of a stack of span names */
static uint16_t span_style(rd_t *R, char names[][96], int n, int link)
{
    of_doc_t *d = R->d;
    of_rstyle_t rs = { .link = (int16_t)link };
    of_sb_t key = { 0 };
    for (int i = 0; i < n; i++) {
        if (i) of_sb_putc(&key, '\x1f');
        of_sb_puts(&key, names[i]);
        /* the style and its parents, for showing */
        int s = ps_find(d, names[i]);
        int chain[8], c = 0;
        for (; s >= 0 && c < 8; s = d->ps[s].parent) chain[c++] = s;
        for (int k = c - 1; k >= 0; k--) {
            const of_pstyle_t *ps = &d->ps[chain[k]];
            if (ps->b) rs.b = ps->b;
            if (ps->i) rs.i = ps->i;
            if (ps->u) rs.u = ps->u;
            if (ps->s) rs.s = ps->s;
            if (ps->color) rs.color = ps->color;
            if (ps->half_pt) rs.half_pt = ps->half_pt;
        }
    }
    rs.raw = n ? key.s : NULL;
    int si = of_doc_style(d, &rs);
    of_sb_free(&key);
    return (uint16_t)si;
}

static void add_obj(rd_t *R, of_para_t *p, int kind, const char *src, size_t a, size_t z, uint16_t st)
{
    int o = of_doc_add_obj(R->d, kind, src + a, z - a);
    of_para_append_obj(R->d, p, o, st);
}

static void parse_p(rd_t *R, of_xml_t *X, int cell, bool locked, int list_ix, int list_lvl, bool numbered)
{
    of_doc_t *d = R->d;
    of_para_t *p = of_doc_insert_para(d, d->np);
    if (!p) { if (X->type == OX_OPEN) of_xml_skip(X); return; }
    int pi = d->np - 1;
    bool h = of_xml_is(X, "text:h");
    p->cell = cell;
    p->locked = locked;
    p->orig = p->slot = d->norig;
    char v[96];
    if (of_xml_attr(X, "text:style-name", v, sizeof v)) p->pstyle = ps_find(d, v);
    if (h) {
        p->heading = of_xml_attr(X, "text:outline-level", v, sizeof v) ? (uint8_t)atoi(v) : 1;
        if (!p->heading) p->heading = 1;
    }
    if (list_ix >= 0 || list_lvl >= 0) {
        p->in_list = true;
        if (numbered) {
            int lv = list_lvl < 0 ? 0 : list_lvl > 9 ? 9 : list_lvl;
            p->ilvl = (uint8_t)lv;
            p->numid = list_ix;
            p->list = list_ix >= 0 && R->ls[list_ix].fmt[lv] == 0 ? OF_LIST_BULLET : OF_LIST_NUMBER;
            if (list_ix < 0) p->list = OF_LIST_BULLET;
        }
    }
    size_t s0 = X->start;
    p->raw_open = of_strndup(X->src + X->start, X->end - X->start);
    if (X->type == OX_OPEN) {
        char names[8][96];
        int nn = 0, links[8], nl = 0;
        int depth = 1, t;
        uint16_t cur = 0;
        while (depth > 0 && (t = of_xml_next(X)) != OX_END && t != OX_ERROR) {
            p = &d->p[pi];
            if (t == OX_TEXT) {
                of_sb_t tx = { 0 };
                of_xml_unescape(X->src + X->start, X->end - X->start, &tx);
                /* ODF collapses runs of white space; the file writes extra spaces as text:s */
                size_t k = 0;
                for (size_t i = 0; i < tx.n; i++) {
                    char c = tx.s[i];
                    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                    if (c == ' ' && ((k > 0 && tx.s[k - 1] == ' ') || (!k && p->len && p->txt[p->len - 1] == ' '))) continue;
                    tx.s[k++] = c;
                }
                if (k) of_para_append(d, p, tx.s, k, cur);
                of_sb_free(&tx);
                continue;
            }
            if (t == OX_CLOSE) {
                if (of_xml_is(X, "text:span") && nn) {
                    nn--;
                    cur = span_style(R, names, nn, nl ? links[nl - 1] : -1);
                } else if (of_xml_is(X, "text:a") && nl) {
                    nl--;
                    cur = span_style(R, names, nn, nl ? links[nl - 1] : -1);
                }
                depth--;
                continue;
            }
            if (t != OX_OPEN && t != OX_EMPTY) continue;
            if (of_xml_is(X, "text:span")) {
                if (t == OX_EMPTY) continue;
                if (nn < 8) {
                    names[nn][0] = 0;
                    of_xml_attr(X, "text:style-name", names[nn], sizeof names[nn]);
                    nn++;
                }
                cur = span_style(R, names, nn, nl ? links[nl - 1] : -1);
                depth++;
                continue;
            }
            if (of_xml_is(X, "text:a")) {
                if (t == OX_EMPTY) continue;
                char **n = of_realloc(d->links, (size_t)(d->nlinks + 1) * sizeof *n);
                if (n) {
                    d->links = n;
                    d->links[d->nlinks] = of_strndup(X->src + X->start, X->end - X->start);
                    if (nl < 8) links[nl++] = d->nlinks;
                    d->nlinks++;
                }
                cur = span_style(R, names, nn, nl ? links[nl - 1] : -1);
                depth++;
                continue;
            }
            if (of_xml_is(X, "text:s")) {
                int c = of_xml_attr(X, "text:c", v, sizeof v) ? atoi(v) : 1;
                for (int k = 0; k < c && k < 200; k++) of_para_append(d, p, " ", 1, cur);
                if (t == OX_OPEN) of_xml_skip(X);
                continue;
            }
            if (of_xml_is(X, "text:tab")) { of_para_append(d, p, "\t", 1, cur); if (t == OX_OPEN) of_xml_skip(X); continue; }
            if (of_xml_is(X, "text:line-break")) { of_para_append(d, p, "\n", 1, cur); continue; }
            size_t a = X->start;
            if (of_xml_is(X, "text:change") || of_xml_is(X, "text:change-start") || of_xml_is(X, "text:change-end")) {
                p->locked = true;
                d->has_track = true;
                add_obj(R, p, OF_OBJ_HIDDEN, X->src, a, X->end, cur);
                continue;
            }
            if (t == OX_EMPTY) {         /* bookmarks, reference marks, soft page breaks */
                add_obj(R, p, OF_OBJ_HIDDEN, X->src, a, X->end, cur);
                continue;
            }
            /* an element with content: one object */
            int kind = OF_OBJ_HIDDEN;
            char *text = NULL, *target = NULL;
            int32_t w = 0, hh = 0;
            if (of_xml_is(X, "draw:frame") || of_xml_is(X, "draw:a")) {
                kind = OF_OBJ_IMAGE;
                if (of_xml_attr(X, "svg:width", v, sizeof v)) w = (int32_t)(size_half_pt(v, 24) / 2);
                if (of_xml_attr(X, "svg:height", v, sizeof v)) hh = (int32_t)(size_half_pt(v, 24) / 2);
                of_xml_t Z = *X;
                int dd = 1, tt;
                while (dd > 0 && (tt = of_xml_next(&Z)) != OX_END && tt != OX_ERROR) {
                    if (tt == OX_CLOSE) { dd--; continue; }
                    if ((tt == OX_OPEN || tt == OX_EMPTY) && of_xml_is(&Z, "draw:image") && !target &&
                        of_xml_attr(&Z, "xlink:href", v, sizeof v) && strncmp(v, "http", 4))
                        target = of_strdup(v[0] == '.' && v[1] == '/' ? v + 2 : v);
                    if ((tt == OX_OPEN || tt == OX_EMPTY) && of_xml_is(&Z, "svg:title")) {}
                    if (tt == OX_OPEN) dd++;
                }
                if (!target) kind = OF_OBJ_HIDDEN;
                of_xml_skip(X);
            } else if (of_xml_is(X, "text:note")) {
                kind = OF_OBJ_NOTE;
                char n[12];
                snprintf(n, sizeof n, "%d", ++R->footnote);
                text = of_strdup(n);
                of_xml_skip(X);
            } else if (of_xml_is(X, "office:annotation") || of_xml_is(X, "text:bookmark-start")) {
                of_xml_skip(X);
            } else if (of_xml_is(X, "text:ruby") || of_xml_is(X, "text:meta") || of_xml_is(X, "text:meta-field")) {
                p->locked = true;
                of_sb_t tx = { 0 };
                of_xml_text(X, &tx);
                kind = OF_OBJ_TEXT;
                text = of_sb_take(&tx);
            } else {
                /* fields and the like: they show their text */
                of_sb_t tx = { 0 };
                of_xml_text(X, &tx);
                kind = tx.n ? OF_OBJ_TEXT : OF_OBJ_HIDDEN;
                text = tx.n ? of_sb_take(&tx) : NULL;
                of_sb_free(&tx);
            }
            int o = of_doc_add_obj(d, kind, X->src + a, X->end - a);
            if (o >= 0) {
                d->obj[o].text = text;
                d->obj[o].target = target;
                d->obj[o].w = w ? w : 120;
                d->obj[o].h = hh ? hh : 90;
            } else {
                of_free(text);
                of_free(target);
            }
            of_para_append_obj(d, p, o, cur);
        }
    }
    p = &d->p[pi];
    if (p->locked) d->any_locked = true;
    if (!p->heading && p->pstyle >= 0)
        for (int s = p->pstyle, g = 0; s >= 0 && g < 8; s = d->ps[s].parent, g++)
            if (d->ps[s].heading) { p->heading = d->ps[s].heading; break; }
    for (int s = p->pstyle, g = 0; s >= 0 && g < 8; s = d->ps[s].parent, g++)
        if (d->ps[s].align) { p->align = d->ps[s].align; break; }
    size_t s1 = X->end;
    /* the only paragraph of its list item: the item is part of it, so that
     * a paragraph merged away takes its item along, and a split one makes
     * an item of its own */
    if (R->item_open && R->item_end == s0) {
        size_t k = s1;
        while (k < X->len && (X->src[k] == ' ' || X->src[k] == '\n' || X->src[k] == '\r' || X->src[k] == '\t')) k++;
        static const char CLOSE[] = "</text:list-item>";
        if (k + sizeof CLOSE - 1 <= X->len && !memcmp(X->src + k, CLOSE, sizeof CLOSE - 1)) {
            p->in_list = 2;
            p->raw_ppr = of_strndup(X->src + R->item_open, R->item_end - R->item_open);
            s0 = R->item_open;
            s1 = k + sizeof CLOSE - 1;
            X->pos = s1;            /* the item's end is consumed here */
            R->item_closed = true;
        }
    }
    R->item_open = 0;
    uint32_t *n0 = of_realloc(d->span0, (size_t)(d->norig + 1) * sizeof *n0);
    uint32_t *n1 = of_realloc(d->span1, (size_t)(d->norig + 1) * sizeof *n1);
    if (n0) d->span0 = n0;
    if (n1) d->span1 = n1;
    if (n0 && n1) {
        d->span0[d->norig] = (uint32_t)s0;
        d->span1[d->norig] = (uint32_t)s1;
        d->norig++;
    }
}

of_doc_t *of_odt_load(const char *path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *raw = of_file_read(path, &len, ODT_MAX);
    if (!raw) { snprintf(err, errlen, "read"); return NULL; }
    of_doc_t *d = of_doc_new(OF_DOC_ODT);
    if (!d) { of_free(raw); return NULL; }
    d->def_half_pt = 24;
    d->file = raw;
    d->file_len = len;
    bool flat = !strcmp(of_ext(path), "fodt");
    rd_t R = { .d = d };
    if (flat) {
        d->src = of_strndup((const char *)raw, len);
        d->src_len = len;
        read_styles(&R, d->src, d->src_len);
    } else {
        if (of_is_encrypted_ooxml(raw, len)) { snprintf(err, errlen, "encrypted"); of_doc_free(d); return NULL; }
        if (!of_zip_open(&d->zip, raw, len)) { snprintf(err, errlen, "zip"); of_doc_free(d); return NULL; }
        size_t ml;
        char *man = (char *)of_doc_part(d, "META-INF/manifest.xml", &ml);
        if (man && strstr(man, "encryption-data")) {
            of_free(man);
            snprintf(err, errlen, "encrypted");
            of_doc_free(d);
            return NULL;
        }
        of_free(man);
        size_t sl;
        char *styles = (char *)of_doc_part(d, "styles.xml", &sl);
        if (styles) read_styles(&R, styles, sl);
        of_free(styles);
        d->src = (char *)of_doc_part(d, "content.xml", &d->src_len);
        if (!d->src) { snprintf(err, errlen, "content"); of_doc_free(d); return NULL; }
        read_styles(&R, d->src, d->src_len);
    }
    d->part = of_strdup("content.xml");
    for (int i = 0; i < d->nps; i++) {
        d->ps[i].parent = R.pnames[i][0] ? ps_find(d, R.pnames[i]) : -1;
        if (d->ps[i].parent == i) d->ps[i].parent = -1;
    }
    /* the lists, as numbering for of_doc_number */
    for (int i = 0; i < R.nls; i++) {
        void *n = of_realloc(d->nums, (size_t)(d->nnums + 1) * sizeof *d->nums);
        if (!n) break;
        d->nums = n;
        d->nums[d->nnums].id = i;
        for (int k = 0; k < 9; k++) {
            d->nums[d->nnums].fmt[k] = (uint8_t)R.ls[i].fmt[k];
            snprintf(d->nums[d->nnums].bullet[k], 8, "%s", R.ls[i].bullet[k]);
        }
        d->nnums++;
    }
    of_xml_t X;
    of_xml_init(&X, d->src, d->src_len);
    int t, tbl = 0, lock = 0, cur_cell = 0, cur_table = -1, row = -1, col = 0;
    int list_lvl = -1, list_ix[16];
    bool item_numbered[16];
    bool in_text = false;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_OPEN && of_xml_is(&X, "office:text")) { in_text = true; continue; }
        if (!in_text) continue;
        if (t == OX_CLOSE && of_xml_is(&X, "office:text")) break;
        if ((t == OX_OPEN || t == OX_EMPTY) && (of_xml_is(&X, "text:p") || of_xml_is(&X, "text:h"))) {
            if (R.item_open && R.item_end != X.start) R.item_open = 0;
            int ix = list_lvl >= 0 ? list_ix[list_lvl > 15 ? 15 : list_lvl] : -1;
            bool num = list_lvl >= 0 && item_numbered[list_lvl > 15 ? 15 : list_lvl];
            parse_p(&R, &X, cur_cell, tbl > 1 || lock > 0, ix, list_lvl, num);
            /* only the first paragraph of an item carries its number */
            if (list_lvl >= 0) item_numbered[list_lvl > 15 ? 15 : list_lvl] = false;
            continue;
        }
        if (t == OX_OPEN && of_xml_is(&X, "text:list")) {
            list_lvl++;
            char v[96] = "";
            int ix = list_lvl > 0 ? list_ix[(list_lvl - 1) > 15 ? 15 : list_lvl - 1] : -1;
            if (of_xml_attr(&X, "text:style-name", v, sizeof v)) ix = list_index(&R, v);
            if (list_lvl < 16) list_ix[list_lvl] = ix;
            continue;
        }
        if (t == OX_CLOSE && of_xml_is(&X, "text:list")) { list_lvl--; continue; }
        if (t == OX_OPEN && of_xml_is(&X, "text:list-item") && list_lvl >= 0) {
            item_numbered[list_lvl > 15 ? 15 : list_lvl] = true;
            R.item_open = X.start;
            /* its first child starts right after, white space aside */
            size_t k = X.end;
            while (k < X.len && (X.src[k] == ' ' || X.src[k] == '\n' || X.src[k] == '\r' || X.src[k] == '\t')) k++;
            R.item_end = k;
            continue;
        }
        if (t == OX_OPEN && of_xml_is(&X, "text:list-header") && list_lvl >= 0) { item_numbered[list_lvl > 15 ? 15 : list_lvl] = false; continue; }
        if (t == OX_OPEN && of_xml_is(&X, "table:table")) {
            tbl++;
            if (tbl == 1) {
                of_table_t *n = of_realloc(d->tables, (size_t)(d->ntables + 1) * sizeof *n);
                if (n) {
                    d->tables = n;
                    memset(&d->tables[d->ntables], 0, sizeof *n);
                    cur_table = d->ntables++;
                }
                row = -1;
            } else {
                d->any_locked = true;
            }
            continue;
        }
        if (t == OX_CLOSE && of_xml_is(&X, "table:table")) { tbl--; if (!tbl) cur_cell = 0; continue; }
        if (tbl == 1 && cur_table >= 0) {
            of_table_t *T = &d->tables[cur_table];
            char v[16];
            if ((t == OX_EMPTY || t == OX_OPEN) && of_xml_is(&X, "table:table-column")) {
                int rep = of_xml_attr(&X, "table:number-columns-repeated", v, sizeof v) ? atoi(v) : 1;
                for (int k = 0; k < rep && T->ncols < 64; k++) T->colw[T->ncols++] = 0;
                continue;
            }
            if (t == OX_OPEN && of_xml_is(&X, "table:table-row")) { row++; col = 0; T->nrows = row + 1; continue; }
            if ((t == OX_OPEN || t == OX_EMPTY) && of_xml_is(&X, "table:table-cell")) {
                int span = of_xml_attr(&X, "table:number-columns-spanned", v, sizeof v) ? atoi(v) : 1;
                of_cell_t2 *n = of_realloc(d->cells, (size_t)(d->ncells + 1) * sizeof *n);
                if (n) {
                    d->cells = n;
                    d->cells[d->ncells] = (of_cell_t2){ cur_table, row, col, span > 0 ? span : 1, false };
                    cur_cell = ++d->ncells;
                }
                col += span > 0 ? span : 1;
                if (t == OX_EMPTY) {
                    /* an empty cell has no paragraph: nothing to edit in it */
                    cur_cell = 0;
                }
                continue;
            }
            if ((t == OX_OPEN || t == OX_EMPTY) && of_xml_is(&X, "table:covered-table-cell")) {
                col++;
                if (t == OX_OPEN) of_xml_skip(&X);
                continue;
            }
            if (t == OX_CLOSE && of_xml_is(&X, "table:table-cell")) { cur_cell = 0; continue; }
        }
        if (t == OX_OPEN && (of_xml_is(&X, "text:table-of-content") || of_xml_is(&X, "text:alphabetical-index") ||
                             of_xml_is(&X, "text:illustration-index") || of_xml_is(&X, "text:bibliography") ||
                             of_xml_is(&X, "text:user-index") || of_xml_is(&X, "text:object-index") ||
                             of_xml_is(&X, "text:table-index") || of_xml_is(&X, "text:tracked-changes"))) {
            if (of_xml_is(&X, "text:tracked-changes")) { d->has_track = true; of_xml_skip(&X); continue; }
            lock++;
            d->any_locked = true;
            continue;
        }
        if (t == OX_CLOSE && (of_xml_is(&X, "text:table-of-content") || of_xml_is(&X, "text:alphabetical-index") ||
                              of_xml_is(&X, "text:illustration-index") || of_xml_is(&X, "text:bibliography") ||
                              of_xml_is(&X, "text:user-index") || of_xml_is(&X, "text:object-index") ||
                              of_xml_is(&X, "text:table-index"))) {
            lock--;
            continue;
        }
        if (t == OX_OPEN && (of_xml_is(&X, "text:sequence-decls") || of_xml_is(&X, "text:variable-decls") ||
                             of_xml_is(&X, "office:forms"))) {
            of_xml_skip(&X);
            continue;
        }
    }
    for (int i = 0; i < d->nps; i++) of_free(R.pnames ? R.pnames[i] : NULL);
    of_free(R.pnames);
    for (int i = 0; i < R.nls; i++) of_free(R.ls[i].name);
    of_free(R.ls);
    if (!d->np) of_doc_insert_para(d, 0);
    for (int i = 0; i < d->np; i++) d->p[i].dirty = false;
    return d;
}

/* -------------------------------------------------------------------------- */
/* Writing                                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    of_sb_t styles;             /* automatic styles to add */
    int     n;
    uint16_t keys[256];         /* run style -> its added style number */
    int      nkeys;
} wr_t;

static int added_style(wr_t *W, const of_doc_t *d, uint16_t si)
{
    for (int i = 0; i < W->nkeys; i++)
        if (W->keys[i] == si) return i + 1;
    if (W->nkeys >= 256) return 0;
    const of_rstyle_t *s = &d->rs[si];
    W->keys[W->nkeys++] = si;
    of_sb_printf(&W->styles, "<style:style style:name=\"OF_T%d\" style:family=\"text\"><style:text-properties", W->nkeys);
    if (s->mask & OF_RS_B)
        of_sb_printf(&W->styles, " fo:font-weight=\"%s\" style:font-weight-asian=\"%s\" style:font-weight-complex=\"%s\"",
                     s->b == OF_TRI_ON ? "bold" : "normal", s->b == OF_TRI_ON ? "bold" : "normal", s->b == OF_TRI_ON ? "bold" : "normal");
    if (s->mask & OF_RS_I)
        of_sb_printf(&W->styles, " fo:font-style=\"%s\" style:font-style-asian=\"%s\" style:font-style-complex=\"%s\"",
                     s->i == OF_TRI_ON ? "italic" : "normal", s->i == OF_TRI_ON ? "italic" : "normal", s->i == OF_TRI_ON ? "italic" : "normal");
    if (s->mask & OF_RS_U)
        of_sb_printf(&W->styles, s->u == OF_TRI_ON ? " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" "
                                                     "style:text-underline-color=\"font-color\""
                                                   : " style:text-underline-style=\"none\"");
    if (s->mask & OF_RS_S)
        of_sb_printf(&W->styles, " style:text-line-through-style=\"%s\"", s->s == OF_TRI_ON ? "solid" : "none");
    if ((s->mask & OF_RS_COLOR) && s->color) of_sb_printf(&W->styles, " fo:color=\"#%06lx\"", (unsigned long)(s->color & 0xFFFFFF));
    if ((s->mask & OF_RS_SIZE) && s->half_pt) of_sb_printf(&W->styles, " fo:font-size=\"%.1fpt\"", s->half_pt / 2.0);
    if ((s->mask & OF_RS_HL) && s->hl) of_sb_printf(&W->styles, " fo:background-color=\"#%06lx\"", (unsigned long)(s->hl & 0xFFFFFF));
    of_sb_puts(&W->styles, "/></style:style>");
    return W->nkeys;
}

static void w_text(of_sb_t *o, const char *s, int n, bool at_start)
{
    int spaces = 0;
    for (int i = 0; i <= n; i++) {
        char c = i < n ? s[i] : 0;
        if (c == ' ' && i < n) { spaces++; continue; }
        if (spaces) {
            /* one space as text where ODF keeps it; the others as text:s */
            bool lead = at_start && i - spaces == 0;
            if (!lead) { of_sb_putc(o, ' '); spaces--; }
            if (spaces == 1) of_sb_puts(o, "<text:s/>");
            else if (spaces > 1) of_sb_printf(o, "<text:s text:c=\"%d\"/>", spaces);
            spaces = 0;
        }
        if (i >= n) break;
        if (c == '\t') of_sb_puts(o, "<text:tab/>");
        else if (c == '\n') of_sb_puts(o, "<text:line-break/>");
        else if (c == '<') of_sb_puts(o, "&lt;");
        else if (c == '>') of_sb_puts(o, "&gt;");
        else if (c == '&') of_sb_puts(o, "&amp;");
        else of_sb_putc(o, c);
    }
}

static const char *heading_style(const of_doc_t *d, int level)
{
    for (int i = 0; i < d->nps; i++)
        if (d->ps[i].heading == level && d->ps[i].id && !strncmp(d->ps[i].id, "Heading", 7)) return d->ps[i].id;
    for (int i = 0; i < d->nps; i++)
        if (d->ps[i].heading == level && d->ps[i].id) return d->ps[i].id;
    return NULL;
}

static void w_para(of_sb_t *o, const of_doc_t *d, const of_para_t *p, wr_t *W)
{
    const char *tag = p->heading ? "text:h" : "text:p";
    if (p->pmask || !p->raw_open) {
        of_sb_printf(o, "<%s", tag);
        const char *sn = NULL;
        if (p->heading) sn = heading_style(d, p->heading);
        else if (p->pstyle >= 0 && p->pstyle < d->nps && !d->ps[p->pstyle].heading) sn = d->ps[p->pstyle].id;
        else if (d->ps_normal >= 0) sn = d->ps[d->ps_normal].id;
        if (sn) {
            of_sb_puts(o, " text:style-name=\"");
            of_sb_xml(o, sn, strlen(sn));
            of_sb_putc(o, '"');
        }
        if (p->heading) of_sb_printf(o, " text:outline-level=\"%d\"", p->heading);
        of_sb_putc(o, '>');
    } else {
        /* the original start tag; <text:p/> opened */
        size_t l = strlen(p->raw_open);
        if (l >= 2 && p->raw_open[l - 2] == '/') {
            of_sb_put(o, p->raw_open, l - 2);
            of_sb_putc(o, '>');
        } else {
            of_sb_puts(o, p->raw_open);
        }
        tag = !strncmp(p->raw_open, "<text:h", 7) ? "text:h" : "text:p";
    }
    int link = -1, k = 0;
    int i = 0;
    while (i < p->len) {
        uint16_t st = p->at[i];
        bool obj = i + 2 < p->len && !memcmp(p->txt + i, OF_OBJ_CHAR, 3);
        int j = i;
        if (obj) j = i + 3;
        else while (j < p->len && p->at[j] == st && !(j + 2 < p->len && !memcmp(p->txt + j, OF_OBJ_CHAR, 3))) j++;
        const of_rstyle_t *rs = &d->rs[st];
        if (rs->link != link) {
            if (link >= 0) of_sb_puts(o, "</text:a>");
            link = rs->link >= 0 && rs->link < d->nlinks ? rs->link : -1;
            if (link >= 0) of_sb_puts(o, d->links[link]);
        }
        /* the original spans, then the change */
        int opened = 0;
        if (rs->raw) {
            const char *a = rs->raw;
            while (*a) {
                const char *z = strchr(a, '\x1f');
                size_t l = z ? (size_t)(z - a) : strlen(a);
                of_sb_puts(o, "<text:span text:style-name=\"");
                of_sb_xml(o, a, l);
                of_sb_puts(o, "\">");
                opened++;
                a += l;
                if (*a) a++;
            }
        }
        if (rs->mask && !obj) {
            int n = added_style(W, d, st);
            if (n) { of_sb_printf(o, "<text:span text:style-name=\"OF_T%d\">", n); opened++; }
        }
        if (obj) {
            int ob = k < p->nobj ? p->obj[k] : -1;
            k++;
            if (ob >= 0 && ob < d->nobj && d->obj[ob].raw) of_sb_puts(o, d->obj[ob].raw);
        } else {
            w_text(o, p->txt + i, j - i, i == 0);
        }
        for (int q = 0; q < opened; q++) of_sb_puts(o, "</text:span>");
        i = j;
    }
    if (link >= 0) of_sb_puts(o, "</text:a>");
    of_sb_printf(o, "</%s>", tag);
}

static char *content_xml(const of_doc_t *d)
{
    wr_t W;
    memset(&W, 0, sizeof W);
    of_sb_t body = { 0 };
    size_t pos = 0;
    for (int i = 0; i < d->norig; i++) {
        of_sb_put(&body, d->src + pos, d->span0[i] - pos);
        for (int k = 0; k < d->np; k++) {
            const of_para_t *p = &d->p[k];
            if (p->slot != i) continue;
            if (p->orig == i && (!p->dirty || p->locked)) {
                of_sb_put(&body, d->src + d->span0[i], d->span1[i] - d->span0[i]);
                continue;
            }
            if (p->in_list == 2) {
                /* a whole item: its own start tag, or a plain one for a new item */
                of_sb_puts(&body, p->orig >= 0 && p->raw_ppr ? p->raw_ppr : "<text:list-item>");
                w_para(&body, d, p, &W);
                of_sb_puts(&body, "</text:list-item>");
                continue;
            }
            /* a new paragraph after a list item's: an item of its own */
            if (p->orig < 0 && p->in_list) of_sb_puts(&body, "</text:list-item><text:list-item>");
            w_para(&body, d, p, &W);
        }
        pos = d->span1[i];
    }
    of_sb_put(&body, d->src + pos, d->src_len - pos);
    if (body.oom) { of_sb_free(&body); of_sb_free(&W.styles); return NULL; }
    if (!W.styles.n) { of_sb_free(&W.styles); return of_sb_take(&body); }
    /* the new automatic styles, at the end of the automatic ones */
    of_sb_t o = { 0 };
    const char *close = strstr(body.s, "</office:automatic-styles>");
    const char *empty = strstr(body.s, "<office:automatic-styles/>");
    if (close) {
        of_sb_put(&o, body.s, (size_t)(close - body.s));
        of_sb_put(&o, W.styles.s, W.styles.n);
        of_sb_puts(&o, close);
    } else if (empty) {
        of_sb_put(&o, body.s, (size_t)(empty - body.s));
        of_sb_puts(&o, "<office:automatic-styles>");
        of_sb_put(&o, W.styles.s, W.styles.n);
        of_sb_puts(&o, "</office:automatic-styles>");
        of_sb_puts(&o, empty + strlen("<office:automatic-styles/>"));
    } else {
        /* none: before the body */
        const char *b = strstr(body.s, "<office:body");
        if (!b) { of_sb_free(&o); of_sb_free(&W.styles); return of_sb_take(&body); }
        of_sb_put(&o, body.s, (size_t)(b - body.s));
        of_sb_puts(&o, "<office:automatic-styles>");
        of_sb_put(&o, W.styles.s, W.styles.n);
        of_sb_puts(&o, "</office:automatic-styles>");
        of_sb_puts(&o, b);
    }
    of_sb_free(&body);
    of_sb_free(&W.styles);
    if (o.oom) { of_sb_free(&o); return NULL; }
    return of_sb_take(&o);
}

static bool save_keeping(of_doc_t *d, const char *path, char *err, size_t errlen)
{
    char *content = content_xml(d);
    if (!content) { snprintf(err, errlen, "memory"); return false; }
    bool ok;
    if (!d->zip.n) {
        /* a flat .fodt is the whole file */
        ok = of_file_write(path, content, strlen(content));
    } else {
        of_zipw_t w;
        of_zipw_init(&w);
        ok = true;
        for (int i = 0; i < d->zip.n && ok; i++) {
            if (!strcmp(d->zip.e[i].name, "content.xml")) ok = of_zipw_add(&w, "content.xml", content, strlen(content), true);
            else ok = of_zipw_copy(&w, &d->zip, i);
        }
        ok = ok && of_zipw_finish(&w, path);
        of_zipw_free(&w);
    }
    of_free(content);
    if (!ok) snprintf(err, errlen, "write");
    return ok;
}

static const char NS[] =
    "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
    "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
    "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
    "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
    "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" "
    "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
    "xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
    "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" office:version=\"1.3\"";

static bool save_new(of_doc_t *d, const char *path, char *err, size_t errlen)
{
    of_zipw_t w;
    of_zipw_init(&w);
    of_zipw_add(&w, "mimetype", "application/vnd.oasis.opendocument.text", 39, false);
    of_sb_t o = { 0 };
    /* the manifest, finished after the body: its pictures */
    of_sb_t man = { 0 };
    of_sb_puts(&man, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">"
                     "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.3\" manifest:media-type=\"application/vnd.oasis.opendocument.text\"/>"
                     "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
                     "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>");
    int npics = 0;
    of_sb_printf(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles %s><office:styles>"
                     "<style:default-style style:family=\"paragraph\"><style:text-properties fo:font-size=\"12pt\"/></style:default-style>"
                     "<style:style style:name=\"Standard\" style:family=\"paragraph\"/>", NS);
    static const char *const HS[4] = { "", "20pt", "16pt", "14pt" };
    for (int h = 1; h <= 3; h++)
        of_sb_printf(&o, "<style:style style:name=\"Heading_20_%d\" style:display-name=\"Heading %d\" style:family=\"paragraph\" "
                         "style:parent-style-name=\"Standard\" style:default-outline-level=\"%d\"><style:paragraph-properties "
                         "fo:margin-top=\"0.4cm\" fo:margin-bottom=\"0.2cm\"/><style:text-properties fo:font-size=\"%s\" "
                         "fo:font-weight=\"bold\"/></style:style>", h, h, h, HS[h]);
    of_sb_puts(&o, "<text:list-style style:name=\"L1\"><text:list-level-style-bullet text:level=\"1\" text:bullet-char=\"\xE2\x80\xA2\">"
                   "<style:list-level-properties text:space-before=\"0.4cm\" text:min-label-width=\"0.6cm\"/></text:list-level-style-bullet></text:list-style>"
                   "<text:list-style style:name=\"L2\"><text:list-level-style-number text:level=\"1\" style:num-format=\"1\" style:num-suffix=\".\">"
                   "<style:list-level-properties text:space-before=\"0.4cm\" text:min-label-width=\"0.6cm\"/></text:list-level-style-number></text:list-style>"
                   "</office:styles></office:document-styles>");
    of_zipw_add(&w, "styles.xml", o.s, o.n, true);
    o.n = 0;
    /* automatic styles for the run looks in use */
    of_sb_printf(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content %s><office:automatic-styles>", NS);
    static const char *const AL[4] = { "start", "center", "end", "justify" };
    for (int a = 1; a < 4; a++)
        of_sb_printf(&o, "<style:style style:name=\"P%d\" style:family=\"paragraph\" style:parent-style-name=\"Standard\">"
                         "<style:paragraph-properties fo:text-align=\"%s\"/></style:style>", a, AL[a]);
    /* indents (quotes, trees): PI<n> */
    for (int k = 1; k <= 12; k++)
        of_sb_printf(&o, "<style:style style:name=\"PI%d\" style:family=\"paragraph\" style:parent-style-name=\"Standard\">"
                         "<style:paragraph-properties fo:margin-left=\"%.1fcm\"/></style:style>", k, k * 1.0);
    of_sb_puts(&o, "<style:style style:name=\"PB\" style:family=\"paragraph\" style:parent-style-name=\"Standard\">"
                   "<style:paragraph-properties fo:break-before=\"page\"/></style:style>");
    for (int s = 1; s < d->nrs; s++) {
        const of_rstyle_t *r = &d->rs[s];
        of_sb_printf(&o, "<style:style style:name=\"T%d\" style:family=\"text\"><style:text-properties", s);
        if (r->b) of_sb_printf(&o, " fo:font-weight=\"%s\"", r->b == OF_TRI_ON ? "bold" : "normal");
        if (r->i) of_sb_printf(&o, " fo:font-style=\"%s\"", r->i == OF_TRI_ON ? "italic" : "normal");
        if (r->u == OF_TRI_ON) of_sb_puts(&o, " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"");
        if (r->s == OF_TRI_ON) of_sb_puts(&o, " style:text-line-through-style=\"solid\"");
        if (r->color) of_sb_printf(&o, " fo:color=\"#%06lx\"", (unsigned long)(r->color & 0xFFFFFF));
        if (r->half_pt) of_sb_printf(&o, " fo:font-size=\"%.1fpt\"", r->half_pt / 2.0);
        if (r->hl) of_sb_printf(&o, " fo:background-color=\"#%06lx\"", (unsigned long)(r->hl & 0xFFFFFF));
        if (r->sup) of_sb_puts(&o, r->sup == 1 ? " style:text-position=\"super 58%\"" : " style:text-position=\"sub 58%\"");
        of_sb_puts(&o, "/></style:style>");
    }
    of_sb_puts(&o, "</office:automatic-styles><office:body><office:text>");
    int cur_table = -1, cur_row = -1, cur_cell = 0, cur_list = 0;
    for (int i = 0; i < d->np; i++) {
        const of_para_t *p = &d->p[i];
        const of_cell_t2 *c = p->cell ? &d->cells[p->cell - 1] : NULL;
        int list = p->cell ? 0 : p->list;
        if (cur_list && list != cur_list) { of_sb_puts(&o, "</text:list>"); cur_list = 0; }
        if (cur_cell && (!c || p->cell != cur_cell)) { of_sb_puts(&o, "</table:table-cell>"); cur_cell = 0; }
        if (cur_row >= 0 && (!c || c->table != cur_table || c->row != cur_row)) { of_sb_puts(&o, "</table:table-row>"); cur_row = -1; }
        if (cur_table >= 0 && (!c || c->table != cur_table)) { of_sb_puts(&o, "</table:table>"); cur_table = -1; }
        if (c && cur_table != c->table) {
            cur_table = c->table;
            of_sb_printf(&o, "<table:table table:name=\"Tabla%d\">", c->table + 1);
            for (int k = 0; k < d->tables[c->table].ncols; k++) of_sb_puts(&o, "<table:table-column/>");
        }
        if (c && cur_row != c->row) { cur_row = c->row; of_sb_puts(&o, "<table:table-row>"); }
        if (c && cur_cell != p->cell) {
            cur_cell = p->cell;
            if (c->span > 1) of_sb_printf(&o, "<table:table-cell table:number-columns-spanned=\"%d\" office:value-type=\"string\">", c->span);
            else of_sb_puts(&o, "<table:table-cell office:value-type=\"string\">");
        }
        if (list && !cur_list) { of_sb_printf(&o, "<text:list text:style-name=\"%s\">", list == OF_LIST_BULLET ? "L1" : "L2"); cur_list = list; }
        if (list) of_sb_puts(&o, "<text:list-item>");
        const char *tag = p->heading ? "text:h" : "text:p";
        if (p->heading) of_sb_printf(&o, "<text:h text:style-name=\"Heading_20_%d\" text:outline-level=\"%d\">", p->heading > 3 ? 3 : p->heading, p->heading);
        else if (p->align) of_sb_printf(&o, "<text:p text:style-name=\"P%d\">", p->align & 3);
        else if (p->indent) of_sb_printf(&o, "<text:p text:style-name=\"PI%d\">", p->indent > 12 ? 12 : p->indent);
        else if (p->len == 3 && p->nobj == 1 && d->obj[p->obj[0]].kind == OF_OBJ_BREAK) of_sb_puts(&o, "<text:p text:style-name=\"PB\">");
        else of_sb_puts(&o, "<text:p text:style-name=\"Standard\">");
        int k = 0, link = -1;
        for (int a = 0; a < p->len;) {
            int lk = d->rs[p->at[a]].link;
            if (lk >= 0 && (lk >= d->nlinks || !of_link_is_url(d->links[lk]))) lk = -1;
            if (lk != link) {
                if (link >= 0) of_sb_puts(&o, "</text:a>");
                link = lk;
                if (link >= 0) {
                    of_sb_puts(&o, "<text:a xlink:type=\"simple\" xlink:href=\"");
                    of_sb_xml(&o, d->links[link], strlen(d->links[link]));
                    of_sb_puts(&o, "\">");
                }
            }
            if (a + 2 < p->len && !memcmp(p->txt + a, OF_OBJ_CHAR, 3)) {
                int ob = k < p->nobj ? p->obj[k] : -1;
                k++;
                of_obj_t *obj = ob >= 0 && ob < d->nobj ? &d->obj[ob] : NULL;
                if (obj && (obj->kind == OF_OBJ_TEXT || obj->kind == OF_OBJ_NOTE) && obj->text) {
                    w_text(&o, obj->text, (int)strlen(obj->text), false);
                } else if (obj && obj->kind == OF_OBJ_IMAGE) {
                    size_t n;
                    const char *ext = "png";
                    uint8_t *bin = of_doc_obj_bytes(d, obj, &n, &ext);
                    if (bin) {
                        npics++;
                        char name[48];
                        snprintf(name, sizeof name, "Pictures/image%d.%s", npics, ext);
                        of_zipw_add(&w, name, bin, n, false);
                        of_free(bin);
                        of_sb_printf(&man, "<manifest:file-entry manifest:full-path=\"%s\" manifest:media-type=\"image/%s\"/>", name, ext);
                        of_sb_printf(&o, "<draw:frame draw:name=\"Imagen%d\" text:anchor-type=\"as-char\" svg:width=\"%ldpt\" svg:height=\"%ldpt\">"
                                         "<draw:image xlink:href=\"%s\" xlink:type=\"simple\" xlink:show=\"embed\" xlink:actuate=\"onLoad\"/></draw:frame>",
                                     npics, (long)(obj->w > 0 ? obj->w : 200), (long)(obj->h > 0 ? obj->h : 150), name);
                    }
                }
                a += 3;
                continue;
            }
            int b = a;
            while (b < p->len && p->at[b] == p->at[a] && !(b + 2 < p->len && !memcmp(p->txt + b, OF_OBJ_CHAR, 3))) b++;
            if (p->at[a]) of_sb_printf(&o, "<text:span text:style-name=\"T%d\">", p->at[a]);
            w_text(&o, p->txt + a, b - a, a == 0);
            if (p->at[a]) of_sb_puts(&o, "</text:span>");
            a = b;
        }
        if (link >= 0) of_sb_puts(&o, "</text:a>");
        of_sb_printf(&o, "</%s>", tag);
        if (list) of_sb_puts(&o, "</text:list-item>");
    }
    if (cur_list) of_sb_puts(&o, "</text:list>");
    if (cur_cell) of_sb_puts(&o, "</table:table-cell>");
    if (cur_row >= 0) of_sb_puts(&o, "</table:table-row>");
    if (cur_table >= 0) of_sb_puts(&o, "</table:table>");
    of_sb_puts(&o, "</office:text></office:body></office:document-content>");
    of_zipw_add(&w, "content.xml", o.s, o.n, true);
    of_sb_puts(&man, "</manifest:manifest>");
    of_zipw_add(&w, "META-INF/manifest.xml", man.s, man.n, true);
    bool ok = !o.oom && !man.oom && of_zipw_finish(&w, path);
    of_sb_free(&man);
    of_sb_free(&o);
    of_zipw_free(&w);
    if (!ok) snprintf(err, errlen, "write");
    return ok;
}

bool of_odt_save(of_doc_t *d, const char *path, char *err, size_t errlen)
{
    err[0] = 0;
    if (d->fmt == OF_DOC_ODT && d->src && d->span0) return save_keeping(d, path, err, errlen);
    return save_new(d, path, err, errlen);
}
