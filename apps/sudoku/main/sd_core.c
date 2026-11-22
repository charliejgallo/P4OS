/*
 * SUDOKU - the engine (see sd_core.h)
 */
#include "sd_core.h"

#include <stdlib.h>
#include <string.h>

#define BIT(d) ((uint16_t)(1u << (d)))

/* The generator's stop flag, seen by the long loops too (a Master board's
 * grading can run for seconds on the board, and destroy() waits for it). */
static volatile bool *s_abort;
#define ABORTED() (s_abort && *s_abort)

/* ==========================================================================
 * The shape
 * ========================================================================== */

void sd_shape_init(sd_shape_t *s, int n, bool diag)
{
    memset(s, 0, sizeof *s);
    s->n = (uint8_t)n;
    s->bw = 3;
    s->bh = (uint8_t)(n == 6 ? 2 : 3);
    s->diag = diag;
    s->ncells = (uint8_t)(n * n);
    memset(s->cage_of, 0xFF, sizeof s->cage_of);
    s->all = (uint16_t)(((1u << (n + 1)) - 1) & ~1u);

    int u = 0;
    for (int r = 0; r < n; r++, u++) {
        s->unit_kind[u] = SD_U_ROW;
        s->unit_len[u] = (uint8_t)n;
        for (int c = 0; c < n; c++) s->unit_cell[u][c] = (uint8_t)(r * n + c);
    }
    for (int c = 0; c < n; c++, u++) {
        s->unit_kind[u] = SD_U_COL;
        s->unit_len[u] = (uint8_t)n;
        for (int r = 0; r < n; r++) s->unit_cell[u][r] = (uint8_t)(r * n + c);
    }
    int bpr = n / s->bw;            /* boxes per row of boxes */
    for (int b = 0; b < n; b++, u++) {
        s->unit_kind[u] = SD_U_BOX;
        s->unit_len[u] = (uint8_t)n;
        int r0 = (b / bpr) * s->bh, c0 = (b % bpr) * s->bw, k = 0;
        for (int r = r0; r < r0 + s->bh; r++)
            for (int c = c0; c < c0 + s->bw; c++) s->unit_cell[u][k++] = (uint8_t)(r * n + c);
    }
    if (diag) {
        s->unit_kind[u] = SD_U_DIAG;
        s->unit_len[u] = (uint8_t)n;
        for (int i = 0; i < n; i++) s->unit_cell[u][i] = (uint8_t)(i * n + i);
        u++;
        s->unit_kind[u] = SD_U_DIAG;
        s->unit_len[u] = (uint8_t)n;
        for (int i = 0; i < n; i++) s->unit_cell[u][i] = (uint8_t)(i * n + (n - 1 - i));
        u++;
    }
    s->nunits = (uint8_t)u;

    for (int i = 0; i < s->ncells; i++) {
        int r = i / n, c = i % n;
        s->cell_unit[i][0] = (uint8_t)r;
        s->cell_unit[i][1] = (uint8_t)(n + c);
        s->cell_unit[i][2] = (uint8_t)(2 * n + (r / s->bh) * bpr + c / s->bw);
        s->cell_unit[i][3] = 0xFF;
        s->cell_diag2[i] = 0xFF;
        if (diag) {
            if (r == c) s->cell_unit[i][3] = (uint8_t)(3 * n);
            if (r + c == n - 1) {
                if (s->cell_unit[i][3] == 0xFF) s->cell_unit[i][3] = (uint8_t)(3 * n + 1);
                else s->cell_diag2[i] = (uint8_t)(3 * n + 1);
            }
        }
    }
    sd_shape_finish(s);
}

void sd_shape_finish(sd_shape_t *s)
{
    for (int i = 0; i < s->ncells; i++) {
        uint8_t mark[SD_MAXC] = { 0 };
        int np = 0;
        for (int u = 0; u < s->nunits; u++) {
            bool in = false;
            for (int k = 0; k < s->unit_len[u]; k++)
                if (s->unit_cell[u][k] == i) in = true;
            if (!in) continue;
            for (int k = 0; k < s->unit_len[u]; k++) {
                int j = s->unit_cell[u][k];
                if (j != i && !mark[j]) {
                    mark[j] = 1;
                    s->peer[i][np++] = (uint8_t)j;
                }
            }
        }
        int cg = s->cage_of[i];
        if (cg != 0xFF) {
            for (int k = 0; k < s->cage_len[cg]; k++) {
                int j = s->cage_cell[cg][k];
                if (j != i && !mark[j] && np < SD_MAXPEERS) {
                    mark[j] = 1;
                    s->peer[i][np++] = (uint8_t)j;
                }
            }
        }
        s->npeers[i] = (uint8_t)np;
    }
}

bool sd_sees(const sd_shape_t *s, int a, int b)
{
    if (a == b) return false;
    for (int k = 0; k < s->npeers[a]; k++)
        if (s->peer[a][k] == b) return true;
    return false;
}

/* ==========================================================================
 * Brute force
 * ========================================================================== */

typedef struct {
    const sd_shape_t *s;
    uint8_t  g[SD_MAXC];
    uint16_t ru[SD_MAXN], cu[SD_MAXN], bu[SD_MAXN], du[2];
    uint16_t ku[SD_MAXCAGES];
    int8_t   ks[SD_MAXCAGES], kn[SD_MAXCAGES];
    int      count, limit;
    uint8_t *out;
    uint32_t nodes, budget;
    bool     oob;
    uint32_t *rng;               /* random digit order, for sd_random_full */
} bf_t;

/* can 'cnt' more distinct digits of 1..n, none in 'used', add up to 'sum'? */
static bool cage_feasible(int n, int cnt, int sum, uint16_t used)
{
    if (cnt == 0) return sum == 0;
    if (sum <= 0) return false;
    int lo = 0, hi = 0, k = 0;
    for (int d = 1; d <= n && k < cnt; d++)
        if (!(used & BIT(d))) { lo += d; k++; }
    if (k < cnt) return false;
    k = 0;
    for (int d = n; d >= 1 && k < cnt; d--)
        if (!(used & BIT(d))) { hi += d; k++; }
    return sum >= lo && sum <= hi;
}

static inline uint16_t bf_cand(const bf_t *b, int i)
{
    const sd_shape_t *s = b->s;
    uint16_t m = s->all & (uint16_t)~(b->ru[s->cell_unit[i][0]] | b->cu[s->cell_unit[i][1] - s->n] |
                                      b->bu[s->cell_unit[i][2] - 2 * s->n]);
    if (s->diag) {
        if (s->cell_unit[i][3] != 0xFF) m &= (uint16_t)~b->du[s->cell_unit[i][3] - 3 * s->n];
        if (s->cell_diag2[i] != 0xFF) m &= (uint16_t)~b->du[1];
    }
    int cg = s->cage_of[i];
    if (cg != 0xFF && m) {
        m &= (uint16_t)~b->ku[cg];
        uint16_t ok = 0;
        for (uint16_t t = m; t; t &= t - 1) {
            int d = sd_lowest(t);
            if (cage_feasible(s->n, b->kn[cg] - 1, b->ks[cg] - d, b->ku[cg] | BIT(d))) ok |= BIT(d);
        }
        m = ok;
    }
    return m;
}

