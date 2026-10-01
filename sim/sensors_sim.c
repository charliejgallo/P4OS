/*
 * P4OS simulator - I2C sensors that answer on the header's bus, so the
 * Módulos app, the portal's Expansión page and the drivers in
 * components/aos_io/aos_sensors.c run against something real enough.
 *
 * Each device speaks its chip's actual protocol - registers with a pointer,
 * 16-bit commands with CRCs, measurement times, power-on values - and the
 * readings drift slowly the way a bench does: the room warms up and cools
 * down, a board on the INA219 draws bursts every few seconds, a pot on the
 * ADS1115 gets turned. The BME280 has a calibration block and its raw ADC
 * values come from inverting Bosch's floating-point formulas (datasheet
 * 8.1), so the driver's integer compensation (datasheet 8.2) is checked
 * against an independent implementation, not against itself.
 *
 * P4_SIM_SENSORS
 *   unset or "1"   the bench set: bme280@0x76 sht31@0x44 bh1750@0x23
 *                  ina219@0x40 ads1115@0x48
 *   "0" / "off"    none (the plain register blobs of io_sim.c, as before)
 *   a list         "bmp280@0x77,sht4x,aht20,ina226@0x41": any of bme280,
 *                  bmp280, sht31 (sht3x), sht4x, aht20, bh1750, ina219,
 *                  ina226, ads1115; the address defaults to the usual one.
 *
 * They answer on every I2C port except i2c.board (whose chips io_sim.c
 * plays already). Thread-safe: the sensor service and the Bus app use the
 * bus from different threads.
 */
#include "aos_hal.h"

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { D_BME280, D_BMP280, D_SHT3X, D_SHT4X, D_AHT20, D_BH1750, D_INA219, D_INA226, D_ADS1115, D_N };

static const struct { const char *name; uint8_t addr; } KIND[D_N] = {
    [D_BME280] = { "bme280", 0x76 }, [D_BMP280] = { "bmp280", 0x76 }, [D_SHT3X] = { "sht31", 0x44 },
    [D_SHT4X] = { "sht4x", 0x44 },   [D_AHT20] = { "aht20", 0x38 },   [D_BH1750] = { "bh1750", 0x23 },
    [D_INA219] = { "ina219", 0x40 }, [D_INA226] = { "ina226", 0x40 }, [D_ADS1115] = { "ads1115", 0x48 },
};

typedef struct {
    int      kind;
    uint8_t  addr;
    /* BME280: the register file */
    uint8_t  regs[256];
    /* 16-bit register chips: pointer and registers */
    uint8_t  ptr;
    uint16_t r16[256];
    /* command chips: the answer waiting to be read, and from when */
    uint8_t  resp[8];
    int      resp_n;
    uint64_t resp_ms;
    /* BH1750 */
    bool     on;
    uint8_t  mode;
    uint64_t mode_ms;
    /* ADS1115: when the conversion in progress ends */
    uint64_t conv_end;
    /* AHT20 */
    bool     cal;
} emu_t;

#define MAX_DEV 8
static emu_t s_dev[MAX_DEV];
static int s_ndev = -1;
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------- */
/* The bench: what the sensors measure, as a function of time                  */
/* -------------------------------------------------------------------------- */

/* P4_SIM_SENSORS_FIXED=1: no drift and no noise (the room at 25 °C, 50 %,
 * 1006,53 hPa), for checking a driver's arithmetic to the last digit */
static int s_fixed = -1;
static double now_s(void)
{
    if (s_fixed < 0) { const char *f = getenv("P4_SIM_SENSORS_FIXED"); s_fixed = f && *f == '1'; }
    return s_fixed ? 0.0 : (double)aos_hal_uptime_ms() / 1000.0;
}

static double noise(double amp)
{
    static uint32_t x = 0x1234567;
    if (s_fixed > 0) return 0.0;
    x = x * 1664525u + 1013904223u;
    return ((double)(x >> 8) / 16777216.0 - 0.5) * 2.0 * amp;
}

