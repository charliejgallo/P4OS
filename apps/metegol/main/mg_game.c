/*
 * METEGOL - a match (see mg_game.h)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "mg_game.h"

#include <math.h>
#include <string.h>
#ifdef MG_DEBUG
#include <stdio.h>
#include <stdlib.h>
#endif

#define WALL_E     0.70f        /* restitution of the bands */
#define POST_E     0.60f
#define FOOT_E     0.55f
#define FOOT_MU    0.45f        /* how much of a foot's sideways speed the ball takes */
#define ROLL_DECEL 140.0f       /* mm/s^2 */
#define DRAG       0.30f        /* 1/s */
#define SPIN_K     0.40f        /* curve: 1/s of turn per unit of spin */
#define SPIN_DECAY 1.6f
#define V_MAX      4800.0f
#define SLOW       30.0f        /* mm/s: the ball is as good as still */
#define POST_R     6.0f

static const float ROD_X[MG_SIDE_RODS] = { -560.0f, -390.0f, -75.0f, 225.0f };
static const int   ROD_N[MG_SIDE_RODS] = { 1, 2, 5, 3 };
static const float ROD_SPACING[MG_SIDE_RODS] = { 0.0f, 240.0f, 122.0f, 205.0f };
static const float ROD_TRAVEL[MG_SIDE_RODS] = { 160.0f, 190.0f, 71.0f, 110.0f };

static float frnd(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) / 16777216.0f;
}

static float clampf(float v, float a, float b)
{
    return v < a ? a : v > b ? b : v;
}

static void snd(mg_game_t *g, int s, float vol)
{
    if (g->nsnd >= 16) return;
    int v = (int)(vol * 255.0f);
    g->snd[g->nsnd] = (uint8_t)s;
    g->snd_vol[g->nsnd] = (uint8_t)(v < 20 ? 20 : v > 255 ? 255 : v);
    g->nsnd++;
}

void mg_ai_config(mg_ai_cfg_t *c, int diff)
{
    /* tuned with tools/harness.sh -p against a person on a phone (one
     * finger for every rod, a quarter of a second late): easy loses to them,
     * normal is about even, hard wins, ace wins big */
    static const mg_ai_cfg_t T[MG_DIFF_N] = {
        /* diff      react  err    speed   power aim   miss   wait  delay  cool */
        { MG_EASY,   0.36f, 55.0f, 520.0f,  0.38f, 0.0f, 0.45f, 0.9f, 0.40f, 0.70f },
        { MG_NORMAL, 0.24f, 34.0f, 800.0f,  0.52f, 0.1f, 0.25f, 0.6f, 0.22f, 0.45f },
        { MG_HARD,   0.14f, 18.0f, 1200.0f, 0.72f, 0.4f, 0.10f, 0.4f, 0.10f, 0.25f },
        { MG_CRACK,  0.09f, 11.0f, 1600.0f, 0.82f, 0.6f, 0.05f, 0.3f, 0.07f, 0.22f },
    };
    *c = T[diff < 0 ? 0 : diff >= MG_DIFF_N ? MG_DIFF_N - 1 : diff];
}

static void rods_reset(mg_game_t *g)
{
    for (int s = 0; s < 2; s++) {
        for (int k = 0; k < MG_SIDE_RODS; k++) {
            mg_rod_t *r = &g->rod[mg_rod_ix(s, k)];
            memset(r, 0, sizeof *r);
            r->side = s;
            r->role = k;
            r->x = s ? -ROD_X[k] : ROD_X[k];
            r->n = ROD_N[k];
            for (int i = 0; i < r->n; i++) r->base[i] = ((float)i - (float)(r->n - 1) * 0.5f) * ROD_SPACING[k];
            r->lo = -ROD_TRAVEL[k];
            r->hi = ROD_TRAVEL[k];
            r->hit_t = 10.0f;
        }
    }
}

