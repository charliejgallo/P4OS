/*
 * Estudio - the engine's insides, shared by es_engine.c (the thread, the
 * sequencer, the voice allocation) and es_dsp.c (the sound). Nothing else
 * includes this.
 */
#pragma once

#include "es.h"

#define ES_QN        256        /* commands in each queue (a power of two) */
#define ES_EVN       128        /* events to the UI */
#define ES_MON       256        /* MIDI messages out */
#define ES_MOFF      48         /* MIDI note-offs waiting */
#define ES_ARPN      16
#define ES_SKIPN     16
#define ES_DLY_MAX   (2 * ES_SR)   /* the delay line: 2 s */

typedef struct {
    uint8_t  on;                /* sounding, release included */
    uint8_t  gate;              /* the key is down */
    uint8_t  part, note;
    uint8_t  sus;               /* released while the pedal held it */
    uint8_t  st1, st2;          /* envelope stages: 0 off, 1 attack, 2 decay/sustain, 3 release */
    uint32_t age;
    float    vel, gain;
    float    pitch, pitch_to;   /* in MIDI notes, for the glide */
    float    ph1, ph2, phm, fbm;
    float    e1, e2;
    float    z[4];              /* two filter stages */
    int32_t  off_in;            /* samples to a sequenced note-off; < 0 none */
    uint32_t rng;
} es_voice_t;

typedef struct {
    uint8_t  on, kind, track;
    uint32_t t;                 /* samples since the hit */
    float    vel, r, m;         /* velocity, tune ratio, decay multiplier */
    float    env, env2, env3, penv;
    float    ka, kb, kc, kp;    /* per-sample decay factors */
    float    ph[6];
    float    z[4];
    float    hp;
    const es_sample_t *smp;
    float    spos, sinc, send;
    float    fade;              /* 1 playing; falling when it was retriggered or choked */
    uint32_t rng;
} es_dvoice_t;

typedef struct {
    float   lph, lval, lsh;     /* the part's LFO */
    float   bend, mod;          /* semitones, 0..1 */
    uint8_t sustain;
    uint8_t stack[8], stack_n;  /* mono: the keys held, last on top */
    float   last_pitch;
    /* envelope coefficients for this block (they only depend on the patch) */
    float   ca1, cd1, cr1, ca2, cd2, cr2;
} es_partst_t;

typedef struct { uint8_t note, ch, dest; int32_t left; } es_moff_t;
typedef struct { int8_t kind; uint8_t id; uint32_t abs; } es_skip_t;   /* kind: part 0..2, 3 drum */

typedef struct {
    es_cmd_t          q[ES_QN];
    volatile uint32_t head, tail;
} es_cmdq_t;

struct es_engine {
    es_proj_t *p;
    bool       live;            /* has the speaker and a thread; false: the export */

    /* the thread */
    volatile bool quit, done, started;
    int           lat_level;
    int           nvoices;

    es_cmdq_t     qui, qmidi;
    es_ev_t       evq[ES_EVN];
    volatile uint32_t ev_h, ev_t;
    es_mout_t     moq[ES_MON];
    volatile uint32_t mo_h, mo_t;
    int           mo_dest;      /* bit0 computer, bit1 the device on the host */
    bool          mo_clock;
    int           mo_pc_track;  /* what goes to the computer: one track on channel 1, or ES_PC_ALL */

    /* the transport */
    volatile uint8_t playing, rec, countin;
    volatile int8_t  cur_pat, cue_pat, chain_pos;
    volatile int16_t step_now;
    float            pos;       /* steps since the pattern started; < 0 counting in */
    int              next_step;
    uint32_t         abs_step;  /* steps fired since play */
    float            abs_pos;   /* steps since play, for recorded lengths */
    float            clk_next;  /* MIDI clock: the next tick, in abs steps */
    float            cin_next;
    bool             ended;     /* export: the song came round */
    int              loops_left;
    es_skip_t        skip[ES_SKIPN];
    int32_t          rec_start[ES_PARTS][128];   /* abs step a recorded key went down, -1 none */
    uint8_t          rec_step[ES_PARTS][128];    /* ...and the pattern's step it went to */

    /* voices */
    es_voice_t   v[ES_VOICES];
    uint32_t     age;
    es_dvoice_t  dv[ES_DRUMS][2];
    uint8_t      dv_next[ES_DRUMS];
    volatile uint32_t dhits[ES_DRUMS];
    es_partst_t  ps[ES_PARTS];
    volatile int live_part;
    es_moff_t    moff[ES_MOFF];

    /* the arpeggiator */
    uint8_t  arp_n, arp_note[ES_ARPN], arp_vel[ES_ARPN];
    uint8_t  arp_keys_down;
    float    arp_clock, arp_next;
    int      arp_i, arp_dir;
    int      arp_last;          /* the note it is sounding, -1 none */
    uint8_t  arp_part;

    /* effects */
    float   *dly;
    int      dly_w;
    float    dly_t, dly_lp;
    float   *rv;                /* the combs and allpasses, one block */
    int      cn[4], ci[4], an[2], ai[2];
    float   *cb[4], *ab[2];
    float    clp[4];
    float    mz[2];
    float    lim_g;
    float    met_ph, met_env, met_f;
    float    chbuf[ES_CH][ES_BLK];

    /* stats, written by the thread */
    uint32_t st_blocks, st_us_sum, st_us_max, st_t0_ms;
    float    st_peak;
    es_stats_t stats;
    uint32_t underruns;
    volatile int queued;

    uint32_t rng;
};

/* es_dsp.c */
void  es_dsp_tables(void);
void  es_dsp_alloc(es_engine_t *e);
void  es_dsp_release(es_engine_t *e);
void  es_dsp_reset(es_engine_t *e);
void  es_voice_start(es_engine_t *e, es_voice_t *v, bool fresh);
void  es_drum_hit(es_engine_t *e, int track, float vel);
void  es_drum_choke(es_engine_t *e, int kind);
void  es_click(es_engine_t *e, bool accent);
void  es_dsp_block(es_engine_t *e, float *out);   /* ES_BLK samples, the whole mix */
float es_dly_beats(int div);

/* the speaker: the board's streaming one, or SDL in the simulator */
bool es_out_open(void);
int  es_out_write(const int16_t *pcm, int n);
int  es_out_queued(void);
void es_out_close(void);
