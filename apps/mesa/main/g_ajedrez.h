/*
 * Juegos de mesa - chess: the rules and the engine, shared by the game's
 * table (g_ajedrez.c) and the engine's search (g_ajsearch.c), and by
 * tools/harness.c, which checks them on the Mac (perft, games against
 * itself).
 *
 * The board is 0x88: a square is rank * 16 + file and anything with 0x88
 * set is off the board, so a ray stops with one AND. A piece is its type
 * (1 pawn .. 6 king) with 8 added for black.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { CH_P = 1, CH_N, CH_B, CH_R, CH_Q, CH_K };
#define CH_BLACK 8
#define CH_TYPE(p) ((p) & 7)
#define CH_COLOR(p) ((p) >> 3)
#define CH_SQ64(s) (((s) + ((s) & 7)) >> 1)
#define CH_SQ88(i) ((i) + ((i) & ~7))

/* castling rights */
enum { CH_WK = 1, CH_WQ = 2, CH_BK = 4, CH_BQ = 8 };

/* a move: from, to (0x88), the piece it becomes, what it is */
enum { CHM_CAP = 1, CHM_EP = 2, CHM_CASTLE = 4, CHM_DOUBLE = 8, CHM_PROMO = 16 };
#define CHMV(f, t, pr, fl) ((uint32_t)(f) | ((uint32_t)(t) << 7) | ((uint32_t)(pr) << 14) | ((uint32_t)(fl) << 17))
#define CHMV_FROM(m) ((int)((m) & 127))
#define CHMV_TO(m) ((int)(((m) >> 7) & 127))
#define CHMV_PROMO(m) ((int)(((m) >> 14) & 7))
#define CHMV_FL(m) ((int)(((m) >> 17) & 31))

#define CH_HIST 128

typedef struct {
    uint8_t  sq[128];
    uint8_t  side;              /* 0 white, 1 black */
    uint8_t  castle;
    int8_t   ep;                /* the square a pawn may take on, or -1 */
    uint8_t  half;              /* moves since a capture or a pawn's */
    uint16_t full;
    uint8_t  king[2];
    int16_t  mat[2];            /* material without pawns and kings, for the phase */
    uint64_t key;
    /* the keys of the last positions, for repetitions: hist[nh - 1] is this one */
    uint64_t hist[CH_HIST];
    int      nh;
} ch_pos_t;

typedef struct {
    uint8_t  captured, castle, half;
    int8_t   ep;
    uint64_t key;
} ch_undo_t;

void     mch_tables(void);                  /* once, before anything else */
void     mch_start(ch_pos_t *p);
bool     mch_fen(ch_pos_t *p, const char *fen);
void     mch_rekey(ch_pos_t *p);
bool     mch_attacked(const ch_pos_t *p, int sq, int by);
bool     mch_in_check(const ch_pos_t *p, int side);
/* pseudo-legal moves; caps: only captures and promotions */
int      mch_gen(const ch_pos_t *p, uint32_t *out, bool caps);
/* legal ones */
int      mch_legal(ch_pos_t *p, uint32_t *out);
/* false (and nothing changed) if it leaves its own king in check */
bool     mch_make(ch_pos_t *p, uint32_t m, ch_undo_t *u);
void     mch_unmake(ch_pos_t *p, uint32_t m, const ch_undo_t *u);
void     mch_null(ch_pos_t *p, ch_undo_t *u);
void     mch_unnull(ch_pos_t *p, const ch_undo_t *u);
int      mch_repeats(const ch_pos_t *p);    /* times this position was seen before */
bool     mch_no_mating_material(const ch_pos_t *p);
/* the move in SAN (with + or #), and back */
void     mch_san(ch_pos_t *p, uint32_t m, char *out);
uint32_t mch_parse_san(ch_pos_t *p, const char *s);
uint64_t mch_perft(ch_pos_t *p, int depth);

/* the engine (g_ajsearch.c) */
typedef struct {
    volatile bool *stop;
    uint32_t     (*now)(void);
    uint32_t       t0, ms;
    int            max_depth;
    int            noise;           /* centipawns of noise on the root's moves */
    uint32_t       seed;
    uint32_t       nodes;
    int            depth_done;
    int            score;
} ch_think_t;

bool     mch_search_init(void);             /* the table: false without memory */
void     mch_search_free(void);
uint32_t mch_think(ch_pos_t *p, ch_think_t *t);
int      mch_eval(const ch_pos_t *p);
