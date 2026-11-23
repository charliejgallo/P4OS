/*
 * DERIVA - the sound, all of it synthesised when the app opens (no
 * pack): the effects (a falling chirp for the gun, filtered noise and a
 * thump for the rocks, inharmonic partials for metal, a rising sweep for
 * the jump...), the engine's hum (a loop of brown noise that follows how
 * hard the ship pushes) and two pieces of music made of a filtered saw
 * bass, a square arpeggio and soft drums: one for the sectors and the
 * menus, a faster one for the bosses. 16 kHz mono on the streaming
 * speaker, ten voices over the music, pumped from LVGL's thread.
 *
 * The simulator's speaker swallows what it gets; AST_SIM_TAP=<file.wav>
 * writes what would be heard, in real time, to listen to it afterwards.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif

#include "ast_app.h"

#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE    16000
#define TARGET  1800
#define CHUNK   320
#define VOICES  10
#define PI      3.14159265f
#define TAU     6.28318531f

typedef struct {
    const int16_t *data;
    uint32_t len, pos;
    int      vol;
    int      snd;
} voice_t;

static struct {
    bool     open, sfx_on, music_on, paused;
    int16_t *snd[SND_N];
    uint32_t snd_len[SND_N];
    int16_t *hum;
    uint32_t hum_len, hum_pos;
    int16_t *mus[2];
    uint32_t mus_len[2], mus_pos;
    int      mus_want, mus_now;
    float    mus_gain, hum_now, hum_want;
    volatile bool mus_ready;
    voice_t  v[VOICES];
    uint32_t rnd;
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    FILE    *tap;
    uint32_t tap_n;
    uint64_t tap_t0;
#endif
} A;

static float nz(void)
{
    return ag_rand01(&A.rnd) * 2.0f - 1.0f;
}

static int16_t *buf(uint32_t n)
{
    return (int16_t *)ast_calloc(sizeof(int16_t) * n);
}

static int16_t clip16(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 30000.0f);
}

/* a one-pole low pass */
typedef struct {
    float y;
} lp_t;

static float lp(lp_t *f, float x, float cut)
{
    /* 1 - e^-w, near enough and without expf per sample */
    float w = TAU * cut / RATE;
    float k = w / (1.0f + w);
    f->y += (x - f->y) * k;
    return f->y;
}

/* ---- the effects ----------------------------------------------------------------------------- */

static void make(int id, float secs, float (*fn)(float t, float len, int i, void *st), void *st)
{
    uint32_t n = (uint32_t)(secs * RATE);
    int16_t *b = buf(n);
    if (!b) return;
    for (uint32_t i = 0; i < n; i++) {
        float t = (float)i / RATE;
        float v = fn(t, secs, (int)i, st);
        /* a few milliseconds in and out, so nothing clicks */
        float e = 1.0f;
        if (i < 32) e = (float)i / 32.0f;
        if (n - i < 64) e *= (float)(n - i) / 64.0f;
        b[i] = clip16(v * e);
    }
    A.snd[id] = b;
    A.snd_len[id] = n;
}

typedef struct {
    float ph, ph2, ph3;
    lp_t f1, f2, f3;
    float a, b;
} st_t;

static float f_shot(float t, float len, int i, void *p)
{
    st_t *s = (st_t *)p;
    float f = 420.0f + 1500.0f * expf(-t * 38.0f);
    s->ph += f / RATE;
    float sq = fmodf(s->ph, 1.0f) < 0.32f ? 1.0f : -1.0f;
    float v = sq * 0.45f + sinf(s->ph * TAU) * 0.4f;
    return v * expf(-t * 26.0f) * 0.75f;
    (void)len;
    (void)i;
}

static float f_shot3(float t, float len, int i, void *p)
{
    st_t *s = (st_t *)p;
    float f = 380.0f + 1300.0f * expf(-t * 30.0f);
    s->ph += f / RATE;
    s->ph2 += f * 1.26f / RATE;
    s->ph3 += f * 1.5f / RATE;
    float v = sinf(s->ph * TAU) + 0.7f * (fmodf(s->ph2, 1.0f) < 0.4f ? 1.0f : -1.0f) * 0.6f + 0.6f * sinf(s->ph3 * TAU);
    return v * expf(-t * 22.0f) * 0.42f;
    (void)len;
    (void)i;
}

