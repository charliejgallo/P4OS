/*
 * P4OS - Correo: memory, text and the encodings mail is written in (cr.h).
 *
 * Mail is old and was written by many hands, so everything here is lenient
 * on the way in and strict on the way out: a charset that is not known is
 * read as Latin-1, a base64 with junk in it skips the junk, an address with
 * a comment still gives its address; and whatever comes out is valid UTF-8.
 */
#include "cr.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN) && !defined(CR_HOST_TEST)
#include "esp_heap_caps.h"
#define CR_ON_BOARD 1
#endif

/* ---- memory ------------------------------------------------------------------------------------ */

void *cr_alloc(size_t n)
{
    if (!n) n = 1;
#ifdef CR_ON_BOARD
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    void *p = malloc(n);
#endif
    if (p) memset(p, 0, n);
    return p;
}

void *cr_realloc(void *p, size_t n)
{
    if (!n) n = 1;
#ifdef CR_ON_BOARD
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return realloc(p, n);
#endif
}

void cr_free(void *p)
{
#ifdef CR_ON_BOARD
    heap_caps_free(p);
#else
    free(p);
#endif
}

char *cr_strndup(const char *s, size_t n)
{
    char *p = cr_alloc(n + 1);
    if (p && n) memcpy(p, s, n);
    return p;
}

char *cr_strdup(const char *s)
{
    return cr_strndup(s ? s : "", s ? strlen(s) : 0);
}

void cr_copy(char *dst, size_t n, const char *src)
{
    if (!n) return;
    size_t l = src ? strlen(src) : 0;
    cr_ncopy(dst, n, src ? src : "", l);
}

/* Copies at most len bytes, cutting at a UTF-8 boundary so a name cut short
 * never ends in half a letter. */
