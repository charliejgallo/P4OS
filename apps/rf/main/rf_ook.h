/*
 * RF - on-off keying: the pulses of a 433/868 MHz remote or sensor out of
 * I/Q, and what they say (rf_ook.c). Plain C, tested on the Mac too
 * (apps/rf/test/).
 *
 * The pulse train is the meeting point of every receiver the app will
 * have: the RTL-SDR's I/Q gives it here, a CC1101's demodulated output (its
 * GDO0 pin through the RMT) will give the same, and the decoders only ever
 * see pulses.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RF_PULSES_MAX 600

/* One transmission: mark (carrier on) and space (off) lengths in
 * microseconds, alternating from a mark; the last space is the gap that
 * ended it. */
typedef struct {
    int n;                          /* marks */
    uint16_t mark[RF_PULSES_MAX], space[RF_PULSES_MAX];
    float snr_db;                   /* the marks' level over the noise */
    int32_t offset_hz;              /* where in the band it was, from the centre */
} rf_pulses_t;

/* ---- the detector: I/Q in, pulse trains out ---- */
typedef struct rf_ook rf_ook_t;
rf_ook_t *rf_ook_new(uint32_t rate);
void rf_ook_free(rf_ook_t *o);
/* n cu8 pairs in; calls done(pulses, ctx) for every finished transmission */
void rf_ook_feed(rf_ook_t *o, const uint8_t *iq, int n, void (*done)(const rf_pulses_t *, void *), void *ctx);

/* ---- what a train says ---- */
enum { RF_MOD_UNKNOWN, RF_MOD_PWM, RF_MOD_PPM, RF_MOD_MANCHESTER };

typedef struct rf_decoded_s {
    char proto[24];                 /* "EV1527", "Nexus-TH", ... or "" when only analysed */
    char text[96];                  /* a plain line in English, for the CSV and the tests (the app writes its own) */
    char code[16];                  /* PT2262: the 12 three-state symbols */
    char key[32];                   /* what identifies the sender, to group repeats and name it in MQTT */
    /* decoded values; has_* say which */
    bool has_temp, has_hum, has_batt, has_button;
    float temp_c;
    int hum, button, channel;
    bool batt_low;
    uint32_t id;
    /* the analysis, for every train */
    int mod;                        /* RF_MOD_* */
    int short_us, long_us, gap_us;  /* the two widths it is made of, and the sync gap */
    int nbits;
    uint8_t bits[RF_PULSES_MAX / 8 + 1];
    char hex[2 * (RF_PULSES_MAX / 8 + 1) + 1];
} rf_decoded_t;

/* Analyses a train and tries the decoders. false if it is too short or too
 * irregular to be anything (noise). */
bool rf_ook_decode(const rf_pulses_t *p, rf_decoded_t *out);

/* rf_rec.c: a line in rf/datos-<day>.csv, and a message over MQTT */
void rf_log_decoded(const rf_decoded_t *d, uint32_t freq_hz, float snr_db, int32_t offset_hz);
bool rf_mqtt_ready(void);
void rf_mqtt_decoded(const rf_decoded_t *d, uint32_t freq_hz, float snr_db);
