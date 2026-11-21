/*
 * Juegos de mesa - chess: the engine.
 *
 * Iterative deepening over a principal variation search: a transposition
 * table, quiescence on captures and promotions, the table's move first,
 * captures by the most valuable victim, two killers a ply, history, a null
 * move and late moves searched shallower; checks are searched one ply
 * deeper. The evaluation is material and squares in two phases (the middle
 * game's and the ending's, blended by what is left on the board), pawns
 * (doubled, isolated, passed), the bishops' pair, rooks on open files,
 * mobility, the pawns in front of the king, and in a bare-king ending the
 * loser's king pushed to the edge.
 *
 * The weaker levels see less deep and add noise to the root's moves: every
 * root move gets an exact score and a random number of up to 'noise'
 * centipawns either way, and the best of the sums is played. So a weak
 * level plays a plausible move a bit worse than the best, not a random
 * one, and still takes a queen left hanging.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "g_ajedrez.h"

#include "mesa_game.h"

#include <stdlib.h>
#include <string.h>

#define INF      32000
#define MATE     30000
#define MATE_IN  29000          /* above this, a mate in so many plies */
#define MAX_PLY  64
#define TT_BITS  17

extern const mesa_host_t *mesa_host;

typedef struct {
    uint32_t check;
    uint16_t mv;
    int16_t  score;
    int8_t   depth;
    uint8_t  flag;
    uint16_t pad;
} tt_t;

enum { TT_EXACT = 1, TT_LOWER = 2, TT_UPPER = 3 };

static tt_t *s_tt;

static const int VAL_MG[7] = { 0, 100, 320, 330, 500, 950, 0 };
static const int VAL_EG[7] = { 0, 120, 300, 320, 550, 1000, 0 };
static int16_t PMG[7][64], PEG[7][64];
static bool s_pst;

static int cen(int f, int r)
{
    /* 0 in a corner .. 6 in the middle */
    int df = f < 4 ? f : 7 - f, dr = r < 4 ? r : 7 - r;
    return df + dr;
}

static void pst_init(void)
{
    if (s_pst) return;
    static const int PAWN_MG[8] = { 0, 0, 4, 10, 20, 32, 55, 0 };
    static const int PAWN_EG[8] = { 0, 4, 10, 20, 36, 60, 95, 0 };
    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            int i = r * 8 + f, c = cen(f, r);
            int fc = f < 4 ? f : 7 - f;                   /* 0 on the rim .. 3 in the middle files */
            /* pawns: forward, more so in the middle files; the e and d pawns
             * left at home cost a little */
            PMG[CH_P][i] = (int16_t)(PAWN_MG[r] + (r >= 2 && r <= 4 ? fc * 4 : 0) - (r == 1 && fc == 3 ? 10 : 0));
            PEG[CH_P][i] = (int16_t)PAWN_EG[r];
            /* knights: the middle, never the rim */
            PMG[CH_N][i] = (int16_t)((c - 3) * 10 - (f == 0 || f == 7 ? 10 : 0) - (r == 0 ? 8 : 0));
            PEG[CH_N][i] = (int16_t)((c - 3) * 8);
            /* bishops: out and towards the middle, the long diagonals */
            PMG[CH_B][i] = (int16_t)((c - 3) * 4 + (f == r || f == 7 - r ? 8 : 0) - (r == 0 ? 10 : 0));
            PEG[CH_B][i] = (int16_t)((c - 3) * 4);
            /* rooks: the seventh rank, the middle files */
            PMG[CH_R][i] = (int16_t)((r == 6 ? 20 : 0) + (fc >= 2 && r == 0 ? 5 : 0) - (f == 0 || f == 7 ? 3 : 0));
            PEG[CH_R][i] = (int16_t)(r == 6 ? 12 : 0);
            /* the queen: a little of the middle; not out too early is the
             * null move's business */
            PMG[CH_Q][i] = (int16_t)((c - 3) * 2 - (r == 0 ? 2 : 0));
            PEG[CH_Q][i] = (int16_t)((c - 3) * 6);
            /* the king: tucked away in the game, in the middle in the ending */
            int km = r == 0 ? (f == 6 || f == 1 ? 30 : f == 2 || f == 7 || f == 0 ? 15 : f == 4 ? 0 : -10)
                   : r == 1 ? -15 : -40 - r * 5;
            PMG[CH_K][i] = (int16_t)km;
            PEG[CH_K][i] = (int16_t)((c - 3) * 12);
        }
    }
    s_pst = true;
}

