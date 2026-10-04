/*
 * P4OS - Notes: the pieces every screen uses.
 *
 * The icons are drawn, not glyphs: the firmware's symbol font has no undo,
 * no highlighter, no indent, and a drawn icon follows the theme's colour and
 * any size for free. Each is a few strokes on a 24-unit grid centred on the
 * button, scaled to it.
 */
#include "nt.h"
#include "aos_i18n.h"
#include "aos_ui.h"

#include <string.h>

lv_obj_t *nt_ui_root;               /* set by notas.c: where sheets go */

lv_obj_t *nt_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *nt_text(lv_obj_t *parent, const char *s, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/* -------------------------------------------------------------------------- */
/* Icons                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    lv_layer_t *layer;
    int32_t cx, cy;
    int32_t u;          /* size of the grid, in 1/24ths... times 100 */
    int32_t sw;
    lv_color_t c;
} pen_t;

#define PX(p, v) ((p)->cx + (int32_t)((v) * (p)->u / 100))
#define PY(p, v) ((p)->cy + (int32_t)((v) * (p)->u / 100))
#define PS(p, v) ((int32_t)((v) * (p)->u / 100))

static void L(pen_t *p, float x1, float y1, float x2, float y2)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = p->c;
    d.width = p->sw;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = p->cx + x1 * p->u / 100;
    d.p1.y = p->cy + y1 * p->u / 100;
    d.p2.x = p->cx + x2 * p->u / 100;
    d.p2.y = p->cy + y2 * p->u / 100;
    lv_draw_line(p->layer, &d);
}

static void A(pen_t *p, float x, float y, float r, int a0, int a1)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = p->c;
    d.width = p->sw;
    d.rounded = 1;
    d.center.x = PX(p, x);
    d.center.y = PY(p, y);
    d.radius = (uint16_t)(PS(p, r) + p->sw / 2);
    d.start_angle = a0;
    d.end_angle = a1;
    lv_draw_arc(p->layer, &d);
}

static void R(pen_t *p, float x1, float y1, float x2, float y2, int32_t radius, bool filled, lv_color_t c, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = radius;
    if (filled) {
        d.bg_color = c;
        d.bg_opa = opa;
    } else {
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = c;
        d.border_width = p->sw;
        d.border_opa = opa;
    }
    lv_area_t a = { PX(p, x1), PY(p, y1), PX(p, x2), PY(p, y2) };
    lv_draw_rect(p->layer, &d, &a);
}

static void C(pen_t *p, float x, float y, float r)
{
    R(p, x - r, y - r, x + r, y + r, LV_RADIUS_CIRCLE, true, p->c, LV_OPA_COVER);
}

static void T(pen_t *p, const char *s, const lv_font_t *f)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = p->c;
    d.text = s;
    d.text_local = 1;
    d.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t a = { p->cx - 60, p->cy - f->line_height / 2, p->cx + 60, p->cy + f->line_height / 2 };
    lv_draw_label(p->layer, &d, &a);
}

