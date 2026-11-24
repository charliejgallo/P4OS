/*
 * DJ - a track on a deck, and the I/O thread.
 *
 * A five-minute track decoded whole is 50 MB of stereo samples; the board
 * has 32 MB for everything. So a deck keeps only chunks of it decoded, in
 * a cache of DJ_NSLOT slots of DJ_CH samples (4 MB per deck): the ones
 * around the playhead, more ahead than behind, and the ones under the cue,
 * the hot cues and the loop, so that a jump there plays at once. The audio
 * thread reads a sample through cmap (chunk -> slot); a chunk that is not
 * there yet plays as silence and counts as a miss.
 *
 * The I/O thread does everything with the card: it loads a track (the MP3
 * frame index: where every frame starts, read once, so any chunk can be
 * decoded on its own), keeps both caches filled, the most urgent chunk
 * first, and writes the recording. It runs under LVGL's priority's
 * neighbour and sleeps when there is nothing to do.
 *
 * Decoding chunk k of an MP3: the frames that cover its samples, plus three
 * before it to warm the decoder up (the bit reservoir and the overlap of
 * the previous granule), unless the decoder is already there because the
 * last chunk it did was k - 1: then it goes on, and the frame that
 * straddles the two chunks is taken from the carry.
 */
#include "dj.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if !defined(AOS_SIM)
#include "esp_heap_caps.h"
#endif

/* ---- memory and small helpers ---- */

void *dj_alloc(size_t bytes)
{
#if defined(AOS_SIM)
    void *p = malloc(bytes);
#else
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (p) memset(p, 0, bytes);
    return p;
}

void dj_free(void *p)
{
    if (!p) return;
#if defined(AOS_SIM)
    free(p);
#else
    heap_caps_free(p);
#endif
}

float dj_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static char s_dir[160];

const char *dj_data_dir(void)
{
    if (!s_dir[0]) snprintf(s_dir, sizeof s_dir, "%s/dj", aos_hal_path_data());
    return s_dir;
}

void dj_mkdirs(void)
{
    mkdir(aos_hal_path_data(), 0775);
    mkdir(dj_data_dir(), 0775);
    mkdir(aos_hal_path_recordings(), 0775);
}

/* a full fence between the slot and the map; spelled out on the board,
 * where the loader's table has no __sync_synchronize to resolve */
#if defined(__riscv)
#define DJ_FENCE() __asm__ volatile("fence rw,rw" ::: "memory")
#else
#define DJ_FENCE() __sync_synchronize()
#endif

static uint32_t rd32le(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* ---- names: the ID3 tag, or "Artist - Title" in the file name ---- */

static void utf8_put(char **o, char *end, uint32_t c)
{
    char *p = *o;
    if (c < 0x80) { if (p + 1 < end) *p++ = (char)c; }
    else if (c < 0x800) { if (p + 2 < end) { *p++ = (char)(0xC0 | (c >> 6)); *p++ = (char)(0x80 | (c & 63)); } }
    else if (p + 3 < end) { *p++ = (char)(0xE0 | (c >> 12)); *p++ = (char)(0x80 | ((c >> 6) & 63)); *p++ = (char)(0x80 | (c & 63)); }
    *o = p;
}

/* an ID3v2 text frame's body (encoding byte first) to UTF-8 */
static void id3_text(const uint8_t *b, int n, char *out, int max)
{
    char *o = out, *end = out + max;
    if (n < 1) { out[0] = 0; return; }
    int enc = b[0];
    b++; n--;
    if (enc == 0) {
        for (int i = 0; i < n && b[i]; i++) utf8_put(&o, end, b[i]);
    } else if (enc == 3) {
        for (int i = 0; i < n && b[i] && o + 1 < end; i++) *o++ = (char)b[i];
    } else {
        bool be = enc == 2;
        int i = 0;
        if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) { be = false; i = 2; }
        else if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) { be = true; i = 2; }
        for (; i + 1 < n; i += 2) {
            uint32_t c = be ? (uint32_t)(b[i] << 8 | b[i + 1]) : (uint32_t)(b[i] | b[i + 1] << 8);
            if (!c) break;
            if (c >= 0xD800 && c < 0xE000) c = '?';
            utf8_put(&o, end, c);
        }
    }
    *o = 0;
    /* trim */
    while (o > out && (o[-1] == ' ')) *--o = 0;
}

/* Reads the ID3v2 tag at the start of 'f' (title and artist), returns the
 * bytes it takes, 0 when there is none. */
