/*
 * P4OS - Office: XLSX (Office Open XML spreadsheets).
 *
 * Reading: the package's relationships lead to the workbook, its sheets,
 * the shared strings and the styles (number formats, bold, italic, colours,
 * fills, alignment). Each sheet's cells come with their formulas; shared
 * formulas are expanded for the members (the master's text, moved); array
 * formulas keep the values the file cached. Every <row> and every <c>
 * remembers where it was in its part.
 *
 * Saving into the same file keeps the package: every part goes out as it
 * was except
 *   - the sheets with changes: the XML before and after <sheetData> as it
 *     was (with <cols> and <dimension> brought up to date), and inside it
 *     the rows nobody touched byte for byte; in a touched row, the cells
 *     nobody touched byte for byte too, and the rest written again (strings
 *     inline, so sharedStrings.xml does not change);
 *   - styles.xml, only when a number format was chosen: the new formats
 *     and cell formats appended, copies of the cell's old one;
 *   - calcChain.xml, which Excel rebuilds and which would be stale, is
 *     dropped with its two mentions, and workbook.xml asks for a full
 *     recalculation on load (so formulas the app does not have get their
 *     results again).
 *
 * Saving a book from elsewhere (a CSV, an ODS, a new one) writes a new,
 * minimal package.
 */
#include "of.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XLSX_MAX (32u * 1024 * 1024)

char *of_excel_to_odf_book(const char *src, const of_book_t *b, int sheet_ix);

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

/* Excel escapes control characters as _xHHHH_ */
static void unescape_x(of_sb_t *sb)
{
    if (!sb->s || !strstr(sb->s, "_x")) return;
    size_t w = 0;
    for (size_t i = 0; i < sb->n;) {
        if (i + 7 <= sb->n && sb->s[i] == '_' && sb->s[i + 1] == 'x' && sb->s[i + 6] == '_') {
            char hex[5] = { sb->s[i + 2], sb->s[i + 3], sb->s[i + 4], sb->s[i + 5], 0 };
            char *end;
            long v = strtol(hex, &end, 16);
            if (*end == 0) {
                char u[4];
                int l = of_utf8_put(u, (uint32_t)v);
                memcpy(sb->s + w, u, (size_t)l);
                w += (size_t)l;
                i += 7;
                continue;
            }
        }
        sb->s[w++] = sb->s[i++];
    }
    sb->n = w;
    sb->s[w] = 0;
}

static uint8_t *part(of_book_t *b, const char *name, size_t *len)
{
    int i = of_zip_find(&b->zip, name);
    return i < 0 ? NULL : of_zip_read(&b->zip, i, len);
}

/* A relationship's target, resolved against the folder of its source. */
static void resolve(const char *dir, const char *target, char *out, size_t len)
{
    if (target[0] == '/') {
        snprintf(out, len, "%s", target + 1);
        return;
    }
    char buf[256];
    snprintf(buf, sizeof buf, "%s%s", dir, target);
    /* fold "../" */
    char *p;
    while ((p = strstr(buf, "/../"))) {
        char *q = p;
        while (q > buf && q[-1] != '/') q--;
        memmove(q, p + 4, strlen(p + 4) + 1);
    }
    snprintf(out, len, "%s", buf);
}

typedef struct {
    char id[32], type[128], target[200];
} rel_t;

static int read_rels(of_book_t *b, const char *name, rel_t **out)
{
    size_t len;
    char *x = (char *)part(b, name, &len);
    *out = NULL;
    if (!x) return 0;
    int n = 0, cap = 0;
    rel_t *r = NULL;
    of_xml_t X;
    of_xml_init(&X, x, len);
    int t;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if ((t == OX_EMPTY || t == OX_OPEN) && of_xml_local_is(&X, "Relationship")) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                rel_t *nr = of_realloc(r, (size_t)cap * sizeof *nr);
                if (!nr) break;
                r = nr;
            }
            memset(&r[n], 0, sizeof r[n]);
            of_xml_attr(&X, "Id", r[n].id, sizeof r[n].id);
            of_xml_attr(&X, "Type", r[n].type, sizeof r[n].type);
            of_xml_attr(&X, "Target", r[n].target, sizeof r[n].target);
            n++;
        }
    }
    of_free(x);
    *out = r;
    return n;
}

static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

static uint32_t argb(const char *s)
{
    size_t l = strlen(s);
    if (l < 6) return 0;
    return (uint32_t)strtoul(s + l - 6, NULL, 16) | 0x1000000u;
}

/* The Office theme's colours, for "theme" colours without reading theme1.xml */
static const uint32_t THEME[10] = { 0xFFFFFF, 0x000000, 0xE7E6E6, 0x44546A, 0x4472C4,
                                    0xED7D31, 0xA5A5A5, 0xFFC000, 0x5B9BD5, 0x70AD47 };

static uint32_t color_attr(of_xml_t *X)
{
    char v[32];
    if (of_xml_attr(X, "rgb", v, sizeof v)) return argb(v);
    if (of_xml_attr(X, "theme", v, sizeof v)) {
        int t = atoi(v);
        if (t >= 0 && t < 10) return THEME[t] | 0x1000000u;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Styles                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct { uint8_t b, i, u; uint32_t color; } font_t;

static void load_styles(of_book_t *b, const char *name)
{
    size_t len;
    char *x = (char *)part(b, name, &len);
    if (!x) return;
    font_t *fonts = NULL;
    int nfonts = 0;
    uint32_t *fills = NULL;
    int nfills = 0;
    of_xml_t X;
    of_xml_init(&X, x, len);
    int t, where = 0;           /* 1 fonts, 2 fills, 3 cellXfs */
    font_t *cur = NULL;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_CLOSE) {
            if (of_xml_is(&X, "fonts") || of_xml_is(&X, "fills") || of_xml_is(&X, "cellXfs")) where = 0;
            if (of_xml_is(&X, "font")) cur = NULL;
            continue;
        }
        if (t != OX_OPEN && t != OX_EMPTY) continue;
        if (of_xml_is(&X, "numFmt")) {
            char id[16], code[256];
            if (of_xml_attr(&X, "numFmtId", id, sizeof id) && of_xml_attr(&X, "formatCode", code, sizeof code)) {
                of_numfmt_t *nf = of_realloc(b->nf, (size_t)(b->nnf + 1) * sizeof *nf);
                if (nf) {
                    b->nf = nf;
                    b->nf[b->nnf].id = (uint32_t)atoi(id);
                    b->nf[b->nnf].code = of_strdup(code);
                    b->nnf++;
                }
            }
        } else if (of_xml_is(&X, "fonts") && t == OX_OPEN) where = 1;
        else if (of_xml_is(&X, "fills") && t == OX_OPEN) where = 2;
        else if (of_xml_is(&X, "cellXfs") && t == OX_OPEN) where = 3;
        else if (of_xml_is(&X, "cellStyleXfs") && t == OX_OPEN) { of_xml_skip(&X); }
        else if (of_xml_is(&X, "dxfs") && t == OX_OPEN) { of_xml_skip(&X); }
        else if (where == 1 && of_xml_is(&X, "font")) {
            font_t *nf = of_realloc(fonts, (size_t)(nfonts + 1) * sizeof *nf);
            if (!nf) continue;
            fonts = nf;
            memset(&fonts[nfonts], 0, sizeof *fonts);
            cur = t == OX_OPEN ? &fonts[nfonts] : NULL;
            nfonts++;
        } else if (where == 1 && cur) {
            char v[16];
            if (of_xml_is(&X, "b")) cur->b = !(of_xml_attr(&X, "val", v, sizeof v) && (v[0] == '0' || v[0] == 'f'));
            else if (of_xml_is(&X, "i")) cur->i = !(of_xml_attr(&X, "val", v, sizeof v) && (v[0] == '0' || v[0] == 'f'));
            else if (of_xml_is(&X, "u")) cur->u = !(of_xml_attr(&X, "val", v, sizeof v) && !strcmp(v, "none"));
            else if (of_xml_is(&X, "color")) cur->color = color_attr(&X);
        } else if (where == 2 && of_xml_is(&X, "fill")) {
            uint32_t *nf = of_realloc(fills, (size_t)(nfills + 1) * sizeof *nf);
            if (!nf) continue;
            fills = nf;
            fills[nfills] = 0;
            if (t == OX_OPEN) {
                /* <patternFill patternType="solid"><fgColor rgb=.../> */
                bool solid = false;
                int d = 1, tt;
                while (d > 0 && (tt = of_xml_next(&X)) != OX_END && tt != OX_ERROR) {
                    if (tt == OX_OPEN) d++;
                    else if (tt == OX_CLOSE) d--;
                    if ((tt == OX_OPEN || tt == OX_EMPTY) && of_xml_is(&X, "patternFill")) {
                        char pt[24] = "";
                        of_xml_attr(&X, "patternType", pt, sizeof pt);
                        solid = pt[0] && strcmp(pt, "none") != 0;
                    }
                    if ((tt == OX_OPEN || tt == OX_EMPTY) && of_xml_is(&X, "fgColor") && solid)
                        fills[nfills] = color_attr(&X);
                }
            }
            nfills++;
        } else if (where == 3 && of_xml_is(&X, "xf")) {
            of_xf_t xf = { 0 };
            char v[16];
            if (of_xml_attr(&X, "numFmtId", v, sizeof v)) xf.numfmt = (uint32_t)atoi(v);
            if (of_xml_attr(&X, "fontId", v, sizeof v)) {
                int f = atoi(v);
                if (f >= 0 && f < nfonts) {
                    xf.bold = fonts[f].b;
                    xf.italic = fonts[f].i;
                    xf.underline = fonts[f].u;
                    xf.color = fonts[f].color;
                    /* black text is the default, not a colour to show */
                    if ((xf.color & 0xFFFFFF) == 0) xf.color = 0;
                }
            }
            if (of_xml_attr(&X, "fillId", v, sizeof v)) {
                int f = atoi(v);
                if (f >= 0 && f < nfills) xf.fill = fills[f];
            }
            if (t == OX_OPEN) {
                int d = 1, tt;
                while (d > 0 && (tt = of_xml_next(&X)) != OX_END && tt != OX_ERROR) {
                    if (tt == OX_OPEN) d++;
                    else if (tt == OX_CLOSE) d--;
                    if ((tt == OX_OPEN || tt == OX_EMPTY) && of_xml_is(&X, "alignment")) {
                        char h[16] = "";
                        of_xml_attr(&X, "horizontal", h, sizeof h);
                        xf.halign = !strcmp(h, "left") ? 1 : !strcmp(h, "center") ? 2 : !strcmp(h, "right") ? 3 : 0;
                    }
                }
            }
            of_xf_t *nx = of_realloc(b->xf, (size_t)(b->nxf + 1) * sizeof *nx);
            if (nx) {
                b->xf = nx;
                xf.from = (uint32_t)b->nxf;
                b->xf[b->nxf++] = xf;
            }
        }
    }
    b->nxf_orig = b->nxf;
    of_free(fonts);
    of_free(fills);
    of_free(x);
}

