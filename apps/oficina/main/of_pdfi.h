/*
 * P4OS - Office: what the PDF text reader (of_pdftext.c) shares with the
 * page painter (of_pdfdraw.c): the fonts as the page uses them (codes,
 * Unicode, widths) and the list of pages. Without LVGL.
 */
#pragma once

#include "of_pdf.h"

#define FL_BOLD     1
#define FL_ITALIC   2
#define FL_MONO     4

typedef struct { uint32_t code; char u[16]; } tuc_t;
typedef struct { uint32_t lo, hi, cp; } tur_t;
typedef struct { uint32_t lo, hi; float w; } cw_t;

typedef struct {
    int      key;               /* the font dictionary's object, or its address */
    const pv_t *dict;
    bool     type0, type3, ucs2;
    bool     symbolic, base_named;  /* simple fonts: flag 4; an encoding named in the file */
    struct { uint8_t len; uint32_t lo, hi; } cs[8];
    int      ncs;
    tuc_t   *uc;
    int      nuc;
    tur_t   *ur;
    int      nur;
    uint32_t enc[256];          /* simple fonts: each code's Unicode */
    const char *names[256];     /* the /Differences' glyph names */
    float    w[256];
    bool     has_w;
    cw_t    *cw;
    int      ncw;
    float    dw;
    const int16_t *std;
    uint8_t  flags;
    double   fm_a, fm_d;        /* Type 3: its matrix */
} font_t;

typedef struct {
    font_t **f;
    int      n, cap;
} fonts_t;

uint32_t    of_pdf_glyph_cp(const char *name);                  /* a glyph name's Unicode, 0 if unknown */
font_t     *of_pdff_load(pdf_t *P, fonts_t *FS, const pv_t *ref);
void        of_pdff_free(fonts_t *FS);
uint32_t    of_pdff_next(const font_t *F, const uint8_t *s, int n, int *i);
const char *of_pdff_text(const font_t *F, uint32_t code, char *buf16);
float       of_pdff_width(const font_t *F, uint32_t code, const char *txt);

/* the pages, in order: MediaBox (or CropBox) and rotation inherited */
typedef struct { const pv_t *page; const pv_t *res; double box[4]; int rot; } pref_t;
int of_pdf_pages(pdf_t *P, pref_t **out);
