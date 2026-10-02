/*
 * P4OS - the LED strip service (aos_leds.h).
 *
 * A thread of its own (PSRAM stack) that owns the strip: it opens it when a
 * configuration names a GPIO and turns it on, renders the effect into a
 * frame in PSRAM, applies the brightness and the current limit, and sends
 * it. Off, it sends black once and goes quiet. Every buffer is PSRAM
 * (aos_hal_io_alloc): nothing here needs internal RAM.
 *
 * The current estimate is WLED's: a LED draws ma_per_led at full white,
 * shared equally by its channels, linearly with each channel's value, plus
 * about 1 mA doing nothing. When a supply budget is given and the frame
 * would go over it, the whole frame is dimmed to fit.
 */
#include "aos_leds.h"
#include "aos_io.h"
#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FPS_MAX     50
#define IDLE_MA     1           /* a WS2812B's own draw with its LEDs off */
#define SAVE_MS     2000
#define OWNER       "LEDs"

static void *s_mx;
static aos_leds_cfg_t s_cfg = {
    .gpio = -1, .type = AOS_STRIP_WS2812B, .order = AOS_ORDER_GRB, .count = 30, .ma_per_led = 55,
    .psu_ma = 1000, .on = false, .brightness = 128, .effect = AOS_FX_RAINBOW, .speed = 128, .intensity = 128,
    .color = { 0xFF7A00, 0x0050FF }, .white = 0,
};
static aos_leds_status_t s_st;
static uint32_t s_gen, s_saved_gen;
static uint64_t s_changed_ms;
static bool s_started;
static uint8_t *s_out;          /* the frame as sent (after brightness), for the preview */
static int s_out_n;

static void lock(void) { aos_hal_mutex_lock(s_mx); }
static void unlock(void) { aos_hal_mutex_unlock(s_mx); }

/* ---- the preferences: one line, so a change is one write ---- */

static void cfg_save(const aos_leds_cfg_t *c)
{
    char v[128];
    snprintf(v, sizeof v, "%d %d %d %u %u %u %d %u %u %u %u %06X %06X %u %d", c->gpio, (int)c->type, (int)c->order,
             c->count, c->ma_per_led, c->psu_ma, c->on, c->brightness, c->effect, c->speed, c->intensity,
             (unsigned)(c->color[0] & 0xFFFFFF), (unsigned)(c->color[1] & 0xFFFFFF), c->white, c->reverse);
    aos_hal_pref_set_str("leds_cfg", v);
}

static void cfg_load(aos_leds_cfg_t *c)
{
    char v[128];
    if (!aos_hal_pref_get_str("leds_cfg", v, sizeof v)) return;
    int g, t, o, on, rev;
    unsigned n, ma, psu, bri, fx, sp, in, c0, c1, w;
    if (sscanf(v, "%d %d %d %u %u %u %d %u %u %u %u %x %x %u %d", &g, &t, &o, &n, &ma, &psu, &on, &bri, &fx, &sp, &in,
               &c0, &c1, &w, &rev) != 15)
        return;
    if (t < 0 || t >= AOS_STRIP_TYPE_COUNT || o < 0 || o >= AOS_ORDER_COUNT || !n || n > AOS_STRIP_MAX_LEDS) return;
    *c = (aos_leds_cfg_t){ .gpio = (int8_t)g, .type = (aos_strip_type_t)t, .order = (aos_strip_order_t)o, .count = (uint16_t)n,
                           .ma_per_led = (uint16_t)ma, .psu_ma = (uint16_t)psu, .on = on != 0, .brightness = (uint8_t)bri,
                           .effect = (uint8_t)(fx < AOS_FX_COUNT ? fx : 0), .speed = (uint8_t)sp, .intensity = (uint8_t)in,
                           .color = { c0, c1 }, .white = (uint8_t)w, .reverse = rev != 0 };
}

/* ---- colour helpers ---- */

