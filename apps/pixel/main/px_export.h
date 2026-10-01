/*
 * PIXEL ART - exporting to files other programs open.
 *
 * Two encoders written for the occasion, because the firmware has neither:
 * lodepng is compiled as a decoder only (for the photo viewer) and there is
 * no GIF library at all. Both are small enough that bringing a library in
 * would have cost more code than this, and code -not data- is the scarce
 * resource of a dynamic app.
 *
 *   PNG  one frame, indexed colour (PLTE of 32 entries), the image data in
 *        STORED deflate blocks. No compression: a 512x512 frame is 257 KB
 *        and any viewer opens it. What matters is that the pixels are exact.
 *   GIF  every frame, looping, one global colour table, LZW-compressed for
 *        real (the scaled-up pixels are long runs and compress ten to one).
 *
 * Both take a 'scale' (1..32): a 64x64 drawing at scale 8 comes out
 * 512x512, which is what a phone or a chat shows at a sensible size without
 * blurring it.
 * No LVGL, no HAL: verified on the Mac by tools/px_harness.c.
 */
#pragma once

#include "px_file.h"

bool px_export_png(const px_doc_t *d, int frame, int scale, const char *path);
bool px_export_gif(const px_doc_t *d, int scale, const char *path);