#define TAU 6.283185307179586
static double room_t(double t) { return s_fixed > 0 ? 25.0 : 22.8 + 1.6 * sin(TAU * t / 180.0) + noise(0.03); }
static double room_h(double t) { return s_fixed > 0 ? 50.0 : 47.0 + 5.0 * sin(TAU * t / 240.0 + 1.0) + noise(0.2); }
static double room_p(double t) { return s_fixed > 0 ? 1006.53 : 1012.6 + 0.8 * sin(TAU * t / 600.0) + noise(0.02); }
static double light(double t)
{
    double l = 420.0 + 180.0 * sin(TAU * t / 60.0) + noise(3.0);
    if (fmod(t, 45.0) < 3.0) l *= 0.08;           /* a hand over it, now and then */
    return l < 0 ? 0 : l;
}
/* a board on the bench: 120 mA idle, radio bursts to ~300 mA every 8 s */
static double load_i(double t)
{
    double s = sin(TAU * t / 8.0);
    return 0.120 + 0.180 * (s > 0 ? s * s * s * s : 0) + noise(0.002);
}
static double load_v(double t) { return 5.04 - 0.35 * load_i(t) + noise(0.003); }
static double ads_in(int ch, double t)
{
    switch (ch) {
    case 0: return 3.298 + noise(0.002);                         /* the 3V3 rail */
    case 1: return 1.65 + 1.40 * sin(TAU * t / 30.0);            /* a pot being turned */
    case 2: return 1.20 + 0.08 * sin(TAU * t / 90.0) + noise(0.001);   /* an NTC divider */
    default: return noise(0.0005);                               /* tied to GND */
    }
}

/* -------------------------------------------------------------------------- */
/* BME280                                                                      */
/* -------------------------------------------------------------------------- */

/* Bosch's datasheet example for T and P, typical values for H */
static const uint16_t T1 = 27504; static const int16_t T2 = 26435, T3 = -1000;
static const uint16_t P1 = 36477; static const int16_t P2 = -10685, P3 = 3024, P4 = 2855, P5 = 140, P6 = -7,
                                                        P7 = 15500, P8 = -14600, P9 = 6000;
static const uint8_t H1 = 75, H3 = 0; static const int16_t H2 = 362, H4 = 313, H5 = 50; static const int8_t H6 = 30;

