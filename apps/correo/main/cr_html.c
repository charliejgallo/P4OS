/*
 * P4OS - Correo: HTML turned into formatted text (cr.h).
 *
 * No web engine: a single pass over the tags that keeps what reading needs
 * and drops the rest. Blocks (paragraphs, headings, list items, quotes,
 * preformatted text, rules) become cr_block_t; inside them bold, italics,
 * underline, code and links become spans. White space collapses as a
 * browser would, tables come out a row per line, pictures named by cid:
 * are shown from the message's own parts, and pictures from the network
 * are never fetched (they are how a newsletter knows it was opened): their
 * alt text stands in, dimmed.
 *
 * Styles, scripts, the head and comments are skipped whole. Nothing in an
 * attribute is ever run or followed by itself; a link is only shown, and
 * the screen offers its address (as a QR code) when it is touched.
 */
#include "cr.h"

#include <stdio.h>
#include <stdlib.h>

#define LIST_MAX 8

typedef struct {
    cr_rt_t  *r;
    cr_msg_t *m;
    cr_buf_t  pend;            /* text waiting for a style change */
    bool      last_space;
    int       bold, ital, under, code, pre, quote;
    int       skip;            /* inside <style>, <script>, <head>... */
    int       link;
    int       nlist;
    bool      ol[LIST_MAX];
    int       count[LIST_MAX];
    int       cells;           /* cells written in this table row */
    int       head;            /* inside a heading */
} hx_t;

static void flush(hx_t *x)
{
    if (!x->pend.n) return;
    cr_rt_t *r = x->r;
    r->style = (uint8_t)((x->bold ? CR_S_BOLD : 0) | (x->ital ? CR_S_ITAL : 0) | (x->under ? CR_S_UNDER : 0) |
                         (x->code ? CR_S_CODE : 0) | (x->link >= 0 ? CR_S_LINK : 0));
    r->link = (int16_t)x->link;
    cr_rt_text(r, x->pend.p, x->pend.n);
    x->pend.n = 0;
    r->style = 0;
    r->link = -1;
}

/* A new block, of the kind the open quote and list say, unless one of the
 * caller's is asked for. */
static void block(hx_t *x, int type, int lvl, const char *bullet)
{
    flush(x);
    if (type < 0) {
        type = x->pre ? CR_B_PRE : x->quote ? CR_B_QUOTE : CR_B_PARA;
        lvl = x->quote;
    }
    cr_rt_block(x->r, type, lvl, bullet);
    x->last_space = true;
}

static void text(hx_t *x, const char *s, size_t n)
{
    if (x->skip) return;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!x->pre && cr_space((unsigned char)c)) {
            if (x->last_space) continue;
            c = ' ';
            x->last_space = true;
        } else {
            x->last_space = c == '\n';
        }
        cr_buf_add(&x->pend, &c, 1);
    }
}

/* ---- entities --------------------------------------------------------------------------------- */

static const struct {
    const char *name;
    uint16_t    cp;
} ENT[] = {
    {"amp", '&'},       {"lt", '<'},         {"gt", '>'},         {"quot", '"'},       {"apos", '\''},
    {"nbsp", ' '},      {"copy", 0xA9},      {"reg", 0xAE},       {"trade", 0x2122},   {"mdash", 0x2014},
    {"ndash", 0x2013},  {"hellip", 0x2026},  {"laquo", 0xAB},     {"raquo", 0xBB},     {"ldquo", 0x201C},
    {"rdquo", 0x201D},  {"lsquo", 0x2018},   {"rsquo", 0x2019},   {"bull", 0x2022},    {"middot", 0xB7},
    {"euro", 0x20AC},   {"pound", 0xA3},     {"cent", 0xA2},      {"deg", 0xB0},       {"times", 0xD7},
    {"divide", 0xF7},   {"iexcl", 0xA1},     {"iquest", 0xBF},    {"ordf", 0xAA},      {"ordm", 0xBA},
    {"aacute", 0xE1},   {"eacute", 0xE9},    {"iacute", 0xED},    {"oacute", 0xF3},    {"uacute", 0xFA},
    {"Aacute", 0xC1},   {"Eacute", 0xC9},    {"Iacute", 0xCD},    {"Oacute", 0xD3},    {"Uacute", 0xDA},
    {"ntilde", 0xF1},   {"Ntilde", 0xD1},    {"uuml", 0xFC},      {"Uuml", 0xDC},      {"ccedil", 0xE7},
    {"agrave", 0xE0},   {"egrave", 0xE8},    {"ograve", 0xF2},    {"acirc", 0xE2},     {"ecirc", 0xEA},
    {"ocirc", 0xF4},    {"auml", 0xE4},      {"ouml", 0xF6},      {"Auml", 0xC4},      {"Ouml", 0xD6},
    {"szlig", 0xDF},    {"zwnj", 0},         {"zwj", 0x200D},     {"shy", 0},          {"rarr", 0x2192},
    {"larr", 0x2190},   {"hearts", 0x2665},  {"check", 0x2713},   {"sect", 0xA7},      {"para", 0xB6},
};