static void serve(mg_game_t *g)
{
    g->state = MG_ST_SERVE;
    g->state_t = 0;
    float s = g->serve_side ? 1.0f : -1.0f;
    g->bx = (frnd(&g->rnd) - 0.5f) * 20.0f;
    g->by = s * (MG_FIELD_Y - MG_BALL_R - 1.0f);
    g->vx = (frnd(&g->rnd) - 0.5f) * 160.0f;
    g->vy = -s * (520.0f + frnd(&g->rnd) * 260.0f) * g->ball_speed;
    g->spin = 0;
    g->ball_in = true;
    g->idle_t = g->far_t = 0;
    g->last_touch = -1;
    g->last_rod = -1;
    g->serve_side ^= 1;
    snd(g, SND_SERVE, 0.8f);
}

void mg_game_start(mg_game_t *g, int halves_min, int goals, bool swap, bool ai0, int diff0, bool ai1, int diff1)
{
    float bs = g->ball_speed > 0.5f ? g->ball_speed : 1.0f;
    uint32_t seed = g->rnd ? g->rnd : 0x5EEDu;
    memset(g, 0, sizeof *g);
    g->ball_speed = bs;
    g->rnd = seed;
    g->halves_min = halves_min;
    g->goals = goals;
    g->swap_sides = swap;
    g->ai_on[0] = ai0;
    g->ai_on[1] = ai1;
    mg_ai_config(&g->ai_cfg[0], diff0);
    mg_ai_config(&g->ai_cfg[1], diff1);
    for (int s = 0; s < 2; s++) {
        g->ai[s].rnd = g->rnd ^ (0x9E3779B9u * (uint32_t)(s + 1));
        g->ai[s].plan_rod = -1;
    }
    rods_reset(g);
    g->serve_side = (int)(frnd(&g->rnd) * 2.0f) & 1;
    serve(g);
    snd(g, SND_WHISTLE, 1.0f);
}