static void bf_set(bf_t *b, int i, int d, bool on)
{
    const sd_shape_t *s = b->s;
    uint16_t bit = BIT(d);
    int r = s->cell_unit[i][0], c = s->cell_unit[i][1] - s->n, x = s->cell_unit[i][2] - 2 * s->n;
    if (on) {
        b->g[i] = (uint8_t)d;
        b->ru[r] |= bit; b->cu[c] |= bit; b->bu[x] |= bit;
    } else {
        b->g[i] = 0;
        b->ru[r] &= (uint16_t)~bit; b->cu[c] &= (uint16_t)~bit; b->bu[x] &= (uint16_t)~bit;
    }
    if (s->diag) {
        if (s->cell_unit[i][3] != 0xFF) {
            int k = s->cell_unit[i][3] - 3 * s->n;
            if (on) b->du[k] |= bit; else b->du[k] &= (uint16_t)~bit;
        }
        if (s->cell_diag2[i] != 0xFF) {
            if (on) b->du[1] |= bit; else b->du[1] &= (uint16_t)~bit;
        }
    }
    int cg = s->cage_of[i];
    if (cg != 0xFF) {
        if (on) { b->ku[cg] |= bit; b->ks[cg] -= (int8_t)d; b->kn[cg]--; }
        else    { b->ku[cg] &= (uint16_t)~bit; b->ks[cg] += (int8_t)d; b->kn[cg]++; }
    }
}

static void bf_rec(bf_t *b)
{
    if (b->count >= b->limit || b->oob) return;
    if (b->budget && ++b->nodes > b->budget) {
        b->oob = true;
        return;
    }
    const sd_shape_t *s = b->s;
    int best = -1, bestc = 99;
    uint16_t bestm = 0;
    for (int i = 0; i < s->ncells; i++) {
        if (b->g[i]) continue;
        uint16_t m = bf_cand(b, i);
        int c = sd_popcount(m);
        if (c == 0) return;
        if (c < bestc) {
            bestc = c;
            best = i;
            bestm = m;
            if (c == 1) break;
        }
    }
    if (best < 0) {
        b->count++;
        if (b->count == 1 && b->out) memcpy(b->out, b->g, s->ncells);
        return;
    }
    uint8_t order[SD_MAXN];
    int no = 0;
    for (uint16_t t = bestm; t; t &= t - 1) order[no++] = (uint8_t)sd_lowest(t);
    if (b->rng) {
        for (int k = no - 1; k > 0; k--) {
            int j = (int)(sd_rand(b->rng) % (uint32_t)(k + 1));
            uint8_t tmp = order[k]; order[k] = order[j]; order[j] = tmp;
        }
    }
    for (int k = 0; k < no; k++) {
        bf_set(b, best, order[k], true);
        bf_rec(b);
        bf_set(b, best, order[k], false);
        if (b->count >= b->limit || b->oob) return;
    }
}

static bool bf_load(bf_t *b, const sd_shape_t *s, const uint8_t *grid)
{
    memset(b, 0, sizeof *b);
    b->s = s;
    for (int k = 0; k < s->ncages; k++) {
        b->ks[k] = (int8_t)s->cage_sum[k];
        b->kn[k] = (int8_t)s->cage_len[k];
    }
    for (int i = 0; i < s->ncells; i++) {
        int d = grid ? grid[i] : 0;
        if (!d) continue;
        if (!(bf_cand(b, i) & BIT(d))) return false;
        bf_set(b, i, d, true);
    }
    return true;
}

int sd_count(const sd_shape_t *s, const uint8_t *grid, int limit, uint8_t *out, uint32_t budget)
{
    bf_t *b = (bf_t *)malloc(sizeof(bf_t));
    if (!b) return -1;
    int r = 0;
    if (bf_load(b, s, grid)) {
        b->limit = limit;
        b->out = out;
        b->budget = budget;
        bf_rec(b);
        r = b->oob ? -1 : b->count;
    }
    free(b);
    return r;
}

bool sd_random_full(const sd_shape_t *s, uint32_t *rng, uint8_t *out)
{
    bf_t *b = (bf_t *)malloc(sizeof(bf_t));
    if (!b) return false;
    bool ok = false;
    if (bf_load(b, s, NULL)) {
        b->limit = 1;
        b->out = out;
        b->rng = rng;
        b->budget = 200000;
        bf_rec(b);
        ok = b->count == 1;
    }
    free(b);
    return ok;
}

/* ==========================================================================
 * The logical solver
 * ========================================================================== */

int sd_tech_level(int t)
{
    switch (t) {
    case SD_T_FULL_HOUSE: case SD_T_NAKED_SINGLE: case SD_T_HIDDEN_SINGLE: case SD_T_CAGE_COMBO:
        return SD_L_EASY;
    case SD_T_INNIE: case SD_T_POINTING: case SD_T_CLAIMING: case SD_T_NAKED_PAIR: case SD_T_HIDDEN_PAIR:
        return SD_L_MEDIUM;
    case SD_T_NAKED_TRIPLE: case SD_T_HIDDEN_TRIPLE: case SD_T_XWING: case SD_T_XYWING: case SD_T_XYZWING:
    case SD_T_SWORDFISH:
        return SD_L_HARD;
    case SD_T_NAKED_QUAD: case SD_T_HIDDEN_QUAD: case SD_T_JELLYFISH: case SD_T_CHAIN_SHORT:
        return SD_L_EXPERT;
    default:
        return SD_L_MASTER;
    }
}

void sd_logic_init(sd_logic_t *L, const sd_shape_t *s, const uint8_t *val)
{
    L->s = s;
    for (int i = 0; i < s->ncells; i++) {
        L->val[i] = val[i];
        L->cand[i] = 0;
    }
    for (int i = 0; i < s->ncells; i++) {
        if (L->val[i]) continue;
        uint16_t m = s->all;
        for (int k = 0; k < s->npeers[i]; k++) {
            int v = L->val[s->peer[i][k]];
            if (v) m &= (uint16_t)~BIT(v);
        }
        L->cand[i] = m;
    }
}

bool sd_logic_solved(const sd_logic_t *L)
{
    for (int i = 0; i < L->s->ncells; i++)
        if (!L->val[i]) return false;
    return true;
}

bool sd_logic_broken(const sd_logic_t *L)
{
    const sd_shape_t *s = L->s;
    for (int i = 0; i < s->ncells; i++)
        if (!L->val[i] && !L->cand[i]) return true;
    for (int u = 0; u < s->nunits; u++) {
        uint16_t have = 0;
        for (int k = 0; k < s->unit_len[u]; k++) {
            int c = s->unit_cell[u][k];
            have |= L->val[c] ? BIT(L->val[c]) : L->cand[c];
        }
        if (have != s->all) return true;
    }
    return false;
}

static void place(sd_logic_t *L, int c, int d)
{
    const sd_shape_t *s = L->s;
    L->val[c] = (uint8_t)d;
    L->cand[c] = 0;
    for (int k = 0; k < s->npeers[c]; k++) L->cand[s->peer[c][k]] &= (uint16_t)~BIT(d);
}

