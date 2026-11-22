/*
 * Juegos de mesa - ludo, for two to four.
 *
 * A board of 15 x 15 cells: a yard in each corner, a cross whose arms
 * carry the track of 52 squares round, and down the middle of each arm a
 * player's way home (five squares and the centre). Each player has four
 * tokens in the yard; a six takes one out to the player's start; a token
 * goes once round and then home, which it enters only with the exact
 * number. A six, taking a token or getting one home rolls again; three
 * sixes in a row lose the turn. A token landing alone on another colour's
 * token sends it back to its yard, except on the safe squares (the starts
 * and the stars). The first to bring the four home wins.
 *
 * Seats: the first player is the bottom left yard, then round the way the
 * tokens go (bottom right, top right, top left). Two players sit across
 * from each other (seats 0 and 2).
 *
 * Cells: 0..51 the track, 52 + 5 seat + k a way home, 72 + seat the
 * centre, 76 + 4 seat + k the yards.
 */
#include "mesa_game.h"

#include <stdio.h>
#include <string.h>

#ifndef N_
#define N_(s) s
#endif

extern const mesa_host_t *mesa_host;

#define TRACK 52
#define C_HOME 52
#define C_GOAL 72
#define C_YARD 76
#define NCELLS 92
/* a token's progress: -1 in the yard, 0..50 round the track from its
 * start, 51..55 the way home, 56 the centre */
#define P_GOAL 56

typedef struct {
    uint8_t np;
    uint8_t seat[4];
    int8_t  prog[4][4];
    uint8_t turn;
    uint8_t die;                /* 0: to roll */
    uint8_t sixes;
    uint8_t moved;              /* the die was used */
    int8_t  result;
    uint8_t finished[4];
} ld_t;

static const mesa_kind_t KINDS[4] = {
    { "r", NULL, 0xD82820, 0x801008, NULL, MESA_KF_TALL, 9 },
    { "g", NULL, 0x28A040, 0x105820, NULL, MESA_KF_TALL, 9 },
    { "y", NULL, 0xF0C020, 0x987008, NULL, MESA_KF_TALL, 9 },
    { "b", NULL, 0x2860D8, 0x103080, NULL, MESA_KF_TALL, 9 },
};
static const char *const SEAT_NAME[4] = { N_("Rojo"), N_("Verde"), N_("Amarillo"), N_("Azul") };
static const uint32_t SEAT_RGB[4] = { 0xD82820, 0x28A040, 0xF0C020, 0x2860D8 };
static const char *const NPLAYERS[] = { "2", "3", "4", NULL };
static const mesa_opt_t OPTS[1] = { { N_("Jugadores"), NPLAYERS, 2 } };

/* the track's squares on the 15 x 15 grid (x right, y up) */
static int8_t TX[TRACK], TY[TRACK];

static void track(void)
{
    if (TX[1]) return;
    int k = 0;
    for (int x = 0; x <= 5; x++, k++) TX[k] = (int8_t)x, TY[k] = 6;
    for (int y = 5; y >= 0; y--, k++) TX[k] = 6, TY[k] = (int8_t)y;
    TX[k] = 7, TY[k] = 0, k++;
    for (int y = 0; y <= 5; y++, k++) TX[k] = 8, TY[k] = (int8_t)y;
    for (int x = 9; x <= 14; x++, k++) TX[k] = (int8_t)x, TY[k] = 6;
    TX[k] = 14, TY[k] = 7, k++;
    for (int x = 14; x >= 9; x--, k++) TX[k] = (int8_t)x, TY[k] = 8;
    for (int y = 9; y <= 14; y++, k++) TX[k] = 8, TY[k] = (int8_t)y;
    TX[k] = 7, TY[k] = 14, k++;
    for (int y = 14; y >= 9; y--, k++) TX[k] = 6, TY[k] = (int8_t)y;
    for (int x = 5; x >= 0; x--, k++) TX[k] = (int8_t)x, TY[k] = 8;
    TX[k] = 0, TY[k] = 7, k++;
}

static bool safe(int t)
{
    return t % 13 == 1 || t % 13 == 9;
}

static int cell_of(const ld_t *g, int p, int k)
{
    int s = g->seat[p], pr = g->prog[p][k];
    if (pr < 0) return C_YARD + s * 4 + k;
    if (pr <= 50) return (1 + 13 * s + pr) % TRACK;
    if (pr < P_GOAL) return C_HOME + s * 5 + (pr - 51);
    return C_GOAL + s;
}