static const int8_t KN[8] = { 33, 31, 18, 14, -33, -31, -18, -14 };
static const int8_t DIRS[8] = { 17, 15, -15, -17, 16, -16, 1, -1 };

int mch_eval(const ch_pos_t *p)
{
    pst_init();
    int mg[2] = { 0, 0 }, eg[2] = { 0, 0 };
    int pawns[2][10];       /* per file, with a file of nothing on each side */
    int wmin[10], bmax[10];
    int bishops[2] = { 0, 0 };
    memset(pawns, 0, sizeof pawns);
    for (int f = 0; f < 10; f++) {
        wmin[f] = 8;
        bmax[f] = -1;
    }
    const uint8_t *b = p->sq;
    for (int s = 0; s < 128; s++) {
        if (s & 0x88) {
            s += 7;
            continue;
        }
        int pc = b[s];
        if (!pc) continue;
        int t = CH_TYPE(pc), c = CH_COLOR(pc), f = s & 7, r = s >> 4;
        int i = c ? ((7 - r) * 8 + f) : (r * 8 + f);
        mg[c] += VAL_MG[t] + PMG[t][i];
        eg[c] += VAL_EG[t] + PEG[t][i];
        if (t == CH_P) {
            pawns[c][f + 1]++;
            if (c == 0 && r < wmin[f + 1]) wmin[f + 1] = r;
            if (c == 1 && r > bmax[f + 1]) bmax[f + 1] = r;
        } else if (t == CH_B) {
            bishops[c]++;
        }
        if (t >= CH_N && t <= CH_Q) {
            /* mobility: the squares it reaches that its own side does not hold */
            int m = 0, own = c ? CH_BLACK : 0;
            if (t == CH_N) {
                for (int k = 0; k < 8; k++) {
                    int q = s + KN[k];
                    if (!(q & 0x88) && (!b[q] || (b[q] & CH_BLACK) != own)) m++;
                }
                mg[c] += (m - 4) * 4;
                eg[c] += (m - 4) * 4;
            } else {
                int k0 = t == CH_R ? 4 : 0, k1 = t == CH_B ? 4 : 8;
                for (int k = k0; k < k1; k++) {
                    for (int q = s + DIRS[k]; !(q & 0x88); q += DIRS[k]) {
                        if (!b[q]) {
                            m++;
                            continue;
                        }
                        if ((b[q] & CH_BLACK) != own) m++;
                        break;
                    }
                }
                int base = t == CH_B ? 6 : t == CH_R ? 7 : 13;
                int wm = t == CH_Q ? 1 : 3, we = t == CH_Q ? 2 : 4;
                mg[c] += (m - base) * wm;
                eg[c] += (m - base) * we;
            }
        }
    }
    /* pawns, rooks on files */
    static const int PASS_MG[8] = { 0, 5, 8, 14, 24, 40, 62, 0 };
    static const int PASS_EG[8] = { 0, 10, 18, 32, 56, 90, 135, 0 };
    for (int s = 0; s < 128; s++) {
        if (s & 0x88) {
            s += 7;
            continue;
        }
        int pc = b[s];
        if (!pc) continue;
        int t = CH_TYPE(pc), c = CH_COLOR(pc), f = (s & 7) + 1, r = s >> 4;
        if (t == CH_P) {
            if (!pawns[c][f - 1] && !pawns[c][f + 1]) {
                mg[c] -= 12;
                eg[c] -= 16;
            }
            bool passed;
            int rr;
            if (c == 0) {
                passed = bmax[f - 1] <= r && bmax[f] <= r && bmax[f + 1] <= r;
                rr = r;
            } else {
                passed = wmin[f - 1] >= r && wmin[f] >= r && wmin[f + 1] >= r;
                rr = 7 - r;
            }
            if (passed) {
                mg[c] += PASS_MG[rr];
                eg[c] += PASS_EG[rr];
            }
        } else if (t == CH_R) {
            if (!pawns[c][f]) {
                int k = pawns[c ^ 1][f] ? 10 : 22;
                mg[c] += k;
                eg[c] += k / 2;
            }
        }
    }
    for (int c = 0; c < 2; c++) {
        for (int f = 1; f <= 8; f++) {
            if (pawns[c][f] > 1) {
                mg[c] -= 10 * (pawns[c][f] - 1);
                eg[c] -= 18 * (pawns[c][f] - 1);
            }
        }
        if (bishops[c] >= 2) {
            mg[c] += 28;
            eg[c] += 45;
        }
        /* the king's shield */
        int k = p->king[c], kf = k & 7, kr = k >> 4, sh = 0;
        int up = c ? -16 : 16;
        if ((c == 0 && kr <= 1) || (c == 1 && kr >= 6)) {
            for (int df = -1; df <= 1; df++) {
                int ff = kf + df;
                if (ff < 0 || ff > 7) continue;
                int q1 = k + up + df, q2 = k + 2 * up + df;
                if (!(q1 & 0x88) && b[q1] == (CH_P | (c ? CH_BLACK : 0))) sh += 12;
                else if (!(q2 & 0x88) && b[q2] == (CH_P | (c ? CH_BLACK : 0))) sh += 6;
                else sh -= 8;
            }
        }
        mg[c] += sh;
    }
    int phase = p->mat[0] + p->mat[1];
    if (phase > 24) phase = 24;
    int score = ((mg[0] - mg[1]) * phase + (eg[0] - eg[1]) * (24 - phase)) / 24;
    /* a bare king: to the edge, and the other king near it */
    for (int c = 0; c < 2; c++) {
        int lose = c ^ 1;
        if (p->mat[lose] || pawns[lose][1] + pawns[lose][2] + pawns[lose][3] + pawns[lose][4] + pawns[lose][5] +
                            pawns[lose][6] + pawns[lose][7] + pawns[lose][8])
            continue;
        if (p->mat[c] < 2) continue;
        int lk = p->king[lose], wk = p->king[c];
        int edge = 6 - cen(lk & 7, lk >> 4);
        int df = (lk & 7) - (wk & 7), dr = (lk >> 4) - (wk >> 4);
        if (df < 0) df = -df;
        if (dr < 0) dr = -dr;
        int near = 14 - (df + dr);
        int bonus = edge * 20 + near * 8;
        score += c == 0 ? bonus : -bonus;
    }
    score += p->side ? -10 : 10;
    return p->side ? -score : score;
}

