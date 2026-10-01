/*
 * P4OS - The header, the pin owners, the ports and modules of modules.txt.
 * Platform-free: the drivers are in aos_io_p4.c / sim/io_sim.c.
 */
#include "aos_io.h"
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* The header (J3), from the schematic and the Waveshare docs                  */
/* -------------------------------------------------------------------------- */

#define B AOS_PIN_BOARD
#define S AOS_PIN_STRAPPING
#define V AOS_PIN_VO4
#define A AOS_PIN_ADC
#define J AOS_PIN_USB_JTAG
#define R AOS_PIN_RESERVED

static const aos_io_pin_t HEADER[40] = {
    { -1,  1, R,     "5V",     "VCC_5V, live with the board off" },
    { -1,  2, R,     "3V3",    "3 A buck, off with POWER" },
    { -1,  3, R,     "5V",     "" },
    {  7,  4, B,     "GPIO7",  "SDA of the board's I2C bus" },
    { -1,  5, R,     "GND",    "" },
    {  8,  6, B,     "GPIO8",  "SCL of the board's I2C bus" },
    { 37,  7, R | S, "GPIO37", "UART0 TX to the CH343 (console)" },
    {  2,  8, 0,     "GPIO2",  "touch INT if R108 is fitted" },
    { 38,  9, R | S, "GPIO38", "UART0 RX from the CH343 (console)" },
    { -1, 10, R,     "GND",    "" },
    {  5, 11, 0,     "GPIO5",  "JTAG MTDO" },
    {  3, 12, 0,     "GPIO3",  "JTAG MTCK" },
    { -1, 13, R,     "GND",    "" },
    {  4, 14, 0,     "GPIO4",  "JTAG MTDI" },
    { 21, 15, A,     "GPIO21", "ADC1 ch5" },
    { 28, 16, 0,     "GPIO28", "" },
    { 22, 17, A,     "GPIO22", "ADC1 ch6" },
    { -1, 18, R,     "3V3",    "" },
    { -1, 19, R,     "GND",    "" },
    { 29, 20, 0,     "GPIO29", "" },
    { 24, 21, J,     "GPIO24", "USB-Serial-JTAG D-" },
    { 30, 22, 0,     "GPIO30", "" },
    { 25, 23, J,     "GPIO25", "USB-Serial-JTAG D+" },
    { 31, 24, 0,     "GPIO31", "" },
    { -1, 25, R,     "USB D-", "USB 2.0 HS, shared with the OTG port" },
    { -1, 26, R,     "GND",    "" },
    { -1, 27, R,     "USB D+", "USB 2.0 HS, shared with the OTG port" },
    { 34, 28, S,     "GPIO34", "JTAG source strapping" },
    { -1, 29, R,     "GND",    "" },
    { 35, 30, R | S, "GPIO35", "BOOT" },
    { 32, 31, 0,     "GPIO32", "" },
    { 49, 32, A,     "GPIO49", "ADC2 ch0" },
    { -1, 33, R,     "GND",    "" },
    { 50, 34, A,     "GPIO50", "ADC2 ch1" },
    { 46, 35, V,     "GPIO46", "VO4 domain: measure first" },
    { 51, 36, A,     "GPIO51", "ADC2 ch2, comparator" },
    { 47, 37, V,     "GPIO47", "VO4 domain" },
    { 52, 38, A,     "GPIO52", "ADC2 ch3, comparator" },
    { 48, 39, V,     "GPIO48", "VO4 domain" },
    { -1, 40, R,     "GND",    "" },
};
#undef B
#undef S
#undef V
#undef A
#undef J
#undef R

const aos_io_pin_t *aos_io_header(void) { return HEADER; }

