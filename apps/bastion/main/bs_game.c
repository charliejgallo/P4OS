/*
 * BASTION - a battle (see bs_game.h)
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "bs_game.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265f
#define SCREEN_X(x) (360.0f + (x) * BS_PX)
#define SCREEN_Y(y, z) (1280.0f - (y) * BS_ROW_Y - (z) * BS_ROW_Z)

/* ---- small things ---------------------------------------------------------------- */

static uint32_t rnd(bs_game_t *g)
{
    uint32_t x = g->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return g->rng = x;
}

static float frand(bs_game_t *g)
{
    return (float)(rnd(g) & 0xFFFF) / 65536.0f;
}

static float fr(bs_game_t *g, float a, float b)
{
    return a + (b - a) * frand(g);
}

static float fmin2(float a, float b) { return a < b ? a : b; }
static float fmax2(float a, float b) { return a > b ? a : b; }

static uint16_t c565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

void bs_game_sound(bs_game_t *g, int snd)
{
    if (snd < 0 || snd >= SND_N || g->snd_cool[snd] > 0 || g->nsnd >= (int)sizeof g->snd) return;
    /* the quick ones would pile up into a buzz */
    static const float GAP[SND_N] = {
        [SND_GATLING] = 0.07f, [SND_LASER] = 0.24f, [SND_FLAME] = 0.24f, [SND_HIT] = 0.06f, [SND_DIE1] = 0.05f,
        [SND_DIE2] = 0.07f, [SND_DIE3] = 0.05f, [SND_BOOM_S] = 0.08f, [SND_BOOM_M] = 0.1f, [SND_ZAP] = 0.1f,
        [SND_DRONE] = 0.2f, [SND_COIN] = 0.1f, [SND_CRYO] = 2.5f, [SND_FORT_HIT] = 0.25f, [SND_ALARM] = 4.0f,
    };
    g->snd_cool[snd] = GAP[snd] > 0 ? GAP[snd] : 0.03f;
    g->snd[g->nsnd++] = (uint8_t)snd;
}

static void part(bs_game_t *g, float sx, float sy, float vx, float vy, float life, uint16_t color, int r, int grav)
{
    bs_part_t *p;
    if (g->npart < BS_MAX_PARTS) p = &g->part[g->npart++];
    else p = &g->part[rnd(g) % BS_MAX_PARTS];
    p->x = sx;
    p->y = sy;
    p->vx = vx;
    p->vy = vy;
    p->life = p->max = life;
    p->color = color;
    p->r = (uint8_t)r;
    p->grav = (uint8_t)grav;
}

static void burst(bs_game_t *g, float x, float y, float z, int n, uint32_t color, float speed, int r)
{
    float sx = SCREEN_X(x), sy = SCREEN_Y(y, z);
    uint16_t c = c565(color);
    for (int i = 0; i < n; i++) {
        float a = fr(g, 0, 2 * PI), v = fr(g, 0.3f, 1.0f) * speed;
        part(g, sx, sy, cosf(a) * v, sinf(a) * v * 0.8f - speed * 0.3f, fr(g, 0.25f, 0.6f), c, r, 1);
    }
}

static bs_fx_t *fx_new(bs_game_t *g, int kind, float life, uint32_t color)
{
    bs_fx_t *best = &g->fx[0];
    for (int i = 0; i < BS_MAX_FX; i++) {
        bs_fx_t *f = &g->fx[i];
        if (!f->on) {
            best = f;
            break;
        }
        /* all busy: the one nearest its end goes */
        if (f->life - f->t < best->life - best->t) best = f;
    }
    memset(best, 0, sizeof *best);
    best->on = 1;
    best->kind = (uint8_t)kind;
    best->life = life;
    best->color = color;
    return best;
}

static void ring(bs_game_t *g, float x, float y, float r0, float r1, float life, uint32_t color)
{
    bs_fx_t *f = fx_new(g, FX_RING, life, color);
    f->x0 = x;
    f->y0 = y;
    f->x1 = r0;
    f->r = r1;
}

static void float_text(bs_game_t *g, float x, float y, const char *text, uint32_t color)
{
    bs_float_t *best = &g->fl[0];
    for (int i = 0; i < BS_MAX_FLOATS; i++) {
        if (g->fl[i].t <= 0) {
            best = &g->fl[i];
            break;
        }
        if (g->fl[i].t < best->t) best = &g->fl[i];
    }
    best->x = x;
    best->y = y;
    best->t = 1.1f;
    best->color = c565(color);
    snprintf(best->text, sizeof best->text, "%.13s", text);
}

/* ---- what the laboratory changes ------------------------------------------------------ */

int bs_game_cost(const bs_game_t *g, int type, int tier)
{
    int c = bs_tier_cost(type, tier);
    return (c * (100 - 3 * g->kit.mastery[type]) + 50) / 100;
}

float bs_game_range(const bs_game_t *g, int type, int tier)
{
    return bs_tier_range(type, tier) * g->k_range;
}

float bs_game_dmg(const bs_game_t *g, int type, int tier)
{
    return bs_tier_dmg(type, tier) * g->k_dmg * (1.0f + 0.08f * (float)g->kit.mastery[type]) *
           (1.0f + 0.01f * (float)(g->kit.rank - 1));
}

int bs_game_sell_value(const bs_game_t *g, int pad)
{
    const bs_tower_t *t = &g->tw[pad];
    if (!t->on) return 0;
    /* a tower put down between waves and not yet used goes back whole */
    return (int)((float)t->invested * g->k_sell + 0.5f);
}

/* ---- aliens ------------------------------------------------------------------------- */

static const bs_path_t *road(const bs_game_t *g, const bs_alien_t *a)
{
    return a->fly ? &g->w->air[a->path] : &g->w->path[a->path];
}

static void alien_place(bs_game_t *g, bs_alien_t *a)
{
    const bs_path_t *p = road(g, a);
    float x, y, dx, dy;
    bs_path_at(p, a->s, &x, &y, &dx, &dy);
    a->x = x - dy * a->off;
    a->y = y + dx * a->off;
    a->dx = dx;
    a->dy = dy;
    a->left = p->len - a->s;
    float ang = atan2f(dy, dx);
    if (ang < 0) ang += 2 * PI;
    a->dir8 = (uint8_t)((int)(ang / (PI / 4) + 0.5f) & 7);
}

static int alien_spawn(bs_game_t *g, int type, int path, float s, float hpk)
{
    for (int i = 0; i < BS_MAX_ALIENS; i++) {
        bs_alien_t *a = &g->al[i];
        if (a->on) continue;
        const bs_alien_def_t *d = &BS_ALIEN[type];
        memset(a, 0, sizeof *a);
        a->on = 1;
        a->type = (uint8_t)type;
        a->path = (uint8_t)path;
        a->fly = d->fly;
        a->id = ++g->next_id;
        if (!a->id) a->id = ++g->next_id;
        a->hpk = hpk;
        a->hp = a->maxhp = d->hp * hpk;
        a->shield = a->maxshield = d->shield * hpk;
        a->s = s;
        a->off = d->boss ? 0.0f : fr(g, -0.72f, 0.72f) * (type == AL_BRUTE ? 0.4f : 1.0f);
        a->anim = frand(g) * 4.0f;
        a->ability_t = fr(g, 2.0f, 4.0f);
        a->cloak_t = fr(g, 2.0f, 3.5f);
        alien_place(g, a);
        g->alive++;
        return i;
    }
    return -1;
}

