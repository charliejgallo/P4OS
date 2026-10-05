/*
 * P4OS - Infrarrojo: the pins, in a thread of the app's.
 *
 * The app's thread owns the receiver and the LED (aos_io_ir_*). It is the only
 * one that blocks: aos_io_ir_send() returns when the frame is out (a NEC
 * frame is 68 ms, an air conditioner's up to half a second) and
 * aos_io_ir_read() waits for a frame. LVGL's task hands it transmissions
 * through a ring of four and takes captures from a slot, all under one
 * mutex; nothing here touches LVGL.
 *
 * A capture is what one press sends. The receiver ends a frame after
 * 30 ms of silence (the most the RMT allows), and many air conditioners
 * send two or three frames with longer gaps between them, so frames that
 * arrive within 160 ms of each other are joined, with the gap measured
 * between them as a long space. A NEC key held down adds its repeat codes
 * the same way; ir_learn.c keeps the first frame of those.
 *
 * What the LED sends reaches the receiver too when both are wired (in the
 * simulator, always): frames that arrive while sending, or just after, are
 * dropped, except during the loop test, which wants exactly that echo.
 *
 * The receiver is open only while something listens (the learn tab, the
 * portal, the loop test); the LED from the first send until the app goes
 * idle in the background.
 */
#include "ir.h"
#include "aos_hal.h"
#include "aos_io.h"

#include <stdio.h>
#include <string.h>

#define RING     4
#define JOIN_MS  160
#define CAP_MS   1500
#define ECHO_MS  200

typedef struct {
    uint32_t *d;
    int       n;
    uint32_t  carrier;
} tx_slot_t;

static void *s_mx;
static bool  s_running;
static volatile bool s_stop, s_done = true;

/* shared, under s_mx */
static int        s_rx_gpio = 28, s_tx_gpio = 32;
static bool       s_reopen;
static tx_slot_t  s_ring[RING];
static int        s_head, s_count;
static int        s_listen;        /* IR_WHO_* bits */
static ir_capture_t *s_cap;         /* the last one finished */
static uint32_t   s_sent;
static char       s_err[96];
static bool       s_idle_close;
static int        s_loop = -1;      /* -1 idle/waiting, 0 nothing heard, 1 heard */
static bool       s_loop_req;
static bool       s_rx_open;

/* the search */
static uint32_t  *s_q;
static int        s_qn;
static unsigned   s_qmask;
static bool       s_find_req;
static int        s_find_prog = -1;
static ir_match_t s_found[IR_FIND_MAX];
static int        s_nfound;

/* the thread's own */
static ir_capture_t *s_work;
static uint16_t  *s_rd;
static uint32_t  *s_seg;

static void lock(void) { aos_hal_mutex_lock(s_mx); }
static void unlock(void) { aos_hal_mutex_unlock(s_mx); }
static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

static void set_err(const char *what, int gpio)
{
    const char *who = aos_io_owner(gpio);
    lock();
    if (who && strcmp(who, IR_OWNER))
        snprintf(s_err, sizeof s_err, "GPIO%d: %s (%s)", gpio, what, who);
    else
        snprintf(s_err, sizeof s_err, "GPIO%d: %s", gpio, what);
    unlock();
}

/* ---------------------------------------------------------------- the search */

static void find_insert(int dev, int code, float score)
{
    int at = s_nfound;
    while (at > 0 && s_found[at - 1].score < score) at--;
    if (at >= IR_FIND_MAX) return;
    int last = s_nfound < IR_FIND_MAX ? s_nfound : IR_FIND_MAX - 1;
    memmove(&s_found[at + 1], &s_found[at], (size_t)(last - at) * sizeof *s_found);
    s_found[at].dev = (int16_t)dev;
    s_found[at].code = (int16_t)code;
    s_found[at].score = score;
    if (s_nfound < IR_FIND_MAX) s_nfound++;
}

