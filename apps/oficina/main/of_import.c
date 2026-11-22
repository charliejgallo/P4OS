/*
 * P4OS - Office: documents read from other formats (phase 3).
 *
 * Each reader builds an of_doc_t through the builder below, marked read
 * only: the document screen shows it, finds in it, copies from it and saves
 * a copy as docx, odt or txt. Here: HTML (reader mode: headings,
 * paragraphs, lists, tables, emphasis, links, pictures; no CSS layout),
 * EPUB (its spine, chapter after chapter, through the HTML reader), mail
 * (.eml: the headers, the HTML or plain body, the attachments listed), FB2,
 * a picture alone, and the preview picture that Apple's Pages, Numbers and
 * Keynote files carry. RTF, Markdown, contacts, calendars and the data tree
 * are in of_rtf.c and of_text2.c.
 */
#include "of_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define READ_MAX (48u * 1024 * 1024)

const char *(*of_tr)(const char *es);           /* the app's _(); NULL: Spanish */
#define _(s) (of_tr ? of_tr(s) : (s))
#define N_(s) (s)               /* marked for the catalog, translated where shown */

/* -------------------------------------------------------------------------- */
/* The builder                                                                 */
/* -------------------------------------------------------------------------- */

void of_bld_init(of_bld_t *b, of_doc_t *d)
{
    memset(b, 0, sizeof *b);
    b->d = d;
    b->cur = -1;
    b->st.link = -1;
    b->numid = 1000;
    d->readonly = true;
}

static of_para_t *cur(of_bld_t *b)
{
    if (b->cur < 0) of_bld_para(b);
    return b->cur >= 0 ? &b->d->p[b->cur] : NULL;
}

void of_bld_para(of_bld_t *b)
{
    of_doc_t *d = b->d;
    if (b->cur >= 0) {
        of_para_t *p = &d->p[b->cur];
        /* an empty one is reused, with what is asked now */
        if (!p->len && p->cell == b->cell) {
            p->heading = (uint8_t)b->heading;
            p->list = (uint8_t)b->list;
            p->ilvl = (uint8_t)b->ilvl;
            p->numid = b->numid;
            p->align = (uint8_t)b->align;
            p->indent = (uint8_t)b->indent;
            b->space = false;
            return;
        }
        /* trailing white space goes */
        while (p->len && (p->txt[p->len - 1] == ' ')) p->txt[--p->len] = 0;
    }
    of_para_t *p = of_doc_insert_para(d, d->np);
    if (!p) return;
    b->cur = d->np - 1;
    p->heading = (uint8_t)b->heading;
    p->list = (uint8_t)b->list;
    p->ilvl = (uint8_t)b->ilvl;
    p->numid = b->numid;
    p->align = (uint8_t)b->align;
    p->indent = (uint8_t)b->indent;
    p->cell = b->cell;
    b->space = false;
}

void of_bld_text(of_bld_t *b, const char *s, size_t n)
{
    of_para_t *p = cur(b);
    if (!p) return;
    uint16_t st = (uint16_t)of_doc_style(b->d, &b->st);
    if (b->pre) {
        size_t from = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i < n && s[i] != '\n' && s[i] != '\r') continue;
            if (i > from) of_para_append(b->d, &b->d->p[b->cur], s + from, i - from, st);
            if (i < n && s[i] == '\n') {
                /* a line of preformatted text is a line of its own */
                of_para_append(b->d, &b->d->p[b->cur], "\n", 1, st);
            }
            from = i + 1;
        }
        return;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f') {
            b->space = true;
            continue;
        }
        p = &b->d->p[b->cur];
        /* the pending space takes the style of what it follows (not a link's underline) */
        if (b->space && p->len && p->txt[p->len - 1] != ' ' && p->txt[p->len - 1] != '\n') {
            uint16_t prev = p->at[p->len - 1];
            of_para_append(b->d, p, " ", 1, b->st.link >= 0 && b->d->rs[prev].link != b->st.link ? prev : st);
        }
        b->space = false;
        /* the run of letters up to the next space */
        size_t j = i;
        while (j < n && s[j] != ' ' && s[j] != '\n' && s[j] != '\r' && s[j] != '\t' && s[j] != '\f') j++;
        of_para_append(b->d, p, s + i, j - i, st);
        i = j - 1;
    }
}

void of_bld_link(of_bld_t *b, const char *url)
{
    if (!url) { b->st.link = -1; return; }
    of_doc_t *d = b->d;
    char **n = of_realloc(d->links, (size_t)(d->nlinks + 1) * sizeof *n);
    if (!n) return;
    d->links = n;
    d->links[d->nlinks] = of_strdup(url);
    b->st.link = (int16_t)d->nlinks++;
}

void of_bld_image(of_bld_t *b, const char *target, bool file, int w_pt, int h_pt)
{
    of_para_t *p = cur(b);
    if (!p) return;
    int o = of_doc_add_obj(b->d, OF_OBJ_IMAGE, "", 0);
    if (o < 0) return;
    of_obj_t *ob = &b->d->obj[o];
    ob->target = of_strdup(target);
    ob->file = file;
    ob->w = w_pt > 0 ? w_pt : 240;
    ob->h = h_pt > 0 ? h_pt : 160;
    if (b->space && p->len) { of_para_append(b->d, p, " ", 1, 0); b->space = false; }
    of_para_append_obj(b->d, &b->d->p[b->cur], o, 0);
}

void of_bld_break(of_bld_t *b)
{
    of_bld_para(b);
    int o = of_doc_add_obj(b->d, OF_OBJ_BREAK, "", 0);
    of_para_append_obj(b->d, &b->d->p[b->cur], o, 0);
    b->cur = -1;            /* what follows starts a paragraph of its own */
}

void of_bld_table(of_bld_t *b)
{
    of_doc_t *d = b->d;
    /* the empty paragraph waiting before it goes */
    if (b->cur >= 0 && b->cur == d->np - 1 && !d->p[b->cur].len && !d->p[b->cur].cell && d->np > 1) {
        of_doc_remove_para(d, b->cur);
        b->cur = -1;
    }
    of_table_t *n = of_realloc(d->tables, (size_t)(d->ntables + 1) * sizeof *n);
    if (!n) return;
    d->tables = n;
    memset(&d->tables[d->ntables], 0, sizeof *n);
    b->table = d->ntables++;
    b->row = -1;
    b->cell = 0;
}

void of_bld_row(of_bld_t *b)
{
    if (!b->d->ntables) of_bld_table(b);
    b->row++;
    b->col = 0;
    b->d->tables[b->table].nrows = b->row + 1;
    b->cell = 0;
}

void of_bld_cell(of_bld_t *b, int span)
{
    of_doc_t *d = b->d;
    if (b->row < 0) of_bld_row(b);
    if (span < 1) span = 1;
    of_cell_t2 *n = of_realloc(d->cells, (size_t)(d->ncells + 1) * sizeof *n);
    if (!n) return;
    d->cells = n;
    d->cells[d->ncells] = (of_cell_t2){ b->table, b->row, b->col, span, false };
    b->cell = ++d->ncells;
    of_table_t *T = &d->tables[b->table];
    b->col += span;
    while (T->ncols < b->col && T->ncols < 64) T->colw[T->ncols++] = 0;
    b->cur = -1;
    of_bld_para(b);
}

void of_bld_table_end(of_bld_t *b)
{
    b->cell = 0;
    b->cur = -1;
    b->row = -1;
}

void of_bld_done(of_bld_t *b)
{
    of_doc_t *d = b->d;
    /* empty trailing paragraphs go, but one stays */
    while (d->np > 1 && !d->p[d->np - 1].len && !d->p[d->np - 1].cell) of_doc_remove_para(d, d->np - 1);
    if (!d->np) of_doc_insert_para(d, 0);
    /* a table whose cell got no paragraph would confuse the layout: every cell has one */
    for (int i = 0; i < d->np; i++) d->p[i].dirty = false;
    of_doc_number(d);
}

