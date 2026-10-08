/*
 * RF - the LoRa meter (rf_lora.h).
 *
 * Finding the packets. Every 2 ms, a 256-point FFT of the band (rf_dsp.c's,
 * the board's SIMD one). Each bin has its own floor, the mean of its noise
 * in dB, followed slowly (and very slowly where something stands over it,
 * so a carrier that stays on becomes floor in some seconds). A bin 12 dB
 * over its floor is on; neighbouring bins on make a segment. A chirp is
 * narrow in 0.27 ms - a frame sees a piece of its sweep - so a packet is
 * the segments of the frames in a row that fall near each other: within
 * 520 kHz (LoRa's widest bandwidth, plus a margin), since a chirp jumps
 * from the top of its band to the bottom at each symbol. It ends after
 * 12 ms with nothing. Two packets at once less than 520 kHz apart are taken
 * as one; at 960 ksps that is the whole view.
 *
 * What a packet is. Over the frames, each bin's power above its floor is
 * summed: a chirp visits every frequency of its band alike, so the band is
 * where that sum stands up - its centre and its width. Then the preamble
 * (8 to 16 up-chirps of the same symbol, LoRa's own) is dechirped against
 * every LoRa bandwidth near that width and every spreading factor 5..12:
 * multiplied by the conjugate of an up-chirp of that slope, a preamble
 * symbol becomes a single tone, and an FFT gathers it in a few bins. The
 * right pair gathers most of the energy; a wrong slope leaves a chirp,
 * spread over the band. The width matters: SF9 at 250 kHz and SF7 at
 * 125 kHz have the same slope (BW^2 / 2^SF).
 *
 * The dechirp takes some tens of ms of float work per packet, so it runs
 * from rf_lora_analyse_pending, on another thread: the feed copies 80 ms of
 * the burst's start into a job and goes on; one job at a time.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")      /* the .so is built -Os */
#endif

#include "rf_lora.h"
#include "rf.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DET_N       256
#define BURSTS      4
#define THRESH     8.9f        /* 9.5 dB over a bin's mean noise: on */
#define JOBS        4
#define MIN_WIDTH   50000       /* narrower is not looked at: LoRa is 125 to 500 kHz but for rare uses */
#define JOIN_HZ     520000
#define HANG_MS     12
#define MIN_FRAMES  4
#define MAX_MS      12000       /* longer: a carrier, not a packet */
#define WIN_MS      80          /* of the start, for the dechirp */
#define RING_MS     200
#define FFT_MAX     16384

typedef struct {
    bool on, ended, job_given, analysed;
    uint32_t uid;               /* its number, for the job that comes back */
    uint64_t s0, s_last;
    int lo, hi;                 /* the segments' envelope, in bins */
    int frames;
    float prof[DET_N];          /* sum over its frames of each bin's power over its floor */
    int sf;
    uint32_t bw;
    float q;
} burst_t;

typedef struct {
    float re, im;
} cpx;

/* a dechirp to do: the start of a burst, copied out of the ring */
typedef struct {
    volatile int state;         /* 0 free, 1 filled (for the analyser), 2 done (for the feed) */
    uint32_t uid;               /* whose */
    uint8_t *iq;
    int n;
    float fc, width, max_t;     /* its centre and width; the longest symbol its duration allows */
    int sf;
    uint32_t bw;
    float q;
} job_t;

struct rf_lora {
    uint32_t rate, hop;
    float bin_hz;
    rf_fft_t *fft;
    uint8_t frame[DET_N * 2];
    int fill;
    float pw[DET_N], floor[DET_N];  /* this frame's power, each bin's mean noise (linear) */
    bool act[DET_N];
    uint32_t nframes;
    uint64_t pos;               /* samples the frames have reached */
    uint64_t ring_pos;          /* samples in the ring so far (a read ahead of pos) */
    uint8_t *ring;              /* the last RING_MS of cu8 */
    uint32_t ring_n, ring_at;   /* its size, and where the next sample goes */
    uint32_t ph;                /* where pos is in its 2 ms hop */
    burst_t b[BURSTS];
    uint32_t next_uid;
    /* the dechirps to do: a queue, so a packet that comes while another is
     * looked at waits its turn instead of rolling out of the ring */
    job_t job[JOBS];
    int job_cap;
    uint64_t (*clock)(void);
    void (*yield)(void);
    rf_lora_stats_t st;
    cpx *xb, *fb;               /* the analyser's: decimated, and the FFT's */
    float *acc;                 /* the windows' power spectra, summed */
    int xb_cap;
};

