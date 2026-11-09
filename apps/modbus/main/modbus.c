/*
 * P4OS - Modbus: a bench tool for Modbus RTU on RS485 (mb.h says what it
 * does and how the pieces meet).
 *
 * The screen: a bar on top with the link (tap it to change port, speed,
 * format), its state and the button that opens and closes it; five tabs
 * under it:
 *
 *   Maestro    a request of any function, once or live; the values in the
 *              format wanted, a tap writes one; find the units, find the
 *              speed
 *   Esclavo    the app answers as unit N: its tables, values that move,
 *              faults on demand
 *   Monitor    listens without driving the line: a row per unit with its
 *              questions, answers, silences, exceptions, CRC errors, delays
 *   Tráfico    every frame, in hex and in words, with its times
 *   Cableado   the transceiver between the header and the bus, drawn with
 *              the pins of the chosen port
 *
 * The role is the last of the first three tabs shown. The link opens closed
 * every time: a slave or a master must not start talking on somebody's bus
 * before the user says so.
 */
#include "mb_ui.h"

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

mb_ui_t MBU;

static const char *const TAB_NAME[TAB_COUNT] = { N_("Maestro"), N_("Esclavo"), N_("Monitor"), N_("Tráfico"), N_("Cableado") };
static const char *const TAB_GLYPH[TAB_COUNT] = { AOS_SYM_SEND, AOS_SYM_CHIP, AOS_SYM_EYE_OUTLINE, AOS_SYM_SCRIPT_TEXT, AOS_SYM_CONNECTION };
static const char *const ROLE_NAME[MB_ROLE_COUNT] = { N_("maestro"), N_("esclavo"), N_("monitor") };

/* ---- settings ---- */

static void settings_load(void)
{
    int32_t v;
    char s[64];
    mb_cfg_t *c = &MB.cfg;
    memset(c, 0, sizeof *c);
    c->tr = aos_hal_pref_get_i32("mbt_tr", &v) && v == MB_TR_TCP ? MB_TR_TCP : MB_TR_RTU;
    if (aos_hal_pref_get_str("mbt_port", s, sizeof c->port) && s[0]) snprintf(c->port, sizeof c->port, "%.15s", s);
    else snprintf(c->port, sizeof c->port, "uart.a");
    c->baud = aos_hal_pref_get_i32("mbt_baud", &v) && v >= 300 && v <= 1000000 ? (uint32_t)v : 9600;
    c->par = aos_hal_pref_get_i32("mbt_par", &v) && (v == 'E' || v == 'O') ? (char)v : 'N';
    c->stop = aos_hal_pref_get_i32("mbt_stop", &v) && v == 2 ? 2 : 1;
    if (aos_hal_pref_get_str("mbt_host", s, sizeof s)) snprintf(c->host, sizeof c->host, "%s", s);
    c->tport = aos_hal_pref_get_i32("mbt_tport", &v) && v > 0 && v < 65536 ? (uint16_t)v : 502;
    c->timeout_ms = aos_hal_pref_get_i32("mbt_tmo", &v) && v >= 20 && v <= 10000 ? (uint16_t)v : 300;
    c->gap_ms = aos_hal_pref_get_i32("mbt_gap", &v) && v >= 0 && v <= 1000 ? (uint16_t)v : 5;
    mb_mreq_t *q = &MB.mreq;
    q->unit = aos_hal_pref_get_i32("mbt_unit", &v) && v >= 0 && v <= 247 ? (uint8_t)v : 1;
    q->fc = aos_hal_pref_get_i32("mbt_fc", &v) && v > 0 && v < 128 ? (uint8_t)v : 3;
    q->addr = aos_hal_pref_get_i32("mbt_addr", &v) ? (uint16_t)v : 0;
    q->n = aos_hal_pref_get_i32("mbt_n", &v) && v > 0 ? (uint16_t)v : 10;
    q->wv[0] = aos_hal_pref_get_i32("mbt_wv", &v) ? (uint16_t)v : 0;
    q->wn = 1;
    MB.m_period_ms = aos_hal_pref_get_i32("mbt_per", &v) && v >= 50 ? (uint16_t)v : 500;
    MB.s_unit = aos_hal_pref_get_i32("mbt_sunit", &v) && v >= 1 && v <= 247 ? (uint8_t)v : 1;
    MB.s_delay_ms = aos_hal_pref_get_i32("mbt_sdel", &v) && v >= 0 && v <= 5000 ? (uint16_t)v : 500;
    MB.s_anim = !aos_hal_pref_get_i32("mbt_sanim", &v) || v;
    MBU.fmt = aos_hal_pref_get_i32("mbt_fmt", &v) && v >= 0 && v < FMT_COUNT ? v : FMT_U16;
    MBU.swap = aos_hal_pref_get_i32("mbt_swap", &v) && v;
    MBU.tab = aos_hal_pref_get_i32("mbt_tab", &v) && v >= 0 && v < TAB_COUNT ? v : TAB_MASTER;
    MB.role = aos_hal_pref_get_i32("mbt_role", &v) && v >= 0 && v < MB_ROLE_COUNT ? (mb_role_t)v : MB_ROLE_MASTER;
}

