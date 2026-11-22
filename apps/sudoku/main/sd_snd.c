/*
 * SUDOKU - the sounds: seven sets of eight short sounds, synthesised by
 * tools/pack_p4.py into the pack (mu-law, 16 kHz), one set per family of
 * themes (a soft click for Clásico, a knock of wood, a drop of water, a
 * synth blip...). Mixed here into the streaming speaker, like Bastion: a few
 * voices, a chunk at a time from the 30 ms timer, never more than ~100 ms
 * queued so a tap is heard when it happens.
 */
#include "sd_app.h"
#include "sd_pak.h"

#include <string.h>

#define RATE    16000
#define TARGET  1600
#define CHUNK   320
#define VOICES  6

static const char *const NAMES[SD_SOUNDS] = { "place", "note", "erase", "error", "unit", "win", "tap", "hint" };

typedef struct {
    const uint8_t *data;
    uint32_t len, pos;
    int vol;
} voice_t;

static struct {
    bool     open;
    uint8_t *snd[SD_SOUND_SETS][SD_SOUNDS];
    uint32_t len[SD_SOUND_SETS][SD_SOUNDS];
    voice_t  v[VOICES];
    int16_t  ulaw[256];
    bool     table;
} S;

void sd_snd_load(int set, uint8_t **data, uint32_t *len)
{
    if (set < 0 || set >= SD_SOUND_SETS) return;
    for (int i = 0; i < SD_SOUNDS; i++) {
        if (S.snd[set][i]) continue;
        S.snd[set][i] = data[i];
        S.len[set][i] = len[i];
        data[i] = NULL;
    }
}

void sd_snd_open(void)
{
    if (!S.table) {
        for (int i = 0; i < 256; i++) {
            int u = ~i & 0xFF;
            int t = ((u & 0x0F) << 3) + 0x84;
            t <<= (u & 0x70) >> 4;
            S.ulaw[i] = (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
        }
        S.table = true;
    }
    if (!S.open && sd_D.sound) S.open = aos_hal_spk_open(RATE);
}

void sd_snd_close(void)
{
    if (S.open) aos_hal_spk_close();
    S.open = false;
    memset(S.v, 0, sizeof S.v);
}

void sd_snd_play(int snd)
{
    if (!sd_D.sound) return;
    if (!S.open) sd_snd_open();
    if (!S.open || snd < 0 || snd >= SD_SOUNDS) return;
    int set = sd_theme()->snd;
    const uint8_t *d = S.snd[set][snd];
    uint32_t len = S.len[set][snd];
    if (!d) {
        d = S.snd[0][snd];
        len = S.len[0][snd];
    }
    if (!d) {
        aos_hal_beep(snd == SD_S_ERROR ? 220 : 880, 30);
        return;
    }
    int pick = 0;
    for (int i = 0; i < VOICES; i++) {
        if (!S.v[i].data) {
            pick = i;
            break;
        }
        if (S.v[i].pos > S.v[pick].pos) pick = i;
    }
    S.v[pick].data = d;
    S.v[pick].len = len;
    S.v[pick].pos = 0;
    S.v[pick].vol = 220;
}

void sd_snd_pump(void)
{
    if (!S.open) return;
    if (!sd_D.sound) {
        sd_snd_close();
        return;
    }
    int16_t buf[CHUNK];
    int guard = 0;
    while (aos_hal_spk_queued() < TARGET && guard++ < 8) {
        int32_t mix[CHUNK];
        memset(mix, 0, sizeof mix);
        for (int i = 0; i < VOICES; i++) {
            voice_t *v = &S.v[i];
            if (!v->data) continue;
            int n = CHUNK;
            if (v->pos + (uint32_t)n > v->len) n = (int)(v->len - v->pos);
            for (int k = 0; k < n; k++) mix[k] += (S.ulaw[v->data[v->pos + k]] * v->vol) >> 8;
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

const char *sd_snd_name(int i);
const char *sd_snd_name(int i)
{
    return i >= 0 && i < SD_SOUNDS ? NAMES[i] : "";
}

/* destroy(): the sounds were the pack's, in PSRAM */
void sd_snd_release(void);
void sd_snd_release(void)
{
    sd_snd_close();
    for (int s = 0; s < SD_SOUND_SETS; s++)
        for (int i = 0; i < SD_SOUNDS; i++) {
            sd_free(S.snd[s][i]);
            S.snd[s][i] = NULL;
            S.len[s][i] = 0;
        }
}
