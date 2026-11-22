/*
 * P4OS - Office: what the app's files share.
 *
 * The app is in three layers:
 *
 *   - the formats, which know nothing of LVGL and also build on the Mac as a
 *     test program (tools/host_test.c): the ZIP package, the XML tokenizer,
 *     the workbook with its formulas and number formats, and the readers and
 *     writers of CSV, XLSX and ODS; the text buffer;
 *   - the views: the grid of a sheet and the text editor, drawn by the app;
 *   - the shell: the files, the recent list, the menus and the keyboard.
 *
 * Saving never loses what the app did not understand. A package is copied
 * entry by entry, and only the parts that were edited are written again;
 * inside an edited sheet the rows and cells nobody touched go out byte for
 * byte (each cell remembers where its XML was in the original part).
 *
 * Everything runs in LVGL's task. The formats allocate in PSRAM.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Memory and strings (of_util.c)                                              */
/* -------------------------------------------------------------------------- */

void *of_alloc(size_t n);               /* zeroed */
void *of_realloc(void *p, size_t n);
void  of_free(void *p);
char *of_strdup(const char *s);
char *of_strndup(const char *s, size_t n);

/* A growing byte string, always NUL-terminated. */
typedef struct {
    char  *s;
    size_t n, cap;
    bool   oom;
} of_sb_t;

void of_sb_put(of_sb_t *b, const char *s, size_t n);
void of_sb_puts(of_sb_t *b, const char *s);
void of_sb_putc(of_sb_t *b, char c);
void of_sb_printf(of_sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void of_sb_xml(of_sb_t *b, const char *s, size_t n);    /* escaped for XML text and attributes */
void of_sb_free(of_sb_t *b);
char *of_sb_take(of_sb_t *b);           /* the string, the builder emptied */

/* The whole file, NUL-terminated (*len without it). NULL if missing or over max. */
uint8_t *of_file_read(const char *path, size_t *len, size_t max);
/* Writes to path.tmp and puts it in place only once it is all on the card. */
bool of_file_write(const char *path, const void *data, size_t len);
bool of_file_exists(const char *path);
const char *of_ext(const char *path);   /* "xlsx", lower case, "" if none */
const char *of_basename(const char *path);

/* UTF-8 */
int  of_utf8_len(uint8_t lead);         /* bytes of a sequence, 1 for a stray byte */
int  of_utf8_put(char *out, uint32_t cp);
uint32_t of_utf8_get(const char *s, int *len);
bool of_utf8_valid(const char *s, size_t n);
/* Latin-1 / Windows-1252 to UTF-8 and back (unmappable -> '?'). */
char *of_latin1_to_utf8(const char *s, size_t n, size_t *out_len);
char *of_utf8_to_latin1(const char *s, size_t n, size_t *out_len, int *lost);

uint32_t of_crc32(uint32_t crc, const void *p, size_t n);

/* -------------------------------------------------------------------------- */
/* Deflate (of_deflate.c): raw deflate, LZ77 with fixed Huffman codes          */
/* -------------------------------------------------------------------------- */

bool of_deflate(const uint8_t *in, size_t n, of_sb_t *out);
/* Raw inflate of a whole buffer; NULL on a damaged stream. NUL-terminated. */
uint8_t *of_inflate(const uint8_t *in, size_t n, size_t expect, size_t *out_len);
/* Tolerant (PDF): zlib header or raw; what came out before an error. */
uint8_t *of_inflate_lax(const uint8_t *in, size_t n, bool zlib, size_t *out_len);

/* -------------------------------------------------------------------------- */
/* ZIP (of_zip.c)                                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    char    *name;
    uint16_t method, flags, mtime, mdate;
    uint32_t crc, csize, usize;
    uint32_t local;             /* offset of the local header */
    uint32_t data;              /* offset of the data          */
    uint32_t ext_attr;
    uint16_t ver_made;
} of_zentry_t;

typedef struct {
    const uint8_t *buf;         /* the whole file, borrowed */
    size_t         len;
    of_zentry_t   *e;
    int            n;
} of_zip_t;

bool     of_zip_open(of_zip_t *z, const uint8_t *buf, size_t len);
void     of_zip_close(of_zip_t *z);
int      of_zip_find(const of_zip_t *z, const char *name);   /* -1 */
uint8_t *of_zip_read(const of_zip_t *z, int i, size_t *len); /* NUL-terminated */

