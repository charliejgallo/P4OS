/*
 * P4OS - Office: text formats shown as documents (phase 3).
 *
 * Markdown rendered (CommonMark's usual parts: headings, emphasis, code,
 * links, pictures, lists, task lists, quotes, rules, fenced and indented
 * code, pipe tables); contacts (vCard 2.1 to 4.0) and calendars
 * (iCalendar: events and tasks, in date order) as readable pages; and the
 * tree of a JSON, XML, YAML or TOML file: one line per value, indented by
 * depth, keys in bold, values coloured by kind.
 */
#include "of_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define READ_MAX (16u * 1024 * 1024)
#define TREE_MAX 5000           /* lines of a tree */

extern const char *(*of_tr)(const char *es);
#define _(s) (of_tr ? of_tr(s) : (s))
#define N_(s) (s)               /* marked for the catalog, translated where shown */

#define C_GREY   0x1808080u
#define C_CODE   0x1307070u
#define C_STRING 0x1208020u
#define C_NUMBER 0x11060C0u
#define C_WORD   0x19040A0u
#define C_KEY    0x1A05010u

static char *read_text(const char *path, size_t *n, char *err, size_t errlen)
{
    char *s = (char *)of_file_read(path, n, READ_MAX);
    if (!s) { snprintf(err, errlen, "read"); return NULL; }
    size_t skip = *n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3) ? 3 : 0;
    if (skip) { memmove(s, s + 3, *n - 3); *n -= 3; s[*n] = 0; }
    if (!of_utf8_valid(s, *n)) {
        size_t m;
        char *u = of_latin1_to_utf8(s, *n, &m);
        if (u) { of_free(s); s = u; *n = m; }
    }
    return s;
}

/* strcasestr is not among the symbols the system gives apps */
static bool has_ci(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++)
        if (!strncasecmp(h, n, l)) return true;
    return false;
}

static void say(of_bld_t *b, const char *s) { of_bld_text(b, s, strlen(s)); }

/* -------------------------------------------------------------------------- */
/* Markdown                                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    of_bld_t *b;
    char      dir[300];         /* the file's folder, for pictures */
} md_t;

static bool md_closes(const char *s, size_t n, size_t from, const char *del, size_t dl)
{
    for (size_t k = from; k + dl <= n; k++) {
        if (s[k] == '\\') { k++; continue; }
        if (!memcmp(s + k, del, dl) && k > from && s[k - 1] != ' ') return true;
    }
    return false;
}

static void md_image(md_t *M, const char *src, size_t n, const char *alt, size_t an)
{
    char ref[300], path[400];
    snprintf(ref, sizeof ref, "%.*s", (int)n, src);
    char *sp = strchr(ref, ' ');
    if (sp) *sp = 0;                    /* ![a](pic.png "title") */
    if (!strncmp(ref, "http", 4) || !ref[0]) {
        of_rstyle_t keep = M->b->st;
        M->b->st.color = C_GREY;
        of_bld_text(M->b, "[", 1);
        of_bld_text(M->b, alt, an);
        of_bld_text(M->b, "]", 1);
        M->b->st = keep;
        return;
    }
    if (ref[0] == '/') snprintf(path, sizeof path, "%s", ref);
    else snprintf(path, sizeof path, "%.299s%.99s", M->dir, ref);
    size_t len;
    uint8_t *bin = of_file_read(path, &len, 16u * 1024 * 1024);
    if (!bin) return;
    int w = 0, h = 0;
    if (len > 24 && !memcmp(bin, "\x89PNG", 4)) {
        w = bin[16] << 24 | bin[17] << 16 | bin[18] << 8 | bin[19];
        h = bin[20] << 24 | bin[21] << 16 | bin[22] << 8 | bin[23];
    } else if (len > 4 && bin[0] == 0xFF && bin[1] == 0xD8) {
        for (size_t i = 2; i + 9 < len;) {
            if (bin[i] != 0xFF) { i++; continue; }
            uint8_t m = bin[i + 1];
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) { h = bin[i + 5] << 8 | bin[i + 6]; w = bin[i + 7] << 8 | bin[i + 8]; break; }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            i += 2 + (size_t)(bin[i + 2] << 8 | bin[i + 3]);
        }
    }
    of_free(bin);
    int wp = w * 3 / 4, hp = h * 3 / 4;
    if (wp > 500) { hp = hp * 500 / wp; wp = 500; }
    of_bld_image(M->b, path, true, wp, hp);
}

