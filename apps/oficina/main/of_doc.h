/*
 * P4OS - Office: documents (docx and odt), without LVGL.
 *
 * A document is its paragraphs in reading order, those of the body and
 * those inside table cells alike (a paragraph says which cell it is in).
 * A paragraph is UTF-8 text with one run-style index per byte, like Notes'
 * blocks, plus what it is: a heading, a list item, its alignment.
 *
 * What the app does not model travels inside the paragraph as an object: a
 * U+FFFC character whose object holds the original XML untouched (a
 * picture, a footnote, a field, a bookmark, a comment). Saving puts it back
 * as it was. A paragraph with something the app cannot keep while editing
 * (tracked changes, content controls, math, a nested table) is shown and
 * locked: it goes out byte for byte and cannot be edited.
 *
 * Saving into the same file rewrites only the edited paragraphs: every
 * original paragraph remembers where its XML was, and the writer splices
 * the part (document.xml, content.xml) around them. New paragraphs go after
 * the one they were split from. Run styles the person changed keep the
 * original properties and change only the ones touched (bold, italic...).
 */
#pragma once

#include "of.h"

#define OF_OBJ_CHAR "\xEF\xBF\xBC"      /* U+FFFC */

enum { OF_DOC_DOCX = 1, OF_DOC_ODT, OF_DOC_TEXT, OF_DOC_RO };     /* RO: read from another format, shown only */

/* tri-state of a run property: as the paragraph style says, on, off */
enum { OF_TRI_UNSET = 0, OF_TRI_ON, OF_TRI_OFF };

/* the properties a person can change, as mask bits */
#define OF_RS_B      0x01
#define OF_RS_I      0x02
#define OF_RS_U      0x04
#define OF_RS_S      0x08
#define OF_RS_COLOR  0x10
#define OF_RS_SIZE   0x20
#define OF_RS_HL     0x40

typedef struct {
    uint8_t  b, i, u, s;        /* OF_TRI_* */
    uint8_t  sup;               /* 1 superscript, 2 subscript */
    uint16_t half_pt;           /* 0: the paragraph's */
    uint32_t color, hl;         /* 0x1RRGGBB, 0 unset */
    int16_t  link;              /* index of doc->links, -1 none */
    uint8_t  mask;              /* OF_RS_* changed by the person */
    char    *raw;               /* docx: the original <w:rPr>; odt: span style names, "A\x1fB" for nested spans */
} of_rstyle_t;

/* an object kept as it was */
enum { OF_OBJ_HIDDEN = 0, OF_OBJ_IMAGE, OF_OBJ_MARK, OF_OBJ_TEXT, OF_OBJ_BREAK, OF_OBJ_NOTE };   /* TEXT: a field's text; NOTE: a footnote's number */

typedef struct {
    uint8_t  kind;              /* OF_OBJ_* */
    char    *raw;               /* the XML, exactly as it was */
    char    *text;              /* what an OF_OBJ_TEXT shows (a field's result, a footnote's number) */
    char    *target;            /* image: its part in the package ("word/media/image1.png"), or a file */
    uint8_t  file;              /* target is a file of the card, not a part */
    int32_t  w, h;              /* image: size in points */
} of_obj_t;

enum { OF_AL_LEFT = 0, OF_AL_CENTER, OF_AL_RIGHT, OF_AL_JUSTIFY };
enum { OF_LIST_NONE = 0, OF_LIST_BULLET, OF_LIST_NUMBER };

#define OF_PP_STYLE  0x01       /* paragraph properties changed: the style */
#define OF_PP_ALIGN  0x02

typedef struct {
    char     *txt;              /* NUL-terminated */
    uint16_t *at;               /* run style per byte */
    int       len, cap;
    int      *obj;              /* object index of each U+FFFC, in order */
    int       nobj, objcap;
    /* what it is */
    uint8_t   heading;          /* 1..9, 0 not a heading */
    uint8_t   align;
    uint8_t   list, ilvl;       /* OF_LIST_*, level */
    uint8_t   indent;           /* read-only documents: levels of indent (quotes, trees) */
    int       numid;            /* docx numId / odt list style index, -1 */
    char      label[24];        /* the list's number or bullet, worked out */
    int       pstyle;           /* index of doc->ps, -1 none */
    uint16_t  mark;             /* the run style typing starts with in an empty paragraph */
    uint8_t   pmask;            /* OF_PP_* */
    /* where it lives */
    int       cell;             /* 0 body, else a table cell (doc->cells index + 1) */
    int       orig, slot;       /* original paragraph index (-1 new); where it goes */
    bool      dirty, locked, sect;
    uint8_t   in_list;          /* 1 in a list; odt 2: the whole list item is this paragraph's */
    char     *raw_open;         /* docx "<w:p ...>"; odt "<text:p ...>" */
    char     *raw_ppr;          /* docx: the original <w:pPr>...</w:pPr>; odt: its list item's start tag */
    /* the view's */
    int32_t   y, h;
} of_para_t;

