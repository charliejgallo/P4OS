/*
 * P4OS (from AmoledOS) - Cámaras: every camera of the house at once, and
 * any of them full screen.
 *
 * Two tabs:
 *
 *   Cámaras   the mosaic of cameras.txt (cam_cfg.c): a 16:9 tile per
 *             camera, laid out for the orientation (one column upright
 *             with few cameras, two with more; 2x2 lying down, where four
 *             640x360 tiles are the whole screen), each tile live at the
 *             mosaic's rate (cam_view.h: keyframes only for H.264, 4 fps
 *             for MJPEG).
 *   Frigate   when cameras.txt names a Frigate server: its cameras (the
 *             latest.jpg of each, every 2 s) and its recent events with
 *             their thumbnails (cam_frigate.c).
 *
 * Tapping a tile opens the viewer: the whole screen, every frame, the
 * picture fitted or filling (a button, remembered), and an overlay that
 * hides by itself: the name, codec, size, fps and bitrate, and buttons for
 * a snapshot (a JPEG in photos/camaras/, where Fotos finds it), fit/fill,
 * turning the screen, and the previous and next camera. The viewer keeps
 * the tile's connection (only the size and the policy change) unless the
 * camera has a 'full' URL of its own; the other tiles let go of theirs
 * while it is open, and take them again on the way back.
 *
 * Frames reach the screen through an lv_canvas per tile and one for the
 * viewer, pointed at the frame the camera's thread finished last (three
 * buffers per camera, cam_view.h). AmoledOS pushed pixels past LVGL to the
 * watch's QSPI panel; P4OS has no such blit yet, and through LVGL the
 * pictures also reach the portal's capture and the simulator's screenshots.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "cam.h"
#include "cam_frigate.h"
#include "cam_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TIMER_MS        15
#define STATS_MS        1000
#define OVERLAY_MS      5000
#define GAP             16
#define TILE_RADIUS     24
#define KEY_MODE        "cam_mode"
#define KEY_TAB         "cam_tab"
#define THUMB           112         /* an event's thumbnail, square */
#define LOG_EVERY       10          /* stats passes per log line in the viewer */

enum { TAB_CAMS = 0, TAB_FRIGATE };
enum { SRC_CAMS = 0, SRC_FRIGATE, SRC_EVENT };

typedef struct {
    cam_t              cam;
    cam_view_t        *v;
    lv_obj_t          *box, *canvas, *name, *badge, *badge_l, *dot, *msg;
    int                bw, bh;
    const cam_frame_t *shown;       /* the frame the canvas points at */
    int                last_state;
    uint32_t           last_detail;
    uint32_t           frames;      /* taken since the last stats pass */
} tile_t;

typedef struct {
    char      id[48];
    lv_obj_t *canvas;
    uint16_t *px;                   /* the canvas's copy of the thumbnail */
} evrow_t;

static struct {
    lv_obj_t  *root;
    int32_t    W, H;
    bool       land;
    cam_cfg_t  cfg;
    uint32_t   cfg_stamp;
    int        tab;
    int        mode;                /* the viewer's cam_mode_t */
    int        rot_restore;         /* -1, or the orientation to go back to */
    lv_timer_t *timer;
    uint64_t   stats_t0;
    bool       closing;

    /* the tabs */
    lv_obj_t  *content;
    lv_obj_t  *seg[2];
    tile_t     tiles[CAM_MAX];
    int        ntiles;

    /* Frigate */
    cam_frigate_t *fg;
    uint32_t   fg_gen;
    tile_t     ftiles[FG_MAX_CAMS];
    int        nftiles;
    lv_obj_t  *fg_grid, *fg_side, *fg_list, *fg_status;
    int32_t    fg_grid_max;         /* upright: the most the grid may take */
    fg_event_t evs[FG_MAX_EVENTS];  /* a copy, thumb pointers not owned */
    int        nev;
    evrow_t    rows[FG_MAX_EVENTS];
    int        nrows;

    /* the viewer */
    struct {
        bool        on;
        bool        leave;          /* back(): close on the next tick */
        int         src, index;
        tile_t     *tile;           /* whose connection it borrowed, or NULL */
        cam_view_t *v;
        bool        own;            /* v is the viewer's (url_full, an event) */
        cam_t       cam;
        lv_obj_t   *obj, *canvas, *top, *bottom, *name, *info, *msg, *fit_g, *snap_g;
        const cam_frame_t *shown;
        uint64_t    overlay_until;
        bool        overlay;
        int         last_state;
        uint32_t    last_detail;
        uint32_t    frames, bytes0, pics0, dec0, conv0, drop0, err0, skips0;
        int         log_n;
        bool        snapping;
        char        info_txt[160];
    } vw;
} A;

