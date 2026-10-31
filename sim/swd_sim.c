/*
 * P4OS simulator - the SWD wire (aos_io_swd_*) leads to an emulated Arm
 * target, so the Programador's SWD family (components/aos_flasher/
 * aos_flasher_swd.c) runs its whole path without a board.
 *
 * P4_SIM_SWD picks the chip: nrf52840 (the default), stm32f103, stm32f411
 * (a Black Pill: 512 KB in sectors), stm32g071 (a Cortex-M0+, 128 KB),
 * stm32l476 (1 MB in two banks), samd21 (an Arduino Zero: 256 KB, an 8 KB
 * bootloader under BOOTPROT), samd51 (an Adafruit M4: 512 KB, 16 KB of
 * bootloader), rp2040, rp2350, or none (nobody on the wire). ":protected" after nrf52840 starts it with APPROTECT on (its memory
 * unreachable until the CTRL-AP erases it all), after an STM32 with read
 * protection set (RDP level 1: its flash unreadable by the debugger), after
 * a SAM with its security bit (only the DSU answers);
 * ":locked" after rp2350 locks its ACCESSCTRL against the debugger.
 *
 * What it plays, at the level of packets, not of edges:
 *
 *   the wire       line resets (50 ones), JTAG-to-SWD, SWD-to-dormant and
 *                  the dormant wake-up (the selection alert and the SWD
 *                  activation code). The RP2040 is a multidrop target: its
 *                  three DPs (two cores and the rescue one) come up dormant
 *                  and say nothing until woken; once awake, a line reset
 *                  not followed by a TARGETSEL leaves all of them answering
 *                  at once - the DPIDR, the same for all, reads fine, and
 *                  anything else is a clash (a parity error), as on the
 *                  board on 2026-10-29. TARGETSEL for core 0 picks it.
 *                  The RP2350's one DP is ADIv6 (DPIDR version 3): also
 *                  dormant at first, it answers without a TARGETSEL and
 *                  stays quiet after one that is not its own (0x00040927).
 *                  Its APs go by address: core 0's MEM-AP at 0x2000, its
 *                  registers at 0xD00 of it.
 *   the DP         DPIDR, ABORT, CTRL/STAT with the power-up handshake,
 *                  SELECT, RDBUFF; AP reads are posted, as on silicon: a
 *                  read gives the previous one's value, RDBUFF the last.
 *   the MEM-AP     CSW (8/16/32 bits, auto-increment wrapping at 1 KB), TAR,
 *                  DRW, IDR, with byte lanes as the bus has them.
 *   the core       DHCSR (halt, run, S_REGRDY), DCRSR/DCRDR for the
 *                  registers, DEMCR's reset catch, AIRCR's reset.
 *   the flash      NOR: a write only clears bits, so writing without
 *                  erasing shows. nRF52840: the NVMC (CONFIG, ERASEPAGE,
 *                  ERASEALL) and the CTRL-AP (AP 1: ERASEALL and its status,
 *                  APPROTECTSTATUS). STM32F103: KEYR, CR (PG, PER, MER,
 *                  STRT, LOCK), SR, AR, the halfword program, and the option
 *                  bytes' erase and RDP key (unprotected at the next reset).
 *                  STM32F411: its controller at 0x40023C00 (SER and SNB over
 *                  16/64/128 KB sectors, MER, PSIZE x32 enforced) and OPTCR,
 *                  whose RDP back to 0xAA erases everything. STM32G071 and
 *                  L476: the controller at 0x40022000 (PER, PNB, BKER for the
 *                  L476's second bank, MER1/MER2), a double word programmed
 *                  by its second word, PROGERR over bits not erased, OPTR
 *                  and OPTSTRT, OBL_LAUNCH as a reset. SAMD21/51: the DSU
 *                  (DID, STATUSB.PROT, the chip erase at its external
 *                  address), the NVMCTRL (PARAM, the page buffer filled
 *                  through the flash's addresses, PBC, WP, ER or EB, the
 *                  D21's ADDR in halfwords, MANW's automatic write), and
 *                  BOOTPROT: the bootloader's rows refuse with LOCKE. RP2040: the
 *                  boot ROM's table of functions; when the core is let run
 *                  at debug_trampoline, the function in r7 is done here
 *                  (connect, exit XIP, erase, program from RAM, flush,
 *                  enter XIP) and the core stops at debug_trampoline_end,
 *                  as on the real chip. RP2350: its ROM table has a pointer
 *                  per architecture (RISC-V and Arm Secure here, so taking
 *                  the wrong one shows), and a function runs only when the
 *                  core is let go at a stub in RAM that ends in a bkpt
 *                  (blx r7, bkpt), where it stops afterwards.
 *   XIP            the RP2040's and RP2350's 16 MB window repeats their
 *                  flash chip (2 and 4 MB here), as the chip's address
 *                  wraps: where the Programador finds a flash's size.
 *   RTT            a "SEGGER RTT" control block in RAM with one up and one
 *                  down buffer. While the core runs, a line goes up every
 *                  half second, and what comes down is answered with an
 *                  echo: what tools/ and the Programador's console read.
 *
 * The flash starts as an "old firmware" of pseudo-random bytes in its
 * first 16 KB; P4_SIM_SWD_DUMP=<file> writes it out at every reset and
 * when the port closes, for the tests to compare.
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { C_NONE, C_NRF52840, C_STM32F103, C_RP2040, C_RP2350, C_STM32F411, C_STM32G071, C_STM32L476, C_SAMD21, C_SAMD51 };

typedef struct {
    int chip;
    bool protect;
    /* the wire */
    int ones;
    bool after_reset, dormant, selected, multidrop, deselected;
    uint32_t targetsel_id;      /* a DP that knows TARGETSEL: its own; 0 none */
    int ts_seq;                 /* a TARGETSEL made of raw sequences (a debugger's): 1 request out, 2 ACK let go */
    uint32_t rom_size;
    bool locked;                /* RP2350: ACCESSCTRL locked against the debugger */
    uint64_t hist;
    int alert_bits;
    /* the DP and the AP */
    uint32_t ctrl, select, rdbuff, dpidr, targetsel;
    bool sticky;
    uint32_t csw, tar;
    /* the core */
    bool halted, catch_reset;
    uint32_t dcrdr, regs[24], demcr;
    /* memory */
    uint8_t *flash, *ram, *rom;
    uint32_t flash_base, flash_size, page, ram_size;
    /* flash controllers */
    uint32_t nvmc_config, ctrlap_status;
    int stm_keys, opt_keys;
    uint32_t stm_cr, stm_sr, stm_ar;
    uint32_t optcr;             /* F411's OPTCR, G071/L476's OPTR */
    bool f1_opt_erased, unprotect_at_reset;
    uint32_t dw_addr, dw_lo;    /* G0/L4: a double word's first word, waiting for its second */
    bool dw_open;
    /* SAM: the NVMCTRL's registers and its page buffer, the DSU */
    uint32_t sam_ctrla, sam_ctrlb, sam_addr, sam_status, sam_intflag, sam_dsu;
    uint8_t sam_pb[512];
    uint32_t sam_boot;          /* bytes BOOTPROT keeps */
    bool xip;
    /* RTT */
    uint32_t rtt_cb;
    uint64_t next_line;
    int line_no;
} tgt_t;

