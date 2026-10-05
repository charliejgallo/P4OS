/*
 * P4OS simulator - PWM, the analog level, infrared and CAN (aos_io.h): the
 * backends of aos_io_signal.c.
 *
 * PWM and the analog level only keep their numbers, with the board's
 * limits (seven channels, three frequencies at once, the LEDC's duty
 * resolution for each frequency), so an app hits the same walls here.
 *
 * Infrared: what any IR output sends arrives at every open input, as if
 * the LED pointed at the receiver. And a remote control in the room presses
 * a key every 6 s on the NEC protocol (address 0x04, the command counting
 * up), unless P4_SIM_IR_REMOTE=0.
 *
 * CAN: every node open in the simulator is on one bus. A node in SELFTEST
 * hears itself. Unless P4_SIM_CAN_TRAFFIC=0 a car is on the bus too, sending
 * what an engine and a dashboard do: 0x0C0 engine speed (10 ms), 0x1A0
 * vehicle speed (20 ms), 0x3E8 coolant and oil temperature (500 ms) and
 * 0x18FEF100 (an extended id, J1939 style, 100 ms); it acknowledges what a
 * NORMAL node sends. Frames reach a node through its filter and a queue of
 * 64 that drops the oldest, as on the board.
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;

static void wait_until(pthread_cond_t *cv, int timeout_ms)
{
    if (timeout_ms < 0) { pthread_cond_wait(cv, &s_mx); return; }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(cv, &s_mx, &ts);
}

/* ======================= PWM ======================= */

static struct { uint32_t hz; int refs; } s_tm[3];
static int s_channels;

static uint8_t bits_for(uint32_t hz)
{
    int b = (int)floor(log2(40000000.0 / (double)hz));
    if (b < 1) b = 1;
    if (b > 20) b = 20;
    return (uint8_t)b;
}

static int tm_get(uint32_t hz)
{
    for (int t = 0; t < 3; t++) if (s_tm[t].refs && s_tm[t].hz == hz) { s_tm[t].refs++; return t; }
    for (int t = 0; t < 3; t++) if (!s_tm[t].refs) { s_tm[t].hz = hz; s_tm[t].refs = 1; return t; }
    aos_hal_log("io", "PWM: no timer left for %u Hz (three frequencies at once at most)", (unsigned)hz);
    return -1;
}

static void tm_put(int t) { if (t >= 0 && s_tm[t].refs) s_tm[t].refs--; }

/* the LEDC's own rounding: the divider is a whole number of 1/256 */
static uint32_t actual_hz(uint32_t hz, uint8_t bits)
{
    double div = 40000000.0 / ((double)hz * (double)(1u << bits));
    div = floor(div * 256.0 + 0.5) / 256.0;
    if (div < 1.0) div = 1.0;
    return (uint32_t)lround(40000000.0 / (div * (double)(1u << bits)));
}

bool aos_io_be_pwm_open(aos_io_pwm_t *p, uint32_t freq_hz)
{
    pthread_mutex_lock(&s_mx);
    if (s_channels >= 7) { pthread_mutex_unlock(&s_mx); aos_hal_log("io", "PWM: all seven channels are in use"); return false; }
    int t = tm_get(freq_hz);
    if (t < 0) { pthread_mutex_unlock(&s_mx); return false; }
    s_channels++;
    p->be = (void *)(intptr_t)(t + 1);
    p->bits = bits_for(freq_hz);
    p->freq = actual_hz(freq_hz, p->bits);
    pthread_mutex_unlock(&s_mx);
    return true;
}

bool aos_io_be_pwm_set_freq(aos_io_pwm_t *p, uint32_t freq_hz)
{
    pthread_mutex_lock(&s_mx);
    int old = (int)(intptr_t)p->be - 1, t;
    if (s_tm[old].refs == 1) { s_tm[old].refs = 0; t = tm_get(freq_hz); if (t < 0) s_tm[old].refs = 1; }
    else { t = tm_get(freq_hz); if (t >= 0) tm_put(old); }
    if (t < 0) { pthread_mutex_unlock(&s_mx); return false; }
    p->be = (void *)(intptr_t)(t + 1);
    p->bits = bits_for(freq_hz);
    p->freq = actual_hz(freq_hz, p->bits);
    pthread_mutex_unlock(&s_mx);
    return true;
}

