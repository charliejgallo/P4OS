/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "radio154.h"

#if !SOC_BLE_SUPPORTED
#error "This hosted-BLE coprocessor example requires a BLE-capable target (excludes ESP32-S2)."
#endif

static const char *TAG = "bt_wifi_cp_hosted_hci";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP-Hosted CP Wi-Fi+BT firmware running");
    /* P4OS: the 802.15.4 radio, for the P4 to ask for (radio154.c) */
    radio154_init();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
