/*
 * P4OS - Office: the text editor.
 *
 * Drawn by the app over of_text_t, so a log of megabytes scrolls as lightly
 * as a note: only the lines in sight are laid out. Line numbers, wrapping
 * (on for prose, off for code and logs, and always off past 2 MB), the
 * Inter or the mono font, two fingers to zoom. Source is coloured by a small
 * lexer per family of languages (C-like, Python and shell, JSON, XML and
 * HTML, INI and TOML, YAML, Markdown, logs); a line's colours do not depend
 * on the lines above it except for C's block comments, which are tracked
 * from the top of what is drawn.
 *
 * Editing: tap to place the caret, long press to select a word and drag to
 * extend, the keyboard of the app or a USB or Bluetooth one (arrows with
 * Shift, Home, End, Ctrl+A/C/X/V/Z/Y/S/F), undo and redo, find and replace.
 * The file keeps its encoding and line ends; the menu changes them.
 */
#include "of_ui.h"
#include "of_pdf.h"
#include "of_doc.h"
#include "aos_fonts.h"
#include "aos_mono.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NZ 6
static const lv_font_t *const TZ[NZ] = { &aos_inter_16, &aos_inter_20, &aos_inter_24, &aos_inter_28, &aos_inter_32, &aos_inter_36 };
static const lv_font_t *const MZ[2] = { &aos_mono_18, &aos_mono_22 };

#define WRAP_MAX (2u * 1024 * 1024)
#define UNDO_MAX 400

enum { LX_NONE = 0, LX_C, LX_HASH, LX_JSON, LX_XML, LX_INI, LX_YAML, LX_MD, LX_LOG };
enum { TK_TEXT = 0, TK_KEYWORD, TK_STRING, TK_NUMBER, TK_COMMENT, TK_TAG, TK_ATTR, TK_HEAD, TK_PUNCT, TK_ERR, TK_WARN };

typedef struct {
    size_t pos;
    char  *del, *ins;           /* what went out and what came in at pos */
    size_t sel_a, sel_b;        /* the caret before */
} op_t;

static struct {
    of_text_t  t;
    char       path[300];
    lv_obj_t  *root, *top, *title, *area, *kb, *find, *f_ta, *r_ta, *b_undo, *b_redo, *b_kb;
    lv_obj_t  *focus_ta;        /* the find bar's field being typed into, or NULL */
    int32_t    W, H, ax, ay, aw, ah;
    int        z;
    bool       mono, wrap, numbers;
    int        lex;
    /* layout: first visual row of each line (wrap), or the line itself */
    int32_t   *vrow;
    int        nvrow;
    int32_t    lh, gutter, textw;
    int32_t    sx, sy;
    size_t     caret, anchor;
    int32_t    want_x;          /* the column kept while going up and down */
    bool       selecting;
    float      pinch;
    float      vy;
    lv_timer_t *fling;
    /* undo */
    op_t       undo[UNDO_MAX], redo[UNDO_MAX];
    int        nundo, nredo;
    uint32_t   last_type_ms;
    bool       modified;
    bool       replace_open;
} T;

static void relayout(void);
static void refresh(void);

static const lv_font_t *font(void)
{
    if (T.mono) return MZ[T.z >= 2 ? 1 : 0];
    return TZ[T.z];
}

static int32_t glyph_w(const lv_font_t *f, uint32_t cp, uint32_t next)
{
    if (cp == '\t') return lv_font_get_glyph_width(f, ' ', 0) * 4;
    return lv_font_get_glyph_width(f, cp, next);
}

static int32_t span_w(const char *s, size_t n, const lv_font_t *f)
{
    int32_t w = 0;
    size_t i = 0;
    while (i < n) {
        int l;
        uint32_t cp = of_utf8_get(s + i, &l);
        uint32_t next = i + (size_t)l < n ? of_utf8_get(s + i + l, NULL) : 0;
        w += glyph_w(f, cp, next);
        i += (size_t)l;
    }
    return w;
}

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */
/* -------------------------------------------------------------------------- */

/* Where line i breaks into rows: up to max offsets (from the line's start),
 * the count returned. Breaks after spaces when there is one. */
static int line_breaks(int line, char *buf, int buflen, int *brk, int max)
{
    int n = of_text_line_copy(&T.t, line, buf, buflen);
    if (!T.wrap) {
        brk[0] = 0;
        return 1;
    }
    const lv_font_t *f = font();
    int rows = 0, start = 0, last_space = -1;
    int32_t w = 0;
    brk[rows++] = 0;
    for (int i = 0; i < n;) {
        int l;
        uint32_t cp = of_utf8_get(buf + i, &l);
        int32_t gw = glyph_w(f, cp, 0);
        if (w + gw > T.textw && i > start) {
            int at = last_space > start ? last_space + 1 : i;
            if (rows < max) brk[rows++] = at;
            start = at;
            w = span_w(buf + start, (size_t)(i - start), f);
            last_space = -1;
            continue;
        }
        if (cp == ' ') last_space = i;
        w += gw;
        i += l;
    }
    return rows;
}

static char *s_lbuf;
#define LBUF 8192

static void build_rows(void)
{
    of_free(T.vrow);
    T.nvrow = T.t.nlines;
    T.vrow = of_alloc((size_t)(T.t.nlines + 1) * sizeof(int32_t));
    if (!T.vrow) return;
    int brk[256];
    int32_t v = 0;
    for (int i = 0; i < T.t.nlines; i++) {
        T.vrow[i] = v;
        v += T.wrap ? line_breaks(i, s_lbuf, LBUF, brk, 256) : 1;
    }
    T.vrow[T.t.nlines] = v;
}

static int32_t total_rows(void) { return T.vrow ? T.vrow[T.t.nlines] : T.t.nlines; }