static void hsv(uint8_t *p, uint8_t h, uint8_t s, uint8_t v)
{
    uint8_t region = h / 43, rem = (uint8_t)((h - region * 43) * 6);
    uint8_t pp = (uint8_t)(v * (255 - s) >> 8), q = (uint8_t)(v * (255 - (s * rem >> 8)) >> 8),
            t = (uint8_t)(v * (255 - (s * (255 - rem) >> 8)) >> 8);
    uint8_t r, g, b;
    switch (region) {
    case 0: r = v; g = t; b = pp; break;
    case 1: r = q; g = v; b = pp; break;
    case 2: r = pp; g = v; b = t; break;
    case 3: r = pp; g = q; b = v; break;
    case 4: r = t; g = pp; b = v; break;
    default: r = v; g = pp; b = q; break;
    }
    p[0] = r; p[1] = g; p[2] = b;
}

static void put(uint8_t *p, uint32_t rgb, uint8_t scale)
{
    p[0] = (uint8_t)(((rgb >> 16) & 0xFF) * scale / 255);
    p[1] = (uint8_t)(((rgb >> 8) & 0xFF) * scale / 255);
    p[2] = (uint8_t)((rgb & 0xFF) * scale / 255);
}

static uint32_t mix(uint32_t a, uint32_t b, int t /* 0..255 */)
{
    uint32_t r = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        r |= (uint32_t)(ca + (cb - ca) * t / 255) << sh;
    }
    return r;
}

