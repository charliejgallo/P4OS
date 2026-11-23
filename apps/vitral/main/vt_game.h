/*
 * VITRAL - the rules (no LVGL, no drawing, integers only)
 *
 * A well of glass. Pieces of three and five squares fall into it; a row
 * that is full shatters. A full row whose glass is all one colour is a
 * "vitral": it scores three times and takes the row under it with it.
 * Clear glass goes with any colour.
 *
 * The well is VT_ROWS rows tall, the top VT_HIDDEN of them above the
 * window (pieces are born there). Row 0 is the top. Columns: up to
 * VT_COLS_MAX; every mode uses 12 (the puzzle closes the ones outside its
 * window with stone).
 *
 * The game only keeps state and reports what happened in ev (bits) and in
 * the fx_* fields; the view and the sound read them after each step and
 * call vt_events_clear().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VT_COLS_MAX     12
#define VT_HIDDEN       4
#define VT_VISIBLE      22
#define VT_ROWS         (VT_HIDDEN + VT_VISIBLE)
#define VT_QUEUE        5           /* pieces known ahead */
#define VT_PZ_MAX_SEQ   24          /* pieces in a puzzle, at most */

/* what a cell holds */
enum {
    VT_EMPTY = 0,
    VT_RUBY, VT_AMBER, VT_EMERALD, VT_SAPPHIRE,     /* the four colours      */
    VT_CLEAR,                                       /* goes with any colour  */
    VT_FROST,                                       /* rubble sent by the rival */
    VT_STONE,                                       /* the puzzle's frame    */
};
#define VT_NCOLORS 4

/* the pieces: two of three squares, eighteen of five (the twelve
 * pentominoes, the six that are not symmetric twice, once per side) */
enum {
    PC_I3, PC_V3,
    PC_I5, PC_L, PC_J, PC_P, PC_Q, PC_T, PC_U, PC_V, PC_W, PC_X,
    PC_Y, PC_Y2, PC_N, PC_N2, PC_Z, PC_S, PC_F, PC_F2,
    PC_COUNT
};

typedef enum {
    VT_MODE_MARATHON,
    VT_MODE_TIME,           /* three minutes */
    VT_MODE_PUZZLE,
    VT_MODE_VERSUS,
} vt_mode_t;

typedef enum {
    VT_PLAY,
    VT_CLEARING,            /* the rows flash, then shatter            */
    VT_OVER,                /* topped out (or the clock ran out)       */
    VT_WON,                 /* the puzzle's window is full             */
    VT_STUCK,               /* the puzzle ran out of pieces            */
} vt_state_t;

/* events of the last step(s), for the view and the sound */
enum {
    EV_MOVE     = 1u << 0,
    EV_ROTATE   = 1u << 1,
    EV_LOCK     = 1u << 2,
    EV_HARD     = 1u << 3,
    EV_CLEAR    = 1u << 4,      /* rows began to flash (fx_rows)          */
    EV_SHATTER  = 1u << 5,      /* they broke: the board changed under them */
    EV_VITRAL   = 1u << 6,      /* among them, a row of one colour         */
    EV_LEVEL    = 1u << 7,
    EV_HOLD     = 1u << 8,
    EV_OVER     = 1u << 9,
    EV_REJECT   = 1u << 10,     /* puzzle: the piece does not fit there    */
    EV_GARBAGE  = 1u << 11,     /* rubble came up from below               */
    EV_WON      = 1u << 12,
    EV_SPAWN    = 1u << 13,
    EV_BOARD    = 1u << 14,     /* the settled glass changed: redraw it    */
    EV_STUCK    = 1u << 15,
    EV_UNDO     = 1u << 16,
    EV_SOFT     = 1u << 17,     /* a step down by hand                     */
    EV_KICKFAIL = 1u << 18,     /* a turn that did not fit                 */
};

typedef struct {
    int8_t x, y;
} vt_pt_t;

typedef struct {
    uint8_t n;                  /* 3 or 5 */
    vt_pt_t c[4][5];            /* cells per rotation, in a 5x5 box */
} vt_shape_t;

extern vt_shape_t vt_shapes[PC_COUNT];     /* filled by vt_init() */

typedef struct {
    uint8_t type, color;
} vt_next_t;

typedef struct {
    int8_t  x, y;
    uint8_t rot;
} vt_pz_place_t;

typedef struct {
    uint8_t  type, rot, color;
    int8_t   x, y;              /* the 5x5 box's corner, in cells */
    uint16_t pid;               /* which piece of glass: the lead goes round it */
} vt_piece_t;

