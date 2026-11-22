/*
 * METEGOL - the computer plays itself, to tune the physics and the four
 * difficulties
 *
 *     tools/harness.sh                  every difficulty against every other
 *     tools/harness.sh -a 0 -b 3 -n 50  easy against crack, fifty matches
 *     tools/harness.sh -g 5             to five goals (default: two halves of 3 min)
 *     tools/harness.sh -v               a line per match
 *     tools/harness.sh -p               a person on the phone against each level
 *
 * It links the match itself (main/mg_game.c, mg_ai.c) and plays it in
 * steps of 1/240 s with no screen. What it prints, per pairing: how many
 * the first side won, the goals a match, the kicks, the balls that sat
 * (still for 3 s: a "stuck" ball) and the ones served again (out of reach
 * or held too long), and the fastest ball. The replay is skipped.
 */
#include "mg_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int verbose, minutes = 3, goals = 0, nmatch = 20;

/* A person with a phone, roughly: one finger moves every rod at once, so
 * only the rod nearest the ball is ever aimed (late, and a little off),
 * and a tap kicks with it when the ball looks near; no aim, no pull shots.
 * The levels are tuned against this. */
typedef struct {
    float lag_x[64], lag_y[64];
    int   at;
    float err, err_v, cool;
    uint32_t rnd;
} person_t;

static float prnd(person_t *p)
{
    p->rnd = p->rnd * 1664525u + 1013904223u;
    return (float)(p->rnd >> 8) / 16777216.0f;
}

static void person_hand(mg_game_t *g, int side, person_t *p, mg_hand_t *h)
{
    memset(h, 0, sizeof *h);
    h->all = true;
    h->kick_rod = -1;
    h->speed = 2200.0f;
    /* it sees the ball a quarter of a second late */
    p->at = (p->at + 1) % 64;
    p->lag_x[p->at] = g->bx;
    p->lag_y[p->at] = g->by;
    int old = (p->at + 64 - 60) % 64;
    float bx = p->lag_x[old], by = p->lag_y[old];
    p->err_v += (prnd(p) - 0.5f) * 400.0f * MG_DT - p->err_v * 2.0f * MG_DT;
    p->err += p->err_v * MG_DT;
    if (p->err > 30) p->err = 30;
    if (p->err < -30) p->err = -30;
    int k = mg_nearest_rod(g, side);
    const mg_rod_t *r = &g->rod[mg_rod_ix(side, k)];
    int f = mg_nearest_fig(r, by);
    float want = by + p->err - (r->base[f] + r->off);
    float m = 1400.0f * MG_DT;
    h->move = want > m ? m : want < -m ? -m : want;
    if (p->cool > 0) p->cool -= MG_DT;
    float dir = side ? -1.0f : 1.0f;
    float rel = (bx - r->x) * dir;
    float fy = r->base[f] + r->off;
    if (p->cool <= 0 && rel > -40.0f && rel < 70.0f && (by - fy) * (by - fy) < 30.0f * 30.0f && prnd(p) < 0.08f) {
        h->kick_rod = k;
        h->kick_power = 0.85f;
        p->cool = 0.35f;
    }
}

typedef struct {
    int won, lost, draw, gf, ga, kicks, stuck, deads;
    double secs;
    float vmax;
} tally_t;

static void play(int d0, int d1, uint32_t seed, tally_t *t)
{
    static mg_game_t g;
    memset(&g, 0, sizeof g);
    g.rnd = seed;
    g.ball_speed = 1.0f;
    bool person = d0 < 0;
    static person_t pp;
    memset(&pp, 0, sizeof pp);
    pp.rnd = seed * 31u + 7u;
    mg_game_start(&g, goals ? 0 : minutes, goals, false, !person, person ? 0 : d0, true, d1);
    long steps = 0, cap = (long)MG_HZ * 60 * 40;
    while (!mg_over(&g) && steps < cap) {
        /* the app would show the replay; here it is skipped */
        if (g.state == MG_ST_REPLAY) mg_replay_end(&g);
        if (person) {
            mg_hand_t h;
            person_hand(&g, 0, &pp, &h);
            mg_game_step(&g, &h, NULL);
        } else {
            mg_game_step(&g, NULL, NULL);
        }
        steps++;
        g.nsnd = 0;
    }
    int w = mg_winner(&g);
    if (w == 0) t->won++;
    else if (w == 1) t->lost++;
    else t->draw++;
    t->gf += g.score[0];
    t->ga += g.score[1];
    t->kicks += g.kicks;
    t->stuck += g.stuck;
    t->deads += g.deads;
    t->secs += (double)steps / MG_HZ;
    if (g.max_speed > t->vmax) t->vmax = g.max_speed;
    if (verbose)
        printf("    %d-%d  %4.0f s  kicks %d  stuck %d  served again %d  vmax %.0f mm/s\n", g.score[0], g.score[1],
               (double)steps / MG_HZ, g.kicks, g.stuck, g.deads, (double)g.max_speed);
}

static const char *const NAME_[MG_DIFF_N + 1] = { "person", "easy", "normal", "hard", "crack" };
#define NAME (NAME_ + 1)

static void pairing(int a, int b)
{
    tally_t t;
    memset(&t, 0, sizeof t);
    for (int i = 0; i < nmatch; i++) play(a, b, 0x1234u + (uint32_t)i * 7919u + (uint32_t)(a * 31 + b) * 104729u, &t);
    double n = nmatch;
    printf("%-6s vs %-6s  won %2d drew %2d lost %2d   goals %.1f-%.1f  %.0f s  kicks %.0f  stuck %.1f  served again %.1f"
           "  vmax %.0f\n",
           NAME[a], NAME[b], t.won, t.draw, t.lost, t.gf / n, t.ga / n, t.secs / n, t.kicks / n, t.stuck / n,
           t.deads / n, (double)t.vmax);
}

int main(int argc, char **argv)
{
    int a = -1, b = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) a = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) b = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) nmatch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) minutes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) goals = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-p")) a = -1, b = -2;
    }
    if (b == -2) {
        printf("%d matches a pairing, %s\n", nmatch, goals ? "to N goals" : "two halves");
        for (int j = 0; j < MG_DIFF_N; j++) pairing(-1, j);
        return 0;
    }
    printf("%d matches a pairing, %s\n", nmatch, goals ? "to N goals" : "two halves");
    if (a >= 0 && b >= 0) {
        pairing(a, b);
        return 0;
    }
    for (int i = 0; i < MG_DIFF_N; i++) {
        for (int j = 0; j < MG_DIFF_N; j++) pairing(i, j);
    }
    return 0;
}