bool of_cache_file(const char *name, const void *data, size_t n, char *out, size_t len)
{
    extern const char *aos_hal_path_data(void);
    char dir[160];
#ifdef OF_HOST
    snprintf(dir, sizeof dir, "/tmp/of_cache");
#else
    snprintf(dir, sizeof dir, "%s/oficina/cache", aos_hal_path_data());
#endif
    mkdir(dir, 0777);
    snprintf(out, len, "%.150s/%.60s", dir, name);
    return of_file_write(out, data, n);
}

/* -------------------------------------------------------------------------- */
/* Pictures: their size from their header                                      */
/* -------------------------------------------------------------------------- */

static bool pic_size(const uint8_t *b, size_t n, int *w, int *h)
{
    if (n > 24 && !memcmp(b, "\x89PNG", 4)) {
        *w = b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19];
        *h = b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23];
        return true;
    }
    if (n > 10 && !memcmp(b, "GIF8", 4)) { *w = b[6] | b[7] << 8; *h = b[8] | b[9] << 8; return true; }
    if (n > 26 && b[0] == 'B' && b[1] == 'M') {
        *w = (int)(b[18] | b[19] << 8 | b[20] << 16 | (uint32_t)b[21] << 24);
        *h = abs((int)(b[22] | b[23] << 8 | b[24] << 16 | (uint32_t)b[25] << 24));
        return true;
    }
    if (n > 4 && b[0] == 0xFF && b[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < n) {
            if (b[i] != 0xFF) { i++; continue; }
            uint8_t m = b[i + 1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            size_t l = (size_t)(b[i + 2] << 8 | b[i + 3]);
            if ((m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) || (m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF)) {
                *h = b[i + 5] << 8 | b[i + 6];
                *w = b[i + 7] << 8 | b[i + 8];
                return true;
            }
            i += 2 + l;
        }
    }
    return false;
}

/* pixels to points: a picture of 96 dpi is three quarters of its pixels */
static void pic_pt(int w, int h, int *wp, int *hp)
{
    *wp = w * 3 / 4;
    *hp = h * 3 / 4;
    if (*wp > 500) { *hp = *hp * 500 / *wp; *wp = 500; }
}

/* -------------------------------------------------------------------------- */
/* HTML                                                                        */
/* -------------------------------------------------------------------------- */

static const struct { const char *n; uint32_t cp; } ENT[] = {
    { "nbsp", 0xA0 }, { "iexcl", 0xA1 }, { "cent", 0xA2 }, { "pound", 0xA3 }, { "euro", 0x20AC }, { "yen", 0xA5 },
    { "sect", 0xA7 }, { "copy", 0xA9 }, { "laquo", 0xAB }, { "reg", 0xAE }, { "deg", 0xB0 }, { "plusmn", 0xB1 },
    { "sup2", 0xB2 }, { "acute", 0xB4 }, { "micro", 0xB5 }, { "para", 0xB6 }, { "middot", 0xB7 }, { "ordm", 0xBA },
    { "ordf", 0xAA }, { "raquo", 0xBB }, { "frac12", 0xBD }, { "iquest", 0xBF }, { "times", 0xD7 }, { "divide", 0xF7 },
    { "Aacute", 0xC1 }, { "Eacute", 0xC9 }, { "Iacute", 0xCD }, { "Oacute", 0xD3 }, { "Uacute", 0xDA }, { "Ntilde", 0xD1 },
    { "Uuml", 0xDC }, { "aacute", 0xE1 }, { "eacute", 0xE9 }, { "iacute", 0xED }, { "oacute", 0xF3 }, { "uacute", 0xFA },
    { "ntilde", 0xF1 }, { "uuml", 0xFC }, { "agrave", 0xE0 }, { "egrave", 0xE8 }, { "ograve", 0xF2 }, { "acirc", 0xE2 },
    { "ecirc", 0xEA }, { "ocirc", 0xF4 }, { "auml", 0xE4 }, { "ouml", 0xF6 }, { "Auml", 0xC4 }, { "Ouml", 0xD6 },
    { "szlig", 0xDF }, { "ccedil", 0xE7 }, { "Ccedil", 0xC7 }, { "aring", 0xE5 }, { "oslash", 0xF8 },
    { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "sbquo", 0x201A },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bdquo", 0x201E }, { "bull", 0x2022 }, { "hellip", 0x2026 },
    { "trade", 0x2122 }, { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 },
    { "le", 0x2264 }, { "ge", 0x2265 }, { "ne", 0x2260 }, { "thinsp", 0x2009 }, { "ensp", 0x2002 }, { "emsp", 0x2003 },
    { "shy", 0 }, { "zwnj", 0 }, { "zwj", 0 }, { "lrm", 0 }, { "rlm", 0 },
};

static void html_unescape(const char *s, size_t n, of_sb_t *o)
{
    size_t from = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') continue;
        size_t k = i + 1;
        while (k < n && k - i < 12 && ((s[k] >= 'a' && s[k] <= 'z') || (s[k] >= 'A' && s[k] <= 'Z') || (s[k] >= '0' && s[k] <= '9') || s[k] == '#'))
            k++;
        if (k >= n || s[k] != ';') continue;
        size_t el = k - i - 1;
        uint32_t cp = 0xFFFFFFFF;
        if (s[i + 1] == '#') {
            of_sb_t t = { 0 };
            of_xml_unescape(s + i, k - i + 1, &t);
            of_sb_put(o, s + from, i - from);
            of_sb_put(o, t.s ? t.s : "", t.n);
            of_sb_free(&t);
            from = k + 1;
            i = k;
            continue;
        }
        static const struct { const char *n; char c; } X5[] = { { "lt", '<' }, { "gt", '>' }, { "amp", '&' }, { "quot", '"' }, { "apos", '\'' } };
        for (size_t e = 0; e < 5 && cp == 0xFFFFFFFF; e++)
            if (strlen(X5[e].n) == el && !strncmp(s + i + 1, X5[e].n, el)) cp = (uint32_t)X5[e].c;
        for (size_t e = 0; e < sizeof ENT / sizeof ENT[0] && cp == 0xFFFFFFFF; e++)
            if (strlen(ENT[e].n) == el && !strncmp(s + i + 1, ENT[e].n, el)) cp = ENT[e].cp;
        if (cp == 0xFFFFFFFF) continue;
        of_sb_put(o, s + from, i - from);
        if (cp == 0xA0) cp = ' ';
        if (cp) {
            char u[4];
            of_sb_put(o, u, (size_t)of_utf8_put(u, cp));
        }
        from = k + 1;
        i = k;
    }
    of_sb_put(o, s + from, n - from);
}