/* to the nearest (lrintf is not among what the firmware lends the apps) */
static inline long rnd(float x)
{
    return (long)(x < 0 ? x - 0.5f : x + 0.5f);
}

static void *big(size_t n)
{
#if defined(ESP_PLATFORM)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
#else
    return malloc(n);
#endif
}

rf_lora_t *rf_lora_new(uint32_t rate)
{
    rf_lora_t *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->rate = rate;
    o->hop = rate / 500;
    if (o->hop < DET_N) o->hop = DET_N;
    o->bin_hz = (float)rate / DET_N;
    o->fft = rf_fft_new(DET_N);
    o->ring_n = rate / 1000 * RING_MS;
    o->ring = big((size_t)o->ring_n * 2);
    o->job_cap = (int)(rate / 1000 * WIN_MS);
    bool jobs_ok = true;
    for (int i = 0; i < JOBS; i++) jobs_ok &= (o->job[i].iq = big((size_t)o->job_cap * 2)) != NULL;
    o->xb_cap = o->job_cap;
    o->xb = big((size_t)o->xb_cap * sizeof(cpx));
    o->fb = big(FFT_MAX * sizeof(cpx));
    o->acc = big(FFT_MAX * sizeof(float));
    if (!o->fft || !o->ring || !jobs_ok || !o->xb || !o->fb || !o->acc) {
        rf_lora_free(o);
        return NULL;
    }
    return o;
}

void rf_lora_free(rf_lora_t *o)
{
    if (!o) return;
    rf_fft_free(o->fft);
    free(o->ring);
    for (int i = 0; i < JOBS; i++) free(o->job[i].iq);
    free(o->xb);
    free(o->fb);
    free(o->acc);
    free(o);
}

/* ---- the analysis --------------------------------------------------------- */

static void fft(cpx *x, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            cpx t = x[i];
            x[i] = x[j];
            x[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float a = -2.0f * (float)M_PI / len;
        cpx wl = { cosf(a), sinf(a) };
        for (int i = 0; i < n; i += len) {
            cpx w = { 1, 0 };
            for (int k = 0; k < len / 2; k++) {
                cpx u = x[i + k], v = x[i + k + len / 2];
                cpx t = { v.re * w.re - v.im * w.im, v.re * w.im + v.im * w.re };
                x[i + k] = (cpx){ u.re + t.re, u.im + t.im };
                x[i + k + len / 2] = (cpx){ u.re - t.re, u.im - t.im };
                float wr = w.re * wl.re - w.im * wl.im;
                w.im = w.re * wl.im + w.im * wl.re;
                w.re = wr;
            }
        }
    }
}

/* How much of the preamble's energy a dechirp at (bw, sf) gathers into one
 * tone: up to 4 symbols' windows from 'from', their power spectra summed,
 * then the 5 strongest neighbouring bins over all of it. A preamble repeats
 * one symbol, so with the right slope and length the tone stays in its bin
 * from window to window; a slope's twin at another bandwidth (SF9 at 250 k,
 * SF7 at 125 k), or something that is not LoRa, moves it, and the sum
 * spreads. A window across two symbols splits its tone in two; the caller
 * tries two starts half a symbol apart, so one has three quarters in one.
 * (Adding the second tone, BW away, was tried: it is exactly where the
 * twin's tone jumps to, and the twins won.) */
