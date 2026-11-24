/*
 * PLANEO - the 3D: the world drawn in columns, the sky, the clouds.
 *
 * The camera sees the world upright first: a column of the picture is a
 * slice of the ground marched from near to far (lines of equal depth, all
 * columns at once), each sample's height projected and drawn from where
 * the column was left up to it - the "voxel space" way. Looking up or down
 * moves the horizon (a shear); the bank is a turn of the whole picture,
 * done at the end into the output frame. So the upright picture is drawn
 * as big as the turned frame needs (up to its diagonal) and the output
 * samples it.
 *
 * Every pass is split in two: the columns' halves for the ground and the
 * sky, the output's rows for the turn and what is drawn over it (the
 * plane, the rings). With vr_set_split() the halves go to both cores.
 *
 * RGB565 in LVGL's byte order (little-endian), as the canvas and the panel.
 */
#pragma once

#include "vu_world.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float x, y, z;              /* the eye, metres (x east, y north, z up) */
    float yaw;                  /* radians, 0 looking along +x, counter-clockwise */
    float pitch;                /* radians, up + (kept within about +-55 degrees) */
    float roll;                 /* radians, right wing down + */
    float tanfov;               /* tan of half the horizontal field of view */
    float time;                 /* seconds: the water, the clouds' drift */
} vr_cam_t;

/* How a point of the world lands on the output frame; filled by vr_setup */
typedef struct {
    float cx, cy, cz;           /* the eye */
    float fx, fy, rx, ry;       /* forward and right, on the ground */
    float f;                    /* focal length, output pixels */
    float hor;                  /* the horizon's row in the upright picture */
    float cxu, cyu;             /* the upright picture's centre */
    float c, s;                 /* the turn */
    float cxo, cyo;             /* the output's centre */
    int   W, H;                 /* output */
    int   Wu, Hu;               /* upright */
} vr_proj_t;

typedef struct {
    uint32_t us_sky, us_ground, us_turn, us_over, us_total;
    uint32_t samples, spans, lines;
    int      wu, hu;
} vr_prof_t;

/* Something drawn over the turned picture, a band of rows at a time */
typedef void (*vr_over_fn)(void *arg, uint16_t *out, int W, int H, int y0, int y1);

/* The buffers for output frames up to maxW x maxH */
bool vr_init(int maxW, int maxH);
void vr_free(void);
void vr_set_split(vw_split_fn split);

void vr_setup(const vr_cam_t *cam, int W, int H, vr_proj_t *p);
/* A point of the world on the output: false if behind or too near. *z is
 * the depth (metres), *scale pixels a metre at that depth. */
bool vr_project(const vr_proj_t *p, float x, float y, float z, float *sx, float *sy, float *depth);

void vr_frame(const vw_world_t *w, const vr_cam_t *cam, const vr_proj_t *p, uint16_t *out,
              vr_over_fn over, void *over_arg, vr_prof_t *prof);

/* 565 helpers */
static inline uint16_t vr_565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

/* d towards s by a32/32 */
static inline uint16_t vr_blend(uint16_t d, uint16_t s, int a32)
{
    uint32_t dd = (d | ((uint32_t)d << 16)) & 0x07E0F81Fu;
    uint32_t ss = (s | ((uint32_t)s << 16)) & 0x07E0F81Fu;
    uint32_t r = (dd + (((ss - dd) * (uint32_t)a32) >> 5)) & 0x07E0F81Fu;
    return (uint16_t)(r | (r >> 16));
}

/* the time, microseconds (the HAL's clock, or the Mac's in the bench) */
uint32_t vr_now_us(void);
