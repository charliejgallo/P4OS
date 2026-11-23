/*
 * TANQUES - the battle without pixels (see tq_game.h)
 */
#include "tq_game.h"
#include "tq_art.h"

#include <stdio.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#define GRAV       9.8f
#define STEP       (1.0f / 120.0f)  /* the shells' physics step */

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

static float map_m(const tq_terrain_t *t)
{
    return t->n * TQ_CELL;
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

static void ev_clear(tq_event_t *e, int type, tq_v3_t at)
{
    memset(e, 0, sizeof *e);
    e->type = type;
    e->at = at;
    e->dirty.x0 = e->dirty.y0 = 1;
    e->dirty.x1 = e->dirty.y1 = 0;
}

static void ev_dirty(tq_event_t *e, tq_vbox_t b)
{
    if (b.x1 < b.x0) return;
    if (e->dirty.x1 < e->dirty.x0) {
        e->dirty = b;
        return;
    }
    if (b.x0 < e->dirty.x0) e->dirty.x0 = b.x0;
    if (b.y0 < e->dirty.y0) e->dirty.y0 = b.y0;
    if (b.x1 > e->dirty.x1) e->dirty.x1 = b.x1;
    if (b.y1 > e->dirty.y1) e->dirty.y1 = b.y1;
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

static bool add_block(tq_state_t *s, const tq_terrain_t *t, int i, int j, int level)
{
    if (s->nblock >= TQ_MAX_BLOCKS || i < 1 || j < 1 || i >= s->grid - 1 || j >= s->grid - 1) return false;
    tq_block_t *b = &s->block[s->nblock++];
    b->i = (int8_t)i;
    b->j = (int8_t)j;
    b->variant = (uint8_t)((i * 7 + j * 3 + level) & 1);
    b->alive = 1;
    b->z = tq_ter_height(t, i + 0.5f, j + 0.5f) + level * TQ_BLOCK_M - 0.05f;
    b->vz = 0;
    return true;
}

static bool near_tank(const tq_state_t *s, float x, float y, float r)
{
    for (int k = 0; k < s->ntank; k++) {
        float dx = x - s->tank[k].x, dy = y - s->tank[k].y;
        if (dx * dx + dy * dy < r * r) return true;
    }
    return false;
}

/* Something of a mode's on the ground there: the trees and walls keep off */
static bool near_mark(const tq_state_t *s, float x, float y, float r)
{
    if (s->mode == TQ_MODE_HILL) {
        float d = sqrtf((x - s->hill_x) * (x - s->hill_x) + (y - s->hill_y) * (y - s->hill_y));
        if (d < s->hill_r + r) return true;
    }
    if (s->mode == TQ_MODE_FLAG)
        for (int f = 0; f < 2; f++)
            if ((x - s->flag[f].hx) * (x - s->flag[f].hx) + (y - s->flag[f].hy) * (y - s->flag[f].hy) < r * r) return true;
    for (int c = 0; c < s->nchest; c++)
        if ((x - s->chest[c][0]) * (x - s->chest[c][0]) + (y - s->chest[c][1]) * (y - s->chest[c][1]) < r * r) return true;
    return false;
}

/* Where a side starts: the beach, its island; elsewhere a side of the map
 * (the modes) or a place round it (the usual) */
static void start_spot(const tq_state_t *s, const tq_terrain_t *t, const tq_setup_t *su, int i, float a0, float *x,
                       float *y)
{
    float M = map_m(t);
    int team = su->t[i].team & 1;
    /* this tank's place among its side's */
    int nth = 0, nside = 0;
    for (int q = 0; q < su->ntank; q++) {
        if ((su->t[q].team & 1) != team) continue;
        if (q < i) nth++;
        nside++;
    }
    bool sides = t->biome == TQ_B_PLAYA || s->mode != TQ_MODE_CLASSIC;
    if (t->biome == TQ_B_PLAYA) {
        const float *I = t->isl[team];
        bool long_y = I[3] > I[2];
        float off = nside > 1 ? (nth - (nside - 1) * 0.5f) * 0.9f / (nside - 1 > 0 ? nside - 1 : 1) : 0;
        *x = I[0] + (long_y ? 0 : off * I[2]);
        *y = I[1] + (long_y ? off * I[3] : 0);
    } else if (sides) {
        float a = a0 + team * TQ_PI + (nside > 1 ? (nth - (nside - 1) * 0.5f) * 0.55f : 0);
        *x = M / 2 + cosf(a) * M * 0.36f;
        *y = M / 2 + sinf(a) * M * 0.36f;
    } else {
        float a = a0 + i * 2 * TQ_PI / su->ntank + randr(-0.25f, 0.25f);
        *x = M / 2 + cosf(a) * M * 0.34f;
        *y = M / 2 + sinf(a) * M * 0.34f;
    }
    tq_ter_land(t, x, y);
}

/* A spot for a chest: on land, clear of the tanks and the other chests */
static bool chest_spot(const tq_state_t *s, const tq_terrain_t *t, float *x, float *y)
{
    float M = map_m(t);
    for (int tries = 0; tries < 400; tries++) {
        float px = randr(3.5f, M - 3.5f), py = randr(3.5f, M - 3.5f);
        if (tq_ter_water(t, px, py) || near_tank(s, px, py, 5.0f) || near_mark(s, px, py, 4.5f)) continue;
        int ci = (int)(px / TQ_CELL + 0.5f), cj = (int)(py / TQ_CELL + 0.5f);
        int m = t->mat[cj * t->nv + ci];
        if (m == TQ_MAT_DECK || (m == TQ_MAT_SPECIAL && t->biome == TQ_B_VOLCAN)) continue;
        *x = px;
        *y = py;
        return true;
    }
    return false;
}

static void tank_reset(tq_tank_t *k, const tq_setup_t *su, int i, int wave)
{
    tq_stats_t st;
    tq_stats(k->type, su->t[i].up, &st);
    k->hp = k->hpmax = st.hpmax * (su->t[i].boss ? 3 : 1) * (10 + 2 * wave) / 10;
    k->armor = st.armor + (su->t[i].boss ? 10 : 0);
    k->apmax = k->ap = st.apmax;
    k->m_per_ap = st.m_per_ap;
    k->climb = st.climb;
    k->max_slope = st.max_slope;
    k->shield = k->type == TQ_T_TORTUGA ? 20 : 0;
    k->emp = 0;
    k->hit_flash = 0;
    k->boss = su->t[i].boss;
    k->nload = 0;
    for (int w = 0; w < TQ_SLOTS_MAX && w < TQ_TANK[k->type].slots + 1; w++) {
        uint8_t id = su->t[i].load[w];
        if (id >= TQ_WEAPONS || k->nload >= TQ_TANK[k->type].slots) continue;
        k->load[k->nload] = id;
        k->level[k->nload] = su->t[i].level[id];
        k->uses[k->nload] = TQ_WEAPON[id].uses ? (uint8_t)TQ_WEAPON[id].uses : 0xFF;
        if (k->type == TQ_T_TOPO && id == TQ_W_MINES) k->uses[k->nload]++;
        k->nload++;
    }
    if (!k->nload) {
        k->load[0] = TQ_W_CANNON;
        k->uses[0] = 0xFF;
        k->nload = 1;
    }
    k->sel = 0;
    k->alive = true;
    k->gone = false;
}

static tq_setup_t s_setup;          /* the battle's, for the waves */

void tq_game_new(tq_state_t *s, tq_terrain_t *t, const tq_setup_t *su)
{
    memset(s, 0, sizeof *s);
    s_setup = *su;
    s_rng = su->seed * 2654435761u + 12345u;
    tq_ter_generate(t, su->seed, TQ_SIZE_M[su->size % TQ_SIZES], su->biome);
    float M = map_m(t);
    s->grid = (int)M;
    s->weather = su->biome == TQ_B_LUNA ? TQ_WX_CLEAR : su->weather;     /* no air, no wind */
    s->grav_k = su->biome == TQ_B_LUNA ? TQ_MOON_G : 1.0f;
    s->diff = su->diff;
    s->ntank = su->ntank;
    s->mode = su->mode;
    s->goal = su->goal;
    s->goal_n = su->goal_n;
    s->protect = -1;
    switch (s->mode) {
    case TQ_MODE_HILL: s->target = su->goal_n ? su->goal_n : 4; break;
    case TQ_MODE_FLAG: s->target = su->goal_n ? su->goal_n : 2; break;
    case TQ_MODE_WAVES: s->target = su->goal_n; s->wave = 1; break;
    default: break;
    }

    /* the tanks: each side together in the modes and on the beach */
    float a0 = randr(0, 2 * TQ_PI);
    for (int i = 0; i < s->ntank; i++) {
        tq_tank_t *k = &s->tank[i];
        start_spot(s, t, su, i, a0, &k->x, &k->y);
        k->yaw = atan2f(M / 2 - k->y, M / 2 - k->x);
        k->tyaw = k->yaw;
        k->type = su->t[i].type;
        k->team = su->t[i].team;
        k->ai = su->t[i].ai;
        k->diff = su->diff;
        memcpy(k->name, su->t[i].name, sizeof k->name);
        tank_reset(k, su, i, 0);
        k->pitch = TQ_TANK[k->type].sprite_pitch * TQ_DEG;
        if (s->mode == TQ_MODE_WAVES && su->t[i].later) {
            k->alive = false;
            k->gone = true;
        }
        if (s->goal == TQ_GOAL_PROTECT && k->team == 0 && k->ai && s->protect < 0) s->protect = i;
        tq_ter_flatten(t, k->x, k->y, 2.2f);
    }

    /* the modes' marks, before the trees and walls keep off them */
    if (s->mode == TQ_MODE_HILL) {
        if (t->biome == TQ_B_PLAYA) {
            /* the hill is the bridge's middle: the one way across */
            s->hill_x = (t->bridge[0] + t->bridge[2]) / 2;
            s->hill_y = (t->bridge[1] + t->bridge[3]) / 2;
        } else {
            s->hill_x = M / 2 + randr(-1.5f, 1.5f);
            s->hill_y = M / 2 + randr(-1.5f, 1.5f);
            tq_ter_flatten(t, s->hill_x, s->hill_y, 2.0f);
        }
        s->hill_r = 3.0f;
    }
    if (s->mode == TQ_MODE_FLAG) {
        for (int f = 0; f < 2; f++) {
            float cx = 0, cy = 0;
            int n = 0;
            for (int i = 0; i < s->ntank; i++)
                if ((s->tank[i].team & 1) == f) {
                    cx += s->tank[i].x;
                    cy += s->tank[i].y;
                    n++;
                }
            if (!n) {
                cx = M / 2;
                cy = M / 2;
            } else {
                cx /= n;
                cy /= n;
            }
            /* home: a little behind the side's start */
            float dx = cx - M / 2, dy = cy - M / 2, l = sqrtf(dx * dx + dy * dy) + 1e-3f;
            float hx = tq_clampf(cx + dx / l * 3.0f, 3.5f, M - 3.5f), hy = tq_clampf(cy + dy / l * 3.0f, 3.5f, M - 3.5f);
            tq_ter_land(t, &hx, &hy);
            s->flag[f].hx = s->flag[f].x = hx;
            s->flag[f].hy = s->flag[f].y = hy;
            s->flag[f].carrier = -1;
        }
    }
    if (s->mode == TQ_MODE_TREASURE) {
        int want = 5 + (s->ntank > 2 ? 2 : 0);
        /* the beach: one on each islet, for the boat */
        if (t->biome == TQ_B_PLAYA)
            for (int q = 2; q < t->nisl && s->nchest < want; q++) {
                s->chest[s->nchest][0] = t->isl[q][0];
                s->chest[s->nchest][1] = t->isl[q][1];
                s->chest_alive[s->nchest++] = 1;
            }
        while (s->nchest < want) {
            float x, y;
            if (!chest_spot(s, t, &x, &y)) break;
            s->chest[s->nchest][0] = x;
            s->chest[s->nchest][1] = y;
            s->chest_alive[s->nchest++] = 1;
        }
    }

    /* walls: a few lines of stones, some two high (houses in the ruins) */
    float dens = TQ_OBJECTS_K[su->objects % TQ_OBJECT_LEVELS];
    int walls = (int)((s->grid / 8 + 1 + (su->biome == TQ_B_RUINAS ? 3 : 0)) * dens + 0.5f);
    for (int w = 0; w < walls; w++) {
        for (int tries = 0; tries < 30; tries++) {
            int i0 = (int)randr(4, s->grid - 8), j0 = (int)randr(4, s->grid - 8);
            if (near_tank(s, i0 + 2.0f, j0 + 2.0f, 6.0f) || near_mark(s, i0 + 2.0f, j0 + 2.0f, 4.5f)) continue;
            if (t->biome == TQ_B_PLAYA) {
                /* on the islands, not in the sea nor on the bridge */
                bool wet = false;
                for (int q = 0; q < 5 && !wet; q++) {
                    float x = i0 + q + 0.5f, y = j0 + 0.5f;
                    int ci = (int)(x / TQ_CELL + 0.5f), cj = (int)(y / TQ_CELL + 0.5f);
                    if (tq_ter_water(t, x, y) || tq_ter_water(t, i0 + 0.5f, j0 + q + 0.5f) ||
                        t->mat[cj * t->nv + ci] == TQ_MAT_DECK)
                        wet = true;
                }
                if (wet) continue;
            }
            if (su->biome == TQ_B_RUINAS && w % 2) {
                /* a house: three walls of a 4 x 4 square */
                int side = (int)randr(0, 3.99f);
                for (int e = 0; e < 4; e++) {
                    for (int q = 0; q < 4; q++) {
                        if (e == side) continue;
                        int i = e == 0 ? i0 + q : (e == 1 ? i0 + 3 : (e == 2 ? i0 + q : i0));
                        int j = e == 0 ? j0 : (e == 1 ? j0 + q : (e == 2 ? j0 + 3 : j0 + q));
                        add_block(s, t, i, j, 0);
                        if (q == 1 || q == 2) add_block(s, t, i, j, 1);
                    }
                }
            } else {
                bool along_x = tq_rand() < 0.5f;
                int len = 3 + (int)randr(0, 3.99f);
                for (int q = 0; q < len; q++) {
                    int i = i0 + (along_x ? q : 0), j = j0 + (along_x ? 0 : q);
                    add_block(s, t, i, j, 0);
                    if (q > 0 && q < len - 1 && tq_rand() < 0.5f) add_block(s, t, i, j, 1);
                }
            }
            break;
        }
    }

    /* trees where there is room: off the paths, the walls and the tanks */
    int want = (int)(M * M / 60.0f * dens * (t->biome == TQ_B_PLAYA ? 0.6f : 1.0f) + 0.5f);
    if (want > TQ_MAX_TREES) want = TQ_MAX_TREES;
    for (int tries = 0; tries < 1500 && s->ntree < want; tries++) {
        float x = randr(3.0f, M - 3.0f), y = randr(3.0f, M - 3.0f);
        bool ok = !near_tank(s, x, y, 4.5f) && !near_mark(s, x, y, 2.5f);
        for (int k = 0; k < s->ntree && ok; k++) {
            float dx = x - s->tree[k].x, dy = y - s->tree[k].y;
            if (dx * dx + dy * dy < 2.6f * 2.6f) ok = false;
        }
        for (int k = 0; k < s->nblock && ok; k++) {
            float dx = x - (s->block[k].i + 0.5f), dy = y - (s->block[k].j + 0.5f);
            if (dx * dx + dy * dy < 2.0f * 2.0f) ok = false;
        }
        int ci = (int)(x / TQ_CELL), cj = (int)(y / TQ_CELL);
        if (ok && t->mat[cj * t->nv + ci] != TQ_MAT_BASE) ok = false;
        if (!ok) continue;
        tq_tree_t *tr = &s->tree[s->ntree++];
        tr->x = x;
        tr->y = y;
        tr->kind = tq_rand() < 0.55f ? TQ_PROP_TA : TQ_PROP_TB;
        tr->alive = 1;
    }
    tq_ter_relight(t);
    s->cur = 0;
}

int tq_game_next(const tq_state_t *s)
{
    for (int i = 1; i <= s->ntank; i++) {
        int k = (s->cur + i) % s->ntank;
        if (s->tank[k].alive) return k;
    }
    return s->cur;
}

int tq_game_winner(const tq_state_t *s)
{
    /* the goals that end it besides the fighting */
    if (s->protect >= 0 && !s->tank[s->protect].alive) return 1;
    for (int f = 0; f < 2; f++)
        if (s->target && s->mode != TQ_MODE_WAVES && s->score[f] >= s->target) return f;
    bool side[2] = { false, false };
    int team = -1;
    bool mixed = false;
    for (int k = 0; k < s->ntank; k++) {
        if (!s->tank[k].alive) continue;
        side[s->tank[k].team & 1] = true;
        if (team >= 0 && s->tank[k].team != team) mixed = true;
        team = s->tank[k].team;
    }
    if (team < 0) return -2;
    if (s->mode == TQ_MODE_WAVES) {
        if (!side[0]) return 1;
        if (!side[1]) return s->target && s->wave >= s->target ? 0 : -1;    /* else another wave */
        return -1;
    }
    if (s->goal == TQ_GOAL_HOLD && side[0] && s->round > s->goal_n) return 0;
    if (s->goal == TQ_GOAL_FAST && mixed && s->round > s->goal_n) return 1;
    if (s->mode == TQ_MODE_TREASURE && mixed) {
        int left = 0;
        for (int c = 0; c < s->nchest; c++) left += s->chest_alive[c];
        if (!left) {
            if (s->score[0] != s->score[1]) return s->score[0] > s->score[1] ? 0 : 1;
            /* a tie: the side with more health */
            int hp[2] = { 0, 0 };
            for (int k = 0; k < s->ntank; k++)
                if (s->tank[k].alive) hp[s->tank[k].team & 1] += s->tank[k].hp;
            return hp[0] >= hp[1] ? 0 : 1;
        }
    }
    return mixed ? -1 : team;
}

bool tq_game_wave_due(const tq_state_t *s)
{
    if (s->mode != TQ_MODE_WAVES) return false;
    bool side[2] = { false, false };
    for (int k = 0; k < s->ntank; k++)
        if (s->tank[k].alive) side[s->tank[k].team & 1] = true;
    return side[0] && !side[1] && !(s->target && s->wave >= s->target);
}

int tq_game_wave(tq_state_t *s, tq_terrain_t *t)
{
    s->wave++;
    float M = map_m(t);
    /* where the people are; the wave comes from the other side */
    float cx = 0, cy = 0;
    int n = 0, slots = 0;
    for (int k = 0; k < s->ntank; k++) {
        if (s->tank[k].team & 1) {
            slots++;
            continue;
        }
        if (!s->tank[k].alive) continue;
        cx += s->tank[k].x;
        cy += s->tank[k].y;
        n++;
    }
    if (n) {
        cx /= n;
        cy /= n;
    }
    float a = atan2f(M / 2 - cy, M / 2 - cx);
    int want = s->wave < slots ? s->wave : slots, come = 0;
    for (int k = 0; k < s->ntank && come < want; k++) {
        tq_tank_t *tk = &s->tank[k];
        if (!(tk->team & 1)) continue;
        tank_reset(tk, &s_setup, k, s->wave - 1);
        float b = a + (come - (want - 1) * 0.5f) * 0.6f;
        tk->x = M / 2 + cosf(b) * M * 0.36f;
        tk->y = M / 2 + sinf(b) * M * 0.36f;
        tq_ter_land(t, &tk->x, &tk->y);
        /* not on anything */
        for (int q = 0; q < 12 && near_tank(s, tk->x, tk->y, 0.1f); q++) tk->x += 1.0f;
        tk->yaw = tk->tyaw = atan2f(cy - tk->y, cx - tk->x);
        tk->diff = s->diff + s->wave / 3 > 2 ? 2 : s->diff + s->wave / 3;
        come++;
    }
    for (int k = 0; k < s->ntank; k++) {
        tq_tank_t *tk = &s->tank[k];
        if ((tk->team & 1) && !tk->alive && come >= want) tk->gone = true;
        /* the people get a little health back between waves */
        if (!(tk->team & 1) && tk->alive) {
            tk->hp += tk->hpmax / 4;
            if (tk->hp > tk->hpmax) tk->hp = tk->hpmax;
        }
    }
    return come;
}

bool tq_game_pickups(tq_state_t *s, const tq_terrain_t *t, int who, tq_event_t *ev)
{
    tq_tank_t *k = &s->tank[who];
    if (!k->alive) return false;
    int side = k->team & 1;
    float z = tq_tank_z(t, k) + 3.0f;
    if (s->mode == TQ_MODE_FLAG) {
        for (int f = 0; f < 2; f++) {
            if (s->flag[f].carrier == who) {
                s->flag[f].x = k->x;
                s->flag[f].y = k->y;
            }
        }
        int mine = side, theirs = side ^ 1;
        /* theirs, lying anywhere: take it */
        if (s->flag[theirs].carrier < 0) {
            float dx = k->x - s->flag[theirs].x, dy = k->y - s->flag[theirs].y;
            if (dx * dx + dy * dy < 1.7f * 1.7f) {
                s->flag[theirs].carrier = who;
                ev_clear(ev, TQ_EV_FLAG_TAKE, tq_v3(k->x, k->y, z));
                ev->value = side;
                return true;
            }
        }
        /* mine, dropped: back home */
        bool home = s->flag[mine].x == s->flag[mine].hx && s->flag[mine].y == s->flag[mine].hy;
        if (s->flag[mine].carrier < 0 && !home) {
            float dx = k->x - s->flag[mine].x, dy = k->y - s->flag[mine].y;
            if (dx * dx + dy * dy < 1.7f * 1.7f) {
                s->flag[mine].x = s->flag[mine].hx;
                s->flag[mine].y = s->flag[mine].hy;
                ev_clear(ev, TQ_EV_FLAG_BACK, tq_v3(k->x, k->y, z));
                ev->value = side;
                return true;
            }
        }
        /* carrying theirs to mine at home: a capture */
        if (s->flag[theirs].carrier == who && home) {
            float dx = k->x - s->flag[mine].hx, dy = k->y - s->flag[mine].hy;
            if (dx * dx + dy * dy < 2.0f * 2.0f) {
                s->flag[theirs].carrier = -1;
                s->flag[theirs].x = s->flag[theirs].hx;
                s->flag[theirs].y = s->flag[theirs].hy;
                s->score[side]++;
                ev_clear(ev, TQ_EV_CAPTURE, tq_v3(k->x, k->y, z));
                ev->value = side;
                return true;
            }
        }
    }
    if (s->mode == TQ_MODE_TREASURE) {
        for (int c = 0; c < s->nchest; c++) {
            if (!s->chest_alive[c]) continue;
            float dx = k->x - s->chest[c][0], dy = k->y - s->chest[c][1];
            if (dx * dx + dy * dy > 1.6f * 1.6f) continue;
            s->chest_alive[c] = 0;
            s->score[side]++;
            ev_clear(ev, TQ_EV_CHEST, tq_v3(s->chest[c][0], s->chest[c][1], z));
            ev->value = side;
            for (int q = 0; q < 10; q++)
                add_part(s, TQ_PART_SPARK, tq_v3(s->chest[c][0], s->chest[c][1], z - 2.4f),
                         tq_v3(randr(-1.5f, 1.5f), randr(-1.5f, 1.5f), randr(1.5f, 3.5f)), 0.9f, 0.14f, 0.04f, 0xFFD23C);
            return true;
        }
    }
    return false;
}

static void new_wind(tq_state_t *s)
{
    float m = s->weather == TQ_WX_WIND ? 2.5f : (s->weather == TQ_WX_STORM ? 4.5f : 0.0f);
    if (s->weather == TQ_WX_WIND && (s->wind_x || s->wind_y)) {
        /* wind turns a little each turn; a storm anything */
        float a = atan2f(s->wind_y, s->wind_x) + randr(-0.5f, 0.5f), v = randr(0.4f, 1.0f) * m;
        s->wind_x = cosf(a) * v;
        s->wind_y = sinf(a) * v;
        return;
    }
    float a = randr(0, 2 * TQ_PI), v = randr(0.3f, 1.0f) * m;
    s->wind_x = cosf(a) * v;
    s->wind_y = sinf(a) * v;
}

static int hurt(tq_state_t *s, int who, int dmg, int by)
{
    tq_tank_t *k = &s->tank[who];
    if (!k->alive || dmg <= 0) return 0;
    dmg = dmg * (100 - k->armor) / 100;
    if (dmg < 1) dmg = 1;
    if (k->shield > 0) {
        int stop = dmg < k->shield ? dmg : k->shield;
        k->shield -= stop;
        dmg -= stop;
    }
    k->hp -= dmg;
    k->hit_flash = 0.6f;
    /* hit, a carrier drops the flag where it is */
    for (int f = 0; f < 2; f++) {
        if (s->mode == TQ_MODE_FLAG && s->flag[f].carrier == who && dmg > 0) {
            s->flag[f].carrier = -1;
            s->flag[f].x = k->x;
            s->flag[f].y = k->y;
        }
    }
    if (by >= 0 && by != who) s->tank[by].dealt += dmg;
    if (k->hp <= 0) {
        k->hp = 0;
        k->alive = false;
        if (by >= 0 && by != who) s->tank[by].kills++;
    }
    return dmg;
}

void tq_game_turn(tq_state_t *s, const tq_terrain_t *t, tq_event_t *ev)
{
    tq_tank_t *k = &s->tank[s->cur];
    ev_clear(ev, TQ_EV_NONE, tq_v3(k->x, k->y, 0));
    /* a round ends when the first tank alive plays again */
    int first = 0;
    while (first < s->ntank && !s->tank[first].alive) first++;
    if (s->cur == first) {
        s->round++;
        for (int z = 0; z < TQ_MAX_ZONES; z++)
            if (s->zone[z].live && --s->zone[z].turns <= 0) s->zone[z].live = false;
    }
    k->ap = k->apmax - k->emp;
    if (k->ap < 2) k->ap = 2;
    k->emp = 0;
    k->moved = false;
    k->shots = 0;
    new_wind(s);
    for (int z = 0; z < TQ_MAX_ZONES; z++) {
        tq_zone_t *zn = &s->zone[z];
        if (!zn->live || zn->kind != TQ_ZONE_FIRE) continue;
        float dx = k->x - zn->x, dy = k->y - zn->y;
        if (dx * dx + dy * dy < zn->r * zn->r) {
            ev->type = TQ_EV_BURN;
            ev->at = tq_v3(k->x, k->y, tq_tank_z(t, k) + 2.5f);
            ev->damage[s->cur] += hurt(s, s->cur, 10, zn->owner);
        }
    }
    /* the hill: a point for a turn begun on it */
    if (s->mode == TQ_MODE_HILL && k->alive && ev->type == TQ_EV_NONE) {
        float dx = k->x - s->hill_x, dy = k->y - s->hill_y;
        if (dx * dx + dy * dy < s->hill_r * s->hill_r) {
            s->score[k->team & 1]++;
            ev->type = TQ_EV_POINT;
            ev->at = tq_v3(k->x, k->y, tq_tank_z(t, k) + 3.2f);
            ev->value = k->team & 1;
        }
    }
}

/* --------------------------------------------------------------------------
 * Driving
 * -------------------------------------------------------------------------- */

static uint16_t s_cost[TQ_GRIDMAX * TQ_GRIDMAX];
static int16_t s_parent[TQ_GRIDMAX * TQ_GRIDMAX];
static uint8_t s_block[TQ_GRIDMAX * TQ_GRIDMAX];

static void build_blocked(const tq_state_t *s, const tq_terrain_t *t, int who)
{
    int G = s->grid;
    bool swims = s->tank[who].type == TQ_T_ANFIBIO;
    memset(s_block, 0, sizeof s_block);
    for (int j = 0; j < G; j++) {
        for (int i = 0; i < G; i++) {
            float x = i + 0.5f, y = j + 0.5f;
            bool b = i < 2 || j < 2 || i > G - 3 || j > G - 3;
            if (!b && !swims && tq_ter_water(t, x, y)) b = true;
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
            for (int k = 0; k < s->ntank && !b; k++) {
                const tq_tank_t *e = &s->tank[k];
                if (k == who || !e->alive) continue;
                float dx = x - e->x, dy = y - e->y;
                if (dx * dx + dy * dy < 2.8f * 2.8f) b = true;
            }
            s_block[j * G + i] = b;
        }
    }
}

void tq_game_reach(tq_state_t *s, const tq_terrain_t *t)
{
    const tq_tank_t *k = &s->tank[s->cur];
    int G = s->grid;
    build_blocked(s, t, s->cur);
    for (int i = 0; i < G * G; i++) {
        s_cost[i] = 0xFFFF;
        s_parent[i] = -1;
    }
    memset(s->reach, 0, sizeof s->reach);
    int si = (int)k->x, sj = (int)k->y;
    if (si < 0 || sj < 0 || si >= G || sj >= G) return;
    int start = sj * G + si;
    int budget = k->ap * 16;
    s_cost[start] = 0;
    static uint8_t done[TQ_GRIDMAX * TQ_GRIDMAX];
    memset(done, 0, sizeof done);
    static const int8_t DI[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DJ[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    for (;;) {
        int best = -1;
        uint16_t bc = 0xFFFF;
        for (int c = 0; c < G * G; c++)
            if (!done[c] && s_cost[c] < bc) { bc = s_cost[c]; best = c; }
        if (best < 0 || bc > budget) break;
        done[best] = 1;
        int bi = best % G, bj = best / G;
        float hb = tq_ter_height(t, bi + 0.5f, bj + 0.5f);
        for (int d = 0; d < 8; d++) {
            int ni = bi + DI[d], nj = bj + DJ[d];
            if (ni < 0 || nj < 0 || ni >= G || nj >= G) continue;
            int n = nj * G + ni;
            if (done[n] || s_block[n]) continue;
            if (d >= 4 && (s_block[bj * G + ni] || s_block[nj * G + bi])) continue;
            float dist = d < 4 ? 1.0f : 1.4142f;
            float slope = (tq_ter_height(t, ni + 0.5f, nj + 0.5f) - hb) / dist;
            if (slope > k->max_slope || slope < -0.95f) continue;
            float c = dist / k->m_per_ap * (1.0f + (slope > 0 ? k->climb * slope : 0.0f));
            int nc = bc + (int)(c * 16.0f + 0.5f);
            if (nc < s_cost[n]) {
                s_cost[n] = (uint16_t)nc;
                s_parent[n] = (int16_t)best;
            }
        }
    }
    /* the tile's colour: how much is left for shooting */
    int cheap = 99, dear = 0;
    for (int w = 0; w < k->nload; w++) {
        int ap = tq_shot_ap(k, w) * 16;
        if (ap < cheap) cheap = ap;
        if (ap > dear) dear = ap;
    }
    for (int c = 0; c < G * G; c++) {
        if (!done[c] || c == start) continue;
        int left = budget - s_cost[c];
        s->reach[c] = left >= dear ? 1 : (left >= cheap ? 2 : 3);
    }
}

static bool line_clear(const tq_state_t *s, float x0, float y0, float x1, float y1)
{
    int G = s->grid;
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    int n = (int)(len / 0.25f) + 1;
    for (int i = 1; i < n; i++) {
        float f = (float)i / n;
        int ci = (int)(x0 + dx * f), cj = (int)(y0 + dy * f);
        if (ci < 0 || cj < 0 || ci >= G || cj >= G) return false;
        if (!s->reach[cj * G + ci] && !(ci == (int)x0 && cj == (int)y0)) return false;
    }
    return true;
}

bool tq_game_route(tq_state_t *s, const tq_terrain_t *t, float x, float y, int *ap)
{
    (void)t;
    int G = s->grid;
    s->npath = 0;
    int ci = (int)x, cj = (int)y;
    if (ci < 0 || cj < 0 || ci >= G || cj >= G) return false;
    int c = cj * G + ci;
    if (!s->reach[c]) return false;
    static int cells[TQ_GRIDMAX * 4];
    int n = 0;
    for (int k = c; k >= 0 && n < (int)(sizeof cells / sizeof cells[0]); k = s_parent[k]) cells[n++] = k;
    static float pts[TQ_GRIDMAX * 4][2];
    int np = 0;
    const tq_tank_t *k = &s->tank[s->cur];
    pts[np][0] = k->x;
    pts[np][1] = k->y;
    np++;
    for (int i = n - 2; i >= 0; i--) {
        pts[np][0] = cells[i] % G + 0.5f;
        pts[np][1] = cells[i] / G + 0.5f;
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
 * Blasts
 * -------------------------------------------------------------------------- */

static float weapon_k(const tq_tank_t *k, int w, bool radius)
{
    int fly = TQ_WEAPON[w].fly;
    float b = 1.0f;
    if (k->type == TQ_T_OBUS && fly == TQ_FLY_LOB) b = radius ? 1.12f : 1.2f;
    if (k->type == TQ_T_AVISPA && (fly == TQ_FLY_GUIDED || fly == TQ_FLY_BEAM) && !radius) b = 1.25f;
    if (k->type == TQ_T_TOPO && fly == TQ_FLY_DRILL && !radius) b = 1.25f;
    if (k->type == TQ_T_TORTUGA && w == TQ_W_TESLA && !radius) b = 1.25f;
    if (k->type == TQ_T_ANFIBIO && w == TQ_W_TWIN && !radius) b = 1.2f;
    return b;
}

static void scatter_props(tq_state_t *s, const tq_terrain_t *t, tq_v3_t at, float radius)
{
    for (int k = 0; k < s->ntree; k++) {
        tq_tree_t *tr = &s->tree[k];
        if (!tr->alive) continue;
        float dx = at.x - tr->x, dy = at.y - tr->y;
        if (dx * dx + dy * dy > (radius * 0.8f) * (radius * 0.8f)) continue;
        tr->alive = 0;
        float h = tq_ter_height(t, tr->x, tr->y);
        uint32_t leaf = t->biome == TQ_B_NIEVE ? 0xF2F6FA : (t->biome == TQ_B_VOLCAN ? 0x4A403C :
                        (t->biome == TQ_B_LUNA ? 0x84E0FF : 0x5FB042));
        for (int i = 0; i < 16; i++) {
            tq_v3_t p = tq_v3(tr->x + randr(-0.8f, 0.8f), tr->y + randr(-0.8f, 0.8f), h + randr(1.6f, 3.4f));
            tq_v3_t v = tq_v3((p.x - at.x) * 1.5f + randr(-1, 1), (p.y - at.y) * 1.5f + randr(-1, 1), randr(1.5f, 4.5f));
            bool lf = i < 11;
            add_part(s, lf ? TQ_PART_LEAF : TQ_PART_CHUNK, p, v, randr(1.2f, 2.0f), lf ? 0.22f : 0.13f, lf ? 0.16f : 0.13f,
                     lf ? leaf : 0x8A5A34);
        }
    }
    for (int k = 0; k < s->nblock; k++) {
        tq_block_t *b = &s->block[k];
        if (!b->alive) continue;
        float dx = at.x - (b->i + 0.5f), dy = at.y - (b->j + 0.5f), dz = at.z - (b->z + 0.5f);
        if (dx * dx + dy * dy + dz * dz > (radius * 0.7f) * (radius * 0.7f)) continue;
        b->alive = 0;
        uint32_t c = t->biome == TQ_B_RUINAS ? 0xB86B4F : (t->biome == TQ_B_NIEVE ? 0xA8DDF0 :
                     (t->biome == TQ_B_PLAYA ? 0xA8784A : 0xB3AB9E));
        for (int i = 0; i < 7; i++) {
            tq_v3_t p = tq_v3(b->i + randr(0.2f, 0.8f), b->j + randr(0.2f, 0.8f), b->z + randr(0.2f, 0.8f));
            tq_v3_t v = tq_v3((p.x - at.x) * 2.0f + randr(-1, 1), (p.y - at.y) * 2.0f + randr(-1, 1), randr(2, 5));
            add_part(s, TQ_PART_CHUNK, p, v, randr(1.4f, 2.2f), randr(0.12f, 0.22f), 0.15f, c);
        }
    }
}

static void fireball(tq_state_t *s, const tq_terrain_t *t, tq_v3_t at, float radius, uint32_t ring)
{
    float k = radius / 2.2f;
    add_part(s, TQ_PART_FLASH, tq_v3(at.x, at.y, at.z + 0.5f), tq_v3(0, 0, 0), 0.16f, 0.9f * k, 2.2f * k, 0xFFFBE0);
    add_part(s, TQ_PART_RING, tq_v3(at.x, at.y, tq_ter_height(t, at.x, at.y) + 0.05f), tq_v3(0, 0, 0), 0.45f, 0.4f,
             radius * 1.5f, ring);
    int nf = radius > 1.5f ? 11 : 6;
    for (int i = 0; i < nf; i++) {
        float a = randr(0, 2 * TQ_PI), rr = randr(0, 0.8f) * k;
        tq_v3_t p = tq_v3(at.x + cosf(a) * rr, at.y + sinf(a) * rr, at.z + randr(0.1f, 0.9f) * k);
        add_part(s, TQ_PART_FIRE, p, tq_v3(cosf(a) * randr(0.5f, 2.0f), sinf(a) * randr(0.5f, 2.0f), randr(0.8f, 2.6f)),
                 randr(0.5f, 0.85f), randr(0.35f, 0.6f) * k, randr(1.0f, 1.6f) * k, 0);
    }
    for (int i = 0; i < nf - 3; i++) {
        tq_v3_t p = tq_v3(at.x + randr(-0.7f, 0.7f), at.y + randr(-0.7f, 0.7f), at.z + randr(0.3f, 1.0f));
        add_part(s, TQ_PART_SMOKE, p, tq_v3(randr(-0.6f, 0.6f), randr(-0.6f, 0.6f), randr(0.8f, 1.6f)), randr(1.6f, 2.4f),
                 0.4f, randr(1.0f, 1.5f) * k, tq_rand() < 0.5f ? 0x8C8A86 : 0x6E6C6A);
    }
    uint32_t soil = t->biome == TQ_B_NIEVE ? 0xE8EEF4 : (t->biome == TQ_B_DESIERTO ? 0xD2A86E :
                    (t->biome == TQ_B_LUNA ? 0x9A9AA2 : (t->biome == TQ_B_PLAYA ? 0xE6D29E : 0x7A5C3E)));
    bool sea = tq_ter_water(t, at.x, at.y);
    for (int i = 0; i < nf + 3; i++) {
        float a = randr(0, 2 * TQ_PI), sp = randr(2.0f, 5.5f);
        if (sea) {
            /* a splash: white and blue drops */
            add_part(s, TQ_PART_CHUNK, tq_v3(at.x, at.y, at.z + 0.2f), tq_v3(cosf(a) * sp * 0.6f, sinf(a) * sp * 0.6f,
                     randr(5.0f, 9.0f)), randr(0.9f, 1.4f), randr(0.12f, 0.2f), 0.1f, i & 1 ? 0xF2FAFF : 0x5CB8E0);
            continue;
        }
        add_part(s, TQ_PART_CHUNK, tq_v3(at.x, at.y, at.z + 0.2f), tq_v3(cosf(a) * sp, sinf(a) * sp, randr(3.5f, 7.0f)),
                 randr(1.1f, 1.7f), randr(0.09f, 0.17f), 0.12f, i & 1 ? soil : (t->biome == TQ_B_LUNA ? 0x55555C : 0x5E4630));
    }
}

/* The usual blast: damage by distance, the crater, trees and stones */
static void blast(tq_state_t *s, tq_terrain_t *t, tq_v3_t at, int w, int level, int owner, float dmg_k, float rad_k,
                  tq_event_t *ev)
{
    const tq_weapon_t *W = &TQ_WEAPON[w];
    const tq_tank_t *o = owner >= 0 ? &s->tank[owner] : NULL;
    float radius = W->radius * tq_w_radius_k(level) * rad_k * (o ? weapon_k(o, w, true) : 1);
    float damage = W->damage * tq_w_damage_k(level) * dmg_k * (o ? weapon_k(o, w, false) : 1);
    for (int k = 0; k < s->ntank; k++) {
        tq_tank_t *tk = &s->tank[k];
        if (!tk->alive) continue;
        float dx = at.x - tk->x, dy = at.y - tk->y, dz = at.z - (tq_tank_z(t, tk) + 0.8f);
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        bool direct = d < TQ_TANK[tk->type].body_r + 0.1f;
        if (d < radius || direct) {
            int bonus = W->fx == TQ_FX_SCATTER ? 0 : 8;
            int dmg = direct ? (int)(damage + bonus) : (int)(damage * (1.0f - 0.75f * d / radius) + 0.5f);
            ev->damage[k] += hurt(s, k, dmg, owner);
            ev->direct[k] |= direct;
        }
    }
    float g = tq_ter_height(t, at.x, at.y);
    float M = map_m(t);
    if (W->crater_r > 0 && at.z - g < 1.2f && at.x > 0 && at.y > 0 && at.x < M && at.y < M)
        ev_dirty(ev, tq_ter_crater(t, at.x, at.y, W->crater_r * rad_k, W->crater_d * rad_k, true));
    scatter_props(s, t, at, radius);
    fireball(s, t, at, radius, 0xFFF2C0);
}

/* --------------------------------------------------------------------------
 * Shells
 * -------------------------------------------------------------------------- */

void tq_game_muzzle(const tq_tank_t *k, const tq_terrain_t *t, tq_v3_t *p, tq_v3_t *v)
{
    const tq_tank_type_t *T = &TQ_TANK[k->type];
    float cp = cosf(k->pitch), sp = sinf(k->pitch);
    float cy = cosf(k->tyaw), sy = sinf(k->tyaw);
    float fwd = T->pivot_x + T->muzzle * cp, up = T->pivot_z + T->muzzle * sp;
    float z = tq_tank_z(t, k);
    *p = tq_v3(k->x + cy * fwd, k->y + sy * fwd, z + up);
    float spd = TQ_WEAPON[tq_weapon_of(k)].speed;
    *v = tq_v3(spd * cp * cy, spd * cp * sy, spd * sp);
}

enum { HIT_NONE = 0, HIT_GROUND, HIT_TREE, HIT_BLOCK, HIT_TANK, HIT_OUT };

static int hit_test(const tq_state_t *s, const tq_terrain_t *t, tq_v3_t p, int owner, float age, bool ground,
                    int *which)
{
    float M = map_m(t);
    if (p.x < -3 || p.y < -3 || p.x > M + 3 || p.y > M + 3 || p.z < TQ_BASE_Z - 3) return HIT_OUT;
    for (int k = 0; k < s->ntank; k++) {
        if (k == owner && age < 0.25f) continue;
        const tq_tank_t *tk = &s->tank[k];
        if (!tk->alive) continue;
        float r = TQ_TANK[tk->type].body_r;
        float dx = (p.x - tk->x) / r, dy = (p.y - tk->y) / r, dz = (p.z - tq_tank_z(t, tk) - 0.85f) / 0.95f;
        if (dx * dx + dy * dy + dz * dz < 1) {
            *which = k;
            return HIT_TANK;
        }
    }
    if (ground) {
        bool in_map = p.x >= 0 && p.y >= 0 && p.x <= M && p.y <= M;
        float g = in_map ? tq_ter_height(t, p.x, p.y) : TQ_BASE_Z;
        if (p.z <= g) return HIT_GROUND;
    }
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

static int nearest_enemy(const tq_state_t *s, int who, tq_v3_t p)
{
    int best = -1;
    float bd = 1e9f;
    for (int k = 0; k < s->ntank; k++) {
        const tq_tank_t *e = &s->tank[k];
        if (!e->alive || e->team == s->tank[who].team) continue;
        float d = (e->x - p.x) * (e->x - p.x) + (e->y - p.y) * (e->y - p.y);
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

/* One physics step of a shell in the air (no collisions) */
static void shell_move(const tq_state_t *s, tq_shell_t *sh)
{
    const tq_weapon_t *W = &TQ_WEAPON[sh->weapon];
    if (W->fly == TQ_FLY_GUIDED && sh->t > 0.25f) {
        int e = nearest_enemy(s, sh->owner, sh->p);
        if (e >= 0) {
            /* turn the horizontal velocity towards it, 50 deg/s */
            float want = atan2f(s->tank[e].y - sh->p.y, s->tank[e].x - sh->p.x);
            float have = atan2f(sh->v.y, sh->v.x), hs = sqrtf(sh->v.x * sh->v.x + sh->v.y * sh->v.y);
            float d = want - have;
            while (d > TQ_PI) d -= 2 * TQ_PI;
            while (d < -TQ_PI) d += 2 * TQ_PI;
            float m = 50 * TQ_DEG * STEP;
            have += d > m ? m : (d < -m ? -m : d);
            sh->v.x = cosf(have) * hs;
            sh->v.y = sinf(have) * hs;
        }
    }
    sh->v.z -= GRAV * W->gravity * s->grav_k * STEP;
    if (W->fly != TQ_FLY_BEAM && !sh->under) {
        sh->v.x += s->wind_x * STEP;
        sh->v.y += s->wind_y * STEP;
    }
    sh->p.x += sh->v.x * STEP;
    sh->p.y += sh->v.y * STEP;
    sh->p.z += sh->v.z * STEP;
    sh->t += STEP;
}

/* Flies a shell from p with v with no effects; what it hit, and where */
static int fly(const tq_state_t *s, const tq_terrain_t *t, tq_shell_t sh, tq_v3_t *end, tq_v3_t *pts, int *npts,
               int maxpts)
{
    int which = -1, n = 0;
    for (int i = 0; i < 1400; i++) {
        shell_move(s, &sh);
        if (pts && n < maxpts && (i % 4) == 3) pts[n++] = sh.p;
        int h = hit_test(s, t, sh.p, sh.owner, sh.t, true, &which);
        if (h) {
            *end = sh.p;
            if (npts) *npts = n;
            return h;
        }
    }
    *end = sh.p;
    if (npts) *npts = n;
    return HIT_OUT;
}

static tq_v3_t barrel_dir(const tq_tank_t *k)
{
    float cp = cosf(k->pitch);
    return tq_v3(cp * cosf(k->tyaw), cp * sinf(k->tyaw), sinf(k->pitch));
}

static tq_shell_t shell_for(const tq_tank_t *k, const tq_terrain_t *t, int who)
{
    tq_shell_t sh;
    memset(&sh, 0, sizeof sh);
    tq_game_muzzle(k, t, &sh.p, &sh.v);
    sh.weapon = tq_weapon_of(k);
    sh.level = k->level[k->sel];
    sh.owner = who;
    sh.live = true;
    return sh;
}

static bool in_smoke(const tq_state_t *s, float x, float y)
{
    for (int z = 0; z < TQ_MAX_ZONES; z++) {
        const tq_zone_t *zn = &s->zone[z];
        if (!zn->live || zn->kind != TQ_ZONE_SMOKE) continue;
        if ((x - zn->x) * (x - zn->x) + (y - zn->y) * (y - zn->y) < zn->r * zn->r) return true;
    }
    return false;
}

void tq_game_arc(tq_state_t *s, const tq_terrain_t *t)
{
    const tq_tank_t *k = &s->tank[s->cur];
    int w = tq_weapon_of(k);
    s->narc = 0;
    s->arc_full = false;
    s->arc_landed = false;
    if (TQ_WEAPON[w].fly == TQ_FLY_SELF) return;
    if (in_smoke(s, k->x, k->y)) return;            /* in the smoke, no guide */
    tq_shell_t sh = shell_for(k, t, s->cur);
    if (TQ_WEAPON[w].fly == TQ_FLY_BEAM) {
        /* the laser: a straight line until something */
        int which;
        tq_v3_t d = barrel_dir(k);
        int n = 0;
        for (float r = 0.3f; r < 40 && n < TQ_MAX_ARC; r += 0.35f) {
            tq_v3_t p = tq_v3(sh.p.x + d.x * r, sh.p.y + d.y * r, sh.p.z + d.z * r);
            s->arc[n++] = p;
            if (hit_test(s, t, p, s->cur, 1, true, &which)) {
                s->arc_end = p;
                s->arc_landed = true;
                break;
            }
        }
        s->narc = n;
        s->arc_full = s->arc_landed;
        return;
    }
    int n = 0;
    int h = fly(s, t, sh, &s->arc_end, s->arc, &n, TQ_MAX_ARC);
    float g = tq_w_guide(w, k->level[k->sel], s->diff);
    if (s->weather == TQ_WX_FOG) g *= 0.7f;
    s->narc = (int)(n * g + 0.5f);
    if (s->narc < 4 && n >= 4) s->narc = 4;
    if (s->narc > n) s->narc = n;
    s->arc_landed = h != HIT_OUT;
    s->arc_full = g >= 0.999f && s->narc == n;
}

static tq_shell_t *new_shell(tq_state_t *s)
{
    for (int i = 0; i < TQ_MAX_SHELLS; i++)
        if (!s->shell[i].live) return &s->shell[i];
    return NULL;
}

static void beam(tq_state_t *s, tq_terrain_t *t, tq_tank_t *k, tq_event_t *ev, int *nev)
{
    tq_shell_t sh = shell_for(k, t, s->cur);
    tq_v3_t d = barrel_dir(k);
    if (in_smoke(s, k->x, k->y)) {
        float a = randr(-6, 6) * TQ_DEG, c = cosf(a), sn = sinf(a);
        d = tq_v3(d.x * c - d.y * sn, d.x * sn + d.y * c, d.z);
    }
    s->beam_a = sh.p;
    s->beam_rgb = TQ_WEAPON[TQ_W_LASER].rgb;
    tq_v3_t p = sh.p;
    tq_event_t *e = &ev[(*nev)++];
    ev_clear(e, TQ_EV_BLAST, p);
    for (float r = 0.3f; r < 40; r += 0.2f) {
        p = tq_v3(sh.p.x + d.x * r, sh.p.y + d.y * r, sh.p.z + d.z * r);
        int which = -1;
        int h = hit_test(s, t, p, s->cur, 1, true, &which);
        if (h == HIT_TREE) {
            /* it cuts trees and goes on */
            s->tree[which].alive = 0;
            scatter_props(s, t, p, 0.2f);
            for (int i = 0; i < 6; i++)
                add_part(s, TQ_PART_SPARK, p, tq_v3(randr(-2, 2), randr(-2, 2), randr(0, 3)), 0.4f, 0.08f, 0.02f, 0xFF7AA8);
            continue;
        }
        if (h) break;
    }
    s->beam_b = p;
    s->beam_t = 0.5f;
    e->at = p;
    blast(s, t, p, TQ_W_LASER, sh.level, s->cur, 1, 1, e);
}

void tq_game_fire(tq_state_t *s, tq_terrain_t *t, tq_event_t *ev, int *nev)
{
    tq_tank_t *k = &s->tank[s->cur];
    int w = tq_weapon_of(k);
    const tq_weapon_t *W = &TQ_WEAPON[w];
    *nev = 0;
    k->ap -= tq_shot_ap(k, k->sel);
    if (k->ap < 0) k->ap = 0;
    k->shots++;
    if (k->uses[k->sel] != 0xFF && k->uses[k->sel] > 0) k->uses[k->sel]--;
    tq_v3_t at = tq_v3(k->x, k->y, tq_tank_z(t, k) + 2.4f);
    if (W->fx == TQ_FX_SHIELD) {
        k->shield += (int)(W->damage * tq_w_damage_k(k->level[k->sel]) * (k->type == TQ_T_TORTUGA ? 1.5f : 1.0f));
        ev_clear(&ev[0], TQ_EV_SHIELD, at);
        ev[0].value = k->shield;
        *nev = 1;
        add_part(s, TQ_PART_RING, tq_v3(k->x, k->y, at.z - 2.2f), tq_v3(0, 0, 0), 0.6f, 0.5f, 2.6f, W->rgb);
        return;
    }
    if (W->fx == TQ_FX_REPAIR) {
        int heal = (int)(W->damage * tq_w_damage_k(k->level[k->sel]) * (k->type == TQ_T_TOPO ? 1.5f : 1.0f));
        if (heal > k->hpmax - k->hp) heal = k->hpmax - k->hp;
        k->hp += heal;
        ev_clear(&ev[0], TQ_EV_HEAL, at);
        ev[0].value = heal;
        *nev = 1;
        for (int i = 0; i < 10; i++)
            add_part(s, TQ_PART_SPARK, tq_v3(k->x + randr(-1, 1), k->y + randr(-1, 1), at.z - 1.6f),
                     tq_v3(0, 0, randr(1, 2.5f)), 0.9f, 0.12f, 0.04f, W->rgb);
        return;
    }
    if (W->fly == TQ_FLY_BEAM) {
        beam(s, t, k, ev, nev);
        return;
    }
    tq_shell_t base = shell_for(k, t, s->cur);
    if (in_smoke(s, k->x, k->y)) {
        float a = randr(-6, 6) * TQ_DEG, c = cosf(a), sn = sinf(a);
        base.v = tq_v3(base.v.x * c - base.v.y * sn, base.v.x * sn + base.v.y * c, base.v.z);
    }
    if (w == TQ_W_TWIN) {
        /* two: the second a moment later, kicked off by the first's recoil */
        for (int i = 0; i < 2; i++) {
            tq_shell_t *sh = new_shell(s);
            if (!sh) break;
            *sh = base;
            if (i) {
                float a = (tq_rand() < 0.5f ? -1 : 1) * randr(1.5f, 3.5f) * TQ_DEG, c = cosf(a), sn = sinf(a);
                float up = randr(0.4f, 1.8f) * TQ_DEG;
                float hs = sqrtf(base.v.x * base.v.x + base.v.y * base.v.y);
                sh->v = tq_v3(base.v.x * c - base.v.y * sn, base.v.x * sn + base.v.y * c, base.v.z + hs * up);
                sh->wait = 0.3f;
            }
        }
    }
    int n = w == TQ_W_TWIN ? 0 : (W->fx == TQ_FX_SCATTER ? 5 : 1);
    for (int i = 0; i < n; i++) {
        tq_shell_t *sh = new_shell(s);
        if (!sh) break;
        *sh = base;
        if (n > 1) {
            float a = (i - 2) * 4.0f * TQ_DEG + randr(-1, 1) * TQ_DEG, c = cosf(a), sn = sinf(a);
            sh->v = tq_v3(base.v.x * c - base.v.y * sn, base.v.x * sn + base.v.y * c, base.v.z + randr(-0.6f, 0.6f));
        }
    }
    /* the muzzle's flash and smoke */
    for (int i = 0; i < 6; i++) {
        bool fl = i < 2;
        add_part(s, fl ? TQ_PART_FIRE : TQ_PART_SMOKE, base.p,
                 tq_v3(base.v.x * 0.06f + randr(-0.5f, 0.5f), base.v.y * 0.06f + randr(-0.5f, 0.5f), randr(0.2f, 0.9f)),
                 fl ? 0.18f : randr(0.6f, 1.0f), fl ? 0.35f : 0.2f, fl ? 0.6f : randr(0.5f, 0.8f), 0xE8E4DC);
    }
}

bool tq_game_flying(const tq_state_t *s)
{
    for (int i = 0; i < TQ_MAX_SHELLS; i++)
        if (s->shell[i].live) return true;
    return false;
}

const tq_shell_t *tq_game_lead(const tq_state_t *s)
{
    for (int i = 0; i < TQ_MAX_SHELLS; i++)
        if (s->shell[i].live) return &s->shell[i];
    return NULL;
}

static void add_zone(tq_state_t *s, int kind, float x, float y, float r, int owner, int turns)
{
    for (int z = 0; z < TQ_MAX_ZONES; z++) {
        if (s->zone[z].live) continue;
        s->zone[z] = (tq_zone_t){ x, y, r, kind, owner, turns, true };
        return;
    }
}

/* A shell landed: what its weapon does there */
static void land(tq_state_t *s, tq_terrain_t *t, tq_shell_t *sh, tq_v3_t at, tq_event_t *ev, int *nev)
{
    const tq_weapon_t *W = &TQ_WEAPON[sh->weapon];
    if (*nev >= TQ_MAX_EVENTS) return;
    tq_event_t *e = &ev[(*nev)++];
    ev_clear(e, TQ_EV_BLAST, at);
    e->weapon = sh->weapon;
    e->split = sh->split;
    e->water = tq_ter_water(t, at.x, at.y);
    tq_tank_t *o = &s->tank[sh->owner];
    float g = tq_ter_height(t, at.x, at.y);
    switch (sh->split ? TQ_FX_BLAST : W->fx) {
    case TQ_FX_NAPALM:
        blast(s, t, at, sh->weapon, sh->level, sh->owner, 1, 1, e);
        add_zone(s, TQ_ZONE_FIRE, at.x, at.y, 3.0f * tq_w_radius_k(sh->level), sh->owner, 2);
        break;
    case TQ_FX_MINES: {
        e->type = TQ_EV_NONE;
        int n = o->type == TQ_T_TOPO ? 4 : 3;
        for (int i = 0; i < n; i++) {
            for (int m = 0; m < TQ_MAX_MINES; m++) {
                if (s->mine[m].live) continue;
                float a = i * 2 * TQ_PI / n + randr(-0.3f, 0.3f);
                s->mine[m] = (tq_mine_t){ at.x + cosf(a) * 1.4f, at.y + sinf(a) * 1.4f, o->team, sh->owner, true };
                break;
            }
        }
        add_part(s, TQ_PART_RING, tq_v3(at.x, at.y, g + 0.05f), tq_v3(0, 0, 0), 0.4f, 0.2f, 1.8f, W->rgb);
        break;
    }
    case TQ_FX_EMP:
        e->type = TQ_EV_EMP;
        for (int k = 0; k < s->ntank; k++) {
            tq_tank_t *tk = &s->tank[k];
            if (!tk->alive || tk->team == o->team) continue;
            float dx = at.x - tk->x, dy = at.y - tk->y;
            if (dx * dx + dy * dy < W->radius * W->radius * tq_w_radius_k(sh->level)) {
                tk->emp = tk->apmax / 2;
                tk->shield = 0;
                e->value++;
            }
        }
        add_part(s, TQ_PART_RING, tq_v3(at.x, at.y, g + 0.1f), tq_v3(0, 0, 0), 0.7f, 0.4f, W->radius * 1.3f, W->rgb);
        add_part(s, TQ_PART_FLASH, at, tq_v3(0, 0, 0), 0.25f, 0.6f, 2.0f, 0xBFF0FF);
        break;
    case TQ_FX_SMOKE:
        e->type = TQ_EV_NONE;
        add_zone(s, TQ_ZONE_SMOKE, at.x, at.y, W->radius, sh->owner, 2);
        for (int i = 0; i < 14; i++)
            add_part(s, TQ_PART_SMOKE, tq_v3(at.x + randr(-2, 2), at.y + randr(-2, 2), g + randr(0.3f, 1.5f)),
                     tq_v3(randr(-0.3f, 0.3f), randr(-0.3f, 0.3f), 0.2f), randr(2.5f, 3.5f), 0.6f, randr(1.4f, 2.0f),
                     0xD8D4CC);
        break;
    case TQ_FX_AIRSTRIKE: {
        /* five bombs along a line across the shot, from high up */
        e->type = TQ_EV_NONE;
        float dx = at.x - o->x, dy = at.y - o->y, l = sqrtf(dx * dx + dy * dy) + 1e-3f;
        for (int i = 0; i < 5; i++) {
            tq_shell_t *b = new_shell(s);
            if (!b) break;
            memset(b, 0, sizeof *b);
            float off = (i - 2) * 1.6f;
            b->p = tq_v3(at.x + dx / l * off, at.y + dy / l * off, g + 16 + i * 2.0f);
            b->v = tq_v3(0, 0, -8);
            b->weapon = TQ_W_AIRSTRIKE;
            b->level = sh->level;
            b->owner = sh->owner;
            b->split = true;
            b->live = true;
            b->t = 1;
        }
        add_part(s, TQ_PART_SMOKE, tq_v3(at.x, at.y, g + 0.3f), tq_v3(0, 0, 1.5f), 2.5f, 0.3f, 1.2f, 0xFF5A46);
        break;
    }
    case TQ_FX_QUAKE: {
        blast(s, t, at, sh->weapon, sh->level, sh->owner, 1, 1, e);
        /* a ring of ground pushed up round the dip */
        for (int i = 0; i < 6; i++) {
            float a = i * TQ_PI / 3 + randr(-0.3f, 0.3f), r = W->radius * 0.8f;
            ev_dirty(e, tq_ter_crater(t, at.x + cosf(a) * r, at.y + sinf(a) * r, 1.6f, -0.9f, false));
        }
        break;
    }
    case TQ_FX_TELEPORT: {
        e->type = TQ_EV_TELEPORT;
        float M = map_m(t);
        float x = tq_clampf(at.x, 2.5f, M - 2.5f), y = tq_clampf(at.y, 2.5f, M - 2.5f);
        if (o->type != TQ_T_ANFIBIO) tq_ter_land(t, &x, &y);
        bool clear = true;
        for (int k = 0; k < s->ntank; k++)
            if (k != sh->owner && s->tank[k].alive &&
                (s->tank[k].x - x) * (s->tank[k].x - x) + (s->tank[k].y - y) * (s->tank[k].y - y) < 9)
                clear = false;
        for (int k = 0; k < 14; k++)
            add_part(s, TQ_PART_SPARK, tq_v3(o->x + randr(-1, 1), o->y + randr(-1, 1), tq_tank_z(t, o) + randr(0, 2)),
                     tq_v3(0, 0, randr(1, 3)), 0.7f, 0.12f, 0.03f, W->rgb);
        if (clear) {
            o->x = x;
            o->y = y;
        }
        for (int k = 0; k < 14; k++)
            add_part(s, TQ_PART_SPARK, tq_v3(o->x + randr(-1, 1), o->y + randr(-1, 1), tq_tank_z(t, o) + randr(0, 2)),
                     tq_v3(0, 0, randr(-1, 1)), 0.9f, 0.12f, 0.03f, W->rgb);
        e->at = tq_v3(o->x, o->y, tq_tank_z(t, o) + 2.4f);
        break;
    }
    case TQ_FX_WALL: {
        e->type = TQ_EV_NONE;
        float dx = at.x - o->x, dy = at.y - o->y;
        bool across_x = fabsf(dy) > fabsf(dx);              /* the wall stands across the shot */
        int ci = (int)at.x, cj = (int)at.y;
        for (int q = -1; q <= 1; q++)
            add_block(s, t, ci + (across_x ? q : 0), cj + (across_x ? 0 : q), 0);
        for (int i = 0; i < 8; i++)
            add_part(s, TQ_PART_PUFF, tq_v3(at.x + randr(-1.5f, 1.5f), at.y + randr(-1.5f, 1.5f), g + 0.3f),
                     tq_v3(0, 0, 0.5f), 0.9f, 0.3f, 0.8f, 0xC9B48E);
        break;
    }
    case TQ_FX_TESLA: {
        /* the lightning jumps from where it fell to whatever is near, the
         * nearest first, each jump from the last one hit */
        e->type = TQ_EV_TESLA;
        float R = W->radius * tq_w_radius_k(sh->level);
        float dmg = W->damage * tq_w_damage_k(sh->level) * weapon_k(o, sh->weapon, false);
        tq_v3_t from = tq_v3(at.x, at.y, at.z + 0.4f);
        bool hit_t[TQ_MAX_TANKS] = { false }, hit_tr[TQ_MAX_TREES] = { false }, hit_b[TQ_MAX_BLOCKS] = { false };
        s->nbolt = 0;
        while (s->nbolt < TQ_MAX_BOLTS) {
            int kind = -1, idx = -1;
            float best = R * R;
            tq_v3_t to = from;
            for (int k = 0; k < s->ntank; k++) {
                const tq_tank_t *tk = &s->tank[k];
                if (!tk->alive || hit_t[k] || k == sh->owner) continue;
                float d = (tk->x - at.x) * (tk->x - at.x) + (tk->y - at.y) * (tk->y - at.y);
                if (d < best) { best = d; kind = 0; idx = k; to = tq_v3(tk->x, tk->y, tq_tank_z(t, tk) + 1.1f); }
            }
            for (int k = 0; k < s->ntree; k++) {
                const tq_tree_t *tr = &s->tree[k];
                if (!tr->alive || hit_tr[k]) continue;
                float d = (tr->x - at.x) * (tr->x - at.x) + (tr->y - at.y) * (tr->y - at.y);
                if (d < best) { best = d; kind = 1; idx = k; to = tq_v3(tr->x, tr->y, tq_ter_height(t, tr->x, tr->y) + 2.0f); }
            }
            for (int k = 0; k < s->nblock; k++) {
                const tq_block_t *b = &s->block[k];
                if (!b->alive || hit_b[k]) continue;
                float bx = b->i + 0.5f, by = b->j + 0.5f;
                float d = (bx - at.x) * (bx - at.x) + (by - at.y) * (by - at.y);
                if (d < best) { best = d; kind = 2; idx = k; to = tq_v3(bx, by, b->z + 0.6f); }
            }
            if (kind < 0) break;
            s->bolt_a[s->nbolt] = from;
            s->bolt_b[s->nbolt] = to;
            s->nbolt++;
            if (kind == 0) {
                hit_t[idx] = true;
                e->damage[idx] += hurt(s, idx, (int)(dmg + 0.5f), sh->owner);
                e->direct[idx] = true;
            } else if (kind == 1) {
                hit_tr[idx] = true;
                scatter_props(s, t, to, 0.3f);
            } else {
                hit_b[idx] = true;
                s->block[idx].alive = 0;
            }
            for (int q = 0; q < 6; q++)
                add_part(s, TQ_PART_SPARK, to, tq_v3(randr(-2, 2), randr(-2, 2), randr(0, 3)), 0.5f, 0.10f, 0.03f, W->rgb);
            from = to;
        }
        s->bolt_t = 0.9f;
        add_part(s, TQ_PART_FLASH, tq_v3(at.x, at.y, at.z + 0.4f), tq_v3(0, 0, 0), 0.3f, 0.6f, 2.4f, 0xCFF6FF);
        add_part(s, TQ_PART_RING, tq_v3(at.x, at.y, g + 0.08f), tq_v3(0, 0, 0), 0.5f, 0.3f, R, W->rgb);
        ev_dirty(e, tq_ter_crater(t, at.x, at.y, W->crater_r, W->crater_d, true));
        if (s->nbolt) e->at = s->bolt_b[0];
        break;
    }
    case TQ_FX_CLUSTER:
    default:
        blast(s, t, at, sh->weapon, sh->level, sh->owner, sh->split && W->fx == TQ_FX_CLUSTER ? 1 : 1, 1, e);
        break;
    }
}

static void spawn_cluster(tq_state_t *s, tq_shell_t *sh)
{
    for (int i = 0; i < 5; i++) {
        tq_shell_t *b = new_shell(s);
        if (!b) break;
        *b = *sh;
        float a = i * 2 * TQ_PI / 5, sp = randr(2.2f, 3.4f);
        b->v = tq_v3(sh->v.x + cosf(a) * sp, sh->v.y + sinf(a) * sp, sh->v.z + randr(-0.5f, 1.0f));
        b->split = true;
    }
    sh->live = false;
    add_part(s, TQ_PART_FLASH, sh->p, tq_v3(0, 0, 0), 0.15f, 0.2f, 0.9f, 0xFFF2A0);
}

void tq_game_step(tq_state_t *s, tq_terrain_t *t, float dt, tq_event_t *ev, int *nev)
{
    *nev = 0;
    int steps = (int)(dt / STEP + 0.5f);
    if (steps < 1) steps = 1;
    for (int i = 0; i < TQ_MAX_SHELLS; i++) {
        tq_shell_t *sh = &s->shell[i];
        if (!sh->live) continue;
        const tq_weapon_t *W = &TQ_WEAPON[sh->weapon];
        if (sh->wait > 0) {
            /* the double cannon's second, still in the barrel */
            sh->wait -= dt;
            if (sh->wait <= 0 && *nev < TQ_MAX_EVENTS) {
                tq_event_t *e = &ev[(*nev)++];
                ev_clear(e, TQ_EV_FIRE2, sh->p);
                e->weapon = sh->weapon;
                for (int q = 0; q < 5; q++)
                    add_part(s, q < 2 ? TQ_PART_FIRE : TQ_PART_SMOKE, sh->p,
                             tq_v3(sh->v.x * 0.06f + randr(-0.5f, 0.5f), sh->v.y * 0.06f + randr(-0.5f, 0.5f), randr(0.2f, 0.9f)),
                             q < 2 ? 0.18f : randr(0.6f, 1.0f), q < 2 ? 0.35f : 0.2f, q < 2 ? 0.6f : 0.7f, 0xE8E4DC);
            }
            continue;
        }
        for (int st = 0; st < steps && sh->live; st++) {
            float vz0 = sh->v.z;
            shell_move(s, sh);
            if (W->fx == TQ_FX_CLUSTER && !sh->split && vz0 > 0 && sh->v.z <= 0) {
                spawn_cluster(s, sh);
                break;
            }
            int which = -1;
            if (sh->under) {
                /* the drill: through the ground, then up it goes */
                sh->drill += sqrtf(sh->v.x * sh->v.x + sh->v.y * sh->v.y + sh->v.z * sh->v.z) * STEP;
                int h = hit_test(s, t, sh->p, sh->owner, sh->t, false, &which);
                if (sh->drill > 3.0f || h == HIT_TANK || h == HIT_OUT) {
                    sh->live = false;
                    land(s, t, sh, sh->p, ev, nev);
                }
                continue;
            }
            int h = hit_test(s, t, sh->p, sh->owner, sh->t, true, &which);
            if (!h && sh->t < 9) continue;
            if (h == HIT_GROUND && W->fly == TQ_FLY_DRILL && !sh->split) {
                sh->under = true;
                sh->v = tq_v3(sh->v.x * 0.5f, sh->v.y * 0.5f, sh->v.z * 0.5f);
                for (int k = 0; k < 6; k++)
                    add_part(s, TQ_PART_CHUNK, sh->p, tq_v3(randr(-2, 2), randr(-2, 2), randr(2, 4)), 1.0f, 0.12f, 0.1f,
                             0x7A5C3E);
                continue;
            }
            if (h == HIT_GROUND && W->fly == TQ_FLY_BOUNCE && sh->bounces < 2) {
                tq_v3_t n;
                tq_ter_normal(t, sh->p.x, sh->p.y, &n);
                float d = tq_dot(sh->v, n);
                sh->v = tq_v3((sh->v.x - 2 * d * n.x) * 0.6f, (sh->v.y - 2 * d * n.y) * 0.6f, (sh->v.z - 2 * d * n.z) * 0.6f);
                sh->p.z = tq_ter_height(t, sh->p.x, sh->p.y) + 0.05f;
                sh->bounces++;
                add_part(s, TQ_PART_PUFF, sh->p, tq_v3(0, 0, 0.6f), 0.6f, 0.2f, 0.6f, 0xC9B48E);
                continue;
            }
            sh->live = false;
            if (h == HIT_OUT) {
                if (*nev < TQ_MAX_EVENTS) ev_clear(&ev[(*nev)++], TQ_EV_MISS, sh->p);
            } else {
                land(s, t, sh, sh->p, ev, nev);
            }
        }
        /* a trail of little puffs */
        if (sh->live && !sh->under && s->npart < TQ_MAX_PARTS - 40)
            add_part(s, TQ_PART_SMOKE, sh->p, tq_v3(randr(-0.2f, 0.2f), randr(-0.2f, 0.2f), 0.3f), 0.7f, 0.10f, 0.32f,
                     W->fly == TQ_FLY_GUIDED ? 0xFFD2B0 : 0xF0EEE8);
    }
    if (s->beam_t > 0) s->beam_t -= dt;
    if (s->bolt_t > 0) {
        s->bolt_t -= dt;
        if (s->bolt_t <= 0) s->nbolt = 0;
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
            q->v.z -= GRAV * s->grav_k * dt * (q->kind == TQ_PART_LEAF ? 0.35f : 1.0f);
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
            q->p.x += q->v.x * dt + (q->kind == TQ_PART_SMOKE ? s->wind_x * 0.15f * dt : 0);
            q->p.y += q->v.y * dt + (q->kind == TQ_PART_SMOKE ? s->wind_y * 0.15f * dt : 0);
            q->p.z += q->v.z * dt;
        }
        i++;
    }
    /* the zones keep puffing */
    for (int z = 0; z < TQ_MAX_ZONES; z++) {
        const tq_zone_t *zn = &s->zone[z];
        if (!zn->live || tq_rand() > dt * (zn->kind == TQ_ZONE_FIRE ? 14 : 5) || s->npart > TQ_MAX_PARTS - 50) continue;
        float a = randr(0, 2 * TQ_PI), r = sqrtf(tq_rand()) * zn->r;
        float x = zn->x + cosf(a) * r, y = zn->y + sinf(a) * r;
        float g = tq_ter_height(t, x, y);
        if (zn->kind == TQ_ZONE_FIRE)
            add_part(s, TQ_PART_FIRE, tq_v3(x, y, g + 0.2f), tq_v3(0, 0, randr(0.8f, 1.6f)), randr(0.5f, 0.8f), 0.25f,
                     randr(0.5f, 0.8f), 0);
        else
            add_part(s, TQ_PART_SMOKE, tq_v3(x, y, g + randr(0.3f, 1.4f)), tq_v3(0, 0, 0.1f), randr(2.0f, 3.0f), 0.6f,
                     randr(1.3f, 1.9f), 0xD8D4CC);
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
            b->z = sup;
        }
    }
    for (int k = 0; k < s->ntank; k++)
        if (s->tank[k].hit_flash > 0) s->tank[k].hit_flash -= dt;
}

bool tq_game_mines(tq_state_t *s, tq_terrain_t *t, int who, tq_event_t *ev)
{
    tq_tank_t *k = &s->tank[who];
    for (int m = 0; m < TQ_MAX_MINES; m++) {
        tq_mine_t *mn = &s->mine[m];
        if (!mn->live || mn->team == k->team) continue;
        float dx = k->x - mn->x, dy = k->y - mn->y;
        if (dx * dx + dy * dy > 1.2f * 1.2f) continue;
        mn->live = false;
        tq_v3_t at = tq_v3(mn->x, mn->y, tq_ter_height(t, mn->x, mn->y) + 0.2f);
        ev_clear(ev, TQ_EV_MINE, at);
        blast(s, t, at, TQ_W_MINES, 0, mn->owner, 1, 1, ev);
        return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * The computer's tanks
 * -------------------------------------------------------------------------- */

static int target_of(const tq_state_t *s, int me)
{
    /* the nearest enemy, a weak one a little nearer */
    int best = -1;
    float bd = 1e9f;
    const tq_tank_t *k = &s->tank[me];
    for (int i = 0; i < s->ntank; i++) {
        const tq_tank_t *e = &s->tank[i];
        if (!e->alive || e->team == k->team) continue;
        float d = sqrtf((e->x - k->x) * (e->x - k->x) + (e->y - k->y) * (e->y - k->y)) * (0.6f + 0.4f * e->hp / e->hpmax);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* How far from the target a slot's shot lands, at its best pitch */
static float best_pitch(const tq_state_t *s, const tq_terrain_t *t, int who, int slot, int target, float yaw,
                        float *pitch, float coarse)
{
    tq_tank_t k = s->tank[who];
    const tq_tank_t *e = &s->tank[target];
    int w = k.load[slot];
    const tq_weapon_t *W = &TQ_WEAPON[w];
    float ez = tq_tank_z(t, e) + 0.8f;
    k.tyaw = yaw;
    k.sel = slot;
    float best = 1e9f, bp = 10 * TQ_DEG;
    float lo = W->min_pitch * TQ_DEG, hi = W->max_pitch * TQ_DEG;
    if (W->fly == TQ_FLY_BEAM) {
        /* straight at it: is the line clear? */
        float dx = e->x - k.x, dy = e->y - k.y, dz = ez - (tq_tank_z(t, &k) + TQ_TANK[k.type].pivot_z);
        float pa = atan2f(dz, sqrtf(dx * dx + dy * dy));
        k.pitch = tq_clampf(pa, lo, hi);
        tq_shell_t sh = shell_for(&k, t, who);
        tq_v3_t d = barrel_dir(&k);
        int which = -1;
        for (float r = 0.3f; r < 40; r += 0.3f) {
            tq_v3_t p = tq_v3(sh.p.x + d.x * r, sh.p.y + d.y * r, sh.p.z + d.z * r);
            int h = hit_test(s, t, p, who, 1, true, &which);
            if (h == HIT_TREE) continue;
            if (h == HIT_TANK && which == target) {
                *pitch = k.pitch;
                return 0;
            }
            if (h) break;
        }
        *pitch = k.pitch;
        return 1e9f;
    }
    for (int pass = 0; pass < 2; pass++) {
        float step = pass ? coarse * 0.1f : coarse;
        float a0 = pass ? bp - coarse : lo, a1 = pass ? bp + coarse : hi;
        for (float a = a0; a <= a1 + 1e-4f; a += step) {
            if (a < lo || a > hi) continue;
            k.pitch = a;
            tq_shell_t sh = shell_for(&k, t, who);
            tq_v3_t end;
            int h = fly(s, t, sh, &end, NULL, NULL, 0);
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

/* What a slot is worth against the target from here: damage expected */
static float slot_value(const tq_state_t *s, const tq_terrain_t *t, int who, int slot, int target, float *yaw,
                        float *pitch)
{
    const tq_tank_t *k = &s->tank[who];
    int w = k->load[slot];
    const tq_weapon_t *W = &TQ_WEAPON[w];
    if (!tq_slot_ready(k, slot)) return -1;
    const tq_tank_t *e = &s->tank[target];
    *yaw = atan2f(e->y - k->y, e->x - k->x);
    *pitch = k->pitch;
    switch (W->fx) {
    case TQ_FX_REPAIR:
        return k->hp < k->hpmax * 45 / 100 ? 40.0f : -1;
    case TQ_FX_SHIELD:
        return (k->hp < k->hpmax * 70 / 100 && k->shield == 0) ? 18.0f : -1;
    case TQ_FX_MINES:
    case TQ_FX_SMOKE:
    case TQ_FX_TELEPORT:
    case TQ_FX_WALL:
        return -1;
    default:
        break;
    }
    float d = best_pitch(s, t, who, slot, target, *yaw, pitch, 3 * TQ_DEG);
    float r = W->radius * tq_w_radius_k(k->level[slot]);
    float hit = d < 0.5f ? 1.0f : tq_clampf(1.0f - d / (r + 0.5f), 0, 1);
    float dmg = W->damage * tq_w_damage_k(k->level[slot]);
    if (W->fx == TQ_FX_AIRSTRIKE) dmg *= 2.5f;
    if (W->fx == TQ_FX_SCATTER) dmg *= 2.5f;
    if (W->fx == TQ_FX_CLUSTER) dmg *= 2.2f;
    if (W->fx == TQ_FX_EMP) dmg = 15;
    if (W->fx == TQ_FX_NAPALM) dmg += 12;
    return hit * dmg - tq_shot_ap(k, slot) * 0.5f;
}

/* What the mode wants of a tank of the board's: a spot to head for, how
 * much it matters, and a bonus for being right there; false: nothing */
static bool objective(const tq_state_t *s, int me, float *ox, float *oy, float *w, float *near_r, float *bonus)
{
    const tq_tank_t *k = &s->tank[me];
    int side = k->team & 1;
    switch (s->mode) {
    case TQ_MODE_HILL:
        *ox = s->hill_x;
        *oy = s->hill_y;
        *w = 1.2f;
        *near_r = s->hill_r - 0.6f;
        *bonus = 12;
        return true;
    case TQ_MODE_FLAG: {
        int mine = side, theirs = side ^ 1;
        if (s->flag[theirs].carrier == me) {
            *ox = s->flag[mine].hx;
            *oy = s->flag[mine].hy;
            *w = 2.0f;
            *near_r = 1.6f;
            *bonus = 20;
            return true;
        }
        bool home = s->flag[mine].x == s->flag[mine].hx && s->flag[mine].y == s->flag[mine].hy;
        if (s->flag[mine].carrier < 0 && !home) {
            *ox = s->flag[mine].x;
            *oy = s->flag[mine].y;
        } else if (s->flag[mine].carrier >= 0) {
            /* chase whoever runs with ours */
            *ox = s->tank[s->flag[mine].carrier].x;
            *oy = s->tank[s->flag[mine].carrier].y;
        } else if (s->flag[theirs].carrier < 0) {
            *ox = s->flag[theirs].x;
            *oy = s->flag[theirs].y;
        } else {
            return false;
        }
        *w = 1.2f;
        *near_r = 1.4f;
        *bonus = 14;
        return true;
    }
    case TQ_MODE_TREASURE: {
        float bd = 1e9f;
        int bc = -1;
        for (int c = 0; c < s->nchest; c++) {
            if (!s->chest_alive[c]) continue;
            float d = (s->chest[c][0] - k->x) * (s->chest[c][0] - k->x) + (s->chest[c][1] - k->y) * (s->chest[c][1] - k->y);
            if (d < bd) {
                bd = d;
                bc = c;
            }
        }
        if (bc < 0) return false;
        *ox = s->chest[bc][0];
        *oy = s->chest[bc][1];
        *w = 1.5f;
        *near_r = 1.3f;
        *bonus = 14;
        return true;
    }
    default:
        return false;
    }
}

void tq_ai_plan_move(tq_state_t *s, const tq_terrain_t *t, int *ap_cost)
{
    int me = s->cur;
    int target = target_of(s, me);
    *ap_cost = 0;
    s->npath = 0;
    if (target < 0) return;
    const tq_tank_t *e = &s->tank[target];
    int G = s->grid;
    float ox = 0, oy = 0, ow = 0, onear = 0, obonus = 0;
    bool obj = objective(s, me, &ox, &oy, &ow, &onear, &obonus);
    typedef struct { int c; float score; } cand_t;
    cand_t top[10];
    int ntop = 0;
    for (int c = 0; c < G * G; c++) {
        if (s->reach[c] != 1 && s->reach[c] != 2) continue;
        float x = c % G + 0.5f, y = c / G + 0.5f;
        float d = sqrtf((x - e->x) * (x - e->x) + (y - e->y) * (y - e->y));
        float sc = -fabsf(d - 14.0f) * 0.5f + tq_ter_height(t, x, y) * 0.6f + (s->reach[c] == 1 ? 1.5f : 0) +
                   tq_rand() * 2.0f;
        if (obj) {
            float od = sqrtf((x - ox) * (x - ox) + (y - oy) * (y - oy));
            sc += -od * ow * 0.5f + (od < onear ? obonus : 0);
        }
        for (int z = 0; z < TQ_MAX_ZONES; z++) {
            const tq_zone_t *zn = &s->zone[z];
            if (zn->live && zn->kind == TQ_ZONE_FIRE && (x - zn->x) * (x - zn->x) + (y - zn->y) * (y - zn->y) < zn->r * zn->r)
                sc -= 10;
        }
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
    int tries = s->diff == 0 ? 3 : (obj ? 3 : 8);
    for (int i = 0; i < ntop && i < tries && pick < 0; i++) {
        memcpy(&tmp, s, sizeof tmp);
        tq_tank_t *k = &tmp.tank[me];
        k->x = top[i].c % G + 0.5f;
        k->y = top[i].c / G + 0.5f;
        float yaw = atan2f(e->y - k->y, e->x - k->x), p;
        for (int w = 0; w < k->nload && pick < 0; w++) {
            const tq_weapon_t *W = &TQ_WEAPON[k->load[w]];
            if (W->fly == TQ_FLY_SELF || W->damage == 0) continue;
            if (best_pitch(&tmp, t, me, w, target, yaw, &p, 4 * TQ_DEG) < 2.0f) pick = top[i].c;
        }
    }
    if (pick < 0 && ntop) pick = top[0].c;
    if (pick >= 0) tq_game_route(s, t, pick % G + 0.5f, pick / G + 0.5f, ap_cost);
}

void tq_ai_plan_aim(tq_state_t *s, const tq_terrain_t *t, float *yaw, float *pitch, int *slot)
{
    int me = s->cur;
    const tq_tank_t *k = &s->tank[me];
    int target = target_of(s, me);
    *yaw = k->tyaw;
    *pitch = k->pitch;
    *slot = -1;
    if (target < 0) return;
    float best = 0;
    for (int w = 0; w < k->nload; w++) {
        float y, p;
        float v = slot_value(s, t, me, w, target, &y, &p);
        if (v > best) {
            best = v;
            *slot = w;
            *yaw = y;
            *pitch = p;
        }
    }
    if (*slot < 0 && k->shots == 0) {
        /* nothing lands near: the cheapest shot at the target anyway (a
         * second shot only when it is worth it) */
        for (int w = 0; w < k->nload; w++) {
            if (tq_slot_ready(k, w) && TQ_WEAPON[k->load[w]].fly != TQ_FLY_SELF) {
                *slot = w;
                float y;
                slot_value(s, t, me, w, target, &y, pitch);
                *yaw = y;
                break;
            }
        }
    }
    /* an honest hand: off by the difficulty */
    static const float EY[TQ_DIFFS] = { 3.5f, 2.0f, 0.9f }, EP[TQ_DIFFS] = { 2.5f, 1.4f, 0.6f };
    float ey = EY[k->diff % TQ_DIFFS], ep = EP[k->diff % TQ_DIFFS];
    if (in_smoke(s, s->tank[target].x, s->tank[target].y)) {
        ey *= 2;
        ep *= 2;
    }
    *yaw += randr(-ey, ey) * TQ_DEG;
    *pitch += randr(-ep, ep) * TQ_DEG;
}