const aos_io_pin_t *aos_io_pin_of_gpio(int gpio)
{
    for (int i = 0; i < 40; i++) if (HEADER[i].gpio == gpio) return &HEADER[i];
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Owners                                                                      */
/* -------------------------------------------------------------------------- */

#define MAX_GPIO 55
static char s_owner[MAX_GPIO][24];
static void *s_mutex;

static void lock(void)
{
    if (!s_mutex) s_mutex = aos_hal_mutex_create();
    aos_hal_mutex_lock(s_mutex);
}
static void unlock(void) { aos_hal_mutex_unlock(s_mutex); }

bool aos_io_claim(int gpio, const char *owner)
{
    const aos_io_pin_t *p = aos_io_pin_of_gpio(gpio);
    if (!p || (p->flags & AOS_PIN_RESERVED) || !owner) return false;
    lock();
    bool ok = !s_owner[gpio][0] || !strcmp(s_owner[gpio], owner);
    if (ok) snprintf(s_owner[gpio], sizeof s_owner[gpio], "%s", owner);
    unlock();
    if (!ok) aos_hal_log("io", "GPIO%d is %s's, %s cannot have it", gpio, s_owner[gpio], owner);
    return ok;
}

void aos_io_release(int gpio, const char *owner)
{
    if (gpio < 0 || gpio >= MAX_GPIO) return;
    lock();
    if (owner && !strcmp(s_owner[gpio], owner)) s_owner[gpio][0] = 0;
    unlock();
}

void aos_io_release_owner(const char *owner)
{
    if (!owner) return;
    lock();
    for (int i = 0; i < MAX_GPIO; i++) if (!strcmp(s_owner[i], owner)) s_owner[i][0] = 0;
    unlock();
}

const char *aos_io_owner(int gpio)
{
    return (gpio >= 0 && gpio < MAX_GPIO && s_owner[gpio][0]) ? s_owner[gpio] : NULL;
}

/* -------------------------------------------------------------------------- */
/* modules.txt                                                                 */
/* -------------------------------------------------------------------------- */

static aos_io_port_t   s_ports[AOS_IO_PORT_MAX];
static aos_io_module_t s_mods[AOS_IO_MODULE_MAX];
static int s_nports, s_nmods;
static bool s_loaded;

/* The default profile, EXPANSION.md */
static const char DEFAULT_MODULES[] =
    "port uart.a tx=30 rx=31 de=29 baud=9600\n"
    "port uart.b tx=3 rx=4 baud=115200\n"
    "port i2c.ext sda=21 scl=22 freq=100000\n"
    "port i2c.board sda=7 scl=8 freq=400000\n"
    "port spi.a sck=52 mosi=50 miso=51 cs=49 freq=1000000\n"
    "module target uart.b en=5 boot=2\n";

int32_t aos_io_arg_int(const char *args, const char *key, int32_t def)
{
    size_t kl = strlen(key);
    for (const char *p = args; p && *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, key, kl) && p[kl] == '=') return (int32_t)strtol(p + kl + 1, NULL, 0);
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return def;
}

static bool fail(char *err, size_t n, int line, const char *why)
{
    if (err && n) snprintf(err, n, "line %d: %s", line, why);
    return false;
}

