/*
 * METEGOL - the picture (Bastion's compositor, lying down)
 *
 * A frame is a list of things to draw, in order (mg_spr, mg_col, mg_text
 * ...), over the stadium's picture. Nobody says what changed: the screen
 * (1280 x 720, as the player sees it) is cut in 16 x 16 tiles and each
 * tile's list of things is hashed; a tile whose hash is the one the target
 * already shows is left alone, the rest get the stadium's pixels back and
 * their things drawn again, clipped to them. Each of the panel's three
 * buffers remembers its own hashes.
 *
 * The panel is upright (720 x 1280) and the game lies down. Turning a
 * whole frame costs the PPA 42-62 ms, so the turn is done per tile row
 * instead: a row of tiles is drawn in the screen's orientation into a
 * strip (each core has its own, 40 KB) and its changed runs are written
 * into the panel's buffer turned, 16 pixels at a time down each column.
 * In the simulator, and for the copy LVGL shows under its panels, the
 * target is a plain 1280 x 720 buffer and the rows are drawn straight in.
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

#define MG_W        1280
#define MG_H        720
#define MG_TILE     16
#define MG_TW       (MG_W / MG_TILE)
#define MG_TH       (MG_H / MG_TILE)
#define MG_NT       (MG_TW * MG_TH)
#define MG_MAX_ITEMS 1600
#define MG_TEXT_ARENA 4096

enum {
    MG_F_FLASH  = 2,    /* whitened                                         */
    MG_F_ADD    = 4,    /* light added instead of blended                   */
    MG_F_SHADOW = 8,    /* text: a dark copy under it                       */
    MG_F_CENTER = 16,   /* text: x is its middle                            */
    MG_F_RIGHT  = 32,   /* text: x is its right end                         */
};

/* how the target holds the screen */
enum {
    MG_OUT_SCREEN = 0,  /* 1280 x 720, as seen                              */
    MG_OUT_ROT90  = 90, /* the panel's buffer, screen (x, y) at (719 - y, x) */
    MG_OUT_ROT270 = 270 /* the panel's buffer, screen (x, y) at (y, 1279 - x) */
};

typedef struct {
    int16_t  ox, oy;
    uint16_t w, h;
    uint32_t off;
} mg_frame_t;

typedef struct {
    uint8_t          *data;
    const mg_frame_t *fr;
    int               nframes, cols;
    uint32_t          id;       /* changes when the sheet's pixels do (hashing) */
} mg_sheet_t;

typedef struct {
    uint32_t cp;
    uint16_t x, y;
    uint8_t  w, h;
    int8_t   ox, oy;
    uint8_t  adv, pad;
    uint16_t spare;
} mg_glyph_t;

typedef struct {
    uint8_t          *data;
    const mg_glyph_t *g;
    const uint8_t    *atlas;
    int               n, line_h, base, aw, ah;
    int16_t           idx[256];
} mg_font_t;

/* a rod's cross-section: one row of pixels repeated down a column */
typedef struct {
    uint16_t px[24];
    uint8_t  a[24];
    int      w;
    uint32_t id;
} mg_profile_t;

typedef struct {
    int16_t x0, y0, x1, y1;
} mg_rect_t;

typedef struct {
    uint8_t  kind, flags, a, aux;
    int16_t  x0, y0, x1, y1;        /* what it covers, inside the screen   */
    int16_t  x, y;
    int16_t  p0, p1, p2, p3;
    uint16_t color, spare;
    const void *p;
    uint32_t hash;
} mg_item_t;

typedef struct {
    const uint16_t *bg;             /* MG_W x MG_H, or NULL (black)         */
    int        bg_dx, bg_dy;        /* the picture shaken (a goal)          */
    uint32_t   salt;                /* mixed into every tile's hash: a shake */

    mg_item_t *item;
    int        nitem;
    bool       overflow;
    char      *text;
    int        ntext;

    uint32_t  *cur;                 /* this frame's hash of every tile      */
    uint8_t   *need;
    uint16_t  *band_ix;
    int        band_cap;
    int        band_at[MG_TH + 1];
    uint8_t    glow[64 * 64];
    uint16_t  *strip[2];            /* a row of tiles, per core             */

    int        drawn_tiles;

    /* one frame's job, for the two halves */
    uint16_t  *fb;
    int        out;
    int        split;
    bool       one_core;
} mg_gfx_t;

static inline uint16_t mg_rgb(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

bool mg_gfx_init(mg_gfx_t *g);
void mg_gfx_free(mg_gfx_t *g);
void mg_gfx_world(mg_gfx_t *g, const uint16_t *bg);

bool mg_sheet_bind(mg_sheet_t *s, uint8_t *data, size_t len);
bool mg_font_bind(mg_font_t *f, uint8_t *data, size_t len);
int  mg_text_width(const mg_font_t *f, const char *s);

/* a sheet in runs from frames of RGBA (as the pack's encoder does it): 'px'
 * is w*h RGB565 and 'al' w*h alpha per frame; returns a new block (mg_free) */
typedef struct {
    const uint16_t *px;
    const uint8_t  *al;
    int w, h, ox, oy;
} mg_rgba_frame_t;
uint8_t *mg_sheet_encode(const mg_rgba_frame_t *fr, int n, int cols, size_t *len);

void mg_gfx_begin(mg_gfx_t *g);
void mg_spr(mg_gfx_t *g, const mg_sheet_t *s, int frame, int x, int y, int flags, int alpha);
/* a vertical bar from y0 to y1 whose every row is the profile, left edge at x */
void mg_col(mg_gfx_t *g, const mg_profile_t *pr, int x, int y0, int y1);
void mg_glow(mg_gfx_t *g, int x, int y, int r, uint16_t color, int k);
void mg_shadow(mg_gfx_t *g, int x, int y, int rx, int ry, int a);
void mg_rect(mg_gfx_t *g, int x, int y, int w, int h, uint16_t color, int a, int radius);
int  mg_text(mg_gfx_t *g, const mg_font_t *f, int x, int y, const char *s, uint16_t color, int flags);
void mg_ring(mg_gfx_t *g, int cx, int cy, int rx, int ry, int thick, uint16_t color, int k, int flags);
void mg_disc(mg_gfx_t *g, int cx, int cy, int rx, int ry, uint16_t color, int a);
/* Draws the frame into fb (laid out as 'out' says), whose tiles' hashes are
 * in 'shown' (MG_NT of them, updated); full: draw it all. Returns the tiles
 * drawn. */
int  mg_gfx_end(mg_gfx_t *g, uint16_t *fb, int out, uint32_t *shown, bool full);
