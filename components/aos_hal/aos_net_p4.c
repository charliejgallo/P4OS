/*
 * P4OS HAL - the network, through the ESP32-C6.
 *
 * The P4 has no radio: esp_wifi_remote turns the ordinary esp_wifi_* calls
 * into RPCs to the C6 over SDIO (esp_hosted 3.x since P4OS 0.7, 1.4 before;
 * docs/C6.md). From here on it is a plain station:
 *
 *   - the network is kept in NVS (wf_ssid / wf_pass) and the switch in
 *     "net_on", so Settings and the Control Centre survive a restart;
 *   - a lost connection is retried with growing pauses (1 s ... 60 s) and
 *     never given up: this is a bench device on a wall plug, not a watch;
 *   - with an address it starts SNTP (the board has no RTC with a cell) and
 *     answers as <device name>.local;
 *   - the radio comes up in a task of its own after boot: the C6's firmware
 *     handshake takes a moment and the screen must not wait for it.
 *
 * And an access point of its own (P4OS-XXXX), for when there is no network
 * to join: away from home, the phone joins the board and opens the portal at
 * 192.168.4.1. See "The access point" below.
 *
 * Not here yet: Bluetooth, ESP-NOW (esp_hosted does not carry it, 3.0.9 neither:
 * HARDWARE.md test 10).
 */
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_hosted.h"
#include "eh_host_mcu_transport_init_event.h"
#include "esp_hosted_ota.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "mdns.h"

static const char *TAG = "net";

#define EV_INITED   BIT0
#define EV_GOT_IP   BIT1

static EventGroupHandle_t s_ev;
static esp_netif_t *s_sta;
static esp_timer_handle_t s_retry;
static volatile aos_net_state_t s_state = AOS_NET_OFF;
static volatile bool s_inited, s_want;
static uint32_t s_failures;
static uint8_t s_reason;
static uint32_t s_next_s;
static char s_ip[16];
static char s_ssid[33];
static bool s_mdns, s_sntp;
static volatile bool s_ap_on;           /* the access point is up (see below) */
static char s_c6_fw[24];                /* the C6's esp_hosted firmware, asked once at boot */
static volatile int s_ap_clients;       /* phones on it, counted from its events */

/* ---- credentials ---- */

static bool creds_get(char *ssid, size_t sn, char *pass, size_t pn)
{
    if (!aos_hal_pref_get_str("wf_ssid", ssid, sn) || !ssid[0]) return false;
    if (!aos_hal_pref_get_str("wf_pass", pass, pn)) pass[0] = 0;
    return true;
}

bool aos_hal_net_has_credentials(void)
{
    char s[33], p[65];
    return creds_get(s, sizeof s, p, sizeof p);
}

bool aos_hal_net_enabled(void)
{
    int32_t on = 1;
    aos_hal_pref_get_i32("net_on", &on);
    return on != 0;
}

/* ---- connecting ---- */

static void connect_now(void)
{
    char ssid[33], pass[65];
    if (!s_inited || !s_want || !creds_get(ssid, sizeof ssid, pass, sizeof pass)) return;
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    snprintf(s_ssid, sizeof s_ssid, "%s", ssid);
    s_state = AOS_NET_CONNECTING;
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK) ESP_LOGW(TAG, "connect: %s", esp_err_to_name(e));
}

static void retry_cb(void *arg) { connect_now(); }

static void schedule_retry(void)
{
    static const uint32_t STEPS[] = { 1, 2, 5, 10, 20, 30, 60 };
    uint32_t i = s_failures < sizeof STEPS / sizeof STEPS[0] ? s_failures : sizeof STEPS / sizeof STEPS[0] - 1;
    s_next_s = STEPS[i];
    esp_timer_stop(s_retry);
    esp_timer_start_once(s_retry, (uint64_t)s_next_s * 1000000);
}

/* mDNS, once: with the Wi-Fi, or with the network over the USB cable
 * (aos_usb_net_p4.c), whichever comes first. */
