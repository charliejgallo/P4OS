/*
 * P4OS - Office: the workbook.
 *
 * A sheet is a sorted array of rows that have something, each a sorted
 * array of the cells that have something: a sheet with ten thousand cells
 * costs ten thousand cells, whatever its size. Lookups are binary searches.
 *
 * A cell holds its value (or its formula and the result), the style index
 * of its format, and where its XML was in the file, for the writers to copy
 * it as it was when nobody touched it. Pointers to cells do not survive
 * making another cell in the same row (the row's array may move).
 */
#include "of.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool g_comma = true;

static int g_lang = 1;

void of_locale_set(bool decimal_comma) { g_comma = decimal_comma; }
bool of_locale_comma(void) { return g_comma; }
void of_locale_lang(int lang) { g_lang = lang; }
int  of_locale_get_lang(void) { return g_lang; }

const char *of_bool_word(bool v)
{
    if (g_lang == 1) return v ? "VERDADERO" : "FALSO";
    if (g_lang == 2) return v ? "WAHR" : "FALSCH";
    return v ? "TRUE" : "FALSE";
}

/* -------------------------------------------------------------------------- */
/* Values                                                                      */
/* -------------------------------------------------------------------------- */

void of_val_free(of_val_t *v)
{
    if (v->t == OV_STR) of_free(v->s);
    memset(v, 0, sizeof *v);
}

void of_val_copy(of_val_t *d, const of_val_t *s)
{
    *d = *s;
    if (s->t == OV_STR) d->s = of_strdup(s->s ? s->s : "");
}

static const char *const ERR_NAMES[] = {
    "", "#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A", "#CIRC!" };

const char *of_err_name(int e)
{
    return e > 0 && e < (int)(sizeof ERR_NAMES / sizeof ERR_NAMES[0]) ? ERR_NAMES[e] : "#VALUE!";
}

int of_err_from_name(const char *s)
{
    for (int i = 1; i < (int)(sizeof ERR_NAMES / sizeof ERR_NAMES[0]); i++)
        if (strcasecmp(s, ERR_NAMES[i]) == 0) return i;
    if (strncasecmp(s, "Err:", 4) == 0) return OE_VALUE;
    return OE_VALUE;
}

/* -------------------------------------------------------------------------- */
/* The book                                                                    */
/* -------------------------------------------------------------------------- */

of_book_t *of_book_new(int fmt)
{
    of_book_t *b = of_alloc(sizeof *b);
    if (!b) return NULL;
    b->fmt = fmt;
    b->csv_sep = g_comma ? ';' : ',';
    return b;
}

static void cell_free(of_cell_t *c)
{
    of_val_free(&c->v);
    of_val_free(&c->cached);
    of_free(c->f);
    of_formula_free(c->ast);
    of_free(c->disp);
}

static void sheet_free(of_sheet_t *s)
{
    for (int r = 0; r < s->nrows; r++) {
        of_row_t *row = &s->rows[r];
        for (int i = 0; i < row->n; i++) cell_free(&row->c[i]);
        of_free(row->c);
        if (row->fx && row->fx_free) row->fx_free(row->fx);
    }
    of_free(s->rows);
    of_free(s->colw);
    of_free(s->colw_set);
    of_free(s->merges);
    of_free(s->name);
    of_free(s->part);
    of_free(s->rid);
}

void of_book_free(of_book_t *b)
{
    if (!b) return;
    for (int i = 0; i < b->nsh; i++) sheet_free(&b->sh[i]);
    of_free(b->sh);
    for (int i = 0; i < b->nnf; i++) of_free(b->nf[i].code);
    of_free(b->nf);
    of_free(b->xf);
    for (int i = 0; i < b->nsst; i++) of_free(b->sst[i]);
    of_free(b->sst);
    for (int i = 0; i < b->nsty; i++) of_free(b->sty[i]);
    of_free(b->sty);
    if (b->sty_code)
        for (int i = 0; i < b->nsty; i++) of_free(b->sty_code[i]);
    of_free(b->sty_code);
    of_zip_close(&b->zip);
    of_free(b->file);
    of_free(b->ods_content);
    of_free(b);
}

