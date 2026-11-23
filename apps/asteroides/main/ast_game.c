/*
 * DERIVA - the game (see ast_game.h): ships, rocks, enemies, bosses,
 * powers, crystals, waves and sectors, in fixed steps of 1/120 s.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "ast_game.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265f
#define TAU 6.28318531f

#define R_(t) (1u << (t))
#define ALL_R 0x1Fu
#define ALL_E 0x0Fu

const ag_sector_t AG_SECTOR[AG_SECTORS] = {
    { R_(RK_STONE), 0, BOSS_NONE, 3, 1.00f, 3 },
    { R_(RK_STONE) | R_(RK_TRIO), R_(EN_SEEKER), BOSS_NONE, 3, 1.05f, 3 },
    { R_(RK_STONE) | R_(RK_TRIO) | R_(RK_METAL), R_(EN_SEEKER) | R_(EN_LAYER), BOSS_NONE, 3, 1.06f, 3 },
    { R_(RK_STONE) | R_(RK_METAL), R_(EN_SEEKER), BOSS_COLOSO, 2, 1.08f, 4 },
    { R_(RK_STONE) | R_(RK_TRIO) | R_(RK_VOLATILE), R_(EN_SEEKER) | R_(EN_LANCER), BOSS_NONE, 3, 1.12f, 4 },
    { R_(RK_STONE) | R_(RK_METAL) | R_(RK_LAPA), R_(EN_LAYER) | R_(EN_LANCER), BOSS_NONE, 3, 1.15f, 5 },
    { ALL_R, R_(EN_SEEKER) | R_(EN_WEAVER), BOSS_NONE, 3, 1.18f, 5 },
    { R_(RK_STONE) | R_(RK_TRIO) | R_(RK_LAPA), R_(EN_SEEKER) | R_(EN_LAYER), BOSS_COLMENA, 2, 1.18f, 5 },
    { R_(RK_STONE) | R_(RK_METAL) | R_(RK_TRIO) | R_(RK_LAPA), R_(EN_LANCER) | R_(EN_WEAVER), BOSS_NONE, 3, 1.27f, 5 },
    { R_(RK_STONE) | R_(RK_VOLATILE) | R_(RK_METAL), ALL_E, BOSS_NONE, 3, 1.33f, 6 },
    { ALL_R, ALL_E, BOSS_NONE, 3, 1.40f, 6 },
    { ALL_R, R_(EN_SEEKER) | R_(EN_WEAVER), BOSS_SERPIENTE, 2, 1.30f, 5 },
};

static const float ROCK_R[3] = { 17.0f, 32.0f, 56.0f };
static const float ROCK_V[3] = { 112.0f, 76.0f, 48.0f };
static const int ROCK_PTS[3] = { 100, 50, 20 };
static const float TYPE_PTS[RK_N] = { 1.0f, 1.5f, 2.0f, 1.5f, 2.0f };
static const float BLAST_R[3] = { 62.0f, 92.0f, 124.0f };

/* the difficulty: rock speed, enemy rate, enemy fire, boss strength */
static const float DIFF_SPEED[AG_DIFF_N] = { 0.84f, 1.0f, 1.14f };
static const float DIFF_ENEMY[AG_DIFF_N] = { 1.35f, 1.0f, 0.78f };
static const float DIFF_FIRE[AG_DIFF_N] = { 0.72f, 1.0f, 1.25f };
static const float DIFF_BOSS[AG_DIFF_N] = { 0.75f, 1.0f, 1.25f };

static const int UP_MAX[UP_N] = { 3, 3, 2, 2, 2 };
static const int UP_COST[UP_N][3] = {
    { 20, 45, 80 },     /* engine */
    { 25, 50, 85 },     /* gun */
    { 40, 90, 0 },      /* armour: a shield at the start of each sector */
    { 20, 45, 0 },      /* jump: a shorter wait */
    { 15, 35, 0 },      /* magnet: crystals from further */
};

int ag_upg_max(int u)
{
    return u >= 0 && u < UP_N ? UP_MAX[u] : 0;
}

int ag_upg_cost(int u, int level)
{
    if (u < 0 || u >= UP_N || level < 0 || level >= UP_MAX[u]) return 0;
    return UP_COST[u][level];
}

/* ---- small things ------------------------------------------------------------------------ */

static float rnd(ag_game_t *g)
{
    return ag_rand01(&g->rnd);
}

static float rndr(ag_game_t *g, float a, float b)
{
    return a + (b - a) * rnd(g);
}

float ag_wrapd(float d, float size)
{
    if (d > size * 0.5f) d -= size;
    else if (d < -size * 0.5f) d += size;
    return d;
}

static float wrap(float v, float size)
{
    if (v < 0) v += size;
    else if (v >= size) v -= size;
    if (v < 0 || v >= size) v = fmodf(fmodf(v, size) + size, size);
    return v;
}

static float angd(float a)
{
    while (a > PI) a -= TAU;
    while (a < -PI) a += TAU;
    return a;
}

static void event(ag_game_t *g, int kind, float x, float y, float a, float b)
{
    if (g->nev >= AG_EVENTS) return;
    ag_event_t *e = &g->ev[g->nev++];
    e->kind = (uint8_t)kind;
    e->x = x;
    e->y = y;
    e->a = a;
    e->b = b;
}

static void sound(ag_game_t *g, int snd, int vol)
{
    for (int i = 0; i < g->nsnd; i++) {
        if (g->snd[i] == snd) {
            if (vol > g->snd_vol[i]) g->snd_vol[i] = (uint8_t)vol;
            return;
        }
    }
    if (g->nsnd >= AG_SNDS) return;
    g->snd[g->nsnd] = (uint8_t)snd;
    g->snd_vol[g->nsnd] = (uint8_t)(vol > 255 ? 255 : vol);
    g->nsnd++;
}

static void shake(ag_game_t *g, float k)
{
    g->shake += k;
    if (g->shake > 30) g->shake = 30;
}

/* the distance between two points of the wrapping field, squared */
static float dist2(const ag_game_t *g, float ax, float ay, float bx, float by, float *dx, float *dy)
{
    float x = ag_wrapd(bx - ax, g->W), y = ag_wrapd(by - ay, g->H);
    if (dx) *dx = x;
    if (dy) *dy = y;
    return x * x + y * y;
}

static void add_score(ag_game_t *g, int pts)
{
    g->score += pts;
    if (g->mode == AG_ARCADE && g->score >= g->next_life) {
        g->next_life += 10000;
        for (int s = 0; s < g->players; s++) {
            if (g->ship[s].in_play && g->ship[s].lives < 6) g->ship[s].lives++;
        }
        event(g, EV_LIFE, g->W / 2, g->H / 2, 0, 0);
        sound(g, SND_LIFE, 220);
    }
}

/* ---- spawning ------------------------------------------------------------------------------- */

static int rock_count(const ag_game_t *g)
{
    int n = 0;
    for (int i = 0; i < AG_ROCKS; i++) n += g->rock[i].on;
    return n;
}

static ag_rock_t *rock_new(ag_game_t *g, int type, int size, float x, float y, float vx, float vy)
{
    for (int i = 0; i < AG_ROCKS; i++) {
        ag_rock_t *r = &g->rock[i];
        if (r->on) continue;
        memset(r, 0, sizeof *r);
        r->on = true;
        r->type = (uint8_t)type;
        r->size = (uint8_t)size;
        r->stuck = -1;
        r->x = wrap(x, g->W);
        r->y = wrap(y, g->H);
        r->vx = vx;
        r->vy = vy;
        r->ang = rndr(g, 0, TAU);
        r->spin = rndr(g, -1.2f, 1.2f) / (1.0f + size * 0.6f);
        r->r = ROCK_R[size] * (type == RK_LAPA ? 0.85f : 1.0f);
        static const float HP[RK_N][3] = {
            { 1, 1, 1 }, { 1, 1, 2 }, { 1, 3, 4 }, { 1, 1, 2 }, { 1, 1, 1 },
        };
        r->hp = HP[type][size];
        r->nv = (uint8_t)(type == RK_LAPA ? 10 : 8 + size * 2);
        for (int k = 0; k < r->nv; k++) {
            float j = type == RK_METAL ? rndr(g, 0.86f, 1.02f) : type == RK_LAPA ? rndr(g, 0.85f, 1.0f) : rndr(g, 0.72f, 1.08f);
            r->shape[k] = (uint8_t)(j * 128.0f);
        }
        return r;
    }
    return NULL;
}

static bool near_ship(const ag_game_t *g, float x, float y, float d)
{
    for (int s = 0; s < g->players; s++) {
        const ag_ship_t *sh = &g->ship[s];
        if (!sh->in_play) continue;
        if (dist2(g, x, y, sh->x, sh->y, NULL, NULL) < d * d) return true;
    }
    return false;
}

/* a point on the field's edge, away from the ships */
static void edge_point(ag_game_t *g, float *x, float *y)
{
    for (int tries = 0; tries < 24; tries++) {
        float t = rnd(g);
        int side = (int)(rnd(g) * 4.0f);
        if (side == 0) { *x = t * g->W; *y = 0; }
        else if (side == 1) { *x = t * g->W; *y = g->H - 1; }
        else if (side == 2) { *x = 0; *y = t * g->H; }
        else { *x = g->W - 1; *y = t * g->H; }
        if (!near_ship(g, *x, *y, 240.0f)) return;
    }
}

static int pick_type(ag_game_t *g, uint8_t mask)
{
    static const float W[RK_N] = { 5.0f, 2.0f, 2.0f, 1.6f, 1.6f };
    float tot = 0;
    for (int t = 0; t < RK_N; t++) {
        if (mask & R_(t)) tot += W[t];
    }
    float v = rnd(g) * tot;
    for (int t = 0; t < RK_N; t++) {
        if (!(mask & R_(t))) continue;
        if (v < W[t]) return t;
        v -= W[t];
    }
    return RK_STONE;
}