void nt_draw_icon(lv_layer_t *layer, int icon, const lv_area_t *a, lv_color_t c, int32_t stroke)
{
    int32_t s = LV_MIN(lv_area_get_width(a), lv_area_get_height(a));
    pen_t p = { layer, (a->x1 + a->x2) / 2, (a->y1 + a->y2) / 2, s * 100 / 46, stroke, c };
    if (p.sw <= 0) p.sw = s >= 64 ? 4 : 3;
    switch (icon) {
    case NT_IC_BACK:    L(&p, 4, -9, -4, 0); L(&p, -4, 0, 4, 9); break;
    case NT_IC_PLUS:    L(&p, 0, -9, 0, 9); L(&p, -9, 0, 9, 0); break;
    case NT_IC_MORE:    C(&p, -7, 0, 2); C(&p, 0, 0, 2); C(&p, 7, 0, 2); break;
    case NT_IC_UNDO:    A(&p, 1, 3, 7, 180, 360); L(&p, -6, 4, -10, -1); L(&p, -6, 4, -1, 1); break;
    case NT_IC_REDO:    A(&p, -1, 3, 7, 180, 360); L(&p, 6, 4, 10, -1); L(&p, 6, 4, 1, 1); break;
    case NT_IC_SEARCH:  A(&p, -2, -2, 6.5f, 0, 360); L(&p, 3, 3, 9, 9); break;
    case NT_IC_PIN:
        R(&p, -5, -10, 5, -7, 2, true, c, LV_OPA_COVER);
        R(&p, -3, -8, 3, 1, 1, true, c, LV_OPA_COVER);
        L(&p, -7, 1, 7, 1);
        L(&p, 0, 1, 0, 10);
        break;
    case NT_IC_TRASH:
        L(&p, -9, -7, 9, -7); L(&p, -3, -10, 3, -10);
        L(&p, -7, -7, -6, 10); L(&p, 7, -7, 6, 10); L(&p, -6, 10, 6, 10);
        L(&p, -2, -3, -2, 6); L(&p, 2, -3, 2, 6);
        break;
    case NT_IC_LIST:
        for (int k = -1; k <= 1; k++) { C(&p, -8, k * 7, 1.9f); L(&p, -3, k * 7, 9, k * 7); }
        break;
    case NT_IC_CHECKBOX:
        R(&p, -9, -9, 9, 9, PS(&p, 4), false, c, LV_OPA_COVER);
        L(&p, -5, 0, -1, 4); L(&p, -1, 4, 5, -4);
        break;
    case NT_IC_INDENT:
    case NT_IC_OUTDENT:
        L(&p, -9, -8, 9, -8); L(&p, 0, -3, 9, -3); L(&p, 0, 2, 9, 2); L(&p, -9, 7, 9, 7);
        if (icon == NT_IC_INDENT) { L(&p, -9, -4, -5, -0.5f); L(&p, -5, -0.5f, -9, 3); }
        else { L(&p, -5, -4, -9, -0.5f); L(&p, -9, -0.5f, -5, 3); }
        break;
    case NT_IC_KB_HIDE:
        R(&p, -11, -10, 11, 3, PS(&p, 2), false, c, LV_OPA_COVER);
        for (int k = -1; k <= 1; k++) C(&p, k * 5, -6, 1);
        L(&p, -5, -1, 5, -1);
        L(&p, -4, 7, 0, 10); L(&p, 0, 10, 4, 7);
        break;
    case NT_IC_CLOSE:   L(&p, -7, -7, 7, 7); L(&p, -7, 7, 7, -7); break;
    case NT_IC_HANDLE:  L(&p, -8, -5, 8, -5); L(&p, -8, 0, 8, 0); L(&p, -8, 5, 8, 5); break;
    case NT_IC_CHEVRON: L(&p, -6, -3, 0, 3); L(&p, 0, 3, 6, -3); break;
    case NT_IC_NOTE:
        L(&p, -8, -10, 4, -10); L(&p, 4, -10, 8, -6); L(&p, 8, -6, 8, 10);
        L(&p, 8, 10, -8, 10); L(&p, -8, 10, -8, -10);
        L(&p, -4, -3, 4, -3); L(&p, -4, 1, 4, 1); L(&p, -4, 5, 1, 5);
        break;
    case NT_IC_TASKS:
        for (int k = -1; k <= 1; k++) {
            R(&p, -10, k * 7 - 2.5f, -5, k * 7 + 2.5f, 2, false, c, LV_OPA_COVER);
            L(&p, -1, k * 7, 10, k * 7);
        }
        break;
    case NT_IC_HIGHLIGHT:
        L(&p, -5, 5, 5, -5);
        p.sw = PS(&p, 6);
        L(&p, -2, 2, 6, -6);
        p.sw = stroke > 0 ? stroke : 3;
        L(&p, -9, 9, -5, 5);
        break;
    case NT_IC_TEXTCOLOR: T(&p, "A", nt_font(NT_ST_BOLD, NT_SZ_M)); break;
    case NT_IC_AA:        T(&p, "Aa", nt_font(0, NT_SZ_M)); break;
    case NT_IC_GRID:
        R(&p, -9, -9, -1, -1, 3, false, c, LV_OPA_COVER); R(&p, 1, -9, 9, -1, 3, false, c, LV_OPA_COVER);
        R(&p, -9, 1, -1, 9, 3, false, c, LV_OPA_COVER);   R(&p, 1, 1, 9, 9, 3, false, c, LV_OPA_COVER);
        break;
    case NT_IC_ROWS:
        R(&p, -9, -9, 9, -3, 3, false, c, LV_OPA_COVER); R(&p, -9, 3, 9, 9, 3, false, c, LV_OPA_COVER);
        break;
    case NT_IC_CHECK:   L(&p, -7, 0, -2, 5); L(&p, -2, 5, 8, -6); break;
    case NT_IC_RESTORE: A(&p, 0, 0, 8, 200, 520); L(&p, -8, -4, -9, 2); L(&p, -8, -4, -3, -3); break;
    case NT_IC_QR:
        R(&p, -10, -10, -2, -2, 2, false, c, LV_OPA_COVER); R(&p, 2, -10, 10, -2, 2, false, c, LV_OPA_COVER);
        R(&p, -10, 2, -2, 10, 2, false, c, LV_OPA_COVER);
        C(&p, 5, 5, 1.6f); C(&p, 9, 9, 1.6f); C(&p, 9, 3, 1.6f); C(&p, 3, 9, 1.6f);
        break;
    case NT_IC_TYPE:
        R(&p, -11, -7, 11, 7, PS(&p, 2), false, c, LV_OPA_COVER);
        for (int k = -2; k <= 2; k++) C(&p, k * 4, -2.5f, 1);
        L(&p, -5, 3, 5, 3);
        break;
    case NT_IC_PALETTE:
        A(&p, 0, 0, 9, 0, 360);
        C(&p, -4, -3, 1.8f); C(&p, 2, -5, 1.8f); C(&p, 5, 1, 1.8f);
        break;
    case NT_IC_INFO:    A(&p, 0, 0, 9, 0, 360); L(&p, 0, -1, 0, 5); C(&p, 0, -5, 1.4f); break;
    case NT_IC_COPY:
        R(&p, -8, -5, 4, 9, 3, false, c, LV_OPA_COVER);
        L(&p, -4, -9, 8, -9); L(&p, 8, -9, 8, 5);
        break;
    case NT_IC_SUN:
        C(&p, 0, 0, 4.5f);
        for (int k = 0; k < 8; k++) {
            static const float dx[8] = { 1, 0.71f, 0, -0.71f, -1, -0.71f, 0, 0.71f };
            static const float dy[8] = { 0, 0.71f, 1, 0.71f, 0, -0.71f, -1, -0.71f };
            L(&p, dx[k] * 7.5f, dy[k] * 7.5f, dx[k] * 10, dy[k] * 10);
        }
        break;
    case NT_IC_MOON:
        A(&p, 0, 0, 8, 100, 350);
        A(&p, 4, -3, 6.5f, 110, 330);
        break;
    default: break;
    }
}

