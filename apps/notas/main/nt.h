/*
 * P4OS - Notes: what the app's files share.
 *
 * A note is a list of blocks (paragraphs, headings, list items, quotes, a
 * rule), each with its text and one style word per byte of it. It lives on
 * the card as Markdown with a little inline HTML for what Markdown has no
 * syntax for (underline, colours, highlight, sizes): nt_doc.c writes it and
 * reads it back, and it stays readable, and editable by hand, from the
 * portal's file manager.
 *
 * Everything here runs in LVGL's task. The one exception is the thread that
 * types a note into a computer (notas.c), which works on its own copy.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Memory: everything the app allocates goes to PSRAM (docs/MEMORY.md)         */
/* -------------------------------------------------------------------------- */

void *nt_alloc(size_t n);               /* zeroed */
void *nt_realloc(void *p, size_t n);
void  nt_free(void *p);
char *nt_strdup(const char *s);

/* A growing string. */
typedef struct {
    char *s;
    int   n, cap;
} nt_sb_t;

void nt_sb_put(nt_sb_t *b, const char *s, int n);   /* n < 0: strlen */
void nt_sb_putc(nt_sb_t *b, char c);
void nt_sb_printf(nt_sb_t *b, const char *fmt, ...);
void nt_sb_free(nt_sb_t *b);

/* -------------------------------------------------------------------------- */
/* Fonts (nt_fonts.c)                                                          */
/* -------------------------------------------------------------------------- */

enum { NT_SZ_M = 0, NT_SZ_S, NT_SZ_L, NT_SZ_XL, NT_SZ_COUNT };   /* 28 24 36 48 px */
#define NT_ST_BOLD    1
#define NT_ST_ITALIC  2

const lv_font_t *nt_font(int style, int size);
bool nt_font_faux_bold(int style, int size);   /* bold asked, the pack is missing */
bool nt_fonts_have_pack(void);
int  nt_font_px(int size);
void nt_fonts_free(void);

/* -------------------------------------------------------------------------- */
/* Theme (nt_theme.c)                                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    bool dark;
    lv_color_t bg, surface, surface2, text, dim, hair, accent, sel, danger;
    lv_color_t kb_bg, kb_key, kb_key2, kb_text;
} nt_theme_t;

extern nt_theme_t NT;
void nt_theme_set(bool dark);

#define NT_FG_COUNT   10        /* 0 = the theme's text colour */
#define NT_HL_COUNT   7         /* 0 = none */
#define NT_TAG_COUNT  9         /* 0 = none */
lv_color_t nt_fg_color(int i);
lv_color_t nt_hl_color(int i);
lv_color_t nt_tag_color(int i);         /* the swatch */
lv_color_t nt_tag_card(int i);          /* a card tinted with it */
const char *nt_tag_name(int i);         /* for the files: "red"... */
int nt_tag_from_name(const char *s);
lv_color_t nt_prio_color(int p);        /* 1 low .. 3 high */

/* -------------------------------------------------------------------------- */
/* The document (nt_doc.c)                                                     */
/* -------------------------------------------------------------------------- */

enum {
    BT_TITLE = 0,   /* block 0, always, and only there */
    BT_PARA,
    BT_H1, BT_H2, BT_H3,
    BT_BULLET,      /* style = NT_BS_* */
    BT_NUM,         /* style = NT_NS_* */
    BT_CHECK,       /* style = priority 0..3, checked */
    BT_QUOTE,
    BT_RULE,        /* no text */
};

enum { NT_BS_DISC, NT_BS_CIRCLE, NT_BS_SQUARE, NT_BS_DASH, NT_BS_ARROW,
       NT_BS_STAR, NT_BS_CHECK, NT_BS_DIAMOND, NT_BS_COUNT };
enum { NT_NS_DEC, NT_NS_ALPHA, NT_NS_ALPHA_UP, NT_NS_ROMAN, NT_NS_ROMAN_UP, NT_NS_COUNT };

