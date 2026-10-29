/*
 * P4OS - Pulses and counts on the board (aos_io.h, aos_io_pulse.c).
 *
 * Edges: a GPIO interrupt on any edge writes the time (esp_timer, us) and
 * the level after it into a buffer. The handler and its buffer are in
 * internal RAM on purpose: an interrupt writes them, and the handler runs
 * with the flash's cache off when the service was installed for that. It
 * is a few hundred bytes, only while a sensor has the pin open.
 *
 * swd: bit-banged on the GPIO registers (gpio_ll), each packet inside a
 * critical section, so a preemption cannot stretch a clock in the middle
 * of one (a target does not mind slow clocks, but a WAIT retried forever
 * would hide it). SWDIO is input and output at once with its pull-up; the
 * direction is the output enable. The clock comes from a delay loop, not
 * a timer: khz is approximate. Nothing here is in internal RAM.
 *
 * shift_in: bit-banged, each clock's high time inside a critical section
 * (a microsecond with this core's interrupts off): an HX711 powers down
 * when its clock stays high more than 60 us, which a preemption in the
 * middle of a clock would do.
 *
 * count: a PCNT unit with its glitch filter for short debounces, and the
 * same GPIO interrupt as the edges, ignoring what comes closer than the
 * debounce to the last edge counted, for long ones (a reed switch).
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

/* the GPIO interrupt service, shared with whoever else installs it */
static bool isr_service(void)
{
    esp_err_t e = gpio_install_isr_service(0);
    if (e == ESP_OK || e == ESP_ERR_INVALID_STATE) return true;
    aos_hal_log("io", "GPIO interrupts: %s", esp_err_to_name(e));
    return false;
}

/* ---- edges ---- */

typedef struct {
    int               gpio;
    int               max;
    volatile int      n;
    volatile bool     armed;
    volatile int64_t  last_us;
    uint32_t          t[AOS_IO_EDGES_MAX];
    uint8_t           lv[AOS_IO_EDGES_MAX];
} edges_be_t;

static void IRAM_ATTR edge_isr(void *arg)
{
    edges_be_t *b = arg;
    if (!b->armed) return;
    int64_t now = esp_timer_get_time();
    int n = b->n;
    if (n < b->max) {
        b->t[n] = (uint32_t)now;
        b->lv[n] = (uint8_t)gpio_ll_get_level(&GPIO, (uint32_t)b->gpio);
        b->n = n + 1;
    }
    b->last_us = now;
}

bool aos_io_be_edges_open(aos_io_edges_t *e)
{
    if (!isr_service()) return false;
    edges_be_t *b = heap_caps_calloc(1, sizeof *b, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!b) return false;
    b->gpio = e->gpio;
    b->max = e->max;
    gpio_set_intr_type(e->gpio, GPIO_INTR_ANYEDGE);
    if (gpio_isr_handler_add(e->gpio, edge_isr, b) != ESP_OK) {
        aos_hal_log("io", "edges: no interrupt on GPIO%d", e->gpio);
        free(b);
        return false;
    }
    gpio_intr_disable(e->gpio);
    e->be = b;
    return true;
}

void aos_io_be_edges_arm(aos_io_edges_t *e)
{
    edges_be_t *b = e->be;
    gpio_intr_disable(e->gpio);
    b->armed = false;
    b->n = 0;
    b->last_us = 0;
    b->armed = true;
    gpio_set_intr_type(e->gpio, GPIO_INTR_ANYEDGE);
    gpio_intr_enable(e->gpio);
}

int aos_io_be_edges_wait(aos_io_edges_t *e, uint32_t idle_us, uint32_t timeout_ms)
{
    edges_be_t *b = e->be;
    int64_t t0 = esp_timer_get_time(), end = t0 + (int64_t)timeout_ms * 1000;
    for (;;) {
        int n = b->n;
        int64_t now = esp_timer_get_time();
        if (n >= b->max) break;
        if (n > 0 && idle_us && now - b->last_us > (int64_t)idle_us) break;
        if (now >= end) break;
        /* short waits spin, longer ones give the core back */
        if (end - now > 2000 && (!n || idle_us > 2000)) vTaskDelay(1);
        else esp_rom_delay_us(20);
    }
    gpio_intr_disable(e->gpio);
    b->armed = false;
    return b->n;
}