of_sheet_t *of_book_add_sheet(of_book_t *b, const char *name)
{
    of_sheet_t *n = of_realloc(b->sh, (size_t)(b->nsh + 1) * sizeof *n);
    if (!n) return NULL;
    b->sh = n;
    of_sheet_t *s = &b->sh[b->nsh++];
    memset(s, 0, sizeof *s);
    s->name = of_strdup(name);
    s->def_colw = 10.7f;
    s->def_rowh = 15.0f;
    return s;
}

/* -------------------------------------------------------------------------- */
/* Rows and cells                                                              */
/* -------------------------------------------------------------------------- */

static int row_find(const of_sheet_t *s, int32_t row, bool *found)
{
    int lo = 0, hi = s->nrows;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s->rows[mid].row < row) lo = mid + 1;
        else hi = mid;
    }
    *found = lo < s->nrows && s->rows[lo].row == row;
    return lo;
}

static int cell_find(const of_row_t *r, int32_t col, bool *found)
{
    int lo = 0, hi = r->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (r->c[mid].col < col) lo = mid + 1;
        else hi = mid;
    }
    *found = lo < r->n && r->c[lo].col == col;
    return lo;
}

of_row_t *of_row_get(const of_sheet_t *s, int32_t row)
{
    bool f;
    int i = row_find(s, row, &f);
    return f ? &s->rows[i] : NULL;
}

of_row_t *of_row_make(of_sheet_t *s, int32_t row)
{
    bool f;
    int i = row_find(s, row, &f);
    if (f) return &s->rows[i];
    if (s->nrows == s->rcap) {
        int cap = s->rcap ? s->rcap * 2 : 64;
        of_row_t *n = of_realloc(s->rows, (size_t)cap * sizeof *n);
        if (!n) return NULL;
        s->rows = n;
        s->rcap = cap;
    }
    memmove(&s->rows[i + 1], &s->rows[i], (size_t)(s->nrows - i) * sizeof *s->rows);
    s->nrows++;
    of_row_t *r = &s->rows[i];
    memset(r, 0, sizeof *r);
    r->row = row;
    return r;
}

of_cell_t *of_cell_get(const of_sheet_t *s, int32_t row, int32_t col)
{
    of_row_t *r = of_row_get(s, row);
    if (!r) return NULL;
    bool f;
    int i = cell_find(r, col, &f);
    return f ? &r->c[i] : NULL;
}

of_cell_t *of_cell_make(of_sheet_t *s, int32_t row, int32_t col)
{
    of_row_t *r = of_row_make(s, row);
    if (!r) return NULL;
    bool f;
    int i = cell_find(r, col, &f);
    if (f) return &r->c[i];
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 8;
        of_cell_t *n = of_realloc(r->c, (size_t)cap * sizeof *n);
        if (!n) return NULL;
        r->c = n;
        r->cap = cap;
    }
    memmove(&r->c[i + 1], &r->c[i], (size_t)(r->n - i) * sizeof *r->c);
    r->n++;
    of_cell_t *c = &r->c[i];
    memset(c, 0, sizeof *c);
    c->col = col;
    return c;
}

void of_cell_clear_value(of_cell_t *c)
{
    of_val_free(&c->v);
    of_free(c->f);
    c->f = NULL;
    of_formula_free(c->ast);
    c->ast = NULL;
    of_free(c->disp);
    c->disp = NULL;
    c->flags &= (uint8_t)~(OC_FORMULA | OC_UNKNOWN_F | OC_ARRAY | OC_SHARED);
    c->shared_si = 0;
    c->hint = 0;
}

