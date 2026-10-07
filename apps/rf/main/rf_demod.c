/*
 * RF - demodulation (rf_demod.h): broadcast FM, AM and narrow FM from cu8
 * I/Q to 48 kHz audio.
 *
 * The chain, for a source at R = 480 kHz x D (D = 2 to 5) or 240 kHz:
 *
 *   cu8 -> 16-bit I and Q, without their DC
 *   A   R -> 480 kHz        low-pass, +-110 kHz kept, Blackman (skipped at 240 kHz)
 *   WFM: B   480 -> 240 kHz  half-band, +-100 kHz: the channel
 *        FM discriminator, de-emphasis, 19 kHz pilot meter
 *        audio 240 -> 48 kHz low-pass to 15 kHz
 *   AM, NFM: C   480 (240) -> 24 kHz   low-pass, +-8 kHz
 *        channel  24 kHz      +-4 kHz (AM) or +-6.5 kHz (NFM)
 *        AM: envelope over its own average (an AGC); NFM: discriminator
 *        audio low-pass to 3 kHz, out at 24 kHz (voice needs no more; at
 *        48 kHz their channel filter in C took ~25 % of a core)
 *   squelch on the channel's power over the noise floor
 *
 * The filters are 16-bit fixed point with I and Q in planes of their own,
 * because on the P4 a float multiply-add costs ~5 cycles whatever the code
 * (esp-dsp's own benchmarks: its hand-written float dot product is no faster
 * than C), while its 16-bit dot product runs on the core's SIMD (PIE) at
 * ~0.8 cycles a tap. In float the chain took 72 % of a core at 960 ksps on
 * the board (2026-10-07). The firmware lends esp-dsp to the apps
 * (aos_symbols.c); in the simulator and the host tests a C loop with the
 * same rounding does the same sums.
 *
 * Each filter is esp-dsp's decimating FIR (dsps_fird_s16), one call per
 * block and plane: called once per output sample instead, its dot product
 * cost ~85 cycles for 32 taps, mostly the call. It wants the taps 16-byte
 * aligned and a multiple of 8, and whole outputs per block: work goes 1200
 * input samples at a time, which every stage's decimation divides (the
 * reads' leftovers wait for the next one). A self-test compares it with
 * the C version once, and C is used if they differ; and stages that
 * decimate by an odd number always go through C (rf_demod_simd() says
 * why), so the app listens at 960 k or 1.92 Msps, where stage A divides by
 * 2 or 4.
 *
 * Only the discriminators, the envelope, the de-emphasis and the pilot's
 * Goertzel are float, at 240 or 48 kHz. The buffers, ~30 KB, are internal
 * RAM (a hot loop over PSRAM ran at a third of the speed on the board: the
 * panel reads its frame from there).
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")      /* the .so is built -Os */
#endif

#include "rf_demod.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN) && !defined(RF_HOST_TEST)
#include "esp_heap_caps.h"
#define RF_SIMD 1
/* esp-dsp 1.8.2, lent by the firmware (tools/gen_symbols.py; its headers
 * are not in the apps' build, so the structure is copied as it is there) */
typedef struct fir_s16_s {
    int16_t *coeffs;
    int16_t *delay;
    int16_t coeffs_len;
    int16_t pos;
    int16_t decim;
    int16_t d_pos;
    int16_t shift;
    int32_t *rounding_buff;
    int32_t rounding_val;
    int16_t free_status;
    int16_t delay_size;
    int16_t interp;
    int16_t interp_pos;
    int16_t start_pos;
} fir_s16_t;
extern int dsps_fird_init_s16(fir_s16_t *fir, int16_t *coeffs, int16_t *delay, int16_t coeffs_len, int16_t decim,
                              int16_t start_pos, int16_t shift);
extern int32_t dsps_fird_s16_arp4(fir_s16_t *fir, const int16_t *input, int16_t *output, int32_t len);
extern int dsps_fird_s16_aexx_free(fir_s16_t *fir);
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHUNK 1200          /* divided by every stage's decimation (2, 3, 4, 5, 10) */
#define ONE   16384.0f          /* full scale of the 16-bit samples: cu8's 128 */