void sd_logic_apply(sd_logic_t *L, const sd_step_t *st)
{
    if (st->cell >= 0 && st->digit) {
        place(L, st->cell, st->digit);
        return;
    }
    for (int k = 0; k < st->nelim; k++) L->cand[st->elim_cell[k]] &= (uint16_t)~BIT(st->elim_digit[k]);
}

static void step_clear(sd_step_t *st, int tech)
{
    memset(st, 0, sizeof *st);
    st->tech = (uint8_t)tech;
    st->cell = -1;
    st->unit = 0xFF;
    st->unit2 = 0xFF;
}

static void add_elim(sd_step_t *st, int c, int d)
{
    for (int k = 0; k < st->nelim; k++)
        if (st->elim_cell[k] == c && st->elim_digit[k] == d) return;
    if (st->nelim < SD_STEP_ELIMS) {
        st->elim_cell[st->nelim] = (uint8_t)c;
        st->elim_digit[st->nelim] = (uint8_t)d;
        st->nelim++;
    }
}

static void add_cell(sd_step_t *st, int c)
{
    for (int k = 0; k < st->ncells; k++)
        if (st->cells[k] == c) return;
    if (st->ncells < SD_STEP_CELLS) st->cells[st->ncells++] = (uint8_t)c;
}

/* ---- singles --------------------------------------------------------------- */

static bool full_house(sd_logic_t *L, sd_step_t *st)
{
    const sd_shape_t *s = L->s;
    for (int u = 0; u < s->nunits; u++) {
        int empty = -1, ne = 0;
        uint16_t have = 0;
        for (int k = 0; k < s->unit_len[u]; k++) {
            int c = s->unit_cell[u][k];
            if (L->val[c]) have |= BIT(L->val[c]);
            else { empty = c; ne++; }
        }
        if (ne != 1) continue;
        uint16_t miss = s->all & (uint16_t)~have;
        if (sd_popcount(miss) != 1 || !(L->cand[empty] & miss)) continue;
        step_clear(st, SD_T_FULL_HOUSE);
        st->cell = (int8_t)empty;
        st->digit = (uint8_t)sd_lowest(miss);
        st->unit = (uint8_t)u;
        return true;
    }
    return false;
}

static bool naked_single(sd_logic_t *L, sd_step_t *st)
{
    const sd_shape_t *s = L->s;
    for (int i = 0; i < s->ncells; i++) {
        if (L->val[i] || sd_popcount(L->cand[i]) != 1) continue;
        step_clear(st, SD_T_NAKED_SINGLE);
        st->cell = (int8_t)i;
        st->digit = (uint8_t)sd_lowest(L->cand[i]);
        return true;
    }
    return false;
}

/* boxes first (they read best), then rows, columns, diagonals */
static int unit_order(const sd_shape_t *s, int k)
{
    int n = s->n;
    if (k < n) return 2 * n + k;
    if (k < 3 * n) return k - n;
    return k;
}

static bool hidden_single(sd_logic_t *L, sd_step_t *st)
{
    const sd_shape_t *s = L->s;
    for (int k = 0; k < s->nunits; k++) {
        int u = unit_order(s, k);
        for (int d = 1; d <= s->n; d++) {
            int where = -1, cnt = 0;
            bool placed = false;
            for (int j = 0; j < s->unit_len[u]; j++) {
                int c = s->unit_cell[u][j];
                if (L->val[c] == d) placed = true;
                else if (L->cand[c] & BIT(d)) { where = c; cnt++; }
            }
            if (placed || cnt != 1) continue;
            step_clear(st, SD_T_HIDDEN_SINGLE);
            st->cell = (int8_t)where;
            st->digit = (uint8_t)d;
            st->unit = (uint8_t)u;
            return true;
        }
    }
    return false;
}

/* ---- Killer ------------------------------------------------------------------ */

typedef struct {
    const sd_logic_t *L;
    const uint8_t *cells;
    int k;
    uint16_t poss[SD_MAXN];
    uint8_t pick[SD_MAXN];
    uint32_t work;
} combo_t;

static void combo_rec(combo_t *c, int i, int sum, uint16_t used)
{
    if (++c->work > 200000) return;
    if (i == c->k) {
        if (sum == 0)
            for (int j = 0; j < c->k; j++) c->poss[j] |= BIT(c->pick[j]);
        return;
    }
    int left = c->k - i;
    if (!cage_feasible(c->L->s->n, left, sum, used)) return;
    uint16_t m = c->L->cand[c->cells[i]] & (uint16_t)~used;
    for (; m; m &= m - 1) {
        int d = sd_lowest(m);
        if (d > sum) break;
        /* the rest only needs something new to try once this digit already
         * appears everywhere it could: a cheap cut, not needed for truth */
        c->pick[i] = (uint8_t)d;
        combo_rec(c, i + 1, sum - d, used | BIT(d));
    }
}

/* the digits each empty cell of a group with this rest sum can still take */
static void group_possible(const sd_logic_t *L, const uint8_t *cells, int k, int sum, uint16_t used,
                           uint16_t *poss)
{
    combo_t c;
    memset(&c, 0, sizeof c);
    c.L = L;
    c.cells = cells;
    c.k = k;
    combo_rec(&c, 0, sum, used);
    for (int j = 0; j < k; j++) poss[j] = c.work > 200000 ? L->cand[cells[j]] : c.poss[j];
}

uint16_t sd_cage_digits(int len, int sum, uint16_t allowed)
{
    uint16_t out = 0;
    /* every subset of the allowed digits, at most 2^9 */
    for (uint32_t m = 0; m < 1024; m += 2) {
        if ((m & allowed) != m || sd_popcount(m) != len) continue;
        int t = 0;
        for (int d = 1; d <= 9; d++)
            if (m & (1u << d)) t += d;
        if (t == sum) out |= (uint16_t)m;
    }
    return out;
}

static bool cage_combo(sd_logic_t *L, sd_step_t *st)
{
    const sd_shape_t *s = L->s;
    for (int g = 0; g < s->ncages; g++) {
        uint8_t empty[SD_CAGE_MAX];
        int k = 0, sum = s->cage_sum[g];
        uint16_t used = 0;
        for (int j = 0; j < s->cage_len[g]; j++) {
            int c = s->cage_cell[g][j];
            if (L->val[c]) { sum -= L->val[c]; used |= BIT(L->val[c]); }
            else empty[k++] = (uint8_t)c;
        }
        if (!k) continue;
        uint16_t poss[SD_MAXN];
        group_possible(L, empty, k, sum, used, poss);
        bool any = false;
        for (int j = 0; j < k; j++)
            if (L->cand[empty[j]] & (uint16_t)~poss[j]) any = true;
        if (!any) continue;
        step_clear(st, SD_T_CAGE_COMBO);
        st->sum = s->cage_sum[g];
        for (int j = 0; j < s->cage_len[g]; j++) add_cell(st, s->cage_cell[g][j]);
        for (int j = 0; j < k; j++) {
            st->digits |= poss[j];
            for (uint16_t m = L->cand[empty[j]] & (uint16_t)~poss[j]; m; m &= m - 1)
                add_elim(st, empty[j], sd_lowest(m));
        }
        return true;
    }
    return false;
}

