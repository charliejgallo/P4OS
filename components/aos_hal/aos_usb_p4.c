/*
 * P4OS - the USB port as a keyboard and mouse of the computer (aos_hal.h,
 * USB block), for the Macro pad.
 *
 * The port is the P4's USB 2.0 High-Speed OTG, on the board's "OTG"
 * connector (HARDWARE.md: H1 "UART" is the CH343 console, H2 "OTG" the P4's
 * own HS PHY; the USB-Serial-JTAG reaches no connector). The console never
 * lives here, so unlike the S3 watch nothing has to move out of the way:
 * KEYS installs TinyUSB (esp_tinyusb) with one HID interface carrying four
 * reports -a keyboard, consumer control for the media keys, a mouse and a
 * gamepad-, a MIDI port, and a network over the cable (CDC-NCM,
 * aos_usb_net_p4.c: the portal at 192.168.7.1 with no Wi-Fi);
 * DISK with one mass storage interface that is the microSD, and CONSOLE
 * uninstalls it and leaves the port idle. Beside the role, the pendrive
 * host (below) is a switch of its own: pendrives on the 40-pin header,
 * through the P4's second, Full-Speed controller while the OTG connector
 * keeps its role, or through the High-Speed one (HOST: the OTG controller
 * taken by the host).
 *
 * DISK: the board lets go of the card (aos_hal_sd_release: the player stops,
 * FAT unmounted, host shut), initialises it again with no filesystem and
 * hands its sectors to esp_tinyusb's MSC storage. While the computer has it
 * nothing on the board may touch /sdcard: the settings close the other apps
 * before asking for this mode, and the card's apps do not open. When the
 * computer ejects it (or goes away), esp_tinyusb mounts it back for the
 * board; that is taken as the end of disk mode, and the port goes back by
 * itself to the mode it had before, the card mounted the usual way. So
 * "eject on the computer" is all it takes, and the card is never on both
 * sides at once.
 *
 * The descriptors are both speeds': the P4's controller comes up at High
 * Speed on a USB 2.0 port, and TinyUSB asks for the full-speed set and the
 * device qualifier as well. The HID endpoint polls every 1 ms at either
 * speed (bInterval 4 = 2^3 microframes at HS, 1 frame at FS).
 *
 * A key blocks the caller until the computer has taken the press and the
 * release (a few ms at 1 ms polling; up to ~40 ms if the report pipe is
 * busy), as the watch's does: callers that type long texts send a character
 * per LVGL tick. A suspended computer (asleep) is woken with a remote wakeup
 * before the report, which the configuration descriptor allows.
 *
 * Tried on a Mac on 2026-09-30 (docs/MACROPAD.md): it enumerates at HS and
 * the three reports work. The log, which the portal shows (Registro, filter
 * "usb"), says each step: the mode switches,
 * the speed the computer took, suspend and resume, the keyboard LEDs the
 * computer sends back (the proof it reads our keyboard) and the reports
 * that could not go, counted.
 */
#include "aos_hal.h"
#include "aos_hid_keys.h"

#include <inttypes.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "class/hid/hid_device.h"
#include "class/midi/midi_device.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "hal/usb_dwc_ll.h"
#include "hal/usb_wrap_ll.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/usb_phy.h"
#include "hal/usb_serial_jtag_ll.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "freertos/queue.h"

/* aos_usb_net_p4.c: the network over the cable, with the keyboard */
bool aos_p4_usb_net_start(void);
void aos_p4_usb_net_stop(void);
void aos_p4_usb_net_relink(bool up);
void aos_p4_usb_net_mac(uint8_t mac[6]);

/* aos_hal_p4.c: the card on its slot with no filesystem, for disk mode */
sdmmc_card_t *aos_p4_sd_card_open(void);
void aos_p4_sd_card_close(sdmmc_card_t *card);

static const char *TAG = "usb";
#define PREF_MODE "usb_mode"
#define PREF_HOST_PORT "usb_hport"

/* -------------------------------------------------------------------------- */
/* Descriptors                                                                 */
/* -------------------------------------------------------------------------- */

enum { RID_KEYBOARD = 1, RID_CONSUMER = 2, RID_MOUSE = 3 };
/* esp_tinyusb takes at most 8 strings: MIDI borrows the product's */
enum { STR_LANG = 0, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_HID, STR_MSC, STR_NET, STR_MAC, STR_COUNT };

/* Keyboard mode is six interfaces: the HID (keyboard, media keys, mouse),
 * the gamepad's own HID, the network (CDC-NCM, two: control and data) and
 * MIDI (two: audio control and streaming). Five IN endpoints of the HS
 * port's eight.
 *
 * The gamepad is a device of its own and not a fourth report of the
 * keyboard's: macOS names a HID device by its first collection, so inside
 * the keyboard it was a keyboard with a gamepad in it - no browser nor the
 * GameController framework listed it, and opening a keyboard needs the
 * Input Monitoring permission (2026-09-30). Alone it is a plain gamepad. */
enum { ITF_HID = 0, ITF_PAD, ITF_NET, ITF_NET_DATA, ITF_MIDI, ITF_MIDI_STREAMING, ITF_TOTAL };
enum { HID_KEYS = 0, HID_PAD = 1 };     /* TinyUSB's HID instances, in interface order */
#define EP_HID_IN    0x81
#define EP_PAD_IN    0x85
#define EP_NET_NOTIF 0x82
#define EP_NET_OUT   0x03
#define EP_NET_IN    0x83
#define EP_MIDI_IN   0x84
#define CFG_LEN    (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN + TUD_CDC_NCM_DESC_LEN + MIDI_OUT_LEN)

/* MIDI from the board to the computer only: one IN endpoint, no OUT.
 * TinyUSB's template has both, and at High Speed a bulk endpoint is 512
 * bytes while esp_tinyusb fixes the MIDI receive buffer at 64
 * (CFG_TUD_MIDI_RX_BUFSIZE, not a Kconfig option): opening the OUT
 * endpoint fails, and with it the whole configuration - the Mac chose it
 * and got a stall (2026-09-30). The pad and the piano only send, so the
 * port has one direction: an external IN jack (the keys) wired to an
 * embedded OUT jack that the IN endpoint carries. */
