/*
 * P4OS - App switcher.
 *
 * The apps kept alive (AOS_APP_FLAG_KEEP / BACKGROUND), most recent first,
 * as cards in a row: a tap brings one to the front, a swipe up on it ends it.
 * Opened by resting the finger halfway up the home gesture.
 *
 * A card shows the app's icon and name over a darker version of its colour.
 * Real thumbnails (a snapshot of the root, reduced) come with the board:
 * LVGL renders a hidden root into a snapshot only if it is laid out, and on
 * the P4 the PPA can shrink it for free, so it is measured there first.
 */
#include "aos_internal.h"
#include "aos_hal.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"

static lv_obj_t *s_ov;

bool aos_switcher_is_open(void) { return s_ov != NULL; }

bool aos_switcher_close(void)
{
    if (!s_ov) return false;
    lv_obj_delete(s_ov);
    s_ov = NULL;
    return true;
}

static void card_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    aos_app_t *app = lv_event_get_user_data(e);
    if (c == LV_EVENT_SHORT_CLICKED) {
        aos_switcher_close();
        aos_ui_open(app->desc.id);
    } else if (c == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) {
        lv_indev_wait_release(lv_indev_active());
        aos_ui_close(app->desc.id);
        aos_switcher_close();
        aos_switcher_open();        /* the row without it */
    }
}

static void bg_event(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_ov) { aos_switcher_close(); aos_ui_home(); }
}

void aos_switcher_open(void)
{
    const aos_geo_t *g = aos_ui_geo();
    aos_switcher_close();
    s_ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ov);
    lv_obj_set_size(s_ov, g->w, g->h);
    lv_obj_set_style_bg_color(s_ov, lv_color_hex(0x07090C), 0);
    lv_obj_set_style_bg_opa(s_ov, 230, 0);
    lv_obj_set_style_text_color(s_ov, lv_color_white(), 0);
    lv_obj_add_flag(s_ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ov, bg_event, LV_EVENT_CLICKED, NULL);

    aos_app_t *alive[AOS_UI_ALIVE_MAX + 2];
    int n = aos_ui_alive(alive, AOS_UI_ALIVE_MAX + 2);
    if (n == 0) {
        lv_obj_t *l = lv_label_create(s_ov);
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x8A93A3), 0);
        lv_label_set_text(l, _("No hay apps abiertas"));
        lv_obj_center(l);
        return;
    }

    lv_obj_t *row = lv_obj_create(s_ov);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, g->w, g->h * 3 / 4);
    lv_obj_center(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 36, 0);
    lv_obj_set_style_pad_hor(row, g->w / 6, 0);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(row, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    int32_t cw = g->w * 2 / 3, ch = g->h * 3 / 5;
    if (g->landscape) { cw = g->w / 3; ch = g->h * 3 / 5; }
    for (int i = 0; i < n; i++) {
        aos_app_t *app = alive[i];
        lv_obj_t *col = lv_obj_create(row);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, cw, ch + 90);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(col, card_event, LV_EVENT_ALL, app);

        lv_obj_t *icon = aos_home_app_icon(col, app, 64);
        lv_obj_set_pos(icon, 0, 0);
        lv_obj_t *name = lv_label_create(col);
        lv_obj_set_style_text_font(name, aos_font_small, 0);
        lv_label_set_text(name, aos_tr(app->desc.name));
        lv_obj_set_pos(name, 80, 16);

        lv_obj_t *card = lv_obj_create(col);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, cw, ch);
        lv_obj_set_pos(card, 0, 90);
        lv_obj_set_style_radius(card, 40, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_color_t c = lv_color_hex(app->desc.color_a ? app->desc.color_a : 0x2C2C2E);
        lv_obj_set_style_bg_color(card, lv_color_darken(c, LV_OPA_50), 0);
        lv_obj_set_style_bg_grad_color(card, lv_color_darken(c, LV_OPA_80), 0);
        lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_border_color(card, lv_color_white(), 0);
        lv_obj_set_style_border_opa(card, LV_OPA_20, 0);
        lv_obj_t *big = aos_home_app_icon(card, app, 160);
        lv_obj_center(big);
        aos_make_decorative(card);
        aos_make_decorative(icon);
    }
}
