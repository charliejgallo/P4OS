/*
 * P4OS - Serial capture service.
 *
 * Each channel is a task that reads one UART port into a ring of lines. It
 * does not belong to any app: the Terminal shows the ring, the portal too,
 * and the capture goes on with anything else in front (UI.md, "servicios").
 *
 * A line ends at '\n'; '\r' is dropped, and so are ANSI colour sequences
 * (ESP-IDF's log sends them) - the Terminal colours by the log level
 * letter instead. Text that stops without a newline (a "login:" prompt) is
 * committed after 250 ms of silence.
 *
 * On top of the lines:
 *
 *   raw bytes   the last 64 KB exactly as they came, for the hex view: a
 *               binary protocol has no lines to speak of.
 *   triggers    up to eight texts, shared by both channels, looked for in
 *               every line (case ignored). A hit marks the line, counts, and
 *               - at most once every ten seconds per trigger - raises a
 *               notification and/or beeps. The bench's reason for them: a
 *               "Guru Meditation" at three in the morning, seen at eight.
 *   boots       a line that looks like a chip starting ("rst:0x", "ESP-ROM:")
 *               is marked, so the view can draw where each run begins.
 *   recording   every line to a file on the card, <sd>/logs/<port>-<date>
 *               .log, with the wall-clock time (or the uptime, before SNTP)
 *               in front. Flushed every two seconds: a pulled card loses at
 *               most that.
 */
#include "aos_io.h"
#include "aos_hal.h"
#include "aos_notif_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define RING_LINES 3000
#define RAW_BYTES  (64 * 1024)

typedef struct {
    char     text[AOS_SERIAL_LINE_MAX];
    uint32_t t_ms;
    uint8_t  flags;                 /* AOS_SERIAL_F_* */
} line_t;

typedef struct {
    aos_io_uart_t *uart;
    volatile bool running, stop;
    char     port[AOS_IO_PORT_NAME_MAX];
    uint32_t baud;
    line_t  *ring;
    volatile uint32_t lines;        /* committed so far */
    uint32_t first;
    char     cur[AOS_SERIAL_LINE_MAX];
    int      cur_len;
    int      esc;                   /* inside an ANSI escape */
    uint64_t last_rx_ms;
    uint64_t rx, tx;
    uint32_t overruns;
    void    *mutex;
    /* raw */
    uint8_t *raw;
    volatile uint64_t raw_total;
    /* recording */
    FILE    *rec;
    char     rec_path[128];
    uint64_t rec_bytes;
    uint64_t rec_flush_ms;
    volatile bool rec_want, rec_stop;
} chan_t;

static chan_t s_ch[AOS_SERIAL_CHANNELS];

/* ---- triggers ---- */

typedef struct {
    char     text[AOS_SERIAL_TRIGGER_MAX];
    uint8_t  flags;                 /* AOS_SERIAL_T_NOTIFY | AOS_SERIAL_T_BEEP */
    uint32_t hits;
    uint64_t last_alert_ms;
} trig_t;

static trig_t s_trig[AOS_SERIAL_TRIGGERS];
static int s_ntrig;
static bool s_trig_loaded;
static void *s_trig_mx;

static void trig_save(void)
{
    char buf[AOS_SERIAL_TRIGGERS * (AOS_SERIAL_TRIGGER_MAX + 4)] = "";
    for (int i = 0; i < s_ntrig; i++) {
        char e[AOS_SERIAL_TRIGGER_MAX + 8];
        snprintf(e, sizeof e, "%u:%.*s\x1f", s_trig[i].flags & 3, AOS_SERIAL_TRIGGER_MAX - 1, s_trig[i].text);   /* unit separator: one line in any store */
        strncat(buf, e, sizeof buf - strlen(buf) - 1);
    }
    aos_hal_pref_set_str("ser_trig", buf);
}

