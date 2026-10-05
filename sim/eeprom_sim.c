/*
 * P4OS simulator - serial EEPROMs on the header, so an app that reads and
 * writes them (and the Bus app) has real chips to talk to.
 *
 * Three families, each with its chip's own protocol and its timing:
 *
 *  I2C, 24xx (Microchip 24LC/24AA, Atmel AT24C, ST M24C...). P4_SIM_EEPROM:
 *    unset           24lc256@0x50
 *    "0" / "off"     none
 *    a list          "24lc02@0x50,24lc512@0x54": any of 24lc01, 24lc02,
 *                    24lc04, 24lc08, 24lc16 (one address byte, the high
 *                    address bits in the device address: a 24LC16 answers
 *                    at 0x50..0x57), 24lc32, 24lc64, 24lc128, 24lc256,
 *                    24lc512 (two address bytes), 24lc1025 (two, and the
 *                    upper 64 KB at device address +4).
 *    Page writes wrap inside their page, as the chips do; after a write the
 *    chip is busy 5 ms and does not acknowledge its address (what "ACK
 *    polling" waits on). A read without an address continues from where
 *    the last one stopped; a read past the end wraps to 0.
 *
 *  SPI, 25xx (25LC/25AA, AT25, M95). Chips of io_sim.c's SPI map
 *    (P4_SIM_SPI_A), by name: 25lc010 25lc020 25lc040 (one address byte,
 *    the 9th address bit of the 040 in the instruction), 25lc080 25lc160
 *    25lc320 25lc640 25lc128 25lc256 25lc512 (two), 25lc1024 (three, plus
 *    0xAB for its electronic signature 0x29). 0x06/0x04 write enable and
 *    disable, 0x05/0x01 status (WIP, WEL, BP1 BP0: a quarter, half or all
 *    of the array write-protected), 0x03 read, 0x02 page write. WIP stays
 *    up 5 ms after a write. The default map has a 25lc640 on GPIO46.
 *
 *  Microwire, 93xx (93C46/56/66/76/86, 93LC...), bit by bit on GPIOs the
 *    app drives itself. P4_SIM_93C = "93c66:cs=28,sk=32,di=34,do=46,org=16"
 *    (org=8 for byte organization); unset, there is none, so those pins
 *    stay plain GPIOs. Start bit, two opcode bits, the address: READ (a
 *    dummy 0 then the data, MSB first, continuing to the next word while
 *    CS stays up), EWEN/EWDS, WRITE, ERASE, ERAL, WRAL; after a write, DO
 *    reads 0 while busy (2 ms) and 1 when ready once CS rises again.
 *
 * Every chip starts with a line of text and then a counting pattern, so a
 * read shows something. Contents live as long as the simulator runs.
 */
#include "aos_hal.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define BUSY_MS 5

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;

static void fill(uint8_t *m, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++) m[i] = (uint8_t)(i * 7 + 1);
    char hdr[64];
    int k = snprintf(hdr, sizeof hdr, "P4OS sim %s, %u bytes\n", name, (unsigned)n);
    if (k > 0) memcpy(m, hdr, (size_t)k < n ? (size_t)k : n);
}

/* ======================= I2C: 24xx ======================= */

typedef struct { const char *name; uint32_t size; uint8_t abytes; uint16_t page; } ee24_type_t;
static const ee24_type_t T24[] = {
    { "24lc01", 128, 1, 8 },      { "24lc02", 256, 1, 8 },      { "24lc04", 512, 1, 16 },
    { "24lc08", 1024, 1, 16 },    { "24lc16", 2048, 1, 16 },    { "24lc32", 4096, 2, 32 },
    { "24lc64", 8192, 2, 32 },    { "24lc128", 16384, 2, 64 },  { "24lc256", 32768, 2, 64 },
    { "24lc512", 65536, 2, 128 }, { "24lc1025", 131072, 2, 128 },
};

typedef struct {
    const ee24_type_t *t;
    uint8_t base;
    uint8_t *mem;
    uint32_t ptr;                   /* the internal address counter */
    uint64_t busy_until;
} ee24_t;