static float f_beam(float t, float len, int i, void *p)
{
    st_t *s = (st_t *)p;
    s->ph += 110.0f / RATE;
    s->ph2 += (220.0f + 30.0f * sinf(t * 90.0f)) / RATE;
    float saw = fmodf(s->ph, 1.0f) * 2.0f - 1.0f;
    float v = saw * 0.4f + sinf(s->ph2 * TAU) * 0.35f + lp(&s->f1, nz(), 3000) * 0.3f;
    return v * 0.55f;
    (void)len;
    (void)i;
}

static float f_boom(float t, float len, int i, void *p)
{
    st_t *s = (st_t *)p;
    float cut = 200.0f + 2800.0f * expf(-t * s->a);
    float n = lp(&s->f2, lp(&s->f1, nz(), cut), cut * 1.3f);
    s->ph += (40.0f + 70.0f * expf(-t * 12.0f)) / RATE;
    float thump = sinf(s->ph * TAU) * expf(-t * 9.0f);
    float env = expf(-t * s->b);
    return (n * 2.2f + thump * 0.8f) * env;
    (void)len;
    (void)i;
}

static float f_clang(float t, float len, int i, void *p)
{
    static const float F[4] = { 620.0f, 1487.0f, 2391.0f, 3712.0f };
    static const float D[4] = { 9.0f, 12.0f, 16.0f, 22.0f };
    float v = 0;
    for (int k = 0; k < 4; k++) v += sinf(TAU * F[k] * t) * expf(-t * D[k]) / (1.0f + k * 0.4f);
    st_t *s = (st_t *)p;
    v += lp(&s->f1, nz(), 5000) * expf(-t * 80.0f) * 0.6f;
    return v * 0.5f;
    (void)len;
    (void)i;
}

static float f_blast(float t, float len, int i, void *p)
{
    st_t *s = (st_t *)p;
    float cut = 140.0f + 3200.0f * expf(-t * 5.0f);
    float n = lp(&s->f2, lp(&s->f1, nz(), cut), cut);
    float crackle = (ag_rand01(&A.rnd) < 0.012f * expf(-t * 3.0f)) ? nz() * 1.5f : 0;
    s->ph += (32.0f + 60.0f * expf(-t * 6.0f)) / RATE;
    float thump = sinf(s->ph * TAU) * expf(-t * 5.0f);
    return (n * 2.6f + crackle + thump) * expf(-t * 2.8f) * 0.9f;
    (void)len;
    (void)i;
}

static float f_stick(float t, float len, int i, void *p)
{
    st_t *s = (st_t *)p;
    s->ph += (150.0f - 70.0f * t / len) / RATE;
    float wob = 0.6f + 0.4f * sinf(TAU * 28.0f * t);
    float v = sinf(s->ph * TAU) * wob * 0.8f + lp(&s->f1, nz(), 900) * 1.5f * expf(-t * 18.0f);
    return v * expf(-t * 7.0f) * 0.8f;
    (void)i;
}

static float f_jump(float t, float len, int i, void *p)
{
    st_t *s = (st_t *)p;
    float f = 220.0f * powf(12.0f, t / len);
    s->ph += f / RATE;
    s->ph2 += (f * 1.5f + 40.0f) / RATE;
    float v = sinf(s->ph * TAU) * 0.6f + sinf(s->ph2 * TAU) * sinf(TAU * 33.0f * t) * 0.35f;
    float env = t < 0.04f ? t / 0.04f : expf(-(t - 0.04f) * 6.0f);
    return v * env * 0.7f;
    (void)i;
}

static float s_note[128];

static float note(float n)
{
    int i = (int)n;
    if (i < 0) i = 0;
    if (i > 127) i = 127;
    if (s_note[i] == 0) s_note[i] = 440.0f * powf(2.0f, ((float)i - 69.0f) / 12.0f);
    return s_note[i];
}