/* ---- small pieces --------------------------------------------------------- */

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *pill(lv_obj_t *parent)
{
    lv_obj_t *p = box(parent, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(p, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_50, 0);
    lv_obj_set_style_radius(p, 16, 0);
    lv_obj_set_style_pad_hor(p, 14, 0);
    lv_obj_set_style_pad_ver(p, 6, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
    return p;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(c, 32, 0);
    lv_obj_set_style_pad_hor(c, 26, 0);
    lv_obj_set_style_bg_color(c, on ? AOS_C_TEXT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = aos_label(c, text, aos_font_small, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    return c;
}

/* A round button with a glyph, for the viewer's overlay. */
static lv_obj_t *round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud, lv_obj_t **label)
{
    lv_obj_t *b = box(parent, AOS_UI_TAP_MIN, AOS_UI_TAP_MIN);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x48484A), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = aos_label(b, glyph, &aos_sym_44, AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    if (label) *label = l;
    return b;
}

/* "12,5" */
static void fmt_dec1(char *out, size_t n, uint32_t tenths)
{
    snprintf(out, n, "%u,%u", (unsigned)(tenths / 10), (unsigned)(tenths % 10));
}

static void set_text(lv_obj_t *l, const char *t)
{
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, t)) lv_label_set_text(l, t);
}

static const char *state_text(const cam_view_t *v)
{
    switch (v->state) {
    case CAM_ST_CONNECTING:  return _("Conectando…");
    case CAM_ST_NEGOTIATING: return _("Abriendo el video…");
    case CAM_ST_WAITING:     return _("Esperando la imagen…");
    default:                 return NULL;
    }
}

/* The words for a camera without picture, or NULL if it is live. */
static void message_for(const cam_view_t *v, char *out, size_t n)
{
    out[0] = '\0';
    if (v->undecodable) {
        snprintf(out, n, "%s", _("Este video no se puede ver en la placa.\n"
                                 "Pasá la cámara a H.264 Baseline\no a MJPEG."));
    } else if (v->state == CAM_ST_UNSUPPORTED || v->state == CAM_ST_ERROR) {
        char d[112];
        aos_hal_mutex_lock(v->mx);
        snprintf(d, sizeof d, "%s", v->detail);
        aos_hal_mutex_unlock(v->mx);
        if (v->state == CAM_ST_ERROR) snprintf(out, n, "%s\n%s", d, _("Reintentando…"));
        else snprintf(out, n, "%s", d);
    } else if (state_text(v)) {
        snprintf(out, n, "%s", state_text(v));
    }
}

/* Points a canvas at a frame, scaled into box_w x box_h if the thread has
 * not caught up with a new size yet (cover or contain, per 'fill'). */
static void canvas_show(lv_obj_t *canvas, const cam_frame_t *f, int box_w, int box_h, bool fill)
{
    lv_canvas_set_buffer(canvas, f->px, f->w, f->h, LV_COLOR_FORMAT_RGB565);
    int32_t scale = LV_SCALE_NONE;
    bool fits = fill ? (f->w == box_w && f->h == box_h)
                     : ((f->w == box_w && f->h <= box_h) || (f->h == box_h && f->w <= box_w));
    if (!fits) {
        int32_t sx = box_w * 256 / f->w, sy = box_h * 256 / f->h;
        scale = fill ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    }
    lv_image_set_pivot(canvas, f->w / 2, f->h / 2);
    lv_image_set_scale(canvas, scale);
    lv_obj_center(canvas);
    lv_obj_invalidate(canvas);
}

/* ---- tiles ------------------------------------------------------------------ */

/* The canvas stops pointing at the camera's frames: they are about to go
 * (a released view is freed by its thread) or to change hands (the viewer
 * borrows the connection). A hidden object is never drawn. */
static void tile_hide_picture(tile_t *t)
{
    if (t->box && t->canvas) {
        lv_obj_add_flag(t->canvas, LV_OBJ_FLAG_HIDDEN);
    }
}

static void tile_stop(tile_t *t)
{
    tile_hide_picture(t);
    if (t->v) {
        cam_view_release(t->v);
        t->v = NULL;
    }
    t->shown = NULL;
}

static void tile_start(tile_t *t)
{
    if (t->v) {
        cam_view_set_target(t->v, t->bw, t->bh, CAM_FILL, true);
        return;
    }
    t->v = cam_view_new(&t->cam, false);
    if (!t->v) return;
    cam_view_set_target(t->v, t->bw, t->bh, CAM_FILL, true);
    t->last_state = -1;
    if (!cam_view_start(t->v)) {
        /* the view keeps its reason; shown as the tile's message */
    }
}

static void open_viewer(int src, int index);

static void tile_click_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    open_viewer(code >> 8, code & 0xFF);
}

