/*
 * P4OS - aos_io backend on the ESP32-P4 (ESP-IDF drivers).
 *
 * UARTs come from a pool (UART0 is the console on GPIO37/38 and never
 * offered); I2C ports get their own master bus, except "i2c.board", which
 * is the BSP's bus shared with the touch and the codecs. Devices on a bus
 * are added on first use and kept. SPI ports take one of the two general
 * purpose hosts, GPSPI2 or GPSPI3: nothing else in the firmware uses them
 * (the panel is MIPI-DSI, the C6 is on SDIO, the PPP link on a UART).
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <string.h>
#include <stdlib.h>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bsp/esp-bsp.h"

/* ---- UART ---- */

static bool s_uart_used[SOC_UART_HP_NUM];

bool aos_io_be_uart_open(aos_io_uart_t *u, bool rs485)
{
    int num = -1;
    for (int i = 1; i < SOC_UART_HP_NUM; i++) if (!s_uart_used[i]) { num = i; break; }
    if (num < 0) { aos_hal_log("io", "no UART left for %s", u->port->name); return false; }
    uart_config_t c = {
        .baud_rate = (int)u->baud, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(num, 8192, 2048, 0, NULL, 0) != ESP_OK) return false;
    uart_param_config(num, &c);
    int de = rs485 ? u->port->pins[2] : UART_PIN_NO_CHANGE;
    uart_set_pin(num, u->port->pins[0], u->port->pins[1], de, UART_PIN_NO_CHANGE);
    if (rs485) uart_set_mode(num, UART_MODE_RS485_HALF_DUPLEX);
    s_uart_used[num] = true;
    u->be = (void *)(intptr_t)num;
    return true;
}

int aos_io_be_uart_read(aos_io_uart_t *u, void *buf, int len, int timeout_ms)
{
    return uart_read_bytes((int)(intptr_t)u->be, buf, len, pdMS_TO_TICKS(timeout_ms));
}

int aos_io_be_uart_write(aos_io_uart_t *u, const void *buf, int len)
{
    return uart_write_bytes((int)(intptr_t)u->be, buf, len);
}

bool aos_io_be_uart_set_baud(aos_io_uart_t *u, uint32_t baud)
{
    return uart_set_baudrate((int)(intptr_t)u->be, baud) == ESP_OK;
}

bool aos_io_be_uart_set_format(aos_io_uart_t *u, char parity, int stop_bits)
{
    int num = (int)(intptr_t)u->be;
    uart_parity_t p = parity == 'E' ? UART_PARITY_EVEN : parity == 'O' ? UART_PARITY_ODD : UART_PARITY_DISABLE;
    return uart_set_parity(num, p) == ESP_OK &&
           uart_set_stop_bits(num, stop_bits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1) == ESP_OK;
}

void aos_io_be_uart_close(aos_io_uart_t *u)
{
    int num = (int)(intptr_t)u->be;
    uart_driver_delete(num);
    s_uart_used[num] = false;
    gpio_reset_pin(u->port->pins[0]);
    gpio_reset_pin(u->port->pins[1]);
}

/* The target's EN and BOOT (aos_io_uart_lines). Open-drain with the pull-up
 * on: 0 pulls the line down, 1 lets it go and the target's own pull-up (the
 * 10k on EN, the strapping pull-up on GPIO0) takes it high. Nothing fights
 * the target's buttons or an auto-reset circuit on it, and a 3V3 target
 * never sees the P4 drive it high. BOOT goes first so that an EN released
 * in the same call finds it already set. */
static void line_setup(int gpio)
{
    gpio_set_level(gpio, 1);
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&c);
    gpio_set_level(gpio, 1);
}

bool aos_io_be_uart_lines(aos_io_uart_t *u, int en, int boot)
{
    if (boot >= 0) {
        if (u->boot < 0) line_setup(u->boot_gpio);
        if (gpio_set_level(u->boot_gpio, boot) != ESP_OK) return false;
    }
    if (en >= 0) {
        if (u->en < 0) line_setup(u->en_gpio);
        if (gpio_set_level(u->en_gpio, en) != ESP_OK) return false;
    }
    return true;
}

void aos_io_be_uart_lines_release(aos_io_uart_t *u)
{
    /* EN last: the target comes out of reset with BOOT already let go */
    if (u->boot_gpio >= 0 && u->boot >= 0) gpio_reset_pin(u->boot_gpio);
    if (u->en_gpio >= 0 && u->en >= 0) gpio_reset_pin(u->en_gpio);
}

