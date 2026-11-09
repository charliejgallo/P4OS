/*
 * P4OS - Luces: a DMX lighting controller (lz.h says the model).
 *
 * The bar on top: the port and what goes out, Blackout and the button that
 * opens and closes the output. Four tabs:
 *
 *   Escenario  the fixtures, a tap selects them; intensity, colour,
 *              strobe and position for the selected ones; the master;
 *              effects (rainbow, wave, sparkle)
 *   Escenas    store what is on stage, recall it with a fade; a chase that
 *              goes through every scene at a tempo (tap tempo)
 *   Patch      which fixture at which address; the types, built in and
 *              from the card
 *   Salida     the port, and the 512 channels as they go out
 *
 * The output opens closed: sending light cues on somebody's rig is the
 * user's call. Once open it keeps going with the app in the background.
 */
#include "lz.h"
#include "lz_ui.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_io.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lz_ui_t LZU;

static const char *const TAB_NAME[LZ_TAB_COUNT] = { N_("Escenario"), N_("Escenas"), N_("Patch"), N_("Salida") };
static const char *const TAB_GLYPH[LZ_TAB_COUNT] = { AOS_SYM_LIGHTBULB_GROUP, AOS_SYM_PLAY_PAUSE, AOS_SYM_GRID, AOS_SYM_CONNECTION };

/* ---- settings ---- */

static void load(void)
{
    int32_t v;
    char s[32];
    if (aos_hal_pref_get_str("lz_port", s, sizeof LZU.port) && s[0]) snprintf(LZU.port, sizeof LZU.port, "%.15s", s);
    else snprintf(LZU.port, sizeof LZU.port, "uart.a");
    LZ.master = aos_hal_pref_get_i32("lz_master", &v) && v >= 0 && v <= 255 ? (uint8_t)v : 255;
    LZ.scene_fade_ms = aos_hal_pref_get_i32("lz_fade", &v) && v >= 0 && v <= 30000 ? (uint16_t)v : 1000;
    LZ.bpm = aos_hal_pref_get_i32("lz_bpm", &v) && v >= 20 && v <= 300 ? (uint16_t)v : 120;
    LZ.chase_fade = aos_hal_pref_get_i32("lz_cfade", &v) && v >= 0 && v <= 100 ? (uint8_t)v : 50;
    LZ.fx_speed = aos_hal_pref_get_i32("lz_fxs", &v) && v >= 1 && v <= 100 ? (uint8_t)v : 30;
    LZU.tab = aos_hal_pref_get_i32("lz_tab", &v) && v >= 0 && v < LZ_TAB_COUNT ? v : LZ_TAB_STAGE;
}

void lz_save(void)
{
    aos_hal_pref_set_str("lz_port", LZU.port);
    aos_hal_pref_set_i32("lz_master", LZ.master);
    aos_hal_pref_set_i32("lz_fade", LZ.scene_fade_ms);
    aos_hal_pref_set_i32("lz_bpm", LZ.bpm);
    aos_hal_pref_set_i32("lz_cfade", LZ.chase_fade);
    aos_hal_pref_set_i32("lz_fxs", LZ.fx_speed);
    aos_hal_pref_set_i32("lz_tab", LZU.tab);
}

/* ---- little widgets ---- */

lv_obj_t *lz_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *lz_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = lz_box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    return c;
}

lv_obj_t *lz_column(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_style_pad_bottom(c, 24, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    return c;
}

lv_obj_t *lz_pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lz_box(parent, LV_SIZE_CONTENT, 72);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 36, 0);
    lv_obj_set_style_pad_hor(b, 24, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (glyph) aos_make_decorative(aos_label(b, glyph, &aos_sym_28, lv_color_white()));
    if (text) aos_make_decorative(aos_label(b, text, aos_font_body, lv_color_white()));
    return b;
}

