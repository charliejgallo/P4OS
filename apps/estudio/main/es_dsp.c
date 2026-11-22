/*
 * Estudio - the sound. Everything here runs in the audio thread, one block
 * of ES_BLK samples at a time, in float.
 *
 * The synth voice: two oscillators (sine from a table, triangle, saw and
 * square with polyBLEP edges, noise), or a two-operator FM pair; a TPT
 * state-variable filter (Zavalishin's), one stage or two for 24 dB; two
 * exponential ADSRs; the part's LFO; a glide in MIDI notes. What depends only
 * on the patch (the envelope coefficients, the LFO) is worked out once per
 * part and block; what depends on the voice (the pitch, the cutoff) once per
 * voice and block; only the oscillators, the filter and the envelopes run
 * per sample.
 *
 * The drums are synthesized the way the analogue boxes did it, from
 * scratch and without a single recorded sample: sines with a falling pitch
 * for the kick and the toms, six detuned squares through a high pass for
 * the hats and the cymbal, band-passed noise in bursts for the clap. The
 * user's own WAVs play through the same voices.
 *
 * Memory: the delay line (2 s, 375 KB) and the reverb's eight buffers
 * (~35 KB) are in PSRAM like the rest; they are read and written in order,
 * so the cache keeps up (docs/MEMORY.md). Nothing here asks for internal RAM.
 */
#include "es_eng.h"

#include <math.h>
#include <string.h>

#define TAB_N   2048
#define TWO_PI  6.28318531f
#define INV_SR  (1.0f / (float)ES_SR)

static float s_sin[TAB_N + 1];
static bool  s_tab_ok;

void es_dsp_tables(void)
{
    if (s_tab_ok) return;
    for (int i = 0; i <= TAB_N; i++) s_sin[i] = sinf(TWO_PI * (float)i / (float)TAB_N);
    s_tab_ok = true;
}

/* a sine of 'ph' turns */
static inline float tsin(float ph)
{
    ph -= floorf(ph);
    float x = ph * (float)TAB_N;
    int   i = (int)x;
    float f = x - (float)i;
    i &= TAB_N - 1;
    return s_sin[i] + (s_sin[i + 1] - s_sin[i]) * f;
}

