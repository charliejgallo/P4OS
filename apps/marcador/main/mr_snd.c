/*
 * Marcador - the sounds.
 *
 * Nothing is recorded: every sound is synthesised into a buffer when the
 * app opens (~150 ms on the board, ~400 KB of PSRAM) and mixed on the
 * streaming speaker while something sounds. Each one is built the way the
 * thing makes it:
 *
 *   - Hard little objects hitting something (a bean on wood, a bead on a
 *     bead, the counter's lever) are a click of noise followed by a few of
 *     the object's own modes: sines at inharmonic frequencies, each dying at
 *     its own rate. Hard wood rings high and short; steel rings longer.
 *   - Soft ones (a match on felt, the eraser, a paper card) are noise
 *     through a band, shaped in time; felt eats the highs.
 *   - The horn is two brassy tones a little out of tune with each other, as
 *     the compressed-air horns of the old boards, with a slow start.
 *   - Chalk is noise in the squeak band, made to stick and slip.
 *
 * Voices play a buffer at a rate (pitch), so twenty beans landing do not
 * sound like one: each gets a slightly different pitch and level.
 */
#include "mr_app.h"

#include "aos_hal.h"

#include <math.h>
#include <string.h>

#define RATE    24000
#define TARGET  960         /* 40 ms queued                              */
#define CHUNK   240
#define VOICES  10

typedef struct {
    float *pcm;
    int    n;
} snd_t;

typedef struct {
    const snd_t *s;
    float        pos, rate, gain;
} voice_t;

static struct {
    bool     ready, open;
    snd_t    snd[MR_S_N];
    voice_t  v[VOICES];
    uint32_t seed;
    uint32_t idle_ms;
} S;

static float nrand(void)
{
    S.seed = S.seed * 1664525u + 1013904223u;
    return (float)(int32_t)S.seed / 2147483648.0f;
}

static float *mk(int idx, float secs)
{
    snd_t *s = &S.snd[idx];
    s->n = (int)(secs * RATE);
    s->pcm = (float *)mr_alloc(sizeof(float) * (size_t)s->n);
    if (!s->pcm) s->n = 0;
    return s->pcm;
}

/* a damped mode starting at 'at' seconds */
static void mode(int idx, float at, float f, float decay, float amp)
{
    snd_t *s = &S.snd[idx];
    if (!s->pcm) return;
    int i0 = (int)(at * RATE);
    float w = 2.0f * 3.14159265f * f / RATE, k = expf(-1.0f / (decay * RATE));
    float e = amp, ph = nrand() * 3.14159f;
    for (int i = i0; i < s->n && e > 1e-4f; i++) {
        s->pcm[i] += sinf(ph) * e;
        ph += w;
        e *= k;
    }
}

/* a burst of noise through a two-pole band, with an exponential tail */
static void noise(int idx, float at, float len, float lo, float hi, float decay, float amp)
{
    snd_t *s = &S.snd[idx];
    if (!s->pcm) return;
    int i0 = (int)(at * RATE), n = (int)(len * RATE);
    float alo = 1.0f - expf(-2.0f * 3.14159265f * lo / RATE);
    float ahi = 1.0f - expf(-2.0f * 3.14159265f * hi / RATE);
    float l1 = 0, l2 = 0, h1 = 0, h2 = 0, k = decay > 0 ? expf(-1.0f / (decay * RATE)) : 1.0f, e = amp;
    for (int i = 0; i < n && i0 + i < s->n; i++) {
        float x = nrand();
        h1 += (x - h1) * ahi;
        h2 += (h1 - h2) * ahi;       /* low pass at hi */
        l1 += (h2 - l1) * alo;
        l2 += (l1 - l2) * alo;       /* what is under lo, to take away */
        float att = i < 24 ? (float)i / 24.0f : 1.0f;
        s->pcm[i0 + i] += (h2 - l2) * e * att * 2.2f;
        e *= k;
    }
}

