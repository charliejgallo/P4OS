/*
 * DIBUJO - the editor.
 *
 * The canvas window is one lv_canvas over an RGB565 buffer the size of the
 * window (vbuf). The document's own screen copy (doc->comp, at the
 * document's resolution) is recomposed only where something changed, and
 * the window samples it at the view's zoom: a dab of the brush redraws a
 * few hundred pixels, a pinch redraws the window once per LVGL frame.
 * The grid, the guides and the symmetry axis are painted into the window
 * as it is sampled; the selection, the floating object's handles and the
 * pad's cursor are LVGL objects on top, which never touch the pixels.
 *
 * One pointer drives every tool: the finger, the USB mouse (the system's
 * second LVGL pointer, so it arrives as the same events) and the joystick's
 * cursor all end in ptr_down / ptr_move / ptr_up in window pixels. Two
 * fingers zoom and pan (aos_gesture); the mouse's wheel zooms about the
 * arrow (see wheel_cb for how it reaches us).
 */
#include "dib_app.h"
#include "lvgl_private.h"           /* lv_anim_t's fields: the wheel's trick */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_F        3.14159265f
#define ZOOM_MAX    16.0f
#define OBJ_MIN_PX  4.0f            /* a shape is born after this much drag */
#define C_GRID      0x9A9AA2
#define C_GUIDE     0x00C8FF
#define C_SYM       0xFF2D95

static uint32_t now_ms(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

static inline uint16_t to565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

static inline uint16_t mix565(uint16_t a, uint16_t b)
{
    return (uint16_t)(((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1));
}

static float clampf(float v, float a, float b)
{
    return v < a ? a : v > b ? b : v;
}

/* lroundf, fminf and fmaxf are not in the firmware's table */
static int rnd(float v) { return (int)floorf(v + 0.5f); }
static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }

/* --------------------------------------------------------------------------
 * Coordinates: window pixels (inside the canvas window) and document pixels
 * -------------------------------------------------------------------------- */

static inline float w2dx(const app_t *a, float sx) { return (sx - a->vox) / a->vs; }
static inline float w2dy(const app_t *a, float sy) { return (sy - a->voy) / a->vs; }
static inline float d2wx(const app_t *a, float x) { return a->vox + x * a->vs; }
static inline float d2wy(const app_t *a, float y) { return a->voy + y * a->vs; }

/* The magnet: the nearest grid line, guide, edge or middle within reach. */
static float snap_axis(const app_t *a, float v, bool vertical)
{
    if (!a->snap) return v;
    const dib_doc_t *d = a->doc;
    float reach = SNAP_PX / a->vs, best = v, bd = reach;
    float size = vertical ? (float)d->w : (float)d->h;
    float cand[3] = { 0, size * 0.5f, size };
    for (int i = 0; i < 3; i++) {
        if (fabsf(cand[i] - v) < bd) { bd = fabsf(cand[i] - v); best = cand[i]; }
    }
    for (int i = 0; i < d->nguides; i++) {
        if ((d->guides[i].vertical != 0) != vertical) continue;
        float g = (float)d->guides[i].pos;
        if (fabsf(g - v) < bd) { bd = fabsf(g - v); best = g; }
    }
    if (a->grid && a->grid_step > 0) {
        float g = roundf(v / (float)a->grid_step) * (float)a->grid_step;
        if (fabsf(g - v) < bd) best = g;
    }
    return best;
}

static dib_pt_t snap_pt(const app_t *a, float x, float y)
{
    return (dib_pt_t){ snap_axis(a, x, true), snap_axis(a, y, false) };
}

/* --------------------------------------------------------------------------
 * The window
 * -------------------------------------------------------------------------- */

static void invalidate_win(app_t *a, int x0, int y0, int x1, int y1)
{
    lv_area_t co;
    lv_obj_get_coords(a->view, &co);
    lv_area_t ar = { co.x1 + x0, co.y1 + y0, co.x1 + x1 - 1, co.y1 + y1 - 1 };
    lv_obj_invalidate_area(a->view, &ar);
}

/* Lines over the sampled pixels, for the rows y0..y1 of columns x0..x1. */
static void paint_vline(app_t *a, int x, int y0, int y1, uint16_t c, bool mix, int dash)
{
    if (x < 0 || x >= a->L.cv_w) return;
    for (int y = y0; y < y1; y++) {
        if (dash && ((y / dash) & 1)) continue;
        uint16_t *p = &a->vbuf[(size_t)y * a->L.cv_w + x];
        *p = mix ? mix565(*p, c) : c;
    }
}

static void paint_hline(app_t *a, int y, int x0, int x1, uint16_t c, bool mix, int dash)
{
    if (y < 0 || y >= a->L.cv_h) return;
    uint16_t *row = &a->vbuf[(size_t)y * a->L.cv_w];
    for (int x = x0; x < x1; x++) {
        if (dash && ((x / dash) & 1)) continue;
        row[x] = mix ? mix565(row[x], c) : c;
    }
}

static void render_win(app_t *a, int x0, int y0, int x1, int y1)
{
    const dib_doc_t *d = a->doc;
    const int W = a->L.cv_w;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > a->L.cv_h) y1 = a->L.cv_h;
    if (x0 >= x1 || y0 >= y1) return;
    const float inv = 1.0f / a->vs;
    const uint16_t out = to565(C_OUTSIDE);
    const bool box = a->vs < 0.75f;         /* average 2x2 when shrinking */
    /* the columns' document x, once for the whole rectangle */
    if (a->xmap_n < W) {
        dib_free(a->xmap);
        a->xmap = dib_alloc(sizeof(int32_t) * W);
        a->xmap_n = a->xmap ? W : 0;
        if (!a->xmap) return;
    }
    int32_t *xmap = a->xmap;
    for (int x = x0; x < x1; x++) {
        int dx = (int)floorf(((float)x + 0.5f - a->vox) * inv);
        xmap[x] = dx >= 0 && dx < d->w ? dx : -1;
    }
    for (int y = y0; y < y1; y++) {
        uint16_t *row = a->vbuf + (size_t)y * W;
        int dy = (int)floorf(((float)y + 0.5f - a->voy) * inv);
        if (dy < 0 || dy >= d->h) {
            for (int x = x0; x < x1; x++) row[x] = out;
            continue;
        }
        const uint16_t *src = d->comp + (size_t)dy * d->w;
        if (!box) {
            for (int x = x0; x < x1; x++) row[x] = xmap[x] >= 0 ? src[xmap[x]] : out;
        } else {
            const uint16_t *src2 = dy + 1 < d->h ? src + d->w : src;
            for (int x = x0; x < x1; x++) {
                int sx = xmap[x];
                if (sx < 0) {
                    row[x] = out;
                    continue;
                }
                int sx2 = sx + 1 < d->w ? sx + 1 : sx;
                uint16_t p = src[sx], q = src[sx2], r = src2[sx], s = src2[sx2];
                unsigned rr = (p >> 11) + (q >> 11) + (r >> 11) + (s >> 11);
                unsigned gg = ((p >> 5) & 63) + ((q >> 5) & 63) + ((r >> 5) & 63) + ((s >> 5) & 63);
                unsigned bb = (p & 31) + (q & 31) + (r & 31) + (s & 31);
                row[x] = (uint16_t)(((rr >> 2) << 11) | ((gg >> 2) << 5) | (bb >> 2));
            }
        }
    }
    /* the document's rows and columns inside this rectangle */
    int dy0 = (int)floorf(d2wy(a, 0)), dy1 = (int)ceilf(d2wy(a, (float)d->h));
    int dx0 = (int)floorf(d2wx(a, 0)), dx1 = (int)ceilf(d2wx(a, (float)d->w));
    int ry0 = y0 > dy0 ? y0 : dy0, ry1 = y1 < dy1 ? y1 : dy1;
    int rx0 = x0 > dx0 ? x0 : dx0, rx1 = x1 < dx1 ? x1 : dx1;
    if (ry0 >= ry1 || rx0 >= rx1) return;

    if (a->grid && a->grid_step * a->vs >= 6.0f) {
        uint16_t gc = to565(C_GRID);
        int k0 = (int)floorf(w2dx(a, (float)rx0) / a->grid_step), k1 = (int)ceilf(w2dx(a, (float)rx1) / a->grid_step);
        for (int k = k0 < 1 ? 1 : k0; k <= k1; k++) {
            int sx = (int)rnd(d2wx(a, (float)(k * a->grid_step)));
            if (sx >= rx0 && sx < rx1 && k * a->grid_step < d->w) paint_vline(a, sx, ry0, ry1, gc, true, 0);
        }
        k0 = (int)floorf(w2dy(a, (float)ry0) / a->grid_step);
        k1 = (int)ceilf(w2dy(a, (float)ry1) / a->grid_step);
        for (int k = k0 < 1 ? 1 : k0; k <= k1; k++) {
            int sy = (int)rnd(d2wy(a, (float)(k * a->grid_step)));
            if (sy >= ry0 && sy < ry1 && k * a->grid_step < d->h) paint_hline(a, sy, rx0, rx1, gc, true, 0);
        }
    }
    if (a->rulers || a->snap) {
        uint16_t gc = to565(C_GUIDE);
        for (int i = 0; i < d->nguides; i++) {
            if (d->guides[i].vertical) {
                int sx = (int)rnd(d2wx(a, d->guides[i].pos));
                if (sx >= x0 && sx < x1) paint_vline(a, sx, ry0, ry1, gc, false, 0);
            } else {
                int sy = (int)rnd(d2wy(a, d->guides[i].pos));
                if (sy >= y0 && sy < y1) paint_hline(a, sy, rx0, rx1, gc, false, 0);
            }
        }
    }
    if (a->sym && (a->tool == T_BRUSH || a->tool == T_ERASER)) {
        uint16_t sc = to565(C_SYM);
        if (a->sym == DIB_SYM_V || a->sym == DIB_SYM_4) {
            int sx = (int)rnd(d2wx(a, d->w * 0.5f));
            if (sx >= x0 && sx < x1) paint_vline(a, sx, ry0, ry1, sc, false, 8);
        }
        if (a->sym == DIB_SYM_H || a->sym == DIB_SYM_4) {
            int sy = (int)rnd(d2wy(a, d->h * 0.5f));
            if (sy >= y0 && sy < y1) paint_hline(a, sy, rx0, rx1, sc, false, 8);
        }
    }
}

/* A rectangle of the document, recomposed and shown. */
static void show_doc_rect(app_t *a, dib_rect_t r)
{
    dib_rect_clip(&r, a->doc->w, a->doc->h);
    if (dib_rect_empty(&r)) return;
    dib_compose(a->doc, r);
    if (a->view_full) return;
    int x0 = (int)floorf(d2wx(a, (float)r.x0)) - 1, x1 = (int)ceilf(d2wx(a, (float)r.x1)) + 1;
    int y0 = (int)floorf(d2wy(a, (float)r.y0)) - 1, y1 = (int)ceilf(d2wy(a, (float)r.y1)) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > a->L.cv_w) x1 = a->L.cv_w;
    if (y1 > a->L.cv_h) y1 = a->L.cv_h;
    if (x0 >= x1 || y0 >= y1) return;
    render_win(a, x0, y0, x1, y1);
    invalidate_win(a, x0, y0, x1, y1);
}

void dj_ed_changed(app_t *a, dib_rect_t r)
{
    dib_rect_union(&a->doc_dirty, &r);
    dj_mark_dirty(a);
}

void dj_ed_all_changed(app_t *a)
{
    a->doc_dirty = (dib_rect_t){ 0, 0, a->doc->w, a->doc->h };
    dj_mark_dirty(a);
    a->view_full = true;
}

/* --------------------------------------------------------------------------
 * Rulers
 * -------------------------------------------------------------------------- */

static int ruler_step(float vs)
{
    static const int steps[] = { 5, 10, 25, 50, 100, 250, 500, 1000 };
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        if (steps[i] * vs >= 60.0f) return steps[i];
    }
    return 1000;
}

