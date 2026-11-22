/*
 * P4OS - Office: drawing PDF pages (of_pdfdraw.c), with the fonts' own
 * outlines (of_pdffont.c). Without LVGL.
 */
#pragma once

#include "of_pdf.h"

/* -------------------------------------------------------------------------- */
/* Paths: moves, lines, quadratic and cubic curves                             */
/* -------------------------------------------------------------------------- */

typedef struct { float x, y; } of_pt_t;

typedef struct {
    uint8_t *op;                /* 'M' 1 point, 'L' 1, 'Q' 2, 'C' 3, 'Z' 0 */
    of_pt_t *pt;
    int      nop, npt, capop, cappt;
    bool     oom;
} of_path_t;

void of_path_reset(of_path_t *p);
void of_path_free(of_path_t *p);
void of_path_move(of_path_t *p, float x, float y);
void of_path_line(of_path_t *p, float x, float y);
void of_path_quad(of_path_t *p, float cx, float cy, float x, float y);
void of_path_cubic(of_path_t *p, float x1, float y1, float x2, float y2, float x3, float y3);
void of_path_close(of_path_t *p);

/* a matrix as PDF writes them: x' = a x + c y + e, y' = b x + d y + f */
typedef struct { float a, b, c, d, e, f; } of_mtx_t;

static inline of_mtx_t of_mtx_mul(of_mtx_t m, of_mtx_t n)
{
    return (of_mtx_t){ m.a * n.a + m.b * n.c, m.a * n.b + m.b * n.d, m.c * n.a + m.d * n.c, m.c * n.b + m.d * n.d,
                       m.e * n.a + m.f * n.c + n.e, m.e * n.b + m.f * n.d + n.f };
}

/* -------------------------------------------------------------------------- */
/* Font programs embedded in the file                                          */
/* -------------------------------------------------------------------------- */

enum { FP_NONE = 0, FP_TT, FP_CFF, FP_T1 };

typedef struct { uint32_t count, offsize, offs, data, end; } of_cffix_t;
typedef struct { const char *name; int gid; } of_gname_t;
typedef struct { uint32_t cp; int gid; } of_guni_t;

typedef struct {
    int       kind;
    uint8_t  *data;             /* the font file (owned) */
    size_t    n;
    of_mtx_t  fm;               /* glyph units to text space */
    int       ngl;
    /* TrueType */
    uint32_t  loca, loca_len, glyf, glyf_len;
    bool      loca32;
    uint32_t  c31, c30, c10, c0;  /* cmap subtables (0: none) */
    uint32_t  hmtx, nhm;
    /* CFF: a range of data */
    uint32_t  cff, cffn;
    of_cffix_t cs, gsubr, lsubr;
    of_cffix_t fdsub[16];
    int       nfd;
    uint32_t  fdselect;
    bool      cid;
    uint16_t *gsid;             /* each glyph's SID, or CID */
    /* the built-in encoding: code -> glyph, -1 none */
    int16_t   benc[256];
    bool      benc_std;
    /* Type 1 */
    struct { uint32_t off, len; } *subr;
    int       nsubr;
    /* by name, by Unicode (from the names), by CID */
    of_gname_t *byname;
    int       nbyname;
    of_guni_t *byuni;
    int       nbyuni;
    of_guni_t *bycid;
    int       nbycid;
    char     *pool;             /* the names' text */
    /* Type 1: each glyph's charstring */
    struct { uint32_t off, len; } *t1cs;
} of_fprog_t;

/* kind: 1 FontFile (Type 1), 2 FontFile2 (TrueType), 3 FontFile3 (CFF or OpenType). Takes data. */
bool of_fprog_load(of_fprog_t *F, uint8_t *data, size_t n, int kind);
void of_fprog_free(of_fprog_t *F);
int  of_fprog_by_name(const of_fprog_t *F, const char *name);       /* -1 if none */
int  of_fprog_by_uni(const of_fprog_t *F, uint32_t cp);
int  of_fprog_by_code(const of_fprog_t *F, int code, bool symbolic); /* the font's own encoding or cmap */
int  of_fprog_by_cid(const of_fprog_t *F, uint32_t cid);
/* The glyph's outline, through m (glyph units to wherever), appended to out. */
bool of_fprog_outline(const of_fprog_t *F, int gid, of_mtx_t m, of_path_t *out);
float of_fprog_advance(const of_fprog_t *F, int gid);               /* in ems, 0 if unknown */

/* A glyph as 8-bit coverage at px pixels an em (of_pdfdraw.c): for fallbacks that have outlines. */
uint8_t *of_pdf_glyph_a8(const of_fprog_t *F, int gid, float px, int *w, int *h, int *left, int *top);
