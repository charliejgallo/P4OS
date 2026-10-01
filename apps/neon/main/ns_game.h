/*
 * NEON SNAKES - the rules
 *
 * No LVGL, no HAL, no floats: plain C over a grid, so the same file builds
 * into the .so, into the simulator and into tools/ns_harness.c with cc.
 *
 * It is DETERMINISTIC on purpose. Two devices play the same match by running
 * this engine side by side (lockstep, like Truco): the host decides which way
 * each human turns on every step and sends only that; the guest applies the
 * same step with the same directions and gets the same board. Everything
 * random - where a fruit appears, where a snake respawns, what a bot decides -
 * comes out of g->rng and nothing else, so nothing outside this file may read
 * it, and nothing here may depend on the clock.
 *
 * P4OS: the arena is no longer fixed. The app sizes it to the screen, so
 * cols, rows, the snake count, how many fruits lie on the table and which
 * cells the pause pill covers all come in through ns_init(). A two-device
 * match keeps the watch's 34x40, four snakes and nine fruits, and draws the same numbers from the dice in the same
 * order: this engine and AmoledOS's hash the same board step by step
 * (tools/ns_harness.c prints the sequence to compare).
 *
 * The grid is one uint16_t per cell:
 *
 *   0                      empty
 *   0xF000 | kind          a fruit (NS_FRUIT_*) or a spark (NS_SPARK_0 + colour)
 *   (id + 1) << 12 | seq   a segment of snake 'id' (0..5); 'seq' is the low
 *                          12 bits of the segment's sequence number
 *
 * A snake is a ring of positions indexed by sequence number: the head is
 * head_seq, the segment behind it head_seq - 1, the tail head_seq - len + 1.
 * The number a cell keeps never changes while the segment lives, which is
 * what lets the drawing ask "which part of which snake is this" in O(1)
 * without the grid being rewritten on every step.
 *
 * What changed is recorded cell by cell in g->chg (a bitmap) for the
 * renderer, and what happened (a fruit eaten, a snake dead) in g->ev for the
 * sound and the effects. The renderer clears both.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Six snakes fit the grid's encoding (ids 0..13 in the top nibble; 0xF is
 * fruit) and the 720x1280 arena, where four got lost. */
#define NS_MAX_SNAKES   6
/* Either side up to 128 cells (a coordinate is a uint8_t), and at most 8192
 * cells in all: the whole screen at 10 px, either way up, with room to
 * spare at the 18 px the app uses. */
#define NS_MAX_COLS     128
#define NS_MAX_ROWS     128
#define NS_MAX_CELLS    8192
#define NS_RING         4096            /* > NS_MAX_CELLS, a power of two   */
#define NS_SEQ_MASK     0x0FFF          /* a snake is never NS_RING long    */
/* What a snake's numbering jumps by when it comes back. It is the watch's
 * ring (2048), not ours: head_seq goes into the hash, and a two-device match
 * with a watch must hash the same. Any even number keeps the stripes. */
#define NS_SEQ_JUMP     2048

enum { NS_UP = 0, NS_RIGHT, NS_DOWN, NS_LEFT, NS_NODIR = 0xFF };

enum {
    NS_MODE_NORMAL = 0,     /* one snake, one fruit, the classic            */
    NS_MODE_COMBAT,         /* several snakes, smaller cells                */
};

/* Fruits, then one spark per snake colour: what a dead snake leaves. */
enum {
    NS_FRUIT_CHERRY = 0,
    NS_FRUIT_APPLE,
    NS_FRUIT_BANANA,
    NS_FRUIT_GRAPES,
    NS_FRUIT_STRAWBERRY,
    NS_FRUIT_ORANGE,
    NS_FRUIT_MELON,
    NS_FRUIT_COUNT,
    NS_SPARK_0 = NS_FRUIT_COUNT,
    NS_KIND_COUNT = NS_SPARK_0 + NS_MAX_SNAKES,
};

#define NS_CELL_FRUIT   0xF000
#define NS_IS_FRUIT(c)  (((c) & 0xF000) == NS_CELL_FRUIT)
#define NS_IS_SNAKE(c)  ((c) != 0 && !NS_IS_FRUIT(c))
#define NS_SNAKE_OF(c)  ((int)((c) >> 12) - 1)

/* How many steps a death flashes before the body turns into sparks. */
#define NS_DYING_STEPS  7
/* Steps until a dead snake comes back, in combat. */
#define NS_RESPAWN_BOT  30
#define NS_RESPAWN_HUMAN 22

