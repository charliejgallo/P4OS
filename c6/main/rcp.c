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
#include "openthread/tasklet.h"
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
        /* A UART's bytes come in from inside OpenThread's loop, which then
         * looks at its timers again before it sleeps; these come from this
         * task, while the loop sleeps in its select() with the timeout it had.
         * A request that only sets a timer (a frame to send: its CSMA backoff
         * is the RCP's own timer) was left waiting until something else woke
         * the loop, and the host gave up on the RCP and reset it (on the
         * board, 2026-11-24). So the loop is woken after every chunk. */
        otTaskletsSignalPending(esp_openthread_get_instance());
        esp_openthread_lock_release();
        s_rx += c.n;
    }
}

/* The energy scan, in short measures. OpenThread's port asks the 802.15.4
 * driver for one measure as long as the host's scan (a Zigbee coordinator's
 * formation asks for 138 ms, 8625 symbols), and on this C6, with its Wi-Fi up,
 * such a measure never ended: the driver stayed in it, with neither its
 * "done" nor its "failed" (on the board, 2026-11-24, counted below), so the
 * host waited forever. radio154's scan, 1 ms measures with a timeout each,
 * always got its answers. So the scan is done the same way here: measures of
 * 64 symbols one after another until the scan's time is up, the strongest
 * kept, each one asked for again if it does not answer in 20 ms or the
 * driver says it failed (esp_ieee802154_ed_failed, a weak function nobody
 * else defines: coexistence refused it). The result goes to OpenThread's own
 * callback, as the one long measure's would have. */
#include "esp_ieee802154.h"
#include "esp_coexist.h"
#include "esp_timer.h"
#include "openthread/platform/radio.h"
#include "esp_openthread.h"

esp_err_t __real_esp_ieee802154_event_callback_list_register(esp_ieee802154_event_cb_list_t cb);

#define ED_CHUNK    64                  /* symbols: 1 ms */
#define ED_WAIT_MS  20

static TaskHandle_t s_ed_task;
static void (*s_ot_ed_done)(int8_t power);       /* OpenThread's port's */
static volatile bool s_ed_on;
static volatile int8_t s_ed_max;
static volatile uint32_t s_ed_samples, s_ed_failed, s_ed_timeouts, s_ed_scans;
static int64_t s_ed_until;

uint32_t rcp_ed_failures(void) { return s_ed_failed; }

static void IRAM_ATTR ed_wake(void)
{
    BaseType_t woke = pdFALSE;
    if (s_ed_task) vTaskNotifyGiveFromISR(s_ed_task, &woke);
    portYIELD_FROM_ISR(woke);
}

static void IRAM_ATTR ed_done(int8_t power)
{
    if (!s_ed_on) {                     /* not one of the scan's: OpenThread's as it was */
        if (s_ot_ed_done) s_ot_ed_done(power);
        return;
    }
    if (!s_ed_samples || power > s_ed_max) s_ed_max = power;
    s_ed_samples++;
    ed_wake();
}

void IRAM_ATTR esp_ieee802154_ed_failed(uint16_t error)
{
    (void)error;
    s_ed_failed++;
    if (s_ed_on) ed_wake();
}

/* While measuring (2026-11-24): each frame sent, and how it ended */
static esp_ieee802154_transmit_done_cb_t s_ot_tx_done;
static esp_ieee802154_transmit_failed_cb_t s_ot_tx_failed;
static volatile uint32_t s_tx_start, s_tx_ok, s_tx_fail[8];

/* The driver's turn against Wi-Fi's: one setting, "txrx", for sending and
 * for listening alike. A coordinator listens all the time: at the middle
 * level that took half of the Wi-Fi's speed (1.5 MB from the card: ~500 KB/s
 * without Zigbee, ~250 with it), and at the low one Wi-Fi was back but most
 * frames were refused (16 of 22; on the board, 2026-11-24). So it is low
 * while listening and middle while a frame goes out: raised before the
 * send, lowered in its "done" or "failed", before the driver goes back to
 * listening. The driver's own variable, written directly: its setter is not
 * in IRAM and these run in its interrupt. */
extern esp_ieee802154_coex_config_t s_coex_config;      /* esp_ieee802154_util.c */
/* high to send: at the middle level, while a door sensor looked for the
 * network, four of the five beacons answering it were refused and it heard
 * none (on the board, 2026-11-24); frames sent are few and short */
#define TX_TURN     IEEE802154_HIGH
#define LISTEN_TURN IEEE802154_LOW

static void IRAM_ATTR tx_done(const uint8_t *frame, const uint8_t *ack, esp_ieee802154_frame_info_t *info)
{
    s_coex_config.txrx = LISTEN_TURN;
    s_tx_ok++;
    if (s_ot_tx_done) s_ot_tx_done(frame, ack, info);
}

static void IRAM_ATTR tx_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error)
{
    s_coex_config.txrx = LISTEN_TURN;
    s_tx_fail[(unsigned)error & 7]++;
    if (s_ot_tx_failed) s_ot_tx_failed(frame, error);
}

