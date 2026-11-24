/*
 * PLANEO - words and numbers inside the frame.
 *
 * The frame is small (426 x 240 by default) and the panel shows it three
 * times larger, so the words are drawn into it at its own size. LVGL
 * renders each word once, in the language of the moment, into an alpha
 * mask (vt_build, LVGL's thread); the worker draws the masks with a dark
 * edge (vt_draw). Numbers are drawn glyph by glyph from masks of
 * "0123456789:.,/+-" made the same way.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { VT_S = 0, VT_M, VT_L, VT_SIZES };

/* the words, each in every size */
enum {
    VT_W_RINGS = 0, VT_W_RACE, VT_W_DELIVER, VT_W_FREE,
    VT_W_GO, VT_W_FINISH, VT_W_WIN, VT_W_LOST, VT_W_PICK, VT_W_DROP,
    VT_W_STALL, VT_W_CRASH, VT_W_RECORD, VT_W_KMH, VT_W_M, VT_W_FIRST, VT_W_SECOND,
    VT_W_LOW, VT_W_TOFIND, VT_W_TODROP, VT_W_LAST,
    VT_WORDS
};

#define VT_DIGITS "0123456789:.,/+-"
#define VT_NDIG   16

typedef struct {
    uint8_t *a;
    int16_t  w, h;
} vt_mask_t;

typedef struct {
    vt_mask_t word[VT_SIZES][VT_WORDS];
    vt_mask_t dig[VT_SIZES][VT_NDIG];
    int       frame_h;            /* the frame height they were made for */
    bool      ok;
} vt_set_t;

/* LVGL's thread: every word for frames of height H (the sizes follow) */
bool vt_build(vt_set_t *t, int H);
void vt_free(vt_set_t *t);

enum { VT_LEFT = 0, VT_CENTER, VT_RIGHT };

/* A mask at (x, y) (its top; x as align says), colour 565, with a dark
 * edge; returns its width */
int vt_draw(uint16_t *out, int W, int H, const vt_mask_t *m, int x, int y, uint16_t col, int align);
/* A number or a time ("1:23.4", "120"): only VT_DIGITS */
int vt_num(uint16_t *out, int W, int H, const vt_set_t *t, int size, const char *s, int x, int y, uint16_t col,
           int align);
int vt_num_w(const vt_set_t *t, int size, const char *s);
