/*
 * P4OS - Palette, typefaces and UI helpers (from AmoledOS).
 *
 * Sizes are real pixels. The 5" panel is ~294 ppi and the watch was ~322, so
 * a pixel is almost the same size on both: what changes is how much fits.
 * The type roles follow iOS's physical sizes (body ~ 2.5 mm). The typeface
 * is Inter: Medium for everything, SemiBold for the names under the home
 * icons, which sit on the wallpaper and need the weight.
 */
#pragma once

#include "lvgl.h"
#include "aos_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Palette (taken from iOS's colour system in dark mode) */
#define AOS_C_BG        lv_color_hex(0x000000)
#define AOS_C_CARD      lv_color_hex(0x1C1C1E)
#define AOS_C_CARD2     lv_color_hex(0x2C2C2E)
#define AOS_C_TEXT      lv_color_hex(0xFFFFFF)
#define AOS_C_DIM       lv_color_hex(0x8E8E93)
#define AOS_C_ACCENT    lv_color_hex(0x0A84FF)
#define AOS_C_GREEN     lv_color_hex(0x30D158)
#define AOS_C_RED       lv_color_hex(0xFF453A)
#define AOS_C_ORANGE    lv_color_hex(0xFF9F0A)
#define AOS_C_YELLOW    lv_color_hex(0xFFD60A)
#define AOS_C_PURPLE    lv_color_hex(0xBF5AF2)
#define AOS_C_PINK      lv_color_hex(0xFF375F)
#define AOS_C_TEAL      lv_color_hex(0x40C8E0)

/* Typefaces resolved at run time from whatever is compiled in */
extern const lv_font_t *aos_font_huge;   /* 64 px, clocks, lock screen    */
extern const lv_font_t *aos_font_large;  /* 48 px, big numbers            */
extern const lv_font_t *aos_font_title;  /* 36 px, screen titles          */
extern const lv_font_t *aos_font_body;   /* 28 px, text, list rows        */
extern const lv_font_t *aos_font_small;  /* 24 px, status bar, secondary  */
extern const lv_font_t *aos_font_caption;/* 20 px, icon labels, captions  */
extern const lv_font_t *aos_font_tiny;   /* 16 px, badges, fine print     */
extern const lv_font_t *aos_font_label;  /* 20 px SemiBold, icon names    */

/* Layout constants of the shell, in px. */
#define AOS_UI_ICON          120    /* home icon, portrait                  */
#define AOS_UI_ICON_LAND     108
#define AOS_UI_ICON_RADIUS   27     /* squircle corner for AOS_UI_ICON      */
#define AOS_UI_TAP_MIN       88     /* smallest comfortable target          */
#define AOS_UI_PAD           24     /* screen side padding                  */
#define AOS_UI_ROW_H         96     /* a list row                           */
#define AOS_UI_RADIUS        28     /* cards and panels                     */

void aos_theme_init(void);

/* Page container: black, no border, no scrolling, fills its parent. */
lv_obj_t *aos_page(lv_obj_t *parent);

/* Quick label. A NULL/0 'font' or 'color' uses the defaults. */
lv_obj_t *aos_label(lv_obj_t *parent, const char *text,
                    const lv_font_t *font, lv_color_t color);

/* Scaled label.
 *
 * LVGL scales from the pivot, and the default pivot is the top-left corner: a
 * centred label scaled afterwards shifts right and down. Here the pivot goes
 * to 50% on both axes, so the text grows evenly both ways and the alignment
 * you set still holds.
 *
 * 'scale' is in LVGL units: 256 = original size. */
lv_obj_t *aos_label_scaled(lv_obj_t *parent, const char *text,
                           const lv_font_t *font, lv_color_t color,
                           int32_t scale);

/* Fixed-width label with the text centred inside.
 *
 * Needed whenever a text that changes length is positioned with
 * lv_obj_align_to(): that function computes the position once and does NOT
 * recompute it when the object changes size, so a content-sized label drifts
 * as the text grows. With a fixed box, the position always holds and the text
 * centres inside it. */
lv_obj_t *aos_label_boxed(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, lv_color_t color,
                          int32_t width, int32_t height);

/* Rounded watchOS-style button. */
lv_obj_t *aos_button(lv_obj_t *parent, const char *text, lv_color_t color,
                     lv_event_cb_t cb, void *user_data);

/* Circular icon with a gradient + glyph/vector, at the size asked for in px. */
lv_obj_t *aos_icon_create(lv_obj_t *parent, const aos_app_desc_t *desc, int32_t size);

/* Takes LV_OBJ_FLAG_CLICKABLE off an object and all its children. In LVGL 9
 * every lv_obj is born clickable, so any piece of decoration eats the touch
 * meant for its container. */
void aos_make_decorative(lv_obj_t *obj);

/* The system's keyboard look: dark keys on a darker tray, the action keys
 * (shift, backspace, OK) a shade lighter, like the phone's. For an
 * lv_keyboard the caller created; sets the font too. */
void aos_keyboard_style(lv_obj_t *kb, const lv_font_t *font);

/* Short Spanish names for dates. */
const char *aos_day_name(int wday);     /* 0 = Sunday -> "DOM" */
const char *aos_month_name(int mon);    /* 0 = January -> "ENE" */

/* Clock hand: a rectangle with its pivot at the bottom end, rotating about the
 * parent's centre. The angle is in tenths of a degree, with 0 = twelve
 * o'clock. Used by the icons and the analogue face. */
lv_obj_t *aos_hand_create(lv_obj_t *parent, int32_t width, int32_t length,
                          lv_color_t color);
void      aos_hand_set_angle(lv_obj_t *hand, int32_t deg_tenths);

#ifdef __cplusplus
}
#endif