void of_sheet_extent(const of_sheet_t *s, int32_t *maxr, int32_t *maxc)
{
    int32_t mr = -1, mc = -1;
    for (int i = 0; i < s->nrows; i++) {
        const of_row_t *r = &s->rows[i];
        for (int k = 0; k < r->n; k++) {
            const of_cell_t *c = &r->c[k];
            if (c->v.t == OV_EMPTY && !c->f) continue;
            if (r->row > mr) mr = r->row;
            if (c->col > mc) mc = c->col;
        }
    }
    *maxr = mr;
    *maxc = mc;
}

float of_sheet_colw(const of_sheet_t *s, int col)
{
    if (col >= 0 && col < s->ncolw && s->colw[col] > 0) return s->colw[col];
    return s->def_colw > 0 ? s->def_colw : 10.7f;
}

void of_sheet_set_colw(of_sheet_t *s, int col, float w)
{
    if (col < 0 || col > 16383) return;
    if (col >= s->ncolw) {
        int n = col + 16;
        float *cw = of_realloc(s->colw, (size_t)n * sizeof *cw);
        uint8_t *set = of_realloc(s->colw_set, (size_t)n);
        if (!cw || !set) {
            if (cw) s->colw = cw;
            if (set) s->colw_set = set;
            return;
        }
        for (int i = s->ncolw; i < n; i++) {
            cw[i] = 0;
            set[i] = 0;
        }
        s->colw = cw;
        s->colw_set = set;
        s->ncolw = n;
    }
    s->colw[col] = w;
    s->colw_set[col] = 1;
    s->cols_dirty = true;
}

/* -------------------------------------------------------------------------- */
/* A1                                                                          */
/* -------------------------------------------------------------------------- */

