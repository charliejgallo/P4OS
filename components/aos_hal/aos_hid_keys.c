/*
 * P4OS - key names to USB HID usages. See aos_hid_keys.h.
 *
 * The numbers are the USB HID Usage Tables' (keyboard page 0x07, consumer
 * page 0x0C), written out here so that this file needs no TinyUSB header
 * and compiles in the simulator as it is.
 */
#include "aos_hid_keys.h"

#include <ctype.h>
#include <string.h>

typedef struct { const char *name; uint16_t code; bool consumer; } named_t;

static const named_t NAMED[] = {
    { "enter", 0x28, false }, { "return", 0x28, false },
    { "esc", 0x29, false }, { "escape", 0x29, false },
    { "backspace", 0x2A, false }, { "delete", 0x2A, false }, { "del", 0x2A, false },
    { "tab", 0x2B, false }, { "space", 0x2C, false },
    { "minus", 0x2D, false }, { "equal", 0x2E, false },
    { "bracketleft", 0x2F, false }, { "bracketright", 0x30, false },
    { "backslash", 0x31, false }, { "semicolon", 0x33, false }, { "quote", 0x34, false },
    { "grave", 0x35, false }, { "comma", 0x36, false }, { "period", 0x37, false },
    { "dot", 0x37, false }, { "slash", 0x38, false },
    { "capslock", 0x39, false },
    { "printscreen", 0x46, false }, { "scrolllock", 0x47, false }, { "pausebreak", 0x48, false },
    { "insert", 0x49, false }, { "home", 0x4A, false }, { "pgup", 0x4B, false }, { "pageup", 0x4B, false },
    { "fwddelete", 0x4C, false }, { "forwarddelete", 0x4C, false },
    { "end", 0x4D, false }, { "pgdn", 0x4E, false }, { "pagedown", 0x4E, false },
    { "right", 0x4F, false }, { "left", 0x50, false }, { "down", 0x51, false }, { "up", 0x52, false },
    { "numlock", 0x53, false }, { "menu", 0x65, false }, { "application", 0x65, false },
    /* media */
    { "play", 0xCD, true }, { "pause", 0xCD, true }, { "playpause", 0xCD, true },
    { "next", 0xB5, true }, { "prev", 0xB6, true }, { "previous", 0xB6, true },
    { "stop", 0xB7, true }, { "eject", 0xB8, true },
    { "mute", 0xE2, true }, { "volup", 0xE9, true }, { "voldown", 0xEA, true },
    { "brightup", 0x6F, true }, { "brightdown", 0x70, true },
};

bool aos_hid_ascii(char ch, uint8_t *key, bool *shift)
{
    /* the punctuation of a US keyboard: unshifted, shifted, usage */
    static const char PUNCT[][3] = {
        { '-', '_', 0x2D }, { '=', '+', 0x2E }, { '[', '{', 0x2F }, { ']', '}', 0x30 },
        { '\\', '|', 0x31 }, { ';', ':', 0x33 }, { '\'', '"', 0x34 }, { '`', '~', 0x35 },
        { ',', '<', 0x36 }, { '.', '>', 0x37 }, { '/', '?', 0x38 },
    };
    static const char SHIFTED_DIGITS[] = ")!@#$%^&*(";     /* shift + 0..9 */
    unsigned char c = (unsigned char)ch;
    *shift = false;
    *key = 0;
    if (c >= 'a' && c <= 'z') { *key = (uint8_t)(0x04 + c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *key = (uint8_t)(0x04 + c - 'A'); *shift = true; return true; }
    if (c >= '1' && c <= '9') { *key = (uint8_t)(0x1E + c - '1'); return true; }
    if (c == '0') { *key = 0x27; return true; }
    if (c == ' ') { *key = 0x2C; return true; }
    if (c == '\n') { *key = 0x28; return true; }
    if (c == '\t') { *key = 0x2B; return true; }
    for (int i = 0; i < 10; i++)
        if (c == (unsigned char)SHIFTED_DIGITS[i]) { *key = i == 0 ? 0x27 : (uint8_t)(0x1E + i - 1); *shift = true; return true; }
    for (size_t i = 0; i < sizeof PUNCT / sizeof PUNCT[0]; i++) {
        if (c == (unsigned char)PUNCT[i][0]) { *key = (uint8_t)PUNCT[i][2]; return true; }
        if (c == (unsigned char)PUNCT[i][1]) { *key = (uint8_t)PUNCT[i][2]; *shift = true; return true; }
    }
    return false;
}

static int modifier(const char *t)
{
    static const struct { const char *n; uint8_t m; } M[] = {
        { "cmd", AOS_HID_MOD_GUI }, { "command", AOS_HID_MOD_GUI }, { "gui", AOS_HID_MOD_GUI },
        { "win", AOS_HID_MOD_GUI }, { "super", AOS_HID_MOD_GUI }, { "meta", AOS_HID_MOD_GUI },
        { "ctrl", AOS_HID_MOD_CTRL }, { "control", AOS_HID_MOD_CTRL },
        { "alt", AOS_HID_MOD_ALT }, { "opt", AOS_HID_MOD_ALT }, { "option", AOS_HID_MOD_ALT },
        { "shift", AOS_HID_MOD_SHIFT },
    };
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) if (!strcmp(t, M[i].n)) return M[i].m;
    return -1;
}

