/*
 * P4OS - 1-Wire and addressable LED strips on any usable GPIO of the header
 * (aos_io.h): the part the board and the simulator share. The backends
 * (aos_io_p4.c, sim/io_sim.c) only move bits.
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool header_pin(int gpio)
{
    const aos_io_pin_t *p = aos_io_pin_of_gpio(gpio);
    return p && !(p->flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD));
}

/* ---- 1-Wire ---- */

aos_io_ow_t *aos_io_ow_open(int gpio, bool pullup, const char *owner)
{
    if (!owner || !owner[0]) return NULL;
    if (!header_pin(gpio)) { aos_hal_log("io", "1-Wire: GPIO%d is not a usable pin of the header", gpio); return NULL; }
    if (!aos_io_claim(gpio, owner)) {
        aos_hal_log("io", "1-Wire: GPIO%d is %s's", gpio, aos_io_owner(gpio) ? aos_io_owner(gpio) : "taken");
        return NULL;
    }
    aos_io_ow_t *b = calloc(1, sizeof *b);
    if (!b) { aos_io_release(gpio, owner); return NULL; }
    b->gpio = (int8_t)gpio;
    b->pullup = pullup;
    snprintf(b->owner, sizeof b->owner, "%s", owner);
    if (!aos_io_be_ow_open(b)) {
        aos_io_release(gpio, owner);
        free(b);
        return NULL;
    }
    return b;
}

bool aos_io_ow_reset(aos_io_ow_t *b) { return b && aos_io_be_ow_reset(b); }
int  aos_io_ow_search(aos_io_ow_t *b, uint64_t *roms, int max) { return b && roms && max > 0 ? aos_io_be_ow_search(b, roms, max) : -1; }
bool aos_io_ow_write(aos_io_ow_t *b, const void *data, size_t n) { return b && (!n || aos_io_be_ow_write(b, data, n)); }
bool aos_io_ow_read(aos_io_ow_t *b, void *data, size_t n) { return b && (!n || aos_io_be_ow_read(b, data, n)); }
int  aos_io_ow_gpio(aos_io_ow_t *b) { return b ? b->gpio : -1; }

void aos_io_ow_close(aos_io_ow_t *b)
{
    if (!b) return;
    aos_io_be_ow_close(b);
    aos_io_release(b->gpio, b->owner);
    free(b);
}

uint8_t aos_io_ow_crc8(const void *data, size_t n)
{
    const uint8_t *d = data;
    uint8_t crc = 0;
    while (n--) {
        uint8_t in = *d++;
        for (int i = 0; i < 8; i++) {
            uint8_t mix = (crc ^ in) & 1;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            in >>= 1;
        }
    }
    return crc;
}

const char *aos_io_ow_family(uint8_t code)
{
    switch (code) {
    case 0x28: return "DS18B20";
    case 0x10: return "DS18S20";
    case 0x22: return "DS1822";
    case 0x3B: return "MAX31850";
    case 0x42: return "DS28EA00";
    case 0x26: return "DS2438";
    case 0x01: return "DS2401";
    case 0x2D: return "DS2431";
    case 0x29: return "DS2408";
    case 0x3A: return "DS2413";
    default:   return NULL;
    }
}

static bool ow_select(aos_io_ow_t *b, uint64_t rom)
{
    if (!aos_io_ow_reset(b)) return false;
    uint8_t cmd[9] = { 0x55 };                  /* MATCH ROM */
    for (int i = 0; i < 8; i++) cmd[1 + i] = (uint8_t)(rom >> (8 * i));
    return aos_io_ow_write(b, cmd, sizeof cmd);
}

static bool ds_scratchpad(aos_io_ow_t *b, uint64_t rom, uint8_t sp[9])
{
    static const uint8_t RD = 0xBE;
    if (!ow_select(b, rom) || !aos_io_ow_write(b, &RD, 1) || !aos_io_ow_read(b, sp, 9)) return false;
    bool all_ff = true;
    for (int i = 0; i < 9; i++) all_ff &= sp[i] == 0xFF;
    return !all_ff && aos_io_ow_crc8(sp, 8) == sp[8];
}

int aos_io_ds18b20_convert_all(aos_io_ow_t *b)
{
    static const uint8_t CMD[2] = { 0xCC, 0x44 };   /* SKIP ROM, CONVERT T */
    if (!aos_io_ow_reset(b)) return -1;
    return aos_io_ow_write(b, CMD, sizeof CMD) ? 750 : -1;
}

