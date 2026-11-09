/*
 * P4OS - DMX512 on the ESP32-P4 (the front is aos_io_dmx.c, the contract
 * aos_io.h).
 *
 * SEND uses ESP-IDF's UART driver at 250000 8N2 and a task that, every
 * period, waits for the last packet to be out, holds the line low for the
 * BREAK by inverting TX, lets it go for the MARK AFTER BREAK and queues the
 * start code and the slots. DE (when the port has one) stays high: DMX
 * only goes one way.
 *
 * RECEIVE does not install the driver: its ring buffer would mix the last
 * bytes of a packet, still in the FIFO when the BREAK is detected, with the
 * next packet. An interrupt of its own takes every byte as it comes (the
 * FIFO threshold is 1: some 22700 interrupts a second at full rate, a few
 * per cent of a core) and, at each BREAK, closes the packet so far. The
 * BREAK's own NUL (a zero byte with a framing error) is dropped. Then it
 * times the BREAK and the MAB by watching the RX pin for a few hundred
 * microseconds: the BREAK is detected about 10 bit times (40 us) after it
 * starts, so its length is that plus the time until the line rises; the
 * MAB, the time until it falls again for the start code's start bit.
 * Polling inside the interrupt is short (a BREAK is ~0.1-0.2 ms on real
 * consoles) and gives up after 3 ms.
 *
 * Not IRAM: while the flash is busy (a preference being written) the
 * interrupt waits, and a packet or two may come out short and be counted
 * as bad. Nothing here goes to internal RAM on purpose.
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_intr_alloc.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "hal/uart_ll.h"
#include "soc/gpio_struct.h"
#include "soc/uart_periph.h"

#define PKT_MAX 513

typedef struct {
    aos_io_dmx_t *d;
    int num;
    uart_dev_t *hw;
    int tx, rx, de;
    /* receive */
    intr_handle_t ih;
    portMUX_TYPE mux;
    uint8_t cur[PKT_MAX + 2];
    int cur_len;
    bool cur_over;
    int fe_n, fe_at;                    /* framing errors in this packet, and where the last one was */
    uint8_t done[PKT_MAX];
    int done_len;
    uint32_t packets, bad, other, shortb;
    uint16_t brk, mab;
    int64_t last_us;
    /* send */
    volatile bool stop, stopped;
} be_t;

/* ---- receive ---- */

static void publish(be_t *b)
{
    if (!b->cur_len) return;
    /* the BREAK's NUL: a zero with a framing error, the last byte read */
    if (b->fe_n && b->fe_at == b->cur_len && b->cur[b->cur_len - 1] == 0) { b->cur_len--; b->fe_n--; }
    if (!b->cur_len) return;
    portENTER_CRITICAL_ISR(&b->mux);
    int n = b->cur_len > PKT_MAX ? PKT_MAX : b->cur_len;
    memcpy(b->done, b->cur, (size_t)n);
    b->done_len = n;
    b->packets++;
    if (b->fe_n || b->cur_over) b->bad++;
    if (b->cur[0] != 0) b->other++;
    b->last_us = esp_timer_get_time();
    portEXIT_CRITICAL_ISR(&b->mux);
}

/* Polls the RX pin for a level, up to 3 ms: the time it took, or -1. */
static int64_t wait_level(int gpio, int level, int64_t from)
{
    int64_t t;
    while ((int)gpio_ll_get_level(&GPIO, (uint32_t)gpio) != level) {
        t = esp_timer_get_time();
        if (t - from > 3000) return -1;
    }
    return esp_timer_get_time();
}

