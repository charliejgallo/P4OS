/*
 * P4OS - raw access to a USB device on the host, for an app that brings its
 * own driver (aos_hal.h, docs/USB.md "Raw devices").
 *
 * The board's drivers each take a kind of device (pendrives, HID, MIDI,
 * serial, webcams, sound cards). A vendor-class device - a software radio
 * (RTL-SDR), a logic analyser, a programmer - is none of those, and its
 * driver is a lot of code that only one app wants: it lives in that app,
 * which talks to the device through this. The firmware stays small and free
 * of the drivers' licences (librtlsdr is GPL; the RF app carries it, as the
 * Doom app carries its engine), and a new device needs a new app, not a new
 * firmware.
 *
 * One client of the host library of its own, with its own task: every
 * transfer's callback runs there. An app opens a device by its address in
 * aos_hal_usb_devices(), claims an interface, and then has:
 *   - control transfers, synchronous;
 *   - bulk and interrupt transfers, synchronous, one at a time;
 *   - a stream: several IN transfers kept in flight on one endpoint, each
 *     one put back as soon as it completes, their data copied into a ring
 *     in PSRAM that the app reads at its pace. A software radio at 2.4 Msps
 *     is 4.8 MB/s that must not stop: the ring takes the app's hiccups, and
 *     what does not fit is dropped whole transfer by whole transfer (so
 *     I/Q pairs never split) and counted.
 *
 * The transfers' buffers are the host library's, in internal RAM like all
 * of its DMA buffers (docs/USB.md, "Webcams": in PSRAM they read garbage),
 * and exist only while a stream runs: 4 x 16 KB is the default.
 *
 * Unplugged while open: the slot is marked gone, every call answers
 * AOS_USB_RAW_GONE, and the client task lets go of the device as soon as
 * no transfer is in flight (the library wants every client to close it).
 * The slot itself stays until the app closes it, so the pointer it holds
 * never dangles, not even across the host being switched off.
 */
#include "aos_hal.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "usb/usb_host.h"

static const char *TAG = "usb";
#define RAW_MAX 4
#define CTRL_DATA 1024              /* the largest control transfer kept allocated; bigger ones get their own */
#define STREAM_XFERS_MAX 16

void aos_p4_usb_dev_use(uint8_t addr, const char *what);

struct aos_usb_raw {
    bool used;
    uint8_t addr;
    usb_device_handle_t dev;        /* NULL once let go of */
    volatile bool gone;
    int claimed;                    /* the interface, -1 none */
    uint8_t claimed_alt;
    int inflight;                   /* transfers submitted and not called back (atomic: two cores) */
    bool orphan;                    /* closed by its app with a transfer still out: freed once it is back */
    /* synchronous transfers: one at a time per device, as the app's thread
     * makes them */
    SemaphoreHandle_t op;           /* serialises the app's calls */
    SemaphoreHandle_t done;         /* given by the callbacks */
    usb_transfer_t *ctrl, *xfer;
    volatile bool ctrl_busy, xfer_busy;     /* a timed-out transfer still in flight */
    /* the stream */
    usb_transfer_t *st[STREAM_XFERS_MAX];
    int st_n;
    uint8_t st_ep;
    volatile bool st_stop;
    StreamBufferHandle_t ring;
    uint64_t st_bytes;
    uint32_t st_dropped, st_errors;
    char owner[24];
};

/* the app's thread submits, the client task's callbacks complete: on two
 * cores, so the count goes through atomics */
static inline void inflight_add(struct aos_usb_raw *u, int d)
{
    __atomic_add_fetch(&u->inflight, d, __ATOMIC_SEQ_CST);
}

static inline int inflight(struct aos_usb_raw *u)
{
    return __atomic_load_n(&u->inflight, __ATOMIC_SEQ_CST);
}

static struct {
    usb_host_client_handle_t client;
    volatile bool stop, done;
    SemaphoreHandle_t mx;           /* guards dev/gone/inflight against the client task letting go */
    struct aos_usb_raw r[RAW_MAX];
} R;

/* Let go of a gone device once nothing is in flight. The client task, and
 * stop(). Under R.mx. */
