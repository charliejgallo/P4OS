/*
 * TANQUES - the battle, without pixels: the map's things, the turn's
 * points, where a tank can drive, the shells' flight, what a blast does,
 * and the computer's tank.
 *
 * Everything here runs in LVGL's thread. The renderers get a copy of
 * tq_state_t every frame (tq_scene.h) and never write it.
 */
#pragma once

#include "tq_terrain.h"

#define TQ_GRID        36           /* driving cells, 1 m */
#define TQ_MAX_TREES   28
#define TQ_MAX_BLOCKS  40
#define TQ_MAX_PARTS   200
#define TQ_MAX_PATH    48
#define TQ_MAX_ARC     120
#define TQ_BLOCK_M     0.96f        /* a stone's height (they stack) */

enum { TQ_W_CANNON = 0, TQ_W_MORTAR, TQ_WEAPONS };

typedef struct {
    const char *name;               /* N_() */
    int   ap;
    float speed;                    /* m/s at the muzzle */
    float radius;                   /* the blast's reach, m */
    int   damage;                   /* at the centre */
    float crater_r, crater_d;
    bool  full_guide;               /* the whole arc shown while aiming */
} tq_weapon_t;

extern const tq_weapon_t TQ_WEAPON[TQ_WEAPONS];

typedef struct {
    float x, y;                     /* on the ground */
    float yaw;                      /* the hull, rad, CCW from +X */
    float tyaw;                     /* the turret, world */
    float pitch;                    /* the barrel's elevation, rad */
    int   hp, hpmax;
    int   ap, apmax;
    int   scheme;
    int   weapon;
    float hit_flash;                /* s left of the hit's flash */
    bool  ai;
} tq_tank_t;

typedef struct {
    float x, y;
    uint8_t kind;                   /* TQ_PROP_ROUND or TQ_PROP_PINE */
    uint8_t alive;
} tq_tree_t;

typedef struct {
    int8_t  i, j;                   /* the 1 m cell */
    uint8_t variant;                /* 0 or 1 */
    uint8_t alive;
    float   z;                      /* its bottom */
    float   vz;                     /* falling */
} tq_block_t;

enum { TQ_PART_PUFF = 0, TQ_PART_FIRE, TQ_PART_CHUNK, TQ_PART_SMOKE, TQ_PART_LEAF, TQ_PART_RING, TQ_PART_FLASH };

typedef struct {
    tq_v3_t p, v;
    float   age, life;
    float   r0, r1;                 /* radius, m, from birth to death */
    uint32_t rgb;
    uint8_t kind;
} tq_part_t;

typedef struct {
    bool    live;
    tq_v3_t p, v;
    int     weapon;
    int     owner;
    float   t;
} tq_shell_t;

typedef struct {
    tq_tank_t  tank[2];
    tq_tree_t  tree[TQ_MAX_TREES];
    int        ntree;
    tq_block_t block[TQ_MAX_BLOCKS];
    int        nblock;
    tq_part_t  part[TQ_MAX_PARTS];
    int        npart;
    tq_shell_t shell;
    int        cur;                 /* whose turn: 0 or 1 */

    /* what the views show besides the things */
    bool       show_reach;
    uint8_t    reach[TQ_GRID * TQ_GRID];    /* 0 out, 1 both weapons left, 2 the cannon, 3 none */
    int        npath;
    float      path[TQ_MAX_PATH][2];
    int        narc;
    tq_v3_t    arc[TQ_MAX_ARC];
    bool       arc_full;            /* else only its first part */
    bool       arc_landed;
    tq_v3_t    arc_end;
    float      pulse;               /* the turn's ring, 0..1 */
} tq_state_t;

/* The map, its trees and stones, the two tanks */
void tq_game_new(tq_state_t *s, tq_terrain_t *t, uint32_t seed, int scheme0, int scheme1, bool vs_ai);

/* Where the tank whose turn it is can go: Dijkstra over the 1 m cells with
 * the slope's cost, into s->reach; cost[] (AP x 16) kept for the path. */
void tq_game_reach(tq_state_t *s, const tq_terrain_t *t);
/* The path to a cell (s->path), and its AP; false if out of reach */
bool tq_game_route(tq_state_t *s, const tq_terrain_t *t, float x, float y, int *ap);

/* A muzzle and its velocity, for the tank's turret and barrel */
void tq_game_muzzle(const tq_tank_t *k, const tq_terrain_t *t, tq_v3_t *p, tq_v3_t *v, int weapon);
/* The arc the aim would fly (s->arc), up to its landing */
void tq_game_arc(tq_state_t *s, const tq_terrain_t *t);

/* Fires: s->shell flies */
void tq_game_fire(tq_state_t *s, const tq_terrain_t *t);

/* Moves the shell and the particles by dt. Returns true when the shell
 * landed this step: *at is where; the blast is the caller's to apply
 * (tq_game_blast), so the renderer can be told about the crater. */
bool tq_game_step(tq_state_t *s, const tq_terrain_t *t, float dt, tq_v3_t *at);

typedef struct {
    int   damage[2];
    int   trees, blocks;
    bool  direct[2];
} tq_blast_t;

/* The blast: damage, the crater (returns its vertex box in *dirty), trees
 * and stones, the fire and the clods */
void tq_game_blast(tq_state_t *s, tq_terrain_t *t, tq_v3_t at, int weapon, int owner,
                   tq_blast_t *out, tq_vbox_t *dirty);

/* The computer's turn: where to drive (a path in s->path), then the aim
 * (yaw, pitch and weapon to turn to) from wherever it stopped */
void tq_ai_plan_move(tq_state_t *s, const tq_terrain_t *t, int *ap_cost);
void tq_ai_plan_aim(tq_state_t *s, const tq_terrain_t *t, float *yaw, float *pitch, int *weapon);

/* A random number 0..1, for the effects */
float tq_rand(void);

/* Heights the things stand on */
float tq_tank_z(const tq_terrain_t *t, const tq_tank_t *k);