/* datasheet 8.1, the floating-point compensation */
static double f_tfine(double adc_T)
{
    double v1 = (adc_T / 16384.0 - T1 / 1024.0) * T2;
    double v2 = (adc_T / 131072.0 - T1 / 8192.0) * (adc_T / 131072.0 - T1 / 8192.0) * T3;
    return v1 + v2;
}
static double f_press(double adc_P, double t_fine)
{
    double v1 = t_fine / 2.0 - 64000.0;
    double v2 = v1 * v1 * P6 / 32768.0;
    v2 = v2 + v1 * P5 * 2.0;
    v2 = v2 / 4.0 + P4 * 65536.0;
    v1 = (P3 * v1 * v1 / 524288.0 + P2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * P1;
    double p = 1048576.0 - adc_P;
    p = (p - v2 / 4096.0) * 6250.0 / v1;
    v1 = P9 * p * p / 2147483648.0;
    v2 = p * P8 / 32768.0;
    return p + (v1 + v2 + P7) / 16.0;             /* Pa */
}
static double f_hum(double adc_H, double t_fine)
{
    double h = t_fine - 76800.0;
    h = (adc_H - (H4 * 64.0 + H5 / 16384.0 * h)) * (H2 / 65536.0 * (1.0 + H6 / 67108864.0 * h * (1.0 + H3 / 67108864.0 * h)));
    return h * (1.0 - H1 * h / 524288.0);
}

/* the raw values that come out as the room's: bisection on the formulas */
static void bme_raw(double tc, double hpa, double rh, uint32_t *aT, uint32_t *aP, uint32_t *aH)
{
    double lo = 0, hi = 1048575;
    for (int i = 0; i < 40; i++) { double m = (lo + hi) / 2; if (f_tfine(m) / 5120.0 < tc) lo = m; else hi = m; }
    *aT = (uint32_t)lrint(lo);
    double tf = f_tfine(*aT);
    lo = 0; hi = 1048575;                          /* pressure falls as adc_P grows */
    for (int i = 0; i < 40; i++) { double m = (lo + hi) / 2; if (f_press(m, tf) > hpa * 100.0) lo = m; else hi = m; }
    *aP = (uint32_t)lrint(lo);
    lo = 0; hi = 65535;
    for (int i = 0; i < 40; i++) { double m = (lo + hi) / 2; if (f_hum(m, tf) < rh) lo = m; else hi = m; }
    *aH = (uint32_t)lrint(lo);
}

static void bme_reset(emu_t *d)
{
    uint8_t *m = d->regs;
    memset(m, 0, 256);
    m[0xD0] = d->kind == D_BME280 ? 0x60 : 0x58;
    const uint16_t tp[12] = { T1, (uint16_t)T2, (uint16_t)T3, P1, (uint16_t)P2, (uint16_t)P3, (uint16_t)P4,
                              (uint16_t)P5, (uint16_t)P6, (uint16_t)P7, (uint16_t)P8, (uint16_t)P9 };
    for (int i = 0; i < 12; i++) { m[0x88 + 2 * i] = (uint8_t)tp[i]; m[0x89 + 2 * i] = (uint8_t)(tp[i] >> 8); }
    m[0xA0] = 0x00;
    if (d->kind == D_BME280) {
        m[0xA1] = H1;
        m[0xE1] = (uint8_t)H2; m[0xE2] = (uint8_t)(H2 >> 8);
        m[0xE3] = H3;
        m[0xE4] = (uint8_t)(H4 >> 4);
        m[0xE5] = (uint8_t)((H4 & 0x0F) | (H5 & 0x0F) << 4);
        m[0xE6] = (uint8_t)(H5 >> 4);
        m[0xE7] = (uint8_t)H6;
    }
    /* the data registers' reset value: "no measurement" */
    m[0xF7] = 0x80; m[0xFA] = 0x80; m[0xFD] = 0x80;
}

static void bme_measure(emu_t *d)
{
    uint8_t *m = d->regs;
    uint8_t ctrl = m[0xF4];
    if ((ctrl & 0x03) == 0) return;                /* sleep: the registers keep the last one */
    double t = now_s();
    uint32_t aT, aP, aH;
    bme_raw(room_t(t), room_p(t), room_h(t), &aT, &aP, &aH);
    if (!(ctrl >> 5)) aT = 0x80000;                /* oversampling 0: skipped */
    if (!((ctrl >> 2) & 7)) aP = 0x80000;
    if (!(m[0xF2] & 7) || d->kind == D_BMP280) aH = 0x8000;
    m[0xF7] = (uint8_t)(aP >> 12); m[0xF8] = (uint8_t)(aP >> 4); m[0xF9] = (uint8_t)(aP << 4);
    m[0xFA] = (uint8_t)(aT >> 12); m[0xFB] = (uint8_t)(aT >> 4); m[0xFC] = (uint8_t)(aT << 4);
    if (d->kind == D_BME280) { m[0xFD] = (uint8_t)(aH >> 8); m[0xFE] = (uint8_t)aH; }
    if ((ctrl & 0x03) != 0x03) m[0xF4] = ctrl & 0xFC;   /* forced: one, then back to sleep */
}

static bool bme_xfer(emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    /* writes are (register, value) pairs; no auto-increment when writing */
    for (size_t i = 0; i + 1 < wn; i += 2) {
        uint8_t reg = w[i], v = w[i + 1];
        if (reg == 0xE0) { if (v == 0xB6) bme_reset(d); }
        else if (reg == 0xF2 || reg == 0xF4 || reg == 0xF5) d->regs[reg] = v;
    }
    if (wn == 1) d->ptr = w[0];                    /* the register pointer */
    if (!rn) return true;
    if (wn >= 2) return false;                     /* a read after a data write: not how it is used */
    uint8_t ptr = d->ptr;
    if (ptr <= 0xFE && ptr + rn > 0xF7) bme_measure(d);
    for (size_t i = 0; i < rn; i++) r[i] = d->regs[(uint8_t)(ptr + i)];
    return true;
}

/* -------------------------------------------------------------------------- */
/* Sensirion SHT3x / SHT4x, Aosong AHT20                                       */
/* -------------------------------------------------------------------------- */

static uint8_t crc8(const uint8_t *p, int n)
{
    uint8_t c = 0xFF;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x31 : c << 1);
    }
    return c;
}

static void put_word(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; p[2] = crc8(p, 2); }

static void sht_measure(emu_t *d, bool sht4, uint32_t delay_ms)
{
    double t = now_s();
    double tc = room_t(t) + 0.35, rh = room_h(t) + 1.2;     /* two sensors never agree */
    double rt = (tc + 45.0) / 175.0 * 65535.0;
    double rr = sht4 ? (rh + 6.0) / 125.0 * 65535.0 : rh / 100.0 * 65535.0;
    put_word(d->resp, (uint16_t)lrint(rt));
    put_word(d->resp + 3, (uint16_t)lrint(rr));
    d->resp_n = 6;
    d->resp_ms = aos_hal_uptime_ms() + delay_ms;
}