/* the tag's name, lower case, without a prefix */
static void tag_name(const of_xml_t *X, char *out, size_t len)
{
    const char *n = X->name;
    int l = X->name_len;
    const char *colon = memchr(n, ':', (size_t)l);
    if (colon) { l -= (int)(colon + 1 - n); n = colon + 1; }
    size_t k = 0;
    for (int i = 0; i < l && k < len - 1; i++) {
        char c = n[i];
        out[k++] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    out[k] = 0;
}

static bool attr_ci(const of_xml_t *X, const char *name, char *out, size_t len)
{
    if (of_xml_attr(X, name, out, len)) return true;
    /* upper case, as old HTML writes them */
    char up[32];
    size_t k = 0;
    for (; name[k] && k < sizeof up - 1; k++) up[k] = name[k] >= 'a' && name[k] <= 'z' ? (char)(name[k] - 32) : name[k];
    up[k] = 0;
    return of_xml_attr(X, up, out, len);
}

static uint32_t css_color(const char *v)
{
    while (*v == ' ') v++;
    if (*v == '#') {
        const char *h = v + 1;
        size_t n = strspn(h, "0123456789abcdefABCDEF");
        if (n == 3) {
            unsigned r, g, b2;
            char t[2] = { 0, 0 };
            t[0] = h[0]; r = (unsigned)strtoul(t, NULL, 16);
            t[0] = h[1]; g = (unsigned)strtoul(t, NULL, 16);
            t[0] = h[2]; b2 = (unsigned)strtoul(t, NULL, 16);
            return (r * 17) << 16 | (g * 17) << 8 | (b2 * 17) | 0x1000000u;
        }
        if (n >= 6) return (uint32_t)strtoul(h, NULL, 16) & 0xFFFFFF ? ((uint32_t)strtoul(h, NULL, 16) & 0xFFFFFF) | 0x1000000u : 0x1000000u;
    }
    static const struct { const char *n; uint32_t c; } N[] = {
        { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF }, { "black", 0x000000 }, { "gray", 0x808080 },
        { "grey", 0x808080 }, { "orange", 0xFFA500 }, { "purple", 0x800080 }, { "navy", 0x000080 }, { "maroon", 0x800000 },
        { "teal", 0x008080 }, { "olive", 0x808000 }, { "brown", 0xA52A2A }, { "darkred", 0x8B0000 }, { "darkblue", 0x00008B },
    };
    for (size_t i = 0; i < sizeof N / sizeof N[0]; i++)
        if (!strncasecmp(v, N[i].n, strlen(N[i].n))) return N[i].c | 0x1000000u;
    return 0;
}

/* an inline style="...": colour, weight, style, decoration */
static void inline_css(of_rstyle_t *st, const char *css)
{
    const char *p;
    if ((p = strstr(css, "font-weight"))) {
        p = strchr(p, ':');
        if (p) { p++; while (*p == ' ') p++; st->b = (!strncmp(p, "bold", 4) || atoi(p) >= 600) ? OF_TRI_ON : st->b; }
    }
    if ((p = strstr(css, "font-style"))) {
        p = strchr(p, ':');
        if (p) { p++; while (*p == ' ') p++; if (!strncmp(p, "italic", 6)) st->i = OF_TRI_ON; }
    }
    if ((p = strstr(css, "text-decoration"))) {
        if (strstr(p, "underline")) st->u = OF_TRI_ON;
        if (strstr(p, "line-through")) st->s = OF_TRI_ON;
    }
    const char *c = css;
    while ((c = strstr(c, "color"))) {
        bool bg = c > css && c[-1] == '-';
        const char *v = strchr(c, ':');
        if (!v) break;
        if (!bg) { uint32_t col = css_color(v + 1); if (col) st->color = col; }
        c = v;
    }
}

/* a path inside the package, from a reference relative to base, with ../ and %20 */
static void resolve(const char *base, const char *ref, char *out, size_t len)
{
    char tmp[400];
    if (ref[0] == '/') snprintf(tmp, sizeof tmp, "%s", ref + 1);
    else snprintf(tmp, sizeof tmp, "%s%s", base, ref);
    char *h = strchr(tmp, '#');
    if (h) *h = 0;
    /* %xx */
    size_t k = 0;
    for (size_t i = 0; tmp[i]; i++) {
        if (tmp[i] == '%' && tmp[i + 1] && tmp[i + 2]) {
            char hx[3] = { tmp[i + 1], tmp[i + 2], 0 };
            tmp[k++] = (char)strtol(hx, NULL, 16);
            i += 2;
        } else {
            tmp[k++] = tmp[i];
        }
    }
    tmp[k] = 0;
    char *p;
    while ((p = strstr(tmp, "/./"))) memmove(p, p + 2, strlen(p + 2) + 1);
    while ((p = strstr(tmp, "/../"))) {
        char *q = p;
        while (q > tmp && q[-1] != '/') q--;
        memmove(q, p + 4, strlen(p + 4) + 1);
    }
    if (!strncmp(tmp, "../", 3)) memmove(tmp, tmp + 3, strlen(tmp + 3) + 1);
    snprintf(out, len, "%s", tmp);
}

typedef struct {
    of_bld_t *b;
    const of_doc_t *d;
    const char *base;
    bool in_zip;
    int  lists[16], nlists;     /* 1 bullets, 2 numbers */
    int  ids[16];
    int  next_id;
    int  tdepth;
} hctx_t;

static void picture(hctx_t *H, const char *src, int w, int h)
{
    if (!src[0] || !strncmp(src, "http", 4) || !strncmp(src, "cid:", 4)) return;
    char target[300];
    int pw = 0, ph = 0;
    if (!strncmp(src, "data:", 5)) {
        /* data:image/png;base64,.... */
        const char *c = strstr(src, ";base64,");
        if (!c) return;
        c += 8;
        size_t n = strlen(c), k = 0;
        uint8_t *bin = of_alloc(n * 3 / 4 + 4);
        if (!bin) return;
        uint32_t acc = 0;
        int bits = 0;
        for (size_t i = 0; i < n; i++) {
            char ch = c[i];
            int v = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52
                  : ch == '+' ? 62 : ch == '/' ? 63 : -1;
            if (v < 0) continue;
            acc = acc << 6 | (uint32_t)v;
            bits += 6;
            if (bits >= 8) { bits -= 8; bin[k++] = (uint8_t)(acc >> bits); }
        }
        char name[32];
        static int seq;
        snprintf(name, sizeof name, "img%d.%s", ++seq, strstr(src, "jpeg") || strstr(src, "jpg") ? "jpg" : "png");
        int iw, ih;
        if (pic_size(bin, k, &iw, &ih)) pic_pt(iw, ih, &pw, &ph);
        bool ok = of_cache_file(name, bin, k, target, sizeof target);
        of_free(bin);
        if (ok) of_bld_image(H->b, target, true, w ? w * 3 / 4 : pw, h ? h * 3 / 4 : ph);
        return;
    }
    resolve(H->base, src, target, sizeof target);
    if (H->in_zip) {
        int i = of_zip_find(&H->d->zip, target);
        if (i < 0) return;
        /* its size, from the first bytes */
        size_t n;
        uint8_t *bin = of_zip_read(&H->d->zip, i, &n);
        int iw, ih;
        if (bin && pic_size(bin, n, &iw, &ih)) pic_pt(iw, ih, &pw, &ph);
        of_free(bin);
        of_bld_image(H->b, target, false, w ? w * 3 / 4 : pw, h ? h * 3 / 4 : ph);
    } else {
        size_t n;
        uint8_t *bin = of_file_read(target, &n, 16u * 1024 * 1024);
        if (!bin) return;
        int iw, ih;
        if (pic_size(bin, n, &iw, &ih)) pic_pt(iw, ih, &pw, &ph);
        of_free(bin);
        of_bld_image(H->b, target, true, w ? w * 3 / 4 : pw, h ? h * 3 / 4 : ph);
    }
}

void of_html_into(of_bld_t *b, const char *src, size_t len, const char *base, bool in_zip)
{
    hctx_t H = { .b = b, .d = b->d, .base = base, .in_zip = in_zip, .next_id = 2000 };
    of_rstyle_t stack[32];
    char stags[32][12];
    int nst = 0;
    int skip = 0;               /* inside <head>, <script>... */
    bool in_body_seen = false;
    of_xml_t X;
    of_xml_init(&X, src, len);
    int t;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_TEXT) {
            if (skip) continue;
            of_sb_t tx = { 0 };
            html_unescape(X.src + X.start, X.end - X.start, &tx);
            if (tx.n) of_bld_text(b, tx.s, tx.n);
            of_sb_free(&tx);
            continue;
        }
        if (t == OX_OTHER) continue;
        char tg[16];
        tag_name(&X, tg, sizeof tg);
        bool open = t == OX_OPEN || t == OX_EMPTY, close = t == OX_CLOSE || t == OX_EMPTY;
        if (!strcmp(tg, "script") || !strcmp(tg, "style") || !strcmp(tg, "noscript") || !strcmp(tg, "template")) {
            if (t == OX_OPEN) {
                /* to its end, as raw text */
                char endtag[24];
                snprintf(endtag, sizeof endtag, "</%s", tg);
                size_t p = X.pos;
                while (p + strlen(endtag) <= len && strncasecmp(src + p, endtag, strlen(endtag))) p++;
                const char *gt = p < len ? memchr(src + p, '>', len - p) : NULL;
                X.pos = gt ? (size_t)(gt - src) + 1 : len;
            }
            continue;
        }
        if (!strcmp(tg, "head")) { if (t == OX_OPEN) skip++; else if (t == OX_CLOSE && skip) skip--; continue; }
        if (!strcmp(tg, "body")) { in_body_seen = true; continue; }
        if (skip) continue;
        (void)in_body_seen;
        /* blocks */
        if (tg[0] == 'h' && tg[1] >= '1' && tg[1] <= '6' && !tg[2]) {
            b->heading = open && !close ? tg[1] - '0' : 0;
            if (open && !close) { b->list = 0; }
            of_bld_para(b);
            continue;
        }
        if (!strcmp(tg, "p") || !strcmp(tg, "div") || !strcmp(tg, "section") || !strcmp(tg, "article") ||
            !strcmp(tg, "header") || !strcmp(tg, "footer") || !strcmp(tg, "main") || !strcmp(tg, "aside") ||
            !strcmp(tg, "nav") || !strcmp(tg, "figure") || !strcmp(tg, "figcaption") || !strcmp(tg, "address") ||
            !strcmp(tg, "center") || !strcmp(tg, "dd") || !strcmp(tg, "dt") || !strcmp(tg, "dl") || !strcmp(tg, "form") ||
            !strcmp(tg, "blockquote") || !strcmp(tg, "caption")) {
            if (open && !close) {
                char al[16];
                if (attr_ci(&X, "align", al, sizeof al)) b->align = !strcasecmp(al, "center") ? OF_AL_CENTER : !strcasecmp(al, "right") ? OF_AL_RIGHT : 0;
                else if (!strcmp(tg, "center")) b->align = OF_AL_CENTER;
            }
            if (close) b->align = 0;
            if (H.nlists == 0) b->list = 0;
            of_bld_para(b);
            if (!strcmp(tg, "blockquote")) {
                if (open && !close) b->indent++;
                else if (close && b->indent) b->indent--;
                if (b->cur >= 0) b->d->p[b->cur].indent = (uint8_t)b->indent;
            }
            continue;
        }
        if (!strcmp(tg, "pre")) {
            of_bld_para(b);
            b->pre = open && !close;
            b->st.color = b->pre ? 0x1307070 : 0;
            continue;
        }
        if (!strcmp(tg, "br")) {
            of_para_t *p = cur(b);
            if (p) of_para_append(b->d, &b->d->p[b->cur], "\n", 1, (uint16_t)of_doc_style(b->d, &b->st));
            b->space = false;
            continue;
        }
        if (!strcmp(tg, "hr")) { of_bld_break(b); continue; }
        if (!strcmp(tg, "ul") || !strcmp(tg, "ol") || !strcmp(tg, "menu")) {
            if (open && !close && H.nlists < 16) {
                H.lists[H.nlists] = tg[0] == 'o' ? OF_LIST_NUMBER : OF_LIST_BULLET;
                H.ids[H.nlists] = H.next_id++;
                H.nlists++;
            } else if (t == OX_CLOSE && H.nlists) {
                H.nlists--;
            }
            b->list = 0;
            of_bld_para(b);
            continue;
        }
        if (!strcmp(tg, "li")) {
            if (open && !close) {
                b->list = H.nlists ? H.lists[H.nlists - 1] : OF_LIST_BULLET;
                b->ilvl = H.nlists ? H.nlists - 1 : 0;
                b->numid = H.nlists ? H.ids[H.nlists - 1] : 1;
                b->cur = b->cur;
                /* a new item even if the last one is empty */
                if (b->cur >= 0 && !b->d->p[b->cur].len && b->d->p[b->cur].list) b->cur = b->cur;
                of_bld_para(b);
            } else {
                b->list = 0;
            }
            continue;
        }
        if (!strcmp(tg, "table")) {
            /* a table in a cell: its text goes in the cell */
            if (open && !close) { if (H.tdepth++ == 0) of_bld_table(b); }
            else if (close && H.tdepth && --H.tdepth == 0) { of_bld_table_end(b); of_bld_para(b); }
            continue;
        }
        if (!strcmp(tg, "tr")) { if (open && !close && H.tdepth == 1) of_bld_row(b); continue; }
        if (!strcmp(tg, "td") || !strcmp(tg, "th")) {
            if (H.tdepth != 1) { if (open && !close) of_bld_text(b, " ", 1); continue; }
            if (open && !close && b->row >= 0) {
                char cs[8] = "1";
                attr_ci(&X, "colspan", cs, sizeof cs);
                of_bld_cell(b, atoi(cs));
                b->st.b = tg[1] == 'h' ? OF_TRI_ON : OF_TRI_UNSET;
            } else if (close) {
                b->st.b = OF_TRI_UNSET;
            }
            continue;
        }
        if (!strcmp(tg, "img")) {
            char sv[1024] = "", wv[16] = "", hv[16] = "";
            attr_ci(&X, "src", sv, sizeof sv);
            attr_ci(&X, "width", wv, sizeof wv);
            attr_ci(&X, "height", hv, sizeof hv);
            picture(&H, sv, atoi(wv), atoi(hv));
            continue;
        }
        if (!strcmp(tg, "image")) {         /* svg <image> in EPUB covers */
            char sv[512] = "";
            if (!of_xml_attr(&X, "xlink:href", sv, sizeof sv)) of_xml_attr(&X, "href", sv, sizeof sv);
            picture(&H, sv, 0, 0);
            continue;
        }
        /* inline: a style pushed and popped */
        static const char *const INL[] = { "b", "strong", "i", "em", "cite", "dfn", "var", "u", "ins", "s", "strike", "del",
                                           "sup", "sub", "a", "code", "kbd", "tt", "samp", "mark", "font", "span", "small", "big", "q", "abbr" };
        bool inl = false;
        for (size_t i = 0; i < sizeof INL / sizeof INL[0]; i++)
            if (!strcmp(tg, INL[i])) inl = true;
        if (!inl) continue;
        if (t == OX_OPEN) {
            if (nst < 32) { stack[nst] = b->st; snprintf(stags[nst], 12, "%.11s", tg); nst++; }
            if (!strcmp(tg, "b") || !strcmp(tg, "strong")) b->st.b = OF_TRI_ON;
            else if (!strcmp(tg, "i") || !strcmp(tg, "em") || !strcmp(tg, "cite") || !strcmp(tg, "dfn") || !strcmp(tg, "var")) b->st.i = OF_TRI_ON;
            else if (!strcmp(tg, "u") || !strcmp(tg, "ins")) b->st.u = OF_TRI_ON;
            else if (!strcmp(tg, "s") || !strcmp(tg, "strike") || !strcmp(tg, "del")) b->st.s = OF_TRI_ON;
            else if (!strcmp(tg, "sup")) b->st.sup = 1;
            else if (!strcmp(tg, "sub")) b->st.sup = 2;
            else if (!strcmp(tg, "code") || !strcmp(tg, "kbd") || !strcmp(tg, "tt") || !strcmp(tg, "samp")) b->st.color = 0x1307070;
            else if (!strcmp(tg, "mark")) b->st.hl = 0x1FFF59D;
            else if (!strcmp(tg, "small")) b->st.half_pt = 18;
            else if (!strcmp(tg, "big")) b->st.half_pt = 28;
            else if (!strcmp(tg, "a")) {
                char href[512];
                if (attr_ci(&X, "href", href, sizeof href)) of_bld_link(b, href);
            } else if (!strcmp(tg, "font")) {
                char c[32];
                if (attr_ci(&X, "color", c, sizeof c)) b->st.color = css_color(c);
            }
            char css[256];
            if (attr_ci(&X, "style", css, sizeof css)) inline_css(&b->st, css);
        } else if (t == OX_CLOSE) {
            /* back to the style before the matching open tag */
            for (int k = nst - 1; k >= 0; k--)
                if (!strcmp(stags[k], tg)) { b->st = stack[k]; nst = k; break; }
        }
    }
}

