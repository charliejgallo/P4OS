/*
 * SOLITARIO - the rules. See so_game.h.
 */
#include "so_game.h"

#include <stdlib.h>
#include <string.h>

#ifndef N_
#define N_(s) (s)
#endif

const char *const so_mode_name[SO_NMODES] = {
    N_("Klondike"),
    N_("Klondike de a 3"),
    N_("Spider, 1 palo"),
    N_("Spider, 2 palos"),
    N_("Spider, 4 palos"),
    N_("FreeCell"),
    N_("Pirámide"),
    N_("TriPeaks"),
    N_("Golf"),
    N_("Yukon"),
    N_("Española"),
};

so_kind_t so_mode_kind(int mode)
{
    switch (mode) {
    case SO_M_KLON1:
    case SO_M_KLON3:    return SO_K_KLONDIKE;
    case SO_M_SPIDER1:
    case SO_M_SPIDER2:
    case SO_M_SPIDER4:  return SO_K_SPIDER;
    case SO_M_FREECELL: return SO_K_FREECELL;
    case SO_M_PYRAMID:  return SO_K_PYRAMID;
    case SO_M_TRIPEAKS: return SO_K_TRIPEAKS;
    case SO_M_GOLF:     return SO_K_GOLF;
    case SO_M_YUKON:    return SO_K_YUKON;
    default:            return SO_K_SPANISH;
    }
}