#define MIDI_OUT_LEN (9 + 9 + 9 + 7 + 6 + 9 + 9 + 5)
#define MIDI_OUT_DESCRIPTOR(_itf, _stridx, _epin, _epsize) \
    /* Audio Control interface and its header, pointing at the streaming one */ \
    9, TUSB_DESC_INTERFACE, _itf, 0, 0, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL, AUDIO_FUNC_PROTOCOL_CODE_UNDEF, _stridx, \
    9, TUSB_DESC_CS_INTERFACE, AUDIO10_CS_AC_INTERFACE_HEADER, U16_TO_U8S_LE(0x0100), U16_TO_U8S_LE(0x0009), 1, (uint8_t)((_itf) + 1), \
    /* MIDI Streaming interface, one endpoint */ \
    9, TUSB_DESC_INTERFACE, (uint8_t)((_itf) + 1), 0, 1, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_MIDI_STREAMING, AUDIO_FUNC_PROTOCOL_CODE_UNDEF, 0, \
    7, TUSB_DESC_CS_INTERFACE, MIDI_CS_INTERFACE_HEADER, U16_TO_U8S_LE(0x0100), U16_TO_U8S_LE(7 + 6 + 9 + 9 + 5), \
    /* IN jack (external, id 2) -> OUT jack (embedded, id 3) */ \
    6, TUSB_DESC_CS_INTERFACE, MIDI_CS_INTERFACE_IN_JACK, MIDI_JACK_EXTERNAL, 2, 0, \
    9, TUSB_DESC_CS_INTERFACE, MIDI_CS_INTERFACE_OUT_JACK, MIDI_JACK_EMBEDDED, 3, 1, 2, 1, 0, \
    /* the IN endpoint (Audio 1.0's are 9 bytes) carrying jack 3 */ \
    9, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0, 0, 0, \
    5, TUSB_DESC_CS_ENDPOINT, MIDI_CS_ENDPOINT_GENERAL, 1, 3
#define EP_MSC_OUT 0x01
#define EP_MSC_IN  0x81
#define DISK_LEN   (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t s_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KEYBOARD)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(RID_CONSUMER)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(RID_MOUSE)),
};

static const uint8_t s_pad_desc[] = { TUD_HID_REPORT_DESC_GAMEPAD() };

/* The bulk endpoints are 64 bytes at Full Speed and 512 at High Speed, as
 * USB 2.0 requires of each. */
static const uint8_t s_cfg_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CFG_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_HID, STR_HID, HID_ITF_PROTOCOL_NONE, sizeof s_report_desc, EP_HID_IN, 16, 1),
    TUD_HID_DESCRIPTOR(ITF_PAD, STR_PRODUCT, HID_ITF_PROTOCOL_NONE, sizeof s_pad_desc, EP_PAD_IN, 16, 1),
    TUD_CDC_NCM_DESCRIPTOR(ITF_NET, STR_NET, STR_MAC, EP_NET_NOTIF, 64, EP_NET_OUT, EP_NET_IN, 64, CFG_TUD_NET_MTU),
    MIDI_OUT_DESCRIPTOR(ITF_MIDI, STR_PRODUCT, EP_MIDI_IN, 64),
};

static const uint8_t s_cfg_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CFG_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_HID, STR_HID, HID_ITF_PROTOCOL_NONE, sizeof s_report_desc, EP_HID_IN, 16, 4),
    TUD_HID_DESCRIPTOR(ITF_PAD, STR_PRODUCT, HID_ITF_PROTOCOL_NONE, sizeof s_pad_desc, EP_PAD_IN, 16, 4),
    /* the notification's interval spelled out: TinyUSB's default, 50, is a
     * count of frames at Full Speed but an exponent at High Speed (1..16),
     * and the Mac never configured the device with it (2026-09-30). 9 =
     * 2^8 microframes = 32 ms. */
    TUD_CDC_NCM_DESCRIPTOR(ITF_NET, STR_NET, STR_MAC, EP_NET_NOTIF, 64, EP_NET_OUT, EP_NET_IN, 512, CFG_TUD_NET_MTU,
                           9, (NCM_NETWORK_CAPS_ETH_FILTER | NCM_NETWORK_CAPS_NTB_INPUT_SIZE)),
    MIDI_OUT_DESCRIPTOR(ITF_MIDI, STR_PRODUCT, EP_MIDI_IN, 512),
};

/* Disk mode: the mass storage interface alone. Bulk endpoints of 64 bytes
 * at Full Speed and 512 at High Speed, as USB 2.0 requires of each. */
static const uint8_t s_disk_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, DISK_LEN, 0, 100),
    TUD_MSC_DESCRIPTOR(0, STR_MSC, EP_MSC_OUT, EP_MSC_IN, 64),
};

static const uint8_t s_disk_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, DISK_LEN, 0, 100),
    TUD_MSC_DESCRIPTOR(0, STR_MSC, EP_MSC_OUT, EP_MSC_IN, 512),
};

static const tusb_desc_device_t s_dev = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,    /* IAD: the network is an association of two interfaces */
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,                 /* Espressif */
    .idProduct = 0x402C,                /* esp_tinyusb's PID map: HID (0x04) | MIDI (0x08), plus 0x20 for the NCM, as the watch */
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

/* another product id for the disk: a computer that remembers devices by
 * vendor and product keeps the keyboard and the disk apart */
static const tusb_desc_device_t s_dev_disk = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4002,                /* esp_tinyusb's PID map: MSC */
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_qualifier_t s_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

static char s_serial[13];               /* the MAC: two boards are two devices */
static char s_product[40];
static char s_net_mac[13];              /* the computer's side of the cable network, as NCM wants it */
static const char *s_strings[STR_COUNT] = {
    [STR_LANG]         = (const char[]){ 0x09, 0x04 },   /* English (US) */
    [STR_MANUFACTURER] = "P4OS",
    [STR_PRODUCT]      = s_product,
    [STR_SERIAL]       = s_serial,
    [STR_HID]          = "P4OS keyboard and mouse",
    [STR_MSC]          = "P4OS microSD",
    [STR_NET]          = "P4OS network",
    [STR_MAC]          = s_net_mac,
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == HID_PAD ? s_pad_desc : s_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    /* the computer's keyboard LEDs (caps lock): nothing shows them, but the
     * log does - the computer sending them is the proof that it took the
     * keyboard report */
    static int last = -1;
    if (instance != HID_KEYS || report_type != HID_REPORT_TYPE_OUTPUT || !bufsize) return;
    int leds = buffer[bufsize > 1 && report_id == 0 ? 1 : 0];
    if (leds == last) return;
    last = leds;
    ESP_LOGI(TAG, "the computer's keyboard LEDs: num %s, caps %s, scroll %s", leds & 1 ? "on" : "off",
             leds & 2 ? "on" : "off", leds & 4 ? "on" : "off");
}

/* -------------------------------------------------------------------------- */
/* Mode                                                                        */
/* -------------------------------------------------------------------------- */

static aos_hal_usb_mode_t s_mode = AOS_HAL_USB_CONSOLE;
static aos_hal_usb_mode_t s_want;
static volatile bool      s_busy;
static SemaphoreHandle_t  s_hid_mx;     /* one report sequence at a time (app, portal) */
static uint8_t            s_buttons;    /* mouse buttons held (aos_hal_usb_mouse_hold) */

static void tusb_event_cb(tinyusb_event_t *ev, void *arg)
{
    (void)arg;
    switch (ev->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "configured by the computer, at %s speed",
                 tud_speed_get() == TUSB_SPEED_HIGH ? "high (480 Mbit/s)" : "full (12 Mbit/s)");
        aos_p4_usb_net_relink(true);
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGI(TAG, "the computer is gone (unplugged, reset or asleep)");
        aos_p4_usb_net_relink(false);
        break;
#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
    case TINYUSB_EVENT_SUSPENDED:
        ESP_LOGI(TAG, "suspended by the computer (remote wakeup %s)",
                 ev->suspended.remote_wakeup ? "allowed" : "not allowed");
        break;
#endif
#ifdef CONFIG_TINYUSB_RESUME_CALLBACK
    case TINYUSB_EVENT_RESUMED: ESP_LOGI(TAG, "resumed"); break;
#endif
    default: break;
    }
}

