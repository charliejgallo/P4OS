/*
 * SOLITARIO - the solver
 *
 * A depth-first search over the same engine the player uses (so_game.c):
 * the moves it tries are so_moves(), ordered by so_move_value(), with the
 * useless ones (value < 0) left out and a move that is always right (a safe
 * card home) played without branching. A table of the states already seen
 * (so_hash(), which treats columns in another order as the same state)
 * keeps it from walking in circles.
 *
 * It is not complete - the pruning can miss a solution - and that is fine
 * for what it is for: a deal it solves IS solvable, the moves are right
 * there, and a deal it cannot solve is thrown away. The search runs in
 * slices (so_solve_run with a node budget), so the app can spread it over
 * frames while the cards are being shuffled.
 */
#pragma once

#include "so_game.h"

typedef enum {
    SO_SOLVE_RUNNING = 0,
    SO_SOLVE_SOLVED,
    SO_SOLVE_FAILED,        /* searched everything it would, no solution */
    SO_SOLVE_GAVE_UP,       /* the budget ran out                        */
} so_solve_res_t;

typedef struct so_solver_s so_solver_t;

/* tt_bits: the table of seen states has 2^tt_bits entries of 8 bytes. */
so_solver_t *so_solver_new(int tt_bits, int max_depth);
void so_solver_free(so_solver_t *s);

/* Starts on a copy of 'g' (the game itself is not touched). 'budget' is the
 * total number of states it may visit before giving up. */
void so_solver_start(so_solver_t *s, const so_game_t *g, uint32_t budget);
/* Works for at most 'nodes' more states. */
so_solve_res_t so_solver_run(so_solver_t *s, uint32_t nodes);

/* After SOLVED: the moves from the starting state to the win. */
int so_solver_path(const so_solver_t *s, const so_move_t **path);
uint32_t so_solver_nodes(const so_solver_t *s);

/* One call to the end: start and run. */
so_solve_res_t so_solve(so_solver_t *s, const so_game_t *g, uint32_t budget);
