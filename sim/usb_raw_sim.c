/*
 * P4OS simulator - raw USB devices (aos_hal_usb_raw_*, aos_usb_raw_p4.c on
 * the board) through the Mac's libusb, so an app with a driver of its own
 * (the RF app and its RTL-SDR) runs against the real device plugged into
 * the Mac.
 *
 * Only with P4_SIM_USB=1: otherwise the simulator lists no devices and opens
 * none, as before. With it, aos_hal_usb_devices() lists the Mac's devices
 * (IDs and class from their descriptors; nothing is opened to read names)
 * and the host reads as on. The Mac's own drivers keep what they hold
 * (keyboards, disks, serial ports): libusb cannot claim those, and the
 * simulator never tries but for the device an app asks for.
 *
 * Built only when CMake finds libusb-1.0 (Homebrew's); without it the weak
 * stubs answer "no device".
 */
#include "aos_hal.h"

#include <libusb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RAW_MAX 4
#define XFERS_MAX 16

struct aos_usb_raw {
    bool used;
    uint8_t addr;
    libusb_device_handle *h;
    int claimed;
    volatile bool gone;
    /* the stream */
    struct libusb_transfer *st[XFERS_MAX];
    int st_n;
    volatile int inflight;
    volatile bool st_stop;
    uint8_t *ring;
    size_t ring_size, head, tail, fill;
    uint64_t bytes;
    uint32_t dropped, errors;
};

