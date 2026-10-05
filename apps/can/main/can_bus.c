/*
 * P4OS - CAN: the bus thread and the file thread (can.h says who does what).
 *
 * The receive queue of aos_io_can holds 64 frames and drops the oldest, so
 * the bus thread does nothing slow: it waits on the queue, takes what is
 * there in one go and files it under the lock. A send nobody acknowledges
 * blocks it for its timeout; five in a row stop the periodic sends and the
 * replay, so a bus with no other node does not hold it forever.
 *
 * The card is the file thread's: the recording is written from the ring a
 * few times a second, behind the frames, so a slow write costs nothing but
 * a little lag (and frames, only if it falls a whole ring behind: they are
 * counted). The portal page talks to it through <data>/can_cmd.txt, which
 * the page writes and this thread reads and deletes, and
 * <data>/can_live.json, which this thread writes once a second while the
 * page has said, in the last 15 s, that it is looking. The commands, a line
 * each: "watch", "send <candump frame>", "rec on", "rec off", "clear", and
 * "reload" (enviar.txt or senales.dbc changed).
 */
#include "can.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

#define OWNER          "CAN"
#define RECV_WAIT_MS   5
#define SEND_WAIT_MS   20
#define STATUS_MS      200
#define FAILS_TO_STOP  5
#define WEB_WATCH_MS   15000

cn_bus_t CB;

static void *s_mx;
static aos_io_can_t *s_can;
static volatile bool s_bus_stop, s_bus_done = true;
static volatile bool s_io_stop, s_io_done = true;
static int s_fails;

void cn_lock(void) { aos_hal_mutex_lock(s_mx); }
void cn_unlock(void) { aos_hal_mutex_unlock(s_mx); }
uint64_t cn_us(void) { return aos_hal_uptime_us(); }
uint32_t cn_ms(void) { return (uint32_t)(aos_hal_uptime_us() / 1000); }

static void *big_alloc(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) {
        memset(p, 0, n);
        return p;
    }
#endif
    return calloc(1, n);
}

static void big_free(void *p)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    heap_caps_free(p);
#else
    free(p);
#endif
}

const char *cn_dir(void)
{
    static char dir[96];
    const char *root = aos_hal_path_sd_root();
    snprintf(dir, sizeof dir, "%s/can", root ? root : aos_hal_path_data());
    mkdir(dir, 0777);
    return dir;
}

/* ---- the ring and the table of ids (with the lock held) ---- */

static uint32_t frame_bits(const cn_frame_t *f)
{
    /* the frame on the wire, with a fifth of stuff bits and the 3-bit gap */
    uint32_t bits = (f->fl & CN_EXT ? 67u : 47u) + (f->fl & CN_RTR ? 0u : 8u * f->len);
    return bits + bits / 5 + 3;
}

static uint32_t s_bits_acc, s_frames_acc;

static void id_update(const cn_frame_t *f)
{
    uint8_t ext = f->fl & CN_EXT;
    cn_id_t *e = NULL;
    for (int i = 0; i < CB.nids; i++)
        if (CB.ids[i].id == f->id && CB.ids[i].fl == ext) { e = &CB.ids[i]; break; }
    uint32_t now_ms = (uint32_t)(f->t_us / 1000);
    if (!e) {
        if (CB.nids >= CN_IDS) return;
        e = &CB.ids[CB.nids++];
        memset(e, 0, sizeof *e);
        e->id = f->id;
        e->fl = ext;
        e->len = f->len;
        memcpy(e->data, f->data, 8);
        e->count = 1;
        e->last_us = f->t_us;
        CB.ids_gen++;
        return;
    }
    if (!(f->fl & CN_RTR)) {
        for (int i = 0; i < 8; i++) {
            uint8_t x = (uint8_t)(e->data[i] ^ (i < f->len ? f->data[i] : 0));
            if (i >= f->len && i >= e->len) continue;
            if (x || (i < f->len) != (i < e->len)) {
                e->changed[i] |= x;
                e->chg_ms[i] = now_ms ? now_ms : 1;
            }
        }
        memcpy(e->data, f->data, 8);
        e->len = f->len;
    }
    e->count++;
    e->period_us = (uint32_t)(f->t_us - e->last_us);
    e->last_us = f->t_us;
}

