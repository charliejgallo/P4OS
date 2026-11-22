/*
 * Juegos de mesa - chess, as a game of the app (mesa_game.h).
 *
 * The rules are g_ajrules.c and the engine g_ajsearch.c; this file gives
 * every piece an id that follows it around the board, keeps the captures
 * in order for the trays, and says when the game is over: mate, stalemate,
 * the third repetition, fifty moves without a capture or a pawn's move,
 * and too little left to mate, all of them by themselves (nobody has to
 * claim them). Running out of time and resigning are the app's.
 */
#include "g_ajedrez.h"

#include "mesa_game.h"

#include <stdio.h>
#include <string.h>

#ifndef N_
#define N_(s) s
#endif

extern const mesa_host_t *mesa_host;

typedef struct {
    ch_pos_t p;
    uint8_t  id[64];            /* the piece on each square, 0xFF none */
    uint8_t  kind[32];          /* each piece's kind now */
    uint8_t  cap[32];           /* the pieces taken, in order */
    uint8_t  cap_by[32];
    uint8_t  ncap;
    uint8_t  last_from, last_to;
    int8_t   result;
    uint8_t  why;
} aj_t;

enum { WHY_NONE, WHY_MATE, WHY_STALE, WHY_REP, WHY_FIFTY, WHY_MATERIAL };
static const char *const WHY[] = {
    NULL, N_("Jaque mate"), N_("Rey ahogado"), N_("Triple repetición"), N_("Cincuenta movimientos"),
    N_("Material insuficiente"),
};

#define K(c, t) ((c) * 6 + (t) - 1)

static const mesa_kind_t KINDS[12] = {
    { "wp", N_("Peón"), 0xF2E6CE, 0x9C8460, "P", MESA_KF_TALL, 9 },
    { "wn", N_("Caballo"), 0xF2E6CE, 0x9C8460, "N", MESA_KF_TALL | MESA_KF_FACING, 12 },
    { "wb", N_("Alfil"), 0xF2E6CE, 0x9C8460, "B", MESA_KF_TALL, 13 },
    { "wr", N_("Torre"), 0xF2E6CE, 0x9C8460, "R", MESA_KF_TALL, 11 },
    { "wq", N_("Dama"), 0xF2E6CE, 0x9C8460, "Q", MESA_KF_TALL, 15 },
    { "wk", N_("Rey"), 0xF2E6CE, 0x9C8460, "K", MESA_KF_TALL, 17 },
    { "bp", N_("Peón"), 0x3A2A1E, 0x140C06, "P", MESA_KF_TALL, 9 },
    { "bn", N_("Caballo"), 0x3A2A1E, 0x140C06, "N", MESA_KF_TALL | MESA_KF_FACING, 12 },
    { "bb", N_("Alfil"), 0x3A2A1E, 0x140C06, "B", MESA_KF_TALL, 13 },
    { "br", N_("Torre"), 0x3A2A1E, 0x140C06, "R", MESA_KF_TALL, 11 },
    { "bq", N_("Dama"), 0x3A2A1E, 0x140C06, "Q", MESA_KF_TALL, 15 },
    { "bk", N_("Rey"), 0x3A2A1E, 0x140C06, "K", MESA_KF_TALL, 17 },
};

static void ids_from_board(aj_t *g)
{
    memset(g->id, 0xFF, sizeof g->id);
    int next = 0;
    for (int i = 0; i < 64; i++) {
        int pc = g->p.sq[CH_SQ88(i)];
        if (!pc || next >= 32) continue;
        g->id[i] = (uint8_t)next;
        g->kind[next] = (uint8_t)K(CH_COLOR(pc), CH_TYPE(pc));
        next++;
    }
    g->ncap = 0;
    g->last_from = g->last_to = 0xFF;
    g->result = -1;
    g->why = WHY_NONE;
}

static void judge(aj_t *g)
{
    uint32_t l[256];
    int n = mch_legal(&g->p, l);
    g->result = -1;
    g->why = WHY_NONE;
    if (!n) {
        if (mch_in_check(&g->p, g->p.side)) {
            g->result = (int8_t)(g->p.side ^ 1);
            g->why = WHY_MATE;
        } else {
            g->result = -2;
            g->why = WHY_STALE;
        }
    } else if (mch_repeats(&g->p) >= 2) {
        g->result = -2;
        g->why = WHY_REP;
    } else if (g->p.half >= 100) {
        g->result = -2;
        g->why = WHY_FIFTY;
    } else if (mch_no_mating_material(&g->p)) {
        g->result = -2;
        g->why = WHY_MATERIAL;
    }
}

