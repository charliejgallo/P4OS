/*
 * TANQUES - the two views.
 *
 * The battle (third person): the whole map is drawn once, from the fixed
 * orthographic camera, into a picture bigger than the screen with its depth
 * beside it ("the world"). A frame is a window of it, copied, with the
 * sprites (tanks, trees, stones, puffs) drawn over it, each pixel tested
 * against the world's depth. A crater draws again only the part of the
 * world it changed.
 *
 * The gunner (first person): drawn whole every frame at half the screen
 * each way - the sky, the hills far off, the ground's triangles, the things
 * as sprites scaled by distance, the turret and the barrel - and doubled.
 *
 * Both go into a screen-sized RGB565 buffer, in two halves of the rows that
 * the app's worker can put on the two cores (tq_scene_set_split).
 */
#pragma once

#include "tq_game.h"

#include <stddef.h>

enum { TQ_VIEW_TOP = 0, TQ_VIEW_FP };

typedef struct {
    int     mode;
    int     w, h;                   /* the screen */
    float   cx, cy;                 /* top: the ground point the view centres on */
    tq_v3_t eye;                    /* fp: the gunner's eye */
    float   yaw, pitch;             /* fp: heading (CCW from +X) and the eye's pitch, rad */
    int     me;                     /* fp: whose eye (that tank is not drawn) */
    bool    hp_bars;
    tq_state_t st;
} tq_req_t;

typedef struct {
    uint32_t total_us;
    uint32_t world_us;              /* top: the world drawn again (a crater, the first frame) */
    uint32_t base_us;               /* top: the window copied; fp: sky and ground */
    uint32_t things_us;             /* the sprites, the puffs, the marks */
    uint32_t up_us;                 /* fp: the doubling */
    int      tris;                  /* fp: triangles drawn */
    int      sprites;
} tq_prof_t;

bool   tq_scene_init(void);
void   tq_scene_free(void);
size_t tq_scene_bytes(void);
void   tq_scene_set_split(bool (*split)(void (*fn)(void *arg, int part), void *arg));

/* The world must be drawn again: all of it, or the vertices of a box */
void tq_scene_world_all(void);
void tq_scene_world_dirty(tq_vbox_t b);

void tq_scene_render(const tq_req_t *r, const tq_terrain_t *t, uint16_t *fb, tq_prof_t *prof);

/* The battle view's mapping, for touches and the HUD. The window keeps
 * inside the world: tq_scene_top_clamp moves the centre so it does. */
void tq_scene_top_clamp(const tq_terrain_t *t, int w, int h, float *cx, float *cy);
void tq_scene_top_project(const tq_terrain_t *t, float cx, float cy, int w, int h, tq_v3_t p, float *sx, float *sy);
bool tq_scene_top_pick(const tq_terrain_t *t, float cx, float cy, int w, int h, float sx, float sy,
                       float *wx, float *wy);

/* The gunner's principal point and focal length on the screen */
void tq_scene_fp_frame(int w, int h, float *cx, float *cy, float *f);
