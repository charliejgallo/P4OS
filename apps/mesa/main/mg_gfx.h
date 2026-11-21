/*
 * Juegos de mesa - the picture: Bastion's compositor (bs_gfx.c), with the
 * screen's size chosen at run time (upright or lying down) and without the
 * world's depth and the fonts, which a board does not need.
 *
 * A frame is a list of things to draw, in order (mg_spr, mg_shadow,
 * mg_ring...), over the table's picture. Nobody says what changed: the
 * screen is cut in 16 x 16 tiles and each tile's list of things is hashed;
 * a tile whose hash is the one the target already shows is left alone, the
 * rest get the table's pixels back and their things drawn again, clipped
 * to them. A still board costs nothing, and a piece in the air costs the
 * tiles it covers.
 *
 * Here the target is the buffer of an LVGL canvas that fills the screen:
 * LVGL turns it with the screen and draws the buttons over it. mg_gfx_end()
 * says which rectangle changed (dirty), which the app invalidates.
 *
 * Sprites come as sheets of runs (tools/pack_p4.py): per row, skips, runs
 * of opaque pixels (copied) and runs of edge pixels with their alpha
 * (blended), so the transparent part of a cell is neither stored nor
 * visited:
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

#define MG_TILE      16
#define MG_MAX_W     1280
#define MG_MAX_TILES ((720 / MG_TILE) * (1280 / MG_TILE))
#define MG_MAX_TH    (1280 / MG_TILE)
#define MG_MAX_ITEMS 1200

enum {
    MG_F_FLASH = 2,     /* whitened                                          */
    MG_F_ADD   = 4,     /* light added instead of blended (rings)            */
};

typedef struct {
    int16_t  ox, oy;
    uint16_t w, h;
    uint32_t off;
} mg_frame_t;

typedef struct {
    uint8_t          *data;     /* the block, as the pack has it            */
    const mg_frame_t *fr;
    int               nframes, cols;
} mg_sheet_t;

typedef struct {
    int16_t x0, y0, x1, y1;
} mg_rect_t;

typedef struct {
    uint8_t  kind, flags, a, aux;
    int16_t  x0, y0, x1, y1;        /* what it covers, inside the screen   */
    int16_t  x, y;
    int16_t  p0, p1, p2, p3;
    uint16_t color, z;
    const void *p;
    uint32_t hash;
} mg_item_t;

typedef struct {
    int        w, h, tw, th, nt;    /* the screen, in pixels and tiles      */
    const uint16_t *bg;             /* w x h: the table under everything    */

    mg_item_t *item;
    int        nitem;
    bool       overflow;

    uint32_t  *cur;                 /* this frame's hash of every tile      */
    uint8_t   *need;                /* tiles to draw into the target        */
    uint16_t  *band_ix;             /* the items of each row of tiles       */
    int        band_cap;
    int        band_at[MG_MAX_TH + 1];
    uint8_t    glow[64 * 64];
    uint8_t    prof[17];

    int        drawn_tiles;
    mg_rect_t  dirty;               /* what the last mg_gfx_end changed (x1 <= x0: none) */
} mg_gfx_t;

static inline uint16_t mg_rgb(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

bool mg_gfx_init(mg_gfx_t *g);
void mg_gfx_free(mg_gfx_t *g);
/* the screen's size and the table's picture (w x h, or NULL: black) */
void mg_gfx_size(mg_gfx_t *g, int w, int h, const uint16_t *bg);

bool mg_sheet_bind(mg_sheet_t *s, uint8_t *data, size_t len);

void mg_gfx_begin(mg_gfx_t *g);
/* a sheet's frame with its anchor on (x, y); alpha 255 is solid */
void mg_spr(mg_gfx_t *g, const mg_sheet_t *s, int frame, int x, int y, int flags, int alpha);
void mg_glow(mg_gfx_t *g, int x, int y, int r, uint16_t color, int k);
void mg_shadow(mg_gfx_t *g, int x, int y, int rx, int ry, int a);
void mg_rect(mg_gfx_t *g, int x, int y, int w, int h, uint16_t color, int a, int radius);
void mg_beam(mg_gfx_t *g, int x0, int y0, int x1, int y1, int halfw, uint16_t color, int k);
void mg_ring(mg_gfx_t *g, int cx, int cy, int rx, int ry, int thick, uint16_t color, int k, int flags);
void mg_disc(mg_gfx_t *g, int cx, int cy, int rx, int ry, uint16_t color, int a);
/* a quadrilateral (a cell seen in perspective), filled: corners in order */
void mg_quad(mg_gfx_t *g, const int16_t *xy, uint16_t color, int a);
/* Draws the frame into fb, whose tiles' hashes are in 'shown' (nt of
 * them, updated); full: fb's picture is unknown, draw it all. Returns the
 * tiles drawn. */
int  mg_gfx_end(mg_gfx_t *g, uint16_t *fb, uint32_t *shown, bool full);