/* The rule of 45: a unit adds up to 1+..+n; the cages that lie wholly inside
 * it take their sums out, and what is left of it adds up to the rest. */
static bool innie(sd_logic_t *L, sd_step_t *st)
{
    const sd_shape_t *s = L->s;
    int total = s->n * (s->n + 1) / 2;
    for (int u = 0; u < 3 * s->n; u++) {
        uint8_t inside[SD_MAXCAGES] = { 0 };
        int sum_in = 0;
        for (int j = 0; j < s->unit_len[u]; j++) {
            int g = s->cage_of[s->unit_cell[u][j]];
            if (g == 0xFF || inside[g]) continue;
            bool whole = true;
            for (int q = 0; q < s->cage_len[g]; q++) {
                int c = s->cage_cell[g][q];
                bool in = false;
                for (int z = 0; z < s->unit_len[u]; z++)
                    if (s->unit_cell[u][z] == c) in = true;
                if (!in) whole = false;
            }
            if (whole) {
                inside[g] = 1;
                sum_in += s->cage_sum[g];
            } else {
                inside[g] = 2;
            }
        }
        uint8_t rest[SD_MAXN], empty[SD_MAXN];
        int nr = 0, ne = 0, sum = total - sum_in;
        uint16_t used = 0;
        for (int j = 0; j < s->unit_len[u]; j++) {
            int c = s->unit_cell[u][j];
            int g = s->cage_of[c];
            if (g != 0xFF && inside[g] == 1) continue;
            rest[nr++] = (uint8_t)c;
            if (L->val[c]) { sum -= L->val[c]; used |= BIT(L->val[c]); }
            else empty[ne++] = (uint8_t)c;
        }
        if (!ne || nr > 4 || nr == s->n) continue;
        uint16_t poss[SD_MAXN];
        group_possible(L, empty, ne, sum, used, poss);
        bool any = false;
        for (int j = 0; j < ne; j++)
            if (L->cand[empty[j]] & (uint16_t)~poss[j]) any = true;
        if (!any) continue;
        step_clear(st, SD_T_INNIE);
        st->unit = (uint8_t)u;
        st->sum = (uint8_t)(total - sum_in);
        for (int j = 0; j < nr; j++) add_cell(st, rest[j]);
        if (ne == 1 && sd_popcount(poss[0]) == 1) {
            st->cell = (int8_t)empty[0];
            st->digit = (uint8_t)sd_lowest(poss[0]);
            return true;
        }
        for (int j = 0; j < ne; j++) {
            st->digits |= poss[j];
            for (uint16_t m = L->cand[empty[j]] & (uint16_t)~poss[j]; m; m &= m - 1)
                add_elim(st, empty[j], sd_lowest(m));
        }
        return true;
    }
    return false;
}

/* ---- locked candidates ------------------------------------------------------- */

static bool unit_has(const sd_shape_t *s, int u, int c)
{
    for (int k = 0; k < s->unit_len[u]; k++)
        if (s->unit_cell[u][k] == c) return true;
    return false;
}

/* The digit's places in unit A all lie where A meets B: it leaves the rest
 * of B. A box and a line is the usual pair; a diagonal counts too. */
static bool locked(sd_logic_t *L, sd_step_t *st, bool from_box)
{
    const sd_shape_t *s = L->s;
    for (int a = 0; a < s->nunits; a++) {
        bool abox = s->unit_kind[a] == SD_U_BOX;
        if (abox != from_box) continue;
        for (int d = 1; d <= s->n; d++) {
            uint8_t where[SD_MAXN];
            int nw = 0;
            for (int k = 0; k < s->unit_len[a]; k++) {
                int c = s->unit_cell[a][k];
                if (!L->val[c] && (L->cand[c] & BIT(d))) where[nw++] = (uint8_t)c;
            }
            if (nw < 2) continue;
            for (int b = 0; b < s->nunits; b++) {
                if (b == a || s->unit_kind[b] == s->unit_kind[a]) continue;
                if (from_box ? s->unit_kind[b] == SD_U_BOX : s->unit_kind[b] != SD_U_BOX) continue;
                bool all = true;
                for (int k = 0; k < nw && all; k++)
                    if (!unit_has(s, b, where[k])) all = false;
                if (!all) continue;
                step_clear(st, from_box ? SD_T_POINTING : SD_T_CLAIMING);
                for (int k = 0; k < s->unit_len[b]; k++) {
                    int c = s->unit_cell[b][k];
                    if (!L->val[c] && (L->cand[c] & BIT(d)) && !unit_has(s, a, c)) add_elim(st, c, d);
                }
                if (!st->nelim) continue;
                st->unit = (uint8_t)a;
                st->unit2 = (uint8_t)b;
                st->digits = BIT(d);
                st->digit = 0;
                for (int k = 0; k < nw; k++) add_cell(st, where[k]);
                return true;
            }
        }
    }
    return false;
}

/* ---- subsets ------------------------------------------------------------------ */

static bool naked_subset(sd_logic_t *L, sd_step_t *st, int size, int tech)
{
    const sd_shape_t *s = L->s;
    for (int k0 = 0; k0 < s->nunits; k0++) {
        int u = unit_order(s, k0);
        uint8_t cl[SD_MAXN];
        int n = 0;
        for (int k = 0; k < s->unit_len[u]; k++) {
            int c = s->unit_cell[u][k];
            int p = sd_popcount(L->cand[c]);
            if (!L->val[c] && p >= 2 && p <= size) cl[n++] = (uint8_t)c;
        }
        if (n < size) continue;
        int idx[4];
        for (int i = 0; i < size; i++) idx[i] = i;
        for (;;) {
            uint16_t m = 0;
            for (int i = 0; i < size; i++) m |= L->cand[cl[idx[i]]];
            if (sd_popcount(m) == size) {
                step_clear(st, tech);
                for (int k = 0; k < s->unit_len[u]; k++) {
                    int c = s->unit_cell[u][k];
                    bool in = false;
                    for (int i = 0; i < size; i++)
                        if (cl[idx[i]] == c) in = true;
                    if (in || L->val[c]) continue;
                    for (uint16_t t = L->cand[c] & m; t; t &= t - 1) add_elim(st, c, sd_lowest(t));
                }
                if (st->nelim) {
                    st->unit = (uint8_t)u;
                    st->digits = m;
                    for (int i = 0; i < size; i++) add_cell(st, cl[idx[i]]);
                    return true;
                }
            }
            /* next combination */
            int i = size - 1;
            while (i >= 0 && idx[i] == n - size + i) i--;
            if (i < 0) break;
            idx[i]++;
            for (int j = i + 1; j < size; j++) idx[j] = idx[j - 1] + 1;
        }
    }
    return false;
}

