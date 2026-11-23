/*
 * VITRAL - drawing on the retro canvas: glass, lead, stone, the rose window
 *
 * Everything is drawn by code into RGB565 buffers the size of the canvas
 * (360x640 upright, 640x360 lying down, shown x2 by the OS). A surface is
 * a part of the canvas with its own origin, optionally turned half a turn:
 * the far player of a match upright sees their well the right way up.
 *
 * Integers only in the pixel loops (a sine table, a hash): the P4 has a
 * single-precision FPU, but these run for every pixel of a cell.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t *px;
    int16_t   cw, ch;           /* the whole canvas (stride == cw) */
    int16_t   ox, oy, w, h;     /* this surface on it */
    bool      flip;             /* turned 180 degrees */
} vt_surf_t;

typedef struct {
    int16_t x, y, w, h;
} vt_rect_t;

static inline uint16_t vt_rgb(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline uint16_t vt_hex(uint32_t c)
{
    return vt_rgb((int)(c >> 16) & 255, (int)(c >> 8) & 255, (int)c & 255);
}

/* a + (b - a) * f / 256 */
static inline uint16_t vt_mix(uint16_t a, uint16_t b, int f)
{
    if (f <= 0) return a;
    if (f >= 256) return b;
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    ar += ((br - ar) * f) >> 8;
    ag += ((bg - ag) * f) >> 8;
    ab += ((bb - ab) * f) >> 8;
    return (uint16_t)((ar << 11) | (ag << 5) | ab);
}

/* adds light (0..256 of the colour 'c') */
static inline uint16_t vt_add(uint16_t a, uint16_t c, int f)
{
    int r = (a >> 11) + (((c >> 11) * f) >> 8);
    int g = ((a >> 5) & 63) + ((((c >> 5) & 63) * f) >> 8);
    int b = (a & 31) + (((c & 31) * f) >> 8);
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* the glass colours, 0xRRGGBB, by cell value (vt_game.h) */
uint32_t vt_glass_rgb(int color);

void vt_surf_init(vt_surf_t *s, uint16_t *px, int cw, int ch, int ox, int oy, int w, int h, bool flip);
/* a part of a surface as a surface of its own (it clips to it) */
vt_surf_t vt_sub(const vt_surf_t *s, int x, int y, int w, int h);
/* a point of the surface on the canvas */
static inline void vt_to_canvas(const vt_surf_t *s, int x, int y, int *cx, int *cy)
{
    *cx = s->flip ? s->ox + s->w - 1 - x : s->ox + x;
    *cy = s->flip ? s->oy + s->h - 1 - y : s->oy + y;
}
/* a rectangle of the surface, in canvas pixels, clipped (w = 0: nothing) */
vt_rect_t vt_surf_rect(const vt_surf_t *s, int x, int y, int w, int h);

/* writes one pixel of the surface (clipped) */
static inline void vt_put(const vt_surf_t *s, int x, int y, uint16_t c)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    int X = s->flip ? s->ox + s->w - 1 - x : s->ox + x;
    int Y = s->flip ? s->oy + s->h - 1 - y : s->oy + y;
    if ((unsigned)X >= (unsigned)s->cw || (unsigned)Y >= (unsigned)s->ch) return;
    s->px[Y * s->cw + X] = c;
}

static inline uint16_t vt_get(const vt_surf_t *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    int X = s->flip ? s->ox + s->w - 1 - x : s->ox + x;
    int Y = s->flip ? s->oy + s->h - 1 - y : s->oy + y;
    if ((unsigned)X >= (unsigned)s->cw || (unsigned)Y >= (unsigned)s->ch) return 0;
    return s->px[Y * s->cw + X];
}

vt_rect_t vt_fill(const vt_surf_t *s, int x, int y, int w, int h, uint16_t c);
/* blends a colour over a rectangle, f of 256 */
vt_rect_t vt_tint(const vt_surf_t *s, int x, int y, int w, int h, uint16_t c, int f);
/* adds light over a rectangle */
vt_rect_t vt_glow(const vt_surf_t *s, int x, int y, int w, int h, uint16_t c, int f);
/* copies a canvas rectangle from one buffer to another (same size) */
void vt_copy(uint16_t *dst, const uint16_t *src, int cw, vt_rect_t r);

/* ---- glass ----
 * A cell of glass c pixels square at (x, y) of the surface. 'lead' says on
 * which sides the lead runs (the neighbour is another piece, or nothing):
 * the bits below. 'gx/gy' is where the cell is in the well, so the waves of
 * the glass go on from one cell to the next of the same piece. 'seed' makes
 * each piece a little different. 'light' 0 = as it rests, more = brighter
 * (the falling piece, a flash). */
enum {
    VT_L_TOP = 1, VT_L_RIGHT = 2, VT_L_BOTTOM = 4, VT_L_LEFT = 8,
    VT_L_TL = 16, VT_L_TR = 32, VT_L_BR = 64, VT_L_BL = 128,
};
void vt_glass_cell(const vt_surf_t *s, int x, int y, int c, int color, uint8_t lead,
                   int gx, int gy, uint32_t seed, int light);
/* the night behind an empty cell; 'target' = a cell of the puzzle's window
 * still to fill, 'edge' its outline (the VT_L_* bits) */
void vt_empty_cell(const vt_surf_t *s, int x, int y, int c, int gx, int gy, bool target, uint8_t edge);
/* the outline of a cell where the piece will land */
void vt_ghost_cell(const vt_surf_t *s, int x, int y, int c, int color, uint8_t lead);

/* the wall behind everything, on the whole surface */
void vt_wall(const vt_surf_t *s);
/* the stone frame round a well (x, y, w, h is the glass inside), with a
 * pointed arch 'arch' pixels tall on top */
void vt_frame(const vt_surf_t *s, int x, int y, int w, int h, int arch, bool oculus);
/* a small framed box (the previews) */
void vt_box(const vt_surf_t *s, int x, int y, int w, int h);
/* the rose window of the title, centred at (cx, cy), radius r */
void vt_rose(const vt_surf_t *s, int cx, int cy, int r);
/* an on-screen button of the button controls: a stone key with a glyph
 * (VT_GLYPH_*) of glass; 'down' lights it */
enum { VT_GLYPH_LEFT, VT_GLYPH_RIGHT, VT_GLYPH_DOWN, VT_GLYPH_DROP, VT_GLYPH_CW, VT_GLYPH_CCW };
void vt_key(const vt_surf_t *s, int x, int y, int w, int h, int glyph, bool down);

/* ---- what changed: tiles of 8x8, gathered into a few rectangles ---- */
#define VT_TILE      8
#define VT_TILES_X   80          /* 640 / 8 */
#define VT_TILES_Y   80
typedef struct {
    uint8_t  t[VT_TILES_Y][VT_TILES_X / 8];
    int16_t  cw, ch;
    bool     any;
} vt_dirty_t;

void vt_dirty_init(vt_dirty_t *d, int cw, int ch);
void vt_dirty_clear(vt_dirty_t *d);
void vt_dirty_add(vt_dirty_t *d, vt_rect_t r);
void vt_dirty_or(vt_dirty_t *d, const vt_dirty_t *o);
/* the marked tiles as rectangles: bands of 48 rows, up to two runs each.
 * Returns how many (<= max). */
int  vt_dirty_rects(const vt_dirty_t *d, vt_rect_t *out, int max);

/* a sine of 256 steps a turn, -127..127 */
extern const int8_t vt_sin[256];
static inline uint32_t vt_hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
