/*
 * RF - software radios for P4OS (apps/rf/README.md).
 *
 * The first screen is the spectrum and its waterfall around a centre
 * frequency, from an RTL-SDR on the USB host: drag to tune, tap to go to a
 * frequency, the number on top to type one, and a row of bands.
 *
 * Two halves that meet in memory:
 *   - the worker (core 0) owns the source: opens it, applies what the UI
 *     asks (frequency, rate, gain: each a few dozen USB transfers), reads
 *     every sample off the USB ring and turns some of them into spectra:
 *     RF_FPS a second, each the average of RF_AVG FFTs;
 *   - LVGL's timer draws the newest spectrum into the trace and a row of
 *     the waterfall, and the status line.
 *
 * The waterfall is an image over a ring of 2 x H rows: each new row is
 * written twice, at r and r + H, and the image shows H rows from r, so it
 * scrolls by moving a pointer instead of 800 KB.
 */
#include "rf.h"
#include "rf_demod.h"
#include "rf_ook.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <dirent.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

extern void lv_image_cache_drop(const void *src);

#define RF_FFT      2048
#define RF_FPS      25
#define RF_AVG      4
#define READ_BYTES  (32 * 1024)
#define TICKS_MAX   12
#define SNAP_PX     36          /* how far from a tap a signal is looked for */

/* A radio mast with waves on both sides, on a blue to violet tile. */
static const uint8_t RF_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 16, 7, 46, 3, AIC_C_TEXT, 255),                       /* mast  */
    AIC_RECT(AIC_CENTER, 0, 40, 30, 7, 3, AIC_C_TEXT, 255),                       /* foot  */
    AIC_RECT(AIC_CENTER, 0, -12, 14, 14, AIC_CIRCLE, AIC_C_TEXT, 255),            /* tip   */
    AIC_ARC(AIC_CENTER, 0, -12, 36, 0, 5, 0, 360, 0, 70, -35, AIC_C_BG, 0, AIC_C_TEXT, 230),
    AIC_ARC(AIC_CENTER, 0, -12, 36, 0, 5, 0, 360, 0, 70, 145, AIC_C_BG, 0, AIC_C_TEXT, 230),
    AIC_ARC(AIC_CENTER, 0, -12, 62, 0, 5, 0, 360, 0, 70, -35, AIC_C_BG, 0, AIC_C_TEXT, 150),
    AIC_ARC(AIC_CENTER, 0, -12, 62, 0, 5, 0, 360, 0, 70, 145, AIC_C_BG, 0, AIC_C_TEXT, 150),
    AIC_END
};

enum { ST_SEARCH, ST_NONE, ST_RUN, ST_LOST };

typedef struct {
    const char *name;
    uint32_t hz, step;
    int mode;
} band_t;

/* centre, step and how to listen of common bands; anyone's, not a list of
 * stations. 433 MHz also carries LPD handhelds (narrow FM); 868 and ADS-B
 * are data, only seen. */
/* the app's modes: the demodulator's, then data (on-off keying, rf_ook.c) */
#define MODE_DATA RF_MODE_COUNT
#define MODES_N   (RF_MODE_COUNT + 1)

static const band_t BANDS[] = {
    { N_("FM"),     98000000,  100000,  RF_MODE_WFM },
    { N_("Aire"),   125000000, 25000,   RF_MODE_AM },
    { N_("Marina"), 156800000, 25000,   RF_MODE_NFM },
    { "2 m",        145000000, 12500,   RF_MODE_NFM },
    { "433",        433920000, 25000,   MODE_DATA },
    { "70 cm",      435000000, 12500,   RF_MODE_NFM },
    { "PMR",        446006250, 12500,   RF_MODE_NFM },
    { "868",        868300000, 25000,   MODE_DATA },
    { "ADS-B",      1090000000, 1000000, RF_MODE_OFF },
};
static const char *const MODES[MODES_N] = { N_("Sin audio"), N_("FM"), N_("AM"), N_("FM angosta"), N_("Datos") };
static const uint32_t MODE_STEP[MODES_N] = { 0, 100000, 25000, 12500, 25000 };
static const uint32_t STEPS[] = { 1000, 5000, 6250, 8330, 10000, 12500, 25000, 100000, 1000000 };
/* 48 kHz times a number the demodulator divides (rf_demod.h) */
static const uint32_t RATES_HS[] = { 960000, 1440000, 1920000, 2400000 };
static const uint32_t RATES_FS[] = { 240000 };
/* While listening: 240 k (no stage before the channel: a fifth of the
 * work), or the rates whose first stage divides by an even number, which
 * the board's SIMD filters (rf_demod.c). Measured on the board for
 * broadcast FM at 960 k: ~60 % of core 0 with the spectrum. */
static const uint32_t RATES_LISTEN[] = { 240000, 960000, 1920000 };

static uint32_t listen_rate(uint32_t r)
{
    return r >= 1440000 ? 1920000 : r >= 900000 ? 960000 : 240000;
}

typedef struct {
    aos_app_t *self;
    lv_obj_t *root;
    int W, H;

    /* the worker's side; written by one side, read by the other */
    volatile int state;
    const char *volatile why;
    volatile uint32_t want_freq, want_rate;
    volatile int want_gain;                 /* tenths of dB, -1 automatic */
    volatile uint32_t freq, rate;           /* what the source has */
    volatile bool paused;                   /* hidden or locked, with no audio */
    bool hidden;
    uint32_t shown_rate;
    volatile bool stop, done;               /* the engine's thread */
    volatile int want_mode, want_sq;        /* RF_MODE_*, squelch dB (0 off) */
    volatile float level_db, pilot_db;      /* the demodulator's, for the status line */
    volatile float floor_bin_db;            /* the spectrum's noise floor, per bin */
    volatile bool sq_open, listening;
    volatile uint32_t info_seq, spec_seq;
    rf_src_info_t info;
    void *mx;
    float *spec;                            /* RF_FFT, under mx */
    volatile uint64_t bytes;
    volatile uint32_t dropped;
    volatile unsigned ui_frames, ui_draw_us;    /* the UI's drawing, for the worker's log */

    /* the UI's */
    uint32_t step;
    uint32_t seen_info, seen_spec;
    float *ui_spec;                         /* RF_FFT copy */
    float *col, *smooth;                    /* W */
    float floor_db;
    bool floor_set;
    lv_obj_t *freq_lbl, *status_lbl, *rate_btn, *gain_btn, *step_btn, *sq_btn;
    lv_obj_t *mode_btn[MODES_N];
    int band_px;                            /* the channel's half width on screen */
    uint16_t c_band, c_bandgrid;
    lv_obj_t *spec_img, *wf_img, *empty, *empty_lbl;
    lv_obj_t *tick_lbl[TICKS_MAX];
    int16_t tick_x[TICKS_MAX];
    uint32_t tick_f[TICKS_MAX];             /* each label's frequency: a tap tunes there */
    int nticks;
    lv_image_dsc_t spec_dsc, wf_dsc;
    bool blit_ok;                           /* straight to the panel (not in the simulator) */
    int16_t *yt, *yd;                       /* W: the trace's top now, and as drawn */
    uint8_t *vgrid, *hgrid;                 /* W, SH */
    uint16_t *c_fill;                       /* SH: the fill's colour per row */
    uint16_t c_bg, c_grid, c_line, c_mark, c_markfill;
    float drawn_ref;
    unsigned ticks_gen, drawn_ticks;
    bool drawn_once;
    uint16_t *spec_px, *wf_px;
    int SH, WH, wf_row;
    uint16_t lut[256];
    lv_timer_t *timer;
    uint64_t stat_at, stat_bytes;
    /* dragging the spectrum */
    bool dragging, moved;
    int32_t drag_x0;
    uint32_t drag_f0;
    /* the frequency sheet */
    lv_obj_t *sheet, *ta;
    lv_obj_t *gain_slider, *gain_val, *sq_slider, *sq_val, *sq_live;
    /* rf/control.txt on the card: what the portal (or anyone) asks for */
    long ctl_mtime, ctl_size;
    /* data: the transmissions decoded, newest last (a ring); the engine
     * fills it under mx, the UI lists it */
    struct rf_event *ev;
    volatile uint32_t ev_n, ev_seq;
    uint32_t seen_ev, live_ev;
    bool log_on, mqtt_on;
    lv_obj_t *ev_list, *ev_empty;
    lv_obj_t *ev_title[40];                 /* the rows' first lines, newest first */
    uint32_t ev_shown_n;                    /* ev_n when the rows were made */
    /* keeping things (rf_rec.c): asked by the UI, done by the engine */
    volatile bool rec_iq, rec_wav;
    lv_obj_t *rec_btn;
    /* the speaker: the board's volume, and a mute of the radio's own */
    lv_obj_t *vol_btn, *vol_val, *vol_slider, *mute_btn;
    volatile bool muted;
    int vol_shown;
    char play_path[160];
    volatile bool play_req, play_stop, playing;
    volatile uint32_t play_rate, play_freq;
} rf_t;

#define EV_MAX 64
typedef struct rf_event {
    rf_decoded_t d;
    uint32_t freq;
    float snr;
    int32_t off;
    uint64_t t_last;                        /* uptime ms of its last repeat */
    struct tm when;
    int count;
} rf_event_t;

static rf_t *A;

static void mode_view(rf_t *a);
static void ev_rebuild(rf_t *a);
static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud);
static lv_obj_t *button_f(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud, const lv_font_t *font,
                          int pad);

static void *psram(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n ? n : 1);
#else
    return malloc(n ? n : 1);
#endif
}

/* ---- the engine ----------------------------------------------------------- */

/* Audio to the board's speaker, keeping between 50 and 200 ms in its ring.
 * The stick's clock and the audio's are two crystals, so the ring drifts:
 * a sample is dropped or doubled now and then. And when the engine falls
 * behind and catches up it makes audio faster than real time: past 300 ms
 * the block goes (a click, once) - a ring left full is a second of delay
 * and every sample after it lost (seen on the board, 2026-10-07). */
static void audio_out(int16_t *pcm, int n, uint32_t rate)
{
    if (n <= 0) return;
    if (A && A->muted) memset(pcm, 0, (size_t)n * sizeof *pcm);   /* the ring keeps its pace */
    int q = aos_hal_spk_queued();
    if (q > (int)rate * 3 / 10) return;
    if (q > (int)rate / 5 && n > 2) n--;
    else if (q < (int)rate / 20 && n > 2) {
        pcm[n] = pcm[n - 1];
        n++;
    }
    aos_hal_spk_write(pcm, n);
    /* development: the simulator's speaker plays nothing, so RF_WAV_OUT
     * keeps what it would have played, as raw 48 kHz 16-bit mono (on the
     * board getenv() is always NULL) */
    static FILE *dump;
    static bool tried;
    if (!tried) {
        tried = true;
        const char *p = getenv("RF_WAV_OUT");
        if (p && *p) dump = fopen(p, "wb");
    }
    if (dump) {
        fwrite(pcm, sizeof(int16_t), n, dump);
        fflush(dump);
    }
}

/* A transmission the detector finished (the engine's thread): decoded,
 * folded into the last event if it is a repeat of it (the same sender
 * within 2 s: remotes send 4 to 10 copies), else a new one - which goes to
 * the day's CSV and over MQTT when those are on. */
