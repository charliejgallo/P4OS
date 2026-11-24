/*
 * VITRAL - test bench for the rules, on the Mac:
 *
 *   cc -O2 -I apps/vitral/main apps/vitral/tools/vt_harness.c \
 *      apps/vitral/main/vt_game.c -o /tmp/vt_harness && /tmp/vt_harness
 *
 * Checks the pieces (the rotations go round and keep their cells), the
 * puzzle windows (every column one run, so each cell can be reached falling
 * straight down), that the generator tiles every level and how long it
 * took, that a level replayed with its own solution is won, and plays a few
 * thousand pieces of each mode with a dumb bot to see nothing breaks.
 * --seeds prints the table of tries for PZ_TRY in vt_game.c.
 */
#include "vt_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void check_shapes(void)
{
    vt_game_t *g = calloc(1, sizeof *g);
    vt_init(g, VT_MODE_MARATHON, 1, 1);
    for (int t = 0; t < PC_COUNT; t++) {
        const vt_shape_t *s = &vt_shapes[t];
        CHECK(s->n == 3 || s->n == 5, "piece %d has %d cells", t, s->n);
        for (int r = 0; r < 4; r++) {
            for (int i = 0; i < s->n; i++) {
                CHECK(s->c[r][i].x >= 0 && s->c[r][i].x < 5 && s->c[r][i].y >= 0 && s->c[r][i].y < 5,
                      "piece %d rot %d leaves the box", t, r);
                for (int j = 0; j < i; j++)
                    CHECK(s->c[r][i].x != s->c[r][j].x || s->c[r][i].y != s->c[r][j].y,
                          "piece %d rot %d repeats a cell", t, r);
            }
        }
    }
    free(g);
}

static void print_board(const vt_game_t *g)
{
    for (int y = VT_HIDDEN; y < VT_ROWS; y++) {
        for (int x = 0; x < g->cols; x++) {
            int c = g->col[y][x];
            putchar(c == VT_STONE ? '=' : c == VT_EMPTY ? (g->mask[y][x] ? '.' : ' ') : '0' + c);
        }
        putchar('\n');
    }
}

/* replays a puzzle's own solution: the generator's placements, in order */
static bool solve_like_generator(vt_game_t *g)
{
    /* the generator filled lowest-leftmost first; replay the same search by
     * dropping each piece of the sequence at the first spot that keeps the
     * window fillable is too much for a bench: instead, place each piece
     * where it covers the lowest-leftmost empty cell, trying rotations and
     * offsets, with a hard drop from the top, as a player would. */
    for (int guard = 0; guard < 200 && g->state == VT_PLAY; guard++) {
        int ex = -1, ey = -1;
        for (int y = VT_ROWS - 1; y >= 0 && ey < 0; y--)
            for (int x = 0; x < g->cols; x++)
                if (g->mask[y][x] && g->col[y][x] == VT_EMPTY) { ex = x; ey = y; break; }
        if (ey < 0) break;
        const vt_shape_t *s = &vt_shapes[g->cur.type];
        bool placed = false;
        for (int r = 0; r < 4 && !placed; r++) {
            for (int k = 0; k < s->n && !placed; k++) {
                int ox = ex - s->c[r][k].x, oy = ey - s->c[r][k].y;
                /* falling straight from the spawn height must end exactly there */
                if (!vt_fits(g, g->cur.type, r, ox, oy)) continue;
                int y = oy;
                while (vt_fits(g, g->cur.type, r, ox, y + 1)) y++;
                if (y != oy) continue;
                bool in = true;
                for (int i = 0; i < s->n; i++) if (!g->mask[oy + s->c[r][i].y][ox + s->c[r][i].x]) in = false;
                if (!in) continue;
                /* is the column clear above? drop from the top */
                int top = oy;
                while (top > 0 && vt_fits(g, g->cur.type, r, ox, top - 1)) top--;
                if (top > 1) continue;
                g->cur.rot = (uint8_t)r;
                g->cur.x = (int8_t)ox;
                g->cur.y = (int8_t)top;
                vt_hard(g);
                placed = true;
            }
        }
        if (!placed) return false;
    }
    return g->state == VT_WON;
}

