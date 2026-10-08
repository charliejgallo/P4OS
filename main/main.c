/*
 * P4OS - start on the Waveshare ESP32-P4-WIFI6-Touch-LCD-5.
 *
 * The HAL brings up the panel, LVGL (in its own task) and the touch; then the
 * apps are registered and the shell is built under the LVGL lock. The 5 Hz
 * tick the shell hands out to the apps runs in a thread of its own, with its
 * stack in PSRAM, and app_main returns: that gives back the main task's 16 KB
 * of internal RAM.
 */
#include <stdio.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"
#if CONFIG_HEAP_TRACING_STANDALONE
#include "esp_heap_caps.h"
#include "esp_heap_trace.h"
#endif

#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_apps.h"
#include "aos_portal.h"
#include "aos_dynapp.h"
#include "aos_i18n.h"
#include "xip_check.h"

static const char *TAG = "p4os";

/* A firmware installed over the network boots on trial (aos_ota_p4.c): it is
 * confirmed once the system has been up for this long, which a crash or a
 * hang at boot never reaches - the next restart then brings the previous
 * image back. Up means the tick running, which needs the UI and the card's
 * scan to have finished. */
#define OTA_TRIAL_MS 30000

/* Three times a restart by software left the board with no network until it
 * was power-cycled (2026-09-29/30): if a network is saved and switched on
 * and there is still no address this long after boot, the board says so in
 * the log and restarts - at most NET_RETRIES times in a row (the count
 * lives in PSRAM that a restart keeps; a power cut starts it over). The
 * previous boot's log (GET /api/log?prev=1) then shows what happened. */
#define NET_DEADLINE_MS 60000
#define NET_RETRIES     2
static EXT_RAM_NOINIT_ATTR struct { uint32_t magic, count; } s_net_boots;
#define NET_MAGIC 0x4E455442u

void aos_bt_p4_tick(void) __attribute__((weak));

/* The log so far, on the card: when the network does not come back the
 * portal cannot be reached to read it, and the cure that has worked (a
 * power cycle, or the board's reset button, which is one) wipes the PSRAM
 * where the previous boot's log lives. 2026-10-06: once in ~140 software
 * restarts the C6 came up with no Wi-Fi, and neither turning the Wi-Fi off
 * and on nor two more restarts brought it back. */
static void log_to_card(const char *why, unsigned n)
{
    const char *root = aos_hal_path_sd_root();
    if (!root) return;
    char path[96];
    snprintf(path, sizeof path, "%s/logs", root);
    mkdir(path, 0777);
    snprintf(path, sizeof path, "%s/logs/%s-%u.txt", root, why, n);
    FILE *f = fopen(path, "w");
    if (!f) return;
    static char buf[4096];
    size_t from = 0, next = 0;
    while (aos_hal_log_read(from, buf, sizeof buf, &next) > 0 && next > from) {
        fputs(buf, f);
        from = next;
    }
    fclose(f);
    ESP_LOGW(TAG, "the log is on the card: %s", path);
}

static void tick_thread(void *arg)
{
    (void)arg;
    bool trial = aos_hal_ota_pending_verify();
    bool net_checked = false;
    if (trial) ESP_LOGW(TAG, "this firmware is on trial: confirmed after %d s up", OTA_TRIAL_MS / 1000);
    bool up = false;
    /* a core dump nobody has read yet: its time is filled in once the clock
     * is set, and the check stops when it is read or erased */
    aos_coredump_info_t cd;
    bool cd_wait = aos_hal_coredump_info(&cd) && cd.present && cd.unread && !cd.seen;
    uint32_t ticks = 0;
    aos_hal_boot_stage("up");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        aos_hal_alive();                /* the hang watchdog's beat (aos_hal_p4.c) */
        if (cd_wait && ++ticks % 150 == 0) {        /* every 30 s */
            aos_hal_coredump_boot_check();
            cd_wait = aos_hal_coredump_info(&cd) && cd.present && cd.unread && !cd.seen;
        }
        if (trial && aos_hal_uptime_ms() > OTA_TRIAL_MS) {
            trial = false;
            aos_hal_ota_mark_valid();
        }
        if (!up && aos_hal_uptime_ms() > OTA_TRIAL_MS) {
            up = true;
            aos_hal_pref_set_i32("boot_bad", 0);    /* the safe boot's counter (aos_hal_p4.c) */
        }
        if (!net_checked && aos_hal_uptime_ms() > NET_DEADLINE_MS) {
            net_checked = true;
            if (s_net_boots.magic != NET_MAGIC) { s_net_boots.magic = NET_MAGIC; s_net_boots.count = 0; }
            bool want = aos_hal_net_enabled() && aos_hal_net_ssid()[0];
            if (!want || aos_hal_net_state() == AOS_NET_CONNECTED) {
                s_net_boots.count = 0;
            } else if (s_net_boots.count < NET_RETRIES) {
                s_net_boots.count++;
                log_to_card("sin-red", (unsigned)s_net_boots.count);
                ESP_LOGE(TAG, "no network %d s after boot (state %d): restarting, try %u of %d", NET_DEADLINE_MS / 1000,
                         (int)aos_hal_net_state(), (unsigned)s_net_boots.count, NET_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(200));
                aos_hal_reboot();
            } else {
                ESP_LOGE(TAG, "still no network after %d restarts: staying up without it", NET_RETRIES);
                log_to_card("sin-red", NET_RETRIES + 1);
            }
        }
        if (aos_hal_lock(0)) {
            aos_ui_tick();
            aos_apps_service_tick();
            aos_hal_unlock();
        }
        aos_dynapp_tick();      /* closes the .so files left unused */
        static int bt_ticks;
        if (aos_bt_p4_tick && ++bt_ticks % 15 == 0) aos_bt_p4_tick();    /* components/aos_ble */
    }
}