static void let_go(struct aos_usb_raw *u)
{
    if (!u->dev || inflight(u)) return;
    if (u->claimed >= 0) usb_host_interface_release(R.client, u->dev, u->claimed);
    usb_host_device_close(R.client, u->dev);
    u->dev = NULL;
    u->claimed = -1;
    ESP_LOGI(TAG, "raw: device %u let go", u->addr);
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event != USB_HOST_CLIENT_EVENT_DEV_GONE) return;
    xSemaphoreTake(R.mx, portMAX_DELAY);
    for (int i = 0; i < RAW_MAX; i++) {
        struct aos_usb_raw *u = &R.r[i];
        if (u->used && u->dev == msg->dev_gone.dev_hdl) {
            ESP_LOGI(TAG, "raw: device %u (%s) unplugged", u->addr, u->owner);
            /* the stream's transfers come back with NO_DEVICE and are not
             * put back; a synchronous one wakes its caller the same way */
            u->gone = true;
        }
    }
    xSemaphoreGive(R.mx);
}

static void raw_task(void *arg)
{
    (void)arg;
    while (!R.stop) {
        usb_host_client_handle_events(R.client, pdMS_TO_TICKS(50));
        xSemaphoreTake(R.mx, portMAX_DELAY);
        for (int i = 0; i < RAW_MAX; i++) {
            struct aos_usb_raw *u = &R.r[i];
            if (!u->used || !u->gone) continue;
            let_go(u);
            if (u->orphan && !u->dev) {
                /* its app closed it while a transfer was out: done now */
                if (u->ctrl) usb_host_transfer_free(u->ctrl);
                if (u->xfer) usb_host_transfer_free(u->xfer);
                u->ctrl = u->xfer = NULL;
                u->orphan = false;
                u->used = false;
            }
        }
        xSemaphoreGive(R.mx);
    }
    R.done = true;
    vTaskDeleteWithCaps(NULL);
}

static void xfer_cb(usb_transfer_t *t)
{
    struct aos_usb_raw *u = t->context;
    if (t == u->xfer) u->xfer_busy = false;
    else u->ctrl_busy = false;      /* u->ctrl, or a big one of its own */
    inflight_add(u, -1);
    xSemaphoreGive(u->done);
}

static void stream_cb(usb_transfer_t *t)
{
    struct aos_usb_raw *u = t->context;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        int n = t->actual_num_bytes;
        if (n > 0) {
            if (xStreamBufferSpacesAvailable(u->ring) >= (size_t)n) xStreamBufferSend(u->ring, t->data_buffer, n, 0);
            else u->st_dropped += n;
            u->st_bytes += n;
        }
    } else if (t->status != USB_TRANSFER_STATUS_CANCELED && t->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        u->st_errors++;
    }
    if (!u->st_stop && !u->gone && t->status != USB_TRANSFER_STATUS_NO_DEVICE &&
        t->status != USB_TRANSFER_STATUS_CANCELED && t->status != USB_TRANSFER_STATUS_STALL) {
        t->num_bytes = t->data_buffer_size;
        if (usb_host_transfer_submit(t) == ESP_OK) return;
        u->st_errors++;
    }
    inflight_add(u, -1);
}

