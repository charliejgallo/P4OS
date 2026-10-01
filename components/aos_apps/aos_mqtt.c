/*
 * P4OS - MQTT 3.1.1 client service. The contract is in aos_mqtt.h; this is
 * how it keeps it.
 *
 * The thread's life: wait for a host and the network, open a TCP stream,
 * CONNECT (clean session), wait for the CONNACK, SUBSCRIBE to the base
 * filters and the extra ones in one packet, resend any QoS 1 publish that
 * was never acknowledged, and loop:
 *
 *     read what came (50 ms at most), cut it into packets, answer them
 *     send what is queued (QoS 1 ones wait for a free slot of 4 in flight)
 *     subscribe / unsubscribe what other code asked for since
 *     resend a QoS 1 publish unanswered for 10 s, with DUP
 *     PINGREQ after half a keepalive of silence on our side; no PINGRESP
 *     within a keepalive (10 s at least) and the session is dead
 *
 * Anything wrong drops the connection and it starts over after a growing
 * pause, except a refused user or password, which waits for new settings.
 *
 * A packet bigger than the receive buffer (8 KB) is not lost: its head is
 * handled as a PUBLISH cut short (the topic, the packet id and the first
 * part of the payload are all at the front; the table keeps its real
 * length) and the rest is skipped as it arrives.
 *
 * Numbers are found without building a tree: a small scanner walks the
 * JSON, records every number and boolean down to the second level under its
 * path, and allocates nothing, so a broker pouring 100 messages a second
 * costs no heap. The pretty printer is the same kind of walk.
 *
 * Everything the thread writes into the table is written under the mutex;
 * sending happens outside it.
 */
#include "aos_mqtt.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RX_SIZE      8192
#define Q_LEN        8
#define INFLIGHT     4
#define XSUBS        8
#define Q_WAIT_MS    30000          /* a publish waits this long for a connection */
#define RESEND_MS    10000
#define PKT_MAX      (AOS_MQTT_TOPIC_MAX + AOS_MQTT_PUB_MAX + 16)

typedef struct {
    int16_t  topic;                 /* owner slot, -1 = free */
    int16_t  next;                  /* the owner's next series, -1 = last */
    uint8_t  n, head;
    char     field[AOS_MQTT_FIELD_MAX];
    uint32_t last_ms;
    float    v[AOS_MQTT_HIST];
    uint32_t t[AOS_MQTT_HIST];
} series_t;

typedef struct {
    char     topic[AOS_MQTT_TOPIC_MAX];
    uint8_t  payload[AOS_MQTT_PUB_MAX];
    uint16_t len;
    uint8_t  qos;
    bool     retain;
    uint32_t ms;
} pubq_t;

typedef struct {
    bool     used;
    uint16_t pid;
    uint32_t sent_ms;
    uint16_t len;
    uint8_t  pkt[PKT_MAX];
} inflight_t;

typedef struct {
    char    f[AOS_MQTT_TOPIC_MAX];
    uint8_t qos;
    uint8_t st;                     /* 0 free, 1 to subscribe, 2 subscribed, 3 to unsubscribe */
} xsub_t;

/* The big things, in one block of PSRAM allocated when the service starts:
 * AOS_BSS_PSRAM is not honoured by this firmware (the .bss stays inside),
 * and malloc would put blocks under 16 KB in internal RAM. */
typedef struct {
    aos_mqtt_topic_t top[AOS_MQTT_MAX_TOPICS];
    series_t         ser[AOS_MQTT_SERIES];
    uint8_t          rx[RX_SIZE];
    uint8_t          tx[PKT_MAX];
    pubq_t           q[Q_LEN];
    inflight_t       inf[INFLIGHT];
    xsub_t           xs[XSUBS];
} big_t;

static big_t *B;
#define s_top (B->top)
#define s_ser (B->ser)
#define s_rx  (B->rx)
#define s_tx  (B->tx)
#define s_q   (B->q)
#define s_inf (B->inf)

#if defined(AOS_SIM)
static void *big_calloc(size_t n) { return calloc(1, n); }
#else
#include "esp_heap_caps.h"
static void *big_calloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}
#endif

static struct {
    void *mx;
    bool started;
    aos_mqtt_config_t cfg;
    uint32_t cfg_gen;
    char autoid[48];
    char cid[48];                   /* the client id in use */
    volatile uint32_t version;
    volatile aos_mqtt_state_t state;
    char err[112];
    volatile bool kick;
    aos_mqtt_stats_t st;
    int ntop;
    int qn;
    /* thread only */
    int h;
    int rx_len;
    uint32_t skip;
    uint16_t next_pid;
    uint32_t last_tx, ping_sent;
    uint16_t sub_pid;
    int sub_count;
} M;                                /* M.h: 0 and below is no socket */

static void lock(void) { aos_hal_mutex_lock(M.mx); }
static void unlock(void) { aos_hal_mutex_unlock(M.mx); }
static void bump(void) { M.version++; }
static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

static void set_state(aos_mqtt_state_t s, const char *why)
{
    lock();
    M.state = s;
    if (why) snprintf(M.err, sizeof M.err, "%s", why);
    bump();
    unlock();
}

/* strlcpy, without asking GCC's format-truncation checker for its opinion */
static void scpy(char *d, size_t n, const char *s)
{
    size_t l = strlen(s);
    if (l >= n) l = n - 1;
    memcpy(d, s, l);
    d[l] = 0;
}

