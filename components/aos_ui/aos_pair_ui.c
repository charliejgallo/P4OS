/*
 * P4OS - the pairing request, over everything (from AmoledOS).
 *
 * When the phone asks to pair, aos_ble has a six-digit code for numeric
 * comparison (aos_hal_bt_pair_code) and waits for an answer. This puts the
 * code on lv_layer_top, over the app and over the lock screen too: the phone
 * is waiting, and the code proves nothing to whoever does not hold the
 * phone. "Coincide" accepts, "Cancelar" (or Back, or the BOOT button)
 * rejects. The screen stays awake while it waits.
 */
#include "aos_pair_ui.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>

static lv_obj_t *s_root;
static uint32_t s_shown;

static void close_it(void)
{
    if (s_root) lv_obj_delete(s_root);
    s_root = NULL;
    s_shown = 0;
}

bool aos_pair_ui_visible(void) { return s_root != NULL; }

/* From a button's own event: delete after the event is done with it */
static void answer(bool yes)
{
    lv_obj_t *r = s_root;
    s_root = NULL;
    s_shown = 0;
    if (r) lv_obj_delete_async(r);
    aos_hal_bt_pair_confirm(yes);
}

static void yes_cb(lv_event_t *e) { (void)e; answer(true); }
static void no_cb(lv_event_t *e) { (void)e; answer(false); }

void aos_pair_ui_cancel(void)
{
    if (!s_root) return;
    close_it();
    aos_hal_bt_pair_confirm(false);
}

static void open_it(uint32_t code)
{
    lv_obj_t *top = lv_layer_top();
    int32_t w = lv_obj_get_width(top), h = lv_obj_get_height(top);
    s_root = lv_obj_create(top);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, w, h);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_70, 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);     /* nothing under it takes the touch */
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_root);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_MIN(w - 64, 600));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 32, 0);
    lv_obj_set_style_pad_all(card, 36, 0);
    lv_obj_set_style_pad_row(card, 24, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    aos_label(card, LV_SYMBOL_BLUETOOTH, aos_font_title, AOS_C_ACCENT);
    lv_obj_t *t = aos_label(card, _("¿El teléfono muestra este número?"), aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(t, lv_pct(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);

    char buf[16];
    snprintf(buf, sizeof buf, "%03u %03u", (unsigned)(code / 1000), (unsigned)(code % 1000));
    aos_label(card, buf, aos_font_huge, AOS_C_GREEN);

    /* ROW_WRAP: in German the two buttons do not fit side by side */
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 20, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    aos_button(row, _("Cancelar"), AOS_C_CARD2, no_cb, NULL);
    aos_button(row, _("Coincide"), AOS_C_GREEN, yes_cb, NULL);
}

void aos_pair_ui_tick(void)
{
    uint32_t code = aos_hal_bt_pair_code();
    if (!code) {
        close_it();                     /* confirmed, cancelled, or the phone went away */
        return;
    }
    aos_hal_activity();                 /* comparing two numbers takes longer than the screen's timeout */
    if (s_root && code == s_shown) return;
    close_it();
    s_shown = code;
    open_it(code);
    aos_hal_beep(1320, 90);
}

void aos_pair_ui_layout(void)
{
    uint32_t code = s_shown;
    if (!s_root) return;
    close_it();
    s_shown = code;
    open_it(code);
}
