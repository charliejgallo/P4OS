/*
 * P4OS - Infrarrojo: a learning remote for the bench (T3 of the workshop).
 *
 *     tools/build_apps.sh infrarrojo
 *     python3 apps/infrarrojo/tools/pack_smartir.py --src <SmartIR checkout>
 *     tools/install_apps.sh p4os.local infrarrojo
 *
 * Four tabs:
 *   Controles  the devices of /sdcard/ir, each a remote drawn the way real
 *              ones are (power on top, the arrows round OK, volume and
 *              channel rockers, the digits), editable; an air conditioner
 *              is a thermostat that sends its whole state (ir_remote.c)
 *   Aprender   the receiver listening: each capture decoded, drawn, and
 *              confirmed by pressing again (ir_learn.c)
 *   Códigos    SmartIR's library by kind, brand and model, and the search
 *              of "which remote is this button from" (ir_base.c)
 *   Cableado   which pins, how to wire the receiver and the LED, and a loop
 *              test (this file)
 *
 * The portal's page (web/infrarrojo.js) reads and writes the same files,
 * and asks the app to send or to listen through two files in /data
 * (ir_portal.c). That works while the app is alive, in front or not: it
 * has AOS_APP_FLAG_BACKGROUND, and lets go of the LED's pin after a minute
 * in the background with nothing to send.
 *
 * The screen is built from the root's size, in both orientations; turning
 * it rebuilds the app (resize is not implemented) and the statics below
 * bring back the tab and the open device.
 */
#include "ir.h"
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_io.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <string.h>

/* A remote with its LED lit and the light going out of it. */
static const uint8_t IR_ICON[] = {
    AIC_HEADER,
    AIC_ARC(AIC_CENTER, -6, -8, 46, 0, 5, 0, 360, 228, 312, 0, AIC_C_BG, 0, AIC_C_TEXT, 150),
    AIC_ARC(AIC_CENTER, -6, -8, 28, 0, 5, 0, 360, 222, 318, 0, AIC_C_BG, 0, AIC_C_TEXT, 230),
    AIC_RECT(AIC_CENTER, -6, 18, 30, 52, 9, AIC_C_TEXT, 255),
    AIC_INTO,
    AIC_RECT(AIC_TOP_MID, 0, 5, 10, 10, AIC_CIRCLE, AIC_C_RED, 255),
    AIC_RECT(AIC_TOP_MID, -6, 21, 8, 8, AIC_CIRCLE, AIC_C_DIM, 255),
    AIC_RECT(AIC_TOP_MID, 6, 21, 8, 8, AIC_CIRCLE, AIC_C_DIM, 255),
    AIC_RECT(AIC_TOP_MID, -6, 33, 8, 8, AIC_CIRCLE, AIC_C_DIM, 255),
    AIC_RECT(AIC_TOP_MID, 6, 33, 8, 8, AIC_CIRCLE, AIC_C_DIM, 255),
    AIC_OUT,
    AIC_END
};

enum { TAB_REMOTES = 0, TAB_LEARN, TAB_BASE, TAB_WIRING, TAB_COUNT };

struct ir_app {
    lv_obj_t *root, *content, *bar;
    lv_obj_t *page[TAB_COUNT], *tab_btn[TAB_COUNT];
    int       tab;
    int32_t   w, h;
    bool      land;
    /* the sheet */
    lv_obj_t *sheet_bg, *sheet, *kb;
    /* the asking sheet's answer */
    void    (*ask_cb)(void *user);
    void     *ask_user;
    /* wiring */
    lv_obj_t *pin_rx, *pin_tx, *loop_lbl;
    bool      loop_wait;
    int       pick_rx;
    bool      hidden;
    uint32_t  hidden_ms;
};

static ir_app_t *A;
static int s_tab;       /* across a rebuild */

/* ----------------------------------------------------------------- helpers */

bool    ir_ui_landscape(void) { return A && A->land; }
int32_t ir_ui_width(void) { return A ? A->w : 720; }
void    ir_ui_toast(const char *text) { aos_ui_toast(text, 2200); }

lv_obj_t *ir_ui_scroll(lv_obj_t *parent)
{
    lv_obj_t *s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_width(s, LV_PCT(100));
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_bottom(s, 32, 0);
    lv_obj_set_style_pad_row(s, 18, 0);
    lv_obj_set_scroll_dir(s, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_ACTIVE);
    return s;
}

