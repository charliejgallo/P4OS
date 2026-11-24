/*
 * DJ - the audio thread: two decks, their channel strips and the mixer.
 *
 * The thread keeps the speaker's ring topped up to a target one block of
 * DJ_BLK samples at a time, like Estudio's: with the board's output for
 * playing live (aos_hal_spk_open_low) the mixer takes 96 samples (2 ms) at a
 * time and the target is 8 ms; on the usual output, 480 and 20 ms.
 *
 * A deck is a playhead (a whole sample and a fraction, in the track's own
 * samples) and a speed. The speed goes after a target: the motor's (the
 * pitch fader, the nudge, sync) when nobody touches the platter, the hand's
 * when somebody does. The hand is a target position: the UI sends how far
 * the platter turned, and the speed is what reaches that position in about
 * 15 ms, smoothed again over a few blocks, so a scratch sounds like vinyl
 * and not like a staircase. Let go and the platter keeps its momentum and
 * falls back to the motor (a backspin is a flick backwards). The samples
 * between two in the track come out of a 4-point Hermite curve.
 *
 * The channel strip, per deck: the track's gain (from the analysis, so two
 * tracks sound alike), a three-band EQ that kills each band (a low-pass at
 * 250 Hz and a high-pass at 2.5 kHz take their bands out, what is left is
 * the mid; with every knob at noon they add back to the input exactly), the
 * filter (low-pass to the left, high-pass to the right, one knob), the
 * effect, the fader. Then the crossfader, the master and a soft clip. The
 * speaker is one: the board hears the sum of both sides; the recording is
 * stereo.
 *
 * On the board the EQ's filters run through esp-dsp's dsps_biquad_f32_arp4
 * (the P4's SIMD); in the simulator, the same direct form II in C.
 */
#include "dj.h"

#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(__riscv)
#define DJ_FENCE() __asm__ volatile("fence rw,rw" ::: "memory")
#else
#define DJ_FENCE() __sync_synchronize()
#endif

#if !defined(AOS_SIM)
extern int dsps_biquad_f32_arp4(const float *input, float *output, int len, float *coef, float *w);
#define BIQUAD dsps_biquad_f32_arp4
#else
static int biquad_c(const float *in, float *out, int len, float *c, float *w)
{
    for (int i = 0; i < len; i++) {
        float d0 = in[i] - c[3] * w[0] - c[4] * w[1];
        out[i] = c[0] * d0 + c[1] * w[0] + c[2] * w[1];
        w[1] = w[0];
        w[0] = d0;
    }
    return 0;
}
#define BIQUAD biquad_c
#endif

const float DJ_FX_BEATS[] = { 0.25f, 0.5f, 0.75f, 1.0f, 2.0f, 4.0f };
const int   DJ_FX_NBEATS = 6;

#define PI_F       3.14159265f
#define ECHO_N     (1 << 17)            /* 2.7 s at 48 kHz */
#define FLG_N      1024
#define REC_N      (1 << 17)            /* frames in the recording ring: 2.7 s */
#define SCR_SMOOTH (0.015f * DJ_SR)     /* the hand: reach its position in ~15 ms */

typedef struct {
    float lo_c[5], hi_c[5];             /* the EQ's two filters: b0 b1 b2 a1 a2 */
    float lo_w[2][2], hi_w[2][2];       /* their state, per side */
    float g[3];                          /* the band gains now (ramped) */
    float f_ic1[2], f_ic2[2];           /* the filter knob's SVF */
    float f_mix;
    /* effects */
    float *echo;                         /* ECHO_N stereo frames */
    uint32_t echo_w;
    float echo_d;                        /* the delay now, in samples (glides) */
    int   echo_tail;                     /* blocks of tail left after the effect goes off */
    float flg[FLG_N * 2];
    uint32_t flg_w;
    float fx_ic1[2], fx_ic2[2];
    float lfo;                           /* 0..1 */
    float crush_l, crush_r, crush_c;
    float wet;                           /* the effect's mix now (ramped) */
    float vol;                           /* the fader now (ramped) */
    float xg;                            /* the crossfader's gain now (ramped) */
} strip_t;

typedef struct {
    bool     loaded;
    uint32_t gen;
    float    ratio;                     /* the track's rate / the bus' */
    int32_t  pi;                        /* the playhead */
    float    pf;
    float    vel;                       /* the speed now, 1 = normal */
    bool     playing;
    bool     touch;
    int32_t  ti;                        /* where the hand wants it */
    float    tf;
    bool     cue_hold;                  /* the cue previews while held */
    float    cue;
    float    hot[DJ_HOT];
    bool     loop_on;
    int32_t  la, lb;
    float    lbeats;
    bool     sync;
    float    bpm, beat0;
    float    rate;                      /* the motor's speed */
    float    pull;                      /* sync's pull on the beats: 1 +- a few % */
    float    vu;
    strip_t  st;
} deck_t;

struct dj_engine {
    dj_track_t trk[DJ_NDECK];
    deck_t     dk[DJ_NDECK];
    dj_pub_t   pub[DJ_NDECK];
    dj_mix_t   mix;
    dj_cmdq_t  q;
    volatile bool quit, done, started, running;
    volatile uint32_t blocks;
    dj_stats_t stats;
    uint32_t   underruns;
    int        queued;
    float      mvu;
    /* the recording */
    int16_t   *rec_ring;
    volatile uint32_t rec_w, rec_r;
    volatile bool rec;
    volatile uint32_t rec_frames;
    uint32_t   rec_lost;
};

/* ---- the queue ---- */

