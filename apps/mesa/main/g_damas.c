/*
 * Juegos de mesa - draughts.
 *
 * Ours (de acá): 8 x 8, twelve men a side on the dark squares, white
 * first; a man steps forward diagonally and takes by jumping; the king
 * flies (any distance along a diagonal, and takes from afar, landing on
 * any empty square past the piece). Taking is compulsory. Two choices of
 * the game's: whether men also take backwards, and whether the capture
 * that takes the most must be chosen (then, among those, the one that
 * takes the most kings). The international game: 10 x 10, twenty men a
 * side, men take backwards too, and the most pieces must be taken.
 *
 * In both, a piece taken stays on the board until the move ends (it may
 * not be jumped twice), and a man becomes a king only if it ends its move
 * on the last row. The side that cannot move loses; fifty moves of kings
 * alone without a capture is a draw.
 *
 * A move is the man's square and each square it lands on; the pieces it
 * takes are found again between them when it is made.
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

enum { E = 0, WM = 1, WK = 2, BM = 3, BK = 4 };
#define WHITE(p) ((p) == WM || (p) == WK)
#define BLACK(p) ((p) == BM || (p) == BK)
#define KING(p) ((p) == WK || (p) == BK)
#define MAXN 10

typedef struct {
    uint8_t n;                  /* 8 or 10 */
    uint8_t back, most;         /* men take backwards; the most must be taken */
    uint8_t side;
    uint8_t b[MAXN * MAXN];
    uint8_t id[MAXN * MAXN];
    uint8_t kind[48];
    uint8_t cap[48], cap_by[48], ncap;
    uint8_t quiet;              /* plies of kings without a capture */
    int8_t  result;
    uint8_t why;
} dm_t;

static const char *const WHY[] = { NULL, N_("Sin jugadas"), N_("Cincuenta jugadas de damas") };

static const mesa_kind_t KINDS[4] = {
    { "w", N_("Peón"), 0xF2E6CE, 0x9C8460, NULL, MESA_KF_FLAT, 3 },
    { "wk", N_("Dama"), 0xF2E6CE, 0xC8A040, NULL, MESA_KF_FLAT, 6 },
    { "b", N_("Peón"), 0x3A2418, 0x140C06, NULL, MESA_KF_FLAT, 3 },
    { "bk", N_("Dama"), 0x3A2418, 0xC8A040, NULL, MESA_KF_FLAT, 6 },
};

static const char *const RULES[] = { N_("De acá (8 × 8)"), N_("Internacionales (10 × 10)"), NULL };
static const char *const NOYES[] = { N_("No"), N_("Sí"), NULL };
static const char *const YESNO[] = { N_("Sí"), N_("No"), NULL };
static const mesa_opt_t OPTS[3] = {
    { N_("Reglas"), RULES, 0 },
    { N_("Los peones comen para atrás"), NOYES, 0 },
    { N_("Comer la mayor cantidad"), YESNO, 0 },
};

static int kind_of(int p)
{
    return p == WM ? 0 : p == WK ? 1 : p == BM ? 2 : 3;
}

static const char *dm_board(const uint8_t *opts, float *w, float *h)
{
    bool intl = opts && opts[0] == 1;
    *w = *h = intl ? 10 : 8;
    return intl ? "dm10" : "dm8";
}

static int dm_players(const uint8_t *opts)
{
    (void)opts;
    return 2;
}

static const char *dm_side(const uint8_t *opts, int p, uint32_t *rgb)
{
    (void)opts;
    if (rgb) *rgb = p ? 0x3A2418 : 0xF2E6CE;
    return p ? N_("Negras") : N_("Blancas");
}

static bool dark(int n, int sq)
{
    return ((sq / n) + (sq % n)) % 2 == 0;
}

static void dm_init(void *st, const uint8_t *opts, uint32_t seed)
{
    (void)seed;
    dm_t *g = (dm_t *)st;
    memset(g, 0, sizeof *g);
    bool intl = opts && opts[0] == 1;
    g->n = intl ? 10 : 8;
    g->back = intl || (opts && opts[1] == 1);
    g->most = intl || !(opts && opts[2] == 1);
    int rows = intl ? 4 : 3, n = g->n, next = 0;
    memset(g->id, 0xFF, sizeof g->id);
    for (int sq = 0; sq < n * n; sq++) {
        if (!dark(n, sq)) continue;
        int r = sq / n;
        int p = r < rows ? WM : r >= n - rows ? BM : E;
        if (!p) continue;
        g->b[sq] = (uint8_t)p;
        g->id[sq] = (uint8_t)next;
        g->kind[next++] = (uint8_t)kind_of(p);
    }
    g->result = -1;
}

