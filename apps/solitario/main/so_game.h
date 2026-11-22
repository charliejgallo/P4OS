/*
 * SOLITARIO - the rules, for every game
 *
 * Nothing here knows about LVGL or the HAL, so the bench
 * (tools/so_harness.c) and the solver use exactly the code the app plays
 * with: a hint or a guaranteed deal is a list of moves this engine accepts.
 *
 * One model serves all the games: a game is a set of piles (the stock, the
 * waste, foundations, tableau columns, free cells, and for Pyramid, TriPeaks
 * the single positions of the layout, each a pile of zero or one card that
 * other positions may cover). A player's action is a so_move_t; applying it
 * writes primitive operations (move n cards, turn one over) to a log, which
 * is what undo plays backwards and what the view animates.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SO_MAXCARDS  104
#define SO_MAXPILES  40
#define SO_PILE_MAX  104

/* The games as the player picks them; each has its own statistics. */
typedef enum {
    SO_M_KLON1 = 0,     /* Klondike, drawing one                         */
    SO_M_KLON3,         /* Klondike, drawing three                       */
    SO_M_SPIDER1,       /* Spider, one suit                              */
    SO_M_SPIDER2,
    SO_M_SPIDER4,
    SO_M_FREECELL,
    SO_M_PYRAMID,
    SO_M_TRIPEAKS,
    SO_M_GOLF,
    SO_M_YUKON,
    SO_M_SPANISH,       /* Klondike with the Spanish deck of forty       */
    SO_NMODES
} so_mode_t;

typedef enum {
    SO_K_KLONDIKE = 0,
    SO_K_SPIDER,
    SO_K_FREECELL,
    SO_K_PYRAMID,
    SO_K_TRIPEAKS,
    SO_K_GOLF,
    SO_K_YUKON,
    SO_K_SPANISH,
} so_kind_t;

typedef enum {
    SO_P_STOCK = 0,
    SO_P_WASTE,
    SO_P_FOUND,
    SO_P_TABLEAU,
    SO_P_CELL,
    SO_P_SLOT,          /* one position of Pyramid or TriPeaks            */
    SO_P_DISCARD,       /* Pyramid's removed pairs                        */
} so_ptype_t;

typedef enum {
    SO_SC_STANDARD = 0,
    SO_SC_VEGAS,
    SO_SC_VEGAS_CUM,    /* Vegas, the balance carried from hand to hand   */
    SO_SC_TIMED,        /* standard, minus time, plus a bonus for speed   */
    SO_SC_NONE,
    SO_NSCORING
} so_scoring_t;

typedef enum {
    SO_MV_MOVE = 0,     /* n cards from 'from' onto 'to'                  */
    SO_MV_DRAW,         /* the stock: to the waste, or one to each column */
    SO_MV_REDEAL,       /* the waste back into the stock                  */
    SO_MV_PAIR,         /* Pyramid: 'from' and 'to' (or 'from' alone, a king) away */
} so_mvkind_t;

typedef struct {
    uint8_t kind, from, to, n;
} so_move_t;

/* primitive operations, as the log keeps them */
enum { SO_OP_MOVE = 0, SO_OP_FLIP };
typedef struct {
    uint8_t op, a, b, n;    /* MOVE: n cards a -> b; FLIP: the top card of a */
} so_op_t;

/* one player's action in the log */
typedef struct {
    uint32_t op0;           /* its first operation                          */
    int32_t  score;         /* the score before it                          */
    uint8_t  passes;
    uint8_t  streak;
    so_move_t mv;
} so_rec_t;

