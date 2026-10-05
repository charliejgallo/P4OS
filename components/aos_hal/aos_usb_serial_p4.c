/*
 * P4OS - USB serial ports on the host: CDC-ACM devices (an Arduino, a board
 * with native USB, a modem) and the CH34x, CP210x and FTDI converters
 * (aos_hal.h, docs/USB.md).
 *
 * Espressif's usb_host_cdc_acm does the work, with its vendor drivers for
 * the three converter families. Its new-device callback says what each
 * device is; a serial one is listed (aos_hal_usb_serial_count, _name) and
 * opened only when asked (aos_hal_usb_serial_open), since an open port
 * holds the device. To the rest of the system they are ports "usb0",
 * "usb1" of aos_io, which the Terminal opens like its UARTs.
 *
 * What comes in lands in a stream buffer in PSRAM (16 KB) that the reader
 * empties; writing blocks up to half a second. A device unplugged while
 * open closes its port: reads answer -1 from then on.
 *
 * Once opened, the USB side stays open until the device goes (or the host
 * stops): closing the port only puts it to rest, dropping what comes in,
 * and opening it again takes it back at the new speed. Closing it for
 * real and opening it again lost the first packet after every reopen with
 * an FTDI (measured on 2026-10-04): the CDC-ACM driver resets its side of
 * the endpoints on close and not the device's, so their data toggles part
 * ways. And that close is where the driver aborted under heavy traffic.
 */
#include "aos_hal.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb/vcp_ch34x.h"
#include "usb/vcp_cp210x.h"
#include "usb/vcp_ftdi.h"

static const char *TAG = "usb";
#define SER_MAX 4
#define RX_BYTES (16 * 1024)

void aos_p4_usb_dev_use(uint8_t addr, const char *what);

enum { K_ACM = 0, K_CH34X, K_CP210X, K_FTDI };

typedef struct {
    bool used;
    uint8_t addr, intf, kind;
    uint16_t vid, pid;
    char name[48];
    /* open */
    cdc_acm_dev_hdl_t cdc;
    StreamBufferHandle_t rx;
    volatile bool closed;           /* unplugged while open */
    volatile bool in_use;           /* opened by someone; false: at rest, input dropped */
} ser_t;

static struct {
    bool installed;
    SemaphoreHandle_t mx;
    ser_t s[SER_MAX];
} S;

static void wide_to_str(const usb_str_desc_t *d, char *out, size_t n)
{
    size_t i = 0;
    if (d)
        for (int k = 0; k < (d->bLength - 2) / 2 && i + 1 < n; k++) {
            uint16_t w = d->wData[k];
            out[i++] = w >= 32 && w < 127 ? (char)w : '?';
        }
    out[i] = 0;
    /* some pad their names with spaces (" USB gamepad           ") */
    while (i && out[i - 1] == ' ') out[--i] = 0;
    size_t lead = 0;
    while (out[lead] == ' ') lead++;
    if (lead) memmove(out, out + lead, i - lead + 1);
}

/* Every new device, from the CDC-ACM driver's context (it opens it for the
 * call): a serial one is listed. */
static void new_dev(usb_device_handle_t dev)
{
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cfg;
    usb_device_info_t di;
    if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK || usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK ||
        usb_host_device_info(dev, &di) != ESP_OK)
        return;
    int kind = -1, intf = 0;
    if (dd->idVendor == NANJING_QINHENG_MICROE_VID) kind = K_CH34X;
    else if (dd->idVendor == SILICON_LABS_VID) kind = K_CP210X;
    else if (dd->idVendor == FTDI_VID) kind = K_FTDI;
    else {
        /* CDC-ACM: a communications interface of the abstract control model */
        int off = 0;
        const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
        while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off)))
            if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
                const usb_intf_desc_t *it = (const usb_intf_desc_t *)d;
                if (it->bInterfaceClass == 0x02 && it->bInterfaceSubClass == 0x02) {
                    kind = K_ACM;
                    intf = it->bInterfaceNumber;
                    break;
                }
            }
    }
    if (kind < 0) return;
    xSemaphoreTake(S.mx, portMAX_DELAY);
    ser_t *s = NULL;
    for (int i = 0; i < SER_MAX && !s; i++) if (!S.s[i].used) s = &S.s[i];
    if (s) {
        memset(s, 0, sizeof *s);
        s->used = true;
        s->addr = di.dev_addr;
        s->intf = intf;
        s->kind = kind;
        s->vid = dd->idVendor;
        s->pid = dd->idProduct;
        char v[24], p[40];
        wide_to_str(di.str_desc_manufacturer, v, sizeof v);
        wide_to_str(di.str_desc_product, p, sizeof p);
        snprintf(s->name, sizeof s->name, "%.23s%s%.23s", v, *v && *p ? " " : "", p);
        if (!s->name[0]) snprintf(s->name, sizeof s->name, "%04x:%04x", s->vid, s->pid);
    }
    xSemaphoreGive(S.mx);
    if (s) {
        static const char *const KIND[] = { "CDC-ACM", "CH34x", "CP210x", "FTDI" };
        ESP_LOGI(TAG, "serial \"%s\" (%s) at address %u", s->name, KIND[kind], s->addr);
        aos_p4_usb_dev_use(s->addr, "serial");
    }
}

