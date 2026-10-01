/*
 * P4OS (from AmoledOS) - Cameras: pictures to screen pixels, scaled.
 *
 * Both converters scale by nearest neighbour from a source rectangle (the
 * crop) to an output of ow x oh, and write RGB565 in LVGL's byte order
 * (native, little-endian), so the result goes to an lv_canvas untouched.
 * They run in the camera's thread, right after the decoder, into the frame
 * the UI will take.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "aos_hal.h"

typedef struct {
    int sx, sy, sw, sh;         /* the crop, in source pixels */
    int ow, oh;                 /* the output */
} cam_geom_t;

typedef enum { CAM_FIT = 0, CAM_FILL } cam_mode_t;

/* Where a src_w x src_h picture goes in a box of box_w x box_h: FIT keeps
 * all of it (the output is smaller than the box on one side), FILL covers
 * the box and crops the middle. */
void cam_geometry(int src_w, int src_h, int box_w, int box_h, cam_mode_t mode, cam_geom_t *g);

/* The colour tables, once, before any thread converts. */
bool cam_conv_init(void);

/* A column map per thread: grows to the widest output it has seen. */
typedef struct {
    uint16_t *xmap;
    int       cap;
} cam_conv_t;

void cam_conv_release(cam_conv_t *c);

/* The whole output into out (ow x oh, packed). */
bool cam_conv_i420(cam_conv_t *c, const aos_h264_pic_t *pic, const cam_geom_t *g, uint16_t *out);
bool cam_conv_rgb565(cam_conv_t *c, const uint16_t *src, int src_stride, const cam_geom_t *g, uint16_t *out);
