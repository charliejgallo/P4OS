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

static char s_url[256];
bool aos_hal_pref_get_str(const char *key, char *out, size_t out_len)
{
    if (strcmp(key, "store_url") || !s_url[0]) return false;
    snprintf(out, out_len, "%s", s_url);
    return true;
}
bool aos_hal_pref_set_str(const char *key, const char *value)
{
    if (!strcmp(key, "store_url")) snprintf(s_url, sizeof s_url, "%s", value);
    return true;
}
