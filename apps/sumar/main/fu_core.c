/*
 * P4OS - Fusión: the rules
 *
 * A move takes every line in the direction of the swipe, leading edge first,
 * and packs its tiles against that edge. Two equal tiles that meet become one
 * of the next value, and a tile made by a merge does not merge again in the
 * same move (so 2 2 4 to the left is 4 4, not 8). A stone (FU_BLOCK) cuts the
 * line in two: each piece packs against its own edge, the stone or the side.
 *
 * Besides the new grid, a move describes what happened to each tile (where it
 * went, whether it was swallowed) so the screen can animate it, with no idea
 * of the rules of its own.
 */
#include "fu_app.h"

#include <string.h>

fu_game_t fu_G;

/* ---- a small random generator: xorshift32 ---- */

static uint32_t s_rng = 0x9E3779B9u;
static uint16_t s_id;

void fu_rng_seed(uint32_t seed)
{
    s_rng = seed ? seed : 0x9E3779B9u;
}

uint32_t fu_rng(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

uint16_t fu_new_id(void)
{
    if (++s_id == 0) s_id = 1;
    return s_id;
}

/* Where the k-th cell of line i is, counting from the edge the tiles go to. */
static void line_cell(int n, int dir, int i, int k, int *r, int *c)
{
    switch (dir) {
    case FU_LEFT:  *r = i;         *c = k;         break;
    case FU_RIGHT: *r = i;         *c = n - 1 - k; break;
    case FU_UP:    *r = k;         *c = i;         break;
    default:       *r = n - 1 - k; *c = i;         break;
    }
}

bool fu_core_move(fu_grid_t *g, int n, int dir, fu_move_t *out)
{
    fu_grid_t ng;
    memset(&ng, 0, sizeof ng);
    memset(out, 0, sizeof *out);

    for (int i = 0; i < n; i++) {
        int t = 0;                  /* where the next tile lands */
        int last = -1;              /* the slot of the last tile placed */
        bool last_merged = false;
        for (int k = 0; k < n; k++) {
            int r, c;
            line_cell(n, dir, i, k, &r, &c);
            uint8_t e = g->v[r][c];
            if (e == FU_BLOCK) {
                ng.v[r][c] = FU_BLOCK;
                t = k + 1;
                last = -1;
                continue;
            }
            if (!e) continue;
            uint16_t id = g->id[r][c];
            int lr = 0, lc = 0;
            if (last >= 0) line_cell(n, dir, i, last, &lr, &lc);
            if (last >= 0 && !last_merged && ng.v[lr][lc] == e && e < FU_EXP_MAX) {
                /* it meets its twin: slides into it and is gone */
                fu_slide_t *s = &out->slide[out->nslides++];
                s->id = id;
                s->r0 = (int8_t)r; s->c0 = (int8_t)c;
                s->r1 = (int8_t)lr; s->c1 = (int8_t)lc;
                s->gone = true;
                ng.v[lr][lc] = (uint8_t)(e + 1);
                fu_merge_t *m = &out->merge[out->nmerges++];
                m->id = ng.id[lr][lc];
                m->r = (int8_t)lr;
                m->c = (int8_t)lc;
                m->exp = (uint8_t)(e + 1);
                out->points += 1u << (e + 1);
                if (e + 1 > out->top) out->top = (uint8_t)(e + 1);
                last_merged = true;
                out->moved = true;
                continue;
            }
            int tr, tc;
            line_cell(n, dir, i, t, &tr, &tc);
            ng.v[tr][tc] = e;
            ng.id[tr][tc] = id;
            if (tr != r || tc != c) {
                fu_slide_t *s = &out->slide[out->nslides++];
                s->id = id;
                s->r0 = (int8_t)r; s->c0 = (int8_t)c;
                s->r1 = (int8_t)tr; s->c1 = (int8_t)tc;
                s->gone = false;
                out->moved = true;
            }
            last = t;
            last_merged = false;
            t++;
        }
    }
    if (out->moved) *g = ng;
    return out->moved;
}

/* A new tile on an empty cell: a 2 nine times in ten, else a 4. */
bool fu_core_spawn(fu_grid_t *g, int n, int *r, int *c)
{
    int empty = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (!g->v[y][x]) empty++;
    if (!empty) return false;
    int pick = (int)(fu_rng() % (uint32_t)empty);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            if (g->v[y][x]) continue;
            if (pick-- == 0) {
                g->v[y][x] = (fu_rng() % 10u) == 0 ? 2 : 1;
                g->id[y][x] = fu_new_id();
                *r = y;
                *c = x;
                return true;
            }
        }
    return false;
}

bool fu_core_can_move(const fu_grid_t *g, int n)
{
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            uint8_t e = g->v[y][x];
            if (!e) return true;
            if (e == FU_BLOCK) continue;
            if (x + 1 < n && g->v[y][x + 1] == e) return true;
            if (y + 1 < n && g->v[y + 1][x] == e) return true;
        }
    return false;
}

int fu_core_top(const fu_grid_t *g, int n)
{
    int top = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (g->v[y][x] != FU_BLOCK && g->v[y][x] > top) top = g->v[y][x];
    return top;
}

/* Stones on random empty cells, never two side by side (a wall of two would
 * cut the board in halves too small to play). */
void fu_core_blocks(fu_grid_t *g, int n, int count)
{
    for (int k = 0; k < count; k++) {
        for (int tries = 0; tries < 200; tries++) {
            int y = (int)(fu_rng() % (uint32_t)n), x = (int)(fu_rng() % (uint32_t)n);
            if (g->v[y][x]) continue;
            bool near = false;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy, xx = x + dx;
                    if (yy >= 0 && yy < n && xx >= 0 && xx < n && g->v[yy][xx] == FU_BLOCK) near = true;
                }
            if (near) continue;
            g->v[y][x] = FU_BLOCK;
            g->id[y][x] = fu_new_id();
            break;
        }
    }
}

/* The first goal: about where a careful game gets in a few minutes. */
uint8_t fu_goal_first(int n, int mode)
{
    static const uint8_t base[4] = { 6, 8, 9, 10 };     /* 64, 256, 512, 1024 */
    int g = base[n - FU_NMIN];
    if (mode == FU_MODE_BLOCKS) g--;
    if (mode == FU_MODE_DUO && n == FU_NMAX) g--;
    return (uint8_t)g;
}

void fu_game_new(int n, int mode)
{
    if (n < FU_NMIN || n > FU_NMAX) n = 4;
    if (mode < 0 || mode >= FU_MODES) mode = FU_MODE_CLASSIC;
    memset(&fu_G, 0, sizeof fu_G);
    fu_rng_seed((uint32_t)aos_hal_uptime_ms() * 2654435761u ^ (uint32_t)lv_tick_get());
    fu_G.active = true;
    fu_G.n = (uint8_t)n;
    fu_G.mode = (uint8_t)mode;
    fu_G.goal = fu_goal_first(n, mode);
    fu_G.undo_left = mode == FU_MODE_DUO ? 0 : 2;
    fu_G.time_ms = mode == FU_MODE_TIME ? 120000 : 0;
    if (mode == FU_MODE_BLOCKS) fu_core_blocks(&fu_G.g, n, n - 2);
    int r, c;
    fu_core_spawn(&fu_G.g, n, &r, &c);
    fu_core_spawn(&fu_G.g, n, &r, &c);
}
