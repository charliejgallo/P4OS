/*
 * P4OS - Office: a pull tokenizer for XML.
 *
 * Not a parser: it hands out start tags, end tags, empty elements and text
 * with where each one is in the source, and does not build anything. That
 * is what saving without losing anything needs: the writers copy the source
 * between the offsets of what they did not touch, and only generate what
 * they changed. Namespaces are taken by their prefix, as the files write
 * them (every producer uses the usual ones: w:, a:, table:, office:).
 *
 * Comments, processing instructions, CDATA and the doctype come out as
 * OX_OTHER (CDATA's text is not decoded: no office part uses it).
 */
#include "of.h"

#include <stdlib.h>
#include <string.h>

void of_xml_init(of_xml_t *x, const char *src, size_t len)
{
    memset(x, 0, sizeof *x);
    x->src = src;
    x->len = len;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static const char *find(const char *s, size_t n, const char *what)
{
    size_t wl = strlen(what);
    if (wl > n) return NULL;
    for (size_t i = 0; i + wl <= n; i++)
        if (s[i] == what[0] && memcmp(s + i, what, wl) == 0) return s + i;
    return NULL;
}

int of_xml_next(of_xml_t *x)
{
    const char *s = x->src;
    size_t p = x->pos, n = x->len;
    x->name = NULL;
    x->name_len = 0;
    x->attrs = NULL;
    x->attrs_len = 0;
    if (p >= n) return x->type = OX_END;
    x->start = p;
    if (s[p] != '<') {
        const char *lt = memchr(s + p, '<', n - p);
        size_t e = lt ? (size_t)(lt - s) : n;
        x->end = x->pos = e;
        return x->type = OX_TEXT;
    }
    if (p + 1 >= n) return x->type = OX_ERROR;
    char c = s[p + 1];
    if (c == '?' || c == '!') {
        const char *end;
        if (c == '!' && p + 4 <= n && memcmp(s + p, "<!--", 4) == 0) {
            end = find(s + p + 4, n - p - 4, "-->");
            end = end ? end + 3 : NULL;
        } else if (c == '!' && p + 9 <= n && memcmp(s + p, "<![CDATA[", 9) == 0) {
            end = find(s + p + 9, n - p - 9, "]]>");
            end = end ? end + 3 : NULL;
        } else {
            end = memchr(s + p, '>', n - p);
            end = end ? end + 1 : NULL;
        }
        if (!end) return x->type = OX_ERROR;
        x->end = x->pos = (size_t)(end - s);
        return x->type = OX_OTHER;
    }
    bool close = c == '/';
    size_t q = p + (close ? 2 : 1);
    size_t name0 = q;
    while (q < n && !is_space(s[q]) && s[q] != '>' && s[q] != '/') q++;
    x->name = s + name0;
    x->name_len = (int)(q - name0);
    /* to the '>' that is not inside a quoted value */
    size_t a0 = q;
    char quote = 0;
    while (q < n) {
        char ch = s[q];
        if (quote) {
            if (ch == quote) quote = 0;
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '>') {
            break;
        }
        q++;
    }
    if (q >= n) return x->type = OX_ERROR;
    x->end = x->pos = q + 1;
    if (close) return x->type = OX_CLOSE;
    bool empty = q > a0 && s[q - 1] == '/';
    x->attrs = s + a0;
    x->attrs_len = (int)(q - a0 - (empty ? 1 : 0));
    return x->type = empty ? OX_EMPTY : OX_OPEN;
}

bool of_xml_is(const of_xml_t *x, const char *name)
{
    size_t l = strlen(name);
    return x->name && (size_t)x->name_len == l && memcmp(x->name, name, l) == 0;
}

bool of_xml_local_is(const of_xml_t *x, const char *local)
{
    if (!x->name) return false;
    const char *nm = x->name;
    int nl = x->name_len;
    const char *colon = memchr(nm, ':', (size_t)nl);
    if (colon) {
        nl -= (int)(colon + 1 - nm);
        nm = colon + 1;
    }
    size_t l = strlen(local);
    return (size_t)nl == l && memcmp(nm, local, l) == 0;
}

bool of_xml_attr_raw(const char *a, int alen, const char *name, const char **v, int *vlen)
{
    size_t nl = strlen(name);
    int i = 0;
    while (i < alen) {
        while (i < alen && is_space(a[i])) i++;
        int k0 = i;
        while (i < alen && a[i] != '=' && !is_space(a[i])) i++;
        int k1 = i;
        while (i < alen && is_space(a[i])) i++;
        if (i >= alen || a[i] != '=') return false;
        i++;
        while (i < alen && is_space(a[i])) i++;
        if (i >= alen) return false;
        char q = a[i];
        if (q != '"' && q != '\'') return false;
        int v0 = ++i;
        while (i < alen && a[i] != q) i++;
        if ((size_t)(k1 - k0) == nl && memcmp(a + k0, name, nl) == 0) {
            *v = a + v0;
            *vlen = i - v0;
            return true;
        }
        i++;
    }
    return false;
}

bool of_xml_attr(const of_xml_t *x, const char *name, char *out, size_t len)
{
    const char *v;
    int vl;
    if (!x->attrs || !of_xml_attr_raw(x->attrs, x->attrs_len, name, &v, &vl)) return false;
    of_sb_t sb = { 0 };
    of_xml_unescape(v, (size_t)vl, &sb);
    size_t k = sb.n < len - 1 ? sb.n : len - 1;
    if (sb.s) memcpy(out, sb.s, k);
    out[k] = 0;
    of_sb_free(&sb);
    return true;
}

void of_xml_skip(of_xml_t *x)
{
    int depth = 1;
    while (depth > 0) {
        int t = of_xml_next(x);
        if (t == OX_OPEN) depth++;
        else if (t == OX_CLOSE) depth--;
        else if (t == OX_END || t == OX_ERROR) return;
    }
}

void of_xml_text(of_xml_t *x, of_sb_t *sb)
{
    int depth = 1;
    while (depth > 0) {
        int t = of_xml_next(x);
        if (t == OX_OPEN) depth++;
        else if (t == OX_CLOSE) depth--;
        else if (t == OX_TEXT) of_xml_unescape(x->src + x->start, x->end - x->start, sb);
        else if (t == OX_EMPTY) {
            /* the ODF whitespace elements, inside a text:p */
            if (of_xml_is(x, "text:s")) {
                char c[8] = "1";
                of_xml_attr(x, "text:c", c, sizeof c);
                int k = atoi(c);
                for (int i = 0; i < (k > 0 ? k : 1) && i < 1000; i++) of_sb_putc(sb, ' ');
            } else if (of_xml_is(x, "text:tab")) {
                of_sb_putc(sb, '\t');
            } else if (of_xml_is(x, "text:line-break")) {
                of_sb_putc(sb, '\n');
            }
        } else if (t == OX_END || t == OX_ERROR) return;
    }
}

void of_xml_unescape(const char *s, size_t n, of_sb_t *out)
{
    size_t from = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') continue;
        const char *semi = memchr(s + i, ';', n - i < 12 ? n - i : 12);
        if (!semi) continue;
        size_t el = (size_t)(semi - (s + i)) + 1;
        char rep[5];
        int rl = 0;
        if (el == 4 && memcmp(s + i, "&lt;", 4) == 0) rep[rl++] = '<';
        else if (el == 4 && memcmp(s + i, "&gt;", 4) == 0) rep[rl++] = '>';
        else if (el == 5 && memcmp(s + i, "&amp;", 5) == 0) rep[rl++] = '&';
        else if (el == 6 && memcmp(s + i, "&quot;", 6) == 0) rep[rl++] = '"';
        else if (el == 6 && memcmp(s + i, "&apos;", 6) == 0) rep[rl++] = '\'';
        else if (el > 3 && s[i + 1] == '#') {
            uint32_t cp = 0;
            bool hex = s[i + 2] == 'x' || s[i + 2] == 'X';
            for (size_t k = i + (hex ? 3 : 2); k < i + el - 1; k++) {
                char c = s[k];
                int d = c >= '0' && c <= '9' ? c - '0'
                      : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0) { cp = 0xFFFD; break; }
                cp = cp * (hex ? 16 : 10) + (uint32_t)d;
            }
            if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
            rl = of_utf8_put(rep, cp);
        } else {
            continue;
        }
        of_sb_put(out, s + from, i - from);
        of_sb_put(out, rep, (size_t)rl);
        from = i + el;
        i += el - 1;
    }
    of_sb_put(out, s + from, n - from);
}
