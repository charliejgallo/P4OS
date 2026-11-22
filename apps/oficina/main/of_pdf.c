/*
 * P4OS - Office: PDF, the file (of_pdf.h). Objects, cross-reference tables
 * and streams (with /Prev chains and hybrid files), object streams, the
 * filters that carry content (Flate, LZW, ASCII85, ASCIIHex, RunLength and
 * the PNG and TIFF predictors), the standard security handler (RC4 40 to
 * 128 bits, AES-128, AES-256 of revisions 5 and 6; the user or the owner
 * password), and a rebuild of the table from the objects themselves when
 * it is damaged. The pages and their text are of_pdftext.c's.
 *
 * Tolerant on purpose: a stream with a wrong /Length is cut at its
 * "endstream", a broken Flate stream gives what came out, an object that
 * is not where the table says is looked for.
 */
#include "of_pdf.h"
#include "of_crypt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 48

/* -------------------------------------------------------------------------- */
/* Values                                                                      */
/* -------------------------------------------------------------------------- */

/* a number from the file as an integer, never out of range */
static inline int pvi(const pv_t *v, double def) { double d = pv_num(v, def); return d > -1.0e9 && d < 1.0e9 ? (int)d : 0; }

void pv_free(pv_t *v)
{
    if (!v) return;
    switch (v->t) {
    case PV_STR: case PV_NAME: case PV_OP: of_free(v->s.s); break;
    case PV_ARR:
        for (int i = 0; i < v->a.n; i++) pv_free(&v->a.v[i]);
        of_free(v->a.v);
        break;
    case PV_DICT: case PV_STREAM:
        for (int i = 0; i < v->d.n; i++) { of_free(v->d.k[i]); pv_free(&v->d.v[i]); }
        of_free(v->d.k);
        of_free(v->d.v);
        break;
    default: break;
    }
    v->t = PV_NULL;
}

pv_t *pv_get(const pv_t *d, const char *key)
{
    if (!d || (d->t != PV_DICT && d->t != PV_STREAM)) return NULL;
    for (int i = 0; i < d->d.n; i++)
        if (!strcmp(d->d.k[i], key)) return &d->d.v[i];
    return NULL;
}

double pv_num(const pv_t *v, double def)
{
    if (!v) return def;
    if (v->t == PV_INT) return (double)v->i;
    if (v->t == PV_REAL) return v->r;
    return def;
}

bool pv_is_name(const pv_t *v, const char *n)
{
    return v && v->t == PV_NAME && !strcmp(v->s.s, n);
}

/* -------------------------------------------------------------------------- */
/* The lexer                                                                   */
/* -------------------------------------------------------------------------- */

