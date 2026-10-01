/*
 * MILA - the world map: a vertical strip of panels (DESIGN.md section 4)
 *
 * Bottom to top: map_home, the worlds' panels in the table's order, then
 * map_soon. Every panel is one opaque picture (tools/blender/SPEC.md section
 * 7) whose level stones come with it (<panel>_nodes in the pack), so a new
 * world is a new panel and nothing here changes. The finger scrolls it, with
 * a little inertia; a tap on a stone opens the level, on the house goes home.
 */
#pragma once

#include "ml_gfx.h"

#include <stdbool.h>

typedef struct app app_t;

bool mlm_open(app_t *a);            /* worker                                */
void mlm_close(app_t *a);
void mlm_step(app_t *a, float dt);
void mlm_band(app_t *a, ml_img_t *im, int y0, int y1);
/* what changed since the last frame (worker, after mlm_step) */
struct ml_dmg;
void mlm_damage(app_t *a, struct ml_dmg *d);
/* the screen turned (worker) */
void mlm_refit(app_t *a);
void mlm_touch(app_t *a, int code, int x, int y);   /* LVGL thread          */