void of_col_name(int32_t col, char *out)
{
    char tmp[8];
    int n = 0;
    col++;
    while (col > 0 && n < 7) {
        int r = (col - 1) % 26;
        tmp[n++] = (char)('A' + r);
        col = (col - 1) / 26;
    }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

void of_ref_name(int32_t row, int32_t col, char *out)
{
    of_col_name(col, out);
    size_t l = strlen(out);
    snprintf(out + l, 16 - l, "%ld", (long)row + 1);
}

bool of_parse_ref(const char *s, int32_t *row, int32_t *col, int *used)
{
    int i = 0;
    if (s[i] == '$') i++;
    int32_t c = 0;
    int nl = 0;
    while (((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')) && nl < 4) {
        char ch = s[i] >= 'a' ? (char)(s[i] - 'a' + 'A') : s[i];
        c = c * 26 + (ch - 'A' + 1);
        i++;
        nl++;
    }
    if (!nl) return false;
    if (s[i] == '$') i++;
    int32_t r = 0;
    int nd = 0;
    while (s[i] >= '0' && s[i] <= '9' && nd < 8) {
        r = r * 10 + (s[i] - '0');
        i++;
        nd++;
    }
    if (!nd || r < 1 || c > 16384 || r > 1048576) return false;
    *row = r - 1;
    *col = c - 1;
    if (used) *used = i;
    return true;
}

/* -------------------------------------------------------------------------- */
/* What the person types                                                       */
/* -------------------------------------------------------------------------- */

static bool parse_number(const char *s, double *out, int *pct, int *cur, int *decimals)
{
    char buf[64];
    int k = 0;
    *pct = 0;
    *cur = 0;
    *decimals = -1;
    const char *p = s;
    while (*p == ' ') p++;
    bool neg = false;
    if (*p == '-' || *p == '+') {
        neg = *p == '-';
        p++;
    }
    if (*p == '$' || (unsigned char)*p == 0xE2) {       /* $ or € (E2 82 AC) */
        if (*p == '$') p++;
        else if ((unsigned char)p[1] == 0x82 && (unsigned char)p[2] == 0xAC) p += 3;
        else return false;
        *cur = 1;
        while (*p == ' ') p++;
        if (*p == '-') {
            neg = !neg;
            p++;
        }
    }
    char dec = g_comma ? ',' : '.', grp = g_comma ? '.' : ',';
    /* both separators present: the last one is the decimal one */
    const char *lc = strrchr(p, ','), *ld = strrchr(p, '.');
    if (lc && ld) {
        dec = lc > ld ? ',' : '.';
        grp = dec == ',' ? '.' : ',';
    }
    bool digits = false, seen_dec = false, exp = false;
    int nd = 0;
    for (; *p; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            buf[k++] = c;
            digits = true;
            if (seen_dec && !exp) nd++;
        } else if (c == dec && !seen_dec && !exp) {
            buf[k++] = '.';
            seen_dec = true;
        } else if (c == grp && !seen_dec && !exp && digits) {
            /* grouping: exactly three digits must follow */
            if (!(p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9' &&
                  !(p[4] >= '0' && p[4] <= '9'))) return false;
        } else if ((c == 'e' || c == 'E') && digits && !exp) {
            buf[k++] = 'e';
            exp = true;
            if (p[1] == '-' || p[1] == '+') buf[k++] = *++p;
        } else if (c == '%' && digits) {
            *pct = 1;
            p++;
            while (*p == ' ') p++;
            if (*p) return false;
            break;
        } else if (c == ' ' && digits) {
            while (*p == ' ') p++;
            if (*p == '%') { *pct = 1; p++; }
            else if (*p == '$') { *cur = 1; p++; }
            else if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x82 && (unsigned char)p[2] == 0xAC) { *cur = 1; p += 3; }
            while (*p == ' ') p++;
            if (*p) return false;
            break;
        } else {
            return false;
        }
        if (k >= 60) return false;
    }
    if (!digits) return false;
    buf[k] = 0;
    double v = strtod(buf, NULL);
    if (neg) v = -v;
    if (*pct) v /= 100;
    *out = v;
    *decimals = seen_dec ? nd : 0;
    return true;
}

static bool parse_date(const char *s, double *out, bool *iso)
{
    int a, b, c;
    char t1, t2;
    *iso = false;
    if (sscanf(s, "%d-%d-%d", &a, &b, &c) == 3 && a > 1000 && strchr(s, '-') - s == 4) {
        if (b < 1 || b > 12 || c < 1 || c > 31) return false;
        *out = of_date_serial(a, b, c);
        *iso = true;
        return true;
    }
    if (sscanf(s, "%d%c%d%c%d", &a, &t1, &b, &t2, &c) == 5 && t1 == t2 && (t1 == '/' || t1 == '.' || t1 == '-')) {
        int d = a, m = b, y = c;
        if (!g_comma) { m = a; d = b; }     /* English: m/d/y */
        if (y < 100) y += y < 70 ? 2000 : 1900;
        if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1900 || y > 9999) return false;
        *out = of_date_serial(y, m, d);
        return true;
    }
    return false;
}

static void set_format_if_general(of_book_t *b, of_cell_t *c, const char *code, int hint)
{
    c->hint = (uint8_t)hint;
    if (!b || b->fmt == OF_FMT_CSV || b->fmt == OF_FMT_TSV || b->fmt == OF_FMT_ODS) return;
    if (b->nxf == 0) {
        b->xf = of_alloc(sizeof *b->xf);
        if (!b->xf) return;
        b->nxf = 1;
        b->nxf_orig = 1;
    }
    const char *now = of_numfmt_code(b, c->style < (uint32_t)b->nxf ? b->xf[c->style].numfmt : 0);
    if (now && strcmp(now, "General") != 0) return;
    c->style = of_book_xf_with_numfmt(b, c->style, code);
    c->flags |= OC_STYLED;
}

void of_sheet_unshare(of_sheet_t *s, uint16_t si)
{
    if (!si) return;
    for (int i = 0; i < s->nrows; i++) {
        of_row_t *r = &s->rows[i];
        for (int k = 0; k < r->n; k++) {
            of_cell_t *c = &r->c[k];
            if (c->shared_si != si) continue;
            c->shared_si = 0;
            c->flags &= (uint8_t)~OC_SHARED;
            c->flags |= OC_EDITED;
            r->dirty = 1;
        }
    }
    s->dirty = true;
}

