/*
 * SOLITARIO - the bench. Plain C, no LVGL: the engine, the solver and the
 * card art, on the Mac.
 *
 *   cc -O2 -I apps/solitario/main apps/solitario/tools/so_harness.c \
 *      apps/solitario/main/so_game.c apps/solitario/main/so_solve.c \
 *      apps/solitario/main/so_art.c apps/solitario/main/so_court.c -lm -o /tmp/soh
 *
 *   /tmp/soh fuzz 2000          random play on every game, with undo checked
 *   /tmp/soh solve 200 [mode] [budget]
 *                               how many deals the solver wins, and how fast
 *   /tmp/soh seeds 1000 out.c [budget]
 *                               the table of solvable seeds (so_seeds.c)
 *   /tmp/soh sheet out.ppm [w]  the deck, backs and felts on a sheet
 *   /tmp/soh time [w]           what drawing each thing costs here
 */
#include "so_game.h"
#include "so_solve.h"
#ifndef SOH_NO_ART
#include "so_art.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int fails;
#define CHECK(c, ...)                                                          \
    do {                                                                       \
        if (!(c)) {                                                            \
            fails++;                                                           \
            if (fails < 20) {                                                  \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
                printf(__VA_ARGS__);                                           \
                printf("\n");                                                  \
            }                                                                  \
        }                                                                      \
    } while (0)

/* every card exactly once, nothing overflowing */
static void check_cards(const so_game_t *g)
{
    int seen[SO_MAXCARDS] = { 0 };
    for (int p = 0; p < g->npiles; p++) {
        for (int i = 0; i < g->pn[p]; i++) {
            seen[g->pc[p][i]]++;
        }
    }
    for (int c = 0; c < g->ncards; c++) {
        CHECK(seen[c] == 1, "mode %d seed %u: card %d seen %d times", g->mode, g->seed, c, seen[c]);
    }
}

static void fuzz(int hands)
{
    so_game_t g;
    memset(&g, 0, sizeof g);
    so_opts_t o = { SO_SC_STANDARD, false };
    uint32_t r = 12345;
    long total = 0;
    for (int mode = 0; mode < SO_NMODES; mode++) {
        int won = 0;
        for (int h = 0; h < hands; h++) {
            o.limited = (h & 1) != 0;
            o.scoring = (uint8_t)(h % SO_NSCORING);
            so_new(&g, mode, (uint32_t)h + 1, &o);
            check_cards(&g);
            for (int step = 0; step < 400; step++) {
                so_move_t l[256];
                int n = so_moves(&g, l, 256);
                if (!n || so_won(&g)) {
                    break;
                }
                /* mostly good moves, some random ones */
                so_move_t m;
                if (so_rand(&r) % 4 && so_hint_quick(&g, &m)) {
                } else {
                    m = l[so_rand(&r) % (uint32_t)n];
                }
                CHECK(so_legal(&g, &m), "mode %d: hint not legal", mode);
                uint64_t h0 = so_hash(&g);
                int32_t s0 = g.score;
                so_apply(&g, &m);
                total++;
                check_cards(&g);
                if (so_rand(&r) % 7 == 0) {
                    so_undo(&g);
                    CHECK(so_hash(&g) == h0 && g.score == s0, "mode %d: undo did not restore", mode);
                    so_apply(&g, &m);
                }
                if (so_can_autocomplete(&g)) {
                    so_move_t a;
                    int k = 0;
                    while (!so_won(&g) && so_auto_step(&g, &a) && k++ < 200) {
                        so_apply(&g, &a);
                    }
                    CHECK(so_won(&g), "mode %d seed %u: autocomplete stopped short", mode, g.seed);
                }
            }
            won += so_won(&g);
            /* everything undone gives the deal back */
            so_game_t f;
            memset(&f, 0, sizeof f);
            so_new(&f, mode, g.seed, &o);
            while (so_undo(&g)) {
            }
            CHECK(so_hash(&g) == so_hash(&f), "mode %d: full undo differs from the deal", mode);
            so_free(&f);
        }
        printf("%-18s %d hands, %d won by the greedy bot\n", so_mode_name[mode], hands, won);
    }
    so_free(&g);
    printf("%ld moves, %d failures\n", total, fails);
}

/* replays a solution on a fresh deal: every move legal, and it wins */
static bool replay(int mode, uint32_t seed, const so_opts_t *o, const so_move_t *path, int n)
{
    so_game_t g;
    memset(&g, 0, sizeof g);
    so_new(&g, mode, seed, o);
    for (int i = 0; i < n; i++) {
        if (!so_legal(&g, &path[i])) {
            so_free(&g);
            return false;
        }
        so_apply(&g, &path[i]);
    }
    bool w = so_won(&g);
    so_free(&g);
    return w;
}

