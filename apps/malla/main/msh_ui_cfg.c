/*
 * Malla - the node: how it is reached, and the basics of its setup.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * The connection sheet offers the three ways: a serial port (the USB
 * host's usb0/usb1, the header's UARTs), TCP to port 4403 (the ESP32 nodes
 * with Wi-Fi on), and Bluetooth (a scan for Meshtastic's service). The one
 * chosen is remembered and the app goes back to it by itself.
 *
 * The node's setup: its owner (long and short name), the LoRa region,
 * preset and hop limit, its channels (rename, add, remove) and the channel
 * set as a meshtastic.org/e/# link and QR, to share or to take one. Each
 * change is an AdminMessage to our own node; a radio or channel change
 * makes the node restart, which the session rides through.
 */
#include "msh_ui.h"

#include "aos_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sheet_close_cb(lv_event_t *e)
{
    (void)e;
    msh_ui_sheet_close();
}

static lv_obj_t *field(lv_obj_t *parent, const char *placeholder, const char *text, int maxlen, bool one_line)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, one_line);
    if (!one_line) lv_obj_set_height(ta, 150);
    lv_textarea_set_max_length(ta, (uint32_t)maxlen);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_text(ta, text ? text : "");
    lv_obj_set_style_text_font(ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(ta, AOS_C_BG, 0);
    lv_obj_set_style_text_color(ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 18, 0);
    lv_obj_set_style_pad_all(ta, 16, 0);
    return ta;
}

static lv_obj_t *s_kb;

static void kb_focus_cb(lv_event_t *e)
{
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, lv_event_get_target(e));
        lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *sheet_kb(lv_obj_t *panel, lv_obj_t *first, lv_keyboard_mode_t mode)
{
    s_kb = lv_keyboard_create(panel);
    aos_keyboard_style(s_kb, aos_font_body);
    lv_obj_set_size(s_kb, LV_PCT(100), UI.land ? 300 : 400);
    lv_keyboard_set_mode(s_kb, mode);
    lv_keyboard_set_textarea(s_kb, first);
    return s_kb;
}

static lv_obj_t *dropdown(lv_obj_t *parent, const char *opts, int sel)
{
    lv_obj_t *d = lv_dropdown_create(parent);
    lv_dropdown_set_options(d, opts);
    lv_dropdown_set_selected(d, (uint32_t)(sel < 0 ? 0 : sel));
    lv_obj_set_width(d, LV_PCT(100));
    lv_obj_set_style_text_font(d, aos_font_small, 0);
    lv_obj_set_style_bg_color(d, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(d, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, 16, 0);
    lv_obj_set_style_pad_all(d, 16, 0);
    lv_obj_t *list = lv_dropdown_get_list(d);
    lv_obj_set_style_text_font(list, aos_font_small, 0);
    lv_obj_set_style_bg_color(list, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(list, AOS_C_TEXT, 0);
    lv_obj_set_style_max_height(list, UI.H / 2, 0);
    return d;
}

static void wrap_label(lv_obj_t *l)
{
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
}

/* ---- confirming ---- */

static void (*s_confirm_fn)(void);

static void confirm_yes_cb(lv_event_t *e)
{
    (void)e;
    void (*fn)(void) = s_confirm_fn;
    msh_ui_sheet_close();
    if (fn) fn();
}

static void confirm(const char *title, const char *text, const char *yes, void (*fn)(void))
{
    s_confirm_fn = fn;
    lv_obj_t *p = msh_ui_sheet(MSH_SHEET_CONFIRM, title);
    wrap_label(msh_ui_label(p, text, aos_font_small, AOS_C_DIM));
    lv_obj_t *r = msh_ui_row(p);
    msh_ui_btn(r, _("Cancelar"), AOS_C_CARD2, sheet_close_cb, NULL);
    msh_ui_btn(r, yes, AOS_C_ACCENT, confirm_yes_cb, NULL);
}

/* ---- the connection sheet ---- */

static lv_obj_t *s_baud, *s_host, *s_port;
static const uint32_t BAUDS[] = { 115200, 9600, 19200, 38400, 57600, 230400, 460800, 921600 };

static void serial_cb(lv_event_t *e)
{
    const char *port = (const char *)lv_event_get_user_data(e);
    msh_target_t t = { 0 };
    t.kind = MSH_LINK_SERIAL;
    msh_scpy(t.port, sizeof t.port, port);
    t.baud = BAUDS[s_baud ? lv_dropdown_get_selected(s_baud) : 0];
    msh_ui_sheet_close();
    msh_app_connect(&t);
}

static void tcp_cb(lv_event_t *e)
{
    (void)e;
    msh_target_t t = { 0 };
    t.kind = MSH_LINK_TCP;
    const char *h = lv_textarea_get_text(s_host);
    while (*h == ' ') h++;
    if (!*h) {
        aos_ui_toast(_("Falta la dirección del nodo"), 1800);
        return;
    }
    msh_scpy(t.host, sizeof t.host, h);
    for (char *p = t.host + strlen(t.host); p > t.host && p[-1] == ' ';) *--p = 0;
    t.tcp_port = (int)strtol(lv_textarea_get_text(s_port), NULL, 10);
    if (t.tcp_port <= 0 || t.tcp_port > 65535) t.tcp_port = 4403;
    msh_ui_sheet_close();
    msh_app_connect(&t);
}

static void ble_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= UI.n_found) return;
    msh_target_t t = { 0 };
    t.kind = MSH_LINK_BLE;
    memcpy(t.addr, UI.found[i].addr, 6);
    t.addr_type = UI.found[i].type;
    msh_scpy(t.name, sizeof t.name, UI.found[i].name);
    msh_ui_sheet_close();
    msh_app_connect(&t);
}