static uint32_t id3v2(FILE *f, dj_track_t *t)
{
    uint8_t h[10];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, 10, f) != 10) return 0;
    if (memcmp(h, "ID3", 3) != 0) return 0;
    uint32_t size = ((uint32_t)(h[6] & 127) << 21) | ((h[7] & 127) << 14) | ((h[8] & 127) << 7) | (h[9] & 127);
    uint32_t total = size + 10 + ((h[5] & 0x10) ? 10 : 0);
    int ver = h[3];
    uint32_t want = size < 65536 ? size : 65536;
    uint8_t *b = dj_alloc(want + 1);
    if (!b) return total;
    uint32_t got = (uint32_t)fread(b, 1, want, f);
    uint32_t i = 0;
    if (ver >= 3 && (h[5] & 0x40) && got >= 4) {       /* the extended header */
        uint32_t x = ver == 4 ? (((uint32_t)(b[0] & 127) << 21) | ((b[1] & 127) << 14) | ((b[2] & 127) << 7) | (b[3] & 127))
                              : (((uint32_t)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]) + 4;
        i = x;
    }
    while (ver >= 3 && i + 10 <= got) {
        const uint8_t *fr = b + i;
        if (!fr[0]) break;
        uint32_t fs = ver == 4 ? (((uint32_t)(fr[4] & 127) << 21) | ((fr[5] & 127) << 14) | ((fr[6] & 127) << 7) | (fr[7] & 127))
                               : (((uint32_t)fr[4] << 24) | (fr[5] << 16) | (fr[6] << 8) | fr[7]);
        if (fs == 0 || i + 10 + fs > got) break;
        if (!memcmp(fr, "TIT2", 4)) id3_text(fr + 10, (int)fs, t->title, sizeof t->title);
        else if (!memcmp(fr, "TPE1", 4)) id3_text(fr + 10, (int)fs, t->artist, sizeof t->artist);
        i += 10 + fs;
    }
    dj_free(b);
    return total;
}

/* A name copied from a Mac comes decomposed: "o" and then the accent as a
 * combining mark (U+0301), which the fonts do not have (a box shows). The
 * Latin letters with an accent, tilde, diaeresis or cedilla are put back
 * together, in place (three bytes become two). */
void dj_nfc(char *s)
{
    /* base letters for U+00C0..U+00FF, and the mark each one takes */
    static const char BASE[] = "AAAAAA_CEEEEIIII_NOOOOO_OUUUUY__aaaaaa_ceeeeiiii_nooooo_ouuuuy_y";
    static const uint8_t MARK[] = {
        0x80, 0x81, 0x82, 0x83, 0x88, 0, 0, 0xA7, 0x80, 0x81, 0x82, 0x88, 0x80, 0x81, 0x82, 0x88,
        0, 0x83, 0x80, 0x81, 0x82, 0x83, 0x88, 0, 0, 0x80, 0x81, 0x82, 0x88, 0x81, 0, 0,
        0x80, 0x81, 0x82, 0x83, 0x88, 0, 0, 0xA7, 0x80, 0x81, 0x82, 0x88, 0x80, 0x81, 0x82, 0x88,
        0, 0x83, 0x80, 0x81, 0x82, 0x83, 0x88, 0, 0, 0x80, 0x81, 0x82, 0x88, 0x81, 0, 0x88,
    };
    char *o = s;
    for (const char *p = s; *p;) {
        if ((unsigned char)p[1] == 0xCC && p[2]) {
            int found = -1;
            for (int i = 0; i < 64; i++)
                if (BASE[i] == p[0] && MARK[i] == (uint8_t)p[2]) { found = i; break; }
            if (found >= 0) {
                unsigned cp = 0xC0u + (unsigned)found;
                *o++ = (char)(0xC0 | (cp >> 6));
                *o++ = (char)(0x80 | (cp & 63));
                p += 3;
                continue;
            }
        }
        *o++ = *p++;
    }
    *o = 0;
}

static void names_from_file(dj_track_t *t)
{
    const char *base = strrchr(t->path, '/');
    base = base ? base + 1 : t->path;
    char name[160];
    dj_copy(name, sizeof name, base);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    char *sep = strstr(name, " - ");
    if (!t->title[0]) dj_copy(t->title, sizeof t->title, sep ? sep + 3 : name);
    if (!t->artist[0] && sep) {
        *sep = 0;
        dj_copy(t->artist, sizeof t->artist, name);
    }
    dj_nfc(t->title);
    dj_nfc(t->artist);
}

/* ---- opening ---- */

#define SCAN_BUF (64 * 1024)

/* Every frame's offset, in one pass over the file. A header counts when the
 * next frame's header is where it says (or the file ends there). */
