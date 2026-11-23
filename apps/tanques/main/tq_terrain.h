/*
 * TANQUES - the ground: a height map the shots dig into.
 *
 * n x n cells of TQ_CELL (the map's size is the battle's: 28 to 40 m), with
 * a material and a scorch per vertex and the vertex's colour already lit
 * (the sun with the shadows the hills cast, the sky, a little occlusion in
 * the hollows). Each biome shapes and colours it its own way. A crater
 * lowers the heights and lights again only the vertices it can have
 * changed; the renderers draw the triangles with those colours.
 */
#pragma once

#include "tq_view.h"

/* per biome: the ground, a path, rock, the biome's own (ice, lava,
 * paving, the beach's sea) and a bridge's deck */
enum { TQ_MAT_BASE = 0, TQ_MAT_PATH, TQ_MAT_ROCK, TQ_MAT_SPECIAL, TQ_MAT_DECK, TQ_MATS };

/* The beach's sea: flat at this height; only the Anfibio drives on it */
#define TQ_SEA_Z 0.10f
#define TQ_ISLANDS_MAX 4

typedef struct {
    int      n, nv;                         /* cells and vertices per side */
    float    size;                          /* metres */
    int      biome;
    float    h[TQ_NVMAX * TQ_NVMAX];
    float    burn[TQ_NVMAX * TQ_NVMAX];     /* scorch, 0..1 */
    uint8_t  mat[TQ_NVMAX * TQ_NVMAX];
    uint8_t  tint[TQ_NVMAX * TQ_NVMAX];     /* the ground's patches, 0..255 */
    uint32_t col[TQ_NVMAX * TQ_NVMAX];      /* lit colour, 0xRRGGBB */
    float    hmax;
    /* the beach: its islands (x, y, half-sizes along x and y; the first two
     * are the sides', the rest islets) and the bridge between the two */
    int      nisl;
    float    isl[TQ_ISLANDS_MAX][4];
    float    bridge[4];                     /* x0, y0, x1, y1 of its middle line */
} tq_terrain_t;

/* A box of vertices, inclusive: what a change touched */
typedef struct {
    int x0, y0, x1, y1;
} tq_vbox_t;

void  tq_ter_generate(tq_terrain_t *t, uint32_t seed, float size, int biome);
/* Levels the ground around a spot (where a tank starts) */
void  tq_ter_flatten(tq_terrain_t *t, float x, float y, float r);
/* Lights everything again (after generating, flattening) */
void  tq_ter_relight(tq_terrain_t *t);
float tq_ter_height(const tq_terrain_t *t, float x, float y);
void  tq_ter_normal(const tq_terrain_t *t, float x, float y, tq_v3_t *n);
/* Lights the vertices in the box again */
void  tq_ter_light(tq_terrain_t *t, tq_vbox_t b);
/* Digs a crater (radius r, depth d metres; d < 0 raises a mound) with a rim
 * and a scorch; returns the vertices whose colour changed */
tq_vbox_t tq_ter_crater(tq_terrain_t *t, float x, float y, float r, float d, bool scorch);
/* The colour of the map's sides at a height (the diorama's cut) */
uint32_t tq_ter_side_colour(const tq_terrain_t *t, float z, float shade);
/* The biome's ground beyond the map (the first person's far plane) and sky */
uint32_t tq_ter_far_colour(int biome, int which);

/* Sea where (the beach only): the vertex nearest */
bool tq_ter_water(const tq_terrain_t *t, float x, float y);
/* A spot on land near (x, y) (the beach: off the sea; elsewhere itself) */
void tq_ter_land(const tq_terrain_t *t, float *x, float *y);

static inline float tq_ter_vh(const tq_terrain_t *t, int i, int j)
{
    return t->h[j * t->nv + i];
}
