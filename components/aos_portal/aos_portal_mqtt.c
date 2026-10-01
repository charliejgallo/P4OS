/*
 * P4OS - the portal's MQTT page: the service's table and its settings from
 * the browser (aos_portal.c hands every /api/mqtt* request here).
 *
 *   GET  /api/mqtt                 state, settings (no password), counters
 *   GET  /api/mqtt/topics          every topic: a preview of the payload, counts, ages
 *   GET  /api/mqtt/topic?t=<topic> one topic: its whole kept payload and its series
 *   POST /api/mqtt/publish         {topic, payload, qos, retain}
 *   POST /api/mqtt/config          {host, port, user, pass, client_id, sub, keepalive, enabled}
 *   POST /api/mqtt/reconnect | /api/mqtt/clear
 *
 * The topic list is written by hand, a topic at a time: each one is copied
 * under the service's lock and sent after unlocking, so a slow browser
 * never holds up the thread that reads the broker.
 */
#include "aos_portal_mqtt.h"
#include "aos_hal.h"
#include "aos_mqtt.h"
#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void add_num(cJSON *o, const char *k, double v)
{
    if (!isfinite(v)) { cJSON_AddNullToObject(o, k); return; }
    char t[24];
    snprintf(t, sizeof t, "%.6g", v);
    cJSON_AddNumberToObject(o, k, strtod(t, NULL));
}

static const char *state_name(aos_mqtt_state_t s)
{
    static const char *const N[] = { "off", "unconfigured", "waiting_net", "connecting", "refused", "connected", "error" };
    return (unsigned)s < sizeof N / sizeof N[0] ? N[s] : "?";
}

static void api_status(aos_httpd_req_t *r)
{
    aos_mqtt_config_t c;
    aos_mqtt_config(&c);
    aos_mqtt_stats_t s;
    aos_mqtt_stats(&s);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", state_name(aos_mqtt_state()));
    cJSON_AddStringToObject(o, "error", aos_mqtt_error());
    cJSON_AddStringToObject(o, "client_id", aos_mqtt_client_id());
    cJSON_AddNumberToObject(o, "version", aos_mqtt_version());
    cJSON *jc = cJSON_AddObjectToObject(o, "config");
    cJSON_AddBoolToObject(jc, "enabled", c.enabled);
    cJSON_AddStringToObject(jc, "host", c.host);
    cJSON_AddNumberToObject(jc, "port", c.port);
    cJSON_AddStringToObject(jc, "user", c.user);
    cJSON_AddBoolToObject(jc, "has_pass", c.pass[0] != 0);
    cJSON_AddStringToObject(jc, "client_id", c.client_id);
    cJSON_AddStringToObject(jc, "sub", c.sub);
    cJSON_AddNumberToObject(jc, "keepalive", c.keepalive);
    cJSON *js = cJSON_AddObjectToObject(o, "stats");
    cJSON_AddNumberToObject(js, "rx_msgs", s.rx_msgs);
    cJSON_AddNumberToObject(js, "tx_msgs", s.tx_msgs);
    cJSON_AddNumberToObject(js, "rx_bytes", s.rx_bytes);
    cJSON_AddNumberToObject(js, "tx_bytes", s.tx_bytes);
    cJSON_AddNumberToObject(js, "connects", s.connects);
    cJSON_AddNumberToObject(js, "drops", s.drops);
    cJSON_AddNumberToObject(js, "pending", s.pending);
    cJSON_AddNumberToObject(js, "queued", s.queued);
    cJSON_AddNumberToObject(js, "cut", s.cut);
    cJSON_AddNumberToObject(js, "evicted", s.evicted);
    cJSON_AddNumberToObject(js, "topics", s.topics);
    cJSON_AddNumberToObject(js, "ping_ms", s.ping_ms);
    cJSON_AddNumberToObject(js, "connected_s", s.connected_ms ? ((uint32_t)aos_hal_uptime_ms() - s.connected_ms) / 1000 : 0);
    send_cjson(r, 200, o);
}

/* Appends s as a JSON string (quotes included), at most max bytes of it. */
static size_t jstr(char *out, size_t n, const char *s, size_t max)
{
    size_t k = 0;
    if (k + 1 < n) out[k++] = '"';
    for (size_t i = 0; s[i] && i < max && k + 8 < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
        else if (c == '\n') { out[k++] = '\\'; out[k++] = 'n'; }
        else if (c < 0x20) k += (size_t)snprintf(out + k, n - k, "\\u%04x", c);
        else out[k++] = (char)c;
    }
    if (k + 1 < n) out[k++] = '"';
    out[k] = 0;
    return k;
}