static bool device_start(bool disk)
{
    if (!s_serial[0]) {
        uint8_t mac[6] = { 0 };
        esp_read_mac(mac, ESP_MAC_BASE);
        snprintf(s_serial, sizeof s_serial, "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        /* the name as the board is called now; fixed until the next install */
        snprintf(s_product, sizeof s_product, "P4OS %s", aos_hal_device_name());
        aos_p4_usb_net_mac(mac);
        snprintf(s_net_mac, sizeof s_net_mac, "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(tusb_event_cb);   /* HS port on the P4 */
    cfg.descriptor.device = disk ? &s_dev_disk : &s_dev;
    cfg.descriptor.qualifier = &s_qualifier;
    cfg.descriptor.string = s_strings;
    cfg.descriptor.string_count = STR_COUNT;
    cfg.descriptor.full_speed_config = disk ? s_disk_fs : s_cfg_fs;
    cfg.descriptor.high_speed_config = disk ? s_disk_hs : s_cfg_hs;
    esp_err_t e = tinyusb_driver_install(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(e));
        return false;
    }
    ESP_LOGI(TAG, "%s up on the OTG port (%s)", disk ? "the card as a disk" : "keyboard and mouse", s_product);
    return true;
}

static void device_stop(void);

/* The keyboard, and the network over the cable with it: a network that
 * does not come up leaves the keyboard alone. */
static bool keys_start(void)
{
    if (!device_start(false)) return false;
    if (!aos_p4_usb_net_start()) ESP_LOGW(TAG, "the network over the cable did not come up; keyboard only");
    return true;
}

/* A report that could not go, logged at most every 2 s with how many: a
 * macro typing a text into a computer that stopped listening must not
 * flood the ring. */
static void report_failed(const char *why)
{
    static uint32_t last_ms, count;
    count++;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (last_ms && now - last_ms < 2000) return;
    ESP_LOGW(TAG, "%" PRIu32 " report(s) not sent: %s", count, why);
    last_ms = now;
    count = 0;
}

static void device_stop(void)
{
    aos_p4_usb_net_stop();              /* nothing when it is not up */
    esp_err_t e = tinyusb_driver_uninstall();
    if (e != ESP_OK) ESP_LOGW(TAG, "tinyusb_driver_uninstall: %s", esp_err_to_name(e));
}

/* -------------------------------------------------------------------------- */
/* Disk                                                                        */
/* -------------------------------------------------------------------------- */

static sdmmc_card_t                *s_disk_card;
static tinyusb_msc_storage_handle_t s_storage;
static volatile bool                s_card_away;
static aos_hal_usb_mode_t           s_before_disk;     /* where an eject goes back to */

/* esp_tinyusb's storage events, from TinyUSB's task. The card back on the
 * board's side means the computer ejected it or went away: disk mode ends
 * there (see the top of the file). */
static void msc_cb(tinyusb_msc_storage_handle_t h, tinyusb_msc_event_t *ev, void *arg)
{
    (void)h; (void)arg;
    switch (ev->id) {
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE: {
        bool on_board = ev->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP;
        s_card_away = !on_board;
        aos_hal_sd_mark_mounted(on_board);
        if (s_busy) break;              /* the teardown's own moves: disk_stop says what matters */
        ESP_LOGI(TAG, "%s", on_board ? "the computer let go of the card (ejected, or gone)"
                                     : "the computer has the card");
        if (on_board && !s_busy && s_mode == AOS_HAL_USB_DISK) aos_hal_usb_mode_set(s_before_disk);
        break;
    }
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        ESP_LOGW(TAG, "the card could not change hands (%s side)",
                 ev->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP ? "board" : "computer");
        break;
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        ESP_LOGE(TAG, "the card has no filesystem the board can mount; it is NOT formatted here");
        break;
    default: break;
    }
}

static void disk_give_back(void)
{
    aos_p4_sd_card_close(s_disk_card);
    s_disk_card = NULL;
    aos_hal_sd_reclaim();
}

/* Files a Mac reads on a volume it is given. Without the first, Spotlight
 * indexes the whole card as it arrives and keeps it busy: measured on
 * 2026-09-30, the eject was refused for minutes ("dissented by mds"). The
 * second keeps fseventsd from writing its logs onto it. */
static void mac_hints(void)
{
    static const char *const files[] = { "/sdcard/.metadata_never_index", "/sdcard/.fseventsd/no_log" };
    mkdir("/sdcard/.fseventsd", 0777);
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        struct stat st;
        if (stat(files[i], &st) == 0) continue;
        FILE *f = fopen(files[i], "w");
        if (f) fclose(f);
    }
}

static bool disk_start(void)
{
    mac_hints();
    if (!aos_hal_sd_release()) {
        ESP_LOGW(TAG, "no card to hand over");
        return false;
    }
    s_disk_card = aos_p4_sd_card_open();
    if (!s_disk_card) { aos_hal_sd_reclaim(); return false; }
    ESP_LOGI(TAG, "card taken: %s, %llu MB", s_disk_card->cid.name,
             (unsigned long long)((uint64_t)s_disk_card->csd.capacity * s_disk_card->csd.sector_size >> 20));
    const tinyusb_msc_driver_config_t dcfg = { .callback = msc_cb };   /* auto-mount on: the eject is the way back */
    esp_err_t e = tinyusb_msc_install_driver(&dcfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_install_driver: %s", esp_err_to_name(e));
        disk_give_back();
        return false;
    }
    const tinyusb_msc_storage_config_t scfg = {
        .medium.card = s_disk_card,
        .fat_fs = {
            .base_path = "/sdcard",
            .config = { .format_if_mount_failed = false, .max_files = 12, .allocation_unit_size = 64 * 1024 },
            .do_not_format = true,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    e = tinyusb_msc_new_storage_sdmmc(&scfg, &s_storage);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_new_storage_sdmmc: %s", esp_err_to_name(e));
        s_storage = NULL;
        tinyusb_msc_uninstall_driver();
        disk_give_back();
        return false;
    }
    s_card_away = true;
    if (!device_start(true)) {
        tinyusb_msc_delete_storage(s_storage);
        s_storage = NULL;
        tinyusb_msc_uninstall_driver();
        s_card_away = false;
        disk_give_back();
        return false;
    }
    return true;
}

static void disk_stop(void)
{
    device_stop();                      /* the device leaves the bus first */
    if (s_storage) {
        esp_err_t e = tinyusb_msc_delete_storage(s_storage);   /* unmounts /sdcard if it was back */
        if (e != ESP_OK) ESP_LOGW(TAG, "tinyusb_msc_delete_storage: %s", esp_err_to_name(e));
        s_storage = NULL;
    }
    tinyusb_msc_uninstall_driver();
    s_card_away = false;
    aos_hal_sd_mark_mounted(false);
    disk_give_back();
}


/* -------------------------------------------------------------------------- */
/* HOST: a pendrive                                                            */
/* -------------------------------------------------------------------------- */

/* The board as the host of pendrives and keyboards, on the 40-pin header
 * (J3), with 5 V on pin 1 or 3 and GND on 5, on one port or both:
 *
 * - AOS_HAL_USB_HOST_HEADER, pins 21 (D-) and 23 (D+): the P4's second,
 *   Full-Speed controller (USB 1.1, USB_DWC_FS) on its own FSLS PHY 0,
 *   GPIO24/25. Those pads are the USB-Serial-JTAG's at boot, and reach no
 *   connector but the header. ESP-IDF maps this controller to FSLS PHY 1,
 *   GPIO26/27, and GPIO26 is the backlight's PWM here: the mapping is
 *   swapped before the host is installed (usb_wrap_ll_phy_select), with the
 *   USB-Serial-JTAG's pads switched off so that it, sent to PHY 1, does not
 *   take GPIO26 either; host_stop and every boot put both back.
 * - AOS_HAL_USB_HOST_BOTH: the two at once, two root ports of one host
 *   library (components/usb is P4OS's copy of ESP-IDF's, which drives one).
 * - AOS_HAL_USB_HOST_OTG, pins 25 (D-) and 27 (D+): the High-Speed
 *   controller, the same as KEYS and DISK (TinyUSB is uninstalled first),
 *   whose lines also go to the OTG connector, which must stay unplugged
 *   then. With 70 cm of wire its port reset failed on every plug; under
 *   15 cm it runs at High Speed (2026-10-04, docs/USB.md).
 *
 * ESP-IDF's USB Host Library runs in a task of its own; Espressif's
 * usb_host_msc is its client, with its own background task, and tells this
 * file when a mass storage device comes and goes; aos_usb_kbd_p4.c is
 * another client, for keyboards. A task here installs it
 * and mounts its FAT at /usb (msc_host_vfs, a FATFS volume: exFAT is not in
 * ESP-IDF, so the pendrive has to be FAT32), and takes it down when it goes
 * or the host stops. Behind a hub (the library's external hub support)
 * the next ones go to /usb2 and /usb3. */

#define USB_ROOT "/usb"
#define PREF_HOST "usb_host"

/* A pendrive's slot: the first mounts at /usb, the next ones (behind a hub)
 * at /usb2 and /usb3. FATFS has four volumes (sdkconfig.defaults): the card
 * and these three. */
typedef struct {
    msc_host_device_handle_t dev;
    msc_host_vfs_handle_t vfs;
    aos_usb_host_info_t info;
} host_slot_t;

static struct {
    volatile bool stop;
    volatile bool lib_done, app_done;
    QueueHandle_t q;
    host_slot_t slot[AOS_USB_HOST_MAX];
    SemaphoreHandle_t mx;
    unsigned map;               /* the controllers in use: BIT0 High Speed (25/27), BIT1 Full Speed (21/23) */
    usb_phy_handle_t phy[2];    /* their PHYs, set up here (the library's own setup does one) */
} H;

static volatile bool s_host_run;        /* the host library is up, on H.map */

/* the controllers a host port setting takes */
static unsigned host_map(int port)
{
    return port == AOS_HAL_USB_HOST_BOTH ? BIT0 | BIT1 : port == AOS_HAL_USB_HOST_HEADER ? BIT1 : BIT0;
}
static volatile bool s_want_host;

/* The FSLS PHYs as the chip has them after power-on: the USB-Serial-JTAG on
 * PHY 0 (GPIO24/25) with its pads on, the Full-Speed controller on PHY 1.
 * LP_SYS keeps the swap across a software restart, which would leave the
 * USB-Serial-JTAG on GPIO26, the backlight: every boot undoes it. */
static void fsls_phys_default(void)
{
    usb_serial_jtag_ll_phy_select(0);
    USB_SERIAL_JTAG.conf0.usb_pad_enable = 1;
}

__attribute__((constructor)) static void fsls_phys_boot(void) { fsls_phys_default(); }

int aos_hal_usb_host_port(void)
{
    int32_t p = AOS_HAL_USB_HOST_HEADER;
    aos_hal_pref_get_i32(PREF_HOST_PORT, &p);
    return p == AOS_HAL_USB_HOST_OTG || p == AOS_HAL_USB_HOST_BOTH ? (int)p : AOS_HAL_USB_HOST_HEADER;
}

const char *aos_hal_path_usb(void) { return USB_ROOT; }

bool aos_hal_usb_host_on(void) { return s_host_run; }

int aos_hal_usb_host_devices(aos_usb_host_info_t *out, int max)
{
    if (!s_host_run || !H.mx) return -1;
    int n = 0;
    xSemaphoreTake(H.mx, portMAX_DELAY);
    for (int i = 0; i < AOS_USB_HOST_MAX; i++)
        if (H.slot[i].info.device && n < max) out[n++] = H.slot[i].info;
    xSemaphoreGive(H.mx);
    return n;
}

bool aos_hal_usb_host_info(aos_usb_host_info_t *out)
{
    if (!s_host_run || !H.mx) return false;
    memset(out, 0, sizeof *out);
    aos_hal_usb_host_devices(out, 1);
    return true;
}

static void wide_to_str(const wchar_t *w, char *out, size_t n)
{
    size_t i = 0;
    for (; w && w[i] && i + 1 < n; i++) out[i] = w[i] < 128 && w[i] >= 32 ? (char)w[i] : '?';
    out[i] = 0;
}

static void msc_event(const msc_host_event_t *ev, void *arg)
{
    (void)arg;
    msc_host_event_t copy = *ev;
    xQueueSend(H.q, &copy, 0);
}

static void host_unmount(host_slot_t *sl)
{
    if (sl->vfs) { msc_host_vfs_unregister(sl->vfs); sl->vfs = NULL; }
    if (sl->dev) { msc_host_uninstall_device(sl->dev); sl->dev = NULL; }
    xSemaphoreTake(H.mx, portMAX_DELAY);
    memset(&sl->info, 0, sizeof sl->info);
    xSemaphoreGive(H.mx);
}

static void host_mount(uint8_t addr)
{
    int k = 0;
    while (k < AOS_USB_HOST_MAX && H.slot[k].dev) k++;
    if (k == AOS_USB_HOST_MAX) {
        ESP_LOGW(TAG, "a pendrive more than %d: left alone", AOS_USB_HOST_MAX);
        return;
    }
    host_slot_t *sl = &H.slot[k];
    aos_usb_host_info_t in = { .device = true };
    esp_err_t e = msc_host_install_device(addr, &sl->dev);
    if (e != ESP_OK) {
        snprintf(in.error, sizeof in.error, "no contesta como disco (%s)", esp_err_to_name(e));
        sl->dev = NULL;
        /* nothing holds the slot: the info goes nowhere either */
        ESP_LOGW(TAG, "pendrive at address %u: %s", addr, in.error);
        return;
    }
    msc_host_device_info_t di;
    if (msc_host_get_device_info(sl->dev, &di) == ESP_OK) {
        in.vid = di.idVendor;
        in.pid = di.idProduct;
        wide_to_str(di.iManufacturer, in.vendor, sizeof in.vendor);
        wide_to_str(di.iProduct, in.product, sizeof in.product);
        in.bytes = (uint64_t)di.sector_count * di.sector_size;
    }
    char root[sizeof in.path];
    if (k) snprintf(root, sizeof root, USB_ROOT "%d", k + 1);
    else snprintf(root, sizeof root, USB_ROOT);
    const esp_vfs_fat_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 4,
                                            .allocation_unit_size = 0 };
    e = msc_host_vfs_register(sl->dev, root, &mc, &sl->vfs);
    if (e == ESP_OK) {
        in.mounted = true;
        snprintf(in.path, sizeof in.path, "%s", root);
    } else {
        sl->vfs = NULL;
        snprintf(in.error, sizeof in.error, "sin FAT32 que montar (%s): exFAT y NTFS no se leen", esp_err_to_name(e));
    }
    ESP_LOGI(TAG, "pendrive %04x:%04x \"%s %s\", %" PRIu64 " MB: %s%s", in.vid, in.pid, in.vendor, in.product,
             in.bytes / 1000000, in.mounted ? "mounted at " : "", in.mounted ? root : in.error);
    xSemaphoreTake(H.mx, portMAX_DELAY);
    sl->info = in;
    xSemaphoreGive(H.mx);
}

/* the USB Host Library's own loop */
static void host_lib_task(void *arg)
{
    (void)arg;
    bool freeing = false;
    uint32_t shown[2] = { 0xFFFFFFFF, 0xFFFFFFFF };
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(200), &flags);
        /* each root port as its controller sees it, when it changes but for
         * the line states, which toggle with every transfer: for a pendrive
         * that does not come up (docs/USB.md) */
        for (int c = 0; c < 2; c++) {
            if (!(H.map & BIT(c))) continue;
            usb_dwc_dev_t *dwc = USB_DWC_LL_GET_HW(c);
            uint32_t now = dwc->hprt_reg.prtconnsts | dwc->hprt_reg.prtena << 1 | dwc->hprt_reg.prtlnsts << 2 |
                           dwc->hprt_reg.prtspd << 4 | dwc->hprt_reg.prtpwr << 6 | dwc->gotgctl_reg.asesvld << 7 |
                           dwc->gotgctl_reg.bsesvld << 8 | dwc->gotgctl_reg.conidsts << 9;
            if ((now & ~0xCu) == (shown[c] & ~0xCu)) continue;
            shown[c] = now;
            ESP_LOGI(TAG, "host port %s: connected %u, enabled %u, lines D+%u D-%u, speed %u (0 HS 1 FS 2 LS), "
                          "power %u, A-session %u, B-session %u, id %u", c ? "21/23" : "25/27",
                     (unsigned)(now & 1), (unsigned)(now >> 1 & 1), (unsigned)(now >> 2 & 1), (unsigned)(now >> 3 & 1),
                     (unsigned)(now >> 4 & 3), (unsigned)(now >> 6 & 1), (unsigned)(now >> 7 & 1), (unsigned)(now >> 8 & 1),
                     (unsigned)(now >> 9 & 1));
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
            freeing = true;
        }
        if (H.stop && (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE || (freeing && H.app_done))) break;
    }
    H.lib_done = true;
    vTaskDelete(NULL);
}