/* the inline parts of a block's text */
static void md_inline(md_t *M, const char *s, size_t n)
{
    of_bld_t *b = M->b;
    of_rstyle_t base = b->st;
    bool bold = false, ital = false, strike = false;
    size_t run = 0;             /* start of plain text waiting */
#define FLUSH(to) do { if ((to) > run) of_bld_text(b, s + run, (to) - run); } while (0)
#define APPLY() do { b->st = base; if (bold) b->st.b = OF_TRI_ON; if (ital) b->st.i = OF_TRI_ON; if (strike) b->st.s = OF_TRI_ON; } while (0)
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < n && strchr("\\`*_{}[]()#+-.!|~<>", s[i + 1])) {
            FLUSH(i);
            run = i + 1;
            i++;
            continue;
        }
        if (c == '`') {
            size_t ticks = 1;
            while (i + ticks < n && s[i + ticks] == '`') ticks++;
            size_t k = i + ticks;
            while (k + ticks <= n) {
                if (!strncmp(s + k, s + i, ticks) && (k + ticks == n || s[k + ticks] != '`')) break;
                k++;
            }
            if (k + ticks <= n) {
                FLUSH(i);
                of_rstyle_t keep = b->st;
                b->st.color = C_CODE;
                size_t a = i + ticks, z = k;
                if (z > a + 1 && s[a] == ' ' && s[z - 1] == ' ') { a++; z--; }
                of_bld_text(b, s + a, z - a);
                b->st = keep;
                i = k + ticks - 1;
                run = i + 1;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c) {
            bool can = bold || md_closes(s, n, i + 2, c == '*' ? "**" : "__", 2);
            if (can && (c == '*' || i == 0 || s[i - 1] == ' ' || bold)) {
                FLUSH(i);
                bold = !bold;
                APPLY();
                i++;
                run = i + 1;
                continue;
            }
        }
        if (c == '*' || c == '_') {
            bool word_in = c == '_' && i > 0 && s[i - 1] != ' ' && !ital;
            bool can = ital || md_closes(s, n, i + 1, c == '*' ? "*" : "_", 1);
            if (can && !word_in && (ital || (i + 1 < n && s[i + 1] != ' '))) {
                FLUSH(i);
                ital = !ital;
                APPLY();
                run = i + 1;
                continue;
            }
        }
        if (c == '~' && i + 1 < n && s[i + 1] == '~' && (strike || md_closes(s, n, i + 2, "~~", 2))) {
            FLUSH(i);
            strike = !strike;
            APPLY();
            i++;
            run = i + 1;
            continue;
        }
        if ((c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) ) {
            size_t a = i + (c == '!' ? 2 : 1), k = a;
            int depth = 1;
            while (k < n && depth) { if (s[k] == '[') depth++; else if (s[k] == ']') depth--; if (depth) k++; }
            if (k < n && k + 1 < n && s[k + 1] == '(') {
                size_t u = k + 2, z = u;
                while (z < n && s[z] != ')') z++;
                if (z < n) {
                    FLUSH(i);
                    if (c == '!') {
                        md_image(M, s + u, z - u, s + a, k - a);
                    } else {
                        char url[400];
                        snprintf(url, sizeof url, "%.*s", (int)(z - u), s + u);
                        char *sp = strchr(url, ' ');
                        if (sp) *sp = 0;
                        of_bld_link(b, url);
                        md_inline(M, s + a, k - a);
                        of_bld_link(b, NULL);
                        APPLY();
                    }
                    i = z;
                    run = z + 1;
                    continue;
                }
            }
        }
        if (c == '<') {
            size_t z = i + 1;
            while (z < n && s[z] != '>' && s[z] != ' ') z++;
            if (z < n && s[z] == '>') {
                if (!strncmp(s + i + 1, "http", 4) || !strncmp(s + i + 1, "mailto:", 7)) {
                    FLUSH(i);
                    char url[400];
                    snprintf(url, sizeof url, "%.*s", (int)(z - i - 1), s + i + 1);
                    of_bld_link(b, url);
                    of_bld_text(b, url, strlen(url));
                    of_bld_link(b, NULL);
                    APPLY();
                    i = z;
                    run = z + 1;
                    continue;
                }
                if (!strncasecmp(s + i, "<br", 3)) {
                    FLUSH(i);
                    if (b->cur >= 0) of_para_append(b->d, &b->d->p[b->cur], "\n", 1, (uint16_t)of_doc_style(b->d, &b->st));
                    b->space = false;
                    i = z;
                    run = z + 1;
                    continue;
                }
                /* other inline HTML tags go */
                if ((s[i + 1] >= 'a' && s[i + 1] <= 'z') || s[i + 1] == '/') {
                    FLUSH(i);
                    i = z;
                    run = z + 1;
                    continue;
                }
            }
        }
        if (c == '&') {
            static const struct { const char *e; const char *u; } E[] = {
                { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&nbsp;", " " }, { "&copy;", "\xC2\xA9" },
                { "&mdash;", "\xE2\x80\x94" }, { "&ndash;", "\xE2\x80\x93" }, { "&hellip;", "\xE2\x80\xA6" }, { "&euro;", "\xE2\x82\xAC" },
            };
            for (size_t e = 0; e < sizeof E / sizeof E[0]; e++) {
                size_t el = strlen(E[e].e);
                if (i + el <= n && !memcmp(s + i, E[e].e, el)) {
                    FLUSH(i);
                    of_bld_text(b, E[e].u, strlen(E[e].u));
                    i += el - 1;
                    run = i + 1;
                    break;
                }
            }
        }
    }
    FLUSH(n);
    b->st = base;
#undef FLUSH
#undef APPLY
}

static int lead_spaces(const char *s, size_t n)
{
    int k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ' ') k++;
        else if (s[i] == '\t') k += 4;
        else break;
    }
    return k;
}

static bool is_rule(const char *s, size_t n)
{
    char c = 0;
    int cnt = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ' ' || s[i] == '\t') continue;
        if (s[i] != '-' && s[i] != '*' && s[i] != '_') return false;
        if (c && s[i] != c) return false;
        c = s[i];
        cnt++;
    }
    return cnt >= 3;
}

/* "1. " "1) " "- " "* " "+ ": the marker's length, 0 none */
static size_t list_marker(const char *s, size_t n, bool *ordered)
{
    if (n >= 2 && (s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ') { *ordered = false; return 2; }
    size_t k = 0;
    while (k < n && k < 9 && s[k] >= '0' && s[k] <= '9') k++;
    if (k && k + 1 < n && (s[k] == '.' || s[k] == ')') && s[k + 1] == ' ') { *ordered = true; return k + 2; }
    return 0;
}

static int split_cells(const char *s, size_t n, const char **cs, size_t *cl, int max)
{
    size_t a = 0;
    while (a < n && (s[a] == ' ')) a++;
    if (a < n && s[a] == '|') a++;
    while (n > a && (s[n - 1] == ' ' || s[n - 1] == '\r')) n--;
    if (n > a && s[n - 1] == '|' && (n < 2 || s[n - 2] != '\\')) n--;
    int k = 0;
    size_t from = a;
    for (size_t i = a; i <= n && k < max; i++) {
        if (i < n && (s[i] != '|' || (i > 0 && s[i - 1] == '\\'))) continue;
        size_t x = from, y = i;
        while (x < y && s[x] == ' ') x++;
        while (y > x && s[y - 1] == ' ') y--;
        cs[k] = s + x;
        cl[k] = y - x;
        k++;
        from = i + 1;
    }
    return k;
}

static bool is_table_sep(const char *s, size_t n)
{
    bool dash = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '-') dash = true;
        else if (c != '|' && c != ':' && c != ' ' && c != '\r') return false;
    }
    return dash && memchr(s, '|', n);
}

void of_md_into(of_bld_t *b, const char *s, size_t n, const char *dir)
{
    md_t M = { .b = b };
    snprintf(M.dir, sizeof M.dir, "%s", dir ? dir : "");
    /* the lines */
    size_t nl = 1;
    for (size_t i = 0; i < n; i++) if (s[i] == '\n') nl++;
    const char **L = of_alloc(nl * sizeof *L);
    size_t *LL = of_alloc(nl * sizeof *LL);
    if (!L || !LL) { of_free(L); of_free(LL); return; }
    size_t k = 0, from = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && s[i] != '\n') continue;
        size_t e = i;
        if (e > from && s[e - 1] == '\r') e--;
        L[k] = s + from;
        LL[k] = e - from;
        k++;
        from = i + 1;
    }
    nl = k;
    of_sb_t para = { 0 };       /* the paragraph's lines, joined */
    int numid = 3000;
    bool in_list = false;
    int lv_ind[9], nlv = 0;