static so_opts_t mode_opts(int mode)
{
    /* the stricter handicap: a deal won with three passes is won with more */
    so_opts_t o = { SO_SC_STANDARD, true };
    (void)mode;
    return o;
}

static void solve(int hands, int only, uint32_t budget)
{
    so_solver_t *s = so_solver_new(22, 1500);
    so_game_t g;
    memset(&g, 0, sizeof g);
    for (int mode = 0; mode < SO_NMODES; mode++) {
        if (only >= 0 && mode != only) {
            continue;
        }
        so_opts_t o = mode_opts(mode);
        int won = 0, failed = 0, gave = 0;
        double t0 = now_s(), worst = 0;
        long nodes = 0, plen = 0;
        for (int h = 0; h < hands; h++) {
            so_new(&g, mode, (uint32_t)h + 1, &o);
            double t1 = now_s();
            so_solve_res_t r = so_solve(s, &g, budget);
            double dt = now_s() - t1;
            if (dt > worst) worst = dt;
            nodes += so_solver_nodes(s);
            if (r == SO_SOLVE_SOLVED) {
                const so_move_t *p;
                int n = so_solver_path(s, &p);
                plen += n;
                CHECK(replay(mode, g.seed, &o, p, n), "mode %d seed %u: the solution does not replay", mode, g.seed);
                won++;
            } else if (r == SO_SOLVE_FAILED) {
                failed++;
            } else {
                gave++;
            }
        }
        double t = now_s() - t0;
        printf("%-18s solved %3d/%d (no %d, gave up %d)  %.1f ms avg, worst %.0f ms, %ld nodes avg, %ld moves avg\n",
               so_mode_name[mode], won, hands, failed, gave, t * 1000.0 / hands, worst * 1000.0,
               nodes / hands, won ? plen / won : 0);
        fflush(stdout);
    }
    so_free(&g);
    so_solver_free(s);
    printf("%d failures\n", fails);
}

/* The table of seeds the solver won, for each game: the app deals from it
 * when it has no time to search (the daily hand, and the board's fallback). */
static void seeds(int per, const char *out, uint32_t budget)
{
    so_solver_t *s = so_solver_new(22, 1500);
    so_game_t g;
    memset(&g, 0, sizeof g);
    FILE *f = fopen(out, "w");
    if (!f) {
        perror(out);
        return;
    }
    fprintf(f, "/*\n * SOLITARIO - seeds the solver won (tools/so_harness.c seeds %d).\n"
               " * Generated: do not edit by hand.\n */\n#include \"so_seeds.h\"\n\n", per);
    for (int mode = 0; mode < SO_NMODES; mode++) {
        so_opts_t o = mode_opts(mode);
        fprintf(f, "static const uint32_t S%d[] = {", mode);
        int n = 0;
        uint32_t seed = 1000 + (uint32_t)mode * 1000003u;
        int tried = 0;
        while (n < per && tried < per * 40) {
            seed = seed * 1664525u + 1013904223u;
            tried++;
            so_new(&g, mode, seed, &o);
            if (so_solve(s, &g, budget) != SO_SOLVE_SOLVED) {
                continue;
            }
            fprintf(f, "%s%s0x%08x", n ? "," : "", n % 6 ? " " : "\n    ", seed);
            n++;
        }
        fprintf(f, "\n};\n");
        printf("%-18s %d seeds from %d tries\n", so_mode_name[mode], n, tried);
        fflush(stdout);
    }
    fprintf(f, "\nconst uint32_t *const so_seed_tab[SO_NMODES] = {");
    for (int m = 0; m < SO_NMODES; m++) fprintf(f, "%s S%d", m ? "," : "", m);
    fprintf(f, " };\nconst uint16_t so_seed_n[SO_NMODES] = {");
    for (int m = 0; m < SO_NMODES; m++) fprintf(f, "%s sizeof S%d / 4", m ? "," : "", m);
    fprintf(f, " };\n");
    fclose(f);
    so_free(&g);
    so_solver_free(s);
}

#ifndef SOH_NO_ART
/* --- the art ------------------------------------------------------------------ */

