/*
 * P4OS - the portal's side of the Claude app: signing in, which wants a
 * real keyboard, and the numbers.
 *
 *   GET  /api/claude              state, the windows, when they were read
 *   POST /api/claude/login        starts a sign-in: {"url": the page to open}
 *   POST /api/claude/code {code}  what that page showed ("code#state")
 *   POST /api/claude/logout
 *
 * The tokens never come out of here: the answer says only whether there is
 * a session.
 */
#include "aos_portal_claude.h"
#include "aos_claude.h"
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

static void add_window(cJSON *o, const char *k, const aos_claude_window_t *w)
{
    if (!w->valid) { cJSON_AddNullToObject(o, k); return; }
    cJSON *j = cJSON_AddObjectToObject(o, k);
    cJSON_AddNumberToObject(j, "pct", round(w->pct * 10) / 10);
    if (w->resets) cJSON_AddNumberToObject(j, "resets", (double)w->resets);
    else cJSON_AddNullToObject(j, "resets");
}

static void api_status(aos_httpd_req_t *r)
{
    static const char *const ST[] = { "signed_out", "waiting_code", "exchanging", "ok", "expired", "error" };
    aos_claude_status_t st;
    aos_claude_status(&st);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", st.state < 6 ? ST[st.state] : "?");
    cJSON_AddStringToObject(o, "error", st.error);
    add_window(o, "five_hour", &st.five_hour);
    add_window(o, "seven_day", &st.seven_day);
    add_window(o, "seven_day_opus", &st.seven_day_opus);
    add_window(o, "seven_day_sonnet", &st.seven_day_sonnet);
    cJSON_AddBoolToObject(o, "extra_enabled", st.extra_enabled);
    if (st.extra_pct >= 0) cJSON_AddNumberToObject(o, "extra_pct", st.extra_pct);
    if (st.fetched) cJSON_AddNumberToObject(o, "fetched", (double)st.fetched);
    else cJSON_AddNullToObject(o, "fetched");
    cJSON_AddNumberToObject(o, "now", (double)time(NULL));
    send_cjson(r, 200, o);
}

bool aos_portal_claude(aos_httpd_req_t *r, const char *m, const char *p)
{
    if (strncmp(p, "claude", 6)) return false;
    p += 6;
    bool post = !strcmp(m, "POST"), get = !strcmp(m, "GET");
    if (get && !*p) { aos_claude_refresh_now(); api_status(r); return true; }
    if (post && !strcmp(p, "/login")) {
        char url[900];
        if (!aos_claude_login_url(url, sizeof url)) { aos_httpd_send_json(r, 500, "{\"ok\":false,\"error\":\"no se pudo armar el enlace\"}"); return true; }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "url", url);
        send_cjson(r, 200, o);
        return true;
    }
    if (post && !strcmp(p, "/code")) {
        char *b = aos_httpd_body_all(r, 2048);
        cJSON *j = b ? cJSON_Parse(b) : NULL;
        free(b);
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "code");
        bool ok = cJSON_IsString(c) && aos_claude_login_code(c->valuestring);
        cJSON_Delete(j);
        if (ok) aos_httpd_send_json(r, 200, "{\"ok\":true}");
        else {
            aos_claude_status_t st;
            aos_claude_status(&st);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddFalseToObject(o, "ok");
            cJSON_AddStringToObject(o, "error", st.error[0] ? st.error : "eso no parece un código");
            send_cjson(r, 400, o);
        }
        return true;
    }
    if (post && !strcmp(p, "/logout")) { aos_claude_logout(); aos_httpd_send_json(r, 200, "{\"ok\":true}"); return true; }
    return false;
}
