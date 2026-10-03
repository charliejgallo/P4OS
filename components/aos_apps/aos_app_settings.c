/*
 * P4OS - Ajustes.
 *
 * iOS-style: grouped lists with a coloured glyph per row, and pages that
 * push over each other (back, or the left-edge swipe, pops one). Written new
 * for P4OS; the watch's Settings (_pending/aos_app_settings.c) was 3.7k lines
 * of PMU switches, touch calibration and watchfaces that have no place here.
 *
 *   Wi-Fi        switch, the network in use, a scan, joining with a password
 *   Bluetooth    switch
 *   Pantalla     orientation, brightness
 *   Sonido       volume, a test tone
 *   Fondo        the wallpaper gradients
 *   Inicio       edit the home screen
 *   Expansión    the 40-pin header with who holds each pin, and the ports of
 *                modules.txt
 *   Idioma       the language packs
 *   Hora         time zone and the device's network name
 *   Almacenamiento  what fills the card, by kind, the card itself, eject
 *   Actualización   the image running and the one in the other slot, and
 *                   going back to it
 *   Diagnóstico  temperature, CPU and memory now; why this boot happened,
 *                safe mode, the hang watchdog, the last crash's dump
 *   Acerca de    board, firmware, memory, the portal's address (and a QR
 *                for the phone), what the HAL can do
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_io.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(AOS_SIM)
static void *big_calloc(size_t n) { return calloc(1, n); }
#else
#include "esp_heap_caps.h"
static void *big_calloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}
#endif

enum { PG_ROOT = 0, PG_WIFI, PG_DISPLAY, PG_SOUND, PG_WALL, PG_EXP, PG_LANG, PG_TIME, PG_ABOUT, PG_USB,
       PG_STORAGE, PG_UPDATE, PG_DIAG, PG_COUNT };

static const char *const PG_TITLE[PG_COUNT] = { N_("Ajustes"), N_("Wi-Fi"), N_("Pantalla"), N_("Sonido"), N_("Fondo"),
                                                N_("Expansión"), N_("Idioma"), N_("Fecha y hora"), N_("Acerca de"),
                                                "USB", N_("Almacenamiento"), N_("Actualización"), N_("Diagnóstico") };

static struct {
    lv_obj_t *root, *page, *kb, *ta, *overlay;
    int32_t W, H;
    bool land;
    int stack[8], depth;
    /* wifi scan, filled by a thread, read by a timer */
    aos_wifi_ap_t aps[20];
    volatile int naps;              /* -3 not yet, -2 scanning, -1 failed, >=0 found */
    char join_ssid[33];
    lv_timer_t *timer;
    lv_obj_t *wifi_list, *wifi_state, *side;
    /* USB page, refreshed by the timer: the port can change by itself (an
     * eject ends disk mode) */
    lv_obj_t *usb_state, *usb_dot, *usb_tick[4], *usb_root;
    int usb_shown;
    int32_t PW;                     /* width of the column being filled */
    int selected;                   /* landscape: the page on the right */
    /* Storage: the count it shows; Diagnostics: the live values */
    int st_shown;
    int ap_shown;                   /* Wi-Fi: the access point's state the page shows */
    lv_obj_t *dg_temp, *dg_cpu, *dg_int, *dg_psram, *dg_up;
    /* a row that asks for a second tap before it acts */
    lv_obj_t *armed;
    uint32_t armed_ms;
} U;

static void show(int page);
static void open_cb(lv_event_t *e);
static lv_obj_t *qr_strip(lv_obj_t *p);
static void qr_box(lv_obj_t *strip, const char *text, const char *caption);
static lv_obj_t *action_row(lv_obj_t *g, const char *label, lv_color_t color, lv_event_cb_t cb);
static bool second_tap(lv_event_t *e, const char *question);
static aos_lang_t s_langs[AOS_LANG_MAX];

/* -------------------------------------------------------------------------- */
/* Rows                                                                        */
/* -------------------------------------------------------------------------- */

static lv_obj_t *group(lv_obj_t *parent, const char *title)
{
    if (title) {
        lv_obj_t *t = aos_label(parent, title, aos_font_caption, AOS_C_DIM);
        lv_obj_set_style_pad_left(t, 24, 0);
        lv_obj_set_style_pad_top(t, 10, 0);
    }
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g, AOS_UI_RADIUS, 0);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_clip_corner(g, false, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    return g;
}

/* The value takes what the label leaves: in landscape the list is a 450 px
 * column, and "Keyboard and mouse" ran over "USB". LVGL sizes the row
 * before its children, so this runs when either changes size, and works from
 * widths and the alignment offsets rather than coordinates, which a child
 * gets only after its size. A value updated later (the USB mode) is cut the
 * same way. */
static void row_fit(lv_obj_t *r, lv_obj_t *l)
{
    lv_obj_t *v = lv_obj_get_user_data(r);
    if (!v) return;
    int32_t left = lv_obj_get_x_aligned(l) + lv_obj_get_width(l) + 16;
    int32_t w = lv_obj_get_content_width(r) + lv_obj_get_x_aligned(v) - left;
    lv_obj_set_width(v, w > 0 ? w : 0);
}
static void row_fit_cb(lv_event_t *e) { row_fit(lv_event_get_target(e), lv_event_get_user_data(e)); }
static void label_fit_cb(lv_event_t *e) { row_fit(lv_event_get_user_data(e), lv_event_get_target(e)); }

/* A row: optional glyph in a coloured square, the label, a value on the
 * right and a chevron when it opens something. */
static lv_obj_t *row(lv_obj_t *g, const char *glyph, uint32_t color, const char *label,
                     const char *value, bool chevron, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *r = lv_obj_create(g);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), AOS_UI_ROW_H);
    lv_obj_set_style_pad_hor(r, 22, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (lv_obj_get_child_count(g) > 1) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
    }
    bool sel = cb == open_cb && (int)(intptr_t)ud == U.selected;
    if (sel) {
        lv_obj_set_style_bg_color(r, AOS_C_ACCENT, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, AOS_UI_RADIUS, 0);
        lv_obj_set_style_border_width(r, 0, 0);
    }
    if (cb) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    }
    int32_t x = 0;
    if (glyph) {
        lv_obj_t *sq = lv_obj_create(r);
        lv_obj_remove_style_all(sq);
        lv_obj_set_size(sq, 56, 56);
        lv_obj_set_style_radius(sq, 14, 0);
        lv_obj_set_style_bg_color(sq, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
        lv_obj_align(sq, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *gl = lv_label_create(sq);
        lv_obj_set_style_text_font(gl, &aos_sym_28, 0);
        lv_obj_set_style_text_color(gl, lv_color_white(), 0);
        lv_label_set_text(gl, glyph);
        lv_obj_center(gl);
        aos_make_decorative(sq);
        x = 76;
    }
    lv_obj_t *l = aos_label(r, label, aos_font_body, sel ? lv_color_white() : AOS_C_TEXT);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, x, 0);
    if (chevron) {
        lv_obj_t *c = lv_label_create(r);
        lv_obj_set_style_text_font(c, &aos_sym_28, 0);
        lv_obj_set_style_text_color(c, sel ? lv_color_white() : AOS_C_DIM, 0);
        lv_label_set_text(c, AOS_SYM_CHEVRON_RIGHT);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    if (value) {
        /* the tick of the chosen option is a glyph: Inter has no U+2713 */
        bool tick = !strcmp(value, AOS_SYM_CHECK);
        lv_obj_t *v = aos_label(r, value, tick ? &aos_sym_28 : aos_font_body, tick ? AOS_C_ACCENT : sel ? lv_color_white() : AOS_C_DIM);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, chevron ? -40 : 0, 0);
        lv_obj_set_user_data(r, v);
        if (!tick) {
            lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_height(v, lv_font_get_line_height(aos_font_body));    /* one line: dots, not a wrap */
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_add_event_cb(r, row_fit_cb, LV_EVENT_SIZE_CHANGED, l);
            lv_obj_add_event_cb(l, label_fit_cb, LV_EVENT_SIZE_CHANGED, r);
        }
    }
    return r;
}

static lv_obj_t *row_switch(lv_obj_t *g, const char *glyph, uint32_t color, const char *label, bool on, lv_event_cb_t cb)
{
    lv_obj_t *r = row(g, glyph, color, label, NULL, false, NULL, NULL);
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 100, 56);
    lv_obj_set_style_bg_color(sw, AOS_C_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_state(sw, LV_STATE_CHECKED, on);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return r;
}

static lv_obj_t *row_slider(lv_obj_t *g, const char *glyph, int value, lv_event_cb_t cb)
{
    lv_obj_t *r = row(g, NULL, 0, "", NULL, false, NULL, NULL);
    lv_obj_t *gl = lv_label_create(r);
    lv_obj_set_style_text_font(gl, &aos_sym_44, 0);
    lv_obj_set_style_text_color(gl, AOS_C_DIM, 0);
    lv_label_set_text(gl, glyph);
    lv_obj_align(gl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *s = lv_slider_create(r);
    lv_obj_set_size(s, lv_pct(78), 16);
    lv_slider_set_range(s, 1, 100);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, AOS_C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s, 12, LV_PART_KNOB);
    lv_obj_align(s, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return r;
}

static void note(lv_obj_t *p, const char *text)
{
    lv_obj_t *l = aos_label(p, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_hor(l, 24, 0);
}

/* -------------------------------------------------------------------------- */
/* Pages                                                                       */
/* -------------------------------------------------------------------------- */

static void open_cb(lv_event_t *e) { show((int)(intptr_t)lv_event_get_user_data(e)); }
static void bt_cb(lv_event_t *e) { aos_hal_bt_enable(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)); }
static void wifi_sw_cb(lv_event_t *e) { aos_hal_net_enable(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)); }
static void edit_home_cb(lv_event_t *e) { aos_ui_edit_home(); }

/* ---- USB ---- */

