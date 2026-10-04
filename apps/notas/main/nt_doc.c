/*
 * P4OS - Notes: the document, and its Markdown.
 *
 * On the card a note looks like this:
 *
 *     ---
 *     title: Compras
 *     type: note
 *     color: yellow
 *     pinned: true
 *     created: 2026-10-04 11:20
 *     modified: 2026-10-04 11:42
 *     ---
 *     # Para el sábado
 *     Algo con **negrita**, *cursiva*, <u>subrayado</u> y ~~tachado~~.
 *     <span style="color:red;background:yellow;font-size:large">Esto</span> en grande.
 *     - un punto
 *       ◦ otro, adentro
 *     1. primero
 *     - [x] hecho
 *     - [ ] !! pendiente, prioridad media
 *     > una cita
 *     ---
 *
 * One line per block, two spaces per level of indent, and the bullet's own
 * glyph for the styles Markdown has no marker for. It is Markdown enough for
 * any viewer, and plain enough to edit from the portal's file manager. The
 * reader takes anything: a line it cannot make out is a paragraph, a tag it
 * does not know is text, a lone asterisk is an asterisk.
 */
#include "nt.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#if !defined(AOS_SIM)
#include "esp_heap_caps.h"
#endif

/* -------------------------------------------------------------------------- */
/* Memory                                                                      */
/* -------------------------------------------------------------------------- */

void *nt_alloc(size_t n)
{
    if (!n) n = 1;
#if defined(AOS_SIM)
    return calloc(1, n);
#else
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
#endif
}

void *nt_realloc(void *p, size_t n)
{
    if (!n) n = 1;
#if defined(AOS_SIM)
    return realloc(p, n);
#else
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
#endif
}

void nt_free(void *p)
{
    free(p);
}

char *nt_strdup(const char *s)
{
    size_t n = strlen(s ? s : "");
    char *d = nt_alloc(n + 1);
    if (d && n) memcpy(d, s, n);
    return d;
}

