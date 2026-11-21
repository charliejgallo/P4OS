/*
 * Juegos de mesa - four in a row: seven columns of six, a disc dropped in
 * a column falls to the lowest free place; four of one colour in a line
 * (across, up or slanting) wins, a full board is a draw. Red first.
 *
 * The board is two bitboards (a column is seven bits: six places and one
 * spare on top, so a line never wraps), searched by alpha-beta with a
 * transposition table, the middle columns first and the moves that lose
 * at once left out. When the search reaches the end of the game the move
 * is perfect; the strongest level searches as deep as its time allows.
 * Only constant shifts: a 64-bit shift by a variable is a call the
 * board's table does not have.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "mesa_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef N_
#define N_(s) s
#endif

extern const mesa_host_t *mesa_host;

#define C4W 7
#define C4H 6

typedef struct {
    uint64_t pos, mask;         /* the side to move's discs; all discs */
    uint8_t  height[C4W];
    uint8_t  owner[C4W * C4H];  /* 0 none, 1 red, 2 yellow */
    uint8_t  id[C4W * C4H];
    uint8_t  moves;
    uint8_t  side;
    int8_t   result;
    uint8_t  win[4];            /* the four that won */
} c4_t;

static const mesa_kind_t KINDS[2] = {
    { "r", NULL, 0xD8281C, 0x8A1408, NULL, MESA_KF_FLAT, 2 },
    { "y", NULL, 0xF0C020, 0xA07808, NULL, MESA_KF_FLAT, 2 },
};

static uint64_t s_bottom[C4W], s_col[C4W], s_top[C4W];

static void tables(void)
{
    if (s_col[0]) return;
    uint64_t b = 1;
    for (int c = 0; c < C4W; c++) {
        s_bottom[c] = b;
        uint64_t col = 0, x = b;
        for (int r = 0; r < C4H; r++) {
            col |= x;
            if (r == C4H - 1) s_top[c] = x;
            x <<= 1;
        }
        s_col[c] = col;
        b <<= 7;
    }
}

static bool four(uint64_t p)
{
    uint64_t m = p & (p >> 7);
    if (m & (m >> 14)) return true;
    m = p & (p >> 6);
    if (m & (m >> 12)) return true;
    m = p & (p >> 8);
    if (m & (m >> 16)) return true;
    m = p & (p >> 1);
    if (m & (m >> 2)) return true;
    return false;
}

static const char *c4_board(const uint8_t *opts, float *w, float *h)
{
    (void)opts;
    *w = C4W;
    *h = C4H;
    return "c4";
}

static int c4_players(const uint8_t *opts)
{
    (void)opts;
    return 2;
}

static const char *c4_side(const uint8_t *opts, int p, uint32_t *rgb)
{
    (void)opts;
    if (rgb) *rgb = p ? 0xF0C020 : 0xD8281C;
    return p ? N_("Amarillo") : N_("Rojo");
}

static void c4_init(void *st, const uint8_t *opts, uint32_t seed)
{
    (void)opts;
    (void)seed;
    tables();
    c4_t *g = (c4_t *)st;
    memset(g, 0, sizeof *g);
    memset(g->id, 0xFF, sizeof g->id);
    g->result = -1;
}

static int c4_turn(const void *st)
{
    const c4_t *g = (const c4_t *)st;
    return g->result != -1 ? -1 : g->side;
}

static int c4_result(const void *st, const char **why)
{
    const c4_t *g = (const c4_t *)st;
    if (why) *why = g->result == -2 ? N_("El tablero está lleno") : g->result >= 0 ? N_("Cuatro en línea") : NULL;
    return g->result;
}

static int c4_moves(const void *st, mesa_move_t *out, int max)
{
    const c4_t *g = (const c4_t *)st;
    if (g->result != -1) return 0;
    int n = 0;
    for (int c = 0; c < C4W && n < max; c++) {
        if (g->height[c] >= C4H) continue;
        memset(&out[n], 0, sizeof out[n]);
        out[n].n = 1;
        out[n].step[0] = (uint8_t)(g->height[c] * C4W + c);
        n++;
    }
    return n;
}

