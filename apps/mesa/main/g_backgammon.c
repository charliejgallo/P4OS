/*
 * Juegos de mesa - backgammon, one game (no doubling cube).
 *
 * Fifteen checkers a side. The first roll is one die each: the higher
 * starts, with those two dice (a tie rolls again). A double plays four
 * times. A checker lands on an empty point, its own or one with a single
 * checker of the other side, which goes to the bar; with a checker on the
 * bar a player must bring it in first. Once all fifteen are home they are
 * borne off: with the exact number, or a higher one when no checker is
 * further back. As many dice as possible must be played, and when only
 * one can, the higher if it can. The first to bear off all fifteen wins;
 * the other with none off is a gammon, and with one still on the bar or
 * in the winner's home a backgammon.
 *
 * A move is one checker with one die (from, to): a turn is up to four of
 * them, and a step is legal only if some play that uses the most dice
 * begins with it. Points 0..23 are the first player's 1..24 (it moves
 * down, the second up); 24 and 25 the bar of each, 26 and 27 what each
 * has borne off.
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

#define BAR(p) (24 + (p))
#define OFF(p) (26 + (p))

typedef struct {
    int8_t  pts[24];            /* + the first player's, - the second's */
    uint8_t bar[2], off[2];
    uint8_t side;
    uint8_t opening;            /* the first roll is still to come */
    uint8_t dv[4], nd;          /* the dice left to play */
    uint8_t shown[4], nshown, used[4];
    int8_t  result;
    uint8_t why;
    uint8_t stack[28][15];      /* the checkers' ids on each cell, bottom first */
    uint8_t ns[28];
} bg_t;

static const mesa_kind_t KINDS[2] = {
    { "w", NULL, 0xF0E6D2, 0xA89470, NULL, MESA_KF_FLAT, 2 },
    { "b", NULL, 0x5A1E14, 0x2A0A06, NULL, MESA_KF_FLAT, 2 },
};
static const char *const WHY[] = { NULL, N_("Sacó todas sus fichas"), N_("Gammon"), N_("Backgammon") };

static int own(const bg_t *g, int p, int i)
{
    int v = g->pts[i];
    return p ? (v < 0 ? -v : 0) : (v > 0 ? v : 0);
}

static int foe(const bg_t *g, int p, int i)
{
    return own(g, 1 - p, i);
}

static const char *bg_board(const uint8_t *opts, float *w, float *h)
{
    (void)opts;
    *w = 15;
    *h = 11;
    return "bg";
}

static int bg_players(const uint8_t *opts)
{
    (void)opts;
    return 2;
}

static const char *bg_side(const uint8_t *opts, int p, uint32_t *rgb)
{
    (void)opts;
    if (rgb) *rgb = p ? 0x5A1E14 : 0xF0E6D2;
    return p ? N_("Rojas") : N_("Blancas");
}

static void push(bg_t *g, int cell, int id)
{
    if (g->ns[cell] < 15) g->stack[cell][g->ns[cell]++] = (uint8_t)id;
}

static int pop(bg_t *g, int cell)
{
    return g->ns[cell] ? g->stack[cell][--g->ns[cell]] : 0;
}

static void bg_init(void *st, const uint8_t *opts, uint32_t seed)
{
    (void)opts;
    (void)seed;
    bg_t *g = (bg_t *)st;
    memset(g, 0, sizeof *g);
    static const int8_t START[4][2] = { { 23, 2 }, { 12, 5 }, { 7, 3 }, { 5, 5 } };
    int id = 0;
    for (int p = 0; p < 2; p++) {
        for (int k = 0; k < 4; k++) {
            int i = p ? 23 - START[k][0] : START[k][0], n = START[k][1];
            g->pts[i] = (int8_t)(p ? -n : n);
            for (int j = 0; j < n; j++) push(g, i, id++);
        }
    }
    g->opening = 1;
    g->result = -1;
}

/* ---- single steps --------------------------------------------------------------------- */

static bool all_home(const bg_t *g, int p)
{
    if (g->bar[p]) return false;
    for (int i = 0; i < 24; i++) {
        bool home = p ? i >= 18 : i <= 5;
        if (!home && own(g, p, i)) return false;
    }
    return true;
}

