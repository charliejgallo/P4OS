/*
 * BASTION - the numbers: what each tower does, what each alien is, what
 * the laboratory sells. One place to tune the game.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    TW_GATLING, TW_CANNON, TW_LASER, TW_MISSILE, TW_TESLA, TW_CRYO, TW_FLAME, TW_RAIL, TW_MORTAR, TW_DRONE,
    TW_EMP, TW_GRAVITY, TW_RADAR, TW_BANK, TW_N
};

enum {
    AL_GRUB, AL_RUNNER, AL_SWARM, AL_SHELL, AL_WASP, AL_SPLIT, AL_SHIELD, AL_HEALER, AL_BRUTE, AL_PHASE,
    AL_JAMMER, AL_CARRIER, AL_QUEEN, AL_TITAN, AL_OVERMIND, AL_MINI, AL_N
};

/* what the laboratory improves for good */
enum { TECH_ARMOR, TECH_FUNDS, TECH_BOUNTY, TECH_DAMAGE, TECH_RANGE, TECH_SALVAGE, TECH_INTEREST, TECH_CRIT, TECH_N };
enum { PW_STRIKE, PW_FREEZE, PW_REPAIR, PW_N };

#define BS_TIERS        3
#define BS_MASTERY_MAX  5
#define BS_TECH_MAX     5
#define BS_POWER_MAX    3
#define BS_WORLDS_MAX   16
#define BS_RANK_MAX     30

typedef struct {
    const char *id;         /* the sheets' name in the pack                 */
    const char *name;
    const char *desc;
    uint32_t    accent;
    int16_t     cost;       /* credits to build; tier 2 and 3 cost 0.8 and 1.3 of it */
    int16_t     unlock;     /* cores in the laboratory, 0: owned from the start */
    float       dmg;        /* a shot, or per second for the ones that burn */
    float       rate;       /* seconds between shots                         */
    float       range;      /* metres                                        */
    float       splash;     /* metres, 0: one target                         */
    uint8_t     air, ground;
    uint8_t     spin;       /* its head is a loop, not an aim                */
    uint8_t     muzzle;     /* how far from the centre the shot leaves, in dm */
    uint8_t     height;     /* and how high, in dm                           */
} bs_tower_def_t;

typedef struct {
    const char *id;
    const char *name;
    float       hp, speed;
    uint8_t     armor;      /* taken off every hit (a hit always does a fifth) */
    uint8_t     reward, lives;
    uint8_t     fly, boss;
    uint8_t     xp;
    float       radius;     /* metres: how big a target it is                */
    float       alt;        /* metres over its shadow                        */
    float       shield;     /* an energy shield that comes back              */
    uint32_t    color;      /* its blood and sparks                          */
} bs_alien_def_t;

typedef struct {
    const char *name;
    const char *desc;
    int16_t     cost[BS_TECH_MAX];
} bs_tech_def_t;

typedef struct {
    const char *name;
    const char *desc;
    int16_t     cost[BS_POWER_MAX];     /* the first is what unlocking it costs */
    float       cooldown;
    uint32_t    color;
} bs_power_def_t;

extern const bs_tower_def_t BS_TOWER[TW_N];
extern const bs_alien_def_t BS_ALIEN[AL_N];
extern const bs_tech_def_t  BS_TECH[TECH_N];
extern const bs_power_def_t BS_POWER[PW_N];
extern const int16_t        BS_MASTERY_COST[BS_MASTERY_MAX];

int   bs_alien_by_id(const char *id);
/* what a tower of that tier (0..2) does, before the laboratory's bonuses */
float bs_tier_dmg(int type, int tier);
float bs_tier_range(int type, int tier);
int   bs_tier_cost(int type, int tier);         /* what reaching that tier costs */
/* the experience a rank asks for, in all */
int   bs_rank_xp(int rank);
int   bs_rank_of(int xp);
