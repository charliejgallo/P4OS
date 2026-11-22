/*
 * P4OS - Office: writing PDF (export of a document, a sheet, a text).
 *
 * A4 pages, the standard fonts (Helvetica and Courier in their four
 * styles, WinAnsi: Spanish, German and the rest of Western Europe; what
 * WinAnsi lacks goes out as '?'), so nothing is embedded but the pictures:
 * JPEG as it is (DCTDecode), PNG's data as it is with the PNG predictor
 * (grey, RGB and palette), PNG with alpha decoded and its alpha as a soft
 * mask. Headings, bold, italics, underline, strikethrough, colours,
 * highlight, sizes, super- and subscript, alignment (justified too), list
 * labels, indents, tables with borders (cells laid out in their columns,
 * rows kept whole across pages), pictures scaled to the column, page
 * breaks, hyperlinks as link annotations, page numbers, the title in the
 * document's information. Content streams are Flate-compressed.
 */
#include "of_doc.h"
#include "of_pdf.h"
#include "of_slides.h"

#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PW 595.28f
#define PH 841.89f
#define MARGIN 56.7f

/* -------------------------------------------------------------------------- */
/* Text to WinAnsi                                                             */
/* -------------------------------------------------------------------------- */

static uint8_t to_ansi(uint32_t cp)
{
    if (cp < 128) return (uint8_t)cp;
    if (cp >= 0xA0 && cp <= 0xFF) return (uint8_t)cp;
    static const uint16_t HI[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };
    for (int i = 0; i < 32; i++) if (HI[i] == cp) return (uint8_t)(0x80 + i);
    switch (cp) {
    case 0x2212: return '-';
    case 0x2192: return '>';
    case 0x2190: return '<';
    case 0x25E6: case 0x25AA: case 0x25CF: case 0x25A0: return 0x95;
    case 0x25CB: case 0x25A1: return 'o';
    case 0xFB01: return 'f';        /* ligatures were split before */
    default: return '?';
    }
}

/* the code point of a WinAnsi byte, for the widths */
static uint32_t from_ansi(uint8_t c)
{
    static const uint16_t HI[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };
    return c >= 0x80 && c < 0xA0 ? HI[c - 0x80] : c;
}

/* -------------------------------------------------------------------------- */
/* The file                                                                    */
/* -------------------------------------------------------------------------- */

enum { F_REG = 0, F_BOLD, F_IT, F_BOLDIT, F_MONO, F_MONOB, NFONTS };
static const char *const FONT_NAMES[NFONTS] = { "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique", "Courier", "Courier-Bold" };

typedef struct { float x0, y0, x1, y1; int link; } annot_t;

typedef struct {
    of_sb_t   out;
    uint32_t *off;              /* by object number */
    int       nobj, cap;
    int       font_obj[NFONTS];
    int      *kids;
    int       nkids;
    of_sb_t   page;             /* the page being drawn */
    annot_t  *ann;
    int       nann;
    struct { int obj; int img; } *pics;    /* pictures written: the doc's object -> XObject */
    int       npics;
    bool      page_open;
    float     y;                /* the pen, from the top */
    float     pw, ph;
    float     left, right, top, bottom;
    int       pageno;
    const of_doc_t *d;
    bool      mono;
    float     base;             /* body size, points */
} pw_t;

static int new_obj(pw_t *W)
{
    if (W->nobj + 1 >= W->cap) {
        int nc = W->cap ? W->cap * 2 : 64;
        uint32_t *n = of_realloc(W->off, (size_t)nc * sizeof *n);
        if (!n) return 0;
        W->off = n;
        W->cap = nc;
    }
    W->nobj++;
    W->off[W->nobj] = 0;
    return W->nobj;
}

static void begin_obj(pw_t *W, int num)
{
    W->off[num] = (uint32_t)W->out.n;
    of_sb_printf(&W->out, "%d 0 obj\n", num);
}

/* a stream object, Flate-compressed when it pays */
static void put_stream(pw_t *W, int num, const char *dict, const void *data, size_t n, bool compress)
{
    begin_obj(W, num);
    of_sb_t z = { 0 };
    bool zl = false;
    if (compress && n > 64) {
        of_sb_put(&z, "\x78\x01", 2);
        if (of_deflate(data, n, &z)) {
            uint32_t a = 1, b = 0;
            const uint8_t *p = data;
            for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
            uint32_t ad = b << 16 | a;
            char be[4] = { (char)(ad >> 24), (char)(ad >> 16), (char)(ad >> 8), (char)ad };
            of_sb_put(&z, be, 4);
            zl = !z.oom;
        }
    }
    of_sb_printf(&W->out, "<<%s%s /Length %lu>>\nstream\n", dict ? dict : "", zl ? " /Filter /FlateDecode" : "",
                 (unsigned long)(zl ? z.n : n));
    of_sb_put(&W->out, zl ? z.s : (const char *)data, zl ? z.n : n);
    of_sb_puts(&W->out, "\nendstream\nendobj\n");
    of_sb_free(&z);
}

static void str_lit(of_sb_t *o, const uint8_t *s, size_t n)
{
    of_sb_putc(o, '(');
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == '(' || c == ')' || c == '\\') { of_sb_putc(o, '\\'); of_sb_putc(o, (char)c); }
        else if (c < 32) of_sb_printf(o, "\\%03o", c);
        else of_sb_putc(o, (char)c);
    }
    of_sb_putc(o, ')');
}

/* a UTF-8 string for the information dictionary: UTF-16BE with its mark */
static void str_utf16(of_sb_t *o, const char *s)
{
    of_sb_puts(o, "<FEFF");
    for (const char *p = s; *p;) {
        int l;
        uint32_t cp = of_utf8_get(p, &l);
        p += l;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            of_sb_printf(o, "%04X%04X", (unsigned)(0xD800 + (cp >> 10)), (unsigned)(0xDC00 + (cp & 0x3FF)));
        } else {
            of_sb_printf(o, "%04X", (unsigned)cp);
        }
    }
    of_sb_putc(o, '>');
}

static void page_end(pw_t *W)
{
    if (!W->page_open) return;
    /* the page number (not on slides) */
    char num[16];
    if (W->pageno > 0) {
    int nl = snprintf(num, sizeof num, "%d", W->pageno);
    float w = 0;
    for (int i = 0; i < nl; i++) w += of_pdf_helv[num[i] - 32] * 9 / 1000.0f;
    of_sb_printf(&W->page, "BT /F1 9 Tf 0.5 g %.2f %.2f Td ", (W->pw - w) / 2, W->bottom / 2 - 3);
    str_lit(&W->page, (const uint8_t *)num, (size_t)nl);
    of_sb_puts(&W->page, " Tj ET\n");
    }
    int content = new_obj(W);
    put_stream(W, content, NULL, W->page.s ? W->page.s : "", W->page.n, true);
    int page = new_obj(W);
    begin_obj(W, page);
    of_sb_printf(&W->out, "<</Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Contents %d 0 R /Resources <</Font <<",
                 W->pw, W->ph, content);
    for (int f = 0; f < NFONTS; f++) of_sb_printf(&W->out, " /F%d %d 0 R", f + 1, W->font_obj[f]);
    of_sb_puts(&W->out, ">> /XObject <<");
    for (int i = 0; i < W->npics; i++) if (W->pics[i].img > 0) of_sb_printf(&W->out, " /Im%d %d 0 R", W->pics[i].img, W->pics[i].img);
    of_sb_puts(&W->out, ">>>>");
    if (W->nann) {
        of_sb_puts(&W->out, " /Annots [");
        for (int i = 0; i < W->nann; i++) {
            const annot_t *a = &W->ann[i];
            const char *url = W->d && a->link < W->d->nlinks ? W->d->links[a->link] : "";
            of_sb_printf(&W->out, "<</Type /Annot /Subtype /Link /Border [0 0 0] /Rect [%.2f %.2f %.2f %.2f] /A <</S /URI /URI ",
                         a->x0, a->y0, a->x1, a->y1);
            str_lit(&W->out, (const uint8_t *)url, strlen(url));
            of_sb_puts(&W->out, ">>>> ");
        }
        of_sb_puts(&W->out, "]");
    }
    of_sb_puts(&W->out, ">>\nendobj\n");
    int *k = of_realloc(W->kids, (size_t)(W->nkids + 1) * sizeof *k);
    if (k) { W->kids = k; W->kids[W->nkids++] = page; }
    W->page.n = 0;
    W->nann = 0;
    W->page_open = false;
}

