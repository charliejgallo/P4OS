/*
 * P4OS - VNC viewer: the keyboard on the screen, and the USB keyboard's keys.
 *
 * A remote computer wants what LVGL's keyboard does not have: Esc, Tab,
 * Ctrl, Alt, Cmd, the arrows, F1-F12, Page Up. So this is a buttonmatrix of
 * its own, in four layers (letters, numbers, symbols, Fn), every key with
 * its X keysym beside it. The modifiers stay lit for the next key (Ctrl,
 * then C: a copy) and go off after it; touched again, they go off. A key
 * held repeats (the buttonmatrix's own repeat), each repeat a press and a
 * release for the server.
 *
 * Letters with Shift go as the capital's keysym, as a hardware keyboard's
 * would, and not as Shift + letter: every server reads that right.
 */
#include "vnc.h"

#include "aos_hal.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdlib.h>
#include <string.h>

enum { KA_CHAR = 0, KA_SYM, KA_MOD, KA_LAYER, KA_HIDE };
enum { M_SHIFT = 1, M_CTRL = 2, M_ALT = 4, M_CMD = 8 };
enum { L_ABC = 0, L_123, L_SYM, L_FN, L_COUNT };

typedef struct {
    const char *label;          /* NULL: end of row */
    uint8_t     act;
    uint8_t     w;              /* relative width, 1-15 */
    uint32_t    arg;            /* keysym, modifier bit, layer */
} vkey_t;

#define ROW_END { NULL, 0, 0, 0 }
#define CH(l)   { l, KA_CHAR, 2, 0 }

/* the row every layer starts with */
#define TOP_ROW \
    { "Esc", KA_SYM, 3, XK_Escape }, { "Tab", KA_SYM, 3, XK_Tab }, \
    { "Ctrl", KA_MOD, 3, M_CTRL }, { "Alt", KA_MOD, 3, M_ALT }, { "Cmd", KA_MOD, 3, M_CMD }, \
    { LV_SYMBOL_LEFT, KA_SYM, 2, XK_Left }, { LV_SYMBOL_UP, KA_SYM, 2, XK_Up }, \
    { LV_SYMBOL_DOWN, KA_SYM, 2, XK_Down }, { LV_SYMBOL_RIGHT, KA_SYM, 2, XK_Right }, ROW_END

static const vkey_t K_ABC[] = {
    TOP_ROW,
    CH("q"), CH("w"), CH("e"), CH("r"), CH("t"), CH("y"), CH("u"), CH("i"), CH("o"), CH("p"), ROW_END,
    CH("a"), CH("s"), CH("d"), CH("f"), CH("g"), CH("h"), CH("j"), CH("k"), CH("l"), CH("ñ"), ROW_END,
    { "Shift", KA_MOD, 3, M_SHIFT }, CH("z"), CH("x"), CH("c"), CH("v"), CH("b"), CH("n"), CH("m"),
    { LV_SYMBOL_BACKSPACE, KA_SYM, 3, XK_BackSpace }, ROW_END,
    { "123", KA_LAYER, 3, L_123 }, { "Fn", KA_LAYER, 3, L_FN }, { " ", KA_CHAR, 10, 0 },
    { LV_SYMBOL_NEW_LINE, KA_SYM, 4, XK_Return }, { LV_SYMBOL_KEYBOARD, KA_HIDE, 3, 0 }, ROW_END,
};

static const vkey_t K_123[] = {
    TOP_ROW,
    CH("1"), CH("2"), CH("3"), CH("4"), CH("5"), CH("6"), CH("7"), CH("8"), CH("9"), CH("0"), ROW_END,
    CH("-"), CH("/"), CH(":"), CH(";"), CH("("), CH(")"), CH("$"), CH("&"), CH("@"), CH("\""), ROW_END,
    { "#+=", KA_LAYER, 3, L_SYM }, CH("."), CH(","), CH("?"), CH("!"), CH("'"), CH("_"), CH("="),
    { LV_SYMBOL_BACKSPACE, KA_SYM, 3, XK_BackSpace }, ROW_END,
    { "ABC", KA_LAYER, 3, L_ABC }, { "Fn", KA_LAYER, 3, L_FN }, { " ", KA_CHAR, 10, 0 },
    { LV_SYMBOL_NEW_LINE, KA_SYM, 4, XK_Return }, { LV_SYMBOL_KEYBOARD, KA_HIDE, 3, 0 }, ROW_END,
};