static void on_pulses(const rf_pulses_t *p, void *ctx)
{
    rf_t *a = ctx;
    static rf_decoded_t d;
    if (!rf_ook_decode(p, &d)) return;
    if (!d.proto[0] && d.nbits < 8) return;     /* not even a code: leave it */
    uint64_t now = aos_hal_uptime_ms();
    uint32_t freq = (uint32_t)((int64_t)a->freq + p->offset_hz);
    aos_hal_mutex_lock(a->mx);
    for (uint32_t k = 0; k < 4 && k < a->ev_n; k++) {
        rf_event_t *e = &a->ev[(a->ev_n - 1 - k) % EV_MAX];
        if (!strcmp(e->d.key, d.key) && now - e->t_last <= 2000) {
            e->count++;
            e->t_last = now;
            if (p->snr_db > e->snr) e->snr = p->snr_db;
            a->ev_seq++;
            aos_hal_mutex_unlock(a->mx);
            return;
        }
    }
    rf_event_t *e = &a->ev[a->ev_n % EV_MAX];
    e->d = d;
    e->freq = freq;
    e->snr = p->snr_db;
    e->off = p->offset_hz;
    e->t_last = now;
    e->count = 1;
    aos_hal_time_now(&e->when);
    a->ev_n++;
    a->ev_seq++;
    aos_hal_mutex_unlock(a->mx);
    if (a->log_on) rf_log_decoded(&d, freq, p->snr_db, p->offset_hz);
    if (a->mqtt_on) rf_mqtt_decoded(&d, freq, p->snr_db);
}

/* The engine: a thread of its own (not the system's one worker), so the
 * radio keeps sounding with the screen locked or another app in front. */
static void engine(void *arg)
{
    rf_t *a = arg;
    uint8_t *buf = psram(READ_BYTES), *frame = psram(RF_FFT * 2);
    float *db = psram(RF_FFT * sizeof(float));
    rf_fft_t *fft = rf_fft_new(RF_FFT);
    /* a read's audio (16 K pairs at 240 kHz: 3277 samples) or the 100 ms
     * cushion written when the speaker opens, whichever is more */
    enum { PCM_MAX = (READ_BYTES / 2 / 5 + 64) > RF_AUDIO_RATE / 10 ? (READ_BYTES / 2 / 5 + 64) : RF_AUDIO_RATE / 10 };
    int16_t *pcm = psram((PCM_MAX + 2) * sizeof(int16_t));
    rf_demod_t *dm = NULL;
    rf_ook_t *ook = NULL;
    uint32_t ook_rate = 0;
    int cur_mode = RF_MODE_OFF;
    uint32_t src_rate = 0, dm_tried = 0;   /* the rate the source runs at; the last one a demodulator was tried at */
    rf_demod_set_clock(aos_hal_uptime_us);
    bool spk = false;
    uint32_t spk_rate = 0;
    uint64_t t_dem = 0;
    if (!buf || !frame || !db || !fft || !pcm) {
        a->why = N_("Sin memoria");
        a->state = ST_NONE;
        goto out;
    }
    rf_src_t *src = NULL;
    bool started = false;
    uint32_t cur_freq = 0, cur_rate = 0;
    int cur_gain = -2, fill = 0;
    uint64_t since = 0, next_try = 0;
    /* where the worker's time goes, logged every 10 s (docs/plan/RF.md) */
    uint64_t t_read = 0, t_fft = 0, t_take = 0, t_log = aos_hal_uptime_us();
    unsigned n_fft = 0;
    while (!a->stop) {
        if (a->paused) {
            if (src && started) {
                src->ops->stop(src);
                started = false;
            }
            if (spk) aos_hal_spk_close();
            spk = false;
            aos_hal_sleep_ms(100);
            continue;
        }
        /* a recording to play back, or back to the radio */
        if (a->play_req || (a->play_stop && a->playing)) {
            if (src) src->ops->close(src);
            src = NULL;
            started = false;
            if (a->play_req) {
                src = rf_src_file_open(a->play_path, a->play_rate, a->play_freq);
                a->playing = src != NULL;
                if (src) {
                    src->ops->info(src, &a->info);
                    cur_freq = cur_rate = 0;
                    cur_gain = -2;
                    a->info_seq++;
                    a->state = ST_RUN;
                    aos_hal_log("rf", "playing %s", a->play_path);
                }
            } else a->playing = false;
            a->play_req = a->play_stop = false;
        }
        if (!src) {
            if (spk) aos_hal_spk_close();
            spk = false;
            if (aos_hal_uptime_ms() < next_try) {
                aos_hal_sleep_ms(100);
                continue;
            }
            const char *why = NULL;
            src = rf_src_file.open(&why);
            if (!src) src = rf_src_rtl.open(&why);
            if (!src) {
                a->why = why;
                if (a->state != ST_LOST) a->state = ST_NONE;
                next_try = aos_hal_uptime_ms() + 1000;
                continue;
            }
            src->ops->info(src, &a->info);
            aos_hal_log("rf", "%s %s, tuner %s, %d gains, %s speed", a->info.kind, a->info.name, a->info.tuner,
                        a->info.ngains, a->info.high_speed ? "high" : "full");
            cur_freq = cur_rate = 0;
            cur_gain = -2;
            started = false;
            a->info_seq++;
            a->state = ST_RUN;
        }
        uint32_t want_rate = a->want_rate;
        if (!a->info.high_speed && want_rate > RATES_FS[0]) want_rate = RATES_FS[0];
        else if (a->playing) want_rate = a->play_rate;
        else if (a->want_mode == MODE_DATA) want_rate = 240000;    /* the band around the centre, as rtl_433 */
        else if (a->info.high_speed && a->want_mode != RF_MODE_OFF) want_rate = listen_rate(want_rate);
        if (want_rate != cur_rate) {
            if (started) src->ops->stop(src);
            started = false;
            uint32_t got = src->ops->set_rate(src, want_rate);
            src_rate = got ? got : src_rate;
            /* a recording is of one rate: it ends here */
            if (rf_iq_on(NULL, NULL)) {
                rf_iq_stop();
                a->rec_iq = false;
            }
            a->rate = src_rate;
            cur_rate = want_rate;
            fill = 0;
        }
        uint32_t want_freq = a->want_freq;
        if (want_freq != cur_freq) {
            if (src->ops->set_freq(src, want_freq)) a->freq = want_freq;
            cur_freq = want_freq;
        }
        if (a->want_gain != cur_gain) {
            cur_gain = a->want_gain;
            src->ops->set_gain(src, cur_gain);
        }
        /* the demodulator follows the mode and the rate */
        int want_mode = a->want_mode;
        /* the rate is the source's own (src_rate), never the UI's copy */
        bool audio_mode = want_mode != RF_MODE_OFF && want_mode != MODE_DATA;
        if (want_mode != cur_mode || (dm && rf_demod_rate(dm) != src_rate) || (ook && ook_rate != src_rate) ||
            (!dm && audio_mode && dm_tried != src_rate)) {
            rf_demod_free(dm);
            dm = audio_mode ? rf_demod_new(want_mode, src_rate) : NULL;
            rf_ook_free(ook);
            ook = want_mode == MODE_DATA ? rf_ook_new(src_rate) : NULL;
            ook_rate = src_rate;
            dm_tried = src_rate;
            if (audio_mode && !dm) aos_hal_log("rf", "no demodulator for mode %d at %u sps", want_mode, (unsigned)src_rate);
            if (dm) aos_hal_log("rf", "demodulator: mode %d at %u sps, filters on %s (%s)", want_mode, (unsigned)src_rate,
                                rf_demod_simd() ? "the SIMD (esp-dsp)" : "C", rf_demod_simd_note());
            cur_mode = want_mode;
            a->pilot_db = 0;
        }
        /* broadcast FM needs no squelch: only AM and narrow FM get it */
        if (dm) rf_demod_set_squelch(dm, want_mode == RF_MODE_WFM ? 0 : a->want_sq);
        /* the speaker at the demodulator's rate (48 k broadcast, 24 k voice) */
        if (dm && spk && spk_rate != rf_demod_audio_rate(dm)) {
            aos_hal_spk_close();
            spk = false;
        }
        if (dm && !spk) {
            spk_rate = rf_demod_audio_rate(dm);
            spk = aos_hal_spk_open(spk_rate);
            /* 100 ms of silence first: the ring's cushion */
            if (spk) {
                memset(pcm, 0, spk_rate / 10 * sizeof(int16_t));
                aos_hal_spk_write(pcm, spk_rate / 10);
            }
        } else if (!dm && spk) {
            aos_hal_spk_close();
            spk = false;
        }
        a->listening = dm && spk;
        /* recordings: as asked, and only while there is something to keep */
        bool want_wav = a->rec_wav && dm && spk;
        if (want_wav && !rf_wav_on(NULL, NULL)) {
            if (!rf_wav_start(spk_rate, a->freq)) a->rec_wav = false;
        } else if (!want_wav && rf_wav_on(NULL, NULL)) rf_wav_stop();
        uint64_t iq_bytes;
        if (a->rec_iq && !rf_iq_on(NULL, NULL)) {
            if (!rf_iq_start(src_rate, a->freq, cur_gain)) a->rec_iq = false;
        } else if (!a->rec_iq && rf_iq_on(&iq_bytes, NULL)) rf_iq_stop();
        if (!started) {
            started = src->ops->start(src);
            if (!started) {
                a->why = N_("La RTL-SDR no empezó a mandar muestras");
                src->ops->close(src);
                src = NULL;
                a->state = ST_NONE;
                next_try = aos_hal_uptime_ms() + 2000;
                continue;
            }
            since = 0;
        }
        uint64_t t0 = aos_hal_uptime_us();
        int n = src->ops->read(src, buf, READ_BYTES, 200);
        t_read += aos_hal_uptime_us() - t0;
        if (n < 0) {
            aos_hal_log("rf", "the radio went away");
            src->ops->close(src);
            src = NULL;
            started = false;
            a->why = N_("Se desconectó la RTL-SDR");
            a->state = ST_LOST;
            continue;
        }
        if (n > 0 && rf_iq_on(NULL, NULL)) rf_iq_write(buf, n);
        if (ook && n > 0) rf_ook_feed(ook, buf, n / 2, on_pulses, a);
        if (dm && n > 0) {
            uint64_t t3 = aos_hal_uptime_us();
            int na = rf_demod_run(dm, buf, n / 2, pcm, PCM_MAX);
            if (rf_wav_on(NULL, NULL)) rf_wav_write(pcm, na);
            if (spk) audio_out(pcm, na, spk_rate);
            rf_demod_stats_t st;
            rf_demod_stats(dm, &st);
            a->level_db = st.level_db;
            a->sq_open = st.open;
            a->pilot_db = st.pilot_db;
            t_dem += aos_hal_uptime_us() - t3;
        }
        /* RF_AVG FFTs from the start of each display period, the rest only read */
        uint32_t period = (a->rate ? a->rate : 2048000) / RF_FPS;
        for (int i = 0; i < n;) {
            if (rf_fft_count(fft) < RF_AVG) {
                int take = RF_FFT * 2 - fill;
                if (take > n - i) take = n - i;
                memcpy(frame + fill, buf + i, take);
                fill += take;
                i += take;
                since += take / 2;
                if (fill == RF_FFT * 2) {
                    uint64_t t1 = aos_hal_uptime_us();
                    rf_fft_add_cu8(fft, frame);
                    t_fft += aos_hal_uptime_us() - t1;
                    n_fft++;
                    fill = 0;
                }
            } else {
                uint32_t skip = period > since ? (period - since) * 2 : 0;
                if (skip > (uint32_t)(n - i)) skip = n - i;
                i += skip;
                since += skip / 2;
            }
            if (since >= period) {
                if (rf_fft_count(fft)) {
                    uint64_t t2 = aos_hal_uptime_us();
                    int navg = rf_fft_count(fft);
                    rf_fft_take_db(fft, db);
                    a->floor_bin_db = rf_fft_floor_db(db, RF_FFT, navg);
                    if (dm) rf_demod_set_noise_floor(dm, a->floor_bin_db, RF_FFT);
                    t_take += aos_hal_uptime_us() - t2;
                    aos_hal_mutex_lock(a->mx);
                    memcpy(a->spec, db, RF_FFT * sizeof(float));
                    a->spec_seq++;
                    aos_hal_mutex_unlock(a->mx);
                }
                since = 0;
                fill = 0;
            }
        }
        uint64_t b;
        uint32_t d;
        src->ops->stats(src, &b, &d);
        a->bytes = b;
        a->dropped = d;
        uint64_t now = aos_hal_uptime_us();
        if (now - t_log >= 10000000) {
            unsigned span = (unsigned)((now - t_log) / 1000);
            if (dm) {
                uint32_t pu[5];
                int ni;
                rf_demod_profile(dm, pu, &ni);
                aos_hal_log("rf", "demod in ms: input %u, decimation %u, channel %u, demodulation %u, audio %u; "
                                  "%d buffers not internal", (unsigned)(pu[0] / 1000), (unsigned)(pu[1] / 1000),
                            (unsigned)(pu[2] / 1000), (unsigned)(pu[3] / 1000), (unsigned)(pu[4] / 1000), ni);
            }
            aos_hal_log("rf", "engine in %u ms: read %u ms, %u FFTs %u ms, dB %u ms, demod %u ms (mode %d, floor %d dB, level %d dB, "
                              "pilot %d dB, audio queued %d); UI drew %u frames in %u ms",
                        span, (unsigned)(t_read / 1000), n_fft, (unsigned)(t_fft / 1000), (unsigned)(t_take / 1000),
                        (unsigned)(t_dem / 1000), cur_mode, (int)a->floor_bin_db, (int)a->level_db, (int)a->pilot_db,
                        spk ? aos_hal_spk_queued() : -1, a->ui_frames, a->ui_draw_us / 1000);
            t_read = t_fft = t_take = t_dem = 0;
            n_fft = 0;
            a->ui_frames = a->ui_draw_us = 0;
            t_log = now;
        }
    }
    if (src) src->ops->close(src);
out:
    if (spk) aos_hal_spk_close();
    rf_wav_stop();
    rf_iq_stop();
    rf_demod_free(dm);
    rf_ook_free(ook);
    free(buf);
    free(frame);
    free(db);
    free(pcm);
    rf_fft_free(fft);
    a->done = true;
}

