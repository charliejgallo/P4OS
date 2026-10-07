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

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
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
} band_t;

/* centre and step of common bands; anyone's, not a list of stations */
static const band_t BANDS[] = {
    { N_("FM"),     98000000,  100000 },
    { N_("Aire"),   125000000, 25000 },
    { N_("Marina"), 156800000, 25000 },
    { "2 m",        145000000, 12500 },
    { "433",        433920000, 25000 },
    { "70 cm",      435000000, 12500 },
    { "PMR",        446100000, 12500 },
    { "868",        868300000, 25000 },
    { "ADS-B",      1090000000, 1000000 },
};
static const uint32_t STEPS[] = { 1000, 5000, 10000, 12500, 25000, 100000, 1000000 };
static const uint32_t RATES_HS[] = { 1024000, 1800000, 2048000, 2400000 };
static const uint32_t RATES_FS[] = { 250000 };

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
    volatile bool paused;                   /* hidden || locked */
    bool hidden;
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
    lv_obj_t *freq_lbl, *status_lbl, *rate_btn, *gain_btn, *step_btn;
    lv_obj_t *spec_img, *wf_img, *empty, *empty_lbl;
    lv_obj_t *tick_lbl[TICKS_MAX];
    int16_t tick_x[TICKS_MAX];
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
    lv_obj_t *gain_slider, *gain_val;
} rf_t;

static rf_t *A;

static void *psram(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n ? n : 1);
#else
    return malloc(n ? n : 1);
#endif
}

/* ---- the worker ----------------------------------------------------------- */

