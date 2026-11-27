/*
 * Botonera - the mixer.
 *
 * The firmware's streaming speaker (aos_hal_spk_*) takes one mono stream and
 * nothing else: the system has no mixer an app can hand several sounds to
 * (aos_hal_play_file plays one file and cuts the previous one). So, like DJ
 * and Estudio, the app mixes by itself: every sound is decoded beforehand to
 * PCM at BN_RATE (bn_dec.c), and this thread adds up to BN_VOICES of them, a
 * block of BN_BLK samples at a time, and writes the sum to the speaker,
 * opened with aos_hal_spk_open_low() so that a touch is heard in ~10 ms plus
 * our queue.
 *
 * Nothing is locked. The UI talks to the thread through a ring of commands
 * (one writer, one reader, each index written by one side only, with a fence
 * between the slot and the index); the thread answers in arrays it alone
 * writes: the voices of each pad, how far the newest one is, and the last
 * command it took. A clip is freed by the UI when the plays it sent equal the
 * voices of it the thread has finished (bn_clip_t, 'starts' and 'ends').
 */
#include "bn.h"

#include "aos_hal.h"

#include <math.h>
#include <string.h>
#if defined(AOS_SIM)
#include <stdio.h>
#include <stdlib.h>
#endif

#if defined(__riscv)
#define BN_FENCE() __asm__ volatile("fence rw,rw" ::: "memory")
#else
#define BN_FENCE() __sync_synchronize()
#endif

#define BN_BLK      64                  /* 2 ms at 32 kHz */
#define RING        128                 /* commands in flight */
#define FADE_N      256                 /* 8 ms to stop without a click */
#define NSLOT       (BN_PADS + 1)       /* slot 0 is the preview (pad -1) */

typedef enum { C_PLAY = 1, C_STOP, C_STOP_ALL, C_GAIN } cmd_type_t;

typedef struct {
    uint8_t    type;
    uint8_t    loop;
    int16_t    slot;
    float      gain;
    bn_clip_t *clip;
    uint32_t   seq;
} cmd_t;

typedef struct {
    bn_clip_t *clip;                    /* NULL: free */
    uint32_t   pos;
    int16_t    slot;
    bool       loop;
    float      g, g_to;                 /* gain now and where it goes */
    int        fade;                    /* > 0: fading out, samples left */
    uint32_t   born;                    /* the order it started in */
} voice_t;

typedef struct {
    /* UI -> thread */
    cmd_t             ring[RING];
    volatile uint32_t w;                /* written by the UI */
    volatile uint32_t r;                /* written by the thread */
    uint32_t          seq;              /* UI's: the last command sent */
    uint32_t          last_play[NSLOT]; /* UI's: the seq of each slot's last play */
    /* thread -> UI */
    volatile uint32_t done_seq;
    volatile int8_t   nvoice[NSLOT];
    volatile uint16_t prog[NSLOT];
    volatile uint32_t load_pct;
    /* the thread's own */
    voice_t           v[BN_VOICES];
    uint32_t          born;
    float             lim;              /* the limiter's gain */
    volatile bool     quit, done;
    bool              running;
} eng_t;

static eng_t *E;

static int slot_of(int pad)
{
    if (pad < 0) return 0;
    if (pad >= BN_PADS) return -1;
    return pad + 1;
}

/* ---- the UI's side ---- */

static bool push(const cmd_t *c)
{
    if (!E || !E->running) return false;
    uint32_t w = E->w;
    if (w - E->r >= RING) return false;         /* full: the thread is behind */
    E->ring[w % RING] = *c;
    BN_FENCE();
    E->w = w + 1;
    return true;
}

bool bn_eng_play(int pad, bn_clip_t *clip, float gain, bool loop)
{
    int s = slot_of(pad);
    if (s < 0 || !clip || !E) return false;
    cmd_t c = { .type = C_PLAY, .loop = loop, .slot = (int16_t)s, .gain = gain, .clip = clip,
                .seq = E->seq + 1 };
    clip->starts++;
    BN_FENCE();
    if (!push(&c)) {
        clip->starts--;
        return false;
    }
    E->seq = c.seq;
    E->last_play[s] = c.seq;
    return true;
}

static void push_simple(cmd_type_t t, int pad, float gain)
{
    if (!E) return;
    int s = pad == -2 ? 0 : slot_of(pad);
    if (s < 0) return;
    cmd_t c = { .type = (uint8_t)t, .slot = (int16_t)s, .gain = gain, .seq = E->seq + 1 };
    if (push(&c)) E->seq = c.seq;
}

void bn_eng_stop_pad(int pad) { push_simple(C_STOP, pad, 0.0f); }
void bn_eng_stop_all(void)    { push_simple(C_STOP_ALL, -2, 0.0f); }
void bn_eng_gain(int pad, float gain) { push_simple(C_GAIN, pad, gain); }

int bn_eng_voices(int pad)
{
    int s = slot_of(pad);
    return E && s >= 0 ? E->nvoice[s] : 0;
}