static uint32_t fnv(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

/* -------------------------------------------------------------------------- */
/* Topic filters                                                               */
/* -------------------------------------------------------------------------- */

bool aos_mqtt_match(const char *f, const char *t)
{
    if (!f || !t) return false;
    /* topics starting with $ are not matched by a leading wildcard */
    if (t[0] == '$' && (f[0] == '#' || f[0] == '+')) return false;
    for (;;) {
        if (f[0] == '#' && f[1] == 0) return true;
        if (f[0] == '+') {
            while (*t && *t != '/') t++;
            f++;
        } else {
            while (*f && *f != '/' && *f == *t) { f++; t++; }
            if (*f && *f != '/') return false;
            if (*t && *t != '/') return false;
        }
        if (!*f && !*t) return true;
        if (*f == '/' && f[1] == '#' && f[2] == 0 && !*t) return true;   /* "a/#" matches "a" */
        if (*f != '/' || *t != '/') return false;
        f++;
        t++;
    }
}

static bool valid_topic_name(const char *t)
{
    return t && t[0] && !strpbrk(t, "+#") && strlen(t) < AOS_MQTT_TOPIC_MAX;
}

/* -------------------------------------------------------------------------- */
/* A JSON scanner that allocates nothing                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *p, *e;
    int nf;
    char (*names)[AOS_MQTT_FIELD_MAX];
    float *vals;
    int max;
} js_t;

static void js_ws(js_t *j) { while (j->p < j->e && isspace((unsigned char)*j->p)) j->p++; }

/* A string; its text (escapes kept as they are, which for keys is fine)
 * copied into out if given. */
static bool js_str(js_t *j, char *out, size_t n)
{
    if (j->p >= j->e || *j->p != '"') return false;
    j->p++;
    size_t k = 0;
    while (j->p < j->e && *j->p != '"') {
        if (*j->p == '\\' && j->p + 1 < j->e) {
            if (out && k + 1 < n) out[k++] = j->p[1];
            j->p += 2;
            continue;
        }
        if (out && k + 1 < n) out[k++] = *j->p;
        j->p++;
    }
    if (out && n) out[k] = 0;
    if (j->p >= j->e) return false;
    j->p++;
    return true;
}

static void js_record(js_t *j, const char *path, float v)
{
    if (!path || !path[0] || j->nf >= j->max) return;
    snprintf(j->names[j->nf], AOS_MQTT_FIELD_MAX, "%s", path);
    j->vals[j->nf++] = v;
}

static bool js_value(js_t *j, const char *path, int depth);

static bool js_object(js_t *j, const char *path, int depth)
{
    j->p++;                             /* { */
    js_ws(j);
    if (j->p < j->e && *j->p == '}') { j->p++; return true; }
    for (;;) {
        js_ws(j);
        char key[AOS_MQTT_FIELD_MAX];
        if (!js_str(j, key, sizeof key)) return false;
        js_ws(j);
        if (j->p >= j->e || *j->p != ':') return false;
        j->p++;
        char sub[AOS_MQTT_FIELD_MAX];
        const char *np = NULL;
        if (depth < 2) {
            if (path && path[0]) snprintf(sub, sizeof sub, "%.12s.%.18s", path, key);
            else snprintf(sub, sizeof sub, "%s", key);
            np = sub;
        }
        if (!js_value(j, np, depth + 1)) return false;
        js_ws(j);
        if (j->p >= j->e) return false;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; return true; }
        return false;
    }
}

static bool js_array(js_t *j, int depth)
{
    j->p++;                             /* [ */
    js_ws(j);
    if (j->p < j->e && *j->p == ']') { j->p++; return true; }
    for (;;) {
        if (!js_value(j, NULL, depth + 1)) return false;
        js_ws(j);
        if (j->p >= j->e) return false;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; return true; }
        return false;
    }
}

static bool js_value(js_t *j, const char *path, int depth)
{
    if (depth > 8) return false;
    js_ws(j);
    if (j->p >= j->e) return false;
    char c = *j->p;
    if (c == '{') return js_object(j, path, depth);
    if (c == '[') return js_array(j, depth);
    if (c == '"') return js_str(j, NULL, 0);
    if (c == 't' && j->e - j->p >= 4 && !memcmp(j->p, "true", 4)) { j->p += 4; js_record(j, path, 1); return true; }
    if (c == 'f' && j->e - j->p >= 5 && !memcmp(j->p, "false", 5)) { j->p += 5; js_record(j, path, 0); return true; }
    if (c == 'n' && j->e - j->p >= 4 && !memcmp(j->p, "null", 4)) { j->p += 4; return true; }
    if (c == '-' || isdigit((unsigned char)c)) {
        char num[32];
        size_t k = 0;
        while (j->p < j->e && strchr("+-.eE0123456789", *j->p)) {
            if (k + 1 < sizeof num) num[k++] = *j->p;
            j->p++;
        }
        num[k] = 0;
        double v = strtod(num, NULL);
        if (isfinite(v)) js_record(j, path, (float)v);
        return true;
    }
    return false;
}

/* A payload that is a number and nothing else (spaces aside). */
static bool plain_number(const uint8_t *p, size_t n, float *out)
{
    while (n && isspace(*p)) { p++; n--; }
    while (n && isspace(p[n - 1])) n--;
    if (!n || n > 31) return false;
    char b[32];
    for (size_t i = 0; i < n; i++) {
        if (!strchr("+-.eE0123456789", p[i])) return false;
        b[i] = (char)p[i];
    }
    b[n] = 0;
    char *end;
    double v = strtod(b, &end);
    if (*end || !isfinite(v) || !strpbrk(b, "0123456789")) return false;
    *out = (float)v;
    return true;
}

static bool is_text(const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) {
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return false;
            i++;
            continue;
        }
        size_t k = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
        if (!k) return false;
        if (i + k >= n) return true;        /* a character cut by the end of a cut payload */
        for (size_t m = 1; m <= k; m++) if ((p[i + m] & 0xC0) != 0x80) return false;
        i += k + 1;
    }
    return true;
}

void aos_mqtt_pretty(const char *s, char *out, size_t n)
{
    if (!out || !n) return;
    out[0] = 0;
    if (!s) return;
    const char *q = s;
    while (*q && isspace((unsigned char)*q)) q++;
    if (*q != '{' && *q != '[') { snprintf(out, n, "%s", s); return; }
    size_t k = 0;
    int ind = 0;
    bool str = false, esc = false;
#define PUT(ch) do { if (k + 1 < n) out[k++] = (ch); } while (0)
#define NL() do { PUT('\n'); for (int _i = 0; _i < ind * 2; _i++) PUT(' '); } while (0)
    for (const char *p = q; *p; p++) {
        char c = *p;
        if (str) {
            if (!esc && c == '\\' && p[1] == 'u' && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3])
                && isxdigit((unsigned char)p[4]) && isxdigit((unsigned char)p[5])) {
                /* "\u00b0C" reads better as "°C" */
                char hx[5] = { p[2], p[3], p[4], p[5], 0 };
                unsigned u = (unsigned)strtoul(hx, NULL, 16);
                if (u >= 0x20 && (u < 0xD800 || u > 0xDFFF)) {
                    if (u < 0x80) PUT((char)u);
                    else if (u < 0x800) { PUT((char)(0xC0 | u >> 6)); PUT((char)(0x80 | (u & 0x3F))); }
                    else { PUT((char)(0xE0 | u >> 12)); PUT((char)(0x80 | ((u >> 6) & 0x3F))); PUT((char)(0x80 | (u & 0x3F))); }
                    p += 5;
                    continue;
                }
            }
            PUT(c);
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') str = false;
            continue;
        }
        if (isspace((unsigned char)c)) continue;
        if (c == '"') { str = true; PUT(c); continue; }
        if (c == '{' || c == '[') {
            PUT(c);
            const char *r = p + 1;
            while (*r && isspace((unsigned char)*r)) r++;
            if (*r == (c == '{' ? '}' : ']')) { PUT(*r); p = r; continue; }
            ind++;
            NL();
        } else if (c == '}' || c == ']') {
            if (ind) ind--;
            NL();
            PUT(c);
        } else if (c == ',') {
            PUT(c);
            NL();
        } else if (c == ':') {
            PUT(':');
            PUT(' ');
        } else {
            PUT(c);
        }
    }
