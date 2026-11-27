/*
 * P4OS - The Bot app: the settings of the board's Telegram bot
 * (aos_bot.c). The bot itself runs as a service, with this app closed.
 *
 * One page, top to bottom: the switch and how it is doing, the token, the
 * chats linked (and the code to link one more), the commands on and off, the
 * rules and the last things it did. A rule is edited on a sheet of its own,
 * with drop-downs; a text (the token, a message) on a keyboard over it.
 */
#include "aos_app.h"
#include "aos_bot.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "aos_zigbee.h"
#include "aos_fonts.h"
#include "aos_sys_glyphs.h"
#include "aos_apps.h"

#include <stdio.h>
#if !defined(AOS_SIM)
#include "esp_heap_caps.h"
#endif
#include <stdlib.h>
#include <string.h>

#define C_BOT lv_color_hex(0x2AABEE)

static struct {
    lv_obj_t *root, *page, *sheet, *overlay, *ta, *kb;
    lv_timer_t *timer;
    int32_t W, H;
    uint32_t seen;
    int32_t scroll;
    /* the rule on the sheet */
    int edit;                       /* its index, -1 new */
    aos_bot_rule_t r;
    void (*text_done)(const char *);
    /* the Zigbee devices on the sheet's drop-downs */
    int nzb, nout;
    uint8_t zb_ieee[AOS_ZB_DEVICES_MAX][8], out_ieee[AOS_ZB_DEVICES_MAX][8];
    int nir;
    aos_zigbee_ir_code_t ir[24];
} U;

static aos_zigbee_dev_t *s_zb;      /* AOS_ZB_DEVICES_MAX, PSRAM, while the app is open */

static void build_page(void);
static void sheet_build(void);

/* ------------------------------------------------------------------------ */
/* Little pieces                                                             */
/* ------------------------------------------------------------------------ */

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent)
{
    lv_obj_t *c = box(parent, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 22, 0);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    return c;
}

static lv_obj_t *section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_small, AOS_C_DIM);
    lv_obj_set_style_pad_top(l, 18, 0);
    return l;
}

static lv_obj_t *wrap_label(lv_obj_t *parent, const char *text, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = aos_label(parent, text, f, c);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, LV_SIZE_CONTENT, 76);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 38, 0);
    lv_obj_set_style_pad_hor(b, 28, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(aos_label(b, text, aos_font_body, AOS_C_TEXT));
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *r = box(parent, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 14, 0);
    return r;
}

static lv_obj_t *switch_(lv_obj_t *parent, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 110, 60);
    lv_obj_set_style_bg_color(sw, C_BOT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_state(sw, LV_STATE_CHECKED, on);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, ud);
    return sw;
}

