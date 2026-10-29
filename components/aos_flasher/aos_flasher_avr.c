/*
 * P4OS - Programmer: the AVR Arduinos, through the bootloader they already
 * have - what the Arduino IDE does with avrdude, without the computer.
 *
 * Three protocols cover the boards people have:
 *
 *   STK500 v1   optiboot (Uno, Nano with the new bootloader, Pro Mini; 115200)
 *               and the old ATmegaBOOT (older Nanos and Duemilanoves; 57600).
 *               Commands end in 0x20 (CRC_EOP); answers come between 0x14
 *               (INSYNC) and 0x10 (OK). Flash in pages at a word address.
 *   STK500 v2   the Mega 2560's bootloader (115200): framed messages,
 *               1B seq len_hi len_lo 0E body xor, the same seq back.
 *   AVR109      Caterina, the Leonardo's and the Micro's (and the Pro
 *               Micro's): one-letter commands over the board's own USB.
 *               Opening its port at 1200 baud and dropping DTR sends the
 *               sketch to the bootloader, which comes up as another USB
 *               device for eight seconds.
 *
 * The bootloader runs for about a second after a reset (eight for
 * Caterina), so the job resets the board first: on a USB port by DTR, as
 * avrdude does (the Arduino's capacitor turns its edge into a pulse on
 * RESET); on the header by the port's EN line wired to RESET. Without either
 * it asks for the reset button and keeps knocking for a few seconds. Which
 * protocol answers is found by trying, in the order the board's USB ID
 * makes likely: v1 at 115200, v1 at 57600, v2, AVR109 at 57600.
 *
 * The chip is its signature (three bytes); the table below gives its flash
 * and page. A bootloader cannot say how big it is, so its room at the top of
 * the flash is the one each kind uses (512 bytes for optiboot on a 32 KB
 * chip, 2 KB for ATmegaBOOT, 4 KB for Caterina, 8 KB for the Mega's): a
 * .hex that reaches into it is the IDE's "with_bootloader" export, which
 * only an ISP programmer can write, and is refused; a dump read back from a
 * board is written without that part.
 *
 * Write: every page that has bytes of the image, 0xFF around them, then
 * each page read back and compared (the bootloaders have no checksum).
 * Read: the whole flash in pages, each read twice unless told not to verify,
 * to the core's dump file. Erase: Caterina's chip erase; the STK500
 * bootloaders have none that works on every version, so their application
 * area is written with 0xFF.
 *
 * HAL-clean: aos_io and the core only, so the simulator runs it against
 * tools/fake_avr_boot.py.
 */
#include "aos_flasher_priv.h"
#include "aos_i18n.h"
#include "aos_hal.h"
#include "aos_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Chips                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    uint8_t sig[3];
    uint32_t flash;
    uint16_t page;
} avr_chip_t;

static const avr_chip_t CHIPS[] = {
    { "ATmega328P",  { 0x1E, 0x95, 0x0F }, 32768,  128 },
    { "ATmega328PB", { 0x1E, 0x95, 0x16 }, 32768,  128 },
    { "ATmega328",   { 0x1E, 0x95, 0x14 }, 32768,  128 },
    { "ATmega168PA", { 0x1E, 0x94, 0x0B }, 16384,  128 },
    { "ATmega168",   { 0x1E, 0x94, 0x06 }, 16384,  128 },
    { "ATmega88P",   { 0x1E, 0x93, 0x0F }, 8192,   64 },
    { "ATmega88",    { 0x1E, 0x93, 0x0A }, 8192,   64 },
    { "ATmega8",     { 0x1E, 0x93, 0x07 }, 8192,   64 },
    { "ATmega32U4",  { 0x1E, 0x95, 0x87 }, 32768,  128 },
    { "ATmega16U4",  { 0x1E, 0x94, 0x88 }, 16384,  128 },
    { "ATmega2560",  { 0x1E, 0x98, 0x01 }, 262144, 256 },
    { "ATmega1280",  { 0x1E, 0x97, 0x03 }, 131072, 256 },
    { "ATmega1284P", { 0x1E, 0x97, 0x05 }, 131072, 256 },
    { "ATmega1284",  { 0x1E, 0x97, 0x06 }, 131072, 256 },
    { "ATmega644P",  { 0x1E, 0x96, 0x0A }, 65536,  256 },
    { "ATmega32",    { 0x1E, 0x95, 0x02 }, 32768,  128 },
    { "ATmega16",    { 0x1E, 0x94, 0x03 }, 16384,  128 },
};
#define N_CHIPS (sizeof CHIPS / sizeof CHIPS[0])