/* ---- memory --------------------------------------------------------------- */

static int s_not_internal;      /* buffers that fell back to PSRAM */

/* 16-byte aligned, zeroed, internal and DMA-capable when it can be (without
 * the DMA flag the heap may hand out the slow LP SRAM, docs/APPS-P4.md).
 * The pointer just before the block remembers the allocation. */
static void *fast_alloc(size_t n)
{
    size_t total = n + 16 + sizeof(void *);
    void *raw = NULL;
#ifdef RF_SIMD
    raw = heap_caps_calloc(1, total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!raw) s_not_internal++;
#endif
    if (!raw) raw = calloc(1, total);
    if (!raw) return NULL;
    uintptr_t p = ((uintptr_t)raw + sizeof(void *) + 15) & ~(uintptr_t)15;
    ((void **)p)[-1] = raw;
    return (void *)p;
}

static void fast_free(void *p)
{
    if (p) free(((void **)p)[-1]);
}

/* ---- filters -------------------------------------------------------------- */

/* A decimating FIR over one plane, as esp-dsp's dsps_fird_s16_ansi does it:
 * a ring of the last Lc inputs, the taps backwards over it, the sum rounded
 * and >> 15. The simulator's and the tests' filter, and the board's when
 * the SIMD one fails its test. */
typedef struct {
    int16_t *delay;
    int pos;
} ring_t;

__attribute__((unused)) static int fird_c(ring_t *r, const int16_t *coef, int Lc, int D, const int16_t *in, int16_t *out, int nout)
{
    for (int o = 0; o < nout; o++) {
        for (int j = 0; j < D; j++) {
            if (r->pos >= Lc) r->pos = 0;
            r->delay[r->pos++] = *in++;
        }
        int64_t acc = 0x7fff;
        int c = Lc - 1;
        for (int k = r->pos; k < Lc; k++) acc += (int32_t)coef[c--] * r->delay[k];
        for (int k = 0; k < r->pos; k++) acc += (int32_t)coef[c--] * r->delay[k];
        out[o] = (int16_t)(acc >> 15);
    }
    return nout;
}

/* The C filter's dot product: 32-bit sums (with these unity-gain low-passes
 * and 16-bit samples the sum of |tap x sample| stays under 2^31), four at a
 * time, rounded as esp-dsp rounds. */
static inline int16_t dot_c(const int16_t *x, const int16_t *c, int L)
{
    int32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    for (int k = 0; k < L; k += 4) {
        a0 += x[k] * c[k];
        a1 += x[k + 1] * c[k + 1];
        a2 += x[k + 2] * c[k + 2];
        a3 += x[k + 3] * c[k + 3];
    }
    return (int16_t)(((a0 + a1) + (a2 + a3) + 0x7fff) >> 15);
}

typedef struct {
    int ntaps, Lc, D, planes, cap;
    int16_t *coef;              /* Lc: the taps, symmetric */
    int16_t *lin[2];            /* per plane: Lc - 1 of history, then the block */
    ring_t ring[2];             /* the reference's state (the self-test) */
#ifdef RF_SIMD
    fir_s16_t fd[2];            /* esp-dsp's */
    bool fd_ok;
#endif
} fir_t;

#ifdef RF_SIMD
static int s_simd = -1;         /* -1 not tested yet, 0 C, 1 SIMD */
#endif
static char s_simd_note[80] = "not on the board";

enum { WIN_HAMMING, WIN_BLACKMAN };

/* A low-pass of at least ntaps cut at fc (a fraction of the input rate),
 * keeping one output in D, over 1 (real) or 2 (I and Q) planes of up to
 * cap inputs a block.
 *
 * The length is rounded up to a multiple of 8 and the taps are symmetric
 * over all of it (an even length: half a sample of delay, which nobody
 * hears). esp-dsp's SIMD filter runs its taps forwards over the samples and
 * its C one backwards; padding odd taps with zeros at one end made the two
 * give the same filter a few samples apart, and the self-test refused the
 * SIMD one (2026-10-07). Symmetric, the two are the same sums. */