static void mdns_up(void)
{
    if (!s_mdns && mdns_init() == ESP_OK) {
        s_mdns = true;
        mdns_hostname_set(aos_hal_device_name());
        mdns_instance_name_set(aos_hal_device_name());
        ESP_LOGI(TAG, "answers as %s.local", aos_hal_device_name());
    }
}

bool aos_hal_mdns_add_netif(void *esp_netif)
{
    mdns_up();
    if (!s_mdns) return false;
    esp_err_t e = mdns_register_netif((esp_netif_t *)esp_netif);
    if (e == ESP_OK) e = mdns_netif_action((esp_netif_t *)esp_netif, MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "mDNS on the USB network: %s", esp_err_to_name(e));
        mdns_unregister_netif((esp_netif_t *)esp_netif);
        return false;
    }
    ESP_LOGI(TAG, "%s.local also answers over the USB cable", aos_hal_device_name());
    return true;
}

void aos_hal_mdns_remove_netif(void *esp_netif)
{
    mdns_netif_action((esp_netif_t *)esp_netif, MDNS_EVENT_DISABLE_IP4);
    mdns_unregister_netif((esp_netif_t *)esp_netif);
}

static void start_services(void)
{
    if (!s_sntp) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        cfg.start = true;
        s_sntp = esp_netif_sntp_init(&cfg) == ESP_OK;
    } else {
        esp_netif_sntp_start();
    }
    mdns_up();
}

static esp_netif_t *s_ap_netif;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_ev, EV_GOT_IP);
        s_ip[0] = 0;
        s_reason = d->reason;
        if (!s_want) { s_state = AOS_NET_OFF; return; }
        s_failures++;
        s_state = s_failures > 3 ? AOS_NET_FAILED : AOS_NET_CONNECTING;
        if (s_ap_on) {
            /* Looking for the home network scans every channel, and the
             * access point stops beaconing on its own while it does: the
             * phone on it would drop every few seconds. With the AP up the
             * board stops looking; choosing a network, or turning the AP
             * off, looks again. */
            ESP_LOGW(TAG, "disconnected (reason %d); not retrying while the access point is up", d->reason);
            s_state = AOS_NET_FAILED;
            return;
        }
        ESP_LOGW(TAG, "disconnected (reason %d), retry %u", d->reason, (unsigned)s_failures);
        schedule_retry();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *c = data;
        s_ap_clients++;
        ESP_LOGI(TAG, "access point: " MACSTR " joined (%d on it)", MAC2STR(c->mac), s_ap_clients);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *c = data;
        if (s_ap_clients > 0) s_ap_clients--;
        ESP_LOGI(TAG, "access point: " MACSTR " left (reason %d, %d on it)", MAC2STR(c->mac), c->reason, s_ap_clients);
    } else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *a = data;
        /* every DHCP server says so, the USB cable's too: only ours */
        if (a->esp_netif == s_ap_netif) ESP_LOGI(TAG, "access point: gave " IPSTR, IP2STR(&a->ip));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *g = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&g->ip_info.ip));
        s_failures = 0;
        s_next_s = 0;
        s_state = AOS_NET_CONNECTED;
        xEventGroupSetBits(s_ev, EV_GOT_IP);
        ESP_LOGI(TAG, "%s: %s", s_ssid, s_ip);
        start_services();
    }
}

/* Modem sleep between beacons put the ping at ~200 ms on the board and, with
 * it, TCP at 170 KB/s (2026-09-29). The board runs on USB for now, so the
 * radio stays awake; the preference wifi_ps=1 brings the saving back for the
 * day it runs on a battery. */
static wifi_ps_type_t idle_ps(void)
{
    int32_t v = 0;
    aos_hal_pref_get_i32("wifi_ps", &v);
    return v ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE;
}

void aos_bt_p4_radio_up(void) __attribute__((weak));

