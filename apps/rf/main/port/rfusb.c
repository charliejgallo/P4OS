/*
 * P4OS - libusb for librtlsdr, on the board's raw USB (port/libusb.h says
 * why and what). Also rf_port.h's log and sleep.
 */
#include "libusb.h"
#include "rf_port.h"
#include "rfusb.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "aos_hal.h"

#define DEV_MAX 12

struct rfusb_context { int unused; };

struct rfusb_device {
    uint8_t addr;
    struct rfusb_device_descriptor dd;
};

struct rfusb_device_handle {
    struct rfusb_device dev;        /* a copy: the list it came from is freed right after the open */
    aos_usb_raw_t *raw;
};

static struct rfusb_context the_ctx;

void rf_port_log(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n) aos_hal_log("rf", "%s", line);
}

void rf_port_usleep(unsigned us)
{
    aos_hal_sleep_ms(us < 1000 ? 1 : (us + 999) / 1000);
}

static int err(int r)
{
    switch (r) {
    case AOS_USB_RAW_GONE: return LIBUSB_ERROR_NO_DEVICE;
    case AOS_USB_RAW_TIMEOUT: return LIBUSB_ERROR_TIMEOUT;
    case AOS_USB_RAW_STALL: return LIBUSB_ERROR_PIPE;
    default: return r < 0 ? LIBUSB_ERROR_IO : r;
    }
}

int rfusb_init(rfusb_context **ctx)
{
    if (ctx) *ctx = &the_ctx;
    return 0;
}

void rfusb_exit(rfusb_context *ctx) { (void)ctx; }

ssize_t rfusb_get_device_list(rfusb_context *ctx, rfusb_device ***list)
{
    (void)ctx;
    aos_usb_dev_t *d = malloc(DEV_MAX * sizeof *d);
    /* NULL-terminated, and the devices' block after the terminator, for free */
    rfusb_device **l = calloc(DEV_MAX + 2, sizeof *l);
    rfusb_device *devs = calloc(DEV_MAX, sizeof *devs);
    if (!d || !l || !devs) {
        free(d);
        free(l);
        free(devs);
        return LIBUSB_ERROR_NO_MEM;
    }
    int n = aos_hal_usb_devices(d, DEV_MAX);
    for (int i = 0; i < n; i++) {
        devs[i].addr = d[i].addr;
        devs[i].dd.bLength = 18;
        devs[i].dd.bDescriptorType = 1;
        devs[i].dd.bDeviceClass = d[i].cls;
        devs[i].dd.idVendor = d[i].vid;
        devs[i].dd.idProduct = d[i].pid;
        devs[i].dd.bNumConfigurations = 1;
        l[i] = &devs[i];
    }
    free(d);
    l[DEV_MAX + 1] = devs;
    *list = l;
    return n;
}

void rfusb_free_device_list(rfusb_device **list, int unref)
{
    (void)unref;
    if (!list) return;
    free(list[DEV_MAX + 1]);
    free(list);
}

int rfusb_get_device_descriptor(rfusb_device *dev, struct rfusb_device_descriptor *desc)
{
    if (!dev || !desc) return LIBUSB_ERROR_INVALID_PARAM;
    *desc = dev->dd;
    return 0;
}

rfusb_device *rfusb_get_device(rfusb_device_handle *h)
{
    return h ? &h->dev : NULL;
}

int rfusb_open(rfusb_device *dev, rfusb_device_handle **out)
{
    rfusb_device_handle *h = calloc(1, sizeof *h);
    if (!h) return LIBUSB_ERROR_NO_MEM;
    h->dev = *dev;
    h->raw = aos_hal_usb_raw_open(dev->addr, "RF");
    if (!h->raw) {
        free(h);
        return LIBUSB_ERROR_ACCESS;
    }
    /* the whole device descriptor (the list has only what the host lists):
     * the string indexes are in it */
    uint8_t dd[18];
    if (aos_hal_usb_raw_control(h->raw, 0x80, 6, 0x0100, 0, dd, sizeof dd, 1000) == (int)sizeof dd) {
        struct rfusb_device_descriptor *d = &h->dev.dd;
        d->bcdUSB = dd[2] | dd[3] << 8;
        d->bDeviceClass = dd[4];
        d->bDeviceSubClass = dd[5];
        d->bDeviceProtocol = dd[6];
        d->bMaxPacketSize0 = dd[7];
        d->bcdDevice = dd[12] | dd[13] << 8;
        d->iManufacturer = dd[14];
        d->iProduct = dd[15];
        d->iSerialNumber = dd[16];
        d->bNumConfigurations = dd[17];
    }
    *out = h;
    return 0;
}

