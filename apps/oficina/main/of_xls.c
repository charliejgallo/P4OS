/*
 * P4OS - Office: Excel 97-2003 (.xls), BIFF8, read into the workbook.
 *
 * The Workbook stream's globals: the shared strings (SST, its CONTINUE
 * records and the character width flag they repeat), number formats,
 * fonts, cell formats (number format, bold, italics, underline, colour,
 * solid fill, horizontal alignment) with the palette, the 1904 date mode,
 * the sheets (hidden ones too). Each sheet: numbers, RK and MULRK, strings
 * (LABELSST, LABEL, RSTRING), booleans and errors, formulas' last results
 * (the formulas themselves are not read: the values stay), column widths,
 * row heights and hidden rows and columns, merged cells, frozen panes.
 * The book opens as an .xls: edited in memory, saved as a copy (xlsx, ods,
 * csv, pdf). Excel 5/95 and encrypted files are said so.
 */
#include "of_cfb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static double rd64(const uint8_t *p)
{
    double d;
    memcpy(&d, p, 8);
    return d;
}

static double rk_value(uint32_t rk)
{
    double v;
    if (rk & 2) {
        int32_t i = (int32_t)rk >> 2;
        v = i;
    } else {
        uint64_t bits = (uint64_t)(rk & 0xFFFFFFFCu) << 32;
        memcpy(&v, &bits, 8);
    }
    return rk & 1 ? v / 100 : v;
}

static const uint32_t PAL[56] = {
    0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF, 0x800000, 0x008000, 0x000080, 0x808000,
    0x800080, 0x008080, 0xC0C0C0, 0x808080, 0x9999FF, 0x993366, 0xFFFFCC, 0xCCFFFF, 0x660066, 0xFF8080, 0x0066CC, 0xCCCCFF,
    0x000080, 0xFF00FF, 0xFFFF00, 0x00FFFF, 0x800080, 0x800000, 0x008080, 0x0000FF, 0x00CCFF, 0xCCFFFF, 0xCCFFCC, 0xFFFF99,
    0x99CCFF, 0xFF99CC, 0xCC99FF, 0xFFCC99, 0x3366FF, 0x33CCCC, 0x99CC00, 0xFFCC00, 0xFF9900, 0xFF6600, 0x666699, 0x969696,
    0x003366, 0x339966, 0x003300, 0x333300, 0x993300, 0x993366, 0x333399, 0x333333 };

typedef struct {
    const uint8_t *s;
    size_t         n;
    size_t         p;
    /* the record's chain: where each CONTINUE starts, for strings that cross */
    size_t        *brk;
    int            nbrk;
} rd_t;

/* the records: each with its data in one buffer, CONTINUEs appended, their starts remembered */
typedef struct { uint16_t type; uint8_t *d; size_t n; size_t *brk; int nbrk; size_t at; } rec_t;

static bool next_rec(const uint8_t *w, size_t wn, size_t *pos, rec_t *r)
{
    if (*pos + 4 > wn) return false;
    r->type = rd16(w + *pos);
    size_t len = rd16(w + *pos + 2);
    r->at = *pos;
    *pos += 4;
    if (*pos + len > wn) len = wn - *pos;
    of_free(r->d);
    of_free(r->brk);
    r->brk = NULL;
    r->nbrk = 0;
    r->d = of_alloc(len + 1);
    if (!r->d) return false;
    memcpy(r->d, w + *pos, len);
    r->n = len;
    *pos += len;
    /* CONTINUE records belong to this one */
    while (*pos + 4 <= wn && rd16(w + *pos) == 0x003C) {
        size_t cl = rd16(w + *pos + 2);
        *pos += 4;
        if (*pos + cl > wn) cl = wn - *pos;
        uint8_t *nd = of_realloc(r->d, r->n + cl + 1);
        size_t *nb = of_realloc(r->brk, (size_t)(r->nbrk + 1) * sizeof *nb);
        if (!nd || !nb) { if (nd) r->d = nd; if (nb) r->brk = nb; break; }
        r->d = nd;
        r->brk = nb;
        r->brk[r->nbrk++] = r->n;
        memcpy(r->d + r->n, w + *pos, cl);
        r->n += cl;
        *pos += cl;
    }
    return true;
}

/* An XLUnicodeRichExtendedString (or a plain XLUnicodeString with rich = false)
 * at *p; a CONTINUE in the middle of its characters repeats the width flag. */
