/*
 * P4OS - Office: what the screens share (LVGL side).
 *
 * of.h is the formats, which build without LVGL; this is the app: the
 * theme, the drawn icons, the sheets and dialogs (from Notes), the
 * keyboard, and the screens.
 */
#pragma once

#include "of.h"
#include "lvgl.h"

/* -------------------------------------------------------------------------- */
/* Theme and fonts (of_ui.c)                                                   */
/* -------------------------------------------------------------------------- */

typedef struct {
    bool dark;
    lv_color_t bg, surface, surface2, text, dim, hair, accent, sel, danger;
    lv_color_t kb_bg, kb_key, kb_key2, kb_text;
    lv_color_t paper, ink, grid, head;      /* the document and the sheet */
} ofu_theme_t;

extern ofu_theme_t OFT;
void ofu_theme_set(bool dark);

enum { OF_SZ_M = 0, OF_SZ_S, OF_SZ_L, OF_SZ_XL, OF_SZ_XS };   /* 28 24 36 48 20 px */
#define OF_ST_BOLD 1
const lv_font_t *ofu_font(int style, int size);
/* the documents' fonts (of_fonts.c): style bit 0 bold, bit 1 italic */
const lv_font_t *ofd_font(int style, int px, bool *faux, bool *slant);
bool ofd_fonts_have_pack(void);
void ofd_fonts_free(void);

/* -------------------------------------------------------------------------- */
/* Widgets (of_ui.c)                                                           */
/* -------------------------------------------------------------------------- */

enum {
    OF_IC_BACK, OF_IC_PLUS, OF_IC_MORE, OF_IC_UNDO, OF_IC_REDO, OF_IC_SEARCH,
    OF_IC_PIN, OF_IC_TRASH, OF_IC_LIST, OF_IC_CHECKBOX, OF_IC_INDENT,
    OF_IC_OUTDENT, OF_IC_KB_HIDE, OF_IC_CLOSE, OF_IC_HANDLE, OF_IC_CHEVRON,
    OF_IC_NOTE, OF_IC_TASKS, OF_IC_HIGHLIGHT, OF_IC_TEXTCOLOR, OF_IC_AA,
    OF_IC_GRID, OF_IC_ROWS, OF_IC_CHECK, OF_IC_RESTORE, OF_IC_QR, OF_IC_TYPE,
    OF_IC_PALETTE, OF_IC_INFO, OF_IC_COPY, OF_IC_SUN, OF_IC_MOON,
    /* Office's own */
    OF_IC_SAVE, OF_IC_FOLDER, OF_IC_UP, OF_IC_SHEET, OF_IC_DOC, OF_IC_SORT,
    OF_IC_FILTER, OF_IC_CLOCK, OF_IC_ZOOM_IN, OF_IC_ZOOM_OUT, OF_IC_KEYBOARD,
};

/* lv_area_intersect is private to LVGL: the app has its own */
static inline bool ofu_clip(lv_area_t *out, const lv_area_t *a, const lv_area_t *b)
{
    out->x1 = LV_MAX(a->x1, b->x1);
    out->y1 = LV_MAX(a->y1, b->y1);
    out->x2 = LV_MIN(a->x2, b->x2);
    out->y2 = LV_MIN(a->y2, b->y2);
    return out->x1 <= out->x2 && out->y1 <= out->y2;
}

extern lv_obj_t *ofu_ui_root;           /* where sheets go: the app's root */

lv_obj_t *ofu_box(lv_obj_t *parent, int32_t w, int32_t h);   /* bare, transparent */
lv_obj_t *ofu_text(lv_obj_t *parent, const char *s, const lv_font_t *f, lv_color_t c);
lv_obj_t *ofu_icon_btn(lv_obj_t *parent, int icon, int32_t size, lv_event_cb_t cb, void *ud);
void      ofu_icon_set(lv_obj_t *btn, int icon);
void      ofu_icon_color(lv_obj_t *btn, lv_color_t c);
void      ofu_draw_icon(lv_layer_t *layer, int icon, const lv_area_t *a, lv_color_t c, int32_t stroke);
void      ofu_draw_checkbox(lv_layer_t *layer, const lv_area_t *a, bool checked, bool round, lv_color_t c, lv_color_t on);
lv_obj_t *ofu_pill(lv_obj_t *parent, const char *s, lv_event_cb_t cb, void *ud);