/* ---- moves ------------------------------------------------------------------------- */

typedef struct {
    uint8_t path[MESA_MAX_STEPS];
    uint8_t n;
    uint8_t taken, kings;
} dmv_t;

static const int DF[4] = { 1, -1, 1, -1 }, DR[4] = { 1, 1, -1, -1 };

static bool mine(const dm_t *g, int p)
{
    return g->side ? BLACK(p) : WHITE(p);
}

static bool theirs(const dm_t *g, int p)
{
    return g->side ? WHITE(p) : BLACK(p);
}

/* every capture sequence from sq, recursively; 'gone' marks pieces taken */
static void jumps(const dm_t *g, int piece, int sq, uint8_t *gone, dmv_t *cur, dmv_t *out, int *nout, int max)
{
    int n = g->n, f = sq % n, r = sq / n;
    bool king = KING(piece);
    int fwd = g->side ? -1 : 1;
    bool any = false;
    for (int d = 0; d < 4; d++) {
        if (!king && !g->back && DR[d] != fwd) continue;
        int ff = f + DF[d], rr = r + DR[d];
        /* a king flies to the piece */
        if (king) {
            while (ff >= 0 && ff < n && rr >= 0 && rr < n && (g->b[rr * n + ff] == E || gone[rr * n + ff] == 2)) {
                ff += DF[d];
                rr += DR[d];
            }
        }
        if (ff < 0 || ff >= n || rr < 0 || rr >= n) continue;
        int over = rr * n + ff;
        if (!theirs(g, g->b[over]) || gone[over]) continue;
        int lf = ff + DF[d], lr = rr + DR[d];
        while (lf >= 0 && lf < n && lr >= 0 && lr < n) {
            int land = lr * n + lf;
            if (g->b[land] != E && !(gone[land] == 2)) break;      /* 2: the moving piece's own square */
            if (cur->n >= MESA_MAX_STEPS) break;
            any = true;
            gone[over] = 1;
            cur->path[cur->n++] = (uint8_t)land;
            cur->taken++;
            if (KING(g->b[over])) cur->kings++;
            int before = *nout;
            jumps(g, piece, land, gone, cur, out, nout, max);
            if (*nout == before && *nout < max) out[(*nout)++] = *cur;
            if (KING(g->b[over])) cur->kings--;
            cur->taken--;
            cur->n--;
            gone[over] = 0;
            if (!king) break;
            lf += DF[d];
            lr += DR[d];
        }
    }
    (void)any;
}

static int gen(const dm_t *g, dmv_t *out, int max)
{
    int n = g->n, k = 0;
    uint8_t gone[MAXN * MAXN];
    memset(gone, 0, sizeof gone);
    for (int sq = 0; sq < n * n; sq++) {
        if (!mine(g, g->b[sq])) continue;
        dmv_t cur;
        memset(&cur, 0, sizeof cur);
        cur.path[0] = (uint8_t)sq;
        cur.n = 1;
        gone[sq] = 2;
        int before = k;
        jumps(g, g->b[sq], sq, gone, &cur, out, &k, max);
        (void)before;
        gone[sq] = 0;
    }
    if (k) {
        /* the most, then the most kings */
        if (g->most) {
            int best = 0, bk = 0;
            for (int i = 0; i < k; i++) {
                if (out[i].taken > best) best = out[i].taken;
            }
            for (int i = 0; i < k; i++) {
                if (out[i].taken == best && out[i].kings > bk) bk = out[i].kings;
            }
            int m = 0;
            bool quality = g->n == 8;
            for (int i = 0; i < k; i++) {
                if (out[i].taken == best && (!quality || out[i].kings == bk)) out[m++] = out[i];
            }
            k = m;
        }
        /* the same landings twice (a king's routes): once */
        int m = 0;
        for (int i = 0; i < k; i++) {
            bool dup = false;
            for (int j = 0; j < m && !dup; j++) {
                dup = out[j].n == out[i].n && !memcmp(out[j].path, out[i].path, out[i].n);
            }
            if (!dup) out[m++] = out[i];
        }
        return m;
    }
    int fwd = g->side ? -1 : 1;
    for (int sq = 0; sq < n * n && k < max; sq++) {
        int p = g->b[sq];
        if (!mine(g, p)) continue;
        int f = sq % n, r = sq / n;
        for (int d = 0; d < 4; d++) {
            if (!KING(p) && DR[d] != fwd) continue;
            int ff = f + DF[d], rr = r + DR[d];
            while (ff >= 0 && ff < n && rr >= 0 && rr < n && g->b[rr * n + ff] == E && k < max) {
                dmv_t *o = &out[k++];
                memset(o, 0, sizeof *o);
                o->path[0] = (uint8_t)sq;
                o->path[1] = (uint8_t)(rr * n + ff);
                o->n = 2;
                if (!KING(p)) break;
                ff += DF[d];
                rr += DR[d];
            }
        }
    }
    return k;
}

