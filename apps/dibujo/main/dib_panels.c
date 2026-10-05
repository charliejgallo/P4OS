/*
 * DIBUJO - the cards over the canvas: brushes, shapes, colour, layers, the
 * menu and the pictures to import; the text entry; the size of a new
 * drawing.
 *
 * A panel exists only while it is open. Opening, closing and rebuilding
 * are asked for with a flag and done by the frame timer (dj_panel_frame), so
 * no panel is ever deleted from inside one of its own buttons' events.
 * Upright a panel covers the canvas window; lying down it takes the whole
 * height beside the tools, which is what a colour picker needs.
 */
#include "dib_app.h"
#include "aos_text_safe.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lv_font_t *const *const dj_text_fonts[NFONTS] = {
    &aos_font_tiny, &aos_font_caption, &aos_font_small, &aos_font_body,
    &aos_font_title, &aos_font_large, &aos_font_huge,
};
const int dj_text_font_px[NFONTS] = { 16, 20, 24, 28, 36, 48, 64 };

const int dj_sizes[NSIZES][2] = {
    { 720, 1280 }, { 1280, 720 }, { 905, 1280 }, { 1280, 905 }, { 1024, 1024 },
};

/* --------------------------------------------------------------------------
 * Colour maths
 * -------------------------------------------------------------------------- */

static dib_px_t hsv_rgb(float h, float s, float v)
{
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    float c = v * s, x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
    float r, g, b;
    if (h < 60) { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }
    return DIB_ARGB(255, (unsigned)((r + m) * 255 + 0.5f), (unsigned)((g + m) * 255 + 0.5f),
                    (unsigned)((b + m) * 255 + 0.5f));
}

static void rgb_hsv(dib_px_t p, float *h, float *s, float *v)
{
    float r = DIB_R(p) / 255.0f, g = DIB_G(p) / 255.0f, b = DIB_B(p) / 255.0f;
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    if (d <= 0) return;                 /* grey: the hue stays as it was */
    float hh;
    if (mx == r) hh = 60.0f * fmodf((g - b) / d, 6.0f);
    else if (mx == g) hh = 60.0f * ((b - r) / d + 2.0f);
    else hh = 60.0f * ((r - g) / d + 4.0f);
    *h = hh < 0 ? hh + 360.0f : hh;
}

static inline uint16_t p565(dib_px_t p)
{
    return dib_565(p);
}

/* --------------------------------------------------------------------------
 * The frame of a panel
 * -------------------------------------------------------------------------- */

static void close_cb(lv_event_t *e)
{
    dj_panel_close(lv_event_get_user_data(e));
}

void dj_panel_open(app_t *a, panel_t p)
{
    if (p == P_NONE) {
        dj_panel_close(a);
        return;
    }
    if (a->has_obj && (p == P_LAYERS || p == P_MENU || p == P_IMPORT)) dj_ed_obj_commit(a);
    a->pnl_open_req = p;
}

void dj_panel_close(app_t *a)
{
    a->pnl_close_req = true;
    a->pnl_open_req = P_NONE;
}

void dj_panel_refresh(app_t *a)
{
    if (a->panel == P_LAYERS || a->panel == P_MENU) a->pnl_rebuild = true;
}

/* The card, a title and a close button; returns the content area's top. */
static lv_obj_t *panel_card(app_t *a, const char *title, int32_t *cw, int32_t *ch)
{
    const layout_t *L = &a->L;
    int32_t x, y, w, h;
    if (!L->land) {
        x = GAP;
        y = L->cv_y;
        w = L->W - 2 * GAP;
        h = L->cv_h;
    } else {
        x = L->cv_x;
        y = GAP;
        w = L->W - L->cv_x - GAP;
        h = L->H - 2 * GAP;
    }
    lv_obj_t *c = dj_ui_card(a->ed, x, y, w, h);
    a->pnl = c;
    lv_obj_t *t = aos_label(c, title, aos_font_title, AOS_C_TEXT);
    lv_obj_set_pos(t, 24, 18);
    lv_obj_t *b = dj_ui_button(c, w - 16 - 72, 10, 72, 64, C_BTN, close_cb, a);
    lv_obj_t *l = aos_label(b, LV_SYMBOL_CLOSE, aos_font_body, AOS_C_TEXT);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(l);
    *cw = w;
    *ch = h;
    return c;
}

/* A scrolling column under the title. */
static lv_obj_t *panel_column(lv_obj_t *card, int32_t w, int32_t h)
{
    lv_obj_t *col = lv_obj_create(card);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, 0, 84);
    lv_obj_set_size(col, w, h - 84 - 8);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(col, 20, 0);
    lv_obj_set_style_pad_bottom(col, 16, 0);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_set_style_pad_column(col, 12, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    return col;
}

static lv_obj_t *section(lv_obj_t *col, int32_t w, const char *text)
{
    lv_obj_t *l = aos_label(col, text, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(l, w - 40);
    lv_obj_set_style_pad_top(l, 8, 0);
    return l;
}

/* --------------------------------------------------------------------------
 * Brushes
 * -------------------------------------------------------------------------- */

static void brush_kind_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    a->brush.kind = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (a->tool != T_BRUSH) dj_ed_set_tool(a, T_BRUSH);
    a->opt_dirty = true;
    dj_ed_refresh(a);
    dj_panel_close(a);
}

static void smooth_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    lv_obj_t *s = lv_event_get_target_obj(e);
    a->brush.smooth = lv_slider_get_value(s) / 100.0f;
    lv_obj_t *l = lv_obj_get_user_data(s);
    if (l) lv_label_set_text_fmt(l, "%s: %d %%", _("Suavizado del trazo"), (int)lv_slider_get_value(s));
}

static void sym_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    a->sym = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    a->view_full = true;
    a->pnl_rebuild = true;
}

static lv_obj_t *slider_row(lv_obj_t *col, int32_t w, int min, int max, int val, lv_event_cb_t cb, app_t *a,
                            lv_obj_t *lbl)
{
    lv_obj_t *s = lv_slider_create(col);
    lv_obj_set_size(s, w - 80, 20);
    lv_obj_set_style_margin_hor(s, 20, 0);
    lv_obj_set_style_margin_ver(s, 16, 0);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x48484A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, C_SEL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 10, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 24);
    lv_obj_set_user_data(s, lbl);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, a);
    return s;
}

