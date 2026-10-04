/*
 * P4OS - colour emoji in any text (aos_emoji.c).
 *
 * The card's /fonts/emoji.pak (tools/gen_emoji.py, Noto's images) holds every
 * emoji, sequences included. aos_text_safe() turns each one it finds in a
 * text into a single private-use code point, and the theme's fonts fall back
 * to an emoji font of their own size that draws that code point as the
 * image. Without the pack, none of this is on and the text works as before.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the pack; false without a card or without the file. */
bool aos_emoji_init(void);
bool aos_emoji_ready(void);

/* The longest emoji that starts at s (UTF-8, len bytes): the bytes it spans,
 * a variation selector after it included, or 0. *cp gets the private-use
 * code point that stands for it in the text. */
size_t aos_emoji_match(const char *s, size_t len, uint32_t *cp);

/* A copy of 'font' (in PSRAM) whose fallback draws the emoji at its size;
 * 'font' itself when the pack is not there. */
const lv_font_t *aos_emoji_font(const lv_font_t *font);

#ifdef __cplusplus
}
#endif
