/*
 * P4OS - who may talk to the portal.
 *
 * Two attacks that need no access to the network, only the user's browser
 * on it, and that worked until 2026-10-04:
 *
 *   A page from anywhere sends requests to the board. A form or a fetch()
 *   with a text/plain body is a "simple" request: the browser sends it with
 *   no preflight, and the board used to act on it (delete a file, change
 *   the Wi-Fi, restart). The browser now says where a request comes from
 *   (Sec-Fetch-Site, Origin): the API answers only the portal's own pages.
 *   A tool (curl, the scripts in tools/) sends neither header and is not
 *   affected.
 *
 *   DNS rebinding: a name of the attacker's that first points to their
 *   server and then to the board makes the browser treat the board as that
 *   site, and read its answers. The Host the browser sends is then the
 *   attacker's name: the portal answers only to its own (an IP, or the
 *   board's name, with or without .local).
 *
 * And then the rules of aos_access.h: where the request came in (the cable,
 * the board's AP, a home network, any other) says whether it needs nothing,
 * a session or the token, or finds the portal closed. The session is a
 * cookie that /api/login sets after the password; the token goes in an
 * Authorization: Bearer header, for the scripts in tools/. The rules and the
 * password are set only on the board (Settings, Portal): someone who got
 * into the portal cannot open it further.
 */
#include "aos_portal_access.h"
#include "aos_hal.h"
#include "aos_access.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

/* "192.168.1.102", "[fe80::1]" */
static bool is_ip_literal(const char *h, size_t n)
{
    if (n && h[0] == '[') return true;
    int dots = 0;
    for (size_t i = 0; i < n; i++) {
        if (h[i] == '.') dots++;
        else if (!isdigit((unsigned char)h[i])) return false;
    }
    return dots == 3;
}

/* the board's name, bare or with a suffix a home router or mDNS gives it */
static bool is_our_name(const char *h, size_t n)
{
    static const char *const SUFFIX[] = { "", ".local", ".lan", ".home", ".home.arpa", ".localdomain" };
    const char *name = aos_hal_device_name();
    size_t nl = strlen(name);
    if (n == 9 && !strncasecmp(h, "localhost", 9)) return true;
    if (n < nl || strncasecmp(h, name, nl)) return false;
    for (size_t i = 0; i < sizeof SUFFIX / sizeof SUFFIX[0]; i++)
        if (n - nl == strlen(SUFFIX[i]) && !strncasecmp(h + nl, SUFFIX[i], n - nl)) return true;
    return false;
}

/* The host part of "name:port" (or of a whole "http://name:port"). */
static void host_part(const char *s, const char **h, size_t *n)
{
    const char *p = strstr(s, "://");
    p = p ? p + 3 : s;
    const char *end = p[0] == '[' ? strchr(p, ']') : NULL;
    if (end) end++;
    else {
        end = p;
        while (*end && *end != ':' && *end != '/') end++;
    }
    *h = p;
    *n = (size_t)(end - p);
}

static void deny(aos_httpd_req_t *r, int status, const char *why)
{
    aos_hal_log("portal", "refused %s %s: %s", aos_httpd_method(r), aos_httpd_path(r), why);
    char json[160];
    snprintf(json, sizeof json, "{\"error\":\"%s\"}", why);
    aos_httpd_send_json(r, status, json);
}

static int s_https_port;
void aos_portal_set_https_port(int port) { s_https_port = port; }

/* ---- where a request came in ---- */

static aos_zone_t zone_of(aos_httpd_req_t *r)
{
    char ip[16] = "";
    aos_httpd_local_ip(r, ip, sizeof ip);
#ifdef AOS_SIM
    /* the simulator answers on the Mac's own address: the cable, unless a
     * test asks to be somewhere else (P4_SIM_ZONE=ap|home|away) */
    const char *z = getenv("P4_SIM_ZONE");
    if (z && !strcmp(z, "ap")) return AOS_ZONE_AP;
    if (z && !strcmp(z, "home")) return AOS_ZONE_HOME;
    if (z && !strcmp(z, "away")) return AOS_ZONE_AWAY;
    if (!strcmp(ip, "127.0.0.1")) return AOS_ZONE_CABLE;
#endif
    if (!strcmp(ip, "192.168.7.1")) return AOS_ZONE_CABLE;
    if (!strcmp(ip, aos_hal_net_ap_ip())) return AOS_ZONE_AP;
    return aos_access_here_trusted() ? AOS_ZONE_HOME : AOS_ZONE_AWAY;
}

