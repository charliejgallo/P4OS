/*
 * P4OS - a USB keyboard types where the on-screen keyboard would
 * (aos_hal_usb_kbd_read, aos_usb_kbd_p4.c in the HAL; docs/USB.md).
 *
 * The target is the text area of the lv_keyboard that is open: shown, on
 * the top layer or the active screen, the last one created first. That
 * covers every text field of the system and of the apps that use LVGL's
 * keyboard, with no app knowing about it. Enter and Esc do what the
 * keyboard's OK and close keys do (LV_EVENT_READY / LV_EVENT_CANCEL to the
 * keyboard, then to its text area); in a text area of several lines Enter
 * is a new line. With no keyboard open, the keys go nowhere.
 *
 * An app with a keyboard of its own (Notas) takes them first, while it is in
 * front, through aos_ui_hwkbd_handler(). The handler is code of a dynamic
 * app: the shell drops it when that app closes (aos_hwkbd_app_gone, from
 * destroy_app), before the app's code can go away.
 */
#include "aos_internal.h"
#include "aos_hal.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static aos_hwkbd_cb_t s_cb;
static char s_owner[48];        /* the app that set it */

void aos_ui_hwkbd_handler(aos_hwkbd_cb_t cb)
{
    const char *id = aos_ui_current_app();
    s_cb = cb && id ? cb : NULL;
    snprintf(s_owner, sizeof s_owner, "%s", s_cb ? id : "");
}

void aos_hwkbd_app_gone(const char *id)
{
    if (s_cb && id && strcmp(id, s_owner) == 0) s_cb = NULL;
}

static lv_obj_t *find_kb(lv_obj_t *o)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(o, &lv_keyboard_class)) return lv_keyboard_get_textarea(o) ? o : NULL;
    for (int i = (int)lv_obj_get_child_count(o) - 1; i >= 0; i--) {
        lv_obj_t *k = find_kb(lv_obj_get_child(o, i));
        if (k) return k;
    }
    return NULL;
}

static lv_obj_t *open_kb(void)
{
    lv_obj_t *k = find_kb(lv_layer_top());
    return k ? k : find_kb(lv_screen_active());
}

static int utf8(uint32_t c, char *out)
{
    if (c < 0x80) { out[0] = (char)c; out[1] = 0; return 1; }
    if (c < 0x800) { out[0] = 0xC0 | c >> 6; out[1] = 0x80 | (c & 0x3F); out[2] = 0; return 2; }
    out[0] = 0xE0 | c >> 12;
    out[1] = 0x80 | (c >> 6 & 0x3F);
    out[2] = 0x80 | (c & 0x3F);
    out[3] = 0;
    return 3;
}

void aos_hwkbd_tick(void)
{
    aos_kbd_event_t ev;
    lv_obj_t *kb = NULL;
    bool looked = false;
    for (int n = 0; n < 32 && aos_hal_usb_kbd_read(&ev); n++) {
        if (s_cb) {
            const char *cur = aos_ui_current_app();
            if (cur && strcmp(cur, s_owner) == 0 && s_cb(ev.key, ev.mods)) continue;
        }
        if (!looked) {
            kb = open_kb();
            looked = true;
        }
        if (!kb) continue;
        lv_obj_t *ta = lv_keyboard_get_textarea(kb);
        if (!ta || (ev.mods & 0x99)) continue;      /* Ctrl or Cmd: a shortcut, not text */
        switch (ev.key) {
        case AOS_KEY_BACKSPACE: lv_textarea_delete_char(ta); break;
        case AOS_KEY_DEL:       lv_textarea_delete_char_forward(ta); break;
        case AOS_KEY_LEFT:      lv_textarea_cursor_left(ta); break;
        case AOS_KEY_RIGHT:     lv_textarea_cursor_right(ta); break;
        case AOS_KEY_UP:        lv_textarea_cursor_up(ta); break;
        case AOS_KEY_DOWN:      lv_textarea_cursor_down(ta); break;
        case AOS_KEY_HOME:      lv_textarea_set_cursor_pos(ta, 0); break;
        case AOS_KEY_END:       lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST); break;
        case AOS_KEY_NEXT:
        case AOS_KEY_PREV:      break;
        case AOS_KEY_ENTER:
            if (!lv_textarea_get_one_line(ta)) {
                lv_textarea_add_char(ta, '\n');
                break;
            }
            /* fall through: as the OK key */
        case AOS_KEY_ESC: {
            lv_event_code_t code = ev.key == AOS_KEY_ESC ? LV_EVENT_CANCEL : LV_EVENT_READY;
            if (lv_obj_send_event(kb, code, NULL) == LV_RESULT_OK && lv_obj_is_valid(ta)) lv_obj_send_event(ta, code, NULL);
            looked = false;         /* the keyboard may be gone now */
            break;
        }
        default:
            if (ev.key >= 32 && ev.key != AOS_KEY_DEL) {
                char s[5];
                utf8(ev.key, s);
                lv_textarea_add_text(ta, s);
            }
            break;
        }
    }
}
