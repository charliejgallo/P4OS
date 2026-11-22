/*
 * HOCKEY - the computer's hand
 *
 * It sees the puck as it was 'react' seconds ago (the history in the
 * game) and guesses where it is now as well as its 'predict' allows. While
 * the puck is away, or coming at its goal, it defends: it stands on the
 * line from its goal to the puck, or where a puck coming at it will cross
 * its line, bounces off the rails included if it reads them. When the puck
 * is in its half, within its 'reach' and not shooting at its goal, it goes
 * behind it (round it, if it is on the wrong side), and shoots through it
 * at a spot of the far mouth, or off a rail if its 'aim' allows; then it
 * goes back for a while ('cool'). Its error drifts slowly, as a hand does.
 *
 * Easy is slow, late and waits before it plays a puck; crack reads a bank
 * shot and hits the corners.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "hk_game.h"

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

void hk_ai_config(hk_ai_cfg_t *c, int diff)
{
    /* tuned with tools/harness.sh -p against a person with a finger (a
     * fifth of a second late, quick, aims at the middle of the mouth):
     * easy loses to them, normal is about even, hard wins, crack wins big */
    static const hk_ai_cfg_t T[HK_DIFF_N] = {
        /* diff      react  err    speed   strike  aim   predict reach  cool  hesitate */
        { HK_EASY,   0.30f, 55.0f, 1000.0f, 1100.0f, 0.0f, 0.0f, 280.0f, 0.75f, 0.65f },
        { HK_NORMAL, 0.20f, 30.0f, 1450.0f, 1550.0f, 0.25f, 0.55f, 380.0f, 0.45f, 0.30f },
        { HK_HARD,   0.16f, 24.0f, 1750.0f, 1800.0f, 0.45f, 0.8f, 430.0f, 0.35f, 0.18f },
        { HK_CRACK,  0.08f, 9.0f,  2600.0f, 2600.0f, 0.8f, 1.0f, 520.0f, 0.20f, 0.04f },
    };
    *c = T[diff < 0 ? 0 : diff >= HK_DIFF_N ? HK_DIFF_N - 1 : diff];
}

/* where a puck at (x, y) going (vx, vy) crosses the line y = line, the side
 * rails bounced if 'rails'; false if it does not come there soon */
static bool cross_at(float x, float y, float vx, float vy, float line, bool rails, float *cx, float *ct)
{
    if (fabsf(vy) < 40.0f) return false;
    float t = (line - y) / vy;
    if (t < 0 || t > 1.6f) return false;
    float xx = x + vx * t;
    float lim = HK_FX - HK_PUCK_R;
    if (rails) {
        for (int i = 0; i < 4; i++) {
            if (xx > lim) xx = 2.0f * lim - xx;
            else if (xx < -lim) xx = -2.0f * lim - xx;
        }
    }
    *cx = clampf(xx, -lim, lim);
    *ct = t;
    return true;
}

