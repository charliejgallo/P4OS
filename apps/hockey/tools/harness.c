/*
 * HOCKEY - the computer plays itself, to tune the physics and the four
 * difficulties
 *
 *     tools/harness.sh                  every difficulty against every other
 *     tools/harness.sh -a 0 -b 3 -n 50  easy against crack, fifty matches
 *     tools/harness.sh -g 7             to seven goals (the default)
 *     tools/harness.sh -t 3             three minutes instead
 *     tools/harness.sh -p               a person with a finger against each level
 *     tools/harness.sh -m 33            the small mallet (40 the middle one, 49 the big one)
 *     tools/harness.sh -s 1.25          the fast puck
 *     tools/harness.sh -v               a line per match
 *
 * It links the match itself (main/hk_game.c, hk_ai.c) and plays it in
 * steps of 1/240 s with no screen. What it prints, per pairing: how many
 * the first side won, the goals a match, the seconds between the second
 * side's goals (how often the board scores on the person), the hard shots,
 * the pucks that sat still and were served again, the fastest puck, and
 * the times a puck went through a mallet (it must be 0). The replay is
 * skipped.
 */
#include "hk_game.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int verbose, minutes = 0, goals = 7, nmatch = 20;
static float mallet_r = HK_MALLET_M, puck_speed = 1.0f;

/* A person with a finger on the phone, roughly: they see the puck a fifth
 * of a second late and do not read bounces; they keep the mallet in front
 * of their goal, under the puck's x, and when the puck is in their half and
 * not shooting at them they go straight at it and swipe it towards the
 * middle of the far goal, a little off. The finger is quick but not a
 * servo: it eases into where it goes. */
typedef struct {
    float lag_x[96], lag_y[96], lag_vx[96], lag_vy[96];
    int   at;
    float err, err_v, cool, swipe_t;
    float fx, fy, fvx, fvy;     /* the finger */
    float aim;
    int   mode;                 /* 0 keeping, 1 swiping */
    uint32_t rnd;
} person_t;

static float prnd(person_t *p)
{
    p->rnd = p->rnd * 1664525u + 1013904223u;
    return (float)(p->rnd >> 8) / 16777216.0f;
}

