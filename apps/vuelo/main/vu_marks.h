/*
 * PLANEO - what the missions put in the sky: the rings (the next one gold,
 * the two after it faint), the columns of light where a package waits or
 * goes, and where the next one is when it is off the screen.
 */
#pragma once

#include "vu_game.h"
#include "vu_render.h"

#define VM_MAXT 400

typedef struct {
    float    x[3], y[3];
    uint16_t col;
    uint8_t  alpha;
} vm_tri_t;

typedef struct {
    vm_tri_t t[VM_MAXT];
    int      n;
    /* the next target, for the arrow at the edge */
    bool     target;
    bool     on_screen;
    float    tx, ty;            /* on the frame (may be outside it) */
    float    dist;              /* metres */
    float    dir_x, dir_y;      /* towards it, on the frame, unit */
} vm_draw_t;

void vm_prepare(vm_draw_t *d, const vg_game_t *g, const vw_world_t *w, const vr_proj_t *p, float time);
void vm_draw(const vm_draw_t *d, uint16_t *out, int W, int H, int y0, int y1);