/* ---- the search ------------------------------------------------------------------ */

typedef struct {
    ch_think_t *t;
    bool        abort;
    uint32_t    killer[MAX_PLY][2];
    int32_t     hist[16][128];
    uint32_t    nodes;
} sr_t;

bool mch_search_init(void)
{
    mch_tables();
    pst_init();
    if (s_tt) return true;
    size_t n = sizeof(tt_t) << TT_BITS;
    s_tt = (tt_t *)(mesa_host && mesa_host->alloc ? mesa_host->alloc(n) : malloc(n));
    if (!s_tt) return false;
    memset(s_tt, 0, n);
    return true;
}

void mch_search_free(void)
{
    if (!s_tt) return;
    if (mesa_host && mesa_host->free) mesa_host->free(s_tt);
    else free(s_tt);
    s_tt = NULL;
}

static uint16_t mv16(uint32_t m)
{
    int pr = CHMV_PROMO(m);
    return (uint16_t)(CH_SQ64(CHMV_FROM(m)) | (CH_SQ64(CHMV_TO(m)) << 6) | ((pr ? pr - 1 : 0) << 12) |
                      (pr ? 1u << 15 : 0));
}

static bool timeout(sr_t *s)
{
    if (s->abort) return true;
    if ((s->nodes & 1023) == 0) {
        ch_think_t *t = s->t;
        if ((t->stop && *t->stop) || (t->ms && t->now && t->now() - t->t0 >= t->ms)) s->abort = true;
    }
    return s->abort;
}

