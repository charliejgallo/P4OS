/*
 * P4OS simulator - aos_io backend.
 *
 * UART ports: P4_SIM_UART_A=/dev/cu.usbserial-110 (and _B) makes "uart.a" a
 * real serial adapter on the Mac - a CH340 on the bench works as the P4's
 * header would. Without it, the port is a make-believe ESP32 printing a boot
 * log and a heartbeat, so the Terminal has something to show.
 *
 * I2C: the board bus answers where the board's chips are; "i2c.ext" answers
 * at the addresses in P4_SIM_I2C_EXT (default 0x76,0x44: a BME280 and an
 * SHT3x). GPIO: levels kept in memory, inputs with pull-up read 1. On top
 * of that, sensors_sim.c's emulated sensors (P4_SIM_SENSORS) answer on every
 * port but the board's with their chips' real protocols, and win over the
 * plain register blobs at their addresses.
 *
 * The target's EN and BOOT (aos_io_uart_lines): on a real adapter they are
 * RTS and DTR, esptool's classic reset (asserted = low at the chip, through
 * the usual two-transistor circuit), set together with one TIOCMSET. A pty
 * has no modem lines, so there they are only recorded - and, with
 * P4_SIM_UART_B_LINES=<file>, appended to that file as "<ms> en=<0|1>
 * boot=<0|1>" lines, which is how tools/fake_esp_rom.py sees the resets of
 * the target it plays. The GPIOs of the lines also take the levels, so the
 * Bus app shows them.
 *
 * SPI: emulated chips (a W25Q128 flash, a MAX31855, an MCP3008) or a
 * loopback, chosen by CS; see the SPI section at the end.
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    int fd;                 /* -1: the make-believe device */
    bool modem;             /* the device has RTS/DTR (not a pty) */
    char lines_path[256];   /* P4_SIM_<PORT>_LINES */
    uint64_t next_ms;
    int step;
    char pending[256];
    int pend_len, pend_pos;
} sim_uart_t;

static void env_name(const char *port, char *out, size_t cap)
{
    snprintf(out, cap, "P4_SIM_%s", port);
    for (char *p = out; *p; p++) *p = *p == '.' ? '_' : (char)toupper((unsigned char)*p);
}

static bool set_speed(int fd, uint32_t baud)
{
    struct termios t;
    if (tcgetattr(fd, &t)) return false;
    tcflag_t fmt = t.c_cflag & (PARENB | PARODD | CSTOPB);     /* a change of speed keeps the format */
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD | fmt;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    cfsetspeed(&t, (speed_t)baud);     /* BSD: speed_t is the baud rate itself */
    return tcsetattr(fd, TCSANOW, &t) == 0;
}

bool aos_io_be_uart_open(aos_io_uart_t *u, bool rs485)
{
    (void)rs485;
    sim_uart_t *s = calloc(1, sizeof *s);
    s->fd = -1;
    char env[40];
    env_name(u->port->name, env, sizeof env);
    const char *dev = getenv(env);
    if (dev && *dev) {
        s->fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (s->fd < 0 || !set_speed(s->fd, u->baud)) {
            aos_hal_log("io", "%s: cannot open %s (%s)", u->port->name, dev, strerror(errno));
            if (s->fd >= 0) close(s->fd);
            free(s);
            return false;
        }
        snprintf(u->desc, sizeof u->desc, "%s = %s", u->port->name, dev);
        int bits;
        s->modem = ioctl(s->fd, TIOCMGET, &bits) == 0;
        char lenv[48];
        snprintf(lenv, sizeof lenv, "%s_LINES", env);
        const char *lp = getenv(lenv);
        if (lp) snprintf(s->lines_path, sizeof s->lines_path, "%s", lp);
    } else {
        snprintf(u->desc, sizeof u->desc, "%s (simulated ESP32)", u->port->name);
    }
    u->be = s;
    return true;
}

/* A plausible ESP-IDF boot followed by a heartbeat, with the colours IDF
 * puts in its log lines. */
static const char *BOOT[] = {
    "ESP-ROM:esp32s3-20210327",
    "rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)",
    "\x1b[0;32mI (24) boot: ESP-IDF v5.5.5 2nd stage bootloader\x1b[0m",
    "\x1b[0;32mI (25) boot: compile time Sep 27 2026 21:40:02\x1b[0m",
    "\x1b[0;32mI (312) cpu_start: Pro cpu start user code\x1b[0m",
    "\x1b[0;32mI (318) app_init: Project name:     gateway-modbus\x1b[0m",
    "\x1b[0;33mW (402) wifi: esp_wifi_connect: retry 1 (reason 201)\x1b[0m",
    "\x1b[0;32mI (1402) wifi: connected to Taller, channel 6, RSSI -58\x1b[0m",
    "\x1b[0;32mI (1590) esp_netif_handlers: sta ip: 192.168.1.77\x1b[0m",
    "\x1b[0;32mI (1602) modbus: TCP slave on port 502\x1b[0m",
    "\x1b[0;31mE (2301) sensors: BME280 not found at 0x76 (ESP_ERR_TIMEOUT)\x1b[0m",
};