static void spawn_rock_edge(ag_game_t *g, int type, int size)
{
    float x, y;
    edge_point(g, &x, &y);
    float a = rndr(g, 0, TAU);
    float v = ROCK_V[size] * g->speed_k * rndr(g, 0.75f, 1.2f) * (type == RK_METAL ? 0.78f : 1.0f);
    rock_new(g, type, size, x, y, cosf(a) * v, sinf(a) * v);
}

static ag_pick_t *pick_new(ag_game_t *g, int kind, float x, float y)
{
    for (int i = 0; i < AG_PICKS; i++) {
        ag_pick_t *p = &g->pick[i];
        if (p->on) continue;
        float a = rndr(g, 0, TAU);
        p->on = true;
        p->kind = (uint8_t)kind;
        p->x = x;
        p->y = y;
        p->vx = cosf(a) * 30.0f;
        p->vy = sinf(a) * 30.0f;
        p->life = 11.0f;
        p->ang = 0;
        return p;
    }
    return NULL;
}

static void drop_power(ag_game_t *g, float x, float y)
{
    /* what is needed least is rarest: a life */
    float v = rnd(g);
    int kind = v < 0.30f ? PW_TRIPLE : v < 0.55f ? PW_SHIELD : v < 0.75f ? PW_BEAM : v < 0.94f ? PW_MAGNET : PW_LIFE;
    pick_new(g, kind, x, y);
}

static void drop_crystals(ag_game_t *g, float x, float y, int n)
{
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < AG_CRYST; i++) {
            ag_cryst_t *c = &g->cryst[i];
            if (c->on) continue;
            float a = rndr(g, 0, TAU), v = rndr(g, 30.0f, 110.0f);
            c->on = true;
            c->x = x;
            c->y = y;
            c->vx = cosf(a) * v;
            c->vy = sinf(a) * v;
            c->life = 9.0f;
            break;
        }
    }
}

static ag_enemy_t *enemy_new(ag_game_t *g, int kind)
{
    for (int i = 0; i < AG_ENEMIES; i++) {
        ag_enemy_t *e = &g->en[i];
        if (e->on) continue;
        memset(e, 0, sizeof *e);
        e->on = true;
        e->kind = (uint8_t)kind;
        edge_point(g, &e->x, &e->y);
        static const float HP[EN_N] = { 1, 3, 2, 3 };
        e->hp = HP[kind];
        if (kind == EN_LAYER) {
            /* across the field, from the edge it came in by */
            float tx = g->W - e->x, ty = g->H - e->y;
            float dx = tx - e->x, dy = ty - e->y, l = sqrtf(dx * dx + dy * dy);
            if (l < 1) l = 1;
            e->vx = dx / l * 72.0f;
            e->vy = dy / l * 72.0f;
            e->ang = atan2f(dy, dx);
            e->t2 = 1.0f;
        } else if (kind == EN_WEAVER) {
            e->y = rndr(g, g->H * 0.2f, g->H * 0.8f);
            e->x = rnd(g) < 0.5f ? 0 : g->W - 1;
            e->vx = e->x < 1 ? 88.0f : -88.0f;
            e->ty = e->y;
            e->t2 = 2.0f;
        } else if (kind == EN_LANCER) {
            e->tx = rndr(g, g->W * 0.25f, g->W * 0.75f);
            e->ty = rndr(g, g->H * 0.25f, g->H * 0.75f);
        }
        return e;
    }
    return NULL;
}

static void ebul_new(ag_game_t *g, int kind, float x, float y, float vx, float vy, float life)
{
    for (int i = 0; i < AG_EBULS; i++) {
        ag_ebul_t *b = &g->ebul[i];
        if (b->on) continue;
        b->on = true;
        b->kind = (uint8_t)kind;
        b->x = x;
        b->y = y;
        b->vx = vx;
        b->vy = vy;
        b->life = life;
        b->arm = kind == EB_MINE ? 0.8f : 0;
        return;
    }
}

/* the nearest ship in play to (x, y), or NULL */
static ag_ship_t *target(ag_game_t *g, float x, float y, float *dx, float *dy)
{
    ag_ship_t *best = NULL;
    float bd = 1e12f;
    for (int s = 0; s < g->players; s++) {
        ag_ship_t *sh = &g->ship[s];
        if (!sh->alive || sh->jump_t > 0) continue;
        float ex, ey;
        float d = dist2(g, x, y, sh->x, sh->y, &ex, &ey);
        if (d < bd) {
            bd = d;
            best = sh;
            if (dx) *dx = ex;
            if (dy) *dy = ey;
        }
    }
    return best;
}

static void aim_shot(ag_game_t *g, int kind, float x, float y, float speed, float spread, int n, float life)
{
    float dx, dy;
    if (!target(g, x, y, &dx, &dy)) return;
    float a = atan2f(dy, dx);
    for (int k = 0; k < n; k++) {
        float b = a + (n > 1 ? spread * ((float)k / (float)(n - 1) - 0.5f) : 0);
        ebul_new(g, kind, x, y, cosf(b) * speed, sinf(b) * speed, life);
    }
    sound(g, SND_ESHOT, 150);
}

/* ---- the ships ------------------------------------------------------------------------------- */

static void release_lapas(ag_game_t *g, int s, float fling)
{
    ag_ship_t *sh = &g->ship[s];
    for (int i = 0; i < AG_ROCKS; i++) {
        ag_rock_t *r = &g->rock[i];
        if (!r->on || r->stuck != s) continue;
        r->stuck = -1;
        r->stun = 2.0f;
        float a = sh->ang + r->stuck_ang;
        r->vx = sh->vx * 0.3f + cosf(a) * fling;
        r->vy = sh->vy * 0.3f + sinf(a) * fling;
    }
    sh->lapas = 0;
}

static void ship_die(ag_game_t *g, int s)
{
    ag_ship_t *sh = &g->ship[s];
    if (!sh->alive) return;
    release_lapas(g, s, 90.0f);
    sh->alive = false;
    sh->lives--;
    sh->respawn_t = 2.0f;
    sh->wait_t = 0;
    sh->beam_on = false;
    for (int p = 0; p < PW_N; p++) sh->pw_t[p] = 0;
    sh->shield = 0;
    event(g, EV_SHIP_DIE, sh->x, sh->y, (float)s, 0);
    sound(g, SND_DIE, 255);
    shake(g, 20);
    g->deaths++;
    if (sh->lives <= 0) sh->in_play = false;
}

/* something lethal touched the ship: true if it went through (the shield
 * held, or it could not be hurt) */
static bool ship_hit(ag_game_t *g, int s, float fx, float fy, int cause)
{
    ag_ship_t *sh = &g->ship[s];
    if (!sh->alive || sh->jump_t > 0 || sh->inv_t > 0) return true;
    g->hits_taken++;
    if (sh->shield > 0) {
        sh->shield--;
        sh->inv_t = 1.2f;
        sh->shield_t = 0.5f;
        float dx = sh->x - fx, dy = sh->y - fy, l = sqrtf(dx * dx + dy * dy);
        if (l > 0.1f) {
            sh->vx += dx / l * 160.0f;
            sh->vy += dy / l * 160.0f;
        }
        event(g, EV_SHIELD, sh->x, sh->y, (float)s, 0);
        sound(g, SND_SHIELD, 230);
        shake(g, 6);
        return true;
    }
    g->cause[cause]++;
    ship_die(g, s);
    return false;
}

static void ship_place(ag_game_t *g, int s)
{
    ag_ship_t *sh = &g->ship[s];
    sh->x = g->W * (g->players == 2 ? (s ? 0.62f : 0.38f) : 0.5f);
    sh->y = g->H * 0.55f;
    sh->vx = sh->vy = 0;
    sh->ang = -PI / 2;
}

static void ship_spawn(ag_game_t *g, int s)
{
    ag_ship_t *sh = &g->ship[s];
    ship_place(g, s);
    sh->alive = true;
    sh->inv_t = 2.5f;
    sh->jump_t = 0;
    sh->fire_cd = 0.2f;
    sh->lapas = 0;
    sh->shield = g->upg[UP_ARMOR] > 0 ? 1 : 0;
}

/* the best of a few random places: furthest from what can hurt */
static void jump_spot(ag_game_t *g, float *ox, float *oy)
{
    float best = -1;
    for (int k = 0; k < 18; k++) {
        float x = rndr(g, 40.0f, g->W - 40.0f), y = rndr(g, 40.0f, g->H - 40.0f);
        float m = 1e9f;
        for (int i = 0; i < AG_ROCKS; i++) {
            const ag_rock_t *r = &g->rock[i];
            if (!r->on || r->stuck >= 0) continue;
            float d = sqrtf(dist2(g, x, y, r->x, r->y, NULL, NULL)) - r->r;
            if (d < m) m = d;
        }
        for (int i = 0; i < AG_ENEMIES; i++) {
            if (!g->en[i].on) continue;
            float d = sqrtf(dist2(g, x, y, g->en[i].x, g->en[i].y, NULL, NULL)) - 20.0f;
            if (d < m) m = d;
        }
        for (int i = 0; i < AG_EBULS; i++) {
            if (!g->ebul[i].on) continue;
            float d = sqrtf(dist2(g, x, y, g->ebul[i].x, g->ebul[i].y, NULL, NULL));
            if (d < m) m = d;
        }
        if (g->boss.on) {
            float d = sqrtf(dist2(g, x, y, g->boss.x, g->boss.y, NULL, NULL)) - g->boss.r - 40.0f;
            if (d < m) m = d;
        }
        if (m > best) {
            best = m;
            *ox = x;
            *oy = y;
        }
    }
}

static int shots_of(const ag_game_t *g, int s)
{
    int n = 0;
    for (int i = 0; i < AG_SHOTS; i++) n += g->shot[i].on && g->shot[i].owner == s;
    return n;
}