static const char *ld_board(const uint8_t *opts, float *w, float *h)
{
    (void)opts;
    *w = *h = 15;
    return "ld";
}

static int ld_players(const uint8_t *opts)
{
    return 2 + (opts ? opts[0] % 3 : 2);
}

static void seats(int np, uint8_t *s)
{
    if (np == 2) {
        s[0] = 0;
        s[1] = 2;
    } else {
        for (int i = 0; i < 4; i++) s[i] = (uint8_t)i;
    }
}

static const char *ld_side(const uint8_t *opts, int p, uint32_t *rgb)
{
    uint8_t s[4];
    seats(ld_players(opts), s);
    if (rgb) *rgb = SEAT_RGB[s[p & 3]];
    return SEAT_NAME[s[p & 3]];
}

static void ld_init(void *st, const uint8_t *opts, uint32_t seed)
{
    (void)seed;
    track();
    ld_t *g = (ld_t *)st;
    memset(g, 0, sizeof *g);
    g->np = (uint8_t)ld_players(opts);
    seats(g->np, g->seat);
    memset(g->prog, -1, sizeof g->prog);
    g->result = -1;
}

static int ld_turn(const void *st)
{
    const ld_t *g = (const ld_t *)st;
    return g->result != -1 ? -1 : g->turn;
}

static int ld_result(const void *st, const char **why)
{
    const ld_t *g = (const ld_t *)st;
    if (why) *why = g->result >= 0 ? N_("Llevó sus cuatro fichas a casa") : NULL;
    return g->result;
}

static int ld_need_roll(const void *st)
{
    const ld_t *g = (const ld_t *)st;
    return g->result == -1 && !g->die ? 1 : 0;
}

static void ld_roll(void *st, uint32_t rnd)
{
    ld_t *g = (ld_t *)st;
    g->die = (uint8_t)(1 + (rnd >> 8) % 6);
    g->moved = 0;
    if (g->die == 6 && ++g->sixes >= 3) {
        /* the third six: the turn goes */
        g->sixes = 0;
        g->die = 0;
        g->turn = (uint8_t)((g->turn + 1) % g->np);
    }
}

static int ld_dice(const void *st, int *vals, bool *used)
{
    const ld_t *g = (const ld_t *)st;
    if (!g->die) return 0;
    vals[0] = g->die;
    used[0] = g->moved != 0;
    return 1;
}

static bool can(const ld_t *g, int p, int k)
{
    int pr = g->prog[p][k], d = g->die;
    if (pr < 0) return d == 6;
    if (pr >= P_GOAL) return false;
    return pr + d <= P_GOAL;
}

static int ld_moves(const void *st, mesa_move_t *out, int max)
{
    const ld_t *g = (const ld_t *)st;
    if (g->result != -1 || !g->die) return 0;
    int p = g->turn, n = 0;
    uint8_t seen[NCELLS];
    memset(seen, 0, sizeof seen);
    for (int k = 0; k < 4 && n < max; k++) {
        if (!can(g, p, k)) continue;
        int c = cell_of(g, p, k);
        /* two tokens together move the same: one move */
        if (c < C_YARD && seen[c]) continue;
        if (c >= C_YARD) {
            /* out of the yard: any one of them */
            bool any = false;
            for (int j = 0; j < 4; j++) any |= j != k && seen[C_YARD + g->seat[p] * 4 + j];
            if (any) continue;
        }
        seen[c] = 1;
        memset(&out[n], 0, sizeof out[n]);
        out[n].n = 1;
        out[n].step[0] = (uint8_t)c;
        out[n].data = (uint32_t)k;
        n++;
    }
    if (!n && max > 0) {
        memset(&out[0], 0, sizeof out[0]);   /* nothing: a pass */
        n = 1;
    }
    return n;
}

