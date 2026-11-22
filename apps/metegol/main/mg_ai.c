/*
 * METEGOL - the computer's hand
 *
 * It sees the ball as it was 'react' seconds ago (the history in the
 * game), guesses where it will cross each of its rods, and puts the
 * nearest figure there with an error that drifts slowly (a hand is not a
 * servo) and no faster than its 'speed'. When the ball is in front of a
 * foot it kicks, aiming for a spot of the mouth by sweeping the rod
 * sideways during the swing (a pull shot) as well as its 'aim' allows;
 * a ball coming from behind (a team-mate's shot) makes it raise the rod.
 *
 * Easy is slow, late, weak and misses a quarter of its chances; crack is
 * almost a machine.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "mg_game.h"

#include <math.h>
#include <string.h>

static float frnd(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) / 16777216.0f;
}

static float clampf(float v, float a, float b)
{
    return v < a ? a : v > b ? b : v;
}

/* where the ball crosses x, bounced off the bands; false if it does not come */
static bool cross_y(float bx, float by, float vx, float vy, float x, float *y, float *t)
{
    if (fabsf(vx) < 60.0f) return false;
    float tt = (x - bx) / vx;
    if (tt < 0 || tt > 0.9f) return false;
    float yy = by + vy * tt;
    float lim = MG_FIELD_Y - MG_BALL_R;
    for (int i = 0; i < 4; i++) {
        if (yy > lim) yy = 2.0f * lim - yy;
        else if (yy < -lim) yy = -2.0f * lim - yy;
    }
    *y = clampf(yy, -lim, lim);
    *t = tt;
    return true;
}