lv_obj_t *ir_ui_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 24, 0);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *ir_ui_btn(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, AOS_UI_TAP_MIN);
    lv_obj_set_style_min_width(b, 160, 0);
    lv_obj_set_style_pad_hor(b, 28, 0);
    lv_obj_set_style_radius(b, AOS_UI_TAP_MIN / 2, 0);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_DISABLED);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, aos_font_body, 0);
    lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

lv_obj_t *ir_ui_header(lv_obj_t *parent, const char *title, const char *back, lv_event_cb_t back_cb, void *user)
{
    lv_obj_t *h = lv_obj_create(parent);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, LV_PCT(100), back ? 96 : 110);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    if (back) {
        lv_obj_t *b = lv_button_create(h);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_SIZE_CONTENT, AOS_UI_TAP_MIN);
        lv_obj_set_style_pad_left(b, AOS_UI_PAD - 8, 0);
        lv_obj_set_style_pad_right(b, 16, 0);
        lv_obj_align(b, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text_fmt(l, "%s", AOS_SYM_CHEVRON_LEFT);
        lv_obj_set_style_text_font(l, &aos_sym_44, 0);
        lv_obj_set_style_text_color(l, AOS_C_ACCENT, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *t = lv_label_create(b);
        lv_label_set_text(t, back);
        lv_obj_set_style_text_font(t, aos_font_body, 0);
        lv_obj_set_style_text_color(t, AOS_C_ACCENT, 0);
        /* a long word ("Fernbedienungen") is cut, not laid over the title */
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(t, 150, 0);
        lv_obj_set_height(t, lv_font_get_line_height(aos_font_body));
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 44, 0);
        lv_obj_set_style_opa(b, LV_OPA_60, LV_STATE_PRESSED);
        if (back_cb) lv_obj_add_event_cb(b, back_cb, LV_EVENT_CLICKED, user);
        lv_obj_t *tl = lv_label_create(h);
        lv_label_set_text(tl, title);
        lv_obj_set_style_text_font(tl, aos_font_body, 0);
        lv_obj_set_style_text_color(tl, AOS_C_TEXT, 0);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(tl, (A ? A->w : 720) - 2 * 232);
        lv_obj_set_style_max_height(tl, 2 * lv_font_get_line_height(aos_font_body), 0);
        lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(tl, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_t *tl = lv_label_create(h);
        lv_label_set_text(tl, title);
        lv_obj_set_style_text_font(tl, aos_font_title, 0);
        lv_obj_set_style_text_color(tl, AOS_C_TEXT, 0);
        lv_obj_align(tl, LV_ALIGN_LEFT_MID, AOS_UI_PAD, 6);
    }
    return h;
}

lv_obj_t *ir_ui_row(lv_obj_t *list, const char *glyph, lv_color_t gcolor, const char *text,
                    const char *sub, lv_event_cb_t cb, void *user)
{
    lv_obj_t *r = lv_button_create(list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), sub ? 112 : AOS_UI_ROW_H);
    lv_obj_set_style_bg_color(r, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, 22, 0);
    lv_obj_set_style_pad_hor(r, 22, 0);
    int32_t x = 0;
    if (glyph) {
        lv_obj_t *dot = lv_obj_create(r);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 64, 64);
        lv_obj_set_style_radius(dot, 18, 0);
        lv_obj_set_style_bg_color(dot, gcolor, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *g = lv_label_create(dot);
        lv_label_set_text(g, glyph);
        lv_obj_set_style_text_font(g, &aos_sym_44, 0);
        lv_obj_set_style_text_color(g, AOS_C_TEXT, 0);
        lv_obj_center(g);
        x = 84;
    }
    int32_t tw = (A ? A->w : 720) - 2 * AOS_UI_PAD - 44 - x - 40;
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, text);
    lv_obj_set_style_text_font(t, aos_font_body, 0);
    lv_obj_set_style_text_color(t, AOS_C_TEXT, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, tw);
    lv_obj_align(t, sub ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, x, sub ? 18 : 0);
    if (sub) {
        lv_obj_t *s = lv_label_create(r);
        lv_label_set_text(s, sub);
        lv_obj_set_style_text_font(s, aos_font_small, 0);
        lv_obj_set_style_text_color(s, AOS_C_DIM, 0);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(s, tw);
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, x, -16);
    }
    if (cb) {
        lv_obj_t *ch = lv_label_create(r);
        lv_label_set_text(ch, AOS_SYM_CHEVRON_RIGHT);
        lv_obj_set_style_text_font(ch, &aos_sym_28, 0);
        lv_obj_set_style_text_color(ch, AOS_C_DIM, 0);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, user);
    }
    return r;
}

/* ------------------------------------------------------------------- sheet */

