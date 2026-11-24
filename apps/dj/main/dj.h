/*
 * DJ - two decks and a mixer for P4OS (docs/internal/PEDIDOS-APPS-2026-11.md,
 * G22).
 *
 * The pieces, and who runs them:
 *
 *   dj_mp3.c     minimp3 (CC0, vendored under its own names: the firmware's
 *                copy is the system player's, with a layout of its own).
 *   dj_track.c   a track on a deck: opening MP3, WAV and FLAC, the MP3
 *                frame index, the cache of decoded chunks around the
 *                playhead, and the I/O thread that fills it and writes the
 *                recording to the card.
 *   dj_flac.c    a small FLAC decoder of our own (frames in, PCM out).
 *   dj_ana.c     the analysis thread: the whole track decoded once, for the
 *                waveform (peak and three bands every 256 samples), the
 *                loudness and the tempo with its beat grid.
 *   dj_engine.c  the audio thread: the decks (playhead, motor, scratch,
 *                cue, hot cues, loops, sync), the channel strip (EQ,
 *                filter, effects), the mixer and the speaker.
 *   dj_meta.c    what the app remembers per track (tempo, grid, cue, hot
 *                cues) and its settings.
 *   dj_ui*.c     the interface, in LVGL's thread: the surface with every
 *                control drawn and touched by hand (five fingers), the
 *                waveforms, the library and the settings.
 *   dj_midi.c    a USB MIDI controller, learned control by control.
 *
 * Sharing between the threads: the knobs and faders are floats the UI
 * writes and the audio thread reads once per block (single 32-bit writes:
 * the worst is a value one block late). Events - play, cue, a hot cue, a
 * loop, the platter moving - go through a single-producer queue. What the
 * audio thread tells back (where each deck is, the meters, its timing) is
 * plain volatile fields.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DJ_TAG       "dj"
#define DJ_SR        48000      /* the board's bus rate: no resampler between us and it */
#define DJ_BLK       64         /* samples per render block: 1.33 ms */

#define DJ_CH_SHIFT  15
#define DJ_CH        (1 << DJ_CH_SHIFT)     /* source samples per cache chunk (0.74 s at 44.1 kHz) */
#define DJ_NSLOT     24                     /* chunks kept per deck: 3 MB, ~18 s of a 44.1 kHz track */
#define DJ_WHOP      256                    /* source samples per waveform entry */
#define DJ_HOT       4                      /* hot cues per deck */
#define DJ_MAXLEN    (2u * 3600u * 48000u)  /* two hours at 48 kHz */
#define DJ_NDECK     2

enum { DJ_FMT_NONE, DJ_FMT_MP3, DJ_FMT_WAV, DJ_FMT_FLAC };
enum { DJ_TS_EMPTY, DJ_TS_LOADING, DJ_TS_READY, DJ_TS_ERROR };
enum { DJ_ANA_WAIT, DJ_ANA_RUN, DJ_ANA_DONE, DJ_ANA_FAIL };

/* one waveform entry: the peak and the three bands' levels, 0..255 */
typedef struct { uint8_t peak, lo, mid, hi; } dj_wave_t;

typedef struct dj_flac dj_flac_t;

/* A track on a deck. The I/O thread owns it while it loads (state
 * LOADING); once READY, the fields above "cache" are fixed until the next
 * load, the cache belongs to the I/O thread (the audio thread only reads
 * slots that cmap points at) and the analysis fields fill in from the
 * analysis thread. */
typedef struct dj_track {
    volatile int      state;
    volatile uint32_t gen;          /* +1 every load */
    char     path[256];
    char     title[96];
    char     artist[96];
    char     err[64];
    int      fmt;
    uint32_t sr;
    uint32_t nsamp;                 /* source samples (frames) in the track */
    uint32_t fsize;

    /* MP3: where each frame starts */
    uint32_t *fidx;
    uint32_t  nframes;
    uint16_t  spf;                  /* samples per frame: 1152 or 576 */
    uint8_t   mp3_ch;

    /* WAV: the data chunk */
    uint32_t data_off;
    uint16_t wav_bits, wav_ch, wav_align;
    uint8_t  wav_float;

    /* FLAC: the decoder (the I/O thread's) and its seek points */
    dj_flac_t *flac;

    /* the cache: DJ_NSLOT chunks of DJ_CH stereo samples */
    int16_t  *slots;
    int32_t   slot_chunk[DJ_NSLOT];  /* which chunk each slot holds, -1 none */
    int8_t   *cmap;                  /* per chunk: its slot, or -1 */
    uint32_t  nchunks;
    volatile uint32_t misses;        /* samples the audio thread found missing */

    /* the analysis */
    dj_wave_t        *wave;
    uint32_t          nwave;
    volatile uint32_t wave_n;       /* entries done so far */
    volatile int      ana;
    volatile float    bpm;          /* 0 until known */
    volatile float    beat0;        /* the first beat, in source samples */
    volatile float    gain;         /* to bring the track to the common loudness */
    volatile float    peak;

    /* remembered per track (dj_meta.c), read by the engine on load */
    float    cue;
    float    hot[DJ_HOT];           /* source samples, < 0 = empty */
    bool     meta_bpm;              /* the tempo came from the file of remembered tracks */
} dj_track_t;

