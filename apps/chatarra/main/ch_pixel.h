/*
 * CHATARRA - pixel art engine with dirty rectangles
 *
 * Copied from apps/cjump/main/cj_pixel.c, which in turn came from arkanos, and
 * on P4OS given the one thing the watch never needed: a SCALE.
 *
 * ---------------------------------------------------------------------------
 * LOGICAL AND PHYSICAL PIXELS (P4OS)
 * ---------------------------------------------------------------------------
 *
 * On the watch everything was drawn into one 184x224 buffer and the app blew
 * it up x2 by hand. On P4OS the OS's retro canvas does the last x2 (720x1280
 * from a 360x640 canvas, docs/RETRO.md), and the game draws into that canvas
 * at two different sizes:
 *
 *   the interface   in UI units: one unit is 2x2 canvas pixels. The menus,
 *                   the HUD, the combat and the booth keep the watch's
 *                   coordinates and proportions, and come out twice as big
 *                   as they were - which on a 5" screen is the right size.
 *   the world       in world units (a cell is 12), at the zoom the player
 *                   pinched to: 2, 3 or 4 canvas pixels per unit.
 *
 * So a buffer carries its scale `s` and the corner `ox, oy` where its logical
 * (0, 0) lands. Every primitive takes LOGICAL coordinates, and each one
 * decides what to do with the pixels in between:
 *
 *   - rectangles, lines of the frames, the ASCII sprites and the letters keep
 *     their blocks, so the game still looks like the game;
 *   - but the sprites and the letters go through an edge-aware upscaler
 *     (EPX's corner rule, generalised to any factor) and a bevel, so a
 *     diagonal is a diagonal and a brick has a lit top and a shaded foot;
 *   - circles, gradients, glows and washes are drawn at the PHYSICAL
 *     resolution, which is detail the watch could not afford and here is free.
 *
 * Anything that wants the physical resolution on purpose - the combat robot,
 * whose half-unit details become real at four canvas pixels per unit - takes
 * ch_nativo(): the same pixels with s = 1 and the origin at the corner.
 *
 * ---------------------------------------------------------------------------
 * The two buffers and the dirty list, as on the watch
 * ---------------------------------------------------------------------------
 *
 *   bg    what stands still: the room, the panels, the menus.
 *   fb    the frame on screen. It starts as a copy of bg.
 *
 * And per frame:
 *
 *   1. restore from bg into fb the rectangles we dirtied last frame
 *   2. draw what moves, recording each rectangle
 *   3. present ONLY the union of the two sets
 *
 * The rectangles are LOGICAL, like everything the game says, and they are
 * turned into canvas pixels only when they are restored and presented.
 * Everything that moves records ONE rectangle enclosing its old position and
 * its new one. Anything that moves and does not record its rectangle leaves a
 * trail stuck on the screen: nothing crashes, it just looks dirty.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Buffer
 *
 * It carries its own clip, in PHYSICAL pixels. That is not a luxury: the HUD
 * is drawn separately and only repainted when a number changes, so a particle
 * escaping upwards would leave rubbish there until the next score change.
 * With the clip set to the playing field, that cannot happen.
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t *px;               /* physical pixels                            */
    int16_t   w, h;             /* physical size                              */
    int16_t   stride;           /* physical pixels per row                    */
    int16_t   s;                /* logical -> physical                        */
    int16_t   ox, oy;           /* where logical (0, 0) is, physical          */
    int16_t   lw, lh;           /* the logical size ch_clip_none() opens up   */
    int16_t   cx0, cy0, cx1, cy1;   /* clip, physical; x1/y1 exclusive        */
} ch_buf_t;

/* A buffer of w x h physical pixels, drawn at s = 1. */
void ch_buf_init(ch_buf_t *b, uint16_t *px, int w, int h);
/* The general one: 'lw x lh' logical units at scale 's', stride in pixels. */
void ch_buf_escala(ch_buf_t *b, uint16_t *px, int lw, int lh, int stride, int s);
/* Clip in LOGICAL coordinates, like everything else. */
void ch_clip(ch_buf_t *b, int x0, int y0, int x1, int y1);
void ch_clip_none(ch_buf_t *b);
/* The same pixels at the physical resolution: s = 1, origin at the corner,
 * the clip kept. For whoever wants detail finer than a logical unit. */
ch_buf_t ch_nativo(const ch_buf_t *b);