static void make_line(sim_uart_t *s)
{
    int nboot = (int)(sizeof BOOT / sizeof BOOT[0]);
    uint32_t t = (uint32_t)aos_hal_uptime_ms();
    if (s->step < nboot) snprintf(s->pending, sizeof s->pending, "%s\r\n", BOOT[s->step]);
    else if (s->step % 7 == 3)
        snprintf(s->pending, sizeof s->pending, "\x1b[0;33mW (%u) modbus: slow reply from 0x%02X: %d ms\x1b[0m\r\n",
                 t, 1 + s->step % 4, 180 + s->step % 90);
    else
        snprintf(s->pending, sizeof s->pending, "\x1b[0;32mI (%u) main: heartbeat %d, free heap %d, temp %.1f C\x1b[0m\r\n",
                 t, s->step - nboot, 172340 - (s->step % 13) * 96, 24.0 + (s->step % 10) * 0.1);
    s->pend_len = (int)strlen(s->pending);
    s->pend_pos = 0;
    s->step++;
}

int aos_io_be_uart_read(aos_io_uart_t *u, void *buf, int len, int timeout_ms)
{
    sim_uart_t *s = u->be;
    if (s->fd >= 0) {
        struct pollfd p = { .fd = s->fd, .events = POLLIN };
        int r = poll(&p, 1, timeout_ms);
        if (r <= 0) return 0;
        ssize_t n = read(s->fd, buf, (size_t)len);
        if (n < 0) return (errno == EAGAIN) ? 0 : -1;
        return (int)n;
    }
    uint64_t now = aos_hal_uptime_ms();
    if (s->pend_pos >= s->pend_len) {
        if (now < s->next_ms) {
            usleep((useconds_t)(timeout_ms * 1000));
            return 0;
        }
        make_line(s);
        s->next_ms = now + (s->step < 12 ? 120 : 700);
    }
    int n = s->pend_len - s->pend_pos;
    if (n > len) n = len;
    memcpy(buf, s->pending + s->pend_pos, (size_t)n);
    s->pend_pos += n;
    return n;
}

int aos_io_be_uart_write(aos_io_uart_t *u, const void *buf, int len)
{
    sim_uart_t *s = u->be;
    if (s->fd >= 0) {
        /* the fd is non-blocking and a pty takes about a kilobyte at a time:
         * keep going while the other side drains, as the board's driver
         * blocks until the bytes are in its ring */
        const uint8_t *p = buf;
        int done = 0, idle_ms = 0;
        while (done < len && idle_ms < 1000) {
            ssize_t n = write(s->fd, p + done, (size_t)(len - done));
            if (n > 0) { done += (int)n; idle_ms = 0; continue; }
            if (n < 0 && errno != EAGAIN && errno != EINTR) return done ? done : -1;
            struct pollfd pw = { .fd = s->fd, .events = POLLOUT };
            poll(&pw, 1, 10);
            idle_ms += 10;
        }
        return done;
    }
    /* the make-believe device echoes what it is sent */
    int n = len < (int)sizeof s->pending - 3 ? len : (int)sizeof s->pending - 3;
    memcpy(s->pending, buf, (size_t)n);
    s->pending[n] = '\r';
    s->pending[n + 1] = '\n';
    s->pend_len = n + 2;
    s->pend_pos = 0;
    return len;
}

bool aos_io_be_uart_set_format(aos_io_uart_t *u, char parity, int stop_bits)
{
    sim_uart_t *s = u->be;
    if (s->fd < 0) return true;
    struct termios t;
    if (tcgetattr(s->fd, &t)) return false;
    t.c_cflag &= ~(tcflag_t)(PARENB | PARODD | CSTOPB);
    if (parity != 'N') t.c_cflag |= PARENB | (parity == 'O' ? PARODD : 0);
    if (stop_bits == 2) t.c_cflag |= CSTOPB;
    return tcsetattr(s->fd, TCSANOW, &t) == 0;
}

bool aos_io_be_uart_set_baud(aos_io_uart_t *u, uint32_t baud)
{
    sim_uart_t *s = u->be;
    return s->fd < 0 || set_speed(s->fd, baud);
}

