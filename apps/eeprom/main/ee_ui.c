/*
 * P4OS - EEPROM: the small pieces every page is made of.
 *
 * Cards, rows and pills in the look of the Bus app, and the sheets that come
 * up over the app: a list to pick from, a confirmation, a line of text on the
 * system keyboard, a number on a hex keypad. One sheet at a time; a sheet is
 * closed from inside its own events, so it is hidden at once and deleted on
 * the next turn of LVGL's loop, and a page rebuilt in the meantime forgets
 * it (the trap Notas measured: HANDOFF-APPS.md, section 8).
 */
#include "ee.h"

#include "aos_sys_glyphs.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lv_obj_t *ee_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *ee_card(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *c = ee_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_pad_row(c, 12, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    return c;
}

lv_obj_t *ee_column(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_set_style_pad_bottom(c, 24, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    return c;
}

lv_obj_t *ee_pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = ee_box(parent, LV_SIZE_CONTENT, 76);
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
    if (glyph) aos_make_decorative(aos_label(b, glyph, &aos_sym_28, lv_color_white()));
    if (text) aos_make_decorative(aos_label(b, text, aos_font_body, lv_color_white()));
    return b;
}

void ee_set_disabled(lv_obj_t *o, bool off)
{
    if (!o) return;
    if (off) {
        lv_obj_add_state(o, LV_STATE_DISABLED);
        lv_obj_set_style_opa(o, LV_OPA_40, 0);
    } else {
        lv_obj_remove_state(o, LV_STATE_DISABLED);
        lv_obj_set_style_opa(o, LV_OPA_COVER, 0);
    }
}

lv_obj_t *ee_caption(lv_obj_t *parent, const char *text, int32_t w)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

lv_obj_t *ee_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(l, 8, 0);
    return l;
}

lv_obj_t *ee_row(lv_obj_t *parent, int32_t w, const char *label, const char *value, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *r = ee_box(parent, w, 72);
    lv_obj_set_style_radius(r, 16, 0);
    if (cb) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
        lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    }
    lv_obj_t *l = aos_label(r, label, aos_font_body, AOS_C_TEXT);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
    aos_make_decorative(l);
    int32_t right = cb ? 40 : 4;
    if (cb) {
        lv_obj_t *ch = aos_label(r, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, AOS_C_DIM);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, 0, 0);
        aos_make_decorative(ch);
    }
    lv_obj_t *v = aos_label(r, value ? value : "", aos_font_body, AOS_C_DIM);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_update_layout(l);
    int32_t vw = w - lv_obj_get_width(l) - right - 32;
    lv_obj_set_style_max_width(v, vw > 60 ? vw : 60, 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -right, 0);
    aos_make_decorative(v);
    return v;
}

lv_obj_t *ee_seg(lv_obj_t *parent, int32_t w, const char *const *items, int n, int sel, lv_event_cb_t cb)
{
    lv_obj_t *s = ee_box(parent, w, 68);
    lv_obj_set_style_bg_color(s, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s, 18, 0);
    lv_obj_set_style_pad_all(s, 4, 0);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW);
    int32_t iw = (w - 8) / n;
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = ee_box(s, iw, 60);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_bg_color(b, i == sel ? C_AMBER : AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = aos_label(b, items[i], aos_font_small, i == sel ? lv_color_black() : AOS_C_TEXT);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(l, iw - 12, 0);
        lv_obj_center(l);
        aos_make_decorative(l);
    }
    return s;
}

/* -------------------------------------------------------------------------- */
/* Sheets                                                                      */
/* -------------------------------------------------------------------------- */

static lv_obj_t *s_overlay, *s_dying;
static ee_pick_cb_t s_pick;
static void *s_pick_ud;

static void dying_cb(void *o)
{
    if (o == s_dying) s_dying = NULL;
    lv_obj_delete(o);
}

void ee_sheet_close(void)
{
    if (!s_overlay) return;
    lv_obj_t *o = s_overlay;
    s_overlay = NULL;
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    if (s_dying) {
        lv_async_call_cancel(dying_cb, s_dying);
        lv_obj_delete(s_dying);
    }
    s_dying = o;
    lv_async_call(dying_cb, o);
}

void ee_sheet_forget(void)
{
    if (s_dying) {
        lv_async_call_cancel(dying_cb, s_dying);
        s_dying = NULL;
    }
    s_overlay = NULL;
}

bool ee_sheet_open(void) { return s_overlay != NULL; }

static void overlay_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) ee_sheet_close();
}

static void overlay_delete_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    if (o == s_overlay) s_overlay = NULL;
    if (o == s_dying) {
        lv_async_call_cancel(dying_cb, o);
        s_dying = NULL;
    }
}