static void kb_show(bool on)
{
    if (!A || !A->kb) return;
    if (on) lv_obj_remove_flag(A->kb, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(A->kb, LV_OBJ_FLAG_HIDDEN);
    /* the card goes above the keyboard while it is up */
    int32_t kb_h = lv_obj_get_height(A->kb);
    if (on) {
        lv_obj_set_style_max_height(A->sheet, A->h - kb_h - 32, 0);
        lv_obj_align(A->sheet, LV_ALIGN_TOP_MID, 0, 16);
    } else {
        lv_obj_set_style_max_height(A->sheet, A->h - 64, 0);
        lv_obj_align(A->sheet, LV_ALIGN_CENTER, 0, 0);
    }
}

static void kb_cb(lv_event_t *e)
{
    (void)e;
    kb_show(false);
    if (A && A->sheet) lv_obj_remove_state(lv_keyboard_get_textarea(A->kb), LV_STATE_FOCUSED);
}

static void ta_focus_cb(lv_event_t *e)
{
    if (!A || !A->kb) return;
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(A->kb, ta);
    kb_show(true);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
}

static void bg_cb(lv_event_t *e)
{
    /* a tap outside the card does nothing: the sheet closes with its buttons */
    (void)e;
}

bool ir_sheet_is_open(void) { return A && A->sheet_bg; }

void ir_sheet_close(void)
{
    if (!A || !A->sheet_bg) return;
    lv_obj_delete(A->sheet_bg);
    A->sheet_bg = A->sheet = A->kb = NULL;
}

lv_obj_t *ir_sheet_open(const char *title)
{
    if (!A) return NULL;
    ir_sheet_close();
    lv_obj_t *bg = lv_obj_create(A->root);
    lv_obj_remove_style_all(bg);
    lv_obj_set_size(bg, A->w, A->h);
    lv_obj_set_style_bg_color(bg, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_70, 0);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bg, bg_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *c = lv_obj_create(bg);
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, A->w - 2 * AOS_UI_PAD > 680 ? 680 : A->w - 2 * AOS_UI_PAD);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(c, A->h - 64, 0);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 28, 0);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_center(c);
    lv_obj_t *t = lv_label_create(c);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, aos_font_title, 0);
    lv_obj_set_style_text_color(t, AOS_C_TEXT, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, LV_PCT(100));
    A->sheet_bg = bg;
    A->sheet = c;
    A->kb = NULL;
    return c;
}

lv_obj_t *ir_sheet_text(lv_obj_t *sheet, const char *label, const char *value, int max)
{
    if (label) {
        lv_obj_t *l = lv_label_create(sheet);
        lv_label_set_text(l, label);
        lv_obj_set_style_text_font(l, aos_font_small, 0);
        lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
    }
    lv_obj_t *ta = lv_textarea_create(sheet);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (uint32_t)max);
    lv_textarea_set_text(ta, value ? value : "");
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_style_text_font(ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(ta, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 18, 0);
    lv_obj_set_style_pad_all(ta, 18, 0);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    if (!A->kb) {
        A->kb = lv_keyboard_create(A->sheet_bg);
        aos_keyboard_style(A->kb, aos_font_body);
        lv_obj_set_size(A->kb, A->w, A->land ? A->h * 52 / 100 : A->h * 36 / 100);
        lv_obj_align(A->kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_event_cb(A->kb, kb_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(A->kb, kb_cb, LV_EVENT_CANCEL, NULL);
        lv_obj_add_flag(A->kb, LV_OBJ_FLAG_HIDDEN);
    }
    return ta;
}

lv_obj_t *ir_sheet_buttons(lv_obj_t *sheet)
{
    lv_obj_t *r = lv_obj_create(sheet);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 16, 0);
    lv_obj_set_style_pad_row(r, 16, 0);
    lv_obj_set_style_pad_top(r, 8, 0);
    return r;
}

static void ask_no(lv_event_t *e) { (void)e; ir_sheet_close(); }

static void ask_yes(lv_event_t *e)
{
    (void)e;
    void (*cb)(void *) = A->ask_cb;
    void *user = A->ask_user;
    ir_sheet_close();
    if (cb) cb(user);
}

void ir_ask(const char *title, const char *text, const char *yes, lv_color_t color,
            void (*cb)(void *user), void *user)
{
    lv_obj_t *s = ir_sheet_open(title);
    if (!s) return;
    lv_obj_t *l = lv_label_create(s);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, aos_font_body, 0);
    lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
    A->ask_cb = cb;
    A->ask_user = user;
    lv_obj_t *r = ir_sheet_buttons(s);
    ir_ui_btn(r, _("Cancelar"), AOS_C_CARD2, ask_no, NULL);
    ir_ui_btn(r, yes, color, ask_yes, NULL);
}