static void run_find(void)
{
    char path[128];
    FILE *f = fopen(ir_pack_path(path, sizeof path), "rb");
    int count = ir_pack_count();
    lock();
    s_nfound = 0;
    int qn = s_qn;
    unsigned mask = s_qmask;
    unlock();
    if (!f || !count) { if (f) fclose(f); lock(); s_find_prog = 100; unlock(); return; }
    fseek(f, 0, SEEK_END);
    uint32_t file_end = (uint32_t)ftell(f);
    int qf = ir_frame_len(s_q, qn);
    /* a known protocol is matched by its fields: two NEC keys of one remote
     * differ in a few spaces only and would look 97 % alike */
    ir_code_t qc;
    bool qknown = ir_decode(s_q, qn, &qc) && !qc.repeat;
    size_t cap = 0;
    uint8_t *buf = NULL;
    for (int i = 0; i < count && !s_stop; i++) {
        const ir_pack_dev_t *d = ir_pack_at(i);
        if (!(mask & (1u << d->cls))) continue;
        uint32_t end = i + 1 < count ? ir_pack_at(i + 1)->blob : file_end;
        size_t len = end > d->blob ? end - d->blob : 0;
        if (len > cap) {
            ir_free(buf);
            buf = ir_alloc(len);
            cap = buf ? len : 0;
        }
        if (!buf || fseek(f, (long)d->blob, SEEK_SET) != 0 || fread(buf, 1, len, f) != len) continue;
        ir_pack_blob_t b;
        if (!ir_pack_parse_blob(buf, len, &b, false)) continue;
        float best = 0;
        int best_code = -1;
        for (int c = 0; c < b.ncodes; c++) {
            /* a cheap look at the length first: pairs against durations */
            uint32_t off = (uint32_t)b.offs[4 * c] | (uint32_t)b.offs[4 * c + 1] << 8 |
                           (uint32_t)b.offs[4 * c + 2] << 16 | (uint32_t)b.offs[4 * c + 3] << 24;
            if (off + 2 > b.codes_len) continue;
            int pairs = b.codes[off] | b.codes[off + 1] << 8;
            if (pairs * 2 < qf / 2 || pairs > qn * 2 + 8) continue;
            int n = ir_pack_code(&b, c, s_seg, IR_TX_MAX);
            float sc;
            if (qknown) {
                ir_code_t k;
                sc = (ir_decode(s_seg, n, &k) && !strcmp(k.proto, qc.proto) && k.addr == qc.addr &&
                      k.cmd == qc.cmd && (strcmp(k.proto, "Kaseikyo") || k.extra == qc.extra)) ? 1.0f : 0.0f;
            } else {
                sc = ir_similarity(s_q, qn, s_seg, n);
                float s1 = ir_similarity(s_q, qf, s_seg, ir_frame_len(s_seg, n));
                if (s1 > sc) sc = s1;
            }
            if (sc > best) { best = sc; best_code = c; }
        }
        if (best >= 0.6f) {
            lock();
            find_insert(i, best_code, best);
            unlock();
        }
        lock();
        s_find_prog = i * 100 / count;
        unlock();
    }
    ir_free(buf);
    fclose(f);
    lock();
    s_find_prog = 100;
    unlock();
}

/* -------------------------------------------------------------- the thread */

static void send_frames(aos_io_ir_t *tx, const uint32_t *d, int n)
{
    /* in pieces: at a gap the hardware cannot hold in one duration, or
     * when a piece would not fit the RMT's buffer */
    int i = 0;
    while (i < n) {
        int k = 0;
        while (i < n && k < AOS_IR_MAX_DURATIONS - 1) {
            bool space = k & 1;
            if (space && d[i] > 60000) break;
            s_rd[k++] = (uint16_t)d[i++];
        }
        if (k & 1) {
            aos_io_ir_send(tx, s_rd, k);
        } else if (k) {
            /* ends on a space: send the marks and wait the space out */
            aos_io_ir_send(tx, s_rd, k - 1);
            aos_hal_sleep_ms(s_rd[k - 1] / 1000 + 1);
        }
        if (i < n && (k & 1)) {
            /* the long gap */
            aos_hal_sleep_ms(d[i] / 1000 + 1);
            i++;
        }
    }
}

