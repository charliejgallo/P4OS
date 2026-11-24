/*
 * DJ - MP3 frames, with minimp3 (lieff, CC0-1.0, apps/dj/main/minimp3/).
 *
 * The firmware has minimp3 too, and exports mp3dec_init/decode_frame, but
 * its copy takes the decoder's scratch (16 KB) from right behind the
 * mp3dec_t, a layout that is the system player's own business. So the app
 * brings its copy, under names of its own (in the simulator every app links
 * into one program with the firmware's), with the scratch in PSRAM next to
 * the state the same way, so the threads keep small stacks.
 *
 * Layer III only, integer output.
 */
#include "dj.h"

#include <string.h>

#define mp3dec_init         dj_mp3dec_init
#define mp3dec_decode_frame dj_mp3dec_decode_frame
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

#include "minimp3/minimp3.h"

struct dj_mp3 {
    mp3dec_t dec;               /* first: the cast below depends on it */
    void    *scratch;
};

#define MINIMP3_SCRATCH ((mp3dec_scratch_t *)((struct dj_mp3 *)dec)->scratch)
#define MINIMP3_IMPLEMENTATION
#include "minimp3/minimp3.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

dj_mp3_t *dj_mp3_new(void)
{
    dj_mp3_t *m = dj_alloc(sizeof *m);
    if (!m) return NULL;
    m->scratch = dj_alloc(sizeof(mp3dec_scratch_t));
    if (!m->scratch) { dj_free(m); return NULL; }
    mp3dec_init(&m->dec);
    return m;
}

void dj_mp3_free(dj_mp3_t *m)
{
    if (!m) return;
    dj_free(m->scratch);
    dj_free(m);
}

void dj_mp3_reset(dj_mp3_t *m)
{
    if (m) mp3dec_init(&m->dec);
}

int dj_mp3_frame(dj_mp3_t *m, const uint8_t *in, int n, int16_t *pcm, int *used, int *ch, int *hz)
{
    mp3dec_frame_info_t info;
    memset(&info, 0, sizeof info);
    int s = mp3dec_decode_frame(&m->dec, in, n, pcm, &info);
    *used = info.frame_bytes;
    *ch = info.channels;
    *hz = info.hz;
    return s;
}

/* A frame header: sync, MPEG 1 / 2 / 2.5, layer III, a bitrate that is not
 * free nor bad, a rate that is not reserved. The length, with padding. */
int dj_mp3_hdr(const uint8_t *h, int *hz, int *spf, int *ch)
{
    static const uint16_t BR[2][15] = {
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 },   /* MPEG 1 */
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },        /* MPEG 2 / 2.5 */
    };
    static const uint16_t SR[3] = { 44100, 48000, 32000 };
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
    int ver = (h[1] >> 3) & 3;          /* 3 = MPEG 1, 2 = MPEG 2, 0 = MPEG 2.5 */
    int layer = (h[1] >> 1) & 3;        /* 1 = layer III */
    int bri = h[2] >> 4, sri = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
    if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) return 0;
    int rate = SR[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    int kbps = BR[ver == 3 ? 0 : 1][bri];
    int samples = ver == 3 ? 1152 : 576;
    if (hz) *hz = rate;
    if (spf) *spf = samples;
    if (ch) *ch = ((h[3] >> 6) == 3) ? 1 : 2;
    return samples / 8 * kbps * 1000 / rate + pad;
}