void aos_io_be_uart_close(aos_io_uart_t *u)
{
    sim_uart_t *s = u->be;
    if (s && s->fd >= 0) close(s->fd);
    free(s);
    u->be = NULL;
}

/* ---- I2C ---- */

bool sim_sensors_on(void);     /* sensors_sim.c */

static bool ext_has(uint8_t addr)
{
    const char *e = getenv("P4_SIM_I2C_EXT");
    if (!e) e = sim_sensors_on() ? "" : "0x76,0x44";      /* the emulated ones stand in for these */
    for (const char *p = e; *p; ) {
        int a = (int)strtol(p, (char **)&p, 0);
        if (a == addr) return true;
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
    }
    return false;
}

bool aos_io_be_i2c_open(aos_io_i2c_t *b) { (void)b; return true; }

/* sensors_sim.c */
bool sim_sensors_has(uint8_t addr);
bool sim_sensors_xfer(uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn);

bool aos_io_be_i2c_probe(aos_io_i2c_t *b, uint8_t addr)
{
    if (!strcmp(b->port->name, "i2c.board")) return addr == 0x5D || addr == 0x18 || addr == 0x40;
    return sim_sensors_has(addr) || ext_has(addr);
}

/* Every simulated device is 256 bytes of registers with auto-increment, the
 * usual shape: a write of [reg] then a read returns from reg on, a write of
 * [reg, v...] stores. A BME280 at 0x76/0x77 gets its chip id (0xD0 = 0x60), a
 * made-up calibration block and a measurement that drifts, so the register
 * view has something that changes. */
static uint8_t s_regs[2][128][256];
static bool s_regs_init[2][128];

static uint8_t *regs_of(aos_io_i2c_t *b, uint8_t addr)
{
    int bus = !strcmp(b->port->name, "i2c.board");
    uint8_t *m = s_regs[bus][addr & 0x7F];
    if (!s_regs_init[bus][addr & 0x7F]) {
        s_regs_init[bus][addr & 0x7F] = true;
        for (int i = 0; i < 256; i++) m[i] = (i >= 0x88 && i < 0xA2) ? (uint8_t)(i * 37 + addr) : 0;
        if (addr == 0x76 || addr == 0x77) { m[0xD0] = 0x60; m[0xF3] = 0x00; m[0xF4] = 0x27; m[0xF5] = 0xA0; }
        if (bus && addr == 0x18) { m[0xFD] = 0x83; m[0xFE] = 0x11; }   /* ES8311 chip id */
    }
    if (addr == 0x76 || addr == 0x77) {
        uint32_t t = (uint32_t)(aos_hal_uptime_ms() / 250);
        m[0xF7] = 0x51; m[0xF8] = (uint8_t)(0x20 + t % 7); m[0xF9] = (uint8_t)(t * 13);
        m[0xFA] = 0x7E; m[0xFB] = (uint8_t)(0x90 + t % 3); m[0xFC] = (uint8_t)(t * 29);
        m[0xFD] = 0x6B; m[0xFE] = (uint8_t)(0x40 + t % 5);
    }
    return m;
}

bool aos_io_be_i2c_xfer(aos_io_i2c_t *b, uint8_t addr, const void *w, size_t wn, void *r, size_t rn, int timeout_ms)
{
    (void)timeout_ms;
    if (!aos_io_be_i2c_probe(b, addr)) return false;
    if (strcmp(b->port->name, "i2c.board") && sim_sensors_has(addr)) return sim_sensors_xfer(addr, w, wn, r, rn);
    uint8_t *m = regs_of(b, addr);
    const uint8_t *wb = w;
    uint8_t reg = wn ? wb[0] : 0;
    for (size_t i = 1; i < wn; i++) m[(uint8_t)(reg + i - 1)] = wb[i];
    for (size_t i = 0; r && i < rn; i++) ((uint8_t *)r)[i] = m[(uint8_t)(reg + i)];
    return true;
}

void aos_io_be_i2c_close(aos_io_i2c_t *b) { (void)b; }

/* ---- GPIO ---- */

static int8_t s_level[64];
static uint8_t s_mode[64];

bool aos_io_be_gpio_mode(int gpio, aos_gpio_mode_t mode)
{
    if (gpio < 0 || gpio >= 64) return false;
    s_mode[gpio] = (uint8_t)mode;
    if (mode == AOS_GPIO_INPUT_PULLUP) s_level[gpio] = 1;
    else if (mode == AOS_GPIO_INPUT_PULLDOWN) s_level[gpio] = 0;
    return true;
}

int aos_io_be_gpio_get(int gpio) { return (gpio >= 0 && gpio < 64) ? s_level[gpio] : -1; }

