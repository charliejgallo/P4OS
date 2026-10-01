/*
 * P4OS - Cameras: cameras.txt, and URLs.
 *
 * AmoledOS kept up to eight cameras in NVS, written by its portal's
 * /camaras page: name, URL, user and password per camera. P4OS keeps the
 * same four fields, and the same eight, in a text file at the root of the
 * card, which the portal's Archivos already edits (and lists among its
 * quick "Editar" buttons). One block per camera:
 *
 *     [Timbre]
 *     url  = rtsp://192.168.0.10:554/Streaming/Channels/102
 *     full = rtsp://192.168.0.10:554/Streaming/Channels/101   (optional)
 *     user = admin
 *     pass = secret
 *
 *     [Frigate]                       (optional: the Frigate tab)
 *     url = http://192.168.0.20:5000
 *
 * 'full' is the stream the full screen opens (a camera's main stream),
 * 'url' the one the mosaic keeps open (its sub stream). Credentials may also
 * come inside the URL (rtsp://user:pass@host/...); they are taken out before
 * anything shows the URL. 'refresh' (seconds) is how often a URL that
 * answers with one JPEG and closes is asked again.
 *
 * The file holds passwords in the clear, like ha.txt before it moves the
 * token into the preferences: the card and the portal are the person's own.
 */
#include "cam.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FILE_NAME   "cameras.txt"
#define MAX_BYTES   (16 * 1024)

static const char TEMPLATE[] =
    "# Cameras for P4OS. One block per camera, up to 8:\n"
    "#\n"
    "# [Entrada]\n"
    "# url  = rtsp://192.168.0.10:554/Streaming/Channels/102\n"
    "# full = rtsp://192.168.0.10:554/Streaming/Channels/101\n"
    "# user = admin\n"
    "# pass = secret\n"
    "#\n"
    "# url   rtsp:// for H.264 Baseline or MJPEG over RTP (decoded on the board),\n"
    "#       http:// for MJPEG (go2rtc: http://host:1984/api/stream.mjpeg?src=cam)\n"
    "#       or for a single JPEG, asked again every 'refresh' seconds.\n"
    "# full  optional: what the full screen opens (the main stream).\n"
    "#\n"
    "# [Frigate]\n"
    "# url = http://192.168.0.20:5000\n"
    "#\n"
    "# See docs/CAMERAS.md.\n";

static char s_path[160];

const char *cam_cfg_path(void)
{
    const char *env = getenv("CAM_CONFIG");
    if (env && env[0]) {
        snprintf(s_path, sizeof s_path, "%s", env);
        return s_path;
    }
    const char *root = aos_hal_path_sd_root();
    if (!root) {
        return NULL;
    }
    snprintf(s_path, sizeof s_path, "%s/%s", root, FILE_NAME);
    return s_path;
}

uint32_t cam_cfg_stamp(void)
{
    const char *p = cam_cfg_path();
    struct stat st;
    if (!p || stat(p, &st) != 0) {
        return 0;
    }
    return (uint32_t)st.st_size * 2654435761u ^ (uint32_t)st.st_mtime;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
    return s;
}

static void copy(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

static bool is_frigate(const char *name)
{
    static const char F[] = "frigate";
    for (int i = 0; F[i]; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != F[i]) return false;
    }
    return name[7] == '\0';
}

/* The URL without its credentials, which go to user/pass if those are empty. */
static void take_url(char *dst, size_t cap, const char *url, char *user, char *pass)
{
    cam_url_t u;
    if (!cam_url_parse(url, &u) || !u.user[0]) {
        copy(dst, cap, url);
        return;
    }
    if (!user[0]) {
        copy(user, CAM_CRED_LEN, u.user);
        copy(pass, CAM_CRED_LEN, u.pass);
    }
    char port[12] = "";
    if (u.port != (u.rtsp ? 554 : 80)) {
        snprintf(port, sizeof port, ":%d", u.port);
    }
    if (snprintf(dst, cap, "%s://%s%s%s", u.rtsp ? "rtsp" : "http", u.host, port, u.path) >= (int)cap) {
        dst[0] = '\0';             /* a cut URL would open something else */
    }
}