static lv_obj_t *icon_choice(app_t *a, lv_obj_t *col, int32_t w, icon_t ic, const char *name, bool on,
                             lv_event_cb_t cb, int value)
{
    lv_obj_t *b = dj_ui_button(col, 0, 0, w, 128, on ? C_SEL : C_BTN, cb, a);
    lv_obj_set_user_data(b, (void *)(intptr_t)value);
    lv_obj_t *im = lv_image_create(b);
    lv_image_set_src(im, dj_icon_get(ic));
    lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(im, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_t *l = aos_label_boxed(b, name, aos_font_caption, AOS_C_TEXT, w - 8, 26);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -14);
    return b;
}

static void build_brushes(app_t *a)
{
    int32_t w, h;
    lv_obj_t *c = panel_card(a, _("Pinceles"), &w, &h);
    lv_obj_t *col = panel_column(c, w, h);
    static const char *const names[4] = { N_("Lápiz"), N_("Pincel suave"), N_("Aerógrafo"), N_("Marcador") };
    static const icon_t icons[4] = { IC_PENCIL, IC_SOFT, IC_AIR, IC_MARKER };
    int32_t bw = (w - 40 - 3 * 12) / 4;
    for (int i = 0; i < 4; i++) icon_choice(a, col, bw, icons[i], _(names[i]), a->brush.kind == i, brush_kind_cb, i);
    lv_obj_t *l = section(col, w, "");
    lv_label_set_text_fmt(l, "%s: %d %%", _("Suavizado del trazo"), (int)(a->brush.smooth * 100 + 0.5f));
    slider_row(col, w, 0, 100, (int)(a->brush.smooth * 100 + 0.5f), smooth_cb, a, l);
    section(col, w, _("Simetría"));
    static const char *const sym[4] = { N_("Ninguna"), N_("Lado a lado"), N_("Arriba y abajo"), N_("Las dos") };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = dj_ui_text_button(col, _(sym[i]), aos_font_small, bw, 72, a->sym == i ? C_SEL : C_BTN, sym_cb, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
    }
}

/* --------------------------------------------------------------------------
 * Shapes
 * -------------------------------------------------------------------------- */

static void shape_kind_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    a->shape = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (a->tool != T_SHAPE) dj_ed_set_tool(a, T_SHAPE);
    a->opt_dirty = true;
    dj_ed_refresh(a);
    dj_panel_close(a);
}

static void radius_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    lv_obj_t *s = lv_event_get_target_obj(e);
    a->shape_radius = (float)lv_slider_get_value(s);
    lv_obj_t *l = lv_obj_get_user_data(s);
    if (l) lv_label_set_text_fmt(l, "%s: %d px", _("Esquinas redondeadas"), (int)a->shape_radius);
    if (a->has_obj && a->obj.kind == DIB_OBJ_RECT) {
        a->obj.radius = a->shape_radius;
        dj_ed_obj_show(a);
    }
}

static void stroke_toggle_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    a->shape_stroke = !a->shape_stroke;
    if (!a->shape_stroke && !a->shape_fill) a->shape_fill = true;     /* something must show */
    if (a->has_obj && a->obj.kind != DIB_OBJ_BITMAP && a->obj.kind != DIB_OBJ_LINE && a->obj.kind != DIB_OBJ_ARROW) {
        a->obj.stroke = a->shape_stroke;
        a->obj.fill = a->shape_fill;
        dj_ed_obj_show(a);
    }
    a->opt_dirty = true;
    a->pnl_rebuild = true;
}

static void build_shapes(app_t *a)
{
    int32_t w, h;
    lv_obj_t *c = panel_card(a, _("Formas"), &w, &h);
    lv_obj_t *col = panel_column(c, w, h);
    static const char *const names[5] = { N_("Línea"), N_("Rectángulo"), N_("Elipse"), N_("Polígono"), N_("Flecha") };
    static const icon_t icons[5] = { IC_LINE, IC_RECT, IC_ELLIPSE, IC_POLY, IC_ARROW };
    int32_t bw = (w - 40 - 4 * 12) / 5;
    for (int i = 0; i < 5; i++) icon_choice(a, col, bw, icons[i], _(names[i]), a->shape == i, shape_kind_cb, i);
    lv_obj_t *l = section(col, w, "");
    lv_label_set_text_fmt(l, "%s: %d px", _("Esquinas redondeadas"), (int)a->shape_radius);
    slider_row(col, w, 0, 200, (int)a->shape_radius, radius_cb, a, l);
    int32_t hw = (w - 40 - 12) / 2;
    dj_ui_text_button(col, a->shape_stroke ? _("Con borde") : _("Sin borde"), aos_font_small, hw, 72,
                   a->shape_stroke ? C_SEL : C_BTN, stroke_toggle_cb, a);
    lv_obj_t *n = section(col, w, _("El borde va con el color de trazo y el relleno con el de relleno (los dos cuadrados de arriba)."));
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
}

/* --------------------------------------------------------------------------
 * Colour
 * -------------------------------------------------------------------------- */

static void sv_draw(app_t *a)
{
    int n = a->sv_side;
    for (int y = 0; y < n; y++) {
        float v = 1.0f - (float)y / (float)(n - 1);
        for (int x = 0; x < n; x++) {
            a->sv_buf[y * n + x] = p565(hsv_rgb(a->hue, (float)x / (float)(n - 1), v));
        }
    }
    lv_obj_invalidate(a->sv_canvas);
}

static void color_marks(app_t *a)
{
    if (!a->sv_canvas) return;
    int n = a->sv_side;
    lv_obj_set_pos(a->sv_mark, lv_obj_get_x(a->sv_canvas) + (int32_t)(a->sat * (n - 1)) - 14,
                   lv_obj_get_y(a->sv_canvas) + (int32_t)((1.0f - a->val) * (n - 1)) - 14);
    int32_t hw = lv_obj_get_width(a->hue_canvas);
    lv_obj_set_pos(a->hue_mark, lv_obj_get_x(a->hue_canvas) + (int32_t)(a->hue / 360.0f * (hw - 1)) - 6,
                   lv_obj_get_y(a->hue_canvas) - 4);
    dib_px_t c = a->edit_fill ? a->color2 : a->color;
    lv_obj_set_style_bg_color(a->col_prev, dj_ui_color(c), 0);
    lv_label_set_text_fmt(a->col_hex, "#%06X", (unsigned)(c & 0xFFFFFF));
}