static void rx_isr(void *arg)
{
    be_t *b = arg;
    uart_dev_t *hw = b->hw;
    for (int guard = 0; guard < 16; guard++) {
        uint32_t st = uart_ll_get_intsts_mask(hw);
        if (!st) break;
        if (st & (UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT)) {
            uint8_t tmp[128];
            uint32_t n = uart_ll_get_rxfifo_len(hw);
            if (n > sizeof tmp) n = sizeof tmp;
            uart_ll_read_rxfifo(hw, tmp, n);
            for (uint32_t i = 0; i < n; i++) {
                if (b->cur_len < PKT_MAX + 1) b->cur[b->cur_len++] = tmp[i];
                else b->cur_over = true;
            }
            uart_ll_clr_intsts_mask(hw, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT);
        }
        if (st & UART_INTR_FRAM_ERR) {
            b->fe_n++;
            b->fe_at = b->cur_len;
            uart_ll_clr_intsts_mask(hw, UART_INTR_FRAM_ERR);
        }
        if (st & UART_INTR_RXFIFO_OVF) {
            uart_ll_rxfifo_rst(hw);
            b->cur_over = true;
            uart_ll_clr_intsts_mask(hw, UART_INTR_RXFIFO_OVF);
        }
        if (st & UART_INTR_BRK_DET) {
            int64_t t_det = esp_timer_get_time();
            uart_ll_rxfifo_rst(hw);                 /* the BREAK's NUL, if it is there */
            uart_ll_clr_intsts_mask(hw, UART_INTR_BRK_DET | UART_INTR_FRAM_ERR | UART_INTR_RXFIFO_FULL);
            publish(b);
            b->cur_len = 0;
            b->cur_over = false;
            b->fe_n = 0;
            b->fe_at = -1;
            /* time the BREAK and the MAB on the pin */
            int64_t t_rise = wait_level(b->rx, 1, t_det);
            int64_t t_fall = t_rise > 0 ? wait_level(b->rx, 0, t_rise) : -1;
            portENTER_CRITICAL_ISR(&b->mux);
            b->brk = t_rise > 0 ? (uint16_t)(t_rise - t_det + 40) : 0;
            b->mab = t_fall > 0 ? (uint16_t)(t_fall - t_rise) : 0;
            if ((b->brk && b->brk < 88) || (b->mab && b->mab < 8)) b->shortb++;
            portEXIT_CRITICAL_ISR(&b->mux);
        }
        uart_ll_clr_intsts_mask(hw, st & ~(UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT | UART_INTR_FRAM_ERR |
                                           UART_INTR_RXFIFO_OVF | UART_INTR_BRK_DET));
    }
}

