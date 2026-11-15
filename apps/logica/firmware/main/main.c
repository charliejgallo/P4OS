/*
 * P4OS Logic - an eight-channel logic analyser for an ESP32-S3, C3 or C6,
 * speaking SUMP (the Open Bench Logic Sniffer's protocol) over the chip's
 * own USB or UART0. The P4OS board's Logic app drives it; so do PulseView
 * and sigrok-cli ("Openbench Logic Sniffer & SUMP compatibles").
 *
 * How it samples: the eight GPIOs are a dedicated-GPIO bundle, which the
 * CPU reads in one instruction (a CSR on the RISC-V chips, ee.get_gpio_in
 * on the S3). The capture loop runs from IRAM with the interrupts off,
 * paced by the CPU's cycle counter: each sample waits for its cycle, so
 * the rate is exact on average and jitters by a few cycles. Faster than
 * the loop can go it cannot: the firmware measures its own loop at boot
 * and says its top rate in the metadata (counted from the compiled loop,
 * not yet measured on a chip: about 4 MHz on a C3 at 160 MHz, 6-8 MHz on
 * an S3 at 240 MHz). The samples go to internal RAM, one byte each: what is
 * free after boot (some 300K samples on an S3, 200K on a C3).
 *
 * The trigger is SUMP's stage 0 (a mask and values on the levels) and,
 * from the P4OS app (command 0xF0), rising, falling and either edges too. Before
 * it fires, the loop fills a ring with the samples SUMP asks to keep from
 * before it; after, the rest. The samples go back newest first, as an OLS
 * sends them.
 *
 * On the S3 the capture runs on the second core, so the first one keeps
 * the USB and hears a reset; on the single-core chips a byte arriving at
 * the link's FIFO stops the wait for a trigger.
 *
 * Inputs are 3.3 V only, with no pull resistors: an unconnected one reads
 * whatever it floats to.
 */
#include <stdio.h>
#include <string.h>

#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_clk_tree.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if CONFIG_LA_LINK_USB_JTAG
#include "hal/usb_serial_jtag_ll.h"
#else
#include "hal/uart_ll.h"
#endif

#define FW_VERSION "0.3"
#define SUMP_CLOCK 100000000u
#define NCH 8

#if CONFIG_IDF_TARGET_ESP32S3
#define CHIP "ESP32-S3"
#elif CONFIG_IDF_TARGET_ESP32C3
#define CHIP "ESP32-C3"
#elif CONFIG_IDF_TARGET_ESP32C6
#define CHIP "ESP32-C6"
#else
#define CHIP CONFIG_IDF_TARGET
#endif

#if SOC_CPU_CORES_NUM > 1 && !CONFIG_FREERTOS_UNICORE
#define TWO_CORES 1
#else
#define TWO_CORES 0
#endif

static const int PINS[NCH] = { CONFIG_LA_PIN0, CONFIG_LA_PIN1, CONFIG_LA_PIN2, CONFIG_LA_PIN3,
                               CONFIG_LA_PIN4, CONFIG_LA_PIN5, CONFIG_LA_PIN6, CONFIG_LA_PIN7 };

static uint8_t *s_buf;
static uint32_t s_mem;                  /* samples the buffer holds */
static uint32_t s_cpu_hz;
static uint32_t s_max_rate;

/* what the host set */
static struct {
    uint8_t mask, val, rise, fall, edge;
    bool start;
    uint32_t div;
    uint32_t read, delay;               /* samples */
    uint32_t flags;
} S;

static volatile bool s_abort;
/* measuring the loop: blocks of 4096 samples left to run, 0 = a capture */
static volatile uint32_t s_calib;

/* ---- the link ---- */