/* the devices that come and go, and the end */
static void host_app_task(void *arg)
{
    (void)arg;
    while (!H.stop) {
        msc_host_event_t ev;
        if (xQueueReceive(H.q, &ev, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (ev.event == MSC_DEVICE_CONNECTED) {
            host_mount(ev.device.address);
        } else if (ev.event == MSC_DEVICE_DISCONNECTED) {
            for (int i = 0; i < AOS_USB_HOST_MAX; i++)
                if (H.slot[i].dev == ev.device.handle) {
                    ESP_LOGI(TAG, "pendrive gone from %s", H.slot[i].info.path[0] ? H.slot[i].info.path : "its slot");
                    host_unmount(&H.slot[i]);
                }
        }
    }
    for (int i = 0; i < AOS_USB_HOST_MAX; i++) host_unmount(&H.slot[i]);
    msc_host_uninstall();
    H.app_done = true;
    vTaskDelete(NULL);
}

/* The library down, its PHYs, and on the Full-Speed controller the FSLS
 * PHYs as they were. The controller's pads go off only after the library:
 * cut first, with a pendrive on the port, the port saw a sudden
 * disconnection and the library's power-off failed its assert in
 * hub_root_stop (a restart, measured on 2026-10-04). usb_phy leaves them on,
 * with the wrap's clock off: the clock comes back for the one write, as
 * they would be on GPIO26 once the swap is undone. Then the
 * USB-Serial-JTAG goes back on GPIO24/25. */
static esp_err_t host_uninstall(void)
{
    esp_err_t e = usb_host_uninstall();
    for (int c = 0; c < 2; c++)
        if (H.phy[c]) {
            usb_del_phy(H.phy[c]);
            H.phy[c] = NULL;
        }
    if (H.map & BIT1) {
        PERIPH_RCC_ATOMIC() {
            _usb_wrap_ll_enable_bus_clock(true);
        }
        usb_wrap_ll_phy_enable_pad(&USB_WRAP, false);
        PERIPH_RCC_ATOMIC() {
            _usb_wrap_ll_enable_bus_clock(false);
        }
        fsls_phys_default();
    }
    return e;
}

/* A controller's PHY in host mode: the UTMI one of the High-Speed
 * controller, the FSLS one of the Full-Speed controller. */
static bool host_phy(int c)
{
    const usb_phy_config_t pc = { .controller = USB_PHY_CTRL_OTG,
                                  .target = c ? USB_PHY_TARGET_INT : USB_PHY_TARGET_UTMI,
                                  .otg_mode = USB_OTG_MODE_HOST, .otg_speed = USB_PHY_SPEED_UNDEFINED };
    esp_err_t e = usb_new_phy(&pc, &H.phy[c]);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "host: the %s PHY: %s", c ? "Full-Speed" : "High-Speed", esp_err_to_name(e));
        H.phy[c] = NULL;
    }
    return e == ESP_OK;
}