bool aos_p4_usb_raw_start(void)
{
    if (!R.mx) R.mx = xSemaphoreCreateMutex();
    R.stop = R.done = false;
    const usb_host_client_config_t cc = { .is_synchronous = false, .max_num_event_msg = 8,
                                          .async = { .client_event_callback = client_event } };
    if (usb_host_client_register(&cc, &R.client) != ESP_OK) return false;
    /* nothing it runs touches the flash: its stack can be PSRAM */
    if (xTaskCreatePinnedToCoreWithCaps(raw_task, "usb_raw", 4096, NULL, 5, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        usb_host_client_deregister(R.client);
        R.client = NULL;
        return false;
    }
    return true;
}

void aos_p4_usb_raw_stop(void)
{
    if (!R.client) return;
    /* every open device is gone for its app: stop the streams, let go */
    xSemaphoreTake(R.mx, portMAX_DELAY);
    for (int i = 0; i < RAW_MAX; i++) {
        struct aos_usb_raw *u = &R.r[i];
        if (!u->used || !u->dev) continue;
        u->gone = true;
        u->st_stop = true;
        if (u->st_n) {
            usb_host_endpoint_halt(u->dev, u->st_ep);
            usb_host_endpoint_flush(u->dev, u->st_ep);
        }
    }
    xSemaphoreGive(R.mx);
    /* the task keeps handling events (the cancelled transfers' callbacks)
     * and lets go of each device as its transfers come back */
    for (int t = 0; t < 50; t++) {
        bool busy = false;
        xSemaphoreTake(R.mx, portMAX_DELAY);
        for (int i = 0; i < RAW_MAX; i++) busy |= R.r[i].used && R.r[i].dev;
        xSemaphoreGive(R.mx);
        if (!busy) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    R.stop = true;
    for (int i = 0; i < 100 && !R.done; i++) {
        usb_host_client_unblock(R.client);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    xSemaphoreTake(R.mx, portMAX_DELAY);
    for (int i = 0; i < RAW_MAX; i++)
        if (R.r[i].used && R.r[i].dev) {
            /* a transfer that never came back: the library goes down anyway */
            __atomic_store_n(&R.r[i].inflight, 0, __ATOMIC_SEQ_CST);
            let_go(&R.r[i]);
        }
    xSemaphoreGive(R.mx);
    usb_host_client_deregister(R.client);
    R.client = NULL;
}

aos_usb_raw_t *aos_hal_usb_raw_open(uint8_t addr, const char *owner)
{
    if (!R.client || !addr) return NULL;
    xSemaphoreTake(R.mx, portMAX_DELAY);
    struct aos_usb_raw *u = NULL;
    for (int i = 0; i < RAW_MAX; i++) {
        if (R.r[i].used && R.r[i].addr == addr && !R.r[i].gone) {
            xSemaphoreGive(R.mx);
            ESP_LOGW(TAG, "raw: device %u is already open (%s)", addr, R.r[i].owner);
            return NULL;
        }
        if (!R.r[i].used && !u) u = &R.r[i];
    }
    usb_device_handle_t dev = NULL;
    if (!u || usb_host_device_open(R.client, addr, &dev) != ESP_OK) {
        xSemaphoreGive(R.mx);
        ESP_LOGW(TAG, "raw: cannot open device %u", addr);
        return NULL;
    }
    SemaphoreHandle_t op = u->op, done = u->done;      /* kept from an earlier use of the slot */
    memset(u, 0, sizeof *u);
    u->op = op ? op : xSemaphoreCreateMutex();
    u->done = done ? done : xSemaphoreCreateBinary();
    u->used = true;
    u->addr = addr;
    u->dev = dev;
    u->claimed = -1;
    snprintf(u->owner, sizeof u->owner, "%s", owner && *owner ? owner : "app");
    xSemaphoreGive(R.mx);
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + CTRL_DATA, 0, &u->ctrl) != ESP_OK) {
        aos_hal_usb_raw_close(u);
        return NULL;
    }
    aos_p4_usb_dev_use(addr, u->owner);
    ESP_LOGI(TAG, "raw: device %u open for %s", addr, u->owner);
    return u;
}

bool aos_hal_usb_raw_gone(aos_usb_raw_t *u)
{
    return !u || !u->used || u->gone;
}

bool aos_hal_usb_raw_claim(aos_usb_raw_t *u, int intf, int alt)
{
    if (aos_hal_usb_raw_gone(u)) return false;
    xSemaphoreTake(u->op, portMAX_DELAY);
    bool ok = false;
    xSemaphoreTake(R.mx, portMAX_DELAY);
    if (u->dev && !u->gone) {
        if (u->claimed >= 0 && u->claimed != intf) usb_host_interface_release(R.client, u->dev, u->claimed);
        esp_err_t e = u->claimed == intf && u->claimed_alt == alt ? ESP_OK
                      : usb_host_interface_claim(R.client, u->dev, intf, alt);
        ok = e == ESP_OK;
        if (ok) {
            u->claimed = intf;
            u->claimed_alt = alt;
        } else ESP_LOGW(TAG, "raw: device %u interface %d: %s", u->addr, intf, esp_err_to_name(e));
    }
    xSemaphoreGive(R.mx);
    xSemaphoreGive(u->op);
    return ok;
}

/* Submit under R.mx, so the client task cannot let go of the device between
 * the check and the submit. */
static esp_err_t submit(struct aos_usb_raw *u, usb_transfer_t *t, bool control)
{
    xSemaphoreTake(R.mx, portMAX_DELAY);
    esp_err_t e = ESP_ERR_INVALID_STATE;
    if (u->dev && !u->gone) {
        t->device_handle = u->dev;
        inflight_add(u, 1);
        e = control ? usb_host_transfer_submit_control(R.client, t) : usb_host_transfer_submit(t);
        if (e != ESP_OK) inflight_add(u, -1);
    }
    xSemaphoreGive(R.mx);
    return e;
}

/* wait for a transfer of ours; false on timeout (it is then still in flight) */
static bool wait_done(struct aos_usb_raw *u, volatile bool *busy, int timeout_ms)
{
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 1000);
    while (*busy) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(until - now) <= 0) return false;
        xSemaphoreTake(u->done, until - now);
    }
    return true;
}

