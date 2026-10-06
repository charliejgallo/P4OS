/*
 * P4OS - VNC viewer: vnc.txt, the saved computers.
 *
 * A text file in the app's data folder (/data/vnc.txt for the portal), which
 * both the app and its page in the portal read and write. One block per
 * computer:
 *
 *     [Estudio]
 *     host    = 192.168.0.20
 *     port    = 5900
 *     pass    = secreto
 *     enc     = auto          auto, tight, zrle, hextile, raw
 *     depth   = 16            16 (fast) or 24 (exact colours)
 *     quality = 6             Tight's JPEG, 0-9
 *     view    = 0             1: look only, nothing is sent
 *     zone    = 1920,404,2940,1912   only that part: one monitor of several
 *
 * Only host is needed. "host:1" is display 1 (port 5901), "host::5901" a
 * port, as other viewers write them. The password is kept in the clear: the
 * app and the page both say so where it is typed.
 */
#include "vnc.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FILE_NAME   "vnc.txt"
#define MAX_LINE    256

static char s_path[160];

static const char *ENC_NAMES[VNC_ENC_COUNT] = { "auto", "tight", "zrle", "hextile", "raw" };

const char *vnc_enc_name(int enc)
{
    return enc >= 0 && enc < VNC_ENC_COUNT ? ENC_NAMES[enc] : ENC_NAMES[0];
}

const char *vnc_cfg_path(void)
{
    const char *env = getenv("VNC_CONFIG");     /* the simulator's tests */
    if (env && env[0]) {
        snprintf(s_path, sizeof s_path, "%s", env);
    } else {
        snprintf(s_path, sizeof s_path, "%s/%s", aos_hal_path_data(), FILE_NAME);
    }
    return s_path;
}

uint32_t vnc_cfg_stamp(void)
{
    struct stat st;
    if (stat(vnc_cfg_path(), &st) != 0) {
        return 0;
    }
    return (uint32_t)st.st_size * 2654435761u ^ (uint32_t)st.st_mtime;
}

void vnc_server_defaults(vnc_server_t *s)
{
    memset(s, 0, sizeof *s);
    s->port = 5900;
    s->enc = VNC_ENC_AUTO;
    s->depth = 16;
    s->quality = 6;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

/* "host:1" (a display) and "host::5901" (a port) */
static void split_host(vnc_server_t *s)
{
    char *c = strchr(s->host, ':');
    if (!c || strchr(c + 1, ':') != strrchr(c + 1, ':')) {
        return;                     /* none, or an IPv6 address */
    }
    if (c[1] == ':') {
        int port = atoi(c + 2);
        if (port > 0 && port < 65536) s->port = port;
    } else if (!strchr(c + 1, ':')) {
        int n = atoi(c + 1);
        if (n >= 0 && n < 100) s->port = 5900 + n;
        else if (n > 0 && n < 65536) s->port = n;
    } else {
        return;
    }
    *c = '\0';
}

void vnc_cfg_load(vnc_cfg_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    FILE *f = fopen(vnc_cfg_path(), "r");
    if (!f) {
        return;
    }
    char line[MAX_LINE];
    vnc_server_t *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!s[0] || s[0] == '#' || s[0] == ';') {
            continue;
        }
        if (s[0] == '[') {
            char *e = strchr(s, ']');
            if (e) *e = '\0';
            cur = NULL;
            if (cfg->count < VNC_MAX_SERVERS) {
                cur = &cfg->s[cfg->count++];
                vnc_server_defaults(cur);
                snprintf(cur->name, sizeof cur->name, "%s", trim(s + 1));
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq || !cur) {
            continue;
        }
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);
        if (!strcmp(key, "host")) {
            snprintf(cur->host, sizeof cur->host, "%s", val);
        } else if (!strcmp(key, "port")) {
            int p = atoi(val);
            if (p > 0 && p < 65536) cur->port = p;
        } else if (!strcmp(key, "pass")) {
            snprintf(cur->pass, sizeof cur->pass, "%s", val);
        } else if (!strcmp(key, "enc")) {
            for (int i = 0; i < VNC_ENC_COUNT; i++) {
                if (!strcmp(val, ENC_NAMES[i])) cur->enc = i;
            }
        } else if (!strcmp(key, "depth")) {
            cur->depth = atoi(val) >= 24 ? 24 : 16;
        } else if (!strcmp(key, "quality")) {
            int q = atoi(val);
            cur->quality = q < 0 ? 0 : q > 9 ? 9 : q;
        } else if (!strcmp(key, "view")) {
            cur->view_only = atoi(val) != 0;
        } else if (!strcmp(key, "zone")) {
            vnc_rect_t z;
            if (sscanf(val, "%d , %d , %d , %d", &z.x, &z.y, &z.w, &z.h) == 4 && z.x >= 0 && z.y >= 0 &&
                z.w > 0 && z.h > 0) {
                cur->zone = z;
            }
        }
    }
    fclose(f);
    /* a block with no host is not a computer */
    int n = 0;
    for (int i = 0; i < cfg->count; i++) {
        if (!cfg->s[i].host[0]) continue;
        if (n != i) cfg->s[n] = cfg->s[i];
        split_host(&cfg->s[n]);
        if (!cfg->s[n].name[0]) vnc_copy(cfg->s[n].name, sizeof cfg->s[n].name, cfg->s[n].host);
        n++;
    }
    cfg->count = n;
}

bool vnc_cfg_save(const vnc_cfg_t *cfg)
{
    const char *path = vnc_cfg_path();
    mkdir(aos_hal_path_data(), 0777);
    char tmp[176];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        return false;
    }
    fputs("# Computers for the VNC viewer of P4OS (apps/vnc). One block each:\n"
          "# [Name]  host, port (5900), pass, enc (auto tight zrle hextile raw),\n"
          "# depth (16 or 24), quality (0-9), view (1: look only),\n"
          "# zone (x,y,w,h: one monitor of several).\n"
          "# The passwords are kept here in the clear.\n", f);
    /* snprintf + fputs: fprintf reached the apps' table after 0.8.0 */
    char b[256];
    for (int i = 0; i < cfg->count; i++) {
        const vnc_server_t *s = &cfg->s[i];
        snprintf(b, sizeof b, "\n[%s]\nhost = %s\nport = %d\n", s->name, s->host, s->port);
        fputs(b, f);
        if (s->pass[0]) {
            snprintf(b, sizeof b, "pass = %s\n", s->pass);
            fputs(b, f);
        }
        if (s->enc != VNC_ENC_AUTO) {
            snprintf(b, sizeof b, "enc = %s\n", vnc_enc_name(s->enc));
            fputs(b, f);
        }
        if (s->depth != 16) fputs("depth = 24\n", f);
        if (s->quality != 6) {
            snprintf(b, sizeof b, "quality = %d\n", s->quality);
            fputs(b, f);
        }
        if (s->view_only) fputs("view = 1\n", f);
        if (s->zone.w > 0) {
            snprintf(b, sizeof b, "zone = %d,%d,%d,%d\n", s->zone.x, s->zone.y, s->zone.w, s->zone.h);
            fputs(b, f);
        }
    }
    bool ok = fclose(f) == 0;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    return ok;
}
