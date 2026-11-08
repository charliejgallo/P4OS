/*
 * P4OS - how a text field shows that it is the one being edited.
 *
 * LVGL's default theme draws a text area's cursor in the theme's text
 * colour, which in its light mode is an almost black grey, two pixels wide.
 * Every field of the system and of the apps sits on AOS_C_CARD (#1C1C1E),
 * so the cursor was there, blinking, and could not be seen. This is a theme
 * of our own on top of LVGL's (its parent): to every lv_textarea it adds a
 * cursor in the accent colour, a ring in the accent colour inside the field
 * while it is focused, and a placeholder in AOS_C_DIM so that an example
 * value does not read as a typed one. Being a theme, it reaches the text
 * areas of the apps on the card too, with no app knowing about it. An app
 * that styles these parts itself in the focused state still wins.
 *
 * The ring is an outline with a negative pad, drawn just inside the field's
 * edge: a border would move the text by its width, and an outline outside
 * would be cut by a parent with no padding.
 *
 * Focus alone is not enough. A press on the form behind the fields (to
 * scroll it, say) takes LVGL's focus off the text area while the keyboard
 * still types into it, and the field went dark. So the field of a keyboard
 * that is shown keeps the focused state, and when that keyboard is hidden
 * or deleted its field loses it (unless the field is in a group, where the
 * focus belongs to the gamepad or the keys). That is the same rule as the
 * USB keyboard's (aos_hwkbd.c): the field being edited is the one of the
 * open keyboard. The keyboards are found as they are created (the theme
 * sees every object), and a timer looks at them while there is any.
 *
 * A password field being edited gets an eye at its right end (2026-11-08):
 * a tap shows what was typed, another hides it, and closing the keyboard
 * hides it again. It is a floating child of the field, in padding added
 * for it, and takes no focus. Whether a field is a password one is asked
 * of the eye once it has one (shown, the field is not in password mode):
 * aos_textfield_is_password(), so the clipboard still does not copy it.
 */
#include "aos_internal.h"
#include "aos_theme.h"

#include "lvgl.h"

#include <stdbool.h>
#include <string.h>

#define RING_W      3
#define CURSOR_W    3
#define BLINK_MS    500
#define MAX_KB      8
#define TICK_MS     100

static lv_theme_t *s_theme;
static lv_style_t s_cursor, s_ring, s_placeholder;

typedef struct {
    lv_obj_t *kb;       /* NULL once deleted (the slot waits for the next tick) */
    lv_obj_t *raw;      /* its text area as last read, valid or not */
    lv_obj_t *ta;       /* the same, once checked to be a living text area */
    bool shown;         /* the keyboard was on the screen at the last tick */
    bool held;          /* we put the focused state on ta */
    bool used;
} kb_slot_t;

static kb_slot_t s_kb[MAX_KB];
static lv_timer_t *s_timer;

static bool on_screen(lv_obj_t *o)
{
    lv_obj_t *scr = lv_obj_get_screen(o);
    if (scr != lv_screen_active() && scr != lv_layer_top() && scr != lv_layer_sys()) return false;
    for (; o; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}

/* ---- the eye of a password field ---- */

#define EYE_W 64

static void eye_cb(lv_event_t *e);

static lv_obj_t *eye_of(lv_obj_t *ta)
{
    for (uint32_t i = 0; ta && i < lv_obj_get_child_count(ta); i++) {
        lv_obj_t *c = lv_obj_get_child(ta, (int32_t)i);
        for (uint32_t k = 0; k < lv_obj_get_event_count(c); k++)
            if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(c, k)) == eye_cb) return c;
    }
    return NULL;
}

bool aos_textfield_is_password(lv_obj_t *ta)
{
    return ta && (lv_textarea_get_password_mode(ta) || eye_of(ta));
}