void nt_draw_bullet(lv_layer_t *layer, int style, int32_t cx, int32_t cy, int32_t r, lv_color_t c)
{
    lv_area_t a = { cx - r * 3, cy - r * 3, cx + r * 3, cy + r * 3 };
    pen_t p = { layer, cx, cy, r * 100 / 4, LV_MAX(2, r / 2), c };
    switch (style) {
    case NT_BS_CIRCLE:  p.sw = LV_MAX(2, r * 2 / 5); A(&p, 0, 0, 3.2f, 0, 360); break;
    case NT_BS_SQUARE:  R(&p, -3.6f, -3.6f, 3.6f, 3.6f, 1, true, c, LV_OPA_COVER); break;
    case NT_BS_DASH:    p.sw = LV_MAX(2, r * 2 / 3); L(&p, -5, 0, 5, 0); break;
    case NT_BS_ARROW:
        p.sw = LV_MAX(2, r * 2 / 3);
        L(&p, -5, 0, 5, 0); L(&p, 5, 0, 1, -4); L(&p, 5, 0, 1, 4);
        break;
    case NT_BS_STAR: {
        lv_draw_triangle_dsc_t t;
        lv_draw_triangle_dsc_init(&t);
        t.color = c;
        float R0 = 6.5f, R1 = 2.7f;
        lv_point_precise_t pt[10];
        for (int k = 0; k < 10; k++) {
            float rr = (k & 1) ? R1 : R0;
            /* -90 degrees + k * 36, cos/sin from a table */
            static const float co[10] = { 0, 0.588f, 0.951f, 0.951f, 0.588f, 0, -0.588f, -0.951f, -0.951f, -0.588f };
            static const float si[10] = { -1, -0.809f, -0.309f, 0.309f, 0.809f, 1, 0.809f, 0.309f, -0.309f, -0.809f };
            pt[k].x = cx + co[k] * rr * p.u / 100;
            pt[k].y = cy + 0.5f + si[k] * rr * p.u / 100;
        }
        for (int k = 0; k < 10; k += 2) {
            t.p[0].x = cx; t.p[0].y = cy + 0.5f * p.u / 100;
            t.p[1] = pt[(k + 9) % 10];
            t.p[2] = pt[k];
            lv_draw_triangle(layer, &t);
            t.p[1] = pt[k];
            t.p[2] = pt[(k + 1) % 10];
            lv_draw_triangle(layer, &t);
        }
        break;
    }
    case NT_BS_CHECK:   p.sw = LV_MAX(2, r * 2 / 3); L(&p, -4.5f, 0, -1.5f, 3.5f); L(&p, -1.5f, 3.5f, 5, -4); break;
    case NT_BS_DIAMOND: {
        lv_draw_triangle_dsc_t t;
        lv_draw_triangle_dsc_init(&t);
        t.color = c;
        float d = 4.8f * p.u / 100;
        t.p[0].x = cx - d; t.p[0].y = cy; t.p[1].x = cx; t.p[1].y = cy - d; t.p[2].x = cx + d; t.p[2].y = cy;
        lv_draw_triangle(layer, &t);
        t.p[1].y = cy + d;
        lv_draw_triangle(layer, &t);
        break;
    }
    default:            C(&p, 0, 0, 3.4f); break;
    }
    (void)a;
}