static void ring_put(const cn_frame_t *f)
{
    CB.ring[CB.seq % CN_RING] = *f;
    CB.seq++;
    id_update(f);
    s_frames_acc++;
    s_bits_acc += frame_bits(f);
}

void cn_bus_clear(void)
{
    cn_lock();
    CB.seq = 0;
    CB.rec_seq = 0;
    CB.nids = 0;
    CB.ids_gen++;
    cn_unlock();
}

/* ---- the bus thread ---- */

static void to_cn(const aos_can_frame_t *a, cn_frame_t *f)
{
    memset(f, 0, sizeof *f);
    f->t_us = a->t_us;
    f->id = a->id;
    f->len = a->len > 8 ? 8 : a->len;
    f->fl = (a->ext ? CN_EXT : 0) | (a->rtr ? CN_RTR : 0);
    if (!a->rtr) memcpy(f->data, a->data, f->len);
}

static void stop_sending_locked(void)
{
    for (int i = 0; i < CN_PERIODIC; i++) CB.per[i].on = false;
    for (int i = 0; i < CN_SAVED_MAX; i++) CN_SAVED[i].running = false;
    CB.replay = false;
}

static void send_now(const cn_frame_t *f)
{
    aos_can_frame_t a = { .id = f->id, .ext = f->fl & CN_EXT, .rtr = f->fl & CN_RTR, .len = f->len };
    memcpy(a.data, f->data, 8);
    bool ok = aos_io_can_send(s_can, &a, SEND_WAIT_MS);
    cn_lock();
    if (ok) {
        s_fails = 0;
        /* in the self test the frame comes back by itself */
        if (CB.cfg.mode != CN_MODE_SELFTEST) {
            cn_frame_t t = *f;
            t.t_us = cn_us();
            t.fl |= CN_TX;
            ring_put(&t);
        }
    } else {
        CB.tx_fail++;
        if (++s_fails >= FAILS_TO_STOP) stop_sending_locked();
    }
    cn_unlock();
}

static void bus_main(void *arg)
{
    (void)arg;
    aos_can_frame_t a;
    cn_frame_t batch[32], out[CN_TXQ + CN_PERIODIC + 16];
    uint64_t next_status = 0, rate_t = cn_us();
    while (!s_bus_stop) {
        int n = 0;
        int r = aos_io_can_recv(s_can, &a, RECV_WAIT_MS);
        while (r > 0) {
            to_cn(&a, &batch[n++]);
            if (n == (int)(sizeof batch / sizeof batch[0])) break;
            r = aos_io_can_recv(s_can, &a, 0);
        }
        if (r < 0) break;
        uint64_t now = cn_us();
        int no = 0;
        bool recover = false;
        cn_lock();
        for (int i = 0; i < n; i++) ring_put(&batch[i]);
        /* what to send: the UI's queue, the periodic ones that are due, the replay's */
        for (int i = 0; i < CB.ntxq; i++) out[no++] = CB.txq[i];
        CB.ntxq = 0;
        for (int i = 0; i < CN_PERIODIC; i++) {
            if (!CB.per[i].on || now < CB.per[i].next_us) continue;
            out[no++] = CB.per[i].f;
            uint64_t p = (uint64_t)CB.per[i].period_ms * 1000;
            CB.per[i].next_us += p;
            if (CB.per[i].next_us < now) CB.per[i].next_us = now + p;   /* fell behind: no burst */
        }
        while (CB.replay && CB.rq_n && CB.rq[CB.rq_head].t_us <= now && no < (int)(sizeof out / sizeof out[0])) {
            out[no++] = CB.rq[CB.rq_head];
            CB.rq_head = (CB.rq_head + 1) % CN_REPLAYQ;
            CB.rq_n--;
            CB.replay_sent++;
        }
        recover = CB.want_recover;
        CB.want_recover = false;
        if (now - rate_t >= 1000000) {
            float dt = (float)(uint32_t)(now - rate_t) / 1e6f;
            for (int i = 0; i < CB.nids; i++) {
                cn_id_t *e = &CB.ids[i];
                e->hz = (float)(e->count - e->count_mark) / dt;
                e->count_mark = e->count;
            }
            CB.fps = (float)s_frames_acc / dt;
            CB.load = (float)s_bits_acc / dt / (float)CB.cfg.rate;
            s_frames_acc = s_bits_acc = 0;
            rate_t = now;
        }
        cn_unlock();

        for (int i = 0; i < no && !s_bus_stop; i++) send_now(&out[i]);
        if (recover) aos_io_can_recover(s_can);
        if (now >= next_status) {
            aos_can_status_t st;
            if (aos_io_can_status(s_can, &st)) {
                cn_lock();
                CB.st = st;
                snprintf(CB.state, sizeof CB.state, "%s", st.state ? st.state : "?");
                cn_unlock();
            }
            next_status = now + STATUS_MS * 1000;
        }
    }
    s_bus_done = true;
}

