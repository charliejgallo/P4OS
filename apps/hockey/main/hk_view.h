/*
 * HOCKEY - the camera: where a point of the table lands on the screen.
 * The same numbers as tools/blender/hk_common.py: orthographic, looking
 * down 16 degrees from the vertical, a pixel a millimetre, the table's
 * centre at (360, 630) of the upright screen.
 */
#pragma once

#define HK_COS  0.961262f
#define HK_SIN  0.275637f
#define HK_CX   360
#define HK_CY   630

#define HK_RAIL_H   32.0f       /* the rails over the surface */
#define HK_GRIP_Z   52.0f       /* a mallet's knob: where a finger holds it */

static inline float hk_sx(float x) { return (float)HK_CX + x; }
static inline float hk_sy(float y, float z) { return (float)HK_CY + y * HK_COS - z * HK_SIN; }
/* a screen point back to the table, at the height z */
static inline float hk_tx(float sx) { return sx - (float)HK_CX; }
static inline float hk_ty(float sy, float z) { return (sy - (float)HK_CY + z * HK_SIN) / HK_COS; }

/* the windows on the rails (tools/blender/table.py): the two scores on the
 * left rail, the clock and the pause on the right one */
#define HK_WIN_X     49         /* left rail's middle */
#define HK_WIN_Y1    571        /* side 1's score (the far one) */
#define HK_WIN_Y0    671        /* side 0's */
#define HK_WIN_W     48
#define HK_WIN_H     54
#define HK_CLOCK_X   671
#define HK_CLOCK_Y   568
#define HK_CLOCK_W   48
#define HK_CLOCK_H   35
#define HK_PAUSE_X   671
#define HK_PAUSE_Y   674
#define HK_PAUSE_R   34         /* what a finger may hit */
