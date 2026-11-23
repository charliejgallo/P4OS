/*
 * TANQUES - the game's tables: the tanks, their upgrades, the weapons, the
 * paints and the emblems, the places, the modes, the story, and what the
 * player owns (the save).
 *
 * Two DLCs add to the game: "Misión a la Luna" (the Tortuga, the Tesla coil,
 * the Moon, waves and the hill, six missions) and "Día de playa" (the
 * Anfibio, the double cannon, the beach, the flag and the treasure, six
 * missions). Their code is all here; what makes one installed is its art
 * pack on the card (tq_art.c), and what is not installed shows locked.
 */
#pragma once

#include "tq_view.h"

/* --------------------------------------------------------------------------
 * Tanks
 * -------------------------------------------------------------------------- */

enum { TQ_T_BOMBO = 0, TQ_T_PULGA, TQ_T_MAMUT, TQ_T_OBUS, TQ_T_AVISPA, TQ_T_TOPO, TQ_T_TORTUGA, TQ_T_ANFIBIO,
       TQ_TANK_TYPES };

/* --------------------------------------------------------------------------
 * The DLCs
 * -------------------------------------------------------------------------- */

enum { TQ_DLC_BASE = 0, TQ_DLC_LUNA, TQ_DLC_PLAYA, TQ_DLCS };
extern const char *const TQ_DLC_NAME[TQ_DLCS];      /* N_() */
extern const char *const TQ_DLC_PACK[TQ_DLCS];      /* the art pack that makes it installed */
/* Which DLCs are installed (bit per DLC; the base always): set by the art */
void tq_dlc_set(uint32_t mask);
bool tq_dlc_have(int dlc);
int  tq_dlc_of_tank(int type);
int  tq_dlc_of_weapon(int w);
int  tq_dlc_of_biome(int biome);
int  tq_dlc_of_mode(int mode);

/* What the art knows too (tools/blender/tanques_art.py, TANKS) */
typedef struct {
    const char *key;            /* the art's name */
    const char *name;           /* N_() */
    const char *role;           /* N_(): one line for the hangar */
    int   price;
    int   hp, armor;            /* armour: % of each hit taken off */
    int   ap;
    float m_per_ap;             /* flat ground an AP drives */
    float climb;                /* how much uphill costs (higher: more) */
    int   slots;                /* weapons it carries */
    float pivot_x, pivot_z;     /* the barrel's pivot */
    float muzzle;               /* the muzzle's distance from the pivot */
    float eye_x, eye_y, eye_z;  /* the gunner's eye */
    float sprite_pitch;         /* the barrel's pitch in the sprites, deg */
    float body_r;               /* the hit ellipsoid's radius, m */
} tq_tank_type_t;

extern const tq_tank_type_t TQ_TANK[TQ_TANK_TYPES];

enum { TQ_UP_ARMOR = 0, TQ_UP_ENGINE, TQ_UP_SPEED, TQ_UP_ENERGY, TQ_UPGRADES };
#define TQ_UP_MAX 4

extern const char *const TQ_UP_NAME[TQ_UPGRADES];      /* N_() */
extern const char *const TQ_UP_INFO[TQ_UPGRADES];      /* N_() */
int tq_up_price(int tank_type, int track, int level);  /* to reach level from level-1 */

/* A tank's numbers with its upgrades */
typedef struct {
    int   hpmax, armor, apmax;
    float m_per_ap, climb, max_slope;
} tq_stats_t;

void tq_stats(int type, const uint8_t up[TQ_UPGRADES], tq_stats_t *out);

/* --------------------------------------------------------------------------
 * Weapons
 * -------------------------------------------------------------------------- */

