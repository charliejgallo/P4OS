/*
 * BASTION - a battle: the aliens on their roads, the towers on their pads,
 * shots, effects and the fort's life. Nothing here draws or touches the
 * screen (bs_draw.c reads these structures), so tools/harness.c plays
 * whole campaigns with a bot to tune the numbers.
 *
 * Positions are metres on the ground (bs_world.h). The battle runs in
 * fixed steps of BS_DT.
 */
#pragma once

#include "bs_data.h"
#include "bs_world.h"

#define BS_DT           (1.0f / 60.0f)
#define BS_MAX_ALIENS   320
#define BS_MAX_SHOTS    96
#define BS_MAX_FX       96
#define BS_MAX_PARTS    700
#define BS_MAX_FLOATS   20
#define BS_MAX_DRONES   5
#define BS_MAX_SPAWN    12
#define BS_BOLT_PTS     8
/* a boss pays as it is worn down, not only when it falls: this much for
 * each twentieth of its health, so the wave it fills alone still buys
 * upgrades */
#define BS_BOSS_PARTS   20
#define BS_BOSS_PART    15
#define BS_MILE_STEP    20      /* waves between two goals of the mode without end */

enum { BS_BUILD, BS_WAVE, BS_WON, BS_LOST };
/* the campaign's sectors; waves without end, to see how far one gets; and
 * a sector with everything unlocked and a fat purse, to try things */
enum { BS_MODE_CAMPAIGN, BS_MODE_ENDLESS, BS_MODE_FREE, BS_MODE_N };
/* the difficulty is the purse: more credits or fewer (0 is the normal one,
 * so a save from before the setting existed reads as normal) */
enum { BS_DIFF_NORMAL, BS_DIFF_EASY, BS_DIFF_HARD, BS_DIFF_N };
enum { TM_FIRST, TM_STRONG, TM_WEAK, TM_NEAR, TM_N };
enum { SH_SHELL, SH_MISSILE, SH_MORTAR };
enum { FX_TRACER, FX_BOLT, FX_RING, FX_BLAST, FX_RAIL, FX_STRIKE, FX_MARK };

/* the sounds, in the order of the pack's names (bs_audio.c) */
enum {
    SND_GATLING, SND_CANNON, SND_LASER, SND_MISSILE, SND_ZAP, SND_CRYO, SND_FLAME, SND_RAIL, SND_MORTAR, SND_DRONE,
    SND_EMP, SND_GRAVITY, SND_BOOM_S, SND_BOOM_M, SND_BOOM_L, SND_HIT, SND_DIE1, SND_DIE2, SND_DIE3, SND_BOSS_ROAR,
    SND_BOSS_DIE, SND_FORT_HIT, SND_ALARM, SND_BUILD, SND_UPGRADE, SND_SELL, SND_COIN, SND_WAVE_START,
    SND_WAVE_CLEAR, SND_VICTORY, SND_DEFEAT, SND_TAP, SND_DENY, SND_STRIKE, SND_FREEZE, SND_REPAIR, SND_STAR, SND_N
};

typedef struct {
    uint8_t  on, type, path, fly;
    uint8_t  cloaked, seen, dir8;
    uint8_t  paid;              /* a boss: the twentieths of its health already paid for */
    uint16_t id;
    float    hp, maxhp, shield, maxshield, shield_t;
    float    s, off, x, y, dx, dy;
    float    slow, slow_t, drag_t, stun_t, burn, burn_t, flash_t, anim, ability_t, cloak_t;
    float    left;              /* metres to the fort */
    float    hpk;               /* its wave's health factor, for what it spawns */
} bs_alien_t;

typedef struct {
    float   x, y, a;
    int16_t target;
    uint16_t tid;
    float   cool;
} bs_drone_t;

typedef struct {
    uint8_t  on, type, tier, mode;
    uint8_t  beam, seen, firing, pad;
    float    x, y;
    float    cool, yaw, heat, off_t, anim, flash_t, boost, cool2;
    float    bx, by, bz;        /* where its beam ends */
    int16_t  target;
    uint16_t tid;
    int      invested, kills;
    bs_drone_t drone[BS_MAX_DRONES];
} bs_tower_t;

typedef struct {
    uint8_t  on, kind, from, tier;
    float    x, y, z, vx, vy, vz, tx, ty, t, life, dmg, splash, speed;
    int16_t  target;
    uint16_t tid;
} bs_shot_t;

