/*
 * P4OS - The portal's side of the backups (components/aos_backup,
 * docs/plan/RESPALDO.md).
 *
 *   GET  /api/backup            the state: whether a job is running, how the
 *                               last one ended, and the backups on the card,
 *                               the newest first ({name, made, fw, board,
 *                               sealed, before, why, prefs, left_out, apps,
 *                               size}; "why" is "", "restore", "daily" or
 *                               "firmware"), and "auto": {on, sealed, day},
 *                               the copies the board makes by itself, which
 *                               are set at the board only
 *   POST /api/backup            {"pass": "..."}: makes one now. Without a
 *                               passphrase it is plain and leaves out what
 *                               looks like a credential; with one, everything
 *                               goes in, sealed
 *   POST /api/backup/restore    {"name": "...", "pass": "..."}: writes its
 *                               preferences over the ones there are, after
 *                               keeping those in a backup of their own. The
 *                               restart is the caller's (/api/restart)
 *   POST /api/backup/delete     {"name": "..."}
 *   POST /api/backup/drive      the backups and the chosen folders of the
 *                               card to the pendrive on the USB host, file
 *                               by file ("drive" in the state says there is
 *                               one; "files" and "kbytes", how far it is).
 *                               The way back, onto the card, is asked for at
 *                               the board
 *   POST /api/backup/server     {"do": "look"}: who answers at the server set at
 *                               the board, its key and whether the board gets
 *                               in ("server" in the state: host, port, user,
 *                               dir, on, pinned, fp, last, seen, seen_in, and
 *                               key_line, the board's public key for the
 *                               server's authorized_keys). {"do": "send"}:
 *                               the sealed backups the server lacks, now.
 *                               The server, its key's acceptance and the
 *                               week's switch are set at the board
 *   GET  /api/backup/apps?name= the apps that backup lists and the card lacks
 *   POST /api/backup/apps       {"name": "..."}: asks the Tienda for them
 *
 * The file itself travels by the calls for any file of the card:
 * GET /api/fs/get?path=/backups/<name>&attachment=1 to keep it on a
 * computer, PUT /api/fs/put?path=/backups/<name> to bring one back.
 *
 * A passphrase sent here travels as the portal's traffic does: in the clear
 * unless the portal is on HTTPS (docs/SECURITY.md). It is not kept.
 */
#include "aos_portal_backup.h"
#include "aos_backup.h"
#include "aos_store.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

#define LIST_MAX 24

static void reply(aos_httpd_req_t *r, int status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    aos_httpd_send_json(r, status, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
}

static void state(aos_httpd_req_t *r, int status, const char *error)
{
    static const char *kinds[] = { "", "other", "card", "not_found", "not_one", "passphrase", "newer", "memory", "busy", "drive",
                                   "setup", "server", "hostkey", "login" };
    aos_backup_status_t st;
    aos_backup_status(&st);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", error == NULL);
    if (error) cJSON_AddStringToObject(o, "error", error);
    cJSON_AddBoolToObject(o, "card", aos_hal_path_sd_root() != NULL);
    cJSON_AddBoolToObject(o, "busy", st.busy);
    cJSON_AddStringToObject(o, "job", st.busy ? st.job : "");
    cJSON_AddNumberToObject(o, "jobs_done", st.jobs_done);
    cJSON_AddBoolToObject(o, "last_ok", st.last_ok);
    cJSON_AddStringToObject(o, "last_job", st.last_job);
    cJSON_AddStringToObject(o, "last", st.last);
    cJSON_AddStringToObject(o, "last_kind", kinds[st.last_kind >= 0 && st.last_kind < (int)(sizeof kinds / sizeof kinds[0]) ? st.last_kind : 1]);
    cJSON_AddStringToObject(o, "last_name", st.name);
    cJSON_AddNumberToObject(o, "written", st.written);
    cJSON_AddBoolToObject(o, "restart", st.restart);
    cJSON_AddBoolToObject(o, "drive", aos_backup_drive());
    cJSON_AddNumberToObject(o, "files", st.files);
    cJSON_AddNumberToObject(o, "kbytes", st.kbytes);
    /* the copies the board makes by itself: read here, changed at the board */
    static const char *whys[] = { "", "restore", "daily", "firmware" };
    cJSON *au = cJSON_AddObjectToObject(o, "auto");
    cJSON_AddBoolToObject(au, "on", aos_backup_auto_on());
    cJSON_AddBoolToObject(au, "sealed", aos_backup_auto_sealed());
    cJSON_AddNumberToObject(au, "day", aos_backup_auto_day());
    /* the server the sealed copies go to: read here, set at the board. The
     * board's key is its public half: what the server's authorized_keys takes */
    aos_backup_server_t sv;
    char line[200] = "", seen[80] = "";
    bool in = false;
    aos_backup_server_get(&sv);
    cJSON *srv = cJSON_AddObjectToObject(o, "server");
    cJSON_AddStringToObject(srv, "host", sv.host);
    cJSON_AddNumberToObject(srv, "port", sv.port);
    cJSON_AddStringToObject(srv, "user", sv.user);
    cJSON_AddStringToObject(srv, "dir", sv.dir);
    cJSON_AddBoolToObject(srv, "on", sv.on);
    cJSON_AddBoolToObject(srv, "pinned", sv.pinned);
    cJSON_AddStringToObject(srv, "fp", sv.fp);
    cJSON_AddNumberToObject(srv, "last", sv.last);
    if (aos_backup_server_seen(seen, sizeof seen, &in)) {
        cJSON_AddStringToObject(srv, "seen", seen);
        cJSON_AddBoolToObject(srv, "seen_in", in);
    }
    if (aos_backup_server_key_line(line, sizeof line)) cJSON_AddStringToObject(srv, "key_line", line);
    cJSON *list = cJSON_AddArrayToObject(o, "backups");
    aos_backup_info_t *all = malloc(sizeof *all * LIST_MAX);
    int n = all ? aos_backup_list(all, LIST_MAX) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "name", all[i].name);
        cJSON_AddNumberToObject(b, "made", all[i].made);
        cJSON_AddStringToObject(b, "fw", all[i].fw);
        cJSON_AddStringToObject(b, "board", all[i].board);
        cJSON_AddBoolToObject(b, "sealed", all[i].sealed);
        cJSON_AddBoolToObject(b, "before", all[i].before);
        cJSON_AddNumberToObject(b, "prefs", all[i].prefs);
        cJSON_AddNumberToObject(b, "left_out", all[i].left_out);
        cJSON_AddNumberToObject(b, "apps", all[i].apps);
        cJSON_AddStringToObject(b, "why", whys[all[i].why >= 0 && all[i].why < 4 ? all[i].why : 0]);
        cJSON_AddNumberToObject(b, "size", all[i].size);
        cJSON_AddItemToArray(list, b);
    }
    free(all);
    reply(r, status, o);
}

