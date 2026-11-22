/*
 * HOCKEY - the app's own state, shared by hockey.c (life cycle, the
 * match's thread, its panels), hk_menu.c (the menus), hk_scene.c (a frame
 * of the match) and hk_input.c (fingers and gamepads).
 */
#pragma once

#include "hk_art.h"
#include "hk_game.h"
#include "hk_gfx.h"

#include "aos_app.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"

#include "lvgl.h"

enum { ST_MENU, ST_LOADING, ST_MATCH };
enum { PN_NONE, PN_PAUSE, PN_RESULT };
enum { SC_TITLE, SC_SETUP, SC_SETTINGS };
enum { MODE_CPU, MODE_TWO };
enum { FMT_GOALS, FMT_TIME };

/* the settings and the last match, in <data>/hockey/ajustes.txt */
typedef struct {
    int  diff;          /* HK_EASY .. HK_CRACK */
    int  sfx, air;      /* sound; the blower's noise */
    int  paddle;        /* 0 small, 1 medium, 2 large */
    int  speed;         /* the puck: 0 slow, 1 normal, 2 fast */
    int  mode, format, minutes, goals;
    char table[16];
} hk_cfg_t;

#define HK_TRAIL 10

/* what a frame shows besides the table (the worker's) */
typedef struct {
    float t;
    float banner_t;
    int   banner;           /* 0 none, 1 goal, 2 full time, 3 golden goal */
    int   banner_side;
    float goal_t;           /* the goal's light */
    int   goal_side;        /* whose goal it lit */
    float hint_t;
    uint32_t hits_seen;
    float ring_t;
    int16_t ring_x, ring_y;
    float trail_x[HK_TRAIL], trail_y[HK_TRAIL];
    int   trail_at;
    float trail_acc;
    bool  touch_on[5];
    int16_t touch_x[5], touch_y[5];
    bool  hud_off;          /* the menus' still: no scores */
    bool  two;              /* two players: the far side's texts are turned */
    int   seen_goals;
    int   seen_state;
    bool  seen_golden;
    /* the texts, in the language of the system */
    char  s_goal[24], s_replay[24], s_full[32], s_golden[32], s_hint[64], s_win[2][32];
    char  name[2][32];
} hk_ui_t;

/* the hands' input, from LVGL's thread to the worker, per side */
typedef struct {
    bool  finger;           /* a finger holds the mallet */
    float fx, fy;           /* where (the table's mm) */
    float pad_x, pad_y;     /* a gamepad's direction, -1..1 */
    bool  pad_fast, pad_home;
} hk_in_t;

typedef struct {
    uint16_t *buf;
    uint32_t  flip;
    uint32_t *hash;
} hk_pbuf_t;

/* a finger, as hk_input.c follows it */
typedef struct {
    uint8_t  id;
    int      side;          /* -1: nobody's; -2: the pause button */
    float    x, y;
} hk_finger_t;

typedef struct {
    aos_app_t  *self;
    lv_obj_t   *root;
    int         state, screen;
    hk_cfg_t    cfg;
    hk_art_t    art;
    bool        art_ok;
    lv_timer_t *timer;
    aos_pad_t   pad;
    aos_pad_menu_t menu;
    uint32_t    pad_prev[2];

    /* the menus */
    lv_obj_t   *scr;
    lv_obj_t   *m_table, *m_format, *m_value, *m_diff[HK_DIFF_N];
    int         still_tb;       /* the table the menus' still shows */

    /* the match */
    hk_game_t  *game;
    hk_ui_t    *ui;
    hk_gfx_t    gfx;
    uint16_t   *fb;             /* the frame LVGL shows under its panels (720 x 1280) */
    uint32_t   *fb_hash;
    bool        fb_full;
    lv_obj_t   *canvas, *panel, *loading;
    int         panel_kind;
    bool        loaded, result_done, button_pause;
    int         tb_ix;

    /* the worker */
    void       *mx;
    volatile bool run;
    volatile int job;           /* 1: load the match's table; 2: a still frame for the menus */
    volatile int job_done;      /* 1 ok, -1 failed */
    bool        worker_on;
    bool        clock_reset;
    uint32_t    last_us;
    float       acc;
    bool        flip_ok;
    uint32_t    flips;
    hk_pbuf_t   pb[3];
    hk_in_t     in[2];
    bool        skip_replay;
    hk_finger_t fing[5];
    volatile uint32_t snd_w, snd_r;
    uint8_t     snd[64], snd_vol[64];
    volatile uint32_t frame_gen;
    uint32_t    frame_seen;

    uint32_t    perf_frames, perf_t0, perf_tiles, perf_items;
    uint32_t    perf_sim_us, perf_list_us, perf_draw_us;
} hk_app_t;

/* hockey.c */
void hk_cfg_load(hk_cfg_t *c);
void hk_cfg_save(const hk_cfg_t *c);
int  hk_lang(void);                 /* 0 es, 1 en, 2 de */
float hk_mallet_r(const hk_cfg_t *c);
void hk_match_start(hk_app_t *a);
void hk_match_quit(hk_app_t *a);
void hk_still(hk_app_t *a);         /* the menus' still, of the table chosen */

/* hk_menu.c */
void hk_menu_show(hk_app_t *a, int screen);
void hk_menu_free(hk_app_t *a);
void hk_menu_pad(hk_app_t *a);
bool hk_menu_back(hk_app_t *a);
lv_obj_t *hk_button(lv_obj_t *parent, const char *text, bool primary, int w, int h, lv_event_cb_t cb, void *ud);
lv_obj_t *hk_label(lv_obj_t *parent, const char *text, const lv_font_t *f, uint32_t col);

/* hk_scene.c */
void hk_scene_strings(hk_app_t *a);
void hk_scene_draw(hk_app_t *a, float dt);

/* hk_input.c */
void hk_input_touch(hk_app_t *a);
void hk_input_pads(hk_app_t *a);
/* the hand of a side played by a person, from what LVGL's thread gathered (worker) */
void hk_input_hand(hk_app_t *a, int side, hk_hand_t *h);

/* hk_audio.c */
bool hk_audio_open(hk_pak_t *pak);
void hk_audio_close(void);
void hk_audio_set(bool sfx, bool air);
void hk_audio_air(bool on);
void hk_audio_pause(bool paused);
void hk_audio_play(int snd, int vol);
void hk_audio_pump(void);