static bool is_ws(uint8_t c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
static bool is_delim(uint8_t c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }

void lx_ws(lx_t *L)
{
    while (L->p < L->n) {
        uint8_t c = L->b[L->p];
        if (is_ws(c)) { L->p++; continue; }
        if (c == '%') { while (L->p < L->n && L->b[L->p] != '\n' && L->b[L->p] != '\r') L->p++; continue; }
        break;
    }
}

static int hexv(uint8_t c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool parse_into(pdf_t *P, lx_t *L, pv_t *out, int depth);

static bool parse_number(lx_t *L, pv_t *out)
{
    size_t s = L->p;
    bool neg = false, real = false;
    if (L->b[L->p] == '+' || L->b[L->p] == '-') { neg = L->b[L->p] == '-'; L->p++; }
    int64_t ip = 0;
    double fr = 0, sc = 1;
    bool any = false;
    while (L->p < L->n) {
        uint8_t c = L->b[L->p];
        if (c >= '0' && c <= '9') {
            any = true;
            if (real) { sc /= 10; fr += (c - '0') * sc; }
            else if (ip < 100000000000000LL) ip = ip * 10 + (c - '0');
            L->p++;
        } else if (c == '.' && !real) {
            real = true;
            L->p++;
        } else if (c == '-' && L->p > s) {
            L->p++;             /* "1-2" written by some producers: the rest is dropped */
        } else {
            break;
        }
    }
    (void)any;
    if (real) { out->t = PV_REAL; out->r = (neg ? -1 : 1) * ((double)ip + fr); }
    else { out->t = PV_INT; out->i = neg ? -ip : ip; }
    return true;
}

static bool parse_string(lx_t *L, pv_t *out)
{
    /* (literal), escapes and balanced parentheses */
    L->p++;
    of_sb_t sb = { 0 };
    int depth = 1;
    while (L->p < L->n) {
        uint8_t c = L->b[L->p++];
        if (c == '\\' && L->p < L->n) {
            uint8_t e = L->b[L->p++];
            switch (e) {
            case 'n': of_sb_putc(&sb, '\n'); break;
            case 'r': of_sb_putc(&sb, '\r'); break;
            case 't': of_sb_putc(&sb, '\t'); break;
            case 'b': of_sb_putc(&sb, '\b'); break;
            case 'f': of_sb_putc(&sb, '\f'); break;
            case '\r': if (L->p < L->n && L->b[L->p] == '\n') L->p++; break;
            case '\n': break;
            default:
                if (e >= '0' && e <= '7') {
                    int v = e - '0';
                    for (int k = 0; k < 2 && L->p < L->n && L->b[L->p] >= '0' && L->b[L->p] <= '7'; k++) v = v * 8 + (L->b[L->p++] - '0');
                    of_sb_putc(&sb, (char)v);
                } else {
                    of_sb_putc(&sb, (char)e);
                }
            }
            continue;
        }
        if (c == '(') depth++;
        else if (c == ')' && --depth == 0) break;
        of_sb_putc(&sb, (char)c);
    }
    out->t = PV_STR;
    out->s.n = (int)sb.n;
    out->s.s = sb.s ? sb.s : of_alloc(1);
    return true;
}

static bool parse_hex(lx_t *L, pv_t *out)
{
    L->p++;
    of_sb_t sb = { 0 };
    int hi = -1;
    while (L->p < L->n && L->b[L->p] != '>') {
        int v = hexv(L->b[L->p++]);
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { of_sb_putc(&sb, (char)(hi << 4 | v)); hi = -1; }
    }
    if (hi >= 0) of_sb_putc(&sb, (char)(hi << 4));
    L->p++;
    out->t = PV_STR;
    out->s.n = (int)sb.n;
    out->s.s = sb.s ? sb.s : of_alloc(1);
    return true;
}

static bool parse_name(lx_t *L, pv_t *out)
{
    L->p++;
    of_sb_t sb = { 0 };
    while (L->p < L->n && !is_ws(L->b[L->p]) && !is_delim(L->b[L->p])) {
        uint8_t c = L->b[L->p++];
        if (c == '#' && L->p + 1 < L->n && hexv(L->b[L->p]) >= 0 && hexv(L->b[L->p + 1]) >= 0) {
            c = (uint8_t)(hexv(L->b[L->p]) << 4 | hexv(L->b[L->p + 1]));
            L->p += 2;
        }
        of_sb_putc(&sb, (char)c);
    }
    out->t = PV_NAME;
    out->s.n = (int)sb.n;
    out->s.s = sb.s ? sb.s : of_alloc(1);
    return true;
}

static bool parse_into(pdf_t *P, lx_t *L, pv_t *out, int depth)
{
    memset(out, 0, sizeof *out);
    lx_ws(L);
    if (L->p >= L->n || depth > MAX_DEPTH) return false;
    uint8_t c = L->b[L->p];
    if (c == '/') return parse_name(L, out);
    if (c == '(') return parse_string(L, out);
    if (c == '<') {
        if (L->p + 1 < L->n && L->b[L->p + 1] == '<') {
            L->p += 2;
            out->t = PV_DICT;
            int cap = 0;
            for (;;) {
                lx_ws(L);
                if (L->p >= L->n) break;
                if (L->b[L->p] == '>') { L->p += L->p + 1 < L->n && L->b[L->p + 1] == '>' ? 2 : 1; break; }
                pv_t k;
                if (L->b[L->p] != '/') {
                    /* not a key: skip a token */
                    if (!parse_into(P, L, &k, depth + 1)) break;
                    pv_free(&k);
                    continue;
                }
                parse_name(L, &k);
                pv_t v;
                if (!parse_into(P, L, &v, depth + 1)) { pv_free(&k); break; }
                if (v.t == PV_OP) {
                    /* "endobj" inside a dictionary: the dictionary was cut */
                    bool stop = !strcmp(v.s.s, "endobj") || !strcmp(v.s.s, "stream");
                    pv_free(&v);
                    pv_free(&k);
                    if (stop) break;
                    continue;
                }
                if (out->d.n == cap) {
                    cap = cap ? cap * 2 : 8;
                    char **nk = of_realloc(out->d.k, (size_t)cap * sizeof *nk);
                    pv_t *nv = of_realloc(out->d.v, (size_t)cap * sizeof *nv);
                    if (nk) out->d.k = nk;
                    if (nv) out->d.v = nv;
                    if (!nk || !nv) { pv_free(&k); pv_free(&v); break; }
                }
                out->d.k[out->d.n] = k.s.s;
                out->d.v[out->d.n] = v;
                out->d.n++;
            }
            return true;
        }
        return parse_hex(L, out);
    }
    if (c == '[') {
        L->p++;
        out->t = PV_ARR;
        int cap = 0;
        for (;;) {
            lx_ws(L);
            if (L->p >= L->n) break;
            if (L->b[L->p] == ']') { L->p++; break; }
            pv_t v;
            if (!parse_into(P, L, &v, depth + 1)) break;
            if (v.t == PV_OP && (!strcmp(v.s.s, "endobj") || !strcmp(v.s.s, "stream"))) { pv_free(&v); break; }
            if (out->a.n == cap) {
                cap = cap ? cap * 2 : 8;
                pv_t *nv = of_realloc(out->a.v, (size_t)cap * sizeof *nv);
                if (!nv) { pv_free(&v); break; }
                out->a.v = nv;
            }
            out->a.v[out->a.n++] = v;
        }
        return true;
    }
    if (c == ')' || c == '>' || c == ']' || c == '}' || c == '{') {
        L->p++;
        out->t = PV_OP;
        out->s.s = of_alloc(2);
        if (out->s.s) out->s.s[0] = (char)c;
        out->s.n = 1;
        return true;
    }
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
        parse_number(L, out);
        /* "num gen R" */
        if (out->t == PV_INT && out->i >= 0 && P) {
            size_t save = L->p;
            lx_ws(L);
            if (L->p < L->n && L->b[L->p] >= '0' && L->b[L->p] <= '9') {
                pv_t g;
                parse_number(L, &g);
                lx_ws(L);
                if (g.t == PV_INT && L->p < L->n && L->b[L->p] == 'R' && (L->p + 1 >= L->n || is_ws(L->b[L->p + 1]) || is_delim(L->b[L->p + 1]))) {
                    L->p++;
                    int num = (int)out->i;
                    out->t = PV_REF;
                    out->ref.num = num;
                    out->ref.gen = (int)g.i;
                    return true;
                }
            }
            L->p = save;
        }
        return true;
    }
    /* a keyword */
    size_t s = L->p;
    while (L->p < L->n && !is_ws(L->b[L->p]) && !is_delim(L->b[L->p])) L->p++;
    if (L->p == s) { L->p++; return parse_into(P, L, out, depth + 1); }
    size_t n = L->p - s;
    if (n == 4 && !memcmp(L->b + s, "true", 4)) { out->t = PV_BOOL; out->b = true; return true; }
    if (n == 5 && !memcmp(L->b + s, "false", 5)) { out->t = PV_BOOL; out->b = false; return true; }
    if (n == 4 && !memcmp(L->b + s, "null", 4)) { out->t = PV_NULL; return true; }
    out->t = PV_OP;
    out->s.s = of_alloc(n + 1);
    if (out->s.s) memcpy(out->s.s, L->b + s, n);
    out->s.n = (int)n;
    return true;
}

bool pv_parse(pdf_t *P, lx_t *L, pv_t *out)
{
    return parse_into(P, L, out, 0);
}

/* -------------------------------------------------------------------------- */
/* Filters                                                                     */
/* -------------------------------------------------------------------------- */

static uint8_t *unpredict(uint8_t *d, size_t *n, const pv_t *parms)
{
    int pred = pvi(pv_get(parms, "Predictor"), 1);
    if (pred < 2) return d;
    int colors = pvi(pv_get(parms, "Colors"), 1);
    int bpc = pvi(pv_get(parms, "BitsPerComponent"), 8);
    int cols = pvi(pv_get(parms, "Columns"), 1);
    if (colors < 1 || colors > 32 || bpc < 1 || bpc > 16 || cols < 1 || cols > 1000000) return d;
    size_t bpp = (size_t)((colors * bpc + 7) / 8);
    size_t row = (size_t)(colors * bpc * cols + 7) / 8;
    if (pred == 2) {
        if (bpc != 8) return d;
        for (size_t r = 0; r + row <= *n; r += row)
            for (size_t i = bpp; i < row; i++) d[r + i] = (uint8_t)(d[r + i] + d[r + i - bpp]);
        return d;
    }
    /* PNG: a filter byte before each row */
    size_t nrows = *n / (row + 1);
    uint8_t *o = of_alloc(nrows * row + 1);
    if (!o) return d;
    for (size_t r = 0; r < nrows; r++) {
        const uint8_t *in = d + r * (row + 1);
        uint8_t ft = in[0];
        in++;
        uint8_t *out = o + r * row, *up = r ? o + (r - 1) * row : NULL;
        for (size_t i = 0; i < row; i++) {
            int a = i >= bpp ? out[i - bpp] : 0, b = up ? up[i] : 0, c = (up && i >= bpp) ? up[i - bpp] : 0;
            int x = in[i];
            switch (ft) {
            case 1: x += a; break;
            case 2: x += b; break;
            case 3: x += (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                x += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                break;
            }
            default: break;
            }
            out[i] = (uint8_t)x;
        }
    }
    of_free(d);
    *n = nrows * row;
    return o;
}

static uint8_t *lzw(const uint8_t *in, size_t n, size_t *out_n, int early)
{
    typedef struct { uint16_t prev; uint8_t c; uint16_t len; } ent_t;
    ent_t *t = of_alloc(4096 * sizeof *t);
    of_sb_t o = { 0 };
    if (!t) return NULL;
    for (int i = 0; i < 256; i++) { t[i].prev = 0xFFFF; t[i].c = (uint8_t)i; t[i].len = 1; }
    int next = 258, width = 9, prev = -1;
    uint32_t acc = 0;
    int bits = 0;
    uint8_t tmp[4096];
    for (size_t i = 0; i < n;) {
        while (bits < width && i < n) { acc = acc << 8 | in[i++]; bits += 8; }
        if (bits < width) break;
        int code = (int)((acc >> (bits - width)) & ((1u << width) - 1));
        bits -= width;
        if (code == 256) { next = 258; width = 9; prev = -1; continue; }
        if (code == 257) break;
        int out_code = code;
        uint8_t first;
        if (code < next) {
            int l = t[code].len, k = code;
            if (l > 4096) break;
            for (int j = l - 1; j >= 0; j--) { tmp[j] = t[k].c; k = t[k].prev; }
            of_sb_put(&o, (const char *)tmp, (size_t)l);
            first = tmp[0];
        } else if (code == next && prev >= 0) {
            int l = t[prev].len, k = prev;
            if (l >= 4096) break;
            for (int j = l - 1; j >= 0; j--) { tmp[j] = t[k].c; k = t[k].prev; }
            tmp[l] = tmp[0];
            of_sb_put(&o, (const char *)tmp, (size_t)l + 1);
            first = tmp[0];
        } else {
            break;
        }
        if (prev >= 0 && next < 4096) {
            t[next].prev = (uint16_t)prev;
            t[next].c = first;
            t[next].len = (uint16_t)(t[prev].len + 1);
            next++;
        }
        prev = out_code;
        if (next + early >= (1 << width) && width < 12) width++;
    }
    of_free(t);
    *out_n = o.n;
    return (uint8_t *)(o.s ? o.s : of_alloc(1));
}

static uint8_t *a85(const uint8_t *in, size_t n, size_t *out_n)
{
    of_sb_t o = { 0 };
    uint32_t acc = 0;
    int k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        if (c == '~') break;
        if (is_ws(c)) continue;
        if (c == 'z' && k == 0) { of_sb_put(&o, "\0\0\0\0", 4); continue; }
        if (c < '!' || c > 'u') continue;
        acc = acc * 85 + (uint32_t)(c - '!');
        if (++k == 5) {
            char b[4] = { (char)(acc >> 24), (char)(acc >> 16), (char)(acc >> 8), (char)acc };
            of_sb_put(&o, b, 4);
            acc = 0;
            k = 0;
        }
    }
    if (k > 1) {
        for (int j = k; j < 5; j++) acc = acc * 85 + 84;
        char b[4] = { (char)(acc >> 24), (char)(acc >> 16), (char)(acc >> 8), (char)acc };
        of_sb_put(&o, b, (size_t)(k - 1));
    }
    *out_n = o.n;
    return (uint8_t *)(o.s ? o.s : of_alloc(1));
}