/* a few notes, one after another, each with its own fall */
static float tune(float t, const float *notes, int n, float step, float decay, float wave)
{
    float v = 0;
    for (int k = 0; k < n; k++) {
        float t0 = (float)k * step;
        if (t < t0) break;
        float tt = t - t0, f = note(notes[k]);
        float ph = fmodf(tt * f, 1.0f);
        float sq = ph < 0.5f ? 1.0f : -1.0f;
        v += (sinf(ph * TAU) * (1.0f - wave) + sq * wave * 0.5f) * expf(-tt * decay);
    }
    return v;
}

static float f_pick(float t, float len, int i, void *p)
{
    static const float N[3] = { 76, 80, 83 };
    return tune(t, N, 3, 0.06f, 9.0f, 0.4f) * 0.45f;
    (void)len;
    (void)i;
    (void)p;
}

static float f_crystal(float t, float len, int i, void *p)
{
    return (sinf(TAU * 2637.0f * t) * 0.5f + sinf(TAU * 3951.0f * t) * 0.3f) * expf(-t * 30.0f) * 0.5f;
    (void)len;
    (void)i;
    (void)p;
}

static float f_shield(float t, float len, int i, void *p)
{
    st_t *s = (st_t *)p;
    s->ph += (330.0f - 160.0f * t / len + 18.0f * sinf(TAU * 14.0f * t)) / RATE;
    return sinf(s->ph * TAU) * expf(-t * 6.0f) * 0.7f;
    (void)i;
}

static float f_die(float t, float len, int i, void *p)
{
    st_t *s = (st_t *)p;
    float cut = 160.0f + 3000.0f * expf(-t * 3.5f);
    float n = lp(&s->f2, lp(&s->f1, nz(), cut), cut);
    s->ph += (820.0f * expf(-t * 2.4f) + 50.0f) / RATE;
    float tone = (fmodf(s->ph, 1.0f) * 2.0f - 1.0f) * 0.3f;
    return (n * 2.4f + tone) * expf(-t * 2.2f) * 0.9f;
    (void)len;
    (void)i;
}

static float f_eshot(float t, float len, int i, void *p)
{
    st_t *s = (st_t *)p;
    s->ph += (950.0f - 400.0f * t / len) / RATE;
    float tri = fabsf(fmodf(s->ph, 1.0f) * 4.0f - 2.0f) - 1.0f;
    return tri * expf(-t * 20.0f) * 0.45f;
    (void)i;
}

static float f_alarm(float t, float len, int i, void *p)
{
    float f = fmodf(t, 0.5f) < 0.25f ? 660.0f : 880.0f;
    float ph = fmodf(t * f, 1.0f);
    float v = (ph < 0.5f ? 1.0f : -1.0f) * 0.3f + sinf(ph * TAU) * 0.3f;
    return v * 0.7f;
    (void)len;
    (void)i;
    (void)p;
}

static float f_wave(float t, float len, int i, void *p)
{
    static const float N[3] = { 84, 88, 91 };
    return tune(t, N, 3, 0.09f, 4.0f, 0.0f) * 0.45f;
    (void)len;
    (void)i;
    (void)p;
}

static float f_life(float t, float len, int i, void *p)
{
    static const float N[5] = { 72, 76, 79, 84, 88 };
    return tune(t, N, 5, 0.07f, 6.0f, 0.5f) * 0.4f;
    (void)len;
    (void)i;
    (void)p;
}

static float f_mine(float t, float len, int i, void *p)
{
    return sinf(TAU * 1250.0f * t) * expf(-t * 40.0f) * 0.4f;
    (void)len;
    (void)i;
    (void)p;
}

static float f_dash(float t, float len, int i, void *p)
{
    st_t *s = (st_t *)p;
    float cut = 400.0f + 3600.0f * (t / len);
    float n = lp(&s->f1, nz(), cut) - lp(&s->f2, nz(), cut * 0.3f);
    float env = sinf(PI * t / len);
    return n * env * 1.6f;
}