bool dj_cmd(dj_engine_t *e, int type, int a, int b, float f)
{
    if (!e) return false;
    dj_cmdq_t *q = &e->q;
    uint32_t h = q->head;
    if (h - q->tail >= DJ_QN) return false;
    dj_cmd_t *c = &q->q[h & (DJ_QN - 1)];
    c->type = (uint8_t)type;
    c->a = (uint8_t)a;
    c->b = (uint8_t)b;
    c->f = f;
    DJ_FENCE();
    q->head = h + 1;
    return true;
}

static bool q_pop(dj_cmdq_t *q, dj_cmd_t *c)
{
    uint32_t t = q->tail;
    if (t == q->head) return false;
    DJ_FENCE();
    *c = q->q[t & (DJ_QN - 1)];
    DJ_FENCE();
    q->tail = t + 1;
    return true;
}

/* ---- filters ---- */

static void rbj(float *c, int type, float f0, float q)
{
    float w = 2.0f * PI_F * f0 / (float)DJ_SR;
    float cw = cosf(w), sw = sinf(w), al = sw / (2.0f * q);
    float a0 = 1.0f + al;
    if (type == 0) {            /* low-pass */
        c[0] = (1.0f - cw) * 0.5f / a0;
        c[1] = (1.0f - cw) / a0;
        c[2] = c[0];
    } else {                    /* high-pass */
        c[0] = (1.0f + cw) * 0.5f / a0;
        c[1] = -(1.0f + cw) / a0;
        c[2] = c[0];
    }
    c[3] = -2.0f * cw / a0;
    c[4] = (1.0f - al) / a0;
}

/* the TPT state-variable filter, one sample; returns low or high */
static inline float svf(float x, float g, float k, float *ic1, float *ic2, bool high)
{
    float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float v3 = x - *ic2;
    float v1 = a1 * *ic1 + a2 * v3;
    float v2 = *ic2 + a2 * *ic1 + a3 * v3;
    *ic1 = 2.0f * v1 - *ic1;
    *ic2 = 2.0f * v2 - *ic2;
    return high ? x - k * v1 - v2 : v2;
}

static inline float soft(float x)
{
    float a = fabsf(x);
    if (a < 0.8f) return x;
    float s = (a - 0.8f) * 5.0f;
    float y = 0.8f + 0.2f * s / (1.0f + s);
    return x < 0.0f ? -y : y;
}

/* ---- reading the track ---- */

static inline void smp(const dj_track_t *t, int32_t i, float *l, float *r)
{
    if (i < 0 || (uint32_t)i >= t->nsamp) { *l = *r = 0.0f; return; }
    int8_t s = t->cmap[i >> DJ_CH_SHIFT];
    if (s < 0) { *l = *r = 0.0f; ((dj_track_t *)t)->misses++; return; }
    const int16_t *p = t->slots + ((size_t)s * DJ_CH + (size_t)(i & (DJ_CH - 1))) * 2;
    *l = (float)p[0] * (1.0f / 32768.0f);
    *r = (float)p[1] * (1.0f / 32768.0f);
}

static inline void hermite(const dj_track_t *t, int32_t i, float f, float *l, float *r)
{
    float l0, r0, l1, r1, l2, r2, l3, r3;
    smp(t, i - 1, &l0, &r0);
    smp(t, i, &l1, &r1);
    smp(t, i + 1, &l2, &r2);
    smp(t, i + 2, &l3, &r3);
    float c1 = 0.5f * (l2 - l0), c2 = l0 - 2.5f * l1 + 2.0f * l2 - 0.5f * l3, c3 = 0.5f * (l3 - l0) + 1.5f * (l1 - l2);
    *l = ((c3 * f + c2) * f + c1) * f + l1;
    c1 = 0.5f * (r2 - r0); c2 = r0 - 2.5f * r1 + 2.0f * r2 - 0.5f * r3; c3 = 0.5f * (r3 - r0) + 1.5f * (r1 - r2);
    *r = ((c3 * f + c2) * f + c1) * f + r1;
}

/* ---- the grid ---- */

static float beat_len(const deck_t *k, const dj_track_t *t)
{
    float bpm = k->bpm > 0.0f ? k->bpm : 120.0f;
    return 60.0f / bpm * (float)t->sr;
}

static float pos_f(const deck_t *k) { return (float)k->pi + k->pf; }

static void set_pos(deck_t *k, float p)
{
    if (p < 0.0f) p = 0.0f;
    k->pi = (int32_t)p;
    k->pf = p - (float)k->pi;
}

/* where the beat nearest to 'p' is (or 'p' with no tempo) */
static float snap(const deck_t *k, const dj_track_t *t, float p, bool down)
{
    if (k->bpm <= 0.0f) return p;
    float P = beat_len(k, t);
    float n = (p - k->beat0) / P;
    n = down ? floorf(n + 0.02f) : floorf(n + 0.5f);
    float s = k->beat0 + n * P;
    return s < 0.0f ? s + P : s;
}

/* the beat phase, 0..1 */
static float phase(const deck_t *k, const dj_track_t *t)
{
    float P = beat_len(k, t);
    float x = (pos_f(k) - k->beat0) / P;
    return x - floorf(x);
}

/* a jump that keeps the beat: to 'p' plus where we are inside the beat */
static float keep_phase(const deck_t *k, const dj_track_t *t, float p, bool quant)
{
    if (!quant || k->bpm <= 0.0f || !k->playing) return p;
    float P = beat_len(k, t);
    float d = fmodf(pos_f(k) - p, P);
    if (d < 0.0f) d += P;
    if (d > P * 0.5f) d -= P;
    return p + d;
}

/* ---- commands ---- */