#define END_PARA() do { if (para.n) { md_inline(&M, para.s, para.n); para.n = 0; } } while (0)
    for (size_t li = 0; li < nl; li++) {
        const char *l = L[li];
        size_t ln = LL[li];
        int ind = lead_spaces(l, ln);
        size_t a = 0;
        while (a < ln && (l[a] == ' ' || l[a] == '\t')) a++;
        const char *t = l + a;
        size_t tn = ln - a;
        /* blank line */
        if (!tn) {
            END_PARA();
            b->cur = -1;
            /* a blank line ends a list unless the next line goes on with it */
            if (in_list) {
                size_t nx = li + 1;
                while (nx < nl && !LL[nx]) nx++;
                bool ord;
                if (nx >= nl || (lead_spaces(L[nx], LL[nx]) < 2 &&
                    !list_marker(L[nx] + strspn(L[nx], " \t"), LL[nx] - strspn(L[nx], " \t"), &ord))) {
                    in_list = false;
                    b->list = 0;
                    b->ilvl = 0;
                    numid++;
                }
            }
            continue;
        }
        /* fenced code */
        if (tn >= 3 && (!strncmp(t, "```", 3) || !strncmp(t, "~~~", 3))) {
            END_PARA();
            char fence[4] = { t[0], t[0], t[0], 0 };
            b->list = 0;
            b->heading = 0;
            b->indent = 1;
            b->cur = -1;
            of_bld_para(b);
            b->pre = true;
            of_rstyle_t keep = b->st;
            b->st.color = C_CODE;
            bool first = true;
            for (li++; li < nl; li++) {
                const char *c = L[li] + strspn(L[li], " ");
                if (!strncmp(c, fence, 3)) break;
                if (!first) of_bld_text(b, "\n", 1);
                of_bld_text(b, L[li], LL[li]);
                first = false;
            }
            b->st = keep;
            b->pre = false;
            b->indent = 0;
            b->cur = -1;
            continue;
        }
        /* indented code (not inside a list) */
        if (ind >= 4 && !in_list && !para.n) {
            b->heading = 0;
            b->indent = 1;
            b->cur = -1;
            of_bld_para(b);
            b->pre = true;
            of_rstyle_t keep = b->st;
            b->st.color = C_CODE;
            bool first = true;
            for (; li < nl; li++) {
                if (LL[li] && lead_spaces(L[li], LL[li]) < 4) { li--; break; }
                if (!first) of_bld_text(b, "\n", 1);
                size_t cut = 0, sp = 0;
                while (cut < LL[li] && sp < 4) { sp += L[li][cut] == '\t' ? 4 : 1; cut++; }
                of_bld_text(b, L[li] + cut, LL[li] - cut);
                first = false;
            }
            b->st = keep;
            b->pre = false;
            b->indent = 0;
            b->cur = -1;
            continue;
        }
        /* headings */
        if (t[0] == '#') {
            int h = 0;
            while (h < (int)tn && t[h] == '#') h++;
            if (h <= 6 && (h == (int)tn || t[h] == ' ')) {
                END_PARA();
                size_t z = tn;
                while (z > (size_t)h && (t[z - 1] == '#' || t[z - 1] == ' ')) z--;
                b->list = 0; b->ilvl = 0; in_list = false;
                b->heading = h;
                b->cur = -1;
                of_bld_para(b);
                if (z > (size_t)h) md_inline(&M, t + h + 1, z - h - 1);
                b->heading = 0;
                b->cur = -1;
                continue;
            }
        }
        /* setext headings: the paragraph's line underlined */
        if (para.n && !in_list && ((t[0] == '=' && strspn(t, "=") == tn) || (t[0] == '-' && strspn(t, "- ") == tn))) {
            if (b->cur >= 0) b->d->p[b->cur].heading = (uint8_t)(t[0] == '=' ? 1 : 2);
            END_PARA();
            b->heading = 0;
            b->cur = -1;
            continue;
        }
        if (is_rule(t, tn)) {
            END_PARA();
            in_list = false;
            b->list = 0;
            of_bld_break(b);
            b->cur = -1;
            continue;
        }
        /* tables: a header row and a separator */
        if (memchr(t, '|', tn) && li + 1 < nl && is_table_sep(L[li + 1], LL[li + 1])) {
            END_PARA();
            b->list = 0;
            in_list = false;
            of_bld_table(b);
            const char *cs[64];
            size_t cl[64];
            for (size_t r = li; r < nl; r++) {
                if (r == li + 1) continue;
                if (!LL[r] || !memchr(L[r], '|', LL[r])) { li = r - 1; break; }
                int nc = split_cells(L[r], LL[r], cs, cl, 64);
                of_bld_row(b);
                for (int c = 0; c < nc; c++) {
                    of_bld_cell(b, 1);
                    of_rstyle_t keep = b->st;
                    if (r == li) b->st.b = OF_TRI_ON;
                    md_inline(&M, cs[c], cl[c]);
                    b->st = keep;
                }
                if (r == nl - 1) li = r;
            }
            of_bld_table_end(b);
            b->cur = -1;
            continue;
        }
        /* quotes */
        if (t[0] == '>') {
            END_PARA();
            int depth = 0;
            size_t q = 0;
            while (q < tn && (t[q] == '>' || t[q] == ' ')) { if (t[q] == '>') depth++; q++; }
            if (b->indent != depth || b->cur < 0) {
                b->indent = depth;
                b->cur = -1;
                of_bld_para(b);
            }
            of_rstyle_t keep = b->st;
            b->st.color = C_GREY;
            b->st.i = OF_TRI_ON;
            if (q < tn) md_inline(&M, t + q, tn - q);
            else b->cur = -1;
            b->st = keep;
            b->space = true;
            /* the quote goes on in the next line, or ends */
            if (li + 1 >= nl || !LL[li + 1] || L[li + 1][strspn(L[li + 1], " ")] != '>') { b->indent = 0; b->cur = -1; }
            continue;
        }
        /* list items */
        bool ord;
        size_t mk = list_marker(t, tn, &ord);
        if (mk) {
            END_PARA();
            if (!in_list) { numid++; nlv = 0; }
            in_list = true;
            /* the level from the indents seen in this list: deeper pushes one, shallower pops */
            while (nlv && ind < lv_ind[nlv - 1]) nlv--;
            if (!nlv || ind > lv_ind[nlv - 1]) { if (nlv < 9) lv_ind[nlv++] = ind; }
            b->list = ord ? OF_LIST_NUMBER : OF_LIST_BULLET;
            b->ilvl = nlv - 1;
            b->numid = numid * 16 + (ord ? 1 : 0);
            b->cur = -1;
            of_bld_para(b);
            const char *it = t + mk;
            size_t itn = tn - mk;
            /* task lists */
            if (itn >= 3 && it[0] == '[' && it[2] == ']' && (it[1] == ' ' || it[1] == 'x' || it[1] == 'X')) {
                say(b, it[1] == ' ' ? "\xE2\x96\xA1 " : "\xE2\x96\xA0 ");
                it += 3;
                itn -= 3;
            }
            of_sb_put(&para, it, itn);
            continue;
        }
        /* a paragraph's line; inside a list item, its continuation */
        if (!para.n && b->cur < 0) {
            if (in_list && ind < 2) { in_list = false; b->list = 0; numid++; }
            if (!in_list) { b->list = 0; b->ilvl = 0; }
            else {
                /* a paragraph inside an item: no new bullet */
                b->list = 0;
                b->indent = b->ilvl + 1;
            }
            of_bld_para(b);
            b->indent = 0;
        }
        if (para.n) {
            /* a hard break: two spaces or a backslash at the end of the line before */
            if (para.n >= 2 && para.s[para.n - 1] == ' ' && para.s[para.n - 2] == ' ') {
                END_PARA();
                if (b->cur >= 0) of_para_append(b->d, &b->d->p[b->cur], "\n", 1, (uint16_t)of_doc_style(b->d, &b->st));
                b->space = false;
            } else if (para.s[para.n - 1] == '\\') {
                para.n--;
                END_PARA();
                if (b->cur >= 0) of_para_append(b->d, &b->d->p[b->cur], "\n", 1, (uint16_t)of_doc_style(b->d, &b->st));
                b->space = false;
            } else {
                of_sb_putc(&para, ' ');
            }
        }
        of_sb_put(&para, t, tn);
    }
    END_PARA();