static bool mp3_index(dj_track_t *t, FILE *f, uint32_t start, uint32_t end)
{
    uint8_t *buf = dj_alloc(SCAN_BUF);
    uint32_t cap = 1 + (end - start) / 96;         /* the shortest frame there is: 8 kbps */
    if (cap > 600000) cap = 600000;
    uint32_t *idx = dj_alloc((size_t)cap * 4 + 4);
    if (!buf || !idx) { dj_free(buf); dj_free(idx); return false; }
    uint32_t base = start, len = 0, p = 0, n = 0;
    int hz0 = 0, spf0 = 0, ch0 = 0;
    fseek(f, (long)start, SEEK_SET);
    for (;;) {
        if (p + 2048 > len && base + len < end) {
            /* slide: keep from p on, fill the rest */
            memmove(buf, buf + p, len - p);
            base += p;
            len -= p;
            p = 0;
            uint32_t want = SCAN_BUF - len;
            if (want > end - (base + len)) want = end - (base + len);
            len += (uint32_t)fread(buf + len, 1, want, f);
        }
        if (p + 4 > len) break;
        int hz, spf, ch;
        int fl = dj_mp3_hdr(buf + p, &hz, &spf, &ch);
        if (fl <= 0 || (hz0 && (hz != hz0 || spf != spf0))) { p++; continue; }
        bool ok;
        if (p + (uint32_t)fl + 4 <= len) ok = dj_mp3_hdr(buf + p + fl, NULL, NULL, NULL) > 0;
        else ok = base + p + (uint32_t)fl >= end - 4 || p + (uint32_t)fl + 4 > len;
        if (!ok) { p++; continue; }
        if (!hz0) { hz0 = hz; spf0 = spf; ch0 = ch; }
        if (n < cap) idx[n++] = base + p;
        p += (uint32_t)fl;
    }
    dj_free(buf);
    if (n < 2 || !hz0) { dj_free(idx); return false; }
    idx[n] = end;           /* one past the last frame: every frame's length is idx[f+1] - idx[f] */
    t->fidx = idx;
    t->nframes = n;
    t->spf = (uint16_t)spf0;
    t->sr = (uint32_t)hz0;
    t->mp3_ch = (uint8_t)ch0;
    t->nsamp = n * (uint32_t)spf0;
    return true;
}

static bool wav_parse(dj_track_t *t, FILE *f)
{
    uint8_t h[12];
    if (fseek(f, 0, SEEK_SET) || fread(h, 1, 12, f) != 12) return false;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
    bool fmt_ok = false;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) return false;
        uint32_t sz = rd32le(c + 4);
        long here = ftell(f);
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t b[40] = { 0 };
            if (fread(b, 1, sz < 40 ? sz : 40, f) < 16) return false;
            int fmt = rd16le(b);
            if (fmt == 0xFFFE && sz >= 26) fmt = rd16le(b + 24);
            t->wav_ch = rd16le(b + 2);
            t->sr = rd32le(b + 4);
            t->wav_align = rd16le(b + 12);
            t->wav_bits = rd16le(b + 14);
            t->wav_float = fmt == 3;
            if (fmt != 1 && fmt != 3) return false;
            if (t->wav_float && t->wav_bits != 32) return false;
            if (!t->wav_float && t->wav_bits != 16 && t->wav_bits != 24 && t->wav_bits != 8 && t->wav_bits != 32) return false;
            if (!t->wav_ch || !t->sr || t->wav_align < t->wav_ch * (t->wav_bits / 8)) return false;
            fmt_ok = true;
        } else if (!memcmp(c, "data", 4)) {
            if (!fmt_ok) return false;
            t->data_off = (uint32_t)here;
            uint32_t avail = t->fsize > (uint32_t)here ? t->fsize - (uint32_t)here : 0;
            if (sz == 0 || sz > avail) sz = avail;           /* a recording that was never closed */
            t->nsamp = sz / t->wav_align;
            return t->nsamp > 0;
        }
        if (fseek(f, here + (long)sz + (sz & 1), SEEK_SET)) return false;
    }
}

static bool ext_is(const char *path, const char *ext)
{
    const char *d = strrchr(path, '.');
    return d && !strcasecmp(d + 1, ext);
}

