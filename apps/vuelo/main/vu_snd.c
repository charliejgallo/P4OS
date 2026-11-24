/*
 * PLANEO - the sound (see vu_snd.h)
 */
#include "vu_snd.h"

#include "aos_hal.h"

#include <math.h>
#include <string.h>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#define RATE    16000
#define TARGET  1600            /* samples kept queued: 100 ms */
#define BLOCK   256
#define VOICES  8

typedef struct {
    bool  on;
    int   kind;                 /* 0 tone, 1 noise */
    float f0, f1;               /* Hz, from and to */
    float amp, ph, lp;
    int   t, dur, delay;
} voice_t;

static bool    s_open, s_on = true;
static voice_t s_v[VOICES];
static float   s_thr, s_v_ms, s_fly;
static float   s_eng_ph, s_beat_ph, s_eng_lp, s_eng_lp2, s_eng_f, s_eng_a;
static float   s_wind_lp, s_wind_lp2, s_wind_a;
static uint32_t s_rng = 0x2545F491u;

static inline float minf(float a, float b)
{
    return a < b ? a : b;       /* fminf is not in the firmware's table */
}

static inline float noise(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(int32_t)s_rng * (1.0f / 2147483648.0f);
}

void vs_enable(bool on)
{
    s_on = on;
}

void vs_open(void)
{
    if (s_open) return;
    s_open = aos_hal_spk_open(RATE);
}

void vs_close(void)
{
    if (!s_open) return;
    aos_hal_spk_close();
    s_open = false;
    memset(s_v, 0, sizeof s_v);
}

void vs_set(float thr, float speed, bool flying)
{
    s_thr = thr;
    s_v_ms = speed;
    s_fly = flying ? 1.0f : 0.0f;
}

static void voice(int kind, float f0, float f1, float amp, int ms, int delay_ms)
{
    int k = 0;
    for (int i = 0; i < VOICES; i++)
        if (!s_v[i].on) {
            k = i;
            break;
        }
    voice_t *v = &s_v[k];
    memset(v, 0, sizeof *v);
    v->on = true;
    v->kind = kind;
    v->f0 = f0;
    v->f1 = f1;
    v->amp = amp;
    v->dur = ms * RATE / 1000;
    v->delay = delay_ms * RATE / 1000;
}

void vs_cue(int id)
{
    if (!s_on) return;
    switch (id) {
    case VS_RING:
        voice(0, 988, 988, 0.22f, 120, 0);
        voice(0, 1480, 1480, 0.20f, 220, 70);
        break;
    case VS_PICK:
        voice(0, 660, 660, 0.2f, 110, 0);
        voice(0, 880, 880, 0.2f, 110, 90);
        voice(0, 1320, 1320, 0.2f, 220, 180);
        break;
    case VS_DROP:
        voice(0, 784, 784, 0.18f, 300, 0);
        voice(0, 988, 988, 0.16f, 300, 0);
        voice(0, 1175, 1175, 0.16f, 400, 120);
        break;
    case VS_COUNT: voice(0, 880, 880, 0.22f, 140, 0); break;
    case VS_GO: voice(0, 1320, 1320, 0.26f, 380, 0); break;
    case VS_FINISH:
        voice(0, 784, 784, 0.2f, 160, 0);
        voice(0, 988, 988, 0.2f, 160, 150);
        voice(0, 1175, 1175, 0.2f, 160, 300);
        voice(0, 1568, 1568, 0.22f, 500, 450);
        break;
    case VS_LOST:
        voice(0, 660, 600, 0.2f, 250, 0);
        voice(0, 520, 440, 0.2f, 450, 220);
        break;
    case VS_CRASH:
        voice(1, 900, 120, 0.55f, 900, 0);
        voice(0, 110, 50, 0.3f, 600, 0);
        break;
    case VS_CLICK: voice(0, 1800, 1800, 0.12f, 25, 0); break;
    case VS_STALL:
        voice(0, 1046, 1046, 0.14f, 90, 0);
        voice(0, 1046, 1046, 0.14f, 90, 160);
        break;
    default: break;
    }
}

static void render(int16_t *out, int n)
{
    /* the engine's pitch and loudness glide to where they should be */
    float want_f = 34.0f + 52.0f * s_thr + s_v_ms * 0.22f;
    float want_a = s_fly * (0.12f + 0.16f * s_thr);
    float wind_a = s_fly * minf(1.0f, s_v_ms * s_v_ms / 9000.0f) * 0.30f;
    float wind_k = 0.02f + minf(0.25f, s_v_ms * 0.0022f);
    for (int i = 0; i < n; i++) {
        s_eng_f += (want_f - s_eng_f) * 0.0006f;
        s_eng_a += (want_a - s_eng_a) * 0.0008f;
        s_wind_a += (wind_a - s_wind_a) * 0.0006f;
        /* the engine: a narrow pulse, filtered, beating at the blades' rate */
        s_eng_ph += s_eng_f / RATE;
        if (s_eng_ph >= 1) s_eng_ph -= 1;
        s_beat_ph += s_eng_f * 2.0f / RATE;
        if (s_beat_ph >= 1) s_beat_ph -= 1;
        float pulse = s_eng_ph < 0.18f ? 1.0f : -0.22f;
        pulse += noise() * 0.25f;
        s_eng_lp += (pulse - s_eng_lp) * 0.09f;
        s_eng_lp2 += (s_eng_lp - s_eng_lp2) * 0.09f;
        float beat = 0.75f + 0.25f * sinf(s_beat_ph * 6.2831853f);
        float x = s_eng_lp2 * s_eng_a * beat * 1.6f;
        /* the wind */
        float nz = noise();
        s_wind_lp += (nz - s_wind_lp) * wind_k;
        s_wind_lp2 += (s_wind_lp - s_wind_lp2) * wind_k;
        x += (s_wind_lp - s_wind_lp2 * 0.6f) * s_wind_a * 2.2f;
        /* the cues */
        for (int k = 0; k < VOICES; k++) {
            voice_t *v = &s_v[k];
            if (!v->on) continue;
            if (v->delay > 0) {
                v->delay--;
                continue;
            }
            float t = (float)v->t / (float)v->dur;
            float f = v->f0 + (v->f1 - v->f0) * t;
            float env = (1.0f - t);
            env *= env;
            if (v->t < 80) env *= (float)v->t / 80.0f;
            float s;
            if (v->kind == 1) {
                v->lp += (noise() - v->lp) * (f / 4000.0f);
                s = v->lp * 2.5f;
            } else {
                v->ph += f / RATE;
                if (v->ph >= 1) v->ph -= 1;
                s = sinf(v->ph * 6.2831853f);
            }
            x += s * v->amp * env;
            if (++v->t >= v->dur) v->on = false;
        }
        if (x > 1) x = 1;
        if (x < -1) x = -1;
        out[i] = (int16_t)(x * 26000.0f);
    }
}

void vs_tick(void)
{
    if (!s_open) return;
    static int16_t buf[BLOCK];
    for (int guard = 0; guard < 16; guard++) {
        int q = aos_hal_spk_queued();
        if (q >= TARGET) break;
        if (!s_on) {
            memset(buf, 0, sizeof buf);
        } else {
            render(buf, BLOCK);
        }
        if (aos_hal_spk_write(buf, BLOCK) <= 0) break;
    }
}
