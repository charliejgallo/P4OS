/*
 * P4OS - growing the C6's app slots without the cable (docs/C6.md, "The
 * slots").
 *
 * A board's C6 keeps the partition table it left the factory with: two app
 * slots of 1.5 MB, and 960 KB unused at the end of its 4 MB. An OTA only
 * ever writes a slot, never the table, so the table stays. This writes the
 * repository's own (partitions_eh_cp_ota_4m.csv, two slots of 1.94 MB, built
 * into this image by gen_esp32part.py, so a flash through J7 and this give the
 * same bytes), only when the P4 asks and only when it is safe:
 *
 *   - the table on the flash is the factory one, entry by entry (anything
 *     else is left alone: someone else's table is theirs);
 *   - the C6 runs from ota_0, which starts at 0x10000 in both tables, so the
 *     running image does not move. (Running from ota_1, the P4 sends the
 *     firmware again first: it lands in ota_0.) Even if the table went in
 *     with the C6 on ota_1, its bootloader would fall back to ota_0's image;
 *   - the sector is written and read back; a difference is written again.
 *
 * What can still go wrong is a power cut in the few milliseconds between the
 * erase and the write: the bootloader then finds no table, and the way back
 * is the cable on J7 (docs/C6.md, "Recovery"). The new layout is used from
 * the next start: the board restarts afterwards.
 */
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "slots.h"

static const char *TAG = "slots";

#define TABLE_AT    0x8000
#define TABLE_SIZE  0x1000
#define ENTRY       32

extern const uint8_t p4os_table_start[] asm("_binary_p4os_table_bin_start");
extern const uint8_t p4os_table_end[] asm("_binary_p4os_table_bin_end");

typedef struct { const char *name; uint8_t type, sub; uint32_t off, size; } part_t;

/* Waveshare's, read from a board's flash (c6/backup/c6-factory.bin) */
static const part_t FACTORY[] = {
    { "nvs", 1, 0x02, 0x9000, 0x4000 },   { "otadata", 1, 0x00, 0xd000, 0x2000 },
    { "phy_init", 1, 0x01, 0xf000, 0x1000 }, { "ota_0", 0, 0x10, 0x10000, 0x180000 },
    { "ota_1", 0, 0x11, 0x190000, 0x180000 },
};

static bool entry_is(const uint8_t *e, const part_t *p)
{
    uint32_t off, size;
    memcpy(&off, e + 4, 4);
    memcpy(&size, e + 8, 4);
    return e[0] == 0xAA && e[1] == 0x50 && e[2] == p->type && e[3] == p->sub && off == p->off && size == p->size &&
           !strncmp((const char *)e + 12, p->name, 16);
}

static bool is_factory(const uint8_t *t)
{
    for (size_t i = 0; i < sizeof FACTORY / sizeof FACTORY[0]; i++)
        if (!entry_is(t + i * ENTRY, &FACTORY[i])) return false;
    /* and nothing after them but the MD5 entry and the erased rest */
    const uint8_t *e = t + sizeof FACTORY / sizeof FACTORY[0] * ENTRY;
    return (e[0] == 0xEB && e[1] == 0xEB) || e[0] == 0xFF;
}

void slots_info(slots_info_t *out)
{
    memset(out, 0, sizeof *out);
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *o0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    out->running_addr = run ? run->address : 0;
    out->slot_size = o0 ? o0->size : 0;     /* as this start read it */
    /* on the heap for the moment it takes: the C6's RAM is short */
    uint8_t *t = malloc(TABLE_SIZE);
    if (!t || esp_flash_read(NULL, t, TABLE_AT, TABLE_SIZE) != ESP_OK) { out->layout = SLOTS_UNKNOWN; free(t); return; }
    size_t n = (size_t)(p4os_table_end - p4os_table_start);
    if (n <= TABLE_SIZE && !memcmp(t, p4os_table_start, n)) out->layout = SLOTS_GROWN;
    else if (is_factory(t)) out->layout = SLOTS_FACTORY;
    else out->layout = SLOTS_OTHER;
    free(t);
}

int slots_grow(void)
{
    slots_info_t in;
    slots_info(&in);
    if (in.layout == SLOTS_GROWN) return 0;             /* already */
    if (in.layout != SLOTS_FACTORY) return -10;         /* not a table this knows: left alone */
    if (in.running_addr != 0x10000) return -11;         /* not on ota_0: install again first */
    size_t n = (size_t)(p4os_table_end - p4os_table_start);
    if (n == 0 || n > TABLE_SIZE) return -12;
    uint8_t *buf = malloc(TABLE_SIZE), *back = malloc(TABLE_SIZE), *orig = malloc(TABLE_SIZE);
    int r = -12;
    if (!buf || !back || !orig || esp_flash_read(NULL, orig, TABLE_AT, TABLE_SIZE) != ESP_OK) goto out;
    memset(buf, 0xFF, TABLE_SIZE);
    memcpy(buf, p4os_table_start, n);
    for (int attempt = 0; attempt < 3; attempt++) {
        ESP_LOGW(TAG, "writing the partition table (attempt %d)", attempt + 1);
        if (esp_flash_erase_region(NULL, TABLE_AT, TABLE_SIZE) != ESP_OK) continue;
        if (esp_flash_write(NULL, buf, TABLE_AT, TABLE_SIZE) != ESP_OK) continue;
        if (esp_flash_read(NULL, back, TABLE_AT, TABLE_SIZE) == ESP_OK && !memcmp(buf, back, TABLE_SIZE)) {
            ESP_LOGW(TAG, "the partition table is the repository's now: two slots of 1.94 MB from the next start");
            r = 1;
            goto out;
        }
    }
    /* the new one would not stay: the factory one back, as it was */
    ESP_LOGE(TAG, "the partition table did not read back right: putting the factory one back");
    for (int attempt = 0; attempt < 3; attempt++) {
        if (esp_flash_erase_region(NULL, TABLE_AT, TABLE_SIZE) == ESP_OK &&
            esp_flash_write(NULL, orig, TABLE_AT, TABLE_SIZE) == ESP_OK &&
            esp_flash_read(NULL, back, TABLE_AT, TABLE_SIZE) == ESP_OK && !memcmp(orig, back, TABLE_SIZE)) {
            r = -13;
            goto out;
        }
    }
    r = -14;                                            /* neither: the cable on J7 */
out:
    free(buf);
    free(back);
    free(orig);
    return r;
}

