/*
 * TANQUES - pixels: triangles, sprites, puffs, the doubling.
 *
 * Everything writes RGB565 in LVGL's byte order (little-endian) and tests a
 * 16-bit "nearness" (0..32767, larger is nearer: 0 is the sky, the
 * background, the far plane). The battle view's nearness is linear in the
 * depth (TQ_QS units a metre); the first person's is 1/z (TQ_QK / z), which
 * interpolates exactly across a triangle on the screen.
 *
 * Every call clips to the target's box, so a frame can be drawn in two
 * halves of its rows, one per core.
 */
#pragma once

#include "tq_view.h"

#include <stddef.h>

#define TQ_QMAX   32767
#define TQ_QS     512.0f            /* battle view: nearness units a metre (64 m in range) */
#define TQ_QZS    16                /* ... a sprite's depth step (1/32 m) */
#define TQ_QK     (0.3f * 32767.0f) /* first person: nearness = TQ_QK / z, z >= 0.3 m */

typedef struct {
    uint16_t *px;           /* colour */
    uint16_t *z;            /* nearness, same stride; NULL: no depth */
    int w, h, stride;
    int cx0, cy0, cx1, cy1; /* the clip box, x0..x1-1 by y0..y1-1 */
} tq_tgt_t;

typedef struct {
    float x, y, q;          /* pixels, nearness */
    float r, g, b;          /* 1..254 */
    float u, v;             /* the detail texture's texels */
} tq_vtx_t;

#define TQ_TRI_DETAIL 1     /* modulate by the ground's grain texture */
#define TQ_TRI_CULL   2     /* skip clockwise (on screen) triangles */

void tq_tri(const tq_tgt_t *t, const tq_vtx_t *a, const tq_vtx_t *b, const tq_vtx_t *c, int flags);

/* A sprite of the pack: four planes (tanques_art.py), each row only its
 * covered run (tools/pack.py). Z may be NULL: the whole sprite at its
 * anchor's depth. */
typedef struct {
    uint16_t x0, len;
    uint32_t off;
} tq_row_t;

typedef struct {
    int16_t w, h, ax, ay;
    const tq_row_t *rows;
    const uint8_t *L, *A, *I, *Z;
    const uint8_t *U;           /* the badge's cell (u * 16 + v), or NULL */
} tq_spr_t;

/* The colours of a paint: 16 regions x 256 shades, RGB565. A tank's has
 * its emblem too: where a badge pixel's cell is set, region 7's colour. */
typedef struct {
    uint16_t c[16 * 256];
    uint16_t emblem[16];
    bool     emblem_on;
} tq_lut_t;

/* The colour of a sprite's pixel k */
static inline uint16_t tq_spr_px(const tq_spr_t *s, const tq_lut_t *lut, int k)
{
    int id = s->I[k];
    if (id == 6 && s->U && lut->emblem_on) {
        int uv = s->U[k];
        if (lut->emblem[15 - (uv & 15)] & (0x8000u >> (uv >> 4))) id = 7;
    }
    return lut->c[(id << 8) | s->L[k]];
}

void tq_lut_build(tq_lut_t *lut, const uint32_t rgb[16]);

/* 1:1 with the anchor at (x, y). Its nearness at the anchor is q0, and
 * each step of the sprite's depth plane moves it qs. zt: the nearness to
 * test against, laid out like the target (NULL: none); zwrite: the sprite's
 * solid pixels write t->z. alpha 0..256. */
void tq_spr_draw(const tq_tgt_t *t, const tq_spr_t *s, const tq_lut_t *lut, int x, int y,
                 int q0, int qs, int alpha, const uint16_t *zt, int zstride, bool zwrite);
/* Scaled (nearest), the anchor at (x, y) */
void tq_spr_scaled(const tq_tgt_t *t, const tq_spr_t *s, const tq_lut_t *lut, float x, float y,
                   float scale, int q0, int qs, int alpha);

/* A shaded ball (a puff of the explosions, a shell, a clod): colour
 * 0xRRGGBB, radius in pixels, alpha 0..256, tested at nearness q */
void tq_ball(const tq_tgt_t *t, float x, float y, float r, uint32_t rgb, int alpha, int q,
             const uint16_t *zt, int zstride);
/* A soft dark ellipse on the ground (a shadow), strength 0..256 */
void tq_shadow(const tq_tgt_t *t, float x, float y, float rx, float ry, int strength);
/* A flat disc, for the path's dots and the aim's marks */
void tq_disc(const tq_tgt_t *t, float x, float y, float r, uint32_t rgb, int alpha);
void tq_ring(const tq_tgt_t *t, float x, float y, float rx, float ry, float width, uint32_t rgb, int alpha);
/* A quad tinted over the picture, depth-tested with a bias (the reach) */
void tq_tint_quad(const tq_tgt_t *t, const float xy[8], const float q[4], uint32_t rgb, int alpha,
                  const uint16_t *zt, int zstride, int bias);
void tq_rect(const tq_tgt_t *t, int x, int y, int w, int h, uint32_t rgb, int alpha);

/* src (w x h) doubled into dst (stride dstride) by bilinear filtering,
 * rows y0..y1-1 of the destination */
void tq_up2(uint16_t *dst, int dstride, const uint16_t *src, int w, int h, int y0, int y1);
/* The same from a window of a bigger source: dst's W columns (even) come
 * from src's columns sx0.., its rows y0..y1-1 from rows sy0 + y/2 */
void tq_up2_window(uint16_t *dst, int dstride, int W, const uint16_t *src, int sw, int sh, int sstride, int sx0,
                   int sy0, int y0, int y1);
/* The same for the columns X0..X1-1 only (X0 even) */
void tq_up2_rect(uint16_t *dst, int dstride, const uint16_t *src, int sw, int sh, int sstride, int sx0, int sy0,
                 int X0, int X1, int y0, int y1);
/* The depth the next draws test against (zt) is 2^shift times smaller than
 * the target: the battle view's world is kept at half size */
void tq_gfx_zshift(int shift);

static inline uint16_t tq_565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

static inline uint16_t tq_blend(uint16_t d, uint16_t s, int a32)
{
    uint32_t dd = (d | ((uint32_t)d << 16)) & 0x07E0F81Fu;
    uint32_t ss = (s | ((uint32_t)s << 16)) & 0x07E0F81Fu;
    uint32_t r = (dd + (((ss - dd) * (uint32_t)a32) >> 5)) & 0x07E0F81Fu;
    return (uint16_t)(r | (r >> 16));
}

/* The grain texture under the ground's colours (64 x 64, 0..256) */
void tq_gfx_init(void);
