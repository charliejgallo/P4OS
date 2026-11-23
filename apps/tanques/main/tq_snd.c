/*
 * TANQUES - the sound (see tq_snd.h)
 *
 * Voices of filtered noise and swept tones, each with an attack and a
 * squared decay, mixed with two continuous beds (the engine, a shell's
 * flight) into the speaker's ring, kept ~120 ms ahead.
 */
#include "tq_snd.h"

#include "aos_hal.h"

#include <math.h>
#include <string.h>

#define RATE    16000
#define TARGET  1920            /* samples kept queued: 120 ms */
#define VOICES  16

enum { V_OFF = 0, V_NOISE, V_TONE, V_SQUARE };

typedef struct {
    uint8_t  kind;
    uint8_t  hp_on;
    int32_t  t, dur;            /* samples; t < 0: still waiting */
    int32_t  amp, attack;
    uint32_t phase, step, step_end;     /* 16.16 over a 256-entry sine */
    int32_t  lp, lp_k, hp;
    uint32_t seed;
} voice_t;

static voice_t  s_v[VOICES];
static bool     s_open, s_on = true;
static uint32_t s_rng = 0x9E3779B9u;
static int16_t  s_sin[256];

/* the beds */
static float s_eng, s_eng_t, s_eng_ph, s_eng_lp, s_eng_lp2;
static float s_fl, s_fl_t, s_fl_hz, s_fl_hz_t, s_fl_ph, s_fl_lp, s_fl_hp;
static int   s_fl_kind;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static float rndf(void)
{
    return (float)(rnd() & 0xFFFF) / 32768.0f - 1.0f;
}

static uint32_t hz_step(int hz)
{
    return (uint32_t)hz * 1049u;        /* 2^24 / 16000: 256 << 16 a cycle */
}

static voice_t *voice_get(void)
{
    for (int i = 0; i < VOICES; i++)
        if (s_v[i].kind == V_OFF) return &s_v[i];
    int best = 0, left = 0x7FFFFFFF;
    for (int i = 0; i < VOICES; i++) {
        int l = s_v[i].dur - s_v[i].t;
        if (l < left) {
            left = l;
            best = i;
        }
    }
    return &s_v[best];
}

/* Noise: lp_k 0..256 is how bright (256 all of it, 20 a rumble); hp takes
 * the bottom off */
static void noise(int ms, int amp, int attack_ms, int lp_k, bool hp, int delay_ms)
{
    voice_t *v = voice_get();
    memset(v, 0, sizeof *v);
    v->kind = V_NOISE;
    v->dur = ms * RATE / 1000;
    v->amp = amp;
    v->attack = attack_ms * RATE / 1000;
    v->lp_k = lp_k;
    v->hp_on = hp;
    v->seed = rnd() | 1;
    v->t = -(delay_ms * RATE / 1000);
}

static void tone(int hz, int hz_end, int ms, int amp, int attack_ms, int delay_ms, uint8_t kind)
{
    voice_t *v = voice_get();
    memset(v, 0, sizeof *v);
    v->kind = kind;
    v->dur = ms * RATE / 1000;
    v->amp = amp;
    v->attack = attack_ms * RATE / 1000;
    v->step = hz_step(hz);
    v->step_end = hz_step(hz_end);
    v->t = -(delay_ms * RATE / 1000);
}

static inline int32_t env(const voice_t *v)
{
    if (v->t < v->attack) return v->amp * v->t / (v->attack ? v->attack : 1);
    int32_t left = v->dur - v->t, span = v->dur - v->attack;
    if (span <= 0) return 0;
    int32_t f = (left << 10) / span;
    return ((v->amp * f) >> 10) * f >> 10;
}