static uint32_t rnd(void)
{
    static uint32_t x = 2463534242u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

/* ---- the effects: each fills n LEDs of f (R G B W) for time t ms ---- */

typedef struct {
    uint8_t *heat;              /* Fire's, one byte a LED */
    uint8_t *spark;             /* Twinkle's levels */
    int n;
} fx_state_t;

static void fx_render(const aos_leds_cfg_t *c, fx_state_t *st, uint8_t *f, int n, uint32_t t)
{
    uint32_t c0 = c->color[0], c1 = c->color[1];
    /* speed 0..255 -> a period: 0 slow (8 s), 255 fast (0.25 s) */
    float period = 8000.0f / (1.0f + c->speed / 8.5f);
    float ph = fmodf((float)t / period, 1.0f);
    int width = 1 + c->intensity * (n > 1 ? n - 1 : 1) / 255 / 4;
    memset(f, 0, (size_t)n * 4);
    switch (c->effect) {
    case AOS_FX_SOLID:
        for (int i = 0; i < n; i++) put(f + 4 * i, c0, 255);
        break;
    case AOS_FX_BLINK: {
        bool lit = ph < 0.25f + c->intensity / 340.0f;
        for (int i = 0; i < n; i++) put(f + 4 * i, lit ? c0 : c1, 255);
        break;
    }
    case AOS_FX_BREATHE: {
        uint8_t v = (uint8_t)(25 + 230 * (0.5f - 0.5f * cosf(ph * 6.2831853f)));
        for (int i = 0; i < n; i++) put(f + 4 * i, c0, v);
        break;
    }
    case AOS_FX_WIPE: {
        /* the strip fills with the first colour, then with the second */
        int k = (int)(ph * 2 * n);
        for (int i = 0; i < n; i++) put(f + 4 * i, k < n ? (i < k ? c0 : c1) : (i < k - n ? c1 : c0), 255);
        break;
    }
    case AOS_FX_RAINBOW:
        for (int i = 0; i < n; i++) hsv(f + 4 * i, (uint8_t)(ph * 256), 255, 255);
        break;
    case AOS_FX_RAINBOW_CYCLE: {
        /* intensity: how many rainbows fit on the strip */
        float reps = 0.5f + c->intensity / 64.0f;
        for (int i = 0; i < n; i++) hsv(f + 4 * i, (uint8_t)((ph + reps * i / n) * 256), 255, 255);
        break;
    }
    case AOS_FX_THEATER: {
        int step = (int)(ph * 3 * 8) % 3;
        for (int i = 0; i < n; i++) put(f + 4 * i, (i % 3) == step ? c0 : c1, (i % 3) == step ? 255 : 40);
        break;
    }
    case AOS_FX_SCANNER: {
        /* Larson: a dot that sweeps there and back with a fading tail */
        float x = ph < 0.5f ? ph * 2 : 2 - ph * 2;
        float pos = x * (n - 1);
        for (int i = 0; i < n; i++) {
            float d = fabsf(i - pos);
            if (d < width + 1) put(f + 4 * i, c0, (uint8_t)(255 * (1 - d / (width + 1))));
        }
        break;
    }
    case AOS_FX_TWINKLE:
        if (st->spark) {
            int chance = 1 + c->intensity / 16;
            for (int i = 0; i < n; i++) {
                if (st->spark[i] > 8) st->spark[i] = (uint8_t)(st->spark[i] * 7 / 8);
                else st->spark[i] = 0;
                if ((int)(rnd() % 1000) < chance) st->spark[i] = 255;
                put(f + 4 * i, mix(c1, c0, st->spark[i]), (uint8_t)(30 + st->spark[i] * 225 / 255));
            }
        }
        break;
    case AOS_FX_FIRE:
        /* Fire2012 (Mark Kriegsman's, as in FastLED and WLED): cool, rise, spark */
        if (st->heat) {
            int cooling = 20 + (255 - c->intensity) / 4;
            for (int i = 0; i < n; i++) {
                int cool = (int)(rnd() % (uint32_t)(cooling * 10 / n + 2));
                st->heat[i] = (uint8_t)(st->heat[i] > cool ? st->heat[i] - cool : 0);
            }
            for (int i = n - 1; i >= 2; i--) st->heat[i] = (uint8_t)((st->heat[i - 1] + 2 * st->heat[i - 2]) / 3);
            if ((int)(rnd() % 255) < 120) {
                int y = (int)(rnd() % (uint32_t)(n < 7 ? n : 7));
                int h = st->heat[y] + 160 + (int)(rnd() % 96);
                st->heat[y] = (uint8_t)(h > 255 ? 255 : h);
            }
            for (int i = 0; i < n; i++) {
                uint8_t h = st->heat[i], *p = f + 4 * i;
                uint8_t t192 = (uint8_t)(h * 191 / 255), ramp = (uint8_t)((t192 & 63) << 2);
                if (t192 > 128) { p[0] = 255; p[1] = 255; p[2] = ramp; }
                else if (t192 > 64) { p[0] = 255; p[1] = ramp; p[2] = 0; }
                else { p[0] = ramp; p[1] = 0; p[2] = 0; }
            }
        }
        break;
    case AOS_FX_METEOR: {
        /* a meteor that runs the strip with a tail that breaks up */
        int head = (int)(ph * (n + width * 4));
        for (int i = 0; i < n; i++) {
            int d = head - i;
            if (d >= 0 && d < width * 4) {
                int v = 255 - d * 255 / (width * 4);
                if (d > 0 && rnd() % 4 == 0) v /= 2;
                put(f + 4 * i, c0, (uint8_t)v);
            }
        }
        break;
    }
    case AOS_FX_GRADIENT: {
        /* the two colours across the strip, sliding */
        for (int i = 0; i < n; i++) {
            float x = fmodf((float)i / n + ph, 1.0f);
            int tt = (int)(x < 0.5f ? x * 2 * 255 : (1 - x) * 2 * 255);
            put(f + 4 * i, mix(c0, c1, tt), 255);
        }
        break;
    }
    case AOS_FX_RUNNING: {
        /* a sine of light running along, in the main colour over the second */
        float waves = 1 + c->intensity / 32.0f;
        for (int i = 0; i < n; i++) {
            float v = 0.5f + 0.5f * sinf((float)i / n * waves * 6.2831853f - ph * 6.2831853f);
            put(f + 4 * i, mix(c1, c0, (int)(v * 255)), 255);
        }
        break;
    }
    default: break;
    }
    if (c->white)
        for (int i = 0; i < n; i++) f[4 * i + 3] = c->white;
    if (c->reverse)
        for (int i = 0; i < n / 2; i++)
            for (int k = 0; k < 4; k++) {
                uint8_t x = f[4 * i + k];
                f[4 * i + k] = f[4 * (n - 1 - i) + k];
                f[4 * (n - 1 - i) + k] = x;
            }
}

/* the frame's current at brightness 255, and the scale that keeps it in
 * the budget */
static uint32_t frame_ma(const aos_leds_cfg_t *c, const uint8_t *f, int n)
{
    int ch = aos_io_strip_type_rgbw(c->type) ? 4 : 3;
    uint64_t sum = 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < ch; k++) sum += f[4 * i + k];
    /* each channel at 255 draws ma_per_led / ch */
    return (uint32_t)(sum * c->ma_per_led / ((uint64_t)255 * ch)) + (uint32_t)n * IDLE_MA;
}