const char *cn_bus_open(const cn_cfg_t *cfg)
{
    static char why[96];
    if (s_can) return NULL;
    int rx = cfg->rx;
    if (cfg->mode == CN_MODE_SELFTEST && rx < 0) rx = cfg->tx;
    if (cfg->tx == rx && cfg->mode != CN_MODE_SELFTEST) return _("Un solo pin para TX y RX sirve sólo en la autoprueba");
    for (int k = 0; k < 2; k++) {
        int g = k ? rx : cfg->tx;
        const char *o = aos_io_owner(g);
        if (o && strcmp(o, OWNER)) {
            snprintf(why, sizeof why, _("El GPIO%d lo tiene %s"), g, o);
            return why;
        }
    }
    static const aos_can_mode_t MODE[CN_MODE_COUNT] = { AOS_CAN_LISTEN, AOS_CAN_NORMAL, AOS_CAN_SELFTEST };
    s_can = aos_io_can_open(cfg->tx, rx, cfg->rate, MODE[cfg->mode], OWNER);
    if (!s_can) return _("El controlador no arrancó: mirá el registro");
    if (cfg->fkind != CN_FILT_NONE) aos_io_can_filter(s_can, cfg->fid, cfg->fmask, cfg->fkind == CN_FILT_EXT);

    cn_lock();
    CB.cfg = *cfg;
    CB.cfg.rx = rx;
    CB.seq = CB.rec_seq = 0;
    CB.nids = 0;
    CB.ids_gen++;
    CB.ntxq = 0;
    CB.tx_fail = 0;
    CB.fps = CB.load = 0;
    memset(&CB.st, 0, sizeof CB.st);
    snprintf(CB.state, sizeof CB.state, "active");
    CB.t0_us = cn_us();
    CB.open = true;
    s_fails = 0;
    s_frames_acc = s_bits_acc = 0;
    cn_unlock();

    s_bus_stop = false;
    s_bus_done = false;
    if (!aos_hal_thread_start("can_bus", bus_main, NULL, 6144, 3)) {
        s_bus_done = true;
        cn_bus_close();
        return _("No hay lugar para otra tarea");
    }
    aos_hal_log("can", "open GPIO%d/%d at %u, mode %d", cfg->tx, rx, (unsigned)cfg->rate, (int)cfg->mode);
    return NULL;
}

void cn_bus_close(void)
{
    if (!s_can) return;
    cn_rec_stop();
    cn_lock();
    stop_sending_locked();
    cn_unlock();
    s_bus_stop = true;
    for (int w = 0; !s_bus_done && w < 400; w++) aos_hal_sleep_ms(5);
    if (!s_bus_done) {
        /* a thread still in the driver: leave the handle to it rather than
         * free it under its feet */
        aos_hal_log("can", "the bus thread did not stop; the port stays open");
        return;
    }
    aos_io_can_close(s_can);
    s_can = NULL;
    cn_lock();
    CB.open = false;
    snprintf(CB.state, sizeof CB.state, "%s", "");
    cn_unlock();
    aos_hal_log("can", "closed");
}

bool cn_bus_send(const cn_frame_t *f)
{
    bool ok = false;
    cn_lock();
    if (CB.open && CB.cfg.mode != CN_MODE_LISTEN && CB.ntxq < CN_TXQ) {
        CB.txq[CB.ntxq++] = *f;
        s_fails = 0;
        ok = true;
    }
    cn_unlock();
    return ok;
}

void cn_periodic_set(int slot, const cn_frame_t *f, uint32_t period_ms, bool on)
{
    cn_lock();
    int free_i = -1, at = -1;
    for (int i = 0; i < CN_PERIODIC; i++) {
        if (CB.per[i].on && CB.per[i].slot == slot) at = i;
        else if (!CB.per[i].on && free_i < 0) free_i = i;
    }
    if (!on) {
        if (at >= 0) CB.per[at].on = false;
    } else {
        if (at < 0) at = free_i;
        if (at >= 0 && period_ms) {
            CB.per[at].f = *f;
            CB.per[at].period_ms = period_ms;
            CB.per[at].next_us = cn_us();
            CB.per[at].slot = slot;
            CB.per[at].on = true;
            s_fails = 0;
        }
    }
    cn_unlock();
}

