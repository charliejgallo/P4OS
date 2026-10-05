/*
 * P4OS - The rear header, its pins and the modules on it (EXPANSION.md).
 *
 * Three layers:
 *
 *   pins     the 40-pin header as the board has it, and who holds each GPIO
 *            (aos_io_claim). Two users can never drive the same pin; the
 *            refusal says who has it.
 *   ports    named groups of pins - "uart.a", "i2c.ext", "spi.a" - from
 *            modules.txt on the card, or the default profile when there is
 *            none. Apps open a PORT, never a GPIO number, so moving a module
 *            to other pins is an edit of modules.txt, not of the app.
 *   modules  what is connected to a port ("rs485 on uart.a at 9600"): a
 *            name an app can ask for.
 *
 * The same API runs on the board (aos_io_p4.c, ESP-IDF drivers) and in the
 * simulator (sim/io_sim.c: a port can be a real serial adapter on the Mac,
 * P4_SIM_UART_A=/dev/cu.usbserial-110).
 *
 * Thread-safety: the open/close and claim calls take an internal lock; a
 * handle is used by one task at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the header ---- */

typedef enum {
    AOS_PIN_FREE      = 0,
    AOS_PIN_BOARD     = 1u << 0,    /* shared with the board (I2C bus, console) */
    AOS_PIN_STRAPPING = 1u << 1,    /* read at reset: mind what hangs off it     */
    AOS_PIN_VO4       = 1u << 2,    /* VDD_IO_5 domain, powered by the LDO VO4   */
    AOS_PIN_ADC       = 1u << 3,
    AOS_PIN_USB_JTAG  = 1u << 4,    /* the USB-Serial-JTAG pair, free if unused  */
    AOS_PIN_RESERVED  = 1u << 5,    /* never offered (console, BOOT)             */
} aos_pin_flags_t;

typedef struct {
    int8_t      gpio;               /* -1: a power or ground pin */
    uint8_t     pin;                /* physical pin, 1..40 */
    uint8_t     flags;              /* aos_pin_flags_t */
    const char *label;              /* "GPIO30", "5V", "GND"... */
    const char *note;
} aos_io_pin_t;

/* All 40 pins, in physical order (pin 1 = 5 V, pin 2 = 3V3). */
const aos_io_pin_t *aos_io_header(void);
const aos_io_pin_t *aos_io_pin_of_gpio(int gpio);   /* NULL: not on the header */

/* ---- who holds a pin ---- */

bool        aos_io_claim(int gpio, const char *owner);   /* false: taken or not free */
void        aos_io_release(int gpio, const char *owner);
void        aos_io_release_owner(const char *owner);     /* all of an owner's pins */
const char *aos_io_owner(int gpio);                      /* NULL: nobody */

/* ---- ports and modules (modules.txt) ---- */

typedef enum { AOS_PORT_UART = 0, AOS_PORT_I2C, AOS_PORT_SPI, AOS_PORT_GPIO } aos_port_kind_t;

#define AOS_IO_PORT_NAME_MAX 16
#define AOS_IO_PORT_MAX      12
#define AOS_IO_MODULE_MAX    16

typedef struct {
    char            name[AOS_IO_PORT_NAME_MAX];     /* "uart.a" */
    aos_port_kind_t kind;
    /* UART: tx rx de(-1) | I2C: sda scl | SPI: sck mosi miso cs(-1) | GPIO: pins[0] */
    int8_t          pins[4];
    uint32_t        freq;                           /* I2C/SPI clock, UART default baud */
} aos_io_port_t;

typedef struct {
    char    name[24];                               /* "rs485", "bme280" */
    char    port[AOS_IO_PORT_NAME_MAX];
    char    args[96];                               /* the rest of the line: "baud=9600 addr=0x76" */
} aos_io_module_t;

/* Reads modules.txt again (aos_hal_path_sd_root()/modules.txt). With no file,
 * the default profile of EXPANSION.md. Returns the number of ports. */
int aos_io_load(void);