void cr_ncopy(char *dst, size_t n, const char *src, size_t len)
{
    if (!n) return;
    if (len >= n) {
        len = n - 1;
        while (len && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    }
    memmove(dst, src, len);
    dst[len] = '\0';
}

int cr_lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

bool cr_space(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

bool cr_istarts(const char *s, const char *p)
{
    while (*p) {
        if (cr_lower((unsigned char)*s++) != cr_lower((unsigned char)*p++)) return false;
    }
    return true;
}

const char *cr_imem(const char *h, size_t n, const char *needle)
{
    size_t k = strlen(needle);
    if (!k) return h;
    for (size_t i = 0; i + k <= n; i++) {
        size_t j = 0;
        while (j < k && cr_lower((unsigned char)h[i + j]) == cr_lower((unsigned char)needle[j])) j++;
        if (j == k) return h + i;
    }
    return NULL;
}

const char *cr_istr(const char *h, const char *needle)
{
    return cr_imem(h, strlen(h), needle);
}

void cr_trim(char *s)
{
    size_t n = strlen(s);
    while (n && cr_space((unsigned char)s[n - 1])) s[--n] = '\0';
    size_t i = 0;
    while (cr_space((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

uint32_t cr_hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h ? h : 1;
}

/* ---- buffers ----------------------------------------------------------------------------------- */

void cr_buf_add(cr_buf_t *b, const void *d, size_t n)
{
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->n + n + 1) c *= 2;
        char *p = cr_realloc(b->p, c);
        if (!p) {
            b->oom = true;
            return;
        }
        b->p = p;
        b->cap = c;
    }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = '\0';
}

void cr_buf_str(cr_buf_t *b, const char *s)
{
    cr_buf_add(b, s, strlen(s));
}

void cr_buf_printf(cr_buf_t *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    if ((size_t)k < sizeof tmp) {
        cr_buf_add(b, tmp, (size_t)k);
        return;
    }
    char *big = cr_alloc((size_t)k + 1);
    if (!big) {
        b->oom = true;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)k + 1, fmt, ap);
    va_end(ap);
    cr_buf_add(b, big, (size_t)k);
    cr_free(big);
}

void cr_buf_free(cr_buf_t *b)
{
    cr_free(b->p);
    memset(b, 0, sizeof *b);
}

char *cr_buf_take(cr_buf_t *b)
{
    if (!b->p) cr_buf_add(b, "", 0);
    char *p = b->p;
    memset(b, 0, sizeof *b);
    return p;
}

/* ---- the arena --------------------------------------------------------------------------------- */

struct cr_chunk {
    cr_chunk_t *next;
    size_t      used, cap;
    char        data[];
};

void *cr_arena_alloc(cr_arena_t *a, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    cr_chunk_t *c = a->head;
    if (!c || c->used + n > c->cap) {
        size_t cap = n > 32768 ? n : 32768;
        c = cr_alloc(sizeof(cr_chunk_t) + cap);
        if (!c) return NULL;
        c->cap = cap;
        c->next = a->head;
        a->head = c;
    }
    void *p = c->data + c->used;
    c->used += n;
    return p;
}

char *cr_arena_strn(cr_arena_t *a, const char *s, size_t n)
{
    char *p = cr_arena_alloc(a, n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void cr_arena_free(cr_arena_t *a)
{
    cr_chunk_t *c = a->head;
    while (c) {
        cr_chunk_t *n = c->next;
        cr_free(c);
        c = n;
    }
    a->head = NULL;
}

/* ---- base64 and quoted-printable --------------------------------------------------------------- */

static int b64v(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_' || c == ',') return 63;
    return -1;
}

size_t cr_b64_stream(cr_b64s_t *s, uint8_t *out, const char *in, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64v((unsigned char)in[i]);
        if (v < 0) continue;            /* line breaks, '=', junk */
        s->acc = (s->acc << 6) | (uint32_t)v;
        s->bits += 6;
        if (s->bits >= 8) {
            s->bits -= 8;
            out[o++] = (uint8_t)(s->acc >> s->bits);
        }
    }
    return o;
}

size_t cr_b64_dec(char *out, const char *in, size_t n)
{
    cr_b64s_t s = {0};
    /* '=' ends the data: what follows a padded group is not more of it */
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '=') {
            s.acc = 0;
            s.bits = 0;
            continue;
        }
        o += cr_b64_stream(&s, (uint8_t *)out + o, in + i, 1);
    }
    return o;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = cr_lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

size_t cr_qp_dec(char *out, const char *in, size_t n, bool header)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '=' && i + 1 < n) {
            if (in[i + 1] == '\n') {          /* soft line break */
                i++;
                continue;
            }
            if (in[i + 1] == '\r' && i + 2 < n && in[i + 2] == '\n') {
                i += 2;
                continue;
            }
            if (i + 2 < n) {
                int a = hexv((unsigned char)in[i + 1]), b = hexv((unsigned char)in[i + 2]);
                if (a >= 0 && b >= 0) {
                    out[o++] = (char)(a * 16 + b);
                    i += 2;
                    continue;
                }
            }
            /* an '=' that is not an escape stays as it is */
        } else if (c == '_' && header) {
            c = ' ';
        } else if (c == '=' && i + 1 == n) {
            continue;
        }
        out[o++] = c;
    }
    return o;
}

void cr_b64_enc(cr_buf_t *b, const void *in, size_t n, int line)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *s = in;
    int col = 0;
    char q[4];
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16;
        if (i + 1 < n) v |= (uint32_t)s[i + 1] << 8;
        if (i + 2 < n) v |= s[i + 2];
        q[0] = T[(v >> 18) & 63];
        q[1] = T[(v >> 12) & 63];
        q[2] = i + 1 < n ? T[(v >> 6) & 63] : '=';
        q[3] = i + 2 < n ? T[v & 63] : '=';
        cr_buf_add(b, q, 4);
        col += 4;
        if (line && col >= line && i + 3 < n) {
            cr_buf_add(b, "\r\n", 2);
            col = 0;
        }
    }
}

/* Quoted-printable for a body of '\n' lines: CRLF out, lines of 76 at most
 * with soft breaks, trailing white space escaped, a lone '.' at the start
 * of a line escaped so no SMTP server takes it for the end. */
