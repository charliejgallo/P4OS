/*
 * P4OS - EEPROM: the chips it knows, and where the chosen one hangs.
 *
 * One row a part, with the numbers its datasheet gives and the other makers'
 * names for the same thing. Pages are the smallest any maker of that size
 * uses (a 25LC080A writes 16 bytes, the B 32: 16 is right for both), so a
 * write never wraps inside a page whoever made the chip.
 *
 * I2C 24xx up to 16 Kbit take one address byte and put the high address
 * bits in the device address, in place of the A pins they do not decode: a
 * 24LC16 answers at 0x50..0x57. From 32 Kbit two address bytes; past 64 KB
 * the upper half is another device address (+4 on the 24LC1025, whose A2
 * pin must be tied high; +1 on the AT24CM01 / M24M01).
 *
 * Microwire 93xx: 'abytes' is the number of address bits at x16, as the
 * datasheets give it (93C56 and 93C66 both take 8, the 56 ignoring the top
 * one; the same for 76 and 86); at x8 it is one more.
 */
#include "ee.h"

#include <stdio.h>
#include <string.h>

static const ee_chip_t CHIPS[] = {
    /* name        also                                   fam     ab pins blk page twc size */
    { "24LC01",   "24AA01 · AT24C01 · M24C01",             EE_I2C, 1, 7, 0,   8,  5,    128 },
    { "24LC02",   "24AA02 · AT24C02 · M24C02",             EE_I2C, 1, 7, 0,   8,  5,    256 },
    { "24LC04",   "24AA04 · AT24C04 · M24C04",             EE_I2C, 1, 6, 0,  16,  5,    512 },
    { "24LC08",   "24AA08 · AT24C08 · M24C08",             EE_I2C, 1, 4, 0,  16,  5,   1024 },
    { "24LC16",   "24AA16 · AT24C16 · M24C16",             EE_I2C, 1, 0, 0,  16,  5,   2048 },
    { "24LC32",   "24AA32 · AT24C32 · M24C32",             EE_I2C, 2, 7, 0,  32,  5,   4096 },
    { "24LC64",   "24AA64 · AT24C64 · M24C64",             EE_I2C, 2, 7, 0,  32,  5,   8192 },
    { "24LC128",  "24AA128 · AT24C128 · M24128",           EE_I2C, 2, 7, 0,  64,  5,  16384 },
    { "24LC256",  "24AA256 · AT24C256 · M24256",           EE_I2C, 2, 7, 0,  64,  5,  32768 },
    { "24LC512",  "24AA512 · AT24C512 · M24512",           EE_I2C, 2, 7, 0, 128,  5,  65536 },
    { "24LC1025", "24AA1025 · 24FC1025",                   EE_I2C, 2, 3, 4, 128,  5, 131072 },
    { "AT24CM01", "M24M01",                                EE_I2C, 2, 6, 1, 256, 10, 131072 },

    { "25LC010",  "25AA010 · AT25010 · M95010",            EE_SPI, 1, 0, 0,  16,  5,    128 },
    { "25LC020",  "25AA020 · AT25020 · M95020",            EE_SPI, 1, 0, 0,  16,  5,    256 },
    { "25LC040",  "25AA040 · AT25040 · M95040",            EE_SPI, 1, 0, 0,  16,  5,    512 },
    { "25LC080",  "25AA080 · AT25080 · M95080",            EE_SPI, 2, 0, 0,  16,  5,   1024 },
    { "25LC160",  "25AA160 · AT25160 · M95160",            EE_SPI, 2, 0, 0,  16,  5,   2048 },
    { "25LC320",  "25AA320 · AT25320 · M95320",            EE_SPI, 2, 0, 0,  32,  5,   4096 },
    { "25LC640",  "25AA640 · AT25640 · M95640",            EE_SPI, 2, 0, 0,  32,  5,   8192 },
    { "25LC128",  "25AA128 · AT25128 · M95128",            EE_SPI, 2, 0, 0,  64,  5,  16384 },
    { "25LC256",  "25AA256 · AT25256 · M95256",            EE_SPI, 2, 0, 0,  64,  5,  32768 },
    { "25LC512",  "25AA512 · AT25512 · M95512",            EE_SPI, 2, 0, 0, 128,  5,  65536 },
    { "25LC1024", "25AA1024 · M95M01",                     EE_SPI, 3, 0, 0, 256,  6, 131072 },
    { "M95M02",   "AT25M02",                               EE_SPI, 3, 0, 0, 256, 10, 262144 },

    { "25Q40",    "W25Q40 · GD25Q40 · MX25L4006",          EE_FLASH, 3, 0, 0, 256, 3,   524288 },
    { "25Q80",    "W25Q80 · GD25Q80 · MX25L8006",          EE_FLASH, 3, 0, 0, 256, 3,  1048576 },
    { "25Q16",    "W25Q16 · GD25Q16 · MX25L1606",          EE_FLASH, 3, 0, 0, 256, 3,  2097152 },
    { "25Q32",    "W25Q32 · GD25Q32 · MX25L3206",          EE_FLASH, 3, 0, 0, 256, 3,  4194304 },
    { "25Q64",    "W25Q64 · GD25Q64 · MX25L6406",          EE_FLASH, 3, 0, 0, 256, 3,  8388608 },
    { "25Q128",   "W25Q128 · GD25Q128 · MX25L12835",       EE_FLASH, 3, 0, 0, 256, 3, 16777216 },

    { "93C46",    "93LC46 · 93AA46 · AT93C46 · M93C46",    EE_MW,  6, 0, 0,   2, 10,    128 },
    { "93C56",    "93LC56 · 93AA56 · AT93C56 · M93C56",    EE_MW,  8, 0, 0,   2, 10,    256 },
    { "93C66",    "93LC66 · 93AA66 · AT93C66 · M93C66",    EE_MW,  8, 0, 0,   2, 10,    512 },
    { "93C76",    "93LC76 · 93AA76 · M93C76",              EE_MW, 10, 0, 0,   2, 10,   1024 },
    { "93C86",    "93LC86 · 93AA86 · AT93C86 · M93C86",    EE_MW, 10, 0, 0,   2, 10,   2048 },
};
#define N_CHIPS ((int)(sizeof CHIPS / sizeof CHIPS[0]))

