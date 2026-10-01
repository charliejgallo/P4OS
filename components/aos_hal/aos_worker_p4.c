/*
 * P4OS - one background task for an app (aos_hal.h, "Worker").
 *
 * The contract is the watch's: one task at a time, the app's function loops
 * on its own and returns once aos_hal_worker_should_stop() says so, and
 * nothing in it touches LVGL. What differs on the P4:
 *
 *   - The stack is in PSRAM. A worker reads the card and decodes, and the
 *     one thing a PSRAM stack cannot do - reach the internal flash - only
 *     happens through the preferences, which go through aos_flash_call
 *     (docs/MEMORY.md).
 *   - The defaults are core 0 at priority 3. LVGL is pinned to core 1 here,
 *     so a worker on core 1 would take turns with the render; on core 0 it
 *     overlaps it. Priority 3 is the apps' threads' (the portal, HA, MQTT):
 *     a worker busy all the time shares the core with them rather than
 *     starving them, and the audio (5-7) stays above. The shell's tick, at
 *     1, gets LVGL's lock back through the mutex's priority inheritance.
 */
#include "aos_hal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define WORKER_CORE_DEFAULT 0
#define WORKER_PRIO_DEFAULT 3
#define STOP_WAIT_MS        3000

static const char *TAG = "worker";

static TaskHandle_t     s_task;
static aos_worker_fn_t  s_fn;
static void            *s_arg;
static volatile bool    s_stop;
static volatile bool    s_done = true;

static void worker_main(void *p)
{
    (void)p;
    s_fn(s_arg);
    s_done = true;
    vTaskDeleteWithCaps(NULL);
}

bool aos_hal_worker_start_on(const char *name, aos_worker_fn_t fn, void *arg,
                             uint32_t stack_bytes, int core, int prio)
{
    if (!fn || !s_done) return false;
    s_fn = fn;
    s_arg = arg;
    s_stop = false;
    s_done = false;
    if (core < 0 || core > 1) core = WORKER_CORE_DEFAULT;
    if (prio < 0) prio = WORKER_PRIO_DEFAULT;
    if (stack_bytes < 4096) stack_bytes = 4096;
    if (xTaskCreatePinnedToCoreWithCaps(worker_main, name ? name : "aos_worker", stack_bytes, NULL,
                                        (UBaseType_t)prio, &s_task, core, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = NULL;
        s_done = true;
        ESP_LOGE(TAG, "no memory for a %u-byte stack", (unsigned)stack_bytes);
        return false;
    }
    return true;
}

bool aos_hal_worker_start(const char *name, aos_worker_fn_t fn, void *arg, uint32_t stack_bytes)
{
    return aos_hal_worker_start_on(name, fn, arg, stack_bytes, -1, -1);
}

void aos_hal_worker_stop(void)
{
    if (s_done) {
        s_task = NULL;
        return;
    }
    s_stop = true;
    for (int waited = 0; !s_done && waited < STOP_WAIT_MS; waited += 10) vTaskDelay(pdMS_TO_TICKS(10));
    if (!s_done) {
        /* Left running: the app is about to free what it reads. Say so; a
         * worker that ignores should_stop() is the app's bug. */
        ESP_LOGE(TAG, "the worker did not stop in %d ms", STOP_WAIT_MS);
        return;
    }
    s_task = NULL;
}

bool aos_hal_worker_running(void) { return !s_done; }

bool aos_hal_worker_should_stop(void) { return s_stop; }

void aos_hal_worker_sleep(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }

/* aos_hal_worker_split(): one helper per core, made the first time a caller
 * on the other core asks, and kept (16 KB of PSRAM each). A caller waits on
 * its own semaphore, so two callers on different cores never share one. */
typedef struct {
    TaskHandle_t      task;
    SemaphoreHandle_t go, done, busy;
    void            (*fn)(void *arg, int part);
    void             *arg;
} split_helper_t;

static split_helper_t s_split[2];

static void split_main(void *p)
{
    split_helper_t *h = p;
    for (;;) {
        xSemaphoreTake(h->go, portMAX_DELAY);
        h->fn(h->arg, 1);
        xSemaphoreGive(h->done);
    }
}

bool aos_hal_worker_split(void (*fn)(void *arg, int part), void *arg)
{
    if (!fn) return false;
    int core = 1 - (int)xPortGetCoreID();
    split_helper_t *h = &s_split[core];
    if (!h->task) {
        if (!h->go) h->go = xSemaphoreCreateBinary();
        if (!h->done) h->done = xSemaphoreCreateBinary();
        if (!h->busy) h->busy = xSemaphoreCreateMutex();
        if (!h->go || !h->done || !h->busy) return false;
        if (xTaskCreatePinnedToCoreWithCaps(split_main, core ? "split1" : "split0", 16384, h,
                                            uxTaskPriorityGet(NULL), &h->task, core,
                                            MALLOC_CAP_SPIRAM) != pdPASS) {
            h->task = NULL;
            ESP_LOGE(TAG, "no memory for the split helper on core %d", core);
            return false;
        }
    }
    xSemaphoreTake(h->busy, portMAX_DELAY);
    vTaskPrioritySet(h->task, uxTaskPriorityGet(NULL));
    h->fn = fn;
    h->arg = arg;
    xSemaphoreGive(h->go);
    fn(arg, 0);
    xSemaphoreTake(h->done, portMAX_DELAY);
    xSemaphoreGive(h->busy);
    return true;
}