void nt_draw_checkbox(lv_layer_t *layer, const lv_area_t *a, bool checked, bool round,
                      lv_color_t c, lv_color_t on)
{
    int32_t s = lv_area_get_width(a);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = round ? LV_RADIUS_CIRCLE : s / 4;
    if (checked) {
        d.bg_color = on;
        d.bg_opa = LV_OPA_COVER;
    } else {
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = c;
        d.border_width = LV_MAX(2, s / 12);
    }
    lv_draw_rect(layer, &d, a);
    if (checked) {
        pen_t p = { layer, (a->x1 + a->x2) / 2, (a->y1 + a->y2) / 2, s * 100 / 24, LV_MAX(2, s / 9),
                    NT.dark ? lv_color_black() : lv_color_white() };
        L(&p, -6, 0, -2, 4.5f);
        L(&p, -2, 4.5f, 6, -4.5f);
    }
}

typedef struct {
    int icon;
    lv_color_t color;
    bool tinted;
} icon_ud_t;

static void icon_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    icon_ud_t *u = lv_obj_get_user_data(o);
    if (!u) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_color_t c = u->tinted ? u->color : NT.text;
    if (lv_obj_has_state(o, LV_STATE_DISABLED)) c = NT.hair;
    nt_draw_icon(lv_event_get_layer(e), u->icon, &a, c, 0);
}

static void icon_delete_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    nt_free(lv_obj_get_user_data(o));
}