/* -------------------------------------------------------------------------- */
/* Shared strings                                                              */
/* -------------------------------------------------------------------------- */

static void load_sst(of_book_t *b, const char *name)
{
    size_t len;
    char *x = (char *)part(b, name, &len);
    if (!x) return;
    of_xml_t X;
    of_xml_init(&X, x, len);
    int t, cap = 0;
    of_sb_t cur = { 0 };
    bool in_si = false;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_OPEN && of_xml_is(&X, "si")) {
            in_si = true;
            cur.n = 0;
            if (cur.s) cur.s[0] = 0;
        } else if (t == OX_EMPTY && of_xml_is(&X, "si")) {
            in_si = true;
            cur.n = 0;
            if (cur.s) cur.s[0] = 0;
            goto store;
        } else if (in_si && t == OX_OPEN && of_xml_is(&X, "rPh")) {
            of_xml_skip(&X);        /* phonetic guide: not the text */
        } else if (in_si && t == OX_OPEN && of_xml_is(&X, "t")) {
            of_xml_text(&X, &cur);
        } else if (t == OX_CLOSE && of_xml_is(&X, "si")) {
        store:
            in_si = false;
            if (b->nsst == cap) {
                cap = cap ? cap * 2 : 256;
                char **ns = of_realloc(b->sst, (size_t)cap * sizeof *ns);
                if (!ns) break;
                b->sst = ns;
            }
            unescape_x(&cur);
            b->sst[b->nsst++] = of_strdup(cur.s ? cur.s : "");
        }
    }
    of_sb_free(&cur);
    of_free(x);
}