static void tile_build(lv_obj_t *parent, tile_t *t, int src, int index, int32_t x, int32_t y, int32_t w, int32_t h)
{
    t->bw = w & ~1;
    t->bh = h & ~1;
    t->box = box(parent, w, h);
    lv_obj_set_pos(t->box, x, y);
    lv_obj_set_style_bg_color(t->box, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(t->box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t->box, TILE_RADIUS, 0);
    lv_obj_set_style_clip_corner(t->box, true, 0);
    lv_obj_add_flag(t->box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t->box, tile_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(src << 8 | index));

    t->canvas = lv_canvas_create(t->box);
    lv_obj_remove_flag(t->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(t->canvas, LV_OBJ_FLAG_HIDDEN);

    t->msg = aos_label(t->box, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(t->msg, w - 40);
    lv_label_set_long_mode(t->msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(t->msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t->msg, LV_ALIGN_CENTER, 0, 24);
    aos_make_decorative(t->msg);
    lv_obj_t *icon = aos_label(t->box, AOS_SYM_CCTV, &aos_sym_44, lv_color_hex(0x48484A));
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -30);
    aos_make_decorative(icon);
    lv_obj_move_to_index(icon, 0);          /* under the picture */

    lv_obj_t *np = pill(t->box);
    lv_obj_align(np, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    t->name = aos_label(np, t->cam.name, aos_font_small, AOS_C_TEXT);
    lv_label_set_long_mode(t->name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(t->name, w - 60, 0);
    aos_make_decorative(np);

    t->badge = pill(t->box);
    lv_obj_align(t->badge, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_set_flex_flow(t->badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t->badge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(t->badge, 8, 0);
    t->dot = box(t->badge, 12, 12);
    lv_obj_set_style_radius(t->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(t->dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t->dot, AOS_C_DIM, 0);
    t->badge_l = aos_label(t->badge, "", aos_font_caption, AOS_C_TEXT);
    aos_make_decorative(t->badge);

    t->last_state = -1;
    t->last_detail = 0;
    /* A tile rebuilt (the screen turned) shows what it had until the thread
     * sends the new size. */
    if (t->shown) {
        lv_obj_remove_flag(t->canvas, LV_OBJ_FLAG_HIDDEN);
        canvas_show(t->canvas, t->shown, t->bw, t->bh, true);
    }
}

static void tile_update(tile_t *t)
{
    cam_view_t *v = t->v;
    if (!v || !t->box) return;
    const cam_frame_t *f = cam_view_take(v);
    if (f) {
        t->shown = f;
        t->frames++;
        lv_obj_remove_flag(t->canvas, LV_OBJ_FLAG_HIDDEN);
        canvas_show(t->canvas, f, t->bw, t->bh, true);
    }
    if (v->state != t->last_state || v->detail_gen != t->last_detail) {
        t->last_state = v->state;
        t->last_detail = v->detail_gen;
        char msg[200];
        message_for(v, msg, sizeof msg);
        set_text(t->msg, msg);
        bool live = v->state == CAM_ST_LIVE && !v->undecodable;
        lv_obj_set_style_bg_color(t->dot, live ? AOS_C_GREEN :
                                  v->state == CAM_ST_ERROR || v->state == CAM_ST_UNSUPPORTED ? AOS_C_RED :
                                  AOS_C_ORANGE, 0);
        if (!live) {
            set_text(t->badge_l, v->state == CAM_ST_ERROR || v->state == CAM_ST_UNSUPPORTED ?
                     _("sin imagen") : _("conectando"));
        } else {
            set_text(t->badge_l, _("en vivo"));     /* until the first rate */
        }
        /* an old picture stays up under a retry, dimmed by the message */
        lv_obj_set_flag(t->msg, LV_OBJ_FLAG_HIDDEN, live && t->shown);
    }
}

/* Once a second: the tile's rate. */
static void tile_stats(tile_t *t, uint32_t dt)
{
    cam_view_t *v = t->v;
    if (!v || !t->box || v->state != CAM_ST_LIVE) {
        t->frames = 0;
        return;
    }
    char buf[48], n[16];
    uint32_t tenths = (uint32_t)(t->frames * 10000ull / (dt ? dt : 1));
    t->frames = 0;
    fmt_dec1(n, sizeof n, tenths);
    const char *codec = v->codec == CAM_CODEC_H264 ? "H.264" : "JPEG";
    if (v->polled) {
        fmt_dec1(n, sizeof n, (uint32_t)(t->cam.refresh_ms / 100));
        snprintf(buf, sizeof buf, _("foto cada %s s"), n);
    } else {
        snprintf(buf, sizeof buf, "%s · %s fps", codec, n);
    }
    set_text(t->badge_l, buf);
}

/* ---- the grid --------------------------------------------------------------- */

/* The column count that gives n 16:9 tiles the most area inside w x h, and
 * the tile's size. Never smaller than min_w wide: past that it scrolls. */
static void grid_for(int n, int32_t w, int32_t h, int32_t *cols, int32_t *tw, int32_t *th)
{
    int32_t best_c = 1, best_w = 0;
    for (int c = 1; c <= n; c++) {
        int r = (n + c - 1) / c;
        int32_t cw = (w - (c - 1) * GAP) / c;
        int32_t ch = (h - (r - 1) * GAP) / r;
        int32_t tw_ = cw;
        if (ch * 16 / 9 < tw_) tw_ = ch * 16 / 9;
        if (tw_ > best_w) {
            best_w = tw_;
            best_c = c;
        }
    }
    if (best_w < 300 && n > 1) {                /* too small: two columns and scroll */
        best_c = w > 1000 ? 3 : 2;
        best_w = (w - (best_c - 1) * GAP) / best_c;
    }
    *cols = best_c;
    *tw = best_w & ~1;
    *th = (best_w * 9 / 16) & ~1;
}

/* Returns the height the grid takes. */
static int32_t layout_tiles(lv_obj_t *parent, tile_t *tiles, int n, int src, int32_t w, int32_t h, bool top)
{
    int32_t cols, tw, th;
    grid_for(n, w, h, &cols, &tw, &th);
    int rows = (n + cols - 1) / cols;
    int32_t gw = cols * tw + (cols - 1) * GAP;
    int32_t gh = rows * th + (rows - 1) * GAP;
    int32_t x0 = (w - gw) / 2;
    int32_t y0 = gh < h && !top ? (h - gh) / 2 : 0;
    for (int i = 0; i < n; i++) {
        int c = i % cols, r = i / cols;
        /* a last row that is not full is centred */
        int in_row = (r == rows - 1) ? n - r * cols : cols;
        int32_t rx = x0 + (cols - in_row) * (tw + GAP) / 2;
        tile_build(parent, &tiles[i], src, i, rx + c * (tw + GAP), y0 + r * (th + GAP), tw, th);
    }
    return gh;
}

/* ---- the Cámaras tab -------------------------------------------------------- */

static void build_empty(lv_obj_t *parent, int32_t w, int32_t h)
{
    if (w > 760) w = 760;
    lv_obj_t *c = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 36, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 18, 0);
    lv_obj_align(c, LV_ALIGN_CENTER, 0, -h / 10);
    aos_label(c, AOS_SYM_CCTV, &aos_sym_72, AOS_C_DIM);
    aos_label(c, _("Todavía no hay cámaras"), aos_font_body, AOS_C_TEXT);
    char msg[400];
    const char *path = cam_cfg_path();
    const char *ip = aos_hal_net_ip();
    snprintf(msg, sizeof msg,
             _("Agregalas en %s, un bloque por cámara con su url rtsp:// o http://. "
               "Desde la compu: el portal, http://%s, Archivos > Editar cameras.txt."),
             path ? path : "cameras.txt", ip && ip[0] ? ip : "…");
    lv_obj_t *l = aos_label(c, msg, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(l, w - 72);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    aos_make_decorative(c);
}

static void build_cams(int32_t w, int32_t h)
{
    A.ntiles = A.cfg.count;
    if (!A.ntiles) {
        build_empty(A.content, w, h);
        return;
    }
    layout_tiles(A.content, A.tiles, A.ntiles, SRC_CAMS, w, h, false);
    if (!A.vw.on) {
        for (int i = 0; i < A.ntiles; i++) tile_start(&A.tiles[i]);
    }
}

/* ---- the Frigate tab -------------------------------------------------------- */

static void event_click_cb(lv_event_t *e)
{
    open_viewer(SRC_EVENT, (int)(intptr_t)lv_event_get_user_data(e));
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    cam_frigate_refresh(A.fg);
    aos_ui_toast(_("Actualizando…"), 1200);
}

static const char *label_es(const char *l)
{
    static const struct { const char *en, *es; } L[] = {
        { "person", N_("Persona") }, { "car", N_("Auto") }, { "dog", N_("Perro") },
        { "cat", N_("Gato") }, { "bicycle", N_("Bicicleta") }, { "motorcycle", N_("Moto") },
        { "bird", N_("Pájaro") }, { "package", N_("Paquete") }, { "truck", N_("Camión") },
    };
    for (size_t i = 0; i < sizeof L / sizeof L[0]; i++) {
        if (!strcmp(L[i].en, l)) return _(L[i].es);
    }
    return l;
}

static void event_when(const fg_event_t *e, char *out, size_t n)
{
    time_t t = (time_t)e->start;
    struct tm tm, now;
    localtime_r(&t, &tm);
    aos_hal_time_now(&now);
    if (tm.tm_year == now.tm_year && tm.tm_yday == now.tm_yday) {
        snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else if (tm.tm_year == now.tm_year && tm.tm_yday == now.tm_yday - 1) {
        snprintf(out, n, _("ayer %02d:%02d"), tm.tm_hour, tm.tm_min);
    } else {
        snprintf(out, n, "%d/%d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
    }
}

static void event_title(const fg_event_t *e, char *out, size_t n)
{
    char when[24];
    event_when(e, when, sizeof when);
    snprintf(out, n, "%s · %s · %s", label_es(e->label), e->camera, when);
}

static void free_rows(void)
{
    for (int i = 0; i < A.nrows; i++) {
        lv_free(A.rows[i].px);
        A.rows[i].px = NULL;
        A.rows[i].canvas = NULL;
    }
    A.nrows = 0;
}

/* The events list, from A.evs. Thumbnails already here are copied now; the
 * others when they arrive (fg_sync). */
static void build_events(void)
{
    if (!A.fg_list) return;
    lv_obj_clean(A.fg_list);        /* the canvases go before their buffers */
    free_rows();
    int32_t w = lv_obj_get_width(A.fg_list);
    if (!A.nev) {
        lv_obj_t *l = aos_label(A.fg_list, A.fg && A.fg->state == FG_OK ? _("Sin eventos recientes.") : "",
                                aos_font_small, AOS_C_DIM);
        lv_obj_set_style_pad_top(l, 12, 0);
        return;
    }
    for (int i = 0; i < A.nev; i++) {
        const fg_event_t *e = &A.evs[i];
        lv_obj_t *row = box(A.fg_list, w, THUMB + 24);
        lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 20, 0);
        if (e->has_snapshot) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, event_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        lv_obj_t *th = box(row, THUMB, THUMB);
        lv_obj_align(th, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_set_style_radius(th, 14, 0);
        lv_obj_set_style_clip_corner(th, true, 0);
        lv_obj_set_style_bg_color(th, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(th, LV_OPA_COVER, 0);
        lv_obj_remove_flag(th, LV_OBJ_FLAG_CLICKABLE);
        evrow_t *r = &A.rows[A.nrows++];
        snprintf(r->id, sizeof r->id, "%s", e->id);
        r->canvas = lv_canvas_create(th);
        lv_obj_remove_flag(r->canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(r->canvas, LV_OBJ_FLAG_HIDDEN);

        char t1[64];
        if (e->score >= 0) snprintf(t1, sizeof t1, "%s  %d %%", label_es(e->label), e->score);
        else snprintf(t1, sizeof t1, "%s", label_es(e->label));
        lv_obj_t *l1 = aos_label(row, t1, aos_font_body, AOS_C_TEXT);
        lv_obj_align(l1, LV_ALIGN_TOP_LEFT, THUMB + 32, 22);
        char when[24], t2[80];
        event_when(e, when, sizeof when);
        snprintf(t2, sizeof t2, "%s · %s", e->camera, when);
        lv_obj_t *l2 = aos_label(row, t2, aos_font_small, AOS_C_DIM);
        lv_obj_align(l2, LV_ALIGN_BOTTOM_LEFT, THUMB + 32, -22);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l2, w - THUMB - 60 - 90);
        lv_obj_t *g = aos_label(row, e->has_clip ? AOS_SYM_VIDEO : e->has_snapshot ? AOS_SYM_IMAGE : "",
                                &aos_sym_28, AOS_C_DIM);
        lv_obj_align(g, LV_ALIGN_RIGHT_MID, -24, 0);
        aos_make_decorative(row);
        if (e->has_snapshot) lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* Thumbnails that arrived since the list was built. */
static void fill_thumbs(void)
{
    if (!A.fg) return;
    aos_hal_mutex_lock(A.fg->mx);
    for (int i = 0; i < A.nrows; i++) {
        evrow_t *r = &A.rows[i];
        if (r->px || !r->canvas) continue;
        for (int j = 0; j < A.fg->nev; j++) {
            if (A.fg->ev[j].thumb && !strcmp(A.fg->ev[j].id, r->id)) {
                r->px = lv_malloc((size_t)THUMB * THUMB * 2);
                if (r->px) {
                    memcpy(r->px, A.fg->ev[j].thumb, (size_t)THUMB * THUMB * 2);
                    lv_canvas_set_buffer(r->canvas, r->px, THUMB, THUMB, LV_COLOR_FORMAT_RGB565);
                    lv_obj_remove_flag(r->canvas, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_center(r->canvas);
                }
                break;
            }
        }
    }
    aos_hal_mutex_unlock(A.fg->mx);
}

static void build_ftiles(void)
{
    if (!A.fg_grid) return;
    for (int i = 0; i < A.nftiles; i++) {
        A.ftiles[i].box = NULL;
    }
    lv_obj_clean(A.fg_grid);
    if (!A.nftiles) {
        lv_obj_t *l = aos_label(A.fg_grid, A.fg && A.fg->state == FG_OK ? _("Frigate no tiene cámaras.") : "",
                                aos_font_small, AOS_C_DIM);
        lv_obj_center(l);
        return;
    }
    int32_t used = layout_tiles(A.fg_grid, A.ftiles, A.nftiles, SRC_FRIGATE,
                                lv_obj_get_width(A.fg_grid), A.land ? lv_obj_get_height(A.fg_grid) : A.fg_grid_max, true);
    if (!A.land && A.fg_side) {
        /* upright, the events take whatever the tiles leave */
        int32_t ch = lv_obj_get_height(A.content);
        if (used > A.fg_grid_max) used = A.fg_grid_max;
        lv_obj_set_height(A.fg_grid, used);
        lv_obj_set_y(A.fg_side, used + GAP * 2);
        lv_obj_set_height(A.fg_side, ch - used - GAP * 2);
        if (A.fg_list) lv_obj_set_height(A.fg_list, ch - used - GAP * 2 - 64);
    }
    if (!A.vw.on) {
        for (int i = 0; i < A.nftiles; i++) tile_start(&A.ftiles[i]);
    }
}

/* New results from the Frigate thread. */
static void fg_sync(void)
{
    if (!A.fg || A.fg->gen == A.fg_gen) return;
    A.fg_gen = A.fg->gen;
    bool cams_changed = false, evs_changed = false;
    char status[120] = "";
    aos_hal_mutex_lock(A.fg->mx);
    int st = A.fg->state;
    if (st == FG_ERROR) snprintf(status, sizeof status, "%s", A.fg->error);
    else if (st == FG_LOADING) snprintf(status, sizeof status, "%s", _("Cargando…"));
    int nc = A.fg->ncams;
    if (nc != A.nftiles) cams_changed = true;
    for (int i = 0; i < nc && !cams_changed; i++) {
        if (strcmp(A.ftiles[i].cam.name, A.fg->cams[i])) cams_changed = true;
    }
    if (A.fg->nev != A.nev) evs_changed = true;
    for (int i = 0; i < A.fg->nev && !evs_changed; i++) {
        if (strcmp(A.evs[i].id, A.fg->ev[i].id)) evs_changed = true;
    }
    if (evs_changed) {
        memcpy(A.evs, A.fg->ev, sizeof(fg_event_t) * (size_t)A.fg->nev);
        A.nev = A.fg->nev;
        for (int i = 0; i < A.nev; i++) A.evs[i].thumb = NULL;
    }
    char names[FG_MAX_CAMS][32];
    memcpy(names, A.fg->cams, sizeof names);
    aos_hal_mutex_unlock(A.fg->mx);

    if (A.fg_status) set_text(A.fg_status, status);
    if (cams_changed && A.tab == TAB_FRIGATE) {
        for (int i = 0; i < A.nftiles; i++) tile_stop(&A.ftiles[i]);
        A.nftiles = nc;
        for (int i = 0; i < nc; i++) {
            memset(&A.ftiles[i], 0, sizeof A.ftiles[i]);
            cam_frigate_cam(A.fg, names[i], &A.ftiles[i].cam);
        }
        build_ftiles();
    }
    if (evs_changed && A.tab == TAB_FRIGATE) build_events();
    fill_thumbs();
}

static void build_frigate(int32_t w, int32_t h)
{
    if (!A.fg) {
        A.fg = cam_frigate_start(A.cfg.frigate, A.cfg.frigate_user, A.cfg.frigate_pass, THUMB, THUMB);
        A.fg_gen = 0;
    }
    int32_t gw, gh, lx, ly, lw, lh;
    if (A.land) {
        gw = w * 58 / 100;
        gh = h;
        lx = gw + GAP * 2;
        ly = 0;
        lw = w - lx;
        lh = h;
    } else {
        gw = w;
        gh = h * 50 / 100;
        lx = 0;
        ly = gh + GAP * 2;
        lw = w;
        lh = h - ly;
    }
    A.fg_grid = box(A.content, gw, gh);
    A.fg_grid_max = gh;
    lv_obj_t *side = box(A.content, lw, lh);
    A.fg_side = side;
    lv_obj_set_pos(side, lx, ly);
    lv_obj_t *head = box(side, lw, 56);
    lv_obj_t *t = aos_label(head, _("Eventos"), aos_font_body, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 4, 0);
    A.fg_status = aos_label(head, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(A.fg_status, LV_ALIGN_LEFT_MID, 150, 2);
    lv_obj_t *rb = box(head, 56, 56);
    lv_obj_align(rb, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(rb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rb, refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rg = aos_label(rb, AOS_SYM_RESTART, &aos_sym_28, AOS_C_ACCENT);
    lv_obj_center(rg);
    aos_make_decorative(rg);
    A.fg_list = lv_obj_create(side);
    lv_obj_remove_style_all(A.fg_list);
    lv_obj_set_size(A.fg_list, lw, lh - 64);
    lv_obj_set_pos(A.fg_list, 0, 64);
    lv_obj_set_flex_flow(A.fg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(A.fg_list, 12, 0);
    lv_obj_set_style_pad_bottom(A.fg_list, 24, 0);
    lv_obj_set_scroll_dir(A.fg_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(A.fg_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_update_layout(A.content);
    build_ftiles();
    build_events();
    fill_thumbs();
    A.fg_gen = 0;                   /* pick up whatever is there */
    fg_sync();
}

/* ---- the frame around the tabs ----------------------------------------------- */

static void build_ui(void);

static void stop_tab_tiles(int tab)
{
    if (tab == TAB_CAMS) {
        for (int i = 0; i < A.ntiles; i++) tile_stop(&A.tiles[i]);
    } else {
        for (int i = 0; i < A.nftiles; i++) tile_stop(&A.ftiles[i]);
    }
}

static void seg_cb(lv_event_t *e)
{
    int tab = (int)(intptr_t)lv_event_get_user_data(e);
    if (tab == A.tab) return;
    stop_tab_tiles(A.tab);
    A.tab = tab;
    aos_hal_pref_set_i32(KEY_TAB, tab);
    build_ui();
}

static void build_tab_content(void)
{
    lv_obj_update_layout(A.content);
    int32_t w = lv_obj_get_width(A.content), h = lv_obj_get_height(A.content);
    A.fg_grid = A.fg_side = A.fg_list = A.fg_status = NULL;
    free_rows();
    for (int i = 0; i < CAM_MAX; i++) A.tiles[i].box = NULL;
    for (int i = 0; i < FG_MAX_CAMS; i++) A.ftiles[i].box = NULL;
    if (A.tab == TAB_FRIGATE && A.cfg.frigate[0]) build_frigate(w, h);
    else build_cams(w, h);
}

static void build_ui(void)
{
    lv_obj_clean(A.root);
    free_rows();
    A.vw.obj = NULL;
    A.W = lv_obj_get_width(A.root);
    A.H = lv_obj_get_height(A.root);
    A.land = A.W > A.H;
    if (!A.cfg.frigate[0]) A.tab = TAB_CAMS;

    const int32_t pad = AOS_UI_PAD;
    const int32_t head_h = A.land ? 88 : 112;
    lv_obj_t *head = box(A.root, A.W - 2 * pad, head_h);
    lv_obj_set_pos(head, pad, 0);
    lv_obj_t *title = aos_label(head, _("Cámaras"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, A.land ? 4 : 12);
    if (A.cfg.frigate[0]) {
        lv_obj_t *segs = box(head, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(segs, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(segs, 10, 0);
        lv_obj_align(segs, LV_ALIGN_RIGHT_MID, 0, A.land ? 4 : 12);
        A.seg[0] = chip(segs, _("Cámaras"), A.tab == TAB_CAMS, seg_cb, (void *)(intptr_t)TAB_CAMS);
        A.seg[1] = chip(segs, "Frigate", A.tab == TAB_FRIGATE, seg_cb, (void *)(intptr_t)TAB_FRIGATE);
    }
    const int32_t bottom = A.land ? 24 : 40;       /* the home indicator's strip */
    A.content = box(A.root, A.W - 2 * pad, A.H - head_h - bottom);
    lv_obj_set_pos(A.content, pad, head_h);
    if (A.tab == TAB_FRIGATE) {
        lv_obj_add_flag(A.content, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_add_flag(A.content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(A.content, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(A.content, LV_SCROLLBAR_MODE_OFF);
    }
    build_tab_content();
}

/* ---- the viewer ---------------------------------------------------------------- */

static void overlay_show(bool on)
{
    A.vw.overlay = on;
    A.vw.overlay_until = on ? aos_hal_uptime_ms() + OVERLAY_MS : 0;
    if (!A.vw.top) return;
    lv_obj_set_flag(A.vw.top, LV_OBJ_FLAG_HIDDEN, !on);
    lv_obj_set_flag(A.vw.bottom, LV_OBJ_FLAG_HIDDEN, !on);
}

static void view_tap_cb(lv_event_t *e)
{
    (void)e;
    overlay_show(!A.vw.overlay);
}

static void view_back_cb(lv_event_t *e)
{
    (void)e;
    A.vw.leave = true;
}

static void view_retarget(void)
{
    if (A.vw.v) {
        cam_view_set_target(A.vw.v, A.W & ~1, A.H & ~1, (cam_mode_t)A.mode, false);
    }
}

static void fit_cb(lv_event_t *e)
{
    (void)e;
    A.mode = A.mode == CAM_FIT ? CAM_FILL : CAM_FIT;
    aos_hal_pref_set_i32(KEY_MODE, A.mode);
    set_text(A.vw.fit_g, A.mode == CAM_FIT ? AOS_SYM_ARROW_EXPAND_HORIZONTAL : AOS_SYM_FULLSCREEN);
    view_retarget();
    overlay_show(true);
}

static void rotate_cb(lv_event_t *e)
{
    (void)e;
    if (A.rot_restore < 0) A.rot_restore = aos_ui_landscape();
    aos_ui_request_landscape(-1);
    overlay_show(true);
}

static void snap_cb(lv_event_t *e)
{
    (void)e;
    if (!A.vw.v || A.vw.snapping) return;
    const char *photos = aos_hal_path_photos();
    if (!photos || !aos_hal_sd_present()) {
        aos_ui_toast(_("Sin tarjeta: no hay dónde guardar la foto"), 2500);
        return;
    }
    char name[CAM_NAME_LEN];
    int n = 0;
    for (const char *p = A.vw.cam.name; *p && n < (int)sizeof name - 1; p++) {
        char c = *p;
        name[n++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') ? c : '_';
    }
    name[n] = '\0';
    struct tm tm;
    aos_hal_time_now(&tm);
    char path[200];
    snprintf(path, sizeof path, "%s/camaras/%s-%04d%02d%02d-%02d%02d%02d.jpg", photos, n ? name : "cam",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    cam_view_snapshot(A.vw.v, path);
    A.vw.snapping = true;
    lv_obj_set_style_text_color(A.vw.snap_g, AOS_C_YELLOW, 0);
    overlay_show(true);
}

static int viewer_count(int src)
{
    return src == SRC_CAMS ? A.ntiles : src == SRC_FRIGATE ? A.nftiles : A.nev;
}

static void close_viewer(void);

static void step_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int src = A.vw.src, n = viewer_count(src);
    if (n < 2) return;
    int i = A.vw.index;
    for (int k = 0; k < n; k++) {
        i = (i + d + n) % n;
        if (src != SRC_EVENT || A.evs[i].has_snapshot) break;
    }
    close_viewer();
    open_viewer(src, i);
}

static void build_viewer(void)
{
    lv_obj_t *o = box(A.root, A.W, A.H);
    A.vw.obj = o;
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, view_tap_cb, LV_EVENT_CLICKED, NULL);

    A.vw.canvas = lv_canvas_create(o);
    lv_obj_remove_flag(A.vw.canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(A.vw.canvas, LV_OBJ_FLAG_HIDDEN);

    A.vw.msg = aos_label(o, "", aos_font_body, AOS_C_DIM);
    lv_obj_set_width(A.vw.msg, A.W - 120);
    lv_label_set_long_mode(A.vw.msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(A.vw.msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(A.vw.msg);
    aos_make_decorative(A.vw.msg);

    /* top: back, name, the numbers */
    const int32_t top_h = 132;
    A.vw.top = box(o, A.W, top_h);
    lv_obj_set_style_bg_color(A.vw.top, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(A.vw.top, lv_color_black(), 0);
    lv_obj_set_style_bg_main_opa(A.vw.top, LV_OPA_80, 0);
    lv_obj_set_style_bg_grad_opa(A.vw.top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_dir(A.vw.top, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(A.vw.top, LV_OPA_COVER, 0);
    lv_obj_t *bk = round_btn(A.vw.top, AOS_SYM_CHEVRON_LEFT, view_back_cb, NULL, NULL);
    lv_obj_align(bk, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 20);
    A.vw.name = aos_label(A.vw.top, A.vw.cam.name, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(A.vw.name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(A.vw.name, A.W - 2 * AOS_UI_PAD - AOS_UI_TAP_MIN - 24);
    lv_obj_align(A.vw.name, LV_ALIGN_TOP_LEFT, AOS_UI_PAD + AOS_UI_TAP_MIN + 20, 24);
    A.vw.info = aos_label(A.vw.top, "", aos_font_caption, lv_color_hex(0xD1D1D6));
    lv_label_set_long_mode(A.vw.info, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(A.vw.info, A.W - 2 * AOS_UI_PAD - AOS_UI_TAP_MIN - 24, lv_font_get_line_height(aos_font_caption));
    lv_obj_align(A.vw.info, LV_ALIGN_TOP_LEFT, AOS_UI_PAD + AOS_UI_TAP_MIN + 20, 66);
    aos_make_decorative(A.vw.name);
    aos_make_decorative(A.vw.info);

    /* bottom: previous, snapshot, fit/fill, turn, next */
    const int32_t bot_h = 150;
    A.vw.bottom = box(o, A.W, bot_h);
    lv_obj_align(A.vw.bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(A.vw.bottom, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(A.vw.bottom, lv_color_black(), 0);
    lv_obj_set_style_bg_main_opa(A.vw.bottom, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_opa(A.vw.bottom, LV_OPA_80, 0);
    lv_obj_set_style_bg_grad_dir(A.vw.bottom, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(A.vw.bottom, LV_OPA_COVER, 0);
    lv_obj_t *row = box(A.vw.bottom, LV_SIZE_CONTENT, AOS_UI_TAP_MIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, A.land ? 36 : 22, 0);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 10);
    bool many = viewer_count(A.vw.src) > 1;
    if (many) round_btn(row, AOS_SYM_CHEVRON_LEFT, step_cb, (void *)(intptr_t)-1, NULL);
    round_btn(row, AOS_SYM_CAMERA, snap_cb, NULL, &A.vw.snap_g);
    if (A.vw.src != SRC_EVENT) {
        round_btn(row, A.mode == CAM_FIT ? AOS_SYM_ARROW_EXPAND_HORIZONTAL : AOS_SYM_FULLSCREEN,
                  fit_cb, NULL, &A.vw.fit_g);
    } else {
        A.vw.fit_g = NULL;
    }
    round_btn(row, A.land ? AOS_SYM_PHONE_ROTATE_PORTRAIT : AOS_SYM_PHONE_ROTATE_LANDSCAPE, rotate_cb, NULL, NULL);
    round_btn(row, AOS_SYM_GRID_LARGE, view_back_cb, NULL, NULL);
    if (many) round_btn(row, AOS_SYM_CHEVRON_RIGHT, step_cb, (void *)(intptr_t)1, NULL);

    A.vw.last_state = -1;
    A.vw.last_detail = 0;
    if (A.vw.shown) {
        lv_obj_remove_flag(A.vw.canvas, LV_OBJ_FLAG_HIDDEN);
        canvas_show(A.vw.canvas, A.vw.shown, A.W & ~1, A.H & ~1, A.mode == CAM_FILL);
    }
    overlay_show(true);
}

static tile_t *tile_of(int src, int index)
{
    if (src == SRC_CAMS && index < A.ntiles) return &A.tiles[index];
    if (src == SRC_FRIGATE && index < A.nftiles) return &A.ftiles[index];
    return NULL;
}

static void open_viewer(int src, int index)
{
    if (A.vw.on || A.closing) return;
    tile_t *t = tile_of(src, index);
    cam_t cam;
    if (t) {
        cam = t->cam;
    } else if (src == SRC_EVENT && index < A.nev && A.fg) {
        char title[CAM_NAME_LEN];
        event_title(&A.evs[index], title, sizeof title);
        cam_frigate_event(A.fg, &A.evs[index], title, &cam);
    } else {
        return;
    }
    memset(&A.vw, 0, sizeof A.vw);
    A.vw.on = true;
    A.vw.src = src;
    A.vw.index = index;
    A.vw.cam = cam;
    if (t && t->v && !cam.url_full[0]) {
        /* the tile's connection, re-targeted: no reconnection */
        A.vw.tile = t;
        A.vw.v = t->v;
        A.vw.shown = t->shown;
        A.vw.own = false;
    } else {
        if (t) tile_stop(t);
        A.vw.v = cam_view_new(&cam, true);
        A.vw.own = true;
    }
    /* the other tiles let go: the viewer gets the CPU, the network and the
     * sockets */
    for (int i = 0; i < A.ntiles; i++) if (&A.tiles[i] != A.vw.tile) tile_stop(&A.tiles[i]);
    for (int i = 0; i < A.nftiles; i++) if (&A.ftiles[i] != A.vw.tile) tile_stop(&A.ftiles[i]);
    if (A.vw.tile) {
        tile_hide_picture(A.vw.tile);
        A.vw.tile->shown = NULL;    /* the viewer owns the frames now */
    }
    build_viewer();
    if (A.vw.v) {
        view_retarget();
        if (A.vw.own) cam_view_start(A.vw.v);
        A.vw.bytes0 = A.vw.v->bytes;
        A.vw.pics0 = A.vw.v->pictures;
        A.vw.dec0 = A.vw.v->dec_ms;
        A.vw.conv0 = A.vw.v->conv_ms;
        A.vw.drop0 = A.vw.v->dropped;
        A.vw.err0 = A.vw.v->errors;
        A.vw.skips0 = A.vw.v->skips;
    }
    aos_hal_net_low_latency(true);
    aos_hal_log("camaras", "open %s (%s)", cam.name, A.vw.own ? "own connection" : "the tile's connection");
}

static void close_viewer(void)
{
    if (!A.vw.on) return;
    tile_t *t = A.vw.tile;
    if (A.vw.obj) lv_obj_delete(A.vw.obj);      /* the canvas goes before its frames */
    if (A.vw.own) {
        cam_view_release(A.vw.v);
    } else if (t && t->v) {
        /* the tile keeps the connection; back to its size and policy, and
         * to the last frame (it is the UI's until the next take) */
        cam_view_set_target(t->v, t->bw, t->bh, CAM_FILL, true);
        t->shown = A.vw.shown;
        if (t->shown && t->canvas) {
            lv_obj_remove_flag(t->canvas, LV_OBJ_FLAG_HIDDEN);
            canvas_show(t->canvas, t->shown, t->bw, t->bh, true);
        }
    }
    memset(&A.vw, 0, sizeof A.vw);
    aos_hal_net_low_latency(false);
    if (A.tab == TAB_CAMS) for (int i = 0; i < A.ntiles; i++) tile_start(&A.tiles[i]);
    else for (int i = 0; i < A.nftiles; i++) tile_start(&A.ftiles[i]);
}

static void viewer_update(uint64_t now, bool stats)
{
    cam_view_t *v = A.vw.v;
    if (!v) {
        set_text(A.vw.msg, _("Sin memoria"));
        return;
    }
    const cam_frame_t *f = cam_view_take(v);
    if (f) {
        A.vw.shown = f;
        A.vw.frames++;
        lv_obj_remove_flag(A.vw.canvas, LV_OBJ_FLAG_HIDDEN);
        canvas_show(A.vw.canvas, f, A.W & ~1, A.H & ~1, A.mode == CAM_FILL);
    }
    if (v->state != A.vw.last_state || v->detail_gen != A.vw.last_detail) {
        A.vw.last_state = v->state;
        A.vw.last_detail = v->detail_gen;
        char msg[200];
        message_for(v, msg, sizeof msg);
        set_text(A.vw.msg, msg);
        lv_obj_set_flag(A.vw.msg, LV_OBJ_FLAG_HIDDEN, v->state == CAM_ST_LIVE && !v->undecodable && A.vw.shown);
    }
    if (A.vw.snapping && v->snap_result) {
        A.vw.snapping = false;
        lv_obj_set_style_text_color(A.vw.snap_g, AOS_C_TEXT, 0);
        aos_ui_toast(v->snap_result > 0 ? _("Foto guardada en Fotos > camaras") : _("No se pudo guardar la foto"), 2500);
    }
    if (A.vw.overlay && A.vw.overlay_until && now > A.vw.overlay_until && v->state == CAM_ST_LIVE) {
        overlay_show(false);
    }
    if (!stats) return;

    uint32_t dt = (uint32_t)(now - A.stats_t0);
    uint32_t pics = v->pictures - A.vw.pics0, bytes = v->bytes - A.vw.bytes0;
    uint32_t dec = v->dec_ms - A.vw.dec0, conv = v->conv_ms - A.vw.conv0;
    uint32_t drop = v->dropped - A.vw.drop0, err = v->errors - A.vw.err0, skips = v->skips - A.vw.skips0;
    A.vw.pics0 = v->pictures;
    A.vw.bytes0 = v->bytes;
    A.vw.dec0 = v->dec_ms;
    A.vw.conv0 = v->conv_ms;
    A.vw.drop0 = v->dropped;
    A.vw.err0 = v->errors;
    A.vw.skips0 = v->skips;
    uint32_t tenths = (uint32_t)(A.vw.frames * 10000ull / (dt ? dt : 1));
    uint32_t shown = A.vw.frames;
    A.vw.frames = 0;
    uint32_t kbps = (uint32_t)(bytes * 8ull / (dt ? dt : 1));
    if (v->state == CAM_ST_LIVE) {
        char fps[16];
        fmt_dec1(fps, sizeof fps, tenths);
        const char *codec = v->codec == CAM_CODEC_H264 ? "H.264" : "MJPEG";
        if (v->polled) {
            snprintf(A.vw.info_txt, sizeof A.vw.info_txt, "%s  ·  %d×%d", _("Foto JPEG"), v->src_w, v->src_h);
        } else {
            snprintf(A.vw.info_txt, sizeof A.vw.info_txt, "%s  ·  %d×%d  ·  %s fps  ·  %u kb/s%s",
                     codec, v->src_w, v->src_h, fps, (unsigned)kbps,
                     v->have_audio ? _("  ·  audio sin reproducir") : "");
        }
        set_text(A.vw.info, A.vw.info_txt);
    } else {
        set_text(A.vw.info, "");
    }
    if (++A.vw.log_n % LOG_EVERY == 0 && v->state == CAM_ST_LIVE) {
        aos_hal_log("camaras", "%s: %u shown/s, %u decoded (dec %u ms, scale %u ms avg), -%u dropped "
                    "(%u skips), %u errors, lag %d ms, %u kb/s, %dx%d -> %dx%d %s",
                    A.vw.cam.name, (unsigned)shown, (unsigned)pics,
                    pics ? (unsigned)(dec / pics) : 0, pics ? (unsigned)(conv / pics) : 0,
                    (unsigned)drop, (unsigned)skips, (unsigned)err, (int)v->lag_ms, (unsigned)kbps,
                    v->src_w, v->src_h, A.vw.shown ? A.vw.shown->w : 0, A.vw.shown ? A.vw.shown->h : 0,
                    v->codec == CAM_CODEC_H264 ? "H264" : "JPEG");
    }
}

/* ---- the timer and the life cycle -------------------------------------------- */

static void timer_cb(lv_timer_t *tm)
{
    (void)tm;
    if (A.closing) return;
    uint64_t now = aos_hal_uptime_ms();
    bool stats = now - A.stats_t0 >= STATS_MS;
    if (A.vw.leave) {
        close_viewer();
    }
    if (A.vw.on) {
        viewer_update(now, stats);
    } else {
        tile_t *ts = A.tab == TAB_CAMS ? A.tiles : A.ftiles;
        int n = A.tab == TAB_CAMS ? A.ntiles : A.nftiles;
        for (int i = 0; i < n; i++) {
            tile_update(&ts[i]);
            if (stats) tile_stats(&ts[i], (uint32_t)(now - A.stats_t0));
        }
    }
    if (stats) A.stats_t0 = now;
}

static void tick(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (A.closing) return;
    if (A.tab == TAB_FRIGATE && !A.vw.on) fg_sync();
    /* cameras.txt edited from the portal: again, while the mosaic shows */
    if (!A.vw.on && cam_cfg_stamp() != A.cfg_stamp) {
        stop_tab_tiles(TAB_CAMS);
        stop_tab_tiles(TAB_FRIGATE);
        A.nftiles = 0;
        if (A.fg) {
            cam_frigate_release(A.fg);
            A.fg = NULL;
        }
        cam_cfg_load(&A.cfg);
        A.cfg_stamp = cam_cfg_stamp();
        memset(A.tiles, 0, sizeof A.tiles);
        for (int i = 0; i < A.cfg.count; i++) A.tiles[i].cam = A.cfg.cams[i];
        A.nev = 0;
        build_ui();
    }
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (A.vw.on) {
        A.vw.leave = true;              /* on the next timer call, not inside the gesture */
        return true;
    }
    return false;
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    A.root = root;
    bool viewing = A.vw.on;
    build_ui();                         /* the tiles keep their connections */
    if (viewing) {
        for (int i = 0; i < A.ntiles; i++) if (&A.tiles[i] != A.vw.tile) tile_stop(&A.tiles[i]);
        for (int i = 0; i < A.nftiles; i++) if (&A.ftiles[i] != A.vw.tile) tile_stop(&A.ftiles[i]);
        build_viewer();
        view_retarget();
    }
    return true;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&A, 0, sizeof A);
    A.root = root;
    A.rot_restore = -1;
    cam_conv_init();
    int32_t v;
    A.mode = aos_hal_pref_get_i32(KEY_MODE, &v) && v == CAM_FILL ? CAM_FILL : CAM_FIT;
    A.tab = aos_hal_pref_get_i32(KEY_TAB, &v) && v == TAB_FRIGATE ? TAB_FRIGATE : TAB_CAMS;
    cam_cfg_load(&A.cfg);
    A.cfg_stamp = cam_cfg_stamp();
    for (int i = 0; i < A.cfg.count; i++) A.tiles[i].cam = A.cfg.cams[i];
    build_ui();
    A.stats_t0 = aos_hal_uptime_ms();
    A.timer = lv_timer_create(timer_cb, TIMER_MS, NULL);

    /* Development: CAM_OPEN=<n> opens the n-th camera of the tab straight away. */
    const char *open = getenv("CAM_OPEN");
    if (open && open[0] >= '0' && open[0] <= '9') {
        open_viewer(A.tab == TAB_FRIGATE ? SRC_FRIGATE : SRC_CAMS, open[0] - '0');
    }
    return &A;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    A.closing = true;
    if (A.timer) {
        lv_timer_delete(A.timer);
        A.timer = NULL;
    }
    lv_obj_clean(A.root);               /* the canvases go before their frames */
    free_rows();
    for (int i = 0; i < CAM_MAX; i++) A.tiles[i].box = NULL;
    for (int i = 0; i < FG_MAX_CAMS; i++) A.ftiles[i].box = NULL;
    if (A.vw.on) {
        if (A.vw.own) cam_view_release(A.vw.v);
        aos_hal_net_low_latency(false);
    }
    memset(&A.vw, 0, sizeof A.vw);
    stop_tab_tiles(TAB_CAMS);
    stop_tab_tiles(TAB_FRIGATE);
    if (A.fg) {
        cam_frigate_release(A.fg);
        A.fg = NULL;
    }
    /* The threads run this app's code: wait for them to leave it. A thread
     * inside a connect takes up to its timeout (5 s). */
    uint64_t t0 = aos_hal_uptime_ms();
    while (cam_view_live() > 0 && aos_hal_uptime_ms() - t0 < 6000) {
        aos_hal_sleep_ms(20);
    }
    if (cam_view_live() > 0) {
        aos_hal_log("camaras", "%d camera threads still running on exit", cam_view_live());
    }
    if (A.rot_restore >= 0 && aos_ui_landscape() != (bool)A.rot_restore) {
        aos_ui_request_landscape(A.rot_restore);
    }
}

static bool camaras_init(aos_app_t *app)
{
    app->desc.id       = "aos.cameras";
    app->desc.name     = "Cámaras";
    app->desc.icon     = AOS_SYM_CCTV;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x334155;
    app->desc.color_b  = 0x0F172A;
    app->desc.order    = 125;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN;
    app->create  = create;
    app->destroy = destroy;
    app->back    = back;
    app->tick    = tick;
    app->resize  = resize;
    return true;
}

AOS_APP_ENTRY(camaras_init);