static float f_bhit(float t, float len, int i, void *p)
{
    st_t *s = (st_t *)p;
    return (sinf(TAU * 170.0f * t) * 0.7f + lp(&s->f1, nz(), 1800) * 1.2f) * expf(-t * 28.0f) * 0.7f;
    (void)len;
    (void)i;
}

static float f_bdie(float t, float len, int i, void *p)
{
    st_t *s = (st_t *)p;
    float cut = 120.0f + 2600.0f * expf(-t * 2.0f);
    float n = lp(&s->f2, lp(&s->f1, nz(), cut), cut);
    s->ph += (60.0f + 140.0f * expf(-t * 1.5f)) / RATE;
    float roar = (fmodf(s->ph, 1.0f) * 2.0f - 1.0f) * 0.35f * expf(-t * 1.2f);
    float crackle = (ag_rand01(&A.rnd) < 0.01f * expf(-t)) ? nz() * 1.2f : 0;
    return (n * 2.6f + roar + crackle) * expf(-t * 1.3f) * 0.9f;
    (void)len;
    (void)i;
}

static float f_ui(float t, float len, int i, void *p)
{
    return sinf(TAU * 1800.0f * t) * expf(-t * 120.0f) * 0.4f;
    (void)len;
    (void)i;
    (void)p;
}

static void make_sounds(void)
{
    st_t s;
#define MK(id, secs, fn)            \
    do {                            \
        memset(&s, 0, sizeof s);    \
        make(id, secs, fn, &s);     \
    } while (0)
    MK(SND_SHOT, 0.11f, f_shot);
    MK(SND_SHOT3, 0.13f, f_shot3);
    MK(SND_BEAM, 0.14f, f_beam);
    memset(&s, 0, sizeof s);
    s.a = 14.0f;
    s.b = 11.0f;
    make(SND_BOOM_S, 0.28f, f_boom, &s);
    memset(&s, 0, sizeof s);
    s.a = 9.0f;
    s.b = 6.5f;
    make(SND_BOOM_M, 0.5f, f_boom, &s);
    memset(&s, 0, sizeof s);
    s.a = 6.0f;
    s.b = 3.8f;
    make(SND_BOOM_L, 0.85f, f_boom, &s);
    MK(SND_CLANG, 0.3f, f_clang);
    MK(SND_BLAST, 1.1f, f_blast);
    MK(SND_STICK, 0.3f, f_stick);
    MK(SND_JUMP, 0.38f, f_jump);
    MK(SND_PICK, 0.3f, f_pick);
    MK(SND_CRYSTAL, 0.13f, f_crystal);
    MK(SND_SHIELD, 0.32f, f_shield);
    MK(SND_DIE, 1.3f, f_die);
    MK(SND_ESHOT, 0.09f, f_eshot);
    MK(SND_ALARM, 1.0f, f_alarm);
    MK(SND_WAVE, 0.6f, f_wave);
    MK(SND_LIFE, 0.65f, f_life);
    MK(SND_MINE, 0.06f, f_mine);
    MK(SND_DASH, 0.4f, f_dash);
    MK(SND_BOSS_HIT, 0.13f, f_bhit);
    MK(SND_BOSS_DIE, 2.2f, f_bdie);
    MK(SND_UI, 0.03f, f_ui);
#undef MK
    /* the engine: brown noise, low, as a loop that meets itself */
    A.hum_len = RATE / 2;
    A.hum = buf(A.hum_len);
    float *tmp = (float *)ast_malloc(sizeof(float) * A.hum_len);
    if (A.hum && tmp) {
        float b = 0;
        lp_t f = { 0 };
        for (uint32_t i = 0; i < A.hum_len; i++) {
            b = b * 0.985f + nz() * 0.06f;
            tmp[i] = lp(&f, b, 500.0f) + 0.15f * sinf(TAU * 55.0f * (float)i / RATE);
        }
        for (uint32_t i = 0; i < A.hum_len; i++) {
            /* cross-fade the ends so the loop has no seam */
            uint32_t j = (i + A.hum_len / 2) % A.hum_len;
            float w = fabsf((float)i / (float)A.hum_len * 2.0f - 1.0f);
            A.hum[i] = clip16((tmp[i] * (1.0f - w) + tmp[j] * w) * 2.2f);
        }
    }
    ast_free(tmp);
}