static bool fir_init(fir_t *f, int ntaps, float fc, int win, int D, int planes, int cap)
{
    memset(f, 0, sizeof *f);
    ntaps = (ntaps + 7) & ~7;
    f->ntaps = ntaps;
    f->D = D;
    f->planes = planes;
    f->cap = cap;
    f->Lc = ntaps;
    float *h = malloc(ntaps * sizeof(float));
    f->coef = fast_alloc((size_t)f->Lc * sizeof(int16_t));
    bool ok = h && f->coef;
    for (int p = 0; p < planes && ok; p++) {
        ok = (f->lin[p] = fast_alloc((size_t)(ntaps - 1 + cap + 8) * sizeof(int16_t))) != NULL &&
             (f->ring[p].delay = fast_alloc((size_t)f->Lc * sizeof(int16_t))) != NULL;
    }
    if (!ok) {
        free(h);
        return false;
    }
    float M = (ntaps - 1) / 2.0f;       /* between two taps: never t == 0 */
    float sum = 0;
    for (int k = 0; k < ntaps; k++) {
        float t = k - M;
        float s = sinf(2 * (float)M_PI * fc * t) / ((float)M_PI * t);
        float x = 2 * (float)M_PI * k / (ntaps - 1);
        float w = win == WIN_BLACKMAN ? 0.42f - 0.5f * cosf(x) + 0.08f * cosf(2 * x) : 0.54f - 0.46f * cosf(x);
        h[k] = s * w;
        sum += h[k];
    }
    for (int k = 0; k < ntaps; k++) {
        float q = h[k] / sum * 32768.0f;
        q += q >= 0 ? 0.5f : -0.5f;             /* rounded (lrintf is not in the firmware's table) */
        f->coef[k] = (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : (int)q);
    }
    free(h);
#ifdef RF_SIMD
    f->fd_ok = true;
    for (int p = 0; p < planes; p++)
        f->fd_ok = f->fd_ok && dsps_fird_init_s16(&f->fd[p], f->coef, f->ring[p].delay, (int16_t)f->Lc, (int16_t)D, 0, 0) == 0;
#endif
    return true;
}

static void fir_free(fir_t *f)
{
#ifdef RF_SIMD
    for (int p = 0; p < f->planes; p++)
        if (f->fd[p].coeffs) dsps_fird_s16_aexx_free(&f->fd[p]);
#endif
    fast_free(f->coef);
    for (int p = 0; p < 2; p++) {
        fast_free(f->lin[p]);
        fast_free(f->ring[p].delay);
    }
    memset(f, 0, sizeof *f);
}

static int s_use_simd = 1;      /* the app may turn it off, to compare (rf_demod_use_simd) */

/* where the next block's samples of a plane go */
static inline int16_t *fir_in(fir_t *f, int plane)
{
    return f->lin[plane] + f->ntaps - 1;
}

/* Runs over the n samples (a multiple of D) written at fir_in(): an output
 * after every D inputs, as esp-dsp's filter gives them. */
static int fir_run(fir_t *f, int n, int16_t *out0, int16_t *out1)
{
    const int nout = n / f->D, L = f->Lc, D = f->D;
    for (int p = 0; p < f->planes; p++) {
        int16_t *out = p ? out1 : out0;
#ifdef RF_SIMD
        /* even decimations only: see rf_demod_simd() */
        if (s_use_simd && s_simd > 0 && f->fd_ok && !(D & 1)) {
            dsps_fird_s16_arp4(&f->fd[p], fir_in(f, p), out, nout);
        } else
#endif
        {
            const int16_t *x = f->lin[p] + D - 1;   /* the first output's window */
            for (int o = 0; o < nout; o++, x += D) out[o] = dot_c(x, f->coef, L);
        }
        memmove(f->lin[p], f->lin[p] + n, (size_t)(f->ntaps - 1) * sizeof(int16_t));
    }
    return nout;
}

