/*
 * P4OS - Status bar.
 *
 * Floats on lv_layer_top() over the home screen and the apps: the time on
 * the left, the state of the links on the right. It is transparent, so an
 * app with AOS_APP_FLAG_UNDER_BAR draws under it and picks the glyph colour
 * with aos_ui_statusbar_style().
 *
 * The right-hand glyphs, left to right: the modules on the header (when
 * any is active), Home Assistant, the SD card, Wi-Fi with its bars, and the
 * battery - only when a battery is connected (AOS_CAP_BATTERY); on the bench
 * the board lives on USB and a permanent "100 %" would be noise.
 */
#include "aos_internal.h"
#include "aos_hal.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *s_bar, *s_time, *s_title, *s_right;
static lv_obj_t *s_wifi, *s_sd, *s_ha, *s_mod, *s_bat, *s_bat_pct;

static lv_obj_t *glyph(lv_obj_t *parent)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &aos_sym_28, 0);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *aos_statusbar_obj(void) { return s_bar; }

void aos_statusbar_create(lv_obj_t *layer)
{
    s_bar = lv_obj_create(layer);
    lv_obj_remove_style_all(s_bar);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_bar, lv_color_white(), 0);

    s_time = lv_label_create(s_bar);
    lv_obj_set_style_text_font(s_time, aos_font_small, 0);
    lv_label_set_text(s_time, "--:--");

    s_title = lv_label_create(s_bar);
    lv_obj_set_style_text_font(s_title, aos_font_caption, 0);
    lv_obj_set_style_text_opa(s_title, LV_OPA_70, 0);
    lv_label_set_text(s_title, "");

    s_right = lv_obj_create(s_bar);
    lv_obj_remove_style_all(s_right);
    lv_obj_set_size(s_right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_right, 10, 0);
    lv_obj_remove_flag(s_right, LV_OBJ_FLAG_CLICKABLE);

    s_mod = glyph(s_right);
    s_ha = glyph(s_right);
    s_sd = glyph(s_right);
    s_wifi = glyph(s_right);
    s_bat_pct = lv_label_create(s_right);
    lv_obj_set_style_text_font(s_bat_pct, aos_font_caption, 0);
    lv_label_set_text(s_bat_pct, "");
    s_bat = glyph(s_right);

    aos_statusbar_layout();
}

void aos_statusbar_layout(void)
{
    const aos_geo_t *g = aos_ui_geo();
    lv_obj_set_size(s_bar, g->w, g->bar_h);
    lv_obj_set_pos(s_bar, 0, 0);
    int32_t side = g->landscape ? 32 : 36;
    lv_obj_align(s_time, LV_ALIGN_LEFT_MID, side, 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(s_right, LV_ALIGN_RIGHT_MID, -side + 8, 0);
}

void aos_statusbar_set_title(const char *title)
{
    lv_label_set_text(s_title, title ? title : "");
}

static void set_glyph(lv_obj_t *l, const char *g, lv_opa_t opa)
{
    if (!g) {
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
    if (strcmp(lv_label_get_text(l), g)) lv_label_set_text(l, g);
    lv_obj_set_style_text_opa(l, opa, 0);
}

static const char *wifi_glyph(void)
{
    aos_net_state_t st = aos_hal_net_state();
    /* the board's own access point, when it is not on a network as well */
    if (st != AOS_NET_CONNECTED && aos_hal_net_ap_active()) return AOS_SYM_ACCESS_POINT;
    if (st == AOS_NET_OFF) return NULL;
    if (st != AOS_NET_CONNECTED) return AOS_SYM_WIFI_STRENGTH_OUTLINE;
    int rssi = aos_hal_net_rssi();
    if (rssi >= -55) return AOS_SYM_WIFI_STRENGTH_4;
    if (rssi >= -65) return AOS_SYM_WIFI_STRENGTH_3;
    if (rssi >= -75) return AOS_SYM_WIFI_STRENGTH_2;
    return AOS_SYM_WIFI_STRENGTH_1;
}

void aos_statusbar_tick(void)
{
    if (!s_bar) return;
    struct tm t;
    aos_hal_time_now(&t);
    char buf[8];
    if (aos_hal_time_is_valid()) snprintf(buf, sizeof buf, "%d:%02d", t.tm_hour, t.tm_min);
    else snprintf(buf, sizeof buf, "--:--");
    if (strcmp(lv_label_get_text(s_time), buf)) lv_label_set_text(s_time, buf);

    const char *w = wifi_glyph();
    set_glyph(s_wifi, w, aos_hal_net_state() == AOS_NET_CONNECTED || aos_hal_net_ap_active() ? LV_OPA_COVER : LV_OPA_50);
    set_glyph(s_sd, aos_hal_sd_present() ? AOS_SYM_SD : NULL, LV_OPA_80);

    /* Home Assistant and the modules have no HAL yet (phases 5 and 6): the
     * glyphs are reserved and stay hidden. */
    set_glyph(s_ha, NULL, LV_OPA_COVER);
    set_glyph(s_mod, NULL, LV_OPA_COVER);

    if (aos_hal_has(AOS_CAP_BATTERY)) {
        aos_battery_t b;
        if (aos_hal_battery_read(&b) && b.percent >= 0) {
            const char *g = b.charging ? AOS_SYM_BATTERY_CHARGING :
                            b.percent > 90 ? AOS_SYM_BATTERY :
                            b.percent > 65 ? AOS_SYM_BATTERY_80 :
                            b.percent > 35 ? AOS_SYM_BATTERY_50 :
                            b.percent > 10 ? AOS_SYM_BATTERY_20 : AOS_SYM_BATTERY_OUTLINE;
            set_glyph(s_bat, g, LV_OPA_COVER);
            lv_obj_remove_flag(s_bat_pct, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_fmt(s_bat_pct, "%d%%", b.percent);
            return;
        }
    }
    set_glyph(s_bat, NULL, LV_OPA_COVER);
    lv_obj_add_flag(s_bat_pct, LV_OBJ_FLAG_HIDDEN);
}
