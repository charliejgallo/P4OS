/*
 * P4OS - USB MIDI devices on the host: keyboards, pads, controllers
 * (aos_hal.h, docs/USB.md).
 *
 * A client of ESP-IDF's USB Host Library. A device with a MIDI streaming
 * interface (audio class 1, subclass 3) has its bulk IN endpoint read with
 * one transfer that resubmits itself, and its bulk OUT one, if any, written
 * on demand. USB-MIDI carries messages as 4-byte event packets: the cable
 * and a code index in the first byte, the MIDI message in the other three;
 * system exclusive comes in pieces, and is left out here.
 *
 * What comes in goes to a queue (aos_hal_midi_read) that drops the oldest
 * when nobody reads it, and to the last few messages that /api/usb shows.
 * And it goes on to the computer (MIDI thru) when the OTG connector is the
 * computer's MIDI port at the same time (keyboard mode, with the host on
 * pins 21/23): a MIDI keyboard on the board plays on the Mac.
 */
#include "aos_hal.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"

static const char *TAG = "usb";
#define MIDI_MAX 2
#define LAST_N   8

void aos_p4_usb_dev_use(uint8_t addr, const char *what);

typedef struct {
    usb_device_handle_t dev;
    uint8_t addr, intf, ep_in, ep_out;
    uint16_t mps_in, mps_out;
    usb_transfer_t *in, *out;
    volatile bool gone, in_busy, out_busy;
    bool halted;
    char name[48];
} midi_t;

static struct {
    usb_host_client_handle_t client;
    volatile bool stop, done;
    QueueHandle_t q;
    SemaphoreHandle_t mx;
    midi_t m[MIDI_MAX];
    aos_midi_msg_t last[LAST_N];
    int nlast;
} M;

/* the length of a MIDI message from its code index number (USB-MIDI 1.0) */
static int cin_len(uint8_t cin)
{
    static const int8_t L[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };
    return L[cin & 15];
}

static void got(const midi_t *m, const uint8_t *p)
{
    uint8_t cin = p[0] & 15;
    /* system exclusive (4..7) left out; the rest are whole messages */
    if (cin < 8 && cin != 5) return;
    if (cin_len(cin) == 0) return;
    aos_midi_msg_t msg = { .status = p[1], .data1 = p[2], .data2 = p[3], .cable = p[0] >> 4 };
    if (xQueueSend(M.q, &msg, 0) != pdTRUE) {
        aos_midi_msg_t old;
        xQueueReceive(M.q, &old, 0);            /* nobody reads: the oldest goes */
        xQueueSend(M.q, &msg, 0);
    }
    xSemaphoreTake(M.mx, portMAX_DELAY);
    memmove(M.last + 1, M.last, (LAST_N - 1) * sizeof M.last[0]);
    M.last[0] = msg;
    if (M.nlast < LAST_N) M.nlast++;
    xSemaphoreGive(M.mx);
    /* MIDI thru to the computer, when the OTG connector is its MIDI port */
    if (aos_hal_usb_midi_ready()) {
        uint8_t kind = msg.status & 0xF0;
        if (kind == 0x90) aos_hal_usb_midi_note(msg.data1, msg.data2, msg.data2 != 0);
        else if (kind == 0x80) aos_hal_usb_midi_note(msg.data1, msg.data2, false);
        else if (kind == 0xB0) aos_hal_usb_midi_cc(msg.data1, msg.data2);
        else if (kind == 0xE0) aos_hal_usb_midi_bend(((msg.data2 << 7) | msg.data1) - 8192);
    }
}

static void in_done(usb_transfer_t *t)
{
    midi_t *m = t->context;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED)
        for (int i = 0; i + 4 <= t->actual_num_bytes; i += 4)
            if (t->data_buffer[i] || t->data_buffer[i + 1]) got(m, t->data_buffer + i);
    bool again = t->status != USB_TRANSFER_STATUS_NO_DEVICE && t->status != USB_TRANSFER_STATUS_CANCELED &&
                 !m->gone && !M.stop;
    m->in_busy = again && usb_host_transfer_submit(t) == ESP_OK;
}

static void out_done(usb_transfer_t *t)
{
    ((midi_t *)t->context)->out_busy = false;
}

