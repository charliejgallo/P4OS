/*
 * Display bring-up and raw frame-buffer tests (HARDWARE.md tests 3, 4).
 *
 * The panel is created here and not through bsp_display_start(), for the same
 * reason AmoledOS drives its own display: we want the frame buffers, the
 * DMA2D flag and the events in our hands. The init sequence is the BSP's
 * (bsp_display_new_with_handles) with the number of frame buffers taken
 * from the "fbs" setting.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_hx8394.h"
#include "driver/ppa.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bench.h"

static const char *TAG = "disp";

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb[3];
static int s_nfb;
static int s_front;
static volatile uint32_t s_frames;
static SemaphoreHandle_t s_frame_sem;

/* draw_bitmap with DMA2D returns before the copy ends: this says when it did */
static void (*volatile s_trans_done)(void);

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *e, void *ctx)
{
    if (s_trans_done) s_trans_done();
    return false;
}

static bool IRAM_ATTR on_frame(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *e, void *ctx)
{
    s_frames++;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_sem, &woken);
    return woken == pdTRUE;
}

esp_err_t disp_init(void)
{
    int nfb = bench_cfg_get("fbs", 2);
    if (nfb < 1 || nfb > 3) nfb = 2;

    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "backlight");

    static esp_ldo_channel_handle_t phy_ldo;
    esp_ldo_channel_config_t ldo = { .chan_id = 3, .voltage_mv = 2500 };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo, &phy_ldo), TAG, "DSI PHY LDO");

    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = 0,
        .lane_bit_rate_mbps = 700,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "DSI bus");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi, &io), TAG, "DBI io");

    esp_lcd_dpi_panel_config_t dpi = HX8394_720_1280_PANEL_30HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi.num_fbs = nfb;
    dpi.flags.use_dma2d = true;
    hx8394_vendor_config_t vendor = {
        .mipi_config = { .dsi_bus = bus, .dpi_config = &dpi, .lane_num = 2 },
    };
    esp_lcd_panel_dev_config_t dev = {
        .bits_per_pixel = 16,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .reset_gpio_num = BSP_LCD_RST,
        .vendor_config = &vendor,
        .flags.reset_active_high = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_hx8394(io, &dev, &s_panel), TAG, "HX8394");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");

    void *fb[3] = { 0 };
    if (nfb == 1) esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb[0]);
    else if (nfb == 2) esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &fb[0], &fb[1]);
    else esp_lcd_dpi_panel_get_frame_buffer(s_panel, 3, &fb[0], &fb[1], &fb[2]);
    for (int i = 0; i < nfb; i++) s_fb[i] = fb[i];
    s_nfb = nfb;

    s_frame_sem = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_frame_buf_complete = on_frame, .on_color_trans_done = on_trans_done };
    esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL);

    for (int i = 0; i < nfb; i++) {
        memset(s_fb[i], 0, LCD_FB_BYTES);
        disp_cache_flush(s_fb[i], LCD_FB_BYTES);
    }
    esp_lcd_panel_disp_on_off(s_panel, true);
    disp_backlight(bench_cfg_get("bl", 80));
    ESP_LOGI(TAG, "panel up: %d frame buffer(s) at %p %p %p", nfb, s_fb[0], s_fb[1], s_fb[2]);
    return ESP_OK;
}

esp_lcd_panel_handle_t disp_panel(void) { return s_panel; }
void disp_set_trans_done(void (*cb)(void)) { s_trans_done = cb; }
int disp_num_fbs(void) { return s_nfb; }
uint16_t *disp_fb(int i) { return (i >= 0 && i < s_nfb) ? s_fb[i] : NULL; }
int disp_front(void) { return s_front; }
uint32_t disp_frame_count(void) { return s_frames; }