static void color_apply(app_t *a)
{
    dj_ed_set_color(a, hsv_rgb(a->hue, a->sat, a->val), a->edit_fill);
    color_marks(a);
}

static void sv_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t co;
    lv_obj_get_coords(a->sv_canvas, &co);
    float n = (float)(a->sv_side - 1);
    float s = (float)(p.x - co.x1) / n, v = 1.0f - (float)(p.y - co.y1) / n;
    a->sat = s < 0 ? 0 : s > 1 ? 1 : s;
    a->val = v < 0 ? 0 : v > 1 ? 1 : v;
    color_apply(a);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        dj_ed_push_recent(a, a->edit_fill ? a->color2 : a->color);
        a->pnl_rebuild = true;
    }
}

static void hue_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t co;
    lv_obj_get_coords(a->hue_canvas, &co);
    float h = (float)(p.x - co.x1) / (float)(co.x2 - co.x1) * 360.0f;
    a->hue = h < 0 ? 0 : h > 359.9f ? 359.9f : h;
    if (a->sat < 0.05f) a->sat = 1.0f;          /* a hue on grey would show nothing */
    if (a->val < 0.05f) a->val = 1.0f;
    sv_draw(a);
    color_apply(a);
}

static void target_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    a->edit_fill = (bool)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    rgb_hsv(a->edit_fill ? a->color2 : a->color, &a->hue, &a->sat, &a->val);
    a->pnl_rebuild = true;
}

static void swatch_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_current_target_obj(e);
    dib_px_t c = (dib_px_t)(uintptr_t)lv_obj_get_user_data(b);
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        /* out of the palette (a recent one only goes when it ages) */
        palette_t *p = &a->pal[a->pal_cur];
        for (int i = 0; i < p->n; i++) {
            if (p->c[i] == c) {
                memmove(&p->c[i], &p->c[i + 1], sizeof(dib_px_t) * (p->n - i - 1));
                p->n--;
                dj_palettes_save(a);
                dj_toast(_("Quitado de la paleta"));
                a->pnl_rebuild = true;
                lv_indev_wait_release(lv_indev_active());
                return;
            }
        }
        return;
    }
    rgb_hsv(c, &a->hue, &a->sat, &a->val);
    dj_ed_set_color(a, c, a->edit_fill);
    sv_draw(a);
    color_marks(a);
}

static void pal_tab_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    a->pal_cur = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    a->pnl_rebuild = true;
}

static void pal_add_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    palette_t *p = &a->pal[a->pal_cur];
    dib_px_t c = a->edit_fill ? a->color2 : a->color;
    if (p->n >= PAL_COLORS) {
        dj_toast(_("La paleta está llena"));
        return;
    }
    p->c[p->n++] = c | 0xFF000000u;
    dj_palettes_save(a);
    a->pnl_rebuild = true;
}

static lv_obj_t *swatch(app_t *a, lv_obj_t *parent, int32_t x, int32_t y, int32_t s, dib_px_t c)
{
    lv_obj_t *b = dj_ui_button(parent, x, y, s, s, dj_ui_color(c), swatch_cb, a);
    lv_obj_add_event_cb(b, swatch_cb, LV_EVENT_LONG_PRESSED, a);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x5A5A64), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_user_data(b, (void *)(uintptr_t)c);
    return b;
}