/* xorshift32, never zero */
uint32_t so_rand(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* --- the log ---------------------------------------------------------------- */

static void log_op(so_game_t *g, int op, int a, int b, int n)
{
    if (g->nops >= g->ops_cap) {
        uint32_t cap = g->ops_cap ? g->ops_cap * 2 : 1024;
        so_op_t *o = (so_op_t *)realloc(g->ops, cap * sizeof *o);
        if (!o) {
            g->oom = true;
            return;
        }
        g->ops = o;
        g->ops_cap = cap;
    }
    g->ops[g->nops++] = (so_op_t){ (uint8_t)op, (uint8_t)a, (uint8_t)b, (uint8_t)n };
}

static void raw_move(so_game_t *g, int a, int b, int n)
{
    memcpy(&g->pc[b][g->pn[b]], &g->pc[a][g->pn[a] - n], (size_t)n);
    g->pn[a] = (uint8_t)(g->pn[a] - n);
    g->pn[b] = (uint8_t)(g->pn[b] + n);
}

static void op_move(so_game_t *g, int a, int b, int n)
{
    if (n <= 0 || n > g->pn[a] || g->pn[b] + n > SO_PILE_MAX) {
        return;
    }
    raw_move(g, a, b, n);
    log_op(g, SO_OP_MOVE, a, b, n);
}

static void op_flip(so_game_t *g, int p)
{
    int c = so_top(g, p);
    if (c < 0) {
        return;
    }
    g->up[c] ^= 1;
    log_op(g, SO_OP_FLIP, p, 0, 0);
}

/* --- the deal --------------------------------------------------------------- */

static int add_pile(so_game_t *g, int type)
{
    int p = g->npiles++;
    g->ptype[p] = (uint8_t)type;
    g->pn[p] = 0;
    g->cover[p][0] = g->cover[p][1] = -1;
    return p;
}

static void put(so_game_t *g, int p, int card, bool up)
{
    g->pc[p][g->pn[p]++] = (uint8_t)card;
    g->up[card] = up;
}

void so_free(so_game_t *g)
{
    free(g->ops);
    free(g->recs);
    g->ops = NULL;
    g->recs = NULL;
    g->nops = g->ops_cap = g->nrecs = g->recs_cap = 0;
}

static int start_score(const so_game_t *g)
{
    if (g->scoring == SO_SC_VEGAS || g->scoring == SO_SC_VEGAS_CUM) {
        return -(int)g->ncards;
    }
    return g->kind == SO_K_SPIDER ? 500 : 0;
}

bool so_new(so_game_t *g, int mode, uint32_t seed, const so_opts_t *o)
{
    so_op_t  *ops  = g->ops;
    so_rec_t *recs = g->recs;
    uint32_t  oc = g->ops_cap, rc = g->recs_cap;
    memset(g, 0, sizeof *g);
    g->ops = ops;
    g->recs = recs;
    g->ops_cap = oc;
    g->recs_cap = rc;

    g->mode = (uint8_t)mode;
    g->kind = (uint8_t)so_mode_kind(mode);
    g->seed = seed;
    g->scoring = o ? o->scoring : SO_SC_STANDARD;
    g->stock = g->waste = g->discard = -1;
    g->nranks = 13;
    g->draw = mode == SO_M_KLON3 ? 3 : 1;
    g->passes_max = -1;
    if (o && o->limited) {
        g->passes_max = 3;
    }
    g->passes = 1;

    /* the deck */
    if (g->kind == SO_K_SPIDER) {
        g->ncards = 104;
        int ns = mode == SO_M_SPIDER1 ? 1 : (mode == SO_M_SPIDER2 ? 2 : 4);
        static const uint8_t SUITS2[2] = { 0, 1 };      /* spades, hearts */
        for (int i = 0; i < 104; i++) {
            int set = i / 13;
            g->suit[i] = ns == 1 ? 0 : (ns == 2 ? SUITS2[set & 1] : (uint8_t)(set & 3));
            g->rank[i] = (uint8_t)(i % 13);
        }
    } else if (g->kind == SO_K_SPANISH) {
        g->ncards = 40;
        g->nranks = 10;
        for (int i = 0; i < 40; i++) {
            g->suit[i] = (uint8_t)(i / 10);
            g->rank[i] = (uint8_t)(i % 10);
        }
    } else {
        g->ncards = 52;
        for (int i = 0; i < 52; i++) {
            g->suit[i] = (uint8_t)(i / 13);
            g->rank[i] = (uint8_t)(i % 13);
        }
    }
    g->score = start_score(g);

    uint8_t deck[SO_MAXCARDS];
    for (int i = 0; i < g->ncards; i++) {
        deck[i] = (uint8_t)i;
    }
    uint32_t s = seed * 2654435761u + 0x6D2B79F5u;
    for (int i = 0; i < 8; i++) {
        so_rand(&s);
    }
    for (int i = g->ncards - 1; i > 0; i--) {
        int j = (int)(so_rand(&s) % (uint32_t)(i + 1));
        uint8_t t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    int k = 0;

    switch (g->kind) {
    case SO_K_KLONDIKE:
    case SO_K_SPANISH: {
        g->stock = (int8_t)add_pile(g, SO_P_STOCK);
        g->waste = (int8_t)add_pile(g, SO_P_WASTE);
        g->found0 = (uint8_t)g->npiles;
        g->nfound = 4;
        for (int i = 0; i < 4; i++) add_pile(g, SO_P_FOUND);
        g->tab0 = (uint8_t)g->npiles;
        g->ntab = 7;
        for (int i = 0; i < 7; i++) add_pile(g, SO_P_TABLEAU);
        for (int r = 0; r < 7; r++) {
            for (int c = r; c < 7; c++) {
                put(g, g->tab0 + c, deck[k++], c == r);
            }
        }
        while (k < g->ncards) put(g, g->stock, deck[k++], false);
        break;
    }
    case SO_K_YUKON: {
        g->found0 = (uint8_t)g->npiles;
        g->nfound = 4;
        for (int i = 0; i < 4; i++) add_pile(g, SO_P_FOUND);
        g->tab0 = (uint8_t)g->npiles;
        g->ntab = 7;
        for (int i = 0; i < 7; i++) add_pile(g, SO_P_TABLEAU);
        put(g, g->tab0, deck[k++], true);
        for (int c = 1; c < 7; c++) {
            for (int i = 0; i < c + 5; i++) {
                put(g, g->tab0 + c, deck[k++], i >= c);
            }
        }
        break;
    }
    case SO_K_SPIDER: {
        g->stock = (int8_t)add_pile(g, SO_P_STOCK);
        g->found0 = (uint8_t)g->npiles;
        g->nfound = 8;
        for (int i = 0; i < 8; i++) add_pile(g, SO_P_FOUND);
        g->tab0 = (uint8_t)g->npiles;
        g->ntab = 10;
        for (int i = 0; i < 10; i++) add_pile(g, SO_P_TABLEAU);
        for (int r = 0; r < 6; r++) {
            for (int c = 0; c < 10; c++) {
                int n = c < 4 ? 6 : 5;
                if (r < n) put(g, g->tab0 + c, deck[k++], r == n - 1);
            }
        }
        while (k < g->ncards) put(g, g->stock, deck[k++], false);
        break;
    }
    case SO_K_FREECELL: {
        g->cell0 = (uint8_t)g->npiles;
        g->ncell = 4;
        for (int i = 0; i < 4; i++) add_pile(g, SO_P_CELL);
        g->found0 = (uint8_t)g->npiles;
        g->nfound = 4;
        for (int i = 0; i < 4; i++) add_pile(g, SO_P_FOUND);
        g->tab0 = (uint8_t)g->npiles;
        g->ntab = 8;
        for (int i = 0; i < 8; i++) add_pile(g, SO_P_TABLEAU);
        for (int i = 0; i < 52; i++) put(g, g->tab0 + i % 8, deck[k++], true);
        break;
    }
    case SO_K_PYRAMID: {
        g->stock = (int8_t)add_pile(g, SO_P_STOCK);
        g->waste = (int8_t)add_pile(g, SO_P_WASTE);
        g->discard = (int8_t)add_pile(g, SO_P_DISCARD);
        g->slot0 = (uint8_t)g->npiles;
        g->nslot = 28;
        for (int i = 0; i < 28; i++) add_pile(g, SO_P_SLOT);
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c <= r; c++) {
                int s = g->slot0 + r * (r + 1) / 2 + c;
                if (r < 6) {
                    g->cover[s][0] = (int8_t)(g->slot0 + (r + 1) * (r + 2) / 2 + c);
                    g->cover[s][1] = (int8_t)(g->cover[s][0] + 1);
                }
                put(g, s, deck[k++], true);
            }
        }
        while (k < g->ncards) put(g, g->stock, deck[k++], false);
        break;
    }
    case SO_K_TRIPEAKS: {
        g->stock = (int8_t)add_pile(g, SO_P_STOCK);
        g->waste = (int8_t)add_pile(g, SO_P_WASTE);
        g->slot0 = (uint8_t)g->npiles;
        g->nslot = 28;
        for (int i = 0; i < 28; i++) add_pile(g, SO_P_SLOT);
        int s0 = g->slot0;
        for (int p = 0; p < 3; p++) {               /* the peaks */
            g->cover[s0 + p][0] = (int8_t)(s0 + 3 + 2 * p);
            g->cover[s0 + p][1] = (int8_t)(s0 + 4 + 2 * p);
        }
        for (int q = 0; q < 6; q++) {               /* the second row */
            int p = q / 2, j = 3 * p + (q & 1);
            g->cover[s0 + 3 + q][0] = (int8_t)(s0 + 9 + j);
            g->cover[s0 + 3 + q][1] = (int8_t)(s0 + 10 + j);
        }
        for (int j = 0; j < 9; j++) {               /* the third */
            g->cover[s0 + 9 + j][0] = (int8_t)(s0 + 18 + j);
            g->cover[s0 + 9 + j][1] = (int8_t)(s0 + 19 + j);
        }
        for (int i = 0; i < 28; i++) put(g, s0 + i, deck[k++], i >= 18);
        while (k < g->ncards - 1) put(g, g->stock, deck[k++], false);
        put(g, g->waste, deck[k++], true);
        break;
    }
    case SO_K_GOLF: {
        g->stock = (int8_t)add_pile(g, SO_P_STOCK);
        g->waste = (int8_t)add_pile(g, SO_P_WASTE);
        g->tab0 = (uint8_t)g->npiles;
        g->ntab = 7;
        for (int i = 0; i < 7; i++) add_pile(g, SO_P_TABLEAU);
        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 7; c++) put(g, g->tab0 + c, deck[k++], true);
        }
        while (k < g->ncards - 1) put(g, g->stock, deck[k++], false);
        put(g, g->waste, deck[k++], true);
        break;
    }
    }
    return true;
}