static int status_result(const usb_transfer_t *t, int bytes)
{
    switch (t->status) {
    case USB_TRANSFER_STATUS_COMPLETED: return bytes;
    case USB_TRANSFER_STATUS_NO_DEVICE: return AOS_USB_RAW_GONE;
    case USB_TRANSFER_STATUS_TIMED_OUT: return AOS_USB_RAW_TIMEOUT;
    case USB_TRANSFER_STATUS_STALL: return AOS_USB_RAW_STALL;
    default: return AOS_USB_RAW_ERROR;
    }
}

int aos_hal_usb_raw_control(aos_usb_raw_t *u, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *data,
                            uint16_t len, int timeout_ms)
{
    if (aos_hal_usb_raw_gone(u)) return AOS_USB_RAW_GONE;
    xSemaphoreTake(u->op, portMAX_DELAY);
    int r;
    /* a control transfer that timed out earlier may still be out: EP0 cannot
     * be halted, so it is waited for */
    if (u->ctrl_busy && !wait_done(u, &u->ctrl_busy, timeout_ms)) {
        r = AOS_USB_RAW_TIMEOUT;
        goto out;
    }
    usb_transfer_t *t = u->ctrl;
    bool own = len > CTRL_DATA;
    if (own && usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + len, 0, &t) != ESP_OK) {
        r = AOS_USB_RAW_ERROR;
        goto out;
    }
    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = type;
    s->bRequest = req;
    s->wValue = value;
    s->wIndex = index;
    s->wLength = len;
    bool in = type & 0x80;
    if (!in && len) memcpy(t->data_buffer + sizeof *s, data, len);
    t->num_bytes = sizeof *s + len;
    t->bEndpointAddress = 0;
    t->callback = xfer_cb;
    t->context = u;
    t->timeout_ms = timeout_ms > 0 ? timeout_ms : 1000;
    xSemaphoreTake(u->done, 0);
    u->ctrl_busy = true;
    esp_err_t e = submit(u, t, true);
    if (e != ESP_OK) {
        u->ctrl_busy = false;
        r = u->gone ? AOS_USB_RAW_GONE : AOS_USB_RAW_ERROR;
    } else if (!wait_done(u, &u->ctrl_busy, timeout_ms + 100)) {
        r = AOS_USB_RAW_TIMEOUT;
        own = false;                /* still in flight: freed when the slot is */
    } else {
        int got = t->actual_num_bytes - (int)sizeof *s;
        if (got < 0) got = 0;
        r = status_result(t, in ? got : len);
        if (r > 0 && in) memcpy(data, t->data_buffer + sizeof *s, r);
    }
    if (own) usb_host_transfer_free(t);
out:
    xSemaphoreGive(u->op);
    return r;
}

/* the endpoint's packet size on the claimed interface, 0 if it is not
 * there; under R.mx, as the client task may be letting go of the device */
static int ep_mps(struct aos_usb_raw *u, uint8_t ep)
{
    int mps = 0;
    xSemaphoreTake(R.mx, portMAX_DELAY);
    const usb_config_desc_t *cfg;
    if (u->dev && !u->gone && u->claimed >= 0 && usb_host_get_active_config_descriptor(u->dev, &cfg) == ESP_OK) {
        int off = 0;
        const usb_ep_desc_t *d = usb_parse_endpoint_descriptor_by_address(cfg, u->claimed, u->claimed_alt, ep, &off);
        if (d) mps = USB_EP_DESC_GET_MPS(d);
    }
    xSemaphoreGive(R.mx);
    return mps;
}

/* cancel what is in flight on an endpoint and make it usable again */
static void ep_cancel(struct aos_usb_raw *u, uint8_t ep, volatile bool *busy_flag, int wait_ms)
{
    xSemaphoreTake(R.mx, portMAX_DELAY);
    if (u->dev && !u->gone) {
        usb_host_endpoint_halt(u->dev, ep);
        usb_host_endpoint_flush(u->dev, ep);
    }
    xSemaphoreGive(R.mx);
    if (busy_flag) wait_done(u, busy_flag, wait_ms);
    else
        for (int i = 0; i < wait_ms / 5 && inflight(u); i++) vTaskDelay(pdMS_TO_TICKS(5));
    xSemaphoreTake(R.mx, portMAX_DELAY);
    if (u->dev && !u->gone) usb_host_endpoint_clear(u->dev, ep);
    xSemaphoreGive(R.mx);
}