static tgt_t T;
static bool s_open;

static bool is_stm(void) { return T.chip == C_STM32F103 || T.chip == C_STM32F411 || T.chip == C_STM32G071 || T.chip == C_STM32L476; }
static bool is_l4(void) { return T.chip == C_STM32G071 || T.chip == C_STM32L476; }
static bool is_sam(void) { return T.chip == C_SAMD21 || T.chip == C_SAMD51; }

#define RAM_BASE   0x20000000u
#define RTT_BUF_UP 1024u
#define RTT_BUF_DN 64u

/* RP2040's boot ROM: function codes and the addresses this ROM gives them */
static const struct { char c[2]; uint16_t at; } RP_FUNCS[] = {
    { "IF", 0x1001 }, { "EX", 0x1011 }, { "RE", 0x1021 }, { "RP", 0x1031 }, { "FC", 0x1041 },
    { "CX", 0x1051 }, { "DT", 0x1061 }, { "DE", 0x1071 },
};

static void dump(void)
{
    const char *p = getenv("P4_SIM_SWD_DUMP");
    if (!p || !T.flash) return;
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(T.flash, 1, T.flash_size, f); fclose(f); }
}

static void rtt_setup(void)
{
    /* the control block, as SEGGER_RTT.c lays it out */
    uint32_t cb = 0x400, up = cb + 0x100, dn = up + RTT_BUF_UP;
    T.rtt_cb = RAM_BASE + cb;
    uint8_t *m = T.ram + cb;
    memcpy(m, "SEGGER RTT\0\0\0\0\0\0", 16);
    uint32_t w[] = { 1, 1,
                     RAM_BASE + cb + 0x80, RAM_BASE + up, RTT_BUF_UP, 0, 0, 0,     /* up 0: name, buf, size, wr, rd, flags */
                     RAM_BASE + cb + 0x80, RAM_BASE + dn, RTT_BUF_DN, 0, 0, 0 };
    memcpy(m + 16, w, sizeof w);
    memcpy(m + 0x80, "Terminal", 9);
    T.next_line = aos_hal_uptime_ms() + 300;
}

static void chip_init(void)
{
    const char *e = getenv("P4_SIM_SWD");
    char name[32] = "nrf52840";
    if (e && *e) snprintf(name, sizeof name, "%s", e);
    char *colon = strchr(name, ':');
    T.protect = colon && !strcmp(colon + 1, "protected");
    T.locked = colon && !strcmp(colon + 1, "locked");
    if (colon) *colon = 0;
    T.chip = !strcmp(name, "stm32f103") ? C_STM32F103 : !strcmp(name, "rp2040") ? C_RP2040 : !strcmp(name, "rp2350") ? C_RP2350
           : !strcmp(name, "stm32f411") ? C_STM32F411 : !strcmp(name, "stm32g071") ? C_STM32G071
           : !strcmp(name, "stm32l476") ? C_STM32L476 : !strcmp(name, "samd21") ? C_SAMD21 : !strcmp(name, "samd51") ? C_SAMD51
           : !strcmp(name, "none") ? C_NONE : C_NRF52840;
    switch (T.chip) {
    case C_NRF52840:
        T.dpidr = 0x2BA01477; T.flash_base = 0; T.flash_size = 1 << 20; T.page = 4096; T.ram_size = 256 << 10;
        T.ctrlap_status = 0;
        break;
    case C_STM32F103:
        T.dpidr = 0x1BA01477; T.flash_base = 0x08000000; T.flash_size = 64 << 10; T.page = 1024; T.ram_size = 20 << 10;
        break;
    case C_STM32F411:
        T.dpidr = 0x2BA01477; T.flash_base = 0x08000000; T.flash_size = 512 << 10; T.page = 16384; T.ram_size = 128 << 10;
        T.optcr = T.protect ? 0x0FFF55ED : 0x0FFFAAED;          /* RDP 0x55 or 0xAA, OPTLOCK */
        break;
    case C_STM32G071:
        T.dpidr = 0x0BC11477; T.flash_base = 0x08000000; T.flash_size = 128 << 10; T.page = 2048; T.ram_size = 36 << 10;
        T.optcr = T.protect ? 0xFFFFFEBB : 0xFFFFFEAA;
        break;
    case C_SAMD21:
        T.dpidr = 0x0BC11477; T.flash_base = 0; T.flash_size = 256 << 10; T.page = 64; T.ram_size = 32 << 10;
        T.sam_boot = 8 << 10;
        T.sam_dsu = T.protect ? 0x010000 : 0;
        break;
    case C_SAMD51:
        T.dpidr = 0x2BA01477; T.flash_base = 0; T.flash_size = 512 << 10; T.page = 512; T.ram_size = 192 << 10;
        T.sam_boot = 16 << 10;
        T.sam_dsu = T.protect ? 0x010000 : 0;
        T.sam_ctrla = 0x0004 | 0x30;            /* WMODE automatic until told otherwise */
        break;
    case C_STM32L476:
        T.dpidr = 0x2BA01477; T.flash_base = 0x08000000; T.flash_size = 1 << 20; T.page = 2048; T.ram_size = 96 << 10;
        T.optcr = T.protect ? 0xFFEFF8BB : 0xFFEFF8AA;          /* DUALBANK (bit 21) set */
        break;
    case C_RP2040:
        T.dpidr = 0x0BC12477; T.flash_base = 0x10000000; T.flash_size = 2 << 20; T.page = 4096; T.ram_size = 264 << 10;
        T.multidrop = true;
        T.targetsel_id = 0x01002927;
        T.dormant = true;
        T.xip = true;
        T.rom_size = 0x4000;
        T.rom = calloc(1, T.rom_size);
        T.rom[0x10] = 'M'; T.rom[0x11] = 'u'; T.rom[0x12] = 1;      /* magic, version 1 */
        /* the function table at 0x7A, two bytes off a word, as a real ROM
         * may have it: read by words it comes out shifted */
        T.rom[0x14] = 0x7A; T.rom[0x15] = 0x00;
        for (size_t i = 0; i < sizeof RP_FUNCS / sizeof RP_FUNCS[0]; i++) {
            uint8_t *t = T.rom + 0x7A + 4 * i;
            t[0] = (uint8_t)RP_FUNCS[i].c[0]; t[1] = (uint8_t)RP_FUNCS[i].c[1];
            t[2] = (uint8_t)RP_FUNCS[i].at; t[3] = (uint8_t)(RP_FUNCS[i].at >> 8);
        }
        break;
    case C_RP2350: {
        T.dpidr = 0x4C013477; T.flash_base = 0x10000000; T.flash_size = 4 << 20; T.page = 4096; T.ram_size = 520 << 10;
        T.targetsel_id = 0x00040927;
        T.dormant = true;
        T.xip = true;
        T.rom_size = 0x8000;
        T.rom = calloc(1, T.rom_size);
        T.rom[0x10] = 'M'; T.rom[0x11] = 'u'; T.rom[0x12] = 2;      /* magic, version 2 */
        /* the A1 table at 0x7E02: code, flags, a pointer per flag. The
         * functions: RISC-V (0x01) and Arm Secure (0x04); a data entry
         * (0x40) and one Arm Secure + Non-secure (0x14) in between */
        T.rom[0x14] = 0x02; T.rom[0x15] = 0x7E;
        uint8_t *t = T.rom + 0x7E02;
        static const struct { char c[2]; uint16_t arm; } F2[] = {
            { "SR", 0x2001 }, { "IF", 0x2011 }, { "EX", 0x2021 }, { "RE", 0x2031 }, { "RP", 0x2041 },
            { "FC", 0x2051 }, { "RA", 0x2061 },
        };
        for (size_t i = 0; i < sizeof F2 / sizeof F2[0]; i++) {
            uint16_t w[] = { (uint16_t)(F2[i].c[0] | F2[i].c[1] << 8), 0x05, (uint16_t)(F2[i].arm + 0x1000), F2[i].arm };
            memcpy(t, w, sizeof w);
            t += sizeof w;
            if (i == 2) {
                uint16_t d[] = { 'G' | 'P' << 8, 0x40, 0x0123, 'X' | 'Y' << 8, 0x14, 0x0F01, 0x0F11 };
                memcpy(t, d, sizeof d);
                t += sizeof d;
            }
        }
        break;
    }
    default: return;
    }
    T.flash = malloc(T.flash_size);
    memset(T.flash, 0xFF, T.flash_size);
    uint32_t x = 0x12345678;
    for (uint32_t i = 0; i < 16384; i++) { x = x * 1103515245u + 12345u; T.flash[i] = (uint8_t)(x >> 16); }
    T.ram = calloc(1, T.ram_size);
    /* the STM32s come up with their flash controller locked */
    T.stm_cr = T.chip == C_STM32F103 ? 0x80 : T.chip == C_STM32F411 ? 0x80000000u : is_l4() ? 0xC0000000u : 0;
    rtt_setup();
}