static inline int ch_fx(const ch_buf_t *b, int x) { return b->ox + x * b->s; }
static inline int ch_fy(const ch_buf_t *b, int y) { return b->oy + y * b->s; }

/* 0xRRGGBB -> RGB565 (the canvas's and the panel's format) */
static inline uint16_t ch_rgb(uint32_t hex)
{
    return (uint16_t)(((hex >> 19) & 0x1F) << 11 |
                      ((hex >> 10) & 0x3F) << 5  |
                      ((hex >> 3)  & 0x1F));
}

/* Blends two RGB565s. 'f' goes from 0 (all a) to 16 (all b). */
uint16_t ch_mix(uint16_t a, uint16_t b, int f);
/* Darkens (f<0) or lightens (f>0) a colour, in sixteenths. */
uint16_t ch_tone(uint16_t c, int f);

/* --------------------------------------------------------------------------
 * Dirty rectangles
 * -------------------------------------------------------------------------- */

#define CH_MAX_DIRTY    24

typedef struct {
    int16_t x0, y0, x1, y1;     /* x1/y1 exclusive */
} ch_rect_t;

typedef struct {
    ch_rect_t r[CH_MAX_DIRTY];
    uint8_t   n;
    bool      all;              /* the whole area, no list */
    int16_t   lw, lh;           /* the logical area a rectangle is clamped to */
} ch_dirty_t;

/* Sets the area (and empties the list). reset() keeps the area. */
void ch_dirty_init(ch_dirty_t *d, int lw, int lh);
void ch_dirty_reset(ch_dirty_t *d);
void ch_dirty_all(ch_dirty_t *d);
/* Adds a rectangle. If it is worth it, it merges it with one already there:
 * keeping twenty separate rectangles costs more than one slightly larger. */
void ch_dirty_add(ch_dirty_t *d, int x, int y, int w, int h);
void ch_dirty_join(ch_dirty_t *dst, const ch_dirty_t *src);
/* How many logical pixels the rectangles add up to. */
int  ch_dirty_area(const ch_dirty_t *d);

/* Copies from one buffer to another of the same geometry, only inside the
 * logical rectangle. */
void ch_restore(ch_buf_t *dst, const ch_buf_t *src, const ch_rect_t *r);

/* --------------------------------------------------------------------------
 * Primitives (logical coordinates)
 * -------------------------------------------------------------------------- */

void ch_px(ch_buf_t *b, int x, int y, uint16_t c);
void ch_fill(ch_buf_t *b, uint16_t c);
void ch_rect(ch_buf_t *b, int x, int y, int w, int h, uint16_t c);
void ch_frame(ch_buf_t *b, int x, int y, int w, int h, uint16_t c);
void ch_hline(ch_buf_t *b, int x, int y, int len, uint16_t c);
void ch_vline(ch_buf_t *b, int x, int y, int len, uint16_t c);
/* A line with a brush one logical unit wide, stepped at the physical
 * resolution: the lightning bolt stops being a staircase. */
void ch_line(ch_buf_t *b, int x0, int y0, int x1, int y1, uint16_t c);
void ch_disc(ch_buf_t *b, int cx, int cy, int r, uint16_t c);
void ch_ring(ch_buf_t *b, int cx, int cy, int r, uint16_t c);
/* An ellipse, filled: a platform seen at an angle. Radii in logical units. */
void ch_ellipse(ch_buf_t *b, int cx, int cy, int rx, int ry, uint16_t c);
/* rectangle with bitten corners (the bite is a smooth 45 degrees) */
void ch_round(ch_buf_t *b, int x, int y, int w, int h, int cut, uint16_t c);
/* vertical gradient between two colours, from row y0 to y1 inclusive */
void ch_vgrad(ch_buf_t *b, int x, int y0, int w, int y1, uint16_t top, uint16_t bot);
/* darkens (f<0) or lightens (f>0) an area, in sixteenths */
void ch_shade(ch_buf_t *b, int x, int y, int w, int h, int f);
/* A wash towards a colour: the zone's air over a finished background.
 * `f` IS OUT OF SIXTEEN (see ch_mix). */
void ch_tint(ch_buf_t *b, int x, int y, int w, int h, uint16_t c, int f);
/* disc blended with what is already there: halos, glints and ripples */
void ch_glow(ch_buf_t *b, int cx, int cy, int r, uint16_t c, int f);
/* thick blended ring: an explosion's wave */
void ch_wave(ch_buf_t *b, int cx, int cy, int r, int thick, uint16_t c, int f);