static bool sht_read(emu_t *d, uint8_t *r, size_t rn)
{
    if (!d->resp_n || aos_hal_uptime_ms() < d->resp_ms) return false;   /* NACK: nothing to give yet */
    for (size_t i = 0; i < rn; i++) r[i] = i < (size_t)d->resp_n ? d->resp[i] : 0xFF;
    d->resp_n = 0;
    return true;
}

static bool sht3_xfer(emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn == 1) return false;                     /* half a command */
    if (wn >= 2) {
        uint16_t c = (uint16_t)(w[0] << 8 | w[1]);
        switch (c) {
        case 0x2400: case 0x240B: case 0x2416: sht_measure(d, false, 15); break;    /* no clock stretching */
        case 0x2C06: case 0x2C0D: case 0x2C10: sht_measure(d, false, 0); break;     /* stretching: ready at the read */
        case 0xF32D: put_word(d->resp, 0x0000); d->resp_n = 3; d->resp_ms = 0; break;
        case 0x3780: put_word(d->resp, 0x1A2B); put_word(d->resp + 3, 0x3C4D); d->resp_n = 6; d->resp_ms = 0; break;
        case 0x30A2: case 0x3041: d->resp_n = 0; break;
        default: return false;
        }
    }
    return rn ? sht_read(d, r, rn) : true;
}

static bool sht4_xfer(emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn > 1) return false;
    if (wn == 1) {
        switch (w[0]) {
        case 0xFD: sht_measure(d, true, 9); break;
        case 0xF6: sht_measure(d, true, 5); break;
        case 0xE0: sht_measure(d, true, 2); break;
        case 0x89: case 0x8F: put_word(d->resp, 0x0C4E); put_word(d->resp + 3, 0x7F12); d->resp_n = 6; d->resp_ms = 0; break;
        case 0x94: d->resp_n = 0; break;
        default: return false;
        }
    }
    return rn ? sht_read(d, r, rn) : true;
}

static bool aht_xfer(emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    uint64_t now = aos_hal_uptime_ms();
    if (wn >= 3 && w[0] == 0xAC) {
        double t = now_s();
        uint32_t hr = (uint32_t)lrint((room_h(t) + 0.8) / 100.0 * 1048576.0);
        uint32_t tr = (uint32_t)lrint((room_t(t) - 0.2 + 50.0) / 200.0 * 1048576.0);
        if (hr > 0xFFFFF) hr = 0xFFFFF;
        d->resp[1] = (uint8_t)(hr >> 12);
        d->resp[2] = (uint8_t)(hr >> 4);
        d->resp[3] = (uint8_t)((hr & 0x0F) << 4 | (tr >> 16 & 0x0F));
        d->resp[4] = (uint8_t)(tr >> 8);
        d->resp[5] = (uint8_t)tr;
        d->resp_ms = now + 80;
        d->resp_n = 7;
    } else if (wn >= 1 && (w[0] == 0xBE || w[0] == 0xE1)) {
        d->cal = true;
    } else if (wn >= 1 && w[0] == 0xBA) {
        d->resp_n = 0;
    }
    if (!rn) return true;
    bool busy = d->resp_n && now < d->resp_ms;
    d->resp[0] = (uint8_t)((busy ? 0x80 : 0) | 0x10 | (d->cal ? 0x08 : 0));
    d->resp[6] = crc8(d->resp, 6);
    for (size_t i = 0; i < rn; i++) r[i] = i < 7 ? d->resp[i] : 0xFF;
    return true;
}

/* -------------------------------------------------------------------------- */
/* BH1750                                                                      */
/* -------------------------------------------------------------------------- */

static bool bh_xfer(emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    for (size_t i = 0; i < wn; i++) {
        uint8_t op = w[i];
        if (op == 0x00) d->on = false;
        else if (op == 0x01) d->on = true;
        else if (op == 0x07) { d->mode_ms = 0; }
        else if (op == 0x10 || op == 0x11 || op == 0x13 || op == 0x20 || op == 0x21 || op == 0x23) {
            d->on = true;
            d->mode = op;
            d->mode_ms = aos_hal_uptime_ms();
        }
    }
    if (!rn) return true;
    uint16_t raw = 0;
    uint32_t need = (d->mode & 0x03) == 0x03 ? 16 : 120;
    if (d->on && d->mode && aos_hal_uptime_ms() - d->mode_ms >= need) {
        double lx = light(now_s()) * 1.2 * ((d->mode & 0x03) == 0x01 ? 2.0 : 1.0);
        raw = (uint16_t)(lx > 65535 ? 65535 : lrint(lx));
        if ((d->mode & 0x03) == 0x03) raw &= 0xFFFC;              /* 4 lx steps */
        if (d->mode & 0x20) d->on = false;                        /* one-time: powers down */
    }
    r[0] = (uint8_t)(raw >> 8);
    if (rn > 1) r[1] = (uint8_t)raw;
    for (size_t i = 2; i < rn; i++) r[i] = 0xFF;
    return true;
}

