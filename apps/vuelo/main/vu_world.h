/*
 * PLANEO - a world: a height map that tiles, with its colours already lit.
 *
 * 1024 x 1024 texels of VW_TEXM metres (2 km a side; the world repeats, so
 * the flight never meets an edge), and five smaller levels for what is far
 * away. A texel is 32 bits: the colour in RGB565 (lit by the sun of the
 * world's hour, with the shadows the hills cast and a little darkness in
 * the hollows), the height in sixteenths of a metre, and whether it is
 * water. Trees and buildings are part of the heights: in the renderer's
 * columns they stand as solid as the hills.
 *
 * Five kinds of world, each made from a seed: islands, canyons, snow, a
 * volcano and a city. The hour (morning, noon, evening, night) chooses the
 * sun, the sky and the fog.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define VW_LG       10
#define VW_N        (1 << VW_LG)
#define VW_TEXM     2.0f                    /* metres a texel at level 0 */
#define VW_SIZE     (VW_N * VW_TEXM)        /* the world's side, metres */
#define VW_HU       16.0f                   /* height units a metre */
#define VW_LEVELS   6                       /* 1024 .. 32 texels a side */
#define VW_WATER    0x80000000u             /* the texel is water */
#define VW_HMASK    0x7FFFu

/* Where texel (x, y) of a level of 2^lg a side is kept: in tiles of 8 x 8
 * (256 bytes), row after row of tiles, so the samples a line of the
 * renderer takes stay close in memory whichever way it runs (a plain row
 * order made every sample of a line looking north or south a miss in the
 * cache, on the board half the frame's time). x and y already wrapped. */
static inline uint32_t vw_idx(uint32_t x, uint32_t y, int lg)
{
    return ((y >> 3) << (lg + 3)) | ((x >> 3) << 6) | ((y & 7) << 3) | (x & 7);
}

static inline int vw_h(uint32_t t)          /* height units */
{
    return (int)((t >> 16) & VW_HMASK);
}

enum { VW_ISLAS = 0, VW_CANONES, VW_NIEVE, VW_VOLCAN, VW_CIUDAD, VW_BIOMES };
enum { VW_MORNING = 0, VW_NOON, VW_EVENING, VW_NIGHT, VW_HOURS };

/* splits a job in two halves, one per core (aos_hal_worker_split); NULL or
 * false: the caller runs both */
typedef bool (*vw_split_fn)(void (*fn)(void *arg, int part), void *arg);

typedef struct {
    uint32_t *lev[VW_LEVELS];
    int      biome, hour;
    uint32_t seed;
    float    water;             /* the sea's (or the river's) level, metres; < 0: none */
    float    hmax;              /* the highest texel, metres */
    float    sun[3];            /* towards the sun, unit */
    /* the look, 0xRRGGBB */
    uint32_t sky_top, sky_hor, fog, sun_col, cloud_lit, cloud_dark;
    float    clouds;            /* how much of the sky the clouds cover, 0..1 */
    float    fog_near, fog_far; /* metres */
    bool     stars;
    /* where the flight starts: a clear spot and a heading */
    float    start[3], start_yaw;
    uint32_t gen_ms[4];         /* heights, trees + shadows, colours, levels */
} vw_world_t;

bool  vw_alloc(vw_world_t *w);
void  vw_free(vw_world_t *w);
/* Makes the world. Takes ~1 s of both cores on the board (split may be
 * NULL). progress, if not NULL, goes 0..100 as it works. */
bool  vw_generate(vw_world_t *w, int biome, uint32_t seed, int hour, vw_split_fn split, volatile int *progress);
/* The ground (or the water, or a tree's top) under a point, metres */
float vw_height(const vw_world_t *w, float x, float y);
/* Whether a point is over water */
bool  vw_is_water(const vw_world_t *w, float x, float y);
/* The cloud layer: 256 x 256 densities (0..255) of 16 m, tiling */
#define VW_CLOUD_LG   8
#define VW_CLOUD_M    16.0f
const uint8_t *vw_clouds(void);
void  vw_clouds_make(uint32_t seed, float cover);

static inline float vw_wrap(float v)
{
    while (v < 0) v += VW_SIZE;
    while (v >= VW_SIZE) v -= VW_SIZE;
    return v;
}
