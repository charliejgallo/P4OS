/*
 * P4OS - DMX: a bench tool for DMX512 (dx.h says what each tab does).
 *
 * The bar on top has the port, the state of the link and the button that
 * opens and closes it. Consola sends; Aparato and Analizador receive; the
 * link follows the tab (Cableado keeps whatever it had).
 */
#include "dx.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

dx_state_t DX;
dx_ui_t DXU;

static const char *const TAB_NAME[DX_TAB_COUNT] = { N_("Consola"), N_("Aparato"), N_("Analizador"), N_("Cableado") };
static const char *const TAB_GLYPH[DX_TAB_COUNT] = { AOS_SYM_TOGGLE_SWITCH, AOS_SYM_LIGHTBULB, AOS_SYM_PULSE, AOS_SYM_CONNECTION };

/* ---- settings ---- */

static void load(void)
{
    int32_t v;
    char s[32];
    if (aos_hal_pref_get_str("dmx_port", s, sizeof DX.port) && s[0]) snprintf(DX.port, sizeof DX.port, "%.15s", s);
    else snprintf(DX.port, sizeof DX.port, "uart.a");
    DX.cfg = (aos_dmx_cfg_t){ .slots = 512, .break_us = 176, .mab_us = 16, .period_ms = 25 };
    if (aos_hal_pref_get_i32("dmx_slots", &v) && v >= 1 && v <= 512) DX.cfg.slots = (uint16_t)v;
    if (aos_hal_pref_get_i32("dmx_brk", &v) && v >= 88 && v <= 2000) DX.cfg.break_us = (uint16_t)v;
    if (aos_hal_pref_get_i32("dmx_mab", &v) && v >= 8 && v <= 1000) DX.cfg.mab_us = (uint16_t)v;
    if (aos_hal_pref_get_i32("dmx_per", &v) && v >= 20 && v <= 1000) DX.cfg.period_ms = (uint16_t)v;
    DX.fx_kind = aos_hal_pref_get_i32("dmx_fxk", &v) && v >= 0 && v < DX_FX_COUNT ? v : DX_FX_PAR7;
    DX.fx_addr = aos_hal_pref_get_i32("dmx_fxa", &v) && v >= 1 && v <= 512 ? v : 1;
    DXU.tab = aos_hal_pref_get_i32("dmx_tab", &v) && v >= 0 && v < DX_TAB_COUNT ? v : DX_TAB_ANALYZER;
}

void dx_save(void)
{
    aos_hal_pref_set_str("dmx_port", DX.port);
    aos_hal_pref_set_i32("dmx_slots", DX.cfg.slots);
    aos_hal_pref_set_i32("dmx_brk", DX.cfg.break_us);
    aos_hal_pref_set_i32("dmx_mab", DX.cfg.mab_us);
    aos_hal_pref_set_i32("dmx_per", DX.cfg.period_ms);
    aos_hal_pref_set_i32("dmx_fxk", DX.fx_kind);
    aos_hal_pref_set_i32("dmx_fxa", DX.fx_addr);
    aos_hal_pref_set_i32("dmx_tab", DXU.tab);
}

/* ---- little widgets ---- */

lv_obj_t *dx_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *dx_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = dx_box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    return c;
}

lv_obj_t *dx_column(lv_obj_t *parent, int32_t w, int32_t h)
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

lv_obj_t *dx_pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = dx_box(parent, LV_SIZE_CONTENT, 72);
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

lv_obj_t *dx_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = dx_box(parent, LV_SIZE_CONTENT, 60);
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

lv_obj_t *dx_chips_row(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *r = dx_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 10, 0);
    return r;
}

lv_obj_t *dx_caption(lv_obj_t *parent, const char *text, int32_t w)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

lv_obj_t *dx_section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 10, 0);
    lv_obj_set_style_pad_top(t, 6, 0);
    return t;
}

/* ---- the keypad (whole numbers) ---- */