void of_cell_input(of_book_t *b, of_sheet_t *s, int32_t row, int32_t col, const char *text)
{
    of_cell_t *c = of_cell_make(s, row, col);
    if (!c) return;
    if (c->shared_si) {
        of_sheet_unshare(s, c->shared_si);
        c = of_cell_get(s, row, col);
    }
    of_cell_clear_value(c);
    c->flags |= OC_EDITED;
    s->dirty = true;
    of_row_t *r = of_row_get(s, row);
    if (r) r->dirty = 1;
    while (*text == ' ') text++;
    if (!*text) return;
    if (text[0] == '\'') {
        c->v.t = OV_STR;
        c->v.s = of_strdup(text + 1);
        return;
    }
    if (text[0] == '=' && text[1]) {
        c->f = of_strdup(text + 1);
        c->flags |= OC_FORMULA;
        bool unknown = false;
        int ix = (int)(s - b->sh);
        c->ast = of_formula_parse(c->f, ix, b, &unknown);
        if (c->ast) {
            int h = of_formula_result_hint(c->ast);
            if (h) set_format_if_general(b, c, h == OH_DATE ? (g_comma ? "dd/mm/yyyy" : "m/d/yyyy")
                                                            : (g_comma ? "dd/mm/yyyy hh:mm" : "m/d/yyyy h:mm"), h);
            c = of_cell_get(s, row, col);
            /* written back in the file's own words: English names, commas */
            of_sb_t sb = { 0 };
            of_formula_write(c->ast, b, &sb);
            if (sb.s && !sb.oom) {
                of_free(c->f);
                c->f = of_sb_take(&sb);
            }
            of_sb_free(&sb);
        } else {
            c->v.t = OV_ERR;
            c->v.err = OE_NAME;
        }
        return;
    }
    if (!strcasecmp(text, "TRUE") || !strcasecmp(text, "VERDADERO") || !strcasecmp(text, "WAHR")) {
        c->v.t = OV_BOOL;
        c->v.n = 1;
        return;
    }
    if (!strcasecmp(text, "FALSE") || !strcasecmp(text, "FALSO") || !strcasecmp(text, "FALSCH")) {
        c->v.t = OV_BOOL;
        c->v.n = 0;
        return;
    }
    double v;
    int pct, cur, dec;
    if (parse_number(text, &v, &pct, &cur, &dec)) {
        c->v.t = OV_NUM;
        c->v.n = v;
        if (pct) set_format_if_general(b, c, dec > 0 ? "0.00%" : "0%", OH_PCT);
        else if (cur) set_format_if_general(b, c, "\"$\"\\ #,##0.00", OH_CUR);
        return;
    }
    bool iso;
    if (parse_date(text, &v, &iso)) {
        c->v.t = OV_NUM;
        c->v.n = v;
        set_format_if_general(b, c, iso ? "yyyy\\-mm\\-dd" : (g_comma ? "dd/mm/yyyy" : "m/d/yyyy"), OH_DATE);
        return;
    }
    c->v.t = OV_STR;
    c->v.s = of_strdup(text);
}

const char *of_cell_code(const of_book_t *b, const of_cell_t *c)
{
    const char *code = "General";
    if (b && c->style < (uint32_t)b->nxf) code = of_numfmt_code(b, b->xf[c->style].numfmt);
    if (b && b->fmt == OF_FMT_ODS && b->sty_code && c->style < (uint32_t)b->nsty && b->sty_code[c->style])
        code = b->sty_code[c->style];
    if ((!code || !strcmp(code, "General")) && c->hint) code = of_hint_code(c->hint);
    return code ? code : "General";
}

static bool has_pct(const char *code)
{
    for (const char *p = code; *p; p++) {
        if (*p == '"') { p++; while (*p && *p != '"') p++; if (!*p) break; continue; }
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '%') return true;
    }
    return false;
}