/* ---- the engine's commands ---- */

enum {
    DC_PLAY,            /* a = deck: play / pause */
    DC_CUE_DOWN,        /* a = deck */
    DC_CUE_UP,
    DC_HOT,             /* a = deck, b = hot cue: jump, or set where it is empty */
    DC_HOT_CLEAR,
    DC_SEEK,            /* a = deck, f = fraction of the track */
    DC_TOUCH,           /* a = deck, b = 1 down / 0 up */
    DC_SCRATCH,         /* a = deck, f = source samples moved by the hand */
    DC_LOOP,            /* a = deck, f = beats (0 = exit) */
    DC_LOOP_SIZE,       /* a = deck, b = 1 double / 0 halve */
    DC_SYNC,            /* a = deck, b = on */
    DC_BEATJUMP,        /* a = deck, f = beats */
    DC_GRID,            /* a = deck, f = beats to move the grid by (fractions too) */
    DC_BPM_SCALE,       /* a = deck, f = factor (2 or 0.5) */
    DC_REC,             /* b = on */
    DC_UNLOAD,          /* a = deck: the loader takes it */
};

typedef struct {
    uint8_t type, a, b, pad;
    float   f;
} dj_cmd_t;

#define DJ_QN 64        /* a power of two */

typedef struct {
    dj_cmd_t q[DJ_QN];
    volatile uint32_t head, tail;
} dj_cmdq_t;

/* ---- the effects ---- */

enum { FX_ECHO, FX_FLANGER, FX_FILTER, FX_CRUSH, FX_COUNT };

/* what the UI sets: one strip per deck, plus the mixer */
typedef struct {
    float eq[3];        /* low, mid, high: -1 (kill) .. 0 .. +1 (+6 dB) */
    float filt;         /* -1 low-pass .. 0 off .. +1 high-pass */
    float vol;          /* 0..1 */
    float pitch;        /* -1..+1 of the range */
    float bend;         /* the nudge: -1..+1 */
    int   fx_type;
    bool  fx_on;
    float fx_amt;       /* 0..1 */
    int   fx_beats;     /* index into DJ_FX_BEATS */
} dj_strip_t;

typedef struct {
    dj_strip_t s[DJ_NDECK];
    float xfade;        /* -1 A .. +1 B */
    float master;       /* 0..1 */
    float range;        /* the pitch fader's range: 0.08, 0.16, 0.5 */
    int   xcurve;       /* 0 smooth, 1 sharp (scratch) */
    bool  quant;        /* hot cues and loops on the grid */
    bool  vinyl;        /* the platter scratches (else it only nudges) */
} dj_mix_t;

/* what the audio thread tells back, per deck */
typedef struct {
    volatile int32_t  pos;          /* source samples */
    volatile float    vel;          /* playback speed, 1 = normal */
    volatile float    rate;         /* the motor's speed (pitch and sync) */
    volatile bool     playing;
    volatile bool     loop_on;
    volatile float    loop_beats;
    volatile int32_t  loop_a, loop_b;
    volatile float    cue;
    volatile float    hot[DJ_HOT];
    volatile bool     sync;
    volatile float    vu;           /* peak of the last ~50 ms, post fader */
    volatile float    bpm;          /* the track's, after x2 / /2 */
    volatile float    beat0;
    volatile uint32_t gen;          /* the track generation the engine is on */
    volatile bool     cue_held;
} dj_pub_t;

typedef struct {
    float    blk_us_avg, blk_us_max, load_pct;
    uint32_t blocks, underruns, misses;
    int      queued;
} dj_stats_t;

extern const float DJ_FX_BEATS[];
extern const int   DJ_FX_NBEATS;