static int line_of_row(int32_t row)
{
    int lo = 0, hi = T.t.nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (T.vrow[mid] <= row) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

/* the rows of line i changed count: rebuild the index from there */
static void rows_from(int line)
{
    if (!T.vrow || T.nvrow != T.t.nlines) { build_rows(); return; }
    int brk[256];
    int32_t v = T.vrow[line];
    for (int i = line; i < T.t.nlines; i++) {
        int32_t old = T.vrow[i + 1] - T.vrow[i];
        int32_t nw = T.wrap ? line_breaks(i, s_lbuf, LBUF, brk, 256) : 1;
        T.vrow[i] = v;
        v += nw;
        if (nw == old && i > line + 1) {
            /* the rest only shifts */
            int32_t d = v - T.vrow[i + 1];
            if (d) for (int k = i + 1; k <= T.t.nlines; k++) T.vrow[k] += d;
            return;
        }
    }
    T.vrow[T.t.nlines] = v;
}

static void metrics(void)
{
    const lv_font_t *f = font();
    T.lh = f->line_height + f->line_height / 5;
    int digits = 1;
    for (int n = T.t.nlines; n >= 10; n /= 10) digits++;
    if (digits < 2) digits = 2;
    T.gutter = T.numbers ? lv_font_get_glyph_width(&aos_mono_18, '0', 0) * (digits + 1) + 12 : 12;
    T.textw = T.aw - T.gutter - 16;
    if (of_text_len(&T.t) > WRAP_MAX) T.wrap = false;
    build_rows();
}

static void clamp(void)
{
    int32_t maxy = total_rows() * T.lh - T.ah + T.lh * 2;
    if (maxy < 0) maxy = 0;
    if (T.sy > maxy) T.sy = maxy;
    if (T.sy < 0) T.sy = 0;
    if (T.wrap || T.sx < 0) T.sx = 0;
}

/* caret -> visual row and x (in the text, before scrolling) */
static void caret_xy(size_t pos, int32_t *row, int32_t *x)
{
    int line = of_text_line_of(&T.t, pos);
    size_t a, b;
    of_text_line_span(&T.t, line, &a, &b);
    int brk[256];
    int n = line_breaks(line, s_lbuf, LBUF, brk, 256);
    int off = (int)(pos - a);
    int r = 0;
    while (r + 1 < n && brk[r + 1] <= off) r++;
    *row = T.vrow[line] + r;
    *x = span_w(s_lbuf + brk[r], (size_t)(off - brk[r]), font());
}

/* a point in the area -> position */
static size_t pos_at(int32_t px, int32_t py)
{
    int32_t row = (py - T.ay + T.sy) / T.lh;
    if (row < 0) row = 0;
    if (row >= total_rows()) return of_text_len(&T.t);
    int line = line_of_row(row);
    size_t a, b;
    of_text_line_span(&T.t, line, &a, &b);
    int brk[256];
    int n = line_breaks(line, s_lbuf, LBUF, brk, 256);
    int r = row - T.vrow[line];
    if (r >= n) r = n - 1;
    int from = brk[r], to = r + 1 < n ? brk[r + 1] : (int)(b - a);
    int32_t x = px - T.ax - T.gutter + T.sx, w = 0;
    const lv_font_t *f = font();
    int i = from;
    while (i < to) {
        int l;
        uint32_t cp = of_utf8_get(s_lbuf + i, &l);
        int32_t gw = glyph_w(f, cp, 0);
        if (w + gw / 2 > x) break;
        w += gw;
        i += l;
    }
    /* a wrapped row ends before its last space: the caret goes before it */
    if (r + 1 < n && i == to && i > from) i--;
    return a + (size_t)i;
}

static void scroll_to_caret(void)
{
    int32_t row, x;
    caret_xy(T.caret, &row, &x);
    int32_t y = row * T.lh;
    if (y < T.sy) T.sy = y;
    else if (y + T.lh > T.sy + T.ah) T.sy = y + T.lh - T.ah;
    if (!T.wrap) {
        if (x < T.sx) T.sx = x > 40 ? x - 40 : 0;
        else if (x > T.sx + T.textw - 20) T.sx = x - T.textw + 80;
    }
    clamp();
}

/* -------------------------------------------------------------------------- */
/* Colours                                                                     */
/* -------------------------------------------------------------------------- */

static int lexer_of(const char *path)
{
    const char *e = of_ext(path);
    static const char *const C[] = { "c", "h", "cpp", "hpp", "cc", "js", "mjs", "ts", "java", "rs", "go", "css", "cs", "swift", "kt" };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++)
        if (!strcmp(e, C[i])) return LX_C;
    if (!strcmp(e, "py") || !strcmp(e, "sh") || !strcmp(e, "rb") || !strcmp(e, "pl") || !strcmp(e, "cmake") ||
        !strcmp(e, "mk") || !strcmp(e, "properties") || !strcmp(e, "env") || !strcmp(e, "gitignore") || !strcmp(e, "lua"))
        return LX_HASH;
    if (!strcmp(e, "json")) return LX_JSON;
    if (!strcmp(e, "xml") || !strcmp(e, "html") || !strcmp(e, "htm") || !strcmp(e, "svg") || !strcmp(e, "kml") || !strcmp(e, "gpx"))
        return LX_XML;
    if (!strcmp(e, "ini") || !strcmp(e, "toml") || !strcmp(e, "conf") || !strcmp(e, "cfg")) return LX_INI;
    if (!strcmp(e, "yaml") || !strcmp(e, "yml")) return LX_YAML;
    if (!strcmp(e, "md") || !strcmp(e, "markdown")) return LX_MD;
    if (!strcmp(e, "log")) return LX_LOG;
    return LX_NONE;
}

static lv_color_t tk_color(int tk)
{
    bool d = OFT.dark;
    switch (tk) {
    case TK_KEYWORD: return lv_color_hex(d ? 0xFF7AB2 : 0xAD3DA4);
    case TK_STRING:  return lv_color_hex(d ? 0xFC6A5D : 0xC41A16);
    case TK_NUMBER:  return lv_color_hex(d ? 0xD0BF69 : 0x1C00CF);
    case TK_COMMENT: return lv_color_hex(d ? 0x7F8C98 : 0x5D6C79);
    case TK_TAG:     return lv_color_hex(d ? 0x5DD8FF : 0x0B4F79);
    case TK_ATTR:    return lv_color_hex(d ? 0xA167E6 : 0x6C36A9);
    case TK_HEAD:    return lv_color_hex(d ? 0x67B7A4 : 0x326D74);
    case TK_PUNCT:   return OFT.dim;
    case TK_ERR:     return lv_color_hex(d ? 0xFF6B60 : 0xC0251A);
    case TK_WARN:    return lv_color_hex(d ? 0xFFB340 : 0xA65A00);
    default:         return OFT.ink;
    }
}

static bool is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool keyword(const char *s, int n)
{
    static const char *const KW[] = {
        "if", "else", "for", "while", "do", "return", "break", "continue", "switch", "case", "default", "static",
        "const", "struct", "enum", "typedef", "void", "int", "char", "bool", "float", "double", "long", "unsigned",
        "sizeof", "true", "false", "NULL", "null", "nil", "def", "class", "import", "from", "as", "in", "not",
        "and", "or", "is", "None", "True", "False", "lambda", "with", "try", "except", "finally", "raise", "yield",
        "function", "var", "let", "new", "this", "self", "fn", "pub", "use", "mod", "match", "impl", "local",
        "then", "end", "elif", "fi", "esac", "export", "echo", "package", "func", "go", "defer", "async", "await",
        "uint8_t", "uint16_t", "uint32_t", "int32_t", "int64_t", "size_t", "include", "define", "pragma", "extern" };
    for (size_t i = 0; i < sizeof KW / sizeof KW[0]; i++)
        if ((int)strlen(KW[i]) == n && !strncmp(KW[i], s, (size_t)n)) return true;
    return false;
}

