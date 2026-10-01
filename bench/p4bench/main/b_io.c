/*
 * Rear header tests (HARDWARE.md test 15, EXPANSION.md).
 *
 *   gpio map            the header as p4bench believes it is
 *   gpio watch [s]      every usable header GPIO as input with pull-up;
 *                       touch each pin with a wire to GND and the bench
 *                       prints which GPIO / physical pin changed. This is
 *                       how the pin map gets verified by hand.
 *   gpio pairs          jumpers across each row (11-12, 15-16, 21-22,
 *                       23-24, 31-32, 35-36, 37-38): drives one side, reads
 *                       the other, both ways.
 *   gpio loop <a> <b>   the same for any two GPIOs
 *   gpio set <g> <0|1>  drive one pin (for the multimeter)
 *   gpio vo4            GPIO46-48 high: measure pins 35/37/39 (VO4 domain)
 *   uart loop <tx> <rx> loopback from 115200 up to 5 Mbaud
 *   i2c scan [sda scl]  board bus (GPIO7/8) or a second bus on any pins
 *   mb ...              one Modbus RTU read over a UART (RS485 board)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"
#include "bench.h"

typedef struct {
    int8_t gpio;
    uint8_t pin;
    const char *note;
    bool watch;      /* safe to turn into an input with pull-up */
} hdr_t;

/* J3, from the schematic and the Waveshare docs. Pin 1 = 5V, pin 2 = 3V3. */
static const hdr_t HDR[] = {
    { 7, 4, "I2C SDA (board bus)", false },   { 8, 6, "I2C SCL (board bus)", false },
    { 37, 7, "UART0 TX (console)", false },   { 2, 8, "free (touch INT if R108)", true },
    { 38, 9, "UART0 RX (console)", false },   { 5, 11, "free (JTAG MTDO)", true },
    { 3, 12, "free (JTAG MTCK)", true },      { 4, 14, "free (JTAG MTDI)", true },
    { 21, 15, "free ADC1_CH5", true },        { 28, 16, "free", true },
    { 22, 17, "free ADC1_CH6", true },        { 29, 20, "free", true },
    { 24, 21, "USB-JTAG D- (free if unused)", true }, { 30, 22, "free", true },
    { 25, 23, "USB-JTAG D+ (free if unused)", true }, { 31, 24, "free", true },
    { 34, 28, "strapping JTAG sel", true },   { 35, 30, "BOOT (strapping)", false },
    { 32, 31, "free", true },                 { 49, 32, "free ADC2_CH0", true },
    { 50, 34, "free ADC2_CH1", true },        { 46, 35, "VO4 domain", true },
    { 51, 36, "free ADC2_CH2", true },        { 47, 37, "VO4 domain", true },
    { 52, 38, "free ADC2_CH3", true },        { 48, 39, "VO4 domain", true },
};
#define NHDR (sizeof HDR / sizeof HDR[0])

static int pin_of(int gpio)
{
    for (int i = 0; i < NHDR; i++) if (HDR[i].gpio == gpio) return HDR[i].pin;
    return -1;
}

static bool loop_pair(int a, int b)
{
    bool ok = true;
    for (int dir = 0; dir < 2; dir++) {
        int out = dir ? b : a, in = dir ? a : b;
        gpio_reset_pin(out);
        gpio_reset_pin(in);
        gpio_set_direction(out, GPIO_MODE_OUTPUT);
        gpio_set_direction(in, GPIO_MODE_INPUT);
        for (int pull = 0; pull < 2; pull++) {
            /* the pull fights the driver: a real connection wins */
            gpio_set_pull_mode(in, pull ? GPIO_PULLDOWN_ONLY : GPIO_PULLUP_ONLY);
            for (int v = 0; v < 2; v++) {
                gpio_set_level(out, v);
                esp_rom_delay_us(50);
                if (gpio_get_level(in) != v) ok = false;
            }
        }
        gpio_reset_pin(out);
        gpio_reset_pin(in);
    }
    return ok;
}

