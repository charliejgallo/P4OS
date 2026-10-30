/*
 * P4OS - Programmer: Arm Cortex-M chips through their SWD port, as any probe
 * does (OpenOCD, probe-rs): write and verify a board's own firmware, and
 * read its RTT console. docs/plan/PROGRAMADOR.md, "F2" and "Alcance de SWD".
 *
 * The wire is aos_io_swd_* on the pins of a UART port of the header (TX is
 * SWCLK, RX is SWDIO, the port's EN line is NRST), so the same 4-6 wires
 * serve for a serial bootloader and for SWD. In the simulator the wire leads
 * to an emulated target (sim/swd_sim.c).
 *
 * ADIv5 on top of it:
 *   connect    line reset, JTAG-to-SWD, DPIDR. Nobody answering may be a
 *              multidrop target (the RP2040): SWD-to-dormant, the dormant
 *              wake-up, a line reset, TARGETSEL for its core 0, DPIDR again.
 *              An RP2040 left awake by an earlier job does answer the first
 *              DPIDR - all three of its DPs at once, with the same value -
 *              so its DPIDR also leads to a line reset and TARGETSEL: with
 *              them all on the wire every other read is a clash (found on
 *              the board: the second job read the core as "Cortex-M?").
 *              Then the sticky errors cleared and the debug power-up.
 *   MEM-AP     AP 0, 32-bit with auto-increment, TAR set again at every
 *              1 KB (where the increment wraps); reads pipelined (each AP
 *              read gives the previous one's value, RDBUFF the last); a
 *              WAIT is retried, a FAULT cleared and reported.
 *   the core   halted through DHCSR, its registers through DCRSR/DCRDR,
 *              reset through AIRCR (and NRST when there is one).
 *
 * The chip: CPUID says the core, then each family's own registers say the
 * rest - the nRF52's FICR (and its CTRL-AP, AP 1), the STM32's DBGMCU_IDCODE
 * and flash size, the RP2040's SYSINFO.
 *
 * Writing, with no algorithm loaded into the target: each family's flash is
 * driven from here, through the bus.
 *   nRF52       the NVMC: CONFIG to erase, ERASEPAGE, CONFIG to write, the
 *               words written straight to the flash's addresses.
 *   STM32F0/F1  the FLASH controller: unlocked with its keys, PER+STRT for
 *               a page, PG and a halfword at a time, BSY and the error bits
 *               after each.
 *   RP2040      the flash is outside the chip: the boot ROM's own functions
 *               do it (connect, exit XIP, erase, program from a buffer in
 *               RAM, flush, enter XIP), called on the halted core through
 *               the ROM's debug trampoline, as OpenOCD does.
 *   RP2350      the same functions, from a ROM table with a pointer for
 *               each architecture (the Secure Arm one is taken), called
 *               through a stub of ours in RAM (no trampoline here): stack
 *               limits cleared, blx r7, bkpt. Before, ACCESSCTRL back to
 *               its defaults (unless locked against the debugger), CP7 on
 *               and the ROM's state reset; after, the flush and the reset
 *               of the address translation. Its debug port is ADIv6: APs
 *               by address, core 0's at 0x2000, with the MEM-AP registers
 *               at 0xD00 of it. As OpenOCD's rp2xxx.c and rp2350.cfg do;
 *               tried only in the simulator (2026-10-29).
 * Every sector the image touches is erased whole and written (bytes the
 * image does not have there come out 0xFF); then what was written is read
 * back and compared. Verifying is the only reading done by SWD: dumping a
 * chip's flash this way is left out on purpose (see the plan).
 *
 * Read protection is said and respected: an nRF52 with APPROTECT, or an
 * STM32 with RDP, is not written; the only thing offered is the chip's own
 * erase-all (the nRF52's CTRL-AP ERASEALL), which takes the protected
 * firmware away with the protection.
 *
 * The RTT console: the "SEGGER RTT" control block is looked for in the
 * target's RAM while it runs (never halted); then its first up buffer is
 * read every 30 ms into the log, a line at a time, and what
 * aos_flasher_console_send() queued goes into its first down buffer.
 */
#include "aos_flasher_priv.h"
#include "aos_i18n.h"
#include "aos_hal.h"
#include "aos_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KHZ_DEFAULT     1000

/* DP registers (A[3:2]) */
#define DP_DPIDR        0x0
#define DP_ABORT        0x0
#define DP_CTRL         0x4
#define DP_SELECT       0x8
#define DP_RDBUFF       0xC
#define DP_TARGETSEL    0xC
/* MEM-AP */
#define AP_CSW          0x00
#define AP_TAR          0x04
#define AP_DRW          0x0C
#define AP_IDR          0xFC
#define CSW_8           0x23000010u     /* HPROT, debug master, single increment */
#define CSW_16          0x23000011u
#define CSW_32          0x23000012u
/* the core */
#define CPUID           0xE000ED00u
#define AIRCR           0xE000ED0Cu
#define DHCSR           0xE000EDF0u
#define DCRSR           0xE000EDF4u
#define DCRDR           0xE000EDF8u
#define DEMCR           0xE000EDFCu
#define DBGKEY          0xA05F0000u

typedef struct swd swd_t;

typedef struct {
    bool (*begin)(swd_t *s);
    bool (*erase)(swd_t *s, uint32_t addr);                                 /* one sector */
    bool (*program)(swd_t *s, uint32_t addr, const uint8_t *d, uint32_t n); /* within a sector, n % 4 == 0 */
    bool (*erase_all)(swd_t *s);
    void (*end)(swd_t *s);
} flash_ops_t;

struct swd {
    aos_io_swd_t *w;
    uint32_t dpidr, select, csw;
    bool multidrop;
    uint32_t cpuid;
    char chip[24], core[16];
    uint32_t flash_base, flash_size, sector, ram_base, ram_size;
    const flash_ops_t *ops;
    bool prot;
    bool nrf;                       /* has the nRF52's CTRL-AP */
    bool adiv6;                     /* APs by address (DPIDR version 3) */
    uint32_t mem_ap;                /* the MEM-AP: an index (ADIv5), an address (ADIv6) */
    bool rp2350;
    uint32_t uf2_family;
    uint16_t rp_fn[12];             /* the RP2040's/RP2350's ROM functions, in RP_* order */
    uint8_t *buf;                   /* a sector */
};

/* -------------------------------------------------------------------------- */
/* The DP and the APs                                                          */
/* -------------------------------------------------------------------------- */

static uint8_t req_of(bool ap, bool rd, uint32_t a)
{
    uint8_t r = (uint8_t)(0x81 | (ap ? 2 : 0) | (rd ? 4 : 0) | ((a & 0xC) << 1));
    return (uint8_t)(r | (__builtin_popcount(r & 0x1E) & 1) << 5);
}

static void clear_errors(swd_t *s)
{
    uint32_t v = 0x1E;      /* STKCMPCLR, STKERRCLR, WDERRCLR, ORUNERRCLR */
    aos_io_swd_xfer(s->w, req_of(false, false, DP_ABORT), &v, false);
}

/* One packet, a WAIT retried; the ACK */
static int xfer(swd_t *s, bool ap, bool rd, uint32_t a, uint32_t *v)
{
    for (int i = 0; i < 400; i++) {
        uint32_t d = rd ? 0 : *v;
        int ack = aos_io_swd_xfer(s->w, req_of(ap, rd, a), &d, false);
        if (ack == 2) {
            if (i > 40) aos_hal_sleep_ms(1);
            continue;
        }
        if (ack == 1 && rd) *v = d;
        if (ack == 4) clear_errors(s);
        return ack;
    }
    return 2;
}