static char *to_utf8(char *s, size_t *n, bool latin1)
{
    if (!latin1 && of_utf8_valid(s, *n)) return s;
    size_t m;
    char *u = of_latin1_to_utf8(s, *n, &m);
    if (!u) return s;
    of_free(s);
    *n = m;
    return u;
}

static void base_of(const char *path, char *out, size_t len)
{
    snprintf(out, len, "%s", path);
    char *sl = strrchr(out, '/');
    if (sl) sl[1] = 0;
    else out[0] = 0;
}

of_doc_t *of_html_load(const char *path, char *err, size_t errlen)
{
    size_t n;
    char *src = (char *)of_file_read(path, &n, READ_MAX);
    if (!src) { snprintf(err, errlen, "read"); return NULL; }
    bool latin1 = strstr(src, "charset=iso-8859-1") || strstr(src, "charset=windows-1252") || strstr(src, "charset=ISO-8859-1");
    src = to_utf8(src, &n, latin1);
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    of_bld_t b;
    of_bld_init(&b, d);
    char base[300];
    base_of(path, base, sizeof base);
    of_html_into(&b, src, n, base, false);
    of_bld_done(&b);
    snprintf(d->kind, sizeof d->kind, "HTML");
    of_free(src);
    return d;
}

/* -------------------------------------------------------------------------- */
/* EPUB                                                                        */
/* -------------------------------------------------------------------------- */