/* -------------------------------------------------------------------- tabs */

static void tab_style(void)
{
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_color_t c = i == A->tab ? AOS_C_ACCENT : AOS_C_DIM;
        lv_obj_t *b = A->tab_btn[i];
        for (uint32_t k = 0; k < lv_obj_get_child_count(b); k++)
            lv_obj_set_style_text_color(lv_obj_get_child(b, (int32_t)k), c, 0);
        if (i == A->tab) lv_obj_remove_flag(A->page[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(A->page[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void ir_ui_goto_tab(int tab)
{
    if (!A || tab < 0 || tab >= TAB_COUNT) return;
    if (A->tab == TAB_LEARN && tab != TAB_LEARN) ir_learn_shown(false);
    A->tab = s_tab = tab;
    tab_style();
    if (tab == TAB_LEARN) ir_learn_shown(true);
}

static void tab_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == A->tab) {
        /* a second tap goes to the top of the tab */
        if (i == TAB_REMOTES) ir_remote_back();
        else if (i == TAB_BASE) while (ir_base_back()) {}
    }
    ir_ui_goto_tab(i);
}

void ir_ui_learn_for(const char *file, int button)
{
    ir_learn_target(file, button);
    ir_ui_goto_tab(TAB_LEARN);
}

/* ------------------------------------------------------------------ wiring */

static const char *pin_text(int gpio, char *out, size_t n)
{
    const aos_io_pin_t *p = aos_io_pin_of_gpio(gpio);
    if (p) snprintf(out, n, _("GPIO%d · pata %d"), gpio, p->pin);
    else snprintf(out, n, "GPIO%d", gpio);
    return out;
}

static void pins_refresh(void)
{
    int rx, tx;
    ir_hw_pins(&rx, &tx);
    char b[48];
    if (A->pin_rx) lv_label_set_text(A->pin_rx, pin_text(rx, b, sizeof b));
    if (A->pin_tx) lv_label_set_text(A->pin_tx, pin_text(tx, b, sizeof b));
}

static const char *port_of(int gpio)
{
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *p = aos_io_port_at(i);
        for (int k = 0; k < 4; k++) if (p->pins[k] == gpio) return p->name;
    }
    return NULL;
}

static void pick_cb(lv_event_t *e)
{
    int gpio = (int)(intptr_t)lv_event_get_user_data(e);
    int rx, tx;
    ir_hw_pins(&rx, &tx);
    if (A->pick_rx) rx = gpio; else tx = gpio;
    if (rx == tx) { ir_ui_toast(_("El receptor y el LED van en pines distintos")); return; }
    ir_hw_set_pins(rx, tx);
    ir_sheet_close();
    pins_refresh();
}

static void pick_close(lv_event_t *e) { (void)e; ir_sheet_close(); }

static void pin_pick(bool rx)
{
    A->pick_rx = rx;
    lv_obj_t *s = ir_sheet_open(rx ? _("Pin del receptor") : _("Pin del LED"));
    int cur_rx, cur_tx;
    ir_hw_pins(&cur_rx, &cur_tx);
    int cur = rx ? cur_rx : cur_tx;
    const aos_io_pin_t *h = aos_io_header();
    for (int i = 0; i < 40; i++) {
        const aos_io_pin_t *p = &h[i];
        if (p->gpio < 0 || (p->flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD))) continue;
        char t[40], sub[96];
        snprintf(t, sizeof t, _("GPIO%d · pata %d"), p->gpio, p->pin);
        const char *who = aos_io_owner(p->gpio);
        const char *port = port_of(p->gpio);
        sub[0] = 0;
        if (who && strcmp(who, IR_OWNER)) snprintf(sub, sizeof sub, _("ocupado: %s"), who);
        else if (port) snprintf(sub, sizeof sub, _("en el puerto %s"), port);
        else if (p->flags & AOS_PIN_VO4) snprintf(sub, sizeof sub, "%s", _("dominio VO4: medir antes"));
        else if (p->flags & AOS_PIN_USB_JTAG) snprintf(sub, sizeof sub, "%s", _("USB-Serial-JTAG"));
        else if (p->flags & AOS_PIN_STRAPPING) snprintf(sub, sizeof sub, "%s", _("se lee al arrancar"));
        else if (p->note && p->note[0]) snprintf(sub, sizeof sub, "%s", p->note);
        lv_obj_t *r = ir_ui_row(s, p->gpio == cur ? AOS_SYM_CHECK : NULL, AOS_C_ACCENT, t,
                                sub[0] ? sub : NULL, pick_cb, (void *)(intptr_t)p->gpio);
        lv_obj_set_style_bg_color(r, p->gpio == cur ? AOS_C_ACCENT : AOS_C_CARD2, 0);
        if (who && strcmp(who, IR_OWNER)) lv_obj_add_state(r, LV_STATE_DISABLED);
    }
    lv_obj_t *b = ir_sheet_buttons(s);
    ir_ui_btn(b, _("Cerrar"), AOS_C_CARD2, pick_close, NULL);
}

static void pin_rx_cb(lv_event_t *e) { (void)e; pin_pick(true); }
static void pin_tx_cb(lv_event_t *e) { (void)e; pin_pick(false); }

static void loop_cb(lv_event_t *e)
{
    (void)e;
    ir_hw_loop_test();
    A->loop_wait = true;
    lv_label_set_text(A->loop_lbl, _("Mandando un NEC y escuchando…"));
    lv_obj_set_style_text_color(A->loop_lbl, AOS_C_DIM, 0);
}

/* the schematics, drawn */

static void dline(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, int w)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1; d.p1.y = y1;
    d.p2.x = x2; d.p2.y = y2;
    d.width = w;
    d.color = c;
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

static void dbox(lv_layer_t *layer, int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t bg, int32_t r,
                 lv_color_t border, int bw)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = bg;
    d.bg_opa = LV_OPA_COVER;
    d.radius = r;
    d.border_color = border;
    d.border_width = bw;
    d.border_opa = bw ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_area_t a = { x, y, x + w - 1, y + h - 1 };
    lv_draw_rect(layer, &d, &a);
}

