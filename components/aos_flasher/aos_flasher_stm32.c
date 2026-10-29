/*
 * P4OS - Programmer: STM32 chips through the bootloader in their system
 * memory (ST's AN3155), on a UART: no probe, the same wires as an ESP32.
 *
 * Into the bootloader: BOOT0 high while NRST is released - the port's BOOT
 * line on BOOT0 and EN on NRST - then 0x7F, from which the bootloader
 * takes the speed. Without the lines, it asks for BOOT0 high and a reset,
 * and keeps knocking for a few seconds. The UART is 8E1: even parity.
 *
 * Every command is its code and its complement, answered by ACK (0x79) or
 * NACK (0x1F). GET says which commands there are (the erase is 0x43 on old
 * bootloaders, 0x44 extended on the rest), GET ID the chip (its PID, the
 * same as DBGMCU_IDCODE's DEV_ID), READ MEMORY the flash size from the
 * family's own register. Read protection (RDP) makes READ and WRITE answer
 * NACK: it is said, and the only thing offered is READOUT UNPROTECT, which
 * mass-erases the chip and takes the protected firmware with it.
 *
 * Write: the pages (or, on the F2/F4, the sectors: 16, 16, 16, 16, 64 and
 * then 128 KB) the image touches are erased, then written 256 bytes at a
 * time, then read back and compared. A family whose layout is not in the
 * table below gets a mass erase instead, after saying so. Read: the flash
 * in 256-byte pieces to the core's dump. At the end the chip is reset with
 * BOOT0 low (the lines), or sent to its program by GO.
 *
 * HAL-clean: aos_io and the core, so the simulator runs it against
 * tools/fake_stm32_boot.py.
 */
#include "aos_flasher_priv.h"
#include "aos_i18n.h"
#include "aos_hal.h"
#include "aos_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACK   0x79
#define NACK  0x1F
#define FLASH_BASE 0x08000000u

enum { L_PAGES = 0, L_F4, L_MASS };

typedef struct {
    uint16_t pid;
    const char *name;
    uint32_t size_reg;      /* where the flash size is, in KB (16 bits) */
    uint8_t layout;
    uint32_t page;          /* L_PAGES: the erase unit */
} stm_chip_t;

static const stm_chip_t CHIPS[] = {
    { 0x412, "STM32F1 low",    0x1FFFF7E0, L_PAGES, 1024 },
    { 0x410, "STM32F1 medium", 0x1FFFF7E0, L_PAGES, 1024 },
    { 0x414, "STM32F1 high",   0x1FFFF7E0, L_PAGES, 2048 },
    { 0x430, "STM32F1 XL",     0x1FFFF7E0, L_PAGES, 2048 },
    { 0x418, "STM32F105/107",  0x1FFFF7E0, L_PAGES, 2048 },
    { 0x420, "STM32F100",      0x1FFFF7E0, L_PAGES, 1024 },
    { 0x444, "STM32F03x",      0x1FFFF7CC, L_PAGES, 1024 },
    { 0x445, "STM32F04x",      0x1FFFF7CC, L_PAGES, 1024 },
    { 0x440, "STM32F05x",      0x1FFFF7CC, L_PAGES, 1024 },
    { 0x448, "STM32F07x",      0x1FFFF7CC, L_PAGES, 2048 },
    { 0x442, "STM32F09x",      0x1FFFF7CC, L_PAGES, 2048 },
    { 0x411, "STM32F2",        0x1FFF7A22, L_F4, 0 },
    { 0x413, "STM32F405/407",  0x1FFF7A22, L_F4, 0 },
    { 0x419, "STM32F42x/43x",  0x1FFF7A22, L_F4, 0 },
    { 0x423, "STM32F401xB/C",  0x1FFF7A22, L_F4, 0 },
    { 0x433, "STM32F401xD/E",  0x1FFF7A22, L_F4, 0 },
    { 0x458, "STM32F410",      0x1FFF7A22, L_F4, 0 },
    { 0x431, "STM32F411",      0x1FFF7A22, L_F4, 0 },
    { 0x441, "STM32F412",      0x1FFF7A22, L_F4, 0 },
    { 0x421, "STM32F446",      0x1FFF7A22, L_F4, 0 },
    { 0x434, "STM32F469/479",  0x1FFF7A22, L_F4, 0 },
    { 0x466, "STM32G03x/04x",  0x1FFF75E0, L_PAGES, 2048 },
    { 0x460, "STM32G07x/08x",  0x1FFF75E0, L_PAGES, 2048 },
    { 0x467, "STM32G0Bx/Cx",   0x1FFF75E0, L_PAGES, 2048 },
    { 0x468, "STM32G43x/44x",  0x1FFF75E0, L_PAGES, 2048 },
    { 0x469, "STM32G47x/48x",  0x1FFF75E0, L_MASS, 0 },     /* 2 or 4 KB pages, by its option bytes */
    { 0x435, "STM32L43x/44x",  0x1FFF75E0, L_PAGES, 2048 },
    { 0x462, "STM32L45x/46x",  0x1FFF75E0, L_PAGES, 2048 },
    { 0x415, "STM32L47x/48x",  0x1FFF75E0, L_PAGES, 2048 },
    { 0x417, "STM32L0x3",      0x1FF8007C, L_PAGES, 128 },
    { 0x416, "STM32L1",        0x1FF8004C, L_PAGES, 256 },
    { 0x449, "STM32F74x/75x",  0x1FF0F442, L_MASS, 0 },
    { 0x450, "STM32H74x/75x",  0x1FF1E880, L_MASS, 0 },
};
#define N_CHIPS (sizeof CHIPS / sizeof CHIPS[0])

