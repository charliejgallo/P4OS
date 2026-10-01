/*
 * P4OS - the portal's Banco page: the supply, the scope and the logger from
 * the browser (aos_portal.c hands every /api/bench* request here).
 *
 *   GET  /api/bench                    the three at once: readings, settings, log
 *   GET  /api/bench/wave               the scope's traces (keeps them coming 3 s)
 *   GET  /api/bench/shot               the scope's own screen, the PNG last taken
 *   POST /api/bench/shot               take a new one
 *   POST /api/bench/riden {v?,i?,on?}
 *   POST /api/bench/scope {cmd} | {connect:"host[:port]"} | {disconnect:true}
 *   POST /api/bench/log {start:true|false} | {mask,interval}
 */
#include "aos_portal_bench.h"
#include "aos_hal.h"
#include "aos_riden.h"
#include "aos_scope.h"
#include "aos_bench_log.h"
#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void send_cjson(aos_httpd_req_t *r, int status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    aos_httpd_send_json(r, status, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
}

static void send_err(aos_httpd_req_t *r, int status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddFalseToObject(o, "ok");
    cJSON_AddStringToObject(o, "error", msg);
    send_cjson(r, status, o);
}

static cJSON *body_json(aos_httpd_req_t *r)
{
    char *b = aos_httpd_body_all(r, 4096);
    cJSON *o = b ? cJSON_Parse(b) : NULL;
    free(b);
    return o;
}

/* A float with the digits it has (0.001 and not 0.0010000000474974513);
 * NAN has no JSON: null. */
static void add_num(cJSON *o, const char *k, double v)
{
    if (isnan(v) || isinf(v)) { cJSON_AddNullToObject(o, k); return; }
    char t[24];
    snprintf(t, sizeof t, "%.6g", v);
    cJSON_AddNumberToObject(o, k, strtod(t, NULL));
}

static void api_bench(aos_httpd_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    /* the supply; the portal watching keeps the link up like the app does */
    aos_riden_keep(AOS_RIDEN_KEEP_UI);
    aos_riden_status_t rd;
    aos_riden_status(&rd);
    cJSON *jr = cJSON_AddObjectToObject(o, "riden");
    cJSON_AddBoolToObject(jr, "linked", rd.linked);
    cJSON_AddBoolToObject(jr, "ok", rd.ok);
    cJSON_AddStringToObject(jr, "where", rd.where);
    cJSON_AddStringToObject(jr, "error", rd.err);
    cJSON_AddNumberToObject(jr, "model", rd.model);
    cJSON_AddNumberToObject(jr, "idec", rd.idec);
    if (rd.ok) {
        add_num(jr, "v_set", rd.v_set);
        add_num(jr, "i_set", rd.i_set);
        add_num(jr, "v", rd.v_out);
        add_num(jr, "i", rd.i_out);
        add_num(jr, "p", rd.p_out);
        add_num(jr, "v_in", rd.v_in);
        cJSON_AddBoolToObject(jr, "on", rd.on);
        cJSON_AddBoolToObject(jr, "cc", rd.cc);
        cJSON_AddNumberToObject(jr, "protect", rd.protect);
        cJSON_AddNumberToObject(jr, "temp", rd.temp_c);
        add_num(jr, "ah", rd.ah);
        add_num(jr, "wh", rd.wh);
    }
    if (rd.wmsg[0]) cJSON_AddStringToObject(jr, "wmsg", rd.wmsg);
    cJSON_AddNumberToObject(jr, "wseq", rd.wseq);
    /* the scope */
    aos_scope_status_t sc;
    aos_scope_status(&sc);
    cJSON *js = cJSON_AddObjectToObject(o, "scope");
    cJSON_AddBoolToObject(js, "connected", sc.connected);
    cJSON_AddBoolToObject(js, "connecting", sc.connecting);
    cJSON_AddStringToObject(js, "host", sc.host);
    cJSON_AddStringToObject(js, "idn", sc.idn);
    cJSON_AddStringToObject(js, "error", sc.error);
    cJSON_AddStringToObject(js, "trig", sc.trig_status);
    add_num(js, "timebase", sc.timebase);
    add_num(js, "trig_level", sc.trig_level);
    cJSON_AddNumberToObject(js, "trig_source", sc.trig_source);
    cJSON_AddStringToObject(js, "trig_slope", sc.trig_slope);
    add_num(js, "fps", sc.fps);
    uint32_t shot_seq = 0;
    aos_scope_screenshot_file(&shot_seq);
    cJSON_AddNumberToObject(js, "shot_seq", shot_seq);
    cJSON *chs = cJSON_AddArrayToObject(js, "ch");
    static const char *const MK[AOS_SCOPE_M_COUNT] = { "vpp", "vmax", "vmin", "vavg", "vrms", "freq", "period" };
    for (int c = 0; c < AOS_SCOPE_CHANNELS; c++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "on", sc.ch[c].on);
        add_num(e, "scale", sc.ch[c].scale);
        add_num(e, "offset", sc.ch[c].offset);
        add_num(e, "probe", sc.ch[c].probe);
        cJSON_AddStringToObject(e, "coupling", sc.ch[c].coupling);
        cJSON *m = cJSON_AddObjectToObject(e, "meas");
        for (int k = 0; k < AOS_SCOPE_M_COUNT; k++) {
            float v;
            uint32_t age;
            add_num(m, MK[k], aos_scope_measure(c + 1, (aos_scope_measure_t)k, &v, &age) && age < 5000 ? v : NAN);
        }
        cJSON_AddItemToArray(chs, e);
    }
    /* the logger */
    bench_log_stat_t l;
    bench_log_stat(&l);
    cJSON *jl = cJSON_AddObjectToObject(o, "log");
    cJSON_AddBoolToObject(jl, "running", l.running);
    cJSON_AddNumberToObject(jl, "mask", l.mask);
    cJSON_AddNumberToObject(jl, "interval", l.interval_ms);
    cJSON_AddNumberToObject(jl, "rows", l.rows);
    cJSON_AddNumberToObject(jl, "elapsed", l.running ? ((uint32_t)aos_hal_uptime_ms() - l.started_ms) / 1000 : 0);
    const char *root = aos_hal_path_sd_root();
    size_t rl = root ? strlen(root) : 0;
    cJSON_AddStringToObject(jl, "path", root && !strncmp(l.path, root, rl) ? l.path + rl : l.path);
    cJSON_AddStringToObject(jl, "error", l.err);
    cJSON *ser = cJSON_AddArrayToObject(jl, "series");
    for (int s = 0; s < BL_SERIES; s++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", bench_log_series_name(s));
        cJSON_AddStringToObject(e, "unit", bench_log_series_unit(s));
        char c[8];
        snprintf(c, sizeof c, "#%06X", (unsigned)(bench_log_series_color(s) & 0xFFFFFF));
        cJSON_AddStringToObject(e, "color", c);
        cJSON_AddItemToArray(ser, e);
    }
    send_cjson(r, 200, o);
}

