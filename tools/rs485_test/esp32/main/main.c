/* The other end of an RS485 bus, for trying P4OS's Modbus app on a real
 * line: an ESP32 with an RS485 module on UART2, IO17 TX and IO4 RX. The
 * bench's is an automatic "RS485 to TTL" board (TXD, RXD, VCC from 3.3 V,
 * GND); a module with a DE pin (MAX3485, SP3485) takes DE and /RE tied
 * together on IO16, which the UART drives either way. Wired as in
 * docs/img/bench-rs485.svg and tools/rs485_test/README.md.
 *
 * It plays one of two roles, chosen with the BOOT button or from the
 * console, and kept across resets:
 *
 *   slave    unit 1 (by default) of a made-up variable-speed drive: holding
 *            0-9 its settings (setpoint, ramps, limits), input 0-9 what it
 *            measures (speed following the setpoint along the ramp, current,
 *            DC bus, temperature, running time), coils 0-7 its commands
 *            (run, reverse, reset, three relays), discrete 0-7 its state.
 *            Functions 1-6, 8 (echo), 15, 16, 17 and 43; exceptions 01-03 as
 *            the standard says. A fault can be played on demand (silent,
 *            slow, broken CRC, busy) to see how the P4 copes as master.
 *   master   polls a slave (the P4 playing one) every 500 ms: holding 0-9,
 *            input 0-7, coils 0-15, a write of holding 0 and a coil every
 *            few rounds, a block write every ten; it prints every answer
 *            and its delay, and the count of what failed and how.
 *
 * The console (UART0, 115200: the board's USB converter, or the P4's
 * Terminal on usb0) prints every frame both ways, in hex, with the silence
 * before it, and takes one-key commands; h for the list.
 *
 * Frames end where their CRC closes (read_frame): UART2 delivers a frame
 * 4 characters after the line goes quiet, and the answer goes out a
 * millisecond or two later, as a real device's does.
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define BUS        UART_NUM_2
#define PIN_TX     17
#define PIN_RX     4
#define PIN_DE     16
#define PIN_BOOT   0
#define PIN_LED    2

enum { ROLE_SLAVE = 0, ROLE_MASTER, ROLE_COUNT };
static const char *const ROLE_NAME[ROLE_COUNT] = { "slave", "master" };
enum { FAULT_NONE = 0, FAULT_SILENT, FAULT_SLOW, FAULT_CRC, FAULT_BUSY, FAULT_COUNT };
static const char *const FAULT_NAME[FAULT_COUNT] = { "none", "silent", "slow (400 ms)", "broken CRC", "busy (06)" };
static const uint32_t BAUDS[] = { 9600, 19200, 38400, 57600, 115200, 4800, 2400 };
#define NBAUDS (sizeof BAUDS / sizeof BAUDS[0])
static const char *const FMT_NAME[4] = { "8N1", "8E1", "8O1", "8N2" };

static struct {
    int role, baud_i, fmt, unit, fault;
    bool quiet;                 /* do not print every frame */
} C = { ROLE_SLAVE, 0, 0, 1, FAULT_NONE, false };

static int64_t s_last_frame_us;

/* ---- settings ---- */

static void cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open("rs485", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cfg", &C, sizeof C);
    nvs_commit(h);
    nvs_close(h);
}

static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open("rs485", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof C;
    typeof(C) tmp;
    if (nvs_get_blob(h, "cfg", &tmp, &n) == ESP_OK && n == sizeof C && tmp.role >= 0 && tmp.role < ROLE_COUNT &&
        tmp.baud_i >= 0 && tmp.baud_i < (int)NBAUDS && tmp.fmt >= 0 && tmp.fmt < 4)
        C = tmp;
    nvs_close(h);
}

/* ---- the line ---- */

static uint16_t crc16(const uint8_t *p, int n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
    }
    return c;
}

static bool crc_ok(const uint8_t *f, int n)
{
    if (n < 4) return false;
    uint16_t c = crc16(f, n - 2);
    return f[n - 2] == (uint8_t)c && f[n - 1] == (uint8_t)(c >> 8);
}