static void check_puzzles(bool seeds)
{
    vt_game_t *g = calloc(1, sizeof *g);
    g->undo = malloc(vt_pz_undo_size() * VT_PZ_MAX_SEQ);
    double worst = 0;
    if (seeds) printf("static const uint8_t PZ_TRY[VT_PZ_LEVELS] = {");
    for (int lv = 0; lv < VT_PZ_LEVELS; lv++) {
        vt_init(g, VT_MODE_PUZZLE, 1, 1);
        /* the window's columns are each one run */
        double t0 = now_ms();
        bool ok = vt_pz_build(g, lv);
        double dt = now_ms() - t0;
        if (dt > worst) worst = dt;
        CHECK(ok, "level %d (%s) did not tile", lv, vt_pz_name(lv));
        if (!ok) continue;
        for (int x = 0; x < g->cols; x++) {
            int runs = 0;
            for (int y = 0; y < VT_ROWS; y++)
                if (g->mask[y][x] && (y == 0 || !g->mask[y - 1][x])) runs++;
            CHECK(runs <= 1, "level %d column %d has %d runs", lv, x, runs);
        }
        int cells = 0;
        for (int i = 0; i < g->pz_n; i++) cells += vt_shapes[g->pz_seq[i].type].n;
        CHECK(cells == g->pz_cells, "level %d: pieces %d cells vs window %d", lv, cells, g->pz_cells);
        if (seeds) printf("%s%d", lv ? ", " : " ", g->pz_try);
        else {
            int five = 0;
            for (int i = 0; i < g->pz_n; i++) five += vt_shapes[g->pz_seq[i].type].n == 5;
            printf("level %2d %-9s %2d cells %2d pieces (%2d of five) try %3d  %.1f ms\n", lv + 1,
                   vt_pz_name(lv), g->pz_cells, g->pz_n, five, g->pz_try, dt);
        }
        /* the generator's own solution, played as a player would: turned
         * at the top, moved over and dropped */
        bool undone = false;
        for (int i = 0; i < g->pz_n && g->state == VT_PLAY; i++) {
            const vt_pz_place_t *p = &g->pz_sol[i];
            CHECK(vt_fits(g, g->cur.type, p->rot, p->x, g->cur.y), "level %d piece %d: no room at the top", lv, i);
            g->cur.rot = p->rot;
            g->cur.x = p->x;
            vt_hard(g);
            CHECK(!(g->ev & EV_REJECT), "level %d piece %d rejected", lv, i);
            vt_events_clear(g);
            if (i == 2 && !undone) {
                undone = true;
                /* undo goes back a piece, and comes again the same */
                vt_pz_undo(g);
                CHECK(g->pz_i == 2, "level %d: undo to %d", lv, g->pz_i);
                i--;
            }
        }
        CHECK(g->state == VT_WON, "level %d: its own solution did not win it", lv);
        (void)solve_like_generator;
    }
    if (seeds) printf(" };\n");
    else printf("puzzles: slowest build %.1f ms\n", worst);
    free(g->undo);
    free(g);
}

/* a bot that picks the placement with the lowest stack, holes counted */
static int eval_board(const vt_game_t *g)
{
    int score = 0, prev = -1;
    for (int x = 0; x < g->cols; x++) {
        int y = 0;
        while (y < VT_ROWS && g->col[y][x] == VT_EMPTY) y++;
        int h = VT_ROWS - y;
        score += h * 5;
        if (h > 14) score += (h - 14) * 40;
        if (prev >= 0) score += (h > prev ? h - prev : prev - h) * 4;
        prev = h;
        for (int yy = y + 1; yy < VT_ROWS; yy++) if (g->col[yy][x] == VT_EMPTY) score += 35;
    }
    return score;
}

static void bot_move(vt_game_t *g)
{
    int best = 1 << 30, br = 0, bx = g->cur.x;
    for (int r = 0; r < 4; r++) {
        for (int x = -3; x < g->cols; x++) {
            if (!vt_fits(g, g->cur.type, r, x, g->cur.y)) continue;
            int y = g->cur.y;
            while (vt_fits(g, g->cur.type, r, x, y + 1)) y++;
            vt_game_t tmp = *g;
            const vt_shape_t *s = &vt_shapes[g->cur.type];
            for (int i = 0; i < s->n; i++) {
                int cy = y + s->c[r][i].y;
                if (cy >= 0) tmp.col[cy][x + s->c[r][i].x] = 1;
            }
            /* full rows go */
            int full = 0;
            for (int yy = 0; yy < VT_ROWS; yy++) {
                bool f = true;
                for (int xx = 0; xx < g->cols; xx++) if (!tmp.col[yy][xx]) f = false;
                if (!f) continue;
                full++;
                for (int k = yy; k > 0; k--) memcpy(tmp.col[k], tmp.col[k - 1], sizeof tmp.col[0]);
                memset(tmp.col[0], 0, sizeof tmp.col[0]);
            }
            int e = eval_board(&tmp) - full * 40;
            if (e < best) { best = e; br = r; bx = x; }
        }
    }
    while (g->cur.rot != br && vt_rotate(g, 1)) {}
    for (int k = 0; k < 14 && g->cur.x != bx; k++) vt_move(g, bx > g->cur.x ? 1 : -1);
    vt_hard(g);
}

