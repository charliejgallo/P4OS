/*
 * Juegos de mesa - reversi: 8 x 8, black first, a disc is placed so it
 * shuts a line of the other colour, which turns. Whoever cannot place
 * passes; when neither can, the most discs win.
 *
 * The board plays alpha-beta on a table of what each square is worth
 * (corners much, the squares next to them little) and mobility, and in
 * the last empties plays to the end, exactly.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "mesa_game.h"

#include <stdio.h>
#include <string.h>

#ifndef N_
#define N_(s) s
#endif

extern const mesa_host_t *mesa_host;

typedef struct {
    uint8_t b[64];              /* 0 empty, 1 black, 2 white */
    uint8_t id[64];
    uint8_t next;
    uint8_t side;               /* 0 black, 1 white */
    uint8_t passes;
    int8_t  result;
} rv_t;

static const mesa_kind_t KINDS[2] = {
    { "b", NULL, 0x1A1A1A, 0x000000, NULL, MESA_KF_FLAT, 2 },
    { "w", NULL, 0xF0EEE8, 0xB0AEA8, NULL, MESA_KF_FLAT, 2 },
};

static const int8_t D8[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };

static const char *rv_board(const uint8_t *opts, float *w, float *h)
{
    (void)opts;
    *w = *h = 8;
    return "rv";
}

static int rv_players(const uint8_t *opts)
{
    (void)opts;
    return 2;
}

static const char *rv_side(const uint8_t *opts, int p, uint32_t *rgb)
{
    (void)opts;
    if (rgb) *rgb = p ? 0xF0EEE8 : 0x1A1A1A;
    return p ? N_("Blancas") : N_("Negras");
}

static void rv_init(void *st, const uint8_t *opts, uint32_t seed)
{
    (void)opts;
    (void)seed;
    rv_t *g = (rv_t *)st;
    memset(g, 0, sizeof *g);
    memset(g->id, 0xFF, sizeof g->id);
    static const uint8_t SQ[4] = { 27, 28, 35, 36 }, C[4] = { 2, 1, 1, 2 };
    for (int i = 0; i < 4; i++) {
        g->b[SQ[i]] = C[i];
        g->id[SQ[i]] = g->next++;
    }
    g->result = -1;
}

static int flips(const uint8_t *b, int sq, int me, uint8_t *out)
{
    if (b[sq]) return 0;
    int them = 3 - me, n = 0, f = sq & 7, r = sq >> 3;
    for (int d = 0; d < 8; d++) {
        int ff = f + D8[d][0], rr = r + D8[d][1], k = 0;
        uint8_t line[8];
        while (ff >= 0 && ff < 8 && rr >= 0 && rr < 8 && b[rr * 8 + ff] == them) {
            line[k++] = (uint8_t)(rr * 8 + ff);
            ff += D8[d][0];
            rr += D8[d][1];
        }
        if (k && ff >= 0 && ff < 8 && rr >= 0 && rr < 8 && b[rr * 8 + ff] == me) {
            if (out) memcpy(out + n, line, (size_t)k);
            n += k;
        }
    }
    return n;
}

static int legal(const uint8_t *b, int me, uint8_t *sqs)
{
    int n = 0;
    for (int sq = 0; sq < 64; sq++) {
        if (flips(b, sq, me, NULL)) {
            if (sqs) sqs[n] = (uint8_t)sq;
            n++;
        }
    }
    return n;
}

static void count(const uint8_t *b, int *bl, int *wh)
{
    *bl = *wh = 0;
    for (int i = 0; i < 64; i++) {
        if (b[i] == 1) (*bl)++;
        else if (b[i] == 2) (*wh)++;
    }
}

static void judge(rv_t *g)
{
    g->result = -1;
    if (legal(g->b, g->side + 1, NULL) || legal(g->b, 2 - g->side, NULL)) return;
    int bl, wh;
    count(g->b, &bl, &wh);
    g->result = (int8_t)(bl > wh ? 0 : wh > bl ? 1 : -2);
}

static int rv_turn(const void *st)
{
    const rv_t *g = (const rv_t *)st;
    return g->result != -1 ? -1 : g->side;
}

static int rv_result(const void *st, const char **why)
{
    const rv_t *g = (const rv_t *)st;
    if (why) *why = g->result != -1 ? N_("No quedan jugadas") : NULL;
    return g->result;
}

