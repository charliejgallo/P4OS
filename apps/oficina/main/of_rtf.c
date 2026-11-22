/*
 * P4OS - Office: RTF, read into a read-only document.
 *
 * Groups and control words, the way WordPad, TextEdit and Word write them:
 * bold, italics, underline, strikethrough, sizes, colours (colortbl),
 * highlight, superscript; alignment, the headings of the stylesheet or
 * \outlinelevel; \'hh in the code page (1252), \uN with its \ucN fallback;
 * line, tab and page breaks; tables (\intbl, \cell, \row); hyperlinks
 * (HYPERLINK fields); PNG and JPEG pictures (\pict, hex or \bin) written to
 * the cache; footnotes shown in their place, small and grey. Font tables,
 * headers, footers, list tables and the like are skipped; the numbers and
 * bullets of lists come from \listtext, as the writer worked them out.
 */
#include "of_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { DS_TEXT = 0, DS_SKIP, DS_COLORS, DS_STYLES, DS_PICT, DS_FLDINST, DS_FLDRSLT, DS_FOOTNOTE };

typedef struct {
    of_rstyle_t st;
    int  dest;
    int  uc;
    int  cf, cb, hl;            /* colour indices */
    bool is_field;              /* this group is a \field */
} gst_t;

typedef struct {
    of_bld_t   b;
    gst_t      g[64];
    int        ng;
    int        skip_depth;      /* groups inside a skipped destination */
    of_sb_t    pend;            /* text waiting in the current style */
    uint32_t   colors[256];
    int        ncolors;
    int        cr, cg, cbl;     /* colortbl being read */
    bool       cset;
    int8_t     style_h[256];    /* \sN -> heading level */
    int        cur_s;
    of_sb_t    sname;
    /* paragraph */
    int        align, heading, pstyle;
    bool       intbl, in_row, in_cell, in_table;
    /* picture */
    of_sb_t    hex;
    int        pic_type;        /* 1 png, 2 jpeg, 0 other */
    int        picw, pich, picwg, pichg, scx, scy;
    /* field */
    of_sb_t    inst;
    char       url[400];
    int        pics;
} rtf_t;

#define G (&R->g[R->ng - 1])

static void flush(rtf_t *R)
{
    if (!R->pend.n) return;
    gst_t *g = G;
    of_rstyle_t st = g->st;
    st.color = g->cf > 0 && g->cf < R->ncolors ? R->colors[g->cf] : 0;
    int h = g->hl ? g->hl : g->cb;
    st.hl = h > 0 && h < R->ncolors ? R->colors[h] : 0;
    if (g->dest == DS_FOOTNOTE) { st.color = 0x1808080; st.half_pt = 16; }
    R->b.st = st;
    of_bld_text(&R->b, R->pend.s, R->pend.n);
    R->pend.n = 0;
}

static void para_props(rtf_t *R)
{
    R->b.align = R->align;
    R->b.heading = R->heading;
}

static void ensure_cell(rtf_t *R)
{
    if (!R->intbl) {
        if (R->in_table) {
            /* the table ended */
            of_bld_table_end(&R->b);
            R->in_table = R->in_row = R->in_cell = false;
            para_props(R);
            of_bld_para(&R->b);
        }
        return;
    }
    if (!R->in_table) { of_bld_table(&R->b); R->in_table = true; R->in_row = false; }
    if (!R->in_row) { of_bld_row(&R->b); R->in_row = true; R->in_cell = false; }
    if (!R->in_cell) { para_props(R); of_bld_cell(&R->b, 1); R->in_cell = true; }
}