static void ppm(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* 'src' (ARGB) over 'dst' (opaque) at x, y */
static void over(uint32_t *dst, int dw, int dh, const so_img_t *src, int x, int y)
{
    for (int j = 0; j < src->h; j++) {
        for (int i = 0; i < src->w; i++) {
            int X = x + i, Y = y + j;
            if (X < 0 || Y < 0 || X >= dw || Y >= dh) continue;
            uint32_t s = src->px[j * src->w + i], d = dst[Y * dw + X];
            int a = (int)(s >> 24);
            int r = (int)(((s >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
            int gg = (int)(((s >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
            int b = (int)((s & 255) * a + (d & 255) * (255 - a)) / 255;
            dst[Y * dw + X] = 0xFF000000u | (uint32_t)(r << 16) | (uint32_t)(gg << 8) | (uint32_t)b;
        }
    }
}

static void sheet(const char *path, int cw)
{
    int ch = so_card_h_for(cw);
    so_art_t art;
    so_art_setup(&art, cw, ch, 0, 0);
    so_img_t card = so_img_new(art.cv_w, art.cv_h);
    int cols = 13, rows = 4 + 1 + 3;
    int W = cols * (art.cv_w + 6) + 20, H = rows * (art.cv_h + 6) + 20 + 400;
    uint32_t *px = (uint32_t *)malloc((size_t)W * H * 4);
    /* the felts across the whole sheet, a band each */
    so_img_t felt = so_img_new(W, H);
    for (int f = 0; f < SO_NFELTS; f++) {
        int y0 = H * f / SO_NFELTS, y1 = H * (f + 1) / SO_NFELTS;
        so_felt_draw(&felt, f, 0, 0);
        memcpy(px + (size_t)y0 * W, felt.px + (size_t)y0 * W, (size_t)(y1 - y0) * W * 4);
    }
    for (int deck = 0; deck < 3; deck++) {
        so_art_setup(&art, cw, ch, deck, 0);
        for (int c = 0; c < 52; c++) {
            if (deck > 0 && (c % 13) < 9 && (c % 13) > 0 && c / 13 > 0) {
                continue;
            }
            so_card_draw(&art, &card, c / 13, c % 13);
            int row = deck == 0 ? c / 13 : 3 + deck;
            if (deck > 0) row = 4 + deck - 1 + (c / 13 >= 2);
            over(px, W, H, &card, 10 + (c % 13) * (art.cv_w + 6), 10 + row * (art.cv_h + 6));
        }
    }
    for (int b = 0; b < SO_NBACKS; b++) {
        so_back_draw(&art, &card, b);
        over(px, W, H, &card, 10 + b * (art.cv_w + 6), 10 + 7 * (art.cv_h + 6));
    }
    for (int k = 0; k < 4; k++) {
        so_slot_draw(&art, &card, k);
        over(px, W, H, &card, 10 + (SO_NBACKS + k) * (art.cv_w + 6), 10 + 7 * (art.cv_h + 6));
    }
    ppm(path, px, W, H);
    printf("%s: %dx%d, card %dx%d\n", path, W, H, cw, ch);
    free(px);
    so_img_free(&felt);
    so_img_free(&card);
    so_art_free(&art);
}

static void timing(int cw)
{
    int ch = so_card_h_for(cw);
    so_art_t art;
    double t0 = now_s();
    so_art_setup(&art, cw, ch, 0, 0);
    so_img_t card = so_img_new(art.cv_w, art.cv_h);
    for (int c = 0; c < 52; c++) so_card_draw(&art, &card, c / 13, c % 13);
    double t1 = now_s();
    for (int b = 0; b < SO_NBACKS; b++) so_back_draw(&art, &card, b);
    double t2 = now_s();
    so_img_t felt = so_img_new(720, 1280);
    double tf[SO_NFELTS];
    for (int f = 0; f < SO_NFELTS; f++) {
        double a = now_s();
        so_felt_draw(&felt, f, 0, 0);
        tf[f] = now_s() - a;
    }
    printf("52 faces at %dx%d: %.1f ms; %d backs: %.1f ms\n", cw, ch, (t1 - t0) * 1000,
           SO_NBACKS, (t2 - t1) * 1000);
    for (int f = 0; f < SO_NFELTS; f++) printf("felt %d at 720x1280: %.1f ms\n", f, tf[f] * 1000);
    so_img_free(&felt);
    so_img_free(&card);
    so_art_free(&art);
}

#endif

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("so_harness fuzz N | solve N [mode] [budget] | seeds N out.c [budget] | sheet out.ppm [w] | time [w]\n");
        return 1;
    }
    if (!strcmp(argv[1], "fuzz")) {
        fuzz(argc > 2 ? atoi(argv[2]) : 200);
    } else if (!strcmp(argv[1], "solve")) {
        solve(argc > 2 ? atoi(argv[2]) : 100, argc > 3 ? atoi(argv[3]) : -1,
              argc > 4 ? (uint32_t)atoi(argv[4]) : 200000);
    } else if (!strcmp(argv[1], "seeds")) {
        seeds(argc > 2 ? atoi(argv[2]) : 100, argc > 3 ? argv[3] : "so_seeds.c",
              argc > 4 ? (uint32_t)atoi(argv[4]) : 400000);
#ifndef SOH_NO_ART
    } else if (!strcmp(argv[1], "sheet")) {
        sheet(argc > 2 ? argv[2] : "sheet.ppm", argc > 3 ? atoi(argv[3]) : 96);
    } else if (!strcmp(argv[1], "time")) {
        timing(argc > 2 ? atoi(argv[2]) : 96);
#endif
    }
    return fails ? 2 : 0;
}
