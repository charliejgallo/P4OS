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

/* The other slot: a file beside the card, <P4_CARD>.slot, written as the
 * image arrives and kept only when it was closed. FAKE_OTA=full makes the
 * slot refuse the image, as one too small would. */
static FILE *s_slot;
static char  s_ota_err[80];
static void slot_path(char *out, size_t n, const char *tail) { snprintf(out, n, "%s.slot%s", getenv("P4_CARD"), tail); }
bool aos_hal_ota_begin(size_t total_bytes)
{
    char p[512];
    const char *how = getenv("FAKE_OTA");
    s_ota_err[0] = 0;
    if (how && !strcmp(how, "full")) { snprintf(s_ota_err, sizeof s_ota_err, "the image (%u B) does not fit the slot", (unsigned)total_bytes); return false; }
    slot_path(p, sizeof p, ".part");
    s_slot = fopen(p, "wb");
    return s_slot != NULL;
}
bool aos_hal_ota_write(const void *data, size_t len) { return s_slot && fwrite(data, 1, len, s_slot) == len; }
bool aos_hal_ota_end(void)
{
    char a[512], b[512];
    if (!s_slot) return false;
    fclose(s_slot);
    s_slot = NULL;
    slot_path(a, sizeof a, ".part");
    slot_path(b, sizeof b, "");
    return rename(a, b) == 0;
}
void aos_hal_ota_abort(void)
{
    char a[512];
    if (s_slot) fclose(s_slot);
    s_slot = NULL;
    slot_path(a, sizeof a, ".part");
    remove(a);
}
const char *aos_hal_ota_error(void) { return s_ota_err; }

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

/* The copy of the preferences an update asks for first (components/aos_backup):
 * counted here, made in tests/backup_host. */
int g_backups_before_fw;
void aos_backup_before_firmware(void) { g_backups_before_fw++; }