void cr_qp_enc(cr_buf_t *b, const char *in, size_t n)
{
    static const char H[] = "0123456789ABCDEF";
    int col = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\r') continue;
        if (c == '\n') {
            cr_buf_add(b, "\r\n", 2);
            col = 0;
            continue;
        }
        bool last = i + 1 == n || in[i + 1] == '\n' || in[i + 1] == '\r';
        bool esc = c == '=' || c >= 127 || (c < 32 && c != '\t') || ((c == ' ' || c == '\t') && last) ||
                   (c == '.' && col == 0);
        int w = esc ? 3 : 1;
        if (col + w > 75) {
            cr_buf_add(b, "=\r\n", 3);
            col = 0;
        }
        if (esc) {
            char e[3] = {'=', H[c >> 4], H[c & 15]};
            cr_buf_add(b, e, 3);
        } else {
            cr_buf_add(b, (const char *)&c, 1);
        }
        col += w;
    }
}

/* ---- charsets ---------------------------------------------------------------------------------- */

/* Windows-1252's 0x80-0x9F, which ISO-8859-1 leaves to control codes and
 * every mailer of the nineties filled with curly quotes. */
static const uint16_t CP1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152,
    0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122,
    0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

static size_t put_cp(char *o, uint32_t cp)
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

/* The length of a valid UTF-8 sequence at s, 0 if it is not one. */
static int utf8_len(const unsigned char *s, size_t n)
{
    if (s[0] < 0x80) return 1;
    int k = s[0] >= 0xF0 && s[0] <= 0xF4 ? 4 : s[0] >= 0xE0 ? 3 : s[0] >= 0xC2 && s[0] < 0xE0 ? 2 : 0;
    if (!k || (size_t)k > n) return 0;
    for (int i = 1; i < k; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
    }
    if (k == 3 && s[0] == 0xE0 && s[1] < 0xA0) return 0;
    if (k == 4 && s[0] == 0xF0 && s[1] < 0x90) return 0;
    return k;
}

bool cr_utf8_valid(const char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        int k = utf8_len((const unsigned char *)s + i, n - i);
        if (!k) return false;
        i += (size_t)k;
    }
    return true;
}

static uint32_t latin15(unsigned char c)
{
    switch (c) {
    case 0xA4: return 0x20AC;
    case 0xA6: return 0x0160;
    case 0xA8: return 0x0161;
    case 0xB4: return 0x017D;
    case 0xB8: return 0x017E;
    case 0xBC: return 0x0152;
    case 0xBD: return 0x0153;
    case 0xBE: return 0x0178;
    }
    return c;
}

char *cr_to_utf8(const char *in, size_t n, const char *cs, size_t *out_n)
{
    char *o = cr_alloc(n * 3 + 1);
    if (!o) return NULL;
    size_t k = 0;
    const unsigned char *s = (const unsigned char *)in;
    bool utf8 = !cs || !cs[0] || cr_istarts(cs, "utf-8") || cr_istarts(cs, "utf8") || cr_istarts(cs, "us-ascii") ||
                cr_istarts(cs, "ascii");
    bool w1252 = cs && (cr_istarts(cs, "windows-1252") || cr_istarts(cs, "cp1252") || cr_istarts(cs, "x-cp1252"));
    bool l15 = cs && (cr_istarts(cs, "iso-8859-15") || cr_istarts(cs, "latin-9") || cr_istarts(cs, "latin9"));
    /* A message that says ISO-8859-1 is very often Windows-1252 in truth
     * (that is what the browsers do too): read 0x80-0x9F that way. */
    if (cs && (cr_istarts(cs, "iso-8859-1") || cr_istarts(cs, "latin1") || cr_istarts(cs, "iso_8859-1")) &&
        !l15) {
        w1252 = true;
    }
    for (size_t i = 0; i < n;) {
        if (utf8) {
            int l = utf8_len(s + i, n - i);
            if (l) {
                memcpy(o + k, s + i, (size_t)l);
                k += (size_t)l;
                i += (size_t)l;
                continue;
            }
            /* not UTF-8 after all: this byte as Windows-1252 */
        }
        unsigned char c = s[i++];
        uint32_t cp = c;
        if (c >= 0x80 && c < 0xA0 && (w1252 || utf8)) cp = CP1252[c - 0x80];
        else if (l15) cp = latin15(c);
        if (cp == 0) continue;
        k += put_cp(o + k, cp);
    }
    o[k] = '\0';
    if (out_n) *out_n = k;
    return o;
}

/* ---- RFC 2047 ---------------------------------------------------------------------------------- */