static void alien_die(bs_game_t *g, int ai, int ti)
{
    bs_alien_t *a = &g->al[ai];
    const bs_alien_def_t *d = &BS_ALIEN[a->type];
    int reward = (int)((float)d->reward * g->k_bounty * g->k_reward + 0.5f);
    g->credits += reward;
    g->earned += reward;
    g->xp += d->xp;
    g->kills++;
    if (ti >= 0 && ti < BS_PADS) g->tw[ti].kills++;
    float z = d->alt + 0.4f;
    burst(g, a->x, a->y, z, d->boss ? 60 : (a->type == AL_BRUTE || a->type == AL_CARRIER ? 26 : 9), d->color,
          d->boss ? 300.0f : 150.0f, d->boss ? 4 : 3);
    if (d->boss) {
        bs_fx_t *f = fx_new(g, FX_BLAST, 0.8f, d->color);
        f->x0 = a->x;
        f->y0 = a->y;
        f->z0 = 1.2f;
        f->r = 4.0f;
        ring(g, a->x, a->y, 0.5f, 6.0f, 0.7f, 0xFFFFFF);
        g->shake = 0.6f;
        bs_game_sound(g, SND_BOSS_DIE);
        char t[14];
        snprintf(t, sizeof t, "+%d", reward);
        float_text(g, a->x, a->y + 1.0f, t, 0xFFE040);
    } else {
        bs_game_sound(g, a->type == AL_SWARM || a->type == AL_MINI ? SND_DIE3 : (a->id & 1 ? SND_DIE1 : SND_DIE2));
        if (reward >= 12) {
            char t[14];
            snprintf(t, sizeof t, "+%d", reward);
            float_text(g, a->x, a->y + 0.6f, t, 0xFFE040);
        }
    }
    int type = a->type;
    int path = a->path;
    float s = a->s, hpk = a->hpk;
    a->on = 0;
    g->alive--;
    if (type == AL_SPLIT) {
        for (int k = 0; k < 3; k++) {
            int n = alien_spawn(g, AL_MINI, path, fmax2(0, s - 0.5f + 0.5f * (float)k), hpk);
            if (n >= 0) g->al[n].off = -0.6f + 0.6f * (float)k;
        }
    }
}

/* dot: damage over time (a beam, a fire), which armour only thins */
static void hurt(bs_game_t *g, int ai, float dmg, bool dot, int ti)
{
    bs_alien_t *a = &g->al[ai];
    if (!a->on || dmg <= 0) return;
    const bs_alien_def_t *d = &BS_ALIEN[a->type];
    if (!dot && g->k_crit > 0 && frand(g) < g->k_crit) {
        dmg *= 2.0f;
        part(g, SCREEN_X(a->x), SCREEN_Y(a->y, d->alt + 0.8f), 0, -60, 0.4f, 0xFFE0, 4, 0);
    }
    if (a->shield > 0) {
        a->shield_t = 3.0f;
        if (dmg <= a->shield) {
            a->shield -= dmg;
            a->flash_t = 0.05f;
            return;
        }
        dmg -= a->shield;
        a->shield = 0;
    }
    a->shield_t = 3.0f;
    if (d->armor) {
        if (dot) dmg *= 1.0f - 0.035f * (float)d->armor;
        else dmg = fmax2(dmg * 0.2f, dmg - (float)d->armor);
    }
    a->hp -= dmg;
    if (!dot) a->flash_t = 0.09f;
    if (d->boss && a->maxhp > 0) {
        /* what it has lost so far, in twentieths: each one pays */
        int due = (int)((1.0f - (a->hp > 0 ? a->hp : 0) / a->maxhp) * (float)BS_BOSS_PARTS);
        if (due > BS_BOSS_PARTS) due = BS_BOSS_PARTS;
        int n = 0;
        while (a->paid < due) {
            a->paid++;
            n++;
        }
        if (n) {
            int pay = (int)((float)(n * BS_BOSS_PART) * g->k_bounty * g->k_reward + 0.5f);
            g->credits += pay;
            g->earned += pay;
            char t[14];
            snprintf(t, sizeof t, "+%d", pay);
            float_text(g, a->x, a->y + 1.6f, t, 0xFFE040);
            burst(g, a->x, a->y, d->alt + 1.2f, 6, 0xFFD83A, 150.0f, 3);
            bs_game_sound(g, SND_COIN);
        }
    }
    if (a->hp <= 0) alien_die(g, ai, ti);
}

static void blast(bs_game_t *g, float x, float y, float r, float dmg, bool air, int ti)
{
    for (int i = 0; i < BS_MAX_ALIENS; i++) {
        bs_alien_t *a = &g->al[i];
        if (!a->on || (a->fly && !air)) continue;
        float dx = a->x - x, dy = a->y - y;
        float rr = r + BS_ALIEN[a->type].radius * 0.5f;
        float d2 = dx * dx + dy * dy;
        if (d2 > rr * rr) continue;
        float k = 1.0f - 0.5f * sqrtf(d2) / rr;
        hurt(g, i, dmg * k, false, ti);
    }
}

static void explode(bs_game_t *g, float x, float y, float z, float r, uint32_t color, int snd)
{
    bs_fx_t *f = fx_new(g, FX_BLAST, 0.38f, color);
    f->x0 = x;
    f->y0 = y;
    f->z0 = z;
    f->r = r;
    burst(g, x, y, z + 0.2f, (int)(6 + r * 8), color, 120.0f + r * 60.0f, 3);
    burst(g, x, y, z + 0.2f, (int)(3 + r * 3), 0xFFFFFF, 90.0f, 2);
    if (r >= 1.6f) ring(g, x, y, 0.3f, r, 0.3f, color);
    bs_game_sound(g, snd);
}

static void leak(bs_game_t *g, int ai)
{
    bs_alien_t *a = &g->al[ai];
    const bs_alien_def_t *d = &BS_ALIEN[a->type];
    g->lives -= d->lives;
    g->leaked++;
    g->leak_by[a->type]++;
    if (g->lives < 0) g->lives = 0;
    g->fort_flash = 0.5f;
    g->shake = fmax2(g->shake, d->boss ? 0.7f : 0.25f);
    burst(g, g->w->fort_x, g->w->fort_y, 1.5f, 14, 0xFF6030, 160.0f, 3);
    bs_game_sound(g, SND_FORT_HIT);
    if (g->lives > 0 && g->lives * 4 <= g->max_lives) bs_game_sound(g, SND_ALARM);
    a->on = 0;
    g->alive--;
}