void cn_periodic_stop_all(void)
{
    cn_lock();
    for (int i = 0; i < CN_PERIODIC; i++) CB.per[i].on = false;
    for (int i = 0; i < CN_SAVED_MAX; i++) CN_SAVED[i].running = false;
    cn_unlock();
}

/* ---- recording and replay: asked for here, done by the file thread ---- */

static FILE *s_rec_f, *s_rp_f;
static char s_rec_path[160], s_rp_path[160];
static uint32_t s_rec_stop_seq;
static bool s_rp_open_req;
static uint64_t s_rp_t0, s_rp_first, s_rp_last;
static bool s_rp_have_first;

bool cn_rec_start(void)
{
    char name[48];
    if (aos_hal_time_is_valid()) {
        struct tm t;
        aos_hal_time_now(&t);
        snprintf(name, sizeof name, "can-%04d%02d%02d-%02d%02d%02d.csv", t.tm_year + 1900, t.tm_mon + 1,
                 t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        /* no clock yet: the first free number */
        struct stat sb;
        for (int i = 1; i < 1000; i++) {
            snprintf(name, sizeof name, "can-%03d.csv", i);
            snprintf(s_rec_path, sizeof s_rec_path, "%s/%s", cn_dir(), name);
            if (stat(s_rec_path, &sb) != 0) break;
        }
    }
    cn_lock();
    bool ok = CB.open && !CB.rec && !s_rec_f;
    if (ok) {
        snprintf(s_rec_path, sizeof s_rec_path, "%s/%s", cn_dir(), name);
        snprintf(CB.rec_name, sizeof CB.rec_name, "%s", name);
        CB.rec_seq = CB.seq;
        CB.rec_frames = CB.rec_lost = 0;
        CB.rec_bytes = 0;
        CB.rec_t0 = cn_us();
        CB.rec = true;
    }
    cn_unlock();
    return ok;
}

void cn_rec_stop(void)
{
    cn_lock();
    if (CB.rec) s_rec_stop_seq = CB.seq;
    CB.rec = false;
    cn_unlock();
}

bool cn_replay_start(const char *name, bool loop)
{
    cn_lock();
    bool ok = CB.open && CB.cfg.mode != CN_MODE_LISTEN && !CB.replay && !s_rp_f;
    if (ok) {
        snprintf(s_rp_path, sizeof s_rp_path, "%s/%s", cn_dir(), name);
        snprintf(CB.replay_name, sizeof CB.replay_name, "%s", name);
        CB.replay_loop = loop;
        CB.replay_sent = 0;
        CB.rq_head = CB.rq_n = 0;
        CB.rq_eof = false;
        CB.replay = true;
        s_rp_open_req = true;
        s_fails = 0;
    }
    cn_unlock();
    return ok;
}

void cn_replay_stop(void)
{
    cn_lock();
    CB.replay = false;
    CB.rq_n = 0;
    cn_unlock();
}

/* ---- the file thread ---- */

static cn_frame_t s_wbuf[256];
static char s_line[160];

static int csv_line(const cn_frame_t *f, uint64_t t0, char *out, int cap)
{
    /* SavvyCAN's GVRET CSV: Time Stamp,ID,Extended,Dir,Bus,LEN,D1..D8 */
    int n = snprintf(out, cap, "%llu,%08X,%s,%s,0,%u", (unsigned long long)(f->t_us > t0 ? f->t_us - t0 : 0),
                     (unsigned)f->id, f->fl & CN_EXT ? "true" : "false", f->fl & CN_TX ? "Tx" : "Rx", f->len);
    for (int i = 0; i < f->len && !(f->fl & CN_RTR) && n < cap - 4; i++) n += snprintf(out + n, cap - n, ",%02X", f->data[i]);
    if (n < cap - 1) out[n++] = '\n';
    out[n] = 0;
    return n;
}

static void rec_service(void)
{
    if (!s_rec_f) {
        cn_lock();
        bool want = CB.rec;
        cn_unlock();
        if (!want) return;
        s_rec_f = fopen(s_rec_path, "w");
        if (!s_rec_f) {
            cn_lock();
            CB.rec = false;
            snprintf(CB.rec_name, sizeof CB.rec_name, "%s", "");
            cn_unlock();
            aos_hal_log("can", "cannot write %s", s_rec_path);
            return;
        }
        fputs("Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8\n", s_rec_f);
    }
    for (;;) {
        cn_lock();
        uint32_t end = CB.rec ? CB.seq : s_rec_stop_seq;
        uint32_t from = CB.rec_seq;
        if ((int32_t)(end - from) < 0) from = end;
        if (end - from > CN_RING) {
            CB.rec_lost += end - from - CN_RING;
            from = end - CN_RING;
        }
        uint32_t n = end - from;
        if (n > 256) n = 256;
        for (uint32_t i = 0; i < n; i++) s_wbuf[i] = CB.ring[(from + i) % CN_RING];
        CB.rec_seq = from + n;
        uint64_t t0 = CB.rec_t0;
        bool done = !CB.rec && CB.rec_seq == end;
        cn_unlock();
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < n; i++) {
            int k = csv_line(&s_wbuf[i], t0, s_line, sizeof s_line);
            fputs(s_line, s_rec_f);
            bytes += (uint64_t)k;
        }
        cn_lock();
        CB.rec_frames += n;
        CB.rec_bytes += bytes;
        cn_unlock();
        if (done) {
            fclose(s_rec_f);
            s_rec_f = NULL;
            aos_hal_log("can", "recorded %s", s_rec_path);
            return;
        }
        if (n < 256) break;
    }
}