/* The last token: the key itself. */
static bool key_token(const char *t, const char *orig, aos_hid_combo_t *out)
{
    size_t n = strlen(t);
    if (n == 1) {
        uint8_t k;
        bool sh;
        /* the original case: "cmd+A" is cmd+shift+a, as typing it would be */
        if (!aos_hid_ascii(orig[0], &k, &sh)) return false;
        out->key = k;
        if (sh) out->mods |= AOS_HID_MOD_SHIFT;
        return true;
    }
    if (!strcmp(t, "plus")) { out->key = 0x2E; out->mods |= AOS_HID_MOD_SHIFT; return true; }
    if (t[0] == 'f' && isdigit((unsigned char)t[1])) {
        int f = 0;
        for (const char *p = t + 1; *p; p++) {
            if (!isdigit((unsigned char)*p)) return false;
            f = f * 10 + (*p - '0');
        }
        if (f >= 1 && f <= 12) { out->key = (uint8_t)(0x3A + f - 1); return true; }
        if (f >= 13 && f <= 24) { out->key = (uint8_t)(0x68 + f - 13); return true; }
        return false;
    }
    for (size_t i = 0; i < sizeof NAMED / sizeof NAMED[0]; i++) {
        if (strcmp(t, NAMED[i].name)) continue;
        if (NAMED[i].consumer) {
            if (out->mods) return false;           /* "cmd+play" means nothing */
            out->consumer = NAMED[i].code;
        } else {
            out->key = (uint8_t)NAMED[i].code;
        }
        return true;
    }
    return false;
}

bool aos_hid_parse(const char *name, aos_hid_combo_t *out)
{
    memset(out, 0, sizeof *out);
    if (!name) return false;
    while (*name == ' ') name++;
    size_t n = strlen(name);
    while (n && name[n - 1] == ' ') n--;
    const char *p = name, *end = name + n;
    if (!n) return false;
    for (;;) {
        /* one token up to the next '+'; a '+' that starts a token is the
         * key itself ("cmd++", "+") */
        const char *s = p;
        if (p < end && *p == '+') p++;
        while (p < end && *p != '+') p++;
        size_t len = (size_t)(p - s);
        bool last = p >= end;
        char tok[32], otok[32];
        if (!len || len >= sizeof tok) break;
        for (size_t i = 0; i < len; i++) {
            otok[i] = s[i];
            tok[i] = (char)tolower((unsigned char)s[i]);
        }
        tok[len] = otok[len] = 0;
        int m = modifier(tok);
        if (last) {
            if (m >= 0) { out->mods |= (uint8_t)m; return true; }   /* modifiers alone */
            if (key_token(tok, otok, out)) return true;
            break;
        }
        if (m < 0) break;                   /* a key before the end */
        out->mods |= (uint8_t)m;
        p++;                                /* the '+' */
    }
    memset(out, 0, sizeof *out);
    return false;
}
