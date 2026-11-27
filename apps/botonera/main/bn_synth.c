/*
 * Botonera - the example sounds, made here.
 *
 * They are synthesised by this code the first time the app opens (and again
 * if one goes missing from the folder while the grid still names it): no
 * recordings of anybody, nothing with an owner, nothing to download. Each is
 * a few oscillators, noise and envelopes, written as a 16-bit mono WAV at
 * 22.05 kHz: all sixteen are ~1.7 MB on the card and take well under a
 * second to make on the board.
 *
 * The loops (siren, clock, beat, rain, wind, heart) are built to join their
 * end to their start: whole cycles of every oscillator, and the noisy ones
 * with their tail crossfaded into their head. Their lengths are multiples of
 * 20 ms, so they also come out whole at the mixer's 32 kHz (441 -> 640).
 */
#include "bn.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SR      22050
#define TAU     6.2831853f

const bn_example_t bn_examples[] = {
    /* page 1: Efectos */
    { "aplausos.wav", N_("Aplausos"), "tap",     0x30D158, BN_MODE_CUT,  0 },
    { "bocina.wav",   N_("Bocina"),   "volume",  0xFF453A, BN_MODE_OVER, 0 },
    { "redoble.wav",  N_("Redoble"),  "fire",    0xFF9F0A, BN_MODE_CUT,  0 },
    { "tada.wav",     N_("Tadá"),     "trophy",  0xFFD60A, BN_MODE_CUT,  0 },
    { "campana.wav",  N_("Campana"),  "bell",    0x40C8E0, BN_MODE_OVER, 0 },
    { "correcto.wav", N_("Correcto"), "check",   0x63E6BE, BN_MODE_OVER, 0 },
    { "error.wav",    N_("Error"),    "close",   0xFF375F, BN_MODE_OVER, 0 },
    { "laser.wav",    N_("Láser"),    "bolt",    0xBF5AF2, BN_MODE_OVER, 0 },
    { "boing.wav",    N_("Boing"),    "star",    0x0A84FF, BN_MODE_OVER, 0 },
    { "trombon.wav",  N_("Trombón triste"), "note", 0x5E5CE6, BN_MODE_CUT, 0 },
    { "sirena.wav",   N_("Sirena"),   "alarm",   0xFF453A, BN_MODE_CUT,  1 },
    { "tictac.wav",   N_("Tic tac"),  "clock",   0x8E8E93, BN_MODE_CUT,  1 },
    /* page 2: Ambiente */
    { "ritmo.wav",    N_("Ritmo"),    "wave",    0xFF9F0A, BN_MODE_CUT,  1 },
    { "lluvia.wav",   N_("Lluvia"),   "rain",    0x0A84FF, BN_MODE_CUT,  1 },
    { "viento.wav",   N_("Viento"),   "wind",    0x40C8E0, BN_MODE_CUT,  1 },
    { "latido.wav",   N_("Latido"),   "heart",   0xFF375F, BN_MODE_CUT,  1 },
};
const int bn_n_examples = (int)(sizeof bn_examples / sizeof bn_examples[0]);

/* ---- tools ---- */

static uint32_t rng = 0x2545F491u;

static float noise(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (float)(int32_t)rng * (1.0f / 2147483648.0f);
}

static float fminf_local(float a, float b) { return a < b ? a : b; }

static float frand(void) { return noise() * 0.5f + 0.5f; }

static float osc_sin(float *ph, float f)
{
    float v = sinf(TAU * *ph);
    *ph += f / SR;
    if (*ph >= 1.0f) *ph -= 1.0f;
    return v;
}

static float osc_saw(float *ph, float f)
{
    float v = 2.0f * *ph - 1.0f;
    *ph += f / SR;
    if (*ph >= 1.0f) *ph -= 1.0f;
    return v;
}

static float osc_sq(float *ph, float f)
{
    float v = *ph < 0.5f ? 1.0f : -1.0f;
    *ph += f / SR;
    if (*ph >= 1.0f) *ph -= 1.0f;
    return v;
}

/* a state-variable filter, Chamberlin's: band and low outputs */
typedef struct { float lo, bp; } svf_t;

static float svf_bp(svf_t *s, float in, float fc, float q)
{
    float f = 2.0f * sinf(3.14159265f * fc / SR);
    if (f > 1.4f) f = 1.4f;
    s->lo += f * s->bp;
    float hi = in - s->lo - s->bp / q;
    s->bp += f * hi;
    return s->bp;
}

static float svf_lp(svf_t *s, float in, float fc, float q)
{
    svf_bp(s, in, fc, q);
    return s->lo;
}

