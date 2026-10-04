/*
 * P4OS - USB keyboards on the pendrive host (aos_hal.h, docs/USB.md).
 *
 * A client of ESP-IDF's USB Host Library, beside usb_host_msc: every new
 * device is opened, and one with a HID interface of the boot keyboard kind
 * (class 3, subclass 1, protocol 1) is kept. The interface is put in the
 * boot protocol (SET_PROTOCOL 0), where every keyboard sends the same
 * eight bytes - modifiers, a reserved byte and up to six keys held - so no
 * report descriptor has to be parsed, and its interrupt IN endpoint is read
 * with one transfer that resubmits itself. Keys pressed since the last
 * report become events in a queue that the shell reads
 * (aos_hal_usb_kbd_read, aos_ui's aos_hwkbd.c) and types into the text
 * field of the on-screen keyboard that is open.
 *
 * Characters come from a layout: US, or Latin American (es-419, the
 * rioplatense one: ñ, ¿¡, AltGr for @ and the rest, and the acute and the
 * diaeresis as dead keys). A key held repeats after 500 ms, 25 times a
 * second, as a computer's would; the client's task times it.
 *
 * Up to two keyboards at once (one per root port, or behind a hub).
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
#define PREF_LAYOUT "usb_kbdlay"
#define KBD_MAX 2

typedef struct {
    usb_device_handle_t dev;
    uint8_t intf, ep;
    uint16_t mps;
    usb_transfer_t *in, *ctrl;
    uint8_t last[8];
    char name[48];
    volatile bool gone;                 /* unplugged or stopping: freed by the task once idle */
    volatile bool in_busy, ctrl_busy;   /* a transfer in flight: not to be freed under it */
    bool halted;
} kbd_t;

static struct {
    usb_host_client_handle_t client;
    volatile bool stop, done;
    QueueHandle_t q;
    SemaphoreHandle_t mx;
    kbd_t kb[KBD_MAX];
    /* the key held for repeating, and when it repeats next */
    uint8_t held_code, held_mods;
    TickType_t held_next;
    uint32_t dead;          /* a dead key waiting for its letter (´ or ¨), 0 if none */
    bool caps;              /* Caps Lock */
} K;

/* ---- layouts ---------------------------------------------------------- */

/* HID usages 0x04..0x38 and 0x64: plain, with Shift, with AltGr (0: none) */
typedef struct { uint8_t usage; uint32_t plain, shift, altgr; } keymap_t;

#define DEAD_ACUTE 0x00B4   /* ´ */
#define DEAD_DIAER 0x00A8   /* ¨ */

static const keymap_t US[] = {
    { 0x1E, '1', '!', 0 }, { 0x1F, '2', '@', 0 }, { 0x20, '3', '#', 0 }, { 0x21, '4', '$', 0 },
    { 0x22, '5', '%', 0 }, { 0x23, '6', '^', 0 }, { 0x24, '7', '&', 0 }, { 0x25, '8', '*', 0 },
    { 0x26, '9', '(', 0 }, { 0x27, '0', ')', 0 }, { 0x2C, ' ', ' ', 0 }, { 0x2D, '-', '_', 0 },
    { 0x2E, '=', '+', 0 }, { 0x2F, '[', '{', 0 }, { 0x30, ']', '}', 0 }, { 0x31, '\\', '|', 0 },
    { 0x32, '#', '~', 0 }, { 0x33, ';', ':', 0 }, { 0x34, '\'', '"', 0 }, { 0x35, '`', '~', 0 },
    { 0x36, ',', '<', 0 }, { 0x37, '.', '>', 0 }, { 0x38, '/', '?', 0 }, { 0x64, '\\', '|', 0 },
};

