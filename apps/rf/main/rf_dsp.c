/*
 * RF - the spectrum: a Hann-windowed complex FFT of 8-bit I/Q, as power in
 * dB per bin, averaged over a few FFTs (rf.h).
 *
 * Plain radix-2 in single-precision float, the P4's FPU: a 2048-point FFT
 * is about 56 k multiply-adds, and the display wants some 25 x 4 of them a
 * second. The samples the display does not need are read and dropped by the
 * worker; demodulation, later, takes them all.
 *
 * The butterflies' working set - the complex samples, the twiddles and the
 * bit-reversal table, 28 KB at 2048 points - is in INTERNAL RAM, an
 * exception to PSRAM first that was measured: with everything in PSRAM an
 * FFT took 4.3 ms on the board (2026-10-06), the panel reading its frame
 * from the same PSRAM all the time. The window and the sums are read once
 * per FFT, in order, and stay in PSRAM.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")      /* the .so is built -Os */
#endif

#include "rf.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN) && !defined(RF_HOST_TEST)
#include "esp_heap_caps.h"
/* On the board the FFT is esp-dsp's 16-bit one on the core's SIMD, lent by
 * the firmware: ~7x the float one (its benchmarks, 1024 points), which took
 * 1.4 ms of the board's time at 2048 points (2026-10-07): 14 % of a core
 * for the display's 100 a second. */
#define RF_FFT_SIMD 1
extern int dsps_fft2r_sc16_arp4_(int16_t *data, int N, int16_t *w);
extern int dsps_bit_rev_sc16_ansi(int16_t *data, int N);
#endif

/* internal and DMA-capable: without the DMA flag the heap may hand out the
 * LP SRAM, many times slower (docs/APPS-P4.md) */
static void *fast_alloc(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN) && !defined(RF_HOST_TEST)
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    return p ? p : malloc(n);
#else
    return malloc(n);
#endif
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct rf_fft {
    int n, log2n;
#ifdef RF_FFT_SIMD
    int16_t *xq;        /* n complex, 16-bit, re and im side by side (internal, aligned) */
    int16_t *wq;        /* n/2 twiddles as esp-dsp's init makes them (internal, aligned) */
    int16_t *hq;        /* the window, Q15 */
    float qscale;       /* |X|^2 of the 16-bit FFT to the float one's */
    void *raw[3];
#endif
    float *win;         /* n */
    float *tw;          /* n/2 twiddles, cos and -sin side by side (internal) */
    uint16_t *rev;      /* n (internal) */
    float *x;           /* n complex samples, re and im side by side (internal) */
    float *acc;         /* n: summed power */
    int nacc;
    float lut[256];     /* (b - 127.4) / 128 */
};

rf_fft_t *rf_fft_new(int n)
{
    int log2n = 0;
    while ((1 << log2n) < n) log2n++;
    n = 1 << log2n;
    rf_fft_t *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->n = n;
    f->log2n = log2n;
    f->win = malloc(n * sizeof(float));
    f->tw = fast_alloc(n * sizeof(float));
    f->rev = fast_alloc(n * sizeof(uint16_t));
    f->x = fast_alloc(2 * n * sizeof(float));
    f->acc = calloc(n, sizeof(float));
    if (!f->win || !f->tw || !f->rev || !f->x || !f->acc) {
        rf_fft_free(f);
        return NULL;
    }
    /* the window's power gain, so a bin reads the same whatever the size */
    float wsum = 0;
    for (int i = 0; i < n; i++) {
        f->win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (n - 1));
        wsum += f->win[i];
    }
    for (int i = 0; i < n; i++) f->win[i] /= wsum;
    for (int i = 0; i < n / 2; i++) {
        f->tw[2 * i] = cosf(2.0f * (float)M_PI * i / n);
        f->tw[2 * i + 1] = -sinf(2.0f * (float)M_PI * i / n);
    }
    for (int i = 0; i < n; i++) {
        unsigned r = 0;
        for (int b = 0; b < log2n; b++) r |= ((i >> b) & 1) << (log2n - 1 - b);
        f->rev[i] = r;
    }
    /* the RTL2832's zero is between 127 and 128 */
    for (int b = 0; b < 256; b++) f->lut[b] = (b - 127.4f) / 128.0f;
#ifdef RF_FFT_SIMD
    size_t sz[3] = { 2 * n * sizeof(int16_t), n * sizeof(int16_t), n * sizeof(int16_t) };
    int16_t **dst[3] = { &f->xq, &f->wq, &f->hq };
    for (int k = 0; k < 3; k++) {
        f->raw[k] = heap_caps_malloc(sz[k] + 16, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        if (!f->raw[k]) f->raw[k] = malloc(sz[k] + 16);
        if (!f->raw[k]) {
            rf_fft_free(f);
            return NULL;
        }
        *dst[k] = (int16_t *)(((uintptr_t)f->raw[k] + 15) & ~(uintptr_t)15);
    }
    /* the twiddles as dsps_fft2r_init_sc16(table, n) leaves them: n/2 of
     * them, then in bit-reversed order (made here so esp-dsp's global table,
     * set once per boot, is not ours to depend on) */
    for (int i = 0; i < n / 2; i++) {
        f->wq[2 * i] = (int16_t)(32767 * cosf(2 * (float)M_PI * i / n));
        f->wq[2 * i + 1] = (int16_t)(32767 * sinf(2 * (float)M_PI * i / n));
    }
    dsps_bit_rev_sc16_ansi(f->wq, n / 2);
    float hsum = 0;
    for (int i = 0; i < n; i++) {
        float h = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (n - 1));
        f->hq[i] = (int16_t)(h * 32767);
        hsum += h;
    }
    /* The samples go in as (b - 128) * 256: the float FFT's x * 32768. The
     * window is Q15, and each of the log2(n) stages halves: X16 = X *
     * 32768 / n, where the float FFT's window is divided by its sum. */
    float k = (float)n / (32768.0f * hsum);
    f->qscale = k * k;