static lv_obj_t *frame(const char *title, int32_t *cw, bool top)
{
    ee_sheet_close();
    s_pick = NULL;
    lv_obj_t *root = U.root;
    s_overlay = ee_box(root, U.W, U.H);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_60, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_FLOATING);
    lv_obj_add_event_cb(s_overlay, overlay_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_overlay, overlay_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *card = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(card);
    int32_t w = U.land ? 720 : U.W - 32;
    lv_obj_set_size(card, w, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, U.H * 88 / 100, 0);
    lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    if (top) lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 16);
    else if (U.land) lv_obj_center(card);
    else lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -16);
    if (title && title[0]) {
        lv_obj_t *t = aos_label(card, title, aos_font_small, AOS_C_DIM);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(t, w - 56);
        lv_obj_set_style_pad_hor(t, 12, 0);
        lv_obj_set_style_pad_ver(t, 8, 0);
    }
    if (cw) *cw = w - 32;
    return card;
}

lv_obj_t *ee_sheet_custom(const char *title, int32_t *content_w) { return frame(title, content_w, false); }

static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ee_pick_cb_t cb = s_pick;
    void *ud = s_pick_ud;
    ee_sheet_close();
    if (cb) cb(i, ud);
}

lv_obj_t *ee_sheet(const char *title, const ee_item_t *items, int n, ee_pick_cb_t cb, void *ud)
{
    int32_t cw;
    lv_obj_t *card = frame(title, &cw, false);
    s_pick = cb;
    s_pick_ud = ud;
    for (int i = 0; i < n; i++) {
        lv_obj_t *r = ee_box(card, cw, items[i].sub ? 100 : 84);
        lv_obj_set_style_radius(r, 18, 0);
        lv_obj_set_style_bg_color(r, items[i].checked ? lv_color_hex(0x3A2A0A) : AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(r, items[i].checked ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(r, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_color_t c = items[i].danger ? AOS_C_RED : items[i].checked ? C_AMBER : AOS_C_TEXT;
        lv_obj_t *l = aos_label(r, items[i].text, aos_font_body, c);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, cw - 40);
        lv_obj_align(l, items[i].sub ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, 20, items[i].sub ? 12 : 0);
        aos_make_decorative(l);
        if (items[i].sub) {
            lv_obj_t *s = aos_label(r, items[i].sub, aos_font_caption, AOS_C_DIM);
            lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(s, cw - 40);
            lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 20, -12);
            aos_make_decorative(s);
        }
    }
    return card;
}

/* -------------------------------------------------------------------------- */
/* Confirmation                                                                */
/* -------------------------------------------------------------------------- */

static void (*s_yes)(void *);
static void *s_yes_ud;

static void confirm_cb(lv_event_t *e)
{
    bool yes = (bool)(intptr_t)lv_event_get_user_data(e);
    void (*cb)(void *) = s_yes;
    void *ud = s_yes_ud;
    ee_sheet_close();
    if (yes && cb) cb(ud);
}

static lv_obj_t *dialog_btn(lv_obj_t *row, const char *label, lv_color_t bg, lv_color_t fg, int32_t w, bool yes,
                            lv_event_cb_t cb)
{
    lv_obj_t *b = ee_box(row, w, 84);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = aos_label(b, label, aos_font_body, fg);
    lv_obj_center(l);
    aos_make_decorative(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)yes);
    return b;
}

static lv_obj_t *btn_row(lv_obj_t *card, int32_t cw)
{
    lv_obj_t *row = ee_box(card, cw, 96);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    return row;
}

void ee_confirm(const char *title, const char *text, const char *yes, bool danger, void (*cb)(void *ud), void *ud)
{
    int32_t cw;
    lv_obj_t *card = frame(title, &cw, false);
    s_yes = cb;
    s_yes_ud = ud;
    lv_obj_t *t = aos_label(card, text, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, cw - 24);
    lv_obj_set_style_pad_hor(t, 12, 0);
    lv_obj_set_style_pad_bottom(t, 8, 0);
    lv_obj_t *row = btn_row(card, cw);
    if (yes) {
        int32_t bw = (cw - 16) / 2;
        dialog_btn(row, _("Cancelar"), AOS_C_CARD2, AOS_C_TEXT, bw, false, confirm_cb);
        dialog_btn(row, yes, danger ? AOS_C_RED : AOS_C_ACCENT, lv_color_white(), bw, true, confirm_cb);
    } else {
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
        dialog_btn(row, _("Aceptar"), AOS_C_ACCENT, lv_color_white(), (cw - 16) / 2, true, confirm_cb);
    }
}

/* -------------------------------------------------------------------------- */
/* A line of text                                                              */
/* -------------------------------------------------------------------------- */

static void (*s_text_cb)(const char *, void *);
static void *s_text_ud;
static lv_obj_t *s_ta;

