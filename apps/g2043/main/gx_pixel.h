/*
 * 2043 - pixel art engine
 *
 * The whole game is drawn into a single RGB565 canvas, the OS's retro canvas
 * (aos_retro.h), which the OS scales by a whole number, nearest neighbour, by
 * hardware on the board. On the watch (AmoledOS) it was 184x224, upscaled x2
 * by hand to the 368x448 screen.
 *
 * On P4OS the field is 240 px wide and as tall as the screen allows:
 *
 *   portrait   240x426 x3 = 720x1278, the whole panel
 *   landscape  240x360 x2 = 480x720, centred, the controls at the sides
 *
 * The width is fixed, so the waves, formations and bosses keep one geometry;
 * the height is chosen when the app opens (gx_h), and everything vertical is
 * laid out from it. Drawing 100 thousand pixels per frame in software fits
 * comfortably; the scale is the OS's job.
 *
 * The buffer is opaque, with no alpha channel: it is painted back to front.
 * The sprites are written as ASCII art and '.' means "do not touch".
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define GX_W            240
#define GX_H_PORTRAIT   426
#define GX_H_LANDSCAPE  360

/* The field's height in rows, set once by the app before anything is drawn
 * (g2043.c). A variable behind a constant's name: every "GX_H" in the game
 * reads the field of the orientation it was opened in. */
extern int gx_h;
#define GX_H            gx_h

typedef struct {
    uint16_t *px;
    int16_t   w;
    int16_t   h;
} gx_buf_t;

/* 0xRRGGBB -> RGB565 (the canvas's and the panel's format) */
static inline uint16_t gx_rgb(uint32_t hex)
{
    return (uint16_t)(((hex >> 19) & 0x1F) << 11 |
                      ((hex >> 10) & 0x3F) << 5  |
                      ((hex >> 3)  & 0x1F));
}

/* Blends two RGB565s. 'f' goes from 0 (all a) to 16 (all b). */
uint16_t gx_mix(uint16_t a, uint16_t b, int f);

/* Palette of the ASCII sprites. false if the character is transparent. */
bool gx_pal(char ch, uint16_t *out);
uint16_t gx_pal_or(char ch, uint16_t fallback);

/* ---- integer trigonometry ------------------------------------------------
 * The angle goes in brads (0..255 is a full turn) and the result in 1/256.
 * With this there is no need for libm, which in a dynamic app has to be
 * exported symbol by symbol. */
int gx_sin(int brad);
int gx_cos(int brad);
/* integer square root, for distances */
int gx_isqrt(int v);
/* angle of a vector, in brads */
int gx_atan2(int y, int x);

/* ---- primitives ---------------------------------------------------------- */

void gx_px(gx_buf_t *b, int x, int y, uint16_t c);
void gx_fill(gx_buf_t *b, uint16_t c);
void gx_rect(gx_buf_t *b, int x, int y, int w, int h, uint16_t c);
void gx_frame(gx_buf_t *b, int x, int y, int w, int h, uint16_t c);
void gx_hline(gx_buf_t *b, int x, int y, int len, uint16_t c);
void gx_vline(gx_buf_t *b, int x, int y, int len, uint16_t c);
void gx_line(gx_buf_t *b, int x0, int y0, int x1, int y1, uint16_t c);
void gx_disc(gx_buf_t *b, int cx, int cy, int r, uint16_t c);
void gx_ring(gx_buf_t *b, int cx, int cy, int r, uint16_t c);
/* rectangle with bitten corners: the style's basic shape */
void gx_round(gx_buf_t *b, int x, int y, int w, int h, int cut, uint16_t c);
/* vertical gradient between two colours, from row y0 to y1 inclusive */
void gx_vgrad(gx_buf_t *b, int y0, int y1, uint16_t top, uint16_t bot);
/* darkens (f<0) or lightens (f>0) an area, in sixteenths */
void gx_shade(gx_buf_t *b, int x, int y, int w, int h, int f);
/* disc blended with what is already there: halos, glints and shock waves */
void gx_glow(gx_buf_t *b, int cx, int cy, int r, uint16_t c, int f);
/* The screen shake and the white flash, in place over the finished frame:
 * shifts it (sx, sy) art pixels, black where it uncovers, and blends it
 * towards white by 'flash' sixteenths. On the watch both were done by the x2
 * upscaler, which walked every pixel anyway; here the OS scales, so the game
 * does it itself, and only on the frames that need it. */
void gx_shake_flash(gx_buf_t *b, int sx, int sy, int flash);

/* ---- ASCII sprites ------------------------------------------------------- */

#define GX_SPRITE(a)    (a), (int)(sizeof(a) / sizeof((a)[0]))

/* (x,y) is the top-left corner */
void gx_blit(gx_buf_t *b, int x, int y, const char *const *rows, int nrows);
/* the same but in a single colour: silhouettes, shadows and the hit flash */
void gx_blit_solid(gx_buf_t *b, int x, int y, const char *const *rows, int nrows,
                   uint16_t c);
/* centred on (cx,cy), which is how the entities' positions are stored */
void gx_blit_c(gx_buf_t *b, int cx, int cy, const char *const *rows, int nrows);
void gx_blit_c_solid(gx_buf_t *b, int cx, int cy, const char *const *rows,
                     int nrows, uint16_t c);
/* the same sprite but narrowed or widened: with this the player's barrel roll
 * and the respawn come out without drawing a frame per step */
void gx_blit_c_xscale(gx_buf_t *b, int cx, int cy, const char *const *rows,
                      int nrows, int dst_w);
/* mirrored vertically: the same sprites work facing downwards */
void gx_blit_c_flipv(gx_buf_t *b, int cx, int cy, const char *const *rows, int nrows);

int gx_sprite_w(const char *const *rows);

/* ---- text (our own 5x7 font, upper case only) ---------------------------- */

#define GX_CH_W         5
#define GX_CH_H         7
#define GX_CH_ADV       6

int  gx_text_w(const char *s);
void gx_text(gx_buf_t *b, int x, int y, const char *s, uint16_t c);
/* with a hard 1 px shadow: it reads over any background */
void gx_text_sh(gx_buf_t *b, int x, int y, const char *s, uint16_t c, uint16_t sh);
void gx_text_center(gx_buf_t *b, int cx, int y, const char *s, uint16_t c, uint16_t sh);