static void trig_load(void)
{
    if (s_trig_loaded) return;
    s_trig_loaded = true;
    if (!s_trig_mx) s_trig_mx = aos_hal_mutex_create();
    char buf[AOS_SERIAL_TRIGGERS * (AOS_SERIAL_TRIGGER_MAX + 4)];
    if (!aos_hal_pref_get_str("ser_trig", buf, sizeof buf)) {
        /* the first time: what every ESP32 bench wants to hear about */
        static const char *const DEF[] = { "Guru Meditation", "abort()", "Brownout" };
        for (size_t i = 0; i < sizeof DEF / sizeof DEF[0]; i++) {
            snprintf(s_trig[s_ntrig].text, sizeof s_trig[0].text, "%s", DEF[i]);
            s_trig[s_ntrig++].flags = AOS_SERIAL_T_NOTIFY;
        }
        return;
    }
    for (char *p = buf; *p && s_ntrig < AOS_SERIAL_TRIGGERS; ) {
        char *nl = strchr(p, '\x1f');
        if (nl) *nl = 0;
        char *colon = strchr(p, ':');
        if (colon && colon[1]) {
            s_trig[s_ntrig].flags = (uint8_t)atoi(p);
            snprintf(s_trig[s_ntrig].text, sizeof s_trig[0].text, "%s", colon + 1);
            s_ntrig++;
        }
        if (!nl) break;
        p = nl + 1;
    }
}

