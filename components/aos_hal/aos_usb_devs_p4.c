/*
 * P4OS - every device on the USB host, known or not (aos_hal_usb_devices,
 * docs/USB.md).
 *
 * A client of ESP-IDF's USB Host Library that opens every new device, keeps
 * what it is - vendor and product (IDs and names), class, speed, the root
 * port and the hub port it hangs from - and holds it open until it goes, so
 * the library tells this client when it does. The drivers (the pendrives in
 * aos_usb_p4.c, HID in aos_usb_hid_p4.c, MIDI, serial, the camera) say what
 * the board does with each one (aos_p4_usb_dev_use), so Settings and the
 * portal can show a device nothing takes, too: what to write a driver for.
 */
#include "aos_hal.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"

static const char *TAG = "usb";
#define DEV_MAX 12

typedef struct {
    usb_device_handle_t hdl;
    aos_usb_dev_t d;
} entry_t;

static struct {
    usb_host_client_handle_t client;
    volatile bool stop, done;
    SemaphoreHandle_t mx;
    entry_t e[DEV_MAX];
} D;

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

static entry_t *by_hdl(usb_device_handle_t h)
{
    for (int i = 0; i < DEV_MAX; i++) if (D.e[i].hdl == h) return &D.e[i];
    return NULL;
}

static void dev_new(uint8_t addr)
{
    usb_device_handle_t dev;
    if (usb_host_device_open(D.client, addr, &dev) != ESP_OK) return;
    entry_t *e = by_hdl(NULL);
    if (!e) {
        ESP_LOGW(TAG, "devices: more than %d, %u not listed", DEV_MAX, addr);
        usb_host_device_close(D.client, dev);
        return;
    }
    aos_usb_dev_t d = { .addr = addr };
    const usb_device_desc_t *dd;
    if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK) {
        d.vid = dd->idVendor;
        d.pid = dd->idProduct;
        d.cls = dd->bDeviceClass;
    }
    /* a class per interface: the device's is 0 (or 0xEF, "miscellaneous")
     * on composite devices; the first interface's then */
    const usb_config_desc_t *cfg;
    if ((d.cls == 0 || d.cls == 0xEF) && usb_host_get_active_config_descriptor(dev, &cfg) == ESP_OK) {
        int off = 0;
        const usb_standard_desc_t *s = (const usb_standard_desc_t *)cfg;
        while ((s = usb_parse_next_descriptor(s, cfg->wTotalLength, &off)))
            if (s->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
                const usb_intf_desc_t *it = (const usb_intf_desc_t *)s;
                /* audio control comes before streaming and MIDI: the MIDI one wins */
                if (!d.cls || (d.cls == 0x01 && it->bInterfaceClass == 0x01 && it->bInterfaceSubClass == 3)) {
                    d.cls = it->bInterfaceClass;
                    d.sub = it->bInterfaceSubClass;
                }
                if (d.cls && d.cls != 0x01) break;
            }
    }
    usb_device_info_t di;
    if (usb_host_device_info(dev, &di) == ESP_OK) {
        d.speed = di.speed == USB_SPEED_HIGH ? 2 : di.speed == USB_SPEED_FULL ? 1 : 0;
        wide_to_str(di.str_desc_manufacturer, d.vendor, sizeof d.vendor);
        wide_to_str(di.str_desc_product, d.product, sizeof d.product);
        /* the root port: P4OS's host library numbers it as the parent's
         * port when there is no parent (components/usb/P4OS.md) */
        if (!di.parent.dev_hdl) d.port = di.parent.port_num ? AOS_HAL_USB_HOST_HEADER : AOS_HAL_USB_HOST_OTG;
        else {
            entry_t *p = by_hdl(di.parent.dev_hdl);
            d.port = p ? p->d.port : 0;
            d.hub_port = di.parent.port_num;
        }
    }
    if (d.cls == 0x09) snprintf(d.uses, sizeof d.uses, "hub");
    xSemaphoreTake(D.mx, portMAX_DELAY);
    e->hdl = dev;
    e->d = d;
    xSemaphoreGive(D.mx);
    ESP_LOGI(TAG, "device %u: %04x:%04x \"%s %s\", class %02x, %s speed, on %s%s", addr, d.vid, d.pid, d.vendor,
             d.product, d.cls, d.speed == 2 ? "high" : d.speed == 1 ? "full" : "low",
             d.port == AOS_HAL_USB_HOST_HEADER ? "21/23" : "25/27", d.hub_port ? ", behind a hub" : "");
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        dev_new(msg->new_dev.address);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        entry_t *e = by_hdl(msg->dev_gone.dev_hdl);
        if (!e) return;
        ESP_LOGI(TAG, "device %u gone", e->d.addr);
        usb_host_device_close(D.client, e->hdl);
        xSemaphoreTake(D.mx, portMAX_DELAY);
        memset(e, 0, sizeof *e);
        xSemaphoreGive(D.mx);
    }
}

