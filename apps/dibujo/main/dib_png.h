/*
 * DIBUJO - a PNG writer for true colour with alpha.
 *
 * Pixel Art's encoder is indexed and stores its data uncompressed: fine for
 * 32 colours and 512 px, not for a 1280x1024 drawing with soft brushes,
 * which would be 5 MB. This one writes RGBA8888 with a filter chosen per row
 * (none, sub, up or Paeth: the one with the smallest sum, the usual rule)
 * and compresses it for real: LZ77 over a 32 KB window with hash chains,
 * coded with deflate's fixed Huffman tables. A drawing - flat areas, long
 * runs - comes out a fraction of its raw size; the fixed tables cost a few
 * percent against zlib's dynamic ones and spare their code.
 *
 * Rows are asked for one at a time, so the picture is never all in memory.
 * No LVGL, no HAL: checked on the Mac by tools/dib_harness.c against
 * Python's zlib.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Fills one row of w RGBA pixels (4 bytes each). false aborts. */
typedef bool (*dib_png_row_fn)(void *user, int y, uint8_t *rgba);

/* progress, if not NULL, goes 0..1000 as rows are written. */
bool dib_png_write(const char *path, int w, int h, dib_png_row_fn rows, void *user,
                   volatile int *progress);

uint32_t dib_crc32(uint32_t crc, const uint8_t *p, size_t n);
