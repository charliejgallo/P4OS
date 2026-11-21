/*
 * SUDOKU - the generator's thread and its stock.
 *
 * A Master board can take the generator a few seconds on the board (it
 * throws away a hundred before one needs a long chain), and nobody wants to
 * wait that long after tapping "Maestro". So a thread keeps one board of
 * every mode and level ready, saved in reserva.bin: starting a game takes
 * one, and the thread makes the next while the game is played. Only when
 * the stock is empty (the first run, or two games of the same kind in a
 * row before the thread caught up) does the screen wait.
 *
 * The daily challenge is not stocked but made from the date, so every board
 * gets the same one; today's is made in advance as soon as there is a clock.
 *
 * The thread touches no LVGL; what it hands over goes through the mutex.
 * destroy() stops it and waits: the loader unloads the .so right after.
 */
#include "sd_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS (5 + 2 + 3 + 4)

const uint8_t SD_MODE_LEVELS[SD_MODES] = { 5, 2, 3, 4, 1 };

static int slot_of(int mode, int level)
{
    int base = 0;
    for (int m = 0; m < mode && m < SD_M_DAILY; m++) base += SD_MODE_LEVELS[m];
    return base + level;
}

static struct {
    void         *mtx;
    volatile bool stop;
    volatile bool running;
    volatile bool busy;
    bool          loaded;
    sd_packed_t   slot[SLOTS];
    bool          have[SLOTS];
    uint32_t      daily_ymd;            /* the stocked daily, 0 none        */
    sd_packed_t   daily;
    /* the board asked for */
    bool          req, req_done;
    int           req_mode, req_level;
    uint32_t      req_daily;
    sd_packed_t   req_out;
    uint32_t      seed;
} GN;

sd_variant_t sd_mode_variant(int mode)
{
    switch (mode) {
    case SD_M_MINI: return SD_V_MINI;
    case SD_M_KILLER: return SD_V_KILLER;
    case SD_M_DIAG: return SD_V_DIAG;
    default: return SD_V_CLASSIC;
    }
}

/* Monday easy, the weekend harder, two variants along the week. */
void sd_daily_pick(uint32_t ymd, int wday, int *mode, int *level)
{
    (void)ymd;
    static const uint8_t M[7] = { SD_M_KILLER, SD_M_CLASSIC, SD_M_CLASSIC, SD_M_DIAG, SD_M_CLASSIC, SD_M_KILLER,
                                  SD_M_CLASSIC };
    static const uint8_t L[7] = { SD_L_MEDIUM, SD_L_EASY, SD_L_MEDIUM, SD_L_MEDIUM, SD_L_HARD, SD_L_EASY,
                                  SD_L_EXPERT };
    if (wday < 0 || wday > 6) wday = 0;
    *mode = M[wday];
    *level = L[wday];
}

#define POOL_MAGIC 0x31505344u          /* "DSP1" */

static void pool_path(char *out, size_t n)
{
    char dir[240];
    sd_save_dir(dir, sizeof dir);
    snprintf(out, n, "%.240s/reserva.bin", dir);
}

static void pool_load(void)
{
    char p[300];
    pool_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    uint32_t h[2];
    if (fread(h, 4, 2, f) == 2 && h[0] == POOL_MAGIC && h[1] == sizeof GN.slot) {
        if (fread(GN.slot, sizeof GN.slot, 1, f) != 1 || fread(GN.have, sizeof GN.have, 1, f) != 1 ||
            fread(&GN.daily_ymd, 4, 1, f) != 1 || fread(&GN.daily, sizeof GN.daily, 1, f) != 1) {
            memset(GN.have, 0, sizeof GN.have);
            GN.daily_ymd = 0;
        }
    }
    fclose(f);
}

static void pool_save(void)
{
    char p[300], tmp[310];
    pool_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    uint32_t h[2] = { POOL_MAGIC, sizeof GN.slot };
    aos_hal_mutex_lock(GN.mtx);
    bool ok = fwrite(h, 4, 2, f) == 2 && fwrite(GN.slot, sizeof GN.slot, 1, f) == 1 &&
              fwrite(GN.have, sizeof GN.have, 1, f) == 1 && fwrite(&GN.daily_ymd, 4, 1, f) == 1 &&
              fwrite(&GN.daily, sizeof GN.daily, 1, f) == 1;
    aos_hal_mutex_unlock(GN.mtx);
    fclose(f);
    if (ok) {
        remove(p);
        rename(tmp, p);
    } else {
        remove(tmp);
    }
}

static bool make(int mode, int level, uint32_t daily, sd_packed_t *out)
{
    sd_puzzle_t *p = (sd_puzzle_t *)malloc(sizeof(sd_puzzle_t));
    if (!p) return false;
    uint32_t seed = daily ? daily * 7919u + 17u : (GN.seed += 0x9E3779B9u) ^ aos_hal_uptime_ms();
    uint32_t t0 = aos_hal_uptime_ms();
    bool ok = sd_generate(sd_mode_variant(mode), level, seed, p, &GN.stop);
    if (ok) {
        sd_pack(p, out);
        aos_hal_log(SD_TAG, "board mode %d level %d (got %d) in %u ms, %d tries", mode, level, p->level,
                    (unsigned)(aos_hal_uptime_ms() - t0), p->tries);
    }
    free(p);
    return ok && !GN.stop;
}