static const keymap_t LATAM[] = {
    { 0x14, 'q', 'Q', '@' },
    { 0x1E, '1', '!', '|' }, { 0x1F, '2', '"', '@' }, { 0x20, '3', '#', 0 }, { 0x21, '4', '$', '~' },
    { 0x22, '5', '%', 0 }, { 0x23, '6', '&', 0 }, { 0x24, '7', '/', 0 }, { 0x25, '8', '(', 0 },
    { 0x26, '9', ')', 0 }, { 0x27, '0', '=', 0 }, { 0x2C, ' ', ' ', 0 },
    { 0x2D, '\'', '?', '\\' }, { 0x2E, 0x00BF, 0x00A1, 0 },             /* ¿ ¡ */
    { 0x2F, DEAD_ACUTE, DEAD_DIAER, 0 },                                /* dead ´ ¨ */
    { 0x30, '+', '*', '~' }, { 0x31, '}', ']', '`' }, { 0x32, '}', ']', '`' },
    { 0x33, 0x00F1, 0x00D1, 0 },                                        /* ñ Ñ */
    { 0x34, '{', '[', '^' }, { 0x35, '|', 0x00B0, 0x00AC },             /* | ° ¬ */
    { 0x36, ',', ';', 0 }, { 0x37, '.', ':', 0 }, { 0x38, '-', '_', 0 }, { 0x64, '<', '>', 0 },
};

int aos_hal_usb_kbd_layout(void)
{
    int32_t v = AOS_KBD_LATAM;
    aos_hal_pref_get_i32(PREF_LAYOUT, &v);
    return v == AOS_KBD_US ? AOS_KBD_US : AOS_KBD_LATAM;
}

void aos_hal_usb_kbd_layout_set(int layout)
{
    aos_hal_pref_set_i32(PREF_LAYOUT, layout == AOS_KBD_US ? AOS_KBD_US : AOS_KBD_LATAM);
}

/* a dead key and the letter after it: á é í ó ú ü, and their capitals */
static uint32_t compose(uint32_t dead, uint32_t c)
{
    static const char V[] = "aeiouAEIOU";
    static const uint32_t ACUTE[] = { 0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xC1, 0xC9, 0xCD, 0xD3, 0xDA };
    static const uint32_t DIAER[] = { 0xE4, 0xEB, 0xEF, 0xF6, 0xFC, 0xC4, 0xCB, 0xCF, 0xD6, 0xDC };
    for (int i = 0; i < 10; i++)
        if (c == (uint32_t)V[i]) return dead == DEAD_ACUTE ? ACUTE[i] : DIAER[i];
    return 0;
}

static void push(uint32_t key, uint8_t mods)
{
    aos_kbd_event_t ev = { .key = key, .mods = mods };
    xQueueSend(K.q, &ev, 0);
}

/* A HID usage pressed, with the modifiers of its report: an event, or a dead
 * key remembered for the next one. */