static int cmd_gpio(int argc, char **argv)
{
    const char *sub = arg_str(argc, argv, 1, "map");

    if (!strcmp(sub, "map")) {
        printf("pin gpio  note\n");
        for (int i = 0; i < NHDR; i++) printf("%3d  %2d   %s\n", HDR[i].pin, HDR[i].gpio, HDR[i].note);
        printf("5V: pins 1,3   3V3: pins 2,18   GND: 5,10,13,19,26,29,33,40   USB HS D-/D+: 25/27\n");
        return 0;
    }

    if (!strcmp(sub, "watch")) {
        int secs = arg_int(argc, argv, 2, 60);
        int lvl[NHDR];
        for (int i = 0; i < NHDR; i++) {
            if (!HDR[i].watch) continue;
            gpio_reset_pin(HDR[i].gpio);
            gpio_set_direction(HDR[i].gpio, GPIO_MODE_INPUT);
            gpio_set_pull_mode(HDR[i].gpio, GPIO_PULLUP_ONLY);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        for (int i = 0; i < NHDR; i++) lvl[i] = HDR[i].watch ? gpio_get_level(HDR[i].gpio) : -1;
        bench_say("gpio watch %d s: touch header pins with a wire to GND", secs);
        int changes = 0;
        int64_t end = bench_us() + secs * 1000000LL;
        while (bench_us() < end) {
            for (int i = 0; i < NHDR; i++) {
                if (!HDR[i].watch) continue;
                int l = gpio_get_level(HDR[i].gpio);
                if (l != lvl[i]) {
                    lvl[i] = l;
                    changes++;
                    bench_report("gpio.watch", "gpio=%d pin=%d level=%d note=\"%s\"", HDR[i].gpio, HDR[i].pin, l, HDR[i].note);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        for (int i = 0; i < NHDR; i++) if (HDR[i].watch) gpio_reset_pin(HDR[i].gpio);
        bench_report("gpio.watch", "done=1 changes=%d", changes);
        return 0;
    }

    if (!strcmp(sub, "pairs")) {
        static const int P[][2] = { { 5, 3 }, { 21, 28 }, { 24, 30 }, { 25, 31 }, { 32, 49 }, { 46, 51 }, { 47, 52 } };
        int good = 0;
        for (int i = 0; i < sizeof P / sizeof P[0]; i++) {
            bool ok = loop_pair(P[i][0], P[i][1]);
            good += ok;
            bench_report("gpio.pair", "a=%d b=%d pins=%d-%d ok=%d", P[i][0], P[i][1], pin_of(P[i][0]), pin_of(P[i][1]), ok);
        }
        bench_report("gpio.pairs", "ok=%d of=%d", good, (int)(sizeof P / sizeof P[0]));
        return 0;
    }

    if (!strcmp(sub, "loop") && argc >= 4) {
        int a = atoi(argv[2]), b = atoi(argv[3]);
        bench_report("gpio.loop", "a=%d b=%d ok=%d", a, b, loop_pair(a, b));
        return 0;
    }

    if (!strcmp(sub, "set") && argc >= 4) {
        int g = atoi(argv[2]), v = atoi(argv[3]);
        gpio_reset_pin(g);
        gpio_set_direction(g, GPIO_MODE_OUTPUT);
        gpio_set_level(g, v);
        bench_report("gpio.set", "gpio=%d pin=%d level=%d", g, pin_of(g), v);
        return 0;
    }

    if (!strcmp(sub, "vo4")) {
        int g[3] = { 46, 47, 48 };
        for (int i = 0; i < 3; i++) { gpio_reset_pin(g[i]); gpio_set_direction(g[i], GPIO_MODE_OUTPUT); gpio_set_level(g[i], 1); }
        bench_say("GPIO46/47/48 high: measure header pins 35, 37, 39 against GND (expect 3.3 V; SD mounted=%d)", sd_mounted());
        bench_report("gpio.vo4", "driven_high=46,47,48 sd_mounted=%d", sd_mounted());
        return 0;
    }

    printf("gpio map|watch [s]|pairs|loop <a> <b>|set <g> <0|1>|vo4\n");
    return 0;
}

static int cmd_uart(int argc, char **argv)
{
    if (argc < 4 || strcmp(argv[1], "loop")) {
        printf("uart loop <tx> <rx>    (jumper between the two pins)\n");
        return 0;
    }
    int tx = atoi(argv[2]), rx = atoi(argv[3]);
    static const int bauds[] = { 115200, 921600, 2000000, 3000000, 5000000 };
    const int n = 16 * 1024;
    uint8_t *out = heap_caps_malloc(n, MALLOC_CAP_INTERNAL), *in = heap_caps_malloc(n, MALLOC_CAP_INTERNAL);
    for (int i = 0; i < n; i++) out[i] = esp_random();
    for (int b = 0; b < sizeof bauds / sizeof bauds[0]; b++) {
        uart_config_t c = { .baud_rate = bauds[b], .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                            .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
        uart_driver_install(UART_NUM_1, 2 * n, 2 * n, 0, NULL, 0);
        uart_param_config(UART_NUM_1, &c);
        uart_set_pin(UART_NUM_1, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        uart_flush_input(UART_NUM_1);
        int64_t t = bench_us();
        uart_write_bytes(UART_NUM_1, out, n);
        int got = 0;
        while (got < n) {
            int r = uart_read_bytes(UART_NUM_1, in + got, n - got, pdMS_TO_TICKS(200));
            if (r <= 0) break;
            got += r;
        }
        double secs = (bench_us() - t) / 1e6;
        int errors = 0;
        for (int i = 0; i < got; i++) if (in[i] != out[i]) errors++;
        uint32_t real = 0;
        uart_get_baudrate(UART_NUM_1, &real);
        bench_report("uart.loop", "tx=%d rx=%d baud=%d real=%lu sent=%d got=%d errors=%d kBps=%.0f",
                     tx, rx, bauds[b], (unsigned long)real, n, got, errors, got / secs / 1000);
        uart_driver_delete(UART_NUM_1);
    }
    heap_caps_free(out);
    heap_caps_free(in);
    return 0;
}

static const char *i2c_guess(uint8_t a)
{
    switch (a) {
    case 0x14: case 0x5D: return "GT911 touch (board)";
    case 0x18: return "ES8311 codec (board)";
    case 0x40: return "ES7210 mic ADC (board) / HTU21 / SHT21 / Si7021 / INA219";
    case 0x36: return "OV5647 camera";
    case 0x3C: case 0x3D: return "SSD1306 / SH1106 OLED";
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
        return "PCF8574 / MCP23017 expander (or BH1750 at 0x23)";
    case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3E: case 0x3F: return "PCF8574A expander / AHT10-20 (0x38)";
    case 0x44: case 0x45: return "SHT3x / SHT4x";
    case 0x48: case 0x49: case 0x4A: case 0x4B: return "ADS1115 / LM75 / TMP102";
    case 0x50: case 0x57: return "AT24Cxx EEPROM";
    case 0x5A: return "CCS811 / MLX90614";
    case 0x62: return "SCD40/41";
    case 0x68: case 0x69: return "DS3231 / DS1307 / MPU6050";
    case 0x76: case 0x77: return "BME280 / BMP280 / BME680";
    default: return "?";
    }
}

static int cmd_i2c(int argc, char **argv)
{
    i2c_master_bus_handle_t bus;
    bool own = false;
    int sda = 7, scl = 8;
    if (argc >= 4) {
        sda = atoi(argv[2]);
        scl = atoi(argv[3]);
        i2c_master_bus_config_t bc = { .i2c_port = 0, .sda_io_num = sda, .scl_io_num = scl,
                                       .clk_source = I2C_CLK_SRC_DEFAULT, .flags.enable_internal_pullup = true };
        if (i2c_new_master_bus(&bc, &bus) != ESP_OK) { bench_report("i2c.scan", "error=bus"); return 0; }
        own = true;
    } else {
        bsp_i2c_init();
        bus = bsp_i2c_get_handle();
    }
    int found = 0;
    for (int a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK) {
            found++;
            bench_report("i2c.dev", "sda=%d scl=%d addr=0x%02X guess=\"%s\"", sda, scl, a, i2c_guess(a));
        }
    }
    bench_report("i2c.scan", "sda=%d scl=%d found=%d", sda, scl, found);
    if (own) i2c_del_master_bus(bus);
    return 0;
}

static uint16_t mb_crc(const uint8_t *p, int n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
    }
    return c;
}

/* mb <tx> <rx> <de|-1> <baud> <slave> <fc 3|4> <addr> <count>
 * e.g. the Riden RD6012: mb 30 31 29 115200 1 3 0 4 */
static int cmd_mb(int argc, char **argv)
{
    if (argc < 9) {
        printf("mb <tx> <rx> <de|-1> <baud> <slave> <fc 3|4> <addr> <count>\n"
               "   RS485 on the default profile: mb 30 31 29 9600 1 3 0 2\n");
        return 0;
    }
    int tx = atoi(argv[1]), rx = atoi(argv[2]), de = atoi(argv[3]), baud = atoi(argv[4]);
    int slave = atoi(argv[5]), fc = atoi(argv[6]), addr = strtol(argv[7], NULL, 0), count = atoi(argv[8]);
    uart_config_t c = { .baud_rate = baud, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    uart_driver_install(UART_NUM_2, 512, 512, 0, NULL, 0);
    uart_param_config(UART_NUM_2, &c);
    /* the UART drives DE itself through RTS in RS485 half-duplex mode */
    uart_set_pin(UART_NUM_2, tx, rx, de >= 0 ? de : UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (de >= 0) uart_set_mode(UART_NUM_2, UART_MODE_RS485_HALF_DUPLEX);
    uint8_t q[8] = { slave, fc, addr >> 8, addr & 0xFF, count >> 8, count & 0xFF };
    uint16_t crc = mb_crc(q, 6);
    q[6] = crc & 0xFF;
    q[7] = crc >> 8;
    uart_flush_input(UART_NUM_2);
    int64_t t = bench_us();
    uart_write_bytes(UART_NUM_2, q, 8);
    uint8_t r[260];
    int want = 5 + 2 * count, got = 0;
    while (got < want) {
        int n = uart_read_bytes(UART_NUM_2, r + got, want - got, pdMS_TO_TICKS(300));
        if (n <= 0) break;
        got += n;
        if (got >= 5 && (r[1] & 0x80)) { want = 5; }
    }
    int ms = (bench_us() - t) / 1000;
    uart_driver_delete(UART_NUM_2);
    char hex[3 * 64 + 1] = { 0 };
    for (int i = 0; i < got && i < 64; i++) snprintf(hex + 3 * i, 4, "%02X ", r[i]);
    bool crc_ok = got >= 5 && mb_crc(r, got - 2) == (r[got - 2] | (r[got - 1] << 8));
    bench_report("mb", "slave=%d fc=%d addr=%d count=%d got=%d crc_ok=%d exception=%d ms=%d raw=\"%s\"",
                 slave, fc, addr, count, got, crc_ok, got >= 3 && (r[1] & 0x80) ? r[2] : 0, ms, hex);
    if (crc_ok && !(r[1] & 0x80))
        for (int i = 0; i < r[2] / 2; i++) printf("  reg %d = %u\n", addr + i, (r[3 + 2 * i] << 8) | r[4 + 2 * i]);
    return 0;
}

void reg_io(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "gpio", .help = "header GPIO tests (map watch pairs loop set vo4)", .func = cmd_gpio },
        { .command = "uart", .help = "uart loop <tx> <rx>", .func = cmd_uart },
        { .command = "i2c", .help = "i2c scan [sda scl]", .func = cmd_i2c },
        { .command = "mb", .help = "one Modbus RTU read over a UART / RS485 board", .func = cmd_mb },
    };
    for (int i = 0; i < sizeof cmds / sizeof cmds[0]; i++) esp_console_cmd_register(&cmds[i]);
}
