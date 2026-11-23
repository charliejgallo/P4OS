/*
 * P4OS - Monitor serie: recording to a CSV on the card.
 *
 * <sd>/plotter/plot-<date>-<time>.csv (plot-up<seconds>.csv before the
 * clock is set), one row per line that had numbers: the wall-clock time
 * with milliseconds (empty without a clock), the seconds since the
 * recording started, and a column per series. A new series appearing in the
 * middle starts a new file with the new header: a CSV has one. Written from
 * the app's timer through stdio's buffer and flushed every two seconds, so
 * a pulled card loses at most that.
 */
#include "pl.h"

#include <sys/stat.h>
#include <time.h>

static FILE *s_f;
static char s_path[256];
static uint32_t s_t0, s_flush, s_gen;
static time_t s_wall0;
static bool s_wall;
static int s_cols, s_part;

bool pl_csv_on(void) { return s_f != NULL; }
const char *pl_csv_path(void) { return s_path; }

static bool open_file(void)
{
    const char *sd = aos_hal_path_sd_root();
    if (!sd) return false;
    char dir[140];
    snprintf(dir, sizeof dir, "%.120s/plotter", sd);
    mkdir(dir, 0775);
    s_wall = aos_hal_time_is_valid();
    s_t0 = pl_now();
    s_wall0 = time(NULL);
    char part[16] = "";
    if (s_part) snprintf(part, sizeof part, "-%d", s_part + 1);
    if (s_wall) {
        struct tm tm;
        localtime_r(&s_wall0, &tm);
        snprintf(s_path, sizeof s_path, "%s/plot-%04d%02d%02d-%02d%02d%02d%s.csv", dir, tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, part);
    } else {
        snprintf(s_path, sizeof s_path, "%s/plot-up%u%s.csv", dir, (unsigned)(s_t0 / 1000), part);
    }
    s_f = fopen(s_path, "w");
    if (!s_f) return false;
    fputs(_("fecha y hora"), s_f);
    fputs(",", s_f);
    fputs(_("segundos"), s_f);
    for (int i = 0; i < PLD.ns; i++) {
        char h[48];
        if (PLD.s[i].unit[0]) snprintf(h, sizeof h, ",%s (%s)", PLD.s[i].name, PLD.s[i].unit);
        else snprintf(h, sizeof h, ",%s", PLD.s[i].name);
        for (char *p = h + 1; *p; p++) if (*p == ',' || *p == '"') *p = ' ';
        fputs(h, s_f);
    }
    fputs("\n", s_f);
    s_cols = PLD.ns;
    s_gen = PLD.gen;
    s_flush = s_t0;
    aos_hal_log("plotter", "recording to %s", s_path);
    return true;
}

bool pl_csv_start(void)
{
    if (s_f) return true;
    s_part = 0;
    return open_file();
}

void pl_csv_stop(void)
{
    if (!s_f) return;
    fclose(s_f);
    s_f = NULL;
}

void pl_csv_row(const pl_row_t *r)
{
    if (!s_f) return;
    if (PLD.ns != s_cols || PLD.gen != s_gen) {
        if (PLD.ns != s_cols) {
            /* a new series: a new file, with its header */
            fclose(s_f);
            s_f = NULL;
            s_part++;
            if (!open_file()) return;
        }
        s_gen = PLD.gen;
    }
    char b[64];
    if (s_wall) {
        uint32_t d = r->t - s_t0;
        time_t w = s_wall0 + (time_t)(d / 1000);
        struct tm tm;
        localtime_r(&w, &tm);
        snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d.%03u,", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, (unsigned)(d % 1000));
    } else {
        b[0] = ',';
        b[1] = 0;
    }
    fputs(b, s_f);
    int32_t ms = (int32_t)(r->t - s_t0);
    snprintf(b, sizeof b, "%s%d.%03d", ms < 0 ? "-" : "", (int)(abs(ms) / 1000), (int)(abs(ms) % 1000));
    fputs(b, s_f);
    for (int i = 0; i < s_cols; i++) {
        if (PL_ISNAN(r->v[i])) { fputs(",", s_f); continue; }
        b[0] = ',';
        pl_fmt(b + 1, sizeof b - 1, r->v[i], PLD.s[i].dec < 0 ? 4 : PLD.s[i].dec);
        fputs(b, s_f);
    }
    fputs("\n", s_f);
}

void pl_csv_poll(void)
{
    if (!s_f) return;
    uint32_t now = pl_now();
    if (now - s_flush > 2000) {
        s_flush = now;
        fflush(s_f);
    }
}
