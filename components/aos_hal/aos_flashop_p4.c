/*
 * P4OS - flash operations from any task, whatever RAM its stack is in.
 *
 * On the P4 every operation on the internal flash (NVS reads included)
 * suspends the cache that PSRAM sits behind, and the flash driver asserts
 * when the calling task's stack is in PSRAM (spi_flash/cache_utils.c,
 * esp_task_stack_is_sane_cache_disabled). CONFIG_SPIRAM_XIP_FROM_PSRAM does
 * not change that: the code keeps running from PSRAM, but the cache is still
 * suspended around the operation. The AmoledOS board met the same assert the
 * moment a PSRAM-stack thread read a preference (2026-09-12).
 *
 * So a task whose stack is in PSRAM hands the operation to this one, whose
 * stack is internal, and waits for it. A task with an internal stack runs it
 * itself, as before. The data may stay where it is: esp_flash_read and
 * esp_flash_write go through an internal bounce buffer when the caller's
 * buffer is in PSRAM. Only the stack matters.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_memory_utils.h"
#include "esp_ota_ops.h"
#include "esp_log.h"

#define FLASHOP_STACK 8192      /* nvs_set_str + commit: ~2 KB on the S3; esp_ota_end verifies the whole image */
#define FLASHOP_PRIO  6

static SemaphoreHandle_t s_mx, s_go, s_done;
static void (*s_fn)(void *);
static void *s_ctx;

static void flashop_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        s_fn(s_ctx);
        xSemaphoreGive(s_done);
    }
}

void aos_flashop_init(void)
{
    if (s_mx) return;
    s_mx = xSemaphoreCreateMutex();
    s_go = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    if (!s_mx || !s_go || !s_done ||
        xTaskCreate(flashop_task, "flashop", FLASHOP_STACK, NULL, FLASHOP_PRIO, NULL) != pdPASS) {
        ESP_LOGE("flashop", "no task: a PSRAM-stack thread must not touch the flash");
        return;
    }
    /* The running partition is looked up in the flash the first time and
     * kept after that: look it up now, from this internal stack. */
    esp_ota_get_running_partition();
}

void aos_flash_call(void (*fn)(void *ctx), void *ctx)
{
    if (esp_ptr_internal((const void *)esp_cpu_get_sp()) ||
        !s_mx || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        fn(ctx);
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_fn = fn;
    s_ctx = ctx;
    xSemaphoreGive(s_go);
    xSemaphoreTake(s_done, portMAX_DELAY);
    xSemaphoreGive(s_mx);
}
