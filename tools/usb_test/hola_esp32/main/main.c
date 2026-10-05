/* A test for P4OS's Programmer and Terminal over the USB host: says hello
 * on the console every two seconds, and echoes back each line it receives.
 * The console is UART0 (a dev board's USB-serial converter) or, on a chip
 * with its own USB (C3, C6, S3, H2), the USB-Serial-JTAG
 * (sdkconfig.defaults.<target>). */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#endif

static void console_init(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();
#else
    uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
#endif
}

static int console_byte(uint8_t *c)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return usb_serial_jtag_read_bytes(c, 1, pdMS_TO_TICKS(20));
#else
    return uart_read_bytes(UART_NUM_0, c, 1, pdMS_TO_TICKS(20));
#endif
}

void app_main(void)
{
    console_init();
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    printf("\r\nHola mundo desde el %s (rev %d), grabado por P4OS\r\n", CONFIG_IDF_TARGET, ci.revision);
    char line[128];
    int n = 0, count = 0;
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        uint8_t c;
        while (console_byte(&c) == 1) {
            if (c == '\r' || c == '\n') {
                if (n) { line[n] = 0; printf("Recibido: %s\r\n", line); n = 0; }
            } else if (n < (int)sizeof line - 1) {
                line[n++] = (char)c;
            }
        }
        fflush(stdout);
        if (xTaskGetTickCount() - next >= pdMS_TO_TICKS(2000)) {
            next = xTaskGetTickCount();
            printf("Hola mundo %d\r\n", ++count);
            fflush(stdout);
        }
    }
}
