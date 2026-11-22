/*
 * P4OS - Office: ODS (OpenDocument spreadsheets).
 *
 * The cells are in content.xml: tables of rows of cells, where a row or a
 * cell can stand for many identical ones (number-rows-repeated,
 * number-columns-repeated: a sheet usually ends in one empty row repeated a
 * million times). A cell has its value and type as attributes, its formula
 * in "of:" syntax (translated to Excel's and back), and the text the program
 * showed in its <text:p>, which the grid shows as it is while the cell keeps
 * its value.
 *
 * Saving into the same file copies the package and rewrites content.xml
 * only, walking the original again: a table with no changes goes out as it
 * was; in a changed one, a run of rows with no changes is copied, and a run
 * with some is split so that the changed rows stand alone; inside a changed
 * row the same happens to its runs of cells. A rewritten cell keeps its
 * style, its spans and whatever else it carried (comments) but its value,
 * its formula and its paragraphs. Nothing else in the file changes.
 *
 * A new file (from a CSV, an XLSX, or new) is a minimal package.
 */
#include "of.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ODS_MAX (32u * 1024 * 1024)

char *of_excel_to_odf_book(const char *src, const of_book_t *b, int sheet_ix);

static int sty_index(of_book_t *b, const char *name)
{
    if (!name || !*name) return 0;
    for (int i = 0; i < b->nsty; i++)
        if (!strcmp(b->sty[i], name)) return i;
    char **n = of_realloc(b->sty, (size_t)(b->nsty + 1) * sizeof *n);
    if (!n) return 0;
    b->sty = n;
    b->sty[b->nsty] = of_strdup(name);
    return b->nsty++;
}

static int attr_int(of_xml_t *X, const char *name, int def)
{
    char v[24];
    if (!of_xml_attr(X, name, v, sizeof v)) return def;
    int k = atoi(v);
    return k > 0 ? k : def;
}

/* "2024-01-15" or "2024-01-15T10:30:00" to a serial */
static double iso_serial(const char *s, int *hint)
{
    int y = 0, m = 0, d = 0, hh = 0, mi = 0;
    double ss = 0;
    int n = sscanf(s, "%d-%d-%dT%d:%d:%lf", &y, &m, &d, &hh, &mi, &ss);
    *hint = n > 3 ? OH_DATETIME : OH_DATE;
    if (n < 3) return 0;
    return of_date_serial(y, m, d) + (hh * 3600 + mi * 60 + ss) / 86400.0;
}

/* "PT12H30M00S" */
static double dur_days(const char *s)
{
    double h = 0, m = 0, sec = 0, cur = 0;
    for (const char *p = s; *p; p++) {
        if ((*p >= '0' && *p <= '9') || *p == '.') {
            char *end;
            cur = strtod(p, &end);
            p = end - 1;
        } else if (*p == 'H') h = cur;
        else if (*p == 'M') m = cur;
        else if (*p == 'S') sec = cur;
    }
    return (h * 3600 + m * 60 + sec) / 86400.0;
}

/* -------------------------------------------------------------------------- */
/* Number formats: ODF's data styles, turned into Excel format codes           */
/* -------------------------------------------------------------------------- */

typedef struct {
    char  name[48];
    char *code;
    char  map_pos[48];          /* style:map value()>=0 -> this one is the negative section */
    char  map_neg[48];          /* style:map value()<0  -> that one is the negative section */
} dstyle_t;

typedef struct {
    char name[48], data[48];
} cstyle_t;

static void lit(of_sb_t *o, const char *s)
{
    if (!*s) return;
    of_sb_putc(o, '"');
    for (; *s; s++)
        if (*s != '"') of_sb_putc(o, *s);
    of_sb_putc(o, '"');
}

/* The data style element just opened, to its end. */
static void read_dstyle(of_xml_t *X, dstyle_t *d)
{
    of_sb_t o = { 0 };
    bool pct = of_xml_is(X, "number:percentage-style");
    bool text_style = of_xml_is(X, "number:text-style");
    int depth = 1, t;
    char v[48];
    while (depth > 0 && (t = of_xml_next(X)) != OX_END && t != OX_ERROR) {
        if (t == OX_CLOSE) { depth--; continue; }
        if (t != OX_OPEN && t != OX_EMPTY) continue;
        bool open = t == OX_OPEN;
        if (of_xml_is(X, "number:number") || of_xml_is(X, "number:scientific-number")) {
            int dp = of_xml_attr(X, "number:decimal-places", v, sizeof v) ? atoi(v) : 0;
            int mdp = of_xml_attr(X, "number:min-decimal-places", v, sizeof v) ? atoi(v) : dp;
            int mi = of_xml_attr(X, "number:min-integer-digits", v, sizeof v) ? atoi(v) : 1;
            bool grp = of_xml_attr(X, "number:grouping", v, sizeof v) && !strcmp(v, "true");
            if (grp) of_sb_puts(&o, mi > 0 ? "#,##0" : "#,###");
            else if (mi <= 0) of_sb_putc(&o, '#');
            else for (int i = 0; i < mi && i < 12; i++) of_sb_putc(&o, '0');
            if (dp > 0) {
                of_sb_putc(&o, '.');
                for (int i = 0; i < dp && i < 15; i++) of_sb_putc(&o, i < mdp ? '0' : '#');
            }
            if (of_xml_is(X, "number:scientific-number")) {
                int ed = of_xml_attr(X, "number:min-exponent-digits", v, sizeof v) ? atoi(v) : 2;
                of_sb_puts(&o, "E+");
                for (int i = 0; i < ed && i < 4; i++) of_sb_putc(&o, '0');
            }
        } else if (of_xml_is(X, "number:fraction")) {
            of_sb_puts(&o, "# ?/?");
        } else if (of_xml_is(X, "number:text") && open) {
            of_sb_t tx = { 0 };
            of_xml_text(X, &tx);
            const char *s2 = tx.s ? tx.s : "";
            if (pct && !strcmp(s2, "%")) of_sb_putc(&o, '%');
            else lit(&o, s2);
            of_sb_free(&tx);
            continue;
        } else if (of_xml_is(X, "number:currency-symbol") && open) {
            of_sb_t tx = { 0 };
            of_xml_text(X, &tx);
            lit(&o, tx.s ? tx.s : "$");
            of_sb_free(&tx);
            continue;
        } else if (of_xml_is(X, "number:text-content")) {
            of_sb_putc(&o, '@');
        } else if (of_xml_is(X, "number:day")) {
            bool lng = of_xml_attr(X, "number:style", v, sizeof v) && !strcmp(v, "long");
            of_sb_puts(&o, lng ? "dd" : "d");
        } else if (of_xml_is(X, "number:month")) {
            bool lng = of_xml_attr(X, "number:style", v, sizeof v) && !strcmp(v, "long");
            bool txt = of_xml_attr(X, "number:textual", v, sizeof v) && !strcmp(v, "true");
            of_sb_puts(&o, txt ? (lng ? "mmmm" : "mmm") : (lng ? "mm" : "m"));
        } else if (of_xml_is(X, "number:year")) {
            bool lng = of_xml_attr(X, "number:style", v, sizeof v) && !strcmp(v, "long");
            of_sb_puts(&o, lng ? "yyyy" : "yy");
        } else if (of_xml_is(X, "number:day-of-week")) {
            bool lng = of_xml_attr(X, "number:style", v, sizeof v) && !strcmp(v, "long");
            of_sb_puts(&o, lng ? "dddd" : "ddd");
        } else if (of_xml_is(X, "number:hours")) {
            bool lng = of_xml_attr(X, "number:style", v, sizeof v) && !strcmp(v, "long");
            of_sb_puts(&o, lng ? "hh" : "h");
        } else if (of_xml_is(X, "number:minutes")) {
            bool lng = of_xml_attr(X, "number:style", v, sizeof v) && !strcmp(v, "long");
            of_sb_puts(&o, lng ? "mm" : "m");
        } else if (of_xml_is(X, "number:seconds")) {
            bool lng = of_xml_attr(X, "number:style", v, sizeof v) && !strcmp(v, "long");
            of_sb_puts(&o, lng ? "ss" : "s");
        } else if (of_xml_is(X, "number:am-pm")) {
            of_sb_puts(&o, "AM/PM");
        } else if (of_xml_is(X, "style:map")) {
            char cond[64] = "", ap[48] = "";
            of_xml_attr(X, "style:condition", cond, sizeof cond);
            of_xml_attr(X, "style:apply-style-name", ap, sizeof ap);
            if (strstr(cond, ">=0") || strstr(cond, ">0")) snprintf(d->map_pos, sizeof d->map_pos, "%s", ap);
            else if (strstr(cond, "<0")) snprintf(d->map_neg, sizeof d->map_neg, "%s", ap);
        }
        if (open) {
            /* an element with children we do not need (text properties) */
            if (!of_xml_is(X, "number:number")) of_xml_skip(X);
            else depth++;
        }
    }
    if (text_style && !o.n) of_sb_putc(&o, '@');
    d->code = of_sb_take(&o);
}