static uint8_t *ahx(const uint8_t *in, size_t n, size_t *out_n)
{
    of_sb_t o = { 0 };
    int hi = -1;
    for (size_t i = 0; i < n && in[i] != '>'; i++) {
        int v = hexv(in[i]);
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { of_sb_putc(&o, (char)(hi << 4 | v)); hi = -1; }
    }
    if (hi >= 0) of_sb_putc(&o, (char)(hi << 4));
    *out_n = o.n;
    return (uint8_t *)(o.s ? o.s : of_alloc(1));
}

static uint8_t *rle(const uint8_t *in, size_t n, size_t *out_n)
{
    of_sb_t o = { 0 };
    for (size_t i = 0; i < n;) {
        uint8_t l = in[i++];
        if (l == 128) break;
        if (l < 128) {
            size_t k = (size_t)l + 1;
            if (i + k > n) k = n - i;
            of_sb_put(&o, (const char *)in + i, k);
            i += k;
        } else if (i < n) {
            for (int k = 0; k < 257 - l; k++) of_sb_putc(&o, (char)in[i]);
            i++;
        }
    }
    *out_n = o.n;
    return (uint8_t *)(o.s ? o.s : of_alloc(1));
}

/* -------------------------------------------------------------------------- */
/* Encryption                                                                  */
/* -------------------------------------------------------------------------- */

static const uint8_t PAD[32] = { 0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
                                 0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A };

static void pad_pw(const char *pw, uint8_t out[32])
{
    size_t l = pw ? strlen(pw) : 0;
    if (l > 32) l = 32;
    if (l) memcpy(out, pw, l);
    memcpy(out + l, PAD, 32 - l);
}

/* Algorithm 2: the file key from a (padded) user password */
static void key_r4(pdf_t *P, const uint8_t pw[32], const uint8_t *O, int32_t perm, const uint8_t *id, int idn, int R, bool meta)
{
    of_md5_t m;
    of_md5_init(&m);
    of_md5_update(&m, pw, 32);
    of_md5_update(&m, O, 32);
    uint8_t p[4] = { (uint8_t)perm, (uint8_t)(perm >> 8), (uint8_t)(perm >> 16), (uint8_t)(perm >> 24) };
    of_md5_update(&m, p, 4);
    of_md5_update(&m, id, (size_t)idn);
    if (R >= 4 && !meta) of_md5_update(&m, "\xFF\xFF\xFF\xFF", 4);
    uint8_t h[16];
    of_md5_final(&m, h);
    if (R >= 3)
        for (int i = 0; i < 50; i++) of_md5(h, (size_t)P->keylen, h);
    memcpy(P->key, h, (size_t)P->keylen);
}