static float dechirp(rf_lora_t *o, const cpx *x, int m, float fsd, uint32_t bw, int sf, float from, float *gathered, bool plain)
{
    double T = (double)(1u << sf) / bw;
    double Lx = T * fsd;                          /* samples a symbol, not a whole number */
    int L = (int)Lx;
    x += (int)(from * Lx);
    m -= (int)(from * Lx);
    int wins = (int)(m / Lx);
    if (wins > 4) wins = 4;
    *gathered = 0;
    if (L < 32 || wins < 2) return 0;
    int N = 1;
    while (N < L) N <<= 1;
    if (N > FFT_MAX) return 0;
    float k = (float)(bw / T);                    /* Hz per second */
    float a0 = (float)(2 * M_PI * (-(double)bw / 2) / fsd), da = (float)(2 * M_PI * k / ((double)fsd * fsd));
    if (plain) a0 = da = 0;         /* no chirp: what the window gathers as it is */
    float *acc = o->acc;
    memset(acc, 0, N * sizeof *acc);
    cpx *y = o->fb;
    for (int wi = 0; wi < wins; wi++) {
        const cpx *xw = x + (int)(wi * Lx + 0.5);
        cpx r = { 1, 0 };
        cpx w = { cosf(a0), sinf(a0) }, v = { cosf(da), sinf(da) };
        for (int n = 0; n < L; n++) {
            /* x times the conjugate of the reference up-chirp */
            y[n] = (cpx){ xw[n].re * r.re + xw[n].im * r.im, xw[n].im * r.re - xw[n].re * r.im };
            cpx t = { r.re * w.re - r.im * w.im, r.re * w.im + r.im * w.re };
            r = t;
            cpx u = { w.re * v.re - w.im * v.im, w.re * v.im + w.im * v.re };
            w = u;
            if ((n & 1023) == 1023) {
                float g = 1.0f / sqrtf(r.re * r.re + r.im * r.im);
                r.re *= g, r.im *= g;
                g = 1.0f / sqrtf(w.re * w.re + w.im * w.im);
                w.re *= g, w.im *= g;
            }
        }
        for (int n = L; n < N; n++) y[n] = (cpx){ 0, 0 };
        fft(y, N);
        for (int i = 0; i < N; i++) acc[i] += y[i].re * y[i].re + y[i].im * y[i].im;
        if (o->yield) o->yield();       /* the engine, at the same priority, must keep reading */
    }
    float total = 0;
    for (int i = 0; i < N; i++) total += acc[i];
    if (total <= 0) return 0;
#define P5(c) (acc[((c) - 2 + N) % N] + acc[((c) - 1 + N) % N] + acc[(c) % N] + acc[((c) + 1) % N] + acc[((c) + 2) % N])
    int best = 0;
    float e1 = 0;
    for (int i = 0; i < N; i++) {
        float e = P5(i);
        if (e > e1) e1 = e, best = i;
    }
    (void)best;
#undef P5
    /* the tone's own power, A^2 for a tone of amplitude A (it puts N L A^2
     * in its bins): the same yardstick for every try, the mean of D samples
     * keeping a tone in the band at its amplitude */
    *gathered = e1 / ((float)wins * N * L);
    return e1 / total;
}

/* The bandwidths and spreading factors tried: LoRa's from 62.5 kHz and SF7
 * up, what Meshtastic, LoRaWAN and most sensors use. The narrower ones and
 * SF5/6 exist but are rare, and on the board's busy 915 MHz band they made
 * a hopping neighbour's narrow bursts pass for LoRa (2026-10-08). */
static const uint32_t LORA_BW[] = { 62500, 125000, 250000, 500000 };
#define SF_MIN 7