static struct {
    bool tried, ok;
    libusb_context *ctx;
    pthread_t ev;
    pthread_mutex_t mx;
    pthread_cond_t cv;
    struct aos_usb_raw r[RAW_MAX];
} S = { .mx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

/* The Mac numbers devices per bus, so two of them can share an address (the
 * stick and the board's own USB-serial adapter did): the simulator gives
 * each (bus, address) a number of its own, the "address" the board's list
 * would have. */
static struct { uint8_t bus, addr; } s_ids[250];
static int s_nids;

static uint8_t sim_addr(libusb_device *d)
{
    uint8_t bus = libusb_get_bus_number(d), addr = libusb_get_device_address(d);
    for (int i = 0; i < s_nids; i++)
        if (s_ids[i].bus == bus && s_ids[i].addr == addr) return (uint8_t)(i + 1);
    if (s_nids == (int)(sizeof s_ids / sizeof s_ids[0])) return 0;
    s_ids[s_nids].bus = bus;
    s_ids[s_nids].addr = addr;
    return (uint8_t)++s_nids;
}

static void *events(void *arg)
{
    (void)arg;
    for (;;) {
        struct timeval tv = { 0, 100000 };
        libusb_handle_events_timeout_completed(S.ctx, &tv, NULL);
    }
    return NULL;
}

static bool up(void)
{
    if (S.tried) return S.ok;
    S.tried = true;
    const char *e = getenv("P4_SIM_USB");
    if (!e || !*e || *e == '0') return false;
    if (libusb_init(&S.ctx) != 0) {
        printf("[usb] libusb_init failed\n");
        return false;
    }
    pthread_create(&S.ev, NULL, events, NULL);
    S.ok = true;
    printf("[usb] P4_SIM_USB: the Mac's USB devices through libusb\n");
    return true;
}

bool aos_hal_usb_host_on(void)
{
    return up();
}

int aos_hal_usb_devices(aos_usb_dev_t *out, int max)
{
    if (!up()) return 0;
    libusb_device **list;
    ssize_t cnt = libusb_get_device_list(S.ctx, &list);
    int n = 0;
    for (ssize_t i = 0; i < cnt && n < max; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
        if (dd.bDeviceClass == 0x09) continue;          /* hubs are not listed on the board either */
        aos_usb_dev_t d = { 0 };
        d.addr = sim_addr(list[i]);
        if (!d.addr) continue;
        d.vid = dd.idVendor;
        d.pid = dd.idProduct;
        d.cls = dd.bDeviceClass;
        int sp = libusb_get_device_speed(list[i]);
        d.speed = sp >= LIBUSB_SPEED_HIGH ? 2 : sp == LIBUSB_SPEED_FULL ? 1 : 0;
        d.port = AOS_HAL_USB_HOST_OTG;
        if (!d.cls) {
            struct libusb_config_descriptor *c;
            if (libusb_get_active_config_descriptor(list[i], &c) == 0) {
                if (c->bNumInterfaces && c->interface[0].num_altsetting) {
                    d.cls = c->interface[0].altsetting[0].bInterfaceClass;
                    d.sub = c->interface[0].altsetting[0].bInterfaceSubClass;
                }
                libusb_free_config_descriptor(c);
            }
        }
        snprintf(d.product, sizeof d.product, "%04x:%04x (Mac)", d.vid, d.pid);
        pthread_mutex_lock(&S.mx);
        for (int k = 0; k < RAW_MAX; k++)
            if (S.r[k].used && S.r[k].addr == d.addr && !S.r[k].gone) snprintf(d.uses, sizeof d.uses, "app");
        pthread_mutex_unlock(&S.mx);
        out[n++] = d;
    }
    if (cnt >= 0) libusb_free_device_list(list, 1);
    return n;
}

aos_usb_raw_t *aos_hal_usb_raw_open(uint8_t addr, const char *owner)
{
    if (!up() || !addr) return NULL;
    struct aos_usb_raw *u = NULL;
    pthread_mutex_lock(&S.mx);
    for (int i = 0; i < RAW_MAX; i++) {
        if (S.r[i].used && S.r[i].addr == addr && !S.r[i].gone) {
            pthread_mutex_unlock(&S.mx);
            return NULL;
        }
        if (!S.r[i].used && !u) u = &S.r[i];
    }
    pthread_mutex_unlock(&S.mx);
    if (!u) return NULL;
    libusb_device **list;
    ssize_t cnt = libusb_get_device_list(S.ctx, &list);
    libusb_device_handle *h = NULL;
    for (ssize_t i = 0; i < cnt; i++)
        if (sim_addr(list[i]) == addr) {
            int r = libusb_open(list[i], &h);
            if (r) printf("[usb] raw: open %u: %s\n", addr, libusb_error_name(r));
            break;
        }
    if (cnt >= 0) libusb_free_device_list(list, 1);
    if (!h) return NULL;
    pthread_mutex_lock(&S.mx);
    memset(u, 0, sizeof *u);
    u->used = true;
    u->addr = addr;
    u->h = h;
    u->claimed = -1;
    pthread_mutex_unlock(&S.mx);
    printf("[usb] raw: device %u open for %s\n", addr, owner ? owner : "app");
    return u;
}

bool aos_hal_usb_raw_gone(aos_usb_raw_t *u)
{
    return !u || !u->used || u->gone;
}

static int result(int r)
{
    if (r >= 0) return r;
    switch (r) {
    case LIBUSB_ERROR_NO_DEVICE: return AOS_USB_RAW_GONE;
    case LIBUSB_ERROR_TIMEOUT: return AOS_USB_RAW_TIMEOUT;
    case LIBUSB_ERROR_PIPE: return AOS_USB_RAW_STALL;
    default: return AOS_USB_RAW_ERROR;
    }
}

bool aos_hal_usb_raw_claim(aos_usb_raw_t *u, int intf, int alt)
{
    if (aos_hal_usb_raw_gone(u)) return false;
    int r = libusb_claim_interface(u->h, intf);
    if (r == 0 && alt) r = libusb_set_interface_alt_setting(u->h, intf, alt);
    if (r) {
        printf("[usb] raw: claim %d: %s\n", intf, libusb_error_name(r));
        if (r == LIBUSB_ERROR_NO_DEVICE) u->gone = true;
        return false;
    }
    u->claimed = intf;
    return true;
}

int aos_hal_usb_raw_control(aos_usb_raw_t *u, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *data,
                            uint16_t len, int timeout_ms)
{
    if (aos_hal_usb_raw_gone(u)) return AOS_USB_RAW_GONE;
    int r = libusb_control_transfer(u->h, type, req, value, index, data, len, timeout_ms > 0 ? timeout_ms : 1000);
    if (r == LIBUSB_ERROR_NO_DEVICE) u->gone = true;
    return result(r);
}

int aos_hal_usb_raw_transfer(aos_usb_raw_t *u, uint8_t ep, void *data, int len, int timeout_ms)
{
    if (aos_hal_usb_raw_gone(u)) return AOS_USB_RAW_GONE;
    int got = 0;
    /* libusb tells bulk from interrupt by the endpoint itself only through
     * the descriptor: bulk is what these devices stream on; interrupt
     * endpoints answer LIBUSB_ERROR_INVALID_PARAM to a bulk transfer on
     * some platforms, then interrupt is tried */
    int r = libusb_bulk_transfer(u->h, ep, data, len, &got, timeout_ms > 0 ? timeout_ms : 1000);
    if (r == LIBUSB_ERROR_INVALID_PARAM || r == LIBUSB_ERROR_NOT_SUPPORTED)
        r = libusb_interrupt_transfer(u->h, ep, data, len, &got, timeout_ms > 0 ? timeout_ms : 1000);
    if (r == LIBUSB_ERROR_NO_DEVICE) u->gone = true;
    if (r == LIBUSB_ERROR_TIMEOUT && got) return got;
    return r ? result(r) : got;
}

static void LIBUSB_CALL stream_cb(struct libusb_transfer *t)
{
    struct aos_usb_raw *u = t->user_data;
    pthread_mutex_lock(&S.mx);
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        size_t n = t->actual_length;
        if (u->ring_size - u->fill >= n) {
            for (size_t k = 0; k < n;) {
                size_t run = u->ring_size - u->head;
                if (run > n - k) run = n - k;
                memcpy(u->ring + u->head, t->buffer + k, run);
                u->head = (u->head + run) % u->ring_size;
                k += run;
            }
            u->fill += n;
            pthread_cond_broadcast(&S.cv);
        } else u->dropped += n;
        u->bytes += n;
    } else if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
        u->gone = true;
        pthread_cond_broadcast(&S.cv);
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED) u->errors++;
    bool again = !u->st_stop && !u->gone && t->status != LIBUSB_TRANSFER_CANCELLED &&
                 t->status != LIBUSB_TRANSFER_NO_DEVICE && t->status != LIBUSB_TRANSFER_STALL;
    pthread_mutex_unlock(&S.mx);
    if (again && libusb_submit_transfer(t) == 0) return;
    pthread_mutex_lock(&S.mx);
    u->inflight--;
    pthread_cond_broadcast(&S.cv);
    pthread_mutex_unlock(&S.mx);
}

