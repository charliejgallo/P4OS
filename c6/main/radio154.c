/*
 * P4OS - the C6's 802.15.4 radio, lent to the P4 (docs/plan/ZIGBEE.md).
 *
 * What the P4 asks for and what goes back is in aos_154_proto.h. The radio
 * is driven directly (esp_ieee802154), with no Zigbee or Thread stack: a
 * scan of the channels' energy and the frames heard on one, which is what
 * the workshop needs first. It shares the antenna with the Wi-Fi and the
 * BLE by time (Espressif's software coexistence).
 *
 * The radio is off at every start and only a request of the P4 turns it on.
 * If the C6 falls over twice in a row while it is on, it stays off until the
 * P4 says UNGUARD: the Wi-Fi the board lives on must survive whatever this
 * does (aos_154_proto.h, "The guard").
 *
 * The radio's events come in an interrupt: a frame heard is copied into a
 * ring and given back to the driver at once, and the task sends the ring to
 * the P4 every 40 ms, or sooner when it fills.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_ieee802154.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "eh_cp_feat_peer_data.h"
#include "aos_154_proto.h"
#include "radio154.h"

static const char *TAG = "radio154";

#define RING        8192                /* frames waiting for the P4: ~60 of the longest; a power of two, so the counters may wrap */
#define BATCH       4096                /* what goes in one message */
#define FLUSH_MS    40
#define ED_SAMPLES  8
#define ED_SYMBOLS  64                  /* one energy measure: 64 symbols, 1 ms */

typedef struct { uint8_t b[8]; uint8_t n; } cmd_t;

static QueueHandle_t s_cmds;
static TaskHandle_t  s_task;
static uint8_t       s_state = AOS_154_IDLE, s_channel;
static bool          s_on, s_cb_set;
static int8_t        s_err;
static volatile uint32_t s_frames, s_dropped;

/* the scan's counters, written by the interrupt */
static volatile uint16_t s_scan_frames, s_scan_bytes;
static volatile int8_t   s_ed;
static volatile bool     s_counting;

static uint8_t s_ring[RING];
static volatile uint32_t s_head, s_tail;        /* bytes written, bytes read: they only grow */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_batch[BATCH];

/* The guard: in memory that a restart does not clear. */
#define GUARD_MAGIC 0x31353447u
typedef struct { uint32_t magic; uint8_t on, faults; } guard_t;
static RTC_NOINIT_ATTR guard_t s_guard;

static bool guarded(void) { return s_guard.faults >= 2; }

static void guard_boot(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    if (s_guard.magic != GUARD_MAGIC) {
        s_guard.magic = GUARD_MAGIC;
        s_guard.on = 0;
        s_guard.faults = 0;
    }
    bool fault = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    if (s_guard.on && fault && s_guard.faults < 255) s_guard.faults++;
    s_guard.on = 0;
    if (s_guard.faults) ESP_LOGW(TAG, "the radio was on in %u restarts by a fault in a row%s", s_guard.faults,
                                 guarded() ? ": it stays off" : "");
}

/* ---- the interrupt's side ------------------------------------------------ */

static void IRAM_ATTR rx_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    uint8_t len = frame[0];
    if (len > 127) len = 127;
    if (s_counting) {
        s_scan_frames++;
        s_scan_bytes += len;
    } else if (s_state == AOS_154_SNIFFING) {
        aos_154_rec_t rec = {
            .len = len, .rssi = info->rssi, .lqi = info->lqi, .channel = info->channel,
            .t_lo = (uint32_t)info->timestamp, .t_hi = (uint32_t)(info->timestamp >> 32),
        };
        uint32_t need = sizeof rec + len;
        portENTER_CRITICAL_ISR(&s_mux);
        if (RING - (s_head - s_tail) >= need) {
            uint32_t at = s_head;
            const uint8_t *r = (const uint8_t *)&rec;
            for (uint32_t i = 0; i < sizeof rec; i++) s_ring[at++ % RING] = r[i];
            for (uint32_t i = 0; i < len; i++) s_ring[at++ % RING] = frame[1 + i];
            s_head = at;
            s_frames++;
        } else s_dropped++;
        portEXIT_CRITICAL_ISR(&s_mux);
    }
    esp_ieee802154_receive_handle_done(frame);
}

static void IRAM_ATTR ed_done(int8_t power)
{
    s_ed = power;
    BaseType_t woke = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &woke);
    portYIELD_FROM_ISR(woke);
}

/* ---- the task's side ----------------------------------------------------- */

static void send_status(void)
{
    aos_154_status_t st = {
        .proto = AOS_154_PROTO, .state = s_state, .channel = s_state ? s_channel : 0,
        .guarded = guarded(), .frames = s_frames, .dropped = s_dropped,
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000), .err = s_err, .faults = s_guard.faults,
    };
    eh_cp_feat_peer_data_send(AOS_154_MSG_STATUS, (const uint8_t *)&st, sizeof st);
}

static bool radio_on(void)
{
    if (s_on) return true;
    if (guarded()) { s_err = -2; return false; }
    if (!s_cb_set) {
        esp_ieee802154_event_cb_list_t cb = { .rx_done_cb = rx_done, .ed_done_cb = ed_done };
        if (esp_ieee802154_event_callback_list_register(cb) != ESP_OK) { s_err = -3; return false; }
        s_cb_set = true;
    }
    s_guard.on = 1;
    if (esp_ieee802154_enable() != ESP_OK) { s_guard.on = 0; s_err = -4; return false; }
    esp_ieee802154_set_promiscuous(true);
    esp_ieee802154_set_coordinator(false);
    esp_ieee802154_set_rx_when_idle(true);
    s_on = true;
    return true;
}

