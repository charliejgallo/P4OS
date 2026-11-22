/*
 * P4OS - Office: the text of a PDF, reflowed into a read-only document.
 *
 * Every page's content stream is interpreted (text, its matrices and
 * states, form XObjects, pictures) and every glyph decoded to Unicode: the
 * font's ToUnicode map, or its encoding (WinAnsi, MacRoman, Standard,
 * PDFDoc and /Differences with the glyph names), or the UCS-2 CMaps.
 * Glyphs become runs, runs lines, lines segments (two columns are found
 * and read one after the other), and segments paragraphs: a line joins the
 * one above when it has the same size, follows at a line's distance, is not
 * indented and the one above was full. Bigger sizes are headings; lines
 * that repeat at the top or the bottom of the pages (running heads, page
 * numbers) go. Hyphens at the end of a line join the word. Pictures (JPEG
 * as they are, the rest as PNG) are written to the cache and placed where
 * they were in the reading order.
 */
#include "of_doc.h"
#include "of_pdf.h"
#include "of_pdfi.h"

#include <math.h>

/* the system gives apps float sqrt and fabs, not the double ones */
#define DABS(x) ((x) < 0 ? -(x) : (x))
#define DSQRT(x) ((double)sqrtf((float)(x)))
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_PAGES   2000
#define MAX_PICS    400

/* a number from the file as an integer, never out of range */
static inline int pvi(const pv_t *v, double def) { double d = pv_num(v, def); return d > -1.0e9 && d < 1.0e9 ? (int)d : 0; }
static inline int di(double d) { return d > -1.0e9 && d < 1.0e9 ? (int)d : 0; }

extern const char *(*of_tr)(const char *es);
#define _(s) (of_tr ? of_tr(s) : (s))

/* -------------------------------------------------------------------------- */
/* Tables: the standard fonts' widths, the encodings, glyph names              */
/* -------------------------------------------------------------------------- */

const int16_t of_pdf_helv[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556,
    556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556,
    556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584 };
const int16_t of_pdf_helvb[95] = {
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556,
    556, 556, 333, 333, 584, 584, 584, 611, 975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556, 333, 556, 611, 556, 611, 556, 333, 611,
    611, 278, 278, 556, 278, 889, 611, 611, 611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584 };
const int16_t of_pdf_times[95] = {
    250, 333, 408, 500, 500, 833, 778, 180, 333, 333, 500, 564, 250, 333, 250, 278, 500, 500, 500, 500, 500, 500, 500, 500,
    500, 500, 278, 278, 564, 564, 564, 444, 921, 722, 667, 667, 722, 611, 556, 722, 722, 333, 389, 722, 611, 889, 722, 722,
    556, 722, 667, 556, 611, 722, 722, 944, 722, 722, 611, 333, 278, 333, 469, 500, 333, 444, 500, 444, 500, 444, 333, 500,
    500, 278, 278, 500, 278, 778, 500, 500, 500, 500, 333, 389, 278, 500, 500, 722, 500, 500, 444, 480, 200, 480, 541 };

/* a width for any code point, from a table of ASCII: accented letters as their base */
int of_pdf_width(const int16_t *t, uint32_t cp)
{
    static const char BASE[] = "AAAAAAWCEEEEIIIIDNOOOOO+OUUUUYPbaaaaaamceeeeiiiionooooo+ouuuuypy";
    if (cp >= 32 && cp < 127) return t[cp - 32];
    if (cp == 0xA0) return t[0];
    if (cp >= 0xC0 && cp <= 0xFF) return t[BASE[cp - 0xC0] - 32];
    switch (cp) {
    case 0x2013: case 0x20AC: return t['0' - 32];
    case 0x2014: case 0x2026: return t['W' - 32] + 56;
    case 0x2018: case 0x2019: case 0x201A: return t[',' - 32];
    case 0x201C: case 0x201D: case 0x201E: return t['(' - 32];
    case 0x2022: return t['*' - 32];
    default: return t['o' - 32];
    }
}

static const uint16_t CP1252[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };

static const uint16_t MACROMAN[128] = {
    0xC4, 0xC5, 0xC7, 0xC9, 0xD1, 0xD6, 0xDC, 0xE1, 0xE0, 0xE2, 0xE4, 0xE3, 0xE5, 0xE7, 0xE9, 0xE8,
    0xEA, 0xEB, 0xED, 0xEC, 0xEE, 0xEF, 0xF1, 0xF3, 0xF2, 0xF4, 0xF6, 0xF5, 0xFA, 0xF9, 0xFB, 0xFC,
    0x2020, 0xB0, 0xA2, 0xA3, 0xA7, 0x2022, 0xB6, 0xDF, 0xAE, 0xA9, 0x2122, 0xB4, 0xA8, 0x2260, 0xC6, 0xD8,
    0x221E, 0xB1, 0x2264, 0x2265, 0xA5, 0xB5, 0x2202, 0x2211, 0x220F, 0x3C0, 0x222B, 0xAA, 0xBA, 0x3A9, 0xE6, 0xF8,
    0xBF, 0xA1, 0xAC, 0x221A, 0x192, 0x2248, 0x2206, 0xAB, 0xBB, 0x2026, 0xA0, 0xC0, 0xC3, 0xD5, 0x152, 0x153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0xF7, 0x25CA, 0xFF, 0x178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0xB7, 0x201A, 0x201E, 0x2030, 0xC2, 0xCA, 0xC1, 0xCB, 0xC8, 0xCD, 0xCE, 0xCF, 0xCC, 0xD3, 0xD4,
    0xF8FF, 0xD2, 0xDA, 0xDB, 0xD9, 0x131, 0x2C6, 0x2DC, 0xAF, 0x2D8, 0x2D9, 0x2DA, 0xB8, 0x2DD, 0x2DB, 0x2C7 };

/* StandardEncoding above 0x7F, where it has something */
static const uint16_t STD_HI[][2] = {
    { 0xA1, 0xA1 }, { 0xA2, 0xA2 }, { 0xA3, 0xA3 }, { 0xA4, 0x2044 }, { 0xA5, 0xA5 }, { 0xA6, 0x192 }, { 0xA7, 0xA7 },
    { 0xA8, 0xA4 }, { 0xA9, 0x27 }, { 0xAA, 0x201C }, { 0xAB, 0xAB }, { 0xAC, 0x2039 }, { 0xAD, 0x203A }, { 0xAE, 0xFB01 },
    { 0xAF, 0xFB02 }, { 0xB1, 0x2013 }, { 0xB2, 0x2020 }, { 0xB3, 0x2021 }, { 0xB4, 0xB7 }, { 0xB6, 0xB6 }, { 0xB7, 0x2022 },
    { 0xB8, 0x201A }, { 0xB9, 0x201E }, { 0xBA, 0x201D }, { 0xBB, 0xBB }, { 0xBC, 0x2026 }, { 0xBD, 0x2030 }, { 0xBF, 0xBF },
    { 0xC1, 0x60 }, { 0xC2, 0xB4 }, { 0xC3, 0x2C6 }, { 0xC4, 0x2DC }, { 0xC5, 0xAF }, { 0xC6, 0x2D8 }, { 0xC7, 0x2D9 },
    { 0xC8, 0xA8 }, { 0xCA, 0x2DA }, { 0xCB, 0xB8 }, { 0xCD, 0x2DD }, { 0xCE, 0x2DB }, { 0xCF, 0x2C7 }, { 0xD0, 0x2014 },
    { 0xE1, 0xC6 }, { 0xE3, 0xAA }, { 0xE8, 0x141 }, { 0xE9, 0xD8 }, { 0xEA, 0x152 }, { 0xEB, 0xBA }, { 0xF1, 0xE6 },
    { 0xF5, 0x131 }, { 0xF8, 0x142 }, { 0xF9, 0xF8 }, { 0xFA, 0x153 }, { 0xFB, 0xDF } };

static const struct { const char *n; uint16_t cp; } GLYPHS[] = {
    { "space", 0x20 }, { "exclam", 0x21 }, { "quotedbl", 0x22 }, { "numbersign", 0x23 }, { "dollar", 0x24 }, { "percent", 0x25 },
    { "ampersand", 0x26 }, { "quotesingle", 0x27 }, { "quoteright", 0x2019 }, { "parenleft", 0x28 }, { "parenright", 0x29 },
    { "asterisk", 0x2A }, { "plus", 0x2B }, { "comma", 0x2C }, { "hyphen", 0x2D }, { "minus", 0x2212 }, { "period", 0x2E },
    { "slash", 0x2F }, { "zero", 0x30 }, { "one", 0x31 }, { "two", 0x32 }, { "three", 0x33 }, { "four", 0x34 }, { "five", 0x35 },
    { "six", 0x36 }, { "seven", 0x37 }, { "eight", 0x38 }, { "nine", 0x39 }, { "colon", 0x3A }, { "semicolon", 0x3B },
    { "less", 0x3C }, { "equal", 0x3D }, { "greater", 0x3E }, { "question", 0x3F }, { "at", 0x40 }, { "bracketleft", 0x5B },
    { "backslash", 0x5C }, { "bracketright", 0x5D }, { "asciicircum", 0x5E }, { "underscore", 0x5F }, { "grave", 0x60 },
    { "quoteleft", 0x2018 }, { "braceleft", 0x7B }, { "bar", 0x7C }, { "braceright", 0x7D }, { "asciitilde", 0x7E },
    { "exclamdown", 0xA1 }, { "cent", 0xA2 }, { "sterling", 0xA3 }, { "currency", 0xA4 }, { "yen", 0xA5 }, { "brokenbar", 0xA6 },
    { "section", 0xA7 }, { "dieresis", 0xA8 }, { "copyright", 0xA9 }, { "ordfeminine", 0xAA }, { "guillemotleft", 0xAB },
    { "logicalnot", 0xAC }, { "registered", 0xAE }, { "macron", 0xAF }, { "degree", 0xB0 }, { "plusminus", 0xB1 },
    { "twosuperior", 0xB2 }, { "threesuperior", 0xB3 }, { "acute", 0xB4 }, { "mu", 0xB5 }, { "paragraph", 0xB6 },
    { "periodcentered", 0xB7 }, { "middot", 0xB7 }, { "cedilla", 0xB8 }, { "onesuperior", 0xB9 }, { "ordmasculine", 0xBA },
    { "guillemotright", 0xBB }, { "onequarter", 0xBC }, { "onehalf", 0xBD }, { "threequarters", 0xBE }, { "questiondown", 0xBF },
    { "multiply", 0xD7 }, { "divide", 0xF7 }, { "germandbls", 0xDF }, { "AE", 0xC6 }, { "ae", 0xE6 }, { "Oslash", 0xD8 },
    { "oslash", 0xF8 }, { "Eth", 0xD0 }, { "eth", 0xF0 }, { "Thorn", 0xDE }, { "thorn", 0xFE }, { "OE", 0x152 }, { "oe", 0x153 },
    { "Scaron", 0x160 }, { "scaron", 0x161 }, { "Zcaron", 0x17D }, { "zcaron", 0x17E }, { "Ydieresis", 0x178 },
    { "ydieresis", 0xFF }, { "dotlessi", 0x131 }, { "Lslash", 0x141 }, { "lslash", 0x142 }, { "florin", 0x192 },
    { "circumflex", 0x2C6 }, { "tilde", 0x2DC }, { "caron", 0x2C7 }, { "breve", 0x2D8 }, { "dotaccent", 0x2D9 }, { "ring", 0x2DA },
    { "ogonek", 0x2DB }, { "hungarumlaut", 0x2DD }, { "bullet", 0x2022 }, { "endash", 0x2013 }, { "emdash", 0x2014 },
    { "quotedblleft", 0x201C }, { "quotedblright", 0x201D }, { "quotesinglbase", 0x201A }, { "quotedblbase", 0x201E },
    { "ellipsis", 0x2026 }, { "dagger", 0x2020 }, { "daggerdbl", 0x2021 }, { "perthousand", 0x2030 }, { "guilsinglleft", 0x2039 },
    { "guilsinglright", 0x203A }, { "trademark", 0x2122 }, { "Euro", 0x20AC }, { "euro", 0x20AC }, { "fi", 0xFB01 },
    { "fl", 0xFB02 }, { "ff", 0xFB00 }, { "ffi", 0xFB03 }, { "ffl", 0xFB04 }, { "fraction", 0x2044 }, { "nbspace", 0xA0 },
    { "nonbreakingspace", 0xA0 }, { "arrowright", 0x2192 }, { "arrowleft", 0x2190 }, { "arrowup", 0x2191 }, { "arrowdown", 0x2193 },
    { "notequal", 0x2260 }, { "lessequal", 0x2264 }, { "greaterequal", 0x2265 }, { "infinity", 0x221E }, { "openbullet", 0x25E6 },
    { "checkmark", 0x2713 }, { "filledbox", 0x25A0 }, { "square", 0x25A1 }, { "circle", 0x25CB }, { "sfthyphen", 0xAD },
    { "softhyphen", 0xAD }, { "approxequal", 0x2248 }, { "Delta", 0x394 }, { "Omega", 0x3A9 }, { "pi", 0x3C0 },
};

