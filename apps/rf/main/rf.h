/*
 * RF - software radios and transceivers for P4OS (apps/rf/README.md).
 *
 * The app is built on sources: a source gives 8-bit I/Q samples (cu8, the
 * RTL2832's own format) at a sample rate around a centre frequency, and
 * the app does the rest (spectrum, waterfall; demodulation later). The
 * RTL-SDR on the USB host is the first one; an rtl_tcp server, another SDR
 * on USB or a recording on the card are more rf_src_ops_t, not another app.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- the spectrum (rf_dsp.c) ---- */
typedef struct rf_fft rf_fft_t;
rf_fft_t *rf_fft_new(int n);            /* n rounded up to a power of two */
void rf_fft_free(rf_fft_t *f);
int  rf_fft_size(const rf_fft_t *f);
void rf_fft_add_cu8(rf_fft_t *f, const uint8_t *iq);   /* n I/Q pairs (2n bytes) into the sum */
void rf_fft_use_simd(bool on);          /* the board's 16-bit SIMD FFT (default) or float, to compare */
int  rf_fft_count(const rf_fft_t *f);
void rf_fft_take_db(rf_fft_t *f, float *out);         /* n bins, lowest frequency first; resets */
/* The noise floor under the bins of an average of navg FFTs: their 20th
 * percentile, which sits below the noise's mean by an amount that depends
 * on navg (chi-squared), corrected for */
float rf_fft_floor_db(const float *db, int n, int navg);

/* ---- sources ---- */
typedef struct rf_src rf_src_t;
#define RF_GAINS_MAX 40
typedef struct {
    const char *kind;           /* "RTL-SDR" */
    char name[64];              /* the device's own name */
    char tuner[24];             /* "R820T" */
    uint32_t fmin, fmax;        /* Hz the tuner covers */
    int ngains;
    int gains[RF_GAINS_MAX];    /* tenths of a dB, lowest first */
    bool high_speed;            /* on a High-Speed port; Full Speed carries ~0.3 Msps at most */
} rf_src_info_t;

typedef struct rf_src_ops {
    const char *kind;
    /* the first device of this kind, opened; NULL and a reason (a key for
     * _()) when there is none or it cannot be opened */
    rf_src_t *(*open)(const char **why);
    void (*info)(rf_src_t *s, rf_src_info_t *out);
    bool (*set_freq)(rf_src_t *s, uint32_t hz);
    uint32_t (*set_rate)(rf_src_t *s, uint32_t sps);    /* the rate it got, 0 if refused */
    bool (*set_gain)(rf_src_t *s, int tenth_db);        /* < 0: automatic */
    bool (*start)(rf_src_t *s);
    /* cu8 I/Q, an even count of bytes; 0 on timeout, < 0 once the device is gone */
    int  (*read)(rf_src_t *s, uint8_t *iq, int bytes, int timeout_ms);
    void (*stats)(rf_src_t *s, uint64_t *bytes, uint32_t *dropped);
    void (*stop)(rf_src_t *s);
    void (*close)(rf_src_t *s);
} rf_src_ops_t;

struct rf_src {
    const rf_src_ops_t *ops;
};

extern const rf_src_ops_t rf_src_rtl;
extern const rf_src_ops_t rf_src_file;   /* the simulator's RF_IQ_FILE (development) */
/* a recording on the card as a source (rf_src_file.c): cu8 at its rate,
 * in real time, over and over; NULL if it cannot be read */
rf_src_t *rf_src_file_open(const char *path, uint32_t rate, uint32_t freq_hz);

/* ---- what the app keeps (rf_rec.c), all from the engine's thread ---- */
struct rf_decoded_s;
bool rf_wav_start(uint32_t rate, uint32_t freq_hz);         /* to the recorder's folder */
void rf_wav_write(const int16_t *pcm, int n);
bool rf_wav_on(uint32_t *samples, uint32_t *rate);
void rf_wav_stop(void);
bool rf_iq_start(uint32_t rate, uint32_t freq_hz, int gain);   /* rf/iq/<when>_<hz>_<sps>.cu8 */
void rf_iq_write(const uint8_t *iq, int bytes);
bool rf_iq_on(uint64_t *written, uint32_t *dropped);
void rf_iq_stop(void);
const char *rf_rec_last_path(bool iq);