/* --- small rules ------------------------------------------------------------ */

bool so_slot_free(const so_game_t *g, int p)
{
    for (int i = 0; i < 2; i++) {
        int c = g->cover[p][i];
        if (c >= 0 && g->pn[c]) {
            return false;
        }
    }
    return true;
}

static bool klondike_like(const so_game_t *g)
{
    return g->kind == SO_K_KLONDIKE || g->kind == SO_K_SPANISH || g->kind == SO_K_YUKON ||
           g->kind == SO_K_FREECELL;
}

/* may 'lo' lie on 'hi' in a tableau column */
static bool builds_on(const so_game_t *g, int lo, int hi)
{
    if (g->rank[hi] != g->rank[lo] + 1) {
        return false;
    }
    return g->kind == SO_K_SPIDER || so_red(g, hi) != so_red(g, lo);
}

/* the run a player may lift: the same rule, plus Spider wants one suit */
static bool runs_on(const so_game_t *g, int lo, int hi)
{
    if (g->kind == SO_K_SPIDER) {
        return g->rank[hi] == g->rank[lo] + 1 && g->suit[hi] == g->suit[lo];
    }
    return builds_on(g, lo, hi);
}

static int count_empty(const so_game_t *g, int p0, int n, int except)
{
    int e = 0;
    for (int i = 0; i < n; i++) {
        if (p0 + i != except && !g->pn[p0 + i]) {
            e++;
        }
    }
    return e;
}

/* FreeCell: how many cards can move at once onto 'to' */
static int fc_capacity(const so_game_t *g, int from, int to)
{
    int cells = count_empty(g, g->cell0, g->ncell, -1);
    int cols  = count_empty(g, g->tab0, g->ntab, from);
    if (!g->pn[to]) {
        cols--;
    }
    if (cols < 0) {
        cols = 0;
    }
    return (cells + 1) << cols;
}

bool so_can_pick(const so_game_t *g, int p, int idx)
{
    if (p < 0 || p >= g->npiles || idx < 0 || idx >= g->pn[p]) {
        return false;
    }
    int top = g->pn[p] - 1;
    int c = g->pc[p][idx];
    if (!g->up[c]) {
        return false;
    }
    switch (g->ptype[p]) {
    case SO_P_TABLEAU:
        if (g->kind == SO_K_GOLF) {
            return idx == top;
        }
        if (g->kind == SO_K_YUKON) {
            return true;
        }
        for (int i = idx; i < top; i++) {
            if (!runs_on(g, g->pc[p][i + 1], g->pc[p][i])) {
                return false;
            }
        }
        return true;
    case SO_P_WASTE:
        return idx == top && g->kind != SO_K_TRIPEAKS && g->kind != SO_K_GOLF;
    case SO_P_FOUND:
        return idx == top && g->kind != SO_K_SPIDER;
    case SO_P_CELL:
        return idx == top;
    case SO_P_SLOT:
        return so_slot_free(g, p);
    default:
        return false;
    }
}