static ee24_t s_24[8];
static int s_n24 = -1;

static void ee24_init(void)
{
    if (s_n24 >= 0) return;
    s_n24 = 0;
    const char *e = getenv("P4_SIM_EEPROM");
    if (!e) e = "24lc256@0x50";
    if (!strcmp(e, "0") || !strcasecmp(e, "off")) return;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *tok = strtok(buf, ", "); tok && s_n24 < 8; tok = strtok(NULL, ", ")) {
        char *at = strchr(tok, '@');
        if (at) *at = 0;
        const ee24_type_t *t = NULL;
        for (size_t i = 0; i < sizeof T24 / sizeof T24[0]; i++)
            if (!strcasecmp(tok, T24[i].name)) t = &T24[i];
        if (!t) { aos_hal_log("sim", "EEPROM: no chip called \"%s\"", tok); continue; }
        ee24_t *c = &s_24[s_n24++];
        c->t = t;
        c->base = at ? (uint8_t)strtol(at + 1, NULL, 0) : 0x50;
        c->mem = malloc(t->size);
        fill(c->mem, t->size, t->name);
    }
    for (int i = 0; i < s_n24; i++)
        aos_hal_log("sim", "emulated EEPROM %s at 0x%02X", s_24[i].t->name, s_24[i].base);
}

/* The chip at this device address, and the address bits it carries. */
static ee24_t *ee24_at(uint8_t addr, uint32_t *hi)
{
    ee24_init();
    for (int i = 0; i < s_n24; i++) {
        ee24_t *c = &s_24[i];
        if (c->t->abytes == 1) {
            uint32_t blocks = c->t->size > 256 ? c->t->size / 256 : 1;
            if (addr >= c->base && addr < c->base + blocks) { *hi = (uint32_t)(addr - c->base) << 8; return c; }
        } else if (c->t->size > 65536) {
            if (addr == c->base) { *hi = 0; return c; }
            if (addr == c->base + 4) { *hi = 0x10000; return c; }
        } else if (addr == c->base) {
            *hi = 0;
            return c;
        }
    }
    return NULL;
}

bool sim_ee24_has(uint8_t addr)
{
    pthread_mutex_lock(&s_mx);
    uint32_t hi;
    ee24_t *c = ee24_at(addr, &hi);
    bool ok = c && aos_hal_uptime_ms() >= c->busy_until;     /* busy: no ACK */
    pthread_mutex_unlock(&s_mx);
    return ok;
}

bool sim_ee24_xfer(uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    pthread_mutex_lock(&s_mx);
    uint32_t hi;
    ee24_t *c = ee24_at(addr, &hi);
    if (!c || aos_hal_uptime_ms() < c->busy_until) { pthread_mutex_unlock(&s_mx); return false; }
    const ee24_type_t *t = c->t;
    if (wn >= t->abytes) {
        uint32_t a = t->abytes == 1 ? w[0] : (uint32_t)w[0] << 8 | w[1];
        c->ptr = (hi | a) % t->size;
        size_t nd = wn - t->abytes;
        if (nd) {
            /* a page write: the address wraps inside the page */
            uint32_t page0 = c->ptr - c->ptr % t->page, off = c->ptr % t->page;
            for (size_t i = 0; i < nd; i++) c->mem[page0 + (off + i) % t->page] = w[t->abytes + i];
            c->busy_until = aos_hal_uptime_ms() + BUSY_MS;
        }
    }
    for (size_t i = 0; r && i < rn; i++) {
        r[i] = c->mem[c->ptr];
        c->ptr = (c->ptr + 1) % t->size;
    }
    pthread_mutex_unlock(&s_mx);
    return true;
}

/* ======================= SPI: 25xx ======================= */