static int mvv(const ch_pos_t *p, uint32_t m)
{
    static const int V[7] = { 0, 1, 3, 3, 5, 9, 20 };
    int victim = CHMV_FL(m) & CHM_EP ? CH_P : CH_TYPE(p->sq[CHMV_TO(m)]);
    int attacker = CH_TYPE(p->sq[CHMV_FROM(m)]);
    return V[victim] * 16 - V[attacker] + (CHMV_FL(m) & CHM_PROMO ? CHMV_PROMO(m) * 8 : 0);
}

static void order(sr_t *s, const ch_pos_t *p, uint32_t *ms, int *sc, int n, uint16_t ttm, int ply)
{
    for (int i = 0; i < n; i++) {
        uint32_t m = ms[i];
        int fl = CHMV_FL(m);
        if (ttm && mv16(m) == ttm) sc[i] = 1 << 30;
        else if (fl & (CHM_CAP | CHM_PROMO)) sc[i] = (1 << 28) + mvv(p, m);
        else if (ply < MAX_PLY && m == s->killer[ply][0]) sc[i] = (1 << 27) + 2;
        else if (ply < MAX_PLY && m == s->killer[ply][1]) sc[i] = (1 << 27) + 1;
        else sc[i] = s->hist[p->sq[CHMV_FROM(m)] & 15][CHMV_TO(m)];
    }
}

static inline uint32_t pick(uint32_t *ms, int *sc, int n, int i)
{
    int best = i;
    for (int j = i + 1; j < n; j++) {
        if (sc[j] > sc[best]) best = j;
    }
    if (best != i) {
        uint32_t m = ms[i];
        ms[i] = ms[best];
        ms[best] = m;
        int k = sc[i];
        sc[i] = sc[best];
        sc[best] = k;
    }
    return ms[i];
}

static int qsearch(sr_t *s, ch_pos_t *p, int alpha, int beta, int ply)
{
    s->nodes++;
    if (timeout(s)) return 0;
    int stand = mch_eval(p);
    if (stand >= beta) return stand;
    if (stand > alpha) alpha = stand;
    if (ply >= MAX_PLY - 1) return stand;
    uint32_t ms[128];
    int sc[128];
    int n = mch_gen(p, ms, true);
    order(s, p, ms, sc, n, 0, MAX_PLY);
    for (int i = 0; i < n; i++) {
        uint32_t m = pick(ms, sc, n, i);
        /* hopeless captures: even taking it would not reach alpha */
        if (!(CHMV_FL(m) & CHM_PROMO)) {
            int victim = CHMV_FL(m) & CHM_EP ? CH_P : CH_TYPE(p->sq[CHMV_TO(m)]);
            if (stand + VAL_EG[victim] + 200 < alpha) continue;
        }
        ch_undo_t u;
        if (!mch_make(p, m, &u)) continue;
        int v = -qsearch(s, p, -beta, -alpha, ply + 1);
        mch_unmake(p, m, &u);
        if (s->abort) return 0;
        if (v >= beta) return v;
        if (v > alpha) alpha = v;
    }
    return alpha;
}

