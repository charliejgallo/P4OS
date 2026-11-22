/*
 * P4OS - the Tienda in the portal (docs/plan/TIENDA.md).
 *
 * The store's service (components/aos_store), as the page and the scripts
 * see it:
 *
 *   GET  /api/store                its state: the job in hand and its
 *                                  progress, how the last one ended, the
 *                                  catalogue's source and release, and every
 *                                  app: {file, id, ver, have, state, fits,
 *                                  and "new" when it is of the newest
 *                                  release that brought apps}
 *                                  (state: absent, installed, update, by_hand,
 *                                  or set_aside: it made the board fall while
 *                                  loading; installing it again is the way back)
 *   GET  /api/store/catalog        the catalogue as it was downloaded
 *                                  (NDJSON: the source, then an app a line):
 *                                  names, descriptions, icons, packs
 *   POST /api/store/refresh        downloads every source's catalogue again;
 *                                  {"url": "..."} puts another address in the
 *                                  official one's place first ("" goes back),
 *                                  in developer mode only
 *   POST /api/store/propose        {"url": "user/repo"}: asks for a source.
 *                                  It is not added: the Tienda shows it at
 *                                  the board, where a person looks at it,
 *                                  reads the warning and decides
 *   POST /api/store/sideload       the body is a package (.p4app): installed
 *                                  by the same steps; ?path=/x.p4app takes
 *                                  one that is on the card or a pendrive
 *
 * The state also lists the sources ({name, url, release, apps, skipped}) and
 * says whether developer mode is on. Sources are added and taken off at the
 * board only, and so is developer mode: none of that is here.
 *   POST /api/store/install        {"file": "bastion", "packs": "need"}:
 *                                  packs is need, all, none, or extras by
 *                                  name ("a.pak,b.pak")
 *   POST /api/store/remove         {"file": "bastion"}
 *   POST /api/store/replace        every app copied by hand that is NOT the
 *                                  catalogue's, installed over from its
 *                                  source, in one job
 *   POST /api/store/adopt          a receipt for what was copied by hand and
 *                                  is the catalogue's
 *   POST /api/store/cancel
 *   POST /api/store/firmware       the firmware the official catalogue offers for
 *                                  this board: downloaded, checked and written to
 *                                  the other slot ("fw" in the state says which,
 *                                  and "ready" once it is a restart away); the
 *                                  restart is /api/ota/restart, as after an upload
 *
 * Every POST queues its job and answers with the state at once: ask again
 * to follow it.
 *
 * And the pieces by hand, from phase 0, for measuring on the board:
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
#include "aos_store.h"
#include "aos_fetch.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static void store_state(aos_httpd_req_t *r, int status, const char *error)
{
    static const char *states[] = { "absent", "installed", "update", "by_hand", "set_aside" };
    aos_store_status_t st;
    aos_store_status(&st);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", error == NULL);
    if (error) cJSON_AddStringToObject(o, "error", error);
    cJSON_AddStringToObject(o, "url", aos_store_url());
    cJSON_AddBoolToObject(o, "dev", aos_store_dev());
    cJSON *srcs = cJSON_AddArrayToObject(o, "sources");
    aos_store_source_t sr;
    for (int i = 0; aos_store_source(i, &sr); i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", sr.name);
        cJSON_AddStringToObject(e, "url", sr.url);
        cJSON_AddStringToObject(e, "release", sr.release);
        cJSON_AddNumberToObject(e, "apps", sr.apps);
        cJSON_AddNumberToObject(e, "skipped", sr.skipped);
        cJSON_AddItemToArray(srcs, e);
    }
    cJSON_AddStringToObject(o, "source", st.source);
    cJSON_AddStringToObject(o, "release", st.release);
    cJSON_AddBoolToObject(o, "busy", st.busy);
    if (st.busy) {
        cJSON *j = cJSON_AddObjectToObject(o, "job");
        cJSON_AddStringToObject(j, "what", st.job);
        cJSON_AddStringToObject(j, "file", st.file);
        cJSON_AddStringToObject(j, "step", st.step);
        cJSON_AddNumberToObject(j, "got", st.got);
        cJSON_AddNumberToObject(j, "total", st.total);
        cJSON_AddNumberToObject(j, "queued", st.queued);
    }
    cJSON_AddNumberToObject(o, "jobs_done", st.jobs_done);
    cJSON_AddBoolToObject(o, "last_ok", st.last_ok);
    cJSON_AddStringToObject(o, "last", st.last);
    static const char *kinds[] = { "", "other", "network", "not_there", "clock", "server", "card", "package", "rules", "firmware",
                                   "catalog", "source", "cancelled", "memory", "not_listed", "in_store", "not_store",
                                   "flash", "up_to_date" };
    cJSON_AddStringToObject(o, "last_kind", kinds[st.last_kind >= 0 && st.last_kind < (int)(sizeof kinds / sizeof kinds[0]) ? st.last_kind : 1]);
    char asked[200];
    if (aos_store_proposal(asked, sizeof asked)) cJSON_AddStringToObject(o, "proposed", asked);
    /* the system's firmware, as the official catalogue offers it for this board */
    aos_store_fw_t fw;
    bool offered = aos_store_fw(&fw);
    cJSON *f = cJSON_AddObjectToObject(o, "fw");
    cJSON_AddStringToObject(f, "running", aos_hal_firmware_version());
    cJSON_AddStringToObject(f, "profile", fw.profile);
    if (offered) {
        cJSON_AddStringToObject(f, "ver", fw.ver);
        cJSON_AddNumberToObject(f, "size", fw.size);
        cJSON_AddBoolToObject(f, "newer", fw.newer);
        cJSON_AddBoolToObject(f, "ready", fw.ready);
    }
    cJSON *apps = cJSON_AddArrayToObject(o, "apps");
    aos_store_app_t a;
    for (int i = 0; aos_store_app(i, &a); i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "file", a.file);
        cJSON_AddStringToObject(e, "id", a.id);
        cJSON_AddStringToObject(e, "ver", a.ver);
        cJSON_AddStringToObject(e, "have", a.have);
        cJSON_AddStringToObject(e, "state", states[a.state <= AOS_STORE_SET_ASIDE ? a.state : 0]);
        cJSON_AddBoolToObject(e, "fits", a.fits);
        cJSON_AddNumberToObject(e, "src", a.src);
        cJSON_AddStringToObject(e, "origin", a.origin == AOS_STORE_LOOSE ? "package" : a.origin == AOS_STORE_HAND ? "hand" : "source");
        if (a.system) cJSON_AddTrueToObject(e, "system");
        if (a.fresh) cJSON_AddTrueToObject(e, "new");
        cJSON_AddItemToArray(apps, e);
    }
    reply(r, status, o);
}