static void bt_on_cb(lv_event_t *e)
{
    (void)e;
    aos_hal_bt_enable(true);
    msh_ui_conn_sheet();
}

#if MSH_BLE_PAIR
/* The node's keys go: the next connection pairs again and asks for the PIN
 * (a node reflashed, or whose PIN was changed). */
static void forget_cb(lv_event_t *e)
{
    (void)e;
    msh_lock();
    msh_target_t t = MSH.target;
    msh_unlock();
    if (t.kind != MSH_LINK_BLE) return;
    msh_sess_stop();
    aos_hal_ble_gatt_forget(t.addr, t.addr_type);
    aos_ui_toast(_("Emparejamiento olvidado: la próxima vez pide el PIN"), 2500);
    msh_ui_sheet_close();
    msh_app_connect(&t);
}
#endif

static void disconnect_cb(lv_event_t *e)
{
    (void)e;
    msh_ui_sheet_close();
    msh_app_disconnect();
}

static void fill_scan(void)
{
    if (!UI.scan_list) return;
    lv_obj_clean(UI.scan_list);
    if (!UI.n_found) {
        msh_ui_label(UI.scan_list, _("Buscando nodos Meshtastic cerca…"), aos_font_small, AOS_C_DIM);
        return;
    }
    for (int i = 0; i < UI.n_found; i++) {
        char t[80];
        snprintf(t, sizeof t, "%s  ·  %d dBm", UI.found[i].name[0] ? UI.found[i].name : "Meshtastic", UI.found[i].rssi);
        lv_obj_t *b = msh_ui_btn(UI.scan_list, t, AOS_C_CARD2, ble_pick_cb, (void *)(intptr_t)i);
        lv_obj_set_width(b, LV_PCT(100));
    }
}

static void adv_name(const uint8_t *d, int len, char *out, size_t cap)
{
    for (int i = 0; i + 1 < len;) {
        int l = d[i];
        if (!l || i + 1 + l > len) break;
        if (d[i + 1] == 0x09 || d[i + 1] == 0x08) {
            size_t n = (size_t)(l - 1) < cap - 1 ? (size_t)(l - 1) : cap - 1;
            memcpy(out, d + i + 2, n);
            out[n] = 0;
            return;
        }
        i += 1 + l;
    }
}

void msh_ui_scan_tick(void)
{
    if (!UI.scanning) return;
    aos_ble_adv_t rep[24];
    int n = aos_hal_ble_scan_read(rep, 24);
    bool changed = false;
    for (int k = 0; k < n; k++) {
        const aos_ble_adv_t *a = &rep[k];
        int i;
        for (i = 0; i < UI.n_found && memcmp(UI.found[i].addr, a->addr, 6); i++) {}
        bool mesh = msh_ble_adv_is_meshtastic(a->data, a->len);
        if (i == UI.n_found) {
            if (!mesh || UI.n_found >= (int)(sizeof UI.found / sizeof UI.found[0])) continue;
            memset(&UI.found[i], 0, sizeof UI.found[i]);
            memcpy(UI.found[i].addr, a->addr, 6);
            UI.found[i].type = a->addr_type;
            UI.n_found++;
            changed = true;
        }
        UI.found[i].rssi = a->rssi;
        UI.found[i].seen = msh_now_s();
        char nm[32] = "";
        adv_name(a->data, a->len, nm, sizeof nm);
        if (nm[0] && strcmp(nm, UI.found[i].name)) {
            msh_scpy(UI.found[i].name, sizeof UI.found[i].name, nm);
            changed = true;
        }
    }
    static uint32_t last;
    if (changed || msh_now_s() != last) {
        last = msh_now_s();
        fill_scan();
    }
}