typedef struct {
    char    *id, *name;         /* docx styleId and name; odt style name */
    int      parent;            /* basedOn */
    uint8_t  heading;           /* outline level */
    uint8_t  b, i, u, s, align;
    uint16_t half_pt;
    uint32_t color;
} of_pstyle_t;

typedef struct {
    int     table, row, col, span;
    bool    vmerged;            /* continues the cell above */
} of_cell_t2;

typedef struct {
    int     nrows, ncols;
    int32_t colw[64];           /* points */
} of_table_t;

typedef struct of_doc_s {
    int         fmt;            /* OF_DOC_* */
    of_para_t  *p;
    int         np, pcap;
    of_rstyle_t *rs;
    int         nrs;
    of_obj_t   *obj;
    int         nobj;
    char      **links;          /* the hyperlinks' raw start tags */
    int         nlinks;
    of_pstyle_t *ps;
    int         nps;
    int         ps_normal;
    uint16_t    def_half_pt;    /* the document's default size */
    of_cell_t2 *cells;
    int         ncells;
    of_table_t *tables;
    int         ntables;
    /* numbering: numId -> per level format (0 bullet, 1 decimal, 2 lower letter, 3 upper letter, 4 lower roman, 5 upper roman) */
    struct { int id; uint8_t fmt[9]; char bullet[9][8]; } *nums;
    int         nnums;
    /* the file */
    uint8_t    *file;
    size_t      file_len;
    of_zip_t    zip;
    char       *part;           /* the main part's name */
    char       *src;            /* its original XML */
    size_t      src_len;
    uint32_t   *span0, *span1;  /* original paragraph i: [span0, span1) in src */
    int         norig;
    bool        any_locked, has_track;
    int         nnew_styles;    /* odt: automatic styles added */
    bool        readonly;       /* read from RTF, HTML, EPUB...: shown, saved only as a copy */
    char        kind[16];       /* what it was read from, for the person: "EPUB" */
    char       *note;           /* something to say when it opens */
} of_doc_t;

/* the model */
of_doc_t  *of_doc_new(int fmt);
void       of_doc_free(of_doc_t *d);
of_para_t *of_doc_insert_para(of_doc_t *d, int at);         /* empty */
void       of_doc_remove_para(of_doc_t *d, int at);
int        of_doc_style(of_doc_t *d, const of_rstyle_t *s);  /* interned index */
int        of_doc_add_obj(of_doc_t *d, int kind, const char *raw, size_t n);
void       of_para_append(of_doc_t *d, of_para_t *p, const char *s, size_t n, uint16_t style);
void       of_para_append_obj(of_doc_t *d, of_para_t *p, int obj, uint16_t style);
/* Editing: they mark the paragraphs dirty. Positions are bytes. */
void of_para_insert(of_doc_t *d, int pi, int pos, const char *s, int n, uint16_t style);
void of_para_delete(of_doc_t *d, int pi, int pos, int n);
bool of_doc_split(of_doc_t *d, int pi, int pos);            /* Enter */
bool of_doc_can_merge(const of_doc_t *d, int pi);           /* pi into pi-1 */
bool of_doc_merge(of_doc_t *d, int pi);
/* Turns a property on or off over [pos, pos+n) of a paragraph. */
void of_para_restyle(of_doc_t *d, int pi, int pos, int n, uint8_t mask, const of_rstyle_t *val);
/* The effective properties of a run in a paragraph (styles resolved). */
void of_doc_effective(const of_doc_t *d, const of_para_t *p, uint16_t rs, of_rstyle_t *out);
/* the numbers and bullets of the lists */
void of_doc_number(of_doc_t *d);
/* plain text of everything, a paragraph per line */
char *of_doc_plain(const of_doc_t *d);
/* the object of the k-th U+FFFC of a paragraph */
of_obj_t *of_para_obj_at(const of_doc_t *d, const of_para_t *p, int pos);

/* copies of paragraphs, for undo */
of_para_t *of_para_clone(const of_para_t *src);
void       of_para_free_fields(of_para_t *p);