static void link_init(void)
{
#if CONFIG_LA_LINK_USB_JTAG
    usb_serial_jtag_driver_config_t c = { .rx_buffer_size = 2048, .tx_buffer_size = 4096 };
    usb_serial_jtag_driver_install(&c);
#else
    uart_config_t c = { .baud_rate = CONFIG_LA_UART_BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    uart_driver_install(UART_NUM_0, 2048, 4096, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &c);
#endif
}

static int link_read(uint8_t *p, int n, TickType_t wait)
{
#if CONFIG_LA_LINK_USB_JTAG
    return usb_serial_jtag_read_bytes(p, n, wait);
#else
    return uart_read_bytes(UART_NUM_0, p, n, wait);
#endif
}

static void link_write(const void *p, int n)
{
    const uint8_t *b = p;
    while (n > 0) {
#if CONFIG_LA_LINK_USB_JTAG
        int w = usb_serial_jtag_write_bytes(b, n, pdMS_TO_TICKS(1000));
#else
        int w = uart_write_bytes(UART_NUM_0, b, n);
#endif
        if (w <= 0) return;             /* nobody listens: drop it */
        b += w;
        n -= w;
    }
}

/* with the interrupts off, a byte from the host waits in the hardware FIFO */
static IRAM_ATTR inline bool host_spoke(void)
{
#if TWO_CORES
    return s_abort;
#elif CONFIG_LA_LINK_USB_JTAG
    return usb_serial_jtag_ll_rxfifo_data_available() != 0;
#else
    return uart_ll_get_rxfifo_len(UART_LL_GET_HW(0)) != 0;
#endif
}

/* ---- capturing ---- */

#define RD() ((uint8_t)dedic_gpio_cpu_ll_read_in())

typedef struct {
    uint32_t period, rem, rate;         /* cycles per sample = period + rem/rate */
    uint32_t n, pre;
    uint8_t lm, lv, rise, fall, edge;
    bool trig;
    uint32_t end;                       /* out: where the newest sample is, + 1 */
    bool aborted;
} cap_t;

/* noinline: inlined into its task it would leave IRAM for the flash, and a
 * cache miss in the loop is a hole in the sampling */
static IRAM_ATTR __attribute__((noinline)) void capture(cap_t *c)
{
    uint8_t *buf = s_buf;
    const uint32_t n = c->n, period = c->period, rem = c->rem, rate = c->rate;
    uint32_t idx = 0, filled = 0, frac = 0, check = 4096;
    uint8_t prev = RD();
    uint32_t t = esp_cpu_get_cycle_count() + period;
    c->aborted = false;
    if (c->trig) {
        const uint8_t lm = c->lm, lv = c->lv, rise = c->rise, fall = c->fall, edge = c->edge;
        const uint32_t pre = c->pre;
        for (;;) {
            while ((int32_t)(esp_cpu_get_cycle_count() - t) < 0) {}
            t += period;
            frac += rem;
            if (frac >= rate) {
                frac -= rate;
                t++;
            }
            uint8_t s = RD();
            buf[idx] = s;
            if (++idx == n) idx = 0;
            if (filled < pre) filled++;
            else if ((s & lm) == lv && (rise & ~prev & s) == rise && (fall & prev & ~s) == fall &&
                     (edge & (prev ^ s)) == edge) break;
            prev = s;
            if (!--check) {
                check = 4096;
                if (s_calib && !--s_calib) {
                    c->aborted = true;
                    return;
                }
                if (host_spoke()) {
                    c->aborted = true;
                    return;
                }
                /* the wait was longer than the loop: start the pace again */
                if ((int32_t)(esp_cpu_get_cycle_count() - t) > (int32_t)(period * 64)) t = esp_cpu_get_cycle_count();
            }
        }
    }
    /* after the trigger (or with none), the rest. Should the wait have
     * fallen behind its pace, the pace starts again here rather than
     * running to catch up: catching up takes samples closer together than
     * their rate, and the first stretch after the trigger would be read
     * out of time (seen on a C3: the first 15 characters of a UART line) */
    if ((int32_t)(esp_cpu_get_cycle_count() - t) > (int32_t)period) t = esp_cpu_get_cycle_count() + period;
    for (uint32_t k = c->trig ? n - c->pre - 1 : n; k; k--) {
        while ((int32_t)(esp_cpu_get_cycle_count() - t) < 0) {}
        t += period;
        frac += rem;
        if (frac >= rate) {
            frac -= rate;
            t++;
        }
        buf[idx] = RD();
        if (++idx == n) idx = 0;
    }
    c->end = idx;
}

/* The loop's own speed: the capture itself, unpaced, waiting for a trigger
 * that never comes and checking it on every sample (its slowest path), for
 * 16384 samples. (A lighter copy of the loop measured 8.33 MHz on a C3 that
 * the real one could not keep: it fell behind while waiting, and the start
 * after the trigger came out of time.) */
static uint32_t loop_cycles(void)
{
    cap_t c = { 0 };
    c.period = 1;
    c.rate = 1;
    c.n = 1024;
    c.pre = 0;
    c.trig = true;
    c.lm = 0;
    c.lv = 1;                   /* (s & 0) == 1: never */
    s_calib = 4;
    uint32_t t0 = esp_cpu_get_cycle_count();
    capture(&c);
    uint32_t dt = esp_cpu_get_cycle_count() - t0;
    s_calib = 0;
    return dt / (4 * 4096);
}

typedef struct {
    cap_t c;
    TaskHandle_t waiter;
} job_t;

static IRAM_ATTR void capture_task(void *arg)
{
    job_t *j = arg;
    portDISABLE_INTERRUPTS();
    capture(&j->c);
    portENABLE_INTERRUPTS();
    xTaskNotifyGive(j->waiter);
    vTaskDelete(NULL);
}

static void run(void)
{
    uint32_t rate = SUMP_CLOCK / (S.div + 1);
    if (rate > s_max_rate) rate = s_max_rate;
    if (!rate) rate = 1;
    uint32_t n = S.read ? S.read : 4;
    if (n > s_mem) n = s_mem;
    uint32_t delay = S.delay > n ? n : S.delay;
    job_t j = { 0 };
    j.c.period = s_cpu_hz / rate;
    j.c.rem = s_cpu_hz % rate;
    j.c.rate = rate;
    j.c.n = n;
    j.c.trig = S.start && (S.mask || S.rise || S.fall || S.edge);
    j.c.pre = j.c.trig ? n - (delay ? delay : 1) : 0;
    j.c.lm = S.mask;
    j.c.lv = S.val & S.mask;
    j.c.rise = S.rise;
    j.c.fall = S.fall;
    j.c.edge = S.edge;
    j.waiter = xTaskGetCurrentTaskHandle();
    s_abort = false;
#if TWO_CORES
    xTaskCreatePinnedToCore(capture_task, "capture", 4096, &j, configMAX_PRIORITIES - 1, NULL, 1);
    /* meanwhile this core keeps the link: any byte from the host stops it */
    while (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20))) {
        uint8_t b;
        if (link_read(&b, 1, 0) == 1) s_abort = true;
    }