/* ---- memory ---- */

static uint8_t *ptr(uint32_t a)
{
    if (T.flash && a >= T.flash_base && a - T.flash_base < T.flash_size) {
        if (T.chip == C_RP2040 && !T.xip) return NULL;
        return T.flash + (a - T.flash_base);
    }
    /* the RP's XIP window is 16 MB: the flash chip repeats in it */
    if ((T.chip == C_RP2040 || T.chip == C_RP2350) && T.flash && T.xip && a >= T.flash_base && a - T.flash_base < (16u << 20))
        return T.flash + (a - T.flash_base) % T.flash_size;
    if (is_stm() && a < T.flash_size) return T.flash + a;                 /* the alias at 0 */
    if (T.ram && a >= RAM_BASE && a - RAM_BASE < T.ram_size) return T.ram + (a - RAM_BASE);
    if (T.rom && a < T.rom_size) return T.rom + a;
    return NULL;
}

static bool is_flash(uint32_t a)
{
    return T.flash && ((a >= T.flash_base && a - T.flash_base < T.flash_size) || (is_stm() && a < T.flash_size));
}

static uint32_t flash_off(uint32_t a) { return a >= T.flash_base ? a - T.flash_base : a; }

static void reset_core(void)
{
    T.halted = T.catch_reset;
    T.nvmc_config = 0;
    T.stm_keys = T.opt_keys = 0;
    T.stm_cr = T.chip == C_STM32F103 ? 0x80 : T.chip == C_STM32F411 ? 0x80000000u : 0xC0000000u;   /* LOCK (and OPTLOCK) */
    T.dw_open = false;
    if (T.chip == C_STM32F411) T.optcr |= 1;                    /* OPTLOCK */
    if (T.unprotect_at_reset) { T.protect = false; T.unprotect_at_reset = false; T.f1_opt_erased = false; }
    T.xip = true;
    dump();
}

/* The CoreSight identification a debugger walks (the probe,
 * aos_flasher_dap.c, with pyOCD or OpenOCD behind it): the MEM-AP's BASE
 * points at a ROM table at 0xE00FF000 whose one entry is the SCS at
 * 0xE000E000, each with its peripheral and component IDs. The SCS's part
 * number is the core's (0x008 M0+, 0x000 M3, 0x00C M4). */
static bool coresight_id(uint32_t a, uint32_t *v)
{
    if (a == 0xE00FF000) { *v = 0xFFF0F003; return true; }      /* present, 32-bit, 0xE000E000 */
    uint32_t base = a & ~0xFFFu, off = a & 0xFFF;
    if ((base != 0xE00FF000 && base != 0xE000E000) || off < 0xFD0) return false;
    bool rom = base == 0xE00FF000;
    uint16_t part = rom ? 0x4C4 : T.chip == C_RP2040 || T.chip == C_STM32G071 || T.chip == C_SAMD21 ? 0x008 : T.chip == C_STM32F103 ? 0x000 : 0x00C;
    uint8_t id[12] = { 0x04, 0, 0, 0,                           /* PIDR4..7: JEP106 continuation 4 (Arm) */
                       (uint8_t)part, (uint8_t)(0xB0 | part >> 8), 0x0B, 0,   /* PIDR0..3: Arm (0x43B) */
                       0x0D, rom ? 0x10 : 0xE0, 0x05, 0xB1 };   /* CIDR: a ROM table, or generic IP */
    *v = id[(off - 0xFD0) / 4];
    return true;
}

/* ---- SAM D21/D51: the DSU and the NVMCTRL ---- */

