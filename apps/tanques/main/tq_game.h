/*
 * TANQUES - the battle, without pixels: the map's things, the turn's
 * points, where a tank can drive, the shells and what each weapon does, the
 * weather, and the computer's tanks.
 *
 * Everything here runs in LVGL's thread. The renderers get a copy of
 * tq_state_t every frame (tq_scene.h) and never write it.
 */
#pragma once

#include "tq_data.h"
#include "tq_terrain.h"

#define TQ_MAX_TANKS   4
#define TQ_GRIDMAX     44           /* driving cells, 1 m */
#define TQ_MAX_TREES   80
#define TQ_MAX_BLOCKS  128
#define TQ_MAX_PARTS   240
#define TQ_MAX_SHELLS  8
#define TQ_MAX_MINES   12
#define TQ_MAX_ZONES   8
#define TQ_MAX_PATH    48
#define TQ_MAX_ARC     120
#define TQ_BLOCK_M     0.96f        /* a stone's height (they stack) */

typedef struct {
    int      type, team;
    bool     ai;
    int      diff;
    char     name[20];
    float    x, y;                  /* on the ground */
    float    yaw;                   /* the hull, rad, CCW from +X */
    float    tyaw;                  /* the turret, world */
    float    pitch;                 /* the barrel's elevation, rad */
    int      hp, hpmax, armor;
    int      ap, apmax;
    float    m_per_ap, climb, max_slope;
    int      sel;                   /* the weapon picked: an index into load */
    int      nload;
    uint8_t  load[TQ_SLOTS_MAX];
    uint8_t  level[TQ_SLOTS_MAX];
    uint8_t  uses[TQ_SLOTS_MAX];    /* left this battle (0xFF: no limit) */
    int      shield;                /* damage it still stops */
    int      emp;                   /* AP it loses next turn */
    float    hit_flash;
    bool     alive;
    int      dealt;                 /* damage done to others */
    int      kills;
    bool     moved;                 /* drove this turn */
    int      shots;                 /* fired this turn */
    bool     gone;                  /* not on the map (a wave not come yet): never drawn, never hit */
    bool     boss;
} tq_tank_t;

typedef struct {
    float x, y;
    uint8_t kind;                   /* TQ_PROP_TA or TQ_PROP_TB */
    uint8_t alive;
} tq_tree_t;

typedef struct {
    int8_t  i, j;                   /* the 1 m cell */
    uint8_t variant;                /* 0 or 1 */
    uint8_t alive;
    float   z;                      /* its bottom */
    float   vz;                     /* falling */
} tq_block_t;

enum { TQ_PART_PUFF = 0, TQ_PART_FIRE, TQ_PART_CHUNK, TQ_PART_SMOKE, TQ_PART_LEAF, TQ_PART_RING, TQ_PART_FLASH,
       TQ_PART_SPARK };

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
    int     weapon, level, owner;
    float   t;
    int     bounces;
    float   drill;                  /* metres dug */
    bool    split;                  /* a cluster's bomblet */
    bool    under;                  /* a drill below ground */
    float   wait;                   /* the double cannon's second: seconds before it leaves */
} tq_shell_t;

typedef struct {
    float x, y;
    int   team, owner;
    bool  live;
} tq_mine_t;

enum { TQ_ZONE_FIRE = 0, TQ_ZONE_SMOKE };

typedef struct {
    float x, y, r;
    int   kind, owner;
    int   turns;                    /* the rounds it still lasts */
    bool  live;
} tq_zone_t;

/* What happened this step, for the app to show */
enum { TQ_EV_NONE = 0, TQ_EV_BLAST, TQ_EV_MISS, TQ_EV_HEAL, TQ_EV_SHIELD, TQ_EV_EMP, TQ_EV_TELEPORT, TQ_EV_MINE,
       TQ_EV_BURN,
       TQ_EV_TESLA,                 /* the coil's lightning, at its first target */
       TQ_EV_FIRE2,                 /* the double cannon's second shot leaves */
       TQ_EV_POINT,                 /* the hill: a point (value: the team) */
       TQ_EV_FLAG_TAKE, TQ_EV_FLAG_BACK, TQ_EV_CAPTURE,     /* value: the team that did it */
       TQ_EV_CHEST,                 /* value: the team */
};

