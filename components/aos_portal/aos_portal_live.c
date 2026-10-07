/*
 * P4OS - the portal's door to the apps' live data (aos_hal.h, aos_live.c,
 * docs/PORTAL-PAGES.md "Live data"):
 *
 *   GET  /api/live?app=<id>             the app's blobs: [{key, type, len, seq, age_ms}]
 *   GET  /api/live?app=<id>&key=<key>   one, as its type, with X-Live-Seq; 404 if there is none
 *   POST /api/live?app=<id>             the body (1 KB at most) queued for the app
 */
#include "aos_portal_live.h"
#include "aos_hal.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void error(aos_httpd_req_t *r, int status, const char *text)
{
    char j[160];
    snprintf(j, sizeof j, "{\"ok\":false,\"error\":\"%s\"}", text);
    aos_httpd_send_json(r, status, j);
}

bool aos_portal_live(aos_httpd_req_t *r, const char *m, const char *p)
{
    if (strcmp(p, "live")) return false;
    char app[24] = "", key[16] = "";
    if (!aos_httpd_query(r, "app", app, sizeof app) || !app[0]) {
        error(r, 400, "falta app");
        return true;
    }
    if (!strcmp(m, "POST")) {
        long len = aos_httpd_body_len(r);
        if (len < 0 || len > AOS_LIVE_MSG_MAX) {
            error(r, 413, "mensaje de 1 KB como mucho");
            return true;
        }
        char *body = aos_httpd_body_all(r, AOS_LIVE_MSG_MAX);
        bool ok = body && aos_hal_live_push(app, body, strlen(body));
        free(body);
        if (ok) aos_httpd_send_json(r, 200, "{\"ok\":true}");
        else error(r, 503, "la cola de la app está llena");
        return true;
    }
    if (strcmp(m, "GET")) {
        error(r, 405, "GET o POST");
        return true;
    }
    if (!aos_httpd_query(r, "key", key, sizeof key) || !key[0]) {
        aos_live_info_t info[16];
        int n = aos_hal_live_list(app, info, 16);
        cJSON *a = cJSON_CreateArray();
        for (int i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "key", info[i].key);
            cJSON_AddStringToObject(o, "type", info[i].type);
            cJSON_AddNumberToObject(o, "len", info[i].len);
            cJSON_AddNumberToObject(o, "seq", info[i].seq);
            cJSON_AddNumberToObject(o, "age_ms", info[i].age_ms);
            cJSON_AddItemToArray(a, o);
        }
        char *s = cJSON_PrintUnformatted(a);
        aos_httpd_send_json(r, 200, s ? s : "[]");
        free(s);
        cJSON_Delete(a);
        return true;
    }
    void *buf = malloc(AOS_LIVE_BLOB_MAX);
    if (!buf) {
        error(r, 500, "sin memoria");
        return true;
    }
    char type[32];
    uint32_t seq = 0;
    int len = aos_hal_live_get(app, key, buf, AOS_LIVE_BLOB_MAX, type, sizeof type, &seq);
    if (len < 0) error(r, 404, "no hay datos: ¿está abierta la app?");
    else {
        char hdr[64];
        snprintf(hdr, sizeof hdr, "X-Live-Seq: %u\r\n", (unsigned)seq);
        if (aos_httpd_begin(r, 200, type, len, hdr) && len) aos_httpd_write(r, buf, len);
    }
    free(buf);
    return true;
}