static lv_obj_t *dropdown(lv_obj_t *parent, const char *opts, int sel, lv_event_cb_t cb)
{
    lv_obj_t *d = lv_dropdown_create(parent);
    lv_dropdown_set_options(d, opts);
    lv_dropdown_set_selected(d, (uint32_t)(sel < 0 ? 0 : sel));
    lv_obj_set_size(d, LV_PCT(100), 80);
    lv_obj_set_style_text_font(d, aos_font_body, 0);
    lv_obj_set_style_bg_color(d, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(d, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, 20, 0);
    lv_obj_set_style_pad_hor(d, 22, 0);
    lv_obj_t *list = lv_dropdown_get_list(d);
    lv_obj_set_style_text_font(list, aos_font_body, 0);
    lv_obj_set_style_bg_color(list, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(list, AOS_C_TEXT, 0);
    lv_obj_set_style_max_height(list, U.H / 2, 0);
    if (cb) lv_obj_add_event_cb(d, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return d;
}

static void dev_name(const aos_zigbee_dev_t *d, char *out, size_t n)
{
    if (d->name[0]) snprintf(out, n, "%s", d->name);
    else if (d->model[0]) snprintf(out, n, "%s", d->model);
    else snprintf(out, n, "0x%02x%02x%02x", d->ieee[2], d->ieee[1], d->ieee[0]);
}

static bool has_out(const aos_zigbee_dev_t *d) { return d->onoff_eps != 0; }

/* how many outputs the device with that IEEE has (a two-relay module, 2) */
static int outs_of(const uint8_t ieee[8])
{
    int nd = s_zb ? aos_zigbee_devices(s_zb, AOS_ZB_DEVICES_MAX) : 0;
    for (int i = 0; i < nd; i++)
        if (!memcmp(s_zb[i].ieee, ieee, 8)) {
            int n = 0;
            for (int k = 0; k < s_zb[i].neps && k < 4; k++) n += (s_zb[i].onoff_eps >> k) & 1;
            return n;
        }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Typing                                                                    */
/* ------------------------------------------------------------------------ */

static void overlay_close(void)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = U.ta = U.kb = NULL;
}

static void text_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    static char t[400];
    snprintf(t, sizeof t, "%s", lv_textarea_get_text(U.ta));
    void (*done)(const char *) = c == LV_EVENT_READY ? U.text_done : NULL;
    overlay_close();
    if (done) done(t);
}

static void text_entry(const char *title, const char *hint, const char *value, int max, bool one_line, bool password,
                       void (*done)(const char *))
{
    overlay_close();
    U.text_done = done;
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(aos_label(U.overlay, title, aos_font_title, AOS_C_TEXT), LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 30);
    U.ta = lv_textarea_create(U.overlay);
    lv_textarea_set_one_line(U.ta, one_line);
    lv_textarea_set_max_length(U.ta, (uint32_t)max);
    lv_textarea_set_text(U.ta, value ? value : "");
    if (password) lv_textarea_set_password_mode(U.ta, true);
    if (hint) lv_textarea_set_placeholder_text(U.ta, hint);
    int32_t kh = U.H * 2 / 5;
    lv_obj_set_size(U.ta, U.W - 2 * AOS_UI_PAD, one_line ? 88 : U.H - kh - 140);
    lv_obj_align(U.ta, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 100);
    lv_obj_set_style_text_font(U.ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(U.ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_DIM, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_width(U.ta, 0, 0);
    lv_obj_set_style_radius(U.ta, 20, 0);
    lv_obj_set_style_pad_all(U.ta, 22, 0);
    U.kb = lv_keyboard_create(U.overlay);
    lv_obj_set_size(U.kb, U.W, kh);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.kb, aos_font_body);
    lv_keyboard_set_textarea(U.kb, U.ta);
    lv_obj_add_event_cb(U.kb, text_cb, LV_EVENT_ALL, NULL);
}

/* ------------------------------------------------------------------------ */
/* The page                                                                  */
/* ------------------------------------------------------------------------ */

static void on_cb(lv_event_t *e)
{
    aos_bot_enable(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void token_done(const char *t)
{
    if (!aos_bot_set_token(t)) aos_ui_toast(_("Eso no parece un token de bot (número:letras)"), 2500);
    build_page();
}

static void token_cb(lv_event_t *e)
{
    text_entry(_("El token del bot"), "123456789:AA...", "", 90, true, false, token_done);
}

static void link_cb(lv_event_t *e)
{
    aos_bot_link_code();
    build_page();
}

static void link_cancel_cb(lv_event_t *e)
{
    aos_bot_link_cancel();
    build_page();
}

static void forget_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    aos_bot_chat_t c[AOS_BOT_CHATS_MAX];
    int n = aos_bot_chats(c, AOS_BOT_CHATS_MAX);
    if (i < n) aos_bot_chat_forget(c[i].id);
    build_page();
}

static void cmd_cb(lv_event_t *e)
{
    uint32_t bit = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    uint32_t m = aos_bot_cmds();
    if (lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)) m |= bit;
    else m &= ~bit;
    aos_bot_set_cmds(m);
}

static void rule_on_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    aos_bot_rule_t r[AOS_BOT_RULES_MAX];
    int n = aos_bot_rules(r, AOS_BOT_RULES_MAX);
    if (i >= n) return;
    r[i].on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    aos_bot_rule_set(i, &r[i]);
}

static void rule_open_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    memset(&U.r, 0, sizeof U.r);
    U.edit = -1;
    if (i >= 0) {
        aos_bot_rule_t r[AOS_BOT_RULES_MAX];
        int n = aos_bot_rules(r, AOS_BOT_RULES_MAX);
        if (i < n) { U.r = r[i]; U.edit = i; }
    }
    if (U.edit < 0) {
        U.r.on = true;
        U.r.days = 0x7F;
        U.r.at_min = 8 * 60;
        U.r.op = AOS_BOT_OP_CHANGE;
        U.r.var = AOS_BOT_V_ALARM;
        U.r.gap_s = 30;
    }
    sheet_build();
}

static const char *state_words(const aos_bot_info_t *in, lv_color_t *c)
{
    *c = AOS_C_DIM;
    switch (in->state) {
    case AOS_BOT_OFF: return _("Apagado");
    case AOS_BOT_NO_TOKEN: *c = AOS_C_ORANGE; return _("Falta el token");
    case AOS_BOT_WAITING_NET: *c = AOS_C_ORANGE; return _("Esperando la red");
    case AOS_BOT_CONNECTING: return _("Conectando con Telegram…");
    case AOS_BOT_ONLINE: *c = AOS_C_GREEN; return _("En línea");
    case AOS_BOT_BAD_TOKEN: *c = AOS_C_RED; return _("Telegram no reconoce el token");
    default: *c = AOS_C_ORANGE; return in->why[0] ? in->why : _("Con problemas, reintentando");
    }
}

static void build_page(void)
{
    if (!U.root) return;
    if (U.page) { U.scroll = lv_obj_get_scroll_y(U.page); lv_obj_delete(U.page); }
    U.seen = aos_bot_version();
    aos_bot_info_t in;
    aos_bot_info(&in);
    U.page = box(U.root, U.W, U.H);
    lv_obj_add_flag(U.page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(U.page, LV_DIR_VER);
    lv_obj_set_flex_flow(U.page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(U.page, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_ver(U.page, 24, 0);
    lv_obj_set_style_pad_row(U.page, 14, 0);
    char b[200];

    /* the switch */
    lv_obj_t *c = card(U.page);
    lv_obj_t *r = row(c);
    lv_obj_t *left = box(r, U.W - 2 * AOS_UI_PAD - 44 - 140, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    aos_label(left, _("Bot de Telegram"), aos_font_title, AOS_C_TEXT);
    lv_color_t sc;
    const char *sw = state_words(&in, &sc);
    if (in.state == AOS_BOT_ONLINE && in.username[0]) snprintf(b, sizeof b, "%s · @%s", sw, in.username);
    else snprintf(b, sizeof b, "%s", sw);
    lv_obj_t *st = wrap_label(left, b, aos_font_small, sc);
    (void)st;
    switch_(r, aos_bot_enabled(), on_cb, NULL);
    if (in.received || in.sent) {
        snprintf(b, sizeof b, _("%u pedidos recibidos · %u mensajes mandados"), (unsigned)in.received, (unsigned)in.sent);
        aos_label(c, b, aos_font_small, AOS_C_DIM);
    }

    /* the token */
    section(U.page, _("TOKEN"));
    c = card(U.page);
    r = row(c);
    aos_label(r, aos_bot_has_token() ? _("Guardado") : _("Falta"), aos_font_body, aos_bot_has_token() ? AOS_C_GREEN : AOS_C_ORANGE);
    pill(r, aos_bot_has_token() ? _("Cambiar") : _("Cargar"), AOS_C_CARD2, token_cb, NULL);
    wrap_label(c, _("En Telegram, @BotFather y /newbot te dan el token. También se puede pegar desde el portal, en la página Bot. Nunca se vuelve a mostrar."),
               aos_font_small, AOS_C_DIM);

    /* the chats */
    section(U.page, _("CHATS VINCULADOS"));
    c = card(U.page);
    aos_bot_chat_t ch[AOS_BOT_CHATS_MAX];
    int nch = aos_bot_chats(ch, AOS_BOT_CHATS_MAX);
    for (int i = 0; i < nch; i++) {
        r = row(c);
        aos_label(r, ch[i].name[0] ? ch[i].name : "?", aos_font_body, AOS_C_TEXT);
        pill(r, _("Olvidar"), AOS_C_CARD2, forget_cb, (void *)(intptr_t)i);
    }
    if (in.code) {
        lv_obj_t *code = aos_label(c, "", &aos_inter_num_96, C_BOT);
        lv_label_set_text_fmt(code, "%03u %03u", (unsigned)(in.code / 1000), (unsigned)(in.code % 1000));
        snprintf(b, sizeof b, _("Mandale al bot: /vincular %u   (vale %u:%02u más)"), (unsigned)in.code,
                 (unsigned)(in.code_left_s / 60), (unsigned)(in.code_left_s % 60));
        wrap_label(c, b, aos_font_body, AOS_C_TEXT);
        pill(c, _("Cancelar"), AOS_C_CARD2, link_cancel_cb, NULL);
    } else {
        if (!nch) wrap_label(c, _("Nadie todavía. Sólo los chats vinculados pueden hablar con la placa."), aos_font_small, AOS_C_DIM);
        pill(c, _("Vincular un chat"), C_BOT, link_cb, NULL);
    }

    /* the commands */
    section(U.page, _("COMANDOS"));
    c = card(U.page);
    struct { uint32_t bit; const char *name; } C[] = {
        { AOS_BOT_CMD_STATE, N_("/estado: cómo está la placa") },
        { AOS_BOT_CMD_SHOT, N_("/captura: la pantalla") },
        { AOS_BOT_CMD_ZIGBEE, N_("/zigbee, /prender, /apagar") },
        { AOS_BOT_CMD_IR, N_("/ir: los códigos IR de Zigbee") },
        { AOS_BOT_CMD_NOTICE, N_("/aviso: un aviso en la pantalla") },
        { AOS_BOT_CMD_RULES, N_("/reglas: verlas y pausarlas") },
        { AOS_BOT_CMD_APPS, N_("Las apps pueden mandar mensajes") },
    };
    uint32_t m = aos_bot_cmds();
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        r = row(c);
        lv_obj_t *l = aos_label(r, aos_tr(C[i].name), aos_font_body, AOS_C_TEXT);
        lv_obj_set_width(l, U.W - 2 * AOS_UI_PAD - 44 - 130);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        switch_(r, (m & C[i].bit) != 0, cmd_cb, (void *)(uintptr_t)C[i].bit);
    }

    /* the rules */
    section(U.page, _("REGLAS"));
    c = card(U.page);
    aos_bot_rule_t rl[AOS_BOT_RULES_MAX];
    int nr = aos_bot_rules(rl, AOS_BOT_RULES_MAX);
    for (int i = 0; i < nr; i++) {
        r = row(c);
        char t[300];
        aos_bot_rule_text(&rl[i], t, sizeof t);
        lv_obj_t *l = aos_label(r, t, aos_font_body, rl[i].on ? AOS_C_TEXT : AOS_C_DIM);
        lv_obj_set_width(l, U.W - 2 * AOS_UI_PAD - 44 - 130);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(l, rule_open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        switch_(r, rl[i].on, rule_on_cb, (void *)(intptr_t)i);
    }
    if (!nr)
        wrap_label(c, _("Una regla manda un mensaje o una captura, prende o apaga algo, cuando pasa algo: una puerta que se abre, una hora, un aviso que llega."),
                   aos_font_small, AOS_C_DIM);
    if (nr < AOS_BOT_RULES_MAX) pill(c, _("Nueva regla"), C_BOT, rule_open_cb, (void *)(intptr_t)-1);

    /* the log */
    static char lines[16][96];
    int nl = aos_bot_log(lines, 16);
    if (nl) {
        section(U.page, _("LO ÚLTIMO"));
        c = card(U.page);
        for (int i = nl - 1; i >= 0; i--) wrap_label(c, lines[i], aos_font_small, AOS_C_DIM);
    }
    lv_obj_update_layout(U.page);
    lv_obj_scroll_to_y(U.page, U.scroll, LV_ANIM_OFF);
    if (U.sheet) lv_obj_move_foreground(U.sheet);
    if (U.overlay) lv_obj_move_foreground(U.overlay);
}

/* ------------------------------------------------------------------------ */
/* The rule's sheet                                                          */
/* ------------------------------------------------------------------------ */

static void sheet_close(void)
{
    if (U.sheet) lv_obj_delete(U.sheet);
    U.sheet = NULL;
}

static int sel_of(lv_event_t *e) { return (int)lv_dropdown_get_selected(lv_event_get_target(e)); }

static void trig_cb(lv_event_t *e) { U.r.trig = (uint8_t)sel_of(e); sheet_build(); }
static void dev_cb(lv_event_t *e)
{
    int i = sel_of(e);
    if (i < U.nzb) memcpy(U.r.ieee, U.zb_ieee[i], 8);
    U.r.out = 0;
    sheet_build();          /* its outputs, if it has several */
}
static void var_cb(lv_event_t *e) { U.r.var = (uint8_t)sel_of(e); sheet_build(); }
static void op_cb(lv_event_t *e) { U.r.op = (uint8_t)sel_of(e); sheet_build(); }
static void yesno_cb(lv_event_t *e) { U.r.value = sel_of(e) == 0 ? 1 : 0; }
static void hour_cb(lv_event_t *e) { U.r.at_min = (uint16_t)(sel_of(e) * 60 + U.r.at_min % 60); }
static void min_cb(lv_event_t *e) { U.r.at_min = (uint16_t)(U.r.at_min / 60 * 60 + sel_of(e) * 5); }
static void act_cb(lv_event_t *e) { U.r.act = (uint8_t)sel_of(e); sheet_build(); }
static void adev_cb(lv_event_t *e)
{
    int i = sel_of(e);
    if (i < U.nout) memcpy(U.r.act_ieee, U.out_ieee[i], 8);
    U.r.act_out = 0;
    sheet_build();
}
static void ir_cb(lv_event_t *e) { int i = sel_of(e); if (i < U.nir) snprintf(U.r.ir, sizeof U.r.ir, "%s", U.ir[i].name); }
static void out_cb(lv_event_t *e) { U.r.out = (uint8_t)sel_of(e); }
static void act_out_cb(lv_event_t *e)
{
    int i = sel_of(e);
    U.r.act_out = i >= outs_of(U.r.act_ieee) ? AOS_BOT_OUT_ALL : (uint8_t)i;
}

/* "Salida 1\nSalida 2[\nTodas]" */
static void out_opts(int n, bool all, char *out, size_t on)
{
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < n && o < on; i++) o += (size_t)snprintf(out + o, on - o, "%s%s %d", i ? "\n" : "", _("Salida"), i + 1);
    if (all && o < on) snprintf(out + o, on - o, "\n%s", _("Todas"));
}

static void gap_cb(lv_event_t *e)
{
    static const uint16_t G[] = { 30, 60, 300, 900, 3600 };
    U.r.gap_s = G[sel_of(e)];
}

static void day_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    U.r.days ^= (uint8_t)(1u << d);
    sheet_build();
}

static void value_done(const char *t) { U.r.value = strtof(t, NULL); sheet_build(); }
static void value_cb(lv_event_t *e)
{
    char v[24];
    snprintf(v, sizeof v, "%g", (double)U.r.value);
    text_entry(_("El valor"), "25", v, 10, true, false, value_done);
}

static void filter_done(const char *t) { snprintf(U.r.filter, sizeof U.r.filter, "%s", t); sheet_build(); }
static void filter_cb(lv_event_t *e)
{
    text_entry(_("Sólo los avisos de…"), _("WhatsApp, Home Assistant… (vacío: todos)"), U.r.filter, 38, true, false, filter_done);
}

static void text_done(const char *t) { snprintf(U.r.text, sizeof U.r.text, "%s", t); sheet_build(); }
static void textmsg_cb(lv_event_t *e)
{
    text_entry(_("El mensaje"), _("{dispositivo} {valor} {hora} {placa} {app} {titulo} {texto}"), U.r.text,
               (int)sizeof U.r.text - 1, false, false, text_done);
}

static void save_cb(lv_event_t *e)
{
    if (U.r.trig == AOS_BOT_T_ZIGBEE && !memcmp(U.r.ieee, "\0\0\0\0\0\0\0\0", 8)) {
        aos_ui_toast(_("Elegí un dispositivo"), 2000);
        return;
    }
    if (U.r.trig == AOS_BOT_T_TIME && !(U.r.days & 0x7F)) { aos_ui_toast(_("Elegí al menos un día"), 2000); return; }
    if (U.r.act >= AOS_BOT_A_ZB_ON && U.r.act <= AOS_BOT_A_ZB_TOGGLE && !memcmp(U.r.act_ieee, "\0\0\0\0\0\0\0\0", 8)) {
        aos_ui_toast(_("Elegí qué prender o apagar"), 2000);
        return;
    }
    if (U.r.act == AOS_BOT_A_IR && !U.r.ir[0]) { aos_ui_toast(_("Elegí un código IR"), 2000); return; }
    if (!aos_bot_rule_set(U.edit, &U.r)) { aos_ui_toast(_("No pude guardar la regla"), 2000); return; }
    sheet_close();
    build_page();
}

static void delete_cb(lv_event_t *e)
{
    if (U.edit >= 0) aos_bot_rule_delete(U.edit);
    sheet_close();
    build_page();
}

static void cancel_cb(lv_event_t *e) { sheet_close(); }

static void zb_lists(void)
{
    U.nzb = U.nout = 0;
    U.nir = aos_zigbee_ir_codes(U.ir, 24);
    if (U.nir < 0) U.nir = 0;
    if (!s_zb) return;
    int n = aos_zigbee_devices(s_zb, AOS_ZB_DEVICES_MAX);
    for (int i = 0; i < n; i++) {
        memcpy(U.zb_ieee[U.nzb++], s_zb[i].ieee, 8);
        if (has_out(&s_zb[i])) memcpy(U.out_ieee[U.nout++], s_zb[i].ieee, 8);
    }
}

/* the drop-down's options for a list of IEEEs, the selected one in *sel */
static void dev_opts(uint8_t (*ieee)[8], int n, const uint8_t want[8], char *out, size_t on, int *sel)
{
    size_t o = 0;
    out[0] = 0;
    *sel = -1;
    int nd = s_zb ? aos_zigbee_devices(s_zb, AOS_ZB_DEVICES_MAX) : 0;
    for (int i = 0; i < n && o < on; i++) {
        char name[40] = "?";
        for (int k = 0; k < nd; k++) if (!memcmp(s_zb[k].ieee, ieee[i], 8)) dev_name(&s_zb[k], name, sizeof name);
        for (char *p = name; *p; p++) if (*p == '\n') *p = ' ';
        o += (size_t)snprintf(out + o, on - o, "%s%s", i ? "\n" : "", name);
        if (!memcmp(ieee[i], want, 8)) *sel = i;
    }
}

static lv_obj_t *field(lv_obj_t *parent, const char *label)
{
    aos_label(parent, label, aos_font_small, AOS_C_DIM);
    return parent;
}

static lv_obj_t *tap_field(lv_obj_t *parent, const char *text, const char *empty, lv_event_cb_t cb)
{
    lv_obj_t *b = box(parent, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_pad_all(b, 22, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = wrap_label(b, text && text[0] ? text : empty, aos_font_body, text && text[0] ? AOS_C_TEXT : AOS_C_DIM);
    (void)l;
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void sheet_build(void)
{
    int32_t scroll = U.sheet ? lv_obj_get_scroll_y(U.sheet) : 0;
    sheet_close();
    zb_lists();
    U.sheet = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.sheet, lv_color_hex(0x0E0E12), 0);
    lv_obj_set_style_bg_opa(U.sheet, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.sheet, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(U.sheet, LV_DIR_VER);
    lv_obj_set_flex_flow(U.sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(U.sheet, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_ver(U.sheet, 28, 0);
    lv_obj_set_style_pad_row(U.sheet, 12, 0);
    aos_label(U.sheet, U.edit < 0 ? _("Nueva regla") : _("La regla"), aos_font_title, AOS_C_TEXT);
    char opts[AOS_ZB_DEVICES_MAX * 42];
    int sel;

    lv_obj_t *c = card(U.sheet);
    field(c, _("Cuando"));
    snprintf(opts, sizeof opts, "%s\n%s\n%s\n%s", _("Cambia un dispositivo Zigbee"), _("Es una hora"),
             _("Arranca la placa"), _("Llega un aviso"));
    dropdown(c, opts, U.r.trig, trig_cb);
    if (U.r.trig == AOS_BOT_T_ZIGBEE) {
        if (!U.nzb) wrap_label(c, _("No hay dispositivos Zigbee: se suman en la app Zigbee."), aos_font_small, AOS_C_ORANGE);
        else {
            dev_opts(U.zb_ieee, U.nzb, U.r.ieee, opts, sizeof opts, &sel);
            if (sel < 0) { sel = 0; memcpy(U.r.ieee, U.zb_ieee[0], 8); }
            dropdown(c, opts, sel, dev_cb);
        }
        field(c, _("Qué mira"));
        snprintf(opts, sizeof opts, "%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s", _("Prendido o apagado"),
                 _("Abierta, movimiento, agua (alarma)"), _("Presencia"), _("Temperatura (°C)"), _("Humedad (%)"),
                 _("Batería (%)"), _("Nivel (%)"), _("Un evento: un botón, un control"));
        dropdown(c, opts, U.r.var, var_cb);
        int no = outs_of(U.r.ieee);
        if (U.r.var == AOS_BOT_V_ON && no > 1) {
            out_opts(no, false, opts, sizeof opts);
            dropdown(c, opts, U.r.out < no ? U.r.out : 0, out_cb);
        }
        if (U.r.var != AOS_BOT_V_EVENT) {
            bool yesno = U.r.var == AOS_BOT_V_ON || U.r.var == AOS_BOT_V_ALARM || U.r.var == AOS_BOT_V_OCC;
            if (yesno && (U.r.op == AOS_BOT_OP_ABOVE || U.r.op == AOS_BOT_OP_BELOW)) U.r.op = AOS_BOT_OP_IS;
            if (yesno) snprintf(opts, sizeof opts, "%s\n%s", _("cuando cambia"), _("cuando pasa a…"));
            else snprintf(opts, sizeof opts, "%s\n%s\n%s\n%s", _("cuando cambia"), _("cuando pasa de…"),
                          _("cuando baja de…"), _("cuando llega a…"));
            int op = yesno ? (U.r.op == AOS_BOT_OP_IS ? 1 : 0) : U.r.op;
            /* yes/no: index 1 comes back as ABOVE and is made IS above, on the rebuild */
            dropdown(c, opts, op, op_cb);
            if (U.r.op != AOS_BOT_OP_CHANGE) {
                if (yesno) {
                    const char *a = U.r.var == AOS_BOT_V_ON ? _("prendido") : U.r.var == AOS_BOT_V_OCC ? _("hay alguien") : _("alarma (abierta…)");
                    const char *b = U.r.var == AOS_BOT_V_ON ? _("apagado") : U.r.var == AOS_BOT_V_OCC ? _("no hay nadie") : _("normal (cerrada…)");
                    snprintf(opts, sizeof opts, "%s\n%s", a, b);
                    dropdown(c, opts, U.r.value != 0 ? 0 : 1, yesno_cb);
                } else {
                    char v[24];
                    snprintf(v, sizeof v, "%g", (double)U.r.value);
                    tap_field(c, v, "0", value_cb);
                }
            }
        }
    } else if (U.r.trig == AOS_BOT_T_TIME) {
        char o2[24 * 3 + 1] = "";
        for (int h = 0; h < 24; h++) snprintf(o2 + strlen(o2), sizeof o2 - strlen(o2), "%s%02d", h ? "\n" : "", h);
        lv_obj_t *r = row(c);
        lv_obj_t *dh = dropdown(r, o2, U.r.at_min / 60, hour_cb);
        lv_obj_set_width(dh, (U.W - 2 * AOS_UI_PAD - 44 - 14) / 2);
        o2[0] = 0;
        for (int m = 0; m < 60; m += 5) snprintf(o2 + strlen(o2), sizeof o2 - strlen(o2), "%s%02d", m ? "\n" : "", m);
        lv_obj_t *dm = dropdown(r, o2, (U.r.at_min % 60) / 5, min_cb);
        lv_obj_set_width(dm, (U.W - 2 * AOS_UI_PAD - 44 - 14) / 2);
        lv_obj_t *days = row(c);
        lv_obj_set_flex_align(days, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_flex_flow(days, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(days, 10, 0);
        static const char *const D[] = { N_("do"), N_("lu"), N_("ma"), N_("mi"), N_("ju"), N_("vi"), N_("sá") };
        for (int k = 1; k <= 7; k++) {
            int d = k % 7;
            pill(days, aos_tr(D[d]), (U.r.days & (1u << d)) ? C_BOT : AOS_C_CARD2, day_cb, (void *)(intptr_t)d);
        }
    } else if (U.r.trig == AOS_BOT_T_NOTIF) {
        field(c, _("De qué app (vacío: todas)"));
        tap_field(c, U.r.filter, _("Todas"), filter_cb);
    }

    c = card(U.sheet);
    field(c, _("Hacer"));
    snprintf(opts, sizeof opts, "%s\n%s\n%s\n%s\n%s\n%s", _("Mandar un mensaje"), _("Mandar una captura"),
             _("Prender un dispositivo"), _("Apagar un dispositivo"), _("Cambiar un dispositivo"), _("Mandar un código IR"));
    dropdown(c, opts, U.r.act, act_cb);
    if (U.r.act >= AOS_BOT_A_ZB_ON && U.r.act <= AOS_BOT_A_ZB_TOGGLE) {
        if (!U.nout) wrap_label(c, _("No hay dispositivos que se prendan y apaguen."), aos_font_small, AOS_C_ORANGE);
        else {
            dev_opts(U.out_ieee, U.nout, U.r.act_ieee, opts, sizeof opts, &sel);
            if (sel < 0) { sel = 0; memcpy(U.r.act_ieee, U.out_ieee[0], 8); }
            dropdown(c, opts, sel, adev_cb);
            int no = outs_of(U.r.act_ieee);
            if (no > 1) {
                out_opts(no, true, opts, sizeof opts);
                dropdown(c, opts, U.r.act_out == AOS_BOT_OUT_ALL || U.r.act_out >= no ? no : U.r.act_out, act_out_cb);
            }
        }
    } else if (U.r.act == AOS_BOT_A_IR) {
        if (!U.nir) wrap_label(c, _("No hay códigos IR guardados (app Zigbee, emisor IR)."), aos_font_small, AOS_C_ORANGE);
        else {
            size_t o = 0;
            sel = 0;
            for (int i = 0; i < U.nir && o < sizeof opts; i++) {
                o += (size_t)snprintf(opts + o, sizeof opts - o, "%s%s", i ? "\n" : "", U.ir[i].name);
                if (!strcmp(U.ir[i].name, U.r.ir)) sel = i;
            }
            if (!U.r.ir[0]) snprintf(U.r.ir, sizeof U.r.ir, "%s", U.ir[0].name);
            dropdown(c, opts, sel, ir_cb);
        }
    } else {
        field(c, U.r.act == AOS_BOT_A_SHOT ? _("Con este texto") : _("El mensaje"));
        tap_field(c, U.r.text, _("El de siempre (tocá para escribir otro)"), textmsg_cb);
    }
    field(c, _("Como mucho, una vez cada"));
    static const uint16_t G[] = { 30, 60, 300, 900, 3600 };
    sel = 0;
    for (int i = 0; i < 5; i++) if (U.r.gap_s == G[i]) sel = i;
    snprintf(opts, sizeof opts, "%s\n%s\n%s\n%s\n%s", _("30 segundos"), _("1 minuto"), _("5 minutos"), _("15 minutos"), _("1 hora"));
    dropdown(c, opts, sel, gap_cb);

    lv_obj_t *r = row(U.sheet);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (U.edit >= 0) pill(r, _("Borrar"), AOS_C_RED, delete_cb, NULL);
    pill(r, _("Cancelar"), AOS_C_CARD2, cancel_cb, NULL);
    pill(r, _("Guardar"), C_BOT, save_cb, NULL);
    lv_obj_update_layout(U.sheet);
    lv_obj_scroll_to_y(U.sheet, scroll, LV_ANIM_OFF);
}

/* ------------------------------------------------------------------------ */
/* The app                                                                   */
/* ------------------------------------------------------------------------ */

static void timer_cb(lv_timer_t *t)
{
    if (U.sheet || U.overlay) return;
    static uint32_t last_s;
    aos_bot_info_t in;
    aos_bot_info(&in);
    /* the link code's countdown moves every second */
    if (aos_bot_version() != U.seen || (in.code && in.code_left_s != last_s)) {
        last_s = in.code_left_s;
        build_page();
    }
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    if (!s_zb) {
        size_t n = sizeof(aos_zigbee_dev_t) * AOS_ZB_DEVICES_MAX;
#if defined(AOS_SIM)
        s_zb = calloc(1, n);
#else
        s_zb = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    }
    build_page();
    U.timer = lv_timer_create(timer_cb, 500, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    if (U.timer) lv_timer_delete(U.timer);
    free(s_zb);
    s_zb = NULL;
    memset(&U, 0, sizeof U);
}

static bool back(aos_app_t *self, void *inst)
{
    if (U.overlay) { overlay_close(); return true; }
    if (U.sheet) { sheet_close(); return true; }
    return false;
}

void aos_app_bot_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.bot", .name = "Bot", .icon = AOS_SYM_ROBOT,
            .color_a = 0x5AC8FA, .color_b = 0x1C7ED6,
            .order = 535,
        },
        .create = create, .destroy = destroy, .back = back,
    };
}