static void dtext(lv_layer_t *layer, int32_t x, int32_t y, int32_t w, const char *t, lv_color_t c,
                  const lv_font_t *f, lv_text_align_t al)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = t;
    d.font = f;
    d.color = c;
    d.align = al;
    lv_area_t a = { x, y, x + w - 1, y + lv_font_get_line_height(f) * 2 };
    lv_draw_label(layer, &d, &a);
}

/* a resistor as a box on a wire */
static void dres(lv_layer_t *l, int32_t x1, int32_t y, int32_t x2, const char *val)
{
    int32_t m = (x1 + x2) / 2;
    dline(l, x1, y, m - 36, y, AOS_C_DIM, 4);
    dline(l, m + 36, y, x2, y, AOS_C_DIM, 4);
    dbox(l, m - 36, y - 14, 72, 28, AOS_C_CARD, 4, AOS_C_ORANGE, 3);
    dtext(l, m - 80, y - 50, 160, val, AOS_C_ORANGE, aos_font_caption, LV_TEXT_ALIGN_CENTER);
}

static void draw_rx(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *l = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t x = a.x1, y = a.y1, w = lv_area_get_width(&a);
    char t[48];
    int rx;
    ir_hw_pins(&rx, NULL);
    const aos_io_pin_t *p = aos_io_pin_of_gpio(rx);
    /* the receiver: a dark body with the dome, three legs */
    int32_t bx = x + 40, by = y + 30;
    dbox(l, bx, by, 130, 110, lv_color_hex(0x30303A), 14, AOS_C_DIM, 2);
    dbox(l, bx + 35, by + 25, 60, 60, lv_color_hex(0x15151A), LV_RADIUS_CIRCLE, AOS_C_DIM, 2);
    dtext(l, bx - 20, by + 118, 170, "TSOP38238", AOS_C_DIM, aos_font_caption, LV_TEXT_ALIGN_CENTER);
    static const char *LEG[3] = { "OUT", "GND", "VS" };
    int32_t hx = x + w - 230;
    for (int i = 0; i < 3; i++) {
        int32_t ly = by + 20 + i * 36;
        dline(l, bx + 130, ly, hx, ly, i == 0 ? AOS_C_GREEN : i == 1 ? AOS_C_DIM : AOS_C_RED, 5);
        dtext(l, bx + 140, ly - 30, 80, LEG[i], AOS_C_TEXT, aos_font_caption, LV_TEXT_ALIGN_LEFT);
    }
    snprintf(t, sizeof t, _("GPIO%d (pata %d)"), rx, p ? p->pin : 0);
    dtext(l, hx + 10, by + 6, 230, t, AOS_C_GREEN, aos_font_small, LV_TEXT_ALIGN_LEFT);
    dtext(l, hx + 10, by + 42, 230, _("GND (pata 19)"), AOS_C_TEXT, aos_font_small, LV_TEXT_ALIGN_LEFT);
    dtext(l, hx + 10, by + 78, 230, _("3V3 (pata 18)"), AOS_C_RED, aos_font_small, LV_TEXT_ALIGN_LEFT);
}