static bool dp_read(swd_t *s, uint32_t a, uint32_t *v) { return xfer(s, false, true, a, v) == 1; }
static bool dp_write(swd_t *s, uint32_t a, uint32_t v) { return xfer(s, false, false, a, &v) == 1; }

static bool ap_sel(swd_t *s, uint32_t ap, uint32_t reg)
{
    /* ADIv5: APSEL in the top byte; ADIv6: the AP's address, the MEM-AP's
     * registers at 0xD00 of it */
    uint32_t sel = s->adiv6 ? ap + 0xD00 + (reg & 0xF0) : ap << 24 | (reg & 0xF0);
    if (sel == s->select) return true;
    if (!dp_write(s, DP_SELECT, sel)) return false;
    s->select = sel;
    return true;
}

static bool ap_read(swd_t *s, uint32_t ap, uint32_t reg, uint32_t *v)
{
    uint32_t d;
    return ap_sel(s, ap, reg) && xfer(s, true, true, reg, &d) == 1 && dp_read(s, DP_RDBUFF, v);
}

static bool ap_write(swd_t *s, uint32_t ap, uint32_t reg, uint32_t v)
{
    return ap_sel(s, ap, reg) && xfer(s, true, false, reg, &v) == 1;
}

/* STICKYERR after a run of pipelined accesses */
static bool sticky_ok(swd_t *s)
{
    uint32_t c;
    if (!dp_read(s, DP_CTRL, &c)) return false;
    if (c & 0x20) { clear_errors(s); return false; }
    return true;
}

/* -------------------------------------------------------------------------- */
/* Memory                                                                      */
/* -------------------------------------------------------------------------- */

static bool set_csw(swd_t *s, uint32_t csw)
{
    if (csw == s->csw) return true;
    if (!ap_write(s, s->mem_ap, AP_CSW, csw)) return false;
    s->csw = csw;
    return true;
}

static bool rd32(swd_t *s, uint32_t addr, uint32_t *v)
{
    return set_csw(s, CSW_32) && ap_write(s, s->mem_ap, AP_TAR, addr) && ap_read(s, s->mem_ap, AP_DRW, v);
}

static bool wr32(swd_t *s, uint32_t addr, uint32_t v)
{
    return set_csw(s, CSW_32) && ap_write(s, s->mem_ap, AP_TAR, addr) && ap_write(s, s->mem_ap, AP_DRW, v);
}

static bool wr16(swd_t *s, uint32_t addr, uint16_t v)
{
    return set_csw(s, CSW_16) && ap_write(s, s->mem_ap, AP_TAR, addr) && ap_write(s, s->mem_ap, AP_DRW, (uint32_t)v << (8 * (addr & 2)));
}

static bool wr8(swd_t *s, uint32_t addr, uint8_t v)
{
    return set_csw(s, CSW_8) && ap_write(s, s->mem_ap, AP_TAR, addr) && ap_write(s, s->mem_ap, AP_DRW, (uint32_t)v << (8 * (addr & 3)));
}

/* n bytes from addr, both multiples of four */
static bool mem_read(swd_t *s, uint32_t addr, uint8_t *buf, uint32_t n)
{
    if (!set_csw(s, CSW_32)) return false;
    while (n) {
        uint32_t chunk = 1024 - (addr & 1023);
        if (chunk > n) chunk = n;
        uint32_t words = chunk / 4, v;
        if (!ap_write(s, s->mem_ap, AP_TAR, addr) || !ap_sel(s, s->mem_ap, AP_DRW)) return false;
        if (xfer(s, true, true, AP_DRW, &v) != 1) return false;     /* posted: nothing yet */
        for (uint32_t i = 1; i < words; i++) {
            if (xfer(s, true, true, AP_DRW, &v) != 1) return false;
            memcpy(buf + 4 * (i - 1), &v, 4);
        }
        if (!dp_read(s, DP_RDBUFF, &v)) return false;
        memcpy(buf + 4 * (words - 1), &v, 4);
        buf += chunk;
        addr += chunk;
        n -= chunk;
    }
    return sticky_ok(s);
}

static bool mem_write(swd_t *s, uint32_t addr, const uint8_t *buf, uint32_t n)
{
    if (!set_csw(s, CSW_32)) return false;
    while (n) {
        uint32_t chunk = 1024 - (addr & 1023);
        if (chunk > n) chunk = n;
        if (!ap_write(s, s->mem_ap, AP_TAR, addr) || !ap_sel(s, s->mem_ap, AP_DRW)) return false;
        for (uint32_t i = 0; i < chunk; i += 4) {
            uint32_t v;
            memcpy(&v, buf + i, 4);
            if (xfer(s, true, false, AP_DRW, &v) != 1) return false;
        }
        buf += chunk;
        addr += chunk;
        n -= chunk;
    }
    return sticky_ok(s);
}

/* -------------------------------------------------------------------------- */
/* Connecting                                                                  */
/* -------------------------------------------------------------------------- */

static const uint8_t ONES[7] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t ZERO[1] = { 0 };

static void line_reset(swd_t *s)
{
    aos_io_swd_seq(s->w, ONES, 56);
    aos_io_swd_seq(s->w, ZERO, 8);
}

static void jtag_to_swd(swd_t *s)
{
    static const uint8_t SEQ[] = { 0x9E, 0xE7 };
    aos_io_swd_seq(s->w, ONES, 56);
    aos_io_swd_seq(s->w, SEQ, 16);
    line_reset(s);
}

/* SWD-to-dormant, then out of it into SWD (ADIv5.2, B5.3) */
static void dormant_wake(swd_t *s)
{
    static const uint8_t TO_DORMANT[] = { 0xBC, 0xE3 };
    static const uint8_t ALERT[16] = { 0x92, 0xF3, 0x09, 0x62, 0x95, 0x2D, 0x85, 0x86,
                                       0xE9, 0xAF, 0xDD, 0xE3, 0xA2, 0x0E, 0xBC, 0x19 };
    static const uint8_t ACTIVATE[] = { 0xA0, 0x01 };       /* four low, then 0x1A */
    aos_io_swd_seq(s->w, ONES, 56);
    aos_io_swd_seq(s->w, TO_DORMANT, 16);
    aos_io_swd_seq(s->w, ONES, 8);
    aos_io_swd_seq(s->w, ALERT, 128);
    aos_io_swd_seq(s->w, ACTIVATE, 12);
    line_reset(s);
}

#define DPIDR_RP2040    0x0BC12477u
#define TARGETSEL_CORE0 0x01002927u

/* After a line reset: TARGETSEL for the RP2040's core 0 (it has no ACK),
 * and its DPIDR, now from that DP alone. */
static bool select_core0(swd_t *s)
{
    uint32_t sel = TARGETSEL_CORE0;
    aos_io_swd_xfer(s->w, req_of(false, false, DP_TARGETSEL), &sel, true);
    return dp_read(s, DP_DPIDR, &s->dpidr);
}

