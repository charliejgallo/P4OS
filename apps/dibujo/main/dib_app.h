/*
 * DIBUJO - what the app's files share: the instance, the layout and the
 * functions one part calls in another.
 *
 *   dibujo.c       life cycle, the gallery, files, the worker (export, import)
 *   dib_editor.c   the editor: bars, the view, touch, mouse, pad, keyboard,
 *                  tools and the floating object's handles
 *   dib_panels.c   the cards over the canvas: brushes, colour, layers,
 *                  menu, text, sizes, pictures to import
 *   dib_icons.c    the tools' icons, drawn with the shape rasterizer
 *
 * Nothing is deleted from inside its own event: panels and the options bar
 * are rebuilt by the frame timer when a flag says so.
 */
#pragma once

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "dib_doc.h"
#include "dib_paint.h"
#include "dib_raster.h"

#define TAG             "dibujo"
#define APP_ID          "aos.dibujo"

#define BAR_H           88
#define GAP             8
#define FRAME_MS        16
#define AUTOSAVE_MS     20000
#define PINCH_UNDO_MS   350
#define HANDLE_R        30          /* screen px around a handle that grabs it */
#define ROT_HANDLE_D    56          /* the turning handle, out from the box    */
#define SNAP_PX         12          /* the magnet's reach, screen px           */
#define RULER_W         28
#define MAX_PROJECTS    48
#define NRECENT         10
#define NPALETTES       4
#define PAL_COLORS      16
#define TEXT_MAX        200

#define C_PANEL         lv_color_hex(0x1C1C1E)
#define C_BTN           lv_color_hex(0x2C2C2E)
#define C_SEL           lv_color_hex(0x0A84FF)
#define C_OUTSIDE       0x26262A    /* around the document, RGB888 */

typedef enum {
    T_BRUSH = 0,
    T_ERASER,
    T_SHAPE,
    T_TEXT,
    T_FILL,
    T_PICK,
    T_SELECT,
    T_HAND,
    T_COUNT
} tool_t;

typedef enum {
    P_NONE = 0,
    P_BRUSHES,
    P_COLOR,
    P_LAYERS,
    P_MENU,
    P_SHAPES,
    P_IMPORT,
} panel_t;

/* What a pointer (finger, mouse, pad) is doing on the canvas. */
typedef enum {
    G_NONE = 0,
    G_STROKE,
    G_PICK,
    G_PAN,
    G_SHAPE_NEW,                /* dragging out a new shape               */
    G_POLY_PT,                  /* placing a polygon's vertex             */
    G_SEL_NEW,                  /* dragging out a selection               */
    G_OBJ_MOVE,
    G_OBJ_CORNER,               /* scaling by a corner (grab = 0..3)      */
    G_OBJ_ROT,
    G_OBJ_END,                  /* a line's end (grab = 0, 1)             */
    G_GUIDE,                    /* a guide from a ruler                   */
    G_SWALLOW,                  /* the rest of a touch that did its job   */
} grab_t;

/* Where things go, from the root's size. */
typedef struct {
    bool    land;
    int32_t W, H;
    int32_t top_x, top_y, top_w;            /* the bar with back, undo...   */
    int32_t tool_x, tool_y, tool_w, tool_h; /* the tools: a row or a column */
    int32_t tool_sz;                        /* one tool button              */
    int32_t opt_x, opt_y, opt_w;            /* the options of the tool      */
    int32_t cv_x, cv_y, cv_w, cv_h;         /* the canvas window            */
} layout_t;

typedef struct {
    char     name[24];
    dib_px_t c[PAL_COLORS];
    int      n;
} palette_t;

typedef struct {
    char     file[32];          /* "dibujo3.dib" */
    int      w, h;
    long     mtime;
    uint16_t *prev;             /* DIB_PREVIEW^2, NULL: unreadable */
    int      pw, ph;
} project_t;

typedef struct app app_t;

/* An export or an import, run by the worker while a card says so. */
typedef enum { JOB_NONE = 0, JOB_EXPORT, JOB_IMPORT } job_t;

struct app {
    aos_app_t  *self;
    lv_obj_t   *root;
    layout_t    L;
    bool        closing;
    bool        exit_req;
    bool        exiting;
    lv_timer_t *frame;
    size_t      budget;

    /* ---- the gallery ---- */
    lv_obj_t   *gal;
    lv_obj_t   *gal_list;
    lv_obj_t   *gal_sheet;      /* new / actions over the gallery */
    project_t   proj[MAX_PROJECTS];
    int         nproj;
    int         sheet_proj;     /* the project the sheet is about */
    int         sheet_req;      /* a long press asks for the sheet of this one */
    bool        sheet_confirm;
    bool        gal_dirty;      /* rebuild the list */
    bool        sheet_close_req;
    int         size_req;       /* the size chooser picked this one, -1 = none */

