/*
 * PLANEO - the game: what each flight asks for.
 *
 * Every world has three missions, each at its own hour:
 *   - Anillos: a course of rings through the world's best parts, in order,
 *     against the clock (the stars are times);
 *   - Carrera: the same kind of course against a rival plane that flies it
 *     perfectly at its speed (a star for finishing, two for winning,
 *     three for winning by a margin);
 *   - Entregas: packages to pick up and carry, flying low through the
 *     green light where each waits and the orange one where it goes.
 * And the free flight: any open world, any hour, nothing to do but fly.
 *
 * The courses come from the world's seed, so a mission is the same every
 * time and a record means something. A crash costs five seconds and puts
 * the plane back at the last thing it passed. Stars open the worlds.
 */
#pragma once

#include "vu_flight.h"
#include "vu_world.h"

#include <stdbool.h>
#include <stdint.h>

enum { VG_RINGS = 0, VG_RACE, VG_DELIVER, VG_KINDS, VG_FREE = 9 };

#define VG_MISSIONS     (VW_BIOMES * VG_KINDS)
#define VG_MAXPTS       24
#define VG_RING_R       13.0f
#define VG_BEAM_R       30.0f
#define VG_BEAM_LOW     45.0f       /* metres over the ground to take a package */
#define VG_CRASH_S      5.0f

enum { VG_ST_COUNT = 0, VG_ST_FLY, VG_ST_DONE };

/* events for the sound and the banners, a bit each */
enum {
    VG_EV_RING = 1, VG_EV_PICK = 2, VG_EV_DROP = 4, VG_EV_FINISH = 8, VG_EV_CRASH = 16, VG_EV_GO = 32,
    VG_EV_COUNT = 64, VG_EV_PASSED = 128,
};

typedef struct {
    float p[3];
    float n[3];                 /* a ring's facing (the course's direction) */
} vg_pt_t;

typedef struct {
    int      kind, biome, hour, mission;   /* mission: 0..14, or -1 free */
    int      state;
    float    t;                 /* the clock: seconds since ¡YA! (counting down before) */
    float    penalty;
    vg_pt_t  pt[VG_MAXPTS];
    int      npt;
    int      next;              /* the next ring / point */
    bool     carrying;
    /* the rival */
    float    rival_v, rival_s, rival_len, rival_done_t;
    float    rival_p[3], rival_f[3], rival_r[3], rival_u[3];
    float    rival_start[3];
    float    seglen[VG_MAXPTS];
    /* the end */
    float    final_t;
    int      stars;
    bool     won, record;
    float    par[3];            /* the times for 1, 2, 3 stars (rings, packages) */
    /* where the plane goes back to after a crash */
    float    safe_p[3], safe_yaw;
    uint32_t events;
    float    banner_t;          /* how long the last banner has been up */
    int      banner;            /* a VT_W_ word, -1 none */
} vg_game_t;

/* The mission's world and hour */
void vg_mission_world(int mission, int *biome, int *hour);
int  vg_mission_kind(int mission);

/* A flight in the world just made: a mission (0..14) or free (-1) */
void vg_start(vg_game_t *g, const vw_world_t *w, int mission, vf_plane_t *pl);
/* A step: the plane moved from p0 to where it is now */
void vg_step(vg_game_t *g, const vw_world_t *w, vf_plane_t *pl, const float p0[3], float dt);
/* The rival's pose for the renderer (the race only) */
bool vg_rival(const vg_game_t *g);

/* Records: stars and the best time of each mission, kept in preferences */
typedef struct {
    uint8_t  stars[VG_MISSIONS];
    uint32_t best_ms[VG_MISSIONS];  /* 0: none */
} vg_save_t;

void vg_load(vg_save_t *s);
void vg_store(const vg_save_t *s, int mission);
int  vg_total_stars(const vg_save_t *s);
/* The stars a world needs to open */
int  vg_world_needs(int biome);

/* The time as "1:23.4" */
void vg_fmt_time(char *b, int n, float t);