typedef struct {
    uint8_t  x[NS_RING];
    uint8_t  y[NS_RING];
    uint16_t head_seq;
    uint16_t len;
    uint16_t grow;          /* segments still to add: the tail waits        */
    uint8_t  dir;           /* where the head went on the last step         */
    uint8_t  alive;         /* 1 on the board (moving or dying)             */
    uint8_t  dying;         /* > 0: flashing, still on the board, not moving*/
    uint8_t  human;         /* driven from outside, not by the bot          */
    uint16_t respawn;       /* steps left while off the board               */
    uint16_t age;           /* steps since it (re)appeared                  */
    uint8_t  colour;        /* palette index for the drawing                */
} ns_snake_t;

enum { NS_EV_EAT = 1, NS_EV_DIE, NS_EV_SPAWN };

typedef struct {
    uint8_t type;
    uint8_t snake;
    uint8_t x, y;
    uint8_t kind;           /* NS_EV_EAT: what was eaten                    */
} ns_event_t;

#define NS_MAX_EVENTS   16

typedef struct {
    uint8_t    mode;
    uint8_t    cols, rows;
    uint8_t    nsnakes;
    uint8_t    fruit_target;
    uint8_t    over;            /* normal mode: the only snake is gone      */
    uint16_t   nfruits;
    uint32_t   rng;
    uint32_t   step_no;

    uint16_t   grid[NS_MAX_CELLS];
    ns_snake_t s[NS_MAX_SNAKES];

    /* Cells under the pause pill: no fruit and no snake is ever put
     * there (a snake may still crawl through). A bit per cell; all clear in
     * a two-device match, where it would change the dice's outcomes. */
    uint8_t    cover[(NS_MAX_CELLS + 7) / 8];
    bool       cover_any;

    /* for the renderer: cells whose drawing may have changed */
    uint8_t    chg[(NS_MAX_CELLS + 7) / 8];
    bool       chg_any;
    ns_event_t ev[NS_MAX_EVENTS];
    uint8_t    nev;

    /* the bots' flood fill, stamped so it never has to be cleared */
    uint16_t   seen[NS_MAX_CELLS];
    uint16_t   stamp;
    uint16_t   queue[NS_MAX_CELLS];
} ns_game_t;

/* humans: how many snakes are driven from outside (the first 'humans'
 * indices). Normal mode ignores both and plays one human alone. cols x rows
 * is the arena (clamped to NS_MAX_*), 'fruits' how many lie on it at once.
 * The watch's are 21x24 and 1 (normal), 34x40 and 9 (combat). 'cover' is a
 * bitmap of cells (index y * cols + x) where nothing may be put, or NULL. */
void ns_init(ns_game_t *g, uint8_t mode, uint32_t seed, int humans, int snakes,
             int cols, int rows, int fruits, const uint8_t *cover);

/* Replaces the covered cells (the screen turned and the pill moved);
 * NULL clears them. Only for games that are not in lockstep. */
void ns_set_cover(ns_game_t *g, const uint8_t *cover);

static inline bool ns_covered(const ns_game_t *g, int i)
{
    return g->cover_any && (g->cover[i >> 3] & (1u << (i & 7)));
}

/* One step. dirs[i] is where human snake i wants to go, NS_NODIR to keep
 * going; a reversal is ignored. Bots decide for themselves. */
void ns_step(ns_game_t *g, const uint8_t dirs[NS_MAX_SNAKES]);

/* What the bot would do with snake i right now: the harness and NS_AUTO use
 * it to let a "human" play itself. Reads g->rng, so in lockstep only the
 * host may call it (and it does, before sending the step). */
uint8_t ns_bot_choice(ns_game_t *g, int i);

/* A hash of everything that matters, to check two engines agree. */
uint32_t ns_hash(const ns_game_t *g);

/* A consistency check of the grid against the rings: 0 if all is well,
 * otherwise a code saying what broke. For the harness. */
int ns_check(const ns_game_t *g);

/* The segment of snake i that is i-th from the head. */
static inline int ns_seg_x(const ns_snake_t *s, int k) { return s->x[(uint16_t)(s->head_seq - k) & (NS_RING - 1)]; }
static inline int ns_seg_y(const ns_snake_t *s, int k) { return s->y[(uint16_t)(s->head_seq - k) & (NS_RING - 1)]; }

/* Which segment (0 = head) a grid value is, for its snake. */
static inline int ns_seg_index(const ns_snake_t *s, uint16_t cell)
{
    return (int)((uint16_t)(s->head_seq - (cell & NS_SEQ_MASK)) & NS_SEQ_MASK);
}

static inline void ns_mark(ns_game_t *g, int x, int y)
{
    int i = y * g->cols + x;
    g->chg[i >> 3] |= (uint8_t)(1u << (i & 7));
    g->chg_any = true;
}

extern const int8_t NS_DX[4];
extern const int8_t NS_DY[4];

/* How much a fruit makes you grow. */
int ns_fruit_growth(int kind);
