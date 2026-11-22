/*
 * HOCKEY - the picture (Metegol's compositor, which is Bastion's, upright)
 *
 * A frame is a list of things to draw, in order (hk_spr, hk_text, hk_ring
 * ...), over the table's picture. Nobody says what changed: the screen
 * (720 x 1280, upright as the panel is) is cut in 16 x 16 tiles and each
 * tile's list of things is hashed; a tile whose hash is the one the target
 * already shows is left alone, the rest get the table's pixels back and
 * their things drawn again, clipped to them, half the rows of tiles on
 * each core. Each of the panel's three buffers remembers its own hashes.
 * The target is the panel's free buffer itself, or the copy LVGL shows
 * under its panels: both are laid out as the screen is.
 *
 * Sprites come as sheets of runs, as in Bastion (bs_gfx.h): per row,
 * skips, runs of opaque pixels (copied) and runs of edge pixels with
 * their alpha (blended).
 *
 *   u32 'BSSH'  u16 frames  u16 cols
 *   frames x { i16 ox, oy;  u16 w, h;  u32 off }     ox, oy from the anchor
 *   at off: u32 row[h] (offsets from the sheet's start), then the rows:
 *   u16 ops, (kind << 14) | count: 0 end of row, 1 skip, 2 count opaque
 *   pixels follow, 3 count (pixel, alpha) pairs follow
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HK_W        720
#define HK_H        1280
#define HK_TILE     16
#define HK_TW       (HK_W / HK_TILE)
#define HK_TH       (HK_H / HK_TILE)
#define HK_NT       (HK_TW * HK_TH)
#define HK_MAX_ITEMS 700
#define HK_TEXT_ARENA 4096

enum {
    HK_F_FLASH  = 2,    /* whitened                                         */
    HK_F_ADD    = 4,    /* light added instead of blended                   */
    HK_F_SHADOW = 8,    /* text: a dark copy under it                       */
    HK_F_CENTER = 16,   /* text: x is its middle                            */
    HK_F_RIGHT  = 32,   /* text: x is its right end                         */
    HK_F_ROT    = 64,   /* text: turned half a turn, for the far player     */
};

typedef struct {
    int16_t  ox, oy;
    uint16_t w, h;
    uint32_t off;
} hk_frame_t;

typedef struct {
    uint8_t          *data;
    const hk_frame_t *fr;
    int               nframes, cols;
    uint32_t          id;       /* changes when the sheet's pixels do (hashing) */
} hk_sheet_t;

typedef struct {
    uint32_t cp;
    uint16_t x, y;
    uint8_t  w, h;
    int8_t   ox, oy;
    uint8_t  adv, pad;
    uint16_t spare;
} hk_glyph_t;

typedef struct {
    uint8_t          *data;
    const hk_glyph_t *g;
    const uint8_t    *atlas;
    int               n, line_h, base, aw, ah;
    int16_t           idx[256];
} hk_font_t;

typedef struct {
    int16_t x0, y0, x1, y1;
} hk_rect_t;

typedef struct {
    uint8_t  kind, flags, a, aux;
    int16_t  x0, y0, x1, y1;        /* what it covers, inside the screen   */
    int16_t  x, y;
    int16_t  p0, p1, p2, p3;
    uint16_t color, spare;
    const void *p;
    uint32_t hash;
} hk_item_t;

typedef struct {
    const uint16_t *bg;             /* HK_W x HK_H, or NULL (black)         */
    int        bg_dx, bg_dy;        /* the picture shaken (a goal)          */
    uint32_t   salt;                /* mixed into every tile's hash: a shake */

    hk_item_t *item;
    int        nitem;
    bool       overflow;
    char      *text;
    int        ntext;

    uint32_t  *cur;                 /* this frame's hash of every tile      */
    uint8_t   *need;
    uint16_t  *band_ix;
    int        band_cap;
    int        band_at[HK_TH + 1];
    uint8_t    glow[64 * 64];

    int        drawn_tiles;

    /* one frame's job, for the two halves */
    uint16_t  *fb;
    int        split;
    bool       one_core;
} hk_gfx_t;

static inline uint16_t hk_rgb(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

bool hk_gfx_init(hk_gfx_t *g);
void hk_gfx_free(hk_gfx_t *g);
void hk_gfx_world(hk_gfx_t *g, const uint16_t *bg);

bool hk_sheet_bind(hk_sheet_t *s, uint8_t *data, size_t len);
bool hk_font_bind(hk_font_t *f, uint8_t *data, size_t len);
int  hk_text_width(const hk_font_t *f, const char *s);

/* a sheet in runs from frames of RGBA (as the pack's encoder does it): 'px'
 * is w*h RGB565 and 'al' w*h alpha per frame; returns a new block (hk_free) */
typedef struct {
    const uint16_t *px;
    const uint8_t  *al;
    int w, h, ox, oy;
} hk_rgba_frame_t;
uint8_t *hk_sheet_encode(const hk_rgba_frame_t *fr, int n, int cols, size_t *len);

void hk_gfx_begin(hk_gfx_t *g);
void hk_spr(hk_gfx_t *g, const hk_sheet_t *s, int frame, int x, int y, int flags, int alpha);
void hk_glow(hk_gfx_t *g, int x, int y, int r, uint16_t color, int k);
void hk_shadow(hk_gfx_t *g, int x, int y, int rx, int ry, int a);
void hk_rect(hk_gfx_t *g, int x, int y, int w, int h, uint16_t color, int a, int radius);
int  hk_text(hk_gfx_t *g, const hk_font_t *f, int x, int y, const char *s, uint16_t color, int flags);
void hk_ring(hk_gfx_t *g, int cx, int cy, int rx, int ry, int thick, uint16_t color, int k, int flags);
void hk_disc(hk_gfx_t *g, int cx, int cy, int rx, int ry, uint16_t color, int a);
/* Draws the frame into fb, whose tiles' hashes are in 'shown' (HK_NT of
 * them, updated); full: draw it all. Returns the tiles drawn. */
int  hk_gfx_end(hk_gfx_t *g, uint16_t *fb, uint32_t *shown, bool full);