/* the Latin-1 accented capitals; the small ones are +0x20 with a small name */
static const struct { const char *n; uint16_t cp; } ACC[] = {
    { "Agrave", 0xC0 }, { "Aacute", 0xC1 }, { "Acircumflex", 0xC2 }, { "Atilde", 0xC3 }, { "Adieresis", 0xC4 }, { "Aring", 0xC5 },
    { "Ccedilla", 0xC7 }, { "Egrave", 0xC8 }, { "Eacute", 0xC9 }, { "Ecircumflex", 0xCA }, { "Edieresis", 0xCB }, { "Igrave", 0xCC },
    { "Iacute", 0xCD }, { "Icircumflex", 0xCE }, { "Idieresis", 0xCF }, { "Ntilde", 0xD1 }, { "Ograve", 0xD2 }, { "Oacute", 0xD3 },
    { "Ocircumflex", 0xD4 }, { "Otilde", 0xD5 }, { "Odieresis", 0xD6 }, { "Ugrave", 0xD9 }, { "Uacute", 0xDA },
    { "Ucircumflex", 0xDB }, { "Udieresis", 0xDC }, { "Yacute", 0xDD } };

uint32_t of_pdf_glyph_cp(const char *n)
{
    if (!n[0]) return 0;
    if (!n[1] && ((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= 'A' && n[0] <= 'Z'))) return (uint8_t)n[0];
    if (!strncmp(n, "uni", 3) && strlen(n) >= 7) return (uint32_t)strtoul((char[5]){ n[3], n[4], n[5], n[6], 0 }, NULL, 16);
    if (n[0] == 'u' && strlen(n) >= 5 && strlen(n) <= 7) {
        char *e;
        unsigned long v = strtoul(n + 1, &e, 16);
        if (!*e) return (uint32_t)v;
    }
    for (size_t i = 0; i < sizeof GLYPHS / sizeof GLYPHS[0]; i++)
        if (!strcmp(n, GLYPHS[i].n)) return GLYPHS[i].cp;
    for (size_t i = 0; i < sizeof ACC / sizeof ACC[0]; i++) {
        if (!strcmp(n, ACC[i].n)) return ACC[i].cp;
        if ((n[0] | 32) == (ACC[i].n[0] | 32) && n[0] >= 'a' && !strcmp(n + 1, ACC[i].n + 1)) return ACC[i].cp + 0x20u;
    }
    /* "a.sc", "e.alt": the part before the dot */
    const char *dot = strchr(n, '.');
    if (dot && dot > n && dot - n < 31) {
        char b[32];
        memcpy(b, n, (size_t)(dot - n));
        b[dot - n] = 0;
        return of_pdf_glyph_cp(b);
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Fonts                                                                       */
/* -------------------------------------------------------------------------- */

static void put_utf8(char *o, size_t cap, const uint32_t *cp, int n)
{
    size_t k = 0;
    for (int i = 0; i < n; i++) {
        char t[4];
        int l = of_utf8_put(t, cp[i]);
        if (k + (size_t)l >= cap) break;
        memcpy(o + k, t, (size_t)l);
        k += (size_t)l;
    }
    o[k] = 0;
}

/* UTF-16BE (a CMap's destination) to code points */
static int utf16(const uint8_t *s, int n, uint32_t *out, int max)
{
    int k = 0;
    for (int i = 0; i + 1 < n && k < max; i += 2) {
        uint32_t u = (uint32_t)(s[i] << 8 | s[i + 1]);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
            uint32_t l = (uint32_t)(s[i + 2] << 8 | s[i + 3]);
            u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00);
            i += 2;
        }
        out[k++] = u;
    }
    if (n == 1 && k < max) out[k++] = s[0];
    return k;
}

static uint32_t bytes_code(const pv_t *v)
{
    uint32_t c = 0;
    for (int i = 0; i < v->s.n && i < 4; i++) c = c << 8 | (uint8_t)v->s.s[i];
    return c;
}

static void add_uc(font_t *F, uint32_t code, const uint32_t *cp, int n)
{
    if (!(F->nuc & 63)) {
        tuc_t *t = of_realloc(F->uc, (size_t)(F->nuc + 64) * sizeof *t);
        if (!t) return;
        F->uc = t;
    }
    F->uc[F->nuc].code = code;
    put_utf8(F->uc[F->nuc].u, sizeof F->uc[F->nuc].u, cp, n);
    F->nuc++;
}

static int uc_cmp(const void *a, const void *b)
{
    uint32_t x = ((const tuc_t *)a)->code, y = ((const tuc_t *)b)->code;
    return x < y ? -1 : x > y;
}

static void parse_cmap(pdf_t *P, font_t *F, const uint8_t *d, size_t n)
{
    lx_t L = { d, n, 0 };
    pv_t v[3];
    for (;;) {
        pv_t op;
        if (!pv_parse(NULL, &L, &op)) break;
        if (op.t != PV_OP) { pv_free(&op); continue; }
        int mode = !strcmp(op.s.s, "begincodespacerange") ? 1 : !strcmp(op.s.s, "beginbfchar") ? 2 : !strcmp(op.s.s, "beginbfrange") ? 3 : 0;
        pv_free(&op);
        if (!mode) continue;
        int per = mode == 3 ? 3 : 2;
        for (;;) {
            int got = 0;
            bool end = false;
            for (int k = 0; k < per; k++) {
                if (!pv_parse(NULL, &L, &v[k])) { end = true; break; }
                if (v[k].t == PV_OP) { pv_free(&v[k]); end = true; break; }
                got++;
            }
            if (end) { for (int k = 0; k < got; k++) pv_free(&v[k]); break; }
            if (mode == 1 && v[0].t == PV_STR && v[1].t == PV_STR && F->ncs < 8 && v[0].s.n >= 1 && v[0].s.n <= 4) {
                F->cs[F->ncs].len = (uint8_t)v[0].s.n;
                F->cs[F->ncs].lo = bytes_code(&v[0]);
                F->cs[F->ncs].hi = bytes_code(&v[1]);
                F->ncs++;
            } else if (mode == 2 && v[0].t == PV_STR) {
                uint32_t cp[8];
                int k = 0;
                if (v[1].t == PV_STR) k = utf16((const uint8_t *)v[1].s.s, v[1].s.n, cp, 8);
                else if (v[1].t == PV_NAME && (cp[0] = of_pdf_glyph_cp(v[1].s.s))) k = 1;
                if (k) add_uc(F, bytes_code(&v[0]), cp, k);
            } else if (mode == 3 && v[0].t == PV_STR && v[1].t == PV_STR) {
                uint32_t lo = bytes_code(&v[0]), hi = bytes_code(&v[1]);
                if (hi < lo || hi - lo > 65535) hi = lo;
                if (v[2].t == PV_STR) {
                    uint32_t cp[8];
                    int k = utf16((const uint8_t *)v[2].s.s, v[2].s.n, cp, 8);
                    if (k == 1) {
                        tur_t *t = of_realloc(F->ur, (size_t)(F->nur + 1) * sizeof *t);
                        if (t) { F->ur = t; F->ur[F->nur++] = (tur_t){ lo, hi, cp[0] }; }
                    } else if (k > 1 && hi - lo < 512) {
                        for (uint32_t c = lo; c <= hi; c++) {
                            add_uc(F, c, cp, k);
                            cp[k - 1]++;
                        }
                    }
                } else if (v[2].t == PV_ARR) {
                    for (int i = 0; i < v[2].a.n && lo + (uint32_t)i <= hi; i++) {
                        if (v[2].a.v[i].t != PV_STR) continue;
                        uint32_t cp[8];
                        int k = utf16((const uint8_t *)v[2].a.v[i].s.s, v[2].a.v[i].s.n, cp, 8);
                        if (k) add_uc(F, lo + (uint32_t)i, cp, k);
                    }
                }
            }
            for (int k = 0; k < per; k++) pv_free(&v[k]);
        }
    }
    if (F->nuc) qsort(F->uc, (size_t)F->nuc, sizeof *F->uc, uc_cmp);
    (void)P;
}

static void base_encoding(font_t *F, const char *name)
{
    for (int c = 0; c < 256; c++) {
        uint32_t u = (uint32_t)c;
        if (!strcmp(name, "MacRomanEncoding")) { if (c >= 128) u = MACROMAN[c - 128]; }
        else if (!strcmp(name, "StandardEncoding")) {
            if (c == 0x27) u = 0x2019;
            else if (c == 0x60) u = 0x2018;
            else if (c >= 128) {
                u = 0;
                for (size_t i = 0; i < sizeof STD_HI / sizeof STD_HI[0]; i++) if (STD_HI[i][0] == c) u = STD_HI[i][1];
            }
        } else {
            /* WinAnsi, and PDFDoc as near as it matters */
            if (c >= 0x80 && c < 0xA0) u = CP1252[c - 0x80];
        }
        if (c < 32) u = 0;
        F->enc[c] = u;
    }
}

/* strcasestr is not among the symbols the system gives apps */
static bool has_ci(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++)
        if (!strncasecmp(h, n, l)) return true;
    return false;
}

static void font_flags(font_t *F, const char *base, const pv_t *desc, pdf_t *P)
{
    if (base) {
        const char *b = strchr(base, '+');
        b = b && b - base == 6 ? b + 1 : base;
        if (has_ci(b, "bold") || has_ci(b, "black") || has_ci(b, "heavy") || has_ci(b, "semibold") || has_ci(b, "demi")) F->flags |= FL_BOLD;
        if (has_ci(b, "italic") || has_ci(b, "oblique") || strstr(b, "Kursiv")) F->flags |= FL_ITALIC;
        if (has_ci(b, "courier") || has_ci(b, "mono") || has_ci(b, "consolas") || has_ci(b, "menlo") || has_ci(b, "monaco")) F->flags |= FL_MONO;
        if (!strncmp(b, "Times", 5) || !strncmp(b, "TimesNewRoman", 13)) F->std = of_pdf_times;
        else if (!strncmp(b, "Courier", 7)) F->std = NULL;
        else F->std = F->flags & FL_BOLD ? of_pdf_helvb : of_pdf_helv;
    }
    if (desc) {
        int fl = pvi(pdf_get(P, desc, "Flags"), 0);
        if (fl & 64) F->flags |= FL_ITALIC;
        if (fl & 1) F->flags |= FL_MONO;
        if (fl & 262144) F->flags |= FL_BOLD;
        if (pv_num(pdf_get(P, desc, "FontWeight"), 0) >= 600) F->flags |= FL_BOLD;
        if (pv_num(pdf_get(P, desc, "ItalicAngle"), 0) < -5) F->flags |= FL_ITALIC;
    }
}