/* The style word, one per byte of text. 0 is plain text. */
#define A_B         0x0001
#define A_I         0x0002
#define A_U         0x0004
#define A_S         0x0008
#define A_SZ_SHIFT  4           /* NT_SZ_*: 0 normal, 1 small, 2 large, 3 huge */
#define A_SZ_MASK   (3u << A_SZ_SHIFT)
#define A_FG_SHIFT  6           /* 0..15, nt_fg_color */
#define A_FG_MASK   (15u << A_FG_SHIFT)
#define A_HL_SHIFT  10          /* 0..7, nt_hl_color */
#define A_HL_MASK   (7u << A_HL_SHIFT)
#define A_SZ(a)     (((a) & A_SZ_MASK) >> A_SZ_SHIFT)
#define A_FG(a)     (((a) & A_FG_MASK) >> A_FG_SHIFT)
#define A_HL(a)     (((a) & A_HL_MASK) >> A_HL_SHIFT)

#define NT_MAX_INDENT 5

typedef struct {
    int     start, end;         /* bytes */
    int32_t y, h, asc;          /* from the block's top */
} nt_line_t;

typedef struct {
    uint8_t   type, style, indent, checked;
    int       len, cap;
    char     *txt;              /* NUL-terminated */
    uint16_t *at;               /* one per byte */
    /* layout, the editor's */
    int32_t   y, h;
    int       nlines, lcap;
    nt_line_t *lines;
    int       num;              /* ordinal of a BT_NUM */
} nt_blk_t;

typedef struct {
    nt_blk_t *b;
    int       n, cap;
} nt_doc_t;

enum { NT_KIND_NOTE = 0, NT_KIND_TASKS };

typedef struct {
    uint8_t  kind;
    uint8_t  tag;               /* colour, 0..NT_TAG_COUNT-1 */
    bool     pinned;
    bool     hide_done;         /* tasks */
    uint32_t created, modified; /* epoch, 0 = unknown (no clock yet) */
} nt_meta_t;

void      nt_doc_init(nt_doc_t *d);         /* empty, with its title block */
void      nt_doc_clear(nt_doc_t *d);        /* frees every block */
nt_blk_t *nt_doc_insert(nt_doc_t *d, int at, int type);
void      nt_doc_remove(nt_doc_t *d, int at);
void      nt_blk_insert(nt_blk_t *b, int pos, const char *s, int n, uint16_t attr);
void      nt_blk_delete(nt_blk_t *b, int pos, int n);
void      nt_blk_set(nt_blk_t *b, const char *s, uint16_t attr);
void      nt_doc_number(nt_doc_t *d);       /* fills num of the BT_NUM blocks */
void      nt_num_label(int style, int n, char *out, size_t len);

/* Markdown, both ways. from_md never fails: what it does not know is text. */
void nt_doc_to_md(const nt_doc_t *d, const nt_meta_t *m, nt_sb_t *out);
void nt_doc_from_md(nt_doc_t *d, nt_meta_t *m, const char *src, const char *fallback_title);
/* The text without styles, a paragraph per line (with "- " and "[x]"). */
void nt_doc_plain(const nt_doc_t *d, nt_sb_t *out, bool with_title);
int  nt_doc_words(const nt_doc_t *d, int *chars);

/* A frozen copy of blocks [b0, b1] cut at bytes p0 (in b0) and p1 (in b1):
 * the undo history and the clipboard. */
typedef struct nt_frag nt_frag_t;
nt_frag_t *nt_frag_copy(const nt_doc_t *d, int b0, int p0, int b1, int p1);
nt_frag_t *nt_frag_whole(const nt_doc_t *d);
void       nt_frag_restore(nt_doc_t *d, const nt_frag_t *f);   /* the whole doc */
/* Pastes at (b, p); leaves (b, p) after what went in. */
void       nt_frag_paste(nt_doc_t *d, const nt_frag_t *f, int *b, int *p);
size_t     nt_frag_size(const nt_frag_t *f);
int        nt_frag_blocks(const nt_frag_t *f);
const char *nt_frag_text(const nt_frag_t *f, int i, int *len);
int32_t    nt_text_width(const char *s, const lv_font_t *f);   /* kerned, like a label */
void       nt_frag_free(nt_frag_t *f);

/* -------------------------------------------------------------------------- */
/* The notes on the card (nt_store.c)                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    char     file[96];          /* name in the folder, "Compras.md" */
    char     title[96];
    char    *text;              /* plain text, for the preview and the search */
    nt_meta_t meta;
    uint32_t mtime;             /* the file's, when the note has no date */
    int      done, total;       /* tasks */
} nt_entry_t;

typedef struct {
    nt_entry_t *e;
    int         n, cap;
} nt_index_t;