#undef PUT
#undef NL
    out[k] = 0;
}

/* -------------------------------------------------------------------------- */
/* The table                                                                   */
/* -------------------------------------------------------------------------- */

void aos_mqtt_lock(void) { if (M.mx) aos_hal_mutex_lock(M.mx); }
void aos_mqtt_unlock(void) { if (M.mx) aos_hal_mutex_unlock(M.mx); }
int aos_mqtt_topic_count(void) { return M.ntop; }

const aos_mqtt_topic_t *aos_mqtt_topic_at(int i)
{
    return B && i >= 0 && i < AOS_MQTT_MAX_TOPICS && s_top[i].used ? &s_top[i] : NULL;
}

static int find_n(const char *t, size_t n, uint32_t h)
{
    if (!B) return -1;
    for (int i = 0; i < AOS_MQTT_MAX_TOPICS; i++)
        if (s_top[i].used && s_top[i].hash == h && !strncmp(s_top[i].topic, t, n) && !s_top[i].topic[n]) return i;
    return -1;
}

int aos_mqtt_topic_find(const char *t)
{
    if (!t) return -1;
    size_t n = strlen(t);
    return find_n(t, n, fnv(t, n));
}

static void series_free_all(int slot)
{
    for (int s = s_top[slot].series; s >= 0;) {
        int nx = s_ser[s].next;
        s_ser[s].topic = -1;
        s_ser[s].next = -1;
        s = nx;
    }
    s_top[slot].series = -1;
    s_top[slot].nfields = 0;
}

static void series_unlink(int s)
{
    int owner = s_ser[s].topic;
    if (owner < 0) return;
    int16_t *link = &s_top[owner].series;
    while (*link >= 0 && *link != s) link = &s_ser[*link].next;
    if (*link == s) *link = s_ser[s].next;
    if (s_top[owner].nfields) s_top[owner].nfields--;
    s_ser[s].topic = -1;
    s_ser[s].next = -1;
}

static int series_get(int slot, const char *field, uint32_t now)
{
    int16_t *link = &s_top[slot].series;
    while (*link >= 0) {
        if (!strcmp(s_ser[*link].field, field)) return *link;
        link = &s_ser[*link].next;
    }
    if (s_top[slot].nfields >= AOS_MQTT_FIELDS) return -1;
    int pick = -1;
    for (int s = 0; s < AOS_MQTT_SERIES; s++) if (s_ser[s].topic < 0) { pick = s; break; }
    if (pick < 0) {
        /* full: the series heard from least recently goes */
        uint32_t oldest = 0;
        for (int s = 0; s < AOS_MQTT_SERIES; s++) {
            uint32_t age = now - s_ser[s].last_ms;
            if (s_ser[s].topic != slot && (pick < 0 || age > oldest)) { oldest = age; pick = s; }
        }
        if (pick < 0) return -1;
        series_unlink(pick);
        /* the owner's chain may have been ours to walk: find the tail again */
        link = &s_top[slot].series;
        while (*link >= 0) link = &s_ser[*link].next;
    }
    series_t *se = &s_ser[pick];
    memset(se, 0, sizeof *se);
    se->topic = (int16_t)slot;
    se->next = -1;
    scpy(se->field, sizeof se->field, field);
    *link = (int16_t)pick;
    s_top[slot].nfields++;
    return pick;
}

static void series_push(int slot, const char *field, float v, uint32_t now)
{
    int s = series_get(slot, field, now);
    if (s < 0) return;
    series_t *se = &s_ser[s];
    se->v[se->head] = v;
    se->t[se->head] = now;
    se->head = (uint8_t)((se->head + 1) % AOS_MQTT_HIST);
    if (se->n < AOS_MQTT_HIST) se->n++;
    se->last_ms = now;
}

static int slot_for(const char *t, size_t n, uint32_t h, uint32_t now)
{
    int i = find_n(t, n, h);
    if (i >= 0) return i;
    int pick = -1;
    for (int k = 0; k < AOS_MQTT_MAX_TOPICS; k++) if (!s_top[k].used) { pick = k; break; }
    if (pick < 0) {
        uint32_t oldest = 0;
        for (int k = 0; k < AOS_MQTT_MAX_TOPICS; k++) {
            uint32_t age = now - s_top[k].last_ms;
            if (pick < 0 || age > oldest) { oldest = age; pick = k; }
        }
        series_free_all(pick);
        M.st.evicted++;
        M.ntop--;
    }
    aos_mqtt_topic_t *e = &s_top[pick];
    memset(e, 0, sizeof *e);
    memcpy(e->topic, t, n);
    e->topic[n] = 0;
    e->hash = h;
    e->series = -1;
    e->first_ms = now;
    e->used = true;
    M.ntop++;
    return pick;
}

