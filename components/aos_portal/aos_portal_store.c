/*
 * P4OS - the Tienda's pieces, by hand (docs/plan/TIENDA.md, phase 0).
 *
 * There is no store yet. These calls are for measuring its parts on the
 * board, one at a time, before anything is built on them: a download to the
 * card with the interface alive, whether a .so would load without loading
 * it, and putting an app in and taking it off while the board runs. The
 * store's service replaces them with its own API.
 *
 *   GET  /api/store/fetch          the download: state, bytes, times, and the
 *                                  board's free memory as it goes
 *   POST /api/store/fetch          {"url", "path", "sha256", "size"}: starts
 *                                  one; path is on the card ("/apps/x.so"),
 *                                  sha256 and size may be left out
 *   POST /api/store/fetch/cancel
 *   GET  /api/store/check?file=x.so    would /apps/x.so load on this firmware:
 *                                      {ok, missing: [...], why}
 *   POST /api/store/load?file=x.so     registers /apps/x.so now, no restart
 *   POST /api/store/unload?file=x.so   takes its apps off and its code out
 *
 * They give the portal nothing it did not have: it writes any file of the
 * card and restarts the board already.
 */
#include "aos_portal_store.h"
#include "aos_fetch.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AOS_SIM
#include "aos_dynapp.h"
#endif

static int      s_id;               /* the one download of these calls */
static uint32_t s_int_start, s_int_min, s_psram_min;

static void reply(aos_httpd_req_t *r, int status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    aos_httpd_send_json(r, status, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
}

static void error(aos_httpd_req_t *r, int status, const char *text)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddFalseToObject(o, "ok");
    cJSON_AddStringToObject(o, "error", text);
    reply(r, status, o);
}

#ifndef AOS_SIM
/* A name inside /apps, and nothing else: no folders, no way up. */
static bool so_name(aos_httpd_req_t *r, char *out, size_t n)
{
    if (!aos_httpd_query(r, "file", out, n) || !out[0]) return false;
    size_t l = strlen(out);
    return !strchr(out, '/') && !strstr(out, "..") && l > 3 && !strcmp(out + l - 3, ".so");
}
#endif

static void fetch_status(aos_httpd_req_t *r)
{
    static const char *names[] = { "none", "busy", "done", "failed", "cancelled" };
    aos_fetch_info_t in = { 0 };
    bool have = s_id && aos_fetch_info(s_id, &in);
    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    /* the lowest seen by whoever asks: ask often while it runs */
    if (have && in.state == AOS_FETCH_BUSY) {
        if (fi < s_int_min) s_int_min = fi;
        if (fp < s_psram_min) s_psram_min = fp;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddTrueToObject(o, "ok");
    cJSON_AddStringToObject(o, "state", names[have ? in.state : 0]);
    if (have) {
        cJSON_AddNumberToObject(o, "status", in.status);
        cJSON_AddNumberToObject(o, "got", in.got);
        cJSON_AddNumberToObject(o, "total", in.total);
        cJSON_AddNumberToObject(o, "resumed", in.resumed);
        cJSON_AddNumberToObject(o, "connects", in.connects);
        cJSON_AddNumberToObject(o, "redirects", in.redirects);
        cJSON_AddNumberToObject(o, "retries", in.retries);
        cJSON_AddNumberToObject(o, "ms", in.ms);
        cJSON_AddNumberToObject(o, "connect_ms", in.connect_ms);
        cJSON_AddNumberToObject(o, "verify_ms", in.verify_ms);
        cJSON_AddStringToObject(o, "host", in.host);
        cJSON_AddStringToObject(o, "why", in.why);
        cJSON_AddNumberToObject(o, "internal_at_start", s_int_start);
        cJSON_AddNumberToObject(o, "internal_min", s_int_min);
        cJSON_AddNumberToObject(o, "psram_min", s_psram_min);
    }
    cJSON_AddNumberToObject(o, "internal_free", fi);
    cJSON_AddNumberToObject(o, "psram_free", fp);
    reply(r, 200, o);
}

static void fetch_start(aos_httpd_req_t *r)
{
    const char *root = aos_hal_path_sd_root();
    char *body = aos_httpd_body_all(r, 8192);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(j, "url"));
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(j, "path"));
    const char *sha = cJSON_GetStringValue(cJSON_GetObjectItem(j, "sha256"));
    cJSON *size = cJSON_GetObjectItem(j, "size");
    if (!root) error(r, 503, _("sin tarjeta"));
    else if (!url || !path || path[0] != '/' || strstr(path, "..")) error(r, 400, _("faltan url y path"));
    else {
        aos_fetch_info_t in;
        if (s_id && aos_fetch_info(s_id, &in) && in.state == AOS_FETCH_BUSY) error(r, 409, _("ya hay una bajada"));
        else {
            if (s_id) aos_fetch_release(s_id);
            char full[224];
            snprintf(full, sizeof full, "%s%s", root, path);
            s_id = aos_fetch_start(url, full, sha, cJSON_IsNumber(size) ? (uint32_t)size->valuedouble : 0);
            if (s_id <= 0) {
                s_id = 0;
                error(r, 400, _("no se pudo empezar: ¿la dirección es http(s)?"));
            } else {
                aos_hal_heap_info(&s_int_start, &s_psram_min);
                s_int_min = s_int_start;
                fetch_status(r);
            }
        }
    }
    cJSON_Delete(j);
}