void nt_sb_put(nt_sb_t *b, const char *s, int n)
{
    if (n < 0) n = (int)strlen(s);
    if (b->n + n + 1 > b->cap) {
        int cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *q = nt_realloc(b->s, cap);
        if (!q) return;
        b->s = q;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

void nt_sb_putc(nt_sb_t *b, char c)
{
    nt_sb_put(b, &c, 1);
}

void nt_sb_printf(nt_sb_t *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) nt_sb_put(b, tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp - 1);
}

void nt_sb_free(nt_sb_t *b)
{
    nt_free(b->s);
    memset(b, 0, sizeof *b);
}

/* -------------------------------------------------------------------------- */
/* Blocks                                                                      */
/* -------------------------------------------------------------------------- */

static void blk_reserve(nt_blk_t *b, int n)
{
    if (n + 1 <= b->cap) return;
    int cap = b->cap ? b->cap : 32;
    while (cap < n + 1) cap *= 2;
    char *t = nt_realloc(b->txt, cap);
    uint16_t *a = nt_realloc(b->at, cap * sizeof(uint16_t));
    if (t) b->txt = t;
    if (a) b->at = a;
    if (t && a) b->cap = cap;
}

static void blk_free(nt_blk_t *b)
{
    nt_free(b->txt);
    nt_free(b->at);
    nt_free(b->lines);
    memset(b, 0, sizeof *b);
}

nt_blk_t *nt_doc_insert(nt_doc_t *d, int at, int type)
{
    if (d->n + 1 > d->cap) {
        int cap = d->cap ? d->cap * 2 : 32;
        nt_blk_t *q = nt_realloc(d->b, cap * sizeof(nt_blk_t));
        if (!q) return NULL;
        d->b = q;
        d->cap = cap;
    }
    if (at < 0) at = 0;
    if (at > d->n) at = d->n;
    memmove(&d->b[at + 1], &d->b[at], (d->n - at) * sizeof(nt_blk_t));
    d->n++;
    nt_blk_t *b = &d->b[at];
    memset(b, 0, sizeof *b);
    b->type = (uint8_t)type;
    blk_reserve(b, 16);
    if (b->txt) b->txt[0] = 0;
    return b;
}

void nt_doc_remove(nt_doc_t *d, int at)
{
    if (at < 0 || at >= d->n) return;
    blk_free(&d->b[at]);
    memmove(&d->b[at], &d->b[at + 1], (d->n - at - 1) * sizeof(nt_blk_t));
    d->n--;
}

void nt_doc_init(nt_doc_t *d)
{
    memset(d, 0, sizeof *d);
    nt_doc_insert(d, 0, BT_TITLE);
}

void nt_doc_clear(nt_doc_t *d)
{
    for (int i = 0; i < d->n; i++) blk_free(&d->b[i]);
    nt_free(d->b);
    memset(d, 0, sizeof *d);
}

void nt_blk_insert(nt_blk_t *b, int pos, const char *s, int n, uint16_t attr)
{
    if (n <= 0) return;
    blk_reserve(b, b->len + n);
    if (b->cap < b->len + n + 1) return;
    if (pos < 0) pos = 0;
    if (pos > b->len) pos = b->len;
    memmove(b->txt + pos + n, b->txt + pos, b->len - pos);
    memmove(b->at + pos + n, b->at + pos, (b->len - pos) * sizeof(uint16_t));
    memcpy(b->txt + pos, s, n);
    for (int i = 0; i < n; i++) b->at[pos + i] = attr;
    b->len += n;
    b->txt[b->len] = 0;
}

void nt_blk_delete(nt_blk_t *b, int pos, int n)
{
    if (pos < 0) { n += pos; pos = 0; }
    if (pos + n > b->len) n = b->len - pos;
    if (n <= 0) return;
    memmove(b->txt + pos, b->txt + pos + n, b->len - pos - n);
    memmove(b->at + pos, b->at + pos + n, (b->len - pos - n) * sizeof(uint16_t));
    b->len -= n;
    b->txt[b->len] = 0;
}

void nt_blk_set(nt_blk_t *b, const char *s, uint16_t attr)
{
    b->len = 0;
    if (b->txt) b->txt[0] = 0;
    nt_blk_insert(b, 0, s, (int)strlen(s), attr);
}

static bool is_list(int t)
{
    return t == BT_BULLET || t == BT_NUM || t == BT_CHECK;
}

void nt_doc_number(nt_doc_t *d)
{
    int cnt[NT_MAX_INDENT + 1] = { 0 };
    int sty[NT_MAX_INDENT + 1] = { 0 };
    for (int i = 1; i < d->n; i++) {
        nt_blk_t *b = &d->b[i];
        int k = b->indent > NT_MAX_INDENT ? NT_MAX_INDENT : b->indent;
        for (int j = k + 1; j <= NT_MAX_INDENT; j++) cnt[j] = 0;
        if (!is_list(b->type)) {
            for (int j = k; j <= NT_MAX_INDENT; j++) cnt[j] = 0;
            b->num = 0;
            continue;
        }
        if (b->type == BT_NUM) {
            if (cnt[k] > 0 && sty[k] == b->style) cnt[k]++;
            else { cnt[k] = 1; sty[k] = b->style; }
            b->num = cnt[k];
        } else {
            cnt[k] = 0;
            b->num = 0;
        }
    }
}

static void roman(int n, bool upper, char *out, size_t len)
{
    static const int v[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    static const char *const s[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < 13 && n > 0; i++) {
        while (n >= v[i] && o + 3 < len) {
            for (const char *p = s[i]; *p; p++) out[o++] = upper ? (char)toupper((unsigned char)*p) : *p;
            n -= v[i];
        }
    }
    out[o] = 0;
}

static void alpha(int n, bool upper, char *out, size_t len)
{
    char tmp[8];
    int k = 0;
    while (n > 0 && k < 7) {
        n--;
        tmp[k++] = (char)((upper ? 'A' : 'a') + n % 26);
        n /= 26;
    }
    size_t o = 0;
    while (k > 0 && o + 1 < len) out[o++] = tmp[--k];
    out[o] = 0;
}

void nt_num_label(int style, int n, char *out, size_t len)
{
    char core[16];
    if (n < 1) n = 1;
    switch (style) {
    case NT_NS_ALPHA:     alpha(n, false, core, sizeof core); snprintf(out, len, "%s)", core); break;
    case NT_NS_ALPHA_UP:  alpha(n, true, core, sizeof core);  snprintf(out, len, "%s)", core); break;
    case NT_NS_ROMAN:     roman(n, false, core, sizeof core); snprintf(out, len, "%s.", core); break;
    case NT_NS_ROMAN_UP:  roman(n, true, core, sizeof core);  snprintf(out, len, "%s.", core); break;
    default:              snprintf(out, len, "%d.", n); break;
    }
}

/* -------------------------------------------------------------------------- */
/* Writing Markdown                                                            */
/* -------------------------------------------------------------------------- */

/* The bullets Markdown has no marker for travel as their own glyph. */
static const char *const BULLET_MARK[NT_BS_COUNT] = {
    "-", "\xE2\x97\xA6", "\xE2\x96\xAA", "\xE2\x80\x93", "\xE2\x86\x92",
    "\xE2\x98\x85", "\xE2\x9C\x93", "\xE2\x97\x86" };   /* - ◦ ▪ – → ★ ✓ ◆ */

/* CSS names, so a Markdown viewer shows something close. */
static const char *const FG_CSS[NT_FG_COUNT] = {
    "", "red", "orange", "gold", "green", "teal", "royalblue", "purple", "deeppink", "gray" };
static const char *const HL_CSS[NT_HL_COUNT] = {
    "", "yellow", "lightgreen", "lightblue", "pink", "orange", "plum" };
static const char *const SZ_CSS[NT_SZ_COUNT] = { "", "small", "large", "x-large" };

static void put_escaped(nt_sb_t *o, const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\' || c == '*' || c == '~' || c == '<') nt_sb_putc(o, '\\');
        nt_sb_putc(o, c);
    }
}

static void open_run(nt_sb_t *o, uint16_t a, bool marks)
{
    if (A_FG(a) || A_HL(a) || A_SZ(a)) {
        nt_sb_put(o, "<span style=\"", -1);
        const char *sep = "";
        if (A_FG(a) && A_FG(a) < NT_FG_COUNT) { nt_sb_printf(o, "color:%s", FG_CSS[A_FG(a)]); sep = ";"; }
        if (A_HL(a) && A_HL(a) < NT_HL_COUNT) { nt_sb_printf(o, "%sbackground:%s", sep, HL_CSS[A_HL(a)]); sep = ";"; }
        if (A_SZ(a)) nt_sb_printf(o, "%sfont-size:%s", sep, SZ_CSS[A_SZ(a)]);
        nt_sb_put(o, "\">", -1);
    }
    if (a & A_U) nt_sb_put(o, "<u>", -1);
    if (!marks) return;
    if (a & A_S) nt_sb_put(o, "~~", -1);
    if (a & A_B) nt_sb_put(o, "**", -1);
    if (a & A_I) nt_sb_put(o, "*", -1);
}

static void close_run(nt_sb_t *o, uint16_t a, bool marks)
{
    if (marks) {
        if (a & A_I) nt_sb_put(o, "*", -1);
        if (a & A_B) nt_sb_put(o, "**", -1);
        if (a & A_S) nt_sb_put(o, "~~", -1);
    }
    if (a & A_U) nt_sb_put(o, "</u>", -1);
    if (A_FG(a) || A_HL(a) || A_SZ(a)) nt_sb_put(o, "</span>", -1);
}

static void put_inline(nt_sb_t *o, const nt_blk_t *b)
{
    int i = 0;
    while (i < b->len) {
        uint16_t a = b->at[i];
        int j = i;
        while (j < b->len && b->at[j] == a) j++;
        if (!a) {
            put_escaped(o, b->txt + i, j - i);
        } else {
            /* Emphasis marks must hug the text ("** x**" is not bold), so
             * the run's own spaces go outside them. */
            int s = i, e = j;
            while (s < e && b->txt[s] == ' ') s++;
            while (e > s && b->txt[e - 1] == ' ') e--;
            bool marks = (a & (A_B | A_I | A_S)) && e > s;
            open_run(o, a, false);
            put_escaped(o, b->txt + i, s - i);
            if (marks) open_run(o, a & (A_B | A_I | A_S), true);
            put_escaped(o, b->txt + s, e - s);
            if (marks) close_run(o, a & (A_B | A_I | A_S), true);
            put_escaped(o, b->txt + e, j - e);
            close_run(o, a, false);
        }
        i = j;
    }
}

/* Would this paragraph read back as something else? Then a backslash goes
 * first. Erring on the side of escaping costs a character. */
static bool needs_escape(const nt_blk_t *b)
{
    if (b->len == 0) return false;
    unsigned char c = (unsigned char)b->txt[0];
    if (c == ' ' || c == '\t' || c >= 0x80 || ispunct(c) || isdigit(c)) return true;
    if (isalpha(c) && b->len > 1 && (b->txt[1] == ')' || b->txt[1] == '.')) return true;
    /* a roman numeral and a dot */
    int k = 0;
    while (k < b->len && strchr("ivxlcdmIVXLCDM", b->txt[k])) k++;
    return k > 0 && k < b->len && b->txt[k] == '.';
}

static void put_date(nt_sb_t *o, const char *key, uint32_t t)
{
    if (!t) return;
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    nt_sb_printf(o, "%s: %04d-%02d-%02d %02d:%02d\n", key, tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min);
}

void nt_doc_to_md(const nt_doc_t *d, const nt_meta_t *m, nt_sb_t *o)
{
    nt_sb_put(o, "---\n", -1);
    const char *title = d->n ? d->b[0].txt : "";
    bool quote = strpbrk(title, ":#\"'\\") || title[0] == ' ' || title[0] == '-' ||
                 (title[0] && title[strlen(title) - 1] == ' ');
    nt_sb_put(o, "title: ", -1);
    if (quote) {
        nt_sb_putc(o, '"');
        for (const char *p = title; *p; p++) {
            if (*p == '"' || *p == '\\') nt_sb_putc(o, '\\');
            nt_sb_putc(o, *p);
        }
        nt_sb_putc(o, '"');
    } else {
        nt_sb_put(o, title, -1);
    }
    nt_sb_putc(o, '\n');
    nt_sb_put(o, m->kind == NT_KIND_TASKS ? "type: tasks\n" : "type: note\n", -1);
    if (m->tag) nt_sb_printf(o, "color: %s\n", nt_tag_name(m->tag));
    if (m->pinned) nt_sb_put(o, "pinned: true\n", -1);
    if (m->hide_done) nt_sb_put(o, "hide_done: true\n", -1);
    put_date(o, "created", m->created);
    put_date(o, "modified", m->modified);
    nt_sb_put(o, "---\n", -1);

    /* a body that is one empty paragraph is no body */
    if (d->n == 2 && d->b[1].type == BT_PARA && d->b[1].len == 0) return;

    for (int i = 1; i < d->n; i++) {
        const nt_blk_t *b = &d->b[i];
        if (b->type == BT_RULE) {
            nt_sb_put(o, "---\n", -1);
            continue;
        }
        for (int k = 0; k < b->indent; k++) nt_sb_put(o, "  ", 2);
        char lab[24];
        switch (b->type) {
        case BT_H1: nt_sb_put(o, "# ", -1); break;
        case BT_H2: nt_sb_put(o, "## ", -1); break;
        case BT_H3: nt_sb_put(o, "### ", -1); break;
        case BT_QUOTE: nt_sb_put(o, "> ", -1); break;
        case BT_BULLET:
            nt_sb_put(o, BULLET_MARK[b->style < NT_BS_COUNT ? b->style : 0], -1);
            nt_sb_putc(o, ' ');
            if (b->len && b->txt[0] == '[') nt_sb_putc(o, '\\');
            break;
        case BT_NUM:
            nt_num_label(b->style, b->num, lab, sizeof lab);
            nt_sb_put(o, lab, -1);
            nt_sb_putc(o, ' ');
            break;
        case BT_CHECK:
            nt_sb_put(o, b->checked ? "- [x] " : "- [ ] ", -1);
            for (int k = 0; k < (b->style & 3); k++) nt_sb_putc(o, '!');
            if (b->style & 3) nt_sb_putc(o, ' ');
            else if (b->len && b->txt[0] == '!') nt_sb_putc(o, '\\');
            break;
        default:
            if (needs_escape(b)) nt_sb_putc(o, '\\');
            break;
        }
        put_inline(o, b);
        nt_sb_putc(o, '\n');
    }
}

/* -------------------------------------------------------------------------- */
/* Reading Markdown                                                            */
/* -------------------------------------------------------------------------- */

static int css_index(const char *v, int n, const char *const *names, int count)
{
    for (int i = 1; i < count; i++) {
        if ((int)strlen(names[i]) == n && strncmp(v, names[i], n) == 0) return i;
    }
    return 0;
}

/* <span style="color:red;background:yellow;font-size:large"> -> the style word */
static uint16_t parse_span(const char *s, int n, uint16_t a)
{
    const char *st = NULL;
    for (int i = 0; i + 6 < n; i++) {
        if (strncmp(s + i, "style=", 6) == 0) { st = s + i + 6; break; }
    }
    if (!st) return a;
    const char *end = s + n;
    if (*st == '"' || *st == '\'') st++;
    while (st < end && *st != '"' && *st != '\'' && *st != '>') {
        const char *k = st;
        while (st < end && *st != ':' && *st != ';' && *st != '"' && *st != '\'') st++;
        int kl = (int)(st - k);
        if (st >= end || *st != ':') { if (st < end && *st == ';') st++; continue; }
        st++;
        while (st < end && *st == ' ') st++;
        const char *v = st;
        while (st < end && *st != ';' && *st != '"' && *st != '\'') st++;
        int vl = (int)(st - v);
        while (vl > 0 && v[vl - 1] == ' ') vl--;
        while (kl > 0 && k[kl - 1] == ' ') kl--;
        while (kl > 0 && *k == ' ') { k++; kl--; }
        if (kl == 5 && strncmp(k, "color", 5) == 0) {
            int i = css_index(v, vl, FG_CSS, NT_FG_COUNT);
            if (!i && vl == 6 && strncmp(v, "yellow", 6) == 0) i = 3;
            a = (uint16_t)((a & ~A_FG_MASK) | (i << A_FG_SHIFT));
        } else if ((kl == 10 && strncmp(k, "background", 10) == 0) ||
                   (kl == 16 && strncmp(k, "background-color", 16) == 0)) {
            int i = css_index(v, vl, HL_CSS, NT_HL_COUNT);
            a = (uint16_t)((a & ~A_HL_MASK) | (i << A_HL_SHIFT));
        } else if (kl == 9 && strncmp(k, "font-size", 9) == 0) {
            int i = css_index(v, vl, SZ_CSS, NT_SZ_COUNT);
            if (!i && vl == 8 && strncmp(v, "xx-large", 8) == 0) i = NT_SZ_XL;
            a = (uint16_t)((a & ~A_SZ_MASK) | (i << A_SZ_SHIFT));
        }
        if (st < end && *st == ';') st++;
    }
    return a;
}

/* Is there a closing 'mark' later on the line, not escaped? */
static bool closes_later(const char *s, int i, int n, const char *mark, int ml)
{
    for (int j = i; j + ml <= n; j++) {
        if (s[j] == '\\') { j++; continue; }
        if (strncmp(s + j, mark, ml) == 0 && j > 0 && s[j - 1] != ' ') {
            /* "**" is not a closing "*" */
            if (ml == 1 && j + 1 < n && s[j + 1] == '*') { j++; continue; }
            return true;
        }
    }
    return false;
}

/* strncasecmp is not among the firmware's symbols */
static bool same_ci(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

#define SPAN_DEPTH 8

static void parse_inline(nt_blk_t *b, const char *s, int n)
{
    uint16_t span[SPAN_DEPTH];
    int depth = 0;
    uint16_t base = 0, flags = 0;
    int bold = 0, ital = 0, strike = 0, under = 0, mark = 0;
    int i = 0;
    while (i < n) {
        char c = s[i];
        uint16_t a = (uint16_t)(base | flags);
        if (c == '\\' && i + 1 < n && ispunct((unsigned char)s[i + 1])) {
            nt_blk_insert(b, b->len, s + i + 1, 1, a);
            i += 2;
            continue;
        }
        if (c == '*' || c == '~') {
            bool dbl = i + 1 < n && s[i + 1] == c;
            if (c == '~' && !dbl) goto literal;
            int ml = dbl ? 2 : 1;
            const char *mk = c == '~' ? "~~" : (dbl ? "**" : "*");
            uint16_t bit = c == '~' ? A_S : (dbl ? A_B : A_I);
            /* "***x***" is bold and italic */
            if (c == '*' && dbl && i + 2 < n && s[i + 2] == '*' && !(flags & A_B) && !(flags & A_I) &&
                i + 3 < n && s[i + 3] != ' ' && closes_later(s, i + 3, n, "***", 3)) {
                flags |= A_B | A_I;
                i += 3;
                continue;
            }
            if (c == '*' && dbl && i + 2 < n && s[i + 2] == '*' && (flags & A_B) && (flags & A_I)) {
                flags &= (uint16_t)~(A_B | A_I);
                i += 3;
                continue;
            }
            if (flags & bit) {
                if (i > 0 && s[i - 1] != ' ') { flags &= (uint16_t)~bit; i += ml; continue; }
            } else if (i + ml < n && s[i + ml] != ' ' && closes_later(s, i + ml, n, mk, ml)) {
                flags |= bit;
                i += ml;
                continue;
            }
            goto literal;
        }
        if (c == '<') {
            const char *gt = memchr(s + i, '>', n - i);
            if (!gt) goto literal;
            int tl = (int)(gt - (s + i)) + 1;
            const char *t = s + i + 1;
            int l = tl - 2;
            bool close = l > 0 && t[0] == '/';
            if (close) { t++; l--; }
            int nl = 0;
            while (nl < l && isalpha((unsigned char)t[nl])) nl++;
            #define TAG(x) (nl == (int)sizeof(x) - 1 && same_ci(t, x, nl))
            if (TAG("u") || TAG("ins")) {
                under += close ? -1 : 1;
                if (under < 0) under = 0;
                flags = under ? (flags | A_U) : (flags & (uint16_t)~A_U);
            } else if (TAG("b") || TAG("strong")) {
                bold += close ? -1 : 1;
                if (bold < 0) bold = 0;
                flags = bold ? (flags | A_B) : (flags & (uint16_t)~A_B);
            } else if (TAG("i") || TAG("em")) {
                ital += close ? -1 : 1;
                if (ital < 0) ital = 0;
                flags = ital ? (flags | A_I) : (flags & (uint16_t)~A_I);
            } else if (TAG("s") || TAG("del") || TAG("strike")) {
                strike += close ? -1 : 1;
                if (strike < 0) strike = 0;
                flags = strike ? (flags | A_S) : (flags & (uint16_t)~A_S);
            } else if (TAG("mark")) {
                mark += close ? -1 : 1;
                if (mark < 0) mark = 0;
                base = (uint16_t)((base & ~A_HL_MASK) | ((mark ? 1 : 0) << A_HL_SHIFT));
            } else if (TAG("span") || TAG("font")) {
                if (close) {
                    if (depth > 0) base = span[--depth];
                } else if (depth < SPAN_DEPTH) {
                    span[depth++] = base;
                    base = parse_span(s + i, tl, base);
                }
            } else if (TAG("br")) {
                /* a block is one line */
            } else {
                goto literal;
            }
            #undef TAG
            i += tl;
            continue;
        }
    literal:
        nt_blk_insert(b, b->len, s + i, 1, a);
        i++;
    }
}

static bool starts(const char *s, int n, const char *p)
{
    int l = (int)strlen(p);
    return n >= l && strncmp(s, p, l) == 0;
}

/* The marker at the start of a line: sets type and style, returns its length. */
static int marker(const char *s, int n, nt_blk_t *b, int prev_type, int prev_style)
{
    static const char *const ALT[][2] = {
        { "* ", "\0" }, { "+ ", "\0" }, { "\xE2\x80\xA2 ", "\0" },      /* • */
        { "\xE2\x96\xA0 ", "\2" }, { "\xE2\x9E\xA4 ", "\4" },           /* ■ ➤ */
        { "\xE2\x9C\x94 ", "\6" }, { "\xE2\x99\xA6 ", "\7" },           /* ✔ ♦ */
    };
    if (starts(s, n, "### ") || starts(s, n, "#### ") || starts(s, n, "##### ")) {
        b->type = BT_H3;
        int k = 0;
        while (k < n && s[k] == '#') k++;
        return k + 1;
    }
    if (starts(s, n, "## ")) { b->type = BT_H2; return 3; }
    if (starts(s, n, "# "))  { b->type = BT_H1; return 2; }
    if (starts(s, n, "> "))  { b->type = BT_QUOTE; return 2; }
    if (n >= 1 && s[0] == '>') { b->type = BT_QUOTE; return 1; }

    const char *const boxes[] = { "- [ ] ", "- [x] ", "- [X] ", "* [ ] ", "* [x] ", "[ ] ", "[x] ", "[X] " };
    for (int k = 0; k < 8; k++) {
        if (starts(s, n, boxes[k])) {
            int l = (int)strlen(boxes[k]);
            b->type = BT_CHECK;
            b->checked = boxes[k][l - 3] != ' ';
            int p = 0;
            while (l + p < n && s[l + p] == '!' && p < 3) p++;
            if (p && l + p < n && s[l + p] == ' ') { b->style = (uint8_t)p; l += p + 1; }
            return l;
        }
    }
    for (int k = 0; k < NT_BS_COUNT; k++) {
        char m[8];
        snprintf(m, sizeof m, "%s ", BULLET_MARK[k]);
        if (starts(s, n, m)) { b->type = BT_BULLET; b->style = (uint8_t)k; return (int)strlen(m); }
    }
    for (size_t k = 0; k < sizeof ALT / sizeof ALT[0]; k++) {
        if (starts(s, n, ALT[k][0])) {
            b->type = BT_BULLET;
            b->style = (uint8_t)ALT[k][1][0];
            return (int)strlen(ALT[k][0]);
        }
    }
    /* 12. / 12) */
    int k = 0;
    while (k < n && k < 6 && isdigit((unsigned char)s[k])) k++;
    if (k > 0 && k + 1 < n && (s[k] == '.' || s[k] == ')') && s[k + 1] == ' ') {
        b->type = BT_NUM;
        b->style = NT_NS_DEC;
        return k + 2;
    }
    /* iv. / IV. (or a letter and a dot) */
    k = 0;
    while (k < n && k < 8 && strchr("ivxlcdmIVXLCDM", s[k])) k++;
    if (k > 0 && k + 1 < n && s[k] == '.' && s[k + 1] == ' ') {
        bool up = isupper((unsigned char)s[0]);
        /* a lone c. after a b. is a letter */
        bool letters = k == 1 && prev_type == BT_NUM &&
                       (prev_style == NT_NS_ALPHA || prev_style == NT_NS_ALPHA_UP);
        b->type = BT_NUM;
        b->style = letters ? (up ? NT_NS_ALPHA_UP : NT_NS_ALPHA) : (up ? NT_NS_ROMAN_UP : NT_NS_ROMAN);
        return k + 2;
    }
    /* b) / B) / b. */
    k = 0;
    while (k < n && k < 3 && isalpha((unsigned char)s[k])) k++;
    if (k > 0 && k + 1 < n && (s[k] == ')' || (s[k] == '.' && k == 1)) && s[k + 1] == ' ') {
        b->type = BT_NUM;
        b->style = isupper((unsigned char)s[0]) ? NT_NS_ALPHA_UP : NT_NS_ALPHA;
        return k + 2;
    }
    return 0;
}

static uint32_t parse_date(const char *v)
{
    int y, mo, d, h = 0, mi = 0;
    if (sscanf(v, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) < 3) return (uint32_t)strtol(v, NULL, 10);
    struct tm tm = { 0 };
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return t > 0 ? (uint32_t)t : 0;
}

static void unquote(char *v)
{
    size_t n = strlen(v);
    if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) {
        char q = v[0];
        size_t o = 0;
        for (size_t i = 1; i + 1 < n; i++) {
            if (q == '"' && v[i] == '\\' && i + 2 < n) i++;
            v[o++] = v[i];
        }
        v[o] = 0;
    }
}

static const char *front_matter(const char *src, nt_meta_t *m, char *title, size_t tlen, bool *has_title)
{
    if (strncmp(src, "---", 3) != 0) return src;
    const char *p = src + 3;
    while (*p == ' ') p++;
    if (*p == '\r') p++;
    if (*p != '\n') return src;
    p++;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l && p[l - 1] == '\r') l--;
        if (l >= 3 && strncmp(p, "---", 3) == 0) return e ? e + 1 : p + strlen(p);
        char line[256];
        if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l);
        line[l] = 0;
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = 0;
            char *v = colon + 1;
            while (*v == ' ') v++;
            char *ve = v + strlen(v);
            while (ve > v && ve[-1] == ' ') *--ve = 0;
            unquote(v);
            if (!strcmp(line, "title")) { snprintf(title, tlen, "%s", v); *has_title = true; }
            else if (!strcmp(line, "type")) m->kind = !strcmp(v, "tasks") ? NT_KIND_TASKS : NT_KIND_NOTE;
            else if (!strcmp(line, "color")) m->tag = (uint8_t)nt_tag_from_name(v);
            else if (!strcmp(line, "pinned")) m->pinned = !strcmp(v, "true") || !strcmp(v, "yes");
            else if (!strcmp(line, "hide_done")) m->hide_done = !strcmp(v, "true");
            else if (!strcmp(line, "created")) m->created = parse_date(v);
            else if (!strcmp(line, "modified")) m->modified = parse_date(v);
        }
        if (!e) break;
        p = e + 1;
    }
    return src;     /* never closed: it was not front matter */
}