static void shot_new(ag_game_t *g, int s, float a)
{
    ag_ship_t *sh = &g->ship[s];
    for (int i = 0; i < AG_SHOTS; i++) {
        ag_shot_t *b = &g->shot[i];
        if (b->on) continue;
        float v = 700.0f + 50.0f * (float)g->upg[UP_GUN];
        b->on = true;
        b->owner = (uint8_t)s;
        b->x = wrap(sh->x + cosf(sh->ang) * 16.0f, g->W);
        b->y = wrap(sh->y + sinf(sh->ang) * 16.0f, g->H);
        b->vx = cosf(a) * v + sh->vx * 0.5f;
        b->vy = sinf(a) * v + sh->vy * 0.5f;
        b->life = 0.78f;
        return;
    }
}

/* where a ray from the nose leaves the field */
static float ray_len(const ag_game_t *g, float x, float y, float dx, float dy)
{
    float t = 1e9f;
    if (dx > 1e-4f) t = t < ((g->W - x) / dx) ? t : ((g->W - x) / dx);
    if (dx < -1e-4f) t = t < (-x / dx) ? t : (-x / dx);
    if (dy > 1e-4f) t = t < ((g->H - y) / dy) ? t : ((g->H - y) / dy);
    if (dy < -1e-4f) t = t < (-y / dy) ? t : (-y / dy);
    return t < 0 ? 0 : t;
}

/* how far (x, y) lies from the beam of ship sh, or a big number */
static float beam_dist(const ag_ship_t *sh, float x, float y)
{
    float dx = sh->bx - sh->x, dy = sh->by - sh->y;
    float l2 = dx * dx + dy * dy;
    if (l2 < 1) return 1e9f;
    float t = ((x - sh->x) * dx + (y - sh->y) * dy) / l2;
    if (t < 0 || t > 1) return 1e9f;
    float cx = sh->x + dx * t - x, cy = sh->y + dy * t - y;
    return sqrtf(cx * cx + cy * cy);
}

static void rock_damage(ag_game_t *g, int i, float dmg, float fx, float fy, int by);
static void enemy_damage(ag_game_t *g, int i, float dmg, int by);
static bool boss_hit_at(ag_game_t *g, float x, float y, float dmg, bool beam);

static void ship_step(ag_game_t *g, int s, const ag_hand_t *h)
{
    ag_ship_t *sh = &g->ship[s];
    static const ag_hand_t none = { 0 };
    if (!h) h = &none;
    if (!sh->in_play) return;
    if (!sh->alive) {
        sh->respawn_t -= AG_DT;
        if (sh->respawn_t <= 0 && sh->lives > 0 && g->state != AG_ST_OVER) {
            /* back in the middle when there is room, or after a while anyway */
            ship_place(g, s);
            bool room = true;
            for (int i = 0; i < AG_ROCKS && room; i++) {
                const ag_rock_t *r = &g->rock[i];
                if (r->on && dist2(g, sh->x, sh->y, r->x, r->y, NULL, NULL) < (r->r + 110) * (r->r + 110)) room = false;
            }
            for (int i = 0; i < AG_ENEMIES && room; i++) {
                if (g->en[i].on && dist2(g, sh->x, sh->y, g->en[i].x, g->en[i].y, NULL, NULL) < 160 * 160) room = false;
            }
            if (g->boss.on && dist2(g, sh->x, sh->y, g->boss.x, g->boss.y, NULL, NULL) < (g->boss.r + 120) * (g->boss.r + 120)) {
                room = false;
            }
            sh->wait_t += AG_DT;
            if (room || sh->wait_t > 2.5f) ship_spawn(g, s);
        }
        return;
    }
    if (sh->inv_t > 0) sh->inv_t -= AG_DT;
    if (sh->shield_t > 0) sh->shield_t -= AG_DT;
    if (sh->jump_cd > 0) sh->jump_cd -= AG_DT;
    if (sh->fire_cd > 0) sh->fire_cd -= AG_DT;
    for (int p = 0; p < PW_N; p++) {
        if (sh->pw_t[p] > 0) sh->pw_t[p] -= AG_DT;
    }
    sh->beam_on = false;
    sh->firing = false;
    if (sh->jump_t > 0) {
        sh->jump_t -= AG_DT;
        if (sh->jump_t <= 0) {
            sh->x = sh->jx;
            sh->y = sh->jy;
            sh->vx *= 0.3f;
            sh->vy *= 0.3f;
            sh->inv_t = 0.9f;
            static const float CD[3] = { 5.0f, 3.9f, 2.8f };
            sh->jump_cd = CD[g->upg[UP_JUMP]];
            event(g, EV_JUMP_IN, sh->x, sh->y, (float)s, 0);
        }
        return;
    }
    float slow = 1.0f - 0.2f * (float)sh->lapas;
    /* turning */
    float rate = (h->aim ? 7.2f : 4.6f) * (1.0f - 0.16f * (float)sh->lapas);
    float da = 0;
    if (h->aim) {
        float l = sqrtf(h->ax * h->ax + h->ay * h->ay);
        if (l > 0.2f) {
            float want = atan2f(h->ay, h->ax);
            da = angd(want - sh->ang);
            float m = rate * AG_DT;
            if (da > m) da = m;
            if (da < -m) da = -m;
        }
    } else {
        float t = h->turn < -1 ? -1 : h->turn > 1 ? 1 : h->turn;
        da = t * rate * AG_DT;
    }
    sh->ang = angd(sh->ang + da);
    sh->turn = da / (rate * AG_DT);
    /* pushing */
    float th = h->thrust < 0 ? 0 : h->thrust > 1 ? 1 : h->thrust;
    sh->thrust = th;
    float acc = (370.0f + 55.0f * (float)g->upg[UP_ENGINE]) * slow * th;
    sh->vx += cosf(sh->ang) * acc * AG_DT;
    sh->vy += sinf(sh->ang) * acc * AG_DT;
    float drag = 1.0f - 0.32f * AG_DT;
    sh->vx *= drag;
    sh->vy *= drag;
    float vmax = 360.0f + 40.0f * (float)g->upg[UP_ENGINE];
    float v2 = sh->vx * sh->vx + sh->vy * sh->vy;
    if (v2 > vmax * vmax) {
        float k = vmax / sqrtf(v2);
        sh->vx *= k;
        sh->vy *= k;
    }
    sh->x = wrap(sh->x + sh->vx * AG_DT, g->W);
    sh->y = wrap(sh->y + sh->vy * AG_DT, g->H);
    /* the jump */
    if (h->jump && sh->jump_cd <= 0) {
        jump_spot(g, &sh->jx, &sh->jy);
        sh->jump_t = 0.32f;
        release_lapas(g, s, 140.0f);
        event(g, EV_JUMP_OUT, sh->x, sh->y, (float)s, 0);
        sound(g, SND_JUMP, 230);
        g->jumps++;
        return;
    }
    /* shooting */
    if (h->fire && sh->pw_t[PW_BEAM] > 0) {
        float dx = cosf(sh->ang), dy = sinf(sh->ang);
        float nx = sh->x + dx * 16.0f, ny = sh->y + dy * 16.0f;
        float L = ray_len(g, nx, ny, dx, dy);
        sh->beam_on = true;
        sh->firing = true;
        sh->bx = nx + dx * L;
        sh->by = ny + dy * L;
        float dmg = 7.0f * AG_DT;
        for (int i = 0; i < AG_ROCKS; i++) {
            ag_rock_t *r = &g->rock[i];
            if (!r->on || r->stuck >= 0) continue;
            if (beam_dist(sh, r->x, r->y) < r->r * 0.9f) rock_damage(g, i, dmg, sh->x, sh->y, s);
        }
        for (int i = 0; i < AG_ENEMIES; i++) {
            if (g->en[i].on && beam_dist(sh, g->en[i].x, g->en[i].y) < 18.0f) enemy_damage(g, i, dmg, s);
        }
        for (int i = 0; i < AG_EBULS; i++) {
            ag_ebul_t *b = &g->ebul[i];
            if (b->on && b->kind == EB_MINE && beam_dist(sh, b->x, b->y) < 12.0f) b->life = 0.001f;
        }
        if (g->boss.on) {
            /* the beam hits the first thing of the boss along it */
            float dx2 = sh->bx - sh->x, dy2 = sh->by - sh->y;
            float L2 = sqrtf(dx2 * dx2 + dy2 * dy2);
            for (float t = 0; t < L2; t += 8.0f) {
                float px = sh->x + dx2 * t / L2, py = sh->y + dy2 * t / L2;
                if (boss_hit_at(g, px, py, dmg, true)) {
                    sh->bx = px;
                    sh->by = py;
                    break;
                }
            }
        }
        if (--g->beam_snd_t <= 0) {
            sound(g, SND_BEAM, 170);
            g->beam_snd_t = 14;
        }
    } else if (h->fire && sh->fire_cd <= 0) {
        int cap = sh->pw_t[PW_TRIPLE] > 0 ? 18 : 10;
        if (shots_of(g, s) < cap) {
            if (sh->pw_t[PW_TRIPLE] > 0) {
                shot_new(g, s, sh->ang - 0.17f);
                shot_new(g, s, sh->ang);
                shot_new(g, s, sh->ang + 0.17f);
                sound(g, SND_SHOT3, 160);
            } else {
                shot_new(g, s, sh->ang);
                sound(g, SND_SHOT, 150);
            }
            static const float CD[4] = { 0.17f, 0.15f, 0.13f, 0.11f };
            sh->fire_cd = CD[g->upg[UP_GUN]];
            sh->firing = true;
            event(g, EV_MUZZLE, sh->x, sh->y, (float)s, 0);
        }
    }
}

/* ---- rocks ------------------------------------------------------------------------------------ */

static void blast(ag_game_t *g, float x, float y, float R, int by);

