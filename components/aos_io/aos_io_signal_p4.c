/*
 * P4OS - PWM (LEDC), the analog level (sigma-delta), infrared (RMT) and CAN
 * (TWAI) on the board: the backends of aos_io_signal.c.
 *
 * The LEDC's timer 1 and channel 1 belong to the backlight (the BSP sets
 * them up at boot), so PWM has timers 0, 2, 3 and the other seven
 * channels. Everything that is not touched from an ISR lives in PSRAM.
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/sdm.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_mx;

static void lock(void)
{
    if (!s_mx) s_mx = xSemaphoreCreateMutex();      /* first use is from a task, before any other */
    xSemaphoreTake(s_mx, portMAX_DELAY);
}

static void unlock(void) { xSemaphoreGive(s_mx); }

/* ======================= PWM: the LEDC ======================= */

#define BL_TIMER 1                      /* the backlight's (BSP) */
#define BL_CHANNEL 1

static struct { uint32_t asked; uint8_t bits; int refs; } s_tm[LEDC_TIMER_MAX];
static bool s_ch_used[LEDC_CHANNEL_MAX];
static bool s_fade_on;

typedef struct { int ch, tm; } pwm_be_t;

static uint8_t bits_for(uint32_t hz)
{
    uint32_t b = ledc_find_suitable_duty_resolution(80000000, hz);
    if (b < 1) b = 1;
    if (b > 20) b = 20;
    return (uint8_t)b;
}

/* A timer running at hz: one already there, or a free one set up. -1 none. */
static int timer_get(uint32_t hz)
{
    for (int t = 0; t < LEDC_TIMER_MAX; t++)
        if (t != BL_TIMER && s_tm[t].refs && s_tm[t].asked == hz) { s_tm[t].refs++; return t; }
    for (int t = 0; t < LEDC_TIMER_MAX; t++) {
        if (t == BL_TIMER || s_tm[t].refs) continue;
        uint8_t bits = bits_for(hz);
        ledc_timer_config_t tc = {
            .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = bits, .timer_num = t,
            .freq_hz = hz, .clk_cfg = LEDC_AUTO_CLK,
        };
        esp_err_t e = ledc_timer_config(&tc);
        if (e != ESP_OK) { aos_hal_log("io", "PWM: %u Hz: %s", (unsigned)hz, esp_err_to_name(e)); return -1; }
        s_tm[t].asked = hz;
        s_tm[t].bits = bits;
        s_tm[t].refs = 1;
        return t;
    }
    aos_hal_log("io", "PWM: no timer left for %u Hz (three frequencies at once at most)", (unsigned)hz);
    return -1;
}

static void timer_put(int t)
{
    if (t >= 0 && s_tm[t].refs > 0) s_tm[t].refs--;
}

bool aos_io_be_pwm_open(aos_io_pwm_t *p, uint32_t freq_hz)
{
    lock();
    int ch = -1;
    for (int c = 0; c < LEDC_CHANNEL_MAX; c++) if (c != BL_CHANNEL && !s_ch_used[c]) { ch = c; break; }
    if (ch < 0) { unlock(); aos_hal_log("io", "PWM: all seven channels are in use"); return false; }
    int tm = timer_get(freq_hz);
    if (tm < 0) { unlock(); return false; }
    ledc_channel_config_t cc = {
        .gpio_num = p->gpio, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = ch,
        .intr_type = LEDC_INTR_DISABLE, .timer_sel = tm, .duty = 0, .hpoint = 0,
    };
    if (ledc_channel_config(&cc) != ESP_OK) { timer_put(tm); unlock(); return false; }
    s_ch_used[ch] = true;
    pwm_be_t *be = heap_caps_calloc(1, sizeof *be, MALLOC_CAP_SPIRAM);
    be->ch = ch;
    be->tm = tm;
    p->be = be;
    p->bits = s_tm[tm].bits;
    p->freq = ledc_get_freq(LEDC_LOW_SPEED_MODE, tm);
    unlock();
    return true;
}

