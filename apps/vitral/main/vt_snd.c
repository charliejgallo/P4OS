/*
 * VITRAL - the sound, synthesised when the app opens (no pack)
 *
 * Glass: a tap is a few inharmonic partials that die fast (the ratios of a
 * small plate, 1 : 2.76 : 5.40), a break is a burst of bright noise with
 * shards ringing in it, a vitral is a chord of bells. Rendered once into
 * PSRAM at 16 kHz and mixed on the streaming speaker, eight voices, pumped
 * from the game's timer (Hockey's mixer).
 */
#include "vt_snd.h"

#include "aos_hal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RATE    16000
#define TARGET  1600
#define CHUNK   256
#define VOICES  8

typedef struct {
    const int16_t *data;
    uint32_t len, pos;
    int      vol;
    int      snd;
} voice_t;

static struct {
    bool     open, on;
    int16_t *pcm[VT_SND_N];
    uint32_t len[VT_SND_N];
    voice_t  v[VOICES];
    uint32_t rng;
} A;

static float frnd(void)
{
    uint32_t x = A.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    A.rng = x;
    return (float)(x & 0xFFFF) / 32768.0f - 1.0f;
}

/* ---- the instruments ---- */

typedef struct {
    float *buf;
    int    n;
} sbuf_t;

/* a struck piece of glass or a bell: partials at f * ratio, each dying */
static void ping(sbuf_t *b, float at, float f, float amp, float decay, int bell)
{
    static const float GLASS_R[4] = { 1.0f, 2.76f, 5.40f, 8.93f };
    static const float GLASS_A[4] = { 1.0f, 0.55f, 0.28f, 0.12f };
    static const float BELL_R[5] = { 0.5f, 1.0f, 1.19f, 1.5f, 2.0f };
    static const float BELL_A[5] = { 0.35f, 1.0f, 0.45f, 0.30f, 0.25f };
    int np = bell ? 5 : 4;
    int start = (int)(at * RATE);
    int len = (int)(decay * 5.0f * RATE);
    for (int p = 0; p < np; p++) {
        float r = bell ? BELL_R[p] : GLASS_R[p], a = (bell ? BELL_A[p] : GLASS_A[p]) * amp;
        float fr = f * r;
        if (fr > RATE * 0.45f) continue;
        float w = 6.2831853f * fr / RATE;
        /* higher partials die sooner */
        float dk = expf(-1.0f / (decay * RATE / (1.0f + (float)p * (bell ? 0.4f : 1.2f))));
        float env = a, ph = frnd() * 3.0f;
        for (int i = 0; i < len; i++) {
            int k = start + i;
            if (k >= b->n) break;
            float att = i < 40 ? (float)i / 40.0f : 1.0f;
            b->buf[k] += env * att * sinf(ph);
            ph += w;
            if (ph > 6.2831853f) ph -= 6.2831853f;
            env *= dk;
            if (env < 0.0005f) break;
        }
    }
}

/* noise through a one-pole filter: 'hp' 1 = only the bright part */
static void noise(sbuf_t *b, float at, float dur, float amp, float decay, float tone, int hp)
{
    int start = (int)(at * RATE), len = (int)(dur * RATE);
    float lp = 0, env = amp, dk = expf(-1.0f / (decay * RATE));
    for (int i = 0; i < len; i++) {
        int k = start + i;
        if (k >= b->n) break;
        float x = frnd();
        lp += (x - lp) * tone;
        float y = hp ? x - lp : lp;
        float att = i < 24 ? (float)i / 24.0f : 1.0f;
        b->buf[k] += y * env * att;
        env *= dk;
    }
}

/* a low thump */
static void thud(sbuf_t *b, float at, float f, float amp, float decay)
{
    int start = (int)(at * RATE), len = (int)(decay * 6.0f * RATE);
    float ph = 0, env = amp, dk = expf(-1.0f / (decay * RATE));
    for (int i = 0; i < len; i++) {
        int k = start + i;
        if (k >= b->n) break;
        float fr = f * (1.0f + 0.8f * expf(-(float)i / 300.0f));
        b->buf[k] += sinf(ph) * env;
        ph += 6.2831853f * fr / RATE;
        env *= dk;
    }
}

/* shards: a scatter of small glass pings */
static void shards(sbuf_t *b, float at, float spread, int n, float amp)
{
    for (int i = 0; i < n; i++) {
        float t = at + (frnd() * 0.5f + 0.5f) * spread * (0.3f + 0.7f * (float)i / (float)n);
        float f = 2200.0f + (frnd() * 0.5f + 0.5f) * 3800.0f;
        ping(b, t, f, amp * (0.4f + 0.6f * (frnd() * 0.5f + 0.5f)), 0.025f + 0.03f * (frnd() * 0.5f + 0.5f), 0);
    }
}

