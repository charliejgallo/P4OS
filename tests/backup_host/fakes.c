/* What aos_backup.c asks of the HAL, on the Mac: the threads, the clock and
 * the log of the downloader's tests (tests/fetch_host/fakes.c, taken whole),
 * and here a card that is a folder and preferences that are a file. */
#define aos_hal_firmware_version fetch_host_version
#include "../fetch_host/fakes.c"
#undef aos_hal_firmware_version

#include <string.h>

/* FAKE_FW=v0.31.0: the board's firmware */
const char *aos_hal_firmware_version(void)
{
    const char *v = getenv("FAKE_FW");
    return v ? v : "v0.30.0";
}
const char *aos_hal_device_name(void) { return "placa de prueba"; }

/* P4_CARD=<folder>: the card's root */
const char *aos_hal_path_sd_root(void) { return getenv("P4_CARD"); }

/* The board's preferences: a file (P4_PREFS) with a line each, "i key
 * value" or "s key value"; a value's line breaks are kept as \x01. The
 * whole file is read and written each time: a key is there once. */
#define PREFS_MAX 512
typedef struct { char type; char key[40]; char *val; } pref_t;
static pref_t s_p[PREFS_MAX];
static int s_np;

static void prefs_load(void)
{
    for (int i = 0; i < s_np; i++) free(s_p[i].val);
    s_np = 0;
    const char *path = getenv("P4_PREFS");
    FILE *f = path ? fopen(path, "r") : NULL;
    static char line[16384];
    while (f && fgets(line, sizeof line, f) && s_np < PREFS_MAX) {
        line[strcspn(line, "\n")] = 0;
        char *k = line + 2, *v = strchr(k, ' ');
        if ((line[0] != 'i' && line[0] != 's') || line[1] != ' ' || !v) continue;
        *v++ = 0;
        for (char *c = v; *c; c++) if (*c == 1) *c = '\n';
        s_p[s_np].type = line[0];
        snprintf(s_p[s_np].key, sizeof s_p[0].key, "%s", k);
        s_p[s_np].val = strdup(v);
        s_np++;
    }
    if (f) fclose(f);
}

static bool prefs_save(void)
{
    const char *path = getenv("P4_PREFS");
    FILE *f = path ? fopen(path, "w") : NULL;
    if (!f) return false;
    for (int i = 0; i < s_np; i++) {
        fprintf(f, "%c %s ", s_p[i].type, s_p[i].key);
        for (const char *c = s_p[i].val; *c; c++) fputc(*c == '\n' ? 1 : *c, f);
        fputc('\n', f);
    }
    fclose(f);
    return true;
}

static bool pref_put(char type, const char *key, const char *val)
{
    prefs_load();
    int at = -1;
    for (int i = 0; i < s_np; i++) if (!strcmp(s_p[i].key, key)) at = i;
    if (at < 0) {
        if (s_np >= PREFS_MAX) return false;
        at = s_np++;
        snprintf(s_p[at].key, sizeof s_p[0].key, "%s", key);
        s_p[at].val = NULL;
    }
    free(s_p[at].val);
    s_p[at].type = type;
    s_p[at].val = strdup(val);
    return prefs_save();
}

bool aos_hal_pref_set_str(const char *key, const char *value) { return pref_put('s', key, value); }
bool aos_hal_pref_set_i32(const char *key, int32_t value)
{
    char v[32];
    snprintf(v, sizeof v, "%d", (int)value);
    return pref_put('i', key, v);
}
bool aos_hal_pref_get_str(const char *key, char *out, size_t out_len)
{
    prefs_load();
    for (int i = 0; i < s_np; i++)
        if (!strcmp(s_p[i].key, key) && s_p[i].type == 's') { snprintf(out, out_len, "%s", s_p[i].val); return true; }
    return false;
}
bool aos_hal_pref_get_i32(const char *key, int32_t *out)
{
    prefs_load();
    for (int i = 0; i < s_np; i++)
        if (!strcmp(s_p[i].key, key) && s_p[i].type == 'i') { *out = (int32_t)atoi(s_p[i].val); return true; }
    return false;
}
int aos_hal_pref_foreach(aos_hal_pref_visit_t visit, void *ctx)
{
    prefs_load();
    for (int i = 0; i < s_np; i++) visit(s_p[i].key, s_p[i].type == 's', (int32_t)atoi(s_p[i].val), s_p[i].val, ctx);
    return s_np;
}

bool aos_hal_pref_erase(const char *key)
{
    prefs_load();
    for (int i = 0; i < s_np; i++)
        if (!strcmp(s_p[i].key, key)) {
            free(s_p[i].val);
            s_p[i] = s_p[--s_np];
            return prefs_save();
        }
    return false;
}

/* FAKE_SSID=casa: the board knows a network */
const char *aos_hal_net_ssid(void)
{
    const char *v = getenv("FAKE_SSID");
    return v ? v : "";
}

/* P4_USB=u1: a pendrive, mounted at that folder (a short name: the HAL's
 * path is "/usb") */