enum {
    TQ_W_CANNON = 0, TQ_W_MORTAR, TQ_W_MISSILE, TQ_W_CLUSTER, TQ_W_LASER,
    TQ_W_NAPALM, TQ_W_MINES, TQ_W_EMP, TQ_W_SHIELD, TQ_W_REPAIR,
    TQ_W_SMOKE, TQ_W_BOUNCER, TQ_W_DRILL, TQ_W_AIRSTRIKE, TQ_W_SNIPER,
    TQ_W_SCATTER, TQ_W_QUAKE, TQ_W_TELEPORT, TQ_W_WALL, TQ_W_BIGBOMB,
    TQ_W_TESLA, TQ_W_TWIN,      /* the DLCs' */
    TQ_WEAPONS
};

/* How it flies */
enum { TQ_FLY_SHELL = 0, TQ_FLY_LOB, TQ_FLY_GUIDED, TQ_FLY_BEAM, TQ_FLY_BOUNCE, TQ_FLY_DRILL, TQ_FLY_SELF };
/* What it does where it lands */
enum {
    TQ_FX_BLAST = 0, TQ_FX_CLUSTER, TQ_FX_NAPALM, TQ_FX_MINES, TQ_FX_EMP, TQ_FX_SHIELD, TQ_FX_REPAIR,
    TQ_FX_SMOKE, TQ_FX_AIRSTRIKE, TQ_FX_SCATTER, TQ_FX_QUAKE, TQ_FX_TELEPORT, TQ_FX_WALL, TQ_FX_TESLA,
};

typedef struct {
    const char *name;           /* N_() */
    const char *info;           /* N_(): one line for the hangar */
    int   ap;
    int   price;                /* 0: everyone has it */
    int   fly, fx;
    float speed;                /* m/s at the muzzle */
    float gravity;              /* x g */
    float radius;               /* the blast's reach */
    int   damage;
    float crater_r, crater_d;
    int   uses;                 /* a battle's ammunition; 0: no limit */
    float min_pitch, max_pitch; /* deg */
    uint32_t rgb;               /* its colour on the buttons and the shell */
} tq_weapon_t;

extern const tq_weapon_t TQ_WEAPON[TQ_WEAPONS];

#define TQ_W_LEVELS 3           /* upgrade levels beyond the base */
int   tq_w_price(int weapon, int level);   /* to reach level from level-1 */
float tq_w_damage_k(int level);            /* x damage */
float tq_w_radius_k(int level);            /* x radius */
/* How much of the arc the gunner sees: 0..1 (1 = all, and the landing) */
float tq_w_guide(int weapon, int level, int difficulty);

/* --------------------------------------------------------------------------
 * Paint and emblems
 * -------------------------------------------------------------------------- */

#define TQ_PAINTS   12
#define TQ_EMBLEMS  12
extern const uint32_t TQ_PAINT[TQ_PAINTS];
extern const char *const TQ_PAINT_NAME[TQ_PAINTS];          /* N_() */
/* An emblem as 16 x 16 bits, a row a uint16_t, bit 15 the left (drawn by
 * code: a star, a heart, a bolt...) */
void tq_emblem(int k, uint16_t out[16]);

/* --------------------------------------------------------------------------
 * The save: what the player has
 * -------------------------------------------------------------------------- */

#define TQ_STORY_MISSIONS 24     /* twelve of the game, six of each DLC */
#define TQ_SLOTS_MAX 4

typedef struct {
    uint8_t owned;
    uint8_t up[TQ_UPGRADES];
    uint8_t load[TQ_SLOTS_MAX];         /* weapons, 0xFF empty */
    uint8_t paint1, paint2, emblem, emblem_paint;
} tq_garage_t;

typedef struct {
    uint32_t version;
    int32_t  coins;
    uint8_t  tank;                      /* the one picked */
    tq_garage_t g[TQ_TANK_TYPES];
    uint8_t  w_owned[TQ_WEAPONS];
    uint8_t  w_level[TQ_WEAPONS];
    uint8_t  stars[TQ_STORY_MISSIONS];  /* 0 = not won; 1..3 */
    /* the free battle's last settings */
    uint8_t  f_biome, f_size, f_rivals, f_diff, f_weather, f_players;
    uint8_t  sfx;
    uint32_t wins, battles;
    uint8_t  f_objects;                 /* the free battle's trees and walls: 1 + level (0: the middle) */
    uint8_t  f_mode;                    /* the free battle's mode */
    uint8_t  best_wave;                 /* the most waves lived through */
    uint8_t  spare[5];
} tq_save_t;