static int pair_sum_ok(const so_game_t *g, int a, int b)
{
    return g->rank[a] + g->rank[b] == 11;
}

bool so_drop_move(const so_game_t *g, int p, int idx, int to, so_move_t *m)
{
    if (p == to || to < 0 || to >= g->npiles || !so_can_pick(g, p, idx)) {
        return false;
    }
    int n = g->pn[p] - idx;
    int c = g->pc[p][idx];
    int t = so_top(g, to);
    so_move_t mv = { SO_MV_MOVE, (uint8_t)p, (uint8_t)to, (uint8_t)n };

    if (g->kind == SO_K_PYRAMID) {
        if (n != 1) {
            return false;
        }
        if (g->rank[c] == 12) {
            if (to != g->discard) {
                return false;
            }
            mv = (so_move_t){ SO_MV_PAIR, (uint8_t)p, 255, 1 };
        } else {
            bool avail = (g->ptype[to] == SO_P_SLOT && g->pn[to] && so_slot_free(g, to)) ||
                         (to == g->waste && t >= 0);
            if (!avail || !pair_sum_ok(g, c, t)) {
                return false;
            }
            mv = (so_move_t){ SO_MV_PAIR, (uint8_t)p, (uint8_t)to, 2 };
        }
        if (m) *m = mv;
        return true;
    }

    switch (g->ptype[to]) {
    case SO_P_TABLEAU:
        if (g->kind == SO_K_GOLF) {
            return false;
        }
        if (t < 0) {
            if ((g->kind == SO_K_KLONDIKE || g->kind == SO_K_YUKON) && g->rank[c] != 12) return false;
            if (g->kind == SO_K_SPANISH && g->rank[c] != 9) return false;
        } else if (!g->up[t] || !builds_on(g, c, t)) {
            return false;
        }
        if (g->kind == SO_K_FREECELL && n > fc_capacity(g, p, to)) {
            return false;
        }
        break;
    case SO_P_FOUND:
        if (n != 1 || g->kind == SO_K_SPIDER) {
            return false;
        }
        if (t < 0 ? g->rank[c] != 0 : (g->suit[t] != g->suit[c] || g->rank[c] != g->rank[t] + 1)) {
            return false;
        }
        break;
    case SO_P_CELL:
        if (n != 1 || t >= 0) {
            return false;
        }
        break;
    case SO_P_WASTE:
        if ((g->kind != SO_K_TRIPEAKS && g->kind != SO_K_GOLF) || n != 1 || t < 0) {
            return false;
        } else {
            int d = (int)g->rank[c] - (int)g->rank[t];
            if (d < 0) d = -d;
            if (!(d == 1 || (g->kind == SO_K_TRIPEAKS && d == 12))) {
                return false;
            }
        }
        break;
    default:
        return false;
    }
    if (m) *m = mv;
    return true;
}

bool so_stock_move(const so_game_t *g, so_move_t *m)
{
    if (g->stock < 0) {
        return false;
    }
    if (g->pn[g->stock]) {
        if (g->kind == SO_K_SPIDER) {
            /* the deal needs every column covered */
            if (count_empty(g, g->tab0, g->ntab, -1)) {
                return false;
            }
        }
        if (m) *m = (so_move_t){ SO_MV_DRAW, (uint8_t)g->stock, 0, 0 };
        return true;
    }
    if (g->waste >= 0 && g->pn[g->waste] &&
        (g->kind == SO_K_KLONDIKE || g->kind == SO_K_SPANISH || g->kind == SO_K_PYRAMID) &&
        (g->passes_max < 0 || g->passes < g->passes_max)) {
        if (m) *m = (so_move_t){ SO_MV_REDEAL, (uint8_t)g->waste, (uint8_t)g->stock, 0 };
        return true;
    }
    return false;
}

/* --- every move --------------------------------------------------------------- */

static void push(so_move_t *out, int *n, int max, so_move_t m)
{
    if (*n < max) {
        out[(*n)++] = m;
    }
}

