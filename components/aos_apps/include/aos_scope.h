/*
 * P4OS - the bench's oscilloscope: a Rigol DS1000Z (DS1054Z, DS1074Z,
 * DS1104Z...) over its raw SCPI socket, TCP port 5555 (aos_scope.c).
 *
 * A service with a thread of its own, like Home Assistant's: it holds the
 * connection, reads the instrument's settings, fetches the traces of the
 * channels that are on (screen mode, ~1200 points, BYTE format, converted
 * with the preamble) while someone watches, and keeps the automatic
 * measurements of those channels fresh about once a second whether anyone
 * watches or not (the Registrador samples them). Commands queue and go out
 * between reads; nothing here blocks the caller.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AOS_SCOPE_CHANNELS 4
#define AOS_SCOPE_POINTS   1200

typedef enum {
    AOS_SCOPE_M_VPP = 0, AOS_SCOPE_M_VMAX, AOS_SCOPE_M_VMIN, AOS_SCOPE_M_VAVG, AOS_SCOPE_M_VRMS,
    AOS_SCOPE_M_FREQ, AOS_SCOPE_M_PERIOD, AOS_SCOPE_M_COUNT
} aos_scope_measure_t;

typedef struct {
    bool  on;
    float scale;                    /* V/div */
    float offset;                   /* V */
    float probe;                    /* x1, x10 */
    char  coupling[6];              /* "DC", "AC", "GND" */
} aos_scope_chan_t;

typedef struct {
    bool     connected;
    bool     connecting;
    char     host[64];
    char     idn[96];               /* "RIGOL TECHNOLOGIES,DS1104Z,DS1ZA...,00.04.04" */
    char     error[96];             /* the last reason it could not connect, for the user */
    char     trig_status[8];        /* "TD", "WAIT", "RUN", "AUTO", "STOP" */
    float    timebase;              /* s/div */
    float    trig_level;            /* V */
    int      trig_source;           /* 1..4 */
    char     trig_slope[6];         /* "POS", "NEG", "RFAL" */
    aos_scope_chan_t ch[AOS_SCOPE_CHANNELS];
    uint32_t wave_seq;              /* bumps with every new set of traces */
    float    fps;                   /* trace sets per second, lately */
} aos_scope_status_t;

void aos_scope_start(void);                                  /* the service's thread; idempotent */
void aos_scope_connect(const char *host, int port);          /* port 0 = 5555; remembered in the prefs */
void aos_scope_disconnect(void);
void aos_scope_status(aos_scope_status_t *out);

/* Fetch traces while true (a screen showing them); measurements go on regardless. */
void aos_scope_live(bool on);
/* Fetch traces for the next ms too, whatever aos_scope_live says (the
 * portal, while a page polls). */
void aos_scope_live_for(uint32_t ms);

/* The latest trace of channel 1..4: up to 'max' points in volts, and the time
 * between points; returns how many, 0 if none. */
int  aos_scope_wave(int ch, float *volts, int max, float *xinc);

/* The latest value of a measurement of channel 1..4, and how old it is.
 * false while there is none (channel off, or the scope's "invalid"). */
bool aos_scope_measure(int ch, aos_scope_measure_t item, float *out, uint32_t *age_ms);

/* A SCPI command, queued: ":RUN", ":CHAN1:SCAL 0.5", ":TIM:MAIN:SCAL 1e-3"... */
bool aos_scope_cmd(const char *scpi);

/* The scope's own screen, as a PNG on the card (<sd>/data/scope.png):
 * request it, and the sequence number changes when a new one is there. */
bool aos_scope_screenshot(void);
const char *aos_scope_screenshot_file(uint32_t *seq);
/* true from aos_scope_screenshot() until that request is over, whether a
 * new picture came of it or not (the link broke, no card to save it on). */
bool aos_scope_screenshot_busy(void);

#ifdef __cplusplus
}
#endif