static void devs_task(void *arg)
{
    (void)arg;
    while (!D.stop) usb_host_client_handle_events(D.client, pdMS_TO_TICKS(200));
    for (int i = 0; i < DEV_MAX; i++)
        if (D.e[i].hdl) usb_host_device_close(D.client, D.e[i].hdl);
    xSemaphoreTake(D.mx, portMAX_DELAY);
    memset(D.e, 0, sizeof D.e);
    xSemaphoreGive(D.mx);
    usb_host_client_deregister(D.client);
    D.client = NULL;
    D.done = true;
    vTaskDelete(NULL);
}

/* aos_usb_p4.c: first of the clients, so it sees every device first */
bool aos_p4_usb_devs_start(void)
{
    if (!D.mx) D.mx = xSemaphoreCreateMutex();
    D.stop = D.done = false;
    memset(D.e, 0, sizeof D.e);
    const usb_host_client_config_t cc = { .is_synchronous = false, .max_num_event_msg = 8,
                                          .async = { .client_event_callback = client_event } };
    if (usb_host_client_register(&cc, &D.client) != ESP_OK) return false;
    if (xTaskCreatePinnedToCore(devs_task, "usb_devs", 3072, NULL, 4, NULL, 0) != pdPASS) {
        usb_host_client_deregister(D.client);
        D.client = NULL;
        return false;
    }
    return true;
}

void aos_p4_usb_devs_stop(void)
{
    if (!D.client) return;
    D.stop = true;
    for (int i = 0; i < 100 && !D.done; i++) {
        usb_host_client_unblock(D.client);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* A driver says what it does with a device: "disco /usb", "keyboard"... */
void aos_p4_usb_dev_use(uint8_t addr, const char *what)
{
    if (!D.mx || !what) return;
    /* the registry may hear of the device after the driver: a moment */
    for (int t = 0; t < 10; t++) {
        xSemaphoreTake(D.mx, portMAX_DELAY);
        for (int i = 0; i < DEV_MAX; i++)
            if (D.e[i].hdl && D.e[i].d.addr == addr) {
                char *u = D.e[i].d.uses;
                size_t n = strlen(u);
                if (!strstr(u, what)) snprintf(u + n, sizeof D.e[i].d.uses - n, "%s%s", n ? ", " : "", what);
                xSemaphoreGive(D.mx);
                return;
            }
        xSemaphoreGive(D.mx);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

int aos_hal_usb_devices(aos_usb_dev_t *out, int max)
{
    if (!D.mx || !D.client) return 0;
    int n = 0;
    xSemaphoreTake(D.mx, portMAX_DELAY);
    for (int i = 0; i < DEV_MAX && n < max; i++) if (D.e[i].hdl) out[n++] = D.e[i].d;
    xSemaphoreGive(D.mx);
    return n;
}
