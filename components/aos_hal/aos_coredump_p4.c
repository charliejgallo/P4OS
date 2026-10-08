/*
 * P4OS - the core dump a panic leaves in its flash partition, for the portal
 * (aos_hal.h, "Core dump"). Every access goes through aos_flash_call(): the
 * portal's threads have their stacks in PSRAM, and the flash driver asserts
 * there (docs/MEMORY.md). The dump is an ELF: tools/coredump.sh downloads it
 * and decodes it against the ELF of the firmware that made it.
 *
 * Which firmware made it matters as much as what it says: a dump stays in
 * flash through any number of restarts until it is erased, and read against
 * today's ELF an old one looks like a new bug with the wrong lines (it
 * happened with the BLE scanner's panic of 0.12's first OTA). The firmware's
 * ELF sha is in the dump's own notes. esp_core_dump_get_summary() would give
 * it, but it maps the whole partition with esp_partition_mmap(), and on this
 * board that fails (it is tried first, and the error logged); so the notes,
 * the crashed task's TCB and the exception frame on its stack are read here
 * with esp_flash_read(), the way the summary walks them.
 *
 * At boot a dump not seen before is noted in the preferences as unread, with
 * the time it was first seen (filled in once the clock is set): the log and
 * the portal say there is one until it is downloaded or erased.
 */
#include "aos_hal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv/rvruntime-frames.h"

static const char *TAG = "coredump";

void aos_flash_call(void (*fn)(void *ctx), void *ctx);

/* the dump's layout (components/espcoredump, ELF format) */
#define CD_HDR_BYTES        24          /* core_dump_header_t: six uint32 */
#define NOTE_INFO_TYPE      8266        /* version + app ELF sha */
#define NOTE_EXTRA_TYPE     677         /* RISC-V: the crashed task's TCB */
#define PT_LOAD_T           1
#define PT_NOTE_T           4
#define MAX_PHDRS           160
#define MAX_NOTES_BYTES     4096

typedef struct {
    int     op;                 /* 0 info, 1 read, 2 erase */
    aos_coredump_info_t *info;
    size_t  off, len;
    void   *buf;
    bool    ok;
} cd_job_t;

static bool rd(size_t at, void *dst, size_t n)
{
    return esp_flash_read(NULL, dst, at, n) == ESP_OK;
}

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* What esp_core_dump_get_summary() gives, read from flash. */
static void parse(size_t addr, size_t size, aos_coredump_info_t *o)
{
    size_t elf = addr + CD_HDR_BYTES;
    uint8_t eh[52];
    if (size < CD_HDR_BYTES + sizeof eh || !rd(elf, eh, sizeof eh) || memcmp(eh, "\x7f" "ELF", 4)) return;
    uint32_t phoff = u32(eh + 0x1C);
    uint16_t phentsize = u16(eh + 0x2A), phnum = u16(eh + 0x2C);
    if (phentsize < 32 || phnum == 0 || phnum > MAX_PHDRS) return;
    uint8_t *ph = malloc((size_t)phnum * phentsize);
    uint8_t *notes = malloc(MAX_NOTES_BYTES);
    if (!ph || !notes || !rd(elf + phoff, ph, (size_t)phnum * phentsize)) goto out;

    uint32_t tcb = 0;
    for (int i = 0; i < phnum; i++) {
        const uint8_t *p = ph + (size_t)i * phentsize;
        if (u32(p) != PT_NOTE_T) continue;
        uint32_t off = u32(p + 4), sz = u32(p + 16);
        if (sz > MAX_NOTES_BYTES) sz = MAX_NOTES_BYTES;
        if (!rd(elf + off, notes, sz)) continue;
        for (uint32_t at = 0; at + 12 <= sz;) {
            uint32_t namesz = u32(notes + at), descsz = u32(notes + at + 4), type = u32(notes + at + 8);
            uint32_t desc = at + 12 + ((namesz + 3) & ~3u);
            if (desc + descsz > sz) break;
            if (type == NOTE_INFO_TYPE && descsz >= 4 + 16) {
                /* uint32 version, then the sha as a hex string */
                snprintf(o->elf_sha, sizeof o->elf_sha, "%.16s", (const char *)notes + desc + 4);
                for (char *c = o->elf_sha; *c; c++) {
                    if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) { *c = 0; break; }
                }
            } else if (type == NOTE_EXTRA_TYPE && descsz >= 4) {
                tcb = u32(notes + desc);
            }
            at = desc + ((descsz + 3) & ~3u);
        }
    }
    /* the crashed task's TCB segment, then its stack, which starts with the
     * exception frame */
    for (int i = 0, found = 0; tcb && i < phnum; i++) {
        const uint8_t *p = ph + (size_t)i * phentsize;
        if (u32(p) != PT_LOAD_T) continue;
        uint32_t off = u32(p + 4), vaddr = u32(p + 8);
        if (found) {
            RvExcFrame f;
            if (rd(elf + off, &f, sizeof f)) {
                o->pc = f.mepc;
                o->ra = f.ra;
                o->sp = f.sp;
                o->mcause = f.mcause;
                o->mtval = f.mtval;
            }
            break;
        }
        if (vaddr == tcb) {
            char name[configMAX_TASK_NAME_LEN + 1] = { 0 };
            if (rd(elf + off + offsetof(StaticTask_t, ucDummy7), name, configMAX_TASK_NAME_LEN)) {
                snprintf(o->task, sizeof o->task, "%.15s", name);
            }
            found = 1;
        }
    }
