/*
 * RF - a recording as a source: cu8 I/Q from a file, at its own rate, in
 * real time, over and over (rf.h).
 *
 * The app plays back what it recorded (the .cu8 in rf/iq, rf_src_file_open), and
 * in the simulator the environment's RF_IQ_FILE, "path@rate" (on the board
 * getenv() is always NULL), wins over the RTL-SDR, so the app is tried
 * against the signals of apps/rf/test/.
 */
#include "rf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aos_hal.h"
#include "aos_i18n.h"

typedef struct {
    rf_src_t base;
    FILE *f;
    uint32_t rate;
    char name[48];
    uint64_t t0, sent, bytes;
    bool running;
} file_src_t;

static rf_src_t *open_path(const char *path, uint32_t rate)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    file_src_t *s = calloc(1, sizeof *s);
    if (!s) {
        fclose(f);
        return NULL;
    }
    s->f = f;
    s->rate = rate;
    const char *base = strrchr(path, '/');
    snprintf(s->name, sizeof s->name, "%s", base ? base + 1 : path);
    s->base.ops = &rf_src_file;
    return &s->base;
}

rf_src_t *rf_src_file_open(const char *path, uint32_t rate, uint32_t freq_hz)
{
    (void)freq_hz;
    return open_path(path, rate);
}

static rf_src_t *file_open(const char **why)
{
    const char *e = getenv("RF_IQ_FILE");
    if (!e || !*e) {
        *why = N_("No hay ninguna RTL-SDR en el host USB");
        return NULL;
    }
    char path[256];
    snprintf(path, sizeof path, "%s", e);
    char *at = strrchr(path, '@');
    uint32_t rate = at ? (uint32_t)strtoul(at + 1, NULL, 10) : 2400000;
    if (at) *at = 0;
    rf_src_t *src = open_path(path, rate);
    if (!src) *why = N_("No hay ninguna RTL-SDR en el host USB");
    return src;
}

static void file_info(rf_src_t *b, rf_src_info_t *o)
{
    file_src_t *s = (file_src_t *)b;
    memset(o, 0, sizeof *o);
    o->kind = "I/Q";
    snprintf(o->name, sizeof o->name, "%s", s->name);
    snprintf(o->tuner, sizeof o->tuner, "%s", _("archivo"));
    o->fmin = 1000000;
    o->fmax = 2000000000;
    o->high_speed = true;
}

static bool file_set_freq(rf_src_t *b, uint32_t hz) { (void)b; (void)hz; return true; }
static bool file_set_gain(rf_src_t *b, int g) { (void)b; (void)g; return true; }
static uint32_t file_set_rate(rf_src_t *b, uint32_t sps) { (void)sps; return ((file_src_t *)b)->rate; }

static bool file_start(rf_src_t *b)
{
    file_src_t *s = (file_src_t *)b;
    s->t0 = aos_hal_uptime_us();
    s->sent = 0;
    s->running = true;
    return true;
}

/* as fast as the samples would come from the air, not faster */
static int file_read(rf_src_t *b, uint8_t *iq, int bytes, int timeout_ms)
{
    file_src_t *s = (file_src_t *)b;
    bytes &= ~1;
    uint64_t due = (aos_hal_uptime_us() - s->t0) * s->rate / 1000000 * 2;
    if (due <= s->sent) {
        uint64_t wait_us = (s->sent - due) / 2 * 1000000 / s->rate + 1000;
        if (wait_us > (uint64_t)timeout_ms * 1000) wait_us = (uint64_t)timeout_ms * 1000;
        aos_hal_sleep_ms((uint32_t)(wait_us / 1000) + 1);
        due = (aos_hal_uptime_us() - s->t0) * s->rate / 1000000 * 2;
        if (due <= s->sent) return 0;
    }
    uint64_t can = due - s->sent;
    if ((uint64_t)bytes > can) bytes = (int)can & ~1;
    int n = (int)fread(iq, 1, bytes, s->f);
    if (n < bytes) {
        rewind(s->f);
        n += (int)fread(iq + n, 1, bytes - n, s->f);
    }
    s->sent += n;
    s->bytes += n;
    return n & ~1;
}

static void file_stats(rf_src_t *b, uint64_t *bytes, uint32_t *dropped)
{
    *bytes = ((file_src_t *)b)->bytes;
    *dropped = 0;
}

static void file_stop(rf_src_t *b) { ((file_src_t *)b)->running = false; }

static void file_close(rf_src_t *b)
{
    file_src_t *s = (file_src_t *)b;
    fclose(s->f);
    free(s);
}

const rf_src_ops_t rf_src_file = {
    .kind = "I/Q",
    .open = file_open,
    .info = file_info,
    .set_freq = file_set_freq,
    .set_rate = file_set_rate,
    .set_gain = file_set_gain,
    .start = file_start,
    .read = file_read,
    .stats = file_stats,
    .stop = file_stop,
    .close = file_close,
};