of_doc_t *of_epub_load(const char *path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *raw = of_file_read(path, &len, READ_MAX);
    if (!raw) { snprintf(err, errlen, "read"); return NULL; }
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    d->file = raw;
    d->file_len = len;
    if (!of_zip_open(&d->zip, raw, len)) { snprintf(err, errlen, "zip"); of_doc_free(d); return NULL; }
    size_t cl;
    char *cont = (char *)of_doc_part(d, "META-INF/container.xml", &cl);
    char opf[300] = "";
    if (cont) {
        of_xml_t X;
        of_xml_init(&X, cont, cl);
        int t;
        while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR)
            if ((t == OX_EMPTY || t == OX_OPEN) && of_xml_local_is(&X, "rootfile")) { of_xml_attr(&X, "full-path", opf, sizeof opf); break; }
        of_free(cont);
    }
    size_t ol;
    char *o = opf[0] ? (char *)of_doc_part(d, opf, &ol) : NULL;
    if (!o) { snprintf(err, errlen, "opf"); of_doc_free(d); return NULL; }
    char base[300];
    base_of(opf, base, sizeof base);
    /* the manifest and the spine */
    typedef struct { char id[64], href[256], type[48]; } item_t;
    item_t *items = NULL;
    int nitems = 0;
    char (*spine)[64] = NULL;
    int nspine = 0;
    char title[256] = "", author[128] = "";
    of_xml_t X;
    of_xml_init(&X, o, ol);
    int t;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_OPEN && of_xml_local_is(&X, "title") && !title[0]) {
            of_sb_t s = { 0 };
            of_xml_text(&X, &s);
            snprintf(title, sizeof title, "%s", s.s ? s.s : "");
            of_sb_free(&s);
        } else if (t == OX_OPEN && of_xml_local_is(&X, "creator") && !author[0]) {
            of_sb_t s = { 0 };
            of_xml_text(&X, &s);
            snprintf(author, sizeof author, "%s", s.s ? s.s : "");
            of_sb_free(&s);
        } else if ((t == OX_EMPTY || t == OX_OPEN) && of_xml_local_is(&X, "item")) {
            item_t *n = of_realloc(items, (size_t)(nitems + 1) * sizeof *n);
            if (!n) break;
            items = n;
            memset(&items[nitems], 0, sizeof *n);
            of_xml_attr(&X, "id", items[nitems].id, sizeof items[nitems].id);
            of_xml_attr(&X, "href", items[nitems].href, sizeof items[nitems].href);
            of_xml_attr(&X, "media-type", items[nitems].type, sizeof items[nitems].type);
            nitems++;
        } else if ((t == OX_EMPTY || t == OX_OPEN) && of_xml_local_is(&X, "itemref")) {
            char (*n)[64] = of_realloc(spine, (size_t)(nspine + 1) * sizeof *n);
            if (!n) break;
            spine = n;
            spine[nspine][0] = 0;
            of_xml_attr(&X, "idref", spine[nspine], 64);
            nspine++;
        }
    }
    of_free(o);
    of_bld_t b;
    of_bld_init(&b, d);
    if (title[0]) {
        b.heading = 1;
        of_bld_para(&b);
        of_bld_text(&b, title, strlen(title));
        b.heading = 0;
        if (author[0]) {
            of_bld_para(&b);
            b.st.i = OF_TRI_ON;
            of_bld_text(&b, author, strlen(author));
            b.st.i = OF_TRI_UNSET;
        }
    }
    for (int s = 0; s < nspine; s++) {
        for (int i = 0; i < nitems; i++) {
            if (strcmp(items[i].id, spine[s])) continue;
            char p[400];
            resolve(base, items[i].href, p, sizeof p);
            size_t hl;
            char *html = (char *)of_doc_part(d, p, &hl);
            if (!html) break;
            char cbase[400];
            base_of(p, cbase, sizeof cbase);
            if (d->np > 1 || d->p[0].len) of_bld_break(&b);
            of_html_into(&b, html, hl, cbase, true);
            of_free(html);
            break;
        }
    }
    of_free(items);
    of_free(spine);
    of_bld_done(&b);
    snprintf(d->kind, sizeof d->kind, "EPUB");
    return d;
}

/* -------------------------------------------------------------------------- */
/* Mail (.eml, RFC 5322 and MIME)                                              */
/* -------------------------------------------------------------------------- */

static size_t b64_decode(const char *s, size_t n, uint8_t *out)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[k++] = (uint8_t)(acc >> bits); }
    }
    return k;
}

static size_t qp_decode(const char *s, size_t n, uint8_t *out, bool header)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '=' && i + 1 < n && (s[i + 1] == '\n' || s[i + 1] == '\r')) {
            i++;
            if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') i++;
            continue;
        }
        if (s[i] == '=' && i + 2 < n) {
            char hx[3] = { s[i + 1], s[i + 2], 0 };
            char *e;
            long v = strtol(hx, &e, 16);
            if (!*e) { out[k++] = (uint8_t)v; i += 2; continue; }
        }
        out[k++] = header && s[i] == '_' ? ' ' : (uint8_t)s[i];
    }
    return k;
}