void dx_overlay_close(void)
{
    if (DXU.overlay) lv_obj_delete(DXU.overlay);
    DXU.overlay = NULL;
    DXU.kp_value = NULL;
}

static void kp_cb(lv_event_t *e)
{
    const char *k = lv_event_get_user_data(e);
    size_t n = strlen(DXU.kp_text);
    if (!strcmp(k, "ok")) {
        int v = atoi(DXU.kp_text);
        void (*done)(int) = DXU.kp_done;
        dx_overlay_close();
        if (done) done(v);
        return;
    }
    if (!strcmp(k, "x")) { dx_overlay_close(); return; }
    if (DXU.kp_fresh && strcmp(k, "<")) { DXU.kp_text[0] = 0; n = 0; }
    DXU.kp_fresh = false;
    if (!strcmp(k, "<")) { if (n) DXU.kp_text[n - 1] = 0; }
    else if (n < 6) strcat(DXU.kp_text, k);
    lv_label_set_text(DXU.kp_value, DXU.kp_text[0] ? DXU.kp_text : "0");
}

void dx_keypad(const char *title, int value, void (*done)(int v))
{
    dx_overlay_close();
    DXU.kp_done = done;
    DXU.kp_fresh = true;
    snprintf(DXU.kp_text, sizeof DXU.kp_text, "%d", value);
    DXU.overlay = dx_box(DXU.root, DXU.W, DXU.H);
    lv_obj_set_style_bg_color(DXU.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(DXU.overlay, LV_OPA_80, 0);
    lv_obj_add_flag(DXU.overlay, LV_OBJ_FLAG_CLICKABLE);
    const int32_t kw = DXU.land ? 130 : 180, kh = DXU.land ? 70 : 96, gap = 12;
    lv_obj_t *sh = dx_card(DXU.overlay, 3 * kw + 4 * gap, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sh, gap, 0);
    lv_obj_set_style_pad_top(sh, 20, 0);
    lv_obj_set_flex_flow(sh, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(sh, gap, 0);
    lv_obj_center(sh);
    lv_obj_t *t = aos_label(sh, title, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    DXU.kp_value = aos_label(sh, DXU.kp_text, aos_font_huge, DX_AMBER);
    lv_obj_set_width(DXU.kp_value, lv_pct(100));
    lv_obj_set_style_text_align(DXU.kp_value, LV_TEXT_ALIGN_CENTER, 0);
    static const char *const K[] = { "7", "8", "9", "4", "5", "6", "1", "2", "3", "<", "0", "x", "ok", NULL };
    for (int i = 0; K[i]; i++) {
        bool ok = !strcmp(K[i], "ok");
        lv_obj_t *b = dx_box(sh, ok ? 3 * kw + 2 * gap : kw, kh);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_style_bg_color(b, ok ? DX_AMBER : lv_color_hex(0x3A3A3C), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, kp_cb, LV_EVENT_CLICKED, (void *)K[i]);
        const char *txt = !strcmp(K[i], "<") ? LV_SYMBOL_BACKSPACE : !strcmp(K[i], "x") ? _("Cancelar") : ok ? _("Aplicar") : K[i];
        lv_obj_center(aos_label(b, txt, ok || !strcmp(K[i], "x") ? aos_font_body : aos_font_title, AOS_C_TEXT));
    }
}

/* ---- the link ---- */

void dx_send_universe(void)
{
    if (DX.h && DX.dir == AOS_DMX_SEND) aos_io_dmx_write(DX.h, 1, DX.uni + 1, 512);
}

static void link_close(void)
{
    if (DX.h) aos_io_dmx_close(DX.h);
    DX.h = NULL;
}

static void link_tick(void)
{
    aos_dmx_dir_t want = DXU.tab == DX_TAB_CONSOLE ? AOS_DMX_SEND : AOS_DMX_RECEIVE;
    if (DXU.tab == DX_TAB_WIRE && DX.h) want = DX.dir;
    if (DX.h && (!DX.want_open || DX.dir != want)) link_close();
    if (DX.want_open && !DX.h) {
        DX.h = aos_io_dmx_open(DX.port, want, "DMX");
        if (!DX.h) {
            snprintf(DX.err, sizeof DX.err, _("no se pudo abrir %s (¿lo tiene otra app?)"), DX.port);
            DX.want_open = false;
            return;
        }
        DX.err[0] = 0;
        DX.dir = want;
        if (want == AOS_DMX_SEND) { aos_io_dmx_config(DX.h, &DX.cfg); dx_send_universe(); }
        memset(&DX.st, 0, sizeof DX.st);
        DX.rx_slots = 0;
    }
    if (!DX.h) return;
    DX.rx_slots = aos_io_dmx_read(DX.h, DX.rx, sizeof DX.rx, &DX.st);
    if (DX.dir == AOS_DMX_RECEIVE) {
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        for (int i = 1; i <= DX.rx_slots && i <= 512; i++) {
            if (DX.rx[i] != DX.prev[i]) { DX.chg_ms[i] = now ? now : 1; DX.prev[i] = DX.rx[i]; }
        }
    }
}

/* ---- the bar ---- */

static void bar_refresh(void)
{
    if (!DXU.bar_state) return;
    char t[96];
    if (DX.err[0]) {
        lv_label_set_text(DXU.bar_state, DX.err);
        lv_obj_set_style_text_color(DXU.bar_state, AOS_C_ORANGE, 0);
    } else if (!DX.h) {
        lv_label_set_text(DXU.bar_state, _("cerrado"));
        lv_obj_set_style_text_color(DXU.bar_state, AOS_C_DIM, 0);
    } else if (DX.dir == AOS_DMX_SEND) {
        snprintf(t, sizeof t, _("mandando · %u canales · %.0f paquetes/s"), DX.cfg.slots, (double)DX.st.hz);
        lv_label_set_text(DXU.bar_state, t);
        lv_obj_set_style_text_color(DXU.bar_state, AOS_C_GREEN, 0);
    } else {
        bool sig = DX.st.age_ms < 1000;
        if (sig) snprintf(t, sizeof t, _("recibiendo · %u canales · %.0f paquetes/s"), DX.st.slots, (double)DX.st.hz);
        else snprintf(t, sizeof t, "%s", _("escuchando · sin señal"));
        lv_label_set_text(DXU.bar_state, t);
        lv_obj_set_style_text_color(DXU.bar_state, sig ? AOS_C_GREEN : AOS_C_ORANGE, 0);
    }
    lv_obj_set_style_bg_color(DXU.bar_btn, DX.want_open ? AOS_C_RED : DX_TEAL, 0);
    lv_label_set_text(lv_obj_get_child(DXU.bar_btn, 0), DX.want_open ? AOS_SYM_STOP : AOS_SYM_PLAY);
    lv_label_set_text(lv_obj_get_child(DXU.bar_btn, 1), DX.want_open ? _("Cerrar") : _("Abrir"));
}

static void bar_btn_cb(lv_event_t *e)
{
    (void)e;
    DX.want_open = !DX.want_open;
    DX.err[0] = 0;
    link_tick();
    bar_refresh();
}

static void port_pick_cb(lv_event_t *e)
{
    snprintf(DX.port, sizeof DX.port, "%s", (const char *)lv_event_get_user_data(e));
    bool was = DX.want_open;
    link_close();
    DX.want_open = was;
    dx_save();
    dx_overlay_close();
    dx_rebuild();
}

static void bar_port_cb(lv_event_t *e)
{
    (void)e;
    dx_overlay_close();
    DXU.overlay = dx_box(DXU.root, DXU.W, DXU.H);
    lv_obj_set_style_bg_color(DXU.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(DXU.overlay, LV_OPA_80, 0);
    lv_obj_add_flag(DXU.overlay, LV_OBJ_FLAG_CLICKABLE);
    int32_t w = DXU.W - 2 * AOS_UI_PAD;
    lv_obj_t *c = dx_card(DXU.overlay, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 12, 0);
    lv_obj_center(c);
    aos_label(c, _("Puerto"), aos_font_title, AOS_C_TEXT);
    lv_obj_t *r = dx_chips_row(c, w - 40);
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *p = aos_io_port_at(i);
        if (p->kind != AOS_PORT_UART) continue;
        char t[40];
        snprintf(t, sizeof t, p->pins[2] >= 0 ? "%s · DE %d" : "%s", p->name, p->pins[2]);
        dx_chip(r, t, !strcmp(p->name, DX.port), port_pick_cb, (void *)p->name);
    }
    dx_caption(c, _("DMX necesita un puerto UART del header con un transceptor RS485. Un puerto con DE lo deja arriba para mandar y abajo para escuchar."), w - 40);
}

/* ---- pages ---- */

void dx_rebuild(void)
{
    dx_overlay_close();
    lv_obj_clean(DXU.content);
    memset(DXU.fader, 0, sizeof DXU.fader);
    memset(DXU.fader_v, 0, sizeof DXU.fader_v);
    DXU.page_l = DXU.fx_draw = DXU.fx_list = DXU.fx_state = NULL;
    DXU.an_stats = DXU.an_map = DXU.an_nums = DXU.an_page_l = NULL;
    for (int i = 0; i < DX_TAB_COUNT; i++) {
        lv_color_t c = i == DXU.tab ? DX_TEAL : AOS_C_DIM;
        lv_obj_set_style_text_color(lv_obj_get_child(DXU.tabs[i], 0), c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(DXU.tabs[i], 1), c, 0);
    }
    char t[48];
    snprintf(t, sizeof t, "%s", DX.port);
    lv_label_set_text(lv_obj_get_child(DXU.bar_port, 0), t);
    int32_t ch = lv_obj_get_height(DXU.content) - 8;
    switch (DXU.tab) {
    case DX_TAB_CONSOLE: dx_console_build(DXU.content, ch); break;
    case DX_TAB_FIXTURE: dx_fixture_build(DXU.content, ch); break;
    case DX_TAB_ANALYZER: dx_analyzer_build(DXU.content, ch); break;
    default: dx_wire_build(DXU.content, ch); break;
    }
    bar_refresh();
}

static void tab_cb(lv_event_t *e)
{
    DXU.tab = (int)(intptr_t)lv_event_get_user_data(e);
    dx_save();
    link_tick();
    dx_rebuild();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t n;
    n++;
    dx_console_tick();
    link_tick();
    if (n % 2 == 0) {
        bar_refresh();
        if (DXU.tab == DX_TAB_FIXTURE) dx_fixture_refresh();
        else if (DXU.tab == DX_TAB_ANALYZER) dx_analyzer_refresh();
    }
}

static void build_screen(lv_obj_t *root)
{
    lv_obj_clean(root);
    DXU.root = root;
    DXU.overlay = NULL;
    DXU.W = lv_obj_get_width(root);
    DXU.H = lv_obj_get_height(root);
    DXU.land = DXU.W > DXU.H;
    const int32_t tab_h = DXU.land ? 92 : 112, bar_h = 96;
    lv_obj_t *bar = dx_box(root, DXU.W, bar_h);
    lv_obj_set_style_pad_hor(bar, AOS_UI_PAD, 0);
    DXU.bar_port = dx_box(bar, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_radius(DXU.bar_port, 30, 0);
    lv_obj_set_style_pad_hor(DXU.bar_port, 20, 0);
    lv_obj_set_style_bg_color(DXU.bar_port, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(DXU.bar_port, LV_OPA_COVER, 0);
    lv_obj_add_flag(DXU.bar_port, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(DXU.bar_port, bar_port_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(aos_label(DXU.bar_port, "", aos_font_small, AOS_C_TEXT));
    lv_obj_align(DXU.bar_port, LV_ALIGN_TOP_LEFT, 0, 4);
    DXU.bar_state = aos_label(bar, "", aos_font_tiny, AOS_C_DIM);
    lv_obj_set_width(DXU.bar_state, DXU.W - 2 * AOS_UI_PAD - 220);
    lv_label_set_long_mode(DXU.bar_state, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(DXU.bar_state, LV_ALIGN_BOTTOM_LEFT, 8, -2);
    DXU.bar_btn = dx_pill(bar, AOS_SYM_PLAY, _("Abrir"), DX_TEAL, bar_btn_cb, NULL);
    lv_obj_align(DXU.bar_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    DXU.content = dx_box(root, DXU.W, DXU.H - tab_h - bar_h);
    lv_obj_set_pos(DXU.content, 0, bar_h);
    lv_obj_set_style_pad_hor(DXU.content, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(DXU.content, 8, 0);
    lv_obj_update_layout(DXU.content);
    lv_obj_t *tb = dx_box(root, DXU.W, tab_h);
    lv_obj_set_pos(tb, 0, DXU.H - tab_h);
    lv_obj_set_style_bg_color(tb, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < DX_TAB_COUNT; i++) {
        lv_obj_t *t = dx_box(tb, DXU.W / DX_TAB_COUNT, tab_h);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(aos_label(t, TAB_GLYPH[i], &aos_sym_44, AOS_C_DIM), LV_ALIGN_CENTER, 0, DXU.land ? -14 : -16);
        lv_obj_align(aos_label(t, aos_tr(TAB_NAME[i]), aos_font_tiny, AOS_C_DIM), LV_ALIGN_CENTER, 0, DXU.land ? 26 : 30);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        DXU.tabs[i] = t;
    }
    dx_rebuild();
}

/* ---- the app ---- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&DXU, 0, sizeof DXU);
    memset(&DX, 0, sizeof DX);
    load();
    DX.want_open = false;           /* never send on somebody's rig before the user says so */
    build_screen(root);
    DXU.timer = lv_timer_create(timer_cb, 100, NULL);
    return &DXU;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (DXU.timer) lv_timer_delete(DXU.timer);
    DXU.timer = NULL;
    link_close();
    dx_save();
    memset(&DXU, 0, sizeof DXU);
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    build_screen(root);
    return true;
}

static void show(aos_app_t *self, void *inst) { (void)self; (void)inst; if (DXU.timer) lv_timer_resume(DXU.timer); }
static void hide(aos_app_t *self, void *inst) { (void)self; (void)inst; if (DXU.timer) lv_timer_pause(DXU.timer); }

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (DXU.overlay) { dx_overlay_close(); return true; }
    return false;
}

/* A spotlight: a can on its yoke and its beam. */
static const uint8_t DX_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER,   0,  -6, 34, 62,  6,  AIC_C_TEXT, 255),     /* the can */
    AIC_ROT(-300),
    AIC_RECT(AIC_CENTER,  16,  12, 30, 30, AIC_CIRCLE, AIC_C_YELLOW, 255),  /* the lens */
    AIC_RECT(AIC_CENTER,   0,  32, 54,  6,  3,  AIC_C_DIM, 255),      /* the base */
    AIC_RECT(AIC_CENTER, -28,  14,  6, 36,  3,  AIC_C_DIM, 255),      /* the yoke */
    AIC_END
};

static bool init(aos_app_t *app)
{
    app->desc.id       = "aos.dmx";
    app->desc.name     = "DMX";
    app->desc.icon     = AOS_SYM_LIGHTBULB;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x7C3AED;
    app->desc.color_b  = 0x3B0764;
    app->desc.flags    = AOS_APP_FLAG_KEEP | AOS_APP_FLAG_KEEP_AWAKE;
    app->desc.order    = 122;
    aos_icon_set_ops(app, DX_ICON, sizeof DX_ICON);
    app->create  = create;
    app->destroy = destroy;
    app->resize  = resize;
    app->show    = show;
    app->hide    = hide;
    app->back    = back;
    return true;
}

AOS_APP_ENTRY(init);