bool aos_hal_usb_raw_stream_start(aos_usb_raw_t *u, uint8_t ep, int xfer_bytes, int xfers, int ring_bytes)
{
    if (aos_hal_usb_raw_gone(u) || !(ep & 0x80) || u->st_n) return false;
    if (xfer_bytes <= 0) xfer_bytes = 16 * 1024;
    if (xfers <= 0) xfers = 4;
    if (xfers > XFERS_MAX) xfers = XFERS_MAX;
    if (ring_bytes <= 0) ring_bytes = 1024 * 1024;
    xfer_bytes = (xfer_bytes + 511) / 512 * 512;
    if (ring_bytes < 2 * xfer_bytes) ring_bytes = 2 * xfer_bytes;
    u->ring = malloc(ring_bytes);
    if (!u->ring) return false;
    u->ring_size = ring_bytes;
    u->head = u->tail = u->fill = 0;
    u->bytes = 0;
    u->dropped = u->errors = 0;
    u->st_stop = false;
    for (int i = 0; i < xfers; i++) {
        struct libusb_transfer *t = libusb_alloc_transfer(0);
        uint8_t *b = malloc(xfer_bytes);
        if (!t || !b) {
            free(b);
            libusb_free_transfer(t);
            break;
        }
        libusb_fill_bulk_transfer(t, u->h, ep, b, xfer_bytes, stream_cb, u, 0);
        t->flags = LIBUSB_TRANSFER_FREE_BUFFER;
        u->st[u->st_n++] = t;
    }
    for (int i = 0; i < u->st_n; i++) {
        pthread_mutex_lock(&S.mx);
        u->inflight++;
        pthread_mutex_unlock(&S.mx);
        if (libusb_submit_transfer(u->st[i]) != 0) {
            pthread_mutex_lock(&S.mx);
            u->inflight--;
            pthread_mutex_unlock(&S.mx);
            aos_hal_usb_raw_stream_stop(u);
            return false;
        }
    }
    printf("[usb] raw: device %u streaming from %02x, %d x %d bytes, ring %d KB\n", u->addr, ep, u->st_n, xfer_bytes,
           ring_bytes / 1024);
    return true;
}

