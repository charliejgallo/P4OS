/*
 * P4OS - the Riden RD60xx as a service for the Banco (aos_riden.h).
 *
 * The register map is the one measured on the real unit (riden-psu/README.md),
 * the same the Modbus app's Riden tab reads: 0-3 model, serial and firmware;
 * 4-19 the live block (temperature, setpoints, output, power, input, lock,
 * protection, CV/CC, enable); 38-41 Ah and Wh.
 */
#include "aos_riden.h"
#include "aos_modbus.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define OWNER "Banco"

typedef struct { uint16_t addr, v; char what[32]; } wreq_t;

static struct {
    void *mx;
    bool started;
    uint32_t keep_ms[3];            /* by who */
    uint32_t gen;                   /* bumps when the link changes */
    wreq_t q[6];
    int qn;
    aos_riden_status_t st;
} R;

static void lock(void) { aos_hal_mutex_lock(R.mx); }
static void unlock(void) { aos_hal_mutex_unlock(R.mx); }

static aos_mb_link_t link_from_prefs(void)
{
    aos_mb_link_t l = { .transport = AOS_MB_RTU, .gap_ms = 20, .timeout_ms = 300, .retries = 3 };
    int32_t v;
    if (!aos_hal_pref_get_str("mb_rd_port", l.port, sizeof l.port)) snprintf(l.port, sizeof l.port, "uart.a");
    l.baud = aos_hal_pref_get_i32("mb_rd_baud", &v) ? (uint32_t)v : 115200;
    l.unit = aos_hal_pref_get_i32("mb_rd_unit", &v) ? (uint8_t)v : 1;
    return l;
}

static bool wanted(void)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    lock();
    bool w = false;
    for (int i = 0; i < 3; i++) w |= R.keep_ms[i] && now - R.keep_ms[i] < 2000;
    unlock();
    return w;
}

static void set_err(const char *where, int rc)
{
    lock();
    snprintf(R.st.err, sizeof R.st.err, "%s: %s", where, aos_mb_strerror(rc));
    R.st.seq++;                     /* the last reading stays good for a moment: see aos_riden_status */
    unlock();
}

static void worker(void *arg)
{
    (void)arg;
    aos_mb_t *m = NULL;
    uint32_t gen = 0, t_slow = 0;
    bool have_id = false;
    for (;;) {
        if (!wanted()) {
            if (m) {
                aos_mb_close(m);
                m = NULL;
                lock(); R.st.linked = R.st.ok = false; R.st.seq++; unlock();
            }
            aos_hal_sleep_ms(200);
            continue;
        }
        lock();
        uint32_t want_gen = R.gen;
        unlock();
        if (m && gen != want_gen) { aos_mb_close(m); m = NULL; }
        if (!m) {
            aos_mb_link_t l = link_from_prefs();
            char err[96];
            m = aos_mb_open(&l, OWNER, err, sizeof err);
            lock();
            snprintf(R.st.where, sizeof R.st.where, "%s · %u · %s %u", l.port, (unsigned)l.baud, _("unidad"), l.unit);
            R.st.linked = m != NULL;
            R.st.ok = false;
            snprintf(R.st.err, sizeof R.st.err, "%s", m ? "" : err);
            R.st.seq++;
            unlock();
            gen = want_gen;
            have_id = false;
            t_slow = 0;
            if (!m) { aos_hal_sleep_ms(1500); continue; }
        }
        /* writes first, each read back */
        lock();
        wreq_t q = R.q[0];
        bool have = R.qn > 0;
        if (have) { memmove(R.q, R.q + 1, (size_t)(R.qn - 1) * sizeof R.q[0]); R.qn--; }
        unlock();
        if (have) {
            int rc = aos_mb_write_reg(m, q.addr, q.v);
            char msg[96] = "";
            if (rc) snprintf(msg, sizeof msg, "%s: %s", q.what, aos_mb_strerror(rc));
            else if (q.addr != 18) {
                uint16_t back;
                if (!aos_mb_read(m, AOS_MB_HOLDING, q.addr, 1, &back) && back != q.v)
                    snprintf(msg, sizeof msg, _("La fuente no aceptó %s"), q.what);
            }
            lock();
            snprintf(R.st.wmsg, sizeof R.st.wmsg, "%s", msg);
            R.st.wseq++;
            unlock();
            continue;
        }
        int rc;
        if (!have_id) {
            uint16_t id[4];
            rc = aos_mb_read(m, AOS_MB_HOLDING, 0, 4, id);
            if (rc) { set_err(_("modelo"), rc); goto next; }
            have_id = true;
            lock();
            R.st.model = id[0] / 10;
            R.st.serial = (uint32_t)id[1] << 16 | id[2];
            R.st.fw = id[3];
            R.st.idec = R.st.model == 6006 ? 3 : 2;
            unlock();
        }
        uint16_t f[16];
        rc = aos_mb_read(m, AOS_MB_HOLDING, 4, 16, f);
        if (rc) { set_err(_("lectura"), rc); goto next; }
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        uint16_t s[4];
        bool slow = now - t_slow >= 2000 && !aos_mb_read(m, AOS_MB_HOLDING, 38, 4, s);
        if (slow) t_slow = now;
        lock();
        float is = R.st.idec == 3 ? 1000.0f : 100.0f;
        R.st.temp_c = f[0] ? -(int)f[1] : (int)f[1];
        R.st.v_set = f[4] / 100.0f;
        R.st.i_set = f[5] / is;
        R.st.v_out = f[6] / 100.0f;
        R.st.i_out = f[7] / is;
        R.st.p_out = ((uint32_t)f[8] << 16 | f[9]) / 100.0f;
        R.st.v_in = f[10] / 100.0f;
        R.st.locked = f[11] != 0;
        R.st.protect = f[12];
        R.st.cc = f[13] != 0;
        R.st.on = f[14] != 0;
        if (slow) {
            R.st.ah = ((uint32_t)s[0] << 16 | s[1]) / 1000.0f;
            R.st.wh = ((uint32_t)s[2] << 16 | s[3]) / 1000.0f;
        }
        R.st.ok = true;
        R.st.err[0] = 0;
        R.st.t_ms = now;
        R.st.seq++;
        unlock();
    next:
        if (rc == AOS_MB_E_IO) { aos_mb_close(m); m = NULL; aos_hal_sleep_ms(500); }
        aos_hal_sleep_ms(rc ? 300 : 200);
    }
}