font_t *of_pdff_load(pdf_t *P, fonts_t *FS, const pv_t *ref)
{
    const pv_t *fd = pdf_res(P, ref);
    if (!fd || fd->t != PV_DICT) return NULL;
    int key = ref && ref->t == PV_REF ? ref->ref.num : -(int)((uintptr_t)fd & 0x7FFFFFF);
    for (int i = 0; i < FS->n; i++) if (FS->f[i]->key == key) return FS->f[i];
    if (FS->n == FS->cap) {
        int nc = FS->cap ? FS->cap * 2 : 16;
        font_t **n = of_realloc(FS->f, (size_t)nc * sizeof *n);
        if (!n) return NULL;
        FS->f = n;
        FS->cap = nc;
    }
    /* each font apart: the states keep pointers to them */
    font_t *F = of_alloc(sizeof *F);
    if (!F) return NULL;
    FS->f[FS->n++] = F;
    F->key = key;
    F->dict = fd;
    F->fm_a = F->fm_d = 0.001;
    const pv_t *sub = pdf_get(P, fd, "Subtype");
    F->type0 = pv_is_name(sub, "Type0");
    F->type3 = pv_is_name(sub, "Type3");
    const pv_t *base = pdf_get(P, fd, "BaseFont");
    const pv_t *desc = pdf_get(P, fd, "FontDescriptor");
    const pv_t *desc_font = fd;
    if (F->type0) {
        const pv_t *df = pdf_get(P, fd, "DescendantFonts");
        if (df && df->t == PV_ARR && df->a.n) desc_font = pdf_res(P, &df->a.v[0]);
        if (desc_font && desc_font->t == PV_DICT) desc = pdf_get(P, desc_font, "FontDescriptor");
        const pv_t *enc = pdf_get(P, fd, "Encoding");
        if (enc && enc->t == PV_NAME && (strstr(enc->s.s, "UCS2") || strstr(enc->s.s, "UTF16"))) F->ucs2 = true;
        /* the widths of CIDs: [c [w ...] c1 c2 w ...] */
        F->dw = (float)pv_num(desc_font ? pdf_get(P, desc_font, "DW") : NULL, 1000);
        const pv_t *W = desc_font ? pdf_get(P, desc_font, "W") : NULL;
        for (int i = 0; W && W->t == PV_ARR && i < W->a.n;) {
            const pv_t *a = pdf_res(P, &W->a.v[i]);
            const pv_t *b = i + 1 < W->a.n ? pdf_res(P, &W->a.v[i + 1]) : NULL;
            if (!a || !b) break;
            if (b->t == PV_ARR) {
                uint32_t c = (uint32_t)pv_num(a, 0);
                cw_t *n = of_realloc(F->cw, (size_t)(F->ncw + b->a.n) * sizeof *n);
                if (!n) break;
                F->cw = n;
                for (int k = 0; k < b->a.n; k++) F->cw[F->ncw++] = (cw_t){ c + (uint32_t)k, c + (uint32_t)k, (float)pv_num(pdf_res(P, &b->a.v[k]), F->dw) };
                i += 2;
            } else if (i + 2 < W->a.n) {
                cw_t *n = of_realloc(F->cw, (size_t)(F->ncw + 1) * sizeof *n);
                if (!n) break;
                F->cw = n;
                F->cw[F->ncw++] = (cw_t){ (uint32_t)pv_num(a, 0), (uint32_t)pv_num(b, 0), (float)pv_num(pdf_res(P, &W->a.v[i + 2]), F->dw) };
                i += 3;
            } else {
                break;
            }
        }
    }
    font_flags(F, base && base->t == PV_NAME ? base->s.s : NULL, desc && desc->t == PV_DICT ? desc : NULL, P);
    if (F->type3) {
        const pv_t *fm = pdf_get(P, fd, "FontMatrix");
        if (fm && fm->t == PV_ARR && fm->a.n >= 4) {
            F->fm_a = pv_num(&fm->a.v[0], 0.001);
            F->fm_d = pv_num(&fm->a.v[3], 0.001);
        }
    }
    if (!F->type0) {
        /* the encoding: a base, then the differences */
        int fl = desc && desc->t == PV_DICT ? pvi(pdf_get(P, desc, "Flags"), 0) : 0;
        bool symbolic = (fl & 4) && !(fl & 32);
        const pv_t *enc = pdf_get(P, fd, "Encoding");
        const char *bn = enc && enc->t == PV_NAME ? enc->s.s : NULL;
        if (enc && enc->t == PV_DICT) {
            const pv_t *be = pdf_get(P, enc, "BaseEncoding");
            if (be && be->t == PV_NAME) bn = be->s.s;
        }
        F->symbolic = (fl & 4) != 0;
        F->base_named = bn != NULL;
        if (!bn) bn = pv_is_name(sub, "TrueType") || symbolic ? "WinAnsiEncoding" : "StandardEncoding";
        base_encoding(F, bn);
        if (base && base->t == PV_NAME && (strstr(base->s.s, "Symbol") || strstr(base->s.s, "Dingbats"))) {
            for (int c = 0; c < 256; c++) F->enc[c] = 0;
            F->enc[0xB7] = 0x2022;
            F->enc[0x6C] = 0x25CF;
            F->enc[0x6E] = 0x25A0;
        }
        const pv_t *dif = enc && enc->t == PV_DICT ? pdf_get(P, enc, "Differences") : NULL;
        int code = 0;
        for (int i = 0; dif && dif->t == PV_ARR && i < dif->a.n; i++) {
            const pv_t *x = &dif->a.v[i];
            if (x->t == PV_INT) code = (int)x->i;
            else if (x->t == PV_NAME) {
                if (code >= 0 && code < 256) { F->enc[code] = of_pdf_glyph_cp(x->s.s); F->names[code] = x->s.s; }
                code++;
            }
        }
        /* widths */
        const pv_t *W = pdf_get(P, fd, "Widths");
        int first = pvi(pdf_get(P, fd, "FirstChar"), 0);
        if (W && W->t == PV_ARR) {
            F->has_w = true;
            float miss = (float)pv_num(desc && desc->t == PV_DICT ? pdf_get(P, desc, "MissingWidth") : NULL, 0);
            for (int c = 0; c < 256; c++) F->w[c] = miss;
            for (int i = 0; i < W->a.n && first + i < 256; i++)
                if (first + i >= 0) F->w[first + i] = (float)pv_num(pdf_res(P, &W->a.v[i]), miss);
        }
    }
    const pv_t *tu = pdf_get(P, fd, "ToUnicode");
    if (tu && tu->t == PV_STREAM) {
        size_t n;
        const pv_t *tr = pv_get(fd, "ToUnicode");
        uint8_t *d = pdf_stream(P, tu, tr && tr->t == PV_REF ? tr->ref.num : 0, &n, NULL);
        if (d) { parse_cmap(P, F, d, n); of_free(d); }
    }
    return F;
}

void of_pdff_free(fonts_t *FS)
{
    for (int i = 0; i < FS->n; i++) { of_free(FS->f[i]->uc); of_free(FS->f[i]->ur); of_free(FS->f[i]->cw); of_free(FS->f[i]); }
    of_free(FS->f);
    memset(FS, 0, sizeof *FS);
}

/* the next code of a string: its value and length */
uint32_t of_pdff_next(const font_t *F, const uint8_t *s, int n, int *i)
{
    if (!F->type0) return s[(*i)++];
    if (F->ncs) {
        for (int len = 1; len <= 4 && *i + len <= n; len++) {
            uint32_t c = 0;
            for (int k = 0; k < len; k++) c = c << 8 | s[*i + k];
            for (int r = 0; r < F->ncs; r++)
                if (F->cs[r].len == len && c >= F->cs[r].lo && c <= F->cs[r].hi) { *i += len; return c; }
        }
    }
    uint32_t c = (uint32_t)s[*i] << 8;
    if (*i + 1 < n) c |= s[*i + 1];
    *i += 2;
    return c;
}

const char *of_pdff_text(const font_t *F, uint32_t code, char *buf)
{
    if (F->nuc) {
        int lo = 0, hi = F->nuc - 1;
        while (lo <= hi) {
            int m = (lo + hi) / 2;
            if (F->uc[m].code == code) return F->uc[m].u;
            if (F->uc[m].code < code) lo = m + 1;
            else hi = m - 1;
        }
    }
    for (int i = 0; i < F->nur; i++)
        if (code >= F->ur[i].lo && code <= F->ur[i].hi) {
            uint32_t cp = F->ur[i].cp + (code - F->ur[i].lo);
            put_utf8(buf, 16, &cp, 1);
            return buf;
        }
    uint32_t cp = 0;
    if (!F->type0) cp = F->enc[code & 255];
    else if (F->ucs2) cp = code;
    if (!cp) return NULL;
    put_utf8(buf, 16, &cp, 1);
    return buf;
}

float of_pdff_width(const font_t *F, uint32_t code, const char *txt)
{
    if (F->type0) {
        for (int i = 0; i < F->ncw; i++) if (code >= F->cw[i].lo && code <= F->cw[i].hi) return F->cw[i].w;
        return F->dw;
    }
    if (F->has_w) return F->w[code & 255];
    if (!F->std) return 600;        /* Courier, and the unknown */
    int l;
    uint32_t cp = txt ? of_utf8_get(txt, &l) : code;
    return (float)of_pdf_width(F->std, cp);
}

/* -------------------------------------------------------------------------- */
/* Pages: runs, lines, segments                                                */
/* -------------------------------------------------------------------------- */

typedef struct { double a, b, c, d, e, f; } mtx_t;

static mtx_t mul(mtx_t m, mtx_t n)
{
    return (mtx_t){ m.a * n.a + m.b * n.c, m.a * n.b + m.b * n.d, m.c * n.a + m.d * n.c, m.c * n.b + m.d * n.d,
                    m.e * n.a + m.f * n.c + n.e, m.e * n.b + m.f * n.d + n.f };
}

typedef struct {
    float    x0, x1, y, size;
    uint8_t  flags, sup;
    uint32_t color;
    int      t0, tn;            /* its text in the page's buffer */
} span_t;

typedef struct {
    float  x0, x1, y, size;
    int    s0, ns;              /* spans, in order (page->order) */
    int    pic;                 /* a picture: its index, else -1 */
    int    chars;
    float  right;               /* where the lines of its column end */
    int    line;                /* the line it is part of */
    uint8_t drop, cells, bold, mono;
} seg_t;

typedef struct {
    char   path[112];
    float  x0, y0, x1, y1;
} pic_t;

typedef struct {
    span_t *sp;
    int     nsp, capsp;
    of_sb_t txt;
    int    *order;
    seg_t  *sg;
    int     nsg;
    pic_t  *pic;
    int     npic;
    float   w, h;
    int     chars;
} page_t;

typedef struct {
    mtx_t    ctm;
    font_t  *font;
    double   fs, tc, tw, th, tl, ts;
    int      tr;
    uint32_t color;
} gs_t;

typedef struct {
    pdf_t   *P;
    fonts_t  fonts;
    page_t  *pg;
    double   bx0, by0, bx1, by1;
    int      rot;
    gs_t     g[32];
    int      ng;
    mtx_t    tm, tlm;
    int      depth;
    int      missing;           /* glyphs with no text to say */
    int      vertical;          /* glyphs going up or down the page */
    int      npics;
    uint32_t file_id;
    struct { int obj; char path[112]; } done[64];
    int      ndone;
} ctx_t;