static void draw_tx(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *l = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t x = a.x1, y = a.y1, w = lv_area_get_width(&a);
    int tx;
    ir_hw_pins(NULL, &tx);
    const aos_io_pin_t *p = aos_io_pin_of_gpio(tx);
    char t[48];
    /* GPIO - 1k - base; collector - LED - resistor - 5 V; emitter - GND */
    int32_t qx = x + w / 2, qy = y + 170;          /* the transistor */
    snprintf(t, sizeof t, _("GPIO%d (pata %d)"), tx, p ? p->pin : 0);
    dtext(l, x, qy - 80, 240, t, AOS_C_GREEN, aos_font_small, LV_TEXT_ALIGN_LEFT);
    dline(l, x + 10, qy, x + 40, qy, AOS_C_GREEN, 5);
    dres(l, x + 40, qy, qx - 40, "1 kΩ");
    /* the transistor's body and legs */
    dbox(l, qx - 44, qy - 44, 88, 88, AOS_C_CARD2, LV_RADIUS_CIRCLE, AOS_C_DIM, 3);
    dline(l, qx - 40, qy, qx - 12, qy, AOS_C_DIM, 4);
    dline(l, qx - 12, qy - 26, qx - 12, qy + 26, AOS_C_TEXT, 5);
    dline(l, qx - 12, qy - 12, qx + 20, qy - 34, AOS_C_TEXT, 4);
    dline(l, qx - 12, qy + 12, qx + 20, qy + 34, AOS_C_TEXT, 4);
    dtext(l, qx + 52, qy - 16, 220, "2N2222 / BC337", AOS_C_DIM, aos_font_caption, LV_TEXT_ALIGN_LEFT);
    /* the collector up to the LED, the resistor and 5 V */
    dline(l, qx + 20, qy - 34, qx + 20, y + 120, AOS_C_DIM, 4);
    dbox(l, qx + 4, y + 82, 32, 38, AOS_C_RED, 6, AOS_C_RED, 0);
    dtext(l, qx + 48, y + 80, 230, _("LED IR (cátodo abajo)"), AOS_C_TEXT, aos_font_caption, LV_TEXT_ALIGN_LEFT);
    dline(l, qx + 20, y + 82, qx + 20, y + 52, AOS_C_DIM, 4);
    dline(l, qx + 20, y + 52, x + w - 140, y + 52, AOS_C_DIM, 4);
    dres(l, qx + 60, y + 52, x + w - 150, "33 Ω");
    dtext(l, x + w - 136, y + 36, 136, _("5V (pata 1)"), AOS_C_RED, aos_font_small, LV_TEXT_ALIGN_LEFT);
    /* the emitter to ground */
    dline(l, qx + 20, qy + 34, qx + 20, y + 270, AOS_C_DIM, 4);
    dline(l, qx - 4, y + 270, qx + 44, y + 270, AOS_C_DIM, 5);
    dline(l, qx + 6, y + 280, qx + 34, y + 280, AOS_C_DIM, 4);
    dtext(l, qx + 52, y + 256, 220, _("GND (pata 26)"), AOS_C_TEXT, aos_font_small, LV_TEXT_ALIGN_LEFT);
}

static lv_obj_t *note(lv_obj_t *parent, const char *text, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

static lv_obj_t *card_title(lv_obj_t *card, const char *text)
{
    lv_obj_t *l = lv_label_create(card);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, aos_font_body, 0);
    lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
    return l;
}

static lv_obj_t *pin_row(lv_obj_t *card, const char *what, lv_obj_t **val, lv_event_cb_t cb)
{
    lv_obj_t *r = lv_button_create(card);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), AOS_UI_TAP_MIN);
    lv_obj_set_style_bg_color(r, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, 18, 0);
    lv_obj_set_style_pad_hor(r, 20, 0);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, what);
    lv_obj_set_style_text_font(l, aos_font_body, 0);
    lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    *val = lv_label_create(r);
    lv_obj_set_style_text_font(*val, aos_font_body, 0);
    lv_obj_set_style_text_color(*val, AOS_C_ACCENT, 0);
    lv_obj_align(*val, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, NULL);
    return r;
}

