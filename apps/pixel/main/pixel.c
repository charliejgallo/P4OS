/*
 * P4OS - Pixel Art (from AmoledOS): a drawing app for 8x8 to 64x64 grids,
 * with frames.
 *
 * Twelve documents ("lienzos"), each a stack of up to 16 frames of cells
 * painted from a 32-colour palette. Frames are duplicated and retouched to
 * make an animation, played back on the screen and exported as a looping
 * GIF; a single frame goes out as a PNG. The files land on the microSD under
 * /pixel, in the watch's format: a .pix of 8x8 or 16x16 opens on AmoledOS as
 * it is. The app notices when a file changes on the card (the portal's file
 * manager, the card in a computer) and reloads it.
 *
 * On the 5" panel the canvas is the point: 704 px across upright, 640 lying
 * down, both multiples of 64 so a cell is a whole number of pixels at every
 * size (704 = 88 x 8 = 44 x 16 = 22 x 32 = 11 x 64). Upright the canvas sits
 * between the bar (frames) and the tools, with the 32 colours as a 8x4 grid
 * underneath where the thumb reaches; lying down the canvas is on the left
 * and bar, tools and palette are a column on the right.
 *
 * How it draws, and why it is cheap:
 *
 *   - ONE canvas of 704x704 RGB565 (990 KB, PSRAM via malloc) shown 1:1.
 *     Painting a cell writes that square into the buffer and invalidates
 *     ONLY that square: LVGL blits a few hundred pixels, not the screen.
 *     Switching frames rewrites the whole buffer and invalidates it once.
 *   - The document never touches LVGL (px_file.c) and the encoders never
 *     touch the document's owner (px_export.c): both are verified on the Mac
 *     by tools/px_harness.c, byte by byte, before the board sees them.
 *   - Both screens -gallery and editor- are built once and shown/hidden, and
 *     rebuilt only when the screen turns (resize()). Nothing is destroyed
 *     from inside an event callback, which is the rule this system enforces
 *     the hard way.
 *
 * Zoom (aos_gesture.h): two fingers pinch the canvas from 1x up to cells of
 * ~176 px about the point between them and drag it around; one finger still
 * paints. The canvas stays the same window - what changes is how big a cell
 * is drawn in it and where the document sits (view_*), so painting a cell
 * still invalidates only its square. The first finger of a pinch lands a
 * moment before the second and has already painted: a stroke younger than
 * PINCH_UNDO_MS when the pinch starts is taken back. The magnifier in the
 * tools does the same with one finger: 2x about the centre, or back to fit.
 *
 * Sending a drawing to another board over ESP-NOW (the watch's "link") is
 * kept, but it only shows when aos_hal_link_start() works: on the P4 the
 * radio is the C6 behind esp_hosted and there is no ESP-NOW yet, so today
 * the option simply is not in the menu.
 *
 * Pending: AmoledOS's portal page /pixel (drawing with a mouse from the
 * browser) lives in AmoledOS's web component, which P4OS does not build.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "aos_gesture.h"
#include "aos_sys_glyphs.h"

#include "px_file.h"
#include "px_export.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define TAG             "pixel"

#define BTN_H           88                  /* AOS_UI_TAP_MIN: every control */
#define BTN_SQ          88                  /* the square bar buttons        */
#define GAP             12
#define PAL_COLS        8
#define PAL_ROWS        4
#define PAL_MIN_H       (PAL_ROWS * 56)     /* upright, what the palette needs */
#define EDGE_CLEAR      16                  /* above the home gesture's strip  */

#define TH_PX           192                 /* gallery thumbnail: 24/12/6/3 px cells */
#define TH_ROW_H        (TH_PX + 48)        /* thumbnail + its label          */

#define GRID_COLOR      0x3A3A3E            /* between cells; on black it reads, just */

#define TIMER_MS        50
#define SAVE_RETRY_MS   10000
#define SLOT_ERR        0xFF
#define AUTOSAVE_MS     3000
#define WATCH_MS        3000
#define PINCH_UNDO_MS   350                 /* a stroke this young is the pinch's first finger */
#define UNDO_MAX        16                  /* strokes, fills and clears kept */
#define EXPORT_PX       512                 /* side of the exported image     */
#define CELL_MAX_PX     176                 /* the zoom stops at cells this big */

enum { TOOL_PEN = 0, TOOL_FILL, TOOL_PICK, TOOL_COUNT };

/* Where everything goes, from the root's size (portrait or landscape). */
typedef struct {
    bool    land;
    int32_t W, H;
    int32_t cv, cv_x, cv_y;     /* the canvas window, a multiple of 64       */
    int32_t cx, cw;             /* the controls: x and width                 */
    int32_t bar_y, tool_y;
    int32_t pal_y, pal_h;
} layout_t;

typedef struct {
    aos_app_t *self;
    lv_obj_t  *root;
    lv_timer_t *timer;
    bool       closing;
    bool       exit_req;
    bool       exiting;
    layout_t   L;

    /* the document being edited, and a scratch one for thumbnails */
    px_doc_t  *doc;
    px_doc_t  *scratch;
    int        slot;            /* -1 in the gallery                         */
    int        frame;
    int        color;
    int        tool;
    bool       grid;            /* the lines between cells                   */
    bool       dirty;
    uint32_t   changed_ms;

    /* Undo: a ring of whole frames (up to 4 KB each), tagged with the frame
     * they belong to. A stroke is pushed on its FIRST change, from 'before',
     * the frame as it was when the finger landed: a tap that paints nothing
     * leaves nothing to undo. */
    uint8_t   *undo_buf;
    int8_t     undo_fr[UNDO_MAX];
    int        undo_top, undo_n;
    uint8_t   *before;
    bool       stroke;          /* a finger is down and painting             */
    bool       stroke_pushed;
    int        last_x, last_y;  /* the stroke's last cell, -1 = none         */
    uint32_t   stroke_ms;       /* when it started                           */

    /* the view: a cell is cv/size*zoom px, the document's corner at
     * view_x/y inside the canvas window (both <= 0 when zoomed) */
    float      view_zoom, view_x, view_y;
    bool       pinching;        /* from the pinch's start to every finger up */
    bool       view_dirty;
    lv_timer_t *view_timer;

    /* what the file looked like when loaded, to notice it changing */
    long       file_size;
    long       file_mtime;
    uint32_t   watch_ms;
    uint32_t   gal_sig;

    /* gallery */
    lv_obj_t  *gal;
    lv_obj_t  *slot_cv[PX_SLOTS];
    lv_obj_t  *slot_lbl[PX_SLOTS];
    uint16_t  *thumb[PX_SLOTS];
    uint8_t    slot_size[PX_SLOTS];         /* 0 = empty, SLOT_ERR = unreadable */
    uint8_t    slot_frames[PX_SLOTS];
    lv_obj_t  *sizer;                       /* the "new document" overlay   */
    int        sizer_slot;
    lv_obj_t  *gal_info;

    /* editor */
    lv_obj_t  *ed;
    lv_obj_t  *canvas;
    uint16_t  *big;
    size_t     big_px;                      /* what 'big' holds, in pixels  */
    lv_obj_t  *touch;
    lv_obj_t  *lbl_frame, *lbl_finfo;
    lv_obj_t  *btn_tool[TOOL_COUNT], *lbl_tool[TOOL_COUNT], *cap_tool[TOOL_COUNT];
    lv_obj_t  *btn_undo, *btn_zoom, *cap_zoom;
    lv_obj_t  *swatch[PX_COLORS];
    lv_obj_t  *menu;                        /* built when opened, deleted when closed */
    lv_obj_t  *mi_del_frame, *mi_speed, *mi_del_doc, *mi_grid;
    bool       menu_del_req;                /* the timer deletes it: never from its own callback */
    bool       confirm_del;
    uint32_t   save_retry_ms;               /* after a failed save, do not hammer the card */
    bool       save_failed;
    bool       playing;
    uint32_t   play_next_ms;

    /* Sending a drawing to another board (AmoledOS docs/LINK.md). The link
     * is only up while there is a partner paired in Enlace AND the radio
     * starts; the file goes as it is on the card, in chunks over the
     * reliable channel, and the other side answers with the slot it landed
     * in. */
    bool       link_up;
    char       pname[AOS_LINK_NAME_MAX + 1];
    uint8_t    partner_mac[6];
    uint8_t   *tx_buf, *rx_buf;
    uint32_t   tx_total, tx_off, tx_ms;
    uint32_t   rx_total, rx_got, rx_ms;
    bool       tx_active, rx_active;
} app_t;

/* The link's protocol is the watch's, unchanged, so a P4 and a watch can
 * still trade drawings: that is why only the sizes a watch opens travel, and
 * why a file is at most a 16x16 of 16 frames (the offset is 16 bits). */
#define PXL_PROTO       1
#define PXL_CHUNK       236                 /* 4 of header in a 242-byte reliable payload */
#define PXL_SIZE_MAX    16
#define PXL_MAX_FILE    (12 + PX_COLORS * 3 + PX_MAX_FRAMES * PXL_SIZE_MAX * PXL_SIZE_MAX + 64)
#define PXL_TX_TIMEOUT  8000
#define PXL_RX_TIMEOUT  5000

enum { PXL_START = 1, PXL_DATA = 2, PXL_DONE = 3 };

typedef struct __attribute__((packed)) {
    uint8_t  type, proto;
    uint8_t  pad[2];
    uint32_t total;
} pxl_start_t;

typedef struct __attribute__((packed)) {
    uint8_t  type, proto;
    uint16_t off;
    uint8_t  data[PXL_CHUNK];
} pxl_data_t;

typedef struct __attribute__((packed)) {
    uint8_t type, proto;
    int8_t  slot;                           /* 0-based; -1 no room, -2 unreadable */
    uint8_t pad;
} pxl_done_t;

static void go_gallery(app_t *a);
static void go_editor(app_t *a, int slot);
static void editor_refresh(app_t *a);
static void menu_close(app_t *a);
static void mi_send_cb(lv_event_t *e);

/* --------------------------------------------------------------------------
 * Paths
 *
 * With a card: /sdcard/pixel. Without one: the data directory, no
 * sub-folder (on the watch that was SPIFFS, whose names are flat).
 * -------------------------------------------------------------------------- */

static const char *px_dir(void)
{
    static char dir[96];
    static bool made;
    const char *sd = aos_hal_path_sd_root();
    if (sd) {
        snprintf(dir, sizeof(dir), "%s/pixel", sd);
        if (!made) {
            mkdir(dir, 0777);
            made = true;
        }
    } else {
        snprintf(dir, sizeof(dir), "%s", aos_hal_path_data());
    }
    return dir;
}

static void slot_path(int slot, char *out, size_t n)
{
    snprintf(out, n, "%s/lienzo%d.pix", px_dir(), slot + 1);
}