/* The rows' order IS aos_hal_usb_mode_t's: CONSOLE, KEYS, DISK, HOST. */
static const char *usb_name(int mode)
{
    switch (mode) {
    case AOS_HAL_USB_KEYS: return _("Teclado y mouse");   /* and gamepad, MIDI, network: the name stays short */
    case AOS_HAL_USB_DISK: return _("Disco");
    case AOS_HAL_USB_HOST: return "Host";
    default:               return _("Apagado");
    }
}

/* What the port is doing now, in a sentence, and the colour of its dot. */
static const char *usb_state_text(uint32_t *color)
{
    *color = 0x8E8E93;
    if (aos_hal_usb_busy()) return _("Cambiando de modo…");
    bool pc = aos_hal_usb_connected();
    switch (aos_hal_usb_mode()) {
    case AOS_HAL_USB_KEYS:
        if (pc) {
            *color = 0x34C759;
            return aos_hal_usb_net_up() ? _("La computadora tomó la placa como teclado y mouse. El portal también contesta por el cable: 192.168.7.1")
                                        : _("La computadora tomó la placa como teclado y mouse.");
        }
        *color = 0xFF9F0A;
        return _("Esperando a la computadora. Conectá el cable al conector OTG.");
    case AOS_HAL_USB_DISK:
        if (pc) {
            *color = 0x34C759;
            return _("La computadora tiene la tarjeta. Cuando termines, expulsala ahí: vuelve sola a la placa.");
        }
        *color = 0xFF9F0A;
        return _("Esperando a la computadora. La tarjeta ya salió de la placa: las apps de la tarjeta no abren.");
    default:
        return _("El puerto está libre: la placa no aparece en la computadora.");
    }
}

static void usb_refresh(void)
{
    int mode = aos_hal_usb_mode();
    int key = (mode << 4) | (aos_hal_usb_busy() << 3) | (aos_hal_usb_connected() << 2);
    if (key == U.usb_shown) return;
    U.usb_shown = key;
    if (U.usb_root) lv_label_set_text(U.usb_root, usb_name(mode));
    if (!U.usb_state) return;
    uint32_t c;
    lv_label_set_text(U.usb_state, usb_state_text(&c));
    lv_obj_set_style_bg_color(U.usb_dot, lv_color_hex(c), 0);
    for (int i = 0; i < 4; i++)
        if (U.usb_tick[i]) lv_obj_set_flag(U.usb_tick[i], LV_OBJ_FLAG_HIDDEN, i != mode);
}

static void usb_mode_cb(lv_event_t *e)
{
    int want = (int)(intptr_t)lv_event_get_user_data(e);
    if (want == AOS_HAL_USB_HOST) {
        aos_ui_toast(_("Esta placa no da 5 V por el OTG: no puede alimentar un pendrive"), 2500);
        return;
    }
    if (want == (int)aos_hal_usb_mode()) return;
    if (aos_hal_usb_busy()) { aos_ui_toast(_("El USB está cambiando de modo"), 1500); return; }
    if (aos_hal_usb_mode() == AOS_HAL_USB_DISK && aos_hal_usb_connected())
        aos_ui_toast(_("Mejor expulsar la tarjeta en la computadora antes"), 2500);
    if (want == AOS_HAL_USB_DISK) {
        if (!aos_hal_sd_present()) { aos_ui_toast(_("No hay tarjeta"), 1500); return; }
        /* nothing on the board may have a file open on the card */
        aos_ui_close_others();
    }
    if (!aos_hal_usb_mode_set((aos_hal_usb_mode_t)want)) aos_ui_toast(_("El USB está cambiando de modo"), 1500);
    U.usb_shown = -1;
    usb_refresh();
}

