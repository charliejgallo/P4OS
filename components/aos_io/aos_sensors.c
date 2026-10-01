/*
 * P4OS - I2C sensors: the drivers, the autodetect and the service thread
 * (aos_sensors.h, docs/MODULES.md).
 *
 * Platform-free: everything goes through aos_io_i2c_*, so the same code
 * reads a real BME280 on the board and sim/sensors_sim.c's in the simulator.
 *
 * One thread, one round a second:
 *   1. the sensors modules.txt declares, merged with the ones autodetect
 *      found, make the table (one slot per port + address);
 *   2. per I2C port with something to do: open it (claiming its pins as
 *      "Sensores"), autodetect when it is due, read every slot on it, and
 *      close it again - the Bus app can have the port between two rounds;
 *   3. the values go to the table under the lock, with a five-minute
 *      history per value.
 *
 * The drivers do only what the chips need after power-on. Autodetect only
 * reads registers and sends the read-only commands a chip answers with its
 * id or a CRC (SHT status and serial, AHT status); it never configures a
 * chip it has not identified first - a declared one, the user has said is
 * there.
 */
#include "aos_sensors.h"
#include "aos_io.h"
#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* strncpy that always terminates, and that GCC's format-truncation check
 * leaves alone (it flags every snprintf of a longer buffer into a shorter) */
static void scpy(char *d, size_t n, const char *s)
{
    size_t i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

#define OWNER       "Sensores"
#define XFER_MS     50
#define ROUND_MS    1000
#define SCAN_FAST   5000        /* autodetect while somebody looks */
#define SCAN_SLOW   60000
#define KEEP_MS     10000

/* -------------------------------------------------------------------------- */
/* The chips                                                                   */
/* -------------------------------------------------------------------------- */

enum { CH_BME280, CH_BMP280, CH_SHT3X, CH_SHT4X, CH_AHT20, CH_BH1750, CH_INA219, CH_INA226, CH_ADS1115, CH_N };

static const aos_sensor_chip_t CHIPS[CH_N] = {
    [CH_BME280]  = { "bme280",  "BME280",  { 0x76, 0x77 }, 2, false, false },
    [CH_BMP280]  = { "bmp280",  "BMP280",  { 0x76, 0x77 }, 2, false, false },
    [CH_SHT3X]   = { "sht3x",   "SHT3x",   { 0x44, 0x45 }, 2, false, false },
    [CH_SHT4X]   = { "sht4x",   "SHT4x",   { 0x44, 0x45 }, 2, false, false },
    [CH_AHT20]   = { "aht20",   "AHT20",   { 0x38 }, 1, false, false },
    [CH_BH1750]  = { "bh1750",  "BH1750",  { 0x23, 0x5C }, 2, false, false },
    [CH_INA219]  = { "ina219",  "INA219",  { 0x40, 0x41, 0x44, 0x45 }, 4, true, false },
    [CH_INA226]  = { "ina226",  "INA226",  { 0x40, 0x41, 0x44, 0x45 }, 4, true, false },
    [CH_ADS1115] = { "ads1115", "ADS1115", { 0x48, 0x49, 0x4A, 0x4B }, 4, false, true },
};

/* Other names people give them in modules.txt */
static const struct { const char *alias; int chip; } ALIAS[] = {
    { "sht30", CH_SHT3X }, { "sht31", CH_SHT3X }, { "sht35", CH_SHT3X },
    { "sht40", CH_SHT4X }, { "sht41", CH_SHT4X }, { "sht45", CH_SHT4X },
    { "aht10", CH_AHT20 }, { "aht21", CH_AHT20 }, { "aht2x", CH_AHT20 }, { "aht25", CH_AHT20 },
    { "gy302", CH_BH1750 },
};

int aos_sensor_chip_count(void) { return CH_N; }
const aos_sensor_chip_t *aos_sensor_chip_at(int i) { return i >= 0 && i < CH_N ? &CHIPS[i] : NULL; }

static int chip_index(const char *id)
{
    if (!id || !*id) return -1;
    for (int i = 0; i < CH_N; i++) if (!strcmp(CHIPS[i].id, id)) return i;
    for (size_t i = 0; i < sizeof ALIAS / sizeof ALIAS[0]; i++) if (!strcmp(ALIAS[i].alias, id)) return ALIAS[i].chip;
    /* "bme280_b", "ina219-banco": the part before the separator */
    char base[24];
    size_t n = strcspn(id, "_-.");
    if (n == strlen(id) || n >= sizeof base) return -1;
    memcpy(base, id, n);
    base[n] = 0;
    return chip_index(base);
}

const aos_sensor_chip_t *aos_sensor_chip_find(const char *id)
{
    int c = chip_index(id);
    return c >= 0 ? &CHIPS[c] : NULL;
}

const char *aos_sensor_unit(int q)
{
    static const char *const U[AOS_SQ_COUNT] = { "°C", "%", "hPa", "lx", "V", "A", "W" };
    return q >= 0 && q < AOS_SQ_COUNT ? U[q] : "";
}

/* -------------------------------------------------------------------------- */
/* The table                                                                   */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint16_t T1; int16_t T2, T3;
    uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t  H1, H3; int16_t H2, H4, H5; int8_t H6;
} bme_cal_t;

typedef struct {
    bool     used;
    aos_sensor_t pub;           /* under s_mx */
    int      chip;
    bool     declared_now;      /* in modules.txt this round */
    bool     detected;          /* autodetect found it */
    int32_t  shunt_mohm, gain_mv;
    bool     nocrc;             /* aht10: no CRC byte */
    /* the driver's state */
    bool     inited;
    uint32_t init_ms;
    bme_cal_t cal;
    float   *hist;              /* [AOS_SENSOR_VALUES][AOS_SENSOR_HIST] */
    int      h_pos, h_n;
} slot_t;