void nt_doc_from_md(nt_doc_t *d, nt_meta_t *m, const char *src, const char *fallback_title)
{
    nt_doc_clear(d);
    nt_doc_init(d);
    memset(m, 0, sizeof *m);
    char title[256] = "";
    bool has_title = false;
    const char *p = front_matter(src ? src : "", m, title, sizeof title, &has_title);
    if (!has_title) snprintf(title, sizeof title, "%s", fallback_title ? fallback_title : "");

    int prev_type = BT_PARA, prev_style = 0;
    while (*p) {
        const char *e = strchr(p, '\n');
        int l = e ? (int)(e - p) : (int)strlen(p);
        int len = l;
        if (len && p[len - 1] == '\r') len--;

        int sp = 0, k = 0;
        while (k < len && (p[k] == ' ' || p[k] == '\t')) { sp += p[k] == '\t' ? 2 : 1; k++; }
        const char *s = p + k;
        int n = len - k;
        nt_blk_t *b = nt_doc_insert(d, d->n, BT_PARA);
        if (!b) break;
        b->indent = (uint8_t)(sp / 2 > NT_MAX_INDENT ? NT_MAX_INDENT : sp / 2);
        int trimmed = n;
        while (trimmed > 0 && s[trimmed - 1] == ' ') trimmed--;
        if (n > 0 && s[0] == '\\') {
            s++;
            n--;
        } else if (trimmed == 3 && (starts(s, n, "---") || starts(s, n, "***") || starts(s, n, "___"))) {
            b->type = BT_RULE;
            b->indent = 0;
            n = 0;
        } else {
            int ml = marker(s, n, b, prev_type, prev_style);
            s += ml;
            n -= ml;
            if (b->type == BT_BULLET && n > 1 && s[0] == '\\' && s[1] == '[') { s++; n--; }
            if (b->type == BT_CHECK && n > 1 && s[0] == '\\' && s[1] == '!') { s++; n--; }
        }
        if (n > 0) parse_inline(b, s, n);
        prev_type = b->type;
        prev_style = b->style;
        if (!e) break;
        p = e + 1;
    }

    /* A hand-written note's first heading is its title, when it has none. */
    if (!has_title && d->n > 1 && d->b[1].type == BT_H1) {
        snprintf(title, sizeof title, "%s", d->b[1].txt);
        nt_doc_remove(d, 1);
    }
    nt_blk_set(&d->b[0], title, 0);
    if (d->n == 1) nt_doc_insert(d, 1, m->kind == NT_KIND_TASKS ? BT_CHECK : BT_PARA);
    nt_doc_number(d);
}

