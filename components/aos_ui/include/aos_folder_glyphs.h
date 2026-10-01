/*
 * P4OS - the folder glyphs of menu.txt (a curated set of Material Design
 * Icons, tools/gen_folder_glyphs.py). On the watch they sat inside a hexagon;
 * here they are the glyph a folder shows while it has no apps to preview,
 * and the same names still work in menu.txt.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;           /* MDI's own name, what menu.txt stores */
    uint32_t    codepoint;      /* in aos_folder_font */
} aos_folder_glyph_t;

extern const aos_folder_glyph_t aos_folder_glyphs[];
extern const int                aos_folder_glyph_count;

LV_FONT_DECLARE(aos_folder_font);   /* 42 px */

/* The glyph's code point by name; the catalogue's first ("folder") if the
 * name is not in it. */
uint32_t aos_folder_glyph_codepoint(const char *name);

#ifdef __cplusplus
}
#endif