int                    aos_io_port_count(void);
const aos_io_port_t   *aos_io_port_at(int i);
const aos_io_port_t   *aos_io_port_find(const char *name);
int                    aos_io_module_count(void);
const aos_io_module_t *aos_io_module_at(int i);
const aos_io_module_t *aos_io_module_find(const char *name);
/* "baud=9600 addr=0x76": the value of one key, or def. */
int32_t aos_io_arg_int(const char *args, const char *key, int32_t def);

/* Checks a modules.txt before it is written (the portal, Settings). */
bool aos_io_validate(const char *text, size_t len, char *err, size_t err_len);

/* ---- UART ---- */

typedef struct aos_io_uart aos_io_uart_t;

/* Opens a UART port, claiming its pins for 'owner'. rs485: drive DE (the
 * port's de pin) from the UART itself, half duplex. */
aos_io_uart_t *aos_io_uart_open(const char *port, uint32_t baud, bool rs485, const char *owner);
int  aos_io_uart_read(aos_io_uart_t *u, void *buf, int len, int timeout_ms);   /* bytes, 0 on timeout, <0 error */
int  aos_io_uart_write(aos_io_uart_t *u, const void *buf, int len);
bool aos_io_uart_set_baud(aos_io_uart_t *u, uint32_t baud);
/* 8 data bits always; parity 'N', 'E' or 'O', 1 or 2 stop bits. A port
 * opens as 8N1. Modbus RTU wants 8E1 by the book, and many meters do. */
bool aos_io_uart_set_format(aos_io_uart_t *u, char parity, int stop_bits);
/* The two lines a target board resets and boots with (programmer): its EN
 * (reset, low = held in reset) and its BOOT/GPIO0 (low while EN rises =
 * the ROM's download mode). Levels as the target sees them, 1/0; -1 leaves
 * a line alone. When both change in one call, BOOT goes first, so an EN
 * rising in the same call already finds BOOT where it was asked to be.
 *
 * On the board they are the GPIOs of the module on this port in
 * modules.txt ("module target uart.b en=5 boot=2"), claimed for the port's
 * owner on first use (Settings > Expansion shows them under that name),
 * driven open-drain - 0 pulls the line low, 1 lets go and the target's own
 * pull-up has it - and let go again on close, so a target left alone runs.
 * In the simulator, with the port on a real adapter (P4_SIM_UART_B=
 * /dev/cu.xxx), EN is RTS and BOOT is DTR, the way esptool drives the
 * usual two-transistor auto-reset circuit (asserted = low at the chip).
 *
 * false: the port has no such lines (no module on it with en= or boot=),
 * or a pin is somebody else's. aos_io_uart_lines(u, -1, -1) just asks. */
bool aos_io_uart_lines(aos_io_uart_t *u, int en, int boot);
/* A USB port that is an ESP32-C3/C6/S3/H2's own USB-Serial-JTAG: its reset
 * into the ROM is a sequence of DTR and RTS that the chip's USB logic
 * decodes (no transistors), driven raw with aos_io_uart_dtr_rts. */
bool aos_io_uart_is_usb_jtag(aos_io_uart_t *u);
bool aos_io_uart_dtr_rts(aos_io_uart_t *u, bool dtr, bool rts);
/* The EN and BOOT GPIOs modules.txt gives a port, -1 for a missing one;
 * false when it gives neither. For showing them, before opening anything. */
bool aos_io_port_lines(const char *port, int *en_gpio, int *boot_gpio);
void aos_io_uart_close(aos_io_uart_t *u);
const char *aos_io_uart_desc(aos_io_uart_t *u);  /* "uart.a TX30 RX31" or the Mac device */

/* ---- I2C ---- */

typedef struct aos_io_i2c aos_io_i2c_t;
aos_io_i2c_t *aos_io_i2c_open(const char *port, const char *owner);      /* "i2c.ext", "i2c.board" */
bool aos_io_i2c_probe(aos_io_i2c_t *b, uint8_t addr);
bool aos_io_i2c_xfer(aos_io_i2c_t *b, uint8_t addr, const void *w, size_t wn, void *r, size_t rn, int timeout_ms);
void aos_io_i2c_close(aos_io_i2c_t *b);
/* What usually lives at an address, for scanners ("BME280 / BMP280"). */
const char *aos_io_i2c_guess(uint8_t addr, bool board_bus);

