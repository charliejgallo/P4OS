/*
 * P4OS - firmware update over the network (aos_hal.h, "Firmware update").
 *
 * The partition table has two 8 MB app slots and an otadata; this writes an
 * image into the idle one as the portal streams it in, and points the
 * bootloader at it. Two things the P4 needs that the watch did not:
 *
 *  - Every flash operation goes through aos_flash_call(): the portal's
 *    threads have their stacks in PSRAM, and the flash driver asserts when
 *    the caller's stack is there (docs/MEMORY.md). The data may stay in
 *    PSRAM (esp_flash_write bounces it).
 *  - The image boots on trial (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): until
 *    main.c confirms it, a restart - a crash on boot included - brings the
 *    previous one back. main.c confirms it once the system has been up for
 *    30 s (see there).
 *
 * The music stops during the upload, as on the watch: the decoder starving a
 * core while the flash is written is what cut uploads short there.
 */
#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_flash.h"

static const char *TAG = "ota";

void aos_flash_call(void (*fn)(void *ctx), void *ctx);

static esp_ota_handle_t s_ota;
static const esp_partition_t *s_part;
static char s_err[96];
static size_t s_written;

typedef struct {
    int         op;             /* 0 begin, 1 write, 2 end, 3 abort, 4 mark valid, 5 boot other, 6 states */
    size_t      size;
    const void *data;
    esp_err_t   err;
    const char *what;
    aos_ota_info_t *info;       /* op 6 */
} ota_job_t;