/* makes it; with ids for the screen, or without for the search */
static void make(dm_t *g, const uint8_t *path, int np, bool ids)
{
    int n = g->n;
    int from = path[0], sq = from;
    int p = g->b[from];
    bool took = false;
    for (int i = 1; i < np; i++) {
        int to = path[i];
        int df = (to % n) > (sq % n) ? 1 : -1, dr = (to / n) > (sq / n) ? 1 : -1;
        for (int c = sq + dr * n + df; c != to; c += dr * n + df) {
            if (g->b[c] != E) {
                if (ids && g->id[c] != 0xFF && g->ncap < 48) {
                    g->cap_by[g->ncap] = g->side;
                    g->cap[g->ncap++] = g->id[c];
                    g->id[c] = 0xFF;
                }
                g->b[c] = E;
                took = true;
            }
        }
        sq = to;
    }
    g->b[from] = E;
    if (ids) {
        uint8_t id = g->id[from];
        g->id[from] = 0xFF;
        g->id[sq] = id;
    }
    int last = g->side ? 0 : n - 1;
    if (!KING(p) && sq / n == last) p = g->side ? BK : WK;
    g->b[sq] = (uint8_t)p;
    if (ids && g->id[sq] != 0xFF) g->kind[g->id[sq]] = (uint8_t)kind_of(p);
    g->quiet = took || !KING(g->b[sq]) ? 0 : (uint8_t)(g->quiet < 250 ? g->quiet + 1 : 250);
    g->side ^= 1;
}

static void judge(dm_t *g)
{
    dmv_t ms[64];
    g->result = -1;
    g->why = 0;
    if (!gen(g, ms, 64)) {
        g->result = (int8_t)(g->side ^ 1);
        g->why = 1;
    } else if (g->quiet >= 100) {
        g->result = -2;
        g->why = 2;
    }
}

static int dm_turn(const void *st)
{
    const dm_t *g = (const dm_t *)st;
    return g->result != -1 ? -1 : g->side;
}

static int dm_result(const void *st, const char **why)
{
    const dm_t *g = (const dm_t *)st;
    if (why) *why = WHY[g->why];
    return g->result;
}

static int dm_moves(const void *st, mesa_move_t *out, int max)
{
    const dm_t *g = (const dm_t *)st;
    if (g->result != -1) return 0;
    dmv_t ms[MESA_MAX_MOVES];
    int n = gen(g, ms, max < MESA_MAX_MOVES ? max : MESA_MAX_MOVES);
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        out[i].n = ms[i].n;
        memcpy(out[i].step, ms[i].path, ms[i].n);
        out[i].data = ms[i].taken;
    }
    return n;
}

static uint32_t dm_apply(void *st, const mesa_move_t *m)
{
    dm_t *g = (dm_t *)st;
    int before = g->ncap;
    int p = g->b[m->step[0]];
    make(g, m->step, m->n, true);
    uint32_t ev = MESA_EV_MOVE;
    if (g->ncap > before) ev |= MESA_EV_CAPTURE;
    if (!KING(p) && KING(g->b[m->step[m->n - 1]])) ev |= MESA_EV_PROMOTE;
    judge(g);
    if (g->result != -1) ev |= MESA_EV_END;
    return ev;
}

