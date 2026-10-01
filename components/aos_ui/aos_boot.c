/*
 * P4OS - the boot screen (aos_ui.h, "Boot screen").
 *
 * The white name on black with a thin bar under it, over everything (LVGL's
 * top layer), while the card's apps are read: without it the home screen
 * showed its icons arriving one by one for seconds. The bar is the scan's
 * real progress, not a clock; aos_ui_boot_done() fades it out onto the home
 * screen, and a boot that never says it is done still ends after
 * BOOT_MAX_MS.
 */
#include "aos_ui.h"
#include "aos_internal.h"
#include "aos_theme.h"

#define BOOT_MAX_MS     30000
#define FADE_MS         350
#define BAR_W           280
#define BAR_H           4

static lv_obj_t *s_boot, *s_bar;
static lv_timer_t *s_guard;

static void boot_delete(lv_anim_t *a)
{
    (void)a;
    if (s_boot) lv_obj_delete(s_boot);
    s_boot = s_bar = NULL;
}

static void fade_cb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }

static void guard_cb(lv_timer_t *t)
{
    (void)t;
    aos_ui_boot_done();
}

void aos_ui_boot_show(void)
{
    if (s_boot) return;
    s_boot = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_boot);
    lv_obj_set_size(s_boot, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_boot, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_boot, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_boot, LV_OBJ_FLAG_CLICKABLE);         /* nothing under it takes a tap */
    lv_obj_remove_flag(s_boot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(s_boot);
    lv_obj_set_style_text_font(name, aos_font_huge, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(name, 6, 0);
    lv_label_set_text(name, "P4OS");
    lv_obj_align(name, LV_ALIGN_CENTER, 0, -30);

    s_bar = lv_bar_create(s_boot);
    lv_obj_set_size(s_bar, BAR_W, BAR_H);
    lv_obj_set_style_radius(s_bar, BAR_H / 2, 0);
    lv_obj_set_style_radius(s_bar, BAR_H / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(s_bar, 250, 0);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_bar_set_value(s_bar, 30, LV_ANIM_OFF);               /* the firmware is up: a sliver */
    lv_obj_align_to(s_bar, name, LV_ALIGN_OUT_BOTTOM_MID, 0, 56);

    s_guard = lv_timer_create(guard_cb, BOOT_MAX_MS, NULL);
    lv_timer_set_repeat_count(s_guard, 1);
}

void aos_ui_boot_progress(int done, int total)
{
    if (!s_bar || total <= 0) return;
    if (done > total) done = total;
    /* 3% for the firmware, the rest for the card */
    lv_bar_set_value(s_bar, 30 + (int32_t)done * 970 / total, LV_ANIM_ON);
}

void aos_ui_boot_done(void)
{
    if (s_guard) {
        lv_timer_delete(s_guard);
        s_guard = NULL;
    }
    if (!s_boot) return;
    lv_bar_set_value(s_bar, 1000, LV_ANIM_OFF);
    lv_obj_remove_flag(s_boot, LV_OBJ_FLAG_CLICKABLE);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_boot);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, FADE_MS);
    lv_anim_set_delay(&a, 120);
    lv_anim_set_exec_cb(&a, fade_cb);
    lv_anim_set_completed_cb(&a, boot_delete);
    lv_anim_start(&a);
}