bool aos_io_be_gpio_set(int gpio, int level)
{
    if (gpio < 0 || gpio >= 64) return false;
    s_level[gpio] = level ? 1 : 0;
    return true;
}

/* ---- the target's EN and BOOT ---- */

bool aos_io_be_uart_lines(aos_io_uart_t *u, int en, int boot)
{
    sim_uart_t *s = u->be;
    if (boot >= 0 && u->boot_gpio >= 0) s_level[u->boot_gpio] = (int8_t)boot;
    if (en >= 0 && u->en_gpio >= 0) s_level[u->en_gpio] = (int8_t)en;
    if (s->fd >= 0 && s->modem) {
        int bits;
        if (ioctl(s->fd, TIOCMGET, &bits)) return false;
        if (boot >= 0) bits = boot ? (bits & ~TIOCM_DTR) : (bits | TIOCM_DTR);
        if (en >= 0) bits = en ? (bits & ~TIOCM_RTS) : (bits | TIOCM_RTS);
        if (ioctl(s->fd, TIOCMSET, &bits)) return false;
    }
    if (s->lines_path[0]) {
        int e = en >= 0 ? en : u->en, b = boot >= 0 ? boot : u->boot;
        FILE *f = fopen(s->lines_path, "a");
        if (f) {
            fprintf(f, "%llu en=%d boot=%d\n", (unsigned long long)aos_hal_uptime_ms(), e < 0 ? 1 : e, b < 0 ? 1 : b);
            fclose(f);
        }
    }
    return true;
}

void aos_io_be_uart_lines_release(aos_io_uart_t *u)
{
    /* let go = both high at the chip: RTS and DTR released */
    aos_io_be_uart_lines(u, u->en_gpio >= 0 ? 1 : -1, u->boot_gpio >= 0 ? 1 : -1);
}

/* ---- SPI ----
 *
 * The port's wires lead to emulated chips, chosen by the CS line of each
 * transfer. P4_SIM_SPI_A (P4_SIM_<PORT>, as the UARTs) says which:
 *
 *   unset                         49=w25q128,48=max31855,47=mcp3008
 *   "49=max31855,48=w25q128"      a chip per CS GPIO
 *   "mcp3008"                     that chip on any CS (and with none)
 *   "loop"                        MOSI wired to MISO: what a jumper between
 *                                 header pins 34 and 36 does on the board
 *   "none"                        nothing connected: MISO's pull-up, 0xFF
 *
 * The chips:
 *
 *   w25q128   Winbond 16 MB flash. 0x9F gives EF 40 18, 0x90 EF 17, 0xAB
 *             17, 0x4B a unique id; 0x05/0x35 status, 0x03/0x0B read,
 *             0x06/0x04 write enable, 0x02 page program, 0x20/0xD8/0xC7
 *             erase. The first 64 KB are kept (the rest reads erased), and
 *             start with a line of text so a read shows something.
 *   max31855  thermocouple converter, read-only, 32 bits: a K junction near
 *             24 °C that drifts a few degrees over a minute, the chip's own
 *             temperature near 26. "max31855:open" reports an open probe.
 *   mcp3008   8-channel 10-bit ADC, emulated bit by bit after the start bit
 *             (so the usual 01 80 00 and the other alignments work), VREF
 *             3.3 V: ch0 a slow sine, ch1 a pot at mid travel, ch2 a 30 s
 *             ramp, ch3 ground, ch4 the 3V3 rail, ch5 a light sensor, ch6-7
 *             floating.
 *
 * With no CS (the app drives one with aos_io_gpio_*), the chip is the one
 * whose CS GPIO the app holds low as an output. The chips also mind the
 * line the way real ones do: a mode they do not speak, or a clock above
 * their maximum, and what comes back is shifted a bit or has bits wrong;
 * LSB-first reverses every byte on the wire. */

#include <math.h>
#include <pthread.h>

enum { SD_NONE = 0, SD_LOOP, SD_FLASH, SD_TC, SD_TC_OPEN, SD_ADC };

typedef struct {
    int8_t any;                 /* the chip on every CS, 0 = per CS */
    int8_t by_cs[64];
} sim_spi_map_t;

typedef struct { sim_spi_map_t map; } sim_spi_t;

static pthread_mutex_t s_spi_mx = PTHREAD_MUTEX_INITIALIZER;
static uint8_t s_flash[65536];
static bool s_flash_init, s_flash_wel;