bool aos_io_be_pwm_set_duty(aos_io_pwm_t *p, float duty, uint32_t fade_ms)
{
    (void)p; (void)duty; (void)fade_ms;
    return true;
}

void aos_io_be_pwm_close(aos_io_pwm_t *p)
{
    pthread_mutex_lock(&s_mx);
    tm_put((int)(intptr_t)p->be - 1);
    s_channels--;
    pthread_mutex_unlock(&s_mx);
    p->be = NULL;
}

/* ======================= the analog level ======================= */

static int s_dacs;

bool aos_io_be_dac_open(aos_io_dac_t *d)
{
    pthread_mutex_lock(&s_mx);
    bool ok = s_dacs < 8;
    if (ok) s_dacs++;
    pthread_mutex_unlock(&s_mx);
    (void)d;
    return ok;
}

bool aos_io_be_dac_set(aos_io_dac_t *d, float level) { (void)d; (void)level; return true; }

void aos_io_be_dac_close(aos_io_dac_t *d)
{
    (void)d;
    pthread_mutex_lock(&s_mx);
    s_dacs--;
    pthread_mutex_unlock(&s_mx);
}

/* ======================= infrared ======================= */

#define IR_FRAMES 4

typedef struct ir_sim {
    aos_io_ir_t *ir;
    struct ir_sim *next;
    uint16_t frame[IR_FRAMES][AOS_IR_MAX_DURATIONS];
    int len[IR_FRAMES];
    int head, count;
    pthread_cond_t cv;
} ir_sim_t;

static ir_sim_t *s_ir_rx;
static pthread_t s_remote;
static bool s_remote_on;

static void ir_deliver(const uint16_t *us, int n)
{
    for (ir_sim_t *r = s_ir_rx; r; r = r->next) {
        if (r->count == IR_FRAMES) { r->head = (r->head + 1) % IR_FRAMES; r->count--; }
        int at = (r->head + r->count) % IR_FRAMES;
        memcpy(r->frame[at], us, (size_t)n * sizeof *us);
        r->len[at] = n;
        r->count++;
        pthread_cond_broadcast(&r->cv);
    }
}

static void *remote_main(void *arg)
{
    (void)arg;
    uint8_t cmd = 0x10;
    for (;;) {
        struct timespec ts = { 6, 0 };
        nanosleep(&ts, NULL);
        /* NEC: 9 ms mark, 4.5 ms space, 32 bits LSB first (address, its
         * inverse, command, its inverse), a closing mark */
        uint16_t f[67];
        int n = 0;
        f[n++] = 9000; f[n++] = 4500;
        uint32_t word = 0x04u | (uint32_t)(uint8_t)~0x04u << 8 | (uint32_t)cmd << 16 | (uint32_t)(uint8_t)~cmd << 24;
        for (int b = 0; b < 32; b++) { f[n++] = 560; f[n++] = (word >> b & 1) ? 1690 : 560; }
        f[n++] = 560;
        pthread_mutex_lock(&s_mx);
        ir_deliver(f, n);
        pthread_mutex_unlock(&s_mx);
        cmd = (uint8_t)(cmd + 1);
    }
    return NULL;
}

bool aos_io_be_ir_open(aos_io_ir_t *ir)
{
    if (ir->tx) return true;
    ir_sim_t *r = calloc(1, sizeof *r);
    if (!r) return false;
    r->ir = ir;
    pthread_cond_init(&r->cv, NULL);
    pthread_mutex_lock(&s_mx);
    r->next = s_ir_rx;
    s_ir_rx = r;
    const char *e = getenv("P4_SIM_IR_REMOTE");
    if (!s_remote_on && !(e && !strcmp(e, "0"))) {
        s_remote_on = true;
        pthread_create(&s_remote, NULL, remote_main, NULL);
        pthread_detach(s_remote);
    }
    pthread_mutex_unlock(&s_mx);
    ir->be = r;
    return true;
}

