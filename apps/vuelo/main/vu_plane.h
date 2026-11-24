/*
 * PLANEO - the plane: a small stunt plane made of triangles by code, drawn
 * in real time over the world (seen from behind it banks and climbs as it
 * really does), with its shadow on the ground and the propeller's blur.
 */
#pragma once

#include "vu_render.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float p[3];                 /* where, metres */
    float f[3], r[3], u[3];     /* forward, right, up */
    float prop;                 /* the propeller's speed, 0..1 */
    float ground;               /* the ground's height under it (the shadow), metres */
    uint32_t paint[3];          /* the body, the wings and tail, the trim: 0xRRGGBB */
} vp_pose_t;

#define VP_MAXT 220

typedef struct {
    float    x[3], y[3];
    float    depth;
    uint16_t col;
    uint8_t  alpha;             /* 32 solid */
} vp_tri_t;

typedef struct {
    vp_tri_t t[VP_MAXT];
    int      n;
    /* the propeller's disc: centre and its two half axes on screen */
    bool     disc;
    float    dcx, dcy, dax, day, dbx, dby;
} vp_draw_t;

void vp_build(void);
/* The pose into screen triangles, far first (once a frame, before the
 * halves draw); sun: the world's, towards the sun */
void vp_prepare(vp_draw_t *d, const vp_pose_t *pose, const vr_proj_t *p, const float sun[3], uint32_t amb_rgb);
/* Rows y0..y1-1 of the output */
void vp_draw(const vp_draw_t *d, uint16_t *out, int W, int H, int y0, int y1);

/* A flat-coloured triangle, clipped to rows y0..y1-1; alpha 32 is solid */
void vp_fill(uint16_t *out, int W, int y0, int y1, const float *x, const float *y, uint16_t col, int alpha);
