/* P4OS - growing the C6's app slots without the cable (slots.c). */
#pragma once
#include <stdint.h>

enum { SLOTS_UNKNOWN = 0, SLOTS_FACTORY, SLOTS_GROWN, SLOTS_OTHER };

typedef struct {
    uint8_t  layout;            /* SLOTS_*: what the table on the flash is now */
    uint32_t slot_size;         /* ota_0's, as this start read the table */
    uint32_t running_addr;      /* where the running image is */
} slots_info_t;

void slots_info(slots_info_t *out);
/* 1 written (from the next start), 0 already the repository's; below 0 not
 * done: -10 not the factory table, -11 not running from ota_0, -12 cannot
 * read it, -13 the new one did not read back (the factory one is back),
 * -14 neither reads back (the cable on J7). */
int  slots_grow(void);