static bool check_user_r4(pdf_t *P, const uint8_t *U, const uint8_t *id, int idn, int R)
{
    uint8_t x[32];
    if (R == 2) {
        memcpy(x, PAD, 32);
        of_rc4(P->key, (size_t)P->keylen, x, 32);
        return !memcmp(x, U, 32);
    }
    of_md5_t m;
    of_md5_init(&m);
    of_md5_update(&m, PAD, 32);
    of_md5_update(&m, id, (size_t)idn);
    of_md5_final(&m, x);
    of_rc4(P->key, (size_t)P->keylen, x, 16);
    for (int i = 1; i <= 19; i++) {
        uint8_t k[16];
        for (int j = 0; j < P->keylen; j++) k[j] = (uint8_t)(P->key[j] ^ i);
        of_rc4(k, (size_t)P->keylen, x, 16);
    }
    return !memcmp(x, U, 16);
}

/* revision 6's hash (ISO 32000-2, algorithm 2.B) */
static void hash6(const uint8_t *pw, size_t pl, const uint8_t *salt, const uint8_t *udata, int R, uint8_t out[32])
{
    uint8_t buf[127 + 64 + 48];
    size_t n = 0;
    memcpy(buf, pw, pl); n = pl;
    memcpy(buf + n, salt, 8); n += 8;
    if (udata) { memcpy(buf + n, udata, 48); n += 48; }
    uint8_t K[64];
    of_sha256(buf, n, K);
    if (R == 5) { memcpy(out, K, 32); return; }
    size_t kl = 32;
    uint8_t *k1 = of_alloc((127 + 64 + 48) * 64);
    if (!k1) { memcpy(out, K, 32); return; }
    for (int i = 0;; i++) {
        size_t one = 0;
        uint8_t tmp[127 + 64 + 48];
        memcpy(tmp, pw, pl); one = pl;
        memcpy(tmp + one, K, kl); one += kl;
        if (udata) { memcpy(tmp + one, udata, 48); one += 48; }
        for (int r = 0; r < 64; r++) memcpy(k1 + r * one, tmp, one);
        size_t len = one * 64;
        of_aes_cbc_encrypt(K, 128, K + 16, k1, len);
        int sum = 0;
        for (int j = 0; j < 16; j++) sum += k1[j];
        switch (sum % 3) {
        case 0: of_sha256(k1, len, K); kl = 32; break;
        case 1: of_sha384(k1, len, K); kl = 48; break;
        default: of_sha512(k1, len, K); kl = 64; break;
        }
        if (i >= 63 && k1[len - 1] <= (unsigned)(i + 1 - 32)) break;
        if (i > 2000) break;
    }
    of_free(k1);
    memcpy(out, K, 32);
}

static const pv_t *str_of(pdf_t *P, const pv_t *d, const char *k)
{
    const pv_t *v = pdf_res(P, pv_get(d, k));
    return v && v->t == PV_STR ? v : NULL;
}

/* 0: ready (or nothing to do); -1: the password is wrong; -2: a handler the app does not have */
static int crypt_init(pdf_t *P, const char *password)
{
    const pv_t *er = pv_get(&P->trailer, "Encrypt");
    if (!er) return 0;
    if (er->t == PV_REF) P->enc_obj = er->ref.num;
    const pv_t *E = pdf_res(P, er);
    if (!E || E->t != PV_DICT) return 0;
    if (!pv_is_name(pdf_res(P, pv_get(E, "Filter")), "Standard")) return -2;
    int V = pvi(pdf_res(P, pv_get(E, "V")), 0);
    int R = pvi(pdf_res(P, pv_get(E, "R")), 2);
    const pv_t *O = str_of(P, E, "O"), *U = str_of(P, E, "U");
    if (!O || !U || O->s.n < 32 || U->s.n < 32) return -2;
    int32_t perm = (int32_t)pv_num(pdf_res(P, pv_get(E, "P")), 0);
    const pv_t *em = pv_get(E, "EncryptMetadata");
    bool meta = !(em && em->t == PV_BOOL && !em->b);
    const pv_t *ida = pdf_res(P, pv_get(&P->trailer, "ID"));
    const uint8_t *id = (const uint8_t *)"";
    int idn = 0;
    if (ida && ida->t == PV_ARR && ida->a.n && ida->a.v[0].t == PV_STR) { id = (const uint8_t *)ida->a.v[0].s.s; idn = ida->a.v[0].s.n; }
    /* the method: V 1-2 RC4; V 4-5 by crypt filter */
    P->crypt = 1;
    int bits = pvi(pdf_res(P, pv_get(E, "Length")), 40);
    if (V >= 4) {
        const pv_t *cf = pdf_res(P, pv_get(E, "CF"));
        const pv_t *stmf = pdf_res(P, pv_get(E, "StmF")), *strf = pdf_res(P, pv_get(E, "StrF"));
        const pv_t *std = cf ? pdf_res(P, pv_get(cf, stmf && stmf->t == PV_NAME ? stmf->s.s : "StdCF")) : NULL;
        const pv_t *cfm = std ? pdf_res(P, pv_get(std, "CFM")) : NULL;
        if (pv_is_name(cfm, "AESV2")) P->crypt = 2;
        else if (pv_is_name(cfm, "AESV3")) P->crypt = 3;
        else if (pv_is_name(cfm, "V2")) P->crypt = 1;
        else if (pv_is_name(cfm, "None")) P->crypt = 0;
        double l = pv_num(std ? pdf_res(P, pv_get(std, "Length")) : NULL, 0);
        if (l > 0) bits = l <= 32 ? (int)l * 8 : (int)l;
        P->no_streams = pv_is_name(stmf, "Identity");
        P->no_strings = pv_is_name(strf, "Identity");
        if (P->crypt == 2) bits = 128;
    }
    if (R >= 5) {
        /* AES-256 */
        P->crypt = 3;
        P->keylen = 32;
        const pv_t *UE = str_of(P, E, "UE"), *OE = str_of(P, E, "OE");
        if (U->s.n < 48 || O->s.n < 48 || !UE || !OE || UE->s.n < 32 || OE->s.n < 32) return -2;
        const uint8_t *u = (const uint8_t *)U->s.s, *o = (const uint8_t *)O->s.s;
        size_t pl = password ? strlen(password) : 0;
        if (pl > 127) pl = 127;
        const uint8_t *pw = (const uint8_t *)(password ? password : "");
        uint8_t h[32], iv[16] = { 0 };
        hash6(pw, pl, u + 32, NULL, R, h);
        if (!memcmp(h, u, 32)) {
            hash6(pw, pl, u + 40, NULL, R, h);
            memcpy(P->key, UE->s.s, 32);
            of_aes_cbc_decrypt(h, 256, iv, P->key, 32, false);
            return 0;
        }
        hash6(pw, pl, o + 32, u, R, h);
        if (!memcmp(h, o, 32)) {
            hash6(pw, pl, o + 40, u, R, h);
            memcpy(P->key, OE->s.s, 32);
            of_aes_cbc_decrypt(h, 256, iv, P->key, 32, false);
            return 0;
        }
        return -1;
    }
    P->keylen = R == 2 ? 5 : bits / 8;
    if (P->keylen < 5) P->keylen = 5;
    if (P->keylen > 16) P->keylen = 16;
    uint8_t pw[32];
    pad_pw(password, pw);
    key_r4(P, pw, (const uint8_t *)O->s.s, perm, id, idn, R, meta);
    if (check_user_r4(P, (const uint8_t *)U->s.s, id, idn, R)) return 0;
    /* as the owner password: it opens the user password stored in /O */
    uint8_t h[16];
    of_md5(pw, 32, h);
    if (R >= 3)
        for (int i = 0; i < 50; i++) of_md5(h, (size_t)P->keylen, h);
    uint8_t up[32];
    memcpy(up, O->s.s, 32);
    if (R == 2) {
        of_rc4(h, (size_t)P->keylen, up, 32);
    } else {
        for (int i = 19; i >= 0; i--) {
            uint8_t k[16];
            for (int j = 0; j < P->keylen; j++) k[j] = (uint8_t)(h[j] ^ i);
            of_rc4(k, (size_t)P->keylen, up, 32);
        }
    }
    key_r4(P, up, (const uint8_t *)O->s.s, perm, id, idn, R, meta);
    if (check_user_r4(P, (const uint8_t *)U->s.s, id, idn, R)) return 0;
    return -1;
}