static int crc_put(uint8_t *f, int n)
{
    uint16_t c = crc16(f, n);
    f[n] = (uint8_t)c;
    f[n + 1] = (uint8_t)(c >> 8);
    return n + 2;
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void line_setup(void)
{
    uart_config_t c = {
        .baud_rate = (int)BAUDS[C.baud_i],
        .data_bits = UART_DATA_8_BITS,
        .parity = C.fmt == 1 ? UART_PARITY_EVEN : C.fmt == 2 ? UART_PARITY_ODD : UART_PARITY_DISABLE,
        .stop_bits = C.fmt == 3 ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_is_driver_installed(BUS)) uart_driver_delete(BUS);
    uart_driver_install(BUS, 1024, 1024, 0, NULL, 0);
    uart_param_config(BUS, &c);
    uart_set_pin(BUS, PIN_TX, PIN_RX, PIN_DE, UART_PIN_NO_CHANGE);
    uart_set_mode(BUS, UART_MODE_RS485_HALF_DUPLEX);
    uart_set_rx_timeout(BUS, 4);        /* 3.5 characters, rounded up */
    printf("\nline: UART2 TX IO%d RX IO%d DE IO%d, %" PRIu32 " %s, role %s, unit %d\n", PIN_TX, PIN_RX, PIN_DE,
           BAUDS[C.baud_i], FMT_NAME[C.fmt], ROLE_NAME[C.role], C.unit);
}

static uint32_t char_us(float chars)
{
    int bits = 1 + 8 + (C.fmt == 1 || C.fmt == 2 ? 1 : 0) + (C.fmt == 3 ? 2 : 1);
    return (uint32_t)(chars * bits * 1e6f / BAUDS[C.baud_i]);
}

static void show(const char *dir, const uint8_t *f, int n, const char *note)
{
    int64_t now = esp_timer_get_time();
    if (!C.quiet) {
        printf("%8.3f %s +%5.1f ms ", now / 1e6, dir, s_last_frame_us ? (now - s_last_frame_us) / 1000.0 : 0.0);
        for (int i = 0; i < n && i < 40; i++) printf("%02X ", f[i]);
        if (n > 40) printf("... (%d)", n);
        if (note && *note) printf(" %s", note);
        printf("\n");
    }
    s_last_frame_us = now;
    gpio_set_level(PIN_LED, 1);
}

/* Reads one frame. UART2 hands its bytes over 4 characters after the line
 * goes quiet, so a whole frame usually arrives at once: as soon as what is
 * there ends in a good CRC it is a frame, and the answer can go out within a
 * few milliseconds, as a real device's does. Without a good CRC it waits
 * for 30 characters of silence (a long frame comes in FIFO-sized pieces) and
 * returns what came, for the caller to call damaged. 0 when nothing came by
 * the deadline. */
static int read_frame(uint8_t *buf, int max, int wait_ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)wait_ms * 1000, last = 0;
    int n = 0;
    for (;;) {
        int r = uart_read_bytes(BUS, buf + n, max - n, 0);
        int64_t now = esp_timer_get_time();
        if (r > 0) {
            n += r;
            last = now;
            if (n >= max || crc_ok(buf, n)) return n;
        } else if (n && now - last >= (int64_t)char_us(30.0f)) return n;
        else if (!n && now >= end) return 0;
        vTaskDelay(1);
    }
}

static void send_frame(uint8_t *f, int n, const char *note)
{
    show("TX", f, n, note);
    uart_write_bytes(BUS, f, n);
    uart_wait_tx_done(BUS, pdMS_TO_TICKS(500));
}

/* ---- the slave: a made-up drive ---- */

static uint16_t hr[100], ir[100];
static uint8_t coil[64], di[64];
static float s_speed;           /* rpm, follows the setpoint along the ramp */
static int64_t s_run_us;

static void drive_init(void)
{
    hr[0] = 1450;               /* speed setpoint, rpm */
    hr[1] = 50;                 /* acceleration ramp, 0.1 s */
    hr[2] = 30;                 /* deceleration ramp, 0.1 s */
    hr[3] = 160;                /* current limit, 0.1 A */
    hr[4] = 1500;               /* top speed, rpm */
    hr[9] = 0x0102;             /* "firmware" 1.2 */
}