    /* ---- the document ---- */
    dib_doc_t  *doc;
    char        cur_file[32];   /* "" in the gallery */
    bool        dirty;
    uint32_t    changed_ms;

    /* ---- the editor ---- */
    lv_obj_t   *ed;
    lv_obj_t   *view;           /* lv_canvas over vbuf */
    uint16_t   *vbuf;
    size_t      vbuf_px;
    int32_t    *xmap;           /* a window row's document columns */
    int         xmap_n;
    lv_obj_t   *touch;
    lv_obj_t   *wheel_pad;      /* the touch layer's invisible content */
    lv_obj_t   *btn_tool[T_COUNT];
    lv_obj_t   *img_tool[T_COUNT];
    lv_obj_t   *btn_undo, *btn_redo, *btn_back, *btn_zoom, *lbl_zoom;
    lv_obj_t   *btn_color, *sw_color, *btn_layers, *btn_menu;
    lv_obj_t   *opt;            /* the options bar */
    bool        opt_dirty;
    lv_obj_t   *lbl_size, *lbl_opac, *lbl_aux;
    lv_obj_t   *ruler_top, *ruler_left;
    uint16_t   *rbuf_top, *rbuf_left;
    lv_obj_t   *sel_box;        /* the marching rectangle (not marching) */
    lv_obj_t   *obj_line;       /* the floating object's outline */
    lv_point_precise_t obj_pts[6];
    lv_obj_t   *handle[6];      /* 4 corners, the turn, (lines: 2 ends) */
    lv_obj_t   *poly_line;
    lv_point_precise_t poly_pts[DIB_POLY_MAX + 1];
    lv_obj_t   *cursor;         /* the pad's */
    lv_obj_t   *busy;           /* the worker's card */
    lv_obj_t   *busy_lbl;

    /* view: screen = o + doc * s, inside the canvas window */
    float       vs, vox, voy;
    bool        view_full;      /* redraw the whole window */
    dib_rect_t  doc_dirty;      /* recompose and redraw this */
    bool        overlay_dirty;

    /* tools */
    tool_t      tool;
    tool_t      tool_before_pick;
    dib_brush_t brush;          /* T_BRUSH */
    float       eraser_size, eraser_opacity;
    float       brush_size[DIB_BR_COUNT], brush_opacity[DIB_BR_COUNT];
    int         shape;          /* dib_obj_kind_t, not BITMAP */
    bool        shape_fill, shape_stroke;
    float       shape_w, shape_radius;
    int         fill_tol;
    bool        sample_all;
    int         font_idx;
    dib_px_t    color, color2;  /* stroke, fill */
    bool        edit_fill;      /* the colour panel edits color2 */
    float       hue, sat, val;
    dib_px_t    recent[NRECENT];
    int         nrecent;
    palette_t   pal[NPALETTES];
    int         pal_cur;

    /* design aids */
    bool        grid;
    int         grid_step;
    bool        rulers;
    bool        snap;
    int         sym;

    /* the pointer */
    grab_t      grab;
    int         grab_idx;
    bool        ptr_down;
    float       px, py;         /* where it is, document px */
    float       sx0, sy0;       /* where it went down, screen (window) px */
    float       dx0, dy0;       /* where it went down, document px */
    uint32_t    down_ms, move_ms;
    dib_stroke_t stroke;
    bool        pinching;
    float       obj_ang0, obj_w0, obj_h0, obj_cx0, obj_cy0;
    dib_pt_t    obj_fix;        /* the corner that stays while scaling */

    /* the floating object */
    bool        has_obj;
    dib_obj_t   obj;
    bool        obj_from_cut;   /* cancelling puts the pixels back */
    bool        obj_uniform;    /* corners keep the aspect (text, pictures) */
    bool        obj_is_text;
    char        obj_text[TEXT_MAX];
    int         obj_font;
    dib_px_t    obj_text_c;

    /* polygon in the making */
    dib_pt_t    poly[DIB_POLY_MAX];
    int         npoly;

    /* selection, document px; empty when none */
    dib_rect_t  sel;
    dib_px_t   *clip;           /* copied pixels */
    int         clip_w, clip_h;

    /* panels */
    panel_t     panel;
    lv_obj_t   *pnl;
    bool        pnl_close_req;
    panel_t     pnl_open_req;
    bool        pnl_rebuild;
    lv_obj_t   *sv_canvas, *hue_canvas, *sv_mark, *hue_mark, *col_prev, *col_hex;
    uint16_t   *sv_buf, *hue_buf;
    int         sv_side;
    uint16_t   *lthumb[DIB_MAX_LAYERS];
    bool        confirm;        /* a destructive button waiting for its second tap */