bool aos_hal_usb_host_info(aos_usb_host_info_t *out)
{
    const char *v = getenv("P4_USB");
    memset(out, 0, sizeof *out);
    if (!v) return true;
    out->device = out->mounted = true;
    snprintf(out->path, sizeof out->path, "%s", v);
    return true;
}

/* ---- a server, for the copy that leaves the board (components/aos_ssh/aos_sshc.c
 * is tried on its own, against the simulator, in tests/sshc_host) ----
 *
 * P4_SRV=<folder>: the server's home; without it nobody answers.
 * P4_SRV_KEY=<text>: the key it shows.   P4_SRV_DENY=1: it does not let the board's key in. */
#include "aos_sshc.h"
#include <sys/stat.h>

struct aos_sshc { char root[200]; char key[200]; char why[100]; };

aos_net_state_t aos_hal_net_state(void) { return AOS_NET_CONNECTED; }

static const char *srv_key(void) { const char *k = getenv("P4_SRV_KEY"); return k ? k : "QUFBQQ=="; }

aos_sshc_t *aos_sshc_open(const aos_sshc_opts_t *o, int *err, char *why, size_t why_len, char *seen)
{
    const char *root = getenv("P4_SRV");
    if (seen) seen[0] = 0;
    if (!root || strcmp(o->host, "nas.example")) { *err = AOS_SSHC_E_CONNECT; snprintf(why, why_len, "%s does not answer", o->host); return NULL; }
    if (seen) snprintf(seen, 600, "%s", srv_key());
    if (o->hostkey && o->hostkey[0] && strcmp(o->hostkey, srv_key())) { *err = AOS_SSHC_E_HOSTKEY; snprintf(why, why_len, "the server's key is not the one that was kept"); return NULL; }
    if (getenv("P4_SRV_DENY")) { *err = AOS_SSHC_E_AUTH; snprintf(why, why_len, "the server does not let the board's key in"); return NULL; }
    aos_sshc_t *c = calloc(1, sizeof *c);
    snprintf(c->root, sizeof c->root, "%s", root);
    snprintf(c->key, sizeof c->key, "%s", srv_key());
    *err = AOS_SSHC_OK;
    return c;
}
const char *aos_sshc_hostkey(const aos_sshc_t *c) { return c->key; }
const char *aos_sshc_fingerprint(const aos_sshc_t *c) { (void)c; return "ED25519 SHA256:fake"; }
bool aos_sshc_fingerprint_of(const char *hostkey, char *out, size_t n)
{
    if (!hostkey || !hostkey[0]) return false;
    snprintf(out, n, "ED25519 SHA256:%.20s", hostkey);
    return true;
}
void aos_sshc_close(aos_sshc_t *c) { free(c); }
const char *aos_sshc_why(const aos_sshc_t *c) { return c->why; }
int aos_sshc_sftp(aos_sshc_t *c) { (void)c; return AOS_SSHC_OK; }
static void srv_path(aos_sshc_t *c, const char *p, char out[600]) { snprintf(out, 600, "%s/%s", c->root, p); }
int aos_sshc_mkdirs(aos_sshc_t *c, const char *path)
{
    char f[600];
    srv_path(c, path, f);
    for (char *p = f + strlen(c->root) + 1; ; p++)
        if (*p == '/' || !*p) { char k = *p; *p = 0; mkdir(f, 0775); *p = k; if (!k) break; }
    return AOS_SSHC_OK;
}
int aos_sshc_stat(aos_sshc_t *c, const char *path, bool *there, uint64_t *size)
{
    char f[600];
    struct stat st;
    srv_path(c, path, f);
    *there = stat(f, &st) == 0;
    if (size) *size = *there ? (uint64_t)st.st_size : 0;
    return AOS_SSHC_OK;
}
int aos_sshc_put(aos_sshc_t *c, const char *local, const char *remote)
{
    char f[600], buf[4096];
    srv_path(c, remote, f);
    FILE *in = fopen(local, "rb"), *out = in ? fopen(f, "wb") : NULL;
    size_t got;
    if (!out) { if (in) fclose(in); snprintf(c->why, sizeof c->why, "the server would not take it"); return in ? AOS_SSHC_E_REMOTE : AOS_SSHC_E_LOCAL; }
    while ((got = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, got, out);
    fclose(in); fclose(out);
    return AOS_SSHC_OK;
}
int aos_sshc_list(aos_sshc_t *c, const char *path, aos_sshc_name_t each, void *ctx) { (void)c; (void)path; (void)each; (void)ctx; return AOS_SSHC_OK; }
int aos_sshc_remove(aos_sshc_t *c, const char *path) { char f[600]; srv_path(c, path, f); remove(f); return AOS_SSHC_OK; }
bool aos_sshc_key_line(const uint8_t seed[32], const char *comment, char *out, size_t n)
{
    snprintf(out, n, "ssh-ed25519 FAKE%02x%02x %s", seed[0], seed[1], comment);
    return true;
}
