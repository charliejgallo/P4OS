/*
 * Tortuga - the sounds (mu-law in the pack, tools/pack_p4.py), mixed into
 * the streaming speaker while something plays: Juegos de mesa's mixer.
 */
#include "tg_app.h"

#include <stdio.h>
#include <string.h>

#define RATE   16000
#define TARGET 1600
#define CHUNK  320
#define VOICES 6

static const char *const NAMES[SND_N] = { "step", "turn", "bump", "splash", "eat", "key", "door", "win", "fail",
                                          "tap" };
static const uint8_t LEVEL[SND_N] = { 150, 110, 220, 200, 190, 170, 190, 210, 190, 110 };

typedef struct {
    const uint8_t *data;
    uint32_t len, pos;
    int vol;
} voice_t;

static struct {
    bool     open;
    uint8_t *snd[SND_N];
    uint32_t len[SND_N];
    voice_t  v[VOICES];
    int16_t  ulaw[256];
    uint32_t idle_since;
} A;

void tg_audio_open(void)
{
    tg_audio_close();
    for (int i = 0; i < 256; i++) {
        int u = ~i & 0xFF;
        int t = ((u & 0x0F) << 3) + 0x84;
        t <<= (u & 0x70) >> 4;
        A.ulaw[i] = (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
    }
    char p[160];
    snprintf(p, sizeof p, "%s/%s", aos_hal_path_apps(), TG_PACK);
    tg_pak_t pak;
    if (!tg_pak_open(&pak, p)) return;
    for (int i = 0; i < SND_N; i++) {
        char n[TGX_NAME_LEN];
        snprintf(n, sizeof n, "snd_%s", NAMES[i]);
        const tg_entry_t *e = tg_pak_find(&pak, n);
        if (!e || e->type != PK_PCM) continue;
        A.snd[i] = (uint8_t *)tg_pak_load(&pak, e);
        A.len[i] = A.snd[i] ? e->rawlen : 0;
    }
    tg_pak_close(&pak);
}

void tg_audio_close(void)
{
    if (A.open) aos_hal_spk_close();
    for (int i = 0; i < SND_N; i++) tg_free(A.snd[i]);
    memset(&A, 0, sizeof A);
}

void tg_audio_play(int snd)
{
    if (!tg || !tg->prefs.sound || snd < 0 || snd >= SND_N || !A.snd[snd]) return;
    if (!A.open) {
        A.open = aos_hal_spk_open(RATE);
        if (!A.open) return;
    }
    int pick = -1;
    for (int i = 0; i < VOICES && pick < 0; i++) {
        if (!A.v[i].data) pick = i;
    }
    if (pick < 0) pick = 0;
    A.v[pick].data = A.snd[snd];
    A.v[pick].len = A.len[snd];
    A.v[pick].pos = 0;
    A.v[pick].vol = LEVEL[snd];
    A.idle_since = 0;
}

void tg_audio_pump(void)
{
    if (!A.open) return;
    bool any = false;
    for (int i = 0; i < VOICES; i++) any |= A.v[i].data != NULL;
    if (!any) {
        /* the speaker goes back to the system a while after the last sound */
        uint32_t now = lv_tick_get();
        if (!A.idle_since) A.idle_since = now;
        if (now - A.idle_since > 3000) {
            aos_hal_spk_close();
            A.open = false;
        }
        return;
    }
    int16_t buf[CHUNK];
    int guard = 0;
    while (aos_hal_spk_queued() < TARGET && guard++ < 12) {
        int32_t mix[CHUNK];
        memset(mix, 0, sizeof mix);
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
            buf[k] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
        if (aos_hal_spk_write(buf, CHUNK) <= 0) break;
    }
}