typedef aos_sensor_port_t port_st_t;

static slot_t s_slot[AOS_SENSOR_MAX];
static aos_sensor_cand_t s_cand[AOS_SENSOR_CAND_MAX];
static int s_ncand;
static port_st_t s_pst[AOS_IO_PORT_MAX];
static int s_npst;
static void *s_mx;
static int s_started, s_ready;
static volatile bool s_rescan;
static volatile uint32_t s_seq, s_keep_ms;

static void lock(void) { aos_hal_mutex_lock(s_mx); }
static void unlock(void) { aos_hal_mutex_unlock(s_mx); }

/* -------------------------------------------------------------------------- */
/* Bus helpers                                                                 */
/* -------------------------------------------------------------------------- */

static bool wr(aos_io_i2c_t *b, uint8_t a, const uint8_t *w, size_t n) { return aos_io_i2c_xfer(b, a, w, n, NULL, 0, XFER_MS); }
static bool rd(aos_io_i2c_t *b, uint8_t a, uint8_t *r, size_t n) { return aos_io_i2c_xfer(b, a, NULL, 0, r, n, XFER_MS); }

static bool reg_rd(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint8_t *r, size_t n)
{
    return aos_io_i2c_xfer(b, a, &reg, 1, r, n, XFER_MS);
}

static bool reg_wr8(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint8_t v)
{
    uint8_t w[2] = { reg, v };
    return wr(b, a, w, 2);
}

static bool reg_rd16(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint16_t *v)
{
    uint8_t r[2];
    if (!reg_rd(b, a, reg, r, 2)) return false;
    *v = (uint16_t)(r[0] << 8 | r[1]);
    return true;
}

static bool reg_wr16(aos_io_i2c_t *b, uint8_t a, uint8_t reg, uint16_t v)
{
    uint8_t w[3] = { reg, (uint8_t)(v >> 8), (uint8_t)v };
    return wr(b, a, w, 3);
}

/* Sensirion's and Aosong's CRC-8: polynomial 0x31, starts at 0xFF */
static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t c = 0xFF;
    for (int i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x31 : c << 1);
    }
    return c;
}

static bool cmd16(aos_io_i2c_t *b, uint8_t a, uint16_t cmd)
{
    uint8_t w[2] = { (uint8_t)(cmd >> 8), (uint8_t)cmd };
    return wr(b, a, w, 2);
}

/* -------------------------------------------------------------------------- */
/* The drivers: each fills v[] and returns an AOS_SERR_*-like code             */
/* -------------------------------------------------------------------------- */

enum { R_OK = 0, R_NACK, R_CRC, R_ID, R_NOTREADY };

static void setv(slot_t *s, int k, const char *key, int q, float v)
{
    scpy(s->pub.v[k].key, sizeof s->pub.v[k].key, key);
    s->pub.v[k].q = (uint8_t)q;
    s->pub.v[k].v = v;
    if (s->pub.n < k + 1) s->pub.n = k + 1;
}

/* ---- BME280 / BMP280 (Bosch datasheet, section 4.2.3 and 8.2) ---- */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int bme_init(aos_io_i2c_t *b, slot_t *s)
{
    uint8_t id = 0;
    if (!reg_rd(b, s->pub.addr, 0xD0, &id, 1)) return R_NACK;
    bool is280 = id == 0x60, isbmp = id == 0x58 || id == 0x56 || id == 0x57;
    if ((s->chip == CH_BME280 && !is280) || (s->chip == CH_BMP280 && !isbmp)) {
        if (is280) s->chip = CH_BME280;          /* declared as the other one: trust the chip */
        else if (isbmp) s->chip = CH_BMP280;
        else { snprintf(s->pub.err, sizeof s->pub.err, "0x%02X", id); return R_ID; }
        scpy(s->pub.chip, sizeof s->pub.chip, CHIPS[s->chip].name);
    }
    reg_wr8(b, s->pub.addr, 0xE0, 0xB6);           /* soft reset: a known state */
    aos_hal_sleep_ms(4);
    for (int i = 0; i < 10; i++) {                  /* im_update: the NVM copy is done */
        uint8_t st = 1;
        if (reg_rd(b, s->pub.addr, 0xF3, &st, 1) && !(st & 0x01)) break;
        aos_hal_sleep_ms(2);
    }
    uint8_t c[26], h[7];
    if (!reg_rd(b, s->pub.addr, 0x88, c, 26)) return R_NACK;
    bme_cal_t *k = &s->cal;
    k->T1 = le16(c + 0);  k->T2 = (int16_t)le16(c + 2);  k->T3 = (int16_t)le16(c + 4);
    k->P1 = le16(c + 6);  k->P2 = (int16_t)le16(c + 8);  k->P3 = (int16_t)le16(c + 10);
    k->P4 = (int16_t)le16(c + 12); k->P5 = (int16_t)le16(c + 14); k->P6 = (int16_t)le16(c + 16);
    k->P7 = (int16_t)le16(c + 18); k->P8 = (int16_t)le16(c + 20); k->P9 = (int16_t)le16(c + 22);
    k->H1 = c[25];
    if (s->chip == CH_BME280) {
        if (!reg_rd(b, s->pub.addr, 0xE1, h, 7)) return R_NACK;
        k->H2 = (int16_t)le16(h);
        k->H3 = h[2];
        k->H4 = (int16_t)((int16_t)(int8_t)h[3] * 16 | (h[4] & 0x0F));
        k->H5 = (int16_t)((int16_t)(int8_t)h[5] * 16 | (h[4] >> 4));
        k->H6 = (int8_t)h[6];
        reg_wr8(b, s->pub.addr, 0xF2, 0x01);       /* humidity x1: before ctrl_meas, which latches it */
    }
    if (k->T1 == 0 || k->P1 == 0) return R_ID;     /* no calibration: not a real one */
    reg_wr8(b, s->pub.addr, 0xF5, 0x88);           /* standby 500 ms, IIR filter 4 */
    reg_wr8(b, s->pub.addr, 0xF4, 0x57);           /* T x2, P x16, normal mode */
    return R_OK;
}