static const dstyle_t *dfind(const dstyle_t *d, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(d[i].name, name)) return &d[i];
    return NULL;
}

/* Each cell style's format, with the positive and negative sections joined. */
static void resolve_codes(of_book_t *b, const dstyle_t *ds, int nds, const cstyle_t *cs, int ncs)
{
    if (!b->nsty) return;
    b->sty_code = of_alloc((size_t)b->nsty * sizeof(char *));
    if (!b->sty_code) return;
    for (int i = 0; i < b->nsty; i++) {
        const char *data = NULL;
        for (int k = 0; k < ncs; k++)
            if (!strcmp(cs[k].name, b->sty[i])) data = cs[k].data;
        if (!data || !*data) continue;
        const dstyle_t *d = dfind(ds, nds, data);
        if (!d || !d->code) continue;
        const dstyle_t *pos = d->map_pos[0] ? dfind(ds, nds, d->map_pos) : NULL;
        const dstyle_t *neg = d->map_neg[0] ? dfind(ds, nds, d->map_neg) : NULL;
        of_sb_t o = { 0 };
        if (pos && pos->code) {
            of_sb_puts(&o, pos->code);
            of_sb_putc(&o, ';');
            /* the negative section writes its own minus sign */
            of_sb_puts(&o, d->code);
        } else if (neg && neg->code) {
            of_sb_puts(&o, d->code);
            of_sb_putc(&o, ';');
            of_sb_puts(&o, neg->code);
        } else {
            of_sb_puts(&o, d->code);
        }
        b->sty_code[i] = of_sb_take(&o);
    }
}

/* -------------------------------------------------------------------------- */
/* Reading                                                                     */
/* -------------------------------------------------------------------------- */

/* the automatic column styles' widths, in characters */
typedef struct { char name[32]; float w; } colsty_t;

static float len_chars(const char *v)
{
    char *end;
    float x = strtof(v, &end);
    float pt = x;
    if (!strncmp(end, "cm", 2)) pt = x * 28.3465f;
    else if (!strncmp(end, "mm", 2)) pt = x * 2.83465f;
    else if (!strncmp(end, "in", 2)) pt = x * 72;
    else if (!strncmp(end, "pc", 2)) pt = x * 12;
    return pt / 5.25f;
}

static void read_cell(of_book_t *b, int ix, of_xml_t *X, int t, int32_t row, int32_t col, int rep, bool *content)
{
    of_sheet_t *s = &b->sh[ix];
    char ty[24] = "", val[64] = "", sty[64] = "";
    of_xml_attr(X, "office:value-type", ty, sizeof ty);
    of_xml_attr(X, "table:style-name", sty, sizeof sty);
    char *formula = NULL;
    {
        const char *v;
        int vl;
        if (X->attrs && of_xml_attr_raw(X->attrs, X->attrs_len, "table:formula", &v, &vl)) {
            of_sb_t f = { 0 };
            of_xml_unescape(v, (size_t)vl, &f);
            formula = of_odf_to_excel(f.s ? f.s : "");
            of_sb_free(&f);
        }
    }
    of_val_t cv = { 0 };
    int hint = 0;
    if (!strcmp(ty, "float") || !strcmp(ty, "percentage") || !strcmp(ty, "currency")) {
        of_xml_attr(X, "office:value", val, sizeof val);
        cv.t = OV_NUM;
        cv.n = strtod(val, NULL);
        hint = ty[0] == 'p' ? OH_PCT : ty[0] == 'c' ? OH_CUR : 0;
    } else if (!strcmp(ty, "date")) {
        of_xml_attr(X, "office:date-value", val, sizeof val);
        cv.t = OV_NUM;
        cv.n = iso_serial(val, &hint);
    } else if (!strcmp(ty, "time")) {
        of_xml_attr(X, "office:time-value", val, sizeof val);
        cv.t = OV_NUM;
        cv.n = dur_days(val);
        hint = OH_TIME;
    } else if (!strcmp(ty, "boolean")) {
        of_xml_attr(X, "office:boolean-value", val, sizeof val);
        cv.t = OV_BOOL;
        cv.n = !strcmp(val, "true");
    }
    /* the paragraphs */
    of_sb_t text = { 0 };
    if (t == OX_OPEN) {
        int d = 1, tt;
        int paras = 0;
        while (d > 0 && (tt = of_xml_next(X)) != OX_END && tt != OX_ERROR) {
            if (tt == OX_OPEN && of_xml_is(X, "text:p")) {
                if (paras++) of_sb_putc(&text, '\n');
                of_xml_text(X, &text);
                continue;
            }
            if (tt == OX_OPEN && of_xml_is(X, "office:annotation")) { of_xml_skip(X); continue; }
            if (tt == OX_OPEN) d++;
            else if (tt == OX_CLOSE) d--;
        }
    }
    if (!strcmp(ty, "string")) {
        char sv[8];
        cv.t = OV_STR;
        if (X->attrs && of_xml_attr(X, "office:string-value", sv, sizeof sv)) {}
        cv.s = of_strdup(text.s ? text.s : "");
    }
    bool has = cv.t != OV_EMPTY || formula;
    if (has) {
        *content = true;
        if (rep > 256) rep = 256;          /* a value repeated across the whole row: keep the first ones */
        for (int k = 0; k < rep; k++) {
            of_cell_t *c = of_cell_make(s, row, col + k);
            if (!c) break;
            of_val_copy(&c->cached, &cv);
            of_val_copy(&c->v, &cv);
            c->hint = (uint8_t)hint;
            c->style = (uint32_t)sty_index(b, sty);
            if (text.s && cv.t != OV_STR) c->disp = of_strdup(text.s);
            if (formula) {
                c->f = of_strdup(formula);
                c->flags |= OC_FORMULA;
            }
        }
    }
    of_val_free(&cv);
    of_sb_free(&text);
    of_free(formula);
}