static uint32_t now_ms(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

/* --------------------------------------------------------------------------
 * Layout
 * -------------------------------------------------------------------------- */

static void layout_compute(app_t *a, lv_obj_t *root)
{
    layout_t *L = &a->L;
    L->W = lv_obj_get_width(root);
    L->H = lv_obj_get_height(root);
    L->land = L->W > L->H;
    if (!L->land) {
        /* bar, canvas, tools, palette, top to bottom */
        L->bar_y = 8;
        L->cv_y  = L->bar_y + BTN_H + 8;
        int32_t room = L->H - L->cv_y - (GAP + BTN_H + GAP) - PAL_MIN_H - 8;
        int32_t side = L->W - 16 < room ? L->W - 16 : room;
        L->cv    = side / 64 * 64;
        L->cv_x  = (L->W - L->cv) / 2;
        L->cx    = L->cv_x;
        L->cw    = L->cv;
        L->tool_y = L->cv_y + L->cv + GAP;
        L->pal_y = L->tool_y + BTN_H + GAP;
        L->pal_h = L->H - 8 - L->pal_y;
    } else {
        /* the canvas on the left, as tall as fits clear of the home strip;
         * bar, tools and palette in a column on the right */
        L->cv    = (L->H - EDGE_CLEAR) / 64 * 64;
        L->cv_y  = (L->H - EDGE_CLEAR - L->cv) / 2;
        L->cv_x  = 10;
        L->cx    = L->cv_x + L->cv + 16;
        L->cw    = L->W - L->cx - 10;
        L->bar_y = L->cv_y;
        L->tool_y = L->bar_y + BTN_H + GAP;
        L->pal_y = L->tool_y + BTN_H + GAP;
        L->pal_h = L->cv_y + L->cv - L->pal_y;
    }
}

/* --------------------------------------------------------------------------
 * Drawing into the buffers
 * -------------------------------------------------------------------------- */

static void fill_rect(uint16_t *buf, int stride, int x, int y, int w, int h, uint16_t c)
{
    for (int j = 0; j < h; j++) {
        uint16_t *p = buf + (size_t)(y + j) * stride + x;
        for (int i = 0; i < w; i++) {
            p[i] = c;
        }
    }
}

static uint16_t rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

/* lroundf is not in the firmware's table; this is all it takes. */
static int rnd(float v)
{
    return (int)floorf(v + 0.5f);
}

static float cell_px(const app_t *a)
{
    return (float)a->L.cv / (float)a->doc->size * a->view_zoom;
}

static float zoom_max(const app_t *a)
{
    float z = (float)CELL_MAX_PX / ((float)a->L.cv / (float)a->doc->size);
    return z < 1.0f ? 1.0f : z;
}

/* Where cell (x, y) lands in the window, clipped to it: false if it is out
 * of view. The edges are rounded from the view so neighbouring cells meet
 * exactly at any zoom. */
static bool cell_rect(const app_t *a, int x, int y, int *x0, int *y0, int *x1, int *y1)
{
    float cs = cell_px(a);
    int cv = a->L.cv;
    int l = rnd(a->view_x + x * cs), r = rnd(a->view_x + (x + 1) * cs);
    int t = rnd(a->view_y + y * cs), b = rnd(a->view_y + (y + 1) * cs);
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > cv) r = cv;
    if (b > cv) b = cv;
    if (l >= r || t >= b) return false;
    *x0 = l; *y0 = t; *x1 = r; *y1 = b;
    return true;
}

/* One cell of the big canvas: the colour inset by the grid line, or edge to
 * edge with the grid off. */
static void draw_cell(app_t *a, int x, int y, bool invalidate)
{
    int n = a->doc->size;
    int l, t, r, b;
    if (!cell_rect(a, x, y, &l, &t, &r, &b)) {
        return;
    }
    uint16_t c = px_rgb565(a->doc->px[a->frame][y * n + x]);
    int gl = 0, gt = 0;
    if (a->grid) {
        /* the grid line is the cell's first row and column, unless that
         * row or column has scrolled out of the window */
        float cs = cell_px(a);
        gl = (l > 0 || a->view_x + x * cs >= 0) ? 1 : 0;
        gt = (t > 0 || a->view_y + y * cs >= 0) ? 1 : 0;
    }
    if (r - l - gl > 0 && b - t - gt > 0) {
        fill_rect(a->big, a->L.cv, l + gl, t + gt, r - l - gl, b - t - gt, c);
    }
    if (invalidate) {
        lv_area_t co;
        lv_obj_get_coords(a->canvas, &co);
        lv_area_t area = { co.x1 + l, co.y1 + t, co.x1 + r - 1, co.y1 + b - 1 };
        lv_obj_invalidate_area(a->canvas, &area);
    }
}

/* The cell under a screen point, or false. */
static bool cell_at(const app_t *a, int px, int py, int *x, int *y)
{
    lv_area_t co;
    lv_obj_get_coords(a->canvas, &co);
    if (px < co.x1 || py < co.y1 || px >= co.x1 + a->L.cv || py >= co.y1 + a->L.cv) {
        return false;
    }
    float cs = cell_px(a);
    int cx = (int)floorf(((float)(px - co.x1) - a->view_x) / cs);
    int cy = (int)floorf(((float)(py - co.y1) - a->view_y) / cs);
    if (cx < 0 || cy < 0 || cx >= a->doc->size || cy >= a->doc->size) {
        return false;
    }
    *x = cx;
    *y = cy;
    return true;
}

static void draw_frame(app_t *a)
{
    int n = a->doc->size;
    uint16_t grid = rgb565(GRID_COLOR);
    a->view_dirty = false;
    /* the grid is the background: cells are painted on top, inset by 1 px */
    size_t total = (size_t)a->L.cv * a->L.cv;
    for (size_t i = 0; i < total; i++) {
        a->big[i] = grid;
    }
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            draw_cell(a, x, y, false);
        }
    }
    lv_obj_invalidate(a->canvas);
}

/* A 3x5 digit font for the frame badge, one bit per pixel, rows top down. */
static const uint8_t digits3x5[10][5] = {
    { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 },
    { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 },
    { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
};

static void draw_digit(uint16_t *buf, int stride, int x, int y, int d, int scale, uint16_t c)
{
    for (int r = 0; r < 5; r++) {
        for (int k = 0; k < 3; k++) {
            if (digits3x5[d][r] & (4 >> k)) {
                fill_rect(buf, stride, x + k * scale, y + r * scale, scale, scale, c);
            }
        }
    }
}

/* The whole thumbnail is painted by code -the plus of an empty slot, the
 * frame badge- so that a slot is ONE canvas and ONE label. On the watch
 * that was about internal RAM (forty objects for eight pictures starved the
 * card driver); here LVGL's objects live in PSRAM, but one object per slot
 * is still less to lay out and nothing to keep in step. */
static void draw_thumb(uint16_t *buf, const px_doc_t *d, int frames)
{
    int n = d->size;
    int cell = TH_PX / n;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            fill_rect(buf, TH_PX, x * cell, y * cell, cell, cell,
                      px_rgb565(d->px[0][y * n + x]));
        }
    }
    if (frames > 1) {
        /* a dark pill top right: a play triangle and the count, digits 9x15 */
        const int ds = 3;
        int nd = frames > 9 ? 2 : 1;
        int w = 10 + 8 + 6 + nd * (3 * ds + ds) + 6;
        uint16_t dark = rgb565(0x141416), white = rgb565(0xFFFFFF);
        fill_rect(buf, TH_PX, TH_PX - w - 6, 6, w, 27, dark);
        int x = TH_PX - w;
        for (int r = 0; r < 13; r++) {          /* the triangle, 7 px wide */
            int len = r < 7 ? r + 1 : 13 - r;
            fill_rect(buf, TH_PX, x, 13 + r, len, 1, white);
        }
        x += 14;
        if (nd == 2) {
            draw_digit(buf, TH_PX, x, 12, frames / 10, ds, white);
            x += 4 * ds;
        }
        draw_digit(buf, TH_PX, x, 12, frames % 10, ds, white);
    }
}

static void draw_thumb_empty(uint16_t *buf, bool error)
{
    uint16_t bg = rgb565(0x000000), line = rgb565(error ? 0x8A1E22 : 0x2C2C2E);
    fill_rect(buf, TH_PX, 0, 0, TH_PX, TH_PX, bg);
    fill_rect(buf, TH_PX, 0, 0, TH_PX, 3, line);
    fill_rect(buf, TH_PX, 0, TH_PX - 3, TH_PX, 3, line);
    fill_rect(buf, TH_PX, 0, 0, 3, TH_PX, line);
    fill_rect(buf, TH_PX, TH_PX - 3, 0, 3, TH_PX, line);
    uint16_t plus = rgb565(error ? 0xFF453A : 0x8E8E93);
    fill_rect(buf, TH_PX, TH_PX / 2 - 3, TH_PX / 2 - 24, 6, 48, plus);   /* + */
    fill_rect(buf, TH_PX, TH_PX / 2 - 24, TH_PX / 2 - 3, 48, 6, plus);
}

/* --------------------------------------------------------------------------
 * Saving and loading
 * -------------------------------------------------------------------------- */

static void note_file(app_t *a, const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        a->file_size  = (long)st.st_size;
        a->file_mtime = (long)st.st_mtime;
    } else {
        a->file_size = a->file_mtime = -1;
    }
}

static bool save_doc(app_t *a)
{
    if (a->slot < 0) {
        return true;
    }
    char path[160];
    slot_path(a->slot, path, sizeof(path));
    bool ok = px_doc_save(a->doc, path);
    if (ok) {
        a->dirty = false;
        a->save_failed = false;
        note_file(a, path);
    } else {
        /* Said once and retried in ten seconds, not every tick: on the watch
         * the first version hammered a card that had run out of DMA memory
         * twenty times a second, with a toast each time. */
        a->save_retry_ms = now_ms() + SAVE_RETRY_MS;
        if (!a->save_failed) {
            a->save_failed = true;
            aos_hal_log(TAG, "could not save %s", path);
            aos_ui_toast(_("No se pudo guardar"), 1500);
        }
    }
    return ok;
}

static void mark_dirty(app_t *a)
{
    a->dirty = true;
    a->changed_ms = now_ms();
}

/* A signature of the files, to notice them changing from the gallery. */
static uint32_t gallery_signature(void)
{
    uint32_t sig = 0;
    for (int i = 0; i < PX_SLOTS; i++) {
        char path[160];
        slot_path(i, path, sizeof(path));
        struct stat st;
        if (stat(path, &st) == 0) {
            sig = sig * 31u + (uint32_t)st.st_size * 7u + (uint32_t)st.st_mtime + (uint32_t)i;
        } else {
            sig = sig * 31u + 1u;
        }
    }
    return sig;
}

/* --------------------------------------------------------------------------
 * Undo
 * -------------------------------------------------------------------------- */

static size_t frame_bytes(const app_t *a)
{
    return (size_t)a->doc->size * a->doc->size;
}

static void undo_clear(app_t *a)
{
    a->undo_n = 0;
    a->undo_top = 0;
}