static void person_hand(hk_game_t *g, int side, person_t *p, hk_hand_t *h)
{
    float d = hk_dir(side);
    p->at = (p->at + 1) % 96;
    p->lag_x[p->at] = g->px;
    p->lag_y[p->at] = g->py * d;
    p->lag_vx[p->at] = g->pvx;
    p->lag_vy[p->at] = g->pvy * d;
    int old = (p->at + 96 - 46) % 96;       /* 0.19 s */
    float bx = p->lag_x[old], by = p->lag_y[old], bvx = p->lag_vx[old], bvy = p->lag_vy[old];
    /* the eye carries a moving puck on a little, rails included */
    float lim = HK_FX - HK_PUCK_R;
    bx += bvx * 0.10f;
    by += bvy * 0.10f;
    if (bx > lim) bx = 2.0f * lim - bx;
    if (bx < -lim) bx = -2.0f * lim - bx;
    p->err_v += (prnd(p) - 0.5f) * 500.0f * HK_DT - p->err_v * 2.5f * HK_DT;
    p->err = fmaxf(-25.0f, fminf(25.0f, p->err + p->err_v * HK_DT));
    if (p->cool > 0) p->cool -= HK_DT;
    const hk_mallet_t *m = &g->m[side];
    float R = m->r + HK_PUCK_R;
    float tx, ty;
    bool mine = by > 10.0f && g->state != HK_ST_GOAL;
    bool deep = by > HK_FY - 110.0f;
    float mx = m->x, my = m->y * d;
    if (p->mode == 0 && mine && bvy < 650.0f && by > 80.0f && p->cool <= 0 && (!deep || my > by + R * 0.5f)) {
        p->mode = 1;
        p->swipe_t = 0;
        p->aim = (prnd(p) - 0.5f) * 200.0f;
    }
    if (p->mode == 1) {
        p->swipe_t += HK_DT;
        float ux = p->aim - bx, uy = -HK_FY - by, ul = sqrtf(ux * ux + uy * uy);
        ux /= ul;
        uy /= ul;
        /* through the puck, from where the mallet is */
        tx = bx + ux * 140.0f + p->err;
        ty = by + uy * 140.0f;
        if ((mx - bx) * ux + (my - by) * uy > -R * 0.2f) {
            /* the mallet is level with the puck or past it: give it up and cover the goal */
            p->swipe_t = 1.0f;
        }
        if (m->hit_t < 0.02f || p->swipe_t > 0.45f || !mine) {
            p->mode = 0;
            p->cool = 0.35f;
        }
    }
    if (p->mode == 0) {
        float line = HK_HOME_Y - 30.0f;
        if (bvy > 60.0f) {
            float t = (line - by) / bvy;
            float xx = bx + bvx * (t > 0 ? t : 0);
            for (int i = 0; i < 3; i++) {
                if (xx > lim) xx = 2.0f * lim - xx;
                else if (xx < -lim) xx = -2.0f * lim - xx;
            }
            tx = xx + p->err;
        } else {
            float far = HK_FY - by, depth = HK_FY - line;
            tx = (far > depth ? bx * depth / far : bx) + p->err;
        }
        if (tx > HK_GOAL_X + 30.0f) tx = HK_GOAL_X + 30.0f;
        if (tx < -HK_GOAL_X - 30.0f) tx = -HK_GOAL_X - 30.0f;
        ty = line;
        /* a puck sitting by the goal: cover the mouth, close to it */
        if (deep && mine) {
            ty = HK_FY - m->r - 2.0f;
            tx = bx > 0 ? fminf(bx - R, HK_GOAL_X) : fmaxf(bx + R, -HK_GOAL_X);
        }
    }
    /* the finger eases towards it, quick but not instantly */
    float ax = (tx - p->fx) * 160.0f - p->fvx * 22.0f, ay = (ty - p->fy) * 160.0f - p->fvy * 22.0f;
    p->fvx += ax * HK_DT;
    p->fvy += ay * HK_DT;
    float sp = sqrtf(p->fvx * p->fvx + p->fvy * p->fvy);
    if (sp > 3000.0f) {
        p->fvx *= 3000.0f / sp;
        p->fvy *= 3000.0f / sp;
    }
    p->fx += p->fvx * HK_DT;
    p->fy += p->fvy * HK_DT;
    h->on = true;
    h->x = p->fx;
    h->y = p->fy * d;
    h->speed = 4000.0f;
}

typedef struct {
    int won, lost, draw, gf, ga, shots, rescues, tunnels;
    double secs;
    float vmax;
} tally_t;