of_book_t *of_ods_load(const char *path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *raw = of_file_read(path, &len, ODS_MAX);
    if (!raw) { snprintf(err, errlen, "read"); return NULL; }
    of_book_t *b = of_book_new(OF_FMT_ODS);
    if (!b) { of_free(raw); return NULL; }
    b->file = raw;
    b->file_len = len;
    char *content = NULL;
    size_t cl = 0;
    bool flat = !strcmp(of_ext(path), "fods");
    if (flat) {
        content = of_strndup((const char *)raw, len);
        cl = len;
    } else {
        if (of_is_encrypted_ooxml(raw, len)) { snprintf(err, errlen, "encrypted"); of_book_free(b); return NULL; }
        if (!of_zip_open(&b->zip, raw, len)) { snprintf(err, errlen, "zip"); of_book_free(b); return NULL; }
        int i = of_zip_find(&b->zip, "content.xml");
        if (i >= 0) content = (char *)of_zip_read(&b->zip, i, &cl);
        /* an encrypted ODF has a manifest entry with encryption-data */
        int m = of_zip_find(&b->zip, "META-INF/manifest.xml");
        if (m >= 0) {
            size_t ml;
            char *man = (char *)of_zip_read(&b->zip, m, &ml);
            if (man && strstr(man, "encryption-data")) {
                of_free(man);
                of_free(content);
                snprintf(err, errlen, "encrypted");
                of_book_free(b);
                return NULL;
            }
            of_free(man);
        }
    }
    if (!content) { snprintf(err, errlen, "content"); of_book_free(b); return NULL; }
    sty_index(b, "");           /* index 0: no style */
    colsty_t *cs = NULL;
    int ncs = 0;
    dstyle_t *ds = NULL;
    int nds = 0;
    cstyle_t *cst = NULL;
    int ncst = 0;
    of_xml_t X;
    of_xml_init(&X, content, cl);
    int t, ix = -1;
    int32_t row = 0;
    char cur_style[32] = "";
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_CLOSE && of_xml_is(&X, "table:table")) { ix = -1; continue; }
        if (t != OX_OPEN && t != OX_EMPTY) continue;
        if (of_xml_is(&X, "style:style")) {
            char fam[24] = "";
            of_xml_attr(&X, "style:family", fam, sizeof fam);
            cur_style[0] = 0;
            if (!strcmp(fam, "table-column")) of_xml_attr(&X, "style:name", cur_style, sizeof cur_style);
            if (!strcmp(fam, "table-cell")) {
                char dn[48];
                if (of_xml_attr(&X, "style:data-style-name", dn, sizeof dn)) {
                    cstyle_t *n = of_realloc(cst, (size_t)(ncst + 1) * sizeof *n);
                    if (n) {
                        cst = n;
                        memset(&cst[ncst], 0, sizeof *cst);
                        of_xml_attr(&X, "style:name", cst[ncst].name, sizeof cst[ncst].name);
                        snprintf(cst[ncst].data, sizeof cst[ncst].data, "%s", dn);
                        ncst++;
                    }
                }
            }
        } else if (t == OX_OPEN && X.name_len > 13 && !strncmp(X.name, "number:", 7) && !strncmp(X.name + X.name_len - 6, "-style", 6)) {
            dstyle_t *n = of_realloc(ds, (size_t)(nds + 1) * sizeof *n);
            if (n) {
                ds = n;
                memset(&ds[nds], 0, sizeof *ds);
                of_xml_attr(&X, "style:name", ds[nds].name, sizeof ds[nds].name);
                read_dstyle(&X, &ds[nds]);
                nds++;
            }
        } else if (of_xml_is(&X, "style:table-column-properties") && cur_style[0]) {
            char w[32];
            if (of_xml_attr(&X, "style:column-width", w, sizeof w)) {
                colsty_t *n = of_realloc(cs, (size_t)(ncs + 1) * sizeof *n);
                if (n) {
                    cs = n;
                    snprintf(cs[ncs].name, sizeof cs[ncs].name, "%s", cur_style);
                    cs[ncs].w = len_chars(w);
                    ncs++;
                }
            }
        } else if (of_xml_is(&X, "table:table") && t == OX_OPEN) {
            char name[128] = "";
            of_xml_attr(&X, "table:name", name, sizeof name);
            of_sheet_t *s = of_book_add_sheet(b, name);
            ix = s ? b->nsh - 1 : -1;
            row = 0;
            if (s) {
                char disp[16];
                if (of_xml_attr(&X, "table:display", disp, sizeof disp) && !strcmp(disp, "false")) s->state = 1;
            }
            int32_t colpos = 0;
            /* columns and rows, at any depth (header rows, groups) */
            int d = 1, tt;
            while (ix >= 0 && d > 0 && (tt = of_xml_next(&X)) != OX_END && tt != OX_ERROR) {
                if (tt == OX_CLOSE) { d--; continue; }
                if (tt != OX_OPEN && tt != OX_EMPTY) continue;
                if (of_xml_is(&X, "table:table-column")) {
                    int rep = attr_int(&X, "table:number-columns-repeated", 1);
                    char st[32] = "";
                    of_xml_attr(&X, "table:style-name", st, sizeof st);
                    for (int k = 0; k < ncs; k++)
                        if (!strcmp(cs[k].name, st)) {
                            for (int c = 0; c < rep && colpos + c < 1024; c++)
                                of_sheet_set_colw(&b->sh[ix], colpos + c, cs[k].w);
                        }
                    colpos += rep;
                    if (tt == OX_OPEN) d++;
                    continue;
                }
                if (of_xml_is(&X, "table:table-row")) {
                    int rep = attr_int(&X, "table:number-rows-repeated", 1);
                    if (tt == OX_EMPTY) { row += rep; continue; }
                    /* read the cells of the first; copy them to the repeats if it has any */
                    int32_t col = 0;
                    bool content_found = false;
                    int t3;
                    while ((t3 = of_xml_next(&X)) != OX_END && t3 != OX_ERROR) {
                        if (t3 == OX_CLOSE && of_xml_is(&X, "table:table-row")) break;
                        if (t3 != OX_OPEN && t3 != OX_EMPTY) continue;
                        if (of_xml_is(&X, "table:table-cell") || of_xml_is(&X, "table:covered-table-cell")) {
                            int crep = attr_int(&X, "table:number-columns-repeated", 1);
                            if (of_xml_is(&X, "table:table-cell")) read_cell(b, ix, &X, t3, row, col, crep, &content_found);
                            else if (t3 == OX_OPEN) of_xml_skip(&X);
                            col += crep;
                        } else if (t3 == OX_OPEN) {
                            of_xml_skip(&X);
                        }
                    }
                    if (content_found && rep > 1) {
                        of_row_t *src = of_row_get(&b->sh[ix], row);
                        int copies = rep > 1000 ? 1000 : rep;
                        for (int k = 1; src && k < copies; k++) {
                            for (int c = 0; c < src->n; c++) {
                                of_cell_t *from = &of_row_get(&b->sh[ix], row)->c[c];
                                of_cell_t tmp = *from;
                                of_cell_t *to = of_cell_make(&b->sh[ix], row + k, tmp.col);
                                if (!to) break;
                                from = of_cell_get(&b->sh[ix], row, tmp.col);
                                of_val_copy(&to->v, &from->v);
                                of_val_copy(&to->cached, &from->cached);
                                to->style = from->style;
                                to->hint = from->hint;
                                to->disp = of_strdup(from->disp);
                                if (from->f) {
                                    to->f = of_formula_shift(from->f, b, ix, k, 0);
                                    to->flags |= OC_FORMULA;
                                }
                            }
                            src = of_row_get(&b->sh[ix], row);
                        }
                    }
                    row += rep;
                    continue;
                }
                if (tt == OX_OPEN) {
                    /* row groups and header rows hold rows: go in; anything else, skip */
                    if (of_xml_is(&X, "table:table-row-group") || of_xml_is(&X, "table:table-header-rows") ||
                        of_xml_is(&X, "table:table-rows") || of_xml_is(&X, "table:table-column-group") ||
                        of_xml_is(&X, "table:table-header-columns") || of_xml_is(&X, "table:table-columns"))
                        d++;
                    else
                        of_xml_skip(&X);
                }
            }
            ix = -1;
        }
    }
    of_free(cs);
    resolve_codes(b, ds, nds, cst, ncst);
    for (int i = 0; i < nds; i++) of_free(ds[i].code);
    of_free(ds);
    of_free(cst);
    if (!b->nsh) of_book_add_sheet(b, "Hoja1");
    /* the formulas, now that every sheet's name is known */
    for (int i = 0; i < b->nsh; i++)
        for (int r = 0; r < b->sh[i].nrows; r++)
            for (int k = 0; k < b->sh[i].rows[r].n; k++) {
                of_cell_t *c = &b->sh[i].rows[r].c[k];
                if (!c->f) continue;
                bool unk = false;
                c->ast = of_formula_parse(c->f, i, b, &unk);
                if (!c->ast || unk) { c->flags |= OC_UNKNOWN_F; b->any_unknown_f = true; }
            }
    for (int i = 0; i < b->nsh; i++) {
        b->sh[i].dirty = false;
        b->sh[i].cols_dirty = false;
        if (b->sh[i].colw_set) memset(b->sh[i].colw_set, 0, (size_t)b->sh[i].ncolw);
    }
    if (flat) {
        of_free(content);
        b->ods_content = NULL;
    } else {
        b->ods_content = content;
        b->ods_content_len = cl;
    }
    return b;
}

