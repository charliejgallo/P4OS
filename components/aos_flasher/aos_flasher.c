/*
 * P4OS - Programmer: the service's core.
 *
 * One job at a time, in a thread of its own (aos_hal_thread_start), so it
 * outlives the app: the Programador app starts it and draws aos_flasher_status()
 * and the log, the service does the rest with the app open or closed.
 *
 * The core knows nothing of any chip. It keeps the status, the log and the
 * cancel flag, and the dump file; the job itself is run by the family the
 * caller chose (aos_flasher_opts_t.family, aos_flasher_priv.h):
 * aos_flasher_esp.c for the ESP32s, aos_flasher_avr.c for the Arduinos,
 * aos_flasher_swd.c for Cortex-M chips by SWD, aos_flasher_stm32.c for an
 * STM32's serial bootloader.
 *
 * A dump is written to "<name>.part" while it is read, with a SHA-256 kept
 * on the way, and renamed to its final name only once the last byte is in:
 * a cancelled or failed read leaves nothing behind that could be mistaken
 * for a whole one. Its name says the chip, the board's MAC (or ID) when
 * there is one, the date and the address it starts at, in the "@0x" form
 * the list already knows, so writing it back puts it where it came from.
 * The .json beside it says the rest: chip, revision, size, range, hash.
 */
#include "aos_flasher_priv.h"
#include "aos_i18n.h"
#include "aos_hal.h"
#include "aos_io.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define LOG_LINES   200

/* -------------------------------------------------------------------------- */
/* State and the log                                                           */
/* -------------------------------------------------------------------------- */

static void *s_mutex;
aos_flasher_status_t fl_st;
volatile bool fl_cancel;
fl_job_t fl_job;

static struct {
    char    (*text)[AOS_FLASHER_LOG_LINE];
    uint8_t *level;
    uint32_t count, first;
} s_log;

void fl_lock(void)
{
    if (!s_mutex) s_mutex = aos_hal_mutex_create();
    aos_hal_mutex_lock(s_mutex);
}
void fl_unlock(void) { aos_hal_mutex_unlock(s_mutex); }

void fl_log_v(int level, const char *fmt, va_list ap)
{
    char line[AOS_FLASHER_LOG_LINE];
    vsnprintf(line, sizeof line, fmt, ap);
    aos_hal_log("flasher", "%s", line);
    fl_lock();
    if (!s_log.text) {
        s_log.text = calloc(LOG_LINES, AOS_FLASHER_LOG_LINE);
        s_log.level = calloc(LOG_LINES, 1);
    }
    if (s_log.text) {
        uint32_t i = s_log.count % LOG_LINES;
        memcpy(s_log.text[i], line, sizeof line);
        s_log.level[i] = (uint8_t)level;
        s_log.count++;
        if (s_log.count - s_log.first > LOG_LINES) s_log.first = s_log.count - LOG_LINES;
    }
    fl_unlock();
}

void fl_logf(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fl_log_v(level, fmt, ap);
    va_end(ap);
}

uint32_t aos_flasher_log_count(void) { return s_log.count; }
uint32_t aos_flasher_log_first(void) { return s_log.first; }

int aos_flasher_log_line(uint32_t index, char *out, int cap)
{
    int level = -1;
    fl_lock();
    if (s_log.text && index >= s_log.first && index < s_log.count && cap > 0) {
        snprintf(out, (size_t)cap, "%s", s_log.text[index % LOG_LINES]);
        level = s_log.level[index % LOG_LINES];
    }
    fl_unlock();
    return level;
}

void aos_flasher_status(aos_flasher_status_t *out)
{
    fl_lock();
    *out = fl_st;
    if (fl_st.busy && fl_st.job) out->elapsed_ms = (uint32_t)aos_hal_uptime_ms() - fl_st.elapsed_ms;
    fl_unlock();
}

bool aos_flasher_busy(void) { return fl_st.busy; }
void aos_flasher_cancel(void) { if (fl_st.busy) fl_cancel = true; }

void fl_phase(aos_flasher_phase_t ph)
{
    fl_lock();
    fl_st.phase = ph;
    fl_unlock();
}

bool fl_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fl_lock();
    vsnprintf(fl_st.error, sizeof fl_st.error, fmt, ap);
    fl_unlock();
    va_end(ap);
    fl_logf(L_ERR, "%s", fl_st.error);
    return false;
}

void fl_progress(uint32_t add)
{
    fl_lock();
    fl_st.done += add;
    fl_st.percent = fl_st.total ? (int)((uint64_t)fl_st.done * 100 / fl_st.total) : 0;
    if (J.io_ms > 300) {
        fl_st.bytes_per_s = (uint32_t)((uint64_t)J.io_bytes * 1000 / J.io_ms);
        fl_st.eta_ms = fl_st.bytes_per_s ? (uint32_t)((uint64_t)(fl_st.total - fl_st.done) * 1000 / fl_st.bytes_per_s) : 0;
    }
    fl_unlock();
}

