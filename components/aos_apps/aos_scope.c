/*
 * P4OS - the bench's oscilloscope service: a Rigol DS1000Z over its raw SCPI
 * socket. The contract is in aos_scope.h; this is how it keeps it.
 *
 * One thread owns the socket and does all the talking, in turns:
 *
 *   1. the queued commands, each one also written into the status at once
 *      (aos_scope_cmd), so a second tap on "+" steps from the value the
 *      first one asked for; what they touch is read back right after;
 *   2. the screenshot, when asked: :DISP:DATA? ON,0,PNG streamed into
 *      <data>/scope.png through a small buffer, never held whole;
 *   3. while someone watches (aos_scope_live), a set of traces: for each
 *      channel that is on, :WAV:SOUR, :WAV:PRE? and :WAV:DATA? (screen
 *      mode, BYTE, ~1200 points, V = (byte - Yorigin - Yreference) * Yinc),
 *      then :TRIG:STAT?; as fast as the scope answers, but no closer than
 *      SET_MIN_MS apart, and only twice a second while it is stopped;
 *   4. in the time left, a few items of two rounds: the settings (V/div,
 *      offset, probe, coupling of each channel, timebase, trigger) every
 *      second while live and every two otherwise, and the measurements
 *      (:MEAS:ITEM? VPP..PER) of the channels that are on, every second,
 *      live or not - the Registrador samples them with nobody watching.
 *
 * The answers are lines, or IEEE 488.2 blocks "#N<len><data>\n". Anything
 * wrong - a timeout included, since a late answer would desynchronise every
 * answer after it - drops the connection, and it starts over after a pause
 * that grows to 8 s. The real scope does not answer a query it does not
 * know: a timeout is how that shows.
 *
 * Everything the thread leaves for others is written under the mutex.
 */
#include "aos_scope.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define DEFAULT_PORT   5555
#define Q_LEN          16
#define CMD_MAX        80
#define RX_SIZE        2048
#define SET_MIN_MS     60           /* at most ~16 trace sets a second */
#define SET_STOP_MS    500          /* stopped: the picture only changes when someone turns a knob */
#define MEAS_EVERY_MS  1000
#define Q_TIMEOUT_MS   2500
#define BLOCK_TIMEOUT  4000
#define SHOT_TIMEOUT   15000        /* the DS1000Z takes seconds to start sending its PNG */

static const char *const MEAS_NAME[AOS_SCOPE_M_COUNT] = { "VPP", "VMAX", "VMIN", "VAVG", "VRMS", "FREQ", "PER" };

/* The settings, as items of a round. */
enum { IT_DISP, IT_SCAL, IT_OFFS, IT_PROB, IT_COUP, IT_PER_CH };
#define IT_TIM    (AOS_SCOPE_CHANNELS * IT_PER_CH)
#define IT_TLEV   (IT_TIM + 1)
#define IT_TSRC   (IT_TIM + 2)
#define IT_TSLP   (IT_TIM + 3)
#define IT_TSTAT  (IT_TIM + 4)
#define IT_COUNT  (IT_TIM + 5)
#define IT_ALL    ((1u << IT_COUNT) - 1)
#define IT_CH(n)  (((1u << IT_PER_CH) - 1) << ((n) * IT_PER_CH))
#define IT_TRIG   ((1u << IT_TLEV) | (1u << IT_TSRC) | (1u << IT_TSLP) | (1u << IT_TSTAT))

static struct {
    void *mx;
    bool started;
    /* what is asked */
    char host[64];
    int port;
    bool want;
    volatile uint32_t gen;          /* bumps with every connect / disconnect */
    bool live;
    uint32_t live_until;            /* aos_scope_live_for: traces until then too */
    char q[Q_LEN][CMD_MAX];
    int qh, qn;
    bool shot_req;
    bool kick;                      /* someone started watching: a set at once */
    /* what the thread found */
    aos_scope_status_t st;
    float *wave;                    /* AOS_SCOPE_CHANNELS x AOS_SCOPE_POINTS */
    int wave_n[AOS_SCOPE_CHANNELS];
    float wave_xinc[AOS_SCOPE_CHANNELS];
    float meas[AOS_SCOPE_CHANNELS][AOS_SCOPE_M_COUNT];
    uint32_t meas_ms[AOS_SCOPE_CHANNELS][AOS_SCOPE_M_COUNT];
    bool meas_ok[AOS_SCOPE_CHANNELS][AOS_SCOPE_M_COUNT];
    uint32_t shot_seq;
    bool shot_busy;
    char shot_path[160];
    uint32_t set_t[8];              /* when the last trace sets came in, for the rate */
    int set_n;
} S;

