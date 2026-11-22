/*
 * P4OS - Office: CSV and TSV.
 *
 * Reading guesses what the file is like, and saving writes it the same way:
 *
 *   - the separator: the one of ',', ';', tab and '|' that splits the first
 *     lines into the same number of fields (Spanish and German programs
 *     write ';', because their decimal mark is the comma);
 *   - the encoding: UTF-8 (with or without its BOM), else Windows-1252;
 *   - the line ends: CRLF or LF;
 *   - quoting: RFC 4180, fields in double quotes with "" inside, line breaks
 *     inside quotes are part of the field.
 *
 * A field is read as the person would have typed it (of_cell_input): numbers
 * with the decimal mark the separator implies (';' files use the comma),
 * dates, TRUE/FALSE. A field that looks like a formula stays text: a CSV is
 * data, and "=cmd|..." in a downloaded file is the classic trap.
 */
#include "of.h"

#include <stdio.h>
#include <string.h>

#define CSV_MAX (24u * 1024 * 1024)

static int count_fields(const char *s, size_t n, char sep, int *lines_out)
{
    /* fields of the first line, and whether the next lines agree */
    int lines = 0, first = -1, agree = 0, fields = 1;
    bool q = false;
    for (size_t i = 0; i < n && lines < 20; i++) {
        char c = s[i];
        if (c == '"') q = !q;
        else if (!q && c == sep) fields++;
        else if (!q && c == '\n') {
            if (first < 0) first = fields;
            else if (fields == first) agree++;
            lines++;
            fields = 1;
        }
    }
    if (first < 0) first = fields;
    *lines_out = lines;
    return first > 1 ? first * 10 + agree * 100 : 0;
}

of_book_t *of_csv_load(const char *path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *raw = of_file_read(path, &len, CSV_MAX);
    if (!raw) {
        snprintf(err, errlen, "read");
        return NULL;
    }
    of_book_t *b = of_book_new(!strcmp(of_ext(path), "tsv") ? OF_FMT_TSV : OF_FMT_CSV);
    if (!b) { of_free(raw); return NULL; }
    char *s = (char *)raw;
    size_t n = len;
    if (n >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        b->csv_bom = true;
        s += 3;
        n -= 3;
    }
    char *conv = NULL;
    if (!of_utf8_valid(s, n)) {
        conv = of_latin1_to_utf8(s, n, &n);
        if (!conv) { of_free(raw); of_book_free(b); return NULL; }
        s = conv;
        b->csv_latin1 = true;
    }
    b->csv_crlf = memchr(s, '\r', n < 4096 ? n : 4096) != NULL;
    /* the separator */
    static const char SEPS[4] = { ',', ';', '\t', '|' };
    int best = -1, bscore = 0, lines;
    for (int i = 0; i < 4; i++) {
        int sc = count_fields(s, n, SEPS[i], &lines);
        if (sc > bscore) { bscore = sc; best = i; }
    }
    b->csv_sep = best >= 0 ? SEPS[best] : (b->fmt == OF_FMT_TSV ? '\t' : ',');
    if (b->fmt == OF_FMT_TSV) b->csv_sep = '\t';
    /* numbers in a ';' file use the decimal comma, in a ',' file the point */
    bool keep = of_locale_comma();
    if (b->csv_sep == ';') of_locale_set(true);
    else if (b->csv_sep == ',') of_locale_set(false);

    const char *base = of_basename(path);
    char name[64];
    const char *dot = strrchr(base, '.');
    int bl = dot ? (int)(dot - base) : (int)strlen(base);
    if (bl > 60) bl = 60;
    snprintf(name, sizeof name, "%.*s", bl, base);
    of_sheet_t *sh = of_book_add_sheet(b, name);
    of_sb_t f = { 0 };
    int32_t row = 0, col = 0;
    bool q = false;
    for (size_t i = 0; i <= n && sh; i++) {
        char c = i < n ? s[i] : '\n';
        if (q) {
            if (c == '"') {
                if (i + 1 < n && s[i + 1] == '"') { of_sb_putc(&f, '"'); i++; }
                else q = false;
            } else if (c != '\r' || !(i + 1 < n && s[i + 1] == '\n')) {
                of_sb_putc(&f, c);
            }
            continue;
        }
        if (c == '"' && f.n == 0) { q = true; continue; }
        if (c == '\r') continue;
        if (c == b->csv_sep || c == '\n') {
            if (f.n) {
                const char *t = f.s;
                if (t[0] == '=' || t[0] == '+' || t[0] == '@' || t[0] == '\'') {
                    of_cell_t *cell = of_cell_make(sh, row, col);
                    if (cell) {
                        cell->v.t = OV_STR;
                        cell->v.s = of_strdup(t);
                    }
                } else {
                    of_cell_input(b, sh, row, col, t);
                }
                of_cell_t *cell = of_cell_get(sh, row, col);
                if (cell) {
                    cell->flags &= (uint8_t)~OC_EDITED;
                    of_val_copy(&cell->cached, &cell->v);
                    /* as the file wrote it, for showing and saving it again */
                    if (cell->v.t != OV_STR) cell->disp = of_strdup(t);
                    if (cell->v.t == OV_BOOL && !b->csv_true[0]) {
                        bool es = !strcasecmp(t, "VERDADERO") || !strcasecmp(t, "FALSO");
                        bool de = !strcasecmp(t, "WAHR") || !strcasecmp(t, "FALSCH");
                        snprintf(b->csv_true, sizeof b->csv_true, "%s", es ? "VERDADERO" : de ? "WAHR" : "TRUE");
                        snprintf(b->csv_false, sizeof b->csv_false, "%s", es ? "FALSO" : de ? "FALSCH" : "FALSE");
                    }
                }
            }
            f.n = 0;
            if (f.s) f.s[0] = 0;
            if (c == '\n') {
                if (i >= n) break;
                row++;
                col = 0;
            } else {
                col++;
            }
            continue;
        }
        of_sb_putc(&f, c);
    }
    of_sb_free(&f);
    of_locale_set(keep);
    b->csv_quote_all = false;
    sh->dirty = false;
    for (int r = 0; r < sh->nrows; r++) sh->rows[r].dirty = 0;
    of_free(conv);
    of_free(raw);
    return b;
}

