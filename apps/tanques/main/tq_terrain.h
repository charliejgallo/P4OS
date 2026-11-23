/*
 * TANQUES - the ground: a height map the shots dig into.
 *
 * TQ_NV x TQ_NV heights, TQ_CELL apart, with a material and a scorch per
 * vertex and the vertex's colour already lit (the sun with the shadows the
 * hills cast, the sky, a little occlusion in the hollows). A crater lowers
 * the heights and lights again only the vertices it can have changed; the
 * renderers draw the triangles with those colours.
 */
#pragma once

#include "tq_view.h"

enum { TQ_MAT_GRASS = 0, TQ_MAT_DIRT, TQ_MAT_ROCK, TQ_MAT_SAND };

typedef struct {
    float    h[TQ_NV * TQ_NV];
    float    burn[TQ_NV * TQ_NV];          /* scorch, 0..1 */
    uint8_t  mat[TQ_NV * TQ_NV];
    uint8_t  tint[TQ_NV * TQ_NV];          /* the grass's patches, 0..255 */
    uint32_t col[TQ_NV * TQ_NV];           /* lit colour, 0xRRGGBB */
    float    hmax;
} tq_terrain_t;

/* A box of vertices, inclusive: what a change touched */
typedef struct {
    int x0, y0, x1, y1;
} tq_vbox_t;

void  tq_ter_generate(tq_terrain_t *t, uint32_t seed);
float tq_ter_height(const tq_terrain_t *t, float x, float y);
void  tq_ter_normal(const tq_terrain_t *t, float x, float y, tq_v3_t *n);
/* Lights the vertices in the box again */
void  tq_ter_light(tq_terrain_t *t, tq_vbox_t b);
/* Digs a crater (radius r, depth d metres) with a rim and a scorch; returns
 * the vertices whose colour changed (shadows reach downhill of it) */
tq_vbox_t tq_ter_crater(tq_terrain_t *t, float x, float y, float r, float d);
/* The colour of the map's sides at a height (the diorama's cut) */
uint32_t tq_ter_side_colour(float z, float shade);

static inline float tq_ter_vh(const tq_terrain_t *t, int i, int j)
{
    return t->h[j * TQ_NV + i];
}