static void rock_break(ag_game_t *g, int i, int by)
{
    ag_rock_t *r = &g->rock[i];
    if (!r->on) return;
    ag_rock_t old = *r;
    r->on = false;
    if (old.stuck >= 0 && old.stuck < 2 && g->ship[old.stuck].lapas > 0) g->ship[old.stuck].lapas--;
    if (by >= 0) add_score(g, (int)((float)ROCK_PTS[old.size] * TYPE_PTS[old.type]));
    int nc = old.size == 2 ? 2 : old.size == 1 ? 1 : (rnd(g) < 0.5f);
    if (old.type == RK_TRIO) nc++;
    if (old.type == RK_METAL && old.size > 0) nc++;
    drop_crystals(g, old.x, old.y, nc);
    if (by >= 0 && ((old.size == 2 && rnd(g) < 0.07f) || (old.size == 1 && rnd(g) < 0.025f))) drop_power(g, old.x, old.y);
    event(g, EV_BOOM, old.x, old.y, (float)old.size, (float)old.type);
    sound(g, old.size == 2 ? SND_BOOM_L : old.size == 1 ? SND_BOOM_M : SND_BOOM_S, 200);
    shake(g, 1.0f + 2.0f * old.size);
    if (old.type == RK_VOLATILE) {
        blast(g, old.x, old.y, BLAST_R[old.size], by);
        return;
    }
    if (old.size == 0 || (old.type == RK_LAPA && old.size < 1)) return;
    int n = old.type == RK_TRIO ? 3 : 2;
    int ctype = old.type == RK_METAL && old.size == 1 ? RK_STONE : old.type;
    float base = atan2f(old.vy, old.vx) + PI / 2;
    for (int k = 0; k < n; k++) {
        float a = base + TAU * (float)k / (float)n + rndr(g, -0.4f, 0.4f);
        float v = ROCK_V[old.size - 1] * g->speed_k * rndr(g, 0.85f, 1.3f) * (old.type == RK_METAL ? 0.8f : 1.0f);
        float off = ROCK_R[old.size - 1] * 0.6f;
        rock_new(g, ctype, old.size - 1, old.x + cosf(a) * off, old.y + sinf(a) * off, old.vx * 0.45f + cosf(a) * v,
                 old.vy * 0.45f + sinf(a) * v);
    }
}

static void rock_damage(ag_game_t *g, int i, float dmg, float fx, float fy, int by)
{
    ag_rock_t *r = &g->rock[i];
    r->hp -= dmg;
    if (r->hp <= 0) {
        rock_break(g, i, by);
        return;
    }
    if (r->flash <= 0.02f || dmg >= 1.0f) {
        r->flash = 0.12f;
        if (dmg >= 1.0f) {
            event(g, EV_SPARK, fx, fy, 0, 0);
            sound(g, SND_CLANG, 190);
            /* a knock from the side it was hit on */
            float dx = r->x - fx, dy = r->y - fy, l = sqrtf(dx * dx + dy * dy);
            if (l > 0.1f) {
                r->vx += dx / l * 12.0f;
                r->vy += dy / l * 12.0f;
            }
        }
    }
}

static void blast(ag_game_t *g, float x, float y, float R, int by)
{
    event(g, EV_BLAST, x, y, R, 0);
    sound(g, SND_BLAST, 255);
    shake(g, R / 9.0f);
    for (int i = 0; i < AG_ROCKS; i++) {
        ag_rock_t *r = &g->rock[i];
        if (!r->on) continue;
        float d = sqrtf(dist2(g, x, y, r->x, r->y, NULL, NULL));
        if (d > R + r->r * 0.6f) continue;
        if (r->type == RK_VOLATILE) {
            if (r->fuse <= 0) r->fuse = 0.14f;
        } else if (r->type == RK_METAL) {
            rock_damage(g, i, 3.0f, x, y, by);
        } else {
            rock_break(g, i, by);
        }
    }
    for (int i = 0; i < AG_ENEMIES; i++) {
        if (g->en[i].on && dist2(g, x, y, g->en[i].x, g->en[i].y, NULL, NULL) < R * R) enemy_damage(g, i, 3.0f, by);
    }
    for (int i = 0; i < AG_EBULS; i++) {
        ag_ebul_t *b = &g->ebul[i];
        if (b->on && b->kind == EB_MINE && b->life > 0.2f && dist2(g, x, y, b->x, b->y, NULL, NULL) < R * R) b->life = 0.15f;
    }
    if (g->boss.on) boss_hit_at(g, x, y, 3.0f, false);
    for (int s = 0; s < g->players; s++) {
        ag_ship_t *sh = &g->ship[s];
        if (sh->alive && dist2(g, x, y, sh->x, sh->y, NULL, NULL) < (R * 0.72f) * (R * 0.72f)) ship_hit(g, s, x, y, 4);
    }
}

static void rocks_step(ag_game_t *g)
{
    for (int i = 0; i < AG_ROCKS; i++) {
        ag_rock_t *r = &g->rock[i];
        if (!r->on) continue;
        if (r->flash > 0) r->flash -= AG_DT;
        r->ang += r->spin * AG_DT;
        if (r->fuse > 0) {
            r->fuse -= AG_DT;
            if (r->fuse <= 0) {
                rock_break(g, i, 0);
                continue;
            }
        }
        if (r->stuck >= 0) {
            ag_ship_t *sh = &g->ship[r->stuck];
            r->stuck_t += AG_DT;
            float a = sh->ang + r->stuck_ang;
            r->x = wrap(sh->x + cosf(a) * (AG_SHIP_R + r->r * 0.55f), g->W);
            r->y = wrap(sh->y + sinf(a) * (AG_SHIP_R + r->r * 0.55f), g->H);
            r->vx = sh->vx;
            r->vy = sh->vy;
            if (r->stuck_t > 8.0f || !sh->alive) {
                /* it lets go on its own after a while */
                r->stuck = -1;
                r->stun = 2.0f;
                r->vx = cosf(a) * 60.0f;
                r->vy = sinf(a) * 60.0f;
                if (sh->lapas > 0) sh->lapas--;
            }
            continue;
        }
        if (r->type == RK_LAPA) {
            if (r->stun > 0) {
                r->stun -= AG_DT;
            } else {
                float dx, dy;
                ag_ship_t *t = target(g, r->x, r->y, &dx, &dy);
                float d2 = dx * dx + dy * dy;
                if (t && d2 < 300.0f * 300.0f && d2 > 1) {
                    float d = sqrtf(d2);
                    r->vx += dx / d * 55.0f * AG_DT;
                    r->vy += dy / d * 55.0f * AG_DT;
                    float v2 = r->vx * r->vx + r->vy * r->vy, vm = 115.0f * g->speed_k;
                    if (v2 > vm * vm) {
                        float k = vm / sqrtf(v2);
                        r->vx *= k;
                        r->vy *= k;
                    }
                }
            }
        }
        r->x = wrap(r->x + r->vx * AG_DT, g->W);
        r->y = wrap(r->y + r->vy * AG_DT, g->H);
        /* the ships */
        for (int s = 0; s < g->players; s++) {
            ag_ship_t *sh = &g->ship[s];
            if (!sh->alive || sh->jump_t > 0) continue;
            float rr = r->r * 0.86f + AG_SHIP_R;
            if (dist2(g, r->x, r->y, sh->x, sh->y, NULL, NULL) > rr * rr) continue;
            if (sh->inv_t > 0) continue;
            if (r->type == RK_LAPA && r->stun <= 0) {
                if (sh->shield > 0) {
                    /* the shield bursts it */
                    rock_break(g, i, s);
                    break;
                }
                float dx, dy;
                dist2(g, sh->x, sh->y, r->x, r->y, &dx, &dy);
                r->stuck = (int8_t)s;
                r->stuck_ang = angd(atan2f(dy, dx) - sh->ang);
                r->stuck_t = 0;
                sh->lapas++;
                event(g, EV_STICK, r->x, r->y, (float)s, 0);
                sound(g, SND_STICK, 230);
                if (sh->lapas >= 3) {
                    g->cause[1]++;
                    ship_die(g, s);
                }
                break;
            }
            if (r->type == RK_LAPA) continue;
            if (ship_hit(g, s, r->x, r->y, 0) && r->on && sh->alive) {
                /* a shield takes the rock with it */
                rock_break(g, i, s);
            }
            break;
        }
    }
}

/* ---- shots --------------------------------------------------------------------------------- */

static void shots_step(ag_game_t *g)
{
    for (int i = 0; i < AG_SHOTS; i++) {
        ag_shot_t *b = &g->shot[i];
        if (!b->on) continue;
        b->life -= AG_DT;
        if (b->life <= 0) {
            b->on = false;
            continue;
        }
        b->x = wrap(b->x + b->vx * AG_DT, g->W);
        b->y = wrap(b->y + b->vy * AG_DT, g->H);
        bool gone = false;
        for (int k = 0; k < AG_ROCKS && !gone; k++) {
            ag_rock_t *r = &g->rock[k];
            if (!r->on || r->stuck >= 0) continue;
            float rr = r->r * 0.92f + 2.0f;
            if (dist2(g, b->x, b->y, r->x, r->y, NULL, NULL) < rr * rr) {
                gone = true;
                rock_damage(g, k, 1.0f, b->x, b->y, b->owner);
            }
        }
        for (int k = 0; k < AG_ENEMIES && !gone; k++) {
            ag_enemy_t *e = &g->en[k];
            if (!e->on) continue;
            float rr = e->kind == EN_LAYER ? 24.0f : e->kind == EN_WEAVER ? 20.0f : 15.0f;
            if (dist2(g, b->x, b->y, e->x, e->y, NULL, NULL) < rr * rr) {
                gone = true;
                enemy_damage(g, k, 1.0f, b->owner);
            }
        }
        for (int k = 0; k < AG_EBULS && !gone; k++) {
            ag_ebul_t *e = &g->ebul[k];
            if (!e->on || e->kind != EB_MINE || e->life < 0.2f) continue;
            if (dist2(g, b->x, b->y, e->x, e->y, NULL, NULL) < 14.0f * 14.0f) {
                gone = true;
                e->life = 0.02f;
            }
        }
        if (!gone && g->boss.on && boss_hit_at(g, b->x, b->y, 1.0f, false)) gone = true;
        if (gone) b->on = false;
    }
}