/* -------------------------------------------------------------------------- */
/* A sheet                                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t si;
    int32_t  row, col;
    char    *f;
} shared_t;

static void load_sheet(of_book_t *b, int ix, const char *src, size_t len)
{
    of_sheet_t *s = &b->sh[ix];
    shared_t *sh = NULL;
    int nsh = 0;
    of_xml_t X;
    of_xml_init(&X, src, len);
    int t;
    int32_t next_row = 0;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t != OX_OPEN && t != OX_EMPTY) continue;
        char v[64];
        if (of_xml_is(&X, "sheetFormatPr")) {
            if (of_xml_attr(&X, "defaultColWidth", v, sizeof v)) s->def_colw = strtof(v, NULL);
            else if (of_xml_attr(&X, "baseColWidth", v, sizeof v)) s->def_colw = strtof(v, NULL) + 1.7f;
            if (of_xml_attr(&X, "defaultRowHeight", v, sizeof v)) s->def_rowh = strtof(v, NULL);
        } else if (of_xml_is(&X, "pane")) {
            char st[16] = "";
            of_xml_attr(&X, "state", st, sizeof st);
            if (!strncmp(st, "frozen", 6)) {
                if (of_xml_attr(&X, "xSplit", v, sizeof v)) s->freeze_c = (int)strtof(v, NULL);
                if (of_xml_attr(&X, "ySplit", v, sizeof v)) s->freeze_r = (int)strtof(v, NULL);
            }
        } else if (of_xml_is(&X, "col")) {
            char mn[16], mx[16], w[32];
            if (of_xml_attr(&X, "min", mn, sizeof mn) && of_xml_attr(&X, "max", mx, sizeof mx) &&
                of_xml_attr(&X, "width", w, sizeof w)) {
                int a = atoi(mn) - 1, z = atoi(mx) - 1;
                float wd = strtof(w, NULL);
                if (of_xml_attr(&X, "hidden", v, sizeof v) && (v[0] == '1' || v[0] == 't')) wd = 0.01f;
                if (z > a + 512) z = a + 512;      /* "to the end of the sheet": no need to keep them all */
                for (int c = a; c <= z && c < 16384; c++) of_sheet_set_colw(s, c, wd);
            }
        } else if (of_xml_is(&X, "mergeCell")) {
            if (of_xml_attr(&X, "ref", v, sizeof v)) {
                int32_t r1, c1, r2, c2;
                int u;
                char *colon = strchr(v, ':');
                if (colon && of_parse_ref(v, &r1, &c1, &u) && of_parse_ref(colon + 1, &r2, &c2, &u)) {
                    of_range_t *m = of_realloc(s->merges, (size_t)(s->nmerges + 1) * sizeof *m);
                    if (m) {
                        s->merges = m;
                        s->merges[s->nmerges++] = (of_range_t){ (uint32_t)c1, (uint32_t)r1, (uint32_t)c2, (uint32_t)r2 };
                    }
                }
            }
        } else if (of_xml_is(&X, "row")) {
            int32_t r = next_row;
            if (of_xml_attr(&X, "r", v, sizeof v)) r = atoi(v) - 1;
            if (r < 0 || r > 1048575) continue;
            next_row = r + 1;
            of_row_t *row = of_row_make(s, r);
            if (!row) continue;
            row->raw_off = (uint32_t)X.start;
            row->raw_tag_end = (uint32_t)X.end;
            if (of_xml_attr(&X, "ht", v, sizeof v)) {
                char ch[8] = "";
                of_xml_attr(&X, "customHeight", ch, sizeof ch);
                if (ch[0] == '1' || ch[0] == 't') row->height = strtof(v, NULL);
            }
            if (of_xml_attr(&X, "hidden", v, sizeof v) && (v[0] == '1' || v[0] == 't')) row->height = 0.01f;
            if (t == OX_EMPTY) {
                row->raw_len = (uint32_t)(X.end - X.start);
                continue;
            }
            /* its cells */
            int32_t next_col = 0;
            int tt;
            while ((tt = of_xml_next(&X)) != OX_END && tt != OX_ERROR) {
                if (tt == OX_CLOSE && of_xml_is(&X, "row")) {
                    row = of_row_get(s, r);
                    if (row) row->raw_len = (uint32_t)(X.end - row->raw_off);
                    break;
                }
                if ((tt != OX_OPEN && tt != OX_EMPTY) || !of_xml_is(&X, "c")) continue;
                size_t c0 = X.start;
                int32_t cr = r, cc = next_col;
                int u;
                if (of_xml_attr(&X, "r", v, sizeof v)) of_parse_ref(v, &cr, &cc, &u);
                next_col = cc + 1;
                char ty[16] = "n", st[16] = "0";
                of_xml_attr(&X, "t", ty, sizeof ty);
                of_xml_attr(&X, "s", st, sizeof st);
                of_sb_t val = { 0 }, ftxt = { 0 }, istr = { 0 };
                char ft[16] = "", si[16] = "", ref[40] = "";
                bool has_f = false, has_v = false;
                if (tt == OX_OPEN) {
                    int t3;
                    while ((t3 = of_xml_next(&X)) != OX_END && t3 != OX_ERROR) {
                        if (t3 == OX_CLOSE && of_xml_is(&X, "c")) break;
                        if (t3 == OX_OPEN && of_xml_is(&X, "v")) { has_v = true; of_xml_text(&X, &val); }
                        else if ((t3 == OX_OPEN || t3 == OX_EMPTY) && of_xml_is(&X, "f")) {
                            has_f = true;
                            of_xml_attr(&X, "t", ft, sizeof ft);
                            of_xml_attr(&X, "si", si, sizeof si);
                            of_xml_attr(&X, "ref", ref, sizeof ref);
                            if (t3 == OX_OPEN) of_xml_text(&X, &ftxt);
                        } else if (t3 == OX_OPEN && of_xml_is(&X, "is")) {
                            /* inline string: the <t> of it and of its runs */
                            int d = 1, t4;
                            while (d > 0 && (t4 = of_xml_next(&X)) != OX_END && t4 != OX_ERROR) {
                                if (t4 == OX_OPEN && of_xml_is(&X, "t")) { of_xml_text(&X, &istr); continue; }
                                if (t4 == OX_OPEN && of_xml_is(&X, "rPh")) { of_xml_skip(&X); continue; }
                                if (t4 == OX_OPEN) d++;
                                else if (t4 == OX_CLOSE) d--;
                            }
                        }
                    }
                }
                of_cell_t *c = of_cell_make(s, r, cc);
                if (c) {
                    c->raw_off = (uint32_t)c0;
                    c->raw_len = (uint32_t)(X.end - c0);
                    c->style = (uint32_t)atoi(st);
                    of_val_t *cv = &c->cached;
                    if (!strcmp(ty, "s") && has_v) {
                        int k = atoi(val.s ? val.s : "0");
                        cv->t = OV_STR;
                        cv->s = of_strdup(k >= 0 && k < b->nsst ? b->sst[k] : "");
                    } else if (!strcmp(ty, "inlineStr")) {
                        unescape_x(&istr);
                        cv->t = OV_STR;
                        cv->s = of_strdup(istr.s ? istr.s : "");
                    } else if (!strcmp(ty, "str") || !strcmp(ty, "d")) {
                        if (has_v || !strcmp(ty, "str")) {
                            unescape_x(&val);
                            cv->t = OV_STR;
                            cv->s = of_strdup(val.s ? val.s : "");
                        }
                    } else if (!strcmp(ty, "b") && has_v) {
                        cv->t = OV_BOOL;
                        cv->n = val.s && val.s[0] == '1';
                    } else if (!strcmp(ty, "e") && has_v) {
                        cv->t = OV_ERR;
                        cv->err = (uint8_t)of_err_from_name(val.s ? val.s : "");
                    } else if (has_v && val.n) {
                        cv->t = OV_NUM;
                        cv->n = strtod(val.s, NULL);
                    }
                    of_val_copy(&c->v, cv);
                    if (has_f) {
                        c->flags |= OC_FORMULA;
                        if (!strcmp(ft, "array") || !strcmp(ft, "dataTable")) {
                            c->flags |= OC_ARRAY;
                            if (ftxt.n) c->f = of_strdup(ftxt.s);
                        } else if (!strcmp(ft, "shared")) {
                            c->flags |= OC_SHARED;
                            c->shared_si = (uint16_t)(atoi(si) + 1);
                            if (ftxt.n) {
                                c->f = of_strdup(ftxt.s);
                                shared_t *ns = of_realloc(sh, (size_t)(nsh + 1) * sizeof *ns);
                                if (ns) {
                                    sh = ns;
                                    sh[nsh++] = (shared_t){ (uint32_t)atoi(si), r, cc, of_strdup(ftxt.s) };
                                }
                            }
                        } else if (ftxt.n) {
                            c->f = of_strdup(ftxt.s);
                        }
                    }
                }
                of_sb_free(&val);
                of_sb_free(&ftxt);
                of_sb_free(&istr);
            }
        } else if (of_xml_is(&X, "sheetData") && t == OX_EMPTY) {
            /* nothing in it */
        }
    }
    /* shared formula members: the master's formula, moved */
    for (int i = 0; i < s->nrows; i++) {
        of_row_t *row = &s->rows[i];
        for (int k = 0; k < row->n; k++) {
            of_cell_t *c = &row->c[k];
            if (!(c->flags & OC_SHARED) || c->f) continue;
            for (int m = 0; m < nsh; m++) {
                if (sh[m].si + 1 != c->shared_si) continue;
                c->f = of_formula_shift(sh[m].f, b, ix, row->row - sh[m].row, c->col - sh[m].col);
                break;
            }
        }
    }
    for (int m = 0; m < nsh; m++) of_free(sh[m].f);
    of_free(sh);
    /* parse them all now that the sheet names are known */
    for (int i = 0; i < s->nrows; i++)
        for (int k = 0; k < s->rows[i].n; k++) {
            of_cell_t *c = &s->rows[i].c[k];
            if (!c->f || (c->flags & OC_ARRAY)) continue;
            bool unk = false;
            c->ast = of_formula_parse(c->f, ix, b, &unk);
            if (!c->ast || unk) {
                c->flags |= OC_UNKNOWN_F;
                b->any_unknown_f = true;
            }
        }
}