static bool parse(const char *text, size_t len, bool fill, char *err, size_t err_len)
{
    int nports = 0, nmods = 0, line_no = 0;
    size_t pos = 0;
    while (pos < len) {
        char line[160];
        size_t n = 0;
        line_no++;
        while (pos < len && text[pos] != '\n') {
            if (n + 1 >= sizeof line) return fail(err, err_len, line_no, "line too long");
            line[n++] = text[pos++];
        }
        pos++;
        while (n && isspace((unsigned char)line[n - 1])) n--;
        line[n] = 0;
        char *c = line;
        while (*c == ' ' || *c == '\t') c++;
        if (!*c || *c == '#') continue;

        char kw[12], name[AOS_IO_PORT_NAME_MAX + 8];
        int off = 0;
        if (sscanf(c, "%11s %23s %n", kw, name, &off) < 2) return fail(err, err_len, line_no, "expected: port|module <name> ...");
        const char *args = c + off;
        if (!strcmp(kw, "port")) {
            if (nports >= AOS_IO_PORT_MAX) return fail(err, err_len, line_no, "too many ports");
            if (strlen(name) >= AOS_IO_PORT_NAME_MAX) return fail(err, err_len, line_no, "port name too long");
            aos_io_port_t p = { 0 };
            snprintf(p.name, sizeof p.name, "%.*s", (int)sizeof p.name - 1, name);
            memset(p.pins, -1, sizeof p.pins);
            const char *keys[4];
            if (!strncmp(name, "uart", 4)) {
                p.kind = AOS_PORT_UART; keys[0] = "tx"; keys[1] = "rx"; keys[2] = "de"; keys[3] = NULL;
                p.freq = aos_io_arg_int(args, "baud", 115200);
            } else if (!strncmp(name, "i2c", 3)) {
                p.kind = AOS_PORT_I2C; keys[0] = "sda"; keys[1] = "scl"; keys[2] = keys[3] = NULL;
                p.freq = aos_io_arg_int(args, "freq", 100000);
            } else if (!strncmp(name, "spi", 3)) {
                p.kind = AOS_PORT_SPI; keys[0] = "sck"; keys[1] = "mosi"; keys[2] = "miso"; keys[3] = "cs";
                p.freq = aos_io_arg_int(args, "freq", 1000000);
            } else if (!strncmp(name, "gpio", 4)) {
                p.kind = AOS_PORT_GPIO; keys[0] = "pin"; keys[1] = keys[2] = keys[3] = NULL;
            } else {
                return fail(err, err_len, line_no, "port names start with uart, i2c, spi or gpio");
            }
            for (int k = 0; k < 4 && keys[k]; k++) {
                int g = aos_io_arg_int(args, keys[k], -1);
                if (g >= 0) {
                    const aos_io_pin_t *pin = aos_io_pin_of_gpio(g);
                    if (!pin) return fail(err, err_len, line_no, "a GPIO that is not on the header");
                    if ((pin->flags & AOS_PIN_RESERVED)) return fail(err, err_len, line_no, "a reserved pin (console, BOOT)");
                    p.pins[k] = (int8_t)g;
                } else if (k < 2) {
                    return fail(err, err_len, line_no, "missing pin");
                }
            }
            for (int i = 0; i < nports; i++)
                if (!strcmp(s_ports[i].name, p.name) && fill) return fail(err, err_len, line_no, "two ports with the same name");
            if (fill) s_ports[nports] = p;
            nports++;
        } else if (!strcmp(kw, "module")) {
            if (nmods >= AOS_IO_MODULE_MAX) return fail(err, err_len, line_no, "too many modules");
            char port[AOS_IO_PORT_NAME_MAX];
            int off2 = 0;
            if (sscanf(args, "%15s %n", port, &off2) < 1) return fail(err, err_len, line_no, "module needs a port");
            if (fill) {
                aos_io_module_t *m = &s_mods[nmods];
                snprintf(m->name, sizeof m->name, "%.*s", (int)sizeof m->name - 1, name);
                snprintf(m->port, sizeof m->port, "%s", port);
                snprintf(m->args, sizeof m->args, "%.*s", (int)sizeof m->args - 1, args + off2);
            }
            nmods++;
        } else {
            return fail(err, err_len, line_no, "unknown line (port, module)");
        }
    }
    if (fill) { s_nports = nports; s_nmods = nmods; }
    return true;
}

bool aos_io_validate(const char *text, size_t len, char *err, size_t err_len)
{
    return parse(text, len, false, err, err_len);
}

int aos_io_load(void)
{
    char path[256];
    const char *root = aos_hal_path_sd_root();
    s_loaded = true;
    if (root) {
        snprintf(path, sizeof path, "%s/modules.txt", root);
        FILE *f = fopen(path, "rb");
        if (f) {
            char *buf = malloc(8192);
            size_t n = buf ? fread(buf, 1, 8192, f) : 0;
            fclose(f);
            char err[64] = "";
            bool ok = buf && parse(buf, n, true, err, sizeof err);
            free(buf);
            if (ok) {
                aos_hal_log("io", "%s: %d ports, %d modules", path, s_nports, s_nmods);
                return s_nports;
            }
            aos_hal_log("io", "%s ignored, %s", path, err);
        }
    }
    parse(DEFAULT_MODULES, sizeof DEFAULT_MODULES - 1, true, NULL, 0);
    return s_nports;
}