static const char *const ZONE[] = { "cable", "ap", "home", "away" };

/* the value of cookie 'name', into out */
static bool cookie(aos_httpd_req_t *r, const char *name, char *out, size_t n)
{
    const char *c = aos_httpd_header(r, "Cookie");
    size_t nl = strlen(name);
    for (const char *p = c; p && *p; ) {
        while (*p == ' ' || *p == ';') p++;
        if (!strncmp(p, name, nl) && p[nl] == '=') {
            const char *v = p + nl + 1;
            size_t l = strcspn(v, ";");
            if (l >= n) return false;
            memcpy(out, v, l);
            out[l] = 0;
            return true;
        }
        p = strchr(p, ';');
    }
    return false;
}

static bool authed(aos_httpd_req_t *r)
{
    char id[AOS_ACCESS_SESSION_LEN + 2];
    if (cookie(r, "p4s", id, sizeof id) && aos_access_session_ok(id)) return true;
    const char *a = aos_httpd_header(r, "Authorization");
    return a && !strncasecmp(a, "Bearer ", 7) && aos_access_token_ok(a + 7);
}

static void send_cjson_access(aos_httpd_req_t *r, int status, cJSON *o, const char *extra)
{
    char *s = cJSON_PrintUnformatted(o);
    const char *body = s ? s : "{}";
    if (aos_httpd_begin(r, status, "application/json", (long)strlen(body), extra)) aos_httpd_write(r, body, strlen(body));
    free(s);
    cJSON_Delete(o);
}

/* GET /api/auth: what this request needs, and whether it has it. The page
 * asks before anything else, to show the login or the portal. */
static void api_auth(aos_httpd_req_t *r, aos_zone_t zone, aos_access_t need)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "zone", ZONE[zone]);
    cJSON_AddStringToObject(o, "need", need == AOS_ACCESS_OPEN ? "open" : need == AOS_ACCESS_LOGIN ? "login" : "closed");
    cJSON_AddBoolToObject(o, "ok", need == AOS_ACCESS_OPEN || (need == AOS_ACCESS_LOGIN && authed(r)));
    char id[AOS_ACCESS_SESSION_LEN + 2];
    cJSON_AddBoolToObject(o, "session", cookie(r, "p4s", id, sizeof id) && aos_access_session_ok(id));
    cJSON_AddBoolToObject(o, "has_password", aos_access_has_password());
    cJSON_AddStringToObject(o, "name", aos_hal_device_name());
    send_cjson_access(r, 200, o, NULL);
}

/* POST /api/login {"password": "..."}: a session, as a cookie the page's
 * scripts cannot read and no other site sends */
static void api_login(aos_httpd_req_t *r)
{
    char *raw = aos_httpd_body_all(r, 512);
    cJSON *b = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    const cJSON *pw = cJSON_GetObjectItem(b, "password");
    int wait = 0;
    bool ok = aos_access_has_password() && cJSON_IsString(pw) && aos_access_check_password(pw->valuestring, &wait);
    cJSON_Delete(b);
    cJSON *o = cJSON_CreateObject();
    if (!ok) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", wait ? "demasiados intentos: esperá un poco" : "contraseña incorrecta");
        if (wait) cJSON_AddNumberToObject(o, "wait_s", wait);
        send_cjson_access(r, 403, o, NULL);
        return;
    }
    char id[AOS_ACCESS_SESSION_LEN + 1], extra[200];
    aos_access_session_new(id);
    snprintf(extra, sizeof extra, "Set-Cookie: p4s=%s; Path=/; Max-Age=31536000; HttpOnly; SameSite=Strict%s\r\n",
             id, aos_httpd_is_tls(r) ? "; Secure" : "");
    cJSON_AddBoolToObject(o, "ok", true);
    aos_hal_log("access", "a session opened");
    send_cjson_access(r, 200, o, extra);
}

static void api_logout(aos_httpd_req_t *r)
{
    char id[AOS_ACCESS_SESSION_LEN + 2];
    if (cookie(r, "p4s", id, sizeof id)) aos_access_session_end(id);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    send_cjson_access(r, 200, o, "Set-Cookie: p4s=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict\r\n");
}

