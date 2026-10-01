/*
 * p4bench entry point: NVS, results ring, panel + LVGL, then a console on
 * UART0 (the "UART" USB-C port) with one command group per block of the
 * board. `help` lists them; tools/bench_run.py drives them from the Mac.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_console.h"
#include "nvs_flash.h"
#include "bench.h"

static const char *TAG = "p4bench";

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    bench_log_init();

    /* The screen is optional: if the panel does not come up, the console
     * and every non-display test still work. */
    if (disp_init() == ESP_OK) lvport_start();
    else ESP_LOGE(TAG, "display init failed, continuing headless");

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.task_stack_size = 16384;     /* "wifi up" runs esp_wifi_init through esp_hosted here: 4 KB overflowed on the board */
    rc.prompt = "p4bench> ";
    rc.max_cmdline_length = 256;
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uc, &rc, &repl));
    esp_console_register_help_command();

    reg_sys();
    reg_display();
    reg_lvgl();
    reg_touch();
    reg_sd();
    reg_net();
    reg_audio();
    reg_codec();
    reg_io();
    reg_elf();

    /* test 0 runs by itself on every boot */
    int ret;
    esp_console_run("info", &ret);
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
