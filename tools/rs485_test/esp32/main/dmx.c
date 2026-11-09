/* DMX512 for the bench's ESP32 (main.c has the rest): a console that plays
 * a show, and a light that shows what it gets on a strip of 8 WS2812B
 * pixels on IO18 (5 V and GND from the DevKit).
 *
 *   console  UART2 at 250000 8N2 through the same RS485 module, a packet
 *            every 25 ms (40 a second), 512 slots, BREAK 176 us and MAB
 *            16 us made by inverting TX. The show: a rainbow running along
 *            eight RGB pixels on channels 1-24, a 7-channel PAR on 25-31
 *            (dimmer breathing, colour cycling); "p" cycles it with a chase
 *            (one channel at a time, to find which is which), all at a
 *            third, and black. Its own strip shows channels 1-24.
 *   light    receives with an interrupt of its own (every byte; a BREAK
 *            closes the packet, as the P4 does) and puts 24 channels from
 *            its address on the strip: pixel i = address + 3i (R, G, B).
 *            "+" and "-" move the address by one, ">" by 24. A line a
 *            second: packets a second, slots, start code, BREAK and MAB as
 *            measured, bad packets.
 *            It also answers RDM (rdm_resp.c, 2026-11-09): a controller
 *            finds it by discovery (UID 7FF0 and the last 4 bytes of the
 *            MAC), reads what it is, sets its address (the same one + and
 *            - move) and its label, and turns identify on: the strip then
 *            blinks white. A request is answered as soon as its last byte
 *            is in (the length is in its third byte), from a task the
 *            interrupt wakes, with the receive interrupt off while it talks
 *            so it does not hear itself.
 *
 * The strip is limited to a third of its brightness: eight pixels at full
 * white draw almost half an amp, and it all comes from the USB. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/uart.h"
#include "esp_intr_alloc.h"
#include "esp_mac.h"
#include "rdm_resp.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "hal/uart_ll.h"
#include "soc/gpio_struct.h"
#include "soc/uart_periph.h"

#define BUS        UART_NUM_2
#define PIN_TX     17
#define PIN_RX     4
#define PIN_DE     16
#define PIN_PIX    18
#define NPIX       8
#define BRIGHT_MAX 85           /* of 255: a third */

/* ---- the strip ---- */

static rmt_channel_handle_t s_rmt;
static rmt_encoder_handle_t s_enc;

static void pixels_init(void)
{
    if (s_rmt) return;
    rmt_tx_channel_config_t ch = {
        .gpio_num = PIN_PIX, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10000000,
        .mem_block_symbols = 64, .trans_queue_depth = 2,
    };
    if (rmt_new_tx_channel(&ch, &s_rmt) != ESP_OK) { printf("strip: no RMT channel\n"); return; }
    rmt_bytes_encoder_config_t ec = {
        .bit0 = { .level0 = 1, .duration0 = 4, .level1 = 0, .duration1 = 8 },     /* 0.4 us high, 0.8 low */
        .bit1 = { .level0 = 1, .duration0 = 8, .level1 = 0, .duration1 = 4 },     /* 0.8 high, 0.4 low */
        .flags.msb_first = 1,
    };
    rmt_new_bytes_encoder(&ec, &s_enc);
    rmt_enable(s_rmt);
}

/* rgb: NPIX x (R, G, B), 0-255; sent as GRB at a third */
static void pixels_show(const uint8_t *rgb)
{
    if (!s_rmt) return;
    static uint8_t grb[NPIX * 3];
    for (int i = 0; i < NPIX; i++) {
        grb[3 * i] = (uint8_t)(rgb[3 * i + 1] * BRIGHT_MAX / 255);
        grb[3 * i + 1] = (uint8_t)(rgb[3 * i] * BRIGHT_MAX / 255);
        grb[3 * i + 2] = (uint8_t)(rgb[3 * i + 2] * BRIGHT_MAX / 255);
    }
    rmt_transmit_config_t tc = { .loop_count = 0 };
    rmt_tx_wait_all_done(s_rmt, 20);
    rmt_transmit(s_rmt, s_enc, grb, sizeof grb, &tc);
}

/* ---- the console ---- */

static uint8_t s_uni[513];
static int s_pattern;
static const char *const PATTERN[4] = { "show (rainbow on 1-24, PAR on 25-31)", "chase (one channel at a time)", "all at a third", "black" };
static uint32_t s_sent;

static void hue(float h, uint8_t *rgb)
{
    float r = fabsf(fmodf(h * 6.0f, 6.0f) - 3.0f) - 1.0f;
    float g = 2.0f - fabsf(fmodf(h * 6.0f, 6.0f) - 2.0f);
    float b = 2.0f - fabsf(fmodf(h * 6.0f, 6.0f) - 4.0f);
    rgb[0] = (uint8_t)(fminf(fmaxf(r, 0), 1) * 255);
    rgb[1] = (uint8_t)(fminf(fmaxf(g, 0), 1) * 255);
    rgb[2] = (uint8_t)(fminf(fmaxf(b, 0), 1) * 255);
}