void rf_demod_use_simd(bool on)
{
    s_use_simd = on;
}

/* The SIMD filter against the C one, on noise through filters decimating
 * by 2 and by 4. Odd decimations are left to C: through a 56-tap filter
 * decimating by 5 the SIMD one parted from C as soon as its delay line
 * wrapped (2026-10-07, the first 11 outputs equal, then not) - with an odd
 * step its position in the line goes odd, and its 128-bit loads, it seems,
 * cannot start there. */
bool rf_demod_simd(void)
{
#ifdef RF_SIMD
    if (s_simd >= 0) return s_simd > 0;
    s_simd = 0;
    static const int TAPS[2] = { 21, 55 }, DEC[2] = { 2, 4 };
    bool same = true;
    for (int t = 0; t < 2 && same; t++) {
        fir_t a, b;
        int16_t *oa = fast_alloc(600 * sizeof(int16_t)), *ob = fast_alloc(600 * sizeof(int16_t));
        if (!oa || !ob || !fir_init(&a, TAPS[t], 0.2f, WIN_BLACKMAN, DEC[t], 1, 1200) ||
            !fir_init(&b, TAPS[t], 0.2f, WIN_BLACKMAN, DEC[t], 1, 1200) || !a.fd_ok) {
            same = false;
        } else {
            uint32_t r = 12345;
            for (int blk = 0; blk < 3 && same; blk++) {
                for (int i = 0; i < 1200; i++) {
                    r = r * 1103515245u + 12345u;
                    fir_in(&a, 0)[i] = fir_in(&b, 0)[i] = (int16_t)((int)((r >> 16) % 30000) - 15000);
                }
                /* its return value is not the count (esp-dsp 1.8.2 returns a
                 * register it never loads): the count is ours */
                dsps_fird_s16_arp4(&a.fd[0], fir_in(&a, 0), oa, 1200 / DEC[t]);
                int na = 1200 / DEC[t];
                int nb = fird_c(&b.ring[0], b.coef, b.Lc, DEC[t], fir_in(&b, 0), ob, 1200 / DEC[t]);
                same = na == nb && !memcmp(oa, ob, (size_t)na * sizeof(int16_t));
                if (!same) {
                    int k = 0;
                    while (k < na && k < nb && oa[k] == ob[k]) k++;
                    snprintf(s_simd_note, sizeof s_simd_note, "%d taps /%d, block %d: %d/%d outputs, first difference at %d (%d vs %d)",
                             a.Lc, DEC[t], blk, na, nb, k, k < na ? oa[k] : 0, k < nb ? ob[k] : 0);
                }
            }
        }
        fir_free(&a);
        fir_free(&b);
        fast_free(oa);
        fast_free(ob);
    }
    s_simd = same ? 1 : 0;
    if (same) snprintf(s_simd_note, sizeof s_simd_note, "the same sums as C");
    return same;
#else
    return false;
#endif
}

/* ---- the demodulator ------------------------------------------------------ */

struct rf_demod {
    int mode;
    uint32_t rate;
    int32_t dc_i, dc_q;         /* the input's DC, in 16-bit units */
    int64_t dc_si, dc_sq;       /* sums toward the next estimate */
    int32_t dc_n;
    bool has_a;
    fir_t a, b, c, ch, au;
    int16_t *ti, *tq;           /* a stage's output when it is not the next one's input */
    float pi, pq;               /* the discriminator's last sample */
    float fm_scale;
    float de, de_k;             /* de-emphasis */
    float am_avg, hp;           /* AM's carrier, narrow FM's offset */
    float gain;
    /* squelch */
    int sq_db;
    double pwr_acc;
    int pwr_n, pwr_len;
    float noise_db, level_db;
    bool open, noise_given;
    int hang;
    /* the pilot: Goertzel at 17, 19 and 21 kHz on one 20 ms block in four */
    float g[3][2], gk[3];
    int gn, gblock;
    float pilot_db;
    uint64_t us[5];
    uint8_t pend[2 * CHUNK];    /* a read's leftover pairs, short of a block */
    int npend;
};