static void ota_do(void *ctx)
{
    ota_job_t *j = ctx;
    switch (j->op) {
    case 0:
        j->what = "esp_ota_get_next_update_partition";
        s_part = esp_ota_get_next_update_partition(NULL);
        if (!s_part) { j->err = ESP_ERR_NOT_FOUND; break; }
        if (j->size > s_part->size) { j->what = "la imagen no entra en la ranura"; j->err = ESP_ERR_INVALID_SIZE; break; }
        j->what = "esp_ota_begin";
        /* with the size known only what it needs is erased (a whole 8 MB
         * slot takes several seconds) */
        j->err = esp_ota_begin(s_part, j->size ? j->size : OTA_SIZE_UNKNOWN, &s_ota);
        break;
    case 1:
        j->what = "esp_ota_write";
        j->err = esp_ota_write(s_ota, j->data, j->size);
        break;
    case 2:
        j->what = "esp_ota_end";
        j->err = esp_ota_end(s_ota);
        if (j->err == ESP_OK) {
            j->what = "esp_ota_set_boot_partition";
            j->err = esp_ota_set_boot_partition(s_part);
        }
        break;
    case 3:
        j->what = "esp_ota_abort";
        j->err = esp_ota_abort(s_ota);
        break;
    case 4:
        j->what = "esp_ota_mark_app_valid_cancel_rollback";
        j->err = esp_ota_mark_app_valid_cancel_rollback();
        break;
    case 6: {
        /* reading a slot's state or its image header reads the flash, too */
        aos_ota_info_t *o = j->info;
        j->what = "esp_ota_get_state_partition";
        const esp_partition_t *run_p = esp_ota_get_running_partition();
        esp_ota_img_states_t st;
        o->trial = run_p && esp_ota_get_state_partition(run_p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
        if (run_p) snprintf(o->slot, sizeof o->slot, "%s", run_p->label);
        const esp_app_desc_t *me = esp_app_get_description();
        snprintf(o->built, sizeof o->built, "%s %s", me->date, me->time);
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        if (other) {
            snprintf(o->other_slot, sizeof o->other_slot, "%s", other->label);
            o->other_size = other->size;
            esp_app_desc_t d;
            if (esp_ota_get_partition_description(other, &d) == ESP_OK) {
                snprintf(o->other_version, sizeof o->other_version, "%s", d.version);
                snprintf(o->other_built, sizeof o->other_built, "%s %s", d.date, d.time);
                for (int i = 0; i < 8; i++) snprintf(o->other_elf + 2 * i, 3, "%02x", d.app_elf_sha256[i]);
            }
            if (esp_ota_get_state_partition(other, &st) == ESP_OK)
                snprintf(o->other_state, sizeof o->other_state, "%s",
                         st == ESP_OTA_IMG_VALID ? "valid" : st == ESP_OTA_IMG_INVALID ? "invalid"
                         : st == ESP_OTA_IMG_ABORTED ? "aborted" : st == ESP_OTA_IMG_NEW ? "new"
                         : st == ESP_OTA_IMG_PENDING_VERIFY ? "pending" : "undefined");
        }
        esp_flash_read_id(NULL, &o->flash_id);
        j->err = ESP_OK;
        break;
    }
    case 5: {
        j->what = "esp_ota_set_boot_partition";
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_app_desc_t d;
        j->err = !other ? ESP_ERR_NOT_FOUND : esp_ota_get_partition_description(other, &d);
        if (j->err == ESP_OK) j->err = esp_ota_set_boot_partition(other);
        break;
    }
    }
}

static bool run(int op, const void *data, size_t size)
{
    ota_job_t j = { .op = op, .data = data, .size = size };
    aos_flash_call(ota_do, &j);
    if (j.err != ESP_OK) {
        snprintf(s_err, sizeof s_err, "%s: %s", j.what, esp_err_to_name(j.err));
        ESP_LOGE(TAG, "%s", s_err);
        return false;
    }
    return true;
}

bool aos_hal_ota_begin(size_t total_bytes)
{
    if (s_ota) aos_hal_ota_abort();         /* an interrupted one left the slot open */
    s_err[0] = 0;
    s_written = 0;
    aos_hal_player_stop();
    s_part = NULL;
    if (!run(0, NULL, total_bytes)) {
        s_ota = 0;
        return false;
    }
    ESP_LOGI(TAG, "writing into %s (%u KB), image of %u B", s_part->label, (unsigned)(s_part->size / 1024),
             (unsigned)total_bytes);
    return true;
}

bool aos_hal_ota_write(const void *data, size_t len)
{
    if (!s_ota) return false;
    if (!run(1, data, len)) {
        run(3, NULL, 0);
        s_ota = 0;
        return false;
    }
    s_written += len;
    return true;
}

bool aos_hal_ota_end(void)
{
    if (!s_ota) return false;
    bool ok = run(2, NULL, 0);
    s_ota = 0;
    if (ok) ESP_LOGW(TAG, "%s is the boot partition now (%u B); it starts on trial", s_part->label,
                     (unsigned)s_written);
    return ok;
}

void aos_hal_ota_abort(void)
{
    if (!s_ota) return;
    run(3, NULL, 0);
    s_ota = 0;
    ESP_LOGW(TAG, "upload given up after %u B: the running image stays", (unsigned)s_written);
}

const char *aos_hal_ota_error(void) { return s_err; }

bool aos_hal_ota_info(aos_ota_info_t *out)
{
    memset(out, 0, sizeof *out);
    ota_job_t j = { .op = 6, .info = out };
    aos_flash_call(ota_do, &j);
    return j.err == ESP_OK;
}

bool aos_hal_ota_pending_verify(void)
{
    aos_ota_info_t o;
    return aos_hal_ota_info(&o) && o.trial;
}

void aos_hal_ota_mark_valid(void)
{
    if (!aos_hal_ota_pending_verify()) return;
    if (run(4, NULL, 0)) ESP_LOGW(TAG, "this image is confirmed: no rollback now");
}

const char *aos_hal_ota_running_slot(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

bool aos_hal_ota_boot_other(void)
{
    if (!run(5, NULL, 0)) return false;
    ESP_LOGW(TAG, "the other slot boots at the next restart");
    return true;
}