static int dev_named(const char *n, size_t len)
{
    static const struct { const char *name; int dev; } N[] = {
        { "none", SD_NONE }, { "loop", SD_LOOP }, { "loopback", SD_LOOP },
        { "w25q128", SD_FLASH }, { "flash", SD_FLASH },
        { "max31855", SD_TC }, { "max31855:open", SD_TC_OPEN },
        { "mcp3008", SD_ADC }, { "adc", SD_ADC },
    };
    for (size_t i = 0; i < sizeof N / sizeof N[0]; i++)
        if (strlen(N[i].name) == len && !strncasecmp(n, N[i].name, len)) return N[i].dev;
    return -1;
}

static void map_parse(const char *spec, sim_spi_map_t *m)
{
    memset(m, 0, sizeof *m);
    for (const char *p = spec; *p; ) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if (e == p) break;
        const char *eq = memchr(p, '=', (size_t)(e - p));
        if (eq) {
            int g = atoi(p), d = dev_named(eq + 1, (size_t)(e - eq - 1));
            if (g >= 0 && g < 64 && d >= 0) m->by_cs[g] = (int8_t)d;
            else aos_hal_log("io", "sim SPI: \"%.*s\" not understood", (int)(e - p), p);
        } else {
            int d = dev_named(p, (size_t)(e - p));
            if (d >= 0) m->any = (int8_t)(d == SD_NONE ? -1 : d);
            else aos_hal_log("io", "sim SPI: no chip called \"%.*s\"", (int)(e - p), p);
        }
        p = e;
    }
}

bool aos_io_be_spi_open(aos_io_spi_t *s, bool first)
{
    (void)first;
    sim_spi_t *st = calloc(1, sizeof *st);
    char env[40];
    env_name(s->port->name, env, sizeof env);
    const char *spec = getenv(env);
    map_parse(spec ? spec : "49=w25q128,48=max31855,47=mcp3008", &st->map);
    s->actual_hz = s->clock_hz;
    s->be = st;
    return true;
}

bool aos_io_be_spi_set_clock(aos_io_spi_t *s, uint32_t hz)
{
    s->actual_hz = hz;
    return true;
}

void aos_io_be_spi_close(aos_io_spi_t *s, bool last)
{
    (void)last;
    free(s->be);
    s->be = NULL;
}

static int chip_for(aos_io_spi_t *s)
{
    sim_spi_t *st = s->be;
    if (st->map.any) return st->map.any < 0 ? SD_NONE : st->map.any;
    if (s->cs >= 0) return st->map.by_cs[s->cs];
    for (int g = 0; g < 64; g++)
        if (st->map.by_cs[g] && s_mode[g] == AOS_GPIO_OUTPUT && s_level[g] == 0) return st->map.by_cs[g];
    return SD_NONE;
}

static double sim_t(void) { return (double)aos_hal_uptime_ms() / 1000.0; }
static double jitter(double amp) { return amp * ((double)rand() / RAND_MAX * 2.0 - 1.0); }

static void flash_init(void)
{
    if (s_flash_init) return;
    s_flash_init = true;
    memset(s_flash, 0xFF, sizeof s_flash);
    static const char HELLO[] = "P4OS simulated W25Q128, sector 0. Hola desde el banco!\n";
    memcpy(s_flash, HELLO, sizeof HELLO - 1);
}

static uint8_t flash_at(uint32_t a) { return a < sizeof s_flash ? s_flash[a] : 0xFF; }

static void flash_xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    flash_init();
    memset(rx, 0xFF, n);
    if (!n) return;
    uint8_t cmd = tx[0];
    uint32_t addr = n >= 4 ? ((uint32_t)tx[1] << 16 | (uint32_t)tx[2] << 8 | tx[3]) : 0;
    static const uint8_t JEDEC[3] = { 0xEF, 0x40, 0x18 };
    static const uint8_t UID[8] = { 0xD8, 0x61, 0x2C, 0x4B, 0x13, 0x37, 0x3A, 0x26 };
    switch (cmd) {
    case 0x9F: for (size_t i = 1; i < n && i < 4; i++) rx[i] = JEDEC[i - 1]; break;
    case 0x90: for (size_t i = 4; i < n; i++) rx[i] = ((i - 4) ^ (addr & 1)) & 1 ? 0x17 : 0xEF; break;
    case 0xAB: for (size_t i = 4; i < n; i++) rx[i] = 0x17; break;
    case 0x4B: for (size_t i = 5; i < n && i < 13; i++) rx[i] = UID[i - 5]; break;
    case 0x05: for (size_t i = 1; i < n; i++) rx[i] = s_flash_wel ? 0x02 : 0x00; break;
    case 0x35: for (size_t i = 1; i < n; i++) rx[i] = 0x02; break;
    case 0x03: for (size_t i = 4; i < n; i++) rx[i] = flash_at((addr + (uint32_t)(i - 4)) & 0xFFFFFF); break;
    case 0x0B: for (size_t i = 5; i < n; i++) rx[i] = flash_at((addr + (uint32_t)(i - 5)) & 0xFFFFFF); break;
    case 0x06: s_flash_wel = true; break;
    case 0x04: s_flash_wel = false; break;
    case 0x02:
        if (s_flash_wel)
            for (size_t i = 4; i < n; i++) {
                uint32_t a = (addr & ~0xFFu) | ((addr + (uint32_t)(i - 4)) & 0xFF);   /* wraps in its page */
                if (a < sizeof s_flash) s_flash[a] &= tx[i];
            }
        s_flash_wel = false;
        break;
    case 0x20: case 0xD8: {
        uint32_t sz = cmd == 0x20 ? 4096 : 65536, a = addr & ~(sz - 1);
        if (s_flash_wel) for (uint32_t i = 0; i < sz; i++) if (a + i < sizeof s_flash) s_flash[a + i] = 0xFF;
        s_flash_wel = false;
        break;
    }
    case 0xC7: case 0x60:
        if (s_flash_wel) memset(s_flash, 0xFF, sizeof s_flash);
        s_flash_wel = false;
        break;
    default: break;
    }
}