static void ensure(void) { if (!s_loaded) aos_io_load(); }

int aos_io_port_count(void) { ensure(); return s_nports; }
const aos_io_port_t *aos_io_port_at(int i) { ensure(); return (i >= 0 && i < s_nports) ? &s_ports[i] : NULL; }
int aos_io_module_count(void) { ensure(); return s_nmods; }
const aos_io_module_t *aos_io_module_at(int i) { ensure(); return (i >= 0 && i < s_nmods) ? &s_mods[i] : NULL; }

const aos_io_port_t *aos_io_port_find(const char *name)
{
    ensure();
    for (int i = 0; name && i < s_nports; i++) if (!strcmp(s_ports[i].name, name)) return &s_ports[i];
    return NULL;
}

const aos_io_module_t *aos_io_module_find(const char *name)
{
    ensure();
    for (int i = 0; name && i < s_nmods; i++) if (!strcmp(s_mods[i].name, name)) return &s_mods[i];
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Opening ports: claim the pins, then the backend                             */
/* -------------------------------------------------------------------------- */

static bool claim_pins(const aos_io_port_t *p, const char *owner)
{
    if (!strcmp(p->name, "i2c.board")) return true;         /* shared bus, the board's own */
    for (int k = 0; k < 4; k++) {
        if (p->pins[k] < 0) continue;
        if (!aos_io_claim(p->pins[k], owner)) {
            for (int j = 0; j < k; j++) if (p->pins[j] >= 0) aos_io_release(p->pins[j], owner);
            return false;
        }
    }
    return true;
}

static void release_pins(const aos_io_port_t *p, const char *owner)
{
    if (!strcmp(p->name, "i2c.board")) return;
    for (int k = 0; k < 4; k++) if (p->pins[k] >= 0) aos_io_release(p->pins[k], owner);
}

aos_io_uart_t *aos_io_uart_open(const char *port, uint32_t baud, bool rs485, const char *owner)
{
    const aos_io_port_t *p = aos_io_port_find(port);
    if (!p || p->kind != AOS_PORT_UART) { aos_hal_log("io", "no UART port %s", port ? port : "?"); return NULL; }
    if (!claim_pins(p, owner)) return NULL;
    aos_io_uart_t *u = calloc(1, sizeof *u);
    u->port = p;
    snprintf(u->owner, sizeof u->owner, "%s", owner);
    u->baud = baud ? baud : p->freq;
    snprintf(u->desc, sizeof u->desc, "%s TX%d RX%d", p->name, p->pins[0], p->pins[1]);
    int en, boot;
    aos_io_port_lines(p->name, &en, &boot);
    u->en_gpio = (int8_t)en;
    u->boot_gpio = (int8_t)boot;
    u->en = u->boot = -1;
    if (!aos_io_be_uart_open(u, rs485 && p->pins[2] >= 0)) {
        release_pins(p, owner);
        free(u);
        return NULL;
    }
    return u;
}

int aos_io_uart_read(aos_io_uart_t *u, void *buf, int len, int timeout_ms) { return u ? aos_io_be_uart_read(u, buf, len, timeout_ms) : -1; }
int aos_io_uart_write(aos_io_uart_t *u, const void *buf, int len) { return u ? aos_io_be_uart_write(u, buf, len) : -1; }

bool aos_io_uart_set_baud(aos_io_uart_t *u, uint32_t baud)
{
    if (!u || !aos_io_be_uart_set_baud(u, baud)) return false;
    u->baud = baud;
    return true;
}

bool aos_io_uart_set_format(aos_io_uart_t *u, char parity, int stop_bits)
{
    if (!u || (parity != 'N' && parity != 'E' && parity != 'O') || (stop_bits != 1 && stop_bits != 2)) return false;
    return aos_io_be_uart_set_format(u, parity, stop_bits);
}

/* The target's EN and BOOT: whatever the modules on this port say (the
 * default profile has "module target uart.b en=5 boot=2"). A pin that is
 * not on the header, or is one of the reserved ones, does not count. */
bool aos_io_port_lines(const char *port, int *en_gpio, int *boot_gpio)
{
    int en = -1, boot = -1;
    for (int i = 0; port && i < aos_io_module_count(); i++) {
        const aos_io_module_t *m = aos_io_module_at(i);
        if (strcmp(m->port, port)) continue;
        if (en < 0) en = aos_io_arg_int(m->args, "en", -1);
        if (boot < 0) boot = aos_io_arg_int(m->args, "boot", -1);
    }
    const aos_io_pin_t *pe = en >= 0 ? aos_io_pin_of_gpio(en) : NULL;
    const aos_io_pin_t *pb = boot >= 0 ? aos_io_pin_of_gpio(boot) : NULL;
    if (en >= 0 && (!pe || (pe->flags & AOS_PIN_RESERVED))) { aos_hal_log("io", "%s: en=%d is not a usable pin", port, en); en = -1; }
    if (boot >= 0 && (!pb || (pb->flags & AOS_PIN_RESERVED))) { aos_hal_log("io", "%s: boot=%d is not a usable pin", port, boot); boot = -1; }
    if (en_gpio) *en_gpio = en;
    if (boot_gpio) *boot_gpio = boot;
    return en >= 0 || boot >= 0;
}

bool aos_io_uart_lines(aos_io_uart_t *u, int en, int boot)
{
    if (!u || (u->en_gpio < 0 && u->boot_gpio < 0)) return false;
    if (!u->lines_claimed) {
        /* the pins become the port owner's the first time they are wanted,
         * not on open: a Terminal on the same port never touches them */
        if (u->en_gpio >= 0 && !aos_io_claim(u->en_gpio, u->owner)) return false;
        if (u->boot_gpio >= 0 && !aos_io_claim(u->boot_gpio, u->owner)) {
            if (u->en_gpio >= 0) aos_io_release(u->en_gpio, u->owner);
            return false;
        }
        u->lines_claimed = true;
    }
    if (u->en_gpio < 0) en = -1;
    if (u->boot_gpio < 0) boot = -1;
    if (en < 0 && boot < 0) return true;
    if (en >= 0) en = en ? 1 : 0;
    if (boot >= 0) boot = boot ? 1 : 0;
    if (!aos_io_be_uart_lines(u, en, boot)) return false;
    if (en >= 0) u->en = (int8_t)en;
    if (boot >= 0) u->boot = (int8_t)boot;
    return true;
}

void aos_io_uart_close(aos_io_uart_t *u)
{
    if (!u) return;
    if (u->lines_claimed) {
        aos_io_be_uart_lines_release(u);
        if (u->en_gpio >= 0) aos_io_release(u->en_gpio, u->owner);
        if (u->boot_gpio >= 0) aos_io_release(u->boot_gpio, u->owner);
    }
    aos_io_be_uart_close(u);
    release_pins(u->port, u->owner);
    free(u);
}

const char *aos_io_uart_desc(aos_io_uart_t *u) { return u ? u->desc : ""; }

aos_io_i2c_t *aos_io_i2c_open(const char *port, const char *owner)
{
    const aos_io_port_t *p = aos_io_port_find(port);
    if (!p || p->kind != AOS_PORT_I2C) return NULL;
    if (!claim_pins(p, owner)) return NULL;
    aos_io_i2c_t *b = calloc(1, sizeof *b);
    b->port = p;
    snprintf(b->owner, sizeof b->owner, "%s", owner);
    if (!aos_io_be_i2c_open(b)) {
        release_pins(p, owner);
        free(b);
        return NULL;
    }
    return b;
}

bool aos_io_i2c_probe(aos_io_i2c_t *b, uint8_t addr) { return b && aos_io_be_i2c_probe(b, addr); }

bool aos_io_i2c_xfer(aos_io_i2c_t *b, uint8_t addr, const void *w, size_t wn, void *r, size_t rn, int timeout_ms)
{
    return b && aos_io_be_i2c_xfer(b, addr, w, wn, r, rn, timeout_ms);
}

void aos_io_i2c_close(aos_io_i2c_t *b)
{
    if (!b) return;
    aos_io_be_i2c_close(b);
    release_pins(b->port, b->owner);
    free(b);
}

const char *aos_io_i2c_guess(uint8_t a, bool board)
{
    if (board) {
        if (a == 0x14 || a == 0x5D) return "GT911 (touch)";
        if (a == 0x18) return "ES8311 (speaker codec)";
        if (a == 0x40) return "ES7210 (microphones)";
        if (a == 0x36) return "OV5647 (camera)";
    }
    switch (a) {
    case 0x3C: case 0x3D: return "SSD1306 / SH1106 OLED";
    case 0x20: case 0x21: case 0x22: case 0x24: case 0x25: case 0x26: case 0x27: return "PCF8574 / MCP23017";
    case 0x23: return "BH1750 / PCF8574";
    case 0x38: return "AHT10 / AHT20 / PCF8574A";
    case 0x39: case 0x3A: case 0x3B: case 0x3E: case 0x3F: return "PCF8574A";
    case 0x40: return "HTU21 / SHT21 / Si7021 / INA219";
    case 0x41: case 0x44: case 0x45: return "SHT3x / SHT4x / INA219";
    case 0x48: case 0x49: case 0x4A: case 0x4B: return "ADS1115 / LM75 / TMP102";
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: return "AT24Cxx EEPROM";
    case 0x5A: return "CCS811 / MLX90614";
    case 0x62: return "SCD40 / SCD41";
    case 0x68: case 0x69: return "DS3231 / DS1307 / MPU6050";
    case 0x76: case 0x77: return "BME280 / BMP280 / BME680";
    default: return "";
    }
}

/* -------------------------------------------------------------------------- */
/* SPI                                                                         */
/* -------------------------------------------------------------------------- */

/* The open handles, so that a port opened twice (two chips on the same
 * wires, each with its own CS) keeps SCK, MOSI and MISO claimed - and the
 * backend's bus up - until the last of them closes. Each handle carries a
 * copy of its port: modules.txt may be loaded again while it is open. */
#define SPI_OPEN_MAX 8
static aos_io_spi_t *s_spi_open[SPI_OPEN_MAX];

static int spi_users(const char *port, const aos_io_spi_t *except)
{
    int n = 0;
    for (int i = 0; i < SPI_OPEN_MAX; i++)
        if (s_spi_open[i] && s_spi_open[i] != except && !strcmp(s_spi_open[i]->port->name, port)) n++;
    return n;
}

static bool cs_in_use(int gpio, const aos_io_spi_t *except)
{
    for (int i = 0; i < SPI_OPEN_MAX; i++)
        if (s_spi_open[i] && s_spi_open[i] != except && s_spi_open[i]->cs == gpio) return true;
    return false;
}

static bool usable_pin(int gpio)
{
    const aos_io_pin_t *p = aos_io_pin_of_gpio(gpio);
    return p && !(p->flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD));
}

