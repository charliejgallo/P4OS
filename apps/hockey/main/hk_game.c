/*
 * HOCKEY - a match (see hk_game.h)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "hk_game.h"

#include <math.h>
#include <string.h>

#define RAIL_E     0.86f        /* restitution of the rails */
#define RAIL_MU    0.04f        /* what a rail takes of the speed along it */
#define POST_E     0.70f
#define MALLET_E   0.78f        /* puck against mallet */
#define MALLET_MU  0.12f        /* how much of a mallet's sideways slide the puck takes */
#define DECEL      22.0f        /* mm/s^2: the air bed is almost free */
#define DRAG       0.045f       /* 1/s */
#define V_MAX      2600.0f      /* mm/s at the normal puck speed */
#define SLOW       30.0f        /* mm/s: as good as still */
#define IDLE_S     4.0f         /* a still puck is served again after this */

static float frnd(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) / 16777216.0f;
}

static float clampf(float v, float a, float b)
{
    return v < a ? a : v > b ? b : v;
}

static void snd(hk_game_t *g, int s, float vol)
{
    if (g->nsnd >= 16) return;
    int v = (int)(vol * 255.0f);
    g->snd[g->nsnd] = (uint8_t)s;
    g->snd_vol[g->nsnd] = (uint8_t)(v < 24 ? 24 : v > 255 ? 255 : v);
    g->nsnd++;
}

void hk_mallet_clamp(const hk_game_t *g, int side, float *x, float *y)
{
    float r = g->m[side].r, d = hk_dir(side);
    float ly = clampf(*y * d, r, HK_FY - r);
    float lx = clampf(*x, -(HK_FX - r), HK_FX - r);
    /* the round corners */
    float cx = HK_FX - HK_CORNER_R, cy = HK_FY - HK_CORNER_R;
    if (fabsf(lx) > cx && ly > cy) {
        float sx = lx < 0 ? -1.0f : 1.0f;
        float dx = fabsf(lx) - cx, dy = ly - cy;
        float dd = sqrtf(dx * dx + dy * dy), lim = HK_CORNER_R - r;
        if (dd > lim && dd > 1e-4f) {
            lx = sx * (cx + dx / dd * lim);
            ly = cy + dy / dd * lim;
        }
    }
    *x = lx;
    *y = ly * d;
}

static void mallets_home(hk_game_t *g)
{
    for (int s = 0; s < 2; s++) {
        hk_mallet_t *m = &g->m[s];
        m->x = m->tx = 0;
        m->y = m->ty = hk_dir(s) * HK_HOME_Y;
        m->vx = m->vy = 0;
        m->hit_t = 10.0f;
    }
}

/* the puck lies still in the half of the side that gets it */
static void serve(hk_game_t *g, int side)
{
    g->state = HK_ST_SERVE;
    g->state_t = 0;
    g->serve_side = side;
    g->px = (frnd(&g->rnd) - 0.5f) * 60.0f;
    g->py = hk_dir(side) * HK_FY * 0.40f;
    g->pvx = g->pvy = 0;
    g->puck_in = true;
    g->idle_t = 0;
    g->last_touch = -1;
    /* a mallet sitting there steps back */
    hk_mallet_t *m = &g->m[side];
    float dx = m->x - g->px, dy = m->y - g->py, R = m->r + HK_PUCK_R + 8.0f;
    if (dx * dx + dy * dy < R * R) {
        m->y = g->py + hk_dir(side) * R;
        hk_mallet_clamp(g, side, &m->x, &m->y);
    }
    for (int s = 0; s < 2; s++) {
        g->ai[s].mode = AI_DEFEND;
        g->ai[s].wait = 0;
    }
    snd(g, SND_PLACE, 0.8f);
}

void hk_game_start(hk_game_t *g, int minutes, int goals, bool ai0, int diff0, bool ai1, int diff1, float mallet_r)
{
    float ps = g->puck_speed > 0.5f ? g->puck_speed : 1.0f;
    uint32_t seed = g->rnd ? g->rnd : 0x5EEDu;
    memset(g, 0, sizeof *g);
    g->puck_speed = ps;
    g->rnd = seed;
    g->minutes = minutes;
    g->goals = goals;
    g->ai_on[0] = ai0;
    g->ai_on[1] = ai1;
    hk_ai_config(&g->ai_cfg[0], diff0);
    hk_ai_config(&g->ai_cfg[1], diff1);
    for (int s = 0; s < 2; s++) {
        g->ai[s].rnd = g->rnd ^ (0x9E3779B9u * (uint32_t)(s + 1));
        g->m[s].r = mallet_r > 20.0f ? mallet_r : HK_MALLET_M;
        g->m[s].speed = 3000.0f;
    }
    mallets_home(g);
    /* against the board the person serves first; two people toss for it */
    int first = ai1 && !ai0 ? 0 : ai0 && !ai1 ? 1 : (int)(frnd(&g->rnd) * 2.0f) & 1;
    serve(g, first);
    snd(g, SND_BUZZ, 1.0f);
}