/* -------------------------------------------------------------------------- */
/* Writing a cell                                                              */
/* -------------------------------------------------------------------------- */

static void w_num(of_sb_t *o, double n)
{
    char buf[40];
    snprintf(buf, sizeof buf, "%.17g", n);
    for (int p = 15; p < 17; p++) {
        char t[40];
        snprintf(t, sizeof t, "%.*g", p, n);
        if (strtod(t, NULL) == n) { snprintf(buf, sizeof buf, "%s", t); break; }
    }
    of_sb_puts(o, buf);
}

static void w_para(of_sb_t *o, const char *s)
{
    of_sb_puts(o, "<text:p>");
    int spaces = 0;
    for (const char *p = s;; p++) {
        if (*p == ' ') { spaces++; continue; }
        if (spaces) {
            /* one space as it is, the rest as <text:s/> (and a leading one too) */
            bool lead = p - spaces == s;
            if (!lead) { of_sb_putc(o, ' '); spaces--; }
            if (spaces == 1) of_sb_puts(o, "<text:s/>");
            else if (spaces > 1) of_sb_printf(o, "<text:s text:c=\"%d\"/>", spaces);
            spaces = 0;
        }
        if (!*p) break;
        if (*p == '\n') of_sb_puts(o, "</text:p><text:p>");
        else if (*p == '\t') of_sb_puts(o, "<text:tab/>");
        else if (*p == '<') of_sb_puts(o, "&lt;");
        else if (*p == '>') of_sb_puts(o, "&gt;");
        else if (*p == '&') of_sb_puts(o, "&amp;");
        else if ((unsigned char)*p >= 0x20 || *p == '\r') of_sb_putc(o, *p);
    }
    of_sb_puts(o, "</text:p>");
}

static void iso_date(double v, int hint, char *out, size_t len)
{
    int y, m, d;
    of_serial_date(v, &y, &m, &d);
    double frac = v - (double)(long)v;
    long secs = (long)(frac * 86400 + 0.5);
    if (hint == OH_DATETIME || secs)
        snprintf(out, len, "%04d-%02d-%02dT%02ld:%02ld:%02ld", y, m, d, secs / 3600, secs / 60 % 60, secs % 60);
    else
        snprintf(out, len, "%04d-%02d-%02d", y, m, d);
}