/* -------------------------------------------------------------------------- */
/* Plain text                                                                  */
/* -------------------------------------------------------------------------- */

void nt_doc_plain(const nt_doc_t *d, nt_sb_t *o, bool with_title)
{
    if (with_title && d->n && d->b[0].len) {
        nt_sb_put(o, d->b[0].txt, d->b[0].len);
        nt_sb_putc(o, '\n');
    }
    for (int i = 1; i < d->n; i++) {
        const nt_blk_t *b = &d->b[i];
        for (int k = 0; k < b->indent; k++) nt_sb_put(o, "  ", 2);
        char lab[24];
        switch (b->type) {
        case BT_RULE:   nt_sb_put(o, "----------", -1); break;
        case BT_QUOTE:  nt_sb_put(o, "> ", -1); break;
        case BT_BULLET: nt_sb_put(o, b->style == NT_BS_DISC ? "\xE2\x80\xA2 " : BULLET_MARK[b->style % NT_BS_COUNT], -1);
                        if (b->style != NT_BS_DISC) nt_sb_putc(o, ' ');
                        break;
        case BT_NUM:    nt_num_label(b->style, b->num, lab, sizeof lab); nt_sb_put(o, lab, -1); nt_sb_putc(o, ' '); break;
        case BT_CHECK:  nt_sb_put(o, b->checked ? "[x] " : "[ ] ", -1); break;
        default: break;
        }
        nt_sb_put(o, b->txt, b->len);
        nt_sb_putc(o, '\n');
    }
}

