/*
 * P4OS - diagnostic: is the firmware that runs from PSRAM the one in flash?
 *
 * Built only with P4OS_XIP_CHECK set in the environment (main/CMakeLists.txt).
 * Written to chase a crash on the first boot after some OTA updates; that
 * one turned out to be the app loader's whole-cache sync (docs/MEMORY.md,
 * "Cache maintenance on PSRAM"), and both checks here came out clean, but
 * they stay for the next memory mystery. With CONFIG_SPIRAM_XIP_FROM_PSRAM the
 * startup code copies .text and .rodata from flash into PSRAM and runs them
 * from there; PSRAM keeps its contents across a software restart, so on the
 * first boot of a new image the pages hold the previous image until the copy
 * lands. If part of the copy were lost on its way through the cache, the CPU
 * would run pieces of the old firmware: a crash anywhere, on the first boot
 * of a new image only, and never on the next one.
 *
 * xip_check_run() reads every page of both segments back through the cache
 * (what the CPU sees) and compares it with flash; xip_check_log() puts the
 * result in the log ring, which exists only after aos_hal_init(). The pad
 * (P4OS_XIP_PAD=<KB>) shifts the layout of a second image, as a real change
 * does, for alternating OTAs between two builds.
 */
#include "xip_check.h"

#if P4OS_XIP_CHECK
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_private/mmu_psram_flash.h"

static const char *TAG = "xipcheck";

extern char _instruction_reserved_start, _instruction_reserved_end;
extern char _rodata_reserved_start, _rodata_reserved_end;

#define PAGE  CONFIG_MMU_PAGE_SIZE
#define CHUNK 4096

typedef struct {
    uint32_t bytes, diff, pages_bad, first, read_err;
} seg_t;

static seg_t s_text, s_ro;
static uint32_t s_ms;
static bool s_done;

static void check_seg(const char *start, const char *end, seg_t *r)
{
    uintptr_t a = (uintptr_t)start & ~(uintptr_t)(PAGE - 1);
    uintptr_t b = ((uintptr_t)end + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return;
    uintptr_t last_bad_page = 0;
    for (uintptr_t p = a; p < b; p += CHUNK) {
        size_t pa = mmu_xip_psram_flash_vaddr_to_paddr((const void *)p);
        if (pa == SIZE_MAX || esp_flash_read(NULL, buf, pa, CHUNK) != ESP_OK) {
            r->read_err++;
            continue;
        }
        const uint8_t *ram = (const uint8_t *)p;
        for (size_t i = 0; i < CHUNK; i++) {
            if (ram[i] == buf[i]) continue;
            if (!r->diff) r->first = (uint32_t)(p + i);
            r->diff++;
            uintptr_t pg = (p + i) & ~(uintptr_t)(PAGE - 1);
            if (pg != last_bad_page) {
                r->pages_bad++;
                last_bad_page = pg;
            }
        }
        r->bytes += CHUNK;
    }
    free(buf);
}

#if P4OS_XIP_PAD
/* shifts .text and .rodata of this build against the other one */
__attribute__((used)) static const uint8_t s_pad_ro[P4OS_XIP_PAD * 1024] = { 1 };
__attribute__((used, noinline)) uint32_t xip_check_pad_text(uint32_t x)
{
    /* a few KB of code that nothing calls */
#define R(n) x = x * 2654435761u + (n); x ^= x >> 13;
#define R10(n) R(n) R(n + 1) R(n + 2) R(n + 3) R(n + 4) R(n + 5) R(n + 6) R(n + 7) R(n + 8) R(n + 9)
    R10(1) R10(11) R10(21) R10(31) R10(41) R10(51) R10(61) R10(71) R10(81) R10(91)
    R10(101) R10(111) R10(121) R10(131) R10(141) R10(151) R10(161) R10(171) R10(181) R10(191)
    return x + s_pad_ro[x % sizeof s_pad_ro];
}
#endif

void xip_check_run(void)
{
#if P4OS_XIP_PAD
    static volatile uint32_t sink;
    sink = xip_check_pad_text(sink);       /* kept by the linker: something calls it */
#endif
    int64_t t0 = esp_timer_get_time();
    check_seg(&_instruction_reserved_start, &_instruction_reserved_end, &s_text);
    check_seg(&_rodata_reserved_start, &_rodata_reserved_end, &s_ro);
    s_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    s_done = true;
}

void xip_check_log(void)
{
    if (!s_done) return;
    const seg_t *s[2] = { &s_text, &s_ro };
    const char *nm[2] = { ".text", ".rodata" };
    for (int i = 0; i < 2; i++) {
        if (s[i]->diff || s[i]->read_err) {
            ESP_LOGE(TAG, "%s: %u bytes differ from flash in %u page(s), first at 0x%08x (%u read errors), of %u",
                     nm[i], (unsigned)s[i]->diff, (unsigned)s[i]->pages_bad, (unsigned)s[i]->first,
                     (unsigned)s[i]->read_err, (unsigned)s[i]->bytes);
        } else {
            ESP_LOGW(TAG, "%s: all %u bytes in PSRAM match flash", nm[i], (unsigned)s[i]->bytes);
        }
    }
    ESP_LOGW(TAG, "checked in %u ms", (unsigned)s_ms);
}


/* ---- sentinels around esp_hosted's DMA buffers ------------------------------
 *
 * A suspect of 2026-10-06, cleared by this (no overrun in 40 boots that had
 * two crashes): a loader's freshly zeroed symbol table read back with a name
 * pointer of 0xFFFFFFFF while the C6's SDIO link came up, which is what an
 * SDIO read returns from a side with nothing ready. Every buffer esp_hosted
 * asks for through
 * eh_host_port_dma_alloc_aligned (wrapped by the linker,
 * main/CMakeLists.txt) gets 128 bytes of guard before it and 256 after it,
 * written with a pattern and pushed out of the cache, so that a DMA writing
 * past either end shows in PSRAM. A timer reads the guards every 20 ms
 * (after dropping their lines from the cache) and logs the first damage. */
#include "esp_cache.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define G_PRE   128
#define G_POST  256
#define G_PAT   0xA5
#define G_MAX   32

typedef struct {
    uint8_t *base;          /* what heap_caps gave */
    size_t   len;           /* what esp_hosted asked for */
    void    *caller;
    uint32_t born_ms;
    bool     reported;
} guard_t;

static guard_t s_g[G_MAX];
static portMUX_TYPE s_gmux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_gtimer;
static uint32_t s_gchecks;

void *__real_eh_host_port_dma_alloc_aligned(size_t n, size_t alignment);
void __real_eh_host_port_dma_free(void *p);

static void guard_fill(uint8_t *p, size_t n)
{
    memset(p, G_PAT, n);
    esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M);        /* out to PSRAM */
    esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C);        /* and off the cache */
}