static uint32_t ld_apply(void *st, const mesa_move_t *m)
{
    ld_t *g = (ld_t *)st;
    int p = g->turn;
    uint32_t ev = MESA_EV_MOVE;
    bool again = g->die == 6;
    if (m->n) {
        int k = (int)m->data;
        int pr = g->prog[p][k];
        g->prog[p][k] = (int8_t)(pr < 0 ? 0 : pr + g->die);
        int np = g->prog[p][k];
        if (np == P_GOAL) {
            ev |= MESA_EV_HOME;
            again = true;
        } else if (np <= 50) {
            int t = (1 + 13 * g->seat[p] + np) % TRACK;
            if (!safe(t)) {
                for (int q = 0; q < g->np; q++) {
                    if (q == p) continue;
                    for (int j = 0; j < 4; j++) {
                        int qp = g->prog[q][j];
                        if (qp >= 0 && qp <= 50 && (1 + 13 * g->seat[q] + qp) % TRACK == t) {
                            g->prog[q][j] = -1;
                            ev |= MESA_EV_CAPTURE;
                            again = true;
                        }
                    }
                }
            }
        }
        bool all = true;
        for (int j = 0; j < 4; j++) all &= g->prog[p][j] == P_GOAL;
        if (all) {
            g->result = (int8_t)p;
            ev |= MESA_EV_END;
            return ev;
        }
    }
    g->die = 0;
    if (again && m->n) {
        ev |= MESA_EV_AGAIN;
    } else {
        g->sixes = 0;
        g->turn = (uint8_t)((g->turn + 1) % g->np);
    }
    return ev;
}

static int ld_pieces(const void *st, mesa_piece_t *out, int max)
{
    const ld_t *g = (const ld_t *)st;
    int n = 0;
    uint8_t stack[NCELLS];
    memset(stack, 0, sizeof stack);
    for (int p = 0; p < g->np; p++) {
        for (int k = 0; k < 4 && n < max; k++) {
            mesa_piece_t *o = &out[n++];
            memset(o, 0, sizeof *o);
            o->id = (uint8_t)(p * 4 + k);
            o->kind = g->seat[p];
            o->cell = (uint8_t)cell_of(g, p, k);
            o->slot = stack[o->cell]++;
            if (g->result == -1 && g->turn == p && g->die && !g->moved && can(g, p, k)) o->flags |= MESA_PF_LIFT;
        }
    }
    return n;
}

static int ld_cells(const void *st)
{
    (void)st;
    return NCELLS;
}

static void ld_cell_pos(const void *st, int cell, int slot, float *u, float *v)
{
    (void)st;
    float x, y;
    if (cell < TRACK) {
        x = TX[cell];
        y = TY[cell];
    } else if (cell < C_GOAL) {
        int s = (cell - C_HOME) / 5, k = (cell - C_HOME) % 5;
        static const int8_t SX[4] = { 1, 0, -1, 0 }, SY[4] = { 0, 1, 0, -1 };
        static const int8_t BX[4] = { 1, 7, 13, 7 }, BY[4] = { 7, 1, 7, 13 };
        x = BX[s] + SX[s] * k;
        y = BY[s] + SY[s] * k;
    } else if (cell < C_YARD) {
        /* the centre: each colour on its own side of it */
        int s = cell - C_GOAL;
        static const float GX[4] = { 6.2f, 7.0f, 7.8f, 7.0f }, GY[4] = { 7.0f, 6.2f, 7.0f, 7.8f };
        *u = GX[s] + 0.5f + (float)((slot & 1) * 2 - 1) * (slot > 0 ? 0.18f : 0) ;
        *v = GY[s] + 0.5f + (float)(slot >> 1) * 0.2f - (slot ? 0.1f : 0);
        return;
    } else {
        int s = (cell - C_YARD) / 4, k = (cell - C_YARD) % 4;
        static const float YX[4] = { 1.5f, 10.5f, 10.5f, 1.5f }, YY[4] = { 1.5f, 1.5f, 10.5f, 10.5f };
        x = YX[s] + (k & 1) * 2.0f;
        y = YY[s] + (k >> 1) * 2.0f;
        *u = x + 0.5f;
        *v = y + 0.5f;
        return;
    }
    /* tokens together stand side by side */
    float off = slot ? 0.22f * (float)((slot & 1) ? 1 : -1) : 0;
    *u = x + 0.5f + off;
    *v = y + 0.5f + (slot >= 2 ? 0.2f : 0);
}

static int ld_cell_at(const void *st, float u, float v)
{
    int best = MESA_NO_CELL;
    float bd = 0.5f;
    for (int c = 0; c < NCELLS; c++) {
        float cu, cv;
        ld_cell_pos(st, c, 0, &cu, &cv);
        float d = (cu - u) * (cu - u) + (cv - v) * (cv - v);
        if (d < bd) {
            bd = d;
            best = c;
        }
    }
    return best;
}