static bool sam_read(uint32_t a, uint32_t *v)
{
    bool d51 = T.chip == C_SAMD51;
    switch (a) {
    case 0x41002000: case 0x41002100: *v = T.sam_dsu; return true;     /* CTRL, STATUSA, STATUSB */
    case 0x41002018: case 0x41002118: *v = d51 ? 0x60060004 : 0x10010300; return true;   /* D51J19A, D21J18A */
    }
    if (T.protect) return false;
    switch (a) {
    case 0x41004000: *v = T.sam_ctrla; return true;
    case 0x41004004: *v = T.sam_ctrlb; return true;
    case 0x41004008: {                                                 /* PARAM: pages, PSZ */
        uint32_t psz = 0;
        while ((8u << psz) < T.page) psz++;
        *v = psz << 16 | T.flash_size / T.page;
        return true;
    }
    case 0x00804000: *v = d51 ? 0xF69A9239 : 0xD8E0C7FA; return true;  /* the user row: BOOTPROT 13 (16 KB), 2 (8 KB) */
    }
    if (d51) {
        if (a == 0x41004010) { *v = T.sam_intflag | (T.sam_status | 13u << 8 | 1) << 16; return true; }   /* STATUS: READY, BOOTPROT */
        if (a == 0x41004014) { *v = T.sam_addr; return true; }
    } else {
        if (a == 0x41004014) { *v = T.sam_intflag | 1; return true; }   /* READY */
        if (a == 0x41004018) { *v = T.sam_status; return true; }
        if (a == 0x4100401C) { *v = T.sam_addr; return true; }
    }
    return false;
}

static void sam_command(uint32_t cmd)
{
    bool d51 = T.chip == C_SAMD51;
    uint32_t addr = d51 ? T.sam_addr : T.sam_addr * 2;
    uint32_t unit = d51 ? T.page * 16 : T.page * 4;
    uint32_t lock_err = d51 ? 0 : 0x08;
    int er = d51 ? 0x01 : 0x02, wp = d51 ? 0x03 : 0x04, pbc = d51 ? 0x15 : 0x44;
    if ((int)cmd == pbc) { memset(T.sam_pb, 0xFF, sizeof T.sam_pb); return; }
    if (addr >= T.flash_size) { if (d51) T.sam_intflag |= 0x02; else T.sam_status |= 0x10; return; }   /* ADDRE, NVME */
    if ((int)cmd == er || (int)cmd == wp) {
        if (addr < T.sam_boot) { if (d51) T.sam_intflag |= 0x08; else T.sam_status |= lock_err; return; }   /* LOCKE */
        if ((int)cmd == er) memset(T.flash + (addr & ~(unit - 1)), 0xFF, unit);
        else for (uint32_t i = 0; i < T.page; i++) T.flash[(addr & ~(T.page - 1)) + i] &= T.sam_pb[i];
        if (d51) T.sam_intflag |= 1;
        return;
    }
    if (d51) T.sam_intflag |= 0x40; else T.sam_status |= 0x10;                                         /* NVME */
}

/* a write to the SAM's registers of 'size' bytes at a (its lanes as the bus has them) */
static bool sam_write(uint32_t a, int size, uint32_t lanes)
{
    bool d51 = T.chip == C_SAMD51;
    uint32_t w = a & ~3u;
    uint32_t v = lanes >> 8 * (a & 3);
    if (w == 0x41002100 || w == 0x41002000) {
        if ((a & 3) == 0 && (v & 0x10)) {                               /* CTRL.CE: everything, and the protection */
            memset(T.flash, 0xFF, T.flash_size);
            T.protect = false;
            T.sam_dsu = 0x0100;                                         /* STATUSA.DONE */
        } else if (size == 4 && (lanes & 0x0100)) {
            T.sam_dsu &= ~0x0100u;
        }
        return true;
    }
    if (T.protect) return a >= 0x41000000 && a < 0x42000000;            /* quietly ignored */
    if (d51) {
        switch (w) {
        case 0x41004000: T.sam_ctrla = v & 0xFFFF; return true;
        case 0x41004004:
            if ((v >> 8 & 0xFF) == 0xA5) sam_command(v & 0x7F);
            return true;
        case 0x41004010: if ((a & 3) == 0) T.sam_intflag &= ~(v & 0xFF); return true;
        case 0x41004014: T.sam_addr = lanes; return true;
        }
    } else {
        switch (w) {
        case 0x41004000:
            if ((a & 3) == 0 && (v >> 8 & 0xFF) == 0xA5) sam_command(v & 0x7F);
            return true;
        case 0x41004004: T.sam_ctrlb = lanes; return true;
        case 0x41004018: if ((a & 3) == 0) T.sam_status &= ~(v & 0x1C); return true;
        case 0x4100401C: T.sam_addr = lanes; return true;
        }
    }
    return false;
}

/* A write to the flash's addresses fills the page buffer; the D21 with
 * MANW off writes the page by itself at its last word */
static void sam_buffer(uint32_t a, int size, uint32_t lanes)
{
    uint32_t o = a & (T.page - 1);
    for (int i = 0; i < size; i++) T.sam_pb[o + i] = (uint8_t)(lanes >> 8 * ((a & 3) + i));
    if (T.chip == C_SAMD21 && !(T.sam_ctrlb & 0x80) && o + size == T.page) {
        T.sam_addr = (a & ~(T.page - 1)) / 2;
        sam_command(0x04);
    }
}

/* registers that are not memory; false: not one of them */
static bool sam_read(uint32_t a, uint32_t *v);

