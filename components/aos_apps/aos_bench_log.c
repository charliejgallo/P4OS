/*
 * P4OS - the Banco's logger (aos_bench_log.h).
 *
 * One row per tick: the wall-clock time (or the uptime when the clock was
 * never set), the seconds since the start, and a column per chosen series.
 * A value the instrument does not have right now (the supply unplugged, a
 * channel off, the scope's "invalid" measurement, one older than three
 * ticks) is an empty cell, never a stale number: a gap in a log is honest,
 * a repeated value is not.
 */
#include "aos_bench_log.h"
#include "aos_riden.h"
#include "aos_scope.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *const NAME[BL_SERIES] = {
    "Riden V", "Riden A", "Riden W",
    "CH1 Vpp", "CH1 Vavg", "CH1 Freq", "CH2 Vpp", "CH2 Vavg", "CH2 Freq",
    "CH3 Vpp", "CH3 Vavg", "CH3 Freq", "CH4 Vpp", "CH4 Vavg", "CH4 Freq",
};
static const uint32_t CH_COLOR[4] = { 0xFFD60A, 0x40C8E0, 0xFF5AD2, 0x3B82F6 };   /* the Rigol's own */

const char *bench_log_series_name(int s) { return s >= 0 && s < BL_SERIES ? NAME[s] : ""; }

const char *bench_log_series_unit(int s)
{
    if (s == BL_RD_I) return "A";
    if (s == BL_RD_P) return "W";
    if (s >= BL_CH_FIRST && (s - BL_CH_FIRST) % 3 == 2) return "Hz";
    return "V";
}

uint32_t bench_log_series_color(int s)
{
    static const uint32_t RD[3] = { 0x30D158, 0xFF9F0A, 0xF59E0B };
    if (s < BL_CH_FIRST) return RD[s];
    return CH_COLOR[(s - BL_CH_FIRST) / 3];
}

static struct {
    void *mx;
    bool started;
    bench_log_stat_t st;
    bool stop;
    FILE *f;
    float *hist;                    /* BL_SERIES x BL_HISTORY, a ring */
    int hn, hpos;
} L;

static void lock(void) { aos_hal_mutex_lock(L.mx); }
static void unlock(void) { aos_hal_mutex_unlock(L.mx); }

static void init(void)
{
    if (L.mx) return;
    L.mx = aos_hal_mutex_create();
    int32_t v;
    L.st.mask = aos_hal_pref_get_i32("bench_lmask", &v) ? (uint32_t)v : (1u << BL_RD_V | 1u << BL_RD_I | 1u << BL_CH_FIRST);
    L.st.interval_ms = aos_hal_pref_get_i32("bench_lint", &v) && v >= 200 ? (uint32_t)v : 1000;
}

void bench_log_stat(bench_log_stat_t *out)
{
    init();
    lock();
    *out = L.st;
    unlock();
}

void bench_log_configure(uint32_t mask, uint32_t interval_ms)
{
    init();
    lock();
    if (!L.st.running) {                /* a log keeps the columns it started with */
        L.st.mask = mask & ((1u << BL_SERIES) - 1);
        L.st.interval_ms = interval_ms < 200 ? 200 : interval_ms;
    }
    mask = L.st.mask;
    interval_ms = L.st.interval_ms;
    unlock();
    aos_hal_pref_set_i32("bench_lmask", (int32_t)mask);
    aos_hal_pref_set_i32("bench_lint", (int32_t)interval_ms);
}

int bench_log_history(int s, float *out, int max)
{
    init();
    if (s < 0 || s >= BL_SERIES) return 0;
    lock();
    int n = 0;
    if (L.hist) {
        int count = L.hn < max ? L.hn : max;
        int start = (L.hpos - count + BL_HISTORY) % BL_HISTORY;
        for (; n < count; n++) out[n] = L.hist[s * BL_HISTORY + (start + n) % BL_HISTORY];
    }
    unlock();
    return n;
}

/* A value of a series right now; NAN when there is none worth writing. */
static float sample(int s, uint32_t max_age)
{
    if (s < BL_CH_FIRST) {
        aos_riden_status_t r;
        aos_riden_status(&r);
        if (!r.ok || (uint32_t)aos_hal_uptime_ms() - r.t_ms > max_age) return NAN;
        return s == BL_RD_V ? r.v_out : s == BL_RD_I ? r.i_out : r.p_out;
    }
    static const aos_scope_measure_t M[3] = { AOS_SCOPE_M_VPP, AOS_SCOPE_M_VAVG, AOS_SCOPE_M_FREQ };
    int k = s - BL_CH_FIRST;
    float v;
    uint32_t age;
    if (!aos_scope_measure(k / 3 + 1, M[k % 3], &v, &age) || age > max_age) return NAN;
    return v;
}