/* The traces, written by hand: 4 x 1200 numbers through cJSON would be
 * thousands of small mallocs. */
static void api_wave(aos_httpd_req_t *r)
{
    aos_scope_live_for(3000);
    aos_scope_status_t sc;
    aos_scope_status(&sc);
    static float v[AOS_SCOPE_POINTS] AOS_BSS_PSRAM;
    static void *mx;
    if (!mx) mx = aos_hal_mutex_create();
    if (!aos_httpd_begin(r, 200, "application/json", -1, "Cache-Control: no-store\r\n")) return;
    aos_hal_mutex_lock(mx);             /* one static buffer for however many pages poll */
    char buf[1400];
    int n = snprintf(buf, sizeof buf, "{\"seq\":%u,\"timebase\":%g,\"trig_level\":%g,\"trig_source\":%d,\"ch\":[",
                     (unsigned)sc.wave_seq, (double)sc.timebase, (double)sc.trig_level, sc.trig_source);
    for (int c = 0; c < AOS_SCOPE_CHANNELS; c++) {
        float xinc = 0;
        int np = sc.ch[c].on ? aos_scope_wave(c + 1, v, AOS_SCOPE_POINTS, &xinc) : 0;
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%s{\"on\":%s,\"scale\":%g,\"offset\":%g,\"xinc\":%g,\"v\":[",
                      c ? "," : "", sc.ch[c].on ? "true" : "false", (double)sc.ch[c].scale, (double)sc.ch[c].offset, (double)xinc);
        for (int i = 0; i < np; i++) {
            if (n > (int)sizeof buf - 24) { aos_httpd_write(r, buf, (size_t)n); n = 0; }
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%.4g", i ? "," : "", (double)v[i]);
        }
        n += snprintf(buf + n, sizeof buf - (size_t)n, "]}");
    }
    n += snprintf(buf + n, sizeof buf - (size_t)n, "]}");
    aos_httpd_write(r, buf, (size_t)n);
    aos_hal_mutex_unlock(mx);
}