static int rv_moves(const void *st, mesa_move_t *out, int max)
{
    const rv_t *g = (const rv_t *)st;
    if (g->result != -1) return 0;
    uint8_t sq[64];
    int n = legal(g->b, g->side + 1, sq);
    if (!n && max > 0) {
        memset(&out[0], 0, sizeof out[0]);     /* a pass */
        return 1;
    }
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        out[i].n = 1;
        out[i].step[0] = sq[i];
    }
    return n;
}

static uint32_t rv_apply(void *st, const mesa_move_t *m)
{
    rv_t *g = (rv_t *)st;
    uint32_t ev = MESA_EV_MOVE;
    if (m->n) {
        int sq = m->step[0], me = g->side + 1;
        uint8_t f[64];
        int n = flips(g->b, sq, me, f);
        for (int i = 0; i < n; i++) g->b[f[i]] = (uint8_t)me;
        g->b[sq] = (uint8_t)me;
        g->id[sq] = g->next < 96 ? g->next++ : 95;
        if (n) ev |= MESA_EV_FLIP | MESA_EV_DROP;
    }
    g->side ^= 1;
    judge(g);
    if (g->result != -1) ev |= MESA_EV_END;
    return ev;
}

static bool rv_notation(const void *st, const mesa_move_t *m, char *out, size_t n)
{
    (void)st;
    if (!m->n) snprintf(out, n, "--");
    else snprintf(out, n, "%c%d", 'a' + (m->step[0] & 7), 1 + (m->step[0] >> 3));
    return true;
}

static int rv_pieces(const void *st, mesa_piece_t *out, int max)
{
    const rv_t *g = (const rv_t *)st;
    int n = 0;
    for (int sq = 0; sq < 64 && n < max; sq++) {
        if (!g->b[sq]) continue;
        mesa_piece_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->id = g->id[sq];
        o->kind = (uint8_t)(g->b[sq] - 1);
        o->cell = (uint8_t)sq;
    }
    return n;
}

static int rv_cells(const void *st)
{
    (void)st;
    return 64;
}

static void rv_cell_pos(const void *st, int cell, int slot, float *u, float *v)
{
    (void)st;
    (void)slot;
    *u = (float)(cell & 7) + 0.5f;
    *v = (float)(cell >> 3) + 0.5f;
}

static int rv_cell_at(const void *st, float u, float v)
{
    (void)st;
    if (u < 0 || v < 0 || u >= 8 || v >= 8) return MESA_NO_CELL;
    return (int)v * 8 + (int)u;
}

static bool rv_info(const void *st, int player, char *out, size_t n)
{
    const rv_t *g = (const rv_t *)st;
    int bl, wh;
    count(g->b, &bl, &wh);
    const char *w = mesa_host && mesa_host->tr ? mesa_host->tr("fichas") : "fichas";
    snprintf(out, n, "%d %s", player ? wh : bl, w);
    return true;
}

/* ---- the board's play ------------------------------------------------------------------ */

static const int16_t W[64] = {
    120, -25, 20, 6, 6, 20, -25, 120,
    -25, -45, -4, -3, -3, -4, -45, -25,
    20, -4, 12, 3, 3, 12, -4, 20,
    6, -3, 3, 2, 2, 3, -3, 6,
    6, -3, 3, 2, 2, 3, -3, 6,
    20, -4, 12, 3, 3, 12, -4, 20,
    -25, -45, -4, -3, -3, -4, -45, -25,
    120, -25, 20, 6, 6, 20, -25, 120,
};

typedef struct {
    mesa_ai_t *ai;
    uint32_t nodes, deadline;
    bool abort;
} rs_t;

static int eval(const uint8_t *b, int me)
{
    int them = 3 - me, s = 0;
    for (int i = 0; i < 64; i++) {
        int w = W[i];
        /* a corner taken makes its neighbours safe */
        if (w < 0) {
            int f = i & 7, r = i >> 3;
            int cf = f < 4 ? 0 : 7, cr = r < 4 ? 0 : 7;
            if (b[cr * 8 + cf]) w = 4;
        }
        if (b[i] == me) s += w;
        else if (b[i] == them) s -= w;
    }
    int mm = legal(b, me, NULL), mt = legal(b, them, NULL);
    s += (mm - mt) * 8;
    return s;
}