static const char *aj_board(const uint8_t *opts, float *w, float *h)
{
    (void)opts;
    *w = 8;
    *h = 8;
    return "aj";
}

static int aj_players(const uint8_t *opts)
{
    (void)opts;
    return 2;
}

static const char *aj_side(const uint8_t *opts, int p, uint32_t *rgb)
{
    (void)opts;
    if (rgb) *rgb = p ? 0x2A1E16 : 0xF2E6CE;
    return p ? N_("Negras") : N_("Blancas");
}

static void aj_init(void *st, const uint8_t *opts, uint32_t seed)
{
    (void)opts;
    (void)seed;
    aj_t *g = (aj_t *)st;
    memset(g, 0, sizeof *g);
    mch_tables();
    mch_start(&g->p);
    ids_from_board(g);
}

static bool aj_setup(void *st, const uint8_t *opts, const char *text)
{
    (void)opts;
    aj_t *g = (aj_t *)st;
    ch_pos_t p;
    if (!mch_fen(&p, text)) return false;
    memset(g, 0, sizeof *g);
    g->p = p;
    ids_from_board(g);
    judge(g);
    return true;
}

static int aj_turn(const void *st)
{
    const aj_t *g = (const aj_t *)st;
    return g->result != -1 ? -1 : g->p.side;
}

static int aj_result(const void *st, const char **why)
{
    const aj_t *g = (const aj_t *)st;
    if (why) *why = WHY[g->why];
    return g->result;
}

static const char *aj_status(const void *st)
{
    const aj_t *g = (const aj_t *)st;
    if (g->result != -1) return WHY[g->why];
    return mch_in_check(&g->p, g->p.side) ? N_("Jaque") : NULL;
}

static void to_move(uint32_t m, mesa_move_t *o)
{
    memset(o, 0, sizeof *o);
    o->n = 2;
    o->step[0] = (uint8_t)CH_SQ64(CHMV_FROM(m));
    o->step[1] = (uint8_t)CH_SQ64(CHMV_TO(m));
    o->tag = (uint8_t)CHMV_PROMO(m);
    o->data = m;
}

static int aj_moves(const void *st, mesa_move_t *out, int max)
{
    aj_t *g = (aj_t *)st;
    if (g->result != -1) return 0;
    uint32_t l[256];
    ch_pos_t p = g->p;
    int n = mch_legal(&p, l);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) to_move(l[i], &out[i]);
    return n;
}

static uint32_t aj_apply(void *st, const mesa_move_t *mv)
{
    aj_t *g = (aj_t *)st;
    uint32_t m = mv->data;
    int f = CHMV_FROM(m), t = CHMV_TO(m), fl = CHMV_FL(m);
    int f64 = CH_SQ64(f), t64 = CH_SQ64(t);
    int side = g->p.side;
    uint32_t ev = MESA_EV_MOVE;
    /* the ids first, from the board as it is */
    int cap_sq = fl & CHM_EP ? CH_SQ64(t + (side ? 16 : -16)) : (g->p.sq[t] ? t64 : -1);
    if (cap_sq >= 0 && g->id[cap_sq] != 0xFF && g->ncap < 32) {
        g->cap_by[g->ncap] = (uint8_t)side;
        g->cap[g->ncap++] = g->id[cap_sq];
        g->id[cap_sq] = 0xFF;
        ev |= MESA_EV_CAPTURE;
    }
    uint8_t moving = g->id[f64];
    g->id[f64] = 0xFF;
    g->id[t64] = moving;
    if (fl & CHM_CASTLE) {
        int rf = t > f ? f + 3 : f - 4, rt = t > f ? f + 1 : f - 1;
        g->id[CH_SQ64(rt)] = g->id[CH_SQ64(rf)];
        g->id[CH_SQ64(rf)] = 0xFF;
        ev |= MESA_EV_CASTLE;
    }
    if ((fl & CHM_PROMO) && moving != 0xFF) {
        g->kind[moving] = (uint8_t)K(side, CHMV_PROMO(m));
        ev |= MESA_EV_PROMOTE;
    }
    ch_undo_t u;
    if (!mch_make(&g->p, m, &u)) return 0;      /* never: the move came from aj_moves */
    g->last_from = (uint8_t)f64;
    g->last_to = (uint8_t)t64;
    judge(g);
    if (mch_in_check(&g->p, g->p.side)) ev |= MESA_EV_CHECK;
    if (g->result != -1) ev |= MESA_EV_END;
    return ev;
}