void disp_cache_flush(const void *p, size_t len)
{
    /* The frame buffers are cache-line aligned; partial ranges are not. */
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)127;
    uintptr_t b = ((uintptr_t)p + len + 127) & ~(uintptr_t)127;
    esp_cache_msync((void *)a, b - a, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void disp_show_fb(int i)
{
    if (i < 0 || i >= s_nfb) return;
    /* Handing the driver one of its own frame buffers switches the scan-out
     * to it instead of copying. */
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb[i]);
    s_front = i;
}

bool disp_wait_frame(int timeout_ms)
{
    xSemaphoreTake(s_frame_sem, 0);
    return xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void disp_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    bsp_display_brightness_set(percent);
}

/* ------------------------------------------------------------------ */

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h && j < LCD_H; j++)
        for (int i = x; i < x + w && i < LCD_W; i++) fb[j * LCD_W + i] = c;
}

/* Colour bars, a 1 px white border, corner markers and a gradient: shows
 * the colour order, the edges and the orientation (red corner = top-left of
 * the native portrait frame). */
static void draw_bars(uint16_t *fb)
{
    static const uint16_t bars[8] = { 0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000 };
    for (int b = 0; b < 8; b++) fill_rect(fb, b * LCD_W / 8, 0, LCD_W / 8, LCD_H / 2, bars[b]);
    for (int y = LCD_H / 2; y < LCD_H * 3 / 4; y++)
        for (int x = 0; x < LCD_W; x++) {
            uint8_t v = x * 255 / (LCD_W - 1);
            fb[y * LCD_W + x] = rgb(v, v, v);
        }
    for (int y = LCD_H * 3 / 4; y < LCD_H; y++)
        for (int x = 0; x < LCD_W; x++) {
            uint8_t v = x * 255 / (LCD_W - 1);
            int band = (y - LCD_H * 3 / 4) * 3 / (LCD_H / 4);
            fb[y * LCD_W + x] = band == 0 ? rgb(v, 0, 0) : band == 1 ? rgb(0, v, 0) : rgb(0, 0, v);
        }
    for (int x = 0; x < LCD_W; x++) { fb[x] = 0xFFFF; fb[(LCD_H - 1) * LCD_W + x] = 0xFFFF; }
    for (int y = 0; y < LCD_H; y++) { fb[y * LCD_W] = 0xFFFF; fb[y * LCD_W + LCD_W - 1] = 0xFFFF; }
    fill_rect(fb, 8, 8, 64, 64, 0xF800);                     /* top-left: red */
    fill_rect(fb, LCD_W - 72, 8, 64, 64, 0x07E0);            /* top-right: green */
    fill_rect(fb, 8, LCD_H - 72, 64, 64, 0x001F);            /* bottom-left: blue */
    fill_rect(fb, LCD_W - 72, LCD_H - 72, 64, 64, 0xFFFF);   /* bottom-right: white */
}

