/*
 * VISOR 3D - drawing a mesh: transform, flat shading, z-buffer. Runs in the
 * worker; writes RGB565 in LVGL's byte order (little-endian), which the
 * P4's blits take as they are (big_endian = false) and the simulator's
 * canvas too.
 */
#pragma once

#include "v3_mesh.h"

typedef struct {
    float yaw, pitch;       /* radians: about the model's up, then the screen's X */
    float roll;             /* radians, in the screen's plane: the two-finger twist */
    float scale;            /* 1 = the model fills about 70% of the smaller side */
    float px, py;           /* pan, in pixels of the target                  */
    int   mode;             /* V3_SOLID or V3_WIRE                           */
    uint32_t bg;            /* background, 0xRRGGBB                          */
} v3_view_t;

enum { V3_SOLID = 0, V3_WIRE, V3_MODES };

/* Per-mesh scratch: the transformed vertices, x y z side by side - one
 * PSRAM cache line per corner instead of three (measured on the watch: most
 * of a frame was the triangle loop waiting on memory). */
typedef struct {
    float *xyz;
    int    nv;
} v3_scratch_t;

bool v3_scratch_alloc(v3_scratch_t *s, int nv);
void v3_scratch_free(v3_scratch_t *s);

/* A rectangle of pixels, x0..x1-1 by y0..y1-1; empty when x1 <= x0. */
typedef struct {
    int x0, y0, x1, y1;
} v3_box_t;

/* Draws into fb (w x h, RGB565 little-endian) with zb (w x h) as depth.
 *
 * Clearing a 720x1280 frame and its depth is 3.7 MB of PSRAM writes, which
 * is both time and the burst docs/MEMORY.md warns about (the panel's
 * refresh reads the same PSRAM). So only what the last frame drew is
 * cleared: 'fb_dirty' is the box the previous frame drew into THIS fb (the
 * rest of it is already the background) and 'zb_dirty' the one it drew into
 * zb (the rest is already far). Both come back as what this frame drew.
 * A box covering the whole target clears it all: that is how a buffer of
 * another size or background starts. In wireframe zb is not touched.
 * Returns the triangles actually drawn. */
int v3_render(const v3_mesh_t *m, const v3_view_t *view, v3_scratch_t *s,
              uint16_t *fb, uint16_t *zb, int w, int h,
              v3_box_t *fb_dirty, v3_box_t *zb_dirty);

/* Microseconds spent since the last call: clear, transform, rasterise. */
void v3_prof(uint32_t out[3]);
