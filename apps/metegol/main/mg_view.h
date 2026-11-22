/*
 * METEGOL - the camera: where a point of the table lands on the screen.
 * The same numbers as tools/blender/mg_common.py: orthographic, looking
 * down 32 degrees from the vertical, 0.8 pixels a millimetre, the field's
 * centre at (640, 450).
 */
#pragma once

#define MG_S    0.8f
#define MG_COS  0.848048f
#define MG_SIN  0.529919f
#define MG_CX   640
#define MG_CY   450

#define MG_WALL_T   26.0f       /* the long bands' thickness */
#define MG_WALL_H   72.0f

static inline float mg_sx(float x) { return (float)MG_CX + x * MG_S; }
static inline float mg_sy(float y, float z) { return (float)MG_CY + (y * MG_COS - z * MG_SIN) * MG_S; }
/* a screen row back to the field's y (on the ground) */
static inline float mg_gy(float sy) { return (sy - (float)MG_CY) / (MG_S * MG_COS); }
static inline float mg_gx(float sx) { return (sx - (float)MG_CX) / MG_S; }