/* ---- SPI ----
 *
 * A device on an SPI port: the port gives SCK, MOSI, MISO and, usually, a
 * chip select ("port spi.a sck=52 mosi=50 miso=51 cs=49 freq=1000000"). A
 * second device on the same wires gets its own CS from a module line on the
 * port - "module lora spi.a cs=48 rst=46 busy=47" - and is opened with
 * cfg.module = "lora". The same owner may open one port several times, one
 * handle per device; the wires stay claimed until the last one closes.
 *
 * Every aos_io_spi_xfer() is one CS assertion, however long: CS goes low,
 * n bytes go out on MOSI while n come in on MISO, CS goes high. The chips on
 * a bench almost all want "command, then read": aos_io_spi_write_read() does
 * that in one assertion, which is what a register read needs (two separate
 * xfers would let CS go up in between and the chip forget the command).
 *
 * On the board the port is a GPSPI host through the GPIO matrix, so the
 * clock is held to AOS_IO_SPI_MAX_HZ; the driver rounds to what the divider
 * can do (aos_io_spi_desc() says what it got). MISO has the internal pull-up
 * on: with nothing connected it reads 0xFF, not noise. In the simulator the
 * port talks to emulated chips (a W25Q128 flash, a MAX31855, an MCP3008) or
 * loops MOSI back to MISO; see sim/io_sim.c and docs/MODULES.md. */

#define AOS_IO_SPI_MAX_HZ      40000000u
#define AOS_IO_SPI_MIN_HZ      10000u
#define AOS_IO_SPI_DEFAULT_HZ  1000000u

#define AOS_IO_SPI_CS_PORT     0        /* the port's cs= (or the module's) */
#define AOS_IO_SPI_CS_NONE     (-1)     /* no CS: the app drives one itself with aos_io_gpio_* */

typedef struct {
    uint32_t    clock_hz;   /* 0: the port's freq= in modules.txt, else AOS_IO_SPI_DEFAULT_HZ */
    uint8_t     mode;       /* 0..3: CPOL << 1 | CPHA, as in every datasheet */
    bool        lsb_first;  /* false: most significant bit first, as nearly every chip wants */
    int8_t      cs;         /* AOS_IO_SPI_CS_PORT, AOS_IO_SPI_CS_NONE or a GPIO of the header */
    const char *module;     /* with cs = AOS_IO_SPI_CS_PORT: take CS from this module's cs= */
} aos_io_spi_cfg_t;

typedef struct aos_io_spi aos_io_spi_t;

/* Opens a device on an SPI port for 'owner'; cfg NULL is all defaults (the
 * port's CS and clock, mode 0, MSB first). NULL when the port is not in
 * modules.txt or is not SPI, when a pin is someone else's (the log says
 * whose: "GPIO50 is Bus's, lora cannot have it"), when the CS asked for is
 * not a usable header pin, or when the board has no SPI host left. */
aos_io_spi_t *aos_io_spi_open(const char *port, const char *owner, const aos_io_spi_cfg_t *cfg);
/* Full duplex, one CS assertion. tx NULL sends 0xFF bytes (MOSI idle high:
 * what SD cards, flashes and most sensors expect while they talk); rx NULL
 * throws the received bytes away. timeout_ms bounds the wait for the wires
 * when another handle on the same port is in the middle of a transfer; a
 * transfer that has started always ends (the master makes the clock). */
bool aos_io_spi_xfer(aos_io_spi_t *s, const void *tx, void *rx, size_t n, int timeout_ms);
/* wn bytes out, then rn bytes in (sending 0xFF), in one CS assertion. What
 * comes back while w goes out is dropped. Waits up to a second for the
 * wires, as aos_io_spi_xfer() with timeout_ms 1000. */
