/* What aos_store.c asks of the HAL beyond the downloader's needs
 * (tests/fetch_host/fakes.c, taken whole). */
#define aos_hal_firmware_version fetch_host_version
#include "../fetch_host/fakes.c"
#undef aos_hal_firmware_version

#include <string.h>

/* FAKE_FW=v0.29.0-3-gabc: the board's firmware */
const char *aos_hal_firmware_version(void)
{
    const char *v = getenv("FAKE_FW");
    return v ? v : "v0.29.0";
}

/* P4_CARD=<folder>: the card's root */
const char *aos_hal_path_sd_root(void) { return getenv("P4_CARD"); }

/* The board's preferences: a file beside the card (P4_PREFS), a "key=value"
 * line each, so they outlive one run of the test's program as the board's
 * outlive a restart. A value's line breaks are kept as \x01. */
static bool pref_get(const char *key, char *out, size_t n)
{
    const char *path = getenv("P4_PREFS");
    FILE *f = path ? fopen(path, "r") : NULL;
    char line[2048];
    bool found = false;
    size_t kl = strlen(key);
    while (f && fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) || line[kl] != '=') continue;
        char *v = line + kl + 1;
        v[strcspn(v, "\n")] = 0;
        for (char *c = v; *c; c++) if (*c == 1) *c = '\n';
        snprintf(out, n, "%s", v);
        found = true;
    }
    if (f) fclose(f);
    return found;
}
static bool pref_set(const char *key, const char *value)
{
    const char *path = getenv("P4_PREFS");
    FILE *f = path ? fopen(path, "a") : NULL;
    if (!f) return false;
    fprintf(f, "%s=", key);
    for (const char *c = value; *c; c++) fputc(*c == '\n' ? 1 : *c, f);
    fputc('\n', f);
    fclose(f);
    return true;
}
bool aos_hal_pref_get_str(const char *key, char *out, size_t out_len) { return pref_get(key, out, out_len) && out[0]; }
bool aos_hal_pref_set_str(const char *key, const char *value) { return pref_set(key, value); }
bool aos_hal_pref_get_i32(const char *key, int32_t *out)
{
    char v[32];
    if (!pref_get(key, v, sizeof v)) return false;
    *out = (int32_t)atoi(v);
    return true;
}
bool aos_hal_pref_set_i32(const char *key, int32_t value)
{
    char v[32];
    snprintf(v, sizeof v, "%d", (int)value);
    return pref_set(key, v);
}