static void deck_take(dj_engine_t *e, int d)
{
    deck_t *k = &e->dk[d];
    dj_track_t *t = &e->trk[d];
    k->loaded = true;
    k->gen = t->gen;
    k->ratio = (float)t->sr / (float)DJ_SR;
    k->playing = false;
    k->touch = false;
    k->cue_hold = false;
    k->vel = 0.0f;
    k->loop_on = false;
    k->sync = false;
    k->cue = t->cue;
    for (int i = 0; i < DJ_HOT; i++) k->hot[i] = t->hot[i];
    k->bpm = t->bpm;
    k->beat0 = t->beat0;
    set_pos(k, k->cue);
}

static void loop_set(deck_t *k, const dj_track_t *t, float beats, bool quant)
{
    float P = beat_len(k, t);
    float a = quant ? snap(k, t, pos_f(k), true) : pos_f(k);
    if (a > pos_f(k) + 1.0f) a -= P;
    if (a < 0.0f) a = 0.0f;
    k->la = (int32_t)a;
    k->lb = (int32_t)(a + beats * P);
    if ((uint32_t)k->lb > t->nsamp) k->lb = (int32_t)t->nsamp;
    k->lbeats = beats;
    k->loop_on = k->lb > k->la + 64;
}

static void command(dj_engine_t *e, const dj_cmd_t *c)
{
    int d = c->a < DJ_NDECK ? c->a : 0;
    deck_t *k = &e->dk[d];
    dj_track_t *t = &e->trk[d];
    bool q = e->mix.quant;
    if (c->type == DC_REC) {
        if (c->b && !e->rec) {
            e->rec_frames = 0;
            e->rec_lost = 0;
            e->rec = true;
        } else if (!c->b) {
            e->rec = false;
        }
        return;
    }
    if (c->type == DC_UNLOAD) {
        k->loaded = false;
        for (int h = 0; h < DJ_HOT; h++) k->hot[h] = -1.0f;
        k->cue = 0.0f;
        k->playing = false;
        k->vel = 0.0f;
        k->loop_on = false;
        return;
    }
    if (!k->loaded) return;
    switch (c->type) {
    case DC_PLAY:
        if (k->cue_hold) { k->cue_hold = false; k->playing = true; break; }
        k->playing = !k->playing;
        break;
    case DC_CUE_DOWN:
        if (k->playing) {
            k->playing = false;
            k->vel = 0.0f;
            set_pos(k, k->cue);
        } else if (fabsf(pos_f(k) - k->cue) < 64.0f) {
            k->cue_hold = true;
            k->playing = true;
        } else {
            k->cue = q ? snap(k, t, pos_f(k), false) : pos_f(k);
            set_pos(k, k->cue);
        }
        break;
    case DC_CUE_UP:
        if (k->cue_hold) {
            k->cue_hold = false;
            k->playing = false;
            k->vel = 0.0f;
            set_pos(k, k->cue);
        }
        break;
    case DC_HOT:
        if (c->b >= DJ_HOT) break;
        if (k->hot[c->b] < 0.0f) {
            k->hot[c->b] = q ? snap(k, t, pos_f(k), false) : pos_f(k);
        } else {
            set_pos(k, keep_phase(k, t, k->hot[c->b], q));
            if (k->loop_on && (k->pi < k->la || k->pi >= k->lb)) k->loop_on = false;
        }
        break;
    case DC_HOT_CLEAR:
        if (c->b < DJ_HOT) k->hot[c->b] = -1.0f;
        break;
    case DC_SEEK: {
        float p = dj_clampf(c->f, 0.0f, 1.0f) * (float)t->nsamp;
        if (q && k->bpm > 0.0f) p = snap(k, t, p, false);
        set_pos(k, keep_phase(k, t, p, q));
        k->loop_on = false;
        break;
    }
    case DC_TOUCH:
        k->touch = c->b != 0;
        k->ti = k->pi;
        k->tf = k->pf;
        break;
    case DC_SCRATCH: {
        if (!k->touch) break;
        float tf = k->tf + c->f;
        int32_t whole = (int32_t)floorf(tf);
        k->ti += whole;
        k->tf = tf - (float)whole;
        break;
    }
    case DC_LOOP:
        if (c->f <= 0.0f || (k->loop_on && fabsf(k->lbeats - c->f) < 0.001f)) k->loop_on = false;
        else loop_set(k, t, c->f, q);
        break;
    case DC_LOOP_SIZE:
        if (k->loop_on) {
            float len = (float)(k->lb - k->la);
            float nl = c->b ? len * 2.0f : len * 0.5f;
            if (nl < 32.0f || (float)k->la + nl > (float)t->nsamp) break;
            k->lb = k->la + (int32_t)nl;
            k->lbeats *= c->b ? 2.0f : 0.5f;
            while (k->pi >= k->lb) k->pi -= k->lb - k->la;
        } else {
            k->lbeats = dj_clampf(k->lbeats * (c->b ? 2.0f : 0.5f), 1.0f / 16.0f, 32.0f);
        }
        break;
    case DC_SYNC: {
        k->sync = c->b != 0;
        deck_t *o = &e->dk[1 - d];
        if (!k->sync) break;
        o->sync = false;
        /* pressed with both playing: onto the other's beat at once */
        if (k->playing && o->playing && o->loaded && k->bpm > 0.0f && o->bpm > 0.0f && !k->touch) {
            float dp = phase(o, &e->trk[1 - d]) - phase(k, t);
            if (dp > 0.5f) dp -= 1.0f;
            if (dp < -0.5f) dp += 1.0f;
            set_pos(k, pos_f(k) + dp * beat_len(k, t));
        }
        break;
    }
    case DC_BEATJUMP: {
        float p = pos_f(k) + c->f * beat_len(k, t);
        if (p < 0.0f) p = 0.0f;
        if (p >= (float)t->nsamp) p = (float)t->nsamp - 1.0f;
        set_pos(k, p);
        if (k->loop_on) {
            float sh = c->f * beat_len(k, t);
            k->la += (int32_t)sh;
            k->lb += (int32_t)sh;
            if (k->la < 0 || (uint32_t)k->lb > t->nsamp) k->loop_on = false;
        }
        break;
    }
    case DC_GRID:
        if (k->bpm <= 0.0f) break;
        k->beat0 += c->f * beat_len(k, t);
        {
            float P = beat_len(k, t);
            while (k->beat0 >= P) k->beat0 -= P;
            while (k->beat0 < 0.0f) k->beat0 += P;
        }
        break;
    case DC_BPM_SCALE:
        if (k->bpm > 0.0f) {
            float nb = k->bpm * c->f;
            if (nb >= 40.0f && nb <= 300.0f) k->bpm = nb;
        }
        break;
    default:
        break;
    }
}

