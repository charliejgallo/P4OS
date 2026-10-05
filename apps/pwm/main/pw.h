/*
 * P4OS - PWM generator: what the app's files share.
 *
 * Up to seven channels, each on a header pin of its own, each in one of four
 * modes: free PWM (frequency and duty), a servo (50 Hz and a pulse width), a
 * LED (brightness through a gamma curve) and an analog level (the
 * sigma-delta modulator through an RC filter). A channel's value is kept
 * normalised, 0..1 of its range, so the knob, the patterns and the portal
 * treat all four alike; pw_engine.c turns it into a duty or a level.
 *
 *   pwm.c        the app: icon, life cycle
 *   pw_engine.c  the hardware, the patterns, the conversions and formats
 *   pw_knob.c    the big knob (finger, two fingers, the mouse wheel)
 *   pw_ui.c      the screen, both orientations
 *   pw_sheets.c  pins, channel settings, steps, presets, wiring
 *   pw_store.c   the files on the card: presets and the portal's two files
 *
 * There is one instance at a time, so its state is a global (pw_g).
 */
#pragma once

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_io.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PW_OWNER      "PWM"         /* who holds the pins, as other apps show it */
#define PW_CH_MAX     7             /* the LEDC's channels (the eighth is the backlight) */
#define PW_STEPS_MAX  8
#define PW_TICK_MS    20
#define PW_FREQ_MAX   20000000u
#define PW_PERIOD_MIN 0.05f         /* a pattern's cycle, seconds */
#define PW_PERIOD_MAX 60.0f

enum { PW_M_PWM, PW_M_SERVO, PW_M_LED, PW_M_DAC, PW_M_COUNT };
enum { PW_P_NONE, PW_P_BREATHE, PW_P_SWEEP, PW_P_STROBE, PW_P_RAMP, PW_P_SINE, PW_P_STEPS, PW_P_COUNT };
enum { PW_T_VALUE, PW_T_FREQ, PW_T_SPEED };     /* what the knob turns */

/* What a channel is: saved in presets, mirrored to the portal. */
typedef struct {
    int8_t   gpio;              /* -1: no pin yet */
    uint8_t  mode;
    bool     on;
    bool     invert;
    uint32_t freq;              /* asked, PWM and LED */
    float    duty;              /* PWM, 0..1 */
    float    pulse;             /* servo, us */
    uint16_t smin, smax;        /* servo stops, us */
    uint16_t sdeg;              /* servo travel, degrees */
    float    bright;            /* LED, 0..1 as seen */
    float    gamma;             /* LED curve */
    float    level;             /* analog, 0..1 of 3.3 V */
    uint8_t  pat;
    uint8_t  nsteps;
    float    period;            /* s, one cycle of the pattern */
    float    lo, hi;            /* the pattern's span, 0..1 of the range */
    float    step[PW_STEPS_MAX];/* 0..1 of the range */
} pw_cfg_t;

/* A channel: what it is and what the hardware is doing. */
typedef struct {
    pw_cfg_t      c;
    aos_io_pwm_t *pwm;
    aos_io_dac_t *dac;
    char          err[96];
    float         live;         /* 0..1 of the range, out now (patterns move it) */
    float         hw;           /* last duty or level written */
    bool          hw_valid;
    bool          dirty;        /* the hardware is behind the config */
    bool          freq_dirty;
    bool          inv_dirty;
    uint32_t      asked;        /* the frequency the timer was asked for */
    uint32_t      freq_ms;      /* last frequency change: they are spaced */
    uint32_t      busy_until;   /* a hardware fade runs: no writes before */
    uint32_t      t0;           /* the pattern's start */
} pw_ch_t;

typedef struct {
    lv_obj_t *catcher, *arc, *input, *what, *val, *sub, *fine;
    float     last_ang;
    float     acc;              /* the drag's position, in the target's units */
    bool      dragging, recentering;
    int32_t   park;             /* the catcher's resting scroll */
} pw_knob_t;

typedef struct {
    aos_app_t *app;
    lv_obj_t  *root;
    int32_t    W, H;
    bool       land;

    pw_ch_t    ch[PW_CH_MAX];
    int        n;               /* channels in use, ch[0..n-1] */
    int        sel;
    int        target;

    lv_timer_t *engine, *io;
    uint32_t    ticks;

    /* the card */
    uint32_t    save_due;       /* 0: nothing to write */
    uint32_t    ack;            /* the portal's last request taken */

    /* the screen */
    lv_obj_t   *hdr_info, *strip, *chip[PW_CH_MAX], *chip_add;
    lv_obj_t   *pin_btn, *pin_lbl, *mode_bm, *gear, *power;
    lv_obj_t   *target_bm, *dec_dn, *dec_up, *err_lbl;
    lv_obj_t   *minus, *plus;
    pw_knob_t   knob;
    lv_obj_t   *signal;
    lv_obj_t   *pat_bm, *range, *range_lbl, *period_lbl, *steps_btn, *pat_note;
    char        chip_txt[PW_CH_MAX][48];
    float       shown_live;

    lv_obj_t   *sheet;          /* an open sheet over the screen */
    lv_obj_t   *sheet_dying;
    lv_obj_t   *sheet_ta, *sheet_kb;
    int         wire_tab;
} pw_t;