static int cell_hint(const of_book_t *b, const of_cell_t *c)
{
    if (c->hint) return c->hint;
    if (b->fmt == OF_FMT_XLSX && c->style < (uint32_t)b->nxf) {
        const char *code = of_numfmt_code(b, b->xf[c->style].numfmt);
        if (of_numfmt_is_date(code)) return strchr(code, 'h') || strchr(code, 'H') ? OH_DATETIME : OH_DATE;
        if (code && strchr(code, '%')) return OH_PCT;
    }
    return 0;
}

/* The value attributes and the paragraphs of a cell (after its start tag's
 * name and the attributes kept). */
static void w_value(of_sb_t *o, const of_book_t *b, int sheet, const of_cell_t *c, const char *kept_children, size_t kl,
                    const char *style_name)
{
    if (style_name && *style_name) {
        of_sb_puts(o, " table:style-name=\"");
        of_sb_xml(o, style_name, strlen(style_name));
        of_sb_putc(o, '"');
    }
    if (c->f) {
        char *odf = of_excel_to_odf_book(c->f, b, sheet);
        if (odf) {
            of_sb_puts(o, " table:formula=\"");
            of_sb_xml(o, odf, strlen(odf));
            of_sb_putc(o, '"');
            of_free(odf);
        }
    }
    char disp[512];
    bool right;
    of_cell_t tmp = *c;
    tmp.disp = NULL;
    tmp.flags |= OC_EDITED;
    of_cell_display(b, &tmp, disp, sizeof disp, &right);
    int hint = cell_hint(b, c);
    switch (c->v.t) {
    case OV_NUM:
        if (hint == OH_DATE || hint == OH_DATETIME) {
            char iso[40];
            iso_date(c->v.n, hint, iso, sizeof iso);
            of_sb_printf(o, " office:value-type=\"date\" office:date-value=\"%s\"", iso);
        } else if (hint == OH_TIME) {
            long secs = (long)(c->v.n * 86400 + 0.5);
            of_sb_printf(o, " office:value-type=\"time\" office:time-value=\"PT%02ldH%02ldM%02ldS\"", secs / 3600,
                         secs / 60 % 60, secs % 60);
        } else {
            of_sb_puts(o, hint == OH_PCT ? " office:value-type=\"percentage\" office:value=\"" : " office:value-type=\"float\" office:value=\"");
            w_num(o, c->v.n);
            of_sb_putc(o, '"');
        }
        break;
    case OV_BOOL:
        of_sb_printf(o, " office:value-type=\"boolean\" office:boolean-value=\"%s\"", c->v.n ? "true" : "false");
        break;
    case OV_STR:
        if (c->f) {
            of_sb_puts(o, " office:value-type=\"string\" office:string-value=\"");
            of_sb_xml(o, c->v.s ? c->v.s : "", c->v.s ? strlen(c->v.s) : 0);
            of_sb_putc(o, '"');
        } else {
            of_sb_puts(o, " office:value-type=\"string\"");
        }
        break;
    case OV_ERR:
        /* an error result: LibreOffice writes the formula and the text */
        break;
    default:
        break;
    }
    if (c->v.t == OV_EMPTY && !kl) {
        of_sb_puts(o, "/>");
        return;
    }
    of_sb_putc(o, '>');
    if (kl) of_sb_put(o, kept_children, kl);
    if (c->v.t != OV_EMPTY) w_para(o, c->v.t == OV_STR ? (c->v.s ? c->v.s : "") : disp);
    of_sb_puts(o, "</table:table-cell>");
}

/* -------------------------------------------------------------------------- */
/* Saving into the same package                                                */
/* -------------------------------------------------------------------------- */

static bool same_val(const of_val_t *a, const of_val_t *b)
{
    if (a->t != b->t) return false;
    switch (a->t) {
    case OV_NUM: case OV_BOOL: return a->n == b->n;
    case OV_STR: return strcmp(a->s ? a->s : "", b->s ? b->s : "") == 0;
    case OV_ERR: return a->err == b->err;
    default: return true;
    }
}

static bool cell_changed(const of_cell_t *c)
{
    if (c->flags & (OC_EDITED | OC_STYLED)) return true;
    if (c->f && !(c->flags & OC_UNKNOWN_F) && !same_val(&c->v, &c->cached)) return true;
    return false;
}

static bool row_changed(const of_sheet_t *s, int32_t r)
{
    const of_row_t *row = of_row_get(s, r);
    if (!row) return false;
    if (row->dirty) {
        for (int k = 0; k < row->n; k++)
            if (cell_changed(&row->c[k])) return true;
    }
    for (int k = 0; k < row->n; k++)
        if (cell_changed(&row->c[k])) return true;
    return false;
}

/* The start tag of an element with one attribute's value replaced (or added,
 * or dropped when val is NULL). */
static void w_tag_attr(of_sb_t *o, const of_xml_t *X, const char *attr, const char *val, bool self_close)
{
    of_sb_putc(o, '<');
    of_sb_put(o, X->name, (size_t)X->name_len);
    const char *a = X->attrs;
    int al = X->attrs_len, i = 0;
    size_t nl = strlen(attr);
    while (i < al) {
        while (i < al && (a[i] == ' ' || a[i] == '\n' || a[i] == '\t' || a[i] == '\r')) i++;
        int k0 = i;
        while (i < al && a[i] != '=') i++;
        int k1 = i;
        i++;
        while (i < al && a[i] != '"' && a[i] != '\'') i++;
        if (i >= al) break;
        char q = a[i++];
        while (i < al && a[i] != q) i++;
        i++;
        if ((size_t)(k1 - k0) == nl && !strncmp(a + k0, attr, nl)) continue;
        of_sb_putc(o, ' ');
        of_sb_put(o, a + k0, (size_t)(i - k0));
    }
    if (val) of_sb_printf(o, " %s=\"%s\"", attr, val);
    of_sb_puts(o, self_close ? "/>" : ">");
}

/* An element as it was, with its repeat count set to n. */
static void w_repeated(of_sb_t *o, const char *src, size_t a, size_t z, const char *attr, int n)
{
    of_xml_t X;
    of_xml_init(&X, src + a, z - a);
    int t = of_xml_next(&X);
    char v[16];
    snprintf(v, sizeof v, "%d", n);
    w_tag_attr(o, &X, attr, n > 1 ? v : NULL, t == OX_EMPTY);
    if (t == OX_OPEN) of_sb_put(o, src + a + X.end, z - a - X.end);
}

