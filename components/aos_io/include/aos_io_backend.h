/*
 * P4OS - what a platform provides under aos_io (aos_io_p4.c on the board,
 * sim/io_sim.c in the simulator). Not for apps.
 */
#pragma once

#include "aos_io.h"

#ifdef __cplusplus
extern "C" {
#endif

struct aos_io_uart {
    const aos_io_port_t *port;
    char     owner[24];
    char     desc[48];
    uint32_t baud;
    void    *be;                    /* the backend's own state */
    /* aos_io_uart_lines(): the target's EN and BOOT, from the module on the
     * port (-1: none), whether they are claimed yet and the last levels
     * asked for (-1: never driven) */
    int8_t   en_gpio, boot_gpio;
    bool     lines_claimed;
    int8_t   en, boot;
    /* a USB serial port of the host ("usb0"): the HAL's handle, and no
     * backend nor pins; -1 for a UART */
    int16_t  usb;
    bool     usb_jtag;              /* an Espressif chip's own USB-Serial-JTAG (303a:1001) */
};

struct aos_io_i2c {
    const aos_io_port_t *port;
    char  owner[24];
    void *be;
};

bool aos_io_be_uart_open(aos_io_uart_t *u, bool rs485);
int  aos_io_be_uart_read(aos_io_uart_t *u, void *buf, int len, int timeout_ms);
int  aos_io_be_uart_write(aos_io_uart_t *u, const void *buf, int len);
bool aos_io_be_uart_set_baud(aos_io_uart_t *u, uint32_t baud);
bool aos_io_be_uart_set_format(aos_io_uart_t *u, char parity, int stop_bits);
void aos_io_be_uart_close(aos_io_uart_t *u);
/* Drives the target's lines (u->en_gpio / u->boot_gpio, already claimed):
 * levels 1/0, -1 leaves one alone, BOOT before EN. _release lets them go
 * (inputs again), before aos_io_be_uart_close(). */
bool aos_io_be_uart_lines(aos_io_uart_t *u, int en, int boot);
void aos_io_be_uart_lines_release(aos_io_uart_t *u);

bool aos_io_be_i2c_open(aos_io_i2c_t *b);
bool aos_io_be_i2c_probe(aos_io_i2c_t *b, uint8_t addr);
bool aos_io_be_i2c_xfer(aos_io_i2c_t *b, uint8_t addr, const void *w, size_t wn, void *r, size_t rn, int timeout_ms);
void aos_io_be_i2c_close(aos_io_i2c_t *b);

/* An SPI device. aos_io.c has resolved and claimed everything before the
 * backend sees it: cs is the GPIO (-1: none), clock_hz is clamped, and the
 * port's wires are already the owner's. 'first' is true for the first open
 * handle on the port (the backend brings the bus up) and 'last' on close
 * for the last one (it takes it down). */
struct aos_io_spi {
    const aos_io_port_t *port;      /* = &pcopy */
    aos_io_port_t pcopy;            /* modules.txt may be loaded again while open */
    char     owner[24];
    char     desc[48];
    int8_t   cs;
    uint8_t  mode;
    bool     lsb_first;
    uint32_t clock_hz;              /* asked for */
    uint32_t actual_hz;             /* what the backend says it makes */
    void    *be;
};

bool aos_io_be_spi_open(aos_io_spi_t *s, bool first);
/* One CS assertion, n bytes each way; tx NULL sends 0xFF, rx NULL drops. */
bool aos_io_be_spi_xfer(aos_io_spi_t *s, const uint8_t *tx, uint8_t *rx, size_t n, int timeout_ms);
bool aos_io_be_spi_set_clock(aos_io_spi_t *s, uint32_t hz);
void aos_io_be_spi_close(aos_io_spi_t *s, bool last);

/* 1-Wire: gpio claimed by aos_io.c; the backend owns the bus in be. */
struct aos_io_ow {
    int8_t gpio;
    bool   pullup;
    char   owner[24];
    void  *be;
};
bool aos_io_be_ow_open(aos_io_ow_t *b);
bool aos_io_be_ow_reset(aos_io_ow_t *b);
int  aos_io_be_ow_search(aos_io_ow_t *b, uint64_t *roms, int max);
bool aos_io_be_ow_write(aos_io_ow_t *b, const uint8_t *data, size_t n);
bool aos_io_be_ow_read(aos_io_ow_t *b, uint8_t *data, size_t n);
void aos_io_be_ow_close(aos_io_ow_t *b);

/* LED strips: wire is the frame already in the strip's byte order and
 * width (count * 3 or 4 bytes). */
struct aos_io_strip {
    int8_t          gpio;
    aos_strip_cfg_t cfg;
    uint8_t         bpp;        /* bytes a LED on the wire: 3 or 4 */
    char            owner[24];
    uint8_t        *wire;       /* count * bpp, PSRAM */
    void           *be;
};
bool aos_io_be_strip_open(aos_io_strip_t *s);
bool aos_io_be_strip_send(aos_io_strip_t *s);       /* s->wire, waits for the end */
void aos_io_be_strip_close(aos_io_strip_t *s);

bool aos_io_be_gpio_mode(int gpio, aos_gpio_mode_t mode);
int  aos_io_be_gpio_get(int gpio);
bool aos_io_be_gpio_set(int gpio, int level);

#ifdef __cplusplus
}
#endif