/* ---- the music --------------------------------------------------------------------------------- */

/* eight bars: bass (midi notes per eighth), the chord under the arpeggio */
static void make_music(int which)
{
    if (A.mus[which]) return;
    float bpm = which ? 132.0f : 104.0f;
    float beat = 60.0f / bpm;
    int bars = 8;
    uint32_t n = (uint32_t)(beat * 4.0f * (float)bars * RATE);
    int16_t *b = buf(n);
    if (!b) return;
    /* the chords: A minor, F, C, G for the sectors; D minor, B flat, C, A for the bosses */
    static const int ROOT[2][8] = { { 45, 41, 48, 43, 45, 41, 48, 40 }, { 38, 46, 48, 45, 38, 46, 43, 45 } };
    static const int MINOR[2][8] = { { 1, 0, 0, 0, 1, 0, 0, 0 }, { 1, 0, 0, 0, 1, 0, 1, 0 } };
    lp_t fb = { 0 }, fh = { 0 }, fa = { 0 };
    float ph_b = 0, ph_a = 0, ph_k = 0;
    float sixteenth = beat / 4.0f;
    for (uint32_t i = 0; i < n; i++) {
        float t = (float)i / RATE;
        int bar = (int)(t / (beat * 4.0f)) % bars;
        float in_bar = fmodf(t, beat * 4.0f);
        int six = (int)(in_bar / sixteenth);
        float ts = fmodf(in_bar, sixteenth);
        int root = ROOT[which][bar];
        /* bass: eighths on the root, an octave jump on the off-beats */
        int bn = root + ((six / 2) % 2 ? 12 : 0);
        if (which && (six % 4) == 3) bn = root + 7;
        ph_b += note((float)bn) / RATE;
        float saw = fmodf(ph_b, 1.0f) * 2.0f - 1.0f;
        float te = fmodf(in_bar, sixteenth * 2.0f);
        float bcut = 300.0f + 1700.0f * expf(-te * 14.0f);
        float bass = lp(&fb, saw, bcut) * (0.6f + 0.4f * expf(-te * 6.0f));
        /* arpeggio: the chord's notes up and down, sixteenths, two octaves up */
        int third = MINOR[which][bar] ? 3 : 4;
        static const int SEQ[8] = { 0, 1, 2, 3, 2, 1, 0, 2 };
        int deg = SEQ[six % 8];
        int an = root + 24 + (deg == 0 ? 0 : deg == 1 ? third : deg == 2 ? 7 : 12);
        ph_a += note((float)an) / RATE;
        float sq = fmodf(ph_a, 1.0f) < 0.3f ? 1.0f : -1.0f;
        float arp = lp(&fa, sq, 2600.0f) * expf(-ts * 18.0f) * 0.32f;
        /* drums: a kick on the beats, a hat on the off-sixteenths, a snare on 2 and 4 */
        float tb = fmodf(in_bar, beat);
        int beat_i = (int)(in_bar / beat);
        ph_k += (45.0f + 110.0f * expf(-tb * 30.0f)) / RATE;
        float kick = sinf(ph_k * TAU) * expf(-tb * 11.0f) * 0.9f;
        if (tb < 1.0f / RATE) ph_k = 0;
        float hat = 0;
        if (six % 2 == 1) {
            float nzv = nz();
            hat = (nzv - lp(&fh, nzv, 6000.0f)) * expf(-ts * 70.0f) * 0.22f;
        }
        float snare = 0;
        if (beat_i % 2 == 1) snare = nz() * expf(-tb * 22.0f) * 0.3f;
        float v = bass * 0.55f + arp + kick * 0.5f + hat + snare;
        b[i] = clip16(v * 0.5f);
    }
    A.mus[which] = b;
    A.mus_len[which] = n;
}