static void find_win(c4_t *g, int who)
{
    static const int D[4][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 } };
    for (int r = 0; r < C4H; r++) {
        for (int c = 0; c < C4W; c++) {
            for (int d = 0; d < 4; d++) {
                int k = 0;
                for (; k < 4; k++) {
                    int rr = r + D[d][1] * k, cc = c + D[d][0] * k;
                    if (rr < 0 || rr >= C4H || cc < 0 || cc >= C4W || g->owner[rr * C4W + cc] != who) break;
                    g->win[k] = (uint8_t)(rr * C4W + cc);
                }
                if (k == 4) return;
            }
        }
    }
}

static uint32_t c4_apply(void *st, const mesa_move_t *m)
{
    c4_t *g = (c4_t *)st;
    int cell = m->step[0], c = cell % C4W;
    int r = g->height[c];
    uint64_t bit = (g->mask + s_bottom[c]) & s_col[c];
    g->pos ^= g->mask;      /* now the other side's, before the disc goes in */
    g->mask |= bit;
    uint64_t mine = g->pos ^ g->mask;     /* the mover's discs, the new one included */
    g->height[c]++;
    g->owner[r * C4W + c] = (uint8_t)(g->side + 1);
    g->id[r * C4W + c] = g->moves;
    g->moves++;
    uint32_t ev = MESA_EV_MOVE | MESA_EV_DROP;
    if (four(mine)) {
        g->result = (int8_t)g->side;
        find_win(g, g->side + 1);
        ev |= MESA_EV_END;
    } else if (g->moves >= C4W * C4H) {
        g->result = -2;
        ev |= MESA_EV_END;
    }
    g->side ^= 1;
    return ev;
}

static bool c4_notation(const void *st, const mesa_move_t *m, char *out, size_t n)
{
    (void)st;
    snprintf(out, n, "%d", m->step[0] % C4W + 1);
    return true;
}

static int c4_pieces(const void *st, mesa_piece_t *out, int max)
{
    const c4_t *g = (const c4_t *)st;
    int n = 0;
    for (int i = 0; i < C4W * C4H && n < max; i++) {
        if (!g->owner[i]) continue;
        mesa_piece_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->id = g->id[i];
        o->kind = (uint8_t)(g->owner[i] - 1);
        o->cell = (uint8_t)i;
        if (g->result >= 0) {
            bool w = false;
            for (int k = 0; k < 4; k++) w |= g->win[k] == i;
            if (!w) o->flags |= MESA_PF_DIM;
        }
    }
    return n;
}

static int c4_cells(const void *st)
{
    (void)st;
    return C4W * C4H;
}

static void c4_cell_pos(const void *st, int cell, int slot, float *u, float *v)
{
    (void)st;
    (void)slot;
    *u = (float)(cell % C4W) + 0.5f;
    *v = (float)(cell / C4W) + 0.5f;
}

static int c4_cell_at(const void *st, float u, float v)
{
    const c4_t *g = (const c4_t *)st;
    (void)v;
    if (u < 0 || u >= C4W || v < -1 || v > C4H + 1.5f) return MESA_NO_CELL;
    int c = (int)u;
    if (g->height[c] >= C4H) return MESA_NO_CELL;
    return g->height[c] * C4W + c;
}

/* a new disc falls from above its column */
static int c4_path(const void *st, const mesa_move_t *m, int id, uint8_t *cells, int max)
{
    (void)st;
    (void)id;
    if (max < 1) return 0;
    cells[0] = (uint8_t)((C4H - 1) * C4W + m->step[0] % C4W);
    return 1;
}

/* ---- the board's play -------------------------------------------------------------------- */

#define TT_N (1u << 16)

typedef struct {
    uint32_t key;
    int8_t   val;
    uint8_t  depth, flag, move;
} c4tt_t;

typedef struct {
    mesa_ai_t *ai;
    c4tt_t    *tt;
    uint32_t   nodes, deadline;
    bool       abort;
} cs_t;

static const int ORDER[C4W] = { 3, 2, 4, 1, 5, 0, 6 };