int aos_hal_usb_raw_stream_read(aos_usb_raw_t *u, void *buf, int len, int timeout_ms)
{
    if (!u || !u->used || !u->ring) return AOS_USB_RAW_ERROR;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&S.mx);
    while (!u->fill && !u->gone)
        if (pthread_cond_timedwait(&S.cv, &S.mx, &until)) break;
    size_t n = u->fill < (size_t)len ? u->fill : (size_t)len;
    for (size_t k = 0; k < n;) {
        size_t run = u->ring_size - u->tail;
        if (run > n - k) run = n - k;
        memcpy((uint8_t *)buf + k, u->ring + u->tail, run);
        u->tail = (u->tail + run) % u->ring_size;
        k += run;
    }
    u->fill -= n;
    bool gone = u->gone;
    pthread_mutex_unlock(&S.mx);
    if (!n && gone) return AOS_USB_RAW_GONE;
    return (int)n;
}

void aos_hal_usb_raw_stream_stats(aos_usb_raw_t *u, uint64_t *bytes, uint32_t *dropped, uint32_t *errors)
{
    if (bytes) *bytes = u ? u->bytes : 0;
    if (dropped) *dropped = u ? u->dropped : 0;
    if (errors) *errors = u ? u->errors : 0;
}

void aos_hal_usb_raw_stream_stop(aos_usb_raw_t *u)
{
    if (!u || !u->used || !u->st_n) return;
    pthread_mutex_lock(&S.mx);
    u->st_stop = true;
    pthread_mutex_unlock(&S.mx);
    for (int i = 0; i < u->st_n; i++) libusb_cancel_transfer(u->st[i]);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 1;
    pthread_mutex_lock(&S.mx);
    while (u->inflight)
        if (pthread_cond_timedwait(&S.cv, &S.mx, &until)) break;
    int left = u->inflight;
    pthread_mutex_unlock(&S.mx);
    if (!left)
        for (int i = 0; i < u->st_n; i++) libusb_free_transfer(u->st[i]);
    printf("[usb] raw: device %u stream stopped: %llu bytes, %u dropped, %u errors%s\n", u->addr,
           (unsigned long long)u->bytes, u->dropped, u->errors, left ? " (transfers left out)" : "");
    memset(u->st, 0, sizeof u->st);
    u->st_n = 0;
    free(u->ring);
    u->ring = NULL;
}

void aos_hal_usb_raw_close(aos_usb_raw_t *u)
{
    if (!u || !u->used) return;
    aos_hal_usb_raw_stream_stop(u);
    if (u->claimed >= 0) libusb_release_interface(u->h, u->claimed);
    libusb_close(u->h);
    printf("[usb] raw: device %u closed\n", u->addr);
    pthread_mutex_lock(&S.mx);
    memset(u, 0, sizeof *u);
    pthread_mutex_unlock(&S.mx);
}
