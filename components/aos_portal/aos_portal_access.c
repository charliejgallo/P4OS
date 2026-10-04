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
 */
#include "aos_portal_access.h"
#include "aos_hal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

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