static void to_view(const ctx_t *C, double x, double y, double *vx, double *vy)
{
    switch (C->rot) {
    case 90:  *vx = y - C->by0; *vy = x - C->bx0; break;
    case 180: *vx = C->bx1 - x; *vy = y - C->by0; break;
    case 270: *vx = C->by1 - y; *vy = C->bx1 - x; break;
    default:  *vx = x - C->bx0; *vy = C->by1 - y; break;
    }
}

static span_t *new_span(page_t *pg)
{
    if (pg->nsp == pg->capsp) {
        int nc = pg->capsp ? pg->capsp * 2 : 256;
        span_t *n = of_realloc(pg->sp, (size_t)nc * sizeof *n);
        if (!n) return NULL;
        pg->sp = n;
        pg->capsp = nc;
    }
    span_t *s = &pg->sp[pg->nsp++];
    memset(s, 0, sizeof *s);
    s->t0 = (int)pg->txt.n;
    return s;
}

static void emit(ctx_t *C, const char *t, double x, double y, double xe, double size, uint8_t flags, uint32_t color)
{
    page_t *pg = C->pg;
    bool space = t[0] == ' ' && !t[1];
    /* ligatures and the like, as letters the fonts have */
    char buf[8];
    int l;
    uint32_t cp = of_utf8_get(t, &l);
    if (!t[l]) {
        static const char *const LIG[] = { "ff", "fi", "fl", "ffi", "ffl", "st", "st" };
        if (cp >= 0xFB00 && cp <= 0xFB06) t = LIG[cp - 0xFB00];
        else if (cp == 0xA0) { t = " "; space = true; }
        else if (cp == 0xAD) return;
        else if (cp >= 0xF000 && cp <= 0xF0FF) {
            /* symbol fonts mapped to the private area: bullets, as bullets */
            snprintf(buf, sizeof buf, "%s", cp == 0xF0B7 || cp == 0xF0A7 || cp == 0xF06C || cp == 0xF0D8 ? "\xE2\x80\xA2" : "");
            if (!buf[0]) return;
            t = buf;
        } else if (cp < 32) {
            return;
        }
    }
    span_t *s = pg->nsp ? &pg->sp[pg->nsp - 1] : NULL;
    if (s && s->flags == flags && s->color == color && DABS(s->size - size) < 0.06 * size && DABS(s->y - y) < 0.3 * size) {
        double gap = x - s->x1;
        if (gap > -0.6 * size && gap < 0.2 * size) {
            of_sb_puts(&pg->txt, t);
            s->tn = (int)pg->txt.n - s->t0;
            if (xe > s->x1) s->x1 = (float)xe;
            return;
        }
        if (gap >= 0.2 * size && gap < 1.6 * size) {
            if (!space && s->tn && pg->txt.s[pg->txt.n - 1] != ' ') of_sb_putc(&pg->txt, ' ');
            of_sb_puts(&pg->txt, t);
            s->tn = (int)pg->txt.n - s->t0;
            if (xe > s->x1) s->x1 = (float)xe;
            return;
        }
    }
    if (space) return;
    s = new_span(pg);
    if (!s) return;
    s->x0 = (float)x;
    s->x1 = (float)(xe > x ? xe : x + size * 0.3);
    s->y = (float)y;
    s->size = (float)size;
    s->flags = flags;
    s->color = color;
    of_sb_puts(&pg->txt, t);
    s->tn = (int)pg->txt.n - s->t0;
}

static void show(ctx_t *C, const pv_t *str)
{
    gs_t *g = &C->g[C->ng - 1];
    font_t *F = g->font;
    if (!F || str->t != PV_STR) return;
    const uint8_t *s = (const uint8_t *)str->s.s;
    int n = str->s.n;
    double th = g->th;
    for (int i = 0; i < n;) {
        bool one = !F->type0;
        uint32_t code = of_pdff_next(F, s, n, &i);
        char buf[16];
        const char *t = of_pdff_text(F, code, buf);
        float w = of_pdff_width(F, code, t);
        double w0 = F->type3 ? w * F->fm_a : w / 1000.0;
        mtx_t M = mul(C->tm, g->ctm);
        double scale = F->type3 ? F->fm_d / 0.001 : 1;
        double size = g->fs * scale * DSQRT(M.c * M.c + M.d * M.d);
        double x = g->ts * M.c + M.e, y = g->ts * M.d + M.f;
        double tx = (w0 * g->fs + g->tc + (one && code == 32 ? g->tw : 0)) * th;
        double xe = tx * M.a + g->ts * M.c + M.e, ye = tx * M.b + g->ts * M.d + M.f;
        C->tm.e += tx * C->tm.a;
        C->tm.f += tx * C->tm.b;
        if (!t) { if (code != 32) C->missing++; continue; }
        if (size < 1 || size > 400) continue;
        double vx, vy, vex, vey;
        to_view(C, x, y, &vx, &vy);
        to_view(C, xe, ye, &vex, &vey);
        /* text going up or down the page (labels on the side): left out */
        if ((DABS(vex - vx) < DABS(vey - vy) * 0.5 && DABS(vey - vy) > 0.05 * size) || vex < vx - 0.05 * size) { C->vertical++; continue; }
        uint8_t fl = F->flags;
        if (g->tr == 2 || g->tr == 6) fl |= FL_BOLD;
        emit(C, t, vx, vy, vex, size, fl, g->color);
        C->pg->chars++;
    }
}

/* -------------------------------------------------------------------------- */
/* Pictures                                                                    */
/* -------------------------------------------------------------------------- */

static bool write_png(const char *path, const uint8_t *px, int w, int h, int ch)
{
    size_t row = (size_t)w * (size_t)ch;
    uint8_t *raw = of_alloc((row + 1) * (size_t)h);
    if (!raw) return false;
    for (int y = 0; y < h; y++) memcpy(raw + y * (row + 1) + 1, px + (size_t)y * row, row);
    of_sb_t z = { 0 };
    of_sb_put(&z, "\x78\x01", 2);
    bool ok = of_deflate(raw, (row + 1) * (size_t)h, &z);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < (row + 1) * (size_t)h; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    uint32_t ad = b << 16 | a;
    char be[4] = { (char)(ad >> 24), (char)(ad >> 16), (char)(ad >> 8), (char)ad };
    of_sb_put(&z, be, 4);
    of_free(raw);
    of_sb_t o = { 0 };
    of_sb_put(&o, "\x89PNG\r\n\x1a\n", 8);
    uint8_t ih[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16),
                       (uint8_t)(h >> 8), (uint8_t)h, 8, (uint8_t)(ch == 1 ? 0 : 2), 0, 0, 0 };
    const struct { const char *t; const uint8_t *d; size_t n; } C[3] = { { "IHDR", ih, 13 }, { "IDAT", (const uint8_t *)z.s, z.n }, { "IEND", NULL, 0 } };
    for (int i = 0; i < 3; i++) {
        char ln[4] = { (char)(C[i].n >> 24), (char)(C[i].n >> 16), (char)(C[i].n >> 8), (char)C[i].n };
        of_sb_put(&o, ln, 4);
        size_t at = o.n;
        of_sb_put(&o, C[i].t, 4);
        if (C[i].n) of_sb_put(&o, (const char *)C[i].d, C[i].n);
        uint32_t crc = of_crc32(0, o.s + at, o.n - at);
        char cb[4] = { (char)(crc >> 24), (char)(crc >> 16), (char)(crc >> 8), (char)crc };
        of_sb_put(&o, cb, 4);
    }
    ok = ok && !o.oom && of_file_write(path, o.s, o.n);
    of_sb_free(&z);
    of_sb_free(&o);
    return ok;
}

/* The picture's file in the cache (once per picture object); false if it
 * cannot be shown (a mask, JPEG 2000, fax, a colour space the app lacks). */
static bool pic_file(ctx_t *C, const pv_t *im, int num, char *out, size_t len)
{
    for (int i = 0; i < C->ndone; i++)
        if (C->done[i].obj == num && num > 0) { snprintf(out, len, "%s", C->done[i].path); return out[0] != 0; }
    pdf_t *P = C->P;
    out[0] = 0;
    const pv_t *mask = pdf_get(P, im, "ImageMask");
    int w = pvi(pdf_get(P, im, "Width"), 0), h = pvi(pdf_get(P, im, "Height"), 0);
    bool ok = false;
    if (!(mask && mask->t == PV_BOOL && mask->b) && w > 0 && h > 0 && (int64_t)w * h <= 8000000) {
        const char *imf = NULL;
        size_t n;
        uint8_t *d = pdf_stream(P, im, num, &n, &imf);
        char dir[160], name[64];
        extern const char *aos_hal_path_data(void);
#ifdef OF_HOST
        snprintf(dir, sizeof dir, "/tmp/of_cache");
#else
        snprintf(dir, sizeof dir, "%s/oficina/cache", aos_hal_path_data());
#endif
        if (d && imf && !strcmp(imf, "DCTDecode")) {
            snprintf(name, sizeof name, "pdf%08lx_%d.jpg", (unsigned long)C->file_id, num);
            ok = of_cache_file(name, d, n, out, len);
        } else if (d && !imf) {
            /* raw samples: grey or RGB rows, 8 bits */
            const pv_t *cs = pdf_get(P, im, "ColorSpace");
            int bpc = pvi(pdf_get(P, im, "BitsPerComponent"), 8);
            int comps = 0;
            const uint8_t *lut = NULL;
            int lut_comps = 0, hival = 0;
            uint8_t *lut_own = NULL;
            if (pv_is_name(cs, "DeviceGray") || pv_is_name(cs, "CalGray") || pv_is_name(cs, "G")) comps = 1;
            else if (pv_is_name(cs, "DeviceRGB") || pv_is_name(cs, "CalRGB") || pv_is_name(cs, "RGB")) comps = 3;
            else if (pv_is_name(cs, "DeviceCMYK") || pv_is_name(cs, "CMYK")) comps = 4;
            else if (cs && cs->t == PV_ARR && cs->a.n >= 1) {
                const pv_t *k = pdf_res(P, &cs->a.v[0]);
                if (pv_is_name(k, "ICCBased") && cs->a.n >= 2) comps = pvi(pdf_get(P, pdf_res(P, &cs->a.v[1]), "N"), 3);
                else if (pv_is_name(k, "CalRGB")) comps = 3;
                else if (pv_is_name(k, "CalGray")) comps = 1;
                else if ((pv_is_name(k, "Indexed") || pv_is_name(k, "I")) && cs->a.n >= 4) {
                    const pv_t *bs = pdf_res(P, &cs->a.v[1]);
                    lut_comps = pv_is_name(bs, "DeviceGray") ? 1 : pv_is_name(bs, "DeviceCMYK") ? 4 : 3;
                    if (bs && bs->t == PV_ARR && bs->a.n >= 2 && pv_is_name(pdf_res(P, &bs->a.v[0]), "ICCBased"))
                        lut_comps = pvi(pdf_get(P, pdf_res(P, &bs->a.v[1]), "N"), 3);
                    hival = pvi(pdf_res(P, &cs->a.v[2]), 255);
                    const pv_t *lt = pdf_res(P, &cs->a.v[3]);
                    if (lt && lt->t == PV_STR) lut = (const uint8_t *)lt->s.s;
                    else if (lt && lt->t == PV_STREAM) {
                        size_t ln;
                        lut_own = pdf_stream(P, lt, cs->a.v[3].t == PV_REF ? cs->a.v[3].ref.num : 0, &ln, NULL);
                        lut = lut_own;
                    }
                    if (lut) comps = -1;
                }
            }
            const pv_t *dec = pdf_get(P, im, "Decode");
            bool invert = dec && dec->t == PV_ARR && dec->a.n >= 2 && pv_num(&dec->a.v[0], 0) > 0.5;
            int in_comps = comps < 0 ? 1 : comps;
            int out_ch = comps == 1 || (comps < 0 && lut_comps == 1) ? 1 : 3;
            size_t rowbits = (size_t)w * (size_t)in_comps * (size_t)bpc;
            size_t rowbytes = (rowbits + 7) / 8;
            if (comps != 0 && (bpc == 8 || ((bpc == 1 || bpc == 2 || bpc == 4) && in_comps == 1)) && n >= rowbytes * (size_t)h) {
                uint8_t *px = of_alloc((size_t)w * (size_t)h * (size_t)out_ch);
                if (px) {
                    for (int y = 0; y < h; y++) {
                        const uint8_t *r = d + (size_t)y * rowbytes;
                        for (int x = 0; x < w; x++) {
                            uint8_t *o = px + ((size_t)y * (size_t)w + (size_t)x) * (size_t)out_ch;
                            int v[4] = { 0, 0, 0, 0 };
                            for (int k = 0; k < in_comps; k++) {
                                size_t bit = ((size_t)x * (size_t)in_comps + (size_t)k) * (size_t)bpc;
                                int val = bpc == 8 ? r[bit / 8] : (r[bit / 8] >> (8 - bpc - (int)(bit % 8))) & ((1 << bpc) - 1);
                                v[k] = val;
                            }
                            if (comps < 0) {
                                int ix = v[0] > hival ? hival : v[0];
                                const uint8_t *e = lut + ix * lut_comps;
                                if (lut_comps == 1) o[0] = e[0];
                                else if (lut_comps == 4) {
                                    int k = e[3];
                                    o[0] = (uint8_t)((255 - e[0]) * (255 - k) / 255);
                                    o[1] = (uint8_t)((255 - e[1]) * (255 - k) / 255);
                                    o[2] = (uint8_t)((255 - e[2]) * (255 - k) / 255);
                                } else { o[0] = e[0]; o[1] = e[1]; o[2] = e[2]; }
                            } else if (comps == 1) {
                                int g = bpc == 8 ? v[0] : v[0] * 255 / ((1 << bpc) - 1);
                                o[0] = (uint8_t)(invert ? 255 - g : g);
                            } else if (comps == 4) {
                                o[0] = (uint8_t)((255 - v[0]) * (255 - v[3]) / 255);
                                o[1] = (uint8_t)((255 - v[1]) * (255 - v[3]) / 255);
                                o[2] = (uint8_t)((255 - v[2]) * (255 - v[3]) / 255);
                            } else {
                                o[0] = (uint8_t)v[0]; o[1] = (uint8_t)v[1]; o[2] = (uint8_t)v[2];
                            }
                        }
                    }
                    snprintf(name, sizeof name, "pdf%08lx_%d.png", (unsigned long)C->file_id, num);
                    mkdir(dir, 0777);
                    snprintf(out, len, "%.150s/%.60s", dir, name);
                    ok = write_png(out, px, w, h, out_ch);
                    of_free(px);
                }
            }
            of_free(lut_own);
        }
        of_free(d);
        (void)dir;
    }
    if (!ok) out[0] = 0;
    if (C->ndone < 64 && num > 0) {
        C->done[C->ndone].obj = num;
        snprintf(C->done[C->ndone].path, sizeof C->done[C->ndone].path, "%s", out);
        C->ndone++;
    }
    return ok;
}