/* -------------------------------------------------------------------------- */
/* INA219 / INA226, ADS1115: 16-bit registers behind a pointer                 */
/* -------------------------------------------------------------------------- */

#define SHUNT_OHM 0.1

static void ina_defaults(emu_t *d)
{
    memset(d->r16, 0, sizeof d->r16);
    if (d->kind == D_INA219) d->r16[0] = 0x399F;
    else { d->r16[0] = 0x4127; d->r16[0xFE] = 0x5449; d->r16[0xFF] = 0x2260; }
}

static void ina_update(emu_t *d)
{
    double t = now_s(), i = load_i(t), v = load_v(t), vsh = i * SHUNT_OHM;
    int32_t sh, bus;
    if (d->kind == D_INA219) {
        static const double RANGE[4] = { 0.04, 0.08, 0.16, 0.32 };
        double fs = RANGE[(d->r16[0] >> 11) & 3];
        if (vsh > fs) vsh = fs;
        sh = (int32_t)lrint(vsh / 10e-6);
        bus = (int32_t)lrint(v / 0.004) << 3 | 0x02;               /* CNVR */
        d->r16[1] = (uint16_t)(int16_t)sh;
        d->r16[2] = (uint16_t)bus;
        int32_t cur = (int32_t)((int64_t)sh * d->r16[5] / 4096);
        d->r16[4] = (uint16_t)(int16_t)cur;
        d->r16[3] = (uint16_t)((int64_t)cur * (bus >> 3) / 5000);
    } else {
        sh = (int32_t)lrint(vsh / 2.5e-6);
        bus = (int32_t)lrint(v / 0.00125);
        d->r16[1] = (uint16_t)(int16_t)sh;
        d->r16[2] = (uint16_t)bus;
        int32_t cur = (int32_t)((int64_t)sh * d->r16[5] / 2048);
        d->r16[4] = (uint16_t)(int16_t)cur;
        d->r16[3] = (uint16_t)((int64_t)cur * bus / 20000);
    }
}

static void ads_defaults(emu_t *d)
{
    memset(d->r16, 0, sizeof d->r16);
    d->r16[1] = 0x8583;
    d->r16[2] = 0x8000;
    d->r16[3] = 0x7FFF;
}

static void ads_convert(emu_t *d)
{
    static const double FSR[8] = { 6.144, 4.096, 2.048, 1.024, 0.512, 0.256, 0.256, 0.256 };
    static const int SPS[8] = { 8, 16, 32, 64, 128, 250, 475, 860 };
    uint16_t c = d->r16[1];
    int mux = (c >> 12) & 7;
    double t = now_s(), v;
    switch (mux) {
    case 0: v = ads_in(0, t) - ads_in(1, t); break;
    case 1: v = ads_in(0, t) - ads_in(3, t); break;
    case 2: v = ads_in(1, t) - ads_in(3, t); break;
    case 3: v = ads_in(2, t) - ads_in(3, t); break;
    default: v = ads_in(mux - 4, t); break;
    }
    double fs = FSR[(c >> 9) & 7];
    long raw = lrint(v / fs * 32768.0);
    if (raw > 32767) raw = 32767;
    if (raw < -32768) raw = -32768;
    d->r16[0] = (uint16_t)(int16_t)raw;
    d->conv_end = aos_hal_uptime_ms() + (uint64_t)(1000 / SPS[(c >> 5) & 7]) + 1;
}

