/*
 * P4OS - Notes: a Spanish keyboard.
 *
 * LVGL's own keyboard types into a textarea, and it has no ñ and no
 * accents. This one is a button matrix that types into a target - the
 * rich-text editor, or any textarea - with:
 *
 *   - ñ on the home row, and a dead accent key: ´ then a vowel is á, ¨ then
 *     u is ü (the second page has ¨, ß and the dashes and quotes the pack's
 *     fonts carry);
 *   - shift on its own at the start of a sentence (the target says when),
 *     shift twice for caps lock;
 *   - the space bar as a trackpad: slide along it and the caret follows.
 *
 * Keys fire on press, like a phone's, and backspace and the arrows repeat
 * when held; letters do not.
 */
#include "nt.h"
#include "aos_i18n.h"

#include <string.h>

#define K_SHIFT   LV_SYMBOL_UP
#define K_BKSP    LV_SYMBOL_BACKSPACE
#define K_ENTER   LV_SYMBOL_NEW_LINE
#define K_LEFT    LV_SYMBOL_LEFT
#define K_RIGHT   LV_SYMBOL_RIGHT
#define K_NUM     "123"
#define K_SYM     "#+="
#define K_ABC     "abc"
#define K_ACUTE   "\xC2\xB4"        /* ´ */
#define K_DIAER   "\xC2\xA8"        /* ¨ */

enum { M_LOWER, M_UPPER, M_SYM1, M_SYM2 };

static const char *const MAP_LOWER[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\xC3\xB1", "\n",
    K_SHIFT, "z", "x", "c", "v", "b", "n", "m", K_BKSP, "\n",
    K_NUM, K_ACUTE, ",", " ", ".", K_ENTER, "" };
static const char *const MAP_UPPER[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\xC3\x91", "\n",
    K_SHIFT, "Z", "X", "C", "V", "B", "N", "M", K_BKSP, "\n",
    K_NUM, K_ACUTE, ",", " ", ".", K_ENTER, "" };
static const char *const MAP_SYM1[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
    K_SYM, ".", ",", "?", "!", "'", "\xC2\xBF", "\xC2\xA1", K_BKSP, "\n",
    K_ABC, K_LEFT, " ", K_RIGHT, K_ENTER, "" };
static const char *const MAP_SYM2[] = {
    "[", "]", "{", "}", "#", "%", "^", "*", "+", "=", "\n",
    "_", "\\", "|", "~", "<", ">", "\xE2\x82\xAC", "\xC2\xA3", "\xE2\x80\xA2", "\xC2\xB0", "\n",
    K_NUM, "\xE2\x80\xA6", "\xE2\x80\x93", "\xE2\x80\x94", "\xC2\xAB", "\xC2\xBB", K_DIAER, "\xC3\x9F", K_BKSP, "\n",
    K_ABC, K_LEFT, " ", K_RIGHT, K_ENTER, "" };

#define NR LV_BUTTONMATRIX_CTRL_NO_REPEAT
#define CK LV_BUTTONMATRIX_CTRL_CHECKED
#define CT LV_BUTTONMATRIX_CTRL_CLICK_TRIG

/* Widths are in units of the row (at most 15: the bits above are flags);
 * every row adds up to 20. */
static const lv_buttonmatrix_ctrl_t CTRL_LETTERS[] = {
    2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR,
    2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR,
    3 | NR | CK, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 3 | CK,
    3 | NR | CK, 2 | NR, 2 | NR, 8 | NR | CT, 2 | NR, 3 | NR | CK };
static const lv_buttonmatrix_ctrl_t CTRL_SYM[] = {
    2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR,
    2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR,
    3 | NR | CK, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 2 | NR, 3 | CK,
    3 | NR | CK, 3 | CK, 8 | NR | CT, 3 | CK, 3 | NR | CK };

#define SHIFT_ID   20       /* index of the shift key in the letter maps */
#define ACUTE_ID   30

static struct {
    lv_obj_t *kb;
    nt_kb_target_t t;
    bool attached;
    int mode;
    bool caps_lock, auto_shift;
    uint32_t shift_ms;
    const char *dead;               /* the accent waiting for its vowel */
    /* trackpad */
    bool space_down, pad;
    int32_t pad_x;
    /* textarea target */
    lv_obj_t *ta;
    void (*ta_done)(lv_obj_t *);
} K;

static void apply_mode(void)
{
    if (!K.kb) return;
    switch (K.mode) {
    case M_UPPER: lv_buttonmatrix_set_map(K.kb, MAP_UPPER); lv_buttonmatrix_set_ctrl_map(K.kb, CTRL_LETTERS); break;
    case M_SYM1:  lv_buttonmatrix_set_map(K.kb, MAP_SYM1);  lv_buttonmatrix_set_ctrl_map(K.kb, CTRL_SYM); break;
    case M_SYM2:  lv_buttonmatrix_set_map(K.kb, MAP_SYM2);  lv_buttonmatrix_set_ctrl_map(K.kb, CTRL_SYM); break;
    default:      lv_buttonmatrix_set_map(K.kb, MAP_LOWER); lv_buttonmatrix_set_ctrl_map(K.kb, CTRL_LETTERS); break;
    }
    if (K.mode == M_UPPER) lv_buttonmatrix_clear_button_ctrl(K.kb, SHIFT_ID, CK);   /* lit while on */
    if ((K.mode == M_LOWER || K.mode == M_UPPER) && K.dead) lv_buttonmatrix_set_button_ctrl(K.kb, ACUTE_ID, CK);
}