/* ---- drawing -------------------------------------------------------------- */

static uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* dark blue to cyan to yellow to red, the usual waterfall */
static void build_lut(uint16_t *lut)
{
    static const uint8_t S[][4] = {
        { 0, 0, 0, 12 }, { 50, 0, 24, 110 }, { 100, 0, 120, 210 }, { 150, 30, 210, 200 },
        { 200, 250, 220, 40 }, { 235, 255, 90, 20 }, { 255, 255, 240, 230 },
    };
    for (int i = 0; i < 256; i++) {
        int k = 0;
        while (k < 5 && i > S[k + 1][0]) k++;
        float t = (float)(i - S[k][0]) / (S[k + 1][0] - S[k][0]);
        lut[i] = rgb565((int)(S[k][1] + t * (S[k + 1][1] - S[k][1])), (int)(S[k][2] + t * (S[k + 1][2] - S[k][2])),
                        (int)(S[k][3] + t * (S[k + 1][3] - S[k][3])));
    }
}

static void fmt_hz(char *out, size_t n, uint32_t hz)
{
    /* MHz with a decimal comma, to the hundred Hz */
    unsigned mhz = hz / 1000000, rest = (hz % 1000000) / 100;
    snprintf(out, n, "%u,%04u MHz", mhz, rest);
}

static void fmt_tick(char *out, size_t n, uint32_t hz, uint32_t tick)
{
    unsigned mhz = hz / 1000000, khz = (hz % 1000000) / 1000;
    if (tick >= 1000000) snprintf(out, n, "%u", mhz);
    else if (tick >= 100000) snprintf(out, n, "%u,%u", mhz, khz / 100);
    else if (tick >= 10000) snprintf(out, n, "%u,%02u", mhz, khz / 10);
    else snprintf(out, n, "%u,%03u", mhz, khz);
}

static void place_ticks(rf_t *a)
{
    uint32_t span = a->rate ? a->rate : a->want_rate;
    static const uint32_t T[] = { 10000, 20000, 25000, 50000, 100000, 200000, 250000, 500000, 1000000 };
    uint32_t tick = T[8];
    for (int i = 0; i < 9; i++)
        if (span / T[i] <= (uint32_t)(a->W / 110)) {
            tick = T[i];
            break;
        }
    uint32_t f = a->want_freq;          /* where it is going: the worker follows within a frame */
    int64_t lo = (int64_t)f - span / 2;
    int64_t first = (lo + tick - 1) / tick * tick;
    a->nticks = 0;
    for (int64_t t = first; t < lo + span && a->nticks < TICKS_MAX; t += tick) {
        int x = (int)((t - lo) * a->W / span);
        char s[24];
        fmt_tick(s, sizeof s, (uint32_t)t, tick);
        lv_obj_t *l = a->tick_lbl[a->nticks];
        lv_label_set_text(l, s);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(l, x - 60);
        a->tick_f[a->nticks] = (uint32_t)t;
        a->tick_x[a->nticks++] = (int16_t)x;
    }
    for (int i = a->nticks; i < TICKS_MAX; i++) lv_obj_add_flag(a->tick_lbl[i], LV_OBJ_FLAG_HIDDEN);
    /* the channel being listened to, shaded around the middle */
    uint32_t hw = rf_demod_half_width(a->want_mode);
    a->band_px = hw ? (int)((uint64_t)hw * a->W / span) : -1;
    if (hw && a->band_px < 1) a->band_px = 1;
    a->ticks_gen++;
}

/* the noise floor: the 20th percentile of the columns, slowly followed */
static void follow_floor(rf_t *a)
{
    static uint16_t hist[256];
    memset(hist, 0, sizeof hist);
    for (int x = 0; x < a->W; x++) {
        int b = (int)((a->col[x] + 160.0f) * 2.0f);
        hist[b < 0 ? 0 : b > 255 ? 255 : b]++;
    }
    int want = a->W / 5, acc = 0, b = 0;
    while (b < 255 && (acc += hist[b]) < want) b++;
    float f = b / 2.0f - 160.0f;
    if (!a->floor_set) {
        a->floor_db = f;
        a->floor_set = true;
    } else a->floor_db += (f - a->floor_db) * 0.05f;
}

/* the spectrum's pixel at (x, y) with the trace's top at t */
static inline uint16_t spec_pixel(const rf_t *a, int x, int y, int t)
{
    int dx = x - a->W / 2;
    if (y < t) {
        if (!dx) return a->c_mark;
        bool band = dx <= a->band_px && dx >= -a->band_px;
        return a->hgrid[y] || a->vgrid[x] ? (band ? a->c_bandgrid : a->c_grid) : band ? a->c_band : a->c_bg;
    }
    if (y <= t + 1) return a->c_line;
    return x == a->W / 2 ? a->c_markfill : a->c_fill[y];
}

/* Where the panel takes the two pictures straight (the board): LVGL's own
 * drawing of them cost a core and a half for 12 frames a second
 * (2026-10-06), since every frame re-rendered 620 000 pixels out of PSRAM.
 * Not under anything LVGL draws over the app (a panel, the switcher, a
 * toast, our own sheets): then the images are shown through LVGL. */
static bool may_blit(rf_t *a)
{
    return a->blit_ok && !aos_ui_overlay() && !a->sheet && lv_obj_has_flag(a->empty, LV_OBJ_FLAG_HIDDEN);
}