bool aos_portal_store(aos_httpd_req_t *r, const char *m, const char *p)
{
    if (strncmp(p, "store/", 6)) return false;
    p += 6;
    bool get = !strcmp(m, "GET"), post = !strcmp(m, "POST");

    if (!strcmp(p, "fetch") && get) fetch_status(r);
    else if (!strcmp(p, "fetch") && post) fetch_start(r);
    else if (!strcmp(p, "fetch/cancel") && post) {
        if (s_id) aos_fetch_cancel(s_id);
        fetch_status(r);
    }
#ifndef AOS_SIM
    else if (!strcmp(p, "check") && get) {
        char file[64], path[160], *missing = malloc(4096);
        const char *root = aos_hal_path_sd_root();
        if (!missing || !root || !so_name(r, file, sizeof file)) error(r, 400, _("falta file=<nombre>.so"));
        else {
            snprintf(path, sizeof path, "%s/apps/%s", root, file);
            uint64_t t = aos_hal_uptime_ms();
            int n = aos_dynapp_check(path, missing, 4096);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", n == 0);
            cJSON_AddNumberToObject(o, "result", n);
            cJSON_AddNumberToObject(o, "ms", (double)(aos_hal_uptime_ms() - t));
            if (n > 0) {
                cJSON *a = cJSON_AddArrayToObject(o, "missing");
                for (char *s = strtok(missing, " "); s; s = strtok(NULL, " ")) cJSON_AddItemToArray(a, cJSON_CreateString(s));
            } else if (n < 0) cJSON_AddStringToObject(o, "why", missing);
            reply(r, 200, o);
        }
        free(missing);
    }
    else if ((!strcmp(p, "load") || !strcmp(p, "unload")) && post) {
        char file[64];
        if (!so_name(r, file, sizeof file)) error(r, 400, _("falta file=<nombre>.so"));
        else {
            uint32_t fi = 0, before = 0, after = 0;
            aos_hal_heap_info(&fi, &before);
            uint64_t t = aos_hal_uptime_ms();
            int n = p[0] == 'l' ? (aos_dynapp_load(file) ? 1 : 0) : aos_dynapp_unload_file(file);
            aos_hal_heap_info(&fi, &after);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", n > 0);
            cJSON_AddNumberToObject(o, "apps", n);
            cJSON_AddNumberToObject(o, "ms", (double)(aos_hal_uptime_ms() - t));
            cJSON_AddNumberToObject(o, "psram_before", before);
            cJSON_AddNumberToObject(o, "psram_after", after);
            cJSON_AddNumberToObject(o, "internal_free", fi);
            reply(r, n > 0 ? 200 : 409, o);
        }
    }
#endif
    else error(r, 404, _("no existe"));
    return true;
}