bool dj_track_open(dj_track_t *t, const char *path)
{
    snprintf(t->path, sizeof t->path, "%s", path);
    t->title[0] = t->artist[0] = t->err[0] = 0;
    t->fmt = DJ_FMT_NONE;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(t->err, sizeof t->err, "%s", _("No se pudo abrir")); return false; }
    fseek(f, 0, SEEK_END);
    t->fsize = (uint32_t)ftell(f);
    bool ok = false;
    if (ext_is(path, "wav")) {
        ok = wav_parse(t, f);
        if (ok) t->fmt = DJ_FMT_WAV;
    } else if (ext_is(path, "flac")) {
        int ch = 0;
        fclose(f);
        f = NULL;
        t->flac = dj_flac_open(path, &t->sr, &t->nsamp, &ch);
        ok = t->flac != NULL;
        if (ok) t->fmt = DJ_FMT_FLAC;
    } else {
        uint32_t start = id3v2(f, t);
        uint32_t end = t->fsize;
        if (end >= 128) {
            uint8_t tag[3];
            fseek(f, (long)end - 128, SEEK_SET);
            if (fread(tag, 1, 3, f) == 3 && !memcmp(tag, "TAG", 3)) end -= 128;
        }
        ok = start < end && mp3_index(t, f, start, end);
        if (ok) t->fmt = DJ_FMT_MP3;
    }
    if (f) fclose(f);
    if (ok && (t->sr < 8000 || t->sr > 96000)) ok = false;
    if (ok && (float)t->nsamp / (float)t->sr > (float)DJ_MAXLEN / (float)DJ_SR) {
        snprintf(t->err, sizeof t->err, "%s", _("Demasiado largo"));
        dj_track_close(t);
        return false;
    }
    if (!ok) {
        if (!t->err[0]) snprintf(t->err, sizeof t->err, "%s", _("Formato no soportado"));
        dj_track_close(t);
        return false;
    }
    names_from_file(t);
    t->nchunks = (t->nsamp + DJ_CH - 1) >> DJ_CH_SHIFT;
    t->cmap = dj_alloc(t->nchunks);
    t->nwave = (t->nsamp + DJ_WHOP - 1) / DJ_WHOP;
    t->wave = dj_alloc((size_t)t->nwave * sizeof(dj_wave_t));
    if (!t->cmap || !t->wave) {
        snprintf(t->err, sizeof t->err, "%s", _("Sin memoria"));
        dj_track_close(t);
        return false;
    }
    memset(t->cmap, 0xFF, t->nchunks);
    for (int i = 0; i < DJ_NSLOT; i++) t->slot_chunk[i] = -1;
    t->wave_n = 0;
    t->ana = DJ_ANA_WAIT;
    t->bpm = 0.0f;
    t->beat0 = 0.0f;
    t->gain = 1.0f;
    t->peak = 0.0f;
    t->misses = 0;
    return true;
}

void dj_track_close(dj_track_t *t)
{
    dj_free(t->fidx);
    t->fidx = NULL;
    dj_free(t->cmap);
    t->cmap = NULL;
    dj_free(t->wave);
    t->wave = NULL;
    if (t->flac) dj_flac_close(t->flac);
    t->flac = NULL;
    t->nframes = t->nsamp = t->nchunks = t->nwave = 0;
    t->wave_n = 0;
    t->fmt = DJ_FMT_NONE;
}

bool dj_track_alloc_cache(dj_track_t *t)
{
    if (t->slots) return true;
    t->slots = dj_alloc((size_t)DJ_NSLOT * DJ_CH * 2 * sizeof(int16_t));
    for (int i = 0; i < DJ_NSLOT; i++) t->slot_chunk[i] = -1;
    return t->slots != NULL;
}

void dj_track_free_cache(dj_track_t *t)
{
    dj_free(t->slots);
    t->slots = NULL;
}

float dj_track_secs(const dj_track_t *t)
{
    return t && t->sr ? (float)t->nsamp / (float)t->sr : 0.0f;
}

/* ---- the I/O thread ---- */

#define WARM      3         /* MP3 frames decoded before a chunk that does not follow the last */
#define AHEAD     10        /* chunks ahead of the playhead */
#define BEHIND    4
#define IN_BUF    (80 * 1024)
#define REC_FR    4096      /* frames written at a time */

typedef struct {
    FILE     *f;            /* the track's file (MP3, WAV), kept open */
    dj_mp3_t *mp3;
    uint32_t  next_frame;   /* the frame the decoder would do next; UINT32_MAX = cold */
    int32_t   carry_frame;  /* the frame in carry[], -1 none */
    int16_t   carry[1152 * 2];
    uint32_t  gen;          /* of the track the state above belongs to */
} io_deck_t;

static struct {
    dj_engine_t *e;
    volatile bool quit, done, running;
    void    *mtx;
    char     req[DJ_NDECK][256];
    volatile uint32_t req_n[DJ_NDECK];
    uint32_t  req_seen[DJ_NDECK];
    io_deck_t d[DJ_NDECK];
    uint8_t  *in;
    int16_t  *tmp;          /* a frame's or a read's samples */
    /* the recording */
    FILE     *rec;
    char      rec_path[200];
    uint32_t  rec_frames;
    uint32_t  rec_hdr_ms;
    int16_t  *rec_buf;
} IO;

bool dj_meta_load(dj_track_t *t);           /* dj_meta.c */