int so_moves(const so_game_t *g, so_move_t *out, int max)
{
    int n = 0;
    so_move_t m;

    if (g->kind == SO_K_PYRAMID) {
        int av[32], na = 0;
        for (int i = 0; i < g->nslot; i++) {
            int p = g->slot0 + i;
            if (g->pn[p] && so_slot_free(g, p)) av[na++] = p;
        }
        if (g->pn[g->waste]) av[na++] = g->waste;
        for (int i = 0; i < na; i++) {
            int a = so_top(g, av[i]);
            if (g->rank[a] == 12) {
                push(out, &n, max, (so_move_t){ SO_MV_PAIR, (uint8_t)av[i], 255, 1 });
                continue;
            }
            for (int j = i + 1; j < na; j++) {
                if (pair_sum_ok(g, a, so_top(g, av[j]))) {
                    push(out, &n, max, (so_move_t){ SO_MV_PAIR, (uint8_t)av[i], (uint8_t)av[j], 2 });
                }
            }
        }
        if (so_stock_move(g, &m)) push(out, &n, max, m);
        return n;
    }

    if (g->kind == SO_K_TRIPEAKS || g->kind == SO_K_GOLF) {
        int p0 = g->kind == SO_K_GOLF ? g->tab0 : g->slot0;
        int np = g->kind == SO_K_GOLF ? g->ntab : g->nslot;
        for (int i = 0; i < np; i++) {
            int p = p0 + i;
            if (g->pn[p] && so_drop_move(g, p, g->pn[p] - 1, g->waste, &m)) push(out, &n, max, m);
        }
        if (so_stock_move(g, &m)) push(out, &n, max, m);
        return n;
    }

    /* the column games: every source, every start, every target */
    for (int p = 0; p < g->npiles; p++) {
        int t = g->ptype[p];
        if (!g->pn[p] || t == SO_P_STOCK || t == SO_P_DISCARD) {
            continue;
        }
        int lo = g->pn[p] - 1;
        if (t == SO_P_TABLEAU) {
            while (lo > 0 && so_can_pick(g, p, lo - 1)) lo--;
        }
        for (int idx = g->pn[p] - 1; idx >= lo; idx--) {
            if (!so_can_pick(g, p, idx)) {
                continue;
            }
            for (int q = 0; q < g->npiles; q++) {
                if (so_drop_move(g, p, idx, q, &m)) {
                    push(out, &n, max, m);
                }
            }
        }
    }
    if (so_stock_move(g, &m)) push(out, &n, max, m);
    return n;
}

bool so_legal(const so_game_t *g, const so_move_t *m)
{
    so_move_t l[256];
    int n = so_moves(g, l, 256);
    for (int i = 0; i < n; i++) {
        if (l[i].kind == m->kind && l[i].from == m->from && l[i].to == m->to &&
            (m->kind != SO_MV_MOVE || l[i].n == m->n)) {
            return true;
        }
    }
    return false;
}

/* --- playing it --------------------------------------------------------------- */

static bool vegas(const so_game_t *g)
{
    return g->scoring == SO_SC_VEGAS || g->scoring == SO_SC_VEGAS_CUM;
}

/* the top card of a column face down: turn it */
static void reveal(so_game_t *g, int p)
{
    int c = so_top(g, p);
    if (c >= 0 && !g->up[c]) {
        op_flip(g, p);
        if (!vegas(g) && g->kind != SO_K_SPIDER) {
            g->score += 5;
        }
    }
}

/* Spider: a whole suit from king to ace on top of a column goes home */
static void spider_run(so_game_t *g, int p)
{
    int n = g->pn[p];
    if (n < 13) {
        return;
    }
    for (int i = 0; i < 13; i++) {
        int c = g->pc[p][n - 1 - i];
        if (!g->up[c] || g->rank[c] != i || g->suit[c] != g->suit[g->pc[p][n - 1]]) {
            return;
        }
    }
    for (int f = 0; f < g->nfound; f++) {
        if (!g->pn[g->found0 + f]) {
            op_move(g, p, g->found0 + f, 13);
            g->score += vegas(g) ? 65 : 100;
            reveal(g, p);
            return;
        }
    }
}

/* TriPeaks: positions nothing lies on any more turn up */
static void uncover(so_game_t *g)
{
    for (int i = 0; i < g->nslot; i++) {
        int p = g->slot0 + i;
        if (g->pn[p] && !g->up[so_top(g, p)] && so_slot_free(g, p)) {
            op_flip(g, p);
        }
    }
}

void so_apply(so_game_t *g, const so_move_t *m)
{
    if (g->nrecs >= g->recs_cap) {
        uint32_t cap = g->recs_cap ? g->recs_cap * 2 : 256;
        so_rec_t *r = (so_rec_t *)realloc(g->recs, cap * sizeof *r);
        if (!r) {
            g->oom = true;
            return;
        }
        g->recs = r;
        g->recs_cap = cap;
    }
    so_rec_t *r = &g->recs[g->nrecs++];
    r->op0 = g->nops;
    r->score = g->score;
    r->passes = g->passes;
    r->streak = g->streak;
    r->mv = *m;
    g->moves++;

    bool vg = vegas(g);
    int  from = m->from, to = m->to;

    switch (m->kind) {
    case SO_MV_MOVE: {
        int ft = g->ptype[from], tt = g->ptype[to];
        op_move(g, from, to, m->n);
        if (tt == SO_P_FOUND) {
            g->score += vg ? 5 : 10;
        } else if (ft == SO_P_FOUND) {
            g->score -= vg ? 5 : (g->kind == SO_K_FREECELL ? 10 : 15);
        } else if (ft == SO_P_WASTE && tt == SO_P_TABLEAU && !vg) {
            g->score += 5;
        }
        if (tt == SO_P_WASTE) {                    /* TriPeaks, Golf */
            g->streak++;
            g->score += vg ? 5 : g->streak;
            if (g->kind == SO_K_TRIPEAKS && from < g->slot0 + 3 && !vg) {
                g->score += 15;                    /* a peak */
            }
        }
        if (g->kind == SO_K_SPIDER) {
            if (!vg) g->score -= 1;
            spider_run(g, to);
        }
        if (ft == SO_P_TABLEAU && g->kind != SO_K_GOLF) {
            reveal(g, from);
        }
        if (g->kind == SO_K_TRIPEAKS) {
            uncover(g);
        }
        break;
    }
    case SO_MV_DRAW:
        if (g->kind == SO_K_SPIDER) {
            for (int i = 0; i < g->ntab && g->pn[g->stock]; i++) {
                op_move(g, g->stock, g->tab0 + i, 1);
                op_flip(g, g->tab0 + i);
            }
            if (!vg) g->score -= 1;
            for (int i = 0; i < g->ntab; i++) spider_run(g, g->tab0 + i);
        } else {
            int k = g->kind == SO_K_KLONDIKE || g->kind == SO_K_SPANISH ? g->draw : 1;
            for (int i = 0; i < k && g->pn[g->stock]; i++) {
                op_move(g, g->stock, g->waste, 1);
                op_flip(g, g->waste);
            }
            g->streak = 0;
        }
        break;
    case SO_MV_REDEAL:
        while (g->pn[g->waste]) {
            op_flip(g, g->waste);
            op_move(g, g->waste, g->stock, 1);
        }
        g->passes++;
        if (!vg && g->kind != SO_K_PYRAMID) {
            g->score -= g->draw == 1 ? 100 : 20;
        }
        break;
    case SO_MV_PAIR:
        op_move(g, from, g->discard, 1);
        if (to != 255) {
            op_move(g, to, g->discard, 1);
        }
        g->score += 5 * (to != 255 ? 2 : 1);
        if (!vg && so_won(g)) {
            g->score += 25;
        }
        break;
    }
    if (!vg && g->score < 0) {
        g->score = 0;
    }
}