out:
    free(ph);
    free(notes);
}

/* Which firmware the sha belongs to: this one, the other slot's, or neither. */
static void which_firmware(aos_coredump_info_t *o)
{
    /* as long as the dump has it: 9 characters from firmware built before
     * CONFIG_APP_RETRIEVE_LEN_ELF_SHA was raised to 16 */
    size_t n = strlen(o->elf_sha);
    if (n < 8) return;
    char sha[17];
    esp_app_get_elf_sha256(sha, sizeof sha);
    if (!strncmp(sha, o->elf_sha, n)) {
        snprintf(o->slot, sizeof o->slot, "running");
        snprintf(o->version, sizeof o->version, "%s", esp_app_get_description()->version);
        return;
    }
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d;
    if (other && esp_ota_get_partition_description(other, &d) == ESP_OK) {
        char osha[17];
        for (int i = 0; i < 8; i++) snprintf(osha + 2 * i, 3, "%02x", d.app_elf_sha256[i]);
        if (!strncmp(osha, o->elf_sha, n)) {
            snprintf(o->slot, sizeof o->slot, "other");
            snprintf(o->version, sizeof o->version, "%s", d.version);
        }
    }
}

static void cd_do(void *ctx)
{
    cd_job_t *j = ctx;
    size_t addr = 0, size = 0;
    j->ok = false;
    if (j->op == 2) {
        j->ok = esp_core_dump_image_erase() == ESP_OK;
        return;
    }
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || !size) {
        j->ok = j->op == 0;     /* none: info says so */
        return;
    }
    if (j->op == 0) {
        aos_coredump_info_t *o = j->info;
        o->present = true;
        o->size = size;
        o->valid = esp_core_dump_image_check() == ESP_OK;
        /* IDF's summary first; it maps the partition, which fails here */
        static bool said;
        esp_core_dump_summary_t *sm = calloc(1, sizeof *sm);
        esp_err_t se = sm ? esp_core_dump_get_summary(sm) : ESP_ERR_NO_MEM;
        if (se == ESP_OK) {
            snprintf(o->task, sizeof o->task, "%.15s", sm->exc_task);
            o->pc = sm->exc_pc;
            o->ra = sm->ex_info.ra;
            o->sp = sm->ex_info.sp;
            o->mcause = sm->ex_info.mcause;
            o->mtval = sm->ex_info.mtval;
            snprintf(o->elf_sha, sizeof o->elf_sha, "%.16s", (const char *)sm->app_elf_sha256);
        } else {
            if (!said) {
                /* ESP_FAIL says only that the map failed: say why */
                const esp_partition_t *cp = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                     ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
                const void *m = NULL;
                esp_partition_mmap_handle_t mh;
                esp_err_t me = cp ? esp_partition_mmap(cp, 0, cp->size, ESP_PARTITION_MMAP_DATA, &m, &mh) : ESP_ERR_NOT_FOUND;
                if (me == ESP_OK) esp_partition_munmap(mh);
                ESP_LOGW(TAG, "IDF's summary: %s (mapping the %u KB partition: %s); reading the dump's notes instead",
                         esp_err_to_name(se), cp ? (unsigned)(cp->size / 1024) : 0, esp_err_to_name(me));
            }
            said = true;
            parse(addr, size, o);
        }
        free(sm);
        which_firmware(o);
        j->ok = true;
        return;
    }
    if (j->off >= size) { j->len = 0; j->ok = true; return; }
    if (j->len > size - j->off) j->len = size - j->off;
    j->ok = esp_flash_read(NULL, j->buf, addr + j->off, j->len) == ESP_OK;
}