/* threats: places that would make four, for each side */
static uint64_t winning_spots(uint64_t p, uint64_t mask)
{
    /* vertical */
    uint64_t r = (p << 1) & (p << 2) & (p << 3);
    uint64_t t;
    /* horizontal */
    t = (p << 7) & (p << 14);
    r |= t & (p << 21);
    r |= t & (p >> 7);
    t = (p >> 7) & (p >> 14);
    r |= t & (p << 7);
    r |= t & (p >> 21);
    /* diagonal 1 */
    t = (p << 6) & (p << 12);
    r |= t & (p << 18);
    r |= t & (p >> 6);
    t = (p >> 6) & (p >> 12);
    r |= t & (p << 6);
    r |= t & (p >> 18);
    /* diagonal 2 */
    t = (p << 8) & (p << 16);
    r |= t & (p << 24);
    r |= t & (p >> 8);
    t = (p >> 8) & (p >> 16);
    r |= t & (p << 8);
    r |= t & (p >> 24);
    uint64_t board = 0;
    for (int c = 0; c < C4W; c++) board |= s_col[c];
    return r & (board ^ mask);
}

static int popcount64(uint64_t x)
{
    uint32_t a = (uint32_t)x, b = (uint32_t)(x >> 32);
    int n = 0;
    while (a) {
        a &= a - 1;
        n++;
    }
    while (b) {
        b &= b - 1;
        n++;
    }
    return n;
}

static int heur(uint64_t pos, uint64_t mask)
{
    uint64_t opp = pos ^ mask;
    int s = popcount64(winning_spots(pos, mask)) * 4 - popcount64(winning_spots(opp, mask)) * 4;
    s += popcount64(pos & s_col[3]) - popcount64(opp & s_col[3]);
    if (s > 40) s = 40;
    if (s < -40) s = -40;
    return s;
}

/* the value for the side to move: +100 - plies for a win, 0 a draw, a guess between */
static int negamax(cs_t *s, uint64_t pos, uint64_t mask, int moves, int depth, int alpha, int beta)
{
    s->nodes++;
    if ((s->nodes & 4095) == 0) {
        if (s->ai->stop || (s->ai->now_ms && (int32_t)(s->ai->now_ms() - s->deadline) >= 0)) s->abort = true;
    }
    if (s->abort) return 0;
    if (moves >= C4W * C4H) return 0;
    /* a win at once */
    for (int c = 0; c < C4W; c++) {
        if (mask & s_top[c]) continue;
        uint64_t nb = (mask + s_bottom[c]) & s_col[c];
        if (four(pos | nb)) return 100 - moves;
    }
    if (depth <= 0) return heur(pos, mask);
    uint32_t key = (uint32_t)((pos + mask) ^ ((pos + mask) >> 32)) * 2654435761u;
    c4tt_t *e = &s->tt[(key >> 8) & (TT_N - 1)];
    int ttm = -1;
    if (e->key == key) {
        ttm = e->move;
        if (e->depth >= depth) {
            if (e->flag == 0) return e->val;
            if (e->flag == 1 && e->val >= beta) return e->val;
            if (e->flag == 2 && e->val <= alpha) return e->val;
        }
    }
    int a0 = alpha, best = -1000, bm = -1;
    /* moves that do not hand the other side a win right above */
    uint64_t opp_wins = winning_spots(pos ^ mask, mask);
    int order[C4W], no = 0;
    if (ttm >= 0) order[no++] = ttm;
    for (int i = 0; i < C4W; i++) {
        if (ORDER[i] != ttm) order[no++] = ORDER[i];
    }
    for (int i = 0; i < no; i++) {
        int c = order[i];
        if (mask & s_top[c]) continue;
        uint64_t nb = (mask + s_bottom[c]) & s_col[c];
        /* the other side has a win on the place above: losing at once */
        if (opp_wins & (nb << 1)) {
            if (best < -(100 - moves - 2)) {
                best = -(100 - moves - 2);
                if (bm < 0) bm = c;
            }
            continue;
        }
        uint64_t np = pos ^ mask, nm = mask | nb;
        int v = -negamax(s, np, nm, moves + 1, depth - 1, -beta, -alpha);
        if (s->abort) return 0;
        if (v > best) {
            best = v;
            bm = c;
        }
        if (v > alpha) alpha = v;
        if (alpha >= beta) break;
    }
    if (bm < 0) return best;
    e->key = key;
    e->val = (int8_t)(best > 127 ? 127 : best < -127 ? -127 : best);
    e->depth = (uint8_t)depth;
    e->move = (uint8_t)bm;
    e->flag = best <= a0 ? 2 : best >= beta ? 1 : 0;
    return best;
}