static int bme_read(aos_io_i2c_t *b, slot_t *s)
{
    uint8_t d[8];
    int n = s->chip == CH_BME280 ? 8 : 6;
    if (!reg_rd(b, s->pub.addr, 0xF7, d, (size_t)n)) return R_NACK;
    int32_t adc_P = (int32_t)(d[0] << 12 | d[1] << 4 | d[2] >> 4);
    int32_t adc_T = (int32_t)(d[3] << 12 | d[4] << 4 | d[5] >> 4);
    if (adc_T == 0x80000) return R_NOTREADY;       /* the reset value: no measurement yet */
    const bme_cal_t *k = &s->cal;

    /* temperature, 0.01 °C, and t_fine for the other two */
    int32_t v1 = ((((adc_T >> 3) - ((int32_t)k->T1 << 1))) * (int32_t)k->T2) >> 11;
    int32_t v2 = (((((adc_T >> 4) - (int32_t)k->T1) * ((adc_T >> 4) - (int32_t)k->T1)) >> 12) * (int32_t)k->T3) >> 14;
    int32_t t_fine = v1 + v2;
    int32_t T = (t_fine * 5 + 128) >> 8;
    setv(s, 0, "temp", AOS_SQ_TEMP, T / 100.0f);

    /* pressure, Pa in Q24.8 */
    int64_t p1 = (int64_t)t_fine - 128000;
    int64_t p2 = p1 * p1 * (int64_t)k->P6;
    p2 += (p1 * (int64_t)k->P5) << 17;
    p2 += ((int64_t)k->P4) << 35;
    p1 = ((p1 * p1 * (int64_t)k->P3) >> 8) + ((p1 * (int64_t)k->P2) << 12);
    p1 = ((((int64_t)1) << 47) + p1) * (int64_t)k->P1 >> 33;
    if (p1 == 0 || adc_P == 0x80000) return R_NOTREADY;
    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - p2) * 3125) / p1;
    p1 = ((int64_t)k->P9 * (p >> 13) * (p >> 13)) >> 25;
    p2 = ((int64_t)k->P8 * p) >> 19;
    p = ((p + p1 + p2) >> 8) + ((int64_t)k->P7 << 4);
    setv(s, 1, "press", AOS_SQ_PRESS, (float)p / 256.0f / 100.0f);

    if (s->chip == CH_BME280) {
        int32_t adc_H = (int32_t)(d[6] << 8 | d[7]);
        if (adc_H == 0x8000) return R_NOTREADY;
        int32_t x = t_fine - 76800;
        x = (((((adc_H << 14) - ((int32_t)k->H4 << 20) - ((int32_t)k->H5 * x)) + 16384) >> 15) *
             (((((((x * (int32_t)k->H6) >> 10) * (((x * (int32_t)k->H3) >> 11) + 32768)) >> 10) + 2097152) *
               (int32_t)k->H2 + 8192) >> 14));
        x = x - (((((x >> 15) * (x >> 15)) >> 7) * (int32_t)k->H1) >> 4);
        if (x < 0) x = 0;
        if (x > 419430400) x = 419430400;
        setv(s, 2, "hum", AOS_SQ_HUM, (float)(x >> 12) / 1024.0f);
    }
    return R_OK;
}

/* ---- SHT3x: single shot, high repeatability, no clock stretching ---- */

static int sht_pair(slot_t *s, const uint8_t *d, bool sht4)
{
    if (crc8(d, 2) != d[2] || crc8(d + 3, 2) != d[5]) return R_CRC;
    float t = -45.0f + 175.0f * (float)(d[0] << 8 | d[1]) / 65535.0f;
    float h = sht4 ? -6.0f + 125.0f * (float)(d[3] << 8 | d[4]) / 65535.0f
                   : 100.0f * (float)(d[3] << 8 | d[4]) / 65535.0f;
    if (h < 0) h = 0;
    if (h > 100) h = 100;
    setv(s, 0, "temp", AOS_SQ_TEMP, t);
    setv(s, 1, "hum", AOS_SQ_HUM, h);
    return R_OK;
}

static int sht3_read(aos_io_i2c_t *b, slot_t *s)
{
    if (!cmd16(b, s->pub.addr, 0x2400)) return R_NACK;
    aos_hal_sleep_ms(16);
    uint8_t d[6];
    if (!rd(b, s->pub.addr, d, 6)) return R_NACK;
    return sht_pair(s, d, false);
}

static int sht4_read(aos_io_i2c_t *b, slot_t *s)
{
    uint8_t c = 0xFD;                              /* high precision */
    if (!wr(b, s->pub.addr, &c, 1)) return R_NACK;
    aos_hal_sleep_ms(10);
    uint8_t d[6];
    if (!rd(b, s->pub.addr, d, 6)) return R_NACK;
    return sht_pair(s, d, true);
}

/* ---- AHT20 / AHT10 ---- */

static int aht_init(aos_io_i2c_t *b, slot_t *s)
{
    uint8_t c = 0x71, st = 0;
    if (!aos_io_i2c_xfer(b, s->pub.addr, &c, 1, &st, 1, XFER_MS)) return R_NACK;
    if (!(st & 0x08)) {                            /* not calibrated: the init command */
        uint8_t w[3] = { (uint8_t)(s->nocrc ? 0xE1 : 0xBE), 0x08, 0x00 };
        if (!wr(b, s->pub.addr, w, 3)) return R_NACK;
        aos_hal_sleep_ms(10);
    }
    return R_OK;
}

