/*
 * SUDOKU - the engine: the shape of a board, a brute-force solver that
 * counts solutions, a logical solver that works like a person (one
 * technique at a time, the simplest first) and the generator built on both.
 *
 * No LVGL and no HAL here: the same file builds into the app and into the
 * harness on the Mac (tools/sd_harness.c), which is where the generator's
 * speed and the spread of its difficulties are measured.
 *
 * A board is 9x9 (3x3 boxes) or 6x6 (boxes 3 wide and 2 high). Its
 * no-repeat groups are the rows, the columns and the boxes, plus the two
 * diagonals in the X variant and the cages in Killer. A cage repeats no
 * digit and its cells add up to its sum, but it need not hold every digit:
 * it is a "group" and not a "unit". Units (rows, columns, boxes,
 * diagonals) are the groups that hold each digit exactly once, which is
 * what a hidden single or a fish needs.
 *
 * Candidates are bit masks, bit d for digit d (1..9); bit 0 is unused.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SD_MAXN      9
#define SD_MAXC      81
#define SD_MAXCAGES  48
#define SD_CAGE_MAX  6          /* cells in a cage                            */
#define SD_MAXUNITS  (3 * SD_MAXN + 2)
#define SD_MAXPEERS  40

/* Techniques, in the order the logical solver tries them. */
enum {
    SD_T_NONE = 0,
    SD_T_FULL_HOUSE,
    SD_T_NAKED_SINGLE,
    SD_T_HIDDEN_SINGLE,
    SD_T_CAGE_COMBO,        /* Killer: what the cage's sum allows          */
    SD_T_INNIE,             /* Killer: the rule of 45 inside one unit      */
    SD_T_POINTING,          /* locked candidates, box -> line              */
    SD_T_CLAIMING,          /* locked candidates, line -> box              */
    SD_T_NAKED_PAIR,
    SD_T_HIDDEN_PAIR,
    SD_T_NAKED_TRIPLE,
    SD_T_HIDDEN_TRIPLE,
    SD_T_XWING,
    SD_T_NAKED_QUAD,
    SD_T_HIDDEN_QUAD,
    SD_T_SWORDFISH,
    SD_T_XYWING,
    SD_T_XYZWING,
    SD_T_CHAIN_SHORT,
    SD_T_JELLYFISH,
    SD_T_CHAIN_LONG,
    SD_T_REVEAL,            /* a hint only: the cell's value, no reasoning */
    SD_T_WRONG,             /* a hint only: a digit that does not go there */
    SD_T_COUNT
};

/* Difficulty levels: what the hardest technique needed is worth. */
enum { SD_L_EASY = 0, SD_L_MEDIUM, SD_L_HARD, SD_L_EXPERT, SD_L_MASTER, SD_LEVELS };

typedef struct {
    uint8_t n, bw, bh;          /* side, box width, box height                */
    bool    diag;
    uint8_t ncages;
    uint8_t cage_of[SD_MAXC];   /* 0xFF: in no cage                           */
    uint8_t cage_sum[SD_MAXCAGES];
    uint8_t cage_len[SD_MAXCAGES];
    uint8_t cage_cell[SD_MAXCAGES][SD_CAGE_MAX];

    /* derived by sd_shape_init() */
    uint8_t ncells, nunits;
    uint8_t unit_len[SD_MAXUNITS];
    uint8_t unit_cell[SD_MAXUNITS][SD_MAXN];
    uint8_t unit_kind[SD_MAXUNITS];     /* SD_U_* */
    uint8_t cell_unit[SD_MAXC][4];      /* row, col, box, diag (0xFF)       */
    uint8_t cell_diag2[SD_MAXC];        /* second diagonal (centre), 0xFF   */
    uint8_t npeers[SD_MAXC];
    uint8_t peer[SD_MAXC][SD_MAXPEERS];
    uint16_t all;                       /* mask of 1..n                     */
} sd_shape_t;

enum { SD_U_ROW = 0, SD_U_COL, SD_U_BOX, SD_U_DIAG };

void sd_shape_init(sd_shape_t *s, int n, bool diag);
/* call after filling the cages (cage_of, sums, cells) */
void sd_shape_finish(sd_shape_t *s);
bool sd_sees(const sd_shape_t *s, int a, int b);

