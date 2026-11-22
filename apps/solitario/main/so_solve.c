/*
 * SOLITARIO - the solver. See so_solve.h.
 *
 * Iterative, not recursive: a solution can be hundreds of moves deep, and
 * the LVGL task's stack is no place for that. Every level of the search is
 * a frame with its moves sorted best first and the next one to try.
 */
#include "so_solve.h"

#include <stdlib.h>
#include <string.h>

#define FRAME_MOVES 48

typedef struct {
    so_move_t mv[FRAME_MOVES];
    uint8_t   n, next;
} frame_t;

struct so_solver_s {
    so_game_t g;                /* the working copy                    */
    uint64_t *tt;
    uint32_t  tt_mask, tt_used;
    frame_t  *fr;
    int       depth, max_depth;
    uint32_t  nodes, budget;
    so_solve_res_t res;
    so_move_t *path;
    int       npath;
};

so_solver_t *so_solver_new(int tt_bits, int max_depth)
{
    so_solver_t *s = (so_solver_t *)malloc(sizeof *s);
    if (!s) {
        return NULL;
    }
    memset(s, 0, sizeof *s);
    s->tt_mask = (1u << tt_bits) - 1;
    s->tt = (uint64_t *)malloc(((size_t)s->tt_mask + 1) * sizeof(uint64_t));
    s->max_depth = max_depth;
    s->fr = (frame_t *)malloc((size_t)max_depth * sizeof(frame_t));
    s->path = (so_move_t *)malloc((size_t)max_depth * sizeof(so_move_t));
    if (!s->tt || !s->fr || !s->path) {
        so_solver_free(s);
        return NULL;
    }
    return s;
}

void so_solver_free(so_solver_t *s)
{
    if (!s) {
        return;
    }
    so_free(&s->g);
    free(s->tt);
    free(s->fr);
    free(s->path);
    free(s);
}

/* true if it was already there */
static bool tt_seen(so_solver_t *s, uint64_t h)
{
    if (!h) {
        h = 1;
    }
    uint32_t i = (uint32_t)h & s->tt_mask;
    for (uint32_t k = 0; k < 64; k++) {
        uint64_t e = s->tt[i];
        if (e == h) {
            return true;
        }
        if (!e) {
            if (s->tt_used < s->tt_mask - (s->tt_mask >> 3)) {
                s->tt[i] = h;
                s->tt_used++;
            }
            return false;
        }
        i = (i + 1) & s->tt_mask;
    }
    return false;
}

/* The moves worth trying from here, best first. */
static void gen(so_solver_t *s, frame_t *f)
{
    so_move_t l[160];
    int       v[160];
    int n = so_moves(&s->g, l, 160);
    int k = 0;
    for (int i = 0; i < n; i++) {
        int val = so_move_value(&s->g, &l[i]);
        if (val >= 100) {
            /* always right: no need to look at anything else */
            f->mv[0] = l[i];
            f->n = 1;
            f->next = 0;
            return;
        }
        if (val < 0) {
            continue;
        }
        l[k] = l[i];
        v[k] = val;
        k++;
    }
    /* insertion sort, best first: the lists are short */
    for (int i = 1; i < k; i++) {
        so_move_t m = l[i];
        int       x = v[i];
        int       j = i - 1;
        while (j >= 0 && v[j] < x) {
            l[j + 1] = l[j];
            v[j + 1] = v[j];
            j--;
        }
        l[j + 1] = m;
        v[j + 1] = x;
    }
    if (k > FRAME_MOVES) {
        k = FRAME_MOVES;
    }
    memcpy(f->mv, l, (size_t)k * sizeof(so_move_t));
    f->n = (uint8_t)k;
    f->next = 0;
}

void so_solver_start(so_solver_t *s, const so_game_t *g, uint32_t budget)
{
    so_free(&s->g);
    memcpy(&s->g, g, sizeof *g);
    s->g.ops = NULL;
    s->g.recs = NULL;
    s->g.nops = s->g.ops_cap = s->g.nrecs = s->g.recs_cap = 0;
    memset(s->tt, 0, ((size_t)s->tt_mask + 1) * sizeof(uint64_t));
    s->tt_used = 0;
    s->nodes = 0;
    s->budget = budget;
    s->npath = 0;
    s->depth = 0;
    s->res = SO_SOLVE_RUNNING;
    if (so_won(&s->g)) {
        s->res = SO_SOLVE_SOLVED;
        return;
    }
    tt_seen(s, so_hash(&s->g));
    gen(s, &s->fr[0]);
    s->depth = 1;
}

so_solve_res_t so_solver_run(so_solver_t *s, uint32_t nodes)
{
    uint32_t stop = s->nodes + nodes;
    while (s->res == SO_SOLVE_RUNNING) {
        if (s->depth <= 0) {
            s->res = SO_SOLVE_FAILED;
            break;
        }
        if (s->nodes >= s->budget) {
            s->res = SO_SOLVE_GAVE_UP;
            break;
        }
        if (s->nodes >= stop) {
            break;
        }
        frame_t *f = &s->fr[s->depth - 1];
        if (f->next >= f->n) {
            s->depth--;
            if (s->depth > 0) {
                so_undo(&s->g);
            }
            continue;
        }
        so_move_t m = f->mv[f->next++];
        so_apply(&s->g, &m);
        s->nodes++;
        if (s->g.oom) {
            s->res = SO_SOLVE_GAVE_UP;
            break;
        }
        if (so_won(&s->g)) {
            s->npath = 0;
            for (int d = 0; d < s->depth; d++) {
                s->path[s->npath++] = s->fr[d].mv[s->fr[d].next - 1];
            }
            s->res = SO_SOLVE_SOLVED;
            break;
        }
        if (tt_seen(s, so_hash(&s->g)) || s->depth >= s->max_depth) {
            so_undo(&s->g);
            continue;
        }
        gen(s, &s->fr[s->depth]);
        s->depth++;
    }
    return s->res;
}

int so_solver_path(const so_solver_t *s, const so_move_t **path)
{
    if (path) {
        *path = s->path;
    }
    return s->res == SO_SOLVE_SOLVED ? s->npath : 0;
}

uint32_t so_solver_nodes(const so_solver_t *s)
{
    return s->nodes;
}

so_solve_res_t so_solve(so_solver_t *s, const so_game_t *g, uint32_t budget)
{
    so_solver_start(s, g, budget);
    return so_solver_run(s, budget + 1);
}
