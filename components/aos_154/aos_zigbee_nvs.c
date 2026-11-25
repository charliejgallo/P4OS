/*
 * P4OS - the stack's NVS while the board emulates a device
 * (docs/plan/ZIGBEE-EMULAR.md, "Un solo NVS, dos redes").
 *
 * esp-zigbee-lib keeps its network in one NVS namespace, "zigbee", opened
 * with nvs_open_from_partition (esp_zigbee_plat_datasets.c, its only NVS
 * open). Joining Home Assistant's network there would overwrite the
 * board's own; while emulating, the same calls open "zbemu" instead, and
 * a factory reset of the stack there (leaving that network) erases only
 * it. Neither namespace is in the backups (they walk "p4os" alone): the
 * other network's key stays on the board, not in a backup. Every other
 * caller of nvs_open_from_partition passes through untouched.
 */
#include <stdbool.h>
#include <string.h>

#include "nvs.h"

volatile bool g_aos_zb_emu_nvs;         /* set before the stack starts (aos_zigbee_p4.c) */

esp_err_t __real_nvs_open_from_partition(const char *part, const char *ns, nvs_open_mode_t mode, nvs_handle_t *h);

esp_err_t __wrap_nvs_open_from_partition(const char *part, const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    if (g_aos_zb_emu_nvs && ns && !strcmp(ns, "zigbee")) ns = "zbemu";
    return __real_nvs_open_from_partition(part, ns, mode, h);
}
