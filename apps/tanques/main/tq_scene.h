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
#include "tq_gfx.h"

#include <stddef.h>

enum { TQ_VIEW_TOP = 0, TQ_VIEW_FP, TQ_VIEW_SHOW };

typedef struct {
    int     mode;
    int     w, h;                   /* the screen */
    float   cx, cy;                 /* top: the ground point the view centres on */
    tq_v3_t eye;                    /* fp: the gunner's eye */
    float   yaw, pitch;             /* fp: heading (CCW from +X) and the eye's pitch, rad */
    int     me;                     /* fp: whose eye (that tank is not drawn) */
    bool    hp_bars;
    const tq_lut_t *lut[TQ_MAX_TANKS];  /* each tank's paint */
    const tq_lut_t *wreck;          /* a tank that lost */
    uint32_t rgb[TQ_MAX_TANKS];     /* each tank's colour on the bars */
    bool    fp_half;                /* fp: leave it at half size (the PPA doubles it) */
    int     show_type, show_frame;  /* show: the hangar's turntable, painted with lut[0] */
    float   show_x, show_y;         /* where its anchor goes */
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

/* Redrawing only where things move (see tq_scene.c): the key of what else
 * the frame depends on (0: draw it whole), the boxes of what moves, and
 * those boxes drawn again into a frame drawn with the same key */
typedef struct {
    int16_t x0, y0, x1, y1;
} tq_rect_t;

#define TQ_RECTS 20

typedef struct {
    int       n;
    tq_rect_t r[TQ_RECTS];
} tq_rects_t;

uint32_t tq_scene_top_key(const tq_req_t *r, const tq_terrain_t *t);
void     tq_scene_top_rects(const tq_req_t *r, const tq_terrain_t *t, tq_rects_t *out);
void     tq_rects_add(tq_rects_t *rs, int x0, int y0, int x1, int y1, int W, int H);
void     tq_scene_top_partial(const tq_req_t *r, const tq_terrain_t *t, uint16_t *fb, const tq_rects_t *rc,
                              tq_prof_t *prof);

/* The first person's last picture at half size (w x h) */
const uint16_t *tq_scene_fp_half(int *w, int *h);

/* The gunner's principal point and focal length on the screen */
void tq_scene_fp_frame(int w, int h, float *cx, float *cy, float *f);
