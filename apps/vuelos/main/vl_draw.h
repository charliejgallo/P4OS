/*
 * VUELOS - drawing into an RGB565 buffer: filled polygons, anti-aliased thick
 * lines and text from a glyph atlas.
 *
 * The pixels are little-endian, LVGL's own RGB565: the P4's blit takes them
 * as they are (the DMA2D copy for a 1:1 frame, big_endian false), and so does
 * the LVGL canvas the frame is also shown through. The watch's panel wanted
 * big-endian and every blend swapped twice; here nothing swaps.
 *
 * Nothing here touches LVGL, so all of it runs in the worker. The one LVGL
 * step, turning a font into glyph masks, is vl_font_bake(), called once from
 * create().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef struct {
    uint16_t *px;
    int       w, h;                 /* the buffer */
    int       cx0, cy0, cx1, cy1;   /* the clip: [cx0, cx1) x [cy0, cy1) */
} vl_fb_t;

static inline uint16_t vl_565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

void vl_fb_clip(vl_fb_t *fb, int x0, int y0, int x1, int y1);
void vl_fb_clip_all(vl_fb_t *fb);
void vl_fill_rect(vl_fb_t *fb, int x, int y, int w, int h, uint32_t rgb);

/* Blend one pixel (alpha 0..255). */
void vl_blend_px(vl_fb_t *fb, int x, int y, uint32_t rgb, int alpha);

/* Filled polygon, even-odd over all its rings (holes come out by themselves).
 * xy: 2 floats per point; ring_n: points per ring. The left and right end of
 * every span are blended by their coverage, which smooths the steep edges. */
void vl_fill_poly(vl_fb_t *fb, const float *xy, const uint32_t *ring_n, int nring, uint32_t rgb);
/* The same; aa false rounds the span ends instead (write-only, cheaper). */
void vl_fill_poly_ex(vl_fb_t *fb, const float *xy, const uint32_t *ring_n, int nring, uint32_t rgb, bool aa);

/* A polyline of width w (px), round joins and ends, anti-aliased. Widths
 * under one pixel are drawn one pixel wide and fainter. alpha 0..255. */
void vl_polyline(vl_fb_t *fb, const float *xy, int n, float w, uint32_t rgb, int alpha);

/* Filled disc with an anti-aliased edge. */
void vl_disc(vl_fb_t *fb, float cx, float cy, float r, uint32_t rgb, int alpha);

/* ---------------------------------------------------------------------------
 * Text
 * ------------------------------------------------------------------------- */

#define VL_GLYPH_FIRST 0x20
#define VL_GLYPH_LAST  0xFF
#define VL_GLYPHS      (VL_GLYPH_LAST - VL_GLYPH_FIRST + 1)
#define VL_HALO        2            /* px of halo around every glyph */

typedef struct {
    int16_t  adv;                   /* advance, px */
    int16_t  w, h;                  /* the masks, halo margin included */
    uint8_t *a;                     /* the glyph's coverage */
    uint8_t *halo;                  /* the same, grown by the halo */
} vl_glyph_t;

typedef struct {
    vl_glyph_t g[VL_GLYPHS];
    uint8_t   *atlas;               /* every mask, one block */
    int        line_h;
    int        pad, top;            /* where the glyph sits in its masks */
    bool       ok;
} vl_font_t;

/* LVGL thread only. */
bool vl_font_bake(vl_font_t *f, const lv_font_t *font);
/* A font half the size of src, every mask averaged 2x2: the labels of a
 * half-resolution render (the smallest font the firmware has is 14 px, and
 * the half render wants 8 to 12). Any thread. */
bool vl_font_half(vl_font_t *dst, const vl_font_t *src);
void vl_font_free(vl_font_t *f);

/* Next codepoint of a UTF-8 string; unknown ones come back as '?'. */
uint32_t vl_utf8_next(const char **s);

int vl_text_width(const vl_font_t *f, const char *s);

/* Horizontal text, its top-left corner at x, y; halo first. */
void vl_text(vl_fb_t *fb, const vl_font_t *f, int x, int y, const char *s,
             uint32_t rgb, uint32_t halo_rgb, bool halo);

/* One glyph centred at (cx, cy), turned by angle (radians, clockwise on the
 * screen). pass 0 draws its halo, pass 1 the glyph. */
void vl_glyph_rot(vl_fb_t *fb, const vl_font_t *f, uint32_t cp, float cx, float cy,
                  float angle, uint32_t rgb, int pass);