static void ruler_draw(app_t *a, bool top)
{
    lv_obj_t *cv = top ? a->ruler_top : a->ruler_left;
    uint16_t *buf = top ? a->rbuf_top : a->rbuf_left;
    if (!cv || !buf) return;
    int len = top ? a->L.cv_w : a->L.cv_h;
    int w = top ? len : RULER_W, h = top ? RULER_W : len;
    uint16_t bg = to565(0x2C2C2E), fg = to565(0xB0B0B8), gd = to565(C_GUIDE);
    for (int i = 0; i < w * h; i++) buf[i] = bg;
    int step = ruler_step(a->vs);
    int minor = step / 5 ? step / 5 : 1;
    float o = top ? a->vox : a->voy;
    int size = top ? a->doc->w : a->doc->h;
    int k0 = (int)floorf(-o / a->vs / minor) - 1, k1 = (int)ceilf((len - o) / a->vs / minor) + 1;
    for (int k = k0; k <= k1; k++) {
        int v = k * minor;
        if (v < 0 || v > size) continue;
        int p = (int)rnd(o + v * a->vs);
        if (p < 0 || p >= len) continue;
        int tick = v % step == 0 ? RULER_W - 4 : RULER_W / 3;
        for (int t = 0; t < tick; t++) {
            if (top) buf[(RULER_W - 1 - t) * w + p] = fg;
            else buf[p * w + (RULER_W - 1 - t)] = fg;
        }
    }
    for (int i = 0; i < a->doc->nguides; i++) {
        if ((a->doc->guides[i].vertical != 0) != top) continue;
        int p = (int)rnd(o + a->doc->guides[i].pos * a->vs);
        for (int dd = -5; dd <= 5; dd++) {
            int q = p + dd;
            if (q < 0 || q >= len) continue;
            for (int t = 0; t < 10 - (dd < 0 ? -dd : dd) * 2 + 2 && t < RULER_W; t++) {
                if (top) buf[t * w + q] = gd;
                else buf[q * w + t] = gd;
            }
        }
    }
    /* the numbers, through the canvas's own layer */
    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = lv_color_hex(0xD1D1D6);
    ld.font = aos_font_tiny;
    for (int k = (int)floorf(-o / a->vs / step); k <= (int)ceilf((len - o) / a->vs / step); k++) {
        int v = k * step;
        if (v < 0 || v > size) continue;
        int p = (int)rnd(o + v * a->vs);
        if (p < 0 || p >= len - 8) continue;
        char t[12];
        snprintf(t, sizeof t, "%d", v);
        ld.text = t;
        ld.text_local = 1;
        lv_area_t ar;
        if (top) ar = (lv_area_t){ p + 3, 0, p + 60, 18 };
        else ar = (lv_area_t){ 1, p + 2, RULER_W - 1, p + 20 };
        if (!top && v >= 100) continue;     /* no room for three digits across */
        lv_draw_label(&layer, &ld, &ar);
    }
    lv_canvas_finish_layer(cv, &layer);
    lv_obj_invalidate(cv);
}