static void start(void)
{
    if (R.started) return;
    R.mx = aos_hal_mutex_create();
    R.st.idec = 2;
    R.started = aos_hal_thread_start("riden", worker, NULL, 6144, 4);
}

void aos_riden_keep(int who)
{
    start();
    int i = who == AOS_RIDEN_KEEP_LOG ? 1 : 0;
    lock();
    R.keep_ms[i] = (uint32_t)aos_hal_uptime_ms() | 1;
    unlock();
}

void aos_riden_status(aos_riden_status_t *out)
{
    start();
    lock();
    *out = R.st;
    unlock();
    /* one lost answer is not a missing supply: the values hold for 3 s */
    if (out->ok && (uint32_t)aos_hal_uptime_ms() - out->t_ms > 3000) out->ok = false;
}

void aos_riden_reload(void)
{
    start();
    lock();
    R.gen++;
    unlock();
}

static bool push(uint16_t addr, uint16_t v, const char *what)
{
    start();
    lock();
    bool ok = R.qn < (int)(sizeof R.q / sizeof R.q[0]);
    if (ok) {
        wreq_t *q = &R.q[R.qn++];
        q->addr = addr;
        q->v = v;
        snprintf(q->what, sizeof q->what, "%s", what);
    }
    unlock();
    return ok;
}

static void fmt(char *out, size_t n, double v, int dec, const char *unit)
{
    snprintf(out, n, "%.*f %s", dec, v, unit);
    for (char *p = out; *p; p++) if (*p == '.') *p = ',';
}

bool aos_riden_set_v(float v)
{
    if (v < 0) v = 0;
    char w[32];
    fmt(w, sizeof w, v, 2, "V");
    return push(8, (uint16_t)lroundf(v * 100), w);
}

bool aos_riden_set_i(float a)
{
    if (a < 0) a = 0;
    lock();
    int dec = R.st.idec == 3 ? 3 : 2;
    unlock();
    char w[32];
    fmt(w, sizeof w, a, dec, "A");
    return push(9, (uint16_t)lroundf(a * (dec == 3 ? 1000.0f : 100.0f)), w);
}

bool aos_riden_set_output(bool on)
{
    return push(18, on ? 1 : 0, on ? _("encender") : _("apagar"));
}