static void net_task(void *arg)
{
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta, aos_hal_device_name());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    int64_t t = esp_timer_get_time();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "wifi init: %s (is the C6 answering?)", esp_err_to_name(e));
        vTaskDeleteWithCaps(NULL);
        return;
    }
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_event, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);     /* the network lives in our prefs, not in the C6 */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_err_t pe = esp_wifi_set_ps(idle_ps());
    wifi_ps_type_t ps = WIFI_PS_NONE;
    esp_wifi_get_ps(&ps);
    ESP_LOGI(TAG, "power save: asked %d (%s), the C6 has %d", (int)idle_ps(), esp_err_to_name(pe), (int)ps);
    s_inited = true;
    xEventGroupSetBits(s_ev, EV_INITED);
    ESP_LOGI(TAG, "radio up in %lld ms", (esp_timer_get_time() - t) / 1000);
    s_want = aos_hal_net_enabled();
    int32_t ap = 0;
    aos_hal_pref_get_i32("ap_on", &ap);
    if (ap) aos_hal_net_ap_start();     /* it was up before the restart */
    connect_now();
    /* the co-processor's firmware, once. A C6 from esp_hosted 2.x on says
     * it in its INIT event, kept by the transport: no RPC. Without it, the
     * RPC, from this task: one made from the UI under LVGL's lock is what
     * froze the screen once (see the RSSI below). Settings' Diagnostics
     * reads the copy. After the connection has been started: the factory
     * firmware of the replacement board's C6 (2026-10-03) says 0.0.0 in
     * that event and lets the RPC time out after 5 s, and the network must
     * not wait for that. */
    uint32_t v = eh_host_mcu_transport_get_fw_version();
    esp_hosted_coprocessor_fwver_t fw = { 0 };
    if (v) {
        snprintf(s_c6_fw, sizeof s_c6_fw, "%u.%u.%u", (unsigned)(v >> 16 & 0xFF), (unsigned)(v >> 8 & 0xFF),
                 (unsigned)(v & 0xFF));
        ESP_LOGI(TAG, "the C6 runs esp_hosted %s", s_c6_fw);
    } else if (esp_hosted_get_coprocessor_fwversion(&fw) == ESP_OK) {
        snprintf(s_c6_fw, sizeof s_c6_fw, "%u.%u.%u", (unsigned)fw.major1, (unsigned)fw.minor1, (unsigned)fw.patch1);
        ESP_LOGI(TAG, "the C6 runs esp_hosted %s", s_c6_fw);
    } else {
        snprintf(s_c6_fw, sizeof s_c6_fw, "?");
        ESP_LOGW(TAG, "the C6 did not say its firmware version");
    }
    /* Bluetooth goes through the same link (components/aos_ble) */
    if (aos_bt_p4_radio_up) aos_bt_p4_radio_up();
    vTaskDeleteWithCaps(NULL);
}

/* Called at the end of aos_hal_init. */
void aos_net_p4_start(void)
{
    s_ev = xEventGroupCreate();
    const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi-retry" };
    esp_timer_create(&ta, &s_retry);
    xTaskCreateWithCaps(net_task, "net-up", 6144, NULL, 5, NULL, MALLOC_CAP_SPIRAM);   /* no flash in it */
}

bool aos_net_p4_up(void) { return s_inited; }

/* ---- the HAL's network API ---- */

aos_net_state_t aos_hal_net_state(void) { return s_state; }
const char *aos_hal_net_coprocessor_fw(void) { return s_c6_fw; }

bool aos_hal_net_test_freeze_link(void)
{
    /* Not a reset of the C6: that one esp_hosted sees by itself (the SDIO
     * writes fail and its driver restarts the board, tested). */
    TaskHandle_t t = xTaskGetHandle("sdio_process_rx");
    if (!t) return false;
    ESP_LOGW(TAG, "test: sdio_process_rx frozen; the link watchdog should restart the board");
    vTaskSuspend(t);
    return true;
}