of_book_t *of_xlsx_load(const char *path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *raw = of_file_read(path, &len, XLSX_MAX);
    if (!raw) { snprintf(err, errlen, "read"); return NULL; }
    if (of_is_encrypted_ooxml(raw, len)) {
        of_free(raw);
        snprintf(err, errlen, "encrypted");
        return NULL;
    }
    of_book_t *b = of_book_new(OF_FMT_XLSX);
    if (!b) { of_free(raw); return NULL; }
    b->file = raw;
    b->file_len = len;
    if (!of_zip_open(&b->zip, raw, len)) {
        snprintf(err, errlen, "zip");
        of_book_free(b);
        return NULL;
    }
    /* the workbook */
    rel_t *rels;
    int nr = read_rels(b, "_rels/.rels", &rels);
    char wb[200] = "xl/workbook.xml";
    for (int i = 0; i < nr; i++)
        if (ends_with(rels[i].type, "/officeDocument")) resolve("", rels[i].target, wb, sizeof wb);
    of_free(rels);
    char dir[200];
    snprintf(dir, sizeof dir, "%s", wb);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0;
    else dir[0] = 0;
    char relname[256];
    snprintf(relname, sizeof relname, "%s_rels/%s.rels", dir, of_basename(wb));
    nr = read_rels(b, relname, &rels);
    for (int i = 0; i < nr; i++) {
        char p[200];
        resolve(dir, rels[i].target, p, sizeof p);
        if (ends_with(rels[i].type, "/sharedStrings")) load_sst(b, p);
        else if (ends_with(rels[i].type, "/styles")) load_styles(b, p);
    }
    size_t wl;
    char *w = (char *)part(b, wb, &wl);
    if (!w) {
        of_free(rels);
        snprintf(err, errlen, "workbook");
        of_book_free(b);
        return NULL;
    }
    of_xml_t X;
    of_xml_init(&X, w, wl);
    int t;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t != OX_OPEN && t != OX_EMPTY) continue;
        char v[256];
        if (of_xml_is(&X, "workbookPr")) {
            if (of_xml_attr(&X, "date1904", v, sizeof v)) b->date1904 = v[0] == '1' || v[0] == 't';
        } else if (of_xml_is(&X, "sheet")) {
            char name[128] = "", rid[64] = "", id[16] = "", st[16] = "";
            of_xml_attr(&X, "name", name, sizeof name);
            of_xml_attr(&X, "r:id", rid, sizeof rid);
            of_xml_attr(&X, "sheetId", id, sizeof id);
            of_xml_attr(&X, "state", st, sizeof st);
            of_sheet_t *s = of_book_add_sheet(b, name);
            if (!s) continue;
            s->rid = of_strdup(rid);
            s->sheet_id = (uint32_t)atoi(id);
            s->state = st[0] && strcmp(st, "visible") != 0;
            for (int i = 0; i < nr; i++)
                if (!strcmp(rels[i].id, rid)) {
                    char p[200];
                    resolve(dir, rels[i].target, p, sizeof p);
                    s->part = of_strdup(p);
                    /* a chart sheet or a dialog sheet is not cells */
                    if (!ends_with(rels[i].type, "/worksheet")) s->state = 2;
                }
        } else if (of_xml_is(&X, "definedName")) {
            snprintf(b->notes + strlen(b->notes), sizeof b->notes - strlen(b->notes), "names ");
        }
    }
    of_free(w);
    of_free(rels);
    for (int i = 0; i < b->nsh; i++) {
        if (!b->sh[i].part || b->sh[i].state == 2) continue;
        size_t sl;
        char *sx = (char *)part(b, b->sh[i].part, &sl);
        if (!sx) continue;
        load_sheet(b, i, sx, sl);
        of_free(sx);
    }
    if (!b->nsh) of_book_add_sheet(b, "Hoja1");
    for (int i = 0; i < b->nsh; i++) {
        b->sh[i].dirty = false;
        b->sh[i].cols_dirty = false;
        if (b->sh[i].colw_set) memset(b->sh[i].colw_set, 0, (size_t)b->sh[i].ncolw);
    }
    return b;
}

/* -------------------------------------------------------------------------- */
/* Writing cells                                                               */
/* -------------------------------------------------------------------------- */

static void w_num(of_sb_t *o, double n)
{
    char buf[40];
    snprintf(buf, sizeof buf, "%.17g", n);
    /* the shortest that reads back the same */
    for (int p = 15; p < 17; p++) {
        char t[40];
        snprintf(t, sizeof t, "%.*g", p, n);
        if (strtod(t, NULL) == n) { snprintf(buf, sizeof buf, "%s", t); break; }
    }
    of_sb_puts(o, buf);
}

static const char *err_out(int e)
{
    return e == OE_CIRC || e == OE_NONE ? "#NUM!" : of_err_name(e);
}

static void w_cell(of_sb_t *o, const of_book_t *b, int32_t row, const of_cell_t *c, const char *raw_f, size_t raw_f_len)
{
    (void)b;
    char ref[16];
    of_ref_name(row, c->col, ref);
    const of_val_t *v = &c->v;
    bool formula = c->f || raw_f_len;
    if (!formula && v->t == OV_EMPTY) {
        if (c->style) of_sb_printf(o, "<c r=\"%s\" s=\"%lu\"/>", ref, (unsigned long)c->style);
        return;
    }
    of_sb_printf(o, "<c r=\"%s\"", ref);
    if (c->style) of_sb_printf(o, " s=\"%lu\"", (unsigned long)c->style);
    const char *t = NULL;
    switch (v->t) {
    case OV_STR: t = formula ? "str" : "inlineStr"; break;
    case OV_BOOL: t = "b"; break;
    case OV_ERR: t = "e"; break;
    default: break;
    }
    if (t) of_sb_printf(o, " t=\"%s\"", t);
    of_sb_putc(o, '>');
    if (raw_f_len) {
        of_sb_put(o, raw_f, raw_f_len);
    } else if (c->f) {
        of_sb_puts(o, "<f>");
        of_sb_xml(o, c->f, strlen(c->f));
        of_sb_puts(o, "</f>");
    }
    switch (v->t) {
    case OV_NUM: of_sb_puts(o, "<v>"); w_num(o, v->n); of_sb_puts(o, "</v>"); break;
    case OV_BOOL: of_sb_puts(o, v->n ? "<v>1</v>" : "<v>0</v>"); break;
    case OV_ERR: of_sb_printf(o, "<v>%s</v>", err_out(v->err)); break;
    case OV_STR:
        if (formula) {
            of_sb_puts(o, "<v>");
            of_sb_xml(o, v->s ? v->s : "", v->s ? strlen(v->s) : 0);
            of_sb_puts(o, "</v>");
        } else {
            of_sb_puts(o, "<is><t xml:space=\"preserve\">");
            of_sb_xml(o, v->s ? v->s : "", v->s ? strlen(v->s) : 0);
            of_sb_puts(o, "</t></is>");
        }
        break;
    default:
        break;
    }
    of_sb_puts(o, "</c>");
}

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

/* The <f ...>...</f> or <f .../> inside a cell's raw XML. */
static bool raw_formula(const char *cell, size_t n, const char **f, size_t *fl)
{
    of_xml_t X;
    of_xml_init(&X, cell, n);
    int t;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if ((t == OX_OPEN || t == OX_EMPTY) && of_xml_is(&X, "f")) {
            size_t a = X.start;
            if (t == OX_OPEN) of_xml_skip(&X);
            *f = cell + a;
            *fl = X.end - a;
            return true;
        }
    }
    return false;
}

static void w_row(of_sb_t *o, const of_book_t *b, const char *src, const of_row_t *r)
{
    if (!r->dirty && r->raw_len) {
        of_sb_put(o, src + r->raw_off, r->raw_len);
        return;
    }
    /* its start tag: the original attributes but spans, which may be wrong now */
    bool any = false;
    for (int k = 0; k < r->n; k++) {
        const of_cell_t *c = &r->c[k];
        if (c->v.t != OV_EMPTY || c->f || c->style || (c->raw_len && !(c->flags & OC_EDITED))) any = true;
    }
    if (r->raw_len) {
        of_xml_t X;
        of_xml_init(&X, src + r->raw_off, r->raw_tag_end - r->raw_off);
        of_xml_next(&X);
        of_sb_puts(o, "<row");
        const char *a = X.attrs;
        int al = X.attrs_len;
        /* copy attribute by attribute */
        int i = 0;
        while (i < al) {
            while (i < al && (a[i] == ' ' || a[i] == '\n' || a[i] == '\r' || a[i] == '\t')) i++;
            int k0 = i;
            while (i < al && a[i] != '=') i++;
            int k1 = i;
            i++;
            while (i < al && a[i] != '"' && a[i] != '\'') i++;
            if (i >= al) break;
            char q = a[i++];
            while (i < al && a[i] != q) i++;
            i++;
            if (k1 - k0 == 5 && !strncmp(a + k0, "spans", 5)) continue;
            of_sb_putc(o, ' ');
            of_sb_put(o, a + k0, (size_t)(i - k0));
        }
    } else {
        of_sb_printf(o, "<row r=\"%ld\"", (long)r->row + 1);
    }
    if (!any) {
        of_sb_puts(o, "/>");
        return;
    }
    of_sb_putc(o, '>');
    for (int k = 0; k < r->n; k++) {
        const of_cell_t *c = &r->c[k];
        bool touched = c->flags & (OC_EDITED | OC_STYLED);
        if (!touched && c->raw_len) {
            if (!c->f || (c->flags & (OC_UNKNOWN_F | OC_ARRAY)) || same_val(&c->v, &c->cached)) {
                of_sb_put(o, src + c->raw_off, c->raw_len);
                continue;
            }
            /* its result changed: the same formula element, a new value */
            const char *f;
            size_t fl;
            if (raw_formula(src + c->raw_off, c->raw_len, &f, &fl)) {
                w_cell(o, b, r->row, c, f, fl);
                continue;
            }
        }
        w_cell(o, b, r->row, c, NULL, 0);
    }
    of_sb_puts(o, "</row>");
}