static void normalise(int idx, float peak)
{
    snd_t *s = &S.snd[idx];
    float m = 0;
    for (int i = 0; i < s->n; i++)
        if (fabsf(s->pcm[i]) > m) m = fabsf(s->pcm[i]);
    if (m <= 0) return;
    for (int i = 0; i < s->n; i++) s->pcm[i] *= peak / m;
}

static void build(void)
{
    S.seed = 0x5EEDu;

    /* a match laid on felt: a dull tick, the felt keeps the highs */
    mk(MR_S_MATCH_ON, 0.09f);
    noise(MR_S_MATCH_ON, 0, 0.03f, 300, 2400, 0.006f, 1.0f);
    mode(MR_S_MATCH_ON, 0.001f, 820, 0.012f, 0.35f);
    mode(MR_S_MATCH_ON, 0.001f, 1630, 0.008f, 0.2f);
    mode(MR_S_MATCH_ON, 0.000f, 190, 0.02f, 0.25f);
    normalise(MR_S_MATCH_ON, 0.55f);

    /* lifted off: a brush of the head on the cloth */
    mk(MR_S_MATCH_OFF, 0.10f);
    noise(MR_S_MATCH_OFF, 0, 0.08f, 900, 5000, 0.025f, 0.6f);
    mode(MR_S_MATCH_OFF, 0.0f, 640, 0.01f, 0.15f);
    normalise(MR_S_MATCH_OFF, 0.35f);

    /* the matches rattling in their box as one is taken out */
    mk(MR_S_BOX, 0.22f);
    for (int i = 0; i < 9; i++) {
        float t = 0.012f * (float)i + 0.004f * (nrand() + 1);
        noise(MR_S_BOX, t, 0.012f, 1200, 6000, 0.003f, 0.5f + 0.3f * nrand());
        mode(MR_S_BOX, t, 2100 + 600 * nrand(), 0.004f, 0.25f);
    }
    noise(MR_S_BOX, 0.0f, 0.2f, 2000, 7000, 0.06f, 0.15f);
    normalise(MR_S_BOX, 0.35f);

    /* a dry bean on a wooden table: hard, short, a little of the board under it */
    mk(MR_S_BEAN, 0.12f);
    noise(MR_S_BEAN, 0, 0.004f, 1500, 9000, 0.001f, 1.0f);
    mode(MR_S_BEAN, 0, 2350, 0.006f, 0.7f);
    mode(MR_S_BEAN, 0, 3920, 0.004f, 0.5f);
    mode(MR_S_BEAN, 0, 5650, 0.003f, 0.35f);
    mode(MR_S_BEAN, 0, 230, 0.03f, 0.18f);
    mode(MR_S_BEAN, 0.022f, 2600, 0.004f, 0.18f);     /* the little bounce */
    normalise(MR_S_BEAN, 0.6f);

    /* a bean onto other beans: three ticks close together */
    mk(MR_S_BEAN_SOFT, 0.10f);
    for (int i = 0; i < 3; i++) {
        float t = 0.018f * (float)i + 0.004f * (nrand() + 1);
        noise(MR_S_BEAN_SOFT, t, 0.003f, 2000, 9000, 0.001f, 0.7f - 0.15f * (float)i);
        mode(MR_S_BEAN_SOFT, t, 3100 + 500 * nrand(), 0.004f, 0.4f - 0.1f * (float)i);
    }
    normalise(MR_S_BEAN_SOFT, 0.4f);

    /* picked up: a scrape */
    mk(MR_S_BEAN_GRAB, 0.05f);
    noise(MR_S_BEAN_GRAB, 0, 0.04f, 2500, 8000, 0.01f, 0.5f);
    normalise(MR_S_BEAN_GRAB, 0.22f);

    /* the counter's lever going down: a spring and a pawl in a steel case */
    mk(MR_S_CLICK, 0.16f);
    noise(MR_S_CLICK, 0, 0.003f, 2000, 11000, 0.0008f, 1.0f);
    mode(MR_S_CLICK, 0, 3240, 0.018f, 0.55f);
    mode(MR_S_CLICK, 0, 4710, 0.012f, 0.42f);
    mode(MR_S_CLICK, 0, 6180, 0.009f, 0.3f);
    mode(MR_S_CLICK, 0, 1180, 0.03f, 0.22f);
    mode(MR_S_CLICK, 0, 420, 0.02f, 0.25f);
    normalise(MR_S_CLICK, 0.7f);

    /* and coming back up */
    mk(MR_S_CLICK_UP, 0.09f);
    noise(MR_S_CLICK_UP, 0, 0.002f, 3000, 11000, 0.0006f, 0.6f);
    mode(MR_S_CLICK_UP, 0, 3900, 0.012f, 0.35f);
    mode(MR_S_CLICK_UP, 0, 5480, 0.008f, 0.25f);
    normalise(MR_S_CLICK_UP, 0.35f);

    /* the reset knob turned: the wheels ratchet round to nought */
    mk(MR_S_RESET, 0.42f);
    {
        float t = 0;
        for (int i = 0; i < 16; i++) {
            noise(MR_S_RESET, t, 0.002f, 2500, 10000, 0.0006f, 0.7f);
            mode(MR_S_RESET, t, 4200 + 300 * nrand(), 0.006f, 0.35f);
            t += 0.030f - 0.0012f * (float)i;
        }
        mode(MR_S_RESET, t, 3240, 0.03f, 0.5f);
        noise(MR_S_RESET, 0, t, 1500, 6000, 0.2f, 0.08f);
    }
    normalise(MR_S_RESET, 0.55f);

    /* the horn: two reeds out of tune with each other, air behind them */
    mk(MR_S_HORN, 1.7f);
    {
        snd_t *s = &S.snd[MR_S_HORN];
        float ph[2] = { 0, 0 }, f[2] = { 233.0f, 294.5f }, lp = 0, lp2 = 0;
        for (int i = 0; s->pcm && i < s->n; i++) {
            float t = (float)i / RATE;
            float env = mr_smooth(0.0f, 0.09f, t) * (1.0f - mr_smooth(1.45f, 1.7f, t));
            float vib = 1.0f + 0.004f * sinf(t * 2 * 3.14159f * 5.5f);
            float x = 0;
            for (int k = 0; k < 2; k++) {
                ph[k] += f[k] * vib * (1.0f + 0.012f * (1.0f - env)) / RATE;
                if (ph[k] >= 1) ph[k] -= 1;
                /* a buzzing reed: a narrow pulse, rich in odd and even partials */
                x += (ph[k] < 0.28f ? 1.0f : -0.39f);
            }
            x += 0.06f * nrand();
            lp += (x - lp) * 0.32f;
            lp2 += (lp - lp2) * 0.45f;
            s->pcm[i] = lp2 * env;
        }
    }
    normalise(MR_S_HORN, 0.8f);

    /* a short buzzer (the end of a period, if the horn is too much) */
    mk(MR_S_BUZZ, 0.6f);
    {
        snd_t *s = &S.snd[MR_S_BUZZ];
        float ph = 0;
        for (int i = 0; s->pcm && i < s->n; i++) {
            float t = (float)i / RATE;
            float env = mr_smooth(0, 0.01f, t) * (1 - mr_smooth(0.5f, 0.6f, t));
            ph += 110.0f / RATE;
            if (ph >= 1) ph -= 1;
            s->pcm[i] = ((ph < 0.5f ? 1.0f : -1.0f) * 0.6f + 0.15f * nrand()) * env;
        }
    }
    normalise(MR_S_BUZZ, 0.5f);

    /* the board's relays when a number changes: clack-clack inside the cabinet */
    mk(MR_S_RELAY, 0.12f);
    for (int i = 0; i < 2; i++) {
        float t = 0.035f * (float)i;
        noise(MR_S_RELAY, t, 0.003f, 800, 6000, 0.001f, 0.8f);
        mode(MR_S_RELAY, t, 1450, 0.01f, 0.4f);
        mode(MR_S_RELAY, t, 160, 0.025f, 0.35f);
    }
    normalise(MR_S_RELAY, 0.4f);

    /* a flip card over the rings: the rush of the card, then it slaps the back */
    mk(MR_S_FLIP, 0.22f);
    noise(MR_S_FLIP, 0, 0.12f, 700, 5000, 0.05f, 0.5f);
    noise(MR_S_FLIP, 0.13f, 0.03f, 400, 4000, 0.008f, 1.0f);
    mode(MR_S_FLIP, 0.13f, 380, 0.02f, 0.3f);
    mode(MR_S_FLIP, 0.13f, 1250, 0.01f, 0.2f);
    normalise(MR_S_FLIP, 0.5f);

    /* a maple bead hitting another */
    mk(MR_S_BEAD, 0.12f);
    noise(MR_S_BEAD, 0, 0.003f, 1200, 8000, 0.0008f, 0.9f);
    mode(MR_S_BEAD, 0, 1180, 0.022f, 0.6f);
    mode(MR_S_BEAD, 0, 2710, 0.012f, 0.4f);
    mode(MR_S_BEAD, 0, 4320, 0.007f, 0.3f);
    normalise(MR_S_BEAD, 0.6f);

    /* chalk across slate: stick, slip, stick */
    mk(MR_S_CHALK, 0.30f);
    {
        snd_t *s = &S.snd[MR_S_CHALK];
        float l1 = 0, l2 = 0, h = 0, ph = 0, sq = 0;
        for (int i = 0; s->pcm && i < s->n; i++) {
            float t = (float)i / RATE;
            float env = mr_smooth(0, 0.02f, t) * (1 - mr_smooth(0.2f, 0.3f, t));
            float x = nrand();
            h += (x - h) * 0.75f;
            l1 += (h - l1) * 0.12f;
            l2 += (l1 - l2) * 0.12f;
            ph += (37.0f + 9.0f * sinf(t * 21.0f)) / RATE;   /* the stick-slip rate */
            if (ph >= 1) ph -= 1;
            sq += ((ph < 0.18f ? 1.0f : 0.25f) - sq) * 0.02f;
            s->pcm[i] = (h - l2) * env * sq;
        }
    }
    normalise(MR_S_CHALK, 0.35f);

    mk(MR_S_CHALK_TAP, 0.05f);
    noise(MR_S_CHALK_TAP, 0, 0.004f, 1500, 9000, 0.001f, 1.0f);
    mode(MR_S_CHALK_TAP, 0, 1900, 0.006f, 0.4f);
    mode(MR_S_CHALK_TAP, 0, 260, 0.012f, 0.3f);
    normalise(MR_S_CHALK_TAP, 0.4f);

    /* the felt eraser rubbed to and fro */
    mk(MR_S_ERASE, 0.55f);
    {
        snd_t *s = &S.snd[MR_S_ERASE];
        float l1 = 0, l2 = 0;
        for (int i = 0; s->pcm && i < s->n; i++) {
            float t = (float)i / RATE;
            float env = mr_smooth(0, 0.04f, t) * (1 - mr_smooth(0.42f, 0.55f, t));
            float am = 0.45f + 0.55f * fabsf(sinf(t * 2 * 3.14159f * 3.6f));
            float x = nrand();
            l1 += (x - l1) * 0.22f;
            l2 += (l1 - l2) * 0.22f;
            s->pcm[i] = (l1 - l2 * 0.6f) * env * am;
        }
    }
    normalise(MR_S_ERASE, 0.3f);

    mk(MR_S_TAP, 0.04f);
    noise(MR_S_TAP, 0, 0.003f, 1500, 7000, 0.001f, 0.6f);
    mode(MR_S_TAP, 0, 1800, 0.008f, 0.4f);
    normalise(MR_S_TAP, 0.25f);

    /* the winner: four bells, up */
    mk(MR_S_WIN, 1.3f);
    {
        static const float N[4] = { 523.25f, 659.25f, 783.99f, 1046.5f };
        for (int i = 0; i < 4; i++) {
            float t = 0.11f * (float)i;
            mode(MR_S_WIN, t, N[i], 0.35f, 0.5f);
            mode(MR_S_WIN, t, N[i] * 2.01f, 0.18f, 0.18f);
            mode(MR_S_WIN, t, N[i] * 3.0f, 0.08f, 0.08f);
        }
        mode(MR_S_WIN, 0.44f, 1046.5f * 1.5f, 0.3f, 0.15f);
    }
    normalise(MR_S_WIN, 0.6f);

    mk(MR_S_UNDO, 0.12f);
    {
        snd_t *s = &S.snd[MR_S_UNDO];
        float ph = 0;
        for (int i = 0; s->pcm && i < s->n; i++) {
            float t = (float)i / RATE;
            ph += (900.0f - 2600.0f * t) / RATE;
            s->pcm[i] = sinf(ph * 6.2831853f) * expf(-t * 30.0f);
        }
    }
    normalise(MR_S_UNDO, 0.3f);

    S.ready = true;
}