/* ---- the engine (dj_engine.c) ---- */

typedef struct dj_engine dj_engine_t;

dj_engine_t *dj_engine_new(void);
void         dj_engine_free(dj_engine_t *e);
bool         dj_engine_start(dj_engine_t *e);
void         dj_engine_stop(dj_engine_t *e);
bool         dj_engine_running(const dj_engine_t *e);
bool         dj_cmd(dj_engine_t *e, int type, int a, int b, float f);
dj_mix_t    *dj_mix(dj_engine_t *e);
const dj_pub_t *dj_pub(const dj_engine_t *e, int deck);
dj_track_t  *dj_deck_track(dj_engine_t *e, int deck);
void         dj_stats(dj_engine_t *e, dj_stats_t *out);
float        dj_master_vu(const dj_engine_t *e);
uint32_t     dj_engine_blocks(const dj_engine_t *e);
/* the memory the echo and the recording need, taken when first used (UI thread) */
bool         dj_engine_echo_mem(dj_engine_t *e, int deck);
bool         dj_engine_rec_mem(dj_engine_t *e);
/* the recording */
bool         dj_rec_on(const dj_engine_t *e);
uint32_t     dj_rec_ms(const dj_engine_t *e);
/* the ring the audio thread fills while recording and the I/O thread drains */
int          dj_rec_take(dj_engine_t *e, int16_t *out, int max_frames);

/* ---- tracks and the I/O thread (dj_track.c) ---- */

bool dj_io_start(dj_engine_t *e);
void dj_io_stop(void);
/* ask the I/O thread to load 'path' on 'deck' (copied) */
void dj_load(int deck, const char *path);
/* the recording file: opened by the I/O thread when the engine starts one */
bool dj_io_recording(void);
const char *dj_io_rec_path(void);

/* the cache, for the analysis thread and the I/O thread */
bool dj_track_open(dj_track_t *t, const char *path);
void dj_track_close(dj_track_t *t);
bool dj_track_alloc_cache(dj_track_t *t);
void dj_track_free_cache(dj_track_t *t);
float dj_track_secs(const dj_track_t *t);
void  dj_nfc(char *s);                     /* decomposed accents put back together */

/* ---- analysis (dj_ana.c) ---- */

bool dj_ana_start(dj_engine_t *e);
void dj_ana_stop(void);
void dj_ana_track(dj_track_t *t);
/* the tempo out of an onset envelope sampled at 'fr' Hz, and the beat's
 * phase (with the low band's energy rise, 'kick', to tell on from off) */
float dj_ana_tempo(const float *onset, const float *kick, int n, float fr, float *phase_frames);

/* ---- MP3 (dj_mp3.c) ---- */

typedef struct dj_mp3 dj_mp3_t;
dj_mp3_t *dj_mp3_new(void);
void      dj_mp3_free(dj_mp3_t *m);
void      dj_mp3_reset(dj_mp3_t *m);
/* one frame: returns the samples per channel (0 = none), *used the bytes
 * eaten, *ch and *hz what the frame says */
int       dj_mp3_frame(dj_mp3_t *m, const uint8_t *in, int n, int16_t *pcm, int *used, int *ch, int *hz);
/* the length of the frame at 'p' (a header), 0 if it is not one */
int       dj_mp3_hdr(const uint8_t *p, int *hz, int *spf, int *ch);

/* ---- FLAC (dj_flac.c) ---- */

dj_flac_t *dj_flac_open(const char *path, uint32_t *sr, uint32_t *nsamp, int *ch);
void       dj_flac_close(dj_flac_t *f);
/* decode from sample 'from' on, into 'out' (stereo int16), up to 'n'
 * samples; returns how many it wrote */
int        dj_flac_read(dj_flac_t *f, uint32_t from, int16_t *out, int n);

/* ---- memory and small helpers ---- */

void    *dj_alloc(size_t bytes);           /* PSRAM, zeroed */
void     dj_free(void *p);
float    dj_clampf(float v, float lo, float hi);
static inline float dj_maxf(float a, float b) { return a > b ? a : b; }
/* a string into a smaller buffer, cut (the board's GCC will not take a
 * snprintf "%s" that may truncate) */
static inline void dj_copy(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i + 1 < n; i++) dst[i] = src[i];
    if (n) dst[i] = 0;
}
void     dj_mkdirs(void);
const char *dj_data_dir(void);                  /* the app's data folder */
