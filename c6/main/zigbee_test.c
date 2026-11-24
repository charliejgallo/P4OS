/*
 * P4OS - Espressif's Zigbee stack on the C6, started only when the P4 asks
 * (docs/plan/ZIGBEE.md, stage 3): a measuring step before deciding where the
 * stack lives. It forms a network of its own as a coordinator, on the one
 * channel asked for and closed to joining, and the C6's status tells the
 * memory before and after. Nothing joins it and it joins nothing.
 *
 * The stack and radio154.c share the radio: radio154 turns it off before
 * this starts, and registers its callbacks again afterwards. The guard
 * counts a fault here like one with the radio on.
 */
#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"
#include "zigbee_test.h"

#if CONFIG_ZB_ENABLED
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_zigbee.h"
#include "esp_coexist.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "zbtest";

static volatile uint8_t s_state;        /* ZB_* */
static volatile uint32_t s_heap_before, s_heap_after;
static uint8_t s_channel;
static TaskHandle_t s_task;

uint8_t zigbee_test_state(void) { return s_state; }
uint32_t zigbee_test_heap_before(void) { return s_heap_before; }
uint32_t zigbee_test_heap_after(void) { return s_heap_after; }

/* Commissioning is asked for from the stack's task queue, not from inside a
 * signal: called from the handler, formation never returned (on the board,
 * 2026-11-24). */
static void commission(void *ctx)
{
    if ((uintptr_t)ctx == EZB_BDB_MODE_NETWORK_FORMATION) {
        /* the one channel, set again here: the stack's start may put back its default */
        ezb_bdb_set_primary_channel_set(1u << s_channel);
        ezb_bdb_set_secondary_channel_set(0);
        ESP_LOGI(TAG, "channels 0x%08x / 0x%08x, factory new %d", (unsigned)ezb_bdb_get_primary_channel_set(),
                 (unsigned)ezb_bdb_get_secondary_channel_set(), ezb_bdb_is_factory_new());
    }
    ezb_err_t e = ezb_bdb_start_top_level_commissioning((ezb_bdb_comm_mode_mask_t)(uintptr_t)ctx);
    ESP_LOGI(TAG, "commissioning 0x%x asked: %d", (unsigned)(uintptr_t)ctx, e);
}

static bool on_signal(const ezb_app_signal_t *sig)
{
    ezb_app_signal_type_t t = ezb_app_signal_get_type(sig);
    const ezb_bdb_signal_simple_params_t *p = ezb_app_signal_get_params(sig);
    ESP_LOGI(TAG, "signal %s (0x%04x)", ezb_app_signal_to_string(t), t);
    switch (t) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:   /* started with no autostart: the BDB's initialization first */
        esp_zigbee_task_queue_post(commission, (void *)(uintptr_t)EZB_BDB_MODE_INITIALIZATION);
        return true;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        ESP_LOGI(TAG, "started (status %d); forming a network on channel %u", p ? p->status : -1, s_channel);
        esp_zigbee_task_queue_post(commission, (void *)(uintptr_t)EZB_BDB_MODE_NETWORK_FORMATION);
        return true;
    case EZB_BDB_SIGNAL_FORMATION:
        s_heap_after = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
        if (p && p->status == 0) {
            s_state = ZB_FORMED;
            ESP_LOGI(TAG, "formed: %u bytes free (%u before)", (unsigned)s_heap_after, (unsigned)s_heap_before);
        } else {
            s_state = ZB_FAILED;
            ESP_LOGW(TAG, "formation failed (%d)", p ? p->status : -1);
        }
        return true;
    default:
        return false;
    }
}

static void task(void *arg)
{
    (void)arg;
    /* Wi-Fi and 802.15.4 sharing the air: listening worked without it, but
     * the stack has to send (Espressif's esp_coexist.h) */
    ESP_LOGI(TAG, "Wi-Fi and 802.15.4 coexistence: %d", esp_coex_wifi_i154_enable());
    esp_zigbee_config_t cfg = {
        .device_config = { .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR, .install_code_policy = false,
                           .zczr_config = { .max_children = 16 } },
        .platform_config = { .storage_partition_name = "nvs",
                             .radio_config = { .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE } },
    };
    if (esp_zigbee_init(&cfg) != ESP_OK) {
        s_state = ZB_FAILED;
        ESP_LOGW(TAG, "init failed");
    } else {
        ezb_app_signal_add_handler(on_signal);
        ezb_bdb_set_primary_channel_set(1u << s_channel);
        if (esp_zigbee_start(false) != ESP_OK) s_state = ZB_FAILED;
        else esp_zigbee_launch_mainloop();      /* returns after esp_zigbee_stop */
        /* not esp_zigbee_factory_reset(): it restarts the C6, and the link
         * to the P4 with it. What the stack saved stays in the NVS: a few
         * hundred bytes, the same network again the next time. */
        esp_zigbee_deinit();
    }
    if (s_state != ZB_FAILED) s_state = ZB_OFF;
    s_task = NULL;
    vTaskDelete(NULL);
}

bool zigbee_test_start(uint8_t channel)
{
    if (s_task || channel < 11 || channel > 26) return false;
    s_channel = channel;
    s_heap_before = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    s_heap_after = 0;
    s_state = ZB_STARTING;
    if (xTaskCreate(task, "zigbee", 6144, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        s_state = ZB_FAILED;
        return false;
    }
    return true;
}

void zigbee_test_stop(void)
{
    if (!s_task) return;
    esp_zigbee_stop();
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
}

bool zigbee_test_running(void) { return s_task != NULL; }

#else   /* the image releases carry: no stack, the requests refused */

bool zigbee_test_start(uint8_t channel) { (void)channel; return false; }
void zigbee_test_stop(void) {}
bool zigbee_test_running(void) { return false; }
uint8_t zigbee_test_state(void) { return ZB_OFF; }
uint32_t zigbee_test_heap_before(void) { return 0; }
uint32_t zigbee_test_heap_after(void) { return 0; }

#endif