/* ---- playing ------------------------------------------------------------------------------------ */

bool ast_audio_open(void)
{
    bool s = A.sfx_on, m = A.music_on;
    ast_audio_close();
    A.sfx_on = s;
    A.music_on = m;
    A.rnd = 0xC0FFEEu;
    make_sounds();
    A.mus_now = -1;
    A.open = aos_hal_spk_open(RATE);
    if (!A.open) aos_hal_log("asteroides", "no speaker");
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    const char *tap = getenv("AST_SIM_TAP");
    if (tap && tap[0]) {
        A.tap = fopen(tap, "wb");
        if (A.tap) {
            uint8_t hdr[44] = { 0 };
            fwrite(hdr, 1, 44, A.tap);
            A.tap_n = 0;
            A.tap_t0 = aos_hal_uptime_us();
            A.open = true;
        }
    }
#endif
    return A.open;
}

#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
/* the header says how much there is: written again after every chunk, as
 * the simulator may quit without closing the app */
static void tap_header(void)
{
    uint32_t bytes = A.tap_n * 2;
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0 };
    uint32_t v = 36 + bytes;
    memcpy(h + 4, &v, 4);
    v = RATE;
    memcpy(h + 24, &v, 4);
    v = RATE * 2;
    memcpy(h + 28, &v, 4);
    h[32] = 2;
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    long at = ftell(A.tap);
    fseek(A.tap, 0, SEEK_SET);
    fwrite(h, 1, 44, A.tap);
    fseek(A.tap, at, SEEK_SET);
    fflush(A.tap);
}

static void tap_close(void)
{
    if (!A.tap) return;
    tap_header();
    fclose(A.tap);
    A.tap = NULL;
}
#endif

/* the two pieces take a moment: the worker makes them, the mixer waits */
void ast_audio_make_music(void)
{
    if (A.mus_ready) return;
    make_music(0);
    make_music(1);
    A.mus_ready = true;
}

void ast_audio_close(void)
{
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    tap_close();
#endif
    if (A.open && aos_hal_spk_is_open()) aos_hal_spk_close();
    for (int i = 0; i < SND_N; i++) ast_free(A.snd[i]);
    ast_free(A.hum);
    ast_free(A.mus[0]);
    ast_free(A.mus[1]);
    bool s = A.sfx_on, m = A.music_on;
    memset(&A, 0, sizeof A);
    A.sfx_on = s;
    A.music_on = m;
}

void ast_audio_set(bool sfx, bool music)
{
    A.sfx_on = sfx;
    A.music_on = music;
}

void ast_audio_music(int which)
{
    A.mus_want = which;
}

void ast_audio_thrust(float level)
{
    A.hum_want = level;
}

void ast_audio_pause(bool paused)
{
    A.paused = paused;
}

static const uint8_t LEVEL[SND_N] = {
    [SND_SHOT] = 120, [SND_SHOT3] = 130, [SND_BEAM] = 120, [SND_BOOM_S] = 170, [SND_BOOM_M] = 200,
    [SND_BOOM_L] = 230, [SND_CLANG] = 150, [SND_BLAST] = 255, [SND_STICK] = 200, [SND_JUMP] = 200,
    [SND_PICK] = 190, [SND_CRYSTAL] = 110, [SND_SHIELD] = 210, [SND_DIE] = 255, [SND_ESHOT] = 130,
    [SND_ALARM] = 170, [SND_WAVE] = 170, [SND_LIFE] = 200, [SND_MINE] = 110, [SND_DASH] = 190,
    [SND_BOSS_HIT] = 170, [SND_BOSS_DIE] = 255, [SND_UI] = 140,
};