bool aos_p4_usb_kbd_start(void);
void aos_p4_usb_kbd_stop(void);

static bool host_start(void)
{
    if (!H.mx) H.mx = xSemaphoreCreateMutex();
    if (!H.q) H.q = xQueueCreate(8, sizeof(msc_host_event_t));
    memset(H.slot, 0, sizeof H.slot);
    H.stop = H.lib_done = H.app_done = false;
    H.map = host_map(aos_hal_usb_host_port());
    gpio_drive_cap_t cap26 = GPIO_DRIVE_CAP_DEFAULT, cap27 = GPIO_DRIVE_CAP_DEFAULT;
    if (H.map & BIT1) {
        /* the USB-Serial-JTAG off its pads, and the Full-Speed controller
         * onto FSLS PHY 0 (GPIO24/25) before its PHY is set up: the wrap's
         * reset in usb_new_phy does not touch LP_SYS, so the swap holds */
        USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;
        usb_wrap_ll_phy_select(&USB_WRAP, 0);
        gpio_get_drive_capability(GPIO_NUM_26, &cap26);
        gpio_get_drive_capability(GPIO_NUM_27, &cap27);
    }
    /* The PHYs set up here: the library's own setup does only one, and both
     * controllers at once are two (components/usb, P4OS's copy of ESP-IDF's
     * host library, drives both as two root ports). */
    bool ok = (!(H.map & BIT0) || host_phy(0)) && (!(H.map & BIT1) || host_phy(1));
    /* The ports come up unpowered, the controllers are set up, and only
     * then they are powered. */
    const usb_host_config_t hc = { .skip_phy_setup = true, .root_port_unpowered = true,
                                   .intr_flags = ESP_INTR_FLAG_LEVEL1, .peripheral_map = H.map };
    esp_err_t e = ok ? usb_host_install(&hc) : ESP_FAIL;
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "host: usb_host_install: %s", esp_err_to_name(e));
        for (int c = 0; c < 2; c++)
            if (H.phy[c]) {
                usb_del_phy(H.phy[c]);
                H.phy[c] = NULL;
            }
        if (H.map & BIT1) fsls_phys_default();
        return false;
    }
    if (H.map & BIT1) {
        /* usb_phy gives the 40 mA drive of USB pads to the pins it thinks
         * the PHY is on (26/27, the backlight): back to what they had, and
         * to the pins it really is on */
        gpio_set_drive_capability(GPIO_NUM_26, cap26);
        gpio_set_drive_capability(GPIO_NUM_27, cap27);
        gpio_set_drive_capability(GPIO_NUM_24, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability(GPIO_NUM_25, GPIO_DRIVE_CAP_3);
    }
    if (H.map & BIT0) {
        /* (Forcing Full Speed here, FSLSSupp, was tried on 2026-10-04 on
         * long wires: the port reset failed all the same. Short wires were
         * the answer: it stays at High Speed.) */
        usb_dwc_dev_t *dwc = USB_DWC_LL_GET_HW(0);
        /* VBUS valid, by override. This board's VBUS (the OTG connector's,
         * and the header's 5 V) reaches no pin of the P4 (schematic, USB
         * page): the PHY never sees a session, and a host without one may
         * drop its port right at the reset. (The Full-Speed controller gets
         * them from ESP-IDF, through the GPIO matrix.) */
        dwc->gotgctl_reg.vbvalidoven = 1;
        dwc->gotgctl_reg.vbvalidovval = 1;
        dwc->gotgctl_reg.avalidoven = 1;
        dwc->gotgctl_reg.avalidovval = 1;
    }
    usb_host_lib_set_root_port_power(true);
    if (xTaskCreatePinnedToCore(host_lib_task, "usb_host", 4096, NULL, 5, NULL, 0) != pdPASS) {
        host_uninstall();
        return false;
    }
    const msc_host_driver_config_t mc = { .create_backround_task = true, .task_priority = 5, .stack_size = 4096,
                                          .core_id = 0, .callback = msc_event };
    e = msc_host_install(&mc);
    if (e != ESP_OK || xTaskCreatePinnedToCore(host_app_task, "usb_msc", 4096, NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "host: msc_host_install: %s", esp_err_to_name(e));
        H.stop = true;
        H.app_done = true;
        if (e == ESP_OK) msc_host_uninstall();
        for (int i = 0; i < 50 && !H.lib_done; i++) vTaskDelay(pdMS_TO_TICKS(20));
        host_uninstall();
        return false;
    }
    /* keyboards: a client of its own beside usb_host_msc */
    if (!aos_p4_usb_kbd_start()) ESP_LOGW(TAG, "host: no keyboard client");
    ESP_LOGI(TAG, "host: waiting for pendrives and keyboards on %s",
             H.map == (BIT0 | BIT1) ? "J3 25/27 (High Speed) and 21/23 (Full Speed)"
             : H.map & BIT1 ? "J3 21 D- 23 D+ (Full-Speed controller)"
                            : "J3 25 D- 27 D+ or the OTG connector (High-Speed controller)");
    return true;
}