typedef struct {
    of_sb_t  out;               /* the package, built in memory */
    of_sb_t  cd;                /* its central directory        */
    int      count;
    bool     ok;
} of_zipw_t;

void of_zipw_init(of_zipw_t *w);
/* The entry as it is: same compressed bytes, same CRC. */
bool of_zipw_copy(of_zipw_t *w, const of_zip_t *z, int i);
bool of_zipw_add(of_zipw_t *w, const char *name, const void *data, size_t n, bool deflate);
/* Closes the directory and writes the file (atomically). */
bool of_zipw_finish(of_zipw_t *w, const char *path);
void of_zipw_free(of_zipw_t *w);

/* -------------------------------------------------------------------------- */
/* XML (of_xml.c): a pull tokenizer that keeps byte offsets                    */
/* -------------------------------------------------------------------------- */

enum { OX_END = 0, OX_OPEN, OX_CLOSE, OX_EMPTY, OX_TEXT, OX_OTHER, OX_ERROR };

typedef struct {
    const char *src;
    size_t      len, pos;
    /* the token */
    int         type;
    size_t      start, end;     /* the whole token: "<c ...>" or the text   */
    const char *name;           /* element name (with its prefix)            */
    int         name_len;
    const char *attrs;          /* raw attributes of an OPEN / EMPTY        */
    int         attrs_len;
} of_xml_t;

void of_xml_init(of_xml_t *x, const char *src, size_t len);
int  of_xml_next(of_xml_t *x);
bool of_xml_is(const of_xml_t *x, const char *name);     /* with prefix: "c", "table:table-cell" */
/* The local name matches whatever the prefix (w:t, a:t, t). */
bool of_xml_local_is(const of_xml_t *x, const char *local);
/* An attribute of the current token, decoded into out; false if absent. */
bool of_xml_attr(const of_xml_t *x, const char *name, char *out, size_t len);
/* Raw (still escaped) value of an attribute in an attribute string. */
bool of_xml_attr_raw(const char *attrs, int alen, const char *name, const char **v, int *vlen);
/* Skips to the end of the element just opened (the current token must be OPEN). */
void of_xml_skip(of_xml_t *x);
/* Appends the text of an element (current token OPEN) and its children to sb,
 * decoded, until its end. */
void of_xml_text(of_xml_t *x, of_sb_t *sb);
/* Decodes entities and character references. */
void of_xml_unescape(const char *s, size_t n, of_sb_t *out);

/* -------------------------------------------------------------------------- */
/* The workbook (of_sheet.c)                                                   */
/* -------------------------------------------------------------------------- */

enum { OV_EMPTY = 0, OV_NUM, OV_STR, OV_BOOL, OV_ERR };
enum { OE_NONE = 0, OE_NULL, OE_DIV0, OE_VALUE, OE_REF, OE_NAME, OE_NUM, OE_NA, OE_CIRC };

typedef struct {
    uint8_t t;                  /* OV_* */
    uint8_t err;                /* OE_* of an OV_ERR */
    double  n;                  /* OV_NUM, OV_BOOL (0/1) */
    char   *s;                  /* OV_STR, owned by whoever holds the value */
} of_val_t;

void of_val_free(of_val_t *v);
void of_val_copy(of_val_t *dst, const of_val_t *src);
const char *of_err_name(int err);       /* "#DIV/0!" */
int  of_err_from_name(const char *s);

/* Cell flags */
#define OC_EDITED     0x01      /* changed by the person: written again   */
#define OC_FORMULA    0x02      /* f holds a formula                      */
#define OC_UNKNOWN_F  0x04      /* a function the app does not have: its
                                 * cached value stays, and so does its XML */
#define OC_ARRAY      0x08      /* part of an array formula: kept as is   */
#define OC_SHARED     0x10      /* xlsx shared formula member             */
#define OC_STYLED     0x20      /* style changed (number format)          */

/* What a number is when no number format says it (CSV, ODS) */
enum { OH_NONE = 0, OH_DATE, OH_TIME, OH_DATETIME, OH_PCT, OH_CUR };

typedef struct of_node of_node_t;