static void worker(void *arg)
{
    (void)arg;
    uint32_t next = (uint32_t)aos_hal_uptime_ms();
    for (;;) {
        lock();
        bool stop = L.stop;
        uint32_t mask = L.st.mask, iv = L.st.interval_ms, t0 = L.st.started_ms;
        unlock();
        if (stop) break;
        if (mask & 7) aos_riden_keep(AOS_RIDEN_KEEP_LOG);
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        if ((int32_t)(now - next) < 0) {
            uint32_t w = next - now;
            aos_hal_sleep_ms(w > 100 ? 100 : w);
            continue;
        }
        next += iv;
        if ((int32_t)(now - next) > (int32_t)iv) next = now + iv;       /* fell behind: don't burst */
        /* the readings: the supply's arrive every ~200 ms, the scope's every ~1 s */
        uint32_t max_age = 3 * iv > 2500 ? 3 * iv : 2500;
        float v[BL_SERIES];
        for (int s = 0; s < BL_SERIES; s++) v[s] = mask & (1u << s) ? sample(s, max_age) : NAN;
        char line[512];
        int n;
        if (aos_hal_time_is_valid()) {
            struct tm t;
            aos_hal_time_now(&t);
            n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                         t.tm_hour, t.tm_min, t.tm_sec);
        } else {
            n = snprintf(line, sizeof line, "+%lu", (unsigned long)(now / 1000));
        }
        n += snprintf(line + n, sizeof line - (size_t)n, ",%.3f", (now - t0) / 1000.0);
        for (int s = 0; s < BL_SERIES && n < (int)sizeof line - 24; s++) {
            if (!(mask & (1u << s))) continue;
            if (isnan(v[s])) n += snprintf(line + n, sizeof line - (size_t)n, ",");
            else n += snprintf(line + n, sizeof line - (size_t)n, ",%.6g", v[s]);
        }
        line[n++] = '\n';
        bool ok = fwrite(line, 1, (size_t)n, L.f) == (size_t)n;
        fflush(L.f);
        lock();
        if (L.hist) {
            for (int s = 0; s < BL_SERIES; s++) L.hist[s * BL_HISTORY + L.hpos] = v[s];
            L.hpos = (L.hpos + 1) % BL_HISTORY;
            if (L.hn < BL_HISTORY) L.hn++;
        }
        if (ok) L.st.rows++;
        else snprintf(L.st.err, sizeof L.st.err, "%s", _("No se pudo escribir en la tarjeta."));
        L.st.seq++;
        unlock();
        if (!ok) break;
    }
    fclose(L.f);
    lock();
    L.f = NULL;
    L.st.running = false;
    L.started = false;
    L.st.seq++;
    unlock();
    aos_hal_log("bench", "log closed: %s, %u rows", L.st.path, (unsigned)L.st.rows);
}

bool bench_log_start(void)
{
    init();
    lock();
    bool busy = L.started, closing = L.stop;
    uint32_t mask = L.st.mask;
    unlock();
    if (busy) return !closing;          /* the previous one is still closing: try again in a moment */
    const char *root = aos_hal_path_sd_root();
    char err[96] = "";
    if (!root) snprintf(err, sizeof err, "%s", _("No hay tarjeta."));
    else if (!mask) snprintf(err, sizeof err, "%s", _("Elegí al menos un valor."));
    if (err[0]) {
        lock(); snprintf(L.st.err, sizeof L.st.err, "%s", err); L.st.seq++; unlock();
        return false;
    }
    char dir[96], when[32], path[128];
    snprintf(dir, sizeof dir, "%.80s/logs", root);
    mkdir(dir, 0777);
    if (aos_hal_time_is_valid()) {
        struct tm t;
        aos_hal_time_now(&t);
        strftime(when, sizeof when, "%Y%m%d-%H%M%S", &t);
    } else {
        snprintf(when, sizeof when, "t%lu", (unsigned long)(aos_hal_uptime_ms() / 1000));
    }
    snprintf(path, sizeof path, "%.90s/banco-%.20s.csv", dir, when);
    FILE *f = fopen(path, "w");
    if (!f) {
        lock(); snprintf(L.st.err, sizeof L.st.err, "%s", _("No se pudo crear el archivo.")); L.st.seq++; unlock();
        return false;
    }
    fprintf(f, "time,seconds");
    for (int s = 0; s < BL_SERIES; s++)
        if (mask & (1u << s)) fprintf(f, ",%s (%s)", NAME[s], bench_log_series_unit(s));
    fprintf(f, "\n");
    fflush(f);
    if (!L.hist) L.hist = malloc(sizeof(float) * BL_SERIES * BL_HISTORY);
    lock();
    L.f = f;
    L.stop = false;
    L.hn = L.hpos = 0;
    L.st.rows = 0;
    L.st.err[0] = 0;
    L.st.started_ms = (uint32_t)aos_hal_uptime_ms();
    snprintf(L.st.path, sizeof L.st.path, "%s", path);
    L.st.running = true;
    L.started = true;
    L.st.seq++;
    unlock();
    aos_scope_start();
    if (!aos_hal_thread_start("benchlog", worker, NULL, 6144, 3)) {
        fclose(f);
        lock(); L.f = NULL; L.st.running = L.started = false; snprintf(L.st.err, sizeof L.st.err, "no thread"); unlock();
        return false;
    }
    aos_hal_log("bench", "logging to %s every %u ms", path, (unsigned)L.st.interval_ms);
    return true;
}

void bench_log_stop(void)
{
    init();
    lock();
    L.stop = true;
    unlock();
}