typedef struct {
    int       type;
    tq_v3_t   at;
    int       damage[TQ_MAX_TANKS];
    bool      direct[TQ_MAX_TANKS];
    int       value;                /* healed, shielded */
    tq_vbox_t dirty;                /* the ground changed (x1 < x0: not) */
    int       weapon;               /* a landing's: what landed (for the sound) */
    bool      split;                /* ... and whether it was a bomblet */
    bool      water;                /* it fell in the sea */
} tq_event_t;

#define TQ_MAX_EVENTS 8
#define TQ_MAX_CHESTS 8
#define TQ_MAX_BOLTS  6

typedef struct {
    tq_tank_t  tank[TQ_MAX_TANKS];
    int        ntank;
    int        cur;                 /* whose turn */
    int        round;
    tq_tree_t  tree[TQ_MAX_TREES];
    int        ntree;
    tq_block_t block[TQ_MAX_BLOCKS];
    int        nblock;
    tq_part_t  part[TQ_MAX_PARTS];
    int        npart;
    tq_shell_t shell[TQ_MAX_SHELLS];
    tq_mine_t  mine[TQ_MAX_MINES];
    tq_zone_t  zone[TQ_MAX_ZONES];
    float      wind_x, wind_y;      /* m/s^2 on the shells */
    int        weather, diff;
    int        grid;                /* driving cells per side (the map's metres) */

    /* the laser's beam, while it shows */
    tq_v3_t    beam_a, beam_b;
    float      beam_t;
    uint32_t   beam_rgb;

    /* what the views show besides the things */
    bool       show_reach;
    uint8_t    reach[TQ_GRIDMAX * TQ_GRIDMAX];  /* 0 out, 1 both weapons left, 2 one, 3 none */
    int        npath;
    float      path[TQ_MAX_PATH][2];
    int        narc;
    tq_v3_t    arc[TQ_MAX_ARC];
    bool       arc_full;            /* the landing shows */
    bool       arc_landed;
    tq_v3_t    arc_end;
    float      pulse;               /* the turn's ring, 0..1 */

    float      grav_k;              /* the shells' gravity here (the Moon less) */
    /* the Tesla's lightning, while it shows */
    int        nbolt;
    tq_v3_t    bolt_a[TQ_MAX_BOLTS], bolt_b[TQ_MAX_BOLTS];
    float      bolt_t;

    /* the mode and the mission's goal */
    int        mode, goal, goal_n;
    int        target;              /* points, captures or waves that win (0: none) */
    int        score[2];            /* per side: points, captures, chests */
    int        protect;             /* TQ_GOAL_PROTECT: the tank that must live (-1) */
    /* the hill */
    float      hill_x, hill_y, hill_r;
    /* the flags, one a side: where they are, home, who carries each (-1) */
    struct {
        float x, y, hx, hy;
        int   carrier;
    } flag[2];
    /* the chests */
    int        nchest;
    float      chest[TQ_MAX_CHESTS][2];
    uint8_t    chest_alive[TQ_MAX_CHESTS];
    /* the waves */
    int        wave;
} tq_state_t;

/* A battle to set up: the map and who plays */
typedef struct {
    int      biome, size, weather, diff;
    int      objects;               /* trees and walls: 0..TQ_OBJECT_LEVELS-1, the middle the usual */
    int      mode, goal, goal_n;    /* TQ_MODE_*, TQ_GOAL_* and its number (0: the mode's usual) */
    uint32_t seed;
    int      ntank;
    struct {
        int      type, team;
        bool     ai;
        char     name[20];
        uint8_t  up[TQ_UPGRADES];
        uint8_t  load[TQ_SLOTS_MAX];    /* 0xFF empty */
        uint8_t  level[TQ_WEAPONS];
        bool     boss;                  /* three times the health */
        bool     later;                 /* the waves: a slot that comes in a later wave */
    } t[TQ_MAX_TANKS];
} tq_setup_t;