/* where a checker of p on cell 'from' goes with die d, or -1 */
static int dest(const bg_t *g, int p, int from, int d)
{
    if (g->bar[p] && from != BAR(p)) return -1;
    int t;
    if (from == BAR(p)) t = p ? d - 1 : 24 - d;
    else t = p ? from + d : from - d;
    if (t >= 0 && t < 24) return foe(g, p, t) >= 2 ? -1 : t;
    if (!all_home(g, p)) return -1;
    /* bearing off: exact, or the furthest back with a higher die */
    int need = p ? 24 - from : from + 1;
    if (d == need) return OFF(p);
    if (d < need) return -1;
    for (int i = 0; i < 24; i++) {
        bool further = p ? (i >= 18 && i < from) : (i <= 5 && i > from);
        if (further && own(g, p, i)) return -1;
    }
    return OFF(p);
}

static void step(bg_t *g, int p, int from, int to, bool ids)
{
    int id = 0;
    if (from == BAR(p)) g->bar[p]--;
    else g->pts[from] = (int8_t)(g->pts[from] - (p ? -1 : 1));
    if (ids) id = pop(g, from);
    if (to == OFF(p)) {
        g->off[p]++;
    } else {
        if (foe(g, p, to) == 1) {
            g->pts[to] = 0;
            g->bar[1 - p]++;
            if (ids) push(g, BAR(1 - p), pop(g, to));
        }
        g->pts[to] = (int8_t)(g->pts[to] + (p ? -1 : 1));
    }
    if (ids) push(g, to, id);
}

typedef struct {
    uint8_t from, to, die;
} st_t;

/* the most dice any play from here uses; first[] the first steps of those plays */
static int plays(const bg_t *g, int p, const uint8_t *dv, int nd, int depth, st_t *first, int *nfirst, st_t *path,
                 int *maxused, int *maxdie)
{
    int best = 0;
    bool any = false;
    for (int k = 0; k < nd; k++) {
        bool dup = false;
        for (int j = 0; j < k; j++) dup |= dv[j] == dv[k];
        if (dup) continue;
        int d = dv[k];
        for (int from = 0; from < 26; from++) {
            if (from < 24 ? !own(g, p, from) : (from != BAR(p) || !g->bar[p])) continue;
            int to = dest(g, p, from, d);
            if (to < 0) continue;
            any = true;
            bg_t c = *g;
            step(&c, p, from, to, false);
            uint8_t rest[4];
            int nr = 0;
            for (int j = 0; j < nd; j++) {
                if (j != k) rest[nr++] = dv[j];
            }
            path[depth].from = (uint8_t)from;
            path[depth].to = (uint8_t)to;
            path[depth].die = (uint8_t)d;
            int used = 1 + plays(&c, p, rest, nr, depth + 1, first, nfirst, path, maxused, maxdie);
            if (used > best) best = used;
        }
    }
    if (!any && depth > 0 && first) {
        /* a play ends here: depth dice used; keep its first step if it is among the best */
        int top = path[0].die;
        if (depth > *maxused || (depth == *maxused && depth == 1 && top > *maxdie)) {
            *maxused = depth;
            *maxdie = top;
            *nfirst = 0;
        }
        if (depth == *maxused && (depth > 1 || top == *maxdie) && *nfirst < 64) first[(*nfirst)++] = path[0];
    }
    return best;
}

static int legal_steps(const bg_t *g, st_t *out)
{
    if (g->result != -1 || g->opening || !g->nd) return 0;
    st_t first[64], path[4];
    int nf = 0, mu = 0, md = 0;
    plays(g, g->side, g->dv, g->nd, 0, first, &nf, path, &mu, &md);
    int n = 0;
    for (int i = 0; i < nf; i++) {
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (out[j].from == first[i].from && out[j].to == first[i].to) {
                dup = true;
                if (first[i].die < out[j].die) out[j].die = first[i].die;
            }
        }
        if (!dup) out[n++] = first[i];
    }
    return n;
}

static int bg_turn(const void *st)
{
    const bg_t *g = (const bg_t *)st;
    return g->result != -1 ? -1 : g->side;
}

static int bg_result(const void *st, const char **why)
{
    const bg_t *g = (const bg_t *)st;
    if (why) *why = WHY[g->why];
    return g->result;
}

