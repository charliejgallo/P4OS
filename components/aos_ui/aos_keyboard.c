/*
 * P4OS - the on-screen keyboard, written the way a phone's is (2026-11-08).
 *
 * Every lv_keyboard of the system and of the card's apps: the theme of
 * aos_textfield.c hands each one here as it is created, so no app has to
 * know. What it adds to LVGL's keyboard:
 *
 *   - Spanish: an ñ key beside the l, and holding a key that has variants
 *     opens a bubble over it (á à ä â, ü, ñ, ç, ¿, ¡...); sliding onto one
 *     and letting go types it, letting go anywhere else types the key.
 *   - Shift as on the phone: one tap is a capital for the next letter only,
 *     two quick taps lock it (the key then reads ABC), a tap more unlocks.
 *     An app that sets LV_KEYBOARD_MODE_TEXT_UPPER itself gets it locked,
 *     as before.
 *   - The space bar as a trackpad: drag along it (or hold it and then
 *     drag) and the cursor moves, by letters sideways and by lines up and
 *     down in a text of several lines; no space is typed then. A plain tap
 *     types the space when the finger lifts.
 *   - Letters no longer repeat while held (the hold is for the bubble);
 *     backspace and the arrows still do.
 *
 * LVGL's keyboard keeps doing the rest (lv_keyboard_def_event_cb). Its maps
 * are global (kb_map in lv_keyboard.c), so ours are set once, at start, for
 * the text modes and the symbols. The space and shift keys, and a capital
 * typed once, are handled here before LVGL sees them: a pre-process
 * callback that stops the event. Theme callbacks run before the widget's
 * constructor, so this is the only order that is sure to come first.
 */
#include "aos_internal.h"
#include "aos_theme.h"

#include "lvgl.h"
#include "lvgl_private.h"           /* lv_keyboard_t, lv_buttonmatrix_t */

#include <stdbool.h>
#include <string.h>

#define MAX_KB          8
#define MOVE_PX         14          /* a drag on the space bar starts here */
#define STEP_X          22          /* px of drag per letter */
#define STEP_Y          44          /* px of drag per line */
#define DOUBLE_TAP_MS   400
#define CELL_W          76
#define CELL_H          92

#define SHIFT_KEY       LV_SYMBOL_UP
#define CAPS_KEY        "ABC"

/* ---- the maps ---- */

/* A key types when the finger lifts, as on a phone (and so a slide off it
 * cancels it, and a hold can open the bubble first). LVGL's own maps get
 * that from POPOVER, which it strips when its popovers are off: CLICK_TRIG
 * it leaves alone. */
#define K(w)    (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT | (w))
#define MODE(w) (LV_KEYBOARD_CTRL_BUTTON_FLAGS | (w))
#define DARK(w) (LV_BUTTONMATRIX_CTRL_CHECKED | (w))
#define SPACE   (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT | 6)
/* the shift key lit (lighter: no CHECKED) while capitals are on */
#define LIT(w)  (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT | (w))

