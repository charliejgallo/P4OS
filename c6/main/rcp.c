/*
 * P4OS - the C6 as a radio co-processor (RCP) for the P4's Zigbee or Thread
 * stack, over esp_hosted's peer_data (docs/plan/ZIGBEE.md, stage 3).
 *
 * The IDF's OpenThread RCP speaks spinel, HDLC-framed, to its host through
 * two functions only: otPlatUartSend() for what goes out and
 * otPlatUartReceived() for what comes in. Espressif's own examples give it a
 * UART wired to the host; this board has none between the P4 and the C6,
 * and esp_hosted's "spinel over the hosted transport" is "not implemented
 * yet" there. So the link wraps otPlatUartSend (-Wl,--wrap, main/
 * CMakeLists.txt): the bytes go to the P4 as SPINEL messages, and what the P4
 * sends comes in to otPlatUartReceived under the OpenThread lock. The RCP's
 * own UART is set up on UART1 with no pins: nothing comes or goes by it.
 *
 * It starts only when the P4 asks (radio154.c, RCP_START), takes the radio
 * from radio154 for good, and stays until the C6 restarts.
 */
#include <string.h>
#include "sdkconfig.h"
#include "rcp.h"

#if CONFIG_OPENTHREAD_RADIO
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "openthread/error.h"
#include "eh_cp_feat_peer_data.h"

/* OpenThread's UART platform functions (examples/platforms/utils/uart.h in
 * its tree, not among the IDF's public headers) */
otError otPlatUartSend(const uint8_t *aBuf, uint16_t aBufLength);
void otPlatUartSendDone(void);
void otPlatUartReceived(const uint8_t *aBuf, uint16_t aBufLength);
#include "aos_154_proto.h"

static const char *TAG = "rcp";

#define CHUNK 256
typedef struct { uint16_t n; uint8_t b[CHUNK]; } chunk_t;

static QueueHandle_t s_in;
static volatile bool s_running;
static volatile uint32_t s_tx, s_rx;

bool rcp_running(void) { return s_running; }
uint32_t rcp_tx_bytes(void) { return s_tx; }
uint32_t rcp_rx_bytes(void) { return s_rx; }

/* what the RCP sends: to the P4 */
otError __wrap_otPlatUartSend(const uint8_t *buf, uint16_t len)
{
    eh_cp_feat_peer_data_send(AOS_154_MSG_SPINEL, buf, len);
    s_tx += len;
    otPlatUartSendDone();
    return OT_ERROR_NONE;
}

/* what the P4 sends: from peer_data's task into a queue ... */
void rcp_from_host(const uint8_t *data, size_t len)
{
    if (!s_running || !s_in) return;
    while (len) {
        chunk_t c;
        c.n = (uint16_t)(len > CHUNK ? CHUNK : len);
        memcpy(c.b, data, c.n);
        if (xQueueSend(s_in, &c, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "%u bytes from the P4 lost: the queue is full", (unsigned)len);
            return;
        }
        data += c.n;
        len -= c.n;
    }
}

/* ... and from the queue into the RCP, with OpenThread's lock */
static void feeder(void *arg)
{
    (void)arg;
    chunk_t c;
    for (;;) {
        if (xQueueReceive(s_in, &c, portMAX_DELAY) != pdTRUE) continue;
        esp_openthread_lock_acquire(portMAX_DELAY);
        otPlatUartReceived(c.b, c.n);
        esp_openthread_lock_release();
        s_rx += c.n;
    }
}

bool rcp_start(void)
{
    if (s_running) return true;
    s_in = xQueueCreate(16, sizeof(chunk_t));
    if (!s_in) return false;
    /* the OpenThread task queue and the radio driver wait on eventfds */
    esp_vfs_eventfd_config_t ev = { .max_fds = 3 };
    esp_err_t e = esp_vfs_eventfd_register(&ev);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "eventfd: %d", e);
    static esp_openthread_config_t cfg = {
        .platform_config = {
            .radio_config = { .radio_mode = RADIO_MODE_NATIVE },
            .host_config = {
                .host_connection_mode = HOST_CONNECTION_MODE_RCP_UART,
                .host_uart_config = {
                    .port = 1,
                    .uart_config = { .baud_rate = 460800, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                                     .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                                     .source_clk = UART_SCLK_DEFAULT },
                    .rx_pin = UART_PIN_NO_CHANGE,
                    .tx_pin = UART_PIN_NO_CHANGE,
                },
            },
            .port_config = { .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10 },
        },
    };
    s_running = true;                   /* before: the P4 may speak as soon as it starts */
    if (esp_openthread_start(&cfg) != ESP_OK) {
        s_running = false;
        ESP_LOGE(TAG, "OpenThread's RCP did not start");
        return false;
    }
    xTaskCreate(feeder, "rcp_in", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "the RCP is up, spinel over peer_data");
    return true;
}

#else   /* an image without OpenThread: the request refused */

bool rcp_start(void) { return false; }
bool rcp_running(void) { return false; }
void rcp_from_host(const uint8_t *data, size_t len) { (void)data; (void)len; }
uint32_t rcp_tx_bytes(void) { return 0; }
uint32_t rcp_rx_bytes(void) { return 0; }

#endif