void dj_load(int deck, const char *path)
{
    if (deck < 0 || deck >= DJ_NDECK || !IO.mtx) return;
    aos_hal_mutex_lock(IO.mtx);
    snprintf(IO.req[deck], sizeof IO.req[deck], "%s", path);
    IO.req_n[deck]++;
    aos_hal_mutex_unlock(IO.mtx);
}

/* until the audio thread has finished two more blocks: whatever it was
 * reading when we changed the map, it is done with */
static void wait_engine(void)
{
    if (!dj_engine_running(IO.e)) return;
    uint32_t b0 = dj_engine_blocks(IO.e);
    for (int i = 0; i < 40 && dj_engine_blocks(IO.e) - b0 < 2; i++) aos_hal_sleep_ms(1);
}

static void deck_load(int d)
{
    char path[256];
    aos_hal_mutex_lock(IO.mtx);
    snprintf(path, sizeof path, "%s", IO.req[d]);
    IO.req_seen[d] = IO.req_n[d];
    aos_hal_mutex_unlock(IO.mtx);

    dj_track_t *t = dj_deck_track(IO.e, d);
    t->state = DJ_TS_LOADING;
    DJ_FENCE();
    dj_cmd(IO.e, DC_UNLOAD, d, 0, 0.0f);
    wait_engine();
    io_deck_t *s = &IO.d[d];
    if (s->f) fclose(s->f);
    s->f = NULL;
    s->next_frame = UINT32_MAX;
    s->carry_frame = -1;
    dj_track_close(t);
    uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
    bool ok = dj_track_open(t, path);
    if (ok && t->fmt != DJ_FMT_FLAC) {
        s->f = fopen(path, "rb");
        ok = s->f != NULL;
    }
    if (!ok) {
        t->state = DJ_TS_ERROR;
        aos_hal_log(DJ_TAG, "load %s: %s", path, t->err);
        return;
    }
    t->cue = 0.0f;
    for (int i = 0; i < DJ_HOT; i++) t->hot[i] = -1.0f;
    t->meta_bpm = dj_meta_load(t);
    t->gen++;
    s->gen = t->gen;
    DJ_FENCE();
    t->state = DJ_TS_READY;
    aos_hal_log(DJ_TAG, "deck %c: %s (%s, %u Hz, %.1f s, %u frames) in %u ms%s",
                'A' + d, path, t->fmt == DJ_FMT_MP3 ? "mp3" : t->fmt == DJ_FMT_WAV ? "wav" : "flac",
                (unsigned)t->sr, (double)dj_track_secs(t), (unsigned)t->nframes,
                (unsigned)((uint32_t)aos_hal_uptime_ms() - t0), t->meta_bpm ? ", tempo remembered" : "");
}

/* copy what frame 'f' (pcm, 'n' samples, interleaved by 'ch') has of chunk
 * k into 'dst' */
static void frame_into(const int16_t *pcm, int n, int ch, uint32_t fsamp0, uint32_t s0, uint32_t s1, int16_t *dst)
{
    uint32_t a = fsamp0 > s0 ? fsamp0 : s0;
    uint32_t b = fsamp0 + (uint32_t)n < s1 ? fsamp0 + (uint32_t)n : s1;
    for (uint32_t s = a; s < b; s++) {
        const int16_t *p = pcm + (s - fsamp0) * (uint32_t)ch;
        int16_t *q = dst + (s - s0) * 2;
        q[0] = p[0];
        q[1] = ch > 1 ? p[1] : p[0];
    }
}

static void decode_mp3(dj_track_t *t, io_deck_t *s, uint32_t k, int16_t *dst)
{
    uint32_t s0 = k << DJ_CH_SHIFT, s1 = s0 + DJ_CH;
    if (s1 > t->nsamp) s1 = t->nsamp;
    memset(dst, 0, (size_t)DJ_CH * 2 * sizeof(int16_t));
    uint32_t spf = t->spf;
    uint32_t f0 = s0 / spf, f1 = (s1 - 1) / spf;
    uint32_t start;
    if (s->carry_frame >= 0 && (uint32_t)s->carry_frame == f0 && s->next_frame == f0 + 1) {
        frame_into(s->carry, (int)spf, 2, f0 * spf, s0, s1, dst);
        start = f0 + 1;
    } else if (s->next_frame == f0) {
        start = f0;
    } else {
        dj_mp3_reset(s->mp3);
        start = f0 > WARM ? f0 - WARM : 0;
    }
    uint32_t f = start;
    while (f <= f1) {
        /* as many whole frames as fit in the buffer */
        uint32_t last = f;
        while (last < f1 && t->fidx[last + 2] - t->fidx[f] <= IN_BUF) last++;
        uint32_t off = t->fidx[f], len = t->fidx[last + 1] - off;
        if (len > IN_BUF) len = IN_BUF;
        if (fseek(s->f, (long)off, SEEK_SET) || fread(IO.in, 1, len, s->f) != len) {
            s->next_frame = UINT32_MAX;
            return;
        }
        for (uint32_t g = f; g <= last; g++) {
            uint32_t o = t->fidx[g] - off;
            int used, ch, hz;
            int n = dj_mp3_frame(s->mp3, IO.in + o, (int)(len - o), IO.tmp, &used, &ch, &hz);
            if (n <= 0) continue;               /* a frame without its reservoir: silence */
            if (n > (int)spf) n = (int)spf;
            if (g >= f0) frame_into(IO.tmp, n, ch, g * spf, s0, s1, dst);
            if (g == f1) {
                for (int i = 0; i < n; i++) {
                    s->carry[i * 2] = IO.tmp[i * ch];
                    s->carry[i * 2 + 1] = IO.tmp[i * ch + (ch > 1)];
                }
                for (int i = n; i < (int)spf; i++) s->carry[i * 2] = s->carry[i * 2 + 1] = 0;
                s->carry_frame = (int32_t)g;
            }
        }
        f = last + 1;
    }
    s->next_frame = f1 + 1;
}

