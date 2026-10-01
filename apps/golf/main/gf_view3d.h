/*
 * GOLF - the 3D view from behind the golfer
 *
 * A voxel-space renderer (the Comanche technique) over the hole's height
 * grid: for every screen column a ray walks the ground from near to far and
 * paints the part of the column each sample rises above, so hills hide what
 * is behind them and the green can sit up on its plateau. The ground's
 * colour is the map renderer's own texture (gf_map_albedo), lit here with a
 * light that stays behind the camera whatever way the player aims, and
 * fogged with distance. Then the sky with clouds on a plane far above, the
 * hills on the horizon, and the trees and the flag as billboards tested
 * against the depth of the ground.
 *
 * It runs once per shot (the camera does not move while the golfer swings),
 * so it can afford to be thorough. The golfer is a sprite rendered in Blender
 * with THIS camera (tools/blender/SPEC.md), which is why it stands on the
 * ground this draws.
 */
#pragma once

#include "gf_world.h"
#include "gf_gfx.h"

typedef struct {
    float x, y, z;          /* position, metres                              */
    float yaw;              /* heading, radians; 0 = +y                      */
    float pitch;            /* looking down by this much, radians            */
    float f;                /* focal length, pixels                          */
    float cx, cy;           /* principal point                               */
    /* derived */
    float fx, fy, rx, ry;   /* forward and right, on the ground              */
    float cp, sp;
} gf_cam_t;

/* The picture the swing camera makes. The golfer was rendered in Blender
 * with the watch's 368x448 camera (50 degrees of vertical field) at
 * GF_ART_SCALE times the pixels, so on P4OS the focal length is that many
 * times the watch's whatever the orientation; what changes is where the
 * principal point sits on the screen (gf_cam_frame), and the art is placed
 * by its offset from it (GF_ART_PPX, GF_ART_PPY: the centre of the frame
 * the golfer was rendered in). */
#define GF_ART_SCALE    2
#define GF_CAM_F        (GF_ART_SCALE * 480.3737f)   /* 224 / tan 25 deg */
#define GF_ART_PPX      (GF_ART_SCALE * 184)
#define GF_ART_PPY      (GF_ART_SCALE * 224)
void gf_cam_frame(float cx, float cy);
void gf_cam_frame_get(float *cx, float *cy);

/* The swing camera: behind the ball at (bx, by) looking along 'aim'. */
void gf_cam_swing(gf_cam_t *c, const gf_world_t *w, float bx, float by, float aim);
/* Any camera, with the focal length and principal point of gf_cam_frame */
void gf_cam_set(gf_cam_t *c, float x, float y, float z, float yaw, float pitch);

/* World point to screen; false when it is behind the camera. zc = depth. */
bool gf_cam_project(const gf_cam_t *c, float x, float y, float z, float *sx, float *sy, float *zc);

typedef struct {
    const uint16_t *tex;    /* gf_map_albedo() output                        */
    int             tw, th;
    float           mpp;
} gf_albedo_t;

/* Where the picture goes. The ground and the sky, which are smooth, are
 * drawn at 1/div of the screen into 'low' (with their depth, decimetres,
 * 0xFFFF = sky) and scaled up into 'out'; the trees and the flag, which are
 * the Blender art, are drawn over it at the screen's own resolution, tested
 * against that depth. */
typedef struct {
    uint16_t *out;          /* W x H, dithered                                */
    int       W, H;
    uint16_t *low;          /* (W / div) x (H / div)                          */
    uint16_t *depth;        /* the same size as low                           */
    int       div;          /* 1 or 2                                         */
} gf_v3d_target_t;

void gf_view3d_render(const gf_world_t *w, const gf_cam_t *c, const gf_albedo_t *alb,
                      const gf_v3d_target_t *t);