static int32_t voice_sample(voice_t *v)
{
    int32_t e = env(v), s = 0;
    switch (v->kind) {
    case V_NOISE: {
        v->seed ^= v->seed << 13;
        v->seed ^= v->seed >> 17;
        v->seed ^= v->seed << 5;
        int32_t n = (int32_t)(v->seed & 0xFFFF) - 32768;
        v->lp += ((n - v->lp) * v->lp_k) >> 8;
        s = v->lp;
        if (v->hp_on) {
            int32_t hp = s - v->hp;
            v->hp += (s - v->hp) >> 4;
            s = hp;
        }
        break;
    }
    case V_TONE:
    case V_SQUARE: {
        int32_t k = v->dur ? (int32_t)(((int64_t)v->t << 8) / v->dur) : 0;
        uint32_t st = v->step + (uint32_t)((((int32_t)v->step_end - (int32_t)v->step) >> 8) * k);
        v->phase += st;
        s = s_sin[(v->phase >> 16) & 255];
        if (v->kind == V_SQUARE) s = s > 0 ? 14000 + (s >> 2) : -14000 + (s >> 2);
        break;
    }
    default:
        break;
    }
    v->t++;
    if (v->t >= v->dur) v->kind = V_OFF;
    return (s * e) >> 15;
}

/* A blast: the crack, the roar, a falling tone; size 0 small .. 2 big */
static void blast(int size, int delay)
{
    static const int AMP[3] = { 7000, 10000, 13000 };
    static const int MS[3] = { 380, 700, 1300 };
    int a = AMP[size], ms = MS[size];
    noise(70, a, 0, 230, true, delay);                      /* the crack */
    noise(ms, a, 4, 70 - size * 15, false, delay);          /* the roar */
    noise(ms / 2, a * 2 / 3, 0, 140, false, delay + 10);    /* its grit */
    tone(320 - size * 40, 110, ms * 2 / 3, a * 3 / 4, 3, delay, V_TONE);
    if (size == 2) noise(1800, a / 2, 200, 30, false, delay + 300);   /* the rumble after */
}

/* --------------------------------------------------------------------------
 * The events
 * -------------------------------------------------------------------------- */