/* ---- the mallets ---------------------------------------------------------------------------- */

static void mallet_aim(hk_game_t *g, int side, const hk_hand_t *h)
{
    hk_mallet_t *m = &g->m[side];
    if (!h || !h->on) {
        m->tx = m->x;
        m->ty = m->y;
        return;
    }
    float x = h->x, y = h->y;
    hk_mallet_clamp(g, side, &x, &y);
    m->tx = x;
    m->ty = y;
    m->speed = h->speed > 50.0f ? h->speed : 50.0f;
}

/* ---- the puck ------------------------------------------------------------------------------- */

static void bounce(hk_game_t *g, float nx, float ny, float e, int sound)
{
    float vn = g->pvx * nx + g->pvy * ny;
    if (vn >= 0) return;
    g->pvx -= (1.0f + e) * vn * nx;
    g->pvy -= (1.0f + e) * vn * ny;
    /* the rail rubs a little of the speed along it */
    float tx = -ny, ty = nx;
    float vt = g->pvx * tx + g->pvy * ty;
    g->pvx -= RAIL_MU * vt * tx;
    g->pvy -= RAIL_MU * vt * ty;
    if (-vn > 120.0f && g->wall_t <= 0) {
        snd(g, sound, -vn / 2600.0f);
        g->wall_t = 0.04f;
    }
}

/* the rails, the round corners, the posts and the goals' slots */
static void walls(hk_game_t *g)
{
    const float R = HK_PUCK_R;
    float ax = fabsf(g->px), ay = fabsf(g->py);
    float sx = g->px < 0 ? -1.0f : 1.0f, sy = g->py < 0 ? -1.0f : 1.0f;
    if (ay > HK_FY - R && ax < HK_GOAL_X) {
        /* in the mouth: past the line only through the slot */
        if (ay > HK_FY && ax > HK_GOAL_X - R) {
            g->px = sx * (HK_GOAL_X - R);
            bounce(g, -sx, 0, 0.3f, SND_POST);
        }
    } else {
        if (ax > HK_FX - R) {
            g->px = sx * (HK_FX - R);
            bounce(g, -sx, 0, RAIL_E, SND_WALL);
        }
        if (ay > HK_FY - R) {
            g->py = sy * (HK_FY - R);
            bounce(g, 0, -sy, RAIL_E, SND_WALL);
        }
        float cx = HK_FX - HK_CORNER_R, cy = HK_FY - HK_CORNER_R;
        ax = fabsf(g->px);
        ay = fabsf(g->py);
        if (ax > cx && ay > cy) {
            float dx = ax - cx, dy = ay - cy;
            float d = sqrtf(dx * dx + dy * dy), lim = HK_CORNER_R - R;
            if (d > lim) {
                float nx = dx / d, ny = dy / d;
                g->px = sx * (cx + nx * lim);
                g->py = sy * (cy + ny * lim);
                bounce(g, -nx * sx, -ny * sy, RAIL_E, SND_WALL);
            }
        }
    }
    for (int k = 0; k < 4; k++) {
        float qx = (k & 1) ? HK_GOAL_X : -HK_GOAL_X, qy = (k & 2) ? HK_FY : -HK_FY;
        float dx = g->px - qx, dy = g->py - qy;
        float d2 = dx * dx + dy * dy, lim = R + HK_POST_R;
        if (d2 < lim * lim && d2 > 1e-6f) {
            float d = sqrtf(d2), nx = dx / d, ny = dy / d;
            g->px = qx + nx * lim;
            g->py = qy + ny * lim;
            bounce(g, nx, ny, POST_E, SND_POST);
        }
    }
}