static void w_cols(of_sb_t *o, const of_sheet_t *s)
{
    int last = -1;
    for (int c = 0; c < s->ncolw; c++)
        if (s->colw[c] > 0) last = c;
    if (last < 0) return;
    of_sb_puts(o, "<cols>");
    for (int c = 0; c <= last;) {
        if (s->colw[c] <= 0) { c++; continue; }
        int e = c;
        while (e + 1 <= last && s->colw[e + 1] == s->colw[c]) e++;
        if (s->colw[c] < 0.1f)
            of_sb_printf(o, "<col min=\"%d\" max=\"%d\" width=\"%.2f\" hidden=\"1\" customWidth=\"1\"/>", c + 1, e + 1,
                         (double)(s->def_colw > 0 ? s->def_colw : 10.7f));
        else
            of_sb_printf(o, "<col min=\"%d\" max=\"%d\" width=\"%.2f\" customWidth=\"1\"/>", c + 1, e + 1, (double)s->colw[c]);
        c = e + 1;
    }
    of_sb_puts(o, "</cols>");
}

/* The <cols> of an edited sheet: the original <col> elements whose columns
 * kept their width as they were (styles and outline levels with them), the
 * changed ones split out. */
static void w_cols_merge(of_sb_t *o, const of_sheet_t *s, const char *src, size_t a, size_t z)
{
    of_xml_t X;
    of_xml_init(&X, src + a, z - a);
    int t;
    bool open = false;
    int covered_to = -1;
    of_sb_puts(o, "<cols>");
    open = true;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if ((t != OX_EMPTY && t != OX_OPEN) || !of_xml_is(&X, "col")) continue;
        char mn[16], mx[16];
        if (!of_xml_attr(&X, "min", mn, sizeof mn) || !of_xml_attr(&X, "max", mx, sizeof mx)) continue;
        int c0 = atoi(mn) - 1, c1 = atoi(mx) - 1;
        bool changed = false;
        for (int c = c0; c <= c1 && c < s->ncolw; c++)
            if (s->colw_set && s->colw_set[c]) changed = true;
        if (!changed) {
            of_sb_put(o, X.src + X.start, X.end - X.start);
        } else {
            /* the attributes of the original, with min, max and width per column */
            for (int c = c0; c <= c1 && c < 16384; c++) {
                of_sb_printf(o, "<col min=\"%d\" max=\"%d\"", c + 1, c + 1);
                const char *at = X.attrs;
                int al = X.attrs_len, i = 0;
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
                    int kl = k1 - k0;
                    if ((kl == 3 && (!strncmp(at + k0, "min", 3) || !strncmp(at + k0, "max", 3))) ||
                        (kl == 5 && !strncmp(at + k0, "width", 5)) || (kl == 11 && !strncmp(at + k0, "customWidth", 11)) ||
                        (kl == 6 && !strncmp(at + k0, "hidden", 6)))
                        continue;
                    of_sb_putc(o, ' ');
                    of_sb_put(o, at + k0, (size_t)(i - k0));
                }
                float w = of_sheet_colw(s, c);
                if (w < 0.1f) of_sb_printf(o, " width=\"%.2f\" hidden=\"1\" customWidth=\"1\"/>", (double)s->def_colw);
                else of_sb_printf(o, " width=\"%.2f\" customWidth=\"1\"/>", (double)w);
            }
        }
        if (c1 > covered_to) covered_to = c1;
    }
    /* columns changed beyond every original range */
    for (int c = covered_to + 1; c < s->ncolw; c++)
        if (s->colw_set && s->colw_set[c])
            of_sb_printf(o, "<col min=\"%d\" max=\"%d\" width=\"%.2f\" customWidth=\"1\"/>", c + 1, c + 1,
                         (double)of_sheet_colw(s, c));
    if (open) of_sb_puts(o, "</cols>");
}

static char *sheet_xml(const of_book_t *b, const of_sheet_t *s, const char *src, size_t len)
{
    of_sb_t o = { 0 };
    /* where things are */
    size_t sd0 = 0, sd1 = 0, sd_end = 0, cols0 = 0, cols1 = 0, dim0 = 0, dim1 = 0;
    bool sd_empty = false;
    of_xml_t X;
    of_xml_init(&X, src, len);
    int t;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_EMPTY && of_xml_is(&X, "dimension")) { dim0 = X.start; dim1 = X.end; }
        else if (t == OX_OPEN && of_xml_is(&X, "cols")) { cols0 = X.start; of_xml_skip(&X); cols1 = X.end; }
        else if (t == OX_EMPTY && of_xml_is(&X, "cols")) { cols0 = X.start; cols1 = X.end; }
        else if (t == OX_OPEN && of_xml_is(&X, "sheetData")) {
            sd0 = X.start;
            sd1 = X.end;
            of_xml_skip(&X);
            sd_end = X.end;
            break;
        } else if (t == OX_EMPTY && of_xml_is(&X, "sheetData")) {
            sd0 = X.start;
            sd1 = sd_end = X.end;
            sd_empty = true;
            break;
        }
    }
    if (!sd0) return NULL;
    int32_t mr, mc;
    of_sheet_extent(s, &mr, &mc);
    size_t pos = 0;
    if (dim1 && dim1 < sd0) {
        of_sb_put(&o, src, dim0);
        char a[16], z[16];
        of_ref_name(0, 0, a);
        of_ref_name(mr < 0 ? 0 : mr, mc < 0 ? 0 : mc, z);
        if (mr < 0) of_sb_puts(&o, "<dimension ref=\"A1\"/>");
        else of_sb_printf(&o, "<dimension ref=\"%s:%s\"/>", a, z);
        pos = dim1;
    }
    if (s->cols_dirty) {
        if (cols1 && cols1 <= sd0) {
            of_sb_put(&o, src + pos, cols0 - pos);
            w_cols_merge(&o, s, src, cols0, cols1);
            pos = cols1;
        } else {
            of_sb_put(&o, src + pos, sd0 - pos);
            w_cols(&o, s);
            pos = sd0;
        }
    }
    of_sb_put(&o, src + pos, sd0 - pos);
    of_sb_puts(&o, "<sheetData>");
    (void)sd1;
    (void)sd_empty;
    for (int i = 0; i < s->nrows; i++) w_row(&o, b, src, &s->rows[i]);
    of_sb_puts(&o, "</sheetData>");
    of_sb_put(&o, src + sd_end, len - sd_end);
    if (o.oom) { of_sb_free(&o); return NULL; }
    return of_sb_take(&o);
}

/* -------------------------------------------------------------------------- */
/* The other parts that change                                                 */
/* -------------------------------------------------------------------------- */

static void replace_count(of_sb_t *o, const char *tag, size_t tl, int count)
{
    /* the start tag with its count="" set to 'count' */
    const char *v;
    int vl;
    of_xml_t X;
    of_xml_init(&X, tag, tl);
    of_xml_next(&X);
    if (X.attrs && of_xml_attr_raw(X.attrs, X.attrs_len, "count", &v, &vl)) {
        of_sb_put(o, tag, (size_t)(v - tag));
        of_sb_printf(o, "%d", count);
        of_sb_put(o, v + vl, tl - (size_t)(v + vl - tag));
    } else {
        of_sb_put(o, tag, tl);
    }
}