static void wide_to_str(const usb_str_desc_t *d, char *out, size_t n)
{
    size_t i = 0;
    if (d)
        for (int k = 0; k < (d->bLength - 2) / 2 && i + 1 < n; k++) {
            uint16_t w = d->wData[k];
            out[i++] = w >= 32 && w < 127 ? (char)w : '?';
        }
    out[i] = 0;
}

static void midi_free(midi_t *m)
{
    if (m->in) usb_host_transfer_free(m->in);
    if (m->out) usb_host_transfer_free(m->out);
    if (m->dev) {
        usb_host_interface_release(M.client, m->dev, m->intf);
        usb_host_device_close(M.client, m->dev);
    }
    xSemaphoreTake(M.mx, portMAX_DELAY);
    memset(m, 0, sizeof *m);
    xSemaphoreGive(M.mx);
}

static void dev_new(uint8_t addr)
{
    usb_device_handle_t dev;
    if (usb_host_device_open(M.client, addr, &dev) != ESP_OK) return;
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) { usb_host_device_close(M.client, dev); return; }
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    const usb_intf_desc_t *intf = NULL;
    uint8_t ep_in = 0, ep_out = 0;
    uint16_t mps_in = 0, mps_out = 0;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off))) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            if (intf && ep_in) break;
            const usb_intf_desc_t *it = (const usb_intf_desc_t *)d;
            intf = it->bInterfaceClass == 0x01 && it->bInterfaceSubClass == 0x03 ? it : NULL;
            ep_in = ep_out = 0;
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && intf) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
            if (USB_EP_DESC_GET_XFERTYPE(e) != USB_TRANSFER_TYPE_BULK && USB_EP_DESC_GET_XFERTYPE(e) != USB_TRANSFER_TYPE_INTR)
                continue;
            if (USB_EP_DESC_GET_EP_DIR(e) && !ep_in) { ep_in = e->bEndpointAddress; mps_in = USB_EP_DESC_GET_MPS(e); }
            if (!USB_EP_DESC_GET_EP_DIR(e) && !ep_out) { ep_out = e->bEndpointAddress; mps_out = USB_EP_DESC_GET_MPS(e); }
        }
    }
    midi_t *m = NULL;
    for (int i = 0; i < MIDI_MAX && !m; i++) if (!M.m[i].dev) m = &M.m[i];
    if (!intf || !ep_in || !m) {
        if (intf && !m) ESP_LOGW(TAG, "MIDI: more than %d devices, left alone", MIDI_MAX);
        usb_host_device_close(M.client, dev);
        return;
    }
    m->dev = dev;
    m->addr = addr;
    m->intf = intf->bInterfaceNumber;
    m->ep_in = ep_in;
    m->ep_out = ep_out;
    m->mps_in = mps_in;
    m->mps_out = mps_out;
    if (usb_host_interface_claim(M.client, dev, m->intf, intf->bAlternateSetting) != ESP_OK ||
        usb_host_transfer_alloc(mps_in, 0, &m->in) != ESP_OK ||
        (ep_out && usb_host_transfer_alloc(mps_out < 4 ? 4 : mps_out, 0, &m->out) != ESP_OK)) {
        ESP_LOGW(TAG, "MIDI at address %u: could not claim it", addr);
        midi_free(m);
        return;
    }
    usb_device_info_t di;
    char product[40] = "", vendor[24] = "";
    if (usb_host_device_info(dev, &di) == ESP_OK) {
        wide_to_str(di.str_desc_manufacturer, vendor, sizeof vendor);
        wide_to_str(di.str_desc_product, product, sizeof product);
    }
    xSemaphoreTake(M.mx, portMAX_DELAY);
    snprintf(m->name, sizeof m->name, "%s%s%s", vendor, *vendor && *product ? " " : "", product);
    xSemaphoreGive(M.mx);
    m->in->num_bytes = mps_in;
    m->in->device_handle = dev;
    m->in->bEndpointAddress = ep_in;
    m->in->callback = in_done;
    m->in->context = m;
    esp_err_t e = usb_host_transfer_submit(m->in);
    m->in_busy = e == ESP_OK;
    aos_p4_usb_dev_use(addr, "MIDI");
    ESP_LOGI(TAG, "MIDI \"%s\": in %02x%s: %s", m->name, ep_in, ep_out ? ", out too" : "",
             e == ESP_OK ? "listening" : esp_err_to_name(e));
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) dev_new(msg->new_dev.address);
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE)
        for (int i = 0; i < MIDI_MAX; i++)
            if (M.m[i].dev == msg->dev_gone.dev_hdl && !M.m[i].gone) {
                ESP_LOGI(TAG, "MIDI \"%s\" gone", M.m[i].name);
                M.m[i].gone = true;
            }
}