int aos_io_be_ir_read(aos_io_ir_t *ir, uint16_t *us, int max, int timeout_ms)
{
    ir_sim_t *r = ir->be;
    pthread_mutex_lock(&s_mx);
    if (!r->count) wait_until(&r->cv, timeout_ms);
    int n = 0;
    if (r->count) {
        n = r->len[r->head] < max ? r->len[r->head] : max;
        memcpy(us, r->frame[r->head], (size_t)n * sizeof *us);
        r->head = (r->head + 1) % IR_FRAMES;
        r->count--;
    }
    pthread_mutex_unlock(&s_mx);
    return n;
}

bool aos_io_be_ir_carrier(aos_io_ir_t *ir) { (void)ir; return true; }

bool aos_io_be_ir_send(aos_io_ir_t *ir, const uint16_t *us, int n)
{
    (void)ir;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) total += us[i];
    aos_hal_sleep_ms((int)(total / 1000) + 1);      /* as long as it takes on air */
    pthread_mutex_lock(&s_mx);
    ir_deliver(us, n);
    pthread_mutex_unlock(&s_mx);
    return true;
}

void aos_io_be_ir_close(aos_io_ir_t *ir)
{
    ir_sim_t *r = ir->be;
    if (!r) return;
    pthread_mutex_lock(&s_mx);
    for (ir_sim_t **pp = &s_ir_rx; *pp; pp = &(*pp)->next) if (*pp == r) { *pp = r->next; break; }
    pthread_mutex_unlock(&s_mx);
    pthread_cond_destroy(&r->cv);
    free(r);
    ir->be = NULL;
}

/* ======================= CAN ======================= */

#define CAN_Q 64

typedef struct can_sim {
    aos_io_can_t *c;
    struct can_sim *next;
    aos_can_frame_t q[CAN_Q];
    int head, count;
    uint32_t f_id, f_mask;
    bool f_ext, f_on;
    pthread_cond_t cv;
} can_sim_t;

static can_sim_t *s_can;
static bool s_car_on;

static void can_deliver(const can_sim_t *from, const aos_can_frame_t *f)
{
    aos_can_frame_t g = *f;
    g.t_us = aos_hal_uptime_us();
    for (can_sim_t *n = s_can; n; n = n->next) {
        /* a node in the self test is off the bus: it hears only itself */
        bool self = from && from->c->mode == AOS_CAN_SELFTEST;
        if (self ? n != from : (n == from || n->c->mode == AOS_CAN_SELFTEST)) continue;
        if (n->f_on && (g.ext != n->f_ext || (g.id & n->f_mask) != (n->f_id & n->f_mask))) continue;
        if (n->count == CAN_Q) { n->head = (n->head + 1) % CAN_Q; n->count--; n->c->dropped++; }
        n->q[(n->head + n->count) % CAN_Q] = g;
        n->count++;
        pthread_cond_broadcast(&n->cv);
    }
}

