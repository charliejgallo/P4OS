/*
 * P4OS - the boot screen (aos_ui.h, "Boot screen").
 *
 * The mark (a chip: a squircle with its pins), the name and a thin bar,
 * white on black over everything (LVGL's top layer), while the card's apps
 * are read: without it the home screen showed its icons arriving one by one
 * for seconds. The mark pops in, its pins light up one after another and
 * the name closes in, all in under a second and with plain objects (no
 * transforms, so no layer buffers). The bar is the scan's real progress,
 * not a clock; aos_ui_boot_done() fades it all out onto the home screen,
 * and a boot that never says it is done still ends after BOOT_MAX_MS.
 */
#include "aos_ui.h"
#include "aos_internal.h"
#include "aos_theme.h"

#define BOOT_MAX_MS     30000
#define FADE_MS         350
#define BAR_W           280
#define BAR_H           4
#define MARK            150     /* the chip's side */
#define MARK_Y          (-120)  /* its centre, from the screen's */
#define PIN_W           8
#define PIN_L           22
#define PIN_GAP         6       /* between the chip and its pins */

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

static void size_cb(void *obj, int32_t v)
{
    lv_obj_set_size((lv_obj_t *)obj, v, v);
    lv_obj_set_style_radius((lv_obj_t *)obj, v * 225 / 1000, 0);
}

/* the chip fades by its own fill, not its opacity: opacity on an object
 * with children draws it through a layer buffer */
static void bg_fade_cb(void *obj, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }

static void spacing_cb(void *obj, int32_t v) { lv_obj_set_style_text_letter_space((lv_obj_t *)obj, v, 0); }

static void anim(lv_obj_t *o, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms, uint32_t delay,
                 lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_path_cb(&a, path);
    cb(o, from);
    lv_anim_start(&a);
}

/* The chip: a squircle in the system's blue to indigo with "P4" on it, and
 * three pins a side that light up around it, clockwise from the top left. */
static lv_obj_t *mark_create(lv_obj_t *parent)
{
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_remove_style_all(m);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_bg_grad_color(m, lv_color_hex(0x5E5CE6), 0);
    lv_obj_set_style_bg_grad_dir(m, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(m, lv_color_white(), 0);
    lv_obj_set_style_border_opa(m, LV_OPA_20, 0);
    lv_obj_set_style_border_width(m, 2, 0);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(m, LV_ALIGN_CENTER, 0, MARK_Y);
    anim(m, size_cb, MARK * 55 / 100, MARK, 520, 0, lv_anim_path_overshoot);
    anim(m, bg_fade_cb, LV_OPA_TRANSP, LV_OPA_COVER, 260, 0, lv_anim_path_ease_out);

    lv_obj_t *t = lv_label_create(m);
    lv_obj_set_style_text_font(t, aos_font_large, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_text(t, "P4");
    lv_obj_center(t);
    anim(t, fade_cb, LV_OPA_TRANSP, LV_OPA_COVER, 300, 220, lv_anim_path_ease_out);

    /* the pins, by the chip's final size: top (left to right), right (top
     * to bottom), bottom (right to left), left (bottom to top) */
    const int32_t half = MARK / 2 + PIN_GAP + PIN_L / 2, step = MARK / 4;
    for (int i = 0; i < 12; i++) {
        int side = i / 3, k = i % 3 - 1;            /* k: -1, 0, 1 along the side */
        int32_t x, y, w, h;
        switch (side) {
        case 0:  x = k * step;  y = -half; w = PIN_W; h = PIN_L; break;
        case 1:  x = half;      y = k * step; w = PIN_L; h = PIN_W; break;
        case 2:  x = -k * step; y = half;  w = PIN_W; h = PIN_L; break;
        default: x = -half;     y = -k * step; w = PIN_L; h = PIN_W; break;
        }
        lv_obj_t *p = lv_obj_create(parent);
        lv_obj_remove_style_all(p);
        lv_obj_set_size(p, w, h);
        lv_obj_set_style_radius(p, PIN_W / 2, 0);
        lv_obj_set_style_bg_color(p, lv_color_hex(i % 2 ? 0x5E5CE6 : 0x64D2FF), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_align(p, LV_ALIGN_CENTER, x, MARK_Y + y);
        anim(p, fade_cb, LV_OPA_TRANSP, LV_OPA_COVER, 160, 360 + (uint32_t)i * 35, lv_anim_path_ease_out);
    }
    return m;
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

    mark_create(s_boot);

    lv_obj_t *name = lv_label_create(s_boot);
    lv_obj_set_style_text_font(name, aos_font_huge, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_label_set_text(name, "P4OS");
    lv_obj_align(name, LV_ALIGN_CENTER, 0, MARK_Y + MARK / 2 + PIN_GAP + PIN_L + 90);
    anim(name, spacing_cb, 28, 6, 600, 250, lv_anim_path_ease_out);
    anim(name, fade_cb, LV_OPA_TRANSP, LV_OPA_COVER, 450, 250, lv_anim_path_ease_out);

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
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, MARK_Y + MARK / 2 + PIN_GAP + PIN_L + 90 + 80);

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