/* The apps a backup lists and the card lacks; with 'ask', also asked of the
 * Tienda, each with the extras it had. */
static void apps(aos_httpd_req_t *r, const char *name, bool ask)
{
    aos_backup_app_t *all = malloc(sizeof *all * 96);
    int n = all && name ? aos_backup_missing_apps(name, all, 96) : 0, asked = 0;
    cJSON *o = cJSON_CreateObject(), *list = cJSON_AddArrayToObject(o, "apps");
    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "file", all[i].file);
        cJSON_AddStringToObject(a, "packs", all[i].packs);
        cJSON_AddItemToArray(list, a);
        /* what it cannot start without, and the extras it had */
        if (ask && aos_store_install(all[i].file, all[i].packs[0] ? all[i].packs : "need")) asked++;
    }
    free(all);
    cJSON_AddBoolToObject(o, "ok", true);
    if (ask) cJSON_AddNumberToObject(o, "asked", asked);
    reply(r, 200, o);
}

bool aos_portal_backup(aos_httpd_req_t *r, const char *m, const char *p)
{
    if (strncmp(p, "backup", 6) || (p[6] && p[6] != '/')) return false;
    bool get = !strcmp(m, "GET"), post = !strcmp(m, "POST");
    p += p[6] ? 7 : 6;
    if (get && !p[0]) { state(r, 200, NULL); return true; }
    if (get && !strcmp(p, "apps")) {
        char name[AOS_BACKUP_NAME_MAX] = "";
        aos_httpd_query(r, "name", name, sizeof name);
        apps(r, name, false);
        return true;
    }
    if (!post) { state(r, 405, _("GET o POST")); return true; }

    char *body = aos_httpd_body_all(r, 2048);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    if (body) memset(body, 0, strlen(body));
    free(body);
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(j, "pass"));
    const char *fail = NULL;
    if (!p[0]) {
        if (!aos_backup_make(pass)) fail = _("ya hay un respaldo en curso");
    } else if (!strcmp(p, "restore")) {
        if (!name || !aos_backup_restore(name, pass)) fail = _("falta name, o ya hay un respaldo en curso");
    } else if (!strcmp(p, "server")) {
        /* a look, or the copy now; where the server is and its key are the board's to say */
        const char *what = cJSON_GetStringValue(cJSON_GetObjectItem(j, "do"));
        if (what && !strcmp(what, "look")) { if (!aos_backup_server_look()) fail = _("ya hay un respaldo en curso"); }
        else if (what && !strcmp(what, "send")) { if (!aos_backup_server_send()) fail = _("ya hay un respaldo en curso"); }
        else fail = _("falta do: look o send");
    } else if (!strcmp(p, "drive")) {
        if (!aos_backup_drive()) fail = _("no hay un pendrive");
        else if (!aos_backup_to_drive()) fail = _("ya hay un respaldo en curso");
    } else if (!strcmp(p, "delete")) {
        if (!name || !aos_backup_delete(name)) fail = _("no se pudo borrar");
    } else if (!strcmp(p, "apps")) {
        apps(r, name, true);
        cJSON_Delete(j);
        return true;
    } else fail = _("no existe");
    state(r, fail ? 400 : 200, fail);
    cJSON_Delete(j);
    return true;
}
