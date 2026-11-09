/*
 * P4OS - Luces: Escenario, the programmer.
 *
 * The fixtures as tiles drawn in the colour they show; a tap selects or
 * leaves one, a long press selects only that one. Below, for the selected
 * ones (all of them when none is): intensity, colour, strobe, position (when
 * some fixture moves), the effects and the master.
 */
#include "lz_ui.h"

#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <string.h>

typedef struct { const char *name; uint8_t r, g, b, w; } swatch_t;

static const swatch_t SWATCH[] = {
    { N_("Rojo"), 255, 0, 0, 0 },      { N_("Naranja"), 255, 96, 0, 0 }, { N_("Ámbar"), 255, 160, 0, 0 },
    { N_("Amarillo"), 255, 230, 0, 0 }, { N_("Verde"), 0, 255, 0, 0 },    { N_("Cian"), 0, 255, 255, 0 },
    { N_("Azul"), 0, 0, 255, 0 },      { N_("Violeta"), 128, 0, 255, 0 }, { N_("Magenta"), 255, 0, 200, 0 },
    { N_("Cálido"), 255, 180, 90, 0 }, { N_("Blanco"), 255, 255, 255, 255 },
};

static const uint8_t STROBE_V[4] = { 0, 60, 140, 230 };
static const char *const STROBE_NAME[4] = { N_("Sin strobe"), N_("Lento"), N_("Medio"), N_("Rápido") };
static const char *const FX_NAME[LZ_FX_COUNT] = { N_("Ninguno"), N_("Arcoíris"), N_("Ola"), N_("Destellos") };

static int first_selected(void)
{
    for (int i = 0; i < LZ.nfix; i++) if (LZ.sel[i]) return i;
    return LZ.nfix ? 0 : -1;
}

static int count_selected(void)
{
    int n = 0;
    for (int i = 0; i < LZ.nfix; i++) n += LZ.sel[i];
    return n;
}

/* ---- the fixtures ---- */

static void tile_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        memset(LZ.sel, 0, sizeof LZ.sel);
        LZ.sel[i] = true;
    } else LZ.sel[i] = !LZ.sel[i];
    lz_stage_refresh();
}

static void sel_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    for (int i = 0; i < LZ.nfix; i++) LZ.sel[i] = k == 0 ? true : k == 1 ? false : k == 2 ? i % 2 == 0 : i % 2 == 1;
    lz_stage_refresh();
}

/* ---- the programmer ---- */

static void dim_cb(lv_event_t *e)
{
    lz_set_dim((uint8_t)lv_slider_get_value(lv_event_get_target(e)));
    lz_stage_refresh();
}

static void dim_chip_cb(lv_event_t *e)
{
    int pct = (int)(intptr_t)lv_event_get_user_data(e);
    lz_set_dim((uint8_t)(pct * 255 / 100));
    lz_stage_refresh();
}

static void swatch_cb(lv_event_t *e)
{
    const swatch_t *s = lv_event_get_user_data(e);
    lz_set_rgb(s->r, s->g, s->b, s->w);
    /* a colour on a fixture that is dark is a colour nobody sees */
    bool any = count_selected() > 0;
    for (int i = 0; i < LZ.nfix; i++)
        if ((LZ.sel[i] || !any) && !LZ.look[i].dim) { lz_set_dim(255); break; }
    lz_stage_refresh();
}

static void strobe_cb(lv_event_t *e)
{
    lz_set_strobe(STROBE_V[(int)(intptr_t)lv_event_get_user_data(e)]);
    lz_stage_refresh();
}

static void pad_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    int32_t x = p.x - a.x1, y = p.y - a.y1;
    x = x < 0 ? 0 : x > w ? w : x;
    y = y < 0 ? 0 : y > h ? h : y;
    lz_set_pos((uint16_t)(x * 65535 / w), (uint16_t)(y * 65535 / h));
    lz_stage_refresh();
}

static void center_cb(lv_event_t *e) { (void)e; lz_set_pos(32768, 32768); lz_stage_refresh(); }

static void fx_cb(lv_event_t *e)
{
    lz_effect((int)(intptr_t)lv_event_get_user_data(e));
    lz_stage_refresh();
}

static void speed_cb(lv_event_t *e)
{
    LZ.fx_speed = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    lz_save();
}

static void master_cb(lv_event_t *e)
{
    LZ.master = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    lz_stage_refresh();
}

static void master_save_cb(lv_event_t *e) { (void)e; lz_save(); }

/* ---- the page ---- */