bool so_undo(so_game_t *g)
{
    if (!g->nrecs) {
        return false;
    }
    so_rec_t *r = &g->recs[--g->nrecs];
    while (g->nops > r->op0) {
        so_op_t *o = &g->ops[--g->nops];
        if (o->op == SO_OP_MOVE) {
            raw_move(g, o->b, o->a, o->n);
        } else {
            g->up[so_top(g, o->a)] ^= 1;
        }
    }
    g->score = r->score;
    g->passes = r->passes;
    g->streak = r->streak;
    if (g->moves) g->moves--;
    return true;
}

/* --- the end ------------------------------------------------------------------ */

int so_home_count(const so_game_t *g)
{
    int n = 0;
    switch (g->kind) {
    case SO_K_PYRAMID:
        return g->pn[g->discard];
    case SO_K_TRIPEAKS:
        for (int i = 0; i < g->nslot; i++) n += !g->pn[g->slot0 + i];
        return n;
    case SO_K_GOLF:
        for (int i = 0; i < g->ntab; i++) n += 5 - g->pn[g->tab0 + i];
        return n;
    default:
        for (int i = 0; i < g->nfound; i++) n += g->pn[g->found0 + i];
        return n;
    }
}

bool so_won(const so_game_t *g)
{
    switch (g->kind) {
    case SO_K_PYRAMID:
    case SO_K_TRIPEAKS:
        for (int i = 0; i < g->nslot; i++) {
            if (g->pn[g->slot0 + i]) return false;
        }
        return true;
    case SO_K_GOLF:
        return so_home_count(g) == 35;
    default:
        return so_home_count(g) == g->ncards;
    }
}

bool so_stuck(const so_game_t *g)
{
    so_move_t m[4];
    return !so_won(g) && so_moves(g, m, 4) == 0;
}

/* --- what a move is worth ------------------------------------------------------- */

/* a card may go home without ever being missed below (Klondike's usual rule) */
static bool safe_home(const so_game_t *g, int c)
{
    int r = g->rank[c];
    if (r <= 1) {
        return true;
    }
    int lo_opp = 99, lo_same = 99;
    int fr[4] = { -1, -1, -1, -1 };
    for (int f = 0; f < g->nfound; f++) {
        int t = so_top(g, g->found0 + f);
        if (t >= 0) fr[g->suit[t]] = g->rank[t];
    }
    for (int s = 0; s < 4; s++) {
        if (s == g->suit[c]) continue;
        bool red = g->kind == SO_K_SPANISH ? s < 2 : (s == 1 || s == 2);
        bool opp = red != so_red(g, c);
        if (opp) {
            if (fr[s] < lo_opp) lo_opp = fr[s];
        } else if (fr[s] < lo_same) {
            lo_same = fr[s];
        }
    }
    return lo_opp >= r - 1 && (lo_same == 99 || lo_same >= r - 2);
}

static int face_down_below(const so_game_t *g, int p, int idx)
{
    int n = 0;
    for (int i = 0; i < idx; i++) n += !g->up[g->pc[p][i]];
    return n;
}

/* is there a king that would use an empty column (Klondike, Yukon) */
static bool king_waiting(const so_game_t *g, int except)
{
    int kr = g->kind == SO_K_SPANISH ? 9 : 12;
    if (g->waste >= 0) {
        int w = so_top(g, g->waste);
        if (w >= 0 && g->rank[w] == kr) return true;
    }
    for (int i = 0; i < g->ntab; i++) {
        int p = g->tab0 + i;
        if (p == except) continue;
        for (int k = 1; k < g->pn[p]; k++) {
            int c = g->pc[p][k];
            if (g->up[c] && g->rank[c] == kr && so_can_pick(g, p, k)) return true;
        }
    }
    return false;
}