typedef struct {
    int32_t   col;
    uint8_t   flags;
    uint8_t   state;            /* recalculation: 0 stale, 1 busy, 2 done */
    uint16_t  shared_si;        /* xlsx shared formula group + 1, 0 none  */
    uint8_t   hint;             /* OH_*: what a number is, without a format */
    uint32_t  style;            /* xlsx s= ; ods index in the sheet's style names */
    of_val_t  v;                /* the value, or a formula's result       */
    of_val_t  cached;           /* the result as read from the file       */
    char     *f;                /* formula, Excel syntax, without '='     */
    of_node_t *ast;
    char     *disp;             /* ods: the text the file showed (unedited) */
    /* where the cell's XML was in the original part (0 len: not there) */
    uint32_t  raw_off, raw_len;
    uint32_t  raw_rep;          /* ods: number-columns-repeated of its run */
} of_cell_t;

typedef struct {
    int32_t    row;
    uint8_t    dirty;           /* a cell of it changed */
    uint8_t    hidden;          /* by the filter (not saved) */
    float      height;          /* points, 0 = default */
    of_cell_t *c;               /* sorted by col */
    int        n, cap;
    uint32_t   raw_off, raw_len;        /* the whole <row>...</row>     */
    uint32_t   raw_tag_end;             /* end of its start tag          */
    uint32_t   raw_rep;                 /* ods number-rows-repeated      */
    void      *fx;              /* the format's own (ods: the runs of cells) */
    void     (*fx_free)(void *);
} of_row_t;

typedef struct {
    uint32_t c1, r1, c2, r2;
} of_range_t;

typedef struct {
    char      *name;
    of_row_t  *rows;            /* sorted by row */
    int        nrows, rcap;
    float     *colw;            /* column widths in characters, 0 = default */
    int        ncolw;
    uint8_t   *colw_set;        /* changed by the person */
    float      def_colw, def_rowh;
    int        freeze_r, freeze_c;      /* rows / columns frozen at the top, left */
    of_range_t *merges;
    int        nmerges;
    bool       dirty;           /* a cell of it changed */
    bool       cols_dirty;
    /* the format's own */
    char      *part;            /* xlsx: "xl/worksheets/sheet1.xml" */
    char      *rid;
    uint32_t   sheet_id;
    int        state;           /* 0 visible, 1 hidden */
} of_sheet_t;

/* A number format: an Excel format code, parsed when first used. */
typedef struct {
    uint32_t id;
    char    *code;
} of_numfmt_t;

typedef struct {
    uint32_t numfmt;            /* numFmtId */
    uint8_t  bold, italic, underline, halign;   /* 0 general, 1 left, 2 center, 3 right */
    uint32_t color, fill;       /* 0xRRGGBB | 0x1000000 when set */
    uint32_t from;              /* an added one: the xf it was made from */
} of_xf_t;

enum { OF_FMT_NONE = 0, OF_FMT_CSV, OF_FMT_TSV, OF_FMT_XLSX, OF_FMT_ODS, OF_FMT_XLS };   /* XLS: read only, saved as a copy */

typedef struct of_book of_book_t;

struct of_book {
    int         fmt;            /* OF_FMT_* as loaded */
    of_sheet_t *sh;
    int         nsh;
    bool        date1904;
    of_numfmt_t *nf;            /* custom number formats */
    int         nnf;
    of_xf_t    *xf;
    int         nxf;
    bool        styles_dirty;   /* an xf was added */
    int         nxf_orig;
    /* the file */
    uint8_t    *file;           /* the original bytes */
    size_t      file_len;
    of_zip_t    zip;
    char      **sst;            /* xlsx shared strings */
    int         nsst;
    /* ods: names of the cell styles, index = of_cell_t.style */
    char      **sty;
    char      **sty_code;       /* ods: each style's number format, Excel's way, or NULL */
    int         nsty;
    /* csv */
    char        csv_sep;
    char        csv_true[12], csv_false[12];    /* the words the file used */
    bool        csv_crlf, csv_latin1, csv_bom, csv_quote_all;
    /* ods: the original content.xml */
    char       *ods_content;
    size_t      ods_content_len;
    /* what was found and will be kept, for the person */
    char        notes[256];
    bool        any_unknown_f;
};

of_book_t *of_book_new(int fmt);        /* one empty sheet */
void       of_book_free(of_book_t *b);
of_sheet_t *of_book_add_sheet(of_book_t *b, const char *name);