typedef struct {
    uint8_t  on, kind, n, pad;
    float    x0, y0, z0, x1, y1, z1, t, life, r;
    uint32_t color;
    float    px[BS_BOLT_PTS], py[BS_BOLT_PTS], pz[BS_BOLT_PTS];
} bs_fx_t;

/* sparks, smoke, fire: screen pixels, they only look good */
typedef struct {
    float    x, y, vx, vy, life, max;
    uint16_t color;
    uint8_t  r, grav;
} bs_part_t;

typedef struct {
    float    x, y, t;
    uint16_t color;
    char     text[14];
} bs_float_t;

typedef struct {
    uint8_t alien, path;
    int     left;
    float   t, gap, hpk;
} bs_spawn_t;

/* what the laboratory gives a battle (bs_save.h makes one from the save) */
typedef struct {
    uint8_t owned[TW_N];
    uint8_t mastery[TW_N];
    uint8_t tech[TECH_N];
    uint8_t power[PW_N];
    int     rank;
    uint8_t mode, diff;         /* BS_MODE_*, BS_DIFF_* */
} bs_kit_t;

typedef struct {
    const bs_world_t *w;
    int        sector;
    bs_kit_t   kit;
    int        state;
    float      time, state_t;
    int        credits, lives, max_lives;
    int        wave, nwaves;        /* wave: the one being fought, or the next one */
    bs_spawn_t sp[BS_MAX_SPAWN];
    int        nsp;
    int        mode;
    bs_group_t next[BS_MAX_SPAWN];  /* the wave about to come */
    int        nnext;
    float      k_reward;
    uint16_t   hp0;                 /* endless: the first wave's health, percent */
    /* endless: a goal every mile_step waves. Reaching one pays cores (kept
     * in cores_won until the battle ends), credits and some of the fort's
     * life, and is a star of the three the mode gives */
    int        mile_step, miles, mile_cores, mile_credits, mile_lives;
    float      mile_t;              /* its banner, seconds left */
    uint32_t   roster;              /* endless: the aliens this world has */
    int        alive;
    float      freeze_t, shake, fort_flash, alarm_t;
    float      power_cd[PW_N];
    float      k_dmg, k_range, k_bounty, k_sell, k_interest, k_crit;
    bool       radar_dirty;

    bs_alien_t al[BS_MAX_ALIENS];
    bs_tower_t tw[BS_PADS];
    bs_shot_t  shot[BS_MAX_SHOTS];
    bs_fx_t    fx[BS_MAX_FX];
    bs_part_t  part[BS_MAX_PARTS];
    int        npart;
    bs_float_t fl[BS_MAX_FLOATS];

    int        kills, earned, xp, leaked, built, stars, cores_won;
    uint32_t   seen;                /* the aliens that came on screen (bits of AL_*) */
    uint16_t   leak_by[AL_N];       /* who got through, by kind */
    uint8_t    lives_at[BS_WAVES];  /* the fort's life after each wave */
    uint8_t    snd[24];
    int        nsnd;
    uint32_t   rng;
    uint16_t   next_id;
    float      snd_cool[SND_N];
} bs_game_t;

void bs_game_start(bs_game_t *g, const bs_world_t *w, int sector, const bs_kit_t *kit);
void bs_game_step(bs_game_t *g);
/* the next wave, when the last one is done */
bool bs_game_call_wave(bs_game_t *g);
bool bs_game_build(bs_game_t *g, int pad, int type);
bool bs_game_upgrade(bs_game_t *g, int pad);
bool bs_game_sell(bs_game_t *g, int pad);
void bs_game_mode(bs_game_t *g, int pad);
/* a power, at a point of the ground for the ones that ask for one */
bool bs_game_power(bs_game_t *g, int kind, float x, float y);

int   bs_game_cost(const bs_game_t *g, int type, int tier);
int   bs_game_sell_value(const bs_game_t *g, int pad);
float bs_game_range(const bs_game_t *g, int type, int tier);
float bs_game_dmg(const bs_game_t *g, int type, int tier);
/* the wave about to come: how many of each alien (counts[AL_N]) */
void  bs_game_preview(const bs_game_t *g, int *counts);
void  bs_game_sound(bs_game_t *g, int snd);