static void *car_main(void *arg)
{
    (void)arg;
    for (uint32_t tick = 0;; tick++) {
        struct timespec ts = { 0, 10000000 };       /* 10 ms */
        nanosleep(&ts, NULL);
        double t = (double)aos_hal_uptime_ms() / 1000.0;
        pthread_mutex_lock(&s_mx);
        /* engine speed: idling at 800 rpm with a rev every 8 s, x4 in the frame */
        double rpm = 800 + 2600 * pow(fmax(0, sin(t * 0.785)), 6);
        uint16_t r4 = (uint16_t)(rpm * 4);
        aos_can_frame_t f = { .id = 0x0C0, .len = 8, .data = { 0, 0, (uint8_t)(r4 >> 8), (uint8_t)r4, 0, 0, 0, (uint8_t)tick } };
        can_deliver(NULL, &f);
        if (tick % 2 == 0) {
            uint16_t kmh = (uint16_t)((40 + 25 * sin(t * 0.1)) * 100);
            aos_can_frame_t s = { .id = 0x1A0, .len = 4, .data = { (uint8_t)(kmh >> 8), (uint8_t)kmh, 0, 0 } };
            can_deliver(NULL, &s);
        }
        if (tick % 10 == 0) {
            aos_can_frame_t j = { .id = 0x18FEF100, .ext = true, .len = 8,
                                  .data = { 0xFF, (uint8_t)(tick >> 8), (uint8_t)tick, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };
            can_deliver(NULL, &j);
        }
        if (tick % 50 == 0) {
            uint8_t cool = (uint8_t)(40 + 88 - 48 * exp(-t / 120.0));   /* warming up to 88 C, +40 */
            aos_can_frame_t c = { .id = 0x3E8, .len = 2, .data = { cool, (uint8_t)(cool + 6) } };
            can_deliver(NULL, &c);
        }
        pthread_mutex_unlock(&s_mx);
    }
    return NULL;
}

bool aos_io_be_can_open(aos_io_can_t *c)
{
    can_sim_t *n = calloc(1, sizeof *n);
    if (!n) return false;
    n->c = c;
    pthread_cond_init(&n->cv, NULL);
    pthread_mutex_lock(&s_mx);
    n->next = s_can;
    s_can = n;
    const char *e = getenv("P4_SIM_CAN_TRAFFIC");
    if (!s_car_on && !(e && !strcmp(e, "0"))) {
        pthread_t th;
        s_car_on = true;
        pthread_create(&th, NULL, car_main, NULL);
        pthread_detach(th);
    }
    pthread_mutex_unlock(&s_mx);
    c->be = n;
    return true;
}

bool aos_io_be_can_filter(aos_io_can_t *c, uint32_t id, uint32_t mask, bool ext)
{
    can_sim_t *n = c->be;
    pthread_mutex_lock(&s_mx);
    n->f_id = id;
    n->f_mask = mask;
    n->f_ext = ext;
    n->f_on = mask != 0;
    pthread_mutex_unlock(&s_mx);
    return true;
}

bool aos_io_be_can_send(aos_io_can_t *c, const aos_can_frame_t *f, int timeout_ms)
{
    (void)timeout_ms;
    can_sim_t *n = c->be;
    pthread_mutex_lock(&s_mx);
    /* NORMAL needs somebody to acknowledge: another node, or the car */
    bool acked = c->mode == AOS_CAN_SELFTEST || s_car_on;
    for (can_sim_t *o = s_can; o && !acked; o = o->next) if (o != n && o->c->mode == AOS_CAN_NORMAL) acked = true;
    if (acked) can_deliver(n, f);
    pthread_mutex_unlock(&s_mx);
    return acked;
}

int aos_io_be_can_recv(aos_io_can_t *c, aos_can_frame_t *f, int timeout_ms)
{
    can_sim_t *n = c->be;
    pthread_mutex_lock(&s_mx);
    if (!n->count) wait_until(&n->cv, timeout_ms);
    int got = 0;
    if (n->count) {
        *f = n->q[n->head];
        n->head = (n->head + 1) % CAN_Q;
        n->count--;
        got = 1;
    }
    pthread_mutex_unlock(&s_mx);
    return got;
}

bool aos_io_be_can_status(aos_io_can_t *c, aos_can_status_t *st)
{
    (void)c;
    st->state = "active";
    return true;
}

bool aos_io_be_can_recover(aos_io_can_t *c) { (void)c; return true; }

void aos_io_be_can_close(aos_io_can_t *c)
{
    can_sim_t *n = c->be;
    if (!n) return;
    pthread_mutex_lock(&s_mx);
    for (can_sim_t **pp = &s_can; *pp; pp = &(*pp)->next) if (*pp == n) { *pp = n->next; break; }
    pthread_mutex_unlock(&s_mx);
    pthread_cond_destroy(&n->cv);
    free(n);
    c->be = NULL;
}