static bool can_go_home(const so_game_t *g, int c)
{
    for (int f = 0; f < g->nfound; f++) {
        int t = so_top(g, g->found0 + f);
        if (t < 0 ? g->rank[c] == 0 : (g->suit[t] == g->suit[c] && g->rank[c] == g->rank[t] + 1)) {
            return true;
        }
    }
    return false;
}

int so_move_value(const so_game_t *g, const so_move_t *m)
{
    if (m->kind == SO_MV_DRAW) {
        return g->kind == SO_K_SPIDER ? 2 : 1;
    }
    if (m->kind == SO_MV_REDEAL) {
        return 0;
    }
    if (m->kind == SO_MV_PAIR) {
        int v = 20;
        for (int i = 0; i < 2; i++) {
            int p = i ? m->to : m->from;
            if (p == 255) continue;
            if (g->ptype[p] == SO_P_SLOT) {
                v += 10 + (p - g->slot0) / 4;          /* lower rows open more */
            }
        }
        return m->to == 255 ? v + 10 : v;
    }

    int from = m->from, to = m->to;
    int ft = g->ptype[from], tt = g->ptype[to];
    int idx = g->pn[from] - m->n;
    int c = g->pc[from][idx];
    int below = idx > 0 ? g->pc[from][idx - 1] : -1;
    bool opens = below >= 0 && !g->up[below];

    if (g->kind == SO_K_TRIPEAKS || g->kind == SO_K_GOLF) {
        int v = 20;
        if (g->kind == SO_K_GOLF) {
            v += g->pn[from] * 3;
        } else {
            for (int i = 0; i < g->nslot; i++) {
                int p = g->slot0 + i;
                if (g->cover[p][0] == from || g->cover[p][1] == from) v += 6;
            }
        }
        return v;
    }

    if (ft == SO_P_FOUND) {
        return -1;
    }
    if (tt == SO_P_FOUND) {
        int v = (klondike_like(g) && safe_home(g, c)) ? 100 : 60;
        if (opens) v += 20;
        if (ft == SO_P_WASTE) v += 5;
        return v;
    }

    if (g->kind == SO_K_SPIDER) {
        if (ft != SO_P_TABLEAU) return -1;
        int t = so_top(g, to);
        bool same_below = below >= 0 && g->up[below] && runs_on(g, c, below);
        if (t < 0) {
            if (idx == 0) return -1;
            if (same_below) return -1;
            return opens ? 25 : 8;
        }
        bool suit = g->suit[t] == g->suit[c];
        if (same_below) return -1;                 /* the run was already joined */
        int v = 10;
        if (suit) v += 40;
        if (opens) v += 30 + 20 / (face_down_below(g, from, idx) + 1);
        else if (below >= 0 && g->up[below]) v += 15;
        else if (idx == 0) v += 20;                /* empties a column */
        return v;
    }

    if (g->kind == SO_K_FREECELL) {
        if (tt == SO_P_CELL) {
            if (ft == SO_P_CELL) return -1;
            int v = 5;
            if (below >= 0 && can_go_home(g, below)) v += 40;
            if (below >= 0 && runs_on(g, c, below)) v -= 4;
            return v;
        }
        if (ft == SO_P_CELL) {
            return g->pn[to] ? 35 : 12;
        }
        /* column to column */
        if (below >= 0 && runs_on(g, c, below)) return -1;
        if (!g->pn[to]) {
            return idx == 0 ? -1 : 10;
        }
        int v = 30;
        if (below >= 0 && can_go_home(g, below)) v += 20;
        if (idx == 0) v += 10;
        return v;
    }

    /* Klondike, the Spanish one, Yukon */
    if (ft == SO_P_WASTE) {
        return 20;
    }
    if (ft == SO_P_TABLEAU && tt == SO_P_TABLEAU) {
        if (opens) {
            return 50 + face_down_below(g, from, idx);
        }
        if (idx == 0) {
            if (!g->pn[to]) return -1;
            return king_waiting(g, from) ? 12 : -1;
        }
        /* a face-up card stays below */
        if (can_go_home(g, below)) {
            return 30;
        }
        if (g->kind == SO_K_YUKON && !builds_on(g, c, below)) {
            return 8;
        }
        return -1;
    }
    return -1;
}

/* --- for the player ----------------------------------------------------------- */