static bool contains_nocase(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return false;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

/* Called by the reader with the finished line; returns the line's flags. */
static uint8_t check(chan_t *c, int ch, const char *text)
{
    uint8_t f = 0;
    if (strstr(text, "rst:0x") || strstr(text, "ESP-ROM:")) f |= AOS_SERIAL_F_BOOT;
    aos_hal_mutex_lock(s_trig_mx);
    int alert = -1;
    for (int i = 0; i < s_ntrig; i++) {
        if (!contains_nocase(text, s_trig[i].text)) continue;
        f |= AOS_SERIAL_F_TRIGGER;
        s_trig[i].hits++;
        uint64_t now = aos_hal_uptime_ms();
        if (s_trig[i].flags && (!s_trig[i].last_alert_ms || now - s_trig[i].last_alert_ms > 10000)) {
            s_trig[i].last_alert_ms = now;
            alert = i;
        }
    }
    trig_t t = alert >= 0 ? s_trig[alert] : (trig_t){ 0 };
    aos_hal_mutex_unlock(s_trig_mx);
    if (alert >= 0) {
        if (t.flags & AOS_SERIAL_T_NOTIFY) {
            aos_notif_t n = { 0 };
            n.uid = 0x20000000u | (uint32_t)(ch << 8) | (uint32_t)alert;
            n.category = AOS_NOTIF_OTHER;
            snprintf(n.app, sizeof n.app, "Terminal");
            snprintf(n.title, sizeof n.title, "%.15s: %.46s", c->port, t.text);
            snprintf(n.message, sizeof n.message, "%.*s", (int)sizeof n.message - 1, text);
            n.when = time(NULL);
            n.important = true;
            aos_notif_push(&n);
        }
        if (t.flags & AOS_SERIAL_T_BEEP) aos_hal_beep(1320, 140);
    }
    return f;
}

int aos_serial_trigger_count(void) { trig_load(); return s_ntrig; }

bool aos_serial_trigger_get(int i, aos_serial_trigger_t *out)
{
    trig_load();
    if (i < 0 || i >= s_ntrig || !out) return false;
    aos_hal_mutex_lock(s_trig_mx);
    snprintf(out->text, sizeof out->text, "%s", s_trig[i].text);
    out->flags = s_trig[i].flags;
    out->hits = s_trig[i].hits;
    aos_hal_mutex_unlock(s_trig_mx);
    return true;
}

int aos_serial_trigger_add(const char *text, uint8_t flags)
{
    trig_load();
    if (!text || !text[0] || s_ntrig >= AOS_SERIAL_TRIGGERS) return -1;
    aos_hal_mutex_lock(s_trig_mx);
    trig_t *t = &s_trig[s_ntrig];
    memset(t, 0, sizeof *t);
    snprintf(t->text, sizeof t->text, "%s", text);
    t->flags = flags;
    int i = s_ntrig++;
    trig_save();
    aos_hal_mutex_unlock(s_trig_mx);
    return i;
}

void aos_serial_trigger_set(int i, uint8_t flags)
{
    trig_load();
    if (i < 0 || i >= s_ntrig) return;
    aos_hal_mutex_lock(s_trig_mx);
    s_trig[i].flags = flags;
    trig_save();
    aos_hal_mutex_unlock(s_trig_mx);
}

bool aos_serial_trigger_rename(int i, const char *text)
{
    trig_load();
    if (i < 0 || i >= s_ntrig || !text || !text[0]) return false;
    aos_hal_mutex_lock(s_trig_mx);
    snprintf(s_trig[i].text, sizeof s_trig[i].text, "%s", text);
    s_trig[i].hits = 0;
    trig_save();
    aos_hal_mutex_unlock(s_trig_mx);
    return true;
}

void aos_serial_trigger_remove(int i)
{
    trig_load();
    if (i < 0 || i >= s_ntrig) return;
    aos_hal_mutex_lock(s_trig_mx);
    memmove(&s_trig[i], &s_trig[i + 1], (size_t)(s_ntrig - i - 1) * sizeof s_trig[0]);
    s_ntrig--;
    trig_save();
    aos_hal_mutex_unlock(s_trig_mx);
}

/* ---- recording ---- */

static void rec_open(chan_t *c)
{
    const char *root = aos_hal_path_sd_root();
    if (!root) return;
    char dir[128];
    snprintf(dir, sizeof dir, "%s/logs", root);
    mkdir(dir, 0777);
    char when[32];
    if (aos_hal_time_is_valid()) {
        struct tm t;
        aos_hal_time_now(&t);
        strftime(when, sizeof when, "%Y%m%d-%H%M%S", &t);
    } else {
        snprintf(when, sizeof when, "t%lu", (unsigned long)(aos_hal_uptime_ms() / 1000));
    }
    snprintf(c->rec_path, sizeof c->rec_path, "%.60s/%.15s-%s.log", dir, c->port, when);
    for (char *p = c->rec_path + strlen(dir) + 1; *p; p++) if (*p == '.' && strcmp(p, ".log")) *p = '_';   /* uart.b -> uart_b */
    c->rec = fopen(c->rec_path, "a");
    c->rec_bytes = 0;
    if (c->rec) {
        int n = fprintf(c->rec, "# %s at %u baud, P4OS %s\n", c->port, (unsigned)c->baud, aos_hal_firmware_version());
        if (n > 0) c->rec_bytes += (uint64_t)n;
        aos_hal_log("serial", "recording %s to %s", c->port, c->rec_path);
    }
}

static void rec_line(chan_t *c, const char *text)
{
    if (!c->rec) return;
    char stamp[64];
    if (aos_hal_time_is_valid()) {
        struct tm t;
        aos_hal_time_now(&t);
        snprintf(stamp, sizeof stamp, "%04d-%02d-%02d %02d:%02d:%02d.%03u", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec, (unsigned)(aos_hal_uptime_ms() % 1000));
    } else {
        uint64_t ms = aos_hal_uptime_ms();
        snprintf(stamp, sizeof stamp, "+%lu.%03u", (unsigned long)(ms / 1000), (unsigned)(ms % 1000));
    }
    int n = fprintf(c->rec, "[%s] %s\n", stamp, text);
    if (n > 0) c->rec_bytes += (uint64_t)n;
    else { fclose(c->rec); c->rec = NULL; aos_hal_log("serial", "recording of %s stopped: the card refused", c->port); }
}

/* ---- the reader ---- */

static void commit(chan_t *c, int ch)
{
    c->cur[c->cur_len] = 0;
    uint8_t f = check(c, ch, c->cur);
    aos_hal_mutex_lock(c->mutex);
    line_t *l = &c->ring[c->lines % RING_LINES];
    memcpy(l->text, c->cur, (size_t)c->cur_len + 1);
    l->t_ms = (uint32_t)aos_hal_uptime_ms();
    l->flags = f;
    c->lines++;
    if (c->lines - c->first > RING_LINES) c->first = c->lines - RING_LINES;
    aos_hal_mutex_unlock(c->mutex);
    rec_line(c, c->cur);
    c->cur_len = 0;
}

static void feed(chan_t *c, int ch, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) c->raw[(c->raw_total + (uint64_t)i) % RAW_BYTES] = b[i];
    c->raw_total += (uint64_t)n;
    for (int i = 0; i < n; i++) {
        uint8_t v = b[i];
        if (c->esc) {
            /* ESC [ params letter */
            if (c->esc == 1) c->esc = v == '[' ? 2 : 0;
            else if ((v >= 'A' && v <= 'Z') || (v >= 'a' && v <= 'z')) c->esc = 0;
            continue;
        }
        if (v == 0x1B) { c->esc = 1; continue; }
        if (v == '\r') continue;
        if (v == '\n') { commit(c, ch); continue; }
        if (v == '\t') v = ' ';
        if (v < 0x20) continue;
        if (c->cur_len >= AOS_SERIAL_LINE_MAX - 1) { c->overruns++; commit(c, ch); }
        c->cur[c->cur_len++] = (char)v;
    }
}