/* One message into the table. payload holds 'have' bytes of 'len'. */
static void table_put(const char *t, size_t tn, const uint8_t *payload, size_t have, uint32_t len, int qos, bool retained)
{
    if (tn >= AOS_MQTT_TOPIC_MAX) { lock(); M.st.too_long++; bump(); unlock(); return; }
    /* the numbers first, outside the lock */
    char names[AOS_MQTT_FIELDS][AOS_MQTT_FIELD_MAX];
    float vals[AOS_MQTT_FIELDS];
    int nf = 0;
    uint8_t kind = AOS_MQTT_TEXT;
    float num;
    size_t o = 0;
    while (o < have && isspace(payload[o])) o++;
    if (!len) kind = AOS_MQTT_EMPTY;
    else if (have == len && plain_number(payload, have, &num)) {
        kind = AOS_MQTT_NUMBER;
        names[0][0] = 0;
        vals[0] = num;
        nf = 1;
    } else if (o < have && (payload[o] == '{' || payload[o] == '[')) {
        kind = AOS_MQTT_JSON;
        js_t j = { .p = (const char *)payload + o, .e = (const char *)payload + have, .names = names, .vals = vals, .max = AOS_MQTT_FIELDS };
        js_value(&j, "", 0);        /* a cut payload stops where it stops: what came before counts */
        nf = j.nf;
    } else if (!is_text(payload, have)) {
        kind = AOS_MQTT_BINARY;
    }
    uint32_t now = now_ms();
    int64_t wall = aos_hal_time_is_valid() ? (int64_t)time(NULL) : 0;
    uint32_t h = fnv(t, tn);
    lock();
    int i = slot_for(t, tn, h, now);
    aos_mqtt_topic_t *e = &s_top[i];
    size_t keep = have < AOS_MQTT_PAYLOAD_MAX - 1 ? have : AOS_MQTT_PAYLOAD_MAX - 1;
    memcpy(e->payload, payload, keep);
    e->payload[keep] = 0;
    if (kind == AOS_MQTT_BINARY) for (size_t k = 0; k < keep; k++) if (!e->payload[k]) e->payload[k] = '.';
    if (keep < len) M.st.cut++;
    e->len = len;
    e->count++;
    e->last_ms = now;
    e->last_wall = wall;
    e->qos = (uint8_t)qos;
    e->retained = retained;
    e->kind = kind;
    for (int k = 0; k < nf; k++) series_push(i, names[k], vals[k], now);
    M.st.rx_msgs++;
    bump();
    e->seq = M.version;
    unlock();
}

int aos_mqtt_fields(int slot, char names[][AOS_MQTT_FIELD_MAX], float *last, int max)
{
    if (!B || slot < 0 || slot >= AOS_MQTT_MAX_TOPICS || !s_top[slot].used) return 0;
    int n = 0;
    for (int s = s_top[slot].series; s >= 0 && n < max; s = s_ser[s].next) {
        if (names) snprintf(names[n], AOS_MQTT_FIELD_MAX, "%s", s_ser[s].field);
        if (last) last[n] = s_ser[s].n ? s_ser[s].v[(s_ser[s].head + AOS_MQTT_HIST - 1) % AOS_MQTT_HIST] : NAN;
        n++;
    }
    return n;
}

int aos_mqtt_history(int slot, const char *field, float *v, uint32_t *t_ms, int max)
{
    if (!B || slot < 0 || slot >= AOS_MQTT_MAX_TOPICS || !s_top[slot].used || !field) return 0;
    for (int s = s_top[slot].series; s >= 0; s = s_ser[s].next) {
        if (strcmp(s_ser[s].field, field)) continue;
        const series_t *se = &s_ser[s];
        int n = se->n < max ? se->n : max;
        int start = (se->head + AOS_MQTT_HIST - n) % AOS_MQTT_HIST;
        for (int k = 0; k < n; k++) {
            int idx = (start + k) % AOS_MQTT_HIST;
            if (v) v[k] = se->v[idx];
            if (t_ms) t_ms[k] = se->t[idx];
        }
        return n;
    }
    return 0;
}

void aos_mqtt_clear(void)
{
    if (!M.mx || !B) return;
    lock();
    for (int i = 0; i < AOS_MQTT_MAX_TOPICS; i++) s_top[i].used = false;
    for (int s = 0; s < AOS_MQTT_SERIES; s++) { s_ser[s].topic = -1; s_ser[s].next = -1; }
    M.ntop = 0;
    bump();
    unlock();
}

/* -------------------------------------------------------------------------- */
/* Configuration                                                               */
/* -------------------------------------------------------------------------- */

static void pref_str(const char *k, char *out, size_t n, const char *def)
{
    if (!aos_hal_pref_get_str(k, out, n)) snprintf(out, n, "%s", def);
}

static void cfg_load(void)
{
    aos_mqtt_config_t *c = &M.cfg;
    int32_t v;
    c->enabled = aos_hal_pref_get_i32("mqtt_on", &v) ? v != 0 : true;
    pref_str("mqtt_host", c->host, sizeof c->host, "");
    c->port = aos_hal_pref_get_i32("mqtt_port", &v) && v > 0 && v < 65536 ? v : 1883;
    pref_str("mqtt_user", c->user, sizeof c->user, "");
    pref_str("mqtt_pass", c->pass, sizeof c->pass, "");
    pref_str("mqtt_client", c->client_id, sizeof c->client_id, "");
    pref_str("mqtt_sub", c->sub, sizeof c->sub, "#");
    c->keepalive = aos_hal_pref_get_i32("mqtt_ka", &v) && v >= 0 && v <= 65535 ? v : 30;
    if (!aos_hal_pref_get_str("mqtt_autoid", M.autoid, sizeof M.autoid) || !M.autoid[0]) {
        uint32_t r = fnv(aos_hal_device_name(), strlen(aos_hal_device_name())) ^ (uint32_t)aos_hal_uptime_ms() * 2654435761u ^ (uint32_t)rand();
        snprintf(M.autoid, sizeof M.autoid, "%.30s-%04x", aos_hal_device_name(), (unsigned)(r & 0xFFFF));
        aos_hal_pref_set_str("mqtt_autoid", M.autoid);
    }
}

static void cfg_save(void)
{
    const aos_mqtt_config_t *c = &M.cfg;
    aos_hal_pref_set_i32("mqtt_on", c->enabled ? 1 : 0);
    aos_hal_pref_set_str("mqtt_host", c->host);
    aos_hal_pref_set_i32("mqtt_port", c->port);
    aos_hal_pref_set_str("mqtt_user", c->user);
    aos_hal_pref_set_str("mqtt_pass", c->pass);
    aos_hal_pref_set_str("mqtt_client", c->client_id);
    aos_hal_pref_set_str("mqtt_sub", c->sub);
    aos_hal_pref_set_i32("mqtt_ka", c->keepalive);
}

void aos_mqtt_config(aos_mqtt_config_t *out)
{
    if (!M.mx) { memset(out, 0, sizeof *out); return; }
    lock();
    *out = M.cfg;
    if (out->pass[0]) snprintf(out->pass, sizeof out->pass, "%s", AOS_MQTT_PASS_KEPT);
    unlock();
}

const char *aos_mqtt_client_id(void) { return M.cid[0] ? M.cid : (M.cfg.client_id[0] ? M.cfg.client_id : M.autoid); }

static void trim(char *s)
{
    size_t l = strlen(s);
    while (l && isspace((unsigned char)s[l - 1])) s[--l] = 0;
    size_t i = 0;
    while (isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, l - i + 1);
}