static bool hidden_subset(sd_logic_t *L, sd_step_t *st, int size, int tech)
{
    const sd_shape_t *s = L->s;
    for (int k0 = 0; k0 < s->nunits; k0++) {
        int u = unit_order(s, k0);
        uint8_t dl[SD_MAXN];
        uint16_t pos[SD_MAXN + 1];
        int n = 0, empties = 0;
        for (int d = 1; d <= s->n; d++) {
            uint16_t p = 0;
            bool placed = false;
            for (int k = 0; k < s->unit_len[u]; k++) {
                int c = s->unit_cell[u][k];
                if (L->val[c] == d) placed = true;
                else if (!L->val[c] && (L->cand[c] & BIT(d))) p |= (uint16_t)(1u << k);
            }
            pos[d] = p;
            int pc = sd_popcount(p);
            if (!placed && pc >= 2 && pc <= size) dl[n++] = (uint8_t)d;
        }
        for (int k = 0; k < s->unit_len[u]; k++)
            if (!L->val[s->unit_cell[u][k]]) empties++;
        if (n < size || empties <= size) continue;
        int idx[4];
        for (int i = 0; i < size; i++) idx[i] = i;
        for (;;) {
            uint16_t p = 0, dm = 0;
            for (int i = 0; i < size; i++) {
                p |= pos[dl[idx[i]]];
                dm |= BIT(dl[idx[i]]);
            }
            if (sd_popcount(p) == size) {
                step_clear(st, tech);
                for (int k = 0; k < s->unit_len[u]; k++) {
                    if (!(p & (1u << k))) continue;
                    int c = s->unit_cell[u][k];
                    for (uint16_t t = L->cand[c] & (uint16_t)~dm; t; t &= t - 1) add_elim(st, c, sd_lowest(t));
                    add_cell(st, c);
                }
                if (st->nelim) {
                    st->unit = (uint8_t)u;
                    st->digits = dm;
                    return true;
                }
            }
            int i = size - 1;
            while (i >= 0 && idx[i] == n - size + i) i--;
            if (i < 0) break;
            idx[i]++;
            for (int j = i + 1; j < size; j++) idx[j] = idx[j - 1] + 1;
        }
    }
    return false;
}

/* ---- fish ---------------------------------------------------------------------- */

static bool fish(sd_logic_t *L, sd_step_t *st, int size, int tech)
{
    const sd_shape_t *s = L->s;
    int n = s->n;
    for (int dir = 0; dir < 2; dir++) {        /* 0: base rows, 1: base columns */
        for (int d = 1; d <= n; d++) {
            uint8_t bl[SD_MAXN];
            uint16_t cover[SD_MAXN];
            int nb = 0;
            for (int b = 0; b < n; b++) {
                uint16_t m = 0;
                for (int k = 0; k < n; k++) {
                    int c = dir == 0 ? b * n + k : k * n + b;
                    if (!L->val[c] && (L->cand[c] & BIT(d))) m |= (uint16_t)(1u << k);
                }
                int pc = sd_popcount(m);
                if (pc >= 2 && pc <= size) {
                    bl[nb] = (uint8_t)b;
                    cover[nb] = m;
                    nb++;
                }
            }
            if (nb < size) continue;
            int idx[4];
            for (int i = 0; i < size; i++) idx[i] = i;
            for (;;) {
                uint16_t m = 0;
                for (int i = 0; i < size; i++) m |= cover[idx[i]];
                if (sd_popcount(m) == size) {
                    step_clear(st, tech);
                    for (int k = 0; k < n; k++) {
                        if (!(m & (1u << k))) continue;
                        for (int b = 0; b < n; b++) {
                            bool base = false;
                            for (int i = 0; i < size; i++)
                                if (bl[idx[i]] == b) base = true;
                            int c = dir == 0 ? b * n + k : k * n + b;
                            if (L->val[c] || !(L->cand[c] & BIT(d))) continue;
                            if (base) add_cell(st, c);
                            else add_elim(st, c, d);
                        }
                    }
                    if (st->nelim) {
                        st->digits = BIT(d);
                        st->unit = (uint8_t)(dir == 0 ? SD_U_ROW : SD_U_COL);   /* the base's kind */
                        return true;
                    }
                }
                int i = size - 1;
                while (i >= 0 && idx[i] == nb - size + i) i--;
                if (i < 0) break;
                idx[i]++;
                for (int j = i + 1; j < size; j++) idx[j] = idx[j - 1] + 1;
            }
        }
    }
    return false;
}

/* ---- wings ----------------------------------------------------------------------- */

static bool xywing(sd_logic_t *L, sd_step_t *st)
{
    const sd_shape_t *s = L->s;
    for (int p = 0; p < s->ncells; p++) {
        if (L->val[p] || sd_popcount(L->cand[p]) != 2) continue;
        uint16_t pm = L->cand[p];
        for (int i = 0; i < s->npeers[p]; i++) {
            int a = s->peer[p][i];
            uint16_t am = L->cand[a];
            if (L->val[a] || sd_popcount(am) != 2 || sd_popcount(am & pm) != 1) continue;
            for (int j = i + 1; j < s->npeers[p]; j++) {
                int b = s->peer[p][j];
                uint16_t bm = L->cand[b];
                if (L->val[b] || sd_popcount(bm) != 2 || sd_popcount(bm & pm) != 1) continue;
                if ((am & pm) == (bm & pm)) continue;
                uint16_t z = am & bm & (uint16_t)~pm;
                if (sd_popcount(z) != 1 || (am | bm | pm) != (pm | z)) continue;
                int zd = sd_lowest(z);
                step_clear(st, SD_T_XYWING);
                for (int c = 0; c < s->ncells; c++) {
                    if (c == a || c == b || L->val[c] || !(L->cand[c] & z)) continue;
                    if (sd_sees(s, c, a) && sd_sees(s, c, b)) add_elim(st, c, zd);
                }
                if (st->nelim) {
                    add_cell(st, p);
                    add_cell(st, a);
                    add_cell(st, b);
                    st->digits = pm | z;
                    st->digit = (uint8_t)zd;
                    return true;
                }
            }
        }
    }
    return false;
}

static bool xyzwing(sd_logic_t *L, sd_step_t *st)
{
    const sd_shape_t *s = L->s;
    for (int p = 0; p < s->ncells; p++) {
        if (L->val[p] || sd_popcount(L->cand[p]) != 3) continue;
        uint16_t pm = L->cand[p];
        for (int i = 0; i < s->npeers[p]; i++) {
            int a = s->peer[p][i];
            uint16_t am = L->cand[a];
            if (L->val[a] || sd_popcount(am) != 2 || (am & pm) != am) continue;
            for (int j = i + 1; j < s->npeers[p]; j++) {
                int b = s->peer[p][j];
                uint16_t bm = L->cand[b];
                if (L->val[b] || sd_popcount(bm) != 2 || (bm & pm) != bm || am == bm) continue;
                uint16_t z = am & bm;
                if (sd_popcount(z) != 1) continue;
                int zd = sd_lowest(z);
                step_clear(st, SD_T_XYZWING);
                for (int c = 0; c < s->ncells; c++) {
                    if (c == a || c == b || c == p || L->val[c] || !(L->cand[c] & z)) continue;
                    if (sd_sees(s, c, a) && sd_sees(s, c, b) && sd_sees(s, c, p)) add_elim(st, c, zd);
                }
                if (st->nelim) {
                    add_cell(st, p);
                    add_cell(st, a);
                    add_cell(st, b);
                    st->digits = pm;
                    st->digit = (uint8_t)zd;
                    return true;
                }
            }
        }
    }
    return false;
}