bool aos_io_spi_write_read(aos_io_spi_t *s, const void *w, size_t wn, void *r, size_t rn);
/* A new clock for the next transfers, clamped to MIN..MAX_HZ. */
bool aos_io_spi_set_clock(aos_io_spi_t *s, uint32_t hz);
uint32_t aos_io_spi_clock(aos_io_spi_t *s);      /* as the hardware makes it */
int  aos_io_spi_cs_gpio(aos_io_spi_t *s);        /* the CS in use, -1 none */
void aos_io_spi_close(aos_io_spi_t *s);
const char *aos_io_spi_desc(aos_io_spi_t *s);    /* "spi.a CS49 1 MHz mode 0" */

/* A GPIO named in a module line - "module lora spi.a cs=48 rst=46" with
 * key "rst" gives 46 - checked to be a usable pin of the header; -1 when
 * the module, the key or the pin is missing. For a module's extra lines
 * (reset, busy, an interrupt), which the app then claims with
 * aos_io_gpio_mode(). */
int aos_io_module_gpio(const char *module, const char *key);

/* ---- GPIO ---- */

typedef enum { AOS_GPIO_INPUT = 0, AOS_GPIO_INPUT_PULLUP, AOS_GPIO_INPUT_PULLDOWN, AOS_GPIO_OUTPUT } aos_gpio_mode_t;
bool aos_io_gpio_mode(int gpio, aos_gpio_mode_t mode, const char *owner);
int  aos_io_gpio_get(int gpio);                  /* 0/1, -1 if not available */
bool aos_io_gpio_set(int gpio, int level);

/* ---- 1-Wire (any usable GPIO of the header) ----
 *
 * One bus per GPIO, for the DS18B20 and its family. On the board it is
 * Espressif's onewire_bus over the RMT: the slots are timed by the
 * peripheral, not by a loop with interrupts off. A 4.7 k pull-up to 3V3 on
 * the wire is what the parts want; 'pullup' adds the internal one, enough
 * for one sensor on a short cable. ROM ids are 64-bit, family code in the
 * low byte and CRC in the high one, as they come off the wire. */
typedef struct aos_io_ow aos_io_ow_t;
aos_io_ow_t *aos_io_ow_open(int gpio, bool pullup, const char *owner);
bool aos_io_ow_reset(aos_io_ow_t *b);                     /* true: a presence pulse */
int  aos_io_ow_search(aos_io_ow_t *b, uint64_t *roms, int max);   /* how many, -1 error */
bool aos_io_ow_write(aos_io_ow_t *b, const void *data, size_t n);
bool aos_io_ow_read(aos_io_ow_t *b, void *data, size_t n);
void aos_io_ow_close(aos_io_ow_t *b);
int  aos_io_ow_gpio(aos_io_ow_t *b);
uint8_t aos_io_ow_crc8(const void *data, size_t n);       /* Dallas/Maxim CRC-8 */
const char *aos_io_ow_family(uint8_t code);               /* "DS18B20", NULL unknown */

/* The DS18B20 (and DS18S20, DS1822, MAX31850): convert_all starts a
 * conversion on every sensor at once (skip ROM) and returns the wait it
 * needs (ms, by the slowest resolution; 750 at 12 bits); read gets one
 * sensor's scratchpad, checks its CRC and gives degrees C and the
 * resolution in bits. 85.0 straight after power-up is the part's reset
 * value, not a reading. */
int  aos_io_ds18b20_convert_all(aos_io_ow_t *b);          /* ms to wait, -1 nobody there */
bool aos_io_ds18b20_read(aos_io_ow_t *b, uint64_t rom, float *celsius, int *bits);
bool aos_io_ds18b20_set_bits(aos_io_ow_t *b, uint64_t rom, int bits);   /* 9..12, kept in its EEPROM */

