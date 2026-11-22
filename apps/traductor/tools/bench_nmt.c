/*
 * TRADUCTOR - the neural translator on the Mac, the way the board runs it:
 * one thread, float, no BLAS, and built without vectorizing
 * (tools/bench_nmt.sh). It prints the translation, the multiply-adds and
 * the time, and the estimate for the P4 from the multiply-adds.
 *
 *   bench_nmt <pak> <dir> [--int8] [--card] [--runs N] "text" ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../main/td_nmt.h"

static uint32_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static double us_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "bench_nmt <pak> <dir> [--int8] [--card] [--runs N] text...\n");
        return 2;
    }
    bool int8 = false;
    int mode = TD_NMT_EMB_RAM, runs = 1;
    td_pak_t pak;
    if (!td_pak_open(&pak, argv[1])) {
        fprintf(stderr, "%s: not a pack\n", argv[1]);
        return 1;
    }
    int a = 3;
    for (; a < argc && !strncmp(argv[a], "--", 2); a++) {
        if (!strcmp(argv[a], "--int8")) int8 = true;
        else if (!strcmp(argv[a], "--card")) mode = TD_NMT_EMB_CARD;
        else if (!strcmp(argv[a], "--runs") && a + 1 < argc) runs = atoi(argv[++a]);
    }
    double l0 = us_now();
    const char *err = NULL;
    td_nmt_t *m = td_nmt_load(&pak, argv[2], mode, &err);
    if (!m) {
        fprintf(stderr, "load: %s\n", err ? err : "?");
        return 1;
    }
    printf("loaded %s in %.0f ms: %.1f MB of model, %.1f MB in use\n", argv[2], (us_now() - l0) / 1000,
           td_nmt_bytes(m) / 1e6, td_mem_used() / 1e6);
    td_nmt_set_int8(m, int8);
    td_nmt_set_clock(m, ms_now);
    static char out[16384];
    double tot_us = 0, tot_macs = 0;
    for (; a < argc; a++) {
        td_nmt_stats_t st;
        double best = 1e30;
        for (int r = 0; r < runs; r++) {
            memset(&st, 0, sizeof st);
            double t0 = us_now();
            if (!td_nmt_translate(m, argv[a], out, sizeof out, &st)) {
                fprintf(stderr, "translate failed\n");
                return 1;
            }
            double dt = us_now() - t0;
            if (dt < best) best = dt;
        }
        tot_us += best;
        tot_macs += st.macs_m;
        /* the P4: ~5 cycles a float multiply-add at 360 MHz (docs/APPS-P4.md) */
        double p4 = st.macs_m * 1e6 * 5 / 360e6;
        printf("\n> %s\n< %s\n  %u pieces in, %u out, shortlist %u, %u M multiply-adds, "
               "%.1f ms here (%.0f M/s); P4 at 5 cycles: %.2f s one core, %.2f s two\n",
               argv[a], out, st.src_tokens, st.out_tokens, st.cand, st.macs_m, best / 1000,
               st.macs_m / (best / 1e6), p4, p4 / 2);
    }
    printf("\nall: %.0f M multiply-adds in %.1f ms\n", tot_macs, tot_us / 1000);
    td_nmt_free(m);
    td_pak_close(&pak);
    return 0;
}
