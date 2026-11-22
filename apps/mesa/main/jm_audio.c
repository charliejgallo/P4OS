/*
 * Juegos de mesa - the sounds: a piece set down on wood, a capture, a
 * check, the dice, a disc turned or dropped, a win and a loss, a tap and a
 * "no". Synthesised by tools/sounds.py (nothing recorded), mu-law at
 * 16 kHz in the wooden pack (s_<name>), mixed in eight voices on the
 * streaming speaker, which is open only while something sounds. Pumped
 * from LVGL's timer. Bastion's mixer, smaller.
 */
#include "jm_app.h"

#include "aos_hal.h"

#include <stdio.h>
#include <string.h>

#define RATE   16000
#define TARGET 1600
#define CHUNK  320
#define VOICES 8

static const char *const NAMES[SND_N] = { "move", "capture", "check", "dice", "flip", "drop", "win", "lose", "tap",
                                          "deny" };
static const uint8_t LEVEL[SND_N] = { 220, 235, 170, 200, 150, 210, 200, 190, 120, 140 };

typedef struct {
    const uint8_t *data;
    uint32_t len, pos;
    int vol;
} voice_t;

static struct {
    bool     loaded, open;
    uint8_t *snd[SND_N];
    uint32_t len[SND_N];
    voice_t  v[VOICES];
    int16_t  ulaw[256];
    uint32_t idle_since;
} A;

void jm_audio_open(void)
{
    jm_audio_close();
    for (int i = 0; i < 256; i++) {
        int u = ~i & 0xFF;
        int t = ((u & 0x0F) << 3) + 0x84;
        t <<= (u & 0x70) >> 4;
        A.ulaw[i] = (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
    }
    char p[160];
    snprintf(p, sizeof p, "%s/mesa_p4.pak", aos_hal_path_apps());
    jm_pak_t pak;
    if (!jm_pak_open(&pak, p)) return;
    for (int i = 0; i < SND_N; i++) {
        char n[JM_NAME_LEN];
        snprintf(n, sizeof n, "s_%s", NAMES[i]);
        const jm_entry_t *e = jm_pak_find(&pak, n);
        if (!e || e->type != PK_PCM) continue;
        A.snd[i] = (uint8_t *)jm_pak_load(&pak, e);
        A.len[i] = A.snd[i] ? e->rawlen : 0;
    }
    jm_pak_close(&pak);
    A.loaded = true;
}

void jm_audio_close(void)
{
    if (A.open) aos_hal_spk_close();
    for (int i = 0; i < SND_N; i++) jm_free(A.snd[i]);
    memset(&A, 0, sizeof A);
}

void jm_audio_play(int snd)
{
    if (!jm || !jm->prefs.sound || snd < 0 || snd >= SND_N || !A.snd[snd]) return;
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

void jm_audio_pump(void)
{
    if (!A.open) return;
    bool any = false;
    for (int i = 0; i < VOICES; i++) any |= A.v[i].data != NULL;
    if (!any) {
        /* the speaker goes back to the system a while after the last sound */
        uint32_t now = jm_now();
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