static int search(sr_t *s, ch_pos_t *p, int depth, int alpha, int beta, int ply, bool null_ok)
{
    bool pv = beta - alpha > 1;
    if (ply > 0) {
        if (p->half >= 100 || mch_repeats(p) >= 1 || mch_no_mating_material(p)) return 0;
        /* mates further than one already found cannot be better */
        int ma = -MATE + ply, mb = MATE - ply - 1;
        if (alpha < ma) alpha = ma;
        if (beta > mb) beta = mb;
        if (alpha >= beta) return alpha;
    }
    bool check = mch_in_check(p, p->side);
    if (check) depth++;
    if (depth <= 0) return qsearch(s, p, alpha, beta, ply);
    s->nodes++;
    if (timeout(s)) return 0;
    if (ply >= MAX_PLY - 1) return mch_eval(p);

    tt_t *e = &s_tt[(uint32_t)p->key & ((1u << TT_BITS) - 1)];
    uint32_t chk = (uint32_t)(p->key >> 32);
    uint16_t ttm = 0;
    if (e->check == chk) {
        ttm = e->mv;
        if (!pv && e->depth >= depth) {
            int v = e->score;
            if (v > MATE_IN) v -= ply;
            else if (v < -MATE_IN) v += ply;
            if (e->flag == TT_EXACT) return v;
            if (e->flag == TT_LOWER && v >= beta) return v;
            if (e->flag == TT_UPPER && v <= alpha) return v;
        }
    }

    int stat = check ? -INF : mch_eval(p);
    /* futility at the frontier: a quiet position far below alpha */
    if (!pv && !check && depth <= 2 && stat - 120 * depth >= beta && beta < MATE_IN) return stat;
    if (null_ok && !pv && !check && depth >= 3 && stat >= beta && p->mat[p->side] > 0) {
        ch_undo_t u;
        mch_null(p, &u);
        int R = depth >= 7 ? 3 : 2;
        int v = -search(s, p, depth - 1 - R, -beta, -beta + 1, ply + 1, false);
        mch_unnull(p, &u);
        if (s->abort) return 0;
        if (v >= beta) return v >= MATE_IN ? beta : v;
    }

    uint32_t ms[256];
    int sc[256];
    int n = mch_gen(p, ms, false);
    order(s, p, ms, sc, n, ttm, ply);
    int best = -INF, legal = 0, a0 = alpha;
    uint32_t bestm = 0;
    for (int i = 0; i < n; i++) {
        uint32_t m = pick(ms, sc, n, i);
        ch_undo_t u;
        if (!mch_make(p, m, &u)) continue;
        legal++;
        bool quiet = !(CHMV_FL(m) & (CHM_CAP | CHM_PROMO));
        int v;
        if (legal == 1) {
            v = -search(s, p, depth - 1, -beta, -alpha, ply + 1, true);
        } else {
            int red = 0;
            if (quiet && !check && depth >= 3 && legal > 3 && !mch_in_check(p, p->side)) {
                red = legal > 8 ? 2 : 1;
                if (pv) red--;
            }
            v = -search(s, p, depth - 1 - red, -alpha - 1, -alpha, ply + 1, true);
            if (v > alpha && red) v = -search(s, p, depth - 1, -alpha - 1, -alpha, ply + 1, true);
            if (v > alpha && v < beta) v = -search(s, p, depth - 1, -beta, -alpha, ply + 1, true);
        }
        mch_unmake(p, m, &u);
        if (s->abort) return 0;
        if (v > best) {
            best = v;
            bestm = m;
        }
        if (v > alpha) {
            alpha = v;
            if (v >= beta) {
                if (quiet && ply < MAX_PLY) {
                    if (s->killer[ply][0] != m) {
                        s->killer[ply][1] = s->killer[ply][0];
                        s->killer[ply][0] = m;
                    }
                    int32_t *h = &s->hist[p->sq[CHMV_FROM(m)] & 15][CHMV_TO(m)];
                    *h += depth * depth;
                    if (*h > (1 << 26)) {
                        for (int a = 0; a < 16; a++) {
                            for (int b = 0; b < 128; b++) s->hist[a][b] >>= 1;
                        }
                    }
                }
                break;
            }
        }
    }
    if (!legal) return check ? -MATE + ply : 0;
    int sv = best;
    if (sv > MATE_IN) sv += ply;
    else if (sv < -MATE_IN) sv -= ply;
    e->check = chk;
    e->mv = mv16(bestm);
    e->score = (int16_t)sv;
    e->depth = (int8_t)depth;
    e->flag = (uint8_t)(best >= beta ? TT_LOWER : best > a0 ? TT_EXACT : TT_UPPER);
    return best;
}