lv_obj_t *nt_icon_btn(lv_obj_t *parent, int icon, int32_t size, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, size / 4, 0);
    lv_obj_set_style_bg_color(b, NT.surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, NT.surface2, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    icon_ud_t *u = nt_alloc(sizeof *u);
    if (u) u->icon = icon;
    lv_obj_set_user_data(b, u);
    lv_obj_add_event_cb(b, icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(b, icon_delete_cb, LV_EVENT_DELETE, NULL);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

void nt_icon_set(lv_obj_t *btn, int icon)
{
    icon_ud_t *u = lv_obj_get_user_data(btn);
    if (u && u->icon != icon) { u->icon = icon; lv_obj_invalidate(btn); }
}

void nt_icon_color(lv_obj_t *btn, lv_color_t c)
{
    icon_ud_t *u = lv_obj_get_user_data(btn);
    if (u) { u->color = c; u->tinted = true; lv_obj_invalidate(btn); }
}

lv_obj_t *nt_pill(lv_obj_t *parent, const char *s, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_pad_hor(b, 24, 0);
    lv_obj_set_style_radius(b, 30, 0);
    lv_obj_set_style_bg_color(b, NT.surface, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, NT.accent, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, NT.text, 0);
    lv_obj_set_style_text_color(b, lv_color_black(), LV_STATE_CHECKED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, nt_font(0, NT_SZ_S), 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

/* -------------------------------------------------------------------------- */
/* Sheets                                                                      */
/* -------------------------------------------------------------------------- */

static lv_obj_t *s_overlay;
static lv_obj_t *s_dying;           /* closed, waiting for its deletion */
static nt_pick_cb_t s_pick;
static void *s_pick_ud;

/* A sheet is closed from inside its own rows' events, so it is hidden at
 * once and deleted on the next turn of LVGL's loop. If the screen is
 * rebuilt before that turn (most picks rebuild it), the parent's clean
 * deletes it first: nt_sheet_forget() cancels the pending deletion. */
static void dying_cb(void *o)
{
    if (o == s_dying) s_dying = NULL;
    lv_obj_delete(o);
}

void nt_sheet_close(void)
{
    if (s_overlay) {
        lv_obj_t *o = s_overlay;
        s_overlay = NULL;
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        if (s_dying) {
            lv_async_call_cancel(dying_cb, s_dying);
            lv_obj_delete(s_dying);
        }
        s_dying = o;
        lv_async_call(dying_cb, o);
    }
}

void nt_sheet_forget(void)
{
    if (s_dying) {
        lv_async_call_cancel(dying_cb, s_dying);
        s_dying = NULL;
    }
    s_overlay = NULL;
}

bool nt_sheet_open(void)
{
    return s_overlay != NULL;
}

static void overlay_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) nt_sheet_close();
}

static void overlay_delete_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    if (o == s_overlay) s_overlay = NULL;
    if (o == s_dying) {
        lv_async_call_cancel(dying_cb, o);
        s_dying = NULL;
    }
}

static lv_obj_t *sheet_frame(const char *title, int32_t *cw)
{
    nt_sheet_close();
    lv_obj_t *root = nt_ui_root ? nt_ui_root : lv_layer_top();
    int32_t W = lv_obj_get_width(root), H = lv_obj_get_height(root);
    bool land = W > H;
    s_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, W, H);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_50, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, overlay_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_overlay, overlay_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *card = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(card);
    int32_t w = land ? 680 : W - 32;
    lv_obj_set_width(card, w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, H * 88 / 100, 0);
    lv_obj_set_style_bg_color(card, NT.surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    if (land) lv_obj_center(card);
    else lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -16);
    if (title && title[0]) {
        lv_obj_t *t = nt_text(card, title, nt_font(NT_ST_BOLD, NT_SZ_S), NT.dim);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(t, w - 48);
        lv_obj_set_style_pad_hor(t, 12, 0);
        lv_obj_set_style_pad_top(t, 8, 0);
        lv_obj_set_style_pad_bottom(t, 8, 0);
    }
    if (cw) *cw = w - 32;
    return card;
}

typedef struct {
    int index;
    lv_color_t color;
    int icon;
} row_ud_t;

static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    nt_pick_cb_t cb = s_pick;
    void *ud = s_pick_ud;
    nt_sheet_close();
    if (cb) cb(i, ud);
}

static void row_icon_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    row_ud_t *u = lv_obj_get_user_data(o);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    nt_draw_icon(lv_event_get_layer(e), u->icon, &a, u->color, 3);
}

static void free_ud_cb(lv_event_t *e)
{
    nt_free(lv_obj_get_user_data(lv_event_get_target(e)));
}

lv_obj_t *nt_sheet(const char *title, const nt_item_t *items, int n, nt_pick_cb_t cb, void *ud)
{
    int32_t cw;
    lv_obj_t *card = sheet_frame(title, &cw);
    s_pick = cb;
    s_pick_ud = ud;
    for (int i = 0; i < n; i++) {
        lv_obj_t *r = lv_obj_create(card);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, cw, 88);
        lv_obj_set_style_radius(r, 18, 0);
        lv_obj_set_style_bg_color(r, NT.surface2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_color_t c = items[i].danger ? NT.danger : NT.text;
        int32_t x = 20;
        if (items[i].icon >= 0) {
            lv_obj_t *ic = nt_box(r, 48, 48);
            row_ud_t *u = nt_alloc(sizeof *u);
            u->icon = items[i].icon;
            u->color = c;
            lv_obj_set_user_data(ic, u);
            lv_obj_add_event_cb(ic, row_icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
            lv_obj_add_event_cb(ic, free_ud_cb, LV_EVENT_DELETE, NULL);
            lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);
            x = 84;
        }
        lv_obj_t *l = nt_text(r, items[i].label, nt_font(0, NT_SZ_M), c);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, cw - x - 70);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, x, 0);
        if (items[i].checked) {
            lv_obj_t *ck = nt_box(r, 44, 44);
            row_ud_t *u = nt_alloc(sizeof *u);
            u->icon = NT_IC_CHECK;
            u->color = NT.accent;
            lv_obj_set_user_data(ck, u);
            lv_obj_add_event_cb(ck, row_icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
            lv_obj_add_event_cb(ck, free_ud_cb, LV_EVENT_DELETE, NULL);
            lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -16, 0);
        }
    }
    return card;
}

