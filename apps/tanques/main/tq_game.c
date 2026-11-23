/*
 * TANQUES - the battle without pixels (see tq_game.h)
 */
#include "tq_game.h"
#include "tq_art.h"

#include "aos_i18n.h"

#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#define GRAV       9.8f
#define M_PER_AP   1.6f             /* metres of flat ground an AP drives */
#define STEP       (1.0f / 120.0f)  /* the shells' physics step */

const tq_weapon_t TQ_WEAPON[TQ_WEAPONS] = {
    { N_("Cañón"),   3, 23.0f, 2.2f, 30, 1.3f, 0.45f, false },
    { N_("Mortero"), 5, 15.0f, 3.0f, 38, 1.8f, 0.70f, true },
};

static uint32_t s_rng = 0x9E3779B9u;

float tq_rand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xFFFFFF) * (1.0f / 16777216.0f);
}

static float randr(float a, float b)
{
    return a + (b - a) * tq_rand();
}

float tq_tank_z(const tq_terrain_t *t, const tq_tank_t *k)
{
    /* the tank sits on its highest points, not sunk into the slope */
    float c = tq_ter_height(t, k->x, k->y);
    float cs = cosf(k->yaw), sn = sinf(k->yaw), m = c;
    static const float P[4][2] = { { 1.1f, 0.8f }, { 1.1f, -0.8f }, { -1.1f, 0.8f }, { -1.1f, -0.8f } };
    float sum = 0;
    for (int i = 0; i < 4; i++) {
        float h = tq_ter_height(t, k->x + cs * P[i][0] - sn * P[i][1], k->y + sn * P[i][0] + cs * P[i][1]);
        sum += h;
        if (h > m) m = h;
    }
    float avg = (sum + c) * 0.2f;
    return avg + (m - avg) * 0.5f;
}

/* --------------------------------------------------------------------------
 * The map
 * -------------------------------------------------------------------------- */

static float block_support(const tq_state_t *s, const tq_terrain_t *t, int self)
{
    const tq_block_t *b = &s->block[self];
    float sup = tq_ter_height(t, b->i + 0.5f, b->j + 0.5f);
    for (int k = 0; k < s->nblock; k++) {
        const tq_block_t *o = &s->block[k];
        if (k == self || !o->alive || o->i != b->i || o->j != b->j) continue;
        if (o->z < b->z - 0.01f && o->z + TQ_BLOCK_M > sup) sup = o->z + TQ_BLOCK_M;
    }
    return sup;
}

static void add_block(tq_state_t *s, const tq_terrain_t *t, int i, int j, int level)
{
    if (s->nblock >= TQ_MAX_BLOCKS) return;
    tq_block_t *b = &s->block[s->nblock++];
    b->i = (int8_t)i;
    b->j = (int8_t)j;
    b->variant = (uint8_t)((i * 7 + j * 3 + level) & 1);
    b->alive = 1;
    b->z = tq_ter_height(t, i + 0.5f, j + 0.5f) + level * TQ_BLOCK_M - 0.05f;
    b->vz = 0;
}