/* =?charset?B|Q?text?= in a header */
static void decode_words(const char *s, of_sb_t *o)
{
    while (*s) {
        const char *a = strstr(s, "=?");
        if (!a) { of_sb_puts(o, s); return; }
        const char *q1 = strchr(a + 2, '?');
        const char *q2 = q1 ? strchr(q1 + 1, '?') : NULL;
        const char *e = q2 ? strstr(q2 + 1, "?=") : NULL;
        if (!e) { of_sb_puts(o, s); return; }
        /* white space between two encoded words goes */
        const char *pre_end = a;
        bool only_ws = true;
        for (const char *p = s; p < a; p++) if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') only_ws = false;
        if (!(only_ws && o->n)) of_sb_put(o, s, (size_t)(pre_end - s));
        char cs[32];
        snprintf(cs, sizeof cs, "%.*s", (int)(q1 - a - 2), a + 2);
        char enc = q1[1];
        size_t n = (size_t)(e - q2 - 1);
        uint8_t *bin = of_alloc(n + 4);
        size_t k = (enc == 'B' || enc == 'b') ? b64_decode(q2 + 1, n, bin) : qp_decode(q2 + 1, n, bin, true);
        if (!strncasecmp(cs, "utf-8", 5) || of_utf8_valid((char *)bin, k)) of_sb_put(o, (char *)bin, k);
        else {
            size_t m;
            char *u = of_latin1_to_utf8((char *)bin, k, &m);
            if (u) { of_sb_put(o, u, m); of_free(u); }
        }
        of_free(bin);
        s = e + 2;
    }
}

typedef struct {
    of_bld_t *b;
    const char *html;           /* the chosen body */
    size_t html_n;
    char *text;                 /* or plain text */
    size_t text_n;
    bool  have_html;
    char  names[16][96];
    size_t sizes[16];
    int   natt;
} mail_t;

/* a header's value (unfolded), from a block of headers */
static bool header(const char *h, size_t hn, const char *name, char *out, size_t len)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl < hn; i++) {
        if ((i == 0 || h[i - 1] == '\n') && !strncasecmp(h + i, name, nl) && h[i + nl] == ':') {
            size_t k = 0;
            size_t j = i + nl + 1;
            while (j < hn && (h[j] == ' ' || h[j] == '\t')) j++;
            for (; j < hn && k < len - 1; j++) {
                if (h[j] == '\r') continue;
                if (h[j] == '\n') {
                    if (j + 1 < hn && (h[j + 1] == ' ' || h[j + 1] == '\t')) { out[k++] = ' '; j++; continue; }
                    break;
                }
                out[k++] = h[j];
            }
            out[k] = 0;
            return true;
        }
    }
    return false;
}

static bool param(const char *v, const char *name, char *out, size_t len)
{
    const char *p = v;
    size_t nl = strlen(name);
    while ((p = strchr(p, ';'))) {
        p++;
        while (*p == ' ') p++;
        if (!strncasecmp(p, name, nl) && (p[nl] == '=' || (p[nl] == '*' && strchr(p, '=')))) {
            const char *e = strchr(p, '=') + 1;
            if (*e == '"') {
                e++;
                const char *z = strchr(e, '"');
                snprintf(out, len, "%.*s", z ? (int)(z - e) : (int)strlen(e), e);
            } else {
                size_t l = strcspn(e, "; ");
                snprintf(out, len, "%.*s", (int)l, e);
            }
            return true;
        }
    }
    return false;
}

static char *decode_body(const char *body, size_t n, const char *cte, const char *ctype, size_t *out_n)
{
    uint8_t *bin = of_alloc(n + 4);
    if (!bin) return NULL;
    size_t k;
    if (!strncasecmp(cte, "base64", 6)) k = b64_decode(body, n, bin);
    else if (!strncasecmp(cte, "quoted-printable", 16)) k = qp_decode(body, n, bin, false);
    else { memcpy(bin, body, n); k = n; }
    bin[k] = 0;
    char cs[32] = "";
    param(ctype, "charset", cs, sizeof cs);
    if (strncasecmp(ctype, "text/", 5)) { *out_n = k; return (char *)bin; }
    if (!strncasecmp(cs, "utf-8", 5) || (!cs[0] && of_utf8_valid((char *)bin, k))) { *out_n = k; return (char *)bin; }
    size_t m;
    char *u = of_latin1_to_utf8((char *)bin, k, &m);
    of_free(bin);
    *out_n = m;
    return u;
}

static void mime_part(mail_t *M, const char *s, size_t n, int depth)
{
    /* headers end at an empty line */
    size_t hl = 0;
    while (hl + 1 < n && !(s[hl] == '\n' && (s[hl + 1] == '\n' || (s[hl + 1] == '\r' && hl + 2 < n && s[hl + 2] == '\n')))) hl++;
    size_t body = hl + 1;
    while (body < n && (s[body] == '\r' || s[body] == '\n')) { body++; if (s[body - 1] == '\n') break; }
    if (body < n && s[body] == '\n') body++;
    char ctype[256] = "text/plain", cte[64] = "7bit", disp[256] = "";
    header(s, hl, "Content-Type", ctype, sizeof ctype);
    header(s, hl, "Content-Transfer-Encoding", cte, sizeof cte);
    header(s, hl, "Content-Disposition", disp, sizeof disp);
    if (!strncasecmp(ctype, "multipart/", 10) && depth < 8) {
        char bnd[128];
        if (!param(ctype, "boundary", bnd, sizeof bnd)) return;
        char marker[140];
        snprintf(marker, sizeof marker, "--%s", bnd);
        size_t ml = strlen(marker);
        const char *p = s + body, *end = s + n;
        const char *part = NULL;
        while (p < end) {
            const char *hit = NULL;
            for (const char *q = p; q + ml <= end; q++)
                if ((q == s + body || q[-1] == '\n') && !memcmp(q, marker, ml)) { hit = q; break; }
            if (!hit) break;
            if (part) mime_part(M, part, (size_t)(hit - part), depth + 1);
            if (hit + ml + 1 < end && hit[ml] == '-' && hit[ml + 1] == '-') break;
            const char *nl = memchr(hit, '\n', (size_t)(end - hit));
            if (!nl) break;
            part = nl + 1;
            p = part;
        }
        return;
    }
    char fname[96] = "";
    if (!param(disp, "filename", fname, sizeof fname)) param(ctype, "name", fname, sizeof fname);
    bool attach = !strncasecmp(disp, "attachment", 10) || (fname[0] && strncasecmp(ctype, "text/", 5));
    if (attach) {
        if (M->natt < 16) {
            of_sb_t dn = { 0 };
            decode_words(fname, &dn);
            snprintf(M->names[M->natt], sizeof M->names[M->natt], "%s", dn.s ? dn.s : fname);
            of_sb_free(&dn);
            M->sizes[M->natt] = !strncasecmp(cte, "base64", 6) ? (n - body) * 3 / 4 : n - body;
            M->natt++;
        }
        return;
    }
    if (!strncasecmp(ctype, "text/html", 9) && !M->have_html) {
        size_t m;
        char *h = decode_body(s + body, n - body, cte, ctype, &m);
        if (h) { M->html = h; M->html_n = m; M->have_html = true; }
    } else if (!strncasecmp(ctype, "text/plain", 10) && !M->text) {
        size_t m;
        M->text = decode_body(s + body, n - body, cte, ctype, &M->text_n);
        (void)m;
    }
}

static void labelled(of_bld_t *b, const char *label, const char *value)
{
    if (!value[0]) return;
    of_bld_para(b);
    b->st.b = OF_TRI_ON;
    b->st.color = 0x1808080;
    of_bld_text(b, label, strlen(label));
    of_bld_text(b, ": ", 2);
    b->st.b = OF_TRI_UNSET;
    b->st.color = 0;
    of_sb_t v = { 0 };
    decode_words(value, &v);
    of_bld_text(b, v.s ? v.s : "", v.n);
    of_sb_free(&v);
}