int aos_io_module_gpio(const char *module, const char *key)
{
    const aos_io_module_t *m = aos_io_module_find(module);
    if (!m || !key) return -1;
    int g = aos_io_arg_int(m->args, key, -1);
    if (g < 0) return -1;
    if (!usable_pin(g)) {
        aos_hal_log("io", "%s: %s=%d is not a usable pin of the header", module, key, g);
        return -1;
    }
    return g;
}

static void fmt_hz(char *out, size_t n, uint32_t hz)
{
    if (hz >= 1000000 && hz % 1000000 == 0) snprintf(out, n, "%u MHz", (unsigned)(hz / 1000000));
    else if (hz >= 1000000) snprintf(out, n, "%.2f MHz", (double)(hz / 1e6f));
    else snprintf(out, n, "%u kHz", (unsigned)((hz + 500) / 1000));
}

static void spi_desc(aos_io_spi_t *s)
{
    char hz[16], cs[12];
    fmt_hz(hz, sizeof hz, s->actual_hz ? s->actual_hz : s->clock_hz);
    if (s->cs >= 0) snprintf(cs, sizeof cs, "CS%d", s->cs);
    else snprintf(cs, sizeof cs, "no CS");
    /* bounded so the 48 bytes always hold it: port names are short */
    snprintf(s->desc, sizeof s->desc, "%.12s %.8s %.10s mode %u%s", s->port->name, cs, hz, (unsigned)s->mode,
             s->lsb_first ? " LSB" : "");
}