/* ---- I2C ---- */

#define DEVS 8
typedef struct {
    i2c_master_bus_handle_t bus;
    bool own;
    uint8_t addr[DEVS];
    i2c_master_dev_handle_t dev[DEVS];
    int ndev;
} i2c_be_t;

bool aos_io_be_i2c_open(aos_io_i2c_t *b)
{
    i2c_be_t *s = calloc(1, sizeof *s);
    if (!strcmp(b->port->name, "i2c.board")) {
        bsp_i2c_init();
        s->bus = bsp_i2c_get_handle();
    } else {
        i2c_master_bus_config_t c = {
            .i2c_port = -1,                     /* any free controller */
            .sda_io_num = b->port->pins[0], .scl_io_num = b->port->pins[1],
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&c, &s->bus) != ESP_OK) { free(s); return false; }
        s->own = true;
    }
    b->be = s;
    return s->bus != NULL;
}

static i2c_master_dev_handle_t dev_for(aos_io_i2c_t *b, uint8_t addr)
{
    i2c_be_t *s = b->be;
    for (int i = 0; i < s->ndev; i++) if (s->addr[i] == addr) return s->dev[i];
    if (s->ndev >= DEVS) return NULL;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr,
                               .scl_speed_hz = b->port->freq ? b->port->freq : 100000 };
    if (i2c_master_bus_add_device(s->bus, &dc, &s->dev[s->ndev]) != ESP_OK) return NULL;
    s->addr[s->ndev] = addr;
    return s->dev[s->ndev++];
}

bool aos_io_be_i2c_probe(aos_io_i2c_t *b, uint8_t addr)
{
    return i2c_master_probe(((i2c_be_t *)b->be)->bus, addr, 30) == ESP_OK;
}

bool aos_io_be_i2c_xfer(aos_io_i2c_t *b, uint8_t addr, const void *w, size_t wn, void *r, size_t rn, int timeout_ms)
{
    i2c_master_dev_handle_t d = dev_for(b, addr);
    if (!d) return false;
    if (wn && rn) return i2c_master_transmit_receive(d, w, wn, r, rn, timeout_ms) == ESP_OK;
    if (wn) return i2c_master_transmit(d, w, wn, timeout_ms) == ESP_OK;
    return i2c_master_receive(d, r, rn, timeout_ms) == ESP_OK;
}

void aos_io_be_i2c_close(aos_io_i2c_t *b)
{
    i2c_be_t *s = b->be;
    if (!s) return;
    for (int i = 0; i < s->ndev; i++) i2c_master_bus_rm_device(s->dev[i]);
    if (s->own) i2c_del_master_bus(s->bus);
    free(s);
    b->be = NULL;
}

/* ---- SPI ----
 *
 * One host per port, brought up by the port's first handle and taken down
 * by its last; each handle is a device on it with its own CS, mode and
 * clock. The pins go through the GPIO matrix (the header's GPIOs are not
 * the hosts' IO_MUX pins), which aos_io.c holds to 40 MHz.
 *
 * Buffers. GPSPI DMA on the P4 can read and write PSRAM directly
 * (SPI_TRANS_DMA_USE_PSRAM), but it then shares the PSRAM bandwidth with
 * the panel's framebuffers, and the IDF documents what happens when it
 * loses: bytes silently missing, flagged after the fact. So the transfers
 * go through two small bounce buffers in internal DMA memory instead - 2 KB
 * out and 2 KB in per open port, allocated when the bus comes up and freed
 * with it, cache-line aligned so the driver can hand them to the DMA as
 * they are. A longer transfer goes in 2 KB pieces with CS held low between
 * them (SPI_TRANS_CS_KEEP_ACTIVE under spi_device_acquire_bus()), so it is
 * still one assertion for the chip. This is the RAM-inside rule's
 * exception, written down: 4 KB, only while a port is open.
 *
 * What the driver still copies: it wants the length of an RX buffer to be
 * a whole number of cache lines too, and makes an aligned copy when it is
 * not. Pieces are cut at a 64-byte boundary so that only the last few bytes
 * of a transfer (under 64) ever take that path - a 64-byte allocation for
 * the length of one transaction, as every SPI user on the P4 pays.
 *
 * Short transfers are polled (spi_device_polling_transmit, no interrupt or
 * context switch: a register read is a few microseconds); one that would
 * keep the bus busy for more than SPI_POLL_US goes through the interrupt
 * path and the task sleeps meanwhile. */