of_doc_t *of_eml_load(const char *path, char *err, size_t errlen)
{
    size_t n;
    char *src = (char *)of_file_read(path, &n, READ_MAX);
    if (!src) { snprintf(err, errlen, "read"); return NULL; }
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    of_bld_t b;
    of_bld_init(&b, d);
    size_t hl = 0;
    while (hl + 1 < n && !(src[hl] == '\n' && (src[hl + 1] == '\n' || src[hl + 1] == '\r'))) hl++;
    char v[1024];
    if (header(src, hl, "Subject", v, sizeof v)) {
        of_sb_t s = { 0 };
        decode_words(v, &s);
        b.heading = 1;
        of_bld_para(&b);
        of_bld_text(&b, s.s ? s.s : "", s.n);
        b.heading = 0;
        of_sb_free(&s);
    }
    static const char *const H[][2] = { { "From", N_("De") }, { "To", N_("Para") }, { "Cc", N_("Copia") }, { "Date", N_("Fecha") } };
    for (size_t i = 0; i < 4; i++)
        if (header(src, hl, H[i][0], v, sizeof v)) labelled(&b, _(H[i][1]), v);
    of_bld_break(&b);
    mail_t M = { .b = &b };
    mime_part(&M, src, n, 0);
    if (M.have_html) {
        of_html_into(&b, M.html, M.html_n, "", false);
    } else if (M.text) {
        /* plain text: a paragraph per line, quoted lines in italics */
        const char *p = M.text, *e = M.text + M.text_n;
        while (p < e) {
            const char *nl = memchr(p, '\n', (size_t)(e - p));
            size_t l = nl ? (size_t)(nl - p) : (size_t)(e - p);
            if (l && p[l - 1] == '\r') l--;
            of_bld_para(&b);
            b.st.i = l && p[0] == '>' ? OF_TRI_ON : OF_TRI_UNSET;
            of_bld_text(&b, p, l);
            p = nl ? nl + 1 : e;
        }
        b.st.i = OF_TRI_UNSET;
    }
    if (M.natt) {
        b.heading = 2;
        of_bld_para(&b);
        const char *a = _("Adjuntos");
        of_bld_text(&b, a, strlen(a));
        b.heading = 0;
        for (int i = 0; i < M.natt; i++) {
            b.list = OF_LIST_BULLET;
            of_bld_para(&b);
            char line[160];
            snprintf(line, sizeof line, "%s (%lu KB)", M.names[i], (unsigned long)((M.sizes[i] + 1023) / 1024));
            of_bld_text(&b, line, strlen(line));
        }
        b.list = 0;
    }
    of_free((void *)M.html);
    of_free(M.text);
    of_bld_done(&b);
    snprintf(d->kind, sizeof d->kind, "EML");
    of_free(src);
    return d;
}

/* -------------------------------------------------------------------------- */
/* FB2                                                                         */
/* -------------------------------------------------------------------------- */

of_doc_t *of_fb2_load(const char *path, char *err, size_t errlen)
{
    size_t n;
    char *src = (char *)of_file_read(path, &n, READ_MAX);
    if (!src) { snprintf(err, errlen, "read"); return NULL; }
    src = to_utf8(src, &n, strstr(src, "encoding=\"windows-1251\"") == NULL && strstr(src, "encoding=\"windows-1252\"") != NULL);
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    of_bld_t b;
    of_bld_init(&b, d);
    /* the pictures first: <binary id content-type> */
    typedef struct { char id[64], path[300]; int w, h; } bin_t;
    bin_t *bins = NULL;
    int nbins = 0;
    of_xml_t X;
    of_xml_init(&X, src, n);
    int t;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t != OX_OPEN || !of_xml_local_is(&X, "binary")) continue;
        char id[64] = "";
        of_xml_attr(&X, "id", id, sizeof id);
        size_t a = X.end;
        of_xml_skip(&X);
        size_t z = X.start;
        uint8_t *bin = of_alloc((z - a) * 3 / 4 + 4);
        if (!bin) continue;
        size_t k = b64_decode(src + a, z - a, bin);
        bin_t *nb = of_realloc(bins, (size_t)(nbins + 1) * sizeof *nb);
        if (nb) {
            bins = nb;
            memset(&bins[nbins], 0, sizeof *nb);
            snprintf(bins[nbins].id, sizeof bins[nbins].id, "%s", id);
            char name[80];
            snprintf(name, sizeof name, "fb2_%.60s", id);
            int iw = 0, ih = 0;
            if (pic_size(bin, k, &iw, &ih)) pic_pt(iw, ih, &bins[nbins].w, &bins[nbins].h);
            if (of_cache_file(name, bin, k, bins[nbins].path, sizeof bins[nbins].path)) nbins++;
        }
        of_free(bin);
    }
    of_xml_init(&X, src, n);
    int section = 0, in_title = 0;
    bool in_body = false, in_desc = false;
    char tg[24];
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_TEXT) {
            if (!in_body && !in_title) continue;
            of_sb_t tx = { 0 };
            of_xml_unescape(X.src + X.start, X.end - X.start, &tx);
            if (tx.n) of_bld_text(&b, tx.s, tx.n);
            of_sb_free(&tx);
            continue;
        }
        if (t == OX_OTHER) continue;
        tag_name(&X, tg, sizeof tg);
        bool open = t == OX_OPEN, close = t == OX_CLOSE;
        if (!strcmp(tg, "description")) { in_desc = open; continue; }
        if (in_desc) {
            if (open && !strcmp(tg, "book-title")) {
                b.heading = 1;
                of_bld_para(&b);
                of_sb_t s = { 0 };
                of_xml_text(&X, &s);
                of_bld_text(&b, s.s ? s.s : "", s.n);
                of_sb_free(&s);
                b.heading = 0;
                of_bld_para(&b);
            }
            continue;
        }
        if (!strcmp(tg, "body")) { in_body = open; continue; }
        if (!strcmp(tg, "binary")) { if (open) of_xml_skip(&X); continue; }
        if (!in_body) continue;
        if (!strcmp(tg, "section")) { section += open ? 1 : close ? -1 : 0; if (open && d->np > 2) of_bld_break(&b); continue; }
        if (!strcmp(tg, "title")) { in_title = open; b.heading = open ? (section < 1 ? 1 : section > 2 ? 3 : section + 1) : 0; of_bld_para(&b); continue; }
        if (!strcmp(tg, "p") || !strcmp(tg, "v") || !strcmp(tg, "subtitle") || !strcmp(tg, "text-author")) {
            if (open || t == OX_EMPTY) of_bld_para(&b);
            continue;
        }
        if (!strcmp(tg, "empty-line")) { of_bld_para(&b); of_bld_para(&b); continue; }
        if (!strcmp(tg, "epigraph") || !strcmp(tg, "cite") || !strcmp(tg, "poem")) { b.st.i = open ? OF_TRI_ON : OF_TRI_UNSET; continue; }
        if (!strcmp(tg, "emphasis")) { b.st.i = open ? OF_TRI_ON : OF_TRI_UNSET; continue; }
        if (!strcmp(tg, "strong")) { b.st.b = open ? OF_TRI_ON : OF_TRI_UNSET; continue; }
        if (!strcmp(tg, "strikethrough")) { b.st.s = open ? OF_TRI_ON : OF_TRI_UNSET; continue; }
        if (!strcmp(tg, "sup")) { b.st.sup = open ? 1 : 0; continue; }
        if (!strcmp(tg, "sub")) { b.st.sup = open ? 2 : 0; continue; }
        if (!strcmp(tg, "image") && (t == OX_EMPTY || open)) {
            char href[96] = "";
            if (!of_xml_attr(&X, "l:href", href, sizeof href)) of_xml_attr(&X, "xlink:href", href, sizeof href);
            for (int i = 0; i < nbins; i++)
                if (href[0] == '#' && !strcmp(bins[i].id, href + 1)) of_bld_image(&b, bins[i].path, true, bins[i].w, bins[i].h);
            continue;
        }
    }
    of_free(bins);
    of_bld_done(&b);
    snprintf(d->kind, sizeof d->kind, "FB2");
    of_free(src);
    return d;
}

/* -------------------------------------------------------------------------- */
/* A picture, and the previews of Apple's files                                */
/* -------------------------------------------------------------------------- */