static uint32_t clamp_hz(uint32_t hz)
{
    if (hz < AOS_IO_SPI_MIN_HZ) return AOS_IO_SPI_MIN_HZ;
    if (hz > AOS_IO_SPI_MAX_HZ) return AOS_IO_SPI_MAX_HZ;
    return hz;
}

/* The wires of the port (not its CS: each handle claims the one it uses). */
static bool spi_claim_wires(const aos_io_port_t *p, const char *owner)
{
    for (int k = 0; k < 3; k++) {
        if (p->pins[k] < 0 || aos_io_claim(p->pins[k], owner)) continue;
        for (int j = 0; j < k; j++) if (p->pins[j] >= 0) aos_io_release(p->pins[j], owner);
        return false;
    }
    return true;
}

static void spi_release_wires(const aos_io_port_t *p, const char *owner)
{
    for (int k = 0; k < 3; k++) if (p->pins[k] >= 0) aos_io_release(p->pins[k], owner);
}

aos_io_spi_t *aos_io_spi_open(const char *port, const char *owner, const aos_io_spi_cfg_t *cfg)
{
    static const aos_io_spi_cfg_t DEF = { 0 };
    if (!cfg) cfg = &DEF;
    const aos_io_port_t *p = aos_io_port_find(port);
    if (!p || p->kind != AOS_PORT_SPI) {
        aos_hal_log("io", "no SPI port %s in modules.txt", port ? port : "?");
        return NULL;
    }
    if (!owner || !owner[0] || cfg->mode > 3) return NULL;

    /* which CS: an explicit GPIO, none, a module's, or the port's own */
    int cs = -1;
    if (cfg->cs > 0) {
        cs = cfg->cs;
        if (!usable_pin(cs)) { aos_hal_log("io", "%s: CS GPIO%d is not a usable pin of the header", p->name, cs); return NULL; }
    } else if (cfg->cs == AOS_IO_SPI_CS_PORT) {
        if (cfg->module && cfg->module[0]) {
            const aos_io_module_t *m = aos_io_module_find(cfg->module);
            if (!m || strcmp(m->port, p->name)) {
                aos_hal_log("io", "no module %s on %s in modules.txt", cfg->module, p->name);
                return NULL;
            }
            cs = aos_io_module_gpio(cfg->module, "cs");
            if (cs < 0) { aos_hal_log("io", "module %s has no usable cs=", cfg->module); return NULL; }
        } else {
            cs = p->pins[3];
        }
    } else if (cfg->cs != AOS_IO_SPI_CS_NONE) {
        return NULL;
    }
    for (int k = 0; k < 3; k++)
        if (cs >= 0 && p->pins[k] == cs) { aos_hal_log("io", "%s: CS GPIO%d is one of the port's wires", p->name, cs); return NULL; }

    aos_io_spi_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->pcopy = *p;
    s->port = &s->pcopy;
    snprintf(s->owner, sizeof s->owner, "%s", owner);
    s->cs = (int8_t)cs;
    s->mode = cfg->mode;
    s->lsb_first = cfg->lsb_first;
    s->clock_hz = clamp_hz(cfg->clock_hz ? cfg->clock_hz : p->freq ? p->freq : AOS_IO_SPI_DEFAULT_HZ);

    /* into the table first, so that two opens at once agree on which one
     * brings the bus up */
    lock();
    int slot = -1;
    for (int i = 0; i < SPI_OPEN_MAX; i++) if (!s_spi_open[i]) { slot = i; break; }
    bool first = spi_users(p->name, NULL) == 0;
    bool cs_shared = cs >= 0 && cs_in_use(cs, NULL);
    if (slot >= 0) s_spi_open[slot] = s;
    unlock();
    if (slot < 0) { aos_hal_log("io", "too many SPI devices open"); free(s); return NULL; }

    bool wires = false, cs_held = false;
    if (!(wires = spi_claim_wires(p, owner))) goto fail;
    if (cs >= 0 && !(cs_held = aos_io_claim(cs, owner))) goto fail;
    if (!aos_io_be_spi_open(s, first)) goto fail;
    spi_desc(s);
    return s;

fail:
    lock();
    s_spi_open[slot] = NULL;
    unlock();
    if (cs_held && !cs_shared) aos_io_release(cs, owner);
    if (wires && first) spi_release_wires(p, owner);
    free(s);
    return NULL;
}