typedef struct {
    aos_io_uart_t *u;
    bool usb, lines, synced;
    uint32_t baud;
    uint8_t cmds[32];
    int ncmds;
    uint8_t version;
    uint16_t pid;
    const stm_chip_t *chip;
    uint32_t flash;
    bool prot;
    uint8_t *buf;           /* 260: a block and its header */
} stm_t;

/* -------------------------------------------------------------------------- */
/* The wire                                                                    */
/* -------------------------------------------------------------------------- */

static bool rx(stm_t *s, void *buf, int n, int ms)
{
    uint8_t *b = buf;
    uint64_t end = aos_hal_uptime_ms() + (uint64_t)ms;
    int got = 0;
    while (got < n) {
        if (fl_cancel) return false;
        uint64_t now = aos_hal_uptime_ms();
        int left = now < end ? (int)(end - now) : 0;
        int r = aos_io_uart_read(s->u, b + got, n - got, left > 50 ? 50 : left);
        if (r < 0) return false;
        got += r;
        if (got < n && !left) return false;
    }
    return true;
}

static bool tx(stm_t *s, const void *buf, int n)
{
    const uint8_t *b = buf;
    int done = 0;
    uint64_t end = aos_hal_uptime_ms() + 1000;
    while (done < n) {
        if (fl_cancel) return false;
        int w = aos_io_uart_write(s->u, b + done, n - done);
        if (w < 0) return false;
        done += w;
        if (done < n) {
            if (aos_hal_uptime_ms() > end) return false;
            aos_hal_sleep_ms(2);
        }
    }
    return true;
}

static void drain(stm_t *s)
{
    uint8_t junk[64];
    uint64_t end = aos_hal_uptime_ms() + 200;
    while (aos_hal_uptime_ms() < end && aos_io_uart_read(s->u, junk, sizeof junk, 20) > 0) {}
}

/* 1: ACK, 0: NACK, -1: nothing or garbage */
static int wait_ack(stm_t *s, int ms)
{
    uint8_t b;
    if (!rx(s, &b, 1, ms)) return -1;
    return b == ACK ? 1 : b == NACK ? 0 : -1;
}

static int cmd(stm_t *s, uint8_t c)
{
    uint8_t m[2] = { c, (uint8_t)~c };
    return tx(s, m, 2) ? wait_ack(s, 500) : -1;
}