static bool rx_open(be_t *b)
{
    uart_config_t c = {
        .baud_rate = 250000, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_param_config(b->num, &c) != ESP_OK) return false;
    if (uart_set_pin(b->num, UART_PIN_NO_CHANGE, b->rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return false;
    /* TX idle high, so an automatic transceiver never drives the bus; DE low */
    gpio_set_direction(b->tx, GPIO_MODE_OUTPUT);
    gpio_set_level(b->tx, 1);
    if (b->de >= 0) { gpio_set_direction(b->de, GPIO_MODE_OUTPUT); gpio_set_level(b->de, 0); }
    b->hw = UART_LL_GET_HW(b->num);
    uart_ll_disable_intr_mask(b->hw, UINT32_MAX);
    uart_ll_set_rxfifo_full_thr(b->hw, 1);
    uart_ll_rxfifo_rst(b->hw);
    uart_ll_clr_intsts_mask(b->hw, UINT32_MAX);
    if (esp_intr_alloc(uart_periph_signal[b->num].irq, ESP_INTR_FLAG_LOWMED, rx_isr, b, &b->ih) != ESP_OK) return false;
    uart_ll_ena_intr_mask(b->hw, UART_INTR_RXFIFO_FULL | UART_INTR_FRAM_ERR | UART_INTR_RXFIFO_OVF | UART_INTR_BRK_DET);
    return true;
}

static void rx_close(be_t *b)
{
    if (b->hw) uart_ll_disable_intr_mask(b->hw, UINT32_MAX);
    if (b->ih) esp_intr_free(b->ih);
    b->ih = NULL;
}

/* ---- send ---- */

static void tx_task(void *arg)
{
    be_t *b = arg;
    aos_io_dmx_t *d = b->d;
    static uint8_t buf[PKT_MAX];
    while (!b->stop) {
        int64_t t0 = esp_timer_get_time();
        aos_hal_mutex_lock(d->mx);
        aos_dmx_cfg_t c = d->cfg;
        memcpy(buf, d->tx, (size_t)c.slots + 1);
        aos_hal_mutex_unlock(d->mx);
        buf[0] = c.start_code;
        uart_wait_tx_done(b->num, pdMS_TO_TICKS(50));
        uart_set_line_inverse(b->num, UART_SIGNAL_TXD_INV);      /* BREAK: the line low */
        esp_rom_delay_us(c.break_us);
        uart_set_line_inverse(b->num, 0);                        /* MARK AFTER BREAK */
        esp_rom_delay_us(c.mab_us);
        uart_write_bytes(b->num, buf, (size_t)c.slots + 1);
        portENTER_CRITICAL(&b->mux);
        b->packets++;
        b->last_us = esp_timer_get_time();
        b->brk = c.break_us;
        b->mab = c.mab_us;
        portEXIT_CRITICAL(&b->mux);
        int64_t spent_ms = (esp_timer_get_time() - t0) / 1000;
        int wait = (int)c.period_ms - (int)spent_ms;
        vTaskDelay(pdMS_TO_TICKS(wait > 1 ? wait : 1));
    }
    b->stopped = true;
}

static bool tx_open(be_t *b)
{
    uart_config_t c = {
        .baud_rate = 250000, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(b->num, 256, 1024, 0, NULL, 0) != ESP_OK) return false;
    if (uart_param_config(b->num, &c) != ESP_OK || uart_set_pin(b->num, b->tx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_driver_delete(b->num);
        return false;
    }
    if (b->de >= 0) { gpio_set_direction(b->de, GPIO_MODE_OUTPUT); gpio_set_level(b->de, 1); }
    b->stop = b->stopped = false;
    if (!aos_hal_thread_start("dmx_tx", tx_task, b, 4096, 6)) {
        uart_driver_delete(b->num);
        return false;
    }
    return true;
}

static void tx_close(be_t *b)
{
    b->stop = true;
    for (int i = 0; i < 100 && !b->stopped; i++) aos_hal_sleep_ms(5);
    uart_wait_tx_done(b->num, pdMS_TO_TICKS(50));
    uart_driver_delete(b->num);
}

/* ---- the backend ---- */

bool aos_io_be_dmx_open(aos_io_dmx_t *d)
{
    be_t *b = calloc(1, sizeof *b);
    if (!b) return false;
    b->d = d;
    b->tx = d->port->pins[0];
    b->rx = d->port->pins[1];
    b->de = d->port->pins[2];
    b->fe_at = -1;
    portMUX_INITIALIZE(&b->mux);
    b->num = aos_io_be_uart_take();
    if (b->num < 0) { aos_hal_log("io", "DMX: no UART left"); free(b); return false; }
    d->be = b;
    bool ok = d->dir == AOS_DMX_SEND ? tx_open(b) : rx_open(b);
    if (!ok) {
        if (d->dir == AOS_DMX_RECEIVE) rx_close(b);
        aos_io_be_uart_give(b->num);
        gpio_reset_pin(b->tx);
        gpio_reset_pin(b->rx);
        if (b->de >= 0) gpio_reset_pin(b->de);
        free(b);
        d->be = NULL;
        aos_hal_log("io", "DMX: could not start");
    }
    return ok;
}

int aos_io_be_dmx_read(aos_io_dmx_t *d, uint8_t *out, int max, aos_dmx_stats_t *st)
{
    be_t *b = d->be;
    if (!b) return 0;
    int n = 0;
    portENTER_CRITICAL(&b->mux);
    if (d->dir == AOS_DMX_RECEIVE && out && max > 0) {
        n = b->done_len < max ? b->done_len : max;
        memcpy(out, b->done, (size_t)n);
    }
    st->packets = b->packets;
    st->bad = b->bad;
    st->other_code = b->other;
    st->short_break = b->shortb;
    st->break_us = b->brk;
    st->mab_us = b->mab;
    st->slots = (uint16_t)(d->dir == AOS_DMX_RECEIVE ? (b->done_len > 0 ? b->done_len - 1 : 0) : d->cfg.slots);
    st->start_code = d->dir == AOS_DMX_RECEIVE ? (b->done_len ? b->done[0] : 0) : d->cfg.start_code;
    int64_t last = b->last_us;
    portEXIT_CRITICAL(&b->mux);
    int64_t age = last ? (esp_timer_get_time() - last) / 1000 : INT32_MAX;
    st->age_ms = (uint32_t)(age > UINT32_MAX ? UINT32_MAX : age);
    return d->dir == AOS_DMX_RECEIVE ? (n > 0 ? n - 1 : 0) : d->cfg.slots;
}

void aos_io_be_dmx_close(aos_io_dmx_t *d)
{
    be_t *b = d->be;
    if (!b) return;
    if (d->dir == AOS_DMX_SEND) tx_close(b);
    else rx_close(b);
    if (b->de >= 0) { gpio_set_level(b->de, 0); gpio_reset_pin(b->de); }
    gpio_reset_pin(b->tx);
    gpio_reset_pin(b->rx);
    aos_io_be_uart_give(b->num);
    free(b);
    d->be = NULL;
}