/* bad bytes in a guard, read from PSRAM; first bad offset in *at */
static int guard_bad(uint8_t *p, size_t n, int *at)
{
    esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != G_PAT) {
            if (!bad) *at = (int)i;
            bad++;
        }
    }
    return bad;
}

static void guard_check_one(guard_t *g, const char *when)
{
    if (!g->base || g->reported) return;
    int at_pre = 0, at_post = 0;
    int pre = guard_bad(g->base, G_PRE, &at_pre);
    int post = guard_bad(g->base + G_PRE + g->len, G_POST, &at_post);
    if (!pre && !post) return;
    g->reported = true;
    uint8_t *post_p = g->base + G_PRE + g->len;
    ESP_LOGE("dmaguard", "%s: buffer %p (%u B, from %p, made at %u ms) damaged: %d B before it (first at -%d), "
             "%d B after it (first at +%d: %02x %02x %02x %02x %02x %02x %02x %02x)",
             when, g->base + G_PRE, (unsigned)g->len, g->caller, (unsigned)g->born_ms, pre, G_PRE - at_pre, post, at_post,
             post_p[at_post], post_p[at_post + 1], post_p[at_post + 2], post_p[at_post + 3],
             post_p[at_post + 4], post_p[at_post + 5], post_p[at_post + 6], post_p[at_post + 7]);
}

static void guard_timer(void *arg)
{
    (void)arg;
    s_gchecks++;
    for (int i = 0; i < G_MAX; i++) {
        portENTER_CRITICAL(&s_gmux);
        guard_t g = s_g[i];
        portEXIT_CRITICAL(&s_gmux);
        if (!g.base || g.reported) continue;
        guard_check_one(&g, "timer");
        if (g.reported) {
            portENTER_CRITICAL(&s_gmux);
            if (s_g[i].base == g.base) s_g[i].reported = true;
            portEXIT_CRITICAL(&s_gmux);
        }
    }
}

void *__wrap_eh_host_port_dma_alloc_aligned(size_t n, size_t alignment)
{
    if (!s_gtimer) {
        const esp_timer_create_args_t a = { .callback = guard_timer, .name = "dmaguard" };
        if (esp_timer_create(&a, &s_gtimer) == ESP_OK) esp_timer_start_periodic(s_gtimer, 20000);
    }
    size_t body = (n + 127) & ~(size_t)127;
    uint8_t *base = heap_caps_aligned_alloc(128, G_PRE + body + G_POST, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!base) return __real_eh_host_port_dma_alloc_aligned(n, alignment);
    guard_fill(base, G_PRE);
    guard_fill(base + G_PRE + n, G_POST + (body - n));
    int slot = -1;
    portENTER_CRITICAL(&s_gmux);
    for (int i = 0; i < G_MAX && slot < 0; i++) {
        if (!s_g[i].base) {
            s_g[i] = (guard_t){ base, n, __builtin_return_address(0), (uint32_t)(esp_timer_get_time() / 1000), false };
            slot = i;
        }
    }
    portEXIT_CRITICAL(&s_gmux);
    ESP_LOGW("dmaguard", "guarded %p: %u B for %p%s", base + G_PRE, (unsigned)n, __builtin_return_address(0),
             slot < 0 ? " (table full: not watched)" : "");
    return base + G_PRE;
}

void __wrap_eh_host_port_dma_free(void *p)
{
    guard_t g = { 0 };
    portENTER_CRITICAL(&s_gmux);
    for (int i = 0; i < G_MAX; i++) {
        if (s_g[i].base && s_g[i].base + G_PRE == (uint8_t *)p) {
            g = s_g[i];
            s_g[i].base = NULL;
            break;
        }
    }
    portEXIT_CRITICAL(&s_gmux);
    if (!g.base) {
        __real_eh_host_port_dma_free(p);
        return;
    }
    guard_check_one(&g, "free");
    heap_caps_free(g.base);
}

uint32_t xip_check_guard_checks(void) { return s_gchecks; }

#else
void xip_check_run(void) {}
void xip_check_log(void) {}
#endif