/* A mode: glyph, name, a paragraph of what happens in it, and the tick. */
static void mode_row(lv_obj_t *g, int mode, const char *glyph, uint32_t color, const char *desc, bool enabled)
{
    lv_obj_t *r = lv_obj_create(g);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(r, 22, 0);
    lv_obj_set_style_pad_ver(r, 20, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (lv_obj_get_child_count(g) > 1) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
    }
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, usb_mode_cb, LV_EVENT_CLICKED, (void *)(intptr_t)mode);
    lv_obj_t *sq = lv_obj_create(r);
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, 56, 56);
    lv_obj_set_style_radius(sq, 14, 0);
    lv_obj_set_style_bg_color(sq, lv_color_hex(enabled ? color : 0x48484A), 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_t *gl = lv_label_create(sq);
    lv_obj_set_style_text_font(gl, &aos_sym_28, 0);
    lv_obj_set_style_text_color(gl, lv_color_white(), 0);
    lv_label_set_text(gl, glyph);
    lv_obj_center(gl);
    aos_make_decorative(sq);
    lv_obj_t *t = aos_label(r, usb_name(mode), aos_font_body, enabled ? AOS_C_TEXT : AOS_C_DIM);
    lv_obj_set_pos(t, 76, 12);
    lv_obj_t *tick = lv_label_create(r);
    lv_obj_set_style_text_font(tick, &aos_sym_28, 0);
    lv_obj_set_style_text_color(tick, AOS_C_ACCENT, 0);
    lv_label_set_text(tick, AOS_SYM_CHECK);
    lv_obj_align(tick, LV_ALIGN_TOP_RIGHT, 0, 12);
    U.usb_tick[mode] = tick;
    lv_obj_t *d = aos_label(r, desc, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(d, lv_pct(100));
    lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_left(d, 76, 0);
    lv_obj_set_style_pad_right(d, 20, 0);
    lv_obj_set_pos(d, 0, 68);
}

static void build_usb(lv_obj_t *p)
{
    lv_obj_t *g = group(p, _("AHORA"));
    lv_obj_t *r = lv_obj_create(g);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(r, 22, 0);
    lv_obj_set_style_pad_ver(r, 22, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    U.usb_dot = lv_obj_create(r);
    lv_obj_remove_style_all(U.usb_dot);
    lv_obj_set_size(U.usb_dot, 18, 18);
    lv_obj_set_style_radius(U.usb_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.usb_dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(U.usb_dot, 0, 8);
    U.usb_state = aos_label(r, "", aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(U.usb_state, lv_pct(100));
    lv_label_set_long_mode(U.usb_state, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_left(U.usb_state, 36, 0);

    g = group(p, _("QUÉ ES EL PUERTO OTG"));
    mode_row(g, AOS_HAL_USB_CONSOLE, AOS_SYM_POWER_PLUG_OFF, 0x8E8E93,
             _("El puerto queda libre y la placa no aparece en la computadora. La consola sigue por el conector UART, y el registro en el portal."),
             true);
    mode_row(g, AOS_HAL_USB_KEYS, AOS_SYM_KEYBOARD, 0x0A84FF,
             _("La placa es un teclado, un mouse, las teclas multimedia, un joystick y un teclado MIDI de la computadora: lo que usa el Macro pad. Además, una red por el cable: el portal contesta en 192.168.7.1 aunque no haya Wi-Fi. La tarjeta sigue en la placa. Abrir el Macro pad lo elige solo."),
             true);
    mode_row(g, AOS_HAL_USB_DISK, AOS_SYM_SD, 0xFF9F0A,
             _("La microSD aparece en la computadora como un pendrive, para copiar apps, música, fotos o mapas a la velocidad del USB. Mientras la tiene la computadora la placa no la toca: al elegirlo se cierran las otras apps, y las de la tarjeta no abren. Expulsala en la computadora y vuelve sola a la placa, con el modo de antes."),
             true);
    mode_row(g, AOS_HAL_USB_HOST, AOS_SYM_USB_PORT, 0x5E5CE6,
             _("Conectarle a la placa un pendrive, un teclado o un mouse. No en esta placa: el conector OTG no da los 5 V que necesitan. Haría falta un hub con fuente."),
             false);
    note(p, _("El conector OTG es el USB 2.0 de alta velocidad del P4 (480 Mbit/s). La placa puede alimentarse por él o por el UART, y pasar de uno al otro sin reiniciarse."));
    U.usb_shown = -1;
    usb_refresh();
}

static void build_root(lv_obj_t *p)
{
    char buf[48];
    lv_obj_t *g = group(p, NULL);
    snprintf(buf, sizeof buf, "%s", aos_hal_net_state() == AOS_NET_CONNECTED ? aos_hal_net_ssid()
                                    : aos_hal_net_enabled() ? _("Sin conectar") : _("No"));
    row(g, AOS_SYM_WIFI, 0x0A84FF, _("Wi-Fi"), buf, true, open_cb, (void *)PG_WIFI);
    row_switch(g, AOS_SYM_BLUETOOTH, 0x0A84FF, _("Bluetooth"), aos_hal_bt_enabled(), bt_cb);
    lv_obj_t *u = row(g, AOS_SYM_USB, 0x636366, "USB", usb_name((int)aos_hal_usb_mode()), true, open_cb, (void *)PG_USB);
    U.usb_root = lv_obj_get_user_data(u);

    g = group(p, NULL);
    row(g, AOS_SYM_BRIGHTNESS_6, 0x5E5CE6, _("Pantalla"), aos_ui_landscape() ? _("Horizontal") : _("Vertical"), true,
        open_cb, (void *)PG_DISPLAY);
    row(g, AOS_SYM_VOLUME_HIGH, 0xFF375F, _("Sonido"), NULL, true, open_cb, (void *)PG_SOUND);
    row(g, AOS_SYM_PALETTE, 0x30B0C7, _("Fondo de pantalla"), NULL, true, open_cb, (void *)PG_WALL);
    row(g, AOS_SYM_VIEW_GRID_OUTLINE, 0xFF9F0A, _("Editar inicio"), NULL, true, edit_home_cb, NULL);

    g = group(p, NULL);
    row(g, AOS_SYM_EXPANSION_CARD, 0x8E8E93, _("Expansión"), NULL, true, open_cb, (void *)PG_EXP);
    row(g, AOS_SYM_SD, 0x8E8E93, _("Almacenamiento"), aos_hal_sd_present() ? NULL : _("sin tarjeta"), true, open_cb,
        (void *)PG_STORAGE);

    g = group(p, NULL);
    const char *lname = aos_i18n_current();
    int nl = aos_i18n_scan(s_langs, AOS_LANG_MAX);
    for (int i = 0; i < nl; i++)
        if (!strcmp(s_langs[i].code, lname)) lname = s_langs[i].name;
    row(g, AOS_SYM_TRANSLATE, 0x34C759, _("Idioma"), lname, true, open_cb, (void *)PG_LANG);
    row(g, AOS_SYM_CLOCK_OUTLINE, 0x636366, _("Fecha y hora"), NULL, true, open_cb, (void *)PG_TIME);

    g = group(p, NULL);
    row(g, AOS_SYM_DOWNLOAD, 0x34C759, _("Actualización"), aos_hal_firmware_version(), true, open_cb, (void *)PG_UPDATE);
    row(g, AOS_SYM_PULSE, 0xFF453A, _("Diagnóstico"),
        aos_ui_safe_mode() ? _("modo seguro") : aos_hal_hang_restarts() ? _("hubo un cuelgue") : NULL, true, open_cb,
        (void *)PG_DIAG);
    row(g, AOS_SYM_INFORMATION_OUTLINE, 0x636366, _("Acerca de"), NULL, true, open_cb, (void *)PG_ABOUT);
}

/* ---- Wi-Fi ---- */

static void scan_thread(void *arg)
{
    int n = aos_hal_net_scan(U.aps, 20);
    U.naps = n < 0 ? -1 : n;
}

static void scan_cb(lv_event_t *e)
{
    if (U.naps == -2) return;
    U.naps = -2;
    if (!aos_hal_thread_start("wifiscan", scan_thread, NULL, 6144, 3)) U.naps = -1;
}

static void kb_close(void)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = NULL;
    U.kb = U.ta = NULL;
}

/* A sheet with a title, one line of text and the keyboard. 'done' gets the
 * text when the keyboard's OK is pressed; the sheet closes either way. */
static void (*s_kb_done)(const char *text);

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) {
        if (s_kb_done) s_kb_done(lv_textarea_get_text(U.ta));
        kb_close();
    } else if (c == LV_EVENT_CANCEL) {
        kb_close();
    }
}

static void kb_cancel_cb(lv_event_t *e) { kb_close(); }

static void kb_ok_cb(lv_event_t *e)
{
    if (s_kb_done) s_kb_done(lv_textarea_get_text(U.ta));
    kb_close();
}

static void kb_open(const char *title, const char *text, int max_len, const char *ok, void (*done)(const char *))
{
    kb_close();
    s_kb_done = done;
    U.overlay = lv_obj_create(U.root);
    lv_obj_remove_style_all(U.overlay);
    lv_obj_set_size(U.overlay, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(U.overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Cancel on the left, the action on the right, like the phone's sheets */
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_obj_create(U.overlay);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_SIZE_CONTENT, 72);
        lv_obj_set_style_pad_hor(b, AOS_UI_PAD, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, i ? kb_ok_cb : kb_cancel_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = aos_label(b, i ? ok : _("Cancelar"), aos_font_body, AOS_C_ACCENT);
        lv_obj_center(l);
        lv_obj_align(b, i ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0, 0);
    }
    lv_obj_t *t = aos_label(U.overlay, title, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(t, U.W - 2 * AOS_UI_PAD);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 90);
    U.ta = lv_textarea_create(U.overlay);
    lv_textarea_set_one_line(U.ta, true);
    if (max_len) lv_textarea_set_max_length(U.ta, max_len);
    lv_textarea_set_text(U.ta, text ? text : "");
    lv_obj_set_size(U.ta, U.W - 2 * AOS_UI_PAD, 88);
    lv_obj_align_to(U.ta, t, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 24);
    lv_obj_set_style_text_font(U.ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(U.ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(U.ta, 0, 0);
    lv_obj_set_style_radius(U.ta, 20, 0);
    lv_obj_set_style_pad_hor(U.ta, 24, 0);
    lv_obj_set_style_pad_ver(U.ta, 22, 0);
    lv_obj_set_style_bg_color(U.ta, AOS_C_ACCENT, LV_PART_CURSOR);
    lv_obj_set_style_border_color(U.ta, AOS_C_ACCENT, LV_PART_CURSOR);
    U.kb = lv_keyboard_create(U.overlay);
    lv_obj_set_size(U.kb, U.W, U.land ? U.H / 2 : U.H * 2 / 5);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.kb, aos_font_body);
    lv_keyboard_set_textarea(U.kb, U.ta);
    lv_obj_add_event_cb(U.kb, kb_event_cb, LV_EVENT_ALL, NULL);
}

static void join_done(const char *pass)
{
    if (aos_hal_net_set_credentials(U.join_ssid, pass)) aos_ui_toast(_("Conectando…"), 1500);
    else aos_ui_toast(_("La contraseña WPA2 lleva de 8 a 63 caracteres"), 2500);
}

static void join_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    snprintf(U.join_ssid, sizeof U.join_ssid, "%s", U.aps[i].ssid);
    if (!U.aps[i].secure) { join_done(""); return; }
    char t[64];
    snprintf(t, sizeof t, "%s \"%s\"", _("Contraseña de"), U.join_ssid);
    kb_open(t, "", 63, _("Unirse"), join_done);
}

static void forget_cb(lv_event_t *e) { aos_hal_net_forget(); show(PG_WIFI); }

static void wifi_list_fill(void)
{
    if (!U.wifi_list) return;
    lv_obj_clean(U.wifi_list);
    if (U.naps == -3) return;
    if (U.naps == -2) { row(U.wifi_list, NULL, 0, _("Buscando…"), NULL, false, NULL, NULL); return; }
    if (U.naps == -1) { row(U.wifi_list, NULL, 0, _("La búsqueda falló"), NULL, false, NULL, NULL); return; }
    for (int i = 0; i < U.naps; i++) {
        char v[24];
        snprintf(v, sizeof v, "%d dBm", U.aps[i].rssi);
        row(U.wifi_list, U.aps[i].secure ? AOS_SYM_LOCK : AOS_SYM_WIFI, 0x3A3A3C, U.aps[i].ssid, v, false, join_cb,
            (void *)(intptr_t)i);
    }
    if (!U.naps) row(U.wifi_list, NULL, 0, _("No se encontraron redes"), NULL, false, NULL, NULL);
}

/* The board's own network (aos_net_p4.c, "The access point"): for where
 * there is no network to join. Two codes: one joins the phone to it, the
 * other opens the portal once it is on. */
static int ap_key(void)
{
    int32_t want = 0;
    aos_hal_pref_get_i32("ap_on", &want);
    return (aos_hal_net_ap_active() ? 2 : 0) | (want ? 1 : 0) | aos_hal_net_ap_clients() << 2;
}

static void ap_sw_cb(lv_event_t *e)
{
    if (lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)) {
        if (!aos_hal_net_ap_start()) aos_ui_toast(_("La radio está ocupada: probá de nuevo en un momento"), 2000);
    } else {
        aos_hal_net_ap_stop();
    }
}

static void ap_newpass_cb(lv_event_t *e)
{
    if (!second_tap(e, _("Tocá otra vez: los conectados se caen"))) return;
    char ssid[33] = "";
    aos_hal_pref_get_str("ap_ssid", ssid, sizeof ssid);
    aos_hal_net_ap_set_config(ssid, "", AOS_AP_PASS_FIXED);
    show(PG_WIFI);
}

/* A Wi-Fi QR's fields escape \ ; , : and the quote with a backslash. */
static void qr_escape(char *out, size_t n, const char *in)
{
    size_t k = 0;
    for (; *in && k + 2 < n; in++) {
        if (strchr("\\;,:\"", *in)) out[k++] = '\\';
        out[k++] = *in;
    }
    out[k] = 0;
}

static void build_ap(lv_obj_t *p)
{
    U.ap_shown = ap_key();
    lv_obj_t *g = group(p, _("RED PROPIA"));
    int32_t want = 0;
    aos_hal_pref_get_i32("ap_on", &want);
    row_switch(g, AOS_SYM_ACCESS_POINT, 0xFF9F0A, _("Punto de acceso"), want != 0, ap_sw_cb);
    if (!aos_hal_net_ap_active()) {
        if (want) row(g, NULL, 0, _("Levantándolo…"), NULL, false, NULL, NULL);
        note(p, _("La placa arma su propia red Wi-Fi, para conectarte desde el teléfono donde no hay otra. Queda prendida al reiniciar."));
        return;
    }
    const char *ssid = aos_hal_net_ap_ssid(), *pass = aos_hal_net_ap_pass();
    row(g, NULL, 0, _("Nombre"), ssid, false, NULL, NULL);
    row(g, NULL, 0, _("Contraseña"), pass, false, NULL, NULL);
    char v[48];
    snprintf(v, sizeof v, "http://%s", aos_hal_net_ap_ip());
    row(g, NULL, 0, _("Portal"), v, false, NULL, NULL);
    snprintf(v, sizeof v, "%d", aos_hal_net_ap_clients());
    row(g, NULL, 0, _("Conectados"), v, false, NULL, NULL);
    action_row(g, _("Cambiar la contraseña"), AOS_C_ACCENT, ap_newpass_cb);
    char es[70], ep[130], join[220], url[40];
    qr_escape(es, sizeof es, ssid);
    qr_escape(ep, sizeof ep, pass);
    snprintf(join, sizeof join, "WIFI:T:WPA;S:%s;P:%s;;", es, ep);
    snprintf(url, sizeof url, "http://%s/", aos_hal_net_ap_ip());
    lv_obj_t *box = qr_strip(p);
    qr_box(box, join, _("1. Unite a la red de la placa con la cámara del teléfono."));
    qr_box(box, url, _("2. Después abrí el portal."));
    note(p, _("Mientras está prendida, la placa deja de buscar la red de casa (si ya estaba conectada, sigue). El teléfono puede avisar que esta red no tiene internet: es así, quedate conectado igual."));
}

static void build_wifi(lv_obj_t *p)
{
    lv_obj_t *g = group(p, NULL);
    row_switch(g, AOS_SYM_WIFI, 0x0A84FF, _("Wi-Fi"), aos_hal_net_enabled(), wifi_sw_cb);
    bool conn = aos_hal_net_state() == AOS_NET_CONNECTED;
    char v[48];
    snprintf(v, sizeof v, "%s", conn ? aos_hal_net_ssid() : _("Sin conectar"));
    U.wifi_state = row(g, NULL, 0, _("Red"), v, false, NULL, NULL);
    if (conn) {
        row(g, NULL, 0, _("Dirección"), aos_hal_net_ip(), false, NULL, NULL);
        snprintf(v, sizeof v, "%d dBm", aos_hal_net_rssi());
        row(g, NULL, 0, _("Señal"), v, false, NULL, NULL);
    }
    if (aos_hal_net_has_credentials()) {
        lv_obj_t *r = row(g, NULL, 0, _("Olvidar esta red"), NULL, false, forget_cb, NULL);
        lv_obj_set_style_text_color(lv_obj_get_child(r, 0), AOS_C_RED, 0);
    }
    build_ap(p);
    U.wifi_list = group(p, _("REDES"));
    wifi_list_fill();
    lv_obj_t *g2 = group(p, NULL);
    row(g2, AOS_SYM_MAGNIFY, 0x0A84FF, _("Buscar de nuevo"), NULL, false, scan_cb, NULL);
    /* like the phone: opening the page looks around once */
    if (U.naps == -3 && aos_hal_net_enabled()) scan_cb(NULL);
    note(p, _("La P4 se conecta por el ESP32-C6 de la placa: sólo 2,4 GHz."));
}

/* ---- Display, sound, wallpaper ---- */

static void orient_cb(lv_event_t *e) { aos_ui_request_landscape((int)(intptr_t)lv_event_get_user_data(e)); }
static void bright_cb(lv_event_t *e) { aos_hal_brightness_set(lv_slider_get_value(lv_event_get_target(e))); }
static void vol_cb(lv_event_t *e) { aos_hal_volume_set(lv_slider_get_value(lv_event_get_target(e))); }
static void beep_cb(lv_event_t *e) { aos_hal_beep(880, 250); }
static void off_cb(lv_event_t *e)
{
    aos_hal_screen_timeouts_set((uint32_t)(uintptr_t)lv_event_get_user_data(e), 0);
    show(PG_DISPLAY);
}

static void build_display(lv_obj_t *p)
{
    lv_obj_t *g = group(p, _("ORIENTACIÓN"));
    row(g, AOS_SYM_PHONE_ROTATE_PORTRAIT, 0x5E5CE6, _("Vertical"), aos_ui_landscape() ? NULL : AOS_SYM_CHECK, false, orient_cb, (void *)0);
    row(g, AOS_SYM_PHONE_ROTATE_LANDSCAPE, 0x5E5CE6, _("Horizontal"), aos_ui_landscape() ? AOS_SYM_CHECK : NULL, false, orient_cb, (void *)1);
    note(p, _("La placa no tiene acelerómetro: la orientación se elige acá o en el Centro de control. Las apps que sólo tienen sentido de una forma giran la pantalla mientras están abiertas."));
    g = group(p, _("BRILLO"));
    row_slider(g, AOS_SYM_BRIGHTNESS_6, aos_hal_brightness_get(), bright_cb);
    uint32_t off = 0;
    aos_hal_screen_timeouts_get(&off, NULL);
    g = group(p, _("APAGAR LA PANTALLA"));
    static const uint32_t OFF_S[] = { 0, 60, 120, 300, 600, 1800 };
    static const char *const OFF_LBL[] = { N_("Nunca"), N_("1 minuto"), N_("2 minutos"), N_("5 minutos"),
                                           N_("10 minutos"), N_("30 minutos") };
    for (size_t i = 0; i < sizeof OFF_S / sizeof OFF_S[0]; i++)
        row(g, AOS_SYM_TIMER_OUTLINE, 0x8E8E93, _(OFF_LBL[i]), off == OFF_S[i] ? AOS_SYM_CHECK : NULL, false, off_cb,
            (void *)(uintptr_t)OFF_S[i]);
    note(p, _("Sin tocarla, la pantalla se atenúa diez segundos antes y después se apaga; un toque la despierta. Los juegos, el video y las apps que lo piden la mantienen encendida mientras están al frente."));
}

static void build_sound(lv_obj_t *p)
{
    lv_obj_t *g = group(p, _("VOLUMEN"));
    row_slider(g, AOS_SYM_VOLUME_HIGH, aos_hal_volume_get(), vol_cb);
    g = group(p, NULL);
    row(g, AOS_SYM_MUSIC, 0xFF375F, _("Probar el parlante"), NULL, false, beep_cb, NULL);
}

static void wall_cb(lv_event_t *e) { aos_ui_set_wallpaper((int)(intptr_t)lv_event_get_user_data(e)); show(PG_WALL); }

static void build_wall(lv_obj_t *p)
{
    lv_obj_t *grid = lv_obj_create(p);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 20, 0);
    int cols = U.PW > 900 ? 8 : 4;
    int32_t w = (U.PW - 2 * AOS_UI_PAD - (cols - 1) * 20) / cols;
    for (int i = 0; i < AOS_UI_WALLPAPERS; i++) {
        uint32_t a, b;
        aos_ui_wallpaper_colors(i, &a, &b);
        lv_obj_t *t = lv_obj_create(grid);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, w, w * 16 / 9);
        lv_obj_set_style_radius(t, 20, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(a), 0);
        lv_obj_set_style_bg_grad_color(t, lv_color_hex(b), 0);
        lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_color(t, lv_color_white(), 0);
        lv_obj_set_style_border_width(t, i == aos_ui_wallpaper() ? 5 : 1, 0);
        lv_obj_set_style_border_opa(t, i == aos_ui_wallpaper() ? LV_OPA_COVER : LV_OPA_20, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, wall_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    note(p, _("Pronto: una imagen JPEG de la tarjeta."));
}

/* ---- Expansion ---- */

/* Which port of modules.txt a GPIO belongs to, as "i2c.ext SDA". */
static bool port_of(int gpio, char *out, size_t n)
{
    static const char *const ROLE[][4] = {
        [AOS_PORT_UART] = { "TX", "RX", "DE", "" }, [AOS_PORT_I2C] = { "SDA", "SCL", "", "" },
        [AOS_PORT_SPI] = { "SCK", "MOSI", "MISO", "CS" }, [AOS_PORT_GPIO] = { "", "", "", "" },
    };
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *pt = aos_io_port_at(i);
        for (int k = 0; k < 4; k++)
            if (pt->pins[k] == gpio) {
                snprintf(out, n, "%s %s", pt->name, pt->kind <= AOS_PORT_GPIO ? ROLE[pt->kind][k] : "");
                return true;
            }
    }
    return false;
}

static void legend(lv_obj_t *p, const uint32_t *col, const char *const *name, int n)
{
    lv_obj_t *box = lv_obj_create(p);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(box, 14, 0);
    lv_obj_set_style_pad_hor(box, 8, 0);
    for (int i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_create(box);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, LV_SIZE_CONTENT, 36);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(c, 8, 0);
        lv_obj_t *sq = lv_obj_create(c);
        lv_obj_remove_style_all(sq);
        lv_obj_set_size(sq, 22, 22);
        lv_obj_set_style_radius(sq, 6, 0);
        lv_obj_set_style_bg_color(sq, lv_color_hex(col[i]), 0);
        lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
        aos_label(c, aos_tr(name[i]), aos_font_caption, AOS_C_DIM);
    }
}

