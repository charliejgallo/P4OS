/*
 * Tortuga - the board on the screen (tg_view.c).
 */
#pragma once

#include "tg_app.h"

typedef struct {
    char t[TG_MAXH][TG_MAXW];   /* the tiles as they are now */
    int w, h;
    float x, y;                 /* the turtle: tiles, or steps on the paper */
    float hd;                   /* its heading, 0 = north, clockwise */
    int anim, frame;            /* SH_WALK, SH_BUMP, SH_EAT, SH_PARTY */
    float lift, sink;           /* pixels up (a jump), down (the water) */
    int tick;
    /* the paper */
    const tg_ev_t *ev;          /* the run's events */
    int strokes;                /* events before this one are drawn whole */
    bool partial;               /* and a stroke being drawn */
    float px0, py0, px1, py1;
    uint8_t pcolor;
    const tg_world_t *figure;
} tg_scene_t;

bool tg_view_open(lv_obj_t *parent, int x, int y, int w, int h);
void tg_view_close(void);
lv_obj_t *tg_view_obj(void);
void tg_view_level(const tg_world_t *start, const tg_world_t *figure);
void tg_view_draw(const tg_scene_t *s);
void tg_view_turtle_point(const tg_scene_t *s, int *x, int *y);
void tg_view_tile_point(float x, float y, float *sx, float *sy);
void tg_view_paper_point(float x, float y, float *sx, float *sy);
/* one frame of a sheet into a buffer, its anchor on (x, y) (the start's turtle) */
void tg_sheet_blit(const tg_sheet_t *sh, int frame, uint16_t *buf, int w, int h, int x, int y);
