/*
 * P4OS - Settings, Developer: what is drawn over everything for whoever is
 * working on the board.
 *
 *   Touches   a ring under every finger the panel reports (two at most),
 *             from aos_hal_touch_frame: what the GT911 says, not what
 *             LVGL's single pointer makes of it.
 *   Fps       frames that reached the panel in the last second: LVGL's
 *             renders plus the apps' own flips. An app that blits bands
 *             straight to the panel (Video) is not counted.
 *   Log       the level of the log (esp_log_level_set): this build has up to
 *             info compiled in, so errors, warnings or everything.
 *
 * All three are preferences, applied again at boot. Both overlays live on
 * lv_layer_sys and take no touches.
 */
#include "aos_internal.h"
#include "aos_hal.h"
#include "aos_theme.h"

#include <stdio.h>

#define DOT 76

static lv_obj_t *s_dot[2], *s_fps;
static lv_timer_t *s_touch_t, *s_fps_t;
static uint32_t s_renders, s_last_renders, s_last_flips, s_last_ms;
static bool s_counting;

static void touch_tick(lv_timer_t *t)
{
    aos_touch_frame_t f;
    int n = aos_hal_touch_frame(&f) ? f.count : 0;
    for (int i = 0; i < 2; i++) {
        if (i < n) {
            lv_obj_set_pos(s_dot[i], f.x[i] - DOT / 2, f.y[i] - DOT / 2);
            lv_obj_remove_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void aos_dev_set_touches(bool on)
{
    aos_hal_pref_set_i32("dev_touch", on);
    if (on && !s_touch_t) {
        for (int i = 0; i < 2; i++) {
            lv_obj_t *d = lv_obj_create(lv_layer_sys());
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, DOT, DOT);
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(d, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_30, 0);
            lv_obj_set_style_border_color(d, i ? lv_color_hex(0xFF9F0A) : lv_color_hex(0x0A84FF), 0);
            lv_obj_set_style_border_width(d, 4, 0);
            lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_FLOATING);
            s_dot[i] = d;
        }
        s_touch_t = lv_timer_create(touch_tick, 20, NULL);
    } else if (!on && s_touch_t) {
        lv_timer_delete(s_touch_t);
        s_touch_t = NULL;
        for (int i = 0; i < 2; i++) { lv_obj_delete(s_dot[i]); s_dot[i] = NULL; }
    }
}

bool aos_dev_touches(void) { return s_touch_t != NULL; }

static void render_cb(lv_event_t *e) { s_renders++; }

static void fps_tick(lv_timer_t *t)
{
    uint32_t now = lv_tick_get(), flips = aos_hal_display_flips();
    uint32_t ms = now - s_last_ms;
    if (!ms) return;
    uint32_t frames = (s_renders - s_last_renders) + (flips - s_last_flips);
    s_last_renders = s_renders;
    s_last_flips = flips;
    s_last_ms = now;
    /* the label's own redraw is one of next second's frames */
    lv_label_set_text_fmt(s_fps, "%u fps", (unsigned)((frames * 1000 + ms / 2) / ms));
    const aos_geo_t *g = aos_ui_geo();
    lv_obj_align(s_fps, LV_ALIGN_TOP_RIGHT, -16, g->bar_h + 6);
}

void aos_dev_set_fps(bool on)
{
    aos_hal_pref_set_i32("dev_fps", on);
    if (on && !s_fps_t) {
        if (!s_counting) {
            lv_display_add_event_cb(lv_display_get_default(), render_cb, LV_EVENT_RENDER_READY, NULL);
            s_counting = true;
        }
        s_fps = lv_label_create(lv_layer_sys());
        lv_obj_set_style_text_font(s_fps, aos_font_caption, 0);
        lv_obj_set_style_text_color(s_fps, lv_color_hex(0x30D158), 0);
        lv_obj_set_style_bg_color(s_fps, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_fps, LV_OPA_70, 0);
        lv_obj_set_style_pad_hor(s_fps, 14, 0);
        lv_obj_set_style_pad_ver(s_fps, 4, 0);
        lv_obj_set_style_radius(s_fps, 14, 0);
        lv_obj_remove_flag(s_fps, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(s_fps, "-- fps");
        s_last_ms = lv_tick_get();
        s_last_renders = s_renders;
        s_last_flips = aos_hal_display_flips();
        s_fps_t = lv_timer_create(fps_tick, 1000, NULL);
        fps_tick(s_fps_t);
    } else if (!on && s_fps_t) {
        lv_timer_delete(s_fps_t);
        s_fps_t = NULL;
        lv_obj_delete(s_fps);
        s_fps = NULL;
    }
}

bool aos_dev_fps(void) { return s_fps_t != NULL; }

/* 1 errors, 2 and warnings, 3 everything this build has (info) */
void aos_dev_set_log_level(int level)
{
    if (level < 1 || level > 3) level = 3;
    aos_hal_pref_set_i32("log_level", level);
    aos_hal_log_level_set(level);
}

int aos_dev_log_level(void)
{
    int32_t v = 3;
    aos_hal_pref_get_i32("log_level", &v);
    return v < 1 || v > 3 ? 3 : (int)v;
}

void aos_dev_init(void)
{
    int32_t v = 0;
    if (aos_hal_pref_get_i32("dev_touch", &v) && v) aos_dev_set_touches(true);
    v = 0;
    if (aos_hal_pref_get_i32("dev_fps", &v) && v) aos_dev_set_fps(true);
    if (aos_dev_log_level() != 3) aos_hal_log_level_set(aos_dev_log_level());
}