/* The token kind of each byte of a line. *in_block: inside a C comment. */
static void colour_line(const char *s, int n, uint8_t *tk, bool *in_block)
{
    memset(tk, TK_TEXT, (size_t)n);
    int i = 0;
    switch (T.lex) {
    case LX_LOG: {
        int kind = TK_TEXT;
        for (int k = 0; k + 4 <= n && k < 64; k++) {
            if (!strncmp(s + k, "ERROR", 5) || !strncmp(s + k, "FATAL", 5) || !strncmp(s + k, "error", 5)) { kind = TK_ERR; break; }
            if (!strncmp(s + k, "WARN", 4) || !strncmp(s + k, "warn", 4)) { kind = TK_WARN; break; }
            if (!strncmp(s + k, "DEBUG", 5) || !strncmp(s + k, "TRACE", 5)) { kind = TK_COMMENT; break; }
        }
        /* the timestamp at the start in grey */
        int ts = 0;
        while (ts < n && ((s[ts] >= '0' && s[ts] <= '9') || strchr("-:.T /[]", s[ts]))) ts++;
        for (int k = 0; k < n; k++) tk[k] = (uint8_t)(k < ts ? TK_COMMENT : kind);
        return;
    }
    case LX_MD: {
        int k = 0;
        while (k < n && s[k] == ' ') k++;
        if (k < n && s[k] == '#') { memset(tk, TK_HEAD, (size_t)n); return; }
        if (k < n && s[k] == '>') { memset(tk, TK_COMMENT, (size_t)n); return; }
        if (k + 1 < n && (s[k] == '-' || s[k] == '*' || s[k] == '+') && s[k + 1] == ' ') tk[k] = TK_KEYWORD;
        if (k + 2 < n && !strncmp(s + k, "```", 3)) { memset(tk, TK_STRING, (size_t)n); return; }
        for (i = 0; i < n; i++) {
            if (s[i] == '`') {
                int j = i + 1;
                while (j < n && s[j] != '`') j++;
                for (int q = i; q <= j && q < n; q++) tk[q] = TK_STRING;
                i = j;
            } else if (s[i] == '*' || s[i] == '_') {
                tk[i] = TK_PUNCT;
            } else if (s[i] == '[') {
                int j = i;
                while (j < n && s[j] != ')') j++;
                for (int q = i; q <= j && q < n; q++) tk[q] = TK_TAG;
                i = j;
            }
        }
        return;
    }
    case LX_INI: {
        int k = 0;
        while (k < n && s[k] == ' ') k++;
        if (k < n && (s[k] == ';' || s[k] == '#')) { memset(tk, TK_COMMENT, (size_t)n); return; }
        if (k < n && s[k] == '[') { memset(tk, TK_HEAD, (size_t)n); return; }
        const char *eq = memchr(s, '=', (size_t)n);
        if (!eq) eq = memchr(s, ':', (size_t)n);
        if (eq) {
            int e = (int)(eq - s);
            for (int q = 0; q < e; q++) tk[q] = TK_ATTR;
            tk[e] = TK_PUNCT;
            for (int q = e + 1; q < n; q++) {
                if (s[q] == '#' || s[q] == ';') { for (; q < n; q++) tk[q] = TK_COMMENT; break; }
                tk[q] = s[q] == '"' || s[q] == '\'' ? TK_STRING : ((s[q] >= '0' && s[q] <= '9') ? TK_NUMBER : TK_STRING);
            }
        }
        return;
    }
    case LX_YAML: {
        int k = 0;
        while (k < n && s[k] == ' ') k++;
        if (k < n && s[k] == '#') { memset(tk, TK_COMMENT, (size_t)n); return; }
        if (k < n && s[k] == '-') tk[k++] = TK_PUNCT;
        const char *colon = memchr(s + k, ':', (size_t)(n - k));
        int e = colon ? (int)(colon - s) : -1;
        for (int q = k; q < e; q++) tk[q] = TK_ATTR;
        for (int q = e < 0 ? k : e + 1; q < n; q++) {
            if (s[q] == '#' && (q == 0 || s[q - 1] == ' ')) { for (; q < n; q++) tk[q] = TK_COMMENT; break; }
            tk[q] = (s[q] >= '0' && s[q] <= '9') ? TK_NUMBER : TK_STRING;
        }
        return;
    }
    case LX_XML: {
        bool in_tag = false;
        for (i = 0; i < n; i++) {
            if (!in_tag && s[i] == '<') {
                if (i + 3 < n && !strncmp(s + i, "<!--", 4)) {
                    const char *e = NULL;
                    for (int q = i; q + 2 < n; q++) if (!strncmp(s + q, "-->", 3)) { e = s + q + 3; break; }
                    int end = e ? (int)(e - s) : n;
                    for (int q = i; q < end; q++) tk[q] = TK_COMMENT;
                    i = end - 1;
                    continue;
                }
                in_tag = true;
                tk[i] = TK_PUNCT;
                int j = i + 1;
                while (j < n && (is_word(s[j]) || s[j] == ':' || s[j] == '/' || s[j] == '-' || s[j] == '?' || s[j] == '!')) tk[j++] = TK_TAG;
                i = j - 1;
            } else if (in_tag) {
                if (s[i] == '>') { tk[i] = TK_PUNCT; in_tag = false; }
                else if (s[i] == '"' || s[i] == '\'') {
                    char q = s[i];
                    int j = i + 1;
                    while (j < n && s[j] != q) j++;
                    for (int k2 = i; k2 <= j && k2 < n; k2++) tk[k2] = TK_STRING;
                    i = j;
                } else if (is_word(s[i])) tk[i] = TK_ATTR;
            } else if (s[i] == '&') {
                int j = i;
                while (j < n && s[j] != ';' && j - i < 10) j++;
                for (int q = i; q <= j && q < n; q++) tk[q] = TK_KEYWORD;
                i = j;
            }
        }
        return;
    }
    default:
        break;
    }
    if (T.lex == LX_NONE) return;
    /* C-like, hash-comment and JSON */
    while (i < n) {
        char c = s[i];
        if (*in_block) {
            tk[i] = TK_COMMENT;
            if (c == '*' && i + 1 < n && s[i + 1] == '/') { tk[i + 1] = TK_COMMENT; i += 2; *in_block = false; continue; }
            i++;
            continue;
        }
        if (T.lex == LX_C && c == '/' && i + 1 < n && s[i + 1] == '/') { for (; i < n; i++) tk[i] = TK_COMMENT; break; }
        if (T.lex == LX_C && c == '/' && i + 1 < n && s[i + 1] == '*') { tk[i] = tk[i + 1] = TK_COMMENT; i += 2; *in_block = true; continue; }
        if (T.lex == LX_HASH && c == '#') { for (; i < n; i++) tk[i] = TK_COMMENT; break; }
        if (c == '"' || c == '\'' || (c == '`' && T.lex == LX_C)) {
            int j = i + 1;
            while (j < n && s[j] != c) j += s[j] == '\\' ? 2 : 1;
            int end = j < n ? j : n - 1;
            /* JSON: a string followed by ':' is a key */
            int kind = TK_STRING;
            if (T.lex == LX_JSON) {
                int q = j + 1;
                while (q < n && s[q] == ' ') q++;
                if (q < n && s[q] == ':') kind = TK_ATTR;
            }
            for (int q = i; q <= end; q++) tk[q] = (uint8_t)kind;
            i = end + 1;
            continue;
        }
        if ((c >= '0' && c <= '9') || (c == '-' && T.lex == LX_JSON && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9')) {
            if (i > 0 && is_word(s[i - 1])) { i++; continue; }
            int j = i + 1;
            while (j < n && (is_word(s[j]) || s[j] == '.' || ((s[j] == '-' || s[j] == '+') && (s[j - 1] == 'e' || s[j - 1] == 'E')))) j++;
            for (int q = i; q < j; q++) tk[q] = TK_NUMBER;
            i = j;
            continue;
        }
        if (is_word(c)) {
            int j = i;
            while (j < n && is_word(s[j])) j++;
            if (keyword(s + i, j - i)) for (int q = i; q < j; q++) tk[q] = TK_KEYWORD;
            i = j;
            continue;
        }
        if (strchr("{}[](),;:", c)) tk[i] = TK_PUNCT;
        else if (c == '#' && T.lex == LX_C) { for (int q = i; q < n && (is_word(s[q]) || q == i); q++) tk[q] = TK_KEYWORD; }
        i++;
    }
}

/* -------------------------------------------------------------------------- */
/* Drawing                                                                     */
/* -------------------------------------------------------------------------- */

static void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, lv_opa_t opa)
{
    if (x2 < x1 || y2 < y1) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = opa;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

static void text_at(lv_layer_t *layer, const char *s, size_t n, int32_t x, int32_t y, lv_color_t c)
{
    if (!n) return;
    char buf[1024];
    if (n >= sizeof buf) n = sizeof buf - 1;
    /* tabs as four spaces, so what is drawn matches what was measured */
    size_t k = 0;
    for (size_t i = 0; i < n && k < sizeof buf - 5; i++) {
        if (s[i] == '\t') { memcpy(buf + k, "    ", 4); k += 4; }
        else buf[k++] = s[i];
    }
    buf[k] = 0;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = font();
    d.color = c;
    d.text = buf;
    d.text_local = 1;
    lv_area_t a = { x, y, x + 8000, y + font()->line_height };
    lv_draw_label(layer, &d, &a);
}

static void sync_origin(void)
{
    lv_area_t a;
    lv_obj_get_coords(T.area, &a);
    T.ax = a.x1;
    T.ay = a.y1;
}

static void area_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    sync_origin();
    lv_area_t keep = layer->_clip_area, cl;
    lv_area_t box = { T.ax, T.ay, T.ax + T.aw - 1, T.ay + T.ah - 1 };
    if (!ofu_clip(&cl, &keep, &box)) return;
    layer->_clip_area = cl;
    fill(layer, cl.x1, cl.y1, cl.x2, cl.y2, OFT.paper, LV_OPA_COVER);
    if (T.numbers) fill(layer, T.ax, cl.y1, T.ax + T.gutter - 6, cl.y2, OFT.head, LV_OPA_COVER);
    const lv_font_t *f = font();
    int32_t row0 = T.sy / T.lh, rows = T.ah / T.lh + 2;
    if (row0 >= total_rows()) { layer->_clip_area = keep; return; }
    int line = line_of_row(row0);
    size_t sa = T.caret < T.anchor ? T.caret : T.anchor, sb = T.caret < T.anchor ? T.anchor : T.caret;
    /* C block comments: look back a few hundred lines for an open one */
    bool in_block = false;
    if (T.lex == LX_C) {
        int from = line > 300 ? line - 300 : 0;
        static uint8_t tmp[LBUF];
        for (int l = from; l < line; l++) {
            int n = of_text_line_copy(&T.t, l, s_lbuf, LBUF);
            colour_line(s_lbuf, n, tmp, &in_block);
        }
    }
    static uint8_t tk[LBUF];
    int brk[256];
    int32_t drawn = 0;
    int32_t cx = -1, cy = -1;
    for (int l = line; l < T.t.nlines && drawn < rows + (row0 - T.vrow[line]); l++) {
        size_t a, b;
        of_text_line_span(&T.t, l, &a, &b);
        int nb = line_breaks(l, s_lbuf, LBUF, brk, 256);
        int n = (int)(b - a) < LBUF - 1 ? (int)(b - a) : LBUF - 1;
        colour_line(s_lbuf, n, tk, &in_block);
        for (int r = 0; r < nb; r++) {
            int32_t vr = T.vrow[l] + r;
            if (vr < row0) continue;
            if (vr > row0 + rows) break;
            int32_t y = T.ay + vr * T.lh - T.sy;
            int from = brk[r], to = r + 1 < nb ? brk[r + 1] : n;
            int32_t x0 = T.ax + T.gutter - T.sx;
            /* the selection behind */
            size_t ra = a + (size_t)from, rb = a + (size_t)to;
            if (sb > sa && sb > ra && sa <= rb) {
                size_t p0 = sa > ra ? sa : ra, p1 = sb < rb ? sb : rb;
                int32_t x1 = x0 + span_w(s_lbuf + from, p0 - ra, f);
                int32_t x2 = x0 + span_w(s_lbuf + from, p1 - ra, f);
                if (sb > rb && r + 1 == nb) x2 += f->line_height / 3;     /* the line end is selected */
                fill(layer, x1, y, x2, y + T.lh - 1, OFT.sel, LV_OPA_40);
            }
            /* runs of one colour */
            int32_t x = x0;
            int i = from;
            lv_area_t tclip = cl;
            tclip.x1 = LV_MAX(cl.x1, T.ax + T.gutter - 4);
            layer->_clip_area = tclip;
            while (i < to) {
                int j = i;
                while (j < to && tk[j] == tk[i]) j++;
                int32_t w = span_w(s_lbuf + i, (size_t)(j - i), f);
                if (x + w > tclip.x1 && x < tclip.x2) text_at(layer, s_lbuf + i, (size_t)(j - i), x, y + (T.lh - f->line_height) / 2, tk_color(tk[i]));
                x += w;
                i = j;
                if (x > tclip.x2) break;
            }
            layer->_clip_area = cl;
            if (r == 0 && T.numbers) {
                char num[12];
                snprintf(num, sizeof num, "%d", l + 1);
                lv_draw_label_dsc_t d;
                lv_draw_label_dsc_init(&d);
                d.font = &aos_mono_18;
                d.color = OFT.dim;
                d.text = num;
                d.text_local = 1;
                d.align = LV_TEXT_ALIGN_RIGHT;
                lv_area_t na = { T.ax, y + (T.lh - aos_mono_18.line_height) / 2, T.ax + T.gutter - 14, y + T.lh };
                lv_draw_label(layer, &d, &na);
            }
            /* the caret */
            if (T.caret >= ra && (T.caret < rb || (T.caret == rb && r + 1 == nb))) {
                cx = x0 + span_w(s_lbuf + from, T.caret - ra, f);
                cy = y;
            }
            drawn++;
        }
    }
    if (cx >= 0 && sa == sb) fill(layer, cx, cy + 2, cx + 2, cy + T.lh - 3, OFT.sel, LV_OPA_COVER);
    layer->_clip_area = keep;
}