int aos_hal_usb_raw_transfer(aos_usb_raw_t *u, uint8_t ep, void *data, int len, int timeout_ms)
{
    if (aos_hal_usb_raw_gone(u)) return AOS_USB_RAW_GONE;
    if (len < 0) return AOS_USB_RAW_ERROR;
    xSemaphoreTake(u->op, portMAX_DELAY);
    int r = AOS_USB_RAW_ERROR;
    int mps = ep_mps(u, ep);
    if (!mps || u->xfer_busy) goto out;
    bool in = ep & 0x80;
    /* an IN transfer asks for whole packets */
    int size = in ? (len + mps - 1) / mps * mps : len;
    if (!size) size = mps;
    if (!u->xfer || (int)u->xfer->data_buffer_size < size) {
        if (u->xfer) usb_host_transfer_free(u->xfer);
        u->xfer = NULL;
        if (usb_host_transfer_alloc(size, 0, &u->xfer) != ESP_OK) goto out;
    }
    usb_transfer_t *t = u->xfer;
    if (!in && len) memcpy(t->data_buffer, data, len);
    t->num_bytes = in ? size : len;
    t->bEndpointAddress = ep;
    t->callback = xfer_cb;
    t->context = u;
    t->timeout_ms = 0;
    xSemaphoreTake(u->done, 0);
    u->xfer_busy = true;
    if (submit(u, t, false) != ESP_OK) {
        u->xfer_busy = false;
        r = u->gone ? AOS_USB_RAW_GONE : AOS_USB_RAW_ERROR;
        goto out;
    }
    if (!wait_done(u, &u->xfer_busy, timeout_ms)) {
        /* the library has no timeout for bulk: halt and flush, and the
         * transfer comes back cancelled */
        ep_cancel(u, ep, &u->xfer_busy, 500);
        r = u->xfer_busy ? AOS_USB_RAW_ERROR : AOS_USB_RAW_TIMEOUT;
        if (t->status == USB_TRANSFER_STATUS_COMPLETED && !u->xfer_busy) r = t->actual_num_bytes;
        goto out_copy;
    }
    r = status_result(t, t->actual_num_bytes);
    if (r == AOS_USB_RAW_STALL) {
        /* a stalled endpoint stays halted until cleared */
        ep_cancel(u, ep, NULL, 0);
    }
out_copy:
    if (r > 0 && in) memcpy(data, t->data_buffer, r > len ? len : r);
    if (r > len && in) r = len;
out:
    xSemaphoreGive(u->op);
    return r;
}

bool aos_hal_usb_raw_stream_start(aos_usb_raw_t *u, uint8_t ep, int xfer_bytes, int xfers, int ring_bytes)
{
    if (aos_hal_usb_raw_gone(u) || !(ep & 0x80) || u->st_n) return false;
    xSemaphoreTake(u->op, portMAX_DELAY);
    bool ok = false;
    int mps = ep_mps(u, ep);
    if (!mps) {
        ESP_LOGW(TAG, "raw: endpoint %02x is not on the claimed interface", ep);
        goto out;
    }
    if (xfer_bytes <= 0) xfer_bytes = 16 * 1024;
    if (xfers <= 0) xfers = 4;
    if (xfers > STREAM_XFERS_MAX) xfers = STREAM_XFERS_MAX;
    if (ring_bytes <= 0) ring_bytes = 1024 * 1024;
    xfer_bytes = (xfer_bytes + mps - 1) / mps * mps;
    if (ring_bytes < 2 * xfer_bytes) ring_bytes = 2 * xfer_bytes;
    u->ring = xStreamBufferCreateWithCaps(ring_bytes, xfer_bytes, MALLOC_CAP_SPIRAM);
    if (!u->ring) goto out;
    u->st_ep = ep;
    u->st_stop = false;
    u->st_bytes = 0;
    u->st_dropped = u->st_errors = 0;
    for (int i = 0; i < xfers; i++) {
        if (usb_host_transfer_alloc(xfer_bytes, 0, &u->st[i]) != ESP_OK) {
            ESP_LOGW(TAG, "raw: stream: no memory for transfer %d of %d x %d bytes (internal RAM)", i + 1, xfers,
                     xfer_bytes);
            break;
        }
        u->st_n = i + 1;
        usb_transfer_t *t = u->st[i];
        t->num_bytes = xfer_bytes;
        t->bEndpointAddress = ep;
        t->callback = stream_cb;
        t->context = u;
        t->timeout_ms = 0;
    }
    if (u->st_n < 2) goto fail;
    for (int i = 0; i < u->st_n; i++)
        if (submit(u, u->st[i], false) != ESP_OK) {
            ESP_LOGW(TAG, "raw: stream: submit failed");
            goto fail;
        }
    ESP_LOGI(TAG, "raw: device %u streaming from %02x, %d x %d bytes in flight, ring %d KB", u->addr, ep, u->st_n,
             xfer_bytes, ring_bytes / 1024);
    ok = true;
    goto out;
fail:
    xSemaphoreGive(u->op);
    aos_hal_usb_raw_stream_stop(u);
    return false;
out:
    xSemaphoreGive(u->op);
    return ok;
}

