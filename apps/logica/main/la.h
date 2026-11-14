/*
 * P4OS - Logic: what the app's files share.
 *
 * A logic analyser for analysers plugged into the board's USB host:
 *
 *   la_fx2.c     the cheap "24 MHz 8 channel" boxes (Cypress FX2LP, sold as
 *                Saleae Logic clones) and the other FX2 boards sigrok knows,
 *                with sigrok's open firmware fx2lafw, which the app loads
 *                into the chip from the card
 *   la_sump.c    anything that speaks SUMP / Open Bench Logic Sniffer over a
 *                serial port, USB or the header's: the app's own firmware
 *                for an ESP32-S3, C3, C6... (firmware/), an OLS, others
 *   la_demo.c    signals made up in memory, to learn and to try the
 *                decoders without hardware
 *
 * A capture is one byte per sample (eight channels) in PSRAM, with two
 * summaries that let the view draw millions of samples per column quickly.
 * The decoders (la_dec*.c) turn it into annotations (a box per byte, bit,
 * field) and packets (a line per transaction, the table); the view
 * (la_view.c) draws both, the page in the portal (web/logica.js) gets them
 * through the live channel (la_live.c), and la_sr.c keeps captures on the
 * card as sigrok sessions (.sr), which PulseView opens.
 *
 * Threads: LVGL's, and one job at a time (a capture, a decode, a file)
 * in a thread of its own that la_job_*() starts and waits for. The capture
 * and the annotations are the job's while it runs: the UI does not read
 * them until LA.busy is false again (it draws the progress instead).
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LA_APP_ID "aos.logica"
#define LA_NCH    8                 /* channels in a sample byte */

/* ---- devices ---- */

enum { LA_DRV_DEMO = 0, LA_DRV_FX2, LA_DRV_SUMP };

typedef struct {
    uint8_t  drv;
    char     name[48];              /* "Saleae Logic (clon)", "ESP32-S3 (SUMP)" */
    char     detail[64];            /* how it is reached, its firmware */
    char     port[16];              /* SUMP: aos_io port, "usb0", "uart.a" */
    uint32_t baud;                  /* SUMP on a UART */
    uint8_t  usb_addr;              /* FX2 */
    uint16_t vid, pid;
    int8_t   prof;                  /* FX2: index in la_fx2.c's table */
    bool     ready;                 /* FX2: fx2lafw runs in it already */
    uint32_t max_rate;              /* Hz */
    uint32_t max_samples;           /* 0: streams, as many as fit in PSRAM */
    uint8_t  channels;
} la_dev_t;

#define LA_DEVS_MAX 8

/* ---- what to capture ---- */

typedef enum { LA_T_X = 0, LA_T_0, LA_T_1, LA_T_RISE, LA_T_FALL, LA_T_EDGE, LA_T_COUNT } la_trig_t;

typedef struct {
    uint32_t rate;                  /* Hz */
    uint32_t samples;
    uint8_t  pre;                   /* percent of the samples before the trigger */
    uint8_t  trig[LA_NCH];          /* la_trig_t per channel: all of them at once (AND) */
    bool     repeat;                /* capture again when one ends */
    uint8_t  demo;                  /* the demo's scene */
} la_cfg_t;

/* ---- a capture ---- */

#define LA_SUM1 64                  /* samples per first-level summary */
#define LA_SUM2 4096                /* per second-level */

typedef struct {
    uint8_t *buf;                   /* PSRAM, n samples */
    uint32_t n, cap;
    uint32_t rate;
    int32_t  trig;                  /* the trigger's sample, -1 none */
    uint8_t  nch;                   /* channels that mean something */
    uint8_t *s1, *s2;               /* (OR, AND) pairs per LA_SUM1 / LA_SUM2 samples */
    uint32_t seq;                   /* new capture or file: the views start over */
    uint32_t lost;                  /* bytes the USB dropped: the capture has gaps */
    bool     stalled;               /* the analyser stopped sending before the end (FX2) */
    char     src[48];               /* the device, or the file it came from */
} la_cap_t;

/* ---- decoders ---- */

enum {
    LA_DEC_UART = 0, LA_DEC_I2C, LA_DEC_SPI, LA_DEC_1WIRE, LA_DEC_CAN, LA_DEC_WS2812,
    LA_DEC_NEC, LA_DEC_DMX, LA_DEC_MODBUS, LA_DEC_PAR, LA_DEC_COUNT
};
#define LA_DEC_MAX 6                /* decoders at a time */
#define LA_DEC_PINS 9                /* the parallel bus: a clock and eight lines */

enum { LA_FMT_HEX = 0, LA_FMT_DEC, LA_FMT_ASCII, LA_FMT_BIN, LA_FMT_COUNT };