/* files */
of_doc_t *of_docx_load(const char *path, char *err, size_t errlen);
bool      of_docx_save(of_doc_t *d, const char *path, char *err, size_t errlen);
of_doc_t *of_odt_load(const char *path, char *err, size_t errlen);
bool      of_odt_save(of_doc_t *d, const char *path, char *err, size_t errlen);
of_doc_t *of_doc_load(const char *path, char *err, size_t errlen);
bool      of_doc_save(of_doc_t *d, int fmt, const char *path, char *err, size_t errlen);
bool      of_doc_new_file(int fmt, const char *path, const char *title);
bool      of_is_doc_ext(const char *ext);
/* a part of the package, inflated (pictures) */
uint8_t  *of_doc_part(const of_doc_t *d, const char *name, size_t *len);
/* a picture object's bytes (from its part or its file) and its kind: "png", "jpeg", "gif", "bmp" */
uint8_t  *of_doc_obj_bytes(const of_doc_t *d, const of_obj_t *o, size_t *len, const char **ext);
/* links of documents read from other formats are URLs; those of docx and odt, start tags */
bool      of_link_is_url(const char *l);

/* -------------------------------------------------------------------------- */
/* Building a document from another format (of_import.c)                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    of_doc_t   *d;
    of_rstyle_t st;             /* the style of what is appended */
    int         cur;            /* the paragraph being filled, -1 none */
    bool        space;          /* collapsing white space: one is pending */
    bool        pre;            /* keep white space as it is */
    int         heading, list, ilvl, align, numid, indent;
    int         cell;           /* the cell being filled, 0 none */
    int         table, row, col;
} of_bld_t;

void of_bld_init(of_bld_t *b, of_doc_t *d);
/* A new paragraph (the current one, if still empty, is reused). */
void of_bld_para(of_bld_t *b);
/* Text in the current style; white space collapsed unless b->pre. */
void of_bld_text(of_bld_t *b, const char *s, size_t n);
void of_bld_link(of_bld_t *b, const char *url);           /* NULL: the link ends */
void of_bld_image(of_bld_t *b, const char *target, bool file, int w_pt, int h_pt);
void of_bld_break(of_bld_t *b);
void of_bld_table(of_bld_t *b);                           /* a new table */
void of_bld_row(of_bld_t *b);
void of_bld_cell(of_bld_t *b, int span);
void of_bld_table_end(of_bld_t *b);
void of_bld_done(of_bld_t *b);
/* A file of the card for a piece of data (a picture in an RTF, an FB2, a mail). */
bool of_cache_file(const char *name, const void *data, size_t n, char *out, size_t len);

/* the readers; each gives a read-only document */
of_doc_t *of_html_load(const char *path, char *err, size_t errlen);
of_doc_t *of_epub_load(const char *path, char *err, size_t errlen);
of_doc_t *of_eml_load(const char *path, char *err, size_t errlen);
of_doc_t *of_fb2_load(const char *path, char *err, size_t errlen);
of_doc_t *of_rtf_load(const char *path, char *err, size_t errlen);
of_doc_t *of_md_load(const char *path, char *err, size_t errlen);
of_doc_t *of_vcf_load(const char *path, char *err, size_t errlen);
of_doc_t *of_ics_load(const char *path, char *err, size_t errlen);
of_doc_t *of_tree_load(const char *path, char *err, size_t errlen);   /* json, xml, yaml, toml */
of_doc_t *of_image_load(const char *path, char *err, size_t errlen);  /* a picture alone */
of_doc_t *of_iwork_load(const char *path, char *err, size_t errlen);  /* the preview of Pages, Numbers, Keynote */
/* PDF's text, reflowed (of_pdftext.c); err "password": it needs one, or it was wrong */
of_doc_t *of_pdf_load(const char *path, const char *password, char *err, size_t errlen);
/* HTML text into a builder (EPUB chapters, mail bodies); base: the folder
 * of the HTML inside the package, for its pictures ("OEBPS/") */
void of_html_into(of_bld_t *b, const char *html, size_t n, const char *base, bool in_zip);
void of_md_into(of_bld_t *b, const char *md, size_t n, const char *dir);
/* which reader takes this extension: 0 none */
enum { OF_RD_NONE = 0, OF_RD_HTML, OF_RD_EPUB, OF_RD_EML, OF_RD_FB2, OF_RD_RTF, OF_RD_MD, OF_RD_VCF, OF_RD_ICS, OF_RD_IMG, OF_RD_IWORK, OF_RD_PDF };
int  of_reader_for(const char *ext);
bool of_is_tree_ext(const char *ext);                       /* json, xml, yaml, toml... */
/* the reader for the file's extension (of_tree_load for the data files) */
of_doc_t *of_read_any(const char *path, char *err, size_t errlen);
/* the app's _(), for the words the readers write ("De", "Adjuntos") */
extern const char *(*of_tr)(const char *es);