/* where a sound placed at 't' seconds starts in the buffer: never before
 * its first sample (a hit nudged early at t = 0 wrote in front of the
 * buffer and broke the heap on the board, 2026-11-27) */
static int at_index(float t)
{
    int i = (int)(t * SR);
    return i < 0 ? 0 : i;
}

static float env_ad(float t, float a, float d)
{
    if (t < 0.0f) return 0.0f;
    if (t < a) return t / a;
    return expf(-(t - a) / d);
}

/* adds a decaying drum or click at 'at' seconds */
static void add_kick(float *o, int n, float at, float g)
{
    float ph = 0.0f;
    int i0 = at_index(at);
    for (int i = i0; i < n && i < i0 + SR / 2; i++) {
        float t = (float)(i - i0) / SR;
        float f = 48.0f + 110.0f * expf(-t / 0.035f);
        o[i] += g * osc_sin(&ph, f) * expf(-t / 0.13f);
    }
}

static void add_snare(float *o, int n, float at, float g, float decay)
{
    float ph = 0.0f;
    svf_t s = { 0 };
    int i0 = at_index(at);
    for (int i = i0; i < n && i < i0 + SR / 2; i++) {
        float t = (float)(i - i0) / SR;
        float tone = osc_sin(&ph, 185.0f) * expf(-t / 0.03f) * 0.5f;
        float nz = svf_bp(&s, noise(), 3200.0f, 0.7f) * expf(-t / decay);
        o[i] += g * (tone + nz);
    }
}

static void add_hat(float *o, int n, float at, float g)
{
    float prev = 0.0f;
    int i0 = at_index(at);
    for (int i = i0; i < n && i < i0 + SR / 10; i++) {
        float t = (float)(i - i0) / SR;
        float w = noise();
        float hp = w - prev;            /* a crude high pass */
        prev = w;
        o[i] += g * hp * expf(-t / 0.018f);
    }
}

static void add_click(float *o, int n, float at, float f, float g)
{
    float ph = 0.0f;
    svf_t s = { 0 };
    int i0 = at_index(at);
    for (int i = i0; i < n && i < i0 + SR / 8; i++) {
        float t = (float)(i - i0) / SR;
        float v = osc_sin(&ph, f) * expf(-t / 0.012f) + svf_bp(&s, noise(), f * 1.5f, 3.0f) * expf(-t / 0.004f);
        o[i] += g * v;
    }
}

/* the tail of a loop over its head: 'n' samples to keep, 'x' more made */
static void loop_join(float *o, int n, int x)
{
    for (int i = 0; i < x; i++) {
        float w = (float)i / (float)x;
        o[i] = o[i] * w + o[n + i] * (1.0f - w);
    }
}

/* ---- the sounds ---- */

static int s_aplausos(float *o, int n)
{
    /* ~35 hands a second, each a 12 ms burst of band-passed noise at its
     * own pitch, coming in and dying away */
    for (int c = 0; c < 110; c++) {
        float at = frand() * 2.3f;
        float dens = at < 0.25f ? at / 0.25f : at > 1.6f ? 1.0f - (at - 1.6f) / 0.7f : 1.0f;
        if (frand() > dens) continue;
        float fc = 900.0f + frand() * 1600.0f, g = 0.4f + frand() * 0.6f;
        svf_t s = { 0 };
        int i0 = at_index(at);
        for (int i = i0; i < n && i < i0 + SR / 15; i++) {
            float t = (float)(i - i0) / SR;
            o[i] += g * svf_bp(&s, noise(), fc, 1.2f) * env_ad(t, 0.001f, 0.012f);
        }
    }
    return n;
}

static int s_bocina(float *o, int n)
{
    static const float on[][2] = { { 0.0f, 0.26f }, { 0.34f, 0.60f }, { 0.68f, 1.36f } };
    float p1 = 0, p2 = 0, p3 = 0, vib = 0;
    svf_t s = { 0 };
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR, a = 0.0f;
        for (int k = 0; k < 3; k++) {
            float ta = t - on[k][0], tb = on[k][1] - t;
            if (ta >= 0 && tb >= 0) a = fminf_local(ta / 0.015f, tb / 0.03f);
        }
        if (a > 1.0f) a = 1.0f;
        float v = osc_sin(&vib, 6.0f) * 2.0f;
        float x = osc_saw(&p1, 466.0f + v) + osc_saw(&p2, 470.0f + v) + 0.8f * osc_saw(&p3, 587.0f + v);
        x = svf_lp(&s, x, 2600.0f, 0.9f);
        x = x * 1.6f / (1.0f + fabsf(x * 1.6f));          /* the brass's grit */
        o[i] = x * a;
    }
    return n;
}