static void aliens_step(bs_game_t *g)
{
    const float dt = BS_DT;
    bool frozen = g->freeze_t > 0;
    /* who stands inside an amplifier's reach is seen by every tower */
    for (int i = 0; i < BS_MAX_ALIENS; i++) g->al[i].seen = 0;
    for (int t = 0; t < BS_PADS; t++) {
        const bs_tower_t *tw = &g->tw[t];
        if (!tw->on || tw->type != TW_RADAR || tw->off_t > 0) continue;
        float r = bs_game_range(g, TW_RADAR, tw->tier) + 1.5f;
        for (int i = 0; i < BS_MAX_ALIENS; i++) {
            bs_alien_t *a = &g->al[i];
            if (!a->on) continue;
            float dx = a->x - tw->x, dy = a->y - tw->y;
            if (dx * dx + dy * dy < r * r) a->seen = 1;
        }
    }
    for (int i = 0; i < BS_MAX_ALIENS; i++) {
        bs_alien_t *a = &g->al[i];
        if (!a->on) continue;
        const bs_alien_def_t *d = &BS_ALIEN[a->type];
        if (a->flash_t > 0) a->flash_t -= dt;
        if (a->slow_t > 0) {
            a->slow_t -= dt;
            if (a->slow_t <= 0) a->slow = 0;
        }
        if (a->drag_t > 0) a->drag_t -= dt;
        if (a->stun_t > 0) a->stun_t -= dt;
        if (a->burn_t > 0) {
            a->burn_t -= dt;
            hurt(g, i, a->burn * dt, true, -1);
            if (!a->on) continue;
            if ((rnd(g) & 7) == 0)
                part(g, SCREEN_X(a->x) + fr(g, -8, 8), SCREEN_Y(a->y, d->alt + 0.5f), fr(g, -15, 15), -70, 0.35f, 0xFC40, 3, 0);
        }
        if (a->maxshield > 0) {
            if (a->shield_t > 0) a->shield_t -= dt;
            else if (a->shield < a->maxshield) a->shield = fmin2(a->maxshield, a->shield + a->maxshield * 0.3f * dt);
        }
        /* what each kind does on its own */
        a->ability_t -= dt;
        switch (a->type) {
        case AL_PHASE:
            a->cloak_t -= dt;
            if (a->cloak_t <= 0) {
                a->cloaked = !a->cloaked;
                a->cloak_t = a->cloaked ? 2.6f : 3.2f;
            }
            break;
        case AL_HEALER:
        case AL_OVERMIND:
            if (a->ability_t <= 0) {
                a->ability_t = a->type == AL_HEALER ? 1.0f : 8.0f;
                float r = a->type == AL_HEALER ? 2.6f : 3.5f;
                bool any = false;
                for (int k = 0; k < BS_MAX_ALIENS; k++) {
                    bs_alien_t *o = &g->al[k];
                    if (!o->on || k == i || o->hp >= o->maxhp) continue;
                    float dx = o->x - a->x, dy = o->y - a->y;
                    if (dx * dx + dy * dy > r * r) continue;
                    o->hp = fmin2(o->maxhp, o->hp + (a->type == AL_HEALER ? 16.0f * a->hpk : o->maxhp * 0.12f));
                    any = true;
                }
                if (any) ring(g, a->x, a->y, 0.3f, r, 0.45f, 0x60FFA0);
                if (a->type == AL_OVERMIND && !frozen && a->left > 6.0f) {
                    /* it blinks forward */
                    ring(g, a->x, a->y, 1.6f, 0.2f, 0.3f, 0xC080FF);
                    a->s += 3.0f;
                    alien_place(g, a);
                    ring(g, a->x, a->y, 0.2f, 1.8f, 0.35f, 0xC080FF);
                    bs_game_sound(g, SND_GRAVITY);
                }
            }
            break;
        case AL_JAMMER:
        case AL_TITAN:
            if (a->ability_t <= 0 && !frozen && a->stun_t <= 0) {
                a->ability_t = a->type == AL_JAMMER ? 5.0f : 7.0f;
                float r = a->type == AL_JAMMER ? 2.8f : 3.8f;
                bool any = false;
                for (int t = 0; t < BS_PADS; t++) {
                    bs_tower_t *tw = &g->tw[t];
                    if (!tw->on) continue;
                    float dx = tw->x - a->x, dy = tw->y - a->y;
                    if (dx * dx + dy * dy > r * r) continue;
                    tw->off_t = a->type == AL_JAMMER ? 2.0f : 2.6f;
                    any = true;
                }
                if (any || a->type == AL_TITAN) {
                    ring(g, a->x, a->y, 0.3f, r, 0.4f, a->type == AL_JAMMER ? 0xFF40C0 : 0xFF8040);
                    if (a->type == AL_TITAN) {
                        g->shake = fmax2(g->shake, 0.35f);
                        bs_game_sound(g, SND_BOOM_M);
                    } else {
                        bs_game_sound(g, SND_DENY);
                    }
                }
            }
            break;
        case AL_CARRIER:
        case AL_QUEEN:
            if (a->ability_t <= 0 && !frozen) {
                a->ability_t = a->type == AL_CARRIER ? 4.0f : 3.5f;
                const bs_path_t *gp = &g->w->path[a->path];
                float s = a->fly ? gp->len * (a->s / fmax2(1.0f, road(g, a)->len)) : a->s;
                for (int k = 0; k < 2; k++) {
                    if (g->alive >= BS_MAX_ALIENS - 20) break;
                    alien_spawn(g, a->type == AL_CARRIER ? AL_SWARM : AL_GRUB, a->path,
                                fmax2(0, s + (a->fly ? 0.0f : 1.2f) - 0.5f * (float)k), a->hpk);
                }
            }
            break;
        default: break;
        }
        /* walking */
        float v = d->speed;
        float slow = a->slow;
        if (a->drag_t > 0 && slow < 0.25f) slow = 0.25f + slow * 0.5f;
        if (d->boss && slow > 0.3f) slow = 0.3f;
        v *= 1.0f - slow;
        if (a->stun_t > 0 || frozen) v = 0;
        a->s += v * dt;
        a->anim += dt * (0.5f + v * 2.6f) * (d->fly ? 2.0f : 1.0f) + (d->fly ? dt * 4.0f : 0.0f);
        alien_place(g, a);
        if (!((g->seen >> a->type) & 1) && a->y < 38.6f && a->x > -9.6f && a->x < 9.6f) g->seen |= 1u << a->type;
        if (a->left <= (d->boss ? 1.6f : 0.4f)) leak(g, i);
    }
}

/* ---- towers ------------------------------------------------------------------------ */

static bool on_screen(const bs_alien_t *a)
{
    return a->y < 38.6f && a->y > 4.5f && a->x > -9.6f && a->x < 9.6f;
}

static bool can_hit(const bs_alien_t *a, bool air, bool ground)
{
    if (!a->on || (a->fly ? !air : !ground)) return false;
    if (a->cloaked && !a->seen) return false;
    return on_screen(a);
}

static int acquire(bs_game_t *g, float x, float y, float range, float min_r, bool air, bool ground, int mode, bool air_first)
{
    int best = -1;
    float score = 0;
    bool best_air = false;
    for (int i = 0; i < BS_MAX_ALIENS; i++) {
        const bs_alien_t *a = &g->al[i];
        if (!can_hit(a, air, ground)) continue;
        float dx = a->x - x, dy = a->y - y;
        float d2 = dx * dx + dy * dy;
        float rr = range + BS_ALIEN[a->type].radius * 0.5f;
        if (d2 > rr * rr || d2 < min_r * min_r) continue;
        float sc;
        switch (mode) {
        case TM_STRONG: sc = a->hp + a->shield; break;
        case TM_WEAK:   sc = -(a->hp + a->shield); break;
        case TM_NEAR:   sc = -d2; break;
        default:        sc = -a->left; break;
        }
        bool fl = air_first && a->fly;
        if (best < 0 || (fl && !best_air) || (fl == best_air && sc > score)) {
            best = i;
            score = sc;
            best_air = fl;
        }
    }
    return best;
}

static bool target_ok(bs_game_t *g, bs_tower_t *t, float range, float min_r, bool air, bool ground)
{
    if (t->target < 0) return false;
    const bs_alien_t *a = &g->al[t->target];
    if (a->id != t->tid || !can_hit(a, air, ground)) return false;
    float dx = a->x - t->x, dy = a->y - t->y;
    float d2 = dx * dx + dy * dy;
    float rr = range + BS_ALIEN[a->type].radius * 0.5f + 0.2f;
    return d2 <= rr * rr && d2 >= min_r * min_r;
}

/* turns the tower towards (x, y); returns how far off it still is */
static float aim(bs_tower_t *t, float x, float y, float rate)
{
    float want = atan2f(y - t->y, x - t->x);
    float d = want - t->yaw;
    while (d > PI) d -= 2 * PI;
    while (d < -PI) d += 2 * PI;
    float step = rate * BS_DT;
    if (d > step) t->yaw += step;
    else if (d < -step) t->yaw -= step;
    else t->yaw = want;
    if (t->yaw < 0) t->yaw += 2 * PI;
    if (t->yaw >= 2 * PI) t->yaw -= 2 * PI;
    return fabsf(d) < step ? 0.0f : fabsf(d) - step;
}

static void muzzle(const bs_tower_t *t, float *x, float *y, float *z)
{
    const bs_tower_def_t *d = &BS_TOWER[t->type];
    float m = (float)d->muzzle * 0.1f;
    *x = t->x + cosf(t->yaw) * m;
    *y = t->y + sinf(t->yaw) * m;
    *z = (float)d->height * 0.1f + 0.12f * (float)t->tier;
}

static bs_shot_t *shot_new(bs_game_t *g, int kind, const bs_tower_t *t, int ti)
{
    for (int i = 0; i < BS_MAX_SHOTS; i++) {
        bs_shot_t *s = &g->shot[i];
        if (s->on) continue;
        memset(s, 0, sizeof *s);
        s->on = 1;
        s->kind = (uint8_t)kind;
        s->from = (uint8_t)ti;
        s->tier = t->tier;
        s->target = -1;
        muzzle(t, &s->x, &s->y, &s->z);
        return s;
    }
    return NULL;
}

static void tracer(bs_game_t *g, float x0, float y0, float z0, float x1, float y1, float z1, uint32_t color)
{
    bs_fx_t *f = fx_new(g, FX_TRACER, 0.07f, color);
    f->x0 = x0;
    f->y0 = y0;
    f->z0 = z0;
    f->x1 = x1;
    f->y1 = y1;
    f->z1 = z1;
}