static void set_mode(int m)
{
    if (K.mode == m) return;
    K.mode = m;
    apply_mode();
}

void nt_kb_refresh_caps(void)
{
    if (!K.kb || !K.attached || K.caps_lock) return;
    if (K.mode != M_LOWER && K.mode != M_UPPER) return;
    bool want = K.t.caps && K.t.caps();
    K.auto_shift = want;
    set_mode(want ? M_UPPER : M_LOWER);
}

static const char *accent(const char *dead, const char *key)
{
    static const char *const ACUTE[][2] = {
        { "a", "\xC3\xA1" }, { "e", "\xC3\xA9" }, { "i", "\xC3\xAD" }, { "o", "\xC3\xB3" }, { "u", "\xC3\xBA" },
        { "A", "\xC3\x81" }, { "E", "\xC3\x89" }, { "I", "\xC3\x8D" }, { "O", "\xC3\x93" }, { "U", "\xC3\x9A" },
        { "y", "\xC3\xBD" }, { "Y", "\xC3\x9D" } };
    static const char *const DIAER[][2] = {
        { "u", "\xC3\xBC" }, { "U", "\xC3\x9C" }, { "a", "\xC3\xA4" }, { "A", "\xC3\x84" },
        { "o", "\xC3\xB6" }, { "O", "\xC3\x96" }, { "e", "\xC3\xAB" }, { "i", "\xC3\xAF" } };
    if (strcmp(dead, K_ACUTE) == 0) {
        for (size_t k = 0; k < sizeof ACUTE / sizeof ACUTE[0]; k++)
            if (strcmp(key, ACUTE[k][0]) == 0) return ACUTE[k][1];
    } else {
        for (size_t k = 0; k < sizeof DIAER / sizeof DIAER[0]; k++)
            if (strcmp(key, DIAER[k][0]) == 0) return DIAER[k][1];
    }
    return NULL;
}

static void type(const char *s)
{
    if (K.t.insert) K.t.insert(s);
}

static void key(const char *txt)
{
    if (!K.attached) return;
    if (strcmp(txt, K_SHIFT) == 0) {
        uint32_t now = lv_tick_get();
        if (K.mode == M_UPPER && now - K.shift_ms < 400) {
            K.caps_lock = true;
        } else if (K.mode == M_UPPER) {
            K.caps_lock = false;
            set_mode(M_LOWER);
        } else {
            K.caps_lock = false;
            set_mode(M_UPPER);
        }
        K.auto_shift = false;
        K.shift_ms = now;
        return;
    }
    if (strcmp(txt, K_NUM) == 0) { set_mode(M_SYM1); return; }
    if (strcmp(txt, K_SYM) == 0) { set_mode(M_SYM2); return; }
    if (strcmp(txt, K_ABC) == 0) { K.caps_lock = false; set_mode(M_LOWER); nt_kb_refresh_caps(); return; }
    if (strcmp(txt, K_ACUTE) == 0 || strcmp(txt, K_DIAER) == 0) {
        if (K.dead && strcmp(K.dead, txt) == 0) {       /* twice: the mark itself */
            K.dead = NULL;
            type(txt);
        } else {
            K.dead = strcmp(txt, K_ACUTE) == 0 ? K_ACUTE : K_DIAER;
        }
        apply_mode();
        return;
    }
    if (strcmp(txt, K_BKSP) == 0) {
        if (K.dead) { K.dead = NULL; apply_mode(); return; }
        if (K.t.backspace) K.t.backspace();
        nt_kb_refresh_caps();
        return;
    }
    if (strcmp(txt, K_ENTER) == 0) {
        if (K.t.enter) K.t.enter();
        nt_kb_refresh_caps();
        return;
    }
    if (strcmp(txt, K_LEFT) == 0)  { if (K.t.move) K.t.move(-1); return; }
    if (strcmp(txt, K_RIGHT) == 0) { if (K.t.move) K.t.move(1); return; }

    if (K.dead) {
        const char *d = K.dead;
        K.dead = NULL;
        const char *a = accent(d, txt);
        if (a) type(a);
        else { type(d); type(txt); }
        apply_mode();
    } else {
        type(txt);
    }
    if (K.mode == M_UPPER && !K.caps_lock) {
        set_mode(M_LOWER);
    }
    /* after ". " or "?", up again */
    if (strcmp(txt, " ") == 0 || K.mode == M_LOWER) nt_kb_refresh_caps();
    /* the symbol pages go back to letters after a space, like a phone's */
    if ((K.mode == M_SYM1 || K.mode == M_SYM2) && strcmp(txt, " ") == 0) {
        set_mode(M_LOWER);
        nt_kb_refresh_caps();
    }
}