static void rulers_update(app_t *a)
{
    bool on = a->rulers;
    if (a->ruler_top) {
        if (on) lv_obj_remove_flag(a->ruler_top, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(a->ruler_top, LV_OBJ_FLAG_HIDDEN);
    }
    if (a->ruler_left) {
        if (on) lv_obj_remove_flag(a->ruler_left, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(a->ruler_left, LV_OBJ_FLAG_HIDDEN);
    }
    if (on) {
        ruler_draw(a, true);
        ruler_draw(a, false);
    }
}

/* --------------------------------------------------------------------------
 * The view: fit, zoom, pan
 * -------------------------------------------------------------------------- */

static float fit_scale(const app_t *a)
{
    float m = a->rulers ? (float)RULER_W : 0.0f;
    float sx = ((float)a->L.cv_w - m - 24) / (float)a->doc->w;
    float sy = ((float)a->L.cv_h - m - 24) / (float)a->doc->h;
    return sx < sy ? sx : sy;
}

static void view_clamp(app_t *a)
{
    float zmin = fit_scale(a) * 0.5f;
    if (zmin > 1.0f) zmin = 1.0f;
    a->vs = clampf(a->vs, zmin, ZOOM_MAX);
    /* keep at least a corner of the document in the window */
    float dw = a->doc->w * a->vs, dh = a->doc->h * a->vs, keep = 80;
    a->vox = clampf(a->vox, keep - dw, (float)a->L.cv_w - keep);
    a->voy = clampf(a->voy, keep - dh, (float)a->L.cv_h - keep);
}

void dj_ed_view_fit(app_t *a)
{
    float m = a->rulers ? (float)RULER_W : 0.0f;
    a->vs = fit_scale(a);
    a->vox = m + ((float)a->L.cv_w - m - a->doc->w * a->vs) * 0.5f;
    a->voy = m + ((float)a->L.cv_h - m - a->doc->h * a->vs) * 0.5f;
    a->view_full = true;
    dj_ed_refresh(a);
}

void dj_ed_view_actual(app_t *a)
{
    dj_ed_zoom_at(a, 1.0f / a->vs, a->L.cv_w * 0.5f, a->L.cv_h * 0.5f);
}

void dj_ed_zoom_at(app_t *a, float k, float sx, float sy)
{
    float ns = a->vs * k;
    float zmin = fit_scale(a) * 0.5f;
    if (zmin > 1.0f) zmin = 1.0f;
    ns = clampf(ns, zmin, ZOOM_MAX);
    k = ns / a->vs;
    a->vox = sx - (sx - a->vox) * k;
    a->voy = sy - (sy - a->voy) * k;
    a->vs = ns;
    view_clamp(a);
    a->view_full = true;
    dj_ed_refresh(a);
}

/* --------------------------------------------------------------------------
 * Overlays: the selection, the floating object's handles, the polygon in
 * the making, the pad's cursor
 * -------------------------------------------------------------------------- */

static void hide(lv_obj_t *o)
{
    if (o && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void show_at(lv_obj_t *o, int32_t x, int32_t y)
{
    if (!o) return;
    lv_obj_set_pos(o, x, y);
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static bool obj_is_line(const app_t *a)
{
    return a->obj.kind == DIB_OBJ_LINE || a->obj.kind == DIB_OBJ_ARROW;
}

/* The handles in window pixels: 0..3 corners and 4 the turn, or 0, 1 the
 * ends of a line. Returns how many. */
static int obj_handles(const app_t *a, dib_pt_t *h)
{
    if (obj_is_line(a)) {
        dib_pt_t p, q;
        dib_obj_ends(&a->obj, &p, &q);
        h[0] = (dib_pt_t){ d2wx(a, p.x), d2wy(a, p.y) };
        h[1] = (dib_pt_t){ d2wx(a, q.x), d2wy(a, q.y) };
        return 2;
    }
    dib_pt_t c[4];
    dib_obj_corners(&a->obj, c);
    for (int i = 0; i < 4; i++) h[i] = (dib_pt_t){ d2wx(a, c[i].x), d2wy(a, c[i].y) };
    float mx = (h[0].x + h[1].x) * 0.5f, my = (h[0].y + h[1].y) * 0.5f;
    float cx = d2wx(a, a->obj.cx), cy = d2wy(a, a->obj.cy);
    float dx = mx - cx, dy = my - cy, len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) {
        dx = sinf(a->obj.ang);
        dy = -cosf(a->obj.ang);
        len = 1.0f;
    }
    h[4] = (dib_pt_t){ mx + dx / len * ROT_HANDLE_D, my + dy / len * ROT_HANDLE_D };
    return 5;
}

static void overlay_update(app_t *a)
{
    const int32_t ox = a->L.cv_x, oy = a->L.cv_y;
    /* the selection */
    if (a->tool == T_SELECT && !dib_rect_empty(&a->sel) && !a->has_obj) {
        int32_t x0 = (int32_t)rnd(d2wx(a, (float)a->sel.x0)), y0 = (int32_t)rnd(d2wy(a, (float)a->sel.y0));
        int32_t x1 = (int32_t)rnd(d2wx(a, (float)a->sel.x1)), y1 = (int32_t)rnd(d2wy(a, (float)a->sel.y1));
        lv_obj_set_size(a->sel_box, x1 - x0 + 4, y1 - y0 + 4);
        show_at(a->sel_box, ox + x0 - 2, oy + y0 - 2);
    } else {
        hide(a->sel_box);
    }
    /* the floating object */
    if (a->has_obj) {
        dib_pt_t h[5];
        int n = obj_handles(a, h);
        for (int i = 0; i < 6; i++) {
            if (i < n) show_at(a->handle[i], ox + (int32_t)h[i].x - 14, oy + (int32_t)h[i].y - 14);
            else hide(a->handle[i]);
        }
        if (n == 5) {
            for (int i = 0; i < 4; i++) a->obj_pts[i] = (lv_point_precise_t){ ox + h[i].x, oy + h[i].y };
            a->obj_pts[4] = a->obj_pts[0];
            lv_line_set_points(a->obj_line, a->obj_pts, 5);
            if (lv_obj_has_flag(a->obj_line, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(a->obj_line, LV_OBJ_FLAG_HIDDEN);
        } else {
            hide(a->obj_line);
        }
    } else {
        for (int i = 0; i < 6; i++) hide(a->handle[i]);
        hide(a->obj_line);
    }
    /* the polygon being placed */
    if (a->npoly > 0 && a->tool == T_SHAPE) {
        for (int i = 0; i < a->npoly; i++) {
            a->poly_pts[i] = (lv_point_precise_t){ ox + d2wx(a, a->poly[i].x), oy + d2wy(a, a->poly[i].y) };
        }
        int n = a->npoly;
        if (n == 1) a->poly_pts[n++] = a->poly_pts[0];
        lv_line_set_points(a->poly_line, a->poly_pts, n);
        if (lv_obj_has_flag(a->poly_line, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(a->poly_line, LV_OBJ_FLAG_HIDDEN);
        /* the first vertex shows where to tap to close it */
        show_at(a->handle[5], (int32_t)a->poly_pts[0].x - 14, (int32_t)a->poly_pts[0].y - 14);
    } else {
        hide(a->poly_line);
        if (!a->has_obj) hide(a->handle[5]);
    }
    /* the pad's cursor */
    if (a->pad_seen && a->pad.connected && !a->pad_menu_on && a->panel == P_NONE) {
        int32_t r = 20;
        if (a->tool == T_BRUSH || a->tool == T_ERASER) {
            float sz = (a->tool == T_ERASER ? a->eraser_size : a->brush_size[a->brush.kind]) * a->vs;
            r = (int32_t)(sz * 0.5f);
            if (r < 10) r = 10;
            if (r > 300) r = 300;
        }
        lv_obj_set_size(a->cursor, 2 * r, 2 * r);
        show_at(a->cursor, ox + (int32_t)a->cur_x - r, oy + (int32_t)a->cur_y - r);
    } else {
        hide(a->cursor);
    }
}

/* --------------------------------------------------------------------------
 * The floating object
 * -------------------------------------------------------------------------- */

void dj_ed_obj_show(app_t *a)
{
    dib_rect_t r = { 0, 0, 0, 0 };
    dib_obj_render(a->doc, &a->obj, &r);
    dj_ed_changed(a, r);
    a->overlay_dirty = true;
}

void dj_ed_obj_commit(app_t *a)
{
    if (!a->has_obj) return;
    dib_rect_t r = { 0, 0, 0, 0 };
    dib_obj_render(a->doc, &a->obj, &r);        /* make sure it is what is shown */
    dib_flt_commit(a->doc, &r);
    dj_ed_changed(a, r);
    dib_obj_free(&a->obj);
    a->has_obj = false;
    a->obj_from_cut = false;
    a->obj_is_text = false;
    a->overlay_dirty = true;
    a->opt_dirty = true;
    dj_ed_refresh(a);
}

void dj_ed_obj_cancel(app_t *a)
{
    if (!a->has_obj) return;
    dib_rect_t r = { 0, 0, 0, 0 };
    dib_flt_clear(a->doc, &r);
    if (a->obj_from_cut) dib_undo(a->doc, &r);  /* the pixels go back where they were */
    dj_ed_changed(a, r);
    dib_obj_free(&a->obj);
    a->has_obj = false;
    a->obj_from_cut = false;
    a->obj_is_text = false;
    a->overlay_dirty = true;
    a->opt_dirty = true;
    dj_ed_refresh(a);
}

void dj_ed_obj_from_bitmap(app_t *a, dib_px_t *bmp, int w, int h, float cx, float cy,
                            bool from_cut, bool uniform)
{
    if (a->has_obj) dj_ed_obj_commit(a);
    memset(&a->obj, 0, sizeof a->obj);
    a->obj.kind = DIB_OBJ_BITMAP;
    a->obj.bmp = bmp;
    a->obj.bw = w;
    a->obj.bh = h;
    a->obj.w = (float)w;
    a->obj.h = (float)h;
    a->obj.cx = cx;
    a->obj.cy = cy;
    a->has_obj = true;
    a->obj_from_cut = from_cut;
    a->obj_uniform = uniform;
    a->opt_dirty = true;
    dj_ed_obj_show(a);
    dj_ed_refresh(a);
}

/* A new shape with the tool's current style. */
static void obj_new_shape(app_t *a, dib_pt_t p)
{
    memset(&a->obj, 0, sizeof a->obj);
    a->obj.kind = a->shape;
    a->obj.cx = p.x;
    a->obj.cy = p.y;
    a->obj.stroke = a->shape_stroke || a->shape == DIB_OBJ_LINE || a->shape == DIB_OBJ_ARROW;
    a->obj.fill = a->shape_fill && a->shape != DIB_OBJ_LINE && a->shape != DIB_OBJ_ARROW;
    a->obj.stroke_w = a->shape_w;
    a->obj.radius = a->shape == DIB_OBJ_RECT ? a->shape_radius : 0;
    a->obj.stroke_c = a->color;
    a->obj.fill_c = a->color2;
    a->obj_uniform = false;
    a->obj_from_cut = false;
    a->obj_is_text = false;
}

/* The polygon's vertices become a floating shape in their own box. */
static void poly_finish(app_t *a)
{
    if (a->npoly < 2) {
        a->npoly = 0;
        a->overlay_dirty = true;
        return;
    }
    float x0 = a->poly[0].x, x1 = x0, y0 = a->poly[0].y, y1 = y0;
    for (int i = 1; i < a->npoly; i++) {
        if (a->poly[i].x < x0) x0 = a->poly[i].x;
        if (a->poly[i].x > x1) x1 = a->poly[i].x;
        if (a->poly[i].y < y0) y0 = a->poly[i].y;
        if (a->poly[i].y > y1) y1 = a->poly[i].y;
    }
    float w = x1 - x0 < 1 ? 1 : x1 - x0, h = y1 - y0 < 1 ? 1 : y1 - y0;
    obj_new_shape(a, (dib_pt_t){ (x0 + x1) * 0.5f, (y0 + y1) * 0.5f });
    a->obj.kind = DIB_OBJ_POLY;
    a->obj.w = w;
    a->obj.h = h;
    a->obj.npoly = a->npoly;
    for (int i = 0; i < a->npoly; i++) {
        a->obj.poly[i] = (dib_pt_t){ (a->poly[i].x - a->obj.cx) / w, (a->poly[i].y - a->obj.cy) / h };
    }
    a->npoly = 0;
    a->has_obj = true;
    a->opt_dirty = true;
    dj_ed_obj_show(a);
    dj_ed_refresh(a);
}

/* What a press on the floating object grabs: a handle, the body, or nothing. */
static grab_t obj_grab(app_t *a, float sx, float sy, int *idx)
{
    dib_pt_t h[5];
    int n = obj_handles(a, h);
    float best = HANDLE_R * HANDLE_R;
    int bi = -1;
    for (int i = 0; i < n; i++) {
        float dx = h[i].x - sx, dy = h[i].y - sy, dd = dx * dx + dy * dy;
        if (dd < best) {
            best = dd;
            bi = i;
        }
    }
    if (bi >= 0) {
        *idx = bi;
        if (n == 2) return G_OBJ_END;
        return bi == 4 ? G_OBJ_ROT : G_OBJ_CORNER;
    }
    if (dib_obj_hit(&a->obj, w2dx(a, sx), w2dy(a, sy), 16.0f / a->vs)) return G_OBJ_MOVE;
    return G_NONE;
}

static void obj_drag(app_t *a, float x, float y)
{
    dib_obj_t *o = &a->obj;
    switch (a->grab) {
    case G_OBJ_MOVE: {
        float cx = a->obj_cx0 + (x - a->dx0), cy = a->obj_cy0 + (y - a->dy0);
        if (a->snap) {
            dib_pt_t s = snap_pt(a, cx, cy);
            cx = s.x;
            cy = s.y;
        }
        if (o->kind == DIB_OBJ_BITMAP && o->ang == 0.0f) {
            /* whole pixels while it is not turned: a moved selection stays sharp */
            cx = roundf(cx - o->w * 0.5f) + o->w * 0.5f;
            cy = roundf(cy - o->h * 0.5f) + o->h * 0.5f;
        }
        o->cx = cx;
        o->cy = cy;
        break;
    }
    case G_OBJ_END: {
        dib_pt_t p, q;
        dib_obj_ends(o, &p, &q);
        dib_pt_t n = snap_pt(a, x, y);
        if (a->grab_idx == 0) p = n;
        else q = n;
        dib_obj_set_ends(o, p, q);
        break;
    }
    case G_OBJ_ROT: {
        float ang = atan2f(y - o->cy, x - o->cx) + PI_F * 0.5f;
        if (a->snap) {
            float st = PI_F / 12.0f;        /* 15 degrees */
            ang = roundf(ang / st) * st;
        }
        o->ang = ang;
        break;
    }
    case G_OBJ_CORNER: {
        /* the opposite corner stays; in the box's own frame the dragged one
         * is where the pointer is */
        dib_pt_t p = snap_pt(a, x, y);
        float c = cosf(-o->ang), s = sinf(-o->ang);
        float vx = (p.x - a->obj_fix.x) * c - (p.y - a->obj_fix.y) * s;
        float vy = (p.x - a->obj_fix.x) * s + (p.y - a->obj_fix.y) * c;
        static const float sgx[4] = { -1, 1, 1, -1 }, sgy[4] = { -1, -1, 1, 1 };
        float gx = sgx[a->grab_idx], gy = sgy[a->grab_idx];
        float w = vx * gx, h = vy * gy;
        if (a->obj_uniform && fabsf(a->obj_w0) > 0.5f && fabsf(a->obj_h0) > 0.5f) {
            float k = fabsf(w / a->obj_w0) > fabsf(h / a->obj_h0) ? w / fabsf(a->obj_w0) : h / fabsf(a->obj_h0);
            if (k < 0.02f) k = 0.02f;
            w = fabsf(a->obj_w0) * k;
            h = fabsf(a->obj_h0) * k;
        }
        if (fabsf(w) < 2.0f) w = w < 0 ? -2.0f : 2.0f;
        if (fabsf(h) < 2.0f) h = h < 0 ? -2.0f : 2.0f;
        /* keep the sign the object had: a flip is a drag past the corner */
        o->w = (a->obj_w0 < 0 ? -w : w);
        o->h = (a->obj_h0 < 0 ? -h : h);
        float lx = gx * w * 0.5f, ly = gy * h * 0.5f;
        float cc = cosf(o->ang), ss = sinf(o->ang);
        o->cx = a->obj_fix.x + lx * cc - ly * ss;
        o->cy = a->obj_fix.y + lx * ss + ly * cc;
        break;
    }
    default:
        return;
    }
    dj_ed_obj_show(a);
}

/* --------------------------------------------------------------------------
 * The selection
 * -------------------------------------------------------------------------- */

static void sel_to_obj(app_t *a, bool cut)
{
    if (dib_rect_empty(&a->sel)) return;
    dib_rect_t r = a->sel;
    dib_px_t *bmp = dib_copy_rect(a->doc, r, cut);
    if (!bmp) return;
    if (cut) dj_ed_changed(a, r);
    dj_ed_obj_from_bitmap(a, bmp, r.x1 - r.x0, r.y1 - r.y0, (r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f, cut, false);
    if (!cut) {
        /* a copy steps aside so it is seen */
        a->obj.cx += 24;
        a->obj.cy += 24;
        dj_ed_obj_show(a);
    }
    a->sel = (dib_rect_t){ 0, 0, 0, 0 };
}

static void sel_copy(app_t *a)
{
    if (dib_rect_empty(&a->sel)) return;
    dib_free(a->clip);
    a->clip = dib_copy_rect(a->doc, a->sel, false);
    a->clip_w = a->clip ? a->sel.x1 - a->sel.x0 : 0;
    a->clip_h = a->clip ? a->sel.y1 - a->sel.y0 : 0;
    if (a->clip) dj_toast(_("Copiado"));
}

static void sel_paste(app_t *a)
{
    if (!a->clip) return;
    dib_px_t *b = dib_alloc((size_t)a->clip_w * a->clip_h * 4);
    if (!b) return;
    memcpy(b, a->clip, (size_t)a->clip_w * a->clip_h * 4);
    if (a->tool != T_SELECT) dj_ed_set_tool(a, T_SELECT);
    float cx = w2dx(a, a->L.cv_w * 0.5f), cy = w2dy(a, a->L.cv_h * 0.5f);
    cx = roundf(cx - a->clip_w * 0.5f) + a->clip_w * 0.5f;
    cy = roundf(cy - a->clip_h * 0.5f) + a->clip_h * 0.5f;
    dj_ed_obj_from_bitmap(a, b, a->clip_w, a->clip_h, cx, cy, false, false);
}

static void sel_delete(app_t *a)
{
    if (dib_rect_empty(&a->sel)) return;
    dib_clear_rect(a->doc, a->sel);
    dj_ed_changed(a, a->sel);
    dj_ed_refresh(a);
}

static void sel_all(app_t *a)
{
    if (a->has_obj) dj_ed_obj_commit(a);
    if (a->tool != T_SELECT) dj_ed_set_tool(a, T_SELECT);
    a->sel = (dib_rect_t){ 0, 0, a->doc->w, a->doc->h };
    a->overlay_dirty = true;
    a->opt_dirty = true;
}

/* --------------------------------------------------------------------------
 * Colours
 * -------------------------------------------------------------------------- */

void dj_ed_push_recent(app_t *a, dib_px_t c)
{
    c |= 0xFF000000u;
    int i = 0;
    while (i < a->nrecent && a->recent[i] != c) i++;
    if (i == 0 && a->nrecent) return;
    if (i == a->nrecent) {
        if (a->nrecent < NRECENT) a->nrecent++;
        i = a->nrecent - 1;
    }
    memmove(&a->recent[1], &a->recent[0], sizeof(dib_px_t) * i);
    a->recent[0] = c;
}

void dj_ed_set_color(app_t *a, dib_px_t c, bool second)
{
    c |= 0xFF000000u;
    if (second) a->color2 = c;
    else a->color = c;
    if (a->has_obj && a->obj.kind != DIB_OBJ_BITMAP) {
        if (second) a->obj.fill_c = c;
        else a->obj.stroke_c = c;
        dj_ed_obj_show(a);
    }
    if (a->has_obj && a->obj_is_text && !second && c != a->obj_text_c) {
        /* a text takes the new colour: drawn again, same box */
        dib_px_t *bmp;
        int w, h;
        if (dj_text_render(a, a->obj_text, a->obj_font, c, &bmp, &w, &h)) {
            float kx = a->obj.w / (float)a->obj.bw, ky = a->obj.h / (float)a->obj.bh;
            dib_free(a->obj.bmp);
            a->obj.bmp = bmp;
            a->obj.bw = w;
            a->obj.bh = h;
            a->obj.w = w * kx;
            a->obj.h = h * ky;
            a->obj_text_c = c;
            dj_ed_obj_show(a);
        }
    }
    dj_ed_refresh(a);
}

/* --------------------------------------------------------------------------
 * The pointer
 * -------------------------------------------------------------------------- */

static void brush_for_stroke(app_t *a, dib_brush_t *b)
{
    if (a->tool == T_ERASER) {
        *b = (dib_brush_t){ DIB_BR_ERASER, a->eraser_size, a->eraser_opacity, a->brush.smooth, 0, a->sym };
    } else {
        *b = a->brush;
        b->size = a->brush_size[a->brush.kind];
        b->opacity = a->brush_opacity[a->brush.kind];
        b->color = a->color;
        b->sym = a->sym;
    }
}

static int guide_near(const app_t *a, bool vertical, float sv)
{
    for (int i = 0; i < a->doc->nguides; i++) {
        if ((a->doc->guides[i].vertical != 0) != vertical) continue;
        float p = vertical ? d2wx(a, a->doc->guides[i].pos) : d2wy(a, a->doc->guides[i].pos);
        if (fabsf(p - sv) < 20) return i;
    }
    return -1;
}

static void ptr_down(app_t *a, float sx, float sy)
{
    dib_doc_t *d = a->doc;
    a->ptr_down = true;
    a->down_ms = a->move_ms = now_ms();
    a->sx0 = sx;
    a->sy0 = sy;
    float x = w2dx(a, sx), y = w2dy(a, sy);
    a->dx0 = x;
    a->dy0 = y;
    a->px = x;
    a->py = y;
    if (a->pinching) {
        a->grab = G_SWALLOW;
        return;
    }
    /* a guide out of a ruler, or an old one taken from it */
    if (a->rulers && (sx < RULER_W || sy < RULER_W)) {
        bool vertical = sx < RULER_W && sy >= RULER_W;
        if (sx < RULER_W && sy < RULER_W) {
            a->grab = G_SWALLOW;
            return;
        }
        int g = guide_near(a, vertical, vertical ? sy : sx);
        if (g < 0) {
            g = guide_near(a, vertical, vertical ? sx : sy);
        }
        if (g < 0 && d->nguides < DIB_MAX_GUIDES) {
            g = d->nguides++;
            d->guides[g].vertical = vertical;
            d->guides[g].pos = (int16_t)(vertical ? x : y);
        }
        a->grab = g >= 0 ? G_GUIDE : G_SWALLOW;
        a->grab_idx = g;
        return;
    }
    if (a->tool == T_HAND) {
        a->grab = G_PAN;
        return;
    }
    if (a->has_obj) {
        int idx = 0;
        grab_t g = obj_grab(a, sx, sy, &idx);
        if (g != G_NONE) {
            a->grab = g;
            a->grab_idx = idx;
            a->obj_cx0 = a->obj.cx;
            a->obj_cy0 = a->obj.cy;
            a->obj_w0 = a->obj.w;
            a->obj_h0 = a->obj.h;
            a->obj_ang0 = a->obj.ang;
            if (g == G_OBJ_CORNER) {
                dib_pt_t c[4];
                dib_obj_corners(&a->obj, c);
                a->obj_fix = c[(idx + 2) % 4];
            }
            return;
        }
        dj_ed_obj_commit(a);
        if (a->tool != T_SHAPE && a->tool != T_SELECT) {
            a->grab = G_SWALLOW;            /* that tap was only to let go of it */
            return;
        }
    }
    switch (a->tool) {
    case T_BRUSH:
    case T_ERASER: {
        dib_brush_t b;
        brush_for_stroke(a, &b);
        if (d->layers[d->active].visible == false) {
            dj_toast(_("La capa está oculta"));
            a->grab = G_SWALLOW;
            return;
        }
        if (dib_stroke_begin(&a->stroke, d, &b, x, y)) {
            a->grab = G_STROKE;
            if (a->tool == T_BRUSH) dj_ed_push_recent(a, a->color);
        } else {
            a->grab = G_SWALLOW;
        }
        break;
    }
    case T_FILL: {
        dib_rect_t r = { 0, 0, 0, 0 };
        if (dib_fill(d, (int)x, (int)y, a->color, 1.0f, a->fill_tol, a->sample_all, &r)) {
            dj_ed_changed(a, r);
            dj_ed_push_recent(a, a->color);
            dj_ed_refresh(a);
        }
        a->grab = G_SWALLOW;
        break;
    }
    case T_PICK:
        a->grab = G_PICK;
        break;
    case T_SHAPE:
        if (a->shape == DIB_OBJ_POLY) {
            dib_pt_t p = snap_pt(a, x, y);
            if (a->npoly >= 3) {
                float fx = d2wx(a, a->poly[0].x) - sx, fy = d2wy(a, a->poly[0].y) - sy;
                if (fx * fx + fy * fy < HANDLE_R * HANDLE_R) {
                    poly_finish(a);
                    a->grab = G_SWALLOW;
                    return;
                }
            }
            if (a->npoly < DIB_POLY_MAX) a->poly[a->npoly++] = p;
            a->grab = G_POLY_PT;
            a->overlay_dirty = true;
            a->opt_dirty = a->npoly == 1;
        } else {
            obj_new_shape(a, snap_pt(a, x, y));
            a->dx0 = a->obj.cx;
            a->dy0 = a->obj.cy;
            a->grab = G_SHAPE_NEW;
        }
        break;
    case T_TEXT:
        a->grab = G_SWALLOW;                /* the text opens on release, if it was a tap */
        break;
    case T_SELECT:
        if (!dib_rect_empty(&a->sel) && x >= a->sel.x0 && x < a->sel.x1 && y >= a->sel.y0 && y < a->sel.y1) {
            sel_to_obj(a, true);
            a->grab = G_OBJ_MOVE;
            a->obj_cx0 = a->obj.cx;
            a->obj_cy0 = a->obj.cy;
        } else {
            a->sel = (dib_rect_t){ 0, 0, 0, 0 };
            a->grab = G_SEL_NEW;
            a->overlay_dirty = true;
        }
        break;
    default:
        a->grab = G_SWALLOW;
        break;
    }
}

static void ptr_move(app_t *a, float sx, float sy)
{
    if (!a->ptr_down) return;
    dib_doc_t *d = a->doc;
    float x = w2dx(a, sx), y = w2dy(a, sy);
    float psx = d2wx(a, a->px), psy = d2wy(a, a->py);
    a->px = x;
    a->py = y;
    a->move_ms = now_ms();
    switch (a->grab) {
    case G_STROKE:
        dib_stroke_to(&a->stroke, x, y);
        break;
    case G_PICK: {
        dib_px_t c = dib_pick(d, (int)x, (int)y, a->sample_all);
        if (c >> 24) dj_ed_set_color(a, c, false);
        break;
    }
    case G_PAN:
        a->vox += sx - psx;
        a->voy += sy - psy;
        view_clamp(a);
        a->view_full = true;
        break;
    case G_SHAPE_NEW: {
        float ddx = sx - a->sx0, ddy = sy - a->sy0;
        if (!a->has_obj && ddx * ddx + ddy * ddy < OBJ_MIN_PX * OBJ_MIN_PX) break;
        dib_pt_t p0 = { a->dx0, a->dy0 }, p1 = snap_pt(a, x, y);
        if (a->obj.kind == DIB_OBJ_LINE || a->obj.kind == DIB_OBJ_ARROW) {
            dib_obj_set_ends(&a->obj, p0, p1);
        } else {
            a->obj.cx = (p0.x + p1.x) * 0.5f;
            a->obj.cy = (p0.y + p1.y) * 0.5f;
            a->obj.w = fabsf(p1.x - p0.x);
            a->obj.h = fabsf(p1.y - p0.y);
            a->obj.ang = 0;
        }
        if (!a->has_obj) {
            a->has_obj = true;
            a->opt_dirty = true;
            dj_ed_refresh(a);
        }
        dj_ed_obj_show(a);
        break;
    }
    case G_POLY_PT:
        if (a->npoly) a->poly[a->npoly - 1] = snap_pt(a, x, y);
        a->overlay_dirty = true;
        break;
    case G_SEL_NEW: {
        dib_pt_t p0 = snap_pt(a, a->dx0, a->dy0), p1 = snap_pt(a, x, y);
        dib_rect_t r = { (int)floorf(minf(p0.x, p1.x)), (int)floorf(minf(p0.y, p1.y)),
                         (int)ceilf(maxf(p0.x, p1.x)), (int)ceilf(maxf(p0.y, p1.y)) };
        dib_rect_clip(&r, d->w, d->h);
        a->sel = r;
        a->overlay_dirty = true;
        break;
    }
    case G_OBJ_MOVE:
    case G_OBJ_CORNER:
    case G_OBJ_ROT:
    case G_OBJ_END:
        obj_drag(a, x, y);
        break;
    case G_GUIDE: {
        dib_guide_t *g = &d->guides[a->grab_idx];
        float v = g->vertical ? x : y;
        if (a->grid && a->snap) v = roundf(v / a->grid_step) * a->grid_step;
        g->pos = (int16_t)clampf(v, -32000, 32000);
        a->view_full = true;
        break;
    }
    default:
        break;
    }
}

static void ptr_up(app_t *a)
{
    if (!a->ptr_down) return;
    a->ptr_down = false;
    dib_doc_t *d = a->doc;
    switch (a->grab) {
    case G_STROKE: {
        dib_stroke_end(&a->stroke);
        dib_rect_t r;
        while (dib_stroke_take_dirty(&a->stroke, &r)) dj_ed_changed(a, r);
        dj_mark_dirty(a);
        dj_ed_refresh(a);
        break;
    }
    case G_PICK: {
        dib_px_t c = dib_pick(d, (int)a->px, (int)a->py, a->sample_all);
        if (c >> 24) {
            dj_ed_set_color(a, c, false);
            dj_ed_push_recent(a, c);
        }
        dj_ed_set_tool(a, a->tool_before_pick);
        break;
    }
    case G_SEL_NEW:
        if (a->sel.x1 - a->sel.x0 < 2 || a->sel.y1 - a->sel.y0 < 2) a->sel = (dib_rect_t){ 0, 0, 0, 0 };
        a->opt_dirty = true;
        a->overlay_dirty = true;
        break;
    case G_GUIDE: {
        dib_guide_t *g = &d->guides[a->grab_idx];
        float sv = g->vertical ? d2wx(a, g->pos) : d2wy(a, g->pos);
        float lim = g->vertical ? (float)d->w : (float)d->h;
        if (sv < RULER_W || g->pos < 0 || g->pos > lim) {
            /* back onto the ruler, or off the drawing: it goes */
            memmove(g, g + 1, sizeof(dib_guide_t) * (d->nguides - a->grab_idx - 1));
            d->nguides--;
        }
        a->view_full = true;
        dj_mark_dirty(a);
        break;
    }
    case G_SWALLOW:
        if (a->tool == T_TEXT && !a->has_obj && !a->pinching) {
            float dx = d2wx(a, a->px) - a->sx0, dy = d2wy(a, a->py) - a->sy0;
            if (dx * dx + dy * dy < 16 * 16) dj_text_open(a, a->dx0, a->dy0, false);
        }
        break;
    default:
        break;
    }
    a->grab = G_NONE;
}

/* A press that must not finish what it started (two fingers came). */
static void ptr_abort(app_t *a)
{
    switch (a->grab) {
    case G_STROKE:
        if (now_ms() - a->down_ms < PINCH_UNDO_MS) {
            dib_stroke_cancel(&a->stroke);
        } else {
            dib_stroke_end(&a->stroke);
            dj_mark_dirty(a);
        }
        {
            dib_rect_t r;
            while (dib_stroke_take_dirty(&a->stroke, &r)) dib_rect_union(&a->doc_dirty, &r);
        }
        break;
    case G_SHAPE_NEW:
        if (a->has_obj && now_ms() - a->down_ms < PINCH_UNDO_MS) {
            dib_rect_t r = { 0, 0, 0, 0 };
            dib_flt_clear(a->doc, &r);
            dib_rect_union(&a->doc_dirty, &r);
            a->has_obj = false;
            a->overlay_dirty = true;
        }
        break;
    case G_POLY_PT:
        if (a->npoly && now_ms() - a->down_ms < PINCH_UNDO_MS) a->npoly--;
        a->overlay_dirty = true;
        break;
    case G_SEL_NEW:
        a->sel = (dib_rect_t){ 0, 0, 0, 0 };
        a->overlay_dirty = true;
        break;
    default:
        break;
    }
    a->grab = G_SWALLOW;
    dj_ed_refresh(a);
}

static void touch_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (a->closing || !a->doc || a->job) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ptr_up(a);
        return;
    }
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t co;
    lv_obj_get_coords(a->view, &co);
    float sx = (float)(p.x - co.x1), sy = (float)(p.y - co.y1);
    if (code == LV_EVENT_PRESSED) {
        if (a->panel != P_NONE) dj_panel_close(a);
        ptr_down(a, sx, sy);
    } else if (code == LV_EVENT_PRESSING) {
        if (a->pinching && a->grab != G_SWALLOW) ptr_abort(a);
        if (!a->pinching) ptr_move(a, sx, sy);
    }
}

static void gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    app_t *a = user;
    if (a->closing || !a->doc || a->job) return;
    lv_area_t co;
    lv_obj_get_coords(a->view, &co);
    switch (ev->type) {
    case AOS_GESTURE_PINCH_BEGIN:
        a->pinching = true;
        if (a->ptr_down) ptr_abort(a);
        break;
    case AOS_GESTURE_PINCH:
        dj_ed_zoom_at(a, ev->scale, ev->x - co.x1, ev->y - co.y1);
        a->vox += ev->dx;
        a->voy += ev->dy;
        view_clamp(a);
        break;
    case AOS_GESTURE_PINCH_END:
        a->pinching = false;
        break;
    default:
        break;
    }
}

/* The mouse's wheel. The system turns it into a scroll of whatever
 * scrollable object is under the arrow (aos_hwmouse.c), always animated, so
 * the touch layer is made scrollable (with nothing visible to scroll: an
 * empty child three windows tall) and the scroll is caught as it begins:
 * the animation's values give the direction, and are made equal so nothing
 * moves. The arrow's position comes from the mouse's own LVGL pointer. */
static void wheel_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    lv_anim_t *an = lv_event_get_param(e);
    if (!an || !a->doc) return;
    int32_t dy = an->end_value - an->start_value;
    an->end_value = an->start_value;
    if (!dy || a->job) return;
    float sx = a->L.cv_w * 0.5f, sy = a->L.cv_h * 0.5f;
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
        if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER && lv_indev_get_cursor(in)) {
            lv_point_t p;
            lv_indev_get_point(in, &p);
            lv_area_t co;
            lv_obj_get_coords(a->view, &co);
            sx = (float)(p.x - co.x1);
            sy = (float)(p.y - co.y1);
            break;
        }
    }
    dj_ed_zoom_at(a, dy > 0 ? 1.25f : 0.8f, sx, sy);
}

/* --------------------------------------------------------------------------
 * The bars
 * -------------------------------------------------------------------------- */

static icon_t tool_icon(const app_t *a, tool_t t)
{
    static const icon_t brush[] = { IC_PENCIL, IC_SOFT, IC_AIR, IC_MARKER, IC_ERASER };
    static const icon_t shape[] = { IC_LINE, IC_RECT, IC_ELLIPSE, IC_POLY, IC_ARROW };
    switch (t) {
    case T_BRUSH: return brush[a->brush.kind];
    case T_ERASER: return IC_ERASER;
    case T_SHAPE: return shape[a->shape < 5 ? a->shape : 1];
    case T_TEXT: return IC_TEXT;
    case T_FILL: return IC_FILL;
    case T_PICK: return IC_PICK;
    case T_SELECT: return IC_SELECT;
    default: return IC_HAND;
    }
}

void dj_ed_refresh(app_t *a)
{
    if (!a->ed || !a->doc) return;
    for (int i = 0; i < T_COUNT; i++) {
        if (!a->btn_tool[i]) continue;
        lv_obj_set_style_bg_color(a->btn_tool[i], i == (int)a->tool ? C_SEL : C_BTN, 0);
        lv_image_set_src(a->img_tool[i], dj_icon_get(tool_icon(a, (tool_t)i)));
    }
    bool can_undo = a->doc->nundo > 0 || a->has_obj, can_redo = a->doc->nredo > 0 && !a->has_obj;
    lv_obj_set_style_opa(a->btn_undo, can_undo ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_opa(a->btn_redo, can_redo ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_label_set_text_fmt(a->lbl_zoom, "%d %%", (int)rnd(a->vs * 100.0f));
    lv_obj_set_style_bg_color(a->sw_color, dj_ui_color(a->color), 0);
    lv_obj_t *fill = lv_obj_get_child(a->btn_color, 0);
    if (fill) lv_obj_set_style_bg_color(fill, dj_ui_color(a->color2), 0);
}

static void tool_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (a->closing || a->job) return;
    tool_t t = (tool_t)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (t == a->tool && t == T_BRUSH) {
        dj_panel_open(a, a->panel == P_BRUSHES ? P_NONE : P_BRUSHES);
        return;
    }
    if (t == a->tool && t == T_SHAPE) {
        dj_panel_open(a, a->panel == P_SHAPES ? P_NONE : P_SHAPES);
        return;
    }
    if (a->panel != P_NONE) dj_panel_close(a);
    dj_ed_set_tool(a, t);
}

void dj_ed_set_tool(app_t *a, tool_t t)
{
    if (t == T_PICK && a->tool != T_PICK) a->tool_before_pick = a->tool;
    if (a->tool == T_SHAPE && a->npoly) {
        if (a->npoly >= 2) poly_finish(a);
        a->npoly = 0;
    }
    bool keep_obj = (t == T_HAND) || (a->has_obj && t == a->tool);
    if (a->has_obj && !keep_obj) dj_ed_obj_commit(a);
    if (t != T_SELECT) a->sel = (dib_rect_t){ 0, 0, 0, 0 };
    bool sym_changed = (a->tool == T_BRUSH || a->tool == T_ERASER) != (t == T_BRUSH || t == T_ERASER);
    a->tool = t;
    if (a->sym && sym_changed) a->view_full = true;    /* the axis shows with the brushes */
    a->opt_dirty = true;
    a->overlay_dirty = true;
    dj_ed_refresh(a);
}

static void undo_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (!a->closing && !a->job) dj_ed_undo(a);
}

static void redo_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (!a->closing && !a->job) dj_ed_redo(a);
}

void dj_ed_undo(app_t *a)
{
    if (a->ptr_down) return;
    if (a->npoly) {
        a->npoly = 0;
        a->overlay_dirty = true;
        return;
    }
    if (a->has_obj) {
        dj_ed_obj_cancel(a);
        return;
    }
    dib_rect_t r = { 0, 0, 0, 0 };
    if (dib_undo(a->doc, &r)) {
        dj_ed_changed(a, r);
        if (r.x1 - r.x0 >= a->doc->w && r.y1 - r.y0 >= a->doc->h) a->view_full = true;
        dj_panel_refresh(a);
    }
    dj_ed_refresh(a);
}

void dj_ed_redo(app_t *a)
{
    if (a->ptr_down || a->has_obj) return;
    dib_rect_t r = { 0, 0, 0, 0 };
    if (dib_redo(a->doc, &r)) {
        dj_ed_changed(a, r);
        if (r.x1 - r.x0 >= a->doc->w && r.y1 - r.y0 >= a->doc->h) a->view_full = true;
        dj_panel_refresh(a);
    }
    dj_ed_refresh(a);
}

static void back_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (a->closing || a->job) return;
    /* leaving swaps the screens: the frame timer does it, not this event */
    lv_obj_set_user_data(a->btn_back, (void *)1);
}

static void zoom_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (a->closing || a->job) return;
    float fit = fit_scale(a);
    if (fabsf(a->vs - fit) < 0.01f) dj_ed_view_actual(a);
    else dj_ed_view_fit(a);
}