static int s_redoble(float *o, int n)
{
    /* a roll that grows for 1.7 s, then a crash and a kick */
    for (float t = 0.0f; t < 1.72f; t += 0.034f) {
        float g = 0.15f + 0.6f * t / 1.72f;
        add_snare(o, n, t + frand() * 0.003f, g * (0.8f + 0.2f * frand()), 0.05f);
    }
    add_kick(o, n, 1.76f, 1.2f);
    add_snare(o, n, 1.76f, 0.9f, 0.09f);
    /* the crash: noise through a few metallic bands */
    svf_t b[4] = { { 0 } };
    static const float fb[4] = { 3100.0f, 4700.0f, 6300.0f, 8200.0f };
    int i0 = at_index(1.76f);
    for (int i = i0; i < n; i++) {
        float t = (float)(i - i0) / SR, w = noise(), x = 0.0f;
        for (int k = 0; k < 4; k++) x += svf_bp(&b[k], w, fb[k], 2.0f);
        o[i] += 0.5f * x * env_ad(t, 0.002f, 0.32f);
    }
    return n;
}

static void brass_note(float *o, int n, float at, float dur, float f, float g)
{
    float p1 = 0, p2 = 0;
    svf_t s = { 0 };
    int i0 = at_index(at), i1 = (int)((at + dur + 0.25f) * SR);
    for (int i = i0; i < n && i < i1; i++) {
        float t = (float)(i - i0) / SR;
        float a = t < 0.02f ? t / 0.02f : t < dur ? 1.0f - 0.25f * (t - 0.02f) / dur : 0.75f * expf(-(t - dur) / 0.07f);
        float x = osc_saw(&p1, f) + 0.6f * osc_saw(&p2, f * 1.003f);
        x = svf_lp(&s, x, 900.0f + 2800.0f * env_ad(t, 0.03f, 0.25f), 1.1f);
        o[i] += g * x * a;
    }
}

static int s_tada(float *o, int n)
{
    brass_note(o, n, 0.00f, 0.12f, 523.25f, 0.5f);
    brass_note(o, n, 0.14f, 0.12f, 659.25f, 0.5f);
    brass_note(o, n, 0.28f, 0.12f, 783.99f, 0.5f);
    brass_note(o, n, 0.44f, 0.80f, 1046.5f, 0.45f);
    brass_note(o, n, 0.44f, 0.80f, 783.99f, 0.35f);
    brass_note(o, n, 0.44f, 0.80f, 659.25f, 0.35f);
    add_snare(o, n, 0.44f, 0.4f, 0.12f);
    return n;
}

static int s_campana(float *o, int n)
{
    static const float r[] = { 0.56f, 0.92f, 1.19f, 1.71f, 2.0f, 2.74f, 3.0f, 3.76f, 4.07f };
    static const float a[] = { 1.0f, 0.67f, 1.0f, 1.8f, 2.67f, 1.67f, 1.46f, 1.33f, 1.33f };
    static const float d[] = { 1.6f, 0.9f, 0.65f, 0.55f, 0.33f, 0.28f, 0.2f, 0.15f, 0.13f };
    for (int k = 0; k < 9; k++) {
        float ph = 0.0f, f = 880.0f * r[k];
        for (int i = 0; i < n; i++) {
            float t = (float)i / SR;
            o[i] += a[k] * 0.15f * osc_sin(&ph, f) * env_ad(t, 0.001f, d[k] * 1.4f);
        }
    }
    return n;
}

static void ding(float *o, int n, float at, float f, float g, float d)
{
    float p1 = 0, p2 = 0;
    int i0 = at_index(at);
    for (int i = i0; i < n; i++) {
        float t = (float)(i - i0) / SR;
        o[i] += g * (osc_sin(&p1, f) + 0.3f * osc_sin(&p2, f * 2.0f)) * env_ad(t, 0.002f, d);
    }
}

static int s_correcto(float *o, int n)
{
    ding(o, n, 0.00f, 1318.5f, 0.6f, 0.12f);
    ding(o, n, 0.11f, 1760.0f, 0.6f, 0.25f);
    return n;
}

static int s_error(float *o, int n)
{
    float p1 = 0, p2 = 0;
    svf_t s = { 0 };
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float a = (t < 0.22f || (t > 0.30f && t < 0.62f)) ? 1.0f : 0.0f;
        float x = osc_sq(&p1, 110.0f) + osc_sq(&p2, 116.0f);
        o[i] = svf_lp(&s, x * a, 1800.0f, 0.8f);
    }
    return n;
}

