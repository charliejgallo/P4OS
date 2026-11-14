/*
 * P4OS - Logic: the portal's page (web/logica.js), through the live channel
 * (aos_hal_live_*, docs/PORTAL-PAGES.md "Live data").
 *
 * What the app puts:
 *
 *   state  JSON, twice a second while the page looks and at once when
 *          something changes: the job and its progress, the devices, the
 *          settings, the decoders, what capture and decode are in memory
 *   data   a piece of the capture, asked for by the page ("data <seq>
 *          <from>"): a 32-byte header ("LAD1", seq, n, from, to, rate,
 *          trigger, flags) and the samples from `from` as runs, a byte of
 *          value and the run's length as a varint, up to ~60 KB; `to` is
 *          where the page asks next. Logic signals sit still most of the
 *          time, so a capture of millions of samples comes in a few pieces
 *   ann    the annotations from `from` ("LAA1", seq, total, from, to, then
 *          s0, s1, row, kind, text length, text)
 *   pkt    the packets the same way ("LAP1"; row, error, text)
 *
 * What the page sends, a line or several: "capture", "stop", "probe",
 * "dev <i>", "decode", "save", "load <path>", "data/ann/pkt <seq> <from>",
 * and any line of the settings file (rate=..., trig=..., dec2=...), which
 * the app takes as if it had been set on the screen.
 */
#include "la.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOB (60 * 1024)

static uint32_t s_state_ms, s_state_hash;
static uint8_t *s_blob;

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static size_t jstr(char *o, size_t n, const char *s)
{
    size_t k = 0;
    if (k < n) o[k++] = '"';
    for (; *s && k + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            o[k++] = '\\';
            o[k++] = (char)c;
        } else if (c < 0x20) {
            k += (size_t)snprintf(o + k, n - k, "\\u%04x", c);
        } else {
            o[k++] = (char)c;
        }
    }
    if (k < n) o[k++] = '"';
    if (k < n) o[k] = 0;
    return k;
}

#define PUT(...) do { if (k < n) k += (size_t)snprintf(o + k, n - k, __VA_ARGS__); } while (0)
#define PUTS(s) do { if (k < n) k += jstr(o + k, n - k, (s)); } while (0)

static size_t state_json(char *o, size_t n)
{
    size_t k = 0;
    la_lock();
    PUT("{\"busy\":%s,\"state\":%d,\"prog\":%u,\"total\":%u,\"err\":%s,\"msg\":", LA.busy ? "true" : "false", LA.state,
        (unsigned)LA.prog, (unsigned)LA.prog_total, LA.msg_err ? "true" : "false");
    PUTS(LA.msg);
    PUT(",\"cap\":{\"ok\":%s,\"seq\":%u,\"n\":%u,\"rate\":%u,\"trig\":%d,\"lost\":%u,\"src\":", LA.cap_ok ? "true" : "false",
        (unsigned)LA.cap.seq, (unsigned)(LA.cap_ok ? LA.cap.n : 0), (unsigned)LA.cap.rate, (int)LA.cap.trig,
        (unsigned)LA.cap.lost);
    PUTS(LA.cap.src);
    PUT("},\"an\":{\"ok\":%s,\"seq\":%u,\"nann\":%u,\"npkt\":%u,\"full\":%s,\"summary\":[", LA.an_ok ? "true" : "false",
        (unsigned)LA.an.seq, (unsigned)LA.an.nann, (unsigned)LA.an.npkt, LA.an.full ? "true" : "false");
    for (int r = 0; r < LA_DEC_MAX; r++) {
        if (r) PUT(",");
        PUTS(LA.an.summary[r]);
    }
    la_unlock();
    PUT("]},\"dev\":%d,\"devs\":[", LA.dev);
    for (int i = 0; i < LA.ndevs; i++) {
        const la_dev_t *d = &LA.devs[i];
        PUT("%s{\"drv\":%d,\"name\":", i ? "," : "", d->drv);
        PUTS(d->name);
        PUT(",\"detail\":");
        PUTS(d->detail);
        PUT(",\"max_rate\":%u,\"max_samples\":%u}", (unsigned)d->max_rate, (unsigned)d->max_samples);
    }
    PUT("],\"rates\":[");
    int nr;
    const uint32_t *r = la_rates(LA.ndevs ? &LA.devs[LA.dev] : NULL, &nr);
    for (int i = 0; i < nr; i++) PUT("%s%u", i ? "," : "", (unsigned)r[i]);
    PUT("],\"max_samples\":%u,\"names\":[", (unsigned)la_max_samples());
    for (int i = 0; i < LA_NCH; i++) {
        if (i) PUT(",");
        PUTS(LA.chname[i]);
    }
    PUT("],\"decs\":[");
    for (int i = 0; i < LA_DEC_MAX; i++) {
        char t[96];
        la_dec_describe(&LA.dec[i], t, sizeof t);
        PUT("%s{\"desc\":", i ? "," : "");
        PUTS(t);
        PUT(",\"name\":");
        PUTS(la_dec_name(LA.dec[i].type));
        PUT("}");
    }
    PUT("],\"cfg\":");
    char *cfg = malloc(2048);
    if (cfg) {
        la_cfg_text(cfg, 2048);
        PUTS(cfg);
        free(cfg);
    } else {
        PUT("\"\"");
    }
    PUT(",\"fx2dir\":");
    const char *fd = la_fx2_fw_dir();
    PUTS(fd ? fd : "");
    PUT(",\"host\":%s}", aos_hal_usb_host_on() ? "true" : "false");
    return k < n ? k : n - 1;
}