static uint64_t (*s_clock)(void);
static inline uint64_t now_us(void) { return s_clock ? s_clock() : 0; }

uint32_t rf_demod_half_width(int mode)
{
    switch (mode) {
    case RF_MODE_WFM: return 100000;
    case RF_MODE_AM: return 4000;
    case RF_MODE_NFM: return 6500;
    default: return 0;
    }
}

rf_demod_t *rf_demod_new(int mode, uint32_t rate)
{
    if (mode <= RF_MODE_OFF || mode >= RF_MODE_COUNT) return NULL;
    int da = rate == 240000 ? 1 : (rate % 480000 == 0 ? (int)(rate / 480000) : 0);
    if (da < 1 || da > 5) return NULL;
    (void)rf_demod_simd();
    rf_demod_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->mode = mode;
    d->rate = rate;
    float r1 = rate;
    bool ok = true;
    if (da > 1) {
        /* +-110 kHz through, Blackman (74 dB) down by 370 kHz, where what
         * folds back into +-110 kHz at 480 kHz starts */
        ok = ok && fir_init(&d->a, (int)(5.5f * rate / 260000.0f) + 1, 240000.0f / rate, WIN_BLACKMAN, da, 2, CHUNK);
        d->has_a = true;
        r1 = 480000;
    }
    int n1 = CHUNK / da;                /* what stage A gives per block */
    if (mode == RF_MODE_WFM) {
        if (r1 == 480000) ok = ok && fir_init(&d->b, 55, 0.25f, WIN_BLACKMAN, 2, 2, n1);
        int n2 = r1 == 480000 ? n1 / 2 : n1;
        /* 15 kHz through, down by 33 kHz (what folds back into 15 kHz at 48 kHz) */
        ok = ok && fir_init(&d->au, 73, 24000.0f / 240000.0f, WIN_BLACKMAN, 5, 1, n2);
        d->fm_scale = 1.0f / (2 * (float)M_PI * 75000.0f / 240000.0f);
        rf_demod_set_deemph_us(d, 75);
        d->gain = 0.8f;
        static const float GF[3] = { 17000, 19000, 21000 };
        for (int k = 0; k < 3; k++) d->gk[k] = 2 * cosf(2 * (float)M_PI * GF[k] / 240000.0f);
        d->pwr_len = 24;                /* chunks to a squelch decision */
    } else {
        /* to 24 kHz: 10 or 20, even, so on the SIMD */
        int dc = (int)(r1 / 24000);
        /* +-8 kHz through, down by 16 kHz (what folds back into 8 kHz at 24 kHz) */
        ok = ok && fir_init(&d->c, (int)(5.5f * r1 / 8000.0f) + 1, 12000.0f / r1, WIN_BLACKMAN, dc, 2, n1);
        int n3 = n1 / dc;
        /* the channel, Blackman: AM +-4 kHz down by 6.5, narrow FM +-6.5 kHz
         * down by 9.5 (the next 12.5 kHz channel's edge) */
        if (mode == RF_MODE_AM) ok = ok && fir_init(&d->ch, 53, 5250.0f / 24000.0f, WIN_BLACKMAN, 1, 2, n3);
        else ok = ok && fir_init(&d->ch, 45, 8000.0f / 24000.0f, WIN_BLACKMAN, 1, 2, n3);
        ok = ok && fir_init(&d->au, 40, 4000.0f / 24000.0f, WIN_HAMMING, 1, 1, n3);
        d->fm_scale = 1.0f / (2 * (float)M_PI * 5000.0f / 24000.0f);
        d->gain = mode == RF_MODE_AM ? 0.7f : 1.2f;
        d->sq_db = 10;
        d->pwr_len = (int)(rate / CHUNK / 20) + 1;     /* ~50 ms */
    }
    d->ti = fast_alloc((size_t)(n1 + 8) * sizeof(int16_t));
    d->tq = fast_alloc((size_t)(n1 + 8) * sizeof(int16_t));
    d->noise_db = 200;
    if (!ok || !d->ti || !d->tq) {
        rf_demod_free(d);
        return NULL;
    }
    return d;
}