static void page_begin(pw_t *W)
{
    if (W->page_open) page_end(W);
    W->page_open = true;
    W->pageno++;
    W->y = W->top;
}

/* room for h points more, or a new page */
static void need(pw_t *W, float h)
{
    if (!W->page_open) page_begin(W);
    else if (W->y + h > W->ph - W->bottom && W->y > W->top + 1) page_begin(W);
}

/* -------------------------------------------------------------------------- */
/* Pictures                                                                    */
/* -------------------------------------------------------------------------- */

static uint8_t *png_unfilter(uint8_t *d, size_t n, int w, int h, int bpp, size_t *rowlen)
{
    size_t row = (size_t)w * (size_t)bpp;
    if (n < (row + 1) * (size_t)h) return NULL;
    uint8_t *o = of_alloc(row * (size_t)h + 1);
    if (!o) return NULL;
    for (int r = 0; r < h; r++) {
        const uint8_t *in = d + (size_t)r * (row + 1);
        uint8_t ft = in[0], *out = o + (size_t)r * row, *up = r ? out - row : NULL;
        in++;
        for (size_t i = 0; i < row; i++) {
            int a = i >= (size_t)bpp ? out[i - (size_t)bpp] : 0, b = up ? up[i] : 0, c = up && i >= (size_t)bpp ? up[i - (size_t)bpp] : 0, x = in[i];
            if (ft == 1) x += a;
            else if (ft == 2) x += b;
            else if (ft == 3) x += (a + b) / 2;
            else if (ft == 4) {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                x += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            }
            out[i] = (uint8_t)x;
        }
    }
    *rowlen = row;
    return o;
}

/* The picture as an XObject (written once); its pixel size. 0: cannot. */
/* a picture's bytes (JPEG or PNG) as an XObject; 0: cannot */
static int picture_data(pw_t *W, const uint8_t *b, size_t n, const char *ext, int *pw, int *ph)
{
    int img = 0, w = 0, h = 0;
    if (b && ext && !strcmp(ext, "jpeg")) {
        int comps = 3;
        for (size_t i = 2; i + 9 < n;) {
            if (b[i] != 0xFF) { i++; continue; }
            uint8_t m = b[i + 1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                h = b[i + 5] << 8 | b[i + 6];
                w = b[i + 7] << 8 | b[i + 8];
                comps = b[i + 9];
                break;
            }
            i += 2 + (size_t)(b[i + 2] << 8 | b[i + 3]);
        }
        if (w > 0 && h > 0) {
            img = new_obj(W);
            char dict[200];
            snprintf(dict, sizeof dict, "/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /%s /BitsPerComponent 8 /Filter /DCTDecode%s",
                     w, h, comps == 1 ? "DeviceGray" : comps == 4 ? "DeviceCMYK" : "DeviceRGB", comps == 4 ? " /Decode [1 0 1 0 1 0 1 0]" : "");
            put_stream(W, img, dict, b, n, false);
        }
    } else if (b && ext && !strcmp(ext, "png") && n > 33) {
        w = (int)((uint32_t)b[16] << 24 | (uint32_t)b[17] << 16 | (uint32_t)b[18] << 8 | b[19]);
        h = (int)((uint32_t)b[20] << 24 | (uint32_t)b[21] << 16 | (uint32_t)b[22] << 8 | b[23]);
        int depth = b[24], ct = b[25], inter = b[28];
        of_sb_t idat = { 0 }, plte = { 0 };
        for (size_t p = 8; p + 12 <= n;) {
            uint32_t l = (uint32_t)b[p] << 24 | (uint32_t)b[p + 1] << 16 | (uint32_t)b[p + 2] << 8 | b[p + 3];
            if (p + 12 + l > n) break;
            if (!memcmp(b + p + 4, "IDAT", 4)) of_sb_put(&idat, (const char *)b + p + 8, l);
            else if (!memcmp(b + p + 4, "PLTE", 4)) of_sb_put(&plte, (const char *)b + p + 8, l);
            p += 12 + l;
        }
        if (!inter && w > 0 && h > 0 && idat.n) {
            if ((ct == 0 || ct == 2) && depth == 8) {
                img = new_obj(W);
                char dict[300];
                snprintf(dict, sizeof dict, "/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /%s /BitsPerComponent 8 "
                                            "/Filter /FlateDecode /DecodeParms <</Predictor 15 /Colors %d /BitsPerComponent 8 /Columns %d>>",
                         w, h, ct ? "DeviceRGB" : "DeviceGray", ct ? 3 : 1, w);
                /* already compressed: written as it is */
                begin_obj(W, img);
                of_sb_printf(&W->out, "<<%s /Length %lu>>\nstream\n", dict, (unsigned long)idat.n);
                of_sb_put(&W->out, idat.s, idat.n);
                of_sb_puts(&W->out, "\nendstream\nendobj\n");
            } else if (ct == 3 && plte.n >= 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) {
                img = new_obj(W);
                of_sb_t dict = { 0 };
                of_sb_printf(&dict, "/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace [/Indexed /DeviceRGB %d <", w, h, (int)plte.n / 3 - 1);
                for (size_t i = 0; i < plte.n; i++) of_sb_printf(&dict, "%02X", (uint8_t)plte.s[i]);
                of_sb_printf(&dict, ">] /BitsPerComponent %d /Filter /FlateDecode /DecodeParms <</Predictor 15 /Colors 1 /BitsPerComponent %d /Columns %d>>",
                             depth, depth, w);
                begin_obj(W, img);
                of_sb_printf(&W->out, "<<%s /Length %lu>>\nstream\n", dict.s, (unsigned long)idat.n);
                of_sb_put(&W->out, idat.s, idat.n);
                of_sb_puts(&W->out, "\nendstream\nendobj\n");
                of_sb_free(&dict);
            } else if ((ct == 4 || ct == 6) && depth == 8 && (int64_t)w * h <= 6000000) {
                /* colour and alpha apart: the alpha is a soft mask */
                size_t rn;
                uint8_t *raw = of_inflate_lax((const uint8_t *)idat.s, idat.n, true, &rn);
                int bpp = ct == 6 ? 4 : 2, cc = bpp - 1;
                size_t row;
                uint8_t *px = raw ? png_unfilter(raw, rn, w, h, bpp, &row) : NULL;
                if (px) {
                    size_t np = (size_t)w * (size_t)h;
                    uint8_t *col = of_alloc(np * (size_t)cc + 1), *al = of_alloc(np + 1);
                    if (col && al) {
                        for (size_t i = 0; i < np; i++) {
                            memcpy(col + i * (size_t)cc, px + i * (size_t)bpp, (size_t)cc);
                            al[i] = px[i * (size_t)bpp + (size_t)cc];
                        }
                        int mask = new_obj(W);
                        char dict[200];
                        snprintf(dict, sizeof dict, "/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray /BitsPerComponent 8", w, h);
                        put_stream(W, mask, dict, al, np, true);
                        img = new_obj(W);
                        snprintf(dict, sizeof dict, "/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /%s /BitsPerComponent 8 /SMask %d 0 R",
                                 w, h, cc == 3 ? "DeviceRGB" : "DeviceGray", mask);
                        put_stream(W, img, dict, col, np * (size_t)cc, true);
                    }
                    of_free(col);
                    of_free(al);
                    of_free(px);
                }
                of_free(raw);
            }
        }
        of_sb_free(&idat);
        of_sb_free(&plte);
    }
    *pw = w;
    *ph = h;
    return img;
}