int bn_eng_progress(int pad)
{
    int s = slot_of(pad);
    return E && s >= 0 ? E->prog[s] : 0;
}

bool bn_eng_busy(int pad)
{
    int s = slot_of(pad);
    if (!E || s < 0) return false;
    if (E->nvoice[s] > 0) return true;
    return (int32_t)(E->last_play[s] - E->done_seq) > 0;
}

uint32_t bn_eng_load_pct(void) { return E ? E->load_pct : 0; }

bool bn_eng_clip_idle(const bn_clip_t *c)
{
    if (!c) return true;
    BN_FENCE();
    return c->starts == c->ends;
}

bool bn_eng_running(void) { return E && E->running; }

/* ---- the thread ---- */

static void voice_end(voice_t *v)
{
    if (!v->clip) return;
    v->clip->ends++;
    v->clip = NULL;
}

static void take_commands(eng_t *e)
{
    while (e->r != e->w) {
        BN_FENCE();
        cmd_t c = e->ring[e->r % RING];
        BN_FENCE();
        e->r = e->r + 1;
        switch (c.type) {
        case C_PLAY: {
            voice_t *v = NULL;
            for (int i = 0; i < BN_VOICES && !v; i++) if (!e->v[i].clip) v = &e->v[i];
            if (!v) {
                /* all taken: the oldest one gives its place */
                voice_t *old = &e->v[0];
                for (int i = 1; i < BN_VOICES; i++) if (e->v[i].born < old->born) old = &e->v[i];
                voice_end(old);
                v = old;
            }
            v->clip = c.clip;
            v->pos = 0;
            v->slot = c.slot;
            v->loop = c.loop != 0;
            v->g = v->g_to = c.gain;
            v->fade = 0;
            v->born = ++e->born;
            break;
        }
        case C_STOP:
            for (int i = 0; i < BN_VOICES; i++)
                if (e->v[i].clip && e->v[i].slot == c.slot && !e->v[i].fade) e->v[i].fade = FADE_N;
            break;
        case C_STOP_ALL:
            for (int i = 0; i < BN_VOICES; i++)
                if (e->v[i].clip && !e->v[i].fade) e->v[i].fade = FADE_N;
            break;
        case C_GAIN:
            for (int i = 0; i < BN_VOICES; i++)
                if (e->v[i].clip && e->v[i].slot == c.slot) e->v[i].g_to = c.gain;
            break;
        }
        e->done_seq = c.seq;
    }
}

static void mix_block(eng_t *e, int16_t *out)
{
    float acc[BN_BLK];
    memset(acc, 0, sizeof acc);
    for (int i = 0; i < BN_VOICES; i++) {
        voice_t *v = &e->v[i];
        bn_clip_t *c = v->clip;
        if (!c) continue;
        const int16_t *p = c->pcm;
        float g = v->g, step = (v->g_to - v->g) / (float)BN_BLK;
        for (int k = 0; k < BN_BLK; k++) {
            if (v->pos >= c->n) {
                if (v->loop && c->n) v->pos = 0;
                else break;
            }
            float s = (float)p[v->pos++] * g;
            if (v->fade) {
                s *= (float)v->fade / (float)FADE_N;
                if (--v->fade == 0) {
                    v->pos = c->n;      /* done */
                    v->loop = false;
                }
            }
            acc[k] += s;
            g += step;
        }
        v->g = v->g_to;
        if (v->pos >= c->n && !v->loop) voice_end(v);
    }
    /* the limiter: many sounds at once must not clip. It comes down at once
     * to what the loudest sample of the block needs and goes back up slowly
     * (~0.3 s) */
    float peak = 0.0f;
    for (int k = 0; k < BN_BLK; k++) {
        float a = fabsf(acc[k]);
        if (a > peak) peak = a;
    }
    float need = peak > 30000.0f ? 30000.0f / peak : 1.0f;
    float g0 = e->lim, g1;
    if (need < g0) g1 = need;
    else g1 = g0 + (1.0f - g0) * 0.007f;
    /* coming down it is the new gain for the whole block (a ramp would let
     * the block's first samples through too loud); going up, a ramp */
    if (g1 < g0) g0 = g1;
    for (int k = 0; k < BN_BLK; k++) {
        float g = g0 + (g1 - g0) * (float)k / (float)BN_BLK;
        float s = acc[k] * g;
        out[k] = (int16_t)(s > 32767.0f ? 32767 : s < -32768.0f ? -32768 : (int)s);
    }
    e->lim = g1;
}

