/*
 * PLANEO - the flight: an arcade plane and the camera that follows it.
 *
 * The plane flies where its nose points. The stick rolls and pitches it,
 * a bank turns it (the more banked, the tighter, the slower the tighter),
 * the throttle and the climb set the speed, and below the stall speed the
 * nose drops by itself. Let go of the stick and the wings come level.
 * The pedals (the rudder) swing the nose sideways without banking: a slow
 * flat turn, for lining up with a ring.
 * The world repeats, so the plane's place is kept within one copy of it.
 */
#pragma once

#include "vu_plane.h"
#include "vu_render.h"
#include "vu_world.h"

#include <stdbool.h>

enum { VF_CHASE = 0, VF_COCKPIT, VF_VIEWS };

typedef struct {
    float p[3];
    float f[3], r[3], u[3];
    float v;                    /* airspeed, m/s */
    float thr;                  /* the engine, 0..1, following in_thr */
    /* what the hands ask */
    float in_x, in_y;           /* the stick: right rolls right, up raises the nose; -1..1 */
    float in_thr;               /* 0..1 */
    float in_rud;               /* the pedals: right turns the nose right, wings level; -1..1 */
    /* read back */
    float bank, pitch, heading;
    float agl;                  /* metres over what is under it */
    float ground;
    bool  crashed;
    float crash_t;
    float stall;                /* 0..1, how deep in the stall */
    /* the camera */
    int   view;
    float cam_d[3];
    float cam_roll;
    float cam_eye[3];
    bool  cam_ok;
} vf_plane_t;

void vf_reset(vf_plane_t *pl, const vw_world_t *w);
void vf_step(vf_plane_t *pl, const vw_world_t *w, float dt);
void vf_camera(vf_plane_t *pl, const vw_world_t *w, float dt, float time, vr_cam_t *cam);
void vf_pose(const vf_plane_t *pl, vp_pose_t *pose);
/* Flies by itself (the demo and the board's measurement): turns gently,
 * follows the ground at a height, sweeps over the world */
void vf_autopilot(vf_plane_t *pl, const vw_world_t *w, float t);
/* Flies towards a point (the simulator's tests of the missions) */
void vf_autopilot_to(vf_plane_t *pl, const vw_world_t *w, const float target[3]);