/* ---- enemies --------------------------------------------------------------------------------- */

static void enemy_damage(ag_game_t *g, int i, float dmg, int by)
{
    ag_enemy_t *e = &g->en[i];
    e->hp -= dmg;
    e->flash = 0.12f;
    if (e->hp > 0) {
        if (dmg >= 1.0f) sound(g, SND_CLANG, 150);
        return;
    }
    e->on = false;
    static const int PTS[EN_N] = { 150, 300, 400, 350 };
    if (by >= 0) add_score(g, PTS[e->kind]);
    drop_crystals(g, e->x, e->y, 3);
    if (by >= 0 && rnd(g) < 0.4f) drop_power(g, e->x, e->y);
    event(g, EV_ENEMY_DIE, e->x, e->y, (float)e->kind, 0);
    sound(g, SND_BOOM_M, 230);
    shake(g, 4);
    g->kills++;
    g->since_kill = 0;
}

static void enemies_step(ag_game_t *g)
{
    float fire_k = DIFF_FIRE[g->diff];
    for (int i = 0; i < AG_ENEMIES; i++) {
        ag_enemy_t *e = &g->en[i];
        if (!e->on) continue;
        e->t += AG_DT;
        if (e->flash > 0) e->flash -= AG_DT;
        float dx = 0, dy = 0;
        ag_ship_t *tg = target(g, e->x, e->y, &dx, &dy);
        float hit_r = 14.0f;
        switch (e->kind) {
        case EN_SEEKER: {
            float want = tg ? atan2f(dy, dx) : e->ang;
            float da = angd(want - e->ang);
            float m = 2.4f * AG_DT;
            e->ang += da > m ? m : da < -m ? -m : da;
            float acc = 240.0f;
            e->vx += cosf(e->ang) * acc * AG_DT;
            e->vy += sinf(e->ang) * acc * AG_DT;
            float vm = (150.0f + 4.0f * (float)(g->sector > 12 ? 12 : g->sector)) * (0.85f + 0.15f * fire_k);
            float v2 = e->vx * e->vx + e->vy * e->vy;
            if (v2 > vm * vm) {
                float k = vm / sqrtf(v2);
                e->vx *= k;
                e->vy *= k;
            }
            e->x = wrap(e->x + e->vx * AG_DT, g->W);
            e->y = wrap(e->y + e->vy * AG_DT, g->H);
            break;
        }
        case EN_LAYER: {
            e->x += e->vx * AG_DT;
            e->y += e->vy * AG_DT;
            e->ang += 0.8f * AG_DT;
            if (e->x < -60 || e->y < -60 || e->x > g->W + 60 || e->y > g->H + 60) {
                e->on = false;
                continue;
            }
            e->t2 -= AG_DT;
            if (e->t2 <= 0 && e->x > 20 && e->x < g->W - 20 && e->y > 20 && e->y < g->H - 20) {
                e->t2 = 1.5f / fire_k;
                ebul_new(g, EB_MINE, e->x, e->y, 0, 0, 14.0f);
                sound(g, SND_MINE, 120);
            }
            hit_r = 22.0f;
            break;
        }
        case EN_LANCER: {
            if (e->state == 0) {
                /* in: to a point of the field */
                float ex = ag_wrapd(e->tx - e->x, g->W), ey = ag_wrapd(e->ty - e->y, g->H);
                e->x = wrap(e->x + ex * 1.6f * AG_DT, g->W);
                e->y = wrap(e->y + ey * 1.6f * AG_DT, g->H);
                if (tg) e->ang = atan2f(dy, dx);
                if (e->t > 1.3f) {
                    e->state = 1;
                    e->t = 0;
                }
            } else if (e->state == 1) {
                /* aiming: the line shows where it goes */
                if (tg) {
                    float want = atan2f(dy, dx);
                    float da = angd(want - e->ang), m = 2.6f * AG_DT;
                    e->ang += da > m ? m : da < -m ? -m : da;
                }
                if (e->t > 1.15f / (0.7f + 0.3f * fire_k)) {
                    e->state = 2;
                    e->t = 0;
                    e->vx = cosf(e->ang) * 800.0f;
                    e->vy = sinf(e->ang) * 800.0f;
                    sound(g, SND_DASH, 210);
                    event(g, EV_DASH, e->x, e->y, e->ang, 0);
                }
            } else if (e->state == 2) {
                e->x = wrap(e->x + e->vx * AG_DT, g->W);
                e->y = wrap(e->y + e->vy * AG_DT, g->H);
                if (e->t > 0.55f) {
                    e->state = 3;
                    e->t = 0;
                }
            } else {
                e->vx *= 1.0f - 2.5f * AG_DT;
                e->vy *= 1.0f - 2.5f * AG_DT;
                e->x = wrap(e->x + e->vx * AG_DT, g->W);
                e->y = wrap(e->y + e->vy * AG_DT, g->H);
                if (e->t > 1.3f) {
                    e->state = 1;
                    e->t = 0;
                }
            }
            break;
        }
        case EN_WEAVER: {
            e->x = wrap(e->x + e->vx * AG_DT, g->W);
            e->y = wrap(e->ty + sinf(e->t * 1.6f) * 90.0f, g->H);
            e->ang += 2.0f * AG_DT;
            e->t2 -= AG_DT;
            if (e->t2 <= 0) {
                e->t2 = 2.4f / fire_k;
                aim_shot(g, EB_ORB, e->x, e->y, 200.0f, 0.7f, 3, 3.6f);
            }
            hit_r = 18.0f;
            break;
        }
        default: break;
        }
        for (int s = 0; s < g->players; s++) {
            ag_ship_t *sh = &g->ship[s];
            if (!sh->alive || sh->jump_t > 0) continue;
            float rr = hit_r + AG_SHIP_R;
            if (dist2(g, e->x, e->y, sh->x, sh->y, NULL, NULL) < rr * rr && sh->inv_t <= 0) {
                ship_hit(g, s, e->x, e->y, 2);
                /* the enemy goes too */
                if (e->on && e->kind != EN_LAYER) enemy_damage(g, i, 10.0f, -1);
                break;
            }
        }
    }
}

static void ebuls_step(ag_game_t *g)
{
    for (int i = 0; i < AG_EBULS; i++) {
        ag_ebul_t *b = &g->ebul[i];
        if (!b->on) continue;
        b->life -= AG_DT;
        if (b->arm > 0) b->arm -= AG_DT;
        if (b->life <= 0) {
            b->on = false;
            if (b->kind == EB_MINE) blast(g, b->x, b->y, 54.0f, 0);
            continue;
        }
        b->x = wrap(b->x + b->vx * AG_DT, g->W);
        b->y = wrap(b->y + b->vy * AG_DT, g->H);
        float rr = b->kind == EB_MINE ? 11.0f : b->kind == EB_ORB ? 8.0f : 5.0f;
        for (int s = 0; s < g->players; s++) {
            ag_ship_t *sh = &g->ship[s];
            if (!sh->alive || sh->jump_t > 0 || sh->inv_t > 0) continue;
            if (b->kind == EB_MINE && b->arm > 0) continue;
            float r2 = (rr + AG_SHIP_R) * (rr + AG_SHIP_R);
            if (dist2(g, b->x, b->y, sh->x, sh->y, NULL, NULL) > r2) continue;
            if (b->kind == EB_MINE) {
                b->life = 0.001f;
            } else {
                b->on = false;
                ship_hit(g, s, b->x, b->y, 3);
            }
            break;
        }
    }
}

/* ---- bosses ------------------------------------------------------------------------------------ */

static void boss_start(ag_game_t *g, int kind)
{
    ag_boss_t *b = &g->boss;
    memset(b, 0, sizeof *b);
    b->on = true;
    b->kind = (uint8_t)kind;
    float hk = DIFF_BOSS[g->diff];
    if (g->mode == AG_ARCADE) hk *= 1.0f + 0.25f * (float)((g->sector - 1) / 18);
    b->x = g->W / 2;
    b->y = -120.0f;
    b->dir = 1;
    if (kind == BOSS_COLOSO) {
        b->r = 122.0f;
        b->nparts = 6;
        for (int i = 0; i < 6; i++) b->part_hp[i] = 6.0f * hk;
        b->hp = b->hp_max = 36.0f * hk;
        b->spin = 0.35f;
        b->vx = 34.0f;
        b->vy = 26.0f;
    } else if (kind == BOSS_COLMENA) {
        b->r = 104.0f;
        b->nparts = 3;
        for (int i = 0; i < 3; i++) b->part_hp[i] = 10.0f * hk;
        b->hp = b->hp_max = 34.0f * hk;
        b->t2 = 3.0f;
    } else {
        b->r = 28.0f;
        b->nseg = AG_SEGS;
        for (int i = 0; i < AG_SEGS; i++) b->seg_hp[i] = 3.0f * hk;
        b->hp = b->hp_max = 28.0f * hk;
        b->x = g->W / 2;
        b->y = -40.0f;
        b->ang = PI / 2;
        for (int i = 0; i < AG_TRAIL; i++) {
            b->trail_x[i] = b->x;
            b->trail_y[i] = b->y - (float)(AG_TRAIL - i) * 0.0f;
        }
    }
    b->t = 0;
}

/* the serpent's ring k (0 behind the head) on its trail */
static void seg_pos(const ag_game_t *g, int k, float *x, float *y)
{
    const ag_boss_t *b = &g->boss;
    int back = (k + 1) * 22;
    int i = (b->trail_i - back) & (AG_TRAIL - 1);
    *x = b->trail_x[i];
    *y = b->trail_y[i];
}