static void hit(hk_game_t *g, int side, float nx, float ny, float mvx, float mvy)
{
    float rx = g->pvx - mvx, ry = g->pvy - mvy;
    float vn = rx * nx + ry * ny;
    hk_mallet_t *m = &g->m[side];
    if (vn < 0) {
        g->pvx -= (1.0f + MALLET_E) * vn * nx;
        g->pvy -= (1.0f + MALLET_E) * vn * ny;
        float tx = -ny, ty = nx;
        float vt = rx * tx + ry * ty;
        g->pvx -= MALLET_MU * vt * tx;
        g->pvy -= MALLET_MU * vt * ty;
        float hard = -vn;
        if (hard > 90.0f && g->hit_t <= 0) {
            snd(g, hard > 1300.0f ? SND_HIT : SND_HIT_SOFT, 0.25f + hard / 3200.0f);
            g->hit_t = 0.035f;
        }
        if (hard > 1500.0f) g->shots++;
        g->hit_x = g->px - nx * HK_PUCK_R;
        g->hit_y = g->py - ny * HK_PUCK_R;
        g->hit_v = hard;
        g->hits++;
        g->rec_hit = true;
    }
    m->hit_t = 0;
    g->last_touch = side;
    if (g->state == HK_ST_SERVE) g->state = HK_ST_PLAY;
}

/* One substep of length h: the mallets go from a[] to b[] in it (their
 * speed over the step is mv[]), the puck along its velocity; every contact
 * on the way is found at its time and answered there. */
static void sweep(hk_game_t *g, const float a[2][2], const float b[2][2], const float mv[2][2], float h)
{
    float u = 0;
    for (int it = 0; it < 4 && u < 1.0f; it++) {
        float rest = 1.0f - u;
        float ex = g->px + g->pvx * h * rest, ey = g->py + g->pvy * h * rest;
        float best = 2.0f;
        int who = -1;
        bool inside = false;
        for (int s = 0; s < 2; s++) {
            float R = g->m[s].r + HK_PUCK_R;
            float max = a[s][0] + (b[s][0] - a[s][0]) * u, may = a[s][1] + (b[s][1] - a[s][1]) * u;
            float d0x = g->px - max, d0y = g->py - may;
            float dvx = (ex - g->px) - (b[s][0] - max), dvy = (ey - g->py) - (b[s][1] - may);
            float A = dvx * dvx + dvy * dvy, B = 2.0f * (d0x * dvx + d0y * dvy);
            float Cc = d0x * d0x + d0y * d0y - R * R;
            if (Cc < 0) {
                if (best > 0) {
                    best = 0;
                    who = s;
                    inside = true;
                }
                continue;
            }
            if (A < 1e-6f || B >= 0) continue;
            float disc = B * B - 4.0f * A * Cc;
            if (disc < 0) continue;
            float t = (-B - sqrtf(disc)) / (2.0f * A);
            if (t >= 0 && t <= 1.0f && t < best) {
                best = t;
                who = s;
                inside = false;
            }
        }
        if (who < 0) {
            g->px = ex;
            g->py = ey;
            return;
        }
        g->px += (ex - g->px) * best;
        g->py += (ey - g->py) * best;
        u += rest * best;
        float mx = a[who][0] + (b[who][0] - a[who][0]) * u, my = a[who][1] + (b[who][1] - a[who][1]) * u;
        float dx = g->px - mx, dy = g->py - my, d = sqrtf(dx * dx + dy * dy);
        float nx, ny;
        if (d < 1e-3f) {
            nx = 0;
            ny = -hk_dir(who);
        } else {
            nx = dx / d;
            ny = dy / d;
        }
        float R = g->m[who].r + HK_PUCK_R;
        if (inside || d < R) {
            g->px = mx + nx * (R + 0.05f);
            g->py = my + ny * (R + 0.05f);
        }
        hit(g, who, nx, ny, mv[who][0], mv[who][1]);
    }
}

static void friction(hk_game_t *g, float h)
{
    float sp = sqrtf(g->pvx * g->pvx + g->pvy * g->pvy);
    if (sp < 1e-3f) return;
    float dec = DECEL * h + DRAG * sp * h;
    float k = sp > dec ? (sp - dec) / sp : 0.0f;
    g->pvx *= k;
    g->pvy *= k;
}

/* a puck squeezed against a rail by a mallet pushes the mallet back */
static void unsqueeze(hk_game_t *g)
{
    for (int s = 0; s < 2; s++) {
        hk_mallet_t *m = &g->m[s];
        float R = m->r + HK_PUCK_R;
        float dx = g->px - m->x, dy = g->py - m->y, d2 = dx * dx + dy * dy;
        if (d2 >= R * R) continue;
        float d = sqrtf(d2);
        float nx = d > 1e-3f ? dx / d : 0, ny = d > 1e-3f ? dy / d : -hk_dir(s);
        /* the puck first, where the walls allow it ... */
        float qx = m->x + nx * R, qy = m->y + ny * R;
        float ox = g->px, oy = g->py;
        g->px = qx;
        g->py = qy;
        float vx = g->pvx, vy = g->pvy;
        walls(g);
        g->pvx = vx;
        g->pvy = vy;
        /* ... and what it could not take, the mallet */
        dx = g->px - m->x;
        dy = g->py - m->y;
        d2 = dx * dx + dy * dy;
        if (d2 < R * R) {
            m->x = g->px - nx * R;
            m->y = g->py - ny * R;
            hk_mallet_clamp(g, s, &m->x, &m->y);
        }
        (void)ox;
        (void)oy;
    }
}

