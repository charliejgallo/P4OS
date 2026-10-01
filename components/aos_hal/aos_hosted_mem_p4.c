/*
 * P4OS - esp_hosted's memory and threads.
 *
 * Buffers. esp_hosted takes every buffer it trades with the C6 straight from
 * heap_caps_aligned_alloc(INTERNAL | DMA): its mempool (mempool.c), the
 * streaming receive buffers (sdio_drv.c, MEM_ALLOC in os_wrapper.h) and its
 * allocator entry. The mempool never gives a block back, and the streaming
 * buffers grow to the longest burst the C6 sends. On the board that ran
 * internal RAM out twice (2026-09-29, from the core dumps): a network scan
 * (sdio_drv.c:701) and an upload over a wider TCP window, where an 18 KB
 * streaming buffer could not be had (sdio_drv.c:670).
 *
 * The P4's SDMMC can DMA to and from PSRAM (SOC_SDMMC_PSRAM_DMA_CAPABLE),
 * so all of them come from PSRAM, cache-line aligned as that path needs. The
 * call is renamed inside esp_hosted's library only, after it is built
 * (objcopy --redefine-sym in the project's CMakeLists.txt), so nothing else
 * in the firmware is affected. The link does 1-2 MB/s: next to the panel's
 * 100 MB/s that is nothing on the PSRAM bus.
 *
 * The receive side has to stay in streaming mode: the C6's factory firmware
 * sends streams, and with the host in packet mode the link died at the first
 * upload.
 *
 * Threads. Its threads come from a table of function pointers
 * (g_hosted_osi_funcs). There are six (sdio_read, sdio_write, sdio_rx_buf,
 * sdio_process_rx, rpc_rx, rpc_tx), 5 KB each, all at priority 23, and their
 * stacks go to PSRAM: 30 KB less internal RAM. None of them touches the
 * flash, which is the one thing a PSRAM stack cannot do (see
 * aos_flashop_p4.c). esp_hosted starts them from a constructor of its own,
 * before app_main, so the entries are replaced from a constructor with a
 * priority, which runs before every plain one (startup.c, do_global_ctors).
 * The PSRAM heap is already up by then (add_psram_to_heap, CORE stage).
 */
#include "hosted_os_abstraction.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include <stdlib.h>

extern hosted_osi_funcs_t g_hosted_osi_funcs;

#define PSRAM_LINE 128              /* CONFIG_CACHE_L2_CACHE_LINE_128B */

/* What heap_caps_aligned_alloc means inside libespressif__esp_hosted.a */
void *aos_hosted_aligned_alloc(size_t align, size_t size, uint32_t caps)
{
    size_t a = align < PSRAM_LINE ? PSRAM_LINE : align;
    void *p = heap_caps_aligned_alloc(a, (size + PSRAM_LINE - 1) & ~(size_t)(PSRAM_LINE - 1), MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_aligned_alloc(align, size, caps);
}

/* Same contract as hosted_thread_create (port/src/os_wrapper.c): returns a
 * pointer to the task handle, which _h_thread_cancel frees. */
static void *thread_create(char *name, uint32_t prio, uint32_t stack, void (*fn)(void const *), void *arg)
{
    TaskHandle_t *h = malloc(sizeof *h);
    if (!h) return NULL;
    if (xTaskCreatePinnedToCoreWithCaps((TaskFunction_t)fn, name, stack, arg, prio, h,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM) == pdPASS) return h;
    if (xTaskCreate((TaskFunction_t)fn, name, stack, arg, prio, h) == pdPASS) return h;
    free(h);
    return NULL;
}

static int thread_cancel(void *handle)
{
    TaskHandle_t *h = handle;
    if (!h) return -1;
    if (esp_ptr_external_ram(pxTaskGetStackStart(*h))) vTaskDeleteWithCaps(*h);
    else vTaskDelete(*h);
    free(h);
    return 0;
}

/* esp_hosted 1.4.7 destroys the semaphore of a sync RPC's table entry
 * without looking, and an entry left with none (two RPCs crossing, one of
 * them timed out) asserted in hosted_destroy_semaphore - a panic, from a
 * status-bar RSSI read (2026-09-29). Nothing to destroy is now an error. */
static int (*s_destroy_sem)(void *sem);

static int destroy_semaphore(void *sem)
{
    return sem ? s_destroy_sem(sem) : -1;
}

__attribute__((constructor(200))) static void hosted_threads_to_psram(void)
{
    g_hosted_osi_funcs._h_thread_create = thread_create;
    g_hosted_osi_funcs._h_thread_cancel = thread_cancel;
    s_destroy_sem = g_hosted_osi_funcs._h_destroy_semaphore;
    g_hosted_osi_funcs._h_destroy_semaphore = destroy_semaphore;
}
