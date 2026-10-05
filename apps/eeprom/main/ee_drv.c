/*
 * P4OS - EEPROM: the three wire protocols, and the jobs that use them.
 *
 * A job runs in a thread of its own, one at a time (aos_hal_thread_start, as
 * the Bus app does: the HAL's worker is one for the whole system, and a video
 * left open in the background would hold it). The thread fills J; the app's
 * timer sees J.seq move and takes the results in LVGL's task. The image a
 * read fills or a write takes its bytes from belongs to the app and is
 * handed over in J.img; the app does not touch it while J.busy.
 *
 *   I2C 24xx   reads in blocks of 64 bytes, never across a 256-byte block of
 *              the one-byte chips (whose device address changes there).
 *              Writes a page at a time, never across a page, and then polls
 *              the device address until the chip acknowledges again ("ACK
 *              polling": it ignores the bus while it writes).
 *   SPI 25xx   READ 03 with one, two or three address bytes (the 25xx040's
 *              ninth bit rides in the instruction, bit 3); WREN 06 before
 *              every page write 02, then RDSR 05 until WIP drops.
 *   Flash      the same READ with three bytes; a 4 KB sector is erased (20)
 *              and its 256-byte pages programmed, skipping the blank ones.
 *   Microwire  bit by bit on four GPIOs: CS (active high), SK, DI (our
 *              output) and DO (our input, pulled up). Start bit, two opcode
 *              bits, the address; a READ answers with a dummy 0 and the word,
 *              most significant bit first; a WRITE is followed by CS low and
 *              high again, and DO says busy (0) or ready (1). EWEN before
 *              writing, EWDS after.
 *
 * What a write does, whatever the chip:
 *
 *   1. the whole chip is read and kept as a version ("antes"): a write is
 *      never the only copy of what was there;
 *   2. 4 KB at a time, what should be there is compared with that copy, and
 *      only the pages (sectors, words) that differ are written;
 *   3. every 4 KB that was written is read back and compared.
 *
 * Detecting reads only. Sizes are found by where a sequential read rolls
 * over to the start again; when the start is all one value that cannot be
 * told, and the size test (which writes byte 0 twice and puts it back) can.
 */
#include "ee.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ee_job_t J;

#define CHUNK 4096

static struct {
    const ee_chip_t *c;
    aos_io_i2c_t    *i2c;
    aos_io_spi_t    *spi;
    bool             mw;
    int              abits, org;        /* Microwire: address bits as sent, bits a word */
} D;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(J.msg, sizeof J.msg, fmt, ap);
    va_end(ap);
}

static void add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void add(const char *fmt, ...)
{
    size_t k = strlen(J.msg);
    if (k + 2 >= sizeof J.msg) return;
    if (k) J.msg[k++] = '\n';
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(J.msg + k, sizeof J.msg - k, fmt, ap);
    va_end(ap);
}

static bool uniform(const uint8_t *p, size_t n)
{
    for (size_t i = 1; i < n; i++)
        if (p[i] != p[0]) return false;
    return true;
}

/* -------------------------------------------------------------------------- */
/* I2C 24xx                                                                    */
/* -------------------------------------------------------------------------- */

static int i2c_addr_bytes(uint32_t a, uint8_t *w)
{
    if (D.c->abytes == 1) { w[0] = (uint8_t)a; return 1; }
    w[0] = (uint8_t)(a >> 8);
    w[1] = (uint8_t)a;
    return 2;
}

/* Waits for the chip to answer its address again after a write. */
static bool i2c_ready(uint8_t dev, int ms)
{
    uint64_t t0 = aos_hal_uptime_ms();
    for (;;) {
        if (aos_io_i2c_probe(D.i2c, dev)) return true;
        uint64_t el = aos_hal_uptime_ms() - t0;
        if (el > (uint64_t)ms) return false;
        if (el >= 2) aos_hal_sleep_ms(1);
    }
}

static bool i2c_read(uint32_t a, uint8_t *p, uint32_t n)
{
    while (n) {
        uint32_t k = n > 64 ? 64 : n;
        uint32_t lim = D.c->abytes == 1 ? 256 - (a & 255) : 65536 - (a & 0xFFFF);
        if (k > lim) k = lim;
        uint8_t w[2];
        int wn = i2c_addr_bytes(a, w);
        uint8_t dev = ee_i2c_dev(D.c, J.conf.i2c_addr, a);
        bool ok = aos_io_i2c_xfer(D.i2c, dev, w, (size_t)wn, p, k, 100);
        if (!ok && i2c_ready(dev, 20)) ok = aos_io_i2c_xfer(D.i2c, dev, w, (size_t)wn, p, k, 100);
        if (!ok) return false;
        a += k;
        p += k;
        n -= k;
    }
    return true;
}

static bool i2c_write_page(uint32_t a, const uint8_t *p, uint32_t n)
{
    uint8_t w[2 + 256];
    int wn = i2c_addr_bytes(a, w);
    memcpy(w + wn, p, n);
    uint8_t dev = ee_i2c_dev(D.c, J.conf.i2c_addr, a);
    int wait = D.c->twc_ms * 4 + 10;
    bool ok = aos_io_i2c_xfer(D.i2c, dev, w, (size_t)wn + n, NULL, 0, 100);
    if (!ok && i2c_ready(dev, wait)) ok = aos_io_i2c_xfer(D.i2c, dev, w, (size_t)wn + n, NULL, 0, 100);
    return ok && i2c_ready(dev, wait);
}

/* One sequential read of n bytes from 'a', without splitting it: what the
 * size tests use to see where the chip rolls over. */