typedef struct {
    uint8_t  type;
    bool     on;
    int8_t   ch[LA_DEC_PINS];       /* the pins' channels (la_dec_pin_name), -1 none */
    uint32_t baud;                  /* UART, Modbus: 0 = guess it; CAN: bit/s */
    uint8_t  bits;                  /* UART data bits 5..9; SPI word bits 4..16 */
    char     parity;                /* 'N' 'E' 'O' */
    uint8_t  stop;                  /* 1 or 2 */
    uint8_t  mode;                  /* SPI 0..3 */
    bool     lsb;                   /* SPI: least significant bit first */
    bool     inv;                   /* UART: inverted line; SPI: CS active high */
    uint8_t  fmt;                   /* LA_FMT_ */
} la_dec_cfg_t;

/* the colour of an annotation */
enum { LA_K_DATA = 0, LA_K_ADDR, LA_K_START, LA_K_ACK, LA_K_ERR, LA_K_CTRL, LA_K_INFO, LA_K_COUNT };

typedef struct {
    uint32_t s0, s1;                /* samples */
    uint8_t  row;                   /* the decoder's slot */
    uint8_t  kind;                  /* LA_K_ */
    char     t[22];
} la_ann_t;                         /* 32 bytes */

typedef struct {
    uint32_t s0, s1;
    uint8_t  row;
    uint8_t  err;
    char     t[86];
} la_pkt_t;                         /* 96 bytes */

#define LA_ANN_MAX 65536
#define LA_PKT_MAX 8192

typedef struct {
    la_ann_t *ann;                  /* PSRAM; grouped by row, each row in time order */
    uint32_t nann;
    uint32_t row_first[LA_DEC_MAX], row_n[LA_DEC_MAX];
    la_pkt_t *pkt;
    uint32_t npkt;
    bool     full;                  /* ran out of room: the end is not decoded */
    char     summary[LA_DEC_MAX][64];   /* "UART 115200 8N1 (adivinado)", per row */
    uint32_t seq;                   /* bumped by every decode */
} la_anns_t;

/* ---- the app's state ---- */

enum { LA_ST_IDLE = 0, LA_ST_CONNECT, LA_ST_WAIT, LA_ST_CAPTURE, LA_ST_DECODE, LA_ST_FILE };

typedef struct {
    la_cfg_t     cfg;
    la_dec_cfg_t dec[LA_DEC_MAX];
    char         chname[LA_NCH][12];
    bool         chshow[LA_NCH];

    la_dev_t     devs[LA_DEVS_MAX];
    int          ndevs;
    int          dev;               /* in devs[] */

    la_cap_t     cap;               /* what the views show */
    la_cap_t     nxt;               /* where a capture or a file goes while cap stays in view */
    la_anns_t    an;
    bool         cap_ok, an_ok;     /* the UI may draw them (they change under la_lock) */

    /* the job (shared with its thread: guarded by la_lock) */
    volatile bool busy;             /* a job runs */
    volatile bool stop;             /* asked to stop */
    int          state;             /* LA_ST_ */
    uint32_t     prog, prog_total;  /* samples so far / to go */
    char         msg[96];           /* the last job's result or problem */
    bool         msg_err;
    bool         again;             /* repeat: start another capture when this one is drawn */
    bool         decode_after;      /* the capture job decodes too */
    uint32_t     job_done;          /* bumped when a job ends: the UI picks it up */
} la_t;

extern la_t LA;

void la_lock(void);
void la_unlock(void);
void la_set_msg(bool err, const char *fmt, ...);
void *la_big_alloc(size_t n);       /* PSRAM on the board, NULL if it does not fit */
void la_big_free(void *p);
uint32_t la_ms(void);
const char *la_dir(void);           /* <card>/logica, made if missing; NULL without a card */
void la_cfg_save(void);
bool la_cfg_line(const char *line);               /* one key=value of the settings */
int  la_cfg_text(char *out, size_t n);            /* all of them */
bool la_capture_ui(void);                         /* the Capture button (and the repeat after it) */
void la_capture_cancel_repeat(void);

/* la_cap.c */
bool     la_cap_alloc(la_cap_t *c, uint32_t n);   /* buf for n samples (keeps it if big enough) */
void     la_cap_free(la_cap_t *c);
void     la_cap_summarise(la_cap_t *c);           /* after buf/n changed */
/* OR and AND of the samples [a, b), and the first and last of them */
void     la_cap_range(const la_cap_t *c, uint32_t a, uint32_t b, uint8_t *or_, uint8_t *and_);
/* the first sample >= from where the trigger holds (prev = the sample before), -1 none */
int32_t  la_trig_find(const uint8_t *trig, const uint8_t *buf, uint32_t n, uint32_t from, uint8_t prev);
bool     la_trig_any(const uint8_t *trig);
/* next sample > from where channel ch changes, or n */
uint32_t la_next_edge(const la_cap_t *c, int ch, uint32_t from, uint32_t to);
uint32_t la_max_samples(void);                    /* what PSRAM allows now */
void     la_rotate(uint8_t *buf, uint32_t n, uint32_t first);   /* buf[first] becomes buf[0] */