int aos_hal_usb_raw_stream_read(aos_usb_raw_t *u, void *buf, int len, int timeout_ms)
{
    if (!u || !u->used || !u->ring) return AOS_USB_RAW_ERROR;
    size_t n = xStreamBufferReceive(u->ring, buf, len, pdMS_TO_TICKS(timeout_ms));
    if (!n && u->gone) return AOS_USB_RAW_GONE;
    return (int)n;
}

void aos_hal_usb_raw_stream_stats(aos_usb_raw_t *u, uint64_t *bytes, uint32_t *dropped, uint32_t *errors)
{
    if (bytes) *bytes = u ? u->st_bytes : 0;
    if (dropped) *dropped = u ? u->st_dropped : 0;
    if (errors) *errors = u ? u->st_errors : 0;
}

void aos_hal_usb_raw_stream_stop(aos_usb_raw_t *u)
{
    if (!u || !u->used || (!u->st_n && !u->ring)) return;
    xSemaphoreTake(u->op, portMAX_DELAY);
    u->st_stop = true;
    if (inflight(u)) ep_cancel(u, u->st_ep, NULL, 500);
    if (inflight(u)) {
        /* gone, or the library never answered: the transfers cannot be
         * freed while out; they leak rather than be freed under the DMA */
        ESP_LOGW(TAG, "raw: stream on device %u: %d transfers did not come back", u->addr, inflight(u));
    } else {
        for (int i = 0; i < u->st_n; i++) usb_host_transfer_free(u->st[i]);
    }
    memset(u->st, 0, sizeof u->st);
    u->st_n = 0;
    if (u->ring) vStreamBufferDeleteWithCaps(u->ring);
    u->ring = NULL;
    ESP_LOGI(TAG, "raw: device %u stream stopped: %llu bytes, %u dropped, %u errors", u->addr,
             (unsigned long long)u->st_bytes, (unsigned)u->st_dropped, (unsigned)u->st_errors);
    xSemaphoreGive(u->op);
}

void aos_hal_usb_raw_close(aos_usb_raw_t *u)
{
    if (!u || !u->used) return;
    aos_hal_usb_raw_stream_stop(u);
    xSemaphoreTake(u->op, portMAX_DELAY);
    /* a timed-out transfer still out: given a moment, then left to the
     * library (it is freed with the slot only if it came back) */
    for (int i = 0; i < 40 && (u->ctrl_busy || u->xfer_busy); i++) vTaskDelay(pdMS_TO_TICKS(5));
    xSemaphoreTake(R.mx, portMAX_DELAY);
    if (u->dev && !inflight(u)) {
        if (u->claimed >= 0) usb_host_interface_release(R.client, u->dev, u->claimed);
        usb_host_device_close(R.client, u->dev);
        u->dev = NULL;
        u->claimed = -1;
    }
    bool free_now = !u->dev && !inflight(u);
    if (free_now) {
        if (u->ctrl) usb_host_transfer_free(u->ctrl);
        if (u->xfer) usb_host_transfer_free(u->xfer);
        u->ctrl = u->xfer = NULL;
        u->used = false;
    } else {
        /* a transfer still out: the client task lets go of the device and
         * frees the slot once it is back (the app's pointer is dead now) */
        u->gone = true;
        u->orphan = true;
    }
    xSemaphoreGive(R.mx);
    ESP_LOGI(TAG, "raw: device %u closed%s", u->addr, free_now ? "" : " (a transfer still out)");
    xSemaphoreGive(u->op);
}