static int bg_need_roll(const void *st)
{
    const bg_t *g = (const bg_t *)st;
    return g->result == -1 && (g->opening || !g->nd) ? 2 : 0;
}

static void bg_roll(void *st, uint32_t rnd)
{
    bg_t *g = (bg_t *)st;
    int a = 1 + (int)((rnd >> 4) % 6), b = 1 + (int)((rnd >> 12) % 6);
    memset(g->used, 0, sizeof g->used);
    if (g->opening) {
        g->shown[0] = (uint8_t)a;
        g->shown[1] = (uint8_t)b;
        g->nshown = 2;
        if (a == b) return;
        g->opening = 0;
        g->side = a > b ? 0 : 1;
        g->dv[0] = (uint8_t)a;
        g->dv[1] = (uint8_t)b;
        g->nd = 2;
        return;
    }
    if (a == b) {
        for (int i = 0; i < 4; i++) g->dv[i] = g->shown[i] = (uint8_t)a;
        g->nd = g->nshown = 4;
    } else {
        g->dv[0] = g->shown[0] = (uint8_t)a;
        g->dv[1] = g->shown[1] = (uint8_t)b;
        g->nd = g->nshown = 2;
    }
}

static int bg_dice(const void *st, int *vals, bool *used)
{
    const bg_t *g = (const bg_t *)st;
    for (int i = 0; i < g->nshown; i++) {
        vals[i] = g->shown[i];
        used[i] = g->used[i] != 0;
    }
    return g->nshown;
}

static int bg_moves(const void *st, mesa_move_t *out, int max)
{
    const bg_t *g = (const bg_t *)st;
    if (g->result != -1 || g->opening || !g->nd) return 0;
    st_t s[64];
    int n = legal_steps(g, s);
    if (!n) {
        if (max > 0) memset(&out[0], 0, sizeof out[0]);
        return max > 0 ? 1 : 0;
    }
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        out[i].n = 2;
        out[i].step[0] = s[i].from;
        out[i].step[1] = s[i].to;
        out[i].data = s[i].die;
    }
    return n;
}

static void end_turn(bg_t *g)
{
    g->nd = 0;
    g->side ^= 1;
}

static uint32_t bg_apply(void *st, const mesa_move_t *m)
{
    bg_t *g = (bg_t *)st;
    uint32_t ev = MESA_EV_MOVE;
    int p = g->side;
    if (!m->n) {
        end_turn(g);
        return ev;
    }
    int bar_before = g->bar[1 - p];
    step(g, p, m->step[0], m->step[1], true);
    if (g->bar[1 - p] > bar_before) ev |= MESA_EV_CAPTURE;
    if (m->step[1] == OFF(p)) ev |= MESA_EV_HOME;
    int d = (int)m->data;
    for (int i = 0; i < g->nd; i++) {
        if (g->dv[i] == d) {
            g->dv[i] = g->dv[--g->nd];
            break;
        }
    }
    for (int i = 0; i < g->nshown; i++) {
        if (!g->used[i] && g->shown[i] == d) {
            g->used[i] = 1;
            break;
        }
    }
    if (g->off[p] == 15) {
        g->result = (int8_t)p;
        int o = 1 - p;
        bool in_home = g->bar[o] > 0;
        for (int i = 0; i < 24; i++) {
            bool winners_home = p ? i >= 18 : i <= 5;
            if (winners_home && own(g, o, i)) in_home = true;
        }
        g->why = g->off[o] ? 1 : in_home ? 3 : 2;
        return ev | MESA_EV_END;
    }
    st_t s[64];
    if (!g->nd || !legal_steps(g, s)) end_turn(g);
    return ev;
}

static bool bg_notation(const void *st, const mesa_move_t *m, char *out, size_t n)
{
    const bg_t *g = (const bg_t *)st;
    if (!m->n) {
        snprintf(out, n, "-");
        return true;
    }
    int p = g->side;
    char a[8], b[8];
    int f = m->step[0], t = m->step[1];
    if (f == BAR(p)) snprintf(a, sizeof a, "bar");
    else snprintf(a, sizeof a, "%d", p ? 24 - f : f + 1);
    if (t == OFF(p)) snprintf(b, sizeof b, "off");
    else snprintf(b, sizeof b, "%d", p ? 24 - t : t + 1);
    snprintf(out, n, "%s/%s", a, b);
    return true;
}