static void worker(void *arg)
{
    (void)arg;
    aos_io_ir_t *rx = NULL, *tx = NULL;
    int rx_gpio = -1, tx_gpio = -1;
    uint32_t tx_carrier = 0;
    uint32_t echo_from = 0, echo_until = 0;
    uint32_t cap_first = 0, cap_last = 0;
    uint32_t loop_until = 0;
    s_work->n = 0;

    while (!s_stop) {
        lock();
        bool reopen = s_reopen;
        s_reopen = false;
        int want_rx = s_rx_gpio, want_tx = s_tx_gpio;
        bool listen = s_listen || loop_until;
        bool loop_req = s_loop_req;
        s_loop_req = false;
        tx_slot_t slot = { 0 };
        if (s_count) {
            slot = s_ring[s_head];
            s_head = (s_head + 1) % RING;
            s_count--;
        }
        bool close_tx = s_idle_close && !s_count && !slot.n;
        s_idle_close = false;
        bool find = s_find_req;
        s_find_req = false;
        unlock();

        if (reopen || close_tx) {
            if (tx) { aos_io_ir_close(tx); tx = NULL; }
        }
        if (reopen && rx) { aos_io_ir_close(rx); rx = NULL; }

        if (loop_req) {
            /* a NEC frame the receiver should hear back */
            static const ir_code_t TEST = { "NEC", 0x00, 0x5A, 0, 32, false };
            static ir_button_t b;
            memset(&b, 0, sizeof b);
            b.code = TEST;
            uint32_t carrier;
            uint32_t *d = s_seg;
            int n = ir_button_pulses(&b, false, d, IR_TX_MAX, &carrier);
            lock();
            s_loop = -1;
            unlock();
            loop_until = now_ms() + 900;
            if (!rx) {
                rx = aos_io_ir_rx_open(want_rx, IR_GAP_US, IR_OWNER);
                rx_gpio = want_rx;
                if (!rx) set_err("no se pudo abrir el receptor", want_rx);
            }
            if (!tx) {
                tx = aos_io_ir_tx_open(want_tx, IR_OWNER);
                tx_gpio = want_tx;
                tx_carrier = 0;
                if (!tx) set_err("no se pudo abrir el LED", want_tx);
            }
            if (tx && rx) {
                aos_io_ir_set_carrier(tx, carrier, 33);
                tx_carrier = carrier;
                send_frames(tx, d, n);
            } else {
                loop_until = 0;
                lock();
                s_loop = 0;
                unlock();
            }
            continue;
        }

        if (slot.n) {
            if (tx && tx_gpio != want_tx) { aos_io_ir_close(tx); tx = NULL; }
            if (!tx) {
                tx = aos_io_ir_tx_open(want_tx, IR_OWNER);
                tx_gpio = want_tx;
                tx_carrier = 0;
                if (!tx) set_err("no se pudo abrir el LED", want_tx);
            }
            if (tx) {
                if (slot.carrier != tx_carrier) {
                    if (!aos_io_ir_set_carrier(tx, slot.carrier, 33)) aos_hal_log("ir", "carrier %u refused", (unsigned)slot.carrier);
                    tx_carrier = slot.carrier;
                }
                echo_from = now_ms();
                send_frames(tx, slot.d, slot.n);
                echo_until = now_ms() + ECHO_MS;
                aos_hal_log("ir", "sent %d durations at %u Hz on GPIO%d, %u ms", slot.n, (unsigned)slot.carrier,
                            tx_gpio, (unsigned)(echo_until - ECHO_MS - echo_from));
                lock();
                s_sent++;
                unlock();
            }
            continue;       /* the next one, if queued, before listening */
        }

        if (find) {
            run_find();
            continue;
        }

        if (listen && (!rx || rx_gpio != want_rx)) {
            if (rx) aos_io_ir_close(rx);
            rx = aos_io_ir_rx_open(want_rx, IR_GAP_US, IR_OWNER);
            rx_gpio = want_rx;
            if (!rx) {
                set_err("no se pudo abrir el receptor", want_rx);
                lock();
                s_listen = 0;
                unlock();
            }
        } else if (!listen && rx) {
            aos_io_ir_close(rx);
            rx = NULL;
            s_work->n = 0;
        }
        lock();
        s_rx_open = rx != NULL;
        unlock();

        if (!rx) {
            aos_hal_sleep_ms(20);
            continue;
        }
        int n = aos_io_ir_read(rx, s_rd, AOS_IR_MAX_DURATIONS, 40);
        uint32_t t = now_ms();
        if (n > 0) {
            bool echo = (int32_t)(t - echo_from) >= 0 && (int32_t)(echo_until - t) > 0;
            if (loop_until) {
                uint32_t f[AOS_IR_MAX_DURATIONS];
                for (int i = 0; i < n; i++) f[i] = s_rd[i];
                ir_code_t c;
                if (ir_decode(f, n, &c) && !strcmp(c.proto, "NEC") && c.cmd == 0x5A) {
                    loop_until = 0;
                    lock();
                    s_loop = 1;
                    unlock();
                }
            } else if (!echo) {
                ir_capture_t *w = s_work;
                bool room = true;
                if (!w->n) {
                    cap_first = t;
                    w->frames = 1;
                } else if (w->n + 1 + n > IR_CAP_MAX) {
                    room = false;           /* full: this press is long enough */
                } else {
                    /* the silence between the two, measured: a frame is
                     * read once the receiver has been quiet for 30 ms */
                    uint32_t dur = 0;
                    for (int i = 0; i < n; i++) dur += s_rd[i];
                    uint32_t gap = (t - cap_last) * 1000;
                    gap = gap > dur + IR_GAP_US ? gap - dur : IR_GAP_US;
                    if (!(w->n & 1)) w->d[w->n - 1] += gap;    /* it ended on a space */
                    else w->d[w->n++] = gap;
                    w->frames++;
                }
                if (room) {
                    for (int i = 0; i < n && w->n < IR_CAP_MAX; i++) w->d[w->n++] = s_rd[i];
                    cap_last = t;
                }
            }
        }
        if (loop_until && (int32_t)(t - loop_until) >= 0) {
            loop_until = 0;
            lock();
            if (s_loop < 0) s_loop = 0;
            unlock();
        }
        ir_capture_t *w = s_work;
        if (w->n && ((t - cap_last) > JOIN_MS || (t - cap_first) > CAP_MS || w->n >= IR_CAP_MAX - 8)) {
            /* the frame ends on a mark */
            if (!(w->n & 1)) w->n--;
            lock();
            memcpy(s_cap->d, w->d, (size_t)w->n * sizeof w->d[0]);
            s_cap->n = w->n;
            s_cap->frames = w->frames;
            s_cap->seq++;
            unlock();
            w->n = 0;
        }
    }
    if (rx) aos_io_ir_close(rx);
    if (tx) aos_io_ir_close(tx);
    lock();
    s_rx_open = false;
    unlock();
    s_done = true;
}