#undef END_PARA
    of_sb_free(&para);
    of_free(L);
    of_free(LL);
    b->list = 0;
    b->indent = 0;
}

static void dir_of(const char *path, char *out, size_t len)
{
    snprintf(out, len, "%s", path);
    char *sl = strrchr(out, '/');
    if (sl) sl[1] = 0;
    else out[0] = 0;
}

of_doc_t *of_md_load(const char *path, char *err, size_t errlen)
{
    size_t n;
    char *s = read_text(path, &n, err, errlen);
    if (!s) return NULL;
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    of_bld_t b;
    of_bld_init(&b, d);
    char dir[300];
    dir_of(path, dir, sizeof dir);
    of_md_into(&b, s, n, dir);
    of_bld_done(&b);
    snprintf(d->kind, sizeof d->kind, "Markdown");
    of_free(s);
    return d;
}

/* -------------------------------------------------------------------------- */
/* vCard and iCalendar: content lines                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    char name[32];
    char params[160];
    char *value;                /* unescaped, UTF-8 */
} cline_t;

/* the lines unfolded: a line starting with a space or tab continues the one before */
static int unfold(char *s, size_t n, char ***out)
{
    int cap = 64, k = 0;
    char **L = of_alloc((size_t)cap * sizeof *L);
    if (!L) return 0;
    size_t w = 0, start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r') continue;
        if (s[i] == '\n') {
            if (i + 1 < n && (s[i + 1] == ' ' || s[i + 1] == '\t')) { i++; continue; }
            /* vCard 2.1 quoted-printable soft breaks: "=" at the end */
            s[w++] = 0;
            if (k == cap) { cap *= 2; char **nl = of_realloc(L, (size_t)cap * sizeof *nl); if (!nl) break; L = nl; }
            L[k++] = s + start;
            start = w;
            continue;
        }
        s[w++] = s[i];
    }
    if (w > start && k < cap) { s[w] = 0; L[k++] = s + start; }
    *out = L;
    return k;
}

static void qp_inplace(char *v)
{
    char *o = v;
    for (char *p = v; *p; p++) {
        if (*p == '=' && p[1] && p[2]) {
            char hx[3] = { p[1], p[2], 0 };
            char *e;
            long x = strtol(hx, &e, 16);
            if (!*e) { *o++ = (char)x; p += 2; continue; }
        }
        *o++ = *p;
    }
    *o = 0;
}

static bool parse_line(char *l, cline_t *c)
{
    char *colon = NULL;
    bool q = false;
    for (char *p = l; *p; p++) {
        if (*p == '"') q = !q;
        if (*p == ':' && !q) { colon = p; break; }
    }
    if (!colon) return false;
    *colon = 0;
    char *semi = strchr(l, ';');
    if (semi) *semi = 0;
    char *nm = l;
    char *dot = strchr(nm, '.');            /* item1.TEL */
    if (dot) nm = dot + 1;
    snprintf(c->name, sizeof c->name, "%s", nm);
    for (char *p = c->name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    snprintf(c->params, sizeof c->params, "%s", semi ? semi + 1 : "");
    char *v = colon + 1;
    if (has_ci(c->params, "QUOTED-PRINTABLE")) qp_inplace(v);
    /* \n \, \; \\ */
    char *o = v;
    for (char *p = v; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *o++ = *p == 'n' || *p == 'N' ? '\n' : *p;
            continue;
        }
        *o++ = *p;
    }
    *o = 0;
    c->value = v;
    return true;
}

static void add_line(of_bld_t *b, const char *label, const char *value)
{
    if (!value || !value[0]) return;
    of_bld_para(b);
    of_rstyle_t keep = b->st;
    b->st.color = C_GREY;
    say(b, label);
    say(b, "  ");
    b->st = keep;
    /* a value of several lines keeps them */
    const char *p = value;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        of_bld_text(b, p, l);
        if (!nl) break;
        if (b->cur >= 0) of_para_append(b->d, &b->d->p[b->cur], "\n", 1, (uint16_t)of_doc_style(b->d, &b->st));
        b->space = false;
        p = nl + 1;
    }
}

static const char *type_word(const char *params)
{
    if (has_ci(params, "CELL") || has_ci(params, "MOBILE")) return _("Celular");
    if (has_ci(params, "WORK")) return _("Trabajo");
    if (has_ci(params, "HOME")) return _("Casa");
    if (has_ci(params, "FAX")) return _("Fax");
    return NULL;
}