static void reader(void *arg)
{
    chan_t *c = arg;
    int ch = (int)(c - s_ch);
    uint8_t buf[512];
    while (!c->stop) {
        int n = aos_io_uart_read(c->uart, buf, sizeof buf, 50);
        uint64_t now = aos_hal_uptime_ms();
        if (n > 0) {
            c->rx += (uint64_t)n;
            c->last_rx_ms = now;
            feed(c, ch, buf, n);
        } else if (n < 0) {
            aos_hal_sleep_ms(100);
        } else if (c->cur_len && now - c->last_rx_ms > 250) {
            commit(c, ch);
        }
        /* the card is only touched from this task */
        if (c->rec_want && !c->rec) { c->rec_want = false; rec_open(c); }
        if (c->rec_stop) {
            c->rec_stop = false;
            if (c->rec) { fclose(c->rec); c->rec = NULL; }
        }
        if (c->rec && now - c->rec_flush_ms > 2000) { c->rec_flush_ms = now; fflush(c->rec); }
    }
    if (c->rec) { fclose(c->rec); c->rec = NULL; }
    aos_io_uart_close(c->uart);
    c->uart = NULL;
    c->running = false;
}

bool aos_serial_start(int ch, const char *port, uint32_t baud)
{
    if (ch < 0 || ch >= AOS_SERIAL_CHANNELS) return false;
    chan_t *c = &s_ch[ch];
    bool was_recording = c->rec != NULL;
    aos_serial_stop(ch);
    trig_load();
    if (!c->mutex) c->mutex = aos_hal_mutex_create();
    if (!c->ring) c->ring = calloc(RING_LINES, sizeof(line_t));
    if (!c->raw) c->raw = malloc(RAW_BYTES);
    if (!c->ring || !c->raw) return false;
    char owner[24];
    snprintf(owner, sizeof owner, "serial %d", ch);
    c->uart = aos_io_uart_open(port, baud, false, owner);
    if (!c->uart) return false;
    snprintf(c->port, sizeof c->port, "%s", port);
    c->baud = baud;
    c->stop = false;
    c->running = true;
    c->cur_len = 0;
    c->esc = 0;
    c->rec_want = was_recording;        /* a change of speed keeps recording, in a new file */
    char name[16];
    snprintf(name, sizeof name, "serial%d", ch);
    if (!aos_hal_thread_start(name, reader, c, 6144, 5)) {
        aos_io_uart_close(c->uart);
        c->uart = NULL;
        c->running = false;
        return false;
    }
    aos_hal_log("serial", "channel %d on %s at %u", ch, aos_io_uart_desc(c->uart), (unsigned)baud);
    return true;
}

