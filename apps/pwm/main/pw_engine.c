/*
 * P4OS - PWM generator: the hardware, the patterns, the numbers.
 *
 * Every write to a channel happens here, from one LVGL timer every 20 ms:
 * the screen and the portal only change the config and mark it dirty. That
 * matters because of the LEDC's hardware fades. In the IDF, a duty change
 * on a channel whose fade is still running blocks until the fade is over,
 * so a pattern that fades in hardware owns its channel until the segment
 * ends (busy_until), and nothing else touches it before (esp_driver_ledc,
 * ledc.c: _ledc_fade_hw_acquire waits forever on the semaphore the fade's
 * interrupt gives back). Closing waits for the fade too, so that no channel
 * goes to its next owner halfway through one.
 *
 * Smooth patterns (breathe, sweep, ramp, sine) on a PWM channel go as
 * hardware fades of 40-120 ms, from where the pattern is to where it will
 * be at the end of the segment: piecewise linear in duty, smooth to the eye
 * and to a servo. Strobe and steps jump, and the analog level has no fades,
 * so those are written every tick.
 */
#include "pw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

pw_t *pw_g;

#define PI_F 3.14159265f
#define E_F  2.71828183f

static float clampf(float v, float a, float b)
{
    if (!(v >= a)) return a;        /* NaN too */
    return v > b ? b : v;
}

static bool pending(uint32_t until, uint32_t now) { return (int32_t)(until - now) > 0; }

/* ---------------------------------------------------------------- config */

void pw_cfg_default(pw_cfg_t *c, int mode)
{
    memset(c, 0, sizeof *c);
    c->gpio   = -1;
    c->mode   = (uint8_t)mode;
    c->freq   = 1000;
    c->duty   = 0.5f;
    c->pulse  = 1500.0f;
    c->smin   = 500;
    c->smax   = 2500;
    c->sdeg   = 180;
    c->bright = 0.5f;
    c->gamma  = 2.2f;
    c->level  = 0.5f;
    c->pat    = PW_P_NONE;
    c->period = 2.0f;
    c->lo     = 0.0f;
    c->hi     = 1.0f;
    c->nsteps = 4;
    c->step[0] = 0.0f;
    c->step[1] = 0.33f;
    c->step[2] = 0.66f;
    c->step[3] = 1.0f;
}

float pw_val(const pw_cfg_t *c)
{
    switch (c->mode) {
    case PW_M_PWM:   return c->duty;
    case PW_M_SERVO: return c->smax > c->smin ? clampf((c->pulse - c->smin) / (float)(c->smax - c->smin), 0, 1) : 0;
    case PW_M_LED:   return c->bright;
    default:         return c->level;
    }
}

void pw_val_set(pw_cfg_t *c, float v)
{
    v = clampf(v, 0, 1);
    switch (c->mode) {
    case PW_M_PWM:   c->duty = v; break;
    case PW_M_SERVO: c->pulse = roundf(c->smin + v * (float)(c->smax - c->smin)); break;
    case PW_M_LED:   c->bright = v; break;
    default:         c->level = v; break;
    }
}

float pw_out(const pw_ch_t *h, float v)
{
    v = clampf(v, 0, 1);
    switch (h->c.mode) {
    case PW_M_SERVO: {
        float us = h->c.smin + v * (float)(h->c.smax - h->c.smin);
        float hz = h->pwm ? (float)aos_io_pwm_freq(h->pwm) : 50.0f;
        return clampf(us * hz / 1e6f, 0, 1);
    }
    case PW_M_LED: return powf(v, h->c.gamma);
    default:       return v;
    }
}

bool pw_has_freq(int mode) { return mode == PW_M_PWM || mode == PW_M_LED; }

uint32_t pw_freq_of(const pw_cfg_t *c)
{
    if (c->mode == PW_M_SERVO) return 50;
    if (c->mode == PW_M_DAC) return 0;
    return c->freq;
}