typedef struct { const char *name; uint32_t size; uint8_t abytes; uint16_t page; } ee25_type_t;
static const ee25_type_t T25[] = {
    { "25lc010", 128, 1, 16 },    { "25lc020", 256, 1, 16 },    { "25lc040", 512, 1, 16 },
    { "25lc080", 1024, 2, 16 },   { "25lc160", 2048, 2, 16 },   { "25lc320", 4096, 2, 32 },
    { "25lc640", 8192, 2, 32 },   { "25lc128", 16384, 2, 64 },  { "25lc256", 32768, 2, 64 },
    { "25lc512", 65536, 2, 128 }, { "25lc1024", 131072, 3, 256 },
};
#define N25 ((int)(sizeof T25 / sizeof T25[0]))

typedef struct { uint8_t *mem; uint8_t sr; uint64_t busy_until; } ee25_t;
static ee25_t s_25[N25];

int sim_ee25_type(const char *name, size_t len)
{
    for (int i = 0; i < N25; i++)
        if (strlen(T25[i].name) == len && !strncasecmp(name, T25[i].name, len)) return i;
    return -1;
}

static bool ee25_protected(const ee25_t *c, const ee25_type_t *t, uint32_t a)
{
    int bp = (c->sr >> 2) & 3;      /* none, upper 1/4, upper 1/2, all */
    if (!bp) return false;
    uint32_t from = bp == 1 ? t->size - t->size / 4 : bp == 2 ? t->size / 2 : 0;
    return a >= from;
}

/* One transaction, CS low for its whole length. */
void sim_ee25_xfer(int type, const uint8_t *tx, uint8_t *rx, size_t n)
{
    memset(rx, 0xFF, n);
    if (type < 0 || type >= N25 || !n) return;
    pthread_mutex_lock(&s_mx);
    const ee25_type_t *t = &T25[type];
    ee25_t *c = &s_25[type];
    if (!c->mem) { c->mem = malloc(t->size); fill(c->mem, t->size, t->name); }
    bool busy = aos_hal_uptime_ms() < c->busy_until;
    uint8_t op = tx[0];
    uint32_t a9 = 0;
    if (t->size == 512 && (op & 0xF7) == 0x03) { a9 = (op & 0x08) ? 0x100 : 0; op &= 0xF7; }
    if (t->size == 512 && (op & 0xF7) == 0x02) { a9 = (op & 0x08) ? 0x100 : 0; op &= 0xF7; }
    switch (op) {
    case 0x05:                      /* RDSR: WIP is bit 0 */
        for (size_t i = 1; i < n; i++) rx[i] = (uint8_t)((c->sr & 0x8C) | (c->sr & 0x02) | (busy ? 1 : 0));
        break;
    case 0x06: if (!busy) c->sr |= 0x02; break;              /* WREN */
    case 0x04: if (!busy) c->sr &= (uint8_t)~0x02; break;    /* WRDI */
    case 0x01:                      /* WRSR: BP1 BP0 (and WPEN) */
        if (!busy && (c->sr & 0x02) && n >= 2) {
            c->sr = (uint8_t)((c->sr & 0x03) | (tx[1] & 0x8C));
            c->sr &= (uint8_t)~0x02;
            c->busy_until = aos_hal_uptime_ms() + BUSY_MS;
        }
        break;
    case 0xAB:                      /* the 1024's signature */
        if (t->abytes == 3) for (size_t i = 4; i < n; i++) rx[i] = 0x29;
        break;
    case 0x03:
    case 0x02: {
        if (busy || n < 1u + t->abytes) break;
        uint32_t a = a9;
        for (int k = 0; k < t->abytes; k++) a = a << 8 | tx[1 + k];
        a %= t->size;
        size_t d0 = 1u + t->abytes;
        if (op == 0x03) {
            for (size_t i = d0; i < n; i++) { rx[i] = c->mem[a]; a = (a + 1) % t->size; }
        } else if (c->sr & 0x02) {
            uint32_t page0 = a - a % t->page, off = a % t->page;
            for (size_t i = d0; i < n; i++) {
                uint32_t at = page0 + (uint32_t)((off + (i - d0)) % t->page);
                if (!ee25_protected(c, t, at)) c->mem[at] = tx[i];
            }
            c->sr &= (uint8_t)~0x02;
            c->busy_until = aos_hal_uptime_ms() + BUSY_MS;
        }
        break;
    }
    default:
        break;
    }
    pthread_mutex_unlock(&s_mx);
}