int mg_nearest_fig(const mg_rod_t *r, float y)
{
    int best = 0;
    float bd = 1e9f;
    for (int i = 0; i < r->n; i++) {
        float d = fabsf(r->base[i] + r->off - y);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

int mg_nearest_rod(const mg_game_t *g, int side)
{
    int best = 0;
    float bd = 1e9f;
    for (int k = 0; k < MG_SIDE_RODS; k++) {
        float d = fabsf(g->rod[mg_rod_ix(side, k)].x - g->bx);
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

bool mg_foot(const mg_rod_t *r, int fig, float *fx, float *fy)
{
    float dir = r->side ? -1.0f : 1.0f;
    float a = r->ang;
    *fy = r->base[fig] + r->off;
    if (fabsf(a) >= MG_FOOT_MAX) {
        *fx = r->x + dir * MG_FOOT_D * tanf(clampf(a, -1.2f, 1.2f));
        return false;
    }
    *fx = r->x + dir * MG_FOOT_D * tanf(a);
    return true;
}

/* ---- the hands ------------------------------------------------------------------------------------ */

static void rod_kick(mg_game_t *g, int side, int k, float power, float spin, float slide)
{
    mg_rod_t *r = &g->rod[mg_rod_ix(side, k)];
    if (r->kick == 1) return;
    r->kick = 1;
    r->kick_turn = 0;
    r->kick_w = (16.0f + 32.0f * clampf(power, 0.0f, 1.0f)) * (0.85f + 0.15f * g->ball_speed);
    r->kick_spin = clampf(spin, -1.0f, 1.0f);
    r->kick_slide = clampf(slide, -1900.0f, 1900.0f);
    /* the rods in front of it get out of the way of its own shot */
    for (int j = k + 1; j < MG_SIDE_RODS; j++) {
        mg_rod_t *f = &g->rod[mg_rod_ix(side, j)];
        if (f->raise_t < 0.30f) f->raise_t = 0.30f;
    }
    g->kicks++;
}

static void apply_hand(mg_game_t *g, int side, const mg_hand_t *h)
{
    for (int k = 0; k < MG_SIDE_RODS; k++) {
        mg_rod_t *r = &g->rod[mg_rod_ix(side, k)];
        if (h->absolute) {
            r->tgt_off = clampf(h->target[k], r->lo, r->hi);
        } else if (h->all || h->rod == k) {
            r->tgt_off = clampf(r->tgt_off + h->move, r->lo, r->hi);
        }
        r->hold = h->raise || (h->raise_mask & (1u << k));
    }
    if (h->kick_rod >= 0 && h->kick_rod < MG_SIDE_RODS) rod_kick(g, side, h->kick_rod, h->kick_power, h->kick_spin, h->kick_slide);
}

/* ---- the rods ------------------------------------------------------------------------------------- */

static void rods_step(mg_game_t *g, const float speed[2])
{
    for (int i = 0; i < MG_RODS; i++) {
        mg_rod_t *r = &g->rod[i];
        float sp = speed[r->side];
        float old = r->off;
        float want = r->tgt_off;
        if (r->kick == 1) {
            /* a pull shot: the hand sweeps the rod while it turns */
            want = clampf(r->off + r->kick_slide * MG_DT, r->lo, r->hi);
            r->tgt_off = want;
            sp = 4000.0f;
        }
        float d = want - r->off, m = sp * MG_DT;
        r->off += clampf(d, -m, m);
        if (r->off <= r->lo || r->off >= r->hi) {
            if (fabsf(r->voff) > 900.0f) snd(g, SND_STOP, fabsf(r->voff) / 2500.0f);
            r->off = clampf(r->off, r->lo, r->hi);
        }
        r->voff = (r->off - old) / MG_DT;

        if (r->raise_t > 0) r->raise_t -= MG_DT;
        /* raised forward: the feet go away from a ball that comes from behind */
        float rest = (r->hold || r->raise_t > 0) ? MG_RAISED : 0.0f;
        if (r->kick == 1) {
            /* a whole turn: the foot goes over and comes round from behind */
            r->w = r->kick_w;
            r->ang = mg_wrap(r->ang + r->w * MG_DT);
            r->kick_turn += r->w * MG_DT;
            if (r->kick_turn >= 6.2f) {
                r->kick = 0;
                r->w = 0;
            }
        } else {
            float w = clampf((rest - r->ang) * 30.0f, -16.0f, 16.0f);
            r->w = w;
            r->ang += w * MG_DT;
        }
        r->hit_t += MG_DT;
    }
}

/* ---- the ball ------------------------------------------------------------------------------------- */

static void bounce(mg_game_t *g, float nx, float ny, float e, int sound)
{
    float vn = g->vx * nx + g->vy * ny;
    if (vn >= 0) return;
    g->vx -= (1.0f + e) * vn * nx;
    g->vy -= (1.0f + e) * vn * ny;
    /* a band takes some of the spin, and turns some of it into the bounce */
    g->spin *= 0.55f;
    if (-vn > 180.0f && g->wall_t <= 0) {
        snd(g, sound, -vn / 3500.0f);
        g->wall_t = 0.05f;
    }
}

/* the field's bands, its rounded corners, the posts and the nets */
static void walls(mg_game_t *g)
{
    const float R = MG_BALL_R;
    float ax = fabsf(g->bx), ay = fabsf(g->by);
    float sx = g->bx < 0 ? -1.0f : 1.0f, sy = g->by < 0 ? -1.0f : 1.0f;
    if (ax > MG_FIELD_X - 2.0f && ay < MG_GOAL_Y) {
        /* in the mouth or the net: its sides and its back */
        if (ay > MG_GOAL_Y - R && ax > MG_FIELD_X) {
            g->by = sy * (MG_GOAL_Y - R);
            bounce(g, 0, -sy, 0.3f, SND_NET);
        }
        if (ax > MG_FIELD_X + MG_GOAL_D - R) {
            g->bx = sx * (MG_FIELD_X + MG_GOAL_D - R);
            bounce(g, -sx, 0, 0.15f, SND_NET);
        }
    } else {
        if (ay > MG_FIELD_Y - R) {
            g->by = sy * (MG_FIELD_Y - R);
            bounce(g, 0, -sy, WALL_E, SND_WALL);
        }
        if (ax > MG_FIELD_X - R && ay >= MG_GOAL_Y) {
            g->bx = sx * (MG_FIELD_X - R);
            bounce(g, -sx, 0, WALL_E, SND_WALL);
        }
        /* the corners are round: a ball cannot sit in one */
        float cx = MG_FIELD_X - MG_CORNER_R, cy = MG_FIELD_Y - MG_CORNER_R;
        ax = fabsf(g->bx);
        ay = fabsf(g->by);
        if (ax > cx && ay > cy) {
            float dx = ax - cx, dy = ay - cy;
            float d = sqrtf(dx * dx + dy * dy), lim = MG_CORNER_R - R;
            if (d > lim) {
                float nx = dx / d, ny = dy / d;
                g->bx = sx * (cx + nx * lim);
                g->by = sy * (cy + ny * lim);
                bounce(g, -nx * sx, -ny * sy, WALL_E, SND_WALL);
            }
        }
    }
    /* the posts */
    for (int k = 0; k < 4; k++) {
        float px = (k & 1) ? MG_FIELD_X : -MG_FIELD_X, py = (k & 2) ? MG_GOAL_Y : -MG_GOAL_Y;
        float dx = g->bx - px, dy = g->by - py;
        float d2 = dx * dx + dy * dy, lim = R + POST_R;
        if (d2 < lim * lim && d2 > 1e-6f) {
            float d = sqrtf(d2), nx = dx / d, ny = dy / d;
            g->bx = px + nx * lim;
            g->by = py + ny * lim;
            bounce(g, nx, ny, POST_E, SND_POST);
        }
    }
}

/* a foot against the ball; f in 0..1 is how far into the step the rods are */
static void feet(mg_game_t *g, const float *ang0, const float *off0, float f)
{
    const float lim = MG_BALL_R + MG_FOOT_R;
    for (int i = 0; i < MG_RODS; i++) {
        mg_rod_t *r = &g->rod[i];
        float a = mg_wrap(ang0[i] + mg_wrap(r->ang - ang0[i]) * f);
        if (fabsf(a) >= MG_FOOT_MAX) continue;
        float dir = r->side ? -1.0f : 1.0f;
        float fx = r->x + dir * MG_FOOT_D * tanf(a);
        if (fabsf(g->bx - fx) > lim + 1.0f) continue;
        float off = off0[i] + (r->off - off0[i]) * f;
        for (int k = 0; k < r->n; k++) {
            float fy = r->base[k] + off;
            float cy = clampf(g->by, fy - MG_FOOT_HALF, fy + MG_FOOT_HALF);
            float dx = g->bx - fx, dy = g->by - cy;
            float d2 = dx * dx + dy * dy;
            if (d2 >= lim * lim) continue;
            float d = sqrtf(d2), nx, ny;
            if (d < 1e-3f) {
                /* dead centre: out the side the rod faces */
                nx = dir;
                ny = 0;
                d = 0;
            } else {
                nx = dx / d;
                ny = dy / d;
            }
            g->bx = fx + nx * lim;
            g->by = cy + ny * lim;
            float c = cosf(a);
            float sec2 = 1.0f / (c * c);
            if (sec2 > 1.5f) sec2 = 1.5f;
            float vfx = dir * MG_FOOT_D * r->w * sec2, vfy = r->voff;
            float rx = g->vx - vfx, ry = g->vy - vfy;
            float vn = rx * nx + ry * ny;
            if (vn < 0) {
                g->vx -= (1.0f + FOOT_E) * vn * nx;
                g->vy -= (1.0f + FOOT_E) * vn * ny;
                /* the foot drags the ball along its sweep */
                float tx = -ny, ty = nx;
                float vt = rx * tx + ry * ty;
                g->vx -= FOOT_MU * vt * tx;
                g->vy -= FOOT_MU * vt * ty;
                if (r->kick == 1) g->spin += r->kick_spin * 2.2f + (vfy / 1500.0f);
                float hard = -vn;
                if (hard > 120.0f && g->kick_t <= 0) {
                    snd(g, hard > 1400.0f ? SND_KICK : SND_KICK_SOFT, hard / 4000.0f);
                    g->kick_t = 0.04f;
                }
                if (r->kick == 1 && hard > 1500.0f) g->shots[r->side]++;
            }
            r->hit_t = 0;
            g->last_touch = r->side;
            g->last_rod = i;
        }
    }
}

static void ball_move(mg_game_t *g, float h)
{
    float sp = sqrtf(g->vx * g->vx + g->vy * g->vy);
    if (sp > 1e-3f) {
        /* the effect bends the path */
        g->vx += -g->vy * g->spin * SPIN_K * h;
        g->vy += g->vx * g->spin * SPIN_K * h;
        float dec = ROLL_DECEL * h + DRAG * sp * h;
        float k = sp > dec ? (sp - dec) / sp : 0.0f;
        g->vx *= k;
        g->vy *= k;
    }
    g->spin *= 1.0f - SPIN_DECAY * h;
    /* the table's ramps: the corners by the goals and the foot of the bands
     * are raised a little, so a ball does not die where no foot reaches */
    float ax = fabsf(g->bx), ay = fabsf(g->by);
    float sx = g->bx < 0 ? -1.0f : 1.0f, sy = g->by < 0 ? -1.0f : 1.0f;
    if (ax > 470.0f && ay > 170.0f) {
        g->vy -= sy * 380.0f * h;
        g->vx -= sx * 160.0f * h;
    } else if (ay > 305.0f) {
        g->vy -= sy * 220.0f * h;
    }
    g->bx += g->vx * h;
    g->by += g->vy * h;
    g->roll += sp * h;
}

/* ---- the rules -------------------------------------------------------------------------------------- */

static void record(mg_game_t *g)
{
    g->rec_acc += MG_DT;
    if (g->rec_acc < 1.0f / 60.0f) return;
    g->rec_acc -= 1.0f / 60.0f;
    mg_rec_t *e = &g->rec[g->rec_at];
    e->bx = (int16_t)clampf(g->bx * 10.0f, -32000, 32000);
    e->by = (int16_t)clampf(g->by * 10.0f, -32000, 32000);
    for (int i = 0; i < MG_RODS; i++) {
        e->off[i] = (int16_t)(g->rod[i].off * 10.0f);
        e->ang[i] = (int16_t)clampf(g->rod[i].ang * 4096.0f, -32000, 32000);
    }
    e->roll = (uint8_t)((int)(g->roll * 0.25f) & 255);
    e->vis = g->ball_in;
    g->rec_at = (g->rec_at + 1) % MG_REC_N;
    if (g->rec_n < MG_REC_N) g->rec_n++;
}

void mg_replay_apply(mg_game_t *g, int k)
{
    if (k < 0) k = 0;
    if (k >= g->rp_len) k = g->rp_len - 1;
    if (k < 0) return;
    const mg_rec_t *e = &g->rec[(g->rp_from + k) % MG_REC_N];
    g->bx = e->bx * 0.1f;
    g->by = e->by * 0.1f;
    for (int i = 0; i < MG_RODS; i++) {
        g->rod[i].off = e->off[i] * 0.1f;
        g->rod[i].ang = e->ang[i] / 4096.0f;
    }
    g->roll = e->roll * 4.0f;
    g->ball_in = e->vis;
}

static void goal(mg_game_t *g, int scorer)
{
#ifdef MG_DEBUG
    printf("goal by %d, last rod %d (side %d role %d), speed %.0f, t %.1f\n", scorer, g->last_rod, g->last_rod >= 0 ? g->rod[g->last_rod].side : -1, g->last_rod >= 0 ? g->rod[g->last_rod].role : -1, (double)sqrtf(g->vx * g->vx + g->vy * g->vy), (double)g->t);
    if (g->last_rod >= 0 && g->rod[g->last_rod].side != scorer && getenv("MG_TRACE")) {
        for (int k = 60; k >= 0; k -= 6) {
            int a = (g->hist_at - k + MG_HIST * 4) % MG_HIST;
            printf("   -%2d  ball %6.0f %6.0f  v %6.0f %6.0f\n", k, (double)g->hist_x[a], (double)g->hist_y[a], (double)g->hist_vx[a], (double)g->hist_vy[a]);
        }
        const mg_rod_t *r = &g->rod[g->last_rod];
        printf("   rod x %.0f off %.0f ang %.2f w %.1f kick %d base0 %.0f\n", (double)r->x, (double)r->off, (double)r->ang, (double)r->w, r->kick, (double)r->base[0]);
    }
#endif
    g->score[scorer]++;
    g->last_goal = scorer;
    g->state = MG_ST_GOAL;
    g->state_t = 0;
    snd(g, SND_NET, 1.0f);
    snd(g, SND_ROAR, 1.0f);
    /* the replay: the last two and a half seconds, and a little after */
    int n = g->rec_n < 150 ? g->rec_n : 150;
    g->rp_len = n;
    g->rp_from = (g->rec_at - n + MG_REC_N) % MG_REC_N;
    memcpy(g->rod_keep, g->rod, sizeof g->rod);
}

void mg_replay_end(mg_game_t *g)
{
    if (g->state != MG_ST_REPLAY) return;
    memcpy(g->rod, g->rod_keep, sizeof g->rod);
    serve(g);
}

bool mg_over(const mg_game_t *g)
{
    return g->state == MG_ST_END;
}

int mg_winner(const mg_game_t *g)
{
    return g->score[0] > g->score[1] ? 0 : g->score[1] > g->score[0] ? 1 : -1;
}

/* the computer's history of the ball: it sees it a little late */
static void hist_push(mg_game_t *g)
{
    g->hist_at = (g->hist_at + 1) % MG_HIST;
    g->hist_x[g->hist_at] = g->bx;
    g->hist_y[g->hist_at] = g->by;
    g->hist_vx[g->hist_at] = g->vx;
    g->hist_vy[g->hist_at] = g->vy;
}

static void halftime_swap(mg_game_t *g)
{
    g->swapped = !g->swapped;
    int t = g->score[0];
    g->score[0] = g->score[1];
    g->score[1] = t;
    bool b = g->ai_on[0];
    g->ai_on[0] = g->ai_on[1];
    g->ai_on[1] = b;
    mg_ai_cfg_t c = g->ai_cfg[0];
    g->ai_cfg[0] = g->ai_cfg[1];
    g->ai_cfg[1] = c;
    mg_ai_t a = g->ai[0];
    g->ai[0] = g->ai[1];
    g->ai[1] = a;
}

void mg_game_step(mg_game_t *g, const mg_hand_t *hand0, const mg_hand_t *hand1)
{
    g->nsnd = g->nsnd > 12 ? 12 : g->nsnd;
    g->t += MG_DT;
    g->state_t += MG_DT;
    if (g->wall_t > 0) g->wall_t -= MG_DT;
    if (g->kick_t > 0) g->kick_t -= MG_DT;

    switch (g->state) {
    case MG_ST_GOAL:
        /* the ball drops into the goal's tray; the rods stay where they were */
        if (g->state_t > 0.18f) g->ball_in = false;
        ball_move(g, MG_DT);
        if (g->state_t > 1.7f) {
            if (g->goals && g->score[g->last_goal] >= g->goals) {
                g->state = MG_ST_END;
                g->state_t = 0;
                snd(g, SND_WHISTLE_END, 1.0f);
            } else if (g->rp_len > 20) {
                g->state = MG_ST_REPLAY;
                g->state_t = 0;
                g->rp_t = 0;
            } else {
                serve(g);
            }
        }
        return;
    case MG_ST_REPLAY:
        /* the app shows it (mg_replay_apply); it ends there or by a tap */
        return;
    case MG_ST_HALF:
        if (g->state_t > 2.6f) {
            g->half = 1;
            g->clock = 0;
            if (g->swap_sides) halftime_swap(g);
            rods_reset(g);
            serve(g);
            snd(g, SND_WHISTLE, 1.0f);
        }
        return;
    case MG_ST_END:
        return;
    default:
        break;
    }

    /* the hands */
    mg_hand_t ai[2];
    const mg_hand_t *h[2] = { hand0, hand1 };
    float speed[2];
    for (int s = 0; s < 2; s++) {
        if (g->ai_on[s]) {
            mg_ai_hand(g, s, &ai[s]);
            h[s] = &ai[s];
        }
        speed[s] = h[s] ? h[s]->speed : 0;
        if (h[s]) apply_hand(g, s, h[s]);
        else {
            for (int k = 0; k < MG_SIDE_RODS; k++) g->rod[mg_rod_ix(s, k)].hold = false;
        }
    }

    float ang0[MG_RODS], off0[MG_RODS];
    for (int i = 0; i < MG_RODS; i++) {
        ang0[i] = g->rod[i].ang;
        off0[i] = g->rod[i].off;
    }
    rods_step(g, speed);

    /* substeps: neither the ball nor a foot moves more than ~6 mm in one */
    float sp = sqrtf(g->vx * g->vx + g->vy * g->vy);
    float most = sp * MG_DT;
    for (int i = 0; i < MG_RODS; i++) {
        float m = fabsf(mg_wrap(g->rod[i].ang - ang0[i])) * MG_FOOT_D * 2.0f + fabsf(g->rod[i].off - off0[i]);
        if (m > most && fabsf(g->bx - g->rod[i].x) < 120.0f) most = m;
    }
    int n = 1 + (int)(most / 6.0f);
    if (n > 12) n = 12;
    float hstep = MG_DT / (float)n;
    for (int k = 1; k <= n; k++) {
        ball_move(g, hstep);
        walls(g);
        feet(g, ang0, off0, (float)k / (float)n);
    }
    sp = sqrtf(g->vx * g->vx + g->vy * g->vy);
    if (sp > V_MAX * g->ball_speed) {
        g->vx *= V_MAX * g->ball_speed / sp;
        g->vy *= V_MAX * g->ball_speed / sp;
        sp = V_MAX * g->ball_speed;
    }
    if (sp > g->max_speed) g->max_speed = sp;
    hist_push(g);
    record(g);

    if (g->state == MG_ST_SERVE && g->state_t > 0.5f) g->state = MG_ST_PLAY;

    /* a goal: the ball's centre past the line, inside the mouth */
    if (fabsf(g->by) < MG_GOAL_Y && fabsf(g->bx) > MG_FIELD_X + 6.0f) {
        goal(g, g->bx > 0 ? 0 : 1);
        return;
    }

    /* how near a goal it is, for the crowd */
    float ng = (fabsf(g->bx) - 250.0f) / 350.0f;
    g->near_goal = clampf(ng, 0.0f, 1.0f);

    /* a ball that sits: out of every foot's reach it is served again soon,
     * within reach after a while (a player may hold it, but not forever) */
    if (sp < SLOW) {
        g->idle_t += MG_DT;
        bool reach = false;
        for (int i = 0; i < MG_RODS && !reach; i++) {
            const mg_rod_t *r = &g->rod[i];
            if (fabsf(g->bx - r->x) > 80.0f) continue;
            for (int k = 0; k < r->n; k++) {
                float y = r->base[k];
                if (g->by > y + r->lo - 22.0f && g->by < y + r->hi + 22.0f) reach = true;
            }
        }
        if (!reach) g->far_t += MG_DT;
        else g->far_t = 0;
        if (g->idle_t > 3.0f && g->idle_t - MG_DT <= 3.0f) g->stuck++;
        if (g->far_t > 1.6f || g->idle_t > 6.0f) {
            g->deads++;
#ifdef MG_DEBUG
            printf("dead %s at %.0f %.0f\n", g->far_t > 1.6f ? "far" : "held", (double)g->bx, (double)g->by);
#endif
            snd(g, SND_WHISTLE, 0.7f);
            serve(g);
        }
    } else {
        g->idle_t = 0;
        g->far_t = 0;
    }

    /* the clock */
    if (g->halves_min > 0) {
        g->clock += MG_DT;
        if (g->clock >= (float)g->halves_min * 60.0f) {
            if (g->half == 0) {
                g->state = MG_ST_HALF;
                g->state_t = 0;
                snd(g, SND_WHISTLE_END, 0.9f);
            } else {
                g->state = MG_ST_END;
                g->state_t = 0;
                snd(g, SND_WHISTLE_END, 1.0f);
            }
        }
    }
}