/* ---- chains ------------------------------------------------------------------------
 *
 * Alternating inference chains over single candidates. A strong link says
 * "one of the two is true" (the only two places of a digit in a unit, the
 * only two digits of a cell); a weak link "not both" (same cell, or the same
 * digit in cells that see each other). Starting from a candidate assumed
 * false, a strong link makes the next one true, a weak link from a true one
 * makes the next one false, and so on. Wherever it lands TRUE, the start and
 * that end cannot both be false: whatever sees both of them goes. Landing on
 * the start itself, TRUE, proves it.
 *
 * Breadth first, so every chain found is the shortest from its start; of
 * all of them the shortest that eliminates something is the one reported.
 */

#define CH_NODES (SD_MAXC * SD_MAXN)
#define CH_WEAK  48
#define CH_STRONG 12

typedef struct {
    int16_t  id[SD_MAXC][SD_MAXN + 1];
    uint8_t  ncell[CH_NODES], ndig[CH_NODES];
    int      n;
    uint8_t  nweak[CH_NODES], nstrong[CH_NODES];
    int16_t  weak[CH_NODES][CH_WEAK];
    int16_t  strong[CH_NODES][CH_STRONG];
    int16_t  par[CH_NODES][2];
    uint8_t  depth[CH_NODES][2];
    int16_t  queue[CH_NODES * 2];
    uint8_t  mark[CH_NODES];
} chain_ws_t;

static void link_add(int16_t *list, uint8_t *cnt, int max, int to)
{
    for (int k = 0; k < *cnt; k++)
        if (list[k] == to) return;
    if (*cnt < max) list[(*cnt)++] = (int16_t)to;
}

static void chain_build(const sd_logic_t *L, chain_ws_t *w)
{
    const sd_shape_t *s = L->s;
    w->n = 0;
    for (int c = 0; c < s->ncells; c++) {
        for (int d = 0; d <= SD_MAXN; d++) w->id[c][d] = -1;
        if (L->val[c]) continue;
        for (uint16_t m = L->cand[c]; m; m &= m - 1) {
            int d = sd_lowest(m);
            w->id[c][d] = (int16_t)w->n;
            w->ncell[w->n] = (uint8_t)c;
            w->ndig[w->n] = (uint8_t)d;
            w->nweak[w->n] = 0;
            w->nstrong[w->n] = 0;
            w->n++;
        }
    }
    for (int a = 0; a < w->n; a++) {
        int c = w->ncell[a], d = w->ndig[a];
        /* same cell */
        for (uint16_t m = L->cand[c]; m; m &= m - 1) {
            int e = sd_lowest(m);
            if (e != d) link_add(w->weak[a], &w->nweak[a], CH_WEAK, w->id[c][e]);
        }
        if (sd_popcount(L->cand[c]) == 2) {
            uint16_t o = L->cand[c] & (uint16_t)~BIT(d);
            link_add(w->strong[a], &w->nstrong[a], CH_STRONG, w->id[c][sd_lowest(o)]);
        }
        /* same digit, peers */
        for (int k = 0; k < s->npeers[c]; k++) {
            int p = s->peer[c][k];
            if (w->id[p][d] >= 0) link_add(w->weak[a], &w->nweak[a], CH_WEAK, w->id[p][d]);
        }
    }
    /* conjugate pairs */
    for (int u = 0; u < s->nunits; u++) {
        for (int d = 1; d <= s->n; d++) {
            int x = -1, y = -1, cnt = 0;
            for (int k = 0; k < s->unit_len[u]; k++) {
                int c = s->unit_cell[u][k];
                if (w->id[c][d] >= 0) {
                    if (cnt == 0) x = w->id[c][d];
                    else y = w->id[c][d];
                    cnt++;
                }
            }
            if (cnt != 2) continue;
            link_add(w->strong[x], &w->nstrong[x], CH_STRONG, y);
            link_add(w->strong[y], &w->nstrong[y], CH_STRONG, x);
        }
    }
}

static bool chains(sd_logic_t *L, sd_step_t *st, int min_nodes, int max_nodes, int tech)
{
    chain_ws_t *w = (chain_ws_t *)malloc(sizeof(chain_ws_t));
    if (!w) return false;
    chain_build(L, w);
    int best_len = 99;
    sd_step_t best;
    step_clear(&best, tech);

    for (int s0 = 0; s0 < w->n; s0++) {
        if (ABORTED()) break;
        if (!w->nstrong[s0]) continue;
        for (int i = 0; i < w->n; i++) {
            w->depth[i][0] = w->depth[i][1] = 0xFF;
            w->mark[i] = 0;
        }
        for (int k = 0; k < w->nweak[s0]; k++) w->mark[w->weak[s0][k]] = 1;
        int qh = 0, qt = 0;
        /* state 0: false, 1: true; the queue holds node * 2 + state */
        w->depth[s0][0] = 1;
        w->par[s0][0] = -1;
        w->queue[qt++] = (int16_t)(s0 * 2);
        while (qh < qt) {
            int q = w->queue[qh++];
            int a = q >> 1, on = q & 1;
            int dep = w->depth[a][on];
            if (dep >= max_nodes || dep + 1 >= best_len) continue;
            int nl = on ? w->nweak[a] : w->nstrong[a];
            const int16_t *ls = on ? w->weak[a] : w->strong[a];
            for (int k = 0; k < nl; k++) {
                int b = ls[k], bon = !on;
                if (w->depth[b][bon] != 0xFF) continue;
                w->depth[b][bon] = (uint8_t)(dep + 1);
                w->par[b][bon] = (int16_t)q;
                w->queue[qt++] = (int16_t)(b * 2 + bon);
                if (!bon || dep + 1 < min_nodes) continue;
                /* landed TRUE on b: s0 or b */
                sd_step_t cur;
                step_clear(&cur, tech);
                if (b == s0) {
                    cur.cell = (int8_t)w->ncell[s0];
                    cur.digit = w->ndig[s0];
                } else {
                    for (int j = 0; j < w->nweak[b]; j++) {
                        int x = w->weak[b][j];
                        if (x != s0 && w->mark[x]) add_elim(&cur, w->ncell[x], w->ndig[x]);
                    }
                    if (!cur.nelim) continue;
                }
                /* the chain, end to start */
                int len = 0, z = b * 2 + 1;
                uint16_t tmp[SD_STEP_CELLS];
                while (z >= 0 && len < SD_STEP_CELLS) {
                    int nd = z >> 1;
                    tmp[len++] = (uint16_t)(w->ncell[nd] * 16 + w->ndig[nd]);
                    z = w->par[nd][z & 1];
                }
                cur.nchain = (uint8_t)len;
                for (int j = 0; j < len; j++) {
                    cur.chain[j] = tmp[len - 1 - j];
                    add_cell(&cur, cur.chain[j] >> 4);
                }
                cur.digits = BIT(w->ndig[s0]) | BIT(w->ndig[b]);
                if (len < best_len) {
                    best_len = len;
                    best = cur;
                }
            }
        }
    }
    free(w);
    if (best_len == 99) return false;
    *st = best;
    return true;
}

/* ---- the steps, simplest first ------------------------------------------------------ */

