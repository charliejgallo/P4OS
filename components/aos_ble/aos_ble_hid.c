/*
 * P4OS - the board as a Bluetooth keyboard, mouse and media keys (HID over
 * GATT) for a computer. See aos_ble.h.
 *
 * The same three reports as the USB port's KEYS mode (aos_usb_p4.c), so the
 * same key names drive both: a keyboard (id 1: modifiers, a reserved byte and
 * six keys; its LED output report is accepted and ignored), the consumer
 * control (id 2: one 16-bit usage, the media keys) and a mouse (id 3: five
 * buttons, x, y, wheel and pan). HOGP also wants the Device Information
 * service with a PnP ID and the Battery service; both are here.
 *
 * The services are registered only with the mode on, before the stack starts
 * (NimBLE cannot add services to a running host), so switching the mode
 * restarts the stack (aos_ble_hid_enable).
 *
 * A computer and the phone can be connected at once (aos_ble.c tells them
 * apart by ANCS). The phone is refused the HID characteristics: an iPhone
 * that takes the board for a keyboard hides its own on-screen one.
 */
#include "aos_ble.h"
#include "aos_hal.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "aos_ble_hid";

#define RID_KEYBOARD 1
#define RID_CONSUMER 2
#define RID_MOUSE    3

/* The report map: TinyUSB's TUD_HID_REPORT_DESC_KEYBOARD, _CONSUMER and
 * _MOUSE with these report ids, written out (the USB side builds the same
 * thing from the macros). */
static const uint8_t REPORT_MAP[] = {
    /* keyboard */
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, RID_KEYBOARD,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x95, 0x05, 0x75, 0x01, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x95, 0x06, 0x75, 0x08, 0x81, 0x00,
    0xC0,
    /* consumer control */
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, RID_CONSUMER,
    0x15, 0x01, 0x26, 0xFF, 0x03, 0x19, 0x01, 0x2A, 0xFF, 0x03, 0x95, 0x01, 0x75, 0x10, 0x81, 0x00,
    0xC0,
    /* mouse */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, RID_MOUSE,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x95, 0x03, 0x75, 0x08, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7F, 0x95, 0x01, 0x75, 0x08, 0x81, 0x06,
    0xC0, 0xC0,
};

/* bcdHID 1.11, no country, "normally connectable" */
static const uint8_t HID_INFO[] = { 0x11, 0x01, 0x00, 0x02 };
/* PnP ID: USB-IF vendor id source, Espressif's VID 0x303A, PID 0x4002, v1.0 */
static const uint8_t PNP_ID[] = { 0x02, 0x3A, 0x30, 0x02, 0x40, 0x00, 0x01 };

static const char MANUFACTURER[] = "P4OS";
static uint16_t s_h_kbd, s_h_cons, s_h_mouse, s_h_batt;
static uint8_t s_protocol = 1;          /* report protocol */
static volatile bool s_sub_kbd, s_sub_cons, s_sub_mouse;
static bool s_registered;

/* Report Reference descriptors: { report id, type } (1 input, 2 output) */
static const uint8_t REF_KBD[] = { RID_KEYBOARD, 1 };
static const uint8_t REF_KBD_OUT[] = { RID_KEYBOARD, 2 };
static const uint8_t REF_CONS[] = { RID_CONSUMER, 1 };
static const uint8_t REF_MOUSE[] = { RID_MOUSE, 1 };

static int put(struct ble_gatt_access_ctxt *ctxt, const void *p, size_t n)
{
    return os_mbuf_append(ctxt->om, p, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    if (aos_ble_is_phone(conn)) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    const void *what = arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        return put(ctxt, what, 2);              /* a Report Reference */
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (what == &s_protocol && OS_MBUF_PKTLEN(ctxt->om) >= 1) os_mbuf_copydata(ctxt->om, 0, 1, &s_protocol);
        return 0;                               /* the control point, the LEDs: taken and ignored */
    }
    if (what == REPORT_MAP) return put(ctxt, REPORT_MAP, sizeof REPORT_MAP);
    if (what == HID_INFO) return put(ctxt, HID_INFO, sizeof HID_INFO);
    if (what == PNP_ID) return put(ctxt, PNP_ID, sizeof PNP_ID);
    if (what == &s_protocol) return put(ctxt, &s_protocol, 1);
    if (what == &s_h_batt) {
        uint8_t pct = 100;                      /* on a USB supply */
        return put(ctxt, &pct, 1);
    }
    if (what == MANUFACTURER) return put(ctxt, MANUFACTURER, sizeof MANUFACTURER - 1);
    static const uint8_t zero[8];               /* a report read: nothing pressed */
    if (what == REF_KBD) return put(ctxt, zero, 8);
    if (what == REF_CONS) return put(ctxt, zero, 2);
    if (what == REF_MOUSE) return put(ctxt, zero, 5);
    return 0;
}