static void tower_step(bs_game_t *g, int ti)
{
    bs_tower_t *t = &g->tw[ti];
    const bs_tower_def_t *d = &BS_TOWER[t->type];
    const float dt = BS_DT;
    t->anim += dt;
    if (t->flash_t > 0) t->flash_t -= dt;
    t->beam = 0;
    t->firing = 0;
    if (t->off_t > 0) {
        t->off_t -= dt;
        if ((rnd(g) & 15) == 0)
            part(g, SCREEN_X(t->x) + fr(g, -20, 20), SCREEN_Y(t->y, 1.2f), fr(g, -30, 30), -40, 0.3f, 0xF81F, 2, 0);
        return;
    }
    if (t->cool > 0) t->cool -= dt;
    float range = bs_game_range(g, t->type, t->tier) * (t->boost > 1.0f ? 1.08f : 1.0f);
    float dmg = bs_game_dmg(g, t->type, t->tier) * t->boost;
    bool air = d->air, ground = d->ground;
    float mx, my, mz;

    switch (t->type) {
    case TW_GATLING:
    case TW_CANNON:
    case TW_MISSILE:
    case TW_RAIL:
    case TW_MORTAR:
    case TW_LASER:
    case TW_FLAME: {
        float min_r = t->type == TW_MORTAR ? 3.0f : 0.0f;
        if (!target_ok(g, t, range, min_r, air, ground)) {
            t->target = (int16_t)acquire(g, t->x, t->y, range, min_r, air, ground, t->mode, t->type == TW_MISSILE);
            t->tid = t->target >= 0 ? g->al[t->target].id : 0;
            t->heat = 0;
        }
        if (t->target < 0) {
            if (t->heat > 0) t->heat = fmax2(0, t->heat - dt);
            break;
        }
        bs_alien_t *a = &g->al[t->target];
        const bs_alien_def_t *ad = &BS_ALIEN[a->type];
        float off = aim(t, a->x, a->y, t->type == TW_RAIL || t->type == TW_MORTAR ? 3.2f : 9.0f);
        muzzle(t, &mx, &my, &mz);
        float az = ad->alt + 0.45f;
        if (t->type == TW_LASER) {
            if (off > 0.3f) break;
            t->beam = 1;
            t->bx = a->x;
            t->by = a->y;
            t->bz = az;
            t->heat = fmin2(1.0f, t->heat + dt / 2.5f);
            hurt(g, t->target, dmg * (1.0f + 1.6f * t->heat) * dt, true, ti);
            if ((rnd(g) & 3) == 0) part(g, SCREEN_X(t->bx) + fr(g, -5, 5), SCREEN_Y(t->by, t->bz), fr(g, -60, 60), fr(g, -90, -20), 0.25f, c565(d->accent), 2, 1);
            bs_game_sound(g, SND_LASER);
            break;
        }
        if (t->type == TW_FLAME) {
            if (off > 0.5f) break;
            t->firing = 1;
            float cx = cosf(t->yaw), cy = sinf(t->yaw);
            for (int i = 0; i < BS_MAX_ALIENS; i++) {
                bs_alien_t *o = &g->al[i];
                if (!o->on || o->fly) continue;
                float dx = o->x - t->x, dy = o->y - t->y;
                float dd = sqrtf(dx * dx + dy * dy);
                if (dd > range + 0.4f || dd < 0.05f) continue;
                if ((dx * cx + dy * cy) / dd < 0.86f) continue;     /* a cone of 60 degrees */
                o->burn = dmg * 0.3f;
                o->burn_t = 3.0f;
                hurt(g, i, dmg * dt, true, ti);
            }
            /* the jet: sparks thrown along the aim, in screen pixels */
            float sx = SCREEN_X(mx), sy = SCREEN_Y(my, mz);
            float tx = SCREEN_X(t->x + cx * range), ty = SCREEN_Y(t->y + cy * range, 0.5f);
            for (int k = 0; k < 2; k++) {
                float life = fr(g, 0.28f, 0.42f);
                part(g, sx, sy, (tx - sx) / life + fr(g, -40, 40), (ty - sy) / life + fr(g, -40, 40), life,
                     k ? 0xFD20 : 0xFFE0, 5, 0);
            }
            bs_game_sound(g, SND_FLAME);
            break;
        }
        if (off > (t->type == TW_RAIL ? 0.04f : 0.22f) || t->cool > 0) break;
        t->flash_t = 0.07f;
        switch (t->type) {
        case TW_GATLING:
            t->cool = d->rate * (t->tier == 2 ? 0.8f : 1.0f);
            hurt(g, t->target, dmg, false, ti);
            tracer(g, mx, my, mz, a->x + fr(g, -0.1f, 0.1f), a->y, az, 0xFFE080);
            if (ad->armor >= 5) bs_game_sound(g, SND_HIT);
            bs_game_sound(g, SND_GATLING);
            break;
        case TW_CANNON: {
            t->cool = d->rate;
            bs_shot_t *s = shot_new(g, SH_SHELL, t, ti);
            if (s) {
                float dist = hypotf(a->x - s->x, a->y - s->y);
                s->life = fmax2(0.08f, dist / 15.0f);
                /* where it will be when the shell lands */
                float px, py, dx, dy;
                bs_path_at(road(g, a), a->s + BS_ALIEN[a->type].speed * (1.0f - a->slow) * s->life * (a->stun_t > 0 ? 0 : 1), &px, &py, &dx, &dy);
                s->tx = px - dy * a->off;
                s->ty = py + dx * a->off;
                s->vx = s->x;
                s->vy = s->y;
                s->vz = s->z;
                s->dmg = dmg;
                s->splash = d->splash + 0.15f * (float)t->tier;
            }
            bs_game_sound(g, SND_CANNON);
            break;
        }
        case TW_MISSILE: {
            t->cool = d->rate;
            int n = t->tier == 2 ? 2 : 1;
            for (int k = 0; k < n; k++) {
                bs_shot_t *s = shot_new(g, SH_MISSILE, t, ti);
                if (!s) break;
                s->target = t->target;
                s->tid = t->tid;
                s->dmg = dmg / (float)n * (n == 2 ? 1.25f : 1.0f);
                s->splash = d->splash;
                s->speed = 5.0f;
                float a0 = t->yaw + (k ? 0.5f : -0.5f) * (n == 2 ? 1.0f : 0.0f);
                s->vx = cosf(a0) * 3.0f;
                s->vy = sinf(a0) * 3.0f;
                s->vz = 4.0f;
                s->tx = a->x;
                s->ty = a->y;
                s->life = 6.0f;
            }
            bs_game_sound(g, SND_MISSILE);
            break;
        }
        case TW_RAIL: {
            t->cool = d->rate;
            float cx = cosf(t->yaw), cy = sinf(t->yaw);
            float len = range + 3.0f;
            for (int i = 0; i < BS_MAX_ALIENS; i++) {
                bs_alien_t *o = &g->al[i];
                if (!o->on || o->fly) continue;
                float dx = o->x - t->x, dy = o->y - t->y;
                float along = dx * cx + dy * cy;
                if (along < 0 || along > len) continue;
                float across = fabsf(dx * cy - dy * cx);
                if (across > 0.5f + BS_ALIEN[o->type].radius * 0.6f) continue;
                hurt(g, i, dmg, false, ti);
                burst(g, o->x, o->y, 0.5f, 4, d->accent, 120.0f, 2);
            }
            bs_fx_t *f = fx_new(g, FX_RAIL, 0.32f, d->accent);
            f->x0 = mx;
            f->y0 = my;
            f->z0 = mz;
            f->x1 = t->x + cx * len;
            f->y1 = t->y + cy * len;
            f->z1 = 0.5f;
            bs_game_sound(g, SND_RAIL);
            break;
        }
        case TW_MORTAR: {
            t->cool = d->rate;
            bs_shot_t *s = shot_new(g, SH_MORTAR, t, ti);
            if (s) {
                s->life = 1.3f;
                float px, py, dx, dy;
                bs_path_at(road(g, a), a->s + BS_ALIEN[a->type].speed * (1.0f - a->slow) * s->life * (a->stun_t > 0 ? 0 : 1), &px, &py, &dx, &dy);
                s->tx = px;
                s->ty = py;
                s->vx = s->x;
                s->vy = s->y;
                s->vz = s->z;
                s->dmg = dmg;
                s->splash = d->splash + 0.25f * (float)t->tier;
            }
            bs_game_sound(g, SND_MORTAR);
            break;
        }
        default: break;
        }
        break;
    }
    case TW_TESLA: {
        if (t->cool > 0) break;
        int first = acquire(g, t->x, t->y, range, 0, air, ground, t->mode, false);
        if (first < 0) break;
        t->cool = d->rate;
        t->flash_t = 0.15f;
        bs_fx_t *f = fx_new(g, FX_BOLT, 0.16f, d->accent);
        f->px[0] = t->x;
        f->py[0] = t->y;
        f->pz[0] = (float)d->height * 0.1f + 0.2f * (float)t->tier;
        f->n = 1;
        int hit[BS_BOLT_PTS];
        int nh = 0, cur = first;
        float k = 1.0f;
        int jumps = 3 + t->tier;
        while (cur >= 0 && nh < jumps && f->n < BS_BOLT_PTS) {
            bs_alien_t *a = &g->al[cur];
            f->px[f->n] = a->x;
            f->py[f->n] = a->y;
            f->pz[f->n] = BS_ALIEN[a->type].alt + 0.45f;
            f->n++;
            hit[nh++] = cur;
            float ax = a->x, ay = a->y;
            hurt(g, cur, dmg * k, false, ti);
            k *= 0.78f;
            /* the nearest one it has not touched yet */
            int nx = -1;
            float bd = 2.6f * 2.6f;
            for (int i = 0; i < BS_MAX_ALIENS; i++) {
                const bs_alien_t *o = &g->al[i];
                if (!o->on || !on_screen(o)) continue;
                bool seen = false;
                for (int q = 0; q < nh; q++) seen |= hit[q] == i;
                if (seen) continue;
                float dx = o->x - ax, dy = o->y - ay;
                float d2 = dx * dx + dy * dy;
                if (d2 < bd) {
                    bd = d2;
                    nx = i;
                }
            }
            cur = nx;
        }
        bs_game_sound(g, SND_ZAP);
        break;
    }
    case TW_CRYO:
    case TW_GRAVITY: {
        bool any = false;
        float slow = t->type == TW_CRYO ? 0.35f + 0.1f * (float)t->tier : 0.0f;
        for (int i = 0; i < BS_MAX_ALIENS; i++) {
            bs_alien_t *a = &g->al[i];
            if (!a->on || !on_screen(a)) continue;
            float dx = a->x - t->x, dy = a->y - t->y;
            if (dx * dx + dy * dy > range * range) continue;
            any = true;
            if (t->type == TW_CRYO) {
                if (slow > a->slow) a->slow = slow;
                a->slow_t = 0.35f;
            } else {
                a->drag_t = 0.3f;
            }
            hurt(g, i, dmg * dt, true, ti);
        }
        t->firing = any;
        if (!any) break;
        if (t->type == TW_CRYO) {
            if ((rnd(g) & 3) == 0) {
                float a = fr(g, 0, 2 * PI), r = fr(g, 0.3f, range);
                part(g, SCREEN_X(t->x + cosf(a) * r), SCREEN_Y(t->y + sinf(a) * r, 0.3f), fr(g, -10, 10), -30, 0.6f, 0xCFFF, 2, 0);
            }
            if (t->cool <= 0) {
                t->cool = 1.6f;
                ring(g, t->x, t->y, 0.4f, range, 0.7f, d->accent);
                bs_game_sound(g, SND_CRYO);
            }
        } else if (t->cool <= 0) {
            /* the implosion: everyone in reach goes back down the road */
            t->cool = d->rate - 0.6f * (float)t->tier;
            for (int i = 0; i < BS_MAX_ALIENS; i++) {
                bs_alien_t *a = &g->al[i];
                if (!a->on || BS_ALIEN[a->type].boss || !on_screen(a)) continue;
                float dx = a->x - t->x, dy = a->y - t->y;
                if (dx * dx + dy * dy > range * range) continue;
                a->s = fmax2(0, a->s - (2.2f + 0.6f * (float)t->tier));
                a->stun_t = fmax2(a->stun_t, 0.5f);
                hurt(g, i, dmg * 1.5f, false, ti);
            }
            ring(g, t->x, t->y, range, 0.3f, 0.45f, d->accent);
            t->flash_t = 0.4f;
            bs_game_sound(g, SND_GRAVITY);
        }
        break;
    }
    case TW_EMP: {
        if (t->cool > 0) break;
        bool any = false;
        for (int i = 0; i < BS_MAX_ALIENS && !any; i++) {
            const bs_alien_t *a = &g->al[i];
            if (!a->on || !on_screen(a)) continue;
            float dx = a->x - t->x, dy = a->y - t->y;
            any = dx * dx + dy * dy <= range * range;
        }
        if (!any) break;
        t->cool = d->rate - 0.5f * (float)t->tier;
        t->flash_t = 0.3f;
        for (int i = 0; i < BS_MAX_ALIENS; i++) {
            bs_alien_t *a = &g->al[i];
            if (!a->on || !on_screen(a)) continue;
            float dx = a->x - t->x, dy = a->y - t->y;
            if (dx * dx + dy * dy > range * range) continue;
            float st = 1.0f + 0.35f * (float)t->tier;
            if (BS_ALIEN[a->type].boss) st *= 0.4f;
            a->stun_t = fmax2(a->stun_t, st);
            if (a->maxshield > 0) {
                a->shield = 0;
                a->shield_t = 6.0f;
            }
            a->cloaked = 0;
            hurt(g, i, dmg, false, ti);
        }
        ring(g, t->x, t->y, 0.4f, range, 0.4f, d->accent);
        ring(g, t->x, t->y, 0.2f, range * 0.7f, 0.5f, 0xFFFFFF);
        bs_game_sound(g, SND_EMP);
        break;
    }
    case TW_DRONE: {
        int n = 2 + t->tier;
        float each = dmg;
        for (int k = 0; k < n && k < BS_MAX_DRONES; k++) {
            bs_drone_t *dr = &t->drone[k];
            bool ok = dr->target >= 0 && g->al[dr->target].id == dr->tid && can_hit(&g->al[dr->target], true, true);
            if (ok) {
                const bs_alien_t *a = &g->al[dr->target];
                float dx = a->x - t->x, dy = a->y - t->y;
                ok = dx * dx + dy * dy <= (range + 0.6f) * (range + 0.6f);
            }
            if (!ok) {
                /* each drone its own prey when there is enough of it */
                dr->target = (int16_t)acquire(g, t->x, t->y, range, 0, true, true, (t->mode + k) % TM_N, false);
                dr->tid = dr->target >= 0 ? g->al[dr->target].id : 0;
            }
            float wx, wy;
            if (dr->target >= 0) {
                const bs_alien_t *a = &g->al[dr->target];
                float ang = t->anim * 2.5f + (float)k * 2.1f;
                wx = a->x + cosf(ang) * 0.9f;
                wy = a->y + sinf(ang) * 0.9f;
            } else {
                float ang = t->anim * 1.2f + (float)k * (2 * PI / (float)n);
                wx = t->x + cosf(ang) * 1.3f;
                wy = t->y + sinf(ang) * 1.3f;
            }
            float dx = wx - dr->x, dy = wy - dr->y;
            float dd = sqrtf(dx * dx + dy * dy);
            float step = 7.0f * dt;
            if (dd > step) {
                dr->x += dx / dd * step;
                dr->y += dy / dd * step;
                dr->a = atan2f(dy, dx);
            } else {
                dr->x = wx;
                dr->y = wy;
            }
            if (dr->cool > 0) dr->cool -= dt;
            if (dr->target >= 0) {
                bs_alien_t *a = &g->al[dr->target];
                float ex = a->x - dr->x, ey = a->y - dr->y;
                if (ex * ex + ey * ey < 1.6f * 1.6f) {
                    dr->a = atan2f(ey, ex);
                    hurt(g, dr->target, each * dt, true, ti);
                    if (dr->cool <= 0) {
                        dr->cool = 0.14f;
                        tracer(g, dr->x, dr->y, 1.8f, a->x, a->y, BS_ALIEN[a->type].alt + 0.4f, d->accent);
                        bs_game_sound(g, SND_DRONE);
                    }
                }
            }
        }
        break;
    }
    default: break;
    }
}