/* ---- addressable LED strips (any usable GPIO of the header) ----
 *
 * The single-wire kind: WS2812B and its relatives. The board sends them
 * with the RMT, from a buffer in PSRAM (aos_io_p4.c). show() takes the
 * colour of each LED as R, G, B, W bytes (W ignored on an RGB strip) and
 * reorders them for the strip; it returns once the frame is on the wire.
 * Four strips at most at once (the RMT's four TX channels, one of which a
 * 1-Wire bus also takes). */
typedef enum {
    AOS_STRIP_WS2812B = 0,      /* 800 kHz, GRB: also WS2813, WS2815, WS2811 at 800 */
    AOS_STRIP_WS2811_400,       /* the old 400 kHz WS2811 */
    AOS_STRIP_SK6812,           /* 800 kHz, GRB, tighter T0H */
    AOS_STRIP_SK6812_RGBW,      /* four bytes a LED, GRBW */
    AOS_STRIP_TYPE_COUNT
} aos_strip_type_t;

typedef enum { AOS_ORDER_GRB = 0, AOS_ORDER_RGB, AOS_ORDER_BRG, AOS_ORDER_RBG, AOS_ORDER_GBR, AOS_ORDER_BGR,
               AOS_ORDER_COUNT } aos_strip_order_t;

typedef struct {
    aos_strip_type_t  type;
    aos_strip_order_t order;
    uint16_t          count;    /* LEDs, 1..AOS_STRIP_MAX_LEDS */
} aos_strip_cfg_t;

#define AOS_STRIP_MAX_LEDS 1500

typedef struct aos_io_strip aos_io_strip_t;
aos_io_strip_t *aos_io_strip_open(int gpio, const aos_strip_cfg_t *cfg, const char *owner);
bool aos_io_strip_show(aos_io_strip_t *s, const uint8_t *rgbw);   /* count * 4 bytes */
void aos_io_strip_close(aos_io_strip_t *s);
const char *aos_io_strip_type_name(aos_strip_type_t t);   /* "WS2812B" */
const char *aos_io_strip_order_name(aos_strip_order_t o); /* "GRB" */
bool aos_io_strip_type_rgbw(aos_strip_type_t t);

/* ---- PWM (any usable GPIO of the header) ----
 *
 * The LEDC: seven channels (the eighth drives the backlight), 1 Hz to
 * 40 MHz. The duty resolution falls as the frequency rises - 20 bits up to
 * 76 Hz, 14 at 4.8 kHz, 10 at 78 kHz, 2 at 20 MHz - and _bits() says what
 * a channel has. Channels on the same frequency share a timer; there are
 * three, so three different frequencies at once at most (open fails past
 * that). A servo is 50 Hz and a pulse width: set_pulse_us(1500) centres
 * most. fade() ramps in hardware and returns at once. Invert flips the
 * output, as for a LED to 3V3 through the pin. */
typedef struct aos_io_pwm aos_io_pwm_t;
aos_io_pwm_t *aos_io_pwm_open(int gpio, uint32_t freq_hz, const char *owner);
bool     aos_io_pwm_set_freq(aos_io_pwm_t *p, uint32_t freq_hz);    /* keeps the duty */
uint32_t aos_io_pwm_freq(const aos_io_pwm_t *p);                    /* what the hardware gives */
int      aos_io_pwm_bits(const aos_io_pwm_t *p);
bool     aos_io_pwm_set_duty(aos_io_pwm_t *p, float duty);          /* 0..1 */
float    aos_io_pwm_duty(const aos_io_pwm_t *p);
bool     aos_io_pwm_set_pulse_us(aos_io_pwm_t *p, float us);        /* the high time of each period */
bool     aos_io_pwm_fade(aos_io_pwm_t *p, float duty, uint32_t ms);
bool     aos_io_pwm_set_invert(aos_io_pwm_t *p, bool invert);
void     aos_io_pwm_close(aos_io_pwm_t *p);

/* ---- an analog level out (any usable GPIO of the header) ----
 *
 * The P4 has no DAC. The sigma-delta modulator puts out a pulse density
 * (1 MHz) that an RC low-pass turns into a voltage: 1 kOhm and 1 uF give
 * 0..3.3 V with a few mV of ripple and settle in ~5 ms. 256 steps, eight
 * channels. Not for audio. */
