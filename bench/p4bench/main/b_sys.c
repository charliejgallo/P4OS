/*
 * System tests: identity (0), PSRAM bandwidth (2), temperature and load (20),
 * RTC retention (17), battery ADC (18), tasks, settings.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_console.h"
#include "esp_app_desc.h"
#include "esp_async_memcpy.h"
#include "esp_rtc_time.h"
#include "esp_private/esp_clk.h"
#include "hal/efuse_hal.h"
#include "driver/temperature_sensor.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "nvs_flash.h"
#include "bench.h"

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
    }
}

static int cmd_info(int argc, char **argv)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t rev = efuse_hal_chip_revision();
    uint32_t flash = 0, id = 0;
    esp_flash_get_size(NULL, &flash);
    esp_flash_read_id(NULL, &id);
    const esp_app_desc_t *app = esp_app_get_description();
    bench_report("info", "chip=esp32p4 rev=v%lu.%lu cores=%d cpu_mhz=%d flash_mb=%lu flash_id=0x%06lx "
                 "psram_mb=%u idf=%s build=%s_%s reset=%s rev_min_cfg=%d",
                 (unsigned long)(rev / 100), (unsigned long)(rev % 100), ci.cores,
                 esp_clk_cpu_freq() / 1000000, (unsigned long)(flash >> 20), (unsigned long)id,
                 (unsigned)(esp_psram_get_size() >> 20), app->idf_ver, app->date, app->time,
                 reset_name(esp_reset_reason()), CONFIG_ESP32P4_REV_MIN_FULL);
    bench_report("mem", "int_free=%u int_largest=%u psram_free=%u psram_largest=%u dma_free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    char st[160];
    snprintf(st, sizeof st, "v%lu.%lu  %d MHz  flash %lu MB  PSRAM %u MB  int %u K free",
             (unsigned long)(rev / 100), (unsigned long)(rev % 100), esp_clk_cpu_freq() / 1000000,
             (unsigned long)(flash >> 20), (unsigned)(esp_psram_get_size() >> 20),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
    lvport_ui_status(st);
    return 0;
}

static double mbps(size_t bytes, int64_t us) { return us > 0 ? bytes / (double)us : 0; }

static bool memcpy_done(async_memcpy_handle_t mc, async_memcpy_event_t *e, void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &woken);
    return woken == pdTRUE;
}

static int cmd_psram(int argc, char **argv)
{
    /* test 2: 4 MB blocks in PSRAM, 128 KB blocks for internal <-> PSRAM */
    const size_t big = 4 << 20, small = 128 << 10;
    uint8_t *a = heap_caps_aligned_alloc(128, big, MALLOC_CAP_SPIRAM);
    uint8_t *b = heap_caps_aligned_alloc(128, big, MALLOC_CAP_SPIRAM);
    uint8_t *s = heap_caps_aligned_alloc(128, small, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!a || !b || !s) {
        bench_report("psram", "error=nomem");
        goto out;
    }
    int64_t t;
    t = bench_us(); memset(a, 0x5A, big); double w = mbps(big, bench_us() - t);
    t = bench_us(); memcpy(b, a, big); double cp = mbps(big, bench_us() - t);
    t = bench_us();
    volatile uint32_t sum = 0;
    for (size_t i = 0; i < big / 4; i += 8) sum += ((uint32_t *)b)[i];   /* one read per 32 B */
    double rd = mbps(big, bench_us() - t);
    t = bench_us(); for (int k = 0; k < 32; k++) memcpy(s, a + k * small, small); double p2i = mbps(32 * small, bench_us() - t);
    t = bench_us(); for (int k = 0; k < 32; k++) memcpy(a + k * small, s, small); double i2p = mbps(32 * small, bench_us() - t);

    /* the AXI GDMA memcpy, the engine a worker can use without the CPU */
    double dma = -1;
    async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
    cfg.backlog = 16;
    async_memcpy_handle_t mc;
    if (esp_async_memcpy_install_gdma_axi(&cfg, &mc) == ESP_OK) {
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        t = bench_us();
        esp_async_memcpy(mc, b, a, big, memcpy_done, done);
        xSemaphoreTake(done, pdMS_TO_TICKS(2000));
        dma = mbps(big, bench_us() - t);
        vSemaphoreDelete(done);
        esp_async_memcpy_uninstall(mc);
    }
    bench_report("psram", "memset_MBps=%.0f memcpy_MBps=%.0f read_MBps=%.0f psram2int_MBps=%.0f int2psram_MBps=%.0f gdma_MBps=%.0f",
                 w, cp, rd, p2i, i2p, dma);
out:
    heap_caps_free(a); heap_caps_free(b); heap_caps_free(s);
    return 0;
}

static temperature_sensor_handle_t s_tsens;

static float chip_temp(void)
{
    if (!s_tsens) {
        temperature_sensor_config_t c = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&c, &s_tsens) != ESP_OK) return -999;
        temperature_sensor_enable(s_tsens);
    }
    float t = -999;
    temperature_sensor_get_celsius(s_tsens, &t);
    return t;
}

static int cmd_temp(int argc, char **argv)
{
    bench_report("temp", "chip_c=%.1f", chip_temp());
    return 0;
}

static volatile bool s_spin;
static void spin_task(void *arg)
{
    volatile uint32_t x = 1;
    while (s_spin) x = x * 1664525u + 1013904223u;
    vTaskDelete(NULL);
}