int ee_chip_count(void) { return N_CHIPS; }

const ee_chip_t *ee_chip_at(int i) { return i >= 0 && i < N_CHIPS ? &CHIPS[i] : &CHIPS[8]; }

int ee_chip_find(const char *name)
{
    for (int i = 0; name && i < N_CHIPS; i++)
        if (!strcmp(CHIPS[i].name, name)) return i;
    return -1;
}

int ee_chip_by_size(int fam, uint32_t size, uint8_t abytes)
{
    for (int i = 0; i < N_CHIPS; i++)
        if (CHIPS[i].fam == fam && CHIPS[i].size == size && (!abytes || CHIPS[i].abytes == abytes)) return i;
    return -1;
}

const char *ee_fam_name(int fam)
{
    static const char *const N[EE_FAM_COUNT] = { "I2C 24xx", "SPI 25xx", "Flash 25Qxx", "Microwire 93xx" };
    return fam >= 0 && fam < EE_FAM_COUNT ? N[fam] : "?";
}

void ee_size_text(uint32_t n, char *out, size_t len)
{
    if (n >= 1048576 && !(n % 1048576)) snprintf(out, len, "%u MB", (unsigned)(n / 1048576));
    else if (n >= 1024 && !(n % 1024)) snprintf(out, len, "%u KB", (unsigned)(n / 1024));
    else snprintf(out, len, "%u B", (unsigned)n);
}

uint8_t ee_i2c_dev(const ee_chip_t *c, uint8_t base, uint32_t addr)
{
    if (c->abytes == 1) return (uint8_t)(base + (addr >> 8));
    if (c->size > 65536) return (uint8_t)(base + (addr >> 16) * c->blk);
    return base;
}

int ee_i2c_bases(const ee_chip_t *c, uint8_t *out, int max)
{
    int n = 0;
    for (int a = 0; a < 8 && n < max; a++)
        if (!(a & ~c->apins)) out[n++] = (uint8_t)(0x50 + a);
    return n;
}

/* -------------------------------------------------------------------------- */
/* Where it hangs, kept in the preferences                                     */
/* -------------------------------------------------------------------------- */

void ee_conf_load(ee_conf_t *c)
{
    memset(c, 0, sizeof *c);
    char name[24] = "";
    aos_hal_pref_get_str("ee.chip", name, sizeof name);
    c->chip = ee_chip_find(name);
    if (c->chip < 0) c->chip = ee_chip_find("24LC256");
    snprintf(c->i2c_port, sizeof c->i2c_port, "i2c.ext");
    aos_hal_pref_get_str("ee.i2c", c->i2c_port, sizeof c->i2c_port);
    int32_t v;
    c->i2c_addr = aos_hal_pref_get_i32("ee.addr", &v) && v >= 0x50 && v <= 0x57 ? (uint8_t)v : 0x50;
    snprintf(c->spi_port, sizeof c->spi_port, "spi.a");
    aos_hal_pref_get_str("ee.spi", c->spi_port, sizeof c->spi_port);
    c->spi_cs = aos_hal_pref_get_i32("ee.cs", &v) && v >= -1 && v < 64 ? (int)v : -1;
    c->spi_hz = aos_hal_pref_get_i32("ee.hz", &v) && v >= (int32_t)AOS_IO_SPI_MIN_HZ ? (uint32_t)v : 1000000;
    /* the simulator's example wiring (P4_SIM_93C), plain GPIOs of the header */
    static const int MW_DEF[4] = { 28, 32, 34, 46 };
    memcpy(c->mw, MW_DEF, sizeof c->mw);
    char mw[32] = "";
    if (aos_hal_pref_get_str("ee.mw", mw, sizeof mw)) {
        int p[4];
        if (sscanf(mw, "%d,%d,%d,%d", &p[0], &p[1], &p[2], &p[3]) == 4) memcpy(c->mw, p, sizeof p);
    }
    c->mw_org = aos_hal_pref_get_i32("ee.org", &v) && v == 8 ? 8 : 16;
}

void ee_conf_save(const ee_conf_t *c)
{
    aos_hal_pref_set_str("ee.chip", ee_chip_at(c->chip)->name);
    aos_hal_pref_set_str("ee.i2c", c->i2c_port);
    aos_hal_pref_set_i32("ee.addr", c->i2c_addr);
    aos_hal_pref_set_str("ee.spi", c->spi_port);
    aos_hal_pref_set_i32("ee.cs", c->spi_cs);
    aos_hal_pref_set_i32("ee.hz", (int32_t)c->spi_hz);
    char mw[32];
    snprintf(mw, sizeof mw, "%d,%d,%d,%d", c->mw[0], c->mw[1], c->mw[2], c->mw[3]);
    aos_hal_pref_set_str("ee.mw", mw);
    aos_hal_pref_set_i32("ee.org", c->mw_org);
}