bool aos_io_be_pwm_set_freq(aos_io_pwm_t *p, uint32_t freq_hz)
{
    pwm_be_t *be = p->be;
    lock();
    int tm;
    if (s_tm[be->tm].refs == 1) {
        /* the timer is this channel's alone: set it again */
        s_tm[be->tm].refs = 0;
        tm = timer_get(freq_hz);
        if (tm < 0) { s_tm[be->tm].refs = 1; unlock(); return false; }
    } else {
        tm = timer_get(freq_hz);
        if (tm < 0) { unlock(); return false; }
        timer_put(be->tm);
    }
    if (tm != be->tm) ledc_bind_channel_timer(LEDC_LOW_SPEED_MODE, be->ch, tm);
    be->tm = tm;
    p->bits = s_tm[tm].bits;
    p->freq = ledc_get_freq(LEDC_LOW_SPEED_MODE, tm);
    unlock();
    return true;
}

bool aos_io_be_pwm_set_duty(aos_io_pwm_t *p, float duty, uint32_t fade_ms)
{
    pwm_be_t *be = p->be;
    uint32_t full = 1u << p->bits;
    uint32_t d = (uint32_t)lroundf(duty * (float)full);
    if (d > full) d = full;
    if (fade_ms) {
        if (!s_fade_on) s_fade_on = ledc_fade_func_install(0) == ESP_OK;
        if (s_fade_on &&
            ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, be->ch, d, (int)fade_ms) == ESP_OK &&
            ledc_fade_start(LEDC_LOW_SPEED_MODE, be->ch, LEDC_FADE_NO_WAIT) == ESP_OK)
            return true;
    }
    return ledc_set_duty(LEDC_LOW_SPEED_MODE, be->ch, d) == ESP_OK &&
           ledc_update_duty(LEDC_LOW_SPEED_MODE, be->ch) == ESP_OK;
}

void aos_io_be_pwm_close(aos_io_pwm_t *p)
{
    pwm_be_t *be = p->be;
    if (!be) return;
    lock();
    ledc_stop(LEDC_LOW_SPEED_MODE, be->ch, 0);
    s_ch_used[be->ch] = false;
    timer_put(be->tm);
    unlock();
    gpio_reset_pin(p->gpio);
    heap_caps_free(be);
    p->be = NULL;
}

/* ======================= the analog level: sigma-delta ======================= */

bool aos_io_be_dac_open(aos_io_dac_t *d)
{
    sdm_channel_handle_t ch = NULL;
    sdm_config_t cfg = { .gpio_num = d->gpio, .clk_src = SDM_CLK_SRC_DEFAULT, .sample_rate_hz = 1000000 };
    esp_err_t e = sdm_new_channel(&cfg, &ch);
    if (e != ESP_OK) { aos_hal_log("io", "analog out on GPIO%d: %s", d->gpio, esp_err_to_name(e)); return false; }
    sdm_channel_enable(ch);
    d->be = ch;
    return aos_io_be_dac_set(d, 0.0f);
}

bool aos_io_be_dac_set(aos_io_dac_t *d, float level)
{
    /* -128 is no pulses, 127 nearly all of them */
    int density = (int)lroundf(level * 255.0f) - 128;
    if (density > 127) density = 127;
    if (density < -128) density = -128;
    return sdm_channel_set_pulse_density((sdm_channel_handle_t)d->be, (int8_t)density) == ESP_OK;
}

void aos_io_be_dac_close(aos_io_dac_t *d)
{
    if (!d->be) return;
    sdm_channel_disable((sdm_channel_handle_t)d->be);
    sdm_del_channel((sdm_channel_handle_t)d->be);
    gpio_reset_pin(d->gpio);
    d->be = NULL;
}

/* ======================= infrared: the RMT ======================= */

#define IR_RES_HZ 1000000               /* a tick a microsecond */
#define IR_SYMBOLS (AOS_IR_MAX_DURATIONS / 2)

typedef struct {
    rmt_channel_handle_t ch;
    /* receive: two buffers, so the next frame can land while one is read */
    QueueHandle_t done;                 /* (buffer index << 16) | symbols */
    rmt_symbol_word_t *buf[2];
    int armed;                          /* the buffer the RMT is filling, -1 none */
    rmt_receive_config_t rc;
    /* send */
    rmt_encoder_handle_t enc;
    rmt_symbol_word_t *out;
} ir_be_t;