#define SPI_CHUNK    2048
#ifdef CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define SPI_ALIGN    CONFIG_CACHE_L1_CACHE_LINE_SIZE
#else
#define SPI_ALIGN    4
#endif
#define SPI_POLL_US  100

typedef struct {
    spi_host_device_t host;
    char              port[AOS_IO_PORT_NAME_MAX];    /* "": free */
    int8_t            pins[4];
    SemaphoreHandle_t lock;                          /* one transfer at a time on the wires */
    uint8_t          *btx, *brx;                     /* the bounce buffers */
} spi_bus_t;

static spi_bus_t s_spi_bus[2];

typedef struct {
    spi_bus_t          *bus;
    spi_device_handle_t dev;
} spi_be_t;

static spi_bus_t *bus_up(const aos_io_port_t *p)
{
    spi_bus_t *b = NULL;
    for (int i = 0; i < 2; i++) if (!s_spi_bus[i].port[0]) { b = &s_spi_bus[i]; b->host = i ? SPI3_HOST : SPI2_HOST; break; }
    if (!b) { aos_hal_log("io", "no SPI host left for %s", p->name); return NULL; }
    spi_bus_config_t c = {
        .sclk_io_num = p->pins[0], .mosi_io_num = p->pins[1], .miso_io_num = p->pins[2],
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .data4_io_num = -1, .data5_io_num = -1, .data6_io_num = -1, .data7_io_num = -1,
        .max_transfer_sz = SPI_CHUNK,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    b->btx = heap_caps_aligned_alloc(SPI_ALIGN, SPI_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    b->brx = heap_caps_aligned_alloc(SPI_ALIGN, SPI_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    b->lock = xSemaphoreCreateMutex();
    esp_err_t e = (b->btx && b->brx && b->lock) ? spi_bus_initialize(b->host, &c, SPI_DMA_CH_AUTO) : ESP_ERR_NO_MEM;
    if (e != ESP_OK) {
        aos_hal_log("io", "%s: spi_bus_initialize: %s", p->name, esp_err_to_name(e));
        heap_caps_free(b->btx);
        heap_caps_free(b->brx);
        if (b->lock) vSemaphoreDelete(b->lock);
        memset(b, 0, sizeof *b);
        return NULL;
    }
    /* nothing on MISO reads 0xFF, not whatever the floating pin picks up */
    if (p->pins[2] >= 0) gpio_pullup_en(p->pins[2]);
    snprintf(b->port, sizeof b->port, "%s", p->name);
    memcpy(b->pins, p->pins, sizeof b->pins);
    return b;
}

static void bus_down(spi_bus_t *b)
{
    spi_bus_free(b->host);
    for (int k = 0; k < 3; k++) if (b->pins[k] >= 0) gpio_reset_pin(b->pins[k]);
    heap_caps_free(b->btx);
    heap_caps_free(b->brx);
    vSemaphoreDelete(b->lock);
    memset(b, 0, sizeof *b);
}

static bool dev_add(aos_io_spi_t *s, spi_be_t *d, uint32_t hz)
{
    spi_device_interface_config_t dc = {
        .mode = s->mode,
        .clock_speed_hz = (int)hz,
        .spics_io_num = s->cs,
        .queue_size = 1,
        .flags = s->lsb_first ? SPI_DEVICE_BIT_LSBFIRST : 0,
    };
    esp_err_t e = spi_bus_add_device(d->bus->host, &dc, &d->dev);
    if (e != ESP_OK) {
        aos_hal_log("io", "%s: spi_bus_add_device at %u Hz: %s", s->port->name, (unsigned)hz, esp_err_to_name(e));
        d->dev = NULL;
        return false;
    }
    int khz = 0;
    s->actual_hz = spi_device_get_actual_freq(d->dev, &khz) == ESP_OK ? (uint32_t)khz * 1000u : hz;
    return true;
}

bool aos_io_be_spi_open(aos_io_spi_t *s, bool first)
{
    spi_be_t *d = calloc(1, sizeof *d);
    if (!d) return false;
    if (first) d->bus = bus_up(s->port);
    else
        for (int i = 0; i < 2; i++)
            if (!strcmp(s_spi_bus[i].port, s->port->name)) d->bus = &s_spi_bus[i];
    if (!d->bus || !dev_add(s, d, s->clock_hz)) {
        if (first && d->bus) bus_down(d->bus);
        free(d);
        return false;
    }
    s->be = d;
    return true;
}

bool aos_io_be_spi_xfer(aos_io_spi_t *s, const uint8_t *tx, uint8_t *rx, size_t n, int timeout_ms)
{
    spi_be_t *d = s->be;
    spi_bus_t *b = d->bus;
    if (xSemaphoreTake(b->lock, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    bool many = n > SPI_CHUNK || (n > SPI_ALIGN && n % SPI_ALIGN);
    if (many) spi_device_acquire_bus(d->dev, portMAX_DELAY);
    bool ok = true, has_miso = b->pins[2] >= 0;
    for (size_t done = 0; ok && done < n; ) {
        size_t len = n - done;
        if (len > SPI_CHUNK) len = SPI_CHUNK;
        else if (len > SPI_ALIGN && len % SPI_ALIGN) len -= len % SPI_ALIGN;   /* the odd tail goes last, alone */
        if (tx) memcpy(b->btx, tx + done, len);
        else memset(b->btx, 0xFF, len);
        spi_transaction_t t = {
            .flags = done + len < n ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
            .length = len * 8,
            .tx_buffer = b->btx,
            .rx_buffer = rx && has_miso ? b->brx : NULL,
        };
        bool poll = (uint64_t)len * 8u * 1000000u <= (uint64_t)SPI_POLL_US * (s->actual_hz ? s->actual_hz : s->clock_hz);
        esp_err_t e = poll ? spi_device_polling_transmit(d->dev, &t) : spi_device_transmit(d->dev, &t);
        if (e != ESP_OK) {
            aos_hal_log("io", "%s: transfer of %u bytes: %s", s->port->name, (unsigned)len, esp_err_to_name(e));
            ok = false;
            break;
        }
        if (rx) {
            if (has_miso) memcpy(rx + done, b->brx, len);
            else memset(rx + done, 0xFF, len);
        }
        done += len;
    }
    if (many) spi_device_release_bus(d->dev);
    xSemaphoreGive(b->lock);
    return ok;
}

bool aos_io_be_spi_set_clock(aos_io_spi_t *s, uint32_t hz)
{
    spi_be_t *d = s->be;
    xSemaphoreTake(d->bus->lock, portMAX_DELAY);
    spi_bus_remove_device(d->dev);
    bool ok = dev_add(s, d, hz);
    if (!ok) dev_add(s, d, s->clock_hz);          /* back to the old one rather than no device */
    xSemaphoreGive(d->bus->lock);
    return ok && d->dev;
}

void aos_io_be_spi_close(aos_io_spi_t *s, bool last)
{
    spi_be_t *d = s->be;
    if (!d) return;
    if (d->dev) spi_bus_remove_device(d->dev);
    if (s->cs >= 0) gpio_reset_pin(s->cs);
    if (last) bus_down(d->bus);
    free(d);
    s->be = NULL;
}

/* ---- GPIO ---- */

bool aos_io_be_gpio_mode(int gpio, aos_gpio_mode_t mode)
{
    gpio_config_t c = { .pin_bit_mask = 1ULL << gpio };
    c.mode = mode == AOS_GPIO_OUTPUT ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT;
    c.pull_up_en = mode == AOS_GPIO_INPUT_PULLUP;
    c.pull_down_en = mode == AOS_GPIO_INPUT_PULLDOWN;
    return gpio_config(&c) == ESP_OK;
}

int aos_io_be_gpio_get(int gpio) { return gpio_get_level(gpio); }
bool aos_io_be_gpio_set(int gpio, int level) { return gpio_set_level(gpio, level) == ESP_OK; }

/* ---- 1-Wire: Espressif's onewire_bus over the RMT (one TX and one RX
 * channel a bus). Small: its receive buffer is a few symbols. ---- */

#include "onewire_bus.h"
#include "onewire_device.h"

bool aos_io_be_ow_open(aos_io_ow_t *b)
{
    onewire_bus_config_t bc = { .bus_gpio_num = b->gpio, .flags = { .en_pull_up = b->pullup } };
    onewire_bus_rmt_config_t rc = { .max_rx_bytes = 16 };      /* a scratchpad and its CRC */
    onewire_bus_handle_t bus = NULL;
    esp_err_t e = onewire_new_bus_rmt(&bc, &rc, &bus);
    if (e != ESP_OK) {
        aos_hal_log("io", "1-Wire on GPIO%d: %s", b->gpio, esp_err_to_name(e));
        return false;
    }
    b->be = bus;
    return true;
}

bool aos_io_be_ow_reset(aos_io_ow_t *b) { return onewire_bus_reset((onewire_bus_handle_t)b->be) == ESP_OK; }

int aos_io_be_ow_search(aos_io_ow_t *b, uint64_t *roms, int max)
{
    onewire_device_iter_handle_t it = NULL;
    if (onewire_new_device_iter((onewire_bus_handle_t)b->be, &it) != ESP_OK) return -1;
    int n = 0;
    onewire_device_t dev;
    esp_err_t e;
    while (n < max && (e = onewire_device_iter_get_next(it, &dev)) != ESP_ERR_NOT_FOUND) {
        if (e == ESP_OK) roms[n++] = dev.address;
        else break;                     /* a CRC error mid-search: what was found so far */
    }
    onewire_del_device_iter(it);
    return n;
}

bool aos_io_be_ow_write(aos_io_ow_t *b, const uint8_t *data, size_t n)
{
    for (size_t off = 0; off < n; off += 255) {
        size_t k = n - off > 255 ? 255 : n - off;
        if (onewire_bus_write_bytes((onewire_bus_handle_t)b->be, data + off, (uint8_t)k) != ESP_OK) return false;
    }
    return true;
}

bool aos_io_be_ow_read(aos_io_ow_t *b, uint8_t *data, size_t n)
{
    for (size_t off = 0; off < n; off += 16) {
        size_t k = n - off > 16 ? 16 : n - off;
        if (onewire_bus_read_bytes((onewire_bus_handle_t)b->be, data + off, k) != ESP_OK) return false;
    }
    return true;
}

void aos_io_be_ow_close(aos_io_ow_t *b)
{
    if (b->be) onewire_bus_del((onewire_bus_handle_t)b->be);
    b->be = NULL;
    gpio_reset_pin(b->gpio);
}

/* ---- LED strips: one RMT TX channel each, a bytes encoder for the bits
 * and a copy encoder for the reset (IDF's led_strip example's shape). No
 * DMA: its symbol buffer would be internal RAM; the ISR refills the
 * channel's memory from the PSRAM frame instead, 96 symbols at a time. ---- */

#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes, *copy;
    int state;
    rmt_symbol_word_t reset;
} strip_enc_t;

typedef struct {
    rmt_channel_handle_t ch;
    rmt_encoder_t *enc;
} strip_be_t;

#define STRIP_RES_HZ 10000000           /* 0.1 us a tick */

static size_t strip_encode(rmt_encoder_t *e, rmt_channel_handle_t ch, const void *data, size_t size,
                           rmt_encode_state_t *ret)
{
    strip_enc_t *se = __containerof(e, strip_enc_t, base);
    rmt_encode_state_t st = RMT_ENCODING_RESET, out = RMT_ENCODING_RESET;
    size_t n = 0;
    if (se->state == 0) {
        n += se->bytes->encode(se->bytes, ch, data, size, &st);
        if (st & RMT_ENCODING_COMPLETE) se->state = 1;
        if (st & RMT_ENCODING_MEM_FULL) { *ret = RMT_ENCODING_MEM_FULL; return n; }
    }
    if (se->state == 1) {
        n += se->copy->encode(se->copy, ch, &se->reset, sizeof se->reset, &st);
        if (st & RMT_ENCODING_COMPLETE) { se->state = 0; out |= RMT_ENCODING_COMPLETE; }
        if (st & RMT_ENCODING_MEM_FULL) out |= RMT_ENCODING_MEM_FULL;
    }
    *ret = out;
    return n;
}

static esp_err_t strip_enc_reset(rmt_encoder_t *e)
{
    strip_enc_t *se = __containerof(e, strip_enc_t, base);
    rmt_encoder_reset(se->bytes);
    rmt_encoder_reset(se->copy);
    se->state = 0;
    return ESP_OK;
}

static esp_err_t strip_enc_del(rmt_encoder_t *e)
{
    strip_enc_t *se = __containerof(e, strip_enc_t, base);
    rmt_del_encoder(se->bytes);
    rmt_del_encoder(se->copy);
    free(se);
    return ESP_OK;
}

/* the datasheets' timings, in ticks: T0H, T0L, T1H, T1L, and the reset
 * (300 us covers the newer WS2812B's 280) */
static const struct { uint16_t t0h, t0l, t1h, t1l, reset_us; } TIMING[AOS_STRIP_TYPE_COUNT] = {
    [AOS_STRIP_WS2812B]     = { 4, 8, 8, 4, 300 },
    [AOS_STRIP_WS2811_400]  = { 5, 20, 12, 13, 300 },
    [AOS_STRIP_SK6812]      = { 3, 9, 6, 6, 300 },
    [AOS_STRIP_SK6812_RGBW] = { 3, 9, 6, 6, 300 },
};

bool aos_io_be_strip_open(aos_io_strip_t *s)
{
    strip_be_t *be = calloc(1, sizeof *be);
    strip_enc_t *se = calloc(1, sizeof *se);
    if (!be || !se) goto fail;
    rmt_tx_channel_config_t cc = {
        .gpio_num = s->gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = STRIP_RES_HZ,
        .mem_block_symbols = 96,
        .trans_queue_depth = 2,
    };
    esp_err_t e = rmt_new_tx_channel(&cc, &be->ch);
    if (e != ESP_OK) {
        cc.mem_block_symbols = 48;      /* the other channels took the second block */
        e = rmt_new_tx_channel(&cc, &be->ch);
    }
    if (e != ESP_OK) { aos_hal_log("io", "LED strip on GPIO%d: %s", s->gpio, esp_err_to_name(e)); goto fail; }
    const typeof(TIMING[0]) *t = &TIMING[s->cfg.type];
    rmt_bytes_encoder_config_t bc = {
        .bit0 = { .level0 = 1, .duration0 = t->t0h, .level1 = 0, .duration1 = t->t0l },
        .bit1 = { .level0 = 1, .duration0 = t->t1h, .level1 = 0, .duration1 = t->t1l },
        .flags.msb_first = 1,
    };
    rmt_copy_encoder_config_t cpc = {};
    if (rmt_new_bytes_encoder(&bc, &se->bytes) != ESP_OK || rmt_new_copy_encoder(&cpc, &se->copy) != ESP_OK) goto fail;
    uint32_t half = (uint32_t)t->reset_us * (STRIP_RES_HZ / 1000000) / 2;
    se->reset = (rmt_symbol_word_t){ .level0 = 0, .duration0 = half, .level1 = 0, .duration1 = half };
    se->base.encode = strip_encode;
    se->base.reset = strip_enc_reset;
    se->base.del = strip_enc_del;
    be->enc = &se->base;
    se = NULL;
    if (rmt_enable(be->ch) != ESP_OK) goto fail;
    s->be = be;
    return true;
fail:
    if (se) {
        if (se->bytes) rmt_del_encoder(se->bytes);
        if (se->copy) rmt_del_encoder(se->copy);
        free(se);
    }
    if (be) {
        if (be->enc) rmt_del_encoder(be->enc);
        if (be->ch) rmt_del_channel(be->ch);
        free(be);
    }
    return false;
}

bool aos_io_be_strip_send(aos_io_strip_t *s)
{
    strip_be_t *be = s->be;
    rmt_transmit_config_t tc = { .loop_count = 0 };
    if (rmt_transmit(be->ch, be->enc, s->wire, (size_t)s->cfg.count * s->bpp, &tc) != ESP_OK) return false;
    /* a WS2812B LED is 30 us on the wire: 1500 of them, 45 ms */
    int ms = 20 + s->cfg.count * (s->cfg.type == AOS_STRIP_WS2811_400 ? 8 : 4) * s->bpp / 100;
    return rmt_tx_wait_all_done(be->ch, ms) == ESP_OK;
}

void aos_io_be_strip_close(aos_io_strip_t *s)
{
    strip_be_t *be = s->be;
    if (!be) return;
    rmt_tx_wait_all_done(be->ch, 100);
    rmt_disable(be->ch);
    rmt_del_encoder(be->enc);
    rmt_del_channel(be->ch);
    free(be);
    s->be = NULL;
    gpio_reset_pin(s->gpio);
}
