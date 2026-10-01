/*
 * P4OS - Retro canvas ("lienzo retro"): the OS service of recipe C
 *
 * The game draws a small RGB565 canvas; this file shows it scaled by an
 * integer factor and draws and reads the on-screen controls. docs/RETRO.md
 * has the contract and the measurements; aos_retro.h the API.
 *
 * How the scale gets to the screen without extra copies:
 *
 *   The canvas is shown by an ordinary LVGL object (the "view") whose
 *   DRAW_MAIN event adds ONE image draw task: the canvas as an
 *   lv_image_dsc_t, scale x k, pivot at the corner, no antialiasing. A draw
 *   unit of our own claims exactly that task (the source pointer is ours) and
 *   writes the scaled pixels straight into the draw buffer LVGL is rendering,
 *   only over the area being refreshed:
 *
 *     - the part of that area aligned to whole canvas pixels goes to
 *       aos_hal_retro_scale(): the PPA's SRM on the board (aos_retro_p4.c),
 *       software in the simulator (sim/retro_sim.c);
 *     - the ragged edges (up to k-1 rows or columns on each side, where a
 *       refresh chunk cuts a canvas pixel in two) and anything the HAL
 *       declines go through the CPU below, nearest neighbour.
 *
 *   So there is no scaled copy anywhere: the game draws its small canvas,
 *   the scaled pixels exist only in the draw buffer being rendered, LVGL's
 *   usual flush takes them to the frame buffer, and the app's LVGL panels
 *   compose on top as usual. The view also answers COVER_CHECK, so LVGL does
 *   not paint the background under it first.
 *
 *   If our unit declines (a layer that is not RGB565, say), LVGL's software
 *   unit draws the same task with its own transform code: slower, same
 *   pixels.
 *
 * The controls are LVGL objects too (drawn by the OS, in the theme), but they
 * are NOT read through LVGL's pointer, which knows one finger: they are
 * hit-tested against the HAL's raw touch frames (aos_hal_touch_frames), so a
 * thumb on the arrows and another on A both count.
 */
#include "aos_retro.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_sys_glyphs.h"

#include "lvgl_private.h"

#include <stdlib.h>
#include <string.h>

#define TAG "retro"

/* Not used by any other draw unit of LVGL 9.5 (sw 1 ... nanovg 10, ppa 80,
 * sdl 100). */
#define RETRO_UNIT_ID       77

#define CTRL_MIN_PAD        420     /* room the arrows and A/B need below the game */
#define CTRL_MIN_SMALL      150     /* ... and a pause row alone                  */
#define SIDE_LAND           300     /* landscape: width of each control column    */
#define HIT_PAD             20      /* touch targets are this much bigger          */
#define MAX_TOP             40      /* top margin of the canvas, portrait         */
/* LR_SPLIT: the buttons Claude Jump was tuned with (its own until 2026-09-30) */
#define SPLIT_W             208     /* upright: each button's width at most        */
#define SPLIT_H_TALL        240     /* ... and height                              */
#define SPLIT_H_WIDE        300     /* lying down: the height in its column        */
#define SPLIT_GAP           16      /* from the screen's edges                     */
#define SPLIT_PAD           24      /* their touch target is this much bigger      */

typedef struct {
    int16_t x1, y1, x2, y2;         /* root coords, inclusive */
} rect_t;

/* HIT_SHOW: drawn and lit, but hit-tested by someone else (the d-pad's
 * arrows, which the pad's disc reads as one) */
typedef enum { HIT_RECT = 0, HIT_DISC, HIT_DPAD, HIT_SHOW } hit_kind_t;

typedef struct {
    uint32_t    bit;                /* AOS_RETRO_BTN_*, or 0 for the d-pad      */
    hit_kind_t  kind;
    rect_t      r;                  /* the visual; the hit adds 'pad'            */
    int16_t     pad;                /* HIT_PAD, or SPLIT_PAD                     */
    lv_obj_t   *obj;
    lv_obj_t   *caption;
} ctl_t;

#define MAX_CTL 12

typedef struct {
    bool          active;
    aos_retro_t   pub;
    uint32_t      flags;
    bool          land;             /* laid out lying down */
    lv_image_dsc_t img;
    lv_obj_t     *root;
    int16_t       root_x, root_y;   /* root's screen position (touch frames are screen) */

    /* controls */
    lv_obj_t     *ctl_box;
    ctl_t         ctl[MAX_CTL];
    int           nctl;
    ctl_t        *dpad;
    rect_t        slider;
    bool          has_slider;
    lv_obj_t     *slider_obj, *slider_thumb;
    bool          controls_shown;
    bool          touch_off;        /* show_controls(false): a panel covers the game */

    /* input */
    uint32_t      seq;
    uint32_t      held, edges, rel_edges, shown_bits;
    bool          touch_on;
    uint8_t       prev_count;       /* fingers in the previous sample */
    int16_t       prev_x[2], prev_y[2]; /* ... and where they were (root coords) */
    int16_t       tx, ty;
    bool          tap_pending;
    int16_t       tap_x, tap_y;
    lv_timer_t   *poll_timer;

    /* pacing */
    lv_timer_t   *pace;
    aos_retro_fn_t step, draw;
    void         *user;
    int           fps;
    uint64_t      t0;
    uint32_t      ticks;
    bool          paused;
    uint32_t      gen;              /* bumped by end()/run(): a callback that ended us sees it */
    uint64_t      fps_t;
    uint32_t      fps_n;

    /* stats */
    aos_retro_stats_t st;
    uint64_t      acc_us;
    uint32_t      acc_px;
    uint8_t       acc_hw;
    bool          log_stats;
    bool          full;             /* AOS_RETRO_FULL: every present is the whole canvas */
    bool          verify;           /* AOS_RETRO_VERIFY (simulator): check every pixel   */
    bool          lvgl_only;        /* AOS_RETRO_LVGL: leave the scale to LVGL's own code */
    uint32_t      verify_px, verify_bad, verify_areas;
} retro_t;

static retro_t s;
static lv_draw_unit_t *s_unit;

static void stats_roll(void);

/* -------------------------------------------------------------------------- */
/* The CPU scaler: nearest neighbour, for the edges and as the fallback        */
/* -------------------------------------------------------------------------- */

/* Fills the screen rows [ry1, ry2] and columns [rx1, rx2], given in SCALED
 * canvas coordinates (0 .. w*k-1), at 'dst' (which points at (rx1, ry1)). */
static void cpu_scale(const uint16_t *src, int sw, int k,
                      int rx1, int ry1, int rx2, int ry2,
                      uint16_t *dst, int stride_px)
{
    int n = rx2 - rx1 + 1;
    if (n <= 0 || ry2 < ry1) return;
    for (int ry = ry1; ry <= ry2; ry++, dst += stride_px) {
        /* a row of the same canvas row as the one above: copy it */
        if (ry > ry1 && ry / k == (ry - 1) / k) {
            memcpy(dst, dst - stride_px, (size_t)n * 2);
            continue;
        }
        const uint16_t *srow = src + (size_t)(ry / k) * sw;
        int x = rx1, i = 0;
        int sx = x / k;
        int run = k - x % k;
        while (i < n) {
            uint16_t c = srow[sx++];
            if (run > n - i) run = n - i;
            switch (run) {                /* k is 2..6 in practice */
            case 3: dst[i + 2] = c; /* fallthrough */
            case 2: dst[i + 1] = c; /* fallthrough */
            case 1: dst[i] = c; break;
            default:
                for (int j = 0; j < run; j++) dst[i + j] = c;
            }
            i += run;
            run = k;
        }
    }
}