void tq_game_new(tq_state_t *s, tq_terrain_t *t, const tq_setup_t *su);

/* The turn's start: AP back (less an EMP), the weather's wind, fire
 * burning whoever stands in it (into ev) */
void tq_game_turn(tq_state_t *s, const tq_terrain_t *t, tq_event_t *ev);
/* The next tank alive after the current one */
int  tq_game_next(const tq_state_t *s);
/* The side that won (the last standing, or the mode's or the goal's), -1
 * if none yet, -2 if nobody is left */
int  tq_game_winner(const tq_state_t *s);
/* The waves: the board's side is out and another wave is to come */
bool tq_game_wave_due(const tq_state_t *s);
/* Brings the next wave in (the board's slots back, tougher); its size */
int  tq_game_wave(tq_state_t *s, tq_terrain_t *t);
/* A tank drove onto a flag, a chest: what it did (ev), false if nothing */
bool tq_game_pickups(tq_state_t *s, const tq_terrain_t *t, int who, tq_event_t *ev);

/* Where the tank whose turn it is can go (s->reach) */
void tq_game_reach(tq_state_t *s, const tq_terrain_t *t);
/* The path to a cell (s->path), and its AP; false if out of reach */
bool tq_game_route(tq_state_t *s, const tq_terrain_t *t, float x, float y, int *ap);
/* A tank drove over a mine? The blast is in ev */
bool tq_game_mines(tq_state_t *s, tq_terrain_t *t, int who, tq_event_t *ev);

/* The weapon picked by a tank, and its level */
static inline int tq_weapon_of(const tq_tank_t *k)
{
    return k->load[k->sel];
}

/* A turn has one shot, and a second one at twice the AP for a tank that
 * did not drive */
#define TQ_SHOTS_MAX 2

/* What a slot's shot costs now */
static inline int tq_shot_ap(const tq_tank_t *k, int slot)
{
    return TQ_WEAPON[k->load[slot]].ap * (k->shots + 1);
}

/* A slot the tank may fire now (AP, uses left, the turn's shots) */
static inline bool tq_slot_ready(const tq_tank_t *k, int slot)
{
    if (k->shots >= TQ_SHOTS_MAX || (k->shots && k->moved)) return false;
    return k->uses[slot] && tq_shot_ap(k, slot) <= k->ap;
}

/* Anything the tank may still fire this turn */
static inline bool tq_can_fire(const tq_tank_t *k)
{
    for (int w = 0; w < k->nload; w++)
        if (tq_slot_ready(k, w)) return true;
    return false;
}

void tq_game_muzzle(const tq_tank_t *k, const tq_terrain_t *t, tq_v3_t *p, tq_v3_t *v);
/* The arc the aim would fly (s->arc), as much as the weapon's guide shows */
void tq_game_arc(tq_state_t *s, const tq_terrain_t *t);

/* Fires the current tank's weapon. Self weapons and the laser act at once
 * (their events in ev, *nev); the rest fly (tq_game_flying). */
void tq_game_fire(tq_state_t *s, tq_terrain_t *t, tq_event_t *ev, int *nev);
bool tq_game_flying(const tq_state_t *s);
/* The first shell in the air (the camera follows it), or NULL */
const tq_shell_t *tq_game_lead(const tq_state_t *s);

/* Moves shells, particles and falling stones by dt; landings and what
 * they did are in ev[0..*nev) */
void tq_game_step(tq_state_t *s, tq_terrain_t *t, float dt, tq_event_t *ev, int *nev);

/* The computer's turn: where to drive (a path in s->path), then the aim
 * (yaw, pitch and the weapon's slot) from wherever it stopped */
void tq_ai_plan_move(tq_state_t *s, const tq_terrain_t *t, int *ap_cost);
void tq_ai_plan_aim(tq_state_t *s, const tq_terrain_t *t, float *yaw, float *pitch, int *slot);

float tq_rand(void);
float tq_tank_z(const tq_terrain_t *t, const tq_tank_t *k);