static void key_down(uint8_t u, uint8_t mods)
{
    bool shift = mods & 0x22, ctrl = mods & 0x11, altgr = mods & 0x40, gui = mods & 0x88;
    uint32_t key = 0;
    switch (u) {
    case 0x28: case 0x58: key = AOS_KEY_ENTER; break;
    case 0x29: key = AOS_KEY_ESC; break;
    case 0x2A: key = AOS_KEY_BACKSPACE; break;
    case 0x2B: key = shift ? AOS_KEY_PREV : AOS_KEY_NEXT; break;
    case 0x4C: key = AOS_KEY_DEL; break;
    case 0x4A: key = AOS_KEY_HOME; break;
    case 0x4D: key = AOS_KEY_END; break;
    case 0x4F: key = AOS_KEY_RIGHT; break;
    case 0x50: key = AOS_KEY_LEFT; break;
    case 0x51: key = AOS_KEY_DOWN; break;
    case 0x52: key = AOS_KEY_UP; break;
    default: break;
    }
    if (key) {
        K.dead = 0;
        push(key, mods);
        return;
    }
    if (u == 0x39) {                    /* Caps Lock (its LED stays as the keyboard has it) */
        K.caps = !K.caps;
        return;
    }
    /* letters, upper case with Shift or Caps Lock; with AltGr only what the
     * layout's table says (q -> @ on the Latin American one) */
    if (u >= 0x04 && u <= 0x1D && !altgr) key = (shift != K.caps ? 'A' : 'a') + (u - 0x04);
    const keymap_t *map = aos_hal_usb_kbd_layout() == AOS_KBD_US ? US : LATAM;
    size_t n = map == US ? sizeof US / sizeof US[0] : sizeof LATAM / sizeof LATAM[0];
    for (size_t i = 0; i < n; i++)
        if (map[i].usage == u) {
            key = altgr ? map[i].altgr : shift ? map[i].shift : map[i].plain;
            break;
        }
    /* Ctrl or Cmd with a key is a shortcut, not text: it goes with its mods */
    if (!key || (ctrl && !altgr) || gui) {
        if (key) push(key, mods);
        return;
    }
    if (key == DEAD_ACUTE || key == DEAD_DIAER) {
        if (K.dead) {                   /* twice: the accent itself */
            push(K.dead, 0);
            K.dead = 0;
        } else K.dead = key;
        return;
    }
    if (K.dead) {
        uint32_t c = compose(K.dead, key);
        if (c) key = c;
        else {
            push(K.dead, 0);            /* no such letter: the accent, then the key */
            if (key == ' ') key = 0;    /* ´ and space: just the accent */
        }
        K.dead = 0;
        if (!key) return;
    }
    push(key, mods);
}

/* ---- the reports ------------------------------------------------------ */

static void in_done(usb_transfer_t *t)
{
    kbd_t *kb = t->context;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes >= 8) {
        const uint8_t *r = t->data_buffer;
        /* 0x01 in every key slot: too many keys at once (phantom state) */
        if (r[2] != 0x01) {
            for (int i = 2; i < 8; i++) {
                uint8_t u = r[i];
                if (u < 0x04 || memchr(kb->last + 2, u, 6)) continue;
                key_down(u, r[0]);
                K.held_code = u;
                K.held_mods = r[0];
                K.held_next = xTaskGetTickCount() + pdMS_TO_TICKS(500);
            }
            if (K.held_code && !memchr(r + 2, K.held_code, 6)) K.held_code = 0;
            if (K.held_code) K.held_mods = r[0];
            memcpy(kb->last, r, 8);
        }
    }
    bool again = t->status != USB_TRANSFER_STATUS_NO_DEVICE && t->status != USB_TRANSFER_STATUS_CANCELED &&
                 !kb->gone && !K.stop;
    kb->in_busy = again && usb_host_transfer_submit(t) == ESP_OK;
}

static void ctrl_done(usb_transfer_t *t)
{
    kbd_t *kb = t->context;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED)
        ESP_LOGW(TAG, "keyboard: class request %02x: status %d", t->data_buffer[1], t->status);
    kb->ctrl_busy = false;
}

/* A class request to the interface (SET_PROTOCOL), asynchronous: this runs
 * inside the client's callback, where waiting for it would block the very
 * task that completes it. Without SET_IDLE, a keyboard may repeat its report
 * every 500 ms: the same keys, so nothing new. */