static void pics_remember(pw_t *W, int key, int img)
{
    void *np = of_realloc(W->pics, (size_t)(W->npics + 1) * sizeof *W->pics);
    if (np) {
        W->pics = np;
        W->pics[W->npics].obj = key;
        W->pics[W->npics].img = img;
        W->npics++;
    }
}

static int picture_obj(pw_t *W, int obj, int *pw, int *ph)
{
    for (int i = 0; i < W->npics; i++)
        if (W->pics[i].obj == obj) return W->pics[i].img;
    size_t n;
    const char *ext = NULL;
    uint8_t *b = of_doc_obj_bytes(W->d, &W->d->obj[obj], &n, &ext);
    int img = b ? picture_data(W, b, n, ext, pw, ph) : 0;
    of_free(b);
    pics_remember(W, obj, img);
    return img;
}

/* -------------------------------------------------------------------------- */
/* Laying out a paragraph                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  font;
    float    size, rise;
    uint32_t color, hl;
    uint8_t  u, s;
    int16_t  link;
} look_t;

/* a piece of a line: a word (or part of one), a space, a picture */
typedef struct {
    int     kind;               /* 0 text, 1 picture */
    look_t  lk;
    int     t0, tn;             /* text: WinAnsi bytes in the buffer */
    float   x, w;
    bool    space;              /* a space before it */
    int     lead;               /* spaces at the start of a line (code keeps its indent) */
    int     obj;                /* picture */
    float   ph;
} atom_t;

typedef struct {
    int    a0, an;              /* atoms */
    float  w, asc, desc, h;
    bool   last;                /* the paragraph's last line (not justified) */
} line_t2;

typedef struct {
    atom_t  *at;
    int      nat, capat;
    of_sb_t  txt;
    line_t2 *ln;
    int      nln;
} lay_t;

static void lay_free(lay_t *L)
{
    of_free(L->at);
    of_sb_free(&L->txt);
    of_free(L->ln);
    memset(L, 0, sizeof *L);
}

static float text_w(const pw_t *W, const look_t *lk, const char *s, int n)
{
    if (lk->font >= F_MONO) return n * 600 * lk->size / 1000.0f;
    const int16_t *t = lk->font == F_BOLD || lk->font == F_BOLDIT ? of_pdf_helvb : of_pdf_helv;
    float w = 0;
    for (int i = 0; i < n; i++) w += (float)of_pdf_width(t, from_ansi((uint8_t)s[i]));
    (void)W;
    return w * lk->size / 1000.0f;
}

static atom_t *add_atom(lay_t *L)
{
    if (L->nat == L->capat) {
        int nc = L->capat ? L->capat * 2 : 64;
        atom_t *n = of_realloc(L->at, (size_t)nc * sizeof *n);
        if (!n) return NULL;
        L->at = n;
        L->capat = nc;
    }
    atom_t *a = &L->at[L->nat++];
    memset(a, 0, sizeof *a);
    return a;
}

static float heading_size(const pw_t *W, int h)
{
    static const float F[] = { 1, 1.8f, 1.45f, 1.2f, 1.08f, 1.04f, 1, 1, 1, 1 };
    return W->base * F[h < 10 ? h : 9];
}

static void look_of(const pw_t *W, const of_para_t *p, uint16_t rs, look_t *lk)
{
    of_rstyle_t e;
    of_doc_effective(W->d, p, rs, &e);
    bool b = e.b == OF_TRI_ON || p->heading, i = e.i == OF_TRI_ON;
    memset(lk, 0, sizeof *lk);
    lk->font = W->mono ? (b ? F_MONOB : F_MONO) : (uint8_t)(b && i ? F_BOLDIT : b ? F_BOLD : i ? F_IT : F_REG);
    lk->size = p->heading ? heading_size(W, p->heading) : W->base;
    if (e.half_pt && !W->mono) lk->size = e.half_pt / 2.0f * W->base / 11.0f;
    if (lk->size < 4) lk->size = 4;
    if (e.sup) { lk->rise = e.sup == 1 ? lk->size * 0.33f : -lk->size * 0.15f; lk->size *= 0.65f; }
    lk->color = e.color;
    lk->hl = e.hl;
    lk->u = e.u == OF_TRI_ON || e.link >= 0;
    lk->s = e.s == OF_TRI_ON;
    lk->link = e.link >= 0 && e.link < W->d->nlinks && of_link_is_url(W->d->links[e.link]) ? e.link : -1;
    if (lk->link >= 0 && !lk->color) lk->color = 0x10563C1;
}