const char *nt_dir(void);               /* /sdcard/notas */
const char *nt_trash_dir(void);         /* /sdcard/notas/papelera */
void   nt_index_scan(nt_index_t *ix, bool trash);
void   nt_index_free(nt_index_t *ix);
void   nt_index_sort(nt_index_t *ix, int order);   /* NT_ORDER_* */
enum { NT_ORDER_MODIFIED = 0, NT_ORDER_CREATED, NT_ORDER_TITLE, NT_ORDER_COUNT };
uint32_t nt_now(void);                  /* 0 while the clock is not set */

bool nt_load(const char *dir, const char *file, nt_doc_t *d, nt_meta_t *m);
/* Writes the note, renaming its file after the title when it changed.
 * 'file' is in and out. */
bool nt_save(char *file, size_t len, nt_doc_t *d, nt_meta_t *m, bool rename_it);
bool nt_new_file(const char *title, char *file, size_t len);   /* a free name */
bool nt_trash(const char *file);                /* to the bin */
bool nt_restore(const char *file);              /* back from it */
bool nt_purge(const char *file);                /* from the bin, for good */
int  nt_purge_all(void);
bool nt_duplicate(const char *file, char *out, size_t len);
void nt_seed(void);                     /* the welcome note, the first time */

/* -------------------------------------------------------------------------- */
/* Keyboard (nt_kb.c): Spanish, with ñ, accents and the space bar as a        */
/* trackpad. It types into whatever target is attached.                        */
/* -------------------------------------------------------------------------- */

typedef struct {
    void (*insert)(const char *utf8);
    void (*backspace)(void);
    void (*enter)(void);
    void (*move)(int dx);               /* the caret, in characters */
    bool (*caps)(void);                 /* should the next letter be upper case */
} nt_kb_target_t;

lv_obj_t *nt_kb_create(lv_obj_t *parent, int32_t w, int32_t h);
void      nt_kb_attach(const nt_kb_target_t *t);
void      nt_kb_refresh_caps(void);     /* the caret moved: maybe shift again */
void      nt_kb_destroyed(void);
/* A target that types into an lv_textarea; enter calls 'done'. */
void      nt_kb_attach_textarea(lv_obj_t *ta, void (*done)(lv_obj_t *ta));

/* -------------------------------------------------------------------------- */
/* Shared widgets (nt_ui.c)                                                    */
/* -------------------------------------------------------------------------- */

enum {
    NT_IC_BACK, NT_IC_PLUS, NT_IC_MORE, NT_IC_UNDO, NT_IC_REDO, NT_IC_SEARCH,
    NT_IC_PIN, NT_IC_TRASH, NT_IC_LIST, NT_IC_CHECKBOX, NT_IC_INDENT,
    NT_IC_OUTDENT, NT_IC_KB_HIDE, NT_IC_CLOSE, NT_IC_HANDLE, NT_IC_CHEVRON,
    NT_IC_NOTE, NT_IC_TASKS, NT_IC_HIGHLIGHT, NT_IC_TEXTCOLOR, NT_IC_AA,
    NT_IC_GRID, NT_IC_ROWS, NT_IC_CHECK, NT_IC_RESTORE, NT_IC_QR, NT_IC_TYPE,
    NT_IC_PALETTE, NT_IC_INFO, NT_IC_COPY, NT_IC_SUN, NT_IC_MOON,
};

lv_obj_t *nt_box(lv_obj_t *parent, int32_t w, int32_t h);   /* bare, transparent */
lv_obj_t *nt_text(lv_obj_t *parent, const char *s, const lv_font_t *f, lv_color_t c);
/* A square button with a drawn icon; 'color' tints the icon. */
lv_obj_t *nt_icon_btn(lv_obj_t *parent, int icon, int32_t size, lv_event_cb_t cb, void *ud);
void      nt_icon_set(lv_obj_t *btn, int icon);
void      nt_icon_color(lv_obj_t *btn, lv_color_t c);
/* Draws an icon centred in 'a' (for custom-drawn things). */
void      nt_draw_icon(lv_layer_t *layer, int icon, const lv_area_t *a, lv_color_t c, int32_t stroke);
void      nt_draw_bullet(lv_layer_t *layer, int style, int32_t cx, int32_t cy, int32_t r, lv_color_t c);
void      nt_draw_checkbox(lv_layer_t *layer, const lv_area_t *a, bool checked, bool round, lv_color_t c, lv_color_t on);
lv_obj_t *nt_pill(lv_obj_t *parent, const char *s, lv_event_cb_t cb, void *ud);