static void api_topics(aos_httpd_req_t *r)
{
    if (!aos_httpd_begin(r, 200, "application/json", -1, "Cache-Control: no-store\r\n")) return;
    char *buf = malloc(2048);
    aos_mqtt_topic_t *t = malloc(sizeof *t);
    if (!buf || !t) { free(buf); free(t); return; }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    int n = snprintf(buf, 2048, "{\"version\":%u,\"topics\":[", (unsigned)aos_mqtt_version());
    bool first = true;
    for (int i = 0; i < AOS_MQTT_MAX_TOPICS; i++) {
        aos_mqtt_lock();
        const aos_mqtt_topic_t *e = aos_mqtt_topic_at(i);
        if (e) *t = *e;
        aos_mqtt_unlock();
        if (!e) continue;
        if (n > 2048 - 700) { if (!aos_httpd_write(r, buf, (size_t)n)) break; n = 0; }
        n += snprintf(buf + n, 2048 - (size_t)n, "%s{\"t\":", first ? "" : ",");
        n += (int)jstr(buf + n, 2048 - (size_t)n, t->topic, AOS_MQTT_TOPIC_MAX);
        n += snprintf(buf + n, 2048 - (size_t)n, ",\"p\":");
        n += (int)jstr(buf + n, 2048 - (size_t)n, t->kind == AOS_MQTT_BINARY ? "" : t->payload, 160);
        n += snprintf(buf + n, 2048 - (size_t)n, ",\"n\":%u,\"age\":%u,\"r\":%s,\"q\":%u,\"k\":%u,\"len\":%u,\"nf\":%u}",
                      (unsigned)t->count, (unsigned)(now - t->last_ms), t->retained ? "true" : "false", (unsigned)t->qos,
                      (unsigned)t->kind, (unsigned)t->len, (unsigned)t->nfields);
        first = false;
    }
    n += snprintf(buf + n, 2048 - (size_t)n, "]}");
    aos_httpd_write(r, buf, (size_t)n);
    free(buf);
    free(t);
}

static void api_topic(aos_httpd_req_t *r)
{
    char name[AOS_MQTT_TOPIC_MAX];
    if (!aos_httpd_query(r, "t", name, sizeof name)) { send_err(r, 400, "falta t"); return; }
    aos_mqtt_topic_t *t = malloc(sizeof *t);
    char (*names)[AOS_MQTT_FIELD_MAX] = malloc(AOS_MQTT_FIELDS * AOS_MQTT_FIELD_MAX);
    float *last = malloc(AOS_MQTT_FIELDS * sizeof *last);
    float *v = malloc(AOS_MQTT_FIELDS * AOS_MQTT_HIST * sizeof *v);
    uint32_t *tm = malloc(AOS_MQTT_FIELDS * AOS_MQTT_HIST * sizeof *tm);
    int np[AOS_MQTT_FIELDS] = { 0 };
    if (!t || !names || !last || !v || !tm) {
        free(t); free(names); free(last); free(v); free(tm);
        send_err(r, 500, "sin memoria");
        return;
    }
    aos_mqtt_lock();
    int slot = aos_mqtt_topic_find(name);
    const aos_mqtt_topic_t *e = slot >= 0 ? aos_mqtt_topic_at(slot) : NULL;
    int nf = 0;
    if (e) {
        *t = *e;
        nf = aos_mqtt_fields(slot, names, last, AOS_MQTT_FIELDS);
        for (int i = 0; i < nf; i++) np[i] = aos_mqtt_history(slot, names[i], v + i * AOS_MQTT_HIST, tm + i * AOS_MQTT_HIST, AOS_MQTT_HIST);
    }
    aos_mqtt_unlock();
    if (!e) {
        free(t); free(names); free(last); free(v); free(tm);
        send_err(r, 404, "ese tópico no está en la tabla");
        return;
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "topic", t->topic);
    cJSON_AddStringToObject(o, "payload", t->kind == AOS_MQTT_BINARY ? "" : t->payload);
    cJSON_AddNumberToObject(o, "len", t->len);
    cJSON_AddNumberToObject(o, "count", t->count);
    cJSON_AddNumberToObject(o, "age", now - t->last_ms);
    cJSON_AddNumberToObject(o, "wall", (double)t->last_wall);
    cJSON_AddBoolToObject(o, "retained", t->retained);
    cJSON_AddNumberToObject(o, "qos", t->qos);
    cJSON_AddNumberToObject(o, "kind", t->kind);
    cJSON *fs = cJSON_AddArrayToObject(o, "fields");
    for (int i = 0; i < nf; i++) {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "name", names[i]);
        add_num(f, "last", last[i]);
        cJSON *jv = cJSON_AddArrayToObject(f, "v"), *ja = cJSON_AddArrayToObject(f, "age");
        for (int k = 0; k < np[i]; k++) {
            char s[24];
            snprintf(s, sizeof s, "%.6g", (double)v[i * AOS_MQTT_HIST + k]);
            cJSON_AddItemToArray(jv, cJSON_CreateNumber(strtod(s, NULL)));
            cJSON_AddItemToArray(ja, cJSON_CreateNumber(now - tm[i * AOS_MQTT_HIST + k]));
        }
        cJSON_AddItemToArray(fs, f);
    }
    free(t); free(names); free(last); free(v); free(tm);
    send_cjson(r, 200, o);
}

