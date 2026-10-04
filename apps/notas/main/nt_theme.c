/*
 * P4OS - Notes: the two looks, dark (the default, the system's) and light.
 *
 * Every colour a note can carry has a shade for each: a yellow letter that
 * reads on black is unreadable on white, and a highlighter that works under
 * black text drowns white text. The files keep the index, not the shade, so
 * a note written in the dark reads right in the light.
 */
#include "nt.h"
#include "aos_i18n.h"

#include <string.h>
#include <strings.h>

nt_theme_t NT;

/* index 0 is the theme's own text colour */
static const uint32_t FG_DARK[NT_FG_COUNT] = {
    0xFFFFFF, 0xFF453A, 0xFF9F0A, 0xFFD60A, 0x30D158, 0x40C8E0, 0x409CFF, 0xBF5AF2, 0xFF6482, 0x98989D };
static const uint32_t FG_LIGHT[NT_FG_COUNT] = {
    0x1C1C1E, 0xD70015, 0xC93400, 0xA05A00, 0x248A3D, 0x0071A4, 0x0040DD, 0x8944AB, 0xD30F45, 0x6C6C70 };

/* index 0 is no highlight */
static const uint32_t HL_DARK[NT_HL_COUNT] = {
    0x000000, 0x7A6400, 0x1E6B34, 0x1D4E89, 0x8A2347, 0x8A4A00, 0x5E3480 };
static const uint32_t HL_LIGHT[NT_HL_COUNT] = {
    0xFFFFFF, 0xFFE866, 0xB8F0C2, 0xBFDDFF, 0xFFC2D4, 0xFFD3A1, 0xE2C9F5 };

static const uint32_t TAG[NT_TAG_COUNT] = {
    0x8E8E93, 0xFF453A, 0xFF9F0A, 0xFFD60A, 0x30D158, 0x40C8E0, 0x0A84FF, 0xBF5AF2, 0xFF375F };
static const char *const TAG_NAME[NT_TAG_COUNT] = {
    "", "red", "orange", "yellow", "green", "teal", "blue", "purple", "pink" };

void nt_theme_set(bool dark)
{
    NT.dark = dark;
    if (dark) {
        NT.bg       = lv_color_hex(0x000000);
        NT.surface  = lv_color_hex(0x1C1C1E);
        NT.surface2 = lv_color_hex(0x2C2C2E);
        NT.text     = lv_color_hex(0xFFFFFF);
        NT.dim      = lv_color_hex(0x8E8E93);
        NT.hair     = lv_color_hex(0x38383A);
        NT.accent   = lv_color_hex(0xFFD60A);       /* the notepad's yellow */
        NT.sel      = lv_color_hex(0x0A84FF);
        NT.danger   = lv_color_hex(0xFF453A);
        NT.kb_bg    = lv_color_hex(0x1C1C1E);
        NT.kb_key   = lv_color_hex(0x4A4A4E);
        NT.kb_key2  = lv_color_hex(0x2C2C2E);
        NT.kb_text  = lv_color_hex(0xFFFFFF);
    } else {
        NT.bg       = lv_color_hex(0xF2F2F7);
        NT.surface  = lv_color_hex(0xFFFFFF);
        NT.surface2 = lv_color_hex(0xE5E5EA);
        NT.text     = lv_color_hex(0x1C1C1E);
        NT.dim      = lv_color_hex(0x6C6C70);
        NT.hair     = lv_color_hex(0xD1D1D6);
        NT.accent   = lv_color_hex(0xC98A00);
        NT.sel      = lv_color_hex(0x007AFF);
        NT.danger   = lv_color_hex(0xD70015);
        NT.kb_bg    = lv_color_hex(0xD1D3D9);
        NT.kb_key   = lv_color_hex(0xFFFFFF);
        NT.kb_key2  = lv_color_hex(0xADB1BB);
        NT.kb_text  = lv_color_hex(0x000000);
    }
}

lv_color_t nt_fg_color(int i)
{
    if (i <= 0 || i >= NT_FG_COUNT) return NT.text;
    return lv_color_hex(NT.dark ? FG_DARK[i] : FG_LIGHT[i]);
}

lv_color_t nt_hl_color(int i)
{
    if (i <= 0 || i >= NT_HL_COUNT) return NT.surface;
    return lv_color_hex(NT.dark ? HL_DARK[i] : HL_LIGHT[i]);
}

lv_color_t nt_tag_color(int i)
{
    if (i < 0 || i >= NT_TAG_COUNT) i = 0;
    return lv_color_hex(TAG[i]);
}

lv_color_t nt_tag_card(int i)
{
    if (i <= 0 || i >= NT_TAG_COUNT) return NT.surface;
    return lv_color_mix(lv_color_hex(TAG[i]), NT.surface, NT.dark ? 70 : 60);
}

const char *nt_tag_name(int i)
{
    return (i > 0 && i < NT_TAG_COUNT) ? TAG_NAME[i] : "";
}

int nt_tag_from_name(const char *s)
{
    for (int i = 1; i < NT_TAG_COUNT; i++) if (strcasecmp(s, TAG_NAME[i]) == 0) return i;
    return 0;
}

lv_color_t nt_prio_color(int p)
{
    static const uint32_t c[4] = { 0x8E8E93, 0x0A84FF, 0xFF9F0A, 0xFF453A };
    return lv_color_hex(c[p & 3]);
}