of_doc_t *of_vcf_load(const char *path, char *err, size_t errlen)
{
    size_t n;
    char *s = read_text(path, &n, err, errlen);
    if (!s) return NULL;
    char **L = NULL;
    int nl = unfold(s, n, &L);
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    of_bld_t b;
    of_bld_init(&b, d);
    int count = 0, pics = 0;
    for (int i = 0; i < nl; i++) {
        if (strcasecmp(L[i], "BEGIN:VCARD")) continue;
        /* one card: the name first, then the rest in a fixed order */
        int j = i + 1;
        while (j < nl && strcasecmp(L[j], "END:VCARD")) j++;
        cline_t *C = of_alloc((size_t)(j - i) * sizeof *C);
        int nc = 0;
        for (int k = i + 1; k < j && C; k++)
            if (parse_line(L[k], &C[nc])) nc++;
        const char *fn = NULL, *nn = NULL;
        for (int k = 0; k < nc; k++) {
            if (!strcmp(C[k].name, "FN")) fn = C[k].value;
            if (!strcmp(C[k].name, "N")) nn = C[k].value;
        }
        char name[200] = "";
        if (fn && fn[0]) snprintf(name, sizeof name, "%s", fn);
        else if (nn) {
            /* Family;Given;Middle;Prefix;Suffix */
            char tmp[200];
            snprintf(tmp, sizeof tmp, "%s", nn);
            char *parts[5] = { 0 };
            int np = 0;
            char *p = tmp;
            while (np < 5) { parts[np++] = p; char *sc = strchr(p, ';'); if (!sc) break; *sc = 0; p = sc + 1; }
            snprintf(name, sizeof name, "%s%s%s", np > 1 ? parts[1] : "", np > 1 && parts[1][0] ? " " : "", parts[0]);
        }
        if (!name[0]) snprintf(name, sizeof name, "%s", _("(sin nombre)"));
        b.heading = 2;
        of_bld_para(&b);
        say(&b, name);
        b.heading = 0;
        static const struct { const char *prop, *label; } ORDER[] = {
            { "ORG", N_("Organización") }, { "TITLE", N_("Cargo") }, { "TEL", N_("Teléfono") }, { "EMAIL", N_("Correo") },
            { "ADR", N_("Dirección") }, { "URL", N_("Web") }, { "BDAY", N_("Cumpleaños") }, { "NICKNAME", N_("Apodo") }, { "NOTE", N_("Nota") },
        };
        for (size_t o = 0; o < sizeof ORDER / sizeof ORDER[0]; o++) {
            for (int k = 0; k < nc; k++) {
                if (strcmp(C[k].name, ORDER[o].prop)) continue;
                char v[600];
                snprintf(v, sizeof v, "%s", C[k].value);
                /* structured values: the parts joined */
                if (!strcmp(ORDER[o].prop, "ADR") || !strcmp(ORDER[o].prop, "ORG")) {
                    char out[600] = "";
                    size_t w = 0;
                    for (char *p = v; ; ) {
                        char *sc = strchr(p, ';');
                        if (sc) *sc = 0;
                        if (*p) w += (size_t)snprintf(out + w, sizeof out - w, "%s%s", w ? ", " : "", p);
                        if (!sc || w >= sizeof out - 1) break;
                        p = sc + 1;
                    }
                    snprintf(v, sizeof v, "%s", out);
                }
                if (!strcmp(ORDER[o].prop, "BDAY") && strlen(v) >= 8 && v[4] != '-') {
                    char t2[16];
                    snprintf(t2, sizeof t2, "%.2s/%.2s/%.4s", v + 6, v + 4, v);
                    snprintf(v, sizeof v, "%s", t2);
                } else if (!strcmp(ORDER[o].prop, "BDAY") && strlen(v) >= 10) {
                    char t2[16];
                    snprintf(t2, sizeof t2, "%.2s/%.2s/%.4s", v + 8, v + 5, v);
                    snprintf(v, sizeof v, "%s", t2);
                }
                if (!strncasecmp(v, "tel:", 4) || !strncasecmp(v, "mailto:", 7)) memmove(v, strchr(v, ':') + 1, strlen(strchr(v, ':') + 1) + 1);
                const char *tw = type_word(C[k].params);
                char label[64];
                snprintf(label, sizeof label, "%s%s%s%s", _(ORDER[o].label), tw ? " (" : "", tw ? tw : "", tw ? ")" : "");
                add_line(&b, label, v);
            }
        }
        /* the photo, if it comes inside */
        for (int k = 0; k < nc; k++) {
            if (strcmp(C[k].name, "PHOTO")) continue;
            const char *v = C[k].value;
            const char *b64 = strstr(v, "base64,");
            if (b64) v = b64 + 7;
            else if (!has_ci(C[k].params, "ENCODING=B") && !has_ci(C[k].params, "BASE64")) continue;
            size_t vl = strlen(v), bk = 0;
            uint8_t *bin = of_alloc(vl * 3 / 4 + 4);
            if (!bin) continue;
            uint32_t acc = 0;
            int bits = 0;
            for (size_t q = 0; q < vl; q++) {
                char ch = v[q];
                int x = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52
                      : ch == '+' ? 62 : ch == '/' ? 63 : -1;
                if (x < 0) continue;
                acc = acc << 6 | (uint32_t)x;
                bits += 6;
                if (bits >= 8) { bits -= 8; bin[bk++] = (uint8_t)(acc >> bits); }
            }
            char nm[40], pth[300];
            snprintf(nm, sizeof nm, "vcf%d.%s", ++pics, bk > 2 && bin[0] == 0x89 ? "png" : "jpg");
            if (of_cache_file(nm, bin, bk, pth, sizeof pth)) {
                of_bld_para(&b);
                of_bld_image(&b, pth, true, 72, 72);
            }
            of_free(bin);
            break;
        }
        of_free(C);
        count++;
        i = j;
    }
    of_free(L);
    of_free(s);
    if (!count) {
        of_bld_para(&b);
        say(&b, _("No hay contactos en este archivo."));
    }
    of_bld_done(&b);
    snprintf(d->kind, sizeof d->kind, "vCard");
    char note[64];
    snprintf(note, sizeof note, _("%d contactos"), count);
    d->note = of_strdup(note);
    return d;
}

/* "20261122T143000Z", "20261122": readable, and a key to sort by */
static void ical_date(const char *v, const char *params, char *out, size_t len, char *key, size_t klen)
{
    int Y = 0, M = 0, Dd = 0, h = -1, m = 0;
    size_t l = strlen(v);
    if (l >= 8) {
        sscanf(v, "%4d%2d%2d", &Y, &M, &Dd);
        if (l >= 15 && v[8] == 'T') sscanf(v + 9, "%2d%2d", &h, &m);
    }
    snprintf(key, klen, "%04d%02d%02d%02d%02d", Y, M, Dd, h < 0 ? 0 : h, m);
    if (!Y) { snprintf(out, len, "%s", v); return; }
    if (h < 0) snprintf(out, len, "%02d/%02d/%04d", Dd, M, Y);
    else snprintf(out, len, "%02d/%02d/%04d %02d:%02d%s", Dd, M, Y, h, m, v[l - 1] == 'Z' ? " UTC" : "");
    (void)params;
}

typedef struct {
    char key[16];
    int  first, last;           /* lines */
    bool todo;
} ev_t;

