/*
 * DERIVA - the game: rules and physics, with no screen and no LVGL, so
 * that tools/harness.c can play it on the Mac.
 *
 * The field is a rectangle that wraps at its edges (its size is the
 * screen's minus the control deck, in pixels). Steps are fixed, 1/120 s.
 * A ship turns, pushes with inertia and shoots; rocks split when hit:
 *
 *   stone     in two, down to the small ones
 *   trio      in three
 *   metal     takes several hits, and shots spark off it
 *   volatile  blows up and takes what is near with it (chains)
 *   lapa      a soft green one that drifts towards the ship and sticks to
 *             the hull: each one slows it, three crush it; a jump leaves
 *             them behind
 *
 * Enemies: the seeker (a dart that chases), the layer (drops mines as it
 * crosses), the lancer (aims with a line, then charges), the weaver (rides
 * a wave and shoots slow orbs). Bosses every fourth sector of the campaign
 * and every sixth wave of the arcade: the Colossus (a rock in armour
 * plates), the Hive (turrets and a hangar of seekers), the Serpent (rings
 * shot off from the tail). Powers fall from enemies and big rocks: triple
 * shot, shield, beam, magnet, a life. Crystals fall from everything and
 * buy upgrades between sectors.
 *
 * The game says what happened as events (for the picture: sparks,
 * shakes) and sounds (for the speaker); it never draws.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AG_DT       (1.0f / 120.0f)
#define AG_ROCKS    120
#define AG_SHOTS    40
#define AG_EBULS    64
#define AG_ENEMIES  16
#define AG_PICKS    8
#define AG_CRYST    96
#define AG_EVENTS   96
#define AG_SNDS     32
#define AG_SECTORS  12
#define AG_SEGS     14
#define AG_TRAIL    512

#define AG_SHIP_R   15.0f

enum { RK_STONE, RK_TRIO, RK_METAL, RK_VOLATILE, RK_LAPA, RK_N };
enum { EN_SEEKER, EN_LAYER, EN_LANCER, EN_WEAVER, EN_N };
enum { BOSS_NONE, BOSS_COLOSO, BOSS_COLMENA, BOSS_SERPIENTE };
enum { PW_TRIPLE, PW_SHIELD, PW_BEAM, PW_MAGNET, PW_LIFE, PW_N };
enum { EB_SHOT, EB_ORB, EB_MINE };
enum { AG_CAMPAIGN, AG_ARCADE };
enum { AG_EASY, AG_NORMAL, AG_HARD, AG_DIFF_N };
/* the upgrades bought between sectors */
enum { UP_ENGINE, UP_GUN, UP_ARMOR, UP_JUMP, UP_MAGNET, UP_N };

enum {
    AG_ST_INTRO,        /* the sector's or wave's banner                       */
    AG_ST_PLAY,
    AG_ST_CLEAR,        /* a wave cleared: a breath before the next            */
    AG_ST_BOSS_IN,      /* the boss's warning                                  */
    AG_ST_DONE,         /* the sector is over (the campaign's workshop)        */
    AG_ST_OVER,         /* no lives left                                       */
    AG_ST_WON           /* the last sector cleared                             */
};

/* events, for the picture */
enum {
    EV_BOOM,            /* a rock broke: a = size, b = type                     */
    EV_SPARK,           /* a shot on armour                                     */
    EV_BLAST,           /* an explosion's ring: a = radius                      */
    EV_SHIP_DIE,        /* a = the ship                                         */
    EV_JUMP_OUT, EV_JUMP_IN,
    EV_PICK,            /* a = the power                                        */
    EV_CRYSTAL,
    EV_SHIELD,          /* the shield took a hit                                */
    EV_STICK,           /* a lapa took hold                                     */
    EV_ENEMY_DIE,       /* a = kind                                             */
    EV_BOSS_DIE,
    EV_BOSS_PART,       /* a plate, a turret, a ring came off                   */
    EV_MUZZLE,          /* a = the ship                                         */
    EV_LIFE,
    EV_DASH,
};

/* sounds, for the speaker */
enum {
    SND_SHOT, SND_SHOT3, SND_BEAM, SND_BOOM_S, SND_BOOM_M, SND_BOOM_L, SND_CLANG, SND_BLAST, SND_STICK,
    SND_JUMP, SND_PICK, SND_CRYSTAL, SND_SHIELD, SND_DIE, SND_ESHOT, SND_ALARM, SND_WAVE, SND_LIFE,
    SND_MINE, SND_DASH, SND_BOSS_HIT, SND_BOSS_DIE, SND_UI, SND_N
};

typedef struct {
    bool     on;
    uint8_t  type, size, nv;
    int8_t   stuck;             /* the ship it holds on to, or -1           */
    float    x, y, vx, vy, ang, spin, r, hp, flash;
    float    stuck_ang, stuck_t, stun, fuse;
    uint8_t  shape[12];         /* each corner's radius, /128 of r          */
} ag_rock_t;

typedef struct {
    bool    on;
    uint8_t owner;
    float   x, y, vx, vy, life;
} ag_shot_t;

typedef struct {
    bool    on;
    uint8_t kind;
    float   x, y, vx, vy, life, arm;
} ag_ebul_t;