static void render(int snd, sbuf_t *b)
{
    /* notes of a pentatonic scale, for the cascades and the bells */
    static const float NOTE[8] = { 523.3f, 587.3f, 659.3f, 784.0f, 880.0f, 1046.5f, 1174.7f, 1318.5f };
    switch (snd) {
    case VT_SND_MOVE:   ping(b, 0, 2600.0f, 0.18f, 0.010f, 0); break;
    case VT_SND_ROTATE: ping(b, 0, 1900.0f, 0.30f, 0.030f, 0); break;
    case VT_SND_SOFT:   ping(b, 0, 3200.0f, 0.08f, 0.006f, 0); break;
    case VT_SND_LOCK:
        thud(b, 0, 140.0f, 0.35f, 0.025f);
        ping(b, 0, 980.0f, 0.28f, 0.030f, 0);
        noise(b, 0, 0.03f, 0.12f, 0.008f, 0.3f, 1);
        break;
    case VT_SND_HARD:
        noise(b, 0, 0.07f, 0.20f, 0.03f, 0.08f, 0);
        thud(b, 0.045f, 110.0f, 0.55f, 0.035f);
        ping(b, 0.045f, 820.0f, 0.30f, 0.035f, 0);
        break;
    case VT_SND_HOLD:
        noise(b, 0, 0.12f, 0.16f, 0.05f, 0.05f, 0);
        ping(b, 0.02f, 1400.0f, 0.15f, 0.04f, 0);
        break;
    case VT_SND_CLEAR1:
    case VT_SND_CLEAR2:
    case VT_SND_CLEAR3: {
        int k = snd - VT_SND_CLEAR1;
        noise(b, 0, 0.25f + 0.15f * (float)k, 0.55f, 0.06f + 0.03f * (float)k, 0.35f, 1);
        noise(b, 0, 0.08f, 0.25f, 0.03f, 0.05f, 0);
        shards(b, 0.01f, 0.30f + 0.15f * (float)k, 14 + 10 * k, 0.32f);
        for (int i = 0; i <= k; i++) ping(b, 0.04f * (float)i, NOTE[2 + 2 * i], 0.18f, 0.10f, 1);
        break;
    }
    case VT_SND_VITRAL:
        noise(b, 0, 0.3f, 0.35f, 0.08f, 0.35f, 1);
        shards(b, 0.0f, 0.35f, 20, 0.25f);
        ping(b, 0.00f, NOTE[0], 0.40f, 0.45f, 1);
        ping(b, 0.06f, NOTE[2], 0.36f, 0.45f, 1);
        ping(b, 0.12f, NOTE[4], 0.34f, 0.45f, 1);
        ping(b, 0.18f, NOTE[5] * 1.0f, 0.34f, 0.55f, 1);
        ping(b, 0.30f, NOTE[7], 0.22f, 0.40f, 1);
        break;
    case VT_SND_LEVEL:
        for (int i = 0; i < 4; i++) ping(b, 0.09f * (float)i, NOTE[1 + i * 2 > 7 ? 7 : 1 + i * 2], 0.30f, 0.20f, 1);
        break;
    case VT_SND_OVER:
        ping(b, 0.00f, NOTE[4] / 2.0f, 0.40f, 0.30f, 1);
        ping(b, 0.25f, NOTE[2] / 2.0f, 0.40f, 0.30f, 1);
        ping(b, 0.50f, NOTE[0] / 2.0f, 0.45f, 0.60f, 1);
        noise(b, 0.50f, 0.6f, 0.30f, 0.15f, 0.25f, 1);
        shards(b, 0.50f, 0.6f, 24, 0.22f);
        break;
    case VT_SND_REJECT:
        thud(b, 0, 120.0f, 0.5f, 0.03f);
        thud(b, 0.11f, 95.0f, 0.5f, 0.04f);
        break;
    case VT_SND_WIN:
        for (int i = 0; i < 6; i++) {
            static const int M[6] = { 0, 2, 4, 3, 5, 7 };
            ping(b, 0.11f * (float)i, NOTE[M[i]], 0.32f, i == 5 ? 0.7f : 0.22f, 1);
        }
        shards(b, 0.55f, 0.8f, 26, 0.12f);
        break;
    case VT_SND_GARBAGE:
        thud(b, 0, 70.0f, 0.5f, 0.08f);
        noise(b, 0, 0.25f, 0.18f, 0.08f, 0.04f, 0);
        break;
    case VT_SND_TAP: ping(b, 0, 1500.0f, 0.18f, 0.02f, 0); break;
    case VT_SND_TICK: ping(b, 0, 2100.0f, 0.22f, 0.015f, 0); break;
    default: break;
    }
}