static int16_t wav_sample(const uint8_t *p, int bits, bool fl)
{
    if (fl) {
        union { uint32_t u; float f; } v = { .u = rd32le(p) };
        float x = v.f * 32767.0f;
        return (int16_t)(x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : (int)x);
    }
    switch (bits) {
    case 8:  return (int16_t)((p[0] - 128) << 8);
    case 24: return (int16_t)(p[1] | (p[2] << 8));
    case 32: return (int16_t)(p[2] | (p[3] << 8));
    default: return (int16_t)rd16le(p);
    }
}

static void decode_wav(dj_track_t *t, io_deck_t *s, uint32_t k, int16_t *dst)
{
    uint32_t s0 = k << DJ_CH_SHIFT, n = DJ_CH;
    if (s0 + n > t->nsamp) n = t->nsamp - s0;
    memset(dst, 0, (size_t)DJ_CH * 2 * sizeof(int16_t));
    uint32_t done = 0, per = IN_BUF / t->wav_align;
    fseek(s->f, (long)(t->data_off + s0 * t->wav_align), SEEK_SET);
    int bps = t->wav_bits / 8;
    while (done < n) {
        uint32_t m = n - done < per ? n - done : per;
        uint32_t got = (uint32_t)fread(IO.in, t->wav_align, m, s->f);
        for (uint32_t i = 0; i < got; i++) {
            const uint8_t *p = IO.in + i * t->wav_align;
            int16_t l = wav_sample(p, t->wav_bits, t->wav_float);
            int16_t r = t->wav_ch > 1 ? wav_sample(p + bps, t->wav_bits, t->wav_float) : l;
            dst[(done + i) * 2] = l;
            dst[(done + i) * 2 + 1] = r;
        }
        if (got < m) break;
        done += got;
    }
}

static void decode_chunk(dj_track_t *t, io_deck_t *s, uint32_t k, int16_t *dst)
{
    if (t->fmt == DJ_FMT_MP3) decode_mp3(t, s, k, dst);
    else if (t->fmt == DJ_FMT_WAV) decode_wav(t, s, k, dst);
    else if (t->fmt == DJ_FMT_FLAC) {
        uint32_t s0 = k << DJ_CH_SHIFT, n = DJ_CH;
        if (s0 + n > t->nsamp) n = t->nsamp - s0;
        memset(dst, 0, (size_t)DJ_CH * 2 * sizeof(int16_t));
        dj_flac_read(t->flac, s0, dst, (int)n);
    }
}

/* the chunks a deck wants, most urgent first */
static int wanted(dj_track_t *t, const dj_pub_t *p, int32_t *w, int max)
{
    int n = 0;
    int32_t nc = (int32_t)t->nchunks;
#define WANT(c) do { int32_t _c = (c); if (_c >= 0 && _c < nc && n < max) { bool _dup = false; \
        for (int _i = 0; _i < n; _i++) { if (w[_i] == _c) { _dup = true; break; } } \
        if (!_dup) w[n++] = _c; } } while (0)
    int32_t pos = p->pos < 0 ? 0 : p->pos;
    int32_t c = pos >> DJ_CH_SHIFT;
    int dir = p->vel < -0.05f ? -1 : 1;
    WANT(c);
    WANT(c + dir);
    WANT(c - dir);
    if (p->loop_on) {
        int32_t a = p->loop_a >> DJ_CH_SHIFT, b = p->loop_b >> DJ_CH_SHIFT;
        for (int32_t i = a; i <= b && i - a < 8; i++) WANT(i);
    }
    for (int i = 2; i <= 3; i++) WANT(c + i * dir);
    WANT((int32_t)p->cue >> DJ_CH_SHIFT);
    for (int h = 0; h < DJ_HOT; h++)
        if (p->hot[h] >= 0.0f) WANT((int32_t)p->hot[h] >> DJ_CH_SHIFT);
    for (int i = 4; i <= AHEAD; i++) WANT(c + i * dir);
    WANT(((int32_t)p->cue >> DJ_CH_SHIFT) + 1);
    for (int h = 0; h < DJ_HOT; h++)
        if (p->hot[h] >= 0.0f) WANT(((int32_t)p->hot[h] >> DJ_CH_SHIFT) + 1);
    for (int i = 2; i <= BEHIND; i++) WANT(c - i * dir);
