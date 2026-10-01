/*
 * Claudito - the two scenes
 *
 * The background is drawn once per scene (and once more when night falls)
 * into a separate buffer and then copied onto the stage on every frame.
 * Redrawing the parquet plank by plank fourteen times a second makes no sense
 * at all when a memcpy does the same thing.
 */
#pragma once

#include "cl_pixel.h"

typedef enum {
    CL_SCENE_HOME = 0,      /* living room: parquet, window, plant, rug */
    CL_SCENE_PARK,          /* yard: grass, tree, fence, sun            */
    CL_SCENE_COUNT
} cl_scene_id_t;

/* Row of the stage where the feet rest, the same in both scenes so changing
 * setting does not move the critter. */
#define CL_FLOOR_Y      66

/* The whole still background. 'night' bakes the night in: a night sky with
 * its moon and stars, the ground darkened, the lamp lit. It used to be a
 * darkening pass over the stage on every frame. */
void cl_scene_draw(cl_buf_t *b, cl_scene_id_t scene, bool night);

/* What moves by itself (clouds, sun, butterfly, twinkling stars, fireflies)
 * goes on top of the already copied background, in areas where it covers
 * nothing: that way the background stays a memcpy. Writes up to 'max'
 * rectangles of what it drew into 'out' (drawing coordinates) and returns how
 * many, so that only those get presented. */
int cl_scene_anim(cl_buf_t *b, cl_scene_id_t scene, int frame, bool night,
                  cl_rect_t *out, int max);

const char *cl_scene_name(cl_scene_id_t scene);
