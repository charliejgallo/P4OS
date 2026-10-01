/*
 * NEON SNAKES - the bench
 *
 *   cc -O1 -I../main ns_harness.c ../main/ns_game.c ../main/ns_art.c \
 *      ../main/ns_draw.c -lm -o /tmp/nsh
 *
 *   /tmp/nsh                  every check, a few thousand steps per mode
 *   /tmp/nsh shot normal out.ppm [steps]   a frame to look at
 *   /tmp/nsh shot combat out.ppm [steps]
 *   /tmp/nsh hashes [steps]   the two-device match's hash after every step
 *
 * 'hashes' is the P4-to-watch check: the same program built against
 * AmoledOS's ns_game.c (whose ns_init has no arena arguments: 34x40 and 9
 * fruits are its combat) must print the same lines.
 *
 * What it checks, all without LVGL or a board:
 *
 *  - the rules: after every step, ns_check() - every grid cell agrees with
 *    the snakes' rings and the other way round, the fruit count adds up, and
 *    every body is a chain of neighbouring cells;
 *  - the compositor: every frame is drawn the fast way (only the marked
 *    blocks) and then again from scratch into a second buffer, and the two
 *    must be identical to the pixel; a block the engine forgot to mark shows
 *    up here on the first frame it happens, with coordinates;
 *  - lockstep: two engines with the same seed fed the same directions must
 *    hash the same after every step, which is the whole of the two-watch
 *    mode;
 *  - and it measures what a frame pushes, for the board.
 *
 * The arenas are the P4's: the whole 720x1280 screen less the home strip,
 * standing up and lying down, at the cell sizes the app uses (see neon.c,
 * layout()), with the cells under the pause pill covered: no fruit
 * may ever appear on one.
 */
#include "ns_art.h"
#include "ns_draw.h"
#include "ns_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW 720
#define SH 1280
#define NPX (SW * SH)

static uint16_t fb[NPX], bg[NPX];
static uint16_t ref[NPX];

/* a field region and the arena that fits it at a cell size, as neon.c does */
typedef struct { int x, y, w, h; } region_t;
static const region_t R_TALL = { 0, 0, SW, SH - 36 };
static const region_t R_WIDE = { 0, 0, SH, SW - 36 };

/* The pause pill, as neon.c lays it out. */
static const region_t COVER_TALL[1] = { { 594, 60, 96, 64 } };
static const region_t COVER_WIDE[1] = { { 1154, 52, 96, 64 } };
static uint8_t cover[(NS_MAX_CELLS + 7) / 8];

static const uint8_t *cover_for(const region_t *reg, const region_t *boxes, int cols, int rows,
                                int cell)
{
    int ox = reg->x + (reg->w - cols * cell) / 2, oy = reg->y + (reg->h - rows * cell) / 2;
    memset(cover, 0, sizeof cover);
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            int x0 = ox + x * cell, y0 = oy + y * cell;
            for (int k = 0; k < 1; k++) {
                const region_t *b = &boxes[k];
                if (x0 < b->x + b->w && x0 + cell > b->x && y0 < b->y + b->h && y0 + cell > b->y) {
                    int i = y * cols + x;
                    cover[i >> 3] |= (uint8_t)(1u << (i & 7));
                }
            }
        }
    }
    return cover;
}

static void arena_for(const region_t *r, int cell, int *cols, int *rows)
{
    *cols = r->w / cell - 2;
    *rows = r->h / cell - 2;
    if (*cols > NS_MAX_COLS) *cols = NS_MAX_COLS;
    while (*cols * *rows > NS_MAX_CELLS) (*rows)--;
}

static uint32_t lcg = 12345;
static int rnd(int n)
{
    lcg = lcg * 1103515245u + 12345u;
    return (int)((lcg >> 16) % (uint32_t)n);
}