static void picture(ctx_t *C, const pv_t *im, int num)
{
    if (C->npics >= MAX_PICS) return;
    mtx_t m = C->g[C->ng - 1].ctm;
    double xs[4] = { m.e, m.a + m.e, m.c + m.e, m.a + m.c + m.e }, ys[4] = { m.f, m.b + m.f, m.d + m.f, m.b + m.d + m.f };
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = 0; i < 4; i++) {
        double vx, vy;
        to_view(C, xs[i], ys[i], &vx, &vy);
        if (vx < x0) x0 = vx;
        if (vx > x1) x1 = vx;
        if (vy < y0) y0 = vy;
        if (vy > y1) y1 = vy;
    }
    if (x1 - x0 < 24 || y1 - y0 < 24) return;          /* rules, bullets, decoration */
    char path[112];
    if (!pic_file(C, im, num, path, sizeof path)) return;
    page_t *pg = C->pg;
    pic_t *n = of_realloc(pg->pic, (size_t)(pg->npic + 1) * sizeof *n);
    if (!n) return;
    pg->pic = n;
    pic_t *p = &pg->pic[pg->npic++];
    snprintf(p->path, sizeof p->path, "%s", path);
    p->x0 = (float)x0; p->y0 = (float)y0; p->x1 = (float)x1; p->y1 = (float)y1;
    C->npics++;
}

/* -------------------------------------------------------------------------- */
/* The content stream                                                          */
/* -------------------------------------------------------------------------- */

static void run(ctx_t *C, const uint8_t *d, size_t n, const pv_t *res);

static uint32_t rgb(double r, double g, double b)
{
    int R = di(r * 255 + 0.5), G = di(g * 255 + 0.5), B = di(b * 255 + 0.5);
    if (R < 0) R = 0;
    if (G < 0) G = 0;
    if (B < 0) B = 0;
    if (R > 255) R = 255;
    if (G > 255) G = 255;
    if (B > 255) B = 255;
    /* near black and grey are "no colour" */
    if (R < 80 && G < 80 && B < 80) return 0;
    if (abs(R - G) < 16 && abs(G - B) < 16 && R < 140) return 0;
    if (R > 235 && G > 235 && B > 235) return 0;      /* white text: shown as text */
    return 0x1000000u | (uint32_t)(R << 16 | G << 8 | B);
}

static void xobject(ctx_t *C, const pv_t *res, const char *name)
{
    const pv_t *xd = pdf_get(C->P, res, "XObject");
    const pv_t *ref = pv_get(xd, name);
    const pv_t *x = pdf_res(C->P, ref);
    if (!x || x->t != PV_STREAM) return;
    int num = ref && ref->t == PV_REF ? ref->ref.num : 0;
    const pv_t *st = pdf_get(C->P, x, "Subtype");
    if (pv_is_name(st, "Image")) { picture(C, x, num); return; }
    if (!pv_is_name(st, "Form") || C->depth > 6) return;
    size_t len;
    uint8_t *d = pdf_stream(C->P, x, num, &len, NULL);
    if (!d) return;
    if (C->ng < 32) { C->g[C->ng] = C->g[C->ng - 1]; C->ng++; }
    const pv_t *m = pdf_get(C->P, x, "Matrix");
    if (m && m->t == PV_ARR && m->a.n == 6) {
        mtx_t fm = { pv_num(&m->a.v[0], 1), pv_num(&m->a.v[1], 0), pv_num(&m->a.v[2], 0), pv_num(&m->a.v[3], 1), pv_num(&m->a.v[4], 0), pv_num(&m->a.v[5], 0) };
        C->g[C->ng - 1].ctm = mul(fm, C->g[C->ng - 1].ctm);
    }
    const pv_t *fr = pdf_get(C->P, x, "Resources");
    mtx_t tm = C->tm, tlm = C->tlm;
    C->depth++;
    run(C, d, len, fr && fr->t == PV_DICT ? fr : res);
    C->depth--;
    C->tm = tm;
    C->tlm = tlm;
    if (C->ng > 1) C->ng--;
    of_free(d);
}

static void run(ctx_t *C, const uint8_t *d, size_t n, const pv_t *res)
{
    lx_t L = { d, n, 0 };
    pv_t op[16];
    int nop = 0;
    for (;;) {
        pv_t v;
        if (!pv_parse(NULL, &L, &v)) break;
        if (v.t != PV_OP) {
            if (nop == 16) { pv_free(&op[0]); memmove(op, op + 1, 15 * sizeof op[0]); nop--; }
            op[nop++] = v;
            continue;
        }
        const char *o = v.s.s;
        gs_t *g = &C->g[C->ng - 1];
        double a0 = nop > 0 ? pv_num(&op[0], 0) : 0, a1 = nop > 1 ? pv_num(&op[1], 0) : 0;
        switch (o[0]) {
        case 'q': if (!o[1] && C->ng < 32) { C->g[C->ng] = *g; C->ng++; } break;
        case 'Q': if (!o[1] && C->ng > 1) C->ng--; break;
        case 'c':
            if (!strcmp(o, "cm") && nop >= 6) {
                mtx_t m = { pv_num(&op[nop - 6], 1), pv_num(&op[nop - 5], 0), pv_num(&op[nop - 4], 0), pv_num(&op[nop - 3], 1),
                            pv_num(&op[nop - 2], 0), pv_num(&op[nop - 1], 0) };
                g->ctm = mul(m, g->ctm);
            }
            break;
        case 'B':
            if (!strcmp(o, "BT")) { C->tm = C->tlm = (mtx_t){ 1, 0, 0, 1, 0, 0 }; }
            else if (!strcmp(o, "BI")) {
                /* an inline picture: up to "ID", its data, "EI" */
                for (;;) {
                    pv_t x;
                    if (!pv_parse(NULL, &L, &x)) break;
                    bool id = x.t == PV_OP && !strcmp(x.s.s, "ID");
                    pv_free(&x);
                    if (id) break;
                }
                L.p++;
                while (L.p + 2 < L.n && !(L.b[L.p] == 'E' && L.b[L.p + 1] == 'I' && L.b[L.p - 1] <= ' ' && L.b[L.p + 2] <= ' ')) L.p++;
                L.p += 2;
            }
            break;
        case 'T':
            switch (o[1]) {
            case 'f':
                if (nop >= 2 && op[nop - 2].t == PV_NAME) {
                    const pv_t *fonts = pdf_get(C->P, res, "Font");
                    g->font = of_pdff_load(C->P, &C->fonts, pv_get(fonts, op[nop - 2].s.s));
                    g->fs = pv_num(&op[nop - 1], 12);
                }
                break;
            case 'c': g->tc = a0; break;
            case 'w': g->tw = a0; break;
            case 'z': g->th = a0 / 100.0; break;
            case 'L': g->tl = a0; break;
            case 'r': g->tr = di(a0); break;
            case 's': g->ts = a0; break;
            case 'd': case 'D':
                if (o[1] == 'D') g->tl = -a1;
                C->tlm = mul((mtx_t){ 1, 0, 0, 1, a0, a1 }, C->tlm);
                C->tm = C->tlm;
                break;
            case 'm':
                if (nop >= 6) {
                    C->tlm = (mtx_t){ pv_num(&op[nop - 6], 1), pv_num(&op[nop - 5], 0), pv_num(&op[nop - 4], 0), pv_num(&op[nop - 3], 1),
                                      pv_num(&op[nop - 2], 0), pv_num(&op[nop - 1], 0) };
                    C->tm = C->tlm;
                }
                break;
            case '*':
                C->tlm = mul((mtx_t){ 1, 0, 0, 1, 0, -g->tl }, C->tlm);
                C->tm = C->tlm;
                break;
            case 'j': if (nop >= 1) show(C, &op[nop - 1]); break;
            case 'J':
                if (nop >= 1 && op[nop - 1].t == PV_ARR) {
                    const pv_t *a = &op[nop - 1];
                    for (int i = 0; i < a->a.n; i++) {
                        if (a->a.v[i].t == PV_STR) show(C, &a->a.v[i]);
                        else {
                            double tx = -pv_num(&a->a.v[i], 0) / 1000.0 * g->fs * g->th;
                            C->tm.e += tx * C->tm.a;
                            C->tm.f += tx * C->tm.b;
                        }
                    }
                }
                break;
            }
            break;
        case '\'': case '"':
            C->tlm = mul((mtx_t){ 1, 0, 0, 1, 0, -g->tl }, C->tlm);
            C->tm = C->tlm;
            if (o[0] == '"' && nop >= 3) { g->tw = a0; g->tc = a1; }
            if (nop >= 1) show(C, &op[nop - 1]);
            break;
        case 'D': if (!strcmp(o, "Do") && nop >= 1 && op[nop - 1].t == PV_NAME) xobject(C, res, op[nop - 1].s.s); break;
        case 'r': if (!strcmp(o, "rg") && nop >= 3) g->color = rgb(pv_num(&op[nop - 3], 0), pv_num(&op[nop - 2], 0), pv_num(&op[nop - 1], 0)); break;
        case 'g': if (!o[1] && nop >= 1) g->color = 0; break;
        case 'k':
            if (!o[1] && nop >= 4) {
                double c = pv_num(&op[nop - 4], 0), m = pv_num(&op[nop - 3], 0), y = pv_num(&op[nop - 2], 0), k = pv_num(&op[nop - 1], 0);
                g->color = rgb((1 - c) * (1 - k), (1 - m) * (1 - k), (1 - y) * (1 - k));
            }
            break;
        case 's':
            if ((!strcmp(o, "sc") || !strcmp(o, "scn"))) {
                int k = 0;
                while (k < nop && (op[k].t == PV_INT || op[k].t == PV_REAL)) k++;
                if (k == 3) g->color = rgb(pv_num(&op[0], 0), pv_num(&op[1], 0), pv_num(&op[2], 0));
                else if (k == 4) {
                    double c = pv_num(&op[0], 0), m = pv_num(&op[1], 0), y = pv_num(&op[2], 0), kk = pv_num(&op[3], 0);
                    g->color = rgb((1 - c) * (1 - kk), (1 - m) * (1 - kk), (1 - y) * (1 - kk));
                } else g->color = 0;
            }
            break;
        default: break;
        }
        pv_free(&v);
        for (int i = 0; i < nop; i++) pv_free(&op[i]);
        nop = 0;
    }
    for (int i = 0; i < nop; i++) pv_free(&op[i]);
}

