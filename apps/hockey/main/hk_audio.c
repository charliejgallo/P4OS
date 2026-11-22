/*
 * HOCKEY - the sound: the effects and the air bed's blower (a loop)
 * synthesised by tools/sounds.py, mu-law at 16 kHz in the common pack;
 * eight voices over the loop on the streaming speaker. The blower runs
 * while a match is on. Pumped from LVGL's thread (Metegol's mixer).
 */
#include "hk_app.h"

#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

#define RATE    16000
#define TARGET  1800
#define CHUNK   320
#define VOICES  8

static const char *const NAMES[SND_N] = { "hit", "hit_soft", "wall", "post", "goal", "place", "buzz", "buzz_end", "tap" };
static const uint8_t LEVEL[SND_N] = {
    [SND_HIT] = 240, [SND_HIT_SOFT] = 190, [SND_WALL] = 200, [SND_POST] = 210, [SND_GOAL] = 255,
    [SND_PLACE] = 150, [SND_BUZZ] = 170, [SND_BUZZ_END] = 180, [SND_TAP] = 140,
};

typedef struct {
    const uint8_t *data;
    uint32_t len, pos;
    int      vol;
    int      snd;
} voice_t;

static struct {
    bool     open;
    bool     sfx_on, air_on, paused, air;
    uint8_t *snd[SND_N];
    uint32_t snd_len[SND_N];
    uint8_t *loop;
    uint32_t loop_len, loop_pos;
    float    air_now;
    voice_t  v[VOICES];
    int16_t  ulaw[256];
} A;

static void ulaw_table(void)
{
    for (int i = 0; i < 256; i++) {
        int u = ~i & 0xFF;
        int t = ((u & 0x0F) << 3) + 0x84;
        t <<= (u & 0x70) >> 4;
        A.ulaw[i] = (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
    }
}

static uint8_t *load(hk_pak_t *pak, const char *name, uint32_t *len)
{
    char n[HK_NAME_LEN];
    snprintf(n, sizeof n, "s_%.20s", name);
    const hk_entry_t *e = hk_pak_find(pak, n);
    if (!e || e->type != PK_PCM) return NULL;
    uint8_t *d = (uint8_t *)hk_pak_load(pak, e);
    *len = d ? e->rawlen : 0;
    return d;
}

bool hk_audio_open(hk_pak_t *pak)
{
    bool s = A.sfx_on, r = A.air_on;
    hk_audio_close();
    A.sfx_on = s;
    A.air_on = r;
    ulaw_table();
    for (int i = 0; i < SND_N; i++) A.snd[i] = load(pak, NAMES[i], &A.snd_len[i]);
    A.loop = load(pak, "air", &A.loop_len);
    A.open = aos_hal_spk_open(RATE);
    if (!A.open) aos_hal_log("hockey", "no speaker");
    return A.open;
}

void hk_audio_close(void)
{
    if (A.open) aos_hal_spk_close();
    for (int i = 0; i < SND_N; i++) hk_free(A.snd[i]);
    hk_free(A.loop);
    bool s = A.sfx_on, r = A.air_on;
    memset(&A, 0, sizeof A);
    A.sfx_on = s;
    A.air_on = r;
}

void hk_audio_set(bool sfx, bool air)
{
    A.sfx_on = sfx;
    A.air_on = air;
}

void hk_audio_air(bool on)
{
    A.air = on;
}

void hk_audio_pause(bool paused)
{
    A.paused = paused;
}

void hk_audio_play(int snd, int vol)
{
    if (!A.open || !A.sfx_on || snd < 0 || snd >= SND_N || !A.snd[snd]) return;
    vol = vol * (LEVEL[snd] ? LEVEL[snd] : 160) / 255;
    int pick = -1;
    /* the same sound just begun: louder, not twice */
    for (int i = 0; i < VOICES; i++) {
        if (A.v[i].data && A.v[i].snd == snd && A.v[i].pos < 400) {
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

void hk_audio_pump(void)
{
    if (!A.open) return;
    int16_t buf[CHUNK];
    int guard = 0;
    while (aos_hal_spk_queued() < TARGET && guard++ < 16) {
        int32_t mix[CHUNK];
        memset(mix, 0, sizeof mix);
        /* the blower comes up and goes down, it does not click on */
        float want = A.air && A.air_on && A.sfx_on && !A.paused ? 1.0f : 0.0f;
        A.air_now += (want - A.air_now) * 0.06f;
        int lv = (int)(A.air_now * 70.0f);
        if (A.loop && A.loop_len && lv > 0) {
            for (int k = 0; k < CHUNK; k++) {
                mix[k] += (A.ulaw[A.loop[A.loop_pos]] * lv) >> 8;
                if (++A.loop_pos >= A.loop_len) A.loop_pos = 0;
            }
        }
        for (int i = 0; i < VOICES; i++) {
            voice_t *v = &A.v[i];
            if (!v->data) continue;
            int n = CHUNK;
            if (v->pos + (uint32_t)n > v->len) n = (int)(v->len - v->pos);
            for (int k = 0; k < n; k++) mix[k] += (A.ulaw[v->data[v->pos + k]] * v->vol) >> 8;
            v->pos += (uint32_t)n;
            if (v->pos >= v->len) v->data = NULL;
        }
        for (int k = 0; k < CHUNK; k++) {
            int32_t s = mix[k];
            if (s > 22000) s = 22000 + (s - 22000) / 4;
            if (s < -22000) s = -22000 + (s + 22000) / 4;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            buf[k] = (int16_t)s;
        }
        if (aos_hal_spk_write(buf, CHUNK) <= 0) break;
    }
}
