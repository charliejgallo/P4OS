/*
 * METEGOL - the sound: the effects and two loops (the crowd's murmur, the
 * menus' music) synthesised by tools/sounds.py, mu-law at 16 kHz in the
 * common pack; eight voices over the loops on the streaming speaker. The
 * murmur's loudness follows the ball (mg_audio_crowd): it swells as the
 * ball nears a goal. Pumped from LVGL's thread (Bastion's mixer).
 */
#include "mg_app.h"

#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

#define RATE    16000
#define TARGET  1800
#define CHUNK   320
#define VOICES  8

static const char *const NAMES[SND_N] = { "kick", "kick_soft", "wall", "post", "net", "whistle", "whistle_end",
                                          "roar", "serve", "stop", "tap", "ooh" };
static const uint8_t LEVEL[SND_N] = {
    [SND_KICK] = 230, [SND_KICK_SOFT] = 170, [SND_WALL] = 170, [SND_POST] = 230, [SND_NET] = 210,
    [SND_WHISTLE] = 200, [SND_WHISTLE_END] = 210, [SND_ROAR] = 255, [SND_SERVE] = 140, [SND_STOP] = 150,
    [SND_TAP] = 150, [SND_OOH] = 200,
};

typedef struct {
    const uint8_t *data;
    uint32_t len, pos;
    int      vol;
    int      snd;
} voice_t;

typedef struct {
    uint8_t *data;
    uint32_t len, pos;
} loop_t;

static struct {
    bool     open, loaded;
    bool     sfx_on, music_on, crowd_on, paused, music;
    uint8_t *snd[SND_N];
    uint32_t snd_len[SND_N];
    loop_t   crowd, tune;
    float    crowd_level, crowd_now;
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

static uint8_t *load(mg_pak_t *pak, const char *name, uint32_t *len)
{
    char n[MG_NAME_LEN];
    snprintf(n, sizeof n, "s_%.20s", name);
    const mg_entry_t *e = mg_pak_find(pak, n);
    if (!e || e->type != PK_PCM) return NULL;
    uint8_t *d = (uint8_t *)mg_pak_load(pak, e);
    *len = d ? e->rawlen : 0;
    return d;
}

bool mg_audio_open(mg_pak_t *pak)
{
    bool s = A.sfx_on, m = A.music_on, c = A.crowd_on;
    mg_audio_close();
    A.sfx_on = s;
    A.music_on = m;
    A.crowd_on = c;
    ulaw_table();
    for (int i = 0; i < SND_N; i++) A.snd[i] = load(pak, NAMES[i], &A.snd_len[i]);
    A.crowd.data = load(pak, "crowd", &A.crowd.len);
    A.tune.data = load(pak, "music_menu", &A.tune.len);
    A.crowd_level = -1;
    A.loaded = true;
    A.open = aos_hal_spk_open(RATE);
    if (!A.open) aos_hal_log("metegol", "no speaker");
    return A.open;
}

void mg_audio_close(void)
{
    if (A.open) aos_hal_spk_close();
    for (int i = 0; i < SND_N; i++) mg_free(A.snd[i]);
    mg_free(A.crowd.data);
    mg_free(A.tune.data);
    bool s = A.sfx_on, m = A.music_on, c = A.crowd_on;
    memset(&A, 0, sizeof A);
    A.sfx_on = s;
    A.music_on = m;
    A.crowd_on = c;
}

void mg_audio_set(bool sfx, bool music, bool crowd)
{
    A.sfx_on = sfx;
    A.music_on = music;
    A.crowd_on = crowd;
}

void mg_audio_music(bool on)
{
    A.music = on;
}

void mg_audio_crowd(float level)
{
    A.crowd_level = level;
}

void mg_audio_pause(bool paused)
{
    A.paused = paused;
}

void mg_audio_play(int snd, int vol)
{
    if (!A.open || !A.sfx_on || snd < 0 || snd >= SND_N || !A.snd[snd]) return;
    /* the crowd's own voices go with the crowd's switch */
    if ((snd == SND_ROAR || snd == SND_OOH) && !A.crowd_on) return;
    vol = vol * (LEVEL[snd] ? LEVEL[snd] : 160) / 255;
    int pick = -1;
    for (int i = 0; i < VOICES; i++) {
        if (A.v[i].data && A.v[i].snd == snd && A.v[i].pos < 600) {
            pick = i;
            break;
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

static void loop_mix(int32_t *mix, loop_t *l, int vol)
{
    if (!l->data || !l->len || vol <= 0) return;
    for (int k = 0; k < CHUNK; k++) {
        mix[k] += (A.ulaw[l->data[l->pos]] * vol) >> 8;
        if (++l->pos >= l->len) l->pos = 0;
    }
}

void mg_audio_pump(void)
{
    if (!A.open) return;
    int16_t buf[CHUNK];
    int guard = 0;
    while (aos_hal_spk_queued() < TARGET && guard++ < 16) {
        int32_t mix[CHUNK];
        memset(mix, 0, sizeof mix);
        if (!A.paused) {
            if (A.music && A.music_on) loop_mix(mix, &A.tune, 150);
            /* the murmur glides to its level */
            float want = A.crowd_on && A.crowd_level >= 0 ? A.crowd_level : 0;
            A.crowd_now += (want - A.crowd_now) * 0.08f;
            loop_mix(mix, &A.crowd, (int)(A.crowd_now * 120.0f));
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