bool aos_mqtt_set_config(const aos_mqtt_config_t *c)
{
    if (!c || !M.mx) return false;
    aos_mqtt_config_t n = *c;
    trim(n.host);
    trim(n.user);
    trim(n.client_id);
    trim(n.sub);
    if (n.port <= 0 || n.port > 65535) n.port = 1883;
    if (n.keepalive < 0 || n.keepalive > 65535) n.keepalive = 30;
    if (!n.sub[0]) snprintf(n.sub, sizeof n.sub, "#");
    lock();
    if (!strcmp(n.pass, AOS_MQTT_PASS_KEPT)) snprintf(n.pass, sizeof n.pass, "%s", M.cfg.pass);
    bool same = !memcmp(&n, &M.cfg, sizeof n);
    M.cfg = n;
    if (!same) { M.cfg_gen++; M.kick = true; }
    bump();
    unlock();
    if (!same) cfg_save();
    return true;
}

void aos_mqtt_set_enabled(bool on)
{
    if (!M.mx) return;
    lock();
    bool ch = M.cfg.enabled != on;
    M.cfg.enabled = on;
    if (ch) { M.cfg_gen++; M.kick = true; }
    bump();
    unlock();
    if (ch) aos_hal_pref_set_i32("mqtt_on", on ? 1 : 0);
}

void aos_mqtt_reconnect(void) { M.kick = true; }

bool aos_mqtt_import_file(void)
{
    const char *root = aos_hal_path_sd_root();
    if (!root || !M.mx) return false;
    char path[160];
    snprintf(path, sizeof path, "%s/mqtt.txt", root);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char *lines[32];
    int nl = 0;
    char buf[256];
    bool got = false, pass_moved = false;
    lock();
    aos_mqtt_config_t c = M.cfg;
    unlock();
    aos_mqtt_config_t was = c;
    while (nl < 32 && fgets(buf, sizeof buf, f)) {
        char *eq = strchr(buf, '=');
        char key[20] = "", val[160] = "";
        if (eq && buf[0] != '#') {
            snprintf(key, sizeof key, "%.*s", (int)(eq - buf < 19 ? eq - buf : 19), buf);
            snprintf(val, sizeof val, "%s", eq + 1);
            trim(key);
            trim(val);
            for (char *p = key; *p; p++) *p = (char)tolower((unsigned char)*p);
        }
        if (!strcmp(key, "host") || !strcmp(key, "broker") || !strcmp(key, "server")) {
            /* host:port is fine too */
            char *colon = strrchr(val, ':');
            if (colon && !strchr(colon, ']') && atoi(colon + 1) > 0) { c.port = atoi(colon + 1); *colon = 0; }
            scpy(c.host, sizeof c.host, val);
        } else if (!strcmp(key, "port") && atoi(val) > 0) {
            c.port = atoi(val);
        } else if (!strcmp(key, "user") || !strcmp(key, "username")) {
            scpy(c.user, sizeof c.user, val);
        } else if ((!strcmp(key, "password") || !strcmp(key, "pass")) && strcmp(val, AOS_MQTT_PASS_KEPT)) {
            scpy(c.pass, sizeof c.pass, val);
            pass_moved = true;
            snprintf(buf, sizeof buf, "%s=" AOS_MQTT_PASS_KEPT "\n", key);
        } else if (!strcmp(key, "client_id") || !strcmp(key, "client")) {
            scpy(c.client_id, sizeof c.client_id, val);
        } else if (!strcmp(key, "sub") || !strcmp(key, "subscribe") || !strcmp(key, "topic")) {
            if (val[0]) scpy(c.sub, sizeof c.sub, val);
        } else if (!strcmp(key, "keepalive") && atoi(val) >= 0) {
            c.keepalive = atoi(val);
        }
        lines[nl++] = strdup(buf);
    }
    fclose(f);
    if (memcmp(&c, &was, sizeof c)) {
        c.enabled = true;
        aos_mqtt_set_config(&c);
        got = true;
    }
    /* the password does not stay on the card in the clear */
    if (pass_moved && (f = fopen(path, "w"))) {
        for (int i = 0; i < nl; i++) if (lines[i]) fputs(lines[i], f);
        fclose(f);
        aos_hal_log("mqtt", "password moved from mqtt.txt into the preferences");
    }
    for (int i = 0; i < nl; i++) free(lines[i]);
    return got;
}

/* -------------------------------------------------------------------------- */
/* Publishing and subscribing, from any task                                   */
/* -------------------------------------------------------------------------- */

bool aos_mqtt_publish_len(const char *topic, const void *payload, int len, int qos, bool retain)
{
    aos_mqtt_start();
    if (len < 0) len = payload ? (int)strlen(payload) : 0;
    if (!valid_topic_name(topic) || len > AOS_MQTT_PUB_MAX || !M.mx || !B) return false;
    lock();
    bool ok = M.cfg.host[0] && M.cfg.enabled && M.qn < Q_LEN;
    if (ok) {
        pubq_t *q = &s_q[M.qn++];
        snprintf(q->topic, sizeof q->topic, "%s", topic);
        if (len) memcpy(q->payload, payload, (size_t)len);
        q->len = (uint16_t)len;
        q->qos = qos > 0 ? 1 : 0;
        q->retain = retain;
        q->ms = now_ms();
        M.st.queued = (uint32_t)M.qn;
        bump();
    }
    unlock();
    return ok;
}

bool aos_mqtt_publish(const char *topic, const char *payload, int qos, bool retain)
{
    return aos_mqtt_publish_len(topic, payload, -1, qos, retain);
}

bool aos_mqtt_subscribe(const char *filter, int qos)
{
    aos_mqtt_start();
    if (!filter || !filter[0] || strlen(filter) >= AOS_MQTT_TOPIC_MAX || !M.mx || !B) return false;
    lock();
    int free_i = -1;
    bool ok = false;
    for (int i = 0; i < XSUBS; i++) {
        if (B->xs[i].st && B->xs[i].st != 3 && !strcmp(B->xs[i].f, filter)) { ok = true; break; }
        if (!B->xs[i].st && free_i < 0) free_i = i;
    }
    if (!ok && free_i >= 0) {
        snprintf(B->xs[free_i].f, sizeof B->xs[free_i].f, "%s", filter);
        B->xs[free_i].qos = qos > 0 ? 1 : 0;
        B->xs[free_i].st = 1;
        ok = true;
    }
    unlock();
    return ok;
}