int aos_io_be_edges_get(aos_io_edges_t *e, uint32_t *t_us, uint8_t *level, int max)
{
    edges_be_t *b = e->be;
    int n = b->n < max ? b->n : max;
    for (int i = 0; i < n; i++) {
        t_us[i] = b->t[i] - b->t[0];
        level[i] = b->lv[i];
    }
    return n;
}

void aos_io_be_edges_close(aos_io_edges_t *e)
{
    edges_be_t *b = e->be;
    if (!b) return;
    gpio_intr_disable(e->gpio);
    gpio_isr_handler_remove(e->gpio);
    gpio_set_intr_type(e->gpio, GPIO_INTR_DISABLE);
    free(b);
    e->be = NULL;
}

/* ---- a pulse out, bits in ---- */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

bool aos_io_be_pulse_out(int gpio, int level, uint32_t us)
{
    gpio_set_level(gpio, level ? 1 : 0);
    esp_rom_delay_us(us);
    gpio_set_level(gpio, level ? 0 : 1);
    return true;
}

bool aos_io_be_shift_in(int data, int clk, int bits, int extra, uint32_t *out)
{
    uint32_t v = 0;
    gpio_set_level(clk, 0);
    for (int i = 0; i < bits + extra; i++) {
        portENTER_CRITICAL(&s_mux);
        gpio_ll_set_level(&GPIO, (uint32_t)clk, 1);
        esp_rom_delay_us(1);
        int d = gpio_ll_get_level(&GPIO, (uint32_t)data);
        gpio_ll_set_level(&GPIO, (uint32_t)clk, 0);
        portEXIT_CRITICAL(&s_mux);
        if (i < bits) v = v << 1 | (uint32_t)d;
        esp_rom_delay_us(1);
    }
    *out = v;
    return true;
}

/* ---- count ---- */

typedef struct {
    pcnt_unit_handle_t    unit;
    pcnt_channel_handle_t chan;
    /* the interrupt's count, for long debounces */
    int                   gpio;
    bool                  both;
    uint32_t              debounce_us;
    volatile uint32_t     n;
    volatile int64_t      last_us;
} count_be_t;

#define PCNT_LIMIT 30000

static void IRAM_ATTR count_isr(void *arg)
{
    count_be_t *b = arg;
    int64_t now = esp_timer_get_time();
    if (b->last_us && now - b->last_us < (int64_t)b->debounce_us) return;
    if (!b->both && !gpio_ll_get_level(&GPIO, (uint32_t)b->gpio)) return;
    b->last_us = now;
    b->n++;
}

static void count_free(count_be_t *b)
{
    if (b->unit) {
        pcnt_unit_stop(b->unit);
        pcnt_unit_disable(b->unit);
        if (b->chan) pcnt_del_channel(b->chan);
        pcnt_unit_remove_watch_point(b->unit, PCNT_LIMIT);
        pcnt_unit_remove_watch_point(b->unit, -PCNT_LIMIT);
        pcnt_del_unit(b->unit);
    }
    free(b);
}