static inline float noise(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* the correction around a step of the waveform, t and dt in turns */
static inline float blep(float t, float dt, float idt)
{
    if (t < dt) { t *= idt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) * idt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static inline float osc(int w, float ph, float dt, float idt, float pw, uint32_t *rng)
{
    switch (w) {
    case W_SINE: return tsin(ph);
    case W_TRI:  return 4.0f * fabsf(ph - 0.5f) - 1.0f;
    case W_SAW:  return 2.0f * ph - 1.0f - blep(ph, dt, idt);
    case W_SQUARE: {
        float v = ph < pw ? 1.0f : -1.0f;
        v += blep(ph, dt, idt);
        float p2 = ph - pw;
        if (p2 < 0.0f) p2 += 1.0f;
        return v - blep(p2, dt, idt);
    }
    default: return noise(rng);
    }
}

/* tanh, near enough, and never above 1 */
static inline float sat(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* ---- the state-variable filter ---- */

typedef struct { float a1, a2, a3, k; } svf_t;

static inline void svf_set(svf_t *f, float fc, float k)
{
    if (fc < 16.0f) fc = 16.0f;
    if (fc > 0.45f * ES_SR) fc = 0.45f * ES_SR;
    float g = tanf(3.14159265f * fc * INV_SR);
    f->k = k;
    f->a1 = 1.0f / (1.0f + g * (g + k));
    f->a2 = g * f->a1;
    f->a3 = g * f->a2;
}

/* one sample; z[0], z[1] the state; returns the low pass, bp/hp through
 * the pointers */
static inline float svf_run(const svf_t *f, float *z, float x, float *bp, float *hp)
{
    float v3 = x - z[1];
    float v1 = f->a1 * z[0] + f->a2 * v3;
    float v2 = z[1] + f->a2 * z[0] + f->a3 * v3;
    z[0] = 2.0f * v1 - z[0];
    z[1] = 2.0f * v2 - z[1];
    if (bp) *bp = v1;
    if (hp) *hp = x - f->k * v1 - v2;
    return v2;
}

/* ---- envelopes ---- */

/* the factor that brings a value 99 % of the way in 'sec' */
static inline float env_k(float sec)
{
    if (sec < 0.0005f) sec = 0.0005f;
    return 1.0f - expf(-4.6f / (sec * ES_SR));
}

/* attack: aims at 1.25 and stops at 1, so it is a curve that takes 'sec' */
static inline float att_k(float sec)
{
    if (sec < 0.0005f) sec = 0.0005f;
    return 1.0f - expf(-1.609f / (sec * ES_SR));
}

static inline float env_step(uint8_t *st, float e, float ka, float kd, float s, float kr)
{
    switch (*st) {
    case 1:
        e += (1.25f - e) * ka;
        if (e >= 1.0f) { e = 1.0f; *st = 2; }
        return e;
    case 2:
        return e + (s - e) * kd;
    case 3:
        return e - e * kr;
    default:
        return 0.0f;
    }
}

/* ---- allocation ---- */

static const int COMB_LEN[4] = { 1116, 1188, 1277, 1356 };     /* Freeverb's, at 44.1 kHz */
static const int AP_LEN[2]   = { 556, 441 };

void es_dsp_alloc(es_engine_t *e)
{
    es_dsp_tables();
    e->dly = es_alloc(ES_DLY_MAX * sizeof(float));
    int total = 0;
    for (int i = 0; i < 4; i++) { e->cn[i] = COMB_LEN[i] * ES_SR / 44100; total += e->cn[i]; }
    for (int i = 0; i < 2; i++) { e->an[i] = AP_LEN[i] * ES_SR / 44100; total += e->an[i]; }
    e->rv = es_alloc((size_t)total * sizeof(float));
    float *p = e->rv;
    for (int i = 0; i < 4; i++) { e->cb[i] = p; p += e->cn[i]; }
    for (int i = 0; i < 2; i++) { e->ab[i] = p; p += e->an[i]; }
    e->lim_g = 1.0f;
}

void es_dsp_release(es_engine_t *e)
{
    es_free(e->dly);
    es_free(e->rv);
    e->dly = NULL;
    e->rv = NULL;
}

/* silence everything: the buffers too */
void es_dsp_reset(es_engine_t *e)
{
    memset(e->v, 0, sizeof e->v);
    memset(e->dv, 0, sizeof e->dv);
    if (e->dly) memset(e->dly, 0, ES_DLY_MAX * sizeof(float));
    if (e->rv) {
        int total = e->cn[0] + e->cn[1] + e->cn[2] + e->cn[3] + e->an[0] + e->an[1];
        memset(e->rv, 0, (size_t)total * sizeof(float));
    }
    memset(e->clp, 0, sizeof e->clp);
    memset(e->mz, 0, sizeof e->mz);
    e->dly_lp = 0;
    e->met_env = 0;
    e->lim_g = 1.0f;
}

float es_dly_beats(int div)
{
    static const float B[DD_COUNT] = { 0.125f, 0.25f, 0.375f, 1.0f / 3.0f, 0.5f, 0.75f,
                                       2.0f / 3.0f, 1.0f, 1.5f, 2.0f };
    return B[div < 0 || div >= DD_COUNT ? DD_8D : div];
}

/* ---- the synth voice ---- */

void es_voice_start(es_engine_t *e, es_voice_t *v, bool fresh)
{
    (void)e;
    if (fresh) {
        memset(v->z, 0, sizeof v->z);
        v->e1 = v->e2 = 0.0f;
        v->ph1 = 0.0f;
        v->ph2 = 0.25f;
        v->phm = 0.0f;
        v->fbm = 0.0f;
    }
    v->st1 = v->st2 = 1;
}

/* the part's LFO and envelope coefficients, once per block */
static void part_block(es_engine_t *e, int part)
{
    const float *P = e->p->part[part].p;
    es_partst_t *ps = &e->ps[part];
    ps->lph += P[P_LFOR] * ES_BLK * INV_SR;
    bool wrap = false;
    if (ps->lph >= 1.0f) { ps->lph -= floorf(ps->lph); wrap = true; }
    float ph = ps->lph, v;
    switch ((int)P[P_LFOW]) {
    case W_SINE:   v = tsin(ph); break;
    case W_TRI:    v = 1.0f - 4.0f * fabsf(ph - 0.5f); break;
    case W_SAW:    v = 1.0f - 2.0f * ph; break;
    case W_SQUARE: v = ph < 0.5f ? 1.0f : -1.0f; break;
    default:
        if (wrap) ps->lsh = noise(&e->rng);
        v = ps->lsh;
        break;
    }
    ps->lval = v;
    ps->ca1 = att_k(P[P_A1]);
    ps->cd1 = env_k(P[P_D1]);
    ps->cr1 = env_k(P[P_R1]);
    ps->ca2 = att_k(P[P_A2]);
    ps->cd2 = env_k(P[P_D2]);
    ps->cr2 = env_k(P[P_R2]);
}

static void voice_block(es_engine_t *e, es_voice_t *v, float *out)
{
    const float *P = e->p->part[v->part].p;
    es_partst_t *ps = &e->ps[v->part];

    /* the glide: a one-pole on the pitch, in notes */
    if (v->pitch != v->pitch_to) {
        float gt = P[P_GLIDE];
        float k = gt < 0.002f ? 1.0f : 1.0f - expf(-4.6f * ES_BLK / (gt * ES_SR));
        v->pitch += (v->pitch_to - v->pitch) * k;
        if (fabsf(v->pitch_to - v->pitch) < 0.002f) v->pitch = v->pitch_to;
    }

    int   dst = (int)P[P_LFODST];
    float dep = P[P_LFODEP] + ps->mod;
    if (dep > 1.0f) dep = 1.0f;
    float lfo = ps->lval * dep;

    float semis = v->pitch + ps->bend + 12.0f * P[P_O1OCT];
    if (dst == LD_PITCH) semis += lfo * dep * 12.0f;        /* square law: fine vibrato low down */
    float f1 = 440.0f * exp2f((semis - 69.0f) * (1.0f / 12.0f));
    bool fm = P[P_ENGINE] >= 0.5f;
    float f2 = fm ? f1 * P[P_FMRAT] : f1 * exp2f((P[P_O2SEMI] + P[P_O2FINE] * 0.01f) * (1.0f / 12.0f));
    float dt1 = f1 * INV_SR, dt2 = f2 * INV_SR;
    if (dt1 > 0.45f) dt1 = 0.45f;
    if (dt2 > 0.45f) dt2 = 0.45f;
    float idt1 = 1.0f / dt1, idt2 = 1.0f / dt2;
    float pw = P[P_O1PW] + (dst == LD_PW ? lfo * 0.45f : 0.0f);
    pw = es_clampf(pw, 0.05f, 0.95f);

    /* the cutoff, from the filter envelope's value now */
    float oct = P[P_FENV] * 5.0f * v->e2 + P[P_FKEY] * (v->pitch - 60.0f) * (1.0f / 12.0f)
              + P[P_FVEL] * (v->vel - 0.75f) * 3.0f;
    if (dst == LD_CUT) oct += lfo * 3.0f;
    float cut = P[P_CUT] * exp2f(oct);
    float res = P[P_RES];
    int ftype = (int)P[P_FTYPE];
    svf_t fa, fb;
    svf_set(&fa, cut, 2.0f - 1.97f * res);
    if (ftype == F_LP24) svf_set(&fb, cut, 1.414f);

    float amp = v->gain;
    if (dst == LD_AMP) amp *= 1.0f - 0.5f * (1.0f + ps->lval) * dep;
    /* resonance makes a filter louder at the cutoff and quieter below it */
    amp *= 1.0f - 0.35f * res;

    int w1 = (int)P[P_O1W], w2 = (int)P[P_O2W];
    float mix2 = P[P_O2MIX], nz = P[P_NOISE];
    float s1 = P[P_S1], s2 = P[P_S2];
    float idx = P[P_FMIDX], idxe = P[P_FMENV], fbk = P[P_FMFB] * 0.9f;
    float mixn = 1.0f / (1.0f + mix2 * 0.7f + nz * 0.5f);

    if (v->gate == 0 && v->st1 && v->st1 != 3) v->st1 = 3;
    if (v->gate == 0 && v->st2 && v->st2 != 3) v->st2 = 3;

    float e1 = v->e1, e2 = v->e2;
    float ph1 = v->ph1, ph2 = v->ph2;
    for (int i = 0; i < ES_BLK; i++) {
        e1 = env_step(&v->st1, e1, ps->ca1, ps->cd1, s1, ps->cr1);
        e2 = env_step(&v->st2, e2, ps->ca2, ps->cd2, s2, ps->cr2);
        float x;
        if (fm) {
            float m = tsin(ph2 + fbk * v->fbm);
            v->fbm = m;
            x = tsin(ph1 + (idx + idxe * e2) * m * 0.159f);
            if (nz > 0.0f) x += nz * noise(&v->rng) * 0.5f;
        } else {
            x = osc(w1, ph1, dt1, idt1, pw, &v->rng);
            if (mix2 > 0.0f) x += mix2 * osc(w2, ph2, dt2, idt2, 0.5f, &v->rng);
            if (nz > 0.0f) x += nz * noise(&v->rng) * 0.5f;
            x *= mixn;
        }
        ph1 += dt1; if (ph1 >= 1.0f) ph1 -= 1.0f;
        ph2 += dt2; if (ph2 >= 1.0f) ph2 -= 1.0f;

        float bp, hp, y;
        switch (ftype) {
        case F_HP: svf_run(&fa, v->z, x, NULL, &hp); y = hp; break;
        case F_BP: svf_run(&fa, v->z, x, &bp, NULL); y = bp * 1.6f; break;
        case F_LP24: y = svf_run(&fb, v->z + 2, svf_run(&fa, v->z, x, NULL, NULL), NULL, NULL); break;
        default: y = svf_run(&fa, v->z, x, NULL, NULL); break;
        }
        out[i] += y * e1 * amp;
    }
    v->e1 = e1;
    v->e2 = e2;
    v->ph1 = ph1;
    v->ph2 = ph2;
    if (v->st1 == 3 && e1 < 0.0002f) v->on = 0;
    if (v->st1 == 0) v->on = 0;
}

/* ---- drums ---- */

static inline float dk(float sec)            /* per-sample factor of an exp decay */
{
    if (sec < 0.0003f) sec = 0.0003f;
    return expf(-1.0f / (sec * ES_SR));
}

/* the six squares of the metal: the 808's frequencies */
static const float METAL[6] = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f };

void es_drum_choke(es_engine_t *e, int kind)
{
    for (int t = 0; t < ES_DRUMS; t++)
        for (int k = 0; k < 2; k++)
            if (e->dv[t][k].on && e->dv[t][k].kind == kind && e->dv[t][k].fade >= 1.0f)
                e->dv[t][k].fade = 0.999f;
}

void es_drum_hit(es_engine_t *e, int track, float vel)
{
    const es_dtrack_t *dt = &e->p->dt[track];
    int slot = e->dv_next[track];
    e->dv_next[track] ^= 1;
    es_dvoice_t *old = &e->dv[track][slot ^ 1];
    if (old->on && old->fade >= 1.0f) old->fade = 0.999f;     /* the last hit gets out of the way */
    es_dvoice_t *d = &e->dv[track][slot];
    memset(d, 0, sizeof *d);
    d->on = 1;
    d->track = (uint8_t)track;
    d->vel = vel;
    d->fade = 1.0f;
    d->rng = e->rng ^ (0x9E3779B9u * (uint32_t)(track + 1));
    d->r = exp2f(dt->tune * (1.0f / 12.0f));
    d->m = es_clampf(dt->decay, 0.1f, 2.0f);
    d->kind = dt->src;
    if (d->kind >= DS_COUNT) {
        d->smp = es_bank_find(dt->sample);
        if (!d->smp || !d->smp->len) { d->on = 0; return; }
        d->sinc = (float)d->smp->rate * INV_SR * d->r;
        d->send = (float)(d->smp->len - 1);
        if (d->m < 1.0f) d->send *= d->m;
        e->dhits[track]++;
        return;
    }
    float r = d->r, m = d->m;
    d->env = d->env2 = d->env3 = d->penv = 1.0f;
    switch (d->kind) {
    case DS_KICK:  d->ka = dk(0.12f * m); d->kp = dk(0.035f); d->kb = dk(0.003f); break;
    case DS_BOOM:  d->ka = dk(0.42f * m); d->kp = dk(0.05f); break;
    case DS_SNARE: d->ka = dk(0.045f * m); d->kb = dk(0.09f * m); d->kp = dk(0.02f); break;
    case DS_CLAP:  d->kb = dk(0.08f * m); break;
    case DS_HATC:  d->ka = dk(0.025f * m); break;
    case DS_HATO:  d->ka = dk(0.16f * m); break;
    case DS_CYM:   d->ka = dk(0.55f * m); break;
    case DS_TOML: case DS_TOMH: d->ka = dk(0.15f * m); d->kp = dk(0.06f); d->kb = dk(0.02f); break;
    case DS_BELL:  d->ka = dk(0.015f); d->kb = dk(0.11f * m); break;
    case DS_CLAVE: d->ka = dk(0.025f * m); break;
    case DS_RIM:   d->ka = dk(0.012f * m); d->kb = dk(0.02f * m); d->kc = dk(0.003f); break;
    case DS_SHAKER: d->ka = dk(0.07f * m); break;
    }
    (void)r;
    /* the metal's squares from scattered phases: all six starting high
     * at once is a click through the high pass */
    for (int k = 0; k < 6; k++) d->ph[k] = (float)(es_rand(&d->rng) >> 8) * (1.0f / 16777216.0f);
    if (d->kind == DS_HATC) es_drum_choke(e, DS_HATO);
    e->dhits[track]++;
}

static float metal(es_dvoice_t *d, float scale)
{
    float s = 0.0f;
    for (int k = 0; k < 6; k++) {
        d->ph[k] += METAL[k] * scale * INV_SR;
        if (d->ph[k] >= 1.0f) d->ph[k] -= 1.0f;
        s += d->ph[k] < 0.5f ? 1.0f : -1.0f;
    }
    return s * (1.0f / 6.0f);
}

static void drum_block(es_engine_t *e, es_dvoice_t *d, float *out, float level)
{
    float g = d->vel * level;
    float r = d->r;
    svf_t f;
    int n = ES_BLK;
    float y[ES_BLK];

    if (d->smp) {
        const int16_t *s = d->smp->data;
        for (int i = 0; i < n; i++) {
            if (d->spos >= d->send) { d->on = 0; n = i; break; }
            int   k = (int)d->spos;
            float fr = d->spos - (float)k;
            float a = s[k], b = s[k + 1];
            float v = (a + (b - a) * fr) * (1.0f / 32768.0f);
            /* the last 5 ms of a cut sample fade out */
            float left = (d->send - d->spos) / d->sinc;
            if (left < 240.0f) v *= left * (1.0f / 240.0f);
            y[i] = v;
            d->spos += d->sinc;
        }
    } else switch (d->kind) {
    case DS_KICK: case DS_BOOM: case DS_TOML: case DS_TOMH: {
        float f0, f1, click;
        if (d->kind == DS_KICK)      { f0 = 160.0f; f1 = 48.0f; click = 0.25f; }
        else if (d->kind == DS_BOOM) { f0 = 110.0f; f1 = 45.0f; click = 0.0f; }
        else if (d->kind == DS_TOML) { f0 = 150.0f; f1 = 92.0f; click = 0.08f; }
        else                         { f0 = 240.0f; f1 = 165.0f; click = 0.08f; }
        f0 *= r; f1 *= r;
        float drive = d->kind == DS_BOOM ? 2.2f : 1.4f;
        for (int i = 0; i < n; i++) {
            float fq = f1 + (f0 - f1) * d->penv;
            d->penv *= d->kp;
            d->ph[0] += fq * INV_SR;
            if (d->ph[0] >= 1.0f) d->ph[0] -= 1.0f;
            float v = tsin(d->ph[0]) * d->env;
            d->env *= d->ka;
            if (click > 0.0f) { v += noise(&d->rng) * d->env2 * click; d->env2 *= d->kb; }
            y[i] = sat(v * drive) * 0.85f;
        }
        break;
    }
    case DS_SNARE:
        svf_set(&f, 3200.0f * r, 0.9f);
        for (int i = 0; i < n; i++) {
            float pf = 1.0f + 0.4f * d->penv;
            d->penv *= d->kp;
            d->ph[0] += 185.0f * r * pf * INV_SR; if (d->ph[0] >= 1.0f) d->ph[0] -= 1.0f;
            d->ph[1] += 330.0f * r * pf * INV_SR; if (d->ph[1] >= 1.0f) d->ph[1] -= 1.0f;
            float tone = (0.6f * tsin(d->ph[0]) + 0.4f * tsin(d->ph[1])) * d->env;
            float bp, hp;
            svf_run(&f, d->z, noise(&d->rng), &bp, &hp);
            float nz = (bp * 1.2f + hp * 0.35f) * d->env2;
            d->env *= d->ka;
            d->env2 *= d->kb;
            y[i] = tone * 0.55f + nz * 0.8f;
        }
        break;
    case DS_CLAP:
        svf_set(&f, 1150.0f * r, 0.7f);
        for (int i = 0; i < n; i++) {
            float t = (float)(d->t + (uint32_t)i) * INV_SR, en;
            if (t < 0.03f) {
                float tb = fmodf(t, 0.01f);
                en = expf(-tb * 250.0f);
            } else {
                en = d->env2;
                d->env2 *= d->kb;
            }
            float bp;
            svf_run(&f, d->z, noise(&d->rng), &bp, NULL);
            y[i] = bp * en * 1.7f;
        }
        d->env = d->env2;
        break;
    case DS_HATC: case DS_HATO: case DS_CYM: {
        float hpf = d->kind == DS_CYM ? 4500.0f : 7000.0f;
        float nzm = d->kind == DS_CYM ? 0.5f : 0.3f;
        float sc = d->kind == DS_CYM ? 1.0f * r : 1.7f * r;
        svf_set(&f, hpf * (0.7f + 0.3f * r), 1.2f);
        for (int i = 0; i < n; i++) {
            float x = metal(d, sc) + nzm * noise(&d->rng);
            float hp;
            svf_run(&f, d->z, x, NULL, &hp);
            y[i] = hp * d->env * 0.9f;
            d->env *= d->ka;
        }
        break;
    }
    case DS_BELL:
        svf_set(&f, 1000.0f * r, 1.0f);
        for (int i = 0; i < n; i++) {
            d->ph[0] += 540.0f * r * INV_SR; if (d->ph[0] >= 1.0f) d->ph[0] -= 1.0f;
            d->ph[1] += 800.0f * r * INV_SR; if (d->ph[1] >= 1.0f) d->ph[1] -= 1.0f;
            float x = (d->ph[0] < 0.5f ? 0.5f : -0.5f) + (d->ph[1] < 0.5f ? 0.5f : -0.5f);
            float bp;
            svf_run(&f, d->z, x, &bp, NULL);
            float en = 0.6f * d->env + 0.4f * d->env2;
            d->env *= d->ka;
            d->env2 *= d->kb;
            y[i] = bp * en * 0.8f;
        }
        d->env = d->env2;   /* the long one says when it is over */
        break;
    case DS_CLAVE:
        for (int i = 0; i < n; i++) {
            d->ph[0] += 2500.0f * r * INV_SR; if (d->ph[0] >= 1.0f) d->ph[0] -= 1.0f;
            y[i] = tsin(d->ph[0]) * d->env * 0.8f;
            d->env *= d->ka;
        }
        break;
    case DS_RIM:
        svf_set(&f, 900.0f, 1.4f);
        for (int i = 0; i < n; i++) {
            d->ph[0] += 1700.0f * r * INV_SR; if (d->ph[0] >= 1.0f) d->ph[0] -= 1.0f;
            d->ph[1] += 480.0f * r * INV_SR;  if (d->ph[1] >= 1.0f) d->ph[1] -= 1.0f;
            float x = tsin(d->ph[0]) * d->env + 0.5f * tsin(d->ph[1]) * d->env2
                    + noise(&d->rng) * d->env3 * 0.6f;
            float hp;
            svf_run(&f, d->z, x, NULL, &hp);
            y[i] = hp * 0.9f;
            d->env *= d->ka; d->env2 *= d->kb; d->env3 *= d->kc;
        }
        d->env = d->env2 > d->env ? d->env2 : d->env;
        break;
    case DS_SHAKER:
        svf_set(&f, 5000.0f * r, 1.3f);
        for (int i = 0; i < n; i++) {
            float t = (float)(d->t + (uint32_t)i) * INV_SR, en;
            if (t < 0.012f) en = t * (1.0f / 0.012f);
            else { en = d->env; d->env *= d->ka; }
            float hp;
            svf_run(&f, d->z, noise(&d->rng), NULL, &hp);
            y[i] = hp * en * 0.7f;
        }
        break;
    default:
        d->on = 0;
        return;
    }

    /* the declick when the next hit or a choke takes over */
    for (int i = 0; i < n; i++) {
        float v = y[i] * g;
        if (d->fade < 1.0f) {
            v *= d->fade;
            d->fade -= 1.0f / 96.0f;
            if (d->fade <= 0.0f) { d->on = 0; n = i; break; }
        }
        out[i] += v;
    }
    d->t += ES_BLK;
    if (!d->smp && d->t > ES_SR / 50 && d->env < 0.0003f) d->on = 0;
}

/* ---- the metronome ---- */

void es_click(es_engine_t *e, bool accent)
{
    e->met_f = accent ? 2400.0f : 1600.0f;
    e->met_ph = 0.0f;
    e->met_env = accent ? 1.0f : 0.7f;
}

/* ---- the mix ---- */

void es_dsp_block(es_engine_t *e, float *out)
{
    es_proj_t *p = e->p;
    memset(e->chbuf, 0, sizeof e->chbuf);

    for (int i = 0; i < ES_PARTS; i++) part_block(e, i);

    int nv = 0;
    for (int i = 0; i < ES_VOICES; i++) {
        es_voice_t *v = &e->v[i];
        if (!v->on) continue;
        nv++;
        if (v->off_in >= 0) {
            v->off_in -= ES_BLK;
            if (v->off_in < 0 && v->gate) v->gate = 0;
        }
        voice_block(e, v, e->chbuf[v->part]);
    }
    e->stats.voices = nv;

    for (int t = 0; t < ES_DRUMS; t++) {
        float lv = p->dt[t].level;
        for (int k = 0; k < 2; k++)
            if (e->dv[t][k].on) drum_block(e, &e->dv[t][k], e->chbuf[3], lv);
    }

    /* the channels: drive, volume, sends */
    float dsend[ES_BLK], rsend[ES_BLK], dry[ES_BLK];
    memset(dsend, 0, sizeof dsend);
    memset(rsend, 0, sizeof rsend);
    memset(dry, 0, sizeof dry);
    for (int c = 0; c < ES_CH; c++) {
        const es_chan_t *ch = &p->ch[c];
        if (ch->mute) continue;
        float vol = ch->vol * ch->vol * (c == 3 ? 0.85f : 0.5f);
        float drv = ch->drive;
        float pre = 1.0f + drv * 9.0f, post = 1.0f / (1.0f + drv * 2.5f);
        for (int i = 0; i < ES_BLK; i++) {
            float x = e->chbuf[c][i];
            if (drv > 0.001f) x = sat(x * pre) * post;
            x *= vol;
            dry[i] += x;
            dsend[i] += x * ch->dly;
            rsend[i] += x * ch->rev;
        }
    }

    /* the delay, in time with the tempo; the time glides when the tempo
     * moves, so it bends instead of clicking */
    const es_fx_t *fx = &p->fx;
    float target = es_dly_beats(fx->dly_div) * 60.0f / p->bpm * ES_SR;
    if (target > ES_DLY_MAX - 4) target = ES_DLY_MAX - 4;
    if (e->dly_t <= 0.0f) e->dly_t = target;
    e->dly_t += (target - e->dly_t) * 0.02f;
    float fb = fx->dly_fb * 0.92f, tone = 0.15f + 0.8f * fx->dly_tone;
    float dret = fx->dly_ret;

    /* the reverb */
    float rfb = 0.70f + 0.28f * fx->rev_size, damp = 0.05f + 0.6f * fx->rev_damp;
    float rret = fx->rev_ret;

    /* the master filter: left of the centre a low pass, right a high pass */
    float mf = fx->mfilt;
    bool mon = fabsf(mf) > 0.02f;
    svf_t ms;
    if (mon) {
        float fc = mf < 0.0f ? 20000.0f * exp2f(mf * 9.0f) : 20.0f * exp2f(mf * 9.5f);
        svf_set(&ms, fc, 2.0f - 1.9f * fx->mres);
    }
    float mg = fx->master * fx->master * 1.3f;
    float met = p->met_vol * 0.5f;
    float peak = e->st_peak;
    float *dl = e->dly;

    for (int i = 0; i < ES_BLK; i++) {
        float m = dry[i];
        if (dl) {
            float rp = (float)e->dly_w - e->dly_t;
            while (rp < 0.0f) rp += ES_DLY_MAX;
            int   k = (int)rp;
            float fr = rp - (float)k;
            int   k2 = k + 1 >= ES_DLY_MAX ? 0 : k + 1;
            float d = dl[k] + (dl[k2] - dl[k]) * fr;
            e->dly_lp += (d - e->dly_lp) * tone;
            dl[e->dly_w] = dsend[i] + sat(e->dly_lp * fb);
            if (++e->dly_w >= ES_DLY_MAX) e->dly_w = 0;
            m += d * dret;
        }
        if (e->rv) {
            float in = rsend[i] * 0.25f, acc = 0.0f;
            for (int c = 0; c < 4; c++) {
                float *b = e->cb[c];
                float yv = b[e->ci[c]];
                e->clp[c] = yv * (1.0f - damp) + e->clp[c] * damp;
                b[e->ci[c]] = in + e->clp[c] * rfb;
                if (++e->ci[c] >= e->cn[c]) e->ci[c] = 0;
                acc += yv;
            }
            for (int a = 0; a < 2; a++) {
                float *b = e->ab[a];
                float bo = b[e->ai[a]];
                b[e->ai[a]] = acc + bo * 0.5f;
                acc = bo - acc;
                if (++e->ai[a] >= e->an[a]) e->ai[a] = 0;
            }
            m += acc * rret;
        }
        if (mon) {
            float hp;
            float lp = svf_run(&ms, e->mz, m, NULL, &hp);
            m = mf < 0.0f ? lp : hp;
        }
        m *= mg;
        if (e->met_env > 0.0005f) {
            e->met_ph += e->met_f * INV_SR;
            if (e->met_ph >= 1.0f) e->met_ph -= 1.0f;
            m += tsin(e->met_ph) * e->met_env * met;
            e->met_env *= 0.9985f;
        }
        /* the limiter: instant down, 80 ms back up, then a hard stop */
        float a = fabsf(m);
        if (a * e->lim_g > 0.95f) e->lim_g = 0.95f / a;
        else e->lim_g += (1.0f - e->lim_g) * 0.0003f;
        m *= e->lim_g;
        if (m > 1.0f) m = 1.0f;
        if (m < -1.0f) m = -1.0f;
        if (a > peak) peak = a;
        out[i] = m;
    }
    e->st_peak = peak;
}