/* the devices list knows what is still plugged in */
static bool present(const ser_t *s)
{
    static aos_usb_dev_t d[12];         /* 1.5 KB: not on a caller's stack; under S.mx like every caller */
    int n = aos_hal_usb_devices(d, 12);
    for (int i = 0; i < n; i++)
        if (d[i].addr == s->addr && d[i].vid == s->vid && d[i].pid == s->pid) return true;
    return false;
}

/* the list without the ones that went away (closed ones only: an open port
 * stays until it is closed, reading -1) */
/* the USB side closed for real: the device went, or the host stops */
static void dispose(ser_t *s)
{
    if (s->cdc) cdc_acm_host_close(s->cdc);
    s->cdc = NULL;
    if (s->rx) vStreamBufferDeleteWithCaps(s->rx);
    s->rx = NULL;
    s->in_use = false;
}

/* the list without the ones that went away (one in use stays, reading -1,
 * until whoever has it closes it) */
static void prune(void)
{
    for (int i = 0; i < SER_MAX; i++) {
        ser_t *s = &S.s[i];
        if (!s->used || s->in_use) continue;
        if (s->cdc && s->closed) {
            dispose(s);
            s->used = false;
        } else if (!s->cdc && !present(s)) s->used = false;
    }
}

static bool rx_cb(const uint8_t *data, size_t len, void *arg)
{
    ser_t *s = arg;
    /* at rest, nobody reads: dropped. Full: what does not fit is lost. */
    if (s->rx && s->in_use) xStreamBufferSend(s->rx, data, len, 0);
    return true;
}

static void event_cb(const cdc_acm_host_dev_event_data_t *ev, void *arg)
{
    ser_t *s = arg;
    if (ev->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGI(TAG, "serial \"%s\" unplugged", s->name);
        s->closed = true;
    }
}

bool aos_p4_usb_serial_start(void)
{
    if (!S.mx) S.mx = xSemaphoreCreateMutex();
    memset(S.s, 0, sizeof S.s);
    const cdc_acm_host_driver_config_t cfg = { .driver_task_stack_size = 4096, .driver_task_priority = 5, .xCoreID = 0,
                                               .new_dev_cb = new_dev };
    esp_err_t e = cdc_acm_host_install(&cfg);
    S.installed = e == ESP_OK;
    if (!S.installed) ESP_LOGW(TAG, "serial: cdc_acm_host_install: %s", esp_err_to_name(e));
    return S.installed;
}

void aos_p4_usb_serial_stop(void)
{
    if (!S.installed) return;
    for (int i = 0; i < SER_MAX; i++) dispose(&S.s[i]);
    cdc_acm_host_uninstall();
    S.installed = false;
    memset(S.s, 0, sizeof S.s);
}

int aos_hal_usb_serial_count(void)
{
    if (!S.installed) return 0;
    xSemaphoreTake(S.mx, portMAX_DELAY);
    prune();
    int n = 0;
    for (int i = 0; i < SER_MAX; i++) n += S.s[i].used;
    xSemaphoreGive(S.mx);
    return n;
}

/* the i-th listed port's slot */
static int slot_of(int index)
{
    for (int i = 0, k = 0; i < SER_MAX; i++)
        if (S.s[i].used && k++ == index) return i;
    return -1;
}

bool aos_hal_usb_serial_name(int index, char *out, size_t n)
{
    if (!S.installed) return false;
    xSemaphoreTake(S.mx, portMAX_DELAY);
    int i = slot_of(index);
    if (i >= 0) snprintf(out, n, "%s", S.s[i].name);
    xSemaphoreGive(S.mx);
    return i >= 0;
}