bool aos_io_be_count_open(aos_io_count_t *c, aos_gpio_mode_t pull)
{
    count_be_t *b = heap_caps_calloc(1, sizeof *b, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!b) return false;
    b->gpio = c->gpio;
    b->both = c->both;
    b->debounce_us = c->debounce_us;
    gpio_config_t g = { .pin_bit_mask = 1ULL << c->gpio, .mode = GPIO_MODE_INPUT,
                        .pull_up_en = pull == AOS_GPIO_INPUT_PULLUP, .pull_down_en = pull == AOS_GPIO_INPUT_PULLDOWN };
    gpio_config(&g);
    if (c->debounce_us >= 10) {
        /* a long debounce: the interrupt counts */
        if (!isr_service()) { free(b); return false; }
        gpio_set_intr_type(c->gpio, c->both ? GPIO_INTR_ANYEDGE : GPIO_INTR_POSEDGE);
        if (gpio_isr_handler_add(c->gpio, count_isr, b) != ESP_OK) { free(b); return false; }
        gpio_intr_enable(c->gpio);
        c->be = b;
        return true;
    }
    pcnt_unit_config_t uc = { .low_limit = -PCNT_LIMIT, .high_limit = PCNT_LIMIT, .flags = { .accum_count = 1 } };
    esp_err_t e = pcnt_new_unit(&uc, &b->unit);
    if (e == ESP_OK) {
        pcnt_glitch_filter_config_t f = { .max_glitch_ns = c->debounce_us ? c->debounce_us * 1000 : 1000 };
        pcnt_unit_set_glitch_filter(b->unit, &f);
        pcnt_chan_config_t cc = { .edge_gpio_num = c->gpio, .level_gpio_num = -1 };
        e = pcnt_new_channel(b->unit, &cc, &b->chan);
    }
    if (e == ESP_OK) {
        pcnt_channel_set_edge_action(b->chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     c->both ? PCNT_CHANNEL_EDGE_ACTION_INCREASE : PCNT_CHANNEL_EDGE_ACTION_HOLD);
        pcnt_channel_set_level_action(b->chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP);
        pcnt_unit_add_watch_point(b->unit, PCNT_LIMIT);
        pcnt_unit_add_watch_point(b->unit, -PCNT_LIMIT);
        e = pcnt_unit_enable(b->unit);
    }
    if (e == ESP_OK) e = pcnt_unit_clear_count(b->unit);
    if (e == ESP_OK) e = pcnt_unit_start(b->unit);
    if (e != ESP_OK) {
        aos_hal_log("io", "count on GPIO%d: %s", c->gpio, esp_err_to_name(e));
        count_free(b);
        return false;
    }
    c->be = b;
    return true;
}

uint32_t aos_io_be_count_get(aos_io_count_t *c)
{
    count_be_t *b = c->be;
    if (!b->unit) return b->n;
    int v = 0;
    pcnt_unit_get_count(b->unit, &v);
    return (uint32_t)v;
}

void aos_io_be_count_close(aos_io_count_t *c)
{
    count_be_t *b = c->be;
    if (!b) return;
    if (!b->unit) {
        gpio_intr_disable(c->gpio);
        gpio_isr_handler_remove(c->gpio);
        gpio_set_intr_type(c->gpio, GPIO_INTR_DISABLE);
    }
    count_free(b);
    c->be = NULL;
}

/* ---- SWD ---- */

typedef struct { uint32_t dly; } swd_p4_t;

static inline void swd_wait(uint32_t n) { for (volatile uint32_t i = n; i; i--) {} }

/* host drives: the target samples on the rising edge */
static inline void swd_out(aos_io_swd_t *s, uint32_t dly, int bit)
{
    gpio_ll_set_level(&GPIO, (uint32_t)s->dio, (uint32_t)bit);
    gpio_ll_set_level(&GPIO, (uint32_t)s->clk, 0);
    swd_wait(dly);
    gpio_ll_set_level(&GPIO, (uint32_t)s->clk, 1);
    swd_wait(dly);
}

/* target drives: it changes on the rising edge, read before it */
static inline int swd_in(aos_io_swd_t *s, uint32_t dly)
{
    gpio_ll_set_level(&GPIO, (uint32_t)s->clk, 0);
    swd_wait(dly);
    int b = gpio_ll_get_level(&GPIO, (uint32_t)s->dio);
    gpio_ll_set_level(&GPIO, (uint32_t)s->clk, 1);
    swd_wait(dly);
    return b;
}