static void put_field(of_sb_t *o, const char *s, char sep)
{
    bool q = false;
    for (const char *p = s; *p; p++)
        if (*p == sep || *p == '"' || *p == '\n' || *p == '\r') q = true;
    if (s[0] == ' ' || (*s && s[strlen(s) - 1] == ' ')) q = true;
    if (!q) {
        of_sb_puts(o, s);
        return;
    }
    of_sb_putc(o, '"');
    for (const char *p = s; *p; p++) {
        if (*p == '"') of_sb_putc(o, '"');
        of_sb_putc(o, *p);
    }
    of_sb_putc(o, '"');
}

bool of_csv_save(of_book_t *b, int sheet, const char *path, char *err, size_t errlen)
{
    if (sheet < 0 || sheet >= b->nsh) sheet = 0;
    of_sheet_t *s = &b->sh[sheet];
    char sep = b->csv_sep ? b->csv_sep : ',';
    bool keep = of_locale_comma();
    if (sep == ';') of_locale_set(true);
    else if (sep == ',') of_locale_set(false);
    of_sb_t o = { 0 };
    int32_t maxr, maxc;
    of_sheet_extent(s, &maxr, &maxc);
    const char *eol = b->csv_crlf ? "\r\n" : "\n";
    int32_t prev = -1;
    for (int i = 0; i < s->nrows; i++) {
        of_row_t *r = &s->rows[i];
        if (r->row > maxr) break;
        for (int32_t k = prev + 1; k < r->row; k++) of_sb_puts(&o, eol);
        int32_t col = 0;
        for (int k = 0; k < r->n; k++) {
            of_cell_t *c = &r->c[k];
            if (c->v.t == OV_EMPTY && !c->f) continue;
            while (col < c->col) { of_sb_putc(&o, sep); col++; }
            char buf[512];
            if (c->disp && !(c->flags & (OC_EDITED | OC_STYLED)) && (b->fmt == OF_FMT_CSV || b->fmt == OF_FMT_TSV)) {
                snprintf(buf, sizeof buf, "%s", c->disp);
            } else if (c->v.t == OV_BOOL) {
                snprintf(buf, sizeof buf, "%s", c->v.n ? (b->csv_true[0] ? b->csv_true : "TRUE")
                                                       : (b->csv_false[0] ? b->csv_false : "FALSE"));
            } else if (c->v.t == OV_NUM && (b->fmt == OF_FMT_CSV || b->fmt == OF_FMT_TSV) && !c->hint &&
                !(c->style < (uint32_t)b->nxf && b->xf[c->style].numfmt)) {
                of_fmt_general(c->v.n, buf, sizeof buf);
            } else if (c->v.t == OV_STR) {
                put_field(&o, c->v.s ? c->v.s : "", sep);
                continue;
            } else {
                bool right;
                of_cell_display(b, c, buf, sizeof buf, &right);
            }
            put_field(&o, buf, sep);
        }
        of_sb_puts(&o, eol);
        prev = r->row;
    }
    of_locale_set(keep);
    if (o.oom) { of_sb_free(&o); snprintf(err, errlen, "memory"); return false; }
    const char *data = o.s ? o.s : "";
    size_t dl = o.n;
    char *conv = NULL;
    if (b->csv_latin1) {
        int lost = 0;
        conv = of_utf8_to_latin1(data, dl, &dl, &lost);
        if (!conv) { of_sb_free(&o); return false; }
        data = conv;
    }
    bool ok;
    if (b->csv_bom && !b->csv_latin1) {
        of_sb_t w = { 0 };
        of_sb_put(&w, "\xEF\xBB\xBF", 3);
        of_sb_put(&w, data, dl);
        ok = !w.oom && of_file_write(path, w.s, w.n);
        of_sb_free(&w);
    } else {
        ok = of_file_write(path, data, dl);
    }
    of_free(conv);
    of_sb_free(&o);
    if (!ok) snprintf(err, errlen, "write");
    return ok;
}