static const avr_chip_t *chip_of(const uint8_t sig[3])
{
    for (size_t i = 0; i < N_CHIPS; i++)
        if (!memcmp(CHIPS[i].sig, sig, 3)) return &CHIPS[i];
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* The port                                                                    */
/* -------------------------------------------------------------------------- */

enum { P_NONE = 0, P_STK1, P_STK2, P_AVR109 };

typedef struct {
    aos_io_uart_t *u;
    char port[AOS_IO_PORT_NAME_MAX];
    bool usb, lines;
    int proto;
    uint32_t baud;
    uint8_t sig[3];
    const avr_chip_t *chip;
    uint32_t flash, app_end;        /* the application area: [0, app_end) */
    uint16_t page;
    uint8_t seq;                    /* STK500 v2 */
    bool in_prog;
    uint8_t msg[300];               /* a v2 message: a 256-byte page and its header */
    uint8_t *pg, *rd;               /* a page to write, one read back */
} avr_t;

/* Exactly n bytes within ms, or false (a cancel too). */
static bool rx(avr_t *a, void *buf, int n, int ms)
{
    uint8_t *b = buf;
    uint64_t end = aos_hal_uptime_ms() + (uint64_t)ms;
    int got = 0;
    while (got < n) {
        if (fl_cancel) return false;
        uint64_t now = aos_hal_uptime_ms();
        int left = now < end ? (int)(end - now) : 0;
        int r = aos_io_uart_read(a->u, b + got, n - got, left > 50 ? 50 : left);
        if (r < 0) return false;
        got += r;
        if (got < n && !left) return false;
    }
    return true;
}

static bool tx(avr_t *a, const void *buf, int n)
{
    const uint8_t *b = buf;
    uint64_t end = aos_hal_uptime_ms() + 1000;
    int done = 0;
    while (done < n) {
        if (fl_cancel) return false;
        int w = aos_io_uart_write(a->u, b + done, n - done);
        if (w < 0) return false;
        done += w;
        if (done < n) {
            if (aos_hal_uptime_ms() > end) return false;
            aos_hal_sleep_ms(2);
        }
    }
    return true;
}

/* Whatever is waiting: a sketch's output, the half of an answer that came
 * late. Until 30 ms of quiet, at most 300 ms. */
static void drain(avr_t *a)
{
    uint8_t junk[64];
    uint64_t end = aos_hal_uptime_ms() + 300;
    while (aos_hal_uptime_ms() < end && aos_io_uart_read(a->u, junk, sizeof junk, 30) > 0) {}
}

static bool open_port(avr_t *a, const char *name, uint32_t baud)
{
    snprintf(a->port, sizeof a->port, "%s", name);
    a->usb = !strncmp(name, "usb", 3);
    a->u = aos_io_uart_open(name, baud, false, AOS_FLASHER_OWNER);
    /* a board on its own USB (a Leonardo) can be away for a moment */
    for (int i = 0; !a->u && a->usb && i < 30 && !fl_cancel; i++) {
        aos_hal_sleep_ms(100);
        a->u = aos_io_uart_open(name, baud, false, AOS_FLASHER_OWNER);
    }
    if (!a->u) return false;
    a->baud = baud;
    a->lines = a->usb || aos_io_uart_lines(a->u, -1, -1);
    return true;
}

/* Into the bootloader. USB: DTR off for a quarter of a second (the
 * capacitor empties), then on - its edge is the reset - as avrdude does.
 * The header: EN, wired to RESET, low and released. */
static void reset_board(avr_t *a)
{
    if (a->usb) {
        aos_io_uart_dtr_rts(a->u, false, false);
        aos_hal_sleep_ms(250);
        aos_io_uart_dtr_rts(a->u, true, true);
        aos_hal_sleep_ms(50);
    } else if (a->lines) {
        aos_io_uart_lines(a->u, 0, -1);
        aos_hal_sleep_ms(50);
        aos_io_uart_lines(a->u, 1, -1);
        aos_hal_sleep_ms(20);
    }
    drain(a);
}

/* -------------------------------------------------------------------------- */
/* STK500 v1                                                                   */
/* -------------------------------------------------------------------------- */

#define STK_OK          0x10
#define STK_INSYNC      0x14
#define CRC_EOP         0x20

/* 'cmd' ends in CRC_EOP; 'out_n' bytes come back between INSYNC and OK. */
static bool v1_cmd(avr_t *a, const uint8_t *cmd, int n, uint8_t *out, int out_n, int ms)
{
    uint8_t b;
    if (!tx(a, cmd, n)) return false;
    if (!rx(a, &b, 1, ms) || b != STK_INSYNC) return false;
    if (out_n && !rx(a, out, out_n, ms)) return false;
    return rx(a, &b, 1, ms) && b == STK_OK;
}

static bool v1_sync(avr_t *a, int tries)
{
    static const uint8_t SYNC[] = { 0x30, CRC_EOP };
    for (int i = 0; i < tries && !fl_cancel; i++) {
        if (v1_cmd(a, SYNC, 2, NULL, 0, 120)) return true;
        drain(a);
    }
    return false;
}

static bool v1_param(avr_t *a, uint8_t p, uint8_t *v)
{
    uint8_t c[] = { 0x41, p, CRC_EOP };
    return v1_cmd(a, c, 3, v, 1, 300);
}

static bool v1_addr(avr_t *a, uint32_t byte_addr)
{
    uint32_t w = byte_addr >> 1;
    uint8_t c[] = { 0x55, (uint8_t)w, (uint8_t)(w >> 8), CRC_EOP };
    return v1_cmd(a, c, 4, NULL, 0, 300);
}

static bool v1_write(avr_t *a, uint32_t addr, const uint8_t *data, uint16_t n)
{
    if (!v1_addr(a, addr)) return false;
    uint8_t *m = a->msg;
    m[0] = 0x64;
    m[1] = (uint8_t)(n >> 8);
    m[2] = (uint8_t)n;
    m[3] = 'F';
    memcpy(m + 4, data, n);
    m[4 + n] = CRC_EOP;
    return v1_cmd(a, m, 5 + n, NULL, 0, 1000);     /* the page's erase and write: ~9 ms */
}

static bool v1_read(avr_t *a, uint32_t addr, uint8_t *data, uint16_t n)
{
    if (!v1_addr(a, addr)) return false;
    uint8_t c[] = { 0x74, (uint8_t)(n >> 8), (uint8_t)n, 'F', CRC_EOP };
    return v1_cmd(a, c, 5, data, n, 1000);
}

/* -------------------------------------------------------------------------- */
/* STK500 v2                                                                   */
/* -------------------------------------------------------------------------- */

#define V2_START        0x1B
#define V2_TOKEN        0x0E
#define V2_STATUS_OK    0x00

/* One message and its answer: 'ans' gets the answer's body (its command
 * byte, the status, the rest), *ans_n its length. */
static bool v2_cmd(avr_t *a, const uint8_t *body, int n, uint8_t *ans, int *ans_n, int cap, int ms)
{
    uint8_t h[5] = { V2_START, ++a->seq, (uint8_t)(n >> 8), (uint8_t)n, V2_TOKEN };
    uint8_t ck = 0;
    for (int i = 0; i < 5; i++) ck ^= h[i];
    for (int i = 0; i < n; i++) ck ^= body[i];
    if (!tx(a, h, 5) || !tx(a, body, n) || !tx(a, &ck, 1)) return false;
    uint64_t end = aos_hal_uptime_ms() + (uint64_t)ms;
    for (;;) {
        /* the start byte, then the header */
        uint8_t b;
        int left = (int)(end > aos_hal_uptime_ms() ? end - aos_hal_uptime_ms() : 0);
        if (!left || !rx(a, &b, 1, left)) return false;
        if (b != V2_START) continue;
        uint8_t r[4];
        if (!rx(a, r, 4, 200)) return false;
        int len = r[1] << 8 | r[2];
        if (r[3] != V2_TOKEN || len > cap || len < 2) continue;
        uint8_t c2 = V2_START ^ r[0] ^ r[1] ^ r[2] ^ r[3], got;
        if (!rx(a, ans, len, 500) || !rx(a, &got, 1, 200)) return false;
        for (int i = 0; i < len; i++) c2 ^= ans[i];
        if (c2 != got || r[0] != a->seq) continue;  /* a stale or broken one: wait for ours */
        *ans_n = len;
        return ans[0] == body[0] && ans[1] == V2_STATUS_OK;
    }
}

static bool v2_simple(avr_t *a, const uint8_t *body, int n, int ms)
{
    uint8_t ans[32];
    int an;
    return v2_cmd(a, body, n, ans, &an, sizeof ans, ms);
}

static bool v2_sign_on(avr_t *a, char *name, size_t cap)
{
    static const uint8_t C[] = { 0x01 };
    uint8_t ans[32];
    int an;
    for (int i = 0; i < 4 && !fl_cancel; i++) {
        if (v2_cmd(a, C, 1, ans, &an, sizeof ans, 200)) {
            int l = an > 3 ? ans[2] : 0;
            if (l > an - 3) l = an - 3;
            snprintf(name, cap, "%.*s", l, (const char *)ans + 3);
            return true;
        }
        drain(a);
    }
    return false;
}

static bool v2_addr(avr_t *a, uint32_t byte_addr)
{
    uint32_t w = byte_addr >> 1;
    if (a->flash > 65536) w |= 0x80000000u;    /* the extended address: the Mega's upper 128 KB */
    uint8_t c[] = { 0x06, (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w };
    return v2_simple(a, c, 5, 300);
}

static bool v2_write(avr_t *a, uint32_t addr, const uint8_t *data, uint16_t n)
{
    if (!v2_addr(a, addr)) return false;
    /* mode 0xC1 (page, write the page), the ISP commands avrdude sends for
     * an ATmega2560; the bootloader only uses the length and the data */
    uint8_t *m = a->msg;
    const uint8_t head[] = { 0x13, (uint8_t)(n >> 8), (uint8_t)n, 0xC1, 10, 0x40, 0x4C, 0x20, 0xFF, 0xFF };
    memcpy(m, head, sizeof head);
    memcpy(m + sizeof head, data, n);
    uint8_t ans[8];
    int an;
    return v2_cmd(a, m, (int)sizeof head + n, ans, &an, sizeof ans, 1000);
}

static bool v2_read(avr_t *a, uint32_t addr, uint8_t *data, uint16_t n)
{
    if (!v2_addr(a, addr)) return false;
    uint8_t c[] = { 0x14, (uint8_t)(n >> 8), (uint8_t)n, 0x20 };
    int an;
    /* the answer: 14 00 <data> 00 */
    if (!v2_cmd(a, c, 4, a->msg, &an, sizeof a->msg, 1000) || an < n + 3) return false;
    memcpy(data, a->msg + 2, n);
    return true;
}

/* -------------------------------------------------------------------------- */
/* AVR109 (Caterina)                                                           */
/* -------------------------------------------------------------------------- */

/* 'out_n' bytes back; with cr, one more that must be '\r'. */
static bool c109(avr_t *a, const uint8_t *cmd, int n, uint8_t *out, int out_n, bool cr, int ms)
{
    if (!tx(a, cmd, n)) return false;
    if (out_n && !rx(a, out, out_n, ms)) return false;
    uint8_t b;
    return !cr || (rx(a, &b, 1, ms) && b == '\r');
}

static bool c109_hello(avr_t *a, char *name, size_t cap)
{
    for (int i = 0; i < 4 && !fl_cancel; i++) {
        uint8_t id[7];
        if (c109(a, (const uint8_t *)"S", 1, id, 7, false, 200)) {
            snprintf(name, cap, "%.7s", (const char *)id);
            uint8_t p;
            /* 'p' answers 'S' (serial): a real AVR109 and not an echo */
            if (c109(a, (const uint8_t *)"p", 1, &p, 1, false, 200) && p == 'S') return true;
        }
        drain(a);
    }
    return false;
}

static bool c109_addr(avr_t *a, uint32_t byte_addr)
{
    uint32_t w = byte_addr >> 1;
    uint8_t c[] = { 'A', (uint8_t)(w >> 8), (uint8_t)w };
    return c109(a, c, 3, NULL, 0, true, 300);
}

static bool c109_write(avr_t *a, uint32_t addr, const uint8_t *data, uint16_t n)
{
    if (!c109_addr(a, addr)) return false;
    uint8_t *m = a->msg;
    m[0] = 'B';
    m[1] = (uint8_t)(n >> 8);
    m[2] = (uint8_t)n;
    m[3] = 'F';
    memcpy(m + 4, data, n);
    return c109(a, m, 4 + n, NULL, 0, true, 1000);
}

static bool c109_read(avr_t *a, uint32_t addr, uint8_t *data, uint16_t n)
{
    if (!c109_addr(a, addr)) return false;
    uint8_t c[] = { 'g', (uint8_t)(n >> 8), (uint8_t)n, 'F' };
    return c109(a, c, 4, data, n, false, 1000);
}

/* -------------------------------------------------------------------------- */
/* The session: whichever protocol answered                                    */
/* -------------------------------------------------------------------------- */

static bool page_write(avr_t *a, uint32_t addr, const uint8_t *data)
{
    switch (a->proto) {
    case P_STK1: return v1_write(a, addr, data, a->page);
    case P_STK2: return v2_write(a, addr, data, a->page);
    case P_AVR109: return c109_write(a, addr, data, a->page);
    default: return false;
    }
}

static bool page_read(avr_t *a, uint32_t addr, uint8_t *data)
{
    switch (a->proto) {
    case P_STK1: return v1_read(a, addr, data, a->page);
    case P_STK2: return v2_read(a, addr, data, a->page);
    case P_AVR109: return c109_read(a, addr, data, a->page);
    default: return false;
    }
}

static bool read_signature(avr_t *a)
{
    switch (a->proto) {
    case P_STK1: {
        static const uint8_t C[] = { 0x75, CRC_EOP };
        return v1_cmd(a, C, 2, a->sig, 3, 300);
    }
    case P_STK2:
        for (int i = 0; i < 3; i++) {
            uint8_t c[] = { 0x1B, 4, 0x30, 0x00, (uint8_t)i, 0x00 }, ans[8];
            int an;
            if (!v2_cmd(a, c, 6, ans, &an, sizeof ans, 300) || an < 3) return false;
            a->sig[i] = ans[2];
        }
        return true;
    case P_AVR109: {
        uint8_t s[3];
        if (!c109(a, (const uint8_t *)"s", 1, s, 3, false, 300)) return false;
        a->sig[0] = s[2];       /* AVR109 sends it backwards */
        a->sig[1] = s[1];
        a->sig[2] = s[0];
        return true;
    }
    default: return false;
    }
}

static bool enter_prog(avr_t *a)
{
    bool ok;
    switch (a->proto) {
    case P_STK1: {
        static const uint8_t C[] = { 0x50, CRC_EOP };
        ok = v1_cmd(a, C, 2, NULL, 0, 300);
        break;
    }
    case P_STK2: {
        static const uint8_t C[] = { 0x10, 200, 100, 25, 32, 0, 0x53, 3, 0xAC, 0x53, 0, 0 };
        ok = v2_simple(a, C, sizeof C, 500);
        break;
    }
    case P_AVR109: ok = c109(a, (const uint8_t *)"P", 1, NULL, 0, true, 300); break;
    default: ok = false;
    }
    a->in_prog = ok;
    return ok;
}

/* Out of the bootloader, into the sketch. */
static bool leave(avr_t *a)
{
    switch (a->proto) {
    case P_STK1: {
        static const uint8_t C[] = { 0x51, CRC_EOP };
        return v1_cmd(a, C, 2, NULL, 0, 300);
    }
    case P_STK2: {
        static const uint8_t C[] = { 0x11, 1, 1 };
        return v2_simple(a, C, sizeof C, 300);
    }
    case P_AVR109:
        if (a->in_prog) c109(a, (const uint8_t *)"L", 1, NULL, 0, true, 300);
        return c109(a, (const uint8_t *)"E", 1, NULL, 0, true, 300);
    default: return false;
    }
}

/* -------------------------------------------------------------------------- */
/* Finding the bootloader                                                      */
/* -------------------------------------------------------------------------- */

typedef struct { int proto; uint32_t baud; } try_t;

/* The Arduino and Arduino.org IDs of a Leonardo, a Micro, an Esplora and
 * their bootloaders (the low byte 0x36/0x37/0x3C, 0x80 more for the sketch),
 * and SparkFun's Pro Micro (9205/9206, 9203/9204 at 3.3 V). */
static bool caterina_ids(uint16_t vid, uint16_t pid, bool *boot)
{
    uint8_t lo = (uint8_t)pid;
    if ((vid == 0x2341 || vid == 0x2A03) && (lo == 0x36 || lo == 0x37 || lo == 0x3C) && (pid >> 8 == 0x00 || pid >> 8 == 0x80)) {
        *boot = pid >> 8 == 0x00;
        return true;
    }
    if (vid == 0x1B4F && pid >= 0x9203 && pid <= 0x9206) {
        *boot = pid == 0x9203 || pid == 0x9205;
        return true;
    }
    return false;
}

/* The Mega 2560 and Mega ADK (the 16U2 on Arduino's and Arduino.org's): v2 first */
static bool mega_ids(uint16_t vid, uint16_t pid)
{
    return (vid == 0x2341 || vid == 0x2A03) && (pid == 0x0010 || pid == 0x0042 || pid == 0x0044 || pid == 0x003F || pid == 0x0242);
}

static int usb_index(const char *port)
{
    return !strncmp(port, "usb", 3) && port[3] >= '0' && port[3] <= '9' ? atoi(port + 3) : -1;
}

/* A Leonardo's sketch to its bootloader: 1200 baud and DTR dropped. The
 * board goes away and comes back as another device, maybe at another index:
 * the first port with a Caterina bootloader's ID. */
static bool caterina_touch(avr_t *a)
{
    fl_logf(L_INFO, "%s", _("Toque a 1200 baudios: la placa vuelve como su bootloader"));
    aos_io_uart_set_baud(a->u, 1200);
    aos_io_uart_dtr_rts(a->u, true, true);
    aos_hal_sleep_ms(50);
    aos_io_uart_dtr_rts(a->u, false, false);
    aos_hal_sleep_ms(50);
    aos_io_uart_close(a->u);
    a->u = NULL;
    aos_hal_sleep_ms(400);
    uint64_t end = aos_hal_uptime_ms() + 8000;
    while (aos_hal_uptime_ms() < end && !fl_cancel) {
        for (int i = 0; i < aos_hal_usb_serial_count(); i++) {
            uint16_t vid, pid;
            bool boot = false;
            if (aos_hal_usb_serial_ids(i, &vid, &pid) && caterina_ids(vid, pid, &boot) && boot) {
                char name[16];
                snprintf(name, sizeof name, "usb%d", i);
                if (open_port(a, name, 57600)) {
                    if (strcmp(name, J.port)) fl_logf(L_INFO, _("El bootloader apareció en %s"), name);
                    return true;
                }
            }
        }
        aos_hal_sleep_ms(100);
    }
    return fl_cancel ? false : fl_fail("%s", _("El bootloader no apareció: apretá reset dos veces seguidas y probá de nuevo"));
}

static bool hello(avr_t *a, const try_t *t, char *boot, size_t cap)
{
    if (a->baud != t->baud) {
        aos_io_uart_set_baud(a->u, t->baud);
        a->baud = t->baud;
    }
    a->proto = t->proto;
    a->seq = 0;
    switch (t->proto) {
    case P_STK1: {
        if (!v1_sync(a, 6)) return false;
        uint8_t maj = 0, min = 0;
        v1_param(a, 0x81, &maj);
        v1_param(a, 0x82, &min);
        /* optiboot says 4.4 to 8.x; ATmegaBOOT 1.x or 2.x */
        if (t->baud == 115200 && maj >= 4) snprintf(boot, cap, "optiboot %u.%u, %u", maj, min, (unsigned)t->baud);
        else snprintf(boot, cap, "STK500 v1 %u.%u, %u", maj, min, (unsigned)t->baud);
        return true;
    }
    case P_STK2: {
        char n[16];
        if (!v2_sign_on(a, n, sizeof n)) return false;
        snprintf(boot, cap, "STK500 v2 (%.12s), %u", n, (unsigned)t->baud);
        return true;
    }
    case P_AVR109: {
        char n[8];
        if (!c109_hello(a, n, sizeof n)) return false;
        snprintf(boot, cap, "AVR109 (%s)", !strcmp(n, "CATERIN") ? "Caterina" : n);
        return true;
    }
    default: return false;
    }
}

/* The room each bootloader takes at the top of the flash. */
static uint32_t boot_room(const avr_t *a)
{
    switch (a->proto) {
    case P_STK1: return a->baud == 115200 ? (a->flash > 32768 ? 1024 : 512) : 2048;
    case P_STK2: return 8192;
    case P_AVR109: return 4096;
    default: return 0;
    }
}

static bool connect(avr_t *a)
{
    fl_phase(AOS_FLASHER_CONNECTING);
    uint16_t vid = 0, pid = 0;
    bool cat_boot = false;
    int ui = usb_index(J.port);
    bool ids = ui >= 0 && aos_hal_usb_serial_ids(ui, &vid, &pid);
    char boot[32] = "";
    if (ids && caterina_ids(vid, pid, &cat_boot)) {
        if (!cat_boot && !caterina_touch(a)) return false;
        try_t t = { P_AVR109, 57600 };
        if (!hello(a, &t, boot, sizeof boot)) return fl_cancel ? false : fl_fail("%s", _("El bootloader no contestó"));
    } else {
        static const try_t ORDER[] = { { P_STK1, 115200 }, { P_STK1, 57600 }, { P_STK2, 115200 }, { P_AVR109, 57600 } };
        static const try_t MEGA[] = { { P_STK2, 115200 }, { P_STK1, 115200 }, { P_STK1, 57600 }, { P_AVR109, 57600 } };
        const try_t *order = ids && mega_ids(vid, pid) ? MEGA : ORDER;
        bool found = false;
        if (a->lines) {
            for (int i = 0; i < 4 && !found && !fl_cancel; i++) {
                if (a->baud != order[i].baud) { aos_io_uart_set_baud(a->u, order[i].baud); a->baud = order[i].baud; }
                reset_board(a);
                found = hello(a, &order[i], boot, sizeof boot);
            }
        } else {
            /* the reset button by hand: knock on every door for ~15 s */
            fl_logf(L_WARN, "%s", _("Sin línea de reset: apretá el botón de reset de la placa"));
            uint64_t end = aos_hal_uptime_ms() + 15000;
            for (int i = 0; !found && !fl_cancel && aos_hal_uptime_ms() < end; i = (i + 1) % 4)
                found = hello(a, &order[i], boot, sizeof boot);
        }
        if (fl_cancel) return false;
        if (!found)
            return fl_fail("%s", a->lines ? _("La placa no respondió: revisá TX/RX cruzados, GND y RESET")
                                          : _("La placa no respondió: apretá reset justo después de empezar"));
    }
    fl_lock();
    snprintf(fl_st.boot, sizeof fl_st.boot, "%s", boot);
    fl_st.baud = a->baud;
    fl_st.lines = a->lines;
    fl_unlock();
    return true;
}

static bool read_info(avr_t *a)
{
    fl_phase(AOS_FLASHER_INFO);
    if (!read_signature(a)) return fl_cancel ? false : fl_fail("%s", _("No se pudo leer la firma del chip"));
    a->chip = chip_of(a->sig);
    char sig[16];
    snprintf(sig, sizeof sig, "%02X %02X %02X", a->sig[0], a->sig[1], a->sig[2]);
    if (a->chip) {
        a->flash = a->chip->flash;
        a->page = a->chip->page;
        a->app_end = a->flash - boot_room(a);
    }
    fl_lock();
    fl_st.chip_known = true;
    snprintf(fl_st.chip, sizeof fl_st.chip, "%s", a->chip ? a->chip->name : sig);
    fl_st.flash_size = a->flash;
    fl_st.page = a->page;
    fl_unlock();
    if (a->chip)
        fl_logf(L_OK, "%s (%s), flash %u KB, %s", a->chip->name, sig, (unsigned)(a->flash >> 10), fl_st.boot);
    else
        fl_logf(L_WARN, _("Firma %s: un chip que no conozco"), sig);
    if (!a->chip && J.job != AOS_FLASHER_JOB_DETECT) return fl_fail("%s", _("No sé el tamaño de su flash ni de su página"));
    if (a->chip && a->proto == P_STK1 && a->flash > 131072)
        return fl_fail("%s", _("Esta flash pasa de 128 KB: el STK500 v1 no llega"));
    return true;
}

/* -------------------------------------------------------------------------- */
/* Jobs                                                                        */
/* -------------------------------------------------------------------------- */

static void timed(uint64_t t0, uint32_t n)
{
    J.io_ms += aos_hal_uptime_ms() - t0;
    J.io_bytes += n;
}

/* The image's byte at addr, or -1 where it has none. */
static int image_byte(const aos_flasher_image_t *img, uint32_t addr)
{
    for (int i = 0; i < img->n; i++)
        if (addr >= img->r[i].addr && addr - img->r[i].addr < img->r[i].len) return img->r[i].data[addr - img->r[i].addr];
    return -1;
}

static bool page_has_bytes(const aos_flasher_image_t *img, uint32_t addr, uint16_t page)
{
    for (int i = 0; i < img->n; i++)
        if (img->r[i].addr < addr + page && img->r[i].addr + img->r[i].len > addr) return true;
    return false;
}

static bool write_image(avr_t *a)
{
    if (J.src.project) return fl_fail("%s", _("Una carpeta de ESP-IDF no es para un Arduino"));
    aos_flasher_image_t img;
    char err[96];
    uint32_t at = J.src.kind == AOS_FLASHER_IMAGE ? 0 : J.src.files[0].offset;
    if (!aos_flasher_image_load(J.src.path, at, &img, err, sizeof err)) return fl_fail("%s: %s", J.src.name, err);
    bool ok = false;
    uint32_t end = 0;
    for (int i = 0; i < img.n; i++) if (img.r[i].addr + img.r[i].len > end) end = img.r[i].addr + img.r[i].len;
    if (end > a->flash) {
        fl_fail(_("No entra: llega a %u KB y la flash tiene %u KB"), (unsigned)((end + 1023) >> 10), (unsigned)(a->flash >> 10));
        goto out;
    }
    /* the bootloader's room: a dump leaves it out, a .hex must not reach it */
    bool trimmed = false;
    for (uint32_t x = a->app_end; x < end; x++) {
        int b = image_byte(&img, x);
        if (b < 0 || b == 0xFF) continue;
        if (!J.src.dump) {
            fl_fail(_("%s trae un bootloader (0x%X): grabá el .hex sin \"with_bootloader\""), J.src.name, (unsigned)x);
            goto out;
        }
        trimmed = true;
        break;
    }
    if (trimmed) fl_logf(L_INFO, _("El bootloader (desde 0x%X) no se toca"), (unsigned)a->app_end);

    uint32_t pages = 0;
    for (uint32_t p = 0; p < a->app_end; p += a->page) if (page_has_bytes(&img, p, a->page)) pages++;
    if (!pages) { fl_fail("%s", _("El archivo no tiene nada para grabar")); goto out; }
    bool verify = !J.opts.no_verify;
    fl_lock();
    fl_st.file_count = 1;
    fl_st.file_index = 0;
    snprintf(fl_st.file_name, sizeof fl_st.file_name, "%s", J.src.name);
    fl_st.file_offset = img.r[0].addr;
    fl_st.total = pages * a->page * (verify ? 2 : 1);
    fl_st.done = 0;
    fl_st.verified = false;
    fl_unlock();
    fl_logf(L_INFO, _("%u páginas de %u bytes (%u KB)"), (unsigned)pages, a->page, (unsigned)((pages * a->page + 1023) >> 10));
    if (!enter_prog(a)) { fl_fail("%s", _("El bootloader no entró en modo de grabación")); goto out; }

    fl_phase(AOS_FLASHER_WRITING);
    for (uint32_t p = 0; p < a->app_end && !fl_cancel; p += a->page) {
        if (!page_has_bytes(&img, p, a->page)) continue;
        for (uint16_t i = 0; i < a->page; i++) {
            int b = image_byte(&img, p + i);
            a->pg[i] = b < 0 ? 0xFF : (uint8_t)b;
        }
        uint64_t t0 = aos_hal_uptime_ms();
        if (!page_write(a, p, a->pg)) {
            if (!fl_cancel) fl_fail(_("0x%X: falló la escritura"), (unsigned)p);
            goto out;
        }
        timed(t0, a->page);
        fl_progress(a->page);
    }
    if (fl_cancel) goto out;
    fl_logf(L_OK, _("%s: escrito"), J.src.name);
    if (verify) {
        fl_phase(AOS_FLASHER_VERIFYING);
        for (uint32_t p = 0; p < a->app_end && !fl_cancel; p += a->page) {
            if (!page_has_bytes(&img, p, a->page)) continue;
            uint64_t t0 = aos_hal_uptime_ms();
            if (!page_read(a, p, a->rd)) {
                if (!fl_cancel) fl_fail(_("0x%X: falló la lectura"), (unsigned)p);
                goto out;
            }
            timed(t0, a->page);
            for (uint16_t i = 0; i < a->page; i++) {
                int b = image_byte(&img, p + i);
                if (b >= 0 && a->rd[i] != b) {
                    fl_fail(_("0x%X: la placa tiene %02X y el archivo %02X"), (unsigned)(p + i), a->rd[i], b);
                    goto out;
                }
            }
            fl_progress(a->page);
        }
        if (fl_cancel) goto out;
        fl_logf(L_OK, "%s", _("Verificado: cada página igual al archivo"));
    }
    fl_lock();
    fl_st.verified = verify;
    fl_unlock();
    ok = true;
out:
    aos_flasher_image_free(&img);
    return ok;
}

static bool read_flash(avr_t *a)
{
    uint32_t from = J.opts.read_from, len = J.opts.read_len;
    if (from >= a->flash) return fl_fail(_("0x%X queda fuera de la flash (%u KB)"), (unsigned)from, (unsigned)(a->flash >> 10));
    from &= ~(uint32_t)(a->page - 1);
    if (!len || len > a->flash - from) len = a->flash - from;
    len = (len + a->page - 1) & ~(uint32_t)(a->page - 1);
    if (len > a->flash - from) len = a->flash - from;
    if (!enter_prog(a)) return fl_fail("%s", _("El bootloader no entró en modo de grabación"));
    if (!fl_dump_open(from, len)) return false;
    fl_phase(AOS_FLASHER_READING);
    bool verify = !J.opts.no_verify;
    for (uint32_t p = from; p < from + len && !fl_cancel; p += a->page) {
        uint64_t t0 = aos_hal_uptime_ms();
        bool r = page_read(a, p, a->pg);
        if (r && verify) r = page_read(a, p, a->rd);
        timed(t0, a->page);
        if (!r) {
            fl_dump_close(false, false, NULL);
            return fl_cancel ? false : fl_fail(_("0x%X: falló la lectura"), (unsigned)p);
        }
        if (verify && memcmp(a->pg, a->rd, a->page)) {
            fl_dump_close(false, false, NULL);
            return fl_fail(_("0x%X: dos lecturas distintas: revisá el cable"), (unsigned)p);
        }
        if (!fl_dump_write(a->pg, a->page)) { fl_dump_close(false, false, NULL); return false; }
    }
    if (fl_cancel) { fl_dump_close(false, false, NULL); return false; }
    char extra[128];
    snprintf(extra, sizeof extra, "\"family\":\"avr\",\"signature\":\"%02x%02x%02x\",\"bootloader\":\"%s\",\"boot_from\":%u",
             a->sig[0], a->sig[1], a->sig[2], fl_st.boot, (unsigned)a->app_end);
    fl_dump_close(true, verify, extra);
    return true;
}

static bool erase_flash(avr_t *a)
{
    fl_phase(AOS_FLASHER_ERASING);
    if (!enter_prog(a)) return fl_fail("%s", _("El bootloader no entró en modo de grabación"));
    uint64_t t0 = aos_hal_uptime_ms();
    if (a->proto == P_AVR109) {
        if (!c109(a, (const uint8_t *)"e", 1, NULL, 0, true, 10000))
            return fl_cancel ? false : fl_fail("%s", _("No se pudo borrar"));
    } else {
        fl_logf(L_INFO, _("Escribiendo 0xFF en el programa (%u KB)"), (unsigned)(a->app_end >> 10));
        fl_lock();
        fl_st.total = a->app_end;
        fl_st.done = 0;
        fl_unlock();
        memset(a->pg, 0xFF, a->page);
        for (uint32_t p = 0; p < a->app_end && !fl_cancel; p += a->page) {
            uint64_t t1 = aos_hal_uptime_ms();
            if (!page_write(a, p, a->pg)) return fl_cancel ? false : fl_fail(_("0x%X: falló la escritura"), (unsigned)p);
            timed(t1, a->page);
            fl_progress(a->page);
        }
        if (fl_cancel) return false;
    }
    uint32_t ms = (uint32_t)(aos_hal_uptime_ms() - t0);
    fl_logf(L_OK, _("Programa borrado en %u,%u s (el bootloader queda)"), (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
    return true;
}

static bool run(avr_t *a)
{
    fl_phase(AOS_FLASHER_OPENING);
    const aos_io_port_t *pt = aos_io_port_find(J.port);
    bool usb = !strncmp(J.port, "usb", 3);
    if (!usb && (!pt || pt->kind != AOS_PORT_UART)) return fl_fail(_("No hay un puerto serie %s"), J.port);
    if (!open_port(a, J.port, 115200)) {
        if (usb) return fl_fail(_("No se pudo abrir %s: ¿está enchufado, o abierto en la Terminal?"), J.port);
        const char *o = aos_io_owner(pt->pins[0]);
        if (!o) o = aos_io_owner(pt->pins[1]);
        return o ? fl_fail(_("%s está ocupado: lo tiene %s"), J.port, o) : fl_fail(_("No se pudo abrir %s"), J.port);
    }
    int en = -1, boot = -1;
    aos_io_port_lines(J.port, &en, &boot);
    if (usb) fl_logf(L_INFO, _("%s, reset por DTR"), aos_io_uart_desc(a->u));
    else if (a->lines) fl_logf(L_INFO, _("%s, RESET por EN (GPIO%d)"), aos_io_uart_desc(a->u), en);
    if (!connect(a) || !read_info(a)) return false;
    switch (J.job) {
    case AOS_FLASHER_JOB_DETECT: return true;
    case AOS_FLASHER_JOB_FLASH: return write_image(a);
    case AOS_FLASHER_JOB_READ: return read_flash(a);
    case AOS_FLASHER_JOB_ERASE: return erase_flash(a);
    default: return false;
    }
}

bool fl_avr_run(void)
{
    avr_t *a = calloc(1, sizeof *a);
    if (!a) return fl_fail("%s", _("Sin memoria"));
    a->pg = malloc(256);
    a->rd = malloc(256);
    bool ok = a->pg && a->rd ? run(a) : fl_fail("%s", _("Sin memoria"));
    if (a->u) {
        /* back to the sketch, also after a failure or a cancel */
        bool cancelled = fl_cancel;
        fl_cancel = false;
        if (a->proto) {
            fl_phase(AOS_FLASHER_RESETTING);
            bool left = leave(a);
            if (!left && a->lines && a->proto != P_AVR109) reset_board(a);
            if (ok && J.job != AOS_FLASHER_JOB_DETECT) fl_logf(L_INFO, "%s", _("Placa reiniciada"));
        }
        fl_cancel = cancelled;
        aos_io_uart_close(a->u);
    }
    free(a->pg);
    free(a->rd);
    free(a);
    return ok;
}