/* the paragraph cut into atoms, then into lines of width w */
static void lay_para(pw_t *W, const of_para_t *p, float width, lay_t *L)
{
    memset(L, 0, sizeof *L);
    bool space = false;
    int k = 0, lead = 0;
    for (int i = 0; i < p->len;) {
        if (i + 2 < p->len && !memcmp(p->txt + i, OF_OBJ_CHAR, 3)) {
            int ob = k < p->nobj ? p->obj[k] : -1;
            k++;
            i += 3;
            const of_obj_t *o = ob >= 0 && ob < W->d->nobj ? &W->d->obj[ob] : NULL;
            if (!o) continue;
            if ((o->kind == OF_OBJ_TEXT || o->kind == OF_OBJ_NOTE) && o->text) {
                atom_t *a = add_atom(L);
                if (!a) break;
                look_of(W, p, p->at[i - 3], &a->lk);
                if (o->kind == OF_OBJ_NOTE) { a->lk.rise = a->lk.size * 0.33f; a->lk.size *= 0.65f; }
                a->t0 = (int)L->txt.n;
                for (const char *s = o->text; *s;) {
                    int l;
                    uint32_t cp = of_utf8_get(s, &l);
                    s += l;
                    of_sb_putc(&L->txt, (char)to_ansi(cp));
                }
                a->tn = (int)L->txt.n - a->t0;
                a->space = space;
                space = false;
            } else if (o->kind == OF_OBJ_IMAGE) {
                atom_t *a = add_atom(L);
                if (!a) break;
                a->kind = 1;
                a->obj = ob;
                float ow = o->w > 0 ? (float)o->w : 200, oh = o->h > 0 ? (float)o->h : 150;
                float maxh = (W->ph - W->top - W->bottom) * 0.9f;
                if (ow > width) { oh = oh * width / ow; ow = width; }
                if (oh > maxh) { ow = ow * maxh / oh; oh = maxh; }
                a->w = ow;
                a->ph = oh;
                a->space = space;
                space = false;
            }
            continue;
        }
        char c = p->txt[i];
        if (c == ' ' || c == '\t') {
            space = true;
            /* at the start of a line: counted */
            if (!L->nat || L->at[L->nat - 1].kind == 2) lead += c == '\t' ? 4 : 1;
            i++;
            continue;
        }
        if (c == '\n') {
            atom_t *a = add_atom(L);
            if (!a) break;
            a->kind = 2;        /* a line break */
            i++;
            space = false;
            continue;
        }
        /* a word: up to a space, a style change or an object */
        uint16_t st = p->at[i];
        atom_t *a = add_atom(L);
        if (!a) break;
        look_of(W, p, st, &a->lk);
        a->t0 = (int)L->txt.n;
        a->space = space;
        a->lead = lead;
        lead = 0;
        space = false;
        while (i < p->len && p->at[i] == st && p->txt[i] != ' ' && p->txt[i] != '\t' && p->txt[i] != '\n' &&
               !(i + 2 < p->len && !memcmp(p->txt + i, OF_OBJ_CHAR, 3))) {
            int l;
            uint32_t cp = of_utf8_get(p->txt + i, &l);
            if (i + l > p->len) l = p->len - i;
            i += l;
            if (cp >= 0xFB00 && cp <= 0xFB04) {
                static const char *const LIG[] = { "ff", "fi", "fl", "ffi", "ffl" };
                of_sb_puts(&L->txt, LIG[cp - 0xFB00]);
            } else {
                of_sb_putc(&L->txt, (char)to_ansi(cp));
            }
        }
        a->tn = (int)L->txt.n - a->t0;
    }
    /* widths */
    for (int i = 0; i < L->nat; i++)
        if (L->at[i].kind == 0) L->at[i].w = text_w(W, &L->at[i].lk, L->txt.s + L->at[i].t0, L->at[i].tn);
    /* lines */
    int start = 0;
    while (start < L->nat || !L->nln) {
        line_t2 ln = { start, 0, 0, 0, 0, 0, false };
        float x = 0;
        int i = start;
        for (; i < L->nat; i++) {
            atom_t *a = &L->at[i];
            if (a->kind == 2) { i++; break; }
            float sp = a->space && i > start ? a->lk.size * (a->lk.font >= F_MONO ? 0.6f : 0.278f) : 0;
            if (a->kind == 1) sp = a->space && i > start ? W->base * 0.278f : 0;
            if (i > start && x + sp + a->w > width) break;
            if (i == start && a->kind == 0 && a->w > width && a->tn > 1) {
                /* a word longer than the line: cut it */
                int cut = a->tn;
                while (cut > 1 && text_w(W, &a->lk, L->txt.s + a->t0, cut) > width) cut--;
                atom_t rest = *a;
                rest.t0 += cut;
                rest.tn -= cut;
                rest.space = false;
                a->tn = cut;
                a->w = text_w(W, &a->lk, L->txt.s + a->t0, cut);
                rest.w = text_w(W, &rest.lk, L->txt.s + rest.t0, rest.tn);
                atom_t *nw = add_atom(L);
                if (!nw) break;
                a = &L->at[i];
                memmove(&L->at[i + 2], &L->at[i + 1], (size_t)(L->nat - i - 2) * sizeof *L->at);
                L->at[i + 1] = rest;
            }
            if (i == start && a->lead) sp = a->lead * a->lk.size * (a->lk.font >= F_MONO ? 0.6f : 0.278f);
            a->x = x + sp;
            x = a->x + a->w;
            float asc = a->kind == 1 ? a->ph : a->lk.size * 0.8f + (a->lk.rise > 0 ? a->lk.rise : 0);
            float desc = a->kind == 1 ? 0 : a->lk.size * 0.22f - (a->lk.rise < 0 ? a->lk.rise : 0);
            if (asc > ln.asc) ln.asc = asc;
            if (desc > ln.desc) ln.desc = desc;
        }
        ln.an = i - start;
        ln.w = x;
        if (!ln.asc) { ln.asc = W->base * (p->heading ? heading_size(W, p->heading) / W->base : 1) * 0.8f; ln.desc = ln.asc * 0.27f; }
        float lead = (ln.asc + ln.desc) * 1.18f;
        ln.h = lead;
        ln.last = i >= L->nat;
        line_t2 *nl = of_realloc(L->ln, (size_t)(L->nln + 1) * sizeof *nl);
        if (!nl) break;
        L->ln = nl;
        L->ln[L->nln++] = ln;
        if (i == start) i++;        /* never stuck */
        start = i;
        if (start >= L->nat) break;
    }
}

static void color_cmd(of_sb_t *o, uint32_t c, const char *op)
{
    if (!c) of_sb_printf(o, "0 %s ", op[0] == 'r' ? "g" : "G");
    else of_sb_printf(o, "%.3f %.3f %.3f %s ", ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, op);
}

/* Draws the lines of a laid-out paragraph from the pen at x; returns the height used. */
static void draw_lines(pw_t *W, lay_t *L, float x, float width, int align, bool split_pages)
{
    for (int li = 0; li < L->nln; li++) {
        line_t2 *ln = &L->ln[li];
        if (split_pages) need(W, ln->h);
        float base = W->ph - (W->y + ln->asc);
        float extra = 0, off = 0;
        int gaps = 0;
        for (int i = 1; i < ln->an; i++) if (L->at[ln->a0 + i].space) gaps++;
        if (align == OF_AL_CENTER) off = (width - ln->w) / 2;
        else if (align == OF_AL_RIGHT) off = width - ln->w;
        else if (align == OF_AL_JUSTIFY && !ln->last && gaps) extra = (width - ln->w) / (float)gaps;
        float shift = 0;
        for (int i = 0; i < ln->an;) {
            atom_t *a = &L->at[ln->a0 + i];
            if (i && a->space) shift += extra;
            float ax = x + off + a->x + shift;
            if (a->kind == 1) {
                int pw = 0, ph = 0;
                int img = picture_obj(W, a->obj, &pw, &ph);
                if (img > 0)
                    of_sb_printf(&W->page, "q %.2f 0 0 %.2f %.2f %.2f cm /Im%d Do Q\n", a->w, a->ph, ax, base, img);
                i++;
                continue;
            }
            if (a->kind != 0 || !a->tn) { i++; continue; }
            /* a run: the atoms that follow with the same look, their spaces as spaces (copy and search see words) */
            int j = i + 1;
            while (j < ln->an) {
                const atom_t *b = &L->at[ln->a0 + j];
                if (b->kind != 0 || memcmp(&b->lk, &a->lk, sizeof a->lk)) break;
                j++;
            }
            of_sb_t run = { 0 };
            for (int k = i; k < j; k++) {
                const atom_t *b = &L->at[ln->a0 + k];
                if (k > i && b->space) of_sb_putc(&run, ' ');
                of_sb_put(&run, L->txt.s + b->t0, (size_t)b->tn);
            }
            const atom_t *z = &L->at[ln->a0 + j - 1];
            float zshift = shift;
            for (int k = i + 1; k < j; k++) if (L->at[ln->a0 + k].space) zshift += extra;
            float rw = (x + off + z->x + zshift + z->w) - ax;
            if (a->lk.hl) {
                color_cmd(&W->page, a->lk.hl, "rg");
                of_sb_printf(&W->page, "%.2f %.2f %.2f %.2f re f\n", ax, base - a->lk.size * 0.22f, rw, a->lk.size * 1.05f);
            }
            of_sb_puts(&W->page, "BT ");
            color_cmd(&W->page, a->lk.color, "rg");
            of_sb_printf(&W->page, "/F%d %.2f Tf %.2f %.2f Td ", a->lk.font + 1, a->lk.size, ax, base + a->lk.rise);
            if (extra) of_sb_printf(&W->page, "%.3f Tw ", extra);
            str_lit(&W->page, (const uint8_t *)run.s, run.n);
            of_sb_puts(&W->page, extra ? " Tj 0 Tw ET\n" : " Tj ET\n");
            of_sb_free(&run);
            if (a->lk.u || a->lk.s) {
                color_cmd(&W->page, a->lk.color, "rg");
                float t = a->lk.size * 0.05f;
                if (a->lk.u) of_sb_printf(&W->page, "%.2f %.2f %.2f %.2f re f\n", ax, base + a->lk.rise - a->lk.size * 0.12f, rw, t);
                if (a->lk.s) of_sb_printf(&W->page, "%.2f %.2f %.2f %.2f re f\n", ax, base + a->lk.rise + a->lk.size * 0.28f, rw, t);
            }
            if (a->lk.link >= 0) {
                annot_t *n = of_realloc(W->ann, (size_t)(W->nann + 1) * sizeof *n);
                if (n) {
                    W->ann = n;
                    W->ann[W->nann++] = (annot_t){ ax, base - a->lk.size * 0.22f, ax + rw, base + a->lk.size * 0.85f, a->lk.link };
                }
            }
            of_sb_puts(&W->page, "0 g ");
            shift = zshift;
            i = j;
        }
        W->y += ln->h;
    }
}