static bool reg_read(uint32_t a, uint32_t *v)
{
    if (is_sam() && sam_read(a, v)) return true;
    if (coresight_id(a, v)) return true;
    switch (a) {
    case 0xE000ED00: *v = T.chip == C_RP2040 || T.chip == C_STM32G071 || T.chip == C_SAMD21 ? 0x410CC601 : T.chip == C_RP2350 ? 0x411FD210 : T.chip == C_STM32F103 ? 0x411FC231 : 0x410FC241; return true;
    case 0xE000EDF0: *v = 0x00010001 | (T.halted ? 0x00020002 : 0); return true;     /* S_REGRDY, C_DEBUGEN, S_HALT */
    case 0xE000EDF8: *v = T.dcrdr; return true;
    case 0xE000EDFC: *v = T.demcr; return true;
    }
    if (T.chip == C_NRF52840) {
        switch (a) {
        case 0x10000010: *v = 4096; return true;
        case 0x10000014: *v = 256; return true;
        case 0x10000060: *v = 0x8E5C21A7; return true;
        case 0x10000064: *v = 0x4D2F0B13; return true;
        case 0x10000100: *v = 0x52840; return true;
        case 0x10000104: *v = 0x41414430; return true;  /* "AAD0" */
        case 0x4001E400: *v = 1; return true;            /* READY */
        case 0x4001E504: *v = T.nvmc_config; return true;
        }
    } else if (T.chip == C_STM32F103) {
        switch (a) {
        case 0xE0042000: *v = 0x20036410; return true;
        case 0x1FFFF7E0: *v = T.flash_size >> 10; return true;
        case 0x1FFFF7E8: *v = 0x0670FF55; return true;
        case 0x1FFFF7EC: *v = 0x50578348; return true;
        case 0x1FFFF7F0: *v = 0x87112434; return true;
        case 0x4002200C: *v = T.stm_sr; return true;
        case 0x40022010: *v = T.stm_cr; return true;
        case 0x4002201C: *v = T.protect || T.f1_opt_erased ? 2 : 0; return true;   /* erased option bytes read as protected */
        }
    } else if (T.chip == C_STM32F411) {
        switch (a) {
        case 0xE0042000: *v = 0x10006431; return true;
        case 0x1FFF7A20: *v = (T.flash_size >> 10) << 16; return true;    /* the size is the upper halfword */
        case 0x1FFF7A10: *v = 0x00470036; return true;
        case 0x1FFF7A14: *v = 0x30385111; return true;
        case 0x1FFF7A18: *v = 0x32333436; return true;
        case 0x40023C0C: *v = T.stm_sr; return true;
        case 0x40023C10: *v = T.stm_cr; return true;
        case 0x40023C14: *v = T.optcr; return true;
        }
    } else if (is_l4()) {
        switch (a) {
        case 0x40015800: if (T.chip == C_STM32G071) { *v = 0x10006460; return true; } break;
        case 0xE0042000: if (T.chip == C_STM32L476) { *v = 0x10076415; return true; } break;
        case 0x1FFF75E0: *v = 0xFFFF0000u | T.flash_size >> 10; return true;
        case 0x1FFF7590: *v = 0x002B0042; return true;
        case 0x1FFF7594: *v = 0x4E4B5013; return true;
        case 0x1FFF7598: *v = 0x20393833; return true;
        case 0x40022010: *v = T.stm_sr; return true;
        case 0x40022014: *v = T.stm_cr; return true;
        case 0x40022020: *v = T.optcr; return true;
        }
    } else if (T.chip == C_RP2040) {
        if (a == 0x40000000) { *v = 0x10002927; return true; }
    } else if (T.chip == C_RP2350) {
        if (a == 0x40000000) { *v = 0x20004927; return true; }
        if (a == 0x40060000) { *v = T.locked ? 0x08 : 0; return true; }
    }
    return false;
}