static char *ustr(const rec_t *r, size_t *p, bool rich, bool len8)
{
    if (*p + (len8 ? 2 : 3) > r->n) { *p = r->n; return of_strdup(""); }
    size_t cch = len8 ? r->d[*p] : rd16(r->d + *p);
    *p += len8 ? 1 : 2;
    uint8_t fl = r->d[(*p)++];
    int runs = 0;
    uint32_t ext = 0;
    if (rich && (fl & 0x08) && *p + 2 <= r->n) { runs = rd16(r->d + *p); *p += 2; }
    if (rich && (fl & 0x04) && *p + 4 <= r->n) { ext = rd32(r->d + *p); *p += 4; }
    bool high = fl & 1;
    of_sb_t sb = { 0 };
    for (size_t i = 0; i < cch && *p < r->n; i++) {
        /* a continue boundary here: a new flag byte */
        for (int b = 0; b < r->nbrk; b++)
            if (r->brk[b] == *p && i > 0) { high = r->d[(*p)++] & 1; break; }
        if (*p + (high ? 2 : 1) > r->n) break;
        uint32_t ch = high ? rd16(r->d + *p) : r->d[*p];
        *p += high ? 2 : 1;
        if (high && ch >= 0xD800 && ch < 0xDC00 && *p + 2 <= r->n) {
            uint32_t lo = rd16(r->d + *p);
            if (lo >= 0xDC00 && lo < 0xE000) { ch = 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00); *p += 2; i++; }
        }
        if (!high && ch >= 0x80 && ch < 0xA0) {
            uint8_t b = (uint8_t)ch;
            size_t m;
            char *u = of_latin1_to_utf8((const char *)&b, 1, &m);
            int l;
            if (u) { ch = of_utf8_get(u, &l); of_free(u); }
        }
        char u[4];
        of_sb_put(&sb, u, (size_t)of_utf8_put(u, ch ? ch : ' '));
    }
    *p += (size_t)runs * 4 + ext;
    if (*p > r->n) *p = r->n;
    return sb.s ? sb.s : of_strdup("");
}

typedef struct { uint8_t b, i, u; uint32_t color; } font_t;