bool cam_cfg_load(cam_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    const char *path = cam_cfg_path();
    if (!path) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        f = fopen(path, "wb");
        if (f) {
            fwrite(TEMPLATE, 1, sizeof TEMPLATE - 1, f);
            fclose(f);
        }
        return true;
    }
    char line[400];
    cam_t *cur = NULL;
    bool in_frigate = false;
    int bytes = 0;
    while (fgets(line, sizeof line, f) && (bytes += (int)strlen(line)) < MAX_BYTES) {
        char *s = trim(line);
        if (!s[0] || s[0] == '#' || s[0] == ';') {
            continue;
        }
        if (s[0] == '[') {
            char *e = strchr(s, ']');
            if (e) *e = '\0';
            char *name = trim(s + 1);
            in_frigate = is_frigate(name);
            cur = NULL;
            if (!in_frigate && cfg->count < CAM_MAX) {
                cur = &cfg->cams[cfg->count++];
                memset(cur, 0, sizeof(*cur));
                copy(cur->name, sizeof cur->name, name);
                cur->refresh_ms = 1000;
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);
        if (in_frigate) {
            if (!strcmp(key, "url")) take_url(cfg->frigate, sizeof cfg->frigate, val, cfg->frigate_user, cfg->frigate_pass);
            else if (!strcmp(key, "user")) copy(cfg->frigate_user, sizeof cfg->frigate_user, val);
            else if (!strcmp(key, "pass")) copy(cfg->frigate_pass, sizeof cfg->frigate_pass, val);
            continue;
        }
        if (!cur) {
            continue;
        }
        if (!strcmp(key, "url")) take_url(cur->url, sizeof cur->url, val, cur->user, cur->pass);
        else if (!strcmp(key, "full")) take_url(cur->url_full, sizeof cur->url_full, val, cur->user, cur->pass);
        else if (!strcmp(key, "user")) copy(cur->user, sizeof cur->user, val);
        else if (!strcmp(key, "pass")) copy(cur->pass, sizeof cur->pass, val);
        else if (!strcmp(key, "refresh")) cur->refresh_ms = (int)(strtof(val, NULL) * 1000.0f);
    }
    fclose(f);
    /* A block with no URL is not a camera. */
    int n = 0;
    for (int i = 0; i < cfg->count; i++) {
        if (cfg->cams[i].url[0]) {
            if (n != i) cfg->cams[n] = cfg->cams[i];
            n++;
        }
    }
    cfg->count = n;
    return true;
}

bool cam_url_parse(const char *url, cam_url_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *p;
    if (strncmp(url, "rtsp://", 7) == 0) {
        out->rtsp = true;
        out->port = 554;
        p = url + 7;
    } else if (strncmp(url, "http://", 7) == 0) {
        out->port = 80;
        p = url + 7;
    } else {
        return false;
    }
    const char *slash = strchr(p, '/');
    const char *at = strchr(p, '@');
    if (at && (!slash || at < slash)) {
        /* user[:pass]@ */
        const char *colon = memchr(p, ':', (size_t)(at - p));
        const char *ue = colon ? colon : at;
        size_t un = (size_t)(ue - p);
        if (un < sizeof out->user) {
            memcpy(out->user, p, un);
            out->user[un] = '\0';
        }
        if (colon) {
            size_t pn = (size_t)(at - colon - 1);
            if (pn < sizeof out->pass) {
                memcpy(out->pass, colon + 1, pn);
                out->pass[pn] = '\0';
            }
        }
        p = at + 1;
    }
    const char *host_end = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(host_end - p));
    const char *name_end = colon ? colon : host_end;
    if (name_end == p || (size_t)(name_end - p) >= sizeof(out->host)) {
        return false;
    }
    memcpy(out->host, p, (size_t)(name_end - p));
    if (colon) {
        out->port = atoi(colon + 1);
        if (out->port <= 0 || out->port > 65535) {
            return false;
        }
    }
    snprintf(out->path, sizeof(out->path), "%s", slash ? slash : "/");
    return true;
}

/* ---- base64 --------------------------------------------------------------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(char c)
{
    const char *p = strchr(B64, c);
    return (c && p) ? (int)(p - B64) : -1;
}

int cam_b64_decode(const char *in, int in_len, uint8_t *out, int out_max)
{
    int n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (int i = 0; i < in_len; i++) {
        int v = b64_val(in[i]);
        if (v < 0) {
            if (in[i] == '=') {
                break;
            }
            continue;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_max) {
                return -1;
            }
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

int cam_b64_encode(const uint8_t *in, int in_len, char *out, int out_max)
{
    int n = 0;
    for (int i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len) v |= in[i + 2];
        if (n + 4 >= out_max) {
            return -1;
        }
        out[n++] = B64[(v >> 18) & 63];
        out[n++] = B64[(v >> 12) & 63];
        out[n++] = i + 1 < in_len ? B64[(v >> 6) & 63] : '=';
        out[n++] = i + 2 < in_len ? B64[v & 63] : '=';
    }
    out[n] = '\0';
    return n;
}