static lv_obj_t *card_titled(lv_obj_t *col, int32_t w, const char *title, lv_obj_t **value)
{
    lv_obj_t *c = lz_card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_t *top = lz_box(c, w - 36, 40);
    lv_obj_align(aos_label(top, title, aos_font_small, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 0, 0);
    if (value) {
        *value = aos_label(top, "", aos_font_small, AOS_C_DIM);
        lv_obj_align(*value, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return c;
}

void lz_stage_build(lv_obj_t *parent, int32_t ch)
{
    lz_page_t *P = &LZU.pg;
    int32_t w = LZU.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = lz_column(parent, w, ch);

    /* the fixtures */
    lv_obj_t *sr = lz_box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(sr, 10, 0);
    static const char *const SEL[4] = { N_("Todos"), N_("Ninguno"), N_("Pares"), N_("Impares") };
    for (int k = 0; k < 4; k++) lz_chip(sr, aos_tr(SEL[k]), false, sel_cb, (void *)(intptr_t)k);
    P->sel_l = aos_label(sr, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(P->sel_l, 8, 0);

    if (!LZ.nfix) lz_caption(col, _("No hay aparatos: agregalos en Patch."), w);
    int cols = LZU.land ? 8 : 5;
    int32_t tw = (w - (cols - 1) * 10) / cols;
    lv_obj_t *g = lz_box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(g, 10, 0);
    for (int i = 0; i < LZ.nfix; i++) {
        lv_obj_t *t = lz_card(g, tw, 132);
        lv_obj_set_style_border_color(t, LZ_ACCENT, 0);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(t, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_event_cb(t, tile_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(t, tile_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        lv_obj_t *d = lz_box(t, 52, 52);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(d, lv_color_hex(0x48484A), 0);
        lv_obj_set_style_border_width(d, 2, 0);
        lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 12);
        lv_obj_t *n = aos_label(t, LZ.fix[i].name, aos_font_tiny, AOS_C_TEXT);
        lv_obj_set_width(n, tw - 12);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -30);
        char a[16];
        snprintf(a, sizeof a, "%u", LZ.fix[i].addr);
        lv_obj_align(aos_label(t, a, aos_font_tiny, AOS_C_DIM), LV_ALIGN_BOTTOM_MID, 0, -6);
        P->tile[i] = t;
        P->tile_dot[i] = d;
    }

    /* intensity */
    lv_obj_t *c = card_titled(col, w, _("Intensidad"), &P->dim_l);
    P->dim_s = lz_slider(c, w - 60, 0, 255, 0, dim_cb, NULL);
    lv_obj_t *r = lz_chips_row(c, w - 36);
    static const int PCT[] = { 0, 25, 50, 75, 100 };
    for (int k = 0; k < 5; k++) {
        char t[8];
        snprintf(t, sizeof t, "%d %%", PCT[k]);
        lz_chip(r, t, false, dim_chip_cb, (void *)(intptr_t)PCT[k]);
    }

    /* colour */
    bool color = false, strobe = false, pos = false;
    for (int i = 0; i < LZ.nfix; i++) {
        const lz_type_t *t = &LZ.types[LZ.fix[i].type];
        color |= t->has_color;
        strobe |= t->has_strobe;
        pos |= t->has_pos;
    }
    if (color) {
        c = card_titled(col, w, _("Color"), NULL);
        r = lz_box(c, w - 36, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_gap(r, 14, 0);
        int per = LZU.land ? 11 : 6;
        int32_t sw = (w - 36 - (per - 1) * 14) / per;
        for (size_t k = 0; k < sizeof SWATCH / sizeof SWATCH[0]; k++) {
            lv_obj_t *b = lz_box(r, sw, sw + 30);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(b, swatch_cb, LV_EVENT_CLICKED, (void *)&SWATCH[k]);
            lv_obj_t *d = lz_box(b, sw - 8, sw - 8);
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(d, lv_color_make(SWATCH[k].r, SWATCH[k].g, SWATCH[k].b), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_60, LV_STATE_PRESSED);
            lv_obj_add_flag(d, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 0);
            lv_obj_align(aos_label(b, aos_tr(SWATCH[k].name), aos_font_tiny, AOS_C_DIM), LV_ALIGN_BOTTOM_MID, 0, 0);
        }
    }

    /* strobe */
    if (strobe) {
        c = card_titled(col, w, "Strobe", NULL);
        r = lz_chips_row(c, w - 36);
        for (int k = 0; k < 4; k++) P->strobe_chip[k] = lz_chip(r, aos_tr(STROBE_NAME[k]), false, strobe_cb, (void *)(intptr_t)k);
    }

    /* position */
    if (pos) {
        c = card_titled(col, w, _("Posición (pan y tilt)"), NULL);
        int32_t ps = LZU.land ? 300 : w - 36;
        if (ps > 420) ps = 420;
        P->pad = lz_box(c, ps, ps * 3 / 4);
        lv_obj_set_style_bg_color(P->pad, lv_color_hex(0x111114), 0);
        lv_obj_set_style_bg_opa(P->pad, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(P->pad, 16, 0);
        lv_obj_set_style_border_color(P->pad, lv_color_hex(0x3A3A3C), 0);
        lv_obj_set_style_border_width(P->pad, 2, 0);
        lv_obj_add_flag(P->pad, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(P->pad, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_remove_flag(P->pad, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_event_cb(P->pad, pad_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_t *hx = lz_box(P->pad, ps, 2);
        lv_obj_set_style_bg_color(hx, lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_style_bg_opa(hx, LV_OPA_COVER, 0);
        lv_obj_center(hx);
        lv_obj_t *vx = lz_box(P->pad, 2, ps * 3 / 4);
        lv_obj_set_style_bg_color(vx, lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_style_bg_opa(vx, LV_OPA_COVER, 0);
        lv_obj_center(vx);
        P->pad_dot = lz_box(P->pad, 36, 36);
        lv_obj_set_style_radius(P->pad_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(P->pad_dot, LZ_ACCENT, 0);
        lv_obj_set_style_bg_opa(P->pad_dot, LV_OPA_COVER, 0);
        lv_obj_remove_flag(P->pad_dot, LV_OBJ_FLAG_CLICKABLE);
        lz_pill(c, AOS_SYM_AXIS_ARROW, _("Al centro"), AOS_C_CARD2, center_cb, NULL);
    }

    /* effects */
    c = card_titled(col, w, _("Efecto"), NULL);
    r = lz_chips_row(c, w - 36);
    for (int k = 0; k < LZ_FX_COUNT; k++) P->fx_chip[k] = lz_chip(r, aos_tr(FX_NAME[k]), LZ.fx == k, fx_cb, (void *)(intptr_t)k);
    aos_label(c, _("Velocidad"), aos_font_caption, AOS_C_DIM);
    P->speed_s = lz_slider(c, w - 60, 1, 100, LZ.fx_speed, speed_cb, NULL);
    lz_caption(c, _("El efecto corre sobre los aparatos elegidos al tocarlo (todos, si no hay ninguno elegido), encima de la escena."), w - 36);

    /* master */
    c = card_titled(col, w, "Master", &P->master_l);
    P->master_s = lz_slider(c, w - 60, 0, 255, LZ.master, master_cb, NULL);
    lv_obj_add_event_cb(P->master_s, master_save_cb, LV_EVENT_RELEASED, NULL);
    lz_caption(c, _("Baja la intensidad de todo lo que sale, sin tocar las escenas. Blackout, arriba, apaga todo de golpe."), w - 36);

    lz_stage_refresh();
}

void lz_stage_refresh(void)
{
    lz_page_t *P = &LZU.pg;
    if (!P->dim_s) return;
    for (int i = 0; i < LZ.nfix; i++) {
        if (!P->tile[i]) continue;
        uint8_t r, g, b;
        lz_shown_color(i, &r, &g, &b);
        lv_obj_set_style_bg_color(P->tile_dot[i], lv_color_make(r, g, b), 0);
        lv_obj_set_style_border_width(P->tile[i], LZ.sel[i] ? 4 : 0, 0);
    }
    int n = count_selected();
    if (n) lv_label_set_text_fmt(P->sel_l, _("%d elegidos"), n);
    else lv_label_set_text(P->sel_l, _("ninguno elegido: todos"));
    int f = first_selected();
    if (f >= 0) {
        const lz_look_t *l = &LZ.look[f];
        if (!lv_obj_has_state(P->dim_s, LV_STATE_PRESSED)) lv_slider_set_value(P->dim_s, l->dim, LV_ANIM_OFF);
        lv_label_set_text_fmt(P->dim_l, "%d %%", (l->dim * 100 + 127) / 255);
        for (int k = 0; k < 4; k++) {
            int cur = l->strobe == STROBE_V[k] ? 1 : 0;
            lz_chip_set(P->strobe_chip[k], cur);
        }
        if (P->pad_dot) {
            int32_t pw = lv_obj_get_width(P->pad), ph = lv_obj_get_height(P->pad);
            lv_obj_set_pos(P->pad_dot, (int32_t)l->pan * pw / 65535 - 18, (int32_t)l->tilt * ph / 65535 - 18);
        }
    }
    for (int k = 0; k < LZ_FX_COUNT; k++) lz_chip_set(P->fx_chip[k], LZ.fx == k);
    if (!lv_obj_has_state(P->master_s, LV_STATE_PRESSED)) lv_slider_set_value(P->master_s, LZ.master, LV_ANIM_OFF);
    lv_label_set_text_fmt(P->master_l, "%d %%", (LZ.master * 100 + 127) / 255);
}