static int ld_path(const void *st, const mesa_move_t *m, int id, uint8_t *cells, int max)
{
    const ld_t *g = (const ld_t *)st;     /* the position after the move */
    if (!m->n || max < 1) return 0;
    int p = id / 4, k = id % 4;
    if (p >= g->np || (uint32_t)k != m->data) return 0;
    int to = g->prog[p][k];
    if (to < 0) return 0;
    /* where it was: the progress whose cell the move started from */
    int from = -1;
    ld_t t = *g;
    for (int pr = to - 1; pr >= 0; pr--) {
        t.prog[p][k] = (int8_t)pr;
        if (cell_of(&t, p, k) == m->step[0]) {
            from = pr;
            break;
        }
    }
    int d = 0;
    for (int pr = from + 1; pr <= to && d < max; pr++) {
        t.prog[p][k] = (int8_t)pr;
        cells[d++] = (uint8_t)cell_of(&t, p, k);
    }
    return d;
}

static void ld_dice_pos(const void *st, float *u, float *v)
{
    (void)st;
    *u = 7.5f;
    *v = 7.5f;
}

static bool ld_info(const void *st, int player, char *out, size_t n)
{
    const ld_t *g = (const ld_t *)st;
    int home = 0;
    for (int k = 0; k < 4; k++) home += g->prog[player][k] == P_GOAL;
    const char *w = mesa_host && mesa_host->tr ? mesa_host->tr("en casa") : "en casa";
    snprintf(out, n, "%d/4 %s", home, w);
    return true;
}

/* ---- the board's play: the best move by a few rules of thumb ---------------------------------- */

static bool ld_ai(const void *st, int level, mesa_ai_t *ai, mesa_move_t *out)
{
    const ld_t *g = (const ld_t *)st;
    mesa_move_t ms[8];
    int n = ld_moves(st, ms, 8);
    if (!n) return false;
    int p = g->turn, best = 0, bs = -100000;
    uint32_t seed = ai->seed ? ai->seed : 1;
    for (int i = 0; i < n; i++) {
        if (!ms[i].n) break;
        int k = (int)ms[i].data, pr = g->prog[p][k];
        int np = pr < 0 ? 0 : pr + g->die;
        int s = np * 2;
        if (pr < 0) s += 60;
        if (np == P_GOAL) s += 80;
        if (np > 50 && pr <= 50) s += 40;
        int t = np <= 50 ? (1 + 13 * g->seat[p] + np) % TRACK : -1;
        if (t >= 0) {
            if (safe(t)) s += 25;
            /* a capture, and the danger where it lands and where it was */
            for (int q = 0; q < g->np; q++) {
                if (q == p) continue;
                for (int j = 0; j < 4; j++) {
                    int qp = g->prog[q][j];
                    if (qp < 0 || qp > 50) continue;
                    int qt = (1 + 13 * g->seat[q] + qp) % TRACK;
                    if (qt == t && !safe(t)) s += 90 + qp;
                    int behind = (t - qt + TRACK) % TRACK;
                    if (behind >= 1 && behind <= 6 && !safe(t)) s -= 30;
                    if (pr >= 0 && pr <= 50) {
                        int ot = (1 + 13 * g->seat[p] + pr) % TRACK;
                        int b2 = (ot - qt + TRACK) % TRACK;
                        if (b2 >= 1 && b2 <= 6 && !safe(ot)) s += 20;
                    }
                }
            }
        }
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        s += (int)(seed % (uint32_t)(level <= 1 ? 120 : level == 2 ? 40 : 4));
        if (s > bs) {
            bs = s;
            best = i;
        }
    }
    *out = ms[best];
    ai->depth = 1;
    return true;
}

const mesa_game_t mesa_ludo = {
    .word = "ludo",
    .name = N_("Ludo"),
    .about = N_("De dos a cuatro, con dados"),
    .prefix = "ld",
    .flags = MESA_GF_DICE,
    .min_players = 2,
    .max_players = 4,
    .levels = 3,
    .state_size = sizeof(ld_t),
    .kinds = KINDS,
    .nkinds = 4,
    .opts = OPTS,
    .nopts = 1,
    .board = ld_board,
    .players = ld_players,
    .side = ld_side,
    .init = ld_init,
    .turn = ld_turn,
    .result = ld_result,
    .info = ld_info,
    .moves = ld_moves,
    .apply = ld_apply,
    .need_roll = ld_need_roll,
    .roll = ld_roll,
    .dice = ld_dice,
    .pieces = ld_pieces,
    .cells = ld_cells,
    .cell_pos = ld_cell_pos,
    .cell_at = ld_cell_at,
    .path = ld_path,
    .dice_pos = ld_dice_pos,
    .ai_move = ld_ai,
};