static bool connect(swd_t *s)
{
    fl_phase(AOS_FLASHER_CONNECTING);
    s->select = 0xFFFFFFFF;
    s->csw = 0;
    jtag_to_swd(s);
    if (!dp_read(s, DP_DPIDR, &s->dpidr)) {
        s->dpidr = 0;
    } else if (s->dpidr == DPIDR_RP2040) {
        /* awake from an earlier job, and every DP answering */
        line_reset(s);
        if (!select_core0(s)) return fl_cancel ? false : fl_fail("%s", _("La RP2040 contestó pero no se pudo elegir su núcleo 0"));
        s->multidrop = true;
    }
    if (!s->dpidr) {
        /* a multidrop DP says nothing until it is named: the RP2040's core 0 */
        dormant_wake(s);
        bool picked = select_core0(s);
        if (picked && s->dpidr == DPIDR_RP2040) {
            s->multidrop = true;
        } else if (!picked) {
            /* or one that only needed waking: no TARGETSEL (a DP that knows
             * TARGETSEL and was not the one named stays quiet until the next
             * line reset, as the RP2350's) */
            dormant_wake(s);
            if (!dp_read(s, DP_DPIDR, &s->dpidr))
                return fl_cancel ? false : fl_fail("%s", _("Nadie contestó por SWD: revisá SWCLK, SWDIO, GND y que el objetivo tenga alimentación"));
        }
    }
    /* DPIDR version 3: ADIv6, APs by address. The one ADIv6 chip known
     * here is the RP2350, whose core 0 is at 0x2000. */
    if ((s->dpidr >> 12 & 0xF) == 3) {
        s->adiv6 = true;
        s->mem_ap = 0x2000;
    }
    clear_errors(s);
    if (!dp_write(s, DP_CTRL, 0x50000000)) return fl_fail("%s", _("El DP no aceptó el pedido de encendido"));
    uint32_t c = 0;
    for (int i = 0; i < 100 && !fl_cancel; i++) {
        if (dp_read(s, DP_CTRL, &c) && (c & 0xA0000000u) == 0xA0000000u) break;
        aos_hal_sleep_ms(1);
    }
    if ((c & 0xA0000000u) != 0xA0000000u) return fl_fail("%s", _("El depurador del objetivo no se encendió"));
    return true;
}

/* -------------------------------------------------------------------------- */
/* The core                                                                    */
/* -------------------------------------------------------------------------- */

static bool halt(swd_t *s)
{
    if (!wr32(s, DHCSR, DBGKEY | 3)) return false;
    for (int i = 0; i < 100; i++) {
        uint32_t v;
        if (rd32(s, DHCSR, &v) && (v & 0x20000)) return true;
        aos_hal_sleep_ms(1);
    }
    return false;
}

static bool reg_write(swd_t *s, int r, uint32_t v)
{
    if (!wr32(s, DCRDR, v) || !wr32(s, DCRSR, 0x10000u | (uint32_t)r)) return false;
    for (int i = 0; i < 50; i++) {
        uint32_t d;
        if (rd32(s, DHCSR, &d) && (d & 0x10000)) return true;
    }
    return false;
}

/* The target back to its own program: debug let go, a system reset. */
static void reset_run(swd_t *s)
{
    fl_phase(AOS_FLASHER_RESETTING);
    wr32(s, DEMCR, 0);
    wr32(s, DHCSR, DBGKEY);
    wr32(s, AIRCR, 0x05FA0004);
    if (aos_io_swd_nrst(s->w, true)) {
        aos_hal_sleep_ms(20);
        aos_io_swd_nrst(s->w, false);
    }
}

/* -------------------------------------------------------------------------- */
/* nRF52                                                                       */
/* -------------------------------------------------------------------------- */

#define NVMC_READY      0x4001E400u
#define NVMC_CONFIG     0x4001E504u
#define NVMC_ERASEPAGE  0x4001E508u
#define NVMC_ERASEALL   0x4001E50Cu

static bool nvmc_wait(swd_t *s, int ms)
{
    for (int i = 0; i < ms; i++) {
        uint32_t v;
        if (rd32(s, NVMC_READY, &v) && (v & 1)) return true;
        aos_hal_sleep_ms(1);
    }
    return false;
}

static bool nrf_begin(swd_t *s) { return halt(s); }

static bool nrf_erase(swd_t *s, uint32_t addr)
{
    bool ok = wr32(s, NVMC_CONFIG, 2) && wr32(s, NVMC_ERASEPAGE, addr) && nvmc_wait(s, 200);
    return wr32(s, NVMC_CONFIG, 0) && ok;
}

static bool nrf_program(swd_t *s, uint32_t addr, const uint8_t *d, uint32_t n)
{
    bool ok = wr32(s, NVMC_CONFIG, 1) && mem_write(s, addr, d, n) && nvmc_wait(s, 100);
    return wr32(s, NVMC_CONFIG, 0) && ok;
}

/* CTRL-AP (AP 1): the erase that works with APPROTECT on */
static bool nrf_ctrlap_erase(swd_t *s)
{
    if (!ap_write(s, 1, 0x04, 1)) return false;
    uint32_t st = 1;
    for (int i = 0; i < 300 && st && !fl_cancel; i++) {
        aos_hal_sleep_ms(50);
        if (!ap_read(s, 1, 0x08, &st)) return false;
    }
    ap_write(s, 1, 0x04, 0);
    ap_write(s, 1, 0x00, 1);        /* RESET */
    aos_hal_sleep_ms(10);
    ap_write(s, 1, 0x00, 0);
    return !st;
}

static bool nrf_erase_all(swd_t *s)
{
    if (s->prot) return nrf_ctrlap_erase(s);
    if (!halt(s)) return false;
    bool ok = wr32(s, NVMC_CONFIG, 2) && wr32(s, NVMC_ERASEALL, 1) && nvmc_wait(s, 2000);
    return wr32(s, NVMC_CONFIG, 0) && ok;
}

static void nrf_end(swd_t *s) { (void)s; }

static const flash_ops_t NRF52 = { nrf_begin, nrf_erase, nrf_program, nrf_erase_all, nrf_end };

static bool nrf_identify(swd_t *s)
{
    uint32_t prot = 1;
    ap_read(s, 1, 0x0C, &prot);
    s->prot = prot == 0;
    s->flash_base = 0;
    s->ram_base = 0x20000000;
    s->ops = &NRF52;
    if (s->prot) {
        snprintf(s->chip, sizeof s->chip, "nRF52");
        return true;
    }
    uint32_t part = 0, variant = 0, pg = 0, npg = 0, ram = 0;
    rd32(s, 0x10000100, &part);
    rd32(s, 0x10000104, &variant);
    rd32(s, 0x10000010, &pg);
    rd32(s, 0x10000014, &npg);
    rd32(s, 0x1000010C, &ram);
    char v[5] = { (char)(variant >> 24), (char)(variant >> 16), (char)(variant >> 8), (char)variant, 0 };
    for (int i = 0; i < 4; i++) if (v[i] < 0x20 || v[i] > 0x7E) v[i] = 0;
    snprintf(s->chip, sizeof s->chip, "nRF%X%s%s", (unsigned)part, v[0] ? " " : "", v);
    s->sector = pg && pg <= 65536 ? pg : 4096;
    s->flash_size = npg && npg < 4096 ? npg * s->sector : 0;
    s->ram_size = ram && ram <= 1024 ? ram << 10 : 0x10000;
    s->uf2_family = part == 0x52840 ? 0xADA52840 : part == 0x52833 ? 0x621E937A : part == 0x52832 ? 0x1B57745F : 0;
    uint32_t id0 = 0, id1 = 0;
    if (rd32(s, 0x10000060, &id0) && rd32(s, 0x10000064, &id1))
        fl_logf(L_INFO, "DEVICEID %08X%08X", (unsigned)id1, (unsigned)id0);
    return true;
}