void aos_serial_stop(int ch)
{
    if (ch < 0 || ch >= AOS_SERIAL_CHANNELS) return;
    chan_t *c = &s_ch[ch];
    if (!c->running) return;
    c->stop = true;
    for (int i = 0; i < 100 && c->running; i++) aos_hal_sleep_ms(10);
}

bool aos_serial_record(int ch, bool on)
{
    if (ch < 0 || ch >= AOS_SERIAL_CHANNELS) return false;
    chan_t *c = &s_ch[ch];
    if (!c->running || !aos_hal_path_sd_root()) return false;
    if (on) c->rec_want = true;
    else c->rec_stop = true;
    return true;
}

bool aos_serial_stat(int ch, aos_serial_stat_t *out)
{
    if (ch < 0 || ch >= AOS_SERIAL_CHANNELS || !out) return false;
    chan_t *c = &s_ch[ch];
    memset(out, 0, sizeof *out);
    out->running = c->running;
    snprintf(out->port, sizeof out->port, "%s", c->port);
    snprintf(out->desc, sizeof out->desc, "%s", c->uart ? aos_io_uart_desc(c->uart) : "");
    out->baud = c->baud;
    out->lines = c->lines;
    out->first = c->first;
    out->rx_bytes = c->rx;
    out->tx_bytes = c->tx;
    out->overruns = c->overruns;
    out->recording = c->rec != NULL || c->rec_want;
    snprintf(out->rec_path, sizeof out->rec_path, "%s", c->rec ? c->rec_path : "");
    out->rec_bytes = c->rec_bytes;
    out->raw_total = c->raw_total;
    return true;
}

int aos_serial_line_ex(int ch, uint32_t index, char *out, int cap, uint32_t *t_ms, uint8_t *flags)
{
    if (ch < 0 || ch >= AOS_SERIAL_CHANNELS || !s_ch[ch].ring) return -1;
    chan_t *c = &s_ch[ch];
    aos_hal_mutex_lock(c->mutex);
    int n = -1;
    if (index >= c->first && index < c->lines) {
        const line_t *l = &c->ring[index % RING_LINES];
        n = (int)strlen(l->text);
        if (n >= cap) n = cap - 1;
        memcpy(out, l->text, (size_t)n);
        out[n] = 0;
        if (t_ms) *t_ms = l->t_ms;
        if (flags) *flags = l->flags;
    }
    aos_hal_mutex_unlock(c->mutex);
    return n;
}

int aos_serial_line(int ch, uint32_t index, char *out, int cap, uint32_t *t_ms)
{
    return aos_serial_line_ex(ch, index, out, cap, t_ms, NULL);
}

int aos_serial_raw(int ch, uint64_t from, uint8_t *out, int max, uint64_t *next)
{
    if (ch < 0 || ch >= AOS_SERIAL_CHANNELS || !s_ch[ch].raw) { if (next) *next = 0; return 0; }
    chan_t *c = &s_ch[ch];
    uint64_t total = c->raw_total, oldest = total > RAW_BYTES ? total - RAW_BYTES : 0;
    if (from < oldest) from = oldest;
    if (from > total) from = total;
    int n = (int)(total - from < (uint64_t)max ? total - from : (uint64_t)max);
    for (int i = 0; i < n; i++) out[i] = c->raw[(from + (uint64_t)i) % RAW_BYTES];
    if (next) *next = from + (uint64_t)n;
    return n;
}

int aos_serial_send(int ch, const void *data, int len)
{
    if (ch < 0 || ch >= AOS_SERIAL_CHANNELS || !s_ch[ch].uart) return -1;
    int n = aos_io_uart_write(s_ch[ch].uart, data, len);
    if (n > 0) s_ch[ch].tx += (uint64_t)n;
    return n;
}

void aos_serial_clear(int ch)
{
    if (ch < 0 || ch >= AOS_SERIAL_CHANNELS || !s_ch[ch].mutex) return;
    chan_t *c = &s_ch[ch];
    aos_hal_mutex_lock(c->mutex);
    c->first = c->lines;
    aos_hal_mutex_unlock(c->mutex);
}