static void text_done(bool ok)
{
    char buf[128];
    snprintf(buf, sizeof buf, "%s", s_ta ? lv_textarea_get_text(s_ta) : "");
    void (*cb)(const char *, void *) = s_text_cb;
    void *ud = s_text_ud;
    s_ta = NULL;
    ee_sheet_close();
    if (ok && cb) cb(buf, ud);
}

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) text_done(true);
    else if (c == LV_EVENT_CANCEL) text_done(false);
}

static void text_btn_cb(lv_event_t *e) { text_done((bool)(intptr_t)lv_event_get_user_data(e)); }

void ee_ask_text(const char *title, const char *initial, int max, void (*cb)(const char *, void *), void *ud)
{
    int32_t cw;
    lv_obj_t *card = frame(title, &cw, true);
    s_text_cb = cb;
    s_text_ud = ud;
    s_ta = lv_textarea_create(card);
    lv_obj_set_width(s_ta, cw);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_max_length(s_ta, (uint32_t)max);
    lv_textarea_set_text(s_ta, initial ? initial : "");
    lv_obj_set_style_text_font(s_ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(s_ta, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(s_ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(s_ta, 0, 0);
    lv_obj_set_style_radius(s_ta, 16, 0);
    lv_obj_set_style_pad_all(s_ta, 16, 0);
    lv_obj_add_state(s_ta, LV_STATE_FOCUSED);
    lv_obj_t *row = btn_row(card, cw);
    int32_t bw = (cw - 16) / 2;
    dialog_btn(row, _("Cancelar"), AOS_C_CARD2, AOS_C_TEXT, bw, false, text_btn_cb);
    dialog_btn(row, _("Listo"), AOS_C_ACCENT, lv_color_white(), bw, true, text_btn_cb);

    lv_obj_t *kb = lv_keyboard_create(s_overlay);
    lv_obj_set_size(kb, U.W, U.land ? U.H * 52 / 100 : U.H * 36 / 100);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(kb, aos_font_body);
    lv_keyboard_set_textarea(kb, s_ta);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_CANCEL, NULL);
}

/* -------------------------------------------------------------------------- */
/* A number in hex                                                             */
/* -------------------------------------------------------------------------- */

static void (*s_hex_cb)(uint32_t, void *);
static void *s_hex_ud;
static lv_obj_t *s_hex_val;
static char s_hex[12];
static int s_hex_max;

static void hex_show(void)
{
    if (s_hex_val) lv_label_set_text_fmt(s_hex_val, "0x%s", s_hex[0] ? s_hex : "_");
}

static void hex_key_cb(lv_event_t *e)
{
    lv_obj_t *m = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(m);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    size_t n = strlen(s_hex);
    if (id < 16) {
        if ((int)n < s_hex_max) { s_hex[n] = "0123456789ABCDEF"[id]; s_hex[n + 1] = 0; }
    } else if (id == 16) {
        if (n) s_hex[n - 1] = 0;
    } else {
        uint32_t v = (uint32_t)strtoul(s_hex[0] ? s_hex : "0", NULL, 16);
        void (*cb)(uint32_t, void *) = s_hex_cb;
        void *ud = s_hex_ud;
        s_hex_val = NULL;
        ee_sheet_close();
        if (cb) cb(v, ud);
        return;
    }
    hex_show();
}

void ee_ask_hex(const char *title, uint32_t initial, int max_digits, void (*cb)(uint32_t, void *), void *ud)
{
    int32_t cw;
    lv_obj_t *card = frame(title, &cw, false);
    s_hex_cb = cb;
    s_hex_ud = ud;
    s_hex_max = max_digits > 8 ? 8 : max_digits;
    snprintf(s_hex, sizeof s_hex, "%X", (unsigned)initial);
    if (!initial) s_hex[0] = 0;
    s_hex_val = aos_label(card, "", aos_font_large, C_AMBER);
    lv_obj_set_style_pad_hor(s_hex_val, 12, 0);
    hex_show();
    static const char *map[24];
    static const char *const D[16] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C", "D", "E", "F" };
    int k = 0;
    for (int i = 0; i < 16; i++) {
        map[k++] = D[i];
        if (i % 4 == 3) map[k++] = "\n";
    }
    map[k++] = _("Borrar");
    map[k++] = _("Listo");
    map[k] = "";
    lv_obj_t *m = lv_buttonmatrix_create(card);
    lv_buttonmatrix_set_map(m, map);
    lv_obj_set_size(m, cw, U.land ? 420 : 480);
    lv_obj_set_style_text_font(m, aos_font_title, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_pad_gap(m, 8, 0);
    lv_obj_set_style_bg_color(m, AOS_C_CARD2, LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, AOS_C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_radius(m, 16, LV_PART_ITEMS);
    lv_obj_set_style_border_width(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, AOS_C_DIM, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_buttonmatrix_set_button_ctrl(m, 17, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_style_bg_color(m, AOS_C_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(m, aos_font_body, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(m, hex_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
}