void tq_snd(int id)
{
    if (!s_on) return;
    if (!s_open) {
        switch (id) {
        case TQ_SND_TURN: aos_hal_beep(880, 60); break;
        case TQ_SND_FIRE:
        case TQ_SND_FIRE_LOB:
        case TQ_SND_FIRE_ROCKET:
        case TQ_SND_FIRE_SNIPER:
        case TQ_SND_FIRE_SCATTER: aos_hal_beep(520, 50); break;
        case TQ_SND_BLAST_S:
        case TQ_SND_BLAST:
        case TQ_SND_BLAST_L:
        case TQ_SND_KO: aos_hal_beep(330, 140); break;
        case TQ_SND_BUY: aos_hal_beep(1500, 40); break;
        case TQ_SND_WIN: aos_hal_beep(1047, 90); aos_hal_beep(1568, 200); break;
        case TQ_SND_LOSE: aos_hal_beep(300, 300); break;
        default: break;
        }
        return;
    }
    switch (id) {
    case TQ_SND_CLICK:
        noise(12, 2400, 0, 190, true, 0);
        tone(1900, 1500, 22, 1400, 1, 0, V_TONE);
        break;
    case TQ_SND_DENY:
        tone(230, 190, 140, 3600, 2, 0, V_SQUARE);
        break;
    case TQ_SND_BUY:
        noise(40, 3000, 0, 220, true, 0);
        tone(1319, 1319, 80, 4200, 2, 0, V_TONE);
        tone(1760, 1760, 260, 4400, 2, 80, V_TONE);
        tone(2637, 2637, 200, 1600, 2, 80, V_TONE);
        break;
    case TQ_SND_TURN:
        tone(784, 784, 110, 4200, 4, 0, V_TONE);
        tone(1175, 1175, 220, 4200, 4, 110, V_TONE);
        break;
    case TQ_SND_FIRE:
        noise(90, 12000, 0, 220, true, 0);
        noise(420, 9000, 2, 45, false, 0);
        tone(300, 100, 240, 7000, 2, 0, V_TONE);
        break;
    case TQ_SND_FIRE_LOB:
        tone(260, 90, 200, 9000, 3, 0, V_TONE);
        noise(260, 8000, 3, 60, false, 0);
        noise(40, 4000, 0, 200, true, 0);
        break;
    case TQ_SND_FIRE_ROCKET:
        noise(60, 6000, 0, 200, true, 0);
        noise(700, 5500, 60, 160, true, 20);
        tone(260, 900, 600, 2200, 40, 20, V_SQUARE);
        break;
    case TQ_SND_FIRE_LASER:
        tone(3000, 300, 280, 4200, 1, 0, V_SQUARE);
        tone(1500, 150, 280, 3600, 1, 0, V_TONE);
        tone(4400, 900, 160, 1800, 1, 30, V_TONE);
        break;
    case TQ_SND_FIRE_SNIPER:
        noise(50, 14000, 0, 250, true, 0);
        noise(260, 6500, 1, 80, false, 0);
        tone(1800, 600, 120, 2600, 1, 0, V_TONE);
        break;
    case TQ_SND_FIRE_SCATTER:
        for (int i = 0; i < 4; i++) {
            noise(60, 10000, 0, 220, true, i * 28);
            noise(220, 6000, 2, 60, false, i * 28);
        }
        break;
    case TQ_SND_FIRE_DRILL:
        tone(220, 700, 500, 3200, 20, 0, V_SQUARE);
        noise(500, 4000, 30, 120, true, 0);
        break;
    case TQ_SND_FIRE_CALL:
        for (int i = 0; i < 3; i++) tone(1400, 1400, 70, 2600, 2, i * 120, V_SQUARE);
        tone(520, 380, 1600, 3400, 400, 400, V_SQUARE);
        noise(1600, 3500, 400, 90, false, 400);
        break;
    case TQ_SND_BLAST_S: blast(0, 0); break;
    case TQ_SND_BLAST: blast(1, 0); break;
    case TQ_SND_BLAST_L: blast(2, 0); break;
    case TQ_SND_CLANG:
        tone(1180, 1100, 260, 4200, 1, 0, V_SQUARE);
        tone(1730, 1650, 320, 3400, 1, 0, V_TONE);
        tone(2950, 2800, 200, 2000, 1, 0, V_TONE);
        break;
    case TQ_SND_KO:
        blast(2, 120);
        tone(700, 90, 900, 3800, 10, 200, V_SQUARE);
        for (int i = 0; i < 5; i++) noise(40, 5000, 0, 200, true, 500 + i * 140 + (int)(rnd() % 60));
        break;
    case TQ_SND_MISS:
        tone(900, 260, 360, 3200, 5, 0, V_TONE);
        noise(200, 2500, 20, 120, true, 120);
        break;
    case TQ_SND_HEAL: {
        static const int N[] = { 659, 880, 1047, 1319 };
        for (int i = 0; i < 4; i++) tone(N[i], N[i], 240, 3800, 4, i * 85, V_TONE);
        break;
    }
    case TQ_SND_SHIELD:
        tone(420, 1260, 450, 3600, 30, 0, V_TONE);
        tone(630, 1890, 450, 2600, 30, 40, V_TONE);
        noise(450, 1600, 100, 200, true, 0);
        break;
    case TQ_SND_EMP:
        tone(1800, 90, 420, 4200, 2, 0, V_SQUARE);
        for (int i = 0; i < 6; i++) noise(25, 6000, 0, 250, true, 60 + i * 55);
        break;
    case TQ_SND_TELEPORT:
        tone(300, 2600, 220, 4000, 10, 0, V_TONE);
        tone(2600, 400, 240, 3600, 5, 230, V_TONE);
        noise(400, 1800, 50, 230, true, 0);
        break;
    case TQ_SND_BURN:
        noise(500, 4000, 60, 120, true, 0);
        for (int i = 0; i < 6; i++) noise(18, 5000, 0, 250, true, (int)(rnd() % 450));
        break;
    case TQ_SND_MINES:
        for (int i = 0; i < 3; i++) {
            tone(1500, 1300, 50, 3000, 1, i * 140, V_SQUARE);
            noise(40, 3500, 0, 160, true, i * 140);
        }
        break;
    case TQ_SND_SMOKE:
        noise(900, 6000, 120, 100, true, 0);
        noise(120, 4000, 0, 220, true, 0);
        break;
    case TQ_SND_WALL:
        for (int i = 0; i < 3; i++) {
            noise(110, 9000, 0, 50, false, i * 150);
            tone(260, 120, 110, 6000, 1, i * 150, V_TONE);
        }
        break;
    case TQ_SND_QUAKE:
        noise(1600, 11000, 150, 35, false, 0);
        noise(1400, 6000, 200, 110, false, 100);
        for (int i = 0; i < 8; i++) tone(200, 140, 120, 5000, 5, i * 160, V_SQUARE);
        break;
    case TQ_SND_WIN: {
        static const int N[] = { 523, 659, 784, 1047, 784, 1047, 1319 };
        static const int D[] = { 0, 120, 240, 360, 560, 680, 800 };
        for (int i = 0; i < 7; i++) tone(N[i], N[i], i == 6 ? 600 : 160, 5000, 4, D[i], V_TONE);
        for (int i = 0; i < 7; i++) tone(N[i] / 2, N[i] / 2, i == 6 ? 600 : 160, 2000, 4, D[i], V_SQUARE);
        break;
    }
    case TQ_SND_LOSE:
        tone(440, 415, 260, 4400, 6, 0, V_SQUARE);
        tone(392, 370, 260, 4200, 6, 280, V_SQUARE);
        tone(330, 220, 700, 4400, 6, 560, V_SQUARE);
        break;
    case TQ_SND_TESLA:
        for (int i = 0; i < 5; i++) {
            noise(60, 9000, 0, 250, true, i * 70);
            tone(2400 - i * 250, 600, 90, 3000, 1, i * 70, V_SQUARE);
        }
        noise(500, 4000, 20, 200, true, 0);
        tone(120, 60, 600, 3000, 20, 0, V_SQUARE);
        break;
    case TQ_SND_SPLASH:
        noise(500, 8000, 5, 170, true, 0);
        noise(300, 5000, 0, 60, false, 0);
        for (int i = 0; i < 5; i++) tone(900 + (int)(rnd() % 900), 1800, 40, 1600, 2, 80 + i * 60, V_TONE);
        break;
    case TQ_SND_POINT:
        tone(1047, 1047, 90, 4000, 2, 0, V_TONE);
        tone(1568, 1568, 220, 4000, 2, 90, V_TONE);
        break;
    case TQ_SND_FLAG:
        tone(659, 659, 100, 4200, 3, 0, V_SQUARE);
        tone(880, 880, 100, 4200, 3, 110, V_SQUARE);
        tone(1175, 1175, 260, 4200, 3, 220, V_SQUARE);
        break;
    case TQ_SND_CAPTURE: {
        static const int N[] = { 784, 988, 1175, 1568, 1175, 1568 };
        for (int i = 0; i < 6; i++) tone(N[i], N[i], i == 5 ? 500 : 120, 4800, 3, i * 110, V_TONE);
        noise(300, 3000, 0, 220, true, 0);
        break;
    }
    case TQ_SND_COIN:
        tone(1976, 1976, 70, 4200, 1, 0, V_SQUARE);
        tone(2637, 2637, 320, 4200, 1, 70, V_SQUARE);
        break;
    case TQ_SND_WAVE:
        for (int i = 0; i < 2; i++) tone(440, 660, 380, 4600, 30, i * 450, V_SQUARE);
        break;
    default:
        break;
    }
}

