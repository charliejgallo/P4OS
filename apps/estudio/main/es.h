/*
 * Estudio - a synthesizer, a drum machine and a sequencer for P4OS.
 *
 * The pieces, and who runs them:
 *
 *   es_dsp.c     the sound: oscillators, filters, envelopes, the synth
 *                voices, the synthesized drums, the user's samples, the
 *                effects and the master. Called from the audio thread only.
 *   es_engine.c  the audio thread: the command queues, the transport and the
 *                sequencer clock, the arpeggiator, the voice allocation, the
 *                metronome, the speaker (es_out_*), and the same engine run
 *                offline for the WAV export.
 *   es_patch.c   the synth's parameters as a table, and the factory presets.
 *   es_proj.c    the project model and its JSON on the card (a small parser
 *                of our own: the firmware lends no JSON library).
 *   es_file.c    WAV in (samples) and out (export), the MIDI file export.
 *   es_midi.c    a thread for USB MIDI, in and out.
 *   es_ui*.c     the interface, in LVGL's thread.
 *
 * Sharing between the threads: the project lives in PSRAM and the UI edits
 * it in place (bytes and floats, single writes: the audio thread may see a
 * step half a block late, never a torn value it could crash on). Everything
 * that is an event - a key, a pad, play, stop - goes through a
 * single-producer queue: one from the UI, one from the MIDI thread. What the
 * audio thread tells back (a note recorded, the step it is on, its timing)
 * is either a queue to the UI or a plain volatile field.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ES_SR        48000      /* the board's bus rate: no resampler in the way */
#define ES_BLK       32         /* samples per render block (0.67 ms): control rate */
#define ES_VOICES    8          /* the synth voices, shared by the three parts */
#define ES_PARTS     3          /* melodic tracks */
#define ES_DRUMS     8          /* drum tracks */
#define ES_CH        4          /* mixer channels: the three parts and the drums */
#define ES_PATS      8          /* patterns A-H */
#define ES_STEPS     64         /* the longest pattern, in sixteenths */
#define ES_NOTES     96         /* notes of one part in one pattern */
#define ES_CHAIN     64         /* slots in the song */
#define ES_BANK      16         /* user samples loaded at once */
#define ES_NAME      40

/* ---- the synth's patch: a flat table of parameters (es_patch.c) ---- */

enum {
    P_ENGINE,                                   /* 0 subtractive, 1 FM */
    P_O1W, P_O1OCT, P_O1PW,
    P_O2W, P_O2SEMI, P_O2FINE, P_O2MIX, P_NOISE,
    P_FTYPE, P_CUT, P_RES, P_FENV, P_FKEY, P_FVEL,
    P_A1, P_D1, P_S1, P_R1,
    P_A2, P_D2, P_S2, P_R2,
    P_LFOW, P_LFOR, P_LFODST, P_LFODEP,
    P_GLIDE, P_MONO,
    P_FMRAT, P_FMIDX, P_FMENV, P_FMFB,
    P_VOL, P_VELAMP,
    P_COUNT
};

enum { W_SINE, W_TRI, W_SAW, W_SQUARE, W_NOISE, W_COUNT };
enum { F_LP12, F_LP24, F_HP, F_BP };
enum { LD_PITCH, LD_CUT, LD_AMP, LD_PW };

typedef enum { PK_LIN, PK_EXP, PK_SQ, PK_INT, PK_ENUM } es_pkind_t;   /* PK_SQ: squared, fine near 0 */

typedef struct {
    const char *key;            /* in the JSON */
    const char *name;           /* N_(): shown */
    float       min, max, def;
    es_pkind_t  kind;
    uint8_t     unit;           /* es_punit: how the value is printed */
    const char *const *opts;    /* PK_ENUM: N_() names */
} es_pdesc_t;

enum { U_NONE, U_HZ, U_SEC, U_PCT, U_SEMI, U_CENT, U_OCT, U_RATIO, U_X };

extern const es_pdesc_t ES_PDESC[P_COUNT];

typedef struct {
    char  name[ES_NAME];
    float p[P_COUNT];
} es_patch_t;

int               es_patch_factory_n(void);
const es_patch_t *es_patch_factory(int i);
void              es_patch_default(es_patch_t *p);
float             es_param_to01(int id, float v);       /* for a slider */
float             es_param_from01(int id, float t);
void              es_param_format(int id, float v, char *out, size_t n);

/* ---- drums ---- */