/* in place; the length left */
static size_t decrypt(pdf_t *P, int num, int gen, uint8_t *d, size_t n)
{
    if (!P->crypt) return n;
    if (P->crypt == 3) {
        if (n < 16) return 0;
        size_t k = of_aes_cbc_decrypt(P->key, 256, d, d + 16, n - 16, true);
        memmove(d, d + 16, k);
        return k;
    }
    uint8_t buf[16 + 5 + 4], ok[16];
    memcpy(buf, P->key, (size_t)P->keylen);
    size_t l = (size_t)P->keylen;
    buf[l++] = (uint8_t)num; buf[l++] = (uint8_t)(num >> 8); buf[l++] = (uint8_t)(num >> 16);
    buf[l++] = (uint8_t)gen; buf[l++] = (uint8_t)(gen >> 8);
    if (P->crypt == 2) { memcpy(buf + l, "sAlT", 4); l += 4; }
    of_md5(buf, l, ok);
    size_t kl = (size_t)P->keylen + 5 > 16 ? 16 : (size_t)P->keylen + 5;
    if (P->crypt == 1) { of_rc4(ok, kl, d, n); return n; }
    if (n < 16) return 0;
    size_t k = of_aes_cbc_decrypt(ok, 128, d, d + 16, n - 16, true);
    memmove(d, d + 16, k);
    return k;
}

static void decrypt_strings(pdf_t *P, pv_t *v, int num, int gen, int depth)
{
    if (depth > MAX_DEPTH) return;
    if (v->t == PV_STR) v->s.n = (int)decrypt(P, num, gen, (uint8_t *)v->s.s, (size_t)v->s.n);
    else if (v->t == PV_ARR) for (int i = 0; i < v->a.n; i++) decrypt_strings(P, &v->a.v[i], num, gen, depth + 1);
    else if (v->t == PV_DICT || v->t == PV_STREAM) for (int i = 0; i < v->d.n; i++) decrypt_strings(P, &v->d.v[i], num, gen, depth + 1);
}

/* -------------------------------------------------------------------------- */
/* Objects                                                                     */
/* -------------------------------------------------------------------------- */

static bool grow_x(pdf_t *P, int n)
{
    if (n <= P->nx) return true;
    if (n > 8000000) return false;
    xref_t *x = of_realloc(P->x, (size_t)n * sizeof *x);
    pv_t **c = of_realloc(P->cache, (size_t)n * sizeof *c);
    uint8_t *ld = of_realloc(P->loading, (size_t)n);
    if (x) P->x = x;
    if (c) P->cache = c;
    if (ld) P->loading = ld;
    if (!x || !c || !ld) return false;
    memset(P->x + P->nx, 0, (size_t)(n - P->nx) * sizeof *x);
    memset(P->cache + P->nx, 0, (size_t)(n - P->nx) * sizeof *c);
    memset(P->loading + P->nx, 0, (size_t)(n - P->nx));
    P->nx = n;
    return true;
}

static bool kw_at(const pdf_t *P, size_t p, const char *kw)
{
    size_t l = strlen(kw);
    return p + l <= P->len && !memcmp(P->buf + p, kw, l);
}

/* the data of a stream starting after "stream", and its length */
static void stream_bounds(pdf_t *P, lx_t *L, pv_t *v)
{
    size_t p = L->p;
    if (p < P->len && P->buf[p] == '\r') p++;
    if (p < P->len && P->buf[p] == '\n') p++;
    double len = pv_num(pdf_res(P, pv_get(v, "Length")), -1);
    size_t end = 0;
    if (len >= 0 && p + (size_t)len <= P->len) {
        /* trust /Length if "endstream" follows */
        size_t e = p + (size_t)len;
        size_t q = e;
        while (q < P->len && q < e + 4 && is_ws(P->buf[q])) q++;
        if (kw_at(P, q, "endstream")) end = e;
    }
    if (!end) {
        /* look for it */
        const uint8_t *f = NULL;
        for (size_t q = p; q + 9 <= P->len; q++)
            if (P->buf[q] == 'e' && !memcmp(P->buf + q, "endstream", 9)) { f = P->buf + q; break; }
        end = f ? (size_t)(f - P->buf) : P->len;
        while (end > p && (P->buf[end - 1] == '\n' || P->buf[end - 1] == '\r')) end--;
    }
    v->t = PV_STREAM;
    v->soff = p;
    v->slen = end - p;
}

static void rebuild(pdf_t *P);

static pv_t *load_plain(pdf_t *P, int num, size_t off, bool retry)
{
    lx_t L = { P->buf, P->len, off };
    pv_t a, b, kw;
    bool ok = pv_parse(NULL, &L, &a) && a.t == PV_INT && a.i == num && pv_parse(NULL, &L, &b) && b.t == PV_INT &&
              pv_parse(NULL, &L, &kw) && kw.t == PV_OP && !strcmp(kw.s.s, "obj");
    int gen = ok ? (int)b.i : 0;
    if (ok) pv_free(&kw);
    if (!ok) {
        if (retry && !P->rebuilt) { rebuild(P); return num < P->nx && P->x[num].type == 1 ? load_plain(P, num, P->x[num].off, false) : NULL; }
        return NULL;
    }
    pv_t *v = of_alloc(sizeof *v);
    if (!v) return NULL;
    if (!pv_parse(P, &L, v)) { of_free(v); return NULL; }
    if (v->t == PV_DICT) {
        size_t save = L.p;
        lx_ws(&L);
        if (kw_at(P, L.p, "stream")) { L.p += 6; stream_bounds(P, &L, v); }
        else L.p = save;
    }
    v->gen = gen;
    if (P->crypt && num != P->enc_obj && !P->no_strings) decrypt_strings(P, v, num, gen, 0);
    return v;
}