/* the amplifiers' reach is worked out when a tower comes or goes */
static void radar_update(bs_game_t *g)
{
    for (int i = 0; i < BS_PADS; i++) g->tw[i].boost = 1.0f;
    for (int r = 0; r < BS_PADS; r++) {
        const bs_tower_t *ra = &g->tw[r];
        if (!ra->on || ra->type != TW_RADAR) continue;
        float range = bs_game_range(g, TW_RADAR, ra->tier);
        float k = 1.2f + 0.1f * (float)ra->tier;
        for (int i = 0; i < BS_PADS; i++) {
            bs_tower_t *t = &g->tw[i];
            if (!t->on || i == r || t->type == TW_RADAR || t->type == TW_BANK) continue;
            float dx = t->x - ra->x, dy = t->y - ra->y;
            if (dx * dx + dy * dy <= range * range && k > t->boost) t->boost = k;
        }
    }
    g->radar_dirty = false;
}

/* ---- shots, effects ------------------------------------------------------------------- */

static void shots_step(bs_game_t *g)
{
    const float dt = BS_DT;
    for (int i = 0; i < BS_MAX_SHOTS; i++) {
        bs_shot_t *s = &g->shot[i];
        if (!s->on) continue;
        s->t += dt;
        const bs_tower_def_t *d = &BS_TOWER[g->tw[s->from].on ? g->tw[s->from].type : TW_CANNON];
        if (s->kind == SH_SHELL) {
            float u = s->t / s->life;
            if (u >= 1.0f) {
                blast(g, s->tx, s->ty, s->splash, s->dmg, false, s->from);
                explode(g, s->tx, s->ty, 0.2f, s->splash, 0xFF8030, SND_BOOM_S);
                s->on = 0;
                continue;
            }
            s->x = s->vx + (s->tx - s->vx) * u;
            s->y = s->vy + (s->ty - s->vy) * u;
            s->z = s->vz * (1.0f - u) + 0.3f * u + 0.6f * u * (1.0f - u);
        } else if (s->kind == SH_MORTAR) {
            float u = s->t / s->life;
            if (u >= 1.0f) {
                blast(g, s->tx, s->ty, s->splash, s->dmg, false, s->from);
                explode(g, s->tx, s->ty, 0.2f, s->splash, d->accent, SND_BOOM_L);
                g->shake = fmax2(g->shake, 0.18f);
                s->on = 0;
                continue;
            }
            s->x = s->vx + (s->tx - s->vx) * u;
            s->y = s->vy + (s->ty - s->vy) * u;
            s->z = s->vz * (1.0f - u) + 22.0f * u * (1.0f - u);
            if ((rnd(g) & 1) == 0) part(g, SCREEN_X(s->x), SCREEN_Y(s->y, s->z), fr(g, -8, 8), fr(g, -8, 8), 0.3f, c565(d->accent), 3, 0);
        } else {
            /* a missile: it chases, faster and faster */
            float tz = 0.4f;
            if (s->target >= 0 && g->al[s->target].on && g->al[s->target].id == s->tid) {
                const bs_alien_t *a = &g->al[s->target];
                s->tx = a->x;
                s->ty = a->y;
                tz = BS_ALIEN[a->type].alt + 0.4f;
            } else if (s->target >= 0) {
                /* its prey died: the nearest one to where it was */
                s->target = (int16_t)acquire(g, s->tx, s->ty, 3.0f, 0, true, true, TM_NEAR, false);
                s->tid = s->target >= 0 ? g->al[s->target].id : 0;
            }
            float dx = s->tx - s->x, dy = s->ty - s->y, dz = tz - s->z;
            float dd = sqrtf(dx * dx + dy * dy + dz * dz);
            s->speed = fmin2(13.0f, s->speed + 14.0f * dt);
            if (dd < 0.35f + s->speed * dt || s->t > s->life) {
                blast(g, s->x, s->y, s->splash, s->dmg, true, s->from);
                explode(g, s->x, s->y, s->z, s->splash, 0xB0FF40, SND_BOOM_S);
                s->on = 0;
                continue;
            }
            float k = fmin2(1.0f, 9.0f * dt);
            s->vx += (dx / dd * s->speed - s->vx) * k;
            s->vy += (dy / dd * s->speed - s->vy) * k;
            s->vz += (dz / dd * s->speed - s->vz) * k;
            s->x += s->vx * dt;
            s->y += s->vy * dt;
            s->z += s->vz * dt;
            if (s->z < 0.2f) s->z = 0.2f;
            part(g, SCREEN_X(s->x), SCREEN_Y(s->y, s->z), fr(g, -10, 10), fr(g, -10, 10), 0.28f, 0xFE60, 3, 0);
        }
    }
    for (int i = 0; i < BS_MAX_FX; i++) {
        bs_fx_t *f = &g->fx[i];
        if (!f->on) continue;
        float before = f->t;
        f->t += dt;
        if (f->kind == FX_STRIKE && before < 0.55f && f->t >= 0.55f) {
            blast(g, f->x0, f->y0, f->r, f->z1, true, -1);
            explode(g, f->x0, f->y0, 0.3f, f->r, 0xFF9040, SND_BOOM_L);
            ring(g, f->x0, f->y0, 0.3f, f->r * 1.3f, 0.4f, 0xFFFFFF);
            g->shake = 0.5f;
        }
        if (f->t >= f->life) f->on = 0;
    }
    int n = 0;
    for (int i = 0; i < g->npart; i++) {
        bs_part_t *p = &g->part[i];
        p->life -= dt;
        if (p->life <= 0) continue;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        if (p->grav) p->vy += 420.0f * dt;
        else {
            p->vx *= 0.96f;
            p->vy *= 0.96f;
        }
        if (n != i) g->part[n] = *p;
        n++;
    }
    g->npart = n;
    for (int i = 0; i < BS_MAX_FLOATS; i++) {
        if (g->fl[i].t > 0) {
            g->fl[i].t -= dt;
            g->fl[i].y += dt * 1.2f;
        }
    }
}

