/*
 * P4OS - Mi casa: estado.txt, the services and the options.
 *
 * A text file in the app's data folder (/data/estado.txt for the portal),
 * which the app and its page both read and write. The options first, then
 * one block per service, in the order they are shown:
 *
 *     fails = 2               failures in a row before it is "down" (1-5)
 *     sound = 1               a beep with each notice
 *     boot  = 1               start watching with the board
 *
 *     [Domótica]
 *     group   = Casa
 *     tmpl    = ha            the template it came from: its glyph
 *     type    = http          http (also https) or tcp
 *     url     = http://10.20.30.10:8123/manifest.json
 *     expect  = 200           codes that count as up: 200, 200-399, 200,401
 *     word    = Home Assistant  must be in the response (any case)
 *     host    = 10.20.30.5    tcp
 *     port    = 445           tcp
 *     every   = 60            seconds between checks
 *     slow    = 800           ms above which it is yellow
 *     timeout = 5000          ms
 *     cert    = 14            warn when the certificate has fewer days left
 *     paused  = 1
 *     trust   = 1             trust the certificate saved for it
 *
 * Only the name and the url (or host and port) are needed.
 */
#include "ec.h"

#include <stdlib.h>
#include <sys/stat.h>

#define FILE_NAME "estado.txt"
#define MAX_LINE  320

static const ec_tmpl_t TMPL[] = {
    /* word        label                kind     tls    port  path              expect     match             group           glyph                    every slow */
    { "ha",       "Home Assistant",    EC_HTTP, false, 8123, "/manifest.json", "200",     "Home Assistant", N_("Casa"),     AOS_SYM_HOME_ASSISTANT,  60, 800 },
    { "frigate",  "Frigate",           EC_HTTP, false, 5000, "/api/config",    "200",     "cameras",        N_("Cámaras"),  AOS_SYM_CCTV,            60, 800 },
    { "nas",      N_("NAS"),           EC_TCP,  false, 445,  "",               "",        "",               N_("Red"),      AOS_SYM_DATABASE,        60, 200 },
    { "printer",  N_("Impresora 3D"),  EC_HTTP, false, 80,   "/",              "200-399", "",               N_("Taller"),   AOS_SYM_PACKAGE_VARIANT, 60, 1000 },
    { "router",   N_("Router"),        EC_HTTP, false, 80,   "/",              "200-499", "",               N_("Red"),      AOS_SYM_ROUTER_WIRELESS, 30, 300 },
    { "web",      N_("Página web"),    EC_HTTP, true,  443,  "/",              "200-399", "",               N_("Internet"), AOS_SYM_WEB,             300, 1500 },
    { "tcp",      N_("Puerto TCP"),    EC_TCP,  false, 22,   "",               "",        "",               N_("Red"),      AOS_SYM_LAN,             60, 200 },
};
#define TMPL_N (int)(sizeof TMPL / sizeof TMPL[0])

int ec_tmpl_n(void) { return TMPL_N; }
const ec_tmpl_t *ec_tmpl(int i) { return &TMPL[i >= 0 && i < TMPL_N ? i : TMPL_N - 1]; }

int ec_tmpl_find(const char *word)
{
    for (int i = 0; i < TMPL_N; i++)
        if (!strcmp(TMPL[i].word, word)) return i;
    return -1;
}

void ec_copy(char *dst, size_t n, const char *src)
{
    if (!n) return;
    size_t k = 0;
    if (src) for (; src[k] && k + 1 < n; k++) dst[k] = src[k];
    dst[k] = '\0';
}

void ec_svc_from_tmpl(ec_svc_t *s, int t, const char *host)
{
    const ec_tmpl_t *m = ec_tmpl(t);
    memset(s, 0, sizeof *s);
    s->tmpl = (uint8_t)(t >= 0 && t < TMPL_N ? t : TMPL_N - 1);
    s->kind = m->kind;
    ec_copy(s->name, sizeof s->name, _(m->label));
    ec_copy(s->group, sizeof s->group, _(m->group));
    /* no made-up address: one that is not changed would be tried for real */
    const char *h = host ? host : "";
    if (m->kind == EC_HTTP) {
        bool def = m->port == (m->tls ? 443 : 80);
        if (def) snprintf(s->url, sizeof s->url, "%s://%.96s%s", m->tls ? "https" : "http", h, m->path);
        else snprintf(s->url, sizeof s->url, "%s://%.96s:%d%s", m->tls ? "https" : "http", h, m->port, m->path);
    } else {
        ec_copy(s->host, sizeof s->host, h);
    }
    s->port = m->port;
    ec_copy(s->expect, sizeof s->expect, m->expect);
    ec_copy(s->word, sizeof s->word, m->match);
    s->every_s = m->every_s;
    s->slow_ms = m->slow_ms;
    s->timeout_ms = 5000;
    s->cert_days = m->tls ? 14 : 0;
}