bool aos_mqtt_unsubscribe(const char *filter)
{
    if (!filter || !M.mx || !B) return false;
    lock();
    bool ok = false;
    for (int i = 0; i < XSUBS; i++) {
        if (B->xs[i].st && !strcmp(B->xs[i].f, filter)) {
            B->xs[i].st = B->xs[i].st == 2 ? 3 : 0;
            ok = true;
        }
    }
    unlock();
    return ok;
}

/* -------------------------------------------------------------------------- */
/* Packets                                                                     */
/* -------------------------------------------------------------------------- */

static size_t put_u16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; return 2; }

static size_t put_str(uint8_t *b, const char *s, size_t n)
{
    put_u16(b, (uint16_t)n);
    memcpy(b + 2, s, n);
    return n + 2;
}

/* Writes the fixed header in front of a body already at s_tx + 5 and
 * returns where the packet starts (the header is 2..5 bytes). */
static uint8_t *frame(uint8_t type, size_t body, size_t *total)
{
    uint8_t hdr[5];
    size_t k = 0;
    hdr[k++] = type;
    size_t x = body;
    do {
        uint8_t d = x % 128;
        x /= 128;
        if (x) d |= 0x80;
        hdr[k++] = d;
    } while (x && k < 5);
    uint8_t *start = s_tx + 5 - k;
    memcpy(start, hdr, k);
    *total = k + body;
    return start;
}

static bool tx(const void *p, size_t n)
{
    if (M.h <= 0) return false;
    if (aos_hal_tcp_send(M.h, p, (int)n, 3000) != (int)n) return false;
    M.last_tx = now_ms();
    lock();
    M.st.tx_bytes += (uint32_t)n;
    unlock();
    return true;
}

static uint16_t pid_next(void)
{
    if (++M.next_pid == 0) M.next_pid = 1;
    return M.next_pid;
}

static bool send_connect(const aos_mqtt_config_t *c)
{
    uint8_t *b = s_tx + 5;
    size_t k = 0;
    k += put_str(b + k, "MQTT", 4);
    b[k++] = 4;                         /* 3.1.1 */
    bool user = c->user[0] != 0, pass = user && c->pass[0];
    b[k++] = (uint8_t)(0x02 | (user ? 0x80 : 0) | (pass ? 0x40 : 0));
    k += put_u16(b + k, (uint16_t)c->keepalive);
    k += put_str(b + k, M.cid, strlen(M.cid));
    if (user) k += put_str(b + k, c->user, strlen(c->user));
    if (pass) k += put_str(b + k, c->pass, strlen(c->pass));
    size_t n;
    uint8_t *p = frame(0x10, k, &n);
    return tx(p, n);
}

static bool send_simple(uint8_t type, uint16_t pid)
{
    uint8_t b[4] = { type, 2, (uint8_t)(pid >> 8), (uint8_t)pid };
    return tx(b, 4);
}

/* SUBSCRIBE with the base filters and every extra one waiting (st 1 or 2
 * when 'all'). */
static bool send_subscribe(bool all)
{
    uint8_t *b = s_tx + 5;
    size_t k = 0;
    M.sub_pid = pid_next();
    k += put_u16(b + k, M.sub_pid);
    int count = 0;
    if (all) {
        char list[sizeof M.cfg.sub];
        lock();
        snprintf(list, sizeof list, "%s", M.cfg.sub);
        unlock();
        char *save = NULL;
        for (char *f = strtok_r(list, ", ", &save); f; f = strtok_r(NULL, ", ", &save)) {
            size_t l = strlen(f);
            if (k + l + 3 > PKT_MAX - 8) break;
            k += put_str(b + k, f, l);
            b[k++] = 1;
            count++;
        }
    }
    lock();
    for (int i = 0; i < XSUBS; i++) {
        xsub_t *x = &B->xs[i];
        if (!(x->st == 1 || (all && x->st == 2))) continue;
        size_t l = strlen(x->f);
        if (k + l + 3 > PKT_MAX - 8) break;
        k += put_str(b + k, x->f, l);
        b[k++] = x->qos;
        x->st = 2;
        count++;
    }
    unlock();
    if (!count) return true;
    M.sub_count = count;
    size_t n;
    uint8_t *p = frame(0x82, k, &n);
    return tx(p, n);
}

static bool send_unsubscribe(void)
{
    uint8_t *b = s_tx + 5;
    size_t k = 0;
    k += put_u16(b + k, pid_next());
    int count = 0;
    lock();
    for (int i = 0; i < XSUBS; i++) {
        xsub_t *x = &B->xs[i];
        if (x->st != 3) continue;
        size_t l = strlen(x->f);
        if (k + l + 2 > PKT_MAX - 8) break;
        k += put_str(b + k, x->f, l);
        x->st = 0;
        count++;
    }
    unlock();
    if (!count) return true;
    size_t n;
    uint8_t *p = frame(0xA2, k, &n);
    return tx(p, n);
}

/* Sends one queued publish; false only when the socket failed. */
static bool send_queued(void)
{
    for (;;) {
        lock();
        if (!M.qn) { unlock(); return true; }
        pubq_t q = s_q[0];
        int slot = -1;
        if (q.qos) for (int i = 0; i < INFLIGHT; i++) if (!s_inf[i].used) { slot = i; break; }
        if (q.qos && slot < 0) { unlock(); return true; }       /* wait for a PUBACK */
        memmove(s_q, s_q + 1, (size_t)(M.qn - 1) * sizeof s_q[0]);
        M.qn--;
        M.st.queued = (uint32_t)M.qn;
        unlock();
        uint8_t *b = s_tx + 5;
        size_t k = put_str(b, q.topic, strlen(q.topic));
        uint16_t pid = 0;
        if (q.qos) { pid = pid_next(); k += put_u16(b + k, pid); }
        memcpy(b + k, q.payload, q.len);
        k += q.len;
        size_t n;
        uint8_t *p = frame((uint8_t)(0x30 | (q.qos << 1) | (q.retain ? 1 : 0)), k, &n);
        if (q.qos) {
            inflight_t *f = &s_inf[slot];
            f->used = true;
            f->pid = pid;
            f->sent_ms = now_ms();
            f->len = (uint16_t)n;
            memcpy(f->pkt, p, n);
            lock(); M.st.pending++; unlock();
        }
        if (!tx(p, n)) return false;
        lock(); M.st.tx_msgs++; bump(); unlock();
    }
}