/* A sheet over everything: a list of actions, from the bottom upright and
 * centred lying down. Picking one calls cb(index, ud) and closes it. */
typedef struct {
    const char *label;
    int         icon;           /* -1 none */
    bool        danger;
    bool        checked;        /* a tick at the right */
} nt_item_t;
typedef void (*nt_pick_cb_t)(int index, void *ud);
lv_obj_t *nt_sheet(const char *title, const nt_item_t *items, int n, nt_pick_cb_t cb, void *ud);
/* An empty sheet the caller fills (returns its content column). */
lv_obj_t *nt_sheet_custom(const char *title, int32_t *content_w);
void      nt_sheet_close(void);
void      nt_sheet_forget(void);        /* its parent is about to be cleaned */
bool      nt_sheet_open(void);
/* Yes/no. */
void      nt_confirm(const char *title, const char *text, const char *yes, bool danger,
                     void (*cb)(void *ud), void *ud);
/* A row of colour swatches; cb gets the index. */
lv_obj_t *nt_swatches(lv_obj_t *parent, int32_t w, int count, lv_color_t (*color)(int),
                      int selected, bool first_is_none, lv_event_cb_t cb, void *ud);

/* -------------------------------------------------------------------------- */
/* The rich-text editor (nt_editor.c)                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    void (*changed)(void);              /* the text changed (autosave) */
    void (*caret)(void);                /* the caret or the selection moved */
    void (*focus)(void);                /* a tap in the text: show the keyboard */
} nt_ed_cbs_t;

lv_obj_t *nt_ed_create(lv_obj_t *parent, nt_doc_t *doc, const nt_ed_cbs_t *cbs);
void      nt_ed_destroyed(void);
void      nt_ed_relayout(void);         /* the width changed */
const nt_kb_target_t *nt_ed_target(void);
void      nt_ed_scroll_to_caret(void);
void      nt_ed_place(int blk, int pos);
void      nt_ed_caret_pos(int *blk, int *pos);
void      nt_ed_set_scroll(int32_t y);
int32_t   nt_ed_get_scroll(void);
bool      nt_ed_has_selection(void);
void      nt_ed_select_all(void);
void      nt_ed_clear_selection(void);
/* What applies at the caret or over the selection, for the toolbar. */
uint16_t  nt_ed_attr(void);             /* bits common to all the selection */
int       nt_ed_block_type(int *style);
/* Styles: over the selection, or for what is typed next. */
void      nt_ed_toggle(uint16_t bit);
void      nt_ed_set_size(int size);
void      nt_ed_set_fg(int fg);
void      nt_ed_set_hl(int hl);
void      nt_ed_clear_format(void);
void      nt_ed_set_block(int type, int style);   /* to every block in the selection */
void      nt_ed_indent(int delta);
void      nt_ed_toggle_check_block(void);
bool      nt_ed_undo(void);
bool      nt_ed_redo(void);
bool      nt_ed_can_undo(void);
bool      nt_ed_can_redo(void);
void      nt_ed_copy(bool cut);
void      nt_ed_paste(void);
bool      nt_ed_clip_full(void);
void      nt_ed_reset_history(void);
void      nt_ed_clip_free(void);
void      nt_ed_forget(void);           /* the note closed: drop it and its history */
void      nt_ed_set_focus(bool focus);  /* the keyboard is up: show the caret */

/* -------------------------------------------------------------------------- */
/* Task lists (nt_tasks.c)                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    void (*changed)(void);
    void (*keyboard)(bool show);
} nt_tasks_cbs_t;

lv_obj_t *nt_tasks_create(lv_obj_t *parent, nt_doc_t *doc, nt_meta_t *meta,
                          const nt_tasks_cbs_t *cbs);
void      nt_tasks_destroyed(void);
void      nt_tasks_forget(void);        /* the list closed */
void      nt_tasks_refresh(void);
bool      nt_tasks_back(void);          /* closes an edit in progress */
void      nt_tasks_commit(void);        /* an edit in progress, into the doc */
void      nt_tasks_sync(void);          /* its text into the doc, the edit goes on */
int32_t   nt_tasks_get_scroll(void);
void      nt_tasks_set_scroll(int32_t y);
void      nt_tasks_uncheck_all(void);
void      nt_tasks_delete_done(void);
void      nt_tasks_sort_prio(void);