void cr_hdr_decode(char *out, size_t on, const char *in, size_t n)
{
    cr_buf_t res = {0};
    cr_buf_t run = {0};          /* bytes of consecutive encoded words in one charset */
    char run_cs[32] = "";
    bool last_word = false;
    size_t ws_start = 0;
    size_t i = 0;
    while (i < n) {
        if (in[i] == '=' && i + 1 < n && in[i + 1] == '?') {
            /* =?charset?enc?text?= */
            size_t p = i + 2, q1 = p;
            while (q1 < n && in[q1] != '?') q1++;
            size_t q2 = q1 + 2;
            if (q1 + 2 < n && in[q1 + 2] == '?') {
                size_t e = q2 + 1;
                while (e + 1 < n && !(in[e] == '?' && in[e + 1] == '=')) e++;
                if (e + 1 < n) {
                    char cs[32];
                    cr_ncopy(cs, sizeof cs, in + p, q1 - p);
                    char *star = strchr(cs, '*');           /* RFC 2231 language */
                    if (star) *star = '\0';
                    char enc = (char)cr_lower((unsigned char)in[q1 + 1]);
                    const char *t = in + q2 + 1;
                    size_t tn = e - (q2 + 1);
                    char *dec = cr_alloc(tn + 1);
                    size_t dn = enc == 'b' ? cr_b64_dec(dec, t, tn) : cr_qp_dec(dec, t, tn, true);
                    /* the white space between two encoded words is not text */
                    if (!last_word && ws_start < i) {
                        if (run.n) {
                            size_t un;
                            char *u = cr_to_utf8(run.p, run.n, run_cs, &un);
                            if (u) cr_buf_add(&res, u, un);
                            cr_free(u);
                            run.n = 0;
                        }
                    }
                    if (run.n && strcasecmp(run_cs, cs) != 0) {
                        size_t un;
                        char *u = cr_to_utf8(run.p, run.n, run_cs, &un);
                        if (u) cr_buf_add(&res, u, un);
                        cr_free(u);
                        run.n = 0;
                    }
                    cr_copy(run_cs, sizeof run_cs, cs);
                    cr_buf_add(&run, dec, dn);
                    cr_free(dec);
                    i = e + 2;
                    last_word = true;
                    ws_start = i;
                    continue;
                }
            }
        }
        if (cr_space((unsigned char)in[i]) && last_word) {
            /* maybe the gap between two words: hold it */
            size_t j = i;
            while (j < n && cr_space((unsigned char)in[j])) j++;
            if (j + 1 < n && in[j] == '=' && in[j + 1] == '?') {
                i = j;
                continue;
            }
        }
        if (run.n) {
            size_t un;
            char *u = cr_to_utf8(run.p, run.n, run_cs, &un);
            if (u) cr_buf_add(&res, u, un);
            cr_free(u);
            run.n = 0;
        }
        last_word = false;
        /* a raw 8-bit header: UTF-8 if it is, else Windows-1252 */
        size_t j = i;
        while (j < n && !(in[j] == '=' && j + 1 < n && in[j + 1] == '?')) j++;
        if (j == i) j = i + 1;
        size_t un;
        char *u = cr_to_utf8(in + i, j - i, "utf-8", &un);
        if (u) {
            /* unfold: CR and LF go, tabs become spaces */
            for (size_t k = 0; k < un; k++) {
                char c = u[k];
                if (c == '\r' || c == '\n') continue;
                if (c == '\t') c = ' ';
                cr_buf_add(&res, &c, 1);
            }
        }
        cr_free(u);
        i = j;
        ws_start = i;
    }
    if (run.n) {
        size_t un;
        char *u = cr_to_utf8(run.p, run.n, run_cs, &un);
        if (u) cr_buf_add(&res, u, un);
        cr_free(u);
    }
    cr_buf_free(&run);
    /* control characters out, runs of spaces to one */
    size_t k = 0;
    bool sp = false;
    for (size_t r = 0; r < res.n && k + 1 < on; r++) {
        unsigned char c = (unsigned char)res.p[r];
        if (c < 32) c = ' ';
        if (c == ' ') {
            if (sp || !k) continue;
            sp = true;
        } else {
            sp = false;
        }
        out[k++] = (char)c;
    }
    while (k && out[k - 1] == ' ') k--;
    if (on) {
        out[k] = '\0';
        /* never end in half a letter */
        while (k && !cr_utf8_valid(out, k)) out[--k] = '\0';
    }
    cr_buf_free(&res);
}