of_book_t *of_xls_load(const char *path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *raw = of_file_read(path, &len, 64u * 1024 * 1024);
    if (!raw) { snprintf(err, errlen, "read"); return NULL; }
    of_cfb_t C;
    if (!of_cfb_open(&C, raw, len)) { of_free(raw); snprintf(err, errlen, "format"); return NULL; }
    int wi = of_cfb_find(&C, "Workbook");
    if (wi < 0) wi = of_cfb_find(&C, "Book");
    size_t wn = 0;
    uint8_t *w = wi >= 0 ? of_cfb_read(&C, wi, &wn) : NULL;
    if (!w) {
        bool enc = of_cfb_find(&C, "EncryptedPackage") >= 0;
        of_cfb_close(&C);
        of_free(raw);
        snprintf(err, errlen, "%s", enc ? "encrypted" : "format");
        return NULL;
    }
    of_book_t *b = of_book_new(OF_FMT_XLS);
    uint32_t pal[56];
    memcpy(pal, PAL, sizeof pal);
    font_t *fonts = NULL;
    int nfonts = 0;
    struct { uint32_t pos; uint8_t hidden, type; char *name; } *bs = NULL;
    int nbs = 0;
    rec_t r = { 0 };
    size_t pos = 0;
    bool ok = true;
    /* the globals */
    while (next_rec(w, wn, &pos, &r)) {
        if (r.type == 0x0809) {
            if (r.n >= 2 && rd16(r.d) != 0x0600) { snprintf(err, errlen, "old"); ok = false; break; }
        } else if (r.type == 0x002F) {
            snprintf(err, errlen, "encrypted");
            ok = false;
            break;
        } else if (r.type == 0x0022 && r.n >= 2) {
            b->date1904 = rd16(r.d) == 1;
        } else if (r.type == 0x0092 && r.n >= 2) {
            int n = rd16(r.d);
            for (int i = 0; i < n && i < 56 && 2 + (size_t)i * 4 + 3 <= r.n; i++)
                pal[i] = (uint32_t)r.d[2 + i * 4] << 16 | (uint32_t)r.d[3 + i * 4] << 8 | r.d[4 + i * 4];
        } else if (r.type == 0x0031 && r.n >= 14) {
            /* font index 4 does not exist: the fifth record is index 5 */
            font_t *nf = of_realloc(fonts, (size_t)(nfonts + 2) * sizeof *nf);
            if (!nf) break;
            fonts = nf;
            if (nfonts == 4) memset(&fonts[nfonts++], 0, sizeof *fonts);
            font_t *f = &fonts[nfonts++];
            uint16_t gr = rd16(r.d + 2), icv = rd16(r.d + 4), bls = rd16(r.d + 6);
            f->b = bls >= 600;
            f->i = (gr & 2) != 0;
            f->u = r.d[10] != 0;
            f->color = icv >= 8 && icv < 64 && icv != 8 ? pal[icv - 8] | 0x1000000u : 0;
        } else if (r.type == 0x041E && r.n >= 5) {
            size_t p = 2;
            char *code = ustr(&r, &p, false, false);
            of_numfmt_t *nf = of_realloc(b->nf, (size_t)(b->nnf + 1) * sizeof *nf);
            if (nf) { b->nf = nf; b->nf[b->nnf].id = rd16(r.d); b->nf[b->nnf].code = code; b->nnf++; }
            else of_free(code);
        } else if (r.type == 0x00E0 && r.n >= 20) {
            of_xf_t xf;
            memset(&xf, 0, sizeof xf);
            uint16_t ifnt = rd16(r.d);
            xf.numfmt = rd16(r.d + 2);
            int alc = r.d[6] & 7;
            xf.halign = (uint8_t)(alc == 1 ? 1 : alc == 2 ? 2 : alc == 3 ? 3 : 0);
            if (ifnt < nfonts) { xf.bold = fonts[ifnt].b; xf.italic = fonts[ifnt].i; xf.underline = fonts[ifnt].u; xf.color = fonts[ifnt].color; }
            uint32_t d14 = rd32(r.d + 14);
            int fls = (int)(d14 >> 26) & 0x3F;
            int fore = rd16(r.d + 18) & 0x7F;
            if (fls == 1 && fore >= 8 && fore < 64) xf.fill = pal[fore - 8] | 0x1000000u;
            of_xf_t *nx = of_realloc(b->xf, (size_t)(b->nxf + 1) * sizeof *nx);
            if (nx) { b->xf = nx; xf.from = (uint32_t)b->nxf; b->xf[b->nxf++] = xf; }
        } else if (r.type == 0x0085 && r.n >= 8) {
            void *nb = of_realloc(bs, (size_t)(nbs + 1) * sizeof *bs);
            if (!nb) break;
            bs = nb;
            bs[nbs].pos = rd32(r.d);
            bs[nbs].hidden = r.d[4] & 3;
            bs[nbs].type = r.d[5];
            size_t p = 6;
            bs[nbs].name = ustr(&r, &p, false, true);
            nbs++;
        } else if (r.type == 0x00FC && r.n >= 8) {
            uint32_t cnt = rd32(r.d + 4);
            if (cnt > 4000000) cnt = 0;
            b->sst = of_alloc((size_t)(cnt + 1) * sizeof(char *));
            size_t p = 8;
            for (uint32_t i = 0; b->sst && i < cnt && p < r.n; i++) b->sst[b->nsst++] = ustr(&r, &p, true, false);
        } else if (r.type == 0x000A) {
            break;
        }
    }
    b->nxf_orig = b->nxf;
    /* the sheets */
    for (int s = 0; ok && s < nbs; s++) {
        if (bs[s].type != 0) continue;              /* charts and macro sheets */
        of_sheet_t *sh = of_book_add_sheet(b, bs[s].name ? bs[s].name : "Hoja");
        if (!sh) break;
        sh->state = bs[s].hidden ? 1 : 0;
        pos = bs[s].pos;
        int32_t frow = -1, fcol = -1;
        of_cell_t *last_formula = NULL;
        bool frozen = false;
        int pane_x = 0, pane_y = 0;
        int depth = 0;
        while (next_rec(w, wn, &pos, &r)) {
            const uint8_t *d = r.d;
            if (r.type == 0x0809) { depth++; continue; }
            if (r.type == 0x000A) { if (--depth <= 0) break; continue; }
            if (depth > 1) continue;                /* an embedded chart's records */
            int32_t row = r.n >= 4 ? rd16(d) : 0, col = r.n >= 4 ? rd16(d + 2) : 0;
            uint16_t xf = r.n >= 6 ? rd16(d + 4) : 0;
            of_cell_t *c = NULL;
            switch (r.type) {
            case 0x0203:
                if (r.n < 14) break;
                c = of_cell_make(sh, row, col);
                if (c) { c->v.t = OV_NUM; c->v.n = rd64(d + 6); }
                break;
            case 0x027E:
                if (r.n < 10) break;
                c = of_cell_make(sh, row, col);
                if (c) { c->v.t = OV_NUM; c->v.n = rk_value(rd32(d + 6)); }
                break;
            case 0x00BD: {
                if (r.n < 6) break;
                int last = rd16(d + r.n - 2);
                for (int k = col, q = 4; k <= last && (size_t)q + 6 <= r.n - 2; k++, q += 6) {
                    of_cell_t *m = of_cell_make(sh, row, k);
                    if (!m) break;
                    m->v.t = OV_NUM;
                    m->v.n = rk_value(rd32(d + q + 2));
                    m->style = rd16(d + q);
                }
                break;
            }
            case 0x00FD:
                if (r.n < 10) break;
                c = of_cell_make(sh, row, col);
                if (c) {
                    uint32_t i = rd32(d + 6);
                    c->v.t = OV_STR;
                    c->v.s = of_strdup(i < (uint32_t)b->nsst && b->sst[i] ? b->sst[i] : "");
                }
                break;
            case 0x0204: case 0x00D6: {
                if (r.n < 9) break;
                size_t p = 6;
                char *t = ustr(&r, &p, false, false);
                c = of_cell_make(sh, row, col);
                if (c) { c->v.t = OV_STR; c->v.s = t; }
                else of_free(t);
                break;
            }
            case 0x0205:
                if (r.n < 8) break;
                c = of_cell_make(sh, row, col);
                if (c) {
                    if (d[7]) {
                        static const struct { uint8_t c; int e; } E[] = { { 0x00, OE_NULL }, { 0x07, OE_DIV0 }, { 0x0F, OE_VALUE }, { 0x17, OE_REF },
                                                                         { 0x1D, OE_NAME }, { 0x24, OE_NUM }, { 0x2A, OE_NA } };
                        c->v.t = OV_ERR;
                        c->v.err = OE_VALUE;
                        for (size_t k = 0; k < sizeof E / sizeof E[0]; k++) if (E[k].c == d[6]) c->v.err = (uint8_t)E[k].e;
                    } else {
                        c->v.t = OV_BOOL;
                        c->v.n = d[6] ? 1 : 0;
                    }
                }
                break;
            case 0x0006:
                if (r.n < 20) break;
                c = of_cell_make(sh, row, col);
                if (!c) break;
                last_formula = NULL;
                if (rd16(d + 12) == 0xFFFF) {
                    switch (d[6]) {
                    case 0: last_formula = c; c->v.t = OV_STR; c->v.s = of_strdup(""); break;
                    case 1: c->v.t = OV_BOOL; c->v.n = d[8] ? 1 : 0; break;
                    case 2: c->v.t = OV_ERR; c->v.err = OE_VALUE; break;
                    default: c->v.t = OV_STR; c->v.s = of_strdup(""); break;
                    }
                } else {
                    c->v.t = OV_NUM;
                    c->v.n = rd64(d + 6);
                }
                break;
            case 0x0207:
                if (last_formula && r.n >= 3) {
                    size_t p = 0;
                    of_free(last_formula->v.s);
                    last_formula->v.s = ustr(&r, &p, false, false);
                    last_formula = NULL;
                }
                break;
            case 0x007D:
                if (r.n >= 10) {
                    int c1 = rd16(d), c2 = rd16(d + 2);
                    float wd = rd16(d + 4) / 256.0f;
                    if (rd16(d + 8) & 1) wd = 0.01f;
                    for (int k = c1; k <= c2 && k < 256; k++) of_sheet_set_colw(sh, k, wd);
                }
                break;
            case 0x0055:
                if (r.n >= 2) sh->def_colw = rd16(d) + 0.7f;
                break;
            case 0x0208:
                if (r.n >= 16) {
                    of_row_t *rw = of_row_make(sh, row);
                    uint16_t ht = rd16(d + 6) & 0x7FFF;
                    uint32_t fl = rd32(d + 12);
                    if (rw) {
                        if (fl & 0x40) rw->height = ht / 20.0f;
                        if (fl & 0x20) rw->height = 0.01f;
                    }
                }
                break;
            case 0x00E5:
                if (r.n >= 2) {
                    int n = rd16(d);
                    for (int k = 0; k < n && 2 + (size_t)k * 8 + 8 <= r.n; k++) {
                        const uint8_t *m = d + 2 + k * 8;
                        of_range_t *nm = of_realloc(sh->merges, (size_t)(sh->nmerges + 1) * sizeof *nm);
                        if (!nm) break;
                        sh->merges = nm;
                        sh->merges[sh->nmerges++] = (of_range_t){ rd16(m + 4), rd16(m), rd16(m + 6), rd16(m + 2) };
                    }
                }
                break;
            case 0x023E:
                if (r.n >= 2) frozen = (rd16(d) & 0x0008) != 0;
                break;
            case 0x0041:
                if (r.n >= 4) { pane_x = rd16(d); pane_y = rd16(d + 2); }
                break;
            default: break;
            }
            if (c && r.type != 0x00BD) c->style = xf;
            (void)frow;
            (void)fcol;
        }
        if (frozen) { sh->freeze_r = pane_y; sh->freeze_c = pane_x; }
        /* the widths are the file's, not the person's */
        if (sh->colw_set) memset(sh->colw_set, 0, (size_t)sh->ncolw);
    }
    of_free(r.d);
    of_free(r.brk);
    for (int s = 0; s < nbs; s++) of_free(bs[s].name);
    of_free(bs);
    of_free(fonts);
    of_free(w);
    of_cfb_close(&C);
    of_free(raw);
    if (!ok) { of_book_free(b); return NULL; }
    if (!b->nsh) of_book_add_sheet(b, "Hoja1");
    return b;
}