/* An address, big-endian, with its XOR */
static int send_addr(stm_t *s, uint32_t a)
{
    uint8_t m[5] = { (uint8_t)(a >> 24), (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a, 0 };
    m[4] = m[0] ^ m[1] ^ m[2] ^ m[3];
    return tx(s, m, 5) ? wait_ack(s, 500) : -1;
}

static bool has(const stm_t *s, uint8_t c)
{
    for (int i = 0; i < s->ncmds; i++) if (s->cmds[i] == c) return true;
    return false;
}

/* -------------------------------------------------------------------------- */
/* Commands                                                                    */
/* -------------------------------------------------------------------------- */

static bool c_get(stm_t *s)
{
    uint8_t n;
    if (cmd(s, 0x00) != 1 || !rx(s, &n, 1, 300) || n > 32 || !rx(s, &s->version, 1, 300)) return false;
    s->ncmds = n;
    return rx(s, s->cmds, n, 300) && wait_ack(s, 300) == 1;
}

static bool c_gid(stm_t *s)
{
    uint8_t n, id[2];
    if (cmd(s, 0x02) != 1 || !rx(s, &n, 1, 300) || n != 1 || !rx(s, id, 2, 300)) return false;
    s->pid = (uint16_t)(id[0] << 8 | id[1]);
    return wait_ack(s, 300) == 1;
}

/* n bytes (1..256) from addr; 0: NACK (protected), -1: no answer */
static int c_read(stm_t *s, uint32_t addr, uint8_t *out, int n)
{
    int a = cmd(s, 0x11);
    if (a != 1) return a;
    if ((a = send_addr(s, addr)) != 1) return a;
    uint8_t m[2] = { (uint8_t)(n - 1), (uint8_t)~(n - 1) };
    if (!tx(s, m, 2) || (a = wait_ack(s, 500)) != 1) return a;
    return rx(s, out, n, 1000) ? 1 : -1;
}

/* n bytes (multiple of 4, up to 256) to addr */
static int c_write(stm_t *s, uint32_t addr, const uint8_t *d, int n)
{
    int a = cmd(s, 0x31);
    if (a != 1) return a;
    if ((a = send_addr(s, addr)) != 1) return a;
    uint8_t *m = s->buf;
    m[0] = (uint8_t)(n - 1);
    memcpy(m + 1, d, (size_t)n);
    uint8_t x = m[0];
    for (int i = 0; i < n; i++) x ^= d[i];
    m[1 + n] = x;
    return tx(s, m, n + 2) ? wait_ack(s, 2000) : -1;
}

/* Some pages (their numbers), or all of them with n < 0 */
static int c_erase(stm_t *s, const uint16_t *pages, int n, int ms)
{
    bool ext = has(s, 0x44);
    int a = cmd(s, ext ? 0x44 : 0x43);
    if (a != 1) return a;
    uint8_t *m = s->buf;
    int k = 0;
    if (ext) {
        if (n < 0) { m[k++] = 0xFF; m[k++] = 0xFF; }
        else {
            m[k++] = (uint8_t)((n - 1) >> 8);
            m[k++] = (uint8_t)(n - 1);
            for (int i = 0; i < n; i++) { m[k++] = (uint8_t)(pages[i] >> 8); m[k++] = (uint8_t)pages[i]; }
        }
    } else {
        if (n < 0) { m[k++] = 0xFF; }
        else {
            m[k++] = (uint8_t)(n - 1);
            for (int i = 0; i < n; i++) m[k++] = (uint8_t)pages[i];
        }
    }
    if (n < 0 && !ext) { m[k++] = 0x00; }       /* the legacy global erase: FF 00 */
    else {
        uint8_t x = 0;
        for (int i = 0; i < k; i++) x ^= m[i];
        m[k++] = x;
    }
    return tx(s, m, k) ? wait_ack(s, ms) : -1;
}

/* -------------------------------------------------------------------------- */
/* Getting in                                                                  */
/* -------------------------------------------------------------------------- */

/* BOOT0 high, NRST low and released: the system memory runs */
static void reset_into_boot(stm_t *s)
{
    if (!s->lines) return;
    aos_io_uart_lines(s->u, 0, 1);
    aos_hal_sleep_ms(50);
    aos_io_uart_lines(s->u, 1, 1);
    aos_hal_sleep_ms(150);          /* the bootloader looks at its UARTs after a few ms */
    drain(s);
}

/* 0x7F: ACK when it takes the speed from it, NACK when it already had */
static bool sync_boot(stm_t *s, int tries)
{
    for (int i = 0; i < tries && !fl_cancel; i++) {
        uint8_t b = 0x7F;
        if (!tx(s, &b, 1)) return false;
        int a = wait_ack(s, 200);
        if (a >= 0) return true;
        drain(s);
    }
    return false;
}

static bool connect(stm_t *s)
{
    fl_phase(AOS_FLASHER_CONNECTING);
    static const uint32_t SPEEDS[] = { 115200, 57600 };
    bool ok = false;
    if (s->lines) {
        for (int i = 0; i < 2 && !ok && !fl_cancel; i++) {
            aos_io_uart_set_baud(s->u, SPEEDS[i]);
            aos_io_uart_set_format(s->u, 'E', 1);
            s->baud = SPEEDS[i];
            reset_into_boot(s);
            ok = sync_boot(s, 4);
        }
    } else {
        fl_logf(L_WARN, "%s", _("Sin EN/BOOT: poné BOOT0 en 1 y apretá el reset del STM32"));
        aos_io_uart_set_format(s->u, 'E', 1);
        uint64_t end = aos_hal_uptime_ms() + 15000;
        while (!ok && !fl_cancel && aos_hal_uptime_ms() < end) ok = sync_boot(s, 1);
    }
    if (fl_cancel) return false;
    if (!ok) return fl_fail("%s", _("El bootloader del STM32 no contestó: revisá TX/RX cruzados, GND, BOOT0 y NRST"));
    if (!c_get(s)) return fl_fail("%s", _("El bootloader no contestó a GET"));
    if (!c_gid(s)) return fl_fail("%s", _("El bootloader no dijo qué chip es"));
    fl_lock();
    snprintf(fl_st.boot, sizeof fl_st.boot, "AN3155 v%u.%u, %u 8E1", s->version >> 4, s->version & 15, (unsigned)s->baud);
    fl_st.baud = s->baud;
    fl_st.lines = s->lines;
    fl_unlock();
    return true;
}

static bool read_info(stm_t *s)
{
    fl_phase(AOS_FLASHER_INFO);
    for (size_t i = 0; i < N_CHIPS; i++) if (CHIPS[i].pid == s->pid) s->chip = &CHIPS[i];
    char name[24];
    if (s->chip) snprintf(name, sizeof name, "%s", s->chip->name);
    else snprintf(name, sizeof name, "STM32 (0x%03X)", s->pid);
    uint8_t kb[2] = { 0, 0 };
    int r = s->chip ? c_read(s, s->chip->size_reg, kb, 2) : -1;
    s->prot = r == 0;
    if (r == 1) s->flash = (uint32_t)(kb[0] | kb[1] << 8) << 10;
    if (r < 0 && s->chip) drain(s);
    fl_lock();
    fl_st.chip_known = true;
    snprintf(fl_st.chip, sizeof fl_st.chip, "%.15s", name);
    fl_st.flash_size = s->flash;
    fl_st.page = (uint16_t)(s->chip && s->chip->layout == L_PAGES ? s->chip->page : 0);
    fl_st.secure = s->prot;
    fl_unlock();
    char fl[16] = "?";
    if (s->flash) snprintf(fl, sizeof fl, "%u KB", (unsigned)(s->flash >> 10));
    fl_logf(L_OK, "%s (PID 0x%03X), flash %s, %s", name, s->pid, fl, has(s, 0x44) ? _("borrado extendido") : _("borrado clásico"));
    if (s->prot) fl_logf(L_WARN, "%s", _("Protegido contra lectura (RDP): sólo se puede quitar borrando todo"));
    else if (!s->chip) fl_logf(L_WARN, "%s", _("Un STM32 que no está en la tabla: grabarlo borra todo antes"));
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

/* The erase unit holding offset o (from the start of the flash): its number,
 * where it starts and its size. F2/F4: 4 x 16 KB, 64 KB, then 128 KB, and a
 * 2 MB part's second bank numbered from 12. */
static void unit_of(const stm_t *s, uint32_t o, uint16_t *num, uint32_t *start, uint32_t *size)
{
    if (s->chip->layout == L_PAGES) {
        *num = (uint16_t)(o / s->chip->page);
        *start = *num * s->chip->page;
        *size = s->chip->page;
        return;
    }
    uint32_t bank = 0;
    if (o >= (1u << 20) && s->flash > (1u << 20)) { bank = 1u << 20; o -= bank; }
    uint16_t n;
    uint32_t st, sz;
    if (o < 0x10000) { n = (uint16_t)(o >> 14); st = (uint32_t)n << 14; sz = 0x4000; }
    else if (o < 0x20000) { n = 4; st = 0x10000; sz = 0x10000; }
    else { n = (uint16_t)(5 + ((o - 0x20000) >> 17)); st = 0x20000 + ((uint32_t)(n - 5) << 17); sz = 0x20000; }
    *num = (uint16_t)(n + (bank ? 12 : 0));
    *start = st + bank;
    *size = sz;
}

static int image_byte(const aos_flasher_image_t *img, uint32_t addr)
{
    for (int i = 0; i < img->n; i++)
        if (addr >= img->r[i].addr && addr - img->r[i].addr < img->r[i].len) return img->r[i].data[addr - img->r[i].addr];
    return -1;
}

static bool used(const aos_flasher_image_t *img, uint32_t addr, uint32_t len)
{
    for (int i = 0; i < img->n; i++)
        if (img->r[i].addr < addr + len && img->r[i].addr + img->r[i].len > addr) return true;
    return false;
}

static bool erase_for(stm_t *s, const aos_flasher_image_t *img, uint32_t end)
{
    fl_phase(AOS_FLASHER_ERASING);
    if (!s->chip || s->chip->layout == L_MASS) {
        fl_logf(L_WARN, "%s", _("Borrando toda la flash (este chip no tiene el mapa de páginas acá)"));
        return c_erase(s, NULL, -1, 60000) == 1 ? true : fl_fail("%s", _("No se pudo borrar"));
    }
    uint16_t list[64];
    int n = 0, total = 0;
    uint32_t o = 0;
    while (o < end - FLASH_BASE && !fl_cancel) {
        uint16_t num;
        uint32_t st, sz;
        unit_of(s, o, &num, &st, &sz);
        if (used(img, FLASH_BASE + st, sz)) {
            list[n++] = num;
            total++;
        }
        o = st + sz;
        if (n == (int)(sizeof list / sizeof list[0]) || (n && o >= end - FLASH_BASE)) {
            /* an F4's 128 KB sector takes up to 2 s: time for each */
            int ms = 2000 + n * (s->chip->layout == L_F4 ? 4000 : 100);
            if (c_erase(s, list, n, ms) != 1) return fl_cancel ? false : fl_fail("%s", _("No se pudo borrar"));
            n = 0;
        }
    }
    if (n && c_erase(s, list, n, 2000 + n * 4000) != 1) return fl_cancel ? false : fl_fail("%s", _("No se pudo borrar"));
    fl_logf(L_INFO, _("%d páginas o sectores borrados"), total);
    return !fl_cancel;
}

static bool write_image(stm_t *s)
{
    if (s->prot) return fl_fail("%s", _("El chip está protegido contra lectura: no se graba. Borralo entero primero."));
    if (J.src.project) return fl_fail("%s", _("Una carpeta de ESP-IDF no es para este chip"));
    aos_flasher_image_t img;
    char err[96];
    uint32_t at = J.src.kind == AOS_FLASHER_IMAGE ? 0 : J.src.files[0].offset;
    if (at < FLASH_BASE) at += FLASH_BASE;          /* a .bin: from the start of the flash */
    if (!aos_flasher_image_load(J.src.path, at, &img, err, sizeof err)) return fl_fail("%s: %s", J.src.name, err);
    bool ok = false;
    uint32_t limit = s->flash ? s->flash : 2u << 20, end = 0;
    for (int i = 0; i < img.n; i++) {
        uint32_t e = img.r[i].addr + img.r[i].len;
        if (img.r[i].addr < FLASH_BASE || e > FLASH_BASE + limit) {
            fl_fail(_("0x%08X queda fuera de la flash (0x%08X, %u KB)"), (unsigned)img.r[i].addr, (unsigned)FLASH_BASE,
                    (unsigned)(limit >> 10));
            goto out;
        }
        if (e > end) end = e;
    }
    uint32_t blocks = 0;
    for (uint32_t a = FLASH_BASE; a < end; a += 256) blocks += used(&img, a, 256);
    bool verify = !J.opts.no_verify;
    fl_lock();
    fl_st.file_count = 1;
    fl_st.file_index = 0;
    snprintf(fl_st.file_name, sizeof fl_st.file_name, "%s", J.src.name);
    fl_st.file_offset = img.r[0].addr;
    fl_st.total = blocks * 256 * (verify ? 2 : 1);
    fl_st.done = 0;
    fl_st.verified = false;
    fl_unlock();
    if (!erase_for(s, &img, end)) goto out;
    fl_phase(AOS_FLASHER_WRITING);
    for (uint32_t a = FLASH_BASE; a < end && !fl_cancel; a += 256) {
        if (!used(&img, a, 256)) continue;
        uint8_t blk[256];
        for (int i = 0; i < 256; i++) { int b = image_byte(&img, a + (uint32_t)i); blk[i] = b < 0 ? 0xFF : (uint8_t)b; }
        uint64_t t0 = aos_hal_uptime_ms();
        if (c_write(s, a, blk, 256) != 1) { if (!fl_cancel) fl_fail(_("0x%08X: falló la escritura"), (unsigned)a); goto out; }
        timed(t0, 256);
        fl_progress(256);
    }
    if (fl_cancel) goto out;
    fl_logf(L_OK, _("%s: escrito"), J.src.name);
    if (verify) {
        fl_phase(AOS_FLASHER_VERIFYING);
        for (uint32_t a = FLASH_BASE; a < end && !fl_cancel; a += 256) {
            if (!used(&img, a, 256)) continue;
            uint8_t blk[256];
            uint64_t t0 = aos_hal_uptime_ms();
            if (c_read(s, a, blk, 256) != 1) { if (!fl_cancel) fl_fail(_("0x%08X: falló la lectura"), (unsigned)a); goto out; }
            timed(t0, 256);
            for (int i = 0; i < 256; i++) {
                int b = image_byte(&img, a + (uint32_t)i);
                if (b >= 0 && blk[i] != b) {
                    fl_fail(_("0x%08X: el chip tiene %02X y el archivo %02X"), (unsigned)(a + (uint32_t)i), blk[i], b);
                    goto out;
                }
            }
            fl_progress(256);
        }
        if (fl_cancel) goto out;
        fl_logf(L_OK, "%s", _("Verificado: la flash es igual al archivo"));
    }
    fl_lock();
    fl_st.verified = verify;
    fl_unlock();
    ok = true;
out:
    aos_flasher_image_free(&img);
    return ok;
}

static bool read_flash(stm_t *s)
{
    if (s->prot) return fl_fail("%s", _("El chip está protegido contra lectura: no se puede leer"));
    if (!s->flash) return fl_fail("%s", _("No se sabe el tamaño de la flash: indicá cuánto leer"));
    uint32_t from = J.opts.read_from, len = J.opts.read_len;
    if (from >= FLASH_BASE) from -= FLASH_BASE;
    if (from >= s->flash) return fl_fail(_("0x%X queda fuera de la flash (%u KB)"), (unsigned)from, (unsigned)(s->flash >> 10));
    if (!len || len > s->flash - from) len = s->flash - from;
    if (!fl_dump_open(FLASH_BASE + from, len)) return false;
    fl_phase(AOS_FLASHER_READING);
    bool verify = !J.opts.no_verify;
    uint8_t a[256], b[256];
    for (uint32_t o = 0; o < len && !fl_cancel; o += 256) {
        int n = len - o < 256 ? (int)(len - o) : 256;
        uint64_t t0 = aos_hal_uptime_ms();
        bool r = c_read(s, FLASH_BASE + from + o, a, n) == 1;
        if (r && verify) r = c_read(s, FLASH_BASE + from + o, b, n) == 1;
        timed(t0, (uint32_t)n);
        if (!r) { fl_dump_close(false, false, NULL); return fl_cancel ? false : fl_fail(_("0x%08X: falló la lectura"), (unsigned)(FLASH_BASE + from + o)); }
        if (verify && memcmp(a, b, (size_t)n)) { fl_dump_close(false, false, NULL); return fl_fail(_("0x%X: dos lecturas distintas: revisá el cable"), (unsigned)(FLASH_BASE + from + o)); }
        if (!fl_dump_write(a, (uint32_t)n)) { fl_dump_close(false, false, NULL); return false; }
    }
    if (fl_cancel) { fl_dump_close(false, false, NULL); return false; }
    char extra[96];
    snprintf(extra, sizeof extra, "\"family\":\"stm32\",\"pid\":\"0x%03x\",\"bootloader\":\"%s\"", s->pid, fl_st.boot);
    fl_dump_close(true, verify, extra);
    return true;
}

static bool erase_all(stm_t *s)
{
    fl_phase(AOS_FLASHER_ERASING);
    uint64_t t0 = aos_hal_uptime_ms();
    if (s->prot) {
        /* READOUT UNPROTECT: the chip mass-erases itself and restarts */
        fl_logf(L_INFO, "%s", _("Quitando la protección: el chip se borra entero y se reinicia"));
        if (cmd(s, 0x92) != 1 || wait_ack(s, 30000) != 1) return fl_cancel ? false : fl_fail("%s", _("No se pudo quitar la protección"));
        s->prot = false;
        fl_lock();
        fl_st.secure = false;
        fl_unlock();
    } else {
        fl_logf(L_INFO, "%s", _("Borrando toda la flash"));
        if (c_erase(s, NULL, -1, 60000) != 1) return fl_cancel ? false : fl_fail("%s", _("No se pudo borrar"));
    }
    uint32_t ms = (uint32_t)(aos_hal_uptime_ms() - t0);
    fl_logf(L_OK, _("Flash borrada en %u,%u s"), (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
    return true;
}

static bool run(stm_t *s)
{
    fl_phase(AOS_FLASHER_OPENING);
    const aos_io_port_t *pt = aos_io_port_find(J.port);
    s->usb = !strncmp(J.port, "usb", 3);
    if (!s->usb && (!pt || pt->kind != AOS_PORT_UART)) return fl_fail(_("No hay un puerto serie %s"), J.port);
    s->u = aos_io_uart_open(J.port, 115200, false, AOS_FLASHER_OWNER);
    for (int i = 0; !s->u && s->usb && i < 30 && !fl_cancel; i++) {
        aos_hal_sleep_ms(100);
        s->u = aos_io_uart_open(J.port, 115200, false, AOS_FLASHER_OWNER);
    }
    if (!s->u) {
        if (s->usb) return fl_fail(_("No se pudo abrir %s: ¿está enchufado, o abierto en la Terminal?"), J.port);
        const char *o = aos_io_owner(pt->pins[0]);
        if (!o) o = aos_io_owner(pt->pins[1]);
        return o ? fl_fail(_("%s está ocupado: lo tiene %s"), J.port, o) : fl_fail(_("No se pudo abrir %s"), J.port);
    }
    s->lines = aos_io_uart_lines(s->u, -1, -1);
    int en = -1, boot = -1;
    aos_io_port_lines(J.port, &en, &boot);
    if (s->lines && !s->usb) fl_logf(L_INFO, _("%s, NRST por EN (GPIO%d) y BOOT0 por BOOT (GPIO%d)"), aos_io_uart_desc(s->u), en, boot);
    else if (s->lines) fl_logf(L_INFO, _("%s, NRST por RTS y BOOT0 por DTR"), aos_io_uart_desc(s->u));
    if (!connect(s) || !read_info(s)) return false;
    switch (J.job) {
    case AOS_FLASHER_JOB_DETECT: return true;
    case AOS_FLASHER_JOB_FLASH: return write_image(s);
    case AOS_FLASHER_JOB_READ: return read_flash(s);
    case AOS_FLASHER_JOB_ERASE: return erase_all(s);
    default: return fl_fail("%s", _("Eso no se hace con el bootloader del STM32"));
    }
}

bool fl_stm32_run(void)
{
    stm_t *s = calloc(1, sizeof *s);
    if (!s) return fl_fail("%s", _("Sin memoria"));
    s->buf = malloc(600);
    bool ok = s->buf ? run(s) : fl_fail("%s", _("Sin memoria"));
    if (s->u) {
        bool cancelled = fl_cancel;
        fl_cancel = false;
        if (s->pid) {
            /* back to its program: a reset with BOOT0 low, or GO */
            fl_phase(AOS_FLASHER_RESETTING);
            if (s->lines) {
                aos_io_uart_lines(s->u, 0, 0);
                aos_hal_sleep_ms(50);
                aos_io_uart_lines(s->u, 1, 0);
            } else if (!s->prot && cmd(s, 0x21) == 1) {
                send_addr(s, FLASH_BASE);
            }
            if (ok && J.job != AOS_FLASHER_JOB_DETECT) fl_logf(L_INFO, "%s", _("Placa reiniciada"));
        }
        fl_cancel = cancelled;
        aos_io_uart_close(s->u);
    }
    free(s->buf);
    free(s);
    return ok;
}