/* -------------------------------------------------------------------------- */
/* Editing                                                                     */
/* -------------------------------------------------------------------------- */

static void op_free(op_t *o)
{
    of_free(o->del);
    of_free(o->ins);
    memset(o, 0, sizeof *o);
}

static void push_undo(size_t pos, const char *del, size_t dl, const char *ins, size_t il, bool typing)
{
    uint32_t now = lv_tick_get();
    /* letters typed one after the other are one step */
    if (typing && T.nundo && !dl && il && now - T.last_type_ms < 1500) {
        op_t *o = &T.undo[T.nundo - 1];
        if (o->ins && !o->del && o->pos + strlen(o->ins) == pos && il == 1 && ins[0] != '\n' && ins[0] != ' ') {
            size_t ol = strlen(o->ins);
            char *n = of_realloc(o->ins, ol + il + 1);
            if (n) { memcpy(n + ol, ins, il); n[ol + il] = 0; o->ins = n; T.last_type_ms = now; return; }
        }
    }
    T.last_type_ms = now;
    if (T.nundo == UNDO_MAX) {
        op_free(&T.undo[0]);
        memmove(&T.undo[0], &T.undo[1], (UNDO_MAX - 1) * sizeof(op_t));
        T.nundo--;
    }
    op_t *o = &T.undo[T.nundo++];
    o->pos = pos;
    o->del = dl ? of_strndup(del, dl) : NULL;
    o->ins = il ? of_strndup(ins, il) : NULL;
    o->sel_a = T.anchor;
    o->sel_b = T.caret;
    for (int i = 0; i < T.nredo; i++) op_free(&T.redo[i]);
    T.nredo = 0;
}

/* Replaces [a, b) with s */
static void replace(size_t a, size_t b, const char *s, size_t n, bool typing)
{
    char *del = b > a ? of_text_range(&T.t, a, b) : NULL;
    push_undo(a, del, b - a, s, n, typing);
    int line = of_text_line_of(&T.t, a);
    int lines0 = T.t.nlines;
    if (b > a) of_text_delete(&T.t, a, b - a);
    if (n) of_text_insert(&T.t, a, s, n);
    of_free(del);
    T.caret = T.anchor = a + n;
    T.modified = true;
    if (T.t.nlines != lines0) {
        int d0 = 1;
        for (int k = T.t.nlines; k >= 10; k /= 10) d0++;
        build_rows();
        (void)d0;
    } else {
        rows_from(line);
    }
}

static bool has_sel(void) { return T.caret != T.anchor; }

static void del_sel(void)
{
    size_t a = T.caret < T.anchor ? T.caret : T.anchor, b = T.caret < T.anchor ? T.anchor : T.caret;
    replace(a, b, "", 0, false);
}