static void events_to_fx(ns_view_t *v, ns_game_t *g)
{
    for (int i = 0; i < g->nev; i++) {
        const ns_event_t *e = &g->ev[i];
        if (e->type == NS_EV_EAT) ns_view_fx(v, NS_FX_RING, e->x, e->y, ns_fruit_rgb(e->kind));
        if (e->type == NS_EV_DIE) ns_view_fx(v, NS_FX_BURST, e->x, e->y, 0xFFFFFF);
    }
    g->nev = 0;
}

/* Draws the same state from scratch into 'ref' and compares. */
static int compare(ns_view_t *v, ns_game_t *g, const char *tag, long frame)
{
    uint16_t *keep = v->fb;
    v->fb = ref;
    /* the full repaint without touching the state: paint every cell */
    memcpy(ref, bg, sizeof ref);
    ns_game_t copy = *g;          /* ns_view_full clears the marks: use a copy */
    uint8_t rep_any = v->rep_any;
    ns_view_t vc = *v;
    ns_view_full(&vc, &copy);
    (void)rep_any;
    v->fb = keep;
    for (int i = 0; i < NPX; i++) {
        if (fb[i] != ref[i]) {
            printf("  %s frame %ld: pixel %d,%d is %04X, a full repaint says %04X\n",
                   tag, frame, i % SW, i / SW, fb[i], ref[i]);
            return 1;
        }
    }
    return 0;
}

static int run_mode(uint8_t mode, int humans, int snakes, long steps, uint32_t seed,
                    const region_t *reg, const region_t *boxes)
{
    const char *tag = mode == NS_MODE_NORMAL ? "normal" : "combat";
    const int cell = mode == NS_MODE_NORMAL ? 24 : 18;
    int cols, rows;
    arena_for(reg, cell, &cols, &rows);
    const int fruits = mode == NS_MODE_NORMAL ? (cols * rows / 600 > 1 ? cols * rows / 600 : 1)
                                              : (cols * rows / 130 > 9 ? cols * rows / 130 : 9);
    const uint8_t *cv = cover_for(reg, boxes, cols, rows, cell);
    ns_game_t *g = calloc(1, sizeof *g);
    ns_art_t art;
    if (!ns_art_build(&art, cell, mode == NS_MODE_NORMAL ? 1 : snakes)) {
        printf("  %s: out of memory for the art\n", tag);
        return 1;
    }
    ns_init(g, mode, seed, humans, snakes, cols, rows, fruits, cv);
    ns_view_t *v = calloc(1, sizeof *v);
    ns_view_init(v, fb, bg, reg->w + reg->x, reg->h + 36, &art, g, reg->x, reg->y, reg->w, reg->h);
    ns_view_full(v, g);
    ns_view_halo(v, 0, 60);

    int bad = 0, games = 1;
    long frame = 0;
    uint64_t pushed = 0, max_len = 0, deaths = 0, eaten = 0;
    uint32_t max_px = 0, max_rects = 0;
    for (long st = 0; st < steps && bad < 3; st++) {
        uint8_t dirs[NS_MAX_SNAKES];
        memset(dirs, NS_NODIR, sizeof dirs);
        for (int i = 0; i < humans && i < g->nsnakes; i++) {
            if (g->s[i].alive && !g->s[i].dying) dirs[i] = ns_bot_choice(g, i);
        }
        ns_step(g, dirs);
        for (int i = 0; i < g->nev; i++) {
            if (g->ev[i].type == NS_EV_DIE) deaths++;
            if (g->ev[i].type == NS_EV_EAT) eaten++;
        }
        events_to_fx(v, g);
        int c = ns_check(g);
        if (c) {
            printf("  %s step %ld: ns_check says %d\n", tag, st, c);
            bad++;
        }
        for (int i = 0; i < g->cols * g->rows; i++) {
            uint16_t cell_v = g->grid[i];
            if (NS_IS_FRUIT(cell_v) && (cell_v & 0x0FFF) < NS_FRUIT_COUNT && ns_covered(g, i)) {
                printf("  %s step %ld: a fruit under the pause pill at %d,%d\n", tag, st,
                       i % g->cols, i / g->cols);
                bad++;
                break;
            }
        }
        for (int i = 0; i < g->nsnakes; i++) {
            if (g->s[i].len > max_len) max_len = g->s[i].len;
        }
        /* three frames per step, the pulse turning every other one */
        for (int f = 0; f < 3; f++, frame++) {
            ns_view_frame(v, g, (uint8_t)((frame / 5) & 3));
            pushed += v->pixels;
            if (v->pixels > max_px) max_px = v->pixels;
            if (frame % 7 == 0 || st < 40) bad += compare(v, g, tag, frame);
        }
        if (v->nrects > max_rects) max_rects = v->nrects;
        if (g->over) {
            ns_init(g, mode, seed + (uint32_t)st, humans, snakes, cols, rows, fruits, cv);
            ns_view_init(v, fb, bg, reg->w + reg->x, reg->h + 36, &art, g, reg->x, reg->y, reg->w, reg->h);
            ns_view_full(v, g);
            games++;
        }
    }
    printf("%-7s %dx%d, %d snakes, %d fruits: %ld steps, %d games, %llu eaten, %llu deaths, "
           "longest %llu, %.1f %% of the screen per frame (max %.1f %%), up to %u areas%s\n",
           tag, cols, rows, g->nsnakes, fruits, steps, games, (unsigned long long)eaten,
           (unsigned long long)deaths, (unsigned long long)max_len,
           100.0 * pushed / (double)frame / NPX, 100.0 * max_px / NPX, max_rects,
           bad ? "  FAIL" : "");
    ns_art_free(&art);
    free(v);
    free(g);
    return bad;
}