/* The children of a cell other than its paragraphs (comments, mostly). */
static void kept_children(const char *src, size_t a, size_t z, of_sb_t *out)
{
    of_xml_t X;
    of_xml_init(&X, src + a, z - a);
    int t = of_xml_next(&X);
    if (t != OX_OPEN) return;
    int d = 1;
    while (d > 0 && (t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_OPEN && d == 1 && !of_xml_is(&X, "text:p")) {
            size_t s0 = X.start;
            of_xml_skip(&X);
            of_sb_put(out, X.src + s0, X.end - s0);
            continue;
        }
        if (t == OX_EMPTY && d == 1 && !of_xml_is(&X, "text:p")) {
            of_sb_put(out, X.src + X.start, X.end - X.start);
            continue;
        }
        if (t == OX_OPEN) { of_xml_skip(&X); continue; }   /* a text:p */
        if (t == OX_CLOSE) d--;
    }
}

static void w_new_cell(of_sb_t *o, const of_book_t *b, int sheet, const of_cell_t *c)
{
    of_sb_puts(o, "<table:table-cell");
    const char *sn = c && c->style < (uint32_t)b->nsty ? b->sty[c->style] : NULL;
    if (!c) { of_sb_puts(o, "/>"); return; }
    w_value(o, b, sheet, c, NULL, 0, sn);
}

/* A changed row: its runs of cells, split where a cell changed. */
static void w_row_cells(of_sb_t *o, const of_book_t *b, int sheet, const char *src, size_t a, size_t z, int32_t row)
{
    const of_sheet_t *s = &b->sh[sheet];
    of_xml_t X;
    of_xml_init(&X, src + a, z - a);
    int t = of_xml_next(&X);                    /* <table:table-row ...> */
    w_tag_attr(o, &X, "table:number-rows-repeated", NULL, false);
    int32_t col = 0;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_CLOSE && of_xml_is(&X, "table:table-row")) break;
        if (t != OX_OPEN && t != OX_EMPTY) {
            of_sb_put(o, X.src + X.start, X.end - X.start);
            continue;
        }
        size_t c0 = X.start;
        bool is_cell = of_xml_is(&X, "table:table-cell"), covered = of_xml_is(&X, "table:covered-table-cell");
        int rep = 1;
        if (is_cell || covered) {
            char v[16];
            if (of_xml_attr(&X, "table:number-columns-repeated", v, sizeof v)) rep = atoi(v) > 0 ? atoi(v) : 1;
        }
        if (t == OX_OPEN) of_xml_skip(&X);
        size_t c1 = X.end;
        if (!is_cell) {
            of_sb_put(o, X.src + c0, c1 - c0);
            col += covered ? rep : 0;
            continue;
        }
        /* the run [col, col + rep) */
        int32_t k = 0;
        char style[64] = "";
        {
            of_xml_t Y;
            of_xml_init(&Y, X.src + c0, c1 - c0);
            of_xml_next(&Y);
            of_xml_attr(&Y, "table:style-name", style, sizeof style);
        }
        while (k < rep) {
            /* how many unchanged from here */
            int32_t same = 0;
            while (k + same < rep) {
                const of_cell_t *c = of_cell_get(s, row, col + k + same);
                if (c && cell_changed(c)) break;
                same++;
            }
            if (same) {
                w_repeated(o, X.src, c0, c1, "table:number-columns-repeated", same);
                k += same;
                continue;
            }
            const of_cell_t *c = of_cell_get(s, row, col + k);
            /* the cell rewritten: its tag's attributes but the value ones */
            of_xml_t Y;
            of_xml_init(&Y, X.src + c0, c1 - c0);
            of_xml_next(&Y);
            of_sb_puts(o, "<table:table-cell");
            const char *at = Y.attrs;
            int al = Y.attrs_len, i = 0;
            while (i < al) {
                while (i < al && (at[i] == ' ' || at[i] == '\n' || at[i] == '\t' || at[i] == '\r')) i++;
                int k0 = i;
                while (i < al && at[i] != '=') i++;
                int k1 = i;
                i++;
                while (i < al && at[i] != '"' && at[i] != '\'') i++;
                if (i >= al) break;
                char q = at[i++];
                while (i < al && at[i] != q) i++;
                i++;
                const char *an = at + k0;
                int nl = k1 - k0;
                if ((nl > 7 && !strncmp(an, "office:", 7)) || (nl > 8 && !strncmp(an, "calcext:", 8)) ||
                    (nl == 13 && !strncmp(an, "table:formula", 13)) ||
                    (nl == 29 && !strncmp(an, "table:number-columns-repeated", 29)) ||
                    (nl == 16 && !strncmp(an, "table:style-name", 16)))
                    continue;
                of_sb_putc(o, ' ');
                of_sb_put(o, at + k0, (size_t)(i - k0));
            }
            of_sb_t kids = { 0 };
            if (rep == 1) kept_children(X.src, c0, c1, &kids);
            const char *sn = c->style && c->style < (uint32_t)b->nsty ? b->sty[c->style] : style;
            w_value(o, b, sheet, c, kids.s, kids.n, sn);
            of_sb_free(&kids);
            k++;
        }
        col += rep;
    }
    /* cells changed past the end of the row as written */
    const of_row_t *r = of_row_get(s, row);
    if (r)
        for (int k = 0; k < r->n; k++) {
            const of_cell_t *c = &r->c[k];
            if (c->col < col || !cell_changed(c)) continue;
            if (c->col > col) of_sb_printf(o, "<table:table-cell table:number-columns-repeated=\"%ld\"/>", (long)(c->col - col));
            w_new_cell(o, b, sheet, c);
            col = c->col + 1;
        }
    of_sb_puts(o, "</table:table-row>");
}

static void w_new_row(of_sb_t *o, const of_book_t *b, int sheet, int32_t row)
{
    const of_row_t *r = of_row_get(&b->sh[sheet], row);
    of_sb_puts(o, "<table:table-row>");
    int32_t col = 0;
    for (int k = 0; r && k < r->n; k++) {
        const of_cell_t *c = &r->c[k];
        if (c->v.t == OV_EMPTY && !c->f) continue;
        if (c->col > col) of_sb_printf(o, "<table:table-cell table:number-columns-repeated=\"%ld\"/>", (long)(c->col - col));
        w_new_cell(o, b, sheet, c);
        col = c->col + 1;
    }
    if (!col) of_sb_puts(o, "<table:table-cell/>");
    of_sb_puts(o, "</table:table-row>");
}

static bool sheet_changed(const of_sheet_t *s)
{
    if (s->dirty) return true;
    for (int i = 0; i < s->nrows; i++)
        for (int k = 0; k < s->rows[i].n; k++)
            if (cell_changed(&s->rows[i].c[k])) return true;
    return false;
}

