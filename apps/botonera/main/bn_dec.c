/*
 * Botonera - WAV and MP3 to the mixer's PCM: BN_RATE, mono, 16 bits, in
 * PSRAM, at most BN_CLIP_MAX_S seconds.
 *
 * Decoding happens once, when a page is shown or a file is chosen, on the
 * loader thread (bn_ui.c); after that a touch only points the mixer at the
 * samples, so it sounds at once and as many times as wanted.
 *
 * WAV: PCM of 8, 16, 24 and 32 bits, 32-bit float, the extensible header,
 * any number of channels (they are averaged), any rate. MP3: minimp3 (lieff,
 * CC0-1.0, apps/botonera/main/minimp3/), the same copy DJ brings, under names
 * of its own (in the simulator every app links into one program) and with the
 * decoder's scratch (16 KB) in PSRAM next to its state, so the loader's stack
 * stays small. The rate is changed by linear interpolation, which is plenty
 * for a small speaker.
 */
#include "bn.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM)
#include "esp_heap_caps.h"
#endif

#define mp3dec_init             bn_mp3dec_init
#define mp3dec_decode_frame     bn_mp3dec_decode_frame
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

typedef struct {
    mp3dec_t dec;               /* first: the cast below depends on it */
    void    *scratch;
} bn_mp3_t;

#define MINIMP3_SCRATCH ((mp3dec_scratch_t *)((bn_mp3_t *)dec)->scratch)
#define MINIMP3_IMPLEMENTATION
#include "minimp3/minimp3.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

volatile bool bn_dec_abort;

void *bn_alloc(size_t bytes)
{
#if defined(AOS_SIM)
    void *p = malloc(bytes);
#else
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (p) memset(p, 0, bytes);
    return p;
}

void bn_free(void *p)
{
    if (!p) return;
#if defined(AOS_SIM)
    free(p);
#else
    heap_caps_free(p);
#endif
}

/* ---- the output: resampled as it comes ---- */

typedef struct {
    int16_t *pcm;
    uint32_t n, cap;            /* samples */
    uint32_t src_rate;
    float    step;              /* source samples per output sample */
    float    t;                 /* where the next output falls, from 'prev' */
    float    prev;              /* the last source sample of the previous chunk */
    bool     started, cut;
} sink_t;

static bool sink_init(sink_t *s, uint32_t rate, uint32_t est_src)
{
    memset(s, 0, sizeof *s);
    if (rate < 4000 || rate > 192000) return false;
    s->src_rate = rate;
    s->step = (float)rate / (float)BN_RATE;
    uint32_t want = est_src ? (uint32_t)((float)est_src / s->step) + 64 : BN_RATE * 4;
    if (want > BN_RATE * BN_CLIP_MAX_S) want = BN_RATE * BN_CLIP_MAX_S;
    s->cap = want;
    s->pcm = bn_alloc(s->cap * sizeof(int16_t));
    return s->pcm != NULL;
}

static bool sink_grow(sink_t *s)
{
    uint32_t cap = s->cap * 2;
    if (cap > BN_RATE * BN_CLIP_MAX_S) cap = BN_RATE * BN_CLIP_MAX_S;
    if (cap <= s->cap) return false;
    int16_t *p = bn_alloc(cap * sizeof(int16_t));
    if (!p) return false;
    memcpy(p, s->pcm, s->n * sizeof(int16_t));
    bn_free(s->pcm);
    s->pcm = p;
    s->cap = cap;
    return true;
}

/* 'm' mono source samples (as float, -32768..32767). Returns false once it
 * is full (the length cap): there is no point in decoding further. */
static bool sink_put(sink_t *s, const float *m, int cnt)
{
    if (cnt <= 0) return true;
    if (!s->started) {
        s->prev = m[0];
        s->started = true;
        s->t = 0.0f;
    }
    /* positions are counted from 'prev' (index -1 of this chunk) */
    while (true) {
        float pos = s->t;               /* 0 = prev, 1 = m[0] ... */
        int i = (int)pos;
        if (i >= cnt) break;
        float a = i == 0 ? s->prev : m[i - 1];
        float b = m[i];
        float f = pos - (float)i;
        float v = a + (b - a) * f;
        if (s->n >= s->cap && !sink_grow(s)) {
            s->cut = true;
            return false;
        }
        s->pcm[s->n++] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
        s->t += s->step;
    }
    s->t -= (float)cnt;
    s->prev = m[cnt - 1];
    return true;
}