/* one line of a recording: ours (GVRET) or anything with the same columns */
static bool csv_parse(char *s, uint64_t *ts, cn_frame_t *f)
{
    char *p = s, *e;
    memset(f, 0, sizeof *f);
    /* the time stamp, in microseconds (strtoull is not in the firmware's table) */
    uint64_t t = 0;
    for (e = p; *e >= '0' && *e <= '9'; e++) t = t * 10 + (uint64_t)(*e - '0');
    if (e == p || *e != ',') return false;
    *ts = t;
    p = e + 1;
    f->id = (uint32_t)strtoul(p, &e, 16);
    if (e == p || *e != ',') return false;
    p = e + 1;
    if (!strncasecmp(p, "true", 4) || *p == '1') f->fl |= CN_EXT;
    p = strchr(p, ',');                 /* Dir */
    if (!p) return false;
    p = strchr(p + 1, ',');             /* Bus */
    if (!p) return false;
    p = strchr(p + 1, ',');             /* LEN */
    if (!p) return false;
    long len = strtol(p + 1, &e, 10);
    if (e == p + 1 || len < 0 || len > 8) return false;
    f->len = (uint8_t)len;
    p = e;
    for (int i = 0; i < f->len; i++) {
        if (*p != ',') return false;
        f->data[i] = (uint8_t)strtoul(p + 1, &e, 16);
        p = e;
    }
    if (f->id > (f->fl & CN_EXT ? 0x1FFFFFFFu : 0x7FFu)) f->fl |= CN_EXT;
    return f->id <= 0x1FFFFFFFu;
}

static void replay_service(void)
{
    cn_lock();
    bool want = CB.replay, open_req = s_rp_open_req;
    s_rp_open_req = false;
    cn_unlock();
    if (!want) {
        if (s_rp_f) { fclose(s_rp_f); s_rp_f = NULL; }
        return;
    }
    if (open_req || !s_rp_f) {
        if (s_rp_f) fclose(s_rp_f);
        s_rp_f = fopen(s_rp_path, "r");
        if (!s_rp_f) { cn_replay_stop(); return; }
        s_rp_t0 = cn_us() + 200000;
        s_rp_have_first = false;
    }
    for (;;) {
        cn_lock();
        bool room = CB.replay && CB.rq_n < CN_REPLAYQ;
        cn_unlock();
        if (!room) break;
        if (!fgets(s_line, sizeof s_line, s_rp_f)) {
            cn_lock();
            bool loop = CB.replay_loop;
            bool drained = CB.rq_n == 0;
            cn_unlock();
            if (loop) {
                rewind(s_rp_f);
                s_rp_t0 = s_rp_last + 100000;     /* a pause between the rounds */
                s_rp_have_first = false;
                continue;
            }
            if (drained) cn_replay_stop();
            break;
        }
        uint64_t ts;
        cn_frame_t f;
        if (!csv_parse(s_line, &ts, &f)) continue;
        if (!s_rp_have_first) { s_rp_first = ts; s_rp_have_first = true; }
        f.t_us = s_rp_t0 + (ts - s_rp_first);
        s_rp_last = f.t_us;
        cn_lock();
        CB.rq[(CB.rq_head + CB.rq_n) % CN_REPLAYQ] = f;
        CB.rq_n++;
        cn_unlock();
    }
}