static void thread_fn(void *arg)
{
    (void)arg;
    if (!GN.loaded) {
        pool_load();
        GN.loaded = true;
    }
    while (!GN.stop) {
        /* what the screen is waiting for comes first */
        aos_hal_mutex_lock(GN.mtx);
        bool req = GN.req && !GN.req_done;
        int rm = GN.req_mode, rl = GN.req_level;
        uint32_t rd = GN.req_daily;
        aos_hal_mutex_unlock(GN.mtx);
        if (req) {
            GN.busy = true;
            sd_packed_t out;
            bool ok = make(rm, rl, rd, &out);
            GN.busy = false;
            if (!ok) continue;
            aos_hal_mutex_lock(GN.mtx);
            if (GN.req && GN.req_mode == rm && GN.req_level == rl && GN.req_daily == rd) {
                GN.req_out = out;
                GN.req_done = true;
            }
            if (rd) {
                GN.daily_ymd = rd;
                GN.daily = out;
            }
            aos_hal_mutex_unlock(GN.mtx);
            if (rd) pool_save();
            continue;
        }
        /* today's challenge, ahead of time */
        uint32_t ymd;
        int wday;
        if (sd_today(&ymd, &wday) && ymd != GN.daily_ymd) {
            int m, l;
            sd_daily_pick(ymd, wday, &m, &l);
            sd_packed_t out;
            GN.busy = true;
            bool ok = make(m, l, ymd, &out);
            GN.busy = false;
            if (ok) {
                aos_hal_mutex_lock(GN.mtx);
                GN.daily_ymd = ymd;
                GN.daily = out;
                aos_hal_mutex_unlock(GN.mtx);
                pool_save();
            }
            continue;
        }
        /* the stock */
        int empty = -1;
        aos_hal_mutex_lock(GN.mtx);
        for (int i = 0; i < SLOTS; i++)
            if (!GN.have[i]) {
                empty = i;
                break;
            }
        aos_hal_mutex_unlock(GN.mtx);
        if (empty < 0) {
            aos_hal_sleep_ms(150);
            continue;
        }
        int mode = 0, level = empty;
        while (mode < SD_M_DAILY && level >= SD_MODE_LEVELS[mode]) level -= SD_MODE_LEVELS[mode++];
        sd_packed_t out;
        GN.busy = true;
        bool ok = make(mode, level, 0, &out);
        GN.busy = false;
        if (!ok) continue;
        aos_hal_mutex_lock(GN.mtx);
        GN.slot[empty] = out;
        GN.have[empty] = true;
        aos_hal_mutex_unlock(GN.mtx);
        pool_save();
    }
    GN.running = false;
}

void sd_gen_start(void)
{
    if (GN.running) return;
    if (!GN.mtx) GN.mtx = aos_hal_mutex_create();
    if (!GN.seed) GN.seed = aos_hal_uptime_ms() * 2654435761u + 1u;
    GN.stop = false;
    GN.running = true;
    /* low priority: the interface and the drawing come first */
    if (!aos_hal_thread_start("sd_gen", thread_fn, NULL, 24 * 1024, 2)) {
        GN.running = false;
        aos_hal_log(SD_TAG, "no generator thread");
    }
}

void sd_gen_stop(void)
{
    GN.stop = true;
    /* a Master board's grading can take a while: up to 8 s */
    for (int i = 0; i < 800 && GN.running; i++) aos_hal_sleep_ms(10);
    if (GN.running) aos_hal_log(SD_TAG, "generator did not stop");
}

bool sd_gen_take(int mode, int level, uint32_t daily, sd_packed_t *out)
{
    if (!GN.mtx) GN.mtx = aos_hal_mutex_create();
    bool got = false;
    aos_hal_mutex_lock(GN.mtx);
    if (daily) {
        if (GN.daily_ymd == daily) {
            *out = GN.daily;
            got = true;
        }
    } else if (mode < SD_M_DAILY && level < SD_MODE_LEVELS[mode]) {
        int s = slot_of(mode, level);
        if (GN.have[s]) {
            *out = GN.slot[s];
            GN.have[s] = false;
            got = true;
        }
    }
    GN.req = !got;
    GN.req_done = false;
    GN.req_mode = mode;
    GN.req_level = level;
    GN.req_daily = daily;
    aos_hal_mutex_unlock(GN.mtx);
    return got;
}

bool sd_gen_poll(sd_packed_t *out)
{
    bool got = false;
    if (!GN.mtx) return false;
    aos_hal_mutex_lock(GN.mtx);
    if (GN.req && GN.req_done) {
        *out = GN.req_out;
        GN.req = false;
        GN.req_done = false;
        got = true;
    }
    aos_hal_mutex_unlock(GN.mtx);
    return got;
}

bool sd_gen_busy(void)
{
    return GN.busy;
}