static void color_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (a->closing || a->job) return;
    dj_panel_open(a, a->panel == P_COLOR ? P_NONE : P_COLOR);
}

static void layers_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (a->closing || a->job) return;
    dj_panel_open(a, a->panel == P_LAYERS ? P_NONE : P_LAYERS);
}

static void menu_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (a->closing || a->job) return;
    dj_panel_open(a, a->panel == P_MENU ? P_NONE : P_MENU);
}

static lv_obj_t *icon_button(app_t *a, lv_obj_t *parent, int32_t x, int32_t y, int32_t sz, icon_t ic,
                             lv_event_cb_t cb, lv_obj_t **img)
{
    lv_obj_t *b = dj_ui_button(parent, x, y, sz, sz, C_BTN, cb, a);
    lv_obj_t *im = lv_image_create(b);
    lv_image_set_src(im, dj_icon_get(ic));
    lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(im);
    if (img) *img = im;
    return b;
}

static lv_obj_t *glyph_button(app_t *a, lv_obj_t *parent, int32_t x, int32_t y, int32_t w, const char *glyph,
                              const lv_font_t *font, lv_event_cb_t cb)
{
    lv_obj_t *b = dj_ui_button(parent, x, y, w, BAR_H, C_BTN, cb, a);
    lv_obj_t *l = aos_label(b, glyph, font, AOS_C_TEXT);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(l);
    return b;
}