static pv_t *load_from_objstm(pdf_t *P, int num, int stm, int idx)
{
    /* the object stream, decoded once and kept (the last four) */
    int slot = -1;
    for (int i = 0; i < 4; i++) if (P->os[i].num == stm && P->os[i].data) slot = i;
    if (slot < 0) {
        pv_t *s = pdf_obj(P, stm);
        if (!s || s->t != PV_STREAM) return NULL;
        size_t n;
        uint8_t *d = pdf_stream(P, s, stm, &n, NULL);
        if (!d) return NULL;
        slot = P->os_next++ & 3;
        of_free(P->os[slot].data);
        of_free(P->os[slot].offs);
        P->os[slot].num = stm;
        P->os[slot].data = d;
        P->os[slot].len = n;
        int cnt = pvi(pdf_res(P, pv_get(s, "N")), 0);
        int first = pvi(pdf_res(P, pv_get(s, "First")), 0);
        if (cnt < 0 || cnt > 1000000) cnt = 0;
        P->os[slot].first = first;
        P->os[slot].n = cnt;
        P->os[slot].offs = of_alloc((size_t)(cnt + 1) * 2 * sizeof(int));
        lx_t L = { d, n, 0 };
        for (int i = 0; i < cnt && P->os[slot].offs; i++) {
            pv_t a, b;
            if (!pv_parse(NULL, &L, &a) || !pv_parse(NULL, &L, &b)) break;
            P->os[slot].offs[i * 2] = pvi(&a, -1);
            P->os[slot].offs[i * 2 + 1] = pvi(&b, 0);
        }
    }
    struct pdf_os *o = &P->os[slot];
    if (!o->offs) return NULL;
    int at = -1;
    if (idx >= 0 && idx < o->n && o->offs[idx * 2] == num) at = idx;
    for (int i = 0; i < o->n && at < 0; i++) if (o->offs[i * 2] == num) at = i;
    if (at < 0) return NULL;
    lx_t L = { o->data, o->len, (size_t)(o->first + o->offs[at * 2 + 1]) };
    if (L.p >= L.n) return NULL;
    pv_t *v = of_alloc(sizeof *v);
    if (!v) return NULL;
    if (!pv_parse(P, &L, v)) { of_free(v); return NULL; }
    return v;
}

pv_t *pdf_obj(pdf_t *P, int num)
{
    if (num <= 0 || num >= P->nx) {
        if (num > 0 && !P->rebuilt) { rebuild(P); if (num < P->nx) return pdf_obj(P, num); }
        return NULL;
    }
    if (P->cache[num]) return P->cache[num];
    if (P->loading[num]) return NULL;
    P->loading[num] = 1;
    pv_t *v = NULL;
    if (P->x[num].type == 1) v = load_plain(P, num, P->x[num].off, true);
    else if (P->x[num].type == 2) v = load_from_objstm(P, num, (int)P->x[num].off, P->x[num].gen);
    else if (!P->rebuilt) {
        P->loading[num] = 0;
        rebuild(P);
        if (num < P->nx && P->x[num].type) return pdf_obj(P, num);
        return NULL;
    }
    if (num < P->nx) {
        P->loading[num] = 0;
        P->cache[num] = v;
    }
    return v;
}

const pv_t *pdf_res(pdf_t *P, const pv_t *v)
{
    for (int i = 0; v && v->t == PV_REF && i < 16; i++) v = pdf_obj(P, v->ref.num);
    return v && v->t == PV_REF ? NULL : v;
}

const pv_t *pdf_get(pdf_t *P, const pv_t *d, const char *key)
{
    return pdf_res(P, pv_get(d, key));
}

uint8_t *pdf_stream(pdf_t *P, const pv_t *s, int num, size_t *out_n, const char **image_filter)
{
    if (!s || s->t != PV_STREAM) return NULL;
    if (image_filter) *image_filter = NULL;
    size_t n = s->slen;
    uint8_t *d = of_alloc(n + 1);
    if (!d) return NULL;
    memcpy(d, P->buf + s->soff, n);
    bool is_xref = pv_is_name(pdf_res(P, pv_get(s, "Type")), "XRef");
    if (P->crypt && !P->no_streams && !is_xref && num != P->enc_obj) n = decrypt(P, num, s->gen, d, n);
    const pv_t *f = pdf_res(P, pv_get(s, "Filter"));
    const pv_t *parms = pdf_res(P, pv_get(s, "DecodeParms"));
    if (!parms) parms = pdf_res(P, pv_get(s, "DP"));
    return pdf_decode(P, d, n, f, parms, out_n, image_filter);
}

uint8_t *pdf_decode(pdf_t *P, uint8_t *d, size_t n, const pv_t *f, const pv_t *parms, size_t *out_n, const char **image_filter)
{
    if (image_filter) *image_filter = NULL;
    int nf = !f ? 0 : f->t == PV_ARR ? f->a.n : 1;
    for (int i = 0; i < nf; i++) {
        const pv_t *fi = f->t == PV_ARR ? pdf_res(P, &f->a.v[i]) : f;
        const pv_t *pi = parms && parms->t == PV_ARR ? (i < parms->a.n ? pdf_res(P, &parms->a.v[i]) : NULL) : parms;
        if (!fi || fi->t != PV_NAME) break;
        const char *fn = fi->s.s;
        uint8_t *o = NULL;
        size_t on = 0;
        if (!strcmp(fn, "FlateDecode") || !strcmp(fn, "Fl")) {
            bool zl = n >= 2 && (d[0] & 0x0F) == 8 && ((d[0] << 8) | d[1]) % 31 == 0;
            o = of_inflate_lax(d, n, zl, &on);
            if (!o && zl) o = of_inflate_lax(d + 2, n - 2, false, &on);
            if (!o) o = of_alloc(1);
            if (o) o = unpredict(o, &on, pi);
        } else if (!strcmp(fn, "LZWDecode") || !strcmp(fn, "LZW")) {
            o = lzw(d, n, &on, pvi(pv_get(pi, "EarlyChange"), 1));
            if (o) o = unpredict(o, &on, pi);
        } else if (!strcmp(fn, "ASCII85Decode") || !strcmp(fn, "A85")) {
            o = a85(d, n, &on);
        } else if (!strcmp(fn, "ASCIIHexDecode") || !strcmp(fn, "AHx")) {
            o = ahx(d, n, &on);
        } else if (!strcmp(fn, "RunLengthDecode") || !strcmp(fn, "RL")) {
            o = rle(d, n, &on);
        } else if (!strcmp(fn, "Crypt")) {
            continue;
        } else {
            /* DCT, JPX, CCITT, JBIG2: the picture's own coding, left for the caller */
            if (image_filter) *image_filter = !strcmp(fn, "DCT") ? "DCTDecode" : !strcmp(fn, "CCF") ? "CCITTFaxDecode" : fn;
            break;
        }
        of_free(d);
        if (!o) { *out_n = 0; return NULL; }
        d = o;
        n = on;
    }
    d[n] = 0;       /* the decoders leave a byte to spare */
    *out_n = n;
    return d;
}

