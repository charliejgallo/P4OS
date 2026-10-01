/*
 * TURBO - pixels
 *
 * P4OS: everything the worker draws is RGB565 in NATIVE (little-endian)
 * order, LVGL's own: the frame goes to the panel with
 * aos_hal_display_blit_scaled(..., big_endian = false) standing up, or
 * already turned for the panel with aos_hal_display_blit_native() lying
 * down, and the canvas under the panels shows the same buffers. The watch
 * kept the panel's big-endian order and swapped on every blend.
 *
 * The screen's shape is a variable (tb_view): 720 x 1280 upright, 1280 x
 * 720 lying down, set before a race or a scene is drawn. The camera is the
 * watch's with twice the focal length (600 px), so across the 720-pixel
 * column the road looks as it did across the watch's 368.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TB_WMAX     1280        /* the largest side, for per-row tables  */
#define TB_HMAX     1280

typedef struct {
    int w, h;           /* the frame                                          */
    int cx;             /* the road's centre column                           */
    int hor;            /* the horizon row of a flat road                     */
    int car_y;          /* where the player's car's rear bumper meets the road */
    bool land;          /* lying down                                         */
} tb_view_t;

extern tb_view_t tb_view;
/* sets tb_view for a screen of w x h */
void tb_view_set(int w, int h);

#define TB_W        (tb_view.w)
#define TB_H        (tb_view.h)
#define TB_CX       (tb_view.cx)
#define TB_HOR      (tb_view.hor)
#define TB_CAR_Y    (tb_view.car_y)
#define TB_F        600.0f      /* focal length, px (the watch: 300)      */
#define TB_K        2           /* screen px per watch px, for the HUD    */