/* ------------------------------------------------------------------- API */

void ir_hw_start(void)
{
    if (s_running) return;
    if (!s_mx) s_mx = aos_hal_mutex_create();
    int32_t v;
    if (aos_hal_pref_get_i32("ir_rx", &v)) s_rx_gpio = (int)v;
    if (aos_hal_pref_get_i32("ir_tx", &v)) s_tx_gpio = (int)v;
    for (int i = 0; i < RING; i++) if (!s_ring[i].d) s_ring[i].d = ir_alloc(IR_TX_MAX * sizeof(uint32_t));
    if (!s_cap) s_cap = ir_alloc(sizeof *s_cap);
    if (!s_work) s_work = ir_alloc(sizeof *s_work);
    if (!s_rd) s_rd = ir_alloc(AOS_IR_MAX_DURATIONS * sizeof(uint16_t));
    if (!s_seg) s_seg = ir_alloc(IR_TX_MAX * sizeof(uint32_t));
    if (!s_q) s_q = ir_alloc(IR_CAP_MAX * sizeof(uint32_t));
    s_head = s_count = 0;
    s_listen = 0;
    s_find_prog = -1;
    s_loop = -1;
    /* A thread of its own, not the HAL's worker: there is one worker in the
     * whole system, and this app lives on in the background (the portal),
     * where it would keep Video or Doom from starting theirs - and their
     * aos_hal_worker_stop() would stop this one. The stack (PSRAM) holds the
     * loop test's frame copy: 4 KB of it. */
    if (!s_done) { aos_hal_log("ir", "the last thread has not finished"); return; }
    s_stop = false;
    s_done = false;
    s_running = aos_hal_thread_start("infrarrojo", worker, NULL, 12 * 1024, 3);
    if (!s_running) { s_done = true; aos_hal_log("ir", "no thread"); }
}

void ir_hw_stop(void)
{
    if (!s_running) return;
    s_stop = true;
    for (int waited = 0; !s_done && waited < 3000; waited += 10) aos_hal_sleep_ms(10);
    s_running = false;
    if (!s_done) {
        /* still sending or searching: leave it its buffers rather than pull
         * them from under it */
        aos_hal_log("ir", "the thread did not stop in 3 s");
        return;
    }
    for (int i = 0; i < RING; i++) { ir_free(s_ring[i].d); s_ring[i].d = NULL; }
    ir_free(s_cap); s_cap = NULL;
    ir_free(s_work); s_work = NULL;
    ir_free(s_rd); s_rd = NULL;
    ir_free(s_seg); s_seg = NULL;
    ir_free(s_q); s_q = NULL;
}

