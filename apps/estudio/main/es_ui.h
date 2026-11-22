/*
 * Estudio - the interface, shared by the es_ui*.c files. Everything here
 * runs in LVGL's thread.
 *
 * The screen is a bar (the transport, the tempo, the six views) and a body
 * that each view builds and tears down: Tocar (the keyboard, the scale grid
 * and the pads), Ritmo (the drum steps), Notas (the piano roll), Sonido (the
 * synth), Mezcla (the mixer and the effects) and Canción (the patterns, the
 * song, the project, the export, MIDI and the audio settings). Lying down is
 * the main view, with the keyboard wide; standing up the sequencer's grid
 * turns so that the steps run down the screen.
 */
#pragma once

#include "es.h"

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "lvgl.h"

enum { V_PLAY, V_DRUMS, V_ROLL, V_SOUND, V_MIX, V_SONG, V_COUNT };
enum { PM_KEYS, PM_GRID, PM_PADS };

#define C_BG      0x0B0B10
#define C_PANEL   0x17171E
#define C_CELL    0x24242D
#define C_CELL2   0x2E2E39
#define C_LINE    0x3A3A46
#define C_TEXT    0xF2F2F7
#define C_DIM     0x8E8E93
#define C_REC     0xFF453A
#define C_PLAY    0x30D158

#define ES_BINDS  96
#define U_MFILT   99         /* ui_fmt: the master filter, -1..1 */

/* a slider bound to a float of the project */
typedef struct {
    float   *v;
    float    min, max;
    uint8_t  fmt;               /* U_* */
    int      param;             /* >= 0: a patch parameter, the table drives it */
    lv_obj_t *val;              /* the value's label */
    void   (*after)(void);
} es_bind_t;

typedef struct {
    aos_app_t  *self;
    lv_obj_t   *root, *bar, *body;
    int32_t     W, H;
    bool        land;
    es_proj_t  *p;
    es_engine_t *e;
    bool        audio_ok;

    int view, part, pat, page, track;
    int play_mode, octave, vel_lvl, scale, key_root;
    int dmode;                  /* the drum grid: 0 steps, 1 velocity, 2 probability */
    int lane;                   /* the roll's lane: 0 velocity, 1 probability */
    int roll_low[ES_PARTS];     /* the lowest note the roll shows */
    int last_len;               /* the length of the last note drawn */
    int export_loops;

    /* the bar */
    lv_obj_t *b_play, *b_rec, *l_bpm, *b_met, *tab[V_COUNT], *meter, *l_cpu;
    int       shown_bpm, shown_state;

    lv_timer_t *t_ui, *t_touch;

    /* the view in the body */
    void (*refresh)(void);
    void (*forget)(void);
    lv_obj_t *grid;
    int       last_step, last_pat;

    es_bind_t binds[ES_BINDS];
    int       nbinds;

    /* the project on the card */
    char      file[ES_NAME + 8];
    bool      dirty;
    uint32_t  dirty_ms;

    /* settings, in preferences */
    int latency, voices, mo_dest, mo_clock, mo_pc;

    uint32_t tap_ms[6];
    int      ntap;
    uint32_t log_ms;
    uint32_t load_test_until;
    float    load_test_mono[ES_PARTS];   /* the parts' mono switch, put back after the test */

    lv_obj_t *sheet;
    es_pat_t *clip;
    bool      clip_ok;
} es_ui_t;

extern es_ui_t *es_ui;

/* es_ui.c: the shell */
void es_ui_build(void);
void es_ui_show_view(int v);
void es_ui_dirty(void);
void es_ui_select_pat(int pat);
void es_ui_bpm_set(float bpm);
void es_ui_project_new(void);
bool es_ui_project_open(const char *file);
void es_ui_project_save(void);
void es_ui_preload_samples(void);
void es_ui_apply_settings(void);
void es_ui_load_test(void);

/* helpers (es_ui.c) */
lv_obj_t *ui_box(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *ui_col(lv_obj_t *parent, int32_t w, int32_t gap);
lv_obj_t *ui_row(lv_obj_t *parent, int32_t w, int32_t gap);
lv_obj_t *ui_label(lv_obj_t *parent, const char *txt, const lv_font_t *f, uint32_t color);
lv_obj_t *ui_btn(lv_obj_t *parent, const char *txt, const lv_font_t *f, int32_t w, int32_t h,
                 lv_event_cb_t cb, void *ud);
void      ui_btn_on(lv_obj_t *b, bool on, uint32_t color);
void      ui_btn_text(lv_obj_t *b, const char *txt);
lv_obj_t *ui_seg(lv_obj_t *parent, const char *const *opts, int n, int sel, int32_t w, int32_t h,
                 lv_event_cb_t cb, uint32_t color);
void      ui_seg_set(lv_obj_t *seg, int sel, uint32_t color);
lv_obj_t *ui_card(lv_obj_t *parent, const char *title, int32_t w);
lv_obj_t *ui_slider(lv_obj_t *parent, const char *name, float *v, float min, float max, int fmt,
                    int32_t w, void (*after)(void));
lv_obj_t *ui_param(lv_obj_t *parent, int param, float *v, int32_t w, void (*after)(void));
lv_obj_t *ui_stepper(lv_obj_t *parent, const char *txt, int32_t w, int32_t h, lv_event_cb_t minus,
                     lv_event_cb_t plus, lv_obj_t **label);
void      ui_fmt(int fmt, float v, char *out, size_t n);

lv_obj_t *ui_sheet(const char *title);           /* the content to fill */
void      ui_sheet_close(void);
void      ui_ask_text(const char *title, const char *init, void (*done)(const char *text));
void      ui_confirm(const char *text, const char *ok, void (*yes)(void));

uint32_t  ui_part_color(int part);
uint32_t  ui_drum_color(int track);
const char *ui_track_name(int track, bool shortname);
int       ui_pat_used(const es_pat_t *t);

/* the views */
void es_view_play(lv_obj_t *body);
void es_view_drums(lv_obj_t *body);
void es_view_roll(lv_obj_t *body);
void es_view_sound(lv_obj_t *body);
void es_view_mix(lv_obj_t *body);
void es_view_song(lv_obj_t *body);
void es_play_touch(void);                        /* the fingers, every 10 ms */
void es_play_release_all(void);
lv_obj_t *es_pat_chips(lv_obj_t *parent, int32_t h);   /* A-H, shared by the views */
void      es_pat_chips_refresh(lv_obj_t *row);
