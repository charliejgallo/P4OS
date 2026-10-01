/*
 * P4OS - the log in a ring, so the portal can show it without a cable.
 *
 * aos_hal_log() on both platforms, and on the board every ESP_LOG line too
 * (aos_hal_p4.c hooks esp_log's vprintf), land here as text lines. Positions
 * are absolute byte counts since boot, so a reader that keeps the last one it
 * got never sees a line twice and knows when it fell behind the ring.
 *
 * On the board the ring is 256 KB in a part of PSRAM that a restart does not
 * clear (EXT_RAM_NOINIT_ATTR; the boot's PSRAM test skips it). At the next
 * boot, if the ring left behind is whole, its last PREV_BYTES are kept as
 * "the previous boot" (aos_hal_log_prev_read, GET /api/log?prev=1): what
 * happened before a panic, a watchdog or a hang, with no serial cable - the
 * USB port may be busy being a keyboard. A power cut loses it.
 *
 * Platform-free but for that attribute: a mutex from the HAL, nothing else.
 */
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_cache.h"
#define RING_BYTES  (256 * 1024)
#define PREV_BYTES  (64 * 1024)
#define RING_MAGIC  0x50344C47u          /* "P4LG" */
typedef struct {
    uint32_t magic, check;               /* check = magic ^ total, written last */
    uint32_t total;                      /* bytes ever written in that boot */
    char     data[RING_BYTES];
} ring_t;
static EXT_RAM_NOINIT_ATTR ring_t s_nr;
static char *s_ring = s_nr.data;
static char *s_prev;                     /* the previous boot's tail, or NULL */
static size_t s_prev_len;
#else
#define RING_BYTES  (32 * 1024)
static char *s_ring;
#endif

static size_t s_total;                  /* bytes ever written */
static void *s_mx;

static void ring_open(void)
{
#ifdef ESP_PLATFORM
    if (s_nr.magic == RING_MAGIC && s_nr.check == (RING_MAGIC ^ s_nr.total) && s_nr.total) {
        size_t n = s_nr.total < PREV_BYTES ? s_nr.total : PREV_BYTES;
        s_prev = malloc(n);
        if (s_prev) {
            size_t from = s_nr.total - n;
            for (size_t i = 0; i < n; i++) s_prev[i] = s_nr.data[(from + i) % RING_BYTES];
            s_prev_len = n;
        }
    }
    s_nr.magic = RING_MAGIC;
    s_nr.total = 0;
    s_nr.check = RING_MAGIC;
#else
    s_ring = malloc(RING_BYTES);
#endif
    s_mx = aos_hal_mutex_create();
}

void aos_logring_add(const char *text, size_t len)
{
    if (!s_mx) {
        ring_open();
        if (!s_ring || !s_mx) return;
    }
    aos_hal_mutex_lock(s_mx);
    for (size_t i = 0; i < len; i++) s_ring[(s_total + i) % RING_BYTES] = text[i];
    s_total += len;
#ifdef ESP_PLATFORM
    s_nr.total = (uint32_t)s_total;
    s_nr.check = RING_MAGIC ^ s_nr.total;
    /* out of the cache now, not whenever it is evicted: a reset loses what
     * is still only in the cache, and the last lines are the ones wanted */
    size_t at = (s_total - len) % RING_BYTES, first = len < RING_BYTES - at ? len : RING_BYTES - at;
    esp_cache_msync(s_nr.data + at, first, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (first < len) esp_cache_msync(s_nr.data, len - first, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync(&s_nr, 16, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#endif
    aos_hal_mutex_unlock(s_mx);
}

size_t aos_hal_log_total(void) { return s_total; }

size_t aos_hal_log_read(size_t from, char *out, size_t out_len, size_t *next)
{
    if (!s_ring || !s_mx || !out_len) { if (next) *next = s_total; return 0; }
    aos_hal_mutex_lock(s_mx);
    size_t oldest = s_total > RING_BYTES ? s_total - RING_BYTES : 0;
    if (from < oldest) from = oldest;
    if (from > s_total) from = s_total;
    size_t n = s_total - from;
    if (n > out_len - 1) n = out_len - 1;
    for (size_t i = 0; i < n; i++) out[i] = s_ring[(from + i) % RING_BYTES];
    out[n] = 0;
    if (next) *next = from + n;
    aos_hal_mutex_unlock(s_mx);
    return n;
}

size_t aos_hal_log_prev_read(size_t from, char *out, size_t out_len, size_t *next)
{
#ifdef ESP_PLATFORM
    if (!s_prev || from >= s_prev_len || !out_len) { if (next) *next = s_prev ? s_prev_len : 0; if (out_len) out[0] = 0; return 0; }
    size_t n = s_prev_len - from;
    if (n > out_len - 1) n = out_len - 1;
    memcpy(out, s_prev + from, n);
    out[n] = 0;
    if (next) *next = from + n;
    return n;
#else
    (void)from;
    if (out_len) out[0] = 0;
    if (next) *next = 0;
    return 0;
#endif
}