static int cmd_load(int argc, char **argv)
{
    /* test 20: both cores busy, for power and temperature readings */
    int secs = arg_int(argc, argv, 1, 30);
    float t0 = chip_temp();
    s_spin = true;
    xTaskCreatePinnedToCore(spin_task, "spin0", 2048, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(spin_task, "spin1", 2048, NULL, 1, NULL, 1);
    bench_say("both cores at 100%% for %d s - read the USB power meter now", secs);
    vTaskDelay(pdMS_TO_TICKS(secs * 1000));
    s_spin = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    bench_report("load", "secs=%d temp_before=%.1f temp_after=%.1f", secs, t0, chip_temp());
    return 0;
}

static int cmd_tasks(int argc, char **argv)
{
    static char buf[4096];
    vTaskList(buf);
    printf("Name          State Prio Stack Num Core\n%s\n", buf);
    vTaskGetRunTimeStats(buf);
    printf("Name          Time        %%\n%s\n", buf);
    return 0;
}

/* test 17: set the time, power the board off (button, then unplugged),
 * power it on and check. If the P4's RTC domain lives on VBAT (H3) the
 * time survives; without a cell it should not survive unplugging. */
static int cmd_rtc(int argc, char **argv)
{
    const char *sub = arg_str(argc, argv, 1, "check");
    if (!strcmp(sub, "set")) {
        long epoch = arg_int(argc, argv, 2, 0);
        if (epoch < 1700000000) { printf("rtc set <unix epoch>   (python3 -c 'import time;print(int(time.time()))')\n"); return 0; }
        struct timeval tv = { .tv_sec = epoch };
        settimeofday(&tv, NULL);
        bench_cfg_set("rtc_set", (int32_t)epoch);
        bench_report("rtc.set", "epoch=%ld", epoch);
        return 0;
    }
    time_t now = time(NULL);
    bench_report("rtc.check", "epoch=%lld set_at=%ld rtc_us=%llu uptime_s=%lld valid=%d",
                 (long long)now, (long)bench_cfg_get("rtc_set", 0), (unsigned long long)esp_rtc_get_time_us(),
                 bench_us() / 1000000, now > 1700000000);
    return 0;
}

/* test 18: battery sense on GPIO20 through a 200K/100K divider */
static int cmd_bat(int argc, char **argv)
{
    adc_unit_t unit;
    adc_channel_t ch;
    if (adc_oneshot_io_to_channel(20, &unit, &ch) != ESP_OK) { bench_report("bat", "error=no_adc_channel"); return 0; }
    adc_oneshot_unit_handle_t h;
    adc_oneshot_unit_init_cfg_t uc = { .unit_id = unit };
    if (adc_oneshot_new_unit(&uc, &h) != ESP_OK) { bench_report("bat", "error=unit"); return 0; }
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(h, ch, &cc);
    adc_cali_handle_t cali = NULL;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cf = { .unit_id = unit, .chan = ch, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_cali_create_scheme_curve_fitting(&cf, &cali);
#endif
    int raw_sum = 0, mv_sum = 0, n = 32;
    for (int i = 0; i < n; i++) {
        int raw = 0, mv = 0;
        adc_oneshot_read(h, ch, &raw);
        raw_sum += raw;
        if (cali) { adc_cali_raw_to_voltage(cali, raw, &mv); mv_sum += mv; }
    }
    int mv = cali ? mv_sum / n : -1;
    bench_report("bat", "adc_unit=%d ch=%d raw=%d pin_mv=%d battery_mv=%d calibrated=%d",
                 unit + 1, ch, raw_sum / n, mv, mv >= 0 ? mv * 3 : -1, cali != NULL);
    adc_oneshot_del_unit(h);
    return 0;
}

static int cmd_reboot(int argc, char **argv) { esp_restart(); return 0; }

static int cmd_cfg(int argc, char **argv)
{
    const char *sub = arg_str(argc, argv, 1, "list");
    if (!strcmp(sub, "erase")) {
        nvs_handle_t h;
        if (nvs_open("p4bench", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
        bench_say("settings erased");
        return 0;
    }
    if (!strcmp(sub, "set") && argc >= 4) {
        bench_cfg_set(argv[2], atoi(argv[3]));
        bench_say("%s=%s", argv[2], argv[3]);
        return 0;
    }
    nvs_iterator_t it = NULL;
    esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, "p4bench", NVS_TYPE_ANY, &it);
    while (e == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (info.type == NVS_TYPE_I32) printf("%s = %ld\n", info.key, (long)bench_cfg_get(info.key, 0));
        else printf("%s = (string)\n", info.key);
        e = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return 0;
}

void reg_sys(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "info", .help = "chip, memory, versions (test 0)", .func = cmd_info },
        { .command = "psram", .help = "PSRAM / internal / GDMA bandwidth (test 2)", .func = cmd_psram },
        { .command = "temp", .help = "chip temperature", .func = cmd_temp },
        { .command = "load", .help = "load [secs]: both cores at 100% (test 20)", .func = cmd_load },
        { .command = "tasks", .help = "task list and CPU time", .func = cmd_tasks },
        { .command = "rtc", .help = "rtc set <epoch> | rtc check (test 17)", .func = cmd_rtc },
        { .command = "bat", .help = "battery voltage on GPIO20 (test 18)", .func = cmd_bat },
        { .command = "cfg", .help = "cfg [list|set k v|erase]", .func = cmd_cfg },
        { .command = "reboot", .help = "restart", .func = cmd_reboot },
    };
    for (int i = 0; i < sizeof cmds / sizeof cmds[0]; i++) esp_console_cmd_register(&cmds[i]);
}