void rf_demod_free(rf_demod_t *d)
{
    if (!d) return;
    fir_free(&d->a);
    fir_free(&d->b);
    fir_free(&d->c);
    fir_free(&d->ch);
    fir_free(&d->au);
    fast_free(d->ti);
    fast_free(d->tq);
    free(d);
}

int rf_demod_mode(const rf_demod_t *d)
{
    return d->mode;
}

uint32_t rf_demod_rate(const rf_demod_t *d)
{
    return d->rate;
}

void rf_demod_set_squelch(rf_demod_t *d, int db)
{
    d->sq_db = db < 0 ? 0 : db;
}

void rf_demod_set_deemph_us(rf_demod_t *d, int us)
{
    d->de_k = 1.0f - expf(-1.0f / (240000.0f * us * 1e-6f));
}

/* atan2 within 0.004 rad: the discriminator runs it 240 000 times a second */
static inline float fast_atan2f(float y, float x)
{
    float ax = fabsf(x), ay = fabsf(y);
    float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    if (mx == 0) return 0;
    float a = mn / mx, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}

/* the channel's noise bandwidth: what its filter lets through, in Hz */
static float noise_bw(const rf_demod_t *d)
{
    switch (d->mode) {
    case RF_MODE_WFM: return 240000.0f;
    case RF_MODE_AM: return 10500.0f;
    default: return 16000.0f;
    }
}

void rf_demod_set_noise_floor(rf_demod_t *d, float bin_db, int fft_n)
{
    /* a Hann-windowed bin of white noise reads 1.5 / n of its power
     * (rf_dsp.c normalises by the window's sum); the channel holds bw / rate */
    d->noise_db = bin_db - 10.0f * log10f(1.5f / fft_n) + 10.0f * log10f(noise_bw(d) / d->rate);
    d->noise_given = true;
}

static void squelch_feed(rf_demod_t *d, const int16_t *i, const int16_t *q, int n)
{
    int64_t acc = 0;
    for (int k = 0; k < n; k++) acc += (int32_t)i[k] * i[k] + (int32_t)q[k] * q[k];
    d->pwr_acc += (double)acc / ((double)ONE * ONE);
    d->pwr_n += n;
}

static void squelch_decide(rf_demod_t *d, int chunks_done)
{
    if (!d->pwr_n) return;
    float p = 10.0f * log10f((float)(d->pwr_acc / d->pwr_n) + 1e-20f);
    d->pwr_acc = 0;
    d->pwr_n = 0;
    if (!d->noise_given) {
        /* nobody gave a floor (the tests): the quietest moment so far,
         * creeping up 0.1 dB a second */
        if (p < d->noise_db) d->noise_db = p;
        else d->noise_db += 0.1f * chunks_done * CHUNK / d->rate;
    }
    d->level_db = p - d->noise_db;
    if (!d->sq_db || d->level_db >= d->sq_db) {
        d->open = true;
        d->hang = (int)(0.3f * d->rate / CHUNK);        /* 300 ms after the signal goes */
    } else {
        d->hang -= chunks_done;
        if (d->hang <= 0) d->open = false;
    }
}