static bn_clip_t *sink_finish(sink_t *s)
{
    if (!s->n) {
        bn_free(s->pcm);
        return NULL;
    }
    bn_clip_t *c = bn_alloc(sizeof *c);
    if (!c) {
        bn_free(s->pcm);
        return NULL;
    }
    /* give back what the estimate took of more, if it is worth it */
    if (s->cap - s->n > 16384) {
        int16_t *p = bn_alloc(s->n * sizeof(int16_t));
        if (p) {
            memcpy(p, s->pcm, s->n * sizeof(int16_t));
            bn_free(s->pcm);
            s->pcm = p;
        }
    }
    /* no fades at the ends: a loop of rain or a siren is continuous from
     * its last sample to its first, and a dip there would tick every turn */
    c->pcm = s->pcm;
    c->n = s->n;
    c->cut = s->cut;
    return c;
}

void bn_clip_free(bn_clip_t *c)
{
    if (!c) return;
    bn_free(c->pcm);
    bn_free(c);
}

/* ---- WAV ---- */

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

#define CHUNK 8192

static bn_clip_t *dec_wav(FILE *f, uint32_t fsize, char *why, size_t why_n)
{
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        snprintf(why, why_n, "wav");
        return NULL;
    }
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0, data_len = 0;
    bool have_fmt = false, have_data = false;
    while (!have_data) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        uint32_t len = rd32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            uint8_t b[40];
            uint32_t k = len < sizeof b ? len : sizeof b;
            if (fread(b, 1, k, f) != k || k < 16) break;
            fmt = rd16(b);
            ch = rd16(b + 2);
            rate = rd32(b + 4);
            bits = rd16(b + 14);
            if (fmt == 0xFFFE && k >= 26) fmt = rd16(b + 24);     /* extensible: the subformat */
            have_fmt = true;
            if (len > k) fseek(f, (long)(len - k), SEEK_CUR);
            if (len & 1) fseek(f, 1, SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) {
            long here = ftell(f);
            uint32_t rest = fsize > (uint32_t)here ? fsize - (uint32_t)here : 0;
            /* a recording cut short says 0 (or too much): what is there counts */
            data_len = len && len <= rest ? len : rest;
            have_data = true;
        } else {
            fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
    if (!have_fmt || !have_data || !ch || ch > 8) {
        snprintf(why, why_n, "wav");
        return NULL;
    }
    bool flt = fmt == 3 && bits == 32;
    if (!(fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !flt) {
        snprintf(why, why_n, "wav %u/%u", (unsigned)fmt, (unsigned)bits);
        return NULL;
    }
    uint32_t bps = bits / 8u, frame = bps * ch;
    uint32_t frames = data_len / frame;
    sink_t s;
    if (!sink_init(&s, rate, frames)) {
        snprintf(why, why_n, rate ? "mem" : "wav");
        return NULL;
    }
    uint8_t *buf = bn_alloc(CHUNK);
    float *mono = bn_alloc((CHUNK / 1) * sizeof(float));
    if (!buf || !mono) {
        bn_free(buf);
        bn_free(mono);
        bn_free(s.pcm);
        snprintf(why, why_n, "mem");
        return NULL;
    }
    uint32_t per = CHUNK / frame;
    float inv = 1.0f / (float)ch;
    while (frames) {
        uint32_t want = frames < per ? frames : per;
        size_t got = fread(buf, frame, want, f);
        if (!got) break;
        frames -= (uint32_t)got;
        for (size_t i = 0; i < got; i++) {
            const uint8_t *p = buf + i * frame;
            float acc = 0.0f;
            for (int c = 0; c < ch; c++, p += bps) {
                float v;
                if (flt) {
                    union { uint32_t u; float f; } u = { .u = rd32(p) };
                    v = u.f * 32767.0f;
                } else if (bits == 16) {
                    v = (float)(int16_t)rd16(p);
                } else if (bits == 8) {
                    v = (float)((int)p[0] - 128) * 256.0f;
                } else if (bits == 24) {
                    int32_t x = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
                    v = (float)(x >> 16);
                } else {
                    v = (float)((int32_t)rd32(p) >> 16);
                }
                acc += v;
            }
            mono[i] = acc * inv;
        }
        if (!sink_put(&s, mono, (int)got) || bn_dec_abort) break;
    }
    bn_free(buf);
    bn_free(mono);
    return sink_finish(&s);
}

/* ---- MP3 ---- */

#define MP3_BUF (16 * 1024)

static bn_clip_t *dec_mp3(FILE *f, uint32_t fsize, char *why, size_t why_n)
{
    bn_mp3_t *m = bn_alloc(sizeof *m);
    uint8_t *in = bn_alloc(MP3_BUF);
    int16_t *pcm = bn_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    float *mono = bn_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME / 2 * sizeof(float));
    if (m) m->scratch = bn_alloc(sizeof(mp3dec_scratch_t));
    if (!m || !in || !pcm || !mono || !m->scratch) {
        if (m) bn_free(m->scratch);
        bn_free(m);
        bn_free(in);
        bn_free(pcm);
        bn_free(mono);
        snprintf(why, why_n, "mem");
        return NULL;
    }
    mp3dec_init(&m->dec);
    /* an ID3v2 tag in front is skipped whole: a cover image in it is
     * hundreds of KB that minimp3 would wade through looking for frames */
    uint8_t id3[10];
    if (fread(id3, 1, 10, f) == 10 && !memcmp(id3, "ID3", 3)) {
        uint32_t sz = (uint32_t)(id3[6] & 0x7F) << 21 | (uint32_t)(id3[7] & 0x7F) << 14 |
                      (uint32_t)(id3[8] & 0x7F) << 7 | (id3[9] & 0x7F);
        fseek(f, (long)(10 + sz), SEEK_SET);
    } else {
        fseek(f, 0, SEEK_SET);
    }
    sink_t s;
    bool inited = false, go = true;
    int have = 0;
    bool eof = false;
    while (go) {
        if (!eof && have < MP3_BUF) {
            size_t got = fread(in + have, 1, (size_t)(MP3_BUF - have), f);
            if (got == 0) eof = true;
            have += (int)got;
        }
        if (have <= 0) break;
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof info);
        int ns = mp3dec_decode_frame(&m->dec, in, have, pcm, &info);
        int used = info.frame_bytes;
        if (used <= 0) {
            if (eof) break;
            used = have > 1 ? have - 1 : have;  /* garbage: move on */
        }
        if (ns > 0) {
            if (!inited) {
                /* the length from the first frame's bitrate, to size the
                 * output once */
                uint32_t est = info.bitrate_kbps > 0
                    ? (uint32_t)((float)fsize * 8.0f / (float)info.bitrate_kbps / 1000.0f * (float)info.hz)
                    : 0;
                if (!sink_init(&s, (uint32_t)info.hz, est)) {
                    snprintf(why, why_n, "mem");
                    go = false;
                    break;
                }
                inited = true;
            }
            int chn = info.channels > 1 ? 2 : 1;
            for (int i = 0; i < ns; i++)
                mono[i] = chn == 2 ? ((float)pcm[2 * i] + (float)pcm[2 * i + 1]) * 0.5f : (float)pcm[i];
            if (!sink_put(&s, mono, ns)) go = false;
        }
        if (bn_dec_abort) {
            go = false;
            break;
        }
        memmove(in, in + used, (size_t)(have - used));
        have -= used;
    }
    bn_free(m->scratch);
    bn_free(m);
    bn_free(in);
    bn_free(pcm);
    bn_free(mono);
    if (!inited) {
        if (!why[0]) snprintf(why, why_n, "mp3");
        return NULL;
    }
    return sink_finish(&s);
}

bn_clip_t *bn_decode(const char *path, char *why, size_t why_n)
{
    why[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(why, why_n, "open");
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t magic[4] = { 0 };
    size_t got = fread(magic, 1, 4, f);
    fseek(f, 0, SEEK_SET);
    bn_clip_t *c = NULL;
    if (got == 4 && !memcmp(magic, "RIFF", 4)) c = dec_wav(f, (uint32_t)(sz > 0 ? sz : 0), why, why_n);
    else c = dec_mp3(f, (uint32_t)(sz > 0 ? sz : 0), why, why_n);
    fclose(f);
    if (!c && !why[0]) snprintf(why, why_n, "empty");
    return c;
}