static int ev_cmp(const void *a, const void *b) { return strcmp(((const ev_t *)a)->key, ((const ev_t *)b)->key); }

of_doc_t *of_ics_load(const char *path, char *err, size_t errlen)
{
    size_t n;
    char *s = read_text(path, &n, err, errlen);
    if (!s) return NULL;
    char **L = NULL;
    int nl = unfold(s, n, &L);
    cline_t *C = of_alloc((size_t)(nl + 1) * sizeof *C);
    int nc = 0;
    for (int i = 0; i < nl && C; i++)
        if (parse_line(L[i], &C[nc])) nc++;
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    of_bld_t b;
    of_bld_init(&b, d);
    /* the calendar's name */
    for (int k = 0; k < nc; k++)
        if (!strcmp(C[k].name, "X-WR-CALNAME")) { b.heading = 1; of_bld_para(&b); say(&b, C[k].value); b.heading = 0; break; }
    ev_t *E = of_alloc((size_t)(nc + 1) * sizeof *E);
    int ne = 0;
    for (int k = 0; k < nc && E; k++) {
        if (strcmp(C[k].name, "BEGIN") || (strcasecmp(C[k].value, "VEVENT") && strcasecmp(C[k].value, "VTODO"))) continue;
        ev_t *e = &E[ne];
        memset(e, 0, sizeof *e);
        e->todo = !strcasecmp(C[k].value, "VTODO");
        e->first = k + 1;
        int z = k + 1;
        int depth = 0;
        for (; z < nc; z++) {
            if (!strcmp(C[z].name, "BEGIN")) depth++;
            if (!strcmp(C[z].name, "END")) { if (!depth) break; depth--; }
            char out[64];
            if ((!strcmp(C[z].name, "DTSTART") || (!e->key[0] && !strcmp(C[z].name, "DUE"))) && !depth)
                ical_date(C[z].value, C[z].params, out, sizeof out, e->key, sizeof e->key);
        }
        e->last = z;
        ne++;
        k = z;
    }
    qsort(E, (size_t)ne, sizeof *E, ev_cmp);
    char month_key[8] = "";
    for (int i = 0; i < ne; i++) {
        ev_t *e = &E[i];
        /* a heading per month */
        if (strncmp(month_key, e->key, 6) && e->key[0] != '0') {
            static const char *const MON[12] = { N_("Enero"), N_("Febrero"), N_("Marzo"), N_("Abril"), N_("Mayo"), N_("Junio"),
                                                 N_("Julio"), N_("Agosto"), N_("Septiembre"), N_("Octubre"), N_("Noviembre"),
                                                 N_("Diciembre") };
            snprintf(month_key, sizeof month_key, "%.6s", e->key);
            int mo = (e->key[4] - '0') * 10 + (e->key[5] - '0');
            char hd[48];
            snprintf(hd, sizeof hd, "%s %.4s", mo >= 1 && mo <= 12 ? _(MON[mo - 1]) : "", e->key);
            b.heading = 2;
            of_bld_para(&b);
            say(&b, hd);
            b.heading = 0;
        }
        const char *sum = NULL, *loc = NULL, *desc = NULL, *rr = NULL, *status = NULL;
        char start[64] = "", end[64] = "", due[64] = "", k2[16];
        int depth = 0;
        for (int k = e->first; k < e->last; k++) {
            if (!strcmp(C[k].name, "BEGIN")) depth++;
            if (!strcmp(C[k].name, "END")) depth--;
            if (depth) continue;
            if (!strcmp(C[k].name, "SUMMARY")) sum = C[k].value;
            else if (!strcmp(C[k].name, "LOCATION")) loc = C[k].value;
            else if (!strcmp(C[k].name, "DESCRIPTION")) desc = C[k].value;
            else if (!strcmp(C[k].name, "RRULE")) rr = C[k].value;
            else if (!strcmp(C[k].name, "STATUS")) status = C[k].value;
            else if (!strcmp(C[k].name, "DTSTART")) ical_date(C[k].value, C[k].params, start, sizeof start, k2, sizeof k2);
            else if (!strcmp(C[k].name, "DTEND")) ical_date(C[k].value, C[k].params, end, sizeof end, k2, sizeof k2);
            else if (!strcmp(C[k].name, "DUE")) ical_date(C[k].value, C[k].params, due, sizeof due, k2, sizeof k2);
        }
        b.heading = 3;
        of_bld_para(&b);
        if (e->todo) say(&b, status && !strcasecmp(status, "COMPLETED") ? "\xE2\x96\xA0 " : "\xE2\x96\xA1 ");
        say(&b, sum && sum[0] ? sum : _("(sin título)"));
        b.heading = 0;
        char when[160];
        /* the same day: only the end's time */
        if (start[0] && end[0] && !strncmp(start, end, 10) && strlen(end) > 11) snprintf(when, sizeof when, "%s \xE2\x80\x93 %s", start, end + 11);
        else if (start[0] && end[0]) snprintf(when, sizeof when, "%s \xE2\x80\x93 %s", start, end);
        else snprintf(when, sizeof when, "%s", start);
        add_line(&b, _("Cuándo"), when);
        add_line(&b, _("Vence"), due);
        add_line(&b, _("Dónde"), loc);
        if (rr) {
            const char *f = strstr(rr, "FREQ=");
            const char *w = !f ? NULL : !strncmp(f + 5, "DAILY", 5) ? _("Todos los días") : !strncmp(f + 5, "WEEKLY", 6) ? _("Todas las semanas")
                          : !strncmp(f + 5, "MONTHLY", 7) ? _("Todos los meses") : !strncmp(f + 5, "YEARLY", 6) ? _("Todos los años") : NULL;
            add_line(&b, _("Se repite"), w ? w : rr);
        }
        if (desc && desc[0]) add_line(&b, _("Notas"), desc);
    }
    if (!ne) {
        of_bld_para(&b);
        say(&b, _("No hay eventos en este archivo."));
    }
    of_free(E);
    of_free(C);
    of_free(L);
    of_free(s);
    of_bld_done(&b);
    snprintf(d->kind, sizeof d->kind, "iCalendar");
    char note[64];
    snprintf(note, sizeof note, _("%d eventos"), ne);
    d->note = of_strdup(note);
    return d;
}