static int s_laser(float *o, int n)
{
    float ph = 0.0f, ph2 = 0.0f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float f = 3000.0f * expf(-t * 9.0f) + 120.0f;
        o[i] = (0.7f * osc_sq(&ph, f) + 0.5f * osc_sin(&ph2, f * 0.5f)) * env_ad(t, 0.002f, 0.12f);
    }
    return n;
}

static int s_boing(float *o, int n)
{
    float ph = 0.0f, lfo = 0.0f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float w = osc_sin(&lfo, 11.0f) * 70.0f * expf(-t * 3.0f);
        float f = 140.0f + 220.0f * (1.0f - expf(-t * 7.0f)) + w;
        float x = osc_sin(&ph, f);
        x = x + 0.25f * x * x * x;
        o[i] = x * env_ad(t, 0.004f, 0.28f);
    }
    return n;
}

static int s_trombon(float *o, int n)
{
    /* four steps down, the last one long and wobbling */
    static const float f[4] = { 233.08f, 220.0f, 207.65f, 196.0f };
    static const float at[4] = { 0.0f, 0.42f, 0.84f, 1.26f };
    static const float dur[4] = { 0.36f, 0.36f, 0.36f, 0.86f };
    for (int k = 0; k < 4; k++) {
        float p1 = 0, p2 = 0, lfo = 0;
        svf_t s = { 0 };
        int i0 = at_index(at[k]), i1 = (int)((at[k] + dur[k] + 0.08f) * SR);
        for (int i = i0; i < n && i < i1; i++) {
            float t = (float)(i - i0) / SR;
            float vib = k == 3 ? osc_sin(&lfo, 5.5f) * 5.0f * (t / dur[k]) : 0.0f;
            float bend = k == 3 ? -8.0f * t : 0.0f;
            float a = t < 0.03f ? t / 0.03f : t < dur[k] ? 1.0f : expf(-(t - dur[k]) / 0.03f);
            float x = osc_saw(&p1, f[k] + vib + bend) + 0.5f * osc_saw(&p2, (f[k] + vib + bend) * 1.004f);
            /* the mute: the filter opens and closes on each note, "wah" */
            float wah = 500.0f + 1300.0f * sinf(3.14159f * fminf_local(t / dur[k], 1.0f));
            o[i] += 0.6f * svf_lp(&s, x, wah, 2.0f) * a;
        }
    }
    return n;
}

static int s_sirena(float *o, int n)
{
    /* up and down every 1.5 s: two whole sweeps and, the mean being 900 Hz,
     * exactly 2700 cycles in 3 s, so the loop joins in phase */
    float ph = 0.0f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float f = 900.0f + 300.0f * sinf(TAU * t / 1.5f);
        float x = 2.0f * fabsf(2.0f * ph - 1.0f) - 1.0f;      /* triangle */
        ph += f / SR;
        if (ph >= 1.0f) ph -= 1.0f;
        o[i] = x * 0.8f + 0.2f * sinf(TAU * ph * 2.0f);
    }
    return n;
}

static int s_tictac(float *o, int n)
{
    for (int k = 0; k < 4; k++) add_click(o, n, 0.5f * (float)k, k & 1 ? 1500.0f : 2100.0f, 0.8f);
    return n;
}

static int s_ritmo(float *o, int n)
{
    /* two bars at 100 a minute: kick on 1 and 3 (and the "and" of 3 in the
     * second bar), snare on 2 and 4, hats on the eighths */
    const float b = 0.6f;
    for (int bar = 0; bar < 2; bar++) {
        float t0 = (float)bar * 4.0f * b;
        add_kick(o, n, t0, 1.0f);
        add_kick(o, n, t0 + 2.0f * b, 1.0f);
        if (bar) add_kick(o, n, t0 + 2.5f * b, 0.7f);
        add_snare(o, n, t0 + b, 0.7f, 0.07f);
        add_snare(o, n, t0 + 3.0f * b, 0.7f, 0.07f);
        for (int e = 0; e < 8; e++) add_hat(o, n, t0 + (float)e * b * 0.5f, e & 1 ? 0.25f : 0.4f);
    }
    return n;
}

static int s_lluvia(float *o, int n)
{
    int x = SR / 2, m = n + x;
    float lp = 0.0f, lp2 = 0.0f;
    for (int i = 0; i < m; i++) {
        float w = noise();
        lp += 0.25f * (w - lp);
        lp2 += 0.02f * (w - lp2);
        o[i] = 0.35f * lp + 0.8f * lp2;
    }
    for (int d = 0; d < 180; d++) {
        float at = frand() * ((float)m / SR - 0.05f);
        add_click(o, m, at, 2500.0f + 3500.0f * frand(), 0.08f + 0.12f * frand());
    }
    loop_join(o, n, x);
    return n;
}

