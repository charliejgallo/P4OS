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

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

/* internal and DMA-capable: without the DMA flag the heap may hand out the
 * LP SRAM, many times slower (docs/APPS-P4.md) */
static void *fast_alloc(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
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
    return f;
}

void rf_fft_free(rf_fft_t *f)
{
    if (!f) return;
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

void rf_fft_add_cu8(rf_fft_t *f, const uint8_t *iq)
{
    const int n = f->n;
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