bool aos_io_spi_xfer(aos_io_spi_t *s, const void *tx, void *rx, size_t n, int timeout_ms)
{
    if (!s) return false;
    if (!n) return true;
    return aos_io_be_spi_xfer(s, tx, rx, n, timeout_ms);
}

bool aos_io_spi_write_read(aos_io_spi_t *s, const void *w, size_t wn, void *r, size_t rn)
{
    if (!s) return false;
    size_t n = wn + rn;
    if (!n) return true;
    /* one buffer each way, w then 0xFF out, and the tail of what comes back
     * kept: the bench's register reads fit on the stack, anything longer
     * goes to the heap (PSRAM from 1 KB up, docs/MEMORY.md) */
    uint8_t stack_tx[64], stack_rx[64];
    uint8_t *tx = n <= sizeof stack_tx ? stack_tx : malloc(n);
    uint8_t *rx = n <= sizeof stack_rx ? stack_rx : malloc(n);
    bool ok = tx && rx;
    if (ok) {
        if (wn) memcpy(tx, w, wn);
        memset(tx + wn, 0xFF, rn);
        ok = aos_io_be_spi_xfer(s, tx, rx, n, 1000);       /* a second for the wires, as the I2C reads */
        if (ok && r && rn) memcpy(r, rx + wn, rn);
    }
    if (tx != stack_tx) free(tx);
    if (rx != stack_rx) free(rx);
    return ok;
}

