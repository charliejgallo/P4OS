/*
 * RF - the LoRa meter (rf_lora.c): packets found in the band, timed, and
 * their bandwidth and spreading factor read from their preamble. Plain C,
 * tested on the Mac too (apps/rf/test/).
 *
 * It does not decode what a packet says; it tells that one was sent, where,
 * how strong, for how long, and with which settings - so the busy share of
 * a channel, and which Meshtastic preset or LoRaWAN data rate is on it,
 * whatever the region.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t start;             /* the burst's first sample, counted from the start */
    uint32_t dur_us;            /* time on the air */
    int32_t offset_hz;          /* its centre, from the tuner's */
    uint32_t width_hz;          /* how wide it showed */
    float snr_db;               /* in its own width */
    int sf;                     /* 5..12, 0 if no chirp was found */
    uint32_t bw_hz;             /* the LoRa bandwidth its chirps match (0: none) */
    float quality;              /* how much of a symbol's energy the dechirp gathered, 0..1 */
} rf_lora_pkt_t;

typedef struct rf_lora rf_lora_t;
rf_lora_t *rf_lora_new(uint32_t rate);
void rf_lora_free(rf_lora_t *o);

/* n cu8 pairs in; calls done(pkt, ctx) for each packet once it ended and
 * was looked at. Cheap: one 256-point FFT every 2 ms. */
void rf_lora_feed(rf_lora_t *o, const uint8_t *iq, int n, void (*done)(const rf_lora_pkt_t *, void *), void *ctx);

/* The slow part: the dechirp of a preamble the feed set aside (tens of ms
 * of work for a packet). From another thread (the app's), or right after
 * the feed (the tests). Returns true if there was one to do. */
bool rf_lora_analyse_pending(rf_lora_t *o);

/* The Meshtastic preset with this bandwidth and spreading factor, or NULL */
const char *rf_lora_preset(int sf, uint32_t bw_hz);

/* rf_rec.c: a line in rf/lora-<day>.csv */
void rf_log_lora(const rf_lora_pkt_t *p, uint32_t freq_hz, const char *preset);