bool rf_lora_analyse_pending(rf_lora_t *o)
{
    if (!o) return false;
    job_t *j = NULL;
    for (int i = 0; i < JOBS; i++)       /* the oldest first */
        if (o->job[i].state == 1 && (!j || o->job[i].uid < j->uid)) j = &o->job[i];
    if (!j) return false;
    uint64_t t0 = o->clock ? o->clock() : 0;
    /* Each bandwidth near the width that showed - a weak packet shows
     * narrower than it is, so up to 3.5 times it - gives its best spreading
     * factor, which must stand clearly over that bandwidth's other ones.
     * Among those, the one that gathered most of the window's power wins: a
     * slope's twin at half the bandwidth (SF9 at 250 k, SF7 at 125 k)
     * gathers its tone too - at a rate equal to its bandwidth, its two
     * halves even land in one bin - but only from the half of the band its
     * narrower filter lets through. */
    int best_sf = 0;
    uint32_t best_bw = 0;
    float best_q = 0, best_g = 0;
    for (size_t bi = 0; bi < sizeof LORA_BW / sizeof LORA_BW[0]; bi++) {
        uint32_t bw = LORA_BW[bi];
        if (j->width < 0.28f * bw || j->width > 2.6f * bw || bw > o->rate * 0.95f) continue;
        int bw_sf = 0;
        float bw_q = 0, bw_second = 0, bw_g = 0;
        /* to its centre, and down to a rate a little over its bandwidth:
         * the mean of D samples (a sinc, -2.3 dB at the band's edge for D = 3) */
        int D = (int)(o->rate / bw);
        if (D < 1) D = 1;
        float fsd = (float)o->rate / D;
        int m = j->n / D;
        if (m > o->xb_cap) m = o->xb_cap;
        float a = (float)(-2 * M_PI * j->fc / o->rate);
        cpx w = { cosf(a), sinf(a) }, r = { 1, 0 };
        const uint8_t *q = j->iq;
        for (int i = 0; i < m; i++) {
            float sr = 0, si = 0;
            for (int j = 0; j < D; j++, q += 2) {
                float xr = q[0] - 127.5f, xi = q[1] - 127.5f;
                sr += xr * r.re - xi * r.im;
                si += xr * r.im + xi * r.re;
                cpx t = { r.re * w.re - r.im * w.im, r.re * w.im + r.im * w.re };
                r = t;
            }
            o->xb[i] = (cpx){ sr / D, si / D };
            if ((i & 255) == 255) {
                float g = 1.0f / sqrtf(r.re * r.re + r.im * r.im);
                r.re *= g, r.im *= g;
            }
        }
        for (int sf = SF_MIN; sf <= 12; sf++) {
            if ((float)(1u << sf) / bw > j->max_t) break;     /* longer symbols than the packet allows */
            float gv, gh;
            float qv = dechirp(o, o->xb, m, fsd, bw, sf, 0, &gv, false), qh = dechirp(o, o->xb, m, fsd, bw, sf, 0.5f, &gh, false);
            if (qh > qv) qv = qh, gv = gh;
            if (qv > bw_q) bw_second = bw_q, bw_q = qv, bw_sf = sf, bw_g = gv;
            else if (qv > bw_second) bw_second = qv;
        }
        /* a chirp: most of a symbol in a tone, clearly more than the other
         * slopes; and of the bandwidths that pass, the one that gathered
         * the most of what came in (a twin's filter lets half of it by).
         * And the dechirp must gather much more than the window does
         * without it: a remote's keyed carrier is a tone already. */
        float plain_g;
        float plain = bw_sf ? dechirp(o, o->xb, m, fsd, bw, bw_sf, 0, &plain_g, true) : 1;
        if (bw_q >= 0.3f && bw_q >= 1.6f * bw_second && bw_q >= 1.5f * plain && bw_g > best_g)
            best_g = bw_g, best_q = bw_q, best_sf = bw_sf, best_bw = bw;
    }
    bool ok = best_sf != 0;
    j->sf = ok ? best_sf : 0;
    j->bw = ok ? best_bw : 0;
    j->q = best_q;          /* 0 when nothing passed */
    if (o->clock) {
        uint32_t us = (uint32_t)(o->clock() - t0);
        o->st.analysed++;
        o->st.analyse_us += us;
        if (us > o->st.analyse_max_us) o->st.analyse_max_us = us;
    }
    __sync_synchronize();
    j->state = 2;
    return true;
}