static uint32_t rnd(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x2545F491u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

uint32_t mch_think(ch_pos_t *p, ch_think_t *t)
{
    mch_search_init();
    sr_t *s = (sr_t *)(mesa_host && mesa_host->alloc ? mesa_host->alloc(sizeof *s) : malloc(sizeof *s));
    if (!s) return 0;
    memset(s, 0, sizeof *s);
    s->t = t;
    if (t->now) t->t0 = t->now();
    uint32_t ms[256];
    int nm = mch_legal(p, ms);
    if (!nm) {
        if (mesa_host && mesa_host->free) mesa_host->free(s);
        else free(s);
        return 0;
    }
    int noise[256];
    uint32_t seed = t->seed;
    for (int i = 0; i < nm; i++) noise[i] = t->noise ? (int)(rnd(&seed) % (uint32_t)(2 * t->noise + 1)) - t->noise : 0;
    int score[256];
    uint32_t best = ms[0];
    int best_score = -INF;
    int maxd = t->max_depth > 0 ? t->max_depth : MAX_PLY - 4;
    for (int d = 1; d <= maxd; d++) {
        int alpha = -INF, dbest = -1, dscore = -INF;
        for (int i = 0; i < nm; i++) {
            ch_undo_t u;
            mch_make(p, ms[i], &u);
            int v;
            if (t->noise) {
                /* every root move's exact value */
                v = -search(s, p, d - 1, -INF, INF, 1, true);
            } else if (i == 0) {
                v = -search(s, p, d - 1, -INF, -alpha, 1, true);
            } else {
                v = -search(s, p, d - 1, -alpha - 1, -alpha, 1, true);
                if (v > alpha && !s->abort) v = -search(s, p, d - 1, -INF, -alpha, 1, true);
            }
            mch_unmake(p, ms[i], &u);
            if (s->abort) break;
            score[i] = v;
            int k = v + (v > -MATE_IN && v < MATE_IN ? noise[i] : 0);
            if (k > dscore || dbest < 0) {
                dscore = k;
                dbest = i;
            }
            if (v > alpha) alpha = v;
        }
        if (dbest >= 0 && (!s->abort || dbest == 0 || dscore > best_score)) {
            /* the best of this depth goes first next time */
            uint32_t m = ms[dbest];
            int sm = score[dbest], nz = noise[dbest];
            memmove(ms + 1, ms, sizeof ms[0] * (size_t)dbest);
            memmove(score + 1, score, sizeof score[0] * (size_t)dbest);
            memmove(noise + 1, noise, sizeof noise[0] * (size_t)dbest);
            ms[0] = m;
            score[0] = sm;
            noise[0] = nz;
            best = m;
            best_score = sm;
            if (!s->abort) t->depth_done = d;
        }
        if (s->abort) break;
        /* a forced mate found: no deeper */
        if (best_score > MATE_IN || best_score < -MATE_IN) break;
        if (nm == 1 && d >= 2) break;
        /* half the time gone: another iteration would not finish */
        if (t->ms && t->now && (t->now() - t->t0) * 2 > t->ms && d >= 4) break;
    }
    t->nodes = s->nodes;
    t->score = best_score;
    if (mesa_host && mesa_host->free) mesa_host->free(s);
    else free(s);
    return best;
}