static void drive_tick(float dt)
{
    bool run = coil[0], reset = coil[2];
    if (reset) { memset(ir, 0, sizeof ir); s_run_us = 0; coil[2] = 0; }
    float target = run ? (float)(hr[0] > hr[4] ? hr[4] : hr[0]) : 0;
    float ramp = (float)hr[4] / ((run ? hr[1] : hr[2]) / 10.0f + 0.01f);   /* rpm per second */
    if (s_speed < target) s_speed = fminf(target, s_speed + ramp * dt);
    else s_speed = fmaxf(target, s_speed - ramp * dt);
    if (run) s_run_us += (int64_t)(dt * 1e6f);
    float load = s_speed / 1500.0f;
    float amps = s_speed > 1 ? 2.0f + 8.0f * load * load : 0;
    if (amps > hr[3] / 10.0f) amps = hr[3] / 10.0f;
    static float temp = 28;
    temp += ((28 + amps * 3) - temp) * dt / 60;
    ir[0] = (uint16_t)s_speed;
    ir[1] = (uint16_t)(amps * 10);
    ir[2] = (uint16_t)(565 + (int)(sinf(esp_timer_get_time() / 3e5f) * 4));      /* DC bus, V */
    ir[3] = (uint16_t)(temp * 10);
    ir[4] = (uint16_t)(s_run_us / 3600000000LL);                               /* hours */
    ir[5] = (uint16_t)(s_run_us / 60000000LL % 60);                            /* minutes */
    ir[6] = (uint16_t)(coil[1] ? 1 : 0);
    di[0] = s_speed > 1;                                                       /* running */
    di[1] = fabsf(s_speed - target) < 1 && run;                                /* at speed */
    di[2] = amps >= hr[3] / 10.0f;                                             /* current limit */
    di[3] = temp > 70;                                                         /* hot */
    gpio_set_level(5, coil[3]);
}

static int slave_answer(const uint8_t *q, int qn, uint8_t *o)
{
    int fc = q[1], ex = 0, n = 2;
    uint16_t a = qn >= 6 ? get16(q + 2) : 0, cnt = qn >= 8 ? get16(q + 4) : 0;
    o[0] = (uint8_t)C.unit;
    o[1] = (uint8_t)fc;
    switch (fc) {
    case 1: case 2:
        if (cnt < 1 || cnt > 2000) { ex = 3; break; }
        if (a + cnt > 64) { ex = 2; break; }
        o[2] = (uint8_t)((cnt + 7) / 8);
        memset(o + 3, 0, o[2]);
        for (int i = 0; i < cnt; i++) if ((fc == 1 ? coil : di)[a + i]) o[3 + i / 8] |= (uint8_t)(1 << (i % 8));
        n = 3 + o[2];
        break;
    case 3: case 4:
        if (cnt < 1 || cnt > 125) { ex = 3; break; }
        if (a + cnt > 100) { ex = 2; break; }
        o[2] = (uint8_t)(2 * cnt);
        for (int i = 0; i < cnt; i++) put16(o + 3 + 2 * i, (fc == 3 ? hr : ir)[a + i]);
        n = 3 + o[2];
        break;
    case 5:
        if (cnt != 0xFF00 && cnt != 0) { ex = 3; break; }
        if (a >= 64) { ex = 2; break; }
        coil[a] = cnt != 0;
        printf("         coil %u = %d\n", a, coil[a]);
        memcpy(o + 2, q + 2, 4);
        n = 6;
        break;
    case 6:
        if (a >= 100) { ex = 2; break; }
        hr[a] = cnt;
        printf("         holding %u = %u\n", a, cnt);
        memcpy(o + 2, q + 2, 4);
        n = 6;
        break;
    case 8:
        if (a != 0) { ex = 1; break; }
        memcpy(o + 2, q + 2, 4);
        n = 6;
        break;
    case 15: case 16: {
        int bc = qn >= 7 ? q[6] : 0;
        if (fc == 15 ? (cnt < 1 || cnt > 1968 || bc != (cnt + 7) / 8) : (cnt < 1 || cnt > 123 || bc != 2 * cnt)) { ex = 3; break; }
        if (a + cnt > (fc == 15 ? 64 : 100)) { ex = 2; break; }
        for (int i = 0; i < cnt; i++) {
            if (fc == 15) coil[a + i] = (q[7 + i / 8] >> (i % 8)) & 1;
            else hr[a + i] = get16(q + 7 + 2 * i);
        }
        printf("         %s %u..%u written\n", fc == 15 ? "coils" : "holding", a, a + cnt - 1);
        memcpy(o + 2, q + 2, 4);
        n = 6;
        break;
    }
    case 17: {
        static const char ID[] = "ESP32-VFD";
        o[2] = sizeof ID;               /* the text and the run indicator */
        memcpy(o + 3, ID, sizeof ID - 1);
        o[3 + sizeof ID - 1] = coil[0] ? 0xFF : 0x00;
        n = 3 + o[2];
        break;
    }
    case 43: {
        if (qn < 7 || q[2] != 14) { ex = 1; break; }
        static const char *const OBJ[3] = { "P4OS bench", "ESP32 drive", "1.2" };
        o[2] = 14; o[3] = 1; o[4] = 0x01; o[5] = 0; o[6] = 0; o[7] = 3;
        n = 8;
        for (int i = 0; i < 3; i++) {
            int l = (int)strlen(OBJ[i]);
            o[n++] = (uint8_t)i;
            o[n++] = (uint8_t)l;
            memcpy(o + n, OBJ[i], l);
            n += l;
        }
        break;
    }
    default:
        ex = 1;
    }
    if (ex) { o[1] = (uint8_t)(fc | 0x80); o[2] = (uint8_t)ex; n = 3; }
    return n;
}