/* Scales the screen area 'a' (already inside the canvas's scaled area and the
 * buffer) into the layer's buffer. Returns true if the HAL did the bulk. */
static bool scale_area(const lv_area_t *a, int ox, int oy, lv_layer_t *layer)
{
    lv_draw_buf_t *buf = layer->draw_buf;
    const int k = s.pub.scale, sw = s.pub.w, sh = s.pub.h;
    const uint16_t *src = s.pub.px;
    const int stride = (int)(buf->header.stride / 2);
    const int bx = layer->buf_area.x1, by = layer->buf_area.y1;
    uint16_t *base = (uint16_t *)buf->data;
#define DST(sx_, sy_) (base + (size_t)((sy_) - by) * stride + ((sx_) - bx))

    /* in scaled canvas coordinates, inclusive */
    int rx1 = a->x1 - ox, ry1 = a->y1 - oy, rx2 = a->x2 - ox, ry2 = a->y2 - oy;

    /* the aligned core, exclusive ends */
    int cx1 = (rx1 + k - 1) / k * k, cy1 = (ry1 + k - 1) / k * k;
    int cx2 = (rx2 + 1) / k * k, cy2 = (ry2 + 1) / k * k;
    bool hw = false;

    if (cx2 > cx1 && cy2 > cy1) {
        hw = aos_hal_retro_scale(src, sw, sh, cx1 / k, cy1 / k, (cx2 - cx1) / k, (cy2 - cy1) / k, k,
                                 base, buf->data_size, stride, (int)buf->header.h,
                                 ox + cx1 - bx, oy + cy1 - by);
        if (!hw) cpu_scale(src, sw, k, cx1, cy1, cx2 - 1, cy2 - 1, DST(ox + cx1, oy + cy1), stride);
        /* the ragged frame around the core */
        if (cy1 > ry1) cpu_scale(src, sw, k, rx1, ry1, rx2, cy1 - 1, DST(a->x1, a->y1), stride);
        if (ry2 >= cy2) cpu_scale(src, sw, k, rx1, cy2, rx2, ry2, DST(a->x1, oy + cy2), stride);
        if (cx1 > rx1) cpu_scale(src, sw, k, rx1, cy1, cx1 - 1, cy2 - 1, DST(a->x1, oy + cy1), stride);
        if (rx2 >= cx2) cpu_scale(src, sw, k, cx2, cy1, rx2, cy2 - 1, DST(ox + cx2, oy + cy1), stride);
    } else {
        cpu_scale(src, sw, k, rx1, ry1, rx2, ry2, DST(a->x1, a->y1), stride);
    }
#undef DST
    return hw;
}

/* -------------------------------------------------------------------------- */
/* The draw unit                                                               */
/* -------------------------------------------------------------------------- */

static int32_t unit_evaluate(lv_draw_unit_t *u, lv_draw_task_t *t)
{
    (void)u;
    if (!s.active || s.lvgl_only || t->type != LV_DRAW_TASK_TYPE_IMAGE) return 0;
    const lv_draw_image_dsc_t *d = (const lv_draw_image_dsc_t *)t->draw_dsc;
    if (d->src != &s.img) return 0;
    const lv_layer_t *layer = d->base.layer;
    if (!layer || layer->color_format != LV_COLOR_FORMAT_RGB565) return 0;
    if (d->rotation != 0 || d->skew_x || d->skew_y || d->scale_x != 256 * s.pub.scale ||
        d->scale_y != 256 * s.pub.scale || d->opa < LV_OPA_MAX || d->recolor_opa > LV_OPA_MIN ||
        d->bitmap_mask_src || d->clip_radius || d->blend_mode != LV_BLEND_MODE_NORMAL ||
        t->opa < LV_OPA_MAX) return 0;
    t->preference_score = 0;
    t->preferred_draw_unit_id = RETRO_UNIT_ID;
    return 1;
}

static void unit_execute(lv_draw_task_t *t)
{
    lv_layer_t *layer = t->target_layer;
    lv_area_t view = {
        t->area.x1, t->area.y1,
        t->area.x1 + s.pub.w * s.pub.scale - 1, t->area.y1 + s.pub.h * s.pub.scale - 1,
    };
    lv_area_t a;
    if (!lv_area_intersect(&a, &view, &t->clip_area)) return;
    if (!lv_area_intersect(&a, &a, &layer->buf_area)) return;

    uint64_t t0 = aos_hal_uptime_us();
    bool hw = scale_area(&a, view.x1, view.y1, layer);
    s.acc_us += aos_hal_uptime_us() - t0;
    s.acc_px += (uint32_t)lv_area_get_size(&a);
    if (hw) s.acc_hw = 1;

#ifdef AOS_SIM
    /* AOS_RETRO_VERIFY=1: every pixel just written against the canvas pixel
     * it must come from. It checks the split into block and ragged edges
     * that the board also does (the simulator's HAL scaler writes the block
     * like the PPA, in one piece). Not with P4_SIM_RETRO_BILINEAR. */
    if (s.verify) {
        lv_draw_buf_t *buf = layer->draw_buf;
        const int stride = (int)(buf->header.stride / 2), k = s.pub.scale;
        uint32_t bad = 0;
        for (int y = a.y1; y <= a.y2; y++) {
            const uint16_t *d = (const uint16_t *)buf->data + (size_t)(y - layer->buf_area.y1) * stride;
            const uint16_t *src = s.pub.px + (size_t)((y - view.y1) / k) * s.pub.w;
            for (int x = a.x1; x <= a.x2; x++) {
                if (d[x - layer->buf_area.x1] != src[(x - view.x1) / k]) bad++;
            }
        }
        s.verify_px += (uint32_t)lv_area_get_size(&a);
        s.verify_areas++;
        if (bad) {
            s.verify_bad += bad;
            aos_hal_log(TAG, "VERIFY: %u wrong pixels in %d,%d..%d,%d (buffer %d,%d..%d,%d)",
                        (unsigned)bad, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2,
                        (int)layer->buf_area.x1, (int)layer->buf_area.y1,
                        (int)layer->buf_area.x2, (int)layer->buf_area.y2);
        }
    }
#endif
}

static int32_t unit_dispatch(lv_draw_unit_t *u, lv_layer_t *layer)
{
    lv_draw_task_t *t = lv_draw_get_available_task(layer, NULL, RETRO_UNIT_ID);
    if (!t || t->preferred_draw_unit_id != RETRO_UNIT_ID) return LV_DRAW_UNIT_IDLE;

    /* Alone on the layer, or not at all. The PPA writes the buffer by DMA and
     * the driver invalidates the cache over WHOLE rows of it: a software
     * draw thread painting the same rows beside the canvas at that moment
     * (the board has two) could lose what it wrote. LVGL asks us again as
     * soon as any task finishes. */
    for (lv_draw_task_t *o = layer->draw_task_head; o; o = o->next) {
        if (o != t && o->state == LV_DRAW_TASK_STATE_IN_PROGRESS) return LV_DRAW_UNIT_IDLE;
    }
    if (!lv_draw_layer_alloc_buf(layer)) return LV_DRAW_UNIT_IDLE;

    t->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    t->draw_unit = u;
    unit_execute(t);
    t->state = LV_DRAW_TASK_STATE_FINISHED;
    lv_draw_dispatch_request();
    return 1;
}

/* Registered the first time a game opens and kept: LVGL has no way to take a
 * draw unit out, and without an active canvas it claims nothing. */
static void unit_ensure(void)
{
    if (s_unit) return;
    s_unit = lv_draw_create_unit(sizeof(lv_draw_unit_t));
    s_unit->evaluate_cb = unit_evaluate;
    s_unit->dispatch_cb = unit_dispatch;
    s_unit->name = "AOS_RETRO";
}