static const vkey_t K_SYM[] = {
    TOP_ROW,
    CH("["), CH("]"), CH("{"), CH("}"), CH("#"), CH("%"), CH("^"), CH("*"), CH("+"), CH("="), ROW_END,
    CH("_"), CH("\\"), CH("|"), CH("~"), CH("<"), CH(">"), CH("€"), CH("`"), CH("¿"), CH("¡"), ROW_END,
    { "123", KA_LAYER, 3, L_123 }, CH("."), CH(","), CH("?"), CH("!"), CH("'"), CH("\""), CH("°"),
    { LV_SYMBOL_BACKSPACE, KA_SYM, 3, XK_BackSpace }, ROW_END,
    { "ABC", KA_LAYER, 3, L_ABC }, { "Fn", KA_LAYER, 3, L_FN }, { " ", KA_CHAR, 10, 0 },
    { LV_SYMBOL_NEW_LINE, KA_SYM, 4, XK_Return }, { LV_SYMBOL_KEYBOARD, KA_HIDE, 3, 0 }, ROW_END,
};

static const vkey_t K_FN[] = {
    TOP_ROW,
    { "F1", KA_SYM, 2, XK_F1 }, { "F2", KA_SYM, 2, XK_F1 + 1 }, { "F3", KA_SYM, 2, XK_F1 + 2 },
    { "F4", KA_SYM, 2, XK_F1 + 3 }, { "F5", KA_SYM, 2, XK_F1 + 4 }, { "F6", KA_SYM, 2, XK_F1 + 5 }, ROW_END,
    { "F7", KA_SYM, 2, XK_F1 + 6 }, { "F8", KA_SYM, 2, XK_F1 + 7 }, { "F9", KA_SYM, 2, XK_F1 + 8 },
    { "F10", KA_SYM, 2, XK_F1 + 9 }, { "F11", KA_SYM, 2, XK_F1 + 10 }, { "F12", KA_SYM, 2, XK_F1 + 11 }, ROW_END,
    { "Ins", KA_SYM, 2, XK_Insert }, { "Home", KA_SYM, 2, XK_Home }, { "PgUp", KA_SYM, 2, XK_Page_Up },
    { "Del", KA_SYM, 2, XK_Delete }, { "End", KA_SYM, 2, XK_End }, { "PgDn", KA_SYM, 2, XK_Page_Down }, ROW_END,
    { "ABC", KA_LAYER, 3, L_ABC }, { "123", KA_LAYER, 3, L_123 }, { " ", KA_CHAR, 10, 0 },
    { LV_SYMBOL_NEW_LINE, KA_SYM, 4, XK_Return }, { LV_SYMBOL_KEYBOARD, KA_HIDE, 3, 0 }, ROW_END,
};

static const vkey_t *const LAYERS[L_COUNT] = { K_ABC, K_123, K_SYM, K_FN };
static const int LAYER_LEN[L_COUNT] = {
    sizeof K_ABC / sizeof K_ABC[0], sizeof K_123 / sizeof K_123[0],
    sizeof K_SYM / sizeof K_SYM[0], sizeof K_FN / sizeof K_FN[0],
};

#define MAX_KEYS 64
#define ROWS     5

struct vnc_kbd {
    lv_obj_t    *bm;
    vnc_sess_t **sess;
    void       (*on_hide)(void *);
    void        *user;
    int          layer;
    uint8_t      mods;
    int32_t      h;
    const char  *map[MAX_KEYS + ROWS + 1];
    lv_buttonmatrix_ctrl_t ctrl[MAX_KEYS];
    const vkey_t *keys[MAX_KEYS];        /* button id -> key */
    char         upper[MAX_KEYS][4];    /* the letters' capitals, while Shift is on */
};