static float lines_height(const lay_t *L)
{
    float h = 0;
    for (int i = 0; i < L->nln; i++) h += L->ln[i].h;
    return h;
}

/* -------------------------------------------------------------------------- */
/* The document                                                                */
/* -------------------------------------------------------------------------- */

static void para_block(pw_t *W, const of_para_t *p, float x, float width)
{
    float indent = p->label[0] ? 18.0f * (1 + p->ilvl) : 18.0f * p->indent;
    if (indent > width / 2) indent = width / 2;
    lay_t L;
    lay_para(W, p, width - indent, &L);
    float before = p->heading ? heading_size(W, p->heading) * 0.7f : 0;
    float after = p->heading ? W->base * 0.35f : W->base * 0.45f;
    if (p->label[0] && !p->heading) after = W->base * 0.2f;
    /* a heading is not left alone at the foot of a page */
    float keep = p->heading ? lines_height(&L) + W->base * 3.5f : (L.nln ? L.ln[0].h : W->base);
    if (W->page_open && W->y > W->top + 1) W->y += before;
    need(W, keep);
    if (p->label[0]) {
        /* the list's label, at the left of the first line */
        look_t lk;
        look_of(W, p, p->len ? p->at[0] : p->mark, &lk);
        lk.u = 0;
        lk.link = -1;
        uint8_t lb[24];
        int n = 0;
        for (const char *s = p->label; *s && n < 23;) {
            int l;
            uint32_t cp = of_utf8_get(s, &l);
            s += l;
            lb[n++] = to_ansi(cp == 0x25E6 ? 0x2022 : cp);
        }
        float asc = L.nln ? L.ln[0].asc : lk.size * 0.8f;
        float lw = text_w(W, &lk, (const char *)lb, n);
        of_sb_printf(&W->page, "BT /F%d %.2f Tf %.2f %.2f Td ", lk.font + 1, lk.size, x + indent - lw - 5, W->ph - (W->y + asc));
        str_lit(&W->page, lb, (size_t)n);
        of_sb_puts(&W->page, " Tj ET\n");
    }
    draw_lines(W, &L, x + indent, width - indent, p->align, true);
    W->y += after;
    lay_free(&L);
}

/* the table that starts at paragraph i; returns the first paragraph after it */
static int table_block(pw_t *W, int i, float x, float width)
{
    const of_doc_t *d = W->d;
    int t = d->cells[d->p[i].cell - 1].table;
    int end = i;
    while (end < d->np && d->p[end].cell && d->cells[d->p[end].cell - 1].table == t) end++;
    const of_table_t *T = &d->tables[t];
    int nc = T->ncols > 0 ? T->ncols : 1;
    if (nc > 64) nc = 64;
    float cw[64], total = 0;
    for (int c = 0; c < nc; c++) { cw[c] = T->colw[c] > 0 ? (float)T->colw[c] : 1; total += cw[c]; }
    bool any = false;
    for (int c = 0; c < nc; c++) if (T->colw[c] > 0) any = true;
    /* widths given: kept, shrunk only if they do not fit; none: the column's width shared */
    float sc = any && total > width ? width / total : 1;
    for (int c = 0; c < nc; c++) cw[c] = any ? cw[c] * sc : width / nc;
    float pad = 4;
    if (W->page_open && W->y > W->top + 1) W->y += W->base * 0.3f;
    /* row by row */
    for (int r = 0; r < T->nrows; r++) {
        /* the cells of the row, and their paragraphs */
        struct { int col, span, p0, p1; float h; } cell[64];
        int ncell = 0;
        for (int k = i; k < end; k++) {
            const of_cell_t2 *c = &d->cells[d->p[k].cell - 1];
            if (c->row != r) continue;
            if (ncell && cell[ncell - 1].p1 == k && d->p[k].cell == d->p[cell[ncell - 1].p0].cell) { cell[ncell - 1].p1 = k + 1; continue; }
            if (ncell == 64) break;
            cell[ncell].col = c->col < nc ? c->col : nc - 1;
            cell[ncell].span = c->span > 0 ? c->span : 1;
            cell[ncell].p0 = k;
            cell[ncell].p1 = k + 1;
            cell[ncell].h = 0;
            ncell++;
        }
        /* heights */
        float rh = W->base * 1.4f;
        for (int c = 0; c < ncell; c++) {
            float w = 0;
            for (int s = 0; s < cell[c].span && cell[c].col + s < nc; s++) w += cw[cell[c].col + s];
            float h = 0;
            for (int k = cell[c].p0; k < cell[c].p1; k++) {
                lay_t L;
                lay_para(W, &d->p[k], w - 2 * pad, &L);
                h += lines_height(&L);
                lay_free(&L);
            }
            cell[c].h = h + 2 * pad;
            if (cell[c].h > rh) rh = cell[c].h;
        }
        if (rh > W->ph - W->top - W->bottom) rh = W->ph - W->top - W->bottom;
        need(W, rh);
        float y0 = W->y;
        for (int c = 0; c < ncell; c++) {
            float cx = x;
            for (int s = 0; s < cell[c].col; s++) cx += cw[s];
            float w = 0;
            for (int s = 0; s < cell[c].span && cell[c].col + s < nc; s++) w += cw[cell[c].col + s];
            W->y = y0 + pad;
            for (int k = cell[c].p0; k < cell[c].p1; k++) {
                lay_t L;
                lay_para(W, &d->p[k], w - 2 * pad, &L);
                draw_lines(W, &L, cx + pad, w - 2 * pad, d->p[k].align, false);
                lay_free(&L);
            }
            of_sb_printf(&W->page, "0.6 G 0.5 w %.2f %.2f %.2f %.2f re S\n", cx, W->ph - (y0 + rh), w, rh);
        }
        W->y = y0 + rh;
    }
    W->y += W->base * 0.6f;
    return end;
}

static void doc_body(pw_t *W)
{
    const of_doc_t *d = W->d;
    float x = W->left, width = W->pw - W->left - W->right;
    for (int i = 0; i < d->np;) {
        const of_para_t *p = &d->p[i];
        if (p->cell) { i = table_block(W, i, x, width); continue; }
        /* a page break alone in its paragraph */
        if (p->nobj == 1 && p->len == 3 && d->obj[p->obj[0]].kind == OF_OBJ_BREAK) {
            if (W->page_open) page_begin(W);
            i++;
            continue;
        }
        bool brk = false;
        for (int k = 0; k < p->nobj; k++) if (p->obj[k] >= 0 && d->obj[p->obj[k]].kind == OF_OBJ_BREAK) brk = true;
        para_block(W, p, x, width);
        if (brk) page_begin(W);
        i++;
    }
}

static bool finish(pw_t *W, const char *path, const char *title)
{
    if (!W->page_open && !W->nkids) page_begin(W);
    page_end(W);
    /* the page tree, the catalogue, the information */
    W->off[2] = (uint32_t)W->out.n;
    of_sb_printf(&W->out, "2 0 obj\n<</Type /Pages /Count %d /Kids [", W->nkids);
    for (int i = 0; i < W->nkids; i++) of_sb_printf(&W->out, "%d 0 R ", W->kids[i]);
    of_sb_puts(&W->out, "]>>\nendobj\n");
    W->off[1] = (uint32_t)W->out.n;
    of_sb_puts(&W->out, "1 0 obj\n<</Type /Catalog /Pages 2 0 R>>\nendobj\n");
    int info = new_obj(W);
    begin_obj(W, info);
    of_sb_puts(&W->out, "<</Producer (P4OS Oficina)");
    if (title && title[0]) { of_sb_puts(&W->out, " /Title "); str_utf16(&W->out, title); }
    of_sb_puts(&W->out, ">>\nendobj\n");
    size_t xref = W->out.n;
    of_sb_printf(&W->out, "xref\n0 %d\n0000000000 65535 f \n", W->nobj + 1);
    for (int i = 1; i <= W->nobj; i++) of_sb_printf(&W->out, "%010lu 00000 n \n", (unsigned long)W->off[i]);
    of_sb_printf(&W->out, "trailer\n<</Size %d /Root 1 0 R /Info %d 0 R>>\nstartxref\n%lu\n%%%%EOF\n", W->nobj + 1, info, (unsigned long)xref);
    bool ok = !W->out.oom && of_file_write(path, W->out.s, W->out.n);
    return ok;
}