    /* text entry */
    lv_obj_t   *txt_card, *txt_ta, *txt_kb;
    float       txt_x, txt_y;   /* where it goes, document px */
    bool        txt_edit;       /* editing the floating text */
    lv_obj_t   *txt_canvas;     /* hidden, renders the text */

    /* import list */
    char      (*imp_files)[96];
    int         nimp;
    int         imp_req;        /* the file picked, -1 = none */

    /* the worker */
    job_t       job;
    volatile int  job_progress;
    volatile bool job_done;
    volatile bool job_ok;
    char        job_path[160];
    uint16_t   *job_img;        /* the decoded picture */
    int         job_w, job_h;

    /* pad, keyboard, mouse */
    aos_pad_t   pad;
    aos_pad_menu_t pad_menu;
    bool        pad_menu_on;
    bool        pad_seen;
    float       cur_x, cur_y;   /* the pad's cursor, window px */
    bool        pad_b_combo;
    uint32_t    pad_ms;
    bool        kb_on;
};

/* dibujo.c */
const char *dj_dir(void);
void        dj_toast(const char *msg);
bool        dj_save(app_t *a);
void        dj_go_gallery(app_t *a);
void        dj_export_png(app_t *a);
void        dj_import(app_t *a, const char *path);
void        dj_mark_dirty(app_t *a);
void        dj_palettes_save(app_t *a);
lv_obj_t   *dj_ui_button(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t c,
                      lv_event_cb_t cb, void *ud);
lv_obj_t   *dj_ui_text_button(lv_obj_t *parent, const char *text, const lv_font_t *font, int32_t w,
                           int32_t h, lv_color_t c, lv_event_cb_t cb, void *ud);
lv_obj_t   *dj_ui_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);
lv_color_t  dj_ui_color(dib_px_t p);

/* dib_editor.c */
void dj_ed_build(app_t *a, lv_obj_t *root);
void dj_ed_enter(app_t *a);
void dj_ed_leave(app_t *a);
void dj_ed_frame(app_t *a);
void dj_ed_refresh(app_t *a);          /* bars: undo, zoom, colour, tool */
void dj_ed_set_tool(app_t *a, tool_t t);
void dj_ed_view_fit(app_t *a);
void dj_ed_view_actual(app_t *a);
void dj_ed_zoom_at(app_t *a, float k, float sx, float sy);
void dj_ed_undo(app_t *a);
void dj_ed_redo(app_t *a);
void dj_ed_changed(app_t *a, dib_rect_t r);        /* recompose r and show it */
void dj_ed_all_changed(app_t *a);
void dj_ed_obj_commit(app_t *a);
void dj_ed_obj_cancel(app_t *a);
void dj_ed_obj_show(app_t *a);                     /* re-render it after a change */
void dj_ed_obj_from_bitmap(app_t *a, dib_px_t *bmp, int w, int h, float cx, float cy,
                            bool from_cut, bool uniform);
void dj_ed_set_color(app_t *a, dib_px_t c, bool second);
void dj_ed_push_recent(app_t *a, dib_px_t c);
bool dj_ed_key(app_t *a, uint32_t key, uint8_t mods);
void dj_ed_keys_take(app_t *a, bool on);
bool dj_ed_back(app_t *a);
void dj_ed_resize_cleanup(app_t *a);

/* dib_panels.c */
void dj_panel_open(app_t *a, panel_t p);
void dj_panel_close(app_t *a);
void dj_panel_frame(app_t *a);             /* deferred opens, closes, rebuilds */
void dj_panel_refresh(app_t *a);           /* values changed under an open panel */
void dj_text_open(app_t *a, float x, float y, bool edit);
void dj_text_close(app_t *a);
void dj_text_frame(app_t *a);
bool dj_text_render(app_t *a, const char *txt, int font, dib_px_t color, dib_px_t **bmp, int *w, int *h);
void dj_import_open(app_t *a);
void dj_sizes_open(app_t *a);
void dj_sizes_chosen(app_t *a, int w, int h);
void dj_ed_rulers_changed(app_t *a);
extern const int dj_sizes[][2];
#define NSIZES 5
extern const lv_font_t *const *const dj_text_fonts[];
extern const int dj_text_font_px[];
#define NFONTS 7

/* dib_icons.c */
typedef enum {
    IC_PENCIL = 0, IC_SOFT, IC_AIR, IC_MARKER, IC_ERASER,
    IC_LINE, IC_RECT, IC_ELLIPSE, IC_POLY, IC_ARROW,
    IC_TEXT, IC_FILL, IC_PICK, IC_SELECT, IC_HAND,
    IC_UNDO, IC_REDO, IC_LAYERS, IC_COUNT
} icon_t;
const lv_image_dsc_t *dj_icon_get(icon_t i);
void dj_icons_free(void);