static char *styles_xml(const of_book_t *b, const char *src, size_t len)
{
    of_sb_t o = { 0 };
    of_xml_t X;
    of_xml_init(&X, src, len);
    int t;
    size_t ss_end = 0, nf0 = 0, nf_tag_end = 0, nf_close = 0, cx0 = 0, cx_tag_end = 0, cx_close = 0;
    int nf_count = 0, cx_count = 0;
    /* the raw <xf> of every original cell format */
    size_t *xf_a = of_alloc((size_t)(b->nxf + 1) * sizeof(size_t));
    size_t *xf_z = of_alloc((size_t)(b->nxf + 1) * sizeof(size_t));
    if (!xf_a || !xf_z) { of_free(xf_a); of_free(xf_z); return NULL; }
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_OPEN && of_xml_is(&X, "styleSheet")) ss_end = X.end;
        else if (t == OX_OPEN && of_xml_is(&X, "numFmts")) { nf0 = X.start; nf_tag_end = X.end; }
        else if (t == OX_CLOSE && of_xml_is(&X, "numFmts")) nf_close = X.start;
        else if ((t == OX_EMPTY || t == OX_OPEN) && of_xml_is(&X, "numFmt")) nf_count++;
        else if (t == OX_OPEN && of_xml_is(&X, "cellXfs")) {
            cx0 = X.start;
            cx_tag_end = X.end;
            int tt;
            while ((tt = of_xml_next(&X)) != OX_END && tt != OX_ERROR) {
                if (tt == OX_CLOSE && of_xml_is(&X, "cellXfs")) { cx_close = X.start; break; }
                if ((tt == OX_OPEN || tt == OX_EMPTY) && of_xml_is(&X, "xf")) {
                    size_t a = X.start;
                    if (tt == OX_OPEN) of_xml_skip(&X);
                    if (cx_count <= b->nxf) { xf_a[cx_count] = a; xf_z[cx_count] = X.end; }
                    cx_count++;
                }
            }
        }
    }
    if (!cx0 || !cx_close || !ss_end) { of_free(xf_a); of_free(xf_z); return NULL; }
    /* the formats the file does not have yet */
    of_sb_t nfs = { 0 };
    int added_nf = 0;
    for (int i = 0; i < b->nnf; i++) {
        if (b->nf[i].id < 164) continue;
        bool there = false;
        of_xml_t Y;
        of_xml_init(&Y, src + nf0, nf_close > nf0 ? nf_close - nf0 : 0);
        int ty;
        while ((ty = of_xml_next(&Y)) != OX_END && ty != OX_ERROR) {
            char id[16];
            if ((ty == OX_EMPTY || ty == OX_OPEN) && of_xml_is(&Y, "numFmt") && of_xml_attr(&Y, "numFmtId", id, sizeof id) &&
                (uint32_t)atoi(id) == b->nf[i].id) there = true;
        }
        if (there) continue;
        of_sb_printf(&nfs, "<numFmt numFmtId=\"%lu\" formatCode=\"", (unsigned long)b->nf[i].id);
        of_sb_xml(&nfs, b->nf[i].code, strlen(b->nf[i].code));
        of_sb_puts(&nfs, "\"/>");
        added_nf++;
    }
    /* write */
    size_t pos = 0;
    if (added_nf) {
        if (nf_close) {
            of_sb_put(&o, src, nf0);
            replace_count(&o, src + nf0, nf_tag_end - nf0, nf_count + added_nf);
            of_sb_put(&o, src + nf_tag_end, nf_close - nf_tag_end);
            of_sb_put(&o, nfs.s, nfs.n);
            pos = nf_close;
        } else {
            of_sb_put(&o, src, ss_end);
            of_sb_printf(&o, "<numFmts count=\"%d\">", added_nf);
            of_sb_put(&o, nfs.s, nfs.n);
            of_sb_puts(&o, "</numFmts>");
            pos = ss_end;
        }
    }
    of_sb_put(&o, src + pos, cx0 - pos);
    replace_count(&o, src + cx0, cx_tag_end - cx0, cx_count + (b->nxf - b->nxf_orig));
    of_sb_put(&o, src + cx_tag_end, cx_close - cx_tag_end);
    for (int i = b->nxf_orig; i < b->nxf; i++) {
        uint32_t from = b->xf[i].from;
        if ((int)from < cx_count && (int)from <= b->nxf) {
            /* its model with numFmtId and applyNumberFormat set */
            const char *xs = src + xf_a[from];
            size_t xl = xf_z[from] - xf_a[from];
            of_xml_t Y;
            of_xml_init(&Y, xs, xl);
            of_xml_next(&Y);
            of_sb_puts(&o, "<xf");
            const char *at = Y.attrs;
            int al = Y.attrs_len, k = 0;
            while (k < al) {
                while (k < al && (at[k] == ' ' || at[k] == '\n' || at[k] == '\t' || at[k] == '\r')) k++;
                int k0 = k;
                while (k < al && at[k] != '=') k++;
                int k1 = k;
                k++;
                while (k < al && at[k] != '"' && at[k] != '\'') k++;
                if (k >= al) break;
                char q = at[k++];
                while (k < al && at[k] != q) k++;
                k++;
                if ((k1 - k0 == 8 && !strncmp(at + k0, "numFmtId", 8)) ||
                    (k1 - k0 == 17 && !strncmp(at + k0, "applyNumberFormat", 17)))
                    continue;
                of_sb_putc(&o, ' ');
                of_sb_put(&o, at + k0, (size_t)(k - k0));
            }
            of_sb_printf(&o, " numFmtId=\"%lu\" applyNumberFormat=\"1\"", (unsigned long)b->xf[i].numfmt);
            /* the rest of the element as it was */
            if (Y.type == OX_EMPTY) of_sb_puts(&o, "/>");
            else {
                of_sb_putc(&o, '>');
                of_sb_put(&o, xs + Y.end, xl - Y.end);
            }
        } else {
            of_sb_printf(&o, "<xf numFmtId=\"%lu\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>",
                         (unsigned long)b->xf[i].numfmt);
        }
    }
    of_sb_put(&o, src + cx_close, len - cx_close);
    of_sb_free(&nfs);
    of_free(xf_a);
    of_free(xf_z);
    if (o.oom) { of_sb_free(&o); return NULL; }
    return of_sb_take(&o);
}

/* Removes the element (start tag to end, or empty) whose attribute 'attr'
 * ends with 'value', in a list part like [Content_Types].xml or a .rels. */
static char *drop_entry(const char *src, size_t len, const char *elem, const char *attr, const char *value)
{
    of_sb_t o = { 0 };
    of_xml_t X;
    of_xml_init(&X, src, len);
    int t;
    size_t pos = 0;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if ((t == OX_EMPTY || t == OX_OPEN) && of_xml_local_is(&X, elem)) {
            char v[256];
            size_t a = X.start;
            if (t == OX_OPEN) of_xml_skip(&X);
            of_xml_t Y;
            of_xml_init(&Y, src + a, X.end - a);
            of_xml_next(&Y);
            if (of_xml_attr(&Y, attr, v, sizeof v) && ends_with(v, value)) {
                of_sb_put(&o, src + pos, a - pos);
                pos = X.end;
            }
        }
    }
    of_sb_put(&o, src + pos, len - pos);
    return of_sb_take(&o);
}

