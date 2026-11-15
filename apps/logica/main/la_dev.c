/*
 * P4OS - Logic: the devices, and the jobs that run beside the UI.
 *
 * A job is a capture, a decode, saving or opening a file, or asking the
 * serial ports who speaks SUMP. One at a time, each in a thread of its own
 * (aos_hal_thread_start) that ends with it; destroy() waits for it, as the
 * app's code goes away with the .so.
 *
 * A new capture goes into LA.nxt while LA.cap stays in view, and the two
 * swap under la_lock when it is ready, so the screen keeps the last capture
 * while the next one waits for its trigger. When the memory does not allow
 * two (a capture of more than about half the free PSRAM), the old one is
 * let go first and the view says so (LA.cap_ok false).
 */
#include "la.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile bool la_rescan;
volatile int la_prefer = -1;     /* a probe found one: choose it at the next listing */

const char *la_drv_name(int drv)
{
    switch (drv) {
    case LA_DRV_FX2: return "FX2 (fx2lafw)";
    case LA_DRV_SUMP: return "SUMP / OLS";
    default: return "Demo";
    }
}

static const uint32_t R_FX2[] = { 20000, 25000, 50000, 100000, 200000, 250000, 500000, 1000000, 2000000,
                                  3000000, 4000000, 6000000, 8000000, 12000000, 16000000, 24000000 };
static const uint32_t R_SUMP[] = { 10000, 20000, 50000, 100000, 200000, 500000, 1000000, 2000000, 4000000,
                                   5000000, 10000000, 20000000, 25000000, 50000000, 100000000 };
static const uint32_t R_DEMO[] = { 100000, 250000, 500000, 1000000, 2000000, 4000000, 8000000, 16000000, 24000000 };

const uint32_t *la_rates(const la_dev_t *d, int *n)
{
    const uint32_t *r;
    int k;
    if (!d || d->drv == LA_DRV_DEMO) {
        r = R_DEMO;
        k = (int)(sizeof R_DEMO / sizeof R_DEMO[0]);
    } else if (d->drv == LA_DRV_FX2) {
        r = R_FX2;
        k = (int)(sizeof R_FX2 / sizeof R_FX2[0]);
    } else {
        r = R_SUMP;
        k = (int)(sizeof R_SUMP / sizeof R_SUMP[0]);
    }
    if (d && d->max_rate)
        while (k > 1 && r[k - 1] > d->max_rate) k--;
    /* a SUMP device's own top (the app's firmware measures it: 8.33 MHz on
     * a C3) goes last when the list does not have it */
    if (d && d->drv == LA_DRV_SUMP && d->max_rate > r[k - 1] && d->max_rate < r[k < 15 ? k : 14] && k < 20) {
        static uint32_t with_top[21];
        memcpy(with_top, r, (size_t)k * sizeof r[0]);
        with_top[k++] = d->max_rate;
        r = with_top;
    }
    *n = k;
    return r;
}

static void add_demo(void)
{
    if (LA.ndevs >= LA_DEVS_MAX) return;
    la_dev_t *d = &LA.devs[LA.ndevs++];
    memset(d, 0, sizeof *d);
    d->drv = LA_DRV_DEMO;
    snprintf(d->name, sizeof d->name, "%s", _("Demo (señales de prueba)"));
    snprintf(d->detail, sizeof d->detail, "%s", _("sin hardware: para aprender y probar los decodificadores"));
    d->max_rate = 24000000;
    d->max_samples = 8u * 1024 * 1024;
    d->channels = 8;
}

void la_devs_scan(bool probe_serial)
{
    (void)probe_serial;
    la_dev_t was = LA.ndevs ? LA.devs[LA.dev] : (la_dev_t){ .drv = 0xFF };
    int32_t pref = -1;
    if (!LA.ndevs) aos_hal_pref_get_i32("la_drv", &pref);
    LA.ndevs = 0;
    LA.ndevs += la_fx2_find(LA.devs + LA.ndevs, LA_DEVS_MAX - 1 - LA.ndevs);
    LA.ndevs += la_sump_find(LA.devs + LA.ndevs, LA_DEVS_MAX - 1 - LA.ndevs, false);
    add_demo();
    LA.dev = LA.ndevs - 1;      /* the demo, unless something better is there */
    int pick = -1;
    if (la_prefer >= 0) {
        for (int i = 0; i < LA.ndevs && pick < 0; i++)
            if (LA.devs[i].drv == la_prefer) pick = i;
        la_prefer = -1;
        if (pick >= 0) {
            LA.dev = pick;
            aos_hal_pref_set_i32("la_drv", LA.devs[pick].drv);
            return;
        }
    }
    for (int i = 0; i < LA.ndevs && pick < 0; i++) {
        const la_dev_t *d = &LA.devs[i];
        if (was.drv != 0xFF) {
            if (d->drv == was.drv && (d->drv != LA_DRV_FX2 || (d->vid == was.vid && d->pid == was.pid)) &&
                (d->drv != LA_DRV_SUMP || !strcmp(d->port, was.port)))
                pick = i;
        } else if (pref >= 0 && d->drv == pref) {
            pick = i;
        }
    }
    /* a real analyser plugged in wins over the demo it replaced */
    if (pick < 0 || LA.devs[pick].drv == LA_DRV_DEMO)
        for (int i = 0; i < LA.ndevs; i++)
            if (LA.devs[i].drv == LA_DRV_FX2) {
                pick = i;
                break;
            }
    if (pick >= 0) LA.dev = pick;
}

