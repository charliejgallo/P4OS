/*
 * P4OS - SSH client: keys as a terminal sends them.
 *
 * The keyboard on the screen is apps/vnc's (vnc_kbd.c) made for a shell: a
 * buttonmatrix in four layers (letters, numbers, symbols, Fn) whose top row
 * is Esc, Tab, Ctrl, Alt and the arrows. Ctrl and Alt stay lit for the next
 * key (Ctrl, then C) and go off after it. A key held repeats.
 *
 * Every key, from it or from a USB keyboard, becomes xterm's bytes in
 * sc_key_bytes(): the arrows as ESC [ A or, when the computer asked for it
 * (vim, less), ESC O A; with modifiers as ESC [ 1 ; m A; Ctrl+letter as
 * its control code; Alt as an ESC before the key; Backspace as DEL.
 */
#include "sc.h"

#include "aos_hal.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the bytes ---- */

static size_t utf8(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | (c >> 12 & 0x3F));
    o[2] = (char)(0x80 | (c >> 6 & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

size_t sc_key_bytes(uint32_t key, uint8_t mods, bool app_cursor, char out[16])
{
    int m = 1 + ((mods & SC_M_SHIFT) ? 1 : 0) + ((mods & SC_M_ALT) ? 2 : 0) + ((mods & SC_M_CTRL) ? 4 : 0);
    static const char ARROWS[] = "ABCD";    /* up down right left */
    if (key >= SC_K_UP && key <= SC_K_LEFT) {
        char f = ARROWS[key - SC_K_UP];
        if (m > 1) return (size_t)snprintf(out, 16, "\x1b[1;%d%c", m, f);
        return (size_t)snprintf(out, 16, app_cursor ? "\x1bO%c" : "\x1b[%c", f);
    }
    if (key == SC_K_HOME || key == SC_K_END) {
        char f = key == SC_K_HOME ? 'H' : 'F';
        if (m > 1) return (size_t)snprintf(out, 16, "\x1b[1;%d%c", m, f);
        return (size_t)snprintf(out, 16, app_cursor ? "\x1bO%c" : "\x1b[%c", f);
    }
    int tilde = key == SC_K_INS ? 2 : key == SC_K_DEL ? 3 : key == SC_K_PGUP ? 5 : key == SC_K_PGDN ? 6 : 0;
    if (key >= SC_K_F1 && key < SC_K_F1 + 12) {
        static const int FN[12] = { 0, 0, 0, 0, 15, 17, 18, 19, 20, 21, 23, 24 };
        int i = (int)(key - SC_K_F1);
        if (i < 4) {
            if (m > 1) return (size_t)snprintf(out, 16, "\x1b[1;%d%c", m, "PQRS"[i]);
            return (size_t)snprintf(out, 16, "\x1bO%c", "PQRS"[i]);
        }
        tilde = FN[i];
    }
    if (tilde) {
        if (m > 1) return (size_t)snprintf(out, 16, "\x1b[%d;%d~", tilde, m);
        return (size_t)snprintf(out, 16, "\x1b[%d~", tilde);
    }
    size_t n = 0;
    if (mods & SC_M_ALT) out[n++] = 0x1b;
    switch (key) {
    case SC_K_ESC: out[n++] = 0x1b; return n;
    case SC_K_TAB: out[n++] = '\t'; return n;
    case SC_K_BTAB: return (size_t)snprintf(out, 16, "\x1b[Z");
    case SC_K_ENTER: out[n++] = '\r'; return n;
    case SC_K_BS: out[n++] = (mods & SC_M_CTRL) ? 8 : 0x7F; return n;
    default: break;
    }
    if (key >= 0x110000) return 0;
    if (mods & SC_M_CTRL) {
        uint32_t c = key;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c >= '@' && c <= '_') { out[n++] = (char)(c - '@'); return n; }
        if (c == ' ' || c == '2') { out[n++] = 0; return n; }
        if (c == '?' || c == '8') { out[n++] = 0x7F; return n; }
        if (c >= '3' && c <= '7') { out[n++] = (char)(c - '3' + 27); return n; }
    }
    return n + utf8(key, out + n);
}

/* ---- the keyboard on the screen ---- */

enum { KA_CHAR = 0, KA_KEY, KA_MOD, KA_LAYER, KA_HIDE };
enum { L_ABC = 0, L_123, L_SYM, L_FN, L_COUNT };

typedef struct {
    const char *label;          /* NULL: end of row */
    uint8_t     act;
    uint8_t     w;              /* relative width, 1-15 */
    uint32_t    arg;            /* a key, a modifier bit, a layer */
} skey_t;

#define ROW_END { NULL, 0, 0, 0 }
#define CH(l)   { l, KA_CHAR, 2, 0 }

#define TOP_ROW \
    { "Esc", KA_KEY, 3, SC_K_ESC }, { "Tab", KA_KEY, 3, SC_K_TAB }, \
    { "Ctrl", KA_MOD, 3, SC_M_CTRL }, { "Alt", KA_MOD, 3, SC_M_ALT }, \
    { LV_SYMBOL_LEFT, KA_KEY, 2, SC_K_LEFT }, { LV_SYMBOL_UP, KA_KEY, 2, SC_K_UP }, \
    { LV_SYMBOL_DOWN, KA_KEY, 2, SC_K_DOWN }, { LV_SYMBOL_RIGHT, KA_KEY, 2, SC_K_RIGHT }, ROW_END

#define BOTTOM_ROW(a, la, b, lb) \
    { a, KA_LAYER, 3, la }, { b, KA_LAYER, 3, lb }, { " ", KA_CHAR, 10, 0 }, \
    { LV_SYMBOL_NEW_LINE, KA_KEY, 4, SC_K_ENTER }, { LV_SYMBOL_KEYBOARD, KA_HIDE, 3, 0 }, ROW_END

static const skey_t K_ABC[] = {
    TOP_ROW,
    CH("q"), CH("w"), CH("e"), CH("r"), CH("t"), CH("y"), CH("u"), CH("i"), CH("o"), CH("p"), ROW_END,
    CH("a"), CH("s"), CH("d"), CH("f"), CH("g"), CH("h"), CH("j"), CH("k"), CH("l"), CH("ñ"), ROW_END,
    { "Shift", KA_MOD, 3, SC_M_SHIFT }, CH("z"), CH("x"), CH("c"), CH("v"), CH("b"), CH("n"), CH("m"),
    { LV_SYMBOL_BACKSPACE, KA_KEY, 3, SC_K_BS }, ROW_END,
    BOTTOM_ROW("123", L_123, "Fn", L_FN),
};

static const skey_t K_123[] = {
    TOP_ROW,
    CH("1"), CH("2"), CH("3"), CH("4"), CH("5"), CH("6"), CH("7"), CH("8"), CH("9"), CH("0"), ROW_END,
    CH("-"), CH("/"), CH(":"), CH(";"), CH("("), CH(")"), CH("$"), CH("&"), CH("@"), CH("\""), ROW_END,
    { "#+=", KA_LAYER, 3, L_SYM }, CH("."), CH(","), CH("?"), CH("!"), CH("'"), CH("_"), CH("="),
    { LV_SYMBOL_BACKSPACE, KA_KEY, 3, SC_K_BS }, ROW_END,
    BOTTOM_ROW("ABC", L_ABC, "Fn", L_FN),
};

static const skey_t K_SYM[] = {
    TOP_ROW,
    CH("["), CH("]"), CH("{"), CH("}"), CH("#"), CH("%"), CH("^"), CH("*"), CH("+"), CH("="), ROW_END,
    CH("_"), CH("\\"), CH("|"), CH("~"), CH("<"), CH(">"), CH("`"), CH("€"), CH("¿"), CH("¡"), ROW_END,
    { "123", KA_LAYER, 3, L_123 }, CH("."), CH(","), CH("?"), CH("!"), CH("'"), CH("\""), CH("°"),
    { LV_SYMBOL_BACKSPACE, KA_KEY, 3, SC_K_BS }, ROW_END,
    BOTTOM_ROW("ABC", L_ABC, "Fn", L_FN),
};

static const skey_t K_FN[] = {
    TOP_ROW,
    { "F1", KA_KEY, 2, SC_K_F1 }, { "F2", KA_KEY, 2, SC_K_F1 + 1 }, { "F3", KA_KEY, 2, SC_K_F1 + 2 },
    { "F4", KA_KEY, 2, SC_K_F1 + 3 }, { "F5", KA_KEY, 2, SC_K_F1 + 4 }, { "F6", KA_KEY, 2, SC_K_F1 + 5 }, ROW_END,
    { "F7", KA_KEY, 2, SC_K_F1 + 6 }, { "F8", KA_KEY, 2, SC_K_F1 + 7 }, { "F9", KA_KEY, 2, SC_K_F1 + 8 },
    { "F10", KA_KEY, 2, SC_K_F1 + 9 }, { "F11", KA_KEY, 2, SC_K_F1 + 10 }, { "F12", KA_KEY, 2, SC_K_F1 + 11 }, ROW_END,
    { "Ins", KA_KEY, 2, SC_K_INS }, { "Home", KA_KEY, 2, SC_K_HOME }, { "PgUp", KA_KEY, 2, SC_K_PGUP },
    { "Del", KA_KEY, 2, SC_K_DEL }, { "End", KA_KEY, 2, SC_K_END }, { "PgDn", KA_KEY, 2, SC_K_PGDN }, ROW_END,
    BOTTOM_ROW("ABC", L_ABC, "123", L_123),
};

static const skey_t *const LAYERS[L_COUNT] = { K_ABC, K_123, K_SYM, K_FN };
static const int LAYER_LEN[L_COUNT] = {
    sizeof K_ABC / sizeof K_ABC[0], sizeof K_123 / sizeof K_123[0],
    sizeof K_SYM / sizeof K_SYM[0], sizeof K_FN / sizeof K_FN[0],
};

#define MAX_KEYS 64
#define ROWS     5

struct sc_kbd {
    lv_obj_t *bm;
    void (*send)(void *, uint32_t, uint8_t);
    void (*on_hide)(void *);
    void *user;
    int layer;
    uint8_t mods;
    int32_t h;
    const char *map[MAX_KEYS + ROWS + 1];
    lv_buttonmatrix_ctrl_t ctrl[MAX_KEYS];
    const skey_t *keys[MAX_KEYS];
    char upper[MAX_KEYS][4];
};

static uint32_t utf8_first(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80) return p[0];
    if ((p[0] & 0xE0) == 0xC0) return (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
    if ((p[0] & 0xF0) == 0xE0) return (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
    return (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 | (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
}

static void build(sc_kbd_t *k)
{
    const skey_t *keys = LAYERS[k->layer];
    int n = LAYER_LEN[k->layer], m = 0, b = 0, row = 0;
    for (int i = 0; i < n; i++) {
        if (!keys[i].label) {
            if (++row < ROWS) k->map[m++] = "\n";
            continue;
        }
        const skey_t *key = &keys[i];
        const char *label = key->label;
        if (key->act == KA_CHAR && (k->mods & SC_M_SHIFT)) {
            uint32_t c = utf8_first(label);
            if (c >= 'a' && c <= 'z') {
                k->upper[b][0] = (char)(c - 32);
                k->upper[b][1] = '\0';
                label = k->upper[b];
            } else if (c == 0xF1) label = "Ñ";
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
    sc_kbd_t *k = lv_event_get_user_data(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(k->bm);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE || id >= MAX_KEYS || !k->keys[id]) return;
    const skey_t *key = k->keys[id];
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
    case KA_KEY:
        k->send(k->user, key->arg, k->mods);
        break;
    case KA_CHAR: {
        uint32_t c = utf8_first(key->label);
        uint8_t mods = k->mods;
        if ((mods & SC_M_SHIFT) && c >= 'a' && c <= 'z') c -= 32;
        else if ((mods & SC_M_SHIFT) && c == 0xF1) c = 0xD1;
        mods &= (uint8_t)~SC_M_SHIFT;       /* the character already is what Shift makes */
        k->send(k->user, c, mods);
        break;
    }
    }
    if (k->mods) {
        k->mods = 0;
        build(k);
    }
}

sc_kbd_t *sc_kbd_create(lv_obj_t *parent, int32_t w, bool land,
                        void (*send)(void *user, uint32_t key, uint8_t mods),
                        void (*on_hide)(void *user), void *user)
{
    sc_kbd_t *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->send = send;
    k->on_hide = on_hide;
    k->user = user;
    k->h = land ? 330 : 440;
    k->bm = lv_buttonmatrix_create(parent);
    aos_keyboard_style(k->bm, land ? aos_font_caption : aos_font_small);
    lv_obj_set_style_pad_all(k->bm, 8, 0);
    lv_obj_set_style_pad_gap(k->bm, land ? 7 : 9, 0);
    lv_obj_set_style_bg_color(k->bm, AOS_C_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_size(k->bm, w, k->h);
    lv_obj_align(k->bm, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(k->bm, key_cb, LV_EVENT_VALUE_CHANGED, k);
    build(k);
    return k;
}

int32_t sc_kbd_height(const sc_kbd_t *k) { return k ? k->h : 0; }

void sc_kbd_delete(sc_kbd_t *k)
{
    if (!k) return;
    if (k->bm) lv_obj_delete(k->bm);
    free(k);
}

/* ---- the USB keyboard ---- */

bool sc_hwkey(uint32_t key, uint8_t hid, uint32_t *out_key, uint8_t *out_mods)
{
    if (key >= AOS_KEY_CONSUMER) return false;      /* media keys: the system's */
    uint8_t mods = 0;
    if (hid & 0x22) mods |= SC_M_SHIFT;
    if (hid & 0x11) mods |= SC_M_CTRL;
    if (hid & 0x44) mods |= SC_M_ALT;
    uint32_t k;
    switch (key) {
    case AOS_KEY_UP:        k = SC_K_UP; break;
    case AOS_KEY_DOWN:      k = SC_K_DOWN; break;
    case AOS_KEY_LEFT:      k = SC_K_LEFT; break;
    case AOS_KEY_RIGHT:     k = SC_K_RIGHT; break;
    case AOS_KEY_ESC:       k = SC_K_ESC; break;
    case AOS_KEY_DEL:       k = SC_K_DEL; break;
    case AOS_KEY_BACKSPACE: k = SC_K_BS; break;
    case AOS_KEY_ENTER:     k = SC_K_ENTER; break;
    case AOS_KEY_NEXT:      k = SC_K_TAB; break;
    case AOS_KEY_PREV:      k = SC_K_BTAB; break;
    case AOS_KEY_HOME:      k = SC_K_HOME; break;
    case AOS_KEY_END:       k = SC_K_END; break;
    default:
        if (key < 0x20) return false;
        k = key;
        /* the character is composed already (Shift, AltGr); Ctrl and Alt are
         * for the terminal: AltGr comes as Alt, so only Ctrl or a plain
         * letter keeps Alt */
        if (!(mods & SC_M_CTRL) && !(key < 0x80 && (mods & SC_M_ALT) && !(hid & 0x40))) mods = 0;
        mods &= (uint8_t)~SC_M_SHIFT;
        break;
    }
    *out_key = k;
    *out_mods = mods;
    return true;
}