void msh_ui_scan_stop(void)
{
    if (UI.scanning) aos_hal_ble_scan_stop();
    UI.scanning = false;
    UI.scan_list = NULL;
}

void msh_ui_conn_sheet(void)
{
    lv_obj_t *p = msh_ui_sheet(MSH_SHEET_CONN, _("Conectar con el nodo"));
    msh_lock();
    msh_target_t cur = MSH.target;
    char err[96];
    msh_scpy(err, sizeof err, MSH.err);
    msh_unlock();
    if (cur.kind != MSH_LINK_NONE) {
        char t[120], line[200];
        msh_target_text(&cur, t, sizeof t);
        snprintf(line, sizeof line, "%s: %s%s%s", _("Ahora"), t, err[0] ? "  ·  " : "", err[0] ? _(err) : "");
        wrap_label(msh_ui_label(p, line, aos_font_small, err[0] ? AOS_C_ORANGE : AOS_C_DIM));
        lv_obj_t *r0 = msh_ui_row(p);
        lv_obj_set_flex_flow(r0, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(r0, 12, 0);
        msh_ui_btn(r0, _("Desconectar y olvidar"), AOS_C_CARD2, disconnect_cb, NULL);
#if MSH_BLE_PAIR
        if (cur.kind == MSH_LINK_BLE) msh_ui_btn(r0, _("Emparejar de nuevo"), AOS_C_CARD2, forget_cb, NULL);
#endif
    }

    /* serial */
    lv_obj_t *c = msh_ui_card(p);
    lv_obj_set_style_bg_color(c, AOS_C_CARD2, 0);
    msh_ui_label(c, _("Por USB o serie"), aos_font_body, AOS_C_TEXT);
    char note[120];
    int nusb = aos_hal_usb_serial_count();
    snprintf(note, sizeof note, nusb ? _("%d puerto USB enchufado. El nodo va a 115200 salvo que lo hayas cambiado.")
                                     : _("Enchufá el nodo al USB de la placa, o a un UART del conector."), nusb);
    wrap_label(msh_ui_label(c, note, aos_font_small, AOS_C_DIM));
    char bauds[120] = "";
    for (size_t i = 0; i < sizeof BAUDS / sizeof BAUDS[0]; i++) {
        size_t l = strlen(bauds);
        snprintf(bauds + l, sizeof bauds - l, "%s%lu", i ? "\n" : "", (unsigned long)BAUDS[i]);
    }
    s_baud = dropdown(c, bauds, 0);
    lv_obj_t *r = msh_ui_row(c);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, 12, 0);
    static const char *const USB[] = { "usb0", "usb1" };
    for (int i = 0; i < 2; i++) msh_ui_btn(r, USB[i], i < nusb ? AOS_C_ACCENT : AOS_C_BG, serial_cb, (void *)USB[i]);
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *pt = aos_io_port_at(i);
        if (pt && pt->kind == AOS_PORT_UART) msh_ui_btn(r, pt->name, AOS_C_BG, serial_cb, (void *)pt->name);
    }

    /* TCP */
    c = msh_ui_card(p);
    lv_obj_set_style_bg_color(c, AOS_C_CARD2, 0);
    msh_ui_label(c, _("Por Wi-Fi (TCP)"), aos_font_body, AOS_C_TEXT);
    wrap_label(msh_ui_label(c, _("Un nodo con Wi-Fi atiende a los clientes en el puerto 4403."), aos_font_small, AOS_C_DIM));
    s_host = field(c, "meshtastic.local", cur.kind == MSH_LINK_TCP ? cur.host : "", 63, true);
    r = msh_ui_row(c);
    s_port = field(r, "4403", "4403", 5, true);
    lv_obj_set_width(s_port, 180);
    lv_textarea_set_accepted_chars(s_port, "0123456789");
    if (cur.kind == MSH_LINK_TCP && cur.tcp_port) {
        char pp[8];
        snprintf(pp, sizeof pp, "%d", cur.tcp_port);
        lv_textarea_set_text(s_port, pp);
    }
    msh_ui_btn(r, _("Conectar"), AOS_C_ACCENT, tcp_cb, NULL);
    lv_obj_add_event_cb(s_host, kb_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_port, kb_focus_cb, LV_EVENT_FOCUSED, NULL);

    /* BLE */
    c = msh_ui_card(p);
    lv_obj_set_style_bg_color(c, AOS_C_CARD2, 0);
    msh_ui_label(c, _("Por Bluetooth"), aos_font_body, AOS_C_TEXT);
    if (!aos_hal_bt_enabled()) {
        wrap_label(msh_ui_label(c, _("El Bluetooth de la placa está apagado."), aos_font_small, AOS_C_DIM));
        msh_ui_btn(c, _("Prender el Bluetooth"), AOS_C_ACCENT, bt_on_cb, NULL);
    } else {
#if !MSH_BLE_PAIR
        wrap_label(msh_ui_label(c, _("Este firmware todavía no empareja con PIN: sólo anda con un nodo en modo sin PIN."),
                                aos_font_small, AOS_C_ORANGE));
#endif
        UI.scan_list = lv_obj_create(c);
        lv_obj_remove_style_all(UI.scan_list);
        lv_obj_set_size(UI.scan_list, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(UI.scan_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(UI.scan_list, 10, 0);
        UI.n_found = 0;
        UI.scanning = aos_hal_ble_scan_start(true, 30);
        fill_scan();
    }
    s_kb = sheet_kb(p, s_host, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

/* ---- the PIN ---- */

static lv_obj_t *s_pin;

static void pin_ok_cb(lv_event_t *e)
{
    (void)e;
    const char *t = lv_textarea_get_text(s_pin);
    if (strlen(t) != 6) {
        aos_ui_toast(_("El PIN tiene seis cifras"), 1500);
        return;
    }
    msh_sess_passkey((uint32_t)strtoul(t, NULL, 10));
    msh_ui_sheet_close();
}

static void pin_kb_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) pin_ok_cb(e);
}

static void cmp_cb(lv_event_t *e)
{
    msh_sess_passkey((uint32_t)(uintptr_t)lv_event_get_user_data(e));
    msh_ui_sheet_close();
}

/* The board chose the PIN, or both sides show a number to compare: the
 * number big, and for a comparison the two answers. */
static void number_sheet(msh_ask_t ask, uint32_t num)
{
    lv_obj_t *p = msh_ui_sheet(MSH_SHEET_PIN, _("Emparejar"));
    wrap_label(msh_ui_label(p, ask == MSH_ASK_SHOW ? _("Escribí este PIN en el nodo para emparejarlo.")
                                                   : _("¿El nodo muestra este mismo número?"),
                            aos_font_small, AOS_C_DIM));
    char n[12];
    snprintf(n, sizeof n, "%06lu", (unsigned long)(num % 1000000));
    lv_obj_t *nl = msh_ui_label(p, n, aos_font_huge, AOS_C_TEXT);
    lv_obj_set_width(nl, LV_PCT(100));
    lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *r = msh_ui_row(p);
    if (ask == MSH_ASK_COMPARE) {
        msh_ui_btn(r, _("No es el mismo"), AOS_C_CARD2, cmp_cb, (void *)0);
        msh_ui_btn(r, _("Es el mismo"), AOS_C_ACCENT, cmp_cb, (void *)1);
    } else {
        msh_ui_btn(r, _("Listo"), AOS_C_CARD2, sheet_close_cb, NULL);
    }
}

void msh_ui_pin_sheet(void)
{
    msh_lock();
    msh_ask_t ask = MSH.ask;
    uint32_t num = MSH.ask_num;
    msh_unlock();
    if (ask == MSH_ASK_SHOW || ask == MSH_ASK_COMPARE) {
        number_sheet(ask, num);
        return;
    }
    lv_obj_t *p = msh_ui_sheet(MSH_SHEET_PIN, _("Emparejar"));
    wrap_label(msh_ui_label(p, _("Escribí el PIN de seis cifras que muestra el nodo. Uno sin pantalla suele usar 123456."),
                            aos_font_small, AOS_C_DIM));
    s_pin = field(p, "123456", "", 6, true);
    lv_textarea_set_accepted_chars(s_pin, "0123456789");
    lv_obj_set_style_text_font(s_pin, aos_font_title, 0);
    msh_ui_btn(p, _("Emparejar"), AOS_C_ACCENT, pin_ok_cb, NULL);
    lv_obj_t *kb = sheet_kb(p, s_pin, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_event_cb(kb, pin_kb_cb, LV_EVENT_READY, NULL);
}

/* ---- the owner ---- */

static lv_obj_t *s_long, *s_short;

static void owner_save_cb(lv_event_t *e)
{
    (void)e;
    const char *l = lv_textarea_get_text(s_long), *s = lv_textarea_get_text(s_short);
    if (!l[0] || !s[0]) {
        aos_ui_toast(_("Hacen falta los dos nombres"), 1500);
        return;
    }
    if (msh_set_owner(l, s)) aos_ui_toast(_("Nombre cambiado"), 1500);
    else aos_ui_toast(_("No se pudo mandar al nodo"), 1800);
    msh_ui_sheet_close();
    UI.dirty = true;
}

static void owner_sheet(void)
{
    lv_obj_t *p = msh_ui_sheet(MSH_SHEET_EDIT, _("Nombre del nodo"));
    msh_lock();
    msh_node_t *me = msh_node_find(MSH.my_num);
    char ln[40] = "", sn[8] = "";
    if (me) {
        msh_scpy(ln, sizeof ln, me->long_name);
        msh_scpy(sn, sizeof sn, me->short_name);
    }
    msh_unlock();
    msh_ui_label(p, _("Nombre largo (hasta 24 letras)"), aos_font_small, AOS_C_DIM);
    s_long = field(p, _("Nombre largo"), ln, 24, true);
    msh_ui_label(p, _("Nombre corto (hasta 4)"), aos_font_small, AOS_C_DIM);
    s_short = field(p, "ABCD", sn, 4, true);
    msh_ui_btn(p, _("Guardar"), AOS_C_ACCENT, owner_save_cb, NULL);
    lv_obj_add_event_cb(s_long, kb_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_short, kb_focus_cb, LV_EVENT_FOCUSED, NULL);
    sheet_kb(p, s_long, LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void owner_cb(lv_event_t *e)
{
    (void)e;
    owner_sheet();
}

/* ---- the radio ---- */

static lv_obj_t *s_region, *s_preset, *s_hops;
static meshtastic_Config_LoRaConfig s_lora_new;

static void lora_apply(void)
{
    if (msh_set_lora(&s_lora_new)) aos_ui_toast(_("Radio cambiada: el nodo se reinicia"), 2500);
    else aos_ui_toast(_("No se pudo mandar al nodo"), 1800);
}

static void lora_save_cb(lv_event_t *e)
{
    (void)e;
    if (MSH.state != MSH_ST_READY) { aos_ui_toast(_("El nodo no está conectado"), 1800); return; }
    msh_lock();
    s_lora_new = MSH.lora;
    msh_unlock();
    s_lora_new.region = (meshtastic_Config_LoRaConfig_RegionCode)lv_dropdown_get_selected(s_region);
    s_lora_new.use_preset = true;
    s_lora_new.modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)lv_dropdown_get_selected(s_preset);
    s_lora_new.hop_limit = lv_dropdown_get_selected(s_hops) + 1;
    s_lora_new.tx_enabled = true;
    confirm(_("Cambiar la radio"),
            _("El nodo se reinicia para tomar la región y el preset nuevos. Todos los nodos de una malla tienen que usar el mismo preset."),
            _("Cambiar"), lora_apply);
}

/* ---- channels ---- */

static int s_ch_edit;
static lv_obj_t *s_ch_name;

static void psk_text(const meshtastic_ChannelSettings *s, char *out, size_t cap)
{
    if (s->psk.size == 0 || (s->psk.size == 1 && s->psk.bytes[0] == 0)) snprintf(out, cap, "%s", _("sin cifrar"));
    else if (s->psk.size == 1) snprintf(out, cap, "%s", _("clave predeterminada"));
    else if (s->psk.size == 16) snprintf(out, cap, "AES-128");
    else snprintf(out, cap, "AES-256");
}

static void ch_save_cb(lv_event_t *e)
{
    (void)e;
    msh_lock();
    meshtastic_Channel c = MSH.ch[s_ch_edit];
    msh_unlock();
    c.index = (int8_t)s_ch_edit;
    c.has_settings = true;
    msh_scpy(c.settings.name, sizeof c.settings.name, lv_textarea_get_text(s_ch_name));
    if (msh_set_channel(&c)) aos_ui_toast(_("Canal guardado"), 1500);
    else aos_ui_toast(_("No se pudo mandar al nodo"), 1800);
    msh_ui_sheet_close();
    UI.dirty = true;
}

static void ch_delete(void)
{
    meshtastic_Channel c = meshtastic_Channel_init_zero;
    c.index = (int8_t)s_ch_edit;
    c.role = meshtastic_Channel_Role_DISABLED;
    if (msh_set_channel(&c)) aos_ui_toast(_("Canal borrado"), 1500);
    UI.dirty = true;
}

static void ch_delete_cb(lv_event_t *e)
{
    (void)e;
    confirm(_("Borrar el canal"), _("Los mensajes de este canal dejan de llegar a este nodo."), _("Borrar"), ch_delete);
}

static void ch_sheet(int idx)
{
    s_ch_edit = idx;
    char title[40];
    snprintf(title, sizeof title, _("Canal %d"), idx);
    lv_obj_t *p = msh_ui_sheet(MSH_SHEET_CHANNEL, title);
    msh_lock();
    meshtastic_Channel c = MSH.ch[idx];
    msh_unlock();
    char psk[32];
    psk_text(&c.settings, psk, sizeof psk);
    char line[120];
    snprintf(line, sizeof line, "%s  ·  %s", idx == 0 ? _("primario") : _("secundario"), psk);
    msh_ui_label(p, line, aos_font_small, AOS_C_DIM);
    msh_ui_label(p, _("Nombre (hasta 11 letras; vacío en el primario es el del preset)"), aos_font_small, AOS_C_DIM);
    s_ch_name = field(p, _("Nombre"), c.settings.name, 11, true);
    lv_obj_t *r = msh_ui_row(p);
    msh_ui_btn(r, _("Guardar"), AOS_C_ACCENT, ch_save_cb, NULL);
    if (idx > 0) msh_ui_btn(r, _("Borrar"), AOS_C_RED, ch_delete_cb, NULL);
    sheet_kb(p, s_ch_name, LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void ch_cb(lv_event_t *e)
{
    if (MSH.state != MSH_ST_READY) { aos_ui_toast(_("El nodo no está conectado"), 1800); return; }
    ch_sheet((int)(intptr_t)lv_event_get_user_data(e));
}

static void ch_add_cb(lv_event_t *e)
{
    (void)e;
    if (MSH.state != MSH_ST_READY) { aos_ui_toast(_("El nodo no está conectado"), 1800); return; }
    int idx = -1;
    msh_lock();
    for (int i = 1; i < MSH_CHANNELS && idx < 0; i++)
        if (MSH.ch[i].role == meshtastic_Channel_Role_DISABLED) idx = i;
    msh_unlock();
    if (idx < 0) {
        aos_ui_toast(_("El nodo ya tiene los ocho canales"), 1800);
        return;
    }
    meshtastic_Channel c = meshtastic_Channel_init_zero;
    c.index = (int8_t)idx;
    c.role = meshtastic_Channel_Role_SECONDARY;
    c.has_settings = true;
    c.settings.psk.size = 32;
    aos_hal_random(c.settings.psk.bytes, 32);
    snprintf(c.settings.name, sizeof c.settings.name, "Canal%d", idx);
    if (msh_set_channel(&c)) ch_sheet(idx);
}

/* ---- sharing ---- */

static void share_cb(lv_event_t *e)
{
    (void)e;
    static char url[1200];
    if (!msh_url_make(url, sizeof url)) {
        aos_ui_toast(_("Todavía no se conocen los canales del nodo"), 1800);
        return;
    }
    lv_obj_t *p = msh_ui_sheet(MSH_SHEET_SHARE, _("Compartir los canales"));
    wrap_label(msh_ui_label(p, _("Escaneálo con la app de Meshtastic del teléfono para sumarse a estos canales."),
                            aos_font_small, AOS_C_DIM));
    int sz = UI.land ? 380 : 560;
    lv_obj_t *qr = msh_ui_row(p);
    lv_obj_set_flex_align(qr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *q = lv_qrcode_create(qr);
    lv_qrcode_set_size(q, sz);
    lv_qrcode_set_dark_color(q, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(q, lv_color_hex(0xFFFFFF));
    lv_qrcode_set_quiet_zone(q, true);
    lv_qrcode_update(q, url, (uint32_t)strlen(url));
    lv_obj_t *ul = msh_ui_label(p, url, aos_font_caption, AOS_C_DIM);
    wrap_label(ul);
    msh_ui_btn(p, _("Listo"), AOS_C_CARD2, sheet_close_cb, NULL);
}

static lv_obj_t *s_url;
static meshtastic_ChannelSet *s_cs;

static void import_apply(void)
{
    if (s_cs && msh_apply_channel_set(s_cs)) aos_ui_toast(_("Canales cambiados: el nodo se reinicia"), 2500);
    else aos_ui_toast(_("No se pudo mandar al nodo"), 1800);
}

static void import_ok_cb(lv_event_t *e)
{
    (void)e;
    if (!s_cs) s_cs = (meshtastic_ChannelSet *)msh_alloc(sizeof *s_cs);
    if (!s_cs) return;
    if (!msh_url_parse(lv_textarea_get_text(s_url), s_cs)) {
        aos_ui_toast(_("No es una URL de canales de Meshtastic"), 2000);
        return;
    }
    char text[300], names[160] = "";
    for (int i = 0; i < (int)s_cs->settings_count; i++) {
        size_t l = strlen(names);
        snprintf(names + l, sizeof names - l, "%s%s", i ? ", " : "", s_cs->settings[i].name[0] ? s_cs->settings[i].name : "LongFast");
    }
    snprintf(text, sizeof text, _("Reemplaza los canales del nodo por estos %d: %s. Si trae la radio, también. El nodo se reinicia."),
             (int)s_cs->settings_count, names);
    confirm(_("Tomar estos canales"), text, _("Reemplazar"), import_apply);
}

static void import_cb(lv_event_t *e)
{
    (void)e;
    if (MSH.state != MSH_ST_READY) { aos_ui_toast(_("El nodo no está conectado"), 1800); return; }
    lv_obj_t *p = msh_ui_sheet(MSH_SHEET_IMPORT, _("Tomar canales de una URL"));
    wrap_label(msh_ui_label(p, _("Pegá una URL https://meshtastic.org/e/#... (también se puede desde la página del portal)."),
                            aos_font_small, AOS_C_DIM));
    s_url = field(p, "https://meshtastic.org/e/#...", "", 1000, false);
    msh_ui_btn(p, _("Revisar"), AOS_C_ACCENT, import_ok_cb, NULL);
    sheet_kb(p, s_url, LV_KEYBOARD_MODE_TEXT_LOWER);
}

/* ---- the tab ---- */

static void conn_cb(lv_event_t *e)
{
    (void)e;
    msh_ui_conn_sheet();
}

static void reconnect_cb(lv_event_t *e)
{
    (void)e;
    msh_lock();
    msh_target_t t = MSH.target;
    msh_unlock();
    if (t.kind != MSH_LINK_NONE) msh_app_connect(&t);
}

static void kv(lv_obj_t *card, const char *k, const char *v)
{
    lv_obj_t *r = msh_ui_row(card);
    lv_obj_t *kl = msh_ui_label(r, k, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(kl, UI.land ? 260 : 230);
    lv_obj_t *vl = msh_ui_label(r, v, aos_font_small, AOS_C_TEXT);
    lv_obj_set_flex_grow(vl, 1);
    lv_label_set_long_mode(vl, LV_LABEL_LONG_MODE_WRAP);
}

void msh_ui_cfg_build(lv_obj_t *body)
{
    lv_obj_t *col = msh_ui_col(body);
    char line[200], v[64];
    msh_lock();
    msh_state_t st = MSH.state;
    msh_target_t tg = MSH.target;

    /* the connection */
    lv_obj_t *c = msh_ui_card(col);
    msh_ui_label(c, _("Conexión"), aos_font_body, AOS_C_TEXT);
    if (tg.kind == MSH_LINK_NONE) {
        wrap_label(msh_ui_label(c, _("Ningún nodo elegido."), aos_font_small, AOS_C_DIM));
    } else {
        msh_target_text(&tg, v, sizeof v);
        kv(c, _("Por"), v);
        const char *sts = st == MSH_ST_READY ? _("conectado") : st == MSH_ST_CONFIG ? _("recibiendo la base de nodos")
                        : st == MSH_ST_OPENING ? _("conectando") : st == MSH_ST_RETRY ? _("reintentando") : _("parado");
        kv(c, _("Estado"), sts);
        if (MSH.err[0]) kv(c, _("Último error"), _(MSH.err));
        snprintf(line, sizeof line, "%lu / %lu", (unsigned long)MSH.pkts_in, (unsigned long)MSH.pkts_out);
        kv(c, _("Paquetes (in / out)"), line);
    }
    lv_obj_t *r = msh_ui_row(c);
    msh_ui_btn(r, tg.kind == MSH_LINK_NONE ? _("Conectar") : _("Cambiar"), AOS_C_ACCENT, conn_cb, NULL);
    if (tg.kind != MSH_LINK_NONE && st != MSH_ST_READY) msh_ui_btn(r, _("Reintentar ya"), AOS_C_CARD2, reconnect_cb, NULL);
    if (MSH.log_n) {
        char log[MSH_LOG_LINES * 100] = "";
        int from = MSH.log_n > 6 ? MSH.log_n - 6 : 0;
        for (int i = from; i < MSH.log_n; i++) {
            size_t l = strlen(log);
            snprintf(log + l, sizeof log - l, "%s%.90s", i > from ? "\n" : "", MSH.log[(MSH.log_head + i) % MSH_LOG_LINES]);
        }
        lv_obj_t *ll = msh_ui_label(c, log, aos_font_caption, AOS_C_DIM);
        wrap_label(ll);
    }

    if (!MSH.my_num) {
        msh_unlock();
        return;
    }

    /* this node */
    msh_node_t *me = msh_node_find(MSH.my_num);
    c = msh_ui_card(col);
    msh_ui_label(c, _("Este nodo"), aos_font_body, AOS_C_TEXT);
    char safe[120];
    if (me) {
        snprintf(line, sizeof line, "%s (%s)", me->long_name, me->short_name);
        msh_ui_safe(safe, sizeof safe, line);
        kv(c, _("Nombre"), safe);
    }
    msh_node_id(MSH.my_num, v);
    kv(c, "ID", v);
    if (MSH.fw[0]) kv(c, _("Firmware"), MSH.fw);
    const char *hw = msh_hw_name(MSH.hw_model);
    if (hw) kv(c, _("Equipo"), hw);
    kv(c, _("Rol"), msh_role_name(MSH.have_dev ? (int)MSH.dev.role : MSH.role));
    snprintf(line, sizeof line, "%d", MSH.n_nodes);
    kv(c, _("Nodos conocidos"), line);
    if (MSH.have_bt_cfg) {
        if (MSH.bt_cfg.mode == meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN)
            snprintf(line, sizeof line, _("PIN fijo %06lu"), (unsigned long)MSH.bt_cfg.fixed_pin);
        else snprintf(line, sizeof line, "%s", MSH.bt_cfg.mode == meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN
                                                   ? _("sin PIN") : _("PIN al azar en su pantalla"));
        kv(c, "Bluetooth", MSH.bt_cfg.enabled ? line : _("apagado"));
    }
    msh_ui_btn(c, _("Cambiar el nombre"), AOS_C_CARD2, owner_cb, NULL);

    /* the radio */
    if (MSH.have_lora) {
        c = msh_ui_card(col);
        msh_ui_label(c, _("Radio LoRa"), aos_font_body, AOS_C_TEXT);
        static char regions[600], presets[300];
        regions[0] = presets[0] = 0;
        for (int i = 0; i <= 37; i++) {
            size_t l = strlen(regions);
            snprintf(regions + l, sizeof regions - l, "%s%s", i ? "\n" : "", msh_region_name(i));
        }
        for (int i = 0; i <= 16; i++) {
            size_t l = strlen(presets);
            snprintf(presets + l, sizeof presets - l, "%s%s", i ? "\n" : "", msh_preset_name(i));
        }
        msh_ui_label(c, _("Región"), aos_font_small, AOS_C_DIM);
        s_region = dropdown(c, regions, MSH.lora.region);
        msh_ui_label(c, _("Preset"), aos_font_small, AOS_C_DIM);
        s_preset = dropdown(c, presets, MSH.lora.use_preset ? MSH.lora.modem_preset : 0);
        msh_ui_label(c, _("Saltos como máximo"), aos_font_small, AOS_C_DIM);
        s_hops = dropdown(c, "1\n2\n3\n4\n5\n6\n7", MSH.lora.hop_limit ? (int)MSH.lora.hop_limit - 1 : 2);
        if (!MSH.lora.use_preset)
            wrap_label(msh_ui_label(c, _("El nodo usa una radio a medida, sin preset: guardar la pasa a un preset."),
                                    aos_font_small, AOS_C_ORANGE));
        msh_ui_btn(c, _("Guardar la radio"), AOS_C_ACCENT, lora_save_cb, NULL);
    }

    /* channels */
    c = msh_ui_card(col);
    msh_ui_label(c, _("Canales"), aos_font_body, AOS_C_TEXT);
    for (int i = 0; i < MSH_CHANNELS; i++) {
        const meshtastic_Channel *ch = &MSH.ch[i];
        if (ch->role == meshtastic_Channel_Role_DISABLED) continue;
        char name[24], psk[32];
        msh_channel_name(i, name, sizeof name);
        psk_text(&ch->settings, psk, sizeof psk);
        snprintf(line, sizeof line, "%d  ·  %s  ·  %s", i, name, psk);
        msh_ui_safe(safe, sizeof safe, line);
        lv_obj_t *b = msh_ui_btn(c, safe, AOS_C_CARD2, ch_cb, (void *)(intptr_t)i);
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_set_style_text_align(lv_obj_get_child(b, 0), LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(lv_obj_get_child(b, 0), LV_ALIGN_LEFT_MID, 0, 0);
    }
    r = msh_ui_row(c);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(r, 12, 0);
    msh_ui_btn(r, _("Agregar canal"), AOS_C_CARD2, ch_add_cb, NULL);
    msh_ui_btn(r, _("Compartir (QR)"), AOS_C_ACCENT, share_cb, NULL);
    msh_ui_btn(r, _("Tomar una URL"), AOS_C_CARD2, import_cb, NULL);
    msh_unlock();
}