static bool all_ascii(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x80) return false;
    }
    return true;
}

void cr_hdr_encode(cr_buf_t *b, const char *in, bool phrase)
{
    if (all_ascii(in)) {
        if (phrase && in[strcspn(in, "()<>[]:;@\\,.\"")]) {
            cr_buf_add(b, "\"", 1);
            for (const char *p = in; *p; p++) {
                if (*p == '"' || *p == '\\') cr_buf_add(b, "\\", 1);
                cr_buf_add(b, p, 1);
            }
            cr_buf_add(b, "\"", 1);
        } else {
            cr_buf_str(b, in);
        }
        return;
    }
    /* base64 words of at most 45 bytes (60 characters inside =?UTF-8?B?...?=),
     * cut between letters, folded onto lines of their own */
    size_t n = strlen(in), i = 0;
    bool first = true;
    while (i < n) {
        size_t take = n - i > 45 ? 45 : n - i;
        while (take < n - i && take && ((unsigned char)in[i + take] & 0xC0) == 0x80) take--;
        if (!first) cr_buf_add(b, "\r\n ", 3);
        cr_buf_str(b, "=?UTF-8?B?");
        cr_b64_enc(b, in + i, take, 0);
        cr_buf_str(b, "?=");
        i += take;
        first = false;
    }
}

/* ---- dates ------------------------------------------------------------------------------------- */

static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static const char *const MON_EN[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                       "jul", "aug", "sep", "oct", "nov", "dec"};

static int month_of(const char *s)
{
    for (int i = 0; i < 12; i++) {
        if (cr_istarts(s, MON_EN[i])) return i + 1;
    }
    return 0;
}

static uint32_t mk(int y, int mo, int d, int h, int mi, int s, int tz_min)
{
    if (y < 100) y += y < 70 ? 2000 : 1900;
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    /* unsigned, so it goes on past 2038 */
    int32_t days = days_from_civil(y, (uint32_t)mo, (uint32_t)d);
    uint32_t t = (uint32_t)days * 86400u + (uint32_t)(h * 3600 + mi * 60 + s);
    return t - (uint32_t)(tz_min * 60);
}

static int zone(const char *p)
{
    while (*p == ' ') p++;
    if (*p == '+' || *p == '-') {
        int v = atoi(p + 1);
        int m = (v / 100) * 60 + v % 100;
        return *p == '-' ? -m : m;
    }
    static const struct {
        const char *z;
        int m;
    } Z[] = {{"EDT", -240}, {"EST", -300}, {"CDT", -300}, {"CST", -360}, {"MDT", -360},
             {"MST", -420}, {"PDT", -420}, {"PST", -480}, {"CET", 60},   {"CEST", 120}};
    for (size_t i = 0; i < sizeof Z / sizeof Z[0]; i++) {
        if (cr_istarts(p, Z[i].z)) return Z[i].m;
    }
    return 0;                          /* GMT, UT, Z, or a guess */
}

uint32_t cr_date_parse(const char *s)
{
    /* [Wkd,] DD Mon YYYY HH:MM[:SS] zone */
    const char *p = s;
    while (*p && (*p < '0' || *p > '9')) p++;
    int d = atoi(p);
    while (*p >= '0' && *p <= '9') p++;
    while (*p == ' ' || *p == '-') p++;
    int mo = month_of(p);
    if (!mo) return 0;
    while (*p && *p != ' ' && *p != '-') p++;
    while (*p == ' ' || *p == '-') p++;
    int y = atoi(p);
    while (*p >= '0' && *p <= '9') p++;
    while (*p == ' ') p++;
    int h = atoi(p);
    while (*p >= '0' && *p <= '9') p++;
    int mi = 0, sec = 0;
    if (*p == ':') {
        mi = atoi(++p);
        while (*p >= '0' && *p <= '9') p++;
        if (*p == ':') {
            sec = atoi(++p);
            while (*p >= '0' && *p <= '9') p++;
        }
    }
    return mk(y, mo, d, h, mi, sec, zone(p));
}