void mr_snd_open(void)
{
    if (!S.ready) build();
}

void mr_snd_close(void)
{
    if (S.open) aos_hal_spk_close();
    S.open = false;
    for (int i = 0; i < MR_S_N; i++) mr_free(S.snd[i].pcm);
    memset(&S, 0, sizeof S);
}

void mr_snd_play(int snd, float gain, float pitch)
{
    if (!mr_A || !mr_A->sound || snd < 0 || snd >= MR_S_N) return;
    if (!S.ready) build();
    if (!S.snd[snd].n) return;
    if (!S.open) {
        S.open = aos_hal_spk_open(RATE);
        if (!S.open) return;
    }
    int pick = 0;
    for (int i = 0; i < VOICES; i++) {
        if (!S.v[i].s) {
            pick = i;
            break;
        }
        if (S.v[i].pos > S.v[pick].pos) pick = i;
    }
    voice_t *v = &S.v[pick];
    v->s = &S.snd[snd];
    v->pos = 0;
    v->rate = pitch > 0.2f ? pitch : 1.0f;
    v->gain = gain;
    S.idle_ms = 0;
}

void mr_snd_pump(void)
{
    if (!S.open) return;
    bool any = false;
    for (int i = 0; i < VOICES; i++)
        if (S.v[i].s) any = true;
    if (!any) {
        /* the codec goes back after a few seconds of nothing */
        if (!S.idle_ms) S.idle_ms = mr_ms();
        if (mr_ms() - S.idle_ms > 4000) {
            aos_hal_spk_close();
            S.open = false;
        }
        return;
    }
    int16_t buf[CHUNK];
    int guard = 0;
    while (aos_hal_spk_queued() < TARGET && guard++ < 6) {
        float mix[CHUNK];
        memset(mix, 0, sizeof mix);
        for (int i = 0; i < VOICES; i++) {
            voice_t *v = &S.v[i];
            if (!v->s) continue;
            for (int k = 0; k < CHUNK; k++) {
                int p = (int)v->pos;
                if (p + 1 >= v->s->n) {
                    v->s = NULL;
                    break;
                }
                float fr = v->pos - (float)p;
                mix[k] += (v->s->pcm[p] + (v->s->pcm[p + 1] - v->s->pcm[p]) * fr) * v->gain;
                v->pos += v->rate;
            }
        }
        for (int k = 0; k < CHUNK; k++) {
            float s = mix[k] * 26000.0f;
            if (s > 24000) s = 24000 + (s - 24000) * 0.25f;
            if (s < -24000) s = -24000 + (s + 24000) * 0.25f;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            buf[k] = (int16_t)s;
        }
        if (aos_hal_spk_write(buf, CHUNK) <= 0) break;
    }
}