/* ---- keysyms ------------------------------------------------------------------ */

uint32_t vnc_keysym_char(uint32_t cp)
{
    if ((cp >= 0x20 && cp < 0x7F) || (cp >= 0xA0 && cp <= 0xFF)) return cp;   /* Latin-1 is itself */
    if (cp == 0x20AC) return 0x20AC;                                            /* XK_EuroSign */
    if (cp == '\n' || cp == '\r') return XK_Return;
    if (cp == '\t') return XK_Tab;
    if (cp == 8) return XK_BackSpace;
    return 0x01000000u | cp;                                                    /* any other Unicode */
}

static uint32_t utf8_first(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80) return p[0];
    if ((p[0] & 0xE0) == 0xC0) return (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
    if ((p[0] & 0xF0) == 0xE0) return (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
    return (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 | (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
}

static const uint32_t MOD_SYMS[4] = { XK_Shift_L, XK_Control_L, XK_Alt_L, XK_Super_L };

/* A key with its modifiers around it: downs, the key, ups. */
static void send_combo(vnc_sess_t *s, uint32_t keysym, uint8_t mods)
{
    for (int i = 0; i < 4; i++) if (mods & (1 << i)) vnc_sess_key(s, MOD_SYMS[i], true);
    vnc_sess_key(s, keysym, true);
    vnc_sess_key(s, keysym, false);
    for (int i = 3; i >= 0; i--) if (mods & (1 << i)) vnc_sess_key(s, MOD_SYMS[i], false);
}

/* ---- the keyboard on the screen -------------------------------------------------- */

static bool is_letter(uint32_t c)
{
    return (c >= 'a' && c <= 'z') || c == 0xF1;     /* ñ */
}

static void build(vnc_kbd_t *k)
{
    const vkey_t *keys = LAYERS[k->layer];
    int n = LAYER_LEN[k->layer], m = 0, b = 0, row = 0;
    for (int i = 0; i < n; i++) {
        if (!keys[i].label) {
            if (++row < ROWS) k->map[m++] = "\n";
            continue;
        }
        const vkey_t *key = &keys[i];
        const char *label = key->label;
        if (key->act == KA_CHAR && (k->mods & M_SHIFT)) {
            uint32_t c = utf8_first(label);
            if (c >= 'a' && c <= 'z') {
                k->upper[b][0] = (char)(c - 32);
                k->upper[b][1] = '\0';
                label = k->upper[b];
            } else if (c == 0xF1) {
                label = "Ñ";
            }
        }
        k->map[m++] = label;
        k->keys[b] = key;
        lv_buttonmatrix_ctrl_t c = (lv_buttonmatrix_ctrl_t)key->w;
        if (key->act == KA_MOD) {
            c |= LV_BUTTONMATRIX_CTRL_CHECKABLE | LV_BUTTONMATRIX_CTRL_NO_REPEAT;
            if (k->mods & key->arg) c |= LV_BUTTONMATRIX_CTRL_CHECKED;
        } else if (key->act == KA_LAYER || key->act == KA_HIDE) {
            c |= LV_BUTTONMATRIX_CTRL_NO_REPEAT;
        }
        k->ctrl[b++] = c;
    }
    k->map[m] = "";
    lv_buttonmatrix_set_map(k->bm, k->map);
    lv_buttonmatrix_set_ctrl_map(k->bm, k->ctrl);
}

static void key_cb(lv_event_t *e)
{
    vnc_kbd_t *k = lv_event_get_user_data(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(k->bm);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE || id >= MAX_KEYS || !k->keys[id]) return;
    const vkey_t *key = k->keys[id];
    vnc_sess_t *s = *k->sess;
    switch (key->act) {
    case KA_MOD:
        k->mods ^= (uint8_t)key->arg;
        build(k);
        return;
    case KA_LAYER:
        k->layer = (int)key->arg;
        build(k);
        return;
    case KA_HIDE:
        if (k->on_hide) k->on_hide(k->user);
        return;
    case KA_SYM:
        send_combo(s, key->arg, k->mods);
        break;
    case KA_CHAR: {
        uint32_t c = utf8_first(key->label);
        uint8_t mods = k->mods;
        if ((mods & M_SHIFT) && is_letter(c)) {
            c = c == 0xF1 ? 0xD1 : c - 32;
            mods &= (uint8_t)~M_SHIFT;
        } else if (!(mods & (M_CTRL | M_ALT | M_CMD))) {
            mods &= (uint8_t)~M_SHIFT;  /* a symbol is already what Shift would make */
        }
        send_combo(s, vnc_keysym_char(c), mods);
        break;
    }
    }
    if (k->mods) {
        k->mods = 0;
        build(k);
    }
}

vnc_kbd_t *vnc_kbd_create(lv_obj_t *parent, int32_t w, bool land, vnc_sess_t **sess,
                          void (*on_hide)(void *), void *user)
{
    vnc_kbd_t *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->sess = sess;
    k->on_hide = on_hide;
    k->user = user;
    k->h = land ? 330 : 440;
    k->bm = lv_buttonmatrix_create(parent);
    aos_keyboard_style(k->bm, land ? aos_font_caption : aos_font_small);
    lv_obj_set_style_pad_all(k->bm, 8, 0);
    lv_obj_set_style_pad_gap(k->bm, land ? 7 : 9, 0);
    lv_obj_set_style_bg_color(k->bm, lv_color_hex(0x2C2C2E), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(k->bm, AOS_C_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_size(k->bm, w, k->h);
    lv_obj_align(k->bm, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(k->bm, key_cb, LV_EVENT_VALUE_CHANGED, k);
    build(k);
    return k;
}

int32_t vnc_kbd_height(const vnc_kbd_t *k)
{
    return k ? k->h : 0;
}

lv_obj_t *vnc_kbd_obj(const vnc_kbd_t *k)
{
    return k ? k->bm : NULL;
}

void vnc_kbd_delete(vnc_kbd_t *k)
{
    if (!k) return;
    if (k->bm) lv_obj_delete(k->bm);
    free(k);
}

/* ---- the USB keyboard ------------------------------------------------------------- */

bool vnc_hwkey(vnc_sess_t *s, uint32_t key, uint8_t hid_mods)
{
    if (!s || key >= AOS_KEY_CONSUMER) return false;    /* media keys: the system's */
    uint8_t mods = 0;
    if (hid_mods & 0x22) mods |= M_SHIFT;
    if (hid_mods & 0x11) mods |= M_CTRL;
    if (hid_mods & 0x44) mods |= M_ALT;
    if (hid_mods & 0x88) mods |= M_CMD;
    uint32_t sym;
    switch (key) {
    case AOS_KEY_UP:        sym = XK_Up; break;
    case AOS_KEY_DOWN:      sym = XK_Down; break;
    case AOS_KEY_LEFT:      sym = XK_Left; break;
    case AOS_KEY_RIGHT:     sym = XK_Right; break;
    case AOS_KEY_ESC:       sym = XK_Escape; break;
    case AOS_KEY_DEL:       sym = XK_Delete; break;
    case AOS_KEY_BACKSPACE: sym = XK_BackSpace; break;
    case AOS_KEY_ENTER:     sym = XK_Return; break;
    case AOS_KEY_NEXT:      sym = XK_Tab; break;
    case AOS_KEY_PREV:      sym = XK_Tab; mods |= M_SHIFT; break;
    case AOS_KEY_HOME:      sym = XK_Home; break;
    case AOS_KEY_END:       sym = XK_End; break;
    default:
        if (key < 0x20) return false;
        sym = vnc_keysym_char(key);
        /* the character is already composed (Shift, AltGr): send it bare,
         * unless it is a shortcut */
        if (!(mods & (M_CTRL | M_CMD))) mods = 0;
        else mods &= (uint8_t)~M_SHIFT;
        break;
    }
    send_combo(s, sym, mods);
    return true;
}