static void slave_round(void)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    drive_tick(last ? (now - last) / 1e6f : 0);
    last = now;
    uint8_t q[260], o[260];
    int n = read_frame(q, sizeof q, 20);
    if (!n) return;
    if (!crc_ok(q, n)) { show("RX", q, n, "<- bad CRC, ignored"); return; }
    if (q[0] != C.unit && q[0] != 0) { show("RX", q, n, "(another unit)"); return; }
    show("RX", q, n, q[0] ? "" : "(broadcast: no answer)");
    int m = slave_answer(q, n, o);
    if (!q[0]) return;
    switch (C.fault) {
    case FAULT_SILENT: printf("         (fault: silent)\n"); return;
    case FAULT_SLOW: vTaskDelay(pdMS_TO_TICKS(400)); break;
    case FAULT_BUSY: o[1] = (uint8_t)(q[1] | 0x80); o[2] = 6; m = 3; break;
    default: break;
    }
    m = crc_put(o, m);
    if (C.fault == FAULT_CRC) o[m - 1] ^= 0x5A;
    send_frame(o, m, C.fault ? FAULT_NAME[C.fault] : "");
}

/* ---- the master ---- */

static uint32_t m_req, m_ok, m_tmo, m_crc, m_exc, m_bad, m_rt_sum, m_rt_max;

static int transact(uint8_t *q, int qn, uint8_t *a, const char *what)
{
    qn = crc_put(q, qn);
    uart_flush_input(BUS);
    send_frame(q, qn, what);
    int64_t t0 = esp_timer_get_time();
    m_req++;
    int n = read_frame(a, 260, 500);
    uint32_t rt = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (!n) { m_tmo++; printf("         no answer in 500 ms\n"); return -1; }
    if (!crc_ok(a, n)) { m_crc++; show("RX", a, n, "<- bad CRC"); return -2; }
    if (a[0] != q[0] || (a[1] & 0x7F) != q[1]) { m_bad++; show("RX", a, n, "<- not the answer to that"); return -3; }
    m_rt_sum += rt;
    if (rt > m_rt_max) m_rt_max = rt;
    m_ok++;
    char note[48];
    if (a[1] & 0x80) { m_exc++; snprintf(note, sizeof note, "<- exception %02X, %" PRIu32 " ms", a[2], rt); }
    else snprintf(note, sizeof note, "<- %" PRIu32 " ms", rt);
    show("RX", a, n, note);
    return n;
}

