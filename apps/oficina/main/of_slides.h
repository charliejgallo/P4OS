/*
 * P4OS - Office: presentations (.pptx, .ppsx, .potx, .odp), without LVGL.
 *
 * A deck is its slides; a slide, its shapes in drawing order (the master's
 * and the layout's own shapes first, as they are drawn under the slide's),
 * its background and its speaker notes. Every shape is resolved when read:
 * its place and size in points, its fill and outline as colours, its text
 * with every run's size, weight and colour worked out from the placeholder
 * chain (slide, layout, master), the master's text styles and the theme.
 * What is drawn is then plain: no inheritance left for the view.
 */
#pragma once

#include "of.h"

typedef struct {
    char    *t;                 /* UTF-8 */
    float    size;              /* points */
    uint8_t  b, i, u, s;
    uint8_t  sup;               /* 1 superscript, 2 subscript */
    uint32_t color;             /* 0x1RRGGBB */
    char    *link;              /* a URL, or NULL */
} sl_run_t;

typedef struct {
    sl_run_t *r;
    int       nr;
    uint8_t   align;            /* OF_AL_* */
    uint8_t   level;
    char      bullet[8];        /* the bullet's text, or "" */
    uint8_t   autonum;          /* 0 none, 1 "1.", 2 "a.", 3 "A.", 4 "i.", 5 "I.", 6 "1)" */
    int       start;            /* the first number */
    int       number;           /* worked out: this paragraph's */
    uint32_t  bullet_color;
    float     margin, indent;   /* points: the text's left edge, the first line's offset (bullet at margin+indent) */
    float     before, after;    /* points */
    float     line;             /* line spacing, 1 = single */
    float     empty_size;       /* the size of an empty paragraph */
} sl_para_t;

typedef struct {
    sl_para_t *p;
    int        np;
    uint8_t    anchor;          /* 0 top, 1 middle, 2 bottom */
    float      l, t, r, b;      /* insets, points */
    bool       wrap;
} sl_text_t;

enum { SL_TEXT = 0, SL_PIC, SL_TABLE, SL_LINE };
enum { SG_NONE = 0, SG_RECT, SG_ROUND, SG_ELLIPSE, SG_TRIANGLE, SG_DIAMOND, SG_ARROW };

typedef struct {
    sl_text_t tx;
    uint32_t  fill;
    int       col, row, cspan, rspan;
    bool      hidden;           /* covered by a merged cell */
} sl_cell_t;

typedef struct {
    int       kind;             /* SL_* */
    int       geom;             /* SG_* (text shapes) */
    float     x, y, w, h;       /* points */
    float     rot;              /* degrees */
    bool      flip_h, flip_v;
    uint32_t  fill, line;       /* 0 none */
    float     line_w;           /* points */
    sl_text_t tx;
    char     *pic;              /* SL_PIC: the part in the package ("ppt/media/image1.png") */
    int       rows, cols;       /* SL_TABLE */
    float    *colw, *rowh;
    sl_cell_t *cells;
    int       ncells;
    bool      placeholder;      /* from the slide's own (not the master's) */
    char     *label;            /* what is not drawn: "Gráfico", "SmartArt" */
} sl_shape_t;

typedef struct {
    sl_shape_t *sh;
    int         nsh;
    uint32_t    bg;             /* 0x1RRGGBB */
    char       *bg_pic;         /* a picture as background */
    char       *notes;          /* the speaker notes, UTF-8, '\n' between paragraphs */
    char       *title;          /* the title placeholder's text */
    bool        hidden;
} sl_slide_t;

typedef struct of_deck_s {
    float       w, h;           /* the slide's size, points */
    sl_slide_t *s;
    int         ns;
    uint8_t    *file;
    size_t      len;
    of_zip_t    zip;
    char        kind[8];        /* "PPTX", "ODP", "PPT" */
    bool        pic_files;      /* pictures are files of the card (a .ppt's, in the cache), not parts */
} of_deck_t;

of_deck_t *of_deck_load(const char *path, char *err, size_t errlen);
void       of_deck_free(of_deck_t *dk);
bool       of_is_deck_ext(const char *ext);
uint8_t   *of_deck_part(const of_deck_t *dk, const char *name, size_t *len);
/* The text of a deck as a document (a heading per slide, its boxes, its notes, its pictures). */
struct of_doc_s *of_deck_to_doc(const of_deck_t *dk, bool notes);
/* Every slide as a page of its own size (of_pdfw.c). */
bool of_pdf_write_deck(const of_deck_t *dk, const char *path, const char *title, bool notes, char *err, size_t errlen);

/* of_pptx.c and of_odp.c, called by of_deck_load */
bool sl_pptx_read(of_deck_t *dk, char *err, size_t errlen);
bool sl_odp_read(of_deck_t *dk, char *err, size_t errlen);
/* shared helpers (of_slides.c) */
sl_shape_t *sl_add_shape(sl_slide_t *s);
sl_para_t  *sl_add_para(sl_text_t *t);
sl_run_t   *sl_add_run(sl_para_t *p, const char *text, size_t n);
void        sl_number(sl_text_t *t);       /* the numbers of auto-numbered paragraphs */
void        sl_text_free(sl_text_t *t);

/* a small XML tree, for the readers (of_slides.c); attributes point into the source, which must outlive it */
typedef struct xn xn_t;
struct xn {
    char       *name;           /* "a:p" */
    const char *local;          /* "p" */
    const char *attrs;
    int         alen;
    char       *text;           /* a "#text" node's text, unescaped */
    xn_t       *kid, *last, *next, *parent;
};
xn_t  *xn_parse(const char *src, size_t len);
void   xn_free(xn_t *n);
xn_t  *xn_kid(const xn_t *n, const char *local);
xn_t  *xn_path(const xn_t *n, const char *path);       /* "spPr/xfrm/off" */
bool   xn_attr(const xn_t *n, const char *name, char *out, size_t len);
double xn_num(const xn_t *n, const char *name, double def);
void   xn_text(const xn_t *n, of_sb_t *sb);             /* all the text below */