extern pw_t *pw_g;

static inline pw_ch_t *pw_cur(void) { return pw_g->n ? &pw_g->ch[pw_g->sel] : NULL; }

/* ---- pw_engine.c ---- */
void     pw_cfg_default(pw_cfg_t *c, int mode);
float    pw_val(const pw_cfg_t *c);                 /* the knob's value, 0..1 */
void     pw_val_set(pw_cfg_t *c, float v);
float    pw_out(const pw_ch_t *h, float v);         /* 0..1 of the range -> duty or level */
uint32_t pw_freq_of(const pw_cfg_t *c);             /* what the timer is asked */
bool     pw_has_freq(int mode);
uint32_t pw_nice_freq(float hz);
float    pw_pat_at(const pw_cfg_t *c, float ph);    /* 0..1 of the range at phase ph */
bool     pw_pat_smooth(int pat);

int      pw_add(int mode);                          /* index, -1 if full */
void     pw_remove(int i);
bool     pw_set_on(pw_ch_t *h, bool on);
void     pw_set_gpio(pw_ch_t *h, int gpio);
void     pw_set_mode(pw_ch_t *h, int mode);
void     pw_set_freq(pw_ch_t *h, uint32_t hz);
void     pw_set_pattern(pw_ch_t *h, int pat);
void     pw_touch(pw_ch_t *h);                      /* the value changed: send it */
void     pw_all_off(void);
void     pw_close_all(void);
void     pw_apply(const pw_cfg_t *cfg, int n);      /* a preset, or the portal's request */
int      pw_freqs_in_use(uint32_t out[3]);
int      pw_gpio_user(int gpio, int except);         /* the channel with that pin, -1 */
void     pw_engine_start(void);
void     pw_engine_stop(void);

void     pw_fnum(char *b, size_t n, float v, int dec);    /* decimal comma */
void     pw_fmt_freq(char *b, size_t n, float hz);
void     pw_fmt_time(char *b, size_t n, float s);
void     pw_fmt_value(const pw_ch_t *h, float v, char *big, size_t nb, char *sub, size_t ns);
const char *pw_mode_name(int mode);
const char *pw_pat_name(int pat);
uint32_t pw_mode_color(int mode);

/* ---- pw_knob.c ---- */
void     pw_knob_build(lv_obj_t *parent, int32_t size);
void     pw_knob_refresh(void);
void     pw_knob_nudge(int dir);                    /* -/+ buttons: a hundredth */

/* ---- pw_ui.c ---- */
void     pw_ui_build(void);
void     pw_ui_refresh(void);
void     pw_ui_live(void);
void     pw_ui_select(int i);
lv_obj_t *pw_pill(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, void *user);
lv_obj_t *pw_bm(lv_obj_t *p, const char **map, int32_t w, int32_t h, const lv_font_t *font, lv_event_cb_t cb);
void     pw_bm_set(lv_obj_t *bm, int count, int checked, uint32_t disabled_mask);

/* ---- pw_sheets.c ---- */
lv_obj_t *pw_sheet_open(const char *title);         /* the sheet's body */
void     pw_sheet_close(void);
void     pw_sheet_forget(void);
void     pw_sheet_pins(void);
void     pw_sheet_settings(void);
void     pw_sheet_steps(void);
void     pw_sheet_presets(void);
void     pw_sheet_wiring(void);

/* ---- pw_draw.c ---- */
void     pw_draw_signal(lv_event_t *e);
void     pw_draw_schematic(lv_event_t *e);           /* the tab in pw_g->wire_tab */

/* ---- pw_store.c ---- */
void     pw_store_dir(char *out, size_t n);         /* /sdcard/pwm, "" without a card */
bool     pw_store_save(const char *path);
int      pw_store_load(const char *path, pw_cfg_t *out, int max, uint32_t *seq);
void     pw_store_changed(void);                    /* write the state soon */
void     pw_store_restore(void);                    /* at start: the last state, outputs off */
void     pw_store_poll(void);                       /* the io timer */
void     pw_store_final(void);                      /* destroy: run=0 */