of_cell_t *of_cell_get(const of_sheet_t *s, int32_t row, int32_t col);
of_cell_t *of_cell_make(of_sheet_t *s, int32_t row, int32_t col);
of_row_t  *of_row_get(const of_sheet_t *s, int32_t row);
of_row_t  *of_row_make(of_sheet_t *s, int32_t row);
void       of_cell_clear_value(of_cell_t *c);
/* A shared formula group becomes separate formulas (one of them is edited). */
void       of_sheet_unshare(of_sheet_t *s, uint16_t si);
/* The extent of the cells with something in them: max row and col, -1 when empty. */
void       of_sheet_extent(const of_sheet_t *s, int32_t *maxr, int32_t *maxc);
float      of_sheet_colw(const of_sheet_t *s, int col);
void       of_sheet_set_colw(of_sheet_t *s, int col, float w);

/* What the person typed into a cell: a number (with the locale's decimal
 * comma or point, %, a date d/m/y), TRUE/FALSE, "=formula", or text. A
 * leading apostrophe forces text. Marks the cell edited. */
void of_cell_input(of_book_t *b, of_sheet_t *s, int32_t row, int32_t col, const char *text);
/* What goes in the edit bar: "=SUM(A1:A3)", "12.5", "texto". */
void of_cell_source(const of_book_t *b, const of_cell_t *c, char *out, size_t len);
/* The format a hint implies. */
const char *of_hint_code(int hint);
/* What the grid shows, formatted. */
void of_cell_display(const of_book_t *b, const of_cell_t *c, char *out, size_t len, bool *right);

/* Locale: decimal comma (es, de) or point. */
void of_locale_set(bool decimal_comma);
bool of_locale_comma(void);
/* The interface language for TRUE/FALSE and month names: 0 en, 1 es, 2 de. */
void of_locale_lang(int lang);
int  of_locale_get_lang(void);
const char *of_bool_word(bool v);

/* A1 references */
void of_col_name(int32_t col, char *out);            /* 0 -> "A", 26 -> "AA" */
bool of_parse_ref(const char *s, int32_t *row, int32_t *col, int *used);
void of_ref_name(int32_t row, int32_t col, char *out);   /* "B3" */

/* Sorting a block of rows by one column. Formulas move with their rows, their
 * relative references shifted. */
void of_sheet_sort(of_book_t *b, of_sheet_t *s, int32_t r1, int32_t r2, int32_t c1, int32_t c2,
                   int32_t key_col, bool desc);

/* -------------------------------------------------------------------------- */
/* Formulas (of_formula.c)                                                     */
/* -------------------------------------------------------------------------- */

/* Parses Excel syntax ("SUM(A1:B2)*2", ',' or ';' between arguments, the
 * Spanish names of the common functions too). NULL on a syntax error. */
of_node_t *of_formula_parse(const char *src, int sheet_ix, const of_book_t *b, bool *unknown_fn);
void       of_formula_free(of_node_t *n);
/* OH_DATE for DATE() and TODAY(), OH_DATETIME for NOW(): the cell shows a date. */
int        of_formula_result_hint(const of_node_t *n);
/* The formula written back, Excel syntax, English names (what xlsx keeps). */
void       of_formula_write(const of_node_t *n, const of_book_t *b, of_sb_t *out);
/* The same formula moved by (dr, dc): relative references shift. */
char      *of_formula_shift(const char *src, const of_book_t *b, int sheet_ix, int dr, int dc);
/* The functions the app has, for the help: their names in lang (0 en, 1 es, 2 de). */
int         of_formula_fn_count(void);
const char *of_formula_fn_name(int i, int lang);
/* Every formula of the book again. */
void       of_book_recalc(of_book_t *b);
/* Excel serial date for today, or 0 without a clock. */
double     of_today(void);

/* ODF formulas ("of:=SUM([.A1:.A3])") to Excel syntax and back. */
char *of_odf_to_excel(const char *src);
char *of_excel_to_odf(const char *src);

/* -------------------------------------------------------------------------- */
/* Number formats (of_numfmt.c)                                                */
/* -------------------------------------------------------------------------- */