static inline int16_t to16(float v)
{
    v *= ONE;
    return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

static int to_pcm(rf_demod_t *d, const int16_t *a, int n, int16_t *out, int max)
{
    if (n > max) n = max;
    float k = d->open ? d->gain * 32767.0f / ONE : 0;
    for (int i = 0; i < n; i++) {
        float v = a[i] * k;
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    }
    return n;
}

static int run_chunk(rf_demod_t *d, const uint8_t *iq, int n, int16_t *audio, int max)
{
    uint64_t t0 = now_us(), t1;
    /* cu8 to 16-bit I and Q without the DC, into the first stage's planes
     * (240 kHz broadcast FM has no stage before the discriminator) */
    fir_t *first = d->has_a ? &d->a : d->mode == RF_MODE_WFM ? NULL : &d->c;
    int16_t *xi = first ? fir_in(first, 0) : d->ti, *xq = first ? fir_in(first, 1) : d->tq;
    int dci = d->dc_i, dcq = d->dc_q;
    int32_t si = 0, sq = 0;
    for (int k = 0; k < n; k++) {
        int vi = ((int)iq[2 * k] << 7) - 16384, vq = ((int)iq[2 * k + 1] << 7) - 16384;
        si += vi;
        sq += vq;
        xi[k] = (int16_t)(vi - dci);
        xq[k] = (int16_t)(vq - dcq);
    }
    /* The DC is the mean over 0.1 s, taken whole each time. A block is only
     * 0.5 ms at 2.4 Msps: following each block's mean followed part of a
     * carrier a kHz off the centre too, and AM came out with its second
     * harmonic at -27 dB (the RTL2832's DC hardly moves anyway). */
    d->dc_si += si;
    d->dc_sq += sq;
    d->dc_n += n;
    if (d->dc_n >= (int32_t)(d->rate / 10)) {
        d->dc_i = (int32_t)(d->dc_si / d->dc_n);
        d->dc_q = (int32_t)(d->dc_sq / d->dc_n);
        d->dc_si = d->dc_sq = 0;
        d->dc_n = 0;
    }
    t1 = now_us();
    d->us[0] += t1 - t0;
    t0 = t1;
    int m = n;
    const int16_t *ci16 = xi, *cq16 = xq;
    if (d->has_a) {
        fir_t *next = d->mode == RF_MODE_WFM ? &d->b : &d->c;
        m = fir_run(&d->a, n, fir_in(next, 0), fir_in(next, 1));
        ci16 = fir_in(next, 0);
        cq16 = fir_in(next, 1);
    }
    t1 = now_us();
    d->us[1] += t1 - t0;
    t0 = t1;
    int16_t *au = fir_in(&d->au, 0);
    if (d->mode == RF_MODE_WFM) {
        if (d->has_a) {
            m = fir_run(&d->b, m, d->ti, d->tq);
            ci16 = d->ti;
            cq16 = d->tq;
        }
        squelch_feed(d, ci16, cq16, m);
        t1 = now_us();
        d->us[2] += t1 - t0;
        t0 = t1;
        /* discriminator and de-emphasis, into the audio filter */
        float pi = d->pi, pq = d->pq, de = d->de;
        bool pilot = (d->gblock & 3) == 0;
        for (int k = 0; k < m; k++) {
            float ci = ci16[k], cq = cq16[k];
            float ph = fast_atan2f(cq * pi - ci * pq, ci * pi + cq * pq) * d->fm_scale;
            pi = ci;
            pq = cq;
            if (pilot)
                for (int g = 0; g < 3; g++) {
                    float y = ph + d->gk[g] * d->g[g][0] - d->g[g][1];
                    d->g[g][1] = d->g[g][0];
                    d->g[g][0] = y;
                }
            de += (ph - de) * d->de_k;
            au[k] = to16(de);
        }
        d->pi = pi;
        d->pq = pq;
        d->de = de;
        d->gn += m;
        if (d->gn >= 4800) {
            /* 20 ms: the pilot's power over the mean of its neighbours' */
            if (pilot) {
                float p[3];
                for (int g = 0; g < 3; g++) {
                    float a = d->g[g][0], b = d->g[g][1];
                    p[g] = a * a + b * b - d->gk[g] * a * b;
                    d->g[g][0] = d->g[g][1] = 0;
                }
                float r = 10.0f * log10f((p[1] + 1e-12f) / ((p[0] + p[2]) / 2 + 1e-12f));
                d->pilot_db += (r - d->pilot_db) * 0.25f;
            }
            d->gn = 0;
            d->gblock++;
        }
    } else {
        /* AM and narrow FM */
        m = fir_run(&d->c, m, fir_in(&d->ch, 0), fir_in(&d->ch, 1));
        m = fir_run(&d->ch, m, d->ti, d->tq);
        squelch_feed(d, d->ti, d->tq, m);
        t1 = now_us();
        d->us[2] += t1 - t0;
        t0 = t1;
        if (d->mode == RF_MODE_AM) {
            float avg = d->am_avg;
            for (int k = 0; k < m; k++) {
                float ci = d->ti[k], cq = d->tq[k];
                float e = sqrtf(ci * ci + cq * cq);
                avg += (e - avg) * 0.001f;      /* ~40 ms at 24 kHz: the carrier */
                au[k] = to16(avg > 1.0f ? (e - avg) / avg : 0);
            }
            d->am_avg = avg;
        } else {
            float pi = d->pi, pq = d->pq, hp = d->hp;
            for (int k = 0; k < m; k++) {
                float ci = d->ti[k], cq = d->tq[k];
                float ph = fast_atan2f(cq * pi - ci * pq, ci * pi + cq * pq) * d->fm_scale;
                pi = ci;
                pq = cq;
                hp += (ph - hp) * 0.008f;       /* the offset of a mistuned carrier */
                au[k] = to16(ph - hp);
            }
            d->pi = pi;
            d->pq = pq;
            d->hp = hp;
        }
    }
    t1 = now_us();
    d->us[3] += t1 - t0;
    t0 = t1;
    int a = fir_run(&d->au, m, d->ti, NULL);
    d->us[4] += now_us() - t0;
    return to_pcm(d, d->ti, a, audio, max);
}

int rf_demod_run(rf_demod_t *d, const uint8_t *iq, int n, int16_t *audio, int max)
{
    int out = 0, chunks = 0, i = 0;
    /* whole blocks only: a short one waits for the next read */
    if (d->npend) {
        int take = CHUNK - d->npend < n ? CHUNK - d->npend : n;
        memcpy(d->pend + 2 * d->npend, iq, (size_t)take * 2);
        d->npend += take;
        i = take;
        if (d->npend < CHUNK) return 0;
        out += run_chunk(d, d->pend, CHUNK, audio + out, max - out);
        d->npend = 0;
        chunks++;
    }
    for (; i + CHUNK <= n; i += CHUNK) {
        out += run_chunk(d, iq + 2 * i, CHUNK, audio + out, max - out);
        if (++chunks >= d->pwr_len) {
            squelch_decide(d, chunks);
            chunks = 0;
        }
    }
    if (chunks) squelch_decide(d, chunks);
    if (i < n) {
        memcpy(d->pend, iq + 2 * i, (size_t)(n - i) * 2);
        d->npend = n - i;
    }
    return out;
}

void rf_demod_stats(const rf_demod_t *d, rf_demod_stats_t *o)
{
    o->level_db = d->level_db;
    o->open = d->open;
    o->pilot_db = d->mode == RF_MODE_WFM ? d->pilot_db : 0;
}

void rf_demod_set_clock(uint64_t (*now)(void))
{
    s_clock = now;
}

void rf_demod_profile(rf_demod_t *d, uint32_t out_us[5], int *not_internal)
{
    for (int i = 0; i < 5; i++) {
        out_us[i] = (uint32_t)d->us[i];
        d->us[i] = 0;
    }
    if (not_internal) *not_internal = s_not_internal;
}

const char *rf_demod_simd_note(void)
{
    return s_simd_note;
}

uint32_t rf_demod_audio_rate(const rf_demod_t *d)
{
    return d->mode == RF_MODE_WFM ? 48000 : 24000;
}