static void eye_show(lv_obj_t *ta, lv_obj_t *eye, bool shown)
{
    lv_textarea_set_password_mode(ta, !shown);
    lv_label_set_text(lv_obj_get_child(eye, 0), shown ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static void eye_cb(lv_event_t *e)
{
    lv_obj_t *eye = lv_event_get_current_target(e);
    lv_obj_t *ta = lv_obj_get_parent(eye);
    eye_show(ta, eye, lv_textarea_get_password_mode(ta));
}

static void eye_add(lv_obj_t *ta)
{
    if (!ta || !lv_textarea_get_password_mode(ta) || eye_of(ta)) return;
    lv_obj_set_style_pad_right(ta, lv_obj_get_style_pad_right(ta, LV_PART_MAIN) + EYE_W, 0);
    lv_obj_t *b = lv_obj_create(ta);
    lv_obj_remove_style_all(b);
    lv_obj_add_flag(b, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(b, EYE_W, lv_pct(100));
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, EYE_W, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_color(l, AOS_C_DIM, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, eye_cb, LV_EVENT_CLICKED, NULL);
}

/* the keyboard closed: a password shown is hidden again */
static void eye_hide(lv_obj_t *ta)
{
    lv_obj_t *eye = eye_of(ta);
    if (eye && !lv_textarea_get_password_mode(ta)) eye_show(ta, eye, false);
}

/* the keyboard stopped editing ta: it is not the field being edited any more */
static void let_go(lv_obj_t *ta)
{
    if (!ta) return;
    eye_hide(ta);
    if (!lv_obj_get_group(ta)) lv_obj_remove_state(ta, LV_STATE_FOCUSED);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    bool any = false;
    for (int i = 0; i < MAX_KB; i++) {
        kb_slot_t *s = &s_kb[i];
        if (!s->used) continue;
        if (!s->kb) {
            if (s->shown || s->held) let_go(s->ta);
            memset(s, 0, sizeof *s);
            continue;
        }
        any = true;
        lv_obj_t *cur = lv_keyboard_get_textarea(s->kb);
        if (cur != s->raw) {
            if (s->held || s->shown) let_go(s->ta);
            s->raw = cur;
            s->ta = cur && lv_obj_is_valid(cur) && lv_obj_check_type(cur, &lv_textarea_class) ? cur : NULL;
            s->held = s->shown = false;
        }
        if (!s->ta) continue;
        bool shown = on_screen(s->kb);
        if (shown) eye_add(s->ta);
        if (shown && !lv_obj_has_state(s->ta, LV_STATE_FOCUSED)) {
            lv_obj_add_state(s->ta, LV_STATE_FOCUSED);
            s->held = true;
        } else if (!shown && s->shown) {
            let_go(s->ta);
            s->held = false;
        }
        s->shown = shown;
    }
    if (!any) lv_timer_pause(s_timer);
}

static void kb_deleted_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    for (int i = 0; i < MAX_KB; i++)
        if (s_kb[i].used && s_kb[i].kb == kb) s_kb[i].kb = NULL;
}

static void ta_deleted_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    for (int i = 0; i < MAX_KB; i++)
        if (s_kb[i].used && s_kb[i].ta == ta) {
            s_kb[i].ta = NULL;
            s_kb[i].held = s_kb[i].shown = false;
        }
}

static void add_keyboard(lv_obj_t *kb)
{
    /* lv_theme_apply() on a living keyboard comes through here again */
    for (int i = 0; i < MAX_KB; i++)
        if (s_kb[i].used && s_kb[i].kb == kb) return;
    for (int i = 0; i < MAX_KB; i++) {
        if (s_kb[i].used) continue;
        memset(&s_kb[i], 0, sizeof s_kb[i]);
        s_kb[i].used = true;
        s_kb[i].kb = kb;
        lv_obj_add_event_cb(kb, kb_deleted_cb, LV_EVENT_DELETE, NULL);
        if (!s_timer) s_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
        lv_timer_resume(s_timer);
        return;
    }
    /* more than MAX_KB keyboards alive at once: this one only gets LVGL's focus */
}

static void apply_cb(lv_theme_t *th, lv_obj_t *obj)
{
    (void)th;
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_style(obj, &s_cursor, LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &s_ring, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &s_placeholder, LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_add_event_cb(obj, ta_deleted_cb, LV_EVENT_DELETE, NULL);
        aos_textsel_attach(obj);        /* select, copy, paste (aos_textsel.c) */
    } else if (lv_obj_check_type(obj, &lv_keyboard_class)) {
        add_keyboard(obj);
        aos_keyboard_attach(obj);       /* ñ, accents, shift, the space bar (aos_keyboard.c) */
    }
}

lv_obj_t *aos_textfield_active(void)
{
    for (int i = 0; i < MAX_KB; i++)
        if (s_kb[i].used && s_kb[i].kb && s_kb[i].ta && s_kb[i].shown) return s_kb[i].ta;
    return NULL;
}

void aos_textfield_accent(void)
{
    if (!s_theme) return;
    lv_style_set_border_color(&s_cursor, AOS_C_ACCENT);
    lv_style_set_outline_color(&s_ring, AOS_C_ACCENT);
    lv_obj_report_style_change(&s_cursor);
    lv_obj_report_style_change(&s_ring);
}

void aos_textfield_init(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *base = disp ? lv_display_get_theme(disp) : NULL;
    if (!disp || s_theme) return;

    lv_style_init(&s_cursor);
    lv_style_set_border_side(&s_cursor, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_width(&s_cursor, CURSOR_W);
    lv_style_set_border_opa(&s_cursor, LV_OPA_COVER);
    lv_style_set_bg_opa(&s_cursor, LV_OPA_TRANSP);
    lv_style_set_anim_duration(&s_cursor, BLINK_MS);

    lv_style_init(&s_ring);
    lv_style_set_outline_width(&s_ring, RING_W);
    lv_style_set_outline_pad(&s_ring, -RING_W);
    lv_style_set_outline_opa(&s_ring, LV_OPA_COVER);

    lv_style_init(&s_placeholder);
    lv_style_set_text_color(&s_placeholder, AOS_C_DIM);

    s_theme = lv_theme_create();
    if (!s_theme) return;
    if (base) {
        lv_theme_copy(s_theme, base);
        lv_theme_set_parent(s_theme, base);
    }
    lv_theme_set_apply_cb(s_theme, apply_cb);
    aos_textfield_accent();
    lv_display_set_theme(disp, s_theme);
    aos_keyboard_init();
}