static bool reap(void)
{
    bool left = false;
    for (int i = 0; i < MIDI_MAX; i++) {
        midi_t *m = &M.m[i];
        if (!m->dev || !m->gone) continue;
        if ((m->in_busy || m->out_busy) && !m->halted) {
            m->halted = true;
            if (usb_host_endpoint_halt(m->dev, m->ep_in) == ESP_OK) usb_host_endpoint_flush(m->dev, m->ep_in);
            if (m->ep_out && usb_host_endpoint_halt(m->dev, m->ep_out) == ESP_OK) usb_host_endpoint_flush(m->dev, m->ep_out);
        }
        if (m->in_busy || m->out_busy) { left = true; continue; }
        midi_free(m);
    }
    return left;
}

static void midi_task(void *arg)
{
    (void)arg;
    while (!M.stop) {
        usb_host_client_handle_events(M.client, pdMS_TO_TICKS(50));
        reap();
    }
    for (int i = 0; i < MIDI_MAX; i++) if (M.m[i].dev) M.m[i].gone = true;
    for (int i = 0; i < 50 && reap(); i++) usb_host_client_handle_events(M.client, pdMS_TO_TICKS(20));
    usb_host_client_deregister(M.client);
    M.client = NULL;
    M.done = true;
    vTaskDelete(NULL);
}

bool aos_p4_usb_midi_start(void)
{
    if (!M.q) M.q = xQueueCreate(64, sizeof(aos_midi_msg_t));
    if (!M.mx) M.mx = xSemaphoreCreateMutex();
    M.stop = M.done = false;
    memset(M.m, 0, sizeof M.m);
    const usb_host_client_config_t cc = { .is_synchronous = false, .max_num_event_msg = 5,
                                          .async = { .client_event_callback = client_event } };
    if (usb_host_client_register(&cc, &M.client) != ESP_OK) return false;
    if (xTaskCreatePinnedToCore(midi_task, "usb_midi", 3072, NULL, 4, NULL, 0) != pdPASS) {
        usb_host_client_deregister(M.client);
        M.client = NULL;
        return false;
    }
    return true;
}

void aos_p4_usb_midi_stop(void)
{
    if (!M.client) return;
    M.stop = true;
    for (int i = 0; i < 100 && !M.done; i++) {
        usb_host_client_unblock(M.client);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool aos_hal_midi_read(aos_midi_msg_t *m) { return M.q && xQueueReceive(M.q, m, 0) == pdTRUE; }

/* One message to the first device that takes messages in: a synth module, a
 * keyboard's lights. false if none, or the last one is still going. */
bool aos_hal_midi_send(const aos_midi_msg_t *m)
{
    for (int i = 0; i < MIDI_MAX; i++) {
        midi_t *d = &M.m[i];
        if (!d->dev || d->gone || !d->out || d->out_busy) continue;
        uint8_t kind = m->status >> 4;
        uint8_t *p = d->out->data_buffer;
        p[0] = (m->cable << 4) | (kind >= 8 && kind <= 14 ? kind : 15);
        p[1] = m->status;
        p[2] = m->data1;
        p[3] = m->data2;
        d->out->num_bytes = 4;
        d->out->device_handle = d->dev;
        d->out->bEndpointAddress = d->ep_out;
        d->out->callback = out_done;
        d->out->context = d;
        d->out_busy = usb_host_transfer_submit(d->out) == ESP_OK;
        return d->out_busy;
    }
    return false;
}

int aos_hal_midi_devices(char names[][48], int max)
{
    if (!M.mx) return 0;
    int n = 0;
    xSemaphoreTake(M.mx, portMAX_DELAY);
    for (int i = 0; i < MIDI_MAX && n < max; i++)
        if (M.m[i].dev && !M.m[i].gone) snprintf(names[n++], 48, "%s", M.m[i].name);
    xSemaphoreGive(M.mx);
    return n;
}

int aos_hal_midi_last(aos_midi_msg_t *out, int max)
{
    if (!M.mx) return 0;
    xSemaphoreTake(M.mx, portMAX_DELAY);
    int n = M.nlast < max ? M.nlast : max;
    memcpy(out, M.last, n * sizeof *out);
    xSemaphoreGive(M.mx);
    return n;
}