void of_cell_source(const of_book_t *b, const of_cell_t *c, char *out, size_t len)
{
    out[0] = 0;
    if (!c) return;
    if (c->f) {
        snprintf(out, len, "=%s", c->f);
        return;
    }
    switch (c->v.t) {
    case OV_NUM: {
        /* as it would be typed: 15% and 01/12/2026, not 0.15 and 46357 */
        const char *code = of_cell_code(b, c);
        if (of_numfmt_is_date(code) && c->v.n >= 1) {
            double frac = c->v.n - (double)(long)c->v.n;
            const char *dc = frac > 1e-9 ? of_hint_code(OH_DATETIME) : of_hint_code(OH_DATE);
            bool right;
            of_numfmt_format(dc, &c->v, b && b->date1904, out, len, &right);
        } else if (has_pct(code)) {
            of_fmt_general(c->v.n * 100, out, len);
            size_t l = strlen(out);
            if (l + 1 < len) { out[l] = '%'; out[l + 1] = 0; }
        } else {
            of_fmt_general(c->v.n, out, len);
        }
        break;
    }
    case OV_STR:
        /* text that would read as something else keeps its apostrophe */
        if (c->v.s) {
            double d;
            int p, cu, de;
            bool iso;
            const char *t = c->v.s;
            bool looks = t[0] == '=' || t[0] == '\'' || parse_number(t, &d, &p, &cu, &de) || parse_date(t, &d, &iso) ||
                         !strcasecmp(t, "TRUE") || !strcasecmp(t, "FALSE") || !strcasecmp(t, "VERDADERO") ||
                         !strcasecmp(t, "FALSO");
            snprintf(out, len, "%s%s", looks ? "'" : "", t);
        }
        break;
    case OV_BOOL: snprintf(out, len, "%s", of_bool_word(c->v.n != 0)); break;
    case OV_ERR: snprintf(out, len, "%s", of_err_name(c->v.err)); break;
    default: break;
    }
}

static bool val_same(const of_val_t *a, const of_val_t *b)
{
    if (a->t != b->t) return false;
    switch (a->t) {
    case OV_NUM: case OV_BOOL: return a->n == b->n;
    case OV_STR: return strcmp(a->s ? a->s : "", b->s ? b->s : "") == 0;
    case OV_ERR: return a->err == b->err;
    default: return true;
    }
}

const char *of_hint_code(int hint)
{
    switch (hint) {
    case OH_DATE: return g_comma ? "dd/mm/yyyy" : "m/d/yyyy";
    case OH_TIME: return "hh:mm:ss";
    case OH_DATETIME: return g_comma ? "dd/mm/yyyy hh:mm" : "m/d/yyyy h:mm";
    case OH_PCT: return "0.00%";
    case OH_CUR: return "\"$\" #,##0.00";
    default: return "General";
    }
}

void of_cell_display(const of_book_t *b, const of_cell_t *c, char *out, size_t len, bool *right)
{
    out[0] = 0;
    *right = false;
    if (!c) return;
    /* ods: the file's own text, while it still tells the truth */
    if (c->disp && !(c->flags & (OC_EDITED | OC_STYLED)) && val_same(&c->v, &c->cached)) {
        snprintf(out, len, "%s", c->disp);
        *right = c->v.t == OV_NUM;
        return;
    }
    const of_val_t *v = &c->v;
    switch (v->t) {
    case OV_EMPTY: return;
    case OV_STR: snprintf(out, len, "%s", v->s ? v->s : ""); return;
    case OV_BOOL: snprintf(out, len, "%s", of_bool_word(v->n != 0)); *right = true; return;
    case OV_ERR: snprintf(out, len, "%s", of_err_name(v->err)); *right = true; return;
    default: break;
    }
    uint32_t nf = 0;
    if (b && c->style < (uint32_t)b->nxf) nf = b->xf[c->style].numfmt;
    const char *code = b ? of_numfmt_code(b, nf) : "General";
    if (b && b->fmt == OF_FMT_ODS && b->sty_code && c->style < (uint32_t)b->nsty && b->sty_code[c->style])
        code = b->sty_code[c->style];
    if ((!code || !strcmp(code, "General")) && c->hint) code = of_hint_code(c->hint);
    of_numfmt_format(code ? code : "General", v, b && b->date1904, out, len, right);
}