/* --------------------------------------------------------------------------
 * Integer trigonometry
 *
 * Angles in brads (256 per turn), result in 1/256. With this there is no need
 * for libm, which in a dynamic app is paid for symbol by symbol.
 * -------------------------------------------------------------------------- */
int ch_sin(int brad);
int ch_cos(int brad);
int ch_isqrt(int v);

/* --------------------------------------------------------------------------
 * Sprites and text
 * -------------------------------------------------------------------------- */

#define CH_SPRITE(a)    (a), (int)(sizeof(a) / sizeof((a)[0]))

/* What the upscaler does besides upscaling (ch_blit_px). */
#define CH_HD_BISEL     1u      /* light the top-left edge of every block,
                                   shade its foot: bricks, planks, plates   */
#define CH_HD_BALDOSA   2u      /* a ground tile: its edges continue with its
                                   own colour, and it gets a fine grain      */

void     ch_pal_init(void);     /* once, before drawing anything */
bool     ch_pal(char ch, uint16_t *out);
/* An ASCII sprite: through the upscaler, with the bevel. */
void     ch_blit(ch_buf_t *b, int x, int y, const char *const *rows, int nrows);
/* The same for a ground tile (CH_HD_BALDOSA). */
void     ch_blit_tile(ch_buf_t *b, int x, int y, const char *const *rows, int nrows);
void     ch_blit_c(ch_buf_t *b, int cx, int cy, const char *const *rows, int nrows);
int      ch_sprite_w(const char *const *rows);
/* The upscaler itself, over RGB565 pixels (0 = transparent): 'sw x sh'
 * source pixels, each 'm' logical units wide, at logical (x, y). */
void     ch_blit_px(ch_buf_t *b, int x, int y, const uint16_t *src,
                    int sw, int sh, int m, unsigned flags);

/* --------------------------------------------------------------------------
 * THE 2x ART (ASSETS.md)
 *
 * The world's sprites and tiles are drawn at TWICE the world's resolution: a
 * 12-unit cell is a 24x24 tile, a 15x24 character is a 30x48 drawing. They
 * are blitted 1:1 onto the buffer at s/2 physical pixels per art pixel - no
 * upscaler, no bevel, no grain: the shading is in the drawing. Only an even
 * s gives whole pixels (the world's zooms are 2, 4 and 6); an odd one still
 * tiles, with the art pixels one physical pixel apart in size.
 *
 * x, y are LOGICAL, like everywhere: the anchor of a 2x sprite is the same as
 * its 1x one's, and it covers the same logical box.
 * -------------------------------------------------------------------------- */
void ch_blit2(ch_buf_t *b, int x, int y, const char *const *rows, int nrows);
/* The same with each art pixel 'm' HALF units wide: m = 1 is ch_blit2, m = 2
 * is an art pixel per logical unit, m = 4 an icon drawn at twice that. */
void ch_blit2m(ch_buf_t *b, int x, int y, const char *const *rows, int nrows, int m);
void ch_blit2_px(ch_buf_t *b, int x, int y, const uint16_t *src, int sw, int sh);
/* The 2x drawing when there is one, the 1x through the upscaler when not:
 * what keeps the game whole while the art is being redrawn. */
void ch_blit_hd(ch_buf_t *b, int x, int y, const char *const *rows1, int n1,
                const char *const *rows2, int n2);

#define CH_FW         5
#define CH_FH         7
#define CH_FADV       6

int  ch_text_w(const char *s);
void ch_text(ch_buf_t *b, int x, int y, const char *s, uint16_t c);
void ch_text_sh(ch_buf_t *b, int x, int y, const char *s, uint16_t c, uint16_t sh);
void ch_text_center(ch_buf_t *b, int cx, int y, const char *s, uint16_t c, uint16_t sh);
/* Letters 'm' logical units per font pixel: titles. Width is ch_text_w * m. */
void ch_text_big(ch_buf_t *b, int x, int y, const char *s, uint16_t c,
                 uint16_t sh, int m);

/* Integers to text without snprintf: the HUD is drawn often and newlib takes
 * several hundred bytes of stack, which here is the LVGL task's. */
char *ch_num(char *dst, uint32_t v, int min_digits);