static int serpent_tail(const ag_game_t *g)
{
    for (int k = AG_SEGS - 1; k >= 0; k--) {
        if (g->boss.seg_hp[k] > 0) return k;
    }
    return -1;
}

static void boss_die(ag_game_t *g)
{
    ag_boss_t *b = &g->boss;
    b->on = false;
    add_score(g, 5000);
    drop_crystals(g, b->x, b->y, 25);
    drop_power(g, b->x - 30, b->y);
    drop_power(g, b->x + 30, b->y);
    event(g, EV_BOSS_DIE, b->x, b->y, b->r, (float)b->kind);
    sound(g, SND_BOSS_DIE, 255);
    shake(g, 30);
    g->bosses++;
    g->since_kill = 0;
}

/* a hit at (x, y): true if it touched the boss (a shot stops there) */
static bool boss_hit_at(ag_game_t *g, float x, float y, float dmg, bool beam)
{
    ag_boss_t *b = &g->boss;
    if (!b->on || b->state == 0) return false;
    float dx, dy;
    float d2 = dist2(g, b->x, b->y, x, y, &dx, &dy);
    float d = sqrtf(d2);
    if (b->kind == BOSS_COLOSO) {
        if (d > b->r + 4) return false;
        float a = atan2f(dy, dx) - b->ang;
        while (a < 0) a += TAU;
        while (a >= TAU) a -= TAU;
        int p = (int)(a / (TAU / 6.0f)) % 6;
        if (d > 84.0f && b->part_hp[p] > 0) {
            b->part_hp[p] -= dmg;
            b->part_flash[p] = 0.1f;
            if (!beam) {
                event(g, EV_SPARK, x, y, 0, 0);
                sound(g, SND_CLANG, 170);
            }
            if (b->part_hp[p] <= 0) {
                float pa = b->ang + (p + 0.5f) * TAU / 6.0f;
                event(g, EV_BOSS_PART, b->x + cosf(pa) * 104.0f, b->y + sinf(pa) * 104.0f, pa, 0);
                sound(g, SND_BOOM_L, 240);
                shake(g, 8);
                add_score(g, 300);
                drop_crystals(g, b->x + cosf(pa) * 104.0f, b->y + sinf(pa) * 104.0f, 2);
            }
            return true;
        }
        if (d < 92.0f) {
            if (b->part_hp[p] > 0 && d > 70.0f) return false;
            /* the core, through a gap in the armour */
            b->hp -= dmg;
            b->flash = 0.1f;
            if (!beam) sound(g, SND_BOSS_HIT, 170);
            if (b->hp <= 0) boss_die(g);
            return true;
        }
        return false;
    }
    if (b->kind == BOSS_COLMENA) {
        for (int p = 0; p < 3; p++) {
            if (b->part_hp[p] <= 0) continue;
            float pa = b->ang + (float)p * TAU / 3.0f;
            float tx = b->x + cosf(pa) * b->r, ty = b->y + sinf(pa) * b->r;
            if (dist2(g, tx, ty, x, y, NULL, NULL) < 26.0f * 26.0f) {
                b->part_hp[p] -= dmg;
                b->part_flash[p] = 0.1f;
                if (!beam) sound(g, SND_BOSS_HIT, 160);
                if (b->part_hp[p] <= 0) {
                    event(g, EV_BOSS_PART, tx, ty, pa, 0);
                    sound(g, SND_BOOM_L, 240);
                    shake(g, 8);
                    add_score(g, 500);
                    drop_crystals(g, tx, ty, 3);
                }
                return true;
            }
        }
        if (d > b->r * 0.9f) return false;
        bool open = b->part_hp[0] <= 0 && b->part_hp[1] <= 0 && b->part_hp[2] <= 0;
        if (!open) {
            if (!beam) {
                event(g, EV_SPARK, x, y, 0, 0);
                sound(g, SND_CLANG, 150);
            }
            return true;
        }
        /* open: the shell lets shots through to the core */
        if (d > 58.0f) return false;
        b->hp -= dmg;
        b->flash = 0.1f;
        if (!beam) sound(g, SND_BOSS_HIT, 170);
        if (b->hp <= 0) boss_die(g);
        return true;
    }
    /* the serpent */
    int tail = serpent_tail(g);
    (void)tail;
    for (int k = 0; k < AG_SEGS; k++) {
        if (b->seg_hp[k] <= 0) continue;
        float sx, sy;
        seg_pos(g, k, &sx, &sy);
        if (dist2(g, sx, sy, x, y, NULL, NULL) > 21.0f * 21.0f) continue;
        /* any ring can be shot off; the head only when they are all gone */
        b->seg_hp[k] -= dmg;
        b->seg_flash[k] = 0.1f;
        if (!beam) sound(g, SND_BOSS_HIT, 160);
        if (b->seg_hp[k] <= 0) {
            event(g, EV_BOSS_PART, sx, sy, 0, 1);
            sound(g, SND_BOOM_M, 230);
            shake(g, 5);
            add_score(g, 200);
            drop_crystals(g, sx, sy, 1);
        }
        return true;
    }
    if (d > b->r) return false;
    if (tail >= 0) {
        if (!beam) {
            event(g, EV_SPARK, x, y, 0, 0);
            sound(g, SND_CLANG, 140);
        }
        return true;
    }
    b->hp -= dmg;
    b->flash = 0.1f;
    if (!beam) sound(g, SND_BOSS_HIT, 170);
    if (b->hp <= 0) boss_die(g);
    return true;
}

static void boss_touch(ag_game_t *g, float x, float y, float r)
{
    for (int s = 0; s < g->players; s++) {
        ag_ship_t *sh = &g->ship[s];
        if (!sh->alive || sh->jump_t > 0 || sh->inv_t > 0) continue;
        float rr = r + AG_SHIP_R;
        if (dist2(g, x, y, sh->x, sh->y, NULL, NULL) < rr * rr) ship_hit(g, s, x, y, 5);
    }
}

static void boss_step(ag_game_t *g)
{
    ag_boss_t *b = &g->boss;
    if (!b->on) return;
    float fire_k = DIFF_FIRE[g->diff];
    b->t += AG_DT;
    if (b->flash > 0) b->flash -= AG_DT;
    for (int i = 0; i < 8; i++) {
        if (b->part_flash[i] > 0) b->part_flash[i] -= AG_DT;
    }
    for (int i = 0; i < AG_SEGS; i++) {
        if (b->seg_flash[i] > 0) b->seg_flash[i] -= AG_DT;
    }
    if (b->kind == BOSS_COLOSO) {
        if (b->state == 0) {
            /* coming in from the top */
            b->y += 60.0f * AG_DT;
            b->ang += b->spin * AG_DT;
            if (b->y >= b->r + 80) b->state = 1;
            return;
        }
        bool angry = b->hp < b->hp_max * 0.5f;
        b->ang += (angry ? 0.8f : b->spin) * AG_DT;
        b->x += b->vx * (angry ? 1.5f : 1.0f) * AG_DT;
        b->y += b->vy * (angry ? 1.5f : 1.0f) * AG_DT;
        if (b->x < b->r * 0.8f && b->vx < 0) b->vx = -b->vx;
        if (b->x > g->W - b->r * 0.8f && b->vx > 0) b->vx = -b->vx;
        /* not up under the score */
        if (b->y < b->r * 0.8f + 70.0f && b->vy < 0) b->vy = -b->vy;
        if (b->y > g->H - b->r * 0.8f && b->vy > 0) b->vy = -b->vy;
        b->t2 -= AG_DT;
        if (b->t2 <= 0) {
            b->t2 = (angry ? 2.4f : 3.4f) / fire_k;
            int n = angry ? 3 : 2;
            if (rock_count(g) < 26) {
                for (int k = 0; k < n; k++) {
                    float a = rndr(g, 0, TAU);
                    float v = ROCK_V[0] * rndr(g, 0.9f, 1.3f);
                    rock_new(g, RK_STONE, 0, b->x + cosf(a) * (b->r + 20), b->y + sinf(a) * (b->r + 20), cosf(a) * v,
                             sinf(a) * v);
                }
                sound(g, SND_BOOM_M, 150);
            }
        }
        boss_touch(g, b->x, b->y, 96.0f);
        for (int p = 0; p < 6; p++) {
            if (b->part_hp[p] <= 0) continue;
            float pa = b->ang + (p + 0.5f) * TAU / 6.0f;
            boss_touch(g, b->x + cosf(pa) * 106.0f, b->y + sinf(pa) * 106.0f, 20.0f);
        }
    } else if (b->kind == BOSS_COLMENA) {
        float ty = 210.0f + sinf(b->t * 0.9f) * 40.0f;
        if (b->state == 0) {
            b->y += 70.0f * AG_DT;
            if (b->y >= ty) {
                b->state = 1;
                b->t = 0;
            }
            return;
        }
        float span = g->W / 2 - 130.0f;
        if (span < 40) span = 40;
        b->x = g->W / 2 + sinf(b->t * 0.45f) * span;
        b->y = ty;
        b->ang += 0.25f * AG_DT;
        bool open = b->part_hp[0] <= 0 && b->part_hp[1] <= 0 && b->part_hp[2] <= 0;
        for (int p = 0; p < 3; p++) {
            if (b->part_hp[p] <= 0) continue;
            float ph = fmodf(b->t + p * 0.6f, 1.9f / fire_k);
            if (ph < AG_DT) {
                float pa = b->ang + (float)p * TAU / 3.0f;
                aim_shot(g, EB_SHOT, b->x + cosf(pa) * b->r, b->y + sinf(pa) * b->r, 250.0f, 0, 1, 3.0f);
            }
        }
        b->t2 -= AG_DT;
        if (b->t2 <= 0) {
            int seekers = 0;
            for (int i = 0; i < AG_ENEMIES; i++) seekers += g->en[i].on && g->en[i].kind == EN_SEEKER;
            b->t2 = (open ? 4.0f : 5.5f) / fire_k;
            if (seekers < 3) {
                ag_enemy_t *e = enemy_new(g, EN_SEEKER);
                if (e) {
                    e->x = b->x;
                    e->y = b->y + 30;
                    e->ang = PI / 2;
                    e->vx = 0;
                    e->vy = 120.0f;
                }
            }
        }
        if (open) {
            float ph = fmodf(b->t, 2.2f / fire_k);
            if (ph < AG_DT) {
                for (int k = 0; k < 8; k++) {
                    float a = b->t * 0.7f + (float)k * TAU / 8.0f;
                    ebul_new(g, EB_ORB, b->x, b->y, cosf(a) * 170.0f, sinf(a) * 170.0f, 4.0f);
                }
                sound(g, SND_ESHOT, 190);
            }
        }
        boss_touch(g, b->x, b->y, b->r * 0.85f);
    } else {
        /* the serpent: the head steers for a ship, the rings follow its trail */
        int tail = serpent_tail(g);
        float speed = tail < 0 ? 185.0f : 132.0f;
        if (b->state == 0) {
            b->y += speed * AG_DT;
            if (b->y > 120) b->state = 1;
        } else {
            float dx, dy;
            ag_ship_t *t = target(g, b->x, b->y, &dx, &dy);
            float want = t ? atan2f(dy, dx) : b->ang;
            want += sinf(b->t * 1.7f) * 0.8f;
            float da = angd(want - b->ang), m = 1.2f * AG_DT;
            b->ang += da > m ? m : da < -m ? -m : da;
            b->x = wrap(b->x + cosf(b->ang) * speed * AG_DT, g->W);
            b->y = wrap(b->y + sinf(b->ang) * speed * AG_DT, g->H);
        }
        b->trail_i = (b->trail_i + 1) & (AG_TRAIL - 1);
        b->trail_x[b->trail_i] = b->x;
        b->trail_y[b->trail_i] = b->y;
        if (b->state == 0) return;
        b->t2 -= AG_DT;
        if (b->t2 <= 0) {
            b->t2 = (tail < 0 ? 1.7f : 2.6f) / fire_k;
            aim_shot(g, EB_ORB, b->x, b->y, 190.0f, 0.5f, tail < 0 ? 3 : 2, 3.6f);
        }
        boss_touch(g, b->x, b->y, b->r);
        for (int k = 0; k < AG_SEGS; k++) {
            if (b->seg_hp[k] <= 0) continue;
            float sx, sy;
            seg_pos(g, k, &sx, &sy);
            boss_touch(g, sx, sy, 15.0f);
        }
    }
}