void tq_game_new(tq_state_t *s, tq_terrain_t *t, uint32_t seed, int scheme0, int scheme1, bool vs_ai)
{
    memset(s, 0, sizeof *s);
    s_rng = seed * 2654435761u + 12345u;
    tq_ter_generate(t, seed);

    /* the tanks: south-west and north-east */
    tq_tank_t *a = &s->tank[0], *b = &s->tank[1];
    a->x = 7.5f; a->y = 7.0f; a->yaw = 45 * TQ_DEG;
    b->x = 28.5f; b->y = 29.0f; b->yaw = 225 * TQ_DEG;
    for (int i = 0; i < 2; i++) {
        tq_tank_t *k = &s->tank[i];
        k->tyaw = k->yaw;
        k->pitch = 10 * TQ_DEG;
        k->hp = k->hpmax = 100;
        k->ap = k->apmax = 10;
        k->weapon = TQ_W_CANNON;
    }
    a->scheme = scheme0;
    b->scheme = scheme1;
    b->ai = vs_ai;

    /* stone walls: one across the middle hill's shoulder, one by the hollow */
    for (int i = 13; i <= 18; i++) add_block(s, t, i, 14, 0);
    for (int i = 14; i <= 16; i++) add_block(s, t, i, 14, 1);
    for (int j = 22; j <= 26; j++) add_block(s, t, 23, j, 0);
    add_block(s, t, 23, 24, 1);
    for (int i = 8; i <= 10; i++) add_block(s, t, i, 21, 0);

    /* trees where there is room: off the road, the walls and the tanks */
    for (int tries = 0; tries < 400 && s->ntree < 22; tries++) {
        float x = randr(3.5f, 32.5f), y = randr(3.5f, 32.5f);
        bool ok = true;
        for (int k = 0; k < 2 && ok; k++) {
            float dx = x - s->tank[k].x, dy = y - s->tank[k].y;
            if (dx * dx + dy * dy < 5.0f * 5.0f) ok = false;
        }
        for (int k = 0; k < s->ntree && ok; k++) {
            float dx = x - s->tree[k].x, dy = y - s->tree[k].y;
            if (dx * dx + dy * dy < 2.6f * 2.6f) ok = false;
        }
        for (int k = 0; k < s->nblock && ok; k++) {
            float dx = x - (s->block[k].i + 0.5f), dy = y - (s->block[k].j + 0.5f);
            if (dx * dx + dy * dy < 2.5f * 2.5f) ok = false;
        }
        int ci = (int)(x / TQ_CELL), cj = (int)(y / TQ_CELL);
        if (ok && t->mat[cj * TQ_NV + ci] != TQ_MAT_GRASS) ok = false;
        if (!ok) continue;
        tq_tree_t *tr = &s->tree[s->ntree++];
        tr->x = x;
        tr->y = y;
        tr->kind = tq_rand() < 0.55f ? TQ_PROP_ROUND : TQ_PROP_PINE;
        tr->alive = 1;
    }
    s->cur = 0;
}

/* --------------------------------------------------------------------------
 * Driving
 * -------------------------------------------------------------------------- */

static uint16_t s_cost[TQ_GRID * TQ_GRID];
static int16_t s_parent[TQ_GRID * TQ_GRID];
static uint8_t s_block[TQ_GRID * TQ_GRID];

static void build_blocked(const tq_state_t *s, int who)
{
    memset(s_block, 0, sizeof s_block);
    for (int j = 0; j < TQ_GRID; j++) {
        for (int i = 0; i < TQ_GRID; i++) {
            float x = i + 0.5f, y = j + 0.5f;
            bool b = i < 2 || j < 2 || i > TQ_GRID - 3 || j > TQ_GRID - 3;
            for (int k = 0; k < s->nblock && !b; k++) {
                const tq_block_t *o = &s->block[k];
                if (!o->alive) continue;
                float dx = x - (o->i + 0.5f), dy = y - (o->j + 0.5f);
                if (dx * dx + dy * dy < 1.45f * 1.45f) b = true;
            }
            for (int k = 0; k < s->ntree && !b; k++) {
                const tq_tree_t *o = &s->tree[k];
                float dx = x - o->x, dy = y - o->y;
                if (dx * dx + dy * dy < 1.3f * 1.3f) b = true;
            }
            const tq_tank_t *e = &s->tank[1 - who];
            float dx = x - e->x, dy = y - e->y;
            if (dx * dx + dy * dy < 2.8f * 2.8f) b = true;
            s_block[j * TQ_GRID + i] = b;
        }
    }
}