static void present(rf_t *a, bool spec_changed)
{
    if (may_blit(a)) {
        if (!lv_obj_has_flag(a->spec_img, LV_OBJ_FLAG_HIDDEN)) {
            /* LVGL stops drawing them; it paints their place black once */
            lv_obj_add_flag(a->spec_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(a->wf_img, LV_OBJ_FLAG_HIDDEN);
        }
        lv_area_t c;
        lv_obj_get_coords(a->spec_img, &c);
        bool ok = aos_hal_display_blit_scaled(c.x1, c.y1, a->W, a->SH, a->spec_px, 1, false);
        if (a->want_mode != MODE_DATA) {     /* data mode lists what it got where the waterfall was */
            lv_obj_get_coords(a->wf_img, &c);
            ok = ok && aos_hal_display_blit_scaled(c.x1, c.y1, a->W, a->WH, a->wf_px + a->wf_row * a->W, 1, false);
        }
        if (ok) return;
        a->blit_ok = false;         /* the simulator: LVGL from now on */
    }
    if (lv_obj_has_flag(a->spec_img, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(a->spec_img, LV_OBJ_FLAG_HIDDEN);
        if (a->want_mode != MODE_DATA) lv_obj_remove_flag(a->wf_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (a->want_mode == MODE_DATA) {
        if (spec_changed) {
            lv_image_cache_drop(&a->spec_dsc);
            lv_obj_invalidate(a->spec_img);
        }
        return;
    }
    if (spec_changed) {
        lv_image_cache_drop(&a->spec_dsc);
        lv_obj_invalidate(a->spec_img);
    }
    lv_image_cache_drop(&a->wf_dsc);
    a->wf_dsc.data = (const uint8_t *)(a->wf_px + a->wf_row * a->W);
    lv_image_set_src(a->wf_img, &a->wf_dsc);
    lv_obj_invalidate(a->wf_img);
}

static void draw_spectrum(rf_t *a)
{
    const int W = a->W, SH = a->SH;
    /* bins to columns: the strongest bin under each */
    for (int x = 0; x < W; x++) {
        int b0 = x * RF_FFT / W, b1 = (x + 1) * RF_FFT / W;
        float m = -200;
        for (int b = b0; b < b1 || b == b0; b++) m = a->ui_spec[b] > m ? a->ui_spec[b] : m;
        a->col[x] = m;
    }
    follow_floor(a);
    /* the scale moves in whole steps of 2 dB, so the grid stays put and the
     * trace can be redrawn only where it moved */
    float ref = 2.0f * floorf(a->floor_db / 2.0f);
    float lo = ref - 8, hi = ref + 62;
    float wlo = a->floor_db - 2, whi = a->floor_db + 42;
    bool full = ref != a->drawn_ref || a->ticks_gen != a->drawn_ticks || !a->drawn_once;
    if (full) {
        a->drawn_ref = ref;
        a->drawn_ticks = a->ticks_gen;
        a->drawn_once = true;
        float db_per_row = (hi - lo) / SH;
        for (int y = 0; y < SH; y++) {
            float level = hi - y * db_per_row;
            a->hgrid[y] = fmodf(level + 1000.0f, 10.0f) < db_per_row;
        }
        memset(a->vgrid, 0, W);
        for (int i = 0; i < a->nticks; i++)
            if (a->tick_x[i] >= 0 && a->tick_x[i] < W) a->vgrid[a->tick_x[i]] = 1;
    }
    for (int x = 0; x < W; x++) {
        a->smooth[x] += (a->col[x] - a->smooth[x]) * 0.5f;
        int y = (int)((hi - a->smooth[x]) * SH / (hi - lo));
        a->yt[x] = (int16_t)(y < 0 ? 0 : y >= SH - 2 ? SH - 2 : y);
    }
    if (full) {
        /* row by row (PSRAM likes rows) */
        for (int y = 0; y < SH; y++) {
            uint16_t *row = a->spec_px + y * W;
            for (int x = 0; x < W; x++) row[x] = spec_pixel(a, x, y, a->yt[x]);
        }
    } else {
        /* only the rows between where the trace was and where it is */
        for (int x = 0; x < W; x++) {
            int t0 = a->yd[x], t1 = a->yt[x];
            if (t0 == t1) continue;
            int y0 = t0 < t1 ? t0 : t1, y1 = (t0 < t1 ? t1 : t0) + 1;
            for (int y = y0; y <= y1 && y < SH; y++) a->spec_px[y * W + x] = spec_pixel(a, x, y, t1);
        }
    }
    memcpy(a->yd, a->yt, W * sizeof(int16_t));

    /* a row of the waterfall, twice */
    a->wf_row = (a->wf_row + a->WH - 1) % a->WH;
    uint16_t *r1 = a->wf_px + a->wf_row * W, *r2 = a->wf_px + (a->wf_row + a->WH) * W;
    float k = 255.0f / (whi - wlo);
    for (int x = 0; x < W; x++) {
        int i = (int)((a->col[x] - wlo) * k);
        r1[x] = r2[x] = a->lut[i < 0 ? 0 : i > 255 ? 255 : i];
    }
    r1[W / 2] = r2[W / 2] = a->c_mark;
    present(a, true);
}

/* ---- the UI's state ------------------------------------------------------- */

static void show_freq(rf_t *a)
{
    char s[32];
    fmt_hz(s, sizeof s, a->want_freq);
    lv_label_set_text(a->freq_lbl, s);
}

static void show_buttons(rf_t *a)
{
    char s[48];
    uint32_t r = a->rate ? a->rate : a->want_rate;
    snprintf(s, sizeof s, "%u,%03u Msps", (unsigned)(r / 1000000), (unsigned)(r % 1000000 / 1000));
    lv_label_set_text(lv_obj_get_child(a->rate_btn, 0), s);
    if (a->want_gain < 0) snprintf(s, sizeof s, "%s", _("Gan. auto"));
    else snprintf(s, sizeof s, _("Gan. %d,%d dB"), a->want_gain / 10, a->want_gain % 10);
    lv_label_set_text(lv_obj_get_child(a->gain_btn, 0), s);
    if (a->step >= 1000000) snprintf(s, sizeof s, _("Paso %u MHz"), (unsigned)(a->step / 1000000));
    else if (a->step % 1000) {
        /* 6,25 / 8,33 / 12,5 kHz */
        unsigned frac = a->step % 1000;
        if (frac % 100) snprintf(s, sizeof s, _("Paso %u,%02u kHz"), (unsigned)(a->step / 1000), frac / 10);
        else snprintf(s, sizeof s, _("Paso %u,%u kHz"), (unsigned)(a->step / 1000), frac / 100);
    } else snprintf(s, sizeof s, _("Paso %u kHz"), (unsigned)(a->step / 1000));
    lv_label_set_text(lv_obj_get_child(a->step_btn, 0), s);
    /* only AM and narrow FM use it: dimmed for the others */
    bool sq_used = a->want_mode == RF_MODE_AM || a->want_mode == RF_MODE_NFM;
    if (sq_used && a->want_sq) snprintf(s, sizeof s, _("Silenc. %d dB"), a->want_sq);
    else snprintf(s, sizeof s, "%s", _("Silenc. no"));
    lv_label_set_text(lv_obj_get_child(a->sq_btn, 0), s);
    lv_obj_set_style_text_color(lv_obj_get_child(a->sq_btn, 0), sq_used ? AOS_C_TEXT : AOS_C_DIM, 0);
    for (int m = 0; m < MODES_N; m++)
        lv_obj_set_style_bg_color(a->mode_btn[m], m == a->want_mode ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(a->rec_btn, a->rec_iq || a->rec_wav ? AOS_C_RED : AOS_C_CARD2, 0);
}

static uint32_t clamp_freq(rf_t *a, int64_t f)
{
    int64_t lo = a->info.fmin ? a->info.fmin : 24000000, hi = a->info.fmax ? a->info.fmax : 1766000000;
    return (uint32_t)(f < lo ? lo : f > hi ? hi : f);
}

static void tune(rf_t *a, int64_t hz, bool snap)
{
    if (a->playing || a->play_req) {
        /* a recording has its centre: the spectrum stays where it was */
        aos_ui_toast(_("Reproduciendo una grabación: no se puede sintonizar"), 2000);
        return;
    }
    if (snap && a->step) hz = (hz + a->step / 2) / a->step * a->step;
    a->want_freq = clamp_freq(a, hz);
    show_freq(a);
}

static void capture_now(lv_timer_t *t);
static void show_vol(rf_t *a);
static bool play_file(rf_t *a, const char *name);

/* One line of key=value: from rf/control.txt or from the portal's page (the
 * live channel). freq (Hz, or MHz with a point), mode (off, wfm, am, nfm,
 * data), rate, gain (tenths of a dB, or auto), sq (dB), step (Hz); and from
 * the page also step_by (+1/-1), capture, rec_iq, rec_wav, play (a
 * recording's name), play_stop, log, mqtt, vol (0-100, the board's) and
 * mute (the radio's own). */
static void apply_line(rf_t *a, const char *line)
{
    char k[16], v[80];
    if (sscanf(line, " %15[a-z_] = %79s", k, v) != 2) return;
    long n = strtol(v, NULL, 10);
    if (!strcmp(k, "freq")) {
        double x = strtod(v, NULL);
        tune(a, strchr(v, '.') ? (int64_t)(x * 1e6 + 0.5) : (int64_t)x, false);
    } else if (!strcmp(k, "mode")) {
        static const char *const M[MODES_N] = { "off", "wfm", "am", "nfm", "data" };
        for (int m = 0; m < MODES_N; m++)
            if (!strcmp(v, M[m])) {
                if (a->want_mode == RF_MODE_OFF && m != RF_MODE_OFF && m != MODE_DATA && a->info.high_speed)
                    a->want_rate = m == RF_MODE_WFM ? 960000 : 240000;
                a->want_mode = m;
                if (MODE_STEP[m]) a->step = MODE_STEP[m];
            }
    } else if (!strcmp(k, "rate")) a->want_rate = (uint32_t)strtoul(v, NULL, 10);
    else if (!strcmp(k, "gain")) a->want_gain = !strcmp(v, "auto") ? -1 : (int)n;
    else if (!strcmp(k, "sq")) a->want_sq = (int)n;
    else if (!strcmp(k, "step")) a->step = (uint32_t)strtoul(v, NULL, 10);
    else if (!strcmp(k, "step_by")) tune(a, (int64_t)a->want_freq + (n < 0 ? -1 : 1) * (int64_t)a->step, true);
    else if (!strcmp(k, "capture")) lv_timer_create(capture_now, 250, NULL);
    else if (!strcmp(k, "rec_iq")) a->rec_iq = n != 0;
    else if (!strcmp(k, "rec_wav")) a->rec_wav = n != 0;
    else if (!strcmp(k, "play")) play_file(a, v);
    else if (!strcmp(k, "play_stop")) a->play_stop = true;
    else if (!strcmp(k, "log")) a->log_on = n != 0;
    else if (!strcmp(k, "mqtt")) a->mqtt_on = n != 0;
    else if (!strcmp(k, "vol")) {
        aos_hal_volume_set(n < 0 ? 0 : n > 100 ? 100 : (int)n);
        a->muted = false;
        if (a->vol_btn) show_vol(a);
    } else if (!strcmp(k, "mute")) {
        a->muted = n != 0;
        if (a->vol_btn) show_vol(a);
    }
    else if (!strcmp(k, "simd")) rf_demod_use_simd(n != 0);      /* to measure */
    else if (!strcmp(k, "fft")) rf_fft_use_simd(n != 0);
}

static void applied(rf_t *a)
{
    show_freq(a);
    show_buttons(a);
    place_ticks(a);
    mode_view(a);
}

/* rf/control.txt on the card, read when it changes (apply_line). The
 * portal's page sends the same lines through the live channel. */
static void control_poll(rf_t *a)
{
    const char *root = aos_hal_path_sd_root();
    char path[128];
    snprintf(path, sizeof path, "%s/rf/control.txt", root ? root : aos_hal_path_data());
    struct stat st;
    if (stat(path, &st) != 0) return;
    if ((long)st.st_mtime == a->ctl_mtime && (long)st.st_size == a->ctl_size) return;
    a->ctl_mtime = (long)st.st_mtime;
    a->ctl_size = (long)st.st_size;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) apply_line(a, line);
    fclose(f);
    aos_hal_log("rf", "control.txt: %u Hz, mode %d, %u sps, gain %d, squelch %d", (unsigned)a->want_freq, a->want_mode,
                (unsigned)a->want_rate, a->want_gain, a->want_sq);
    applied(a);
}

/* ---- the portal's page (apps/rf/web/rf.js), through the live channel ---- */

#define LIVE_APP "aos.rf"
#define LIVE_BINS 1024

/* the spectrum: "RFS1", seq, centre Hz, rate, floor (dB x 10), bins, mode,
 * flags (1 listening, 2 squelch open, 4 playing), then the bins as
 * (dB + 140) x 2, the strongest of each pair of the FFT's */
static void live_spec(rf_t *a)
{
    static uint8_t blob[24 + LIVE_BINS];
    static uint32_t seq;
    uint32_t v[4] = { ++seq, a->want_freq, a->rate ? a->rate : a->want_rate, 0 };
    memcpy(blob, "RFS1", 4);
    memcpy(blob + 4, v, 12);
    int16_t fl = (int16_t)(a->floor_bin_db * 10);
    uint16_t nb = LIVE_BINS;
    memcpy(blob + 16, &fl, 2);
    memcpy(blob + 18, &nb, 2);
    blob[20] = (uint8_t)a->want_mode;
    blob[21] = (uint8_t)((a->listening ? 1 : 0) | (a->sq_open ? 2 : 0) | (a->playing ? 4 : 0));
    blob[22] = blob[23] = 0;
    for (int i = 0; i < LIVE_BINS; i++) {
        float d = a->ui_spec[2 * i] > a->ui_spec[2 * i + 1] ? a->ui_spec[2 * i] : a->ui_spec[2 * i + 1];
        float q = (d + 140.0f) * 2.0f;
        blob[24 + i] = (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q);
    }
    aos_hal_live_put(LIVE_APP, "spec", "application/octet-stream", blob, sizeof blob);
}

static void live_state(rf_t *a, unsigned mbs10)
{
    static char j[1400];
    static const char *const M[MODES_N] = { "off", "wfm", "am", "nfm", "data" };
    uint64_t iqb = 0;
    uint32_t drop = 0, ws = 0, wr = 0;
    bool iq = rf_iq_on(&iqb, &drop), wav = rf_wav_on(&ws, &wr);
    int k = snprintf(j, sizeof j,
                     "{\"state\":\"%s\",\"why\":\"%s\",\"kind\":\"%s\",\"name\":\"%s\",\"tuner\":\"%s\","
                     "\"high_speed\":%s,\"fmin\":%u,\"fmax\":%u,\"freq\":%u,\"rate\":%u,\"mode\":\"%s\",\"gain\":%d,"
                     "\"step\":%u,\"sq\":%d,\"level\":%.1f,\"pilot\":%.1f,\"open\":%s,\"listening\":%s,"
                     "\"mbs\":%u.%u,\"dropped\":%u,\"rec_iq\":%s,\"iq_mb\":%u,\"iq_dropped\":%u,\"rec_wav\":%s,"
                     "\"wav_s\":%u,\"playing\":%s,\"play\":\"%s\",\"received\":%u,\"log\":%s,\"mqtt\":%s,"
                     "\"mqtt_ready\":%s,\"vol\":%d,\"muted\":%s,\"gains\":[",
                     a->state == ST_RUN ? "run" : a->state == ST_SEARCH ? "search" : "none", a->why ? a->why : "",
                     a->info.kind ? a->info.kind : "", a->info.name, a->info.tuner, a->info.high_speed ? "true" : "false",
                     (unsigned)a->info.fmin, (unsigned)a->info.fmax, (unsigned)a->want_freq,
                     (unsigned)(a->rate ? a->rate : a->want_rate), M[a->want_mode], a->want_gain, (unsigned)a->step,
                     a->want_sq, (double)a->level_db, (double)a->pilot_db, a->sq_open ? "true" : "false",
                     a->listening ? "true" : "false", mbs10 / 10, mbs10 % 10, (unsigned)a->dropped,
                     iq ? "true" : "false", (unsigned)(iqb / 1000000), (unsigned)drop, wav ? "true" : "false",
                     (unsigned)(ws / (wr ? wr : 1)), a->playing ? "true" : "false",
                     a->playing ? (strrchr(a->play_path, '/') ? strrchr(a->play_path, '/') + 1 : a->play_path) : "",
                     (unsigned)a->ev_n, a->log_on ? "true" : "false", a->mqtt_on ? "true" : "false",
                     rf_mqtt_ready() ? "true" : "false", aos_hal_volume_get(), a->muted ? "true" : "false");
    for (int i = 0; i < a->info.ngains && k < (int)sizeof j - 16; i++)
        k += snprintf(j + k, sizeof j - k, "%s%d", i ? "," : "", a->info.gains[i]);
    snprintf(j + k, sizeof j - k, "]}");
    aos_hal_live_put(LIVE_APP, "state", "application/json", j, strlen(j));
}

/* the last 30 transmissions, newest first, as data: the page writes them */
static void live_events(rf_t *a)
{
    static char j[12 * 1024];
    static const char *const MOD[] = { "", "PWM", "PPM", "Manchester" };
    int k = snprintf(j, sizeof j, "[");
    aos_hal_mutex_lock(a->mx);
    uint32_t n = a->ev_n;
    for (uint32_t i = 0; i < n && i < 30 && k < (int)sizeof j - 400; i++) {
        const rf_event_t *e = &a->ev[(n - 1 - i) % EV_MAX];
        const rf_decoded_t *d = &e->d;
        k += snprintf(j + k, sizeof j - k,
                      "%s{\"n\":%u,\"t\":\"%02d:%02d:%02d\",\"proto\":\"%s\",\"count\":%d,\"freq\":%u,\"off\":%d,"
                      "\"snr\":%.1f,\"mod\":\"%s\",\"short\":%d,\"long\":%d,\"gap\":%d,\"bits\":%d,\"hex\":\"%.64s\","
                      "\"id\":%u,\"code\":\"%s\"",
                      i ? "," : "", (unsigned)(n - 1 - i), e->when.tm_hour, e->when.tm_min, e->when.tm_sec, d->proto,
                      e->count, (unsigned)e->freq, (int)e->off, (double)e->snr, MOD[d->mod], d->short_us, d->long_us,
                      d->gap_us, d->nbits, d->hex, (unsigned)d->id, d->code);
        if (d->has_temp) k += snprintf(j + k, sizeof j - k, ",\"temp\":%.1f", (double)d->temp_c);
        if (d->has_hum) k += snprintf(j + k, sizeof j - k, ",\"hum\":%d", d->hum);
        if (d->has_batt) k += snprintf(j + k, sizeof j - k, ",\"batt_low\":%s", d->batt_low ? "true" : "false");
        if (d->has_button) k += snprintf(j + k, sizeof j - k, ",\"button\":%d", d->button);
        if (d->channel) k += snprintf(j + k, sizeof j - k, ",\"channel\":%d", d->channel);
        k += snprintf(j + k, sizeof j - k, "}");
    }
    aos_hal_mutex_unlock(a->mx);
    snprintf(j + k, sizeof j - k, "]");
    aos_hal_live_put(LIVE_APP, "events", "application/json", j, strlen(j));
}

/* the page's lines, as they come */
static void live_take(rf_t *a)
{
    char msg[AOS_LIVE_MSG_MAX + 1];
    bool any = false;
    while (aos_hal_live_take(LIVE_APP, msg, sizeof msg) > 0) {
        for (char *line = msg; line && *line;) {      /* strtok is not in the firmware's table */
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            apply_line(a, line);
            line = nl ? nl + 1 : NULL;
        }
        any = true;
    }
    if (any) applied(a);
}

static void ui_timer(lv_timer_t *t)
{
    rf_t *a = lv_timer_get_user_data(t);
    /* with nothing to listen to, the spectrum is only worth its 4 MB/s on
     * screen: the stream rests while the board is locked or the app is
     * behind another; listening goes on (and nothing is drawn) */
    bool unseen = a->hidden || (aos_ui_overlay() & AOS_UI_OVER_LOCK);
    /* a page of the portal looking counts as someone looking */
    bool watched = aos_hal_live_idle_ms(LIVE_APP) < 3000;
    a->paused = unseen && a->want_mode == RF_MODE_OFF && !watched;
    live_take(a);
    if (a->state == ST_RUN) {
        if (!lv_obj_has_flag(a->empty, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(a->empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(a->empty, LV_OBJ_FLAG_HIDDEN);
        char s[400];
        if (a->state == ST_SEARCH) snprintf(s, sizeof s, "%s", _("Buscando una RTL-SDR…"));
        else
            snprintf(s, sizeof s, "%s\n\n%s", a->why ? _(a->why) : "",
                     aos_hal_usb_host_on()
                         ? _("Enchufala en el host USB: pines 25/27 del conector de atrás (High Speed, cables de menos de 15 cm) y 5 V del pin 1. Los pines 21/23 son Full Speed: alcanzan para 240 mil muestras por segundo.")
                         : _("El host USB está apagado: prendelo en Ajustes → USB → Host USB, y enchufá la RTL-SDR en los pines 25/27 del conector de atrás."));
        lv_label_set_text(a->empty_lbl, s);
    }
    /* the engine changed the rate (listening keeps to 960 k or 1.92 M) */
    if (a->rate && a->rate != a->shown_rate) {
        a->shown_rate = a->rate;
        show_buttons(a);
        place_ticks(a);
    }
    if (a->info_seq != a->seen_info) {
        a->seen_info = a->info_seq;
        a->want_freq = clamp_freq(a, a->want_freq);
        show_freq(a);
        show_buttons(a);
        place_ticks(a);
    }
    if (a->ev_seq != a->seen_ev && a->want_mode == MODE_DATA && !unseen && !a->sheet) {
        a->seen_ev = a->ev_seq;
        ev_rebuild(a);
    }
    if (a->spec_seq != a->seen_spec && a->state == ST_RUN && (!unseen || watched)) {
        aos_hal_mutex_lock(a->mx);
        memcpy(a->ui_spec, a->spec, RF_FFT * sizeof(float));
        a->seen_spec = a->spec_seq;
        aos_hal_mutex_unlock(a->mx);
        if (!unseen) {
            uint64_t t0 = aos_hal_uptime_us();
            draw_spectrum(a);
            a->ui_draw_us += (unsigned)(aos_hal_uptime_us() - t0);
            a->ui_frames++;
        }
        if (watched) live_spec(a);
    }
    if (watched && a->ev_seq != a->live_ev) {
        a->live_ev = a->ev_seq;
        live_events(a);
    }
    uint64_t now = aos_hal_uptime_ms();
    if (now - a->stat_at >= 1000) {
        uint64_t b = a->bytes;
        /* tenths of MB/s, with a decimal comma */
        unsigned mbs10 = a->stat_at && b >= a->stat_bytes ? (unsigned)((b - a->stat_bytes) * 10 / 1000 / (now - a->stat_at)) : 0;
        char s[160];
        if (a->state == ST_RUN && a->listening) {
            /* what is being heard: the channel over the noise, the pilot */
            int lv = (int)(a->level_db + 0.5f);
            snprintf(s, sizeof s, "%s · %s %d dB%s%s · %u,%u MB/s", _(MODES[a->want_mode]), _("señal"), lv,
                     a->sq_open ? "" : " · ", a->sq_open ? "" : _("silenciado"), mbs10 / 10, mbs10 % 10);
            if (a->want_mode == RF_MODE_WFM && a->pilot_db >= 10) {
                size_t k = strlen(s);
                snprintf(s + k, sizeof s - k, " · %s", _("estéreo"));
            }
        } else if (a->state == ST_RUN)
            snprintf(s, sizeof s, "%s %s · %s · %u,%u MB/s · %s %u", a->info.kind, a->info.tuner,
                     a->info.high_speed ? "High Speed" : "Full Speed", mbs10 / 10, mbs10 % 10, _("perdidos"),
                     (unsigned)a->dropped);
        else snprintf(s, sizeof s, "%s", _("Sin radio"));
        /* what is being kept, or played */
        size_t k = strlen(s);
        uint64_t iqb;
        uint32_t ws, wr, drop;
        if (a->playing) snprintf(s + k, sizeof s - k, " · %s", _("reproduciendo"));
        else if (rf_iq_on(&iqb, &drop))
            snprintf(s + k, sizeof s - k, " · " LV_SYMBOL_SAVE " %u MB%s", (unsigned)(iqb / 1000000), drop ? " !" : "");
        else if (rf_wav_on(&ws, &wr))
            snprintf(s + k, sizeof s - k, " · " LV_SYMBOL_SAVE " %u:%02u", (unsigned)(ws / (wr ? wr : 1) / 60),
                     (unsigned)(ws / (wr ? wr : 1) % 60));
        if (a->state == ST_RUN && a->want_mode == MODE_DATA && !a->listening) {
            k = strlen(s);
            snprintf(s + k, sizeof s - k, " · %u %s", (unsigned)a->ev_n, _("recibidos"));
        }
        lv_label_set_text(a->status_lbl, s);
        a->stat_at = now;
        a->stat_bytes = b;
        if (watched) live_state(a, mbs10);
        control_poll(a);
        if (a->sq_live) {
            snprintf(s, sizeof s, _("Señal ahora: %d dB sobre el ruido"), (int)(a->level_db + 0.5f));
            lv_label_set_text(a->sq_live, s);
        }
        if (a->rate && a->nticks == 0) place_ticks(a);
        /* the volume may change elsewhere too (control centre, the keys) */
        if (a->vol_btn && aos_hal_volume_get() != a->vol_shown) show_vol(a);
    }
}

/* ---- events --------------------------------------------------------------- */

static void spec_event(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    uint32_t span = a->rate ? a->rate : a->want_rate;
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        a->dragging = true;
        a->moved = false;
        a->drag_x0 = p.x;
        a->drag_f0 = a->want_freq;
        break;
    case LV_EVENT_PRESSING:
        if (!a->dragging) break;
        if (LV_ABS(p.x - a->drag_x0) > 12) a->moved = true;
        if (a->moved) {
            tune(a, (int64_t)a->drag_f0 - (int64_t)(p.x - a->drag_x0) * span / a->W, false);
            place_ticks(a);
        }
        break;
    case LV_EVENT_RELEASED:
        if (!a->dragging) break;
        a->dragging = false;
        if (a->moved) tune(a, a->want_freq, true);
        else {
            /* a tap: that frequency to the centre. A finger covers ~60 px,
             * 80 kHz at 0.96 Msps, and a narrow FM channel is 12.5: so a
             * signal near the finger, clearly over the floor, is taken at
             * its peak. The middle column is left out of the search, where
             * the stick's own DC spike stands. */
            lv_area_t c;
            lv_obj_get_coords(lv_event_get_target_obj(e), &c);
            int x = p.x - c.x1, best = -1;
            float bv = a->floor_db + 8;
            for (int k = x - SNAP_PX; k <= x + SNAP_PX; k++)
                if (k >= 0 && k < a->W && LV_ABS(k - a->W / 2) > 2 && a->smooth[k] > bv) {
                    bv = a->smooth[k];
                    best = k;
                }
            if (best >= 0) x = best;
            tune(a, (int64_t)a->want_freq + (int64_t)(x - a->W / 2) * span / a->W, true);
        }
        place_ticks(a);
        break;
    default:
        break;
    }
}

static void tick_tap(lv_event_t *e)
{
    rf_t *a = A;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= a->nticks) return;
    tune(a, a->tick_f[i], false);
    place_ticks(a);
}

static void step_by(lv_event_t *e)
{
    rf_t *a = A;
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    tune(a, (int64_t)a->want_freq + dir * (int64_t)a->step, true);
    place_ticks(a);
}

static void step_cycle(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    int n = sizeof STEPS / sizeof STEPS[0], i = 0;
    while (i < n && STEPS[i] != a->step) i++;
    a->step = STEPS[(i + 1) % n];
    show_buttons(a);
}

static void rate_cycle(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    bool listen = a->want_mode != RF_MODE_OFF;
    const uint32_t *R = !a->info.high_speed ? RATES_FS : listen ? RATES_LISTEN : RATES_HS;
    int n = !a->info.high_speed ? 1 : listen ? 3 : 4, i = 0;
    uint32_t cur = listen && a->info.high_speed ? listen_rate(a->want_rate) : a->want_rate;
    while (i < n && R[i] != cur) i++;
    a->want_rate = R[(i + 1) % n];
    show_buttons(a);
    place_ticks(a);
    if (!a->info.high_speed) aos_ui_toast(_("En los pines 21/23 (Full Speed) sólo entran 240 mil muestras por segundo"), 2500);
}

static void band_tap(lv_event_t *e)
{
    rf_t *a = A;
    const band_t *b = &BANDS[(intptr_t)lv_event_get_user_data(e)];
    a->step = b->step;
    if (b->mode != RF_MODE_OFF && a->info.high_speed && b->mode != a->want_mode)
        a->want_rate = b->mode == RF_MODE_WFM ? 960000 : 240000;
    a->want_mode = b->mode;
    tune(a, b->hz, false);
    show_buttons(a);
    place_ticks(a);
    mode_view(a);
}

static void mode_tap(lv_event_t *e)
{
    rf_t *a = A;
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    if (m == a->want_mode) return;
    /* how wide to look while listening: broadcast FM at 960 k, with its
     * neighbours on screen; the narrow modes at 240 k, a fifth of the work */
    if (a->want_mode == RF_MODE_OFF && m != RF_MODE_OFF && a->info.high_speed)
        a->want_rate = m == RF_MODE_WFM ? 960000 : 240000;
    a->want_mode = m;
    if (MODE_STEP[m]) a->step = MODE_STEP[m];
    tune(a, a->want_freq, true);
    show_buttons(a);
    place_ticks(a);
    mode_view(a);
}

static void sheet_close(rf_t *a)
{
    if (a->sheet) lv_obj_delete(a->sheet);
    a->sheet = a->ta = a->gain_slider = a->gain_val = a->sq_slider = a->sq_val = a->sq_live = NULL;
    a->vol_val = a->vol_slider = a->mute_btn = NULL;
}

static void sheet_bg_tap(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    if (lv_event_get_target_obj(e) == a->sheet) sheet_close(a);
}

static lv_obj_t *sheet_open(rf_t *a, const char *title)
{
    sheet_close(a);
    a->sheet = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->sheet);
    lv_obj_set_size(a->sheet, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(a->sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(a->sheet, LV_OPA_60, 0);
    lv_obj_add_flag(a->sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->sheet, sheet_bg_tap, LV_EVENT_CLICKED, a);
    lv_obj_t *card = lv_obj_create(a->sheet);
    lv_obj_set_width(card, a->W - 2 * AOS_UI_PAD);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(card, AOS_UI_PAD, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 16, 0);
    aos_label(card, title, aos_font_title, AOS_C_TEXT);
    return card;
}

static void freq_ok(rf_t *a)
{
    const char *t = lv_textarea_get_text(a->ta);
    char s[24];
    snprintf(s, sizeof s, "%s", t);
    for (char *c = s; *c; c++)
        if (*c == ',') *c = '.';
    double mhz = strtod(s, NULL);
    if (mhz > 0) {
        tune(a, (int64_t)(mhz * 1e6 + 0.5), false);
        place_ticks(a);
    }
    sheet_close(a);
}

static void kb_event(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) freq_ok(a);
    else if (c == LV_EVENT_CANCEL) sheet_close(a);
}

static void freq_tap(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    lv_obj_t *card = sheet_open(a, _("Frecuencia en MHz"));
    a->ta = lv_textarea_create(card);
    lv_obj_set_width(a->ta, LV_PCT(100));
    lv_textarea_set_one_line(a->ta, true);
    lv_textarea_set_accepted_chars(a->ta, "0123456789.,");
    lv_textarea_set_max_length(a->ta, 12);
    lv_obj_set_style_text_font(a->ta, aos_font_large, 0);
    char s[24];
    snprintf(s, sizeof s, "%u,%04u", (unsigned)(a->want_freq / 1000000), (unsigned)(a->want_freq % 1000000 / 100));
    lv_textarea_set_text(a->ta, s);
    lv_obj_t *kb = lv_keyboard_create(a->sheet);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    aos_keyboard_style(kb, aos_font_body);     /* dark, as the system's other keyboards */
    lv_keyboard_set_textarea(kb, a->ta);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_READY, a);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_CANCEL, a);
}

static void gain_changed(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    int v = lv_slider_get_value(a->gain_slider);
    a->want_gain = v == 0 || !a->info.ngains ? -1 : a->info.gains[v - 1];
    char s[48];
    if (a->want_gain < 0) snprintf(s, sizeof s, "%s", _("Automática"));
    else snprintf(s, sizeof s, "%d,%d dB", a->want_gain / 10, a->want_gain % 10);
    lv_label_set_text(a->gain_val, s);
    show_buttons(a);
}

static void gain_tap(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    lv_obj_t *card = sheet_open(a, _("Ganancia"));
    a->gain_val = aos_label(card, "", aos_font_large, AOS_C_TEXT);
    a->gain_slider = lv_slider_create(card);
    lv_obj_set_width(a->gain_slider, LV_PCT(100));
    lv_obj_set_height(a->gain_slider, 24);
    lv_slider_set_range(a->gain_slider, 0, a->info.ngains > 0 ? a->info.ngains : 1);
    int v = 0;
    for (int i = 0; i < a->info.ngains; i++)
        if (a->info.gains[i] == a->want_gain) v = i + 1;
    lv_slider_set_value(a->gain_slider, v, LV_ANIM_OFF);
    lv_obj_add_event_cb(a->gain_slider, gain_changed, LV_EVENT_VALUE_CHANGED, a);
    aos_label(card, _("Todo a la izquierda es automática. Subila hasta que aparezcan las señales sin que suba el piso de ruido."),
              aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(lv_obj_get_child(card, -1), LV_PCT(100));
    lv_label_set_long_mode(lv_obj_get_child(card, -1), LV_LABEL_LONG_WRAP);
    lv_obj_send_event(a->gain_slider, LV_EVENT_VALUE_CHANGED, NULL);
}

static void sq_changed(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    a->want_sq = lv_slider_get_value(a->sq_slider);
    char s[48];
    if (a->want_sq) snprintf(s, sizeof s, "%d dB", a->want_sq);
    else snprintf(s, sizeof s, "%s", _("Siempre abierto"));
    lv_label_set_text(a->sq_val, s);
    show_buttons(a);
}

/* The speaker button: its symbol says the volume, or that the radio is muted */
static void show_vol(rf_t *a)
{
    int v = aos_hal_volume_get();
    a->vol_shown = v;
    const char *sym = a->muted || v == 0 ? LV_SYMBOL_MUTE : v < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX;
    lv_label_set_text(lv_obj_get_child(a->vol_btn, 0), sym);
    lv_obj_set_style_bg_color(a->vol_btn, a->muted ? AOS_C_RED : AOS_C_CARD2, 0);
    if (a->vol_val) {
        char s[24];
        snprintf(s, sizeof s, "%d %%", v);
        lv_label_set_text(a->vol_val, s);
        lv_label_set_text(lv_obj_get_child(a->mute_btn, 0), a->muted ? _("Activar el sonido") : _("Silenciar"));
        lv_obj_set_style_bg_color(a->mute_btn, a->muted ? AOS_C_RED : AOS_C_CARD2, 0);
    }
}

static void set_muted(rf_t *a, bool m)
{
    a->muted = m;
    show_vol(a);
}

static void vol_changed(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    aos_hal_volume_set(lv_slider_get_value(a->vol_slider));
    if (a->muted) a->muted = false;     /* moving the volume means wanting to hear */
    show_vol(a);
}

static void mute_tap(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    set_muted(a, !a->muted);
}

static void vol_tap(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    lv_obj_t *card = sheet_open(a, _("Volumen"));
    a->vol_val = aos_label(card, "", aos_font_large, AOS_C_TEXT);
    a->vol_slider = lv_slider_create(card);
    lv_obj_set_width(a->vol_slider, LV_PCT(100));
    lv_obj_set_height(a->vol_slider, 24);
    lv_slider_set_range(a->vol_slider, 0, 100);
    lv_slider_set_value(a->vol_slider, aos_hal_volume_get(), LV_ANIM_OFF);
    lv_obj_add_event_cb(a->vol_slider, vol_changed, LV_EVENT_VALUE_CHANGED, a);
    a->mute_btn = button_f(card, "", mute_tap, a, aos_font_body, 20);
    lv_obj_t *l = aos_label(card, _("El volumen es el de la placa, el mismo de todo el sistema. Silenciar calla sólo la radio, que sigue sintonizada."),
                            aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    show_vol(a);
}

static void sq_tap(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    lv_obj_t *card = sheet_open(a, _("Silenciador"));
    a->sq_val = aos_label(card, "", aos_font_large, AOS_C_TEXT);
    a->sq_slider = lv_slider_create(card);
    lv_obj_set_width(a->sq_slider, LV_PCT(100));
    lv_obj_set_height(a->sq_slider, 24);
    lv_slider_set_range(a->sq_slider, 0, 40);
    lv_slider_set_value(a->sq_slider, a->want_sq, LV_ANIM_OFF);
    lv_obj_add_event_cb(a->sq_slider, sq_changed, LV_EVENT_VALUE_CHANGED, a);
    a->sq_live = aos_label(card, "", aos_font_body, AOS_C_ACCENT);
    lv_obj_t *l = aos_label(card, _("Corta el audio de AM y FM angosta mientras el canal no supera al ruido por esos dB; entre 6 y 15 suele andar. Todo a la izquierda, siempre abierto. La FM comercial no lo usa."),
                            aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_send_event(a->sq_slider, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ---- data: what was received ------------------------------------------------ */

static const char *mod_name(int mod)
{
    static const char *const M[] = { "?", "PWM", "PPM", "Manchester" };
    return M[mod >= 0 && mod <= 3 ? mod : 0];
}

/* the event's second line, in the app's language */
static void ev_line(const rf_event_t *e, char *out, size_t n)
{
    const rf_decoded_t *d = &e->d;
    int k = 0;
    if (d->has_temp) {
        k = snprintf(out, n, _("ID %u · canal %d · %.1f °C"), (unsigned)d->id, d->channel, (double)d->temp_c);
        if (d->has_hum) k += snprintf(out + k, n - k, " · %d %%", d->hum);
        if (d->has_batt && d->batt_low) k += snprintf(out + k, n - k, " · %s", _("batería baja"));
    } else if (!strcmp(d->proto, "PT2262")) {
        k = snprintf(out, n, _("código %.8s · datos %s"), d->code, d->code + 8);
    } else if (d->proto[0]) {
        k = snprintf(out, n, _("ID %05X · botón %X"), (unsigned)d->id, d->button);
    } else {
        k = snprintf(out, n, "%s %d/%d µs · %d bits · %.16s%s", mod_name(d->mod), d->short_us, d->long_us, d->nbits,
                     d->hex, strlen(d->hex) > 16 ? "…" : "");
    }
    if ((size_t)k < n) snprintf(out + k, n - k, " · %+d kHz", (int)(e->off / 1000));
}

static void ev_detail(lv_event_t *e);

static void ev_title(const rf_event_t *ev, char *t, size_t n)
{
    int c = snprintf(t, n, "%02d:%02d:%02d  %s", ev->when.tm_hour, ev->when.tm_min, ev->when.tm_sec,
                     ev->d.proto[0] ? ev->d.proto : _("Desconocido"));
    if (ev->count > 1 && (size_t)c < n) snprintf(t + c, n - c, "  ×%d", ev->count);
}

/* New senders make the rows again; a repeat only changes its row's count
 * (made again on every repeat, a row went from under the finger before the
 * tap on it could land). */
static void ev_rebuild(rf_t *a)
{
    char t[160];
    aos_hal_mutex_lock(a->mx);
    uint32_t n = a->ev_n, shown = n < 40 ? n : 40;
    if (n == a->ev_shown_n && n) {
        for (uint32_t k = 0; k < shown; k++)
            if (a->ev_title[k]) {
                ev_title(&a->ev[(n - 1 - k) % EV_MAX], t, sizeof t);
                if (strcmp(lv_label_get_text(a->ev_title[k]), t)) lv_label_set_text(a->ev_title[k], t);
            }
        aos_hal_mutex_unlock(a->mx);
        return;
    }
    aos_hal_mutex_unlock(a->mx);
    lv_obj_clean(a->ev_list);
    memset(a->ev_title, 0, sizeof a->ev_title);
    a->ev_empty = NULL;
    aos_hal_mutex_lock(a->mx);
    n = a->ev_n;
    shown = n < 40 ? n : 40;
    a->ev_shown_n = n;
    for (uint32_t k = 0; k < shown; k++) {
        uint32_t idx = n - 1 - k;
        const rf_event_t *ev = &a->ev[idx % EV_MAX];
        lv_obj_t *row = lv_obj_create(a->ev_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 16, 0);
        lv_obj_set_style_pad_all(row, 14, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 4, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, ev_detail, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);
        ev_title(ev, t, sizeof t);
        a->ev_title[k] = aos_label(row, t, aos_font_small, ev->d.proto[0] ? AOS_C_TEXT : AOS_C_DIM);
        ev_line(ev, t, sizeof t);
        lv_obj_t *l = aos_label(row, t, aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    aos_hal_mutex_unlock(a->mx);
    if (!n) {
        a->ev_empty = aos_label(a->ev_list, _("Esperando transmisiones en la banda: controles remotos, sensores de temperatura, timbres…"),
                                aos_font_body, AOS_C_DIM);
        lv_obj_set_width(a->ev_empty, LV_PCT(100));
        lv_label_set_long_mode(a->ev_empty, LV_LABEL_LONG_WRAP);
    }
}

/* the list where the waterfall was, in data mode */
static void mode_view(rf_t *a)
{
    bool data = a->want_mode == MODE_DATA;
    if (data) {
        lv_obj_add_flag(a->wf_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(a->ev_list, LV_OBJ_FLAG_HIDDEN);
        a->seen_ev = a->ev_seq - 1;
        a->ev_shown_n = (uint32_t)-1;       /* made again on the next tick */
    } else {
        lv_obj_add_flag(a->ev_list, LV_OBJ_FLAG_HIDDEN);
        if (!a->blit_ok) lv_obj_remove_flag(a->wf_img, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ev_detail(lv_event_t *e)
{
    rf_t *a = A;
    uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (!a || idx + EV_MAX < a->ev_n) return;          /* gone from the ring */
    rf_event_t ev;
    aos_hal_mutex_lock(a->mx);
    ev = a->ev[idx % EV_MAX];
    aos_hal_mutex_unlock(a->mx);
    lv_obj_t *card = sheet_open(a, ev.d.proto[0] ? ev.d.proto : _("Desconocido"));
    char t[700], line[160];
    int k = 0;
    ev_line(&ev, line, sizeof line);
    k += snprintf(t + k, sizeof t - k, "%s\n\n", line);
    k += snprintf(t + k, sizeof t - k, _("Hora: %02d:%02d:%02d · %d veces\n"), ev.when.tm_hour, ev.when.tm_min,
                  ev.when.tm_sec, ev.count);
    k += snprintf(t + k, sizeof t - k, _("Frecuencia: %u,%03u MHz (%+d kHz del centro) · %.0f dB sobre el ruido\n"),
                  (unsigned)(ev.freq / 1000000), (unsigned)(ev.freq % 1000000 / 1000), (int)(ev.off / 1000),
                  (double)ev.snr);
    k += snprintf(t + k, sizeof t - k, _("Modulación: %s · corto %d µs · largo %d µs · pausa %d µs\n"),
                  mod_name(ev.d.mod), ev.d.short_us, ev.d.long_us, ev.d.gap_us);
    k += snprintf(t + k, sizeof t - k, _("%d bits: %s"), ev.d.nbits, ev.d.hex);
    lv_obj_t *l = aos_label(card, t, aos_font_caption, AOS_C_TEXT);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
}

/* ---- keeping things --------------------------------------------------------- */

static void capture_now(lv_timer_t *t)
{
    char path[128];
    lv_timer_delete(t);
    if (aos_hal_display_save(path, sizeof path)) {
        char m[160];
        const char *base = strrchr(path, '/');
        snprintf(m, sizeof m, _("Captura guardada: %s"), base ? base + 1 : path);
        aos_ui_toast(m, 2500);
    } else aos_ui_toast(_("No se pudo guardar la captura"), 2000);
}

static void rec_action(lv_event_t *e)
{
    rf_t *a = A;
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    sheet_close(a);
    switch (what) {
    case 0:     /* the screen, once the sheet is gone and the spectrum drawn again */
        lv_timer_create(capture_now, 250, NULL);
        break;
    case 1:
        a->rec_wav = !a->rec_wav;
        aos_ui_toast(a->rec_wav ? _("Grabando el audio en la carpeta de la Grabadora") : _("Grabación de audio terminada"), 2000);
        break;
    case 2:
        a->rec_iq = !a->rec_iq;
        aos_ui_toast(a->rec_iq ? _("Grabando la señal en rf/iq de la tarjeta") : _("Grabación de la señal terminada"), 2000);
        break;
    case 3:
        a->play_stop = true;
        break;
    }
    show_buttons(a);
}

static void rec_switch(lv_event_t *e)
{
    rf_t *a = A;
    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if ((intptr_t)lv_event_get_user_data(e) == 0) a->log_on = on;
    else a->mqtt_on = on;
}

static void play_pick(lv_event_t *e);

static lv_obj_t *sheet_button(lv_obj_t *card, const char *text, lv_event_cb_t cb, intptr_t ud, bool on)
{
    lv_obj_t *b = button(card, text, cb, (void *)ud);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_style_bg_color(b, on ? AOS_C_RED : AOS_C_CARD2, 0);
    return b;
}

static lv_obj_t *sheet_switch(lv_obj_t *card, const char *text, bool on, intptr_t ud)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = aos_label(row, text, aos_font_caption, AOS_C_TEXT);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_t *sw = lv_switch_create(row);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, rec_switch, LV_EVENT_VALUE_CHANGED, (void *)ud);
    return sw;
}

static void play_list(lv_event_t *e);

static void rec_tap(lv_event_t *e)
{
    rf_t *a = lv_event_get_user_data(e);
    lv_obj_t *card = sheet_open(a, _("Guardar"));
    sheet_button(card, _("Captura de pantalla"), rec_action, 0, false);
    bool audio = a->want_mode != RF_MODE_OFF && a->want_mode != MODE_DATA;
    if (audio || a->rec_wav)
        sheet_button(card, a->rec_wav ? _("Terminar la grabación de audio") : _("Grabar el audio"), rec_action, 1, a->rec_wav);
    sheet_button(card, a->rec_iq ? _("Terminar la grabación de la señal") : _("Grabar la señal (I/Q)"), rec_action, 2,
                 a->rec_iq);
    if (a->playing) sheet_button(card, _("Dejar de reproducir"), rec_action, 3, true);
    else sheet_button(card, _("Reproducir una grabación"), play_list, 0, false);
    sheet_switch(card, _("Guardar lo recibido en modo Datos (rf/datos-<día>.csv)"), a->log_on, 0);
    sheet_switch(card, rf_mqtt_ready() ? _("Publicarlo por MQTT") : _("Publicarlo por MQTT (no conectado)"), a->mqtt_on, 1);
    uint32_t r = a->rate ? a->rate : a->want_rate;
    char t[200];
    snprintf(t, sizeof t, _("La señal ocupa %u MB por minuto a la tasa de ahora; la tarjeta escribe unos 3 MB/s."),
             (unsigned)((uint64_t)r * 2 * 60 / 1000000));
    lv_obj_t *l = aos_label(card, t, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
}

/* the recordings on the card, newest first: rf/iq/<when>_<hz>_<sps>.cu8 */
static void play_list(lv_event_t *e)
{
    (void)e;
    rf_t *a = A;
    lv_obj_t *card = sheet_open(a, _("Grabaciones"));
    char dir[96];
    const char *root = aos_hal_path_sd_root();
    snprintf(dir, sizeof dir, "%s/rf/iq", root ? root : aos_hal_path_data());
    DIR *dp = opendir(dir);
    static char names[24][64];
    int n = 0;
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) && n < 24) {
            size_t len = strlen(de->d_name);
            if (de->d_name[0] == '.' || len < 5 || strcmp(de->d_name + len - 4, ".cu8")) continue;
            snprintf(names[n++], sizeof names[0], "%.63s", de->d_name);
        }
        closedir(dp);
    }
    /* the names start with the date: backwards is newest first */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[j], names[i]) > 0) {
                char t[64];
                memcpy(t, names[i], sizeof t);
                memcpy(names[i], names[j], sizeof t);
                memcpy(names[j], t, sizeof t);
            }
    if (!n) aos_label(card, _("No hay grabaciones todavía."), aos_font_body, AOS_C_DIM);
    for (int i = 0; i < n; i++) {
        unsigned hz = 0, sps = 0;
        char when[20] = "";
        sscanf(names[i], "%19[^_]_%u_%u", when, &hz, &sps);
        char t[96];
        snprintf(t, sizeof t, "%s · %u,%03u MHz", when, hz / 1000000, hz % 1000000 / 1000);
        lv_obj_t *b = button_f(card, t, play_pick, (void *)names[i], aos_font_caption, 14);
        lv_obj_set_width(b, LV_PCT(100));
    }
}

static void play_pick(lv_event_t *e)
{
    rf_t *a = A;
    if (play_file(a, lv_event_get_user_data(e))) sheet_close(a);
}

/* plays rf/iq/<name> (its name says its centre and rate) */
static bool play_file(rf_t *a, const char *name)
{
    unsigned hz = 0, sps = 0;
    char when[20];
    if (strchr(name, '/') || sscanf(name, "%19[^_]_%u_%u", when, &hz, &sps) != 3 || !sps) {
        aos_ui_toast(_("No sé a qué frecuencia está grabada"), 2000);
        return false;
    }
    const char *root = aos_hal_path_sd_root();
    snprintf(a->play_path, sizeof a->play_path, "%s/rf/iq/%s", root ? root : aos_hal_path_data(), name);
    a->play_rate = sps;
    a->play_freq = hz;
    a->want_freq = hz;
    a->rec_iq = false;
    a->play_req = true;
    show_freq(a);
    place_ticks(a);
    return true;
}

/* ---- building ------------------------------------------------------------- */

static lv_obj_t *button_f(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud, const lv_font_t *font,
                          int pad)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, AOS_UI_TAP_MIN - 16);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, pad, 0);
    lv_obj_t *l = aos_label(b, text, font, AOS_C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
    return button_f(parent, text, cb, ud, aos_font_small, 20);
}

/* a row of controls across the screen */
static lv_obj_t *ctl_row(rf_t *a, int y, bool scroll)
{
    lv_obj_t *row = lv_obj_create(a->root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, a->W, AOS_UI_TAP_MIN - 8);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    if (scroll) {
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, AOS_UI_PAD, 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_scroll_dir(row, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    } else {
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    }
    return row;
}

static lv_obj_t *image_for(lv_obj_t *parent, lv_image_dsc_t *d, uint16_t *px, int w, int h)
{
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = w * 2;
    d->data_size = w * h * 2;
    d->data = (const uint8_t *)px;
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, d);
    return img;
}

static void build(rf_t *a, lv_obj_t *root)
{
    a->root = root;
    a->W = lv_obj_get_width(root);
    a->H = lv_obj_get_height(root);
    if (a->W > 1280) a->W = 1280;
    bool land = a->W > a->H;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* header: - frequency + and the status line */
    int head = land ? 96 : 150;
    lv_obj_t *minus = button(root, LV_SYMBOL_LEFT, step_by, (void *)(intptr_t)-1);
    lv_obj_align(minus, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 14);
    lv_obj_t *plus = button(root, LV_SYMBOL_RIGHT, step_by, (void *)(intptr_t)1);
    lv_obj_align(plus, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, 14);
    a->freq_lbl = aos_label(root, "", aos_font_large, AOS_C_TEXT);
    lv_obj_align(a->freq_lbl, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_add_flag(a->freq_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(a->freq_lbl, 20);
    lv_obj_add_event_cb(a->freq_lbl, freq_tap, LV_EVENT_CLICKED, a);
    a->status_lbl = aos_label(root, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(a->status_lbl, LV_ALIGN_TOP_MID, 0, land ? 70 : 96);

    /* controls at the bottom: how to listen; rate, gain, step, squelch;
     * the bands */
    int ctl = 3 * (AOS_UI_TAP_MIN - 8) + 8;
    int avail = a->H - head - ctl - 36;
    a->SH = avail * (land ? 45 : 36) / 100;
    a->WH = avail - a->SH;
    int y = head;

    a->spec_px = psram((size_t)a->W * a->SH * 2);
    a->wf_px = psram((size_t)a->W * a->WH * 2 * 2);
    a->col = psram(a->W * sizeof(float));
    a->smooth = psram(a->W * sizeof(float));
    a->yt = psram(a->W * sizeof(int16_t));
    a->yd = psram(a->W * sizeof(int16_t));
    a->vgrid = psram(a->W);
    a->hgrid = psram(a->SH);
    a->c_fill = psram(a->SH * sizeof(uint16_t));
    if (!a->spec_px || !a->wf_px || !a->col || !a->smooth || !a->yt || !a->yd || !a->vgrid || !a->hgrid || !a->c_fill) {
        free(a->spec_px);
        a->spec_px = NULL;
        return;
    }
    /* the tuned frequency is the column in the middle, drawn into the pixels */
    a->c_bg = rgb565(8, 10, 16);
    a->c_grid = rgb565(36, 40, 52);
    a->c_line = rgb565(120, 230, 255);
    a->c_mark = rgb565(200, 50, 50);
    a->c_markfill = rgb565(120, 40, 60);
    a->c_band = rgb565(26, 34, 58);
    a->c_bandgrid = rgb565(50, 60, 90);
    for (int r = 0; r < a->SH; r++) a->c_fill[r] = rgb565(10, 40 + 60 * (a->SH - r) / a->SH, 70 + 60 * (a->SH - r) / a->SH);
    a->blit_ok = true;
    for (int x = 0; x < a->W; x++) a->smooth[x] = -120;
    memset(a->spec_px, 0, (size_t)a->W * a->SH * 2);
    uint16_t dark = a->lut[0];
    for (size_t i = 0; i < (size_t)a->W * a->WH * 2; i++) a->wf_px[i] = dark;

    a->spec_img = image_for(root, &a->spec_dsc, a->spec_px, a->W, a->SH);
    lv_obj_set_pos(a->spec_img, 0, y);
    y += a->SH;
    for (int i = 0; i < TICKS_MAX; i++) {
        a->tick_lbl[i] = aos_label(root, "", aos_font_tiny, AOS_C_DIM);
        lv_obj_set_width(a->tick_lbl[i], 120);
        lv_obj_set_style_text_align(a->tick_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_y(a->tick_lbl[i], y + 6);
        lv_obj_add_flag(a->tick_lbl[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(a->tick_lbl[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(a->tick_lbl[i], 12);
        lv_obj_add_event_cb(a->tick_lbl[i], tick_tap, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    y += 36;
    a->wf_img = image_for(root, &a->wf_dsc, a->wf_px, a->W, a->WH);
    lv_obj_set_pos(a->wf_img, 0, y);
    /* data mode's list, where the waterfall is */
    a->ev_list = lv_obj_create(root);
    lv_obj_remove_style_all(a->ev_list);
    lv_obj_set_size(a->ev_list, a->W, a->WH);
    lv_obj_set_pos(a->ev_list, 0, y);
    lv_obj_set_style_pad_hor(a->ev_list, AOS_UI_PAD / 2, 0);
    lv_obj_set_style_pad_row(a->ev_list, 10, 0);
    lv_obj_set_flex_flow(a->ev_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(a->ev_list, LV_DIR_VER);
    lv_obj_add_flag(a->ev_list, LV_OBJ_FLAG_HIDDEN);
    y += a->WH;
    for (lv_obj_t **o = (lv_obj_t *[]){ a->spec_img, a->wf_img, NULL }; *o; o++) {
        lv_obj_add_flag(*o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(*o, spec_event, LV_EVENT_ALL, a);
    }

    int rh = AOS_UI_TAP_MIN - 8;
    lv_obj_t *modes = ctl_row(a, y + 6, false);
    for (int m = 0; m < MODES_N; m++)
        a->mode_btn[m] = button_f(modes, _(MODES[m]), mode_tap, (void *)(intptr_t)m, aos_font_caption, 14);
    lv_obj_t *row = ctl_row(a, y + 6 + rh, false);
    a->rate_btn = button_f(row, "", rate_cycle, a, aos_font_caption, 14);
    a->gain_btn = button_f(row, "", gain_tap, a, aos_font_caption, 14);
    a->step_btn = button_f(row, "", step_cycle, a, aos_font_caption, 14);
    a->sq_btn = button_f(row, "", sq_tap, a, aos_font_caption, 14);
    a->vol_btn = button_f(row, LV_SYMBOL_VOLUME_MAX, vol_tap, a, aos_font_small, 16);
    a->rec_btn = button_f(row, LV_SYMBOL_SAVE, rec_tap, a, aos_font_small, 16);
    lv_obj_t *bands = ctl_row(a, y + 6 + 2 * rh, true);
    for (size_t i = 0; i < sizeof BANDS / sizeof BANDS[0]; i++) {
        lv_obj_t *b = button(bands, _(BANDS[i].name), band_tap, (void *)(intptr_t)i);
        lv_obj_set_style_bg_color(b, AOS_C_CARD, 0);
    }

    /* no radio yet: a card over the spectrum */
    a->empty = lv_obj_create(root);
    lv_obj_set_size(a->empty, a->W - 2 * AOS_UI_PAD, LV_SIZE_CONTENT);
    lv_obj_align(a->empty, LV_ALIGN_TOP_MID, 0, head + 40);
    lv_obj_set_style_bg_color(a->empty, AOS_C_CARD, 0);
    lv_obj_set_style_border_width(a->empty, 0, 0);
    lv_obj_set_style_radius(a->empty, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(a->empty, AOS_UI_PAD, 0);
    a->empty_lbl = aos_label(a->empty, "", aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(a->empty_lbl, LV_PCT(100));
    lv_label_set_long_mode(a->empty_lbl, LV_LABEL_LONG_WRAP);

    show_freq(a);
    show_buttons(a);
    place_ticks(a);
    mode_view(a);
    ev_rebuild(a);
}

/* ---- the app -------------------------------------------------------------- */

static uint32_t pref_u32(const char *key, uint32_t def)
{
    char s[16];
    return aos_hal_pref_get_str(key, s, sizeof s) ? (uint32_t)strtoul(s, NULL, 10) : def;
}

static int pref_int(const char *key, int def)
{
    char s[16];
    return aos_hal_pref_get_str(key, s, sizeof s) ? (int)strtol(s, NULL, 10) : def;
}

static void pref_put(const char *key, long v)
{
    char s[16];
    snprintf(s, sizeof s, "%ld", v);
    aos_hal_pref_set_str(key, s);
}

static void *rf_create(aos_app_t *self, lv_obj_t *root)
{
    rf_t *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    A = a;
    a->self = self;
    a->mx = aos_hal_mutex_create();
    a->spec = psram(RF_FFT * sizeof(float));
    a->ui_spec = psram(RF_FFT * sizeof(float));
    a->want_freq = pref_u32("rf_freq", 98000000);
    a->want_rate = pref_u32("rf_rate", 2400000);
    /* a rate from before the demodulators (2.048 M, 250 k...): the nearest of now */
    if (a->want_rate % 48000 || (a->want_rate != 240000 && a->want_rate < 960000)) a->want_rate = a->want_rate < 900000 ? 240000 : 2400000;
    a->want_mode = pref_int("rf_mode", RF_MODE_OFF);
    if (a->want_mode < 0 || a->want_mode >= MODES_N) a->want_mode = RF_MODE_OFF;
    a->want_sq = pref_int("rf_sq", 10);
    a->log_on = pref_int("rf_log", 1) != 0;
    a->mqtt_on = pref_int("rf_mqtt", 0) != 0;
    a->ev = psram(EV_MAX * sizeof(rf_event_t));
    if (a->ev) memset(a->ev, 0, EV_MAX * sizeof(rf_event_t));
    a->want_gain = pref_int("rf_gain", -1);
    a->step = pref_u32("rf_step", 100000);
    a->state = ST_SEARCH;
    build_lut(a->lut);
    build(a, root);
    if (!a->spec_px || !a->spec || !a->ui_spec || !a->ev) {
        aos_ui_toast(_("Sin memoria"), 2000);
        return a;
    }
    a->timer = lv_timer_create(ui_timer, 1000 / RF_FPS / 2, a);
    /* priority 1, the system's "tick" thread's own on core 0: if demodulating
     * ever takes the whole core, they take turns and the board goes on (at
     * 3 a busy engine starved tick and the hang watchdog restarted the board,
     * 2026-10-07); what the engine cannot keep up with is dropped and counted */
    if (!aos_hal_thread_start("rf", engine, a, 16 * 1024, 1)) {
        a->done = true;
        aos_ui_toast(_("No pude arrancar la radio"), 2000);
    }
    return a;
}

static void rf_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    rf_t *a = inst;
    aos_hal_live_clear(LIVE_APP);
    a->stop = true;
    /* the engine closes the radio and the speaker on its way out */
    for (int i = 0; i < 300 && !a->done; i++) aos_hal_sleep_ms(10);
    if (a->timer) lv_timer_delete(a->timer);
    if (!a->done) {
        /* stuck in a USB call: leaking beats freeing under it */
        aos_hal_log("rf", "the engine did not stop; its memory is left");
        return;
    }
    pref_put("rf_freq", (long)a->want_freq);
    pref_put("rf_rate", (long)a->want_rate);
    pref_put("rf_gain", (long)a->want_gain);
    pref_put("rf_step", (long)a->step);
    pref_put("rf_mode", a->want_mode);
    pref_put("rf_sq", a->want_sq);
    pref_put("rf_log", a->log_on);
    pref_put("rf_mqtt", a->mqtt_on);
    free(a->ev);
    free(a->spec);
    free(a->ui_spec);
    free(a->spec_px);
    free(a->wf_px);
    free(a->col);
    free(a->smooth);
    free(a->yt);
    free(a->yd);
    free(a->vgrid);
    free(a->hgrid);
    free(a->c_fill);
    if (A == a) A = NULL;
    free(a);
}

static void rf_hide(aos_app_t *self, void *inst)
{
    (void)self;
    rf_t *a = inst;
    a->hidden = true;
    a->paused = a->want_mode == RF_MODE_OFF;
}

static void rf_show(aos_app_t *self, void *inst)
{
    (void)self;
    ((rf_t *)inst)->hidden = false;
}

static bool rf_init(aos_app_t *app)
{
    app->desc.id = "aos.rf";
    app->desc.name = "RF";
    app->desc.icon = LV_SYMBOL_WIFI;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a = 0x2F6BFF;
    app->desc.color_b = 0x7A2BD9;
    app->desc.order = 160;
    app->desc.flags = AOS_APP_FLAG_LONG_DRAG | AOS_APP_FLAG_KEEP_AWAKE;
    aos_icon_set_ops(app, RF_ICON, sizeof RF_ICON);
    app->create = rf_create;
    app->destroy = rf_destroy;
    app->hide = rf_hide;
    app->show = rf_show;
    return true;
}

AOS_APP_ENTRY(rf_init);