/* three significant figures, as the knob shows them */
uint32_t pw_nice_freq(float hz)
{
    if (!(hz >= 1.0f)) return 1;
    if (hz >= (float)PW_FREQ_MAX) return PW_FREQ_MAX;
    if (hz < 1000.0f) return (uint32_t)(hz + 0.5f);
    int e = (int)floorf(log10f(hz));
    float q = powf(10.0f, (float)(e - 2));
    return (uint32_t)(roundf(hz / q) * q + 0.5f);
}

bool pw_pat_smooth(int pat)
{
    return pat == PW_P_BREATHE || pat == PW_P_SWEEP || pat == PW_P_RAMP || pat == PW_P_SINE;
}

float pw_pat_at(const pw_cfg_t *c, float ph)
{
    float s;
    switch (c->pat) {
    case PW_P_BREATHE: {
        /* the usual "breathing LED": e^sin, rescaled to 0..1 */
        float e = expf(sinf(2.0f * PI_F * ph - PI_F / 2.0f));
        s = (e - 1.0f / E_F) / (E_F - 1.0f / E_F);
        break;
    }
    case PW_P_SWEEP:  s = ph < 0.5f ? 2.0f * ph : 2.0f - 2.0f * ph; break;
    case PW_P_STROBE: s = ph < 0.1f ? 1.0f : 0.0f; break;
    case PW_P_RAMP:   s = ph; break;
    case PW_P_SINE:   s = 0.5f - 0.5f * cosf(2.0f * PI_F * ph); break;
    case PW_P_STEPS: {
        int n = c->nsteps < 1 ? 1 : c->nsteps > PW_STEPS_MAX ? PW_STEPS_MAX : c->nsteps;
        int k = (int)(ph * (float)n);
        k = k < 0 ? 0 : k >= n ? n - 1 : k;
        return clampf(c->step[k], 0, 1);
    }
    default: return pw_val(c);
    }
    return clampf(c->lo + (c->hi - c->lo) * s, 0, 1);
}

/* ---------------------------------------------------------------- the hardware */

int pw_freqs_in_use(uint32_t out[3])
{
    int n = 0;
    for (int i = 0; i < pw_g->n; i++) {
        const pw_ch_t *h = &pw_g->ch[i];
        if (!h->pwm) continue;
        bool seen = false;
        for (int k = 0; k < n && k < 3; k++) seen |= out[k] == h->asked;
        if (seen) continue;
        if (n < 3) out[n] = h->asked;
        n++;
    }
    return n;
}

int pw_gpio_user(int gpio, int except)
{
    for (int i = 0; i < pw_g->n; i++)
        if (i != except && pw_g->ch[i].c.gpio == gpio) return i;
    return -1;
}

static void explain(pw_ch_t *h, uint32_t hz)
{
    const char *o = aos_io_owner(h->c.gpio);
    if (o && strcmp(o, PW_OWNER)) {
        snprintf(h->err, sizeof h->err, _("El pin lo tiene %s"), o);
        return;
    }
    if (h->c.mode == PW_M_DAC) {
        snprintf(h->err, sizeof h->err, "%s", _("No se pudo abrir el nivel analógico"));
        return;
    }
    int open = 0;
    for (int i = 0; i < pw_g->n; i++) open += pw_g->ch[i].pwm != NULL;
    uint32_t f[3];
    int nf = pw_freqs_in_use(f);
    bool known = false;
    for (int k = 0; k < nf && k < 3; k++) known |= f[k] == hz;
    if (open >= PW_CH_MAX)
        snprintf(h->err, sizeof h->err, "%s", _("Los siete canales del LEDC están ocupados"));
    else if (nf >= 3 && !known)
        snprintf(h->err, sizeof h->err, "%s", _("Ya hay tres frecuencias distintas: el LEDC no da más"));
    else
        snprintf(h->err, sizeof h->err, "%s", _("No se pudo abrir el PWM (mirá el registro)"));
}