static void radio_off(void)
{
    if (!s_on) return;
    esp_ieee802154_disable();
    s_on = false;
    s_guard.on = 0;
    s_guard.faults = 0;                 /* it went on and off again: the next fault counts from one */
}

static void flush(void)
{
    for (;;) {
        uint32_t n = 0;
        portENTER_CRITICAL(&s_mux);
        uint32_t tail = s_tail, head = s_head;
        portEXIT_CRITICAL(&s_mux);
        /* whole records only, as many as fit a batch */
        while (tail + n < head) {
            uint8_t len = s_ring[(tail + n) % RING];
            uint32_t rec = sizeof(aos_154_rec_t) + len;
            if (n + rec > BATCH) break;
            for (uint32_t i = 0; i < rec; i++) s_batch[n + i] = s_ring[(tail + n + i) % RING];
            n += rec;
        }
        if (!n) return;
        eh_cp_feat_peer_data_send(AOS_154_MSG_FRAMES, s_batch, n);
        portENTER_CRITICAL(&s_mux);
        s_tail = tail + n;
        portEXIT_CRITICAL(&s_mux);
    }
}

static void scan(uint8_t first, uint8_t last, uint16_t dwell)
{
    if (first < AOS_154_CH_FIRST || last > AOS_154_CH_LAST || first > last) { s_err = -1; return; }
    if (dwell < 20) dwell = 20;
    if (dwell > 2000) dwell = 2000;
    s_state = AOS_154_SCANNING;
    for (uint8_t ch = first; ch <= last; ch++) {
        s_channel = ch;
        esp_ieee802154_set_channel(ch);
        int sum = 0, n = 0, max = -128;
        for (int i = 0; i < ED_SAMPLES; i++) {
            ulTaskNotifyTake(pdTRUE, 0);
            if (esp_ieee802154_energy_detect(ED_SYMBOLS) != ESP_OK) continue;
            if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20))) continue;
            sum += s_ed;
            n++;
            if (s_ed > max) max = s_ed;
        }
        /* then listen for the rest of the dwell, counting what is heard */
        s_scan_frames = s_scan_bytes = 0;
        s_counting = true;
        esp_ieee802154_receive();
        vTaskDelay(pdMS_TO_TICKS(dwell));
        s_counting = false;
        aos_154_scan_t r = {
            .channel = ch, .ed_max = (int8_t)(n ? max : -128), .ed_avg = (int8_t)(n ? sum / n : -128),
            .samples = (uint8_t)n, .frames = s_scan_frames, .bytes = s_scan_bytes, .dwell_ms = dwell,
        };
        eh_cp_feat_peer_data_send(AOS_154_MSG_SCAN, (const uint8_t *)&r, sizeof r);
        /* a STOP in the middle ends the scan */
        cmd_t c;
        if (xQueuePeek(s_cmds, &c, 0) == pdTRUE && c.b[0] == AOS_154_OP_STOP) break;
    }
}

static void run(const cmd_t *c)
{
    s_err = 0;
    switch (c->b[0]) {
    case AOS_154_OP_HELLO:
        break;
    case AOS_154_OP_SCAN:
        if (c->n < 5) { s_err = -1; break; }
        if (s_state == AOS_154_SNIFFING) s_state = AOS_154_IDLE;
        if (radio_on()) {
            scan(c->b[1], c->b[2], (uint16_t)(c->b[3] | c->b[4] << 8));
            radio_off();
        }
        s_state = AOS_154_IDLE;
        break;
    case AOS_154_OP_SNIFF:
        if (c->n < 2 || c->b[1] < AOS_154_CH_FIRST || c->b[1] > AOS_154_CH_LAST) { s_err = -1; break; }
        if (!radio_on()) break;
        s_channel = c->b[1];
        s_frames = s_dropped = 0;
        portENTER_CRITICAL(&s_mux);
        s_tail = s_head;
        portEXIT_CRITICAL(&s_mux);
        esp_ieee802154_set_channel(s_channel);
        s_state = AOS_154_SNIFFING;
        esp_ieee802154_receive();
        break;
    case AOS_154_OP_STOP:
        s_state = AOS_154_IDLE;
        radio_off();
        flush();
        break;
    case AOS_154_OP_UNGUARD:
        s_guard.faults = 0;
        break;
    default:
        s_err = -1;
    }
    send_status();
}

static void task(void *arg)
{
    (void)arg;
    int ticks = 0;
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_cmds, &c, pdMS_TO_TICKS(FLUSH_MS)) == pdTRUE) run(&c);
        if (s_state == AOS_154_SNIFFING) {
            flush();
            /* the counters once a second, for the screens */
            if (++ticks >= 1000 / FLUSH_MS) {
                ticks = 0;
                send_status();
            }
        }
    }
}

/* peer_data's callback: from its own task, so only a copy into the queue */
static void on_cmd(uint32_t id, const uint8_t *data, size_t len, void *ctx)
{
    (void)id;
    (void)ctx;
    cmd_t c = { .n = (uint8_t)(len > sizeof c.b ? sizeof c.b : len) };
    if (!c.n) return;
    memcpy(c.b, data, c.n);
    if (xQueueSend(s_cmds, &c, 0) != pdTRUE) ESP_LOGW(TAG, "a request was lost: the queue is full");
}

void radio154_init(void)
{
    guard_boot();
    s_cmds = xQueueCreate(8, sizeof(cmd_t));
    xTaskCreate(task, "radio154", 4096, NULL, 5, &s_task);
    if (eh_cp_feat_peer_data_register_callback(AOS_154_MSG_CMD, on_cmd, NULL) != ESP_OK)
        ESP_LOGE(TAG, "could not register with peer_data");
    else ESP_LOGI(TAG, "ready, the radio off%s", guarded() ? " and guarded" : "");
}
