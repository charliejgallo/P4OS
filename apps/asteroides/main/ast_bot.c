/*
 * DERIVA - the bot (see ast_bot.h). It plays with the stick: a
 * heading and how hard to push, the fire button and the jump.
 */
#include "ast_bot.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265f

void ast_bot_init(ast_bot_t *b, int skill, uint32_t seed)
{
    memset(b, 0, sizeof *b);
    b->skill = skill;
    b->rnd = seed ? seed : 77;
    if (skill == BOT_PERSON) {
        b->react = 0.22f;
        b->think = 0.15f;
        b->attention = 0.8f;
        b->aim_err = 0.13f;
        b->jump_k = 0.5f;
        b->fire_hold = 0.85f;
    } else {
        b->react = 0.06f;
        b->think = 0.05f;
        b->attention = 1.0f;
        b->aim_err = 0.03f;
        b->jump_k = 0.9f;
        b->fire_hold = 1.0f;
    }
}

static float rnd(ast_bot_t *b)
{
    return ag_rand01(&b->rnd);
}

static float gauss(ast_bot_t *b)
{
    return (rnd(b) + rnd(b) + rnd(b) - 1.5f) * 1.4f;
}

typedef struct {
    float t;            /* when it comes closest                        */
    float miss;         /* how close, minus what it takes to touch      */
    float dx, dy;       /* where it is                                  */
    float vx, vy;       /* how it moves, from the ship                  */
} threat_t;

/* when and how near something at (x, y) moving (vx, vy) passes the ship */
static void approach(const ag_game_t *g, const ag_ship_t *sh, float x, float y, float vx, float vy, float r, threat_t *o)
{
    float dx = ag_wrapd(x - sh->x, g->W), dy = ag_wrapd(y - sh->y, g->H);
    float rvx = vx - sh->vx, rvy = vy - sh->vy;
    float v2 = rvx * rvx + rvy * rvy;
    float t = v2 > 1 ? -(dx * rvx + dy * rvy) / v2 : 0;
    if (t < 0) t = 0;
    float cx = dx + rvx * t, cy = dy + rvy * t;
    o->t = t;
    o->miss = sqrtf(cx * cx + cy * cy) - r - AG_SHIP_R;
    o->dx = dx;
    o->dy = dy;
    o->vx = rvx;
    o->vy = rvy;
}

static bool worse(const threat_t *a, const threat_t *b)
{
    /* sooner and closer is worse */
    return a->t + a->miss * 0.004f < b->t + b->miss * 0.004f;
}