#define ENC_R  (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC)

static const struct ble_gatt_svc_def SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),     /* Device Information */
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = access_cb, .arg = (void *)PNP_ID, .flags = ENC_R },
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = access_cb, .arg = (void *)MANUFACTURER, .flags = ENC_R },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),     /* Battery */
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = access_cb, .arg = &s_h_batt,
              .val_handle = &s_h_batt, .flags = ENC_R | BLE_GATT_CHR_F_NOTIFY },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),     /* HID */
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = access_cb, .arg = (void *)HID_INFO, .flags = ENC_R },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = access_cb, .arg = (void *)REPORT_MAP, .flags = ENC_R },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = access_cb, .arg = NULL,
              .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = access_cb, .arg = &s_protocol,
              .flags = ENC_R | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = access_cb, .arg = (void *)REF_KBD,
              .val_handle = &s_h_kbd, .flags = ENC_R | BLE_GATT_CHR_F_NOTIFY,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                    .access_cb = access_cb, .arg = (void *)REF_KBD },
                  { 0 } } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = access_cb, .arg = (void *)REF_KBD_OUT,
              .flags = ENC_R | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                    .access_cb = access_cb, .arg = (void *)REF_KBD_OUT },
                  { 0 } } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = access_cb, .arg = (void *)REF_CONS,
              .val_handle = &s_h_cons, .flags = ENC_R | BLE_GATT_CHR_F_NOTIFY,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                    .access_cb = access_cb, .arg = (void *)REF_CONS },
                  { 0 } } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = access_cb, .arg = (void *)REF_MOUSE,
              .val_handle = &s_h_mouse, .flags = ENC_R | BLE_GATT_CHR_F_NOTIFY,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                    .access_cb = access_cb, .arg = (void *)REF_MOUSE },
                  { 0 } } },
            { 0 },
        },
    },
    { 0 },
};

/* ---- what aos_ble.c calls ---- */

bool aos_ble_hid_wanted(void)
{
    int32_t v = 0;
    aos_hal_pref_get_i32("bt_hid", &v);
    return v != 0;
}

/* aos_ble_start, between nimble_port_init and the host task */
void aos_ble_hid_register(void)
{
    s_registered = false;
    s_sub_kbd = s_sub_cons = s_sub_mouse = false;
    if (!aos_ble_hid_wanted()) return;
    int rc = ble_gatts_count_cfg(SVCS);
    if (rc == 0) rc = ble_gatts_add_svcs(SVCS);
    if (rc) {
        ESP_LOGE(TAG, "the HID services: %d", rc);
        return;
    }
    s_registered = true;
    ESP_LOGI(TAG, "HID services up (keyboard, media keys, mouse)");
}

void aos_ble_hid_subscribe(uint16_t attr, bool notify)
{
    if (attr == s_h_kbd) s_sub_kbd = notify;
    else if (attr == s_h_cons) s_sub_cons = notify;
    else if (attr == s_h_mouse) s_sub_mouse = notify;
    else return;
    ESP_LOGI(TAG, "the computer %s report %u", notify ? "listens to" : "dropped", (unsigned)attr);
}

void aos_ble_hid_host_gone(void) { s_sub_kbd = s_sub_cons = s_sub_mouse = false; }

/* ---- sending ---- */

bool aos_ble_hid_ready(void)
{
    return s_registered && aos_ble_host_conn() != 0xFFFF && s_sub_kbd;
}

bool aos_ble_hid_send(int report_id, const void *data, size_t len)
{
    uint16_t conn = aos_ble_host_conn();
    if (!s_registered || conn == 0xFFFF) return false;
    uint16_t h = report_id == RID_KEYBOARD ? s_h_kbd : report_id == RID_CONSUMER ? s_h_cons : s_h_mouse;
    bool sub = report_id == RID_KEYBOARD ? s_sub_kbd : report_id == RID_CONSUMER ? s_sub_cons : s_sub_mouse;
    if (!sub) return false;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (!om) return false;
    return ble_gatts_notify_custom(conn, h, om) == 0;
}

void aos_ble_hid_enable(bool on)
{
    if (on == aos_ble_hid_wanted()) return;
    aos_hal_pref_set_i32("bt_hid", on ? 1 : 0);
    /* the services go in or out only with the stack down */
    if (aos_ble_running()) {
        aos_ble_stop();
        aos_ble_start();
    }
}
