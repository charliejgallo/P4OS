/*
 * P4OS - a new board with a backup on its card (components/aos_backup,
 * docs/plan/RESPALDO.md, stage 2).
 *
 * A board whose flash was erased starts knowing nothing: no network, no
 * settings. When its card has a backup, this says so once, over the home
 * screen, before the person sets the Wi-Fi by hand: when it was made, by
 * which P4OS, how much it holds, and whether it asks for a passphrase.
 * "Ver respaldos" opens Settings, Backup, where restoring one is two taps
 * (and its passphrase); "Ahora no" leaves it for later, in the same place.
 * Either answer is the last time it asks.
 *
 * What makes a board new is in aos_backup_offer(): it knows no network and
 * never answered this, nor made a backup of its own.
 */
#include "aos_backup_ui.h"
#include "aos_backup.h"
#include "aos_lock.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* The start has the card's apps to read first, and the home screen to
 * draw: the look is taken once it is all up. */
#define LOOK_AFTER_MS 6000

static lv_obj_t *s_root;
static bool s_looked;
/* PSRAM: one record, read once (docs/MEMORY.md). */
AOS_BSS_PSRAM static aos_backup_info_t s_one;

static void close_it(void)
{
    if (s_root) lv_obj_delete(s_root);
    s_root = NULL;
}

bool aos_backup_ui_visible(void) { return s_root != NULL; }

static void answer(bool open)
{
    lv_obj_t *r = s_root;
    s_root = NULL;
    if (r) lv_obj_delete_async(r);      /* from a button's own event */
    aos_backup_offer_done();
    if (open) aos_ui_open_app_with("aos.settings", "backup");
}

static void yes_cb(lv_event_t *e) { (void)e; answer(true); }
static void no_cb(lv_event_t *e) { (void)e; answer(false); }

void aos_backup_ui_cancel(void)
{
    if (!s_root) return;
    close_it();
    aos_backup_offer_done();
}

static lv_obj_t *wrapped(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = aos_label(parent, text, font, color);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static void open_it(void)
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

    aos_label(card, LV_SYMBOL_SD_CARD, aos_font_title, AOS_C_ACCENT);
    wrapped(card, _("Hay un respaldo en la tarjeta"), aos_font_body, AOS_C_TEXT);
    char when[40] = "", buf[200];
    time_t t = (time_t)s_one.made;
    struct tm tm;
    if (t && localtime_r(&t, &tm)) strftime(when, sizeof when, "%d/%m/%Y %H:%M", &tm);
    snprintf(buf, sizeof buf, _("Del %s, de «%.30s» con P4OS %.20s: %d ajustes y %d apps."), when[0] ? when : _("sin fecha"),
             s_one.board, s_one.fw, s_one.prefs, s_one.apps);
    wrapped(card, buf, aos_font_small, AOS_C_DIM);
    wrapped(card, s_one.sealed ? _("Está cifrado: pide su frase.")
                               : _("No lleva contraseñas: la de la Wi-Fi se escribe de nuevo."), aos_font_small, AOS_C_DIM);
    wrapped(card, _("Queda en Ajustes, Respaldo, con los demás que haya."), aos_font_small, AOS_C_DIM);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 20, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    aos_button(row, _("Ahora no"), AOS_C_CARD2, no_cb, NULL);
    aos_button(row, _("Ver respaldos"), AOS_C_ACCENT, yes_cb, NULL);
}

void aos_backup_ui_tick(void)
{
    if (s_looked || aos_hal_uptime_ms() < LOOK_AFTER_MS) return;
    if (aos_lock_covers()) return;          /* for whoever can open the board; asked again once it is */
    s_looked = true;
    if (aos_backup_offer(&s_one)) open_it();
}

void aos_backup_ui_layout(void)
{
    if (!s_root) return;
    close_it();
    open_it();
}
