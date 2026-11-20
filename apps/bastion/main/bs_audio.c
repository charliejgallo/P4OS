/*
 * BASTION - the sound (see bs_audio.h)
 */
#include "bs_audio.h"

#include "bs_game.h"

#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

#define RATE        16000
#define TARGET      1800            /* samples kept queued: ~110 ms          */
#define CHUNK       320
#define VOICES      12

static const char *const NAMES[SND_N] = {
    "shot_gatling", "shot_cannon", "laser", "shot_missile", "zap", "cryo", "flame", "shot_rail", "shot_mortar",
    "drone", "emp", "gravity", "boom_s", "boom_m", "boom_l", "hit", "die1", "die2", "die3", "boss_roar",
    "boss_die", "fort_hit", "alarm", "build", "upgrade", "sell", "coin", "wave_start", "wave_clear", "victory",
    "defeat", "tap", "deny", "strike", "freeze", "repair", "star",
};

/* how loud each is against the rest, 0..255 */
static const uint8_t LEVEL[SND_N] = {
    [SND_GATLING] = 90, [SND_CANNON] = 190, [SND_LASER] = 80, [SND_MISSILE] = 150, [SND_ZAP] = 150, [SND_CRYO] = 110,
    [SND_FLAME] = 100, [SND_RAIL] = 220, [SND_MORTAR] = 200, [SND_DRONE] = 70, [SND_EMP] = 200, [SND_GRAVITY] = 200,
    [SND_BOOM_S] = 150, [SND_BOOM_M] = 200, [SND_BOOM_L] = 235, [SND_HIT] = 80, [SND_DIE1] = 120, [SND_DIE2] = 120,
    [SND_DIE3] = 90, [SND_BOSS_ROAR] = 255, [SND_BOSS_DIE] = 255, [SND_FORT_HIT] = 240, [SND_ALARM] = 220,
    [SND_BUILD] = 220, [SND_UPGRADE] = 230, [SND_SELL] = 210, [SND_COIN] = 150, [SND_WAVE_START] = 240,
    [SND_WAVE_CLEAR] = 240, [SND_VICTORY] = 255, [SND_DEFEAT] = 255, [SND_TAP] = 170, [SND_DENY] = 190,
    [SND_STRIKE] = 255, [SND_FREEZE] = 240, [SND_REPAIR] = 230, [SND_STAR] = 230,
};

typedef struct {
    const uint8_t *data;
    uint32_t len, pos;
    int      vol;
    int      snd;
} voice_t;

static struct {
    bool     open, loaded;
    bool     sfx_on, music_on, paused;
    uint8_t *snd[SND_N];
    uint32_t snd_len[SND_N];
    uint8_t *music;
    uint32_t music_len, music_pos;
    int      track;
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

bool bs_audio_open(bs_pak_t *pak)
{
    bs_audio_close();
    ulaw_table();
    A.track = -1;
    char name[BS_NAME_LEN];
    for (int i = 0; i < SND_N; i++) {
        snprintf(name, sizeof name, "s_%s", NAMES[i]);
        const bs_entry_t *e = bs_pak_find(pak, name);
        if (!e || e->type != PK_PCM) continue;
        A.snd[i] = (uint8_t *)bs_pak_load(pak, e);
        A.snd_len[i] = A.snd[i] ? e->rawlen : 0;
    }
    A.loaded = true;
    A.open = aos_hal_spk_open(RATE);
    if (!A.open) aos_hal_log("bastion", "no speaker");
    return A.open;
}

void bs_audio_close(void)
{
    if (A.open) aos_hal_spk_close();
    for (int i = 0; i < SND_N; i++) bs_free(A.snd[i]);
    bs_free(A.music);
    bool sfx = A.sfx_on, mus = A.music_on;
    memset(&A, 0, sizeof A);
    A.sfx_on = sfx;
    A.music_on = mus;
    A.track = -1;
}

void bs_audio_set(bool sfx, bool music)
{
    A.sfx_on = sfx;
    A.music_on = music;
}

void bs_audio_music(bs_pak_t *pak, int track)
{
    if (!A.loaded || track == A.track) return;
    bs_free(A.music);
    A.music = NULL;
    A.music_len = A.music_pos = 0;
    A.track = track;
    if (track < 0 || !pak) return;
    char name[BS_NAME_LEN];
    if (track == BS_MUSIC_MENU) snprintf(name, sizeof name, "music_menu");
    else snprintf(name, sizeof name, "music%d", track & 3);
    const bs_entry_t *e = bs_pak_find(pak, name);
    if (!e || e->type != PK_PCM) return;
    A.music = (uint8_t *)bs_pak_load(pak, e);
    A.music_len = A.music ? e->rawlen : 0;
}

void bs_audio_pause(bool paused)
{
    A.paused = paused;
}

void bs_audio_play(int snd, int vol)
{
    if (!A.open || !A.sfx_on || snd < 0 || snd >= SND_N || !A.snd[snd]) return;
    vol = vol * (LEVEL[snd] ? LEVEL[snd] : 160) / 255;
    /* the same sound again restarts its voice instead of piling up */
    int pick = -1;
    for (int i = 0; i < VOICES; i++) {
        if (A.v[i].data && A.v[i].snd == snd && A.v[i].pos < 900) {
            pick = i;
            break;
        }
    }
    if (pick < 0) {
        for (int i = 0; i < VOICES; i++) {
            if (!A.v[i].data) {
                pick = i;
                break;
            }
        }
    }
    if (pick < 0) {
        /* all busy: the one nearest its end */
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

void bs_audio_pump(void)
{
    if (!A.open) return;
    int16_t buf[CHUNK];
    int guard = 0;
    while (aos_hal_spk_queued() < TARGET && guard++ < 16) {
        int32_t mix[CHUNK];
        memset(mix, 0, sizeof mix);
        if (A.music && A.music_on && A.music_len && !A.paused) {
            for (int k = 0; k < CHUNK; k++) {
                mix[k] += (A.ulaw[A.music[A.music_pos]] * 170) >> 8;
                if (++A.music_pos >= A.music_len) A.music_pos = 0;
            }
        }
        /* the effects go on under a pause's panel: its buttons click */
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
            /* a soft knee instead of a hard clip when many play at once */
            if (s > 22000) s = 22000 + (s - 22000) / 4;
            if (s < -22000) s = -22000 + (s + 22000) / 4;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            buf[k] = (int16_t)s;
        }
        if (aos_hal_spk_write(buf, CHUNK) <= 0) break;
    }
}