void mg_ai_hand(mg_game_t *g, int side, mg_hand_t *h)
{
    const mg_ai_cfg_t *c = &g->ai_cfg[side];
    mg_ai_t *ai = &g->ai[side];
    memset(h, 0, sizeof *h);
    h->absolute = true;
    h->all = true;
    h->kick_rod = -1;
    h->speed = c->speed;
    float dir = side ? -1.0f : 1.0f;

    /* the ball as it was a moment ago */
    int lag = (int)(c->react * MG_HZ);
    if (lag > MG_HIST - 1) lag = MG_HIST - 1;
    int at = (g->hist_at - lag + MG_HIST * 4) % MG_HIST;
    float bx = g->hist_x[at], by = g->hist_y[at], vx = g->hist_vx[at], vy = g->hist_vy[at];
    /* it does extrapolate the lag away, a little */
    bx += vx * c->react * 0.5f;
    by += vy * c->react * 0.5f;

    if (ai->think > 0) ai->think -= MG_DT;
    if (ai->hold_t > 0) ai->hold_t -= MG_DT;

    for (int k = 0; k < MG_SIDE_RODS; k++) {
        mg_rod_t *r = &g->rod[mg_rod_ix(side, k)];
        /* the hand's drift */
        ai->noise_v[k] += (frnd(&ai->rnd) - 0.5f) * 2.0f * c->err * 9.0f * MG_DT - ai->noise_v[k] * 2.5f * MG_DT;
        ai->noise[k] += ai->noise_v[k] * MG_DT;
        ai->noise[k] = clampf(ai->noise[k], -c->err * 2.0f, c->err * 2.0f);

        float y = by, t = 0;
        float yc;
        if (cross_y(bx, by, vx, vy, r->x, &yc, &t)) y = yc * (t < 0.35f ? 1.0f : 0.6f) + by * (t < 0.35f ? 0.0f : 0.4f);
        /* a rod between the ball and its own goal covers the line from the
         * ball to the goal's middle, unless the ball is already coming */
        float rel = (bx - r->x) * dir;       /* > 0: the ball is in front */
        float gx = -dir * MG_FIELD_X;
        if (rel > 0 && t <= 0.0f) {
            float f = (r->x - bx) / (gx - bx);
            y = by + (0.0f - by) * clampf(f, 0.0f, 1.0f);
        }
        y += ai->noise[k] * (k == MG_GK ? 0.45f : 1.0f);
        int f = mg_nearest_fig(r, y);
        /* the figure that can get there */
        float want = y - r->base[f];
        if (want < r->lo || want > r->hi) {
            for (int i = 0; i < r->n; i++) {
                float w2 = y - r->base[i];
                if (w2 >= r->lo && w2 <= r->hi) {
                    f = i;
                    want = w2;
                    break;
                }
            }
        }
        /* a hand does not chase millimetres: small corrections wait (and the
         * figures that stay put cost the screen nothing) */
        want = clampf(want, r->lo, r->hi);
        h->target[k] = fabsf(want - r->tgt_off) < 6.0f ? r->tgt_off : want;

        /* a ball from behind going forward: its own shot, let it through */
        float real_rel = (g->bx - r->x) * dir;
        if (real_rel < -30.0f && g->vx * dir > 700.0f && g->last_touch == side) h->raise_mask |= (uint8_t)(1u << k);
    }

    /* a kick: the rod with the ball in front of one of its feet, once it
     * has been there long enough for this hand to notice */
    if (ai->think > 0 || g->state != MG_ST_PLAY) {
        ai->reach_t = 0;
        return;
    }
    bool any = false;
    for (int k = 0; k < MG_SIDE_RODS && !any; k++) {
        const mg_rod_t *r = &g->rod[mg_rod_ix(side, k)];
        float rel = (g->bx - r->x) * dir;
        float fy = r->base[mg_nearest_fig(r, g->by)] + r->off;
        if (rel > -78.0f && rel < 80.0f && fabsf(g->by - fy) < MG_FOOT_HALF + MG_BALL_R + 4.0f) any = true;
    }
    ai->reach_t = any ? ai->reach_t + MG_DT : 0;
    if (ai->reach_t < c->delay) return;
    for (int k = 0; k < MG_SIDE_RODS; k++) {
        mg_rod_t *r = &g->rod[mg_rod_ix(side, k)];
        if (r->kick || (h->raise_mask & (1u << k))) continue;
        float rel = (g->bx - r->x) * dir;
        /* in front, or behind: the turn's end comes down on it from behind */
        if (rel < -78.0f || rel > 80.0f) continue;
        if (rel < -6.0f && rel > -22.0f) continue;
        int f = mg_nearest_fig(r, g->by);
        float fy = r->base[f] + r->off;
        if (fabsf(g->by - fy) > MG_FOOT_HALF + MG_BALL_R + 4.0f) continue;
        float sp = sqrtf(g->vx * g->vx + g->vy * g->vy);
        /* a slow ball may be held a moment, to aim */
        if (sp < 200.0f && ai->hold_t <= 0 && ai->plan_rod != k) {
            ai->plan_rod = k;
            ai->hold_t = c->wait * (0.4f + frnd(&ai->rnd));
            ai->aim_y = (frnd(&ai->rnd) - 0.5f) * 2.0f * 70.0f;
            continue;
        }
        if (ai->plan_rod == k && ai->hold_t > 0 && sp < 200.0f) continue;
        ai->plan_rod = -1;
        if (frnd(&ai->rnd) < c->miss) {
            /* a late or early swing */
            ai->think = c->cool + 0.1f;
            h->kick_rod = k;
            h->kick_power = c->power * 0.6f;
            h->kick_slide = (frnd(&ai->rnd) - 0.5f) * 800.0f;
            return;
        }
        /* aim: from the ball to a spot of the mouth */
        float gx = dir * MG_FIELD_X;
        float ty = k >= MG_MID ? ai->aim_y : (frnd(&ai->rnd) - 0.5f) * 300.0f;
        float slope = (ty - g->by) / fabsf(gx - g->bx + 1e-3f);
        float vxs = 2600.0f + 2000.0f * c->power;
        float slide = slope * vxs / 0.45f;
        slide = slide * c->aim + (frnd(&ai->rnd) - 0.5f) * 900.0f * (1.0f - c->aim);
        h->kick_rod = k;
        h->kick_power = c->power * (0.85f + 0.15f * frnd(&ai->rnd));
        h->kick_slide = clampf(slide, -1800.0f, 1800.0f);
        h->kick_spin = (frnd(&ai->rnd) - 0.5f) * 0.6f * c->aim;
        ai->think = c->cool;
        return;
    }
}