const char *of_numfmt_code(const of_book_t *b, uint32_t id);     /* built-in or custom */
/* Formats v with the code; *right: numbers align right. */
void of_numfmt_format(const char *code, const of_val_t *v, bool date1904, char *out, size_t len, bool *right);
bool of_numfmt_is_date(const char *code);
/* What a code is, roughly, for writing it into another format. */
typedef struct {
    int  kind;                  /* 0 general, 1 number, 2 percent, 3 date, 4 time, 5 date and time, 6 text */
    int  decimals;
    bool grouping;
    char prefix[16], suffix[16];
} of_nfinfo_t;
void of_numfmt_info(const char *code, of_nfinfo_t *out);
/* The cell's number format code, whatever the format of the book. */
const char *of_cell_code(const of_book_t *b, const of_cell_t *c);
void of_fmt_general(double n, char *out, size_t len);
/* Excel serial <-> date */
double of_date_serial(int y, int m, int d);
void   of_serial_date(double s, int *y, int *m, int *d);
/* The xf of a cell with its number format replaced; adds one if needed. */
uint32_t of_book_xf_with_numfmt(of_book_t *b, uint32_t xf, const char *code);

/* -------------------------------------------------------------------------- */
/* Readers and writers                                                         */
/* -------------------------------------------------------------------------- */

/* NULL with a reason in err. */
of_book_t *of_csv_load(const char *path, char *err, size_t errlen);
of_book_t *of_xlsx_load(const char *path, char *err, size_t errlen);
of_book_t *of_ods_load(const char *path, char *err, size_t errlen);
of_book_t *of_book_load(const char *path, char *err, size_t errlen);   /* by extension */

/* Saves in fmt (OF_FMT_*). A book saved in the format it was read from keeps
 * everything; saving into another format builds a new package. */
bool of_csv_save(of_book_t *b, int sheet, const char *path, char *err, size_t errlen);
bool of_xlsx_save(of_book_t *b, const char *path, char *err, size_t errlen);
bool of_ods_save(of_book_t *b, const char *path, char *err, size_t errlen);
bool of_book_save(of_book_t *b, int fmt, int sheet, const char *path, char *err, size_t errlen);
/* What saving into fmt would lose, for the warning: OF_LOSE_* bits. */
enum { OF_LOSE_SHEETS = 1, OF_LOSE_FORMULAS = 2, OF_LOSE_FORMATS = 4 };
int  of_book_save_losses(const of_book_t *b, int fmt);
int  of_fmt_from_ext(const char *ext);
bool of_is_sheet_ext(const char *ext);

/* An Office file encrypted with a password (a CFB file with EncryptedPackage). */
bool of_is_encrypted_ooxml(const uint8_t *buf, size_t len);

/* -------------------------------------------------------------------------- */
/* Text (of_text.c)                                                            */
/* -------------------------------------------------------------------------- */

enum { OT_ENC_UTF8 = 0, OT_ENC_LATIN1, OT_ENC_UTF16LE, OT_ENC_UTF16BE };
enum { OT_EOL_LF = 0, OT_EOL_CRLF, OT_EOL_CR };

/* The text, UTF-8 with '\n' line ends, in a gap buffer. */
typedef struct {
    char    *buf;
    size_t   cap, gap0, gap1;   /* the gap is [gap0, gap1) */
    uint32_t *line;             /* start of each line, logical offsets */
    int      nlines, lcap;
    int      enc, eol;
    bool     bom, final_eol;
    bool     modified;
} of_text_t;

bool   of_text_load(of_text_t *t, const char *path, char *err, size_t errlen);
void   of_text_init(of_text_t *t);
void   of_text_free(of_text_t *t);
size_t of_text_len(const of_text_t *t);
char   of_text_at(const of_text_t *t, size_t pos);
void   of_text_insert(of_text_t *t, size_t pos, const char *s, size_t n);
void   of_text_delete(of_text_t *t, size_t pos, size_t n);
/* A copy of [a, b), NUL-terminated. */
char  *of_text_range(const of_text_t *t, size_t a, size_t b);
/* Line of a position, and the line's [start, end) without its '\n'. */
int    of_text_line_of(const of_text_t *t, size_t pos);
void   of_text_line_span(const of_text_t *t, int line, size_t *a, size_t *b);
/* Bytes of one line into out (NUL-terminated, cut at len). */
int    of_text_line_copy(const of_text_t *t, int line, char *out, int len);
/* Search from pos forward (or backward), case-insensitive in ASCII. -1 if none. */
long   of_text_find(const of_text_t *t, size_t pos, const char *needle, bool back, bool match_case);
bool   of_text_save(of_text_t *t, const char *path, char *err, size_t errlen);