/* The thread's own. */
static struct {
    int h;
    uint32_t gen;
    uint8_t rx[RX_SIZE];
    int rx_len, rx_pos;
    uint8_t raw[AOS_SCOPE_CHANNELS][AOS_SCOPE_POINTS];
    int raw_n[AOS_SCOPE_CHANNELS];
    float yinc[AOS_SCOPE_CHANNELS], yorg[AOS_SCOPE_CHANNELS], yref[AOS_SCOPE_CHANNELS], xinc[AOS_SCOPE_CHANNELS];
    char err[96];
    uint32_t dirty;                 /* settings items to read back at once */
    int set_next;                   /* the settings round: next item, IT_COUNT = idle */
    uint32_t set_round_ms;
    int meas_next;                  /* the measurements round, over ch * M_COUNT */
    uint32_t meas_round_ms;
    uint32_t last_set_ms;
    bool force_set;                 /* a command went out: the next set now */
} T;
/* Not initialised in place: that kept its 7 KB in internal RAM (.data), where
 * the linker fragment (aos_hal/psram.lf) cannot move it. PSRAM .bss starts
 * zeroed, and the fields that are not 0 are set before app_main. */
__attribute__((constructor)) static void T_defaults(void) { T.set_next = IT_COUNT; T.meas_next = -1; }

static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }
static void lock(void) { aos_hal_mutex_lock(S.mx); }
static void unlock(void) { aos_hal_mutex_unlock(S.mx); }

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(T.err, sizeof T.err, fmt, ap);
    va_end(ap);
}

/* The connection is still the one asked for. */
static bool wanted(void)
{
    lock();
    bool ok = S.want && S.gen == T.gen;
    unlock();
    return ok;
}

/* -------------------------------------------------------------------------- */
/* SCPI words                                                                  */
/* -------------------------------------------------------------------------- */

/* A header word matches its short form, its long form, or anything between
 * ("TIMEBASE", "TIM", "TIMEB" is not SCPI but harmless). Upper case in. */
static bool word_is(const char *w, size_t len, const char *shrt, const char *lng)
{
    size_t ls = strlen(shrt);
    return len >= ls && len <= strlen(lng) && !strncmp(w, lng, len);
}

/* ":CHAN1:SCAL 0.5" -> which settings item it sets (-1: none), the channel,
 * and the argument. */
static int setter_item(const char *cmd, int *ch, char *arg, size_t arg_len)
{
    char up[CMD_MAX];
    size_t n = 0;
    while (cmd[n] && n < sizeof up - 1) { up[n] = (char)toupper((unsigned char)cmd[n]); n++; }
    up[n] = 0;
    char *sp = strchr(up, ' ');
    snprintf(arg, arg_len, "%s", sp ? sp + 1 : "");
    if (sp) *sp = 0;
    if (strchr(up, '?')) return -1;
    const char *w[5];
    size_t wl[5];
    int nw = 0;
    for (char *p = up; *p && nw < 5; ) {
        while (*p == ':') p++;
        if (!*p) break;
        w[nw] = p;
        while (*p && *p != ':') p++;
        wl[nw] = (size_t)(p - w[nw]);
        nw++;
    }
    *ch = 0;
    if (nw == 2 && wl[0] >= 5 && isdigit((unsigned char)w[0][wl[0] - 1]) && word_is(w[0], wl[0] - 1, "CHAN", "CHANNEL")) {
        int c = w[0][wl[0] - 1] - '1';
        if (c < 0 || c >= AOS_SCOPE_CHANNELS) return -1;
        *ch = c;
        static const char *const SH[IT_PER_CH] = { "DISP", "SCAL", "OFFS", "PROB", "COUP" };
        static const char *const LG[IT_PER_CH] = { "DISPLAY", "SCALE", "OFFSET", "PROBE", "COUPLING" };
        for (int i = 0; i < IT_PER_CH; i++) if (word_is(w[1], wl[1], SH[i], LG[i])) return c * IT_PER_CH + i;
        return -1;
    }
    if (nw >= 2 && word_is(w[0], wl[0], "TIM", "TIMEBASE")) {
        int k = 1;
        if (nw == 3 && word_is(w[1], wl[1], "MAIN", "MAIN")) k = 2;
        if (k == nw - 1 && word_is(w[k], wl[k], "SCAL", "SCALE")) return IT_TIM;
        return -1;
    }
    if (nw == 3 && word_is(w[0], wl[0], "TRIG", "TRIGGER") && word_is(w[1], wl[1], "EDG", "EDGE")) {
        if (word_is(w[2], wl[2], "LEV", "LEVEL")) return IT_TLEV;
        if (word_is(w[2], wl[2], "SOUR", "SOURCE")) return IT_TSRC;
        if (word_is(w[2], wl[2], "SLOP", "SLOPE")) return IT_TSLP;
    }
    return -1;
}