int aos_hal_usb_serial_open(int index, uint32_t baud)
{
    if (!S.installed) return -1;
    xSemaphoreTake(S.mx, portMAX_DELAY);
    prune();
    int i = slot_of(index);
    ser_t *s = i >= 0 ? &S.s[i] : NULL;
    xSemaphoreGive(S.mx);
    if (!s || s->in_use || s->closed) return -1;
    if (s->cdc) {
        /* at rest since its last close: taken back as it is */
        xStreamBufferReset(s->rx);
        s->in_use = true;
        aos_hal_usb_serial_set_format(i, baud, 'N', 1);
        cdc_acm_host_set_control_line_state(s->cdc, true, true);
        ESP_LOGI(TAG, "serial \"%s\" open again at %u baud", s->name, (unsigned)baud);
        return i;
    }
    s->rx = xStreamBufferCreateWithCaps(RX_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (!s->rx) return -1;
    s->closed = false;
    const cdc_acm_host_device_config_t dc = { .connection_timeout_ms = 1000, .out_buffer_size = 512,
                                              .in_buffer_size = 512, .event_cb = event_cb, .data_cb = rx_cb,
                                              .user_arg = s };
    esp_err_t e;
    switch (s->kind) {
    case K_CH34X:  e = ch34x_vcp_open(s->pid, 0, &dc, &s->cdc); break;
    case K_CP210X: e = cp210x_vcp_open(s->pid, 0, &dc, &s->cdc); break;
    case K_FTDI:   e = ftdi_vcp_open(s->pid, 0, &dc, &s->cdc); break;
    default: {
        cdc_acm_host_open_config_t oc = { .vid = s->vid, .pid = s->pid, .interface_idx = s->intf, .dev_addr = s->addr,
                                          .connection_timeout_ms = dc.connection_timeout_ms,
                                          .out_buffer_size = dc.out_buffer_size, .in_buffer_size = dc.in_buffer_size,
                                          .event_cb = event_cb, .data_cb = rx_cb, .user_arg = s };
        e = cdc_acm_host_open(&oc, &s->cdc);
        break;
    }
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "serial \"%s\": open: %s", s->name, esp_err_to_name(e));
        s->cdc = NULL;
        vStreamBufferDeleteWithCaps(s->rx);
        s->rx = NULL;
        return -1;
    }
    s->in_use = true;
    aos_hal_usb_serial_set_format(i, baud, 'N', 1);
    /* DTR and RTS up: what a computer's terminal does, and what an Arduino
     * (or a CDC device that waits for a terminal) expects */
    cdc_acm_host_set_control_line_state(s->cdc, true, true);
    ESP_LOGI(TAG, "serial \"%s\" open at %u baud", s->name, (unsigned)baud);
    return i;
}

bool aos_hal_usb_serial_set_format(int h, uint32_t baud, char parity, int stop_bits)
{
    if (h < 0 || h >= SER_MAX || !S.s[h].cdc || S.s[h].closed) return false;
    cdc_acm_line_coding_t lc = { .dwDTERate = baud, .bCharFormat = stop_bits == 2 ? 2 : 0,
                                 .bParityType = parity == 'O' ? 1 : parity == 'E' ? 2 : 0, .bDataBits = 8 };
    return cdc_acm_host_line_coding_set(S.s[h].cdc, &lc) == ESP_OK;
}

int aos_hal_usb_serial_read(int h, void *buf, int len, int timeout_ms)
{
    if (h < 0 || h >= SER_MAX || !S.s[h].rx) return -1;
    size_t n = xStreamBufferReceive(S.s[h].rx, buf, len, pdMS_TO_TICKS(timeout_ms));
    if (!n && S.s[h].closed) return -1;
    return (int)n;
}

int aos_hal_usb_serial_write(int h, const void *buf, int len)
{
    if (h < 0 || h >= SER_MAX || !S.s[h].cdc || S.s[h].closed) return -1;
    return cdc_acm_host_data_tx_blocking(S.s[h].cdc, buf, len, 500) == ESP_OK ? len : -1;
}

void aos_hal_usb_serial_close(int h)
{
    if (h < 0 || h >= SER_MAX || !S.installed) return;
    xSemaphoreTake(S.mx, portMAX_DELAY);
    ser_t *s = &S.s[h];
    s->in_use = false;                  /* at rest: the USB side stays open (see the top) */
    if (s->closed) {                    /* it is gone: closed for real, and off the list */
        dispose(s);
        s->used = false;
    }
    xSemaphoreGive(S.mx);
}