static void tc_xfer(bool open_probe, uint8_t *rx, size_t n)
{
    double t = sim_t();
    double tc = 24.0 + 2.5 * sin(t / 40.0) + jitter(0.15), cj = 25.9 + 0.3 * sin(t / 300.0) + jitter(0.02);
    int32_t tcq = open_probe ? 0 : (int32_t)lround(tc / 0.25), cjq = (int32_t)lround(cj / 0.0625);
    uint32_t w = ((uint32_t)(tcq & 0x3FFF) << 18) | ((uint32_t)(cjq & 0xFFF) << 4);
    if (open_probe) w |= 1u << 16 | 0x01;          /* fault, OC */
    for (size_t i = 0; i < n; i++) rx[i] = i < 4 ? (uint8_t)(w >> (24 - 8 * i)) : 0x00;
}

static double adc_volts(int ch)
{
    double t = sim_t();
    switch (ch) {
    case 0: return 1.65 + 1.3 * sin(t / 8.0);
    case 1: return 1.65 + jitter(0.004);
    case 2: return 3.3 * fmod(t, 30.0) / 30.0;
    case 3: return 0.002 + jitter(0.002);
    case 4: return 3.298 + jitter(0.002);
    case 5: return 2.1 + 0.3 * sin(t / 25.0) + jitter(0.01);
    default: return 1.4 + jitter(1.0);
    }
}

/* Bit by bit, MSB first: DOUT is high-impedance (reads 1) until the start
 * bit and the four configuration bits are in, then one more clock while it
 * samples, a null bit, the ten bits of the result, and zeros after. */
static void adc_xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    int state = 0, cfg = 0, ncfg = 0, out = -1, code = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t r = 0;
        for (int b = 7; b >= 0; b--) {
            int in = (tx[i] >> b) & 1, o = 1;
            if (state == 0) { if (in) state = 1; }
            else if (state == 1) {
                cfg = cfg << 1 | in;
                if (++ncfg == 4) {
                    bool single = cfg & 8;
                    int ch = cfg & 7;
                    double v = single ? adc_volts(ch) : adc_volts(ch) - adc_volts(ch ^ 1);
                    code = (int)lround(v / 3.3 * 1024.0);
                    code = code < 0 ? 0 : code > 1023 ? 1023 : code;
                    state = 2;
                }
            } else if (state == 2) { state = 3; out = 0; }        /* the sample clock */
            else if (state == 3) {
                o = out == 0 ? 0 : (code >> (10 - out)) & 1;      /* null bit, then B9..B0 */
                if (++out > 10) state = 4;
            } else o = 0;
            r = (uint8_t)(r << 1 | o);
        }
        rx[i] = r;
    }
}

static uint8_t rev8(uint8_t v)
{
    v = (uint8_t)((v & 0xF0) >> 4 | (v & 0x0F) << 4);
    v = (uint8_t)((v & 0xCC) >> 2 | (v & 0x33) << 2);
    return (uint8_t)((v & 0xAA) >> 1 | (v & 0x55) << 1);
}