static size_t put_utf8(char *o, uint32_t cp)
{
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 63));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 63));
        o[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 63));
    o[2] = (char)(0x80 | ((cp >> 6) & 63));
    o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

/* "&...;" at s: the bytes it stands for in out, how far it went in *used.
 * 0 if it is not an entity (the '&' is then text). */
static size_t entity(const char *s, size_t n, char *out, size_t *used)
{
    size_t e = 1;
    while (e < n && e < 12 && s[e] != ';' && !cr_space((unsigned char)s[e]) && s[e] != '&' && s[e] != '<') e++;
    bool semi = e < n && s[e] == ';';
    if (e < 2) return 0;
    uint32_t cp = 0;
    bool ok = false;
    if (s[1] == '#') {
        char tmp[12];
        cr_ncopy(tmp, sizeof tmp, s + 2, e - 2);
        if (tmp[0] == 'x' || tmp[0] == 'X') cp = (uint32_t)strtoul(tmp + 1, NULL, 16);
        else cp = (uint32_t)strtoul(tmp, NULL, 10);
        ok = cp > 0 && cp < 0x110000;
        if (cp >= 0x80 && cp < 0xA0) {
            /* numeric references to Windows-1252 are what old pages wrote */
            static const uint16_t W[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                           0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
                                           0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                           0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
            cp = W[cp - 0x80];
        }
    } else {
        for (size_t i = 0; i < sizeof ENT / sizeof ENT[0]; i++) {
            size_t l = strlen(ENT[i].name);
            if (l == e - 1 && !memcmp(s + 1, ENT[i].name, l)) {
                cp = ENT[i].cp;
                ok = true;
                break;
            }
        }
    }
    if (!ok) return 0;
    *used = semi ? e + 1 : e;
    if (!cp) return 0;
    return put_utf8(out, cp);
}

/* Text with its entities decoded, into the pending text. */
static void text_ent(hx_t *x, const char *s, size_t n)
{
    size_t from = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') continue;
        char u[8];
        size_t used = 0;
        size_t k = entity(s + i, n - i, u, &used);
        if (!used) continue;
        text(x, s + from, i - from);
        if (k) {
            /* a non-breaking space is a space that does not collapse away */
            if (k == 1 && u[0] == ' ' && !x->pre) {
                cr_buf_add(&x->pend, " ", 1);
                x->last_space = false;
            } else {
                text(x, u, k);
            }
        }
        i += used - 1;
        from = i + 1;
    }
    text(x, s + from, n - from);
}

/* ---- attributes ------------------------------------------------------------------------------- */

/* The value of attribute 'name' in a tag's text, decoded of its entities. */
static bool attr(const char *t, size_t n, const char *name, char *out, size_t on)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl < n; i++) {
        if (!cr_space((unsigned char)t[i]) && t[i] != '"' && t[i] != '\'') continue;
        size_t j = i + 1;
        if (j + nl > n || !cr_istarts(t + j, name)) continue;
        j += nl;
        while (j < n && cr_space((unsigned char)t[j])) j++;
        if (j >= n || t[j] != '=') continue;
        j++;
        while (j < n && cr_space((unsigned char)t[j])) j++;
        char q = 0;
        if (j < n && (t[j] == '"' || t[j] == '\'')) q = t[j++];
        size_t k = 0;
        while (j < n && (q ? t[j] != q : !cr_space((unsigned char)t[j]) && t[j] != '>')) {
            if (t[j] == '&') {
                char u[8];
                size_t used = 0;
                size_t ul = entity(t + j, n - j, u, &used);
                if (used) {
                    for (size_t z = 0; z < ul && k + 1 < on; z++) out[k++] = u[z];
                    j += used;
                    continue;
                }
            }
            if (k + 1 < on) out[k++] = t[j];
            j++;
        }
        out[k] = '\0';
        return true;
    }
    if (on) out[0] = '\0';
    return false;
}

static bool is(const char *name, const char *t)
{
    return !strcmp(name, t);
}