/* --------------------------------------------------------------------------
 * The options bar, rebuilt for each tool (from the frame timer)
 * -------------------------------------------------------------------------- */

/* Size 1..300 px on a slider of 0..1000, so the small sizes get room. */
static int size_to_slider(float s) { return (int)rnd(logf(clampf(s, 1, 300)) / logf(300.0f) * 1000.0f); }
static float slider_to_size(int v) { return roundf(expf((float)v / 1000.0f * logf(300.0f))); }

enum { SL_SIZE = 1, SL_OPACITY, SL_WIDTH, SL_TOL };

static void opt_label_update(app_t *a, int what)
{
    char t[48];
    switch (what) {
    case SL_SIZE: {
        float s = a->tool == T_ERASER ? a->eraser_size : a->brush_size[a->brush.kind];
        snprintf(t, sizeof t, "%s %d px", _("Tamaño"), (int)s);
        if (a->lbl_size) lv_label_set_text(a->lbl_size, t);
        break;
    }
    case SL_OPACITY: {
        float o = a->tool == T_ERASER ? a->eraser_opacity : a->brush_opacity[a->brush.kind];
        snprintf(t, sizeof t, "%s %d %%", _("Opacidad"), (int)rnd(o * 100));
        if (a->lbl_opac) lv_label_set_text(a->lbl_opac, t);
        break;
    }
    case SL_WIDTH:
        snprintf(t, sizeof t, "%s %d px", _("Borde"), (int)a->shape_w);
        if (a->lbl_size) lv_label_set_text(a->lbl_size, t);
        break;
    case SL_TOL:
        snprintf(t, sizeof t, "%s %d", _("Tolerancia"), a->fill_tol);
        if (a->lbl_size) lv_label_set_text(a->lbl_size, t);
        break;
    default:
        break;
    }
}

static void slider_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    lv_obj_t *s = lv_event_get_target_obj(e);
    int what = (int)(intptr_t)lv_obj_get_user_data(s);
    int v = lv_slider_get_value(s);
    switch (what) {
    case SL_SIZE:
        if (a->tool == T_ERASER) a->eraser_size = slider_to_size(v);
        else a->brush_size[a->brush.kind] = slider_to_size(v);
        break;
    case SL_OPACITY:
        if (a->tool == T_ERASER) a->eraser_opacity = v / 100.0f;
        else a->brush_opacity[a->brush.kind] = v / 100.0f;
        break;
    case SL_WIDTH:
        a->shape_w = (float)v;
        if (a->has_obj && a->obj.kind != DIB_OBJ_BITMAP) {
            a->obj.stroke_w = (float)v;
            dj_ed_obj_show(a);
        }
        break;
    case SL_TOL:
        a->fill_tol = v;
        break;
    default:
        break;
    }
    opt_label_update(a, what);
    a->overlay_dirty = true;
}