/* the squares numbered as the books do: the dark ones from the top left */
static int number(const dm_t *g, int sq)
{
    int n = g->n, r = sq / n, f = sq % n;
    return (n - 1 - r) * (n / 2) + f / 2 + 1;
}

static bool dm_notation(const void *st, const mesa_move_t *m, char *out, size_t sz)
{
    const dm_t *g = (const dm_t *)st;
    bool cap = m->data > 0;
    size_t k = 0;
    for (int i = 0; i < m->n && k + 5 < sz; i++) {
        if (i) out[k++] = cap ? 'x' : '-';
        k += (size_t)snprintf(out + k, sz - k, "%d", number(g, m->step[i]));
    }
    out[k] = 0;
    return true;
}

static int dm_pieces(const void *st, mesa_piece_t *out, int max)
{
    const dm_t *g = (const dm_t *)st;
    int n = 0, N = g->n;
    for (int sq = 0; sq < N * N && n < max; sq++) {
        if (g->id[sq] == 0xFF) continue;
        mesa_piece_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->id = g->id[sq];
        o->kind = g->kind[o->id];
        o->cell = (uint8_t)sq;
    }
    int slot[2] = { 0, 0 };
    for (int i = 0; i < g->ncap && n < max; i++) {
        mesa_piece_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->id = g->cap[i];
        o->kind = g->kind[o->id];
        o->cell = MESA_NO_CELL;
        o->tray = g->cap_by[i];
        o->slot = (uint8_t)slot[g->cap_by[i] & 1]++;
    }
    return n;
}

static int dm_cells(const void *st)
{
    const dm_t *g = (const dm_t *)st;
    return g->n * g->n;
}

static void dm_cell_pos(const void *st, int cell, int slot, float *u, float *v)
{
    (void)slot;
    const dm_t *g = (const dm_t *)st;
    *u = (float)(cell % g->n) + 0.5f;
    *v = (float)(cell / g->n) + 0.5f;
}

static int dm_cell_at(const void *st, float u, float v)
{
    const dm_t *g = (const dm_t *)st;
    if (u < 0 || v < 0 || u >= g->n || v >= g->n) return MESA_NO_CELL;
    return (int)v * g->n + (int)u;
}

static bool dm_info(const void *st, int player, char *out, size_t n)
{
    const dm_t *g = (const dm_t *)st;
    int men = 0, kings = 0;
    for (int sq = 0; sq < g->n * g->n; sq++) {
        int p = g->b[sq];
        if (player ? BLACK(p) : WHITE(p)) {
            if (KING(p)) kings++;
            else men++;
        }
    }
    const char *tr = mesa_host && mesa_host->tr ? mesa_host->tr("damas") : "damas";
    if (kings) snprintf(out, n, "%d + %d %s", men, kings, tr);
    else snprintf(out, n, "%d", men);
    return true;
}

/* ---- the board's play ------------------------------------------------------------------- */

typedef struct {
    mesa_ai_t *ai;
    uint32_t nodes, deadline;
    bool abort;
} dsr_t;

static int eval(const dm_t *g)
{
    int n = g->n, s = 0;
    int king = n == 8 ? 280 : 300;
    for (int sq = 0; sq < n * n; sq++) {
        int p = g->b[sq];
        if (p == E) continue;
        int r = sq / n, f = sq % n;
        int c = (f > 0 && f < n - 1 ? 4 : 0) + (r > 1 && r < n - 2 ? 3 : 0);
        int v;
        if (p == WM) v = 100 + r * 6 + c + (r == 0 ? 8 : 0);
        else if (p == BM) v = 100 + (n - 1 - r) * 6 + c + (r == n - 1 ? 8 : 0);
        else v = king + c * 2;
        s += WHITE(p) ? v : -v;
    }
    return g->side ? -s : s;
}