/* ======================= Microwire: 93xx ======================= */

typedef struct { const char *name; uint32_t bits; } ee93_type_t;   /* size in bits */
static const ee93_type_t T93[] = {
    { "93c46", 1024 }, { "93c56", 2048 }, { "93c66", 4096 }, { "93c76", 8192 }, { "93c86", 16384 },
};

static struct {
    bool on;
    const ee93_type_t *t;
    int cs, sk, di, dout, org;      /* org: 8 or 16 bits a word */
    uint16_t *mem;                  /* words (bytes kept in the low 8 bits with org=8) */
    int abits;
    /* the shift state */
    int cs_level, sk_level;
    uint32_t in;                    /* bits clocked in since the start bit */
    int nin;
    bool started;
    int phase;                      /* 0 command, 1 data in (write/wral), 2 reading out */
    uint32_t op, addr;
    uint32_t out;                   /* the word being shifted out */
    int nout;
    bool ewen;
    uint64_t busy_until;
    int dlevel;                     /* what DO drives (-1: high impedance) */
} M;

static void ee93_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    const char *e = getenv("P4_SIM_93C");
    if (!e || !*e) return;
    char name[16] = "";
    sscanf(e, "%15[^:]", name);
    for (size_t i = 0; i < sizeof T93 / sizeof T93[0]; i++) if (!strcasecmp(name, T93[i].name)) M.t = &T93[i];
    if (!M.t) { aos_hal_log("sim", "93C: no chip called \"%s\"", name); return; }
    M.cs = M.sk = M.di = M.dout = -1;
    M.org = 16;
    const char *p;
    if ((p = strstr(e, "cs="))) M.cs = atoi(p + 3);
    if ((p = strstr(e, "sk="))) M.sk = atoi(p + 3);
    if ((p = strstr(e, "di="))) M.di = atoi(p + 3);
    if ((p = strstr(e, "do="))) M.dout = atoi(p + 3);
    if ((p = strstr(e, "org="))) M.org = atoi(p + 4) == 8 ? 8 : 16;
    if (M.cs < 0 || M.sk < 0 || M.di < 0 || M.dout < 0) { aos_hal_log("sim", "93C: cs, sk, di and do are needed"); return; }
    uint32_t words = M.t->bits / (uint32_t)M.org;
    M.mem = calloc(words, sizeof(uint16_t));
    uint8_t *tmp = malloc(M.t->bits / 8);
    fill(tmp, M.t->bits / 8, M.t->name);
    for (uint32_t w = 0; w < words; w++)
        M.mem[w] = M.org == 8 ? tmp[w] : (uint16_t)(tmp[2 * w] << 8 | tmp[2 * w + 1]);
    free(tmp);
    for (M.abits = 0; (1u << M.abits) < words; M.abits++) {}
    M.dlevel = -1;
    M.on = true;
    aos_hal_log("sim", "emulated %s (x%d) on CS%d SK%d DI%d DO%d", M.t->name, M.org, M.cs, M.sk, M.di, M.dout);
}

static void ee93_reset(void)
{
    M.in = 0; M.nin = 0; M.started = false; M.phase = 0; M.nout = 0;
}