static void pw_init(pw_t *W, const of_doc_t *d, bool landscape, bool mono)
{
    memset(W, 0, sizeof *W);
    W->d = d;
    W->mono = mono;
    W->pw = landscape ? PH : PW;
    W->ph = landscape ? PW : PH;
    W->left = W->right = W->top = MARGIN;
    W->bottom = MARGIN;
    float hp = d && d->def_half_pt ? d->def_half_pt / 2.0f : 11;
    W->base = mono ? 9 : hp < 8 ? 8 : hp > 14 ? 14 : hp;
    of_sb_puts(&W->out, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");
    new_obj(W);     /* 1: the catalogue */
    new_obj(W);     /* 2: the pages */
    for (int f = 0; f < NFONTS; f++) {
        W->font_obj[f] = new_obj(W);
        begin_obj(W, W->font_obj[f]);
        of_sb_printf(&W->out, "<</Type /Font /Subtype /Type1 /BaseFont /%s /Encoding /WinAnsiEncoding>>\nendobj\n", FONT_NAMES[f]);
    }
}

static void pw_free(pw_t *W)
{
    of_sb_free(&W->out);
    of_sb_free(&W->page);
    of_free(W->off);
    of_free(W->kids);
    of_free(W->ann);
    of_free(W->pics);
}

bool of_pdf_write_doc(const of_doc_t *d, const char *path, const char *title, bool landscape, bool mono, char *err, size_t errlen)
{
    pw_t W;
    pw_init(&W, d, landscape, mono);
    doc_body(&W);
    bool ok = finish(&W, path, title);
    pw_free(&W);
    if (!ok) snprintf(err, errlen, "write");
    return ok;
}

/* -------------------------------------------------------------------------- */
/* A sheet and a text, as documents to write                                   */
/* -------------------------------------------------------------------------- */

static void col_name(int c, char *out)
{
    char t[8];
    int n = 0;
    c++;
    while (c > 0 && n < 7) { t[n++] = (char)('A' + (c - 1) % 26); c = (c - 1) / 26; }
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
}

bool of_pdf_write_book(of_book_t *bk, int sheet, const char *path, const char *title, char *err, size_t errlen)
{
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    if (!d) { snprintf(err, errlen, "memory"); return false; }
    d->def_half_pt = 18;
    of_bld_t b;
    of_bld_init(&b, d);
    bool landscape = false;
    int first = sheet >= 0 ? sheet : 0, last = sheet >= 0 ? sheet : bk->nsh - 1;
    /* the widest sheet decides the paper's side */
    for (int s = first; s <= last; s++) {
        int32_t mr, mc;
        of_sheet_extent(&bk->sh[s], &mr, &mc);
        float w = 0;
        for (int c = 0; c <= mc && c < 60; c++) w += of_sheet_colw(&bk->sh[s], c) * 5.2f + 8;
        if (w > PW - 2 * MARGIN) landscape = true;
    }
    float avail = (landscape ? PH : PW) - 2 * MARGIN;
    for (int s = first; s <= last; s++) {
        of_sheet_t *sh = &bk->sh[s];
        if (sh->state && sheet < 0) continue;
        int32_t mr, mc;
        of_sheet_extent(sh, &mr, &mc);
        if (mr > 2999) mr = 2999;
        if (mc > 59) mc = 59;
        /* columns in groups that fit the paper */
        for (int c0 = 0; c0 <= mc;) {
            float w = 0;
            int c1 = c0;
            while (c1 <= mc && (c1 == c0 || w + of_sheet_colw(sh, c1) * 5.2f + 8 <= avail)) { w += of_sheet_colw(sh, c1) * 5.2f + 8; c1++; }
            if (last > first || c0 > 0 || c1 <= mc) {
                b.heading = 2;
                b.cur = -1;
                of_bld_para(&b);
                char h[160], a[8], z[8];
                col_name(c0, a);
                col_name(c1 - 1, z);
                if (c0 == 0 && c1 > mc) snprintf(h, sizeof h, "%.120s", sh->name);
                else snprintf(h, sizeof h, "%.120s (%s-%s)", sh->name, a, z);
                of_bld_text(&b, h, strlen(h));
                b.heading = 0;
            }
            of_bld_table(&b);
            int t = d->ntables - 1;
            for (int c = c0; c < c1 && c - c0 < 64; c++) d->tables[t].colw[c - c0] = (int32_t)(of_sheet_colw(sh, c) * 5.2f + 8);
            for (int32_t r = 0; r <= mr; r++) {
                of_row_t *row = of_row_get(sh, r);
                if (row && row->hidden) continue;
                of_bld_row(&b);
                for (int c = c0; c < c1; c++) {
                    of_cell_t *cell = row ? of_cell_get(sh, r, c) : NULL;
                    char txt[256] = "";
                    bool right = false;
                    if (cell) of_cell_display(bk, cell, txt, sizeof txt, &right);
                    b.align = right ? OF_AL_RIGHT : 0;
                    of_bld_cell(&b, 1);
                    if (txt[0]) of_bld_text(&b, txt, strlen(txt));
                }
            }
            b.align = 0;
            of_bld_table_end(&b);
            c0 = c1;
        }
    }
    of_bld_done(&b);
    bool ok = of_pdf_write_doc(d, path, title, landscape, false, err, errlen);
    of_doc_free(d);
    return ok;
}

bool of_pdf_write_text(const char *s, size_t n, const char *path, const char *title, char *err, size_t errlen)
{
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    if (!d) { snprintf(err, errlen, "memory"); return false; }
    size_t from = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && s[i] != '\n') continue;
        size_t e = i;
        if (e > from && s[e - 1] == '\r') e--;
        of_para_t *p = of_doc_insert_para(d, d->np);
        if (!p) break;
        /* tabs as spaces to the next multiple of four */
        of_sb_t line = { 0 };
        int col = 0;
        for (size_t k = from; k < e; k++) {
            if (s[k] == '\t') { do { of_sb_putc(&line, ' '); col++; } while (col % 4); continue; }
            of_sb_putc(&line, s[k]);
            if (((unsigned char)s[k] & 0xC0) != 0x80) col++;
        }
        if (line.n) of_para_append(d, &d->p[d->np - 1], line.s, line.n, 0);
        of_sb_free(&line);
        from = i + 1;
    }
    bool ok = of_pdf_write_doc(d, path, title, false, true, err, errlen);
    of_doc_free(d);
    return ok;
}

/* -------------------------------------------------------------------------- */
/* A presentation: every slide a page of its size                              */
/* -------------------------------------------------------------------------- */