static void worker(void *arg)
{
    rf_t *a = arg;
    uint8_t *buf = psram(READ_BYTES), *frame = psram(RF_FFT * 2);
    float *db = psram(RF_FFT * sizeof(float));
    rf_fft_t *fft = rf_fft_new(RF_FFT);
    if (!buf || !frame || !db || !fft) {
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
    while (!aos_hal_worker_should_stop()) {
        if (a->paused) {
            if (src && started) {
                src->ops->stop(src);
                started = false;
            }
            aos_hal_worker_sleep(100);
            continue;
        }
        if (!src) {
            if (aos_hal_uptime_ms() < next_try) {
                aos_hal_worker_sleep(100);
                continue;
            }
            const char *why = NULL;
            src = rf_src_rtl.open(&why);
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
        if (want_rate != cur_rate) {
            if (started) src->ops->stop(src);
            started = false;
            uint32_t got = src->ops->set_rate(src, want_rate);
            a->rate = got ? got : a->rate;
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
                    rf_fft_take_db(fft, db);
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
            aos_hal_log("rf", "worker in %u ms: read %u ms, %u FFTs %u ms, dB %u ms; UI drew %u frames in %u ms",
                        span, (unsigned)(t_read / 1000), n_fft, (unsigned)(t_fft / 1000), (unsigned)(t_take / 1000),
                        a->ui_frames, a->ui_draw_us / 1000);
            t_read = t_fft = t_take = 0;
            n_fft = 0;
            a->ui_frames = a->ui_draw_us = 0;
            t_log = now;
        }
    }
    if (src) src->ops->close(src);
out:
    free(buf);
    free(frame);
    free(db);
    rf_fft_free(fft);
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
        a->tick_x[a->nticks++] = (int16_t)x;
    }
    for (int i = a->nticks; i < TICKS_MAX; i++) lv_obj_add_flag(a->tick_lbl[i], LV_OBJ_FLAG_HIDDEN);
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
    if (y < t) return x == a->W / 2 ? a->c_mark : a->hgrid[y] || a->vgrid[x] ? a->c_grid : a->c_bg;
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
        lv_obj_get_coords(a->wf_img, &c);
        ok = ok && aos_hal_display_blit_scaled(c.x1, c.y1, a->W, a->WH, a->wf_px + a->wf_row * a->W, 1, false);
        if (ok) return;
        a->blit_ok = false;         /* the simulator: LVGL from now on */
    }
    if (lv_obj_has_flag(a->spec_img, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(a->spec_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(a->wf_img, LV_OBJ_FLAG_HIDDEN);
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
    if (a->want_gain < 0) snprintf(s, sizeof s, "%s", _("Ganancia auto"));
    else snprintf(s, sizeof s, _("Ganancia %d,%d dB"), a->want_gain / 10, a->want_gain % 10);
    lv_label_set_text(lv_obj_get_child(a->gain_btn, 0), s);
    if (a->step >= 1000000) snprintf(s, sizeof s, _("Paso %u MHz"), (unsigned)(a->step / 1000000));
    else if (a->step % 1000) snprintf(s, sizeof s, _("Paso %u,%u kHz"), (unsigned)(a->step / 1000), (unsigned)(a->step % 1000 / 100));
    else snprintf(s, sizeof s, _("Paso %u kHz"), (unsigned)(a->step / 1000));
    lv_label_set_text(lv_obj_get_child(a->step_btn, 0), s);
}

static uint32_t clamp_freq(rf_t *a, int64_t f)
{
    int64_t lo = a->info.fmin ? a->info.fmin : 24000000, hi = a->info.fmax ? a->info.fmax : 1766000000;
    return (uint32_t)(f < lo ? lo : f > hi ? hi : f);
}

static void tune(rf_t *a, int64_t hz, bool snap)
{
    if (snap && a->step) hz = (hz + a->step / 2) / a->step * a->step;
    a->want_freq = clamp_freq(a, hz);
    show_freq(a);
}

static void ui_timer(lv_timer_t *t)
{
    rf_t *a = lv_timer_get_user_data(t);
    /* the spectrum is only worth its 4 MB/s on screen: the stream rests
     * while the board is locked (listening, later, will keep it going) */
    a->paused = a->hidden || (aos_ui_overlay() & AOS_UI_OVER_LOCK);
    if (a->state == ST_RUN) {
        if (!lv_obj_has_flag(a->empty, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(a->empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(a->empty, LV_OBJ_FLAG_HIDDEN);
        char s[400];
        if (a->state == ST_SEARCH) snprintf(s, sizeof s, "%s", _("Buscando una RTL-SDR…"));
        else
            snprintf(s, sizeof s, "%s\n\n%s", a->why ? _(a->why) : "",
                     aos_hal_usb_host_on()
                         ? _("Enchufala en el host USB: pines 25/27 del conector de atrás (High Speed, cables de menos de 15 cm) y 5 V del pin 1. Los pines 21/23 son Full Speed: alcanzan para 250 mil muestras por segundo.")
                         : _("El host USB está apagado: prendelo en Ajustes → USB → Host USB, y enchufá la RTL-SDR en los pines 25/27 del conector de atrás."));
        lv_label_set_text(a->empty_lbl, s);
    }
    if (a->info_seq != a->seen_info) {
        a->seen_info = a->info_seq;
        a->want_freq = clamp_freq(a, a->want_freq);
        show_freq(a);
        show_buttons(a);
        place_ticks(a);
    }
    if (a->spec_seq != a->seen_spec && a->state == ST_RUN) {
        aos_hal_mutex_lock(a->mx);
        memcpy(a->ui_spec, a->spec, RF_FFT * sizeof(float));
        a->seen_spec = a->spec_seq;
        aos_hal_mutex_unlock(a->mx);
        uint64_t t0 = aos_hal_uptime_us();
        draw_spectrum(a);
        a->ui_draw_us += (unsigned)(aos_hal_uptime_us() - t0);
        a->ui_frames++;
    }
    uint64_t now = aos_hal_uptime_ms();
    if (now - a->stat_at >= 1000) {
        uint64_t b = a->bytes;
        /* tenths of MB/s, with a decimal comma */
        unsigned mbs10 = a->stat_at && b >= a->stat_bytes ? (unsigned)((b - a->stat_bytes) * 10 / 1000 / (now - a->stat_at)) : 0;
        char s[160];
        if (a->state == ST_RUN)
            snprintf(s, sizeof s, "%s %s · %s · %u,%u MB/s · %s %u", a->info.kind, a->info.tuner,
                     a->info.high_speed ? "High Speed" : "Full Speed", mbs10 / 10, mbs10 % 10, _("perdidos"),
                     (unsigned)a->dropped);
        else snprintf(s, sizeof s, "%s", _("Sin radio"));
        lv_label_set_text(a->status_lbl, s);
        a->stat_at = now;
        a->stat_bytes = b;
        if (a->rate && a->nticks == 0) place_ticks(a);
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
            /* a tap: that frequency to the centre */
            lv_area_t c;
            lv_obj_get_coords(lv_event_get_target_obj(e), &c);
            tune(a, (int64_t)a->want_freq + (int64_t)(p.x - c.x1 - a->W / 2) * span / a->W, true);
        }
        place_ticks(a);
        break;
    default:
        break;
    }
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
    const uint32_t *R = a->info.high_speed ? RATES_HS : RATES_FS;
    int n = a->info.high_speed ? 4 : 1, i = 0;
    while (i < n && R[i] != a->want_rate) i++;
    a->want_rate = R[(i + 1) % n];
    a->rate = 0;
    show_buttons(a);
    place_ticks(a);
    if (!a->info.high_speed) aos_ui_toast(_("En los pines 21/23 (Full Speed) sólo entran 250 mil muestras por segundo"), 2500);
}

static void band_tap(lv_event_t *e)
{
    rf_t *a = A;
    const band_t *b = &BANDS[(intptr_t)lv_event_get_user_data(e)];
    a->step = b->step;
    tune(a, b->hz, false);
    show_buttons(a);
    place_ticks(a);
}

static void sheet_close(rf_t *a)
{
    if (a->sheet) lv_obj_delete(a->sheet);
    a->sheet = a->ta = a->gain_slider = a->gain_val = NULL;
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

/* ---- building ------------------------------------------------------------- */

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, AOS_UI_TAP_MIN - 16);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 20, 0);
    lv_obj_t *l = aos_label(b, text, aos_font_small, AOS_C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
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

    /* controls at the bottom: rate, gain, step, then the bands */
    int ctl = 2 * (AOS_UI_TAP_MIN - 4);
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
    }
    y += 36;
    a->wf_img = image_for(root, &a->wf_dsc, a->wf_px, a->W, a->WH);
    lv_obj_set_pos(a->wf_img, 0, y);
    y += a->WH;
    for (lv_obj_t **o = (lv_obj_t *[]){ a->spec_img, a->wf_img, NULL }; *o; o++) {
        lv_obj_add_flag(*o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(*o, spec_event, LV_EVENT_ALL, a);
    }

    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, a->W, AOS_UI_TAP_MIN - 4);
    lv_obj_set_pos(row, 0, y + 6);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    a->rate_btn = button(row, "", rate_cycle, a);
    a->gain_btn = button(row, "", gain_tap, a);
    a->step_btn = button(row, "", step_cycle, a);

    lv_obj_t *bands = lv_obj_create(root);
    lv_obj_remove_style_all(bands);
    lv_obj_set_size(bands, a->W, AOS_UI_TAP_MIN - 4);
    lv_obj_set_pos(bands, 0, y + 6 + AOS_UI_TAP_MIN - 4);
    lv_obj_set_flex_flow(bands, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bands, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bands, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_column(bands, 12, 0);
    lv_obj_set_scroll_dir(bands, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(bands, LV_SCROLLBAR_MODE_OFF);
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
    a->want_rate = pref_u32("rf_rate", 2048000);
    a->want_gain = pref_int("rf_gain", -1);
    a->step = pref_u32("rf_step", 100000);
    a->state = ST_SEARCH;
    build_lut(a->lut);
    build(a, root);
    if (!a->spec_px || !a->spec || !a->ui_spec) {
        aos_ui_toast(_("Sin memoria"), 2000);
        return a;
    }
    a->timer = lv_timer_create(ui_timer, 1000 / RF_FPS / 2, a);
    if (!aos_hal_worker_start_on("rf", worker, a, 16 * 1024, 0, 3)) aos_ui_toast(_("No pude arrancar la radio"), 2000);
    return a;
}

static void rf_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    rf_t *a = inst;
    aos_hal_worker_stop();
    if (a->timer) lv_timer_delete(a->timer);
    pref_put("rf_freq", (long)a->want_freq);
    pref_put("rf_rate", (long)a->want_rate);
    pref_put("rf_gain", (long)a->want_gain);
    pref_put("rf_step", (long)a->step);
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
    ((rf_t *)inst)->hidden = true;
    ((rf_t *)inst)->paused = true;
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
