/*
 * MILA - pixels
 *
 * Everything the worker draws is RGB565 in NATIVE order (the background
 * cache, the band, the sprites): blending needs no byte swaps. On P4OS the
 * frame stays in that order to the end: the LVGL timer pushes it with
 * aos_hal_display_blit_scaled(..., big_endian = false) or blit_fit, and the
 * canvas under the panels shows the same buffer (the watch swapped each band
 * to its panel's big-endian order on the way out).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The frame is the whole screen as it is now: 720 x 1280 upright, 1280 x
 * 720 lying down. Set before a scene loads and when the screen turns
 * (JOB_FIT); every place that draws takes it from here. */
extern int ml_vw, ml_vh;
#define ML_W        ml_vw
#define ML_H        ml_vh
#define ML_WMAX     1280        /* either side, at most                        */

/* The art is the watch's re-rendered RES times finer (tools/blender/
 * ml_common.py, ML_RES): one cell is 108 x 81 px here against 72 x 54.
 * Every constant the watch had in screen pixels goes through ML_PX(). */
#define ML_RES      1.5f
#define ML_PX(v)    ((int)((v) * ML_RES))

static inline uint16_t ml_rgb(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint16_t ml_hex(uint32_t c)
{
    return ml_rgb((int)(c >> 16) & 255, (int)(c >> 8) & 255, (int)c & 255);
}
static inline void ml_unpack(uint16_t c, int *r, int *g, int *b)
{
    int r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

/* a over b, alpha 0..255 */
static inline uint16_t ml_blend(uint16_t b, uint16_t a, int alpha)
{
    if (alpha >= 255) return a;
    if (alpha <= 0) return b;
    uint32_t al = (uint32_t)(alpha + 4) >> 3;
    uint32_t bb = (b | ((uint32_t)b << 16)) & 0x07E0F81FU;
    uint32_t aa = (a | ((uint32_t)a << 16)) & 0x07E0F81FU;
    uint32_t r  = ((((aa - bb) * al) >> 5) + bb) & 0x07E0F81FU;
    return (uint16_t)(r | (r >> 16));
}

/* k/256 of the colour, k <= 256 */
static inline uint16_t ml_darken(uint16_t c, int k)
{
    uint32_t s = (c | ((uint32_t)c << 16)) & 0x07E0F81FU;
    s = ((s * (uint32_t)(k >> 3)) >> 5) & 0x07E0F81FU;
    return (uint16_t)(s | (s >> 16));
}

/* c + a, per channel, saturating (glows) */
static inline uint16_t ml_add(uint16_t c, uint16_t a)
{
    uint32_t r = (c & 0xF800u) + (a & 0xF800u);
    uint32_t g = (c & 0x07E0u) + (a & 0x07E0u);
    uint32_t b = (c & 0x001Fu) + (a & 0x001Fu);
    if (r > 0xF800u) r = 0xF800u;
    if (g > 0x07E0u) g = 0x07E0u;
    if (b > 0x001Fu) b = 0x001Fu;
    return (uint16_t)(r | g | b);
}

/* k/256 of an additive colour (glow strength) */
static inline uint16_t ml_scale(uint16_t c, int k)
{
    int r = ((c >> 11) & 31) * k >> 8, g = ((c >> 5) & 63) * k >> 8, b = (c & 31) * k >> 8;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

uint32_t ml_mix(uint32_t a, uint32_t b, int t);   /* 8-bit colours, t 0..256 */


/* floorf is a library call on the S3; this is two instructions */
static inline int ml_ifloor(float x)
{
    int i = (int)x;
    return i - (x < (float)i);
}
static inline int ml_iround(float x)
{
    return ml_ifloor(x + 0.5f);
}

/* an image to draw into, with a clip rectangle (x1/y1 exclusive) and an
 * origin: px points at the pixel of the image's (0, 0) even when the
 * memory only holds a band (then px is offset and only the clip rows exist) */
typedef struct {
    uint16_t *px;
    int16_t   w, h;           /* stride = w                                  */
    int16_t   cx0, cy0, cx1, cy1;
} ml_img_t;

void ml_img_init(ml_img_t *im, uint16_t *px, int w, int h);
void ml_img_clip(ml_img_t *im, int x0, int y0, int x1, int y1);
void ml_rect(ml_img_t *im, int x, int y, int w, int h, uint16_t c);
void ml_rect_blend(ml_img_t *im, int x, int y, int w, int h, uint16_t c, int alpha);
void ml_rrect(ml_img_t *im, int x, int y, int w, int h, int r, uint16_t c, int alpha);
/* a disc, centre and radius in 1/16 px, soft edge */
void ml_disc(ml_img_t *im, int cx16, int cy16, int r16, uint16_t c, int alpha);
/* a soft ellipse that darkens, alpha at the centre */
void ml_shadow_ellipse(ml_img_t *im, int cx, int cy, int rx, int ry, int alpha);
/* an alpha mask tinted with c: text, icons baked at open */
typedef struct {
    uint8_t *a;
    int16_t  w, h;
} ml_mask_t;
void ml_mask_draw(ml_img_t *im, const ml_mask_t *m, int x, int y, uint16_t c, int alpha);

/* Every allocation to PSRAM, small ones too (below 1 KB plain malloc gives
 * internal RAM, CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL). free() frees them. */
void *ml_malloc(size_t n);
void *ml_calloc(size_t n, size_t size);
/* Internal RAM that is also DMA-capable: that keeps it out of the LP SRAM,
 * which the heap counts as internal too and which is 8-15 times slower from
 * the HP cores (docs/MEMORY.md). For the bands. */
void *ml_malloc_internal(size_t n);
void *ml_malloc_band(size_t n);      /* a band: internal RAM, or PSRAM with pref "bands_psram" */
bool  ml_bands_psram(void);

/* long jobs call this now and then (the worker's sleep); NULL does nothing */
void ml_set_yield(void (*hook)(void));
void ml_yield(void);
void ml_set_clock(uint32_t (*clock)(void));
uint32_t ml_clock(void);


/* deterministic random numbers (the two watches must agree) */
static inline uint32_t ml_rand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x ? x : 0x9E3779B9u;
}
static inline float ml_randf(uint32_t *s)
{
    return (float)(ml_rand(s) >> 8) * (1.0f / 16777216.0f);
}
static inline uint32_t ml_hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}