static void ee93_command(void)
{
    uint32_t words = M.t->bits / (uint32_t)M.org;
    uint32_t mask = words - 1;
    bool busy = aos_hal_uptime_ms() < M.busy_until;
    switch (M.op) {
    case 2:                         /* READ: a dummy 0, then the words */
        M.out = M.mem[M.addr & mask];
        M.nout = M.org;
        M.phase = 2;
        M.dlevel = 0;
        return;
    case 1: case 0x4 | 1:           /* WRITE / WRAL: the data comes next */
        M.phase = 1;
        M.in = 0; M.nin = 0;
        return;
    case 3:                         /* ERASE */
        if (M.ewen && !busy) { M.mem[M.addr & mask] = M.org == 8 ? 0xFF : 0xFFFF; M.busy_until = aos_hal_uptime_ms() + 2; }
        break;
    case 0: {                       /* the extended ones, by the two top address bits */
        uint32_t ext = M.addr >> (M.abits - 2);
        if (ext == 3) M.ewen = true;
        else if (ext == 0) M.ewen = false;
        else if (ext == 2 && M.ewen && !busy) {         /* ERAL */
            for (uint32_t w = 0; w < words; w++) M.mem[w] = M.org == 8 ? 0xFF : 0xFFFF;
            M.busy_until = aos_hal_uptime_ms() + 2;
        } else if (ext == 1) {                          /* WRAL */
            M.op = 0x4 | 1;
            M.phase = 1;
            M.in = 0; M.nin = 0;
            return;
        }
        break;
    }
    }
    M.phase = 3;                    /* done: ignore the rest until CS falls */
}

void sim_ee93_gpio_set(int gpio, int level)
{
    pthread_mutex_lock(&s_mx);
    ee93_init();
    if (!M.on) { pthread_mutex_unlock(&s_mx); return; }
    if (gpio == M.cs) {
        if (!level && M.cs_level) { ee93_reset(); M.dlevel = -1; }
        if (level && !M.cs_level) {
            ee93_reset();
            /* status after a write: 0 busy, 1 ready */
            M.dlevel = aos_hal_uptime_ms() < M.busy_until ? 0 : 1;
        }
        M.cs_level = level;
    } else if (gpio == M.sk) {
        bool rise = level && !M.sk_level;
        M.sk_level = level;
        if (M.cs_level && rise) {
            extern int sim_io_gpio_level(int gpio);
            int bit = sim_io_gpio_level(M.di);
            uint32_t words = M.t->bits / (uint32_t)M.org;
            if (M.phase == 2) {
                /* reading out: each rising edge brings the next bit */
                if (M.nout == 0) {
                    M.addr = (M.addr + 1) & (words - 1);
                    M.out = M.mem[M.addr];
                    M.nout = M.org;
                }
                M.dlevel = (int)((M.out >> (M.nout - 1)) & 1);
                M.nout--;
            } else if (!M.started) {
                if (bit) { M.started = true; M.in = 0; M.nin = 0; }
            } else if (M.phase == 0) {
                M.in = M.in << 1 | (uint32_t)bit;
                if (++M.nin == 2 + M.abits) {
                    M.op = M.in >> M.abits;
                    M.addr = M.in & ((1u << M.abits) - 1);
                    ee93_command();
                }
            } else if (M.phase == 1) {
                M.in = M.in << 1 | (uint32_t)bit;
                if (++M.nin == M.org) {
                    if (M.ewen && aos_hal_uptime_ms() >= M.busy_until) {
                        if (M.op == 1) M.mem[M.addr & (words - 1)] = (uint16_t)M.in;
                        else for (uint32_t w = 0; w < words; w++) M.mem[w] = (uint16_t)M.in;
                        M.busy_until = aos_hal_uptime_ms() + 2;
                    }
                    M.phase = 3;
                }
            }
        }
    }
    pthread_mutex_unlock(&s_mx);
}

/* What DO drives, if this is the chip's DO pin and it is driving it. */
bool sim_ee93_gpio_get(int gpio, int *level)
{
    pthread_mutex_lock(&s_mx);
    ee93_init();
    bool driving = M.on && gpio == M.dout && M.cs_level && M.dlevel >= 0;
    if (driving) {
        /* busy/ready status follows the clock of time, not of SK */
        if (M.phase == 0 && !M.started) M.dlevel = aos_hal_uptime_ms() < M.busy_until ? 0 : 1;
        *level = M.dlevel;
    }
    pthread_mutex_unlock(&s_mx);
    return driving;
}