static bool is_space(uint32_t id)
{
    const char *t = lv_buttonmatrix_get_button_text(K.kb, id);
    return t && strcmp(t, " ") == 0;
}

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        uint32_t id = lv_buttonmatrix_get_selected_button(K.kb);
        if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
        const char *txt = lv_buttonmatrix_get_button_text(K.kb, id);
        if (txt) key(txt);
    } else if (code == LV_EVENT_PRESSED) {
        uint32_t id = lv_buttonmatrix_get_selected_button(K.kb);
        K.space_down = id != LV_BUTTONMATRIX_BUTTON_NONE && is_space(id);
        K.pad = false;
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        K.pad_x = p.x;
    } else if (code == LV_EVENT_PRESSING && K.space_down) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        int32_t dx = p.x - K.pad_x;
        if (!K.pad && LV_ABS(dx) > 24) {
            K.pad = true;
            K.pad_x = p.x;
            /* no space on release */
            lv_buttonmatrix_set_selected_button(K.kb, LV_BUTTONMATRIX_BUTTON_NONE);
            return;
        }
        if (K.pad) {
            int steps = dx / 16;
            if (steps && K.t.move) {
                K.t.move(steps);
                K.pad_x += steps * 16;
            }
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (K.pad) nt_kb_refresh_caps();
        K.space_down = false;
        K.pad = false;
    }
}

static void kb_delete_cb(lv_event_t *e)
{
    (void)e;
    K.kb = NULL;
}

lv_obj_t *nt_kb_create(lv_obj_t *parent, int32_t w, int32_t h)
{
    K.kb = lv_buttonmatrix_create(parent);
    lv_obj_set_size(K.kb, w, h);
    lv_obj_set_style_text_font(K.kb, nt_font(0, h > 360 ? NT_SZ_M : NT_SZ_S), 0);
    lv_obj_set_style_bg_color(K.kb, NT.kb_bg, 0);
    lv_obj_set_style_bg_opa(K.kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(K.kb, 0, 0);
    lv_obj_set_style_radius(K.kb, 0, 0);
    lv_obj_set_style_pad_all(K.kb, 8, 0);
    lv_obj_set_style_pad_gap(K.kb, h > 360 ? 12 : 8, 0);
    lv_obj_set_style_bg_color(K.kb, NT.kb_key, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(K.kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(K.kb, NT.kb_text, LV_PART_ITEMS);
    lv_obj_set_style_border_width(K.kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(K.kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(K.kb, 12, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(K.kb, NT.kb_key2, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(K.kb, NT.kb_text, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(K.kb, NT.dark ? lv_color_hex(0x8E8E93) : lv_color_hex(0x9C9FA8),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_remove_flag(K.kb, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(K.kb, kb_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(K.kb, kb_delete_cb, LV_EVENT_DELETE, NULL);
    K.mode = -1;
    set_mode(M_LOWER);
    nt_kb_refresh_caps();
    return K.kb;
}

void nt_kb_attach(const nt_kb_target_t *t)
{
    K.dead = NULL;
    K.caps_lock = false;
    if (t) {
        K.t = *t;
        K.attached = true;
    } else {
        memset(&K.t, 0, sizeof K.t);
        K.attached = false;
    }
    K.ta = NULL;
    if (K.kb) {
        set_mode(M_LOWER);
        nt_kb_refresh_caps();
    }
}

void nt_kb_destroyed(void)
{
    K.kb = NULL;
    K.attached = false;
    K.ta = NULL;
}

/* -------------------------------------------------------------------------- */
/* Into a textarea                                                             */
/* -------------------------------------------------------------------------- */

static void ta_insert(const char *s)  { if (K.ta) lv_textarea_add_text(K.ta, s); }
static void ta_bksp(void)             { if (K.ta) lv_textarea_delete_char(K.ta); }
static void ta_enter(void)            { if (K.ta && K.ta_done) K.ta_done(K.ta); }
static void ta_move(int dx)
{
    if (!K.ta) return;
    for (; dx < 0; dx++) lv_textarea_cursor_left(K.ta);
    for (; dx > 0; dx--) lv_textarea_cursor_right(K.ta);
}
static bool ta_caps(void)
{
    if (!K.ta) return false;
    uint32_t pos = lv_textarea_get_cursor_pos(K.ta);
    if (pos == 0) return lv_obj_get_user_data(K.ta) == NULL;   /* user data set = no auto shift */
    return false;
}

static void ta_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == K.ta) {
        K.ta = NULL;
        K.attached = false;
    }
}

void nt_kb_attach_textarea(lv_obj_t *ta, void (*done)(lv_obj_t *ta))
{
    static const nt_kb_target_t t = { ta_insert, ta_bksp, ta_enter, ta_move, ta_caps };
    nt_kb_attach(&t);
    K.ta = ta;
    K.ta_done = done;
    lv_obj_add_event_cb(ta, ta_delete_cb, LV_EVENT_DELETE, NULL);
    nt_kb_refresh_caps();
}