typedef struct {
    vt_mode_t  mode;
    vt_state_t state;
    uint8_t    cols;
    uint8_t    col[VT_ROWS][VT_COLS_MAX];
    uint16_t   pid[VT_ROWS][VT_COLS_MAX];
    uint8_t    mask[VT_ROWS][VT_COLS_MAX];   /* puzzle: the window to fill */

    vt_piece_t cur;
    bool       has_cur;
    vt_next_t  queue[VT_QUEUE];
    vt_next_t  hold;            /* type 0xFF = nothing held */
    bool       hold_used;
    uint8_t    bag[32];
    uint8_t    bag_n, bag_i;
    uint32_t   rng;
    uint16_t   next_pid;

    uint32_t   score;
    uint16_t   lines;
    uint8_t    level;
    uint8_t    start_level;
    uint16_t   combo;
    uint16_t   vitrals;         /* rows of one colour, this game */
    uint32_t   time_ms;         /* playing time */
    uint32_t   time_limit;      /* 0 = none */

    /* the fall */
    uint32_t   grav_acc;        /* ms towards the next row */
    uint32_t   lock_acc;
    uint8_t    lock_resets;
    int8_t     lowest;          /* the lowest row the piece reached */
    bool       soft;            /* held soft drop */
    uint32_t   soft_acc;

    /* the clearing animation */
    uint32_t   clear_t;
    uint8_t    clear_n;
    uint8_t    clear_row[8];
    uint8_t    clear_vit[8];    /* 1 = this row was one colour, 2 = taken by one */

    /* versus */
    uint8_t    garbage_in;      /* rows on their way up */
    uint8_t    garbage_out;     /* rows to send (the app moves them over) */
    uint32_t   gseed;

    /* puzzle */
    uint8_t    pz_level;
    uint8_t    pz_try;          /* which try of the generator tiled it */
    uint8_t    pz_n, pz_i;
    vt_next_t  pz_seq[VT_PZ_MAX_SEQ];
    vt_pz_place_t pz_sol[VT_PZ_MAX_SEQ];   /* where the generator put each */
    uint8_t    pz_cells;        /* mask cells */
    uint8_t    pz_x0, pz_x1;    /* the open columns */
    uint8_t    undo_n;
    uint8_t   *undo;            /* pz_n snapshots, allocated by the app */

    /* what happened */
    uint32_t   ev;
    uint8_t    fx_lock_n;
    vt_pt_t    fx_lock[5];
    uint8_t    fx_lock_color;
    int8_t     fx_hard_from, fx_hard_to;
    vt_piece_t fx_piece;        /* the piece as it locked / was rejected */
    uint8_t    fx_rows_n;
    uint8_t    fx_rows[8];
    uint8_t    fx_rows_vit[8];
    uint8_t    fx_rows_col[8][VT_COLS_MAX];
    uint16_t   fx_rows_pid[8][VT_COLS_MAX];
    uint8_t    fx_cleared;      /* rows in the last clear (with the blasts) */
    uint32_t   fx_points;       /* points of the last clear */
} vt_game_t;

/* timing (ms) */
#define VT_LOCK_MS        500
#define VT_LOCK_RESETS    15
#define VT_CLEAR_FLASH_MS 150
#define VT_CLEAR_MS       300
#define VT_SOFT_MS        28
#define VT_TIME_LIMIT     (3u * 60u * 1000u)

void vt_init(vt_game_t *g, vt_mode_t mode, uint32_t seed, int level);
void vt_events_clear(vt_game_t *g);

/* input; true = it did something */
bool vt_move(vt_game_t *g, int dx);
bool vt_rotate(vt_game_t *g, int dir);     /* +1 clockwise, -1 the other way */
bool vt_down(vt_game_t *g);                /* one row by hand */
void vt_hard(vt_game_t *g);
bool vt_hold(vt_game_t *g);
void vt_soft(vt_game_t *g, bool on);

void vt_tick(vt_game_t *g, uint32_t ms);

/* where the piece would land */
int  vt_ghost_y(const vt_game_t *g);
/* the fraction (0..255) of the next row the piece has fallen, for drawing it
 * between rows; 0 when it rests */
int  vt_fall_frac(const vt_game_t *g);
bool vt_fits(const vt_game_t *g, int type, int rot, int x, int y);
/* the gravity of a level, ms per row */
uint32_t vt_gravity_ms(int level);

/* versus: rows of rubble coming */
void vt_add_garbage(vt_game_t *g, int rows);

/* puzzle */
#define VT_PZ_LEVELS 30
/* builds level 'level' (0-based) into g (already vt_init'ed in PUZZLE);
 * false if the generator gave up (should not happen with the seed table). */
bool vt_pz_build(vt_game_t *g, int level);
/* the name of a puzzle's picture, in Spanish (the app translates it) */
const char *vt_pz_name(int level);
size_t vt_pz_undo_size(void);       /* bytes per snapshot */
bool vt_pz_undo(vt_game_t *g);
/* the par time (ms) for three stars */
uint32_t vt_pz_par(const vt_game_t *g);

/* the simulator's scenes: a stack of real pieces under top[x] */
bool vt_scene(vt_game_t *g, const uint8_t top[VT_COLS_MAX], uint32_t seed);