static bool hw_open(pw_ch_t *h)
{
    uint32_t now = lv_tick_get();
    h->err[0] = 0;
    if (h->c.gpio < 0) {
        snprintf(h->err, sizeof h->err, "%s", _("Elegí un pin para este canal"));
        return false;
    }
    if (h->c.mode == PW_M_DAC) {
        h->dac = aos_io_dac_open(h->c.gpio, PW_OWNER);
        if (!h->dac) { explain(h, 0); return false; }
    } else {
        uint32_t hz = pw_freq_of(&h->c);
        h->pwm = aos_io_pwm_open(h->c.gpio, hz, PW_OWNER);
        if (!h->pwm) { explain(h, hz); return false; }
        h->asked = hz;
        h->inv_dirty = h->c.invert;
    }
    aos_hal_log("pwm", "GPIO%d on as %s", h->c.gpio, pw_mode_name(h->c.mode));
    h->hw_valid = false;
    h->dirty = true;
    h->freq_dirty = false;
    h->busy_until = now;
    h->freq_ms = now;
    h->t0 = now;
    return true;
}

static void hw_close(pw_ch_t *h)
{
    if (h->pwm) {
        /* let a running fade end first (see the top of the file) */
        int32_t r = (int32_t)(h->busy_until - lv_tick_get());
        if (r > 0) aos_hal_sleep_ms((uint32_t)r + 5);
        aos_io_pwm_close(h->pwm);
        h->pwm = NULL;
    }
    if (h->dac) {
        aos_io_dac_close(h->dac);
        h->dac = NULL;
    }
    h->hw_valid = false;
    h->busy_until = 0;
}

static void hw_write(pw_ch_t *h, float o)
{
    if (h->pwm) aos_io_pwm_set_duty(h->pwm, o);
    else if (h->dac) aos_io_dac_set(h->dac, o);
    h->hw = o;
    h->hw_valid = true;
}

/* ---------------------------------------------------------------- channels */

int pw_add(int mode)
{
    if (pw_g->n >= PW_CH_MAX) return -1;
    pw_ch_t *h = &pw_g->ch[pw_g->n];
    memset(h, 0, sizeof *h);
    pw_cfg_default(&h->c, mode);
    h->live = pw_val(&h->c);
    pw_store_changed();
    return pw_g->n++;
}

void pw_remove(int i)
{
    if (i < 0 || i >= pw_g->n) return;
    hw_close(&pw_g->ch[i]);
    memmove(&pw_g->ch[i], &pw_g->ch[i + 1], (size_t)(pw_g->n - i - 1) * sizeof(pw_ch_t));
    pw_g->n--;
    if (pw_g->sel >= pw_g->n) pw_g->sel = pw_g->n ? pw_g->n - 1 : 0;
    pw_store_changed();
}

bool pw_set_on(pw_ch_t *h, bool on)
{
    bool is = h->pwm || h->dac;
    if (on != is) {
        if (on) on = hw_open(h);
        else { hw_close(h); h->err[0] = 0; }
    }
    h->c.on = on;
    pw_store_changed();
    return on;
}

void pw_set_gpio(pw_ch_t *h, int gpio)
{
    bool was = h->pwm || h->dac;
    hw_close(h);
    h->c.gpio = (int8_t)gpio;
    h->err[0] = 0;
    h->c.on = was ? hw_open(h) : false;
    pw_store_changed();
}

void pw_set_mode(pw_ch_t *h, int mode)
{
    if (mode == h->c.mode) return;
    bool kind = (h->c.mode == PW_M_DAC) != (mode == PW_M_DAC);
    bool was = h->pwm || h->dac;
    if (kind) hw_close(h);
    h->c.mode = (uint8_t)mode;
    if (kind) h->c.on = was ? hw_open(h) : false;
    else if (h->pwm) {
        if (pw_freq_of(&h->c) != h->asked) h->freq_dirty = true;
        h->dirty = true;
    }
    pw_store_changed();
}

void pw_set_freq(pw_ch_t *h, uint32_t hz)
{
    h->c.freq = hz < 1 ? 1 : hz > PW_FREQ_MAX ? PW_FREQ_MAX : hz;
    if (h->pwm && pw_freq_of(&h->c) != h->asked) h->freq_dirty = true;
    pw_store_changed();
}

