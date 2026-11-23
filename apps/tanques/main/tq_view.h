/*
 * TANQUES - the cameras and the map's size, shared by the renderers and the
 * game. tools/blender/tanques_art.py renders the sprites with the same
 * numbers: change one, change both.
 */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* The map: a square of up to TQ_MAP_MAX metres (the battle picks 28, 32, 36
 * or 44), heights on a grid of TQ_CELL */
#define TQ_MAP_MAX   44.0f
#define TQ_CELL      0.5f
#define TQ_NMAX      88                     /* cells per side, at most */
#define TQ_NVMAX     (TQ_NMAX + 1)
#define TQ_BASE_Z    (-1.4f)                /* the diorama's floor, under the map */

/* The battle's camera (third person): orthographic, from the south-south-
 * east, looking down at 55 degrees, 32 px per metre on the screen */
#define TQ_TOP_YAW   20.0f
#define TQ_TOP_ELEV  55.0f
#define TQ_TOP_S     32.0f

/* The far sprites of the first-person view: 8 degrees above the horizon,
 * 24 px per metre as packed (rendered at 48, halved by tools/pack.py),
 * indexed by the yaw relative to the view */
#define TQ_LOW_ELEV  8.0f
#define TQ_LOW_S     24.0f

/* The gunner's view: focal length in pixels of the whole screen, looking
 * this much down; the eye is each tank's (tq_data.c, TQ_TANK) */
#define TQ_FP_F      620.0f
#define TQ_FP_PITCH  (-7.0f)

/* Towards the sun: behind the battle camera, on its left */
#define TQ_SUN_AZ    225.0f
#define TQ_SUN_ELEV  52.0f

#define TQ_YAWS      32                     /* sprite yaws, 11.25 degrees apart */
#define TQ_PITCHES   14                     /* barrel frames: -5..60, 5 apart */

#define TQ_PI        3.14159265f
#define TQ_DEG       (TQ_PI / 180.0f)

typedef struct {
    float x, y, z;
} tq_v3_t;

static inline tq_v3_t tq_v3(float x, float y, float z)
{
    tq_v3_t v = { x, y, z };
    return v;
}

static inline float tq_dot(tq_v3_t a, tq_v3_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline float tq_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The sun, as a unit vector towards it */
static inline tq_v3_t tq_sun(void)
{
    float a = TQ_SUN_AZ * TQ_DEG, e = TQ_SUN_ELEV * TQ_DEG;
    return tq_v3(cosf(e) * cosf(a), cosf(e) * sinf(a), sinf(e));
}

/* The battle camera's axes: F the view direction, R the screen's right, U
 * the screen's up (tanques_art.py, view_axes) */
static inline void tq_top_axes(tq_v3_t *F, tq_v3_t *R, tq_v3_t *U)
{
    float y = TQ_TOP_YAW * TQ_DEG, e = TQ_TOP_ELEV * TQ_DEG;
    *F = tq_v3(-sinf(y) * cosf(e), cosf(y) * cosf(e), -sinf(e));
    *R = tq_v3(cosf(y), sinf(y), 0);
    *U = tq_v3(-sinf(y) * sinf(e), cosf(y) * sinf(e), cosf(e));
}

/* The gunner's eye in the world, for a tank at (x, y, z) with its turret at
 * yaw, the eye at (ex, ey, ez) in the turret's frame */
static inline tq_v3_t tq_fp_eye(float x, float y, float z, float yaw, float ex, float ey, float ez)
{
    float c = cosf(yaw), s = sinf(yaw);
    return tq_v3(x + c * ex - s * ey, y + s * ex + c * ey, z + ez);
}

/* A yaw in radians to the nearest of the TQ_YAWS sprite frames */
static inline int tq_yaw_index(float yaw)
{
    float k = yaw * (TQ_YAWS / (2.0f * TQ_PI));
    int i = (int)floorf(k + 0.5f);
    i %= TQ_YAWS;
    return i < 0 ? i + TQ_YAWS : i;
}