bool aos_io_be_swd_open(aos_io_swd_t *s)
{
    swd_p4_t *b = calloc(1, sizeof *b);
    if (!b) return false;
    /* ~5 cycles a turn of the loop at 360 MHz, two waits a clock */
    b->dly = 360000u / (s->khz * 10u);
    if (!b->dly) b->dly = 1;
    gpio_config_t c = { .pin_bit_mask = 1ULL << s->clk, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&c);
    gpio_set_level(s->clk, 1);
    c.pin_bit_mask = 1ULL << s->dio;
    c.mode = GPIO_MODE_INPUT_OUTPUT;
    c.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&c);
    gpio_set_level(s->dio, 1);
    if (s->nrst >= 0) {
        c.pin_bit_mask = 1ULL << s->nrst;
        c.mode = GPIO_MODE_INPUT_OUTPUT_OD;
        gpio_config(&c);
        gpio_set_level(s->nrst, 1);
    }
    s->be = b;
    return true;
}

bool aos_io_be_swd_seq(aos_io_swd_t *s, const uint8_t *bits, int n)
{
    uint32_t dly = ((swd_p4_t *)s->be)->dly;
    gpio_ll_output_enable(&GPIO, (uint32_t)s->dio);
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) swd_out(s, dly, bits[i >> 3] >> (i & 7) & 1);
    portEXIT_CRITICAL(&s_mux);
    return true;
}

int aos_io_be_swd_xfer(aos_io_swd_t *s, uint8_t req, uint32_t *data, bool no_ack)
{
    uint32_t dly = ((swd_p4_t *)s->be)->dly;
    bool rd = req & 0x04;
    int ack = 0, ret;
    portENTER_CRITICAL(&s_mux);
    gpio_ll_output_enable(&GPIO, (uint32_t)s->dio);
    for (int i = 0; i < 8; i++) swd_out(s, dly, req >> i & 1);
    gpio_ll_output_disable(&GPIO, (uint32_t)s->dio);
    swd_in(s, dly);                                         /* turnaround */
    for (int i = 0; i < 3; i++) ack |= swd_in(s, dly) << i;
    if (no_ack && !rd) ack = 1;
    if (ack == 1 && rd) {
        uint32_t v = 0;
        int par = 0;
        for (int i = 0; i < 32; i++) {
            int b = swd_in(s, dly);
            v |= (uint32_t)b << i;
            par ^= b;
        }
        int p = swd_in(s, dly);
        swd_in(s, dly);                                     /* turnaround */
        gpio_ll_output_enable(&GPIO, (uint32_t)s->dio);
        *data = v;
        ret = p == par ? 1 : -1;
    } else if (ack == 1) {
        swd_in(s, dly);                                     /* turnaround */
        gpio_ll_output_enable(&GPIO, (uint32_t)s->dio);
        int par = 0;
        for (int i = 0; i < 32; i++) {
            int b = (int)(*data >> i & 1);
            swd_out(s, dly, b);
            par ^= b;
        }
        swd_out(s, dly, par);
        ret = 1;
    } else if (ack == 2 || ack == 4) {
        swd_in(s, dly);
        gpio_ll_output_enable(&GPIO, (uint32_t)s->dio);
        ret = ack;
    } else {
        /* nobody, or garbage: let a data phase go by, then take the line */
        for (int i = 0; i < 34; i++) swd_in(s, dly);
        gpio_ll_output_enable(&GPIO, (uint32_t)s->dio);
        ret = ack ? ack : 7;
    }
    for (int i = 0; i < 2; i++) swd_out(s, dly, 0);         /* idle */
    portEXIT_CRITICAL(&s_mux);
    return ret;
}

bool aos_io_be_swd_nrst(aos_io_swd_t *s, bool assert)
{
    return gpio_set_level(s->nrst, assert ? 0 : 1) == ESP_OK;
}

void aos_io_be_swd_close(aos_io_swd_t *s)
{
    /* the pins back to inputs: the target keeps running on its own */
    gpio_reset_pin(s->clk);
    gpio_reset_pin(s->dio);
    if (s->nrst >= 0) gpio_reset_pin(s->nrst);
    free(s->be);
    s->be = NULL;
}