bool aos_io_be_spi_xfer(aos_io_spi_t *s, const uint8_t *tx, uint8_t *rx, size_t n, int timeout_ms)
{
    (void)timeout_ms;
    uint8_t *wire_tx = malloc(n), *wire_rx = malloc(n);
    if (!wire_tx || !wire_rx) { free(wire_tx); free(wire_rx); return false; }
    for (size_t i = 0; i < n; i++) {
        uint8_t b = tx ? tx[i] : 0xFF;
        wire_tx[i] = s->lsb_first ? rev8(b) : b;
    }
    pthread_mutex_lock(&s_spi_mx);
    int chip = chip_for(s);
    uint32_t max_hz = 0;
    bool mode_ok = true;
    switch (chip) {
    case SD_LOOP:   memcpy(wire_rx, wire_tx, n); break;
    case SD_FLASH:  flash_xfer(wire_tx, wire_rx, n); max_hz = 50000000; mode_ok = s->mode == 0 || s->mode == 3; break;
    case SD_TC:
    case SD_TC_OPEN: tc_xfer(chip == SD_TC_OPEN, wire_rx, n); max_hz = 5000000; mode_ok = s->mode == 0; break;
    case SD_ADC:    adc_xfer(wire_tx, wire_rx, n); max_hz = 2000000; mode_ok = s->mode == 0 || s->mode == 3; break;
    default:        memset(wire_rx, 0xFF, n); break;
    }
    pthread_mutex_unlock(&s_spi_mx);
    /* a mode the chip does not speak: the master samples on the edge where
     * the chip changes the line, and reads everything one bit late */
    if (!mode_ok) {
        uint8_t carry = 1;
        for (size_t i = 0; i < n; i++) {
            uint8_t b = wire_rx[i];
            wire_rx[i] = (uint8_t)(carry << 7 | b >> 1);
            carry = b & 1;
        }
    }
    /* too fast for it: the odd bit read wrong */
    if (max_hz && s->clock_hz > max_hz)
        for (size_t i = 0; i < n; i++) if (rand() % 3 == 0) wire_rx[i] ^= (uint8_t)(1u << (rand() % 8));
    for (size_t i = 0; rx && i < n; i++) rx[i] = s->lsb_first ? rev8(wire_rx[i]) : wire_rx[i];
    free(wire_tx);
    free(wire_rx);
    return true;
}

/* ---- 1-Wire: P4_SIM_ONEWIRE says what hangs off any GPIO opened as a bus.
 * Unset: two DS18B20s, one near 23 C and one that warms up and cools
 * down over a minute (a hand on it); "none": nothing; "N": N of them.
 * They answer SKIP ROM, MATCH ROM, CONVERT T, READ/WRITE SCRATCHPAD and
 * COPY SCRATCHPAD, as the bytes come, like the real part; the search is
 * answered whole. ---- */

#define OW_SIM_MAX 4

typedef struct {
    uint64_t rom;
    uint8_t  th, tl, cfg;
    int16_t  raw;               /* the last conversion */
} ow_sim_dev_t;

typedef struct {
    int n;
    ow_sim_dev_t dev[OW_SIM_MAX];
    int sel;                    /* -1 none, -2 all (SKIP ROM), else the index */
    int rom_cmd;                /* waiting for: 0 a ROM command, 1 MATCH's bytes, 2 a function */
    uint8_t match[8];
    int match_n, wr_need, wr_n;
    uint8_t wr[3];
    uint8_t out[9];
    int out_n, out_i;
} ow_sim_t;

static pthread_mutex_t s_ow_mx = PTHREAD_MUTEX_INITIALIZER;

static uint64_t ow_sim_rom(uint8_t fam, uint32_t serial)
{
    uint8_t b[8] = { fam, (uint8_t)serial, (uint8_t)(serial >> 8), (uint8_t)(serial >> 16), 0x80, 0x00, 0x00, 0 };
    b[7] = aos_io_ow_crc8(b, 7);
    uint64_t r = 0;
    for (int i = 7; i >= 0; i--) r = r << 8 | b[i];
    return r;
}

bool aos_io_be_ow_open(aos_io_ow_t *b)
{
    ow_sim_t *s = calloc(1, sizeof *s);
    if (!s) return false;
    const char *e = getenv("P4_SIM_ONEWIRE");
    int n = !e ? 2 : !strcmp(e, "none") ? 0 : atoi(e);
    if (n < 0) n = 0;
    if (n > OW_SIM_MAX) n = OW_SIM_MAX;
    for (int i = 0; i < n; i++) {
        s->dev[i].rom = ow_sim_rom(0x28, 0x5A1C00u + (uint32_t)i * 0x1F3u + (uint32_t)b->gpio);
        s->dev[i].th = 0x4B;
        s->dev[i].tl = 0x46;
        s->dev[i].cfg = 0x7F;           /* 12 bits */
        s->dev[i].raw = 0x0550;         /* 85.0: the power-up value */
    }
    s->n = n;
    s->sel = -1;
    b->be = s;
    return true;
}