/* A label over a slider, in a column of the bar. */
static lv_obj_t *opt_slider(app_t *a, int32_t x, int32_t w, int what, int min, int max, int val, lv_obj_t **lbl)
{
    lv_obj_t *l = aos_label(a->opt, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_pos(l, x + 6, 4);
    *lbl = l;
    lv_obj_t *s = lv_slider_create(a->opt);
    lv_obj_set_size(s, w - 52, 18);
    lv_obj_set_pos(s, x + 26, BAR_H - 36);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x48484A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, C_SEL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 8, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 24);
    lv_obj_set_user_data(s, (void *)(intptr_t)what);
    lv_obj_add_event_cb(s, slider_cb, LV_EVENT_VALUE_CHANGED, a);
    opt_label_update(a, what);
    return s;
}

static lv_obj_t *opt_button(app_t *a, int32_t x, int32_t w, const char *text, lv_color_t c, lv_event_cb_t cb)
{
    lv_obj_t *b = dj_ui_button(a->opt, x, 0, w, BAR_H, c, cb, a);
    lv_obj_t *l = aos_label(b, text, aos_font_small, AOS_C_TEXT);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(l, w - 12);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *opt_hint(app_t *a, int32_t x, int32_t w, const char *text)
{
    lv_obj_t *l = aos_label(a->opt, text, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(l, x, 0);
    lv_obj_set_height(l, BAR_H);
    lv_obj_set_style_pad_top(l, BAR_H / 2 - 30, 0);
    return l;
}

static void ok_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (a->npoly) poly_finish(a);
    else dj_ed_obj_commit(a);
}

static void cancel_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (a->npoly) {
        a->npoly = 0;
        a->overlay_dirty = true;
        a->opt_dirty = true;
        return;
    }
    if (!dib_rect_empty(&a->sel) && !a->has_obj) {
        a->sel = (dib_rect_t){ 0, 0, 0, 0 };
        a->overlay_dirty = true;
        a->opt_dirty = true;
        return;
    }
    dj_ed_obj_cancel(a);
}

static void edit_text_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (a->has_obj && a->obj_is_text) dj_text_open(a, a->obj.cx, a->obj.cy, true);
}

static void kind_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    dj_panel_open(a, a->tool == T_SHAPE ? P_SHAPES : P_BRUSHES);
}

static void fill_toggle_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    a->shape_fill = !a->shape_fill;
    if (a->has_obj && a->obj.kind != DIB_OBJ_BITMAP && !obj_is_line(a)) {
        a->obj.fill = a->shape_fill;
        dj_ed_obj_show(a);
    }
    a->opt_dirty = true;
}

static void sample_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    a->sample_all = !a->sample_all;
    a->opt_dirty = true;
}

static void sel_dup_cb(lv_event_t *e) { sel_to_obj(lv_event_get_user_data(e), false); }
static void sel_copy_cb(lv_event_t *e) { sel_copy(lv_event_get_user_data(e)); }
static void sel_paste_cb(lv_event_t *e) { sel_paste(lv_event_get_user_data(e)); }
static void sel_all_cb(lv_event_t *e) { sel_all(lv_event_get_user_data(e)); }

static void sel_del_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    sel_delete(a);
}

static void fit_cb(lv_event_t *e) { dj_ed_view_fit(lv_event_get_user_data(e)); }
static void actual_cb(lv_event_t *e) { dj_ed_view_actual(lv_event_get_user_data(e)); }

static void zin_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    dj_ed_zoom_at(a, 1.5f, a->L.cv_w * 0.5f, a->L.cv_h * 0.5f);
}

static void zout_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    dj_ed_zoom_at(a, 1.0f / 1.5f, a->L.cv_w * 0.5f, a->L.cv_h * 0.5f);
}

static void opt_build(app_t *a)
{
    lv_obj_clean(a->opt);
    a->lbl_size = a->lbl_opac = a->lbl_aux = NULL;
    const int32_t W = a->L.opt_w, B = BAR_H;
    const lv_color_t green = lv_color_hex(0x1F7A3A), red = lv_color_hex(0x8A1E22);
    int32_t x = 0;

    /* a floating object or a polygon being placed: fix it, drop it */
    bool placing = a->npoly > 0 && a->tool == T_SHAPE;
    if (a->has_obj || placing) {
        opt_button(a, 0, B, LV_SYMBOL_OK, green, ok_cb);
        opt_button(a, B + GAP, B, LV_SYMBOL_CLOSE, red, cancel_cb);
        x = 2 * (B + GAP);
        if (placing) {
            opt_hint(a, x, W - x, _("Tocá para agregar puntos; el primero lo cierra"));
            return;
        }
        if (a->obj_is_text) {
            opt_button(a, x, 2 * B, _("Editar texto"), C_BTN, edit_text_cb);
            x += 2 * B + GAP;
            opt_hint(a, x, W - x, _("Arrastrá, las esquinas escalan, el círculo gira"));
            return;
        }
        if (a->obj.kind == DIB_OBJ_BITMAP) {
            opt_hint(a, x, W - x, _("Arrastrá, las esquinas escalan, el círculo gira"));
            return;
        }
        /* a shape: its style, live */
        if (!obj_is_line(a)) {
            opt_button(a, x, B + 24, a->shape_fill ? _("Relleno sí") : _("Relleno no"),
                       a->shape_fill ? C_SEL : C_BTN, fill_toggle_cb);
            x += B + 24 + GAP;
        }
        opt_slider(a, x, W - x, SL_WIDTH, 1, 60, (int)a->shape_w, &a->lbl_size);
        return;
    }
    switch (a->tool) {
    case T_BRUSH:
    case T_ERASER: {
        if (a->tool == T_BRUSH) {
            lv_obj_t *im;
            lv_obj_t *b = dj_ui_button(a->opt, 0, 0, B, B, C_BTN, kind_cb, a);
            im = lv_image_create(b);
            lv_image_set_src(im, dj_icon_get(tool_icon(a, T_BRUSH)));
            lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_center(im);
            x = B + GAP;
        }
        int32_t half = (W - x) / 2;
        float sz = a->tool == T_ERASER ? a->eraser_size : a->brush_size[a->brush.kind];
        float op = a->tool == T_ERASER ? a->eraser_opacity : a->brush_opacity[a->brush.kind];
        opt_slider(a, x, half, SL_SIZE, 0, 1000, size_to_slider(sz), &a->lbl_size);
        opt_slider(a, x + half, W - x - half, SL_OPACITY, 1, 100, (int)rnd(op * 100), &a->lbl_opac);
        break;
    }
    case T_SHAPE: {
        lv_obj_t *b = dj_ui_button(a->opt, 0, 0, B, B, C_BTN, kind_cb, a);
        lv_obj_t *im = lv_image_create(b);
        lv_image_set_src(im, dj_icon_get(tool_icon(a, T_SHAPE)));
        lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(im);
        x = B + GAP;
        if (a->shape != DIB_OBJ_LINE && a->shape != DIB_OBJ_ARROW) {
            opt_button(a, x, B + 24, a->shape_fill ? _("Relleno sí") : _("Relleno no"),
                       a->shape_fill ? C_SEL : C_BTN, fill_toggle_cb);
            x += B + 24 + GAP;
        }
        opt_slider(a, x, W - x, SL_WIDTH, 1, 60, (int)a->shape_w, &a->lbl_size);
        break;
    }
    case T_TEXT:
        opt_hint(a, 8, W - 8, _("Tocá el lienzo donde va el texto"));
        break;
    case T_FILL:
        opt_slider(a, 0, W - 2 * B - GAP, SL_TOL, 0, 128, a->fill_tol, &a->lbl_size);
        opt_button(a, W - 2 * B, 2 * B, a->sample_all ? _("Mira: todo") : _("Mira: la capa"), C_BTN, sample_cb);
        break;
    case T_PICK:
        opt_hint(a, 8, W - 2 * B - 2 * GAP, _("Tocá un color del dibujo"));
        opt_button(a, W - 2 * B, 2 * B, a->sample_all ? _("Mira: todo") : _("Mira: la capa"), C_BTN, sample_cb);
        break;
    case T_SELECT: {
        int32_t bw = (W - 4 * GAP) / 5;
        if (dib_rect_empty(&a->sel)) {
            opt_hint(a, 8, W - 2 * bw - 2 * GAP - 8, _("Arrastrá para elegir una zona"));
            opt_button(a, W - 2 * bw - GAP, bw, _("Todo"), C_BTN, sel_all_cb);
            lv_obj_t *p = opt_button(a, W - bw, bw, _("Pegar"), C_BTN, sel_paste_cb);
            if (!a->clip) lv_obj_set_style_opa(p, LV_OPA_40, 0);
        } else {
            opt_button(a, 0, bw, _("Duplicar"), C_BTN, sel_dup_cb);
            opt_button(a, bw + GAP, bw, _("Copiar"), C_BTN, sel_copy_cb);
            lv_obj_t *p = opt_button(a, 2 * (bw + GAP), bw, _("Pegar"), C_BTN, sel_paste_cb);
            if (!a->clip) lv_obj_set_style_opa(p, LV_OPA_40, 0);
            opt_button(a, 3 * (bw + GAP), bw, _("Borrar"), red, sel_del_cb);
            opt_button(a, 4 * (bw + GAP), bw, LV_SYMBOL_CLOSE, C_BTN, cancel_cb);
        }
        break;
    }
    case T_HAND: {
        int32_t bw = (W - 3 * GAP) / 4;
        opt_button(a, 0, bw, _("Ajustar"), C_BTN, fit_cb);
        opt_button(a, bw + GAP, bw, "100 %", C_BTN, actual_cb);
        opt_button(a, 2 * (bw + GAP), bw, LV_SYMBOL_MINUS, C_BTN, zout_cb);
        opt_button(a, 3 * (bw + GAP), bw, LV_SYMBOL_PLUS, C_BTN, zin_cb);
        break;
    }
    default:
        break;
    }
}

/* --------------------------------------------------------------------------
 * Building and entering
 * -------------------------------------------------------------------------- */