static void k_insert(const char *s)
{
    size_t a = T.caret < T.anchor ? T.caret : T.anchor, b = T.caret < T.anchor ? T.anchor : T.caret;
    replace(a, b, s, strlen(s), true);
    T.want_x = -1;
    scroll_to_caret();
    refresh();
}

static size_t prev_char(size_t p)
{
    if (!p) return 0;
    p--;
    while (p && (of_text_at(&T.t, p) & 0xC0) == 0x80) p--;
    return p;
}

static size_t next_char(size_t p)
{
    size_t len = of_text_len(&T.t);
    if (p >= len) return len;
    p++;
    while (p < len && (of_text_at(&T.t, p) & 0xC0) == 0x80) p++;
    return p;
}

static void k_bksp(void)
{
    if (has_sel()) del_sel();
    else if (T.caret) replace(prev_char(T.caret), T.caret, "", 0, false);
    scroll_to_caret();
    refresh();
}

static void k_enter(void)
{
    /* the new line keeps the indent of this one */
    int line = of_text_line_of(&T.t, T.caret);
    size_t a, b;
    of_text_line_span(&T.t, line, &a, &b);
    char ind[64] = "\n";
    int k = 1;
    for (size_t p = a; p < T.caret && k < 60; p++) {
        char c = of_text_at(&T.t, p);
        if (c != ' ' && c != '\t') break;
        ind[k++] = c;
    }
    ind[k] = 0;
    k_insert(ind);
}

static void move_to(size_t p, bool extend)
{
    T.caret = p;
    if (!extend) T.anchor = p;
    scroll_to_caret();
    refresh();
}

static void k_move(int dx)
{
    size_t p = T.caret;
    for (; dx < 0; dx++) p = prev_char(p);
    for (; dx > 0; dx--) p = next_char(p);
    T.want_x = -1;
    move_to(p, false);
}

static bool k_caps(void)
{
    if (T.lex != LX_NONE && T.lex != LX_MD) return false;
    /* at the start of the text or of a sentence */
    size_t p = T.caret;
    while (p && of_text_at(&T.t, p - 1) == ' ') p--;
    if (!p) return true;
    char c = of_text_at(&T.t, p - 1);
    return c == '\n' || ((c == '.' || c == '?' || c == '!') && p < T.caret);
}

static void vmove(int drow, bool extend)
{
    int32_t row, x;
    caret_xy(T.caret, &row, &x);
    if (T.want_x < 0) T.want_x = x;
    row += drow;
    if (row < 0) { move_to(0, extend); return; }
    if (row >= total_rows()) { move_to(of_text_len(&T.t), extend); return; }
    int32_t y = T.ay + row * T.lh - T.sy + T.lh / 2;
    size_t p = pos_at(T.ax + T.gutter + T.want_x - T.sx, y);
    T.caret = p;
    if (!extend) T.anchor = p;
    scroll_to_caret();
    refresh();
}

static void copy(bool cut)
{
    if (!has_sel()) return;
    size_t a = T.caret < T.anchor ? T.caret : T.anchor, b = T.caret < T.anchor ? T.anchor : T.caret;
    if (b - a > 256 * 1024) { of_app_toast(_("Es demasiado para copiar de una vez")); return; }
    char *s = of_text_range(&T.t, a, b);
    if (s) aos_ui_clip_set(s);
    of_free(s);
    if (cut) { del_sel(); scroll_to_caret(); refresh(); }
}

static void paste(void)
{
    size_t cap = 256 * 1024;
    char *buf = of_alloc(cap);
    if (!buf) return;
    size_t n = aos_ui_clip_get(buf, cap);
    if (n) {
        /* line ends of other systems become the editor's */
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\r') { buf[k++] = '\n'; if (i + 1 < n && buf[i + 1] == '\n') i++; }
            else buf[k++] = buf[i];
        }
        buf[k] = 0;
        size_t a = T.caret < T.anchor ? T.caret : T.anchor, b = T.caret < T.anchor ? T.anchor : T.caret;
        replace(a, b, buf, k, false);
        scroll_to_caret();
        refresh();
    }
    of_free(buf);
}

static void do_undo(void)
{
    if (!T.nundo) return;
    op_t o = T.undo[--T.nundo];
    size_t il = o.ins ? strlen(o.ins) : 0, dl = o.del ? strlen(o.del) : 0;
    if (il) of_text_delete(&T.t, o.pos, il);
    if (dl) of_text_insert(&T.t, o.pos, o.del, dl);
    T.caret = o.sel_b;
    T.anchor = o.sel_a;
    T.redo[T.nredo++] = o;
    T.modified = true;
    build_rows();
    scroll_to_caret();
    refresh();
}

static void do_redo(void)
{
    if (!T.nredo) return;
    op_t o = T.redo[--T.nredo];
    size_t il = o.ins ? strlen(o.ins) : 0, dl = o.del ? strlen(o.del) : 0;
    if (dl) of_text_delete(&T.t, o.pos, dl);
    if (il) of_text_insert(&T.t, o.pos, o.ins, il);
    T.caret = T.anchor = o.pos + il;
    T.undo[T.nundo++] = o;
    T.modified = true;
    build_rows();
    scroll_to_caret();
    refresh();
}

static void find_toggle(void);
static void find_next(bool back);
static void menu_open(void);
static void kb_show(bool on);

static void select_word(size_t p)
{
    size_t a = p, b = p, len = of_text_len(&T.t);
    while (a > 0 && is_word(of_text_at(&T.t, a - 1))) a--;
    while (b < len && is_word(of_text_at(&T.t, b))) b++;
    /* letters with accents count too */
    while (a > 0 && ((uint8_t)of_text_at(&T.t, a - 1) & 0x80)) a--;
    while (b < len && ((uint8_t)of_text_at(&T.t, b) & 0x80)) b++;
    T.anchor = a;
    T.caret = b;
}

static bool k_key(uint32_t k, uint8_t mods)
{
    bool shift = mods & OF_MOD_SHIFT, cmd = mods & OF_MOD_CMD;
    if (T.focus_ta) return false;           /* the find bar types for itself */
    if (cmd) {
        switch (k | 0x20) {
        case 'a': T.anchor = 0; T.caret = of_text_len(&T.t); refresh(); return true;
        case 'c': copy(false); return true;
        case 'x': copy(true); return true;
        case 'v': paste(); return true;
        case 'z': if (shift) do_redo(); else do_undo(); return true;
        case 'y': do_redo(); return true;
        case 's': of_textv_save(); return true;
        case 'f': find_toggle(); return true;
        case 'g': find_next(shift); return true;
        default: break;
        }
        if (k == AOS_KEY_HOME || k == AOS_KEY_UP) { move_to(0, shift); return true; }
        if (k == AOS_KEY_END || k == AOS_KEY_DOWN) { move_to(of_text_len(&T.t), shift); return true; }
        return true;
    }
    switch (k) {
    case AOS_KEY_LEFT: T.want_x = -1; move_to(shift ? prev_char(T.caret) : (has_sel() ? LV_MIN(T.caret, T.anchor) : prev_char(T.caret)), shift); return true;
    case AOS_KEY_RIGHT: T.want_x = -1; move_to(shift ? next_char(T.caret) : (has_sel() ? LV_MAX(T.caret, T.anchor) : next_char(T.caret)), shift); return true;
    case AOS_KEY_UP: vmove(-1, shift); return true;
    case AOS_KEY_DOWN: vmove(1, shift); return true;
    case AOS_KEY_HOME: case AOS_KEY_END: {
        int line = of_text_line_of(&T.t, T.caret);
        size_t a, b;
        of_text_line_span(&T.t, line, &a, &b);
        T.want_x = -1;
        move_to(k == AOS_KEY_HOME ? a : b, shift);
        return true;
    }
    case AOS_KEY_NEXT: k_insert(T.lex == LX_NONE || T.lex == LX_MD ? "\t" : "    "); return true;
    case AOS_KEY_DEL:
        if (has_sel()) del_sel();
        else if (T.caret < of_text_len(&T.t)) replace(T.caret, next_char(T.caret), "", 0, false);
        refresh();
        return true;
    case AOS_KEY_ESC:
        if (has_sel()) { T.anchor = T.caret; refresh(); }
        else kb_show(false);
        return true;
    default: break;
    }
    if (k > 0xFFFF && k != AOS_KEY_ENTER) return true;
    return false;
}