typedef struct aos_io_dac aos_io_dac_t;
aos_io_dac_t *aos_io_dac_open(int gpio, const char *owner);
bool  aos_io_dac_set(aos_io_dac_t *d, float level);     /* 0..1 of 3.3 V */
float aos_io_dac_level(const aos_io_dac_t *d);
void  aos_io_dac_close(aos_io_dac_t *d);

/* ---- infrared, raw (the RMT; any usable GPIO of the header) ----
 *
 * Receive with a demodulating receiver (TSOP38238, VS1838B and kin: OUT
 * low while it sees the carrier). read() waits for one frame and gives its
 * marks and spaces in microseconds, mark first; a frame ends with a space
 * longer than gap_us (open's argument, 0 = 20 ms). A glitch under 1 us is
 * dropped. Up to AOS_IR_MAX_DURATIONS a frame.
 *
 * Send through an IR LED and a transistor (the pin drives the base or the
 * gate, never the LED directly): send() puts out marks as the carrier
 * (38 kHz and 33 % unless set_carrier says otherwise) and spaces as
 * nothing, mark first, and returns when the frame is out. 0 Hz sends the
 * marks as plain high levels, for a wired IR input. A receiver and a
 * sender can be open at once, on two pins. */
#define AOS_IR_MAX_DURATIONS 1024
typedef struct aos_io_ir aos_io_ir_t;
aos_io_ir_t *aos_io_ir_rx_open(int gpio, uint32_t gap_us, const char *owner);
int  aos_io_ir_read(aos_io_ir_t *ir, uint16_t *us, int max, int timeout_ms);   /* durations, 0 none, -1 error */
aos_io_ir_t *aos_io_ir_tx_open(int gpio, const char *owner);
bool aos_io_ir_set_carrier(aos_io_ir_t *ir, uint32_t hz, int duty_percent);
bool aos_io_ir_send(aos_io_ir_t *ir, const uint16_t *us, int n);
void aos_io_ir_close(aos_io_ir_t *ir);

/* ---- CAN (TWAI; any two usable GPIOs of the header) ----
 *
 * The P4's TWAI controllers, classic CAN 2.0 (11- and 29-bit ids, up to 8
 * bytes), 25 kbit/s to 1 Mbit/s. On a real bus a 3.3 V transceiver goes
 * between the pins and CANH/CANL (SN65HVD230 and kin; the header has no
 * 5 V-tolerant pins). Modes:
 *   NORMAL     takes part: acknowledges, sends
 *   LISTEN     only listens: never acknowledges or sends (a car's bus)
 *   SELFTEST   sends without needing an acknowledgement and receives its
 *              own frames: tx_gpio == rx_gpio works with nothing wired
 * recv() returns frames in the order they came, with a microsecond time
 * stamp; a queue of 64 in between drops the oldest. A filter keeps the
 * frames whose id matches id under mask (mask 0: all). */
typedef enum { AOS_CAN_NORMAL = 0, AOS_CAN_LISTEN, AOS_CAN_SELFTEST } aos_can_mode_t;
typedef struct {
    uint32_t id;
    bool     ext;               /* 29-bit id */
    bool     rtr;               /* remote frame: no data, len is what it asks for */
    uint8_t  len;               /* 0..8 */
    uint8_t  data[8];
    uint64_t t_us;              /* when it came (recv) */
} aos_can_frame_t;
typedef struct {
    const char *state;          /* "active", "warning", "passive", "bus_off" (for the app to translate) */
    uint16_t tx_errors, rx_errors;
    uint32_t bus_errors;        /* since open */
    uint32_t received, sent, dropped;
} aos_can_status_t;
typedef struct aos_io_can aos_io_can_t;
aos_io_can_t *aos_io_can_open(int tx_gpio, int rx_gpio, uint32_t bitrate, aos_can_mode_t mode, const char *owner);
bool aos_io_can_filter(aos_io_can_t *c, uint32_t id, uint32_t mask, bool ext);
bool aos_io_can_send(aos_io_can_t *c, const aos_can_frame_t *f, int timeout_ms);
int  aos_io_can_recv(aos_io_can_t *c, aos_can_frame_t *f, int timeout_ms);     /* 1, 0 none, -1 closed */
bool aos_io_can_status(aos_io_can_t *c, aos_can_status_t *st);
bool aos_io_can_recover(aos_io_can_t *c);        /* out of bus_off */
void aos_io_can_close(aos_io_can_t *c);