static void host_stop(void)
{
    aos_p4_usb_kbd_stop();
    H.stop = true;
    usb_host_lib_unblock();
    for (int i = 0; i < 150 && !(H.app_done && H.lib_done); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
        usb_host_lib_unblock();
    }
    esp_err_t e = host_uninstall();
    ESP_LOGI(TAG, "host: shut (%s)", esp_err_to_name(e));
}

bool aos_hal_usb_card_away(void) { return s_mode == AOS_HAL_USB_DISK && s_card_away; }

/* One switch at a time, in a task of its own: installing and tearing down
 * TinyUSB or the host library takes a while and must not hold the LVGL task
 * that asked. It goes from where the port is to s_want (the OTG
 * controller's role: idle, keys or disk) and s_want_host (the pendrive
 * host), in that order of teardown: the host first if it goes or moves,
 * then the OTG's role, then the host up. A host on pins 25/27 takes the
 * OTG controller, so the role is idle then and s_mode says HOST. */
static void switch_task(void *arg)
{
    (void)arg;
    aos_hal_usb_mode_t want = s_want;
    bool host = s_want_host;
    unsigned map = host_map(aos_hal_usb_host_port());
    if (host && (map & BIT0)) want = AOS_HAL_USB_CONSOLE;
    if (s_host_run && (!host || H.map != map)) {
        host_stop();
        s_host_run = false;
        if (s_mode == AOS_HAL_USB_HOST) s_mode = AOS_HAL_USB_CONSOLE;
    }
    if (s_mode != want && s_mode != AOS_HAL_USB_HOST) {
        /* an eject asks for the switch from inside TinyUSB's task: let it
         * answer the computer before the driver goes */
        if (s_mode == AOS_HAL_USB_DISK) vTaskDelay(pdMS_TO_TICKS(200));
        if (s_mode == AOS_HAL_USB_KEYS) device_stop();
        else if (s_mode == AOS_HAL_USB_DISK) disk_stop();
        s_mode = AOS_HAL_USB_CONSOLE;
        if (want == AOS_HAL_USB_KEYS && keys_start()) s_mode = AOS_HAL_USB_KEYS;
        else if (want == AOS_HAL_USB_DISK && disk_start()) s_mode = AOS_HAL_USB_DISK;
    }
    if (host && !s_host_run) {
        if (host_start()) {
            s_host_run = true;
            if (H.map & BIT0) s_mode = AOS_HAL_USB_HOST;
        }
    }
    s_want_host = s_host_run;
    ESP_LOGI(TAG, "the port is %s now%s", s_mode == AOS_HAL_USB_KEYS ? "keyboard and mouse"
                                         : s_mode == AOS_HAL_USB_DISK ? "the card as a disk"
                                         : s_mode == AOS_HAL_USB_HOST ? "the host of a pendrive" : "idle",
             s_host_run && H.map == BIT1 ? ", and the host of a pendrive on J3 21/23" : "");
    /* remembered for the next boot (aos_hal_usb_restore). Disk mode is
     * not: the boot reads the card, so a restart in disk mode comes back
     * in the mode it had before. */
    int32_t keep = s_mode == AOS_HAL_USB_DISK ? (int32_t)s_before_disk : (int32_t)s_mode, was = -1;
    if (!aos_hal_pref_get_i32(PREF_MODE, &was) || was != keep) aos_hal_pref_set_i32(PREF_MODE, keep);
    if (!aos_hal_pref_get_i32(PREF_HOST, &was) || was != (int32_t)s_host_run) aos_hal_pref_set_i32(PREF_HOST, s_host_run);
    s_buttons = 0;
    s_busy = false;
    vTaskDelete(NULL);
}