#undef WANT
    return n;
}

/* One chunk for the deck that needs one most. Returns true if it did one. */
static bool fill_one(void)
{
    int best_d = -1, best_rank = 1 << 30;
    int32_t best_c = -1;
    int32_t w[DJ_NDECK][DJ_NSLOT];
    int wn[DJ_NDECK] = { 0 };
    for (int d = 0; d < DJ_NDECK; d++) {
        dj_track_t *t = dj_deck_track(IO.e, d);
        if (t->state != DJ_TS_READY || !t->slots || IO.d[d].gen != t->gen) continue;
        const dj_pub_t *p = dj_pub(IO.e, d);
        if (p->gen != t->gen) continue;             /* the engine has not taken the track yet */
        wn[d] = wanted(t, p, w[d], DJ_NSLOT - 2);
        for (int i = 0; i < wn[d]; i++) {
            if (t->cmap[w[d][i]] >= 0) continue;
            if (i < best_rank) { best_rank = i; best_d = d; best_c = w[d][i]; }
            break;
        }
    }
    if (best_d < 0) return false;
    dj_track_t *t = dj_deck_track(IO.e, best_d);
    /* a slot: a free one, or the one farthest from the playhead that is not wanted */
    int slot = -1;
    int32_t far = -1;
    int32_t c = dj_pub(IO.e, best_d)->pos >> DJ_CH_SHIFT;
    for (int i = 0; i < DJ_NSLOT; i++) {
        int32_t k = t->slot_chunk[i];
        if (k < 0) { slot = i; break; }
        bool want = false;
        for (int j = 0; j < wn[best_d]; j++) if (w[best_d][j] == k) { want = true; break; }
        if (want) continue;
        int32_t dist = k > c ? k - c : c - k;
        if (dist > far) { far = dist; slot = i; }
    }
    if (slot < 0) return false;
    int32_t old = t->slot_chunk[slot];
    if (old >= 0) {
        t->cmap[old] = -1;
        t->slot_chunk[slot] = -1;
        DJ_FENCE();
        wait_engine();
    }
    int16_t *dst = t->slots + (size_t)slot * DJ_CH * 2;
    decode_chunk(t, &IO.d[best_d], (uint32_t)best_c, dst);
    t->slot_chunk[slot] = best_c;
    DJ_FENCE();
    t->cmap[best_c] = (int8_t)slot;
    return true;
}

/* ---- the recording ---- */

static void wav_header(uint8_t *h, uint32_t frames)
{
    uint32_t data = frames * 4;
    memcpy(h, "RIFF", 4);
    uint32_t v = data + 36;
    for (int i = 0; i < 4; i++) h[4 + i] = (uint8_t)(v >> (8 * i));
    memcpy(h + 8, "WAVEfmt ", 8);
    static const uint8_t F[16] = { 16, 0, 0, 0, 1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0, 0xEE, 2, 0 };   /* PCM, 2 ch, 48 kHz, 192000 B/s */
    memcpy(h + 16, F, 16);
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    for (int i = 0; i < 4; i++) h[40 + i] = (uint8_t)(data >> (8 * i));
}

