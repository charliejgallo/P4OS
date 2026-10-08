/*
 * BLE - a Bluetooth LE scanner and analyser for P4OS (apps/ble/README.md).
 *
 * Four tabs, like Red's:
 *
 *   Cerca     who is around: name or what it is, company, address type,
 *             signal, and what a sensor says, sorted and filtered.
 *   Radar     the same on a radar, by estimated distance; a tap picks one,
 *             and the finder takes it from there (a big signal meter that
 *             beeps faster as you get closer).
 *   Sensores  the readings broadcast by thermometers and the like (BTHome,
 *             pvvx/ATC, Xiaomi, Govee, Ruuvi, SwitchBot, Qingping, Inkbird,
 *             Eddystone TLM), with their last two hours; to CSV and MQTT.
 *   Aire      the statistics: packets a second, devices, companies, kinds of
 *             address and of advertisement, the spread of the signal.
 *
 * A device opens its detail: every AD structure explained, the raw bytes,
 * the signal over two minutes, the interval; from there the finder and,
 * when it takes connections, the GATT explorer (bl_gatt.c).
 *
 * The radio is the firmware's (aos_hal_ble_*, components/aos_ble/
 * aos_ble_scan.c); the meaning of the bytes is bl_decode.c's.
 */
#include "bl.h"
#include "aos_icon_ops.h"

#include <stdio.h>
#include <string.h>

bl_t BL;

/* The Bluetooth rune with three arcs of a radar on its right, on blue. */
static const uint8_t BLE_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -14, 0, 7, 50, 3, AIC_C_TEXT, 255),
    AIC_ARC(AIC_CENTER, -2, -12, 26, 0, 6, 0, 360, 270, 90, 0, AIC_C_BG, 0, AIC_C_TEXT, 255),
    AIC_ARC(AIC_CENTER, -2, 12, 26, 0, 6, 0, 360, 270, 90, 0, AIC_C_BG, 0, AIC_C_TEXT, 255),
    AIC_ARC(AIC_CENTER, 4, 0, 50, 0, 5, 0, 360, 300, 60, 0, AIC_C_BG, 0, AIC_C_TEXT, 200),
    AIC_ARC(AIC_CENTER, 4, 0, 74, 0, 5, 0, 360, 305, 55, 0, AIC_C_BG, 0, AIC_C_TEXT, 120),
    AIC_END
};

static const char *const TAB_NAME[BL_TAB_COUNT] = { N_("Cerca"), N_("Radar"), N_("Sensores"), N_("Aire") };
static const char *const TAB_GLYPH[BL_TAB_COUNT] = { AOS_SYM_BLUETOOTH, AOS_SYM_RADAR, AOS_SYM_THERMOMETER, AOS_SYM_CHART_AREASPLINE };

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

lv_obj_t *bl_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *bl_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = bl_box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    return c;
}

lv_obj_t *bl_vcard(lv_obj_t *parent, int32_t w, int32_t pad, int32_t gap)
{
    lv_obj_t *c = bl_card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, pad, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, gap, 0);
    return c;
}

lv_obj_t *bl_column(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_set_style_pad_bottom(c, 24, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

lv_obj_t *bl_row(lv_obj_t *parent, int32_t w, int32_t h, int32_t gap)
{
    lv_obj_t *r = bl_box(parent, w, h);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    return r;
}

lv_obj_t *bl_wrap(lv_obj_t *parent, int32_t w, int32_t gap)
{
    lv_obj_t *r = bl_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, gap, 0);
    return r;
}

lv_obj_t *bl_pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = bl_box(parent, LV_SIZE_CONTENT, 76);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 38, 0);
    lv_obj_set_style_pad_hor(b, 26, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    aos_make_decorative(aos_label(b, glyph ? glyph : "", &aos_sym_28, lv_color_white()));
    aos_make_decorative(aos_label(b, text ? text : "", aos_font_body, lv_color_white()));
    if (!glyph || !glyph[0]) lv_obj_add_flag(lv_obj_get_child(b, 0), LV_OBJ_FLAG_HIDDEN);
    return b;
}