static const float LEN_S[VT_SND_N] = {
    [VT_SND_MOVE] = 0.06f, [VT_SND_ROTATE] = 0.18f, [VT_SND_SOFT] = 0.04f, [VT_SND_LOCK] = 0.20f,
    [VT_SND_HARD] = 0.28f, [VT_SND_HOLD] = 0.22f, [VT_SND_CLEAR1] = 0.75f, [VT_SND_CLEAR2] = 0.95f,
    [VT_SND_CLEAR3] = 1.15f, [VT_SND_VITRAL] = 2.2f, [VT_SND_LEVEL] = 1.2f, [VT_SND_OVER] = 2.4f,
    [VT_SND_REJECT] = 0.35f, [VT_SND_WIN] = 2.4f, [VT_SND_GARBAGE] = 0.5f, [VT_SND_TAP] = 0.12f,
    [VT_SND_TICK] = 0.08f,
};

static void build(void)
{
    A.rng = 0x1234567u;
    for (int s = 0; s < VT_SND_N; s++) {
        if (A.pcm[s]) continue;
        int n = (int)(LEN_S[s] * RATE);
        if (n <= 0) continue;
        float *f = (float *)malloc((size_t)n * sizeof(float));
        int16_t *p = (int16_t *)malloc((size_t)n * sizeof(int16_t));
        if (!f || !p) {
            free(f);
            free(p);
            continue;
        }
        memset(f, 0, (size_t)n * sizeof(float));
        sbuf_t b = { f, n };
        render(s, &b);
        /* a soft limit, and a fade at the very end */
        for (int i = 0; i < n; i++) {
            float x = f[i];
            if (i > n - 200) x *= (float)(n - i) / 200.0f;
            if (x > 0.9f) x = 0.9f + (x - 0.9f) * 0.2f;
            if (x < -0.9f) x = -0.9f + (x + 0.9f) * 0.2f;
            if (x > 1.0f) x = 1.0f;
            if (x < -1.0f) x = -1.0f;
            p[i] = (int16_t)(x * 30000.0f);
        }
        free(f);
        A.pcm[s] = p;
        A.len[s] = (uint32_t)n;
    }
}

void vt_snd_open(void)
{
    if (A.open) return;
    build();
    A.open = aos_hal_spk_open(RATE);
    if (!A.open) aos_hal_log("vitral", "no speaker");
}

void vt_snd_close(void)
{
    if (A.open) aos_hal_spk_close();
    A.open = false;
    for (int i = 0; i < VOICES; i++) A.v[i].data = NULL;
}

void vt_snd_free(void)
{
    vt_snd_close();
    for (int s = 0; s < VT_SND_N; s++) {
        free(A.pcm[s]);
        A.pcm[s] = NULL;
        A.len[s] = 0;
    }
}

void vt_snd_enable(bool on)
{
    A.on = on;
    if (!on) for (int i = 0; i < VOICES; i++) A.v[i].data = NULL;
}

bool vt_snd_enabled(void)
{
    return A.on;
}

void vt_snd_play(int snd, int vol)
{
    if (!A.open || !A.on || snd < 0 || snd >= VT_SND_N || !A.pcm[snd]) return;
    int pick = -1;
    for (int i = 0; i < VOICES; i++) {
        if (A.v[i].data && A.v[i].snd == snd && A.v[i].pos < 600) {
            if (vol > A.v[i].vol) A.v[i].vol = vol;
            return;
        }
    }
    for (int i = 0; i < VOICES && pick < 0; i++) if (!A.v[i].data) pick = i;
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
    v->data = A.pcm[snd];
    v->len = A.len[snd];
    v->pos = 0;
    v->vol = vol;
    v->snd = snd;
}

void vt_snd_pump(void)
{
    if (!A.open) return;
    int16_t buf[CHUNK];
    int guard = 0;
    while (aos_hal_spk_queued() < TARGET && guard++ < 16) {
        int32_t mix[CHUNK];
        memset(mix, 0, sizeof mix);
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
            int32_t s = mix[k];
            if (s > 24000) s = 24000 + (s - 24000) / 4;
            if (s < -24000) s = -24000 + (s + 24000) / 4;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            buf[k] = (int16_t)s;
        }
        if (aos_hal_spk_write(buf, CHUNK) <= 0) break;
    }
}

#ifdef AOS_SIM_BUILTIN
#include <stdio.h>
/* VT_SND_DUMP=<dir>: writes every sound as a WAV, to hear them on the Mac
 * (the simulator's speaker does not play) */
void vt_snd_dump(const char *dir)
{
    build();
    for (int s = 0; s < VT_SND_N; s++) {
        if (!A.pcm[s]) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/vitral_%02d.wav", dir, s);
        FILE *f = fopen(path, "wb");
        if (!f) continue;
        uint32_t n = A.len[s] * 2u;
        uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                          0x80, 0x3E, 0, 0, 0, 0x7D, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
        uint32_t riff = n + 36u;
        memcpy(h + 4, &riff, 4);
        memcpy(h + 40, &n, 4);
        fwrite(h, 1, 44, f);
        fwrite(A.pcm[s], 2, A.len[s], f);
        fclose(f);
    }
}
#endif