static void api_shot(aos_httpd_req_t *r)
{
    uint32_t seq;
    const char *path = aos_scope_screenshot_file(&seq);
    struct stat st;
    FILE *f = path && !stat(path, &st) ? fopen(path, "rb") : NULL;
    if (!f) { send_err(r, 404, "todavía no hay captura"); return; }
    if (aos_httpd_begin(r, 200, "image/png", (long)st.st_size, "Cache-Control: no-store\r\n")) {
        char *buf = malloc(8192);
        size_t k;
        while (buf && (k = fread(buf, 1, 8192, f)) > 0)
            if (!aos_httpd_write(r, buf, k)) break;
        free(buf);
    }
    fclose(f);
}

static void api_riden(aos_httpd_req_t *r)
{
    cJSON *b = body_json(r);
    if (!b) { send_err(r, 400, "falta el cuerpo"); return; }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(b, "v"), *i = cJSON_GetObjectItemCaseSensitive(b, "i"),
                *on = cJSON_GetObjectItemCaseSensitive(b, "on");
    bool ok = true;
    aos_riden_keep(AOS_RIDEN_KEEP_UI);
    if (cJSON_IsNumber(v)) ok &= aos_riden_set_v((float)v->valuedouble);
    if (cJSON_IsNumber(i)) ok &= aos_riden_set_i((float)i->valuedouble);
    if (cJSON_IsBool(on)) ok &= aos_riden_set_output(cJSON_IsTrue(on));
    cJSON_Delete(b);
    if (ok) aos_httpd_send_json(r, 200, "{\"ok\":true}");
    else send_err(r, 503, "la fuente tiene la cola llena");
}

static void api_scope(aos_httpd_req_t *r)
{
    cJSON *b = body_json(r);
    if (!b) { send_err(r, 400, "falta el cuerpo"); return; }
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(b, "cmd"), *con = cJSON_GetObjectItemCaseSensitive(b, "connect");
    bool ok = true;
    if (cJSON_IsString(con)) {
        char host[64];
        snprintf(host, sizeof host, "%s", con->valuestring);
        int port = 0;
        char *c = strrchr(host, ':');
        if (c) { *c = 0; port = atoi(c + 1); }
        aos_scope_connect(host, port);
    } else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "disconnect"))) {
        aos_scope_disconnect();
    } else if (cJSON_IsString(cmd) && cmd->valuestring[0] == ':' ) {
        ok = aos_scope_cmd(cmd->valuestring);
    } else if (cJSON_IsString(cmd) && cmd->valuestring[0] == '*') {
        ok = aos_scope_cmd(cmd->valuestring);
    } else {
        ok = false;
    }
    cJSON_Delete(b);
    if (ok) aos_httpd_send_json(r, 200, "{\"ok\":true}");
    else send_err(r, 400, "comando no válido o cola llena");
}

static void api_log(aos_httpd_req_t *r)
{
    cJSON *b = body_json(r);
    if (!b) { send_err(r, 400, "falta el cuerpo"); return; }
    const cJSON *start = cJSON_GetObjectItemCaseSensitive(b, "start"), *mask = cJSON_GetObjectItemCaseSensitive(b, "mask"),
                *iv = cJSON_GetObjectItemCaseSensitive(b, "interval");
    bench_log_stat_t l;
    bench_log_stat(&l);
    if (cJSON_IsNumber(mask) || cJSON_IsNumber(iv))
        bench_log_configure(cJSON_IsNumber(mask) ? (uint32_t)mask->valuedouble : l.mask,
                            cJSON_IsNumber(iv) ? (uint32_t)iv->valuedouble : l.interval_ms);
    bool ok = true;
    if (cJSON_IsTrue(start)) ok = bench_log_start();
    else if (cJSON_IsFalse(start)) bench_log_stop();
    cJSON_Delete(b);
    if (ok) { aos_httpd_send_json(r, 200, "{\"ok\":true}"); return; }
    bench_log_stat(&l);
    send_err(r, 400, l.err[0] ? l.err : "no se pudo empezar");
}

bool aos_portal_bench(aos_httpd_req_t *r, const char *m, const char *p)
{
    bool post = !strcmp(m, "POST"), get = !strcmp(m, "GET");
    if (strncmp(p, "bench", 5)) return false;
    p += 5;
    aos_scope_start();
    if (get && !*p) api_bench(r);
    else if (get && !strcmp(p, "/wave")) api_wave(r);
    else if (get && !strcmp(p, "/shot")) api_shot(r);
    else if (post && !strcmp(p, "/shot")) { aos_scope_screenshot(); aos_httpd_send_json(r, 200, "{\"ok\":true}"); }
    else if (post && !strcmp(p, "/riden")) api_riden(r);
    else if (post && !strcmp(p, "/scope")) api_scope(r);
    else if (post && !strcmp(p, "/log")) api_log(r);
    else return false;
    return true;
}