/* ---- one deck, one block ---- */

static void strip_params(dj_engine_t *e, int d, float *gl, float *gm, float *gh)
{
    const dj_strip_t *s = &e->mix.s[d];
    for (int b = 0; b < 3; b++) {
        float v = dj_clampf(s->eq[b], -1.0f, 1.0f);
        float g = v < 0.0f ? (1.0f + v) * (1.0f + v) : 1.0f + v;    /* kill .. unity .. +6 dB */
        if (v < -0.97f) g = 0.0f;
        if (b == 0) *gl = g;
        else if (b == 1) *gm = g;
        else *gh = g;
    }
}

static void deck_speed(dj_engine_t *e, int d)
{
    deck_t *k = &e->dk[d];
    const dj_strip_t *s = &e->mix.s[d];
    float base = 1.0f + dj_clampf(s->pitch, -1.0f, 1.0f) * e->mix.range;
    deck_t *o = &e->dk[1 - d];
    k->pull = 1.0f;
    if (k->sync && o->loaded && k->bpm > 0.0f && o->bpm > 0.0f) {
        float target = o->bpm * o->rate;
        float r = target / k->bpm;
        while (r > 1.5f) r *= 0.5f;
        while (r < 0.75f) r *= 2.0f;
        base = r;
        /* the beats, too: what is left after the press is pulled in */
        if (k->playing && o->playing && !k->touch) {
            float dp = phase(o, &e->trk[1 - d]) - phase(k, &e->trk[d]);
            if (dp > 0.5f) dp -= 1.0f;
            if (dp < -0.5f) dp += 1.0f;
            k->pull = 1.0f + dj_clampf(dp * 0.6f, -0.04f, 0.04f);
        }
    }
    k->rate = base;
}

static void deck_block(dj_engine_t *e, int d, float *L, float *R)
{
    deck_t *k = &e->dk[d];
    dj_track_t *t = &e->trk[d];
    if (!k->loaded || t->state != DJ_TS_READY || t->gen != k->gen) {
        memset(L, 0, DJ_BLK * sizeof(float));
        memset(R, 0, DJ_BLK * sizeof(float));
        return;
    }
    const dj_strip_t *s = &e->mix.s[d];
    float motor = k->playing ? k->rate * k->pull * (1.0f + dj_clampf(s->bend, -1.0f, 1.0f) * 0.12f) : 0.0f;
    float vt, a;
    if (k->touch) {
        float diff = (float)(k->ti - k->pi) + (k->tf - k->pf);
        vt = dj_clampf(diff / (SCR_SMOOTH * k->ratio), -10.0f, 10.0f);
        a = 0.3f;
    } else {
        vt = motor;
        /* a quick start, a short brake, and a released platter's momentum */
        a = fabsf(vt) > fabsf(k->vel) ? 0.06f : 0.035f;
        if (k->playing && fabsf(k->vel - vt) < 0.002f) a = 1.0f;
    }
    float v0 = k->vel, v1 = v0 + (vt - v0) * a;
    if (!k->playing && !k->touch && fabsf(v1) < 0.003f) v1 = 0.0f;
    float g = t->gain;
    for (int i = 0; i < DJ_BLK; i++) {
        float v = v0 + (v1 - v0) * (float)(i + 1) * (1.0f / DJ_BLK);
        float l, r;
        if (v == 0.0f && k->pf == 0.0f) smp(t, k->pi, &l, &r);
        else hermite(t, k->pi, k->pf, &l, &r);
        L[i] = l * g;
        R[i] = r * g;
        float pf = k->pf + v * k->ratio;
        int32_t w = (int32_t)floorf(pf);
        k->pi += w;
        k->pf = pf - (float)w;
        if (k->loop_on && v > 0.0f && k->pi >= k->lb) k->pi -= k->lb - k->la;
        if ((uint32_t)k->pi >= t->nsamp && k->pi > 0) {
            k->pi = (int32_t)t->nsamp - 1;
            k->pf = 0.0f;
            k->playing = false;
            k->cue_hold = false;
            v1 = 0.0f;
        } else if (k->pi < 0) {
            k->pi = 0;
            k->pf = 0.0f;
        }
    }
    k->vel = v1;
    if (k->touch) {
        /* the hand cannot leave the track */
        if (k->ti < 0) { k->ti = 0; k->tf = 0.0f; }
        if ((uint32_t)k->ti >= t->nsamp) k->ti = (int32_t)t->nsamp - 1;
    }
}