static void class_request(kbd_t *kb, uint8_t req, uint16_t value)
{
    uint8_t *b = kb->ctrl->data_buffer;
    b[0] = 0x21;                        /* host to device, class, interface */
    b[1] = req;
    b[2] = value & 0xFF;
    b[3] = value >> 8;
    b[4] = kb->intf;
    b[5] = 0;
    b[6] = b[7] = 0;
    kb->ctrl->num_bytes = 8;
    kb->ctrl->device_handle = kb->dev;
    kb->ctrl->bEndpointAddress = 0;
    kb->ctrl->callback = ctrl_done;
    kb->ctrl->context = kb;
    kb->ctrl_busy = usb_host_transfer_submit_control(K.client, kb->ctrl) == ESP_OK;
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

static void kbd_free(kbd_t *kb)
{
    if (kb->in) { usb_host_transfer_free(kb->in); kb->in = NULL; }
    if (kb->ctrl) { usb_host_transfer_free(kb->ctrl); kb->ctrl = NULL; }
    if (kb->dev) {
        usb_host_interface_release(K.client, kb->dev, kb->intf);
        usb_host_device_close(K.client, kb->dev);
    }
    xSemaphoreTake(K.mx, portMAX_DELAY);
    memset(kb, 0, sizeof *kb);
    xSemaphoreGive(K.mx);
}

/* A new device: kept if it has a boot keyboard interface. */
static void kbd_try(uint8_t addr)
{
    int slot = -1;
    for (int i = 0; i < KBD_MAX; i++) if (!K.kb[i].dev) { slot = i; break; }
    usb_device_handle_t dev;
    if (usb_host_device_open(K.client, addr, &dev) != ESP_OK) return;
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) { usb_host_device_close(K.client, dev); return; }
    int off = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    const usb_intf_desc_t *intf = NULL;
    const usb_ep_desc_t *ep = NULL;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off))) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *it = (const usb_intf_desc_t *)d;
            if (intf && ep) break;
            intf = it->bInterfaceClass == USB_CLASS_HID && it->bInterfaceSubClass == 1 && it->bInterfaceProtocol == 1
                   && it->bAlternateSetting == 0 ? it : NULL;
            ep = NULL;
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && intf && !ep) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
            if (USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_INTR && USB_EP_DESC_GET_EP_DIR(e)) ep = e;
        }
    }
    if (!intf || !ep) { usb_host_device_close(K.client, dev); return; }
    if (slot < 0) {
        ESP_LOGW(TAG, "a keyboard more than %d: left alone", KBD_MAX);
        usb_host_device_close(K.client, dev);
        return;
    }
    kbd_t *kb = &K.kb[slot];
    kb->dev = dev;
    kb->intf = intf->bInterfaceNumber;
    kb->ep = ep->bEndpointAddress;
    kb->mps = USB_EP_DESC_GET_MPS(ep);
    if (usb_host_interface_claim(K.client, dev, kb->intf, 0) != ESP_OK ||
        usb_host_transfer_alloc(64, 0, &kb->ctrl) != ESP_OK ||
        usb_host_transfer_alloc(kb->mps < 8 ? 8 : kb->mps, 0, &kb->in) != ESP_OK) {
        ESP_LOGW(TAG, "keyboard at address %u: could not claim it", addr);
        kbd_free(kb);
        return;
    }
    usb_device_info_t di;
    char product[40] = "", vendor[24] = "";
    if (usb_host_device_info(dev, &di) == ESP_OK) {
        wide_to_str(di.str_desc_manufacturer, vendor, sizeof vendor);
        wide_to_str(di.str_desc_product, product, sizeof product);
    }
    xSemaphoreTake(K.mx, portMAX_DELAY);
    snprintf(kb->name, sizeof kb->name, "%s%s%s", vendor, *vendor && *product ? " " : "", *product ? product : "");
    if (!kb->name[0]) snprintf(kb->name, sizeof kb->name, "USB keyboard");
    xSemaphoreGive(K.mx);
    class_request(kb, 0x0B, 0);         /* SET_PROTOCOL: boot */
    kb->in->num_bytes = kb->mps < 8 ? 8 : kb->mps;
    kb->in->device_handle = dev;
    kb->in->bEndpointAddress = kb->ep;
    kb->in->callback = in_done;
    kb->in->context = kb;
    esp_err_t e = usb_host_transfer_submit(kb->in);
    kb->in_busy = e == ESP_OK;
    ESP_LOGI(TAG, "keyboard \"%s\" (address %u, %s speed): %s", kb->name, addr,
             di.speed == USB_SPEED_HIGH ? "high" : di.speed == USB_SPEED_FULL ? "full" : "low",
             e == ESP_OK ? "typing" : esp_err_to_name(e));
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        kbd_try(msg->new_dev.address);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        for (int i = 0; i < KBD_MAX; i++)
            if (K.kb[i].dev == msg->dev_gone.dev_hdl) {
                ESP_LOGI(TAG, "keyboard \"%s\" gone", K.kb[i].name);
                K.kb[i].gone = true;
                K.held_code = 0;
            }
    }
}

