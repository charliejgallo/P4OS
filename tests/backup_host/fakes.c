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