static int aht_read(aos_io_i2c_t *b, slot_t *s)
{
    uint8_t w[3] = { 0xAC, 0x33, 0x00 }, d[7];
    if (!wr(b, s->pub.addr, w, 3)) return R_NACK;
    aos_hal_sleep_ms(80);
    for (int tries = 0;; tries++) {
        if (!rd(b, s->pub.addr, d, s->nocrc ? 6 : 7)) return R_NACK;
        if (!(d[0] & 0x80)) break;
        if (tries >= 2) return R_NOTREADY;
        aos_hal_sleep_ms(20);
    }
    if (!s->nocrc && crc8(d, 6) != d[6]) return R_CRC;
    uint32_t rh = (uint32_t)d[1] << 12 | (uint32_t)d[2] << 4 | d[3] >> 4;
    uint32_t rt = (uint32_t)(d[3] & 0x0F) << 16 | (uint32_t)d[4] << 8 | d[5];
    setv(s, 0, "temp", AOS_SQ_TEMP, (float)rt * 200.0f / 1048576.0f - 50.0f);
    setv(s, 1, "hum", AOS_SQ_HUM, (float)rh * 100.0f / 1048576.0f);
    return R_OK;
}

/* ---- BH1750: continuous high resolution, 1 lx, 120 ms ---- */

static int bh_init(aos_io_i2c_t *b, slot_t *s)
{
    uint8_t on = 0x01, mode = 0x10;
    if (!wr(b, s->pub.addr, &on, 1) || !wr(b, s->pub.addr, &mode, 1)) return R_NACK;
    s->init_ms = (uint32_t)aos_hal_uptime_ms();
    return R_OK;
}

static int bh_read(aos_io_i2c_t *b, slot_t *s)
{
    if ((uint32_t)aos_hal_uptime_ms() - s->init_ms < 180) return R_NOTREADY;
    uint8_t d[2];
    if (!rd(b, s->pub.addr, d, 2)) return R_NACK;
    setv(s, 0, "lux", AOS_SQ_LUX, (float)(d[0] << 8 | d[1]) / 1.2f);
    return R_OK;
}

/* ---- INA219 / INA226: the shunt's voltage over its resistance ----
 * No calibration register: the current is worked out here from the shunt
 * voltage, so the chip keeps its power-on configuration - which is also
 * what identifies an INA219 (0x399F). */

static int ina_init(aos_io_i2c_t *b, slot_t *s)
{
    if (s->chip == CH_INA226) {
        uint16_t id = 0;
        if (!reg_rd16(b, s->pub.addr, 0xFE, &id)) return R_NACK;
        if (id != 0x5449) { snprintf(s->pub.err, sizeof s->pub.err, "0x%04X", id); return R_ID; }
        /* averages of 16, 1.1 ms per conversion, shunt and bus continuous */
        if (!reg_wr16(b, s->pub.addr, 0x00, 0x4527)) return R_NACK;
    }
    return R_OK;
}

static int ina_read(aos_io_i2c_t *b, slot_t *s)
{
    uint16_t sh, bus;
    if (!reg_rd16(b, s->pub.addr, 0x01, &sh) || !reg_rd16(b, s->pub.addr, 0x02, &bus)) return R_NACK;
    float vsh, vbus;
    if (s->chip == CH_INA219) {
        vsh = (float)(int16_t)sh * 10e-6f;
        vbus = (float)(bus >> 3) * 0.004f;
    } else {
        vsh = (float)(int16_t)sh * 2.5e-6f;
        vbus = (float)bus * 0.00125f;
    }
    float r = (float)(s->shunt_mohm > 0 ? s->shunt_mohm : 100) / 1000.0f;
    float i = vsh / r;
    setv(s, 0, "bus", AOS_SQ_VOLT, vbus);
    setv(s, 1, "current", AOS_SQ_CURR, i);
    setv(s, 2, "power", AOS_SQ_POWER, vbus * i);
    setv(s, 3, "shunt", AOS_SQ_VOLT, vsh);
    return R_OK;
}

/* ---- ADS1115: the four inputs against GND, one shot each at 128 SPS ---- */

static int ads_pga(int32_t mv, int32_t *fsr)
{
    static const int32_t FSR[6] = { 6144, 4096, 2048, 1024, 512, 256 };
    int k = 1;
    for (int i = 0; i < 6; i++) if (mv == FSR[i]) k = i;
    *fsr = FSR[k];
    return k;
}

static int ads_read(aos_io_i2c_t *b, slot_t *s)
{
    int32_t fsr;
    int pga = ads_pga(s->gain_mv, &fsr);
    static const char *const KEY[4] = { "a0", "a1", "a2", "a3" };
    for (int ch = 0; ch < 4; ch++) {
        uint16_t cfg = (uint16_t)(0x8000 | (4 + ch) << 12 | pga << 9 | 0x0100 | 4 << 5 | 0x0003);
        if (!reg_wr16(b, s->pub.addr, 0x01, cfg)) return R_NACK;
        aos_hal_sleep_ms(9);
        uint16_t st = 0;
        for (int t = 0; t < 4; t++) {
            if (!reg_rd16(b, s->pub.addr, 0x01, &st)) return R_NACK;
            if (st & 0x8000) break;                /* OS = 1: not converting any more */
            aos_hal_sleep_ms(2);
        }
        if (!(st & 0x8000)) return R_NOTREADY;
        uint16_t raw;
        if (!reg_rd16(b, s->pub.addr, 0x00, &raw)) return R_NACK;
        setv(s, ch, KEY[ch], AOS_SQ_VOLT, (float)(int16_t)raw * (float)fsr / 32768.0f / 1000.0f);
    }
    return R_OK;
}

