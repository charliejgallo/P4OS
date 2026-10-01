/*
 * P4OS - the core dump a panic leaves in its flash partition, for the portal
 * (aos_hal.h, "Core dump"). Every access goes through aos_flash_call(): the
 * portal's threads have their stacks in PSRAM, and the flash driver asserts
 * there (docs/MEMORY.md). The dump is an ELF: tools/coredump.sh downloads it
 * and has idf.py decode it against the firmware's ELF on the Mac.
 */
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_log.h"

void aos_flash_call(void (*fn)(void *ctx), void *ctx);

typedef struct {
    int     op;                 /* 0 info, 1 read, 2 erase */
    aos_coredump_info_t *info;
    size_t  off, len;
    void   *buf;
    bool    ok;
} cd_job_t;

static void cd_do(void *ctx)
{
    cd_job_t *j = ctx;
    size_t addr = 0, size = 0;
    j->ok = false;
    if (j->op == 2) {
        j->ok = esp_core_dump_image_erase() == ESP_OK;
        return;
    }
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || !size) return;
    if (j->op == 0) {
        aos_coredump_info_t *o = j->info;
        o->present = true;
        o->size = size;
        o->valid = esp_core_dump_image_check() == ESP_OK;
        esp_core_dump_summary_t *sm = calloc(1, sizeof *sm);
        esp_err_t se = sm ? esp_core_dump_get_summary(sm) : ESP_ERR_NO_MEM;
        if (se != ESP_OK) ESP_LOGW("coredump", "no summary: %s", esp_err_to_name(se));
        if (se == ESP_OK) {
            snprintf(o->task, sizeof o->task, "%.15s", sm->exc_task);
            o->pc = sm->exc_pc;
            o->ra = sm->ex_info.ra;
            o->sp = sm->ex_info.sp;
            o->mcause = sm->ex_info.mcause;
            o->mtval = sm->ex_info.mtval;
            snprintf(o->elf_sha, sizeof o->elf_sha, "%.16s", (const char *)sm->app_elf_sha256);
        }
        free(sm);
        j->ok = true;
        return;
    }
    if (j->off >= size) { j->len = 0; j->ok = true; return; }
    if (j->len > size - j->off) j->len = size - j->off;
    j->ok = esp_flash_read(NULL, j->buf, addr + j->off, j->len) == ESP_OK;
}

bool aos_hal_coredump_info(aos_coredump_info_t *out)
{
    memset(out, 0, sizeof *out);
    cd_job_t j = { .op = 0, .info = out };
    aos_flash_call(cd_do, &j);
    return j.ok;
}

size_t aos_hal_coredump_read(size_t off, void *buf, size_t len)
{
    cd_job_t j = { .op = 1, .off = off, .len = len, .buf = buf };
    aos_flash_call(cd_do, &j);
    return j.ok ? j.len : 0;
}

bool aos_hal_coredump_erase(void)
{
    cd_job_t j = { .op = 2 };
    aos_flash_call(cd_do, &j);
    if (j.ok) ESP_LOGW("coredump", "erased");
    return j.ok;
}