/* ---- the rules ------------------------------------------------------------------------------ */

static int16_t q10(float v)
{
    return (int16_t)clampf(v * 10.0f, -32000.0f, 32000.0f);
}

static void record(hk_game_t *g)
{
    g->rec_acc += HK_DT;
    if (g->rec_acc < 1.0f / 60.0f) return;
    g->rec_acc -= 1.0f / 60.0f;
    hk_rec_t *e = &g->rec[g->rec_at];
    e->px = q10(g->px);
    e->py = q10(g->py);
    for (int s = 0; s < 2; s++) {
        e->mx[s] = q10(g->m[s].x);
        e->my[s] = q10(g->m[s].y);
    }
    e->vis = g->puck_in;
    e->hit = g->rec_hit;
    g->rec_hit = false;
    g->rec_at = (g->rec_at + 1) % HK_REC_N;
    if (g->rec_n < HK_REC_N) g->rec_n++;
}

void hk_replay_apply(hk_game_t *g, int k)
{
    if (k < 0) k = 0;
    if (k >= g->rp_len) k = g->rp_len - 1;
    if (k < 0) return;
    const hk_rec_t *e = &g->rec[(g->rp_from + k) % HK_REC_N];
    g->px = e->px * 0.1f;
    g->py = e->py * 0.1f;
    for (int s = 0; s < 2; s++) {
        g->m[s].x = e->mx[s] * 0.1f;
        g->m[s].y = e->my[s] * 0.1f;
    }
    g->puck_in = e->vis;
    if (e->hit) {
        g->hit_x = g->px;
        g->hit_y = g->py;
        g->hit_v = 2000.0f;
        g->hits++;
    }
}

static void goal(hk_game_t *g, int scorer)
{
    g->score[scorer]++;
    g->last_goal = scorer;
    g->state = HK_ST_GOAL;
    g->state_t = 0;
    snd(g, SND_GOAL, 1.0f);
    /* the replay: the last two and a half seconds */
    int n = g->rec_n < 150 ? g->rec_n : 150;
    g->rp_len = n;
    g->rp_from = (g->rec_at - n + HK_REC_N) % HK_REC_N;
    memcpy(g->m_keep, g->m, sizeof g->m);
}

static bool decided(const hk_game_t *g)
{
    if (g->goals && g->score[g->last_goal] >= g->goals) return true;
    return g->golden;
}

static void after_goal(hk_game_t *g)
{
    if (decided(g)) {
        g->state = HK_ST_END;
        g->state_t = 0;
        snd(g, SND_BUZZ_END, 1.0f);
    } else {
        /* the side that took the goal gets the puck */
        serve(g, g->last_goal ^ 1);
    }
}

void hk_replay_end(hk_game_t *g)
{
    if (g->state != HK_ST_REPLAY) return;
    memcpy(g->m, g->m_keep, sizeof g->m);
    after_goal(g);
}

bool hk_over(const hk_game_t *g)
{
    return g->state == HK_ST_END;
}

int hk_winner(const hk_game_t *g)
{
    return g->score[0] > g->score[1] ? 0 : g->score[1] > g->score[0] ? 1 : -1;
}

static void hist_push(hk_game_t *g)
{
    g->hist_at = (g->hist_at + 1) % HK_HIST;
    g->hist_x[g->hist_at] = g->px;
    g->hist_y[g->hist_at] = g->py;
    g->hist_vx[g->hist_at] = g->pvx;
    g->hist_vy[g->hist_at] = g->pvy;
}