static int bg_pieces(const void *st, mesa_piece_t *out, int max)
{
    const bg_t *g = (const bg_t *)st;
    int n = 0;
    for (int c = 0; c < 28; c++) {
        for (int k = 0; k < g->ns[c] && n < max; k++) {
            mesa_piece_t *o = &out[n++];
            memset(o, 0, sizeof *o);
            o->id = g->stack[c][k];
            o->kind = o->id >= 15;
            o->cell = (uint8_t)c;
            o->slot = (uint8_t)k;
        }
    }
    return n;
}

static int bg_cells(const void *st)
{
    (void)st;
    return 28;
}

static float point_x(int i)
{
    if (i <= 5) return 13.0f - (float)i;
    if (i <= 11) return 12.0f - (float)i;
    if (i <= 17) return 1.0f + (float)(i - 12);
    return 8.0f + (float)(i - 18);
}

static void bg_cell_pos(const void *st, int cell, int slot, float *u, float *v)
{
    const bg_t *g = (const bg_t *)st;
    int count = g->ns[cell] ? g->ns[cell] : 1;
    if (cell < 24) {
        float stepv = count > 5 ? 4.0f / (float)(count - 1) : 0.88f;
        *u = point_x(cell);
        *v = cell < 12 ? 0.95f + (float)slot * stepv : 10.05f - (float)slot * stepv;
        return;
    }
    if (cell < 26) {
        *u = 7.0f;
        *v = cell == 24 ? 4.6f - (float)slot * 0.8f : 6.4f + (float)slot * 0.8f;
        return;
    }
    *u = 14.25f;
    *v = cell == 26 ? 0.7f + (float)slot * 0.26f : 10.3f - (float)slot * 0.26f;
}

static int bg_cell_at(const void *st, float u, float v)
{
    const bg_t *g = (const bg_t *)st;
    if (u >= 13.6f && u <= 15.0f) return v < 5.5f ? OFF(0) : OFF(1);
    if (u >= 6.5f && u < 7.5f) return v < 5.5f ? BAR(0) : BAR(1);
    if (v < 0 || v > 11 || u < 0.4f || u > 13.6f) return MESA_NO_CELL;
    for (int i = 0; i < 24; i++) {
        float x = point_x(i);
        if (u >= x - 0.5f && u < x + 0.5f && ((i < 12) == (v < 5.5f))) return i;
    }
    (void)g;
    return MESA_NO_CELL;
}

static void bg_dice_pos(const void *st, float *u, float *v)
{
    const bg_t *g = (const bg_t *)st;
    *u = g->side && !g->opening ? 3.5f : 10.5f;
    *v = 5.5f;
}

static int pips(const bg_t *g, int p)
{
    int s = g->bar[p] * 25;
    for (int i = 0; i < 24; i++) s += own(g, p, i) * (p ? 24 - i : i + 1);
    return s;
}

static bool bg_info(const void *st, int player, char *out, size_t n)
{
    const bg_t *g = (const bg_t *)st;
    const char *w = mesa_host && mesa_host->tr ? mesa_host->tr("afuera") : "afuera";
    snprintf(out, n, "%d pips · %d %s", pips(g, player), g->off[player], w);
    return true;
}

/* ---- the board's play: every play of the roll, the best position at its end ----------------- */

static int eval(const bg_t *g, int p)
{
    int o = 1 - p;
    int s = (pips(g, o) - pips(g, p)) * 2;
    s += (g->off[p] - g->off[o]) * 6;
    s += g->bar[o] * 25 - g->bar[p] * 25;
    int run = 0;
    for (int k = 0; k < 24; k++) {
        int i = p ? k : 23 - k;         /* from p's back to its home */
        int c = own(g, p, i);
        int home_dist = p ? 23 - i : i; /* 0 at the last point */
        if (c >= 2) {
            s += 8 + (home_dist <= 5 ? 10 : home_dist <= 11 ? 4 : 0);
            run++;
            if (run >= 3) s += run * 4;
        } else {
            run = 0;
        }
        if (c == 1) {
            /* a blot: the other side's checkers within six behind it */
            int shots = 0;
            for (int d = 1; d <= 12; d++) {
                int j = p ? i - d : i + d;
                if (j < 0 || j > 23) break;
                if (own(g, o, j)) shots += d <= 6 ? 2 : 1;
            }
            if (g->bar[o]) shots += 2;
            s -= 6 + shots * (4 + (home_dist > 17 ? 0 : 2));
        }
    }
    return s;
}