/* The store's own calls; false when 'p' is one of the pieces by hand. */
static bool store_api(aos_httpd_req_t *r, const char *p, bool get, bool post)
{
    if (!p[0] && get) { store_state(r, 200, NULL); return true; }
    if (!strcmp(p, "/catalog") && get) {
        size_t len = 0;
        char *text = aos_store_catalog(&len);
        if (!text) error(r, 404, _("todavía no hay catálogo: POST /api/store/refresh"));
        else if (aos_httpd_begin(r, 200, "application/x-ndjson; charset=utf-8", (long)len, NULL) && len) aos_httpd_write(r, text, len);
        free(text);
        return true;
    }
    if (post && !strcmp(p, "/sideload")) {
        char path[160] = "";
        const char *why = NULL, *root = aos_hal_path_sd_root();
        if (!root) why = _("sin tarjeta");
        else if (aos_httpd_query(r, "path", path, sizeof path) && path[0]) {
            if (!aos_store_sideload(path)) why = _("ruta inválida, o la cola está llena");
        } else {
            /* the body to the card first: a package may be megabytes */
            char full[260], dir[224];
            snprintf(dir, sizeof dir, "%s/apps/.store", root);
            mkdir(dir, 0755);
            snprintf(dir, sizeof dir, "%s/apps/.store/in", root);
            mkdir(dir, 0755);
            snprintf(full, sizeof full, "%s/upload.p4app", dir);
            FILE *f = fopen(full, "wb");
            char *buf = malloc(16384);
            long left = aos_httpd_body_len(r);
            bool ok = f && buf && left > 0 && left <= 64L * 1024 * 1024;
            while (ok && left > 0) {
                int n = aos_httpd_body_read(r, buf, left > 16384 ? 16384 : (int)left);
                if (n <= 0 || fwrite(buf, 1, (size_t)n, f) != (size_t)n) ok = false;
                left -= n;
            }
            if (f) ok = fclose(f) == 0 && ok;
            free(buf);
            if (!ok) why = _("no se pudo recibir el paquete");
            else if (!aos_store_sideload("apps/.store/in/upload.p4app")) why = _("la cola está llena");
        }
        store_state(r, why ? 400 : 200, why);
        return true;
    }
    if (!post || (strcmp(p, "/refresh") && strcmp(p, "/install") && strcmp(p, "/remove") && strcmp(p, "/adopt") && strcmp(p, "/replace") &&
                  strcmp(p, "/cancel") && strcmp(p, "/propose") && strcmp(p, "/firmware"))) return false;
    char *body = aos_httpd_body_all(r, 2048);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(j, "file"));
    const char *fail = NULL;
    if (!strcmp(p, "/refresh")) {
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(j, "url"));
        if (url && !aos_store_set_url(url)) fail = _("otro catálogo sólo en modo desarrollo, que se prende en la placa (Tienda, Fuentes)");
        else if (!aos_store_refresh()) fail = _("la cola está llena");
    } else if (!strcmp(p, "/install")) {
        if (!file || !aos_store_install(file, cJSON_GetStringValue(cJSON_GetObjectItem(j, "packs")))) fail = _("falta file, o la cola está llena");
    } else if (!strcmp(p, "/remove")) {
        if (!file || !aos_store_remove(file)) fail = _("falta file, o la cola está llena");
    } else if (!strcmp(p, "/propose")) {
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(j, "url"));
        if (!url || !aos_store_propose(url)) fail = _("eso no es una dirección de fuente: usuario/repositorio, o https://...");
    } else if (!strcmp(p, "/adopt")) {
        if (!aos_store_adopt()) fail = _("la cola está llena");
    } else if (!strcmp(p, "/replace")) {
        if (!aos_store_replace_by_hand()) fail = _("la cola está llena");
    } else if (!strcmp(p, "/firmware")) {
        if (!aos_store_fw_install()) fail = _("la cola está llena");
    } else aos_store_cancel();
    store_state(r, fail ? 400 : 200, fail);
    cJSON_Delete(j);
    return true;
}

bool aos_portal_store(aos_httpd_req_t *r, const char *m, const char *p)
{
    if (strncmp(p, "store", 5) || (p[5] && p[5] != '/')) return false;
    bool get = !strcmp(m, "GET"), post = !strcmp(m, "POST");
    if (store_api(r, p + 5, get, post)) return true;
    if (!p[5]) { error(r, 405, _("GET")); return true; }
    p += 6;

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
