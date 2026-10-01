/*
 * GOLF - pixels: RGB565 images, dithering, antialiased primitives, sprites
 * and dirty rectangles
 *
 * Everything is drawn at the screen's own resolution, 1:1, not upscaled
 * pixel art: the course and the golfer are meant to look rendered. On P4OS
 * the screen is 720x1280 upright or 1280x720 lying down, and can turn while
 * the game runs: GF_W / GF_H are the size of the canvas NOW (gf_w, gf_h),
 * and every buffer the size of the screen is allocated for GF_MAXPX pixels
 * so that it fits either way.
 * Backgrounds (the map, the 3D view) are computed in 8 bits per channel and
 * packed to RGB565 through a 4x4 ordered dither, which is what keeps the sky
 * gradient and the soft hill shading free of bands on the AMOLED.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern int gf_w, gf_h;
#define GF_W        gf_w
#define GF_H        gf_h
#define GF_MAXDIM   1280                    /* the longer side, either way up */
#define GF_MAXPX    (720 * 1280)

typedef struct {
    uint16_t *px;
    int16_t   w, h;
    int16_t   cx0, cy0, cx1, cy1;       /* clip, x1/y1 exclusive              */
} gf_img_t;

void gf_img_init(gf_img_t *im, uint16_t *px, int w, int h);
void gf_img_clip(gf_img_t *im, int x0, int y0, int x1, int y1);
void gf_img_noclip(gf_img_t *im);