bool so_tap_move(const so_game_t *g, int p, int idx, bool home, so_move_t *m)
{
    if (p == g->stock) {
        return so_stock_move(g, m);
    }
    if (!so_can_pick(g, p, idx)) {
        return false;
    }
    so_move_t mv, best = { 0 };
    int bestv = -1000;
    bool any = false;
    if (g->kind == SO_K_PYRAMID) {
        if (g->rank[g->pc[p][idx]] == 12) {
            return so_drop_move(g, p, idx, g->discard, m);
        }
        for (int q = 0; q < g->npiles; q++) {
            if (so_drop_move(g, p, idx, q, &mv)) {
                int v = so_move_value(g, &mv);
                if (!any || v > bestv) {
                    best = mv;
                    bestv = v;
                    any = true;
                }
            }
        }
        if (any && m) *m = best;
        return any;
    }
    for (int q = 0; q < g->npiles; q++) {
        if (home && g->ptype[q] != SO_P_FOUND) {
            continue;
        }
        if (!so_drop_move(g, p, idx, q, &mv)) {
            continue;
        }
        int v = so_move_value(g, &mv);
        /* a tap prefers keeping the card in play unless home is safe; a
         * column is better than a free cell, and a cell than nothing */
        if (g->ptype[q] == SO_P_FOUND && v < 100 && !home) v = 40;
        if (g->ptype[q] == SO_P_TABLEAU && v < 0) v = g->pn[q] ? 3 : 1;
        if (g->ptype[q] == SO_P_CELL && v < 2) v = 2;
        if (!any || v > bestv) {
            best = mv;
            bestv = v;
            any = true;
        }
    }
    if (any && m) *m = best;
    return any;
}

bool so_can_autocomplete(const so_game_t *g)
{
    if (!klondike_like(g) || so_won(g)) {
        return false;
    }
    if (g->stock >= 0 && g->pn[g->stock]) {
        return false;
    }
    /* every card up, and the foundations can eat everything in turn */
    int8_t fr[4] = { -1, -1, -1, -1 };
    for (int f = 0; f < g->nfound; f++) {
        int t = so_top(g, g->found0 + f);
        if (t >= 0) fr[g->suit[t]] = (int8_t)g->rank[t];
    }
    uint8_t ptr[SO_MAXPILES];
    for (int p = 0; p < g->npiles; p++) {
        ptr[p] = g->pn[p];
        if (g->ptype[p] == SO_P_TABLEAU) {
            for (int i = 0; i < g->pn[p]; i++) {
                if (!g->up[g->pc[p][i]]) return false;
            }
        }
    }
    int home = so_home_count(g);
    bool moved = true;
    while (moved) {
        moved = false;
        for (int p = 0; p < g->npiles; p++) {
            int t = g->ptype[p];
            if (t != SO_P_TABLEAU && t != SO_P_WASTE && t != SO_P_CELL) continue;
            while (ptr[p]) {
                int c = g->pc[p][ptr[p] - 1];
                if (g->rank[c] != fr[g->suit[c]] + 1) break;
                fr[g->suit[c]]++;
                ptr[p]--;
                home++;
                moved = true;
            }
        }
    }
    return home == g->ncards;
}

bool so_auto_step(const so_game_t *g, so_move_t *m)
{
    int best = -1, bestr = 99;
    so_move_t mv;
    for (int p = 0; p < g->npiles; p++) {
        int t = g->ptype[p];
        if (t != SO_P_TABLEAU && t != SO_P_WASTE && t != SO_P_CELL) continue;
        int c = so_top(g, p);
        if (c < 0) continue;
        for (int f = 0; f < g->nfound; f++) {
            if (so_drop_move(g, p, g->pn[p] - 1, g->found0 + f, &mv) && g->rank[c] < bestr) {
                bestr = g->rank[c];
                best = p;
                if (m) *m = mv;
            }
        }
    }
    return best >= 0;
}

bool so_hint_quick(const so_game_t *g, so_move_t *m)
{
    so_move_t l[256];
    int n = so_moves(g, l, 256);
    int bi = -1, bv = -1;
    for (int i = 0; i < n; i++) {
        int v = so_move_value(g, &l[i]);
        if (l[i].kind == SO_MV_DRAW || l[i].kind == SO_MV_REDEAL) {
            v = -1;                         /* only when nothing else */
        }
        if (v > bv) {
            bv = v;
            bi = i;
        }
    }
    if (bi < 0) {
        for (int i = 0; i < n; i++) {
            if (l[i].kind == SO_MV_DRAW || l[i].kind == SO_MV_REDEAL) {
                bi = i;
                break;
            }
        }
    }
    if (bi < 0) {
        return false;
    }
    if (m) *m = l[bi];
    return true;
}

/* --- the hash ------------------------------------------------------------------- */

static inline uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

uint64_t so_hash(const so_game_t *g)
{
    uint64_t h = 0, comm = 0;
    for (int p = 0; p < g->npiles; p++) {
        uint64_t ph = 1469598103934665603ULL;
        for (int i = 0; i < g->pn[p]; i++) {
            int c = g->pc[p][i];
            /* the same card of the other deck is the same card */
            int face = g->suit[c] * 16 + g->rank[c];
            ph = (ph ^ (uint64_t)(face * 2 + g->up[c] + 1)) * 1099511628211ULL;
        }
        int t = g->ptype[p];
        if (t == SO_P_TABLEAU || t == SO_P_CELL || t == SO_P_FOUND) {
            comm += mix64(ph + (uint64_t)t);
        } else if (t != SO_P_DISCARD) {
            h = mix64(h ^ ph ^ ((uint64_t)p << 56));
        }
    }
    uint64_t pass = g->passes_max >= 0 ? g->passes : 0;
    return mix64(h + comm + pass * 0x9E3779B97F4A7C15ULL);
}
