/*
 * SUDOKU - the engine's harness, on the Mac:
 *
 *     cc -O2 -I../main -o /tmp/sdh sd_harness.c ../main/sd_core.c && /tmp/sdh [count] [variant] [level]
 *
 * Generates 'count' puzzles of each variant and level (or only the ones
 * given), checks that each has exactly one solution and that it is the one
 * the generator kept, grades it again, and prints how long it took, how
 * many boards it threw away and how many clues it left. The board is about
 * fifteen times slower than this Mac: what takes 30 ms here is half a
 * second there.
 */
#include "sd_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static const char *VNAME[] = { "classic", "mini", "killer", "diag" };

static void show(const sd_puzzle_t *p)
{
    const sd_shape_t *s = &p->shape;
    for (int r = 0; r < s->n; r++) {
        for (int c = 0; c < s->n; c++) {
            int v = p->given[r * s->n + c];
            putchar(v ? '0' + v : '.');
        }
        putchar(' ');
    }
    putchar('\n');
}

/* "tech N": N random minimal puzzles graded with everything, and how often
 * each technique was needed */
static int tech_census(int count)
{
    uint32_t used[SD_T_COUNT] = { 0 }, lv[SD_LEVELS] = { 0 };
    int unsolved = 0;
    sd_puzzle_t p;
    for (int k = 0; k < count; k++) {
        sd_generate(SD_V_CLASSIC, SD_L_MASTER, 5000u + (unsigned)k, &p, NULL);
        sd_grade_t g;
        sd_grade(&p.shape, p.given, SD_L_MASTER, &g);
        if (!g.solved) unsolved++;
        else lv[g.level]++;
        for (int t = 0; t < SD_T_COUNT; t++) used[t] += g.used[t];
    }
    for (int t = 1; t < SD_T_COUNT; t++) printf("tech %2d: %u\n", t, used[t]);
    printf("levels %u %u %u %u %u, unsolved %d\n", lv[0], lv[1], lv[2], lv[3], lv[4], unsolved);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && argv[1][0] == 't') return tech_census(atoi(argv[2]));
    int count = argc > 1 ? atoi(argv[1]) : 5;
    int only_v = argc > 2 ? atoi(argv[2]) : -1;
    int only_l = argc > 3 ? atoi(argv[3]) : -1;
    sd_puzzle_t p;
    for (int v = 0; v < SD_VARIANTS; v++) {
        if (only_v >= 0 && v != only_v) continue;
        int levels = v == SD_V_MINI ? 2 : v == SD_V_KILLER ? 3 : SD_LEVELS;
        for (int l = 0; l < levels; l++) {
            if (only_l >= 0 && l != only_l) continue;
            double tot = 0, worst = 0;
            int hit = 0, tries = 0, givens = 0, bad = 0;
            int lv_hist[SD_LEVELS] = { 0 };
            for (int k = 0; k < count; k++) {
                double t0 = now_ms();
                bool ok = sd_generate((sd_variant_t)v, l, 1000u * (unsigned)v + 37u * (unsigned)l + (unsigned)k + 1u,
                                      &p, NULL);
                double dt = now_ms() - t0;
                tot += dt;
                if (dt > worst) worst = dt;
                if (!ok) {
                    bad++;
                    continue;
                }
                uint8_t sol[SD_MAXC];
                int ns = sd_count(&p.shape, p.given, 2, sol, 0);
                bool same = true;
                for (int i = 0; i < p.shape.ncells; i++)
                    if (sol[i] != p.sol[i]) same = false;
                sd_grade_t g;
                sd_grade(&p.shape, p.given, SD_L_MASTER, &g);
                if (ns != 1 || !same || !g.solved) {
                    bad++;
                    printf("  BAD %s L%d k%d: solutions %d same %d solved %d\n", VNAME[v], l, k, ns, same, g.solved);
                }
                if (p.level == l) hit++;
                if (p.level >= 0 && p.level < SD_LEVELS) lv_hist[p.level]++;
                tries += p.tries;
                for (int i = 0; i < p.shape.ncells; i++)
                    if (p.given[i]) givens++;
                if (k == 0) {
                    printf("  e.g. ");
                    show(&p);
                    if (v == SD_V_KILLER) printf("       %d cages\n", p.shape.ncages);
                }
            }
            printf("%-8s L%d: %d puzzles, %.1f ms avg, %.1f worst, level hit %d/%d (%d %d %d %d %d), "
                   "%.1f boards, %.1f clues, %d bad\n",
                   VNAME[v], l, count, tot / count, worst, hit, count, lv_hist[0], lv_hist[1], lv_hist[2],
                   lv_hist[3], lv_hist[4], (double)tries / count, (double)givens / count, bad);
            fflush(stdout);
        }
    }
    return 0;
}
