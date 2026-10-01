/*
 * MONSTER HOP - the zones' airborne bits over a frame
 *
 * From the desktop's finishing touches: embers over the valley, plankton
 * over the bay, fireflies in the woods... drawn in the frame's own pixels
 * before the HUD. The desktop's bloom and vignette are not here: each is a
 * pass over every pixel of the frame, and on the P4 that time is the frame
 * rate (a 1280 x 720 frame is 0.9 Mpx; the bits are ~90 small discs).
 */
#pragma once

#include "mh_gfx.h"
#include "mh_world.h"

#include <stdbool.h>
#include <stdint.h>

#define MHP_BITS 90

typedef struct {
    struct {
        float x, y;             /* level-plane pixels (they move with it) */
        float vx, vy;
        float t, life;
        float phase;
        uint8_t kind;
    } bit[MHP_BITS];
    int      n;
    uint32_t rnd;
    float    clock;
} mh_post_t;

/* the zone's bits, once a frame (cam = the view's top-left, w x h its size) */
void mhp_step(mh_post_t *p, int zone, int cam_x, int cam_y, int w, int h, float dt);
/* and drawn into the frame, a band at a time (im's clip) */
void mhp_draw(const mh_post_t *p, int zone, mh_img_t *im, int cam_x, int cam_y, int w, int h);
void mhp_free(mh_post_t *p);
