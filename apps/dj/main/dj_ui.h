/*
 * DJ - the interface's shared state.
 *
 * The decks and the mixer are not LVGL widgets: LVGL's pointer is one
 * finger, and a DJ has a hand on each platter and one on the crossfader. So
 * every control is a rectangle in a table (ctl_t), drawn by one draw event
 * of one object, and a timer reads all the fingers (aos_touch_points_n, up
 * to five) every 10 ms and hits them against the table itself, like
 * Estudio's keyboard. Each finger owns the control it landed on until it
 * lifts. The waveforms and the overviews are canvases filled pixel by pixel
 * (dj_wave.c). Only the sheets (the library, the settings, the grid) are
 * ordinary LVGL, and while one is up the surface takes no fingers.
 */
#pragma once

#include "dj.h"

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "lvgl.h"

#define DJ_COL_A   0x3DA5FFu
#define DJ_COL_B   0xFF7A3Du
#define DJ_COL_BG  0x0B0B0Du
#define DJ_COL_PNL 0x161618u
#define DJ_COL_BTN 0x2A2A2Eu
#define DJ_COL_DIM 0x8E8E93u

enum {
    K_BTN, K_KNOB, K_VFADER, K_HFADER, K_PLATTER, K_WAVE, K_OVERVIEW, K_INFO, K_VU, K_TEXT,
};

/* what each control does */
enum {
    C_PLATTER, C_PITCH, C_PLAY, C_CUE, C_SYNC, C_HOT1, C_HOT2, C_HOT3, C_HOT4,
    C_LOOP, C_LOOP_HALF, C_LOOP_DBL, C_JUMP_BACK, C_JUMP_FWD, C_LOAD,
    C_FX_TYPE, C_FX_ON, C_FX_AMT, C_FX_BEATS,
    C_EQ_HI, C_EQ_MID, C_EQ_LO, C_FILTER, C_VOL, C_VU, C_INFO, C_OVERVIEW, C_WAVE,
    /* the mixer's own */
    C_XFADE, C_MASTER, C_MVU, C_REC, C_LIB, C_SETTINGS, C_CLOCK,
    C_COUNT
};

typedef struct {
    uint8_t   kind;
    uint8_t   id;
    int8_t    deck;             /* -1 for the mixer's */
    lv_area_t r;                /* in the surface's coordinates */
    float    *val;              /* knobs and faders */
    float     lo, hi, def;
    const char *label;
    uint32_t  color;
    /* what was drawn last, to know when to draw again */
    uint32_t  sig;
    bool      lit;              /* a finger on it (drawn brighter) */
} ctl_t;

typedef struct {
    uint8_t  id;                /* the finger's, 0 = none */
    int16_t  ctl;               /* the control it holds, -1 none */
    float    x0, y0, lx, ly;    /* where it landed, where it was last */
    float    v0;                /* the control's value when it landed */
    float    ang;               /* the platter: the last angle */
    uint32_t t;                 /* when it was last seen moving (the platter's nudge) */
    bool     ring;              /* the platter's outer ring: a nudge, not a scratch */
} finger_t;

#define DJ_MAXCTL 96

typedef struct {
    uint32_t gen;
    float    cue, hot[DJ_HOT], bpm, beat0;
    bool     dirty;
    bool     hand;              /* the grid was set by hand */
    uint32_t dirty_ms;
} meta_track_t;

typedef struct {
    aos_app_t   *self;
    lv_obj_t    *root, *surf;
    dj_engine_t *e;
    dj_mix_t    *m;
    int32_t      W, H;
    bool         land;
    bool         audio_ok;

    ctl_t        ctl[DJ_MAXCTL];
    int          nctl;
    int          idx[C_COUNT][DJ_NDECK + 1];   /* control id + deck -> index (deck 2 = the mixer) */
    finger_t     f[AOS_TOUCH_MAX];
    uint8_t      stale[AOS_TOUCH_MAX];
    bool         covered;
    uint32_t     tap_ms[DJ_MAXCTL];             /* for a double tap */
    float        jog_bend[DJ_NDECK];

    /* the canvases */
    lv_obj_t    *wave_cv, *ov_cv[DJ_NDECK];
    uint16_t    *wave_buf, *ov_buf[DJ_NDECK];
    int32_t      wave_w, wave_h, ov_w, ov_h;
    lv_area_t    wave_r;                        /* both strips, in the surface */
    uint32_t     ov_sig[DJ_NDECK];
    int          zoom;                          /* 0..3 */

    lv_timer_t  *t_ui, *t_touch;
    lv_obj_t    *sheet;
    void       (*sheet_close)(void);
    int          load_deck;

    meta_track_t mt[DJ_NDECK];
    float        fx_amt_def;

    /* MIDI learn */
    int          learn;                         /* 0 off, 1 waiting for a control, 2 for a message */
    int          learn_ctl;

    /* stats */
    uint32_t     log_ms, frames, draw_us, draw_n, touch_n;
    uint32_t     blink;
} dj_ui_t;

extern dj_ui_t *dj_ui;

/* dj_ui.c */
void dj_ui_build(void);
void dj_ui_timers(bool on);
void dj_ui_release_all(void);
void dj_ui_tick(void);
void dj_ui_save_settings(void);
void dj_ui_load_settings(void);
void dj_ui_meta_flush(bool force);
int  dj_ui_find(int id, int deck);
void dj_ui_dirty(int i);
void dj_ui_grid_by_hand(int deck);
const char *dj_fmt_time(char *buf, size_t n, float secs);

/* dj_wave.c */
bool dj_wave_create(void);
void dj_wave_destroy(void);
void dj_wave_render(void);
void dj_ov_render(int deck, bool force);

/* dj_lib.c: the sheets */
void dj_lib_open(int deck);
void dj_settings_open(void);
void dj_grid_open(int deck);
void dj_sheet_close(void);
void dj_lib_forget(void);
void dj_sheet_tick(void);

/* dj_midi.c */
void dj_midi_poll(void);
void dj_midi_load(void);
void dj_midi_clear(void);
void dj_midi_learn_ctl(int ctl);
int  dj_midi_count(void);

/* dj_meta.c */
float dj_meta_bpm(const char *name, uint32_t size, char *cache, int cache_len);
char *dj_meta_read_all(int *len);
void  dj_meta_save(const char *path, uint32_t size, float bpm, float beat0, float cue, const float *hot, bool hand);

/* actions the touch, MIDI and the joystick share */
void dj_act_press(int ctl);
void dj_act_release(int ctl);
void dj_act_value(int ctl, float v);