/* -------------------------------------------------------------------------- */
/* The tree of a data file                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    of_bld_t *b;
    int       lines;
    bool      cut;
} tree_t;

static bool tline(tree_t *Tr, int depth)
{
    if (Tr->lines >= TREE_MAX) { Tr->cut = true; return false; }
    Tr->lines++;
    Tr->b->indent = depth > 12 ? 12 : depth;
    Tr->b->cur = -1;
    of_bld_para(Tr->b);
    return true;
}

static void tput(tree_t *Tr, const char *s, size_t n, uint32_t color, bool bold)
{
    of_rstyle_t keep = Tr->b->st;
    Tr->b->st.color = color;
    if (bold) Tr->b->st.b = OF_TRI_ON;
    /* long values are cut: the tree is for finding one's way */
    if (n > 400) {
        of_bld_text(Tr->b, s, 400);
        of_bld_text(Tr->b, "\xE2\x80\xA6", 3);
    } else {
        of_bld_text(Tr->b, s, n);
    }
    Tr->b->st = keep;
}

/* JSON */
typedef struct { const char *s; size_t n, i; } js_t;

static void js_ws(js_t *J) { while (J->i < J->n && (J->s[J->i] == ' ' || J->s[J->i] == '\n' || J->s[J->i] == '\r' || J->s[J->i] == '\t')) J->i++; }

static bool js_string(js_t *J, of_sb_t *o)
{
    if (J->i >= J->n || J->s[J->i] != '"') return false;
    J->i++;
    while (J->i < J->n && J->s[J->i] != '"') {
        char c = J->s[J->i++];
        if (c != '\\' || J->i >= J->n) { of_sb_putc(o, c); continue; }
        char e = J->s[J->i++];
        switch (e) {
        case 'n': of_sb_putc(o, ' '); break;
        case 't': of_sb_putc(o, ' '); break;
        case 'r': break;
        case 'b': case 'f': break;
        case 'u': {
            if (J->i + 4 > J->n) return false;
            char hx[5] = { J->s[J->i], J->s[J->i + 1], J->s[J->i + 2], J->s[J->i + 3], 0 };
            uint32_t cp = (uint32_t)strtoul(hx, NULL, 16);
            J->i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && J->i + 6 <= J->n && J->s[J->i] == '\\' && J->s[J->i + 1] == 'u') {
                char lo[5] = { J->s[J->i + 2], J->s[J->i + 3], J->s[J->i + 4], J->s[J->i + 5], 0 };
                uint32_t l = (uint32_t)strtoul(lo, NULL, 16);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (l - 0xDC00);
                J->i += 6;
            }
            char u[4];
            of_sb_put(o, u, (size_t)of_utf8_put(u, cp));
            break;
        }
        default: of_sb_putc(o, e); break;
        }
    }
    J->i++;
    return true;
}

static bool js_value(tree_t *Tr, js_t *J, int depth, const char *key, size_t kl, bool key_is_index);

static void js_key(tree_t *Tr, const char *key, size_t kl, bool index)
{
    if (!key) return;
    tput(Tr, key, kl, index ? C_GREY : C_KEY, !index);
    tput(Tr, ": ", 2, C_GREY, false);
}

static bool js_value(tree_t *Tr, js_t *J, int depth, const char *key, size_t kl, bool idx)
{
    js_ws(J);
    if (J->i >= J->n || depth > 64) return false;
    char c = J->s[J->i];
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        J->i++;
        bool show = tline(Tr, depth);
        if (show) {
            js_key(Tr, key, kl, idx);
            /* how many inside, without walking them twice: counted as we go and said at the end */
        }
        int line = Tr->b->cur;
        int count = 0;
        js_ws(J);
        if (J->i < J->n && J->s[J->i] == (obj ? '}' : ']')) { J->i++; if (show) tput(Tr, obj ? "{ }" : "[ ]", 3, C_GREY, false); return true; }
        while (J->i < J->n) {
            js_ws(J);
            if (obj) {
                of_sb_t k = { 0 };
                if (!js_string(J, &k)) { of_sb_free(&k); return false; }
                js_ws(J);
                if (J->i >= J->n || J->s[J->i] != ':') { of_sb_free(&k); return false; }
                J->i++;
                bool ok = js_value(Tr, J, depth + 1, k.s ? k.s : "", k.n, false);
                of_sb_free(&k);
                if (!ok) return false;
            } else {
                char ix[16];
                int l = snprintf(ix, sizeof ix, "%d", count);
                if (!js_value(Tr, J, depth + 1, ix, (size_t)l, true)) return false;
            }
            count++;
            js_ws(J);
            if (J->i < J->n && J->s[J->i] == ',') { J->i++; continue; }
            if (J->i < J->n && J->s[J->i] == (obj ? '}' : ']')) { J->i++; break; }
            return false;
        }
        if (show && line >= 0 && line < Tr->b->d->np) {
            /* "{3}" or "[12]" after the key, in the line opened above */
            int keep = Tr->b->cur;
            Tr->b->cur = line;
            char sz[24];
            int l = snprintf(sz, sizeof sz, obj ? "{%d}" : "[%d]", count);
            tput(Tr, sz, (size_t)l, C_GREY, false);
            Tr->b->cur = keep;
        }
        return true;
    }
    if (!tline(Tr, depth)) {
        /* past the limit: still walk the value, to stay in step */
        if (c == '"') { of_sb_t t = { 0 }; bool ok = js_string(J, &t); of_sb_free(&t); return ok; }
        while (J->i < J->n && !strchr(",}] \n\r\t", J->s[J->i])) J->i++;
        return true;
    }
    js_key(Tr, key, kl, idx);
    if (c == '"') {
        of_sb_t t = { 0 };
        bool ok = js_string(J, &t);
        tput(Tr, "\"", 1, C_STRING, false);
        tput(Tr, t.s ? t.s : "", t.n, C_STRING, false);
        tput(Tr, "\"", 1, C_STRING, false);
        of_sb_free(&t);
        return ok;
    }
    size_t a = J->i;
    while (J->i < J->n && !strchr(",}] \n\r\t", J->s[J->i])) J->i++;
    if (J->i == a) return false;
    bool num = (J->s[a] >= '0' && J->s[a] <= '9') || J->s[a] == '-';
    tput(Tr, J->s + a, J->i - a, num ? C_NUMBER : C_WORD, false);
    return true;
}