typedef struct {
    const char *label;
    int         icon;           /* -1 none */
    bool        danger;
    bool        checked;
} ofu_item_t;
typedef void (*ofu_pick_cb_t)(int index, void *ud);
lv_obj_t *ofu_sheet(const char *title, const ofu_item_t *items, int n, ofu_pick_cb_t cb, void *ud);
lv_obj_t *ofu_sheet_custom(const char *title, int32_t *content_w);
void      ofu_sheet_close(void);
void      ofu_sheet_forget(void);
bool      ofu_sheet_open(void);
void      ofu_confirm(const char *title, const char *text, const char *yes, bool danger,
                      void (*cb)(void *ud), void *ud);
lv_obj_t *ofu_swatches(lv_obj_t *parent, int32_t w, int count, lv_color_t (*color)(int),
                       int selected, bool first_is_none, lv_event_cb_t cb, void *ud);

/* A question with a line to type (of_shell.c): name a file, go to a cell. */
void ofu_prompt(const char *title, const char *initial, const char *ok,
                void (*cb)(const char *text, void *ud), void *ud);

/* -------------------------------------------------------------------------- */
/* Keyboard (of_kb.c)                                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    void (*insert)(const char *utf8);
    void (*backspace)(void);
    void (*enter)(void);
    void (*move)(int dx);
    bool (*caps)(void);
    /* a key of a USB or Bluetooth keyboard, before anything else: true if
     * taken (shortcuts with Ctrl, arrows up and down, Tab, Esc, Delete) */
    bool (*key)(uint32_t key, uint8_t mods);
} ofu_kb_target_t;

lv_obj_t *ofu_kb_create(lv_obj_t *parent, int32_t w, int32_t h);
void      ofu_kb_attach(const ofu_kb_target_t *t);
void      ofu_kb_refresh_caps(void);
void      ofu_kb_numeric(bool on);      /* start on the numbers' page */
void      ofu_kb_destroyed(void);
void      ofu_kb_attach_textarea(lv_obj_t *ta, void (*done)(lv_obj_t *ta));

/* HID modifier bits */
#define OF_MOD_CTRL  0x11
#define OF_MOD_SHIFT 0x22
#define OF_MOD_ALT   0x44
#define OF_MOD_GUI   0x88
#define OF_MOD_CMD   (OF_MOD_CTRL | OF_MOD_GUI)     /* Ctrl or Cmd */

/* -------------------------------------------------------------------------- */
/* The screens                                                                 */
/* -------------------------------------------------------------------------- */

/* oficina.c */
const char *of_docs_dir(void);          /* /sdcard/documentos */
void of_app_home(void);                 /* back to the files */
void of_app_open(const char *path);
enum { OF_OPEN_AUTO = 0, OF_OPEN_TEXT, OF_OPEN_VIEW };
void of_app_open_as(const char *path, int how);   /* TEXT: in the text editor; VIEW: read and shown (md, html, the tree of a json) */
bool of_app_can_text(const char *path);         /* a text format: "Editar como texto" */
bool of_app_can_view(const char *path);         /* "Ver con formato", "Ver como árbol" */
void of_recent_add(const char *path);
void of_app_toast(const char *msg);
bool of_app_landscape(void);
void of_app_back_from_doc(void);      /* the back button of a document: asks if it has changes */
void of_app_pdf_mode(const char *path, bool pages);   /* a PDF's pages drawn, or its text reflowed (kept for the next) */

/* of_sheetview.c */
bool of_sheet_open(lv_obj_t *parent, const char *path, of_book_t *b);
void of_sheet_close_view(void);
bool of_sheet_modified(void);
bool of_sheet_back(void);               /* true: handled (a panel closed, a question asked) */
void of_sheet_tick(void);
bool of_sheet_save(void);
bool of_sheet_save_recovery(void);

/* of_docview.c */
struct of_doc_s;
bool of_docv_open(lv_obj_t *parent, const char *path, struct of_doc_s *d);
void of_docv_close(void);
bool of_docv_modified(void);
bool of_docv_back(void);
bool of_docv_save(void);
bool of_docv_save_recovery(void);

/* of_slideview.c */
struct of_deck_s;
bool of_slidev_open(lv_obj_t *parent, const char *path, struct of_deck_s *dk);
void of_slidev_close(void);
bool of_slidev_back(void);                  /* true: it left the presentation */

/* of_pdfview.c: path NULL lays the open one out again; err as of_pdfr_open's, or "worker" */
bool of_pdfv_open(lv_obj_t *parent, const char *path, const char *password, char *err, size_t errlen);
void of_pdfv_close(void);

/* of_textview.c */
bool of_textv_open(lv_obj_t *parent, const char *path);
void of_textv_close(void);
bool of_textv_modified(void);
bool of_textv_back(void);
bool of_textv_save(void);
bool of_textv_save_recovery(void);