static bool switch_start(void)
{
    if (!s_hid_mx) s_hid_mx = xSemaphoreCreateMutex();
    s_busy = true;
    if (xTaskCreatePinnedToCore(switch_task, "usb_sw", 4096, NULL, 3, NULL, 0) != pdPASS) {
        s_busy = false;
        return false;
    }
    return true;
}

void aos_hal_usb_restore(void)
{
    int32_t m = AOS_HAL_USB_CONSOLE, h = 0;
    aos_hal_pref_get_i32(PREF_MODE, &m);
    aos_hal_pref_get_i32(PREF_HOST, &h);
    if (m != AOS_HAL_USB_KEYS && !h) return;
    ESP_LOGI(TAG, "before the restart the port was %s%s", m == AOS_HAL_USB_KEYS ? "keyboard and mouse" : "idle",
             h ? ", with the pendrive host" : "");
    s_want = m == AOS_HAL_USB_KEYS ? AOS_HAL_USB_KEYS : AOS_HAL_USB_CONSOLE;
    s_want_host = h;
    switch_start();
}

bool aos_hal_usb_host_set(bool on)
{
    if (s_busy) return false;
    if (on == s_host_run) return true;
    ESP_LOGI(TAG, "%s the pendrive host", on ? "starting" : "stopping");
    /* on pins 25/27 the host takes the OTG controller from its role */
    s_want = on && (host_map(aos_hal_usb_host_port()) & BIT0) ? AOS_HAL_USB_CONSOLE
           : s_mode == AOS_HAL_USB_HOST ? AOS_HAL_USB_CONSOLE : s_mode;
    s_want_host = on;
    return switch_start();
}

bool aos_hal_usb_host_port_set(int port)
{
    if (port != AOS_HAL_USB_HOST_OTG && port != AOS_HAL_USB_HOST_HEADER && port != AOS_HAL_USB_HOST_BOTH) return false;
    if (s_busy) return false;
    if (port == aos_hal_usb_host_port()) return true;
    aos_hal_pref_set_i32(PREF_HOST_PORT, port);
    if (!s_host_run) return true;
    /* host on: down and up again on the other controller */
    ESP_LOGI(TAG, "the pendrive's port is J3 %s now", port == AOS_HAL_USB_HOST_BOTH ? "21/23 and 25/27"
                                                     : port == AOS_HAL_USB_HOST_HEADER ? "21/23" : "25/27");
    s_want = (host_map(port) & BIT0) || s_mode == AOS_HAL_USB_HOST ? AOS_HAL_USB_CONSOLE : s_mode;
    s_want_host = true;
    return switch_start();
}

aos_hal_usb_mode_t aos_hal_usb_mode(void) { return s_mode; }
bool aos_hal_usb_busy(void) { return s_busy; }

/* The OTG connector's role. HOST is aos_hal_usb_host_set(true), kept for
 * the callers of before; any other role, with the host on the OTG
 * controller, stops the host. */
bool aos_hal_usb_mode_set(aos_hal_usb_mode_t mode)
{
    if (mode == AOS_HAL_USB_HOST) return aos_hal_usb_host_set(true);
    if (s_busy) return false;
    if (mode == s_mode) return true;
    if (mode == AOS_HAL_USB_DISK) s_before_disk = s_mode == AOS_HAL_USB_HOST ? AOS_HAL_USB_CONSOLE : s_mode;
    ESP_LOGI(TAG, "switching the port to %s", mode == AOS_HAL_USB_KEYS ? "keyboard and mouse"
                                             : mode == AOS_HAL_USB_DISK ? "the card as a disk" : "idle");
    s_want = mode;
    s_want_host = s_host_run && H.map == BIT1;
    return switch_start();
}

bool aos_hal_usb_connected(void)
{
    return !s_busy && (s_mode == AOS_HAL_USB_KEYS || s_mode == AOS_HAL_USB_DISK) && tud_mounted();
}

static bool usb_ready(void)
{
    return !s_busy && s_mode == AOS_HAL_USB_KEYS && tud_mounted();
}

/* -------------------------------------------------------------------------- */
/* The same keys over Bluetooth                                                */
/* -------------------------------------------------------------------------- */

/* With no computer on the cable, a computer paired over Bluetooth in the
 * keyboard mode (components/aos_ble/aos_ble_hid.c) takes the same reports:
 * the Macro pad, the portal and the apps send keys without knowing which
 * way they go. The cable wins when there are both. The gamepad and MIDI
 * stay USB only. Weak: aos_ble is a component of its own. */
bool aos_ble_hid_ready(void) __attribute__((weak));
bool aos_ble_hid_send(int report_id, const void *data, size_t len) __attribute__((weak));

static bool bt_ready(void) { return !usb_ready() && aos_ble_hid_ready && aos_ble_hid_ready(); }

static bool bt_key(uint8_t mods, uint8_t key)
{
    uint8_t r[8] = { mods, 0, key, 0, 0, 0, 0, 0 };
    if (!aos_ble_hid_send(RID_KEYBOARD, r, sizeof r)) return false;
    vTaskDelay(pdMS_TO_TICKS(12));      /* a connection interval or so: some hosts miss a press released at once */
    memset(r, 0, sizeof r);
    aos_ble_hid_send(RID_KEYBOARD, r, sizeof r);
    return true;
}

static bool bt_consumer(uint16_t usage)
{
    uint8_t r[2] = { (uint8_t)usage, (uint8_t)(usage >> 8) };
    if (!aos_ble_hid_send(RID_CONSUMER, r, sizeof r)) return false;
    vTaskDelay(pdMS_TO_TICKS(12));
    memset(r, 0, sizeof r);
    aos_ble_hid_send(RID_CONSUMER, r, sizeof r);
    return true;
}

static bool bt_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    int8_t r[5] = { (int8_t)buttons, dx, dy, wheel, 0 };
    return aos_ble_hid_send(RID_MOUSE, r, sizeof r);
}

bool aos_hal_usb_keys_ready(void) { return usb_ready() || bt_ready(); }