/* ---- the portal page ---- */

static void data_path(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", aos_hal_path_data(), name);
}

static void web_commands(void)
{
    char path[128];
    data_path(path, sizeof path, "can_cmd.txt");
    FILE *f = fopen(path, "r");
    if (!f) return;
    char lines[16][64];
    int n = 0;
    while (n < 16 && fgets(lines[n], sizeof lines[n], f)) n++;
    fclose(f);
    unlink(path);
    for (int i = 0; i < n; i++) {
        char *s = lines[i];
        s[strcspn(s, "\r\n")] = 0;
        if (!strcmp(s, "watch")) {
            cn_lock();
            CB.web_until_ms = (cn_ms() + WEB_WATCH_MS) | 1;     /* 0 means "nobody looks" */
            cn_unlock();
        } else if (!strncmp(s, "send ", 5)) {
            cn_frame_t fr;
            if (cn_parse_candump(s + 5, &fr)) cn_bus_send(&fr);
        } else if (!strcmp(s, "rec on")) {
            cn_rec_start();
        } else if (!strcmp(s, "rec off")) {
            cn_rec_stop();
        } else if (!strcmp(s, "clear")) {
            cn_bus_clear();
        } else if (!strcmp(s, "reload")) {
            cn_lock();
            CB.want_reload = true;      /* the UI's lists: the UI does it */
            cn_unlock();
        }
    }
}

#define LIVE_MAX (24 * 1024)
static char *s_json;