/* -------------------------------------------------------------------------- */
/* The view                                                                    */
/* -------------------------------------------------------------------------- */

static void view_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_current_target_obj(e);

    if (code == LV_EVENT_DRAW_MAIN) {
        if (!s.active) return;
        lv_area_t co;
        lv_obj_get_coords(obj, &co);
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = &s.img;
        d.scale_x = d.scale_y = 256 * s.pub.scale;
        d.pivot.x = d.pivot.y = 0;
        d.antialias = 0;
        lv_area_t ia = { co.x1, co.y1, co.x1 + s.pub.w - 1, co.y1 + s.pub.h - 1 };
        lv_draw_image(lv_event_get_layer(e), &d, &ia);
    } else if (code == LV_EVENT_COVER_CHECK) {
        /* written directly: lv_event_set_cover_res() only ever makes the
         * answer weaker, and the base class already said NOT_COVER for an
         * object without a background */
        lv_cover_check_info_t *info = (lv_cover_check_info_t *)lv_event_get_param(e);
        if (!s.active || info->res == LV_COVER_RES_MASKED) return;
        lv_area_t co;
        lv_obj_get_coords(obj, &co);
        info->res = lv_area_is_in(info->area, &co, 0) ? LV_COVER_RES_COVER : LV_COVER_RES_NOT_COVER;
    } else if (code == LV_EVENT_DELETE) {
        if (!s.active) return;      /* aos_retro_end() deleting it */
        /* the root went away with us inside: end without touching objects */
        s.pub.view = NULL;
        s.ctl_box = NULL;
        s.pub.controls = NULL;
        aos_retro_end();
    }
}

static void ctlbox_deleted(lv_event_t *e)
{
    (void)e;
    s.ctl_box = NULL;
    s.pub.controls = NULL;
    s.nctl = 0;
    s.dpad = NULL;
    s.slider_obj = s.slider_thumb = NULL;
}

/* -------------------------------------------------------------------------- */
/* Controls: drawing                                                           */
/* -------------------------------------------------------------------------- */

/* The controls float over the game (OVERLAY, upright): translucent. Lying
 * down they sit in the columns beside it, on the border, and are solid. */
static bool overlay(void) { return (s.flags & AOS_RETRO_OVERLAY) && !s.land; }