static void put_text(rtf_t *R, const char *s, size_t n)
{
    gst_t *g = G;
    if (g->dest == DS_SKIP || R->skip_depth) return;
    if (g->dest == DS_STYLES) { of_sb_put(&R->sname, s, n); return; }
    if (g->dest == DS_FLDINST) { of_sb_put(&R->inst, s, n); return; }
    if (g->dest == DS_COLORS) {
        for (size_t i = 0; i < n; i++)
            if (s[i] == ';' && R->ncolors < 256) {
                R->colors[R->ncolors++] = R->cset ? (0x1000000u | (uint32_t)R->cr << 16 | (uint32_t)R->cg << 8 | (uint32_t)R->cbl) : 0;
                R->cr = R->cg = R->cbl = 0;
                R->cset = false;
            }
        return;
    }
    if (g->dest == DS_PICT) {
        for (size_t i = 0; i < n; i++)
            if ((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F')) of_sb_putc(&R->hex, s[i]);
        return;
    }
    if (!R->pend.n) {
        ensure_cell(R);
        para_props(R);
        if (R->b.cur >= 0) {
            of_para_t *p = &R->b.d->p[R->b.cur];
            if (!p->len) { p->heading = (uint8_t)R->heading; p->align = (uint8_t)R->align; }
        }
    }
    of_sb_put(&R->pend, s, n);
}

static void put_cp(rtf_t *R, uint32_t cp)
{
    char u[4];
    put_text(R, u, (size_t)of_utf8_put(u, cp));
}

static void picture_done(rtf_t *R)
{
    if (R->pic_type && R->hex.n > 16) {
        size_t n = R->hex.n / 2;
        uint8_t *bin = of_alloc(n + 1);
        if (bin) {
            for (size_t i = 0; i < n; i++) {
                char hx[3] = { R->hex.s[2 * i], R->hex.s[2 * i + 1], 0 };
                bin[i] = (uint8_t)strtol(hx, NULL, 16);
            }
            char name[48], path[300];
            snprintf(name, sizeof name, "rtf%d.%s", ++R->pics, R->pic_type == 1 ? "png" : "jpg");
            /* the size it is shown at: \picwgoal in twips, scaled */
            int w = R->picwg ? R->picwg / 20 : R->picw * 3 / 4;
            int h = R->pichg ? R->pichg / 20 : R->pich * 3 / 4;
            if (R->scx > 0) w = w * R->scx / 100;
            if (R->scy > 0) h = h * R->scy / 100;
            if (w > 500) { h = h * 500 / w; w = 500; }
            if (of_cache_file(name, bin, n, path, sizeof path)) {
                ensure_cell(R);
                of_bld_image(&R->b, path, true, w, h);
            }
            of_free(bin);
        }
    }
    R->hex.n = 0;
    R->pic_type = 0;
}

static void group_open(rtf_t *R)
{
    flush(R);
    if (R->ng >= 64) { R->skip_depth++; return; }
    R->g[R->ng] = R->g[R->ng - 1];
    R->g[R->ng].is_field = false;   /* nested groups keep the destination */
    R->ng++;
}

static void group_close(rtf_t *R)
{
    if (R->skip_depth) { R->skip_depth--; return; }
    flush(R);
    if (R->ng <= 1) return;
    gst_t *g = G;
    gst_t *up = &R->g[R->ng - 2];
    if (g->dest == DS_PICT && up->dest != DS_PICT) picture_done(R);
    if (g->dest == DS_FOOTNOTE && up->dest != DS_FOOTNOTE) { put_text(R, "]", 1); flush(R); }
    if (g->dest == DS_FLDINST && up->dest != DS_FLDINST) {
        /* HYPERLINK "url" */
        const char *s = R->inst.s ? R->inst.s : "";
        const char *h = strstr(s, "HYPERLINK");
        R->url[0] = 0;
        if (h) {
            const char *q = strchr(h, '"');
            const char *z = q ? strchr(q + 1, '"') : NULL;
            if (q && z) snprintf(R->url, sizeof R->url, "%.*s", (int)(z - q - 1), q + 1);
        }
        R->inst.n = 0;
    }
    if (g->dest == DS_STYLES && up->dest == DS_STYLES && R->sname.n) {
        /* "heading 1;" */
        char nm[64];
        snprintf(nm, sizeof nm, "%.*s", (int)R->sname.n, R->sname.s);
        char *sc = strchr(nm, ';');
        if (sc) *sc = 0;
        char *p = nm;
        while (*p == ' ') p++;
        if (R->cur_s > 0 && R->cur_s < 256) {
            if (!strncasecmp(p, "heading ", 8)) R->style_h[R->cur_s] = (int8_t)atoi(p + 8);
            else if (!strcasecmp(p, "title")) R->style_h[R->cur_s] = 1;
        }
        R->sname.n = 0;
        R->cur_s = 0;
    }
    bool was_field = g->is_field;
    R->ng--;
    if (was_field) R->url[0] = 0;
    (void)up;
}

/* control words that start a destination to skip */
static const char *const SKIP[] = {
    "fonttbl", "filetbl", "info", "header", "headerl", "headerr", "headerf", "footer", "footerl", "footerr", "footerf",
    "listtable", "listoverridetable", "revtbl", "rsidtbl", "generator", "themedata", "colorschememapping",
    "latentstyles", "datastore", "xmlnstbl", "mmathPr", "pgdsctbl", "nonshppict", "objdata", "pn", "pntxta", "pntxtb",
    "bkmkstart", "bkmkend", "fldtype", "datafield", "shpinst", "sp", "picprop", "blipuid", "annotation", "atnid",
    "atnauthor", "ftnsep", "ftnsepc", "aftnsep", "aftnsepc", "template", "userprops", "docvar", "wgrffmtfilter",
    "xmlopen", "xmlclose", "pgptbl", "listpicture", "ud",
};

static void control(rtf_t *R, const char *w, bool has, int v, bool star)
{
    gst_t *g = G;
    if (R->skip_depth || g->dest == DS_SKIP) return;
    /* colour table and style sheet arguments */
    if (g->dest == DS_COLORS) {
        if (!strcmp(w, "red")) { R->cr = v; R->cset = true; }
        else if (!strcmp(w, "green")) { R->cg = v; R->cset = true; }
        else if (!strcmp(w, "blue")) { R->cbl = v; R->cset = true; }
        return;
    }
    if (g->dest == DS_STYLES) {
        if (!strcmp(w, "s") && has) R->cur_s = v;
        else if (!strcmp(w, "cs") || !strcmp(w, "ds") || !strcmp(w, "ts")) { R->cur_s = 0; g->dest = DS_SKIP; }
        return;
    }
    if (g->dest == DS_PICT) {
        if (!strcmp(w, "pngblip")) R->pic_type = 1;
        else if (!strcmp(w, "jpegblip")) R->pic_type = 2;
        else if (!strcmp(w, "emfblip") || !strcmp(w, "wmetafile") || !strcmp(w, "macpict") || !strcmp(w, "dibitmap")) R->pic_type = 0;
        else if (!strcmp(w, "picw")) R->picw = v;
        else if (!strcmp(w, "pich")) R->pich = v;
        else if (!strcmp(w, "picwgoal")) R->picwg = v;
        else if (!strcmp(w, "pichgoal")) R->pichg = v;
        else if (!strcmp(w, "picscalex")) R->scx = v;
        else if (!strcmp(w, "picscaley")) R->scy = v;
        return;
    }
    if (g->dest == DS_FLDINST) return;

    /* destinations */
    if (!strcmp(w, "colortbl")) { g->dest = DS_COLORS; R->ncolors = 0; R->cset = false; return; }
    if (!strcmp(w, "stylesheet")) { g->dest = DS_STYLES; return; }
    if (!strcmp(w, "pict")) {
        g->dest = DS_PICT;
        R->pic_type = 0; R->picw = R->pich = R->picwg = R->pichg = R->scx = R->scy = 0;
        R->hex.n = 0;
        return;
    }
    if (!strcmp(w, "field")) { g->is_field = true; return; }
    if (!strcmp(w, "fldinst")) { g->dest = DS_FLDINST; R->inst.n = 0; return; }
    if (!strcmp(w, "fldrslt")) {
        g->dest = DS_FLDRSLT;
        if (R->url[0]) { of_bld_link(&R->b, R->url); g->st.link = R->b.st.link; }
        return;
    }
    if (!strcmp(w, "footnote")) { g->dest = DS_FOOTNOTE; put_text(R, " [", 2); return; }
    if (!strcmp(w, "shppict") || !strcmp(w, "shp") || !strcmp(w, "shprslt") || !strcmp(w, "result") || !strcmp(w, "listtext") ||
        !strcmp(w, "pntext"))
        return;
    for (size_t i = 0; i < sizeof SKIP / sizeof SKIP[0]; i++)
        if (!strcmp(w, SKIP[i])) { g->dest = DS_SKIP; return; }
    if (star) {
        /* an ignorable destination this reader does not know */
        g->dest = DS_SKIP;
        return;
    }

    /* characters */
    if (!strcmp(w, "par") || !strcmp(w, "sect")) {
        flush(R);
        if (g->dest == DS_FOOTNOTE) { put_text(R, " ", 1); return; }
        ensure_cell(R);
        para_props(R);
        /* an empty paragraph stays, as a blank line */
        if (R->b.cur >= 0 && !R->b.d->p[R->b.cur].len && !R->intbl) {
            int keep = R->b.cur;
            of_bld_para(&R->b);
            if (R->b.cur == keep) {
                of_para_t *p = of_doc_insert_para(R->b.d, R->b.d->np);
                if (p) { R->b.cur = R->b.d->np - 1; p->cell = R->b.cell; }
            }
        } else {
            R->b.cur = -1;
            of_bld_para(&R->b);
        }
        return;
    }
    if (!strcmp(w, "line")) {
        flush(R);
        ensure_cell(R);
        if (R->b.cur < 0) of_bld_para(&R->b);
        if (R->b.cur >= 0) of_para_append(R->b.d, &R->b.d->p[R->b.cur], "\n", 1, (uint16_t)of_doc_style(R->b.d, &R->b.st));
        R->b.space = false;
        return;
    }
    if (!strcmp(w, "tab")) { put_text(R, " ", 1); return; }
    if (!strcmp(w, "page")) { flush(R); of_bld_break(&R->b); return; }
    if (!strcmp(w, "cell") || !strcmp(w, "nestcell")) {
        flush(R);
        ensure_cell(R);
        R->in_cell = false;
        return;
    }
    if (!strcmp(w, "row") || !strcmp(w, "nestrow")) { flush(R); R->in_row = false; R->in_cell = false; return; }
    if (!strcmp(w, "u") && has) {
        put_cp(R, (uint32_t)(v < 0 ? v + 65536 : v));    /* the fallback is skipped by the caller */
        return;
    }
    static const struct { const char *w; uint32_t cp; } CH[] = {
        { "emdash", 0x2014 }, { "endash", 0x2013 }, { "lquote", 0x2018 }, { "rquote", 0x2019 }, { "ldblquote", 0x201C },
        { "rdblquote", 0x201D }, { "bullet", 0x2022 }, { "emspace", ' ' }, { "enspace", ' ' }, { "qmspace", ' ' },
    };
    for (size_t i = 0; i < sizeof CH / sizeof CH[0]; i++)
        if (!strcmp(w, CH[i].w)) { put_cp(R, CH[i].cp); return; }

    /* character formatting */
    flush(R);
    g = G;
    uint8_t onoff = has && v == 0 ? OF_TRI_UNSET : OF_TRI_ON;
    if (!strcmp(w, "plain")) {
        int16_t lk = g->st.link;
        memset(&g->st, 0, sizeof g->st);
        g->st.link = lk;
        g->cf = g->cb = g->hl = 0;
    }
    else if (!strcmp(w, "b")) g->st.b = onoff;
    else if (!strcmp(w, "i")) g->st.i = onoff;
    else if (!strcmp(w, "ul") || !strcmp(w, "uld") || !strcmp(w, "uldb") || !strcmp(w, "ulw") || !strcmp(w, "ulth")) g->st.u = onoff;
    else if (!strcmp(w, "ulnone")) g->st.u = OF_TRI_UNSET;
    else if (!strcmp(w, "strike") || !strcmp(w, "striked")) g->st.s = onoff;
    else if (!strcmp(w, "fs") && has) g->st.half_pt = (uint16_t)(v == 24 ? 0 : v);
    else if (!strcmp(w, "cf")) g->cf = v;
    else if (!strcmp(w, "cb") || !strcmp(w, "chcbpat")) g->cb = v;
    else if (!strcmp(w, "highlight")) g->hl = v;
    else if (!strcmp(w, "super") || (!strcmp(w, "up") && v > 0)) g->st.sup = 1;
    else if (!strcmp(w, "sub") || (!strcmp(w, "dn") && v > 0)) g->st.sup = 2;
    else if (!strcmp(w, "nosupersub")) g->st.sup = 0;
    else if (!strcmp(w, "uc") && has) g->uc = v;
    /* paragraph formatting */
    else if (!strcmp(w, "pard")) { R->align = 0; R->heading = 0; R->intbl = false; }
    else if (!strcmp(w, "ql")) R->align = OF_AL_LEFT;
    else if (!strcmp(w, "qc")) R->align = OF_AL_CENTER;
    else if (!strcmp(w, "qr")) R->align = OF_AL_RIGHT;
    else if (!strcmp(w, "qj")) R->align = OF_AL_JUSTIFY;
    else if (!strcmp(w, "s") && has) R->heading = v > 0 && v < 256 && R->style_h[v] > 0 ? R->style_h[v] : 0;
    else if (!strcmp(w, "outlinelevel") && has) R->heading = v >= 0 && v < 9 ? v + 1 : 0;
    else if (!strcmp(w, "intbl")) R->intbl = true;
    else if (!strcmp(w, "itap")) R->intbl = v > 0;
}

of_doc_t *of_rtf_load(const char *path, char *err, size_t errlen)
{
    size_t n;
    char *s = (char *)of_file_read(path, &n, 48u * 1024 * 1024);
    if (!s) { snprintf(err, errlen, "read"); return NULL; }
    if (n < 5 || strncmp(s, "{\\rtf", 5)) { of_free(s); snprintf(err, errlen, "format"); return NULL; }
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    rtf_t *R = of_alloc(sizeof *R);
    if (!d || !R) { of_free(s); of_doc_free(d); of_free(R); snprintf(err, errlen, "memory"); return NULL; }
    of_bld_init(&R->b, d);
    R->ng = 1;
    R->g[0].uc = 1;
    R->g[0].st.link = -1;
    size_t skip_chars = 0;      /* \uN fallback still to skip */
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '{') { skip_chars = 0; group_open(R); continue; }
        if (c == '}') { skip_chars = 0; group_close(R); continue; }
        if (c == '\r' || c == '\n') continue;
        if (c != '\\') {
            if (skip_chars) { skip_chars--; continue; }
            size_t j = i;
            while (j < n && s[j] != '\\' && s[j] != '{' && s[j] != '}' && s[j] != '\r' && s[j] != '\n') j++;
            put_text(R, s + i, j - i);
            i = j - 1;
            continue;
        }
        /* a control */
        if (i + 1 >= n) break;
        char d1 = s[i + 1];
        if (d1 == '\'' && i + 3 < n) {
            char hx[3] = { s[i + 2], s[i + 3], 0 };
            i += 3;
            if (skip_chars) { skip_chars--; continue; }
            uint8_t byte = (uint8_t)strtol(hx, NULL, 16);
            size_t m;
            char *u = of_latin1_to_utf8((const char *)&byte, 1, &m);
            if (u) { put_text(R, u, m); of_free(u); }
            continue;
        }
        if (d1 == '\\' || d1 == '{' || d1 == '}') { i++; if (skip_chars) { skip_chars--; continue; } put_text(R, &s[i], 1); continue; }
        if (d1 == '~') { i++; put_text(R, " ", 1); continue; }
        if (d1 == '_') { i++; put_text(R, "-", 1); continue; }
        if (d1 == '-' || d1 == ':' || d1 == '|') { i++; continue; }
        if (d1 == '\r' || d1 == '\n') { i++; control(R, "par", false, 0, false); continue; }
        bool star = false;
        if (d1 == '*') {
            star = true;
            i += 2;
            while (i < n && (s[i] == ' ' || s[i] == '\r' || s[i] == '\n')) i++;
            if (i >= n || s[i] != '\\') { i--; G->dest = DS_SKIP; continue; }
            d1 = i + 1 < n ? s[i + 1] : 0;
        }
        if (!((d1 >= 'a' && d1 <= 'z') || (d1 >= 'A' && d1 <= 'Z'))) { i++; continue; }
        char w[32];
        size_t k = 0, j = i + 1;
        while (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z'))) { if (k < sizeof w - 1) w[k++] = s[j]; j++; }
        w[k] = 0;
        bool has = false;
        int v = 0, sign = 1;
        if (j < n && s[j] == '-') { sign = -1; j++; }
        while (j < n && s[j] >= '0' && s[j] <= '9') { v = v * 10 + (s[j] - '0'); has = true; j++; }
        v *= sign;
        if (j < n && s[j] == ' ') j++;
        i = j - 1;
        if (!strcmp(w, "bin") && has) {
            /* raw bytes: as hex into a picture, or skipped */
            size_t len = (size_t)(v > 0 ? v : 0);
            if (i + 1 + len > n) len = n - i - 1;
            if (G->dest == DS_PICT && !R->skip_depth)
                for (size_t q = 0; q < len; q++) of_sb_printf(&R->hex, "%02x", (uint8_t)s[i + 1 + q]);
            i += len;
            continue;
        }
        if (skip_chars && strcmp(w, "u")) { skip_chars--; continue; }
        control(R, w, has, v, star);
        if (!strcmp(w, "u") && has && !R->skip_depth && G->dest != DS_SKIP) skip_chars = (size_t)(G->uc > 0 ? G->uc : 0);
    }
    flush(R);
    of_bld_done(&R->b);
    snprintf(d->kind, sizeof d->kind, "RTF");
    of_sb_free(&R->pend);
    of_sb_free(&R->hex);
    of_sb_free(&R->inst);
    of_sb_free(&R->sname);
    of_free(R);
    of_free(s);
    return d;
}