static const char *const MAP_LC[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    SHIFT_KEY, "a", "s", "d", "f", "g", "h", "j", "k", "l", "ñ", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const char *const MAP_UC_ONCE[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    SHIFT_KEY, "A", "S", "D", "F", "G", "H", "J", "K", "L", "Ñ", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const char *const MAP_UC_LOCK[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    CAPS_KEY, "A", "S", "D", "F", "G", "H", "J", "K", "L", "Ñ", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

#define ROW1 MODE(5), K(4), K(4), K(4), K(4), K(4), K(4), K(4), K(4), K(4), K(4), DARK(7)
#define ROW3 DARK(K(1)), DARK(K(1)), K(1), K(1), K(1), K(1), K(1), K(1), K(1), DARK(K(1)), DARK(K(1)), DARK(K(1))
#define ROW4 MODE(2), DARK(2), SPACE, DARK(2), MODE(2)

static const lv_buttonmatrix_ctrl_t CTRL_LC[] = {
    ROW1,
    MODE(6), K(3), K(3), K(3), K(3), K(3), K(3), K(3), K(3), K(3), K(3), DARK(7),
    ROW3, ROW4
};
static const lv_buttonmatrix_ctrl_t CTRL_UC[] = {
    ROW1,
    LIT(6), K(3), K(3), K(3), K(3), K(3), K(3), K(3), K(3), K(3), K(3), DARK(7),
    ROW3, ROW4
};

/* LVGL's symbols page with the same space bar and no repeat on its keys */
static const char *const MAP_SPEC[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t CTRL_SPEC[] = {
    K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), DARK(2),
    MODE(2), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1),
    K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1), K(1),
    ROW4
};

/* ---- the variants a held key offers ---- */

typedef struct {
    const char *key;
    const char *v[5];
} variants_t;

static const variants_t VARIANTS[] = {
    { "a", { "á", "à", "ä", "â" } }, { "A", { "Á", "À", "Ä", "Â" } },
    { "e", { "é", "è", "ë", "ê" } }, { "E", { "É", "È", "Ë", "Ê" } },
    { "i", { "í", "ì", "ï", "î" } }, { "I", { "Í", "Ì", "Ï", "Î" } },
    { "o", { "ó", "ò", "ö", "ô" } }, { "O", { "Ó", "Ò", "Ö", "Ô" } },
    { "u", { "ú", "ü", "ù", "û" } }, { "U", { "Ú", "Ü", "Ù", "Û" } },
    { "n", { "ñ" } },                { "N", { "Ñ" } },
    { "c", { "ç" } },                { "C", { "Ç" } },
    { "?", { "¿" } },                { "!", { "¡" } },
    { "-", { "—", "–" } },           { ".", { "…" } },
    { "$", { "€", "£" } },
};

static const variants_t *variants_of(const char *key)
{
    for (size_t i = 0; key && i < sizeof VARIANTS / sizeof VARIANTS[0]; i++)
        if (!strcmp(VARIANTS[i].key, key)) return &VARIANTS[i];
    return NULL;
}

/* ---- per keyboard ---- */

typedef struct {
    lv_obj_t *kb;
    bool      once;             /* capitals for one letter */
    uint32_t  shift_ms;         /* when that began: a second tap soon locks */
    /* the space bar */
    bool      space;            /* pressed on it */
    bool      moved;            /* used as a trackpad: no space typed */
    lv_point_t last;
    int32_t   acc_x, acc_y;
    /* the bubble of variants */
    lv_obj_t *pop;
    const variants_t *var;
    int       nvar, sel;
    lv_area_t key_area;         /* the held key, on the screen */
} kst_t;

static kst_t s_k[MAX_KB];

static kst_t *state_of(lv_obj_t *kb)
{
    for (int i = 0; i < MAX_KB; i++)
        if (s_k[i].kb == kb) return &s_k[i];
    return NULL;
}

static lv_obj_t *ta_of(lv_obj_t *kb) { return lv_keyboard_get_textarea(kb); }

static void set_lower(lv_obj_t *kb, kst_t *s)
{
    s->once = false;
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
}

/* Capitals, for one letter or locked. Straight onto the button matrix:
 * lv_keyboard_set_mode() does nothing when the mode is already UPPER */
static void set_upper(lv_obj_t *kb, kst_t *s, bool once)
{
    lv_keyboard_t *k = (lv_keyboard_t *)kb;
    s->once = once;
    s->shift_ms = lv_tick_get();
    k->mode = LV_KEYBOARD_MODE_TEXT_UPPER;
    lv_buttonmatrix_set_map(kb, once ? MAP_UC_ONCE : MAP_UC_LOCK);
    lv_buttonmatrix_set_ctrl_map(kb, CTRL_UC);
}

static void type_text(lv_obj_t *kb, kst_t *s, const char *txt)
{
    lv_obj_t *ta = ta_of(kb);
    if (ta) lv_textarea_add_text(ta, txt);
    if (s->once) set_lower(kb, s);
}

static bool is_letter(const char *t)
{
    if (!t || !t[0]) return false;
    unsigned char c = (unsigned char)t[0];
    if (c >= 0x80) return true;                     /* ñ, Ñ */
    return !t[1] && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

/* ---- the bubble ---- */

static void pop_close(kst_t *s)
{
    if (s->pop) lv_obj_delete(s->pop);
    s->pop = NULL;
    s->var = NULL;
    s->nvar = 0;
    s->sel = -1;
}

static void pop_mark(kst_t *s)
{
    for (int i = 0; s->pop && i < s->nvar; i++) {
        lv_obj_t *c = lv_obj_get_child(s->pop, i);
        lv_obj_set_style_bg_opa(c, i == s->sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void pop_open(lv_obj_t *kb, kst_t *s, uint32_t btn, const variants_t *v)
{
    lv_buttonmatrix_t *bm = (lv_buttonmatrix_t *)kb;
    if (btn >= bm->btn_cnt) return;
    pop_close(s);
    lv_area_t kc;
    lv_obj_get_coords(kb, &kc);
    s->key_area = bm->button_areas[btn];
    lv_area_move(&s->key_area, kc.x1, kc.y1);
    s->var = v;
    s->nvar = 0;
    while (s->nvar < 5 && v->v[s->nvar]) s->nvar++;
    s->sel = -1;

    const aos_geo_t *g = aos_ui_geo();
    int32_t w = s->nvar * CELL_W + 12, h = CELL_H + 12;
    int32_t x = s->key_area.x1 + lv_area_get_width(&s->key_area) / 2 - CELL_W / 2 - 6;
    if (x + w > g->w - 6) x = g->w - 6 - w;
    if (x < 6) x = 6;
    int32_t y = s->key_area.y1 - h - 8;
    if (y < 0) y = 0;

    lv_obj_t *p = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x5A5A5E), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 16, 0);
    lv_obj_set_style_shadow_width(p, 24, 0);
    lv_obj_set_style_shadow_opa(p, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(p, 6, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < s->nvar; i++) {
        lv_obj_t *c = lv_label_create(p);
        lv_label_set_text(c, v->v[i]);
        lv_obj_set_size(c, CELL_W, CELL_H);
        lv_obj_set_pos(c, i * CELL_W, 0);
        lv_obj_set_style_text_font(c, aos_font_title, 0);
        lv_obj_set_style_text_color(c, lv_color_white(), 0);
        lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(c, (CELL_H - lv_font_get_line_height(aos_font_title)) / 2, 0);
        lv_obj_set_style_bg_color(c, AOS_C_ACCENT, 0);
        lv_obj_set_style_radius(c, 12, 0);
    }
    s->pop = p;
    /* one variant: it is the one, as the phone does */
    if (s->nvar == 1) s->sel = 0;
    pop_mark(s);
}

/* the finger over the bubble: which variant, by its x, once it is above
 * the key it held */
static void pop_track(kst_t *s, const lv_point_t *p)
{
    if (!s->pop) return;
    int sel = s->sel;
    lv_area_t a;
    lv_obj_get_coords(s->pop, &a);
    if (p->y < s->key_area.y1 + lv_area_get_height(&s->key_area) / 2) {
        int32_t i = (p->x - a.x1 - 6) / CELL_W;
        sel = i < 0 ? 0 : i >= s->nvar ? s->nvar - 1 : (int)i;
    } else if (s->nvar > 1) {
        sel = -1;
    }
    if (sel != s->sel) {
        s->sel = sel;
        pop_mark(s);
    }
}

/* ---- events ---- */

static void click(void) { aos_ui_sound(AOS_SND_KEY); }

/* Before LVGL: what is handled here stops the event */
static void pre_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *kb = lv_event_get_current_target(e);
    kst_t *s = state_of(kb);
    if (!s) return;
    uint32_t btn = lv_buttonmatrix_get_selected_button(kb);
    const char *txt = btn == LV_BUTTONMATRIX_BUTTON_NONE ? NULL : lv_buttonmatrix_get_button_text(kb, btn);
    if (!txt) return;

    if (s->pop) {                       /* the bubble decides on release */
        lv_event_stop_processing(e);
        return;
    }
    if (!strcmp(txt, " ")) {
        lv_event_stop_processing(e);
        if (!s->moved && ta_of(kb)) {
            click();
            lv_textarea_add_char(ta_of(kb), ' ');
        }
        return;
    }
    if (!strcmp(txt, SHIFT_KEY) || !strcmp(txt, CAPS_KEY)) {
        lv_event_stop_processing(e);
        click();
        lv_keyboard_mode_t mode = lv_keyboard_get_mode(kb);
        if (mode != LV_KEYBOARD_MODE_TEXT_UPPER) set_upper(kb, s, true);
        else if (s->once && lv_tick_elaps(s->shift_ms) < DOUBLE_TAP_MS) set_upper(kb, s, false);
        else set_lower(kb, s);
        return;
    }
    if (s->once && is_letter(txt)) {
        lv_event_stop_processing(e);
        click();
        type_text(kb, s, txt);
        return;
    }
}

static void post_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *kb = lv_event_get_current_target(e);
    kst_t *s = state_of(kb);
    if (!s) return;
    if (code == LV_EVENT_DELETE) {
        pop_close(s);
        memset(s, 0, sizeof *s);
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (indev) lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        uint32_t btn = lv_buttonmatrix_get_selected_button(kb);
        const char *txt = btn == LV_BUTTONMATRIX_BUTTON_NONE ? NULL : lv_buttonmatrix_get_button_text(kb, btn);
        s->space = txt && !strcmp(txt, " ");
        s->moved = false;
        s->last = p;
        s->acc_x = s->acc_y = 0;
        pop_close(s);
    } else if (code == LV_EVENT_PRESSING) {
        if (s->pop) {
            pop_track(s, &p);
            return;
        }
        if (!s->space) return;
        int32_t dx = p.x - s->last.x, dy = p.y - s->last.y;
        if (!s->moved) {
            if (LV_ABS(dx) < MOVE_PX && LV_ABS(dy) < MOVE_PX) return;
            s->moved = true;            /* a trackpad from here on */
            s->last = p;
            return;
        }
        s->acc_x += dx;
        s->acc_y += dy;
        s->last = p;
        lv_obj_t *ta = ta_of(kb);
        if (!ta) return;
        while (s->acc_x >= STEP_X) { lv_textarea_cursor_right(ta); s->acc_x -= STEP_X; }
        while (s->acc_x <= -STEP_X) { lv_textarea_cursor_left(ta); s->acc_x += STEP_X; }
        if (lv_textarea_get_one_line(ta)) s->acc_y = 0;
        while (s->acc_y >= STEP_Y) { lv_textarea_cursor_down(ta); s->acc_y -= STEP_Y; }
        while (s->acc_y <= -STEP_Y) { lv_textarea_cursor_up(ta); s->acc_y += STEP_Y; }
    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (s->space) {
            s->moved = true;            /* held: a trackpad, no space */
            s->last = p;
            return;
        }
        uint32_t btn = lv_buttonmatrix_get_selected_button(kb);
        const char *txt = btn == LV_BUTTONMATRIX_BUTTON_NONE ? NULL : lv_buttonmatrix_get_button_text(kb, btn);
        const variants_t *v = variants_of(txt);
        if (v && ta_of(kb)) pop_open(kb, s, btn, v);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s->pop) {
            int sel = s->sel;
            const variants_t *v = s->var;
            pop_close(s);
            if (code == LV_EVENT_RELEASED && v) {
                click();
                /* none chosen: the key itself, as if tapped */
                type_text(kb, s, sel >= 0 ? v->v[sel] : v->key);
            }
        }
        s->space = false;
    }
}

void aos_keyboard_attach(lv_obj_t *kb)
{
    if (state_of(kb)) return;
    kst_t *s = state_of(NULL);
    if (!s) return;                     /* more than MAX_KB at once: LVGL's own */
    memset(s, 0, sizeof *s);
    s->kb = kb;
    s->sel = -1;
    lv_obj_add_event_cb(kb, pre_cb, LV_EVENT_ALL | LV_EVENT_PREPROCESS, NULL);
    lv_obj_add_event_cb(kb, post_cb, LV_EVENT_ALL, NULL);
}

void aos_keyboard_init(void)
{
    /* LVGL's maps are global and lv_keyboard_set_map() wants a keyboard:
     * one, hidden, for the moment it takes */
    lv_obj_t *kb = lv_keyboard_create(lv_layer_top());
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, MAP_LC, CTRL_LC);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, MAP_UC_LOCK, CTRL_UC);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, MAP_SPEC, CTRL_SPEC);
    lv_obj_delete(kb);
}
