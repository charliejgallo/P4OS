/*
 * BASTION - the picture
 *
 * A frame is a list of things to draw, in order (bs_spr, bs_glow, bs_text
 * ...), over the world's picture. Nobody says what changed: the screen is
 * cut in 16 x 16 tiles and each tile's list of things is hashed; a tile
 * whose hash is the one the buffer already shows is left alone, the rest
 * get the world's pixels back and their things drawn again, clipped to
 * them. Each of the panel's three buffers remembers its own hashes, so
 * page flipping needs no history of what moved: a buffer two frames old
 * simply differs in more tiles.
 *
 * A still screen costs nothing, a tower's translucent range is blended
 * once, and a hundred aliens cost what they cover. The tiles of a frame
 * are drawn in two halves, one on each core (aos_hal_worker_split).
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
 *
 * A sprite with BS_F_DEPTH is drawn behind what the world has in front of
 * it: the world's picture comes with the depth of its pixels, and the
 * sprite is a board standing on its foot.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BS_W        720
#define BS_H        1280
#define BS_TILE     16
#define BS_TW       (BS_W / BS_TILE)
#define BS_TH       (BS_H / BS_TILE)
#define BS_NT       (BS_TW * BS_TH)
#define BS_MAX_ITEMS 2600
#define BS_TEXT_ARENA 4096

/* metres to depth units, as the pack stores the world's depth */
#define BS_Z_UNIT   1024.0f
#define BS_Z_OFF    16.0f

enum {
    BS_F_DEPTH = 1,     /* hidden by what the world has in front           */
    BS_F_FLASH = 2,     /* whitened (a hit)                                 */
    BS_F_ADD   = 4,     /* light added instead of blended (rings)           */
    BS_F_SHADOW = 8,    /* text: a dark copy under it                       */
    BS_F_CENTER = 16,   /* text: x is its middle                            */
    BS_F_RIGHT = 32,    /* text: x is its right end                         */
};

typedef struct {
    int16_t  ox, oy;
    uint16_t w, h;
    uint32_t off;
} bs_frame_t;

typedef struct {
    uint8_t          *data;     /* the block, as the pack has it            */
    const bs_frame_t *fr;
    int               nframes, cols;
} bs_sheet_t;

typedef struct {
    uint32_t cp;
    uint16_t x, y;
    uint8_t  w, h;
    int8_t   ox, oy;
    uint8_t  adv, pad;
    uint16_t spare;             /* 16 bytes, as the pack writes them */
} bs_glyph_t;

typedef struct {
    uint8_t          *data;
    const bs_glyph_t *g;
    const uint8_t    *atlas;
    int               n, line_h, base, aw, ah;
    int16_t           idx[256];
} bs_font_t;

typedef struct {
    int16_t x0, y0, x1, y1;
} bs_rect_t;

typedef struct {
    uint8_t  kind, flags, a, aux;
    int16_t  x0, y0, x1, y1;        /* what it covers, inside the screen   */
    int16_t  x, y;
    int16_t  p0, p1, p2, p3;
    uint16_t color, z;
    int16_t  foot;
    const void *p;
    uint32_t hash;
} bs_item_t;

typedef struct {
    /* the world under everything */
    const uint16_t *bg;             /* BS_W x BS_H                          */
    const uint16_t *depth;          /* BS_W x BS_H, or NULL                 */
    uint16_t       *zmin;           /* per tile: the nearest thing in it    */

    bs_item_t *item;
    int        nitem;
    bool       overflow;
    char      *text;
    int        ntext;

    uint32_t  *cur;                 /* this frame's hash of every tile      */
    uint8_t   *need;                /* tiles to draw into the target        */
    uint16_t  *band_ix;             /* the items of each row of tiles       */
    int        band_cap;
    int        band_at[BS_TH + 1];
    uint8_t    glow[64 * 64];
    uint8_t    prof[17];

    /* what the last bs_gfx_end drew */
    int        drawn_tiles;
    bs_rect_t  dirty;               /* its bounding box (x1 <= x0: none)   */

    /* one frame's job, for the two halves */
    uint16_t  *fb;
    int        split;
    bool       one_core;            /* the caller is LVGL's thread: no second half */
} bs_gfx_t;

static inline uint16_t bs_rgb(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

bool bs_gfx_init(bs_gfx_t *g);
void bs_gfx_free(bs_gfx_t *g);
/* the world's picture and depth (the depth may be NULL: no hiding) */
void bs_gfx_world(bs_gfx_t *g, const uint16_t *bg, const uint16_t *depth);

bool bs_sheet_bind(bs_sheet_t *s, uint8_t *data, size_t len);
/* turns the hue of every pixel of the sheet (degrees) */
void bs_sheet_hue(bs_sheet_t *s, int degrees);
bool bs_font_bind(bs_font_t *f, uint8_t *data, size_t len);
int  bs_text_width(const bs_font_t *f, const char *s);

void bs_gfx_begin(bs_gfx_t *g);
/* a sheet's frame with its anchor on (x, y). alpha 255 is solid. With
 * BS_F_DEPTH, z is the depth of the ground it stands on and foot that
 * ground's row (a flier's foot is below the sprite) */
void bs_spr(bs_gfx_t *g, const bs_sheet_t *s, int frame, int x, int y, int flags, int alpha, uint16_t z, int foot);
void bs_glow(bs_gfx_t *g, int x, int y, int r, uint16_t color, int k);
void bs_shadow(bs_gfx_t *g, int x, int y, int rx, int ry, int a);
void bs_rect(bs_gfx_t *g, int x, int y, int w, int h, uint16_t color, int a, int radius);
int  bs_text(bs_gfx_t *g, const bs_font_t *f, int x, int y, const char *s, uint16_t color, int flags);
void bs_beam(bs_gfx_t *g, int x0, int y0, int x1, int y1, int halfw, uint16_t color, int k);
void bs_ring(bs_gfx_t *g, int cx, int cy, int rx, int ry, int thick, uint16_t color, int k, int flags);
void bs_disc(bs_gfx_t *g, int cx, int cy, int rx, int ry, uint16_t color, int a);
/* Draws the frame into fb, whose tiles' hashes are in 'shown' (BS_NT of
 * them, updated); full: fb's picture is unknown, draw it all. Returns the
 * tiles drawn. */
int  bs_gfx_end(bs_gfx_t *g, uint16_t *fb, uint32_t *shown, bool full);

/* depth units of a ground point Y metres from the screen's bottom edge */
static inline uint16_t bs_z_of(float y_m)
{
    float d = (0.6f * y_m + BS_Z_OFF) * BS_Z_UNIT;
    return (uint16_t)(d < 0 ? 0 : d > 65535.0f ? 65535 : (int)d);
}