static int search(rs_t *s, const uint8_t *b, int me, int depth, int alpha, int beta, bool passed, int empties)
{
    s->nodes++;
    if ((s->nodes & 511) == 0) {
        if (s->ai->stop || (s->ai->now_ms && (int32_t)(s->ai->now_ms() - s->deadline) >= 0)) s->abort = true;
    }
    if (s->abort) return 0;
    if (depth <= 0) return eval(b, me);
    uint8_t sq[64];
    int n = legal(b, me, sq);
    if (!n) {
        if (passed) {
            int mine = 0, them = 0;
            for (int i = 0; i < 64; i++) {
                if (b[i] == me) mine++;
                else if (b[i]) them++;
            }
            return (mine - them) * 1000;
        }
        return -search(s, b, 3 - me, depth, -beta, -alpha, true, empties);
    }
    /* the best squares first */
    for (int i = 1; i < n; i++) {
        uint8_t t = sq[i];
        int j = i - 1;
        while (j >= 0 && W[sq[j]] < W[t]) {
            sq[j + 1] = sq[j];
            j--;
        }
        sq[j + 1] = t;
    }
    int best = -1000000;
    for (int i = 0; i < n; i++) {
        uint8_t c[64], f[64];
        memcpy(c, b, 64);
        int k = flips(c, sq[i], me, f);
        for (int j = 0; j < k; j++) c[f[j]] = (uint8_t)me;
        c[sq[i]] = (uint8_t)me;
        int v = -search(s, c, 3 - me, depth - 1, -beta, -alpha, false, empties - 1);
        if (s->abort) return 0;
        if (v > best) best = v;
        if (v > alpha) alpha = v;
        if (alpha >= beta) break;
    }
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

static bool rv_ai(const void *st, int level, mesa_ai_t *ai, mesa_move_t *out)
{
    const rv_t *g = (const rv_t *)st;
    static const struct { int depth, ms, noise, exact; } L[6] = {
        { 1, 200, 40, 0 }, { 2, 400, 20, 0 }, { 3, 700, 8, 6 }, { 5, 1500, 3, 10 }, { 7, 3000, 0, 12 },
        { 10, 6000, 0, 14 },
    };
    if (level < 1) level = 1;
    if (level > 6) level = 6;
    int me = g->side + 1;
    uint8_t sq[64];
    int n = legal(g->b, me, sq);
    memset(out, 0, sizeof *out);
    if (!n) return true;        /* a pass */
    int empties = 0;
    for (int i = 0; i < 64; i++) empties += g->b[i] == 0;
    rs_t s;
    memset(&s, 0, sizeof s);
    s.ai = ai;
    uint32_t t0 = ai->now_ms ? ai->now_ms() : 0, budget = (uint32_t)L[level - 1].ms;
    if (ai->ms && ai->ms < budget) budget = ai->ms;
    s.deadline = t0 + budget;
    int maxd = L[level - 1].depth;
    if (empties <= L[level - 1].exact) maxd = empties;     /* to the end */
    uint32_t seed = ai->seed;
    int noise[64];
    for (int i = 0; i < n; i++) noise[i] = L[level - 1].noise ? (int)(xr(&seed) % (uint32_t)(2 * L[level - 1].noise + 1)) - L[level - 1].noise : 0;
    int best = 0;
    for (int d = 1; d <= maxd; d++) {
        int bi = -1, bv = -2000000;
        for (int i = 0; i < n; i++) {
            uint8_t c[64], f[64];
            memcpy(c, g->b, 64);
            int k = flips(c, sq[i], me, f);
            for (int j = 0; j < k; j++) c[f[j]] = (uint8_t)me;
            c[sq[i]] = (uint8_t)me;
            int v = -search(&s, c, 3 - me, d - 1, -2000000, 2000000, false, empties - 1) + noise[i];
            if (s.abort) break;
            if (v > bv) {
                bv = v;
                bi = i;
            }
        }
        if (s.abort) break;
        if (bi >= 0) best = bi;
        ai->depth = d;
        if (n == 1) break;
    }
    ai->nodes = s.nodes;
    out->n = 1;
    out->step[0] = sq[best];
    return true;
}

const mesa_game_t mesa_reversi = {
    .word = "reversi",
    .name = N_("Reversi"),
    .about = N_("Encerrá las fichas del otro y dalas vuelta"),
    .prefix = "rv",
    .flags = MESA_GF_CLOCK | MESA_GF_ROTATE | MESA_GF_GRID,
    .min_players = 1,
    .max_players = 2,
    .levels = 6,
    .state_size = sizeof(rv_t),
    .kinds = KINDS,
    .nkinds = 2,
    .board = rv_board,
    .players = rv_players,
    .side = rv_side,
    .init = rv_init,
    .turn = rv_turn,
    .result = rv_result,
    .info = rv_info,
    .moves = rv_moves,
    .apply = rv_apply,
    .notation = rv_notation,
    .pieces = rv_pieces,
    .cells = rv_cells,
    .cell_pos = rv_cell_pos,
    .cell_at = rv_cell_at,
    .ai_move = rv_ai,
};