static void show_step(float t)
{
    memset(s_uni, 0, sizeof s_uni);
    switch (s_pattern) {
    case 0:
        for (int p = 0; p < 8; p++) hue(fmodf(t / 6.0f + p / 8.0f, 1.0f), s_uni + 1 + 3 * p);
        s_uni[25] = (uint8_t)(128 + 127 * sinf(t * 1.5f));
        hue(fmodf(t / 10.0f, 1.0f), s_uni + 26);
        break;
    case 1: s_uni[(int)(t / 0.3f) % 512 + 1] = 255; break;
    case 2: memset(s_uni + 1, 85, 512); break;
    default: break;
    }
}

void dmx_pattern_next(void)
{
    s_pattern = (s_pattern + 1) % 4;
    printf("dmx console: %s\n", PATTERN[s_pattern]);
}

static void console_setup(void)
{
    uart_config_t c = {
        .baud_rate = 250000, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(BUS, 256, 1024, 0, NULL, 0);
    uart_param_config(BUS, &c);
    uart_set_pin(BUS, PIN_TX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_set_direction(PIN_DE, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_DE, 1);                  /* one way: the driver always on */
    printf("\ndmx console on UART2 TX IO%d (DE IO%d high), 512 slots, 40 packets a second; p: pattern\n", PIN_TX, PIN_DE);
}

void dmx_console_round(void)
{
    static int64_t next, t_print;
    int64_t now = esp_timer_get_time();
    if (now < next) { vTaskDelay(1); return; }
    next = now + 25000;
    show_step((float)now / 1e6f);
    s_uni[0] = 0;
    uart_wait_tx_done(BUS, pdMS_TO_TICKS(30));
    uart_set_line_inverse(BUS, UART_SIGNAL_TXD_INV);    /* BREAK */
    esp_rom_delay_us(176);
    uart_set_line_inverse(BUS, 0);                      /* MAB */
    esp_rom_delay_us(16);
    uart_write_bytes(BUS, s_uni, sizeof s_uni);
    s_sent++;
    pixels_show(s_uni + 1);
    if (now - t_print > 5000000) {
        t_print = now;
        printf("dmx console: %" PRIu32 " packets sent, %s\n", s_sent, PATTERN[s_pattern]);
    }
}

/* ---- the light ---- */

typedef struct {
    uart_dev_t *hw;
    intr_handle_t ih;
    portMUX_TYPE mux;
    uint8_t cur[516];
    int cur_len, fe_n, fe_at;
    bool over;
    uint8_t done[513];
    int done_len;
    uint32_t packets, bad;
    uint16_t brk, mab;
    int64_t last_us;
} rx_t;

static rx_t R = { .mux = portMUX_INITIALIZER_UNLOCKED };
static int s_addr = 1;

/* RDM: the request the interrupt caught, for the task */
static rdm_dev_t s_rdm = { .uid = { 0x7F, 0xF0 }, .addr = 1, .footprint = 24, .model_id = 0x0001, .label = "Tira del banco",
                           .model = "P4OS bench strip, 8 RGB pixels", .maker = "P4OS", .version = "1.0" };
static uint8_t s_rdm_req[260];
static volatile int s_rdm_len;
static TaskHandle_t s_rdm_task;
static volatile uint32_t s_rdm_answers;

void dmx_addr_step(int d)
{
    s_addr += d;
    if (s_addr < 1) s_addr = 1;
    if (s_addr > 512 - 23) s_addr = 512 - 23;
    s_rdm.addr = (uint16_t)s_addr;
    printf("dmx light: address %d (pixels on %d-%d)\n", s_addr, s_addr, s_addr + 23);
}

static int64_t wait_level(int level, int64_t from)
{
    while ((int)gpio_ll_get_level(&GPIO, PIN_RX) != level)
        if (esp_timer_get_time() - from > 3000) return -1;
    return esp_timer_get_time();
}

static void rx_isr(void *arg)
{
    (void)arg;
    uart_dev_t *hw = R.hw;
    for (int guard = 0; guard < 16; guard++) {
        uint32_t st = uart_ll_get_intsts_mask(hw);
        if (!st) break;
        if (st & (UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT)) {
            uint8_t tmp[128];
            uint32_t n = uart_ll_get_rxfifo_len(hw);
            if (n > sizeof tmp) n = sizeof tmp;
            uart_ll_read_rxfifo(hw, tmp, n);
            for (uint32_t i = 0; i < n; i++) {
                if (R.cur_len < 514) R.cur[R.cur_len++] = tmp[i];
                else R.over = true;
            }
            uart_ll_clr_intsts_mask(hw, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT);
            /* RDM: answer as soon as the request is whole, not at the next BREAK */
            if (R.cur_len >= 3 && R.cur[0] == 0xCC && R.cur_len == R.cur[2] + 2 && !s_rdm_len && s_rdm_task) {
                memcpy(s_rdm_req, R.cur, (size_t)R.cur_len);
                s_rdm_len = R.cur_len;
                BaseType_t hp = pdFALSE;
                vTaskNotifyGiveFromISR(s_rdm_task, &hp);
                if (hp) portYIELD_FROM_ISR();
            }
        }
        if (st & UART_INTR_FRAM_ERR) { R.fe_n++; R.fe_at = R.cur_len; uart_ll_clr_intsts_mask(hw, UART_INTR_FRAM_ERR); }
        if (st & UART_INTR_RXFIFO_OVF) { uart_ll_rxfifo_rst(hw); R.over = true; uart_ll_clr_intsts_mask(hw, UART_INTR_RXFIFO_OVF); }
        if (st & UART_INTR_BRK_DET) {
            int64_t t_det = esp_timer_get_time();
            uart_ll_rxfifo_rst(hw);
            uart_ll_clr_intsts_mask(hw, UART_INTR_BRK_DET | UART_INTR_FRAM_ERR | UART_INTR_RXFIFO_FULL);
            if (R.fe_n && R.fe_at == R.cur_len && R.cur_len && R.cur[R.cur_len - 1] == 0) { R.cur_len--; R.fe_n--; }
            if (R.cur_len && R.cur[0] == 0) {         /* levels; an RDM packet (0xCC) is not for the strip */
                portENTER_CRITICAL_ISR(&R.mux);
                int n = R.cur_len > 513 ? 513 : R.cur_len;
                memcpy(R.done, R.cur, n);
                R.done_len = n;
                R.packets++;
                if (R.fe_n || R.over) R.bad++;
                R.last_us = t_det;
                portEXIT_CRITICAL_ISR(&R.mux);
            }
            R.cur_len = 0;
            R.fe_n = 0;
            R.fe_at = -1;
            R.over = false;
            int64_t t_rise = wait_level(1, t_det);
            int64_t t_fall = t_rise > 0 ? wait_level(0, t_rise) : -1;
            R.brk = t_rise > 0 ? (uint16_t)(t_rise - t_det + 40) : 0;
            R.mab = t_fall > 0 ? (uint16_t)(t_fall - t_rise) : 0;
        }
        uart_ll_clr_intsts_mask(hw, st & ~(UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT | UART_INTR_FRAM_ERR |
                                           UART_INTR_RXFIFO_OVF | UART_INTR_BRK_DET));
    }
}

/* The answers: with the receive interrupt off while the bytes go out (the
 * module would hand them back), a BREAK and MAB first unless it is a
 * discovery answer, straight into the FIFO (no driver in this role). */
static void rdm_task(void *arg)
{
    (void)arg;
    uint8_t out[64];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int len = s_rdm_len;
        bool brk;
        int n = rdm_handle(&s_rdm, s_rdm_req, len, out, &brk);
        if (n > 0) {
            esp_rom_delay_us(200);                  /* at least 176 us after the request */
            uart_ll_disable_intr_mask(R.hw, UART_INTR_RXFIFO_FULL | UART_INTR_FRAM_ERR | UART_INTR_RXFIFO_OVF | UART_INTR_BRK_DET);
            if (brk) {
                uart_set_line_inverse(BUS, UART_SIGNAL_TXD_INV);
                esp_rom_delay_us(176);
                uart_set_line_inverse(BUS, 0);
                esp_rom_delay_us(16);
            }
            uart_ll_write_txfifo(R.hw, out, (uint32_t)n);
            int64_t t0 = esp_timer_get_time();
            while (!uart_ll_is_tx_idle(R.hw) && esp_timer_get_time() - t0 < 10000) {}
            esp_rom_delay_us(100);
            uart_ll_rxfifo_rst(R.hw);
            uart_ll_clr_intsts_mask(R.hw, UINT32_MAX);
            R.cur_len = 0;
            uart_ll_ena_intr_mask(R.hw, UART_INTR_RXFIFO_FULL | UART_INTR_FRAM_ERR | UART_INTR_RXFIFO_OVF | UART_INTR_BRK_DET);
            s_rdm_answers++;
        }
        if (s_rdm.changed) {
            s_rdm.changed = false;
            if (s_rdm.addr != s_addr) { s_addr = s_rdm.addr; printf("rdm: address set to %d\n", s_addr); }
            printf("rdm: identify %s, label \"%s\"\n", s_rdm.identify ? "on" : "off", s_rdm.label);
        }
        s_rdm_len = 0;
    }
}

static void light_setup(void)
{
    uart_config_t c = {
        .baud_rate = 250000, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(BUS, &c);
    /* TX on the UART too, for the RDM answers: idle high, so an automatic
     * module only drives the bus while an answer goes out */
    uart_set_pin(BUS, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_set_direction(PIN_DE, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_DE, 0);
    R.hw = UART_LL_GET_HW(BUS);
    R.fe_at = -1;
    if (!s_rdm_task) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        memcpy(s_rdm.uid + 2, mac + 2, 4);
        xTaskCreate(rdm_task, "rdm", 4096, NULL, 20, &s_rdm_task);
    }
    s_rdm.addr = (uint16_t)s_addr;
    s_rdm.muted = false;
    uart_ll_disable_intr_mask(R.hw, UINT32_MAX);
    uart_ll_set_rxfifo_full_thr(R.hw, 1);
    uart_ll_rxfifo_rst(R.hw);
    uart_ll_clr_intsts_mask(R.hw, UINT32_MAX);
    esp_intr_alloc(uart_periph_signal[BUS].irq, ESP_INTR_FLAG_LOWMED, rx_isr, NULL, &R.ih);
    uart_ll_ena_intr_mask(R.hw, UART_INTR_RXFIFO_FULL | UART_INTR_FRAM_ERR | UART_INTR_RXFIFO_OVF | UART_INTR_BRK_DET);
    printf("\ndmx light on UART2 RX IO%d, address %d, strip on IO%d; +/-/>: address; RDM UID %02X%02X:%02X%02X%02X%02X\n", PIN_RX, s_addr, PIN_PIX,
           s_rdm.uid[0], s_rdm.uid[1], s_rdm.uid[2], s_rdm.uid[3], s_rdm.uid[4], s_rdm.uid[5]);
}

void dmx_light_round(void)
{
    static int64_t t_print, t_show;
    static uint32_t last_packets;
    int64_t now = esp_timer_get_time();
    if (now - t_show < 20000) { vTaskDelay(2); return; }
    t_show = now;
    uint8_t u[513];
    int n;
    uint32_t packets, bad;
    uint16_t brk, mab;
    int64_t last;
    portENTER_CRITICAL(&R.mux);
    n = R.done_len;
    memcpy(u, R.done, n);
    packets = R.packets;
    bad = R.bad;
    brk = R.brk;
    mab = R.mab;
    last = R.last_us;
    portEXIT_CRITICAL(&R.mux);
    uint8_t rgb[24] = { 0 };
    bool live = last && now - last < 1000000;
    for (int i = 0; i < 24 && live; i++) rgb[i] = s_addr + i < n ? u[s_addr + i] : 0;
    if (s_rdm.identify) memset(rgb, (now / 250000) % 2 ? 255 : 0, sizeof rgb);    /* RDM identify: blink white */
    pixels_show(rgb);
    if (now - t_print > 1000000) {
        float hz = (float)(packets - last_packets) * 1e6f / (float)(now - t_print);
        last_packets = packets;
        t_print = now;
        if (!live) printf("dmx light: no signal (%" PRIu32 " packets so far)\n", packets);
        else printf("dmx light: %.1f packets/s, %d slots, start code %02X, BREAK %u us, MAB %u us, %" PRIu32 " packets, %" PRIu32 " bad; ch %d-%d: %u %u %u ...; %" PRIu32 " RDM answers\n",
                    hz, n - 1, u[0], brk, mab, packets, bad, s_addr, s_addr + 2, rgb[0], rgb[1], rgb[2], s_rdm_answers);
    }
}

/* ---- switching ---- */

static int s_mode = -1;     /* 0 console, 1 light, -1 off */

void dmx_stop(void)
{
    if (s_mode == 0) { uart_wait_tx_done(BUS, pdMS_TO_TICKS(50)); uart_driver_delete(BUS); }
    if (s_mode == 1) {
        uart_ll_disable_intr_mask(R.hw, UINT32_MAX);
        esp_intr_free(R.ih);
        R.ih = NULL;
    }
    if (s_mode >= 0) {
        uint8_t off[24] = { 0 };
        pixels_show(off);
        gpio_reset_pin(PIN_TX);
        gpio_reset_pin(PIN_DE);
    }
    s_mode = -1;
}

void dmx_start(bool console)
{
    dmx_stop();
    if (uart_is_driver_installed(BUS)) uart_driver_delete(BUS);
    pixels_init();
    if (console) console_setup();
    else light_setup();
    s_mode = console ? 0 : 1;
}