lv_obj_t *lz_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = lz_box(parent, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_radius(c, 30, 0);
    lv_obj_set_style_pad_hor(c, 20, 0);
    lv_obj_set_style_bg_color(c, on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    lv_obj_center(aos_label(c, text, aos_font_small, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT));
    return c;
}

void lz_chip_set(lv_obj_t *c, bool on)
{
    if (!c) return;
    lv_obj_set_style_bg_color(c, on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
    lv_obj_t *l = lv_obj_get_child(c, 0);
    if (l) lv_obj_set_style_text_color(l, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT, 0);
}

lv_obj_t *lz_chips_row(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *r = lz_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 10, 0);
    return r;
}

lv_obj_t *lz_caption(lv_obj_t *parent, const char *text, int32_t w)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

lv_obj_t *lz_section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 10, 0);
    lv_obj_set_style_pad_top(t, 6, 0);
    return t;
}

lv_obj_t *lz_slider(lv_obj_t *parent, int32_t w, int min, int max, int value, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_size(s, w, 22);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, LZ_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s, 12, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 20);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, ud);
    return s;
}

/* ---- overlays: a number, a text ---- */

void lz_overlay_close(void)
{
    if (LZU.overlay) lv_obj_delete(LZU.overlay);
    LZU.overlay = NULL;
    LZU.kp_value = LZU.ta = NULL;
}

static void sheet_close_cb(lv_event_t *e) { (void)e; lz_overlay_close(); }