bool aos_io_ds18b20_read(aos_io_ow_t *b, uint64_t rom, float *celsius, int *bits)
{
    uint8_t sp[9];
    if (!ds_scratchpad(b, rom, sp)) return false;
    int16_t raw = (int16_t)(sp[0] | sp[1] << 8);
    uint8_t fam = (uint8_t)rom;
    float t;
    int res;
    if (fam == 0x10) {
        /* DS18S20: half degrees, refined with COUNT_REMAIN (datasheet p. 6) */
        t = (float)(raw >> 1) - 0.25f + (float)(16 - sp[6]) / 16.0f;
        res = 12;
    } else if (fam == 0x3B) {
        /* MAX31850: the thermocouple in quarters of a degree, bits 15..2 */
        t = (float)(int16_t)(raw & 0xFFFC) / 16.0f;
        res = 14;
    } else {
        res = ((sp[4] >> 5) & 3) + 9;
        /* the low bits a coarser resolution leaves undefined */
        raw &= (int16_t)~((1 << (12 - res)) - 1);
        t = (float)raw / 16.0f;
    }
    if (celsius) *celsius = t;
    if (bits) *bits = res;
    return true;
}

bool aos_io_ds18b20_set_bits(aos_io_ow_t *b, uint64_t rom, int bits)
{
    if (bits < 9 || bits > 12) return false;
    uint8_t sp[9];
    if (!ds_scratchpad(b, rom, sp)) return false;
    uint8_t wr[4] = { 0x4E, sp[2], sp[3], (uint8_t)(((bits - 9) << 5) | 0x1F) };   /* TH, TL kept */
    static const uint8_t COPY = 0x48;
    if (!ow_select(b, rom) || !aos_io_ow_write(b, wr, sizeof wr)) return false;
    if (!ow_select(b, rom) || !aos_io_ow_write(b, &COPY, 1)) return false;
    aos_hal_sleep_ms(12);                       /* the EEPROM write: 10 ms */
    return true;
}

/* ---- LED strips ---- */

static const char *const TYPE_NAME[AOS_STRIP_TYPE_COUNT] = {
    "WS2812B", "WS2811 400 kHz", "SK6812", "SK6812 RGBW",
};
static const char *const ORDER_NAME[AOS_ORDER_COUNT] = { "GRB", "RGB", "BRG", "RBG", "GBR", "BGR" };
/* where R, G and B go on the wire, for each order */
static const uint8_t ORDER_POS[AOS_ORDER_COUNT][3] = {
    { 1, 0, 2 }, { 0, 1, 2 }, { 1, 2, 0 }, { 0, 2, 1 }, { 2, 0, 1 }, { 2, 1, 0 },
};

const char *aos_io_strip_type_name(aos_strip_type_t t) { return t < AOS_STRIP_TYPE_COUNT ? TYPE_NAME[t] : "?"; }
const char *aos_io_strip_order_name(aos_strip_order_t o) { return o < AOS_ORDER_COUNT ? ORDER_NAME[o] : "?"; }
bool aos_io_strip_type_rgbw(aos_strip_type_t t) { return t == AOS_STRIP_SK6812_RGBW; }

aos_io_strip_t *aos_io_strip_open(int gpio, const aos_strip_cfg_t *cfg, const char *owner)
{
    if (!cfg || !owner || !owner[0] || cfg->type >= AOS_STRIP_TYPE_COUNT || cfg->order >= AOS_ORDER_COUNT ||
        !cfg->count || cfg->count > AOS_STRIP_MAX_LEDS)
        return NULL;
    if (!header_pin(gpio)) { aos_hal_log("io", "LED strip: GPIO%d is not a usable pin of the header", gpio); return NULL; }
    if (!aos_io_claim(gpio, owner)) {
        aos_hal_log("io", "LED strip: GPIO%d is %s's", gpio, aos_io_owner(gpio) ? aos_io_owner(gpio) : "taken");
        return NULL;
    }
    aos_io_strip_t *s = calloc(1, sizeof *s);
    if (!s) { aos_io_release(gpio, owner); return NULL; }
    s->gpio = (int8_t)gpio;
    s->cfg = *cfg;
    s->bpp = aos_io_strip_type_rgbw(cfg->type) ? 4 : 3;
    snprintf(s->owner, sizeof s->owner, "%s", owner);
    /* PSRAM: the RMT reads it from its ISR, which needs no internal RAM
     * (MEMORY.md); a frame only glitches if a flash write stops the ISR */
    s->wire = aos_hal_io_alloc((size_t)cfg->count * s->bpp);
    if (!s->wire || !aos_io_be_strip_open(s)) {
        if (s->wire) aos_hal_io_free(s->wire);
        aos_io_release(gpio, owner);
        free(s);
        return NULL;
    }
    return s;
}

bool aos_io_strip_show(aos_io_strip_t *s, const uint8_t *rgbw)
{
    if (!s || !rgbw) return false;
    const uint8_t *pos = ORDER_POS[s->cfg.order];
    uint8_t *w = s->wire;
    for (unsigned i = 0; i < s->cfg.count; i++, rgbw += 4) {
        for (int c = 0; c < 3; c++) w[pos[c]] = rgbw[c];
        if (s->bpp == 4) w[3] = rgbw[3];
        w += s->bpp;
    }
    return aos_io_be_strip_send(s);
}

void aos_io_strip_close(aos_io_strip_t *s)
{
    if (!s) return;
    aos_io_be_strip_close(s);
    aos_hal_io_free(s->wire);
    aos_io_release(s->gpio, s->owner);
    free(s);
}