/* ---- the serial capture service (aos_serial.c) ----
 *
 * Up to two channels read a UART port into a ring of lines in the
 * background: the Terminal app shows them, but capture goes on with any
 * other app in front, and later the portal and the triggers read them too. */

#define AOS_SERIAL_CHANNELS 2
#define AOS_SERIAL_LINE_MAX 240

typedef struct {
    bool     running;
    char     port[AOS_IO_PORT_NAME_MAX];
    char     desc[48];
    uint32_t baud;
    uint32_t lines;                  /* lines seen since start (monotonic) */
    uint32_t first;                  /* oldest line still in the ring */
    uint64_t rx_bytes, tx_bytes;
    uint32_t overruns;
    bool     recording;              /* to the card, see aos_serial_record */
    char     rec_path[128];          /* the file, once open */
    uint64_t rec_bytes;
    uint64_t raw_total;              /* bytes ever received, for aos_serial_raw */
} aos_serial_stat_t;

/* A line's flags */
#define AOS_SERIAL_F_TRIGGER 0x01    /* a trigger matched it */
#define AOS_SERIAL_F_BOOT    0x02    /* looks like a chip starting (rst:0x, ESP-ROM:) */

bool aos_serial_start(int ch, const char *port, uint32_t baud);
void aos_serial_stop(int ch);
bool aos_serial_stat(int ch, aos_serial_stat_t *out);
/* Line 'index' (absolute, first..lines-1): its text, the time it ended
 * (ms since boot); returns the length, -1 if it left the ring. */
int  aos_serial_line(int ch, uint32_t index, char *out, int cap, uint32_t *t_ms);
int  aos_serial_line_ex(int ch, uint32_t index, char *out, int cap, uint32_t *t_ms, uint8_t *flags);
int  aos_serial_send(int ch, const void *data, int len);
void aos_serial_clear(int ch);

/* The last 64 KB exactly as received (no line cutting, no colour
 * stripping), for a hex view: copies from absolute byte 'from' (older than
 * the ring starts at its oldest), sets *next. */
int  aos_serial_raw(int ch, uint64_t from, uint8_t *out, int max, uint64_t *next);

/* Every line to <sd>/logs/<port>-<date>.log with the time in front; false
 * without a running channel or a card. A change of speed keeps recording,
 * in a new file. */
bool aos_serial_record(int ch, bool on);

/* Triggers: texts looked for in every line of both channels, case ignored.
 * A hit marks the line and counts; with NOTIFY and/or BEEP it also alerts,
 * at most once every 10 s per trigger. Kept in the preferences; the first
 * time there are three (Guru Meditation, abort(), Brownout). */
#define AOS_SERIAL_TRIGGERS    8
#define AOS_SERIAL_TRIGGER_MAX 48
#define AOS_SERIAL_T_NOTIFY    0x01
#define AOS_SERIAL_T_BEEP      0x02
typedef struct {
    char     text[AOS_SERIAL_TRIGGER_MAX];
    uint8_t  flags;
    uint32_t hits;                   /* since boot */
} aos_serial_trigger_t;
int  aos_serial_trigger_count(void);
bool aos_serial_trigger_get(int i, aos_serial_trigger_t *out);
int  aos_serial_trigger_add(const char *text, uint8_t flags);      /* its index, -1 if full */
void aos_serial_trigger_set(int i, uint8_t flags);
bool aos_serial_trigger_rename(int i, const char *text);         /* keeps its flags, counts from 0 */
void aos_serial_trigger_remove(int i);

#ifdef __cplusplus
}
#endif