void pw_set_pattern(pw_ch_t *h, int pat)
{
    h->c.pat = (uint8_t)pat;
    if (pat == PW_P_STEPS && h->c.nsteps < 2) {
        h->c.nsteps = 4;
        for (int k = 0; k < 4; k++) h->c.step[k] = (float)k / 3.0f;
    }
    h->t0 = lv_tick_get();
    h->dirty = true;
    pw_store_changed();
}

void pw_touch(pw_ch_t *h)
{
    h->dirty = true;
    pw_store_changed();
}

void pw_all_off(void)
{
    for (int i = 0; i < pw_g->n; i++) {
        hw_close(&pw_g->ch[i]);
        pw_g->ch[i].c.on = false;
        pw_g->ch[i].err[0] = 0;
    }
    pw_store_changed();
}

void pw_close_all(void)
{
    for (int i = 0; i < pw_g->n; i++) hw_close(&pw_g->ch[i]);
}

/* A whole set of channels at once (a preset, the portal). Channels that
 * keep their pin, kind and on/off keep their hardware too; the rest are
 * closed first and opened after, so two channels can swap pins. */
void pw_apply(const pw_cfg_t *cfg, int n)
{
    if (n > PW_CH_MAX) n = PW_CH_MAX;
    pw_cfg_t in[PW_CH_MAX];
    memcpy(in, cfg, (size_t)n * sizeof *in);
    /* one channel per pin: a repeated one is dropped from the later */
    for (int i = 0; i < n; i++) {
        if (in[i].mode >= PW_M_COUNT) in[i].mode = PW_M_PWM;
        if (in[i].pat >= PW_P_COUNT) in[i].pat = PW_P_NONE;
        for (int k = 0; k < i; k++)
            if (in[i].gpio >= 0 && in[k].gpio == in[i].gpio) { in[i].gpio = -1; in[i].on = false; }
    }
    bool reopen[PW_CH_MAX] = { 0 };
    int had = pw_g->n;
    for (int i = 0; i < had; i++) {
        pw_ch_t *h = &pw_g->ch[i];
        if (i >= n) { hw_close(h); continue; }
        const pw_cfg_t *c = &in[i];
        reopen[i] = c->gpio != h->c.gpio || (c->mode == PW_M_DAC) != (h->c.mode == PW_M_DAC) || c->on != h->c.on;
        if (reopen[i]) hw_close(h);
    }
    for (int i = had; i < n; i++) {
        memset(&pw_g->ch[i], 0, sizeof pw_g->ch[i]);
        reopen[i] = true;
    }
    /* the new count first: an error explained while opening counts them all */
    pw_g->n = n;
    uint32_t now = lv_tick_get();
    for (int i = 0; i < n; i++) {
        pw_ch_t *h = &pw_g->ch[i];
        pw_cfg_t old = h->c;
        h->c = in[i];
        if (reopen[i]) {
            h->err[0] = 0;
            h->c.on = in[i].on ? hw_open(h) : false;
        } else if (h->pwm) {
            if (pw_freq_of(&h->c) != h->asked) h->freq_dirty = true;
            if (old.invert != h->c.invert) h->inv_dirty = true;
        }
        if (old.pat != h->c.pat) h->t0 = now;
        h->dirty = true;
        h->live = pw_val(&h->c);
    }
    if (pw_g->sel >= n) pw_g->sel = n ? n - 1 : 0;
    pw_store_changed();
}

/* ---------------------------------------------------------------- the engine */

/* How long ledc_set_fade_with_time() really takes (esp_driver_ledc,
 * ledc.c, _ledc_set_fade_with_time): whole periods, and when the change is
 * bigger than the periods there are, whole steps of duty, which can make it
 * up to twice as long as asked. */
static uint32_t fade_real_ms(uint32_t ms, float hz, int bits, float from, float to)
{
    float full = (float)(1u << bits);
    uint32_t a = (uint32_t)(from * full + 0.5f), b = (uint32_t)(to * full + 0.5f);
    uint32_t delta = a > b ? a - b : b - a;
    uint32_t total = (uint32_t)((float)ms * hz / 1000.0f);
    uint32_t cycles;
    if (!delta || !total) cycles = 1;
    else if (total > delta) cycles = total / delta * delta;
    else cycles = delta / (delta / total);
    return (uint32_t)((float)cycles * 1000.0f / hz) + 1;
}