void tq_snd_engine(float level)
{
    s_eng_t = s_on && s_open ? (level < 0 ? 0 : level > 1 ? 1 : level) : 0;
}

void tq_snd_flight(int kind, float level, float height)
{
    s_fl_kind = kind;
    s_fl_t = s_on && s_open ? (level < 0 ? 0 : level > 1 ? 1 : level) : 0;
    float h = height < 0 ? 0 : height > 18 ? 18 : height;
    s_fl_hz_t = 650.0f + h * 70.0f;      /* the whistle falls with the shell */
}

/* --------------------------------------------------------------------------
 * The beds
 * -------------------------------------------------------------------------- */

/* The engine: a putt-putt of bursts of dark noise, faster as it works */
static float engine_sample(void)
{
    s_eng += (s_eng_t - s_eng) * 0.0015f;
    if (s_eng < 0.002f) return 0;
    float hz = 16.0f + 16.0f * s_eng;
    s_eng_ph += hz / RATE;
    if (s_eng_ph >= 1) s_eng_ph -= 1;
    float burst = 1.0f - s_eng_ph;
    burst = burst * burst * burst;
    float n = rndf();
    s_eng_lp += (n - s_eng_lp) * 0.18f;
    s_eng_lp2 += (s_eng_lp - s_eng_lp2) * 0.25f;
    /* a buzz under it: the third harmonic of the firing, squared off */
    float buzz = sinf(s_eng_ph * 6.2831853f * 6) > 0 ? 0.25f : -0.25f;
    return (s_eng_lp2 * 2.2f + buzz) * (0.25f + 0.75f * burst) * s_eng * 9000.0f;
}