void app_main(void)
{
#if CONFIG_HEAP_TRACING_STANDALONE
    /* Diagnostic builds only (docs/MEMORY.md, "Internal RAM audit"): every
     * allocation from here on is recorded with its callers, freed ones
     * dropped, and GET /api/heap?trace=1 says who holds the internal RAM.
     * The records live in PSRAM. */
    {
        enum { TRACE_N = 8000 };
        heap_trace_record_t *rec = heap_caps_calloc(TRACE_N, sizeof *rec, MALLOC_CAP_SPIRAM);
        if (rec && heap_trace_init_standalone(rec, TRACE_N) == ESP_OK) heap_trace_start(HEAP_TRACE_LEAKS);
    }
#endif
    xip_check_run();                /* diagnostic builds only (xip_check.c) */
    aos_hal_init();
    xip_check_log();
    aos_hal_boot_stage("apps");
    aos_apps_register_builtin();
    if (aos_hal_lock(0)) {
        aos_ui_init();
        aos_ui_boot_show();         /* until the card's apps are in (aos_boot.c) */
        aos_hal_unlock();
    }
    aos_hal_boot_stage("portal");
    aos_portal_start(80);           /* serves as soon as the Wi-Fi is up */
    /* The apps on the card (.so files in /sdcard/apps): only their descriptors are
     * read now; the code is loaded when one is opened. */
    /* Safe mode: the BOOT button held while the boot screen shows (not at
     * power on: held at reset it sends the chip to the ROM's download
     * mode). The card's apps are not loaded, the tuning preferences are
     * forgotten and the USB port stays idle: a way back from an app or a
     * setting that keeps the board from being usable. Read before the scan
     * and after it, so any moment of the boot screen will do. */
    aos_hal_boot_stage("card's apps");
    /* or asked for from Settings, Developer: one boot only */
    int32_t safe_next = 0;
    aos_hal_pref_get_i32("safe_next", &safe_next);
    if (safe_next) aos_hal_pref_erase("safe_next");
    bool safe = safe_next || aos_hal_button_is_down(AOS_BUTTON_BOOT);
    int dyn = safe ? 0 : aos_dynapp_scan();
    if (!safe && aos_hal_button_is_down(AOS_BUTTON_BOOT)) {
        safe = true;
        static char ids[AOS_MAX_APPS][40];
        int n = 0;
        if (aos_hal_lock(2000)) {
            for (int i = 0; i < aos_ui_app_count() && n < AOS_MAX_APPS; i++) {
                const aos_app_t *a = aos_ui_app_at(i);
                if (a && a->desc.id && aos_dynapp_is_dynamic(a->desc.id)) snprintf(ids[n++], sizeof ids[0], "%s", a->desc.id);
            }
            aos_hal_unlock();
        }
        for (int i = 0; i < n; i++) aos_dynapp_unload(ids[i]);
        dyn = 0;
    }
    if (safe) {
        aos_ui_set_safe_mode(true);
        aos_hal_tune_reset();
        ESP_LOGW(TAG, "safe mode (%s): no apps from the card, tuning preferences forgotten, USB idle",
                 safe_next ? "asked for from Settings" : "BOOT held at boot");
    }
    ESP_LOGI(TAG, "%d app(s) on the card", dyn);
    if (aos_hal_lock(2000)) {
        aos_ui_boot_done();
        aos_hal_unlock();
    }
    aos_hal_boot_stage("usb");
    if (safe) aos_ui_request_toast(_("Modo seguro: sin las apps de la tarjeta. Reiniciá para volver a la normalidad."));
    /* the last crash's dump, if nobody has read it: said in the log, and once
     * on screen when it is new */
    aos_coredump_info_t cd0;
    bool cd_new = aos_hal_coredump_info(&cd0) && cd0.present && !cd0.unread;
    if (aos_hal_coredump_boot_check() && cd_new) {
        aos_ui_request_toast(_("La placa se cayó la vez anterior: el volcado está en el portal, en Registro."));
    }
    else aos_hal_usb_restore();     /* the USB port as it was before the restart; after the scan: never the disk */
    /* 16 KB, in PSRAM: the apps' service ticks run here, and 8 KB had 404
     * bytes left on the board with the music app open (2026-09-29). */
    if (!aos_hal_thread_start("tick", tick_thread, NULL, 16384, 1)) tick_thread(NULL);
    ESP_LOGI(TAG, "P4OS up");
}