static void run_channel(pw_ch_t *h, uint32_t now)
{
    pw_cfg_t *c = &h->c;
    bool pat = c->pat != PW_P_NONE;
    float T = c->period * 1000.0f;
    float ph = 0;
    if (pat) {
        ph = fmodf((float)(uint32_t)(now - h->t0) / T, 1.0f);
        h->live = pw_pat_at(c, ph);
    } else {
        h->live = pw_val(c);
    }
    if (!h->pwm && !h->dac) return;
    if (pending(h->busy_until, now)) return;        /* a fade is running */

    if (h->freq_dirty && (uint32_t)(now - h->freq_ms) >= 40) {
        uint32_t hz = pw_freq_of(c);
        h->freq_ms = now;
        h->freq_dirty = false;
        if (aos_io_pwm_set_freq(h->pwm, hz)) {
            h->asked = hz;
            h->err[0] = 0;
        } else {
            char f[24];
            pw_fmt_freq(f, sizeof f, (float)hz);
            snprintf(h->err, sizeof h->err, _("%s no entra: el LEDC da tres frecuencias a la vez como máximo"), f);
        }
        h->hw_valid = false;
    }
    if (h->inv_dirty) {
        aos_io_pwm_set_invert(h->pwm, c->invert);
        h->inv_dirty = false;
        h->hw_valid = false;
    }

    uint32_t seg = (uint32_t)clampf(T / 12.0f, 40.0f, 120.0f);
    float hz = h->pwm ? (float)aos_io_pwm_freq(h->pwm) : 0.0f;
    /* a fade moves once a period: with fewer than four in a segment (a
     * servo's 50 Hz on a fast pattern, a slow PWM) it is no better than
     * writing every tick, and it would hold the channel too long */
    if (pat && h->pwm && pw_pat_smooth(c->pat) && hz * (float)seg / 1000.0f >= 4.0f) {
        float pe = ph + (float)seg / T;
        if (c->pat == PW_P_RAMP && pe >= 1.0f) {
            /* the ramp jumps back at the end of its cycle: no fade across */
            seg = (uint32_t)((1.0f - ph) * T);
            pe = 1.0f;
        } else if (pe >= 1.0f) {
            pe -= 1.0f;
        }
        float cur = pw_out(h, h->live);
        if (seg < 25) {
            hw_write(h, cur);
            h->dirty = false;
            return;
        }
        if (!h->hw_valid || fabsf(cur - h->hw) > 0.02f) hw_write(h, cur);
        float to = pw_out(h, pw_pat_at(c, pe));
        uint32_t ms = seg * 9 / 10;
        uint32_t real = fade_real_ms(ms, hz, aos_io_pwm_bits(h->pwm), h->hw, to);
        aos_io_pwm_fade(h->pwm, to, ms);
        h->hw = to;
        h->hw_valid = true;
        h->busy_until = now + (real + 2 > seg ? real + 2 : seg);
        h->dirty = false;
        return;
    }

    float o = pw_out(h, h->live);
    if (h->dirty || !h->hw_valid || fabsf(o - h->hw) > 1e-6f) hw_write(h, o);
    h->dirty = false;
}

static void engine_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    for (int i = 0; i < pw_g->n; i++) run_channel(&pw_g->ch[i], now);
    if (++pw_g->ticks % 3 == 0) pw_ui_live();
}

static void io_cb(lv_timer_t *t)
{
    (void)t;
    pw_store_poll();
}

void pw_engine_start(void)
{
    if (!pw_g->engine) pw_g->engine = lv_timer_create(engine_cb, PW_TICK_MS, NULL);
    if (!pw_g->io) pw_g->io = lv_timer_create(io_cb, 300, NULL);
}

void pw_engine_stop(void)
{
    if (pw_g->engine) lv_timer_delete(pw_g->engine);
    if (pw_g->io) lv_timer_delete(pw_g->io);
    pw_g->engine = pw_g->io = NULL;
}

/* ---------------------------------------------------------------- numbers */