void bl_pill_set(lv_obj_t *b, const char *glyph, const char *text, lv_color_t bg)
{
    if (!b) return;
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_label_set_text(lv_obj_get_child(b, 0), glyph);
    lv_label_set_text(lv_obj_get_child(b, 1), text);
}

lv_obj_t *bl_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = bl_box(parent, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_radius(c, 30, 0);
    lv_obj_set_style_pad_hor(c, 22, 0);
    lv_obj_set_style_bg_color(c, on ? BL_C_KEYTOP : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_70, LV_STATE_PRESSED);
    if (cb) {
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_t *l = aos_label(c, text, aos_font_small, on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    return c;
}

lv_obj_t *bl_caption(lv_obj_t *parent, const char *text, int32_t w)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

lv_obj_t *bl_section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 10, 0);
    lv_obj_set_style_pad_top(t, 6, 0);
    return t;
}

lv_obj_t *bl_round_icon(lv_obj_t *parent, const char *glyph, lv_color_t col, int32_t size)
{
    lv_obj_t *c = bl_box(parent, size, size);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, col, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_30, 0);
    lv_obj_center(aos_label(c, glyph, size >= 80 ? &aos_sym_44 : &aos_sym_28, col));
    aos_make_decorative(c);
    return c;
}

/* A title with a "<" that goes back, for the pages over a tab. */
lv_obj_t *bl_back_bar(lv_obj_t *parent, int32_t w, const char *title, lv_event_cb_t back_cb)
{
    lv_obj_t *bar = bl_box(parent, w, 84);
    lv_obj_t *b = bl_box(bar, 84, 84);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(b, 42, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_center(aos_label(b, AOS_SYM_CHEVRON_LEFT, &aos_sym_44, AOS_C_TEXT));
    lv_obj_add_event_cb(b, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = aos_label(bar, title, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(t, w - 110);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 104, 0);
    return bar;
}

lv_obj_t *bl_kv(lv_obj_t *parent, int32_t w, const char *key, const char *val)
{
    lv_obj_t *r = bl_box(parent, w, LV_SIZE_CONTENT);
    int32_t kw = w * 34 / 100;
    lv_obj_t *k = aos_label(r, key, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(k, kw);
    lv_label_set_long_mode(k, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *v = aos_label(r, val, aos_font_small, AOS_C_TEXT);
    lv_obj_set_width(v, w - kw - 12);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_x(v, kw + 12);
    return r;
}

lv_obj_t *bl_canvas_obj(lv_obj_t *parent, int32_t w, int32_t h, lv_event_cb_t draw_cb)
{
    lv_obj_t *o = bl_box(parent, w, h);
    lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}

void bl_draw_text(lv_layer_t *layer, const char *t, const lv_font_t *f, lv_color_t c, int32_t x, int32_t y, int32_t w,
                  lv_text_align_t al)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = t;
    d.text_local = 1;           /* LVGL draws later, in its own threads */
    d.font = f;
    d.color = c;
    d.align = al;
    lv_area_t a = { x, y, x + w, y + lv_font_get_line_height(f) };
    lv_draw_label(layer, &d, &a);
}

void bl_draw_line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, int32_t w, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.color = c;
    d.width = w;
    d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

void bl_fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, lv_opa_t opa, int32_t r)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = opa;
    d.radius = r;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

lv_color_t bl_rssi_color(int rssi)
{
    return rssi >= -60 ? AOS_C_GREEN : rssi >= -75 ? BL_C : rssi >= -88 ? AOS_C_YELLOW : AOS_C_ORANGE;
}

/* The strongest of each second as bars, oldest on the left; n seconds
 * ending at 'newest' (a second number). -100 at the bottom, -30 at the top. */
void bl_spark(lv_layer_t *layer, const lv_area_t *a, const int8_t *hist, int n, int newest, lv_color_t c)
{
    int32_t w = lv_area_get_width(a), h = lv_area_get_height(a);
    if (n <= 0 || w <= 0) return;
    float bw = (float)w / n;
    for (int k = 0; k < n; k++) {
        int v = hist[((uint32_t)(newest - (n - 1 - k)) + BL_HIST * 1000) % BL_HIST];
        if (v == BL_NO_RSSI) continue;
        float f = (v + 100) / 70.0f;
        if (f < 0.04f) f = 0.04f;
        if (f > 1) f = 1;
        int32_t x1 = a->x1 + (int32_t)(k * bw);
        int32_t x2 = a->x1 + (int32_t)((k + 1) * bw) - (bw > 3 ? 1 : 0);
        if (x2 < x1) x2 = x1;
        bl_fill(layer, x1, a->y2 - (int32_t)(f * h), x2, a->y2, c, LV_OPA_80, 0);
    }
}

/* -------------------------------------------------------------------------- */
/* The text field                                                              */
/* -------------------------------------------------------------------------- */

static void (*s_text_done)(const char *);
static lv_obj_t *s_ta;

void bl_overlay_close(void)
{
    if (BL.overlay) lv_obj_delete(BL.overlay);
    BL.overlay = NULL;
    s_ta = NULL;
}

static void text_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    char v[520];
    snprintf(v, sizeof v, "%s", lv_textarea_get_text(s_ta));
    void (*done)(const char *) = c == LV_EVENT_READY ? s_text_done : NULL;
    bl_overlay_close();
    if (done) done(v);
}

void bl_text_entry(const char *title, const char *value, bool hex_kb, void (*done)(const char *))
{
    bl_overlay_close();
    s_text_done = done;
    BL.overlay = bl_box(BL.root, BL.W, BL.H);
    lv_obj_set_style_bg_color(BL.overlay, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(BL.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(BL.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(aos_label(BL.overlay, title, aos_font_title, AOS_C_TEXT), LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 30);
    s_ta = lv_textarea_create(BL.overlay);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_max_length(s_ta, 510);
    lv_textarea_set_text(s_ta, value ? value : "");
    lv_obj_set_size(s_ta, BL.W - 2 * AOS_UI_PAD, 88);
    lv_obj_align(s_ta, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 100);
    lv_obj_set_style_text_font(s_ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(s_ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(s_ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(s_ta, 0, 0);
    lv_obj_set_style_radius(s_ta, 20, 0);
    lv_obj_set_style_pad_hor(s_ta, 24, 0);
    lv_obj_set_style_pad_ver(s_ta, 22, 0);
    lv_obj_t *kb = lv_keyboard_create(BL.overlay);
    lv_obj_set_size(kb, BL.W, BL.land ? BL.H / 2 : BL.H * 2 / 5);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(kb, aos_font_body);
    if (hex_kb) lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, s_ta);
    lv_obj_add_event_cb(kb, text_cb, LV_EVENT_ALL, NULL);
}

/* -------------------------------------------------------------------------- */
/* Settings                                                                    */
/* -------------------------------------------------------------------------- */

static int pref_int(const char *k, int def)
{
    int32_t v;
    return aos_hal_pref_get_i32(k, &v) ? (int)v : def;
}

static void settings_load(void)
{
    BL.tab = pref_int("ble_tab", BL_TAB_LIST);
    if (BL.tab < 0 || BL.tab >= BL_TAB_COUNT) BL.tab = BL_TAB_LIST;
    BL.sort = pref_int("ble_sort", BL_SORT_RSSI);
    if (BL.sort < 0 || BL.sort >= BL_SORT_COUNT) BL.sort = BL_SORT_RSSI;
    BL.filter = pref_int("ble_filt", BL_FILT_ALL);
    if (BL.filter < 0 || BL.filter >= BL_FILT_COUNT) BL.filter = BL_FILT_ALL;
    BL.env = pref_int("ble_env", 1);
    BL.min_rssi = pref_int("ble_minr", -100);
    BL.active = pref_int("ble_act", 1) != 0;
    BL.duty = pref_int("ble_duty", 30);
    BL.sound = pref_int("ble_snd", 1) != 0;
    BL.log_csv = pref_int("ble_csv", 0) != 0;
    BL.mqtt = pref_int("ble_mqtt", 0) != 0;
    BL.hide_gone = pref_int("ble_hide", 0) != 0;
}

void bl_settings_save(void)
{
    aos_hal_pref_set_i32("ble_tab", BL.tab);
    aos_hal_pref_set_i32("ble_sort", BL.sort);
    aos_hal_pref_set_i32("ble_filt", BL.filter);
    aos_hal_pref_set_i32("ble_env", BL.env);
    aos_hal_pref_set_i32("ble_minr", BL.min_rssi);
    aos_hal_pref_set_i32("ble_act", BL.active);
    aos_hal_pref_set_i32("ble_duty", BL.duty);
    aos_hal_pref_set_i32("ble_snd", BL.sound);
    aos_hal_pref_set_i32("ble_csv", BL.log_csv);
    aos_hal_pref_set_i32("ble_mqtt", BL.mqtt);
    aos_hal_pref_set_i32("ble_hide", BL.hide_gone);
}

void bl_scan_status(char *out, size_t n)
{
    int alive = 0;
    for (int i = 0; i < BL.ndev; i++) alive += bl_alive(&BL.dev[i]);
    char pps[16];
    bl_fmt_num(pps, sizeof pps, BL.air.pps, BL.air.pps < 10 ? 1 : 0);
    if (BL.bt_off) snprintf(out, n, "%s", _("Bluetooth apagado"));
    else if (BL.paused) snprintf(out, n, _("%d cerca · en pausa"), alive);
    else snprintf(out, n, _("%d cerca · %s paq/s · %s %d %%"), alive, pps, BL.active ? _("activo") : _("pasivo"), BL.duty);
}

static void bt_on_cb(lv_event_t *e)
{
    (void)e;
    aos_hal_bt_enable(true);
    aos_ui_toast(_("Prendiendo Bluetooth..."), 1500);
    BL.bt_off = false;
    bl_scan_apply();
    bl_rebuild();
}

lv_obj_t *bl_bt_off_card(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *c = bl_vcard(parent, w, 28, 18);
    lv_obj_t *r = bl_row(c, w - 56, 80, 20);
    bl_round_icon(r, AOS_SYM_BLUETOOTH_OFF, AOS_C_DIM, 80);
    lv_obj_t *t = aos_label(r, _("Bluetooth está apagado"), aos_font_body, AOS_C_TEXT);
    lv_obj_set_flex_grow(t, 1);
    bl_caption(c, _("Para escuchar lo que anuncian los equipos de alrededor hace falta el Bluetooth. Se puede apagar de nuevo en Ajustes."), w - 56);
    bl_pill(c, AOS_SYM_BLUETOOTH, _("Prender Bluetooth"), BL_C, bt_on_cb, NULL);
    return c;
}

/* ---- the settings sheet ---- */

static void sheet_build(void);

static void sheet_close_cb(lv_event_t *e) { (void)e; bl_overlay_close(); bl_rebuild(); }

static void opt_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int what = v >> 8, val = (int8_t)(v & 0xFF);
    switch (what) {
    case 0: BL.active = val != 0; break;
    case 1: BL.duty = val; break;
    case 2: BL.env = val; break;
    case 3: BL.min_rssi = val; break;
    case 4: BL.hide_gone = val != 0; break;
    case 5: BL.sound = val != 0; break;
    case 6: BL.log_csv = val != 0; break;
    case 7: BL.mqtt = val != 0; break;
    }
    bl_settings_save();
    bl_scan_apply();
    sheet_build();
}

#define OPT(what, val) (void *)(intptr_t)(((what) << 8) | ((val) & 0xFF))

static void forget_cb(lv_event_t *e)
{
    (void)e;
    bl_forget_all();
    aos_ui_toast(_("Lista vaciada (los favoritos quedan)"), 1500);
    bl_overlay_close();
    bl_rebuild();
}

static void sheet_opts(lv_obj_t *col, int32_t w, const char *title, const char *hint, int what,
                       const char *const *names, const int *vals, int n, int cur)
{
    lv_obj_t *c = bl_vcard(col, w, 24, 14);
    aos_label(c, title, aos_font_body, AOS_C_TEXT);
    if (hint) bl_caption(c, hint, w - 48);
    lv_obj_t *r = bl_wrap(c, w - 48, 12);
    for (int i = 0; i < n; i++) bl_chip(r, names[i], vals[i] == cur, opt_cb, OPT(what, vals[i]));
}

static void sheet_build(void)
{
    if (BL.overlay) lv_obj_delete(BL.overlay);
    BL.overlay = bl_box(BL.root, BL.W, BL.H);
    lv_obj_set_style_bg_color(BL.overlay, lv_color_hex(0x0B0B0F), 0);
    lv_obj_set_style_bg_opa(BL.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(BL.overlay, LV_OBJ_FLAG_CLICKABLE);
    int32_t w = BL.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = bl_column(BL.overlay, w, BL.H);
    lv_obj_set_x(col, AOS_UI_PAD);
    lv_obj_set_style_pad_top(col, 12, 0);
    bl_back_bar(col, w, _("Ajustes del escaneo"), sheet_close_cb);

    static const char *const MODE[] = { N_("Activo"), N_("Pasivo") };
    const char *mode[2] = { _(MODE[0]), _(MODE[1]) };
    static const int MODE_V[] = { 1, 0 };
    sheet_opts(col, w, _("Escaneo"), _("Activo le pide a cada equipo su respuesta de escaneo, donde muchos dicen su nombre. Pasivo sólo escucha: no se anuncia ante nadie."),
               0, mode, MODE_V, 2, BL.active);
    static const char *const DUTY[] = { "10 %", "30 %", "60 %", "100 %" };
    static const int DUTY_V[] = { 10, 30, 60, 100 };
    sheet_opts(col, w, _("Tiempo escuchando"), _("La radio es una sola para Wi-Fi y Bluetooth: escuchar todo el tiempo oye más paquetes y hace más lento el Wi-Fi."),
               1, DUTY, DUTY_V, 4, BL.duty);
    static const char *const ENV[] = { N_("Al aire libre"), N_("Casa"), N_("Oficina") };
    const char *env[3] = { _(ENV[0]), _(ENV[1]), _(ENV[2]) };
    static const int ENV_V[] = { 0, 1, 2 };
    sheet_opts(col, w, _("Entorno, para la distancia"), _("La distancia sale de cuánto se debilita la señal, y eso depende de las paredes y la gente: es una estimación."),
               2, env, ENV_V, 3, BL.env);
    static const char *const MIN[] = { N_("Todos"), "-90 dBm", "-80 dBm", "-70 dBm" };
    const char *mn[4] = { _(MIN[0]), MIN[1], MIN[2], MIN[3] };
    static const int MIN_V[] = { -100, -90, -80, -70 };
    sheet_opts(col, w, _("Señal mínima"), _("Esconde los que están lejos (los favoritos se ven siempre)."), 3, mn, MIN_V, 4, BL.min_rssi);
    static const char *const YN[] = { N_("Sí"), N_("No") };
    const char *yn[2] = { _(YN[0]), _(YN[1]) };
    static const int YN_V[] = { 1, 0 };
    sheet_opts(col, w, _("Esconder los que se fueron"), _("Los que no se oyen hace 30 segundos."), 4, yn, YN_V, 2, BL.hide_gone);
    sheet_opts(col, w, _("Sonido del buscador"), NULL, 5, yn, YN_V, 2, BL.sound);
    sheet_opts(col, w, _("Guardar los sensores en CSV"), _("Una línea por minuto y por sensor en ble/sensores-<día>.csv de la tarjeta, también con la app cerrada."),
               6, yn, YN_V, 2, BL.log_csv);
    sheet_opts(col, w, bl_mqtt_ready() ? _("Publicar los sensores por MQTT") : _("Publicar los sensores por MQTT (no conectado)"),
               _("<placa>/ble/<dirección> con las lecturas en JSON, una vez por minuto. El servidor se configura en la app MQTT."),
               7, yn, YN_V, 2, BL.mqtt);
    lv_obj_t *c = bl_vcard(col, w, 24, 14);
    aos_label(c, _("Lista"), aos_font_body, AOS_C_TEXT);
    bl_caption(c, _("Olvida lo escuchado hasta ahora. Los favoritos y sus nombres quedan."), w - 48);
    bl_pill(c, AOS_SYM_DELETE, _("Vaciar la lista"), AOS_C_CARD2, forget_cb, NULL);
}

static void settings_cb(lv_event_t *e) { (void)e; sheet_build(); }

/* -------------------------------------------------------------------------- */
/* Pages                                                                       */
/* -------------------------------------------------------------------------- */

static void pause_cb(lv_event_t *e)
{
    (void)e;
    BL.paused = !BL.paused;
    bl_scan_apply();
    bl_rebuild();
}

/* The strip on top of every tab: the state of the scan, pause, settings. */
static void top_strip(lv_obj_t *page)
{
    lv_obj_t *r = bl_row(page, BL.cw, 80, 12);
    lv_obj_t *t = aos_label(r, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    char s[96];
    bl_scan_status(s, sizeof s);
    lv_label_set_text(t, s);
    lv_obj_set_user_data(page, t);         /* refreshed by bl_rebuild's owner */
    bl_pill(r, BL.paused ? AOS_SYM_PLAY : AOS_SYM_PAUSE, NULL, BL.paused ? BL_C : AOS_C_CARD2, pause_cb, NULL);
    bl_pill(r, AOS_SYM_COG, NULL, AOS_C_CARD2, settings_cb, NULL);
}

static lv_obj_t *s_status;

void bl_list_gone(void);
void bl_radar_gone(void);
void bl_sens_gone(void);
void bl_air_gone(void);
void bl_detail_gone(void);
void bl_gatt_gone(void);

void bl_page_gone(void)
{
    bl_list_gone();
    bl_radar_gone();
    bl_sens_gone();
    bl_air_gone();
    bl_detail_gone();
    bl_gatt_gone();
    s_status = NULL;
}

void bl_rebuild(void)
{
    bl_page_gone();
    lv_obj_clean(BL.content);
    s_status = NULL;
    for (int i = 0; i < BL_TAB_COUNT; i++) {
        lv_color_t c = (i == BL.tab && BL.page == BL_PAGE_TAB) ? BL_C : AOS_C_DIM;
        lv_obj_set_style_text_color(lv_obj_get_child(BL.tabs[i], 0), c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(BL.tabs[i], 1), c, 0);
    }
    lv_obj_t *page = bl_box(BL.content, BL.cw, BL.ch);
    /* the builders size things by their parent's height: it has to be laid out */
    lv_obj_update_layout(page);
    if (BL.page == BL_PAGE_DETAIL) { bl_detail_build(page); return; }
    if (BL.page == BL_PAGE_FINDER) { bl_finder_build(page); return; }
    if (BL.page == BL_PAGE_GATT) { bl_gatt_build(page); return; }
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 8, 0);
    top_strip(page);
    s_status = lv_obj_get_user_data(page);
    lv_obj_t *body = bl_box(page, BL.cw, BL.ch - 88);
    lv_obj_update_layout(page);
    if (BL.bt_off && BL.tab != BL_TAB_AIR) {
        bl_bt_off_card(body, BL.cw);
        return;
    }
    if (BL.tab == BL_TAB_LIST) bl_list_build(body);
    else if (BL.tab == BL_TAB_RADAR) bl_radar_build(body);
    else if (BL.tab == BL_TAB_SENS) bl_sens_build(body);
    else bl_air_build(body);
}

static bool sel_resolve(void)
{
    BL.sel = bl_find(BL.sel_addr);
    return BL.sel >= 0;
}

static void open_page(int idx, int page)
{
    if (idx < 0 || idx >= BL.ndev) return;
    BL.sel = idx;
    memcpy(BL.sel_addr, BL.dev[idx].addr, 6);
    if (BL.page == BL_PAGE_GATT && page != BL_PAGE_GATT) bl_gatt_close();
    BL.page = page;
    bl_rebuild();
}

void bl_open_detail(int idx) { open_page(idx, BL_PAGE_DETAIL); }
void bl_open_finder(int idx) { open_page(idx, BL_PAGE_FINDER); }
void bl_open_gatt(int idx) { open_page(idx, BL_PAGE_GATT); }

void bl_go_tab(void)
{
    if (BL.page == BL_PAGE_GATT) bl_gatt_close();
    BL.page = BL_PAGE_TAB;
    bl_rebuild();
}

static void tab_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    bl_overlay_close();
    if (BL.page == BL_PAGE_GATT) bl_gatt_close();
    BL.tab = t;
    BL.page = BL_PAGE_TAB;
    bl_settings_save();
    bl_rebuild();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    BL.ticks++;
    bl_scan_drain();
    bl_live_tick();
    if (BL.ticks % 10 == 0) {
        bl_log_tick();
        bl_names_poll();
        /* Bluetooth came on (here or in Settings) or the scan stopped by
         * itself: start it again */
        bool want = !BL.paused && (!BL.hidden || BL.log_csv || BL.mqtt);
        bool was_off = BL.bt_off;
        if (want && !aos_hal_ble_scanning()) bl_scan_apply();
        if (want && was_off != BL.bt_off && !BL.overlay && BL.page == BL_PAGE_TAB && !BL.hidden) bl_rebuild();
    }
    if (BL.hidden || BL.overlay) return;
    if (BL.page == BL_PAGE_FINDER) bl_finder_beep();
    /* the radar sweeps at 10 fps, the finder at 5, the rest twice a second */
    if (BL.page == BL_PAGE_TAB && BL.tab == BL_TAB_RADAR) bl_radar_refresh();
    if (BL.page == BL_PAGE_FINDER && BL.ticks % 2 == 0) {
        if (sel_resolve()) bl_finder_refresh();
    }
    if (BL.page == BL_PAGE_GATT) bl_gatt_refresh();
    if (BL.ticks % 5) return;
    if (s_status) {
        char s[96];
        bl_scan_status(s, sizeof s);
        lv_label_set_text(s_status, s);
    }
    if (BL.page == BL_PAGE_DETAIL) {
        if (sel_resolve()) bl_detail_refresh();
        else bl_go_tab();
        return;
    }
    if (BL.page != BL_PAGE_TAB || BL.bt_off) return;
    if (BL.tab == BL_TAB_LIST) bl_list_refresh();
    else if (BL.tab == BL_TAB_SENS) bl_sens_refresh();
    else if (BL.tab == BL_TAB_AIR) bl_air_refresh();
}

/* -------------------------------------------------------------------------- */
/* Life cycle                                                                  */
/* -------------------------------------------------------------------------- */

void bl_names_first(void);

/* The screen, in whatever size root has now: at creation and at every turn
 * (ble_resize), which keeps the table and the page. */
static void layout(lv_obj_t *root)
{
    BL.root = root;
    BL.overlay = NULL;
    BL.W = lv_obj_get_width(root);
    BL.H = lv_obj_get_height(root);
    BL.land = BL.W > BL.H;
    BL.hidden = false;
    const int32_t tab_h = BL.land ? 96 : 116;
    BL.content = bl_box(root, BL.W, BL.H - tab_h);
    lv_obj_set_style_pad_hor(BL.content, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(BL.content, 8, 0);
    lv_obj_update_layout(BL.content);
    BL.cw = BL.W - 2 * AOS_UI_PAD;
    BL.ch = lv_obj_get_content_height(BL.content);
    BL.tabbar = bl_box(root, BL.W, tab_h);
    lv_obj_set_pos(BL.tabbar, 0, BL.H - tab_h);
    lv_obj_set_style_bg_color(BL.tabbar, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(BL.tabbar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(BL.tabbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(BL.tabbar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < BL_TAB_COUNT; i++) {
        lv_obj_t *t = bl_box(BL.tabbar, BL.W / BL_TAB_COUNT, tab_h);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(aos_label(t, TAB_GLYPH[i], &aos_sym_44, AOS_C_DIM), LV_ALIGN_CENTER, 0, BL.land ? -14 : -16);
        lv_obj_align(aos_label(t, _(TAB_NAME[i]), aos_font_tiny, AOS_C_DIM), LV_ALIGN_CENTER, 0, BL.land ? 26 : 30);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        BL.tabs[i] = t;
    }
    if (BL.page != BL_PAGE_TAB && !sel_resolve()) BL.page = BL_PAGE_TAB;
    bl_rebuild();
}

static void *ble_create(aos_app_t *self, lv_obj_t *root)
{
    memset(&BL, 0, sizeof BL);
    BL.sel = -1;
    BL.self = self;
    if (!bl_scan_init()) {
        lv_obj_center(aos_label(root, _("No hay memoria para abrir BLE"), aos_font_body, AOS_C_DIM));
        return NULL;
    }
    settings_load();
    bl_names_first();
    BL.started_ms = (uint32_t)aos_hal_uptime_ms();
    layout(root);
    bl_scan_apply();
    BL.timer = lv_timer_create(timer_cb, 100, NULL);
    return &BL;
}

static bool ble_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    bl_page_gone();
    if (BL.page == BL_PAGE_GATT) bl_gatt_close();
    if (BL.page == BL_PAGE_GATT) BL.page = BL_PAGE_DETAIL;
    BL.overlay = NULL;              /* under root: cleaned with it */
    lv_obj_clean(root);
    layout(root);
    return true;
}

static void ble_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (BL.timer) lv_timer_delete(BL.timer);
    BL.timer = NULL;
    bl_page_gone();
    BL.overlay = NULL;
    BL.content = NULL;
    bl_gatt_close();
    aos_hal_ble_scan_stop();
    bl_live_clear();
    bl_scan_free();
    BL.page = BL_PAGE_TAB;
}

static void ble_hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    BL.hidden = true;
    bl_overlay_close();
    if (BL.page == BL_PAGE_GATT) {
        bl_gatt_close();
        BL.page = BL_PAGE_DETAIL;
    }
    bl_scan_apply();
}

static void ble_show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    BL.hidden = false;
    bl_scan_apply();
    if (BL.page != BL_PAGE_TAB && !sel_resolve()) BL.page = BL_PAGE_TAB;
    bl_rebuild();
}

static bool ble_back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (!BL.content) return false;
    if (BL.overlay) {
        bl_overlay_close();
        bl_rebuild();
        return true;
    }
    if (BL.page == BL_PAGE_GATT || BL.page == BL_PAGE_FINDER) {
        if (BL.page == BL_PAGE_GATT) bl_gatt_close();
        BL.page = sel_resolve() ? BL_PAGE_DETAIL : BL_PAGE_TAB;
        bl_rebuild();
        return true;
    }
    if (BL.page == BL_PAGE_DETAIL) {
        bl_go_tab();
        return true;
    }
    return false;
}

static bool ble_init(aos_app_t *app)
{
    app->desc.id = "aos.ble";
    app->desc.name = "BLE";
    app->desc.icon = LV_SYMBOL_BLUETOOTH;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a = 0x3B82F6;
    app->desc.color_b = 0x1E3A8A;
    app->desc.order = 172;
    app->desc.flags = AOS_APP_FLAG_BACKGROUND;
    aos_icon_set_ops(app, BLE_ICON, sizeof BLE_ICON);
    app->create = ble_create;
    app->destroy = ble_destroy;
    app->hide = ble_hide;
    app->show = ble_show;
    app->back = ble_back;
    app->resize = ble_resize;
    return true;
}

AOS_APP_ENTRY(ble_init);