/* How many trees and walls: five levels, the middle one the usual */
#define TQ_OBJECT_LEVELS 5
extern const float TQ_OBJECTS_K[TQ_OBJECT_LEVELS];
extern const char *const TQ_OBJECTS_NAME[TQ_OBJECT_LEVELS];   /* N_() */
static inline int tq_objects_level(const tq_save_t *s)
{
    return s->f_objects ? (s->f_objects - 1) % TQ_OBJECT_LEVELS : TQ_OBJECT_LEVELS / 2;
}

void tq_save_defaults(tq_save_t *s);
bool tq_save_load(tq_save_t *s);
void tq_save_store(const tq_save_t *s);

/* --------------------------------------------------------------------------
 * The story
 * -------------------------------------------------------------------------- */

/* The modes: the usual (the last team standing), and the DLCs' four. In
 * each, knocking all the others out wins too. */
enum { TQ_MODE_CLASSIC = 0, TQ_MODE_WAVES, TQ_MODE_HILL, TQ_MODE_FLAG, TQ_MODE_TREASURE, TQ_MODES };
extern const char *const TQ_MODE_NAME[TQ_MODES];   /* N_() */
extern const char *const TQ_MODE_INFO[TQ_MODES];   /* N_(): one line */

/* What a mission asks besides its mode */
enum {
    TQ_GOAL_ALL = 0,        /* knock them all out */
    TQ_GOAL_BOSS,           /* the first rival is a boss: three times the health */
    TQ_GOAL_HOLD,           /* still standing after n rounds */
    TQ_GOAL_PROTECT,        /* an ally of the board's must live */
    TQ_GOAL_FAST,           /* win within n rounds */
};

typedef struct {
    const char *name;           /* N_() */
    const char *info;           /* N_(): what it asks, one line (NULL: knock them out) */
    uint8_t dlc;
    uint8_t biome, size, rivals, diff, weather;
    uint8_t rival_tank[3];
    uint8_t mode, goal, goal_n; /* goal_n: rounds, waves, points... */
    uint8_t ally_tank;          /* TQ_GOAL_PROTECT's */
    uint32_t seed;
    int reward;
} tq_mission_t;

extern const tq_mission_t TQ_MISSION[TQ_STORY_MISSIONS];
/* The first mission of a DLC's six (or of the game's twelve), and how many */
int tq_missions_of(int dlc, int *first);

enum { TQ_B_PRADERA = 0, TQ_B_DESIERTO, TQ_B_NIEVE, TQ_B_RUINAS, TQ_B_VOLCAN, TQ_B_LUNA, TQ_B_PLAYA, TQ_BIOMES };
/* The Moon pulls less: the shells' gravity there */
#define TQ_MOON_G 0.55f
extern const char *const TQ_BIOME_KEY[TQ_BIOMES];
extern const char *const TQ_BIOME_NAME[TQ_BIOMES];        /* N_() */
enum { TQ_WX_CLEAR = 0, TQ_WX_WIND, TQ_WX_STORM, TQ_WX_FOG, TQ_WEATHERS };
extern const char *const TQ_WEATHER_NAME[TQ_WEATHERS];    /* N_() */
#define TQ_SIZES 4
extern const float TQ_SIZE_M[TQ_SIZES];
extern const char *const TQ_SIZE_NAME[TQ_SIZES];          /* N_() */
#define TQ_DIFFS 3
extern const char *const TQ_DIFF_NAME[TQ_DIFFS];          /* N_() */