/* -------------------------------------------------------------------------- */
/* Lines and segments of a page                                                */
/* -------------------------------------------------------------------------- */

static page_t *s_sort_pg;

static int sp_cmp_y(const void *a, const void *b)
{
    const span_t *x = &s_sort_pg->sp[*(const int *)a], *y = &s_sort_pg->sp[*(const int *)b];
    if (x->y != y->y) return x->y < y->y ? -1 : 1;
    return x->x0 < y->x0 ? -1 : x->x0 > y->x0;
}

static int sp_cmp_x(const void *a, const void *b)
{
    const span_t *x = &s_sort_pg->sp[*(const int *)a], *y = &s_sort_pg->sp[*(const int *)b];
    return x->x0 < y->x0 ? -1 : x->x0 > y->x0;
}

static void add_seg(page_t *pg, seg_t s)
{
    seg_t *n = of_realloc(pg->sg, (size_t)(pg->nsg + 1) * sizeof *n);
    if (!n) return;
    pg->sg = n;
    pg->sg[pg->nsg++] = s;
}

static void segments(page_t *pg)
{
    if (!pg->nsp && !pg->npic) return;
    pg->order = of_alloc((size_t)(pg->nsp + 1) * sizeof(int));
    if (!pg->order) return;
    for (int i = 0; i < pg->nsp; i++) pg->order[i] = i;
    s_sort_pg = pg;
    qsort(pg->order, (size_t)pg->nsp, sizeof(int), sp_cmp_y);
    seg_t *tmp = NULL;
    int ntmp = 0;
    for (int i = 0; i < pg->nsp;) {
        /* a line: the spans near the first one's baseline */
        const span_t *f = &pg->sp[pg->order[i]];
        float ly = f->y, lsz = f->size;
        int j = i + 1;
        while (j < pg->nsp) {
            const span_t *s = &pg->sp[pg->order[j]];
            float m = s->size > lsz ? s->size : lsz;
            if (fabsf(s->y - ly) > 0.5f * m) break;
            if (s->size > lsz) { lsz = s->size; ly = s->y; }
            j++;
        }
        qsort(pg->order + i, (size_t)(j - i), sizeof(int), sp_cmp_x);
        /* drop the same text drawn twice (fake bold, shadows) */
        int w = i;
        for (int k = i; k < j; k++) {
            const span_t *s = &pg->sp[pg->order[k]];
            if (w > i) {
                const span_t *p = &pg->sp[pg->order[w - 1]];
                if (p->tn == s->tn && fabsf(p->x0 - s->x0) < 0.3f * s->size && !memcmp(pg->txt.s + p->t0, pg->txt.s + s->t0, (size_t)s->tn)) continue;
            }
            pg->order[w++] = pg->order[k];
        }
        /* segments: a wide gap splits (columns, cells) */
        int k = i;
        while (k < w) {
            seg_t sg = { 0 };
            sg.pic = -1;
            sg.s0 = k;
            const span_t *s = &pg->sp[pg->order[k]];
            sg.x0 = s->x0; sg.x1 = s->x1; sg.y = ly; sg.size = lsz;
            int bold_chars = 0, mono_chars = 0;
            int m = k;
            while (m < w) {
                const span_t *t = &pg->sp[pg->order[m]];
                if (m > k && t->x0 - sg.x1 > 1.6f * lsz) break;
                if (t->x1 > sg.x1) sg.x1 = t->x1;
                sg.chars += t->tn;
                if (t->flags & FL_BOLD) bold_chars += t->tn;
                if (t->flags & FL_MONO) mono_chars += t->tn;
                m++;
            }
            sg.ns = m - k;
            sg.line = i;
            sg.bold = sg.chars && bold_chars * 10 >= sg.chars * 9;
            sg.mono = sg.chars && mono_chars * 10 >= sg.chars * 9;
            for (int q = k; q < m; q++) {
                span_t *t = &pg->sp[pg->order[q]];
                if (t->size < 0.85f * lsz) t->sup = t->y < ly - 0.15f * lsz ? 1 : t->y > ly + 0.1f * lsz ? 2 : 0;
            }
            seg_t *nt = of_realloc(tmp, (size_t)(ntmp + 1) * sizeof *nt);
            if (nt) { tmp = nt; tmp[ntmp++] = sg; }
            k = m;
        }
        /* cells: segments of the same line */
        int first = ntmp;
        while (first > 0 && tmp[first - 1].s0 >= i) first--;
        for (int q = first; q < ntmp; q++) tmp[q].cells = (uint8_t)(ntmp - first > 1 ? ntmp - first : 1);
        i = j;
    }
    /* reading order: two columns when they are there */
    float mid = pg->w / 2;
    int left = 0, right = 0, span = 0, lchars = 0, rchars = 0, lfar = 0;
    for (int q = 0; q < ntmp; q++) {
        if (tmp[q].x1 < mid + 0.02f * pg->w) { left++; lchars += tmp[q].chars; if (tmp[q].x1 > 0.3f * pg->w) lfar++; }
        else if (tmp[q].x0 > mid - 0.02f * pg->w) { right++; rchars += tmp[q].chars; }
        else span++;
    }
    /* prose on both sides, the left lines reaching the gutter: not a table's cells */
    bool cols = left >= 3 && right >= 3 && span * 3 < left + right && lchars >= left * 15 && rchars >= right * 15 && lfar * 3 >= left * 2;
    if (cols) {
        /* cells of a line count on their own side of the gutter */
        for (int q = 0; q < ntmp; q++) {
            int n = 0;
            bool lq = (tmp[q].x0 + tmp[q].x1) / 2 < mid;
            for (int r = 0; r < ntmp; r++)
                if (tmp[r].line == tmp[q].line && ((tmp[r].x0 + tmp[r].x1) / 2 < mid) == lq) n++;
            tmp[q].cells = (uint8_t)(n ? n : 1);
        }
    }
    /* pictures as segments, by their top */
    for (int p = 0; p < pg->npic; p++) {
        seg_t sg = { 0 };
        sg.pic = p;
        sg.x0 = pg->pic[p].x0; sg.x1 = pg->pic[p].x1; sg.y = pg->pic[p].y1 + 0.5f; sg.size = 12;
        seg_t *nt = of_realloc(tmp, (size_t)(ntmp + 1) * sizeof *nt);
        if (nt) { tmp = nt; tmp[ntmp++] = sg; }
    }
    if (!cols) {
        /* by line, pictures where they fall */
        for (int q = 0; q < ntmp; q++)
            for (int r = q; r > 0 && tmp[r].y < tmp[r - 1].y - 0.01f && tmp[r].pic >= 0; r--) {
                seg_t t = tmp[r]; tmp[r] = tmp[r - 1]; tmp[r - 1] = t;
            }
        for (int q = 0; q < ntmp; q++) add_seg(pg, tmp[q]);
    } else {
        /* bands between full-width segments: the left column, then the right */
        int *used = of_alloc((size_t)(ntmp + 1) * sizeof(int));
        float top = -1e9f;
        for (;;) {
            /* the next full-width segment below top */
            float next = 1e9f;
            int nf = -1;
            for (int q = 0; q < ntmp; q++)
                if (!used[q] && tmp[q].x0 < mid - 0.02f * pg->w && tmp[q].x1 > mid + 0.02f * pg->w && tmp[q].y > top && tmp[q].y < next) { next = tmp[q].y; nf = q; }
            for (int side = 0; side < 2; side++) {
                for (;;) {
                    int best = -1;
                    for (int q = 0; q < ntmp; q++) {
                        if (used[q] || tmp[q].y >= next) continue;
                        bool l = (tmp[q].x0 + tmp[q].x1) / 2 < mid;
                        if (l != (side == 0)) continue;
                        if (best < 0 || tmp[q].y < tmp[best].y || (tmp[q].y == tmp[best].y && tmp[q].x0 < tmp[best].x0)) best = q;
                    }
                    if (best < 0) break;
                    used[best] = 1;
                    add_seg(pg, tmp[best]);
                }
            }
            if (nf < 0) break;
            used[nf] = 1;
            add_seg(pg, tmp[nf]);
            top = next;
        }
        of_free(used);
    }
    of_free(tmp);
    /* the right edge of the text, by column: where most long lines end */
    float rl = 0, rr = 0;
    /* the lines of the page's main size only: a wide heading is not the edge */
    float main_sz = 0;
    int main_chars = 0;
    for (int q = 0; q < pg->nsg; q++) {
        int c = 0;
        if (pg->sg[q].pic >= 0) continue;
        for (int r = 0; r < pg->nsg; r++)
            if (pg->sg[r].pic < 0 && fabsf(pg->sg[r].size - pg->sg[q].size) < 0.1f * pg->sg[q].size) c += pg->sg[r].chars;
        if (c > main_chars) { main_chars = c; main_sz = pg->sg[q].size; }
    }
    for (int side = 0; side < 2; side++) {
        float xs[256];
        int nxs = 0;
        for (int q = 0; q < pg->nsg && nxs < 256; q++) {
            seg_t *g = &pg->sg[q];
            if (g->pic >= 0 || g->chars < 20 || fabsf(g->size - main_sz) > 0.1f * main_sz) continue;
            bool r = cols && (g->x0 + g->x1) / 2 >= mid;
            if (r == (side == 1)) xs[nxs++] = g->x1;
        }
        /* the 80th percentile */
        for (int a = 1; a < nxs; a++)
            for (int z = a; z > 0 && xs[z] < xs[z - 1]; z--) { float t = xs[z]; xs[z] = xs[z - 1]; xs[z - 1] = t; }
        float v = nxs ? xs[(nxs * 8) / 10 < nxs ? (nxs * 8) / 10 : nxs - 1] : 0;
        if (side) rr = v;
        else rl = v;
    }
    for (int q = 0; q < pg->nsg; q++) {
        seg_t *g = &pg->sg[q];
        g->right = cols && (g->x0 + g->x1) / 2 >= mid ? rr : (cols && g->x1 > mid + 0.02f * pg->w ? (rr > rl ? rr : rl) : rl);
    }
}