void rfusb_close(rfusb_device_handle *h)
{
    if (!h) return;
    aos_hal_usb_raw_close(h->raw);
    free(h);
}

aos_usb_raw_t *rfusb_raw(struct rfusb_device_handle *h)
{
    return h ? h->raw : NULL;
}

uint8_t rfusb_addr(struct rfusb_device_handle *h)
{
    return h ? h->dev.addr : 0;
}

int rfusb_claim_interface(rfusb_device_handle *h, int intf)
{
    return aos_hal_usb_raw_claim(h->raw, intf, 0) ? 0
           : aos_hal_usb_raw_gone(h->raw)       ? LIBUSB_ERROR_NO_DEVICE
                                                : LIBUSB_ERROR_BUSY;
}

/* released with the device, by close */
int rfusb_release_interface(rfusb_device_handle *h, int intf) { (void)h; (void)intf; return 0; }
int rfusb_kernel_driver_active(rfusb_device_handle *h, int intf) { (void)h; (void)intf; return 0; }
int rfusb_detach_kernel_driver(rfusb_device_handle *h, int intf) { (void)h; (void)intf; return 0; }
int rfusb_attach_kernel_driver(rfusb_device_handle *h, int intf) { (void)h; (void)intf; return 0; }
int rfusb_reset_device(rfusb_device_handle *h) { (void)h; return LIBUSB_ERROR_NOT_SUPPORTED; }

int rfusb_control_transfer(rfusb_device_handle *h, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                           unsigned char *data, uint16_t len, unsigned int timeout)
{
    return err(aos_hal_usb_raw_control(h->raw, type, req, value, index, data, len, timeout ? (int)timeout : 1000));
}

int rfusb_bulk_transfer(rfusb_device_handle *h, unsigned char ep, unsigned char *data, int len, int *got,
                        unsigned int timeout)
{
    int r = aos_hal_usb_raw_transfer(h->raw, ep, data, len, timeout ? (int)timeout : 1000);
    if (got) *got = r > 0 ? r : 0;
    return r >= 0 ? 0 : err(r);
}

int rfusb_get_string_descriptor_ascii(rfusb_device_handle *h, uint8_t index, unsigned char *data, int len)
{
    if (!index || len <= 0) return LIBUSB_ERROR_INVALID_PARAM;
    uint8_t buf[255];
    int r = aos_hal_usb_raw_control(h->raw, 0x80, 6, 0x0300 | index, 0x0409, buf, sizeof buf, 1000);
    if (r < 2) return r < 0 ? err(r) : LIBUSB_ERROR_IO;
    int n = 0;
    for (int i = 2; i + 1 < r && i < buf[0] && n + 1 < len; i += 2)
        data[n++] = buf[i + 1] ? '?' : buf[i];
    data[n] = 0;
    return n;
}

/* the async half: never used (port/libusb.h) */
struct rfusb_transfer *rfusb_alloc_transfer(int iso_packets) { (void)iso_packets; return NULL; }
void rfusb_free_transfer(struct rfusb_transfer *t) { (void)t; }
int rfusb_submit_transfer(struct rfusb_transfer *t) { (void)t; return LIBUSB_ERROR_NOT_SUPPORTED; }
int rfusb_cancel_transfer(struct rfusb_transfer *t) { (void)t; return LIBUSB_ERROR_NOT_SUPPORTED; }
int rfusb_handle_events_timeout_completed(rfusb_context *ctx, struct timeval *tv, int *completed)
{
    (void)ctx; (void)tv; (void)completed;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}
unsigned char *rfusb_dev_mem_alloc(rfusb_device_handle *h, size_t len) { (void)h; (void)len; return NULL; }
int rfusb_dev_mem_free(rfusb_device_handle *h, unsigned char *buf, size_t len)
{
    (void)h; (void)buf; (void)len;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}
