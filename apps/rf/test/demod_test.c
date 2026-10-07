/*
 * demod_test - runs rf_demod.c over a cu8 file and says what came out
 * (apps/rf/test/README.md):
 *
 *   demod_test <wfm|am|nfm> <rate> <in.cu8> [out.wav]
 *
 * It prints the strongest audio tone (by a DFT over 100 Hz steps), how far
 * above the rest of the audio band it stands, the share of the time the
 * squelch was open, the pilot meter (broadcast FM) and the time the
 * demodulator took. Built for the host, with -DRF_HOST_TEST.
 */
#include "rf_demod.h"
#include "rf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void wav(const char *path, const int16_t *a, int n, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t bytes = n * 2, x;
    fwrite("RIFF", 1, 4, f);
    x = 36 + bytes; fwrite(&x, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    x = 16; fwrite(&x, 4, 1, f);
    uint16_t h[2] = { 1, 1 }; fwrite(h, 2, 2, f);
    fwrite(&rate, 4, 1, f);
    x = rate * 2; fwrite(&x, 4, 1, f);
    uint16_t h2[2] = { 2, 16 }; fwrite(h2, 2, 2, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
    fwrite(a, 2, n, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "demod_test <wfm|am|nfm> <rate> <in.cu8> [out.wav]\n");
        return 2;
    }
    int mode = !strcmp(argv[1], "wfm") ? RF_MODE_WFM : !strcmp(argv[1], "am") ? RF_MODE_AM : RF_MODE_NFM;
    uint32_t rate = (uint32_t)atol(argv[2]);
    FILE *f = fopen(argv[3], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *iq = malloc(bytes);
    fread(iq, 1, bytes, f);
    fclose(f);
    long n = bytes / 2;
    rf_demod_t *d = rf_demod_new(mode, rate);
    if (!d) {
        fprintf(stderr, "rf_demod_new refused %s at %u\n", argv[1], rate);
        return 1;
    }
    int16_t *audio = malloc((n / (rate / RF_AUDIO_RATE) + 64) * sizeof(int16_t));
    int na = 0, open_blocks = 0, blocks = 0;
    /* the noise floor from the spectrum, as the app does it: four FFTs of
     * 2048 averaged per read */
    rf_fft_t *fft = rf_fft_new(2048);
    float *db = malloc(2048 * sizeof(float));
    double fft_cpu = 0;
    clock_t t0 = clock();
    /* in the reads the board makes: 16 K pairs */
    long rd = getenv("RF_READ") ? atol(getenv("RF_READ")) : 16384;
    for (long i = 0; i < n; i += rd) {
        int c = n - i < rd ? (int)(n - i) : (int)rd;
        if (c >= 4 * 2048) {
            clock_t f0 = clock();
            for (int k = 0; k < 4; k++) rf_fft_add_cu8(fft, iq + 2 * i + 2 * 2048 * k);
            rf_fft_take_db(fft, db);
            rf_demod_set_noise_floor(d, rf_fft_floor_db(db, 2048, 4), 2048);
            fft_cpu += (double)(clock() - f0) / CLOCKS_PER_SEC;
        }
        na += rf_demod_run(d, iq + 2 * i, c, audio + na, 1 << 20);
        rf_demod_stats_t st;
        rf_demod_stats(d, &st);
        open_blocks += st.open;
        blocks++;
        if (getenv("RF_TRACE") && blocks % 8 == 0)
            printf("  t=%.2f s level %.1f dB %s\n", (double)i / rate, st.level_db, st.open ? "open" : "closed");
    }
    double cpu = (double)(clock() - t0) / CLOCKS_PER_SEC - fft_cpu;
    const uint32_t ar = rf_demod_audio_rate(d);
    rf_demod_stats_t st;
    rf_demod_stats(d, &st);
    /* the tone: the strongest 100 Hz step from 200 Hz to 5 kHz, over the
     * steady second half of the audio (the filters settled); narrow FM's
     * carrier is keyed from 1/4 to 3/4 of the time: from 0.35 to 0.7 */
    int a0 = mode == RF_MODE_NFM ? na * 35 / 100 : na / 2;
    int len = mode == RF_MODE_NFM ? na * 70 / 100 - a0 : na - a0;
    double best = 0, total = 0;
    int bestf = 0;
    for (int fq = 200; fq <= 5000; fq += 100) {
        double re = 0, im = 0;
        for (int i = 0; i < len; i++) {
            double ph = 2 * M_PI * fq * i / ar;
            re += audio[a0 + i] * cos(ph);
            im -= audio[a0 + i] * sin(ph);
        }
        double p = re * re + im * im;
        total += p;
        if (p > best) {
            best = p;
            bestf = fq;
        }
    }
    double rms = 0;
    for (int i = 0; i < len; i++) rms += (double)audio[a0 + i] * audio[a0 + i];
    rms = sqrt(rms / (len ? len : 1));
    printf("%s at %u sps: %d audio samples (%.2f s), tone %d Hz, %.1f dB over the rest, rms %.0f,\n"
           "  squelch open %d/%d reads, level %.1f dB, pilot %.1f dB, %.0f %% of real time on this Mac\n",
           argv[1], rate, na, (double)na / ar, bestf, 10 * log10(best / (total - best + 1e-9)), rms,
           open_blocks, blocks, st.level_db, st.pilot_db, 100 * cpu / ((double)n / rate));
    if (argc > 4) wav(argv[4], audio, na, ar);
    rf_demod_free(d);
    free(iq);
    free(audio);
    return 0;
}