static int drv_init(aos_io_i2c_t *b, slot_t *s)
{
    switch (s->chip) {
    case CH_BME280: case CH_BMP280: return bme_init(b, s);
    case CH_AHT20: return aht_init(b, s);
    case CH_BH1750: return bh_init(b, s);
    case CH_INA219: case CH_INA226: return ina_init(b, s);
    default: return aos_io_i2c_probe(b, s->pub.addr) ? R_OK : R_NACK;
    }
}

static int drv_read(aos_io_i2c_t *b, slot_t *s)
{
    switch (s->chip) {
    case CH_BME280: case CH_BMP280: return bme_read(b, s);
    case CH_SHT3X: return sht3_read(b, s);
    case CH_SHT4X: return sht4_read(b, s);
    case CH_AHT20: return aht_read(b, s);
    case CH_BH1750: return bh_read(b, s);
    case CH_INA219: case CH_INA226: return ina_read(b, s);
    case CH_ADS1115: return ads_read(b, s);
    default: return R_ID;
    }
}

/* -------------------------------------------------------------------------- */
/* Autodetect: prove what a chip is, without configuring it                   */
/* -------------------------------------------------------------------------- */

/* -1: nothing identifiable; -2 - chip: a candidate for that chip */
#define CAND(c) (-2 - (c))

static int ina_identify(aos_io_i2c_t *b, uint8_t a)
{
    uint16_t v;
    if (reg_rd16(b, a, 0xFE, &v) && v == 0x5449) return CH_INA226;     /* "TI" */
    if (reg_rd16(b, a, 0x00, &v) && v == 0x399F) return CH_INA219;     /* its power-on config */
    return -1;
}

static int identify(aos_io_i2c_t *b, uint8_t a)
{
    uint8_t d[6];
    if (a == 0x76 || a == 0x77) {
        uint8_t id = 0;
        if (!reg_rd(b, a, 0xD0, &id, 1)) return -1;
        if (id == 0x60) return CH_BME280;
        if (id == 0x58 || id == 0x56 || id == 0x57) return CH_BMP280;
        return -1;                                 /* a BME680 (0x61) and others: the Bus app */
    }
    if (a == 0x38) {
        uint8_t c = 0x71, st = 0xFF;
        if (aos_io_i2c_xfer(b, a, &c, 1, &st, 1, XFER_MS) && !(st & 0x80) && (st & 0x08)) return CH_AHT20;
        return CAND(CH_AHT20);
    }
    if (a == 0x23 || a == 0x5C) return CAND(CH_BH1750);   /* no id: a PCF8574 looks the same */
    if (a >= 0x48 && a <= 0x4B) {
        uint16_t lo, hi;
        if (reg_rd16(b, a, 0x02, &lo) && reg_rd16(b, a, 0x03, &hi) && lo == 0x8000 && hi == 0x7FFF) return CH_ADS1115;
        int c = ina_identify(b, a);
        return c >= 0 ? c : CAND(CH_ADS1115);
    }
    if (a >= 0x40 && a <= 0x4F) {
        int c = ina_identify(b, a);
        if (c >= 0) return c;
        if (a == 0x44 || a == 0x45) {
            if (cmd16(b, a, 0xF32D) && rd(b, a, d, 3) && crc8(d, 2) == d[2]) return CH_SHT3X;
            uint8_t sn = 0x89;
            if (wr(b, a, &sn, 1)) {
                aos_hal_sleep_ms(2);
                if (rd(b, a, d, 6) && crc8(d, 2) == d[2] && crc8(d + 3, 2) == d[5]) return CH_SHT4X;
            }
            return CAND(CH_SHT3X);
        }
        return CAND(CH_INA219);
    }
    return -1;
}

/* Where the supported chips can be */
static const uint8_t PROBE[] = { 0x23, 0x38, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B,
                                 0x4C, 0x4D, 0x4E, 0x4F, 0x5C, 0x76, 0x77 };

/* -------------------------------------------------------------------------- */
/* Slots                                                                       */
/* -------------------------------------------------------------------------- */

static slot_t *slot_find(const char *port, uint8_t addr)
{
    for (int i = 0; i < AOS_SENSOR_MAX; i++)
        if (s_slot[i].used && s_slot[i].pub.addr == addr && !strcmp(s_slot[i].pub.port, port)) return &s_slot[i];
    return NULL;
}

/* A new slot, called with the lock held */
static slot_t *slot_add(const char *port, uint8_t addr, int chip)
{
    for (int i = 0; i < AOS_SENSOR_MAX; i++) {
        slot_t *s = &s_slot[i];
        if (s->used) continue;
        float *h = s->hist;
        memset(s, 0, sizeof *s);
        s->hist = h ? h : malloc(sizeof(float) * AOS_SENSOR_VALUES * AOS_SENSOR_HIST);
        if (!s->hist) return NULL;
        s->used = true;
        s->chip = chip;
        s->gain_mv = 4096;
        s->shunt_mohm = 100;
        scpy(s->pub.port, sizeof s->pub.port, port);
        s->pub.addr = addr;
        scpy(s->pub.chip, sizeof s->pub.chip, CHIPS[chip].name);
        return s;
    }
    return NULL;
}

static void hist_push(slot_t *s, bool ok)
{
    for (int k = 0; k < AOS_SENSOR_VALUES; k++)
        s->hist[k * AOS_SENSOR_HIST + s->h_pos] = ok && k < s->pub.n ? s->pub.v[k].v : NAN;
    s->h_pos = (s->h_pos + 1) % AOS_SENSOR_HIST;
    if (s->h_n < AOS_SENSOR_HIST) s->h_n++;
}