static void tag_open(hx_t *x, const char *name, const char *t, size_t tn, bool self_close)
{
    if (is(name, "style") || is(name, "script") || is(name, "head") || is(name, "title") || is(name, "noscript") ||
        is(name, "template") || is(name, "xml") || is(name, "svg")) {
        if (!self_close) x->skip++;
        return;
    }
    if (is(name, "body")) {
        x->skip = 0;             /* a <head> that was never closed ends here */
        return;
    }
    if (x->skip) return;
    if (is(name, "br")) {
        text(x, "\n", 1);
        x->last_space = true;
        return;
    }
    if (is(name, "p") || is(name, "div") || is(name, "section") || is(name, "article") || is(name, "header") ||
        is(name, "footer") || is(name, "main") || is(name, "center") || is(name, "address") || is(name, "figure") ||
        is(name, "figcaption") || is(name, "dt") || is(name, "dd") || is(name, "form") || is(name, "fieldset") ||
        is(name, "table") || is(name, "nav") || is(name, "aside")) {
        if (x->r->type == CR_B_LI && x->r->t.n == 0 && !x->pend.n) return;   /* <li><p>: the item */
        block(x, -1, 0, NULL);
        return;
    }
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        block(x, name[1] <= '2' ? CR_B_H1 : CR_B_H2, 0, NULL);
        x->head++;
        return;
    }
    if (is(name, "ul") || is(name, "ol") || is(name, "menu")) {
        if (x->nlist < LIST_MAX) {
            x->ol[x->nlist] = is(name, "ol");
            char st[12];
            x->count[x->nlist] = attr(t, tn, "start", st, sizeof st) ? atoi(st) - 1 : 0;
            x->nlist++;
        }
        block(x, -1, 0, NULL);
        return;
    }
    if (is(name, "li")) {
        char b[12] = "\xE2\x80\xA2";        /* • */
        int d = x->nlist ? x->nlist - 1 : 0;
        if (x->nlist && x->ol[d]) snprintf(b, sizeof b, "%d.", ++x->count[d] % 100000);
        else if (d % 2) cr_copy(b, sizeof b, "-");
        block(x, CR_B_LI, d, b);
        return;
    }
    if (is(name, "blockquote")) {
        x->quote++;
        block(x, -1, 0, NULL);
        return;
    }
    if (is(name, "pre")) {
        x->pre++;
        block(x, CR_B_PRE, 0, NULL);
        return;
    }
    if (is(name, "hr")) {
        block(x, CR_B_HR, 0, NULL);
        cr_rt_end(x->r);
        block(x, -1, 0, NULL);
        return;
    }
    if (is(name, "tr")) {
        block(x, -1, 0, NULL);
        x->cells = 0;
        return;
    }
    if (is(name, "td") || is(name, "th")) {
        if (x->cells++ && (x->pend.n || x->r->t.n)) {
            flush(x);
            text(x, "  \xC2\xB7  ", 6);              /* a middle dot between cells */
        }
        if (is(name, "th")) x->bold++;
        return;
    }
    if (is(name, "b") || is(name, "strong")) {
        flush(x);
        x->bold++;
        return;
    }
    if (is(name, "i") || is(name, "em") || is(name, "cite") || is(name, "var")) {
        flush(x);
        x->ital++;
        return;
    }
    if (is(name, "u") || is(name, "ins")) {
        flush(x);
        x->under++;
        return;
    }
    if (is(name, "code") || is(name, "tt") || is(name, "kbd") || is(name, "samp")) {
        flush(x);
        x->code++;
        return;
    }
    if (is(name, "a")) {
        char href[600];
        flush(x);
        if (attr(t, tn, "href", href, sizeof href) && href[0] && !cr_istarts(href, "javascript:") && href[0] != '#') {
            x->link = cr_rt_link(x->r, href, strlen(href));
        }
        return;
    }
    if (is(name, "img")) {
        char src[400], alt[200], w[12], h[12];
        attr(t, tn, "src", src, sizeof src);
        attr(t, tn, "alt", alt, sizeof alt);
        attr(t, tn, "width", w, sizeof w);
        attr(t, tn, "height", h, sizeof h);
        if (cr_istarts(src, "cid:")) {
            for (int i = 0; i < x->m->natt; i++) {
                if (x->m->att[i].cid[0] && !strcmp(x->m->att[i].cid, src + 4)) {
                    x->m->att[i].inl = true;
                    flush(x);
                    cr_rt_img(x->r, i);
                    block(x, -1, 0, NULL);
                    return;
                }
            }
            /* named before its part was seen: look for it after the walk (cr_mime.c) */
            return;
        }
        if ((w[0] && atoi(w) <= 2) || (h[0] && atoi(h) <= 2)) return;      /* a tracker */
        x->m->remote_img = true;
        if (alt[0]) {
            flush(x);
            int st = x->ital;
            x->ital = 1;
            text(x, "[", 1);
            text_ent(x, alt, strlen(alt));
            text(x, "]", 1);
            flush(x);
            x->ital = st;
        }
        return;
    }
}