/* ---- finding them ----------------------------------------------------------- */

/* The burst's band from its summed profile: the widest run of bins over a
 * line, gaps of 2 allowed. The line is 3 dB over the floor, or 10 dB under
 * the packet's own plateau if that is higher: a node a
 * metre away stood 30 dB up, and its skirts - a chirp's jumps are not band-
 * limited, the stick's range neither - stood 3 dB over the noise across
 * the whole view (the board, 2026-10-08). The widest run, not the one
 * around the strongest bin: a narrow carrier near a packet stands higher
 * per bin than a chirp spread over 250 kHz, and dragged the band along. */
static void band_of(const rf_lora_t *o, const burst_t *b, int *lo, int *hi, float *snr_db)
{
    float f = b->frames ? (float)b->frames : 1;
    /* the plateau: the highest mean over 7 neighbouring bins - a packet's
     * band is 13 bins and more at any rate here, a carrier's 1 to 3 */
    float plateau = 0, run = 0;
    for (int k = 0; k < DET_N; k++) {
        run += b->prof[k] / f - (k >= 7 ? b->prof[k - 7] / f : 0);
        if (k >= 6 && run / 7 > plateau) plateau = run / 7;
    }
    float line = plateau / 10;
    if (line < 2.0f) line = 2.0f;
    int best_l = b->lo, best_h = b->lo;
    for (int k = 0; k < DET_N;) {
        if (b->prof[k] / f < line) {
            k++;
            continue;
        }
        int l = k, h = k, gap = 0;
        for (k++; k < DET_N && gap <= 2; k++) {
            if (b->prof[k] / f >= line) h = k, gap = 0;
            else gap++;
        }
        if (h - l > best_h - best_l) best_l = l, best_h = h;
    }
    *lo = best_l;
    *hi = best_h;
    float s = 0;
    for (int k = best_l; k <= best_h; k++) s += b->prof[k] / f;
    s = s / (best_h - best_l + 1) - 1;
    *snr_db = s > 0 ? 10 * log10f(s) : 0;
    (void)o;
}

static void report(rf_lora_t *o, burst_t *b, void (*done)(const rf_lora_pkt_t *, void *), void *ctx)
{
    int lo, hi;
    float snr;
    band_of(o, b, &lo, &hi, &snr);
    /* narrower than any LoRa worth the name: some other neighbour of the
     * band, which would bury the packets in the list */
    if (b->frames >= MIN_FRAMES && done && (hi - lo) * o->bin_hz >= MIN_WIDTH) {
        rf_lora_pkt_t p = {
            .start = b->s0,
            .dur_us = (uint32_t)((b->s_last + DET_N - b->s0) * 1000000ull / o->rate),
            .offset_hz = (int32_t)rnd(((lo + hi) / 2.0f - DET_N / 2) * o->bin_hz),
            .width_hz = (uint32_t)rnd((hi - lo) * o->bin_hz),
            .snr_db = snr,
            .sf = b->sf,
            .bw_hz = b->bw,
            .quality = b->q,
        };
        done(&p, ctx);
    }
    memset(b, 0, sizeof *b);
}