/* measurements of a channel over [a, b) */
typedef struct {
    uint32_t rises, falls;
    float    freq;                  /* Hz, from the whole periods; 0 if fewer than two rises */
    float    duty;                  /* 0..1 */
    float    hi_min, hi_max, lo_min, lo_max;   /* s, whole pulses only; 0 none */
    float    hi_avg, lo_avg;
    float    high_frac;             /* time high / time */
} la_meas_t;
void la_measure(const la_cap_t *c, int ch, uint32_t a, uint32_t b, la_meas_t *m);

/* la_dev.c: the devices and the capture job */
void        la_devs_scan(bool probe_serial);      /* fills LA.devs (UI thread; probing talks) */
const char *la_drv_name(int drv);
const uint32_t *la_rates(const la_dev_t *d, int *n);
bool        la_capture_start(void);               /* false: busy or nothing to capture with */
void        la_job_stop(void);                    /* asks the job to stop */
void        la_job_wait(void);                    /* waits for the job (destroy) */
bool        la_job_start(int state, void (*fn)(void *), void *arg);
la_cap_t   *la_job_target(uint32_t n);            /* where a job puts a new capture of n samples */
void        la_job_commit(la_cap_t *t, bool ok);  /* it is ready (or failed): the views get it */
bool        la_load_start(const char *path);
bool        la_save_start(const char *path);
bool        la_decode_start(void);
bool        la_probe_start(void);                 /* asks the serial ports for SUMP */
extern volatile bool la_rescan;                   /* a job asks the UI to list the devices again */
extern volatile int la_prefer;                    /* ...and to choose this driver's device */

/* the drivers: each runs inside the capture job, filling LA.cap */
int  la_fx2_find(la_dev_t *out, int max);
const char *la_fx2_capture(const la_dev_t *d, const la_cfg_t *cfg, la_cap_t *c);   /* NULL ok, else why */
const char *la_fx2_fw_dir(void);
int  la_sump_find(la_dev_t *out, int max, bool probe);
bool la_sump_probe(const char *port, uint32_t baud, la_dev_t *out);
const char *la_sump_capture(const la_dev_t *d, const la_cfg_t *cfg, la_cap_t *c);
const char *la_demo_capture(const la_cfg_t *cfg, la_cap_t *c);
const char *la_demo_scene_name(int i);
int  la_demo_scenes(void);
void la_demo_decoders(int scene);                 /* sets LA.dec and names for the scene */

/* la_dec.c */
const char *la_dec_name(int type);
int         la_dec_npins(int type);
const char *la_dec_pin_name(int type, int pin);
bool        la_dec_pin_needed(int type, int pin);
void        la_dec_defaults(la_dec_cfg_t *d, int type);
void        la_decode_all(void);                  /* in a job: LA.cap -> LA.an */
bool        la_anns_alloc(void);
void        la_anns_free(void);
void        la_dec_describe(const la_dec_cfg_t *d, char *out, size_t n);
uint32_t    la_ann_first(int row, uint32_t s);    /* first annotation of row ending after s */

/* la_sr.c: sigrok sessions on the card */
const char *la_sr_save(const char *path);         /* in a job */
const char *la_sr_load(const char *path, la_cap_t *c);   /* in a job */
bool        la_csv_save(const char *path);        /* the packets */
extern bool la_sr_nomem;                          /* the last load failed for want of memory */
uint32_t    la_crc32(uint32_t crc, const void *p, size_t n);

/* la_view.c */
void la_view_create(lv_obj_t *parent, int32_t w, int32_t h);
void la_view_dirty(void);
void la_view_tick(void);                          /* from the app's timer */
void la_view_fit(void);
void la_view_zoom(float f);
void la_view_to_trigger(void);
void la_view_reset(void);
void la_view_goto(uint32_t s0, uint32_t s1);      /* show this range */
bool la_view_cursors(uint32_t *a, uint32_t *b);   /* both placed */
void la_view_cursors_clear(void);
void la_view_visible(uint32_t *a, uint32_t *b);

/* la_ui.c: the bar, the sheets */
void la_ui_build(lv_obj_t *root);
void la_ui_status(void);
void la_ui_after_job(void);
lv_obj_t *la_sheet_open(const char *title);
void la_sheet_close(void);
bool la_sheet_is_open(void);
void la_ui_channel_sheet(int ch);
void la_ui_decoder_sheet(int slot);
void la_ui_packet_toast(uint32_t sample, int row);

/* la_live.c: the portal's page */
void la_live_tick(void);
void la_live_clear(void);

/* la_fmt.c: numbers for people */
void la_fmt_time(float s, char *out, size_t n);   /* "12.5 µs" */
void la_fmt_freq(float hz, char *out, size_t n);  /* "1.25 MHz" */
void la_fmt_rate(uint32_t hz, char *out, size_t n);
void la_fmt_count(uint32_t n, char *out, size_t cap);   /* "1M", "250k" */
void la_fmt_value(uint32_t v, int bits, int fmt, char *out, size_t n);
