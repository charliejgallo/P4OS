/*
 * P4OS - Office: PDF files (of_pdf.c), their text (of_pdftext.c) and
 * writing them (of_pdfw.c). Without LVGL.
 */
#pragma once

#include "of.h"

/* a value of the file */
enum { PV_NULL = 0, PV_BOOL, PV_INT, PV_REAL, PV_STR, PV_NAME, PV_ARR, PV_DICT, PV_REF, PV_STREAM, PV_OP };

typedef struct pv pv_t;
struct pv {
    uint8_t t;
    union {
        bool    b;
        int64_t i;
        double  r;
        struct { char *s; int n; } s;               /* STR (bytes), NAME and OP (NUL-terminated) */
        struct { pv_t *v; int n; } a;
        struct { char **k; pv_t *v; int n; } d;     /* DICT, and STREAM's dictionary */
        struct { int num, gen; } ref;
    };
    size_t soff, slen;                              /* STREAM: its data in the file */
    int    gen;                                     /* an indirect object's generation */
};

typedef struct { const uint8_t *b; size_t n, p; } lx_t;

typedef struct {
    uint32_t off;               /* type 1: offset; type 2: the object stream */
    uint16_t gen;               /* type 2: the index in it */
    uint8_t  type, set;
} xref_t;

typedef struct pdf_s {
    uint8_t   *buf;
    size_t     len;
    xref_t    *x;
    pv_t     **cache;
    uint8_t   *loading;
    int        nx;
    pv_t       trailer;
    const pv_t *catalog;
    int        root;            /* found by the rebuild, when no trailer says */
    bool       rebuilt;
    /* encryption: 0 none, 1 RC4, 2 AES-128, 3 AES-256 */
    int        crypt, keylen, enc_obj;
    uint8_t    key[32];
    bool       no_streams, no_strings;
    struct pdf_os { int num; uint8_t *data; size_t len; int first, n; int *offs; } os[4];
    int        os_next;
    pv_t     **dead;            /* objects the rebuild replaced, freed at the end */
    int        ndead;
} pdf_t;

/* err: "read", "format", "damaged", "password" (wrong or needed), "encrypted" (a handler the app lacks) */
pdf_t      *pdf_open(const char *path, const char *password, char *err, size_t errlen);
void        pdf_close(pdf_t *P);
pv_t       *pdf_obj(pdf_t *P, int num);
const pv_t *pdf_res(pdf_t *P, const pv_t *v);                  /* a reference followed */
const pv_t *pdf_get(pdf_t *P, const pv_t *dict, const char *key);
/* A stream's data, decrypted and decoded; *image_filter: the picture coding left (DCTDecode...). */
uint8_t    *pdf_stream(pdf_t *P, const pv_t *s, int num, size_t *n, const char **image_filter);
/* The same for data of its own (an inline picture): d is taken (freed or returned). */
uint8_t    *pdf_decode(pdf_t *P, uint8_t *d, size_t n, const pv_t *filter, const pv_t *parms, size_t *out_n, const char **image_filter);

void   lx_ws(lx_t *L);
bool   pv_parse(pdf_t *P, lx_t *L, pv_t *out);                 /* P NULL: no "n g R" */
void   pv_free(pv_t *v);
pv_t  *pv_get(const pv_t *dict, const char *key);
double pv_num(const pv_t *v, double def);
bool   pv_is_name(const pv_t *v, const char *name);

/* the standard fonts' widths of ASCII 32-126 (of_pdftext.c), and any code point's */
extern const int16_t of_pdf_helv[95], of_pdf_helvb[95], of_pdf_times[95];
int of_pdf_width(const int16_t *table, uint32_t cp);

/* drawing the pages (of_pdfdraw.c) */
typedef struct of_pdfr of_pdfr_t;
of_pdfr_t *of_pdfr_open(const char *path, const char *password, char *err, size_t errlen);
void       of_pdfr_close(of_pdfr_t *R);
int        of_pdfr_count(const of_pdfr_t *R);
void       of_pdfr_size(const of_pdfr_t *R, int page, float *w, float *h);     /* points, turned as shown */
/* Page 'page' at 'scale' pixels a point: the part from (ox, oy), w x h, into px
 * (RGB565, LVGL's order). *cancel, set from elsewhere, stops it (false back). */
bool       of_pdfr_draw(of_pdfr_t *R, int page, float scale, int ox, int oy, uint16_t *px, int w, int h, volatile bool *cancel);

/* The glyphs of fonts the file names and does not carry: an 8-bit coverage
 * bitmap drawn at px pixels an em, its top-left at (left, top) from the
 * origin (top up), adv its advance in its pixels (0: unknown). flags: 1
 * bold, 2 italic, 4 monospaced, 8 serif. Called from the drawing's thread;
 * the bitmap must last until the next call. */
typedef struct { const uint8_t *a8; int w, h, left, top; float px, adv; } of_fbglyph_t;
typedef bool (*of_pdf_fallback_fn)(uint32_t cp, float px, uint8_t flags, of_fbglyph_t *out);
void of_pdfr_fallback(of_pdf_fallback_fn fn);

/* writing (of_pdfw.c) */
struct of_doc_s;
bool of_pdf_write_doc(const struct of_doc_s *d, const char *path, const char *title, bool landscape, bool mono, char *err, size_t errlen);
bool of_pdf_write_book(of_book_t *b, int sheet, const char *path, const char *title, char *err, size_t errlen);   /* sheet -1: all */
bool of_pdf_write_text(const char *s, size_t n, const char *path, const char *title, char *err, size_t errlen);