/* -------------------------------------------------------------------------- */
/* The cross-reference                                                         */
/* -------------------------------------------------------------------------- */

static void set_x(pdf_t *P, int num, uint8_t type, uint32_t off, uint16_t gen)
{
    if (num < 0 || !grow_x(P, num + 1)) return;
    if (P->x[num].set) return;      /* the newest section wins, and it is read first */
    P->x[num].set = 1;
    P->x[num].type = type;
    P->x[num].off = off;
    P->x[num].gen = gen;
}

static void take_trailer(pdf_t *P, pv_t *t)
{
    if (P->trailer.t == PV_NULL) { P->trailer = *t; t->t = PV_NULL; return; }
    /* keys the newest trailer lacks */
    static const char *const K[] = { "Root", "Info", "ID", "Encrypt" };
    for (int i = 0; i < 4; i++) {
        if (pv_get(&P->trailer, K[i]) || !pv_get(t, K[i])) continue;
        for (int k = 0; k < t->d.n; k++) {
            if (strcmp(t->d.k[k], K[i])) continue;
            char **nk = of_realloc(P->trailer.d.k, (size_t)(P->trailer.d.n + 1) * sizeof *nk);
            pv_t *nv = of_realloc(P->trailer.d.v, (size_t)(P->trailer.d.n + 1) * sizeof *nv);
            if (nk) P->trailer.d.k = nk;
            if (nv) P->trailer.d.v = nv;
            if (!nk || !nv) break;
            P->trailer.d.k[P->trailer.d.n] = t->d.k[k];
            P->trailer.d.v[P->trailer.d.n] = t->d.v[k];
            P->trailer.d.n++;
            t->d.k[k] = of_strdup("");
            t->d.v[k].t = PV_NULL;
        }
    }
    pv_free(t);
}

static bool read_xref(pdf_t *P, size_t off, int depth)
{
    if (off >= P->len || depth > 32) return false;
    lx_t L = { P->buf, P->len, off };
    lx_ws(&L);
    if (kw_at(P, L.p, "xref")) {
        L.p += 4;
        for (;;) {
            lx_ws(&L);
            if (kw_at(P, L.p, "trailer")) { L.p += 7; break; }
            pv_t a, b;
            if (!pv_parse(NULL, &L, &a) || !pv_parse(NULL, &L, &b) || a.t != PV_INT || b.t != PV_INT) return false;
            int start = (int)a.i, cnt = (int)b.i;
            if (cnt < 0 || cnt > 8000000) return false;
            lx_ws(&L);
            for (int i = 0; i < cnt; i++) {
                /* "oooooooooo ggggg n" lines, 20 bytes, sometimes 19 or 21 */
                while (L.p < L.n && is_ws(P->buf[L.p])) L.p++;
                if (L.p + 17 > L.n) return false;
                const char *e = (const char *)P->buf + L.p;
                uint32_t o = (uint32_t)strtoul(e, NULL, 10);
                uint16_t g = (uint16_t)strtoul(e + 11, NULL, 10);
                char t = e[17];
                if (start + i == 0 && t == 'n' && o == 0) t = 'f';
                set_x(P, start + i, t == 'n' ? 1 : 0, o, g);
                L.p += 18;
            }
        }
        pv_t t;
        if (!pv_parse(P, &L, &t) || t.t != PV_DICT) { pv_free(&t); return false; }
        size_t xs = (size_t)pv_num(pv_get(&t, "XRefStm"), 0);
        size_t prev = (size_t)pv_num(pv_get(&t, "Prev"), 0);
        bool has_prev = pv_get(&t, "Prev") != NULL;
        take_trailer(P, &t);
        if (xs) read_xref(P, xs, depth + 1);
        if (has_prev && prev != off) read_xref(P, prev, depth + 1);
        return true;
    }
    /* a cross-reference stream */
    pv_t a, b, kw;
    if (!pv_parse(NULL, &L, &a) || !pv_parse(NULL, &L, &b) || !pv_parse(NULL, &L, &kw) || kw.t != PV_OP || strcmp(kw.s.s, "obj")) {
        pv_free(&kw);
        return false;
    }
    pv_free(&kw);
    pv_t s;
    if (!pv_parse(P, &L, &s) || s.t != PV_DICT) { pv_free(&s); return false; }
    lx_ws(&L);
    if (!kw_at(P, L.p, "stream")) { pv_free(&s); return false; }
    L.p += 6;
    stream_bounds(P, &L, &s);
    size_t n;
    uint8_t *d = pdf_stream(P, &s, -1, &n, NULL);
    const pv_t *W = pv_get(&s, "W");
    int w[3] = { 0, 0, 0 };
    if (W && W->t == PV_ARR && W->a.n >= 3)
        for (int i = 0; i < 3; i++) w[i] = pvi(&W->a.v[i], 0);
    int rowlen = w[0] + w[1] + w[2];
    const pv_t *ix = pv_get(&s, "Index");
    int size = pvi(pv_get(&s, "Size"), 0);
    size_t pos = 0;
    int ranges = ix && ix->t == PV_ARR ? ix->a.n / 2 : 1;
    for (int r = 0; d && rowlen > 0 && rowlen < 32 && r < ranges; r++) {
        int start = ix && ix->t == PV_ARR ? pvi(&ix->a.v[r * 2], 0) : 0;
        int cnt = ix && ix->t == PV_ARR ? pvi(&ix->a.v[r * 2 + 1], 0) : size;
        for (int i = 0; i < cnt && pos + (size_t)rowlen <= n; i++) {
            uint64_t f[3] = { 0, 0, 0 };
            for (int k = 0; k < 3; k++)
                for (int j = 0; j < w[k]; j++) f[k] = f[k] << 8 | d[pos++];
            if (!w[0]) f[0] = 1;
            if (f[0] <= 2) set_x(P, start + i, (uint8_t)f[0], (uint32_t)f[1], (uint16_t)f[2]);
        }
    }
    of_free(d);
    size_t prev = (size_t)pv_num(pv_get(&s, "Prev"), 0);
    bool has_prev = pv_get(&s, "Prev") != NULL;
    take_trailer(P, &s);
    if (has_prev && prev != off) read_xref(P, prev, depth + 1);
    return true;
}

