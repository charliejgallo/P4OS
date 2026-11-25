/*
 * P4OS - esp-zigbee-lib's task queue, again, for an ESP32-P4 before rev 3.
 *
 * Espressif's Zigbee library for the P4 comes built for the P4 from rev 3:
 * esp_zigbee_task_queue_post() tells an interrupt from a task by reading the
 * standard CLIC CSR mintthresh (0x347), which a P4 before rev 3 (this
 * board's, rev 1.3) does not have, so it stops on an illegal instruction
 * (on the board, 2026-11-24: the first signal the stack handled). It is the
 * only such instruction in the three libraries (objdump), in the module
 * esp_zigbee_task_queue.c, whose state is private. So the whole module is
 * replaced here, by -Wl,--wrap of its six functions (CMakeLists.txt), doing
 * what its disassembly does: a queue of {callback, context} that the
 * stack's main loop drains, and an eventfd that wakes the loop. The one
 * change is the test for an interrupt: xPortInIsrContext(), built by this
 * IDF for this chip.
 */
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_eventfd.h"
#include "esp_radio_spinel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* the library's, as its objects call them */
typedef void (*taskq_cb_t)(void *ctx);
typedef esp_err_t (*workflow_update_t)(void *mainloop);
typedef esp_err_t (*workflow_process_t)(const void *mainloop);
esp_err_t esp_zigbee_platform_workflow_register(const char *name, workflow_update_t update, workflow_process_t process);
void esp_zigbee_platform_workflow_unregister(const char *name);
void ezb_tasklet_process(void);
bool ezb_tasklet_has_pendings(void);

#define NAME   "task_queue"
#define DEPTH  32                       /* the stack's own posts, the portal's and the screen's commands, the 1 s tick */

typedef struct { taskq_cb_t cb; void *ctx; } item_t;

static QueueHandle_t s_q;
static int s_fd = -1;

void __wrap_ezb_tasklet_signal_pending(void)
{
    uint64_t one = 1;
    if (s_fd >= 0 && write(s_fd, &one, sizeof one) != sizeof one) ESP_LOGW("zigbee", "the task queue's eventfd");
}

static esp_err_t update(void *mainloop)
{
    esp_radio_spinel_mainloop_context_t *m = mainloop;
    if (s_fd >= 0) {
        FD_SET(s_fd, &m->read_fds);
        if (m->max_fd < s_fd) m->max_fd = s_fd;
    }
    if (ezb_tasklet_has_pendings()) {
        m->timeout.tv_sec = 0;
        m->timeout.tv_usec = 0;
    }
    return ESP_OK;
}

static esp_err_t process(const void *mainloop)
{
    const esp_radio_spinel_mainloop_context_t *m = mainloop;
    if (s_fd >= 0 && FD_ISSET(s_fd, &m->read_fds)) {
        uint64_t n;
        if (read(s_fd, &n, sizeof n) != sizeof n) ESP_LOGW("zigbee", "the task queue's eventfd read");
    }
    ezb_tasklet_process();
    if (!s_q) return ESP_ERR_INVALID_STATE;
    item_t it;
    while (xQueueReceive(s_q, &it, 0) == pdTRUE) it.cb(it.ctx);
    return ESP_OK;
}

esp_err_t __wrap_esp_zigbee_task_queue_init(void)
{
    s_fd = eventfd(0, EFD_SUPPORT_ISR);
    if (s_fd < 0) return ESP_FAIL;
    s_q = xQueueCreate(DEPTH, sizeof(item_t));
    esp_err_t e = s_q ? esp_zigbee_platform_workflow_register(NAME, update, process) : ESP_ERR_NO_MEM;
    if (e != ESP_OK) {
        if (s_q) vQueueDelete(s_q);
        s_q = NULL;
        close(s_fd);
        s_fd = -1;
    }
    return e;
}

void __wrap_esp_zigbee_task_queue_deinit(void)
{
    if (s_q) vQueueDelete(s_q);
    s_q = NULL;
    if (s_fd >= 0) close(s_fd);
    s_fd = -1;
    esp_zigbee_platform_workflow_unregister(NAME);
}

esp_err_t __wrap_esp_zigbee_task_queue_post(taskq_cb_t cb, void *ctx)
{
    if (!s_q) return ESP_ERR_INVALID_STATE;
    item_t it = { cb, ctx };
    BaseType_t woken = pdFALSE;
    BaseType_t ok = xPortInIsrContext() ? xQueueSendFromISR(s_q, &it, &woken) : xQueueSend(s_q, &it, 10);
    if (ok != pdTRUE) return ESP_FAIL;
    __wrap_ezb_tasklet_signal_pending();
    if (woken) portYIELD_FROM_ISR();
    return ESP_OK;
}

esp_err_t __wrap_esp_zigbee_task_queue_update(void *mainloop) { return update(mainloop); }
esp_err_t __wrap_esp_zigbee_task_queue_process(const void *mainloop) { return process(mainloop); }

/* The stack's own log, every level up to debug, into the board's log as
 * "ezb: ..." (the IDF's filter would keep its debug lines out) */
#include <stdarg.h>
#include <stdio.h>
#include "aos_hal.h"
void __wrap_ezb_plat_log(int level, const char *fmt, ...)
{
    if (level > 4) return;              /* verbose stays out */
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (n) aos_hal_log("ezb", "%s", line);
}