static void wiring_build(lv_obj_t *page)
{
    ir_ui_header(page, _("Cableado"), NULL, NULL, NULL);
    lv_obj_t *s = ir_ui_scroll(page);
    if (A->land) {
        lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(s, 18, 0);
    }
    int32_t cw = A->land ? (A->w - 2 * AOS_UI_PAD - 18) / 2 : A->w - 2 * AOS_UI_PAD;

    lv_obj_t *c = ir_ui_card(s);
    lv_obj_set_width(c, cw);
    card_title(c, _("Pines"));
    pin_row(c, _("Receptor"), &A->pin_rx, pin_rx_cb);
    pin_row(c, _("LED"), &A->pin_tx, pin_tx_cb);
    pins_refresh();
    note(c, _("Cualquier pin libre del conector de atrás. La app los toma mientras los usa y los suelta al cerrarse."), AOS_C_DIM);
    lv_obj_t *b = ir_ui_btn(c, _("Prueba de lazo"), AOS_C_ACCENT, loop_cb, NULL);
    (void)b;
    A->loop_lbl = note(c, _("Con el LED apuntando al receptor: manda un código y mira si vuelve."), AOS_C_DIM);

    c = ir_ui_card(s);
    lv_obj_set_width(c, cw);
    card_title(c, _("El receptor"));
    lv_obj_t *d = lv_obj_create(c);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, LV_PCT(100), 200);
    lv_obj_add_event_cb(d, draw_rx, LV_EVENT_DRAW_MAIN, NULL);
    note(c, _("Un receptor con demodulador de 38 kHz (TSOP38238, VS1838B y parecidos). Alimentarlo con 3,3 V, nunca con 5 V: su salida iría con 5 V al pin, y el conector no los aguanta."), AOS_C_DIM);
    note(c, _("Las patas cambian de orden según el modelo: mirar su hoja de datos. Un capacitor de 100 nF entre VS y GND, cerca del receptor, evita lecturas falsas."), AOS_C_DIM);

    c = ir_ui_card(s);
    lv_obj_set_width(c, cw);
    card_title(c, _("El LED"));
    d = lv_obj_create(c);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, LV_PCT(100), 300);
    lv_obj_add_event_cb(d, draw_tx, LV_EVENT_DRAW_MAIN, NULL);
    note(c, _("Nunca el LED directo al pin: un GPIO da unos 20 mA y un LED IR quiere 50 a 100 mA en pulsos. El pin maneja la base de un transistor con 1 kΩ, y el LED va del colector a 5 V con su resistencia."), AOS_C_ORANGE);
    note(c, _("Con 5 V y 33 Ω pasan unos 100 mA: alcanza para 5 a 8 m apuntando. Con 3,3 V y 22 Ω, unos 70 mA y algo menos de alcance. Dos LED en serie con 15 Ω cubren más ángulo."), AOS_C_DIM);
    note(c, _("Las patas de 5 V tienen tensión aun con la placa apagada."), AOS_C_DIM);

    c = ir_ui_card(s);
    lv_obj_set_width(c, cw);
    card_title(c, _("La base de códigos"));
    note(c, _("Los códigos de televisores, aires, ventiladores y luces son de SmartIR (licencia MIT, © 2019 Vassilis Panos, 2024 Li Tin O've Weedle), empaquetados para la tarjeta en infrarrojo_p4.pak."), AOS_C_DIM);
}

static void wiring_tick(void)
{
    if (!A->loop_wait) return;
    int r = ir_hw_loop_result();
    if (r < 0) return;
    A->loop_wait = false;
    lv_label_set_text(A->loop_lbl, r ? _("Volvió: el LED y el receptor andan.") :
                                       _("No volvió nada. Revisar el cableado, la alimentación del receptor y que el LED apunte a él."));
    lv_obj_set_style_text_color(A->loop_lbl, r ? AOS_C_GREEN : AOS_C_ORANGE, 0);
}

/* ------------------------------------------------------------------- frame */