static void play(int d0, int d1, uint32_t seed, tally_t *t)
{
    static hk_game_t g;
    memset(&g, 0, sizeof g);
    g.rnd = seed;
    g.puck_speed = puck_speed;
    bool person = d0 < 0;
    static person_t pp;
    memset(&pp, 0, sizeof pp);
    pp.rnd = seed * 31u + 7u;
    pp.fy = HK_HOME_Y;
    hk_game_start(&g, minutes, minutes ? 0 : goals, !person, person ? 0 : d0, true, d1, mallet_r);
    long steps = 0, cap = (long)HK_HZ * 60 * 30;
    int tunnels = 0;
    while (!hk_over(&g) && steps < cap) {
        if (g.state == HK_ST_REPLAY) hk_replay_end(&g);
        float opy = g.py;
        if (person) {
            hk_hand_t h;
            person_hand(&g, 0, &pp, &h);
            hk_game_step(&g, &h, NULL);
        } else {
            hk_game_step(&g, NULL, NULL);
        }
        /* the puck inside a mallet after a step: it went through */
        for (int s = 0; s < 2; s++) {
            float dx = g.px - g.m[s].x, dy = g.py - g.m[s].y, R = g.m[s].r + HK_PUCK_R;
            if (g.puck_in && dx * dx + dy * dy < (R - 1.0f) * (R - 1.0f)) tunnels++;
        }
        (void)opy;
        static int seen1;
        if (getenv("HK_TRACE") && g.state == HK_ST_GOAL && g.state_t < HK_DT * 0.5f + 1e-6f) {
            int a2 = g.hist_at;
            printf("  goal by %d at %.0f s: last touch %d, puck %.0f,%.0f v %.0f,%.0f; mallet0 %.0f,%.0f  mallet1 %.0f,%.0f\n",
                   g.last_goal, (double)g.t, g.last_touch, (double)g.px, (double)g.py, (double)g.pvx, (double)g.pvy,
                   (double)g.m[0].x, (double)g.m[0].y, (double)g.m[1].x, (double)g.m[1].y);
            for (int k = 120; k > 0; k -= 20) {
                int q = (a2 - k + HK_HIST) % HK_HIST;
                printf("      -%3d  puck %5.0f %5.0f  v %5.0f %5.0f\n", k, (double)g.hist_x[q], (double)g.hist_y[q], (double)g.hist_vx[q], (double)g.hist_vy[q]);
            }
        }
        (void)seen1;
        steps++;
        g.nsnd = 0;
    }
    int w = hk_winner(&g);
    if (w == 0) t->won++;
    else if (w == 1) t->lost++;
    else t->draw++;
    t->gf += g.score[0];
    t->ga += g.score[1];
    t->shots += g.shots;
    t->rescues += g.rescues;
    t->tunnels += tunnels;
    t->secs += (double)steps / HK_HZ;
    if (g.max_speed > t->vmax) t->vmax = g.max_speed;
    if (verbose)
        printf("    %d-%d  %4.0f s  shots %d  served again %d  vmax %.0f mm/s  through %d\n", g.score[0], g.score[1],
               (double)steps / HK_HZ, g.shots, g.rescues, (double)g.max_speed, tunnels);
}

static const char *const NAME_[HK_DIFF_N + 1] = { "person", "easy", "normal", "hard", "crack" };
#define NAME (NAME_ + 1)

static void pairing(int a, int b)
{
    tally_t t;
    memset(&t, 0, sizeof t);
    for (int i = 0; i < nmatch; i++) play(a, b, 0x1234u + (uint32_t)i * 7919u + (uint32_t)(a * 31 + b) * 104729u, &t);
    double n = nmatch;
    printf("%-6s vs %-6s  won %2d drew %2d lost %2d   goals %.1f-%.1f  %.0f s  a goal of the 2nd every %.0f s"
           "  shots %.0f  served again %.1f  vmax %.0f  through %d\n",
           NAME[a], NAME[b], t.won, t.draw, t.lost, t.gf / n, t.ga / n, t.secs / n, t.ga ? t.secs / t.ga : 0.0,
           t.shots / n, t.rescues / n, (double)t.vmax, t.tunnels);
}

int main(int argc, char **argv)
{
    int a = -9, b = -9;
    bool person = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) a = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) b = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) nmatch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) minutes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) goals = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) mallet_r = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) puck_speed = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-p")) person = true;
    }
    printf("%d matches a pairing, %s, mallet %.0f mm, puck x%.2f\n", nmatch, minutes ? "by time" : "to N goals",
           (double)mallet_r, (double)puck_speed);
    if (person) {
        for (int j = 0; j < HK_DIFF_N; j++) {
            if (b >= 0 && j != b) continue;
            pairing(-1, j);
        }
        return 0;
    }
    if (a >= 0 && b >= 0) {
        pairing(a, b);
        return 0;
    }
    for (int i = 0; i < HK_DIFF_N; i++) {
        for (int j = 0; j < HK_DIFF_N; j++) pairing(i, j);
    }
    return 0;
}