void ec_cfg_defaults(ec_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->fails = 2;
    c->sound = true;
    c->boot = true;
}

void ec_cfg_path(char *out, size_t n)
{
    const char *env = getenv("EC_CONFIG");      /* the simulator's tests */
    if (env && env[0]) snprintf(out, n, "%s", env);
    else snprintf(out, n, "%s/%s", aos_hal_path_data(), FILE_NAME);
}

uint32_t ec_cfg_stamp(void)
{
    char path[200];
    ec_cfg_path(path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint32_t)st.st_size * 2654435761u ^ (uint32_t)st.st_mtime;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void fix(ec_svc_t *s)
{
    if (!s->every_s) s->every_s = s->kind == EC_TCP ? 60 : 60;
    s->every_s = clampi(s->every_s, EC_EVERY_MIN, EC_EVERY_MAX);
    if (!s->timeout_ms) s->timeout_ms = 5000;
    s->timeout_ms = clampi(s->timeout_ms, EC_TO_MIN, EC_TO_MAX);
    if (s->slow_ms <= 0) s->slow_ms = s->kind == EC_TCP ? 200 : 800;
    s->slow_ms = clampi(s->slow_ms, 10, 60000);
    s->cert_days = clampi(s->cert_days, 0, 365);
    if (s->kind == EC_TCP) s->port = clampi(s->port, 0, 65535);
    if (s->kind == EC_HTTP && !s->expect[0]) ec_copy(s->expect, sizeof s->expect, "200-399");
}

bool ec_cfg_load(ec_cfg_t *c)
{
    ec_cfg_defaults(c);
    char path[200];
    ec_cfg_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char *line = malloc(MAX_LINE);
    if (!line) {
        fclose(f);
        return false;
    }
    ec_svc_t *cur = NULL;
    while (fgets(line, MAX_LINE, f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            if (cur) fix(cur);
            cur = NULL;
            if (c->n >= EC_MAX) continue;
            char *e = strrchr(s, ']');
            if (e) *e = '\0';
            char *name = trim(s + 1);
            if (!*name) continue;
            cur = &c->s[c->n++];
            memset(cur, 0, sizeof *cur);
            cur->tmpl = (uint8_t)(TMPL_N - 1);
            ec_copy(cur->name, sizeof cur->name, name);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(s), *v = trim(eq + 1);
        if (!cur) {
            if (!strcmp(k, "fails")) c->fails = clampi(atoi(v), 1, 5);
            else if (!strcmp(k, "sound")) c->sound = atoi(v) != 0;
            else if (!strcmp(k, "boot")) c->boot = atoi(v) != 0;
            continue;
        }
        if (!strcmp(k, "group")) ec_copy(cur->group, sizeof cur->group, v);
        else if (!strcmp(k, "tmpl")) {
            int t = ec_tmpl_find(v);
            if (t >= 0) cur->tmpl = (uint8_t)t;
        }
        else if (!strcmp(k, "type")) cur->kind = !strcmp(v, "tcp") ? EC_TCP : EC_HTTP;
        else if (!strcmp(k, "url")) ec_copy(cur->url, sizeof cur->url, v);
        else if (!strcmp(k, "host")) ec_copy(cur->host, sizeof cur->host, v);
        else if (!strcmp(k, "port")) cur->port = atoi(v);
        else if (!strcmp(k, "expect")) ec_copy(cur->expect, sizeof cur->expect, v);
        else if (!strcmp(k, "word")) ec_copy(cur->word, sizeof cur->word, v);
        else if (!strcmp(k, "every")) cur->every_s = atoi(v);
        else if (!strcmp(k, "slow")) cur->slow_ms = atoi(v);
        else if (!strcmp(k, "timeout")) cur->timeout_ms = atoi(v);
        else if (!strcmp(k, "cert")) cur->cert_days = atoi(v);
        else if (!strcmp(k, "paused")) cur->paused = atoi(v) != 0;
        else if (!strcmp(k, "trust")) cur->trust = atoi(v) != 0;
    }
    if (cur) fix(cur);
    free(line);
    fclose(f);
    return true;
}

bool ec_cfg_save(const ec_cfg_t *c)
{
    char path[200], part[210];
    ec_cfg_path(path, sizeof path);
    snprintf(part, sizeof part, "%s.part", path);
    FILE *f = fopen(part, "w");
    if (!f) return false;
    fputs("# Home status for P4OS (apps/estado). The options, then one block per service:\n"
          "# [Name] group, tmpl, type (http or tcp), url (http) or host and port (tcp),\n"
          "# expect (codes: 200, 200-399, 200,401), word (must be in the answer), every (s),\n"
          "# slow (ms), timeout (ms), cert (days of warning), paused, trust.\n", f);
    fprintf(f, "fails = %d\nsound = %d\nboot = %d\n", c->fails, c->sound ? 1 : 0, c->boot ? 1 : 0);
    for (int i = 0; i < c->n; i++) {
        const ec_svc_t *s = &c->s[i];
        fprintf(f, "\n[%s]\n", s->name);
        if (s->group[0]) fprintf(f, "group = %s\n", s->group);
        fprintf(f, "tmpl = %s\n", ec_tmpl(s->tmpl)->word);
        if (s->kind == EC_TCP) {
            fprintf(f, "type = tcp\nhost = %s\nport = %d\n", s->host, s->port);
        } else {
            fprintf(f, "type = http\nurl = %s\n", s->url);
            if (s->expect[0]) fprintf(f, "expect = %s\n", s->expect);
            if (s->word[0]) fprintf(f, "word = %s\n", s->word);
            fprintf(f, "cert = %d\n", s->cert_days);
            if (s->trust) fputs("trust = 1\n", f);
        }
        fprintf(f, "every = %d\nslow = %d\ntimeout = %d\n", s->every_s, s->slow_ms, s->timeout_ms);
        if (s->paused) fputs("paused = 1\n", f);
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) {
        unlink(part);
        return false;
    }
    unlink(path);
    return rename(part, path) == 0;
}

/* ---- addresses ---- */

bool ec_url_split(const char *url, char *host, size_t hn, int *port, char *path, size_t pn, bool *tls)
{
    const char *p = url;
    while (*p == ' ') p++;
    *tls = false;
    if (!strncasecmp(p, "https://", 8)) {
        *tls = true;
        p += 8;
    } else if (!strncasecmp(p, "http://", 7)) {
        p += 7;
    } else {
        return false;
    }
    const char *end = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', (size_t)(end - p));
    if (at) p = at + 1;
    const char *colon = memchr(p, ':', (size_t)(end - p));
    size_t hl = (size_t)((colon ? colon : end) - p);
    if (!hl || hl >= hn) return false;
    memcpy(host, p, hl);
    host[hl] = '\0';
    *port = colon ? atoi(colon + 1) : (*tls ? 443 : 80);
    if (*port <= 0 || *port > 65535) return false;
    const char *rest = end;
    size_t k = 0;
    if (*rest != '/') path[k++] = '/';
    for (; *rest && *rest != '#' && k + 4 < pn; rest++) {
        if (*rest == ' ') {
            path[k++] = '%';
            path[k++] = '2';
            path[k++] = '0';
        } else {
            path[k++] = *rest;
        }
    }
    path[k] = '\0';
    return true;
}

const char *ec_svc_addr(const ec_svc_t *s, char *out, size_t n)
{
    if (s->kind == EC_TCP) snprintf(out, n, "%.96s:%d", s->host, s->port);
    else {
        const char *u = s->url;
        if (!strncmp(u, "http://", 7)) u += 7;      /* https stays visible */
        ec_copy(out, n, u);
    }
    return out;
}

/* "200", "200-399", "200,301-302,401" */
bool ec_expect_ok(const char *expect, int code)
{
    if (!expect || !expect[0]) return code >= 200 && code < 400;
    const char *p = expect;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        char *e;
        long lo = strtol(p, &e, 10);
        if (e == p) break;
        long hi = lo;
        p = e;
        while (*p == ' ') p++;
        if (*p == '-') {
            p++;
            hi = strtol(p, &e, 10);
            if (e == p) hi = lo;
            p = e;
        }
        if (code >= lo && code <= hi) return true;
        while (*p && *p != ',') p++;
    }
    return false;
}

uint32_t ec_svc_key(const ec_svc_t *s)
{
    /* FNV-1a of the name, as typed: a renamed service starts a new history */
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s->name; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h ? h : 1;
}