static int parse_source(const char *s)
{
    /* "CHAN1" (the scope's answer), "CHANNEL2", "chan3" */
    size_t l = strlen(s);
    if (l >= 5 && !strncasecmp(s, "CHAN", 4) && isdigit((unsigned char)s[l - 1])) {
        int n = s[l - 1] - '0';
        return n >= 1 && n <= AOS_SCOPE_CHANNELS ? n : 0;
    }
    return 0;
}

/* Writes what an item's answer (or a setter's argument) says into the status. Locked. */
static void apply_item(int it, const char *v)
{
    aos_scope_status_t *st = &S.st;
    if (it < IT_TIM) {
        aos_scope_chan_t *c = &st->ch[it / IT_PER_CH];
        switch (it % IT_PER_CH) {
        case IT_DISP: c->on = !strcmp(v, "1") || !strncasecmp(v, "ON", 2); break;
        case IT_SCAL: { float f = strtof(v, NULL); if (f > 0) c->scale = f; } break;
        case IT_OFFS: c->offset = strtof(v, NULL); break;
        case IT_PROB: { float f = strtof(v, NULL); if (f > 0) c->probe = f; } break;
        case IT_COUP: {
            char u[6];
            size_t i = 0;
            for (; v[i] && i < sizeof u - 1; i++) u[i] = (char)toupper((unsigned char)v[i]);
            u[i] = 0;
            if (!strcmp(u, "DC") || !strcmp(u, "AC") || !strcmp(u, "GND")) snprintf(c->coupling, sizeof c->coupling, "%s", u);
        } break;
        }
        return;
    }
    switch (it) {
    case IT_TIM: { float f = strtof(v, NULL); if (f > 0) st->timebase = f; } break;
    case IT_TLEV: st->trig_level = strtof(v, NULL); break;
    case IT_TSRC: st->trig_source = parse_source(v); break;
    case IT_TSLP: {
        char u[6];
        size_t i = 0;
        for (; v[i] && i < 4; i++) u[i] = (char)toupper((unsigned char)v[i]);
        u[i] = 0;
        snprintf(st->trig_slope, sizeof st->trig_slope, "%s", !strncmp(u, "POS", 3) ? "POS" : !strncmp(u, "NEG", 3) ? "NEG" : !strncmp(u, "RFAL", 4) ? "RFAL" : u);
    } break;
    case IT_TSTAT: snprintf(st->trig_status, sizeof st->trig_status, "%.7s", v); break;
    }
}

static void item_query(int it, char *out, size_t n)
{
    static const char *const Q[IT_PER_CH] = { "DISP", "SCAL", "OFFS", "PROB", "COUP" };
    if (it < IT_TIM) { snprintf(out, n, ":CHAN%d:%s?", it / IT_PER_CH + 1, Q[it % IT_PER_CH]); return; }
    snprintf(out, n, "%s", it == IT_TIM ? ":TIM:MAIN:SCAL?" : it == IT_TLEV ? ":TRIG:EDGE:LEV?" : it == IT_TSRC ? ":TRIG:EDGE:SOUR?"
                          : it == IT_TSLP ? ":TRIG:EDGE:SLOP?" : ":TRIG:STAT?");
}

/* -------------------------------------------------------------------------- */
/* The socket                                                                  */
/* -------------------------------------------------------------------------- */

static void io_close(void)
{
    if (T.h > 0) aos_hal_tcp_close(T.h);
    T.h = 0;
    T.rx_len = T.rx_pos = 0;
}

static bool io_send(const char *cmd)
{
    char b[CMD_MAX + 2];
    int n = snprintf(b, sizeof b, "%s\n", cmd);
    if (n <= 0 || n >= (int)sizeof b) { fail(_("orden demasiado larga")); return false; }
    if (aos_hal_tcp_send(T.h, b, n, 2000) != n) { fail(_("se cortó la conexión")); return false; }
    return true;
}

/* More bytes into the buffer, waiting until the deadline. false when it passed or the link broke. */
static bool io_fill(uint32_t deadline)
{
    if (T.rx_pos > 0) {
        memmove(T.rx, T.rx + T.rx_pos, (size_t)(T.rx_len - T.rx_pos));
        T.rx_len -= T.rx_pos;
        T.rx_pos = 0;
    }
    if (T.rx_len >= RX_SIZE) { fail(_("respuesta demasiado larga")); return false; }
    for (;;) {
        int left = (int)(deadline - now_ms());
        if (left <= 0) { fail(_("el equipo no respondió a tiempo")); return false; }
        if (!wanted()) { fail(_("cancelado")); return false; }
        int r = aos_hal_tcp_recv(T.h, T.rx + T.rx_len, RX_SIZE - T.rx_len, left < 200 ? left : 200);
        if (r > 0) { T.rx_len += r; return true; }
        if (r < 0) { fail(_("el equipo cortó la conexión")); return false; }
    }
}

