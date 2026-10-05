/*
 * P4OS - PWM, an analog level out, infrared and CAN on the header's pins
 * (aos_io.h): the part the board and the simulator share. It checks what
 * an app asks, claims the pins for it and keeps the numbers; the backends
 * (aos_io_signal_p4.c, sim/io_signal_sim.c) move the hardware.
 */
#include "aos_io_backend.h"
#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool header_pin(int gpio)
{
    const aos_io_pin_t *p = aos_io_pin_of_gpio(gpio);
    return p && !(p->flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD));
}

static bool take(int gpio, const char *owner, const char *what)
{
    if (!owner || !owner[0]) return false;
    if (!header_pin(gpio)) { aos_hal_log("io", "%s: GPIO%d is not a usable pin of the header", what, gpio); return false; }
    if (!aos_io_claim(gpio, owner)) {
        aos_hal_log("io", "%s: GPIO%d is %s's", what, gpio, aos_io_owner(gpio) ? aos_io_owner(gpio) : "taken");
        return false;
    }
    return true;
}

static float clamp01(float v)
{
    if (!(v >= 0.0f)) return 0.0f;      /* NaN too */
    return v > 1.0f ? 1.0f : v;
}

/* ---- PWM ---- */

aos_io_pwm_t *aos_io_pwm_open(int gpio, uint32_t freq_hz, const char *owner)
{
    if (!freq_hz || freq_hz > 20000000) return NULL;
    if (!take(gpio, owner, "PWM")) return NULL;
    aos_io_pwm_t *p = calloc(1, sizeof *p);
    if (!p) { aos_io_release(gpio, owner); return NULL; }
    p->gpio = (int8_t)gpio;
    snprintf(p->owner, sizeof p->owner, "%s", owner);
    if (!aos_io_be_pwm_open(p, freq_hz)) {
        aos_io_release(gpio, owner);
        free(p);
        return NULL;
    }
    return p;
}

bool aos_io_pwm_set_freq(aos_io_pwm_t *p, uint32_t freq_hz)
{
    if (!p || !freq_hz || freq_hz > 20000000) return false;
    if (!aos_io_be_pwm_set_freq(p, freq_hz)) return false;
    return aos_io_be_pwm_set_duty(p, p->invert ? 1.0f - p->duty : p->duty, 0);
}

uint32_t aos_io_pwm_freq(const aos_io_pwm_t *p) { return p ? p->freq : 0; }
int      aos_io_pwm_bits(const aos_io_pwm_t *p) { return p ? p->bits : 0; }
float    aos_io_pwm_duty(const aos_io_pwm_t *p) { return p ? p->duty : 0.0f; }

bool aos_io_pwm_set_duty(aos_io_pwm_t *p, float duty)
{
    if (!p) return false;
    p->duty = clamp01(duty);
    return aos_io_be_pwm_set_duty(p, p->invert ? 1.0f - p->duty : p->duty, 0);
}

bool aos_io_pwm_set_pulse_us(aos_io_pwm_t *p, float us)
{
    if (!p || !p->freq || !(us >= 0.0f)) return false;
    return aos_io_pwm_set_duty(p, us * (float)p->freq / 1e6f);
}

bool aos_io_pwm_fade(aos_io_pwm_t *p, float duty, uint32_t ms)
{
    if (!p) return false;
    p->duty = clamp01(duty);
    return aos_io_be_pwm_set_duty(p, p->invert ? 1.0f - p->duty : p->duty, ms ? ms : 1);
}

bool aos_io_pwm_set_invert(aos_io_pwm_t *p, bool invert)
{
    if (!p) return false;
    p->invert = invert;
    return aos_io_be_pwm_set_duty(p, invert ? 1.0f - p->duty : p->duty, 0);
}

void aos_io_pwm_close(aos_io_pwm_t *p)
{
    if (!p) return;
    aos_io_be_pwm_close(p);
    aos_io_release(p->gpio, p->owner);
    free(p);
}

/* ---- the analog level ---- */

aos_io_dac_t *aos_io_dac_open(int gpio, const char *owner)
{
    if (!take(gpio, owner, "analog out")) return NULL;
    aos_io_dac_t *d = calloc(1, sizeof *d);
    if (!d) { aos_io_release(gpio, owner); return NULL; }
    d->gpio = (int8_t)gpio;
    snprintf(d->owner, sizeof d->owner, "%s", owner);
    if (!aos_io_be_dac_open(d)) {
        aos_io_release(gpio, owner);
        free(d);
        return NULL;
    }
    return d;
}

bool aos_io_dac_set(aos_io_dac_t *d, float level)
{
    if (!d) return false;
    d->level = clamp01(level);
    return aos_io_be_dac_set(d, d->level);
}

float aos_io_dac_level(const aos_io_dac_t *d) { return d ? d->level : 0.0f; }

void aos_io_dac_close(aos_io_dac_t *d)
{
    if (!d) return;
    aos_io_be_dac_close(d);
    aos_io_release(d->gpio, d->owner);
    free(d);
}

/* ---- infrared ---- */