bool sd_logic_step(sd_logic_t *L, int max_level, sd_step_t *st)
{
    bool killer = L->s->ncages > 0;
    if (full_house(L, st) || naked_single(L, st) || hidden_single(L, st)) return true;
    if (killer && cage_combo(L, st)) return true;
    if (max_level < SD_L_MEDIUM) return false;
    if (locked(L, st, true) || locked(L, st, false)) return true;
    if (naked_subset(L, st, 2, SD_T_NAKED_PAIR) || hidden_subset(L, st, 2, SD_T_HIDDEN_PAIR)) return true;
    if (killer && innie(L, st)) return true;
    if (max_level < SD_L_HARD) return false;
    if (naked_subset(L, st, 3, SD_T_NAKED_TRIPLE) || hidden_subset(L, st, 3, SD_T_HIDDEN_TRIPLE)) return true;
    if (fish(L, st, 2, SD_T_XWING)) return true;
    if (xywing(L, st) || xyzwing(L, st)) return true;
    if (fish(L, st, 3, SD_T_SWORDFISH)) return true;
    if (max_level < SD_L_EXPERT) return false;
    if (naked_subset(L, st, 4, SD_T_NAKED_QUAD) || hidden_subset(L, st, 4, SD_T_HIDDEN_QUAD)) return true;
    if (fish(L, st, 4, SD_T_JELLYFISH)) return true;
    if (chains(L, st, 4, 8, SD_T_CHAIN_SHORT)) return true;
    if (max_level < SD_L_MASTER) return false;
    if (chains(L, st, 9, SD_STEP_CELLS, SD_T_CHAIN_LONG)) return true;
    return false;
}

void sd_grade(const sd_shape_t *s, const uint8_t *grid, int max_level, sd_grade_t *g)
{
    memset(g, 0, sizeof *g);
    g->level = -1;
    sd_logic_t *L = (sd_logic_t *)malloc(sizeof(sd_logic_t));
    if (!L) return;
    sd_logic_init(L, s, grid);
    sd_step_t st;
    while (!sd_logic_solved(L)) {
        if (ABORTED()) break;
        if (sd_logic_broken(L) || !sd_logic_step(L, max_level, &st)) break;
        int lv = sd_tech_level(st.tech);
        if (lv > g->level) g->level = lv;
        g->used[st.tech]++;
        g->steps++;
        sd_logic_apply(L, &st);
    }
    g->solved = sd_logic_solved(L);
    if (g->level < 0) g->level = 0;
    free(L);
}

/* ==========================================================================
 * The generator
 * ========================================================================== */

static int count_givens(const sd_shape_t *s, const uint8_t *g)
{
    int k = 0;
    for (int i = 0; i < s->ncells; i++)
        if (g[i]) k++;
    return k;
}

/* Digs a full solution down to a minimal puzzle (every clue left is needed
 * for uniqueness), removing cells in symmetric pairs. */
static void dig(const sd_shape_t *s, uint32_t *rng, const uint8_t *full, uint8_t *g, volatile bool *stop)
{
    int nc = s->ncells;
    memcpy(g, full, nc);
    uint8_t order[SD_MAXC];
    for (int i = 0; i < nc; i++) order[i] = (uint8_t)i;
    for (int i = nc - 1; i > 0; i--) {
        int j = (int)(sd_rand(rng) % (uint32_t)(i + 1));
        uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
    }
    for (int k = 0; k < nc; k++) {
        if (stop && *stop) return;
        int a = order[k], b = nc - 1 - a;
        if (!g[a]) continue;
        uint8_t va = g[a], vb = g[b];
        g[a] = 0;
        g[b] = 0;
        if (sd_count(s, g, 2, NULL, 60000) != 1) {
            g[a] = va;
            g[b] = vb;
        }
    }
}

/* Digs while logic up to 'cap' still solves the board: what is left is
 * unique (a logical solution is the only one) and as hard as the cap lets
 * it be. Cheap below the chains, which is where it is used. */
static void dig_cap(const sd_shape_t *s, uint32_t *rng, const uint8_t *full, uint8_t *g, int cap,
                    volatile bool *stop)
{
    int nc = s->ncells;
    memcpy(g, full, nc);
    uint8_t order[SD_MAXC];
    for (int i = 0; i < nc; i++) order[i] = (uint8_t)i;
    for (int i = nc - 1; i > 0; i--) {
        int j = (int)(sd_rand(rng) % (uint32_t)(i + 1));
        uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
    }
    sd_grade_t gr;
    for (int k = 0; k < nc; k++) {
        if (stop && *stop) return;
        int a = order[k], b = nc - 1 - a;
        if (!g[a]) continue;
        uint8_t va = g[a], vb = g[b];
        g[a] = 0;
        g[b] = 0;
        sd_grade(s, g, cap, &gr);
        if (!gr.solved) {
            g[a] = va;
            g[b] = vb;
        }
    }
    /* then single cells: the board loses its symmetry here and there, and
     * gets the last clues out that a pair could not */
    for (int k = 0; k < nc; k++) {
        if (stop && *stop) return;
        int a = order[k];
        if (!g[a]) continue;
        uint8_t va = g[a];
        g[a] = 0;
        sd_grade(s, g, cap, &gr);
        if (!gr.solved) g[a] = va;
    }
}

/* Logic up to 'cap' from 'g'; wherever it gets stuck, a clue from the
 * solution is added, so the result is always solvable within the cap. */
static void fill_in(const sd_shape_t *s, uint32_t *rng, const uint8_t *sol, uint8_t *g, int cap,
                    volatile bool *stop)
{
    sd_logic_t *L = (sd_logic_t *)malloc(sizeof(sd_logic_t));
    if (!L) return;
    sd_logic_init(L, s, g);
    sd_step_t st;
    int guard = 0;
    while (!sd_logic_solved(L) && guard++ < 400) {
        if (stop && *stop) break;
        if (sd_logic_step(L, cap, &st)) {
            sd_logic_apply(L, &st);
            continue;
        }
        /* stuck: one more clue, where there are most candidates */
        int best = -1, bc = -1;
        uint32_t r = sd_rand(rng);
        for (int k = 0; k < s->ncells; k++) {
            int i = (int)((k + r) % s->ncells);
            if (L->val[i]) continue;
            int c = sd_popcount(L->cand[i]);
            if (c > bc) {
                bc = c;
                best = i;
            }
        }
        if (best < 0) break;
        g[best] = sol[best];
        place(L, best, sol[best]);
    }
    free(L);
}

/* More clues, at random, up to 'min': Easy and Medium want a friendly
 * board. A clue that would make the board easier than 'keep' is not added
 * (-1: any will do). */
static void pad_givens(const sd_shape_t *s, uint32_t *rng, const uint8_t *sol, uint8_t *g, int min, int keep)
{
    int guard = 0;
    sd_grade_t gr;
    while (count_givens(s, g) < min && guard++ < 200) {
        int i = (int)(sd_rand(rng) % s->ncells);
        if (g[i]) continue;
        g[i] = sol[i];
        if (keep >= 0) {
            sd_grade(s, g, keep, &gr);
            if (!gr.solved || gr.level < keep) g[i] = 0;
        }
    }
}

