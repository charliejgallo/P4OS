/*
 * P4OS - Banners (a notification arriving) and toasts (a short notice).
 *
 * Both live on lv_layer_sys(), over the panels. A banner drops from the top
 * for four seconds; a tap opens the notification centre, a swipe up dismisses
 * it. The policy - whether it alerts at all, with sound or not, do not
 * disturb - is aos_notif.c's (from AmoledOS), not this file's: here the
 * `alert` and `sound` fields of each notification are just obeyed.
 *
 * An incoming call from the phone is no banner: it takes the whole screen,
 * over the lock screen too, with the phone's Answer and Reject, and rings
 * until the phone withdraws it (answered, rejected, or given up).
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

/* ---- the phone's actions (ANCS) ---- */

static void action_failed_check(lv_timer_t *t)
{
    if (aos_hal_notif_action_failed()) aos_toast_show(_("El teléfono no pudo hacerlo"), 2200);
}

/* Answer, reject, clear: the phone answers by withdrawing the notification;
 * if it could not do it, it says so a moment later and a toast tells. */
void aos_notif_act(uint32_t uid, bool positive)
{
    aos_hal_activity();
    if (!aos_hal_notif_action(uid, positive)) {
        aos_toast_show(_("El teléfono no está conectado"), 2000);
        return;
    }
    lv_timer_t *t = lv_timer_create(action_failed_check, 1500, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/* ---- an incoming call ---- */

#define CALL_MAX_MS  90000                  /* the phone gave up long before */

static lv_obj_t *s_call;
static lv_timer_t *s_ring;
static aos_notif_t s_call_n;
static uint32_t s_call_t0;

static void call_close(void)
{
    if (s_ring) { lv_timer_delete(s_ring); s_ring = NULL; }
    if (s_call) { lv_obj_delete(s_call); s_call = NULL; }
}

static void ring_cb(lv_timer_t *t)
{
    if (lv_tick_elaps(s_call_t0) > CALL_MAX_MS) { call_close(); return; }
    aos_hal_activity();                     /* the screen stays on while it rings */
    if (s_call_n.sound) aos_hal_beep(1320, 140);
}

static void call_act_cb(lv_event_t *e)
{
    bool positive = lv_event_get_user_data(e) != NULL;
    uint32_t uid = s_call_n.uid;
    call_close();
    aos_notif_act(uid, positive);
}

static lv_obj_t *call_button(lv_obj_t *parent, const char *glyph, uint32_t color, const char *text, bool positive)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 220, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 16, 0);
    lv_obj_t *b = lv_obj_create(col);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 150, 150);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, call_act_cb, LV_EVENT_CLICKED, positive ? (void *)1 : NULL);
    lv_obj_t *g = lv_label_create(b);
    lv_obj_set_style_text_font(g, &aos_sym_72, 0);
    lv_obj_set_style_text_color(g, lv_color_white(), 0);
    lv_label_set_text(g, glyph);
    lv_obj_center(g);
    lv_obj_t *l = lv_label_create(col);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, text);
    return col;
}

/* Drawn again when the screen turns, from the notification it keeps. */
static void call_build(void)
{
    const aos_geo_t *g = aos_ui_geo();
    if (s_call) lv_obj_delete(s_call);
    s_call = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_call);
    lv_obj_set_size(s_call, g->w, g->h);
    lv_obj_set_style_bg_color(s_call, lv_color_hex(0x0B0D10), 0);
    lv_obj_set_style_bg_opa(s_call, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_call, LV_OBJ_FLAG_CLICKABLE);             /* nothing under it gets the touch */
    lv_obj_remove_flag(s_call, LV_OBJ_FLAG_SCROLLABLE);

    char safe[320];
    lv_obj_t *head = lv_label_create(s_call);
    lv_obj_set_style_text_font(head, aos_font_small, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(0x8A93A3), 0);
    aos_text_safe(safe, sizeof safe, s_call_n.app);
    lv_label_set_text_fmt(head, "%s  ·  %s", _("Llamada entrante"), safe);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, g->bar_h + (g->landscape ? 40 : 160));

    lv_obj_t *name = lv_label_create(s_call);
    lv_obj_set_width(name, g->w - 80);
    lv_obj_set_style_text_font(name, aos_font_large, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_WRAP);
    aos_text_safe(safe, sizeof safe, s_call_n.title[0] ? s_call_n.title : _("Número desconocido"));
    lv_label_set_text(name, safe);
    lv_obj_align_to(name, head, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);

    if (s_call_n.message[0]) {
        lv_obj_t *m = lv_label_create(s_call);
        lv_obj_set_width(m, g->w - 80);
        lv_obj_set_style_text_font(m, aos_font_body, 0);
        lv_obj_set_style_text_color(m, lv_color_hex(0xB8C0CC), 0);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
        aos_text_safe(safe, sizeof safe, s_call_n.message);
        lv_label_set_text(m, safe);
        lv_obj_align_to(m, name, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
    }

    lv_obj_t *row = lv_obj_create(s_call);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_MIN(g->w - 40, 700), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -(g->bottom_h + (g->landscape ? 30 : 140)));
    /* only the ones the phone declared (aos_notif_t) */
    if (s_call_n.can_negative) call_button(row, AOS_SYM_PHONE_HANGUP, 0xE5352B, _("Rechazar"), false);
    if (s_call_n.can_positive) call_button(row, AOS_SYM_PHONE, 0x30B556, _("Atender"), true);
}

static void call_show(const aos_notif_t *n)
{
    banner_hide();
    s_call_n = *n;
    s_call_t0 = lv_tick_get();
    call_build();
    if (!s_ring) s_ring = lv_timer_create(ring_cb, 2000, NULL);
    ring_cb(s_ring);
}

void aos_banner_tick(void)
{
    uint32_t uid;
    while (aos_hal_notif_pop_removed(&uid))     /* the history refreshes itself */
        if (s_call && uid == s_call_n.uid) call_close();     /* answered or hung up over there */
    aos_notif_t n;
    while (aos_hal_notif_pop(&n)) {
        if (!n.alert) continue;
        aos_hal_activity();
        if (n.category == AOS_NOTIF_CALL_INCOMING && (n.can_positive || n.can_negative) && !n.pre_existing) {
            call_show(&n);
            continue;
        }
        /* locked, the lock screen's list shows it: no banner over it, and
         * none with its text when the content is to stay hidden */
        if (!aos_lock_is_locked()) banner_show(&n);
        if (n.sound) aos_hal_beep(1760, 60);
    }
}

void aos_banner_create(lv_obj_t *layer) { s_layer = layer; }

void aos_banner_layout(void)
{
    if (s_call) call_build();
    if (s_banner) { lv_obj_delete(s_banner); s_banner = NULL; }
    if (s_toast) { lv_obj_delete(s_toast); s_toast = NULL; }
}

/* ---- toast ---- */

static void toast_done(lv_timer_t *t)
{
    s_toast_timer = NULL;
    if (s_toast) { lv_obj_delete(s_toast); s_toast = NULL; }
}

bool aos_banner_up(void) { return s_banner != NULL || s_call != NULL; }
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