static void jcat(int *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jcat(int *n, const char *fmt, ...)
{
    if (*n >= LIVE_MAX - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(s_json + *n, LIVE_MAX - *n, fmt, ap);
    va_end(ap);
    if (k > 0) *n = *n + k < LIVE_MAX - 1 ? *n + k : LIVE_MAX - 1;
}

static void web_snapshot(void)
{
    static const char *const MODE[CN_MODE_COUNT] = { "listen", "normal", "selftest" };
    static uint32_t count;
    static cn_frame_t last[32];
    static cn_id_t ids[CN_IDS];
    if (!s_json && !(s_json = big_alloc(LIVE_MAX))) return;
    int n = 0, nids, nlast = 0;
    uint32_t now_ms = cn_ms();
    static cn_bus_t b;          /* static: it is 7 KB, the stack is 6 */
    cn_lock();
    b = CB;
    nids = CB.nids;
    memcpy(ids, CB.ids, sizeof(cn_id_t) * (size_t)nids);
    uint32_t avail = CB.seq < 32 ? CB.seq : 32;
    for (uint32_t i = 0; i < avail; i++) last[nlast++] = CB.ring[(CB.seq - avail + i) % CN_RING];
    cn_unlock();
    jcat(&n, "{\"v\":1,\"n\":%u,\"ms\":%u,\"open\":%s", (unsigned)++count, (unsigned)now_ms, b.open ? "true" : "false");
    if (b.open) {
        jcat(&n, ",\"mode\":\"%s\",\"rate\":%u,\"tx\":%d,\"rx\":%d,\"state\":\"%s\"", MODE[b.cfg.mode],
             (unsigned)b.cfg.rate, b.cfg.tx, b.cfg.rx, b.state);
        jcat(&n, ",\"txe\":%u,\"rxe\":%u,\"buserr\":%u,\"rxn\":%u,\"txn\":%u,\"drop\":%u,\"txfail\":%u",
             b.st.tx_errors, b.st.rx_errors, (unsigned)b.st.bus_errors, (unsigned)b.st.received,
             (unsigned)b.st.sent, (unsigned)b.st.dropped, (unsigned)b.tx_fail);
        jcat(&n, ",\"fps\":%.1f,\"load\":%.3f", (double)b.fps, (double)b.load);
    }
    jcat(&n, ",\"rec\":\"%s\",\"recn\":%u,\"replay\":\"%s\"", b.rec ? b.rec_name : "", (unsigned)b.rec_frames,
         b.replay ? b.replay_name : "");
    jcat(&n, ",\"ids\":[");
    for (int i = 0; i < nids; i++) {
        cn_frame_t f = { .id = ids[i].id, .fl = ids[i].fl, .len = ids[i].len };
        memcpy(f.data, ids[i].data, 8);
        char id[12], data[32];
        cn_fmt_id(&f, id, sizeof id);
        cn_fmt_data(&f, data, sizeof data);
        unsigned recent = 0;
        for (int k = 0; k < 8; k++)
            if (ids[i].chg_ms[k] && now_ms - ids[i].chg_ms[k] < 1500) recent |= 1u << k;
        jcat(&n, "%s[\"%s\",%d,%u,\"%s\",%u,%.1f,%u]", i ? "," : "", id, ids[i].fl & CN_EXT ? 1 : 0, ids[i].len,
             data, recent, (double)ids[i].hz, (unsigned)ids[i].count);
    }
    jcat(&n, "],\"last\":[");
    for (int i = 0; i < nlast; i++) {
        char id[12], data[32];
        cn_fmt_id(&last[i], id, sizeof id);
        cn_fmt_data(&last[i], data, sizeof data);
        unsigned long long t = last[i].t_us > b.t0_us ? (last[i].t_us - b.t0_us) / 1000 : 0;
        jcat(&n, "%s[%llu,\"%s\",%d,\"%s\"]", i ? "," : "", t, id, last[i].fl, data);
    }
    jcat(&n, "]}\n");

    char path[128], part[136];
    data_path(path, sizeof path, "can_live.json");
    snprintf(part, sizeof part, "%s.part", path);
    FILE *f = fopen(part, "w");
    if (!f) return;
    fwrite(s_json, 1, (size_t)n, f);
    fclose(f);
    unlink(path);           /* FAT's rename does not replace */
    rename(part, path);
}

static void io_main(void *arg)
{
    (void)arg;
    /* deadlines from now: the simulator's clock is the Mac's, past 2^31 ms */
    uint32_t next_cmd = cn_ms(), next_web = next_cmd, next_flush = next_cmd;
    mkdir(aos_hal_path_data(), 0777);
    while (!s_io_stop) {
        aos_hal_sleep_ms(50);
        rec_service();
        replay_service();
        uint32_t now = cn_ms();
        if (s_rec_f && (int32_t)(now - next_flush) >= 0) {
            fflush(s_rec_f);
            next_flush = now + 1000;
        }
        if ((int32_t)(now - next_cmd) >= 0) {
            web_commands();
            next_cmd = now + 1000;
        }
        cn_lock();
        bool web = CB.web_until_ms && (int32_t)(CB.web_until_ms - now) > 0;
        cn_unlock();
        if (web && (int32_t)(now - next_web) >= 0) {
            web_snapshot();
            next_web = now + 1000;
        }
    }
    /* the app is closing: the bus is already shut, the recording ends here */
    cn_rec_stop();
    if (s_rec_f) rec_service();
    if (s_rec_f) { fclose(s_rec_f); s_rec_f = NULL; }
    if (s_rp_f) { fclose(s_rp_f); s_rp_f = NULL; }
    s_io_done = true;
}

void cn_bus_init(void)
{
    if (!s_mx) s_mx = aos_hal_mutex_create();
    if (!CB.ring) CB.ring = big_alloc(sizeof(cn_frame_t) * CN_RING);
    if (!CB.ids) CB.ids = big_alloc(sizeof(cn_id_t) * CN_IDS);
    if (s_io_done) {
        s_io_stop = false;
        s_io_done = false;
        if (!aos_hal_thread_start("can_io", io_main, NULL, 6144, 2)) s_io_done = true;
    }
}

void cn_bus_deinit(void)
{
    cn_bus_close();
    s_io_stop = true;
    for (int w = 0; !s_io_done && w < 600; w++) aos_hal_sleep_ms(5);
    if (!s_io_done) return;          /* stuck on the card: keep its buffers */
    cn_lock();
    big_free(CB.ring);
    big_free(CB.ids);
    CB.ring = NULL;
    CB.ids = NULL;
    CB.nids = 0;
    CB.seq = 0;
    cn_unlock();
    if (s_json) { big_free(s_json); s_json = NULL; }
}