/* -------------------------------------------------------------------------- */
/* The dump                                                                    */
/* -------------------------------------------------------------------------- */

static struct {
    FILE *f;
    aos_hal_sha256_ctx_t *h;
    char path[176], part[184];
    uint32_t from, len, wrote;
} D;

const char *aos_flasher_dump_dir(void)
{
    static char dir[208];
    const char *fw = aos_flasher_dir();
    if (!fw) return NULL;
    snprintf(dir, sizeof dir, "%s/volcados", fw);
    return dir;
}

/* "ESP32-S3" stays, "nRF52840 (QIAA)" becomes "nRF52840-QIAA": a file name
 * FAT and every computer take. */
static void name_part(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    bool dash = false;
    for (; *s && n + 1 < cap; s++) {
        if (isalnum((unsigned char)*s) || *s == '.') {
            out[n++] = *s;
            dash = false;
        } else if (!dash && n) {
            out[n++] = '-';
            dash = true;
        }
    }
    while (n && out[n - 1] == '-') n--;
    out[n] = 0;
}

bool fl_dump_open(uint32_t from, uint32_t len)
{
    const char *dir = aos_flasher_dump_dir();
    if (!dir) return fl_fail("%s", _("No hay tarjeta para guardar el volcado"));
    mkdir(aos_flasher_dir(), 0777);
    mkdir(dir, 0777);
    char chip[24], id[8] = "", when[24];
    name_part(chip, sizeof chip, fl_st.chip_known && fl_st.chip[0] ? fl_st.chip : "chip");
    if (fl_st.mac_known) snprintf(id, sizeof id, "-%02x%02x%02x", fl_st.mac[3], fl_st.mac[4], fl_st.mac[5]);
    if (aos_hal_time_is_valid()) {
        struct tm t;
        aos_hal_time_now(&t);
        snprintf(when, sizeof when, "%04u%02u%02u-%02u%02u%02u", (unsigned)(t.tm_year + 1900) % 10000,
                 (unsigned)(t.tm_mon + 1) % 100, (unsigned)t.tm_mday % 100, (unsigned)t.tm_hour % 100,
                 (unsigned)t.tm_min % 100, (unsigned)t.tm_sec % 100);
    } else {
        snprintf(when, sizeof when, "t%u", (unsigned)(aos_hal_uptime_ms() / 1000));
    }
    snprintf(D.path, sizeof D.path, "%.100s/%.23s%.7s-%.15s@0x%X.bin", dir, chip, id, when, (unsigned)from);
    snprintf(D.part, sizeof D.part, "%s.part", D.path);
    D.f = fopen(D.part, "wb");
    if (!D.f) return fl_fail(_("No se pudo crear %s"), D.part);
    D.h = aos_hal_sha256_begin();
    D.from = from;
    D.len = len;
    D.wrote = 0;
    fl_lock();
    fl_st.total = len;
    fl_st.done = 0;
    fl_st.out_from = from;
    fl_st.out_path[0] = 0;
    fl_st.out_sha256[0] = 0;
    fl_unlock();
    char sz[16];
    if (len >= (1u << 20) && !(len & 0xFFFFF)) snprintf(sz, sizeof sz, "%u MB", (unsigned)(len >> 20));
    else snprintf(sz, sizeof sz, "%u KB", (unsigned)((len + 1023) >> 10));
    fl_logf(L_INFO, _("Leyendo %s desde 0x%X"), sz, (unsigned)from);
    return true;
}

bool fl_dump_write(const void *buf, uint32_t n)
{
    if (!D.f) return false;
    if (fwrite(buf, 1, n, D.f) != n) return fl_fail("%s", _("No se pudo escribir en la tarjeta: ¿está llena?"));
    aos_hal_sha256_update(D.h, buf, n);
    D.wrote += n;
    fl_progress(n);
    return true;
}