static void undo_push(app_t *a, const uint8_t *cells)
{
    memcpy(a->undo_buf + (size_t)a->undo_top * PX_CELLS, cells, frame_bytes(a));
    a->undo_fr[a->undo_top] = (int8_t)a->frame;
    a->undo_top = (a->undo_top + 1) % UNDO_MAX;
    if (a->undo_n < UNDO_MAX) {
        a->undo_n++;
    }
}

/* Takes the last change back, on whichever frame it was; false if there is
 * nothing to undo. The caller redraws. */
static bool undo_pop(app_t *a)
{
    if (a->undo_n == 0) {
        return false;
    }
    a->undo_top = (a->undo_top + UNDO_MAX - 1) % UNDO_MAX;
    a->undo_n--;
    int f = a->undo_fr[a->undo_top];
    if (f < 0 || f >= a->doc->frames) {
        return false;
    }
    memcpy(a->doc->px[f], a->undo_buf + (size_t)a->undo_top * PX_CELLS, frame_bytes(a));
    a->frame = f;
    return true;
}

/* --------------------------------------------------------------------------
 * Small builders
 * -------------------------------------------------------------------------- */

static lv_obj_t *button(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                        lv_color_t color, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    /* feedback by opacity, never by scale: a transform is a layer */
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x5A5A64), 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    }
    return btn;
}

static lv_obj_t *glyph_button(lv_obj_t *parent, const char *glyph, const lv_font_t *font,
                              int32_t x, int32_t y, int32_t w, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = button(parent, x, y, w, BTN_H, AOS_C_CARD2, cb, ud);
    lv_obj_t *l = aos_label(btn, glyph, font, AOS_C_TEXT);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(l);
    return btn;
}

/* A glyph with its name underneath: the tools. */
static lv_obj_t *tool_button(lv_obj_t *parent, const char *glyph, const lv_font_t *font,
                             const char *caption, int32_t x, int32_t y, int32_t w,
                             lv_event_cb_t cb, void *ud, lv_obj_t **glyph_lbl, lv_obj_t **cap_lbl)
{
    lv_obj_t *btn = button(parent, x, y, w, BTN_H, AOS_C_CARD2, cb, ud);
    lv_obj_t *g = aos_label(btn, glyph, font, AOS_C_TEXT);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 12);
    /* lying down the buttons are ~110 px and "Tomar color" is not: a
     * size smaller rather than cut */
    lv_point_t sz;
    lv_text_get_size(&sz, caption, aos_font_caption, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_font_t *cf = sz.x > w - 12 ? aos_font_tiny : aos_font_caption;
    lv_obj_t *c = aos_label_boxed(btn, caption, cf, AOS_C_TEXT, w - 8, 24);
    lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, -8);
    if (glyph_lbl) *glyph_lbl = g;
    if (cap_lbl) *cap_lbl = c;
    return btn;
}

static lv_obj_t *card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x3A3A3E), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);   /* swallows touches underneath */
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *menu_item(app_t *a, lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                           int32_t w, int32_t h, lv_color_t color)
{
    lv_obj_t *btn = button(parent, 0, 0, w, h, color, cb, a);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_t *l = aos_label(btn, text, aos_font_body, AOS_C_TEXT);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(l, w - 32);
    /* two lines rather than dots: "¿Seguro? Tocá de nuevo" and the German
     * strings are wider than a column */
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return btn;
}

/* --------------------------------------------------------------------------
 * Gallery
 * -------------------------------------------------------------------------- */

/* Reads the files and paints the thumbnails. No LVGL object is touched
 * here. On the watch it ran BEFORE the objects existed because the card's
 * DMA buffers came out of internal RAM; on the P4 they are PSRAM
 * (MEMORY.md), and the order stays because it costs nothing. */
static void gallery_scan(app_t *a)
{
    for (int i = 0; i < PX_SLOTS; i++) {
        char path[160];
        slot_path(i, path, sizeof(path));
        struct stat st;
        bool exists = stat(path, &st) == 0;
        if (px_doc_load(a->scratch, path)) {
            a->slot_size[i]   = a->scratch->size;
            a->slot_frames[i] = a->scratch->frames;
            draw_thumb(a->thumb[i], a->scratch, a->scratch->frames);
        } else {
            /* A file that is there and cannot be read is the card failing,
             * not an empty slot: say so instead of offering to overwrite. */
            a->slot_size[i] = exists ? SLOT_ERR : 0;
            a->slot_frames[i] = 0;
            draw_thumb_empty(a->thumb[i], exists);
            if (exists) {
                aos_hal_log(TAG, "cannot read %s", path);
            }
        }
    }
    a->gal_sig = gallery_signature();
}

static void gallery_labels(app_t *a)
{
    int used = 0;
    for (int i = 0; i < PX_SLOTS; i++) {
        if (a->slot_size[i] == SLOT_ERR) {
            lv_label_set_text_fmt(a->slot_lbl[i], "%d · %s", i + 1, _("no se lee"));
        } else if (a->slot_size[i]) {
            if (a->slot_frames[i] > 1) {
                lv_label_set_text_fmt(a->slot_lbl[i], "%d · %d×%d · %d %s", i + 1,
                                      a->slot_size[i], a->slot_size[i], a->slot_frames[i],
                                      _("cuadros"));
            } else {
                lv_label_set_text_fmt(a->slot_lbl[i], "%d · %d×%d", i + 1,
                                      a->slot_size[i], a->slot_size[i]);
            }
            used++;
        } else {
            lv_label_set_text_fmt(a->slot_lbl[i], "%d · %s", i + 1, _("vacío"));
        }
        lv_obj_invalidate(a->slot_cv[i]);
    }
    if (aos_hal_path_sd_root()) {
        lv_label_set_text_fmt(a->gal_info, "%d/%d · SD /pixel", used, PX_SLOTS);
    } else {
        lv_label_set_text_fmt(a->gal_info, "%d/%d · %s", used, PX_SLOTS,
                              _("sin tarjeta: memoria interna"));
    }
}

static void gallery_refresh(app_t *a)
{
    gallery_scan(a);
    gallery_labels(a);
}

static void sizer_show(app_t *a, int slot)
{
    a->sizer_slot = slot;
    lv_obj_remove_flag(a->sizer, LV_OBJ_FLAG_HIDDEN);
}

static void sizer_pick_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int size = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (a->closing) {
        return;
    }
    lv_obj_add_flag(a->sizer, LV_OBJ_FLAG_HIDDEN);
    if (size == 0) {
        return;                                 /* cancel */
    }
    px_doc_init(a->doc, size);
    a->slot = a->sizer_slot;
    mark_dirty(a);
    save_doc(a);                                /* the file exists from now on */
    go_editor(a, a->slot);
}

static void slot_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int slot = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (a->closing) {
        return;
    }
    if (a->slot_size[slot] == SLOT_ERR) {
        aos_ui_toast(_("No se pudo leer la tarjeta"), 1500);
    } else if (a->slot_size[slot] == 0) {
        sizer_show(a, slot);
    } else {
        go_editor(a, slot);
    }
}

static void gal_back_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->exit_req = true;                         /* applied by the timer */
}