/* ---- waves ---------------------------------------------------------------------------- */

static uint32_t wrand(uint32_t *r)
{
    uint32_t x = *r;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *r = x;
}

/* A wave of the mode without end: made from the wave's number alone, so
 * the button that calls it can show who comes. More of them each wave,
 * closer together, tougher by 7 % a wave (twice at wave 10, fifteen times
 * at wave 40: nobody holds for ever), a boss every tenth. */
static void endless_plan(bs_game_t *g)
{
    /* what tools/bsworlds.py reckons each one is worth */
    static const float COST[AL_N] = {
        [AL_GRUB] = 1.0f, [AL_RUNNER] = 1.3f, [AL_SWARM] = 0.45f, [AL_SHELL] = 3.2f, [AL_WASP] = 1.8f,
        [AL_SPLIT] = 3.0f, [AL_SHIELD] = 3.6f, [AL_HEALER] = 4.0f, [AL_BRUTE] = 9.0f, [AL_PHASE] = 3.4f,
        [AL_JAMMER] = 5.0f, [AL_CARRIER] = 12.0f,
    };
    int w = g->wave;
    uint32_t r = 0x9E3779B9u ^ (uint32_t)(g->w->order * 7919 + w * 104729 + 17);
    for (int i = 0; i < 4; i++) wrand(&r);
    float hp = (float)g->hp0 * powf(1.07f, (float)w);
    if (hp > 60000.0f) hp = 60000.0f;
    float budget = 10.0f * (1.0f + 0.45f * (float)w);
    float tight = 1.0f - 0.02f * (float)w;
    if (tight < 0.5f) tight = 0.5f;
    int pool[AL_N], np = 0;
    for (int a = 0; a < AL_N; a++) {
        if ((g->roster >> a) & 1 && COST[a] > 0) pool[np++] = a;
    }
    /* the dear ones wait a few waves */
    int allow = np;
    if (w < 3 && allow > 3) allow = 3;
    else if (w < 7 && allow > np - 2 && np > 4) allow = np - 2;
    for (int i = allow - 1; i > 0; i--) {
        int k = (int)(wrand(&r) % (uint32_t)(i + 1));
        int t = pool[i];
        pool[i] = pool[k];
        pool[k] = t;
    }
    int kinds = w < 2 ? 1 : w < 6 ? 2 : w < 14 ? 3 : 4;
    if (w % 3 == 2 && w % 10 != 9) kinds = 1;
    if (kinds > allow) kinds = allow;
    g->nnext = 0;
    float delay = 0;
    if (w % 10 == 9) {
        bs_group_t *b = &g->next[g->nnext++];
        b->alien = (uint8_t)g->w->boss;
        b->count = (uint16_t)(1 + w / 30);
        b->gap = 9.0f;
        b->delay = 4.0f;
        b->path = 0;
        b->hp_pct = (uint16_t)(hp * 0.5f);
        budget *= 0.6f;
    }
    for (int k = 0; k < kinds && g->nnext < BS_MAX_SPAWN - g->w->npath; k++) {
        int a = pool[k];
        int cnt = (int)(budget / (float)kinds / COST[a] + 0.5f);
        if (cnt < 1) cnt = 1;
        if (cnt > 50) cnt = 50;
        float gap = a == AL_SWARM ? 0.30f : a == AL_RUNNER ? 0.6f : a == AL_GRUB ? 0.75f : a == AL_WASP ? 0.85f :
                    COST[a] >= 9.0f ? 2.8f : 1.15f;
        gap *= tight;
        int np2 = g->w->npath;
        int split = np2 > 1 && cnt >= 6 && w >= 3 ? np2 : 1;
        for (int q = 0; q < split; q++) {
            bs_group_t *gr = &g->next[g->nnext++];
            gr->alien = (uint8_t)a;
            gr->count = (uint16_t)(cnt / split + (q < cnt % split ? 1 : 0));
            gr->gap = split > 1 ? gap * (float)split * 0.8f : gap;
            gr->delay = delay + 0.3f * (float)q;
            gr->path = (uint8_t)((w + k + q) % np2);
            gr->hp_pct = (uint16_t)hp;
        }
        float span = (float)cnt * gap * 0.5f;
        delay += (span > 7.0f ? 7.0f : span) + 1.0f;
    }
}

