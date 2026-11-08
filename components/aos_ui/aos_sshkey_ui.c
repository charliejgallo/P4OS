/*
 * P4OS - an SSH key not on the list asking to be let in (aos_access_ssh.c):
 * its type, its fingerprint, the user it came as and from where, over
 * everything, with "Rechazar" and "Permitir". The connection waits for the
 * answer (a minute at most).
 *
 * Unlike the Bluetooth code, it waits for the lock screen to go: with a
 * code on the board, letting a key in is for whoever knows the code. Back
 * and the BOOT button are a no.
 */
#include "aos_sshkey_ui.h"
#include "aos_access.h"
#include "aos_lock.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *s_root;
static aos_access_ssh_ask_t s_shown;

static void close_it(void)
{
    if (s_root) lv_obj_delete(s_root);
    s_root = NULL;
    memset(&s_shown, 0, sizeof s_shown);
}

bool aos_sshkey_ui_visible(void) { return s_root != NULL; }

static void answer(bool yes)
{
    lv_obj_t *r = s_root;
    s_root = NULL;
    if (r) lv_obj_delete_async(r);      /* from a button's own event */
    aos_access_ssh_answer(yes);
}

static void yes_cb(lv_event_t *e) { (void)e; answer(true); }
static void no_cb(lv_event_t *e) { (void)e; answer(false); }

void aos_sshkey_ui_cancel(void)
{
    if (!s_root) return;
    close_it();
    aos_access_ssh_answer(false);
}

static lv_obj_t *wrapped(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = aos_label(parent, text, font, color);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static void open_it(const aos_access_ssh_ask_t *q)
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
    lv_obj_set_width(card, LV_MIN(w - 64, 620));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 32, 0);
    lv_obj_set_style_pad_all(card, 36, 0);
    lv_obj_set_style_pad_row(card, 18, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    aos_label(card, LV_SYMBOL_KEYBOARD, aos_font_title, AOS_C_ACCENT);
    wrapped(card, _("¿Dejar entrar esta clave por SSH?"), aos_font_body, AOS_C_TEXT);
    char buf[160];
    snprintf(buf, sizeof buf, _("%s, como «%s», desde %s"), q->type, q->comment, q->from);
    wrapped(card, buf, aos_font_small, AOS_C_DIM);
    /* the fingerprint as ssh-keygen -lf prints it: what to compare */
    wrapped(card, q->fp, aos_font_small, AOS_C_GREEN);
    wrapped(card, _("Queda en la lista de Ajustes, Portal web, hasta que la saques."), aos_font_small, AOS_C_DIM);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 20, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    aos_button(row, _("Rechazar"), AOS_C_CARD2, no_cb, NULL);
    aos_button(row, _("Permitir"), AOS_C_GREEN, yes_cb, NULL);
}

void aos_sshkey_ui_tick(void)
{
    aos_access_ssh_ask_t q;
    if (!aos_access_ssh_asking(&q)) {
        close_it();                     /* answered, or the minute went by */
        return;
    }
    if (aos_lock_covers()) {
        close_it();
        aos_hal_activity();             /* the screen on, so the lock screen is there to open */
        return;
    }
    aos_hal_activity();
    if (s_root && !memcmp(&q, &s_shown, sizeof q)) return;
    close_it();
    s_shown = q;
    open_it(&q);
    aos_hal_beep(1320, 90);
}

void aos_sshkey_ui_layout(void)
{
    if (!s_root) return;
    aos_access_ssh_ask_t q = s_shown;
    close_it();
    s_shown = q;
    open_it(&q);
}