/* -------------------------------------------------------------------------- */
/* Sorting                                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    int32_t   row;              /* where it was */
    of_cell_t *cells;           /* the block's cells of that row */
    int       n;
    const of_val_t *key;
} srow_t;

static bool g_desc;

static int key_rank(const of_val_t *v)
{
    switch (v ? v->t : OV_EMPTY) {
    case OV_NUM: return 0;
    case OV_STR: return 1;
    case OV_BOOL: return 2;
    case OV_ERR: return 3;
    default: return 4;
    }
}

static int srow_cmp(const void *pa, const void *pb)
{
    const srow_t *a = pa, *b = pb;
    int ra = key_rank(a->key), rb = key_rank(b->key);
    int r;
    if (ra == 4 || rb == 4) {
        r = ra - rb;                    /* empties last either way */
        if (r) return r;
        return a->row - b->row;
    }
    if (ra != rb) r = ra - rb;
    else if (ra == 1) r = strcasecmp(a->key->s ? a->key->s : "", b->key->s ? b->key->s : "");
    else r = a->key->n < b->key->n ? -1 : a->key->n > b->key->n ? 1 : 0;
    if (g_desc) r = -r;
    return r ? r : a->row - b->row;    /* stable */
}

void of_sheet_sort(of_book_t *b, of_sheet_t *s, int32_t r1, int32_t r2, int32_t c1, int32_t c2,
                   int32_t key_col, bool desc)
{
    if (r2 < r1 || c2 < c1) return;
    int n = r2 - r1 + 1;
    srow_t *rows = of_alloc((size_t)n * sizeof *rows);
    if (!rows) return;
    int sheet_ix = (int)(s - b->sh);
    /* take the cells out */
    for (int i = 0; i < n; i++) {
        rows[i].row = r1 + i;
        of_row_t *r = of_row_get(s, r1 + i);
        if (!r) continue;
        int cnt = 0;
        for (int k = 0; k < r->n; k++)
            if (r->c[k].col >= c1 && r->c[k].col <= c2) cnt++;
        if (!cnt) continue;
        for (int k = 0; k < r->n; k++)
            if (r->c[k].col >= c1 && r->c[k].col <= c2 && r->c[k].shared_si) of_sheet_unshare(s, r->c[k].shared_si);
        rows[i].cells = of_alloc((size_t)cnt * sizeof(of_cell_t));
        if (!rows[i].cells) continue;
        int w = 0, keep = 0;
        for (int k = 0; k < r->n; k++) {
            if (r->c[k].col >= c1 && r->c[k].col <= c2) rows[i].cells[w++] = r->c[k];
            else r->c[keep++] = r->c[k];
        }
        rows[i].n = w;
        r->n = keep;
        r->dirty = 1;
    }
    for (int i = 0; i < n; i++) {
        rows[i].key = NULL;
        for (int k = 0; k < rows[i].n; k++)
            if (rows[i].cells[k].col == key_col) rows[i].key = &rows[i].cells[k].v;
    }
    g_desc = desc;
    qsort(rows, (size_t)n, sizeof *rows, srow_cmp);
    /* and back, each row in its new place */
    for (int i = 0; i < n; i++) {
        int32_t to = r1 + i;
        int dr = to - rows[i].row;
        for (int k = 0; k < rows[i].n; k++) {
            of_cell_t moved = rows[i].cells[k];
            if (moved.f && dr) {
                char *nf = of_formula_shift(moved.f, b, sheet_ix, dr, 0);
                if (nf) {
                    of_free(moved.f);
                    moved.f = nf;
                    of_formula_free(moved.ast);
                    bool unk = false;
                    moved.ast = of_formula_parse(moved.f, sheet_ix, b, &unk);
                }
            }
            moved.flags |= OC_EDITED;
            moved.flags &= (uint8_t)~OC_SHARED;
            moved.shared_si = 0;
            moved.raw_len = 0;
            of_cell_t *dst = of_cell_make(s, to, moved.col);
            if (dst) *dst = moved;
            of_row_t *r = of_row_get(s, to);
            if (r) r->dirty = 1;
        }
        of_free(rows[i].cells);
    }
    of_free(rows);
    s->dirty = true;
    of_book_recalc(b);
}