static void arg_str(const char *args, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    out[0] = 0;
    for (const char *p = args; p && *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t l = strcspn(p, " \t#");
            if (l >= n) l = n - 1;
            memcpy(out, p, l);
            out[l] = 0;
            return;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
}

static bool board_chip(uint8_t a) { return a == 0x14 || a == 0x5D || a == 0x18 || a == 0x40 || a == 0x36; }

/* modules.txt into the table: declared slots, with their settings */
static void sync_declared(void)
{
    lock();
    for (int i = 0; i < AOS_SENSOR_MAX; i++) s_slot[i].declared_now = false;
    for (int m = 0; m < aos_io_module_count(); m++) {
        const aos_io_module_t *mod = aos_io_module_at(m);
        char cs[24];
        arg_str(mod->args, "chip", cs, sizeof cs);
        int chip = chip_index(cs[0] ? cs : mod->name);
        if (chip < 0) continue;
        uint8_t addr = (uint8_t)aos_io_arg_int(mod->args, "addr", CHIPS[chip].addrs[0]);
        slot_t *s = slot_find(mod->port, addr);
        if (s && s->chip != chip && !(s->chip <= CH_BMP280 && chip <= CH_BMP280)) {
            /* the user says it is something else than autodetect thought */
            s->chip = chip;
            s->inited = false;
            s->pub.n = 0;
            scpy(s->pub.chip, sizeof s->pub.chip, CHIPS[chip].name);
        }
        if (!s) s = slot_add(mod->port, addr, chip);
        if (!s) continue;
        s->declared_now = true;
        s->pub.declared = true;
        scpy(s->pub.name, sizeof s->pub.name, mod->name);
        int32_t sh = aos_io_arg_int(mod->args, "shunt", 100), g = aos_io_arg_int(mod->args, "gain", 4096);
        if (sh != s->shunt_mohm || g != s->gain_mv) { s->shunt_mohm = sh; s->gain_mv = g; }
        s->nocrc = !strcmp(mod->name, "aht10") || !strcmp(cs, "aht10");
    }
    /* what is neither declared nor detected any more goes */
    for (int i = 0; i < AOS_SENSOR_MAX; i++) {
        slot_t *s = &s_slot[i];
        if (!s->used) continue;
        if (!s->declared_now) s->pub.declared = false;
        if (!s->declared_now && !s->detected) s->used = false;
    }
    unlock();
}

/* A detected chip's name: the chip id, or id_<addr> when there are two */
static void detected_name(slot_t *s)
{
    const char *id = CHIPS[s->chip].id;
    bool dup = false;
    for (int i = 0; i < AOS_SENSOR_MAX; i++)
        if (s_slot[i].used && &s_slot[i] != s && !strcmp(s_slot[i].pub.name, id)) dup = true;
    if (dup) snprintf(s->pub.name, sizeof s->pub.name, "%s_%02x", id, s->pub.addr);
    else scpy(s->pub.name, sizeof s->pub.name, id);
}

static void detect(aos_io_i2c_t *b, const char *port)
{
    int found[sizeof PROBE];
    for (size_t i = 0; i < sizeof PROBE; i++) {
        found[i] = -1;
        uint8_t a = PROBE[i];
        slot_t *s = slot_find(port, a);
        if (s && s->pub.declared) { found[i] = -100; continue; }      /* the user's: leave it be */
        if (!aos_io_i2c_probe(b, a)) continue;
        found[i] = identify(b, a);
    }
    lock();
    /* candidates of this port are replaced by this pass's */
    int w = 0;
    for (int i = 0; i < s_ncand; i++) if (strcmp(s_cand[i].port, port)) s_cand[w++] = s_cand[i];
    s_ncand = w;
    for (size_t i = 0; i < sizeof PROBE; i++) {
        uint8_t a = PROBE[i];
        slot_t *s = slot_find(port, a);
        if (found[i] == -100) continue;
        if (found[i] >= 0) {
            if (s && s->chip != found[i]) { s->used = false; s = NULL; }
            if (!s) {
                s = slot_add(port, a, found[i]);
                if (s) detected_name(s);
            }
            if (s) s->detected = true;
            continue;
        }
        if (s && !s->pub.declared) s->used = false;                   /* gone */
        if (found[i] <= -2 && s_ncand < AOS_SENSOR_CAND_MAX) {
            aos_sensor_cand_t *c = &s_cand[s_ncand++];
            scpy(c->port, sizeof c->port, port);
            c->addr = a;
            scpy(c->chip, sizeof c->chip, CHIPS[-2 - found[i]].id);
            scpy(c->guess, sizeof c->guess, aos_io_i2c_guess(a, false));
        }
    }
    unlock();
}

/* -------------------------------------------------------------------------- */
/* The thread                                                                  */
/* -------------------------------------------------------------------------- */

static port_st_t *pst_of(const char *port)
{
    for (int i = 0; i < s_npst; i++) if (!strcmp(s_pst[i].port, port)) return &s_pst[i];
    if (s_npst >= AOS_IO_PORT_MAX) return NULL;
    port_st_t *p = &s_pst[s_npst++];
    memset(p, 0, sizeof *p);
    scpy(p->port, sizeof p->port, port);
    return p;
}

static void read_slot(aos_io_i2c_t *b, slot_t *s)
{
    /* a declared chip on the board's bus where the board has one of its own */
    if (!strcmp(s->pub.port, "i2c.board") && board_chip(s->pub.addr)) {
        lock();
        s->pub.state = AOS_SENSOR_ERR;
        const char *g = aos_io_i2c_guess(s->pub.addr, true);
        snprintf(s->pub.err, sizeof s->pub.err, "board:%.*s", (int)strcspn(g, " ("), g);
        s->pub.errors++;
        hist_push(s, false);
        unlock();
        return;
    }
    /* the driver works on a private copy of the values; the table sees them whole */
    slot_t tmp = *s;
    tmp.pub.err[0] = 0;
    tmp.pub.n = 0;
    int r = R_OK;
    if (!tmp.inited) {
        r = drv_init(b, &tmp);
        if (r == R_OK) tmp.inited = true;
    }
    if (r == R_OK) r = drv_read(b, &tmp);
    lock();
    if (!s->used || strcmp(s->pub.port, tmp.pub.port) || s->pub.addr != tmp.pub.addr) { unlock(); return; }
    s->inited = tmp.inited;
    s->init_ms = tmp.init_ms;
    s->cal = tmp.cal;
    s->chip = tmp.chip;
    memcpy(s->pub.chip, tmp.pub.chip, sizeof s->pub.chip);
    s->pub.reads++;
    if (r == R_OK) {
        s->pub.state = AOS_SENSOR_OK;
        s->pub.err[0] = 0;
        s->pub.n = tmp.pub.n;
        memcpy(s->pub.v, tmp.pub.v, sizeof s->pub.v);
        s->pub.t_ms = (uint32_t)aos_hal_uptime_ms();
        hist_push(s, true);
    } else {
        static const char *const WHY[] = { "", "nack", "crc", "id", "notready" };
        s->pub.state = r == R_NOTREADY && s->pub.state != AOS_SENSOR_OK ? AOS_SENSOR_WAIT : AOS_SENSOR_ERR;
        /* err is "<why>" or "<why>:<detail>" (the id the chip gave) */
        if (tmp.pub.err[0]) snprintf(s->pub.err, sizeof s->pub.err, "%s:%.36s", WHY[r], tmp.pub.err);
        else scpy(s->pub.err, sizeof s->pub.err, WHY[r]);
        if (r != R_NOTREADY) s->pub.errors++;
        /* a chip that stopped answering may have lost power: start it again */
        if (r == R_NACK || r == R_ID) s->inited = false;
        hist_push(s, false);
    }
    unlock();
}

static void service(void *arg)
{
    (void)arg;
    uint32_t last_scan[AOS_IO_PORT_MAX];
    char scan_port[AOS_IO_PORT_MAX][AOS_IO_PORT_NAME_MAX];
    memset(scan_port, 0, sizeof scan_port);
    for (;;) {
        uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
        bool rescan = s_rescan;
        s_rescan = false;
        if (rescan) {
            /* every chip is started again and every port probed now; what
             * is not there any more goes in detect(), the rest keeps its
             * history */
            lock();
            for (int i = 0; i < AOS_SENSOR_MAX; i++) s_slot[i].inited = false;
            unlock();
        }
        sync_declared();
        bool viewed = t0 - s_keep_ms < KEEP_MS;
        char ports[AOS_IO_PORT_MAX][AOS_IO_PORT_NAME_MAX];
        int np = 0;
        for (int i = 0; i < aos_io_port_count() && np < AOS_IO_PORT_MAX; i++) {
            const aos_io_port_t *p = aos_io_port_at(i);
            if (p->kind == AOS_PORT_I2C) scpy(ports[np++], AOS_IO_PORT_NAME_MAX, p->name);
        }
        for (int pi = 0; pi < np; pi++) {
            const char *port = ports[pi];
            bool board = !strcmp(port, "i2c.board");
            /* when is this port's next autodetect */
            int si = -1;
            for (int k = 0; k < AOS_IO_PORT_MAX; k++) if (!strcmp(scan_port[k], port)) { si = k; break; }
            if (si < 0) for (int k = 0; k < AOS_IO_PORT_MAX; k++) if (!scan_port[k][0]) {
                si = k;
                scpy(scan_port[k], sizeof scan_port[k], port);
                last_scan[k] = t0 - SCAN_SLOW;
                break;
            }
            bool scan = !board && si >= 0 && (rescan || t0 - last_scan[si] >= (viewed ? SCAN_FAST : SCAN_SLOW));
            int nslots = 0;
            lock();
            for (int i = 0; i < AOS_SENSOR_MAX; i++) if (s_slot[i].used && !strcmp(s_slot[i].pub.port, port)) nslots++;
            unlock();
            if (!scan && !nslots) continue;

            /* pins somebody else holds (the Bus app driving GPIO21, say):
             * skip the round quietly instead of having the arbiter refuse
             * and log it every second */
            const aos_io_port_t *pp = aos_io_port_find(port);
            bool held = false;
            for (int k = 0; pp && !board && k < 2; k++) {
                const char *o = aos_io_owner(pp->pins[k]);
                if (o && strcmp(o, OWNER)) held = true;
            }
            aos_io_i2c_t *b = held ? NULL : aos_io_i2c_open(port, OWNER);
            if (!b && !held) {
                aos_hal_sleep_ms(40);                  /* the Bus app in the middle of a job: once more */
                b = aos_io_i2c_open(port, OWNER);
            }
            port_st_t *ps;
            if (!b) {
                const aos_io_port_t *p = aos_io_port_find(port);
                const char *o = p ? aos_io_owner(p->pins[0]) : NULL;
                if (!o && p) o = aos_io_owner(p->pins[1]);
                lock();
                ps = pst_of(port);
                if (ps) {
                    ps->state = o ? AOS_SENSOR_BUSY : AOS_SENSOR_ERR;
                    scpy(ps->owner, sizeof ps->owner, o ? o : "");
                }
                for (int i = 0; i < AOS_SENSOR_MAX; i++) {
                    slot_t *s = &s_slot[i];
                    if (!s->used || strcmp(s->pub.port, port)) continue;
                    s->pub.state = o ? AOS_SENSOR_BUSY : AOS_SENSOR_ERR;
                    snprintf(s->pub.err, sizeof s->pub.err, o ? "busy:%s" : "port", o ? o : "");
                    hist_push(s, false);
                }
                unlock();
                continue;
            }
            if (scan) {
                detect(b, port);
                last_scan[si] = (uint32_t)aos_hal_uptime_ms();
            }
            for (int i = 0; i < AOS_SENSOR_MAX; i++) {
                lock();
                bool mine = s_slot[i].used && !strcmp(s_slot[i].pub.port, port);
                unlock();
                if (mine) read_slot(b, &s_slot[i]);
            }
            aos_io_i2c_close(b);
            lock();
            ps = pst_of(port);
            if (ps) {
                ps->state = AOS_SENSOR_OK;
                ps->owner[0] = 0;
                ps->found = 0;
                for (int i = 0; i < AOS_SENSOR_MAX; i++) if (s_slot[i].used && !strcmp(s_slot[i].pub.port, port)) ps->found++;
                if (scan) { ps->scan_ms = (uint32_t)aos_hal_uptime_ms(); ps->scanned = true; }
            }
            unlock();
        }
        s_seq++;
        uint32_t spent = (uint32_t)aos_hal_uptime_ms() - t0;
        aos_hal_sleep_ms(spent < ROUND_MS - 200 ? ROUND_MS - spent : 200);
    }
}

/* -------------------------------------------------------------------------- */
/* The API                                                                     */
/* -------------------------------------------------------------------------- */

void aos_sensors_start(void)
{
    /* the first caller creates the lock and the thread; one that comes at
     * the same moment waits for the lock to exist */
    if (__atomic_exchange_n(&s_started, 1, __ATOMIC_ACQ_REL)) {
        while (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) aos_hal_sleep_ms(1);
        return;
    }
    s_mx = aos_hal_mutex_create();
    s_keep_ms = (uint32_t)aos_hal_uptime_ms();
    __atomic_store_n(&s_ready, 1, __ATOMIC_RELEASE);
    if (!aos_hal_thread_start("sensors", service, NULL, 6144, 3)) aos_hal_log("sensors", "no thread");
}

void aos_sensors_autostart(void)
{
    static bool tried;
    if (tried || __atomic_load_n(&s_started, __ATOMIC_ACQUIRE)) return;
    tried = true;
    for (int m = 0; m < aos_io_module_count(); m++) {
        const aos_io_module_t *mod = aos_io_module_at(m);
        char cs[24];
        arg_str(mod->args, "chip", cs, sizeof cs);
        if (chip_index(cs[0] ? cs : mod->name) >= 0) { aos_sensors_start(); return; }
    }
}

void aos_sensors_keep(void) { aos_sensors_start(); s_keep_ms = (uint32_t)aos_hal_uptime_ms(); }
void aos_sensors_rescan(void) { aos_sensors_start(); s_rescan = true; }
uint32_t aos_sensors_seq(void) { return s_seq; }

int aos_sensors_ports(aos_sensor_port_t *out, int max)
{
    aos_sensors_start();
    lock();
    int n = s_npst < max ? s_npst : max;
    memcpy(out, s_pst, sizeof *out * (size_t)n);
    unlock();
    return n;
}

/* Visible slots in a stable order: declared first, then detected. */
static int visible(int *idx)
{
    int n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < AOS_SENSOR_MAX; i++)
            if (s_slot[i].used && s_slot[i].pub.declared == (pass == 0)) idx[n++] = i;
    return n;
}