void hk_game_step(hk_game_t *g, const hk_hand_t *hand0, const hk_hand_t *hand1)
{
    g->nsnd = g->nsnd > 12 ? 12 : g->nsnd;
    g->t += HK_DT;
    g->state_t += HK_DT;
    if (g->wall_t > 0) g->wall_t -= HK_DT;
    if (g->hit_t > 0) g->hit_t -= HK_DT;

    switch (g->state) {
    case HK_ST_GOAL:
        /* the puck drops down the slot; the mallets may still move */
        if (g->puck_in) {
            g->px += g->pvx * HK_DT;
            g->py += g->pvy * HK_DT;
            if (fabsf(g->py) > HK_FY + HK_PUCK_R * 2.5f) g->puck_in = false;
        }
        if (g->state_t > 1.4f) {
            if (decided(g) || g->rp_len < 20) after_goal(g);
            else {
                g->state = HK_ST_REPLAY;
                g->state_t = 0;
                g->rp_t = 0;
            }
        }
        record(g);
        return;
    case HK_ST_REPLAY:
        /* the app shows it (hk_replay_apply); it ends there or by a tap */
        return;
    case HK_ST_END:
        return;
    default:
        break;
    }

    /* the hands */
    hk_hand_t ai[2];
    const hk_hand_t *h[2] = { hand0, hand1 };
    for (int s = 0; s < 2; s++) {
        if (g->ai_on[s]) {
            hk_ai_hand(g, s, &ai[s]);
            h[s] = &ai[s];
        }
        mallet_aim(g, s, h[s]);
    }

    /* the mallets' moves this step, as fast as their hand allows */
    float a[2][2], b[2][2], mv[2][2];
    float most = sqrtf(g->pvx * g->pvx + g->pvy * g->pvy) * HK_DT;
    for (int s = 0; s < 2; s++) {
        hk_mallet_t *m = &g->m[s];
        float dx = m->tx - m->x, dy = m->ty - m->y, d = sqrtf(dx * dx + dy * dy);
        float lim = m->speed * HK_DT;
        if (d > lim) {
            dx *= lim / d;
            dy *= lim / d;
        }
        float nx = m->x + dx, ny = m->y + dy;
        hk_mallet_clamp(g, s, &nx, &ny);
        a[s][0] = m->x;
        a[s][1] = m->y;
        b[s][0] = nx;
        b[s][1] = ny;
        mv[s][0] = (nx - m->x) / HK_DT;
        mv[s][1] = (ny - m->y) / HK_DT;
        float mm = sqrtf((nx - m->x) * (nx - m->x) + (ny - m->y) * (ny - m->y));
        if (mm > most) most = mm;
        m->hit_t += HK_DT;
    }

    /* substeps: nothing moves more than ~5 mm in one */
    int n = 1 + (int)(most / 5.0f);
    if (n > 16) n = 16;
    float hstep = HK_DT / (float)n;
    for (int k = 1; k <= n; k++) {
        float sa[2][2], sb[2][2];
        for (int s = 0; s < 2; s++) {
            for (int c = 0; c < 2; c++) {
                sa[s][c] = a[s][c] + (b[s][c] - a[s][c]) * (float)(k - 1) / (float)n;
                sb[s][c] = a[s][c] + (b[s][c] - a[s][c]) * (float)k / (float)n;
            }
        }
        friction(g, hstep);
        sweep(g, sa, sb, mv, hstep);
        walls(g);
    }
    for (int s = 0; s < 2; s++) {
        hk_mallet_t *m = &g->m[s];
        m->x = b[s][0];
        m->y = b[s][1];
        m->vx = mv[s][0];
        m->vy = mv[s][1];
    }
    unsqueeze(g);

    float sp = sqrtf(g->pvx * g->pvx + g->pvy * g->pvy);
    float vmax = V_MAX * g->puck_speed;
    if (sp > vmax) {
        g->pvx *= vmax / sp;
        g->pvy *= vmax / sp;
        sp = vmax;
    }
    if (sp > g->max_speed) g->max_speed = sp;
    hist_push(g);
    record(g);

    /* a goal: the puck wholly past the line, down the slot */
    if (fabsf(g->py) > HK_FY + HK_PUCK_R && fabsf(g->px) < HK_GOAL_X) {
        goal(g, g->py > 0 ? 1 : 0);
        return;
    }
    g->near_goal = clampf((fabsf(g->py) - 200.0f) / 320.0f, 0.0f, 1.0f);

    /* a puck that sits: served again to the side whose half it is in */
    if (sp < SLOW && (g->state == HK_ST_PLAY || g->state_t > 8.0f)) {
        g->idle_t += HK_DT;
        if (g->idle_t > IDLE_S) {
            g->rescues++;
            serve(g, g->py > 0 ? 0 : 1);
        }
    } else {
        g->idle_t = 0;
    }

    /* the clock runs from the first touch: what is left of the time, or what was played */
    if (g->state == HK_ST_PLAY && !g->golden) {
        g->clock += HK_DT;
        if (g->minutes > 0 && g->clock >= (float)g->minutes * 60.0f) {
            g->clock = (float)g->minutes * 60.0f;
            if (g->score[0] == g->score[1]) {
                g->golden = true;
                snd(g, SND_BUZZ, 0.8f);
            } else {
                g->state = HK_ST_END;
                g->state_t = 0;
                snd(g, SND_BUZZ_END, 1.0f);
            }
        }
    }
}