static const ofu_kb_target_t TARGET = { k_insert, k_bksp, k_enter, k_move, k_caps, k_key };

/* -------------------------------------------------------------------------- */
/* Gestures                                                                    */
/* -------------------------------------------------------------------------- */

static void fling_cb(lv_timer_t *t)
{
    (void)t;
    T.sy -= (int32_t)(T.vy / 60);
    T.vy *= 0.93f;
    int32_t o = T.sy;
    clamp();
    if (o != T.sy) T.vy = 0;
    lv_obj_invalidate(T.area);
    if (T.vy * T.vy < 400) { lv_timer_delete(T.fling); T.fling = NULL; }
}

static void stop_fling(void)
{
    if (T.fling) { lv_timer_delete(T.fling); T.fling = NULL; }
}

static void set_zoom(int z)
{
    if (z < 0 || z >= NZ || z == T.z) return;
    /* the line at the top stays at the top */
    int32_t row0 = T.sy / (T.lh ? T.lh : 1);
    int line = T.vrow ? line_of_row(row0) : 0;
    T.z = z;
    metrics();
    T.sy = T.vrow[line] * T.lh;
    clamp();
    aos_hal_pref_set_i32("of_tzoom", T.z);
    lv_obj_invalidate(T.area);
}

static void gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    (void)user;
    if (ofu_sheet_open()) return;
    sync_origin();
    switch (ev->type) {
    case AOS_GESTURE_TAP:
        stop_fling();
        T.focus_ta = NULL;
        ofu_kb_attach(&TARGET);
        T.want_x = -1;
        move_to(pos_at((int32_t)ev->x, (int32_t)ev->y), false);
        if (!T.kb) kb_show(true);
        break;
    case AOS_GESTURE_DOUBLE_TAP:
        select_word(pos_at((int32_t)ev->x, (int32_t)ev->y));
        refresh();
        break;
    case AOS_GESTURE_LONG_PRESS:
        select_word(pos_at((int32_t)ev->x, (int32_t)ev->y));
        T.selecting = true;
        refresh();
        break;
    case AOS_GESTURE_DRAG_BEGIN:
        stop_fling();
        T.selecting = ev->after_long;
        break;
    case AOS_GESTURE_DRAG:
        if (T.selecting) {
            T.caret = pos_at((int32_t)ev->x, (int32_t)ev->y);
            refresh();
            break;
        }
        T.sy -= (int32_t)ev->dy;
        if (!T.wrap) {
            T.sx -= (int32_t)ev->dx;
            if (T.sx < 0) T.sx = 0;
        }
        clamp();
        lv_obj_invalidate(T.area);
        break;
    case AOS_GESTURE_DRAG_END:
        if (!T.selecting && ev->vy * ev->vy > 90000) {
            T.vy = ev->vy;
            if (!T.fling) T.fling = lv_timer_create(fling_cb, 16, NULL);
        }
        T.selecting = false;
        break;
    case AOS_GESTURE_PINCH_BEGIN:
        stop_fling();
        T.pinch = 1;
        break;
    case AOS_GESTURE_PINCH:
        T.pinch *= ev->scale;
        if (T.pinch > 1.2f) { set_zoom(T.z + 1); T.pinch = 1; }
        else if (T.pinch < 0.83f) { set_zoom(T.z - 1); T.pinch = 1; }
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* Find and replace                                                            */
/* -------------------------------------------------------------------------- */

static void find_next(bool back)
{
    if (!T.f_ta) return;
    const char *needle = lv_textarea_get_text(T.f_ta);
    if (!needle[0]) return;
    size_t from = back ? (T.caret < T.anchor ? T.caret : T.anchor) : (T.caret > T.anchor ? T.caret : T.anchor);
    long hit;
    if (back) hit = from ? of_text_find(&T.t, from - 1, needle, true, false) : -1;
    else hit = of_text_find(&T.t, from, needle, false, false);
    if (hit < 0) hit = of_text_find(&T.t, back ? of_text_len(&T.t) : 0, needle, back, false);
    if (hit < 0) { of_app_toast(_("No está")); return; }
    T.anchor = (size_t)hit;
    T.caret = (size_t)hit + strlen(needle);
    scroll_to_caret();
    refresh();
}

static void replace_one(void)
{
    const char *needle = lv_textarea_get_text(T.f_ta), *with = lv_textarea_get_text(T.r_ta);
    if (!needle[0]) return;
    size_t a = T.caret < T.anchor ? T.caret : T.anchor, b = T.caret < T.anchor ? T.anchor : T.caret;
    char *sel = has_sel() ? of_text_range(&T.t, a, b) : NULL;
    if (sel && !strcasecmp(sel, needle)) replace(a, b, with, strlen(with), false);
    of_free(sel);
    find_next(false);
}

static void replace_all(void)
{
    const char *needle = lv_textarea_get_text(T.f_ta);
    char *with = of_strdup(lv_textarea_get_text(T.r_ta));
    if (!needle[0] || !with) { of_free(with); return; }
    size_t nl = strlen(needle), wl = strlen(with);
    int count = 0;
    long hit = of_text_find(&T.t, 0, needle, false, false);
    while (hit >= 0 && count < 100000) {
        replace((size_t)hit, (size_t)hit + nl, with, wl, false);
        count++;
        hit = of_text_find(&T.t, (size_t)hit + wl, needle, false, false);
    }
    of_free(with);
    char m[64];
    snprintf(m, sizeof m, _("%d reemplazos"), count);
    of_app_toast(m);
    build_rows();
    refresh();
}

static void ta_focus_cb(lv_event_t *e)
{
    T.focus_ta = lv_event_get_target(e);
    ofu_kb_attach_textarea(T.focus_ta, NULL);
    if (!T.kb) kb_show(true);
}

static void fnext_cb(lv_event_t *e) { (void)e; find_next(false); }
static void fprev_cb(lv_event_t *e) { (void)e; find_next(true); }
static void rone_cb(lv_event_t *e) { (void)e; replace_one(); }
static void rall_cb(lv_event_t *e) { (void)e; replace_all(); }
static void fclose_cb(lv_event_t *e) { (void)e; find_toggle(); }

static void find_done(lv_obj_t *ta)
{
    (void)ta;
    find_next(false);
}

static lv_obj_t *field(lv_obj_t *parent, const char *ph, int32_t w)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, ph);
    lv_obj_set_size(ta, w, 60);
    lv_obj_set_style_text_font(ta, ofu_font(0, OF_SZ_S), 0);
    lv_obj_set_style_bg_color(ta, OFT.surface2, 0);
    lv_obj_set_style_text_color(ta, OFT.text, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 12, 0);
    lv_obj_set_style_pad_ver(ta, 10, 0);
    lv_obj_set_user_data(ta, (void *)1);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void build_find(void)
{
    if (T.find) { lv_obj_delete(T.find); T.find = NULL; T.f_ta = T.r_ta = NULL; }
    T.find = ofu_box(T.root, T.W, T.replace_open ? 152 : 80);
    lv_obj_set_style_bg_color(T.find, OFT.surface, 0);
    lv_obj_set_style_bg_opa(T.find, LV_OPA_COVER, 0);
    int32_t bw = 72, fw = T.W - 24 - 3 * bw - 16;
    T.f_ta = field(T.find, _("Buscar"), fw);
    lv_obj_set_pos(T.f_ta, 12, 10);
    lv_obj_t *p = ofu_icon_btn(T.find, OF_IC_UP, bw, fprev_cb, NULL);
    lv_obj_set_pos(p, 12 + fw + 4, 4);
    lv_obj_t *n = ofu_icon_btn(T.find, OF_IC_CHEVRON, bw, fnext_cb, NULL);
    lv_obj_set_pos(n, 12 + fw + 4 + bw, 4);
    lv_obj_t *x = ofu_icon_btn(T.find, OF_IC_CLOSE, bw, fclose_cb, NULL);
    lv_obj_set_pos(x, 12 + fw + 4 + 2 * bw, 4);
    if (T.replace_open) {
        int32_t rw = T.W - 24 - 2 * 150 - 16;
        T.r_ta = field(T.find, _("Reemplazar por"), rw);
        lv_obj_set_pos(T.r_ta, 12, 82);
        lv_obj_t *b1 = ofu_pill(T.find, _("Este"), rone_cb, NULL);
        lv_obj_set_pos(b1, 12 + rw + 8, 82);
        lv_obj_t *b2 = ofu_pill(T.find, _("Todos"), rall_cb, NULL);
        lv_obj_set_pos(b2, 12 + rw + 8 + 150, 82);
    }
    /* the field takes the keys at once */
    T.focus_ta = T.f_ta;
    ofu_kb_attach_textarea(T.f_ta, find_done);
}