void mb_settings_save(void)
{
    mb_lock();
    mb_cfg_t c = MB.cfg;
    mb_mreq_t q = MB.mreq;
    int per = MB.m_period_ms, su = MB.s_unit, sd = MB.s_delay_ms, sa = MB.s_anim, role = MB.role;
    mb_unlock();
    aos_hal_pref_set_i32("mbt_tr", c.tr);
    aos_hal_pref_set_str("mbt_port", c.port);
    aos_hal_pref_set_i32("mbt_baud", (int32_t)c.baud);
    aos_hal_pref_set_i32("mbt_par", c.par);
    aos_hal_pref_set_i32("mbt_stop", c.stop);
    aos_hal_pref_set_str("mbt_host", c.host);
    aos_hal_pref_set_i32("mbt_tport", c.tport);
    aos_hal_pref_set_i32("mbt_tmo", c.timeout_ms);
    aos_hal_pref_set_i32("mbt_gap", c.gap_ms);
    aos_hal_pref_set_i32("mbt_unit", q.unit);
    aos_hal_pref_set_i32("mbt_fc", q.fc);
    aos_hal_pref_set_i32("mbt_addr", q.addr);
    aos_hal_pref_set_i32("mbt_n", q.n);
    aos_hal_pref_set_i32("mbt_wv", q.wv[0]);
    aos_hal_pref_set_i32("mbt_per", per);
    aos_hal_pref_set_i32("mbt_sunit", su);
    aos_hal_pref_set_i32("mbt_sdel", sd);
    aos_hal_pref_set_i32("mbt_sanim", sa);
    aos_hal_pref_set_i32("mbt_fmt", MBU.fmt);
    aos_hal_pref_set_i32("mbt_swap", MBU.swap);
    aos_hal_pref_set_i32("mbt_tab", MBU.tab);
    aos_hal_pref_set_i32("mbt_role", role);
}

/* ---- little widgets ---- */

lv_obj_t *mb_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *mb_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = mb_box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    return c;
}

lv_obj_t *mb_column(lv_obj_t *parent, int32_t w, int32_t h)
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

lv_obj_t *mb_pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = mb_box(parent, LV_SIZE_CONTENT, 72);
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

lv_obj_t *mb_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = mb_box(parent, LV_SIZE_CONTENT, 60);
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

lv_obj_t *mb_chips_row(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *r = mb_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 10, 0);
    return r;
}

lv_obj_t *mb_caption(lv_obj_t *parent, const char *text, int32_t w)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

lv_obj_t *mb_section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 10, 0);
    lv_obj_set_style_pad_top(t, 6, 0);
    return t;
}