/* ---- seen, unread ------------------------------------------------------------
 * A dump is told from another by its sha and its size: two panics of the same
 * firmware make dumps of different sizes often enough, and a mistake only
 * costs a note in the log. */

static void fingerprint(const aos_coredump_info_t *o, char *out, size_t n)
{
    snprintf(out, n, "%.16s:%u", o->elf_sha, (unsigned)o->size);
}

static void add_meta(aos_coredump_info_t *o)
{
    if (!o->present) return;
    char fp[40], seen_fp[40] = "";
    fingerprint(o, fp, sizeof fp);
    aos_hal_pref_get_str("cd_fp", seen_fp, sizeof seen_fp);
    if (strcmp(fp, seen_fp)) return;        /* not seen at a boot yet */
    int32_t v = 0;
    if (aos_hal_pref_get_i32("cd_when", &v)) o->seen = (uint32_t)v;
    v = 0;
    o->unread = aos_hal_pref_get_i32("cd_unread", &v) && v;
}

bool aos_hal_coredump_info(aos_coredump_info_t *out)
{
    memset(out, 0, sizeof *out);
    cd_job_t j = { .op = 0, .info = out };
    aos_flash_call(cd_do, &j);
    if (j.ok) add_meta(out);
    return j.ok;
}

size_t aos_hal_coredump_read(size_t off, void *buf, size_t len)
{
    cd_job_t j = { .op = 1, .off = off, .len = len, .buf = buf };
    aos_flash_call(cd_do, &j);
    return j.ok ? j.len : 0;
}

void aos_hal_coredump_mark_read(void)
{
    aos_hal_pref_set_i32("cd_unread", 0);
}

bool aos_hal_coredump_erase(void)
{
    cd_job_t j = { .op = 2 };
    aos_flash_call(cd_do, &j);
    if (j.ok) {
        ESP_LOGW(TAG, "erased");
        aos_hal_coredump_mark_read();
    }
    return j.ok;
}

bool aos_hal_coredump_boot_check(void)
{
    aos_coredump_info_t o;
    if (!aos_hal_coredump_info(&o) || !o.present) return false;
    char fp[40], seen_fp[40] = "";
    fingerprint(&o, fp, sizeof fp);
    aos_hal_pref_get_str("cd_fp", seen_fp, sizeof seen_fp);
    if (strcmp(fp, seen_fp)) {
        aos_hal_pref_set_str("cd_fp", fp);
        aos_hal_pref_set_i32("cd_when", 0);
        aos_hal_pref_set_i32("cd_unread", 1);
        o.unread = true;
        o.seen = 0;
        ESP_LOGW(TAG, "a new core dump: task %s, pc 0x%08lx, firmware %s%s%s", o.task[0] ? o.task : "?",
                 (unsigned long)o.pc, o.elf_sha[0] ? o.elf_sha : "?", o.version[0] ? " = " : " (not one of the two slots)",
                 o.version);
    }
    /* the time it was first seen, once the clock knows it */
    time_t now = time(NULL);
    if (o.unread && !o.seen && now > 1700000000) aos_hal_pref_set_i32("cd_when", (int32_t)now);
    if (o.unread) {
        ESP_LOGW(TAG, "an unread core dump waits (portal, Log; tools/coredump.sh)");
    }
    return o.unread;
}

static void panic_later(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    ESP_LOGE(TAG, "a panic on purpose, to try the core dump");
    abort();
}

void aos_hal_coredump_test_panic(uint32_t delay_ms)
{
    if (!aos_hal_thread_start("panic", panic_later, (void *)(uintptr_t)delay_ms, 4096, 5)) abort();
}
