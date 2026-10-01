/*
 * NEON SNAKES - the compositor
 *
 * The screen is one RGB565 buffer the size of the screen, shown 1:1 (no
 * upscaling: the sprites are drawn at the cell size of each mode), and a
 * second one, 'bg', with the arena's frame on black. Nothing is ever redrawn
 * whole during play. A CELL is the unit of repainting: restore it from bg,
 * then blit into it, clipped, the sprite of every cell of its 3x3
 * neighbourhood - a sprite is 2x2 cells centred on its own, so nothing
 * further can reach in - taking the brighter channel, plus any effect
 * centred on those nine cells.
 *
 * What gets repainted in a frame is the 3x3 block around every cell the
 * engine marked (a head that moved, a tail that left, a fruit that appeared),
 * around every fruit when the pulse changes level, and around every effect
 * while it lives and on the frame it ends. The repainted cells are then
 * gathered into rows and rectangles for LVGL to flush, so what reaches the
 * panel is a few small patches per step.
 *
 * P4OS: the screen is 720x1280 either way up and the arena is laid out
 * inside a region of it (what the controls leave), so the sizes are the
 * view's and not constants. On a field this big a frame can mark dozens of
 * scattered blocks - forty fruits breathing at once in combat - and LVGL
 * keeps 32 invalid areas before it gives up and redraws the whole screen. So
 * past NS_MAX_OUT the rectangles are merged, the pair that wastes the fewest
 * pixels first, until NS_MAX_OUT are left: some black between patches is
 * pushed, never the whole screen.
 *
 * The rule that keeps it exact (tools/ns_harness.c checks it against a full
 * repaint every frame): a cell's pixels depend only on the state of its
 * neighbourhood, never on what was in the buffer.
 *
 * The grid has a ring of margin cells around the arena (x = -1..cols,
 * y = -1..rows) that hold nothing but receive glow; they are repainted like
 * any other, and the frame lives in them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ns_art.h"
#include "ns_game.h"

#define NS_EXT_CELLS    (NS_MAX_CELLS + 2 * NS_MAX_COLS + 2 * NS_MAX_ROWS + 4)
#define NS_MAX_FX       16
#define NS_MAX_RUNS     512         /* rectangles before pooling            */
#define NS_MAX_OUT      18          /* what LVGL is handed, at most         */

typedef struct {
    int16_t x0, y0, x1, y1;         /* px, x1/y1 exclusive */
} ns_rect_t;

enum { NS_FX_RING = 1, NS_FX_BURST };

typedef struct {
    uint8_t  type;
    uint8_t  x, y;                  /* the cell it is centred on */
    int8_t   t, len;                /* frame (-1 = not shown yet), length */
    uint32_t rgb;
} ns_fx_t;

typedef struct {
    uint16_t *fb, *bg;
    int       w, h;                 /* the buffers (the screen), px         */
    int       cell, ox, oy, cols, rows;
    const ns_art_t *art;

    uint8_t   rep[NS_EXT_CELLS];
    bool      rep_any;
    uint8_t   pulse;                /* 0..3: the fruits' breathing phase    */

    ns_fx_t   fx[NS_MAX_FX];
    uint8_t   nfx;

    /* the ring that says "this one is you" for the first seconds */
    int8_t    me;                   /* snake index, -1 for none             */
    uint8_t   halo_t;               /* frames left                          */
    int16_t   halo_x, halo_y;       /* where it was drawn last              */

    ns_rect_t runs[NS_MAX_RUNS];    /* scratch for collect_rects()          */
    ns_rect_t rects[NS_MAX_OUT];
    uint8_t   nrects;
    uint32_t  pixels;               /* pushed in the last frame, for the log*/
} ns_view_t;

/* Where the arena's cell (0, 0) lands when cols x rows cells of 'cell' px
 * are centred in the region: ns_view_init() does this, and the app needs it
 * before there is a view, to know which cells its pause pill covers. */
static inline void ns_arena_origin(int cell, int cols, int rows, int rx, int ry, int rw, int rh,
                                   int *ox, int *oy)
{
    *ox = rx + (rw - cols * cell) / 2;
    *oy = ry + (rh - rows * cell) / 2;
}

/* Lays out the arena for the game's grid on the two w x h buffers, centred
 * in the region (rx, ry, rw, rh) with its margin ring inside it, and draws
 * the frame into bg. The cell is the art's. */
void ns_view_init(ns_view_t *v, uint16_t *fb, uint16_t *bg, int w, int h,
                  const ns_art_t *art, const ns_game_t *g,
                  int rx, int ry, int rw, int rh);

/* A burst of colour where something was eaten or died. */
void ns_view_fx(ns_view_t *v, uint8_t type, int x, int y, uint32_t rgb);

/* Show the "you" ring around snake 'me' for 'frames' frames. */
void ns_view_halo(ns_view_t *v, int me, int frames);

/* One frame: consume the engine's marks, advance effects and the pulse
 * (pulse = 0..3), repaint, and leave in v->rects what to invalidate. */
void ns_view_frame(ns_view_t *v, ns_game_t *g, uint8_t pulse);

/* Repaint every cell (and the whole screen goes in v->rects). */
void ns_view_full(ns_view_t *v, ns_game_t *g);