void fl_dump_close(bool ok, bool verified, const char *extra)
{
    if (!D.f) return;
    bool closed = fclose(D.f) == 0;
    D.f = NULL;
    uint8_t sum[32];
    aos_hal_sha256_end(D.h, sum);
    D.h = NULL;
    ok = ok && closed && D.wrote == D.len;
    if (!ok || rename(D.part, D.path) != 0) {
        remove(D.part);
        if (ok) fl_fail(_("No se pudo guardar %s"), D.path);
        return;
    }
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", sum[i]);

    /* the .json beside it */
    char jp[184];
    snprintf(jp, sizeof jp, "%.*s.json", (int)(strlen(D.path) - 4), D.path);
    FILE *j = fopen(jp, "w");
    if (j) {
        char rev[16] = "", mac[24] = "", when[24] = "";
        if (fl_st.revision != 0xFFFF) snprintf(rev, sizeof rev, "v%u.%u", fl_st.revision / 100, fl_st.revision % 100);
        if (fl_st.mac_known)
            snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", fl_st.mac[0], fl_st.mac[1], fl_st.mac[2],
                     fl_st.mac[3], fl_st.mac[4], fl_st.mac[5]);
        if (aos_hal_time_is_valid()) {
            struct tm t;
            aos_hal_time_now(&t);
            strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &t);
        }
        fprintf(j, "{\"chip\":\"%s\",\"revision\":\"%s\",\"mac\":\"%s\",\"flash_size\":%u,"
                   "\"from\":%u,\"length\":%u,\"sha256\":\"%s\",\"verified\":%s,\"port\":\"%s\",\"date\":\"%s\"%s%s}\n",
                fl_st.chip, rev, mac, (unsigned)fl_st.flash_size, (unsigned)D.from, (unsigned)D.len, hex,
                verified ? "true" : "false", J.port, when, extra ? "," : "", extra ? extra : "");
        fclose(j);
    }
    fl_lock();
    snprintf(fl_st.out_path, sizeof fl_st.out_path, "%s", D.path);
    snprintf(fl_st.out_sha256, sizeof fl_st.out_sha256, "%s", hex);
    fl_st.verified = verified;
    fl_unlock();
    const char *base = strrchr(D.path, '/');
    fl_logf(L_OK, _("Guardado: %s"), base ? base + 1 : D.path);
    fl_logf(L_INFO, "SHA-256 %.16s…", hex);
}

/* -------------------------------------------------------------------------- */
/* The job                                                                     */
/* -------------------------------------------------------------------------- */

const char *aos_flasher_family_name(int family)
{
    switch (family) {
    case AOS_FLASHER_ESP: return "ESP32";
    case AOS_FLASHER_AVR: return "Arduino";
    case AOS_FLASHER_SWD: return "SWD";
    case AOS_FLASHER_STM32: return "STM32";
    default: return "?";
    }
}