/* ---- powers and crystals ----------------------------------------------------------------------- */

static void picks_step(ag_game_t *g)
{
    for (int i = 0; i < AG_PICKS; i++) {
        ag_pick_t *p = &g->pick[i];
        if (!p->on) continue;
        p->life -= AG_DT;
        p->ang += 1.5f * AG_DT;
        if (p->life <= 0) {
            p->on = false;
            continue;
        }
        p->x = wrap(p->x + p->vx * AG_DT, g->W);
        p->y = wrap(p->y + p->vy * AG_DT, g->H);
        for (int s = 0; s < g->players; s++) {
            ag_ship_t *sh = &g->ship[s];
            if (!sh->alive || sh->jump_t > 0) continue;
            float rr = 30.0f + 8.0f * (float)g->upg[UP_MAGNET];
            if (dist2(g, p->x, p->y, sh->x, sh->y, NULL, NULL) > rr * rr) continue;
            switch (p->kind) {
            case PW_TRIPLE: sh->pw_t[PW_TRIPLE] = 12.0f; break;
            case PW_BEAM: sh->pw_t[PW_BEAM] = 7.0f; break;
            case PW_MAGNET: sh->pw_t[PW_MAGNET] = 15.0f; break;
            case PW_SHIELD:
                if (sh->shield < 2 + (g->upg[UP_ARMOR] > 1)) sh->shield++;
                sh->shield_t = 0.5f;
                release_lapas(g, s, 160.0f);
                break;
            case PW_LIFE:
                if (sh->lives < 6) sh->lives++;
                event(g, EV_LIFE, p->x, p->y, 0, 0);
                break;
            default: break;
            }
            event(g, EV_PICK, p->x, p->y, (float)p->kind, (float)s);
            sound(g, p->kind == PW_LIFE ? SND_LIFE : SND_PICK, 220);
            g->picks++;
            p->on = false;
            break;
        }
    }
}

static void cryst_step(ag_game_t *g)
{
    for (int i = 0; i < AG_CRYST; i++) {
        ag_cryst_t *c = &g->cryst[i];
        if (!c->on) continue;
        c->life -= AG_DT;
        if (c->life <= 0) {
            c->on = false;
            continue;
        }
        float fr = 1.0f - 1.6f * AG_DT;
        c->vx *= fr;
        c->vy *= fr;
        for (int s = 0; s < g->players; s++) {
            ag_ship_t *sh = &g->ship[s];
            if (!sh->alive || sh->jump_t > 0) continue;
            float dx, dy;
            float d2 = dist2(g, c->x, c->y, sh->x, sh->y, &dx, &dy);
            float pull = sh->pw_t[PW_MAGNET] > 0 ? 440.0f : 60.0f + 45.0f * (float)g->upg[UP_MAGNET];
            if (d2 < pull * pull && d2 > 1) {
                float d = sqrtf(d2);
                float k = (sh->pw_t[PW_MAGNET] > 0 ? 900.0f : 500.0f) * AG_DT / d;
                c->vx += dx * k;
                c->vy += dy * k;
            }
            float rr = 26.0f + 10.0f * (float)g->upg[UP_MAGNET];
            if (d2 < rr * rr) {
                c->on = false;
                g->crystals++;
                g->crystals_run++;
                event(g, EV_CRYSTAL, c->x, c->y, (float)s, 0);
                sound(g, SND_CRYSTAL, 120);
                break;
            }
        }
        c->x = wrap(c->x + c->vx * AG_DT, g->W);
        c->y = wrap(c->y + c->vy * AG_DT, g->H);
    }
}

/* ---- waves -------------------------------------------------------------------------------------- */

static void wave_setup(ag_game_t *g)
{
    int big, mid = 0;
    uint8_t rocks, enemies;
    int boss = BOSS_NONE;
    if (g->mode == AG_CAMPAIGN) {
        const ag_sector_t *s = &AG_SECTOR[g->sector - 1];
        g->speed_k = s->speed * DIFF_SPEED[g->diff];
        big = s->big + (g->wave - 1) / 2;
        mid = g->wave - 1;
        rocks = s->rocks;
        enemies = s->enemies;
        if (s->boss && g->wave == s->waves) {
            boss = s->boss;
            big = 2;
            mid = 0;
            rocks = R_(RK_STONE);
        }
    } else {
        int w = g->sector;
        float k = 1.0f + 0.045f * (float)(w - 1);
        g->speed_k = (k > 1.8f ? 1.8f : k) * DIFF_SPEED[g->diff];
        big = 3 + (w - 1) / 2;
        if (big > 8) big = 8;
        mid = (w - 1) % 3;
        rocks = R_(RK_STONE);
        if (w >= 2) rocks |= R_(RK_TRIO);
        if (w >= 3) rocks |= R_(RK_METAL);
        if (w >= 5) rocks |= R_(RK_VOLATILE);
        if (w >= 7) rocks |= R_(RK_LAPA);
        enemies = 0;
        if (w >= 2) enemies |= R_(EN_SEEKER);
        if (w >= 4) enemies |= R_(EN_LAYER);
        if (w >= 6) enemies |= R_(EN_LANCER);
        if (w >= 8) enemies |= R_(EN_WEAVER);
        if (w % 6 == 0) {
            static const int ORDER[3] = { BOSS_COLOSO, BOSS_COLMENA, BOSS_SERPIENTE };
            boss = ORDER[(w / 6 - 1) % 3];
            big = 2;
            mid = 0;
            rocks = R_(RK_STONE);
        }
    }
    g->rock_kinds = rocks;
    g->enemy_kinds = enemies;
    g->boss_kind = boss;
    for (int i = 0; i < big; i++) {
        int t = pick_type(g, rocks);
        spawn_rock_edge(g, t, t == RK_LAPA ? 1 : 2);
    }
    for (int i = 0; i < mid; i++) {
        int t = pick_type(g, rocks);
        spawn_rock_edge(g, t, 1);
    }
    int lvl = g->mode == AG_CAMPAIGN ? g->sector : (g->sector + 1) / 2 + 1;
    g->enemy_left = enemies && !boss ? 1 + (g->wave - 1) / 2 + lvl / 4 : 0;
    if (g->enemy_left > 5) g->enemy_left = 5;
    g->enemy_t = rndr(g, 6.0f, 9.0f) * DIFF_ENEMY[g->diff];
    g->since_kill = 0;
}

static void wave_begin(ag_game_t *g)
{
    g->boss.on = false;
    wave_setup(g);
    if (g->boss_kind) {
        g->state = AG_ST_BOSS_IN;
        sound(g, SND_ALARM, 230);
    } else {
        g->state = AG_ST_INTRO;
    }
    g->state_t = 0;
}

static bool wave_clear(const ag_game_t *g)
{
    if (g->enemy_left > 0 || g->boss.on) return false;
    for (int i = 0; i < AG_ROCKS; i++) {
        if (g->rock[i].on && g->rock[i].stuck < 0) return false;
    }
    for (int i = 0; i < AG_ENEMIES; i++) {
        if (g->en[i].on) return false;
    }
    return true;
}

