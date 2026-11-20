/*
 * BASTION - a world, as its pack defines it (tools/bsworlds.py writes the
 * text): the roads as dense points, the pads, the fort, and the waves of
 * its three sectors. The game knows no world by name: a new one is a new
 * pack.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BS_PATHS    4
#define BS_PADS     24
#define BS_SECTORS  3
#define BS_WAVES    24
#define BS_LANGS    3

/* metres to the screen (SPEC.md): 40 px a metre across, 32 rows a metre
 * of ground, 24 rows a metre of height */
#define BS_PX       40.0f
#define BS_ROW_Y    32.0f
#define BS_ROW_Z    24.0f
#define BS_SX(x)    ((int)(360.0f + (x) * BS_PX))
#define BS_SY(y)    ((int)(1280.0f - (y) * BS_ROW_Y))

typedef struct {
    int    n;
    float *x, *y, *s;       /* the points and the road's length up to each */
    float  len;
} bs_path_t;

typedef struct {
    uint8_t  alien, path;
    uint16_t count;
    float    gap, delay;
    uint16_t hp_pct;
} bs_group_t;

typedef struct {
    uint16_t first, n;
} bs_wave_t;

typedef struct {
    char      id[12];
    int       order;
    char      name[BS_LANGS][40];
    char      desc[BS_LANGS][120];
    uint32_t  accent, sky;
    int       hue, music, boss;
    float     fort_x, fort_y;
    bs_path_t path[BS_PATHS], air[BS_PATHS];
    int       npath;
    float     pad_x[BS_PADS], pad_y[BS_PADS];
    int       npad;
    int       nwaves[BS_SECTORS];
    int       credits[BS_SECTORS];
    bs_wave_t wave[BS_SECTORS][BS_WAVES];
    bs_group_t *group;
    int       ngroup, cap;
} bs_world_t;

/* head_only: the names, colours and counts, for the map (no roads) */
bool bs_world_parse(bs_world_t *w, char *text, bool head_only);
void bs_world_free(bs_world_t *w);
/* the point s metres along a road, and which way it runs there */
void bs_path_at(const bs_path_t *p, float s, float *x, float *y, float *dx, float *dy);
/* which aliens a sector brings (a mask of AL_*) */
uint32_t bs_world_aliens(const bs_world_t *w, int sector);
