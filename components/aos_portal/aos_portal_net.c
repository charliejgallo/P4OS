/*
 * P4OS - /api/net: the Red app's LAN sweep, from the portal.
 *
 *   GET  /api/net                         how the sweep goes, and who it found so far
 *   POST /api/net {"do": "scan", "full": false}   start one (full: also the
 *                                                  ones that do not answer ping)
 *                 {"do": "stop"}
 *
 * It is the same sweep the app's Hosts tab runs (aos_nettools.c), and the
 * same results: a sweep started here shows on the board's screen if Red is
 * open, and the other way round. Finished, it is saved to the card's /redes,
 * where the portal's Red page reads the past ones.
 *
 * The service stops its threads three seconds after nobody asks (the app's
 * keepalive); here every GET is one, so the sweep goes on while the page
 * polls it and stops when the page is closed.
 */
#include "aos_portal_net.h"
#include "../aos_apps/aos_nettools.h"     /* the Red app's service, private to aos_apps */
#include "aos_hal.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const STATE[] = { "idle", "busy", "done", "failed" };

static void send_cjson(aos_httpd_req_t *r, int status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    aos_httpd_send_json(r, status, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
}

static void api_get(aos_httpd_req_t *r)
{
    nt_keepalive();
    cJSON *o = cJSON_CreateObject();
    char ip[16];
    nt_lock();
    const nt_scan_t *s = nt_scan();
    cJSON_AddStringToObject(o, "state", STATE[s->state & 3]);
    cJSON_AddNumberToObject(o, "phase", s->phase);
    cJSON_AddNumberToObject(o, "done", s->done);
    cJSON_AddNumberToObject(o, "total", s->total);
    cJSON_AddBoolToObject(o, "full", s->full);
    cJSON_AddStringToObject(o, "range", s->range);
    cJSON_AddStringToObject(o, "error", s->err);
    cJSON_AddStringToObject(o, "file", s->file);
    cJSON_AddNumberToObject(o, "elapsed_ms", s->elapsed_ms);
    cJSON *hs = cJSON_AddArrayToObject(o, "hosts");
    for (int i = 0; i < s->n; i++) {
        const nt_host_t *x = &s->hosts[i];
        cJSON *h = cJSON_CreateObject();
        nt_ip_str(x->ip, ip, sizeof ip);
        cJSON_AddStringToObject(h, "ip", ip);
        cJSON_AddStringToObject(h, "name", x->name);
        cJSON *ps = cJSON_AddArrayToObject(h, "ports");
        for (int k = 0; k < x->nports; k++) cJSON_AddItemToArray(ps, cJSON_CreateNumber(x->ports[k]));
        if (x->self) cJSON_AddBoolToObject(h, "self", true);
        cJSON_AddItemToArray(hs, h);
    }
    nt_unlock();
    send_cjson(r, 200, o);
}

static void api_post(aos_httpd_req_t *r)
{
    char *raw = aos_httpd_body_all(r, 256);
    cJSON *b = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    const cJSON *d = cJSON_GetObjectItem(b, "do");
    const char *what = cJSON_IsString(d) ? d->valuestring : "";
    const char *err = NULL;
    if (!strcmp(what, "scan")) {
        if (!nt_scan_start(cJSON_IsTrue(cJSON_GetObjectItem(b, "full")))) {
            nt_lock();
            bool busy = nt_scan()->state == NT_BUSY;
            nt_unlock();
            err = busy ? "ya hay un barrido en marcha" : "no se pudo empezar el barrido";
        }
    } else if (!strcmp(what, "stop")) {
        nt_scan_stop();
    } else {
        err = "\"do\" es scan o stop";
    }
    cJSON_Delete(b);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", !err);
    if (err) cJSON_AddStringToObject(o, "error", err);
    send_cjson(r, err ? 400 : 200, o);
}

bool aos_portal_net(aos_httpd_req_t *r, const char *m, const char *p)
{
    if (strcmp(p, "net")) return false;
    if (!nt_init()) {
        aos_httpd_send_json(r, 500, "{\"error\":\"no hay memoria para la Red\"}");
        return true;
    }
    if (!strcmp(m, "GET")) api_get(r);
    else if (!strcmp(m, "POST")) api_post(r);
    else return false;
    return true;
}
