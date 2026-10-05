/*
 * P4OS - a USB mouse points on the screen (aos_hal_hid_mouse_read,
 * aos_usb_hid_p4.c in the HAL; docs/USB.md).
 *
 * A second LVGL pointer beside the touch screen, made the first time a mouse
 * appears: an arrow drawn here, on the system layer, that LVGL moves. The
 * left button is a finger on the glass, so a click is a tap and a drag is a
 * swipe (the system's edge gestures too). The wheel scrolls whatever
 * scrolls under the arrow; the right button is "back", the middle one
 * "home". The arrow hides after four seconds still and shows at the first
 * motion. An absolute pointer (a USB touch screen, a tablet) maps its
 * surface onto the screen.
 *
 * LVGL's coordinates are the turned screen's (the HAL rotates in the flush),
 * so the arrow needs no turning of its own.
 *
 * An app in front can take the raw reports (aos_ui_hwmouse_handler) and
 * keep any of the buttons and the wheel for itself; the shell drops the
 * handler when the app closes (aos_hwmouse_app_gone, from destroy_app).
 */
#include "aos_internal.h"
#include "aos_hal.h"

#include "lvgl.h"
#ifndef AOS_SIM
#include "esp_heap_caps.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HIDE_MS 4000

static aos_hwmouse_cb_t s_cb;
static char s_owner[48];

void aos_ui_hwmouse_handler(aos_hwmouse_cb_t cb)
{
    const char *id = aos_ui_current_app();
    s_cb = cb && id ? cb : NULL;
    snprintf(s_owner, sizeof s_owner, "%s", s_cb ? id : "");
}

void aos_hwmouse_app_gone(const char *id)
{
    if (s_cb && id && strcmp(id, s_owner) == 0) s_cb = NULL;
}

static struct {
    lv_indev_t *indev;
    lv_obj_t *arrow;
    lv_image_dsc_t img;
    int32_t x, y;
    uint8_t buttons;
    int wheel;
    uint32_t moved_ms;
    bool shown;
    bool took_left;                 /* the app's handler keeps the left button */
} M;

/* the arrow, 12 x 19, drawn at twice that: X black, . white */
static const char *const ARROW[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X......XXXXX",
    "X...X..X    ", "X..XX..X    ", "X.X  X..X   ", "XX   X..X   ", "X     X..X  ", "      X..X  ",
    "       XX   ",
};

static bool make_arrow(void)
{
    const int w = 12 * 2, h = 19 * 2;
#ifdef AOS_SIM
    uint32_t *px = calloc(w * h, 4);
#else
    uint32_t *px = heap_caps_calloc(w * h, 4, MALLOC_CAP_SPIRAM);   /* 3.6 KB, PSRAM first */
#endif
    if (!px) return false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            char c = ARROW[y / 2][x / 2];
            px[y * w + x] = c == 'X' ? 0xFF000000 : c == '.' ? 0xFFFFFFFF : 0;
        }
    M.img.header.magic = LV_IMAGE_HEADER_MAGIC;
    M.img.header.cf = LV_COLOR_FORMAT_ARGB8888;
    M.img.header.w = w;
    M.img.header.h = h;
    M.img.header.stride = w * 4;
    M.img.data_size = w * h * 4;
    M.img.data = (const uint8_t *)px;
    return true;
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    aos_mouse_event_t ev;
    int32_t W = lv_display_get_horizontal_resolution(NULL), H = lv_display_get_vertical_resolution(NULL);
    if (aos_hal_hid_mouse_read(&ev)) {
        if (ev.absolute) {
            M.x = (int32_t)ev.x * (W - 1) / 65535;
            M.y = (int32_t)ev.y * (H - 1) / 65535;
        } else {
            /* a little acceleration: slow moves are precise, fast ones cross
             * the 720 x 1280 screen in a flick */
            int ax = ev.dx < 0 ? -ev.dx : ev.dx, ay = ev.dy < 0 ? -ev.dy : ev.dy;
            int k = ax + ay > 12 ? 4 : ax + ay > 4 ? 3 : 2;
            M.x += ev.dx * k;
            M.y += ev.dy * k;
        }
        M.x = M.x < 0 ? 0 : M.x >= W ? W - 1 : M.x;
        M.y = M.y < 0 ? 0 : M.y >= H ? H - 1 : M.y;
        uint8_t was = M.buttons;
        M.buttons = ev.buttons;
        uint8_t took = 0;
        if (s_cb) {
            const char *cur = aos_ui_current_app();
            if (cur && strcmp(cur, s_owner) == 0) {
                aos_hwmouse_event_t e = {
                    .x = M.x, .y = M.y, .dx = ev.absolute ? 0 : ev.dx, .dy = ev.absolute ? 0 : ev.dy,
                    .wheel = ev.wheel, .buttons = ev.buttons,
                    .pressed = (uint8_t)(ev.buttons & ~was), .released = (uint8_t)(was & ~ev.buttons),
                };
                took = s_cb(&e);
            }
        }
        M.took_left = took & AOS_HWMOUSE_LEFT;
        if (!(took & AOS_HWMOUSE_RIGHT) && (ev.buttons & 2) && !(was & 2)) aos_ui_request_nav(AOS_UI_NAV_BACK);
        if (!(took & AOS_HWMOUSE_MIDDLE) && (ev.buttons & 4) && !(was & 4)) aos_ui_request_nav(AOS_UI_NAV_HOME);
        if (!(took & AOS_HWMOUSE_WHEEL)) M.wheel += ev.wheel;
        M.moved_ms = lv_tick_get();
        aos_hal_activity();
    }
    data->point.x = M.x;
    data->point.y = M.y;
    data->state = (M.buttons & 1) && !M.took_left ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* the object under the arrow that can still scroll that way */
static lv_obj_t *scroller_at(lv_point_t *p, int dy)
{
    lv_obj_t *o = lv_indev_search_obj(lv_layer_top(), p);
    if (!o) o = lv_indev_search_obj(lv_screen_active(), p);
    for (; o; o = lv_obj_get_parent(o)) {
        if (!lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE)) continue;
        if (dy > 0 ? lv_obj_get_scroll_top(o) > 0 : lv_obj_get_scroll_bottom(o) > 0) return o;
    }
    return NULL;
}

void aos_hwmouse_tick(void)
{
    if (!M.indev) {
        if (!aos_hal_hid_mouse_present() || !make_arrow()) return;
        M.arrow = lv_image_create(lv_layer_sys());
        lv_image_set_src(M.arrow, &M.img);
        lv_obj_remove_flag(M.arrow, LV_OBJ_FLAG_CLICKABLE);
        M.indev = lv_indev_create();
        lv_indev_set_type(M.indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(M.indev, read_cb);
        lv_indev_set_cursor(M.indev, M.arrow);
        M.x = lv_display_get_horizontal_resolution(NULL) / 2;
        M.y = lv_display_get_vertical_resolution(NULL) / 2;
        M.moved_ms = lv_tick_get();
    }
    bool present = aos_hal_hid_mouse_present();
    bool show = present && lv_tick_elaps(M.moved_ms) < HIDE_MS;
    if (show != M.shown) {
        if (show) lv_obj_remove_flag(M.arrow, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(M.arrow, LV_OBJ_FLAG_HIDDEN);
        M.shown = show;
    }
    if (M.wheel) {
        lv_point_t p = { M.x, M.y };
        int dy = M.wheel * 90;          /* a notch, about two rows of a list */
        M.wheel = 0;
        lv_obj_t *o = scroller_at(&p, dy);
        if (o) lv_obj_scroll_by_bounded(o, 0, dy, LV_ANIM_ON);
    }
}