void pw_fnum(char *b, size_t n, float v, int dec)
{
    snprintf(b, n, "%.*f", dec, (double)v);
    for (char *p = b; *p; p++) if (*p == '.') *p = ',';
}

static void fmt_sig(char *b, size_t n, float v, const char *unit, int sig)
{
    float a = fabsf(v);
    int dec = a >= 100.0f ? sig - 3 : a >= 10.0f ? sig - 2 : sig - 1;
    if (dec < 0) dec = 0;
    char num[24];
    pw_fnum(num, sizeof num, v, dec);
    snprintf(b, n, "%s %s", num, unit);
}

static void freq_sig(char *b, size_t n, float hz, int sig)
{
    if (hz >= 1e6f) fmt_sig(b, n, hz / 1e6f, "MHz", sig);
    else if (hz >= 1e3f) fmt_sig(b, n, hz / 1e3f, "kHz", sig);
    else snprintf(b, n, "%u Hz", (unsigned)(hz + 0.5f));
}

void pw_fmt_freq(char *b, size_t n, float hz) { freq_sig(b, n, hz, 3); }

void pw_fmt_time(char *b, size_t n, float s)
{
    if (s >= 1.0f) fmt_sig(b, n, s, "s", 3);
    else if (s >= 1e-3f) fmt_sig(b, n, s * 1e3f, "ms", 3);
    else if (s >= 1e-6f) fmt_sig(b, n, s * 1e6f, "µs", 3);
    else fmt_sig(b, n, s * 1e9f, "ns", 3);
}

void pw_fmt_value(const pw_ch_t *h, float v, char *big, size_t nb, char *sub, size_t ns)
{
    const pw_cfg_t *c = &h->c;
    char a[24], f[24];
    v = clampf(v, 0, 1);
    switch (c->mode) {
    case PW_M_PWM:
        pw_fnum(a, sizeof a, v * 100.0f, 1);
        snprintf(big, nb, "%s %%", a);
        if (h->pwm) {
            freq_sig(f, sizeof f, (float)aos_io_pwm_freq(h->pwm), 4);
            snprintf(sub, ns, _("%s · %d bits"), f, aos_io_pwm_bits(h->pwm));
        } else {
            pw_fmt_freq(f, sizeof f, (float)c->freq);
            snprintf(sub, ns, "%s", f);
        }
        break;
    case PW_M_SERVO: {
        float us = c->smin + v * (float)(c->smax - c->smin);
        snprintf(big, nb, "%d°", (int)roundf(v * (float)c->sdeg));
        snprintf(sub, ns, "%d µs", (int)roundf(us));
        break;
    }
    case PW_M_LED:
        pw_fnum(a, sizeof a, v * 100.0f, v < 0.1f ? 1 : 0);
        snprintf(big, nb, "%s %%", a);
        pw_fnum(a, sizeof a, powf(v, c->gamma) * 100.0f, 1);
        snprintf(sub, ns, _("ciclo %s %%"), a);
        break;
    default:
        pw_fnum(a, sizeof a, v * 3.3f, 2);
        snprintf(big, nb, "%s V", a);
        snprintf(sub, ns, _("%d de 255"), (int)roundf(v * 255.0f));
        break;
    }
}

static const char *const MODE_NAMES[PW_M_COUNT] = { N_("PWM"), N_("Servo"), N_("LED"), N_("Nivel") };
static const char *const PAT_NAMES[PW_P_COUNT] = {
    N_("Fijo"), N_("Respirar"), N_("Barrido"), N_("Estrobo"), N_("Rampa"), N_("Seno"), N_("Pasos"),
};
static const uint32_t MODE_COLORS[PW_M_COUNT] = { 0x0A84FF, 0xFF9F0A, 0xFFD60A, 0x40C8E0 };

const char *pw_mode_name(int mode) { return _(MODE_NAMES[mode < PW_M_COUNT ? mode : 0]); }
const char *pw_pat_name(int pat) { return _(PAT_NAMES[pat < PW_P_COUNT ? pat : 0]); }
uint32_t pw_mode_color(int mode) { return MODE_COLORS[mode < PW_M_COUNT ? mode : 0]; }