static int aj_choice_kind(const void *st, const mesa_move_t *m)
{
    const aj_t *g = (const aj_t *)st;
    int pr = m->tag ? m->tag : CH_Q;
    return K(g->p.side, pr);
}

static bool aj_notation(const void *st, const mesa_move_t *m, char *out, size_t n)
{
    const aj_t *g = (const aj_t *)st;
    ch_pos_t p = g->p;
    char san[16];
    mch_san(&p, m->data, san);
    snprintf(out, n, "%s", san);
    return true;
}

static int aj_pieces(const void *st, mesa_piece_t *out, int max)
{
    const aj_t *g = (const aj_t *)st;
    int n = 0;
    for (int i = 0; i < 64 && n < max; i++) {
        if (g->id[i] == 0xFF) continue;
        mesa_piece_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->id = g->id[i];
        o->kind = g->kind[g->id[i]];
        o->cell = (uint8_t)i;
        if (i == g->last_to) o->flags |= MESA_PF_LAST;
    }
    int slot[2] = { 0, 0 };
    for (int i = 0; i < g->ncap && n < max; i++) {
        mesa_piece_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->id = g->cap[i];
        o->kind = g->kind[g->cap[i]];
        o->cell = MESA_NO_CELL;
        o->tray = g->cap_by[i];
        o->slot = (uint8_t)slot[g->cap_by[i] & 1]++;
    }
    return n;
}

static void aj_cell_pos(const void *st, int cell, int slot, float *u, float *v)
{
    (void)st;
    (void)slot;
    *u = (float)(cell & 7) + 0.5f;
    *v = (float)(cell >> 3) + 0.5f;
}

static int aj_cells(const void *st)
{
    (void)st;
    return 64;
}

static int aj_cell_at(const void *st, float u, float v)
{
    (void)st;
    if (u < 0 || v < 0 || u >= 8 || v >= 8) return MESA_NO_CELL;
    return (int)v * 8 + (int)u;
}

/* how each level plays: how deep, how long, how much noise */
static const struct { int depth, ms, noise; } LEVEL[8] = {
    { 1, 300, 220 }, { 2, 400, 140 }, { 3, 600, 80 }, { 4, 900, 45 },
    { 6, 1500, 22 }, { 0, 2500, 8 },  { 0, 5000, 0 }, { 0, 10000, 0 },
};

static bool aj_ai(const void *st, int level, mesa_ai_t *ai, mesa_move_t *out)
{
    const aj_t *g = (const aj_t *)st;
    if (level < 1) level = 1;
    if (level > 8) level = 8;
    ch_pos_t p = g->p;
    ch_think_t t;
    memset(&t, 0, sizeof t);
    t.stop = &ai->stop;
    t.now = ai->now_ms;
    t.max_depth = LEVEL[level - 1].depth;
    t.ms = (uint32_t)LEVEL[level - 1].ms;
    if (ai->ms && ai->ms < t.ms) t.ms = ai->ms;
    t.noise = LEVEL[level - 1].noise;
    /* the strongest levels too vary their first moves a little */
    if (!t.noise && p.full <= 4) t.noise = 6;
    t.seed = ai->seed;
    uint32_t m = mch_think(&p, &t);
    ai->nodes = t.nodes;
    ai->depth = t.depth_done;
    if (!m) return false;
    to_move(m, out);
    return true;
}

const mesa_game_t mesa_ajedrez = {
    .word = "ajedrez",
    .name = N_("Ajedrez"),
    .about = N_("Reglas completas, reloj y planilla"),
    .prefix = "aj",
    .flags = MESA_GF_ROTATE | MESA_GF_CLOCK | MESA_GF_PGN | MESA_GF_HINTS | MESA_GF_GRID,
    .min_players = 1,
    .max_players = 2,
    .levels = 8,
    .state_size = sizeof(aj_t),
    .kinds = KINDS,
    .nkinds = 12,
    .board = aj_board,
    .players = aj_players,
    .side = aj_side,
    .init = aj_init,
    .turn = aj_turn,
    .result = aj_result,
    .status = aj_status,
    .moves = aj_moves,
    .apply = aj_apply,
    .choice_kind = aj_choice_kind,
    .notation = aj_notation,
    .pieces = aj_pieces,
    .cells = aj_cells,
    .cell_pos = aj_cell_pos,
    .cell_at = aj_cell_at,
    .ai_move = aj_ai,
    .setup = aj_setup,
};