uint32_t cr_idate_parse(const char *s)
{
    return cr_date_parse(s);
}

uint32_t cr_now(void)
{
    if (!aos_hal_time_is_valid()) return 0;
    time_t t = time(NULL);
    return t > 0 ? (uint32_t)t : 0;
}

void cr_date_rfc(char *out, size_t n, uint32_t t)
{
    static const char *const WD[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const MO[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t tt = (time_t)t;
    struct tm lt, ut;
    localtime_r(&tt, &lt);
    gmtime_r(&tt, &ut);
    /* the offset, from the two broken-down times */
    int off = (lt.tm_hour - ut.tm_hour) * 60 + (lt.tm_min - ut.tm_min);
    int dd = lt.tm_yday - ut.tm_yday;
    if (dd > 1) dd = -1;
    if (dd < -1) dd = 1;
    off += dd * 1440;
    snprintf(out, n, "%s, %d %s %04d %02d:%02d:%02d %c%02d%02d", WD[lt.tm_wday % 7], lt.tm_mday,
             MO[lt.tm_mon % 12], lt.tm_year + 1900, lt.tm_hour, lt.tm_min, lt.tm_sec, off < 0 ? '-' : '+',
             abs(off) / 60, abs(off) % 60);
}

static const char *const MON_ES[12] = {N_("ene"), N_("feb"), N_("mar"), N_("abr"), N_("may"), N_("jun"),
                                       N_("jul"), N_("ago"), N_("sep"), N_("oct"), N_("nov"), N_("dic")};
static const char *const WD_ES[7] = {N_("dom"), N_("lun"), N_("mar (día)"), N_("mié"),
                                     N_("jue"), N_("vie"), N_("sáb")};

static const char *wd_name(int i)
{
    const char *s = _(WD_ES[i % 7]);
    /* "mar (día)" is the key for Tuesday, so it does not collide with March */
    if (!strcmp(s, "mar (día)")) return "mar";
    return s;
}

void cr_date_short(char *out, size_t n, uint32_t t)
{
    if (!t) {
        cr_copy(out, n, "");
        return;
    }
    time_t tt = (time_t)t, now = (time_t)cr_now();
    struct tm a, b;
    localtime_r(&tt, &a);
    if (!now) {
        snprintf(out, n, "%d %s", a.tm_mday, _(MON_ES[a.tm_mon % 12]));
        return;
    }
    localtime_r(&now, &b);
    int32_t da = days_from_civil(a.tm_year + 1900, (uint32_t)a.tm_mon + 1, (uint32_t)a.tm_mday);
    int32_t db = days_from_civil(b.tm_year + 1900, (uint32_t)b.tm_mon + 1, (uint32_t)b.tm_mday);
    if (da == db) snprintf(out, n, "%02d:%02d", a.tm_hour, a.tm_min);
    else if (da == db - 1) cr_copy(out, n, _("ayer"));
    else if (db - da < 7 && da < db) cr_copy(out, n, wd_name(a.tm_wday));
    else if (a.tm_year == b.tm_year) snprintf(out, n, "%d %s", a.tm_mday, _(MON_ES[a.tm_mon % 12]));
    else snprintf(out, n, "%02d/%02d/%02d", a.tm_mday, a.tm_mon + 1, a.tm_year % 100);
}

void cr_date_long(char *out, size_t n, uint32_t t)
{
    if (!t) {
        cr_copy(out, n, "");
        return;
    }
    time_t tt = (time_t)t;
    struct tm a;
    localtime_r(&tt, &a);
    snprintf(out, n, "%s %d %s %d, %02d:%02d", wd_name(a.tm_wday), a.tm_mday, _(MON_ES[a.tm_mon % 12]),
             a.tm_year + 1900, a.tm_hour, a.tm_min);
}

/* ---- addresses --------------------------------------------------------------------------------- */

bool cr_addr_valid(const char *a)
{
    const char *at = strchr(a, '@');
    if (!at || at == a || !at[1] || strchr(at + 1, '@')) return false;
    if (!strchr(at + 1, '.')) return false;
    for (const char *p = a; *p; p++) {
        if (cr_space((unsigned char)*p) || *p == '<' || *p == '>' || *p == ',' || *p == '"') return false;
    }
    return true;
}

/* One address of a list: "Name <a@b>", "a@b (Name)", "\"Last, First\" <a@b>". */
static void one_addr(const char *s, size_t n, cr_addr_t *a)
{
    memset(a, 0, sizeof *a);
    const char *lt = NULL, *gt = NULL;
    bool q = false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '"' && (i == 0 || s[i - 1] != '\\')) q = !q;
        else if (!q && s[i] == '<') lt = s + i;
        else if (!q && s[i] == '>' && lt) gt = s + i;
    }
    char name[200] = "";
    if (lt && gt && gt > lt) {
        cr_ncopy(a->addr, sizeof a->addr, lt + 1, (size_t)(gt - lt - 1));
        cr_ncopy(name, sizeof name, s, (size_t)(lt - s));
    } else {
        /* a bare address, maybe with a comment */
        const char *par = memchr(s, '(', n);
        size_t an = par ? (size_t)(par - s) : n;
        cr_ncopy(a->addr, sizeof a->addr, s, an);
        if (par) {
            const char *cl = memchr(par, ')', n - (size_t)(par - s));
            cr_ncopy(name, sizeof name, par + 1, cl ? (size_t)(cl - par - 1) : n - (size_t)(par - s) - 1);
        }
    }
    cr_trim(a->addr);
    /* the name: quotes and backslashes out, words decoded */
    char raw[200];
    size_t k = 0;
    for (size_t i = 0; name[i] && k + 1 < sizeof raw; i++) {
        if (name[i] == '\\' && name[i + 1]) {
            raw[k++] = name[++i];
            continue;
        }
        if (name[i] == '"') continue;
        raw[k++] = name[i];
    }
    raw[k] = '\0';
    cr_trim(raw);
    cr_hdr_decode(a->name, sizeof a->name, raw, strlen(raw));
    /* "a@b" <a@b>: the name says nothing */
    if (!strcasecmp(a->name, a->addr)) a->name[0] = '\0';
}