bool aos_io_spi_set_clock(aos_io_spi_t *s, uint32_t hz)
{
    if (!s) return false;
    hz = clamp_hz(hz);
    if (!aos_io_be_spi_set_clock(s, hz)) return false;
    s->clock_hz = hz;
    spi_desc(s);
    return true;
}

uint32_t aos_io_spi_clock(aos_io_spi_t *s) { return s ? (s->actual_hz ? s->actual_hz : s->clock_hz) : 0; }
int aos_io_spi_cs_gpio(aos_io_spi_t *s) { return s ? s->cs : -1; }
const char *aos_io_spi_desc(aos_io_spi_t *s) { return s ? s->desc : ""; }

void aos_io_spi_close(aos_io_spi_t *s)
{
    if (!s) return;
    lock();
    for (int i = 0; i < SPI_OPEN_MAX; i++) if (s_spi_open[i] == s) s_spi_open[i] = NULL;
    bool last = spi_users(s->port->name, NULL) == 0;
    bool cs_free = s->cs >= 0 && !cs_in_use(s->cs, NULL);
    unlock();
    aos_io_be_spi_close(s, last);
    if (cs_free) aos_io_release(s->cs, s->owner);
    if (last) spi_release_wires(s->port, s->owner);
    free(s);
}

/* -------------------------------------------------------------------------- */
/* GPIO                                                                        */
/* -------------------------------------------------------------------------- */

bool aos_io_gpio_mode(int gpio, aos_gpio_mode_t mode, const char *owner)
{
    if (!aos_io_claim(gpio, owner)) return false;
    return aos_io_be_gpio_mode(gpio, mode);
}

int aos_io_gpio_get(int gpio) { return aos_io_pin_of_gpio(gpio) ? aos_io_be_gpio_get(gpio) : -1; }
bool aos_io_gpio_set(int gpio, int level) { return aos_io_pin_of_gpio(gpio) && aos_io_be_gpio_set(gpio, level); }