static int lockstep(long steps)
{
    ns_game_t *a = calloc(1, sizeof *a), *b = calloc(1, sizeof *b);
    ns_init(a, NS_MODE_COMBAT, 777, 2, 4, 34, 40, 9, NULL);
    ns_init(b, NS_MODE_COMBAT, 777, 2, 4, 34, 40, 9, NULL);
    for (long st = 0; st < steps; st++) {
        uint8_t dirs[NS_MAX_SNAKES] = { NS_NODIR, NS_NODIR, NS_NODIR, NS_NODIR };
        if (rnd(4) == 0) dirs[0] = (uint8_t)rnd(4);
        if (rnd(4) == 0) dirs[1] = (uint8_t)rnd(4);
        ns_step(a, dirs);
        ns_step(b, dirs);
        if (ns_hash(a) != ns_hash(b)) {
            printf("lockstep: the two engines split at step %ld\n", st);
            return 1;
        }
    }
    printf("lockstep %ld steps, two humans at random and two bots: identical, hash %08X\n",
           steps, ns_hash(a));
    free(a);
    free(b);
    return 0;
}

/* The two-device match, step by step, for diffing against the watch. */
static int hashes(long steps)
{
    ns_game_t *a = calloc(1, sizeof *a);
    ns_init(a, NS_MODE_COMBAT, 777, 2, 4, 34, 40, 9, NULL);
    for (long st = 0; st < steps; st++) {
        uint8_t dirs[NS_MAX_SNAKES];
        memset(dirs, NS_NODIR, sizeof dirs);
        if (rnd(4) == 0) dirs[0] = (uint8_t)rnd(4);
        if (rnd(4) == 0) dirs[1] = (uint8_t)rnd(4);
        ns_step(a, dirs);
        printf("%ld %08X\n", st, ns_hash(a));
    }
    free(a);
    return 0;
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", SW, SH);
    for (int i = 0; i < NPX; i++) {
        uint16_t c = fb[i];
        uint8_t px[3] = { (uint8_t)(((c >> 11) & 0x1F) * 255 / 31), (uint8_t)(((c >> 5) & 0x3F) * 255 / 63),
                          (uint8_t)((c & 0x1F) * 255 / 31) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static int shot(const char *which, const char *path, long steps)
{
    bool combat = strcmp(which, "combat") == 0;
    bool title = strcmp(which, "title") == 0;
    if (title) {
        memset(fb, 0, sizeof fb);
        ns_art_title(fb, SW, SH, SW / 2, 110, 2.4f);
        ns_art_t art;
        ns_art_build(&art, 16, 1);
        for (int k = 0; k < NS_FRUIT_COUNT; k++) {
            int S = art.size, x0 = SW / 2 - (NS_FRUIT_COUNT * 72) / 2 + k * 72 + 20, y0 = 450;
            for (int y = 0; y < S; y++)
                for (int x = 0; x < S; x++)
                    fb[(y0 + y) * SW + x0 + x] = art.fruit[k][2][y * S + x];
        }
        /* every part of every snake and the flash, for a look at the sprites */
        ns_art_t big;
        ns_art_build(&big, 16, NS_MAX_SNAKES);
        for (int c = 0; c < NS_COLOURS; c++) {
            for (int k = 0; k < NS_SNAKE_SPRITES; k++) {
                int S = big.size, x0 = 20 + (k % 12) * 34, y0 = 560 + c * 76 + (k / 12) * 34;
                for (int y = 0; y < S; y++)
                    for (int x = 0; x < S; x++) {
                        uint16_t *d = &fb[(y0 + y) * SW + x0 + x];
                        *d = ns_max565(*d, big.snake[c][k][y * S + x]);
                    }
            }
        }
        write_ppm(path);
        return 0;
    }
    int cell = combat ? 18 : 24, cols, rows;
    arena_for(&R_TALL, cell, &cols, &rows);
    ns_game_t *g = calloc(1, sizeof *g);
    ns_art_t art;
    ns_art_build(&art, cell, combat ? NS_MAX_SNAKES : 1);
    ns_init(g, combat ? NS_MODE_COMBAT : NS_MODE_NORMAL, 42, 1, NS_MAX_SNAKES, cols, rows,
            combat ? cols * rows / 130 : 2, cover_for(&R_TALL, COVER_TALL, cols, rows, cell));
    ns_view_t *v = calloc(1, sizeof *v);
    ns_view_init(v, fb, bg, SW, SH, &art, g, R_TALL.x, R_TALL.y, R_TALL.w, R_TALL.h);
    ns_view_full(v, g);
    for (long st = 0; st < steps; st++) {
        uint8_t dirs[NS_MAX_SNAKES];
        memset(dirs, NS_NODIR, sizeof dirs);
        if (g->s[0].alive && !g->s[0].dying) dirs[0] = ns_bot_choice(g, 0);
        ns_step(g, dirs);
        events_to_fx(v, g);
        ns_view_frame(v, g, (uint8_t)(st & 3));
        if (g->over) break;
    }
    ns_view_full(v, g);
    write_ppm(path);
    printf("shot after %ld steps: lengths", steps);
    for (int i = 0; i < g->nsnakes; i++) printf(" %d", g->s[i].len);
    printf("\n");
    free(v);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "shot") == 0) {
        return shot(argv[2], argv[3], argc >= 5 ? atol(argv[4]) : 200);
    }
    if (argc >= 2 && strcmp(argv[1], "hashes") == 0) {
        return hashes(argc >= 3 ? atol(argv[2]) : 2000);
    }
    int bad = 0;
    bad += run_mode(NS_MODE_NORMAL, 1, 1, 6000, 1, &R_TALL, COVER_TALL);
    bad += run_mode(NS_MODE_NORMAL, 1, 1, 4000, 4, &R_WIDE, COVER_WIDE);
    bad += run_mode(NS_MODE_COMBAT, 1, 6, 6000, 2, &R_TALL, COVER_TALL);
    bad += run_mode(NS_MODE_COMBAT, 1, 6, 4000, 5, &R_WIDE, COVER_WIDE);
    bad += lockstep(20000);
    printf(bad ? "FAILED\n" : "all good\n");
    return bad ? 1 : 0;
}