/* One answer line, without the "\r\n". Empty lines (a block's trailing
 * newline that was not eaten) are skipped: no answer of ours is empty. */
static bool io_line(char *out, size_t max, int timeout_ms)
{
    uint32_t deadline = now_ms() + (uint32_t)timeout_ms;
    for (;;) {
        uint8_t *s = T.rx + T.rx_pos;
        uint8_t *nl = memchr(s, '\n', (size_t)(T.rx_len - T.rx_pos));
        if (nl) {
            size_t l = (size_t)(nl - s);
            T.rx_pos += (int)l + 1;
            while (l && (s[l - 1] == '\r' || s[l - 1] == ' ')) l--;
            if (!l) continue;
            if (l >= max) l = max - 1;
            memcpy(out, s, l);
            out[l] = 0;
            return true;
        }
        if (!io_fill(deadline)) return false;
    }
}

static bool query(const char *cmd, char *out, size_t max)
{
    return io_send(cmd) && io_line(out, max, Q_TIMEOUT_MS);
}

/* The header of a block: "#N" and N digits of length. */
static bool io_block_head(uint32_t *len, int timeout_ms)
{
    uint32_t deadline = now_ms() + (uint32_t)timeout_ms;
    for (;;) {
        while (T.rx_pos < T.rx_len && (T.rx[T.rx_pos] == '\n' || T.rx[T.rx_pos] == '\r')) T.rx_pos++;
        if (T.rx_len - T.rx_pos >= 2) {
            if (T.rx[T.rx_pos] != '#' || !isdigit(T.rx[T.rx_pos + 1])) { fail(_("respuesta inesperada del equipo")); return false; }
            int nd = T.rx[T.rx_pos + 1] - '0';
            if (T.rx_len - T.rx_pos >= 2 + nd) {
                uint32_t l = 0;
                for (int i = 0; i < nd; i++) {
                    uint8_t c = T.rx[T.rx_pos + 2 + i];
                    if (!isdigit(c)) { fail(_("respuesta inesperada del equipo")); return false; }
                    l = l * 10 + (uint32_t)(c - '0');
                }
                T.rx_pos += 2 + nd;
                *len = l;
                return true;
            }
        }
        if (!io_fill(deadline)) return false;
    }
}

/* The block's bytes: the first 'keep' into dst (may be NULL), all of them
 * to the file (may be NULL; *wr_ok false if it could not take them), then
 * its newline. false if the link broke. */
static bool io_block_body(uint32_t len, uint8_t *dst, uint32_t keep, FILE *f, bool *wr_ok, int timeout_ms)
{
    uint32_t got = 0;
    bool dummy = true;
    if (!wr_ok) wr_ok = &dummy;
    *wr_ok = f != NULL;
    while (got < len) {
        if (T.rx_pos >= T.rx_len && !io_fill(now_ms() + (uint32_t)timeout_ms)) return false;
        uint32_t n = (uint32_t)(T.rx_len - T.rx_pos);
        if (n > len - got) n = len - got;
        const uint8_t *s = T.rx + T.rx_pos;
        if (dst && got < keep) {
            uint32_t k = keep - got < n ? keep - got : n;
            memcpy(dst + got, s, k);
        }
        if (f && *wr_ok && fwrite(s, 1, n, f) != n) *wr_ok = false;
        T.rx_pos += (int)n;
        got += n;
    }
    /* the trailing "\n": usually in the same packet; if not, io_line skips it later */
    if (T.rx_pos < T.rx_len && T.rx[T.rx_pos] == '\n') T.rx_pos++;
    return true;
}

/* -------------------------------------------------------------------------- */
/* The turns                                                                   */
/* -------------------------------------------------------------------------- */

static bool read_item(int it)
{
    char q[32], a[64];
    item_query(it, q, sizeof q);
    if (!query(q, a, sizeof a)) return false;
    lock();
    apply_item(it, a);
    unlock();
    return true;
}

