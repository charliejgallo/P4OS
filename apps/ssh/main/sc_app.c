/*
 * P4OS - SSH client: the app (LVGL's task).
 *
 *   the list    the saved computers; + adds one, the pencil edits it, a tap
 *               connects. "La clave de la placa" shows the line that goes in
 *               a computer's ~/.ssh/authorized_keys, and its QR.
 *   the editor  name, address, port, user. No password: it is asked for
 *               every time and never kept.
 *   the shell   a bar on top (back, the name, the board's key, the
 *               keyboard), the terminal under it and the keyboard at the
 *               bottom when it is up. A tap on the terminal shows or hides
 *               the keyboard; a finger dragged goes through the scrollback.
 *               A USB keyboard types into it straight away.
 *   questions   the host key (new: its fingerprint, to compare; changed:
 *               a warning) and the password, as sheets over the shell,
 *               while the session's thread waits for the answer.
 *
 * The session lives in its thread (sc_conn.c); a timer here looks at its
 * state, opens the questions, redraws the terminal's changed rows and puts
 * the session's notes in it.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_text_safe.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "sc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
void *sc_psram(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!p) p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
#else
void *sc_psram(size_t n) { return calloc(1, n); }
#endif

#define TIMER_MS 40
#define BAR_H    84

/* A window with a prompt in it: ">_" */
static const uint8_t SSH_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 0, 72, 58, 12, AIC_C_TEXT, 255),
    AIC_INTO,
    AIC_RECT(AIC_CENTER, 0, 0, 64, 50, 8, AIC_C_LIT(0x0D1117), 255),
    AIC_INTO,
    AIC_RECT(AIC_CENTER, -14, -6, 20, 6, 3, AIC_C_LIT(0x30D158), 255),
    AIC_ROT(380),
    AIC_RECT(AIC_CENTER, -14, 6, 20, 6, 3, AIC_C_LIT(0x30D158), 255),
    AIC_ROT(-380),
    AIC_RECT(AIC_CENTER, 10, 12, 20, 6, 2, AIC_C_LIT(0x30D158), 255),
    AIC_OUT,
    AIC_OUT,
    AIC_END
};

enum { SCR_LIST = 0, SCR_EDIT, SCR_TERM };

static struct {
    aos_app_t *self;
    lv_obj_t *root;
    int32_t W, H;
    bool land;
    int screen;
    sc_hosts_t hosts;
    lv_timer_t *timer;
    lv_obj_t *keysheet;
    bool typed;                 /* SSHC_TYPE done */

    struct {
        int index;
        sc_host_t h;
        lv_obj_t *form, *name, *host, *port, *user, *kb, *del_btn;
        uint64_t del_armed;
    } ed;

    struct {
        bool on, leave;
        sc_host_t h;
        sc_term_t *term;
        sc_sess_t *sess;
        sc_view_t *view;
        sc_kbd_t *kbd;
        bool kbd_on;
        lv_obj_t *bar, *title, *keybtn, *msg, *msg_btns;
        lv_obj_t *sheet, *pw_ta, *pw_kb;
        int shown;              /* the sc_state_t the UI was last built for, -1 none */
        uint32_t ask_seq;       /* the question it was built for */
        uint32_t bells;
        int cols, rows;
    } tm;
} SC;

static void build_ui(void);
static void open_term(int index);
static void close_term(void);

/* ---- small pieces ---- */

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, AOS_UI_TAP_MIN, AOS_UI_TAP_MIN);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x48484A), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = aos_label(b, glyph, &aos_sym_44, AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    return b;
}

static lv_obj_t *wrap_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t c)
{
    lv_obj_t *l = aos_label(parent, text, font, c);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    aos_make_decorative(l);
    return l;
}

static void set_text(lv_obj_t *l, const char *t)
{
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, t)) lv_label_set_text(l, t);
}