#endif
    return f;
}

void rf_fft_free(rf_fft_t *f)
{
    if (!f) return;
#ifdef RF_FFT_SIMD
    for (int k = 0; k < 3; k++) free(f->raw[k]);
#endif
    free(f->win);
    free(f->tw);
    free(f->rev);
    free(f->x);
    free(f->acc);
    free(f);
}

int rf_fft_size(const rf_fft_t *f)
{
    return f->n;
}

static bool s_fft_simd = true;

void rf_fft_use_simd(bool on)
{
    s_fft_simd = on;
}

void rf_fft_add_cu8(rf_fft_t *f, const uint8_t *iq)
{
    const int n = f->n;
#ifdef RF_FFT_SIMD
    if (s_fft_simd) {
        int16_t *x = f->xq;
        const int16_t *h = f->hq;
        for (int i = 0; i < n; i++) {
            x[2 * i] = (int16_t)((((int)iq[2 * i] - 128) * 256 * h[i]) >> 15);
            x[2 * i + 1] = (int16_t)((((int)iq[2 * i + 1] - 128) * 256 * h[i]) >> 15);
        }
        dsps_fft2r_sc16_arp4_(x, n, f->wq);
        dsps_bit_rev_sc16_ansi(x, n);
        float *acc = f->acc, k = f->qscale;
        for (int i = 0; i < n; i++) {
            int32_t re = x[2 * i], im = x[2 * i + 1];
            acc[i] += (float)(re * re + im * im) * k;
        }
        f->nacc++;
        return;
    }
#endif
    float *x = f->x;
    const float *lut = f->lut, *win = f->win;
    const uint16_t *rev = f->rev;
    for (int i = 0; i < n; i++) {
        float *d = x + 2 * rev[i];
        float w = win[i];
        d[0] = lut[iq[2 * i]] * w;
        d[1] = lut[iq[2 * i + 1]] * w;
    }
    /* the first stage has only the twiddle 1 */
    for (int s = 0; s < 2 * n; s += 4) {
        float ar = x[s], ai = x[s + 1], br = x[s + 2], bi = x[s + 3];
        x[s] = ar + br;
        x[s + 1] = ai + bi;
        x[s + 2] = ar - br;
        x[s + 3] = ai - bi;
    }
    for (int half = 2, step = n / 4; half < n; half <<= 1, step >>= 1) {
        for (int j = 0; j < half; j++) {
            const float wr = f->tw[2 * j * step], wi = f->tw[2 * j * step + 1];
            for (int a = 2 * j; a < 2 * n; a += 4 * half) {
                float *pa = x + a, *pb = pa + 2 * half;
                float tr = pb[0] * wr - pb[1] * wi;
                float ti = pb[0] * wi + pb[1] * wr;
                pb[0] = pa[0] - tr;
                pb[1] = pa[1] - ti;
                pa[0] += tr;
                pa[1] += ti;
            }
        }
    }
    float *acc = f->acc;
    for (int i = 0; i < n; i++) acc[i] += x[2 * i] * x[2 * i] + x[2 * i + 1] * x[2 * i + 1];
    f->nacc++;
}

int rf_fft_count(const rf_fft_t *f)
{
    return f->nacc;
}

/* Out: n bins in dB, the lowest frequency first (bin n/2 of the FFT is
 * the lowest: the halves swap). Resets the sum. */
void rf_fft_take_db(rf_fft_t *f, float *out)
{
    const int n = f->n;
    float k = f->nacc ? 1.0f / f->nacc : 1.0f;
    for (int i = 0; i < n; i++) {
        float p = f->acc[(i + n / 2) & (n - 1)] * k;
        out[i] = 10.0f * log10f(p + 1e-12f);
    }
    memset(f->acc, 0, n * sizeof(float));
    f->nacc = 0;
}

float rf_fft_floor_db(const float *db, int n, int navg)
{
    /* the 20th percentile of a chi-squared of 2 navg degrees over its mean,
     * in dB: 1 FFT -6.5, 2 -3.9, 4 -2.4, 8 -1.6 */
    float bias = navg >= 8 ? 1.6f : navg >= 4 ? 2.4f : navg >= 2 ? 3.9f : 6.5f;
    /* a histogram in half-dB steps from -160 dB: no sort */
    static uint16_t hist[320];
    memset(hist, 0, sizeof hist);
    for (int i = 0; i < n; i++) {
        int b = (int)((db[i] + 160.0f) * 2.0f);
        hist[b < 0 ? 0 : b > 319 ? 319 : b]++;
    }
    int want = n / 5, acc = 0, b = 0;
    while (b < 319 && (acc += hist[b]) < want) b++;
    return b / 2.0f - 160.0f + bias;
}