static lv_obj_t *make_handle(lv_obj_t *parent, bool round_turn)
{
    lv_obj_t *h = lv_obj_create(parent);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, 28, 28);
    lv_obj_set_style_radius(h, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(h, round_turn ? C_SEL : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(h, round_turn ? lv_color_hex(0xFFFFFF) : C_SEL, 0);
    lv_obj_set_style_border_width(h, 3, 0);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(h, LV_OBJ_FLAG_HIDDEN);
    return h;
}

static lv_obj_t *make_line(lv_obj_t *parent, lv_color_t c, int w)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_set_pos(l, 0, 0);
    lv_obj_set_style_line_color(l, c, 0);
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_style_line_dash_width(l, 10, 0);
    lv_obj_set_style_line_dash_gap(l, 6, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

void dj_ed_build(app_t *a, lv_obj_t *root)
{
    const layout_t *L = &a->L;
    a->ed = lv_obj_create(root);
    lv_obj_remove_style_all(a->ed);
    lv_obj_set_size(a->ed, L->W, L->H);
    lv_obj_remove_flag(a->ed, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->ed, LV_OBJ_FLAG_CLICKABLE);

    /* the canvas window first: everything else goes over it */
    a->view = lv_canvas_create(a->ed);
    lv_canvas_set_buffer(a->view, a->vbuf, L->cv_w, L->cv_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(a->view, L->cv_x, L->cv_y);
    lv_obj_set_style_radius(a->view, 0, 0);
    lv_image_set_antialias(a->view, false);
    lv_obj_remove_flag(a->view, LV_OBJ_FLAG_CLICKABLE);

    a->touch = lv_obj_create(a->ed);
    lv_obj_remove_style_all(a->touch);
    lv_obj_set_size(a->touch, L->cv_w, L->cv_h);
    lv_obj_set_pos(a->touch, L->cv_x, L->cv_y);
    lv_obj_add_flag(a->touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_RELEASED, a);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESS_LOST, a);
    aos_gesture_attach(a->touch, 0, gesture_cb, a);
    /* the wheel's catcher (wheel_cb): scrollable, but never by a finger */
    lv_obj_add_flag(a->touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->touch, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(a->touch, LV_SCROLLBAR_MODE_OFF);
    a->wheel_pad = lv_obj_create(a->touch);
    lv_obj_remove_style_all(a->wheel_pad);
    lv_obj_set_size(a->wheel_pad, 1, L->cv_h * 3);
    lv_obj_remove_flag(a->wheel_pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_update_layout(a->touch);
    lv_obj_scroll_to_y(a->touch, L->cv_h, LV_ANIM_OFF);
    lv_obj_add_event_cb(a->touch, wheel_cb, LV_EVENT_SCROLL_BEGIN, a);

    /* rulers, over the window's top and left edges */
    if (!a->rbuf_top) a->rbuf_top = dib_alloc((size_t)(L->W > L->H ? L->W : L->H) * RULER_W * 2);
    if (!a->rbuf_left) a->rbuf_left = dib_alloc((size_t)(L->W > L->H ? L->W : L->H) * RULER_W * 2);
    if (a->rbuf_top && a->rbuf_left) {
        a->ruler_top = lv_canvas_create(a->ed);
        lv_canvas_set_buffer(a->ruler_top, a->rbuf_top, L->cv_w, RULER_W, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(a->ruler_top, L->cv_x, L->cv_y);
        lv_obj_remove_flag(a->ruler_top, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(a->ruler_top, LV_OBJ_FLAG_HIDDEN);
        a->ruler_left = lv_canvas_create(a->ed);
        lv_canvas_set_buffer(a->ruler_left, a->rbuf_left, RULER_W, L->cv_h, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(a->ruler_left, L->cv_x, L->cv_y);
        lv_obj_remove_flag(a->ruler_left, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(a->ruler_left, LV_OBJ_FLAG_HIDDEN);
    }

    /* overlays */
    a->sel_box = lv_obj_create(a->ed);
    lv_obj_remove_style_all(a->sel_box);
    lv_obj_set_style_border_color(a->sel_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(a->sel_box, 2, 0);
    lv_obj_set_style_outline_color(a->sel_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_outline_width(a->sel_box, 2, 0);
    lv_obj_remove_flag(a->sel_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->sel_box, LV_OBJ_FLAG_HIDDEN);
    a->obj_line = make_line(a->ed, C_SEL, 2);
    a->poly_line = make_line(a->ed, lv_color_hex(0xFF9F0A), 3);
    lv_obj_set_style_line_dash_width(a->poly_line, 0, 0);
    for (int i = 0; i < 6; i++) a->handle[i] = make_handle(a->ed, i == 4);
    a->cursor = lv_obj_create(a->ed);
    lv_obj_remove_style_all(a->cursor);
    lv_obj_set_style_radius(a->cursor, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(a->cursor, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(a->cursor, 2, 0);
    lv_obj_set_style_outline_color(a->cursor, lv_color_hex(0x000000), 0);
    lv_obj_set_style_outline_width(a->cursor, 1, 0);
    lv_obj_remove_flag(a->cursor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->cursor, LV_OBJ_FLAG_HIDDEN);

    /* the top bar: back, undo, redo, zoom, colour, layers, menu */
    int32_t x = L->top_x, y = L->top_y, B = BAR_H;
    int32_t zw = L->top_w - 6 * (B + GAP);
    a->btn_back = glyph_button(a, a->ed, x, y, B, LV_SYMBOL_LEFT, aos_font_body, back_cb);
    a->btn_undo = icon_button(a, a->ed, x + (B + GAP), y, B, IC_UNDO, undo_cb, NULL);
    a->btn_redo = icon_button(a, a->ed, x + 2 * (B + GAP), y, B, IC_REDO, redo_cb, NULL);
    a->btn_zoom = dj_ui_button(a->ed, x + 3 * (B + GAP), y, zw, B, C_BTN, zoom_cb, a);
    a->lbl_zoom = aos_label(a->btn_zoom, "100 %", aos_font_small, AOS_C_TEXT);
    lv_obj_remove_flag(a->lbl_zoom, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(a->lbl_zoom);
    int32_t cx = x + 3 * (B + GAP) + zw + GAP;
    a->btn_color = dj_ui_button(a->ed, cx, y, B, B, C_BTN, color_cb, a);
    lv_obj_t *fillsw = lv_obj_create(a->btn_color);             /* child 0: the fill colour */
    lv_obj_remove_style_all(fillsw);
    lv_obj_set_size(fillsw, 34, 34);
    lv_obj_set_pos(fillsw, B - 46, B - 46);
    lv_obj_set_style_radius(fillsw, 8, 0);
    lv_obj_set_style_bg_opa(fillsw, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(fillsw, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(fillsw, 2, 0);
    lv_obj_remove_flag(fillsw, LV_OBJ_FLAG_CLICKABLE);
    a->sw_color = lv_obj_create(a->btn_color);
    lv_obj_remove_style_all(a->sw_color);
    lv_obj_set_size(a->sw_color, 40, 40);
    lv_obj_set_pos(a->sw_color, 12, 12);
    lv_obj_set_style_radius(a->sw_color, 10, 0);
    lv_obj_set_style_bg_opa(a->sw_color, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(a->sw_color, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(a->sw_color, 2, 0);
    lv_obj_remove_flag(a->sw_color, LV_OBJ_FLAG_CLICKABLE);
    a->btn_layers = icon_button(a, a->ed, cx + B + GAP, y, B, IC_LAYERS, layers_cb, NULL);
    a->btn_menu = glyph_button(a, a->ed, cx + 2 * (B + GAP), y, B, LV_SYMBOL_LIST, aos_font_body, menu_cb);

    /* the tools */
    for (int i = 0; i < T_COUNT; i++) {
        int32_t tx = L->land ? L->tool_x : L->tool_x + i * (L->tool_sz + 4);
        int32_t ty = L->land ? L->tool_y + i * (L->tool_sz + 4) : L->tool_y;
        a->btn_tool[i] = icon_button(a, a->ed, tx, ty, L->tool_sz, IC_PENCIL, tool_cb, &a->img_tool[i]);
        lv_obj_set_user_data(a->btn_tool[i], (void *)(intptr_t)i);
    }

    /* the options */
    a->opt = lv_obj_create(a->ed);
    lv_obj_remove_style_all(a->opt);
    lv_obj_set_size(a->opt, L->opt_w, BAR_H);
    lv_obj_set_pos(a->opt, L->opt_x, L->opt_y);
    lv_obj_remove_flag(a->opt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->opt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a->opt, LV_OBJ_FLAG_OVERFLOW_VISIBLE);     /* the pad's outline */
    a->opt_dirty = true;
}

void dj_ed_enter(app_t *a)
{
    lv_obj_remove_flag(a->ed, LV_OBJ_FLAG_HIDDEN);
    a->grab = G_NONE;
    a->ptr_down = false;
    a->pinching = false;
    dj_ed_view_fit(a);
    a->doc_dirty = (dib_rect_t){ 0, 0, 0, 0 };
    a->overlay_dirty = true;
    a->opt_dirty = true;
    a->cur_x = a->L.cv_w * 0.5f;
    a->cur_y = a->L.cv_h * 0.5f;
    rulers_update(a);
    dj_ed_refresh(a);
    dj_ed_keys_take(a, true);
    aos_pad_reset(&a->pad, now_ms());
}

void dj_ed_leave(app_t *a)
{
    if (a->grab == G_STROKE) {
        dib_stroke_end(&a->stroke);
        dj_mark_dirty(a);
    }
    a->grab = G_NONE;
    a->ptr_down = false;
    if (a->panel != P_NONE) dj_panel_close(a);
    dj_text_close(a);
    a->npoly = 0;
    a->sel = (dib_rect_t){ 0, 0, 0, 0 };
    dj_ed_keys_take(a, false);
    a->pad_menu_on = false;
    aos_pad_menu_clear(&a->pad_menu);
}

void dj_ed_resize_cleanup(app_t *a)
{
    if (a->grab == G_STROKE) {
        dib_stroke_end(&a->stroke);
        dj_mark_dirty(a);
    }
    a->grab = G_NONE;
    a->ptr_down = false;
    a->pad_menu_on = false;
    aos_pad_menu_clear(&a->pad_menu);
    dj_text_close(a);
    for (int i = 0; i < T_COUNT; i++) a->btn_tool[i] = a->img_tool[i] = NULL;
    a->ruler_top = a->ruler_left = NULL;
}

bool dj_ed_back(app_t *a)
{
    if (a->txt_card) {
        dj_text_close(a);
        return true;
    }
    if (a->pad_menu_on) {
        a->pad_menu_on = false;
        aos_pad_menu_set(&a->pad_menu, NULL, 0, 0);
        return true;
    }
    if (a->npoly || a->has_obj || !dib_rect_empty(&a->sel)) {
        if (a->npoly) a->npoly = 0;
        else if (a->has_obj) dj_ed_obj_commit(a);
        else a->sel = (dib_rect_t){ 0, 0, 0, 0 };
        a->overlay_dirty = true;
        a->opt_dirty = true;
        return true;
    }
    dj_go_gallery(a);
    return true;
}

/* --------------------------------------------------------------------------
 * The joystick: a cursor on the canvas, A is the finger
 * -------------------------------------------------------------------------- */

static void pad_collect(lv_obj_t *o, lv_obj_t **out, int *n, int max)
{
    uint32_t cnt = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < cnt && *n < max; i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_child_count(c) <= 2 &&
            !lv_obj_check_type(c, &lv_slider_class) && c != o) {
            out[(*n)++] = c;
        } else {
            pad_collect(c, out, n, max);
        }
    }
}

/* The buttons a pad can walk: the open panel's, or the bars'. */
static void pad_menu_fill(app_t *a)
{
    lv_obj_t *items[AOS_PAD_MENU_MAX];
    int n = 0;
    if (a->pnl) {
        pad_collect(a->pnl, items, &n, AOS_PAD_MENU_MAX);
    } else {
        lv_obj_t *top[] = { a->btn_back, a->btn_undo, a->btn_redo, a->btn_zoom, a->btn_color, a->btn_layers, a->btn_menu };
        for (size_t i = 0; i < sizeof top / sizeof top[0]; i++) items[n++] = top[i];
        for (int i = 0; i < T_COUNT; i++) items[n++] = a->btn_tool[i];
        pad_collect(a->opt, items, &n, AOS_PAD_MENU_MAX);
    }
    aos_pad_menu_set(&a->pad_menu, items, n, a->pnl ? 0 : 7 + (int)a->tool);
    a->pad_menu.shown = true;
    aos_pad_menu_set(&a->pad_menu, items, n, a->pnl ? 0 : 7 + (int)a->tool);
}

static void pad_step(app_t *a)
{
    uint32_t now = now_ms();
    float dt = (float)(now - a->pad_ms) / 1000.0f;
    a->pad_ms = now;
    if (dt > 0.1f) dt = 0.1f;
    aos_pad_update(&a->pad, now);
    aos_pad_t *p = &a->pad;
    if (!p->connected) {
        if (a->pad_seen) {
            a->pad_seen = false;
            a->overlay_dirty = true;
        }
        return;
    }
    if (p->pressed || p->x || p->y) {
        if (!a->pad_seen) a->overlay_dirty = true;
        a->pad_seen = true;
    }
    if (a->txt_card) return;
    /* panels and the bars: the outline walks the buttons */
    if (a->pnl || a->pad_menu_on) {
        bool was_panel = a->pnl != NULL;
        if (a->pad_menu.n == 0) pad_menu_fill(a);
        if (aos_pad_pressed(p, AOS_PAD_B | AOS_PAD_START)) {
            if (a->pnl) dj_panel_close(a);
            a->pad_menu_on = false;
            aos_pad_menu_set(&a->pad_menu, NULL, 0, 0);
            a->overlay_dirty = true;
            return;
        }
        aos_pad_menu_step(&a->pad_menu, p);
        if (!a->pad_menu_on && !was_panel) aos_pad_menu_set(&a->pad_menu, NULL, 0, 0);
        return;
    }
    if (aos_pad_pressed(p, AOS_PAD_START)) {
        if (a->ptr_down) ptr_up(a);
        a->pad_menu_on = true;
        pad_menu_fill(a);
        a->overlay_dirty = true;
        return;
    }
    /* the cursor: the stick by how far it leans, the D-pad faster the longer */
    float vx = 0, vy = 0;
    if (p->analog) {
        float sx = p->x / 32767.0f, sy = p->y / 32767.0f;
        vx = sx * fabsf(sx) * 900.0f;
        vy = sy * fabsf(sy) * 900.0f;
    }
    uint32_t dirs = p->held & AOS_PAD_DIRS;
    static uint32_t held_since;
    if (!dirs) held_since = now;
    float sp = 120.0f + clampf((float)(now - held_since) / 1000.0f, 0, 1) * 600.0f;
    if (dirs & AOS_PAD_LEFT) vx = -sp;
    if (dirs & AOS_PAD_RIGHT) vx = sp;
    if (dirs & AOS_PAD_UP) vy = -sp;
    if (dirs & AOS_PAD_DOWN) vy = sp;
    if (aos_pad_pressed(p, AOS_PAD_DIRS)) {
        /* a single press is one pixel of the document, for precision */
        if (p->pressed & AOS_PAD_LEFT) a->cur_x -= a->vs;
        if (p->pressed & AOS_PAD_RIGHT) a->cur_x += a->vs;
        if (p->pressed & AOS_PAD_UP) a->cur_y -= a->vs;
        if (p->pressed & AOS_PAD_DOWN) a->cur_y += a->vs;
        a->overlay_dirty = true;
    }
    if (vx || vy) {
        a->cur_x = clampf(a->cur_x + vx * dt, 0, (float)a->L.cv_w - 1);
        a->cur_y = clampf(a->cur_y + vy * dt, 0, (float)a->L.cv_h - 1);
        a->overlay_dirty = true;
        if (a->ptr_down) ptr_move(a, a->cur_x, a->cur_y);
    }
    if (aos_pad_pressed(p, AOS_PAD_A)) ptr_down(a, a->cur_x, a->cur_y);
    if ((p->released & AOS_PAD_A) && a->ptr_down) ptr_up(a);
    /* L and R: the brush's size; with B held, the zoom */
    if (aos_pad_held(p, AOS_PAD_B) && (p->pressed & (AOS_PAD_L | AOS_PAD_R))) {
        dj_ed_zoom_at(a, (p->pressed & AOS_PAD_R) ? 1.25f : 0.8f, a->cur_x, a->cur_y);
        a->pad_b_combo = true;
    } else if (p->pressed & (AOS_PAD_L | AOS_PAD_R)) {
        float k = (p->pressed & AOS_PAD_R) ? 1.25f : 0.8f;
        if (a->tool == T_ERASER) a->eraser_size = clampf(roundf(a->eraser_size * k + (k > 1 ? 1 : -1)), 1, 300);
        else if (a->tool == T_BRUSH) {
            float *s = &a->brush_size[a->brush.kind];
            *s = clampf(roundf(*s * k + (k > 1 ? 1 : -1)), 1, 300);
        } else {
            dj_ed_zoom_at(a, k, a->cur_x, a->cur_y);
        }
        a->opt_dirty = true;
        a->overlay_dirty = true;
    }
    if (p->released & AOS_PAD_B) {
        if (!a->pad_b_combo) dj_ed_undo(a);
        a->pad_b_combo = false;
    }
}

/* --------------------------------------------------------------------------
 * The keyboard
 * -------------------------------------------------------------------------- */

bool dj_ed_key(app_t *a, uint32_t key, uint8_t mods)
{
    bool ctrl = (mods & 0x99) != 0;     /* Ctrl or the Mac's command, left or right */
    bool shift = (mods & 0x22) != 0;
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    if (a->panel != P_NONE && key == AOS_KEY_ESC) {
        dj_panel_close(a);
        return true;
    }
    if (ctrl) {
        switch (key) {
        case 'z': if (shift) dj_ed_redo(a); else dj_ed_undo(a); return true;
        case 'y': dj_ed_redo(a); return true;
        case 's': if (a->has_obj) dj_ed_obj_commit(a); dj_save(a); dj_toast(_("Guardado")); return true;
        case 'c': sel_copy(a); return true;
        case 'x': sel_copy(a); sel_delete(a); return true;
        case 'v': sel_paste(a); return true;
        case 'a': sel_all(a); return true;
        case 'd': a->sel = (dib_rect_t){ 0, 0, 0, 0 }; a->overlay_dirty = a->opt_dirty = true; return true;
        case 'e': dj_export_png(a); return true;
        default: return false;
        }
    }
    switch (key) {
    case 'b': dj_ed_set_tool(a, T_BRUSH); return true;
    case 'e': dj_ed_set_tool(a, T_ERASER); return true;
    case 'u':
        if (a->tool == T_SHAPE) {
            a->shape = (a->shape + 1) % DIB_OBJ_BITMAP;
            a->opt_dirty = true;
        }
        dj_ed_set_tool(a, T_SHAPE);
        return true;
    case 't': dj_ed_set_tool(a, T_TEXT); return true;
    case 'g': dj_ed_set_tool(a, T_FILL); return true;
    case 'i': dj_ed_set_tool(a, T_PICK); return true;
    case 'm': dj_ed_set_tool(a, T_SELECT); return true;
    case 'h':
    case ' ': dj_ed_set_tool(a, T_HAND); return true;
    case 'x': {
        dib_px_t c = a->color;
        dj_ed_set_color(a, a->color2, false);
        dj_ed_set_color(a, c, true);
        return true;
    }
    case '+':
    case '=': dj_ed_zoom_at(a, 1.25f, a->L.cv_w * 0.5f, a->L.cv_h * 0.5f); return true;
    case '-': dj_ed_zoom_at(a, 0.8f, a->L.cv_w * 0.5f, a->L.cv_h * 0.5f); return true;
    case '0': dj_ed_view_fit(a); return true;
    case '1': dj_ed_view_actual(a); return true;
    case '[':
    case ']': {
        float k = key == ']' ? 1.2f : 1.0f / 1.2f;
        float *s = a->tool == T_ERASER ? &a->eraser_size : &a->brush_size[a->brush.kind];
        *s = clampf(roundf(*s * k + (key == ']' ? 1 : -1)), 1, 300);
        a->opt_dirty = true;
        return true;
    }
    case AOS_KEY_ENTER:
        if (a->npoly) poly_finish(a);
        else if (a->has_obj) dj_ed_obj_commit(a);
        return true;
    case AOS_KEY_ESC:
        if (a->npoly) a->npoly = 0;
        else if (a->has_obj) dj_ed_obj_cancel(a);
        else a->sel = (dib_rect_t){ 0, 0, 0, 0 };
        a->overlay_dirty = a->opt_dirty = true;
        return true;
    case AOS_KEY_DEL:
    case AOS_KEY_BACKSPACE:
        if (a->has_obj) dj_ed_obj_cancel(a);
        else sel_delete(a);
        return true;
    case AOS_KEY_LEFT:
    case AOS_KEY_RIGHT:
    case AOS_KEY_UP:
    case AOS_KEY_DOWN:
        if (a->has_obj) {
            float st = shift ? 10.0f : 1.0f;
            a->obj.cx += key == AOS_KEY_LEFT ? -st : key == AOS_KEY_RIGHT ? st : 0;
            a->obj.cy += key == AOS_KEY_UP ? -st : key == AOS_KEY_DOWN ? st : 0;
            dj_ed_obj_show(a);
        } else {
            float st = shift ? 200.0f : 60.0f;
            a->vox -= key == AOS_KEY_LEFT ? -st : key == AOS_KEY_RIGHT ? st : 0;
            a->voy -= key == AOS_KEY_UP ? -st : key == AOS_KEY_DOWN ? st : 0;
            view_clamp(a);
            a->view_full = true;
        }
        return true;
    default:
        return false;
    }
}

/* --------------------------------------------------------------------------
 * The frame
 * -------------------------------------------------------------------------- */

#ifdef AOS_SIM_BUILTIN
/* DIB_KEYS and DIB_WHEEL (dibujo.c), a second after the editor shows */
static void sim_inputs(app_t *a)
{
    static uint32_t t0;
    static bool done;
    if (done) return;
    if (!t0) t0 = now_ms();
    if (now_ms() - t0 < 1000) return;
    done = true;
    const char *k = getenv("DIB_KEYS");
    if (k && k[0]) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s", k);
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            uint8_t mods = 0;
            if (tok[0] == '^' && tok[1]) {
                mods = 0x01;
                tok++;
            }
            uint32_t key = (uint32_t)(unsigned char)tok[0];
            if (!strcmp(tok, "esc")) key = AOS_KEY_ESC;
            else if (!strcmp(tok, "enter")) key = AOS_KEY_ENTER;
            else if (!strcmp(tok, "del")) key = AOS_KEY_DEL;
            else if (!strcmp(tok, "left")) key = AOS_KEY_LEFT;
            else if (!strcmp(tok, "space")) key = ' ';
            dj_ed_key(a, key, mods);
        }
    }
    const char *w = getenv("DIB_WHEEL");
    if (w && w[0]) {
        /* through the very path the system's mouse takes */
        int n = atoi(w);
        lv_obj_scroll_by_bounded(a->touch, 0, n * 90, LV_ANIM_ON);
    }
    const char *t = getenv("DIB_TOOL");
    if (t && t[0]) dj_ed_set_tool(a, (tool_t)atoi(t));
    const char *p = getenv("DIB_PANEL");
    if (p && p[0]) dj_panel_open(a, (panel_t)atoi(p));
}
#endif

void dj_ed_frame(app_t *a)
{
    if (!a->doc || lv_obj_has_flag(a->ed, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_get_user_data(a->btn_back)) {
        /* the back arrow, acted on outside its own event */
        lv_obj_set_user_data(a->btn_back, NULL);
        dj_go_gallery(a);
        return;
    }
#ifdef AOS_SIM_BUILTIN
    sim_inputs(a);
#endif
    if (!a->job) pad_step(a);
    if (a->grab == G_STROKE) {
        uint32_t now = now_ms();
        if (now - a->move_ms > 30) {
            dib_stroke_hold(&a->stroke, (float)FRAME_MS);
        }
        dib_rect_t r;
        while (dib_stroke_take_dirty(&a->stroke, &r)) dib_rect_union(&a->doc_dirty, &r);
    }
    if (!dib_rect_empty(&a->doc_dirty)) {
        dib_rect_t r = a->doc_dirty;
        a->doc_dirty = (dib_rect_t){ 0, 0, 0, 0 };
        show_doc_rect(a, r);
    }
    if (a->view_full) {
        a->view_full = false;
        render_win(a, 0, 0, a->L.cv_w, a->L.cv_h);
        lv_obj_invalidate(a->view);
        if (a->rulers) rulers_update(a);
        a->overlay_dirty = true;
        dj_ed_refresh(a);
    }
    if (a->overlay_dirty) {
        a->overlay_dirty = false;
        overlay_update(a);
    }
    if (a->opt_dirty) {
        a->opt_dirty = false;
        opt_build(a);
        if (a->pad_menu_on) pad_menu_fill(a);
    }
    if (a->doc->oom) {
        a->doc->oom = false;
        dj_toast(_("Sin memoria: se borró historia o no entra más"));
    }
}

/* Called by the menu when the rulers turn on or off. */
void dj_ed_rulers_changed(app_t *a)
{
    rulers_update(a);
    a->view_full = true;
}