int aos_sensor_count(void)
{
    aos_sensors_start();
    int idx[AOS_SENSOR_MAX];
    lock();
    int n = visible(idx);
    unlock();
    return n;
}

bool aos_sensor_at(int i, aos_sensor_t *out)
{
    aos_sensors_start();
    int idx[AOS_SENSOR_MAX];
    lock();
    int n = visible(idx);
    bool ok = i >= 0 && i < n;
    if (ok) *out = s_slot[idx[i]].pub;
    unlock();
    return ok;
}

bool aos_sensor_value(const char *name, const char *key, float *v)
{
    aos_sensors_start();
    bool ok = false;
    lock();
    for (int i = 0; i < AOS_SENSOR_MAX && !ok; i++) {
        const slot_t *s = &s_slot[i];
        if (!s->used || s->pub.state != AOS_SENSOR_OK || strcmp(s->pub.name, name)) continue;
        for (int k = 0; k < s->pub.n; k++)
            if (!strcmp(s->pub.v[k].key, key)) { *v = s->pub.v[k].v; ok = true; break; }
    }
    unlock();
    return ok;
}

int aos_sensor_history(int i, int k, float *out, int max)
{
    aos_sensors_start();
    int idx[AOS_SENSOR_MAX], got = 0;
    if (k < 0 || k >= AOS_SENSOR_VALUES) return 0;
    lock();
    int n = visible(idx);
    if (i >= 0 && i < n) {
        const slot_t *s = &s_slot[idx[i]];
        int cnt = s->h_n < max ? s->h_n : max;
        int start = (s->h_pos - cnt + AOS_SENSOR_HIST) % AOS_SENSOR_HIST;
        for (int j = 0; j < cnt; j++) out[j] = s->hist[k * AOS_SENSOR_HIST + (start + j) % AOS_SENSOR_HIST];
        got = cnt;
    }
    unlock();
    return got;
}

int aos_sensor_candidates(aos_sensor_cand_t *out, int max)
{
    aos_sensors_start();
    lock();
    int n = s_ncand < max ? s_ncand : max;
    memcpy(out, s_cand, sizeof *out * (size_t)n);
    unlock();
    return n;
}