/* ---- jobs ---- */

typedef struct {
    void (*fn)(void *);
    void *arg;
} job_t;

static void job_main(void *a)
{
    job_t j = *(job_t *)a;
    free(a);
    j.fn(j.arg);
    la_lock();
    LA.busy = false;
    LA.state = LA_ST_IDLE;
    LA.job_done++;
    la_unlock();
}

bool la_job_start(int state, void (*fn)(void *), void *arg)
{
    la_lock();
    if (LA.busy) {
        la_unlock();
        return false;
    }
    LA.busy = true;
    LA.stop = false;
    LA.state = state;
    LA.prog = 0;
    LA.prog_total = 0;
    la_unlock();
    job_t *j = malloc(sizeof *j);
    if (j) {
        j->fn = fn;
        j->arg = arg;
    }
    if (!j || !aos_hal_thread_start("logica", job_main, j, 16384, 5)) {
        free(j);
        la_lock();
        LA.busy = false;
        LA.state = LA_ST_IDLE;
        la_unlock();
        return false;
    }
    return true;
}

void la_job_stop(void)
{
    LA.stop = true;
}

void la_job_wait(void)
{
    la_job_stop();
    for (int i = 0; i < 1000 && LA.busy; i++) aos_hal_sleep_ms(10);
    aos_hal_sleep_ms(30);       /* the thread's last instructions, after it said so */
}

la_cap_t *la_job_target(uint32_t n)
{
    /* the old capture stays in view if there is room for both */
    if (LA.nxt.buf && LA.nxt.cap >= n) return &LA.nxt;
    la_cap_free(&LA.nxt);
    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    if ((uint64_t)n + n / 16 + 4u * 1024 * 1024 < fp && la_cap_alloc(&LA.nxt, n)) return &LA.nxt;
    la_lock();
    LA.cap_ok = false;
    LA.an_ok = false;
    la_unlock();
    la_cap_free(&LA.cap);
    return &LA.cap;
}

void la_job_commit(la_cap_t *t, bool ok)
{
    if (ok) la_cap_summarise(t);
    la_lock();
    if (ok && t == &LA.nxt) {
        la_cap_t tmp = LA.cap;
        LA.cap = LA.nxt;
        LA.nxt = tmp;
        LA.nxt.n = 0;
    }
    if (ok) {
        /* the number goes with the capture in view, not with the buffer:
         * the two buffers swap, and their numbers would come back */
        static uint32_t seq;
        LA.cap.seq = ++seq;
        LA.cap_ok = true;
        LA.an_ok = false;
    } else if (t == &LA.cap) {
        LA.cap.n = 0;
        LA.cap_ok = false;
    }
    la_unlock();
    /* two big buffers are not kept beyond this */
    if (LA.nxt.cap > 4u * 1024 * 1024) la_cap_free(&LA.nxt);
}

static void decode_now(void)
{
    la_lock();
    LA.an_ok = false;
    la_unlock();
    la_decode_all();
    la_lock();
    LA.an_ok = true;
    la_unlock();
}

static void capture_job(void *arg)
{
    (void)arg;
    la_dev_t d = LA.devs[LA.dev];
    la_cfg_t cfg = LA.cfg;
    if (d.drv != LA_DRV_DEMO && d.max_samples && cfg.samples > d.max_samples) cfg.samples = d.max_samples;
    if (cfg.rate > d.max_rate && d.max_rate) cfg.rate = d.max_rate;
    la_lock();
    LA.prog_total = cfg.samples;
    la_unlock();
    la_cap_t *t = la_job_target(cfg.samples);
    const char *why;
    uint32_t t0 = la_ms();
    switch (d.drv) {
    case LA_DRV_FX2: why = la_fx2_capture(&d, &cfg, t); break;
    case LA_DRV_SUMP: why = la_sump_capture(&d, &cfg, t); break;
    default: why = la_demo_capture(&cfg, t); break;
    }
    /* an FX2's address changes once fx2lafw is in; a SUMP device said what
     * it is when it was asked: the list shows it from now on */
    if (d.drv == LA_DRV_FX2 || d.drv == LA_DRV_SUMP) la_rescan = true;
    bool ok = !why && t->n > 0;
    la_job_commit(t, ok);
    if (!ok) {
        la_set_msg(true, "%s", why ? why : _("No llegó ninguna muestra"));
        la_lock();
        LA.again = false;
        la_unlock();
        return;
    }
    char n[16], r[16];
    la_fmt_count(LA.cap.n, n, sizeof n);
    la_fmt_rate(LA.cap.rate, r, sizeof r);
    if (LA.cap.stalled)
        la_set_msg(true, _("%s muestras a %s: el analizador se frenó antes (el USB no dio abasto, probá una velocidad menor)"),
                   n, r);
    else if (LA.cap.lost)
        la_set_msg(true, _("%s muestras a %s, CON HUECOS: el USB perdió %u bytes (probá una velocidad menor)"), n, r,
                   (unsigned)LA.cap.lost);
    else if (la_trig_any(cfg.trig) && LA.cap.trig < 0)
        la_set_msg(false, _("%s muestras a %s, detenida antes del disparo"), n, r);
    else
        la_set_msg(false, _("%s muestras a %s en %.1f s"), n, r, (double)((float)(la_ms() - t0) / 1000.0f));
    aos_hal_log("logica", "capture: %s samples at %s from %s", n, r, LA.cap.src);
    decode_now();
}