lv_obj_t *lz_sheet(const char *title)
{
    lz_overlay_close();
    LZU.overlay = lz_box(LZU.root, LZU.W, LZU.H);
    lv_obj_set_style_bg_color(LZU.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(LZU.overlay, LV_OPA_80, 0);
    lv_obj_add_flag(LZU.overlay, LV_OBJ_FLAG_CLICKABLE);
    int32_t w = LZU.W - 2 * AOS_UI_PAD;
    lv_obj_t *c = lz_card(LZU.overlay, w, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(c, LZU.H - 80, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_center(c);
    lv_obj_t *top = lz_box(c, w - 40, 72);
    lv_obj_align(aos_label(top, title, aos_font_title, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(lz_pill(top, AOS_SYM_CLOSE, NULL, AOS_C_CARD2, sheet_close_cb, NULL), LV_ALIGN_RIGHT_MID, 0, 0);
    return c;
}

void lz_set_port(const char *port)
{
    bool was = LZU.want_open;
    if (LZU.h) { aos_io_dmx_close(LZU.h); LZU.h = NULL; }
    snprintf(LZU.port, sizeof LZU.port, "%.15s", port);
    LZU.want_open = was;
    LZU.err[0] = 0;
    lz_save();
}

static void kp_cb(lv_event_t *e)
{
    const char *k = lv_event_get_user_data(e);
    size_t n = strlen(LZU.kp_text);
    if (!strcmp(k, "ok")) {
        int v = atoi(LZU.kp_text);
        void (*done)(int) = LZU.kp_done;
        lz_overlay_close();
        if (done) done(v);
        return;
    }
    if (!strcmp(k, "x")) { lz_overlay_close(); return; }
    if (LZU.kp_fresh && strcmp(k, "<")) { LZU.kp_text[0] = 0; n = 0; }
    LZU.kp_fresh = false;
    if (!strcmp(k, "<")) { if (n) LZU.kp_text[n - 1] = 0; }
    else if (n < 5) strcat(LZU.kp_text, k);
    lv_label_set_text(LZU.kp_value, LZU.kp_text[0] ? LZU.kp_text : "0");
}

void lz_keypad(const char *title, int value, void (*done)(int v))
{
    lz_overlay_close();
    LZU.kp_done = done;
    LZU.kp_fresh = true;
    snprintf(LZU.kp_text, sizeof LZU.kp_text, "%d", value);
    LZU.overlay = lz_box(LZU.root, LZU.W, LZU.H);
    lv_obj_set_style_bg_color(LZU.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(LZU.overlay, LV_OPA_80, 0);
    lv_obj_add_flag(LZU.overlay, LV_OBJ_FLAG_CLICKABLE);
    const int32_t kw = LZU.land ? 130 : 180, kh = LZU.land ? 70 : 96, gap = 12;
    lv_obj_t *sh = lz_card(LZU.overlay, 3 * kw + 4 * gap, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sh, gap, 0);
    lv_obj_set_style_pad_top(sh, 20, 0);
    lv_obj_set_flex_flow(sh, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(sh, gap, 0);
    lv_obj_center(sh);
    lv_obj_t *t = aos_label(sh, title, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    LZU.kp_value = aos_label(sh, LZU.kp_text, aos_font_huge, LZ_ACCENT);
    lv_obj_set_width(LZU.kp_value, lv_pct(100));
    lv_obj_set_style_text_align(LZU.kp_value, LV_TEXT_ALIGN_CENTER, 0);
    static const char *const K[] = { "7", "8", "9", "4", "5", "6", "1", "2", "3", "<", "0", "x", "ok", NULL };
    for (int i = 0; K[i]; i++) {
        bool ok = !strcmp(K[i], "ok");
        lv_obj_t *b = lz_box(sh, ok ? 3 * kw + 2 * gap : kw, kh);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_style_bg_color(b, ok ? LZ_ACCENT : lv_color_hex(0x3A3A3C), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, kp_cb, LV_EVENT_CLICKED, (void *)K[i]);
        const char *txt = !strcmp(K[i], "<") ? LV_SYMBOL_BACKSPACE : !strcmp(K[i], "x") ? _("Cancelar") : ok ? _("Aplicar") : K[i];
        lv_obj_center(aos_label(b, txt, ok || !strcmp(K[i], "x") ? aos_font_body : aos_font_title, AOS_C_TEXT));
    }
}

static void (*s_text_done)(const char *);

static void text_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    char v[64];
    snprintf(v, sizeof v, "%s", lv_textarea_get_text(LZU.ta));
    void (*done)(const char *) = c == LV_EVENT_READY ? s_text_done : NULL;
    lz_overlay_close();
    if (done) done(v);
}

void lz_text(const char *title, const char *value, void (*done)(const char *))
{
    lz_overlay_close();
    s_text_done = done;
    LZU.overlay = lz_box(LZU.root, LZU.W, LZU.H);
    lv_obj_set_style_bg_color(LZU.overlay, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(LZU.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(LZU.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(aos_label(LZU.overlay, title, aos_font_title, AOS_C_TEXT), LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 24);
    LZU.ta = lv_textarea_create(LZU.overlay);
    lv_textarea_set_one_line(LZU.ta, true);
    lv_textarea_set_max_length(LZU.ta, 22);
    lv_textarea_set_text(LZU.ta, value);
    lv_obj_set_size(LZU.ta, LZU.W - 2 * AOS_UI_PAD, 88);
    lv_obj_align(LZU.ta, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, LZU.land ? 80 : 110);
    lv_obj_set_style_text_font(LZU.ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(LZU.ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(LZU.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(LZU.ta, 0, 0);
    lv_obj_set_style_radius(LZU.ta, 20, 0);
    lv_obj_set_style_pad_hor(LZU.ta, 24, 0);
    lv_obj_set_style_pad_ver(LZU.ta, 22, 0);
    lv_obj_t *kb = lv_keyboard_create(LZU.overlay);
    lv_obj_set_size(kb, LZU.W, LZU.land ? LZU.H / 2 : LZU.H * 2 / 5);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(kb, aos_font_body);
    lv_keyboard_set_textarea(kb, LZU.ta);
    lv_obj_add_event_cb(kb, text_cb, LV_EVENT_ALL, NULL);
}

/* ---- the output ---- */

void lz_output_tick(void)
{
    if (LZU.h && !LZU.want_open) { aos_io_dmx_close(LZU.h); LZU.h = NULL; }
    if (LZU.want_open && !LZU.h) {
        LZU.h = aos_io_dmx_open(LZU.port, AOS_DMX_SEND, "Luces");
        if (!LZU.h) {
            snprintf(LZU.err, sizeof LZU.err, _("no se pudo abrir %s (¿lo tiene otra app?)"), LZU.port);
            LZU.want_open = false;
            return;
        }
        LZU.err[0] = 0;
        aos_dmx_cfg_t c = { .slots = 512, .break_us = 176, .mab_us = 16, .period_ms = 25 };
        aos_io_dmx_config(LZU.h, &c);
    }
    if (LZU.h) {
        aos_io_dmx_write(LZU.h, 1, LZ.uni + 1, 512);
        aos_io_dmx_read(LZU.h, NULL, 0, &LZU.st);
    }
}

void lz_bar_refresh(void)
{
    if (!LZU.bar_state) return;
    if (LZU.err[0]) {
        lv_label_set_text(LZU.bar_state, LZU.err);
        lv_obj_set_style_text_color(LZU.bar_state, AOS_C_ORANGE, 0);
    } else if (LZU.h) {
        lv_label_set_text_fmt(LZU.bar_state, _("saliendo por %s · %.0f paquetes/s%s"), LZU.port, (double)LZU.st.hz, LZ.blackout ? " · BLACKOUT" : "");
        lv_obj_set_style_text_color(LZU.bar_state, LZ.blackout ? AOS_C_RED : AOS_C_GREEN, 0);
    } else {
        lv_label_set_text_fmt(LZU.bar_state, _("salida cerrada (%s)"), LZU.port);
        lv_obj_set_style_text_color(LZU.bar_state, AOS_C_DIM, 0);
    }
    lv_obj_set_style_bg_color(LZU.bar_btn, LZU.want_open ? AOS_C_RED : LZ_ACCENT, 0);
    lv_label_set_text(lv_obj_get_child(LZU.bar_btn, 0), LZU.want_open ? AOS_SYM_STOP : AOS_SYM_PLAY);
    lv_obj_set_style_bg_color(LZU.bar_bo, LZ.blackout ? AOS_C_RED : AOS_C_CARD2, 0);
}

static void bar_btn_cb(lv_event_t *e) { (void)e; LZU.want_open = !LZU.want_open; LZU.err[0] = 0; lz_output_tick(); lz_bar_refresh(); }
static void bar_bo_cb(lv_event_t *e) { (void)e; LZ.blackout = !LZ.blackout; lz_bar_refresh(); }

/* ---- pages ---- */

void lz_rebuild(void)
{
    lz_overlay_close();
    lv_obj_clean(LZU.content);
    memset(&LZU.pg, 0, sizeof LZU.pg);
    LZU.seen_changed = LZ.changed;
    for (int i = 0; i < LZ_TAB_COUNT; i++) {
        lv_color_t c = i == LZU.tab ? LZ_ACCENT : AOS_C_DIM;
        lv_obj_set_style_text_color(lv_obj_get_child(LZU.tabs[i], 0), c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(LZU.tabs[i], 1), c, 0);
    }
    int32_t ch = lv_obj_get_height(LZU.content) - 8;
    switch (LZU.tab) {
    case LZ_TAB_STAGE: lz_stage_build(LZU.content, ch); break;
    case LZ_TAB_SCENES: lz_scenes_build(LZU.content, ch); break;
    case LZ_TAB_PATCH: lz_patch_build(LZU.content, ch); break;
    default: lz_out_build(LZU.content, ch); break;
    }
    lz_bar_refresh();
}

static void tab_cb(lv_event_t *e)
{
    LZU.tab = (int)(intptr_t)lv_event_get_user_data(e);
    lz_save();
    lz_rebuild();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t n;
    n++;
    lz_tick();
    lz_output_tick();
    lz_remote_tick();
    if (LZU.seen_changed != LZ.changed && !LZU.overlay) lz_rebuild();     /* the portal changed the show */
    if (n % 4 == 0) {
        lz_bar_refresh();
        lz_stage_refresh();
        lz_scenes_refresh();
        lz_out_refresh();
    }
}

static void build_screen(lv_obj_t *root)
{
    lv_obj_clean(root);
    LZU.root = root;
    LZU.overlay = NULL;
    LZU.W = lv_obj_get_width(root);
    LZU.H = lv_obj_get_height(root);
    LZU.land = LZU.W > LZU.H;
    const int32_t tab_h = LZU.land ? 92 : 112, bar_h = 96;
    lv_obj_t *bar = lz_box(root, LZU.W, bar_h);
    lv_obj_set_style_pad_hor(bar, AOS_UI_PAD, 0);
    lv_obj_align(aos_label(bar, "Luces", aos_font_large, AOS_C_TEXT), LV_ALIGN_TOP_LEFT, 0, 4);
    LZU.bar_state = aos_label(bar, "", aos_font_tiny, AOS_C_DIM);
    lv_obj_set_width(LZU.bar_state, LZU.W - 2 * AOS_UI_PAD - 330);
    lv_label_set_long_mode(LZU.bar_state, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(LZU.bar_state, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    LZU.bar_btn = lz_pill(bar, AOS_SYM_PLAY, NULL, LZ_ACCENT, bar_btn_cb, NULL);
    lv_obj_align(LZU.bar_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    LZU.bar_bo = lz_pill(bar, NULL, "Blackout", AOS_C_CARD2, bar_bo_cb, NULL);
    lv_obj_align(LZU.bar_bo, LV_ALIGN_RIGHT_MID, -110, 0);
    LZU.content = lz_box(root, LZU.W, LZU.H - tab_h - bar_h);
    lv_obj_set_pos(LZU.content, 0, bar_h);
    lv_obj_set_style_pad_hor(LZU.content, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(LZU.content, 8, 0);
    lv_obj_update_layout(LZU.content);
    lv_obj_t *tb = lz_box(root, LZU.W, tab_h);
    lv_obj_set_pos(tb, 0, LZU.H - tab_h);
    lv_obj_set_style_bg_color(tb, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < LZ_TAB_COUNT; i++) {
        lv_obj_t *t = lz_box(tb, LZU.W / LZ_TAB_COUNT, tab_h);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(aos_label(t, TAB_GLYPH[i], &aos_sym_44, AOS_C_DIM), LV_ALIGN_CENTER, 0, LZU.land ? -14 : -16);
        lv_obj_align(aos_label(t, aos_tr(TAB_NAME[i]), aos_font_tiny, AOS_C_DIM), LV_ALIGN_CENTER, 0, LZU.land ? 26 : 30);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        LZU.tabs[i] = t;
    }
    lz_rebuild();
}

/* ---- the app ---- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&LZU, 0, sizeof LZU);
    if (!LZ.ntypes) {                   /* the show stays loaded while the .so does */
        lz_types_load();
        lz_show_load();
    }
    load();
    LZU.want_open = false;
    build_screen(root);
    LZU.timer = lv_timer_create(timer_cb, 40, NULL);
    return &LZU;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (LZU.timer) lv_timer_delete(LZU.timer);
    if (LZU.h) aos_io_dmx_close(LZU.h);
    aos_hal_live_clear("aos.luces");
    lz_save();
    memset(&LZU, 0, sizeof LZU);
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root) { (void)self; (void)inst; build_screen(root); return true; }

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (LZU.overlay) { lz_overlay_close(); return true; }
    return false;
}

/* Three beams from three cans, in three colours. */
static const uint8_t LZ_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -24,  10, 16, 46,  4, AIC_C_LIT(0xFF375F), 160),
    AIC_RECT(AIC_CENTER,   0,  10, 16, 46,  4, AIC_C_YELLOW, 160),
    AIC_RECT(AIC_CENTER,  24,  10, 16, 46,  4, AIC_C_TEAL, 160),
    AIC_RECT(AIC_CENTER, -24, -22, 18, 14,  3, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER,   0, -22, 18, 14,  3, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER,  24, -22, 18, 14,  3, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER,   0, -31, 70,  5,  2, AIC_C_DIM, 255),
    AIC_END
};

static bool init(aos_app_t *app)
{
    app->desc.id       = "aos.luces";
    app->desc.name     = "Luces";
    app->desc.icon     = AOS_SYM_LIGHTBULB_GROUP;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0xEC4899;
    app->desc.color_b  = 0x6D28D9;
    app->desc.flags    = AOS_APP_FLAG_KEEP | AOS_APP_FLAG_KEEP_AWAKE;
    app->desc.order    = 123;
    aos_icon_set_ops(app, LZ_ICON, sizeof LZ_ICON);
    app->create  = create;
    app->destroy = destroy;
    app->resize  = resize;
    app->back    = back;
    return true;
}

AOS_APP_ENTRY(init);