/* workbook.xml with <calcPr fullCalcOnLoad="1"> */
static char *workbook_xml(const char *src, size_t len)
{
    of_sb_t o = { 0 };
    of_xml_t X;
    of_xml_init(&X, src, len);
    int t;
    size_t after = 0;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if ((t == OX_EMPTY || t == OX_OPEN) && of_xml_is(&X, "calcPr")) {
            const char *v;
            int vl;
            if (of_xml_attr_raw(X.attrs, X.attrs_len, "fullCalcOnLoad", &v, &vl)) {
                of_sb_put(&o, src, (size_t)(v - src));
                of_sb_putc(&o, '1');
                of_sb_put(&o, v + vl, len - (size_t)(v + vl - src));
            } else {
                size_t nm_end = (size_t)(X.name + X.name_len - src);
                of_sb_put(&o, src, nm_end);
                of_sb_puts(&o, " fullCalcOnLoad=\"1\"");
                of_sb_put(&o, src + nm_end, len - nm_end);
            }
            return of_sb_take(&o);
        }
        if (t == OX_CLOSE && (of_xml_is(&X, "sheets") || of_xml_is(&X, "functionGroups") ||
                              of_xml_is(&X, "externalReferences") || of_xml_is(&X, "definedNames")))
            after = X.end;
    }
    if (!after) return NULL;
    of_sb_put(&o, src, after);
    of_sb_puts(&o, "<calcPr fullCalcOnLoad=\"1\"/>");
    of_sb_put(&o, src + after, len - after);
    return of_sb_take(&o);
}

/* -------------------------------------------------------------------------- */
/* Saving into the same package                                                */
/* -------------------------------------------------------------------------- */

static bool save_keeping(of_book_t *b, const char *path, char *err, size_t errlen)
{
    bool any_dirty = false;
    for (int i = 0; i < b->nsh; i++) any_dirty |= b->sh[i].dirty || b->sh[i].cols_dirty;
    /* formula results that changed count as an edit of their sheet */
    for (int i = 0; i < b->nsh; i++)
        for (int r = 0; r < b->sh[i].nrows; r++)
            for (int k = 0; k < b->sh[i].rows[r].n; k++) {
                of_cell_t *c = &b->sh[i].rows[r].c[k];
                if (c->f && !same_val(&c->v, &c->cached) && !(c->flags & (OC_UNKNOWN_F | OC_ARRAY))) {
                    b->sh[i].dirty = true;
                    b->sh[i].rows[r].dirty = 1;
                    any_dirty = true;
                }
            }
    of_zipw_t w;
    of_zipw_init(&w);
    bool ok = true;
    for (int i = 0; i < b->zip.n && ok; i++) {
        const char *nm = b->zip.e[i].name;
        int sheet = -1;
        for (int s = 0; s < b->nsh; s++)
            if (b->sh[s].part && !strcmp(b->sh[s].part, nm)) sheet = s;
        char *out = NULL;
        if (sheet >= 0 && (b->sh[sheet].dirty || b->sh[sheet].cols_dirty)) {
            size_t l;
            char *src = (char *)of_zip_read(&b->zip, i, &l);
            if (src) out = sheet_xml(b, &b->sh[sheet], src, l);
            of_free(src);
            if (!out) { ok = false; snprintf(err, errlen, "sheet"); break; }
        } else if (b->styles_dirty && ends_with(nm, "styles.xml") && !strchr(nm + 3, '/')) {
            size_t l;
            char *src = (char *)of_zip_read(&b->zip, i, &l);
            if (src) out = styles_xml(b, src, l);
            of_free(src);
            if (!out) { ok = false; snprintf(err, errlen, "styles"); break; }
        } else if (any_dirty && ends_with(nm, "calcChain.xml")) {
            continue;
        } else if (any_dirty && !strcmp(nm, "[Content_Types].xml")) {
            size_t l;
            char *src = (char *)of_zip_read(&b->zip, i, &l);
            if (src) out = drop_entry(src, l, "Override", "PartName", "/calcChain.xml");
            of_free(src);
        } else if (any_dirty && ends_with(nm, "workbook.xml.rels")) {
            size_t l;
            char *src = (char *)of_zip_read(&b->zip, i, &l);
            if (src) out = drop_entry(src, l, "Relationship", "Target", "calcChain.xml");
            of_free(src);
        } else if (any_dirty && ends_with(nm, "/workbook.xml")) {
            size_t l;
            char *src = (char *)of_zip_read(&b->zip, i, &l);
            if (src) out = workbook_xml(src, l);
            of_free(src);
        }
        if (out) ok = of_zipw_add(&w, nm, out, strlen(out), true);
        else ok = of_zipw_copy(&w, &b->zip, i);
        of_free(out);
    }
    ok = ok && of_zipw_finish(&w, path);
    of_zipw_free(&w);
    if (!ok && !err[0]) snprintf(err, errlen, "write");
    return ok;
}

/* -------------------------------------------------------------------------- */
/* A new package                                                               */
/* -------------------------------------------------------------------------- */

/* A book from elsewhere has no cell formats of its own: one per number format. */
typedef struct {
    char **code;
    int    n;
} codes_t;

static int code_ix(codes_t *cs, const char *code)
{
    if (!code || !strcmp(code, "General")) return 0;
    for (int i = 0; i < cs->n; i++)
        if (!strcmp(cs->code[i], code)) return i + 1;
    char **n = of_realloc(cs->code, (size_t)(cs->n + 1) * sizeof *n);
    if (!n) return 0;
    cs->code = n;
    cs->code[cs->n++] = of_strdup(code);
    return cs->n;
}