static bool fetch_channel(int c)
{
    char q[32], a[160];
    snprintf(q, sizeof q, ":WAV:SOUR CHAN%d", c + 1);
    if (!io_send(q) || !query(":WAV:PRE?", a, sizeof a)) return false;
    float p[10] = { 0 };
    char *s = a;
    for (int i = 0; i < 10 && *s; i++) {
        p[i] = strtof(s, &s);
        while (*s == ',' || *s == ' ') s++;
    }
    T.xinc[c] = p[4];
    T.yinc[c] = p[7];
    T.yorg[c] = p[8];
    T.yref[c] = p[9];
    uint32_t len;
    if (!io_send(":WAV:DATA?") || !io_block_head(&len, BLOCK_TIMEOUT)) return false;
    uint32_t keep = len < AOS_SCOPE_POINTS ? len : AOS_SCOPE_POINTS;
    if (!io_block_body(len, T.raw[c], keep, NULL, NULL, BLOCK_TIMEOUT)) return false;
    T.raw_n[c] = (int)keep;
    return true;
}

static bool fetch_set(void)
{
    lock();
    bool on[AOS_SCOPE_CHANNELS];
    for (int c = 0; c < AOS_SCOPE_CHANNELS; c++) on[c] = S.st.ch[c].on;
    unlock();
    for (int c = 0; c < AOS_SCOPE_CHANNELS; c++) {
        T.raw_n[c] = 0;
        if (on[c] && !fetch_channel(c)) return false;
    }
    char a[16];
    if (!query(":TRIG:STAT?", a, sizeof a)) return false;
    uint32_t t = now_ms();
    lock();
    apply_item(IT_TSTAT, a);
    for (int c = 0; c < AOS_SCOPE_CHANNELS; c++) {
        float *w = S.wave + c * AOS_SCOPE_POINTS;
        const float k = T.yinc[c], o = T.yorg[c] + T.yref[c];
        for (int i = 0; i < T.raw_n[c]; i++) w[i] = ((float)T.raw[c][i] - o) * k;
        S.wave_n[c] = T.raw_n[c];
        S.wave_xinc[c] = T.xinc[c];
    }
    S.st.wave_seq++;
    S.set_t[S.set_n++ % 8] = t;
    unlock();
    return true;
}

static bool read_measure(int c, int m)
{
    char q[40], a[48];
    snprintf(q, sizeof q, ":MEAS:ITEM? %s,CHAN%d", MEAS_NAME[m], c + 1);
    if (!query(q, a, sizeof a)) return false;
    char *end;
    float v = strtof(a, &end);
    bool ok = end != a && isfinite(v) && fabsf(v) < 9e36f;       /* 9.9E37: no measurement */
    lock();
    S.meas_ok[c][m] = ok;
    if (ok) S.meas[c][m] = v;
    S.meas_ms[c][m] = now_ms();
    unlock();
    return true;
}

static bool screenshot(void)
{
    const char *dir = aos_hal_path_data();
    mkdir(dir, 0755);                       /* there already, most of the time */
    char tmp[176];
    snprintf(tmp, sizeof tmp, "%s/scope.tmp", dir);
    FILE *f = fopen(tmp, "wb");
    uint32_t len = 0;
    uint32_t t0 = now_ms();
    bool saved = false;
    bool ok = io_send(":DISP:DATA? ON,0,PNG") && io_block_head(&len, SHOT_TIMEOUT) &&
              io_block_body(len, NULL, 0, f, &saved, 5000);
    if (f) fclose(f);
    if (ok && saved && len > 8) {
        remove(S.shot_path);                /* FAT does not rename over a file */
        if (rename(tmp, S.shot_path) == 0) {
            lock();
            S.shot_seq++;
            unlock();
            aos_hal_log("scope", "screenshot: %u bytes in %u ms", (unsigned)len, (unsigned)(now_ms() - t0));
        }
    } else {
        remove(tmp);
        if (ok) aos_hal_log("scope", "screenshot: nowhere to save it (%s)", dir);
    }
    return ok;                              /* a file that could not be written is not a broken link */
}