/* native order: no swapping any more */
static inline uint16_t tb_swap(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}
static inline uint16_t tb_rgb(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint16_t tb_hex(uint32_t c)
{
    return tb_rgb((int)(c >> 16) & 255, (int)(c >> 8) & 255, (int)c & 255);
}
static inline void tb_unpack(uint16_t c, int *r, int *g, int *b)
{
    int r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

/* one copy per file that dithers */
static const uint8_t tb_bayer[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};
static inline uint16_t tb_dither(int r, int g, int b, int x, int y)
{
    int d = tb_bayer[((y & 3) << 2) | (x & 3)];
    r += (d >> 1) - 4;
    g += (d >> 2) - 2;
    b += (d >> 1) - 4;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return tb_rgb(r, g, b);
}

static inline int tb_ifloor(float x)
{
    int i = (int)x;
    return i - (x < (float)i);
}

/* a over b, alpha 0..255 */
static inline uint16_t tb_blend(uint16_t b, uint16_t a, int alpha)
{
    if (alpha >= 255) return a;
    if (alpha <= 0) return b;
    /* 565 blend in the 32-bit spread form, alpha in 0..32 */
    uint32_t al = (uint32_t)(alpha + 4) >> 3;
    uint32_t bb = (b | ((uint32_t)b << 16)) & 0x07E0F81FU;
    uint32_t aa = (a | ((uint32_t)a << 16)) & 0x07E0F81FU;
    uint32_t r  = ((((aa - bb) * al) >> 5) + bb) & 0x07E0F81FU;
    return (uint16_t)(r | (r >> 16));
}
/* darkens to k/256 (k <= 256) with one multiply */
static inline uint16_t tb_darken(uint16_t c, int k)
{
    uint32_t s = (c | ((uint32_t)c << 16)) & 0x07E0F81FU;
    s = ((s * (uint32_t)(k >> 3)) >> 5) & 0x07E0F81FU;
    return (uint16_t)(s | (s >> 16));
}

/* k/256 of the colour (k > 256 brightens, clamped) */
static inline uint16_t tb_scale(uint16_t c, int k)
{
    int r = ((c >> 11) & 31) * k >> 8, g = ((c >> 5) & 63) * k >> 8, b = (c & 31) * k >> 8;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* a mix of two 8-bit colours, t 0..256 */
uint32_t tb_mix(uint32_t a, uint32_t b, int t);

/* An image to draw into, with a clip rectangle (x1/y1 exclusive). Pixel
 * (x, y) is px[y * stride + x]: px may point before the memory, which then
 * only holds the clip rectangle (a band of rows upright, a strip of columns
 * lying down: stride = the strip's width, px = strip - x0). */
typedef struct {
    uint16_t *px;
    int32_t   stride;
    int16_t   w, h;
    int16_t   cx0, cy0, cx1, cy1;
} tb_img_t;

static inline uint16_t *tb_row(const tb_img_t *im, int y)
{
    return im->px + (ptrdiff_t)y * im->stride;
}

void tb_img_init(tb_img_t *im, uint16_t *px, int w, int h);
void tb_img_clip(tb_img_t *im, int x0, int y0, int x1, int y1);

void tb_rect(tb_img_t *im, int x, int y, int w, int h, uint16_t c);
void tb_rect_blend(tb_img_t *im, int x, int y, int w, int h, uint16_t c, int alpha);
/* a disc, positions in 1/16 px, soft edge */
void tb_disc(tb_img_t *im, int cx16, int cy16, int r16, uint16_t c, int alpha);
/* rounded rectangle, antialiased corners */
void tb_rrect(tb_img_t *im, int x, int y, int w, int h, int r, uint16_t c, int alpha);
/* a soft ellipse that darkens, alpha at the centre */
void tb_shadow(tb_img_t *im, int cx, int cy, int rx, int ry, int alpha);
/* a glow: an additive-looking soft disc (lights at night, neon) */
void tb_glow(tb_img_t *im, int cx, int cy, int r, uint32_t rgb, int alpha);
/* a thick antialiased segment (the wheel's spokes), ends rounded */
void tb_capsule(tb_img_t *im, float x0, float y0, float x1, float y1, float r, uint16_t c, int alpha);

/* --------------------------------------------------------------------------
 * Sprites: pixels + an 8-bit alpha plane (NULL = opaque), with an anchor.
 * Mip chains: lv[0] the original, each next one half, box-filtered;
 * drawing picks the level just above the size asked for.
 * -------------------------------------------------------------------------- */

typedef struct {
    uint16_t *px;
    uint8_t  *a;
    int16_t   w, h;
    int16_t   ox, oy;           /* the anchor inside it                      */
    uint16_t *span;             /* per row: first and last visible column (mips) */
    uint16_t *run;              /* per row, for 1:1 drawing: visible [0,1),
                                   the longest opaque stretch [2,3) (tb_sprite_runs) */
} tb_sprite_t;

#define TB_MIPS 7
typedef struct {
    tb_sprite_t lv[TB_MIPS];
    uint8_t     n;
    float       ppm;            /* the original's pixels per metre            */
} tb_mip_t;

bool tb_mip_build(tb_mip_t *m, const tb_sprite_t *src);     /* takes src over */
void tb_mip_free(tb_mip_t *m);
/* anchor at (x, y), 'scale' screen px per ORIGINAL px; rows >= clip_y are
 * not drawn (a hill in front hides them); mirror flips it; fog 0..256
 * towards fog_c; shade 256 = as is */
void tb_mip_draw(tb_img_t *im, const tb_mip_t *m, float x, float y, float scale, int clip_y,
                 bool mirror, uint16_t fog_c, int fog, int shade);
/* the screen box tb_mip_draw would touch (x1/y1 exclusive), for culling */
void tb_mip_box(const tb_mip_t *m, float x, float y, float scale, bool mirror, int *x0, int *x1);
/* an alpha-only mip darkening what is under it */
void tb_mip_shadow(tb_img_t *im, const tb_mip_t *m, float x, float y, float scale, int clip_y, int strength);
/* 1:1, anchor at (x, y). With runs (tb_sprite_runs) the opaque middle of
 * each row is one memcpy and only the edges blend. */
void tb_sprite(tb_img_t *im, const tb_sprite_t *s, int x, int y);
/* the same, the whole sprite at alpha opa */
void tb_sprite_opa(tb_img_t *im, const tb_sprite_t *s, int x, int y, int opa);
bool tb_sprite_runs(tb_sprite_t *s);
void tb_sprite_free(tb_sprite_t *s);

/* Every allocation to PSRAM, small ones too (below 1 KB plain malloc gives
 * internal RAM, CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL). free() frees them. */
void *tb_malloc(size_t n);
void *tb_calloc(size_t n, size_t size);
/* internal RAM that is DMA-capable (docs/MEMORY.md: without the flag the
 * heap may hand out the LP SRAM, 8-15 times slower), for the bands */
void *tb_malloc_internal(size_t n);

/* long jobs call this now and then (the worker's sleep); NULL does nothing */
void tb_set_yield(void (*hook)(void));
void tb_yield(void);
void tb_set_clock(uint32_t (*clock)(void));
uint32_t tb_clock(void);

/* a small deterministic random generator (the two boards must agree on
 * the traffic) */
static inline uint32_t tb_rand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x ? x : 0x9E3779B9u;
}
static inline float tb_randf(uint32_t *s)
{
    return (float)(tb_rand(s) >> 8) * (1.0f / 16777216.0f);
}