/* ---- the C6's firmware, installed from the P4 ----
 *
 * The co-processor OTA of esp_hosted: begin, the image in chunks, end, and
 * on a C6 from 2.6 on, activate. The C6 writes it into the slot it is not
 * running and checks it (esp_ota_end) before it switches; a cut transfer or
 * a corrupt image leaves the running firmware in place. A firmware that is
 * sound but cannot talk to the P4 is the one case with no way back from
 * here: the C6 has no rollback, and its serial port (J7) is the recovery
 * (docs/C6.md). So the file must say it is an app for the ESP32-C6 before
 * anything is sent. Then the board restarts: the boot resets the C6
 * through its EN line, and both sides meet again from the start. */
#define C6_CHUNK 1400               /* old C6 firmware takes up to 1500 per RPC */

static aos_c6_update_t s_c6up;
static char s_c6_path[128];
static volatile bool s_c6_busy;

bool aos_net_p4_c6_updating(void) { return s_c6_busy; }

static bool c6_image_read(FILE *f, char *version, size_t n, uint32_t *size)
{
    esp_image_header_t h;
    esp_image_segment_header_t seg;
    esp_app_desc_t d;
    if (fread(&h, sizeof h, 1, f) != 1 || h.magic != ESP_IMAGE_HEADER_MAGIC || h.chip_id != ESP_CHIP_ID_ESP32C6)
        return false;
    if (fread(&seg, sizeof seg, 1, f) != 1 || fread(&d, sizeof d, 1, f) != 1 || d.magic_word != ESP_APP_DESC_MAGIC_WORD)
        return false;
    if (version) snprintf(version, n, "%.*s", (int)sizeof d.version, d.version);
    if (size) {
        fseek(f, 0, SEEK_END);
        *size = (uint32_t)ftell(f);
    }
    return true;
}

bool aos_hal_net_coprocessor_image(const char *path, char *version, size_t n)
{
    if (version && n) version[0] = 0;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return false;
    bool ok = c6_image_read(f, version, n, NULL);
    fclose(f);
    if (!ok && version && n) version[0] = 0;
    return ok;
}

static void c6_fail(const char *why, esp_err_t e)
{
    snprintf(s_c6up.error, sizeof s_c6up.error, "%s%s%s", why, e ? ": " : "", e ? esp_err_to_name(e) : "");
    ESP_LOGE(TAG, "C6 update: %s", s_c6up.error);
    s_c6up.state = AOS_C6_FAILED;
}

static void c6_update_task(void *arg)
{
    FILE *f = fopen(s_c6_path, "rb");
    uint8_t *buf = heap_caps_malloc(C6_CHUNK, MALLOC_CAP_SPIRAM);
    esp_err_t e = ESP_OK;
    bool begun = false;
    if (!f || !buf) {
        c6_fail("no se pudo abrir la imagen", 0);
        goto out;
    }
    ESP_LOGW(TAG, "C6 update: %s, %u bytes, version %s (the C6 runs %s)", s_c6_path, (unsigned)s_c6up.total,
             s_c6up.version, s_c6_fw[0] ? s_c6_fw : "?");
    if ((e = esp_hosted_slave_ota_begin()) != ESP_OK) {
        c6_fail("el C6 no aceptó empezar", e);
        goto out;
    }
    begun = true;
    size_t got;
    while ((got = fread(buf, 1, C6_CHUNK, f)) > 0) {
        if ((e = esp_hosted_slave_ota_write(buf, got)) != ESP_OK) {
            c6_fail("se cortó al mandar la imagen", e);
            goto out;
        }
        s_c6up.sent += got;
    }
    if ((e = esp_hosted_slave_ota_end()) != ESP_OK) {
        c6_fail("el C6 rechazó la imagen", e);
        goto out;
    }
    /* From 2.6 on the C6 switches slots on activate; older ones did it at
     * end and restart by themselves 5 s later. Which one it is comes from
     * its INIT event (0 for the factory firmware, which predates it). */
    uint32_t v = eh_host_mcu_transport_get_fw_version();
    if (v >= ((2u << 16) | (6u << 8)) && (e = esp_hosted_slave_ota_activate()) != ESP_OK) {
        c6_fail("el C6 no activó la imagen", e);
        goto out;
    }
    ESP_LOGW(TAG, "C6 update: %u bytes sent and accepted; restarting", (unsigned)s_c6up.sent);
    s_c6up.state = AOS_C6_DONE;
    fclose(f);
    heap_caps_free(buf);
    vTaskDelay(pdMS_TO_TICKS(3000));    /* the screen says so; an old C6 restarts itself meanwhile */
    esp_unregister_shutdown_handler((shutdown_handler_t)esp_wifi_stop);
    esp_restart();
out:
    (void)begun;
    if (f) fclose(f);
    heap_caps_free(buf);
    s_c6_busy = false;
    vTaskDeleteWithCaps(NULL);
}

