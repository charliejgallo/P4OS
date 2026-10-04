/*
 * P4OS - Banners (a notification arriving) and toasts (a short notice).
 *
 * Both live on lv_layer_sys(), over the panels. A banner drops from the top
 * for four seconds; a tap opens the notification centre, a swipe up dismisses
 * it. The policy - whether it alerts at all, with sound or not, do not
 * disturb - is aos_notif.c's (from AmoledOS), not this file's: here the
 * `alert` and `sound` fields of each notification are just obeyed.
 */
#include "aos_internal.h"
#include "aos_text_safe.h"
#include "aos_lock.h"
#include "aos_hal.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"

#include <string.h>

static lv_obj_t *s_layer, *s_banner, *s_toast;
static lv_timer_t *s_banner_timer, *s_toast_timer;

static void y_cb(void *var, int32_t v) { lv_obj_set_y(var, v); }

static void banner_gone(lv_anim_t *a)
{
    if (s_banner) { lv_obj_delete(s_banner); s_banner = NULL; }
}

static void banner_hide(void)
{
    if (!s_banner) return;
    if (s_banner_timer) { lv_timer_delete(s_banner_timer); s_banner_timer = NULL; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_banner);
    lv_anim_set_exec_cb(&a, y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(s_banner), -lv_obj_get_height(s_banner) - 20);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_completed_cb(&a, banner_gone);
    lv_anim_start(&a);
}

static void banner_timeout(lv_timer_t *t) { s_banner_timer = NULL; banner_hide(); }

static void banner_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_CLICKED) { banner_hide(); aos_panel_open(AOS_PANEL_NOTIF); }
    else if (c == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) banner_hide();
}

static void banner_show(const aos_notif_t *n)
{
    const aos_geo_t *g = aos_ui_geo();
    if (s_banner) { lv_obj_delete(s_banner); s_banner = NULL; }
    if (s_banner_timer) { lv_timer_delete(s_banner_timer); s_banner_timer = NULL; }

    s_banner = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_width(s_banner, LV_MIN(g->w - 32, 700));
    lv_obj_set_height(s_banner, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_banner, 32, 0);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(0x2A2F38), 0);
    lv_obj_set_style_bg_opa(s_banner, 245, 0);
    lv_obj_set_style_pad_all(s_banner, 22, 0);
    lv_obj_set_style_text_color(s_banner, lv_color_white(), 0);
    lv_obj_set_flex_flow(s_banner, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_banner, 4, 0);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_banner, banner_event, LV_EVENT_ALL, NULL);

    lv_obj_t *head = lv_label_create(s_banner);
    lv_obj_set_style_text_font(head, aos_font_caption, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(0xB8C0CC), 0);
    char safe[320];
    aos_text_safe(safe, sizeof safe, n->app);
    lv_label_set_text(head, safe);
    lv_obj_t *t = lv_label_create(s_banner);
    lv_obj_set_style_text_font(t, aos_font_small, 0);
    aos_text_safe(safe, sizeof safe, n->title);
    lv_label_set_text(t, safe);
    lv_obj_t *m = lv_label_create(s_banner);
    lv_obj_set_width(m, lv_pct(100));
    lv_obj_set_style_text_font(m, aos_font_caption, 0);
    lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_height(m, 80, 0);
    aos_text_safe(safe, sizeof safe, n->message);
    lv_label_set_text(m, safe);

    lv_obj_update_layout(s_banner);
    int32_t h = lv_obj_get_height(s_banner);
    lv_obj_set_x(s_banner, (g->w - lv_obj_get_width(s_banner)) / 2);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_banner);
    lv_anim_set_exec_cb(&a, y_cb);
    lv_anim_set_values(&a, -h - 20, g->bar_h + 6);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    s_banner_timer = lv_timer_create(banner_timeout, 4000, NULL);
    lv_timer_set_repeat_count(s_banner_timer, 1);
}

void aos_banner_tick(void)
{
    uint32_t uid;
    while (aos_hal_notif_pop_removed(&uid)) { /* the history refreshes itself */ }
    aos_notif_t n;
    while (aos_hal_notif_pop(&n)) {
        if (!n.alert) continue;
        aos_hal_activity();
        /* locked, the lock screen's list shows it: no banner over it, and
         * none with its text when the content is to stay hidden */
        if (!aos_lock_is_locked()) banner_show(&n);
        if (n.sound) aos_hal_beep(1760, 60);
    }
}

void aos_banner_create(lv_obj_t *layer) { s_layer = layer; }

void aos_banner_layout(void)
{
    if (s_banner) { lv_obj_delete(s_banner); s_banner = NULL; }
    if (s_toast) { lv_obj_delete(s_toast); s_toast = NULL; }
}

/* ---- toast ---- */

static void toast_done(lv_timer_t *t)
{
    s_toast_timer = NULL;
    if (s_toast) { lv_obj_delete(s_toast); s_toast = NULL; }
}

bool aos_banner_up(void) { return s_banner != NULL; }
bool aos_toast_up(void) { return s_toast != NULL; }

void aos_toast_show(const char *text, uint32_t ms)
{
    const aos_geo_t *g = aos_ui_geo();
    if (s_toast) lv_obj_delete(s_toast);
    if (s_toast_timer) lv_timer_delete(s_toast_timer);
    s_toast = lv_obj_create(s_layer ? s_layer : lv_layer_sys());
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_toast, g->w - 80, 0);
    lv_obj_set_style_radius(s_toast, 40, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x2A2F38), 0);
    lv_obj_set_style_bg_opa(s_toast, 240, 0);
    lv_obj_set_style_pad_hor(s_toast, 32, 0);
    lv_obj_set_style_pad_ver(s_toast, 18, 0);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(s_toast);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(l, text ? text : "");
    lv_obj_update_layout(s_toast);
    if (lv_obj_get_width(l) > g->w - 144) lv_obj_set_width(l, g->w - 144);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -(g->bottom_h + 60));
    s_toast_timer = lv_timer_create(toast_done, ms ? ms : 1800, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}