lv_obj_t *nt_sheet_custom(const char *title, int32_t *content_w)
{
    s_pick = NULL;
    return sheet_frame(title, content_w);
}

/* -------------------------------------------------------------------------- */
/* Confirmation                                                                */
/* -------------------------------------------------------------------------- */

static void (*s_yes)(void *);
static void *s_yes_ud;

static void confirm_cb(lv_event_t *e)
{
    bool yes = (bool)(intptr_t)lv_event_get_user_data(e);
    void (*cb)(void *) = s_yes;
    void *ud = s_yes_ud;
    nt_sheet_close();
    if (yes && cb) cb(ud);
}

static lv_obj_t *dialog_btn(lv_obj_t *row, const char *label, lv_color_t bg, lv_color_t fg, int32_t w, bool yes)
{
    lv_obj_t *b = lv_obj_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 84);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_mix(bg, NT.text, 200), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = nt_text(b, label, nt_font(NT_ST_BOLD, NT_SZ_M), fg);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, confirm_cb, LV_EVENT_CLICKED, (void *)(intptr_t)yes);
    return b;
}

void nt_confirm(const char *title, const char *text, const char *yes, bool danger,
                void (*cb)(void *ud), void *ud)
{
    int32_t cw;
    lv_obj_t *card = sheet_frame(NULL, &cw);
    s_yes = cb;
    s_yes_ud = ud;
    lv_obj_t *t = nt_text(card, title, nt_font(NT_ST_BOLD, NT_SZ_L), NT.text);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, cw - 24);
    lv_obj_set_style_pad_all(t, 12, 0);
    if (text && text[0]) {
        lv_obj_t *s = nt_text(card, text, nt_font(0, NT_SZ_M), NT.dim);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(s, cw - 24);
        lv_obj_set_style_pad_hor(s, 12, 0);
        lv_obj_set_style_pad_bottom(s, 16, 0);
    }
    lv_obj_t *row = nt_box(card, cw, 100);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    int32_t bw = (cw - 16) / 2;
    dialog_btn(row, _("Cancelar"), NT.surface2, NT.text, bw, false);
    dialog_btn(row, yes, danger ? NT.danger : NT.accent, danger ? lv_color_white() : lv_color_black(), bw, true);
}

/* -------------------------------------------------------------------------- */
/* Swatches                                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    lv_color_t c;
    bool none, sel;
} sw_ud_t;

static void swatch_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    sw_ud_t *u = lv_obj_get_user_data(o);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t s = lv_area_get_width(&a);
    lv_area_t in = { a.x1 + s / 8, a.y1 + s / 8, a.x2 - s / 8, a.y2 - s / 8 };
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    if (u->none) {
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = NT.dim;
        d.border_width = 3;
        lv_draw_rect(layer, &d, &in);
        lv_draw_line_dsc_t l;
        lv_draw_line_dsc_init(&l);
        l.color = NT.danger;
        l.width = 3;
        l.p1.x = in.x1 + s / 8; l.p1.y = in.y2 - s / 8;
        l.p2.x = in.x2 - s / 8; l.p2.y = in.y1 + s / 8;
        lv_draw_line(layer, &l);
    } else {
        d.bg_color = u->c;
        d.bg_opa = LV_OPA_COVER;
        d.border_color = NT.hair;
        d.border_width = 1;
        lv_draw_rect(layer, &d, &in);
    }
    if (u->sel) {
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.radius = LV_RADIUS_CIRCLE;
        r.bg_opa = LV_OPA_TRANSP;
        r.border_color = NT.text;
        r.border_width = 3;
        lv_draw_rect(layer, &r, &a);
    }
}

lv_obj_t *nt_swatches(lv_obj_t *parent, int32_t w, int count, lv_color_t (*color)(int),
                      int selected, bool first_is_none, lv_event_cb_t cb, void *ud)
{
    int32_t gap = 8;
    int32_t s = (w - gap * (count - 1)) / count;
    if (s > 68) s = 68;
    lv_obj_t *row = nt_box(parent, w, s);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, gap, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < count; i++) {
        lv_obj_t *o = lv_obj_create(row);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, s, s);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        sw_ud_t *u = nt_alloc(sizeof *u);
        u->c = color(i);
        u->none = first_is_none && i == 0;
        u->sel = i == selected;
        lv_obj_set_user_data(o, u);
        lv_obj_add_event_cb(o, swatch_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_add_event_cb(o, free_ud_cb, LV_EVENT_DELETE, NULL);
        lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    (void)ud;
    return row;
}