static void master_round(void)
{
    static uint32_t k;
    static int64_t next;
    int64_t now = esp_timer_get_time();
    if (now < next) { vTaskDelay(pdMS_TO_TICKS(10)); return; }
    next = now + 500000;
    k++;
    uint8_t q[260], a[260];
    q[0] = (uint8_t)C.unit;
    q[1] = 3; put16(q + 2, 0); put16(q + 4, 10);
    transact(q, 6, a, "read holding 0-9");
    vTaskDelay(pdMS_TO_TICKS(20));
    q[1] = 4; put16(q + 2, 0); put16(q + 4, 8);
    transact(q, 6, a, "read input 0-7");
    vTaskDelay(pdMS_TO_TICKS(20));
    q[1] = 1; put16(q + 2, 0); put16(q + 4, 16);
    transact(q, 6, a, "read coils 0-15");
    if (k % 3 == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        q[1] = 6; put16(q + 2, 0); put16(q + 4, (uint16_t)k);
        transact(q, 6, a, "write holding 0");
        vTaskDelay(pdMS_TO_TICKS(20));
        q[1] = 5; put16(q + 2, (uint16_t)(k / 3 % 8)); put16(q + 4, (k / 24) % 2 ? 0 : 0xFF00);
        transact(q, 6, a, "write a coil");
    }
    if (k % 10 == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        q[1] = 16; put16(q + 2, 10); put16(q + 4, 3); q[6] = 6;
        put16(q + 7, (uint16_t)k); put16(q + 9, (uint16_t)(k * 2)); put16(q + 11, (uint16_t)(k * 3));
        transact(q, 13, a, "write holding 10-12");
    }
    if (k % 10 == 0)
        printf("== %" PRIu32 " requests: %" PRIu32 " answered (%" PRIu32 " exceptions), %" PRIu32 " no answer, %" PRIu32 " bad CRC, %" PRIu32
               " wrong; delay avg %" PRIu32 " ms, max %" PRIu32 " ms\n",
               m_req, m_ok, m_exc, m_tmo, m_crc, m_bad, m_ok ? m_rt_sum / m_ok : 0, m_rt_max);
}

/* ---- the console ---- */

static void help(void)
{
    printf("\nRS485 bench node. Keys:\n"
           "  r  role: slave <-> master (or the BOOT button)\n"
           "  b  next speed (9600 19200 38400 57600 115200 4800 2400)\n"
           "  f  next format (8N1 8E1 8O1 8N2)\n"
           "  u  next unit (1..5, the one it answers as or asks)\n"
           "  x  slave: next fault (none, silent, slow, broken CRC, busy)\n"
           "  q  quiet: stop / start printing every frame\n"
           "  s  the master's counts, now\n"
           "  h  this\n");
}

static void key(char c)
{
    switch (c) {
    case 'r': C.role = (C.role + 1) % ROLE_COUNT; break;
    case 'b': C.baud_i = (C.baud_i + 1) % (int)NBAUDS; break;
    case 'f': C.fmt = (C.fmt + 1) % 4; break;
    case 'u': C.unit = C.unit % 5 + 1; break;
    case 'x': C.fault = (C.fault + 1) % FAULT_COUNT; printf("fault: %s\n", FAULT_NAME[C.fault]); break;
    case 'q': C.quiet = !C.quiet; printf("quiet %s\n", C.quiet ? "on" : "off"); return;
    case 's': printf("%" PRIu32 " requests, %" PRIu32 " ok, %" PRIu32 " no answer, %" PRIu32 " bad CRC\n", m_req, m_ok, m_tmo, m_crc); return;
    case 'h': help(); return;
    default: return;
    }
    cfg_save();
    line_setup();
}

void app_main(void)
{
    if (nvs_flash_init() != ESP_OK) { nvs_flash_erase(); nvs_flash_init(); }
    cfg_load();
    C.fault = FAULT_NONE;           /* a fault never survives a reset */
    uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_BOOT, .mode = GPIO_MODE_INPUT, .pull_up_en = 1 };
    gpio_config(&io);
    gpio_config_t led = { .pin_bit_mask = 1ULL << PIN_LED | 1ULL << 5, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&led);
    drive_init();
    help();
    line_setup();
    int boot_was = 1;
    for (;;) {
        uint8_t c;
        while (uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1) key((char)c);
        int boot = gpio_get_level(PIN_BOOT);
        if (!boot && boot_was) key('r');
        boot_was = boot;
        gpio_set_level(PIN_LED, 0);
        if (C.role == ROLE_SLAVE) slave_round();
        else master_round();
    }
}