typedef struct {
    bool    on;
    uint8_t kind, state;
    float   x, y, vx, vy, ang, hp, t, t2, flash, tx, ty;
} ag_enemy_t;

typedef struct {
    bool    on;
    uint8_t kind;
    float   x, y, vx, vy, life, ang;
} ag_pick_t;

typedef struct {
    bool  on;
    float x, y, vx, vy, life;
} ag_cryst_t;

typedef struct {
    bool  in_play;              /* still has a life to play                 */
    bool  alive;
    float x, y, vx, vy, ang;
    float inv_t, respawn_t, jump_t, jump_cd, fire_cd;
    float jx, jy;               /* where the jump comes out                 */
    float wait_t;               /* waiting for room to come back            */
    bool  beam_on;
    float bx, by;               /* where the beam ends                      */
    int   lives;
    int   shield;
    float shield_t;
    float pw_t[PW_N];
    int   lapas;
    float thrust;               /* 0..1 this step                           */
    float turn;                 /* how hard it turned                       */
    bool  firing;
} ag_ship_t;

typedef struct {
    bool    on;
    uint8_t kind, state;
    float   x, y, vx, vy, ang, spin, t, t2, hp, hp_max, flash, r;
    int     nparts;
    float   part_hp[8], part_flash[8];
    int     nseg;
    float   seg_x[AG_SEGS], seg_y[AG_SEGS], seg_hp[AG_SEGS], seg_flash[AG_SEGS];
    float   trail_x[AG_TRAIL], trail_y[AG_TRAIL];
    int     trail_i;
    float   dir;
} ag_boss_t;

/* a hand on the controls, one step */
typedef struct {
    float turn;                 /* -1..1, the turn buttons                  */
    bool  aim;                  /* a stick: turn towards (ax, ay)           */
    float ax, ay;
    float thrust;               /* 0..1                                     */
    bool  fire;
    bool  jump;                 /* pressed since the last step              */
} ag_hand_t;

typedef struct {
    uint8_t  kind;
    float    x, y, a, b;
} ag_event_t;

typedef struct {
    float    W, H;
    int      mode, players, diff;
    int      sector;            /* campaign: 1..12; arcade: the wave        */
    int      wave, waves;
    int      boss_kind;
    int      state;
    float    state_t, t;
    uint32_t rnd;
    int      score, crystals, crystals_run;
    int      next_life;
    int      upg[UP_N];
    float    speed_k;
    float    enemy_t;
    int      enemy_left;
    uint8_t  enemy_kinds;       /* a bit per kind this wave may send        */
    uint8_t  rock_kinds;
    float    since_kill;        /* for the harness: a wave that drags on    */

    ag_ship_t  ship[2];
    ag_rock_t  rock[AG_ROCKS];
    ag_shot_t  shot[AG_SHOTS];
    ag_ebul_t  ebul[AG_EBULS];
    ag_enemy_t en[AG_ENEMIES];
    ag_pick_t  pick[AG_PICKS];
    ag_cryst_t cryst[AG_CRYST];
    ag_boss_t  boss;

    ag_event_t ev[AG_EVENTS];
    int        nev;
    uint8_t    snd[AG_SNDS], snd_vol[AG_SNDS];
    int        nsnd;
    float      shake;           /* added by explosions, the picture decays it */
    int        beam_snd_t;

    /* what happened, for the harness and the log */
    int        deaths, kills, bosses, jumps, picks, hits_taken;
    int        cause[6];        /* lives lost to: a rock, lapas, an enemy, a shot, a blast, a boss */
} ag_game_t;

/* the campaign's sectors: what each one sends (the names and the colours
 * are the picture's, ast_scene.c) */
typedef struct {
    uint8_t rocks;              /* a bit per rock type                     */
    uint8_t enemies;            /* a bit per enemy kind                    */
    uint8_t boss;
    uint8_t waves;
    float   speed;
    uint8_t big;                /* big rocks in the first wave             */
} ag_sector_t;

extern const ag_sector_t AG_SECTOR[AG_SECTORS];

/* the workshop: each upgrade's levels and what the next one costs */
int  ag_upg_max(int u);
int  ag_upg_cost(int u, int level);

/* a new game: players 1 or 2; the field's size */
void ag_new(ag_game_t *g, int mode, int players, int diff, float w, float h, uint32_t seed);
/* a sector of the campaign (1..12), keeping lives, score and upgrades */
void ag_sector_start(ag_game_t *g, int sector);
/* the field changed size (the screen turned): everything keeps its place */
void ag_resize(ag_game_t *g, float w, float h);
/* one step of 1/120 s; hands[i] NULL: that ship's player does nothing */
void ag_step(ag_game_t *g, const ag_hand_t *h0, const ag_hand_t *h1);

void ag_wave_jump(ag_game_t *g, int w);
int  ag_rocks_on(const ag_game_t *g);

/* the menus' still, in the field's present size */
void ag_title_scene(ag_game_t *g, int sector);

/* helpers the picture and the bot share */
float ag_wrapd(float d, float size);
static inline float ag_rand01(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x9E3779B9u;
    return (float)(x >> 8) / 16777216.0f;
}