/* -------------------------------------------------------------------------- */
/* Reports                                                                     */
/* -------------------------------------------------------------------------- */

static bool wait_ready(void)
{
    for (int i = 0; i < 40 && !tud_hid_ready(); i++) vTaskDelay(pdMS_TO_TICKS(1));
    if (tud_hid_ready()) return true;
    report_failed("the computer took nothing for 40 ms");
    return false;
}

static bool begin(void)
{
    if (!usb_ready()) {
        report_failed(s_busy ? "the port is switching modes" : s_mode != AOS_HAL_USB_KEYS ? "the port is not a keyboard"
                      : "no computer has configured the port");
        return false;
    }
    if (tud_suspended()) {                  /* asleep: wake it, then give it a moment */
        tud_remote_wakeup();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    xSemaphoreTake(s_hid_mx, portMAX_DELAY);
    return true;
}

static void end(void) { xSemaphoreGive(s_hid_mx); }

static bool key_press(uint8_t mods, uint8_t key)
{
    uint8_t keys[6] = { key, 0, 0, 0, 0, 0 };
    if (!wait_ready() || !tud_hid_keyboard_report(RID_KEYBOARD, mods, key ? keys : NULL)) return false;
    /* held for a couple of polls: some hosts miss a press released at once */
    vTaskDelay(pdMS_TO_TICKS(8));
    wait_ready();
    tud_hid_keyboard_report(RID_KEYBOARD, 0, NULL);
    return true;
}

static bool consumer_press(uint16_t usage)
{
    if (!wait_ready() || !tud_hid_report(RID_CONSUMER, &usage, sizeof usage)) return false;
    vTaskDelay(pdMS_TO_TICKS(8));
    wait_ready();
    uint16_t none = 0;
    tud_hid_report(RID_CONSUMER, &none, sizeof none);
    return true;
}

bool aos_hal_usb_key_valid(const char *name)
{
    aos_hid_combo_t c;
    return aos_hid_parse(name, &c);
}

bool aos_hal_usb_key(const char *name)
{
    aos_hid_combo_t c;
    if (!aos_hid_parse(name, &c)) return false;
    if (bt_ready()) return c.consumer ? bt_consumer(c.consumer) : bt_key(c.mods, c.key);
    if (!begin()) return false;
    bool ok = c.consumer ? consumer_press(c.consumer) : key_press(c.mods, c.key);
    end();
    return ok;
}

int aos_hal_usb_type(const char *ascii)
{
    if (!ascii) return 0;
    if (bt_ready()) {
        int sent = 0;
        for (const char *p = ascii; *p; p++) {
            uint8_t k;
            bool sh;
            if (!aos_hid_ascii(*p, &k, &sh)) continue;
            if (!bt_key(sh ? AOS_HID_MOD_SHIFT : 0, k)) break;
            sent++;
        }
        return sent;
    }
    if (!begin()) return 0;
    int sent = 0;
    for (const char *p = ascii; *p; p++) {
        uint8_t k;
        bool sh;
        if (!aos_hid_ascii(*p, &k, &sh)) continue;         /* not on a US keyboard */
        if (!key_press(sh ? AOS_HID_MOD_SHIFT : 0, k)) break;
        sent++;
    }
    end();
    return sent;
}

static int8_t clamp8(int v) { return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v); }

bool aos_hal_usb_mouse(int dx, int dy, int wheel)
{
    /* no waiting: a report still in flight drops this one, a pixel nobody misses */
    if (bt_ready()) return bt_mouse(s_buttons, clamp8(dx), clamp8(dy), clamp8(wheel));
    if (!usb_ready() || !tud_hid_ready()) return false;
    return tud_hid_mouse_report(RID_MOUSE, s_buttons, clamp8(dx), clamp8(dy), clamp8(wheel), 0);
}

bool aos_hal_usb_mouse_hold(int buttons)
{
    s_buttons = (uint8_t)(buttons & 0x03);
    if (bt_ready()) return bt_mouse(s_buttons, 0, 0, 0);
    if (!begin()) return false;
    bool ok = wait_ready() && tud_hid_mouse_report(RID_MOUSE, s_buttons, 0, 0, 0, 0);
    end();
    return ok;
}

bool aos_hal_usb_click(int button)
{
    uint8_t b = button == 2 ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT;
    if (bt_ready()) {
        if (!bt_mouse((uint8_t)(s_buttons | b), 0, 0, 0)) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
        bt_mouse(s_buttons, 0, 0, 0);
        return true;
    }
    if (!begin()) return false;
    bool ok = wait_ready() && tud_hid_mouse_report(RID_MOUSE, (uint8_t)(s_buttons | b), 0, 0, 0, 0);
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(20));
        wait_ready();
        tud_hid_mouse_report(RID_MOUSE, s_buttons, 0, 0, 0, 0);
    }
    end();
    return ok;
}

/* The gamepad: one report, no waiting, like the mouse - a report still in
 * flight drops this one, and the next move sends the state again anyway.
 * One stick (x, y), the hat (TinyUSB's: 0 centred, 1 up, then clockwise to
 * 8 up-left) and 32 buttons (GAMEPAD_BUTTON_*). */
bool aos_hal_usb_gamepad(int x, int y, int hat, unsigned buttons)
{
    if (!usb_ready() || !tud_hid_n_ready(HID_PAD)) return false;
    return tud_hid_n_gamepad_report(HID_PAD, 0, clamp8(x), clamp8(y), 0, 0, 0, 0,
                                    (uint8_t)(hat < 0 || hat > 8 ? 0 : hat), (uint32_t)buttons);
}

/* MIDI on channel 1. A message the computer is not reading (no app has the
 * port open) is dropped by TinyUSB when its FIFO is full: never blocks. */
bool aos_hal_usb_midi_ready(void)
{
    /* not tud_midi_mounted(): it wants both directions, and this port has
     * one (MIDI_OUT_DESCRIPTOR); the write checks its endpoint anyway.
     * The cable only: MIDI does not go over Bluetooth. */
    return usb_ready();
}

static bool midi3(uint8_t status, uint8_t d1, uint8_t d2)
{
    if (!aos_hal_usb_midi_ready()) return false;
    uint8_t msg[3] = { status, (uint8_t)(d1 & 0x7F), (uint8_t)(d2 & 0x7F) };
    return tud_midi_stream_write(0, msg, 3) == 3;
}

static int clamp7(int v) { return v < 0 ? 0 : v > 127 ? 127 : v; }

bool aos_hal_usb_midi_note(int note, int velocity, bool on)
{
    return midi3(on ? 0x90 : 0x80, (uint8_t)clamp7(note), (uint8_t)(on ? clamp7(velocity) : 0));
}

bool aos_hal_usb_midi_cc(int control, int value) { return midi3(0xB0, (uint8_t)clamp7(control), (uint8_t)clamp7(value)); }

bool aos_hal_usb_midi_bend(int value)
{
    int v = value + 8192;
    v = v < 0 ? 0 : v > 16383 ? 16383 : v;
    return midi3(0xE0, (uint8_t)(v & 0x7F), (uint8_t)(v >> 7));
}