/* a slide's paragraph as atoms, laid out at width w */
static void lay_slpara(pw_t *W, const sl_para_t *p, float width, lay_t *L)
{
    memset(L, 0, sizeof *L);
    for (int k = 0; k < p->nr; k++) {
        const sl_run_t *r = &p->r[k];
        look_t lk;
        memset(&lk, 0, sizeof lk);
        lk.font = (uint8_t)(r->b && r->i ? F_BOLDIT : r->b ? F_BOLD : r->i ? F_IT : F_REG);
        lk.size = r->size > 1 ? r->size : 18;
        if (r->sup) { lk.rise = r->sup == 1 ? lk.size * 0.33f : -lk.size * 0.15f; lk.size *= 0.65f; }
        lk.color = r->color;
        lk.u = r->u;
        lk.s = r->s;
        lk.link = -1;
        const char *t = r->t ? r->t : "";
        bool space = false;
        while (*t) {
            if (*t == ' ' || *t == '\t') { space = true; t++; continue; }
            if (*t == '\n' || *t == '\v') {
                atom_t *a = add_atom(L);
                if (a) a->kind = 2;
                space = false;
                t++;
                continue;
            }
            atom_t *a = add_atom(L);
            if (!a) return;
            a->lk = lk;
            a->space = space || (L->nat > 1 && k > 0 && t == r->t && L->at[L->nat - 2].kind == 0 && p->r[k - 1].t &&
                                 p->r[k - 1].t[0] && p->r[k - 1].t[strlen(p->r[k - 1].t) - 1] == ' ');
            space = false;
            a->t0 = (int)L->txt.n;
            while (*t && *t != ' ' && *t != '\t' && *t != '\n' && *t != '\v') {
                int l;
                uint32_t cp = of_utf8_get(t, &l);
                t += l;
                of_sb_putc(&L->txt, (char)to_ansi(cp));
            }
            a->tn = (int)L->txt.n - a->t0;
            a->w = text_w(W, &a->lk, L->txt.s + a->t0, a->tn);
        }
    }
    /* lines, as lay_para does */
    int start = 0;
    while (start < L->nat || !L->nln) {
        line_t2 ln = { start, 0, 0, 0, 0, 0, false };
        float x = 0;
        int i = start;
        for (; i < L->nat; i++) {
            atom_t *a = &L->at[i];
            if (a->kind == 2) { i++; break; }
            float sp = a->space && i > start ? a->lk.size * 0.278f : 0;
            if (i > start && x + sp + a->w > width) break;
            a->x = x + sp;
            x = a->x + a->w;
            float asc = a->lk.size * 0.8f + (a->lk.rise > 0 ? a->lk.rise : 0), desc = a->lk.size * 0.22f;
            if (asc > ln.asc) ln.asc = asc;
            if (desc > ln.desc) ln.desc = desc;
        }
        ln.an = i - start;
        ln.w = x;
        if (!ln.asc) { ln.asc = p->empty_size * 0.8f; ln.desc = p->empty_size * 0.22f; }
        ln.h = (ln.asc + ln.desc) * 1.15f * (p->line > 0.3f ? p->line : 1);
        ln.last = i >= L->nat;
        line_t2 *nl = of_realloc(L->ln, (size_t)(L->nln + 1) * sizeof *nl);
        if (!nl) break;
        L->ln = nl;
        L->ln[L->nln++] = ln;
        if (i == start) i++;
        start = i;
        if (start >= L->nat) break;
    }
}