static void find_toggle(void)
{
    if (T.find) {
        lv_obj_delete(T.find);
        T.find = NULL;
        T.f_ta = T.r_ta = NULL;
        T.focus_ta = NULL;
        T.replace_open = false;
        ofu_kb_attach(&TARGET);
    } else {
        build_find();
        if (!T.kb) kb_show(true);
    }
    relayout();
}

/* -------------------------------------------------------------------------- */
/* The menu                                                                    */
/* -------------------------------------------------------------------------- */

static void goto_done(const char *text, void *ud)
{
    (void)ud;
    int line = atoi(text) - 1;
    if (line < 0) line = 0;
    if (line >= T.t.nlines) line = T.t.nlines - 1;
    size_t a, b;
    of_text_line_span(&T.t, line, &a, &b);
    ofu_kb_attach(&TARGET);
    move_to(a, false);
    T.sy = T.vrow[line] * T.lh - T.ah / 3;
    clamp();
    refresh();
}

static int s_tmenu[14];

static void pdf_done(const char *text, void *ud)
{
    (void)ud;
    if (!text[0] || strchr(text, '/')) return;
    char dir[300];
    snprintf(dir, sizeof dir, "%s", T.path);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0;
    char p[320];
    bool has = !strcasecmp(of_ext(text), "pdf");
    snprintf(p, sizeof p, "%.200s/%.100s%s", dir, text, has ? "" : ".pdf");
    if (of_file_exists(p)) { of_app_toast(_("Ya hay un archivo con ese nombre")); return; }
    size_t n = of_text_len(&T.t);
    char *all = of_text_range(&T.t, 0, n);
    char err[64];
    bool ok = all && of_pdf_write_text(all, n, p, of_basename(T.path), err, sizeof err);
    of_free(all);
    of_app_toast(ok ? _("Guardado como PDF") : _("No se pudo guardar"));
}

static void menu_pick(int i, void *ud)
{
    (void)ud;
    switch (i >= 0 && i < 14 ? s_tmenu[i] : -1) {
    case 0: of_textv_save(); break;
    case 1: find_toggle(); break;
    case 2:
        T.replace_open = true;
        if (T.find) { build_find(); relayout(); }
        else find_toggle();
        break;
    case 3: ofu_prompt(_("Ir a la línea"), "", _("Ir"), goto_done, NULL); break;
    case 4: T.wrap = !T.wrap; metrics(); clamp(); refresh(); break;
    case 5: T.numbers = !T.numbers; aos_hal_pref_set_i32("of_tnum", T.numbers); metrics(); refresh(); break;
    case 6: T.mono = !T.mono; metrics(); clamp(); refresh(); break;
    case 7:
        T.t.enc = T.t.enc == OT_ENC_UTF8 ? OT_ENC_LATIN1 : OT_ENC_UTF8;
        T.t.bom = false;
        T.modified = true;
        of_app_toast(T.t.enc == OT_ENC_UTF8 ? _("Se va a guardar en UTF-8") : _("Se va a guardar en Latin-1 (Windows-1252)"));
        refresh();
        break;
    case 8:
        T.t.eol = T.t.eol == OT_EOL_LF ? OT_EOL_CRLF : OT_EOL_LF;
        T.modified = true;
        of_app_toast(T.t.eol == OT_EOL_CRLF ? _("Finales de línea de Windows (CRLF)") : _("Finales de línea de Unix y Mac (LF)"));
        refresh();
        break;
    case 9: T.anchor = 0; T.caret = of_text_len(&T.t); refresh(); break;
    case 10: copy(false); break;
    case 11: paste(); break;
    case 13: {
        char base[128];
        snprintf(base, sizeof base, "%s", of_basename(T.path));
        char *dot = strrchr(base, '.');
        if (dot && dot != base) *dot = 0;
        ofu_prompt(_("Exportar a PDF"), base, _("Guardar"), pdf_done, NULL);
        break;
    }
    case 12: {
        if (T.modified) { of_app_toast(_("Guardá los cambios primero")); break; }
        char p[300];
        snprintf(p, sizeof p, "%s", T.path);
        of_app_open_as(p, OF_OPEN_VIEW);
        break;
    }
    }
}

static void menu_open(void)
{
    static const char *const ENC[] = { "UTF-8", "Latin-1", "UTF-16", "UTF-16" };
    char enc[64], eol[64], info[96];
    snprintf(enc, sizeof enc, _("Codificación: %s"), ENC[T.t.enc & 3]);
    snprintf(eol, sizeof eol, _("Fin de línea: %s"), T.t.eol == OT_EOL_CRLF ? "CRLF" : T.t.eol == OT_EOL_CR ? "CR" : "LF");
    snprintf(info, sizeof info, _("%d líneas"), T.t.nlines);
    ofu_item_t all[14] = {
        { _("Guardar"), OF_IC_SAVE, false, false },
        { _("Buscar"), OF_IC_SEARCH, false, false },
        { _("Buscar y reemplazar"), OF_IC_SEARCH, false, false },
        { _("Ir a la línea..."), OF_IC_LIST, false, false },
        { _("Ajustar las líneas"), OF_IC_ROWS, false, T.wrap },
        { _("Números de línea"), OF_IC_LIST, false, T.numbers },
        { _("Letra de ancho fijo"), OF_IC_TYPE, false, T.mono },
        { enc, OF_IC_TYPE, false, false },
        { eol, OF_IC_TYPE, false, false },
        { _("Seleccionar todo"), OF_IC_CHECKBOX, false, false },
        { _("Copiar"), OF_IC_COPY, false, false },
        { _("Pegar"), OF_IC_COPY, false, false },
        { of_is_tree_ext(of_ext(T.path)) ? _("Ver como árbol") : _("Ver con formato"), OF_IC_DOC, false, false },
        { _("Exportar a PDF..."), OF_IC_DOC, false, false },
    };
    /* "Ver con formato" second, after Guardar, when the file has a view */
    ofu_item_t items[14];
    int n = 0;
    for (int k = 0; k < 14; k++) {
        int id = k == 0 ? 0 : k == 1 ? 12 : k == 13 ? 13 : k - 1;
        if (id == 12 && !of_app_can_view(T.path)) continue;
        s_tmenu[n] = id;
        items[n++] = all[id];
    }
    ofu_sheet(info, items, n, menu_pick, NULL);
}