static bool i2c_seq(uint32_t a, uint8_t *p, uint32_t n)
{
    uint8_t w[2];
    int wn = i2c_addr_bytes(a, w);
    return aos_io_i2c_xfer(D.i2c, ee_i2c_dev(D.c, J.conf.i2c_addr, a), w, (size_t)wn, p, n, 100);
}

/* -------------------------------------------------------------------------- */
/* SPI 25xx and flash                                                          */
/* -------------------------------------------------------------------------- */

static uint8_t spi_rdsr(void)
{
    uint8_t w = 0x05, r = 0xFF;
    if (!aos_io_spi_write_read(D.spi, &w, 1, &r, 1)) return 0xFF;
    return r;
}

static bool spi_op(uint8_t op) { return aos_io_spi_write_read(D.spi, &op, 1, NULL, 0); }

/* The instruction and the address, as many bytes as the chip takes. */
static int spi_hdr(uint8_t op, uint32_t a, int abytes, uint8_t *w)
{
    if (abytes == 1 && (a & 0x100)) op |= 0x08;         /* the 25xx040's ninth bit */
    int n = 0;
    w[n++] = op;
    for (int k = abytes - 1; k >= 0; k--) w[n++] = (uint8_t)(a >> (8 * k));
    return n;
}

static bool spi_wait(int ms)
{
    uint64_t t0 = aos_hal_uptime_ms();
    for (;;) {
        uint8_t sr = spi_rdsr();
        if (sr != 0xFF && !(sr & 0x01)) return true;
        uint64_t el = aos_hal_uptime_ms() - t0;
        if (el > (uint64_t)ms) return false;
        if (el >= 2) aos_hal_sleep_ms(1);
    }
}

static bool spi_read(uint32_t a, uint8_t *p, uint32_t n)
{
    while (n) {
        uint32_t k = n > CHUNK ? CHUNK : n;
        uint8_t w[5];
        int wn = spi_hdr(0x03, a, D.c->abytes, w);
        if (!aos_io_spi_write_read(D.spi, w, (size_t)wn, p, k)) return false;
        a += k;
        p += k;
        n -= k;
    }
    return true;
}

static bool spi_wren(void)
{
    if (!spi_op(0x06)) return false;
    if (spi_rdsr() & 0x02) return true;
    say("%s", _("El chip no habilita la escritura (WEL no sube): mirá /WP y /HOLD."));
    return false;
}

static bool spi_program(uint32_t a, const uint8_t *p, uint32_t n, int wait_ms)
{
    if (!spi_wait(50) || !spi_wren()) return false;
    uint8_t tx[5 + 256];
    int hn = spi_hdr(0x02, a, D.c->abytes, tx);
    memcpy(tx + hn, p, n);
    if (!aos_io_spi_xfer(D.spi, tx, NULL, (size_t)hn + n, 1000)) return false;
    return spi_wait(wait_ms);
}