static bool reg16_xfer(emu_t *d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    if (wn >= 1) d->ptr = w[0];
    if (wn >= 3) {
        uint16_t v = (uint16_t)(w[1] << 8 | w[2]);
        if (d->kind == D_ADS1115) {
            if (d->ptr == 1) {
                d->r16[1] = v & 0x7FFF;
                if (v & 0x8000 || !(v & 0x0100)) ads_convert(d);    /* one shot, or continuous */
            } else if (d->ptr == 2 || d->ptr == 3) d->r16[d->ptr] = v;
        } else {
            if (d->ptr == 0) { if (v & 0x8000) ina_defaults(d); else d->r16[0] = v; }
            else if (d->ptr == 5) d->r16[5] = v & 0xFFFE;
        }
    }
    if (!rn) return true;
    uint16_t v;
    if (d->kind == D_ADS1115) {
        uint64_t now = aos_hal_uptime_ms();
        if (!(d->r16[1] & 0x0100) && now >= d->conv_end) ads_convert(d);  /* continuous */
        v = d->r16[d->ptr & 3];
        if ((d->ptr & 3) == 1) v = (uint16_t)(v | (now >= d->conv_end ? 0x8000 : 0));   /* OS: 1 = idle */
    } else {
        ina_update(d);
        v = d->r16[d->ptr];
    }
    for (size_t i = 0; i < rn; i++) r[i] = i & 1 ? (uint8_t)v : (uint8_t)(v >> 8);
    return true;
}

/* -------------------------------------------------------------------------- */
/* The set, from P4_SIM_SENSORS                                                */
/* -------------------------------------------------------------------------- */

static void add_dev(int kind, int addr)
{
    if (s_ndev >= MAX_DEV) return;
    for (int i = 0; i < s_ndev; i++) if (s_dev[i].addr == addr) return;   /* one per address */
    emu_t *d = &s_dev[s_ndev++];
    memset(d, 0, sizeof *d);
    d->kind = kind;
    d->addr = (uint8_t)addr;
    d->cal = true;
    if (kind == D_BME280 || kind == D_BMP280) bme_reset(d);
    else if (kind == D_INA219 || kind == D_INA226) ina_defaults(d);
    else if (kind == D_ADS1115) ads_defaults(d);
}

static void setup(void)
{
    s_ndev = 0;
    const char *e = getenv("P4_SIM_SENSORS");
    if (e && (!strcmp(e, "0") || !strcmp(e, "off") || !strcmp(e, "none"))) return;
    if (!e || !*e || !strcmp(e, "1")) e = "bme280@0x76,sht31@0x44,bh1750@0x23,ina219@0x40,ads1115@0x48";
    char buf[256];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) {
        char *at = strchr(tok, '@');
        if (at) *at++ = 0;
        int kind = -1;
        for (int k = 0; k < D_N; k++) if (!strcmp(tok, KIND[k].name)) kind = k;
        if (!strcmp(tok, "sht3x") || !strcmp(tok, "sht30")) kind = D_SHT3X;
        if (!strcmp(tok, "sht40") || !strcmp(tok, "sht41")) kind = D_SHT4X;
        if (kind < 0) { aos_hal_log("sim", "P4_SIM_SENSORS: no such sensor '%s'", tok); continue; }
        add_dev(kind, at ? (int)strtol(at, NULL, 0) : KIND[kind].addr);
    }
    char list[160] = "";
    for (int i = 0; i < s_ndev; i++) {
        size_t o = strlen(list);
        snprintf(list + o, sizeof list - o, "%s%s@0x%02X", i ? " " : "", KIND[s_dev[i].kind].name, s_dev[i].addr);
    }
    aos_hal_log("sim", "emulated I2C sensors: %s", s_ndev ? list : "none");
}

static emu_t *find(uint8_t addr)
{
    if (s_ndev < 0) setup();
    for (int i = 0; i < s_ndev; i++) if (s_dev[i].addr == addr) return &s_dev[i];
    return NULL;
}

bool sim_sensors_on(void)
{
    pthread_mutex_lock(&s_mx);
    if (s_ndev < 0) setup();
    bool on = s_ndev > 0;
    pthread_mutex_unlock(&s_mx);
    return on;
}

bool sim_sensors_has(uint8_t addr)
{
    pthread_mutex_lock(&s_mx);
    bool ok = find(addr) != NULL;
    pthread_mutex_unlock(&s_mx);
    return ok;
}

bool sim_sensors_xfer(uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    pthread_mutex_lock(&s_mx);
    emu_t *d = find(addr);
    bool ok = false;
    if (d) {
        switch (d->kind) {
        case D_BME280: case D_BMP280: ok = bme_xfer(d, w, wn, r, rn); break;
        case D_SHT3X: ok = sht3_xfer(d, w, wn, r, rn); break;
        case D_SHT4X: ok = sht4_xfer(d, w, wn, r, rn); break;
        case D_AHT20: ok = aht_xfer(d, w, wn, r, rn); break;
        case D_BH1750: ok = bh_xfer(d, w, wn, r, rn); break;
        default: ok = reg16_xfer(d, w, wn, r, rn); break;
        }
    }
    pthread_mutex_unlock(&s_mx);
    return ok;
}