#else
    xTaskCreate(capture_task, "capture", 4096, &j, configMAX_PRIORITIES - 1, NULL);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#endif
    if (j.c.aborted) return;
    /* newest first; as many bytes per sample as groups are on */
    int groups = 0;
    for (int g = 0; g < 4; g++)
        if (!(S.flags & (1u << (2 + g)))) groups++;
    if (!groups) groups = 1;
    uint8_t out[256];
    int k = 0;
    uint32_t i = j.c.end;
    for (uint32_t m = 0; m < n; m++) {
        i = i ? i - 1 : n - 1;
        out[k++] = s_buf[i];
        for (int g = 1; g < groups; g++) out[k++] = 0;
        if (k > (int)sizeof out - 4) {
            link_write(out, k);
            k = 0;
        }
    }
    if (k) link_write(out, k);
}

/* ---- SUMP ---- */

static void put_meta_str(uint8_t key, const char *s)
{
    link_write(&key, 1);
    link_write(s, (int)strlen(s) + 1);
}

static void put_meta_u32(uint8_t key, uint32_t v)
{
    uint8_t b[5] = { key, (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    link_write(b, 5);
}

static void metadata(void)
{
    put_meta_str(0x01, "P4OS LA " CHIP);
    put_meta_str(0x02, FW_VERSION);
    put_meta_u32(0x20, NCH);
    put_meta_u32(0x21, s_mem);
    put_meta_u32(0x23, s_max_rate);
    put_meta_u32(0x24, 2);
    uint8_t z = 0;
    link_write(&z, 1);
}

static void reset_state(void)
{
    memset(&S, 0, sizeof S);
    S.div = SUMP_CLOCK / 1000000 - 1;
    S.read = S.delay = 4096;
    S.flags = 0x38;
}

static void long_cmd(uint8_t c, uint32_t v)
{
    switch (c) {
    case 0xC0: S.mask = (uint8_t)v; break;
    case 0xC1: S.val = (uint8_t)v; break;
    case 0xC2: S.start = (v >> 27) & 1; break;
    case 0x80: S.div = v & 0xFFFFFF; break;
    case 0x81:
        S.read = ((v & 0xFFFF) + 1) * 4;
        S.delay = ((v >> 16) + 1) * 4;
        break;
    case 0x82: S.flags = v; break;
    case 0x83: S.delay = (v + 1) * 4; break;
    case 0x84: S.read = (v + 1) * 4; break;
    case 0xF0:                          /* P4OS: edges, rising, falling and either */
        S.rise = (uint8_t)v;
        S.fall = (uint8_t)(v >> 8);
        S.edge = (uint8_t)(v >> 16);
        break;
    default: break;                     /* other stages, and what we do not know */
    }
}

void app_main(void)
{
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &s_cpu_hz);
    dedic_gpio_bundle_handle_t bundle = NULL;
    dedic_gpio_bundle_config_t bc = { .gpio_array = PINS, .array_size = NCH, .flags = { .in_en = 1 } };
    for (int i = 0; i < NCH; i++) {
        gpio_reset_pin(PINS[i]);
        gpio_set_direction(PINS[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(PINS[i], GPIO_FLOATING);
    }
    dedic_gpio_new_bundle(&bc, &bundle);

    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_mem = big > 48 * 1024 ? (uint32_t)(big - 32 * 1024) & ~3u : 4096;
    s_buf = heap_caps_malloc(s_mem, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_buf) {
        s_mem = 4096;
        s_buf = heap_caps_malloc(s_mem, MALLOC_CAP_8BIT);
    }
    /* the top rate: a sample every loop and a bit, as SUMP can say it */
    portDISABLE_INTERRUPTS();
    uint32_t cyc = loop_cycles();
    portENABLE_INTERRUPTS();
    uint32_t top = (uint32_t)((uint64_t)s_cpu_hz * 10 / ((uint64_t)cyc * 11));    /* 10 % to spare */
    uint32_t d = SUMP_CLOCK / top;
    if (d < 1) d = 1;
    while (SUMP_CLOCK / d > top) d++;
    s_max_rate = SUMP_CLOCK / d;

    reset_state();
    link_init();
    for (;;) {
        uint8_t c;
        if (link_read(&c, 1, portMAX_DELAY) != 1) continue;
        if (c & 0x80) {
            uint8_t b[4];
            int got = 0;
            while (got < 4) {
                int r = link_read(b + got, 4 - got, pdMS_TO_TICKS(100));
                if (r <= 0) break;
                got += r;
            }
            if (got == 4) long_cmd(c, (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
            continue;
        }
        switch (c) {
        case 0x00: reset_state(); break;
        case 0x01: run(); break;
        case 0x02: link_write("1ALS", 4); break;
        case 0x04: metadata(); break;
        default: break;                 /* XON, XOFF, the rest */
        }
    }
}