static void rec_open(void)
{
    char name[64];
    time_t now = time(NULL);
    struct tm tm;
    if (aos_hal_time_is_valid() && localtime_r(&now, &tm))
        snprintf(name, sizeof name, "mezcla-%04d%02d%02d-%02d%02d%02d.wav", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    else {
        int n = 1;
        for (; n < 1000; n++) {
            snprintf(name, sizeof name, "mezcla-%03d.wav", n);
            char p[200];
            snprintf(p, sizeof p, "%s/%s", aos_hal_path_recordings(), name);
            struct stat st;
            if (stat(p, &st) != 0) break;
        }
    }
    snprintf(IO.rec_path, sizeof IO.rec_path, "%s/%s", aos_hal_path_recordings(), name);
    IO.rec = fopen(IO.rec_path, "wb");
    IO.rec_frames = 0;
    IO.rec_hdr_ms = (uint32_t)aos_hal_uptime_ms();
    if (!IO.rec) { aos_hal_log(DJ_TAG, "rec: cannot open %s", IO.rec_path); return; }
    uint8_t h[44];
    wav_header(h, 0);
    fwrite(h, 1, 44, IO.rec);
    aos_hal_log(DJ_TAG, "rec: %s", IO.rec_path);
}

static void rec_fix_header(void)
{
    uint8_t h[44];
    wav_header(h, IO.rec_frames);
    long here = ftell(IO.rec);
    fseek(IO.rec, 0, SEEK_SET);
    fwrite(h, 1, 44, IO.rec);
    fseek(IO.rec, here, SEEK_SET);
    fflush(IO.rec);
}

/* drain what the engine recorded; true if it wrote anything */
static bool rec_service(void)
{
    bool on = dj_rec_on(IO.e);
    if (on && !IO.rec && !IO.rec_path[0]) rec_open();
    bool did = false;
    for (;;) {
        int n = dj_rec_take(IO.e, IO.rec_buf, REC_FR);
        if (n <= 0) break;
        if (IO.rec) fwrite(IO.rec_buf, 4, (size_t)n, IO.rec);
        IO.rec_frames += (uint32_t)n;
        did = true;
        if (n < REC_FR) break;
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (IO.rec && now - IO.rec_hdr_ms > 10000) {
        rec_fix_header();
        IO.rec_hdr_ms = now;
    }
    if (!on && IO.rec_path[0]) {
        if (IO.rec) {
            rec_fix_header();
            fclose(IO.rec);
            aos_hal_log(DJ_TAG, "rec: %s closed, %u s", IO.rec_path, (unsigned)(IO.rec_frames / DJ_SR));
        }
        IO.rec = NULL;
        IO.rec_path[0] = 0;
    }
    return did;
}

bool dj_io_recording(void) { return IO.rec != NULL; }
const char *dj_io_rec_path(void) { return IO.rec_path; }

static void io_main(void *arg)
{
    (void)arg;
    while (!IO.quit) {
        bool busy = false;
        for (int d = 0; d < DJ_NDECK; d++)
            if (IO.req_n[d] != IO.req_seen[d]) { deck_load(d); busy = true; }
        if (rec_service()) busy = true;
        if (fill_one()) busy = true;
        if (!busy) aos_hal_sleep_ms(4);
    }
    /* a recording in progress is closed whole */
    if (IO.rec) {
        rec_service();
        rec_fix_header();
        fclose(IO.rec);
        IO.rec = NULL;
    }
    IO.rec_path[0] = 0;
    IO.done = true;
}

bool dj_io_start(dj_engine_t *e)
{
    if (IO.running) return true;
    IO.e = e;
    if (!IO.mtx) IO.mtx = aos_hal_mutex_create();
    IO.in = dj_alloc(IN_BUF);
    IO.tmp = dj_alloc(1152 * 2 * sizeof(int16_t));
    IO.rec_buf = dj_alloc(REC_FR * 4);
    bool ok = IO.mtx && IO.in && IO.tmp && IO.rec_buf;
    for (int d = 0; d < DJ_NDECK && ok; d++) {
        IO.d[d].mp3 = dj_mp3_new();
        IO.d[d].next_frame = UINT32_MAX;
        IO.d[d].carry_frame = -1;
        ok = IO.d[d].mp3 != NULL;
    }
    IO.quit = false;
    IO.done = false;
    if (ok) ok = aos_hal_thread_start("dj_io", io_main, NULL, 8192, 5);
    if (!ok) {
        aos_hal_log(DJ_TAG, "no I/O thread");
        IO.done = true;
        dj_io_stop();
        return false;
    }
    IO.running = true;
    return true;
}

void dj_io_stop(void)
{
    if (IO.running) {
        IO.quit = true;
        for (int i = 0; i < 600 && !IO.done; i++) aos_hal_sleep_ms(5);
        if (!IO.done) aos_hal_log(DJ_TAG, "the I/O thread did not stop");
    }
    IO.running = false;
    for (int d = 0; d < DJ_NDECK; d++) {
        if (IO.d[d].f) fclose(IO.d[d].f);
        dj_mp3_free(IO.d[d].mp3);
        memset(&IO.d[d], 0, sizeof IO.d[d]);
    }
    dj_free(IO.in);
    dj_free(IO.tmp);
    dj_free(IO.rec_buf);
    IO.in = NULL;
    IO.tmp = NULL;
    IO.rec_buf = NULL;
    for (int d = 0; d < DJ_NDECK; d++) IO.req_seen[d] = IO.req_n[d];
}