enum { PC_FREE, PC_PORT, PC_TAKEN, PC_CARE, PC_BOARD, PC_5V, PC_3V3, PC_GND, PC_N };
static const uint32_t PIN_COL[PC_N] = { 0x14532D, 0x155E75, 0x1E3A8A, 0x4A3A10, 0x4B1D1D, 0x7F1D1D, 0x7C2D12, 0x2C2C2E };
static const char *const PIN_NAME[PC_N] = { N_("libre"), N_("puerto"), N_("en uso"), N_("con cuidado"),
                                            N_("de la placa"), "5 V", "3V3", "GND" };

static void build_exp(lv_obj_t *p)
{
    note(p, _("El conector de 40 pines de atrás. Cada pin dice qué puerto de modules.txt lo usa, o quién lo tiene tomado ahora."));
    legend(p, PIN_COL, PIN_NAME, PC_N);
    const aos_io_pin_t *hdr = aos_io_header();
    lv_obj_t *grid = lv_obj_create(p);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 8, 0);
    int cols = U.PW > 900 ? 4 : 2;
    int32_t cw = (U.PW - 2 * AOS_UI_PAD - (cols - 1) * 8) / cols;
    for (int i = 0; i < 40; i++) {
        /* each pair as on the board: the even pin on the left, the odd one
         * (5 V's column) on the right */
        const aos_io_pin_t *pin = &hdr[i ^ 1];
        const char *owner = pin->gpio >= 0 ? aos_io_owner(pin->gpio) : NULL;
        char port[40];
        bool in_port = pin->gpio >= 0 && port_of(pin->gpio, port, sizeof port);
        int k = pin->gpio < 0 ? (pin->label[0] == '5' ? PC_5V : pin->label[0] == '3' ? PC_3V3 : PC_GND)
              : owner ? PC_TAKEN
              : (pin->flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD)) ? PC_BOARD
              : in_port ? PC_PORT
              : (pin->flags & (AOS_PIN_STRAPPING | AOS_PIN_VO4 | AOS_PIN_USB_JTAG)) ? PC_CARE
              : PC_FREE;
        const char *desc = owner ? owner : in_port ? port : pin->note;
        lv_obj_t *c = lv_obj_create(grid);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, cw, 88);
        lv_obj_set_style_radius(c, 16, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(PIN_COL[k]), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(c, 16, 0);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *n = lv_label_create(c);
        lv_obj_set_style_text_font(n, aos_font_small, 0);
        lv_obj_set_style_text_color(n, AOS_C_TEXT, 0);
        lv_label_set_text_fmt(n, "%d  %s", pin->pin, pin->label);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 10);
        lv_obj_t *d = aos_label(c, desc ? desc : "", aos_font_tiny, lv_color_hex(0xC8CDD6));
        lv_obj_set_size(d, cw - 32, lv_font_get_line_height(aos_font_tiny));
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(d, LV_ALIGN_BOTTOM_LEFT, 0, -10);
    }
    lv_obj_t *g = group(p, _("PUERTOS (modules.txt)"));
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *pt = aos_io_port_at(i);
        char v[64];
        switch (pt->kind) {
        case AOS_PORT_UART:
            if (pt->pins[2] >= 0) snprintf(v, sizeof v, "TX %d  RX %d  DE %d", pt->pins[0], pt->pins[1], pt->pins[2]);
            else snprintf(v, sizeof v, "TX %d  RX %d", pt->pins[0], pt->pins[1]);
            break;
        case AOS_PORT_I2C: snprintf(v, sizeof v, "SDA %d  SCL %d", pt->pins[0], pt->pins[1]); break;
        case AOS_PORT_SPI:
            if (pt->pins[3] >= 0)
                snprintf(v, sizeof v, "SCK %d  MOSI %d  MISO %d  CS %d", pt->pins[0], pt->pins[1], pt->pins[2], pt->pins[3]);
            else snprintf(v, sizeof v, "SCK %d  MOSI %d  MISO %d", pt->pins[0], pt->pins[1], pt->pins[2]);
            break;
        default: snprintf(v, sizeof v, "GPIO %d", pt->pins[0]); break;
        }
        row(g, NULL, 0, pt->name, v, false, NULL, NULL);
    }
    if (aos_io_module_count()) {
        g = group(p, _("MÓDULOS"));
        for (int i = 0; i < aos_io_module_count(); i++) {
            const aos_io_module_t *m = aos_io_module_at(i);
            row(g, NULL, 0, m->name, m->port, false, NULL, NULL);
        }
    }
}