static void strip_block(dj_engine_t *e, int d, float *L, float *R)
{
    deck_t *k = &e->dk[d];
    strip_t *st = &k->st;
    const dj_strip_t *s = &e->mix.s[d];
    dj_track_t *t = &e->trk[d];

    /* EQ: the low and the high taken out by two filters, the rest is mid */
    float gl, gm, gh;
    strip_params(e, d, &gl, &gm, &gh);
    float lo[2][DJ_BLK], hi[2][DJ_BLK];
    float *x[2] = { L, R };
    bool flat = fabsf(gl - 1.0f) + fabsf(gm - 1.0f) + fabsf(gh - 1.0f) < 0.002f &&
                fabsf(st->g[0] - 1.0f) + fabsf(st->g[1] - 1.0f) + fabsf(st->g[2] - 1.0f) < 0.002f;
    for (int c = 0; c < 2; c++) {
        BIQUAD(x[c], lo[c], DJ_BLK, st->lo_c, st->lo_w[c]);
        BIQUAD(x[c], hi[c], DJ_BLK, st->hi_c, st->hi_w[c]);
    }
    if (!flat) {
        float g0[3] = { st->g[0], st->g[1], st->g[2] }, g1[3] = { gl, gm, gh };
        for (int i = 0; i < DJ_BLK; i++) {
            float u = (float)(i + 1) * (1.0f / DJ_BLK);
            float a = g0[0] + (g1[0] - g0[0]) * u, b = g0[1] + (g1[1] - g0[1]) * u, c2 = g0[2] + (g1[2] - g0[2]) * u;
            for (int c = 0; c < 2; c++) {
                float mid = x[c][i] - lo[c][i] - hi[c][i];
                x[c][i] = a * lo[c][i] + b * mid + c2 * hi[c][i];
            }
        }
    }
    st->g[0] = gl; st->g[1] = gm; st->g[2] = gh;

    /* the filter knob */
    float fk = dj_clampf(s->filt, -1.0f, 1.0f);
    float want = fabsf(fk) > 0.03f ? 1.0f : 0.0f;
    if (want > 0.0f || st->f_mix > 0.0f) {
        bool high = fk > 0.0f;
        float a = fabsf(fk);
        float fc = high ? 20.0f * powf(400.0f, a) : 20000.0f * powf(0.004f, a);    /* to 8 kHz / down to 80 Hz */
        float g = tanf(PI_F * dj_clampf(fc, 20.0f, 20000.0f) / (float)DJ_SR);
        float kq = 1.0f / (0.8f + a * 1.4f);
        float m0 = st->f_mix, m1 = m0 + (want - m0) * 0.2f;
        if (m1 < 0.001f) m1 = 0.0f;
        for (int i = 0; i < DJ_BLK; i++) {
            float m = m0 + (m1 - m0) * (float)(i + 1) * (1.0f / DJ_BLK);
            for (int c = 0; c < 2; c++) {
                float y = svf(x[c][i], g, kq, &st->f_ic1[c], &st->f_ic2[c], high);
                x[c][i] += (y - x[c][i]) * m;
            }
        }
        st->f_mix = m1;
    }

    /* the effect */
    float bpm = k->bpm > 0.0f ? k->bpm * k->rate : 120.0f;
    float beats = DJ_FX_BEATS[s->fx_beats < 0 ? 0 : s->fx_beats >= DJ_FX_NBEATS ? DJ_FX_NBEATS - 1 : s->fx_beats];
    float amt = dj_clampf(s->fx_amt, 0.0f, 1.0f);
    float wet1 = s->fx_on ? 1.0f : 0.0f, wet0 = st->wet;
    float wetn = wet0 + (wet1 - wet0) * 0.15f;
    if (fabsf(wetn - wet1) < 0.002f) wetn = wet1;
    int fx = s->fx_type;
    if (fx == FX_ECHO && st->echo) {
        if (s->fx_on) st->echo_tail = (int)(4.0f * DJ_SR / DJ_BLK);
        if (st->echo_tail > 0) {
            st->echo_tail--;
            float dsamp = dj_clampf(60.0f / bpm * beats * DJ_SR, 64.0f, (float)(ECHO_N - 4));
            float fb = 0.35f + 0.4f * amt;
            for (int i = 0; i < DJ_BLK; i++) {
                float m = wet0 + (wetn - wet0) * (float)(i + 1) * (1.0f / DJ_BLK);
                st->echo_d += (dsamp - st->echo_d) * 0.0005f;
                float rp = (float)st->echo_w - st->echo_d;
                while (rp < 0.0f) rp += (float)ECHO_N;
                uint32_t i0 = (uint32_t)rp;
                float fr = rp - (float)i0;
                uint32_t a0 = i0 & (ECHO_N - 1), a1 = (i0 + 1) & (ECHO_N - 1);
                float el = st->echo[a0 * 2] + (st->echo[a1 * 2] - st->echo[a0 * 2]) * fr;
                float er = st->echo[a0 * 2 + 1] + (st->echo[a1 * 2 + 1] - st->echo[a0 * 2 + 1]) * fr;
                uint32_t w = st->echo_w & (ECHO_N - 1);
                /* the feedback crosses the sides a little: a wider tail */
                st->echo[w * 2] = x[0][i] * m + (el * 0.8f + er * 0.2f) * fb;
                st->echo[w * 2 + 1] = x[1][i] * m + (er * 0.8f + el * 0.2f) * fb;
                st->echo_w++;
                x[0][i] += el * (0.4f + 0.6f * amt);
                x[1][i] += er * (0.4f + 0.6f * amt);
            }
        }
    } else if (fx == FX_FLANGER && (wetn > 0.0f || wet0 > 0.0f)) {
        float lfo_inc = bpm / 60.0f / (beats * 4.0f) / (float)DJ_SR;
        float fb = 0.3f + 0.5f * amt;
        for (int i = 0; i < DJ_BLK; i++) {
            float m = (wet0 + (wetn - wet0) * (float)(i + 1) * (1.0f / DJ_BLK)) * (0.35f + 0.35f * amt);
            st->lfo += lfo_inc;
            if (st->lfo >= 1.0f) st->lfo -= 1.0f;
            float tri = st->lfo < 0.5f ? st->lfo * 2.0f : 2.0f - st->lfo * 2.0f;
            for (int c = 0; c < 2; c++) {
                float ph = c ? (tri < 0.5f ? tri + 0.5f : 1.5f - tri) * 0.9f + 0.1f * tri : tri;   /* the sides apart */
                float dly = 24.0f + ph * 220.0f;
                float rp = (float)st->flg_w - dly;
                while (rp < 0.0f) rp += (float)FLG_N;
                uint32_t i0 = (uint32_t)rp;
                float fr = rp - (float)i0;
                float a0 = st->flg[(i0 & (FLG_N - 1)) * 2 + c], a1 = st->flg[((i0 + 1) & (FLG_N - 1)) * 2 + c];
                float dl = a0 + (a1 - a0) * fr;
                st->flg[(st->flg_w & (FLG_N - 1)) * 2 + c] = x[c][i] + dl * fb;
                x[c][i] = x[c][i] * (1.0f - m * 0.5f) + dl * m;
            }
            st->flg_w++;
        }
    } else if (fx == FX_FILTER && (wetn > 0.0f || wet0 > 0.0f)) {
        float lfo_inc = bpm / 60.0f / (beats * 4.0f) / (float)DJ_SR * DJ_BLK;
        st->lfo += lfo_inc;
        if (st->lfo >= 1.0f) st->lfo -= 1.0f;
        float sn = 0.5f - 0.5f * cosf(2.0f * PI_F * st->lfo);
        float fc = 250.0f * powf(24.0f, sn * (0.4f + 0.6f * amt));
        float g = tanf(PI_F * fc / (float)DJ_SR);
        float kq = 1.0f / (1.5f + 4.0f * amt);
        for (int i = 0; i < DJ_BLK; i++) {
            float m = wet0 + (wetn - wet0) * (float)(i + 1) * (1.0f / DJ_BLK);
            for (int c = 0; c < 2; c++) {
                float y = svf(x[c][i], g, kq, &st->fx_ic1[c], &st->fx_ic2[c], false);
                x[c][i] += (y * 0.9f - x[c][i]) * m;
            }
        }
    } else if (fx == FX_CRUSH && (wetn > 0.0f || wet0 > 0.0f)) {
        float hold = 1.0f + amt * amt * 22.0f;
        float q = powf(2.0f, 12.0f - amt * 8.5f);
        for (int i = 0; i < DJ_BLK; i++) {
            float m = wet0 + (wetn - wet0) * (float)(i + 1) * (1.0f / DJ_BLK);
            st->crush_c += 1.0f;
            if (st->crush_c >= hold) {
                st->crush_c -= hold;
                st->crush_l = floorf(x[0][i] * q + 0.5f) / q;
                st->crush_r = floorf(x[1][i] * q + 0.5f) / q;
            }
            x[0][i] += (st->crush_l - x[0][i]) * m;
            x[1][i] += (st->crush_r - x[1][i]) * m;
        }
    }
    st->wet = wetn;
    (void)t;
}