static void build_gallery(app_t *a, lv_obj_t *root)
{
    const layout_t *L = &a->L;
    a->gal = lv_obj_create(root);
    lv_obj_remove_style_all(a->gal);
    lv_obj_set_size(a->gal, L->W, L->H);
    lv_obj_set_pos(a->gal, 0, 0);
    lv_obj_remove_flag(a->gal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->gal, LV_OBJ_FLAG_CLICKABLE);

    glyph_button(a->gal, LV_SYMBOL_LEFT, aos_font_body, 16, 8, BTN_SQ, gal_back_cb, a);
    lv_obj_t *title = aos_label(a->gal, "Pixel Art", aos_font_title, AOS_C_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8 + (BTN_H - 44) / 2);

    /* 3 x 4 upright, 6 x 2 lying down: the twelve always fit, no scrolling */
    int cols = L->land ? 6 : 3;
    int rows = (PX_SLOTS + cols - 1) / cols;
    int32_t gap = (L->W - cols * TH_PX) / (cols + 1);
    int32_t y0 = 8 + BTN_H + 20;
    for (int i = 0; i < PX_SLOTS; i++) {
        int col = i % cols, row = i / cols;
        int32_t x = gap + col * (TH_PX + gap);
        int32_t y = y0 + row * TH_ROW_H;

        /* The canvas is the slot: clickable itself, the plus and the badge
         * painted inside its buffer. */
        lv_obj_t *cv = lv_canvas_create(a->gal);
        lv_canvas_set_buffer(cv, a->thumb[i], TH_PX, TH_PX, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(cv, TH_PX, TH_PX);
        lv_obj_set_pos(cv, x, y);
        lv_obj_set_style_radius(cv, 0, 0);
        lv_image_set_antialias(cv, false);
        lv_obj_add_flag(cv, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(cv, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_opa(cv, LV_OPA_70, LV_STATE_PRESSED);
        /* a rim, or an all-black drawing would be invisible on the black */
        lv_obj_set_style_outline_width(cv, 2, 0);
        lv_obj_set_style_outline_color(cv, lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_user_data(cv, (void *)(intptr_t)i);
        lv_obj_add_event_cb(cv, slot_cb, LV_EVENT_CLICKED, a);
        a->slot_cv[i] = cv;

        lv_obj_t *lbl = aos_label_boxed(a->gal, "", aos_font_caption, AOS_C_DIM, TH_PX + gap, 26);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(lbl, x - gap / 2, y + TH_PX + 8);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        a->slot_lbl[i] = lbl;
    }

    /* A fixed box that wraps: the German hint is long. */
    lv_obj_t *hint = aos_label(a->gal, _("Tocá un lienzo para editarlo"), aos_font_small, AOS_C_DIM);
    lv_obj_set_width(hint, L->W - 48);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, 24, y0 + rows * TH_ROW_H + (L->land ? -4 : 16));
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    a->gal_info = aos_label_boxed(a->gal, "", aos_font_caption, AOS_C_DIM, L->W, 26);
    lv_obj_set_pos(a->gal_info, 0, L->H - 34);
    lv_obj_remove_flag(a->gal_info, LV_OBJ_FLAG_CLICKABLE);

    /* the "new document" overlay: which size. The two big ones are new on
     * the P4; a watch cannot open them. */
    int32_t sw = L->W - 48 < 600 ? L->W - 48 : 600;
    int32_t bw = (sw - 3 * 24) / 2, bh = 120;
    int32_t sh = 24 + 44 + 24 + 2 * bh + 20 + 24 + BTN_H + 24;
    a->sizer = card(root, (L->W - sw) / 2, (L->H - sh) / 2, sw, sh);
    lv_obj_add_flag(a->sizer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *t = aos_label(a->sizer, _("Nuevo lienzo"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);
    static const int sizes[4] = { 8, 16, 32, 64 };
    static const uint32_t colors[4] = { 0x0A84FF, 0xBF5AF2, 0xD93A6A, 0x1F9E8A };
    for (int i = 0; i < 4; i++) {
        char txt[32];
        snprintf(txt, sizeof(txt), "%d × %d", sizes[i], sizes[i]);
        lv_obj_t *b = menu_item(a, a->sizer, txt, sizer_pick_cb, bw, bh, lv_color_hex(colors[i]));
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), aos_font_title, 0);
        lv_obj_set_user_data(b, (void *)(intptr_t)sizes[i]);
        lv_obj_set_pos(b, 24 + (i % 2) * (bw + 24), 24 + 44 + 24 + (i / 2) * (bh + 20));
    }
    lv_obj_t *bc = menu_item(a, a->sizer, _("Cancelar"), sizer_pick_cb, sw - 48, BTN_H, AOS_C_CARD2);
    lv_obj_set_user_data(bc, (void *)(intptr_t)0);
    lv_obj_align(bc, LV_ALIGN_BOTTOM_MID, 0, -24);
}

/* --------------------------------------------------------------------------
 * Editor: painting
 * -------------------------------------------------------------------------- */

/* The palette is 32 objects on the P4: LVGL's objects live in PSRAM here
 * (aos_lvmem.c), so the watch's trick of one canvas with the swatches
 * painted in -done to spare internal RAM- buys nothing, and an object per
 * swatch gets its tap and its border from LVGL. */
static void palette_refresh(app_t *a)
{
    /* the chosen one: a black rim inside a white ring, which reads on the
     * white and the black swatches alike */
    for (int i = 0; i < PX_COLORS; i++) {
        bool on = i == a->color;
        lv_obj_set_style_border_width(a->swatch[i], on ? 4 : 1, 0);
        lv_obj_set_style_border_color(a->swatch[i], lv_color_hex(on ? 0x000000 : 0x5A5A64), 0);
        lv_obj_set_style_outline_width(a->swatch[i], on ? 4 : 0, 0);
    }
}

static void set_color(app_t *a, int idx)
{
    a->color = idx;
    palette_refresh(a);
    editor_refresh(a);
}

static void swatch_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (idx >= 0 && idx < PX_COLORS) {
        if (a->tool == TOOL_PICK) {
            a->tool = TOOL_PEN;         /* choosing a colour is picking one */
        }
        set_color(a, idx);
    }
}

static void play_stop(app_t *a)
{
    if (a->playing) {
        a->playing = false;
        editor_refresh(a);
    }
}

/* One cell of a stroke. The first cell that actually changes pushes the
 * frame as it was when the finger landed. */
static void paint_cell(app_t *a, int x, int y)
{
    uint8_t *px = a->doc->px[a->frame];
    int idx = y * a->doc->size + x;
    if (px[idx] == a->color) {
        return;
    }
    if (!a->stroke_pushed) {
        undo_push(a, a->before);
        a->stroke_pushed = true;
    }
    px[idx] = (uint8_t)a->color;
    draw_cell(a, x, y, true);
    mark_dirty(a);
}

/* The touch reports ~73 times a second, and a fast finger over a 64x64
 * canvas crosses several 11 px cells between two reports: the stroke is
 * joined with a line of cells, or it would come out dotted. */
static void paint_line(app_t *a, int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        paint_cell(a, x0, y0);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void touch_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (a->stroke) {
            editor_refresh(a);          /* the undo button may have lit up */
        }
        a->stroke = false;
        a->last_x = a->last_y = -1;
        return;
    }
    if (a->playing) {
        if (code == LV_EVENT_PRESSED) {
            play_stop(a);               /* a tap anywhere stops the preview */
        }
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (!indev || a->pinching) {
        return;                         /* two fingers are zooming, not painting */
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int x, y;
    bool in = cell_at(a, p.x, p.y, &x, &y);

    if (code == LV_EVENT_PRESSED) {
        a->last_x = a->last_y = -1;
        if (!in) {
            return;
        }
        uint8_t *px = a->doc->px[a->frame];
        switch (a->tool) {
        case TOOL_PICK:
            a->tool = TOOL_PEN;         /* one pick, then back to painting */
            set_color(a, px[y * a->doc->size + x]);
            return;
        case TOOL_FILL:
            if (px[y * a->doc->size + x] != a->color) {
                undo_push(a, px);
                px_doc_fill(a->doc, a->frame, x, y, (uint8_t)a->color);
                draw_frame(a);
                mark_dirty(a);
                editor_refresh(a);
            }
            return;
        default:
            memcpy(a->before, px, frame_bytes(a));
            a->stroke = true;
            a->stroke_pushed = false;
            a->stroke_ms = now_ms();
            break;
        }
    }
    if (!a->stroke) {
        return;
    }
    if (!in) {
        a->last_x = a->last_y = -1;     /* left the canvas: re-entering starts afresh */
        return;
    }
    if (x == a->last_x && y == a->last_y) {
        return;
    }
    if (a->last_x < 0) {
        paint_cell(a, x, y);
    } else {
        paint_line(a, a->last_x, a->last_y, x, y);
    }
    a->last_x = x;
    a->last_y = y;
}

/* --------------------------------------------------------------------------
 * Editor: zoom (two fingers, or the magnifier)
 * -------------------------------------------------------------------------- */

static void view_clamp(app_t *a)
{
    float zmax = zoom_max(a);
    if (a->view_zoom < 1.0f) a->view_zoom = 1.0f;
    if (a->view_zoom > zmax) a->view_zoom = zmax;
    float cv = (float)a->L.cv;
    float span = cv * a->view_zoom;
    if (a->view_x > 0) a->view_x = 0;
    if (a->view_y > 0) a->view_y = 0;
    if (a->view_x < cv - span) a->view_x = cv - span;
    if (a->view_y < cv - span) a->view_y = cv - span;
}

static void view_reset(app_t *a)
{
    a->view_zoom = 1.0f;
    a->view_x = a->view_y = 0.0f;
    a->pinching = false;
}

/* Zooms by k about a point of the canvas window. */
static void view_zoom_at(app_t *a, float k, float cx, float cy)
{
    float nz = a->view_zoom * k;
    float zmax = zoom_max(a);
    if (nz < 1.0f) nz = 1.0f;
    if (nz > zmax) nz = zmax;
    k = nz / a->view_zoom;
    a->view_x = cx - (cx - a->view_x) * k;
    a->view_y = cy - (cy - a->view_y) * k;
    a->view_zoom = nz;
    view_clamp(a);
    a->view_dirty = true;
}

/* The redraw of a moving view, once per LVGL frame however many pinch
 * events came in it (the touch sends ~73 a second). */
static void view_timer_cb(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    if (a->view_dirty && a->doc && a->ed && !lv_obj_has_flag(a->ed, LV_OBJ_FLAG_HIDDEN)) {
        draw_frame(a);
    }
}

static void gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    app_t *a = (app_t *)user;
    if (a->closing || a->playing) {
        return;
    }
    lv_area_t co;
    lv_obj_get_coords(a->canvas, &co);
    switch (ev->type) {
    case AOS_GESTURE_PINCH_BEGIN:
        a->pinching = true;
        if (a->stroke && a->stroke_pushed && now_ms() - a->stroke_ms < PINCH_UNDO_MS) {
            /* the first finger of the pinch had started painting */
            undo_pop(a);
            a->view_dirty = true;
        }
        a->stroke = false;
        a->last_x = a->last_y = -1;
        break;
    case AOS_GESTURE_PINCH:
        view_zoom_at(a, ev->scale, ev->x - co.x1, ev->y - co.y1);
        a->view_x += ev->dx;
        a->view_y += ev->dy;
        view_clamp(a);
        break;
    case AOS_GESTURE_PINCH_END:
        a->pinching = false;
        if (a->view_zoom < 1.05f) {
            view_reset(a);              /* close enough: back to the whole canvas */
            a->view_dirty = true;
        }
        editor_refresh(a);
        break;
    default:
        break;
    }
}

static void zoom_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    play_stop(a);
    if (a->view_zoom > 1.01f) {
        view_reset(a);
        a->view_dirty = true;
    } else {
        float c = (float)a->L.cv / 2.0f;
        view_zoom_at(a, 2.0f, c, c);
    }
    editor_refresh(a);
}

/* --------------------------------------------------------------------------
 * Editor: frames, tools and the bar
 * -------------------------------------------------------------------------- */

static void show_frame(app_t *a, int frame)
{
    if (frame < 0) frame = a->doc->frames - 1;
    if (frame >= a->doc->frames) frame = 0;
    a->frame = frame;
    draw_frame(a);
    editor_refresh(a);
}

static void editor_refresh(app_t *a)
{
    if (a->slot < 0 || !a->lbl_frame) {
        return;
    }
    if (a->playing) {
        lv_label_set_text_fmt(a->lbl_frame, LV_SYMBOL_PLAY "  %d/%d", a->frame + 1, a->doc->frames);
    } else {
        lv_label_set_text_fmt(a->lbl_frame, "%d/%d", a->frame + 1, a->doc->frames);
    }
    lv_label_set_text_fmt(a->lbl_finfo, "%d×%d · %d ms", a->doc->size, a->doc->size, a->doc->delay_ms);

    /* the chosen tool wears the current colour; its glyph flips to black on
     * light colours so it never vanishes, and black gets a rim */
    const uint8_t *c = px_palette[a->color];
    int luma = (c[0] * 3 + c[1] * 6 + c[2]) / 10;
    lv_color_t ink = luma > 140 ? lv_color_hex(0x000000) : AOS_C_TEXT;
    for (int i = 0; i < TOOL_COUNT; i++) {
        bool on = i == a->tool;
        lv_obj_set_style_bg_color(a->btn_tool[i], on ? lv_color_make(c[0], c[1], c[2]) : AOS_C_CARD2, 0);
        lv_obj_set_style_border_width(a->btn_tool[i], on && luma < 40 ? 3 : 0, 0);
        lv_obj_set_style_text_color(a->lbl_tool[i], on ? ink : AOS_C_TEXT, 0);
        lv_obj_set_style_text_color(a->cap_tool[i], on ? ink : AOS_C_TEXT, 0);
    }
    /* dimmed through the text, not the object's opa: that would be a layer */
    lv_obj_set_style_text_opa(a->btn_undo, a->undo_n > 0 ? LV_OPA_COVER : LV_OPA_40, 0);

    if (a->view_zoom > 1.01f) {
        int z = rnd(a->view_zoom * 10.0f);
        lv_label_set_text_fmt(a->cap_zoom, "x%d.%d", z / 10, z % 10);
        lv_obj_set_style_bg_color(a->btn_zoom, lv_color_hex(0x1E3A5F), 0);
    } else {
        lv_label_set_text(a->cap_zoom, _("Zoom"));
        lv_obj_set_style_bg_color(a->btn_zoom, AOS_C_CARD2, 0);
    }
}

static void ed_back_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    play_stop(a);
    menu_close(a);
    go_gallery(a);
}

static void prev_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (!a->closing) {
        play_stop(a);
        show_frame(a, a->frame - 1);
    }
}