/* ---- the thread ---- */

static void leds_thread(void *arg)
{
    (void)arg;
    aos_io_strip_t *strip = NULL;
    aos_leds_cfg_t open_cfg = { .gpio = -1 };
    uint8_t *frame = NULL, *out = NULL;
    fx_state_t st = { 0 };
    int cap = 0;
    bool dark_sent = false;
    uint64_t last_try = 0;
    uint64_t t0 = aos_hal_uptime_ms(), fps_t = t0;
    int frames = 0;
    for (;;) {
        lock();
        aos_leds_cfg_t c = s_cfg;
        bool save = s_gen != s_saved_gen && aos_hal_uptime_ms() - s_changed_ms > SAVE_MS;
        uint32_t gen = s_gen;
        unlock();
        if (save) {
            cfg_save(&c);
            lock();
            s_saved_gen = gen;
            unlock();
        }

        /* the strip as configured: opened, reopened or closed */
        bool want = c.gpio >= 0 && c.count > 0;
        bool same = strip && open_cfg.gpio == c.gpio && open_cfg.type == c.type && open_cfg.order == c.order &&
                    open_cfg.count == c.count;
        if (strip && (!want || !same)) {
            aos_io_strip_close(strip);
            strip = NULL;
        }
        /* a failed open is tried again once a second; the preview goes on
         * meanwhile (and with no GPIO at all: effects can be chosen first) */
        if (want && !strip && (c.on || !dark_sent) && aos_hal_uptime_ms() - last_try >= 1000) {
            last_try = aos_hal_uptime_ms();
            aos_strip_cfg_t sc = { .type = c.type, .order = c.order, .count = c.count };
            strip = aos_io_strip_open(c.gpio, &sc, OWNER);
            lock();
            if (!strip) {
                const char *who = aos_io_owner(c.gpio);
                if (who && strcmp(who, OWNER)) snprintf(s_st.error, sizeof s_st.error, "GPIO%d: %s", c.gpio, who);
                else snprintf(s_st.error, sizeof s_st.error, "GPIO%d", c.gpio);
            } else {
                s_st.error[0] = 0;
            }
            unlock();
            open_cfg = c;
            if (strip) dark_sent = false;
        }
        if (!want) {
            lock();
            s_st.error[0] = 0;
            unlock();
        }
        if (c.count > cap) {
            aos_hal_io_free(frame); aos_hal_io_free(out); aos_hal_io_free(st.heat); aos_hal_io_free(st.spark);
            frame = aos_hal_io_alloc((size_t)c.count * 4);
            out = aos_hal_io_alloc((size_t)c.count * 4);
            st.heat = aos_hal_io_alloc(c.count);
            st.spark = aos_hal_io_alloc(c.count);
            cap = frame && out && st.heat && st.spark ? c.count : 0;
            if (cap) { memset(st.heat, 0, cap); memset(st.spark, 0, cap); }
            lock();
            aos_hal_io_free(s_out);
            s_out = aos_hal_io_alloc((size_t)c.count * 4);
            s_out_n = 0;
            unlock();
            if (!cap) { aos_hal_sleep_ms(500); continue; }
        }
        int n = c.count;

        if (!c.on) {
            if (strip && !dark_sent) {
                memset(out, 0, (size_t)n * 4);
                aos_io_strip_show(strip, out);
                dark_sent = true;
                aos_io_strip_close(strip);      /* the pin back: off is off */
                strip = NULL;
            }
            lock();
            s_st.running = false;
            s_st.fps = 0;
            s_st.ma_estimate = s_st.ma_out = (uint32_t)n * IDLE_MA;
            s_st.brightness_out = 0;
            s_st.limited = false;
            if (s_out) memset(s_out, 0, (size_t)n * 4);
            s_out_n = n;
            unlock();
            aos_hal_sleep_ms(100);
            continue;
        }

        uint64_t now = aos_hal_uptime_ms();
        fx_render(&c, &st, frame, n, (uint32_t)(now - t0));
        uint32_t full = frame_ma(&c, frame, n);     /* at brightness 255 */
        uint32_t idle = (uint32_t)n * IDLE_MA;
        uint32_t asked = idle + (full - idle) * c.brightness / 255;
        int bri = c.brightness;
        bool limited = false;
        if (c.psu_ma && asked > c.psu_ma && full > idle) {
            int fit = c.psu_ma > idle ? (int)((uint64_t)(c.psu_ma - idle) * 255 / (full - idle)) : 0;
            if (fit < bri) { bri = fit; limited = true; }
        }
        for (int i = 0; i < n * 4; i++) out[i] = (uint8_t)(frame[i] * bri / 255);
        bool ok = strip && aos_io_strip_show(strip, out);
        frames++;
        lock();
        s_st.running = ok;
        s_st.ma_estimate = asked;
        s_st.ma_out = idle + (full - idle) * (uint32_t)bri / 255;
        s_st.brightness_out = (uint8_t)bri;
        s_st.limited = limited;
        if (s_out) memcpy(s_out, out, (size_t)n * 4);
        s_out_n = n;
        if (now - fps_t >= 1000) { s_st.fps = frames; frames = 0; fps_t = now; }
        unlock();
        uint64_t spent = aos_hal_uptime_ms() - now;
        if (spent < 1000 / FPS_MAX) aos_hal_sleep_ms((uint32_t)(1000 / FPS_MAX - spent));
        if (strip) dark_sent = false;
    }
}