static const char *num_label(const sl_para_t *p, char *buf, size_t len)
{
    if (p->bullet[0]) return p->bullet;
    if (!p->autonum) return "";
    int n = p->number > 0 ? p->number : 1;
    switch (p->autonum) {
    case 2: snprintf(buf, len, "%c.", 'a' + (n - 1) % 26); break;
    case 3: snprintf(buf, len, "%c.", 'A' + (n - 1) % 26); break;
    case 4: case 5: {
        static const char *const R[] = { "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x", "xi", "xii" };
        snprintf(buf, len, "%s.", R[(n - 1) % 12]);
        if (p->autonum == 5) for (char *q = buf; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
        break;
    }
    case 6: snprintf(buf, len, "%d)", n); break;
    default: snprintf(buf, len, "%d.", n); break;
    }
    return buf;
}

/* a text body inside the box (x, y, w, h), from the top: its height, drawn when draw */
static float slide_text(pw_t *W, const sl_text_t *t, float x, float y, float w, float h, bool draw)
{
    float inner = w - t->l - t->r;
    if (inner < 4) inner = 4;
    /* measured first, for the vertical anchor */
    float total = 0;
    for (int i = 0; i < t->np; i++) {
        const sl_para_t *p = &t->p[i];
        lay_t L;
        float tw = inner - p->margin;
        lay_slpara(W, p, tw > 8 ? tw : 8, &L);
        total += (i ? p->before : 0) + lines_height(&L) + p->after;
        lay_free(&L);
    }
    if (!draw) return total + t->t + t->b;
    float top = y + t->t;
    float room = h - t->t - t->b;
    if (t->anchor == 1) top += (room - total) / 2;
    else if (t->anchor == 2) top += room - total;
    W->y = top;
    for (int i = 0; i < t->np; i++) {
        const sl_para_t *p = &t->p[i];
        if (i) W->y += p->before;
        lay_t L;
        float tw = inner - p->margin;
        lay_slpara(W, p, tw > 8 ? tw : 8, &L);
        char nb[16];
        const char *lab = num_label(p, nb, sizeof nb);
        if (lab[0] && L.nln) {
            /* the bullet at margin + indent, in the first run's colour */
            uint8_t lb[16];
            int n = 0;
            for (const char *s = lab; *s && n < 15;) {
                int l;
                uint32_t cp = of_utf8_get(s, &l);
                s += l;
                lb[n++] = to_ansi(cp == 0x25E6 || cp == 0x25AA || cp == 0x2013 ? 0x2022 : cp);
            }
            float sz = p->nr ? p->r[0].size : p->empty_size;
            float bx = x + t->l + p->margin + p->indent;
            if (bx < x + t->l) bx = x + t->l;
            of_sb_puts(&W->page, "BT ");
            color_cmd(&W->page, p->bullet_color, "rg");
            of_sb_printf(&W->page, "/F1 %.2f Tf %.2f %.2f Td ", sz, bx, W->ph - (W->y + L.ln[0].asc));
            str_lit(&W->page, lb, (size_t)n);
            of_sb_puts(&W->page, " Tj ET 0 g\n");
        }
        float tx = x + t->l + p->margin;
        /* no bullet: the first line starts at the indent */
        if (!lab[0] && p->indent > 0) tx += 0;
        draw_lines(W, &L, tx, inner - p->margin, p->align, false);
        W->y += p->after;
        lay_free(&L);
    }
    return total;
}

static void geom_path(of_sb_t *o, int g, float x, float y, float w, float h)
{
    /* y is the bottom in PDF space */
    switch (g) {
    case SG_ELLIPSE: {
        float k = 0.5523f, cx = x + w / 2, cy = y + h / 2, rx = w / 2, ry = h / 2;
        of_sb_printf(o, "%.2f %.2f m %.2f %.2f %.2f %.2f %.2f %.2f c %.2f %.2f %.2f %.2f %.2f %.2f c "
                        "%.2f %.2f %.2f %.2f %.2f %.2f c %.2f %.2f %.2f %.2f %.2f %.2f c h ",
                     cx + rx, cy, cx + rx, cy + ry * k, cx + rx * k, cy + ry, cx, cy + ry,
                     cx - rx * k, cy + ry, cx - rx, cy + ry * k, cx - rx, cy,
                     cx - rx, cy - ry * k, cx - rx * k, cy - ry, cx, cy - ry,
                     cx + rx * k, cy - ry, cx + rx, cy - ry * k, cx + rx, cy);
        break;
    }
    case SG_ROUND: {
        float r = (w < h ? w : h) * 0.16f, k = r * 0.4477f;
        of_sb_printf(o, "%.2f %.2f m %.2f %.2f l %.2f %.2f %.2f %.2f %.2f %.2f c %.2f %.2f l %.2f %.2f %.2f %.2f %.2f %.2f c "
                        "%.2f %.2f l %.2f %.2f %.2f %.2f %.2f %.2f c %.2f %.2f l %.2f %.2f %.2f %.2f %.2f %.2f c h ",
                     x + r, y, x + w - r, y, x + w - k, y, x + w, y + k, x + w, y + r, x + w, y + h - r, x + w, y + h - k, x + w - k, y + h,
                     x + w - r, y + h, x + r, y + h, x + k, y + h, x, y + h - k, x, y + h - r, x, y + r, x, y + k, x + k, y, x + r, y);
        break;
    }
    case SG_TRIANGLE: of_sb_printf(o, "%.2f %.2f m %.2f %.2f l %.2f %.2f l h ", x, y, x + w, y, x + w / 2, y + h); break;
    case SG_DIAMOND: of_sb_printf(o, "%.2f %.2f m %.2f %.2f l %.2f %.2f l %.2f %.2f l h ", x + w / 2, y, x + w, y + h / 2, x + w / 2, y + h, x, y + h / 2); break;
    case SG_ARROW: {
        float hd = w * 0.3f;
        of_sb_printf(o, "%.2f %.2f m %.2f %.2f l %.2f %.2f l %.2f %.2f l %.2f %.2f l %.2f %.2f l %.2f %.2f l h ",
                     x, y + h * 0.25f, x + w - hd, y + h * 0.25f, x + w - hd, y, x + w, y + h / 2, x + w - hd, y + h, x + w - hd, y + h * 0.75f, x, y + h * 0.75f);
        break;
    }
    default: of_sb_printf(o, "%.2f %.2f %.2f %.2f re ", x, y, w, h); break;
    }
}

static int deck_picture(pw_t *W, const of_deck_t *dk, const char *name)
{
    int key = -(int)(of_crc32(0, name, strlen(name)) & 0x3FFFFFFF) - 1;
    for (int i = 0; i < W->npics; i++) if (W->pics[i].obj == key) return W->pics[i].img;
    size_t n;
    uint8_t *b = of_deck_part(dk, name, &n);
    const char *ext = NULL;
    if (b && n > 4) ext = !memcmp(b, "\x89PNG", 4) ? "png" : b[0] == 0xFF && b[1] == 0xD8 ? "jpeg" : NULL;
    int pw = 0, ph = 0;
    int img = b && ext ? picture_data(W, b, n, ext, &pw, &ph) : 0;
    of_free(b);
    pics_remember(W, key, img);
    return img;
}

bool of_pdf_write_deck(const of_deck_t *dk, const char *path, const char *title, bool notes, char *err, size_t errlen)
{
    pw_t W;
    pw_init(&W, NULL, false, false);
    W.pw = dk->w;
    W.ph = dk->h;
    W.top = W.bottom = 0;
    (void)notes;
    for (int i = 0; i < dk->ns; i++) {
        const sl_slide_t *s = &dk->s[i];
        page_begin(&W);
        W.pageno = 0;       /* slides carry their own numbers, if any */
        if (s->bg) { color_cmd(&W.page, s->bg, "rg"); of_sb_printf(&W.page, "0 0 %.2f %.2f re f\n", W.pw, W.ph); }
        if (s->bg_pic) {
            int img = deck_picture(&W, dk, s->bg_pic);
            if (img > 0) of_sb_printf(&W.page, "q %.2f 0 0 %.2f 0 0 cm /Im%d Do Q\n", W.pw, W.ph, img);
        }
        for (int k = 0; k < s->nsh; k++) {
            const sl_shape_t *sh = &s->sh[k];
            float bx = sh->x, by = W.ph - (sh->y + sh->h);     /* the bottom, in PDF space */
            of_sb_puts(&W.page, "q ");
            if (sh->rot) {
                float a = -sh->rot * 3.14159265f / 180, c = cosf(a), sn = sinf(a);
                float cx = bx + sh->w / 2, cy = by + sh->h / 2;
                of_sb_printf(&W.page, "%.4f %.4f %.4f %.4f %.2f %.2f cm ", c, sn, -sn, c, cx - c * cx + sn * cy, cy - sn * cx - c * cy);
            }
            if (sh->kind == SL_LINE) {
                color_cmd(&W.page, sh->line ? sh->line : 0x1000000, "RG");
                float x1 = bx, y1 = by + sh->h, x2 = bx + sh->w, y2 = by;
                if (sh->flip_h) { float t = x1; x1 = x2; x2 = t; }
                if (sh->flip_v) { float t = y1; y1 = y2; y2 = t; }
                of_sb_printf(&W.page, "%.2f w %.2f %.2f m %.2f %.2f l S Q\n", sh->line_w > 0 ? sh->line_w : 1, x1, y1, x2, y2);
                continue;
            }
            if (sh->kind == SL_PIC && sh->pic) {
                int img = deck_picture(&W, dk, sh->pic);
                if (img > 0) of_sb_printf(&W.page, "%.2f 0 0 %.2f %.2f %.2f cm /Im%d Do ", sh->w, sh->h, bx, by, img);
                of_sb_puts(&W.page, "Q\n");
                continue;
            }
            if (sh->kind == SL_TABLE) {
                float tw = 0, rh_total = 0;
                for (int c = 0; c < sh->cols; c++) tw += sh->colw ? sh->colw[c] : 0;
                float sc = tw > 0 ? sh->w / tw : 1;
                float *rh = of_alloc((size_t)(sh->rows + 1) * sizeof(float));
                for (int r = 0; rh && r < sh->rows; r++) {
                    rh[r] = sh->rowh ? sh->rowh[r] : 20;
                    for (int c = 0; c < sh->ncells; c++) {
                        const sl_cell_t *cl = &sh->cells[c];
                        if (cl->row != r || cl->hidden) continue;
                        float cw = 0;
                        for (int q = 0; q < cl->cspan && cl->col + q < sh->cols; q++) cw += sh->colw[cl->col + q] * sc;
                        float th = slide_text(&W, &cl->tx, 0, 0, cw, 0, false);
                        if (th > rh[r]) rh[r] = th;
                    }
                    rh_total += rh[r];
                }
                float ry = sh->y;
                for (int r = 0; rh && r < sh->rows; r++) {
                    for (int c = 0; c < sh->ncells; c++) {
                        const sl_cell_t *cl = &sh->cells[c];
                        if (cl->row != r || cl->hidden) continue;
                        float cx = sh->x, cw = 0;
                        for (int q = 0; q < cl->col && q < sh->cols; q++) cx += sh->colw[q] * sc;
                        for (int q = 0; q < cl->cspan && cl->col + q < sh->cols; q++) cw += sh->colw[cl->col + q] * sc;
                        if (cl->fill) { color_cmd(&W.page, cl->fill, "rg"); of_sb_printf(&W.page, "%.2f %.2f %.2f %.2f re f ", cx, W.ph - (ry + rh[r]), cw, rh[r]); }
                        of_sb_printf(&W.page, "0.6 G 0.5 w %.2f %.2f %.2f %.2f re S\n", cx, W.ph - (ry + rh[r]), cw, rh[r]);
                        slide_text(&W, &cl->tx, cx, ry, cw, rh[r], true);
                    }
                    ry += rh[r];
                }
                of_free(rh);
                (void)rh_total;
                of_sb_puts(&W.page, "Q\n");
                continue;
            }
            /* a shape: fill, outline, text */
            if (sh->fill || sh->line) {
                geom_path(&W.page, sh->geom ? sh->geom : SG_RECT, bx, by, sh->w, sh->h);
                if (sh->fill) color_cmd(&W.page, sh->fill, "rg");
                if (sh->line) { color_cmd(&W.page, sh->line, "RG"); of_sb_printf(&W.page, "%.2f w ", sh->line_w > 0 ? sh->line_w : 0.75f); }
                of_sb_puts(&W.page, sh->fill && sh->line ? "B\n" : sh->fill ? "f\n" : "S\n");
            }
            if (sh->tx.np) slide_text(&W, &sh->tx, sh->x, sh->y, sh->w, sh->h, true);
            of_sb_puts(&W.page, "Q\n");
        }
    }
    /* no page numbers on slides: page_end writes one, so it is told the number is 0 */
    bool ok = finish(&W, path, title);
    pw_free(&W);
    if (!ok) snprintf(err, errlen, "write");
    return ok;
}
