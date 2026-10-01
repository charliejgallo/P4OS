/*
 * P4OS - the Banco's logger (aos_bench_log.c): the supply's readings and the
 * scope's measurements, sampled at a fixed pace into a CSV on the card
 * (<sd>/logs/banco-<date>.csv), in a thread of its own so a log goes on with
 * the app closed. The last samples also stay in memory for the chart.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    BL_RD_V = 0, BL_RD_I, BL_RD_P,
    BL_CH_FIRST,                                /* then 3 per channel: Vpp, Vavg, Freq */
    BL_SERIES = BL_CH_FIRST + 3 * 4,
};
#define BL_HISTORY 300

const char *bench_log_series_name(int s);       /* "Riden V", "CH1 Vpp" */
const char *bench_log_series_unit(int s);       /* "V", "A", "W", "Hz" */
uint32_t    bench_log_series_color(int s);

typedef struct {
    bool     running;
    uint32_t mask;                  /* the series chosen */
    uint32_t interval_ms;
    char     path[128];
    uint32_t rows;
    uint32_t started_ms;
    uint32_t seq;                   /* bumps with every row */
    char     err[96];
} bench_log_stat_t;

void bench_log_stat(bench_log_stat_t *out);
void bench_log_configure(uint32_t mask, uint32_t interval_ms);     /* kept in the prefs */
bool bench_log_start(void);         /* false: no card, nothing chosen (err says) */
void bench_log_stop(void);
/* The last samples of a series, oldest first, NAN where there was none. */
int  bench_log_history(int s, float *out, int max);