/* a card over everything in the root, w wide at most, with a column inside */
static lv_obj_t *sheet_open(lv_obj_t **holder, int32_t maxw)
{
    lv_obj_t *dim = box(SC.root, SC.W, SC.H);
    lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_70, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    *holder = dim;
    lv_obj_t *card = box(dim, LV_MIN(SC.W - 48, maxw), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(card, 32, 0);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    return card;
}

static lv_obj_t *button_row(lv_obj_t *card)
{
    lv_obj_t *r = box(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(r, 16, 0);
    lv_obj_set_style_margin_top(r, 10, 0);
    return r;
}

/* ---- the board's key ---- */

static void keysheet_close_cb(lv_event_t *e)
{
    (void)e;
    if (SC.keysheet) lv_obj_delete_async(SC.keysheet);
    SC.keysheet = NULL;
}

static void keysheet_cb(lv_event_t *e)
{
    (void)e;
    char line[200];
    if (!sc_key_line(line, sizeof line)) {
        aos_ui_toast(_("No se pudo hacer la clave"), 2000);
        return;
    }
    lv_obj_t *card = sheet_open(&SC.keysheet, 660);
    wrap_label(card, _("La clave de la placa"), aos_font_title, AOS_C_TEXT);
    wrap_label(card, _("Para entrar sin contraseña, esta línea va en ~/.ssh/authorized_keys del equipo. Lo más fácil: entrá una vez con la contraseña y tocá el candado de arriba."),
               aos_font_small, AOS_C_DIM);
    lv_obj_t *k = wrap_label(card, line, &aos_mono_18, AOS_C_GREEN);
    (void)k;
    int32_t qs = SC.land ? 220 : 300;
    lv_obj_t *qrow = box(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(qrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(qrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *qr = lv_qrcode_create(qrow);
    lv_qrcode_set_size(qr, qs);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, line, (uint32_t)strlen(line));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 10, 0);
    const char *root = aos_hal_path_sd_root(), *data = aos_hal_path_data();
    size_t rl = root ? strlen(root) : 0;
    char hint[200];
    snprintf(hint, sizeof hint, _("Con el SSH de la placa, desde la Mac: ssh %s.local cat %s/ssh/id_ed25519.pub"),
             aos_hal_device_name(), rl && !strncmp(data, root, rl) ? data + rl : data);
    wrap_label(card, hint, aos_font_caption, AOS_C_DIM);
    lv_obj_t *r = button_row(card);
    aos_button(r, _("Listo"), AOS_C_ACCENT, keysheet_close_cb, NULL);
}

/* ---- the list ---- */

static void connect_cb(lv_event_t *e) { open_term((int)(intptr_t)lv_event_get_user_data(e)); }

static void open_editor(int index);

static void edit_cb(lv_event_t *e) { open_editor((int)(intptr_t)lv_event_get_user_data(e)); }

static void add_cb(lv_event_t *e)
{
    (void)e;
    if (SC.hosts.count >= SC_MAX_HOSTS) {
        aos_ui_toast(_("No entran más equipos"), 2000);
        return;
    }
    open_editor(-1);
}

static void build_list(void)
{
    lv_obj_t *page = box(SC.root, SC.W, SC.H);
    lv_obj_set_style_bg_color(page, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);

    lv_obj_t *title = aos_label(page, "SSH", aos_font_title, AOS_C_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 36);
    aos_make_decorative(title);
    lv_obj_t *add = round_btn(page, AOS_SYM_PLUS, add_cb, NULL);
    lv_obj_align(add, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, 22);
    lv_obj_t *key = round_btn(page, AOS_SYM_LOCK, keysheet_cb, NULL);
    lv_obj_align(key, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD - AOS_UI_TAP_MIN - 16, 22);

    const int32_t top = 130;
    lv_obj_t *list = box(page, SC.W, SC.H - top);
    lv_obj_set_pos(list, 0, top);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(list, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_bottom(list, 40, 0);
    lv_obj_set_style_pad_gap(list, 16, 0);

    int cols = SC.land ? 2 : 1;
    int32_t cw = (SC.W - 2 * AOS_UI_PAD - (cols - 1) * 16) / cols;
    for (int i = 0; i < SC.hosts.count; i++) {
        const sc_host_t *h = &SC.hosts.h[i];
        lv_obj_t *card = box(list, cw, 132);
        lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(card, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_radius(card, AOS_UI_RADIUS, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, connect_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *ic = aos_label(card, AOS_SYM_APPLICATION_BRACKETS, &aos_sym_44, AOS_C_GREEN);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 24, 0);
        aos_make_decorative(ic);

        char safe[96];
        aos_text_safe(safe, sizeof safe, h->name);
        lv_obj_t *nm = aos_label(card, safe, aos_font_body, AOS_C_TEXT);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(nm, cw - 96 - 110, lv_font_get_line_height(aos_font_body));
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 92, 26);
        aos_make_decorative(nm);

        char where[200];
        if (h->port == 22) snprintf(where, sizeof where, "%s@%s", h->user, h->host);
        else snprintf(where, sizeof where, "%s@%s:%d", h->user, h->host, h->port);
        lv_obj_t *hl = aos_label(card, where, aos_font_caption, AOS_C_DIM);
        lv_label_set_long_mode(hl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(hl, cw - 96 - 110, lv_font_get_line_height(aos_font_caption));
        lv_obj_align(hl, LV_ALIGN_TOP_LEFT, 92, 76);
        aos_make_decorative(hl);

        lv_obj_t *ed = round_btn(card, AOS_SYM_PENCIL, edit_cb, (void *)(intptr_t)i);
        lv_obj_align(ed, LV_ALIGN_RIGHT_MID, -20, 0);
    }
    if (!SC.hosts.count) {
        lv_obj_t *l = aos_label(list, _("Ningún equipo todavía: agregalo con +.\n\nEn la Mac: Ajustes del Sistema, General, Compartir, «Sesión remota». En una Raspberry Pi: sudo raspi-config, Interface Options, SSH."),
                                aos_font_body, AOS_C_DIM);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, SC.W - 2 * AOS_UI_PAD);
        aos_make_decorative(l);
    }
}

/* ---- the editor ---- */

static void ed_kb_show(lv_obj_t *ta)
{
    if (!SC.ed.kb) return;
    if (ta) {
        lv_keyboard_set_textarea(SC.ed.kb, ta);
        lv_keyboard_set_mode(SC.ed.kb, ta == SC.ed.port ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_remove_flag(SC.ed.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(SC.ed.form, SC.H - lv_obj_get_height(SC.ed.kb));
        lv_obj_scroll_to_view(ta, LV_ANIM_ON);
    } else {
        lv_obj_add_flag(SC.ed.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(SC.ed.form, SC.H);
    }
}

static void ta_focus_cb(lv_event_t *e) { ed_kb_show(lv_event_get_target(e)); }

static void ed_kb_cb(lv_event_t *e)
{
    (void)e;
    ed_kb_show(NULL);
}

static lv_obj_t *field(lv_obj_t *form, const char *label, const char *value, int max)
{
    lv_obj_t *l = aos_label(form, label, aos_font_caption, AOS_C_DIM);
    aos_make_decorative(l);
    lv_obj_t *ta = lv_textarea_create(form);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (uint32_t)max);
    lv_textarea_set_text(ta, value);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_style_text_font(ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 20, 0);
    lv_obj_set_style_pad_hor(ta, 24, 0);
    lv_obj_set_style_pad_ver(ta, 16, 0);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void ed_take(sc_host_t *h)
{
    sc_copy(h->name, sizeof h->name, lv_textarea_get_text(SC.ed.name));
    sc_copy(h->host, sizeof h->host, lv_textarea_get_text(SC.ed.host));
    sc_copy(h->user, sizeof h->user, lv_textarea_get_text(SC.ed.user));
    h->port = atoi(lv_textarea_get_text(SC.ed.port));
}

static void ed_close(void)
{
    SC.screen = SCR_LIST;
    build_ui();
}

static void ed_back_cb(lv_event_t *e)
{
    (void)e;
    ed_close();
}

/* no tabs (the file's separator) and no spaces around */
static void trim(char *s)
{
    for (char *p = s; *p; p++)
        if (*p == '\t') *p = ' ';
    char *a = s;
    while (*a == ' ') a++;
    memmove(s, a, strlen(a) + 1);
    for (char *p = s + strlen(s); p > s && p[-1] == ' ';) *--p = 0;
}

static void ed_save_cb(lv_event_t *e)
{
    (void)e;
    sc_host_t h;
    ed_take(&h);
    trim(h.name);
    trim(h.host);
    trim(h.user);
    if (!h.host[0]) {
        aos_ui_toast(_("Falta la dirección"), 2000);
        return;
    }
    if (!h.user[0]) {
        aos_ui_toast(_("Falta el usuario"), 2000);
        return;
    }
    if (h.port <= 0 || h.port > 65535) h.port = 22;
    if (!h.name[0]) sc_copy(h.name, sizeof h.name, h.host);
    if (SC.ed.index < 0) {
        if (SC.hosts.count >= SC_MAX_HOSTS) return;
        SC.hosts.h[SC.hosts.count++] = h;
    } else {
        SC.hosts.h[SC.ed.index] = h;
    }
    if (!sc_hosts_save(&SC.hosts)) {
        aos_ui_toast(_("No se pudo guardar en la tarjeta"), 2500);
        return;
    }
    ed_close();
}

static void ed_del_cb(lv_event_t *e)
{
    (void)e;
    uint64_t now = aos_hal_uptime_ms();
    if (now - SC.ed.del_armed > 3000) {
        SC.ed.del_armed = now;
        lv_obj_t *l = lv_obj_get_child(SC.ed.del_btn, 0);
        if (l) lv_label_set_text(l, _("Tocá otra vez para borrar"));
        return;
    }
    sc_known_forget(SC.hosts.h[SC.ed.index].host, SC.hosts.h[SC.ed.index].port);
    for (int i = SC.ed.index; i < SC.hosts.count - 1; i++) SC.hosts.h[i] = SC.hosts.h[i + 1];
    SC.hosts.count--;
    sc_hosts_save(&SC.hosts);
    ed_close();
}

static void build_editor(void)
{
    lv_obj_t *page = box(SC.root, SC.W, SC.H);
    lv_obj_set_style_bg_color(page, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_t *form = box(page, SC.W, SC.H);
    SC.ed.form = form;
    lv_obj_add_flag(form, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(form, LV_DIR_VER);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    int32_t side = SC.land ? SC.W / 6 : AOS_UI_PAD;
    lv_obj_set_style_pad_hor(form, side, 0);
    lv_obj_set_style_pad_top(form, 130, 0);
    lv_obj_set_style_pad_bottom(form, 60, 0);
    lv_obj_set_style_pad_row(form, 10, 0);

    lv_obj_t *bk = round_btn(page, AOS_SYM_CHEVRON_LEFT, ed_back_cb, NULL);
    lv_obj_align(bk, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 22);
    lv_obj_t *t = aos_label(page, SC.ed.index < 0 ? _("Equipo nuevo") : _("Equipo"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, AOS_UI_PAD + AOS_UI_TAP_MIN + 20, 36);
    aos_make_decorative(t);

    char port[8];
    snprintf(port, sizeof port, "%d", SC.ed.h.port);
    SC.ed.name = field(form, _("Nombre"), SC.ed.h.name, sizeof SC.ed.h.name - 1);
    SC.ed.host = field(form, _("Dirección (IP o nombre)"), SC.ed.h.host, sizeof SC.ed.h.host - 1);
    lv_textarea_set_placeholder_text(SC.ed.host, "raspberrypi.local");
    SC.ed.port = field(form, _("Puerto"), port, 5);
    lv_textarea_set_accepted_chars(SC.ed.port, "0123456789");
    SC.ed.user = field(form, _("Usuario"), SC.ed.h.user, sizeof SC.ed.h.user - 1);
    lv_textarea_set_placeholder_text(SC.ed.user, "pi");
    lv_obj_t *n = wrap_label(form, _("La contraseña no se guarda: se pide al conectar. Con la clave de la placa instalada en el equipo no hace falta."),
                             aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_margin_top(n, 8, 0);

    lv_obj_t *btns = box(form, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(btns, 12, 0);
    lv_obj_set_style_margin_top(btns, 20, 0);
    aos_button(btns, _("Guardar"), AOS_C_ACCENT, ed_save_cb, NULL);
    SC.ed.del_btn = SC.ed.index >= 0 ? aos_button(btns, _("Borrar"), AOS_C_RED, ed_del_cb, NULL) : NULL;

    SC.ed.kb = lv_keyboard_create(page);
    aos_keyboard_style(SC.ed.kb, aos_font_body);
    lv_obj_set_size(SC.ed.kb, SC.W, SC.land ? SC.H * 45 / 100 : SC.H * 34 / 100);
    lv_obj_align(SC.ed.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(SC.ed.kb, ed_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(SC.ed.kb, ed_kb_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(SC.ed.kb, LV_OBJ_FLAG_HIDDEN);
}

static void open_editor(int index)
{
    memset(&SC.ed, 0, sizeof SC.ed);
    SC.ed.index = index;
    if (index >= 0 && index < SC.hosts.count) SC.ed.h = SC.hosts.h[index];
    else {
        SC.ed.index = -1;
        SC.ed.h.port = 22;
    }
    SC.screen = SCR_EDIT;
    build_ui();
}

/* ---- the shell: keys ---- */

static void send_key(void *user, uint32_t key, uint8_t mods)
{
    (void)user;
    if (!SC.tm.sess) return;
    char b[16];
    sc_term_lock(SC.tm.term);
    bool app = sc_term_app_cursor(SC.tm.term);
    sc_term_unlock(SC.tm.term);
    size_t n = sc_key_bytes(key, mods, app, b);
    if (n) sc_sess_send(SC.tm.sess, b, n);
    sc_view_to_bottom(SC.tm.view);
}

static bool hwkey_cb(uint32_t key, uint8_t mods)
{
    if (!SC.tm.on || SC.tm.sheet) return false;
    uint32_t k;
    uint8_t m;
    if (!sc_hwkey(key, mods, &k, &m)) return false;
    send_key(NULL, k, m);
    return true;
}

/* ---- the shell: layout ---- */

static void term_layout(void);

static void kbd_hide(void *user)
{
    (void)user;
    SC.tm.kbd_on = false;
    term_layout();
}

static void tap_cb(void *user)
{
    (void)user;
    SC.tm.kbd_on = !SC.tm.kbd_on;
    term_layout();
}

static void kbd_btn_cb(lv_event_t *e)
{
    (void)e;
    tap_cb(NULL);
}

static void term_layout(void)
{
    if (SC.tm.kbd) {
        sc_kbd_delete(SC.tm.kbd);
        SC.tm.kbd = NULL;
    }
    int32_t kh = 0;
    if (SC.tm.kbd_on) {
        SC.tm.kbd = sc_kbd_create(SC.root, SC.W, SC.land, send_key, kbd_hide, NULL);
        kh = sc_kbd_height(SC.tm.kbd);
    }
    lv_obj_t *vo = sc_view_obj(SC.tm.view);
    lv_obj_set_pos(vo, 0, BAR_H);
    int cols, rows;
    sc_view_set_size(SC.tm.view, SC.W, SC.H - BAR_H - kh, &cols, &rows);
    if (cols != SC.tm.cols || rows != SC.tm.rows) {
        SC.tm.cols = cols;
        SC.tm.rows = rows;
        sc_term_lock(SC.tm.term);
        sc_term_resize(SC.tm.term, cols, rows);
        sc_term_unlock(SC.tm.term);
        if (SC.tm.sess) sc_sess_resize(SC.tm.sess, cols, rows);
    }
    if (SC.tm.msg) lv_obj_align(SC.tm.msg, LV_ALIGN_TOP_MID, 0, BAR_H + 80);
}

static void leave_cb(lv_event_t *e)
{
    (void)e;
    SC.tm.leave = true;
}

static void install_cb(lv_event_t *e)
{
    (void)e;
    if (SC.tm.sess) sc_sess_install_key(SC.tm.sess);
    if (SC.tm.keybtn) lv_obj_add_flag(SC.tm.keybtn, LV_OBJ_FLAG_HIDDEN);
}

static void retry_cb(lv_event_t *e)
{
    (void)e;
    int idx = -1;
    for (int i = 0; i < SC.hosts.count; i++)
        if (!strcmp(SC.hosts.h[i].host, SC.tm.h.host) && SC.hosts.h[i].port == SC.tm.h.port &&
            !strcmp(SC.hosts.h[i].user, SC.tm.h.user))
            idx = i;
    close_term();
    if (idx >= 0) open_term(idx);
    else {
        SC.screen = SCR_LIST;
        build_ui();
    }
}

static void build_term(void)
{
    lv_obj_t *page = box(SC.root, SC.W, SC.H);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);

    SC.tm.bar = box(page, SC.W, BAR_H);
    lv_obj_set_style_bg_color(SC.tm.bar, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_bg_opa(SC.tm.bar, LV_OPA_COVER, 0);
    lv_obj_t *bk = round_btn(SC.tm.bar, AOS_SYM_CHEVRON_LEFT, leave_cb, NULL);
    lv_obj_set_size(bk, 68, 68);
    lv_obj_align(bk, LV_ALIGN_LEFT_MID, 12, 0);
    char safe[96];
    aos_text_safe(safe, sizeof safe, SC.tm.h.name);
    SC.tm.title = aos_label(SC.tm.bar, safe, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(SC.tm.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(SC.tm.title, SC.W - 2 * 92 - 100, lv_font_get_line_height(aos_font_body));
    lv_obj_align(SC.tm.title, LV_ALIGN_LEFT_MID, 96, 0);
    aos_make_decorative(SC.tm.title);
    lv_obj_t *kb = round_btn(SC.tm.bar, AOS_SYM_KEYBOARD, kbd_btn_cb, NULL);
    lv_obj_set_size(kb, 68, 68);
    lv_obj_align(kb, LV_ALIGN_RIGHT_MID, -12, 0);
    SC.tm.keybtn = round_btn(SC.tm.bar, AOS_SYM_LOCK, install_cb, NULL);
    lv_obj_set_size(SC.tm.keybtn, 68, 68);
    lv_obj_align(SC.tm.keybtn, LV_ALIGN_RIGHT_MID, -12 - 68 - 12, 0);
    lv_obj_add_flag(SC.tm.keybtn, LV_OBJ_FLAG_HIDDEN);

    SC.tm.view = sc_view_create(page, SC.tm.term);
    sc_view_on_tap(SC.tm.view, tap_cb, NULL);

    SC.tm.msg = box(page, SC.W - 2 * AOS_UI_PAD, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(SC.tm.msg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(SC.tm.msg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(SC.tm.msg, 28, 0);
    lv_obj_set_style_pad_row(SC.tm.msg, 18, 0);
    lv_obj_set_style_bg_color(SC.tm.msg, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(SC.tm.msg, LV_OPA_90, 0);
    lv_obj_set_style_radius(SC.tm.msg, AOS_UI_RADIUS, 0);
    lv_obj_t *ml = aos_label(SC.tm.msg, "", aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(ml, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(ml, lv_pct(100));
    lv_obj_set_style_text_align(ml, LV_TEXT_ALIGN_CENTER, 0);
    SC.tm.msg_btns = box(SC.tm.msg, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(SC.tm.msg_btns, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(SC.tm.msg_btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(SC.tm.msg_btns, 16, 0);
    aos_button(SC.tm.msg_btns, _("Volver"), AOS_C_CARD2, leave_cb, NULL);
    aos_button(SC.tm.msg_btns, _("Reconectar"), AOS_C_ACCENT, retry_cb, NULL);
    lv_obj_add_flag(SC.tm.msg_btns, LV_OBJ_FLAG_HIDDEN);

    SC.tm.cols = SC.tm.rows = 0;
    term_layout();
}

static void show_msg(const char *text, bool buttons)
{
    if (!SC.tm.msg) return;
    if (!text) {
        lv_obj_add_flag(SC.tm.msg, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(SC.tm.msg, LV_OBJ_FLAG_HIDDEN);
    set_text(lv_obj_get_child(SC.tm.msg, 0), text);
    if (buttons) lv_obj_remove_flag(SC.tm.msg_btns, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(SC.tm.msg_btns, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(SC.tm.msg);
}

/* ---- the shell: the questions ---- */

static void sheet_close(void)
{
    if (SC.tm.sheet) lv_obj_delete(SC.tm.sheet);
    SC.tm.sheet = NULL;
    SC.tm.pw_ta = SC.tm.pw_kb = NULL;
}

static void hk_cb(lv_event_t *e)
{
    bool yes = lv_event_get_user_data(e) != NULL;
    if (SC.tm.sess) sc_sess_answer_hostkey(SC.tm.sess, yes);
    lv_obj_t *s = SC.tm.sheet;
    SC.tm.sheet = NULL;
    if (s) lv_obj_delete_async(s);      /* from its own button's event */
}

static void ask_hostkey(void)
{
    sc_hostkey_q_t q;
    sc_sess_hostkey(SC.tm.sess, &q);
    lv_obj_t *card = sheet_open(&SC.tm.sheet, 640);
    bool changed = q.known < 0;
    wrap_label(card, changed ? _("¡La clave de este equipo cambió!") : _("Primera vez con este equipo"), aos_font_title,
               changed ? AOS_C_RED : AOS_C_TEXT);
    char where[160];
    snprintf(where, sizeof where, "%s:%d · %s", SC.tm.h.host, SC.tm.h.port, q.type);
    wrap_label(card, where, aos_font_small, AOS_C_DIM);
    wrap_label(card, q.fp, &aos_mono_18, AOS_C_GREEN);
    wrap_label(card, changed ? _("Puede que lo hayan reinstalado, o que alguien se haya puesto en el medio. Si no sabés por qué cambió, no te conectes.")
                             : _("Para estar seguro, compará la huella con la del equipo: en él, ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub. Se recuerda para la próxima."),
               aos_font_small, AOS_C_DIM);
    lv_obj_t *r = button_row(card);
    aos_button(r, _("Cancelar"), AOS_C_CARD2, hk_cb, NULL);
    aos_button(r, changed ? _("Aceptar la nueva") : _("Conectar"), changed ? AOS_C_RED : AOS_C_ACCENT, hk_cb, (void *)1);
}

static void pw_done(bool ok)
{
    if (SC.tm.sess) sc_sess_answer_password(SC.tm.sess, ok ? lv_textarea_get_text(SC.tm.pw_ta) : NULL);
    if (SC.tm.pw_ta) lv_textarea_set_text(SC.tm.pw_ta, "");
    lv_obj_t *s = SC.tm.sheet;
    SC.tm.sheet = NULL;
    SC.tm.pw_ta = SC.tm.pw_kb = NULL;
    if (s) lv_obj_delete_async(s);
}

static void pw_btn_cb(lv_event_t *e) { pw_done(lv_event_get_user_data(e) != NULL); }

static void pw_kb_cb(lv_event_t *e) { pw_done(lv_event_get_code(e) == LV_EVENT_READY); }

static void ask_password(void)
{
    char prompt[96];
    bool retry = false;
    sc_sess_password_prompt(SC.tm.sess, prompt, sizeof prompt, &retry);
    lv_obj_t *dim = box(SC.root, SC.W, SC.H);
    SC.tm.sheet = dim;
    lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_80, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    int32_t kh = SC.land ? SC.H * 45 / 100 : SC.H * 34 / 100;
    lv_obj_t *col = box(dim, LV_MIN(SC.W - 48, 640), SC.H - kh - 40);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 14, 0);
    char title[160];
    if (strchr(prompt, '@')) snprintf(title, sizeof title, _("Contraseña de %s"), prompt);
    else sc_copy(title, sizeof title, prompt);
    wrap_label(col, title, aos_font_body, AOS_C_TEXT);
    if (retry) wrap_label(col, _("No anduvo: probá de nuevo."), aos_font_small, AOS_C_RED);
    SC.tm.pw_ta = lv_textarea_create(col);
    lv_textarea_set_one_line(SC.tm.pw_ta, true);
    lv_textarea_set_password_mode(SC.tm.pw_ta, true);
    lv_textarea_set_max_length(SC.tm.pw_ta, 120);
    lv_obj_set_width(SC.tm.pw_ta, lv_pct(100));
    lv_obj_set_style_text_font(SC.tm.pw_ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(SC.tm.pw_ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(SC.tm.pw_ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(SC.tm.pw_ta, 0, 0);
    lv_obj_set_style_radius(SC.tm.pw_ta, 20, 0);
    lv_obj_set_style_pad_hor(SC.tm.pw_ta, 24, 0);
    lv_obj_set_style_pad_ver(SC.tm.pw_ta, 16, 0);
    lv_obj_t *r = button_row(col);
    aos_button(r, _("Cancelar"), AOS_C_CARD2, pw_btn_cb, NULL);
    aos_button(r, _("Entrar"), AOS_C_ACCENT, pw_btn_cb, (void *)1);
    SC.tm.pw_kb = lv_keyboard_create(dim);
    aos_keyboard_style(SC.tm.pw_kb, aos_font_body);
    lv_obj_set_size(SC.tm.pw_kb, SC.W, kh);
    lv_obj_align(SC.tm.pw_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(SC.tm.pw_kb, SC.tm.pw_ta);
    lv_obj_add_event_cb(SC.tm.pw_kb, pw_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(SC.tm.pw_kb, pw_kb_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_state(SC.tm.pw_ta, LV_STATE_FOCUSED);
}

/* ---- the shell: open and close ---- */

static void open_term(int index)
{
    if (index < 0 || index >= SC.hosts.count) return;
    memset(&SC.tm, 0, sizeof SC.tm);
    SC.tm.on = true;
    SC.tm.h = SC.hosts.h[index];
    SC.tm.shown = -1;
    char kbds[1][48];
    SC.tm.kbd_on = aos_hal_usb_kbd_list(kbds, 1) == 0;     /* with a USB keyboard, the screen's stays down */
    SC.screen = SCR_TERM;
    SC.tm.term = sc_term_new(80, 24);
    if (!SC.tm.term) {
        aos_ui_toast(_("No hay memoria para la terminal"), 2000);
        SC.tm.on = false;
        SC.screen = SCR_LIST;
        return;
    }
    lv_obj_clean(SC.root);
    build_term();
    SC.tm.sess = sc_sess_start(&SC.tm.h, SC.tm.term);
    if (!SC.tm.sess) show_msg(_("No se pudo empezar la conexión"), true);
    aos_ui_set_busy(SC.self->desc.id, SC.tm.sess != NULL);
    aos_ui_hwkbd_handler(hwkey_cb);
    aos_hal_net_low_latency(true);
    aos_hal_log(SC_TAG, "connecting to %s (%s@%s:%d)", SC.tm.h.name, SC.tm.h.user, SC.tm.h.host, SC.tm.h.port);
}

static void close_term(void)
{
    if (!SC.tm.on) return;
    aos_ui_hwkbd_handler(NULL);
    sc_sess_stop(SC.tm.sess);           /* the thread out first: it feeds the terminal */
    SC.tm.sess = NULL;
    sheet_close();
    if (SC.tm.kbd) sc_kbd_delete(SC.tm.kbd);
    if (SC.tm.view) sc_view_delete(SC.tm.view);
    sc_term_free(SC.tm.term);
    aos_ui_set_busy(SC.self->desc.id, false);
    aos_hal_net_low_latency(false);
    memset(&SC.tm, 0, sizeof SC.tm);
}

/* ---- the timer ---- */

static void term_frame(void)
{
    sc_sess_t *s = SC.tm.sess;
    if (SC.tm.leave) {
        close_term();
        SC.screen = SCR_LIST;
        build_ui();
        return;
    }
    if (!s) return;
    char detail[200];
    sc_state_t st = sc_sess_state(s, detail, sizeof detail);
    uint32_t seq = sc_sess_ask_seq(s);
    if ((int)st != SC.tm.shown || seq != SC.tm.ask_seq) {
        SC.tm.ask_seq = seq;
        sc_state_t was = (sc_state_t)SC.tm.shown;
        SC.tm.shown = (int)st;
        if (was == SC_ST_ASK_HOSTKEY || was == SC_ST_ASK_PASSWORD || SC.tm.sheet) sheet_close();
        switch (st) {
        case SC_ST_CONNECTING: {
            char t[160];
            snprintf(t, sizeof t, _("Conectando con %s…"), SC.tm.h.host);
            show_msg(t, false);
            break;
        }
        case SC_ST_ASK_HOSTKEY:
            show_msg(NULL, false);
            ask_hostkey();
            break;
        case SC_ST_ASK_PASSWORD:
            show_msg(NULL, false);
            ask_password();
            break;
        case SC_ST_LIVE: {
            show_msg(NULL, false);
            if (!sc_sess_key_used(s)) lv_obj_remove_flag(SC.tm.keybtn, LV_OBJ_FLAG_HIDDEN);
            /* Development: SSHC_TYPE="ls\n" is typed once the shell is up */
            const char *ty = getenv("SSHC_TYPE");
            if (ty && !SC.typed) {
                SC.typed = true;
                for (const char *p = ty; *p; p++) {
                    if (*p == '\\' && p[1] == 'n') {
                        sc_sess_send(s, "\r", 1);
                        p++;
                    } else sc_sess_send(s, p, 1);
                }
            }
            break;
        }
        case SC_ST_CLOSED:
            sheet_close();
            show_msg(detail, true);
            aos_ui_set_busy(SC.self->desc.id, false);
            break;
        }
    }
    const char *note = sc_sess_take_note(s);
    if (note) {
        char line[300];
        snprintf(line, sizeof line, "\r\n\x1b[1;33m[P4OS] %s\x1b[0m\r\n", _(note));
        sc_term_lock(SC.tm.term);
        sc_term_feed(SC.tm.term, (const uint8_t *)line, strlen(line));
        sc_term_unlock(SC.tm.term);
    }
    sc_term_lock(SC.tm.term);
    uint32_t bells = sc_term_bells(SC.tm.term);
    sc_term_unlock(SC.tm.term);
    if (bells != SC.tm.bells) {
        SC.tm.bells = bells;
        aos_hal_beep(880, 40);
    }
    sc_view_refresh(SC.tm.view);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (SC.screen == SCR_TERM) term_frame();
}

/* ---- the app ---- */

static void build_ui(void)
{
    if (SC.keysheet) {
        lv_obj_delete(SC.keysheet);
        SC.keysheet = NULL;
    }
    lv_obj_clean(SC.root);
    SC.W = lv_obj_get_width(SC.root);
    SC.H = lv_obj_get_height(SC.root);
    SC.land = SC.W > SC.H;
    if (SC.screen == SCR_EDIT) build_editor();
    else if (SC.screen == SCR_TERM) build_term();
    else build_list();
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (SC.keysheet) {
        lv_obj_delete(SC.keysheet);
        SC.keysheet = NULL;
        return true;
    }
    if (SC.screen == SCR_TERM) {
        if (SC.tm.sheet && SC.tm.sess) {
            if (SC.tm.shown == SC_ST_ASK_HOSTKEY) sc_sess_answer_hostkey(SC.tm.sess, false);
            else sc_sess_answer_password(SC.tm.sess, NULL);
            sheet_close();
            return true;
        }
        SC.tm.leave = true;
        return true;
    }
    if (SC.screen == SCR_EDIT) {
        if (SC.ed.kb && !lv_obj_has_flag(SC.ed.kb, LV_OBJ_FLAG_HIDDEN)) ed_kb_show(NULL);
        else ed_close();
        return true;
    }
    return false;
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    SC.root = root;
    if (SC.screen == SCR_TERM) {
        /* the session and the terminal carry on; a question on screen is
         * asked again by the timer */
        sheet_close();
        if (SC.tm.kbd) sc_kbd_delete(SC.tm.kbd);
        if (SC.tm.view) sc_view_delete(SC.tm.view);
        SC.tm.kbd = NULL;
        SC.tm.view = NULL;
        lv_obj_clean(SC.root);
        SC.W = lv_obj_get_width(root);
        SC.H = lv_obj_get_height(root);
        SC.land = SC.W > SC.H;
        build_term();
        SC.tm.shown = -1;
        return true;
    }
    if (SC.screen == SCR_EDIT) ed_take(&SC.ed.h);
    build_ui();
    return true;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&SC, 0, sizeof SC);
    SC.self = self;
    SC.root = root;
    sc_hosts_load(&SC.hosts);
    SC.screen = SCR_LIST;
    build_ui();
    SC.timer = lv_timer_create(timer_cb, TIMER_MS, NULL);
    /* Development: SSHC_OPEN=<n> connects to the n-th computer at once */
    const char *open = getenv("SSHC_OPEN");
    if (open && open[0] >= '0' && open[0] <= '9') open_term(open[0] - '0');
    return &SC;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (SC.timer) {
        lv_timer_delete(SC.timer);
        SC.timer = NULL;
    }
    close_term();
    lv_obj_clean(SC.root);
}

static bool sc_init(aos_app_t *app)
{
    app->desc.id       = "aos.ssh";
    app->desc.name     = "SSH";
    app->desc.icon     = AOS_SYM_APPLICATION_BRACKETS;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x2F3640;
    app->desc.color_b  = 0x0D1117;
    app->desc.order    = 129;
    app->desc.flags    = AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_KEEP;
    aos_icon_set_ops(app, SSH_ICON, sizeof SSH_ICON);
    app->create  = create;
    app->destroy = destroy;
    app->back    = back;
    app->resize  = resize;
    return true;
}

AOS_APP_ENTRY(sc_init);
