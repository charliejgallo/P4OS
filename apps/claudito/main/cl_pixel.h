/*
 * Claudito - pixel art engine
 *
 * Everything visible in the app is drawn into small RGB565 buffers of art
 * pixels. On the watch (368x448) LVGL showed a 92x112 grid x4; on P4OS the
 * grid is the OS's retro canvas (aos_retro.h), 90x160 in portrait or 160x90
 * in landscape, shown x8 with nearest-neighbour: each art pixel is 8x8 real
 * pixels and the canvas is the whole 720x1280 screen. The HUD, the stage and
 * the bar are three rectangles of that one canvas: a cl_buf_t is a view onto
 * part of it, with the canvas's width as its stride.
 *
 * The buffers are opaque: there is no alpha channel, things are drawn back to
 * front and that is that. The sprites are written as ASCII art and '.' means
 * "do not touch".
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* The stage's art is laid out on the watch's 92x78 (the width of its grid,
 * the height of its stage), and every position in cl_scene.c and in the
 * game's logic is on that frame. The P4's stage is taller, and in portrait
 * one column narrower: the buffer's origin (ox, oy) says where that frame
 * falls in it, and what lies outside it is more wall, sky and floor. */
#define CL_ART_W        92
#define CL_STAGE_H      78

typedef struct {
    uint16_t *px;       /* the view's top-left pixel                         */
    int16_t   w;
    int16_t   h;
    int16_t   stride;   /* pixels from one row to the next                   */
    /* Where the drawing's (0,0) falls in the view: every primitive adds it
     * before clipping. 0,0 for the HUD and the bar; the stage uses it to
     * place the watch's 92x78 frame (see CL_ART_W). */
    int16_t   ox, oy;
    /* Night: every colour written is first darkened by this many sixteenths
     * (cl_shade()'s scale, less in blue so the dark goes blue; 0 = as given).
     * The night scenes draw the ground with it and the sky, the moon and the
     * lamp without. */
    int8_t    dim;
} cl_buf_t;

/* A rectangle in drawing coordinates, for the parts of a view that changed */
typedef struct {
    int16_t x, y, w, h;
} cl_rect_t;

/* A view of w x h pixels starting at px, rows 'stride' apart, origin 0,0 */
static inline void cl_view(cl_buf_t *b, uint16_t *px, int w, int h, int stride)
{
    b->px     = px;
    b->w      = (int16_t)w;
    b->h      = (int16_t)h;
    b->stride = (int16_t)stride;
    b->ox     = 0;
    b->oy     = 0;
    b->dim    = 0;
}

/* The visible part of a view, in drawing coordinates: [left, right) x
 * [top, bottom). On the HUD and the bar that is simply 0..w, 0..h. */
static inline int cl_left(const cl_buf_t *b)   { return -b->ox; }
static inline int cl_top(const cl_buf_t *b)    { return -b->oy; }
static inline int cl_right(const cl_buf_t *b)  { return b->w - b->ox; }
static inline int cl_bottom(const cl_buf_t *b) { return b->h - b->oy; }

/* 0xRRGGBB -> RGB565 (the canvas's and the panel's native format) */
static inline uint16_t cl_rgb(uint32_t hex)
{
    return (uint16_t)(((hex >> 19) & 0x1F) << 11 |
                      ((hex >> 10) & 0x3F) << 5  |
                      ((hex >> 3)  & 0x1F));
}

/* Palette of the ASCII sprites. Returns false if the character is
 * transparent. */
bool cl_pal(char ch, uint16_t *out);

/* ---- primitives ---------------------------------------------------------- */

void cl_px(cl_buf_t *b, int x, int y, uint16_t c);
void cl_fill(cl_buf_t *b, uint16_t c);
void cl_copy(cl_buf_t *dst, const cl_buf_t *src);
void cl_rect(cl_buf_t *b, int x, int y, int w, int h, uint16_t c);
void cl_frame(cl_buf_t *b, int x, int y, int w, int h, uint16_t c);
void cl_hline(cl_buf_t *b, int x, int y, int len, uint16_t c);
void cl_vline(cl_buf_t *b, int x, int y, int len, uint16_t c);
void cl_disc(cl_buf_t *b, int cx, int cy, int r, uint16_t c);
void cl_ring(cl_buf_t *b, int cx, int cy, int r, uint16_t c);
/* rectangle with all four corners bitten off: the style's basic shape */
void cl_round(cl_buf_t *b, int x, int y, int w, int h, int cut, uint16_t c);
/* A checkerboard of colour c over the area: half the pixels, the ones where
 * x + y is even. Two bands of colour meet through one of these rows instead
 * of a hard line, which is how a gradient reads at this resolution. */
void cl_dither(cl_buf_t *b, int x, int y, int w, int h, uint16_t c);
/* c moved k sixteenths of the way towards 'to' */
uint16_t cl_blend(uint16_t c, uint16_t to, int k);
/* The same on one pixel of the view: coloured light, where cl_shade() only
 * knows lighter and darker */
void cl_mix(cl_buf_t *b, int x, int y, uint16_t to, int k);
/* One colour darkened (f<0) or lightened (f>0) by f sixteenths */
uint16_t cl_tint(uint16_t c, int f);
/* flat blend: darkens (f<0) or lightens (f>0) an area, in sixteenths */
void cl_shade(cl_buf_t *b, int x, int y, int w, int h, int f);

/* ---- ASCII sprites ------------------------------------------------------- */

#define CL_SPRITE(a)    (a), (int)(sizeof(a) / sizeof((a)[0]))

void cl_blit(cl_buf_t *b, int x, int y, const char *const *rows, int nrows, bool flip);
/* the same but painting everything one colour, for shadows or silhouettes */
void cl_blit_solid(cl_buf_t *b, int x, int y, const char *const *rows, int nrows,
                   bool flip, uint16_t c);

/* ---- text (our own 5x7 font) --------------------------------------------- */

#define CL_CH_W         5
#define CL_CH_H         7
#define CL_CH_ADV       6                   /* advance per character, including the space */

int  cl_text_w(const char *s);
void cl_text(cl_buf_t *b, int x, int y, const char *s, uint16_t c);
/* with a hard 1 px shadow bottom right: it reads over any background */
void cl_text_sh(cl_buf_t *b, int x, int y, const char *s, uint16_t c, uint16_t sh);
void cl_text_center(cl_buf_t *b, int cx, int y, const char *s, uint16_t c, uint16_t sh);
