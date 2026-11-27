/*
 * CIELO - drawing into an RGB565 buffer: discs, lines, spans, rounded
 * boxes and text from glyph masks.
 *
 * The buffer is addressed in the SCREEN's coordinates through two strides:
 * pixel (x, y) is px[off + x * sx + y * sy]. Upright on the panel's own
 * buffer that is sx = 1, sy = width; turned (the screen lying down, drawing
 * straight into the panel's free buffer, which keeps the panel's upright
 * orientation) one of the strides is the panel's width and the other +-1.
 * So the sky is drawn turned at no cost: no PPA pass and no copy of the
 * frame (the lying-down ceiling of docs/APPS-P4.md is the PPA's 42-62 ms).
 *
 * Night mode: every colour a primitive is given goes through cl_tint()
 * first, which turns it into a red of the same brightness.
 *
 * The glyphs are Vuelos' recipe (vl_draw.c): the LVGL font drawn once into
 * an L8 canvas, each glyph's coverage kept with a grown copy for a halo.
 * Nothing here touches LVGL except cl_font_bake(), from create().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef struct {
    uint16_t *px;
    int32_t   off, sx, sy;          /* pixel (x, y) = px[off + x * sx + y * sy] */
    int       w, h;                 /* the screen */
    int       cx0, cy0, cx1, cy1;   /* the clip: [cx0, cx1) x [cy0, cy1) */
    bool      night;
} cl_fb_t;

static inline uint16_t cl_565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

static inline uint16_t *cl_px(const cl_fb_t *fb, int x, int y)
{
    return fb->px + fb->off + x * fb->sx + y * fb->sy;
}

/* A plain buffer of w x h, rows of w. */
void cl_fb_plain(cl_fb_t *fb, uint16_t *px, int w, int h);
/* The panel's buffer (pw x ph, upright) seen as a screen turned by rot
 * degrees (0, 90, 270; 180 too). */
void cl_fb_panel(cl_fb_t *fb, uint16_t *px, int pw, int ph, int rot);
void cl_fb_clip(cl_fb_t *fb, int x0, int y0, int x1, int y1);

uint32_t cl_tint(const cl_fb_t *fb, uint32_t rgb);
uint32_t cl_mix(uint32_t a, uint32_t b, float t);

void cl_fill_rect(cl_fb_t *fb, int x, int y, int w, int h, uint32_t rgb);
/* a horizontal run with soft ends: [xa, xb) in fractional pixels */
void cl_span(cl_fb_t *fb, int y, float xa, float xb, uint32_t rgb, int alpha);
void cl_blend_px(cl_fb_t *fb, int x, int y, uint32_t rgb, int alpha);
void cl_add_px(cl_fb_t *fb, int x, int y, uint32_t rgb, int amount);

/* A filled disc with an anti-aliased edge; under a pixel of radius it
 * fades instead of shrinking. */
void cl_disc(cl_fb_t *fb, float cx, float cy, float r, uint32_t rgb, int alpha);
/* A soft glow, added: brightest in the middle, gone at r. */
void cl_glow(cl_fb_t *fb, float cx, float cy, float r, uint32_t rgb, int amount);
/* A circle's outline of width w. */
void cl_ring(cl_fb_t *fb, float cx, float cy, float r, float w, uint32_t rgb, int alpha);
/* A line of width w (anti-aliased). */
void cl_line(cl_fb_t *fb, float x0, float y0, float x1, float y1, float w, uint32_t rgb, int alpha);
/* A box with round corners, blended. */
void cl_rbox(cl_fb_t *fb, int x, int y, int w, int h, int r, uint32_t rgb, int alpha);
/* The lit part of a disc: the Moon. k is the illuminated fraction, ang
 * the screen angle (radians, x right, y down) towards the Sun. */
void cl_phase_disc(cl_fb_t *fb, float cx, float cy, float r, float k, float ang, uint32_t lit, uint32_t dark);

/* ---------------------------------------------------------------------------
 * Text
 * ------------------------------------------------------------------------- */

#define CL_GLYPH_FIRST 0x20
#define CL_GLYPH_LAST  0xFF
#define CL_GLYPHS      (CL_GLYPH_LAST - CL_GLYPH_FIRST + 1)
#define CL_EXTRA       16           /* symbols from another font */
#define CL_HALO        2

typedef struct {
    int16_t  adv;
    int16_t  w, h;
    uint8_t *a;
    uint8_t *halo;
} cl_glyph_t;

typedef struct {
    cl_glyph_t g[CL_GLYPHS];
    cl_glyph_t x[CL_EXTRA];         /* the extra symbols */
    uint32_t   xcp[CL_EXTRA];
    int        nx;
    uint8_t   *atlas;
    int        line_h;
    int        pad, top;
    bool       ok;
} cl_font_t;

/* LVGL thread only. extra: codepoints to take from sym (may be NULL). */
bool cl_font_bake(cl_font_t *f, const lv_font_t *font, const lv_font_t *sym, const uint32_t *extra, int nextra);
void cl_font_free(cl_font_t *f);

uint32_t cl_utf8_next(const char **s);
int cl_text_width(const cl_font_t *f, const char *s);
/* top-left at x, y; halo (in halo_rgb) under it if halo */
void cl_text(cl_fb_t *fb, const cl_font_t *f, int x, int y, const char *s, uint32_t rgb, uint32_t halo_rgb, bool halo);
/* the same, centred on cx */
void cl_text_c(cl_fb_t *fb, const cl_font_t *f, int cx, int y, const char *s, uint32_t rgb, uint32_t halo_rgb,
               bool halo);