/* A keyboard that is gone (or stopping) is freed here, outside the client's
 * callback: its transfers halted and flushed first, and only once their
 * callbacks have run. */
static bool kbd_reap(void)
{
    bool left = false;
    for (int i = 0; i < KBD_MAX; i++) {
        kbd_t *kb = &K.kb[i];
        if (!kb->dev || !kb->gone) continue;
        if ((kb->in_busy || kb->ctrl_busy) && !kb->halted) {
            kb->halted = true;
            if (usb_host_endpoint_halt(kb->dev, kb->ep) == ESP_OK) usb_host_endpoint_flush(kb->dev, kb->ep);
        }
        if (kb->in_busy || kb->ctrl_busy) {
            left = true;
            continue;
        }
        kbd_free(kb);
    }
    return left;
}

static void kbd_task(void *arg)
{
    (void)arg;
    while (!K.stop) {
        usb_host_client_handle_events(K.client, pdMS_TO_TICKS(20));
        kbd_reap();
        /* the key held repeats */
        if (K.held_code && (int32_t)(xTaskGetTickCount() - K.held_next) >= 0) {
            key_down(K.held_code, K.held_mods);
            K.held_next = xTaskGetTickCount() + pdMS_TO_TICKS(40);
        }
    }
    for (int i = 0; i < KBD_MAX; i++) K.kb[i].gone = true;
    for (int i = 0; i < 50 && kbd_reap(); i++) usb_host_client_handle_events(K.client, pdMS_TO_TICKS(20));
    usb_host_client_deregister(K.client);
    K.client = NULL;
    K.done = true;
    vTaskDelete(NULL);
}

/* aos_usb_p4.c: with the host library up, and before it goes down */
bool aos_p4_usb_kbd_start(void)
{
    if (!K.q) K.q = xQueueCreate(32, sizeof(aos_kbd_event_t));
    if (!K.mx) K.mx = xSemaphoreCreateMutex();
    K.stop = K.done = false;
    K.held_code = 0;
    K.dead = 0;
    K.caps = false;
    memset(K.kb, 0, sizeof K.kb);
    const usb_host_client_config_t cc = { .is_synchronous = false, .max_num_event_msg = 5,
                                          .async = { .client_event_callback = client_event } };
    if (usb_host_client_register(&cc, &K.client) != ESP_OK) return false;
    if (xTaskCreatePinnedToCore(kbd_task, "usb_kbd", 4096, NULL, 4, NULL, 0) != pdPASS) {
        usb_host_client_deregister(K.client);
        K.client = NULL;
        return false;
    }
    return true;
}

void aos_p4_usb_kbd_stop(void)
{
    if (!K.client) return;
    K.stop = true;
    for (int i = 0; i < 100 && !K.done; i++) {
        usb_host_client_unblock(K.client);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool aos_hal_usb_kbd_read(aos_kbd_event_t *ev) { return K.q && xQueueReceive(K.q, ev, 0) == pdTRUE; }

int aos_hal_usb_kbd_list(char names[][48], int max)
{
    if (!K.mx) return 0;
    int n = 0;
    xSemaphoreTake(K.mx, portMAX_DELAY);
    for (int i = 0; i < KBD_MAX && n < max; i++)
        if (K.kb[i].dev) snprintf(names[n++], 48, "%s", K.kb[i].name);
    xSemaphoreGive(K.mx);
    return n;
}