void hk_ai_hand(hk_game_t *g, int side, hk_hand_t *h)
{
    const hk_ai_cfg_t *c = &g->ai_cfg[side];
    hk_ai_t *ai = &g->ai[side];
    const hk_mallet_t *m = &g->m[side];
    float d = hk_dir(side);
    memset(h, 0, sizeof *h);
    h->on = true;
    h->speed = c->speed;

    /* what it sees: the puck a moment ago, in its own frame (its goal at +y) */
    int lag = (int)(c->react * HK_HZ);
    if (lag > HK_HIST - 1) lag = HK_HIST - 1;
    int at = (g->hist_at - lag + HK_HIST) % HK_HIST;
    float sx = g->hist_x[at], sy = g->hist_y[at] * d, svx = g->hist_vx[at], svy = g->hist_vy[at] * d;
    /* and where it thinks it is now */
    float k = c->react * c->predict;
    float px = sx + svx * k, py = sy + svy * k;
    float lim = HK_FX - HK_PUCK_R;
    if (px > lim) px = 2.0f * lim - px;
    if (px < -lim) px = -2.0f * lim - px;
    float mx = m->x, my = m->y * d;
    float R = m->r + HK_PUCK_R;

    /* the hand's error drifts */
    ai->err_v += (frnd(&ai->rnd) - 0.5f) * c->err * 30.0f * HK_DT - ai->err_v * 3.0f * HK_DT;
    ai->err_x = clampf(ai->err_x + ai->err_v * HK_DT, -c->err, c->err);
    if (ai->cool > 0) ai->cool -= HK_DT;
    ai->mode_t += HK_DT;

    bool mine = py > 8.0f && g->state != HK_ST_GOAL;
    ai->wait = mine ? ai->wait + HK_DT : 0;
    bool rushing = svy > 700.0f;              /* coming at its goal, fast */
    /* a slow puck anywhere in its half is always its to play */
    bool slow = svx * svx + svy * svy < 250.0f * 250.0f;
    bool can = mine && !rushing && ((HK_FY - py) < c->reach || slow) && ai->cool <= 0 && ai->wait >= c->hesitate;

    float tx = mx, ty = my;
    if (ai->mode == AI_DEFEND && can) {
        ai->mode = AI_SET;
        ai->mode_t = 0;
        /* where the shot goes: the mouth's middle, its corners, or off a rail */
        float spread = HK_GOAL_X * (0.25f + 0.6f * c->aim);
        ai->aim_x = (frnd(&ai->rnd) * 2.0f - 1.0f) * spread;
        ai->bank = frnd(&ai->rnd) < c->aim * 0.45f;
    }
    if ((ai->mode == AI_SET || ai->mode == AI_SHOT) && (!mine || rushing)) {
        ai->mode = AI_BACK;
        ai->mode_t = 0;
    }

    if (ai->mode == AI_SET || ai->mode == AI_SHOT) {
        /* the shot's line, from the puck to the spot (a bank aims at its mirror) */
        float gx = ai->aim_x + ai->err_x * 0.6f, gy = -HK_FY;
        if (ai->bank) {
            float wall = (px > 0 ? -1.0f : 1.0f) * lim;
            gx = 2.0f * wall - gx;
        }
        float ux = gx - px, uy = gy - py, ul = sqrtf(ux * ux + uy * uy);
        ux /= ul;
        uy /= ul;
        if (ai->mode == AI_SET) {
            float bx = px - ux * (R + 34.0f), by = py - uy * (R + 34.0f);
            /* on the wrong side of the puck: round it first */
            float rel = (mx - px) * ux + (my - py) * uy;
            if (rel > -R * 0.5f) {
                float side_x = mx >= px ? 1.0f : -1.0f;
                if (fabsf(px) > HK_FX - R - 40.0f) side_x = px > 0 ? -1.0f : 1.0f;
                bx = px + side_x * (R + 40.0f);
                by = py + R * 0.9f;
            }
            tx = bx;
            ty = by;
            float dx = mx - bx, dy = my - by;
            if ((dx * dx + dy * dy < 22.0f * 22.0f && rel <= -R * 0.5f) || ai->mode_t > 0.9f) {
                ai->mode = AI_SHOT;
                ai->mode_t = 0;
            }
        } else {
            tx = px + ux * 150.0f;
            ty = py + uy * 150.0f;
            h->speed = c->strike;
            if (m->hit_t < 0.02f || ai->mode_t > 0.32f) {
                ai->mode = AI_BACK;
                ai->mode_t = 0;
                ai->cool = c->cool;
            }
        }
    }
    if (ai->mode == AI_DEFEND || ai->mode == AI_BACK) {
        if (ai->mode == AI_BACK && ai->cool <= 0 && ai->mode_t > 0.2f) ai->mode = AI_DEFEND;
        /* on the line from its goal to the puck, or where it will come */
        float line = HK_HOME_Y;
        float cx, ct;
        if (svy > 60.0f && cross_at(px, py, svx, svy, line, c->predict >= 0.5f, &cx, &ct)) {
            tx = cx;
        } else {
            float depth = HK_FY - line, far = HK_FY - py;
            tx = far > depth ? px * depth / far : px;
        }
        tx = clampf(tx + ai->err_x, -(HK_GOAL_X + 40.0f), HK_GOAL_X + 40.0f);
        ty = line;
        /* a slow puck far away: it may come out a little */
        if (py < -100.0f && fabsf(svy) < 300.0f) ty = line - 40.0f * c->predict;
    }
    h->x = tx;
    h->y = ty * d;
}
