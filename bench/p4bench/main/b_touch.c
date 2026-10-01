/*
 * Touch tests (HARDWARE.md test 7): which address the GT911 answers on, its
 * firmware and configured report rate, whether GPIO2 carries its INT
 * (R108 fitted or not), whether GPIO23 really resets it, the real rate of
 * new samples, and edge coverage with a raw paint view.
 *
 * The lesson from the CST820 audit in AmoledOS: look at the raw coordinates
 * first, calibrate later (the calibration there manufactured dead bands).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"
#include "bsp/esp-bsp.h"
#include "bench.h"

#define GT_STATUS 0x814E
#define GT_POINTS 0x814F
#define GT_PRODUCT 0x8140
#define GT_CFG_VER 0x8047
#define GT_REFRESH 0x8056

static i2c_master_dev_handle_t gt_dev(uint8_t *addr_out)
{
    static i2c_master_dev_handle_t dev;
    static uint8_t addr;
    if (dev) { if (addr_out) *addr_out = addr; return dev; }
    bsp_i2c_init();
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (i2c_master_probe(bus, 0x5D, 50) == ESP_OK) addr = 0x5D;
    else if (i2c_master_probe(bus, 0x14, 50) == ESP_OK) addr = 0x14;
    else return NULL;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) return NULL;
    if (addr_out) *addr_out = addr;
    return dev;
}

static esp_err_t gt_read(uint16_t reg, uint8_t *buf, size_t len)
{
    i2c_master_dev_handle_t d = gt_dev(NULL);
    if (!d) return ESP_ERR_NOT_FOUND;
    uint8_t r[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(d, r, 2, buf, len, 50);
}

static esp_err_t gt_write8(uint16_t reg, uint8_t v)
{
    i2c_master_dev_handle_t d = gt_dev(NULL);
    if (!d) return ESP_ERR_NOT_FOUND;
    uint8_t w[3] = { reg >> 8, reg & 0xFF, v };
    return i2c_master_transmit(d, w, 3, 50);
}

static inline void px(uint16_t *fb, int x, int y, uint16_t c)
{
    if (x >= 0 && x < LCD_W && y >= 0 && y < LCD_H) fb[y * LCD_W + x] = c;
}

static int cmd_touch(int argc, char **argv)
{
    const char *sub = arg_str(argc, argv, 1, "info");

    if (!strcmp(sub, "info")) {
        uint8_t addr = 0;
        if (!gt_dev(&addr)) { bench_report("touch.info", "found=0"); return 0; }
        uint8_t id[11] = { 0 }, ver = 0, rate = 0;
        gt_read(GT_PRODUCT, id, 11);   /* "911" + fw (2) + x max (2) + y max (2) + vendor */
        gt_read(GT_CFG_VER, &ver, 1);
        gt_read(GT_REFRESH, &rate, 1);
        bench_report("touch.info", "found=1 addr=0x%02X product=%.4s fw=0x%02X%02X xmax=%d ymax=%d vendor=0x%02X "
                     "cfg_ver=0x%02X refresh_ms=%d",
                     addr, (char *)id, id[5], id[4], id[6] | (id[7] << 8), id[8] | (id[9] << 8), id[10],
                     ver, 5 + (rate & 0x0F));
        return 0;
    }

    if (!strcmp(sub, "int")) {
        /* GPIO2 only carries INT if R108 is fitted. The GT911 pulses INT on
         * every new report, so edges while touching = wired. */
        int secs = arg_int(argc, argv, 2, 6);
        gpio_config_t io = { .pin_bit_mask = 1ULL << 2, .mode = GPIO_MODE_INPUT, .pull_up_en = 1 };
        gpio_config(&io);
        bench_say("touch int: keep a finger moving on the screen for %d s", secs);
        int edges = 0, last = gpio_get_level(2);
        int64_t end = bench_us() + secs * 1000000LL;
        while (bench_us() < end) {
            int l = gpio_get_level(2);
            if (l != last) { edges++; last = l; }
            esp_rom_delay_us(200);
        }
        gpio_reset_pin(2);
        bench_report("touch.int", "gpio=2 edges=%d wired=%d", edges, edges > 4);
        return 0;
    }

    if (!strcmp(sub, "rst")) {
        /* The BSP says the touch reset is NC; the schematic shows GPIO23 via
         * R37. Pull it low and see if the chip drops off the bus. */
        gpio_config_t io = { .pin_bit_mask = 1ULL << 23, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
        gpio_set_level(23, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        bsp_i2c_init();
        i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
        bool gone = i2c_master_probe(bus, 0x5D, 50) != ESP_OK && i2c_master_probe(bus, 0x14, 50) != ESP_OK;
        gpio_set_level(23, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        bool back = i2c_master_probe(bus, 0x5D, 50) == ESP_OK || i2c_master_probe(bus, 0x14, 50) == ESP_OK;
        gpio_reset_pin(23);
        bench_report("touch.rst", "gpio=23 held_low_gone=%d back_after_release=%d reset_wired=%d", gone, back, gone && back);
        return 0;
    }

    if (!strcmp(sub, "rate")) {
        /* Count "buffer ready" reports per second straight from the status
         * register, with LVGL kept off the chip. */
        int secs = arg_int(argc, argv, 2, 5);
        lvport_touch_hold(true);
        bench_say("touch rate: keep a finger moving for %d s", secs);
        int reports = 0, polls = 0, max_pts = 0;
        int64_t end = bench_us() + secs * 1000000LL;
        while (bench_us() < end) {
            uint8_t st = 0;
            if (gt_read(GT_STATUS, &st, 1) == ESP_OK) {
                polls++;
                if (st & 0x80) {
                    reports++;
                    if ((st & 0x0F) > max_pts) max_pts = st & 0x0F;
                    gt_write8(GT_STATUS, 0);
                }
            }
            esp_rom_delay_us(500);
        }
        lvport_touch_hold(false);
        bench_report("touch.rate", "reports_per_s=%.1f polls_per_s=%.0f max_points=%d",
                     reports / (double)secs, polls / (double)secs, max_pts);
        return 0;
    }

    if (!strcmp(sub, "paint")) {
        /* Raw coordinates painted in the native frame, five colours for five
         * fingers. Run a finger along all four edges: the min/max tell how
         * close to the bezel the panel reports. */
        int secs = arg_int(argc, argv, 2, 20);
        esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)lvport_touch();
        if (!tp) { bench_report("touch.paint", "error=no_touch"); return 0; }
        static const uint16_t col[5] = { 0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F };
        lvport_pause(true);
        lvport_touch_hold(true);
        uint16_t *fb = disp_fb(disp_front());
        memset(fb, 0, LCD_FB_BYTES);
        for (int x = 0; x < LCD_W; x += 60) for (int y = 0; y < LCD_H; y++) px(fb, x, y, 0x2104);
        for (int y = 0; y < LCD_H; y += 60) for (int x = 0; x < LCD_W; x++) px(fb, x, y, 0x2104);
        disp_cache_flush(fb, LCD_FB_BYTES);
        bench_say("touch paint %d s: trace the four edges, then try 5 fingers", secs);
        int minx = 9999, miny = 9999, maxx = -1, maxy = -1, maxn = 0, samples = 0;
        int64_t end = bench_us() + secs * 1000000LL;
        while (bench_us() < end) {
            esp_lcd_touch_read_data(tp);
            esp_lcd_touch_point_data_t p[5];
            uint8_t n = 0;
            esp_lcd_touch_get_data(tp, p, &n, 5);
            if (n > maxn) maxn = n;
            for (int i = 0; i < n; i++) {
                samples++;
                if (p[i].x < minx) minx = p[i].x;
                if (p[i].y < miny) miny = p[i].y;
                if (p[i].x > maxx) maxx = p[i].x;
                if (p[i].y > maxy) maxy = p[i].y;
                for (int dy = -3; dy <= 3; dy++)
                    for (int dx = -3; dx <= 3; dx++) px(fb, p[i].x + dx, p[i].y + dy, col[i % 5]);
                int y0 = p[i].y - 3 < 0 ? 0 : p[i].y - 3;
                disp_cache_flush(fb + y0 * LCD_W, 7 * LCD_W * 2);
            }
            vTaskDelay(pdMS_TO_TICKS(8));
        }
        lvport_touch_hold(false);
        bench_report("touch.paint", "samples=%d max_points=%d min_x=%d max_x=%d min_y=%d max_y=%d",
                     samples, maxn, minx, maxx, miny, maxy);
        lvport_pause(false);
        return 0;
    }

    printf("touch info|int [s]|rst|rate [s]|paint [s]\n");
    return 0;
}

void reg_touch(void)
{
    const esp_console_cmd_t cmd = { .command = "touch", .help = "GT911 tests (info int rst rate paint)", .func = cmd_touch };
    esp_console_cmd_register(&cmd);
}