of_doc_t *of_image_load(const char *path, char *err, size_t errlen)
{
    size_t n;
    uint8_t *head = of_file_read(path, &n, 32u * 1024 * 1024);
    if (!head) { snprintf(err, errlen, "read"); return NULL; }
    int w = 0, h = 0, wp = 400, hp = 300;
    /* a picture alone fills the page's width (the page scales it down), small ones up to twice */
    if (pic_size(head, n, &w, &h) && w > 0 && h > 0) { wp = w * 3 / 2; hp = h * 3 / 2; }
    of_free(head);
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    of_bld_t b;
    of_bld_init(&b, d);
    b.align = OF_AL_CENTER;
    of_bld_para(&b);
    of_bld_image(&b, path, true, wp > 10 ? wp : 400, hp > 10 ? hp : 300);
    of_bld_para(&b);
    char info[64];
    snprintf(info, sizeof info, "%d \xC3\x97 %d", w, h);
    b.st.color = 0x1808080;
    if (w) of_bld_text(&b, info, strlen(info));
    of_bld_done(&b);
    snprintf(d->kind, sizeof d->kind, "IMG");
    return d;
}

of_doc_t *of_iwork_load(const char *path, char *err, size_t errlen)
{
    static const char *const PREVIEWS[] = { "preview.jpg", "preview-web.jpg", "QuickLook/Thumbnail.jpg",
                                            "QuickLook/Preview.jpg", "preview-micro.jpg" };
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    struct stat st;
    char target[400] = "";
    bool file = false;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* a package copied as a folder */
        for (size_t i = 0; i < sizeof PREVIEWS / sizeof PREVIEWS[0] && !target[0]; i++) {
            char p[400];
            snprintf(p, sizeof p, "%.300s/%s", path, PREVIEWS[i]);
            if (of_file_exists(p)) { snprintf(target, sizeof target, "%s", p); file = true; }
        }
    } else {
        size_t len;
        uint8_t *raw = of_file_read(path, &len, READ_MAX);
        if (!raw) { snprintf(err, errlen, "read"); of_doc_free(d); return NULL; }
        d->file = raw;
        d->file_len = len;
        if (of_zip_open(&d->zip, raw, len))
            for (size_t i = 0; i < sizeof PREVIEWS / sizeof PREVIEWS[0] && !target[0]; i++)
                if (of_zip_find(&d->zip, PREVIEWS[i]) >= 0) snprintf(target, sizeof target, "%s", PREVIEWS[i]);
    }
    of_bld_t b;
    of_bld_init(&b, d);
    b.heading = 1;
    of_bld_para(&b);
    const char *base = of_basename(path);
    of_bld_text(&b, base, strlen(base));
    b.heading = 0;
    of_bld_para(&b);
    b.st.color = 0x1808080;
    const char *msg = target[0] ? _("La vista previa que guarda el archivo. El contenido se edita en Pages, Numbers o Keynote.")
                                : _("Este archivo no trae una vista previa.");
    of_bld_text(&b, msg, strlen(msg));
    b.st.color = 0;
    if (target[0]) {
        int pw = 400, ph = 300;
        if (file) {
            size_t n;
            uint8_t *bin = of_file_read(target, &n, 16u * 1024 * 1024);
            int w, h;
            if (bin && pic_size(bin, n, &w, &h)) pic_pt(w, h, &pw, &ph);
            of_free(bin);
        } else {
            size_t n;
            uint8_t *bin = of_doc_part(d, target, &n);
            int w, h;
            if (bin && pic_size(bin, n, &w, &h)) pic_pt(w, h, &pw, &ph);
            of_free(bin);
        }
        b.align = OF_AL_CENTER;
        of_bld_para(&b);
        of_bld_image(&b, target, file, pw, ph);
    }
    /* the text inside, after the picture */
    {
        extern bool of_iwork_text(of_bld_t *b, const of_zip_t *z, const char *folder, const char *ext);
        of_doc_t *tmp = of_doc_new(OF_DOC_RO);
        of_bld_t tb;
        of_bld_init(&tb, tmp);
        bool dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        if (tmp && of_iwork_text(&tb, dir ? NULL : &d->zip, dir ? path : NULL, of_ext(path))) {
            b.align = 0;
            b.heading = 2;
            b.cur = -1;
            of_bld_para(&b);
            const char *h = !strcmp(of_ext(path), "numbers") ? _("Los textos de sus tablas") : _("Su texto");
            of_bld_text(&b, h, strlen(h));
            b.heading = 0;
            b.cur = -1;
            /* the paragraphs found, copied into this document */
            for (int i = 0; i < tmp->np; i++) {
                of_para_t *q = &tmp->p[i];
                if (!q->len) continue;
                b.heading = q->heading;
                b.list = q->list;
                b.cur = -1;
                of_bld_para(&b);
                of_bld_text(&b, q->txt, (size_t)q->len);
            }
            b.heading = 0;
            b.list = 0;
        }
        of_doc_free(tmp);
    }
    of_bld_done(&b);
    snprintf(d->kind, sizeof d->kind, "iWork");
    return d;
}

/* -------------------------------------------------------------------------- */
/* Which reader                                                                */
/* -------------------------------------------------------------------------- */

int of_reader_for(const char *e)
{
    if (!strcmp(e, "html") || !strcmp(e, "htm") || !strcmp(e, "xhtml") || !strcmp(e, "mht") || !strcmp(e, "mhtml")) return OF_RD_HTML;
    if (!strcmp(e, "epub")) return OF_RD_EPUB;
    if (!strcmp(e, "eml")) return OF_RD_EML;
    if (!strcmp(e, "fb2")) return OF_RD_FB2;
    if (!strcmp(e, "rtf")) return OF_RD_RTF;
    if (!strcmp(e, "md") || !strcmp(e, "markdown")) return OF_RD_MD;
    if (!strcmp(e, "vcf") || !strcmp(e, "vcard")) return OF_RD_VCF;
    if (!strcmp(e, "ics") || !strcmp(e, "ical")) return OF_RD_ICS;
    if (!strcmp(e, "jpg") || !strcmp(e, "jpeg") || !strcmp(e, "png") || !strcmp(e, "bmp")) return OF_RD_IMG;
    if (!strcmp(e, "pages") || !strcmp(e, "numbers") || !strcmp(e, "key")) return OF_RD_IWORK;
    if (!strcmp(e, "pdf")) return OF_RD_PDF;
    return OF_RD_NONE;
}

bool of_is_tree_ext(const char *e)
{
    static const char *const X[] = { "json", "geojson", "ipynb", "xml", "svg", "plist", "gpx", "kml", "opf", "rss", "atom",
                                     "yaml", "yml", "toml" };
    for (size_t i = 0; i < sizeof X / sizeof X[0]; i++)
        if (!strcmp(e, X[i])) return true;
    return false;
}

of_doc_t *of_read_any(const char *path, char *err, size_t errlen)
{
    if (of_is_tree_ext(of_ext(path))) return of_tree_load(path, err, errlen);
    switch (of_reader_for(of_ext(path))) {
    case OF_RD_HTML: return !strcmp(of_ext(path), "mht") || !strcmp(of_ext(path), "mhtml") ? of_eml_load(path, err, errlen)
                                                                                          : of_html_load(path, err, errlen);
    case OF_RD_EPUB: return of_epub_load(path, err, errlen);
    case OF_RD_EML: return of_eml_load(path, err, errlen);
    case OF_RD_FB2: return of_fb2_load(path, err, errlen);
    case OF_RD_RTF: return of_rtf_load(path, err, errlen);
    case OF_RD_MD: return of_md_load(path, err, errlen);
    case OF_RD_VCF: return of_vcf_load(path, err, errlen);
    case OF_RD_ICS: return of_ics_load(path, err, errlen);
    case OF_RD_IMG: return of_image_load(path, err, errlen);
    case OF_RD_IWORK: return of_iwork_load(path, err, errlen);
    case OF_RD_PDF: return of_pdf_load(path, NULL, err, errlen);
    default: snprintf(err, errlen, "format"); return NULL;
    }
}