/* works out the wave about to come into g->next */
static void wave_plan(bs_game_t *g)
{
    g->nnext = 0;
    if (g->mode == BS_MODE_ENDLESS) {
        endless_plan(g);
        return;
    }
    if (g->wave >= g->nwaves) return;
    const bs_wave_t *wv = &g->w->wave[g->sector][g->wave];
    for (int i = 0; i < wv->n && g->nnext < BS_MAX_SPAWN; i++) g->next[g->nnext++] = g->w->group[wv->first + i];
}

void bs_game_preview(const bs_game_t *g, int *counts)
{
    memset(counts, 0, sizeof(int) * AL_N);
    for (int i = 0; i < g->nnext; i++) counts[g->next[i].alien] += g->next[i].count;
}

bool bs_game_call_wave(bs_game_t *g)
{
    if (g->state != BS_BUILD || !g->nnext) return false;
    g->nsp = 0;
    bool boss = false;
    for (int i = 0; i < g->nnext; i++) {
        const bs_group_t *gr = &g->next[i];
        bs_spawn_t *sp = &g->sp[g->nsp++];
        sp->alien = gr->alien;
        sp->path = gr->path;
        sp->left = gr->count;
        sp->t = gr->delay;
        sp->gap = gr->gap;
        sp->hpk = (float)gr->hp_pct / 100.0f;
        boss |= BS_ALIEN[gr->alien].boss != 0;
    }
    g->state = BS_WAVE;
    g->state_t = 0;
    bs_game_sound(g, boss ? SND_BOSS_ROAR : SND_WAVE_START);
    return true;
}

static void wave_done(bs_game_t *g)
{
    int bonus = 18 + 4 * (g->wave > 30 ? 30 : g->wave);
    for (int i = 0; i < BS_PADS; i++) {
        bs_tower_t *t = &g->tw[i];
        if (!t->on || t->type != TW_BANK) continue;
        int pay = (int)(bs_tier_dmg(TW_BANK, t->tier) + 0.5f);
        bonus += pay;
        char txt[14];
        snprintf(txt, sizeof txt, "+%d", pay);
        float_text(g, t->x, t->y + 1.0f, txt, 0xFFE040);
        burst(g, t->x, t->y, 1.5f, 8, 0xFFD83A, 120.0f, 3);
    }
    if (g->k_interest > 0) {
        int in = (int)((float)g->credits * g->k_interest);
        int cap = 60 + 15 * (g->wave > 30 ? 30 : g->wave);
        if (in > cap) in = cap;
        bonus += in;
    }
    g->credits += bonus;
    g->earned += bonus;
    g->xp += 5 + (g->wave > 40 ? 40 : g->wave);
    if (g->wave < BS_WAVES) g->lives_at[g->wave] = (uint8_t)g->lives;
    g->wave++;
    if (g->mode == BS_MODE_ENDLESS && g->mile_step > 0 && g->wave % g->mile_step == 0) {
        /* a goal: the n-th pays n times the first */
        g->miles = g->wave / g->mile_step;
        g->mile_cores = 20 * g->miles + 2 * (g->w->order - 1);
        g->mile_credits = 200 + 100 * g->miles;
        g->mile_lives = g->max_lives / 4;
        if (g->lives + g->mile_lives > g->max_lives) g->mile_lives = g->max_lives - g->lives;
        g->cores_won += g->mile_cores;
        g->credits += g->mile_credits;
        g->lives += g->mile_lives;
        g->stars = g->miles > 3 ? 3 : g->miles;
        g->mile_t = 4.0f;
        ring(g, g->w->fort_x, g->w->fort_y, 0.5f, 7.0f, 0.9f, 0xFFD23C);
        burst(g, g->w->fort_x, g->w->fort_y, 2.5f, 50, 0xFFD23C, 260.0f, 4);
        bs_game_sound(g, SND_STAR);
    }
    if (g->mode != BS_MODE_ENDLESS && g->wave >= g->nwaves) {
        g->state = BS_WON;
        g->stars = g->lives * 5 >= g->max_lives * 4 ? 3 : (g->lives * 5 >= g->max_lives * 2 ? 2 : 1);
        g->xp += 30 + 10 * g->stars;
        g->nnext = 0;
        bs_game_sound(g, SND_VICTORY);
    } else {
        g->state = BS_BUILD;
        wave_plan(g);
        bs_game_sound(g, SND_WAVE_CLEAR);
        char txt[14];
        snprintf(txt, sizeof txt, "+%d", bonus);
        float_text(g, g->w->fort_x, g->w->fort_y + 3.0f, txt, 0xFFE040);
    }
    g->state_t = 0;
}

static void spawn_step(bs_game_t *g)
{
    bool more = false;
    for (int i = 0; i < g->nsp; i++) {
        bs_spawn_t *sp = &g->sp[i];
        if (sp->left <= 0) continue;
        more = true;
        sp->t -= BS_DT;
        if (sp->t > 0) continue;
        if (alien_spawn(g, sp->alien, sp->path, 0.0f, sp->hpk) >= 0) {
            sp->left--;
            sp->t += sp->gap * (0.85f + 0.3f * frand(g));
        } else {
            sp->t = 0.3f;
        }
    }
    if (!more && g->alive <= 0) wave_done(g);
}

/* ---- the player's moves -------------------------------------------------------------- */

bool bs_game_build(bs_game_t *g, int pad, int type)
{
    if (pad < 0 || pad >= g->w->npad || type < 0 || type >= TW_N) return false;
    if (g->state == BS_WON || g->state == BS_LOST) return false;
    bs_tower_t *t = &g->tw[pad];
    int cost = bs_game_cost(g, type, 0);
    if (t->on || !g->kit.owned[type] || g->credits < cost) {
        bs_game_sound(g, SND_DENY);
        return false;
    }
    memset(t, 0, sizeof *t);
    t->on = 1;
    t->type = (uint8_t)type;
    t->x = g->w->pad_x[pad];
    t->y = g->w->pad_y[pad];
    t->target = -1;
    t->boost = 1.0f;
    t->mode = type == TW_RAIL ? TM_STRONG : TM_FIRST;
    /* it looks at the road when it is put down */
    float bx = t->x, by = t->y - 1.0f, bd = 1e9f;
    for (int p = 0; p < g->w->npath; p++) {
        const bs_path_t *pa = &g->w->path[p];
        for (int i = 0; i < pa->n; i += 2) {
            float dx = pa->x[i] - t->x, dy = pa->y[i] - t->y;
            float d2 = dx * dx + dy * dy;
            if (d2 < bd) {
                bd = d2;
                bx = pa->x[i];
                by = pa->y[i];
            }
        }
    }
    t->yaw = atan2f(by - t->y, bx - t->x);
    if (t->yaw < 0) t->yaw += 2 * PI;
    for (int k = 0; k < BS_MAX_DRONES; k++) {
        t->drone[k].x = t->x;
        t->drone[k].y = t->y;
        t->drone[k].target = -1;
    }
    t->invested = cost;
    g->credits -= cost;
    g->built++;
    g->radar_dirty = true;
    ring(g, t->x, t->y, 1.6f, 0.5f, 0.3f, BS_TOWER[type].accent);
    burst(g, t->x, t->y, 0.3f, 14, BS_TOWER[type].accent, 150.0f, 3);
    bs_game_sound(g, SND_BUILD);
    return true;
}