/* -------------------------------------------------------------------------- */
/* Pages of the document                                                       */
/* -------------------------------------------------------------------------- */

static void walk(pdf_t *P, const pv_t *node, const pv_t *res, const double *box, int rot, int depth, pref_t **out, int *n)
{
    if (!node || node->t != PV_DICT || depth > 32 || *n >= MAX_PAGES) return;
    const pv_t *r = pdf_get(P, node, "Resources");
    if (r && r->t == PV_DICT) res = r;
    double b[4] = { box[0], box[1], box[2], box[3] };
    const pv_t *mb = pdf_get(P, node, "CropBox");
    if (!mb || mb->t != PV_ARR) mb = pdf_get(P, node, "MediaBox");
    if (mb && mb->t == PV_ARR && mb->a.n == 4)
        for (int i = 0; i < 4; i++) b[i] = pv_num(pdf_res(P, &mb->a.v[i]), b[i]);
    const pv_t *ro = pdf_get(P, node, "Rotate");
    if (ro) rot = (pvi(ro, 0) % 360 + 360) % 360;
    const pv_t *kids = pdf_get(P, node, "Kids");
    if (kids && kids->t == PV_ARR) {
        for (int i = 0; i < kids->a.n; i++) walk(P, pdf_res(P, &kids->a.v[i]), res, b, rot, depth + 1, out, n);
        return;
    }
    pref_t *nn = of_realloc(*out, (size_t)(*n + 1) * sizeof *nn);
    if (!nn) return;
    *out = nn;
    nn[*n].page = node;
    nn[*n].res = res;
    if (b[0] > b[2]) { double t = b[0]; b[0] = b[2]; b[2] = t; }
    if (b[1] > b[3]) { double t = b[1]; b[1] = b[3]; b[3] = t; }
    memcpy(nn[*n].box, b, sizeof b);
    nn[*n].rot = rot;
    (*n)++;
}

int of_pdf_pages(pdf_t *P, pref_t **out)
{
    const pv_t *pages = pdf_get(P, P->catalog, "Pages");
    int n = 0;
    double box[4] = { 0, 0, 612, 792 };
    *out = NULL;
    walk(P, pages, NULL, box, 0, 0, out, &n);
    if (!n) {
        /* no page tree: every /Type /Page object, in file order */
        for (int i = 1; i < P->nx && n < MAX_PAGES; i++) {
            pv_t *o = pdf_obj(P, i);
            if (o && o->t == PV_DICT && pv_is_name(pdf_get(P, o, "Type"), "Page")) walk(P, o, NULL, box, 0, 0, out, &n);
        }
    }
    return n;
}

static void page_free(page_t *pg);

static void gs_reset(ctx_t *C)
{
    C->ng = 1;
    memset(&C->g[0], 0, sizeof C->g[0]);
    C->g[0].ctm = (mtx_t){ 1, 0, 0, 1, 0, 0 };
    C->g[0].th = 1;
    C->tm = C->tlm = (mtx_t){ 1, 0, 0, 1, 0, 0 };
}

static void page_read(ctx_t *C, const pref_t *pr, page_t *pg)
{
    memset(pg, 0, sizeof *pg);
    C->pg = pg;
    C->bx0 = pr->box[0]; C->by0 = pr->box[1]; C->bx1 = pr->box[2]; C->by1 = pr->box[3];
    C->rot = pr->rot;
    C->vertical = 0;
    bool side = C->rot == 90 || C->rot == 270;
    pg->w = (float)(side ? C->by1 - C->by0 : C->bx1 - C->bx0);
    pg->h = (float)(side ? C->bx1 - C->bx0 : C->by1 - C->by0);
    C->ng = 1;
    memset(&C->g[0], 0, sizeof C->g[0]);
    C->g[0].ctm = (mtx_t){ 1, 0, 0, 1, 0, 0 };
    C->g[0].th = 1;
    C->tm = C->tlm = (mtx_t){ 1, 0, 0, 1, 0, 0 };
    const pv_t *cref = pv_get(pr->page, "Contents");
    const pv_t *c = pdf_res(C->P, cref);
    of_sb_t all = { 0 };
    if (c && c->t == PV_STREAM) {
        size_t n;
        uint8_t *d = pdf_stream(C->P, c, cref && cref->t == PV_REF ? cref->ref.num : 0, &n, NULL);
        if (d) { of_sb_put(&all, (const char *)d, n); of_free(d); }
    } else if (c && c->t == PV_ARR) {
        for (int i = 0; i < c->a.n; i++) {
            const pv_t *s = pdf_res(C->P, &c->a.v[i]);
            size_t n;
            uint8_t *d = s ? pdf_stream(C->P, s, c->a.v[i].t == PV_REF ? c->a.v[i].ref.num : 0, &n, NULL) : NULL;
            if (d) { of_sb_put(&all, (const char *)d, n); of_sb_putc(&all, '\n'); of_free(d); }
        }
    }
    if (all.n) run(C, (const uint8_t *)all.s, all.n, pr->res);
    /* a page whose text runs up or down: read again turned, the way most of it reads */
    if (C->vertical > 20 && C->vertical > pg->chars * 2) {
        int best = C->rot, best_chars = pg->chars;
        for (int k = 1; k < 4; k++) {
            int rot = (pr->rot + 90 * k) % 360;
            page_t tmp;
            memset(&tmp, 0, sizeof tmp);
            C->pg = &tmp;
            C->rot = rot;
            C->vertical = 0;
            gs_reset(C);
            int saved = C->npics;
            run(C, (const uint8_t *)all.s, all.n, pr->res);
            C->npics = saved;
            if (tmp.chars > best_chars) { best_chars = tmp.chars; best = rot; }
            page_free(&tmp);
        }
        if (best != C->rot || best_chars > pg->chars) {
            page_free(pg);
            C->pg = pg;
            C->rot = best;
            bool sd2 = best == 90 || best == 270;
            pg->w = (float)(sd2 ? C->by1 - C->by0 : C->bx1 - C->bx0);
            pg->h = (float)(sd2 ? C->bx1 - C->bx0 : C->by1 - C->by0);
            gs_reset(C);
            run(C, (const uint8_t *)all.s, all.n, pr->res);
        }
        C->pg = pg;
    }
    of_sb_free(&all);
    /* a page scanned with its text under it: the text, not the scan */
    if (pg->chars > 40)
        for (int p = 0; p < pg->npic; p++)
            if ((pg->pic[p].x1 - pg->pic[p].x0) * (pg->pic[p].y1 - pg->pic[p].y0) > 0.6f * pg->w * pg->h) {
                memmove(&pg->pic[p], &pg->pic[p + 1], (size_t)(pg->npic - p - 1) * sizeof *pg->pic);
                pg->npic--;
                p--;
            }
    segments(pg);
}

static void page_free(page_t *pg)
{
    of_free(pg->sp);
    of_sb_free(&pg->txt);
    of_free(pg->order);
    of_free(pg->sg);
    of_free(pg->pic);
    memset(pg, 0, sizeof *pg);
}

/* -------------------------------------------------------------------------- */
/* Segments to paragraphs                                                      */
/* -------------------------------------------------------------------------- */

/* a segment's text, joined (for keys and tests) */
static void seg_text(const page_t *pg, const seg_t *s, char *out, size_t len)
{
    size_t k = 0;
    for (int i = 0; i < s->ns && k + 1 < len; i++) {
        const span_t *t = &pg->sp[pg->order[s->s0 + i]];
        if (i && k + 1 < len && out[k - 1] != ' ') out[k++] = ' ';
        for (int c = 0; c < t->tn && k + 1 < len; c++) out[k++] = pg->txt.s[t->t0 + c];
    }
    out[k] = 0;
}

/* running heads and page numbers: digits as '#', then compared */
static uint32_t key_hash(const char *t, bool *lone_number)
{
    uint32_t h = 2166136261u;
    char last = 0;
    int letters = 0;
    for (const char *p = t; *p; p++) {
        char c = *p >= '0' && *p <= '9' ? '#' : (*p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p);
        if (c == '#' && last == '#') continue;
        if (c == ' ') continue;
        if (c != '#' && c != '-' && c != '/' && c != '.' && c != '(' && c != ')') letters++;
        h = (h ^ (uint8_t)c) * 16777619u;
        last = c;
    }
    bool digit = false;
    for (const char *p = t; *p; p++) if (*p >= '0' && *p <= '9') digit = true;
    *lone_number = letters <= 6 && digit;
    return h;
}

static bool is_bullet_start(const char *t, int *skip)
{
    static const char *const B[] = { "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA", "\xE2\x80\xA3", "\xE2\x97\x8F", "\xE2\x97\x8B",
                                     "\xE2\x96\xA0", "\xE2\x96\xA1", "\xE2\x80\x93", "-", "\xC2\xB7", "*" };
    for (size_t i = 0; i < sizeof B / sizeof B[0]; i++) {
        size_t l = strlen(B[i]);
        if (!strncmp(t, B[i], l) && (t[l] == ' ' || (l > 1 && t[l] && t[l] != ' '))) {
            *skip = (int)l;
            while (t[*skip] == ' ') (*skip)++;
            return true;
        }
    }
    /* "1." "12)" "a)" start a paragraph, and keep their number */
    int k = 0;
    while (t[k] >= '0' && t[k] <= '9' && k < 3) k++;
    if (k && (t[k] == '.' || t[k] == ')') && t[k + 1] == ' ') { *skip = 0; return true; }
    if (t[0] >= 'a' && t[0] <= 'z' && t[1] == ')' && t[2] == ' ') { *skip = 0; return true; }
    return false;
}

typedef struct {
    bool  open;
    int   heading, cells;
    float last_y, last_x0, last_x1, x0, x1max, size, right;
    int   lines;
    bool  list;
} para_t;

static void close_para(of_bld_t *b, para_t *ps)
{
    ps->open = false;
    b->list = 0;
    b->heading = 0;
    b->align = 0;
    b->cur = -1;
}

static void append_seg(of_bld_t *b, const page_t *pg, const seg_t *s, int skip, bool heading)
{
    for (int i = 0; i < s->ns; i++) {
        const span_t *t = &pg->sp[pg->order[s->s0 + i]];
        const char *tx = pg->txt.s + t->t0;
        int n = t->tn;
        if (i == 0 && skip) { tx += skip; n -= skip; }
        if (n <= 0) continue;
        of_rstyle_t st = { .link = -1 };
        if (!heading) {
            if (t->flags & FL_BOLD) st.b = OF_TRI_ON;
            if (t->flags & FL_ITALIC) st.i = OF_TRI_ON;
        } else if (t->flags & FL_ITALIC) {
            st.i = OF_TRI_ON;
        }
        st.sup = t->sup;
        st.color = t->color;
        if (i) {
            /* a space between runs that were apart */
            const span_t *p = &pg->sp[pg->order[s->s0 + i - 1]];
            if (t->x0 - p->x1 > 0.15f * t->size && !t->sup) b->space = true;
        }
        b->st = st;
        of_bld_text(b, tx, (size_t)n);
    }
    b->st = (of_rstyle_t){ .link = -1 };
}