/* ---- the mix ---- */

static void xfade_gains(const dj_mix_t *m, float *ga, float *gb)
{
    float x = dj_clampf(m->xfade, -1.0f, 1.0f);
    if (m->xcurve) {
        /* sharp: both full except at the very ends (scratching) */
        *ga = x > 0.9f ? (1.0f - x) * 10.0f : 1.0f;
        *gb = x < -0.9f ? (1.0f + x) * 10.0f : 1.0f;
    } else {
        float u = (x + 1.0f) * 0.25f * PI_F;
        *ga = cosf(u) * 1.41421356f;
        *gb = sinf(u) * 1.41421356f;
        if (*ga > 1.0f) *ga = 1.0f;
        if (*gb > 1.0f) *gb = 1.0f;
    }
}

static void engine_block(dj_engine_t *e, float *ML, float *MR)
{
    dj_cmd_t c;
    while (q_pop(&e->q, &c)) command(e, &c);
    for (int d = 0; d < DJ_NDECK; d++) {
        dj_track_t *t = &e->trk[d];
        deck_t *k = &e->dk[d];
        if (t->state == DJ_TS_READY && (!k->loaded || t->gen != k->gen) && e->pub[d].gen != t->gen) deck_take(e, d);
        if (t->state != DJ_TS_READY && k->loaded) k->loaded = false;
        if (k->loaded && k->bpm <= 0.0f && t->bpm > 0.0f) { k->bpm = t->bpm; k->beat0 = t->beat0; }
    }
    /* the leader's speed before the follower's */
    int first = e->dk[0].sync ? 1 : 0;
    deck_speed(e, first);
    deck_speed(e, 1 - first);

    float ga, gb;
    xfade_gains(&e->mix, &ga, &gb);
    float master = dj_clampf(e->mix.master, 0.0f, 1.0f);
    master *= master;
    memset(ML, 0, DJ_BLK * sizeof(float));
    memset(MR, 0, DJ_BLK * sizeof(float));
    for (int d = 0; d < DJ_NDECK; d++) {
        deck_t *k = &e->dk[d];
        strip_t *st = &k->st;
        float L[DJ_BLK], R[DJ_BLK];
        deck_block(e, d, L, R);
        strip_block(e, d, L, R);
        float v = dj_clampf(e->mix.s[d].vol, 0.0f, 1.0f);
        float vol1 = v * v, vol0 = st->vol;
        float xg1 = d == 0 ? ga : gb, xg0 = st->xg;
        float pk = 0.0f;
        for (int i = 0; i < DJ_BLK; i++) {
            float u = (float)(i + 1) * (1.0f / DJ_BLK);
            float gv = vol0 + (vol1 - vol0) * u;
            float l = L[i] * gv, r = R[i] * gv;
            float al = fabsf(l), ar = fabsf(r);
            if (al > pk) pk = al;
            if (ar > pk) pk = ar;
            float gx = xg0 + (xg1 - xg0) * u;
            ML[i] += l * gx;
            MR[i] += r * gx;
        }
        st->vol = vol1;
        st->xg = xg1;
        k->vu = dj_maxf(k->vu * 0.985f, pk);
    }
    float mp = 0.0f;
    for (int i = 0; i < DJ_BLK; i++) {
        ML[i] = soft(ML[i] * master);
        MR[i] = soft(MR[i] * master);
        float a = fabsf(ML[i]) > fabsf(MR[i]) ? fabsf(ML[i]) : fabsf(MR[i]);
        if (a > mp) mp = a;
    }
    e->mvu = dj_maxf(e->mvu * 0.985f, mp);

    if (e->rec && e->rec_ring) {
        uint32_t w = e->rec_w;
        if (w - e->rec_r + DJ_BLK <= REC_N) {
            for (int i = 0; i < DJ_BLK; i++) {
                uint32_t j = (w + (uint32_t)i) & (REC_N - 1);
                e->rec_ring[j * 2] = (int16_t)(ML[i] * 32767.0f);
                e->rec_ring[j * 2 + 1] = (int16_t)(MR[i] * 32767.0f);
            }
            DJ_FENCE();
            e->rec_w = w + DJ_BLK;
            e->rec_frames += DJ_BLK;
        } else {
            e->rec_lost += DJ_BLK;
        }
    }

    /* what the UI and the loader read */
    for (int d = 0; d < DJ_NDECK; d++) {
        deck_t *k = &e->dk[d];
        dj_pub_t *p = &e->pub[d];
        p->pos = k->pi;
        p->vel = k->vel;
        p->rate = k->rate;
        p->playing = k->playing;
        p->loop_on = k->loop_on;
        p->loop_beats = k->lbeats;
        p->loop_a = k->la;
        p->loop_b = k->lb;
        p->cue = k->cue;
        for (int h = 0; h < DJ_HOT; h++) p->hot[h] = k->hot[h];
        p->sync = k->sync;
        p->vu = k->vu;
        p->bpm = k->bpm;
        p->beat0 = k->beat0;
        p->cue_held = k->cue_hold;
        p->gen = k->loaded ? k->gen : 0;
    }
    DJ_FENCE();
    e->blocks++;
}