static void xml_head(of_sb_t *o)
{
    of_sb_puts(o, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
}

static bool save_new(of_book_t *b, const char *path, char *err, size_t errlen)
{
    of_zipw_t w;
    of_zipw_init(&w);
    of_sb_t o = { 0 };
    int nsheets = 0;
    for (int i = 0; i < b->nsh; i++) nsheets++;

    xml_head(&o);
    of_sb_puts(&o, "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                   "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                   "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                   "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
                   "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>");
    for (int i = 0; i < nsheets; i++)
        of_sb_printf(&o, "<Override PartName=\"/xl/worksheets/sheet%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>", i + 1);
    of_sb_puts(&o, "</Types>");
    of_zipw_add(&w, "[Content_Types].xml", o.s, o.n, true);
    o.n = 0;

    xml_head(&o);
    of_sb_puts(&o, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                   "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
                   "</Relationships>");
    of_zipw_add(&w, "_rels/.rels", o.s, o.n, true);
    o.n = 0;

    xml_head(&o);
    of_sb_puts(&o, "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                   "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">");
    if (b->date1904) of_sb_puts(&o, "<workbookPr date1904=\"1\"/>");
    of_sb_puts(&o, "<bookViews><workbookView/></bookViews><sheets>");
    for (int i = 0; i < nsheets; i++) {
        of_sb_puts(&o, "<sheet name=\"");
        const char *nm = b->sh[i].name ? b->sh[i].name : "Hoja";
        of_sb_xml(&o, nm, strlen(nm));
        of_sb_printf(&o, "\" sheetId=\"%d\"%s r:id=\"rId%d\"/>", i + 1, b->sh[i].state == 1 ? " state=\"hidden\"" : "", i + 1);
    }
    of_sb_puts(&o, "</sheets><calcPr fullCalcOnLoad=\"1\"/></workbook>");
    of_zipw_add(&w, "xl/workbook.xml", o.s, o.n, true);
    o.n = 0;

    xml_head(&o);
    of_sb_puts(&o, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
    for (int i = 0; i < nsheets; i++)
        of_sb_printf(&o, "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet%d.xml\"/>", i + 1, i + 1);
    of_sb_printf(&o, "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>", nsheets + 1);
    of_sb_puts(&o, "</Relationships>");
    of_zipw_add(&w, "xl/_rels/workbook.xml.rels", o.s, o.n, true);
    o.n = 0;

    /* styles: the number formats, and bold/italic/colours as fonts and fills */
    codes_t cs = { 0 };
    bool foreign = b->fmt != OF_FMT_XLSX;
    if (foreign)
        for (int i = 0; i < b->nsh; i++)
            for (int r = 0; r < b->sh[i].nrows; r++)
                for (int k = 0; k < b->sh[i].rows[r].n; k++) {
                    of_cell_t *c = &b->sh[i].rows[r].c[k];
                    if (c->v.t == OV_NUM) code_ix(&cs, of_cell_code(b, c));
                }
    xml_head(&o);
    of_sb_puts(&o, "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">");
    int custom = 0;
    for (int i = 0; i < b->nnf && !foreign; i++)
        if (b->nf[i].id >= 164) custom++;
    if (foreign && cs.n) {
        of_sb_printf(&o, "<numFmts count=\"%d\">", cs.n);
        for (int i = 0; i < cs.n; i++) {
            of_sb_printf(&o, "<numFmt numFmtId=\"%d\" formatCode=\"", 164 + i);
            of_sb_xml(&o, cs.code[i], strlen(cs.code[i]));
            of_sb_puts(&o, "\"/>");
        }
        of_sb_puts(&o, "</numFmts>");
    }
    if (custom) {
        of_sb_printf(&o, "<numFmts count=\"%d\">", custom);
        for (int i = 0; i < b->nnf; i++) {
            if (b->nf[i].id < 164) continue;
            of_sb_printf(&o, "<numFmt numFmtId=\"%lu\" formatCode=\"", (unsigned long)b->nf[i].id);
            of_sb_xml(&o, b->nf[i].code, strlen(b->nf[i].code));
            of_sb_puts(&o, "\"/>");
        }
        of_sb_puts(&o, "</numFmts>");
    }
    int nxf = foreign ? 1 + cs.n : b->nxf ? b->nxf : 1;
    of_sb_printf(&o, "<fonts count=\"%d\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font>", nxf + 1);
    for (int i = 0; i < nxf; i++) {
        of_xf_t x = !foreign && i < b->nxf ? b->xf[i] : (of_xf_t){ 0 };
        of_sb_puts(&o, "<font>");
        if (x.bold) of_sb_puts(&o, "<b/>");
        if (x.italic) of_sb_puts(&o, "<i/>");
        if (x.underline) of_sb_puts(&o, "<u/>");
        of_sb_puts(&o, "<sz val=\"11\"/>");
        if (x.color) of_sb_printf(&o, "<color rgb=\"FF%06lX\"/>", (unsigned long)(x.color & 0xFFFFFF));
        of_sb_puts(&o, "<name val=\"Calibri\"/></font>");
    }
    of_sb_printf(&o, "</fonts><fills count=\"%d\"><fill><patternFill patternType=\"none\"/></fill>"
                     "<fill><patternFill patternType=\"gray125\"/></fill>", nxf + 2);
    for (int i = 0; i < nxf; i++) {
        of_xf_t x = !foreign && i < b->nxf ? b->xf[i] : (of_xf_t){ 0 };
        if (x.fill) of_sb_printf(&o, "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"FF%06lX\"/></patternFill></fill>",
                                 (unsigned long)(x.fill & 0xFFFFFF));
        else of_sb_puts(&o, "<fill><patternFill patternType=\"none\"/></fill>");
    }
    of_sb_puts(&o, "</fills><borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
                   "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>");
    of_sb_printf(&o, "<cellXfs count=\"%d\">", nxf);
    for (int i = 0; i < nxf; i++) {
        of_xf_t x = !foreign && i < b->nxf ? b->xf[i] : (of_xf_t){ 0 };
        if (foreign && i) x.numfmt = (uint32_t)(163 + i);
        static const char *const HA[4] = { "", "left", "center", "right" };
        of_sb_printf(&o, "<xf numFmtId=\"%lu\" fontId=\"%d\" fillId=\"%d\" borderId=\"0\" xfId=\"0\"%s%s%s",
                     (unsigned long)x.numfmt, i + 1, i + 2, x.numfmt ? " applyNumberFormat=\"1\"" : "",
                     (x.bold || x.italic || x.color) ? " applyFont=\"1\"" : "", x.fill ? " applyFill=\"1\"" : "");
        if (x.halign) of_sb_printf(&o, " applyAlignment=\"1\"><alignment horizontal=\"%s\"/></xf>", HA[x.halign & 3]);
        else of_sb_puts(&o, "/>");
    }
    of_sb_puts(&o, "</cellXfs><cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
                   "</styleSheet>");
    of_zipw_add(&w, "xl/styles.xml", o.s, o.n, true);
    o.n = 0;

    for (int i = 0; i < nsheets; i++) {
        of_sheet_t *s = &b->sh[i];
        xml_head(&o);
        of_sb_puts(&o, "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                       "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">");
        int32_t mr, mc;
        of_sheet_extent(s, &mr, &mc);
        char a[16];
        of_ref_name(mr < 0 ? 0 : mr, mc < 0 ? 0 : mc, a);
        of_sb_printf(&o, "<dimension ref=\"A1%s%s\"/>", mr > 0 || mc > 0 ? ":" : "", mr > 0 || mc > 0 ? a : "");
        if (s->freeze_r || s->freeze_c) {
            char tl[16];
            of_ref_name(s->freeze_r, s->freeze_c, tl);
            of_sb_puts(&o, "<sheetViews><sheetView workbookViewId=\"0\"><pane");
            if (s->freeze_c) of_sb_printf(&o, " xSplit=\"%d\"", s->freeze_c);
            if (s->freeze_r) of_sb_printf(&o, " ySplit=\"%d\"", s->freeze_r);
            of_sb_printf(&o, " topLeftCell=\"%s\" activePane=\"%s\" state=\"frozen\"/></sheetView></sheetViews>", tl,
                         s->freeze_r && s->freeze_c ? "bottomRight" : s->freeze_r ? "bottomLeft" : "topRight");
        }
        of_sb_printf(&o, "<sheetFormatPr defaultRowHeight=\"15\"/>");
        w_cols(&o, s);
        of_sb_puts(&o, "<sheetData>");
        for (int r = 0; r < s->nrows; r++) {
            of_row_t *row = &s->rows[r];
            bool any = false;
            for (int k = 0; k < row->n; k++)
                if (row->c[k].v.t != OV_EMPTY || row->c[k].f || row->c[k].style) any = true;
            if (!any && !(row->height > 0)) continue;
            of_sb_printf(&o, "<row r=\"%ld\"", (long)row->row + 1);
            if (row->height > 0.1f) of_sb_printf(&o, " ht=\"%.2f\" customHeight=\"1\"", (double)row->height);
            of_sb_putc(&o, '>');
            for (int k = 0; k < row->n; k++) {
                of_cell_t c = row->c[k];
                if (foreign) c.style = c.v.t == OV_NUM ? (uint32_t)code_ix(&cs, of_cell_code(b, &row->c[k])) : 0;
                w_cell(&o, b, row->row, &c, NULL, 0);
            }
            of_sb_puts(&o, "</row>");
        }
        of_sb_puts(&o, "</sheetData>");
        if (s->nmerges) {
            of_sb_printf(&o, "<mergeCells count=\"%d\">", s->nmerges);
            for (int m = 0; m < s->nmerges; m++) {
                char x1[16], x2[16];
                of_ref_name((int32_t)s->merges[m].r1, (int32_t)s->merges[m].c1, x1);
                of_ref_name((int32_t)s->merges[m].r2, (int32_t)s->merges[m].c2, x2);
                of_sb_printf(&o, "<mergeCell ref=\"%s:%s\"/>", x1, x2);
            }
            of_sb_puts(&o, "</mergeCells>");
        }
        of_sb_puts(&o, "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>"
                       "</worksheet>");
        char nm[48];
        snprintf(nm, sizeof nm, "xl/worksheets/sheet%d.xml", i + 1);
        of_zipw_add(&w, nm, o.s, o.n, true);
        o.n = 0;
    }
    bool ok = !o.oom && of_zipw_finish(&w, path);
    of_sb_free(&o);
    of_zipw_free(&w);
    for (int i = 0; i < cs.n; i++) of_free(cs.code[i]);
    of_free(cs.code);
    if (!ok) snprintf(err, errlen, "write");
    return ok;
}

bool of_xlsx_save(of_book_t *b, const char *path, char *err, size_t errlen)
{
    err[0] = 0;
    if (b->fmt == OF_FMT_XLSX && b->zip.n) return save_keeping(b, path, err, errlen);
    return save_new(b, path, err, errlen);
}