static int search(dsr_t *s, const dm_t *g, int depth, int alpha, int beta, int ply)
{
    s->nodes++;
    if ((s->nodes & 1023) == 0) {
        mesa_ai_t *ai = s->ai;
        if (ai->stop || (ai->now_ms && (int32_t)(ai->now_ms() - s->deadline) >= 0)) s->abort = true;
    }
    if (s->abort) return 0;
    dmv_t ms[96];
    int n = gen(g, ms, 96);
    if (!n) return -20000 + ply;
    bool capture = ms[0].taken > 0;
    /* captures go on whatever the depth: the position is not quiet */
    if (depth <= 0 && !capture) return eval(g);
    if (depth <= -6) return eval(g);
    if (g->quiet >= 100) return 0;
    /* captures first, the most first */
    for (int i = 1; i < n; i++) {
        dmv_t t = ms[i];
        int j = i - 1;
        while (j >= 0 && ms[j].taken < t.taken) {
            ms[j + 1] = ms[j];
            j--;
        }
        ms[j + 1] = t;
    }
    int best = -30000;
    for (int i = 0; i < n; i++) {
        dm_t c = *g;
        make(&c, ms[i].path, ms[i].n, false);
        int v = -search(s, &c, depth - 1, -beta, -alpha, ply + 1);
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

static bool dm_ai(const void *st, int level, mesa_ai_t *ai, mesa_move_t *out)
{
    const dm_t *g = (const dm_t *)st;
    static const struct { int depth, ms, noise; } L[6] = {
        { 1, 300, 90 }, { 2, 500, 50 }, { 4, 800, 25 }, { 6, 1500, 10 }, { 9, 3000, 0 }, { 14, 6000, 0 },
    };
    if (level < 1) level = 1;
    if (level > 6) level = 6;
    dmv_t ms[96];
    int n = gen(g, ms, 96);
    if (!n) return false;
    dsr_t s;
    memset(&s, 0, sizeof s);
    s.ai = ai;
    uint32_t t0 = ai->now_ms ? ai->now_ms() : 0;
    uint32_t ms_budget = (uint32_t)L[level - 1].ms;
    if (ai->ms && ai->ms < ms_budget) ms_budget = ai->ms;
    s.deadline = t0 + ms_budget;
    uint32_t seed = ai->seed;
    int noise[96];
    for (int i = 0; i < n; i++) noise[i] = L[level - 1].noise ? (int)(xr(&seed) % (uint32_t)(2 * L[level - 1].noise + 1)) - L[level - 1].noise : (int)(xr(&seed) % 3);
    int best = 0, depth_done = 0;
    for (int d = 1; d <= L[level - 1].depth; d++) {
        int bi = -1, bv = -40000;
        for (int i = 0; i < n; i++) {
            dm_t c = *g;
            make(&c, ms[i].path, ms[i].n, false);
            int v = -search(&s, &c, d - 1, -30000, 30000, 1);
            if (s.abort) break;
            v += noise[i];
            if (v > bv) {
                bv = v;
                bi = i;
            }
        }
        if (s.abort) break;
        if (bi >= 0) best = bi;
        depth_done = d;
        if (n == 1) break;
        if (ai->now_ms && (ai->now_ms() - t0) * 3 > ms_budget) break;
    }
    ai->nodes = s.nodes;
    ai->depth = depth_done;
    memset(out, 0, sizeof *out);
    out->n = ms[best].n;
    memcpy(out->step, ms[best].path, ms[best].n);
    out->data = ms[best].taken;
    return true;
}

const mesa_game_t mesa_damas = {
    .word = "damas",
    .name = N_("Damas"),
    .about = N_("Las de acá y las internacionales"),
    .prefix = "dm",
    .flags = MESA_GF_ROTATE | MESA_GF_CLOCK | MESA_GF_HINTS | MESA_GF_GRID,
    .min_players = 1,
    .max_players = 2,
    .levels = 6,
    .state_size = sizeof(dm_t),
    .kinds = KINDS,
    .nkinds = 4,
    .opts = OPTS,
    .nopts = 3,
    .board = dm_board,
    .players = dm_players,
    .side = dm_side,
    .init = dm_init,
    .turn = dm_turn,
    .result = dm_result,
    .info = dm_info,
    .moves = dm_moves,
    .apply = dm_apply,
    .notation = dm_notation,
    .pieces = dm_pieces,
    .cells = dm_cells,
    .cell_pos = dm_cell_pos,
    .cell_at = dm_cell_at,
    .ai_move = dm_ai,
};