static void tag_close(hx_t *x, const char *name)
{
    if (is(name, "style") || is(name, "script") || is(name, "head") || is(name, "title") || is(name, "noscript") ||
        is(name, "template") || is(name, "xml") || is(name, "svg")) {
        if (x->skip) x->skip--;
        return;
    }
    if (x->skip) return;
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        if (x->head) x->head--;
        block(x, -1, 0, NULL);
        return;
    }
    if (is(name, "p") || is(name, "div") || is(name, "li") || is(name, "table") || is(name, "section") ||
        is(name, "article") || is(name, "center") || is(name, "dd") || is(name, "dt") || is(name, "header") ||
        is(name, "footer") || is(name, "figure") || is(name, "address") || is(name, "nav") || is(name, "aside")) {
        block(x, -1, 0, NULL);
        return;
    }
    if (is(name, "ul") || is(name, "ol") || is(name, "menu")) {
        if (x->nlist) x->nlist--;
        block(x, -1, 0, NULL);
        return;
    }
    if (is(name, "blockquote")) {
        if (x->quote) x->quote--;
        block(x, -1, 0, NULL);
        return;
    }
    if (is(name, "pre")) {
        if (x->pre) x->pre--;
        block(x, -1, 0, NULL);
        return;
    }
    if (is(name, "th")) {
        flush(x);
        if (x->bold) x->bold--;
        return;
    }
    if (is(name, "b") || is(name, "strong")) {
        flush(x);
        if (x->bold) x->bold--;
        return;
    }
    if (is(name, "i") || is(name, "em") || is(name, "cite") || is(name, "var")) {
        flush(x);
        if (x->ital) x->ital--;
        return;
    }
    if (is(name, "u") || is(name, "ins")) {
        flush(x);
        if (x->under) x->under--;
        return;
    }
    if (is(name, "code") || is(name, "tt") || is(name, "kbd") || is(name, "samp")) {
        flush(x);
        if (x->code) x->code--;
        return;
    }
    if (is(name, "a")) {
        flush(x);
        x->link = -1;
        return;
    }
}

void cr_html_render(cr_rt_t *r, const char *s, size_t n, cr_msg_t *m)
{
    hx_t x;
    memset(&x, 0, sizeof x);
    x.r = r;
    x.m = m;
    x.link = -1;
    x.last_space = true;
    block(&x, -1, 0, NULL);
    size_t i = 0, from = 0;
    while (i < n) {
        if (s[i] != '<') {
            i++;
            continue;
        }
        text_ent(&x, s + from, i - from);
        if (i + 3 < n && !memcmp(s + i, "<!--", 4)) {
            const char *e = cr_imem(s + i + 4, n - i - 4, "-->");
            i = e ? (size_t)(e - s) + 3 : n;
            from = i;
            continue;
        }
        /* the tag's end, quotes respected */
        size_t j = i + 1;
        char q = 0;
        while (j < n && (q || s[j] != '>')) {
            if (q && s[j] == q) q = 0;
            else if (!q && (s[j] == '"' || s[j] == '\'') && j > i + 1 && (s[j - 1] == '=' || cr_space((unsigned char)s[j - 1])))
                q = s[j];
            j++;
        }
        const char *t = s + i + 1;
        size_t tn = j - i - 1;
        bool closing = tn && t[0] == '/';
        if (closing) {
            t++;
            tn--;
        }
        if (tn && (t[0] == '!' || t[0] == '?')) {
            i = j + 1;
            from = i;
            continue;
        }
        char name[16];
        size_t k = 0;
        while (k < tn && k + 1 < sizeof name && !cr_space((unsigned char)t[k]) && t[k] != '/' && t[k] != '>') {
            name[k] = (char)cr_lower((unsigned char)t[k]);
            k++;
        }
        name[k] = '\0';
        /* "<" followed by something that is no tag is text */
        if (!k || !((name[0] >= 'a' && name[0] <= 'z'))) {
            text(&x, "<", 1);
            i++;
            from = i;
            continue;
        }
        bool self_close = tn && t[tn - 1] == '/';
        if (closing) tag_close(&x, name);
        else tag_open(&x, name, t + k, tn - k, self_close);
        i = j + 1;
        from = i;
    }
    if (from < n) text_ent(&x, s + from, n - from);
    flush(&x);
    cr_rt_end(r);
    r->type = CR_B_PARA;
    r->lvl = 0;
    cr_buf_free(&x.pend);
}