static void api_publish(aos_httpd_req_t *r)
{
    cJSON *b = body_json(r);
    if (!b) { send_err(r, 400, "falta el cuerpo"); return; }
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(b, "topic"), *p = cJSON_GetObjectItemCaseSensitive(b, "payload"),
                *q = cJSON_GetObjectItemCaseSensitive(b, "qos");
    bool retain = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "retain"));
    const char *why = NULL;
    aos_mqtt_config_t c;
    aos_mqtt_config(&c);
    if (!cJSON_IsString(t) || !t->valuestring[0]) why = "falta el tópico";
    else if (strpbrk(t->valuestring, "+#")) why = "un tópico para publicar no lleva + ni #";
    else if (!c.host[0]) why = "no hay broker configurado";
    else if (!c.enabled) why = "MQTT está apagado";
    else if (!aos_mqtt_publish(t->valuestring, cJSON_IsString(p) ? p->valuestring : "", cJSON_IsNumber(q) ? q->valueint : 0, retain))
        why = "mensaje demasiado largo o la cola está llena";
    cJSON_Delete(b);
    if (why) send_err(r, 400, why);
    else aos_httpd_send_json(r, 200, aos_mqtt_state() == AOS_MQTT_CONNECTED ? "{\"ok\":true}" : "{\"ok\":true,\"queued\":true}");
}

static void copy_str(const cJSON *b, const char *k, char *out, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(b, k);
    if (!cJSON_IsString(v)) return;
    size_t l = strlen(v->valuestring);
    if (l >= n) l = n - 1;
    memcpy(out, v->valuestring, l);
    out[l] = 0;
}

static void api_config(aos_httpd_req_t *r)
{
    cJSON *b = body_json(r);
    if (!b) { send_err(r, 400, "falta el cuerpo"); return; }
    aos_mqtt_config_t c;
    aos_mqtt_config(&c);                        /* the password comes back as the placeholder: kept */
    copy_str(b, "host", c.host, sizeof c.host);
    copy_str(b, "user", c.user, sizeof c.user);
    copy_str(b, "pass", c.pass, sizeof c.pass);
    copy_str(b, "client_id", c.client_id, sizeof c.client_id);
    copy_str(b, "sub", c.sub, sizeof c.sub);
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(b, "port"))) c.port = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(b, "keepalive"))) c.keepalive = v->valueint;
    if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(b, "enabled"))) c.enabled = cJSON_IsTrue(v);
    cJSON_Delete(b);
    aos_mqtt_set_config(&c);
    aos_httpd_send_json(r, 200, "{\"ok\":true}");
}

bool aos_portal_mqtt(aos_httpd_req_t *r, const char *m, const char *p)
{
    bool post = !strcmp(m, "POST"), get = !strcmp(m, "GET");
    if (strncmp(p, "mqtt", 4) || (p[4] && p[4] != '/')) return false;
    p += 4;
    aos_mqtt_start();
    if (get && !*p) api_status(r);
    else if (get && !strcmp(p, "/topics")) api_topics(r);
    else if (get && !strcmp(p, "/topic")) api_topic(r);
    else if (post && !strcmp(p, "/publish")) api_publish(r);
    else if (post && !strcmp(p, "/config")) api_config(r);
    else if (post && !strcmp(p, "/reconnect")) { aos_mqtt_reconnect(); aos_httpd_send_json(r, 200, "{\"ok\":true}"); }
    else if (post && !strcmp(p, "/clear")) { aos_mqtt_clear(); aos_httpd_send_json(r, 200, "{\"ok\":true}"); }
    else return false;
    return true;
}