static bool reg_write(uint32_t a, uint32_t v)
{
    switch (a) {
    case 0xE000EDF0:
        if ((v >> 16) != 0xA05F) return true;
        /* one step (C_STEP, not C_HALT): a 16-bit instruction, halted again */
        if ((v & 4) && !(v & 2) && T.halted) { T.regs[15] += 2; return true; }
        if (v & 2) T.halted = true;
        else if (T.halted) {
            T.halted = false;
            /* RP2040: a ROM function through the trampoline, then stop */
            if (T.chip == C_RP2040 && (T.regs[15] | 1) == 0x1061) {
                uint32_t fn = T.regs[7] | 1, r0 = T.regs[0], r1 = T.regs[1], r2 = T.regs[2];
                if (fn == 0x1011) T.xip = false;
                else if (fn == 0x1051) T.xip = true;
                else if (fn == 0x1021 && !T.xip) {
                    uint32_t from = r0 & ~4095u, to = (r0 + r1 + 4095u) & ~4095u;
                    if (to <= T.flash_size) memset(T.flash + from, 0xFF, to - from);
                }
                else if (fn == 0x1031 && !T.xip && r0 + r2 <= T.flash_size && r1 >= RAM_BASE && r1 - RAM_BASE + r2 <= T.ram_size)
                    for (uint32_t i = 0; i < r2; i++) T.flash[r0 + i] &= T.ram[r1 - RAM_BASE + i];
                T.regs[15] = 0x1070;
                T.halted = true;
            }
            /* RP2350: our stub in RAM (blx r7; bkpt) */
            uint8_t *st = ptr(T.regs[15]);
            if (T.chip == C_RP2350 && st && T.regs[15] >= RAM_BASE && st[10] == 0xB8 && st[11] == 0x47 && st[12] == 0x00 && st[13] == 0xBE) {
                uint32_t fn = T.regs[7] | 1, r0 = T.regs[0], r1 = T.regs[1], r2 = T.regs[2];
                if (fn >= 0x3000) return true;          /* a RISC-V pointer: the core runs off, never stops */
                if (fn == 0x2021) T.xip = false;        /* EX: direct mode until the end */
                else if (fn == 0x2061) T.xip = true;    /* RA */
                else if (fn == 0x2031 && !T.xip) {
                    uint32_t from = r0 & ~4095u, to = (r0 + r1 + 4095u) & ~4095u;
                    if (to <= T.flash_size) memset(T.flash + from, 0xFF, to - from);
                } else if (fn == 0x2041 && !T.xip && r0 + r2 <= T.flash_size && r1 >= RAM_BASE && r1 - RAM_BASE + r2 <= T.ram_size) {
                    for (uint32_t i = 0; i < r2; i++) T.flash[r0 + i] &= T.ram[r1 - RAM_BASE + i];
                }
                T.regs[15] += 12;
                T.halted = true;
            }
        }
        return true;
    case 0xE000EDF4: {
        int r = v & 0x7F;
        if (r < 24) { if (v & 0x10000) T.regs[r] = T.dcrdr; else T.dcrdr = T.regs[r]; }
        return true;
    }
    case 0xE000EDF8: T.dcrdr = v; return true;
    case 0xE000EDFC: T.demcr = v; T.catch_reset = v & 1; return true;
    case 0xE000ED0C:
        if ((v >> 16) == 0x05FA && (v & 4)) reset_core();
        return true;
    }
    if (T.chip == C_NRF52840) {
        switch (a) {
        case 0x4001E504: T.nvmc_config = v & 3; return true;
        case 0x4001E508:
            if (T.nvmc_config == 2 && v < T.flash_size) memset(T.flash + (v & ~4095u), 0xFF, 4096);
            return true;
        case 0x4001E50C:
            if (T.nvmc_config == 2 && v == 1) memset(T.flash, 0xFF, T.flash_size);
            return true;
        }
    } else if (T.chip == C_STM32F103) {
        switch (a) {
        case 0x40022004:
            if ((T.stm_keys == 0 && v == 0x45670123) || (T.stm_keys == 1 && v == 0xCDEF89AB)) T.stm_keys++;
            else T.stm_keys = 0;
            if (T.stm_keys == 2) T.stm_cr &= ~0x80u;
            return true;
        case 0x4002200C: T.stm_sr &= ~(v & 0x34); return true;     /* write 1 to clear EOP, WRPRTERR, PGERR */
        case 0x40022014: T.stm_ar = v; return true;
        case 0x40022010:
            if (T.stm_cr & 0x80) return true;                       /* locked */
            T.stm_cr = v;
            if ((v & 0x40) && (v & 2)) {                             /* PER + STRT */
                uint32_t o = flash_off(T.stm_ar) & ~(T.page - 1);
                if (o < T.flash_size) memset(T.flash + o, 0xFF, T.page);
                T.stm_sr |= 0x20;
                T.stm_cr &= ~0x40u;
            } else if ((v & 0x40) && (v & 4)) {                      /* MER + STRT */
                memset(T.flash, 0xFF, T.flash_size);
                T.stm_sr |= 0x20;
                T.stm_cr &= ~0x40u;
            } else if ((v & 0x40) && (v & 0x20) && (v & 0x200)) {    /* OPTER + STRT, with OPTWRE */
                T.f1_opt_erased = true;
                T.stm_sr |= 0x20;
                T.stm_cr &= ~0x40u;
            }
            if (T.opt_keys < 2) T.stm_cr &= ~0x200u;                 /* OPTWRE only through its keys */
            return true;
        case 0x40022008:                                            /* OPTKEYR */
            if (T.stm_cr & 0x80) return true;
            if ((T.opt_keys == 0 && v == 0x45670123) || (T.opt_keys == 1 && v == 0xCDEF89AB)) T.opt_keys++;
            else T.opt_keys = 0;
            if (T.opt_keys == 2) T.stm_cr |= 0x200;
            return true;
        }
    } else if (T.chip == C_STM32F411) {
        switch (a) {
        case 0x40023C04:                                            /* KEYR */
            if ((T.stm_keys == 0 && v == 0x45670123) || (T.stm_keys == 1 && v == 0xCDEF89AB)) T.stm_keys++;
            else T.stm_keys = 0;
            if (T.stm_keys == 2) T.stm_cr &= ~0x80000000u;
            return true;
        case 0x40023C08:                                            /* OPTKEYR */
            if ((T.opt_keys == 0 && v == 0x08192A3B) || (T.opt_keys == 1 && v == 0x4C5D6E7F)) T.opt_keys++;
            else T.opt_keys = 0;
            if (T.opt_keys == 2) T.optcr &= ~1u;
            return true;
        case 0x40023C0C: T.stm_sr &= ~(v & 0x1F3); return true;
        case 0x40023C10: {
            if (T.stm_cr & 0x80000000u) return true;
            T.stm_cr = v;
            if (!(v & 0x10000)) return true;                        /* STRT */
            if (v & 2) {                                            /* SER: sector SNB */
                uint32_t n = v >> 3 & 0x1F, off, len;
                if (n < 4) { off = n << 14; len = 0x4000; }
                else if (n == 4) { off = 0x10000; len = 0x10000; }
                else { off = 0x20000 + ((n - 5) << 17); len = 0x20000; }
                if (off + len <= T.flash_size) memset(T.flash + off, 0xFF, len);
                else T.stm_sr |= 0x80;                              /* PGSERR: no such sector */
            } else if (v & 4) {
                memset(T.flash, 0xFF, T.flash_size);
            }
            T.stm_sr |= 1;
            T.stm_cr &= ~0x10000u;
            return true;
        }
        case 0x40023C14:
            if (T.optcr & 1) return true;                           /* OPTLOCK */
            if ((v & 2) && T.protect && (v >> 8 & 0xFF) == 0xAA) {  /* OPTSTRT, RDP back to level 0 */
                memset(T.flash, 0xFF, T.flash_size);
                T.protect = false;
            }
            T.optcr = v & ~2u;
            return true;
        }
    } else if (is_l4()) {
        switch (a) {
        case 0x40022008:                                            /* KEYR */
            if ((T.stm_keys == 0 && v == 0x45670123) || (T.stm_keys == 1 && v == 0xCDEF89AB)) T.stm_keys++;
            else T.stm_keys = 0;
            if (T.stm_keys == 2) T.stm_cr &= ~0x80000000u;
            return true;
        case 0x4002200C:                                            /* OPTKEYR, the flash unlocked first */
            if (T.stm_cr & 0x80000000u) return true;
            if ((T.opt_keys == 0 && v == 0x08192A3B) || (T.opt_keys == 1 && v == 0x4C5D6E7F)) T.opt_keys++;
            else T.opt_keys = 0;
            if (T.opt_keys == 2) T.stm_cr &= ~0x40000000u;
            return true;
        case 0x40022010: T.stm_sr &= ~(v & 0xC3FB); return true;
        case 0x40022014: {
            if (T.stm_cr & 0x80000000u) return true;
            T.stm_cr = v | (T.stm_cr & 0x40000000u);                 /* OPTLOCK only goes with its keys */
            if (v & 0x8000000) { reset_core(); return true; }        /* OBL_LAUNCH: the option bytes load, the chip restarts */
            if ((v & 0x20000) && !(T.stm_cr & 0x40000000u)) {        /* OPTSTRT */
                if (T.protect && (T.optcr & 0xFF) == 0xAA) {
                    memset(T.flash, 0xFF, T.flash_size);
                    T.unprotect_at_reset = true;
                }
                T.stm_cr &= ~0x20000u;
                return true;
            }
            if (!(v & 0x10000)) return true;                        /* STRT */
            uint32_t bank = T.chip == C_STM32L476 ? T.flash_size / 2 : T.flash_size;
            if (v & 2) {                                            /* PER: page PNB of the bank BKER says */
                uint32_t off = ((v >> 3) & 0xFF) * T.page + ((v & 0x800) && T.chip == C_STM32L476 ? bank : 0);
                if ((v >> 3 & 0xFF) * T.page < bank && off < T.flash_size) memset(T.flash + off, 0xFF, T.page);
                else T.stm_sr |= 0x80;                              /* PGSERR */
            } else {
                if (v & 4) memset(T.flash, 0xFF, bank);             /* MER1 */
                if ((v & 0x8000) && T.chip == C_STM32L476) memset(T.flash + bank, 0xFF, bank);  /* MER2 */
            }
            T.stm_sr |= 1;
            T.stm_cr &= ~0x10000u;
            return true;
        }
        case 0x40022020:
            if (!(T.stm_cr & 0x40000000u)) T.optcr = v;              /* OPTR, while OPTLOCK is off */
            return true;
        }
    }
    return false;
}