/* A shell's flight: a whistle that falls, or a rocket's hiss */
static float flight_sample(void)
{
    s_fl += (s_fl_t - s_fl) * 0.002f;
    if (s_fl < 0.002f) return 0;
    if (s_fl_hz <= 0) s_fl_hz = s_fl_hz_t;
    s_fl_hz += (s_fl_hz_t - s_fl_hz) * 0.0008f;
    if (s_fl_kind == 0) {
        s_fl_ph += s_fl_hz / RATE;
        if (s_fl_ph >= 1) s_fl_ph -= 1;
        float w = sinf(s_fl_ph * 6.2831853f) + rndf() * 0.08f;
        return w * s_fl * 2600.0f;
    }
    float n = rndf();
    s_fl_lp += (n - s_fl_lp) * 0.55f;
    float h = s_fl_lp - s_fl_hp;
    s_fl_hp += (s_fl_lp - s_fl_hp) * 0.08f;
    return h * s_fl * 7000.0f;
}

/* --------------------------------------------------------------------------
 * The speaker
 * -------------------------------------------------------------------------- */

void tq_snd_open(void)
{
    if (s_open || !s_on) return;
    for (int i = 0; i < 256; i++) s_sin[i] = (int16_t)(sinf((float)i * (6.2831853f / 256.0f)) * 32767.0f);
    memset(s_v, 0, sizeof s_v);
    s_eng = s_eng_t = 0;
    s_fl = s_fl_t = 0;
    s_open = aos_hal_spk_open(RATE);
    if (!s_open) aos_hal_log("tanques", "no speaker: beeps");
}

void tq_snd_close(void)
{
    if (s_open) aos_hal_spk_close();
    s_open = false;
    s_eng_t = s_fl_t = 0;
}

void tq_snd_enable(bool on)
{
    s_on = on;
    if (on) tq_snd_open();
    else tq_snd_close();
}

bool tq_snd_enabled(void)
{
    return s_on;
}

void tq_snd_tick(void)
{
    if (!s_open) return;
    int need = TARGET - aos_hal_spk_queued();
    if (need <= 0) return;
    static int16_t buf[TARGET];
    if (need > TARGET) need = TARGET;
    for (int i = 0; i < need; i++) {
        int32_t mix = (int32_t)(engine_sample() + flight_sample());
        for (int k = 0; k < VOICES; k++) {
            voice_t *v = &s_v[k];
            if (v->kind == V_OFF) continue;
            if (v->t < 0) {
                v->t++;
                continue;
            }
            mix += voice_sample(v);
        }
        if (mix > 32767) mix = 32767;
        if (mix < -32767) mix = -32767;
        buf[i] = (int16_t)mix;
    }
    aos_hal_spk_write(buf, need);
}