/* Every "n g obj" in the file, the last one of a number winning; the
 * trailer from the last "trailer", or the catalogue found. */
static void rebuild(pdf_t *P)
{
    P->rebuilt = true;
    for (int i = 0; i < P->nx; i++) P->x[i].set = 0;
    for (size_t p = 0; p + 8 < P->len; p++) {
        if (P->buf[p] < '0' || P->buf[p] > '9' || (p && !is_ws(P->buf[p - 1]) && P->buf[p - 1] != '>')) continue;
        size_t q = p;
        uint32_t num = 0;
        while (q < P->len && P->buf[q] >= '0' && P->buf[q] <= '9' && num < 10000000) num = num * 10 + (P->buf[q++] - '0');
        if (q >= P->len || !is_ws(P->buf[q])) continue;
        while (q < P->len && is_ws(P->buf[q])) q++;
        uint32_t gen = 0;
        size_t g0 = q;
        while (q < P->len && P->buf[q] >= '0' && P->buf[q] <= '9') gen = gen * 10 + (P->buf[q++] - '0');
        if (q == g0 || q >= P->len || !is_ws(P->buf[q])) continue;
        while (q < P->len && is_ws(P->buf[q])) q++;
        if (!kw_at(P, q, "obj")) continue;
        if (!grow_x(P, (int)num + 1)) continue;
        P->x[num].type = 1;
        P->x[num].off = (uint32_t)p;
        P->x[num].gen = (uint16_t)gen;
        P->x[num].set = 1;
        /* what was cached from the old table for this number: read again, the old
         * one kept until the end (pages and fonts may still point into it) */
        if (P->cache[num]) {
            pv_t **d = of_realloc(P->dead, (size_t)(P->ndead + 1) * sizeof *d);
            if (d) { P->dead = d; P->dead[P->ndead++] = P->cache[num]; P->cache[num] = NULL; }
        }
        p = q + 2;
    }
    /* objects inside object streams: the streams say which */
    for (int i = 1; i < P->nx; i++) {
        if (P->x[i].type != 1) continue;
        pv_t *s = pdf_obj(P, i);
        if (!s || s->t != PV_STREAM || !pv_is_name(pdf_res(P, pv_get(s, "Type")), "ObjStm")) continue;
        size_t n;
        uint8_t *d = pdf_stream(P, s, i, &n, NULL);
        int cnt = pvi(pdf_res(P, pv_get(s, "N")), 0);
        lx_t L = { d, n, 0 };
        for (int k = 0; d && k < cnt && k < 100000; k++) {
            pv_t a, b;
            if (!pv_parse(NULL, &L, &a) || !pv_parse(NULL, &L, &b)) break;
            int on = pvi(&a, -1);
            if (on > 0 && grow_x(P, on + 1) && P->x[on].type != 1) { P->x[on].type = 2; P->x[on].off = (uint32_t)i; P->x[on].gen = (uint16_t)k; }
        }
        of_free(d);
    }
    if (!pv_get(&P->trailer, "Root")) {
        for (size_t p = P->len; p-- > 7;) {
            if (P->buf[p] != 't' || !kw_at(P, p, "trailer")) continue;
            lx_t L = { P->buf, P->len, p + 7 };
            pv_t t;
            if (pv_parse(P, &L, &t) && t.t == PV_DICT && pv_get(&t, "Root")) { pv_free(&P->trailer); P->trailer = t; break; }
            pv_free(&t);
        }
    }
    if (!pv_get(&P->trailer, "Root")) {
        for (int i = 1; i < P->nx; i++) {
            pv_t *o = pdf_obj(P, i);
            if (o && o->t == PV_DICT && pv_is_name(pdf_res(P, pv_get(o, "Type")), "Catalog")) { P->root = i; break; }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Opening                                                                     */
/* -------------------------------------------------------------------------- */

pdf_t *pdf_open(const char *path, const char *password, char *err, size_t errlen)
{
    pdf_t *P = of_alloc(sizeof *P);
    if (!P) { snprintf(err, errlen, "memory"); return NULL; }
    for (int i = 0; i < 4; i++) P->os[i].num = -1;
    size_t n;
    P->buf = of_file_read(path, &n, 96u * 1024 * 1024);
    P->len = n;
    if (!P->buf) { snprintf(err, errlen, "read"); of_free(P); return NULL; }
    /* "%PDF-" within the first kilobyte */
    bool head = false;
    for (size_t i = 0; i + 5 <= n && i < 1024 && !head; i++) head = !memcmp(P->buf + i, "%PDF-", 5);
    if (!head) { snprintf(err, errlen, "format"); pdf_close(P); return NULL; }
    /* startxref, near the end */
    size_t sx = 0;
    for (size_t p = n > 9 ? n - 9 : 0; p + 9 <= n && p + 4096 > n; p--) {
        if (!memcmp(P->buf + p, "startxref", 9)) { sx = (size_t)strtoul((const char *)P->buf + p + 9, NULL, 10); break; }
        if (!p) break;
    }
    bool ok = sx && read_xref(P, sx, 0) && pv_get(&P->trailer, "Root");
    if (!ok) rebuild(P);
    int c = crypt_init(P, password);
    if (c == -1) { snprintf(err, errlen, "password"); pdf_close(P); return NULL; }
    if (c == -2) { snprintf(err, errlen, "encrypted"); pdf_close(P); return NULL; }
    /* strings already parsed before the key was known are re-read */
    if (P->crypt)
        for (int i = 0; i < P->nx; i++)
            if (P->cache[i] && i != P->enc_obj) { pv_free(P->cache[i]); of_free(P->cache[i]); P->cache[i] = NULL; }
    const pv_t *root = pdf_get(P, &P->trailer, "Root");
    if (!root && P->root) root = pdf_obj(P, P->root);
    if (!root || root->t != PV_DICT) {
        if (!P->rebuilt) { rebuild(P); root = pdf_get(P, &P->trailer, "Root"); if (!root && P->root) root = pdf_obj(P, P->root); }
        if (!root || root->t != PV_DICT) { snprintf(err, errlen, "damaged"); pdf_close(P); return NULL; }
    }
    P->catalog = root;
    return P;
}

void pdf_close(pdf_t *P)
{
    if (!P) return;
    for (int i = 0; i < P->nx; i++)
        if (P->cache[i]) { pv_free(P->cache[i]); of_free(P->cache[i]); }
    for (int i = 0; i < 4; i++) { of_free(P->os[i].data); of_free(P->os[i].offs); }
    for (int i = 0; i < P->ndead; i++) { pv_free(P->dead[i]); of_free(P->dead[i]); }
    of_free(P->dead);
    of_free(P->cache);
    of_free(P->loading);
    of_free(P->x);
    pv_free(&P->trailer);
    of_free(P->buf);
    of_free(P);
}