static bool flash_sector(uint32_t a, const uint8_t *p)
{
    if (!spi_wait(50) || !spi_wren()) return false;
    uint8_t w[4];
    int wn = spi_hdr(0x20, a, 3, w);
    if (!aos_io_spi_write_read(D.spi, w, (size_t)wn, NULL, 0) || !spi_wait(800)) return false;
    for (uint32_t o = 0; o < CHUNK; o += 256) {
        if (uniform(p + o, 256) && p[o] == 0xFF) continue;
        if (!spi_program(a + o, p + o, 256, 20)) return false;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* Microwire 93xx, by hand                                                     */
/* -------------------------------------------------------------------------- */

/* Two microseconds a half clock: 250 kHz, inside every 93xx's limits at 3.3 V. */
static void mw_wait(void)
{
    uint64_t t = aos_hal_uptime_us() + 2;
    while (aos_hal_uptime_us() < t) {}
}

static void mw_out(int bit)
{
    aos_io_gpio_set(J.conf.mw[MW_DI], bit);
    mw_wait();
    aos_io_gpio_set(J.conf.mw[MW_SK], 1);
    mw_wait();
    aos_io_gpio_set(J.conf.mw[MW_SK], 0);
}

static int mw_in(void)
{
    aos_io_gpio_set(J.conf.mw[MW_SK], 1);
    mw_wait();
    int b = aos_io_gpio_get(J.conf.mw[MW_DO]);
    aos_io_gpio_set(J.conf.mw[MW_SK], 0);
    mw_wait();
    return b > 0;
}

static void mw_cs(int level)
{
    aos_io_gpio_set(J.conf.mw[MW_DI], 0);
    aos_io_gpio_set(J.conf.mw[MW_CS], level);
    mw_wait();
}

static void mw_cmd(int op, uint32_t addr)
{
    mw_cs(1);
    mw_out(1);
    mw_out(op >> 1 & 1);
    mw_out(op & 1);
    for (int i = D.abits - 1; i >= 0; i--) mw_out((int)(addr >> i & 1));
}

/* After a write: CS low starts it, CS high again shows busy on DO. */
static bool mw_done(int ms)
{
    mw_cs(0);
    mw_cs(1);
    uint64_t t0 = aos_hal_uptime_ms();
    bool ok = false;
    while (!(ok = aos_io_gpio_get(J.conf.mw[MW_DO]) > 0) && aos_hal_uptime_ms() - t0 <= (uint64_t)ms) mw_wait();
    mw_cs(0);
    return ok;
}

static void mw_ext(int code)            /* EWDS 0, WRAL 1, ERAL 2, EWEN 3 */
{
    mw_cmd(0, (uint32_t)code << (D.abits - 2));
    mw_cs(0);
}

static uint32_t mw_read_word(uint32_t w)
{
    mw_cmd(2, w);
    uint32_t v = 0;
    for (int i = 0; i < D.org; i++) v = v << 1 | (uint32_t)mw_in();
    mw_cs(0);
    return v;
}

static bool mw_write_word(uint32_t w, uint32_t v)
{
    mw_cmd(1, w);
    for (int i = D.org - 1; i >= 0; i--) mw_out((int)(v >> i & 1));
    return mw_done(D.c->twc_ms * 3 + 5);
}

/* The chip answers a READ with a dummy 0 right after the last address bit:
 * clocking zeros in one at a time and watching DO tells how many address
 * bits it takes, which is its size (and its ORG). -1: DO never fell. */
static int mw_probe_abits(void)
{
    mw_cs(1);
    mw_out(1);
    mw_out(1);
    mw_out(0);
    int n = -1;
    for (int k = 1; k <= 13; k++) {
        mw_out(0);
        if (aos_io_gpio_get(J.conf.mw[MW_DO]) == 0) { n = k; break; }
    }
    mw_cs(0);
    return n;
}

static bool mw_read(uint32_t a, uint8_t *p, uint32_t n)
{
    int wb = D.org / 8;
    for (uint32_t o = 0; o < n; o += (uint32_t)wb) {
        uint32_t v = mw_read_word((a + o) / (uint32_t)wb);
        if (wb == 2) { p[o] = (uint8_t)(v >> 8); if (o + 1 < n) p[o + 1] = (uint8_t)v; }
        else p[o] = (uint8_t)v;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* Opening and the generic calls                                               */
/* -------------------------------------------------------------------------- */

/* Why a port would not open: not in modules.txt, or a pin is someone else's. */
static void why_port(const char *port, int cs)
{
    const aos_io_port_t *p = aos_io_port_find(port);
    if (!p) { say(_("No hay un puerto %s en modules.txt."), port); return; }
    int pins[5] = { p->pins[0], p->pins[1], p->pins[2], p->pins[3], cs };
    for (int k = 0; k < 5; k++) {
        const char *o = pins[k] >= 0 ? aos_io_owner(pins[k]) : NULL;
        if (o && strcmp(o, EE_OWNER)) { say(_("El GPIO%d lo tiene %s: soltalo ahí primero."), pins[k], o); return; }
    }
    say(_("No se pudo abrir %s (el registro dice por qué)."), port);
}

static void drv_close(void)
{
    if (D.i2c) aos_io_i2c_close(D.i2c);
    if (D.spi) aos_io_spi_close(D.spi);
    if (D.mw) {
        for (int k = 0; k < 4; k++) {
            aos_io_gpio_mode(J.conf.mw[k], AOS_GPIO_INPUT, EE_OWNER);
            aos_io_release(J.conf.mw[k], EE_OWNER);
        }
    }
    memset(&D, 0, sizeof D);
}

/* Opens the wires and, with 'check', makes sure somebody is there. */
static bool drv_open(bool check)
{
    drv_close();
    D.c = ee_chip_at(J.conf.chip);
    switch (D.c->fam) {
    case EE_I2C:
        D.i2c = aos_io_i2c_open(J.conf.i2c_port, EE_OWNER);
        if (!D.i2c) { why_port(J.conf.i2c_port, -1); return false; }
        if (check && !aos_io_i2c_probe(D.i2c, J.conf.i2c_addr)) {
            say(_("Nadie contesta en 0x%02X de %s: mirá el cableado, A0-A2 y los pull-ups."),
                J.conf.i2c_addr, J.conf.i2c_port);
            return false;
        }
        return true;
    case EE_SPI:
    case EE_FLASH: {
        aos_io_spi_cfg_t cfg = {
            .clock_hz = J.conf.spi_hz,
            .mode = 0,
            .cs = (int8_t)(J.conf.spi_cs < 0 ? AOS_IO_SPI_CS_PORT : J.conf.spi_cs),
        };
        D.spi = aos_io_spi_open(J.conf.spi_port, EE_OWNER, &cfg);
        if (!D.spi) { why_port(J.conf.spi_port, J.conf.spi_cs); return false; }
        if (!check) return true;
        if (D.c->fam == EE_FLASH) {
            uint8_t w = 0x9F, id[3];
            if (!aos_io_spi_write_read(D.spi, &w, 1, id, 3) || uniform(id, 3)) {
                say(_("La flash no da su JEDEC id (%02X %02X %02X): mirá el cableado y /HOLD."), id[0], id[1], id[2]);
                return false;
            }
        } else if (spi_rdsr() == 0xFF) {
            say("%s", _("Nadie contesta en el SPI: MISO queda en 1. Mirá el cableado, /CS y /HOLD."));
            return false;
        }
        return true;
    }
    case EE_MW: {
        static const aos_gpio_mode_t MODE[4] = { AOS_GPIO_OUTPUT, AOS_GPIO_OUTPUT, AOS_GPIO_OUTPUT, AOS_GPIO_INPUT_PULLUP };
        static const char *const NAME[4] = { "CS", "SK", "DI", "DO" };
        for (int k = 0; k < 4; k++) {
            for (int j = 0; j < k; j++)
                if (J.conf.mw[j] == J.conf.mw[k]) {
                    say(_("%s y %s están en el mismo GPIO%d."), NAME[j], NAME[k], J.conf.mw[k]);
                    return false;
                }
        }
        D.mw = true;            /* from here drv_close() gives the pins back */
        for (int k = 0; k < 4; k++) {
            if (!aos_io_gpio_mode(J.conf.mw[k], MODE[k], EE_OWNER)) {
                const char *o = aos_io_owner(J.conf.mw[k]);
                if (o && strcmp(o, EE_OWNER)) say(_("El GPIO%d (%s) lo tiene %s."), J.conf.mw[k], NAME[k], o);
                else say(_("El GPIO%d (%s) no se puede usar."), J.conf.mw[k], NAME[k]);
                return false;
            }
        }
        aos_io_gpio_set(J.conf.mw[MW_CS], 0);
        aos_io_gpio_set(J.conf.mw[MW_SK], 0);
        aos_io_gpio_set(J.conf.mw[MW_DI], 0);
        D.org = J.conf.mw_org;
        D.abits = D.c->abytes + (D.org == 8);
        int got = mw_probe_abits();
        if (got > 0 && got != D.abits) {
            /* the chip knows best: a 93C56 that ignores its top bit, or the
             * simulator's, which takes one bit fewer */
            aos_hal_log("eeprom", "93xx: %d address bits measured, %d by the table", got, D.abits);
            D.abits = got;
        }
        if (check && got < 0) {
            say("%s", _("Nadie contesta: DO no baja después de la dirección. Mirá el cableado y CS."));
            return false;
        }
        return true;
    }
    }
    return false;
}

static bool rd(uint32_t a, uint8_t *p, uint32_t n)
{
    if (D.i2c) return i2c_read(a, p, n);
    if (D.spi) return spi_read(a, p, n);
    if (D.mw) return mw_read(a, p, n);
    return false;
}

static uint32_t unit_size(void)
{
    if (D.c->fam == EE_FLASH) return CHUNK;
    if (D.c->fam == EE_MW) return (uint32_t)D.org / 8;
    return D.c->page;
}

static bool wr_unit(uint32_t a, const uint8_t *p)
{
    switch (D.c->fam) {
    case EE_I2C:   return i2c_write_page(a, p, D.c->page);
    case EE_SPI:   return spi_program(a, p, D.c->page, D.c->twc_ms * 4 + 20);
    case EE_FLASH: return flash_sector(a, p);
    case EE_MW: {
        uint32_t v = D.org == 16 ? (uint32_t)p[0] << 8 | p[1] : p[0];
        return mw_write_word(a / (uint32_t)(D.org / 8), v);
    }
    }
    return false;
}

/* One byte, for the size test. */
static bool wr_byte(uint32_t a, uint8_t v)
{
    if (D.i2c) return i2c_write_page(a, &v, 1);
    if (D.spi) return spi_program(a, &v, 1, D.c->twc_ms * 4 + 20);
    return false;
}

/* A sequential read of n bytes from a, as one transfer. */
static bool rd_seq(uint32_t a, uint8_t *p, uint32_t n)
{
    if (D.i2c) return i2c_seq(a, p, n);
    if (D.spi) {
        uint8_t w[5];
        int wn = spi_hdr(0x03, a, D.c->abytes, w);
        return aos_io_spi_write_read(D.spi, w, (size_t)wn, p, n);
    }
    return false;
}

/* Does a sequential read cross from S-16 back to the start? 'head' is the
 * first 16 bytes. */
static bool wraps_at(uint32_t S, const uint8_t *head)
{
    uint8_t r[32];
    return rd_seq(S - 16, r, 32) && !memcmp(r + 16, head, 16);
}

/* The sizes a family has with this many address bytes, smallest first. */
static int sizes_of(int fam, int abytes, uint32_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < ee_chip_count() && n < max; i++) {
        const ee_chip_t *c = ee_chip_at(i);
        if (c->fam != fam || c->abytes != abytes) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) dup |= out[k] == c->size;
        if (!dup) out[n++] = c->size;
    }
    return n;
}

const char *ee_phase_text(int phase)
{
    switch (phase) {
    case PH_OPEN:   return _("Abriendo…");
    case PH_READ:   return _("Leyendo");
    case PH_BACKUP: return _("Copia de lo que había");
    case PH_WRITE:  return _("Escribiendo y verificando");
    case PH_VERIFY: return _("Verificando");
    case PH_SAVE:   return _("Guardando la versión");
    case PH_DETECT: return _("Buscando");
    }
    return "";
}

/* -------------------------------------------------------------------------- */
/* Detecting                                                                   */
/* -------------------------------------------------------------------------- */

static void detect_i2c(void)
{
    bool ack[8];
    int n = 0, first = -1;
    char list[48] = "";
    for (int i = 0; i < 8; i++) {
        ack[i] = aos_io_i2c_probe(D.i2c, (uint8_t)(0x50 + i));
        if (!ack[i]) continue;
        n++;
        if (first < 0) first = i;
        size_t k = strlen(list);
        snprintf(list + k, sizeof list - k, "%s0x%02X", k ? " " : "", 0x50 + i);
    }
    if (!n) {
        say(_("Nadie contesta entre 0x50 y 0x57 de %s. Mirá el cableado, los pull-ups de SDA y SCL y los 3V3."),
            J.conf.i2c_port);
        return;
    }
    say(_("Responden: %s"), list);
    J.ok = true;
    uint8_t base = (uint8_t)(0x50 + first);
    J.sugg_addr = base;
    J.conf.i2c_addr = base;             /* what the reads below address */

    /* One address byte or two. A one-byte read is safe on both kinds: a
     * two-byte chip takes it as half an address and reads on from where it
     * was, so the same read twice gives the same bytes only on a one-byte
     * chip. (Two address bytes to a one-byte chip would be a write.) */
    uint8_t r1[16], r2[16], z = 0;
    bool got = aos_io_i2c_xfer(D.i2c, base, &z, 1, r1, 16, 100) && aos_io_i2c_xfer(D.i2c, base, &z, 1, r2, 16, 100);
    if (!got) { add("%s", _("No se pudo leer.")); return; }
    if (uniform(r1, 16)) {
        add(_("El principio está todo en 0x%02X: no se sabe leyendo si toma 1 o 2 bytes de dirección. Elegí el chip; la prueba de tamaño lo confirma."), r1[0]);
        return;
    }
    int ab = memcmp(r1, r2, 16) ? 2 : 1;
    const ee_chip_t *keep = D.c;
    static ee_chip_t probe;
    probe = *D.c;
    probe.abytes = (uint8_t)ab;
    probe.size = ab == 1 ? 2048 : 65536;
    probe.apins = ab == 1 ? 0 : 7;
    D.c = &probe;
    uint32_t size = 0;
    if (ab == 1) {
        int m = 0;
        while (first + m < 8 && ack[first + m]) m++;
        if (m >= 8) size = 2048;
        else if (m >= 4 && !(first & 3)) size = 1024;
        else if (m >= 2 && !(first & 1)) size = 512;
        else {
            uint8_t head[16];
            probe.size = 256;
            if (i2c_seq(0, head, 16) && !uniform(head, 16)) size = wraps_at(128, head) ? 128 : 256;
        }
        add("%s", _("Toma 1 byte de dirección (24xx01 a 24xx16)."));
    } else {
        add("%s", _("Toma 2 bytes de dirección (24xx32 o más grande)."));
        uint8_t head[16];
        if (i2c_seq(0, head, 16) && !uniform(head, 16)) {
            uint32_t sz[8];
            int ns = sizes_of(EE_I2C, 2, sz, 8);
            for (int k = 0; k < ns && !size; k++)
                if (sz[k] < 65536 && wraps_at(sz[k], head)) size = sz[k];
            /* no roll-over below 64 KB: a 24xx512, or a 24xx1025 if its
             * upper half answers at +4 */
            if (!size) size = first < 4 && ack[first + 4] ? 131072 : 65536;
        }
    }
    D.c = keep;
    if (!size) {
        add("%s", _("El tamaño no se sabe leyendo (el principio es todo igual): probá la prueba de tamaño."));
        return;
    }
    J.sugg_chip = ee_chip_by_size(EE_I2C, size, (uint8_t)ab);
    char st[16];
    ee_size_text(size, st, sizeof st);
    if (J.sugg_chip >= 0) add(_("Da la vuelta a los %s: un %s."), st, ee_chip_at(J.sugg_chip)->name);
}

static const char *maker(uint8_t id)
{
    switch (id) {
    case 0xEF: return "Winbond";
    case 0xC8: return "GigaDevice";
    case 0xC2: return "Macronix";
    case 0x20: return "Micron / ST";
    case 0x1F: return "Adesto / Atmel";
    case 0xBF: return "SST / Microchip";
    case 0x9D: return "ISSI";
    case 0x85: return "Puya";
    case 0x68: return "Boya";
    case 0x0B: return "XTX";
    case 0x5E: return "Zbit";
    case 0xA1: return "Fudan";
    case 0x01: return "Spansion / Cypress";
    }
    return NULL;
}

static void detect_spi(void)
{
    uint8_t w = 0x9F, id[3] = { 0xFF, 0xFF, 0xFF };
    aos_io_spi_write_read(D.spi, &w, 1, id, 3);
    if (!uniform(id, 3) && id[0] != 0x00 && id[0] != 0xFF) {
        const char *mk = maker(id[0]);
        say(_("Una flash: JEDEC %02X %02X %02X (%s)."), id[0], id[1], id[2], mk ? mk : _("fabricante desconocido"));
        J.ok = true;
        if (id[2] >= 16 && id[2] <= 24) {
            uint32_t size = 1u << id[2];
            char st[16];
            ee_size_text(size, st, sizeof st);
            J.sugg_chip = ee_chip_by_size(EE_FLASH, size, 0);
            add(_("Tamaño por el id: %s."), st);
        } else if (id[2] > 24) {
            add("%s", _("Es de más de 16 MB: con 3 bytes de dirección se ven los primeros 16."));
            J.sugg_chip = ee_chip_by_size(EE_FLASH, 16777216, 0);
        }
        uint8_t sr = spi_rdsr();
        J.have_sr = true;
        J.sr = sr;
        if (sr & 0x1C) add(_("Protegida: estado 0x%02X (BP2-BP0 = %d)."), sr, (sr >> 2) & 7);
        return;
    }
    uint8_t sr = spi_rdsr();
    if (sr == 0xFF) {
        say("%s", _("Nadie contesta en el SPI: MISO queda en 1. Mirá el cableado, /CS y /HOLD."));
        return;
    }
    J.ok = true;
    J.have_sr = true;
    J.sr = sr;
    say(_("Una EEPROM 25xx: estado 0x%02X."), sr);
    if (sr & 0x0C) add(_("Protegida: BP1 BP0 = %d%d."), (sr >> 3) & 1, (sr >> 2) & 1);
    /* the 25xx1024 gives its signature, 0x29, after AB and three dummy bytes */
    uint8_t sig[4] = { 0xAB, 0, 0, 0 }, s = 0;
    if (aos_io_spi_write_read(D.spi, sig, 4, &s, 1) && s == 0x29) {
        J.sugg_chip = ee_chip_find("25LC1024");
        add("%s", _("Firma 0x29: un 25xx1024."));
        return;
    }
    /* Address bytes: a READ with one byte too many starts one byte later,
     * so R(n+1) is R(n) shifted by one exactly when the chip takes n. */
    uint8_t r[4][33];
    for (int k = 0; k < 4; k++) {
        uint8_t hdr[5] = { 0x03, 0, 0, 0, 0 };
        aos_io_spi_write_read(D.spi, hdr, (size_t)k + 2, r[k], 33);
    }
    int ab = 0;
    if (uniform(r[1], 32)) {
        add(_("Está todo en 0x%02X: el tamaño no se sabe leyendo. Elegí el chip; la prueba de tamaño lo confirma."), r[1][0]);
        return;
    }
    for (int k = 0; k < 3 && !ab; k++)
        if (!memcmp(r[k + 1], r[k] + 1, 32)) ab = k + 1;
    if (!ab) { add("%s", _("No se entiende cuántos bytes de dirección toma.")); return; }
    add(_("Toma %d byte(s) de dirección."), ab);
    const ee_chip_t *keep = D.c;
    static ee_chip_t probe;
    probe = *D.c;
    probe.abytes = (uint8_t)ab;
    D.c = &probe;
    uint32_t sz[12];
    int ns = sizes_of(EE_SPI, ab, sz, 12);
    uint32_t size = ns ? sz[ns - 1] : 0;
    for (int k = 0; k + 1 < ns; k++)
        if (wraps_at(sz[k], r[ab - 1])) { size = sz[k]; break; }
    D.c = keep;
    J.sugg_chip = ee_chip_by_size(EE_SPI, size, (uint8_t)ab);
    char st[16];
    ee_size_text(size, st, sizeof st);
    if (J.sugg_chip >= 0) add(_("Da la vuelta a los %s: un %s."), st, ee_chip_at(J.sugg_chip)->name);
}

static void detect_mw(void)
{
    int n = D.abits;            /* drv_open measured it */
    if (mw_probe_abits() < 0) {
        say("%s", _("Nadie contesta: DO no baja después de la dirección. Mirá el cableado, CS y que DO tenga pull-up."));
        return;
    }
    J.ok = true;
    static const struct { int bits; const char *a, *b; int org; } MAP[] = {
        { 6, "93C46", NULL, 16 }, { 7, "93C46", NULL, 8 },
        { 8, "93C56", "93C66", 16 }, { 9, "93C56", "93C66", 8 },
        { 10, "93C76", "93C86", 16 }, { 11, "93C76", "93C86", 8 },
    };
    say(_("DO baja después de %d bits de dirección."), n);
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++) {
        if (MAP[i].bits != n) continue;
        J.sugg_org = MAP[i].org;
        const char *pick = MAP[i].a;
        if (MAP[i].b) {
            /* the smaller one ignores the top address bit: the second half
             * of the bigger one's range reads the first again */
            D.org = MAP[i].org;
            uint32_t words = ee_chip_at(ee_chip_find(MAP[i].b))->size * 8 / (uint32_t)D.org;
            uint32_t h0[8], h1[8];
            for (int k = 0; k < 8; k++) { h0[k] = mw_read_word((uint32_t)k); h1[k] = mw_read_word(words / 2 + (uint32_t)k); }
            bool same = !memcmp(h0, h1, sizeof h0), flat = true;
            for (int k = 1; k < 8; k++) flat &= h0[k] == h0[0];
            if (flat) add(_("Es un %s o un %s: el principio es todo igual y no se distinguen leyendo."), MAP[i].a, MAP[i].b);
            else pick = same ? MAP[i].a : MAP[i].b;
        }
        J.sugg_chip = ee_chip_find(pick);
        add(_("Un %s en x%d (ORG a %s)."), pick, MAP[i].org, MAP[i].org == 16 ? "3V3" : "GND");
        return;
    }
    add("%s", _("No es ninguno de la tabla."));
}

static void job_detect(void)
{
    J.phase = PH_DETECT;
    if (!drv_open(false)) return;
    switch (D.c->fam) {
    case EE_I2C: detect_i2c(); break;
    case EE_SPI:
    case EE_FLASH: detect_spi(); break;
    case EE_MW: detect_mw(); break;
    }
}

/* The size test: writes two marks in byte 0, sees across which boundary a
 * sequential read finds them again, and puts byte 0 back. */
static void job_size(void)
{
    J.phase = PH_DETECT;
    if (!drv_open(true)) return;
    if (D.c->fam == EE_FLASH || D.c->fam == EE_MW) {
        say("%s", _("No hace falta: la flash dice su tamaño en el JEDEC id y la 93xx en la dirección. Usá Detectar."));
        return;
    }
    uint32_t sz[12];
    int ns = sizes_of(D.c->fam, D.c->abytes, sz, 12);
    uint8_t b0;
    if (!rd(0, &b0, 1)) { say("%s", _("No se pudo leer el byte 0.")); return; }
    bool hit[12];
    for (int k = 0; k < ns; k++) hit[k] = true;
    const uint8_t marks[2] = { (uint8_t)(b0 ^ 0x55), (uint8_t)(b0 ^ 0xAA) };
    bool ok = true;
    for (int m = 0; m < 2 && ok; m++) {
        ok = wr_byte(0, marks[m]);
        for (int k = 0; ok && k + 1 < ns; k++) {
            uint8_t r[2];
            hit[k] &= rd_seq(sz[k] - 1, r, 2) && r[1] == marks[m];
        }
    }
    bool back = wr_byte(0, b0);
    uint8_t chk = (uint8_t)~b0;
    back = back && rd(0, &chk, 1) && chk == b0;
    if (!ok) { say("%s", _("No se pudo escribir el byte 0: ¿WP está a 3V3?")); return; }
    uint32_t size = sz[ns - 1];
    for (int k = 0; k + 1 < ns; k++)
        if (hit[k]) { size = sz[k]; break; }
    J.ok = true;
    J.sugg_chip = ee_chip_by_size(D.c->fam, size, D.c->abytes);
    char st[16];
    ee_size_text(size, st, sizeof st);
    say(_("Da la vuelta a los %s: un %s."), st, J.sugg_chip >= 0 ? ee_chip_at(J.sugg_chip)->name : "?");
    if (back) add(_("El byte 0 volvió a 0x%02X."), b0);
    else add(_("¡Ojo! El byte 0 no volvió a 0x%02X: escribilo a mano."), b0);
}

/* -------------------------------------------------------------------------- */
/* Reading and writing                                                         */
/* -------------------------------------------------------------------------- */

static uint32_t read_step(void) { return D.mw ? 64 : D.i2c ? 256 : CHUNK; }

static void save_img(const char *source, char *file, size_t len, bool *same)
{
    file[0] = 0;
    if (!ee_card_ok()) return;
    J.phase = PH_SAVE;
    ee_vw_t vw;
    const char *chip = ee_chip_at(J.conf.chip)->name;
    if (!ee_vw_open(&vw, chip, source)) return;
    if (!ee_vw_put(&vw, J.img, J.size)) { ee_vw_abort(&vw); return; }
    if (!ee_vw_close(&vw, source, J.from_portal ? _("desde el portal") : "", file, len, same)) file[0] = 0;
}

static void job_read(void)
{
    J.phase = PH_OPEN;
    if (!drv_open(true)) return;
    J.phase = PH_READ;
    J.total = J.size;
    uint32_t step = read_step();
    for (uint32_t a = 0; a < J.size; a += step) {
        uint32_t k = J.size - a < step ? J.size - a : step;
        if (!rd(a, J.img + a, k)) { say(_("No contestó en la dirección 0x%X."), (unsigned)a); return; }
        J.done = a + k;
        if (J.cancel) { say("%s", _("Lectura cancelada.")); return; }
    }
    drv_close();
    ee_sums_of(J.img, J.size, &J.sums);
    J.ok = true;
    save_img("lectura", J.saved, sizeof J.saved, &J.same);
    char st[16];
    ee_size_text(J.size, st, sizeof st);
    say(_("Leídos %s."), st);
    if (!ee_card_ok()) add("%s", _("Sin tarjeta: no se guardó la versión."));
    else if (!J.saved[0]) add("%s", _("No se pudo guardar la versión en la tarjeta."));
    else if (J.same) add("%s", _("Igual a la última versión: no se guardó otra."));
}

static void job_write(void)
{
    const ee_chip_t *c = ee_chip_at(J.conf.chip);
    J.phase = PH_OPEN;
    if (!ee_card_ok()) { say("%s", _("Sin tarjeta no se escribe: no habría dónde guardar la copia de lo que había.")); return; }
    if (!drv_open(true)) return;
    if (D.spi) {
        uint8_t sr = spi_rdsr();
        uint8_t bp = c->fam == EE_FLASH ? 0x1C : 0x0C;
        if (sr & bp) {
            say(_("Está protegida (estado 0x%02X): sacale la protección en la pestaña Chip. No se escribió nada."), sr);
            return;
        }
    }
    uint8_t *chunk = ee_alloc(CHUNK), *tgt = ee_alloc(CHUNK);
    FILE *f = NULL;
    if (!chunk || !tgt) { say("%s", _("No hay memoria.")); goto out; }

    /* 1. what was there, to the card */
    J.phase = PH_BACKUP;
    J.total = J.size;
    J.done = 0;
    ee_vw_t vw;
    if (!ee_vw_open(&vw, c->name, "antes")) { say("%s", _("No se pudo guardar la copia en la tarjeta: no se escribió nada.")); goto out; }
    uint32_t step = read_step();
    for (uint32_t a = 0; a < J.size; a += step) {
        uint32_t k = J.size - a < step ? J.size - a : step;
        if (!rd(a, chunk, k) || !ee_vw_put(&vw, chunk, k)) {
            ee_vw_abort(&vw);
            say(_("La copia falló en 0x%X: no se escribió nada."), (unsigned)a);
            goto out;
        }
        J.done = a + k;
        if (J.cancel) { ee_vw_abort(&vw); say("%s", _("Cancelado antes de escribir.")); goto out; }
    }
    if (!ee_vw_close(&vw, "antes", _("copia antes de escribir"), J.saved, sizeof J.saved, &J.same)) {
        say("%s", _("No se pudo guardar la copia en la tarjeta: no se escribió nada."));
        goto out;
    }
    char path[200];
    ee_ver_path(c->name, J.saved, path, sizeof path);
    f = fopen(path, "rb");
    if (!f) { say("%s", _("No se pudo releer la copia: no se escribió nada.")); goto out; }

    /* 2 and 3: the pages that differ, each 4 KB read back */
    J.phase = PH_WRITE;
    J.done = 0;
    uint32_t U = unit_size();
    bool eral = D.mw && J.mode == WR_ERASE;
    if (D.mw) {
        mw_ext(3);                                  /* EWEN */
        if (eral) {
            mw_ext(2);                              /* ERAL */
            mw_done(50);
            J.units = J.size / U;
        }
    }
    for (uint32_t a = 0; a < J.size; a += CHUNK) {
        uint32_t k = J.size - a < CHUNK ? J.size - a : CHUNK;
        if (fread(chunk, 1, k, f) != k) { say("%s", _("No se pudo releer la copia.")); goto out; }
        if (J.mode == WR_FULL) memcpy(tgt, J.img + a, k);
        else if (J.mode == WR_ERASE) memset(tgt, 0xFF, k);
        else {
            memcpy(tgt, chunk, k);
            for (uint32_t i = 0; i < k; i++)
                if (J.dirty[(a + i) >> 3] & (1u << ((a + i) & 7))) tgt[i] = J.img[a + i];
        }
        bool wrote = eral;
        for (uint32_t u = 0; !eral && u < k; u += U) {
            if (!memcmp(tgt + u, chunk + u, U)) continue;
            if (!wr_unit(a + u, tgt + u)) {
                if (!J.msg[0]) say(_("No se pudo escribir en 0x%X."), (unsigned)(a + u));
                add(_("La copia de antes está en Versiones (%s)."), J.saved);
                goto out;
            }
            J.units++;
            wrote = true;
            J.done = a + u + U;
            if (J.cancel) {
                say(_("Cancelado a mitad: quedó escrito hasta 0x%X. La copia de antes está en Versiones."), (unsigned)(a + u + U));
                goto out;
            }
        }
        if (wrote) {
            if (!rd(a, chunk, k)) { say(_("No se pudo releer 0x%X para verificar."), (unsigned)a); goto out; }
            for (uint32_t i = 0; i < k; i++) {
                if (chunk[i] == tgt[i]) continue;
                J.bad_at = a + i;
                say(_("No verificó en 0x%X: el chip tiene 0x%02X y debería 0x%02X."), (unsigned)(a + i), chunk[i], tgt[i]);
                if (c->fam == EE_I2C) add("%s", _("¿WP está a 3V3? Para escribir va a GND."));
                goto out;
            }
        }
        memcpy(J.img + a, tgt, k);              /* the image is now what the chip holds */
        J.done = a + k;
    }
    if (D.mw) mw_ext(0);                        /* EWDS */
    drv_close();
    fclose(f);
    f = NULL;
    ee_sums_of(J.img, J.size, &J.sums);
    J.ok = true;
    if (J.units) save_img("escrita", J.saved2, sizeof J.saved2, &J.same2);
    if (!J.units) say("%s", _("El chip ya tenía eso: no hizo falta escribir nada."));
    else if (J.units == 1) say("%s", c->fam == EE_FLASH ? _("Escrito 1 sector y verificado.")
                                   : c->fam == EE_MW    ? _("Escrita 1 palabra y verificada.")
                                                        : _("Escrita 1 página y verificada."));
    else if (c->fam == EE_FLASH) say(_("Escritos %u sectores y verificados."), (unsigned)J.units);
    else if (c->fam == EE_MW) say(_("Escritas %u palabras y verificadas."), (unsigned)J.units);
    else say(_("Escritas %u páginas y verificadas."), (unsigned)J.units);
out:
    if (D.mw) mw_ext(0);
    if (f) fclose(f);
    free(chunk);
    free(tgt);
}

static void job_status(void)
{
    if (!drv_open(true)) return;
    if (!D.spi) { say("%s", _("Esta memoria no tiene registro de estado.")); return; }
    J.sr = spi_rdsr();
    J.have_sr = true;
    J.ok = true;
    say(_("Estado 0x%02X."), J.sr);
}

static void job_protect(void)
{
    if (!drv_open(true)) return;
    if (!D.spi) { say("%s", _("Esta memoria no tiene registro de estado.")); return; }
    uint8_t mask = D.c->fam == EE_FLASH ? 0xFC : 0x8C;
    if (!spi_wait(50) || !spi_wren()) return;
    uint8_t w[2] = { 0x01, J.new_sr };
    aos_io_spi_write_read(D.spi, w, 2, NULL, 0);
    spi_wait(D.c->fam == EE_FLASH ? 50 : D.c->twc_ms * 4 + 20);
    J.sr = spi_rdsr();
    J.have_sr = true;
    if ((J.sr & mask) != (J.new_sr & mask)) {
        say(_("No cambió: el estado quedó en 0x%02X. Con WPEN puesto, /WP tiene que estar a 3V3."), J.sr);
        return;
    }
    J.ok = true;
    say(_("Listo: estado 0x%02X."), J.sr);
}

/* -------------------------------------------------------------------------- */
/* The thread                                                                  */
/* -------------------------------------------------------------------------- */

static void job_thread(void *arg)
{
    (void)arg;
    switch (J.job) {
    case JOB_DETECT:  job_detect(); break;
    case JOB_SIZE:    job_size(); break;
    case JOB_READ:    job_read(); break;
    case JOB_WRITE:   job_write(); break;
    case JOB_STATUS:  job_status(); break;
    case JOB_PROTECT: job_protect(); break;
    }
    drv_close();
    aos_hal_log("eeprom", "job %d: %s", J.job, J.ok ? "ok" : J.msg);
    J.busy = false;
    J.seq++;
}

bool ee_job_start(int job, int mode)
{
    if (J.busy) return false;
    J.busy = true;
    J.cancel = false;
    J.job = job;
    J.mode = mode;
    J.from_portal = false;
    J.conf = S.conf;
    J.img = S.img;
    J.dirty = S.dirty;
    J.size = ee_chip_at(S.conf.chip)->size;
    J.done = 0;
    J.total = 0;
    J.phase = PH_OPEN;
    J.ok = false;
    J.msg[0] = 0;
    J.sugg_chip = J.sugg_addr = -1;
    J.sugg_org = 0;
    J.have_sr = false;
    J.units = 0;
    J.bad_at = ~0u;
    J.saved[0] = J.saved2[0] = 0;
    J.same = J.same2 = false;
    if (!aos_hal_thread_start("eeprom", job_thread, NULL, 8192, 3)) {
        J.busy = false;
        return false;
    }
    return true;
}

void ee_job_stop(void)
{
    J.cancel = true;
    for (int i = 0; i < 400 && J.busy; i++) aos_hal_sleep_ms(10);
}