/* -------------------------------------------------------------------------- */
/* STM32F0 / F1                                                                */
/* -------------------------------------------------------------------------- */

#define FL_KEYR 0x40022004u
#define FL_SR   0x4002200Cu
#define FL_CR   0x40022010u
#define FL_AR   0x40022014u
#define FL_OBR  0x4002201Cu

static bool stm_wait(swd_t *s, int ms)
{
    for (int i = 0; i <= ms; i++) {
        uint32_t sr;
        if (!rd32(s, FL_SR, &sr)) return false;
        if (!(sr & 1)) {
            if (sr & 0x14) {            /* PGERR, WRPRTERR */
                wr32(s, FL_SR, 0x34);
                return false;
            }
            return true;
        }
        if (ms) aos_hal_sleep_ms(1);
    }
    return false;
}

static bool stm_begin(swd_t *s)
{
    uint32_t cr = 0x80;
    if (!halt(s)) return false;
    wr32(s, FL_SR, 0x34);
    if (!wr32(s, FL_KEYR, 0x45670123) || !wr32(s, FL_KEYR, 0xCDEF89AB) || !rd32(s, FL_CR, &cr)) return false;
    return !(cr & 0x80);
}

static bool stm_erase(swd_t *s, uint32_t addr)
{
    bool ok = wr32(s, FL_CR, 2) && wr32(s, FL_AR, addr) && wr32(s, FL_CR, 0x42) && stm_wait(s, 100);
    return wr32(s, FL_CR, 0) && ok;
}

static bool stm_program(swd_t *s, uint32_t addr, const uint8_t *d, uint32_t n)
{
    bool ok = wr32(s, FL_CR, 1);
    for (uint32_t i = 0; ok && i < n && !fl_cancel; i += 2)
        ok = wr16(s, addr + i, (uint16_t)(d[i] | d[i + 1] << 8)) && stm_wait(s, 5);
    return wr32(s, FL_CR, 0) && ok && !fl_cancel;
}

static bool stm_erase_all(swd_t *s)
{
    if (!stm_begin(s)) return false;
    bool ok = wr32(s, FL_CR, 4) && wr32(s, FL_CR, 0x44) && stm_wait(s, 3000);
    wr32(s, FL_CR, 0x80);
    return ok;
}

static void stm_end(swd_t *s) { wr32(s, FL_CR, 0x80); }

static const flash_ops_t STM32F1 = { stm_begin, stm_erase, stm_program, stm_erase_all, stm_end };

static const struct { uint16_t dev; const char *name; uint16_t page; uint16_t ram_kb; bool f0; } STM_DEVS[] = {
    { 0x412, "STM32F1 low", 1024, 10, false },  { 0x410, "STM32F1 medium", 1024, 20, false },
    { 0x414, "STM32F1 high", 2048, 64, false }, { 0x430, "STM32F1 XL", 2048, 96, false },
    { 0x418, "STM32F105/107", 2048, 64, false },    { 0x420, "STM32F100", 1024, 8, false },
    { 0x444, "STM32F03x", 1024, 4, true },          { 0x445, "STM32F04x", 1024, 6, true },
    { 0x440, "STM32F05x", 1024, 8, true },          { 0x448, "STM32F07x", 2048, 16, true },
    { 0x442, "STM32F09x", 2048, 32, true },
};

/* Families with a different flash controller: named, not written yet */
static const struct { uint16_t dev; const char *name; } STM_OTHER[] = {
    { 0x413, "STM32F405/407" }, { 0x419, "STM32F42x/43x" }, { 0x423, "STM32F401xB/C" }, { 0x433, "STM32F401xD/E" },
    { 0x431, "STM32F411" }, { 0x441, "STM32F412" }, { 0x421, "STM32F446" }, { 0x460, "STM32G07x/08x" },
    { 0x466, "STM32G03x/04x" }, { 0x468, "STM32G43x/44x" }, { 0x469, "STM32G47x/48x" }, { 0x415, "STM32L47x/48x" },
    { 0x435, "STM32L43x/44x" }, { 0x450, "STM32H74x/75x" }, { 0x417, "STM32L0x3" }, { 0x416, "STM32L1" },
};

static bool stm_identify(swd_t *s, bool m0)
{
    uint32_t id = 0;
    if (!rd32(s, m0 ? 0x40015800u : 0xE0042000u, &id) || !(id & 0xFFF)) return false;
    uint16_t dev = id & 0xFFF;
    s->flash_base = 0x08000000;
    s->ram_base = 0x20000000;
    for (size_t i = 0; i < sizeof STM_DEVS / sizeof STM_DEVS[0]; i++) {
        if (STM_DEVS[i].dev != dev) continue;
        uint32_t kb = 0;
        rd32(s, STM_DEVS[i].f0 ? 0x1FFFF7CCu : 0x1FFFF7E0u, &kb);
        snprintf(s->chip, sizeof s->chip, "%s", STM_DEVS[i].name);
        s->sector = STM_DEVS[i].page;
        s->flash_size = (kb & 0xFFFF) << 10;
        s->ram_size = (uint32_t)STM_DEVS[i].ram_kb << 10;
        s->ops = &STM32F1;
        s->uf2_family = 0x5EE21072;     /* STM32F1; the F0's is not common */
        uint32_t obr = 0;
        if (rd32(s, FL_OBR, &obr)) s->prot = obr & 2;
        uint32_t u[3];
        uint32_t ua = STM_DEVS[i].f0 ? 0x1FFFF7ACu : 0x1FFFF7E8u;
        if (rd32(s, ua, &u[0]) && rd32(s, ua + 4, &u[1]) && rd32(s, ua + 8, &u[2]))
            fl_logf(L_INFO, "UID %08X%08X%08X", (unsigned)u[2], (unsigned)u[1], (unsigned)u[0]);
        return true;
    }
    for (size_t i = 0; i < sizeof STM_OTHER / sizeof STM_OTHER[0]; i++)
        if (STM_OTHER[i].dev == dev) { snprintf(s->chip, sizeof s->chip, "%s", STM_OTHER[i].name); return true; }
    snprintf(s->chip, sizeof s->chip, "STM32 (0x%03X)", dev);
    return true;
}

/* -------------------------------------------------------------------------- */
/* RP2040                                                                      */
/* -------------------------------------------------------------------------- */

enum { RP_IF, RP_EX, RP_RE, RP_RP, RP_FC, RP_CX, RP_DT, RP_DE, RP_SR, RP_RA, RP_N };
static const char RP_CODES[RP_N][2] = { "IF", "EX", "RE", "RP", "FC", "CX", "DT", "DE", "SR", "RA" };
/* which each chip needs (RA is optional on the RP2350) */
#define RP2040_NEEDS ((1u << RP_IF) | (1u << RP_EX) | (1u << RP_RE) | (1u << RP_RP) | (1u << RP_FC) | (1u << RP_CX) | \
                      (1u << RP_DT) | (1u << RP_DE))
#define RP2350_NEEDS ((1u << RP_IF) | (1u << RP_EX) | (1u << RP_RE) | (1u << RP_RP) | (1u << RP_FC) | (1u << RP_SR))
#define RP_BUF      0x20010000u     /* where the data to program goes */
#define RP_STACK    0x20040000u
#define RP_STUB     0x20000000u     /* the RP2350's call stub */

/* RP2350's ROM table flags: a pointer follows for each one set */
#define RT_FLAG_ARM_SEC 0x04