static void next_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (!a->closing) {
        play_stop(a);
        show_frame(a, a->frame + 1);
    }
}

static void play_toggle(app_t *a)
{
    if (a->playing) {
        play_stop(a);
        return;
    }
    if (a->doc->frames < 2) {
        aos_ui_toast(_("Hace falta más de un cuadro"), 1200);
        return;
    }
    menu_close(a);
    a->playing = true;
    a->play_next_ms = now_ms() + a->doc->delay_ms;
    editor_refresh(a);
}

static void frame_lbl_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (!a->closing) {
        play_toggle(a);
    }
}

static void tool_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    play_stop(a);
    a->tool = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    editor_refresh(a);
}

static void undo_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    play_stop(a);
    if (undo_pop(a)) {
        mark_dirty(a);
        show_frame(a, a->frame);
    }
}

/* --------------------------------------------------------------------------
 * Editor: the menu
 * -------------------------------------------------------------------------- */

static void menu_refresh(app_t *a)
{
    if (!a->menu) {
        return;
    }
    lv_obj_t *l;
    l = lv_obj_get_child(a->mi_speed, 0);
    lv_label_set_text_fmt(l, "%s: %d ms", _("Velocidad"), a->doc->delay_ms);
    l = lv_obj_get_child(a->mi_grid, 0);
    lv_label_set_text_fmt(l, "%s: %s", _("Cuadrícula"), a->grid ? _("sí") : _("no"));
    l = lv_obj_get_child(a->mi_del_doc, 0);
    lv_label_set_text(l, a->confirm_del ? _("¿Seguro? Tocá de nuevo") : _("Borrar lienzo"));
    lv_obj_set_style_bg_opa(a->mi_del_frame, a->doc->frames > 1 ? LV_OPA_COVER : LV_OPA_30, 0);
}

static bool menu_open(const app_t *a)
{
    return a->menu != NULL && !a->menu_del_req;
}

/* Hides it now and lets the timer delete it: the close nearly always comes
 * from a click on one of its own items, and an object must not be deleted
 * while it is dispatching an event. */
static void menu_close(app_t *a)
{
    a->confirm_del = false;
    if (a->menu) {
        lv_obj_add_flag(a->menu, LV_OBJ_FLAG_HIDDEN);
        a->menu_del_req = true;
    }
}

static void build_menu(app_t *a);

static void menu_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    play_stop(a);
    if (menu_open(a)) {
        menu_close(a);
        return;
    }
    if (a->menu) {
        /* one closed by the previous tap and not yet collected */
        lv_obj_delete(a->menu);
        a->menu = NULL;
        a->menu_del_req = false;
    }
    build_menu(a);
    a->confirm_del = false;
    menu_refresh(a);
    lv_obj_remove_flag(a->menu, LV_OBJ_FLAG_HIDDEN);
}

static void mi_dup_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    int pos = px_doc_frame_dup(a->doc, a->frame);
    if (pos < 0) {
        aos_ui_toast(_("Máximo 16 cuadros"), 1200);
        return;
    }
    undo_clear(a);                  /* the frames moved under the undo's tags */
    mark_dirty(a);
    menu_close(a);
    show_frame(a, pos);
}

static void mi_blank_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    int pos = px_doc_frame_blank(a->doc, a->frame);
    if (pos < 0) {
        aos_ui_toast(_("Máximo 16 cuadros"), 1200);
        return;
    }
    undo_clear(a);
    mark_dirty(a);
    menu_close(a);
    show_frame(a, pos);
}

static void mi_del_frame_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    if (!px_doc_frame_delete(a->doc, a->frame)) {
        return;
    }
    undo_clear(a);
    mark_dirty(a);
    menu_close(a);
    show_frame(a, a->frame >= a->doc->frames ? a->doc->frames - 1 : a->frame);
}

static void mi_clear_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    undo_push(a, a->doc->px[a->frame]);
    memset(a->doc->px[a->frame], 0, PX_CELLS);
    mark_dirty(a);
    menu_close(a);
    show_frame(a, a->frame);
}

static void mi_play_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (!a->closing) {
        play_toggle(a);
    }
}

static void mi_speed_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    static const uint16_t steps[] = { 80, 120, 200, 300, 500, 1000 };
    int i = 0;
    while (i < (int)(sizeof(steps) / sizeof(steps[0])) && steps[i] <= a->doc->delay_ms) {
        i++;
    }
    a->doc->delay_ms = steps[i % (sizeof(steps) / sizeof(steps[0]))];
    mark_dirty(a);
    menu_refresh(a);
    editor_refresh(a);
}

#define KEY_GRID    "px_grid"

static void mi_grid_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    a->grid = !a->grid;
    aos_hal_pref_set_i32(KEY_GRID, a->grid ? 1 : 0);
    menu_refresh(a);
    draw_frame(a);
}

static void export_done(app_t *a, bool ok, const char *path)
{
    (void)a;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (ok) {
        aos_hal_log(TAG, "exported %s", path);
        char msg[96];
        snprintf(msg, sizeof(msg), "%s %s", _("Exportado:"), name);
        aos_ui_toast(msg, 1800);
    } else {
        aos_hal_log(TAG, "export FAILED %s", path);
        aos_ui_toast(_("No se pudo exportar"), 1500);
    }
}

/* 512 px on a side (an 8x8 stops at 256: the encoders scale up to 32x). */
static int export_scale(const app_t *a)
{
    int s = EXPORT_PX / a->doc->size;
    return s > 32 ? 32 : s;
}

static void mi_png_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/lienzo%d-%d.png", px_dir(), a->slot + 1, a->frame + 1);
    menu_close(a);
    export_done(a, px_export_png(a->doc, a->frame, export_scale(a), path), path);
}

static void mi_gif_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/lienzo%d.gif", px_dir(), a->slot + 1);
    menu_close(a);
    export_done(a, px_export_gif(a->doc, export_scale(a), path), path);
}

static void mi_del_doc_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing) {
        return;
    }
    if (!a->confirm_del) {
        a->confirm_del = true;
        menu_refresh(a);
        return;
    }
    char path[160];
    slot_path(a->slot, path, sizeof(path));
    remove(path);
    a->dirty = false;               /* nothing to save on the way out */
    menu_close(a);
    go_gallery(a);
}

/* A card over the canvas, two columns of items; it exists only while it is
 * on screen. The ones used every minute go first and the destructive one is
 * last, behind a second tap. */