void ast_audio_play(int snd, int vol)
{
    if (!A.open || !A.sfx_on || A.paused || snd < 0 || snd >= SND_N || !A.snd[snd]) return;
    vol = vol * (LEVEL[snd] ? LEVEL[snd] : 160) / 255;
    int pick = -1;
    for (int i = 0; i < VOICES; i++) {
        if (A.v[i].data && A.v[i].snd == snd && A.v[i].pos < 600) {
            if (vol > A.v[i].vol) A.v[i].vol = vol;
            return;
        }
    }
    for (int i = 0; i < VOICES && pick < 0; i++) {
        if (!A.v[i].data) pick = i;
    }
    if (pick < 0) {
        uint32_t best = 0;
        for (int i = 0; i < VOICES; i++) {
            uint32_t done = A.v[i].len ? A.v[i].pos * 256 / A.v[i].len : 256;
            if (pick < 0 || done > best) {
                best = done;
                pick = i;
            }
        }
    }
    voice_t *v = &A.v[pick];
    v->data = A.snd[snd];
    v->len = A.snd_len[snd];
    v->pos = 0;
    v->vol = vol;
    v->snd = snd;
}

static void mix_chunk(int16_t *out)
{
    int32_t mix[CHUNK];
    memset(mix, 0, sizeof mix);
    /* the music: fades out, changes, fades in */
    int want = A.music_on && !A.paused ? A.mus_want : 0;
    float target = want > 0 ? 1.0f : 0.0f;
    if (want != A.mus_now) {
        A.mus_gain -= 0.05f;
        if (A.mus_gain <= 0) {
            A.mus_gain = 0;
            A.mus_now = want;
            A.mus_pos = 0;
        }
    } else {
        A.mus_gain += (target - A.mus_gain) * 0.05f;
    }
    int tr = A.mus_now == 3 ? 1 : 0;
    if (A.mus_now > 0 && A.mus_ready && A.mus[tr] && A.mus_gain > 0.01f) {
        int lv = (int)(A.mus_gain * (A.mus_now == 1 ? 110.0f : 85.0f));
        for (int k = 0; k < CHUNK; k++) {
            mix[k] += (A.mus[tr][A.mus_pos] * lv) >> 8;
            if (++A.mus_pos >= A.mus_len[tr]) A.mus_pos = 0;
        }
    }
    /* the engine */
    float hw = A.sfx_on && !A.paused ? A.hum_want : 0;
    A.hum_now += (hw - A.hum_now) * 0.25f;
    int hl = (int)(A.hum_now * 150.0f);
    if (A.hum && hl > 1) {
        for (int k = 0; k < CHUNK; k++) {
            mix[k] += (A.hum[A.hum_pos] * hl) >> 8;
            if (++A.hum_pos >= A.hum_len) A.hum_pos = 0;
        }
    }
    for (int i = 0; i < VOICES; i++) {
        voice_t *v = &A.v[i];
        if (!v->data) continue;
        int n = CHUNK;
        if (v->pos + (uint32_t)n > v->len) n = (int)(v->len - v->pos);
        for (int k = 0; k < n; k++) mix[k] += (v->data[v->pos + k] * v->vol) >> 8;
        v->pos += (uint32_t)n;
        if (v->pos >= v->len) v->data = NULL;
    }
    for (int k = 0; k < CHUNK; k++) {
        int32_t s = mix[k] * 3 / 4;
        if (s > 22000) s = 22000 + (s - 22000) / 4;
        if (s < -22000) s = -22000 + (s + 22000) / 4;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out[k] = (int16_t)s;
    }
}

void ast_audio_pump(void)
{
    if (!A.open) return;
    int16_t out[CHUNK];
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    if (A.tap) {
        uint64_t want = (aos_hal_uptime_us() - A.tap_t0) * RATE / 1000000u;
        while ((uint64_t)A.tap_n + CHUNK <= want) {
            mix_chunk(out);
            fwrite(out, 2, CHUNK, A.tap);
            A.tap_n += CHUNK;
        }
        tap_header();
        return;
    }
#endif
    int guard = 0;
    while (aos_hal_spk_queued() < TARGET && guard++ < 16) {
        mix_chunk(out);
        if (aos_hal_spk_write(out, CHUNK) <= 0) break;
    }
}
