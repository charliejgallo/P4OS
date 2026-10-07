/*
 * RF - demodulation: cu8 I/Q at the source's rate in, 48 kHz mono audio out
 * (rf_demod.c). Plain C with no system calls, so it builds and is tested on
 * the Mac too (apps/rf/test/).
 *
 * The listened frequency is the centre of the I/Q: the channel is filtered
 * around 0 Hz, after a DC blocker that takes away the RTL2832's spike
 * there. The source rate must be 48 kHz times a whole number that the
 * chain divides: 240 kHz, or 480 kHz times 2 to 5 (960 k, 1.44 M, 1.92 M,
 * 2.4 M).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { RF_MODE_OFF = 0, RF_MODE_WFM, RF_MODE_AM, RF_MODE_NFM, RF_MODE_COUNT };

#define RF_AUDIO_RATE 48000    /* the highest audio rate (broadcast FM) */

typedef struct rf_demod rf_demod_t;

typedef struct {
    float level_db;         /* the channel's power over the noise floor the demodulator tracks */
    bool  open;             /* the squelch lets the audio through */
    float pilot_db;         /* WFM: the 19 kHz stereo pilot over its neighbours; < 6 means none */
} rf_demod_stats_t;

/* NULL if the rate is not one of those above, or no memory */
rf_demod_t *rf_demod_new(int mode, uint32_t in_rate);
void rf_demod_free(rf_demod_t *d);
int  rf_demod_mode(const rf_demod_t *d);
uint32_t rf_demod_rate(const rf_demod_t *d);
/* the channel's half width in Hz, for drawing it */
uint32_t rf_demod_half_width(int mode);
/* squelch: dB over the noise floor that opens it; 0 is always open */
void rf_demod_set_squelch(rf_demod_t *d, int db);
/* The noise floor of the spectrum (rf_fft_take_db's dB of one bin, of an
 * fft_n-point FFT): the squelch measures the channel over it. Without it
 * the demodulator follows its own quietest moments. */
void rf_demod_set_noise_floor(rf_demod_t *d, float bin_db, int fft_n);
/* De-emphasis of broadcast FM: 75 us (the Americas, Korea) or 50 us
 * (Europe, most of the rest) */
void rf_demod_set_deemph_us(rf_demod_t *d, int us);
/* the audio's rate: 48 kHz for broadcast FM, 24 kHz for AM and narrow FM */
uint32_t rf_demod_audio_rate(const rf_demod_t *d);
/* n I/Q pairs (2n bytes) in; audio samples out at rf_demod_audio_rate(), at
 * most max (n / 5 + 1200 is always enough: the blocks wait for whole ones) */
int  rf_demod_run(rf_demod_t *d, const uint8_t *iq, int n, int16_t *audio, int max);
void rf_demod_stats(const rf_demod_t *d, rf_demod_stats_t *out);
/* where the time goes: a clock in microseconds for every demodulator, then
 * per demodulator the time since the last call in the input conversion,
 * the first decimation, the channel filters, the demodulation and the
 * audio filter; and how many buffers did not fit in internal RAM */
void rf_demod_set_clock(uint64_t (*now_us)(void));
/* true when the filters run on the P4's SIMD (esp-dsp), after a self-test
 * against the C sums; false in the simulator, or if the test failed */
bool rf_demod_simd(void);
const char *rf_demod_simd_note(void);   /* what the self-test found */
void rf_demod_use_simd(bool on);        /* on by default; off to measure C against it */
void rf_demod_profile(rf_demod_t *d, uint32_t out_us[5], int *not_internal);