static void ow_sim_convert(ow_sim_t *s, int i)
{
    double t = (double)aos_hal_uptime_ms() / 1000.0;
    double c = i == 0 ? 23.0 + 0.4 * sin(t / 37.0) : 24.0 + 6.0 * (0.5 + 0.5 * sin(t / 9.5));
    int bits = ((s->dev[i].cfg >> 5) & 3) + 9;
    int16_t raw = (int16_t)lround(c * 16.0);
    raw &= (int16_t)~((1 << (12 - bits)) - 1);
    s->dev[i].raw = raw;
}

bool aos_io_be_ow_reset(aos_io_ow_t *b)
{
    ow_sim_t *s = b->be;
    pthread_mutex_lock(&s_ow_mx);
    s->sel = -1;
    s->rom_cmd = 0;
    s->out_n = s->out_i = 0;
    s->wr_need = 0;
    bool present = s->n > 0;
    pthread_mutex_unlock(&s_ow_mx);
    return present;
}

int aos_io_be_ow_search(aos_io_ow_t *b, uint64_t *roms, int max)
{
    ow_sim_t *s = b->be;
    int n = 0;
    for (int i = 0; i < s->n && n < max; i++) roms[n++] = s->dev[i].rom;
    return n;
}

bool aos_io_be_ow_write(aos_io_ow_t *b, const uint8_t *data, size_t n)
{
    ow_sim_t *s = b->be;
    pthread_mutex_lock(&s_ow_mx);
    for (size_t k = 0; k < n; k++) {
        uint8_t c = data[k];
        if (s->wr_need) {                       /* WRITE SCRATCHPAD's three bytes */
            s->wr[s->wr_n++] = c;
            if (s->wr_n == 3) {
                for (int i = 0; i < s->n; i++)
                    if (s->sel == -2 || s->sel == i) { s->dev[i].th = s->wr[0]; s->dev[i].tl = s->wr[1]; s->dev[i].cfg = (uint8_t)(s->wr[2] | 0x1F); }
                s->wr_need = 0;
            }
            continue;
        }
        if (s->rom_cmd == 0) {
            if (c == 0xCC) { s->sel = -2; s->rom_cmd = 2; }
            else if (c == 0x55) { s->rom_cmd = 1; s->match_n = 0; }
            continue;
        }
        if (s->rom_cmd == 1) {
            s->match[s->match_n++] = c;
            if (s->match_n == 8) {
                uint64_t r = 0;
                for (int i = 7; i >= 0; i--) r = r << 8 | s->match[i];
                s->sel = -1;
                for (int i = 0; i < s->n; i++) if (s->dev[i].rom == r) s->sel = i;
                s->rom_cmd = 2;
            }
            continue;
        }
        /* a function command for whoever is selected */
        if (c == 0x44) {
            for (int i = 0; i < s->n; i++) if (s->sel == -2 || s->sel == i) ow_sim_convert(s, i);
        } else if (c == 0xBE && s->sel >= 0) {
            ow_sim_dev_t *d = &s->dev[s->sel];
            uint8_t *o = s->out;
            o[0] = (uint8_t)d->raw; o[1] = (uint8_t)(d->raw >> 8); o[2] = d->th; o[3] = d->tl; o[4] = d->cfg;
            o[5] = 0xFF; o[6] = 0x0C; o[7] = 0x10; o[8] = aos_io_ow_crc8(o, 8);
            s->out_n = 9;
            s->out_i = 0;
        } else if (c == 0x4E) {
            s->wr_need = 3;
            s->wr_n = 0;
        }
    }
    pthread_mutex_unlock(&s_ow_mx);
    return true;
}

bool aos_io_be_ow_read(aos_io_ow_t *b, uint8_t *data, size_t n)
{
    ow_sim_t *s = b->be;
    pthread_mutex_lock(&s_ow_mx);
    for (size_t k = 0; k < n; k++) data[k] = s->out_i < s->out_n ? s->out[s->out_i++] : 0xFF;
    pthread_mutex_unlock(&s_ow_mx);
    return true;
}

void aos_io_be_ow_close(aos_io_ow_t *b) { free(b->be); b->be = NULL; }

/* ---- LED strips: nothing on the wire in the simulator; the app's preview
 * draws what the effects engine made. The frame is kept for a peek. ---- */

bool aos_io_be_strip_open(aos_io_strip_t *s) { (void)s; return true; }
bool aos_io_be_strip_send(aos_io_strip_t *s) { (void)s; aos_hal_sleep_ms(1 + s->cfg.count * 30 / 1000); return true; }
void aos_io_be_strip_close(aos_io_strip_t *s) { (void)s; }