void ir_hw_pins(int *rx, int *tx)
{
    if (!s_mx) {
        int32_t v;
        if (aos_hal_pref_get_i32("ir_rx", &v)) s_rx_gpio = (int)v;
        if (aos_hal_pref_get_i32("ir_tx", &v)) s_tx_gpio = (int)v;
        if (rx) *rx = s_rx_gpio;
        if (tx) *tx = s_tx_gpio;
        return;
    }
    lock();
    if (rx) *rx = s_rx_gpio;
    if (tx) *tx = s_tx_gpio;
    unlock();
}

void ir_hw_set_pins(int rx, int tx)
{
    if (!s_mx) return;
    lock();
    s_rx_gpio = rx;
    s_tx_gpio = tx;
    s_reopen = true;
    unlock();
    aos_hal_pref_set_i32("ir_rx", rx);
    aos_hal_pref_set_i32("ir_tx", tx);
}

bool ir_hw_send(const uint32_t *d, int n, uint32_t carrier)
{
    if (!s_running || n <= 0) return false;
    if (n > IR_TX_MAX) n = IR_TX_MAX;
    lock();
    bool ok = s_count < RING;
    if (ok) {
        tx_slot_t *s = &s_ring[(s_head + s_count) % RING];
        memcpy(s->d, d, (size_t)n * sizeof *d);
        s->n = n;
        s->carrier = carrier;
        s_count++;
    }
    unlock();
    return ok;
}

bool ir_hw_send_button(const ir_button_t *b, bool repeat)
{
    static uint32_t buf[IR_TX_MAX];
    uint32_t carrier;
    int n = ir_button_pulses(b, repeat, buf, IR_TX_MAX, &carrier);
    return n > 0 && ir_hw_send(buf, n, carrier);
}

void ir_hw_listen(int who, bool on)
{
    if (!s_mx) return;
    lock();
    if (on) s_listen |= who;
    else s_listen &= ~who;
    unlock();
}

bool ir_hw_listening(void)
{
    if (!s_mx) return false;
    lock();
    bool on = s_listen && s_rx_open;
    unlock();
    return on;
}

bool ir_hw_capture(uint32_t *seen, ir_capture_t *out)
{
    if (!s_mx || !s_cap) return false;
    lock();
    bool fresh = s_cap->seq != *seen;
    if (fresh) {
        memcpy(out->d, s_cap->d, (size_t)s_cap->n * sizeof s_cap->d[0]);
        out->n = s_cap->n;
        out->frames = s_cap->frames;
        out->seq = s_cap->seq;
        *seen = s_cap->seq;
    }
    unlock();
    return fresh;
}

uint32_t ir_hw_capture_seq(void)
{
    if (!s_mx || !s_cap) return 0;
    lock();
    uint32_t q = s_cap->seq;
    unlock();
    return q;
}

uint32_t ir_hw_sent(void)
{
    if (!s_mx) return 0;
    lock();
    uint32_t n = s_sent;
    unlock();
    return n;
}

const char *ir_hw_error(void)
{
    static char out[96];
    out[0] = 0;
    if (!s_mx) return out;
    lock();
    memcpy(out, s_err, sizeof out);
    s_err[0] = 0;
    unlock();
    return out;
}

void ir_hw_idle_close(void)
{
    if (!s_mx) return;
    lock();
    s_idle_close = true;
    unlock();
}

bool ir_hw_find(const uint32_t *d, int n, unsigned cls_mask)
{
    if (!s_running || n <= 0 || !ir_pack_open()) return false;
    if (n > IR_CAP_MAX) n = IR_CAP_MAX;
    lock();
    bool busy = s_find_req || (s_find_prog >= 0 && s_find_prog < 100);
    if (!busy) {
        memcpy(s_q, d, (size_t)n * sizeof *d);
        s_qn = n;
        s_qmask = cls_mask;
        s_find_req = true;
        s_find_prog = 0;
        s_nfound = 0;
    }
    unlock();
    return !busy;
}

int ir_hw_find_progress(void)
{
    if (!s_mx) return -1;
    lock();
    int p = s_find_prog;
    unlock();
    return p;
}

int ir_hw_find_results(ir_match_t *out, int max)
{
    if (!s_mx) return 0;
    lock();
    int n = s_nfound < max ? s_nfound : max;
    memcpy(out, s_found, (size_t)n * sizeof *out);
    unlock();
    return n;
}

void ir_hw_loop_test(void)
{
    if (!s_mx) return;
    lock();
    s_loop_req = true;
    s_loop = -1;
    unlock();
}

int ir_hw_loop_result(void)
{
    if (!s_mx) return 0;
    lock();
    int r = s_loop;
    unlock();
    return r;
}