static void build_menu(app_t *a)
{
    const layout_t *L = &a->L;
    a->menu = card(a->ed, L->cv_x, L->cv_y, L->cv, L->cv);
    a->menu_del_req = false;
    lv_obj_add_flag(a->menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(a->menu, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(a->menu, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(a->menu, 16, 0);
    lv_obj_set_style_pad_row(a->menu, 12, 0);
    lv_obj_set_style_pad_column(a->menu, 12, 0);

    /* two columns: the padding, the 1 px border each side and the gap */
    const int32_t w = (L->cv - 2 * 16 - 2 - 12) / 2, h = 84;
    const lv_color_t blue = lv_color_hex(0x0A5A9E);
    menu_item(a, a->menu, _("Reproducir"),       mi_play_cb,      w, h, AOS_C_CARD2);
    menu_item(a, a->menu, _("Duplicar cuadro"),  mi_dup_cb,       w, h, AOS_C_CARD2);
    menu_item(a, a->menu, _("Cuadro en blanco"), mi_blank_cb,     w, h, AOS_C_CARD2);
    a->mi_speed     = menu_item(a, a->menu, "",  mi_speed_cb,     w, h, AOS_C_CARD2);
    menu_item(a, a->menu, _("Exportar GIF"),     mi_gif_cb,       w, h, blue);
    menu_item(a, a->menu, _("Exportar PNG"),     mi_png_cb,       w, h, blue);
    if (a->link_up) {
        char t[64];
        lv_snprintf(t, sizeof t, _("Enviar a %s"), a->pname);
        lv_obj_t *s = menu_item(a, a->menu, t,   mi_send_cb,      w, h, blue);
        if (a->doc->size > PXL_SIZE_MAX) {
            lv_obj_set_style_bg_opa(s, LV_OPA_30, 0);
        }
    }
    a->mi_grid      = menu_item(a, a->menu, "",  mi_grid_cb,      w, h, AOS_C_CARD2);
    a->mi_del_frame = menu_item(a, a->menu, _("Borrar cuadro"),  mi_del_frame_cb, w, h, AOS_C_CARD2);
    menu_item(a, a->menu, _("Limpiar cuadro"),   mi_clear_cb,     w, h, AOS_C_CARD2);
    a->mi_del_doc   = menu_item(a, a->menu, _("Borrar lienzo"),  mi_del_doc_cb, w, h, lv_color_hex(0x8A1E22));
}

static void build_editor(app_t *a, lv_obj_t *root)
{
    const layout_t *L = &a->L;
    a->ed = lv_obj_create(root);
    lv_obj_remove_style_all(a->ed);
    lv_obj_set_size(a->ed, L->W, L->H);
    lv_obj_set_pos(a->ed, 0, 0);
    lv_obj_remove_flag(a->ed, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->ed, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->ed, LV_OBJ_FLAG_HIDDEN);

    /* the bar: back, previous, frame (tap = play), next, menu */
    int32_t x = L->cx, y = L->bar_y;
    int32_t fw = L->cw - 4 * BTN_SQ - 4 * GAP;
    glyph_button(a->ed, LV_SYMBOL_LEFT, aos_font_body, x, y, BTN_SQ, ed_back_cb, a);
    glyph_button(a->ed, LV_SYMBOL_PREV, aos_font_body, x + BTN_SQ + GAP, y, BTN_SQ, prev_cb, a);
    lv_obj_t *fb = button(a->ed, x + 2 * (BTN_SQ + GAP), y, fw, BTN_H, lv_color_hex(0x1E3A5F),
                          frame_lbl_cb, a);
    a->lbl_frame = aos_label(fb, "1/1", aos_font_body, AOS_C_TEXT);
    lv_obj_remove_flag(a->lbl_frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(a->lbl_frame, LV_ALIGN_TOP_MID, 0, 12);
    a->lbl_finfo = aos_label_boxed(fb, "", aos_font_caption, AOS_C_DIM, fw - 8, 24);
    lv_label_set_long_mode(a->lbl_finfo, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_remove_flag(a->lbl_finfo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(a->lbl_finfo, LV_ALIGN_BOTTOM_MID, 0, -10);
    glyph_button(a->ed, LV_SYMBOL_NEXT, aos_font_body, x + 2 * (BTN_SQ + GAP) + fw + GAP, y,
                 BTN_SQ, next_cb, a);
    glyph_button(a->ed, LV_SYMBOL_LIST, aos_font_body, x + L->cw - BTN_SQ, y, BTN_SQ, menu_cb, a);

    a->canvas = lv_canvas_create(a->ed);
    lv_canvas_set_buffer(a->canvas, a->big, L->cv, L->cv, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(a->canvas, L->cv, L->cv);
    lv_obj_set_pos(a->canvas, L->cv_x, L->cv_y);
    /* the theme's radius would clip the canvas through a mask, in layers */
    lv_obj_set_style_radius(a->canvas, 0, 0);
    lv_image_set_antialias(a->canvas, false);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_SCROLLABLE);

    /* the touch layer over the canvas, which is the only thing that paints */
    a->touch = lv_obj_create(a->ed);
    lv_obj_remove_style_all(a->touch);
    lv_obj_set_size(a->touch, L->cv, L->cv);
    lv_obj_set_pos(a->touch, L->cv_x, L->cv_y);
    lv_obj_add_flag(a->touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_RELEASED, a);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESS_LOST, a);
    aos_gesture_attach(a->touch, 0, gesture_cb, a);

    /* the tools: pencil, fill, pick, undo, zoom */
    int32_t tw = (L->cw - 4 * GAP) / 5;
    static const char *const names[TOOL_COUNT] = { N_("Lápiz"), N_("Rellenar"), N_("Tomar color") };
    for (int i = 0; i < TOOL_COUNT; i++) {
        const char *glyph = i == TOOL_PEN ? AOS_SYM_PENCIL : i == TOOL_FILL ? LV_SYMBOL_TINT : LV_SYMBOL_EYE_OPEN;
        const lv_font_t *font = i == TOOL_PEN ? &aos_sym_28 : aos_font_body;
        a->btn_tool[i] = tool_button(a->ed, glyph, font, _(names[i]), L->cx + i * (tw + GAP),
                                     L->tool_y, tw, tool_cb, a, &a->lbl_tool[i], &a->cap_tool[i]);
        lv_obj_set_user_data(a->btn_tool[i], (void *)(intptr_t)i);
    }
    a->btn_undo = tool_button(a->ed, AOS_SYM_RESTART, &aos_sym_28, _("Deshacer"),
                              L->cx + 3 * (tw + GAP), L->tool_y, tw, undo_cb, a, NULL, NULL);
    a->btn_zoom = tool_button(a->ed, AOS_SYM_MAGNIFY, &aos_sym_28, _("Zoom"),
                              L->cx + L->cw - tw, L->tool_y, tw, zoom_cb, a, NULL, &a->cap_zoom);

    /* the palette: 8 x 4 swatches filling what is left */
    int32_t pw = L->cw / PAL_COLS, ph = L->pal_h / PAL_ROWS;
    int32_t px0 = L->cx + (L->cw - pw * PAL_COLS) / 2;
    for (int i = 0; i < PX_COLORS; i++) {
        const uint8_t *c = px_palette[i];
        lv_obj_t *s = lv_obj_create(a->ed);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, pw - 10, ph - 10);
        lv_obj_set_pos(s, px0 + (i % PAL_COLS) * pw + 5, L->pal_y + (i / PAL_COLS) * ph + 5);
        lv_obj_set_style_radius(s, 14, 0);
        lv_obj_set_style_bg_color(s, lv_color_make(c[0], c[1], c[2]), 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_border_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_color(s, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_outline_pad(s, 0, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(s, (void *)(intptr_t)i);
        lv_obj_add_event_cb(s, swatch_cb, LV_EVENT_CLICKED, a);
        a->swatch[i] = s;
    }
    palette_refresh(a);
}

/* The canvas buffer for this layout: kept when big enough (turning to
 * landscape makes it smaller), grown otherwise. */
static bool canvas_alloc(app_t *a)
{
    size_t need = (size_t)a->L.cv * a->L.cv;
    if (a->big && a->big_px >= need) {
        return true;
    }
    free(a->big);
    a->big = malloc(need * sizeof(uint16_t));
    a->big_px = a->big ? need : 0;
    return a->big != NULL;
}

/* --------------------------------------------------------------------------
 * Navigation between the two screens
 * -------------------------------------------------------------------------- */

static void go_gallery(app_t *a)
{
    if (a->dirty) {
        save_doc(a);
    }
    a->slot = -1;
    a->playing = false;
    lv_obj_add_flag(a->ed, LV_OBJ_FLAG_HIDDEN);
    gallery_refresh(a);
    lv_obj_remove_flag(a->gal, LV_OBJ_FLAG_HIDDEN);
}

/* Shows the editor on the document already in memory. */
static void editor_enter(app_t *a)
{
    lv_obj_add_flag(a->gal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a->sizer, LV_OBJ_FLAG_HIDDEN);
    menu_close(a);
    lv_obj_remove_flag(a->ed, LV_OBJ_FLAG_HIDDEN);
    palette_refresh(a);
    show_frame(a, a->frame);
}

static void go_editor(app_t *a, int slot)
{
    char path[160];
    slot_path(slot, path, sizeof(path));
    if (!px_doc_load(a->doc, path)) {
        aos_hal_log(TAG, "cannot open %s", path);
        aos_ui_toast(_("No se pudo abrir"), 1500);
        return;
    }
    note_file(a, path);
    a->slot = slot;
    a->frame = 0;
    a->dirty = false;
    undo_clear(a);
    a->playing = false;
    a->tool = TOOL_PEN;
    view_reset(a);
    a->watch_ms = now_ms();
    editor_enter(a);
    aos_hal_log(TAG, "opened %s: %dx%d, %d frames", path, a->doc->size, a->doc->size, a->doc->frames);
}

/* --------------------------------------------------------------------------
 * The other board
 * -------------------------------------------------------------------------- */

static bool partner_in_pixel(const app_t *a)
{
    aos_link_neighbour_t nb[AOS_LINK_NEIGHBOURS];
    int n = aos_hal_link_neighbours(nb, AOS_LINK_NEIGHBOURS);
    for (int i = 0; i < n; i++) {
        if (memcmp(nb[i].mac, a->partner_mac, 6) == 0) {
            return strcmp(nb[i].app, "pixel") == 0;
        }
    }
    return false;
}

static void link_send_done(int8_t slot)
{
    pxl_done_t d = { .type = PXL_DONE, .proto = PXL_PROTO, .slot = slot };
    aos_hal_link_send_reliable(&d, sizeof d);
}

/* The whole file arrived: into the first empty slot, then the gallery. */
static void link_rx_finish(app_t *a)
{
    int slot = -1;
    char path[160];
    for (int i = 0; i < PX_SLOTS; i++) {
        slot_path(i, path, sizeof(path));
        struct stat st;
        if (stat(path, &st) != 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        aos_hal_log(TAG, "drawing from %s: no empty slot", a->pname);
        link_send_done(-1);
        return;
    }
    slot_path(slot, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(a->rx_buf, 1, a->rx_total, f) == a->rx_total;
    if (f) fclose(f);
    int size = 0, frames = 0;
    if (!ok || !px_doc_peek(path, &size, &frames)) {
        remove(path);
        aos_hal_log(TAG, "drawing from %s: %lu bytes, not a .pix", a->pname, (unsigned long)a->rx_total);
        link_send_done(-2);
        return;
    }
    aos_hal_log(TAG, "drawing from %s: %dx%d, %d frames, slot %d", a->pname, size, size, frames, slot + 1);
    link_send_done((int8_t)slot);
    char msg[96];
    lv_snprintf(msg, sizeof msg, _("Dibujo de %s en el lienzo %d"), a->pname, slot + 1);
    aos_ui_toast(msg, 2500);
    aos_hal_beep(880, 60);
    if (a->slot < 0) gallery_refresh(a);
}

static void link_tick(app_t *a)
{
    uint32_t now = now_ms();
    if (!a->pname[0]) {
        aos_link_partner_t p;
        if (aos_hal_link_partner(&p) && p.valid) {
            memcpy(a->partner_mac, p.mac, 6);
            snprintf(a->pname, sizeof a->pname, "%s", p.name[0] ? p.name : "?");
        }
    }
    aos_link_frame_t f;
    while (aos_hal_link_recv_reliable(&f) > 0) {
        if (f.len < 4 || f.data[1] != PXL_PROTO) continue;
        if (f.data[0] == PXL_START && f.len >= sizeof(pxl_start_t)) {
            const pxl_start_t *st = (const pxl_start_t *)f.data;
            a->rx_active = st->total > 0 && st->total <= PXL_MAX_FILE;
            a->rx_total  = st->total;
            a->rx_got    = 0;
            a->rx_ms     = now;
        } else if (f.data[0] == PXL_DATA && a->rx_active) {
            const pxl_data_t *d = (const pxl_data_t *)f.data;
            uint32_t len = f.len - 4;
            if ((uint32_t)d->off + len > a->rx_total) {
                a->rx_active = false;
                continue;
            }
            memcpy(a->rx_buf + d->off, d->data, len);
            a->rx_got += len;
            a->rx_ms   = now;
            if (a->rx_got >= a->rx_total) {
                a->rx_active = false;
                link_rx_finish(a);
            }
        } else if (f.data[0] == PXL_DONE && a->tx_active) {
            const pxl_done_t *d = (const pxl_done_t *)f.data;
            a->tx_active = false;
            char msg[96];
            if (d->slot >= 0) {
                lv_snprintf(msg, sizeof msg, _("Llegó al lienzo %d de %s"), d->slot + 1, a->pname);
                aos_hal_beep(660, 40);
            } else if (d->slot == -1) {
                lv_snprintf(msg, sizeof msg, _("%s no tiene lugar"), a->pname);
            } else {
                lv_snprintf(msg, sizeof msg, _("%s no pudo guardarlo"), a->pname);
            }
            aos_ui_toast(msg, 2500);
        }
    }
    if (a->tx_active) {
        /* as many chunks as the queue takes; the rest next tick */
        while (a->tx_off < a->tx_total) {
            pxl_data_t d = { .type = PXL_DATA, .proto = PXL_PROTO, .off = (uint16_t)a->tx_off };
            uint32_t len = a->tx_total - a->tx_off;
            if (len > PXL_CHUNK) len = PXL_CHUNK;
            memcpy(d.data, a->tx_buf + a->tx_off, len);
            if (!aos_hal_link_send_reliable(&d, 4 + len)) break;
            a->tx_off += len;
        }
        if (aos_hal_link_reliable_lost() || now - a->tx_ms > PXL_TX_TIMEOUT) {
            a->tx_active = false;
            aos_hal_link_reliable_reset();
            aos_ui_toast(_("No se pudo enviar"), 2000);
        }
    }
    if (a->rx_active && now - a->rx_ms > PXL_RX_TIMEOUT) {
        a->rx_active = false;
    }
}

static void mi_send_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    menu_close(a);
    if (a->slot < 0 || !a->link_up || a->tx_active) return;
    if (a->doc->size > PXL_SIZE_MAX) {
        /* the watch's protocol and the watch itself stop at 16x16 */
        aos_ui_toast(_("Sólo viajan los de 8×8 y 16×16"), 2500);
        return;
    }
    if (a->dirty) save_doc(a);
    char msg[96];
    if (!partner_in_pixel(a)) {
        lv_snprintf(msg, sizeof msg, _("%s no está en Pixel Art"), a->pname);
        aos_ui_toast(msg, 2500);
        return;
    }
    char path[160];
    slot_path(a->slot, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    long n = 0;
    if (f) {
        n = (long)fread(a->tx_buf, 1, PXL_MAX_FILE, f);
        fclose(f);
    }
    if (n <= 0) {
        aos_ui_toast(_("No se pudo enviar"), 2000);
        return;
    }
    if (aos_hal_link_reliable_lost()) aos_hal_link_reliable_reset();
    pxl_start_t st = { .type = PXL_START, .proto = PXL_PROTO, .total = (uint32_t)n };
    if (!aos_hal_link_send_reliable(&st, sizeof st)) {
        aos_ui_toast(_("No se pudo enviar"), 2000);
        return;
    }
    a->tx_total  = (uint32_t)n;
    a->tx_off    = 0;
    a->tx_ms     = now_ms();
    a->tx_active = true;
    aos_hal_log(TAG, "sending %s (%ld bytes) to %s", path, n, a->pname);
    lv_snprintf(msg, sizeof msg, _("Enviando a %s..."), a->pname);
    aos_ui_toast(msg, 1500);
}

/* Only with a partner paired in Enlace, and only if the radio starts: the
 * link costs radio time and memory, nothing to spend on a board that is
 * alone. On the P4 today aos_hal_link_start() fails (no ESP-NOW through the
 * C6 yet) and the menu simply has no "Enviar". */
static void link_try_start(app_t *a)
{
    aos_link_partner_t partner;
    const char *lk_env = getenv("PX_LINK");     /* the simulator: force a try */
    bool paired = aos_hal_link_partner(&partner) && partner.valid;
    if (!paired && !(lk_env && lk_env[0])) {
        return;
    }
    a->tx_buf = malloc(PXL_MAX_FILE);
    a->rx_buf = malloc(PXL_MAX_FILE);
    if (a->tx_buf && a->rx_buf && aos_hal_link_start()) {
        a->link_up = true;
        aos_hal_link_offer("pixel");
        aos_hal_link_reliable_reset();
        if (paired) {
            memcpy(a->partner_mac, partner.mac, 6);
            snprintf(a->pname, sizeof a->pname, "%s", partner.name);
        }
    } else {
        aos_hal_log(TAG, "no link: sending drawings is off");
        free(a->tx_buf);
        free(a->rx_buf);
        a->tx_buf = a->rx_buf = NULL;
    }
}

/* --------------------------------------------------------------------------
 * The timer: deferred exit, playback, autosave, and watching the card
 * -------------------------------------------------------------------------- */

static void timer_cb(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    if (a->closing) {
        return;
    }
    if (a->exit_req) {
        a->exit_req = false;
        a->exiting = true;                  /* so back() lets the system leave */
        aos_ui_back();
        return;                             /* the context may be gone now */
    }
    uint32_t now = now_ms();

    if (a->playing && (int32_t)(now - a->play_next_ms) >= 0) {
        a->play_next_ms = now + a->doc->delay_ms;
        a->frame = (a->frame + 1) % a->doc->frames;
        draw_frame(a);
        editor_refresh(a);
    }

    if (a->menu_del_req) {
        a->menu_del_req = false;
        lv_obj_delete(a->menu);
        a->menu = NULL;
    }
    if (a->link_up) {
        link_tick(a);
    }

    if (a->dirty && a->slot >= 0 && now - a->changed_ms > AUTOSAVE_MS && !a->stroke &&
        (int32_t)(now - a->save_retry_ms) >= 0) {
        save_doc(a);
    }

    if (now - a->watch_ms > WATCH_MS) {
        a->watch_ms = now;
        if (a->slot >= 0) {
            if (!a->dirty && !a->stroke) {
                char path[160];
                slot_path(a->slot, path, sizeof(path));
                struct stat st;
                if (stat(path, &st) == 0 &&
                    ((long)st.st_size != a->file_size || (long)st.st_mtime != a->file_mtime)) {
                    int keep = a->frame;
                    if (px_doc_load(a->doc, path)) {
                        note_file(a, path);
                        undo_clear(a);
                        view_reset(a);      /* the size may have changed */
                        show_frame(a, keep < a->doc->frames ? keep : 0);
                        aos_hal_log(TAG, "reloaded %s: it changed on disk", path);
                        aos_ui_toast(_("Actualizado desde el portal"), 1200);
                    }
                }
            }
        } else if (gallery_signature() != a->gal_sig) {
            gallery_refresh(a);
        }
    }
}

/* --------------------------------------------------------------------------
 * The sample canvases
 *
 * Seeded ONCE, the first time the app opens with nothing in the folder: a
 * sunset over the sea with a black cat watching it (64x64, three frames of
 * shimmering water: what the big canvas is for), the watch's black kitten
 * walking (16x16, four frames), a beating heart (8x8, three), a winking face
 * (16x16, two) and a checkerboard. A preference remembers the seeding, so
 * deleting them is respected.
 *
 * Content is tables, not code.
 * -------------------------------------------------------------------------- */

/* The kitten's legend, one char per palette index. */
static uint8_t kit_color(char ch)
{
    switch (ch) {
    case '.': return 14;    /* sky            */
    case ',': return 29;    /* lower sky, ice */
    case 'S': return 8;     /* sun            */
    case 's': return 9;     /* sun core       */
    case 'w': return 1;     /* cloud          */
    case 'k': return 0;     /* the cat        */
    case 'e': return 12;    /* eyes, lime     */
    case 'p': return 19;    /* nose, pink     */
    case 'g': return 10;    /* grass          */
    case 'G': return 11;    /* dark grass     */
    case 'l': return 12;    /* grass tuft     */
    case 'f': return 19;    /* pink flower    */
    case 'y': return 8;     /* yellow flower  */
    case 'W': return 1;     /* white flower   */
    default:  return 0;
    }
}

/* Four frames of the walk. The legs alternate, the tail wags, and the
 * flowers in the grass slide left one cell per frame, which is what makes
 * the cat look like it is going somewhere. */
static const char *const kit_frames[4][16] = {
    { "..............SS", ".............SsS", "..............SS", "...ww...........",
      "..wwww..........", "..k.............", ".k.......k...k..", ".k.......kkkkk..",
      ",kkkkkkkkkekek,,", ",,kkkkkkkkkkkp,,", ",,kkkkkkkkkk,,,,", ",,,k,k,,k,k,,,,,",
      "gggkggggkggggggg", "gfgggyggggWgggfg", "glgggglgggggglgg", "GGGGGGGGGGGGGGGG" },
    { "..............SS", ".............SsS", "..............SS", "...ww...........",
      "..wwww..........", ".k..............", ".k.......k...k..", ".k.......kkkkk..",
      ",kkkkkkkkkekek,,", ",,kkkkkkkkkkkp,,", ",,kkkkkkkkkk,,,,", ",,,k,k,,k,k,,,,,",
      "gggkgkggkgkggggg", "fgggyggggWgggfgg", "lgggglgggggglggg", "GGGGGGGGGGGGGGGG" },
    { "..............SS", ".............SsS", "..............SS", "...ww...........",
      "..wwww..........", "..k.............", ".k.......k...k..", ".k.......kkkkk..",
      ",kkkkkkkkkekek,,", ",,kkkkkkkkkkkp,,", ",,kkkkkkkkkk,,,,", ",,,k,k,,k,k,,,,,",
      "gggggkggggkggggg", "gggyggggWgggfggg", "ggggglgggggglggg", "GGGGGGGGGGGGGGGG" },
    { "..............SS", ".............SsS", "..............SS", "...ww...........",
      "..wwww..........", "..k.............", ".kk......k...k..", ".k.......kkkkk..",
      ",kkkkkkkkkekek,,", ",,kkkkkkkkkkkp,,", ",,kkkkkkkkkk,,,,", ",,,k,k,,k,k,,,,,",
      "gggkgkggkgkggggg", "ggyggggWgggfgggg", "ggggglgggggglggg", "GGGGGGGGGGGGGGGG" },
};

/* A black cat sitting with its back to us, the tail curled round at its
 * feet. */
static const char *const cat_sil[18] = {
    "..#.......#.......",
    "..##.....##.......",
    "..#########.......",
    ".###########......",
    ".###########......",
    ".###########......",
    "..#########.......",
    "...#######........",
    "..#########.......",
    ".###########......",
    "#############.....",
    "#############.....",
    "#############...##",
    "#############....#",
    "#############....#",
    ".###########....##",
    "..##########...##.",
    "...###############",
};

/* The sunset: bands of sky, the sun half down, the sea with the sun's
 * reflection broken into stripes that shift every frame, a dark rock and
 * the cat on it. */
static void seed_sunset(px_doc_t *d)
{
    static const uint8_t sky[] = { 16, 16, 18, 18, 17, 17, 19, 19, 20, 6, 6, 8, 9 };
    const int n = 64, horizon = 40, sun_x = 40, sun_y = 38, sun_r = 11;
    px_doc_init(d, n);
    for (int f = 0; f < 3; f++) {
        if (f) px_doc_frame_blank(d, f - 1);
        uint8_t *px = d->px[f];
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                uint8_t c;
                if (y < horizon) {
                    c = sky[y * (int)sizeof(sky) / horizon];
                    int dx = x - sun_x, dy = y - sun_y;
                    int r2 = dx * dx + dy * dy;
                    if (r2 <= sun_r * sun_r) c = r2 <= (sun_r - 4) * (sun_r - 4) ? 9 : 8;
                } else {
                    /* the sea darkens with depth; under the sun, broken
                     * stripes of its light, narrower further down */
                    int depth = y - horizon;
                    c = depth < 6 ? 15 : 16;
                    int half = sun_r - depth / 3;
                    if (half > 0 && x >= sun_x - half && x <= sun_x + half &&
                        ((x + y * 3 + f * 2) % 6) < 3 && (depth + f) % 2 == 0) {
                        c = depth < 8 ? 9 : depth < 16 ? 8 : 6;
                    }
                }
                px[y * n + x] = c;
            }
        }
        /* the rock, a low dark hump on the left */
        for (int y = 50; y < n; y++) {
            for (int x = 0; x < 30; x++) {
                int dx = x - 12, dy = y - 64;
                if (dx * dx * 2 + dy * dy * 5 < 900) px[y * n + x] = 31;
            }
        }
        /* the cat on the rock, black against the sky; the tail tip flicks
         * on the last frame */
        for (int y = 0; y < 18; y++) {
            for (int x = 0; x < 18; x++) {
                if (cat_sil[y][x] == '#') px[(y + 35) * n + x + 4] = 0;
            }
        }
        if (f == 2) {
            px[(12 + 35) * n + 16 + 4] = 0;
            px[(11 + 35) * n + 16 + 4] = 0;
            px[(12 + 35) * n + 17 + 4] = 6;
        }
    }
    d->delay_ms = 300;
}

static void seed_samples(app_t *a)
{
    static const char *const heart[8] = {
        "........", ".XX..XX.", "XXXXXXXX", "XXXXXXXX",
        ".XXXXXX.", "..XXXX..", "...XX...", "........" };
    static const char *const face[16] = {
        "................", ".....BBBBBB.....", "...BBYYYYYYBB...", "..BYYYYYYYYYYB..",
        ".BYYYYYYYYYYYYB.", ".BYYKKYYYYKKYYB.", "BYYYKKYYYYKKYYYB", "BYYYYYYYYYYYYYYB",
        "BYYYYYYYYYYYYYYB", "BYYYKYYYYYYKYYYB", ".BYYYKKYYYYKKYYB", ".BYYYYKKKKKKYYB.",
        "..BYYYYYYYYYYB..", "...BBYYYYYYBB...", ".....BBBBBB.....", "................" };
    char path[160];

    /* 1: the sunset */
    seed_sunset(a->doc);
    slot_path(0, path, sizeof(path));
    px_doc_save(a->doc, path);

    /* 2: the kitten */
    px_doc_init(a->doc, 16);
    for (int f = 0; f < 4; f++) {
        if (f) px_doc_frame_blank(a->doc, f - 1);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                a->doc->px[f][y * 16 + x] = kit_color(kit_frames[f][y][x]);
    }
    a->doc->delay_ms = 200;
    slot_path(1, path, sizeof(path));
    px_doc_save(a->doc, path);

    /* 3: the heart, three shades */
    px_doc_init(a->doc, 8);
    for (int f = 0; f < 3; f++) {
        if (f) px_doc_frame_blank(a->doc, f - 1);
        int color = f == 0 ? 4 : f == 1 ? 19 : 5;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                a->doc->px[f][y * 8 + x] = heart[y][x] == 'X' ? (uint8_t)color : 0;
    }
    a->doc->delay_ms = 300;
    slot_path(2, path, sizeof(path));
    px_doc_save(a->doc, path);

    /* 4: the face, which winks */
    px_doc_init(a->doc, 16);
    for (int f = 0; f < 2; f++) {
        if (f) px_doc_frame_dup(a->doc, 0);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                char ch = face[y][x];
                uint8_t c = ch == 'B' ? 7 : ch == 'Y' ? 8 : 0;
                if (ch == 'K' && f == 1 && y < 7) c = 8;
                a->doc->px[f][y * 16 + x] = c;
            }
    }
    a->doc->delay_ms = 500;
    slot_path(3, path, sizeof(path));
    px_doc_save(a->doc, path);

    /* 5: a checkerboard in blues */
    px_doc_init(a->doc, 16);
    for (int i = 0; i < 256; i++) {
        int x = i % 16, y = i / 16;
        a->doc->px[0][i] = (uint8_t)(((x / 2) + (y / 2)) % 2 ? 13 + (x / 4) : 0);
    }
    slot_path(4, path, sizeof(path));
    px_doc_save(a->doc, path);

    aos_hal_log(TAG, "sample canvases written to %s", px_dir());
}

/* True when no canvas exists at all. */
static bool folder_empty(void)
{
    for (int i = 0; i < PX_SLOTS; i++) {
        char path[160];
        int sz, fr;
        slot_path(i, path, sizeof(path));
        if (px_doc_peek(path, &sz, &fr)) {
            return false;
        }
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

#define KEY_SEEDED  "px_seed"

/* Builds both screens for the root's current size and puts back whichever
 * was showing. */
static bool build_ui(app_t *a, lv_obj_t *root)
{
    layout_compute(a, root);
    if (!canvas_alloc(a)) {
        return false;
    }
    build_gallery(a, root);
    build_editor(a, root);
    gallery_labels(a);
    if (a->slot >= 0) {
        editor_enter(a);
    }
    return true;
}

static void free_all(app_t *a)
{
    free(a->tx_buf);
    free(a->rx_buf);
    free(a->doc);
    free(a->scratch);
    free(a->big);
    free(a->undo_buf);
    free(a->before);
    for (int i = 0; i < PX_SLOTS; i++) {
        free(a->thumb[i]);
    }
    lv_free(a);
}

static void *px_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)lv_malloc_zeroed(sizeof(app_t));
    if (!a) {
        return NULL;
    }
    a->self = self;
    a->root = root;
    a->slot = -1;
    a->color = 1;                           /* white */
    a->last_x = a->last_y = -1;
    a->view_zoom = 1.0f;
    int32_t grid = 1;
    aos_hal_pref_get_i32(KEY_GRID, &grid);
    a->grid = grid != 0;

    /* Everything big goes through malloc() -PSRAM-, never lv_malloc(). */
    a->doc      = malloc(sizeof(px_doc_t));
    a->scratch  = malloc(sizeof(px_doc_t));
    a->undo_buf = malloc((size_t)UNDO_MAX * PX_CELLS);
    a->before   = malloc(PX_CELLS);
    bool ok = a->doc && a->scratch && a->undo_buf && a->before;
    for (int i = 0; i < PX_SLOTS; i++) {
        a->thumb[i] = malloc((size_t)TH_PX * TH_PX * sizeof(uint16_t));
        ok = ok && a->thumb[i];
        if (a->thumb[i]) {
            memset(a->thumb[i], 0, (size_t)TH_PX * TH_PX * sizeof(uint16_t));
        }
    }
    if (!ok) {
        aos_hal_log(TAG, "out of memory");
        free_all(a);
        return NULL;
    }
    px_doc_init(a->doc, 16);
    link_try_start(a);

    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* The samples, once. In the simulator PX_DEMO=1 forces them, for the
     * screenshots. */
    int32_t seeded = 0;
    bool force = false;
#ifdef AOS_SIM_BUILTIN
    const char *env = getenv("PX_DEMO");
    force = env && env[0];
#endif
    if (force || (!(aos_hal_pref_get_i32(KEY_SEEDED, &seeded) && seeded) && folder_empty())) {
        if (folder_empty()) {
            seed_samples(a);
        }
        aos_hal_pref_set_i32(KEY_SEEDED, 1);
    }
    gallery_scan(a);

    if (!build_ui(a, root)) {
        aos_hal_log(TAG, "out of memory for the canvas");
        lv_obj_clean(root);
        if (a->link_up) {
            aos_hal_link_offer("");
            aos_hal_link_stop();
        }
        free_all(a);
        return NULL;
    }
    a->watch_ms = now_ms();
    a->timer = lv_timer_create(timer_cb, TIMER_MS, a);
    a->view_timer = lv_timer_create(view_timer_cb, 16, a);

#ifdef AOS_SIM_BUILTIN
    /* Development switches, simulator only (on the board getenv() is NULL):
     *   PX_DEMO=1   write the samples if the folder is empty
     *   PX_SLOT=n   open canvas n straight away
     *   PX_FRAME=f  ...on frame f
     *   PX_MENU=1   ...with the menu open (to audit its layout)
     *   PX_NEW=1    open the size chooser */
    env = getenv("PX_SLOT");
    if (env && env[0]) {
        int s = atoi(env) - 1;
        if (s >= 0 && s < PX_SLOTS && a->slot_size[s] && a->slot_size[s] != SLOT_ERR) {
            go_editor(a, s);
            const char *fr = getenv("PX_FRAME");
            if (fr && fr[0]) {
                show_frame(a, atoi(fr) - 1);
            }
            const char *m = getenv("PX_MENU");
            if (m && m[0]) {
                build_menu(a);
                menu_refresh(a);
                lv_obj_remove_flag(a->menu, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    env = getenv("PX_NEW");
    if (env && env[0]) {
        sizer_show(a, PX_SLOTS - 1);
    }
#endif
    return a;
}

/* The screen turned: the same document, the same frame and colour, laid out
 * again. The zoom goes back to fit - its offsets were in the old canvas's
 * pixels - and an open menu or size chooser closes. */
static bool px_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return false;
    }
    play_stop(a);
    /* the touch layer hears PRESS_LOST as it goes */
    a->closing = true;
    lv_obj_clean(root);
    a->closing = false;
    a->menu = NULL;
    a->menu_del_req = false;
    a->confirm_del = false;
    a->stroke = false;
    a->last_x = a->last_y = -1;
    view_reset(a);
    a->view_dirty = false;
    if (!build_ui(a, root)) {
        return false;                       /* the runtime creates us again */
    }
    return true;
}

static void px_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return;
    }
    if (a->view_timer) {
        lv_timer_delete(a->view_timer);
        a->view_timer = NULL;
    }
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    if (a->dirty) {
        save_doc(a);
    }
    /* Our objects go HERE, while the context is alive: the runtime deletes
     * the root after destroy(), and the touch layer listens for PRESS_LOST. */
    a->closing = true;
    if (a->root) {
        lv_obj_clean(a->root);
    }
    if (a->link_up) {
        aos_hal_link_offer("");
        aos_hal_link_stop();
    }
    free_all(a);
}

/* Kept alive in the background (AOS_APP_FLAG_KEEP): the drawing is saved
 * and the timers rest until it comes back. */
static void px_hide(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a) {
        play_stop(a);
        a->stroke = false;
        if (a->dirty) {
            save_doc(a);
        }
        if (a->timer) lv_timer_pause(a->timer);
        if (a->view_timer) lv_timer_pause(a->view_timer);
        if (a->link_up) aos_hal_link_offer("");
    }
}

static void px_show(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a) {
        if (a->timer) lv_timer_resume(a->timer);
        if (a->view_timer) lv_timer_resume(a->view_timer);
        if (a->link_up) aos_hal_link_offer("pixel");
        a->watch_ms = 0;                    /* look at the card at once */
    }
}

static bool px_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a || a->exiting) {
        return false;
    }
    if (!lv_obj_has_flag(a->sizer, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(a->sizer, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    if (a->slot >= 0) {
        if (a->playing) {
            play_stop(a);
            return true;
        }
        if (menu_open(a)) {
            menu_close(a);
            return true;
        }
        go_gallery(a);
        return true;
    }
    return false;
}

static bool px_init(aos_app_t *app)
{
    app->desc.id       = "aos.pixel";
    app->desc.name     = "Pixel Art";
    app->desc.icon     = LV_SYMBOL_EDIT;    /* fallback for a firmware without the vector */
    app->desc.icon_vec = AOS_ICON_PIXEL;
    app->desc.color_a  = 0xD93A6A;
    app->desc.color_b  = 0x5B2A86;
    app->desc.order    = 79;
    /* NO_SWIPE and LONG_DRAG together: a stroke across the canvas is a drag
     * far longer than 50 px, and neither the edge's back gesture nor
     * lv_indev_wait_release() may cut it. Back is the arrow in the bar. KEEP:
     * a drawing half done, its frame and its zoom are worth finding again. */
    app->desc.flags    = AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG | AOS_APP_FLAG_KEEP;

    app->create  = px_create;
    app->destroy = px_destroy;
    app->show    = px_show;
    app->hide    = px_hide;
    app->back    = px_back;
    app->resize  = px_resize;
    return true;
}

AOS_APP_ENTRY(px_init);