static void build(void)
{
    lv_obj_t *root = A->root;
    lv_obj_clean(root);
    A->w = lv_obj_get_width(root);
    A->h = lv_obj_get_height(root);
    A->land = A->w > A->h;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    int32_t bar_h = A->land ? 84 : 112;
    A->content = lv_obj_create(root);
    lv_obj_remove_style_all(A->content);
    lv_obj_set_size(A->content, A->w, A->h - bar_h);
    lv_obj_remove_flag(A->content, LV_OBJ_FLAG_SCROLLABLE);

    A->bar = lv_obj_create(root);
    lv_obj_remove_style_all(A->bar);
    lv_obj_set_size(A->bar, A->w, bar_h);
    lv_obj_align(A->bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(A->bar, lv_color_hex(0x121214), 0);
    lv_obj_set_style_bg_opa(A->bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(A->bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(A->bar, 1, 0);
    lv_obj_set_style_border_color(A->bar, AOS_C_CARD2, 0);
    lv_obj_set_flex_flow(A->bar, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(A->bar, LV_OBJ_FLAG_SCROLLABLE);

    static const char *GLYPH[TAB_COUNT] = { AOS_SYM_GESTURE_TAP_BUTTON, AOS_SYM_WAVEFORM, AOS_SYM_DATABASE,
                                            AOS_SYM_DEVELOPER_BOARD };
    static const char *NAME[TAB_COUNT] = { N_("Controles"), N_("Aprender"), N_("Códigos"), N_("Cableado") };
    for (int i = 0; i < TAB_COUNT; i++) {
        A->page[i] = lv_obj_create(A->content);
        lv_obj_remove_style_all(A->page[i]);
        lv_obj_set_size(A->page[i], A->w, A->h - bar_h);
        lv_obj_set_flex_flow(A->page[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(A->page[i], LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *b = lv_button_create(A->bar);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, LV_PCT(100));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_bg_color(b, AOS_C_CARD, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_t *g = lv_label_create(b);
        lv_label_set_text(g, GLYPH[i]);
        lv_obj_set_style_text_font(g, &aos_sym_44, 0);
        lv_obj_t *t = lv_label_create(b);
        lv_label_set_text(t, _(NAME[i]));
        lv_obj_set_style_text_font(t, aos_font_caption, 0);
        /* lying down the glyph and the name side by side, upright stacked */
        lv_obj_set_flex_flow(b, A->land ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(b, A->land ? 12 : 4, 0);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        A->tab_btn[i] = b;
    }
    ir_remote_build(A->page[TAB_REMOTES]);
    ir_learn_build(A->page[TAB_LEARN]);
    ir_base_build(A->page[TAB_BASE]);
    wiring_build(A->page[TAB_WIRING]);
    A->tab = s_tab;
    tab_style();
    if (A->tab == TAB_LEARN) ir_learn_shown(true);
}

static void *ir_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    A = ir_alloc(sizeof *A);
    if (!A) return NULL;
    A->root = root;
    ir_hw_start();
    ir_pack_open();
    build();
    return A;
}

static void ir_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    ir_learn_free();
    ir_remote_free();
    ir_base_free();
    ir_hw_stop();
    ir_pack_close();
    ir_free(A);
    A = NULL;
}

static void ir_show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    A->hidden = false;
    ir_remote_reload();
    if (A->tab == TAB_LEARN) ir_learn_shown(true);
}

static void ir_hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    A->hidden = true;
    A->hidden_ms = (uint32_t)aos_hal_uptime_ms();
    ir_learn_shown(false);
}

static bool ir_back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (ir_sheet_is_open()) { ir_sheet_close(); return true; }
    if (A->tab == TAB_REMOTES) return ir_remote_back();
    if (A->tab == TAB_BASE) return ir_base_back();
    return false;
}

static void ir_tick(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (!A) return;
    ir_portal_tick();
    const char *err = ir_hw_error();
    if (err[0]) aos_ui_toast(err, 3000);
    if (A->hidden) {
        /* a minute in the background with nothing to send: the LED's pin goes back */
        if ((uint32_t)aos_hal_uptime_ms() - A->hidden_ms > 60000) {
            ir_hw_idle_close();
            A->hidden_ms = (uint32_t)aos_hal_uptime_ms();
        }
        return;
    }
    ir_remote_tick();
    ir_learn_tick();
    ir_base_tick();
    wiring_tick();
}

static bool ir_init(aos_app_t *app)
{
    app->desc.id       = "aos.infrarrojo";     /* IR_APP_ID, written out for gen_lang.py */
    app->desc.name     = "Infrarrojo";
    app->desc.icon     = LV_SYMBOL_EYE_OPEN;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0xE0303A;
    app->desc.color_b  = 0x5C0E26;
    app->desc.flags    = AOS_APP_FLAG_BACKGROUND;
    app->desc.order    = 330;
    aos_icon_set_ops(app, IR_ICON, sizeof IR_ICON);

    app->create  = ir_create;
    app->destroy = ir_destroy;
    app->show    = ir_show;
    app->hide    = ir_hide;
    app->back    = ir_back;
    app->tick    = ir_tick;
    return true;
}

AOS_APP_ENTRY(ir_init);