esp_err_t __real_esp_ieee802154_transmit(const uint8_t *frame, bool cca);
esp_err_t __wrap_esp_ieee802154_transmit(const uint8_t *frame, bool cca)
{
    s_tx_start++;
    s_coex_config.txrx = TX_TURN;
    return __real_esp_ieee802154_transmit(frame, cca);
}

/* OpenThread's port's callbacks, with the measure's own put in between */
esp_err_t __wrap_esp_ieee802154_event_callback_list_register(esp_ieee802154_event_cb_list_t cb)
{
    if (s_running) {                    /* only while the RCP starts: radio154's list is its own */
        s_ot_ed_done = cb.ed_done_cb;
        cb.ed_done_cb = ed_done;
        s_ot_tx_done = cb.tx_done_cb;
        cb.tx_done_cb = tx_done;
        s_ot_tx_failed = cb.tx_failed_cb;
        cb.tx_failed_cb = tx_failed;
    }
    return __real_esp_ieee802154_event_callback_list_register(cb);
}

/* called by OpenThread with its lock held */
otError __wrap_otPlatRadioEnergyScan(otInstance *ins, uint8_t ch, uint16_t ms)
{
    (void)ins;
    esp_ieee802154_set_channel(ch);
    s_ed_samples = 0;
    s_ed_max = -128;
    s_ed_until = esp_timer_get_time() + (int64_t)ms * 1000;
    s_ed_scans++;
    s_ed_on = true;
    esp_ieee802154_energy_detect(ED_CHUNK);
    if (s_ed_task) xTaskNotifyGive(s_ed_task);
    return OT_ERROR_NONE;
}

static void ed_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool answered = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_ed_on ? ED_WAIT_MS : 1000)) != 0;
        if (!s_ed_on) {
            static uint32_t seen, ticks;
            uint32_t now = s_tx_start + s_tx_ok;
            for (int i = 0; i < 8; i++) now += s_tx_fail[i];
            ticks++;
            if (now != seen && ticks >= 5) {        /* at most one line in 5 s */
                seen = now;
                ticks = 0;
                ESP_LOGI(TAG, "frames: %u sent, %u done; failed: cca %u, abort %u, no ack %u, bad ack %u, coex %u, sec %u",
                         (unsigned)s_tx_start, (unsigned)s_tx_ok, (unsigned)s_tx_fail[1], (unsigned)s_tx_fail[2],
                         (unsigned)s_tx_fail[3], (unsigned)s_tx_fail[4], (unsigned)s_tx_fail[5], (unsigned)s_tx_fail[6]);
            }
            continue;
        }
        esp_openthread_lock_acquire(portMAX_DELAY);
        if (s_ed_on) {
            if (!answered) s_ed_timeouts++;
            if (esp_timer_get_time() < s_ed_until) esp_ieee802154_energy_detect(ED_CHUNK);
            else {
                s_ed_on = false;
                int8_t r = s_ed_samples ? s_ed_max : OT_RADIO_RSSI_INVALID;
                if (s_ed_scans <= 3 || !s_ed_samples)
                    ESP_LOGI(TAG, "energy scan %u: %u measures, strongest %d dBm (%u refused, %u unanswered)",
                             (unsigned)s_ed_scans, (unsigned)s_ed_samples, r, (unsigned)s_ed_failed,
                             (unsigned)s_ed_timeouts);
                if (s_ot_ed_done) s_ot_ed_done(r);
            }
        }
        esp_openthread_lock_release();
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
    /* Wi-Fi and 802.15.4 taking turns on the one radio: without it, with the
     * C6's Wi-Fi up, the RCP's measures and frames were left hanging (on the
     * board, 2026-11-24). zigbee_test.c does the same. */
    ESP_LOGI(TAG, "Wi-Fi and 802.15.4 coexistence: %s", esp_err_to_name(esp_coex_wifi_i154_enable()));
    s_running = true;                   /* before: the P4 may speak as soon as it starts */
    if (esp_openthread_start(&cfg) != ESP_OK) {
        s_running = false;
        ESP_LOGE(TAG, "OpenThread's RCP did not start");
        return false;
    }
    /* 802.15.4's turn against Wi-Fi's and BLE's: OpenThread sets it by the
     * device's role only when it runs the network itself, not as an RCP.
     * Low while listening, raised for each frame sent (tx_done, above), high
     * for what is timed. */
    esp_ieee802154_coex_config_t cc = { .idle = IEEE802154_IDLE, .txrx = LISTEN_TURN, .txrx_at = IEEE802154_HIGH };
    esp_ieee802154_set_coex_config(cc);
    xTaskCreate(feeder, "rcp_in", 3072, NULL, 5, NULL);
    xTaskCreate(ed_task, "rcp_ed", 3072, NULL, 6, &s_ed_task);
    ESP_LOGI(TAG, "the RCP is up, spinel over peer_data");
    return true;
}

#else   /* an image without OpenThread: the request refused */

bool rcp_start(void) { return false; }
bool rcp_running(void) { return false; }
void rcp_from_host(const uint8_t *data, size_t len) { (void)data; (void)len; }
uint32_t rcp_tx_bytes(void) { return 0; }
uint32_t rcp_rx_bytes(void) { return 0; }
uint32_t rcp_ed_failures(void) { return 0; }

#endif