static void start(void)
{
    if (s_started) return;
    if (!s_mx) s_mx = aos_hal_mutex_create();
    lock();
    if (!s_started) {
        cfg_load(&s_cfg);
        s_saved_gen = s_gen;
        s_started = aos_hal_thread_start("leds", leds_thread, NULL, 6144, 3);
    }
    unlock();
}

void aos_leds_autostart(void)
{
    /* called five times a second: it looks once */
    static bool looked;
    if (looked) return;
    looked = true;
    if (!s_mx) s_mx = aos_hal_mutex_create();
    aos_leds_cfg_t c = s_cfg;
    cfg_load(&c);
    if (c.gpio >= 0 && c.on) start();
}

void aos_leds_get(aos_leds_cfg_t *out)
{
    start();
    lock();
    *out = s_cfg;
    unlock();
}

void aos_leds_set(const aos_leds_cfg_t *cfg)
{
    start();
    lock();
    s_cfg = *cfg;
    if (s_cfg.count < 1) s_cfg.count = 1;
    if (s_cfg.count > AOS_STRIP_MAX_LEDS) s_cfg.count = AOS_STRIP_MAX_LEDS;
    if (s_cfg.effect >= AOS_FX_COUNT) s_cfg.effect = 0;
    s_gen++;
    s_changed_ms = aos_hal_uptime_ms();
    unlock();
}

void aos_leds_status(aos_leds_status_t *out)
{
    start();
    lock();
    *out = s_st;
    unlock();
}

int aos_leds_frame(uint8_t *rgbw, int max_leds)
{
    start();
    lock();
    int n = s_out && s_out_n < max_leds ? s_out_n : s_out ? max_leds : 0;
    if (n) memcpy(rgbw, s_out, (size_t)n * 4);
    unlock();
    return n;
}

int aos_leds_fx_count(void) { return AOS_FX_COUNT; }