enum {
    DS_KICK, DS_BOOM, DS_SNARE, DS_CLAP, DS_HATC, DS_HATO, DS_TOML, DS_TOMH,
    DS_CYM, DS_BELL, DS_CLAVE, DS_RIM, DS_SHAKER,
    DS_COUNT
};
extern const char *const ES_DRUM_NAMES[DS_COUNT];      /* N_() */

typedef struct {
    uint8_t src;                /* DS_*, or DS_COUNT: the sample below */
    char    sample[48];         /* a file of /estudio/muestras */
    float   tune;               /* semitones, -24..24 */
    float   decay;              /* x the sound's own, 0.1..2 */
    float   level;              /* 0..1 */
    uint8_t mute;
} es_dtrack_t;

/* ---- the mixer and the effects ---- */

typedef struct {
    float   vol, drive, dly, rev;   /* 0..1 */
    uint8_t mute;
} es_chan_t;

enum { DD_32, DD_16, DD_16D, DD_8T, DD_8, DD_8D, DD_4T, DD_4, DD_4D, DD_2, DD_COUNT };
extern const char *const ES_DLY_NAMES[DD_COUNT];

typedef struct {
    uint8_t dly_div;
    float   dly_fb, dly_tone, dly_ret;
    float   rev_size, rev_damp, rev_ret;
    float   mfilt;              /* -1 low pass .. 0 off .. 1 high pass */
    float   mres;
    float   master;
} es_fx_t;

/* ---- patterns and the song ---- */

typedef struct { uint8_t vel, prob; } es_dstep_t;      /* vel 0 = off; prob 1..100 % */
typedef struct { uint8_t step, len, note, vel, prob; } es_note_t;

typedef struct {
    uint8_t    len;                         /* 1..ES_STEPS */
    uint8_t    nn[ES_PARTS];
    es_dstep_t d[ES_DRUMS][ES_STEPS];
    es_note_t  n[ES_PARTS][ES_NOTES];
} es_pat_t;

typedef struct {
    char        name[ES_NAME];
    float       bpm;                        /* 40..240 */
    float       swing;                      /* 0.5 (straight) .. 0.75 */
    uint8_t     beats;                      /* the metronome's bar, 2..7 */
    uint8_t     met_on;
    float       met_vol;
    uint8_t     song_mode;                  /* play the chain instead of one pattern */
    uint8_t     chain_n;
    uint8_t     chain[ES_CHAIN];
    es_patch_t  part[ES_PARTS];
    es_chan_t   ch[ES_CH];
    es_dtrack_t dt[ES_DRUMS];
    es_fx_t     fx;
    es_pat_t    pat[ES_PATS];
    /* the arpeggiator, for the part being played */
    uint8_t     arp_on, arp_mode, arp_rate, arp_oct, arp_hold;
} es_proj_t;

enum { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_RANDOM, ARP_PLAYED, ARP_COUNT };
enum { AR_8, AR_8T, AR_16, AR_16T, AR_32, AR_COUNT };

void es_proj_default(es_proj_t *p);
void es_pat_clear(es_pat_t *t);
void es_pat_demo(es_proj_t *p);            /* a first beat, so it plays at once */
int  es_pat_find_note(const es_pat_t *t, int part, int step, int note);   /* index or -1 */
int  es_pat_add_note(es_pat_t *t, int part, int step, int len, int note, int vel);
void es_pat_del_note(es_pat_t *t, int part, int idx);

/* JSON on the card (es_proj.c) */
bool es_proj_save(const es_proj_t *p, const char *path);
bool es_proj_load(es_proj_t *p, const char *path, char *err, size_t errn);
bool es_patch_save(const es_patch_t *p, const char *path);
bool es_patch_load(es_patch_t *p, const char *path);

/* ---- the user's samples (es_file.c) ---- */

typedef struct {
    char     name[48];
    int16_t *data;              /* mono, PSRAM */
    uint32_t len;
    uint32_t rate;
} es_sample_t;

/* load() reads the card and belongs to the UI; find() is what the audio
 * thread uses, and never touches a file */
const es_sample_t *es_bank_load(const char *name, char *err, size_t errn);
const es_sample_t *es_bank_find(const char *name);
void               es_bank_free_all(void);           /* only with the audio thread stopped */
uint32_t           es_bank_bytes(void);

bool es_wav_write_open(const char *path, uint32_t rate, void **h);
bool es_wav_write(void *h, const int16_t *pcm, uint32_t n);
bool es_wav_write_close(void *h);
bool es_midi_export(const es_proj_t *p, const char *path, int pat, int loops);

/* ---- paths ---- */

const char *es_dir(void);                  /* <card>/estudio */
void        es_path(char *out, size_t n, const char *sub, const char *file);
void        es_mkdirs(void);