static int s_viento(float *o, int n)
{
    int x = SR, m = n + x;
    svf_t s = { 0 };
    float l1 = 0.0f, l2 = 0.0f;
    for (int i = 0; i < m; i++) {
        float a = osc_sin(&l1, 1.0f / 6.0f), c = osc_sin(&l2, 1.0f / 2.0f);
        float fc = 500.0f + 350.0f * a + 120.0f * c;
        float g = 0.6f + 0.3f * a + 0.1f * c;
        o[i] = g * svf_bp(&s, noise(), fc, 4.0f);
    }
    loop_join(o, n, x);
    return n;
}

static int s_latido(float *o, int n)
{
    for (int k = 0; k < 2; k++) {
        float t0 = 0.8f * (float)k;
        float ph = 0.0f, ph2 = 0.0f;
        int i0 = at_index(t0);
        for (int i = i0; i < n && i < i0 + (int)(0.6f * SR); i++) {
            float t = (float)(i - i0) / SR;
            float lub = osc_sin(&ph, 62.0f - 20.0f * t) * env_ad(t, 0.01f, 0.07f);
            float t2 = t - 0.22f;
            float dub = t2 > 0 ? osc_sin(&ph2, 55.0f - 15.0f * t2) * 0.8f * env_ad(t2, 0.01f, 0.06f) : 0.0f;
            o[i] += lub + dub;
        }
    }
    return n;
}

typedef int (*gen_fn)(float *o, int n);

static const struct {
    gen_fn fn;
    float  secs;        /* a multiple of 20 ms */
    float  peak;        /* how loud it ends up */
} GEN[] = {
    { s_aplausos, 2.40f, 0.80f }, { s_bocina,   1.40f, 0.85f }, { s_redoble,  2.60f, 0.85f },
    { s_tada,     1.60f, 0.80f }, { s_campana,  2.60f, 0.75f }, { s_correcto, 0.80f, 0.70f },
    { s_error,    0.70f, 0.70f }, { s_laser,    0.50f, 0.70f }, { s_boing,    0.90f, 0.75f },
    { s_trombon,  2.20f, 0.80f }, { s_sirena,   3.00f, 0.60f }, { s_tictac,   2.00f, 0.70f },
    { s_ritmo,    4.80f, 0.80f }, { s_lluvia,   5.00f, 0.45f }, { s_viento,   6.00f, 0.50f },
    { s_latido,   1.60f, 0.85f },
};

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static bool write_wav(const char *path, const float *o, int n, float peak)
{
    float mx = 0.0f;
    for (int i = 0; i < n; i++) if (fabsf(o[i]) > mx) mx = fabsf(o[i]);
    float g = mx > 0.0f ? peak * 32767.0f / mx : 0.0f;
    char tmp[340];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + (uint32_t)n * 2);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);
    put16(h + 22, 1);
    put32(h + 24, SR);
    put32(h + 28, SR * 2);
    put16(h + 32, 2);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, (uint32_t)n * 2);
    bool ok = fwrite(h, 1, 44, f) == 44;
    int16_t buf[1024];
    for (int i = 0; i < n && ok; i += 1024) {
        int k = n - i < 1024 ? n - i : 1024;
        for (int j = 0; j < k; j++) {
            float v = o[i + j] * g;
            buf[j] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
        }
        ok = fwrite(buf, 2, (size_t)k, f) == (size_t)k;
    }
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(tmp);
        return false;
    }
    remove(path);
    return rename(tmp, path) == 0;
}

int bn_synth_missing(void)
{
    struct stat st;
    mkdir(bn_dir(), 0777);
    int made = 0;
    /* the longest, plus the second of crossfade the wind makes */
    float *o = bn_alloc((size_t)(7 * SR) * sizeof(float));
    if (!o) return 0;
    for (int k = 0; k < bn_n_examples; k++) {
        char path[320];
        snprintf(path, sizeof path, "%s/%s", bn_dir(), bn_examples[k].file);
        if (stat(path, &st) == 0) continue;
        int n = (int)(GEN[k].secs * SR + 0.5f);
        memset(o, 0, (size_t)(7 * SR) * sizeof(float));
        rng = 0x2545F491u + (uint32_t)k * 7919u;
        n = GEN[k].fn(o, n);
        if (write_wav(path, o, n, GEN[k].peak)) made++;
        else aos_hal_log(BN_TAG, "could not write %s", path);
    }
    bn_free(o);
    if (made) aos_hal_log(BN_TAG, "made %d example sounds in %s", made, bn_dir());
    return made;
}