/* The boot ROM's table of functions: its 16-bit address at 0x14, then
 * pairs of 16-bit (code, address) up to a code of 0. The table need not
 * start on a word, and the bus has no unaligned accesses: it is read as
 * aligned words and taken apart by halfwords (found on the board: read by
 * words at its own address it came out shifted, and nothing was found). */
static void rp_take(swd_t *s, uint16_t code, uint16_t at)
{
    for (int k = 0; k < RP_N; k++)
        if (code == (uint16_t)(RP_CODES[k][0] | RP_CODES[k][1] << 8)) s->rp_fn[k] = at;
}

/* The RP2350's: from 0x7C00 up (its A1 and later ROMs) a 16-bit code and
 * 16-bit flags, then a halfword for each flag set, in the order of the
 * bits; below that (the A0's), code, flags, one pointer: six bytes. */
static void rp2350_walk(swd_t *s, const uint8_t *b, uint32_t o, uint32_t n, bool a1)
{
    while (o + 4 <= n) {
        uint16_t code = (uint16_t)(b[o] | b[o + 1] << 8), flags = (uint16_t)(b[o + 2] | b[o + 3] << 8);
        if (!code) break;
        int ptrs = a1 ? __builtin_popcount(flags) : 1;
        if ((flags & RT_FLAG_ARM_SEC) && o + 4 + 2u * (uint32_t)ptrs <= n) {
            uint32_t i = a1 ? (uint32_t)__builtin_popcount(flags & (RT_FLAG_ARM_SEC - 1)) : 0;
            rp_take(s, code, (uint16_t)(b[o + 4 + 2 * i] | b[o + 5 + 2 * i] << 8));
        }
        o += 4 + 2u * (uint32_t)ptrs;
    }
}

static bool rp_lookup(swd_t *s)
{
    const char *who = s->rp2350 ? "RP2350" : "RP2040";
    uint32_t v;
    uint32_t magic = s->rp2350 ? 0x02754D : 0x01754D;      /* "Mu" and the ROM's version */
    if (!rd32(s, 0x10, &v) || (v & 0xFFFFFF) != magic) {
        fl_logf(L_ERR, _("%s: la ROM no tiene su marca (0x%06X)"), who, (unsigned)(v & 0xFFFFFF));
        return false;
    }
    if (!rd32(s, 0x14, &v)) return false;
    uint32_t table = v & 0xFFFF, from = table & ~3u, n = 1024;
    if (s->rp2350 && from + n > 0x8000) n = 0x8000 - from;      /* the ROM ends at 32 KB */
    uint8_t *b = s->buf;
    if (!n || !mem_read(s, from, b, n)) return false;
    memset(s->rp_fn, 0, sizeof s->rp_fn);
    if (s->rp2350) {
        rp2350_walk(s, b, table - from, n, table >= 0x7C00);
    } else {
        for (uint32_t o = table - from; o + 4 <= n; o += 4) {
            uint16_t code = (uint16_t)(b[o] | b[o + 1] << 8);
            if (!code) break;
            rp_take(s, code, (uint16_t)(b[o + 2] | b[o + 3] << 8));
        }
    }
    uint32_t need = s->rp2350 ? RP2350_NEEDS : RP2040_NEEDS;
    for (int k = 0; k < RP_N; k++) {
        if (s->rp_fn[k] || !(need & (1u << k))) continue;
        fl_logf(L_ERR, _("%s: a la tabla de la ROM (0x%04X) le falta %.2s"), who, (unsigned)table, RP_CODES[k]);
        return false;
    }
    return true;
}

/* A ROM function on the halted core, with r0-r3. RP2040: through
 * debug_trampoline, which calls r7 and stops at debug_trampoline_end.
 * RP2350: through our stub at RP_STUB, which clears the stack limits,
 * calls r7 and stops at its bkpt. */
static bool rp_call(swd_t *s, int fn, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, int ms)
{
    uint32_t args[4] = { r0, r1, r2, r3 };
    for (int i = 0; i < 4; i++) if (!reg_write(s, i, args[i])) return false;
    uint32_t pc = s->rp2350 ? RP_STUB : s->rp_fn[RP_DT] & ~1u, lr = s->rp2350 ? (RP_STUB + 12) | 1 : s->rp_fn[RP_DE];
    if (!reg_write(s, 7, s->rp_fn[fn]) || !reg_write(s, 13, RP_STACK) || !reg_write(s, 14, lr) ||
        !reg_write(s, 15, pc) || !reg_write(s, 16, 0x01000000))
        return false;
    if (!wr32(s, DHCSR, DBGKEY | 1)) return false;
    for (int i = 0; i < ms; i++) {
        uint32_t v;
        if (rd32(s, DHCSR, &v) && (v & 0x20000)) return true;
        aos_hal_sleep_ms(1);
    }
    halt(s);
    return false;
}

/* The RP2350's stub: movs r6,#0; msr MSPLIM,r6; msr PSPLIM,r6; blx r7;
 * bkpt #0 (at +12); nop */
static const uint8_t RP2350_STUB[16] = { 0x00, 0x26, 0x86, 0xF3, 0x0A, 0x88, 0x86, 0xF3,
                                         0x0B, 0x88, 0xB8, 0x47, 0x00, 0xBE, 0x00, 0xBF };

/* What OpenOCD does before calling the RP2350's ROM: ACCESSCTRL back to
 * its defaults, so the core may reach the flash's QMI whatever the
 * firmware set (unless the firmware locked it against the debugger, which
 * is respected); the coprocessor the ROM's hardening uses, CP7, enabled;
 * the stub in RAM; the ROM's own state reset. */
#define ACCESSCTRL_LOCK     0x40060000u
#define ACCESSCTRL_CFGRESET 0x40060008u
#define CPACR               0xE000ED88u

static bool rp2350_prepare(swd_t *s)
{
    uint32_t lock = 0, cpacr = 0;
    if (!rd32(s, ACCESSCTRL_LOCK, &lock)) return false;
    if (lock & 0x08) {
        fl_logf(L_ERR, "%s", _("RP2350: el firmware bloqueó ACCESSCTRL para el depurador: no se toca"));
        return false;
    }
    if (!wr32(s, ACCESSCTRL_CFGRESET, 0xACCE0001)) return false;
    if (!rd32(s, CPACR, &cpacr) || !wr32(s, CPACR, cpacr | 3u << 14)) return false;
    if (!mem_write(s, RP_STUB, RP2350_STUB, sizeof RP2350_STUB)) return false;
    if (!rp_call(s, RP_SR, 0x01, 0, 0, 0, 100)) {       /* BOOTROM_STATE_RESET_CURRENT_CORE */
        fl_logf(L_ERR, "%s", _("RP2350: bootrom_state_reset no volvió"));
        return false;
    }
    return true;
}

/* Each step says what failed: on the board that is all there is to go by */
static bool rp_begin(swd_t *s)
{
    if (!halt(s)) { fl_logf(L_ERR, _("%s: el núcleo no se detuvo"), s->chip); return false; }
    if (!rp_lookup(s)) return false;
    if (s->rp2350 && !rp2350_prepare(s)) return false;
    if (!rp_call(s, RP_IF, 0, 0, 0, 0, 100)) { fl_logf(L_ERR, _("%s: connect_internal_flash no volvió"), s->chip); return false; }
    if (!rp_call(s, RP_EX, 0, 0, 0, 0, 100)) { fl_logf(L_ERR, _("%s: flash_exit_xip no volvió"), s->chip); return false; }
    return true;
}