static char *content_xml(const of_book_t *b, const char *src, size_t len)
{
    of_sb_t o = { 0 };
    of_xml_t X;
    of_xml_init(&X, src, len);
    int t, ix = -1;
    size_t pos = 0;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t != OX_OPEN || !of_xml_is(&X, "table:table")) continue;
        ix++;
        if (ix >= b->nsh || !sheet_changed(&b->sh[ix])) {
            of_xml_skip(&X);
            continue;
        }
        /* this table, rewritten */
        of_sb_put(&o, src + pos, X.end - pos);
        int32_t row = 0, last_needed = -1;
        const of_sheet_t *s = &b->sh[ix];
        for (int i = 0; i < s->nrows; i++)
            for (int k = 0; k < s->rows[i].n; k++)
                if (cell_changed(&s->rows[i].c[k])) last_needed = s->rows[i].row;
        int d = 1, tt;
        size_t tpos = X.end;
        while (d > 0 && (tt = of_xml_next(&X)) != OX_END && tt != OX_ERROR) {
            if (tt == OX_CLOSE) {
                d--;
                if (d == 0) {
                    /* new rows past the table's end */
                    of_sb_put(&o, src + tpos, X.start - tpos);
                    for (; row <= last_needed; row++) w_new_row(&o, b, ix, row);
                    tpos = X.start;
                }
                continue;
            }
            if (tt != OX_OPEN && tt != OX_EMPTY) continue;
            if (of_xml_is(&X, "table:table-row")) {
                size_t r0 = X.start;
                char v[16];
                int rep = 1;
                if (of_xml_attr(&X, "table:number-rows-repeated", v, sizeof v)) rep = atoi(v) > 0 ? atoi(v) : 1;
                if (tt == OX_OPEN) of_xml_skip(&X);
                size_t r1 = X.end;
                of_sb_put(&o, src + tpos, r0 - tpos);
                tpos = r1;
                int k = 0;
                while (k < rep) {
                    int same = 0;
                    while (k + same < rep && !(row + k + same <= last_needed && row_changed(s, row + k + same))) same++;
                    if (same) {
                        if (same == rep) of_sb_put(&o, src + r0, r1 - r0);
                        else w_repeated(&o, src, r0, r1, "table:number-rows-repeated", same);
                        k += same;
                        continue;
                    }
                    if (tt == OX_OPEN) w_row_cells(&o, b, ix, src, r0, r1, row + k);
                    else w_new_row(&o, b, ix, row + k);
                    k++;
                }
                row += rep;
                continue;
            }
            if (tt == OX_OPEN) {
                if (of_xml_is(&X, "table:table-row-group") || of_xml_is(&X, "table:table-header-rows") ||
                    of_xml_is(&X, "table:table-rows"))
                    d++;
                else
                    of_xml_skip(&X);
            }
        }
        pos = tpos;
    }
    of_sb_put(&o, src + pos, len - pos);
    if (o.oom) { of_sb_free(&o); return NULL; }
    return of_sb_take(&o);
}

static bool save_keeping(of_book_t *b, const char *path, char *err, size_t errlen)
{
    char *content = content_xml(b, b->ods_content, b->ods_content_len);
    if (!content) { snprintf(err, errlen, "content"); return false; }
    of_zipw_t w;
    of_zipw_init(&w);
    bool ok = true;
    for (int i = 0; i < b->zip.n && ok; i++) {
        if (!strcmp(b->zip.e[i].name, "content.xml")) ok = of_zipw_add(&w, "content.xml", content, strlen(content), true);
        else ok = of_zipw_copy(&w, &b->zip, i);
    }
    ok = ok && of_zipw_finish(&w, path);
    of_zipw_free(&w);
    of_free(content);
    if (!ok) snprintf(err, errlen, "write");
    return ok;
}

/* -------------------------------------------------------------------------- */
/* A new package                                                               */
/* -------------------------------------------------------------------------- */

static const char NS[] =
    "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
    "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
    "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
    "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
    "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" "
    "xmlns:number=\"urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0\" "
    "xmlns:of=\"urn:oasis:names:tc:opendocument:xmlns:of:1.2\" office:version=\"1.3\"";

/* An ODF data style for an Excel code. */
static void w_dstyle(of_sb_t *o, const char *name, const char *code)
{
    of_nfinfo_t in;
    of_numfmt_info(code, &in);
    bool en = !of_locale_comma();
    const char *pre = in.prefix, *suf = in.suffix;
    switch (in.kind) {
    case 3: case 5:
        of_sb_printf(o, "<number:date-style style:name=\"%s\">", name);
        if (en) of_sb_puts(o, "<number:month number:style=\"long\"/><number:text>/</number:text><number:day number:style=\"long\"/>");
        else of_sb_puts(o, "<number:day number:style=\"long\"/><number:text>/</number:text><number:month number:style=\"long\"/>");
        of_sb_puts(o, "<number:text>/</number:text><number:year number:style=\"long\"/>");
        if (in.kind == 5) of_sb_puts(o, "<number:text> </number:text><number:hours number:style=\"long\"/><number:text>:</number:text><number:minutes number:style=\"long\"/>");
        of_sb_puts(o, "</number:date-style>");
        return;
    case 4:
        of_sb_printf(o, "<number:time-style style:name=\"%s\"><number:hours number:style=\"long\"/><number:text>:</number:text>"
                        "<number:minutes number:style=\"long\"/><number:text>:</number:text><number:seconds number:style=\"long\"/>"
                        "</number:time-style>", name);
        return;
    case 6:
        of_sb_printf(o, "<number:text-style style:name=\"%s\"><number:text-content/></number:text-style>", name);
        return;
    default:
        break;
    }
    of_sb_printf(o, "<number:%s style:name=\"%s\">", in.kind == 2 ? "percentage-style" : "number-style", name);
    if (*pre) { of_sb_puts(o, "<number:text>"); of_sb_xml(o, pre, strlen(pre)); of_sb_puts(o, "</number:text>"); }
    of_sb_printf(o, "<number:number number:decimal-places=\"%d\" number:min-decimal-places=\"%d\" number:min-integer-digits=\"1\"%s/>",
                 in.decimals, in.decimals, in.grouping ? " number:grouping=\"true\"" : "");
    if (in.kind == 2) of_sb_puts(o, "<number:text>%</number:text>");
    if (*suf) { of_sb_puts(o, "<number:text>"); of_sb_xml(o, suf, strlen(suf)); of_sb_puts(o, "</number:text>"); }
    of_sb_printf(o, "</number:%s>", in.kind == 2 ? "percentage-style" : "number-style");
}