/* ---- the thread ---- */

static bool out_low(void)
{
#if defined(AOS_SIM)
    return false;
#else
    return aos_hal_spk_latency_ms() <= 12;
#endif
}

static int target_q(void)
{
    return out_low() ? 96 + 288 : 480 + 480;      /* 8 ms on the live output, 20 on the usual */
}

static void audio_main(void *arg)
{
    dj_engine_t *e = arg;
    float L[DJ_BLK], R[DJ_BLK];
    int16_t pcm[DJ_BLK];
    uint32_t win_t0 = (uint32_t)aos_hal_uptime_ms();
    uint32_t blocks = 0, us_sum = 0, us_max = 0, m0 = 0;
#if defined(AOS_SIM)
    /* with no sound device (SDL_AUDIODRIVER=dummy) the queue always reads
     * empty: keep to the clock as well */
    uint64_t clk0 = aos_hal_uptime_us(), made = 0;
#endif
    while (!e->quit) {
        int q = aos_hal_spk_queued();
        e->queued = q;
        bool full = q + DJ_BLK > target_q();
#if defined(AOS_SIM)
        if ((made * 1000000u) / DJ_SR > aos_hal_uptime_us() - clk0 + 20000u) full = true;
#endif
        if (full) {
            aos_hal_sleep_ms(1);
            continue;
        }
#if defined(AOS_SIM)
        made += DJ_BLK;
#endif
        if (q == 0 && e->started) e->underruns++;
        uint64_t t0 = aos_hal_uptime_us();
        engine_block(e, L, R);
        for (int i = 0; i < DJ_BLK; i++) {
            float v = (L[i] + R[i]) * 0.5f * 32767.0f;
            pcm[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
        }
        uint32_t us = (uint32_t)(aos_hal_uptime_us() - t0);
        aos_hal_spk_write(pcm, DJ_BLK);
        e->started = true;
        blocks++;
        us_sum += us;
        if (us > us_max) us_max = us;
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        if (now - win_t0 >= 1000 && blocks) {
            float blk_us = (float)DJ_BLK * 1e6f / (float)DJ_SR;
            e->stats.blk_us_avg = (float)us_sum / (float)blocks;
            e->stats.blk_us_max = (float)us_max;
            e->stats.load_pct = 100.0f * e->stats.blk_us_avg / blk_us;
            e->stats.blocks = blocks;
#if defined(AOS_SIM)
            e->stats.underruns = e->underruns;
#else
            e->stats.underruns = (uint32_t)aos_hal_spk_dry();
#endif
            uint32_t m = e->trk[0].misses + e->trk[1].misses;
            e->stats.misses = m - m0;
            m0 = m;
            e->stats.queued = q;
            blocks = us_sum = us_max = 0;
            win_t0 = now;
        }
    }
    e->done = true;
}

static void strip_init(strip_t *st)
{
    rbj(st->lo_c, 0, 250.0f, 0.7071f);
    rbj(st->hi_c, 1, 2500.0f, 0.7071f);
    st->g[0] = st->g[1] = st->g[2] = 1.0f;
    st->echo_d = 0.5f * DJ_SR;
}

dj_engine_t *dj_engine_new(void)
{
    dj_engine_t *e = dj_alloc(sizeof *e);
    if (!e) return NULL;
    bool ok = true;
    for (int d = 0; d < DJ_NDECK; d++) {
        ok = ok && dj_track_alloc_cache(&e->trk[d]);
        strip_init(&e->dk[d].st);
        e->dk[d].lbeats = 4.0f;
        for (int h = 0; h < DJ_HOT; h++) e->dk[d].hot[h] = -1.0f;
        e->dk[d].rate = 1.0f;
        e->dk[d].pull = 1.0f;
        e->mix.s[d].vol = 0.8f;
        e->mix.s[d].fx_amt = 0.5f;
        e->mix.s[d].fx_beats = 3;
        for (int h = 0; h < DJ_HOT; h++) e->pub[d].hot[h] = -1.0f;
    }
    e->mix.master = 0.85f;
    e->mix.range = 0.08f;
    e->mix.quant = true;
    e->mix.vinyl = true;
    if (!ok) {
        dj_engine_free(e);
        return NULL;
    }
    return e;
}

void dj_engine_free(dj_engine_t *e)
{
    if (!e) return;
    dj_engine_stop(e);
    for (int d = 0; d < DJ_NDECK; d++) {
        dj_track_close(&e->trk[d]);
        dj_track_free_cache(&e->trk[d]);
        dj_free(e->dk[d].st.echo);
    }
    dj_free(e->rec_ring);
    dj_free(e);
}

bool dj_engine_start(dj_engine_t *e)
{
    if (!e || e->running) return e && e->running;
    if (!aos_hal_spk_open_low(DJ_SR)) {
        aos_hal_log(DJ_TAG, "the speaker did not open");
        return false;
    }
    e->quit = false;
    e->done = false;
    e->started = false;
    /* above the player (5) and LVGL (4): a late block is a hole in the sound.
     * It sleeps whenever the ring is full, so it never holds the core. */
    if (!aos_hal_thread_start("dj_audio", audio_main, e, 8192, 6)) {
        aos_hal_spk_close();
        aos_hal_log(DJ_TAG, "no audio thread");
        return false;
    }
    e->running = true;
    return true;
}

void dj_engine_stop(dj_engine_t *e)
{
    if (!e || !e->running) return;
    e->quit = true;
    for (int i = 0; i < 400 && !e->done; i++) aos_hal_sleep_ms(5);
    if (!e->done) aos_hal_log(DJ_TAG, "the audio thread did not stop");
    e->running = false;
    aos_hal_spk_close();
    /* the decks stop where they are: the music does not go on without us */
    for (int d = 0; d < DJ_NDECK; d++) {
        e->dk[d].playing = false;
        e->dk[d].cue_hold = false;
        e->dk[d].touch = false;
        e->dk[d].vel = 0.0f;
        e->pub[d].playing = false;
    }
    e->rec = false;
}

/* 1 MB per deck for the echo's line and 512 KB for the recording ring are
 * only taken when they are first used: a set with no echo and no recording
 * leaves 2.5 MB of PSRAM to the rest of the board. The audio thread looks
 * at the pointers before it touches them; they are set before the switch
 * that makes it look. */
bool dj_engine_echo_mem(dj_engine_t *e, int d)
{
    if (!e || d < 0 || d >= DJ_NDECK) return false;
    if (e->dk[d].st.echo) return true;
    float *b = dj_alloc((size_t)ECHO_N * 2 * sizeof(float));
    if (!b) return false;
    DJ_FENCE();
    e->dk[d].st.echo = b;
    return true;
}

bool dj_engine_rec_mem(dj_engine_t *e)
{
    if (!e) return false;
    if (e->rec_ring) return true;
    int16_t *b = dj_alloc((size_t)REC_N * 4);
    if (!b) return false;
    DJ_FENCE();
    e->rec_ring = b;
    return true;
}

bool dj_engine_running(const dj_engine_t *e) { return e && e->running && !e->done; }
dj_mix_t *dj_mix(dj_engine_t *e) { return &e->mix; }
const dj_pub_t *dj_pub(const dj_engine_t *e, int deck) { return &e->pub[deck]; }
dj_track_t *dj_deck_track(dj_engine_t *e, int deck) { return &e->trk[deck]; }
float dj_master_vu(const dj_engine_t *e) { return e->mvu; }
uint32_t dj_engine_blocks(const dj_engine_t *e) { return e->blocks; }
bool dj_rec_on(const dj_engine_t *e) { return e->rec; }
uint32_t dj_rec_ms(const dj_engine_t *e) { return (uint32_t)(e->rec_frames / (DJ_SR / 1000)); }

void dj_stats(dj_engine_t *e, dj_stats_t *out)
{
    *out = e->stats;
    out->queued = e->queued;
}

int dj_rec_take(dj_engine_t *e, int16_t *out, int max_frames)
{
    if (!e->rec_ring) return 0;
    uint32_t r = e->rec_r, w = e->rec_w;
    DJ_FENCE();
    int n = (int)(w - r);
    if (n > max_frames) n = max_frames;
    for (int i = 0; i < n; i++) {
        uint32_t j = (r + (uint32_t)i) & (REC_N - 1);
        out[i * 2] = e->rec_ring[j * 2];
        out[i * 2 + 1] = e->rec_ring[j * 2 + 1];
    }
    DJ_FENCE();
    e->rec_r = r + (uint32_t)n;
    return n;
}