static void spawn_enemies(ag_game_t *g)
{
    if (g->enemy_left <= 0 || !g->enemy_kinds) return;
    g->enemy_t -= AG_DT;
    if (g->enemy_t > 0) return;
    int alive = 0;
    for (int i = 0; i < AG_ENEMIES; i++) alive += g->en[i].on;
    if (alive >= 3) {
        g->enemy_t = 1.0f;
        return;
    }
    int kinds[EN_N], n = 0;
    for (int k = 0; k < EN_N; k++) {
        if (g->enemy_kinds & R_(k)) kinds[n++] = k;
    }
    enemy_new(g, kinds[(int)(rnd(g) * (float)n) % n]);
    g->enemy_left--;
    float base = 9.0f - (float)(g->mode == AG_CAMPAIGN ? g->sector : g->sector / 2) * 0.35f;
    if (base < 4.5f) base = 4.5f;
    g->enemy_t = base * rndr(g, 0.8f, 1.25f) * DIFF_ENEMY[g->diff];
}

/* ---- the game ------------------------------------------------------------------------------------- */

void ag_new(ag_game_t *g, int mode, int players, int diff, float w, float h, uint32_t seed)
{
    memset(g, 0, sizeof *g);
    g->W = w;
    g->H = h;
    g->mode = mode;
    g->players = players < 1 ? 1 : players > 2 ? 2 : players;
    g->diff = diff < 0 || diff >= AG_DIFF_N ? AG_NORMAL : diff;
    g->rnd = seed ? seed : 0x12345u;
    g->next_life = 10000;
    for (int s = 0; s < g->players; s++) {
        g->ship[s].in_play = true;
        g->ship[s].lives = diff == AG_EASY ? 4 : 3;
    }
    for (int i = 0; i < AG_ROCKS; i++) g->rock[i].stuck = -1;
    g->sector = 1;
}

void ag_sector_start(ag_game_t *g, int sector)
{
    g->sector = sector < 1 ? 1 : sector;
    if (g->mode == AG_CAMPAIGN && g->sector > AG_SECTORS) g->sector = AG_SECTORS;
    g->wave = 1;
    g->waves = g->mode == AG_CAMPAIGN ? AG_SECTOR[g->sector - 1].waves : 1;
    memset(g->rock, 0, sizeof g->rock);
    for (int i = 0; i < AG_ROCKS; i++) g->rock[i].stuck = -1;
    memset(g->shot, 0, sizeof g->shot);
    memset(g->ebul, 0, sizeof g->ebul);
    memset(g->en, 0, sizeof g->en);
    memset(g->pick, 0, sizeof g->pick);
    memset(g->cryst, 0, sizeof g->cryst);
    memset(&g->boss, 0, sizeof g->boss);
    g->crystals_run = 0;
    for (int s = 0; s < g->players; s++) {
        ag_ship_t *sh = &g->ship[s];
        if (!sh->in_play) continue;
        int lives = sh->lives;
        memset(sh, 0, sizeof *sh);
        sh->in_play = true;
        sh->lives = lives;
        ship_spawn(g, s);
        sh->shield = g->upg[UP_ARMOR];
        sh->inv_t = 1.5f;
    }
    wave_begin(g);
}

void ag_resize(ag_game_t *g, float w, float h)
{
    if (w < 1 || h < 1 || (w == g->W && h == g->H)) return;
    float kx = w / g->W, ky = h / g->H;
#define SC(o) do { (o).x *= kx; (o).y *= ky; } while (0)
    for (int i = 0; i < 2; i++) {
        SC(g->ship[i]);
        g->ship[i].jx *= kx;
        g->ship[i].jy *= ky;
    }
    for (int i = 0; i < AG_ROCKS; i++) SC(g->rock[i]);
    for (int i = 0; i < AG_SHOTS; i++) SC(g->shot[i]);
    for (int i = 0; i < AG_EBULS; i++) SC(g->ebul[i]);
    for (int i = 0; i < AG_ENEMIES; i++) {
        SC(g->en[i]);
        g->en[i].tx *= kx;
        g->en[i].ty *= ky;
    }
    for (int i = 0; i < AG_PICKS; i++) SC(g->pick[i]);
    for (int i = 0; i < AG_CRYST; i++) SC(g->cryst[i]);
    SC(g->boss);
    for (int i = 0; i < AG_TRAIL; i++) {
        g->boss.trail_x[i] *= kx;
        g->boss.trail_y[i] *= ky;
    }
#undef SC
    g->W = w;
    g->H = h;
}

void ag_step(ag_game_t *g, const ag_hand_t *h0, const ag_hand_t *h1)
{
    g->t += AG_DT;
    g->state_t += AG_DT;
    g->since_kill += AG_DT;
    if (g->state == AG_ST_DONE || g->state == AG_ST_WON) return;
    bool live = g->state != AG_ST_OVER;
    for (int s = 0; s < g->players; s++) ship_step(g, s, live ? (s ? h1 : h0) : NULL);
    rocks_step(g);
    shots_step(g);
    enemies_step(g);
    ebuls_step(g);
    boss_step(g);
    picks_step(g);
    cryst_step(g);
    if (g->state == AG_ST_OVER) return;
    /* out of lives */
    bool any = false;
    for (int s = 0; s < g->players; s++) any |= g->ship[s].in_play;
    if (!any) {
        g->state = AG_ST_OVER;
        g->state_t = 0;
        return;
    }
    switch (g->state) {
    case AG_ST_INTRO:
        if (g->state_t > 1.8f) {
            g->state = AG_ST_PLAY;
            g->state_t = 0;
        }
        break;
    case AG_ST_BOSS_IN:
        if (g->state_t > 2.6f) {
            boss_start(g, g->boss_kind);
            g->state = AG_ST_PLAY;
            g->state_t = 0;
        }
        break;
    case AG_ST_PLAY:
        spawn_enemies(g);
        if (wave_clear(g)) {
            g->state = AG_ST_CLEAR;
            g->state_t = 0;
            sound(g, SND_WAVE, 200);
        }
        break;
    case AG_ST_CLEAR:
        if (g->state_t > 2.4f) {
            if (g->mode == AG_ARCADE) {
                g->sector++;
                g->wave = 1;
                wave_begin(g);
            } else if (g->wave < g->waves) {
                g->wave++;
                wave_begin(g);
            } else {
                g->state = g->sector >= AG_SECTORS ? AG_ST_WON : AG_ST_DONE;
                g->state_t = 0;
            }
        }
        break;
    default: break;
    }
}

/* the title's still: a few of each kind of rock around a ship, crystals */
void ag_title_scene(ag_game_t *g, int sector)
{
    ag_new(g, AG_CAMPAIGN, 1, AG_NORMAL, g->W, g->H, 0xBEEF);
    g->sector = sector;
    g->state = AG_ST_PLAY;
    g->speed_k = 1.0f;
    typedef struct {
        float x, y;
        int type, size;
    } place_t;
    /* upright the buttons take the lower half, lying down the middle */
    static const place_t UP[] = {
        { 0.13f, 0.40f, RK_STONE, 2 }, { 0.84f, 0.33f, RK_METAL, 2 }, { 0.30f, 0.06f, RK_TRIO, 1 },
        { 0.92f, 0.62f, RK_VOLATILE, 1 }, { 0.08f, 0.62f, RK_LAPA, 1 }, { 0.70f, 0.08f, RK_STONE, 1 },
        { 0.64f, 0.27f, RK_STONE, 0 }, { 0.06f, 0.94f, RK_TRIO, 0 }, { 0.94f, 0.92f, RK_STONE, 0 },
    };
    static const place_t LAND[] = {
        { 0.08f, 0.30f, RK_STONE, 2 }, { 0.90f, 0.40f, RK_METAL, 2 }, { 0.20f, 0.80f, RK_TRIO, 1 },
        { 0.78f, 0.84f, RK_VOLATILE, 1 }, { 0.06f, 0.62f, RK_LAPA, 1 }, { 0.93f, 0.12f, RK_STONE, 1 },
        { 0.25f, 0.50f, RK_STONE, 0 }, { 0.74f, 0.60f, RK_TRIO, 0 }, { 0.12f, 0.08f, RK_STONE, 0 },
    };
    bool land = g->W > g->H;
    const place_t *R = land ? LAND : UP;
    for (int i = 0; i < 9; i++) {
        ag_rock_t *r = rock_new(g, R[i].type, R[i].size, R[i].x * g->W, R[i].y * g->H, 0, 0);
        if (r) r->ang = (float)i * 0.7f;
    }
    ag_ship_t *sh = &g->ship[0];
    sh->alive = true;
    sh->x = land ? g->W * 0.16f : g->W * 0.38f;
    sh->y = land ? g->H * 0.48f : g->H * 0.33f;
    sh->ang = land ? -0.25f : -PI / 2 + 0.6f;
    sh->thrust = 1.0f;
    for (int k = 0; k < 6; k++) {
        ag_cryst_t *c = &g->cryst[k];
        c->on = true;
        c->x = land ? g->W * (0.06f + 0.022f * (float)k) : g->W * (0.12f + 0.04f * (float)k);
        c->y = land ? g->H * (0.92f + 0.02f * (float)(k & 1)) : g->H * (0.50f + 0.015f * (float)(k & 1));
        c->life = 9.0f;
    }
    for (int k = 0; k < 4; k++) {
        ag_shot_t *b = &g->shot[k];
        b->on = true;
        float a = sh->ang;
        b->x = sh->x + cosf(a) * (40.0f + 42.0f * (float)k);
        b->y = sh->y + sinf(a) * (40.0f + 42.0f * (float)k);
        b->vx = cosf(a) * 700.0f;
        b->vy = sinf(a) * 700.0f;
        b->life = 1.0f;
    }
}

/* straight to wave w of the sector (the simulator's switches) */
void ag_wave_jump(ag_game_t *g, int w)
{
    memset(g->rock, 0, sizeof g->rock);
    for (int i = 0; i < AG_ROCKS; i++) g->rock[i].stuck = -1;
    memset(g->en, 0, sizeof g->en);
    g->wave = w < 1 ? 1 : w > g->waves ? g->waves : w;
    wave_begin(g);
}

int ag_rocks_on(const ag_game_t *g)
{
    return rock_count(g);
}