int cr_addr_parse(const char *in, cr_addr_t *out, int max)
{
    int k = 0;
    const char *p = in, *start = in;
    bool q = false;
    int angle = 0;
    for (;; p++) {
        char c = *p;
        if (c == '"' && (p == in || p[-1] != '\\')) q = !q;
        else if (!q && c == '<') angle++;
        else if (!q && c == '>' && angle) angle--;
        if (!c || (!q && !angle && (c == ',' || c == ';'))) {
            const char *s = start;
            size_t n = (size_t)(p - s);
            /* a group's "name:" is dropped */
            const char *colon = NULL;
            bool qq = false;
            for (size_t i = 0; i < n; i++) {
                if (s[i] == '"') qq = !qq;
                if (!qq && s[i] == '<') break;
                if (!qq && s[i] == ':') colon = s + i;
            }
            if (colon) {
                n -= (size_t)(colon + 1 - s);
                s = colon + 1;
            }
            while (n && cr_space((unsigned char)*s)) {
                s++;
                n--;
            }
            if (n && k < max) {
                one_addr(s, n, &out[k]);
                if (out[k].addr[0]) k++;
            }
            if (!c) break;
            start = p + 1;
        }
    }
    return k;
}

void cr_addr_show(char *out, size_t n, const cr_addr_t *a)
{
    cr_copy(out, n, a->name[0] ? a->name : a->addr);
}

/* ---- files ------------------------------------------------------------------------------------- */

char *cr_file_read(const char *path, size_t *n, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) len = 0;
    if (max && (size_t)len > max) len = (long)max;
    char *p = cr_alloc((size_t)len + 1);
    if (!p) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(p, 1, (size_t)len, f);
    fclose(f);
    p[got] = '\0';
    if (n) *n = got;
    return p;
}

bool cr_file_write(const char *path, const void *d, size_t n)
{
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(d, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    unlink(path);
    return rename(tmp, path) == 0;
}

bool cr_mkdirs(const char *path)
{
    char p[300];
    cr_copy(p, sizeof p, path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            mkdir(p, 0775);
            *s = '/';
        }
    }
    mkdir(p, 0775);
    struct stat st;
    return stat(p, &st) == 0;
}

