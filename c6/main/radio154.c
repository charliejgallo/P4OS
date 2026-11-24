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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_ieee802154.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "eh_cp_feat_peer_data.h"
#include "aos_154_proto.h"
#include "radio154.h"
#include "zigbee_test.h"
#include "slots.h"
#include "rcp.h"

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

/* ---- the log to the P4 ---------------------------------------------------- */

/* While the P4 wants it (LOG 1), every log line also goes into a ring that
 * the task sends as LOG messages; the line still goes to the serial port
 * as before. A line written while the ring is being sent (peer_data logs
 * too) is not taken, or it would feed itself. */
#define LOGRING 4096
static char s_log[LOGRING];
static volatile uint32_t s_log_head, s_log_tail;
static volatile bool s_log_on, s_log_sending;
static vprintf_like_t s_log_prev;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

static int log_hook(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int r = s_log_prev ? s_log_prev(fmt, ap) : vprintf(fmt, ap);
    if (s_log_on && !s_log_sending && !xPortInIsrContext()) {
        char line[160];
        int n = vsnprintf(line, sizeof line, fmt, copy);
        if (n > (int)sizeof line - 1) n = sizeof line - 1;
        portENTER_CRITICAL(&s_log_mux);
        if (n > 0 && LOGRING - (s_log_head - s_log_tail) >= (uint32_t)n)
            for (int i = 0; i < n; i++) s_log[s_log_head++ % LOGRING] = line[i];
        portEXIT_CRITICAL(&s_log_mux);
    }
    va_end(copy);
    return r;
}

static void log_flush(void)
{
    static char out[1024];
    uint32_t n = 0;
    portENTER_CRITICAL(&s_log_mux);
    while (s_log_tail != s_log_head && n < sizeof out) out[n++] = s_log[s_log_tail++ % LOGRING];
    portEXIT_CRITICAL(&s_log_mux);
    if (!n) return;
    s_log_sending = true;
    eh_cp_feat_peer_data_send(AOS_154_MSG_LOG, (const uint8_t *)out, n);
    s_log_sending = false;
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
        .heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT),
        .heap_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
        .zb = zigbee_test_state(), .zb_channel = s_state == AOS_154_ZIGBEE ? s_channel : 0,
        .zb_heap_before = zigbee_test_heap_before(), .zb_heap_after = zigbee_test_heap_after(),
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
    /* while the RCP has the radio (for good), only a hello, the log and the slots */
    if (s_state == AOS_154_RCP && c->b[0] != AOS_154_OP_HELLO && c->b[0] != AOS_154_OP_LOG &&
        c->b[0] != AOS_154_OP_SLOTS && c->b[0] != AOS_154_OP_RCP_START) {
        s_err = -5;
        send_status();
        return;
    }
    /* while the Zigbee stack has the radio, only its stop, a hello and the guard */
    if (s_state == AOS_154_ZIGBEE && c->b[0] != AOS_154_OP_ZB_STOP && c->b[0] != AOS_154_OP_HELLO &&
        c->b[0] != AOS_154_OP_UNGUARD && c->b[0] != AOS_154_OP_LOG && c->b[0] != AOS_154_OP_SLOTS) {
        s_err = -5;
        send_status();
        return;
    }
    switch (c->b[0]) {
    case AOS_154_OP_ZB_START:
        if (c->n < 2 || c->b[1] < AOS_154_CH_FIRST || c->b[1] > AOS_154_CH_LAST) { s_err = -1; break; }
        if (guarded()) { s_err = -2; break; }
        if (s_state == AOS_154_SNIFFING) flush();
        radio_off();
        s_cb_set = false;               /* the stack registers its own; ours again afterwards */
        s_guard.on = 1;
        s_channel = c->b[1];
        if (zigbee_test_start(s_channel)) s_state = AOS_154_ZIGBEE;
        else {
            s_guard.on = 0;
            s_err = -6;
        }
        break;
    case AOS_154_OP_ZB_STOP:
        if (s_state == AOS_154_ZIGBEE) {
            zigbee_test_stop();
            s_state = AOS_154_IDLE;
            s_guard.on = 0;
            s_guard.faults = 0;
        }
        break;
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
    case AOS_154_OP_RCP_START:
        if (s_state == AOS_154_RCP) break;
        if (guarded()) { s_err = -2; break; }
        if (s_state == AOS_154_SNIFFING) flush();
        radio_off();
        /* the radio's events go to OpenThread's port now, not to radio154's list */
        if (s_cb_set) {
            esp_ieee802154_event_callback_list_unregister();
            s_cb_set = false;
        }
        s_guard.on = 1;                 /* the RCP's radio counts for the guard too */
        if (rcp_start()) s_state = AOS_154_RCP;
        else {
            s_guard.on = 0;
            s_err = -7;
        }
        break;
    case AOS_154_OP_SLOTS:
    case AOS_154_OP_SLOTS_GROW: {
        int8_t grow = 0;
        if (c->b[0] == AOS_154_OP_SLOTS_GROW) {
            if (s_state != AOS_154_IDLE) { s_err = -5; break; }
            grow = (int8_t)slots_grow();
        }
        slots_info_t si;
        slots_info(&si);
        aos_154_slots_t r = { .layout = si.layout, .grow = grow, .slot_size = si.slot_size,
                              .running_addr = si.running_addr };
        eh_cp_feat_peer_data_send(AOS_154_MSG_SLOTS, (const uint8_t *)&r, sizeof r);
        break;
    }
    case AOS_154_OP_LOG:
        if (c->n < 2) { s_err = -1; break; }
        if (!s_log_prev && c->b[1]) s_log_prev = esp_log_set_vprintf(log_hook);
        s_log_on = c->b[1] != 0;
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
        if (s_log_on) log_flush();
        if (s_state == AOS_154_SNIFFING || s_state == AOS_154_ZIGBEE || s_state == AOS_154_RCP) {
            if (s_state == AOS_154_SNIFFING) flush();
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

/* the P4's spinel for the RCP */
static void on_spinel(uint32_t id, const uint8_t *data, size_t len, void *ctx)
{
    (void)id;
    (void)ctx;
    rcp_from_host(data, len);
}

void radio154_init(void)
{
    guard_boot();
    s_cmds = xQueueCreate(8, sizeof(cmd_t));
    xTaskCreate(task, "radio154", 4096, NULL, 5, &s_task);
    if (eh_cp_feat_peer_data_register_callback(AOS_154_MSG_SPINEL_IN, on_spinel, NULL) != ESP_OK)
        ESP_LOGE(TAG, "could not register the RCP's spinel with peer_data");
    if (eh_cp_feat_peer_data_register_callback(AOS_154_MSG_CMD, on_cmd, NULL) != ESP_OK)
        ESP_LOGE(TAG, "could not register with peer_data");
    else ESP_LOGI(TAG, "ready, the radio off%s", guarded() ? " and guarded" : "");
}