static int raw_begin(void)
{
    lvport_pause(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    return disp_front();
}

static void raw_end(int hold_ms)
{
    if (hold_ms > 0) vTaskDelay(pdMS_TO_TICKS(hold_ms));
    lvport_pause(false);
}

static int cmd_lcd(int argc, char **argv)
{
    const char *sub = arg_str(argc, argv, 1, "help");

    if (!strcmp(sub, "info")) {
        bench_report("lcd.info", "w=%d h=%d fbs=%d fb0=%p fb1=%p fb2=%p dma2d=1",
                     LCD_W, LCD_H, s_nfb, s_fb[0], s_fb[1], s_fb[2]);
        return 0;
    }

    if (!strcmp(sub, "rate")) {
        /* test 3: real refresh rate from the frame-done events */
        uint32_t f0 = s_frames;
        int64_t t0 = bench_us();
        vTaskDelay(pdMS_TO_TICKS(3000));
        uint32_t f = s_frames - f0;
        double s = (bench_us() - t0) / 1e6;
        bench_report("lcd.rate", "frames=%lu hz=%.2f", (unsigned long)f, f / s);
        return 0;
    }

    if (!strcmp(sub, "bars") || !strcmp(sub, "pattern")) {
        int hold = arg_int(argc, argv, 2, 5000);
        raw_begin();
        if (!strcmp(sub, "pattern")) {
            /* the DSI host's own colour bars: proves the link without any frame buffer */
            esp_lcd_dpi_panel_set_pattern(s_panel, MIPI_DSI_PATTERN_BAR_VERTICAL);
            bench_say("DSI built-in vertical bars for %d ms", hold);
            vTaskDelay(pdMS_TO_TICKS(hold));
            esp_lcd_dpi_panel_set_pattern(s_panel, MIPI_DSI_PATTERN_NONE);
            raw_end(0);
        } else {
            int f = s_front;
            draw_bars(s_fb[f]);
            disp_cache_flush(s_fb[f], LCD_FB_BYTES);
            bench_say("bars: red corner should be top-left of the portrait frame");
            raw_end(hold);
        }
        bench_report("lcd.bars", "shown_ms=%d", hold);
        return 0;
    }

    if (!strcmp(sub, "fill")) {
        /* test 3: cost of one full frame by CPU, by PPA fill and by DMA2D copy */
        raw_begin();
        uint16_t *fb = s_fb[s_front];
        int64_t t;

        t = bench_us();
        for (int k = 0; k < 10; k++) {
            uint32_t v = (k & 1) ? 0x001F001F : 0xF800F800;
            uint32_t *p = (uint32_t *)fb;
            for (int i = 0; i < LCD_FB_BYTES / 4; i++) p[i] = v;
            disp_cache_flush(fb, LCD_FB_BYTES);
        }
        double cpu_ms = (bench_us() - t) / 10000.0;

        t = bench_us();
        for (int k = 0; k < 10; k++) {
            memset(fb, (k & 1) ? 0x00 : 0xFF, LCD_FB_BYTES);
            disp_cache_flush(fb, LCD_FB_BYTES);
        }
        double memset_ms = (bench_us() - t) / 10000.0;

        ppa_client_handle_t fill;
        ppa_client_config_t pc = { .oper_type = PPA_OPERATION_FILL, .max_pending_trans_num = 1 };
        double ppa_ms = -1;
        if (ppa_register_client(&pc, &fill) == ESP_OK) {
            t = bench_us();
            for (int k = 0; k < 10; k++) {
                ppa_fill_oper_config_t op = {
                    .out = { .buffer = fb, .buffer_size = LCD_FB_BYTES, .pic_w = LCD_W, .pic_h = LCD_H,
                             .fill_cm = PPA_FILL_COLOR_MODE_RGB565 },
                    .fill_block_w = LCD_W, .fill_block_h = LCD_H,
                    .fill_argb_color = { .val = (k & 1) ? 0xFF00FF00 : 0xFF0000FF },
                    .mode = PPA_TRANS_MODE_BLOCKING,
                };
                ppa_do_fill(fill, &op);
            }
            ppa_ms = (bench_us() - t) / 10000.0;
            ppa_unregister_client(fill);
        }

        /* DMA2D: a PSRAM buffer that is not a frame buffer gets copied into
         * the current frame buffer by the driver's DMA2D path. */
        double dma2d_ms = -1;
        uint16_t *src = heap_caps_aligned_alloc(128, LCD_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (src) {
            draw_bars(src);
            disp_cache_flush(src, LCD_FB_BYTES);
            t = bench_us();
            for (int k = 0; k < 10; k++) esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, src);
            dma2d_ms = (bench_us() - t) / 10000.0;
            vTaskDelay(pdMS_TO_TICKS(100));
            heap_caps_free(src);
        }
        double mb = LCD_FB_BYTES / 1048576.0;
        bench_report("lcd.fill", "cpu_ms=%.2f memset_ms=%.2f ppa_ms=%.2f dma2d_ms=%.2f "
                     "cpu_fps=%.1f ppa_fps=%.1f dma2d_fps=%.1f cpu_MBps=%.0f",
                     cpu_ms, memset_ms, ppa_ms, dma2d_ms, 1000 / cpu_ms,
                     ppa_ms > 0 ? 1000 / ppa_ms : 0, dma2d_ms > 0 ? 1000 / dma2d_ms : 0, mb * 1000 / cpu_ms);
        raw_end(500);
        return 0;
    }

    if (!strcmp(sub, "tear")) {
        /* test 3: a vertical bar sweeping sideways. Single buffer writes into
         * the frame being scanned (tearing expected); double buffer draws in
         * the back one and flips on the frame event. */
        int secs = arg_int(argc, argv, 2, 4);
        raw_begin();
        for (int mode = 0; mode < (s_nfb > 1 ? 2 : 1); mode++) {
            bench_say(mode == 0 ? "tear: single buffer (look for a broken bar)" : "tear: double buffer + flip");
            int64_t end = bench_us() + secs * 1000000LL;
            int x = 0, frames = 0;
            while (bench_us() < end) {
                int target = mode == 0 ? s_front : (s_front + 1) % s_nfb;
                uint16_t *fb = s_fb[target];
                memset(fb, 0, LCD_FB_BYTES);
                fill_rect(fb, x, 0, 60, LCD_H, 0xFFFF);
                disp_cache_flush(fb, LCD_FB_BYTES);
                if (mode == 1) { disp_show_fb(target); disp_wait_frame(100); }
                x = (x + 24) % (LCD_W - 60);
                frames++;
            }
            bench_report("lcd.tear", "mode=%s fps=%.1f", mode == 0 ? "single" : "double", frames / (double)secs);
        }
        raw_end(0);
        return 0;
    }

    if (!strcmp(sub, "bl")) {
        int p = arg_int(argc, argv, 2, 80);
        disp_backlight(p);
        bench_cfg_set("bl", p);
        bench_report("lcd.bl", "percent=%d", p);
        return 0;
    }

    if (!strcmp(sub, "blsweep")) {
        /* test 4: find the lowest visible step and see if the curve is linear */
        raw_begin();
        draw_bars(s_fb[s_front]);
        disp_cache_flush(s_fb[s_front], LCD_FB_BYTES);
        static const int steps[] = { 0, 1, 2, 3, 5, 8, 10, 15, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
        for (int i = 0; i < sizeof steps / sizeof steps[0]; i++) {
            disp_backlight(steps[i]);
            bench_say("backlight %d %%", steps[i]);
            vTaskDelay(pdMS_TO_TICKS(1200));
        }
        disp_backlight(bench_cfg_get("bl", 80));
        bench_report("lcd.blsweep", "done=1");
        raw_end(0);
        return 0;
    }

    if (!strcmp(sub, "off") || !strcmp(sub, "on")) {
        bool on = !strcmp(sub, "on");
        /* GPIO33 is the boost enable (pulled up to 5 V through 100K on the board) */
        if (on) bsp_display_backlight_on(); else bsp_display_backlight_off();
        esp_lcd_panel_disp_on_off(s_panel, on);
        bench_report("lcd.power", "on=%d", on);
        return 0;
    }

    if (!strcmp(sub, "fbs")) {
        int n = arg_int(argc, argv, 2, 2);
        bench_cfg_set("fbs", n);
        bench_say("frame buffers = %d after reboot", n);
        return 0;
    }

    printf("lcd info|rate|bars [ms]|pattern [ms]|fill|tear [s]|bl <pct>|blsweep|on|off|fbs <1-3>\n");
    return 0;
}

void reg_display(void)
{
    const esp_console_cmd_t cmd = { .command = "lcd", .help = "display tests (info rate bars pattern fill tear bl blsweep on off fbs)", .func = cmd_lcd };
    esp_console_cmd_register(&cmd);
}