static void worker(void *arg)
{
    (void)arg;
    bool ok = J.opts.family == AOS_FLASHER_AVR ? fl_avr_run() : J.opts.family == AOS_FLASHER_SWD ? fl_swd_run()
            : J.opts.family == AOS_FLASHER_STM32 ? fl_stm32_run() : fl_esp_run();
    /* a console ends by a cancel, and that is how it is meant to end */
    if (J.job == AOS_FLASHER_JOB_CONSOLE && fl_cancel && fl_st.phase == AOS_FLASHER_CONSOLE) ok = true;
    bool cancelled = fl_cancel && !ok;
    if (D.f) fl_dump_close(false, false, NULL);     /* a family that left one open */
    fl_lock();
    fl_st.ok = ok;
    if (ok) fl_st.percent = 100;
    if (ok && (J.job == AOS_FLASHER_JOB_FLASH || J.job == AOS_FLASHER_JOB_READ)) { fl_st.done = fl_st.total; fl_st.eta_ms = 0; }
    fl_st.phase = ok ? AOS_FLASHER_DONE : cancelled ? AOS_FLASHER_CANCELLED : AOS_FLASHER_FAILED;
    if (cancelled) snprintf(fl_st.error, sizeof fl_st.error, "%s", _("Cancelado"));
    fl_st.elapsed_ms = (uint32_t)(aos_hal_uptime_ms() - J.t0);
    fl_st.busy = false;
    fl_st.seq++;
    fl_unlock();
    uint32_t ms = fl_st.elapsed_ms;
    if (ok && J.job == AOS_FLASHER_JOB_CONSOLE)
        fl_logf(L_INFO, "%s", _("Consola cerrada"));
    else if (ok && J.job != AOS_FLASHER_JOB_DETECT)
        fl_logf(L_OK, _("Listo en %u,%u s"), (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
    else if (cancelled)
        fl_logf(L_WARN, "%s", _("Cancelado"));
    fl_cancel = false;
}

static const char *job_word(aos_flasher_job_t job)
{
    switch (job) {
    case AOS_FLASHER_JOB_DETECT: return _("Detectar");
    case AOS_FLASHER_JOB_FLASH: return _("Grabar");
    case AOS_FLASHER_JOB_READ: return _("Leer");
    case AOS_FLASHER_JOB_ERASE: return _("Borrar");
    case AOS_FLASHER_JOB_CONSOLE: return _("Consola");
    default: return "?";
    }
}

static bool start(aos_flasher_job_t job, const char *port)
{
    fl_lock();
    if (fl_st.busy) { fl_unlock(); return false; }
    J.job = job;
    snprintf(J.port, sizeof J.port, "%s", port ? port : "uart.b");
    J.t0 = aos_hal_uptime_ms();
    J.io_ms = 0;
    J.io_bytes = 0;
    fl_cancel = false;
    /* what the last job knew about the chip goes: the next one may be another */
    aos_flasher_status_t keep = fl_st;
    memset(&fl_st, 0, sizeof fl_st);
    fl_st.seq = keep.seq + 1;
    fl_st.busy = true;
    fl_st.job = job;
    fl_st.phase = AOS_FLASHER_OPENING;
    fl_st.revision = 0xFFFF;
    fl_st.family = J.opts.family;
    snprintf(fl_st.port, sizeof fl_st.port, "%s", J.port);
    if (job == AOS_FLASHER_JOB_FLASH) {
        snprintf(fl_st.source, sizeof fl_st.source, "%s", J.src.name);
        fl_st.file_count = J.src.nfiles;
        fl_st.total = J.src.total;
    }
    fl_st.elapsed_ms = (uint32_t)J.t0;      /* the start, while busy (see aos_flasher_status) */
    fl_unlock();
    const char *what = job == AOS_FLASHER_JOB_FLASH ? J.src.name : job == AOS_FLASHER_JOB_DETECT ? "chip"
                     : job == AOS_FLASHER_JOB_CONSOLE ? "RTT" : "flash";
    fl_logf(L_INFO, _("%s %s en %s"), job_word(job), what, J.port);
    if (J.opts.family != AOS_FLASHER_ESP) fl_logf(L_INFO, _("Placa: %s"), aos_flasher_family_name(J.opts.family));
    if (!aos_hal_thread_start("flasher", worker, NULL, 8192, 5)) {
        fl_lock();
        fl_st.busy = false;
        fl_st.phase = AOS_FLASHER_FAILED;
        snprintf(fl_st.error, sizeof fl_st.error, "%s", _("No se pudo lanzar la tarea"));
        fl_unlock();
        return false;
    }
    return true;
}

static void take_opts(const aos_flasher_opts_t *opts)
{
    if (opts) J.opts = *opts;
    else memset(&J.opts, 0, sizeof J.opts);
}

bool aos_flasher_detect(const char *port)
{
    return aos_flasher_detect_ex(port, NULL);
}

bool aos_flasher_detect_ex(const char *port, const aos_flasher_opts_t *opts)
{
    if (fl_st.busy) return false;
    take_opts(opts);
    return start(AOS_FLASHER_JOB_DETECT, port);
}

bool aos_flasher_flash(const char *port, const aos_flasher_source_t *src, const aos_flasher_opts_t *opts)
{
    if (fl_st.busy || !src || !src->nfiles) return false;
    J.src = *src;
    take_opts(opts);
    return start(AOS_FLASHER_JOB_FLASH, port);
}

bool aos_flasher_read(const char *port, const aos_flasher_opts_t *opts)
{
    if (fl_st.busy) return false;
    take_opts(opts);
    return start(AOS_FLASHER_JOB_READ, port);
}

bool aos_flasher_erase(const char *port, const aos_flasher_opts_t *opts)
{
    if (fl_st.busy) return false;
    take_opts(opts);
    return start(AOS_FLASHER_JOB_ERASE, port);
}

/* What goes down to the console: a small queue the job empties */
static struct { uint8_t b[256]; int n; } s_down;

bool aos_flasher_console(const char *port, const aos_flasher_opts_t *opts)
{
    if (fl_st.busy) return false;
    take_opts(opts);
    s_down.n = 0;
    J.opts.family = AOS_FLASHER_SWD;
    return start(AOS_FLASHER_JOB_CONSOLE, port);
}

bool aos_flasher_console_send(const void *data, int n)
{
    if (!fl_st.busy || fl_st.job != AOS_FLASHER_JOB_CONSOLE || n <= 0) return false;
    fl_lock();
    bool ok = s_down.n + n <= (int)sizeof s_down.b;
    if (ok) { memcpy(s_down.b + s_down.n, data, (size_t)n); s_down.n += n; }
    fl_unlock();
    return ok;
}

int fl_console_take(uint8_t *out, int max)
{
    fl_lock();
    int n = s_down.n < max ? s_down.n : max;
    memcpy(out, s_down.b, (size_t)n);
    memmove(s_down.b, s_down.b + n, (size_t)(s_down.n - n));
    s_down.n -= n;
    fl_unlock();
    return n;
}