static void publish(eng_t *e)
{
    int8_t n[NSLOT];
    memset(n, 0, sizeof n);
    uint32_t newest[NSLOT];
    memset(newest, 0, sizeof newest);
    for (int i = 0; i < BN_VOICES; i++) {
        voice_t *v = &e->v[i];
        if (!v->clip || v->slot < 0 || v->slot >= NSLOT) continue;
        n[v->slot]++;
        if (v->born >= newest[v->slot]) {
            newest[v->slot] = v->born;
            e->prog[v->slot] = (uint16_t)(v->clip->n ? v->pos * 1000u / v->clip->n : 0);
        }
    }
    for (int s = 0; s < NSLOT; s++) {
        if (e->nvoice[s] != n[s]) e->nvoice[s] = n[s];
        if (!n[s] && e->prog[s]) e->prog[s] = 0;
    }
    BN_FENCE();
}

static bool out_low(void)
{
#if defined(AOS_SIM)
    return false;
#else
    return aos_hal_spk_latency_ms() <= 12;
#endif
}

static void audio_main(void *arg)
{
    eng_t *e = arg;
    int16_t pcm[BN_BLK];
    uint32_t t_win = (uint32_t)aos_hal_uptime_ms(), us_sum = 0, blocks = 0;
#if defined(AOS_SIM)
    /* with no sound device (SDL_AUDIODRIVER=dummy) the queue always reads
     * empty: keep to the clock as well */
    uint64_t clk0 = aos_hal_uptime_us(), made = 0;
    /* BN_SIM_TAP=<file>: what the mixer makes, as raw 16-bit mono at
     * BN_RATE, to look at without listening */
    const char *tap_path = getenv("BN_SIM_TAP");
    FILE *tap = tap_path ? fopen(tap_path, "ab") : NULL;
#endif
    while (!e->quit) {
        int target = out_low() ? 256 : 640;     /* 8 ms on the live output, 20 on the usual */
        int q = aos_hal_spk_queued();
        bool full = q + BN_BLK > target;
#if defined(AOS_SIM)
        if ((made * 1000000u) / BN_RATE > aos_hal_uptime_us() - clk0 + 20000u) full = true;
#endif
        if (full) {
            take_commands(e);           /* a stop is not left waiting for room */
            aos_hal_sleep_ms(1);
            continue;
        }
#if defined(AOS_SIM)
        made += BN_BLK;
#endif
        uint64_t t0 = aos_hal_uptime_us();
        take_commands(e);
        mix_block(e, pcm);
        publish(e);
        us_sum += (uint32_t)(aos_hal_uptime_us() - t0);
        blocks++;
        aos_hal_spk_write(pcm, BN_BLK);
#if defined(AOS_SIM)
        if (tap) fwrite(pcm, sizeof pcm[0], BN_BLK, tap);
#endif
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        if (now - t_win >= 5000) {
            float blk_us = (float)BN_BLK * 1e6f / (float)BN_RATE;
            uint32_t avg = blocks ? us_sum / blocks : 0;
            e->load_pct = (uint32_t)(100.0f * (float)avg / blk_us);
            int nv = 0;
            for (int i = 0; i < BN_VOICES; i++) nv += e->v[i].clip != NULL;
            if (nv) aos_hal_log(BN_TAG, "audio: %u us per %u us block (%u%%), %d voices, queue %d, dry %d",
                                (unsigned)avg, (unsigned)blk_us, (unsigned)e->load_pct, nv, q,
                                aos_hal_spk_dry());
            us_sum = blocks = 0;
            t_win = now;
        }
    }
    /* what was playing ends here: the UI may free the clips */
    for (int i = 0; i < BN_VOICES; i++) voice_end(&e->v[i]);
    while (e->r != e->w) {
        BN_FENCE();
        cmd_t c = e->ring[e->r % RING];
        if (c.type == C_PLAY && c.clip) c.clip->ends++;
        e->r = e->r + 1;
        e->done_seq = c.seq;
    }
    publish(e);
#if defined(AOS_SIM)
    if (tap) fclose(tap);
#endif
    e->done = true;
}

bool bn_eng_start(void)
{
    if (E && E->running) return true;
    if (!E) {
        E = bn_alloc(sizeof *E);
        if (!E) return false;
    }
    E->lim = 1.0f;
    if (!aos_hal_spk_open_low(BN_RATE)) {
        aos_hal_log(BN_TAG, "the speaker did not open");
        return false;
    }
    E->quit = false;
    E->done = false;
    E->running = true;
    /* above the player (5) and LVGL (4): a late block is a hole in the
     * sound. It sleeps whenever the ring is full, so it never holds the
     * core. The stack is small: the block is 64 samples. */
    if (!aos_hal_thread_start("bn_audio", audio_main, E, 6144, 6)) {
        E->running = false;
        aos_hal_spk_close();
        aos_hal_log(BN_TAG, "no audio thread");
        return false;
    }
    return true;
}

void bn_eng_stop(void)
{
    if (!E) return;
    if (E->running) {
        E->quit = true;
        for (int i = 0; i < 400 && !E->done; i++) aos_hal_sleep_ms(5);
        if (!E->done) aos_hal_log(BN_TAG, "the audio thread did not stop");
        E->running = false;
        aos_hal_spk_close();
    }
    if (E->done || !E->quit) {
        bn_free(E);
        E = NULL;
    }
}