static bool rp_erase(swd_t *s, uint32_t addr)
{
    return rp_call(s, RP_RE, addr - 0x10000000u, 4096, 1u << 16, 0xD8, 2000);
}

static bool rp_program(swd_t *s, uint32_t addr, const uint8_t *d, uint32_t n)
{
    n = (n + 255) & ~255u;          /* the ROM programs whole 256-byte pages */
    return mem_write(s, RP_BUF, d, n) && rp_call(s, RP_RP, addr - 0x10000000u, RP_BUF, n, 0, 2000);
}

/* Back to XIP: the RP2040 by its CX; the RP2350's ROM leaves the flash
 * readable already, and resets its address translation (RA) */
static void rp_end(swd_t *s)
{
    rp_call(s, RP_FC, 0, 0, 0, 0, 100);
    if (!s->rp2350) rp_call(s, RP_CX, 0, 0, 0, 0, 100);
    else if (s->rp_fn[RP_RA]) rp_call(s, RP_RA, 0, 0, 0, 0, 100);
}

static bool rp_erase_all(swd_t *s)
{
    /* the first 2 MB, the usual flash; a bigger one keeps the rest */
    if (!rp_begin(s)) return false;
    bool ok = rp_call(s, RP_RE, 0, 2u << 20, 1u << 16, 0xD8, 30000);
    rp_end(s);
    return ok;
}

static const flash_ops_t RP2040 = { rp_begin, rp_erase, rp_program, rp_erase_all, rp_end };

/* -------------------------------------------------------------------------- */
/* Which chip                                                                  */
/* -------------------------------------------------------------------------- */

