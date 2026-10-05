/* A test for P4OS's Programmer and Terminal over the USB host: says hello
 * on UART0 every two seconds, and echoes back each line it receives. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_chip_info.h"

void app_main(void)
{
    uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    printf("\r\nHola mundo desde el ESP32 (rev %d), grabado por P4OS\r\n", ci.revision);
    char line[128];
    int n = 0, count = 0;
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        uint8_t c;
        while (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(20)) == 1) {
            if (c == '\r' || c == '\n') {
                if (n) { line[n] = 0; printf("Recibido: %s\r\n", line); n = 0; }
            } else if (n < (int)sizeof line - 1) {
                line[n++] = (char)c;
            }
        }
        if (xTaskGetTickCount() - next >= pdMS_TO_TICKS(2000)) {
            next = xTaskGetTickCount();
            printf("Hola mundo %d\r\n", ++count);
        }
    }
}