int nt_doc_words(const nt_doc_t *d, int *chars)
{
    int words = 0, ch = 0;
    for (int i = 0; i < d->n; i++) {
        const nt_blk_t *b = &d->b[i];
        bool in = false;
        for (int k = 0; k < b->len; k++) {
            unsigned char c = (unsigned char)b->txt[k];
            if ((c & 0xC0) != 0x80) ch++;
            bool sp = c == ' ' || c == '\t';
            if (!sp && !in) words++;
            in = !sp;
        }
    }
    if (chars) *chars = ch;
    return words;
}

/* -------------------------------------------------------------------------- */
/* Fragments: the undo history and the clipboard                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t   type, style, indent, checked;
    int       len;
    char     *txt;
    uint16_t *at;
} fblk_t;

struct nt_frag {
    int     n;
    fblk_t *b;
    size_t  bytes;
};

static bool frag_add(nt_frag_t *f, const nt_blk_t *b, int p0, int p1)
{
    fblk_t *q = &f->b[f->n];
    q->type = b->type;
    q->style = b->style;
    q->indent = b->indent;
    q->checked = b->checked;
    q->len = p1 - p0;
    q->txt = nt_alloc(q->len + 1);
    q->at = nt_alloc((q->len + 1) * sizeof(uint16_t));
    if (!q->txt || !q->at) { nt_free(q->txt); nt_free(q->at); return false; }
    memcpy(q->txt, b->txt + p0, q->len);
    memcpy(q->at, b->at + p0, q->len * sizeof(uint16_t));
    f->bytes += sizeof(fblk_t) + q->len * 3 + 2;
    f->n++;
    return true;
}

nt_frag_t *nt_frag_copy(const nt_doc_t *d, int b0, int p0, int b1, int p1)
{
    if (b0 > b1 || (b0 == b1 && p0 > p1)) {
        int t = b0; b0 = b1; b1 = t;
        t = p0; p0 = p1; p1 = t;
    }
    nt_frag_t *f = nt_alloc(sizeof *f);
    if (!f) return NULL;
    f->b = nt_alloc(sizeof(fblk_t) * (b1 - b0 + 1));
    if (!f->b) { nt_free(f); return NULL; }
    for (int i = b0; i <= b1; i++) {
        const nt_blk_t *b = &d->b[i];
        int s = i == b0 ? p0 : 0;
        int e = i == b1 ? p1 : b->len;
        if (!frag_add(f, b, s, e)) break;
    }
    return f;
}

nt_frag_t *nt_frag_whole(const nt_doc_t *d)
{
    return nt_frag_copy(d, 0, 0, d->n - 1, d->b[d->n - 1].len);
}

void nt_frag_free(nt_frag_t *f)
{
    if (!f) return;
    for (int i = 0; i < f->n; i++) { nt_free(f->b[i].txt); nt_free(f->b[i].at); }
    nt_free(f->b);
    nt_free(f);
}

size_t nt_frag_size(const nt_frag_t *f)
{
    return f ? f->bytes : 0;
}

int nt_frag_blocks(const nt_frag_t *f)
{
    return f ? f->n : 0;
}

const char *nt_frag_text(const nt_frag_t *f, int i, int *len)
{
    if (!f || i < 0 || i >= f->n) { *len = 0; return ""; }
    *len = f->b[i].len;
    return f->b[i].txt;
}

int32_t nt_text_width(const char *s, const lv_font_t *f)
{
    int32_t w = 0;
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        int l = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        uint32_t cp = l == 1 ? c : l == 2 ? (c & 0x1F) : l == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < l && i + k < n; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += l;
        uint32_t nx = 0;
        if (i < n) {
            unsigned char d = (unsigned char)s[i];
            int m = d < 0x80 ? 1 : (d >> 5) == 6 ? 2 : (d >> 4) == 14 ? 3 : 4;
            nx = m == 1 ? d : m == 2 ? (d & 0x1F) : m == 3 ? (d & 0x0F) : (d & 0x07);
            for (int k = 1; k < m && i + k < n; k++) nx = (nx << 6) | ((unsigned char)s[i + k] & 0x3F);
        }
        w += lv_font_get_glyph_width(f, cp, nx);
    }
    return w;
}

static void blk_from(nt_blk_t *b, const fblk_t *q)
{
    b->type = q->type;
    b->style = q->style;
    b->indent = q->indent;
    b->checked = q->checked;
}

static void blk_append(nt_blk_t *b, const fblk_t *q, int at)
{
    if (q->len <= 0) return;
    blk_reserve(b, b->len + q->len);
    if (b->cap < b->len + q->len + 1) return;
    memmove(b->txt + at + q->len, b->txt + at, b->len - at);
    memmove(b->at + at + q->len, b->at + at, (b->len - at) * sizeof(uint16_t));
    memcpy(b->txt + at, q->txt, q->len);
    memcpy(b->at + at, q->at, q->len * sizeof(uint16_t));
    b->len += q->len;
    b->txt[b->len] = 0;
}

void nt_frag_restore(nt_doc_t *d, const nt_frag_t *f)
{
    nt_doc_clear(d);
    for (int i = 0; i < f->n; i++) {
        nt_blk_t *b = nt_doc_insert(d, d->n, f->b[i].type);
        if (!b) break;
        blk_from(b, &f->b[i]);
        blk_append(b, &f->b[i], 0);
    }
    if (d->n == 0) nt_doc_init(d);
    if (d->b[0].type != BT_TITLE) nt_doc_insert(d, 0, BT_TITLE);
    if (d->n == 1) nt_doc_insert(d, 1, BT_PARA);
    nt_doc_number(d);
}

void nt_frag_paste(nt_doc_t *d, const nt_frag_t *f, int *pb, int *pp)
{
    if (!f || f->n == 0) return;
    int bi = *pb, pos = *pp;
    nt_blk_t *b = &d->b[bi];
    if (f->n == 1) {
        blk_append(b, &f->b[0], pos);
        *pp = pos + f->b[0].len;
        return;
    }
    /* Split the block at the caret: the tail waits for the last piece. */
    int tail_len = b->len - pos;
    char *tail = nt_alloc(tail_len + 1);
    uint16_t *tail_at = nt_alloc((tail_len + 1) * sizeof(uint16_t));
    if (!tail || !tail_at) { nt_free(tail); nt_free(tail_at); return; }
    memcpy(tail, b->txt + pos, tail_len);
    memcpy(tail_at, b->at + pos, tail_len * sizeof(uint16_t));
    nt_blk_delete(b, pos, tail_len);

    /* The first piece joins the block; an empty block takes its style. */
    if (b->len == 0 && bi > 0 && f->b[0].type != BT_TITLE) blk_from(b, &f->b[0]);
    blk_append(b, &f->b[0], b->len);

    int at = bi;
    for (int i = 1; i < f->n; i++) {
        nt_blk_t *nb = nt_doc_insert(d, at + 1, BT_PARA);
        if (!nb) break;
        at++;
        blk_from(nb, &f->b[i]);
        if (nb->type == BT_TITLE) nb->type = BT_PARA;
        blk_append(nb, &f->b[i], 0);
    }
    nt_blk_t *last = &d->b[at];
    *pb = at;
    *pp = last->len;
    fblk_t t = { 0 };
    t.len = tail_len;
    t.txt = tail;
    t.at = tail_at;
    blk_append(last, &t, last->len);
    nt_free(tail);
    nt_free(tail_at);
    nt_doc_number(d);
}