static lv_obj_t *btn_base(int x, int y, int w, int h, int radius)
{
    lv_obj_t *b = lv_obj_create(s.ctl_box);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, overlay() ? LV_OPA_40 : LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x48484A), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_opa(b, overlay() ? LV_OPA_50 : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, AOS_C_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, overlay() ? LV_OPA_70 : LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, AOS_C_ACCENT, LV_STATE_PRESSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

static lv_obj_t *glyph(lv_obj_t *parent, const char *sym, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
    lv_obj_center(l);
    return l;
}

/* Box position relative to the control container. */
static int bx0, by0;

static ctl_t *add_ctl(uint32_t bit, hit_kind_t kind, int x, int y, int w, int h, int radius)
{
    if (s.nctl >= MAX_CTL) return NULL;
    ctl_t *c = &s.ctl[s.nctl++];
    memset(c, 0, sizeof *c);
    c->bit = bit;
    c->kind = kind;
    c->pad = HIT_PAD;
    c->r = (rect_t){ (int16_t)(bx0 + x), (int16_t)(by0 + y),
                     (int16_t)(bx0 + x + w - 1), (int16_t)(by0 + y + h - 1) };
    c->obj = btn_base(x, y, w, h, radius);
    return c;
}

static void add_arrow(uint32_t bit, const char *sym, int x, int y, int w, int h, const lv_font_t *f)
{
    ctl_t *c = add_ctl(bit, HIT_RECT, x, y, w, h, 28);
    if (c) glyph(c->obj, sym, f);
}

/* LR_SPLIT: one per thumb, chunkier than the pad's (a small radius and a
 * thick border, closer to the pixels behind), as Claude Jump drew its own. */
static void add_split_arrow(uint32_t bit, const char *sym, int x, int y, int w, int h)
{
    ctl_t *c = add_ctl(bit, HIT_RECT, x, y, w, h, 18);
    if (!c) return;
    c->pad = SPLIT_PAD;
    lv_obj_set_style_border_width(c->obj, 3, 0);
    if (overlay()) lv_obj_set_style_border_opa(c->obj, LV_OPA_60, 0);
    lv_obj_t *l = glyph(c->obj, sym, &aos_sym_72);
    if (overlay()) lv_obj_set_style_text_opa(l, LV_OPA_80, 0);
}

static void add_pad_arrow(uint32_t bit, const char *sym, int x, int y, int cell)
{
    ctl_t *c = add_ctl(bit, HIT_SHOW, x, y, cell, cell, 28);
    if (c) glyph(c->obj, sym, &aos_sym_44);
}

static void add_action(uint32_t bit, const char *letter, int cx, int cy, int d)
{
    ctl_t *c = add_ctl(bit, HIT_DISC, cx - d / 2, cy - d / 2, d, d, LV_RADIUS_CIRCLE);
    if (!c) return;
    lv_obj_t *l = glyph(c->obj, letter, aos_font_large);
    (void)l;
    lv_obj_t *cap = lv_label_create(s.ctl_box);
    lv_label_set_text(cap, "");
    lv_obj_set_style_text_font(cap, aos_font_small, 0);
    lv_obj_set_style_text_color(cap, AOS_C_DIM, 0);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(cap, d + 80);
    lv_obj_set_pos(cap, cx - (d + 80) / 2, cy + d / 2 + 8);
    lv_label_set_long_mode(cap, LV_LABEL_LONG_MODE_DOTS);
    c->caption = cap;
}

/* The d-pad: one disc-shaped touch target, four drawn buttons. */
static void add_dpad(int cx, int cy, int cell)
{
    int half = cell / 2;
    add_pad_arrow(AOS_RETRO_BTN_UP,    AOS_SYM_ARROW_UP_BOLD,    cx - half, cy - half - cell, cell);
    add_pad_arrow(AOS_RETRO_BTN_DOWN,  AOS_SYM_ARROW_DOWN_BOLD,  cx - half, cy + half,        cell);
    add_pad_arrow(AOS_RETRO_BTN_LEFT,  AOS_SYM_ARROW_LEFT_BOLD,  cx - half - cell, cy - half, cell);
    add_pad_arrow(AOS_RETRO_BTN_RIGHT, AOS_SYM_ARROW_RIGHT_BOLD, cx + half, cy - half,        cell);
    /* the hub, drawn only */
    lv_obj_t *hub = btn_base(cx - half, cy - half, cell, cell, 12);
    lv_obj_set_style_border_width(hub, 0, 0);
    if (s.nctl < MAX_CTL) {
        ctl_t *c = &s.ctl[s.nctl++];
        memset(c, 0, sizeof *c);
        c->kind = HIT_DPAD;
        c->pad = HIT_PAD;
        c->r = (rect_t){ (int16_t)(bx0 + cx - half - cell), (int16_t)(by0 + cy - half - cell),
                         (int16_t)(bx0 + cx + half + cell - 1), (int16_t)(by0 + cy + half + cell - 1) };
        s.dpad = c;
    }
}

static void add_pause(int cx, int cy)
{
    ctl_t *c = add_ctl(AOS_RETRO_BTN_PAUSE, HIT_RECT, cx - 64, cy - 36, 128, 72, 36);
    if (c) glyph(c->obj, AOS_SYM_PAUSE_CIRCLE, &aos_sym_44);
}

static void add_slider(int x, int y, int w, int h)
{
    lv_obj_t *t = btn_base(x, y, w, h, 28);
    lv_obj_set_style_bg_color(t, AOS_C_CARD, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(0x3A3A3C), 0);
    s.slider_obj = t;
    s.slider = (rect_t){ (int16_t)(bx0 + x), (int16_t)(by0 + y), (int16_t)(bx0 + x + w - 1), (int16_t)(by0 + y + h - 1) };
    s.has_slider = true;

    lv_obj_t *l = glyph(t, AOS_SYM_ARROW_LEFT_BOLD, &aos_sym_28);
    lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_t *r = glyph(t, AOS_SYM_ARROW_RIGHT_BOLD, &aos_sym_28);
    lv_obj_set_style_text_color(r, AOS_C_DIM, 0);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, -24, 0);
    lv_obj_t *hint = lv_label_create(t);
    lv_label_set_text(hint, _("Deslizá el dedo"));
    lv_obj_set_style_text_font(hint, aos_font_caption, 0);
    lv_obj_set_style_text_color(hint, AOS_C_DIM, 0);
    lv_obj_center(hint);

    /* the thumb: follows the finger, only while there is one */
    lv_obj_t *th = lv_obj_create(t);
    lv_obj_remove_style_all(th);
    lv_obj_set_size(th, 72, h - 24);
    lv_obj_set_style_radius(th, 20, 0);
    lv_obj_set_style_bg_color(th, AOS_C_ACCENT, 0);
    lv_obj_set_style_bg_opa(th, LV_OPA_80, 0);
    lv_obj_remove_flag(th, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(th, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(th, 12);
    s.slider_thumb = th;
}

/* The control container covers rect 'c' (root coords); everything inside is
 * laid out relative to it. Portrait: controls below the game, or floating
 * over its bottom (OVERLAY). */
static void layout_portrait(rect_t c)
{
    int W = c.x2 - c.x1 + 1, H = c.y2 - c.y1 + 1;
    uint32_t f = s.flags;
    bool split = (f & AOS_RETRO_LR_SPLIT) && !(f & AOS_RETRO_DPAD);
    bool left = f & (AOS_RETRO_DPAD | AOS_RETRO_LR), right = f & (AOS_RETRO_A | AOS_RETRO_B);
    int y = 16, bottom = H - 48;   /* the bottom 36 px start the home swipe */
    int rcx = W * 73 / 100;         /* A/B: the right thumb's, whatever is on the left */

    if (split) {
        /* the bottom corners, either side of the pause pill */
        int avail = bottom - y - ((f & AOS_RETRO_SLIDER) ? 96 + 24 : 0);
        int h = SPLIT_H_TALL;
        if (right && h > avail * 55 / 100) h = avail * 55 / 100;
        if (h > avail) h = avail;
        int mid = (f & AOS_RETRO_PAUSE) ? 128 + 2 * SPLIT_GAP : SPLIT_GAP;
        int w = (W - 2 * SPLIT_GAP - mid) / 2;
        if (w > SPLIT_W) w = SPLIT_W;
        if (h >= 96) {
            add_split_arrow(AOS_RETRO_BTN_LEFT,  AOS_SYM_ARROW_LEFT_BOLD,  SPLIT_GAP, bottom - h, w, h);
            add_split_arrow(AOS_RETRO_BTN_RIGHT, AOS_SYM_ARROW_RIGHT_BOLD, W - SPLIT_GAP - w, bottom - h, w, h);
        }
        if (f & AOS_RETRO_PAUSE) add_pause(W / 2, bottom - 36);
        rcx = W - SPLIT_GAP - w / 2;    /* A/B above RIGHT */
        bottom -= h + 16;
        left = false;                   /* nothing else on the left */
    } else if (f & AOS_RETRO_PAUSE) {
        /* pause: the bottom row, centred, like START */
        add_pause(W / 2, bottom - 36);
        bottom -= 72 + 16;
    }
    if (f & AOS_RETRO_SLIDER) {
        int sh = (bottom - y) * 42 / 100;
        if (!left && !right) sh = bottom - y;
        if (sh > 220) sh = 220;
        if (sh < 96) sh = 96;
        /* as wide as the canvas: the thumb sits under the column it moves to */
        int sx = s.pub.x - c.x1, sw = s.pub.w * s.pub.scale;
        if (sx < 16) { sw -= 16 - sx; sx = 16; }
        if (sx + sw > W - 16) sw = W - 16 - sx;
        add_slider(sx, y, sw, sh);
        y += sh + 24;
    }
    int bh = bottom - y;
    int cy = y + bh / 2;
    if (bh < 120) return;

    if ((f & AOS_RETRO_LR) && !(f & AOS_RETRO_DPAD) && !right && !split) {
        /* only left / right: the two halves, big, but not taller than a
         * thumb needs */
        int w = (W - 16 * 3) / 2, h = bh > 320 ? 320 : bh;
        int yy = y + (bh - h) / 2;
        add_arrow(AOS_RETRO_BTN_LEFT,  AOS_SYM_ARROW_LEFT_BOLD,  16,         yy, w, h, &aos_sym_72);
        add_arrow(AOS_RETRO_BTN_RIGHT, AOS_SYM_ARROW_RIGHT_BOLD, 16 * 2 + w, yy, w, h, &aos_sym_72);
        return;
    }
    int lcx = right ? W * 27 / 100 : W / 2;
    if (f & AOS_RETRO_DPAD) {
        int cell = bh / 3;
        if (cell > 124) cell = 124;
        add_dpad(lcx, cy, cell);
    } else if ((f & AOS_RETRO_LR) && !split) {
        int w = left && right ? 150 : 200, h = bh - 40 < 240 ? bh - 40 : 240;
        add_arrow(AOS_RETRO_BTN_LEFT,  AOS_SYM_ARROW_LEFT_BOLD,  lcx - w - 8, cy - h / 2, w, h, &aos_sym_72);
        add_arrow(AOS_RETRO_BTN_RIGHT, AOS_SYM_ARROW_RIGHT_BOLD, lcx + 8,     cy - h / 2, w, h, &aos_sym_72);
    }
    if ((f & AOS_RETRO_A) && (f & AOS_RETRO_B)) {
        int d = bh * 45 / 100;
        if (d > 150) d = 150;
        /* the pair diagonal, B lower left, against the right edge; beside a
         * d-pad a little smaller, so B's target stays clear of the pad's */
        if ((f & AOS_RETRO_DPAD) && d > 130) d = 130;
        if (!split) rcx = W - 16 - d * 55 / 100 - d / 2;
        else if (rcx + d * 55 / 100 + d / 2 > W - 16) rcx = W - 16 - d * 55 / 100 - d / 2;
        add_action(AOS_RETRO_BTN_B, "B", rcx - d * 55 / 100, cy + d * 30 / 100 - 20, d);
        add_action(AOS_RETRO_BTN_A, "A", rcx + d * 55 / 100, cy - d * 30 / 100 - 20, d);
    } else if (f & (AOS_RETRO_A | AOS_RETRO_B)) {
        int d = bh * 70 / 100;
        if (d > 180) d = 180;
        if (rcx + d / 2 > W - 16) rcx = W - 16 - d / 2;
        bool a = f & AOS_RETRO_A;
        add_action(a ? AOS_RETRO_BTN_A : AOS_RETRO_BTN_B, a ? "A" : "B", rcx, cy - 20, d);
    }
}

/* Landscape: a column on each side of the game. */
static void layout_landscape(rect_t c)
{
    int W = c.x2 - c.x1 + 1, H = c.y2 - c.y1 + 1;
    uint32_t f = s.flags;
    int gx1 = s.pub.x - c.x1, gx2 = gx1 + s.pub.w * s.pub.scale;
    int lw = gx1, rw = W - gx2;
    int lcx = lw / 2, rcx = gx2 + rw / 2, cy = H / 2;
    int ab_top = 0, ab_bottom = H;  /* the band A/B go in, right column */

    if (f & AOS_RETRO_PAUSE) {
        add_pause(rcx, 60);
        ab_top = 60 + 36 + 16;
    }
    if (f & AOS_RETRO_DPAD) {
        int cell = lw / 3 - 8;
        if (cell > 124) cell = 124;
        add_dpad(lcx, cy, cell);
    } else if (f & AOS_RETRO_LR_SPLIT) {
        /* one low in each column, where the thumbs rest */
        int h = SPLIT_H_WIDE;
        if ((f & (AOS_RETRO_A | AOS_RETRO_B)) && h > H * 40 / 100) h = H * 40 / 100;
        int yb = H - 40 - h;
        int wl = lw - 2 * SPLIT_GAP - 16, wr = rw - 2 * SPLIT_GAP - 16;
        if (wl >= 64) add_split_arrow(AOS_RETRO_BTN_LEFT, AOS_SYM_ARROW_LEFT_BOLD, (lw - wl) / 2, yb, wl, h);
        if (wr >= 64) add_split_arrow(AOS_RETRO_BTN_RIGHT, AOS_SYM_ARROW_RIGHT_BOLD, gx2 + (rw - wr) / 2, yb, wr, h);
        ab_bottom = yb - 16;
    } else if (f & AOS_RETRO_LR) {
        int w = lw / 2 - 24;
        add_arrow(AOS_RETRO_BTN_LEFT,  AOS_SYM_ARROW_LEFT_BOLD,  lcx - w - 8, cy - 120, w, 240, &aos_sym_72);
        add_arrow(AOS_RETRO_BTN_RIGHT, AOS_SYM_ARROW_RIGHT_BOLD, lcx + 8,     cy - 120, w, 240, &aos_sym_72);
    }
    int band = ab_bottom - ab_top;
    int acy = ab_top + band / 2;
    int d = rw - 60;
    if (d > 150) d = 150;
    if ((f & AOS_RETRO_A) && (f & AOS_RETRO_B)) {
        if (d > (band - 80) / 2) d = (band - 80) / 2;
        if (d >= 48) {
            add_action(AOS_RETRO_BTN_A, "A", rcx, acy - d / 2 - 30, d);
            add_action(AOS_RETRO_BTN_B, "B", rcx, acy + d / 2 + 10, d);
        }
    } else if (f & (AOS_RETRO_A | AOS_RETRO_B)) {
        if (d > band - 60) d = band - 60;
        if (d >= 48) {
            add_action((f & AOS_RETRO_A) ? AOS_RETRO_BTN_A : AOS_RETRO_BTN_B,
                       (f & AOS_RETRO_A) ? "A" : "B", rcx, acy, d);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Controls: reading                                                           */
/* -------------------------------------------------------------------------- */

static bool in_rect(const rect_t *r, int x, int y, int pad)
{
    return x >= r->x1 - pad && x <= r->x2 + pad && y >= r->y1 - pad && y <= r->y2 + pad;
}

/* The buttons under (x, y), root coords. *over says whether the point is on
 * any control's touch target at all, the d-pad's dead zone included: such
 * a finger is that control's, never the canvas's. */
static uint32_t hit(int x, int y, bool *over)
{
    uint32_t bits = 0;
    bool on = false;
    for (int i = 0; i < s.nctl; i++) {
        const ctl_t *c = &s.ctl[i];
        if (c->kind == HIT_RECT) {
            if (c->bit && in_rect(&c->r, x, y, c->pad)) {
                bits |= c->bit;
                on = true;
            }
        } else if (c->kind == HIT_DISC) {
            int cx = (c->r.x1 + c->r.x2) / 2, cy = (c->r.y1 + c->r.y2) / 2;
            int r = (c->r.x2 - c->r.x1) / 2 + c->pad;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r) {
                bits |= c->bit;
                on = true;
            }
        } else if (c->kind == HIT_DPAD) {
            /* like a real pad: the direction from the centre, diagonals
             * included, a small dead zone in the middle */
            int cx = (c->r.x1 + c->r.x2) / 2, cy = (c->r.y1 + c->r.y2) / 2;
            int r = (c->r.x2 - c->r.x1) / 2 + c->pad;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r) continue;
            on = true;
            int dead = (c->r.x2 - c->r.x1) / 12;
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            if (ax < dead && ay < dead) continue;
            /* within ~27 degrees of an axis it is that axis alone */
            if (ax * 2 >= ay) bits |= dx < 0 ? AOS_RETRO_BTN_LEFT : AOS_RETRO_BTN_RIGHT;
            if (ay * 2 >= ax) bits |= dy < 0 ? AOS_RETRO_BTN_UP : AOS_RETRO_BTN_DOWN;
        }
    }
    if (over) *over = on;
    return bits;
}

static void show_bits(uint32_t bits)
{
    if (bits == s.shown_bits || !s.ctl_box) return;
    if (s.log_stats) aos_hal_log(TAG, "buttons 0x%02x", (unsigned)bits);
    for (int i = 0; i < s.nctl; i++) {
        ctl_t *c = &s.ctl[i];
        if (!c->obj || !c->bit) continue;
        bool on = (bits & c->bit) != 0;
        if (on) lv_obj_add_state(c->obj, LV_STATE_PRESSED);
        else lv_obj_remove_state(c->obj, LV_STATE_PRESSED);
    }
    s.shown_bits = bits;
}

static void slider_thumb(bool on, int sx)
{
    if (!s.slider_thumb) return;
    if (!on) {
        lv_obj_add_flag(s.slider_thumb, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int w = lv_obj_get_width(s.slider_thumb);
    int x = sx - s.slider.x1 - w / 2;
    int maxx = s.slider.x2 - s.slider.x1 + 1 - w;
    if (x < 0) x = 0;
    if (x > maxx) x = maxx;
    lv_obj_set_x(s.slider_thumb, x);
    lv_obj_remove_flag(s.slider_thumb, LV_OBJ_FLAG_HIDDEN);
}

static void view_rect(rect_t *r)
{
    r->x1 = s.pub.x;
    r->y1 = s.pub.y;
    r->x2 = (int16_t)(s.pub.x + s.pub.w * s.pub.scale - 1);
    r->y2 = (int16_t)(s.pub.y + s.pub.h * s.pub.scale - 1);
}

/* One touch sample: the buttons under the fingers, and the canvas finger.
 *
 * A finger on a control is that control and nothing else, even where the
 * controls float over the canvas (OVERLAY): the canvas finger is the first
 * one that is on the canvas and on no control. So a thumb held on A never
 * takes the place of the finger dragging on the canvas.
 *
 * A tap is a finger that comes down on the canvas (not on a control, and
 * not one sliding in from outside, like an edge swipe) while no other
 * finger is on the canvas. The panel has no finger ids, so a new finger is
 * told by the count going up: with two, it is the one further from where
 * the single finger was. */
static void sample(const aos_touch_frame_t *f)
{
    uint32_t bits = 0;
    bool on = false, slider_on = false;
    int cx = 0, cy = 0, slx = 0;
    int n = f->count < 2 ? f->count : 2;
    int landed = -1;                /* the finger that just came down, if any */
    rect_t vr;
    view_rect(&vr);

    if (n > s.prev_count) {
        if (s.prev_count == 0) {
            landed = 0;             /* (two at once: the first counts) */
        } else {
            int d[2];
            for (int i = 0; i < 2; i++) {
                int dx = f->x[i] - s.prev_x[0], dy = f->y[i] - s.prev_y[0];
                d[i] = dx * dx + dy * dy;
            }
            landed = d[1] >= d[0] ? 1 : 0;
        }
    }

    for (int i = 0; i < n; i++) {
        int x = f->x[i] - s.root_x, y = f->y[i] - s.root_y;
        bool over = false;
        if (s.controls_shown) bits |= hit(x, y, &over);
        if (over) continue;         /* a control's finger, only that */
        bool in_view = (s.flags & AOS_RETRO_TOUCH) && !s.touch_off && in_rect(&vr, x, y, 0);
        if (in_view && i == landed && !s.touch_on) {
            s.tap_pending = true;
            s.tap_x = (int16_t)((x - vr.x1) / s.pub.scale);
            s.tap_y = (int16_t)((y - vr.y1) / s.pub.scale);
        }
        if (on) continue;
        if (in_view) {
            on = true;
            cx = (x - vr.x1) / s.pub.scale;
            cy = (y - vr.y1) / s.pub.scale;
        } else if (s.has_slider && s.controls_shown && in_rect(&s.slider, x, y, HIT_PAD)) {
            on = slider_on = true;
            slx = x;
            cx = (x - vr.x1) / s.pub.scale;
            cy = s.pub.h - 1;
        }
    }
    if (cx < 0) cx = 0;
    if (cx >= s.pub.w) cx = s.pub.w - 1;
    if (cy < 0) cy = 0;
    if (cy >= s.pub.h) cy = s.pub.h - 1;
    if (s.tap_pending) {
        if (s.tap_x >= s.pub.w) s.tap_x = (int16_t)(s.pub.w - 1);
        if (s.tap_y >= s.pub.h) s.tap_y = (int16_t)(s.pub.h - 1);
    }

    s.edges |= bits & ~s.held;
    s.rel_edges |= s.held & ~bits;
    s.held = bits;
    s.prev_count = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        s.prev_x[i] = f->x[i];
        s.prev_y[i] = f->y[i];
    }
    s.touch_on = on;
    if (on) {                       /* lifted: touch() keeps where it was */
        s.tx = (int16_t)cx;
        s.ty = (int16_t)cy;
    }
    slider_thumb(slider_on, slx);
}

static void poll(void)
{
    if (!s.active) return;
    aos_touch_frame_t fr[16];
    uint32_t n = aos_hal_touch_frames(s.seq, fr, 16);
    /* An app in the background (or under the switcher) must not collect the
     * taps meant for whatever is in front: they are read and dropped. */
    bool front = s.pub.view && lv_obj_is_visible(s.pub.view);
    if (front && s.root) {
        /* where the root is NOW: the runtime slides it (open, back swipe) */
        lv_area_t rc;
        lv_obj_get_coords(s.root, &rc);
        s.root_x = (int16_t)rc.x1;
        s.root_y = (int16_t)rc.y1;
    }
    for (uint32_t i = 0; i < n; i++) {
        s.seq = fr[i].seq;
        if (front) {
            sample(&fr[i]);
        } else {
            /* not ours, but where the fingers are: coming back to the
             * front, a finger that was already down is no tap */
            s.prev_count = fr[i].count < 2 ? fr[i].count : 2;
            for (int k = 0; k < 2; k++) {
                s.prev_x[k] = fr[i].x[k];
                s.prev_y[k] = fr[i].y[k];
            }
        }
    }
    if (!front) {
        s.held = s.edges = s.rel_edges = 0;
        s.touch_on = s.tap_pending = false;
    }
    show_bits(s.held);
}

/* LVGL's whole refresh, start to end: what the frame really costs. */
static uint64_t s_refr_t0;

static void refr_event(lv_event_t *e)
{
    if (!s.active) return;
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        s_refr_t0 = aos_hal_uptime_us();
    } else if (s_refr_t0) {
        uint32_t us = (uint32_t)(aos_hal_uptime_us() - s_refr_t0);
        s.st.refresh_us_avg = s.st.refresh_us_avg ? (s.st.refresh_us_avg * 15u + us) / 16u : us;
        s_refr_t0 = 0;
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    poll();
}

/* -------------------------------------------------------------------------- */
/* Pacing                                                                      */
/* -------------------------------------------------------------------------- */

static void pace_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s.active || s.fps <= 0) return;
    uint64_t now = aos_hal_uptime_ms();
    if (s.paused) {
        s.t0 = now;
        s.ticks = 0;
        return;
    }
    uint32_t elapsed = (uint32_t)((now - s.t0) * (uint64_t)s.fps / 1000u);
    uint32_t due = elapsed - s.ticks;
    if (due == 0) return;
    if (due > 4) {                  /* do not spiral: drop the time we cannot catch up */
        s.ticks = elapsed - 4;
        due = 4;
    }
    poll();
    uint32_t gen = s.gen;
    for (uint32_t i = 0; i < due; i++) {
        s.ticks++;
        if (s.step) s.step(s.user);
        if (gen != s.gen || !s.active) return;  /* the step ended us, or re-ran */
    }
    if (s.draw) s.draw(s.user);
    if (gen != s.gen || !s.active) return;
    stats_roll();

    /* a minute's worth of ticks: fold them into t0, so the product stays small */
    if (s.ticks >= (uint32_t)s.fps * 60u) {
        s.t0 += 60000u;
        s.ticks -= (uint32_t)s.fps * 60u;
    }
    s.fps_n++;
    if (now - s.fps_t >= 1000) {
        uint32_t f10 = (uint32_t)(s.fps_n * 10000u / (uint32_t)(now - s.fps_t));
        s.st.fps10 = (uint16_t)(s.st.fps10 ? (s.st.fps10 * 3u + f10) / 4u : f10);
        s.fps_t = now;
        s.fps_n = 0;
    }
}

void aos_retro_run(int fps, aos_retro_fn_t step, aos_retro_fn_t draw, void *user)
{
    if (!s.active) return;
    if (fps < 1) fps = 1;
    if (fps > 120) fps = 120;
    s.gen++;
    s.fps = fps;
    s.step = step;
    s.draw = draw;
    s.user = user;
    s.t0 = aos_hal_uptime_ms();
    s.fps_t = s.t0;
    s.fps_n = 0;
    s.ticks = 0;
    s.paused = false;
    /* half a frame: with the period at exactly one frame, timer jitter
     * gives calls with nothing due and then two steps at once */
    uint32_t period = (uint32_t)(1000 / fps / 2);
    if (period < 1) period = 1;
    if (s.pace) lv_timer_set_period(s.pace, period);
    else s.pace = lv_timer_create(pace_cb, period, NULL);
}

void aos_retro_stop(void)
{
    s.gen++;
    if (s.pace) lv_timer_delete(s.pace);
    s.pace = NULL;
    s.step = s.draw = NULL;
}

void aos_retro_pause(bool paused)
{
    s.paused = paused;
}

/* -------------------------------------------------------------------------- */
/* Life cycle                                                                  */
/* -------------------------------------------------------------------------- */

static int best_scale(int W, int H, int w, int h, int reserve_h, int reserve_w)
{
    int k = (W - reserve_w) / w;
    int kh = (H - reserve_h) / h;
    if (kh < k) k = kh;
    return k;
}

const aos_retro_t *aos_retro_begin(lv_obj_t *root, int w, int h, int scale, uint32_t flags)
{
    if (s.active) aos_retro_end();
    if (!root || w < 8 || h < 8 || w > 1280 || h > 1280) return NULL;
#ifdef AOS_SIM
    /* AOS_RETRO_ADD_FLAGS=0x20c (simulator): extra controls on any game,
     * to look at a layout no game asks for yet */
    const char *add = getenv("AOS_RETRO_ADD_FLAGS");
    if (add) flags |= (uint32_t)strtoul(add, NULL, 0);
#endif
    if (flags & AOS_RETRO_LR_SPLIT) flags |= AOS_RETRO_LR;

    lv_obj_update_layout(root);
    int W = lv_obj_get_width(root), H = lv_obj_get_height(root);
    bool land = W > H;
    bool pad = flags & (AOS_RETRO_DPAD | AOS_RETRO_LR | AOS_RETRO_A | AOS_RETRO_B);
    bool any = pad || (flags & (AOS_RETRO_PAUSE | AOS_RETRO_SLIDER));

    /* the factor */
    int k = scale;
    if (k <= 0) {
        if (!any || (flags & AOS_RETRO_OVERLAY)) {
            k = best_scale(W, H, w, h, 0, 0);
        } else if (land) {
            k = best_scale(W, H, w, h, 0, pad ? 2 * SIDE_LAND : SIDE_LAND);
        } else {
            int need = pad ? CTRL_MIN_PAD : CTRL_MIN_SMALL;
            if (flags & AOS_RETRO_SLIDER) need += pad ? 120 : 96;
            k = best_scale(W, H, w, h, need, 0);
            if (k < 1) {                /* no room below: float them */
                flags |= AOS_RETRO_OVERLAY;
                k = best_scale(W, H, w, h, 0, 0);
            }
        }
    }
    if (k < 1) k = 1;
    if (k > 16) k = 16;

    size_t bytes = (size_t)w * h * 2;
    uint16_t *px = (uint16_t *)aos_hal_retro_alloc(bytes);
    if (!px) {
        aos_hal_log(TAG, "no memory for a %dx%d canvas (%u B)", w, h, (unsigned)bytes);
        return NULL;
    }
    /* s.gen outlives the canvas: a tick that ends the canvas and begins
     * another from inside step() or draw() (Lua's reload) must still see
     * its generation change when it returns to pace_cb() */
    uint32_t gen = s.gen;
    memset(&s, 0, sizeof s);
    s.gen = gen + 1;
    unit_ensure();

    s.flags = flags;
    s.land = land;
    s.root = root;
    s.pub.px = px;
    s.pub.w = (int16_t)w;
    s.pub.h = (int16_t)h;
    s.pub.scale = (int16_t)k;
    s.img.header.magic = LV_IMAGE_HEADER_MAGIC;
    s.img.header.cf = LV_COLOR_FORMAT_RGB565;
    s.img.header.w = (uint32_t)w;
    s.img.header.h = (uint32_t)h;
    s.img.header.stride = (uint32_t)w * 2;
    s.img.data_size = (uint32_t)bytes;
    s.img.data = (const uint8_t *)px;

    /* where it goes */
    int gw = w * k, gh = h * k;
    int gx = (W - gw) / 2, gy;
    if (land || !any || (flags & (AOS_RETRO_OVERLAY | AOS_RETRO_CENTER))) {
        gy = (H - gh) / 2;
    } else {
        gy = (H - gh - (pad ? CTRL_MIN_PAD : CTRL_MIN_SMALL)) / 3;
        if (gy > MAX_TOP) gy = MAX_TOP;
        if (gy < 0) gy = 0;
    }
    s.pub.x = (int16_t)gx;
    s.pub.y = (int16_t)gy;

    lv_area_t rc;
    lv_obj_get_coords(root, &rc);
    s.root_x = (int16_t)rc.x1;
    s.root_y = (int16_t)rc.y1;

    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *v = lv_obj_create(root);
    lv_obj_remove_style_all(v);
    lv_obj_set_pos(v, gx, gy);
    lv_obj_set_size(v, gw, gh);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(v, view_event, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(v, view_event, LV_EVENT_COVER_CHECK, NULL);
    lv_obj_add_event_cb(v, view_event, LV_EVENT_DELETE, NULL);
    s.pub.view = v;
    s.active = true;

    /* the controls */
    if (any) {
        rect_t c;
        if (land) {
            c = (rect_t){ 0, 0, (int16_t)(W - 1), (int16_t)(H - 1) };
        } else if (flags & AOS_RETRO_OVERLAY) {
            int ch = pad ? CTRL_MIN_PAD + 40 : CTRL_MIN_SMALL;
            c = (rect_t){ 0, (int16_t)(H - ch), (int16_t)(W - 1), (int16_t)(H - 1) };
        } else {
            c = (rect_t){ 0, (int16_t)(gy + gh), (int16_t)(W - 1), (int16_t)(H - 1) };
        }
        lv_obj_t *box = lv_obj_create(root);
        lv_obj_remove_style_all(box);
        lv_obj_set_pos(box, c.x1, c.y1);
        lv_obj_set_size(box, c.x2 - c.x1 + 1, c.y2 - c.y1 + 1);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        /* it swallows the pointer (so a thumb on the pad never reaches the
         * app's own touch layers) and its drags (a slider run is not a swipe) */
        if (!(flags & AOS_RETRO_OVERLAY) || land) {
            lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_remove_flag(box, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(box, ctlbox_deleted, LV_EVENT_DELETE, NULL);
        s.ctl_box = box;
        s.pub.controls = box;
        bx0 = c.x1;
        by0 = c.y1;
        if (land) layout_landscape(c);
        else layout_portrait(c);
        s.controls_shown = true;
    }

    /* touch frames from now on only */
    aos_touch_frame_t now;
    if (aos_hal_touch_frame(&now)) {
        s.seq = now.seq;
        /* the finger that opened the app is no tap */
        s.prev_count = now.count < 2 ? now.count : 2;
        for (int i = 0; i < 2; i++) {
            s.prev_x[i] = now.x[i];
            s.prev_y[i] = now.y[i];
        }
    }
    s.poll_timer = lv_timer_create(poll_cb, 16, NULL);
    lv_display_t *disp = lv_obj_get_display(root);
    if (disp) {
        lv_display_add_event_cb(disp, refr_event, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(disp, refr_event, LV_EVENT_REFR_READY, NULL);
    }
    /* development switches; on the board getenv() is always NULL */
    s.log_stats = getenv("AOS_RETRO_STATS") != NULL;
    s.full = getenv("AOS_RETRO_FULL") != NULL;
    s.verify = getenv("AOS_RETRO_VERIFY") != NULL;
    s.lvgl_only = getenv("AOS_RETRO_LVGL") != NULL;

    lv_obj_invalidate(v);
    aos_hal_log(TAG, "canvas %dx%d x%d at %d,%d on %dx%d, flags 0x%x", w, h, k, gx, gy, W, H,
                (unsigned)flags);
    return &s.pub;
}

void aos_retro_end(void)
{
    if (!s.active) return;
    s.active = false;
    s.gen++;
    if (s.pace) lv_timer_delete(s.pace);
    if (s.poll_timer) lv_timer_delete(s.poll_timer);
    s.pace = s.poll_timer = NULL;

    if (s.verify) {
        aos_hal_log(TAG, "VERIFY: %u areas, %u pixels checked, %u wrong", (unsigned)s.verify_areas,
                    (unsigned)s.verify_px, (unsigned)s.verify_bad);
    }
    aos_hal_log(TAG, "end %dx%d x%d: %u frames, scale avg %u us, max %u us, %s",
                s.pub.w, s.pub.h, s.pub.scale, (unsigned)s.st.frames, (unsigned)s.st.scale_us_avg,
                (unsigned)s.st.scale_us_max, s.st.hw ? "HAL scaler" : "CPU");

    /* objects first (their DELETE handlers see active == false and do nothing
     * but clear pointers), then the pixels nobody draws any more */
    lv_display_t *disp = s.root ? lv_obj_get_display(s.root) : lv_display_get_default();
    if (disp) {
        lv_display_remove_event_cb_with_user_data(disp, refr_event, NULL);
    }
    s_refr_t0 = 0;

    lv_obj_t *v = s.pub.view, *box = s.ctl_box;
    s.pub.view = NULL;
    s.ctl_box = NULL;
    if (box) lv_obj_delete(box);
    if (v) lv_obj_delete(v);
    lv_image_cache_drop(&s.img);
    aos_hal_retro_free(s.pub.px);
    uint32_t gen = s.gen;           /* see aos_retro_begin() */
    memset(&s, 0, sizeof s);
    s.gen = gen;
}

const aos_retro_t *aos_retro_get(void)
{
    return s.active ? &s.pub : NULL;
}

/* -------------------------------------------------------------------------- */
/* Presenting                                                                  */
/* -------------------------------------------------------------------------- */

/* What the refresh since the previous present cost. */
static void stats_roll(void)
{
    s.st.frames++;
    if (s.log_stats && s.lvgl_only && s.st.frames % 60 == 0) {
        aos_hal_log(TAG, "%u.%u fps, scaled by LVGL's own code; refresh avg %u us",
                    s.st.fps10 / 10, s.st.fps10 % 10, (unsigned)s.st.refresh_us_avg);
    }
    if (!s.acc_px) return;
    uint32_t us = (uint32_t)s.acc_us;
    s.st.scale_us = us;
    s.st.scale_us_avg = s.st.scale_us_avg ? (s.st.scale_us_avg * 15u + us) / 16u : us;
    if (us > s.st.scale_us_max) s.st.scale_us_max = us;
    s.st.px_last = s.acc_px;
    s.st.hw = s.acc_hw;
    if (s.verify && s.st.frames % 60 == 0) {
        aos_hal_log(TAG, "VERIFY: %u areas, %u pixels checked, %u wrong so far",
                    (unsigned)s.verify_areas, (unsigned)s.verify_px, (unsigned)s.verify_bad);
    }
    if (s.log_stats && s.st.frames % 60 == 0) {
        aos_hal_log(TAG, "%u.%u fps, scale %u us (avg %u, max %u) for %u px, %s; refresh avg %u us",
                    s.st.fps10 / 10, s.st.fps10 % 10, (unsigned)us, (unsigned)s.st.scale_us_avg,
                    (unsigned)s.st.scale_us_max, (unsigned)s.acc_px, s.acc_hw ? "HAL scaler" : "CPU",
                    (unsigned)s.st.refresh_us_avg);
    }
    s.acc_us = 0;
    s.acc_px = 0;
    s.acc_hw = 0;
}

/* Marks a screen area of the view for the next refresh. What
 * lv_obj_invalidate_area() does, less its walk: since LVGL 9.5 every
 * invalidation walks every object on the screen that overlaps the area
 * (lv_obj_get_ext_draw_size(), an event each) looking for blurred or
 * drop-shadowed widgets to redraw with it. A game presents a dozen or two
 * rectangles a frame under a tree of hidden panels, and the walks cost more
 * than the comparing that found them (2043 in the simulator, 2026-09-30).
 * Nothing in P4OS uses blur or drop shadows (aos_panels.c darkens instead);
 * a blurred widget over a canvas would have to be invalidated by its app. */
static void view_invalidate(lv_area_t *a)
{
    lv_obj_t *v = s.pub.view;
    lv_display_t *disp = lv_obj_get_display(v);
    if (!disp || !lv_display_is_invalidation_enabled(disp)) return;
    if (!lv_obj_area_is_visible(v, a)) return;   /* clipped to the parents, hidden = nothing */
#if LV_DRAW_TRANSFORM_USE_MATRIX
    lv_area_increase(a, 5, 5);
#endif
    lv_inv_area(disp, a);
}

void aos_retro_present(void)
{
    if (!s.active || !s.pub.view) return;
    if (!s.pace) stats_roll();      /* with aos_retro_run() it rolls per draw */
    lv_area_t co;
    lv_obj_get_coords(s.pub.view, &co);
    view_invalidate(&co);
}

void aos_retro_present_rect(int x, int y, int w, int h)
{
    if (!s.active || !s.pub.view) return;
    if (s.full) {                   /* AOS_RETRO_FULL: measuring the worst case */
        lv_area_t co;
        lv_obj_get_coords(s.pub.view, &co);
        view_invalidate(&co);
        return;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s.pub.w) w = s.pub.w - x;
    if (y + h > s.pub.h) h = s.pub.h - y;
    if (w <= 0 || h <= 0) return;
    lv_area_t co, a;
    lv_obj_get_coords(s.pub.view, &co);
    int k = s.pub.scale;
    a.x1 = co.x1 + x * k;
    a.y1 = co.y1 + y * k;
    a.x2 = a.x1 + w * k - 1;
    a.y2 = a.y1 + h * k - 1;
    view_invalidate(&a);
}

/* -------------------------------------------------------------------------- */
/* Input                                                                       */
/* -------------------------------------------------------------------------- */

uint32_t aos_retro_buttons(void)
{
    poll();
    return s.held;
}

uint32_t aos_retro_released(void)
{
    poll();
    uint32_t e = s.rel_edges;
    s.rel_edges = 0;
    return e;
}

uint32_t aos_retro_pressed(void)
{
    poll();
    uint32_t e = s.edges;
    s.edges = 0;
    return e;
}

bool aos_retro_touch(int *x, int *y)
{
    poll();
    if (x) *x = s.tx;
    if (y) *y = s.ty;
    return s.active && s.touch_on;
}

bool aos_retro_tap(int *x, int *y)
{
    poll();
    if (!s.tap_pending) return false;
    s.tap_pending = false;
    if (x) *x = s.tap_x;
    if (y) *y = s.tap_y;
    return true;
}

bool aos_retro_to_canvas(int sx, int sy, int *x, int *y)
{
    if (!s.active) return false;
    int rx = sx - s.root_x - s.pub.x, ry = sy - s.root_y - s.pub.y;
    int k = s.pub.scale;
    bool in = rx >= 0 && ry >= 0 && rx < s.pub.w * k && ry < s.pub.h * k;
    int cx = rx / k, cy = ry / k;
    if (rx < 0) cx = 0;
    if (ry < 0) cy = 0;
    if (cx >= s.pub.w) cx = s.pub.w - 1;
    if (cy >= s.pub.h) cy = s.pub.h - 1;
    if (x) *x = cx;
    if (y) *y = cy;
    return in;
}

void aos_retro_set_label(uint32_t btn, const char *text)
{
    for (int i = 0; i < s.nctl; i++) {
        if (s.ctl[i].bit == btn && s.ctl[i].caption) {
            lv_label_set_text(s.ctl[i].caption, text ? text : "");
        }
    }
}

void aos_retro_set_border(uint32_t rgb)
{
    if (s.active && s.root) lv_obj_set_style_bg_color(s.root, lv_color_hex(rgb), 0);
}

void aos_retro_show_controls(bool show)
{
    if (!s.active) return;
    /* the finger on the canvas goes with the controls: under a panel it
     * belongs to the panel's buttons */
    s.touch_off = !show;
    if (!show) {
        s.touch_on = s.tap_pending = false;
    }
    if (!s.ctl_box || s.controls_shown == show) return;
    s.controls_shown = show;
    if (show) lv_obj_remove_flag(s.ctl_box, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s.ctl_box, LV_OBJ_FLAG_HIDDEN);
    if (!show) {
        s.held = 0;
        s.edges = 0;
        show_bits(0);
    }
}

void aos_retro_stats(aos_retro_stats_t *out)
{
    if (out) *out = s.st;
}