bool aos_hal_net_coprocessor_update(const char *path)
{
    if (s_c6_busy || !s_inited || !path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    aos_c6_update_t u = { .state = AOS_C6_SENDING };
    bool ok = c6_image_read(f, u.version, sizeof u.version, &u.total);
    fclose(f);
    if (!ok) return false;
    snprintf(s_c6_path, sizeof s_c6_path, "%s", path);
    s_c6up = u;
    s_c6_busy = true;
    /* reads the card, never the flash: its stack can be in PSRAM */
    if (xTaskCreateWithCaps(c6_update_task, "c6-update", 4096, NULL, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_c6_busy = false;
        s_c6up.state = AOS_C6_IDLE;
        return false;
    }
    return true;
}

void aos_hal_net_coprocessor_status(aos_c6_update_t *out)
{
    if (out) *out = s_c6up;
}
const char *aos_hal_net_ip(void) { return s_ip; }
const char *aos_hal_net_ssid(void) { return s_ssid; }

/* The last reading, kept by the stats task (aos_tasks_p4.c, every 5 s). Not
 * an RPC here: the status bar asks from the shell's tick, under LVGL's lock,
 * and one RPC to the C6 that timed out there froze the screen and then
 * panicked inside esp_hosted (2026-09-29). */
static volatile int s_rssi;

void aos_net_p4_note_rssi(int rssi) { s_rssi = rssi; }

int aos_hal_net_rssi(void)
{
    return s_state == AOS_NET_CONNECTED ? s_rssi : 0;
}

void aos_hal_net_enable(bool on)
{
    aos_hal_pref_set_i32("net_on", on);
    s_want = on;
    if (!s_inited) return;
    if (on) {
        s_failures = 0;
        connect_now();
    } else {
        esp_timer_stop(s_retry);
        esp_wifi_disconnect();
        s_state = AOS_NET_OFF;
        s_ip[0] = 0;
    }
}

bool aos_hal_net_set_credentials(const char *ssid, const char *pass)
{
    size_t sl = ssid ? strlen(ssid) : 0, pl = pass ? strlen(pass) : 0;
    if (sl < 1 || sl > 32 || (pl && (pl < 8 || pl > 63))) return false;
    aos_hal_pref_set_str("wf_ssid", ssid);
    aos_hal_pref_set_str("wf_pass", pass ? pass : "");
    aos_hal_pref_set_i32("net_on", 1);
    s_want = true;
    s_failures = 0;
    if (s_inited) {
        esp_timer_stop(s_retry);
        esp_wifi_disconnect();
        connect_now();
    }
    return true;
}

void aos_hal_net_forget(void)
{
    aos_hal_pref_erase("wf_ssid");
    aos_hal_pref_erase("wf_pass");
    if (s_inited) {
        esp_timer_stop(s_retry);
        esp_wifi_disconnect();
    }
    s_state = AOS_NET_OFF;
    s_ssid[0] = s_ip[0] = 0;
}

void aos_hal_net_retry_info(uint32_t *failures, bool *parked, uint32_t *next_s, uint8_t *reason)
{
    if (failures) *failures = s_failures;
    if (parked) *parked = false;
    if (next_s) *next_s = s_next_s;
    if (reason) *reason = s_reason;
}

bool aos_hal_net_sync_time(void)
{
    if (s_state != AOS_NET_CONNECTED) return false;
    start_services();
    return true;
}

void aos_hal_net_low_latency(bool on)
{
    if (s_inited) esp_wifi_set_ps(on ? WIFI_PS_NONE : idle_ps());
}

static int ap_cmp(const void *a, const void *b)
{
    return ((const aos_wifi_ap_t *)b)->rssi - ((const aos_wifi_ap_t *)a)->rssi;
}

int aos_hal_net_scan(aos_wifi_ap_t *out, int max)
{
    if (!s_ev || !(xEventGroupWaitBits(s_ev, EV_INITED, false, true, pdMS_TO_TICKS(8000)) & EV_INITED)) return -1;
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -1;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (!n) return 0;
    wifi_ap_record_t *rec = calloc(n, sizeof *rec);
    if (!rec) { esp_wifi_clear_ap_list(); return -1; }
    esp_wifi_scan_get_ap_records(&n, rec);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (!rec[i].ssid[0]) continue;
        bool dup = false;       /* one row per network, however many access points it has */
        for (int j = 0; j < k && !dup; j++) dup = !strcmp(out[j].ssid, (const char *)rec[i].ssid);
        if (dup) continue;
        snprintf(out[k].ssid, sizeof out[k].ssid, "%s", (const char *)rec[i].ssid);
        out[k].rssi = rec[i].rssi;
        out[k].secure = rec[i].authmode != WIFI_AUTH_OPEN;
        k++;
    }
    free(rec);
    qsort(out, k, sizeof *out, ap_cmp);
    return k;
}

/* aos_device_name.c: a new name is announced at once */
void aos_hal_device_name_applied(const char *name)
{
    if (s_sta) esp_netif_set_hostname(s_sta, name);
    if (s_mdns) {
        mdns_hostname_set(name);
        mdns_instance_name_set(name);
    }
}

/* ---- The access point ----
 *
 * P4OS-XXXX (the last two bytes of the board's MAC), WPA2, its password made
 * up once from an alphabet with nothing to confuse (no 0/O, no 1/l/I) and
 * nothing to escape in a Wi-Fi QR, and kept in "ap_pass"; a new one only
 * when asked for (ap_set_config with an empty password). "ap_on" brings it
 * back after a restart: on a trip the board keeps being reachable.
 *
 * APSTA, not AP: the board stays on its home network when there is one, and
 * can scan for others. One radio, so the AP takes the STA's channel; when
 * the STA has none it is channel 1. The DHCP server is esp_netif's default
 * AP one (192.168.4.1, the phone gets .2 onwards), and mDNS answers there
 * too (CONFIG_MDNS_PREDEF_NETIF_AP).
 *
 * Everything that talks to the C6 runs in a task of its own: an RPC under
 * LVGL's lock that times out freezes the screen (see the RSSI above). */

static volatile bool s_ap_busy;
static char s_ap_ssid[33], s_ap_pass[65];

static const char AP_ALPHABET[] = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";

const char *aos_hal_net_ap_default_ssid(void)
{
    static char def[16];
    if (!def[0]) {
        uint8_t mac[6] = { 0 };
        esp_efuse_mac_get_default(mac);
        snprintf(def, sizeof def, "P4OS-%02X%02X", mac[4], mac[5]);
    }
    return def;
}

aos_ap_pass_mode_t aos_hal_net_ap_pass_mode(void) { return AOS_AP_PASS_FIXED; }

static void ap_resolve(void)
{
    if (!aos_hal_pref_get_str("ap_ssid", s_ap_ssid, sizeof s_ap_ssid) || !s_ap_ssid[0])
        snprintf(s_ap_ssid, sizeof s_ap_ssid, "%s", aos_hal_net_ap_default_ssid());
    if (!aos_hal_pref_get_str("ap_pass", s_ap_pass, sizeof s_ap_pass) || strlen(s_ap_pass) < 8) {
        for (int i = 0; i < 10; i++) s_ap_pass[i] = AP_ALPHABET[esp_random() % (sizeof AP_ALPHABET - 1)];
        s_ap_pass[10] = 0;
        aos_hal_pref_set_str("ap_pass", s_ap_pass);
    }
}

const char *aos_hal_net_ap_ssid(void) { if (!s_ap_ssid[0]) ap_resolve(); return s_ap_ssid; }
const char *aos_hal_net_ap_pass(void) { if (!s_ap_pass[0]) ap_resolve(); return s_ap_pass; }
const char *aos_hal_net_ap_ip(void) { return "192.168.4.1"; }
bool aos_hal_net_ap_active(void) { return s_ap_on; }
int aos_hal_net_ap_clients(void) { return s_ap_on ? s_ap_clients : 0; }

static void ap_task(void *arg)
{
    bool up = arg != NULL;
    if (!(xEventGroupWaitBits(s_ev, EV_INITED, false, true, pdMS_TO_TICKS(15000)) & EV_INITED)) {
        ESP_LOGW(TAG, "access point: the radio is not up");
        s_ap_busy = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    if (up) {
        /* At boot, with a home network to go to, wait for it a little: the
         * AP takes the STA's channel, and coming up on 1 first means moving
         * a few seconds later, which drops whoever has joined. */
        if (s_want && s_state != AOS_NET_CONNECTED && aos_hal_net_has_credentials() && esp_timer_get_time() < 30000000LL)
            xEventGroupWaitBits(s_ev, EV_GOT_IP, false, true, pdMS_TO_TICKS(12000));
        ap_resolve();
        if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
        wifi_config_t ap = { 0 };
        strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof ap.ap.ssid);
        ap.ap.ssid_len = strlen(s_ap_ssid);
        strlcpy((char *)ap.ap.password, s_ap_pass, sizeof ap.ap.password);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.max_connection = 4;
        ap.ap.pmf_cfg.capable = true;
        ap.ap.channel = 1;
        wifi_ap_record_t cur;
        if (s_state == AOS_NET_CONNECTED && esp_wifi_sta_get_ap_info(&cur) == ESP_OK && cur.primary) ap.ap.channel = cur.primary;
        esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &ap);
        if (e == ESP_OK) {
            s_ap_on = true;
            esp_timer_stop(s_retry);            /* see on_event: no looking while it is up */
            mdns_up();
            ESP_LOGI(TAG, "access point up: %s on channel %d -> http://192.168.4.1/", s_ap_ssid, ap.ap.channel);
        } else {
            ESP_LOGE(TAG, "access point: %s (does the C6's firmware do SoftAP?)", esp_err_to_name(e));
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
    } else {
        s_ap_on = false;
        s_ap_clients = 0;
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "access point down");
        if (s_want && s_state != AOS_NET_CONNECTED) { s_failures = 0; connect_now(); }
    }
    s_ap_busy = false;
    vTaskDeleteWithCaps(NULL);
}