static void build_color(app_t *a)
{
    int32_t w, h;
    lv_obj_t *c = panel_card(a, _("Color"), &w, &h);
    const bool land = a->L.land;
    if (a->hue < 0 || a->hue >= 360) a->hue = 0;

    /* stroke or fill */
    int32_t tw = 150, tx = w - 16 - 72 - 12 - 2 * tw - 12;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = dj_ui_button(c, tx + i * (tw + 12), 10, tw, 64, a->edit_fill == i ? C_SEL : C_BTN, target_cb, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_t *sw = lv_obj_create(b);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, 32, 32);
        lv_obj_align(sw, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_set_style_radius(sw, 8, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sw, dj_ui_color(i ? a->color2 : a->color), 0);
        lv_obj_set_style_border_color(sw, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(sw, 2, 0);
        lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = aos_label(b, i ? _("Relleno") : _("Trazo"), aos_font_caption, AOS_C_TEXT);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 54, 0);
    }

    /* the square and the hue bar */
    int32_t side = land ? h - 84 - 100 : (w - 60) / 2;
    if (side > 340) side = 340;
    if (side < 160) side = 160;
    a->sv_side = side;
    dib_free(a->sv_buf);
    a->sv_buf = dib_alloc((size_t)side * side * 2);
    int32_t hue_w = land ? side : w - 48;
    dib_free(a->hue_buf);
    a->hue_buf = dib_alloc((size_t)hue_w * 40 * 2);
    if (!a->sv_buf || !a->hue_buf) return;
    int32_t x0 = 24, y0 = 90;
    a->sv_canvas = lv_canvas_create(c);
    lv_canvas_set_buffer(a->sv_canvas, a->sv_buf, side, side, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(a->sv_canvas, x0, y0);
    lv_obj_add_flag(a->sv_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->sv_canvas, sv_cb, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->sv_canvas, sv_cb, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(a->sv_canvas, sv_cb, LV_EVENT_RELEASED, a);
    sv_draw(a);
    a->hue_canvas = lv_canvas_create(c);
    lv_canvas_set_buffer(a->hue_canvas, a->hue_buf, hue_w, 40, LV_COLOR_FORMAT_RGB565);
    for (int x = 0; x < hue_w; x++) {
        uint16_t col = p565(hsv_rgb((float)x / (float)(hue_w - 1) * 360.0f, 1, 1));
        for (int y = 0; y < 40; y++) a->hue_buf[y * hue_w + x] = col;
    }
    lv_obj_set_pos(a->hue_canvas, x0, y0 + side + 20);
    lv_obj_add_flag(a->hue_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(a->hue_canvas, 12);
    lv_obj_add_event_cb(a->hue_canvas, hue_cb, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->hue_canvas, hue_cb, LV_EVENT_PRESSING, a);

    a->sv_mark = lv_obj_create(c);
    lv_obj_remove_style_all(a->sv_mark);
    lv_obj_set_size(a->sv_mark, 28, 28);
    lv_obj_set_style_radius(a->sv_mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(a->sv_mark, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(a->sv_mark, 3, 0);
    lv_obj_set_style_outline_color(a->sv_mark, lv_color_hex(0x000000), 0);
    lv_obj_set_style_outline_width(a->sv_mark, 1, 0);
    lv_obj_remove_flag(a->sv_mark, LV_OBJ_FLAG_CLICKABLE);
    a->hue_mark = lv_obj_create(c);
    lv_obj_remove_style_all(a->hue_mark);
    lv_obj_set_size(a->hue_mark, 12, 48);
    lv_obj_set_style_radius(a->hue_mark, 4, 0);
    lv_obj_set_style_border_color(a->hue_mark, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(a->hue_mark, 3, 0);
    lv_obj_remove_flag(a->hue_mark, LV_OBJ_FLAG_CLICKABLE);

    /* the right column (upright: right of the square; lying down too) */
    int32_t rx = x0 + side + 24, rw = w - rx - 24, ry = y0;
    a->col_prev = lv_obj_create(c);
    lv_obj_remove_style_all(a->col_prev);
    lv_obj_set_size(a->col_prev, rw, 72);
    lv_obj_set_pos(a->col_prev, rx, ry);
    lv_obj_set_style_radius(a->col_prev, 14, 0);
    lv_obj_set_style_bg_opa(a->col_prev, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(a->col_prev, lv_color_hex(0x5A5A64), 0);
    lv_obj_set_style_border_width(a->col_prev, 1, 0);
    lv_obj_remove_flag(a->col_prev, LV_OBJ_FLAG_CLICKABLE);
    a->col_hex = aos_label(c, "", aos_font_body, AOS_C_TEXT);
    lv_obj_set_pos(a->col_hex, rx, ry + 84);
    lv_obj_t *l = aos_label(c, _("Recientes"), aos_font_caption, AOS_C_DIM);
    lv_obj_set_pos(l, rx, ry + 130);
    int32_t ss = (rw - 4 * 10) / 5;
    if (ss > 64) ss = 64;
    for (int i = 0; i < a->nrecent; i++) {
        swatch(a, c, rx + (i % 5) * (ss + 10), ry + 160 + (i / 5) * (ss + 10), ss, a->recent[i]);
    }

    /* palettes: tabs and their colours */
    int32_t py = land ? ry + 160 + 2 * (ss + 10) + 8 : y0 + side + 80;
    int32_t px = land ? rx : 24, pw = land ? rw : w - 48;
    int32_t tbw = (pw - 3 * 8) / NPALETTES;
    for (int p = 0; p < NPALETTES; p++) {
        lv_obj_t *b = dj_ui_text_button(c, _(a->pal[p].name), aos_font_caption, tbw, 52,
                                     a->pal_cur == p ? C_SEL : C_BTN, pal_tab_cb, a);
        lv_obj_set_pos(b, px + p * (tbw + 8), py);
        lv_obj_set_user_data(b, (void *)(intptr_t)p);
    }
    palette_t *pal = &a->pal[a->pal_cur];
    int cols = 8;
    int32_t ps = (pw - (cols - 1) * 8) / cols;
    if (land && ps > 52) ps = 52;
    for (int i = 0; i <= pal->n && i < PAL_COLORS + 1; i++) {
        int32_t sx = px + (i % cols) * (ps + 8), sy = py + 64 + (i / cols) * (ps + 8);
        if (i == pal->n) {
            if (pal->n >= PAL_COLORS) break;
            lv_obj_t *b = dj_ui_button(c, sx, sy, ps, ps, C_BTN, pal_add_cb, a);
            lv_obj_t *pl = aos_label(b, LV_SYMBOL_PLUS, aos_font_body, AOS_C_TEXT);
            lv_obj_remove_flag(pl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_center(pl);
        } else {
            swatch(a, c, sx, sy, ps, pal->c[i]);
        }
    }
    if (!land) {
        lv_obj_t *n = aos_label(c, _("Mantené apretado un color para sacarlo de la paleta"), aos_font_caption, AOS_C_DIM);
        lv_obj_set_pos(n, 24, py + 64 + 2 * (ps + 8) + 8);
    }
    lv_obj_update_layout(c);            /* the marks read the canvases' places */
    color_marks(a);
}

/* --------------------------------------------------------------------------
 * Layers
 * -------------------------------------------------------------------------- */

static void layer_row_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (i >= 0 && i < a->doc->nlayers && i != a->doc->active) {
        if (a->has_obj) dj_ed_obj_commit(a);
        a->doc->active = i;
        a->pnl_rebuild = true;
    }
}

static void layer_eye_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (i < 0 || i >= a->doc->nlayers) return;
    dib_layer_t *l = &a->doc->layers[i];
    dib_layer_props(a->doc, i, l->opacity, !l->visible);
    dj_ed_all_changed(a);
    a->pnl_rebuild = true;
}

static void layer_op_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    int op = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    dib_doc_t *d = a->doc;
    int i = d->active;
    char name[DIB_NAME_MAX];
    bool ok = true;
    if (op != 5) a->confirm = false;
    switch (op) {
    case 0:
        if (d->nlayers >= DIB_MAX_LAYERS) {
            dj_toast(_("Ya hay 8 capas"));
            return;
        }
        {
            int n = 1;
            for (int k = 0; k < d->nlayers; k++) {
                int v;
                if (sscanf(d->layers[k].name, "%*s %d", &v) == 1 && v >= n) n = v + 1;
            }
            snprintf(name, sizeof name, "%s %d", _("Capa"), n);
        }
        ok = dib_layer_add(d, i + 1, name) >= 0;
        break;
    case 1:
        ok = dib_layer_dup(d, i) >= 0;
        break;
    case 2:
        ok = dib_layer_move(d, i, i + 1);
        break;
    case 3:
        ok = dib_layer_move(d, i, i - 1);
        break;
    case 4:
        ok = dib_layer_merge_down(d, i);
        break;
    case 5:
        if (d->nlayers <= 1) return;
        if (!a->confirm) {
            a->confirm = true;
            a->pnl_rebuild = true;
            return;
        }
        a->confirm = false;
        ok = dib_layer_delete(d, i);
        break;
    default:
        return;
    }
    if (!ok && op != 2 && op != 3) dj_toast(_("No se pudo"));
    dj_ed_all_changed(a);
    dj_ed_refresh(a);
    a->pnl_rebuild = true;
}

static void layer_opacity_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    lv_obj_t *s = lv_event_get_target_obj(e);
    dib_layer_t *l = &a->doc->layers[a->doc->active];
    dib_layer_props(a->doc, a->doc->active, (uint8_t)(lv_slider_get_value(s) * 255 / 100), l->visible);
    lv_obj_t *lbl = lv_obj_get_user_data(s);
    if (lbl) lv_label_set_text_fmt(lbl, "%s: %d %%", _("Opacidad de la capa"), (int)lv_slider_get_value(s));
    dj_ed_all_changed(a);
}

static void bg_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    static const dib_px_t bgs[3] = { 0xFFFFFFFFu, 0xFF000000u, 0x00000000u };
    int k = 0;
    while (k < 3 && bgs[k] != a->doc->bg) k++;
    dib_px_t next = k >= 3 ? bgs[0] : k == 2 ? (a->color | 0xFF000000u) : bgs[k + 1];
    if (k == 3) next = bgs[0];
    dib_set_bg(a->doc, next);
    dj_ed_all_changed(a);
    a->pnl_rebuild = true;
}

static void build_layers(app_t *a)
{
    int32_t w, h;
    lv_obj_t *c = panel_card(a, _("Capas"), &w, &h);
    dib_doc_t *d = a->doc;
    const int32_t rowh = 92, tw = 72;
    int32_t bh = 72, bw = (w - 48 - 5 * 8) / 6;
    /* the buttons at the bottom, the slider above them, the rows on top */
    int32_t by = h - 16 - bh;
    static const char *const ops[6] = { N_("Nueva"), N_("Duplicar"), N_("Subir"), N_("Bajar"), N_("Unir"), N_("Borrar") };
    for (int k = 0; k < 6; k++) {
        const char *t = k == 5 && a->confirm ? _("¿Seguro?") : _(ops[k]);
        lv_obj_t *b = dj_ui_text_button(c, t, aos_font_caption, bw, bh, k == 5 ? lv_color_hex(0x8A1E22) : C_BTN,
                                     layer_op_cb, a);
        lv_obj_set_pos(b, 24 + k * (bw + 8), by);
        lv_obj_set_user_data(b, (void *)(intptr_t)k);
        bool off = (k == 0 || k == 1) ? d->nlayers >= DIB_MAX_LAYERS : k == 2 ? d->active >= d->nlayers - 1
                 : (k == 3 || k == 4) ? d->active <= 0 : d->nlayers <= 1;
        if (off) lv_obj_set_style_opa(b, LV_OPA_40, 0);
    }
    lv_obj_t *ol = aos_label(c, "", aos_font_small, AOS_C_DIM);
    int op = (d->layers[d->active].opacity * 100 + 127) / 255;
    lv_label_set_text_fmt(ol, "%s: %d %%", _("Opacidad de la capa"), op);
    lv_obj_set_pos(ol, 24, by - 96);
    lv_obj_t *s = lv_slider_create(c);
    lv_obj_set_size(s, w - 80, 20);
    lv_obj_set_pos(s, 40, by - 52);
    lv_slider_set_range(s, 0, 100);
    lv_slider_set_value(s, op, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x48484A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, C_SEL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 10, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 24);
    lv_obj_set_user_data(s, ol);
    lv_obj_add_event_cb(s, layer_opacity_cb, LV_EVENT_VALUE_CHANGED, a);

    lv_obj_t *list = lv_obj_create(c);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 84);
    lv_obj_set_size(list, w, by - 104 - 84);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(list, 24, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_CLICKABLE);
    for (int i = d->nlayers - 1; i >= 0; i--) {
        dib_layer_t *l = &d->layers[i];
        lv_obj_t *row = dj_ui_button(list, 0, 0, w - 48, rowh, i == d->active ? lv_color_hex(0x1E3A5F) : C_BTN,
                                  layer_row_cb, a);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        if (i == d->active) {
            lv_obj_set_style_border_color(row, C_SEL, 0);
            lv_obj_set_style_border_width(row, 3, 0);
        }
        int pw = d->w >= d->h ? tw : tw * d->w / d->h, ph = d->w >= d->h ? tw * d->h / d->w : tw;
        if (!a->lthumb[i]) a->lthumb[i] = dib_alloc(tw * tw * 2);
        if (a->lthumb[i]) {
            dib_thumb(d, i, a->lthumb[i], pw, ph);
            lv_obj_t *cv = lv_canvas_create(row);
            lv_canvas_set_buffer(cv, a->lthumb[i], pw, ph, LV_COLOR_FORMAT_RGB565);
            lv_obj_align(cv, LV_ALIGN_LEFT_MID, 10 + (tw - pw) / 2, 0);
            lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_t *nl = aos_label_boxed(row, l->name, aos_font_body, AOS_C_TEXT, w - 48 - tw - 140, 36);
        lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_remove_flag(nl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(nl, LV_ALIGN_LEFT_MID, tw + 28, -14);
        lv_obj_t *il = aos_label(row, "", aos_font_caption, AOS_C_DIM);
        lv_label_set_text_fmt(il, "%d %%", (l->opacity * 100 + 127) / 255);
        lv_obj_remove_flag(il, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(il, LV_ALIGN_LEFT_MID, tw + 28, 20);
        lv_obj_t *eye = dj_ui_button(row, 0, 0, 84, rowh - 16, l->visible ? C_BTN : lv_color_hex(0x3A3A3C), layer_eye_cb, a);
        lv_obj_align(eye, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_user_data(eye, (void *)(intptr_t)i);
        lv_obj_t *el = aos_label(eye, l->visible ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE, aos_font_body,
                                 l->visible ? AOS_C_TEXT : AOS_C_DIM);
        lv_obj_remove_flag(el, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(el);
    }
    /* the background, under every layer */
    const char *bgname = d->bg == 0xFFFFFFFFu ? _("Blanco") : d->bg == 0xFF000000u ? _("Negro")
                       : !(d->bg >> 24) ? _("Transparente") : _("De color");
    char t[64];
    snprintf(t, sizeof t, "%s: %s", _("Fondo"), bgname);
    lv_obj_t *bgb = dj_ui_text_button(list, t, aos_font_small, w - 48, 64, C_BTN, bg_cb, a);
    (void)bgb;
}

/* --------------------------------------------------------------------------
 * The menu
 * -------------------------------------------------------------------------- */

enum { M_SAVE = 0, M_EXPORT, M_IMPORT, M_GRID, M_GSTEP, M_RULERS, M_SNAP, M_SYM, M_FIT, M_GALLERY, M_COUNT };

static void menu_item_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    int m = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    switch (m) {
    case M_SAVE:
        if (dj_save(a)) dj_toast(_("Guardado"));
        dj_panel_close(a);
        break;
    case M_EXPORT:
        dj_panel_close(a);
        dj_export_png(a);
        break;
    case M_IMPORT:
        a->pnl_open_req = P_IMPORT;
        break;
    case M_GRID:
        a->grid = !a->grid;
        a->view_full = true;
        a->pnl_rebuild = true;
        break;
    case M_GSTEP: {
        static const int steps[] = { 8, 16, 32, 64, 128 };
        int k = 0;
        while (k < 5 && steps[k] != a->grid_step) k++;
        a->grid_step = steps[(k + 1) % 5];
        a->grid = true;
        a->view_full = true;
        a->pnl_rebuild = true;
        break;
    }
    case M_RULERS:
        a->rulers = !a->rulers;
        dj_ed_rulers_changed(a);
        a->pnl_rebuild = true;
        break;
    case M_SNAP:
        a->snap = !a->snap;
        a->view_full = true;
        a->pnl_rebuild = true;
        break;
    case M_SYM:
        a->sym = (a->sym + 1) % 4;
        a->view_full = true;
        a->pnl_rebuild = true;
        break;
    case M_FIT:
        dj_ed_view_fit(a);
        dj_panel_close(a);
        break;
    case M_GALLERY:
        dj_panel_close(a);
        lv_obj_set_user_data(a->btn_back, (void *)1);
        break;
    default:
        break;
    }
}

static void build_menu(app_t *a)
{
    int32_t w, h;
    lv_obj_t *c = panel_card(a, _("Menú"), &w, &h);
    lv_obj_t *col = panel_column(c, w, h);
    int32_t bw = (w - 40 - 12) / 2, bh = 84;
    char t[M_COUNT][64];
    static const char *const sym[4] = { N_("ninguna"), N_("lado a lado"), N_("arriba y abajo"), N_("las dos") };
    snprintf(t[M_SAVE], 64, "%s", _("Guardar"));
    snprintf(t[M_EXPORT], 64, "%s", _("Exportar PNG"));
    snprintf(t[M_IMPORT], 64, "%s", _("Importar una imagen"));
    snprintf(t[M_GRID], 64, "%s: %s", _("Grilla"), a->grid ? _("sí") : _("no"));
    snprintf(t[M_GSTEP], 64, "%s: %d px", _("Paso de la grilla"), a->grid_step);
    snprintf(t[M_RULERS], 64, "%s: %s", _("Reglas y guías"), a->rulers ? _("sí") : _("no"));
    snprintf(t[M_SNAP], 64, "%s: %s", _("Imán"), a->snap ? _("sí") : _("no"));
    snprintf(t[M_SYM], 64, "%s: %s", _("Simetría"), _(sym[a->sym]));
    snprintf(t[M_FIT], 64, "%s", _("Ajustar a la pantalla"));
    snprintf(t[M_GALLERY], 64, "%s", _("Volver a la galería"));
    for (int m = 0; m < M_COUNT; m++) {
        bool on = (m == M_GRID && a->grid) || (m == M_RULERS && a->rulers) || (m == M_SNAP && a->snap) ||
                  (m == M_SYM && a->sym);
        lv_color_t col_c = m == M_EXPORT ? lv_color_hex(0x0A5A9E) : on ? lv_color_hex(0x1E3A5F) : C_BTN;
        lv_obj_t *b = dj_ui_text_button(col, t[m], aos_font_small, bw, bh, col_c, menu_item_cb, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)m);
    }
    char info[96];
    snprintf(info, sizeof info, "%d × %d · %d %s · %u KB", a->doc->w, a->doc->h, a->doc->nlayers,
             a->doc->nlayers == 1 ? _("capa") : _("capas"),
             (unsigned)((a->doc->tile_bytes + a->doc->hist_bytes) >> 10));
    lv_obj_t *l = section(col, w, info);
    (void)l;
}

/* --------------------------------------------------------------------------
 * Pictures to import
 * -------------------------------------------------------------------------- */

static bool is_image(const char *n)
{
    const char *dot = strrchr(n, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png") ||
           !strcasecmp(dot, ".bmp");
}

typedef struct {
    app_t      *a;
    const char *dir;
} imp_ctx_t;

static bool imp_scan(const aos_dir_entry_t *e, void *ctx)
{
    imp_ctx_t *c = ctx;
    app_t *a = c->a;
    if (a->nimp >= 200) return false;
    if (e->dir || e->name[0] == '.' || !is_image(e->name)) return true;
    snprintf(a->imp_files[a->nimp++], 96, "%s/%s", c->dir, e->name);
    return true;
}

static void imp_pick_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    a->imp_req = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    dj_panel_close(a);
}

static void build_import(app_t *a)
{
    int32_t w, h;
    lv_obj_t *c = panel_card(a, _("Importar una imagen"), &w, &h);
    lv_obj_t *col = panel_column(c, w, h);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    if (!a->imp_files) a->imp_files = dib_alloc(200 * 96);
    a->nimp = 0;
    if (a->imp_files) {
        char dir1[128];
        snprintf(dir1, sizeof dir1, "%s/importar", dj_dir());
        imp_ctx_t c1 = { a, dir1 };
        aos_hal_dir_scan(dir1, imp_scan, &c1);
        imp_ctx_t c2 = { a, dj_dir() };
        aos_hal_dir_scan(dj_dir(), imp_scan, &c2);
        imp_ctx_t c3 = { a, aos_hal_path_photos() };
        aos_hal_dir_scan(aos_hal_path_photos(), imp_scan, &c3);
    }
    lv_obj_t *n = section(col, w, _("Va a una capa nueva, abajo de todo; después se mueve y se escala. Las de la carpeta dibujo/importar se suben desde el portal."));
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    if (!a->nimp) {
        section(col, w, _("No hay imágenes (JPG, PNG o BMP) en dibujo/importar ni en fotos."));
        return;
    }
    for (int i = 0; i < a->nimp; i++) {
        const char *name = a->imp_files[i];
        const char *sd = aos_hal_path_sd_root();
        if (sd && !strncmp(name, sd, strlen(sd))) name += strlen(sd) + 1;
        lv_obj_t *b = dj_ui_button(col, 0, 0, w - 40, 72, C_BTN, imp_pick_cb, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_t *l = aos_label_boxed(b, name, aos_font_small, AOS_C_TEXT, w - 72, 32);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
    }
}

/* --------------------------------------------------------------------------
 * The deferred part
 * -------------------------------------------------------------------------- */

static void panel_build(app_t *a, panel_t p)
{
    a->panel = p;
    a->sv_canvas = a->hue_canvas = NULL;
    switch (p) {
    case P_BRUSHES: build_brushes(a); break;
    case P_SHAPES: build_shapes(a); break;
    case P_COLOR: build_color(a); break;
    case P_LAYERS: build_layers(a); break;
    case P_MENU: build_menu(a); break;
    case P_IMPORT: build_import(a); break;
    default: a->panel = P_NONE; break;
    }
    a->overlay_dirty = true;
    if (a->pad_seen && a->pnl) {
        aos_pad_menu_set(&a->pad_menu, NULL, 0, 0);     /* refilled by pad_step */
    }
}

static void panel_delete(app_t *a)
{
    if (a->pnl) lv_obj_delete(a->pnl);
    a->pnl = NULL;
    a->sv_canvas = a->hue_canvas = NULL;
    a->panel = P_NONE;
    aos_pad_menu_set(&a->pad_menu, NULL, 0, 0);
}

void dj_panel_frame(app_t *a)
{
    if (a->pnl_close_req) {
        a->pnl_close_req = false;
        if (a->panel == P_COLOR) dj_palettes_save(a);
        panel_delete(a);
        a->confirm = false;
        a->overlay_dirty = true;
        if (a->imp_req >= 0 && a->imp_req < a->nimp) {
            char path[96];
            snprintf(path, sizeof path, "%s", a->imp_files[a->imp_req]);
            a->imp_req = -1;
            dj_import(a, path);
        }
        a->imp_req = -1;
    }
    if (a->pnl_open_req != P_NONE) {
        panel_t p = a->pnl_open_req;
        a->pnl_open_req = P_NONE;
        panel_delete(a);
        a->confirm = false;
        if (p == P_COLOR) rgb_hsv(a->edit_fill ? a->color2 : a->color, &a->hue, &a->sat, &a->val);
        if (a->doc) panel_build(a, p);
    }
    if (a->pnl_rebuild) {
        a->pnl_rebuild = false;
        if (a->pnl && a->panel != P_NONE) {
            panel_t p = a->panel;
            /* keep the list's scroll: the rows are rebuilt under the finger */
            panel_delete(a);
            panel_build(a, p);
        }
    }
}

/* --------------------------------------------------------------------------
 * Text
 * -------------------------------------------------------------------------- */

bool dj_text_render(app_t *a, const char *txt, int font, dib_px_t color, dib_px_t **bmp, int *w, int *h)
{
    if (font < 0 || font >= NFONTS) font = 3;
    const lv_font_t *f = *dj_text_fonts[font];
    char safe[TEXT_MAX * 2];
    aos_text_safe(safe, sizeof safe, txt);
    if (!safe[0]) return false;
    lv_point_t sz;
    lv_text_get_size(&sz, safe, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int bw = sz.x + 8, bh = sz.y + 8;
    if (bw > 2048) bw = 2048;
    if (bh > 1024) bh = 1024;
    dib_px_t *buf = dib_calloc((size_t)bw * bh * 4);
    if (!buf) return false;
    if (!a->txt_canvas) {
        a->txt_canvas = lv_canvas_create(a->root);
        lv_obj_add_flag(a->txt_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(a->txt_canvas, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_canvas_set_buffer(a->txt_canvas, buf, bw, bh, LV_COLOR_FORMAT_ARGB8888);
    lv_layer_t layer;
    lv_canvas_init_layer(a->txt_canvas, &layer);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = dj_ui_color(color);
    ld.font = f;
    ld.text = safe;
    ld.text_local = 1;
    lv_area_t ar = { 4, 4, bw - 5, bh - 5 };
    lv_draw_label(&layer, &ld, &ar);
    lv_canvas_finish_layer(a->txt_canvas, &layer);
    /* the canvas lets go of the buffer: it is the object's now */
    static uint32_t dummy[4];
    lv_canvas_set_buffer(a->txt_canvas, dummy, 2, 2, LV_COLOR_FORMAT_ARGB8888);
    *bmp = buf;
    *w = bw;
    *h = bh;
    return true;
}

static void txt_font_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    a->font_idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    lv_obj_t *row = lv_obj_get_parent(lv_event_get_current_target_obj(e));
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++) {
        lv_obj_t *b = lv_obj_get_child(row, (int32_t)i);
        lv_obj_set_style_bg_color(b, (int)(intptr_t)lv_obj_get_user_data(b) == a->font_idx ? C_SEL : C_BTN, 0);
    }
}

static bool s_txt_ok;

static void txt_done_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    s_txt_ok = true;
    a->pnl_open_req = P_NONE;
    /* closed by the frame: the keyboard's own event got us here */
    lv_obj_set_user_data(a->txt_card, (void *)1);
}

static void txt_cancel_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    s_txt_ok = false;
    lv_obj_set_user_data(a->txt_card, (void *)1);
}

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) txt_done_cb(e);
    else if (c == LV_EVENT_CANCEL) txt_cancel_cb(e);
}

void dj_text_open(app_t *a, float x, float y, bool edit)
{
    if (a->txt_card) return;
    const layout_t *L = &a->L;
    a->txt_x = x;
    a->txt_y = y;
    a->txt_edit = edit;
    if (edit) a->font_idx = a->obj_font;
    dj_ed_keys_take(a, false);         /* the physical keyboard types here now */
    int32_t kb_h = L->land ? L->H * 45 / 100 : L->H * 30 / 100;
    int32_t ch = L->land ? L->H - kb_h - 2 * GAP : 300;
    a->txt_card = dj_ui_card(a->ed, GAP, GAP, L->W - 2 * GAP, ch);
    lv_obj_set_user_data(a->txt_card, NULL);
    a->txt_ta = lv_textarea_create(a->txt_card);
    lv_obj_set_pos(a->txt_ta, 16, 16);
    lv_obj_set_size(a->txt_ta, L->W - 2 * GAP - 32, ch - 16 - 16 - 84 - 12);
    lv_obj_set_style_text_font(a->txt_ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(a->txt_ta, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(a->txt_ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_color(a->txt_ta, lv_color_hex(0x3A3A3E), 0);
    lv_textarea_set_placeholder_text(a->txt_ta, _("Escribí el texto"));
    lv_textarea_set_max_length(a->txt_ta, TEXT_MAX - 1);
    if (edit) lv_textarea_set_text(a->txt_ta, a->obj_text);
    lv_obj_add_state(a->txt_ta, LV_STATE_FOCUSED);
    /* sizes, then cancel / done */
    lv_obj_t *row = lv_obj_create(a->txt_card);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, 16, ch - 16 - 84);
    const int32_t okw = 128;
    int32_t rw = L->W - 2 * GAP - 32 - 2 * (okw + 8);
    lv_obj_set_size(row, rw, 84);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    int32_t fw = (rw - 6 * (NFONTS - 1)) / NFONTS;
    for (int i = 0; i < NFONTS; i++) {
        char t[8];
        snprintf(t, sizeof t, "%d", dj_text_font_px[i]);
        lv_obj_t *b = dj_ui_text_button(row, t, aos_font_caption, fw, 84, i == a->font_idx ? C_SEL : C_BTN, txt_font_cb, a);
        lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
    }
    lv_obj_t *b = dj_ui_text_button(a->txt_card, _("Cancelar"), aos_font_small, okw, 84, C_BTN, txt_cancel_cb, a);
    lv_obj_set_pos(b, L->W - 2 * GAP - 16 - 2 * okw - 8, ch - 16 - 84);
    b = dj_ui_text_button(a->txt_card, _("Listo"), aos_font_small, okw, 84, C_SEL, txt_done_cb, a);
    lv_obj_set_pos(b, L->W - 2 * GAP - 16 - okw, ch - 16 - 84);

    a->txt_kb = lv_keyboard_create(a->ed);
    lv_obj_set_size(a->txt_kb, L->W, kb_h);
    lv_obj_align(a->txt_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(a->txt_kb, aos_font_body);
    lv_keyboard_set_textarea(a->txt_kb, a->txt_ta);
    lv_obj_add_event_cb(a->txt_kb, kb_cb, LV_EVENT_READY, a);
    lv_obj_add_event_cb(a->txt_kb, kb_cb, LV_EVENT_CANCEL, a);
}

void dj_text_close(app_t *a)
{
    if (!a->txt_card) return;
    lv_obj_delete(a->txt_card);
    if (a->txt_kb) lv_obj_delete(a->txt_kb);
    a->txt_card = a->txt_kb = a->txt_ta = NULL;
    if (a->doc && !lv_obj_has_flag(a->ed, LV_OBJ_FLAG_HIDDEN)) dj_ed_keys_take(a, true);
}

/* Called by the frame when the card asked to close: places the text. */
void dj_text_frame(app_t *a)
{
    if (!a->txt_card || !lv_obj_get_user_data(a->txt_card)) return;
    char txt[TEXT_MAX];
    snprintf(txt, sizeof txt, "%s", lv_textarea_get_text(a->txt_ta));
    bool ok = s_txt_ok;
    dj_text_close(a);
    if (!ok || !txt[0]) return;
    dib_px_t *bmp;
    int w, h;
    if (!dj_text_render(a, txt, a->font_idx, a->color, &bmp, &w, &h)) return;
    if (a->txt_edit && a->has_obj && a->obj_is_text) {
        /* the same box, a new text: keep its scale and turn */
        float k = a->obj.h / (float)a->obj.bh;
        dib_free(a->obj.bmp);
        a->obj.bmp = bmp;
        a->obj.bw = w;
        a->obj.bh = h;
        a->obj.w = w * k;
        a->obj.h = h * k;
        dj_ed_obj_show(a);
    } else {
        dj_ed_obj_from_bitmap(a, bmp, w, h, a->txt_x + w * 0.5f, a->txt_y, false, true);
    }
    snprintf(a->obj_text, sizeof a->obj_text, "%s", txt);
    a->obj_font = a->font_idx;
    a->obj_text_c = a->color;
    a->obj_is_text = true;
    a->opt_dirty = true;
    dj_ed_push_recent(a, a->color);
}

/* --------------------------------------------------------------------------
 * The size of a new drawing (over the gallery)
 * -------------------------------------------------------------------------- */

static void size_pick_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    a->size_req = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
}

static void size_cancel_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    a->sheet_close_req = true;
}

void dj_sizes_open(app_t *a)
{
    if (a->gal_sheet) return;
    const layout_t *L = &a->L;
    lv_obj_t *dim = lv_obj_create(a->gal);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, L->W, L->H);
    lv_obj_set_style_bg_color(dim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_60, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dim, size_cancel_cb, LV_EVENT_CLICKED, a);
    a->gal_sheet = dim;
    int32_t w = L->land ? 1000 : L->W - 2 * 24, h = L->land ? 560 : 820;
    lv_obj_t *c = dj_ui_card(dim, (L->W - w) / 2, (L->H - h) / 2, w, h);
    lv_obj_t *t = aos_label(c, _("Tamaño del dibujo"), aos_font_title, AOS_C_TEXT);
    lv_obj_set_pos(t, 24, 18);
    static const char *const names[NSIZES] = { N_("Pantalla"), N_("Pantalla acostada"), N_("A4"), N_("A4 acostada"),
                                               N_("Cuadrado") };
    int cols = L->land ? 5 : 2;
    int32_t bw = (w - 48 - (cols - 1) * 12) / cols, bh = L->land ? 330 : 220;
    for (int i = 0; i < NSIZES; i++) {
        int32_t x = 24 + (i % cols) * (bw + 12), y = 90 + (i / cols) * (bh + 12);
        lv_obj_t *b = dj_ui_button(c, x, y, bw, bh, C_BTN, size_pick_cb, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        /* a little sheet with the proportions */
        int32_t box = bh - 110, pw = dj_sizes[i][0] * box / 1280, ph = dj_sizes[i][1] * box / 1280;
        lv_obj_t *r = lv_obj_create(b);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, pw, ph);
        lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 16 + (box - ph) / 2);
        lv_obj_set_style_bg_color(r, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, 4, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = aos_label_boxed(b, _(names[i]), aos_font_small, AOS_C_TEXT, bw - 8, 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -44);
        char sz[24];
        snprintf(sz, sizeof sz, "%d × %d", dj_sizes[i][0], dj_sizes[i][1]);
        l = aos_label_boxed(b, sz, aos_font_caption, AOS_C_DIM, bw - 8, 26);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -14);
    }
}

void dj_import_open(app_t *a)
{
    dj_panel_open(a, P_IMPORT);
}