static void best_play(const bg_t *g, int p, const uint8_t *dv, int nd, int depth, st_t *path, st_t *best,
                      int *best_len, int *best_score, int *maxlen, uint32_t *seed, int noise)
{
    bool any = false;
    for (int k = 0; k < nd; k++) {
        bool dup = false;
        for (int j = 0; j < k; j++) dup |= dv[j] == dv[k];
        if (dup) continue;
        for (int from = 0; from < 26; from++) {
            if (from < 24 ? !own(g, p, from) : (from != BAR(p) || !g->bar[p])) continue;
            int to = dest(g, p, from, dv[k]);
            if (to < 0) continue;
            any = true;
            bg_t c = *g;
            step(&c, p, from, to, false);
            uint8_t rest[4];
            int nr = 0;
            for (int j = 0; j < nd; j++) {
                if (j != k) rest[nr++] = dv[j];
            }
            path[depth].from = (uint8_t)from;
            path[depth].to = (uint8_t)to;
            path[depth].die = dv[k];
            best_play(&c, p, rest, nr, depth + 1, path, best, best_len, best_score, maxlen, seed, noise);
        }
    }
    if (!any && depth > 0) {
        int sc = eval(g, p);
        if (noise) {
            *seed ^= *seed << 13;
            *seed ^= *seed >> 17;
            *seed ^= *seed << 5;
            sc += (int)(*seed % (uint32_t)(2 * noise + 1)) - noise;
        }
        if (depth > *maxlen || (depth == *maxlen && sc > *best_score)) {
            *maxlen = depth;
            *best_score = sc;
            *best_len = depth;
            memcpy(best, path, sizeof(st_t) * (size_t)depth);
        }
    }
}

static bool bg_ai(const void *st, int level, mesa_ai_t *ai, mesa_move_t *out)
{
    const bg_t *g = (const bg_t *)st;
    st_t legal[64];
    int nl = legal_steps(g, legal);
    memset(out, 0, sizeof *out);
    if (!nl) return true;
    st_t path[4], best[4];
    int bl = 0, bs = -1000000, ml = 0;
    uint32_t seed = (uint32_t)pips(g, 0) * 7919u + (uint32_t)pips(g, 1) * 104729u + g->dv[0] + ai->seed % 7;
    int noise = level <= 1 ? 60 : level == 2 ? 25 : 0;
    best_play(g, g->side, g->dv, g->nd, 0, path, best, &bl, &bs, &ml, &seed, noise);
    st_t pick = legal[0];
    for (int i = 0; i < nl && bl; i++) {
        if (legal[i].from == best[0].from && legal[i].to == best[0].to) pick = legal[i];
    }
    out->n = 2;
    out->step[0] = pick.from;
    out->step[1] = pick.to;
    out->data = pick.die;
    ai->depth = bl;
    return true;
}

const mesa_game_t mesa_backgammon = {
    .word = "backgammon",
    .name = N_("Backgammon"),
    .about = N_("Carrera de quince fichas con dos dados"),
    .prefix = "bg",
    .flags = MESA_GF_DICE | MESA_GF_ROTATE | MESA_GF_HINTS,
    .min_players = 1,
    .max_players = 2,
    .levels = 3,
    .state_size = sizeof(bg_t),
    .kinds = KINDS,
    .nkinds = 2,
    .board = bg_board,
    .players = bg_players,
    .side = bg_side,
    .init = bg_init,
    .turn = bg_turn,
    .result = bg_result,
    .info = bg_info,
    .moves = bg_moves,
    .apply = bg_apply,
    .notation = bg_notation,
    .need_roll = bg_need_roll,
    .roll = bg_roll,
    .dice = bg_dice,
    .pieces = bg_pieces,
    .cells = bg_cells,
    .cell_pos = bg_cell_pos,
    .cell_at = bg_cell_at,
    .dice_pos = bg_dice_pos,
    .ai_move = bg_ai,
};