static void put_state(void)
{
    char *j = malloc(8192);
    if (!j) return;
    size_t len = state_json(j, 8192);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)j[i]) * 16777619u;
    s_state_hash = h;
    aos_hal_live_put(LA_APP_ID, "state", "application/json", j, len);
    free(j);
}

static uint32_t state_hash(void)
{
    char *j = malloc(8192);
    if (!j) return 0;
    size_t len = state_json(j, 8192);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)j[i]) * 16777619u;
    free(j);
    return h;
}

/* ---- the capture, in pieces ---- */

static size_t varint(uint8_t *p, uint32_t v)
{
    size_t k = 0;
    while (v >= 0x80) {
        p[k++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    p[k++] = (uint8_t)v;
    return k;
}

static void put_data(uint32_t seq, uint32_t from)
{
    if (!s_blob) return;
    uint8_t *b = s_blob;
    la_lock();
    const la_cap_t *c = &LA.cap;
    if (!LA.cap_ok || seq != c->seq || from > c->n) {
        la_unlock();
        return;
    }
    size_t k = 32;
    uint32_t i = from;
    while (i < c->n && k + 8 < BLOB) {
        uint8_t v = c->buf[i];
        uint32_t j = i + 1;
        while (j < c->n) {
            if (!(j % LA_SUM1) && j + LA_SUM1 <= c->n && c->s1[2 * (j / LA_SUM1)] == v && c->s1[2 * (j / LA_SUM1) + 1] == v) {
                j += LA_SUM1;
                continue;
            }
            if (c->buf[j] != v) break;
            j++;
        }
        b[k++] = v;
        k += varint(b + k, j - i);
        i = j;
    }
    memcpy(b, "LAD1", 4);
    wr32(b + 4, c->seq);
    wr32(b + 8, c->n);
    wr32(b + 12, from);
    wr32(b + 16, i);
    wr32(b + 20, c->rate);
    wr32(b + 24, (uint32_t)c->trig);
    wr32(b + 28, c->nch);
    la_unlock();
    aos_hal_live_put(LA_APP_ID, "data", "application/octet-stream", b, k);
}

static void put_anns(bool pkts, uint32_t seq, uint32_t from)
{
    if (!s_blob) return;
    uint8_t *b = s_blob;
    la_lock();
    if (!LA.an_ok || seq != LA.an.seq) {
        la_unlock();
        return;
    }
    uint32_t total = pkts ? LA.an.npkt : LA.an.nann, i = from;
    size_t k = 20;
    while (i < total && k + 120 < BLOB) {
        const char *t;
        if (pkts) {
            const la_pkt_t *p = &LA.an.pkt[i];
            wr32(b + k, p->s0);
            wr32(b + k + 4, p->s1);
            b[k + 8] = p->row;
            b[k + 9] = p->err;
            t = p->t;
        } else {
            const la_ann_t *a = &LA.an.ann[i];
            wr32(b + k, a->s0);
            wr32(b + k + 4, a->s1);
            b[k + 8] = a->row;
            b[k + 9] = a->kind;
            t = a->t;
        }
        size_t l = strlen(t);
        b[k + 10] = (uint8_t)l;
        memcpy(b + k + 11, t, l);
        k += 11 + l;
        i++;
    }
    memcpy(b, pkts ? "LAP1" : "LAA1", 4);
    wr32(b + 4, LA.an.seq);
    wr32(b + 8, total);
    wr32(b + 12, from);
    wr32(b + 16, i);
    la_unlock();
    aos_hal_live_put(LA_APP_ID, pkts ? "pkt" : "ann", "application/octet-stream", b, k);
}

/* ---- what the page says ---- */

/* the page names files as the portal does ("/logica/x.sr", from the card's
 * root) or by name alone (in the app's folder) */
static void card_path(const char *in, char *out, size_t n)
{
    const char *root = aos_hal_path_sd_root();
    const char *dir = la_dir();
    if (!root || !dir) snprintf(out, n, "%s", in);
    else if (!strchr(in, '/')) snprintf(out, n, "%s/%s", dir, in);
    else if (!strncmp(in, root, strlen(root))) snprintf(out, n, "%s", in);
    else snprintf(out, n, "%s%s%s", root, in[0] == '/' ? "" : "/", in);
}

static void command(char *line)
{
    if (!strcmp(line, "capture")) {
        if (!LA.busy) la_capture_ui();
    } else if (!strcmp(line, "stop")) {
        la_capture_cancel_repeat();
        la_job_stop();
    } else if (!strcmp(line, "probe")) {
        la_probe_start();
    } else if (!strcmp(line, "decode")) {
        la_decode_start();
    } else if (!strncmp(line, "dev ", 4)) {
        int i = atoi(line + 4);
        if (!LA.busy && i >= 0 && i < LA.ndevs) {
            LA.dev = i;
            aos_hal_pref_set_i32("la_drv", LA.devs[i].drv);
        }
    } else if (!strncmp(line, "load ", 5) || !strncmp(line, "save ", 5)) {
        char p[200];
        card_path(line + 5, p, sizeof p);
        if (strstr(p, "..")) return;
        if (line[0] == 'l') la_load_start(p);
        else la_save_start(p);
    } else if (!strncmp(line, "data ", 5) || !strncmp(line, "ann ", 4) || !strncmp(line, "pkt ", 4)) {
        char *p = strchr(line, ' ') + 1;
        uint32_t seq = (uint32_t)strtoul(p, &p, 10);
        uint32_t from = (uint32_t)strtoul(p, NULL, 10);
        if (line[0] == 'd') put_data(seq, from);
        else put_anns(line[0] == 'p', seq, from);
    } else if (!strcmp(line, "rescan")) {
        la_rescan = true;
    } else if (strchr(line, '=')) {
        bool dec = !strncmp(line, "dec", 3) || !strncmp(line, "name", 4);
        if (la_cfg_line(line)) {
            la_cfg_save();
            la_view_dirty();
            if (dec && !strncmp(line, "dec", 3) && LA.cap.n) la_decode_start();
        }
    }
}

void la_live_tick(void)
{
    uint32_t idle = aos_hal_live_idle_ms(LA_APP_ID);
    char msg[AOS_LIVE_MSG_MAX + 1];
    int n, got = 0;
    while ((n = aos_hal_live_take(LA_APP_ID, msg, AOS_LIVE_MSG_MAX)) > 0 && got++ < 8) {
        if (!s_blob) s_blob = la_big_alloc(BLOB + 256);
        msg[n] = 0;
        for (char *l = msg; l && *l;) {
            char *nl = strchr(l, '\n');
            if (nl) *nl = 0;
            size_t ln = strlen(l);
            while (ln && l[ln - 1] == '\r') l[--ln] = 0;
            if (ln) command(l);
            l = nl ? nl + 1 : NULL;
        }
        la_ui_status();
        s_state_ms = 0;
    }
    if (idle > 4000) return;
    uint32_t now = la_ms();
    if (!s_state_ms || now - s_state_ms > 500) {
        s_state_ms = now;
        put_state();
    } else if (now - s_state_ms > 150 && state_hash() != s_state_hash) {
        s_state_ms = now;
        put_state();
    }
}

void la_live_clear(void)
{
    aos_hal_live_clear(LA_APP_ID);
    la_big_free(s_blob);
    s_blob = NULL;
}