/* a job: the window of the burst's start, still in the ring */
static bool give_job(rf_lora_t *o, job_t *j, burst_t *b)
{
    uint64_t from = b->s0 + o->rate / 1000;                /* 1 ms in */
    uint64_t to = b->s0 + o->rate / 1000 * (1 + WIN_MS);
    if (to > o->pos) to = o->pos;
    if (to <= from || o->ring_pos - from > o->ring_n) return false;
    int n = (int)(to - from);
    if (n > o->job_cap) n = o->job_cap;
    uint8_t *job_iq = j->iq;
    /* where 'from' sits: as far back from the ring's head as it is in samples */
    uint32_t back = (uint32_t)(o->ring_pos - from);
    uint32_t at = o->ring_at >= back ? o->ring_at - back : o->ring_at + o->ring_n - back;
    int first = (int)(o->ring_n - at) < n ? (int)(o->ring_n - at) : n;
    memcpy(job_iq, o->ring + 2 * at, (size_t)first * 2);
    if (first < n) memcpy(job_iq + 2 * first, o->ring, (size_t)(n - first) * 2);
    int lo, hi;
    float snr;
    band_of(o, b, &lo, &hi, &snr);
    j->n = n;
    j->fc = ((lo + hi) / 2.0f - DET_N / 2) * o->bin_hz;
    j->width = (hi - lo) * o->bin_hz;
    /* a preamble of 6 symbols at least, then something: a symbol is a
     * sixth of the packet at most (unknown while it goes on) */
    j->max_t = b->ended ? (float)(b->s_last + DET_N - b->s0) / o->rate / 6 : 1e9f;
    j->uid = b->uid;
    __sync_synchronize();
    j->state = 1;
    return true;
}

static void frame_done(rf_lora_t *o, uint64_t at, void (*done)(const rf_lora_pkt_t *, void *), void *ctx)
{
    rf_fft_add_cu8(o->fft, o->frame);
    rf_fft_take_pow(o->fft, o->pw);
    o->nframes++;
    /* the floors (each bin's mean noise power), and what stands over them */
    float a = o->nframes < 50 ? 0.1f : 0.004f;
    bool *act = o->act;
    for (int k = 0; k < DET_N; k++) {
        if (o->nframes == 1) o->floor[k] = o->pw[k];
        act[k] = o->nframes > 50 && o->pw[k] > o->floor[k] * THRESH;
        o->floor[k] += (o->pw[k] - o->floor[k]) * (act[k] ? a / 50 : a);
    }
    /* segments: bins on, gaps of one allowed, two bins at least */
    int join = (int)(JOIN_HZ / o->bin_hz);
    bool joined[BURSTS] = { 0 };
    for (int k = 0; k < DET_N;) {
        if (!act[k]) {
            k++;
            continue;
        }
        int lo = k, hi = k;
        while (k < DET_N && (act[k] || (k + 1 < DET_N && act[k + 1]))) {
            if (act[k]) hi = k;
            k++;
        }
        if (hi - lo < 1) continue;
        int bi = -1;
        for (int i = 0; i < BURSTS && bi < 0; i++)
            if (o->b[i].on && lo <= o->b[i].hi + join && hi >= o->b[i].lo - join) bi = i;
        if (bi < 0)
            for (int i = 0; i < BURSTS && bi < 0; i++)
                if (!o->b[i].on && !o->b[i].ended) {
                    bi = i;
                    memset(&o->b[i], 0, sizeof o->b[i]);
                    o->b[i].on = true;
                    o->b[i].uid = ++o->next_uid;
                    o->b[i].s0 = at;
                    o->b[i].lo = lo;
                    o->b[i].hi = hi;
                }
        if (bi < 0) continue;
        burst_t *b = &o->b[bi];
        if (lo < b->lo) b->lo = lo;
        if (hi > b->hi) b->hi = hi;
        b->s_last = at;
        joined[bi] = true;
    }
    for (int i = 0; i < BURSTS; i++) {
        burst_t *b = &o->b[i];
        if (b->on && joined[i]) {
            b->frames++;
            for (int k = 0; k < DET_N; k++) b->prof[k] += o->pw[k] / o->floor[k];
        } else if (b->on && at - b->s_last > o->rate / 1000 * HANG_MS) {
            b->on = false;
            b->ended = true;
        }
        if (b->on && at - b->s0 > o->rate / 1000 * MAX_MS) memset(b, 0, sizeof *b);   /* a carrier */
    }
    /* the dechirps: the jobs back to their bursts (if still there), new ones out */
    for (int ji = 0; ji < JOBS; ji++) {
        job_t *j = &o->job[ji];
        if (j->state != 2) continue;
        for (int i = 0; i < BURSTS; i++)
            if (o->b[i].uid == j->uid && (o->b[i].on || o->b[i].ended)) {
                o->b[i].sf = j->sf;
                o->b[i].bw = j->bw;
                o->b[i].q = j->q;
                o->b[i].analysed = true;
            }
        j->state = 0;
    }
    for (int i = 0; i < BURSTS; i++) {
        burst_t *b = &o->b[i];
        if (!(b->on || b->ended) || b->job_given || b->analysed) continue;
        bool ready = b->ended || at - b->s0 >= o->rate / 1000 * (WIN_MS + 1);
        if (!ready) continue;
        if (b->frames < MIN_FRAMES && b->ended) {
            b->analysed = true;                 /* noise: nothing to look at */
            continue;
        }
        int lo, hi;
        float snr;
        band_of(o, b, &lo, &hi, &snr);
        if ((hi - lo) * o->bin_hz < MIN_WIDTH) {
            b->analysed = true;                 /* too narrow to be worth the work */
            o->st.narrow++;
            continue;
        }
        job_t *j = NULL;
        for (int ji = 0; ji < JOBS && !j; ji++)
            if (o->job[ji].state == 0) j = &o->job[ji];
        if (j) {
            if (give_job(o, j, b)) b->job_given = true;
            else b->analysed = true, o->st.missed++;      /* gone from the ring: not looked at */
        } else if (o->ring_pos - b->s0 > o->ring_n) b->analysed = true, o->st.missed++;
    }
    for (int i = 0; i < BURSTS; i++)
        if (o->b[i].ended && o->b[i].analysed) report(o, &o->b[i], done, ctx);
}