of_doc_t *of_pdf_load(const char *path, const char *password, char *err, size_t errlen)
{
    pdf_t *P = pdf_open(path, password, err, errlen);
    if (!P) return NULL;
    pref_t *pr = NULL;
    int npr = of_pdf_pages(P, &pr);
    if (!npr) { pdf_close(P); snprintf(err, errlen, "damaged"); return NULL; }
    ctx_t *C = of_alloc(sizeof *C);
    page_t *pgs = of_alloc((size_t)npr * sizeof *pgs);
    if (!C || !pgs) { of_free(C); of_free(pgs); of_free(pr); pdf_close(P); snprintf(err, errlen, "memory"); return NULL; }
    C->P = P;
    C->file_id = of_crc32(0, path, strlen(path)) ^ (uint32_t)P->len;
    int chars = 0;
    for (int i = 0; i < npr; i++) {
        page_read(C, &pr[i], &pgs[i]);
        chars += pgs[i].chars;
    }
    /* the body's size: the one with the most letters */
    struct { int sz2, n; } hist[64];
    int nh = 0;
    /* running heads and feet: keys in the top and bottom 8%, by page */
    struct { uint32_t h; int pages, last; } keys[256];
    int nk = 0;
    for (int i = 0; i < npr; i++) {
        page_t *pg = &pgs[i];
        for (int q = 0; q < pg->nsg; q++) {
            seg_t *s = &pg->sg[q];
            if (s->pic >= 0) continue;
            int sz2 = (int)(s->size * 2 + 0.5f);
            int h = 0;
            while (h < nh && hist[h].sz2 != sz2) h++;
            if (h == nh && nh < 64) { hist[nh].sz2 = sz2; hist[nh].n = 0; nh++; }
            if (h < nh) hist[h].n += s->chars;
            if (s->y < 0.08f * pg->h || s->y > 0.92f * pg->h) {
                char t[160];
                seg_text(pg, s, t, sizeof t);
                bool lone;
                uint32_t k = key_hash(t, &lone);
                if (lone && s->chars < 24) { s->drop = 1; continue; }
                int f = 0;
                while (f < nk && keys[f].h != k) f++;
                if (f == nk && nk < 256) { keys[nk].h = k; keys[nk].pages = 0; keys[nk].last = -1; nk++; }
                if (f < nk && keys[f].last != i) { keys[f].pages++; keys[f].last = i; }
            }
        }
    }
    /* the usual distance between lines, in sizes */
    int lead_h[40] = { 0 };
    for (int i = 0; i < npr; i++) {
        page_t *pg = &pgs[i];
        for (int q = 1; q < pg->nsg; q++) {
            seg_t *a = &pg->sg[q - 1], *c = &pg->sg[q];
            if (a->pic >= 0 || c->pic >= 0 || DABS(a->size - c->size) > 0.05f * c->size) continue;
            float r = (c->y - a->y) / c->size;
            if (r > 0.8f && r < 2.8f) lead_h[(int)((r - 0.8f) * 20)]++;
        }
    }
    int lb = 0;
    for (int k = 1; k < 40; k++) if (lead_h[k] > lead_h[lb]) lb = k;
    /* the closest spacing that is common (paragraph gaps can outnumber lines in a short text) */
    for (int k = 0; k < lb; k++) if (lead_h[k] * 4 >= lead_h[lb] && lead_h[k]) { lb = k; break; }
    float lead = lead_h[lb] ? 0.8f + (lb + 0.5f) / 20.0f : 1.25f;
    float body = 10;
    int best = -1;
    for (int h = 0; h < nh; h++) if (best < 0 || hist[h].n > hist[best].n) best = h;
    if (best >= 0) body = hist[best].sz2 / 2.0f;
    int need = npr >= 3 ? (npr * 2 + 4) / 5 : 1000000;
    if (need < 3) need = 3;
    for (int i = 0; i < npr; i++) {
        page_t *pg = &pgs[i];
        for (int q = 0; q < pg->nsg; q++) {
            seg_t *s = &pg->sg[q];
            if (s->pic >= 0 || s->drop || !(s->y < 0.08f * pg->h || s->y > 0.92f * pg->h)) continue;
            char t[160];
            seg_text(pg, s, t, sizeof t);
            bool lone;
            uint32_t k = key_hash(t, &lone);
            for (int f = 0; f < nk; f++) if (keys[f].h == k && keys[f].pages >= need) s->drop = 1;
        }
    }

    /* the document */
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    of_bld_t b;
    of_bld_init(&b, d);
    para_t ps = { 0 };
    for (int i = 0; i < npr; i++) {
        page_t *pg = &pgs[i];
        bool first_text = true;
        /* where fixed-width lines start: their indent is kept */
        float mono_left = 1e9f;
        for (int q = 0; q < pg->nsg; q++) if (pg->sg[q].pic < 0 && pg->sg[q].mono && pg->sg[q].x0 < mono_left) mono_left = pg->sg[q].x0;
        for (int q = 0; q < pg->nsg; q++) {
            seg_t *s = &pg->sg[q];
            if (s->drop) continue;
            if (s->pic >= 0) {
                pic_t *p = &pg->pic[s->pic];
                close_para(&b, &ps);
                b.align = OF_AL_CENTER;
                of_bld_para(&b);
                of_bld_image(&b, p->path, true, (int)(p->x1 - p->x0), (int)(p->y1 - p->y0));
                close_para(&b, &ps);
                continue;
            }
            char t[200];
            seg_text(pg, s, t, sizeof t);
            if (!t[0] || (t[0] == ' ' && !t[1])) continue;
            int heading = 0;
            float r = s->size / body;
            if (s->chars < 240) heading = r >= 1.6f ? 1 : r >= 1.3f ? 2 : r >= 1.12f ? 3 : 0;
            size_t tl = strlen(t);
            if (!heading && s->bold && s->chars >= 3 && s->chars <= 80 && s->cells <= 1 && fabsf(r - 1) < 0.12f &&
                t[tl - 1] != ',' && t[tl - 1] != '-' && t[tl - 1] != ';')
                heading = 3;
            int skip = 0;
            bool bullet = !heading && is_bullet_start(t, &skip);
            /* the first text of a page: does it go on with the last paragraph? */
            if (first_text) {
                first_text = false;
                bool goes_on = false;
                if (ps.open && !ps.heading && !heading && !bullet && fabsf(s->size - ps.size) < 0.15f * s->size) {
                    of_para_t *lp = b.cur >= 0 ? &d->p[b.cur] : NULL;
                    char lc = lp && lp->len ? lp->txt[lp->len - 1] : '.';
                    goes_on = lc != '.' && lc != ':' && lc != '!' && lc != '?';
                }
                if (i > 0 && !goes_on) { close_para(&b, &ps); of_bld_break(&b); }
                if (goes_on) {
                    /* joined below as the next line, whatever the distance */
                    ps.last_y = s->y - s->size * 1.2f;
                    ps.last_x0 = s->x0;
                    ps.x0 = s->x0;
                    ps.x1max = s->x1;
                    ps.last_x1 = s->x1;
                }
            }
            float dy = s->y - ps.last_y;
#ifdef OF_HOST
            if (getenv("OF_PDF_DBG"))
                fprintf(stderr, "y=%.1f x0=%.1f x1=%.1f sz=%.1f cells=%d right=%.1f dy=%.1f lead=%.2f ps.x0=%.1f last_x1=%.1f x1max=%.1f open=%d h=%d | %.30s\n",
                        s->y, s->x0, s->x1, s->size, s->cells, s->right, dy, lead, ps.x0, ps.last_x1, ps.x1max, ps.open, heading, t);
#endif
            bool same_line = ps.open && fabsf(dy) < 0.3f * s->size && s->x0 > ps.last_x1;
            bool join = false;
            if (same_line && !heading) {
                join = true;
            } else if (ps.open && heading && ps.heading == heading && dy > 0 && dy < 1.7f * s->size && fabsf(s->size - ps.size) < 0.1f * s->size) {
                join = true;
            } else if (ps.open && !heading && !ps.heading && !bullet && s->cells <= 1 && ps.cells <= 1 && !s->mono &&
                       (b.cur < 0 || d->p[b.cur].len < 6000) &&
                       fabsf(s->size - ps.size) < 0.15f * s->size && dy > 0.5f * s->size && dy < (lead * 1.22f + 0.04f) * s->size &&
                       s->x0 > ps.x0 - 1.0f * s->size && s->x0 < ps.x0 + 6 * s->size &&
                       !(ps.lines >= 2 && s->x0 > ps.x0 + 1.5f * s->size && s->x0 > ps.last_x0 + 1.5f * s->size) &&
                       !(ps.last_x1 < ps.right - 7 * s->size) && !(ps.lines >= 1 && ps.last_x1 < ps.x1max - 6 * s->size)) {
                join = true;
            }
            if (!join) {
                close_para(&b, &ps);
                b.heading = heading;
                if (heading && fabsf((s->x0 + s->x1) / 2 - pg->w / 2) < 0.05f * pg->w && s->x0 > 0.15f * pg->w) b.align = OF_AL_CENTER;
                if (bullet && skip) { b.list = OF_LIST_BULLET; b.numid = 9000; }
                of_bld_para(&b);
                ps.open = true;
                ps.heading = heading;
                ps.x0 = s->x0;
                ps.x1max = s->x1;
                ps.size = s->size;
                ps.lines = 0;
                ps.cells = s->cells;
                ps.list = bullet;
                if (s->mono && s->x0 > mono_left + 0.3f * s->size && b.cur >= 0) {
                    int sp = (int)((s->x0 - mono_left) / (0.6f * s->size) + 0.5f);
                    for (int k = 0; k < sp && k < 60; k++) of_para_append(d, &d->p[b.cur], " ", 1, 0);
                }
                append_seg(&b, pg, s, bullet ? skip : 0, heading != 0);
            } else if (same_line) {
                /* another cell of the row */
                of_para_t *lp = b.cur >= 0 ? &d->p[b.cur] : NULL;
                if (lp) of_para_append(d, lp, "   ", 3, 0);
                b.space = false;
                append_seg(&b, pg, s, 0, heading != 0);
                ps.cells = 2;
                if (s->x1 > ps.x1max) ps.x1max = s->x1;
                ps.last_x1 = s->x1;
                continue;
            } else {
                /* the next line of the paragraph: a hyphen at the end joins the word */
                of_para_t *lp = b.cur >= 0 ? &d->p[b.cur] : NULL;
                bool hy = false;
                if (lp && lp->len >= 2 && lp->txt[lp->len - 1] == '-' && lp->txt[lp->len - 2] != ' ' && t[0] >= 'a' && t[0] <= 'z') {
                    lp->txt[--lp->len] = 0;
                    hy = true;
                }
                b.space = !hy;
                append_seg(&b, pg, s, 0, heading != 0);
                if (s->x1 > ps.x1max) ps.x1max = s->x1;
            }
            ps.lines++;
            ps.right = s->right;
            ps.last_y = s->y;
            ps.last_x0 = s->x0;
            ps.last_x1 = s->x1;
        }
        page_free(pg);
    }
    close_para(&b, &ps);
    of_bld_done(&b);
    snprintf(d->kind, sizeof d->kind, "PDF");
    char note[160];
    int np = npr;
    if (!chars && C->npics) snprintf(note, sizeof note, _("%d páginas. El PDF no tiene texto: son imágenes (escaneado)"), np);
    else if (!chars) snprintf(note, sizeof note, _("%d páginas. El PDF no tiene texto que se pueda leer"), np);
    else if (C->missing > chars / 4) snprintf(note, sizeof note, _("%d páginas. Parte del texto no se puede leer: sus letras no dicen qué son"), np);
    else snprintf(note, sizeof note, _("%d páginas"), np);
    d->note = of_strdup(note);
    if (!chars && !C->npics) {
        b.cur = -1;
        of_bld_para(&b);
        b.st.color = 0x1808080;
        const char *m = _("Este PDF no tiene texto que la app pueda leer (puede ser una imagen escaneada en un formato que la app no muestra).");
        of_bld_text(&b, m, strlen(m));
        of_bld_done(&b);
    }
    of_pdff_free(&C->fonts);
    of_free(C);
    of_free(pgs);
    of_free(pr);
    pdf_close(P);
    return d;
}