/* ---- Brute force --------------------------------------------------------- */

/* Number of solutions of 'grid' (0 = empty), stopping at 'limit'. 'out'
 * (may be NULL) gets the first solution found. 'budget' bounds the work
 * (nodes); 0 = no bound. Returns -1 if the budget ran out. */
int sd_count(const sd_shape_t *s, const uint8_t *grid, int limit, uint8_t *out, uint32_t budget);

/* A full random solution of the shape (diagonals honoured, no cages). */
bool sd_random_full(const sd_shape_t *s, uint32_t *rng, uint8_t *out);

/* ---- The logical solver -------------------------------------------------- */

#define SD_STEP_ELIMS  64
#define SD_STEP_CELLS  24

typedef struct {
    uint8_t  tech;
    int8_t   cell;              /* a placement: the cell, else -1            */
    uint8_t  digit;             /* ... and its digit                         */
    uint8_t  unit;              /* the unit the reasoning is about, 0xFF     */
    uint8_t  unit2;             /* a second one (claiming, fish cover...)    */
    uint16_t digits;            /* digits of the pattern                     */
    uint8_t  sum;               /* Killer: the sum of the cage or the rest   */
    uint8_t  ncells;
    uint8_t  cells[SD_STEP_CELLS];      /* the pattern, to be highlighted    */
    uint8_t  nelim;
    uint8_t  elim_cell[SD_STEP_ELIMS];
    uint8_t  elim_digit[SD_STEP_ELIMS];
    /* a chain: its nodes as cell * 16 + digit, alternately "off" and "on" */
    uint8_t  nchain;
    uint16_t chain[SD_STEP_CELLS];
} sd_step_t;

typedef struct {
    const sd_shape_t *s;
    uint8_t  val[SD_MAXC];
    uint16_t cand[SD_MAXC];
} sd_logic_t;

/* candidates from the values alone */
void sd_logic_init(sd_logic_t *L, const sd_shape_t *s, const uint8_t *val);
/* the simplest step whose level is <= max_level, false if none */
bool sd_logic_step(sd_logic_t *L, int max_level, sd_step_t *st);
void sd_logic_apply(sd_logic_t *L, const sd_step_t *st);
bool sd_logic_solved(const sd_logic_t *L);
/* a contradiction (a cell with no candidates, a unit missing a digit) */
bool sd_logic_broken(const sd_logic_t *L);

int  sd_tech_level(int tech);

typedef struct {
    bool     solved;
    int      level;             /* of the hardest technique used, -1 if none */
    int      steps;
    uint16_t used[SD_T_COUNT];  /* how many times each technique was used    */
} sd_grade_t;

/* Solves 'grid' by logic alone with techniques up to max_level. */
void sd_grade(const sd_shape_t *s, const uint8_t *grid, int max_level, sd_grade_t *g);

/* ---- The generator -------------------------------------------------------- */

typedef struct {
    sd_shape_t shape;           /* with the cages, in Killer                 */
    uint8_t    given[SD_MAXC];
    uint8_t    sol[SD_MAXC];
    int        level;           /* measured                                  */
    int        tries;           /* boards it took                            */
} sd_puzzle_t;

typedef enum { SD_V_CLASSIC = 0, SD_V_MINI, SD_V_KILLER, SD_V_DIAG, SD_VARIANTS } sd_variant_t;

/* Fills 'p' with a puzzle of that variant and level, from 'seed'. The
 * same seed gives the same puzzle on every board (the daily challenge).
 * 'stop' (may be NULL) is polled: when it turns true the generator gives
 * up and returns false. */
bool sd_generate(sd_variant_t v, int level, uint32_t seed, sd_puzzle_t *p, volatile bool *stop);

/* Killer: the digits a cage of 'len' cells adding up to 'sum' can hold. */
uint16_t sd_cage_digits(int len, int sum, uint16_t allowed);

static inline int sd_popcount(uint32_t m)
{
    int c = 0;
    while (m) {
        m &= m - 1;
        c++;
    }
    return c;
}

static inline int sd_lowest(uint32_t m)
{
    int d = 0;
    while (m && !(m & 1u)) {
        m >>= 1;
        d++;
    }
    return d;
}

static inline uint32_t sd_rand(uint32_t *r)
{
    uint32_t x = *r;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *r = x;
    return x;
}