void rf_lora_feed(rf_lora_t *o, const uint8_t *iq, int n, void (*done)(const rf_lora_pkt_t *, void *), void *ctx)
{
    /* Counters and block copies only: a 64-bit modulo per sample is a call
     * of some hundred cycles on the board's 32-bit core. The whole read goes
     * into the ring first, then the frames walk it. */
    for (int k = 0; k < n;) {
        int take = (int)(o->ring_n - o->ring_at);
        if (take > n - k) take = n - k;
        memcpy(o->ring + 2 * o->ring_at, iq + 2 * k, (size_t)take * 2);
        o->ring_at += take;
        if (o->ring_at == o->ring_n) o->ring_at = 0;
        k += take;
    }
    o->ring_pos += n;
    for (int k = 0; k < n;) {
        int take;
        if (o->ph < DET_N) {
            take = DET_N - (int)o->ph;
            if (take > n - k) take = n - k;
            memcpy(o->frame + 2 * o->ph, iq + 2 * k, (size_t)take * 2);
        } else {
            take = (int)(o->hop - o->ph);
            if (take > n - k) take = n - k;
        }
        o->ph += take;
        o->pos += take;
        k += take;
        if (o->ph == DET_N) frame_done(o, o->pos - DET_N, done, ctx);
        if (o->ph == o->hop) o->ph = 0;
    }
}

void rf_lora_set_clock(rf_lora_t *o, uint64_t (*us)(void), void (*yield)(void))
{
    o->clock = us;
    o->yield = yield;
}

void rf_lora_stats(rf_lora_t *o, rf_lora_stats_t *out)
{
    *out = o->st;
    memset(&o->st, 0, sizeof o->st);
}

const char *rf_lora_preset(int sf, uint32_t bw_hz)
{
    /* Meshtastic's modem presets (its firmware's RadioInterface) */
    static const struct {
        int sf;
        uint32_t bw;
        const char *name;
    } P[] = {
        { 7, 500000, "ShortTurbo" }, { 7, 250000, "ShortFast" },   { 8, 250000, "ShortSlow" },
        { 9, 250000, "MediumFast" }, { 10, 250000, "MediumSlow" }, { 11, 250000, "LongFast" },
        { 11, 125000, "LongModerate" }, { 12, 125000, "LongSlow" },
    };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (P[i].sf == sf && P[i].bw == bw_hz) return P[i].name;
    return NULL;
}