typedef struct {
    uint8_t mode, kind;
    uint8_t ncards, npiles;
    uint8_t nranks;         /* 13, or 10 for the Spanish deck               */
    uint8_t draw;           /* cards drawn at a time                        */
    int8_t  passes_max;     /* through the stock; -1 is no limit            */
    uint8_t passes;         /* passes made so far (the first counts)        */
    uint8_t scoring;
    uint8_t streak;         /* TriPeaks and Golf: cards in a row            */

    uint8_t suit[SO_MAXCARDS];
    uint8_t rank[SO_MAXCARDS];  /* 0 .. nranks - 1: ace first               */
    uint8_t up[SO_MAXCARDS];

    uint8_t ptype[SO_MAXPILES];
    uint8_t pn[SO_MAXPILES];
    uint8_t pc[SO_MAXPILES][SO_PILE_MAX];
    int8_t  cover[SO_MAXPILES][2];  /* SLOT: the positions lying on it      */

    int8_t  stock, waste, discard;
    uint8_t found0, nfound, tab0, ntab, cell0, ncell, slot0, nslot;

    int32_t  score;
    uint32_t moves;
    uint32_t seed;

    so_op_t  *ops;
    uint32_t  nops, ops_cap;
    so_rec_t *recs;
    uint32_t  nrecs, recs_cap;
    bool      oom;
} so_game_t;

/* Options a new hand is dealt with. */
typedef struct {
    uint8_t scoring;
    bool    limited;        /* three passes through the stock, not endless  */
} so_opts_t;

extern const char *const so_mode_name[SO_NMODES];   /* N_() keys */
so_kind_t so_mode_kind(int mode);

/* Deals 'mode' from 'seed'. Frees nothing: call so_free() first on a game
 * that was in use (or zero it once). */
bool so_new(so_game_t *g, int mode, uint32_t seed, const so_opts_t *o);
void so_free(so_game_t *g);

/* Legal moves now, every one; returns how many (at most max). */
int  so_moves(const so_game_t *g, so_move_t *out, int max);
bool so_legal(const so_game_t *g, const so_move_t *m);
/* Plays it (it must be legal) and logs it for undo. */
void so_apply(so_game_t *g, const so_move_t *m);
bool so_undo(so_game_t *g);
static inline bool so_can_undo(const so_game_t *g) { return g->nrecs > 0; }

bool so_won(const so_game_t *g);
/* No move left at all (the stock counts): the hand is lost. */
bool so_stuck(const so_game_t *g);

/* What the player touched: can these cards be lifted (pile from index i up)? */
bool so_can_pick(const so_game_t *g, int pile, int idx);
/* The move that drops pile[idx..] onto 'to', if legal. */
bool so_drop_move(const so_game_t *g, int pile, int idx, int to, so_move_t *m);
/* A tap on pile[idx]: the best place for it. 'home' asks for the foundation
 * (a double tap). */
bool so_tap_move(const so_game_t *g, int pile, int idx, bool home, so_move_t *m);
/* The stock, tapped. */
bool so_stock_move(const so_game_t *g, so_move_t *m);

/* The finish: every card can walk home with no more decisions. Then
 * so_auto_step() gives the next of those moves. */
bool so_can_autocomplete(const so_game_t *g);
bool so_auto_step(const so_game_t *g, so_move_t *m);

/* A heuristic move worth making (no search), or false. */
bool so_hint_quick(const so_game_t *g, so_move_t *m);
/* How much a move is worth to the player and the solver; < 0 is useless. */
int  so_move_value(const so_game_t *g, const so_move_t *m);

/* The state's hash, the same for columns in another order. */
uint64_t so_hash(const so_game_t *g);

/* Cards that reached home, for the meter and Vegas. */
int so_home_count(const so_game_t *g);

/* top card of a pile, or -1 */
static inline int so_top(const so_game_t *g, int p)
{
    return g->pn[p] ? g->pc[p][g->pn[p] - 1] : -1;
}
/* The two colours a column alternates. The Spanish deck has none: there
 * coins and cups play against swords and clubs. */
static inline bool so_red(const so_game_t *g, int c)
{
    if (g->kind == SO_K_SPANISH) {
        return g->suit[c] < 2;
    }
    return g->suit[c] == 1 || g->suit[c] == 2;
}
/* a SLOT is free to play when nothing lies on it */
bool so_slot_free(const so_game_t *g, int p);

/* the small PRNG every deal comes from */
uint32_t so_rand(uint32_t *s);