bool bs_game_upgrade(bs_game_t *g, int pad)
{
    if (pad < 0 || pad >= BS_PADS) return false;
    bs_tower_t *t = &g->tw[pad];
    if (!t->on || t->tier >= BS_TIERS - 1 || g->state == BS_WON || g->state == BS_LOST) return false;
    int cost = bs_game_cost(g, t->type, t->tier + 1);
    if (g->credits < cost) {
        bs_game_sound(g, SND_DENY);
        return false;
    }
    g->credits -= cost;
    t->invested += cost;
    t->tier++;
    g->radar_dirty = true;
    ring(g, t->x, t->y, 0.4f, 1.8f, 0.4f, 0xFFFFFF);
    burst(g, t->x, t->y, 1.0f, 20, BS_TOWER[t->type].accent, 180.0f, 3);
    bs_game_sound(g, SND_UPGRADE);
    return true;
}

bool bs_game_sell(bs_game_t *g, int pad)
{
    if (pad < 0 || pad >= BS_PADS || !g->tw[pad].on) return false;
    bs_tower_t *t = &g->tw[pad];
    int v = bs_game_sell_value(g, pad);
    g->credits += v;
    char txt[14];
    snprintf(txt, sizeof txt, "+%d", v);
    float_text(g, t->x, t->y + 0.8f, txt, 0xFFE040);
    burst(g, t->x, t->y, 0.5f, 12, 0xC0C0C0, 130.0f, 3);
    t->on = 0;
    g->radar_dirty = true;
    bs_game_sound(g, SND_SELL);
    return true;
}

void bs_game_mode(bs_game_t *g, int pad)
{
    if (pad < 0 || pad >= BS_PADS || !g->tw[pad].on) return;
    g->tw[pad].mode = (uint8_t)((g->tw[pad].mode + 1) % TM_N);
    g->tw[pad].target = -1;
    bs_game_sound(g, SND_TAP);
}

bool bs_game_power(bs_game_t *g, int kind, float x, float y)
{
    if (kind < 0 || kind >= PW_N || !g->kit.power[kind] || g->power_cd[kind] > 0 || g->state == BS_WON ||
        g->state == BS_LOST) {
        bs_game_sound(g, SND_DENY);
        return false;
    }
    int lv = g->kit.power[kind] - 1;
    switch (kind) {
    case PW_STRIKE: {
        bs_fx_t *f = fx_new(g, FX_STRIKE, 1.0f, BS_POWER[kind].color);
        f->x0 = x;
        f->y0 = y;
        f->r = 2.6f + 0.3f * (float)lv;
        f->z1 = (320.0f + 260.0f * (float)lv) * (1.0f + 0.22f * (float)(g->w->order - 1));
        bs_game_sound(g, SND_STRIKE);
        break;
    }
    case PW_FREEZE:
        g->freeze_t = 3.0f + 1.0f * (float)lv;
        ring(g, g->w->fort_x, g->w->fort_y, 1.0f, 30.0f, 0.8f, 0xA0F0FF);
        for (int i = 0; i < BS_MAX_ALIENS; i++) {
            if (g->al[i].on) burst(g, g->al[i].x, g->al[i].y, 0.5f, 3, 0xC0F8FF, 60.0f, 2);
        }
        bs_game_sound(g, SND_FREEZE);
        break;
    case PW_REPAIR: {
        if (g->lives >= g->max_lives) {
            bs_game_sound(g, SND_DENY);
            return false;
        }
        int n = 5 + 3 * lv;
        g->lives = g->lives + n > g->max_lives ? g->max_lives : g->lives + n;
        ring(g, g->w->fort_x, g->w->fort_y, 0.5f, 4.0f, 0.6f, 0x60FF90);
        burst(g, g->w->fort_x, g->w->fort_y, 2.0f, 24, 0x60FF90, 140.0f, 3);
        bs_game_sound(g, SND_REPAIR);
        break;
    }
    default: break;
    }
    g->power_cd[kind] = BS_POWER[kind].cooldown * (1.0f - 0.12f * (float)lv);
    return true;
}

/* ---- a battle -------------------------------------------------------------------------- */

void bs_game_start(bs_game_t *g, const bs_world_t *w, int sector, const bs_kit_t *kit)
{
    memset(g, 0, sizeof *g);
    g->w = w;
    g->sector = sector < 0 ? 0 : sector >= BS_SECTORS ? BS_SECTORS - 1 : sector;
    g->kit = *kit;
    if (g->kit.rank < 1) g->kit.rank = 1;
    g->rng = 0x9E3779B9u ^ (uint32_t)(w->order * 7919 + sector * 104729 + 1);
    g->k_dmg = 1.0f + 0.05f * (float)kit->tech[TECH_DAMAGE];
    g->k_range = 1.0f + 0.03f * (float)kit->tech[TECH_RANGE];
    g->k_bounty = 1.0f + 0.06f * (float)kit->tech[TECH_BOUNTY];
    g->k_sell = 0.60f + 0.06f * (float)kit->tech[TECH_SALVAGE];
    g->k_interest = 0.02f * (float)kit->tech[TECH_INTEREST];
    g->k_crit = 0.04f * (float)kit->tech[TECH_CRIT];
    g->mode = kit->mode < BS_MODE_N ? kit->mode : BS_MODE_CAMPAIGN;
    /* the difficulty is the purse: what a battle starts with and what each
     * alien pays */
    static const float START[BS_DIFF_N] = { 1.0f, 1.35f, 0.85f }, PAY[BS_DIFF_N] = { 1.0f, 1.25f, 0.87f };
    int diff = kit->diff < BS_DIFF_N ? kit->diff : BS_DIFF_NORMAL;
    g->k_reward = PAY[diff];
    g->max_lives = g->lives = 20 + 3 * kit->tech[TECH_ARMOR];
    float purse = (float)(w->credits[g->sector] * (10 + kit->tech[TECH_FUNDS]) / 10) * START[diff];
    g->nwaves = w->nwaves[g->sector];
    if (g->mode == BS_MODE_ENDLESS) {
        /* twice the life, a little more to start with, and no last wave */
        g->sector = 0;
        g->max_lives = g->lives = 2 * g->max_lives;
        purse = (float)(w->credits[0] * (10 + kit->tech[TECH_FUNDS]) / 10) * START[diff] * 1.25f;
        g->nwaves = 1000000;
        g->mile_step = BS_MILE_STEP;
        g->hp0 = 100;
        if (w->nwaves[0] > 0 && w->wave[0][0].n > 0) g->hp0 = w->group[w->wave[0][0].first].hp_pct;
        for (int s = 0; s < BS_SECTORS; s++) g->roster |= bs_world_aliens(w, s);
    } else if (g->mode == BS_MODE_FREE) {
        purse *= 3.0f;
    }
    g->credits = (int)(purse + 0.5f);
    g->state = BS_BUILD;
    for (int i = 0; i < BS_PADS; i++) g->tw[i].target = -1;
    wave_plan(g);
}

void bs_game_step(bs_game_t *g)
{
    const float dt = BS_DT;
    for (int i = 0; i < SND_N; i++) {
        if (g->snd_cool[i] > 0) g->snd_cool[i] -= dt;
    }
    g->time += dt;
    g->state_t += dt;
    if (g->shake > 0) g->shake -= dt;
    if (g->fort_flash > 0) g->fort_flash -= dt;
    if (g->freeze_t > 0) g->freeze_t -= dt;
    if (g->mile_t > 0) g->mile_t -= dt;
    for (int i = 0; i < PW_N; i++) {
        if (g->power_cd[i] > 0 && g->state == BS_WAVE) g->power_cd[i] -= dt;
    }
    if (g->state == BS_WON || g->state == BS_LOST) {
        shots_step(g);
        return;
    }
    if (g->radar_dirty) radar_update(g);
    if (g->state == BS_WAVE) spawn_step(g);
    if (g->state == BS_WAVE || g->alive > 0) aliens_step(g);
    for (int i = 0; i < BS_PADS; i++) {
        if (g->tw[i].on) tower_step(g, i);
    }
    shots_step(g);
    if (g->lives <= 0 && g->state != BS_LOST) {
        g->state = BS_LOST;
        g->state_t = 0;
        g->shake = 0.8f;
        bs_fx_t *f = fx_new(g, FX_BLAST, 1.0f, 0xFF7030);
        f->x0 = g->w->fort_x;
        f->y0 = g->w->fort_y;
        f->z0 = 1.5f;
        f->r = 5.0f;
        burst(g, g->w->fort_x, g->w->fort_y, 1.5f, 80, 0xFF8030, 320.0f, 4);
        bs_game_sound(g, SND_DEFEAT);
    }
}