static bool resend_inflight(bool all)
{
    uint32_t now = now_ms();
    for (int i = 0; i < INFLIGHT; i++) {
        inflight_t *f = &s_inf[i];
        if (!f->used || (!all && now - f->sent_ms < RESEND_MS)) continue;
        f->pkt[0] |= 0x08;              /* DUP */
        f->sent_ms = now;
        if (!tx(f->pkt, f->len)) return false;
    }
    return true;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* The remaining length. 1 done, 0 more bytes needed, -1 malformed. */
static int parse_len(const uint8_t *b, int n, uint32_t *rem, int *hdr)
{
    uint32_t v = 0, mul = 1;
    for (int i = 1; i < 5; i++) {
        if (i >= n) return 0;
        v += (uint32_t)(b[i] & 0x7F) * mul;
        mul *= 128;
        if (!(b[i] & 0x80)) { *rem = v; *hdr = i + 1; return 1; }
    }
    return -1;
}

/* One packet (or the head of one too big for the buffer: have < total).
 * Returns false when the connection must go. connack gets the CONNACK's
 * return code if one came (else stays). */
static bool on_packet(const uint8_t *b, int have, uint32_t total, int hdr, int *connack)
{
    uint8_t type = b[0] >> 4;
    const uint8_t *v = b + hdr;
    int vn = have - hdr;
    switch (type) {
    case 2:                             /* CONNACK */
        if (vn >= 2 && connack) *connack = v[1];
        return true;
    case 3: {                           /* PUBLISH */
        int qos = (b[0] >> 1) & 3;
        bool retain = b[0] & 1;
        if (vn < 2) return false;
        uint16_t tl = rd16(v);
        int off = 2 + tl + (qos ? 2 : 0);
        if (off > vn) return false;
        uint16_t pid = qos ? rd16(v + 2 + tl) : 0;
        uint32_t plen = total - (uint32_t)hdr - (uint32_t)off;
        table_put((const char *)v + 2, tl, v + off, (size_t)(vn - off), plen, qos, retain);
        if (qos == 1) return send_simple(0x40, pid);
        if (qos == 2) return send_simple(0x50, pid);
        return true;
    }
    case 4:                             /* PUBACK */
        if (vn >= 2) {
            uint16_t pid = rd16(v);
            for (int i = 0; i < INFLIGHT; i++) {
                if (s_inf[i].used && s_inf[i].pid == pid) {
                    s_inf[i].used = false;
                    lock(); if (M.st.pending) M.st.pending--; bump(); unlock();
                }
            }
        }
        return true;
    case 6:                             /* PUBREL (their QoS 2) */
        return vn >= 2 ? send_simple(0x70, rd16(v)) : false;
    case 5: case 7:                     /* PUBREC / PUBCOMP: we never send QoS 2 */
        return true;
    case 9: {                           /* SUBACK */
        int bad = 0;
        for (int i = 2; i < vn; i++) if (v[i] & 0x80) bad++;
        lock();
        M.st.subs_refused = bad;
        if (bad) snprintf(M.err, sizeof M.err, _("El broker rechazó %d de %d suscripciones"), bad, vn - 2);
        bump();
        unlock();
        return true;
    }
    case 11:                            /* UNSUBACK */
        return true;
    case 13:                            /* PINGRESP */
        if (M.ping_sent) {
            lock();
            M.st.ping_ms = (int32_t)(now_ms() - M.ping_sent);
            bump();
            unlock();
            M.ping_sent = 0;
        }
        return true;
    default:
        return false;
    }
}

/* Reads what is there and handles every whole packet. false = drop. */
static bool pump(int timeout_ms, int *connack)
{
    if (M.rx_len >= RX_SIZE) M.rx_len = 0;
    int r = aos_hal_tcp_recv(M.h, s_rx + M.rx_len, RX_SIZE - M.rx_len, timeout_ms);
    if (r < 0 && r != AOS_TCP_ERR_TIMEOUT) return false;
    if (r > 0) {
        lock(); M.st.rx_bytes += (uint32_t)r; unlock();
        int n = r;
        if (M.skip) {
            /* the tail of a packet too big for the buffer */
            int s = (uint32_t)n < M.skip ? n : (int)M.skip;
            M.skip -= (uint32_t)s;
            memmove(s_rx + M.rx_len, s_rx + M.rx_len + s, (size_t)(n - s));
            n -= s;
        }
        M.rx_len += n;
    }
    int pos = 0;
    while (M.rx_len - pos >= 2 && !M.skip) {
        uint32_t rem;
        int hdr;
        int pr = parse_len(s_rx + pos, M.rx_len - pos, &rem, &hdr);
        if (pr < 0) return false;
        if (!pr) break;
        uint32_t total = (uint32_t)hdr + rem;
        int avail = M.rx_len - pos;
        if (total <= (uint32_t)avail) {
            if (!on_packet(s_rx + pos, (int)total, total, hdr, connack)) return false;
            pos += (int)total;
        } else if (total > RX_SIZE && pos == 0 && M.rx_len == RX_SIZE) {
            /* bigger than the buffer: its head now, the rest skipped */
            if (!on_packet(s_rx, avail, total, hdr, connack)) return false;
            M.skip = total - (uint32_t)avail;
            pos = M.rx_len;
        } else {
            break;
        }
    }
    if (pos) {
        memmove(s_rx, s_rx + pos, (size_t)(M.rx_len - pos));
        M.rx_len -= pos;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* The thread                                                                  */
/* -------------------------------------------------------------------------- */

static void drop_stale_queue(void)
{
    uint32_t now = now_ms();
    lock();
    int k = 0;
    for (int i = 0; i < M.qn; i++) if (now - s_q[i].ms < Q_WAIT_MS) s_q[k++] = s_q[i];
    M.qn = k;
    M.st.queued = (uint32_t)k;
    unlock();
}

static void tcp_error_text(int rc, const aos_mqtt_config_t *c, char *e, size_t n)
{
    if (rc == AOS_TCP_ERR_DNS) snprintf(e, n, _("No encuentro %.60s en la red"), c->host);
    else if (rc == AOS_TCP_ERR_SLOTS) snprintf(e, n, "%s", _("No quedan conexiones TCP libres"));
    else snprintf(e, n, _("%.60s:%d no contesta"), c->host, c->port);
}

static const char *connack_text(int rc)
{
    switch (rc) {
    case 1: return _("El broker no habla MQTT 3.1.1");
    case 2: return _("El broker rechazó el ID de cliente");
    case 3: return _("El broker no está disponible");
    case 4: return _("Usuario o contraseña incorrectos");
    case 5: return _("El broker no autorizó la conexión");
    default: return _("El broker rechazó la conexión");
    }
}

static void service_thread(void *arg)
{
    (void)arg;
    int backoff = 0;
    uint32_t last_import = 0, refused_gen = UINT32_MAX;
    aos_mqtt_import_file();
    for (;;) {
        lock();
        M.kick = false;
        aos_mqtt_config_t c = M.cfg;
        uint32_t gen = M.cfg_gen;
        unlock();
        if (!c.enabled) { set_state(AOS_MQTT_OFF, ""); drop_stale_queue(); aos_hal_sleep_ms(300); continue; }
        if (!c.host[0]) {
            set_state(AOS_MQTT_UNCONFIGURED, "");
            if (now_ms() - last_import > 5000) { last_import = now_ms(); aos_mqtt_import_file(); }
            drop_stale_queue();
            aos_hal_sleep_ms(500);
            continue;
        }
        if (gen == refused_gen) { drop_stale_queue(); aos_hal_sleep_ms(300); continue; }
        if (aos_hal_net_state() != AOS_NET_CONNECTED) {
            set_state(AOS_MQTT_WAITING_NET, "");
            drop_stale_queue();
            aos_hal_sleep_ms(1000);
            continue;
        }
        set_state(AOS_MQTT_CONNECTING, NULL);
        snprintf(M.cid, sizeof M.cid, "%s", c.client_id[0] ? c.client_id : M.autoid);
        int h = aos_hal_tcp_connect(c.host, c.port, 5000);
        bool ok = false;
        if (h <= 0) {
            char e[112];
            tcp_error_text(h, &c, e, sizeof e);
            set_state(AOS_MQTT_ERROR, e);
        } else {
            M.h = h;
            M.rx_len = 0;
            M.skip = 0;
            M.ping_sent = 0;
            int ack = -1;
            if (send_connect(&c)) {
                uint32_t t0 = now_ms();
                while (ack < 0 && now_ms() - t0 < 6000 && !M.kick)
                    if (!pump(100, &ack)) break;
            }
            if (ack == 0) {
                ok = true;
            } else if (ack == 4 || ack == 5) {
                refused_gen = gen;
                set_state(AOS_MQTT_REFUSED, connack_text(ack));
            } else if (ack > 0) {
                set_state(AOS_MQTT_ERROR, connack_text(ack));
            } else if (!M.kick) {
                set_state(AOS_MQTT_ERROR, _("El broker no contestó al CONNECT"));
            }
        }
        if (ok) {
            backoff = 0;
            lock();
            M.state = AOS_MQTT_CONNECTED;
            M.err[0] = 0;
            M.st.connects++;
            M.st.connected_ms = now_ms();
            M.st.ping_ms = -1;
            M.st.subs_refused = 0;
            bump();
            unlock();
            aos_hal_log("mqtt", "connected to %s:%d as %s", c.host, c.port, M.cid);
            bool alive = send_subscribe(true) && resend_inflight(true);
            /* a first PINGREQ at once: the round trip for the counters */
            static const uint8_t PING0[2] = { 0xC0, 0 };
            if (alive && c.keepalive > 0) { M.ping_sent = now_ms() ? now_ms() : 1; alive = tx(PING0, 2); }
            while (alive && !M.kick && aos_hal_net_state() == AOS_NET_CONNECTED) {
                if (!pump(50, NULL)) break;
                if (!send_queued()) break;
                bool want_sub = false, want_unsub = false;
                lock();
                for (int i = 0; i < XSUBS; i++) { want_sub |= B->xs[i].st == 1; want_unsub |= B->xs[i].st == 3; }
                unlock();
                if (want_sub && !send_subscribe(false)) break;
                if (want_unsub && !send_unsubscribe()) break;
                if (!resend_inflight(false)) break;
                uint32_t now = now_ms();
                if (c.keepalive > 0) {
                    uint32_t ka = (uint32_t)c.keepalive * 1000;
                    if (!M.ping_sent && now - M.last_tx >= ka / 2) {
                        M.ping_sent = now ? now : 1;
                        static const uint8_t PINGREQ[2] = { 0xC0, 0 };
                        if (!tx(PINGREQ, 2)) break;
                    }
                    if (M.ping_sent && now - M.ping_sent > (ka > 10000 ? ka : 10000)) {
                        set_state(AOS_MQTT_ERROR, _("El broker dejó de contestar"));
                        break;
                    }
                }
            }
            if (M.kick || !c.enabled) {
                uint8_t d[2] = { 0xE0, 0 };
                tx(d, 2);
            } else {
                lock();
                M.st.drops++;
                if (M.state == AOS_MQTT_CONNECTED) { M.state = AOS_MQTT_ERROR; snprintf(M.err, sizeof M.err, "%s", _("Se perdió la conexión con el broker")); }
                bump();
                unlock();
            }
            lock(); M.st.connected_ms = 0; bump(); unlock();
            /* extra subscriptions go again on the next session */
            lock();
            for (int i = 0; i < XSUBS; i++) if (B->xs[i].st == 3) B->xs[i].st = 0;
            unlock();
        }
        if (M.h > 0) aos_hal_tcp_close(M.h);
        M.h = -1;
        if (M.kick) continue;
        if (gen == refused_gen) continue;
        static const int PAUSE_S[] = { 1, 2, 5, 10, 30 };
        int s = PAUSE_S[backoff < 4 ? backoff++ : 4];
        for (int i = 0; i < s * 10 && !M.kick; i++) { aos_hal_sleep_ms(100); if (i % 10 == 9) drop_stale_queue(); }
    }
}

void aos_mqtt_start(void)
{
    if (M.started) return;
    M.started = true;
    M.mx = aos_hal_mutex_create();
    M.st.ping_ms = -1;
    cfg_load();
    B = big_calloc(sizeof *B);
    if (B) {
        for (int s = 0; s < AOS_MQTT_SERIES; s++) { s_ser[s].topic = -1; s_ser[s].next = -1; }
        aos_hal_log("mqtt", "tables: %u KB", (unsigned)(sizeof *B / 1024));
    }
    if (!B || !aos_hal_thread_start("mqtt", service_thread, NULL, 6144, 4)) {
        M.state = AOS_MQTT_ERROR;
        snprintf(M.err, sizeof M.err, "%s", _("No se pudo arrancar el servicio"));
    }
}

aos_mqtt_state_t aos_mqtt_state(void) { return M.state; }
uint32_t aos_mqtt_version(void) { return M.version; }

const char *aos_mqtt_error(void) { return M.err; }

void aos_mqtt_stats(aos_mqtt_stats_t *out)
{
    if (!M.mx) { memset(out, 0, sizeof *out); out->ping_ms = -1; return; }
    lock();
    *out = M.st;
    out->topics = M.ntop;
    unlock();
}