/* Killer: cages grown over the solution, no digit twice in one. */
static void make_cages(sd_shape_t *s, uint32_t *rng, const uint8_t *sol, int minsz, int maxsz)
{
    int n = s->n, nc = s->ncells;
    memset(s->cage_of, 0xFF, sizeof s->cage_of);
    s->ncages = 0;
    uint8_t order[SD_MAXC];
    for (int i = 0; i < nc; i++) order[i] = (uint8_t)i;
    for (int i = nc - 1; i > 0; i--) {
        int j = (int)(sd_rand(rng) % (uint32_t)(i + 1));
        uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
    }
    for (int k = 0; k < nc; k++) {
        int start = order[k];
        if (s->cage_of[start] != 0xFF || s->ncages >= SD_MAXCAGES) continue;
        int g = s->ncages++;
        int want = minsz + (int)(sd_rand(rng) % (uint32_t)(maxsz - minsz + 1));
        s->cage_len[g] = 0;
        s->cage_cell[g][s->cage_len[g]++] = (uint8_t)start;
        s->cage_of[start] = (uint8_t)g;
        uint16_t used = BIT(sol[start]);
        while (s->cage_len[g] < want) {
            int opts[SD_MAXC], no = 0;
            for (int q = 0; q < s->cage_len[g]; q++) {
                int c = s->cage_cell[g][q], r = c / n, cc = c % n;
                const int dr[4] = { -1, 1, 0, 0 }, dc[4] = { 0, 0, -1, 1 };
                for (int z = 0; z < 4; z++) {
                    int rr = r + dr[z], c2 = cc + dc[z];
                    if (rr < 0 || rr >= n || c2 < 0 || c2 >= n) continue;
                    int x = rr * n + c2;
                    if (s->cage_of[x] == 0xFF && !(used & BIT(sol[x]))) opts[no++] = x;
                }
            }
            if (!no) break;
            int x = opts[sd_rand(rng) % (uint32_t)no];
            s->cage_cell[g][s->cage_len[g]++] = (uint8_t)x;
            s->cage_of[x] = (uint8_t)g;
            used |= BIT(sol[x]);
        }
    }
    /* a lone cell joins a neighbour's cage when it can: fewer give-aways */
    for (int g = 0; g < s->ncages; g++) {
        if (s->cage_len[g] != 1) continue;
        int c = s->cage_cell[g][0], r = c / n, cc = c % n;
        const int dr[4] = { -1, 1, 0, 0 }, dc[4] = { 0, 0, -1, 1 };
        for (int z = 0; z < 4; z++) {
            int rr = r + dr[z], c2 = cc + dc[z];
            if (rr < 0 || rr >= n || c2 < 0 || c2 >= n) continue;
            int h = s->cage_of[rr * n + c2];
            if (h == g || s->cage_len[h] >= SD_CAGE_MAX) continue;
            bool clash = false;
            for (int q = 0; q < s->cage_len[h]; q++)
                if (sol[s->cage_cell[h][q]] == sol[c]) clash = true;
            if (clash) continue;
            s->cage_cell[h][s->cage_len[h]++] = (uint8_t)c;
            s->cage_of[c] = (uint8_t)h;
            s->cage_len[g] = 0;
            break;
        }
    }
    /* drop the emptied cages, renumbering */
    int w = 0;
    for (int g = 0; g < s->ncages; g++) {
        if (!s->cage_len[g]) continue;
        if (w != g) {
            s->cage_len[w] = s->cage_len[g];
            memcpy(s->cage_cell[w], s->cage_cell[g], SD_CAGE_MAX);
        }
        w++;
    }
    s->ncages = (uint8_t)w;
    for (int g = 0; g < s->ncages; g++) {
        int t = 0;
        for (int q = 0; q < s->cage_len[g]; q++) {
            s->cage_of[s->cage_cell[g][q]] = (uint8_t)g;
            t += sol[s->cage_cell[g][q]];
        }
        s->cage_sum[g] = (uint8_t)t;
    }
    sd_shape_finish(s);
}

bool sd_generate(sd_variant_t v, int level, uint32_t seed, sd_puzzle_t *p, volatile bool *stop)
{
    uint32_t rng = seed * 2654435761u ^ 0x5D0C0FFEu;
    if (!rng) rng = 1;
    for (int k = 0; k < 4; k++) sd_rand(&rng);

    int n = v == SD_V_MINI ? 6 : 9;
    bool diag = v == SD_V_DIAG;
    if (level < 0) level = 0;
    if (level >= SD_LEVELS) level = SD_LEVELS - 1;

    sd_puzzle_t *cand = (sd_puzzle_t *)malloc(sizeof(sd_puzzle_t));
    if (!cand) return false;
    s_abort = stop;
    bool have = false;
    int best_score = 1000;
    int max_tries = v == SD_V_KILLER ? 6 : (level >= SD_L_MASTER ? 400 : level >= SD_L_EXPERT ? 200 : 60);

    for (int t = 1; t <= max_tries; t++) {
        if (stop && *stop) break;
        memset(cand, 0, sizeof *cand);
        sd_shape_init(&cand->shape, n, diag);
        sd_shape_t *s = &cand->shape;
        if (!sd_random_full(s, &rng, cand->sol)) continue;
        int score;

        if (v == SD_V_KILLER) {
            /* Easy: small cages and medium logic; Hard: big cages, all of it */
            static const uint8_t mins[3] = { 2, 2, 2 }, maxs[3] = { 3, 4, 5 }, caps[3] = { 1, 2, 4 };
            int lv = level > 2 ? 2 : level;
            make_cages(s, &rng, cand->sol, mins[lv], maxs[lv]);
            fill_in(s, &rng, cand->sol, cand->given, caps[lv], stop);
            cand->level = lv;
            /* fewer clues reads as a better Killer */
            score = count_givens(s, cand->given);
        } else {
            if (level <= SD_L_HARD) dig_cap(s, &rng, cand->sol, cand->given, level, stop);
            else dig(s, &rng, cand->sol, cand->given, stop);
            if (stop && *stop) break;
            sd_grade_t g;
            sd_grade(s, cand->given, level, &g);
            if (!g.solved) {
                fill_in(s, &rng, cand->sol, cand->given, level, stop);
                sd_grade(s, cand->given, level, &g);
            }
            if (g.solved && g.level == level && level <= SD_L_MEDIUM) {
                pad_givens(s, &rng, cand->sol, cand->given, level == SD_L_EASY ? (n == 9 ? 36 : 16) : (n == 9 ? 28 : 13),
                           level == SD_L_EASY ? -1 : level);
                sd_grade(s, cand->given, level, &g);
            }
            cand->level = g.solved ? g.level : SD_L_MASTER;
            /* exactly the level asked is perfect; below it, the nearer the better */
            score = g.solved ? (level - g.level) * 10 : 500;
        }
        cand->tries = t;
        if (score < best_score) {
            best_score = score;
            *p = *cand;
            have = true;
        }
        if (v == SD_V_KILLER ? score <= (level >= 2 ? 2 : 4) : score == 0) break;
    }
    free(cand);
    bool aborted = ABORTED();
    s_abort = NULL;
    return have && !aborted;
}