static bool identify(swd_t *s)
{
    fl_phase(AOS_FLASHER_INFO);
    uint32_t idr = 0;
    ap_read(s, s->mem_ap, AP_IDR, &idr);
    uint32_t ap1 = 0;
    s->nrf = !s->adiv6 && ap_read(s, 1, AP_IDR, &ap1) && ap1 == 0x02880000;
    /* a protected nRF52 answers nothing on the bus: it is an M4 */
    if (!rd32(s, CPUID, &s->cpuid)) s->cpuid = s->nrf ? 0x410FC241 : 0;
    uint16_t part = (s->cpuid >> 4) & 0xFFF;
    const char *core = part == 0xC60 ? "Cortex-M0+" : part == 0xC20 ? "Cortex-M0" : part == 0xC23 ? "Cortex-M3"
                     : part == 0xC24 ? "Cortex-M4" : part == 0xC27 ? "Cortex-M7" : part == 0xD21 ? "Cortex-M33"
                     : part == 0xD20 ? "Cortex-M23" : "Cortex-M?";
    snprintf(s->core, sizeof s->core, "%s", core);
    s->ram_base = 0x20000000;
    s->ram_size = 0x10000;

    uint32_t v = 0;
    if (s->nrf) {
        nrf_identify(s);
    } else if (s->multidrop || (part == 0xC60 && rd32(s, 0x40000000, &v) && (v & 0x0FFFFFFF) == 0x0002927)) {
        snprintf(s->chip, sizeof s->chip, "RP2040");
        s->flash_base = 0x10000000;
        s->flash_size = 0;          /* outside the chip: not known from here */
        s->sector = 4096;
        s->ram_size = 264 << 10;
        s->ops = &RP2040;
        s->uf2_family = 0xE48BFF56;
    } else if (part == 0xD21 && rd32(s, 0x40000000, &v) && (v & 0x0FFFFFFF) == 0x0004927) {
        snprintf(s->chip, sizeof s->chip, "RP2350");
        s->rp2350 = true;
        s->flash_base = 0x10000000;
        s->flash_size = 0;
        s->sector = 4096;
        s->ram_size = 520 << 10;
        s->ops = &RP2040;           /* the same calls, rp_* tell the two apart */
        s->uf2_family = 0xE48BFF59; /* Arm Secure; uf2_fits() takes its siblings too */
    } else if (s->adiv6) {
        snprintf(s->chip, sizeof s->chip, "ADIv6 %s", core);
    } else if (!stm_identify(s, part == 0xC20 || part == 0xC60)) {
        snprintf(s->chip, sizeof s->chip, "%s", core);
    }

    fl_lock();
    fl_st.chip_known = true;
    snprintf(fl_st.chip, sizeof fl_st.chip, "%.15s", s->chip);
    snprintf(fl_st.boot, sizeof fl_st.boot, "%s, DPIDR %08X", core, (unsigned)s->dpidr);
    fl_st.flash_size = s->flash_size;
    fl_st.page = (uint16_t)(s->sector > 65535 ? 0 : s->sector);
    fl_st.secure = s->prot;
    fl_unlock();
    char fl[24] = "?";
    if (s->flash_size) snprintf(fl, sizeof fl, "%u KB", (unsigned)(s->flash_size >> 10));
    fl_logf(L_OK, "%s (%s%s), flash %s", s->chip, core, s->multidrop ? ", multidrop" : "", fl);
    if (s->prot) fl_logf(L_WARN, "%s", _("Protegido contra lectura: sólo se puede borrar entero"));
    else if (!s->ops) fl_logf(L_WARN, "%s", _("Todavía no sé grabar la flash de este chip"));
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

static int image_byte(const aos_flasher_image_t *img, uint32_t addr)
{
    for (int i = 0; i < img->n; i++)
        if (addr >= img->r[i].addr && addr - img->r[i].addr < img->r[i].len) return img->r[i].data[addr - img->r[i].addr];
    return -1;
}

static bool sector_used(const aos_flasher_image_t *img, uint32_t addr, uint32_t len)
{
    for (int i = 0; i < img->n; i++)
        if (img->r[i].addr < addr + len && img->r[i].addr + img->r[i].len > addr) return true;
    return false;
}

static bool write_image(swd_t *s)
{
    if (s->prot) return fl_fail("%s", _("El chip está protegido contra lectura: no se graba. Borralo entero primero."));
    if (!s->ops) return fl_fail(_("Todavía no sé grabar la flash de %s"), s->chip);
    if (J.src.project) return fl_fail("%s", _("Una carpeta de ESP-IDF no es para este chip"));
    aos_flasher_image_t img;
    char err[96];
    uint32_t at = J.src.kind == AOS_FLASHER_IMAGE ? 0 : J.src.files[0].offset;
    if (at < s->flash_base) at += s->flash_base;        /* a .bin: from the start of the flash */
    if (!aos_flasher_image_load(J.src.path, at, &img, err, sizeof err)) return fl_fail("%s: %s", J.src.name, err);
    bool ok = false;
    uint32_t limit = s->flash_size ? s->flash_size : 16u << 20;
    /* an RP2350 takes its "absolute", Arm Secure, RISC-V and Arm
     * Non-secure families (0xE48BFF57, 59, 5A, 5B) */
    bool fam_ok = !img.uf2_family || !s->uf2_family || img.uf2_family == s->uf2_family ||
                  (s->rp2350 && (img.uf2_family == 0xE48BFF57 || (img.uf2_family >= 0xE48BFF59 && img.uf2_family <= 0xE48BFF5B)));
    if (!fam_ok) {
        fl_fail(_("%s es un .uf2 para otro chip (familia 0x%08X)"), J.src.name, (unsigned)img.uf2_family);
        goto out;
    }
    for (int i = 0; i < img.n; i++) {
        if (img.r[i].addr < s->flash_base || img.r[i].addr + img.r[i].len > s->flash_base + limit) {
            fl_fail(_("0x%08X queda fuera de la flash (0x%08X, %u KB)"), (unsigned)img.r[i].addr, (unsigned)s->flash_base,
                    (unsigned)(limit >> 10));
            goto out;
        }
    }
    uint32_t sec = s->sector, nsec = 0;
    for (uint32_t a = s->flash_base; a < s->flash_base + limit; a += sec) nsec += sector_used(&img, a, sec);
    bool verify = !J.opts.no_verify;
    fl_lock();
    fl_st.file_count = 1;
    fl_st.file_index = 0;
    snprintf(fl_st.file_name, sizeof fl_st.file_name, "%s", J.src.name);
    fl_st.file_offset = img.r[0].addr;
    fl_st.total = nsec * sec * (verify ? 2 : 1);
    fl_st.done = 0;
    fl_st.verified = false;
    fl_unlock();
    fl_logf(L_INFO, _("%u sectores de %u KB desde 0x%08X"), (unsigned)nsec, (unsigned)(sec >> 10), (unsigned)img.r[0].addr);
    fl_phase(AOS_FLASHER_ERASING);
    if (!s->ops->begin(s)) { fl_fail("%s", _("No se pudo preparar la flash (detener el núcleo, desbloquearla)")); goto out; }
    for (uint32_t a = s->flash_base; a < s->flash_base + limit && !fl_cancel; a += sec) {
        if (!sector_used(&img, a, sec)) continue;
        uint32_t last = 0;
        for (uint32_t i = 0; i < sec; i++) {
            int b = image_byte(&img, a + i);
            s->buf[i] = b < 0 ? 0xFF : (uint8_t)b;
            if (b >= 0 && b != 0xFF) last = i + 1;
        }
        uint64_t t0 = aos_hal_uptime_ms();
        fl_phase(AOS_FLASHER_ERASING);
        if (!s->ops->erase(s, a)) { if (!fl_cancel) fl_fail(_("0x%08X: no se pudo borrar"), (unsigned)a); s->ops->end(s); goto out; }
        fl_phase(AOS_FLASHER_WRITING);
        last = (last + 3) & ~3u;     /* what is all 0xFF after the last byte needs no writing */
        if (last && !s->ops->program(s, a, s->buf, last)) {
            if (!fl_cancel) fl_fail(_("0x%08X: falló la escritura"), (unsigned)a);
            s->ops->end(s);
            goto out;
        }
        timed(t0, sec);
        fl_progress(sec);
    }
    s->ops->end(s);
    if (fl_cancel) goto out;
    fl_logf(L_OK, _("%s: escrito"), J.src.name);
    if (verify) {
        fl_phase(AOS_FLASHER_VERIFYING);
        for (uint32_t a = s->flash_base; a < s->flash_base + limit && !fl_cancel; a += sec) {
            if (!sector_used(&img, a, sec)) continue;
            uint64_t t0 = aos_hal_uptime_ms();
            if (!mem_read(s, a, s->buf, sec)) { if (!fl_cancel) fl_fail(_("0x%08X: falló la lectura"), (unsigned)a); goto out; }
            timed(t0, sec);
            for (uint32_t i = 0; i < sec; i++) {
                int b = image_byte(&img, a + i);
                if (b >= 0 && s->buf[i] != b) {
                    fl_fail(_("0x%08X: el chip tiene %02X y el archivo %02X"), (unsigned)(a + i), s->buf[i], b);
                    goto out;
                }
            }
            fl_progress(sec);
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

static bool erase_all(swd_t *s)
{
    if (!s->ops && !s->nrf) return fl_fail(_("Todavía no sé borrar la flash de %s"), s->chip);
    if (s->prot && !s->nrf)
        return fl_fail("%s", _("Para quitar la protección de este chip hace falta cambiar sus option bytes: todavía no"));
    fl_phase(AOS_FLASHER_ERASING);
    fl_logf(L_INFO, "%s", s->prot ? _("Borrando todo por el CTRL-AP: se va la protección y el firmware") : _("Borrando toda la flash"));
    uint64_t t0 = aos_hal_uptime_ms();
    bool ok = s->nrf ? nrf_erase_all(s) : s->ops->erase_all(s);
    if (fl_cancel) return false;
    if (!ok) return fl_fail("%s", _("No se pudo borrar"));
    uint32_t ms = (uint32_t)(aos_hal_uptime_ms() - t0);
    fl_logf(L_OK, _("Flash borrada en %u,%u s"), (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
    if (s->prot) {
        s->prot = false;
        fl_lock();
        fl_st.secure = false;
        fl_unlock();
    }
    return true;
}

/* ---- the RTT console ---- */

static bool rtt_find(swd_t *s, uint32_t *cb)
{
    static const char ID[] = "SEGGER RTT";
    fl_logf(L_INFO, _("Buscando el bloque de RTT en 0x%08X (%u KB)"), (unsigned)s->ram_base, (unsigned)(s->ram_size >> 10));
    for (uint32_t off = 0; off < s->ram_size && !fl_cancel; off += 4096) {
        uint32_t n = s->ram_size - off < 4096 ? s->ram_size - off : 4096;
        if (!mem_read(s, s->ram_base + off, s->buf, n)) return false;
        for (uint32_t i = 0; i + 16 <= n; i += 4) {
            if (memcmp(s->buf + i, ID, sizeof ID)) continue;
            *cb = s->ram_base + off + i;
            return true;
        }
    }
    return false;
}

typedef struct { uint32_t at, buf, size, wr, rd; } ring_t;

static bool ring_read(swd_t *s, uint32_t desc, ring_t *r)
{
    uint8_t d[24];
    if (!mem_read(s, desc, d, 24)) return false;
    r->at = desc;
    memcpy(&r->buf, d + 4, 4);
    memcpy(&r->size, d + 8, 4);
    memcpy(&r->wr, d + 12, 4);
    memcpy(&r->rd, d + 16, 4);
    return r->size && r->size < (1u << 20) && r->wr < r->size && r->rd < r->size;
}

/* bytes [from, from+n) of target memory, any alignment */
static bool read_bytes(swd_t *s, uint32_t from, uint8_t *out, uint32_t n)
{
    uint32_t a = from & ~3u, end = (from + n + 3) & ~3u;
    if (end - a > 4096 + 8) return false;
    if (!mem_read(s, a, s->buf, end - a)) return false;
    memcpy(out, s->buf + (from - a), n);
    return true;
}

static void line_out(char *line, int *len)
{
    while (*len && (line[*len - 1] == '\r' || line[*len - 1] == '\n')) (*len)--;
    line[*len] = 0;
    fl_logf(L_OK, "%s", line);
    *len = 0;
}

static bool console(swd_t *s)
{
    uint32_t cb;
    if (!rtt_find(s, &cb)) {
        if (fl_cancel) return false;
        return fl_fail("%s", _("No hay bloque de RTT en la RAM: el firmware tiene que usar SEGGER RTT (y estar corriendo)"));
    }
    uint32_t nup = 0, ndn = 0;
    rd32(s, cb + 16, &nup);
    rd32(s, cb + 20, &ndn);
    uint32_t up_at = cb + 24, dn_at = cb + 24 + 24 * (nup ? nup : 1);
    ring_t up, dn;
    if (!nup || !ring_read(s, up_at, &up)) return fl_fail("%s", _("El bloque de RTT no tiene un buffer de subida válido"));
    bool has_down = ndn && ring_read(s, dn_at, &dn);
    fl_logf(L_INFO, _("RTT en 0x%08X: %u bytes de subida%s"), (unsigned)cb, (unsigned)up.size,
            has_down ? _(", se le puede escribir") : "");
    fl_phase(AOS_FLASHER_CONSOLE);
    static char line[AOS_FLASHER_LOG_LINE];
    int len = 0;
    uint64_t last_byte = 0;
    uint8_t chunk[256];
    while (!fl_cancel) {
        uint32_t wr;
        if (!rd32(s, up.at + 12, &wr) || wr >= up.size) {
            aos_hal_sleep_ms(100);
            if (!fl_cancel && !sticky_ok(s)) return fl_fail("%s", _("Se perdió el contacto con el objetivo"));
            continue;
        }
        while (up.rd != wr && !fl_cancel) {
            uint32_t n = wr > up.rd ? wr - up.rd : up.size - up.rd;
            if (n > sizeof chunk) n = sizeof chunk;
            if (!read_bytes(s, up.buf + up.rd, chunk, n)) break;
            up.rd = (up.rd + n) % up.size;
            wr32(s, up.at + 16, up.rd);
            J.io_bytes += n;
            for (uint32_t i = 0; i < n; i++) {
                char c = (char)chunk[i];
                if (c == '\n') { line_out(line, &len); continue; }
                if (len >= (int)sizeof line - 1) line_out(line, &len);
                line[len++] = c;
            }
            last_byte = aos_hal_uptime_ms();
        }
        if (len && aos_hal_uptime_ms() - last_byte > 400) line_out(line, &len);    /* a prompt with no newline */
        /* what was typed, into the down buffer */
        uint8_t out[64];
        int k = has_down ? fl_console_take(out, sizeof out) : 0;
        if (k > 0) {
            uint32_t drd = 0;
            rd32(s, dn.at + 16, &drd);
            for (int i = 0; i < k; i++) {
                uint32_t next = (dn.wr + 1) % dn.size;
                if (next == drd) break;                 /* full: the rest is lost */
                wr8(s, dn.buf + dn.wr, out[i]);
                dn.wr = next;
            }
            wr32(s, dn.at + 12, dn.wr);
        }
        aos_hal_sleep_ms(30);
    }
    if (len) line_out(line, &len);
    return false;       /* a cancel: the core counts it as the end it is */
}

/* The clock as it comes out of the pins, timed here: idle cycles clocked
 * in one go are SWCLK alone (512 a call, in one critical section), and a
 * DPIDR read is a whole packet with what it costs between two (48 clocks
 * of request, turnarounds, ACK, data, parity and idle). Detectar logs it:
 * the delay loop's khz is only a guess until measured. */
static void measure_clock(swd_t *s)
{
    static const uint8_t IDLE[64] = { 0 };
    uint64_t t0 = aos_hal_uptime_us();
    for (int i = 0; i < 20; i++) aos_io_swd_seq(s->w, IDLE, 512);
    uint64_t t_seq = aos_hal_uptime_us() - t0;
    uint32_t v;
    int ok = 0;
    t0 = aos_hal_uptime_us();
    for (int i = 0; i < 200; i++) ok += aos_io_swd_xfer(s->w, req_of(false, true, DP_DPIDR), &v, false) == 1 && v == s->dpidr;
    uint64_t t_rd = aos_hal_uptime_us() - t0;
    if (!t_seq || !t_rd) return;
    fl_logf(L_INFO, _("SWCLK medido: %u kHz; una lectura del DP cada %u µs (%u kHz efectivos, %d de 200 bien)"),
            (unsigned)(20u * 512u * 1000u / t_seq), (unsigned)(t_rd / 200), (unsigned)(200u * 48u * 1000u / t_rd), ok);
}

/* The port's pins as the wire: TX is SWCLK, RX is SWDIO, EN is NRST (when
 * the port has it). Also the probe's (aos_flasher_dap.c). NULL after
 * fl_fail. */
aos_io_swd_t *fl_swd_open(uint32_t khz)
{
    const aos_io_port_t *pt = aos_io_port_find(J.port);
    if (!pt || pt->kind != AOS_PORT_UART) {
        fl_fail(_("SWD va por los pines de un puerto del header (uart.a, uart.b): %s no es uno"), J.port);
        return NULL;
    }
    int en = -1, boot = -1;
    aos_io_port_lines(J.port, &en, &boot);
    aos_io_swd_t *w = aos_io_swd_open(pt->pins[0], pt->pins[1], en, khz, AOS_FLASHER_OWNER);
    if (!w) {
        const char *o = aos_io_owner(pt->pins[0]);
        if (!o) o = aos_io_owner(pt->pins[1]);
        if (o) fl_fail(_("%s está ocupado: lo tiene %s"), J.port, o);
        else fl_fail(_("No se pudo abrir %s"), J.port);
        return NULL;
    }
    if (en >= 0) fl_logf(L_INFO, _("SWCLK GPIO%d, SWDIO GPIO%d, NRST GPIO%d, ~%u kHz"), pt->pins[0], pt->pins[1], en, (unsigned)khz);
    else fl_logf(L_INFO, _("SWCLK GPIO%d, SWDIO GPIO%d, sin NRST, ~%u kHz"), pt->pins[0], pt->pins[1], (unsigned)khz);
    fl_lock();
    fl_st.lines = en >= 0;
    fl_unlock();
    return w;
}

static bool run(swd_t *s)
{
    fl_phase(AOS_FLASHER_OPENING);
    uint32_t khz = J.opts.baud && J.opts.baud <= 20000 ? J.opts.baud : KHZ_DEFAULT;
    s->w = fl_swd_open(khz);
    if (!s->w) return false;
    if (!connect(s)) return false;
    fl_logf(L_INFO, "DPIDR %08X%s", (unsigned)s->dpidr, s->multidrop ? " (TARGETSEL 01002927)" : "");
    if (!identify(s)) return false;
    switch (J.job) {
    case AOS_FLASHER_JOB_DETECT: measure_clock(s); return true;
    case AOS_FLASHER_JOB_FLASH: return write_image(s);
    case AOS_FLASHER_JOB_READ: return fl_fail("%s", _("Por SWD no se lee la flash entera: sólo se verifica lo grabado"));
    case AOS_FLASHER_JOB_ERASE: return erase_all(s);
    case AOS_FLASHER_JOB_CONSOLE: return console(s);
    default: return false;
    }
}

bool fl_swd_run(void)
{
    swd_t *s = calloc(1, sizeof *s);
    if (!s) return fl_fail("%s", _("Sin memoria"));
    s->buf = malloc(65536 + 16);     /* a sector, up to the 64 KB of an STM32F4's bigger ones */
    bool ok = s->buf ? run(s) : fl_fail("%s", _("Sin memoria"));
    if (s->w) {
        bool cancelled = fl_cancel;
        fl_cancel = false;
        /* written or erased: the target starts again; a look or the console
         * leave it as it was, running */
        if (s->dpidr && (J.job == AOS_FLASHER_JOB_FLASH || J.job == AOS_FLASHER_JOB_ERASE)) {
            reset_run(s);
            if (ok) fl_logf(L_INFO, "%s", _("Objetivo reiniciado"));
        }
        fl_cancel = cancelled;
        aos_io_swd_close(s->w);
    }
    free(s->buf);
    free(s);
    return ok;
}