static void decide(ast_bot_t *b, const ag_game_t *g, int s, ag_hand_t *h)
{
    const ag_ship_t *sh = &g->ship[s];
    memset(h, 0, sizeof *h);
    h->aim = true;
    if (!sh->alive) return;

    /* the worst danger in the next second */
    threat_t worst = { 99, 99, 0, 0, 0, 0 };
    bool danger = false;
    threat_t th;
    for (int i = 0; i < AG_ROCKS; i++) {
        const ag_rock_t *r = &g->rock[i];
        if (!r->on || r->stuck >= 0) continue;
        approach(g, sh, r->x, r->y, r->vx, r->vy, r->r * 0.9f, &th);
        if (th.t < 1.0f && th.miss < 14 && (!danger || worse(&th, &worst))) {
            worst = th;
            danger = true;
        }
    }
    for (int i = 0; i < AG_ENEMIES; i++) {
        const ag_enemy_t *e = &g->en[i];
        if (!e->on) continue;
        approach(g, sh, e->x, e->y, e->vx, e->vy, 16, &th);
        if (th.t < 1.0f && th.miss < 18 && (!danger || worse(&th, &worst))) {
            worst = th;
            danger = true;
        }
    }
    for (int i = 0; i < AG_EBULS; i++) {
        const ag_ebul_t *e = &g->ebul[i];
        if (!e->on) continue;
        approach(g, sh, e->x, e->y, e->vx, e->vy, e->kind == EB_MINE ? 14 : 7, &th);
        if (th.t < 0.9f && th.miss < 10 && (!danger || worse(&th, &worst))) {
            worst = th;
            danger = true;
        }
    }
    if (g->boss.on) {
        const ag_boss_t *bo = &g->boss;
        float bvx = 0, bvy = 0;
        if (bo->kind == BOSS_SERPIENTE) {
            bvx = cosf(bo->ang) * 140.0f;
            bvy = sinf(bo->ang) * 140.0f;
        } else if (bo->kind == BOSS_COLOSO) {
            bvx = bo->vx;
            bvy = bo->vy;
        }
        approach(g, sh, bo->x, bo->y, bvx, bvy, bo->r + 10, &th);
        if (th.t < 1.0f && th.miss < 30 && (!danger || worse(&th, &worst))) {
            worst = th;
            danger = true;
        }
        if (bo->kind == BOSS_SERPIENTE) {
            for (int k = 0; k < AG_SEGS; k++) {
                if (bo->seg_hp[k] <= 0) continue;
                int i = (bo->trail_i - (k + 1) * 22) & (AG_TRAIL - 1);
                approach(g, sh, bo->trail_x[i], bo->trail_y[i], 0, 0, 18, &th);
                if (th.t < 1.0f && th.miss < 16 && (!danger || worse(&th, &worst))) {
                    worst = th;
                    danger = true;
                }
            }
        }
    }
    /* a person misses some of them */
    if (danger && rnd(b) > b->attention) danger = false;

    if (danger) {
        bool soon = worst.t < 0.4f && worst.miss < 4;
        if (soon && sh->jump_cd <= 0 && !b->jump_sent && rnd(b) < b->jump_k) {
            h->jump = true;
            b->jump_sent = true;
            return;
        }
        /* out of its way: across its path, on the side it is not on */
        float px = -worst.vy, py = worst.vx;
        float l = sqrtf(px * px + py * py);
        if (l < 1) {
            px = -worst.dy;
            py = worst.dx;
            l = sqrtf(px * px + py * py) + 1e-3f;
        }
        px /= l;
        py /= l;
        if (px * worst.dx + py * worst.dy > 0) {
            px = -px;
            py = -py;
        }
        float ang = atan2f(py, px) + gauss(b) * b->aim_err * 2.0f;
        h->ax = cosf(ang);
        h->ay = sinf(ang);
        h->thrust = 1.0f;
        float facing = cosf(ang - sh->ang);
        h->thrust = facing > 0.3f ? 1.0f : 0.2f;
        h->fire = rnd(b) < b->fire_hold;
        return;
    }
    b->jump_sent = false;

    /* something to shoot: the nearest, led a little */
    float best = 1e12f, tx = 0, ty = 0;
    bool have = false;
    float bv = 700.0f + 50.0f * (float)g->upg[UP_GUN];
    for (int i = 0; i < AG_ROCKS; i++) {
        const ag_rock_t *r = &g->rock[i];
        if (!r->on || r->stuck >= 0) continue;
        float dx = ag_wrapd(r->x - sh->x, g->W), dy = ag_wrapd(r->y - sh->y, g->H);
        float d2 = dx * dx + dy * dy;
        /* a volatile rock near the ship is left alone */
        if (r->type == RK_VOLATILE && d2 < 170.0f * 170.0f) continue;
        if (d2 < best) {
            float t = sqrtf(d2) / bv;
            best = d2;
            tx = dx + (r->vx - sh->vx * 0.5f) * t;
            ty = dy + (r->vy - sh->vy * 0.5f) * t;
            have = true;
        }
    }
    for (int i = 0; i < AG_ENEMIES; i++) {
        const ag_enemy_t *e = &g->en[i];
        if (!e->on) continue;
        float dx = ag_wrapd(e->x - sh->x, g->W), dy = ag_wrapd(e->y - sh->y, g->H);
        float d2 = (dx * dx + dy * dy) * 0.5f;
        if (d2 < best) {
            float t = sqrtf(d2) / bv;
            best = d2;
            tx = dx + e->vx * t;
            ty = dy + e->vy * t;
            have = true;
        }
    }
    if (g->boss.on && g->boss.state) {
        const ag_boss_t *bo = &g->boss;
        float x = bo->x, y = bo->y;
        if (bo->kind == BOSS_COLMENA) {
            for (int p = 0; p < 3; p++) {
                if (bo->part_hp[p] > 0) {
                    float pa = bo->ang + (float)p * 2.0943951f;
                    x = bo->x + cosf(pa) * bo->r;
                    y = bo->y + sinf(pa) * bo->r;
                    break;
                }
            }
        } else if (bo->kind == BOSS_SERPIENTE) {
            for (int k = AG_SEGS - 1; k >= 0; k--) {
                if (bo->seg_hp[k] > 0) {
                    int i = (bo->trail_i - (k + 1) * 22) & (AG_TRAIL - 1);
                    x = bo->trail_x[i];
                    y = bo->trail_y[i];
                    break;
                }
            }
        }
        float dx = ag_wrapd(x - sh->x, g->W), dy = ag_wrapd(y - sh->y, g->H);
        float d2 = (dx * dx + dy * dy) * 0.3f;
        if (d2 < best) {
            best = d2;
            tx = dx;
            ty = dy;
            have = true;
        }
    }
    float dist = sqrtf(best);
    if (have) {
        float ang = atan2f(ty, tx) + gauss(b) * b->aim_err;
        h->ax = cosf(ang);
        h->ay = sinf(ang);
        float off = fabsf(atan2f(sinf(ang - sh->ang), cosf(ang - sh->ang)));
        h->fire = rnd(b) < b->fire_hold || off < 0.2f;
        /* close in when far, back off a little when near, mostly drift */
        float sp = sqrtf(sh->vx * sh->vx + sh->vy * sh->vy);
        if (dist > 330 && sp < 120 && off < 0.4f) h->thrust = 0.6f;
        else h->thrust = 0;
        /* the stick pushed only part way only turns */
        float k = h->thrust > 0 ? 1.0f : 0.5f;
        h->ax *= k;
        h->ay *= k;
    }
    /* crystals and powers when it is calm */
    if (!have || dist > 260) {
        float cb = 1e12f, cx = 0, cy = 0;
        bool c = false;
        for (int i = 0; i < AG_PICKS; i++) {
            const ag_pick_t *p = &g->pick[i];
            if (!p->on) continue;
            float dx = ag_wrapd(p->x - sh->x, g->W), dy = ag_wrapd(p->y - sh->y, g->H);
            float d2 = (dx * dx + dy * dy) * 0.4f;
            if (d2 < cb) { cb = d2; cx = dx; cy = dy; c = true; }
        }
        for (int i = 0; i < AG_CRYST; i++) {
            const ag_cryst_t *p = &g->cryst[i];
            if (!p->on) continue;
            float dx = ag_wrapd(p->x - sh->x, g->W), dy = ag_wrapd(p->y - sh->y, g->H);
            float d2 = dx * dx + dy * dy;
            if (d2 < cb) { cb = d2; cx = dx; cy = dy; c = true; }
        }
        if (c && cb < 360.0f * 360.0f) {
            float ang = atan2f(cy, cx);
            h->ax = cosf(ang);
            h->ay = sinf(ang);
            float off = fabsf(atan2f(sinf(ang - sh->ang), cosf(ang - sh->ang)));
            float sp = sqrtf(sh->vx * sh->vx + sh->vy * sh->vy);
            h->thrust = off < 0.5f && sp < 200 ? 0.7f : 0;
        }
    }
    /* too fast: turn round and brake */
    float sp = sqrtf(sh->vx * sh->vx + sh->vy * sh->vy);
    if (sp > 250 && rnd(b) < 0.5f) {
        float ang = atan2f(-sh->vy, -sh->vx);
        h->ax = cosf(ang);
        h->ay = sinf(ang);
        h->thrust = cosf(ang - sh->ang) > 0.8f ? 0.8f : 0;
    }
}

void ast_bot_hand(ast_bot_t *b, const ag_game_t *g, int s, ag_hand_t *out)
{
    b->t += AG_DT;
    if (b->t >= b->next) {
        b->next = b->t + b->think * (0.7f + 0.6f * rnd(b));
        if (b->qn < 32) {
            decide(b, g, s, &b->q[b->qn]);
            b->q_t[b->qn] = b->t + b->react;
            b->qn++;
        }
    }
    /* what was decided a reaction ago reaches the thumb now */
    bool jump = false;
    while (b->qn && b->q_t[0] <= b->t) {
        jump |= b->q[0].jump;
        b->now = b->q[0];
        memmove(b->q, b->q + 1, sizeof b->q[0] * (size_t)(b->qn - 1));
        memmove(b->q_t, b->q_t + 1, sizeof b->q_t[0] * (size_t)(b->qn - 1));
        b->qn--;
    }
    *out = b->now;
    out->jump = jump;
    b->now.jump = false;
}