/* ---- the engine (es_engine.c) ---- */

enum {
    EC_NOTE_ON, EC_NOTE_OFF, EC_DRUM, EC_PLAY, EC_STOP, EC_PANIC, EC_BEND, EC_MOD,
    EC_SUSTAIN, EC_CUE_PAT, EC_REC, EC_ARP_RESET, EC_AUDITION,
};
enum { SRC_LIVE, SRC_MIDI };

typedef struct { uint8_t type, a, b, c; float f; } es_cmd_t;

enum { EV_REC_ON, EV_REC_OFF, EV_REC_DRUM };
typedef struct { uint8_t type, pat, a, b, c, step; } es_ev_t;

/* dest: bit0 the computer, one track's notes on channel 1; bit1 the device
 * on the host; bit2 the computer, the message as it is */
typedef struct { uint8_t st, d1, d2, dest; } es_mout_t;
#define ES_PC_ALL (ES_PARTS + 1)    /* es_set_mout's pc_track: everything, each on its channel, and the clock */

typedef struct es_engine es_engine_t;

typedef struct {
    float    load_pct;          /* audio thread's time over the block's, last window */
    float    blk_us_avg, blk_us_max;
    uint32_t underruns;         /* the ring was found empty */
    int      voices;            /* sounding */
    float    peak;              /* 0..1+, last window */
    int      queued;            /* samples waiting in the speaker's ring */
} es_stats_t;

es_engine_t *es_engine_new(es_proj_t *proj, bool live);
void         es_engine_free(es_engine_t *e);
bool         es_engine_start(es_engine_t *e);    /* opens the speaker, starts the thread */
void         es_engine_stop(es_engine_t *e);     /* waits for the thread, closes the speaker */
bool         es_engine_running(const es_engine_t *e);

bool es_cmd(es_engine_t *e, uint8_t type, int a, int b, int c, float f);       /* from the UI */
bool es_cmd_midi(es_engine_t *e, uint8_t type, int a, int b, int c, float f);  /* from the MIDI thread */
bool es_ev_take(es_engine_t *e, es_ev_t *out);                                 /* the UI drains */
bool es_mout_take(es_engine_t *e, es_mout_t *out);                             /* the MIDI thread drains */

/* what the UI shows */
bool     es_playing(const es_engine_t *e);
bool     es_recording(const es_engine_t *e);
int      es_play_pat(const es_engine_t *e);
int      es_play_step(const es_engine_t *e);     /* -1 stopped */
int      es_chain_pos(const es_engine_t *e);
int      es_cued_pat(const es_engine_t *e);      /* -1 none */
uint32_t es_drum_hits(const es_engine_t *e, int track);  /* counts up on every hit */
int      es_part_notes(const es_engine_t *e, int part, uint8_t *notes, int max);
void     es_stats(es_engine_t *e, es_stats_t *out);
bool     es_countin(const es_engine_t *e);

/* settings the UI changes */
void es_set_live_part(es_engine_t *e, int part);
void es_set_latency(es_engine_t *e, int level);   /* 0 low, 1 normal, 2 safe */
int  es_latency_ms(int level);
void es_set_voices(es_engine_t *e, int n);
void es_set_mout(es_engine_t *e, int dest_mask, bool clock, int pc_track);  /* pc_track 0..2 parts, 3 drums, ES_PC_ALL */
void es_drop_sample_refs(es_engine_t *e);
int  es_live_part(const es_engine_t *e);

/* ---- USB MIDI (es_midi.c): a thread of its own while the engine runs ---- */
bool     es_midi_start(es_engine_t *e, es_proj_t *p);
void     es_midi_stop(void);
uint32_t es_midi_in_count(void);           /* messages in so far: the UI blinks on change */

/* offline: the song (or the pattern) into a WAV, in a thread of its own */
bool  es_export_wav_start(const es_proj_t *p, const char *path, int pat, int loops);
float es_export_progress(void);                  /* 0..1, < 0 failed, 2 = done */
void  es_export_cancel(void);
bool  es_export_busy(void);

/* the GM drum note for a track's sound, and back (MIDI in/out) */
int es_drum_gm(int src);
int es_drum_from_gm(const es_proj_t *p, int note);

/* ---- small helpers ---- */

void    *es_alloc(size_t bytes);           /* PSRAM, zeroed */
void     es_free(void *p);

uint32_t es_rand(uint32_t *s);
float    es_clampf(float v, float lo, float hi);
const char *es_note_name(int note, char *buf, size_t n);   /* "C#4" */

/* the log line: es_log("...") goes to aos_hal_log with our tag */
#define ES_TAG "estudio"