/* -------------------------------------------------------------------------- */
/* Loading and saving, by extension                                            */
/* -------------------------------------------------------------------------- */

int of_fmt_from_ext(const char *ext)
{
    if (!strcmp(ext, "csv") || !strcmp(ext, "txt")) return OF_FMT_CSV;
    if (!strcmp(ext, "tsv") || !strcmp(ext, "tab")) return OF_FMT_TSV;
    if (!strcmp(ext, "xlsx") || !strcmp(ext, "xlsm") || !strcmp(ext, "xltx") || !strcmp(ext, "xltm"))
        return OF_FMT_XLSX;
    if (!strcmp(ext, "ods") || !strcmp(ext, "ots")) return OF_FMT_ODS;
    if (!strcmp(ext, "xls") || !strcmp(ext, "xlt")) return OF_FMT_XLS;
    return OF_FMT_NONE;
}

bool of_is_sheet_ext(const char *ext)
{
    return !strcmp(ext, "csv") || !strcmp(ext, "tsv") || of_fmt_from_ext(ext) >= OF_FMT_XLSX;
}

of_book_t *of_book_load(const char *path, char *err, size_t errlen)
{
    int fmt = of_fmt_from_ext(of_ext(path));
    of_book_t *b = NULL;
    switch (fmt) {
    case OF_FMT_CSV: case OF_FMT_TSV: b = of_csv_load(path, err, errlen); break;
    case OF_FMT_XLSX: b = of_xlsx_load(path, err, errlen); break;
    case OF_FMT_ODS: b = of_ods_load(path, err, errlen); break;
    case OF_FMT_XLS: {
        extern of_book_t *of_xls_load(const char *path, char *err, size_t errlen);
        b = of_xls_load(path, err, errlen);
        break;
    }
    default: snprintf(err, errlen, "?"); return NULL;
    }
    if (b) of_book_recalc(b);
    return b;
}

bool of_book_save(of_book_t *b, int fmt, int sheet, const char *path, char *err, size_t errlen)
{
    switch (fmt) {
    case OF_FMT_CSV: case OF_FMT_TSV: {
        char keep = b->csv_sep;
        if (fmt == OF_FMT_TSV) b->csv_sep = '\t';
        else if (keep == '\t') b->csv_sep = g_comma ? ';' : ',';
        bool ok = of_csv_save(b, sheet, path, err, errlen);
        if (!ok) b->csv_sep = keep;
        return ok;
    }
    case OF_FMT_XLSX: return of_xlsx_save(b, path, err, errlen);
    case OF_FMT_ODS: return of_ods_save(b, path, err, errlen);
    default: return false;
    }
}

int of_book_save_losses(const of_book_t *b, int fmt)
{
    bool formulas = false;
    for (int i = 0; i < b->nsh && !formulas; i++)
        for (int r = 0; r < b->sh[i].nrows && !formulas; r++)
            for (int k = 0; k < b->sh[i].rows[r].n; k++)
                if (b->sh[i].rows[r].c[k].f) { formulas = true; break; }
    bool rich = b->fmt == OF_FMT_XLSX || b->fmt == OF_FMT_ODS;
    int lose = 0;
    if (fmt == OF_FMT_CSV || fmt == OF_FMT_TSV) {
        if (b->nsh > 1) lose |= OF_LOSE_SHEETS;
        if (formulas) lose |= OF_LOSE_FORMULAS;
        if (rich) lose |= OF_LOSE_FORMATS;
    } else if (fmt != b->fmt && rich) {
        lose |= OF_LOSE_FORMATS;
    }
    return lose;
}