static bool save_new(of_book_t *b, const char *path, char *err, size_t errlen)
{
    of_zipw_t w;
    of_zipw_init(&w);
    of_zipw_add(&w, "mimetype", "application/vnd.oasis.opendocument.spreadsheet", 46, false);
    of_sb_t o = { 0 };
    of_sb_puts(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">"
                   "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.3\" manifest:media-type=\"application/vnd.oasis.opendocument.spreadsheet\"/>"
                   "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
                   "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
                   "</manifest:manifest>");
    of_zipw_add(&w, "META-INF/manifest.xml", o.s, o.n, true);
    o.n = 0;
    of_sb_printf(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles %s><office:styles>"
                     "<style:style style:name=\"Default\" style:family=\"table-cell\"/></office:styles></office:document-styles>", NS);
    of_zipw_add(&w, "styles.xml", o.s, o.n, true);
    o.n = 0;

    /* automatic styles: column widths, and dates and percentages */
    of_sb_printf(&o, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content %s><office:automatic-styles>", NS);
    /* one data style and one cell style per number format in use */
    char **codes = NULL;
    int ncodes = 0;
    for (int i = 0; i < b->nsh; i++)
        for (int r = 0; r < b->sh[i].nrows; r++)
            for (int k = 0; k < b->sh[i].rows[r].n; k++) {
                of_cell_t *c = &b->sh[i].rows[r].c[k];
                if (c->v.t != OV_NUM) continue;
                const char *code = of_cell_code(b, c);
                if (!code || !strcmp(code, "General")) continue;
                bool seen = false;
                for (int q = 0; q < ncodes && !seen; q++) seen = !strcmp(codes[q], code);
                if (seen) continue;
                char **nc = of_realloc(codes, (size_t)(ncodes + 1) * sizeof *nc);
                if (!nc) continue;
                codes = nc;
                codes[ncodes++] = of_strdup(code);
            }
    for (int q = 0; q < ncodes; q++) {
        char nm[16];
        snprintf(nm, sizeof nm, "N%d", q + 1);
        w_dstyle(&o, nm, codes[q]);
        of_sb_printf(&o, "<style:style style:name=\"ce%d\" style:family=\"table-cell\" style:parent-style-name=\"Default\" "
                         "style:data-style-name=\"N%d\"/>", q + 1, q + 1);
    }
    for (int i = 0; i < b->nsh; i++) {
        of_sheet_t *s = &b->sh[i];
        for (int c = 0; c < s->ncolw; c++)
            if (s->colw[c] > 0)
                of_sb_printf(&o, "<style:style style:name=\"co%d_%d\" style:family=\"table-column\"><style:table-column-properties "
                                 "style:column-width=\"%.3fcm\"/></style:style>", i, c, (double)(s->colw[c] * 5.25f / 28.3465f));
    }
    of_sb_puts(&o, "<style:style style:name=\"coD\" style:family=\"table-column\"><style:table-column-properties "
                   "style:column-width=\"2.258cm\"/></style:style></office:automatic-styles><office:body><office:spreadsheet>");
    /* sty is shared by the cells of xlsx (xf indices) and ods (names): a new
     * package uses only the hints */
    of_book_t tmp = *b;
    for (int i = 0; i < b->nsh; i++) {
        of_sheet_t *s = &b->sh[i];
        of_sb_puts(&o, "<table:table table:name=\"");
        of_sb_xml(&o, s->name ? s->name : "Hoja", s->name ? strlen(s->name) : 4);
        of_sb_puts(&o, "\">");
        int32_t mr, mc;
        of_sheet_extent(s, &mr, &mc);
        int ncols = mc + 1 > s->ncolw ? mc + 1 : s->ncolw;
        for (int c = 0; c < ncols; c++) {
            if (c < s->ncolw && s->colw[c] > 0) of_sb_printf(&o, "<table:table-column table:style-name=\"co%d_%d\"/>", i, c);
            else of_sb_puts(&o, "<table:table-column table:style-name=\"coD\"/>");
        }
        if (!ncols) of_sb_puts(&o, "<table:table-column table:style-name=\"coD\"/>");
        int32_t prev = -1;
        for (int r = 0; r < s->nrows; r++) {
            of_row_t *row = &s->rows[r];
            bool any = false;
            for (int k = 0; k < row->n; k++)
                if (row->c[k].v.t != OV_EMPTY || row->c[k].f) any = true;
            if (!any) continue;
            if (row->row > prev + 1)
                of_sb_printf(&o, "<table:table-row table:number-rows-repeated=\"%ld\"><table:table-cell/></table:table-row>",
                             (long)(row->row - prev - 1));
            of_sb_puts(&o, "<table:table-row>");
            int32_t col = 0;
            for (int k = 0; k < row->n; k++) {
                of_cell_t *c = &row->c[k];
                if (c->v.t == OV_EMPTY && !c->f) continue;
                if (c->col > col) of_sb_printf(&o, "<table:table-cell table:number-columns-repeated=\"%ld\"/>", (long)(c->col - col));
                int h = cell_hint(&tmp, c);
                of_sb_puts(&o, "<table:table-cell");
                of_cell_t cc = *c;
                cc.hint = (uint8_t)h;
                char sn[16] = "";
                if (c->v.t == OV_NUM) {
                    const char *code = of_cell_code(b, c);
                    for (int q = 0; q < ncodes && code; q++)
                        if (!strcmp(codes[q], code)) snprintf(sn, sizeof sn, "ce%d", q + 1);
                    /* the data style says what the number is */
                    of_nfinfo_t in;
                    of_numfmt_info(code, &in);
                    cc.hint = in.kind == 3 ? OH_DATE : in.kind == 5 ? OH_DATETIME : in.kind == 4 ? OH_TIME : in.kind == 2 ? OH_PCT : 0;
                }
                w_value(&o, &tmp, i, &cc, NULL, 0, sn[0] ? sn : NULL);
                col = c->col + 1;
            }
            of_sb_puts(&o, "</table:table-row>");
            prev = row->row;
        }
        if (prev < 0) of_sb_puts(&o, "<table:table-row><table:table-cell/></table:table-row>");
        of_sb_puts(&o, "</table:table>");
    }
    of_sb_puts(&o, "</office:spreadsheet></office:body></office:document-content>");
    of_zipw_add(&w, "content.xml", o.s, o.n, true);
    for (int q = 0; q < ncodes; q++) of_free(codes[q]);
    of_free(codes);
    bool ok = !o.oom && of_zipw_finish(&w, path);
    of_sb_free(&o);
    of_zipw_free(&w);
    if (!ok) snprintf(err, errlen, "write");
    return ok;
}

bool of_ods_save(of_book_t *b, const char *path, char *err, size_t errlen)
{
    err[0] = 0;
    if (b->fmt == OF_FMT_ODS && b->zip.n && b->ods_content) return save_keeping(b, path, err, errlen);
    return save_new(b, path, err, errlen);
}