void tq_game_reach(tq_state_t *s, const tq_terrain_t *t)
{
    const tq_tank_t *k = &s->tank[s->cur];
    build_blocked(s, s->cur);
    for (int i = 0; i < TQ_GRID * TQ_GRID; i++) {
        s_cost[i] = 0xFFFF;
        s_parent[i] = -1;
    }
    memset(s->reach, 0, sizeof s->reach);
    int si = (int)k->x, sj = (int)k->y;
    if (si < 0 || sj < 0 || si >= TQ_GRID || sj >= TQ_GRID) return;
    int start = sj * TQ_GRID + si;
    int budget = k->ap * 16;
    s_cost[start] = 0;
    /* a small Dijkstra: 1296 cells, a linear scan for the open minimum is
     * plenty */
    static uint8_t done[TQ_GRID * TQ_GRID];
    memset(done, 0, sizeof done);
    static const int8_t DI[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DJ[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    for (;;) {
        int best = -1;
        uint16_t bc = 0xFFFF;
        for (int c = 0; c < TQ_GRID * TQ_GRID; c++)
            if (!done[c] && s_cost[c] < bc) { bc = s_cost[c]; best = c; }
        if (best < 0 || bc > budget) break;
        done[best] = 1;
        int bi = best % TQ_GRID, bj = best / TQ_GRID;
        float hb = tq_ter_height(t, bi + 0.5f, bj + 0.5f);
        for (int d = 0; d < 8; d++) {
            int ni = bi + DI[d], nj = bj + DJ[d];
            if (ni < 0 || nj < 0 || ni >= TQ_GRID || nj >= TQ_GRID) continue;
            int n = nj * TQ_GRID + ni;
            if (done[n] || s_block[n]) continue;
            /* no cutting corners past a blocked cell */
            if (d >= 4 && (s_block[bj * TQ_GRID + ni] || s_block[nj * TQ_GRID + bi])) continue;
            float dist = d < 4 ? 1.0f : 1.4142f;
            float dh = tq_ter_height(t, ni + 0.5f, nj + 0.5f) - hb;
            float slope = dh / dist;
            if (slope > 0.85f || slope < -0.95f) continue;           /* too steep */
            float c = dist / M_PER_AP * (1.0f + (slope > 0 ? 2.2f * slope : 0.0f));
            int nc = bc + (int)(c * 16.0f + 0.5f);
            if (nc < s_cost[n]) {
                s_cost[n] = (uint16_t)nc;
                s_parent[n] = (int16_t)best;
            }
        }
    }
    int cheap = TQ_WEAPON[TQ_W_CANNON].ap * 16, dear = TQ_WEAPON[TQ_W_MORTAR].ap * 16;
    for (int c = 0; c < TQ_GRID * TQ_GRID; c++) {
        if (!done[c] || c == start) continue;
        int left = budget - s_cost[c];
        s->reach[c] = left >= dear ? 1 : (left >= cheap ? 2 : 3);
    }
}

static bool line_clear(const tq_state_t *s, float x0, float y0, float x1, float y1)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    int n = (int)(len / 0.25f) + 1;
    for (int i = 1; i < n; i++) {
        float f = (float)i / n;
        int ci = (int)(x0 + dx * f), cj = (int)(y0 + dy * f);
        if (ci < 0 || cj < 0 || ci >= TQ_GRID || cj >= TQ_GRID) return false;
        if (!s->reach[cj * TQ_GRID + ci] && !(ci == (int)x0 && cj == (int)y0)) return false;
    }
    return true;
}

bool tq_game_route(tq_state_t *s, const tq_terrain_t *t, float x, float y, int *ap)
{
    (void)t;
    s->npath = 0;
    int ci = (int)x, cj = (int)y;
    if (ci < 0 || cj < 0 || ci >= TQ_GRID || cj >= TQ_GRID) return false;
    int c = cj * TQ_GRID + ci;
    if (!s->reach[c]) return false;
    int cells[TQ_GRID * 4];
    int n = 0;
    for (int k = c; k >= 0 && n < (int)(sizeof cells / sizeof cells[0]); k = s_parent[k]) cells[n++] = k;
    /* cells from the tank to the goal, then pulled straight where it can */
    float pts[TQ_GRID * 4][2];
    int np = 0;
    const tq_tank_t *k = &s->tank[s->cur];
    pts[np][0] = k->x;
    pts[np][1] = k->y;
    np++;
    for (int i = n - 2; i >= 0; i--) {
        pts[np][0] = cells[i] % TQ_GRID + 0.5f;
        pts[np][1] = cells[i] / TQ_GRID + 0.5f;
        np++;
    }
    int a = 0;
    s->path[s->npath][0] = pts[0][0];
    s->path[s->npath][1] = pts[0][1];
    s->npath++;
    while (a < np - 1 && s->npath < TQ_MAX_PATH) {
        int b = np - 1;
        while (b > a + 1 && !line_clear(s, pts[a][0], pts[a][1], pts[b][0], pts[b][1])) b--;
        s->path[s->npath][0] = pts[b][0];
        s->path[s->npath][1] = pts[b][1];
        s->npath++;
        a = b;
    }
    if (ap) *ap = (s_cost[c] + 15) / 16;
    return s->npath >= 2;
}

/* --------------------------------------------------------------------------
 * Shells
 * -------------------------------------------------------------------------- */

void tq_game_muzzle(const tq_tank_t *k, const tq_terrain_t *t, tq_v3_t *p, tq_v3_t *v, int weapon)
{
    float cp = cosf(k->pitch), sp = sinf(k->pitch);
    float cy = cosf(k->tyaw), sy = sinf(k->tyaw);
    float fwd = 0.64f + 1.40f * cp, up = 1.38f + 1.40f * sp;
    float z = tq_tank_z(t, k);
    *p = tq_v3(k->x + cy * fwd, k->y + sy * fwd, z + up);
    float sp_ = TQ_WEAPON[weapon].speed;
    *v = tq_v3(sp_ * cp * cy, sp_ * cp * sy, sp_ * sp);
}

enum { HIT_NONE = 0, HIT_GROUND, HIT_TREE, HIT_BLOCK, HIT_TANK, HIT_OUT };

static int hit_test(const tq_state_t *s, const tq_terrain_t *t, tq_v3_t p, int owner, float age, int *which)
{
    if (p.x < -3 || p.y < -3 || p.x > TQ_MAP_M + 3 || p.y > TQ_MAP_M + 3 || p.z < TQ_BASE_Z - 3) return HIT_OUT;
    for (int k = 0; k < 2; k++) {
        if (k == owner && age < 0.25f) continue;
        const tq_tank_t *tk = &s->tank[k];
        if (tk->hp <= 0) continue;
        float dx = (p.x - tk->x) / 1.45f, dy = (p.y - tk->y) / 1.45f, dz = (p.z - tq_tank_z(t, tk) - 0.85f) / 0.95f;
        if (dx * dx + dy * dy + dz * dz < 1) {
            *which = k;
            return HIT_TANK;
        }
    }
    bool in_map = p.x >= 0 && p.y >= 0 && p.x <= TQ_MAP_M && p.y <= TQ_MAP_M;
    float g = in_map ? tq_ter_height(t, p.x, p.y) : TQ_BASE_Z;
    if (p.z <= g) return HIT_GROUND;
    for (int k = 0; k < s->ntree; k++) {
        const tq_tree_t *tr = &s->tree[k];
        if (!tr->alive) continue;
        float dx = p.x - tr->x, dy = p.y - tr->y, d2 = dx * dx + dy * dy;
        if (d2 > 1.3f * 1.3f) continue;
        float h = tq_ter_height(t, tr->x, tr->y);
        if ((d2 < 1.0f && p.z > h + 1.3f && p.z < h + 3.7f) || (d2 < 0.3f * 0.3f && p.z < h + 1.7f)) {
            *which = k;
            return HIT_TREE;
        }
    }
    for (int k = 0; k < s->nblock; k++) {
        const tq_block_t *b = &s->block[k];
        if (!b->alive) continue;
        if (fabsf(p.x - (b->i + 0.5f)) < 0.5f && fabsf(p.y - (b->j + 0.5f)) < 0.5f && p.z > b->z &&
            p.z < b->z + TQ_BLOCK_M) {
            *which = k;
            return HIT_BLOCK;
        }
    }
    return HIT_NONE;
}

/* Flies a shell from p with v; returns what it hit and where. */
static int fly(const tq_state_t *s, const tq_terrain_t *t, tq_v3_t p, tq_v3_t v, int owner, tq_v3_t *end,
               tq_v3_t *pts, int *npts, int maxpts)
{
    int which = -1, n = 0;
    for (int i = 0; i < 1200; i++) {
        v.z -= GRAV * STEP;
        p.x += v.x * STEP;
        p.y += v.y * STEP;
        p.z += v.z * STEP;
        if (pts && n < maxpts && (i % 4) == 3) pts[n++] = p;
        int h = hit_test(s, t, p, owner, i * STEP, &which);
        if (h) {
            *end = p;
            if (npts) *npts = n;
            return h;
        }
    }
    *end = p;
    if (npts) *npts = n;
    return HIT_OUT;
}

void tq_game_arc(tq_state_t *s, const tq_terrain_t *t)
{
    const tq_tank_t *k = &s->tank[s->cur];
    tq_v3_t p, v;
    tq_game_muzzle(k, t, &p, &v, k->weapon);
    int n = 0;
    int h = fly(s, t, p, v, s->cur, &s->arc_end, s->arc, &n, TQ_MAX_ARC);
    s->narc = n;
    s->arc_landed = h != HIT_OUT;
    s->arc_full = TQ_WEAPON[k->weapon].full_guide;
    if (!s->arc_full) {
        /* the cannon's guide: the first stretch only */
        int lim = n * 2 / 5;
        if (lim < 6) lim = n < 6 ? n : 6;
        s->narc = lim;
    }
}

void tq_game_fire(tq_state_t *s, const tq_terrain_t *t)
{
    tq_tank_t *k = &s->tank[s->cur];
    tq_shell_t *sh = &s->shell;
    tq_game_muzzle(k, t, &sh->p, &sh->v, k->weapon);
    sh->live = true;
    sh->weapon = k->weapon;
    sh->owner = s->cur;
    sh->t = 0;
    k->ap -= TQ_WEAPON[k->weapon].ap;
    if (k->ap < 0) k->ap = 0;
    /* the muzzle's flash and smoke */
    for (int i = 0; i < 6 && s->npart < TQ_MAX_PARTS; i++) {
        tq_part_t *q = &s->part[s->npart++];
        q->p = sh->p;
        q->v = tq_v3(sh->v.x * 0.06f + randr(-0.5f, 0.5f), sh->v.y * 0.06f + randr(-0.5f, 0.5f), randr(0.2f, 0.9f));
        q->age = 0;
        q->life = i < 2 ? 0.18f : randr(0.6f, 1.0f);
        q->r0 = i < 2 ? 0.35f : 0.2f;
        q->r1 = i < 2 ? 0.6f : randr(0.5f, 0.8f);
        q->kind = i < 2 ? TQ_PART_FIRE : TQ_PART_SMOKE;
        q->rgb = 0xE8E4DC;
    }
}

static void add_part(tq_state_t *s, int kind, tq_v3_t p, tq_v3_t v, float life, float r0, float r1, uint32_t rgb)
{
    if (s->npart >= TQ_MAX_PARTS) return;
    tq_part_t *q = &s->part[s->npart++];
    q->p = p;
    q->v = v;
    q->age = 0;
    q->life = life;
    q->r0 = r0;
    q->r1 = r1;
    q->rgb = rgb;
    q->kind = (uint8_t)kind;
}

bool tq_game_step(tq_state_t *s, const tq_terrain_t *t, float dt, tq_v3_t *at)
{
    bool landed = false;
    tq_shell_t *sh = &s->shell;
    if (sh->live) {
        int steps = (int)(dt / STEP + 0.5f);
        if (steps < 1) steps = 1;
        for (int i = 0; i < steps && sh->live; i++) {
            sh->v.z -= GRAV * STEP;
            sh->p.x += sh->v.x * STEP;
            sh->p.y += sh->v.y * STEP;
            sh->p.z += sh->v.z * STEP;
            sh->t += STEP;
            int which;
            int h = hit_test(s, t, sh->p, sh->owner, sh->t, &which);
            if (h) {
                sh->live = false;
                if (h != HIT_OUT) {
                    landed = true;
                    *at = sh->p;
                }
            }
        }
        /* a trail of little puffs */
        if (sh->live && s->npart < TQ_MAX_PARTS - 40)
            add_part(s, TQ_PART_SMOKE, sh->p, tq_v3(randr(-0.2f, 0.2f), randr(-0.2f, 0.2f), 0.3f), 0.7f, 0.10f, 0.32f,
                     0xF0EEE8);
        if (!sh->live && !landed) s->shell.t = -1;      /* flew off the map */
    }
    /* the particles */
    for (int i = 0; i < s->npart;) {
        tq_part_t *q = &s->part[i];
        q->age += dt;
        if (q->age >= q->life) {
            s->part[i] = s->part[--s->npart];
            continue;
        }
        if (q->kind == TQ_PART_CHUNK || q->kind == TQ_PART_LEAF) {
            q->v.z -= GRAV * dt * (q->kind == TQ_PART_LEAF ? 0.35f : 1.0f);
            q->p.x += q->v.x * dt;
            q->p.y += q->v.y * dt;
            q->p.z += q->v.z * dt;
            float g = tq_ter_height(t, q->p.x, q->p.y);
            if (q->p.z < g + q->r0) {
                q->p.z = g + q->r0;
                q->v.x *= 0.5f;
                q->v.y *= 0.5f;
                q->v.z = -q->v.z * 0.3f;
            }
        } else if (q->kind == TQ_PART_RING || q->kind == TQ_PART_FLASH) {
            /* they only grow */
        } else {
            float k = 1.0f - 1.8f * dt;
            q->v.x *= k;
            q->v.y *= k;
            q->v.z = q->v.z * k + (q->kind == TQ_PART_SMOKE ? 0.6f : 0.3f) * dt;
            q->p.x += q->v.x * dt;
            q->p.y += q->v.y * dt;
            q->p.z += q->v.z * dt;
        }
        i++;
    }
    /* stones with nothing under them fall */
    for (int k = 0; k < s->nblock; k++) {
        tq_block_t *b = &s->block[k];
        if (!b->alive) continue;
        float sup = block_support(s, t, k);
        if (b->z > sup + 0.005f) {
            b->vz -= GRAV * dt;
            b->z += b->vz * dt;
            if (b->z <= sup) {
                b->z = sup;
                b->vz = 0;
            }
        } else if (b->z < sup - 0.3f) {
            b->z = sup;                 /* the ground rose under it (a rim) */
        }
    }
    for (int k = 0; k < 2; k++)
        if (s->tank[k].hit_flash > 0) s->tank[k].hit_flash -= dt;
    return landed;
}

void tq_game_blast(tq_state_t *s, tq_terrain_t *t, tq_v3_t at, int weapon, int owner,
                   tq_blast_t *out, tq_vbox_t *dirty)
{
    (void)owner;
    const tq_weapon_t *w = &TQ_WEAPON[weapon];
    memset(out, 0, sizeof *out);
    dirty->x0 = dirty->y0 = 1;
    dirty->x1 = dirty->y1 = 0;                          /* empty */
    for (int k = 0; k < 2; k++) {
        tq_tank_t *tk = &s->tank[k];
        if (tk->hp <= 0) continue;
        float dx = at.x - tk->x, dy = at.y - tk->y, dz = at.z - (tq_tank_z(t, tk) + 0.8f);
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        bool direct = d < 1.6f;
        if (d < w->radius || direct) {
            int dmg = direct ? w->damage + 8 : (int)(w->damage * (1.0f - 0.75f * d / w->radius) + 0.5f);
            if (dmg < 1) dmg = 1;
            tk->hp -= dmg;
            if (tk->hp < 0) tk->hp = 0;
            tk->hit_flash = 0.6f;
            out->damage[k] = dmg;
            out->direct[k] = direct;
        }
    }
    float g = tq_ter_height(t, at.x, at.y);
    if (at.z - g < 1.2f && at.x > 0 && at.y > 0 && at.x < TQ_MAP_M && at.y < TQ_MAP_M)
        *dirty = tq_ter_crater(t, at.x, at.y, w->crater_r, w->crater_d);
    for (int k = 0; k < s->ntree; k++) {
        tq_tree_t *tr = &s->tree[k];
        if (!tr->alive) continue;
        float dx = at.x - tr->x, dy = at.y - tr->y;
        if (dx * dx + dy * dy > (w->radius * 0.8f) * (w->radius * 0.8f)) continue;
        tr->alive = 0;
        out->trees++;
        float h = tq_ter_height(t, tr->x, tr->y);
        for (int i = 0; i < 16; i++) {
            tq_v3_t p = tq_v3(tr->x + randr(-0.8f, 0.8f), tr->y + randr(-0.8f, 0.8f), h + randr(1.6f, 3.4f));
            tq_v3_t v = tq_v3((p.x - at.x) * 1.5f + randr(-1, 1), (p.y - at.y) * 1.5f + randr(-1, 1), randr(1.5f, 4.5f));
            bool leaf = i < 11;
            add_part(s, leaf ? TQ_PART_LEAF : TQ_PART_CHUNK, p, v, randr(1.2f, 2.0f), leaf ? 0.22f : 0.13f,
                     leaf ? 0.16f : 0.13f, leaf ? (tq_rand() < 0.5f ? 0x5FB042 : 0x4C9A38) : 0x8A5A34);
        }
    }
    for (int k = 0; k < s->nblock; k++) {
        tq_block_t *b = &s->block[k];
        if (!b->alive) continue;
        float dx = at.x - (b->i + 0.5f), dy = at.y - (b->j + 0.5f), dz = at.z - (b->z + 0.5f);
        if (dx * dx + dy * dy + dz * dz > (w->radius * 0.7f) * (w->radius * 0.7f)) continue;
        b->alive = 0;
        out->blocks++;
        for (int i = 0; i < 7; i++) {
            tq_v3_t p = tq_v3(b->i + randr(0.2f, 0.8f), b->j + randr(0.2f, 0.8f), b->z + randr(0.2f, 0.8f));
            tq_v3_t v = tq_v3((p.x - at.x) * 2.0f + randr(-1, 1), (p.y - at.y) * 2.0f + randr(-1, 1), randr(2, 5));
            add_part(s, TQ_PART_CHUNK, p, v, randr(1.4f, 2.2f), randr(0.12f, 0.22f), 0.15f,
                     tq_rand() < 0.5f ? 0xB3AB9E : 0x958E82);
        }
    }
    /* the blast itself: a flash, a ring along the ground, fire, then
     * smoke; clods of the ground */
    uint32_t soil = 0x7A5C3E;
    float k = w->radius / 2.2f;
    add_part(s, TQ_PART_FLASH, tq_v3(at.x, at.y, at.z + 0.5f), tq_v3(0, 0, 0), 0.16f, 0.9f * k, 2.2f * k, 0xFFFBE0);
    add_part(s, TQ_PART_RING, tq_v3(at.x, at.y, tq_ter_height(t, at.x, at.y) + 0.05f), tq_v3(0, 0, 0), 0.45f, 0.4f,
             w->radius * 1.5f, 0xFFF2C0);
    for (int i = 0; i < 11; i++) {
        float a = randr(0, 2 * TQ_PI), rr = randr(0, 0.8f) * k;
        tq_v3_t p = tq_v3(at.x + cosf(a) * rr, at.y + sinf(a) * rr, at.z + randr(0.1f, 0.9f) * k);
        add_part(s, TQ_PART_FIRE, p, tq_v3(cosf(a) * randr(0.5f, 2.0f), sinf(a) * randr(0.5f, 2.0f), randr(0.8f, 2.6f)),
                 randr(0.5f, 0.85f), randr(0.35f, 0.6f) * k, randr(1.0f, 1.6f) * k, 0);
    }
    for (int i = 0; i < 7; i++) {
        tq_v3_t p = tq_v3(at.x + randr(-0.7f, 0.7f), at.y + randr(-0.7f, 0.7f), at.z + randr(0.3f, 1.0f));
        add_part(s, TQ_PART_SMOKE, p, tq_v3(randr(-0.6f, 0.6f), randr(-0.6f, 0.6f), randr(0.8f, 1.6f)),
                 randr(1.6f, 2.4f), 0.4f, randr(1.0f, 1.5f) * w->radius / 2.2f, tq_rand() < 0.5f ? 0x8C8A86 : 0x6E6C6A);
    }
    for (int i = 0; i < 14; i++) {
        float a = randr(0, 2 * TQ_PI), sp = randr(2.0f, 5.5f);
        add_part(s, TQ_PART_CHUNK, tq_v3(at.x, at.y, at.z + 0.2f), tq_v3(cosf(a) * sp, sinf(a) * sp, randr(3.5f, 7.0f)),
                 randr(1.1f, 1.7f), randr(0.09f, 0.17f), 0.12f, i & 1 ? soil : 0x5E4630);
    }
}

/* --------------------------------------------------------------------------
 * The computer's tank
 * -------------------------------------------------------------------------- */

/* The best pitch for a weapon from tank k towards e: how far from e it lands */
static float best_pitch(const tq_state_t *s, const tq_terrain_t *t, int who, int weapon, float yaw, float *pitch,
                        float coarse)
{
    tq_tank_t k = s->tank[who];
    const tq_tank_t *e = &s->tank[1 - who];
    float ez = tq_tank_z(t, e) + 0.8f;
    k.tyaw = yaw;
    float best = 1e9f, bp = 10 * TQ_DEG;
    float lo = -5 * TQ_DEG, hi = 60 * TQ_DEG;
    /* the mortar lobs: only high angles */
    if (weapon == TQ_W_MORTAR) lo = 35 * TQ_DEG;
    for (int pass = 0; pass < 2; pass++) {
        float step = pass ? coarse * 0.1f : coarse;
        float a0 = pass ? bp - coarse : lo, a1 = pass ? bp + coarse : hi;
        for (float a = a0; a <= a1 + 1e-4f; a += step) {
            if (a < lo || a > hi) continue;
            k.pitch = a;
            tq_v3_t p, v, end;
            tq_game_muzzle(&k, t, &p, &v, weapon);
            int h = fly(s, t, p, v, who, &end, NULL, NULL, 0);
            if (h == HIT_OUT) continue;
            float dx = end.x - e->x, dy = end.y - e->y, dz = end.z - ez;
            float d = sqrtf(dx * dx + dy * dy + dz * dz * 0.25f);
            if (h == HIT_TANK) d = 0;
            if (d < best) {
                best = d;
                bp = a;
            }
        }
    }
    *pitch = bp;
    return best;
}

void tq_ai_plan_move(tq_state_t *s, const tq_terrain_t *t, int *ap_cost)
{
    int me = s->cur;
    const tq_tank_t *e = &s->tank[1 - me];
    typedef struct { int c; float score; } cand_t;
    cand_t top[10];
    int ntop = 0;
    for (int c = 0; c < TQ_GRID * TQ_GRID; c++) {
        if (s->reach[c] != 1 && s->reach[c] != 2) continue;
        float x = c % TQ_GRID + 0.5f, y = c / TQ_GRID + 0.5f;
        float d = sqrtf((x - e->x) * (x - e->x) + (y - e->y) * (y - e->y));
        float sc = -fabsf(d - 15.0f) * 0.5f + tq_ter_height(t, x, y) * 0.6f + (s->reach[c] == 1 ? 1.5f : 0) +
                   tq_rand() * 2.0f;
        if (ntop < 10 || sc > top[ntop - 1].score) {
            int i = ntop < 10 ? ntop++ : ntop - 1;
            while (i > 0 && top[i - 1].score < sc) {
                top[i] = top[i - 1];
                i--;
            }
            top[i].c = c;
            top[i].score = sc;
        }
    }
    /* of the best spots, the first from which a shot lands near */
    static tq_state_t tmp;
    int pick = -1;
    for (int i = 0; i < ntop && pick < 0; i++) {
        memcpy(&tmp, s, sizeof tmp);
        tq_tank_t *k = &tmp.tank[me];
        k->x = top[i].c % TQ_GRID + 0.5f;
        k->y = top[i].c / TQ_GRID + 0.5f;
        float yaw = atan2f(e->y - k->y, e->x - k->x), p;
        int wmax = tmp.reach[top[i].c] == 1 ? TQ_W_MORTAR : TQ_W_CANNON;
        for (int w = 0; w <= wmax && pick < 0; w++)
            if (best_pitch(&tmp, t, me, w, yaw, &p, 3 * TQ_DEG) < 2.0f) pick = top[i].c;
    }
    if (pick < 0 && ntop) pick = top[0].c;
    *ap_cost = 0;
    s->npath = 0;
    if (pick >= 0) tq_game_route(s, t, pick % TQ_GRID + 0.5f, pick / TQ_GRID + 0.5f, ap_cost);
}

void tq_ai_plan_aim(tq_state_t *s, const tq_terrain_t *t, float *yaw, float *pitch, int *weapon)
{
    int me = s->cur;
    const tq_tank_t *k = &s->tank[me], *e = &s->tank[1 - me];
    float y = atan2f(e->y - k->y, e->x - k->x);
    float pc = 10 * TQ_DEG, pm = 45 * TQ_DEG;
    float dc = best_pitch(s, t, me, TQ_W_CANNON, y, &pc, 2 * TQ_DEG);
    float dm = k->ap >= TQ_WEAPON[TQ_W_MORTAR].ap ? best_pitch(s, t, me, TQ_W_MORTAR, y, &pm, 2 * TQ_DEG) : 1e9f;
    if (dm + 0.5f < dc) {
        *weapon = TQ_W_MORTAR;
        *pitch = pm;
    } else {
        *weapon = TQ_W_CANNON;
        *pitch = pc;
    }
    /* an honest hand: a little off */
    *yaw = y + randr(-2.2f, 2.2f) * TQ_DEG;
    *pitch += randr(-1.4f, 1.4f) * TQ_DEG;
}
