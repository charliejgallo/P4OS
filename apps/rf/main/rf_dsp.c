/*
 * RF - the spectrum: a Hann-windowed complex FFT of 8-bit I/Q, as power in
 * dB per bin, averaged over a few FFTs (rf.h).
 *
 * Plain radix-2 in single-precision float, the P4's FPU: a 2048-point FFT
 * is about 56 k multiply-adds, and the display wants some 25 x 4 of them a
 * second, a few percent of a core. The samples the display does not need
 * are read and dropped by the worker; demodulation, later, takes them all.
 */
#include "rf.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct rf_fft {
    int n, log2n;
    float *win;         /* n */
    float *cs, *sn;     /* n/2 twiddles */
    uint16_t *rev;      /* n */
    float *re, *im;     /* n */
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
    f->cs = malloc(n / 2 * sizeof(float));
    f->sn = malloc(n / 2 * sizeof(float));
    f->rev = malloc(n * sizeof(uint16_t));
    f->re = malloc(n * sizeof(float));
    f->im = malloc(n * sizeof(float));
    f->acc = calloc(n, sizeof(float));
    if (!f->win || !f->cs || !f->sn || !f->rev || !f->re || !f->im || !f->acc) {
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
        f->cs[i] = cosf(2.0f * (float)M_PI * i / n);
        f->sn[i] = -sinf(2.0f * (float)M_PI * i / n);
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
    free(f->cs);
    free(f->sn);
    free(f->rev);
    free(f->re);
    free(f->im);
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
    float *re = f->re, *im = f->im;
    for (int i = 0; i < n; i++) {
        int k = f->rev[i];
        re[k] = f->lut[iq[2 * i]] * f->win[i];
        im[k] = f->lut[iq[2 * i + 1]] * f->win[i];
    }
    for (int len = 2, step = n / 2; len <= n; len <<= 1, step >>= 1) {
        int half = len >> 1;
        for (int s = 0; s < n; s += len)
            for (int j = 0; j < half; j++) {
                float wr = f->cs[j * step], wi = f->sn[j * step];
                int a = s + j, b = a + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
    }
    for (int i = 0; i < n; i++) f->acc[i] += re[i] * re[i] + im[i] * im[i];
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