/* XML */
static void xml_tree(tree_t *Tr, const char *s, size_t n)
{
    of_xml_t X;
    of_xml_init(&X, s, n);
    int depth = 0, t;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_OPEN || t == OX_EMPTY) {
            if (!tline(Tr, depth)) break;
            tput(Tr, "<", 1, C_GREY, false);
            tput(Tr, X.name, (size_t)X.name_len, C_KEY, true);
            /* the attributes, name="value" */
            const char *a = X.attrs;
            int al = X.attrs_len;
            int i = 0;
            while (i < al) {
                while (i < al && (a[i] == ' ' || a[i] == '\n' || a[i] == '\t' || a[i] == '\r')) i++;
                int k0 = i;
                while (i < al && a[i] != '=' && a[i] != ' ') i++;
                int k1 = i;
                while (i < al && a[i] != '"' && a[i] != '\'') i++;
                if (i >= al) break;
                char q = a[i++];
                int v0 = i;
                while (i < al && a[i] != q) i++;
                int v1 = i++;
                tput(Tr, " ", 1, 0, false);
                tput(Tr, a + k0, (size_t)(k1 - k0), C_WORD, false);
                tput(Tr, "=", 1, C_GREY, false);
                of_sb_t v = { 0 };
                of_xml_unescape(a + v0, (size_t)(v1 - v0), &v);
                tput(Tr, "\"", 1, C_STRING, false);
                tput(Tr, v.s ? v.s : "", v.n, C_STRING, false);
                tput(Tr, "\"", 1, C_STRING, false);
                of_sb_free(&v);
            }
            tput(Tr, t == OX_EMPTY ? "/>" : ">", t == OX_EMPTY ? 2 : 1, C_GREY, false);
            if (t == OX_OPEN) depth++;
        } else if (t == OX_CLOSE) {
            if (depth) depth--;
        } else if (t == OX_TEXT) {
            /* text, if it is more than white space */
            const char *p = X.src + X.start;
            size_t l = X.end - X.start;
            size_t a = 0;
            while (a < l && (p[a] == ' ' || p[a] == '\n' || p[a] == '\r' || p[a] == '\t')) a++;
            if (a == l) continue;
            of_sb_t v = { 0 };
            of_xml_unescape(p, l, &v);
            if (!tline(Tr, depth)) { of_sb_free(&v); break; }
            tput(Tr, v.s ? v.s : "", v.n, 0, false);
            of_sb_free(&v);
        }
    }
}

/* YAML and TOML: line by line, the source's own indentation */
static void lines_tree(tree_t *Tr, const char *s, size_t n, bool toml)
{
    size_t from = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && s[i] != '\n') continue;
        const char *l = s + from;
        size_t ln = i - from;
        from = i + 1;
        if (ln && l[ln - 1] == '\r') ln--;
        size_t a = 0;
        while (a < ln && (l[a] == ' ' || l[a] == '\t')) a++;
        if (a == ln) continue;
        const char *t = l + a;
        size_t tn = ln - a;
        if (tn >= 3 && (!strncmp(t, "---", 3) || !strncmp(t, "...", 3)) && !toml) {
            if (!tline(Tr, 0)) break;
            tput(Tr, t, tn, C_GREY, false);
            continue;
        }
        int depth = toml ? 0 : (int)a / 2;
        if (!tline(Tr, depth)) break;
        if (t[0] == '#') { tput(Tr, t, tn, C_GREY, false); continue; }
        if (toml && t[0] == '[') {
            /* [table] and [[array]]: a heading of its own */
            int dots = 0;
            for (size_t k = 0; k < tn; k++) if (t[k] == '.') dots++;
            Tr->b->d->p[Tr->b->cur].heading = (uint8_t)(dots >= 2 ? 3 : dots + 2);
            tput(Tr, t, tn, 0, false);
            continue;
        }
        size_t off = 0;
        if (!toml && t[0] == '-' && (tn == 1 || t[1] == ' ')) {
            tput(Tr, "\xE2\x80\xA2 ", 4, C_GREY, false);
            off = tn > 1 ? 2 : 1;
        }
        const char *k = t + off;
        size_t kn = tn - off;
        /* key: value / key = value, outside quotes */
        size_t sep = 0;
        char q = 0;
        for (size_t x = 0; x < kn; x++) {
            if (q) { if (k[x] == q) q = 0; continue; }
            if (k[x] == '"' || k[x] == '\'') { if (x == 0) q = k[x]; continue; }
            if (toml && k[x] == '=') { sep = x; break; }
            if (!toml && k[x] == ':' && (x + 1 == kn || k[x + 1] == ' ')) { sep = x; break; }
        }
        if (sep) {
            size_t ke = sep;
            while (ke && k[ke - 1] == ' ') ke--;
            tput(Tr, k, ke, C_KEY, true);
            tput(Tr, ": ", 2, C_GREY, false);
            const char *v = k + sep + 1;
            size_t vn = kn - sep - 1;
            while (vn && *v == ' ') { v++; vn--; }
            uint32_t col = !vn ? 0 : (v[0] == '"' || v[0] == '\'') ? C_STRING : ((v[0] >= '0' && v[0] <= '9') || v[0] == '-') ? C_NUMBER
                         : (!strncmp(v, "true", 4) || !strncmp(v, "false", 5) || !strncmp(v, "null", 4) || v[0] == '~') ? C_WORD : 0;
            if (vn) tput(Tr, v, vn, col, false);
        } else {
            tput(Tr, k, kn, (k[0] == '"' || k[0] == '\'') ? C_STRING : 0, false);
        }
    }
}

of_doc_t *of_tree_load(const char *path, char *err, size_t errlen)
{
    size_t n;
    char *s = read_text(path, &n, err, errlen);
    if (!s) return NULL;
    const char *e = of_ext(path);
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    of_bld_t b;
    of_bld_init(&b, d);
    tree_t Tr = { .b = &b };
    bool ok = true;
    if (!strcmp(e, "json") || !strcmp(e, "geojson") || !strcmp(e, "ipynb")) {
        js_t J = { s, n, 0 };
        ok = js_value(&Tr, &J, 0, NULL, 0, false);
        snprintf(d->kind, sizeof d->kind, "JSON");
        if (ok) {
            /* JSON lines: one value after another */
            js_ws(&J);
            while (ok && J.i < J.n) { ok = js_value(&Tr, &J, 0, NULL, 0, false); js_ws(&J); }
        }
    } else if (!strcmp(e, "toml")) {
        lines_tree(&Tr, s, n, true);
        snprintf(d->kind, sizeof d->kind, "TOML");
    } else if (!strcmp(e, "yaml") || !strcmp(e, "yml")) {
        lines_tree(&Tr, s, n, false);
        snprintf(d->kind, sizeof d->kind, "YAML");
    } else {
        xml_tree(&Tr, s, n);
        snprintf(d->kind, sizeof d->kind, "XML");
    }
    b.indent = 0;
    if (!ok && !Tr.cut) {
        b.cur = -1;
        of_bld_para(&b);
        b.st.color = 0x1C03030;
        say(&b, _("El archivo tiene un error a partir de acá."));
    }
    if (Tr.cut) {
        char m[96];
        snprintf(m, sizeof m, _("Se muestran las primeras %d líneas."), TREE_MAX);
        d->note = of_strdup(m);
    }
    of_bld_done(&b);
    of_free(s);
    return d;
}