static inline uint16_t gf_rgb(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint16_t gf_hex(uint32_t c)
{
    return gf_rgb((int)(c >> 16) & 255, (int)(c >> 8) & 255, (int)c & 255);
}
/* bit replication instead of * 255 / 31: the division was 12 of them per
 * bilinear sample in the 3D view */
static inline void gf_unpack(uint16_t c, int *r, int *g, int *b)
{
    int r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

/* 1 / sqrt(1 + s) for the small s of a slope's normal: no sqrtf, no divide
 * (second order, 1 % off at s = 0.3) */
static inline float gf_inv_len1(float s)
{
    return 1.0f - 0.5f * s + 0.375f * s * s;
}

/* The ordered dither: 8-bit channels to RGB565 at pixel (x, y). */
/* one copy per file that dithers (16 bytes): extern data would go through
 * R_XTENSA_GLOB_DAT, which the .so loader gets wrong with an addend */
static const uint8_t gf_bayer[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};
static inline uint16_t gf_dither(int r, int g, int b, int x, int y)
{
    int d = gf_bayer[((y & 3) << 2) | (x & 3)];     /* 0..15 */
    r += (d >> 1) - 4;                              /* +-4: a 5-bit step is 8 */
    g += (d >> 2) - 2;                              /* +-2: a 6-bit step is 4 */
    b += (d >> 1) - 4;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return gf_rgb(r, g, b);
}

/* floorf/ceilf are library calls on the S3 (no instruction for them), and
 * the renderers call them per pixel: measured, they were a large part of a
 * 1.3 s map. These are two instructions. Valid for |x| < 2^31. */
static inline int gf_ifloor(float x)
{
    int i = (int)x;
    return i - (x < (float)i);
}
static inline float gf_floorf(float x)
{
    return (float)gf_ifloor(x);
}
static inline float gf_ceilf(float x)
{
    int i = (int)x;
    return (float)(i + (x > (float)i));
}

/* a over b, alpha 0..255 (inline: it is in every pixel loop, and the .so's
 * files do not inline across each other) */
static inline uint16_t gf_blend(uint16_t b, uint16_t a, int alpha)
{
    if (alpha >= 255) return a;
    if (alpha <= 0) return b;
    /* 565 blend in the classic 32-bit spread form, alpha in 0..32 */
    uint32_t al = (uint32_t)(alpha + 4) >> 3;
    uint32_t bb = (b | ((uint32_t)b << 16)) & 0x07E0F81FU;
    uint32_t aa = (a | ((uint32_t)a << 16)) & 0x07E0F81FU;
    uint32_t r  = ((((aa - bb) * al) >> 5) + bb) & 0x07E0F81FU;
    return (uint16_t)(r | (r >> 16));
}
/* darkens by k/256 (k = 256 keeps it) */
uint16_t gf_scale(uint16_t c, int k);

/* --------------------------------------------------------------------------
 * Primitives, antialiased where it shows. All honour the clip.
 * -------------------------------------------------------------------------- */

void gf_fill(gf_img_t *im, uint16_t c);
void gf_rect(gf_img_t *im, int x, int y, int w, int h, uint16_t c);
void gf_rect_blend(gf_img_t *im, int x, int y, int w, int h, uint16_t c, int alpha);
/* a disc with a soft edge; positions in 1/16 px */
void gf_disc(gf_img_t *im, int cx16, int cy16, int r16, uint16_t c, int alpha);
/* a disc lit from the upper left: the ball */
void gf_ball(gf_img_t *im, int cx16, int cy16, int r16);
/* a soft ellipse that darkens (shadows), alpha 0..255 at the centre */
void gf_shadow(gf_img_t *im, int cx16, int cy16, int rx16, int ry16, int alpha);
/* an antialiased line of width w16 (1/16 px), round ends */
void gf_line(gf_img_t *im, int x0, int y0, int x1, int y1, int w16, uint16_t c, int alpha);
void gf_ring(gf_img_t *im, int cx16, int cy16, int r16, int w16, uint16_t c, int alpha);
/* rounded rectangle, filled, antialiased corners */
void gf_rrect(gf_img_t *im, int x, int y, int w, int h, int r, uint16_t c, int alpha);

/* --------------------------------------------------------------------------
 * Sprites: RGB565 colour + 8-bit alpha, in separate planes
 * -------------------------------------------------------------------------- */

typedef struct {
    uint16_t *px;
    uint8_t  *a;
    int16_t   w, h;
    int16_t   ox, oy;       /* where its origin (anchor) is inside it        */
} gf_sprite_t;

/* Mip levels of a sprite: lv[0] is the original, each next one half the
 * size, box-filtered. Drawing picks the level just above the size asked
 * for and samples it bilinearly: small trees stop shimmering. A sprite with
 * px == NULL is alpha only (a shadow). */
#define GF_MIPS 6
typedef struct {
    gf_sprite_t lv[GF_MIPS];
    uint8_t     n;
    float       ppm;            /* the original's pixels per metre            */
} gf_mip_t;

bool gf_mip_build(gf_mip_t *m, const gf_sprite_t *src);   /* takes src over  */
void gf_mip_free(gf_mip_t *m);
/* Anchor at (x, y) (float), 'scale' screen px per ORIGINAL px, opacity
 * 0..255, 'tint' 256 = as is. depth/dz: skip pixels where depth < dz; the
 * depth may be smaller than the picture by 1 << dshift (the 3D view keeps
 * it at the ground's resolution), dstride pixels a row. */
void gf_mip_draw(gf_img_t *im, const gf_mip_t *m, float x, float y, float scale, int opa, int tint,
                 const uint16_t *depth, int dstride, int dshift, uint16_t dz, uint16_t fog_c, int fog);
/* while on, gf_mip_draw also writes dz into the depth where it covers at
 * least half (the 3D view's near trees, drawn before the far ones) */
void gf_mip_depth_write(bool on);
/* an alpha-only mip darkening what is under it, strength 0..255 */
void gf_mip_shadow(gf_img_t *im, const gf_mip_t *m, float x, float y, float scale, int strength);

/* draws with its anchor at (x, y) */
void gf_sprite(gf_img_t *im, const gf_sprite_t *s, int x, int y);
/* scaled to 'scale16' sixteenths (nearest), anchor at (x, y); opacity 0..255 */
void gf_sprite_scaled(gf_img_t *im, const gf_sprite_t *s, int x, int y, int scale16, int opa);
/* the rectangle a sprite covers */
void gf_sprite_box(const gf_sprite_t *s, int x, int y, int *x0, int *y0, int *x1, int *y1);

/* --------------------------------------------------------------------------
 * Dirty rectangles (the screen is one canvas; what changed is pushed)
 * -------------------------------------------------------------------------- */

#define GF_MAX_DIRTY  24

typedef struct {
    int16_t x0, y0, x1, y1;
} gf_rect_t;

typedef struct {
    gf_rect_t r[GF_MAX_DIRTY];
    uint8_t   n;
    bool      all;
} gf_dirty_t;

void gf_dirty_reset(gf_dirty_t *d);
void gf_dirty_add(gf_dirty_t *d, int x0, int y0, int x1, int y1);
void gf_dirty_all(gf_dirty_t *d);
int  gf_dirty_area(const gf_dirty_t *d);
/* copy a rectangle from one full-screen buffer to another */
void gf_copy_rect(uint16_t *dst, const uint16_t *src, const gf_rect_t *r);

/* Long renders call this now and then. The app points it at something that
 * lets the other tasks of the core run (the worker's sleep) so that a
 * two-second render never trips the task watchdog; NULL does nothing. */
void gf_set_yield(void (*hook)(void));
/* a millisecond clock for the renderers' own timing (NULL: none) */
void     gf_set_clock(uint32_t (*clock)(void));
uint32_t gf_clock(void);
/* the last render's stages, ms: [0..3], filled by gf_map / gf_view3d */
void gf_prof_set(int i, uint32_t ms);
uint32_t gf_prof_get(int i);
void gf_yield(void);

/* One job in two halves, one per core (aos_hal_worker_split, set by the app
 * while the worker runs). Without a hook, or when it declines, both halves
 * run here, one after the other: the result is the same. */
typedef void (*gf_part_fn)(void *arg, int part);
void gf_set_split(bool (*split)(gf_part_fn fn, void *arg));
void gf_split(gf_part_fn fn, void *arg);

/* Every allocation of the game goes to PSRAM, the small ones too: below
 * 1 KB plain malloc() hands out internal RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL),
 * and the mip levels of the trees alone were ~20 KB of it. free() frees them. */
void *gf_malloc(size_t n);
void *gf_calloc(size_t n, size_t size);
void  gf_free(void *p);
/* what the game holds right now and the most it ever held (bytes): every
 * allocation goes through gf_malloc, so this is the app's whole PSRAM */
size_t gf_mem_used(void);
size_t gf_mem_peak(void);

/* integer sqrt */
int gf_isqrt(int v);
