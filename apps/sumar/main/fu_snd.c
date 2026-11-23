/*
 * P4OS - Fusión: the sounds
 *
 * Synthesised as they play, into the streaming speaker, like Sudoku and
 * Bastión mix theirs: a few voices, each a short tone or a puff of noise
 * with its own envelope, a chunk at a time from the frame timer, never more
 * than ~100 ms queued so a merge is heard when it happens.
 *
 * A merge rings a note that climbs with the value it made, along a
 * pentatonic scale, so a good move sounds like a little run upwards.
 */
#include "fu_app.h"

#include <math.h>
#include <string.h>

#define RATE    16000
#define TARGET  1600
#define CHUNK   320
#define VOICES  8

typedef struct {
    bool  on;
    bool  noise;
    int   delay, len, pos;
    float freq, sweep, phase;
    float amp;
    float attack;               /* samples */
    float lp;                   /* the noise's low pass state */
} voice_t;

static struct {
    bool     open;
    voice_t  v[VOICES];
    uint32_t seed;
} S;

void fu_snd_open(void)
{
    if (!S.open && fu_P.sound) S.open = aos_hal_spk_open(RATE);
}

void fu_snd_close(void)
{
    if (S.open) aos_hal_spk_close();
    S.open = false;
    memset(S.v, 0, sizeof S.v);
}

void fu_snd_free(void)
{
    fu_snd_close();
}

static void voice(float freq, int ms, float amp, int delay_ms, float sweep, bool noise)
{
    int pick = 0;
    for (int i = 0; i < VOICES; i++) {
        if (!S.v[i].on) { pick = i; break; }
        if (S.v[i].pos > S.v[pick].pos) pick = i;
    }
    voice_t *v = &S.v[pick];
    memset(v, 0, sizeof *v);
    v->on = true;
    v->noise = noise;
    v->freq = freq;
    v->len = ms * RATE / 1000;
    v->delay = delay_ms * RATE / 1000;
    v->amp = amp;
    v->sweep = sweep;
    v->attack = 40;
}

/* The pentatonic step for an exponent: 2 is the low C, each value one up. */
static float note(int exp)
{
    static const int steps[5] = { 0, 2, 4, 7, 9 };
    int k = exp - 1;
    if (k < 0) k = 0;
    int semis = (k / 5) * 12 + steps[k % 5];
    return 261.6f * powf(2.0f, (float)semis / 12.0f);
}

void fu_snd_play(int snd, int pitch)
{
    if (!fu_P.sound) return;
    if (!S.open) fu_snd_open();
    if (!S.open) return;
    switch (snd) {
    case FU_S_SLIDE: voice(900, 55, 2200, 0, 0.99990f, true); break;
    case FU_S_MERGE:
        voice(note(pitch), 150, 7000, 0, 1.0f, false);
        voice(note(pitch) * 2.0f, 90, 2200, 0, 1.0f, false);
        break;
    case FU_S_SPAWN: voice(1760, 18, 1600, 30, 1.0f, false); break;
    case FU_S_GOAL:
        for (int i = 0; i < 4; i++) voice(note(8 + i * 2), 180, 6500, i * 90, 1.0f, false);
        voice(note(16), 380, 4500, 360, 1.0f, false);
        break;
    case FU_S_OVER:
        for (int i = 0; i < 3; i++) voice(note(9 - i * 2), 220, 6000, i * 160, 0.99996f, false);
        break;
    case FU_S_UNDO: voice(700, 110, 4500, 0, 0.99990f, false); break;
    case FU_S_DENY: voice(150, 80, 6000, 0, 1.0f, false); break;
    case FU_S_TICK: voice(1000, 25, 3500, 0, 1.0f, false); break;
    default: break;
    }
}

void fu_snd_pump(void)
{
    if (!S.open) return;
    if (!fu_P.sound) {
        fu_snd_close();
        return;
    }
    int16_t buf[CHUNK];
    int guard = 0;
    while (aos_hal_spk_queued() < TARGET && guard++ < 8) {
        float mix[CHUNK];
        memset(mix, 0, sizeof mix);
        for (int i = 0; i < VOICES; i++) {
            voice_t *v = &S.v[i];
            if (!v->on) continue;
            for (int k = 0; k < CHUNK; k++) {
                if (v->delay > 0) { v->delay--; continue; }
                if (v->pos >= v->len) { v->on = false; break; }
                float t = (float)v->pos / (float)v->len;
                float env = (1 - t) * (1 - t);
                if ((float)v->pos < v->attack) env *= (float)v->pos / v->attack;
                float s;
                if (v->noise) {
                    S.seed = S.seed * 1664525u + 1013904223u;
                    float n = (float)(int32_t)S.seed / 2147483648.0f;
                    v->lp += (n - v->lp) * (v->freq / (float)RATE);
                    s = v->lp * 3.0f;
                    v->freq *= v->sweep;
                } else {
                    v->phase += v->freq / (float)RATE;
                    if (v->phase >= 1.0f) v->phase -= 1.0f;
                    float p = v->phase * 6.2831853f;
                    s = sinf(p) + 0.25f * sinf(2 * p);
                    v->freq *= v->sweep;
                }
                mix[k] += s * v->amp * env;
                v->pos++;
            }
        }
        for (int k = 0; k < CHUNK; k++) {
            float s = mix[k];
            if (s > 24000) s = 24000 + (s - 24000) / 4;
            if (s < -24000) s = -24000 + (s + 24000) / 4;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            buf[k] = (int16_t)s;
        }
        if (aos_hal_spk_write(buf, CHUNK) <= 0) break;
    }
}