static uint32_t xr(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static bool c4_ai(const void *st, int level, mesa_ai_t *ai, mesa_move_t *out)
{
    const c4_t *g = (const c4_t *)st;
    static const struct { int depth, ms, noise; } L[7] = {
        { 1, 200, 30 }, { 2, 300, 14 }, { 4, 500, 6 }, { 6, 800, 2 }, { 9, 1500, 0 }, { 14, 3000, 0 }, { 42, 8000, 0 },
    };
    if (level < 1) level = 1;
    if (level > 7) level = 7;
    tables();
    cs_t s;
    memset(&s, 0, sizeof s);
    s.ai = ai;
    s.tt = (c4tt_t *)(mesa_host && mesa_host->alloc ? mesa_host->alloc(sizeof(c4tt_t) * TT_N) : malloc(sizeof(c4tt_t) * TT_N));
    if (!s.tt) return false;
    memset(s.tt, 0, sizeof(c4tt_t) * TT_N);
    uint32_t t0 = ai->now_ms ? ai->now_ms() : 0, budget = (uint32_t)L[level - 1].ms;
    if (ai->ms && ai->ms < budget) budget = ai->ms;
    s.deadline = t0 + budget;
    uint32_t seed = ai->seed;
    int noise[C4W];
    for (int c = 0; c < C4W; c++) noise[c] = L[level - 1].noise ? (int)(xr(&seed) % (uint32_t)(2 * L[level - 1].noise + 1)) - L[level - 1].noise : 0;
    int best = -1;
    int left = C4W * C4H - g->moves;
    for (int d = 1; d <= L[level - 1].depth && d <= left; d++) {
        int bc = -1, bv = -10000;
        for (int i = 0; i < C4W; i++) {
            int c = ORDER[i];
            if (g->height[c] >= C4H) continue;
            uint64_t nb = (g->mask + s_bottom[c]) & s_col[c];
            int v;
            if (four(g->pos | nb)) v = 1000;
            else v = -negamax(&s, g->pos ^ g->mask, g->mask | nb, g->moves + 1, d - 1, -10000, 10000);
            if (s.abort) break;
            /* noise only where nothing is decided */
            if (v > -50 && v < 50) v += noise[c];
            if (v > bv) {
                bv = v;
                bc = c;
            }
        }
        if (s.abort) break;
        best = bc;
        ai->depth = d;
        if (bv >= 50 || bv <= -50) break;
    }
    if (best < 0) {
        for (int i = 0; i < C4W; i++) {
            if (g->height[ORDER[i]] < C4H) {
                best = ORDER[i];
                break;
            }
        }
    }
    ai->nodes = s.nodes;
    if (mesa_host && mesa_host->free) mesa_host->free(s.tt);
    else free(s.tt);
    memset(out, 0, sizeof *out);
    out->n = 1;
    out->step[0] = (uint8_t)(g->height[best] * C4W + best);
    return true;
}

const mesa_game_t mesa_cuatro = {
    .word = "cuatro",
    .name = N_("Cuatro en línea"),
    .about = N_("Cuatro fichas en fila, a lo ancho, a lo alto o en diagonal"),
    .prefix = "c4",
    .flags = MESA_GF_UPRIGHT | MESA_GF_CLOCK,
    .min_players = 1,
    .max_players = 2,
    .levels = 7,
    .state_size = sizeof(c4_t),
    .kinds = KINDS,
    .nkinds = 2,
    .board = c4_board,
    .players = c4_players,
    .side = c4_side,
    .init = c4_init,
    .turn = c4_turn,
    .result = c4_result,
    .moves = c4_moves,
    .apply = c4_apply,
    .notation = c4_notation,
    .pieces = c4_pieces,
    .cells = c4_cells,
    .cell_pos = c4_cell_pos,
    .cell_at = c4_cell_at,
    .path = c4_path,
    .ai_move = c4_ai,
};