static const char CLOSED_PAGE[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>P4OS</title><body style='font:17px -apple-system,system-ui,sans-serif;max-width:560px;margin:60px auto;padding:0 20px'>"
    "<h1>Portal cerrado en esta red</h1><p>Esta red no está marcada como de confianza en la placa. "
    "Se entra igual por la red propia de la placa o por el cable USB, o se abre con contraseña desde "
    "<b>Ajustes, Portal web</b>, en la placa.</p>";

bool aos_portal_access(aos_httpd_req_t *r)
{
    const char *host = aos_httpd_header(r, "Host");
    if (host) {
        const char *h;
        size_t n;
        host_part(host, &h, &n);
        if (!is_ip_literal(h, n) && !is_our_name(h, n)) {
            deny(r, 421, "nombre desconocido: entrá por la IP de la placa o por su nombre .local");
            return false;
        }
    }
    if (strncmp(aos_httpd_path(r), "/api/", 5)) return true;    /* the page and its scripts: nothing to protect */

    /* only the portal's own pages call the API from a browser */
    const char *site = aos_httpd_header(r, "Sec-Fetch-Site");
    if (site && strcasecmp(site, "same-origin") && strcasecmp(site, "none")) {
        deny(r, 403, "pedido desde otra página: rechazado");
        return false;
    }
    const char *origin = aos_httpd_header(r, "Origin");
    if (origin && strcmp(origin, "null")) {
        const char *oh, *hh;
        size_t on, hn;
        host_part(origin, &oh, &on);
        host_part(host ? host : "", &hh, &hn);
        /* the port too: another service on the same address is another site */
        const char *op = oh + on, *hp = hh + hn;
        bool same = on == hn && !strncasecmp(oh, hh, on) &&
                    !strcmp(*op == ':' ? op : "", *hp == ':' ? hp : "");
        if (!same) {
            deny(r, 403, "pedido desde otra página: rechazado");
            return false;
        }
    } else if (origin) {
        deny(r, 403, "pedido sin origen: rechazado");      /* a sandboxed frame, a file:// page */
        return false;
    }
    return true;
}

/* The rules: called for every request after the checks above (the page's
 * own scripts included: they say nothing about the board). */
bool aos_portal_rules(aos_httpd_req_t *r)
{
    const char *p = aos_httpd_path(r), *m = aos_httpd_method(r);
    bool api = !strncmp(p, "/api/", 5);
    aos_zone_t zone = zone_of(r);
    aos_access_t need = aos_access_need(zone);
    if (api && !strcmp(p, "/api/auth")) { api_auth(r, zone, need); return false; }
    if (need == AOS_ACCESS_CLOSED) {
        if (api) deny(r, 403, "el portal está cerrado en esta red");
        else aos_httpd_send(r, 403, "text/html; charset=utf-8", CLOSED_PAGE, sizeof CLOSED_PAGE - 1);
        return false;
    }
    /* away from home with HTTPS on, plain HTTP is only a way to HTTPS: the
     * password and the session must not cross a stranger's network in clear */
    if (zone == AOS_ZONE_AWAY && s_https_port && !aos_httpd_is_tls(r)) {
        const char *host = aos_httpd_header(r, "Host");
        const char *h;
        size_t n;
        host_part(host ? host : aos_hal_net_ip(), &h, &n);
        char loc[200], port[8] = "";
        if (s_https_port != 443) snprintf(port, sizeof port, ":%d", s_https_port);
        snprintf(loc, sizeof loc, "Location: https://%.*s%s%s\r\n", (int)n, h, port, api ? "/" : p);
        /* 302, not 301: a browser keeps a 301 for good, and HTTPS may go off */
        if (aos_httpd_begin(r, 302, "text/plain", 0, loc)) {}
        return false;
    }
    if (api && !strcmp(p, "/api/login") && !strcmp(m, "POST")) { api_login(r); return false; }
    if (api && !strcmp(p, "/api/logout") && !strcmp(m, "POST")) { api_logout(r); return false; }
    if (need == AOS_ACCESS_LOGIN && api && !authed(r)) {
        aos_httpd_send_json(r, 401, "{\"error\":\"hace falta entrar con la contraseña\",\"login\":true}");
        return false;
    }
    return true;
}