static aos_io_ir_t *ir_open(int gpio, bool tx, uint32_t gap_us, const char *owner)
{
    if (!take(gpio, owner, tx ? "IR out" : "IR in")) return NULL;
    aos_io_ir_t *ir = calloc(1, sizeof *ir);
    if (!ir) { aos_io_release(gpio, owner); return NULL; }
    ir->gpio = (int8_t)gpio;
    ir->tx = tx;
    ir->gap_us = gap_us ? (gap_us > 30000 ? 30000 : gap_us) : 20000;
    ir->carrier_hz = 38000;
    ir->duty = 33;
    snprintf(ir->owner, sizeof ir->owner, "%s", owner);
    if (!aos_io_be_ir_open(ir)) {
        aos_io_release(gpio, owner);
        free(ir);
        return NULL;
    }
    return ir;
}

aos_io_ir_t *aos_io_ir_rx_open(int gpio, uint32_t gap_us, const char *owner) { return ir_open(gpio, false, gap_us, owner); }
aos_io_ir_t *aos_io_ir_tx_open(int gpio, const char *owner) { return ir_open(gpio, true, 0, owner); }

int aos_io_ir_read(aos_io_ir_t *ir, uint16_t *us, int max, int timeout_ms)
{
    if (!ir || ir->tx || !us || max <= 0) return -1;
    return aos_io_be_ir_read(ir, us, max, timeout_ms);
}

bool aos_io_ir_set_carrier(aos_io_ir_t *ir, uint32_t hz, int duty_percent)
{
    if (!ir || !ir->tx || hz > 1000000 || duty_percent < 1 || duty_percent > 99) return false;
    ir->carrier_hz = hz;
    ir->duty = (uint8_t)duty_percent;
    return aos_io_be_ir_carrier(ir);
}

bool aos_io_ir_send(aos_io_ir_t *ir, const uint16_t *us, int n)
{
    if (!ir || !ir->tx || !us || n <= 0 || n > AOS_IR_MAX_DURATIONS) return false;
    return aos_io_be_ir_send(ir, us, n);
}

void aos_io_ir_close(aos_io_ir_t *ir)
{
    if (!ir) return;
    aos_io_be_ir_close(ir);
    aos_io_release(ir->gpio, ir->owner);
    free(ir);
}

/* ---- CAN ---- */

aos_io_can_t *aos_io_can_open(int tx_gpio, int rx_gpio, uint32_t bitrate, aos_can_mode_t mode, const char *owner)
{
    if (bitrate < 25000 || bitrate > 1000000 || mode > AOS_CAN_SELFTEST) return NULL;
    if (tx_gpio == rx_gpio && mode != AOS_CAN_SELFTEST) {
        aos_hal_log("io", "CAN: one pin for TX and RX is only for the self test");
        return NULL;
    }
    if (!take(tx_gpio, owner, "CAN TX")) return NULL;
    if (rx_gpio != tx_gpio && !take(rx_gpio, owner, "CAN RX")) { aos_io_release(tx_gpio, owner); return NULL; }
    aos_io_can_t *c = calloc(1, sizeof *c);
    if (!c) goto fail;
    c->tx_gpio = (int8_t)tx_gpio;
    c->rx_gpio = (int8_t)rx_gpio;
    c->bitrate = bitrate;
    c->mode = mode;
    snprintf(c->owner, sizeof c->owner, "%s", owner);
    if (aos_io_be_can_open(c)) return c;
    free(c);
fail:
    aos_io_release(tx_gpio, owner);
    if (rx_gpio != tx_gpio) aos_io_release(rx_gpio, owner);
    return NULL;
}

bool aos_io_can_filter(aos_io_can_t *c, uint32_t id, uint32_t mask, bool ext)
{
    return c && aos_io_be_can_filter(c, id, mask, ext);
}

bool aos_io_can_send(aos_io_can_t *c, const aos_can_frame_t *f, int timeout_ms)
{
    if (!c || !f || f->len > 8 || c->mode == AOS_CAN_LISTEN) return false;
    if (f->id > (f->ext ? 0x1FFFFFFFu : 0x7FFu)) return false;
    if (!aos_io_be_can_send(c, f, timeout_ms)) return false;
    c->sent++;
    return true;
}

int aos_io_can_recv(aos_io_can_t *c, aos_can_frame_t *f, int timeout_ms)
{
    if (!c || !f) return -1;
    int r = aos_io_be_can_recv(c, f, timeout_ms);
    if (r > 0) c->received++;
    return r;
}

bool aos_io_can_status(aos_io_can_t *c, aos_can_status_t *st)
{
    if (!c || !st) return false;
    memset(st, 0, sizeof *st);
    st->state = "active";
    if (!aos_io_be_can_status(c, st)) return false;
    st->received = c->received;
    st->sent = c->sent;
    st->dropped = c->dropped;
    return true;
}

bool aos_io_can_recover(aos_io_can_t *c) { return c && aos_io_be_can_recover(c); }

void aos_io_can_close(aos_io_can_t *c)
{
    if (!c) return;
    aos_io_be_can_close(c);
    aos_io_release(c->tx_gpio, c->owner);
    if (c->rx_gpio != c->tx_gpio) aos_io_release(c->rx_gpio, c->owner);
    free(c);
}
