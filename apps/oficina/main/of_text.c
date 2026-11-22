/*
 * P4OS - Office: plain text.
 *
 * The text lives as UTF-8 with '\n' line ends in a gap buffer (typing in the
 * middle of a megabyte moves only what is between the caret and the gap),
 * with an index of where each line starts, rebuilt from the edit onwards.
 * What the file was like is remembered and given back when saving: its
 * encoding (UTF-8 with or without BOM, UTF-16 with BOM, Windows-1252), its
 * line ends (LF, CRLF, CR) and whether it ended with one.
 */
#include "of.h"

#include <stdio.h>
#include <string.h>

#define TEXT_MAX (16u * 1024 * 1024)

void of_text_init(of_text_t *t)
{
    memset(t, 0, sizeof *t);
    t->cap = 4096;
    t->buf = of_alloc(t->cap);
    t->gap0 = 0;
    t->gap1 = t->cap;
    t->lcap = 256;
    t->line = of_alloc((size_t)t->lcap * sizeof *t->line);
    t->nlines = 1;
    t->final_eol = true;
}

void of_text_free(of_text_t *t)
{
    of_free(t->buf);
    of_free(t->line);
    memset(t, 0, sizeof *t);
}

size_t of_text_len(const of_text_t *t)
{
    return t->cap - (t->gap1 - t->gap0);
}

char of_text_at(const of_text_t *t, size_t pos)
{
    if (pos < t->gap0) return t->buf[pos];
    pos += t->gap1 - t->gap0;
    return pos < t->cap ? t->buf[pos] : 0;
}

static void move_gap(of_text_t *t, size_t pos)
{
    if (pos < t->gap0) {
        size_t n = t->gap0 - pos;
        memmove(t->buf + t->gap1 - n, t->buf + pos, n);
        t->gap0 -= n;
        t->gap1 -= n;
    } else if (pos > t->gap0) {
        size_t n = pos - t->gap0;
        memmove(t->buf + t->gap0, t->buf + t->gap1, n);
        t->gap0 += n;
        t->gap1 += n;
    }
}

static bool grow(of_text_t *t, size_t need)
{
    if (t->gap1 - t->gap0 >= need) return true;
    size_t len = of_text_len(t);
    size_t cap = t->cap * 2;
    while (cap - len < need + 1024) cap *= 2;
    char *nb = of_alloc(cap);
    if (!nb) return false;
    size_t tail = t->cap - t->gap1;
    memcpy(nb, t->buf, t->gap0);
    memcpy(nb + cap - tail, t->buf + t->gap1, tail);
    of_free(t->buf);
    t->buf = nb;
    t->gap1 = cap - tail;
    t->cap = cap;
    return true;
}

/* line starts from line 'from' on */
static void reindex(of_text_t *t, int from)
{
    if (from < 0) from = 0;
    if (from >= t->nlines) from = t->nlines - 1;
    size_t len = of_text_len(t);
    int n = from + 1;
    size_t p = t->line[from];
    /* two passes over the halves, with memchr */
    for (;;) {
        size_t nl;
        if (p < t->gap0) {
            const char *hit = memchr(t->buf + p, '\n', t->gap0 - p);
            if (!hit) { p = t->gap0; continue; }
            nl = (size_t)(hit - t->buf);
        } else {
            if (p >= len) break;
            size_t phys = p + (t->gap1 - t->gap0);
            const char *hit = memchr(t->buf + phys, '\n', t->cap - phys);
            if (!hit) break;
            nl = (size_t)(hit - t->buf) - (t->gap1 - t->gap0);
        }
        if (n == t->lcap) {
            int cap = t->lcap * 2;
            uint32_t *nlx = of_realloc(t->line, (size_t)cap * sizeof *nlx);
            if (!nlx) break;
            t->line = nlx;
            t->lcap = cap;
        }
        t->line[n++] = (uint32_t)(nl + 1);
        p = nl + 1;
    }
    t->nlines = n;
}

void of_text_insert(of_text_t *t, size_t pos, const char *s, size_t n)
{
    if (!n || !grow(t, n)) return;
    size_t len = of_text_len(t);
    if (pos > len) pos = len;
    int line = of_text_line_of(t, pos);
    move_gap(t, pos);
    memcpy(t->buf + t->gap0, s, n);
    t->gap0 += n;
    t->modified = true;
    if (memchr(s, '\n', n)) {
        reindex(t, line);
    } else {
        for (int i = line + 1; i < t->nlines; i++) t->line[i] += (uint32_t)n;
    }
}