bool la_capture_start(void)
{
    if (LA.busy || !LA.ndevs) return false;
    /* a rate this analyser has: the nearest below the one asked for */
    int nr;
    const uint32_t *r = la_rates(&LA.devs[LA.dev], &nr);
    uint32_t best = r[0];
    for (int i = 0; i < nr; i++)
        if (r[i] <= LA.cfg.rate) best = r[i];
    LA.cfg.rate = best;
    if (LA.cfg.samples > la_max_samples()) LA.cfg.samples = la_max_samples() & ~3u;
    if (LA.cfg.samples < 1000) {
        la_set_msg(true, "%s", _("No hay memoria libre para capturar"));
        return false;
    }
    return la_job_start(LA_ST_CONNECT, capture_job, NULL);
}

static void decode_job(void *arg)
{
    (void)arg;
    decode_now();
    la_set_msg(false, _("%u anotaciones, %u paquetes%s"), (unsigned)LA.an.nann, (unsigned)LA.an.npkt,
               LA.an.full ? _(" (no entró todo)") : "");
}

bool la_decode_start(void)
{
    if (!LA.cap.n) return false;
    return la_job_start(LA_ST_DECODE, decode_job, NULL);
}

static char s_path[200];

static void load_job(void *arg)
{
    (void)arg;
    /* the file decides the size: target the largest the memory allows */
    la_cap_t *t = &LA.nxt;
    const char *why = la_sr_load(s_path, t);
    if (why && !la_sr_nomem) {
        la_set_msg(true, "%s", why);
        return;
    }
    if (why) {
        /* no room beside the capture in view: let it go and try again */
        la_lock();
        LA.cap_ok = LA.an_ok = false;
        la_unlock();
        la_cap_free(&LA.cap);
        la_cap_free(&LA.nxt);
        t = &LA.cap;
        why = la_sr_load(s_path, t);
    }
    la_job_commit(t, !why);
    if (why) {
        la_set_msg(true, "%s", why);
        return;
    }
    char n[16], r[16];
    la_fmt_count(LA.cap.n, n, sizeof n);
    la_fmt_rate(LA.cap.rate, r, sizeof r);
    la_set_msg(false, _("Abierta: %s muestras a %s"), n, r);
    decode_now();
}

bool la_load_start(const char *path)
{
    if (LA.busy) return false;
    snprintf(s_path, sizeof s_path, "%s", path);
    return la_job_start(LA_ST_FILE, load_job, NULL);
}

static void save_job(void *arg)
{
    (void)arg;
    const char *why = la_sr_save(s_path);
    if (why) {
        la_set_msg(true, "%s", why);
        return;
    }
    /* the table beside it, when there is one */
    if (LA.an.npkt) {
        char csv[200];
        snprintf(csv, sizeof csv, "%s", s_path);
        char *dot = strrchr(csv, '.');
        if (dot) snprintf(dot, sizeof csv - (size_t)(dot - csv), ".csv");
        la_csv_save(csv);
    }
    const char *b = strrchr(s_path, '/');
    la_set_msg(false, _("Guardada: %s"), b ? b + 1 : s_path);
}

bool la_save_start(const char *path)
{
    if (LA.busy || !LA.cap.n) return false;
    snprintf(s_path, sizeof s_path, "%s", path);
    return la_job_start(LA_ST_FILE, save_job, NULL);
}

static void probe_job(void *arg)
{
    (void)arg;
    la_dev_t found[4];
    int n = la_sump_find(found, 4, true);
    if (n) {
        la_set_msg(false, _("SUMP: %s en %s"), found[0].name, found[0].port);
        la_prefer = LA_DRV_SUMP;
    }
    else la_set_msg(true, "%s", _("Ningún puerto serie USB contestó SUMP"));
    la_rescan = true;
}

bool la_probe_start(void)
{
    return la_job_start(LA_ST_CONNECT, probe_job, NULL);
}