/* The queued commands. false if the link broke. */
static bool send_queued(bool *did)
{
    for (;;) {
        char cmd[CMD_MAX];
        lock();
        bool have = S.qn > 0;
        if (have) {
            snprintf(cmd, sizeof cmd, "%s", S.q[S.qh]);
            S.qh = (S.qh + 1) % Q_LEN;
            S.qn--;
        }
        unlock();
        if (!have) return true;
        *did = true;
        if (!io_send(cmd)) return false;
        int ch;
        char arg[CMD_MAX];
        int it = setter_item(cmd, &ch, arg, sizeof arg);
        char up[8];
        snprintf(up, sizeof up, "%.6s", cmd);
        for (char *p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
        if (it >= 0) {
            T.dirty |= 1u << it;
            if (it < IT_TIM && it % IT_PER_CH == IT_PROB) T.dirty |= IT_CH(ch);      /* the probe rescales V/div and offset */
            if (it < IT_TIM && it % IT_PER_CH == IT_SCAL) T.dirty |= 1u << (ch * IT_PER_CH + IT_OFFS);
        } else if (!strncmp(up, ":AUT", 4) || !strncmp(up, "AUT", 3) || !strncmp(up, "*RST", 4)) {
            /* the scope is busy for seconds looking for the signals: *OPC?
             * answers when it is done, and nothing else is asked meanwhile */
            char a[8];
            aos_hal_sleep_ms(300);
            if (!io_send("*OPC?") || !io_line(a, sizeof a, 12000)) return false;
            T.dirty = IT_ALL;
            T.meas_next = -1;
            T.meas_round_ms = 0;
        } else {
            T.dirty |= 1u << IT_TSTAT;
        }
        T.force_set = true;
        aos_hal_sleep_ms(5);                /* the DS1000Z drops commands that come too close */
    }
}

static bool read_dirty(bool *did)
{
    for (int it = 0; it < IT_COUNT && T.dirty; it++) {
        if (!(T.dirty & (1u << it))) continue;
        T.dirty &= ~(1u << it);
        *did = true;
        if (!read_item(it)) return false;
    }
    return true;
}

/* One step of the background rounds, at most 'budget' queries. */
static bool rounds(bool live, int budget, bool *did)
{
    uint32_t t = now_ms();
    /* settings */
    if (T.set_next >= IT_COUNT && t - T.set_round_ms >= (live ? 1000u : 2000u)) { T.set_next = 0; T.set_round_ms = t; }
    while (budget > 0 && T.set_next < IT_COUNT) {
        int it = T.set_next++;
        if (it == IT_TSTAT && live) continue;       /* read with every set */
        *did = true;
        budget--;
        if (!read_item(it)) return false;
    }
    /* measurements */
    if (T.meas_next < 0 && t - T.meas_round_ms >= MEAS_EVERY_MS) { T.meas_next = 0; T.meas_round_ms = t; }
    while (budget > 0 && T.meas_next >= 0) {
        int k = T.meas_next;
        if (k >= AOS_SCOPE_CHANNELS * AOS_SCOPE_M_COUNT) { T.meas_next = -1; break; }
        T.meas_next++;
        int c = k / AOS_SCOPE_M_COUNT, m = k % AOS_SCOPE_M_COUNT;
        lock();
        bool on = S.st.ch[c].on;
        if (!on) S.meas_ok[c][m] = false;
        unlock();
        if (!on) continue;
        *did = true;
        budget--;
        if (!read_measure(c, m)) return false;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* The thread                                                                  */
/* -------------------------------------------------------------------------- */

static void set_link(bool connected, bool connecting, const char *err)
{
    lock();
    S.st.connected = connected;
    S.st.connecting = connecting;
    if (err) snprintf(S.st.error, sizeof S.st.error, "%s", err);
    if (!connected) {
        memset(S.meas_ok, 0, sizeof S.meas_ok);
        memset(S.wave_n, 0, sizeof S.wave_n);
        S.set_n = 0;
        S.shot_busy = false;
    }
    unlock();
}

static bool open_link(const char *host, int port)
{
    T.h = aos_hal_tcp_connect(host, port, 3000);
    if (T.h <= 0) {
        int e = T.h;
        T.h = 0;
        if (e == AOS_TCP_ERR_DNS) fail(_("no se encuentra «%s»"), host);
        else if (e == AOS_TCP_ERR_SLOTS) fail(_("no quedan conexiones libres"));
        else fail(_("%s:%d no contesta"), host, port);
        return false;
    }
    T.rx_len = T.rx_pos = 0;
    char idn[96];
    if (!query("*IDN?", idn, sizeof idn)) {
        fail(_("%s:%d no respondió a *IDN?"), host, port);
        return false;
    }
    if (!io_send(":WAV:MODE NORM") || !io_send(":WAV:FORM BYTE")) return false;
    for (int it = 0; it < IT_COUNT; it++) if (!read_item(it)) return false;
    lock();
    snprintf(S.st.idn, sizeof S.st.idn, "%s", idn);
    unlock();
    aos_hal_log("scope", "connected to %s:%d: %s", host, port, idn);
    return true;
}

static void service(void *arg)
{
    (void)arg;
    uint32_t pause = 0;
    for (;;) {
        lock();
        bool want = S.want;
        uint32_t gen = S.gen;
        char host[64];
        snprintf(host, sizeof host, "%s", S.host);
        int port = S.port;
        unlock();
        if (!want || !host[0]) {
            if (T.h > 0) io_close();
            set_link(false, false, NULL);
            pause = 0;
            aos_hal_sleep_ms(100);
            continue;
        }
        /* a pause after a failure, cut short by a new request */
        for (uint32_t w = 0; w < pause && wanted(); w += 100) aos_hal_sleep_ms(100);
        lock();
        if (S.gen != gen || !S.want) { unlock(); continue; }
        unlock();
        T.gen = gen;
        set_link(false, true, NULL);
        T.err[0] = 0;
        if (!open_link(host, port)) {
            io_close();
            set_link(false, wanted(), T.err);
            aos_hal_log("scope", "%s", T.err);
            pause = pause ? (pause * 2 > 8000 ? 8000 : pause * 2) : 1000;
            continue;
        }
        set_link(true, false, "");
        pause = 0;
        T.dirty = 0;
        T.set_next = IT_COUNT;
        T.set_round_ms = now_ms();
        T.meas_next = -1;
        T.meas_round_ms = 0;
        T.force_set = true;
        bool ok = true;
        while (ok && wanted()) {
            bool did = false;
            ok = send_queued(&did) && read_dirty(&did);
            if (!ok) break;
            lock();
            bool shot = S.shot_req, live = S.live || (int32_t)(S.live_until - now_ms()) > 0;
            bool stopped = !strcmp(S.st.trig_status, "STOP");
            if (S.kick) T.force_set = true;
            S.kick = false;
            S.shot_req = false;
            if (shot) S.shot_busy = true;
            unlock();
            if (shot) {
                ok = screenshot();
                lock();
                S.shot_busy = false;
                unlock();
                did = true;
                if (!ok) break;
            }
            uint32_t t = now_ms();
            if (live && (T.force_set || t - T.last_set_ms >= (stopped ? SET_STOP_MS : SET_MIN_MS))) {
                T.force_set = false;
                T.last_set_ms = t;
                did = true;
                if (!(ok = fetch_set())) break;
            }
            ok = rounds(live, live ? 4 : 64, &did);
            if (ok && !did) aos_hal_sleep_ms(live ? 5 : 20);
        }
        io_close();
        if (wanted()) {                         /* it broke, not asked to */
            set_link(false, true, T.err);
            aos_hal_log("scope", "link lost: %s", T.err);
            pause = 1000;
        } else {
            set_link(false, false, "");
        }
    }
}

/* -------------------------------------------------------------------------- */
/* The contract                                                                */
/* -------------------------------------------------------------------------- */

void aos_scope_start(void)
{
    if (S.started) return;
    S.mx = aos_hal_mutex_create();
    S.wave = calloc(AOS_SCOPE_CHANNELS * AOS_SCOPE_POINTS, sizeof *S.wave);
    if (!S.mx || !S.wave) { aos_hal_log("scope", "no memory"); return; }
    snprintf(S.shot_path, sizeof S.shot_path, "%s/scope.png", aos_hal_path_data());
    int32_t v;
    if (!aos_hal_pref_get_str("scope_host", S.host, sizeof S.host)) S.host[0] = 0;
    S.port = aos_hal_pref_get_i32("scope_port", &v) && v > 0 ? v : DEFAULT_PORT;
    S.want = S.host[0] && aos_hal_pref_get_i32("scope_on", &v) && v;
    snprintf(S.st.host, sizeof S.st.host, "%s", S.host);
    for (int c = 0; c < AOS_SCOPE_CHANNELS; c++) {
        S.st.ch[c].scale = 1;
        S.st.ch[c].probe = 1;
        snprintf(S.st.ch[c].coupling, sizeof S.st.ch[c].coupling, "DC");
    }
    S.st.timebase = 1e-3f;
    S.st.trig_source = 1;
    snprintf(S.st.trig_slope, sizeof S.st.trig_slope, "POS");
    S.started = aos_hal_thread_start("scope", service, NULL, 8192, 4);
    if (!S.started) aos_hal_log("scope", "no thread");
}

void aos_scope_connect(const char *host, int port)
{
    aos_scope_start();
    if (!S.mx || !host) return;
    if (port <= 0) port = DEFAULT_PORT;
    lock();
    snprintf(S.host, sizeof S.host, "%s", host);
    S.port = port;
    S.want = S.host[0] != 0;
    S.gen++;
    S.qn = 0;
    snprintf(S.st.host, sizeof S.st.host, "%s", S.host);
    S.st.connecting = S.want;
    S.st.error[0] = 0;
    unlock();
    aos_hal_pref_set_str("scope_host", S.host);
    aos_hal_pref_set_i32("scope_port", port);
    aos_hal_pref_set_i32("scope_on", S.want);
}

void aos_scope_disconnect(void)
{
    if (!S.mx) return;
    lock();
    S.want = false;
    S.gen++;
    S.qn = 0;
    S.st.connecting = false;
    unlock();
    aos_hal_pref_set_i32("scope_on", 0);
}

void aos_scope_status(aos_scope_status_t *out)
{
    if (!out) return;
    if (!S.mx) { memset(out, 0, sizeof *out); return; }
    lock();
    *out = S.st;
    int n = S.set_n < 8 ? S.set_n : 8;
    uint32_t newest = n ? S.set_t[(S.set_n - 1) % 8] : 0, oldest = n ? S.set_t[(S.set_n - n) % 8] : 0;
    unlock();
    uint32_t t = now_ms();
    out->fps = n >= 2 && t - newest < 2000 && newest > oldest ? (float)(n - 1) * 1000.0f / (float)(newest - oldest) : 0;
}

void aos_scope_live(bool on)
{
    if (!S.mx) return;
    lock();
    if (on && !S.live) S.kick = true;
    S.live = on;
    unlock();
}

void aos_scope_live_for(uint32_t ms)
{
    if (!S.mx) return;
    lock();
    uint32_t until = now_ms() + ms;
    bool was = S.live || (int32_t)(S.live_until - now_ms()) > 0;
    if ((int32_t)(until - S.live_until) > 0) S.live_until = until;
    if (!was) S.kick = true;
    unlock();
}

int aos_scope_wave(int ch, float *volts, int max, float *xinc)
{
    if (!S.mx || ch < 1 || ch > AOS_SCOPE_CHANNELS || !volts || max <= 0) return 0;
    lock();
    int n = S.wave_n[ch - 1] < max ? S.wave_n[ch - 1] : max;
    memcpy(volts, S.wave + (ch - 1) * AOS_SCOPE_POINTS, (size_t)n * sizeof *volts);
    if (xinc) *xinc = S.wave_xinc[ch - 1];
    unlock();
    return n;
}

bool aos_scope_measure(int ch, aos_scope_measure_t item, float *out, uint32_t *age_ms)
{
    if (!S.mx || ch < 1 || ch > AOS_SCOPE_CHANNELS || item < 0 || item >= AOS_SCOPE_M_COUNT) return false;
    lock();
    bool ok = S.st.connected && S.st.ch[ch - 1].on && S.meas_ok[ch - 1][item];
    if (ok && out) *out = S.meas[ch - 1][item];
    if (ok && age_ms) *age_ms = now_ms() - S.meas_ms[ch - 1][item];
    unlock();
    return ok;
}

bool aos_scope_cmd(const char *scpi)
{
    if (!S.mx || !scpi || !scpi[0] || strlen(scpi) >= CMD_MAX) return false;
    lock();
    bool ok = S.st.connected && S.qn < Q_LEN;
    if (ok) {
        snprintf(S.q[(S.qh + S.qn) % Q_LEN], CMD_MAX, "%s", scpi);
        S.qn++;
        /* what it sets shows at once; the read-back corrects it if the scope disagrees */
        int ch;
        char arg[CMD_MAX];
        int it = setter_item(scpi, &ch, arg, sizeof arg);
        if (it >= 0 && arg[0]) {
            aos_scope_chan_t *c = &S.st.ch[ch];
            float old_probe = c->probe;
            apply_item(it, arg);
            if (it < IT_TIM && it % IT_PER_CH == IT_PROB && old_probe > 0 && c->probe != old_probe) {
                c->scale *= c->probe / old_probe;       /* what the scope itself does */
                c->offset *= c->probe / old_probe;
            }
        } else if (!strncasecmp(scpi, ":STOP", 5)) {
            snprintf(S.st.trig_status, sizeof S.st.trig_status, "STOP");
        } else if (!strncasecmp(scpi, ":SING", 5)) {
            snprintf(S.st.trig_status, sizeof S.st.trig_status, "WAIT");
        } else if (!strncasecmp(scpi, ":RUN", 4)) {
            snprintf(S.st.trig_status, sizeof S.st.trig_status, "RUN");
        }
    }
    unlock();
    return ok;
}

bool aos_scope_screenshot(void)
{
    if (!S.mx) return false;
    lock();
    bool ok = S.st.connected;
    if (ok) S.shot_req = true;
    unlock();
    return ok;
}

const char *aos_scope_screenshot_file(uint32_t *seq)
{
    if (!S.mx) { if (seq) *seq = 0; return ""; }
    lock();
    if (seq) *seq = S.shot_seq;
    unlock();
    return S.shot_path;
}

bool aos_scope_screenshot_busy(void)
{
    if (!S.mx) return false;
    lock();
    bool b = S.shot_req || S.shot_busy;
    unlock();
    return b;
}