void of_text_delete(of_text_t *t, size_t pos, size_t n)
{
    size_t len = of_text_len(t);
    if (pos >= len || !n) return;
    if (pos + n > len) n = len - pos;
    int line = of_text_line_of(t, pos);
    bool nl = false;
    for (size_t i = 0; i < n && !nl; i++) nl = of_text_at(t, pos + i) == '\n';
    move_gap(t, pos);
    t->gap1 += n;
    t->modified = true;
    if (nl) {
        reindex(t, line);
    } else {
        for (int i = line + 1; i < t->nlines; i++) t->line[i] -= (uint32_t)n;
    }
}

char *of_text_range(const of_text_t *t, size_t a, size_t b)
{
    size_t len = of_text_len(t);
    if (b > len) b = len;
    if (a > b) a = b;
    char *o = of_alloc(b - a + 1);
    if (!o) return NULL;
    size_t k = 0;
    if (a < t->gap0) {
        size_t e = b < t->gap0 ? b : t->gap0;
        memcpy(o, t->buf + a, e - a);
        k = e - a;
        a = e;
    }
    if (a < b) {
        memcpy(o + k, t->buf + a + (t->gap1 - t->gap0), b - a);
        k += b - a;
    }
    o[k] = 0;
    return o;
}

int of_text_line_of(const of_text_t *t, size_t pos)
{
    int lo = 0, hi = t->nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (t->line[mid] <= pos) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

void of_text_line_span(const of_text_t *t, int line, size_t *a, size_t *b)
{
    if (line < 0) line = 0;
    if (line >= t->nlines) line = t->nlines - 1;
    *a = t->line[line];
    *b = line + 1 < t->nlines ? t->line[line + 1] - 1 : of_text_len(t);
}

int of_text_line_copy(const of_text_t *t, int line, char *out, int len)
{
    size_t a, b;
    of_text_line_span(t, line, &a, &b);
    if (b - a > (size_t)len - 1) b = a + (size_t)len - 1;
    int k = 0;
    for (size_t p = a; p < b; p++) out[k++] = of_text_at(t, p);
    out[k] = 0;
    return k;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

long of_text_find(const of_text_t *t, size_t pos, const char *needle, bool back, bool match_case)
{
    size_t n = strlen(needle), len = of_text_len(t);
    if (!n || n > len) return -1;
    if (!back) {
        for (size_t p = pos; p + n <= len; p++) {
            size_t k = 0;
            while (k < n && (match_case ? of_text_at(t, p + k) == needle[k] : lower(of_text_at(t, p + k)) == lower(needle[k]))) k++;
            if (k == n) return (long)p;
        }
    } else {
        if (pos + n > len) pos = len - n;
        for (long p = (long)pos; p >= 0; p--) {
            size_t k = 0;
            while (k < n && (match_case ? of_text_at(t, (size_t)p + k) == needle[k]
                                         : lower(of_text_at(t, (size_t)p + k)) == lower(needle[k]))) k++;
            if (k == n) return p;
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Files                                                                       */
/* -------------------------------------------------------------------------- */

static char *utf16_to_utf8(const uint8_t *s, size_t n, bool be, size_t *out_len)
{
    char *o = of_alloc(n * 2 + 4);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = be ? (uint32_t)(s[i] << 8 | s[i + 1]) : (uint32_t)(s[i + 1] << 8 | s[i]);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
            uint32_t l = be ? (uint32_t)(s[i + 2] << 8 | s[i + 3]) : (uint32_t)(s[i + 3] << 8 | s[i + 2]);
            if (l >= 0xDC00 && l < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00);
                i += 2;
            }
        }
        k += (size_t)of_utf8_put(o + k, u);
    }
    o[k] = 0;
    *out_len = k;
    return o;
}

bool of_text_load(of_text_t *t, const char *path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *raw = of_file_read(path, &len, TEXT_MAX);
    if (!raw) {
        snprintf(err, errlen, of_file_exists(path) ? "big" : "read");
        return false;
    }
    of_text_init(t);
    const char *s = (const char *)raw;
    char *conv = NULL;
    size_t n = len;
    if (n >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        t->bom = true;
        s += 3;
        n -= 3;
    } else if (n >= 2 && ((raw[0] == 0xFF && raw[1] == 0xFE) || (raw[0] == 0xFE && raw[1] == 0xFF))) {
        bool be = raw[0] == 0xFE;
        t->enc = be ? OT_ENC_UTF16BE : OT_ENC_UTF16LE;
        t->bom = true;
        conv = utf16_to_utf8(raw + 2, n - 2, be, &n);
        s = conv;
    }
    if (!conv && !of_utf8_valid(s, n)) {
        conv = of_latin1_to_utf8(s, n, &n);
        s = conv;
        t->enc = OT_ENC_LATIN1;
    }
    if (!s) { of_free(raw); of_text_free(t); return false; }
    /* line ends: what the first one is */
    const char *cr = memchr(s, '\r', n);
    const char *lf = memchr(s, '\n', n);
    if (cr && (!lf || cr < lf)) t->eol = cr + 1 < s + n && cr[1] == '\n' ? OT_EOL_CRLF : OT_EOL_CR;
    /* into the buffer, '\n' only */
    char *norm = of_alloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; norm && i < n; i++) {
        if (s[i] == '\r') {
            norm[k++] = '\n';
            if (i + 1 < n && s[i + 1] == '\n') i++;
        } else {
            norm[k++] = s[i];
        }
    }
    t->final_eol = k == 0 || (norm && norm[k - 1] == '\n');
    /* the final line end is not a line of its own in the editor */
    if (norm && k && norm[k - 1] == '\n') k--;
    if (norm) of_text_insert(t, 0, norm, k);
    of_free(norm);
    of_free(conv);
    of_free(raw);
    t->modified = false;
    return true;
}

bool of_text_save(of_text_t *t, const char *path, char *err, size_t errlen)
{
    size_t len = of_text_len(t);
    char *all = of_text_range(t, 0, len);
    if (!all) { snprintf(err, errlen, "memory"); return false; }
    of_sb_t o = { 0 };
    const char *eol = t->eol == OT_EOL_CRLF ? "\r\n" : t->eol == OT_EOL_CR ? "\r" : "\n";
    size_t from = 0;
    for (size_t i = 0; i < len; i++) {
        if (all[i] != '\n') continue;
        of_sb_put(&o, all + from, i - from);
        of_sb_puts(&o, eol);
        from = i + 1;
    }
    of_sb_put(&o, all + from, len - from);
    if (t->final_eol && len) of_sb_puts(&o, eol);
    of_free(all);
    if (o.oom) { of_sb_free(&o); snprintf(err, errlen, "memory"); return false; }
    bool ok;
    const char *data = o.s ? o.s : "";
    size_t dl = o.n;
    if (t->enc == OT_ENC_LATIN1) {
        int lost = 0;
        char *c = of_utf8_to_latin1(data, dl, &dl, &lost);
        ok = c && of_file_write(path, c, dl);
        of_free(c);
    } else if (t->enc == OT_ENC_UTF16LE || t->enc == OT_ENC_UTF16BE) {
        bool be = t->enc == OT_ENC_UTF16BE;
        of_sb_t u = { 0 };
        of_sb_put(&u, be ? "\xFE\xFF" : "\xFF\xFE", 2);
        for (size_t i = 0; i < dl;) {
            int l;
            uint32_t cp = of_utf8_get(data + i, &l);
            i += (size_t)l;
            uint16_t units[2];
            int nu = 1;
            if (cp >= 0x10000) {
                cp -= 0x10000;
                units[0] = (uint16_t)(0xD800 + (cp >> 10));
                units[1] = (uint16_t)(0xDC00 + (cp & 0x3FF));
                nu = 2;
            } else {
                units[0] = (uint16_t)cp;
            }
            for (int k = 0; k < nu; k++) {
                char c2[2];
                c2[be ? 0 : 1] = (char)(units[k] >> 8);
                c2[be ? 1 : 0] = (char)(units[k] & 0xFF);
                of_sb_put(&u, c2, 2);
            }
        }
        ok = !u.oom && of_file_write(path, u.s, u.n);
        of_sb_free(&u);
    } else if (t->bom) {
        of_sb_t u = { 0 };
        of_sb_put(&u, "\xEF\xBB\xBF", 3);
        of_sb_put(&u, data, dl);
        ok = !u.oom && of_file_write(path, u.s, u.n);
        of_sb_free(&u);
    } else {
        ok = of_file_write(path, data, dl);
    }
    of_sb_free(&o);
    if (!ok) snprintf(err, errlen, "write");
    else t->modified = false;
    return ok;
}