static bool ap_spawn(bool up)
{
    if (s_ap_busy || !s_ev) return false;
    s_ap_busy = true;
    if (xTaskCreateWithCaps(ap_task, "net-ap", 4096, up ? (void *)1 : NULL, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_ap_busy = false;
        return false;
    }
    return true;
}

bool aos_hal_net_ap_start(void)
{
    aos_hal_pref_set_i32("ap_on", 1);
    return s_ap_on || ap_spawn(true);
}

void aos_hal_net_ap_stop(void)
{
    aos_hal_pref_set_i32("ap_on", 0);
    if (s_ap_on) ap_spawn(false);
}

bool aos_hal_net_ap_set_config(const char *ssid, const char *pass, aos_ap_pass_mode_t mode)
{
    (void)mode;
    size_t sl = ssid ? strlen(ssid) : 0, pl = pass ? strlen(pass) : 0;
    if (sl > 32 || (pl && (pl < 8 || pl > 63))) return false;
    aos_hal_pref_set_str("ap_ssid", ssid ? ssid : "");
    aos_hal_pref_set_str("ap_pass", pass ? pass : "");     /* empty: a new one is made up */
    s_ap_ssid[0] = s_ap_pass[0] = 0;
    ap_resolve();
    if (s_ap_on) ap_spawn(true);        /* the new name and password at once; whoever is on drops */
    return true;
}
