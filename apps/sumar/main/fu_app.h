/*
 * P4OS - Fusión: what the files share
 *
 *   fusion.c    the app's entry, its life cycle and its icon
 *   fu_core.c   the rules: sliding a line, merging, spawning, blocks, undo
 *   fu_art.c    the four themes and the tiles they draw (sprites made in code)
 *   fu_snd.c    the sounds, synthesised once and mixed into the speaker
 *   fu_ui.c     the screens, the animation and the input (touch, pad, keys)
 *   fu_save.c   the game in course, the records and the achievements
 *
 * A tile is stored as its exponent: 1 is a 2, 2 a 4, 11 a 2048. 0 is an
 * empty cell and FU_BLOCK a stone that never moves (the Bloques mode).
 */
#pragma once

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"
#include "aos_theme.h"
#include "aos_fonts.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#define FU_NMIN      3
#define FU_NMAX      6
#define FU_CELLS     (FU_NMAX * FU_NMAX)
#define FU_BLOCK     0xFF
#define FU_EXP_MAX   17          /* 131072: as far as a 6x6 can go */
#define FU_UNDO_DEPTH 4

enum { FU_UP, FU_DOWN, FU_LEFT, FU_RIGHT };

enum {
    FU_MODE_CLASSIC = 0,
    FU_MODE_TIME,
    FU_MODE_BLOCKS,
    FU_MODE_DUO,
    FU_MODES
};

/* ---- the rules (fu_core.c) ---- */

typedef struct {
    uint8_t  v[FU_NMAX][FU_NMAX];   /* exponent, 0 empty, FU_BLOCK */
    uint16_t id[FU_NMAX][FU_NMAX];  /* which view shows that tile */
} fu_grid_t;

/* One tile's travel in a move. 'gone' is the one swallowed by a merge: it
 * slides into the other and disappears; the one it joins grows. */
typedef struct {
    uint16_t id;
    int8_t   r0, c0, r1, c1;
    bool     gone;
} fu_slide_t;

typedef struct {
    uint16_t id;                    /* the tile that stays and grows */
    int8_t   r, c;
    uint8_t  exp;                   /* what it becomes */
} fu_merge_t;

typedef struct {
    bool       moved;
    int        nslides, nmerges;
    fu_slide_t slide[FU_CELLS];
    fu_merge_t merge[FU_CELLS / 2];
    uint32_t   points;
    uint8_t    top;                 /* biggest exponent made in this move */
} fu_move_t;

void     fu_rng_seed(uint32_t seed);
uint32_t fu_rng(void);
uint16_t fu_new_id(void);
bool     fu_core_move(fu_grid_t *g, int n, int dir, fu_move_t *out);
bool     fu_core_spawn(fu_grid_t *g, int n, int *r, int *c);
bool     fu_core_can_move(const fu_grid_t *g, int n);
int      fu_core_top(const fu_grid_t *g, int n);
void     fu_core_blocks(fu_grid_t *g, int n, int count);

/* ---- the game in course ---- */

typedef struct {
    fu_grid_t g;
    uint32_t  score, score2;        /* score2: the second player in Duo */
    uint8_t   turn;                 /* 0 or 1 in Duo */
    uint32_t  moves;
} fu_snap_t;

typedef struct {
    bool      active;               /* a game is on (or paused) */
    bool      over;
    uint8_t   n, mode;
    fu_grid_t g;
    uint32_t  score, score2;
    uint8_t   turn;
    uint32_t  moves;
    uint8_t   goal;                 /* the exponent to reach next */
    uint8_t   level;                /* goals reached */
    uint8_t   undo_left;
    bool      used_undo;            /* since the last goal (an achievement) */
    int32_t   time_ms;              /* left, in the timed mode */
    uint8_t   nundo;
    fu_snap_t undo[FU_UNDO_DEPTH];
    uint8_t   winner;               /* Duo: 0, 1, or 2 for a tie */
} fu_game_t;

extern fu_game_t fu_G;

void fu_game_new(int n, int mode);
uint8_t fu_goal_first(int n, int mode);

/* ---- themes and art (fu_art.c) ---- */

enum { FU_ST_NEON, FU_ST_GEM, FU_ST_PAPER, FU_ST_FOREST };
#define FU_THEMES 4

typedef struct {
    const char *name;               /* N_() */
    uint8_t     style;
    uint32_t    bg_top, bg_bot;     /* the screen */
    uint32_t    board, slot;        /* the board and its empty cells */
    uint32_t    card, card_line;    /* HUD cards */
    uint32_t    text, dim;
    uint32_t    accent, accent2;    /* the two players, the goal */
    uint32_t    block;
    uint32_t    ramp[12];           /* tile colours from 2 up */
} fu_theme_t;

typedef struct {
    lv_image_dsc_t dsc;
    uint8_t       *data;
} fu_sprite_t;

const fu_theme_t *fu_theme(void);
const fu_theme_t *fu_theme_at(int i);
void fu_art_setup(int theme, int cell);     /* drops the sprites if either changed */
const lv_image_dsc_t *fu_art_tile(uint8_t exp);   /* rendered on first use */
void fu_art_free(void);
uint32_t fu_art_color(uint8_t exp);         /* a tile's main colour (sparks, pops) */
void fu_value_text(uint8_t exp, char *out, int len);

/* ---- sounds (fu_snd.c) ---- */

enum { FU_S_SLIDE, FU_S_MERGE, FU_S_SPAWN, FU_S_GOAL, FU_S_OVER, FU_S_UNDO, FU_S_DENY, FU_S_TICK, FU_SOUNDS };
void fu_snd_open(void);
void fu_snd_close(void);
void fu_snd_play(int snd, int pitch);       /* pitch: the exponent for merges */
void fu_snd_pump(void);
void fu_snd_free(void);

/* ---- saved things (fu_save.c) ---- */

typedef struct {
    uint8_t  n, mode, theme;
    bool     sound;
    uint32_t ach;                   /* achievements, a bit each */
    uint8_t  themes_used;
    uint8_t  sizes_played;
} fu_prefs_t;

extern fu_prefs_t fu_P;

void     fu_prefs_load(void);
void     fu_prefs_save(void);
uint32_t fu_record(int n, int mode);
uint8_t  fu_record_tile(int n, int mode);
bool     fu_record_offer(int n, int mode, uint32_t score, uint8_t tile);  /* true: new record */
void     fu_game_save(void);
bool     fu_game_load(void);
void     fu_game_forget(void);

enum {
    FU_ACH_128, FU_ACH_512, FU_ACH_1024, FU_ACH_4096, FU_ACH_POCKET, FU_ACH_CLEAN,
    FU_ACH_TRIPLE, FU_ACH_CLOCK, FU_ACH_STONES, FU_ACH_DUO, FU_ACH_MARATHON, FU_ACH_THEMES,
    FU_ACHS
};
const char *fu_ach_title(int i);            /* untranslated: _() at the use */
const char *fu_ach_text(int i);

/* ---- the screens (fu_ui.c) ---- */

void fu_ui_create(lv_obj_t *root, aos_app_t *self);
void fu_ui_rebuild(lv_obj_t *root);         /* a turn of the screen */
void fu_ui_destroy(void);
void fu_ui_frame(void);
void fu_ui_hide(void);
void fu_ui_show(void);
bool fu_ui_back(void);
bool fu_ui_key(uint32_t key, uint8_t mods);