/* A bus access of 'size' bytes (1, 2, 4) at a; data in its byte lanes. */
static bool mem_access(uint32_t a, int size, bool wr, uint32_t *lanes)
{
    if (T.chip == C_NRF52840 && T.protect) return false;
    /* an STM32 at RDP level 1 keeps its flash from the debugger */
    if (is_stm() && T.protect && !wr && is_flash(a)) return false;
    /* a protected SAM: the DSU, nothing else */
    if (is_sam()) {
        if (wr && sam_write(a, size, *lanes)) return true;
        if (T.protect && (a & ~0xFFFu) != 0x41002000) return false;
        if (wr && is_flash(a)) { sam_buffer(a, size, *lanes); return true; }
    }
    /* the F1's RDP option byte: its key unprotects, and erases all */
    if (T.chip == C_STM32F103 && wr && (a & ~1u) == 0x1FFFF800 && size == 2) {
        if ((T.stm_cr & 0x210) == 0x210 && (*lanes & 0xFFFF) == 0x5AA5) {
            memset(T.flash, 0xFF, T.flash_size);
            T.unprotect_at_reset = true;
            T.stm_sr |= 0x20;
        }
        return true;
    }
    uint32_t aligned = a & ~3u, v;
    if (size == 4 && !wr && reg_read(aligned, &v)) { *lanes = v; return true; }
    if (size == 4 && wr && reg_write(aligned, *lanes)) return true;
    if (!wr && reg_read(aligned, &v)) { *lanes = v; return true; }
    /* the bus has no unaligned accesses: the low bits of TAR are dropped */
    a &= ~(uint32_t)(size - 1);
    uint8_t *p = ptr(a);
    if (!p) {
        if (!wr) *lanes = 0;
        return a >= 0x40000000;     /* other peripherals: quiet */
    }
    int sh = (int)(a & 3) * 8;
    if (!wr) {
        uint32_t w = 0;
        for (int i = 0; i < size; i++) w |= (uint32_t)p[i] << (sh + 8 * i);
        *lanes = w;
        return true;
    }
    if (is_flash(a) && T.chip == C_STM32F411) {
        if (!(T.stm_cr & 1) || (T.stm_cr & 0x80000000u)) { T.stm_sr |= 0x80; return true; }     /* PGSERR */
        if (size != 4 || (T.stm_cr >> 8 & 3) != 2) { T.stm_sr |= 0x40; return true; }        /* PGPERR: not PSIZE x32 */
        for (int i = 0; i < size; i++) p[i] &= (uint8_t)(*lanes >> (sh + 8 * i));
        T.stm_sr |= 1;
        return true;
    }
    if (is_flash(a) && is_l4()) {
        if (!(T.stm_cr & 1) || (T.stm_cr & 0x80000000u)) { T.stm_sr |= 0x80; return true; }     /* PGSERR */
        if (size != 4) { T.stm_sr |= 0x40; return true; }                                     /* SIZERR */
        if (!(a & 7)) {                         /* the first word: kept until its second */
            T.dw_addr = a;
            T.dw_lo = *lanes;
            T.dw_open = true;
            return true;
        }
        if (!T.dw_open || T.dw_addr != a - 4) { T.stm_sr |= 0x80; T.dw_open = false; return true; }   /* PGSERR */
        T.dw_open = false;
        uint8_t *q = p - 4;
        for (int i = 0; i < 8; i++) if (q[i] != 0xFF) { T.stm_sr |= 0x08; return true; }      /* PROGERR: not erased */
        for (int i = 0; i < 4; i++) { q[i] = (uint8_t)(T.dw_lo >> 8 * i); p[i] = (uint8_t)(*lanes >> 8 * i); }
        T.stm_sr |= 1;
        return true;
    }
    if (is_flash(a)) {
        bool ok = (T.chip == C_NRF52840 && T.nvmc_config == 1) ||
                  (T.chip == C_STM32F103 && (T.stm_cr & 1) && !(T.stm_cr & 0x80) && size == 2);
        if (!ok) return T.chip != C_STM32F103 ? true : (T.stm_sr |= 0x10, true);   /* ignored; F1: WRPRTERR */
        if (T.chip == C_STM32F103 && (p[0] != 0xFF || p[1] != 0xFF)) { T.stm_sr |= 0x04; return true; }  /* PGERR */
        for (int i = 0; i < size; i++) p[i] &= (uint8_t)(*lanes >> (sh + 8 * i));
        if (T.chip == C_STM32F103) T.stm_sr |= 0x20;
        return true;
    }
    for (int i = 0; i < size; i++) p[i] = (uint8_t)(*lanes >> (sh + 8 * i));
    return true;
}

/* ---- the running firmware's RTT ---- */

static uint32_t rd32(uint32_t a) { uint8_t *p = ptr(a); return p ? (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24 : 0; }
static void wr32(uint32_t a, uint32_t v) { uint8_t *p = ptr(a); if (p) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); } }

static void rtt_put(const char *s)
{
    uint32_t up = T.rtt_cb + 24, buf = rd32(up + 4), size = rd32(up + 8), wr = rd32(up + 12), rd = rd32(up + 16);
    for (; *s; s++) {
        uint32_t next = (wr + 1) % size;
        if (next == rd) break;                  /* full: the rest is dropped, as in NO_BLOCK_SKIP */
        uint8_t *p = ptr(buf + wr);
        if (p) *p = (uint8_t)*s;
        wr = next;
    }
    wr32(up + 12, wr);
}

static void tick(void)
{
    if (!T.rtt_cb || T.halted || (T.chip == C_NRF52840 && T.protect)) return;
    uint32_t dn = T.rtt_cb + 24 + 24, buf = rd32(dn + 4), size = rd32(dn + 8), wr = rd32(dn + 12), rd = rd32(dn + 16);
    if (wr != rd && size) {
        char line[80] = "eco: ";
        size_t n = 5;
        while (rd != wr && n + 2 < sizeof line) {
            uint8_t *p = ptr(buf + rd);
            line[n++] = p ? (char)*p : '?';
            rd = (rd + 1) % size;
        }
        wr32(dn + 16, rd);
        if (line[n - 1] != '\n') line[n++] = '\n';
        line[n] = 0;
        rtt_put(line);
    }
    uint64_t now = aos_hal_uptime_ms();
    if (now >= T.next_line) {
        T.next_line = now + 500;
        char line[64];
        snprintf(line, sizeof line, "hola desde el objetivo, vuelta %d\n", ++T.line_no);
        rtt_put(line);
    }
}

/* ---- the wire ---- */

bool aos_io_be_swd_open(aos_io_swd_t *s)
{
    if (!T.flash) chip_init();
    s_open = true;
    s->be = &T;
    return true;
}

static void line_bit(int b)
{
    T.hist = T.hist >> 1 | (uint64_t)b << 63;
    if (!T.dormant && (T.hist >> 48) == 0xE3BC) { T.dormant = true; T.ones = 0; return; }    /* SWD-to-dormant */
    if (T.dormant) {
        if (T.alert_bits) {
            if (++T.alert_bits == 13) {
                /* four zeros, then the activation code 0x1A for SWD */
                if ((T.hist >> 52) == 0x1A0) T.dormant = false;
                T.alert_bits = 0;
            }
        } else if (T.hist == 0x19BC0EA2E3DDAFE9ull) {
            T.alert_bits = 1;
        }
        return;
    }
    if (b) {
        if (++T.ones >= 50) { T.after_reset = true; T.deselected = false; if (T.multidrop) T.selected = false; }
        return;
    }
    T.ones = 0;
}