lv_obj_t *mb_switch(lv_obj_t *parent, bool on, lv_event_cb_t cb)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 96, 52);
    lv_obj_set_style_bg_color(sw, C_TEAL, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_state(sw, LV_STATE_CHECKED, on);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

void mb_columns(lv_obj_t *parent, int32_t ch, int32_t left_w, lv_obj_t **l, int32_t *lw, lv_obj_t **r, int32_t *rw)
{
    int32_t w = MBU.W - 2 * AOS_UI_PAD;
    if (MBU.land) {
        *lw = left_w;
        *rw = w - left_w - AOS_UI_PAD;
        *l = mb_column(parent, *lw, ch);
        *r = mb_column(parent, *rw, ch);
        lv_obj_set_x(*r, *lw + AOS_UI_PAD);
    } else {
        *lw = *rw = w;
        *l = *r = mb_column(parent, w, ch);
    }
}

/* ---- the numeric keypad (hex too, for registers) ---- */

void mb_overlay_close(void)
{
    if (MBU.overlay) lv_obj_delete(MBU.overlay);
    MBU.overlay = NULL;
    MBU.kp_value = MBU.ta = MBU.kb = NULL;
}

static void kp_show(void) { if (MBU.kp_value) lv_label_set_text(MBU.kp_value, MBU.kp_text[0] ? MBU.kp_text : "0"); }

static void kp_key_cb(lv_event_t *e)
{
    const char *k = lv_event_get_user_data(e);
    size_t n = strlen(MBU.kp_text);
    if (!strcmp(k, "ok")) {
        char t[16];
        snprintf(t, sizeof t, "%s", MBU.kp_text);
        for (char *p = t; *p; p++) if (*p == ',') *p = '.';
        double v = strncmp(t, "0x", 2) ? strtod(t, NULL) : (double)strtoul(t + 2, NULL, 16);
        void (*done)(double) = MBU.kp_done;
        mb_overlay_close();
        if (done) done(v);
        return;
    }
    if (!strcmp(k, "x")) { mb_overlay_close(); return; }
    if (MBU.kp_fresh && strcmp(k, "<")) { MBU.kp_text[0] = 0; n = 0; }
    MBU.kp_fresh = false;
    bool hex = !strncmp(MBU.kp_text, "0x", 2);
    if (!strcmp(k, "<")) { if (n) MBU.kp_text[n - 1] = 0; }
    else if (!strcmp(k, "-")) {
        if (!hex) {
            if (MBU.kp_text[0] == '-') memmove(MBU.kp_text, MBU.kp_text + 1, n);
            else if (n < 10) { memmove(MBU.kp_text + 1, MBU.kp_text, n + 1); MBU.kp_text[0] = '-'; }
        }
    } else if (!strcmp(k, "0x")) { snprintf(MBU.kp_text, sizeof MBU.kp_text, "0x"); }
    else if (!strcmp(k, ",")) { if (MBU.kp_dec && !hex && !strchr(MBU.kp_text, ',') && n < 10) strcat(MBU.kp_text, n ? "," : "0,"); }
    else if (n < 10) {
        bool digit = k[0] >= '0' && k[0] <= '9';
        if (!digit && !hex) return;
        const char *c = strchr(MBU.kp_text, ',');
        if (!c || (int)strlen(c + 1) < MBU.kp_dec) strcat(MBU.kp_text, k);
    }
    kp_show();
}

void mb_keypad(const char *title, double value, int dec, const char *unit, void (*done)(double))
{
    mb_overlay_close();
    MBU.kp_done = done;
    MBU.kp_dec = dec;
    MBU.kp_fresh = true;
    snprintf(MBU.kp_text, sizeof MBU.kp_text, "%.*f", dec, value);
    for (char *p = MBU.kp_text; *p; p++) if (*p == '.') *p = ',';
    MBU.overlay = mb_box(MBU.root, MBU.W, MBU.H);
    lv_obj_set_style_bg_color(MBU.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(MBU.overlay, LV_OPA_80, 0);
    lv_obj_add_flag(MBU.overlay, LV_OBJ_FLAG_CLICKABLE);
    bool hexkeys = MBU.kp_hex;
    const int cols = hexkeys ? 4 : 3;
    const int32_t kw = MBU.land ? 130 : (hexkeys ? 150 : 180), kh = MBU.land ? 70 : 96, gap = 12;
    lv_obj_t *sh = mb_card(MBU.overlay, cols * kw + (cols + 1) * gap, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sh, gap, 0);
    lv_obj_set_style_pad_top(sh, 20, 0);
    lv_obj_set_flex_flow(sh, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(sh, gap, 0);
    lv_obj_center(sh);
    lv_obj_t *t = aos_label(sh, title, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *vr = mb_box(sh, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(vr, 10, 0);
    MBU.kp_value = aos_label(vr, "", aos_font_huge, C_AMBER);
    if (unit && unit[0]) aos_label(vr, unit, aos_font_title, AOS_C_DIM);
    kp_show();
    static const char *const K10[] = { "7", "8", "9", "4", "5", "6", "1", "2", "3", "-", "0", ",", "<", "x", "ok", NULL };
    static const char *const K16[] = { "7", "8", "9", "A", "4", "5", "6", "B", "1", "2", "3", "C", "0x", "0", "D", "E",
                                       "-", "<", "F", "x", "ok", NULL };
    const char *const *K = hexkeys ? K16 : K10;
    for (int i = 0; K[i]; i++) {
        bool ok = !strcmp(K[i], "ok");
        int span = ok && hexkeys ? 2 : 1;
        lv_obj_t *b = mb_box(sh, span * kw + (span - 1) * gap, kh);
        lv_obj_set_style_radius(b, 18, 0);
        bool dim = !strcmp(K[i], ",") && !MBU.kp_dec;
        lv_obj_set_style_bg_color(b, ok ? C_AMBER : (K[i][0] >= '0' && K[i][0] <= '9' && K[i][1] == 0) ? lv_color_hex(0x3A3A3C) : AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(b, dim ? LV_OPA_30 : LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, kp_key_cb, LV_EVENT_CLICKED, (void *)K[i]);
        const char *txt = !strcmp(K[i], "<") ? LV_SYMBOL_BACKSPACE : !strcmp(K[i], "x") ? _("Cancelar") : ok ? _("Aplicar") : K[i];
        bool word = !strcmp(K[i], "x") || ok || !strcmp(K[i], "<");
        lv_obj_center(aos_label(b, txt, word ? aos_font_body : aos_font_title, AOS_C_TEXT));
    }
    MBU.kp_hex = false;
}

/* ---- a text field ---- */

static void (*s_text_done)(const char *);

static void text_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    char v[160];
    snprintf(v, sizeof v, "%s", lv_textarea_get_text(MBU.ta));
    void (*done)(const char *) = c == LV_EVENT_READY ? s_text_done : NULL;
    mb_overlay_close();
    if (done) done(v);
}

void mb_text_entry(const char *title, const char *value, void (*done)(const char *))
{
    mb_overlay_close();
    s_text_done = done;
    MBU.overlay = mb_box(MBU.root, MBU.W, MBU.H);
    lv_obj_set_style_bg_color(MBU.overlay, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(MBU.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(MBU.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = aos_label(MBU.overlay, title, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(t, MBU.W - 2 * AOS_UI_PAD);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 24);
    MBU.ta = lv_textarea_create(MBU.overlay);
    lv_textarea_set_one_line(MBU.ta, true);
    lv_textarea_set_text(MBU.ta, value);
    lv_obj_set_size(MBU.ta, MBU.W - 2 * AOS_UI_PAD, 88);
    lv_obj_align(MBU.ta, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, MBU.land ? 80 : 120);
    lv_obj_set_style_text_font(MBU.ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(MBU.ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(MBU.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(MBU.ta, 0, 0);
    lv_obj_set_style_radius(MBU.ta, 20, 0);
    lv_obj_set_style_pad_hor(MBU.ta, 24, 0);
    lv_obj_set_style_pad_ver(MBU.ta, 22, 0);
    MBU.kb = lv_keyboard_create(MBU.overlay);
    lv_obj_set_size(MBU.kb, MBU.W, MBU.land ? MBU.H / 2 : MBU.H * 2 / 5);
    lv_obj_align(MBU.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(MBU.kb, aos_font_body);
    lv_keyboard_set_textarea(MBU.kb, MBU.ta);
    lv_obj_add_event_cb(MBU.kb, text_cb, LV_EVENT_ALL, NULL);
}

/* ---- the link sheet ---- */

static const uint32_t BAUDS[] = { 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200 };
static const struct { char par; uint8_t stop; const char *name; } FORMATS[] = {
    { 'N', 1, "8N1" }, { 'E', 1, "8E1" }, { 'O', 1, "8O1" }, { 'N', 2, "8N2" },
};
static const uint16_t TIMEOUTS[] = { 100, 300, 1000, 3000 };
static const uint16_t GAPS[] = { 0, 5, 20, 50 };

static void link_sheet(void);

static void link_changed(void)
{
    mb_settings_save();
    link_sheet();
    mb_bar_refresh();
}

static void ls_tr_cb(lv_event_t *e) { mb_lock(); MB.cfg.tr = (mb_tr_t)(intptr_t)lv_event_get_user_data(e); mb_unlock(); link_changed(); }
static void ls_port_cb(lv_event_t *e)
{
    mb_lock();
    snprintf(MB.cfg.port, sizeof MB.cfg.port, "%s", (const char *)lv_event_get_user_data(e));
    mb_unlock();
    link_changed();
}
static void ls_baud_cb(lv_event_t *e) { mb_lock(); MB.cfg.baud = BAUDS[(intptr_t)lv_event_get_user_data(e)]; mb_unlock(); link_changed(); }
static void ls_baud_done(double v)
{
    if (v >= 300 && v <= 1000000) { mb_lock(); MB.cfg.baud = (uint32_t)v; mb_unlock(); }
    link_changed();
}
static void ls_baud_other_cb(lv_event_t *e) { (void)e; mb_keypad(_("Velocidad (baudios)"), MB.cfg.baud, 0, "", ls_baud_done); }
static void ls_fmt_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    mb_lock();
    MB.cfg.par = FORMATS[i].par;
    MB.cfg.stop = FORMATS[i].stop;
    mb_unlock();
    link_changed();
}
static void ls_tmo_cb(lv_event_t *e) { mb_lock(); MB.cfg.timeout_ms = TIMEOUTS[(intptr_t)lv_event_get_user_data(e)]; mb_unlock(); link_changed(); }
static void ls_gap_cb(lv_event_t *e) { mb_lock(); MB.cfg.gap_ms = GAPS[(intptr_t)lv_event_get_user_data(e)]; mb_unlock(); link_changed(); }
static void ls_host_done(const char *h)
{
    char host[64];
    snprintf(host, sizeof host, "%s", h);
    int port = 502;
    char *c = strrchr(host, ':');
    if (c) { *c = 0; port = atoi(c + 1) > 0 ? atoi(c + 1) : 502; }
    mb_lock();
    snprintf(MB.cfg.host, sizeof MB.cfg.host, "%s", host);
    MB.cfg.tport = (uint16_t)port;
    mb_unlock();
    link_changed();
}
static void ls_host_cb(lv_event_t *e)
{
    (void)e;
    char v[80] = "";
    if (MB.cfg.host[0]) snprintf(v, sizeof v, "%s:%d", MB.cfg.host, MB.cfg.tport);
    mb_text_entry(_("Equipo Modbus TCP (dirección:puerto)"), v, ls_host_done);
}
static void ls_close_cb(lv_event_t *e) { (void)e; mb_overlay_close(); mb_rebuild_page(); }

static void link_sheet(void)
{
    mb_overlay_close();
    mb_cfg_t c;
    mb_lock();
    c = MB.cfg;
    mb_unlock();
    MBU.overlay = mb_box(MBU.root, MBU.W, MBU.H);
    lv_obj_set_style_bg_color(MBU.overlay, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(MBU.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(MBU.overlay, LV_OBJ_FLAG_CLICKABLE);
    int32_t w = MBU.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = mb_column(MBU.overlay, w, MBU.H - 20);
    lv_obj_set_pos(col, AOS_UI_PAD, 20);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *head = mb_box(col, w, 80);
    lv_obj_align(aos_label(head, _("Conexión"), aos_font_large, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_align(mb_pill(head, AOS_SYM_CHECK, _("Listo"), C_TEAL, ls_close_cb, NULL), LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *r = mb_chips_row(col, w);
    mb_chip(r, "RTU", c.tr == MB_TR_RTU, ls_tr_cb, (void *)MB_TR_RTU);
    mb_chip(r, "TCP", c.tr == MB_TR_TCP, ls_tr_cb, (void *)MB_TR_TCP);
    if (c.tr == MB_TR_RTU) {
        mb_section(col, _("PUERTO"));
        r = mb_chips_row(col, w);
        for (int i = 0; i < aos_io_port_count(); i++) {
            const aos_io_port_t *p = aos_io_port_at(i);
            if (p->kind != AOS_PORT_UART) continue;
            char t[40];
            if (p->pins[2] >= 0) snprintf(t, sizeof t, "%s · RS485 · DE %d", p->name, p->pins[2]);
            else snprintf(t, sizeof t, "%s", p->name);
            mb_chip(r, t, !strcmp(p->name, c.port), ls_port_cb, (void *)p->name);
        }
        static const char *const USB[] = { "usb0", "usb1" };
        for (int i = 0; i < 2; i++) {
            char t[32];
            snprintf(t, sizeof t, "%s · USB", USB[i]);
            mb_chip(r, t, !strcmp(USB[i], c.port), ls_port_cb, (void *)USB[i]);
        }
        mb_caption(col, _("Un puerto con pin DE maneja el transceptor RS485 solo; usb0 es un adaptador USB-RS485 enchufado al host USB."), w);
        mb_section(col, _("VELOCIDAD"));
        r = mb_chips_row(col, w);
        bool std = false;
        for (size_t i = 0; i < sizeof BAUDS / sizeof BAUDS[0]; i++) {
            char t[12];
            snprintf(t, sizeof t, "%u", (unsigned)BAUDS[i]);
            mb_chip(r, t, c.baud == BAUDS[i], ls_baud_cb, (void *)(intptr_t)i);
            std |= c.baud == BAUDS[i];
        }
        char t[24];
        if (std) snprintf(t, sizeof t, "%s", _("otra…"));
        else snprintf(t, sizeof t, "%u", (unsigned)c.baud);
        mb_chip(r, t, !std, ls_baud_other_cb, NULL);
        mb_section(col, _("FORMATO"));
        r = mb_chips_row(col, w);
        for (size_t i = 0; i < sizeof FORMATS / sizeof FORMATS[0]; i++)
            mb_chip(r, FORMATS[i].name, FORMATS[i].par == c.par && FORMATS[i].stop == c.stop, ls_fmt_cb, (void *)(intptr_t)i);
        mb_caption(col, _("El estándar pide 8E1; muchos equipos vienen en 8N1 o 8N2. Si no contesta, en Maestro está \"Buscar velocidad\"."), w);
    } else {
        mb_section(col, _("EQUIPO"));
        r = mb_chips_row(col, w);
        char t[96];
        if (c.host[0]) snprintf(t, sizeof t, "%s:%d", c.host, c.tport);
        else snprintf(t, sizeof t, "%s", _("tocá para poner la dirección"));
        mb_chip(r, t, false, ls_host_cb, NULL);
        mb_caption(col, _("Por TCP la app es sólo maestro, por ahora."), w);
    }
    mb_section(col, _("ESPERA DE LA RESPUESTA (MAESTRO)"));
    r = mb_chips_row(col, w);
    for (size_t i = 0; i < sizeof TIMEOUTS / sizeof TIMEOUTS[0]; i++) {
        char t[16];
        snprintf(t, sizeof t, "%u ms", TIMEOUTS[i]);
        mb_chip(r, t, c.timeout_ms == TIMEOUTS[i], ls_tmo_cb, (void *)(intptr_t)i);
    }
    mb_section(col, _("SILENCIO ANTES DE CADA PEDIDO"));
    r = mb_chips_row(col, w);
    for (size_t i = 0; i < sizeof GAPS / sizeof GAPS[0]; i++) {
        char t[16];
        snprintf(t, sizeof t, "%u ms", GAPS[i]);
        mb_chip(r, t, c.gap_ms == GAPS[i], ls_gap_cb, (void *)(intptr_t)i);
    }
    mb_caption(col, _("Algunos equipos (la Riden, varios medidores) dejan de contestar si los pedidos llegan pegados."), w);
}

/* ---- the bar on top ---- */

static void bar_conn_cb(lv_event_t *e) { (void)e; link_sheet(); }

static void bar_btn_cb(lv_event_t *e)
{
    (void)e;
    mb_lock();
    MB.want_open = !MB.want_open;
    if (!MB.want_open) { MB.m_live = false; MB.tool_stop = true; }
    MB.err[0] = 0;
    mb_unlock();
    mb_bar_refresh();
    if (MBU.tab == TAB_MASTER) mb_rebuild_page();
}

void mb_bar_refresh(void)
{
    if (!MBU.bar_conn) return;
    mb_lock();
    mb_cfg_t c = MB.cfg;
    bool want = MB.want_open, open = MB.open;
    char err[96], desc[64];
    snprintf(err, sizeof err, "%s", MB.err);
    snprintf(desc, sizeof desc, "%s", MB.desc);
    mb_role_t role = MB.role;
    mb_unlock();
    char t[96];
    if (c.tr == MB_TR_RTU) snprintf(t, sizeof t, "%s · %u 8%c%u", c.port, (unsigned)c.baud, c.par ? c.par : 'N', c.stop ? c.stop : 1);
    else snprintf(t, sizeof t, "TCP %s:%d", c.host[0] ? c.host : "?", c.tport);
    lv_label_set_text(lv_obj_get_child(MBU.bar_conn, 0), t);
    if (err[0]) {
        lv_label_set_text(MBU.bar_state, err);
        lv_obj_set_style_text_color(MBU.bar_state, AOS_C_ORANGE, 0);
    } else if (open) {
        lv_label_set_text_fmt(MBU.bar_state, "%s · %s", aos_tr(ROLE_NAME[role]), desc);
        lv_obj_set_style_text_color(MBU.bar_state, AOS_C_GREEN, 0);
    } else {
        lv_label_set_text_fmt(MBU.bar_state, want ? _("abriendo… (%s)") : _("cerrado · rol: %s"), aos_tr(ROLE_NAME[role]));
        lv_obj_set_style_text_color(MBU.bar_state, AOS_C_DIM, 0);
    }
    lv_obj_set_style_bg_color(MBU.bar_btn, want ? AOS_C_RED : C_TEAL, 0);
    lv_label_set_text(lv_obj_get_child(MBU.bar_btn, 0), want ? AOS_SYM_STOP : AOS_SYM_PLAY);
    lv_label_set_text(lv_obj_get_child(MBU.bar_btn, 1), want ? _("Cerrar") : _("Abrir"));
}

/* ---- pages ---- */

void mb_rebuild_page(void)
{
    /* the same tab built again keeps where its columns were scrolled to */
    static int last_tab = -1;
    int32_t sy[2] = { 0, 0 };
    if (last_tab == MBU.tab)
        for (uint32_t i = 0; i < 2 && i < lv_obj_get_child_count(MBU.content); i++) sy[i] = lv_obj_get_scroll_y(lv_obj_get_child(MBU.content, (int32_t)i));
    last_tab = MBU.tab;
    mb_overlay_close();
    prof_forget();
    lv_obj_clean(MBU.content);
    MBU.m_status = MBU.m_stats = MBU.m_grid = MBU.m_text = MBU.m_tool = MBU.m_tool_bar = MBU.m_found = NULL;
    MBU.s_stats = MBU.s_last = NULL;
    memset(MBU.s_hr, 0, sizeof MBU.s_hr);
    memset(MBU.s_coil, 0, sizeof MBU.s_coil);
    memset(MBU.s_ir, 0, sizeof MBU.s_ir);
    memset(MBU.s_di, 0, sizeof MBU.s_di);
    MBU.mon_tot = MBU.mon_rows = NULL;
    MBU.t_list = MBU.t_pause = NULL;
    memset(MBU.t_rows, 0, sizeof MBU.t_rows);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_color_t c = i == MBU.tab ? C_TEAL : AOS_C_DIM;
        lv_obj_set_style_text_color(lv_obj_get_child(MBU.tabs[i], 0), c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(MBU.tabs[i], 1), c, 0);
    }
    int32_t ch = lv_obj_get_height(MBU.content) - 8;
    switch (MBU.tab) {
    case TAB_MASTER: tab_master_build(MBU.content, ch); break;
    case TAB_SLAVE: tab_slave_build(MBU.content, ch); break;
    case TAB_MON: tab_mon_build(MBU.content, ch); break;
    case TAB_TRAFFIC: tab_traffic_build(MBU.content, ch); break;
    default: tab_wire_build(MBU.content, ch); break;
    }
    lv_obj_update_layout(MBU.content);
    for (uint32_t i = 0; i < 2 && i < lv_obj_get_child_count(MBU.content); i++)
        if (sy[i]) lv_obj_scroll_to_y(lv_obj_get_child(MBU.content, (int32_t)i), sy[i], LV_ANIM_OFF);
    MBU.tseen = MBU.logseen = 0xFFFFFFFF;
}

static void tab_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    MBU.tab = t;
    if (t <= TAB_MON) {
        mb_lock();
        mb_role_t r = (mb_role_t)t;
        if (MB.role != r) {
            MB.role = r;
            MB.m_live = false;
            MB.tool_stop = true;
        }
        mb_unlock();
    }
    mb_settings_save();
    mb_rebuild_page();
    mb_bar_refresh();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (MBU.boot) {
        MBU.boot = false;
        MBU.paused = !MBU.paused;
        aos_ui_toast(MBU.paused ? _("Tráfico en pausa") : _("Tráfico sigue"), 1200);
        if (MBU.tab == TAB_TRAFFIC) mb_rebuild_page();
    }
    mb_lock();
    uint32_t seq = MB.seq, mseq = MB.m_seq + MB.tool_seq, sseq = MB.s_seq, lseq = MB.log_seq;
    mb_unlock();
    if (seq != MBU.seen) {
        MBU.seen = seq;
        mb_bar_refresh();
        if (MBU.tab == TAB_MASTER) tab_master_refresh(mseq != MBU.mseen);
        else if (MBU.tab == TAB_SLAVE) tab_slave_refresh();
        else if (MBU.tab == TAB_MON) tab_mon_refresh();
        MBU.mseen = mseq;
    }
    if (MBU.tab == TAB_SLAVE && sseq != MBU.sseen) { MBU.sseen = sseq; tab_slave_refresh(); }
    static uint32_t n;
    if (MBU.tab == TAB_SLAVE && ++n % 4 == 0) tab_slave_refresh();       /* the moving inputs */
    if (MBU.tab == TAB_TRAFFIC && !MBU.paused && lseq != MBU.logseen) { MBU.logseen = lseq; tab_traffic_refresh(); }
    if (MBU.tab <= TAB_SLAVE && !MBU.overlay) prof_refresh();
    prof_tick();
    remote_tick();
}

static void build_screen(lv_obj_t *root)
{
    lv_obj_clean(root);
    MBU.root = root;
    MBU.overlay = NULL;
    MBU.W = lv_obj_get_width(root);
    MBU.H = lv_obj_get_height(root);
    MBU.land = MBU.W > MBU.H;
    const int32_t tab_h = MBU.land ? 92 : 112, bar_h = 96;
    /* the bar */
    lv_obj_t *bar = mb_box(root, MBU.W, bar_h);
    lv_obj_set_style_pad_hor(bar, AOS_UI_PAD, 0);
    MBU.bar_conn = mb_box(bar, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_radius(MBU.bar_conn, 30, 0);
    lv_obj_set_style_pad_hor(MBU.bar_conn, 20, 0);
    lv_obj_set_style_bg_color(MBU.bar_conn, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(MBU.bar_conn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(MBU.bar_conn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(MBU.bar_conn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(MBU.bar_conn, bar_conn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(aos_label(MBU.bar_conn, "", aos_font_small, AOS_C_TEXT));
    lv_obj_align(MBU.bar_conn, LV_ALIGN_TOP_LEFT, 0, 4);
    MBU.bar_state = aos_label(bar, "", aos_font_tiny, AOS_C_DIM);
    lv_obj_set_width(MBU.bar_state, MBU.W - 2 * AOS_UI_PAD - 220);
    lv_label_set_long_mode(MBU.bar_state, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(MBU.bar_state, LV_ALIGN_BOTTOM_LEFT, 8, -2);
    MBU.bar_btn = mb_pill(bar, AOS_SYM_PLAY, _("Abrir"), C_TEAL, bar_btn_cb, NULL);
    lv_obj_align(MBU.bar_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    /* the content */
    MBU.content = mb_box(root, MBU.W, MBU.H - tab_h - bar_h);
    lv_obj_set_pos(MBU.content, 0, bar_h);
    lv_obj_set_style_pad_hor(MBU.content, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(MBU.content, 8, 0);
    lv_obj_update_layout(MBU.content);
    /* the tabs */
    lv_obj_t *tb = mb_box(root, MBU.W, tab_h);
    lv_obj_set_pos(tb, 0, MBU.H - tab_h);
    lv_obj_set_style_bg_color(tb, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *t = mb_box(tb, MBU.W / TAB_COUNT, tab_h);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(aos_label(t, TAB_GLYPH[i], &aos_sym_44, AOS_C_DIM), LV_ALIGN_CENTER, 0, MBU.land ? -14 : -16);
        lv_obj_align(aos_label(t, aos_tr(TAB_NAME[i]), aos_font_tiny, AOS_C_DIM), LV_ALIGN_CENTER, 0, MBU.land ? 26 : 30);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        MBU.tabs[i] = t;
    }
    mb_rebuild_page();
    mb_bar_refresh();
}

/* ---- the app ---- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&MBU, 0, sizeof MBU);
    if (!mb_bus_init()) {
        lv_obj_center(aos_label(root, _("No hay memoria para el banco Modbus"), aos_font_body, AOS_C_RED));
        return NULL;
    }
    mb_lock();
    settings_load();
    MB.want_open = false;           /* every time: see the top of this file */
    MB.err[0] = 0;
    mb_unlock();
    prof_init();
    MBU.seen = MBU.mseen = MBU.sseen = 0xFFFFFFFF;
    build_screen(root);
    MBU.timer = lv_timer_create(timer_cb, 150, NULL);
    return &MBU;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (MBU.timer) lv_timer_delete(MBU.timer);
    MBU.timer = NULL;
    mb_settings_save();
    aos_hal_live_clear("aos.modbus");
    mb_bus_deinit();
    memset(&MBU, 0, sizeof MBU);
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    build_screen(root);
    return true;
}

static void show(aos_app_t *self, void *inst) { (void)self; (void)inst; if (MBU.timer) lv_timer_resume(MBU.timer); }
static void hide(aos_app_t *self, void *inst) { (void)self; (void)inst; if (MBU.timer) lv_timer_pause(MBU.timer); }

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (MBU.overlay) {
        mb_overlay_close();
        mb_rebuild_page();
        return true;
    }
    return false;
}

static bool button(aos_app_t *self, void *inst, int action)
{
    (void)self;
    (void)inst;
    if (action != AOS_BUTTON_CLICK) return action == AOS_BUTTON_PRESS;
    MBU.boot = true;                  /* acted on in the next timer round */
    return true;
}

/* Two boxes on a pair of wires, and the frame between them. */
static const uint8_t MB_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER,   0,  14, 76,  5,  2,         AIC_C_TEXT, 255),   /* A */
    AIC_RECT(AIC_CENTER,   0,  25, 76,  5,  2,         AIC_C_TEXT, 150),   /* B */
    AIC_RECT(AIC_CENTER, -22,   2,  4, 22,  1,         AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER,  22,   2,  4, 22,  1,         AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, -22, -16, 26, 20,  4,         AIC_C_TEXT, 255),   /* master */
    AIC_RECT(AIC_CENTER,  22, -16, 26, 20,  4,         AIC_C_YELLOW, 255), /* slave */
    AIC_RECT(AIC_CENTER,   0, -28, 14,  4,  2,         AIC_C_YELLOW, 255), /* the frame going */
    AIC_RECT(AIC_CENTER,   0, -20, 14,  4,  2,         AIC_C_TEXT, 200),   /* and coming back */
    AIC_END
};

static bool init(aos_app_t *app)
{
    app->desc.id       = "aos.modbus";
    app->desc.name     = "Modbus";
    app->desc.icon     = AOS_SYM_LAN;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x10B981;
    app->desc.color_b  = 0x047857;
    app->desc.flags    = AOS_APP_FLAG_KEEP | AOS_APP_FLAG_KEEP_AWAKE;
    app->desc.order    = 121;
    aos_icon_set_ops(app, MB_ICON, sizeof MB_ICON);

    app->create  = create;
    app->destroy = destroy;
    app->resize  = resize;
    app->show    = show;
    app->hide    = hide;
    app->back    = back;
    app->button  = button;
    return true;
}

AOS_APP_ENTRY(init);