/* ---- Rows that ask twice ---- */

/* Going back to the other image or erasing a crash dump is one tap away
 * from a mistake: the first tap turns the row's label into the question,
 * the second within five seconds does it. */
static const char *s_armed_text;

static void disarm(void)
{
    if (U.armed) lv_label_set_text(lv_obj_get_child(U.armed, 0), s_armed_text);
    U.armed = NULL;
}

static bool second_tap(lv_event_t *e, const char *question)
{
    lv_obj_t *r = lv_event_get_current_target(e);
    if (U.armed == r) { U.armed = NULL; return true; }
    disarm();
    lv_obj_t *l = lv_obj_get_child(r, 0);
    static char was[96];
    snprintf(was, sizeof was, "%s", lv_label_get_text(l));
    s_armed_text = was;
    lv_label_set_text(l, question);
    U.armed = r;
    U.armed_ms = (uint32_t)aos_hal_uptime_ms();
    return false;
}

static lv_obj_t *action_row(lv_obj_t *g, const char *label, lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *r = row(g, NULL, 0, label, NULL, false, cb, NULL);
    lv_obj_t *l = lv_obj_get_child(r, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    return r;
}

static void fmt_size(char *out, size_t n, uint64_t b)
{
    if (b >= 1073741824ull) snprintf(out, n, "%.1f GB", b / 1073741824.0);
    else if (b >= 1048576ull) snprintf(out, n, "%.0f MB", b / 1048576.0);
    else if (b >= 1024) snprintf(out, n, "%.0f KB", b / 1024.0);
    else snprintf(out, n, "%u B", (unsigned)b);
}

/* ---- Storage ---- */

/* What fills the card, by the top-level folder it lives in. The walk goes
 * through aos_hal_dir_scan (one pass per folder: FAT's stat() per file is
 * O(n^2)) in a thread of its own, and the page redraws when it is done.
 * "Otros" is the rest of what the card says is used: loose files, hidden
 * folders (a Mac's Spotlight index), and FAT's own clusters. */
enum { ST_APPS, ST_MUSIC, ST_PHOTOS, ST_VIDEOS, ST_MAPS, ST_RECS, ST_OTHER, ST_N };
static const char *const ST_NAME[ST_N] = { N_("Apps y sus datos"), N_("Música"), N_("Fotos"), N_("Videos"),
                                           N_("Mapas"), N_("Grabaciones"), N_("Otros") };
static const uint32_t ST_COL[ST_N] = { 0x0A84FF, 0xFF375F, 0xFFD60A, 0xBF5AF2, 0x30D158, 0xFF9F0A, 0x8E8E93 };
static const char *const ST_GLYPH[ST_N] = { AOS_SYM_APPS, AOS_SYM_MUSIC, AOS_SYM_IMAGE, AOS_SYM_TELEVISION,
                                            AOS_SYM_EARTH, AOS_SYM_MICROPHONE, AOS_SYM_FILE_OUTLINE };

static struct {
    volatile int state;             /* 0 not counted, 1 counting, 2 done, -1 failed */
    volatile int gen;               /* moves on every change the page shows */
    volatile uint32_t files;
    uint64_t bytes[ST_N];
} S;

#define ST_NAME_MAX 128
#define ST_DIRS_MAX 256

typedef struct {
    uint64_t bytes;
    char (*dirs)[ST_NAME_MAX];
    int ndirs;
} st_walk_t;

static bool st_walk_cb(const aos_dir_entry_t *e, void *ctx)
{
    st_walk_t *w = ctx;
    if (!strcmp(e->name, ".") || !strcmp(e->name, "..")) return true;
    if (!e->dir) {
        w->bytes += e->size;
        S.files++;
    } else if (w->ndirs < ST_DIRS_MAX && strlen(e->name) < ST_NAME_MAX) {
        snprintf(w->dirs[w->ndirs++], ST_NAME_MAX, "%s", e->name);
    }
    return true;
}

/* A folder and everything under it. The subfolders are read first and
 * walked after, so no two folders are open at once. */
static uint64_t st_walk(const char *path, int depth)
{
    st_walk_t w = { .dirs = big_calloc(ST_DIRS_MAX * ST_NAME_MAX) };
    if (!w.dirs) return 0;
    aos_hal_dir_scan(path, st_walk_cb, &w);
    uint64_t b = w.bytes;
    char *sub = big_calloc(512);
    for (int i = 0; sub && depth < 12 && i < w.ndirs; i++) {
        snprintf(sub, 512, "%s/%s", path, w.dirs[i]);
        b += st_walk(sub, depth + 1);
    }
    free(sub);
    free(w.dirs);
    return b;
}

static const char *base_of(const char *path)
{
    const char *b = path ? strrchr(path, '/') : NULL;
    return b ? b + 1 : path ? path : "";
}

static int st_kind(const char *name)
{
    static const char *const APPS[] = { "apps", "apps_off", "icons", "lang", "data", "doom", "3d", "lua", "pixel" };
    if (name[0] == '.') return ST_OTHER;
    for (size_t i = 0; i < sizeof APPS / sizeof APPS[0]; i++)
        if (!strcmp(name, APPS[i])) return ST_APPS;
    if (!strcmp(name, base_of(aos_hal_path_apps())) || !strcmp(name, base_of(aos_hal_path_data()))) return ST_APPS;
    if (!strcmp(name, base_of(aos_hal_path_music()))) return ST_MUSIC;
    if (!strcmp(name, base_of(aos_hal_path_photos()))) return ST_PHOTOS;
    if (!strcmp(name, "videos")) return ST_VIDEOS;
    if (!strcmp(name, "maps")) return ST_MAPS;
    if (!strcmp(name, base_of(aos_hal_path_recordings()))) return ST_RECS;
    return ST_OTHER;
}

static void st_thread(void *arg)
{
    (void)arg;
    const char *root = aos_hal_path_sd_root();
    st_walk_t top = { .dirs = big_calloc(ST_DIRS_MAX * ST_NAME_MAX) };
    if (!root || !top.dirs || aos_hal_dir_scan(root, st_walk_cb, &top) < 0) {
        free(top.dirs);
        S.state = -1;
        S.gen++;
        return;
    }
    S.bytes[ST_OTHER] = top.bytes;          /* loose files at the root */
    char *sub = big_calloc(512);
    for (int i = 0; sub && i < top.ndirs; i++) {
        snprintf(sub, 512, "%s/%s", root, top.dirs[i]);
        S.bytes[st_kind(top.dirs[i])] += st_walk(sub, 1);
    }
    free(sub);
    free(top.dirs);
    S.state = 2;
    S.gen++;
}

static void st_count(void)
{
    if (S.state == 1) return;
    memset((void *)S.bytes, 0, sizeof S.bytes);
    S.files = 0;
    S.state = 1;
    S.gen++;
    if (!aos_hal_thread_start("storage", st_thread, NULL, 8192, 2)) { S.state = -1; S.gen++; }
}

static void eject_cb(lv_event_t *e)
{
    if (aos_hal_usb_mode() == AOS_HAL_USB_DISK) { aos_ui_toast(_("La tarjeta la tiene la computadora (modo Disco)"), 2500); return; }
    if (S.state == 1) { aos_ui_toast(_("Esperá a que termine de contar"), 1500); return; }
    if (!second_tap(e, _("Tocá otra vez para expulsarla"))) return;
    aos_ui_close_others();          /* nothing may keep a file open on it */
    if (aos_hal_sd_release()) aos_ui_toast(_("Ya podés sacar la tarjeta"), 2500);
    else aos_ui_toast(_("No se pudo expulsar la tarjeta"), 2000);
    S.state = 0;
    show(PG_STORAGE);
}

static void mount_cb(lv_event_t *e)
{
    if (aos_hal_usb_mode() == AOS_HAL_USB_DISK) { aos_ui_toast(_("La tarjeta la tiene la computadora (modo Disco)"), 2500); return; }
    if (aos_hal_sd_reclaim()) aos_ui_toast(_("Tarjeta montada"), 1500);
    else aos_ui_toast(_("No hay tarjeta, o no se pudo leer"), 2000);
    S.state = 0;
    show(PG_STORAGE);
}

static void build_storage(lv_obj_t *p)
{
    char v[64], a[24], b[24];
    U.st_shown = S.gen;
    uint64_t tot = 0, fr = 0;
    bool card = aos_hal_sd_usage(&tot, &fr) && tot;
    if (!card) {
        lv_obj_t *g = group(p, NULL);
        row(g, AOS_SYM_SD, 0x8E8E93, _("Tarjeta"), aos_hal_usb_mode() == AOS_HAL_USB_DISK ? _("en la computadora") : _("no hay"),
            false, NULL, NULL);
        if (aos_hal_usb_mode() != AOS_HAL_USB_DISK) action_row(g, _("Montar la tarjeta"), AOS_C_ACCENT, mount_cb);
        note(p, _("Con una microSD puesta, montala acá. En modo Disco del USB la tiene la computadora: vuelve sola al expulsarla allá."));
        return;
    }
    if (S.state == 0) st_count();
    uint64_t used = tot - fr, known = 0;
    for (int k = 0; k < ST_OTHER; k++) known += S.bytes[k];

    /* the bar: one segment per kind, then the free space */
    lv_obj_t *g = group(p, NULL);
    lv_obj_set_style_pad_all(g, 22, 0);
    lv_obj_set_style_pad_row(g, 16, 0);
    fmt_size(a, sizeof a, used);
    fmt_size(b, sizeof b, tot);
    snprintf(v, sizeof v, _("%s usados de %s"), a, b);
    aos_label(g, v, aos_font_body, AOS_C_TEXT);
    lv_obj_t *bar = lv_obj_create(g);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), 28);
    lv_obj_set_style_radius(bar, 8, 0);
    lv_obj_set_style_clip_corner(bar, true, 0);
    lv_obj_set_style_bg_color(bar, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 2, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    uint64_t shown[ST_N];
    memcpy(shown, S.bytes, sizeof shown);
    shown[ST_OTHER] = used > known ? used - known : 0;    /* while counting, the part not reached yet */
    for (int k = 0; k <= ST_N; k++) {
        uint64_t bytes = k < ST_N ? shown[k] : fr;
        if (!bytes) continue;
        lv_obj_t *seg = lv_obj_create(bar);
        lv_obj_remove_style_all(seg);
        lv_obj_set_height(seg, lv_pct(100));
        /* flex shares the width by grow: per-255 of the card, at least 1 */
        int grow = (int)lroundf(255.0f * (float)bytes / (float)tot);
        lv_obj_set_flex_grow(seg, (uint8_t)(grow < 1 ? 1 : grow > 255 ? 255 : grow));
        lv_obj_set_style_bg_color(seg, k == ST_N ? AOS_C_CARD2
                                       : lv_color_hex(k == ST_OTHER && S.state != 2 ? 0x48484A : ST_COL[k]), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    }

    g = group(p, NULL);
    for (int k = 0; k < ST_N; k++) {
        if (S.state == 2) fmt_size(v, sizeof v, shown[k]);
        else if (S.state == 1) snprintf(v, sizeof v, "…");
        else snprintf(v, sizeof v, "--");
        row(g, ST_GLYPH[k], ST_COL[k], aos_tr(ST_NAME[k]), v, false, NULL, NULL);
    }
    fmt_size(v, sizeof v, fr);
    row(g, AOS_SYM_CIRCLE_OUTLINE, 0x3A3A3C, _("Libre"), v, false, NULL, NULL);
    if (S.state == 1) {
        note(p, _("Contando lo que hay en la tarjeta…"));
    } else if (S.state == 2) {
        snprintf(v, sizeof v, _("%u archivos."), (unsigned)S.files);
        note(p, v);
    } else if (S.state < 0) {
        note(p, _("No se pudo recorrer la tarjeta."));
    }

    aos_sd_info_t si;
    if (aos_hal_sd_info(&si)) {
        g = group(p, _("LA TARJETA"));
        snprintf(v, sizeof v, "%s%s%s", si.kind ? si.kind : "?", si.fs && si.fs[0] ? " · " : "", si.fs ? si.fs : "");
        row(g, NULL, 0, _("Tipo"), v, false, NULL, NULL);
        if (si.name[0]) row(g, NULL, 0, _("Modelo"), si.name, false, NULL, NULL);
        fmt_size(v, sizeof v, si.capacity);
        row(g, NULL, 0, _("Capacidad"), v, false, NULL, NULL);
        if (si.freq_khz) {
            snprintf(v, sizeof v, _("%u MHz, %d bits"), (unsigned)(si.freq_khz / 1000), si.bus_width);
            row(g, NULL, 0, _("Bus"), v, false, NULL, NULL);
        }
        if (si.year) {
            snprintf(v, sizeof v, "%02d/%04d", si.month, si.year);
            row(g, NULL, 0, _("Fabricada"), v, false, NULL, NULL);
        }
    }
    g = group(p, NULL);
    action_row(g, _("Expulsar la tarjeta"), AOS_C_RED, eject_cb);
    note(p, _("Expulsala antes de sacarla: se cierran las otras apps, y las de la tarjeta no abren hasta que se monte de nuevo. Para copiar archivos desde la computadora, el modo Disco del USB."));
}

/* ---- Update ---- */

static void reboot_cb(lv_timer_t *t) { aos_hal_reboot(); }

static void rollback_cb(lv_event_t *e)
{
    if (!second_tap(e, _("Tocá otra vez para reiniciar con esa"))) return;
    if (!aos_hal_ota_boot_other()) {
        const char *err = aos_hal_ota_error();
        aos_ui_toast(err && err[0] ? err : _("La otra ranura no tiene una imagen entera"), 2500);
        return;
    }
    aos_ui_toast(_("Reiniciando con la versión anterior…"), 2000);
    lv_timer_t *t = lv_timer_create(reboot_cb, 1500, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static const char *slot_state_text(const char *st)
{
    if (!strcmp(st, "valid"))     return _("anduvo: se puede volver");
    if (!strcmp(st, "new") || !strcmp(st, "undefined")) return _("sin probar");
    if (!strcmp(st, "pending"))   return _("a prueba");
    if (!strcmp(st, "invalid"))   return _("marcada como mala");
    if (!strcmp(st, "aborted"))   return _("a medio instalar");
    return st[0] ? st : "--";
}

static void build_update(lv_obj_t *p)
{
    aos_ota_info_t o;
    bool ok = aos_hal_ota_info(&o);
    lv_obj_t *g = group(p, _("LA QUE CORRE"));
    row(g, NULL, 0, _("Versión"), aos_hal_firmware_version(), false, NULL, NULL);
    if (ok && o.built[0]) row(g, NULL, 0, _("Compilada"), o.built, false, NULL, NULL);
    if (ok && o.slot[0]) row(g, NULL, 0, _("Ranura"), o.slot, false, NULL, NULL);
    if (ok) row(g, NULL, 0, _("Estado"), o.trial ? _("a prueba") : _("confirmada"), false, NULL, NULL);

    g = group(p, _("LA ANTERIOR"));
    if (!ok) {
        row(g, NULL, 0, _("No se pudo leer la otra ranura"), NULL, false, NULL, NULL);
    } else if (!o.other_version[0]) {
        row(g, NULL, 0, _("Ranura"), o.other_slot[0] ? o.other_slot : "--", false, NULL, NULL);
        row(g, NULL, 0, _("Imagen"), _("ninguna"), false, NULL, NULL);
    } else {
        row(g, NULL, 0, _("Versión"), o.other_version, false, NULL, NULL);
        row(g, NULL, 0, _("Compilada"), o.other_built, false, NULL, NULL);
        row(g, NULL, 0, _("Ranura"), o.other_slot, false, NULL, NULL);
        row(g, NULL, 0, _("Estado"), slot_state_text(o.other_state), false, NULL, NULL);
        if (strcmp(o.other_state, "invalid") && strcmp(o.other_state, "aborted"))
            action_row(g, _("Volver a esta versión"), AOS_C_ACCENT, rollback_cb);
    }
    note(p, _("Las versiones nuevas llegan por el portal (Firmware) o con tools/ota.sh, y se escriben en la otra ranura. Arrancan a prueba: si la placa se reinicia antes de confirmarla, a los 30 s, vuelve sola a la anterior."));
}

/* ---- Diagnostics ---- */

static const char *reset_text(const char *k)
{
    if (!k) return "--";
    if (!strcmp(k, "power-on"))   return _("al encender");
    if (!strcmp(k, "software"))   return _("por software");
    if (!strcmp(k, "panic"))      return _("por un error (pánico)");
    if (!strcmp(k, "int-wdt"))    return _("watchdog de interrupciones");
    if (!strcmp(k, "task-wdt"))   return _("watchdog de tareas");
    if (!strcmp(k, "wdt"))        return _("watchdog");
    if (!strcmp(k, "brownout"))   return _("caída de tensión");
    if (!strcmp(k, "deep-sleep")) return _("al salir del sueño profundo");
    if (!strcmp(k, "external"))   return _("botón de reset");
    if (!strcmp(k, "usb"))        return _("por el USB");
    if (!strcmp(k, "jtag"))       return _("por JTAG");
    if (!strcmp(k, "efuse"))      return _("error de eFuse");
    if (!strcmp(k, "pwr-glitch")) return _("pico en la alimentación");
    if (!strcmp(k, "cpu-lockup")) return _("CPU trabada");
    return _("desconocido");
}

static void diag_refresh(void)
{
    char v[48];
    aos_sys_stats_t ss;
    if (aos_hal_sys_stats(&ss)) {
        if (U.dg_temp) {
            if (isnan(ss.chip_c)) snprintf(v, sizeof v, "--");
            else snprintf(v, sizeof v, "%.1f °C", ss.chip_c);
            lv_label_set_text(U.dg_temp, v);
        }
        if (U.dg_cpu) {
            if (ss.cpu_load[0] < 0) snprintf(v, sizeof v, "--");
            else snprintf(v, sizeof v, "%d %%  ·  %d %%", ss.cpu_load[0], ss.cpu_load[1]);
            lv_label_set_text(U.dg_cpu, v);
        }
    }
    aos_sys_info_t si;
    if (aos_hal_sys_info(&si)) {
        if (U.dg_int) {
            snprintf(v, sizeof v, _("%u KB (mínimo %u)"), (unsigned)(si.mem[AOS_MEM_INTERNAL].free / 1024),
                     (unsigned)(si.mem[AOS_MEM_INTERNAL].min_free / 1024));
            lv_label_set_text(U.dg_int, v);
        }
    }
    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    if (U.dg_psram) {
        snprintf(v, sizeof v, "%.1f MB", fp / 1048576.0);
        lv_label_set_text(U.dg_psram, v);
    }
    if (U.dg_up) {
        uint64_t up = aos_hal_uptime_ms() / 1000;
        snprintf(v, sizeof v, "%llu h %02llu min", up / 3600, (up / 60) % 60);
        lv_label_set_text(U.dg_up, v);
    }
}

static void dump_erase_cb(lv_event_t *e)
{
    if (!second_tap(e, _("Tocá otra vez para borrarlo"))) return;
    aos_ui_toast(aos_hal_coredump_erase() ? _("Volcado borrado") : _("No se pudo borrar"), 1500);
    show(PG_DIAG);
}

static void monitor_cb(lv_event_t *e) { aos_ui_open("aos.sysmon"); }

static void build_diag(lv_obj_t *p)
{
    char v[64];
    lv_obj_t *g = group(p, _("AHORA"));
    U.dg_temp = lv_obj_get_user_data(row(g, AOS_SYM_THERMOMETER, 0xFF9F0A, _("Temperatura del chip"), "--", false, NULL, NULL));
    U.dg_cpu = lv_obj_get_user_data(row(g, AOS_SYM_CHIP, 0x0A84FF, _("CPU (núcleos 0 · 1)"), "--", false, NULL, NULL));
    U.dg_int = lv_obj_get_user_data(row(g, AOS_SYM_MEMORY, 0x5E5CE6, _("Interna libre"), "--", false, NULL, NULL));
    U.dg_psram = lv_obj_get_user_data(row(g, AOS_SYM_MEMORY, 0x5E5CE6, _("PSRAM libre"), "--", false, NULL, NULL));
    U.dg_up = lv_obj_get_user_data(row(g, AOS_SYM_TIMER_OUTLINE, 0x8E8E93, _("Encendida hace"), "--", false, NULL, NULL));
    diag_refresh();

    g = group(p, _("ESTE ARRANQUE"));
    aos_sys_info_t si;
    row(g, NULL, 0, _("Motivo del reinicio"), aos_hal_sys_info(&si) ? reset_text(si.reset_reason) : "--", false, NULL, NULL);
    row(g, NULL, 0, _("Modo seguro"), aos_ui_safe_mode() ? _("sí: sin las apps de la tarjeta") : _("no"), false, NULL, NULL);
    /* the hang watchdog's restarts in a row that led to this boot */
    int hang = aos_hal_hang_restarts();
    if (hang) snprintf(v, sizeof v, "%d", hang);
    else snprintf(v, sizeof v, "%s", _("ninguno"));
    row(g, NULL, 0, _("Reinicios por cuelgue"), v, false, NULL, NULL);
    if (aos_ui_safe_mode()) note(p, _("Se arrancó con BOOT apretado: sin las apps de la tarjeta y con los ajustes de pantalla de fábrica. Reiniciá para volver a la normalidad."));

    g = group(p, _("RADIO (ESP32-C6)"));
    const char *c6 = aos_hal_net_coprocessor_fw();
    row(g, NULL, 0, _("Firmware del C6"), !c6[0] ? _("todavía no arrancó") : !strcmp(c6, "?") ? _("no la dijo") : c6,
        false, NULL, NULL);
    row(g, NULL, 0, _("Conexión"), "SDIO · esp_hosted", false, NULL, NULL);
    note(p, _("El Wi-Fi lo hace el ESP32-C6 de la placa; el P4 le habla por SDIO. Esta es la versión del firmware que corre en el C6."));

    g = group(p, _("ÚLTIMO CUELGUE"));
    aos_coredump_info_t ci;
    if (!aos_hal_coredump_info(&ci) || !ci.present) {
        row(g, NULL, 0, _("Volcado"), _("ninguno"), false, NULL, NULL);
    } else {
        row(g, NULL, 0, _("Volcado"), ci.valid ? _("guardado") : _("dañado"), false, NULL, NULL);
        if (ci.task[0]) row(g, NULL, 0, _("Tarea"), ci.task, false, NULL, NULL);
        fmt_size(v, sizeof v, ci.size);
        row(g, NULL, 0, _("Tamaño"), v, false, NULL, NULL);
        if (ci.elf_sha[0]) row(g, NULL, 0, _("Firmware (ELF)"), ci.elf_sha, false, NULL, NULL);
        action_row(g, _("Borrar el volcado"), AOS_C_RED, dump_erase_cb);
        note(p, _("El volcado se baja desde el portal (Firmware, Último cuelgue) o con tools/coredump.sh, y se lee con la ELF de esa compilación."));
    }

    g = group(p, NULL);
    row(g, AOS_SYM_CHART_LINE, 0x34C759, _("Abrir el Monitor"), NULL, true, monitor_cb, NULL);
}

/* ---- QR codes ---- */

/* A row of QR codes, centred, wrapping when the column is narrow. */
static lv_obj_t *qr_strip(lv_obj_t *p)
{
    lv_obj_t *box = lv_obj_create(p);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(box, 28, 0);
    return box;
}

/* One code with its caption underneath. Dark on white with a quiet zone:
 * phones read that, not the inverse. */
static void qr_box(lv_obj_t *strip, const char *text, const char *caption)
{
    lv_obj_t *col = lv_obj_create(strip);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 300, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_t *card = lv_obj_create(col);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_t *qr = lv_qrcode_create(card);
    lv_qrcode_set_size(qr, 260);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, text, (uint32_t)strlen(text));
    lv_obj_t *t = aos_label(col, caption, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(t, lv_pct(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
}

/* ---- The portal's address, for About ---- */

static void build_portal(lv_obj_t *p)
{
    char v[64];
    lv_obj_t *g = group(p, _("PORTAL WEB"));
    snprintf(v, sizeof v, "http://%s.local", aos_hal_device_name());
    row(g, NULL, 0, _("En la red"), v, false, NULL, NULL);
    bool wifi = aos_hal_net_state() == AOS_NET_CONNECTED;
    char url[48] = "";
    if (wifi) {
        snprintf(url, sizeof url, "http://%s/", aos_hal_net_ip());
        snprintf(v, sizeof v, "http://%s", aos_hal_net_ip());
        row(g, NULL, 0, _("Por Wi-Fi"), v, false, NULL, NULL);
    }
    if (aos_hal_net_ap_active()) {
        snprintf(v, sizeof v, "http://%s", aos_hal_net_ap_ip());
        row(g, NULL, 0, _("Por su red propia"), v, false, NULL, NULL);
        if (!wifi) snprintf(url, sizeof url, "http://%s/", aos_hal_net_ap_ip());
    }
    if (aos_hal_usb_net_up()) row(g, NULL, 0, _("Por el cable USB"), "http://192.168.7.1", false, NULL, NULL);
    if (!url[0]) {
        note(p, _("Sin Wi-Fi no hay código QR: el teléfono tiene que estar en la misma red que la placa, o en la red propia de la placa (Ajustes, Wi-Fi)."));
        return;
    }
    lv_obj_t *box = qr_strip(p);
    qr_box(box, url, _("Escaneálo con el teléfono para abrir el portal."));
}

/* ---- Language, time, about ---- */

static void lang_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    aos_ui_request_language(s_langs[i].code);
}

static void build_lang(lv_obj_t *p)
{
    int n = aos_i18n_scan(s_langs, AOS_LANG_MAX);
    lv_obj_t *g = group(p, NULL);
    for (int i = 0; i < n; i++)
        row(g, NULL, 0, s_langs[i].name, !strcmp(s_langs[i].code, aos_i18n_current()) ? AOS_SYM_CHECK : NULL, false, lang_cb,
            (void *)(intptr_t)i);
    note(p, _("Cambiar el idioma cierra las apps abiertas."));
}

static const struct { const char *name, *tz; } TZS[] = {
    { N_("Buenos Aires (UTC−3)"), "<-03>3" }, { N_("Santiago (UTC−4/−3)"), "<-04>4<-03>,M9.1.6/24,M4.1.6/24" },
    { N_("São Paulo (UTC−3)"), "<-03>3" }, { N_("Ciudad de México (UTC−6)"), "CST6" },
    { N_("Nueva York (UTC−5/−4)"), "EST5EDT,M3.2.0,M11.1.0" }, { N_("Madrid (UTC+1/+2)"), "CET-1CEST,M3.5.0,M10.5.0/3" },
    { N_("Londres (UTC+0/+1)"), "GMT0BST,M3.5.0/1,M10.5.0/2" }, { "UTC", "UTC0" },
};

static void tz_cb(lv_event_t *e)
{
    aos_hal_timezone_set(TZS[(int)(intptr_t)lv_event_get_user_data(e)].tz);
    show(PG_TIME);
}

static void name_done(const char *name)
{
    if (aos_hal_device_name_set(name)) show(PG_ABOUT);
    else aos_ui_toast(_("Sólo a-z, 0-9 y guiones"), 2000);
}

static void name_cb(lv_event_t *e) { kb_open(_("Nombre en la red"), aos_hal_device_name(), AOS_DEVICE_NAME_MAX, _("Listo"), name_done); }

static void build_time(lv_obj_t *p)
{
    struct tm t;
    aos_hal_time_now(&t);
    char v[48];
    snprintf(v, sizeof v, "%02d/%02d/%04d  %d:%02d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, t.tm_hour, t.tm_min);
    lv_obj_t *g = group(p, NULL);
    row(g, NULL, 0, _("Ahora"), aos_hal_time_is_valid() ? v : _("sin hora (esperando la red)"), false, NULL, NULL);
    g = group(p, _("ZONA HORARIA"));
    bool ticked = false;
    for (size_t i = 0; i < sizeof TZS / sizeof TZS[0]; i++) {
        bool on = !ticked && !strcmp(TZS[i].tz, aos_hal_timezone_get());
        ticked |= on;
        row(g, NULL, 0, _(TZS[i].name), on ? AOS_SYM_CHECK : NULL, false, tz_cb, (void *)(intptr_t)i);
    }
    note(p, _("La hora llega por la red (SNTP). La placa no tiene reloj con pila todavía."));
}

static void build_about(lv_obj_t *p)
{
    char v[64];
    lv_obj_t *g = group(p, NULL);
    row(g, NULL, 0, _("Placa"), aos_hal_board_name(), false, NULL, NULL);
    row(g, NULL, 0, _("Firmware"), aos_hal_firmware_version(), false, NULL, NULL);
    row(g, NULL, 0, _("Nombre en la red"), aos_hal_device_name(), true, name_cb, NULL);
    build_portal(p);
    g = group(p, _("MEMORIA"));
    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    snprintf(v, sizeof v, "%u KB", (unsigned)(fi / 1024));
    row(g, NULL, 0, _("Interna libre"), v, false, NULL, NULL);
    snprintf(v, sizeof v, "%.1f MB", fp / 1048576.0);
    row(g, NULL, 0, _("PSRAM libre"), v, false, NULL, NULL);
    uint64_t tot = 0, fr = 0;
    if (aos_hal_sd_usage(&tot, &fr)) snprintf(v, sizeof v, "%.1f / %.1f GB", fr / 1073741824.0, tot / 1073741824.0);
    else snprintf(v, sizeof v, "%s", _("sin tarjeta"));
    row(g, NULL, 0, _("microSD libre"), v, false, NULL, NULL);
    uint64_t up = aos_hal_uptime_ms() / 1000;
    snprintf(v, sizeof v, "%llu h %02llu min", up / 3600, (up / 60) % 60);
    row(g, NULL, 0, _("Encendida hace"), v, false, NULL, NULL);
    g = group(p, _("QUÉ HAY"));
    static const struct { uint32_t cap; const char *name; } CAPS[] = {
        { AOS_CAP_WIFI, "Wi-Fi" }, { AOS_CAP_BLE, "Bluetooth" }, { AOS_CAP_MIC, N_("Micrófono") },
        { AOS_CAP_SPEAKER, N_("Parlante") }, { AOS_CAP_DUPLEX, N_("Audio full dúplex") },
        { AOS_CAP_USB_DEVICE, "USB" }, { AOS_CAP_HEADER, N_("Conector de expansión") },
        { AOS_CAP_BATTERY, N_("Batería") }, { AOS_CAP_CAMERA, N_("Cámara") }, { AOS_CAP_IMU, N_("Acelerómetro") },
    };
    for (size_t i = 0; i < sizeof CAPS / sizeof CAPS[0]; i++)
        row(g, NULL, 0, aos_tr(CAPS[i].name), aos_hal_has(CAPS[i].cap) ? _("sí") : _("no"), false, NULL, NULL);
}

/* -------------------------------------------------------------------------- */
/* Navigation                                                                  */
/* -------------------------------------------------------------------------- */

static void back_btn_cb(lv_event_t *e);

/* A scrolling column holding one page. */
static lv_obj_t *column(lv_obj_t *parent, int32_t x, int32_t w)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, U.H);
    lv_obj_set_pos(c, x, 0);
    lv_obj_set_style_pad_hor(c, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_bottom(c, 30, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    return c;
}

static void fill(lv_obj_t *col, int pg, bool back)
{
    U.PW = lv_obj_get_style_width(col, 0);
    lv_obj_t *head = lv_obj_create(col);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), back ? 120 : 90);
    if (back) {
        lv_obj_t *b = lv_obj_create(head);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_SIZE_CONTENT, 56);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, back_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_color(l, AOS_C_ACCENT, 0);
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        lv_label_set_text_fmt(l, LV_SYMBOL_LEFT "  %s", aos_tr(PG_TITLE[U.depth > 1 ? U.stack[U.depth - 2] : PG_ROOT]));
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    }
    lv_obj_t *t = aos_label(head, aos_tr(PG_TITLE[pg]), aos_font_large, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_BOTTOM_LEFT, 4, 0);

    switch (pg) {
    case PG_WIFI: build_wifi(col); break;
    case PG_DISPLAY: build_display(col); break;
    case PG_SOUND: build_sound(col); break;
    case PG_WALL: build_wall(col); break;
    case PG_EXP: build_exp(col); break;
    case PG_LANG: build_lang(col); break;
    case PG_TIME: build_time(col); break;
    case PG_ABOUT: build_about(col); break;
    case PG_USB: build_usb(col); break;
    case PG_STORAGE: build_storage(col); break;
    case PG_UPDATE: build_update(col); break;
    case PG_DIAG: build_diag(col); break;
    default: build_root(col); break;
    }
}

/* Portrait: one page, pushed over the list. Landscape: the list on the left
 * and the chosen page on the right, like the tablet's; with nothing chosen
 * yet the right side shows Wi-Fi. */
#define SIDEBAR_W 460

static void build(int pg)
{
    kb_close();
    int32_t side_y = U.side ? lv_obj_get_scroll_y(U.side) : 0;
    if (U.page) lv_obj_delete(U.page);
    U.wifi_list = U.wifi_state = U.side = NULL;
    U.usb_state = U.usb_dot = U.usb_root = NULL;
    U.dg_temp = U.dg_cpu = U.dg_int = U.dg_psram = U.dg_up = NULL;
    U.armed = NULL;
    memset(U.usb_tick, 0, sizeof U.usb_tick);
    if (!U.land) {
        U.page = column(U.root, 0, U.W);
        U.selected = -1;
        fill(U.page, pg, pg != PG_ROOT);
        return;
    }
    U.page = lv_obj_create(U.root);
    lv_obj_remove_style_all(U.page);
    lv_obj_set_size(U.page, U.W, U.H);
    lv_obj_remove_flag(U.page, LV_OBJ_FLAG_SCROLLABLE);
    int right = pg == PG_ROOT ? PG_WIFI : pg;
    U.selected = right;
    lv_obj_t *side = column(U.page, 0, SIDEBAR_W);
    lv_obj_set_style_pad_right(side, AOS_UI_PAD / 2, 0);
    fill(side, PG_ROOT, false);
    lv_obj_update_layout(side);
    lv_obj_scroll_to_y(side, side_y, LV_ANIM_OFF);   /* choosing a page must not jump the list */
    U.side = side;
    lv_obj_t *main = column(U.page, SIDEBAR_W, U.W - SIDEBAR_W);
    lv_obj_set_style_pad_left(main, AOS_UI_PAD / 2, 0);
    fill(main, right, false);
}

/* Opens a page on top (or rebuilds the current one if it is the same). */
static void show(int pg)
{
    if (U.depth && U.stack[U.depth - 1] == pg) { build(pg); return; }
    /* in landscape the sidebar swaps the right-hand page, it does not stack */
    if (U.land && pg != PG_ROOT && U.depth >= 2) U.stack[U.depth - 1] = pg;
    else if (U.depth < 8) U.stack[U.depth++] = pg;
    build(pg);
}

static bool pop(void)
{
    if (U.overlay) { kb_close(); return true; }
    if (U.depth <= 1 || (U.land && U.depth <= 2)) return false;
    U.depth--;
    build(U.stack[U.depth - 1]);
    return true;
}

static void back_btn_cb(lv_event_t *e) { pop(); }

static void timer_cb(lv_timer_t *t)
{
    static int shown = -4;
    if (U.naps != shown && U.wifi_list) { shown = U.naps; wifi_list_fill(); }
    if (U.usb_state || U.usb_root) usb_refresh();
    int pg = U.land ? U.selected : U.depth ? U.stack[U.depth - 1] : PG_ROOT;
    if (pg == PG_WIFI && U.ap_shown != ap_key() && !U.overlay && !U.armed) build(pg);   /* the access point came up or down */
    if (pg == PG_STORAGE && U.st_shown != S.gen && !U.overlay) build(pg);   /* the count moved on */
    static int ticks;
    if (U.dg_temp && ++ticks % 3 == 0) diag_refresh();
    if (U.armed && (uint32_t)aos_hal_uptime_ms() - U.armed_ms > 5000) disarm();
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    int stack[8], depth = U.depth;
    memcpy(stack, U.stack, sizeof stack);
    int naps = U.naps;
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    U.naps = (naps == -2 || !depth) ? -3 : naps;
    /* a turn of the screen comes back to the same page */
    if (depth) { memcpy(U.stack, stack, sizeof stack); U.depth = depth; build(U.stack[depth - 1]); }
    else show(PG_ROOT);
    U.timer = lv_timer_create(timer_cb, 300, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    if (U.timer) lv_timer_delete(U.timer);
    U.timer = NULL;
    U.page = U.side = NULL;
}

static bool back(aos_app_t *self, void *inst) { return pop(); }

void aos_app_settings_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.settings", .name = "Ajustes", .icon = AOS_SYM_COG,
            .color_a = 0x8E8E93, .color_b = 0x48484A,
            .flags = AOS_APP_FLAG_KEEP, .order = 900,
        },
        .create = create, .destroy = destroy, .back = back,
    };
}