/* A debugger behind the probe writes TARGETSEL as raw sequences (CMSIS-DAP's
 * SWD_Sequence: the request out, five clocks let go, the 32 bits and their
 * parity out), since the DP gives it no ACK: told apart here by their
 * shape, right after a line reset. */
bool aos_io_be_swd_seq(aos_io_swd_t *s, const uint8_t *bits, int n)
{
    (void)s;
    if (n == 8 && bits[0] == 0x99 && T.after_reset && T.targetsel_id) {
        T.ts_seq = 1;
    } else if (n == 33 && T.ts_seq == 2) {
        uint32_t v = (uint32_t)bits[0] | bits[1] << 8 | bits[2] << 16 | (uint32_t)bits[3] << 24;
        T.selected = v == T.targetsel_id;
        T.deselected = !T.selected;
        T.after_reset = false;
        T.ts_seq = 0;
        return true;
    } else {
        T.ts_seq = 0;
    }
    for (int i = 0; i < n; i++) line_bit(bits[i >> 3] >> (i & 7) & 1);
    return true;
}

bool aos_io_be_swd_seq_in(aos_io_swd_t *s, uint8_t *bits, int n)
{
    (void)s;
    /* nobody drives the line: the pull-up reads ones */
    for (int i = 0; i < n; i++) bits[i >> 3] |= (uint8_t)(1u << (i & 7));
    T.ts_seq = T.ts_seq == 1 && n == 5 ? 2 : 0;
    return true;
}

bool aos_io_be_swd_clock(aos_io_swd_t *s)
{
    (void)s;
    return true;
}

static uint32_t ap_read(uint32_t reg, int *ack)
{
    int apsel = (int)(T.select >> 24);
    uint32_t v = 0;
    if (apsel == 1 && T.chip == C_NRF52840) {
        switch (reg) {
        case 0x08: v = T.ctrlap_status; T.ctrlap_status = 0; break;
        case 0x0C: v = T.protect ? 0 : 1; break;
        case 0xFC: v = 0x02880000; break;
        }
        return v;
    }
    if (apsel != 0) return 0;
    switch (reg) {
    case 0x00: return T.csw | (T.protect && T.chip == C_NRF52840 ? 0 : 0x40);   /* DeviceEn */
    case 0x04: return T.tar;
    case 0xFC: return T.chip == C_RP2040 ? 0x04770031 : T.chip == C_RP2350 ? 0x34770008 : T.chip == C_NRF52840 ? 0x24770011 : 0x14770011;
    case 0xF8: return 0xE00FF003;
    case 0x0C: {
        int size = 1 << (T.csw & 3);
        uint32_t lanes;
        if (!mem_access(T.tar, size, false, &lanes)) { *ack = 4; T.sticky = true; return 0; }
        if ((T.csw >> 4 & 3) == 1) T.tar = (T.tar & ~0x3FFu) | ((T.tar + (uint32_t)size) & 0x3FF);
        return lanes;
    }
    }
    return 0;
}

static void ap_write(uint32_t reg, uint32_t v, int *ack)
{
    int apsel = (int)(T.select >> 24);
    if (apsel == 1 && T.chip == C_NRF52840) {
        if (reg == 0x04 && v == 1) {
            memset(T.flash, 0xFF, T.flash_size);
            T.protect = false;
            T.ctrlap_status = 1;            /* busy, for one read */
        }
        return;
    }
    if (apsel != 0) return;
    switch (reg) {
    case 0x00: T.csw = v & ~0x40u; break;
    case 0x04: T.tar = v; break;
    case 0x0C: {
        int size = 1 << (T.csw & 3);
        if (!mem_access(T.tar, size, true, &v)) { *ack = 4; T.sticky = true; return; }
        if ((T.csw >> 4 & 3) == 1) T.tar = (T.tar & ~0x3FFu) | ((T.tar + (uint32_t)size) & 0x3FF);
        break;
    }
    }
}

int aos_io_be_swd_xfer(aos_io_swd_t *s, uint8_t req, uint32_t *data, bool no_ack)
{
    (void)s;
    if (!T.chip) return 7;
    tick();
    bool ap = req & 2, rd = req & 4;
    uint32_t a = (uint32_t)(req >> 1) & 0x0C;
    int par = (req >> 1 & 1) ^ (req >> 2 & 1) ^ (req >> 3 & 1) ^ (req >> 4 & 1);
    bool first = T.after_reset;
    T.after_reset = false;
    T.ones = 0;
    if (!(req & 1) || (req & 0x40) || !(req & 0x80) || par != (req >> 5 & 1) || T.dormant) return 7;
    if (!ap && !rd && a == 0x0C && first && T.targetsel_id) {   /* TARGETSEL */
        T.selected = *data == T.targetsel_id;
        T.deselected = !T.selected;
        return no_ack ? 1 : 7;
    }
    if (T.deselected) return 7;
    if (T.multidrop && !T.selected) {
        /* every DP at once: the DPIDR agrees, nothing else does */
        if (!ap && rd && a == 0x0) { *data = T.dpidr; return 1; }
        return rd ? -1 : 1;
    }
    if (T.sticky && ap) return 4;
    int ack = 1;
    if (!ap) {
        if (rd) {
            switch (a) {
            case 0x0: *data = T.dpidr; break;
            case 0x4: *data = T.ctrl | (T.ctrl & 0x50000000u) << 1 | (T.sticky ? 0x20 : 0); break;
            case 0x8: *data = T.rdbuff; break;
            case 0xC: *data = T.rdbuff; break;
            }
        } else {
            switch (a) {
            case 0x0: if (*data & 0x04) T.sticky = false; break;
            case 0x4: T.ctrl = *data & 0x50000000u; break;
            case 0x8: T.select = *data; break;
            }
        }
        return ack;
    }
    uint32_t reg = (T.select & 0xF0) | a;
    if (T.chip == C_RP2350 && (T.select & ~0xFFu) != 0x2D00) {
        /* ADIv6: only core 0's MEM-AP (0x2000, its registers at 0xD00) is played */
        if (rd) { *data = T.rdbuff; T.rdbuff = 0; }
        return 1;
    }
    if (rd) {
        uint32_t v = ap_read(reg, &ack);
        *data = T.rdbuff;           /* posted: the last read's value */
        T.rdbuff = v;
    } else {
        ap_write(reg, *data, &ack);
    }
    return ack;
}

bool aos_io_be_swd_nrst(aos_io_swd_t *s, bool assert)
{
    (void)s;
    if (!assert && T.chip) reset_core();
    return true;
}

void aos_io_be_swd_close(aos_io_swd_t *s)
{
    (void)s;
    s_open = false;
    dump();
    T.halted = false;           /* let go: the target runs */
}