uint32_t cr_file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
}

void cr_rmtree(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        char sub[320];
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            struct stat st;
            if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) cr_rmtree(sub);
            else unlink(sub);
        }
        closedir(d);
    }
    remove(path);
}

/* ---- JSON -------------------------------------------------------------------------------------- */

void cr_json_str(cr_buf_t *b, const char *s)
{
    static const char H[] = "0123456789abcdef";
    cr_buf_add(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            char e[2] = {'\\', (char)*p};
            cr_buf_add(b, e, 2);
        } else if (*p == '\n') {
            cr_buf_add(b, "\\n", 2);
        } else if (*p < 32) {
            char e[6] = {'\\', 'u', '0', '0', H[*p >> 4], H[*p & 15]};
            cr_buf_add(b, e, 6);
        } else {
            cr_buf_add(b, (const char *)p, 1);
        }
    }
    cr_buf_add(b, "\"", 1);
}

/* ---- modified UTF-7 (RFC 3501, 5.1.3) ---------------------------------------------------------- */

void cr_mutf7_dec(char *out, size_t n, const char *in)
{
    size_t k = 0;
    for (const char *p = in; *p && k + 5 < n;) {
        if (*p != '&') {
            out[k++] = *p++;
            continue;
        }
        p++;
        if (*p == '-') {
            out[k++] = '&';
            p++;
            continue;
        }
        uint32_t acc = 0;
        int bits = 0;
        uint32_t hi = 0;
        while (*p && *p != '-' && k + 5 < n) {
            int v = b64v((unsigned char)*p++);
            if (v < 0) continue;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 16) {
                bits -= 16;
                uint32_t u = (acc >> bits) & 0xFFFF;
                if (u >= 0xD800 && u < 0xDC00) {
                    hi = u;
                    continue;
                }
                if (u >= 0xDC00 && u < 0xE000 && hi) {
                    u = 0x10000 + ((hi - 0xD800) << 10) + (u - 0xDC00);
                    hi = 0;
                }
                k += put_cp(out + k, u);
            }
        }
        if (*p == '-') p++;
    }
    out[k] = '\0';
}

void cr_mutf7_enc(char *out, size_t n, const char *in)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";
    size_t k = 0;
    const unsigned char *s = (const unsigned char *)in;
    while (*s && k + 8 < n) {
        if (*s >= 0x20 && *s < 0x7F) {
            if (*s == '&') {
                out[k++] = '&';
                out[k++] = '-';
            } else {
                out[k++] = (char)*s;
            }
            s++;
            continue;
        }
        out[k++] = '&';
        uint32_t acc = 0;
        int bits = 0;
        while (*s && (*s < 0x20 || *s >= 0x7F) && k + 8 < n) {
            uint32_t cp;
            int l = utf8_len(s, 4);
            if (l == 1) cp = *s;
            else if (l == 2) cp = ((s[0] & 31u) << 6) | (s[1] & 63u);
            else if (l == 3) cp = ((s[0] & 15u) << 12) | ((s[1] & 63u) << 6) | (s[2] & 63u);
            else if (l == 4) cp = ((s[0] & 7u) << 18) | ((s[1] & 63u) << 12) | ((s[2] & 63u) << 6) | (s[3] & 63u);
            else {
                cp = '?';
                l = 1;
            }
            s += l;
            uint16_t u[2];
            int nu = 1;
            if (cp >= 0x10000) {
                cp -= 0x10000;
                u[0] = (uint16_t)(0xD800 + (cp >> 10));
                u[1] = (uint16_t)(0xDC00 + (cp & 0x3FF));
                nu = 2;
            } else {
                u[0] = (uint16_t)cp;
            }
            for (int i = 0; i < nu; i++) {
                acc = (acc << 16) | u[i];
                bits += 16;
                while (bits >= 6 && k + 4 < n) {
                    bits -= 6;
                    out[k++] = T[(acc >> bits) & 63];
                }
            }
        }
        if (bits && k + 3 < n) out[k++] = T[(acc << (6 - bits)) & 63];
        out[k++] = '-';
    }
    out[k] = '\0';
}
