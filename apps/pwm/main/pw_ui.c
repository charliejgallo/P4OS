/*
 * P4OS - PWM generator: the screen.
 *
 * Upright, one scrolling column: the channels as chips in a row, the
 * selected channel's pin, mode and switch, what the knob turns, the knob,
 * the signal and the pattern. Lying down, three columns: the channels on
 * the left, the knob in the middle, mode, signal and pattern on the right.
 *
 * The screen is built once per orientation and then only refreshed: chips
 * for all seven channels exist and hide, so no event ever deletes the
 * object it came from.
 */
#include "pw.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *s_mode_map[PW_M_COUNT + 1];
static const char *s_target_map[4];
static const char *s_pat_map[PW_P_COUNT + 3];

/* ---------------------------------------------------------------- pieces */

static lv_obj_t *box(lv_obj_t *p, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *flex(lv_obj_t *p, int32_t w, int32_t h, lv_flex_flow_t flow, int32_t gap)
{
    lv_obj_t *o = box(p, w, h);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(o, gap, 0);
    return o;
}

static lv_obj_t *card(lv_obj_t *p, int32_t w, int32_t h)
{
    lv_obj_t *o = box(p, w, h);
    lv_obj_set_style_bg_color(o, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 24, 0);
    lv_obj_set_style_pad_all(o, 16, 0);
    return o;
}

lv_obj_t *pw_pill(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 72);
    lv_obj_set_style_pad_hor(b, 24, 0);
    lv_obj_set_style_radius(b, 36, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_t *l = aos_label(b, text, aos_font_small, AOS_C_TEXT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

lv_obj_t *pw_bm(lv_obj_t *p, const char **map, int32_t w, int32_t h, const lv_font_t *font,
                         lv_event_cb_t cb)
{
    lv_obj_t *bm = lv_buttonmatrix_create(p);
    lv_buttonmatrix_set_map(bm, map);
    lv_obj_set_size(bm, w, h);
    lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(bm, true);
    lv_obj_set_style_bg_color(bm, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_radius(bm, 20, 0);
    lv_obj_set_style_pad_all(bm, 6, 0);
    lv_obj_set_style_pad_gap(bm, 6, 0);
    lv_obj_set_style_bg_color(bm, AOS_C_CARD2, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(bm, 16, LV_PART_ITEMS);
    lv_obj_set_style_border_width(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(bm, font, LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, AOS_C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm, AOS_C_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_opa(bm, LV_OPA_30, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(bm, AOS_C_CARD2, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_recolor_opa(bm, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_add_event_cb(bm, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return bm;
}

void pw_bm_set(lv_obj_t *bm, int count, int checked, uint32_t disabled_mask)
{
    for (int i = 0; i < count; i++) {
        lv_buttonmatrix_clear_button_ctrl(bm, (uint32_t)i, LV_BUTTONMATRIX_CTRL_DISABLED);
        if (disabled_mask & (1u << i)) lv_buttonmatrix_set_button_ctrl(bm, (uint32_t)i, LV_BUTTONMATRIX_CTRL_DISABLED);
    }
    lv_buttonmatrix_set_button_ctrl(bm, (uint32_t)checked, LV_BUTTONMATRIX_CTRL_CHECKED);
}

static void enable(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_state(o, LV_STATE_DISABLED);
    else lv_obj_add_state(o, LV_STATE_DISABLED);
}

/* ---------------------------------------------------------------- events */

static void fix_target(void)
{
    pw_ch_t *h = pw_cur();
    if (!h) return;
    if (pw_g->target == PW_T_FREQ && !pw_has_freq(h->c.mode)) pw_g->target = PW_T_VALUE;
    if (pw_g->target == PW_T_SPEED && h->c.pat == PW_P_NONE) pw_g->target = PW_T_VALUE;
    if (pw_g->target == PW_T_VALUE && h->c.pat != PW_P_NONE) pw_g->target = PW_T_SPEED;
}

void pw_ui_select(int i)
{
    if (i < 0 || i >= pw_g->n) return;
    pw_g->sel = i;
    fix_target();
    pw_ui_refresh();
    if (pw_g->chip[i]) lv_obj_scroll_to_view(pw_g->chip[i], LV_ANIM_ON);
}

static void chip_cb(lv_event_t *e) { pw_ui_select((int)(intptr_t)lv_event_get_user_data(e)); }

static void add_cb(lv_event_t *e)
{
    (void)e;
    int i = pw_add(PW_M_PWM);
    if (i < 0) aos_ui_toast(_("Ya están los siete canales"), 1500);
    else pw_ui_select(i);
}

static void alloff_cb(lv_event_t *e)
{
    (void)e;
    pw_all_off();
    pw_ui_refresh();
    aos_ui_toast(_("Salidas apagadas y pines sueltos"), 1500);
}

static void mode_cb(lv_event_t *e)
{
    pw_ch_t *h = pw_cur();
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (!h || i >= PW_M_COUNT) return;
    pw_set_mode(h, (int)i);
    fix_target();
    pw_ui_refresh();
}

static void power_cb(lv_event_t *e)
{
    pw_ch_t *h = pw_cur();
    if (!h) return;
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (!pw_set_on(h, on) && on && h->err[0]) aos_ui_toast(h->err, 2500);
    pw_ui_refresh();
}

static void target_cb(lv_event_t *e)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (i > PW_T_SPEED) return;
    pw_ch_t *h = pw_cur();
    if (i == PW_T_VALUE && h && h->c.pat != PW_P_NONE)
        aos_ui_toast(_("Con un patrón andando, el valor lo mueve el patrón"), 1500);
    pw_g->target = (int)i;
    pw_ui_refresh();
}

static void dec_cb(lv_event_t *e)
{
    pw_ch_t *h = pw_cur();
    if (!h || pw_g->target != PW_T_FREQ) return;
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    float f = dir > 0 ? (float)h->c.freq * 10.0f : (float)h->c.freq / 10.0f;
    pw_set_freq(h, pw_nice_freq(f));
    pw_ui_refresh();
}

static void pat_cb(lv_event_t *e)
{
    pw_ch_t *h = pw_cur();
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (!h || i >= PW_P_COUNT) return;
    pw_set_pattern(h, (int)i);
    if (i != PW_P_NONE) pw_g->target = PW_T_SPEED;
    fix_target();
    pw_ui_refresh();
}

static void range_cb(lv_event_t *e)
{
    pw_ch_t *h = pw_cur();
    lv_obj_t *s = lv_event_get_target(e);
    if (!h) return;
    h->c.lo = (float)lv_slider_get_left_value(s) / 1000.0f;
    h->c.hi = (float)lv_slider_get_value(s) / 1000.0f;
    pw_store_changed();
    pw_ui_refresh();
}

static void period_cb(lv_event_t *e)
{
    (void)e;
    pw_ch_t *h = pw_cur();
    if (h && h->c.pat != PW_P_NONE) {
        pw_g->target = PW_T_SPEED;
        pw_ui_refresh();
    }
}

static void steps_cb(lv_event_t *e) { (void)e; pw_sheet_steps(); }
static void pin_cb(lv_event_t *e) { (void)e; pw_sheet_pins(); }
static void gear_cb(lv_event_t *e) { (void)e; pw_sheet_settings(); }
static void presets_cb(lv_event_t *e) { (void)e; pw_sheet_presets(); }
static void wiring_cb(lv_event_t *e) { (void)e; pw_sheet_wiring(); }

/* ---------------------------------------------------------------- building */

static void maps_init(void)
{
    for (int i = 0; i < PW_M_COUNT; i++) s_mode_map[i] = pw_mode_name(i);
    s_mode_map[PW_M_COUNT] = "";
    s_target_map[0] = _("Valor");
    s_target_map[1] = _("Frecuencia");
    s_target_map[2] = _("Velocidad");
    s_target_map[3] = "";
    /* 4 + 3 upright, 3 + 3 + 1 lying down (a narrower column) */
    int k = 0, per = pw_g->land ? 3 : 4;
    for (int i = 0; i < PW_P_COUNT; i++) {
        if (i && i % per == 0) s_pat_map[k++] = "\n";
        s_pat_map[k++] = pw_pat_name(i);
    }
    s_pat_map[k] = "";
}

static lv_obj_t *chip_make(lv_obj_t *strip, int i, int32_t w, int32_t h)
{
    lv_obj_t *c = box(strip, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 20, 0);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_set_style_border_width(c, 3, 0);
    lv_obj_set_style_border_color(c, AOS_C_CARD, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *num = aos_label(c, "", aos_font_body, AOS_C_TEXT);           /* 0 */
    lv_label_set_text_fmt(num, "%d", i + 1);
    lv_obj_align(num, LV_ALIGN_TOP_LEFT, 0, -4);
    lv_obj_t *mode = aos_label(c, "", aos_font_caption, AOS_C_DIM);        /* 1 */
    lv_obj_align(mode, LV_ALIGN_TOP_LEFT, 30, 0);
    lv_obj_t *dot = box(c, 16, 16);                                        /* 2 */
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, 0, 2);
    lv_obj_t *val = aos_label(c, "", aos_font_small, AOS_C_TEXT);          /* 3 */
    lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *pin = aos_label(c, "", aos_font_tiny, AOS_C_DIM);            /* 4 */
    lv_obj_align(pin, LV_ALIGN_BOTTOM_RIGHT, 0, -2);
    return c;
}

static void chip_value(int i)
{
    pw_ch_t *h = &pw_g->ch[i];
    char big[40], sub[64];
    pw_fmt_value(h, h->live, big, sizeof big, sub, sizeof sub);
    if (strcmp(big, pw_g->chip_txt[i])) {
        snprintf(pw_g->chip_txt[i], sizeof pw_g->chip_txt[i], "%s", big);
        lv_label_set_text(lv_obj_get_child(pw_g->chip[i], 3), big);
    }
}

static void chip_fill(int i)
{
    lv_obj_t *c = pw_g->chip[i];
    if (!c) return;
    if (i >= pw_g->n) {
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(c, LV_OBJ_FLAG_HIDDEN);
    pw_ch_t *h = &pw_g->ch[i];
    lv_color_t col = lv_color_hex(pw_mode_color(h->c.mode));
    lv_obj_t *mode = lv_obj_get_child(c, 1);
    lv_label_set_text(mode, h->c.pat != PW_P_NONE ? pw_pat_name(h->c.pat) : pw_mode_name(h->c.mode));
    lv_obj_set_style_text_color(mode, col, 0);
    bool on = h->pwm || h->dac;
    lv_obj_set_style_bg_color(lv_obj_get_child(c, 2), on ? AOS_C_GREEN : h->err[0] ? AOS_C_RED : AOS_C_CARD2, 0);
    pw_g->chip_txt[i][0] = 0;
    chip_value(i);
    lv_obj_t *pin = lv_obj_get_child(c, 4);
    if (h->c.gpio < 0) lv_label_set_text(pin, _("sin pin"));
    else lv_label_set_text_fmt(pin, "GPIO%d", h->c.gpio);
    lv_obj_set_style_border_color(c, i == pw_g->sel ? col : AOS_C_CARD, 0);
}

static void build_header(lv_obj_t *p, int32_t w)
{
    if (!pw_g->land) {
        lv_obj_t *r = flex(p, w, 72, LV_FLEX_FLOW_ROW, 12);
        aos_label(r, _("Canales"), aos_font_body, AOS_C_TEXT);
        pw_g->hdr_info = aos_label(r, "", aos_font_caption, AOS_C_DIM);
        lv_obj_set_flex_grow(pw_g->hdr_info, 1);
        lv_label_set_long_mode(pw_g->hdr_info, LV_LABEL_LONG_MODE_DOTS);
        pw_pill(r, _("Apagar todo"), AOS_C_RED, alloff_cb, NULL);
    } else {
        lv_obj_t *r = flex(p, w, 72, LV_FLEX_FLOW_ROW, 12);
        aos_label(r, _("Canales"), aos_font_body, AOS_C_TEXT);
        lv_obj_t *sp = box(r, 1, 1);
        lv_obj_set_flex_grow(sp, 1);
        lv_obj_t *off = pw_pill(r, _("Apagar todo"), AOS_C_RED, alloff_cb, NULL);
        lv_obj_set_style_pad_hor(off, 18, 0);
        pw_g->hdr_info = aos_label(p, "", aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(pw_g->hdr_info, w);
        lv_label_set_long_mode(pw_g->hdr_info, LV_LABEL_LONG_MODE_WRAP);
    }
}

static void build_strip(lv_obj_t *p, int32_t w, int32_t h)
{
    bool land = pw_g->land;
    lv_obj_t *s = flex(p, w, h, land ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW, 10);
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(s, land ? LV_DIR_VER : LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    pw_g->strip = s;
    int32_t cw = land ? w : 196, ch = land ? 92 : h;
    for (int i = 0; i < PW_CH_MAX; i++) pw_g->chip[i] = chip_make(s, i, cw, ch);
    lv_obj_t *a = box(s, land ? w : 120, land ? 72 : h);
    lv_obj_set_style_radius(a, 20, 0);
    lv_obj_set_style_border_width(a, 3, 0);
    lv_obj_set_style_border_color(a, AOS_C_CARD2, 0);
    lv_obj_set_style_border_opa(a, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(a, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(a, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a, add_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = aos_label(a, land ? _("+ Canal") : "+", land ? aos_font_small : aos_font_large, AOS_C_DIM);
    lv_obj_center(l);
    pw_g->chip_add = a;
}

static void build_bar(lv_obj_t *p, int32_t w, bool with_mode)
{
    lv_obj_t *r = flex(p, w, 76, LV_FLEX_FLOW_ROW, 12);
    lv_obj_t *pb = pw_pill(r, "", AOS_C_CARD2, pin_cb, NULL);
    lv_obj_set_width(pb, 168);
    lv_obj_set_style_pad_hor(pb, 8, 0);
    pw_g->pin_btn = pb;
    pw_g->pin_lbl = lv_obj_get_child(pb, 0);
    lv_obj_set_style_text_font(pw_g->pin_lbl, aos_font_caption, 0);
    if (with_mode) {
        int32_t bw = w - 168 - 76 - 96 - 3 * 12;
        pw_g->mode_bm = pw_bm(r, s_mode_map, bw, 72, aos_font_caption, mode_cb);
    } else {
        lv_obj_t *sp = box(r, 1, 1);
        lv_obj_set_flex_grow(sp, 1);
    }
    lv_obj_t *g = pw_pill(r, LV_SYMBOL_SETTINGS, AOS_C_CARD2, gear_cb, NULL);
    lv_obj_set_width(g, 76);
    lv_obj_set_style_pad_hor(g, 0, 0);
    pw_g->gear = g;
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 96, 54);
    lv_obj_set_style_bg_color(sw, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, AOS_C_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, power_cb, LV_EVENT_VALUE_CHANGED, NULL);
    pw_g->power = sw;
}

static void build_target(lv_obj_t *p, int32_t w)
{
    lv_obj_t *r = flex(p, w, 64, LV_FLEX_FLOW_ROW, 10);
    pw_g->target_bm = pw_bm(r, s_target_map, w - 2 * (72 + 10), 64, aos_font_caption, target_cb);
    lv_obj_t *d = pw_pill(r, "÷10", AOS_C_CARD2, dec_cb, (void *)(intptr_t)-1);
    lv_obj_set_size(d, 72, 64);
    lv_obj_set_style_pad_hor(d, 0, 0);
    lv_obj_t *u = pw_pill(r, "×10", AOS_C_CARD2, dec_cb, (void *)(intptr_t)1);
    lv_obj_set_size(u, 72, 64);
    lv_obj_set_style_pad_hor(u, 0, 0);
    pw_g->dec_dn = d;
    pw_g->dec_up = u;
}

static void build_err(lv_obj_t *p, int32_t w)
{
    pw_g->err_lbl = aos_label(p, "", aos_font_caption, AOS_C_RED);
    lv_obj_set_width(pw_g->err_lbl, w);
    lv_label_set_long_mode(pw_g->err_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(pw_g->err_lbl, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_signal(lv_obj_t *p, int32_t w, int32_t h)
{
    lv_obj_t *s = card(p, w, h);
    lv_obj_add_event_cb(s, pw_draw_signal, LV_EVENT_DRAW_MAIN_END, NULL);
    pw_g->signal = s;
}

static void build_pattern(lv_obj_t *p, int32_t w)
{
    lv_obj_t *c = card(p, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(c, 12, 0);
    int32_t iw = w - 32;

    lv_obj_t *top = box(c, iw, 40);
    lv_obj_t *t = aos_label(top, _("Patrón"), aos_font_small, AOS_C_DIM);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    pw_g->period_lbl = aos_label(top, "", aos_font_small, AOS_C_ACCENT);
    lv_obj_align(pw_g->period_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(pw_g->period_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(pw_g->period_lbl, 16);
    lv_obj_add_event_cb(pw_g->period_lbl, period_cb, LV_EVENT_CLICKED, NULL);

    int rows = pw_g->land ? 3 : 2;
    pw_g->pat_bm = pw_bm(c, s_pat_map, iw, rows * 60, pw_g->land ? aos_font_caption : aos_font_small, pat_cb);

    pw_g->range_lbl = aos_label(c, "", aos_font_caption, AOS_C_TEXT);
    lv_obj_t *s = lv_slider_create(c);
    lv_slider_set_mode(s, LV_SLIDER_MODE_RANGE);
    lv_slider_set_range(s, 0, 1000);
    lv_obj_set_size(s, iw - 40, 14);
    lv_obj_set_style_margin_hor(s, 20, 0);
    lv_obj_set_style_margin_ver(s, 14, 0);
    lv_obj_set_style_bg_color(s, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, AOS_C_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 12, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 20);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(s, range_cb, LV_EVENT_VALUE_CHANGED, NULL);
    pw_g->range = s;

    pw_g->steps_btn = pw_pill(c, _("Editar los pasos"), AOS_C_CARD2, steps_cb, NULL);
    pw_g->pat_note = aos_label(c, "", aos_font_tiny, AOS_C_DIM);
    lv_obj_set_width(pw_g->pat_note, iw);
    lv_label_set_long_mode(pw_g->pat_note, LV_LABEL_LONG_MODE_WRAP);
}

static void build_tools(lv_obj_t *p, int32_t w)
{
    lv_obj_t *r = flex(p, w, 76, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *a = pw_pill(r, _("Presets"), AOS_C_CARD2, presets_cb, NULL);
    lv_obj_t *b = pw_pill(r, _("Cableado"), AOS_C_CARD2, wiring_cb, NULL);
    if (pw_g->land) {
        lv_obj_set_flex_grow(a, 1);
        lv_obj_set_flex_grow(b, 1);
    }
}

void pw_ui_build(void)
{
    pw_t *g = pw_g;
    pw_sheet_forget();
    lv_obj_clean(g->root);
    memset(&g->hdr_info, 0, offsetof(pw_t, sheet) - offsetof(pw_t, hdr_info));
    lv_obj_update_layout(g->root);
    g->W = lv_obj_get_width(g->root);
    g->H = lv_obj_get_height(g->root);
    g->land = g->W > g->H;
    maps_init();
    lv_obj_set_style_bg_color(g->root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(g->root, LV_OPA_COVER, 0);
    const int32_t pad = AOS_UI_PAD;

    if (!g->land) {
        lv_obj_t *page = flex(g->root, g->W, g->H, LV_FLEX_FLOW_COLUMN, 16);
        lv_obj_set_style_pad_hor(page, pad, 0);
        lv_obj_set_style_pad_top(page, 4, 0);
        lv_obj_set_style_pad_bottom(page, 64, 0);
        lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scroll_dir(page, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
        int32_t w = g->W - 2 * pad;
        build_header(page, w);
        build_strip(page, w, 112);
        build_bar(page, w, true);
        build_target(page, w);
        build_err(page, w);
        int32_t ks = w < 540 ? w : 540;
        pw_knob_build(page, ks);
        build_signal(page, w, 230);
        build_pattern(page, w);
        build_tools(page, w);
    } else {
        lv_obj_t *row = flex(g->root, g->W, g->H, LV_FLEX_FLOW_ROW, 20);
        lv_obj_set_style_pad_hor(row, pad, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        int32_t ch = g->H - 16;
        int32_t lw = 320, rw = 360, mw = g->W - 2 * pad - lw - rw - 40;

        lv_obj_t *left = flex(row, lw, ch, LV_FLEX_FLOW_COLUMN, 10);
        build_header(left, lw);
        build_strip(left, lw, 10);
        lv_obj_set_flex_grow(g->strip, 1);
        build_tools(left, lw);

        lv_obj_t *mid = flex(row, mw, ch, LV_FLEX_FLOW_COLUMN, 10);
        build_bar(mid, mw, false);
        build_target(mid, mw);
        build_err(mid, mw);
        int32_t ks = ch - 76 - 64 - 36 - 3 * 10;
        if (ks > mw) ks = mw;
        pw_knob_build(mid, ks);

        lv_obj_t *right = flex(row, rw, ch, LV_FLEX_FLOW_COLUMN, 12);
        lv_obj_add_flag(right, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scroll_dir(right, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(right, LV_SCROLLBAR_MODE_OFF);
        g->mode_bm = pw_bm(right, s_mode_map, rw, 68, aos_font_caption, mode_cb);
        build_signal(right, rw, 190);
        build_pattern(right, rw);
    }
    pw_ui_refresh();
}

/* ---------------------------------------------------------------- refreshing */

static void header_info(void)
{
    uint32_t f[3];
    int n = pw_freqs_in_use(f);
    if (!n) {
        lv_label_set_text(pw_g->hdr_info, _("Ningún PWM encendido"));
        return;
    }
    char txt[96] = "", one[24];
    for (int i = 0; i < n && i < 3; i++) {
        pw_fmt_freq(one, sizeof one, (float)f[i]);
        size_t l = strlen(txt);
        snprintf(txt + l, sizeof txt - l, "%s%s", i ? " · " : "", one);
    }
    size_t l = strlen(txt);
    l += (size_t)snprintf(txt + l, sizeof txt - l, "  ");
    snprintf(txt + l, sizeof txt - l, _("(%d de 3 frecuencias)"), n);
    lv_label_set_text(pw_g->hdr_info, txt);
}

void pw_ui_refresh(void)
{
    pw_t *g = pw_g;
    pw_ch_t *h = pw_cur();
    if (!h || !g->strip) return;
    const pw_cfg_t *c = &h->c;
    fix_target();

    header_info();
    for (int i = 0; i < PW_CH_MAX; i++) chip_fill(i);
    if (g->n >= PW_CH_MAX) lv_obj_add_flag(g->chip_add, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(g->chip_add, LV_OBJ_FLAG_HIDDEN);

    if (c->gpio < 0) {
        lv_label_set_text(g->pin_lbl, _("Elegí\nun pin"));
    } else {
        const aos_io_pin_t *p = aos_io_pin_of_gpio(c->gpio);
        lv_label_set_text_fmt(g->pin_lbl, _("GPIO%d\npata %d"), c->gpio, p ? p->pin : 0);
    }
    pw_bm_set(g->mode_bm, PW_M_COUNT, c->mode, 0);
    if ((h->pwm || h->dac) != lv_obj_has_state(g->power, LV_STATE_CHECKED)) {
        if (h->pwm || h->dac) lv_obj_add_state(g->power, LV_STATE_CHECKED);
        else lv_obj_remove_state(g->power, LV_STATE_CHECKED);
    }

    uint32_t dis = 0;
    if (!pw_has_freq(c->mode)) dis |= 1u << PW_T_FREQ;
    if (c->pat == PW_P_NONE) dis |= 1u << PW_T_SPEED;
    if (c->pat != PW_P_NONE) dis |= 1u << PW_T_VALUE;
    pw_bm_set(g->target_bm, 3, g->target, dis);
    enable(g->dec_dn, g->target == PW_T_FREQ);
    enable(g->dec_up, g->target == PW_T_FREQ);

    lv_label_set_text(g->err_lbl, h->err);
    if (h->err[0]) lv_obj_remove_flag(g->err_lbl, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g->err_lbl, LV_OBJ_FLAG_HIDDEN);

    pw_knob_refresh();
    g->shown_live = h->live;
    lv_obj_invalidate(g->signal);

    /* the pattern */
    pw_bm_set(g->pat_bm, PW_P_COUNT, c->pat, 0);
    bool pat = c->pat != PW_P_NONE, steps = c->pat == PW_P_STEPS;
    char t[40];
    pw_fmt_time(t, sizeof t, c->period);
    lv_label_set_text_fmt(g->period_lbl, _("período %s"), t);
    if (pat) lv_obj_remove_flag(g->period_lbl, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g->period_lbl, LV_OBJ_FLAG_HIDDEN);
    bool ranged = pat && !steps;
    if (ranged) {
        lv_obj_remove_flag(g->range, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g->range_lbl, LV_OBJ_FLAG_HIDDEN);
        if (!lv_slider_is_dragged(g->range)) {
            lv_slider_set_value(g->range, (int32_t)(c->hi * 1000.0f + 0.5f), LV_ANIM_OFF);
            lv_slider_set_start_value(g->range, (int32_t)(c->lo * 1000.0f + 0.5f), LV_ANIM_OFF);
        }
        lv_obj_set_style_bg_color(g->range, lv_color_hex(pw_mode_color(c->mode)), LV_PART_INDICATOR);
        char a[40], b[40], s1[64];
        pw_fmt_value(h, c->lo, a, sizeof a, s1, sizeof s1);
        pw_fmt_value(h, c->hi, b, sizeof b, s1, sizeof s1);
        lv_label_set_text_fmt(g->range_lbl, _("Entre %s y %s"), a, b);
    } else {
        lv_obj_add_flag(g->range, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g->range_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (steps) lv_obj_remove_flag(g->steps_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g->steps_btn, LV_OBJ_FLAG_HIDDEN);
    const char *note;
    if (!pat) note = _("Un patrón mueve el valor solo, y sigue con la app en segundo plano. Al cerrar la app, todo se apaga.");
    else if (pw_pat_smooth(c->pat) && c->mode != PW_M_DAC) note = _("Con fades del LEDC: el hardware va de un punto al siguiente.");
    else note = _("La app escribe el valor cada 20 ms.");
    lv_label_set_text(g->pat_note, note);
}

void pw_ui_live(void)
{
    pw_t *g = pw_g;
    if (!g->strip) return;
    for (int i = 0; i < g->n; i++) chip_value(i);
    pw_ch_t *h = pw_cur();
    if (h && fabsf(h->live - g->shown_live) > 0.0005f) {
        g->shown_live = h->live;
        if (g->target == PW_T_VALUE) pw_knob_refresh();
        lv_obj_invalidate(g->signal);
    }
}