static bool ir_rx_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *ed, void *user)
{
    ir_be_t *be = user;
    BaseType_t hp = pdFALSE;
    uint32_t v = (uint32_t)be->armed << 16 | (uint32_t)ed->num_symbols;
    xQueueSendFromISR(be->done, &v, &hp);
    return hp == pdTRUE;
}

static bool ir_arm(ir_be_t *be, int which)
{
    be->armed = which;
    if (rmt_receive(be->ch, be->buf[which], IR_SYMBOLS * sizeof(rmt_symbol_word_t), &be->rc) != ESP_OK) {
        be->armed = -1;
        return false;
    }
    return true;
}

bool aos_io_be_ir_open(aos_io_ir_t *ir)
{
    ir_be_t *be = heap_caps_calloc(1, sizeof *be, MALLOC_CAP_SPIRAM);
    if (!be) return false;
    be->armed = -1;
    esp_err_t e;
    if (!ir->tx) {
        rmt_rx_channel_config_t cc = {
            .gpio_num = ir->gpio, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = IR_RES_HZ,
            .mem_block_symbols = 48,     /* longer frames go by ping-pong */
        };
        e = rmt_new_rx_channel(&cc, &be->ch);
        if (e != ESP_OK) goto fail;
        /* PSRAM: the RMT's ISR copies into them, and is not cache-safe */
        be->buf[0] = heap_caps_malloc(IR_SYMBOLS * sizeof(rmt_symbol_word_t), MALLOC_CAP_SPIRAM);
        be->buf[1] = heap_caps_malloc(IR_SYMBOLS * sizeof(rmt_symbol_word_t), MALLOC_CAP_SPIRAM);
        be->done = xQueueCreateWithCaps(4, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        if (!be->buf[0] || !be->buf[1] || !be->done) { e = ESP_ERR_NO_MEM; goto fail; }
        rmt_rx_event_callbacks_t cb = { .on_recv_done = ir_rx_done };
        rmt_rx_register_event_callbacks(be->ch, &cb, be);
        if ((e = rmt_enable(be->ch)) != ESP_OK) goto fail;
        be->rc.signal_range_min_ns = 1000;
        be->rc.signal_range_max_ns = ir->gap_us * 1000;
        ir->be = be;
        if (!ir_arm(be, 0)) { e = ESP_FAIL; goto fail; }
        return true;
    }
    rmt_tx_channel_config_t cc = {
        .gpio_num = ir->gpio, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = IR_RES_HZ,
        .mem_block_symbols = 48, .trans_queue_depth = 2,
    };
    if ((e = rmt_new_tx_channel(&cc, &be->ch)) != ESP_OK) goto fail;
    rmt_copy_encoder_config_t ec = {};
    if ((e = rmt_new_copy_encoder(&ec, &be->enc)) != ESP_OK) goto fail;
    be->out = heap_caps_malloc(AOS_IR_MAX_DURATIONS * sizeof(rmt_symbol_word_t), MALLOC_CAP_SPIRAM);
    if (!be->out) { e = ESP_ERR_NO_MEM; goto fail; }
    ir->be = be;
    if (!aos_io_be_ir_carrier(ir)) { e = ESP_FAIL; goto fail; }
    if ((e = rmt_enable(be->ch)) != ESP_OK) goto fail;
    return true;
fail:
    aos_hal_log("io", "IR %s on GPIO%d: %s", ir->tx ? "out" : "in", ir->gpio, esp_err_to_name(e));
    ir->be = be;
    aos_io_be_ir_close(ir);
    return false;
}

int aos_io_be_ir_read(aos_io_ir_t *ir, uint16_t *us, int max, int timeout_ms)
{
    ir_be_t *be = ir->be;
    if (be->armed < 0 && !ir_arm(be, 0)) return -1;
    uint32_t v;
    if (xQueueReceive(be->done, &v, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return 0;
    int which = (int)(v >> 16);
    size_t n = v & 0xFFFF;
    /* the other buffer catches the next frame while this one is read */
    ir_arm(be, which ^ 1);
    const rmt_symbol_word_t *s = be->buf[which];
    int k = 0;
    for (size_t i = 0; i < n && k < max; i++) {
        if (s[i].duration0 && k < max) us[k++] = (uint16_t)s[i].duration0;
        if (!s[i].duration1) break;     /* the end */
        if (k < max) us[k++] = (uint16_t)s[i].duration1;
    }
    return k;
}

bool aos_io_be_ir_carrier(aos_io_ir_t *ir)
{
    ir_be_t *be = ir->be;
    if (!ir->carrier_hz) return rmt_apply_carrier(be->ch, NULL) == ESP_OK;
    rmt_carrier_config_t cc = { .frequency_hz = ir->carrier_hz, .duty_cycle = (float)ir->duty / 100.0f };
    return rmt_apply_carrier(be->ch, &cc) == ESP_OK;
}

bool aos_io_be_ir_send(aos_io_ir_t *ir, const uint16_t *us, int n)
{
    ir_be_t *be = ir->be;
    /* marks high (the carrier on), spaces low; a duration over the 15 bits
     * of a half-symbol goes in two halves of the same level */
    int half = 0;
    uint64_t total_us = 0;
    memset(be->out, 0, AOS_IR_MAX_DURATIONS * sizeof(rmt_symbol_word_t));
    for (int i = 0; i < n; i++) {
        uint32_t d = us[i];
        total_us += d;
        int level = (i & 1) ? 0 : 1;
        while (d) {
            uint32_t part = d > 32767 ? 32767 : d;
            if (half >= AOS_IR_MAX_DURATIONS * 2) return false;
            rmt_symbol_word_t *w = &be->out[half / 2];
            if (half & 1) { w->level1 = level; w->duration1 = part; }
            else { w->level0 = level; w->duration0 = part; }
            half++;
            d -= part;
        }
    }
    if (half & 1) { be->out[half / 2].level1 = 0; be->out[half / 2].duration1 = 1; half++; }
    rmt_transmit_config_t tc = { .loop_count = 0 };
    if (rmt_transmit(be->ch, be->enc, be->out, (size_t)(half / 2) * sizeof(rmt_symbol_word_t), &tc) != ESP_OK)
        return false;
    return rmt_tx_wait_all_done(be->ch, (int)(total_us / 1000) + 200) == ESP_OK;
}

void aos_io_be_ir_close(aos_io_ir_t *ir)
{
    ir_be_t *be = ir->be;
    if (!be) return;
    if (be->ch) {
        rmt_disable(be->ch);
        rmt_del_channel(be->ch);
    }
    if (be->enc) rmt_del_encoder(be->enc);
    if (be->done) vQueueDeleteWithCaps(be->done);
    heap_caps_free(be->buf[0]);
    heap_caps_free(be->buf[1]);
    heap_caps_free(be->out);
    heap_caps_free(be);
    gpio_reset_pin(ir->gpio);
    ir->be = NULL;
}

/* ======================= CAN: TWAI ======================= */

#define CAN_QUEUE 64

typedef struct {
    twai_node_handle_t node;
    QueueHandle_t rx;
    aos_io_can_t *c;
    uint8_t tx_data[8];
} can_be_t;

static bool can_rx_done(twai_node_handle_t node, const twai_rx_done_event_data_t *ed, void *user)
{
    (void)ed;
    can_be_t *be = user;
    uint8_t data[8];
    twai_frame_t f = { .buffer = data, .buffer_len = sizeof data };
    if (twai_node_receive_from_isr(node, &f) != ESP_OK) return false;
    aos_can_frame_t out = {
        .id = f.header.id, .ext = f.header.ide, .rtr = f.header.rtr,
        .len = (uint8_t)(f.header.dlc > 8 ? 8 : f.header.dlc),
        .t_us = (uint64_t)esp_timer_get_time(),
    };
    if (!out.rtr) memcpy(out.data, data, out.len);
    BaseType_t hp = pdFALSE;
    if (xQueueSendFromISR(be->rx, &out, &hp) != pdTRUE) {
        /* full: the oldest goes */
        aos_can_frame_t old;
        xQueueReceiveFromISR(be->rx, &old, &hp);
        xQueueSendFromISR(be->rx, &out, &hp);
        be->c->dropped++;
    }
    return hp == pdTRUE;
}

bool aos_io_be_can_open(aos_io_can_t *c)
{
    can_be_t *be = heap_caps_calloc(1, sizeof *be, MALLOC_CAP_SPIRAM);
    if (!be) return false;
    be->c = c;
    be->rx = xQueueCreateWithCaps(CAN_QUEUE, sizeof(aos_can_frame_t), MALLOC_CAP_SPIRAM);
    twai_onchip_node_config_t cfg = {
        .io_cfg = { .tx = c->tx_gpio, .rx = c->rx_gpio, .quanta_clk_out = -1, .bus_off_indicator = -1 },
        .bit_timing = { .bitrate = c->bitrate },
        .tx_queue_depth = 4,
        .fail_retry_cnt = -1,
        .flags = {
            .enable_self_test = c->mode == AOS_CAN_SELFTEST,
            .enable_loopback = c->mode == AOS_CAN_SELFTEST,
            .enable_listen_only = c->mode == AOS_CAN_LISTEN,
        },
    };
    esp_err_t e = be->rx ? twai_new_node_onchip(&cfg, &be->node) : ESP_ERR_NO_MEM;
    if (e == ESP_OK) {
        twai_event_callbacks_t cb = { .on_rx_done = can_rx_done };
        e = twai_node_register_event_callbacks(be->node, &cb, be);
    }
    if (e == ESP_OK) e = twai_node_enable(be->node);
    if (e != ESP_OK) {
        aos_hal_log("io", "CAN on GPIO%d/%d at %u: %s", c->tx_gpio, c->rx_gpio, (unsigned)c->bitrate, esp_err_to_name(e));
        if (be->node) twai_node_delete(be->node);
        if (be->rx) vQueueDeleteWithCaps(be->rx);
        heap_caps_free(be);
        return false;
    }
    c->be = be;
    return true;
}

bool aos_io_be_can_filter(aos_io_can_t *c, uint32_t id, uint32_t mask, bool ext)
{
    can_be_t *be = c->be;
    twai_mask_filter_config_t mf = { .id = id & mask, .mask = mask, .is_ext = ext };
    if (twai_node_disable(be->node) != ESP_OK) return false;
    esp_err_t e = twai_node_config_mask_filter(be->node, 0, &mf);
    twai_node_enable(be->node);
    return e == ESP_OK;
}

bool aos_io_be_can_send(aos_io_can_t *c, const aos_can_frame_t *f, int timeout_ms)
{
    can_be_t *be = c->be;
    /* the driver keeps the frame until it is out: wait for it, so these
     * buffers can be the next frame's */
    memcpy(be->tx_data, f->data, f->len);
    twai_frame_t tf = {
        .header = { .id = f->id, .dlc = f->len, .ide = f->ext, .rtr = f->rtr },
        .buffer = be->tx_data, .buffer_len = f->rtr ? 0 : f->len,
    };
    if (twai_node_transmit(be->node, &tf, timeout_ms) != ESP_OK) return false;
    return twai_node_transmit_wait_all_done(be->node, timeout_ms) == ESP_OK;
}

int aos_io_be_can_recv(aos_io_can_t *c, aos_can_frame_t *f, int timeout_ms)
{
    can_be_t *be = c->be;
    return xQueueReceive(be->rx, f, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? 1 : 0;
}

bool aos_io_be_can_status(aos_io_can_t *c, aos_can_status_t *st)
{
    can_be_t *be = c->be;
    twai_node_status_t s;
    twai_node_record_t r;
    if (twai_node_get_info(be->node, &s, &r) != ESP_OK) return false;
    static const char *const NAME[] = { "active", "warning", "passive", "bus_off" };
    st->state = s.state <= TWAI_ERROR_BUS_OFF ? NAME[s.state] : "?";
    st->tx_errors = s.tx_error_count;
    st->rx_errors = s.rx_error_count;
    st->bus_errors = r.bus_err_num;
    return true;
}

bool aos_io_be_can_recover(aos_io_can_t *c)
{
    return twai_node_recover(((can_be_t *)c->be)->node) == ESP_OK;
}

void aos_io_be_can_close(aos_io_can_t *c)
{
    can_be_t *be = c->be;
    if (!be) return;
    twai_node_disable(be->node);
    twai_node_delete(be->node);
    vQueueDeleteWithCaps(be->rx);
    heap_caps_free(be);
    gpio_reset_pin(c->tx_gpio);
    if (c->rx_gpio != c->tx_gpio) gpio_reset_pin(c->rx_gpio);
    c->be = NULL;
}