static void check_play(vt_mode_t mode, uint32_t seed)
{
    vt_game_t *a = calloc(1, sizeof *a), *b = calloc(1, sizeof *b);
    vt_init(a, mode, seed, 1);
    vt_init(b, mode, seed, 1);
    int pieces = 0, vit = 0, clears = 0, frames = 0;
    while (a->state != VT_OVER && pieces < 3000 && frames < 2000000) {
        if (a->has_cur && a->state == VT_PLAY && (frames % 7) == 0) {
            bot_move(a);
            pieces++;
        }
        vt_tick(a, 17);
        if (a->ev & EV_VITRAL) vit++;
        if (a->ev & EV_CLEAR) clears++;
        vt_events_clear(a);
        if (mode == VT_MODE_VERSUS) {
            if (b->has_cur && b->state == VT_PLAY && (frames % 11) == 0) bot_move(b);
            vt_tick(b, 17);
            vt_events_clear(b);
            if (a->garbage_out) { vt_add_garbage(b, a->garbage_out); a->garbage_out = 0; }
            if (b->garbage_out) { vt_add_garbage(a, b->garbage_out); b->garbage_out = 0; }
            if (b->state == VT_OVER) break;
        }
        frames++;
    }
    printf("mode %d seed %u: %d pieces, %u lines, %d clears, %d vitrals, level %d, score %u, %s\n",
           mode, seed, pieces, a->lines, clears, vit, a->level, a->score,
           a->state == VT_OVER ? "over" : "still going");
    /* the board never holds a cell it should not */
    for (int y = 0; y < VT_ROWS; y++)
        for (int x = 0; x < a->cols; x++)
            CHECK(a->col[y][x] <= VT_STONE, "bad cell");
    free(a);
    free(b);
}

/* a row of one colour scores three times and takes the row under it */
static void check_vitral(void)
{
    vt_game_t *g = calloc(1, sizeof *g);
    vt_init(g, VT_MODE_MARATHON, 3, 1);
    int b = VT_ROWS - 1, a = VT_ROWS - 2;
    for (int x = 0; x < 12; x++) {
        g->col[b][x] = x == 5 ? VT_EMPTY : VT_EMERALD;     /* not full: taken by the vitral */
        g->col[a][x] = x < 9 ? (x == 4 ? VT_CLEAR : VT_RUBY) : VT_EMPTY;
        g->pid[b][x] = g->pid[a][x] = (uint16_t)(100 + x);
    }
    /* a ruby I3 lying in the gap of the upper row */
    g->cur.type = PC_I3;
    g->cur.rot = 0;
    g->cur.color = VT_RUBY;
    g->cur.x = 9 - 1;
    g->cur.y = 2;
    vt_events_clear(g);
    vt_hard(g);
    CHECK(g->ev & EV_VITRAL, "no vitral");
    CHECK(g->fx_rows_n == 2, "vitral took %d rows", g->fx_rows_n);
    CHECK(g->fx_points == 600, "vitral points %u", g->fx_points);
    for (int i = 0; i < 20; i++) vt_tick(g, 17);
    int left = 0;
    for (int x = 0; x < 12; x++) left += g->col[b][x] != VT_EMPTY;
    CHECK(left == 0, "the bottom is not empty after the vitral (%d)", left);
    free(g);
}

int main(int argc, char **argv)
{
    bool seeds = argc > 1 && !strcmp(argv[1], "--seeds");
    check_shapes();
    check_vitral();
    check_puzzles(seeds);
    if (seeds) return fails ? 1 : 0;
    for (uint32_t s = 1; s <= 3; s++) check_play(VT_MODE_MARATHON, s * 977);
    check_play(VT_MODE_TIME, 5);
    check_play(VT_MODE_VERSUS, 7);
    printf(fails ? "%d FAILED\n" : "all good\n", fails);
    return fails ? 1 : 0;
}