/* -------------------------------------------------------------------------- */
/* Screen                                                                      */
/* -------------------------------------------------------------------------- */

static void refresh(void)
{
    if (T.area) lv_obj_invalidate(T.area);
    if (T.b_undo) { if (T.nundo) lv_obj_remove_state(T.b_undo, LV_STATE_DISABLED); else lv_obj_add_state(T.b_undo, LV_STATE_DISABLED); }
    if (T.b_redo) { if (T.nredo) lv_obj_remove_state(T.b_redo, LV_STATE_DISABLED); else lv_obj_add_state(T.b_redo, LV_STATE_DISABLED); }
    if (T.title) {
        char t[160];
        snprintf(t, sizeof t, "%s%s", T.modified ? "\xE2\x80\xA2 " : "", of_basename(T.path));
        lv_label_set_text(T.title, t);
    }
}

static void kb_show(bool on)
{
    if (on && !T.kb) {
        int32_t kbh = T.W > T.H ? 300 : 420;
        T.kb = ofu_kb_create(T.root, T.W, kbh);
        lv_obj_align(T.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    } else if (!on && T.kb) {
        lv_obj_delete(T.kb);
        T.kb = NULL;
    }
    if (T.b_kb) ofu_icon_set(T.b_kb, T.kb ? OF_IC_KB_HIDE : OF_IC_KEYBOARD);
    relayout();
    if (on) scroll_to_caret();
}

static void relayout(void)
{
    T.W = lv_obj_get_width(T.root);
    T.H = lv_obj_get_height(T.root);
    int32_t top = 88, fh = T.find ? lv_obj_get_height(T.find) : 0, kbh = T.kb ? lv_obj_get_height(T.kb) : 0;
    lv_obj_set_size(T.top, T.W, top);
    if (T.find) lv_obj_set_pos(T.find, 0, top);
    T.aw = T.W;
    T.ah = T.H - (top + fh) - kbh;
    lv_obj_set_pos(T.area, 0, top + fh);
    lv_obj_set_size(T.area, T.aw, T.ah);
    lv_obj_update_layout(T.area);
    sync_origin();
    lv_obj_set_width(T.title, T.W - 5 * 84 - 100);
    metrics();
    clamp();
    refresh();
}

static void back_cb(lv_event_t *e) { (void)e; of_app_back_from_doc(); }
static void undo_cb(lv_event_t *e) { (void)e; do_undo(); }
static void redo_cb(lv_event_t *e) { (void)e; do_redo(); }
static void more_cb(lv_event_t *e) { (void)e; menu_open(); }
static void kb_cb(lv_event_t *e)
{
    (void)e;
    if (!T.kb) { T.focus_ta = NULL; ofu_kb_attach(&TARGET); }
    kb_show(!T.kb);
}

bool of_textv_open(lv_obj_t *parent, const char *path)
{
    if (path) {
        of_textv_close();
        memset(&T, 0, sizeof T);
        char err[32];
        if (!s_lbuf) s_lbuf = of_alloc(LBUF + 8);
        if (!s_lbuf || !of_text_load(&T.t, path, err, sizeof err)) return false;
        snprintf(T.path, sizeof T.path, "%s", path);
        T.lex = lexer_of(path);
        T.wrap = T.lex == LX_NONE || T.lex == LX_MD;
        T.mono = T.lex != LX_NONE && T.lex != LX_MD;
        int32_t v = 2;
        aos_hal_pref_get_i32("of_tzoom", &v);
        T.z = v >= 0 && v < NZ ? (int)v : 2;
        v = 1;
        aos_hal_pref_get_i32("of_tnum", &v);
        T.numbers = v != 0;
        T.want_x = -1;
    } else if (!T.t.buf) {
        return false;
    }
    stop_fling();
    T.root = parent;
    T.kb = T.find = NULL;
    T.f_ta = T.r_ta = T.focus_ta = NULL;
    ofu_ui_root = parent;
    lv_obj_set_style_bg_color(parent, OFT.bg, 0);
    T.top = ofu_box(parent, 10, 88);
    lv_obj_set_style_bg_color(T.top, OFT.surface, 0);
    lv_obj_set_style_bg_opa(T.top, LV_OPA_COVER, 0);
    lv_obj_t *bk = ofu_icon_btn(T.top, OF_IC_BACK, 84, back_cb, NULL);
    lv_obj_align(bk, LV_ALIGN_LEFT_MID, 4, 0);
    T.title = ofu_text(T.top, "", ofu_font(OF_ST_BOLD, OF_SZ_M), OFT.text);
    lv_label_set_long_mode(T.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(T.title, ofu_font(OF_ST_BOLD, OF_SZ_M)->line_height);
    lv_obj_align(T.title, LV_ALIGN_LEFT_MID, 92, 0);
    lv_obj_t *mo = ofu_icon_btn(T.top, OF_IC_MORE, 84, more_cb, NULL);
    lv_obj_align(mo, LV_ALIGN_RIGHT_MID, -4, 0);
    T.b_kb = ofu_icon_btn(T.top, OF_IC_KEYBOARD, 84, kb_cb, NULL);
    lv_obj_align(T.b_kb, LV_ALIGN_RIGHT_MID, -88, 0);
    T.b_redo = ofu_icon_btn(T.top, OF_IC_REDO, 84, redo_cb, NULL);
    lv_obj_align(T.b_redo, LV_ALIGN_RIGHT_MID, -172, 0);
    T.b_undo = ofu_icon_btn(T.top, OF_IC_UNDO, 84, undo_cb, NULL);
    lv_obj_align(T.b_undo, LV_ALIGN_RIGHT_MID, -256, 0);
    T.area = ofu_box(parent, 10, 10);
    lv_obj_add_flag(T.area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(T.area, area_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    aos_gesture_attach(T.area, AOS_GESTURE_FLAG_FAST_TAP, gesture_cb, NULL);
    ofu_kb_attach(&TARGET);
    relayout();
    return true;
}

bool of_textv_modified(void)
{
    return T.t.buf && T.modified;
}

bool of_textv_save(void)
{
    char err[32];
    if (!of_text_save(&T.t, T.path, err, sizeof err)) {
        of_app_toast(_("No se pudo guardar"));
        return false;
    }
    T.modified = false;
    of_app_toast(_("Guardado"));
    refresh();
    return true;
}

bool of_textv_save_recovery(void)
{
    if (!T.t.buf) return false;
    char p[340], err[32];
    const char *e = of_ext(T.path);
    int bl = (int)strlen(T.path) - (e[0] ? (int)strlen(e) + 1 : 0);
    snprintf(p, sizeof p, "%.*s (sin guardar)%s%s", bl > 280 ? 280 : bl, T.path, e[0] ? "." : "", e);
    return of_text_save(&T.t, p, err, sizeof err);
}

bool of_textv_back(void)
{
    if (T.find) { find_toggle(); return true; }
    if (T.kb) { kb_show(false); return true; }
    if (has_sel()) { T.anchor = T.caret; refresh(); return true; }
    return false;
}

void of_textv_close(void)
{
    stop_fling();
    for (int i = 0; i < T.nundo; i++) op_free(&T.undo[i]);
    for (int i = 0; i < T.nredo; i++) op_free(&T.redo[i]);
    of_text_free(&T.t);
    of_free(T.vrow);
    memset(&T, 0, sizeof T);
}
