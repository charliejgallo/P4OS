/*
 * METEGOL - the app's own state, shared by metegol.c (life cycle, the
 * match's thread, its panels), mg_menu.c (the menus), mg_scene.c (a frame
 * of the match) and mg_input.c (fingers and gamepads).
 */
#pragma once

#include "mg_art.h"
#include "mg_game.h"
#include "mg_gfx.h"
#include "mg_kit.h"

#include "aos_app.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"

#include "lvgl.h"

enum { ST_MENU, ST_LOADING, ST_MATCH };
enum { PN_NONE, PN_PAUSE, PN_RESULT, PN_HALF };
enum { SC_TITLE, SC_SETUP, SC_SETTINGS };
enum { MODE_CPU, MODE_TWO };
enum { FMT_TIME, FMT_GOALS };

/* the settings and the last match, in <data>/metegol/ajustes.txt */
typedef struct {
    int  diff;          /* MG_EASY .. MG_CRACK */
    int  sfx, music, crowd, shake;
    int  one_rod;       /* 0: every rod at once; 1: the one nearest the ball */
    int  p1_side;       /* 0: player 1 defends the left goal */
    int  speed;         /* 0 slow, 1 normal, 2 fast */
    int  assist;        /* the rods follow the ball a little on their own */
    int  board_lang;    /* the scoreboard: 0 as the system, 1 castellano, 2 English */
    int  mode, league, format, minutes, goals, swap;
    char team[2][20];
    char stadium[24];   /* "" : the home side's */
} mg_cfg_t;

/* what a frame shows besides the table (the worker's) */
#define MG_CONFETTI 60
typedef struct {
    float t;
    float shake_t;
    float banner_t;
    int   banner;           /* 0 none, 1 goal, 2 half time, 3 full time, 4 kick off */
    int   banner_team;
    float crowd_acc[8];
    int   crowd_f[8];
    float conf_x[MG_CONFETTI], conf_y[MG_CONFETTI], conf_vx[MG_CONFETTI], conf_vy[MG_CONFETTI];
    uint16_t conf_c[MG_CONFETTI];
    int   nconf;
    float hint_t;
    bool  touch_on[5];
    int16_t touch_x[5], touch_y[5];
    bool  crowd_on, shake_on;
    bool  hud_off;          /* the menus' still: no scoreboard */
    int   seen_goals;
    int   seen_state;
    /* the texts, in the scoreboard's language */
    char  abbr[2][4];
    char  s_goal[24], s_replay[24], s_half[24], s_full[32], s_hint[96], s_half_n[2][8], s_goals_to[24];
    char  name[2][36];
} mg_ui_t;

/* the hands' input, from LVGL's thread to the worker, per team */
typedef struct {
    float move;
    bool  kick;
    float power, spin, slide;
    bool  raise;
    int   sel;              /* -1: nearest the ball; else the rod a gamepad chose */
    float pad_y;            /* -1..1: a gamepad's direction held (mm/s scaled) */
} mg_in_t;

typedef struct {
    uint16_t *buf;
    uint32_t  flip;
    uint32_t *hash;
} mg_pbuf_t;

/* a finger, as mg_input.c follows it */
typedef struct {
    uint8_t  id;
    int      team;          /* -1: not the match's (a button) */
    float    x0, y0, x, y, ly;
    uint32_t t0;
    float    moved;
    bool     held;
} mg_finger_t;

typedef struct {
    aos_app_t  *self;
    lv_obj_t   *root;
    int         state, screen;
    mg_cfg_t    cfg;
    mg_teams_t  teams;
    mg_art_t    art;
    bool        art_ok;
    lv_timer_t *timer;
    aos_pad_t   pad;
    aos_pad_menu_t menu;
    uint32_t    pad_prev[2];

    /* the menus */
    lv_obj_t   *scr;
    int         pick_slot;      /* which team the grid sets: 0 home, 1 away */
    uint8_t    *badge_px[MG_TEAMS_MAX];
    lv_image_dsc_t badge_dsc[MG_TEAMS_MAX];
    uint8_t    *big_px[2];
    lv_image_dsc_t big_dsc[2];
    lv_obj_t   *m_home, *m_away, *m_home_img, *m_away_img, *m_stadium, *m_format, *m_value, *m_slot[2];
    lv_obj_t   *m_tab[2];
    int         list[MG_TEAMS_MAX], nlist;

    /* the match */
    mg_game_t  *game;
    mg_ui_t    *ui;
    mg_gfx_t    gfx;
    mg_team_t   tm[2];          /* home, away (the away kit if they clash) */
    int         team_ix[2];
    uint16_t   *fb;             /* the frame LVGL shows under its panels (1280 x 720) */
    uint32_t   *fb_hash;
    bool        fb_full;
    lv_obj_t   *canvas, *panel, *loading;
    int         panel_kind;
    bool        loaded, result_done, button_pause, half_seen;
    int         st_ix;

    /* the worker */
    void       *mx;
    volatile bool run;
    volatile int job;           /* 1: load the match's art; 2: a still frame for the menus */
    volatile int job_done;      /* 1 ok, -1 failed */
    bool        worker_on;
    bool        clock_reset;
    uint32_t    last_us;
    float       acc;
    bool        flip_ok;
    int         out;            /* MG_OUT_*: how the panel's buffer lies */
    uint32_t    flips;
    mg_pbuf_t   pb[3];
    mg_in_t     in[2];
    bool        skip_replay;
    mg_finger_t fing[5];
    volatile uint32_t snd_w, snd_r;
    uint8_t     snd[64], snd_vol[64];
    volatile uint32_t frame_gen;
    uint32_t    frame_seen;

    uint32_t    perf_frames, perf_t0, perf_tiles, perf_items;
    uint32_t    perf_sim_us, perf_list_us, perf_draw_us;
} mg_app_t;

/* mg_cfg (metegol.c) */
void mg_cfg_load(mg_cfg_t *c);
void mg_cfg_save(const mg_cfg_t *c);
int  mg_lang(void);                 /* 0 es, 1 en, 2 de */
int  mg_team_of_side(const mg_app_t *a, int side);
int  mg_side_of_team(const mg_app_t *a, int team);

/* mg_menu.c */
void mg_menu_show(mg_app_t *a, int screen);
void mg_menu_free(mg_app_t *a);
void mg_menu_pad(mg_app_t *a);
bool mg_menu_back(mg_app_t *a);
lv_obj_t *mg_button(lv_obj_t *parent, const char *text, bool primary, int w, int h, lv_event_cb_t cb, void *ud);
lv_obj_t *mg_label(lv_obj_t *parent, const char *text, const lv_font_t *f, uint32_t col);
uint8_t *mg_badge_image(const mg_team_t *t, int w, int h, lv_image_dsc_t *d);

/* mg_scene.c */
void mg_scene_strings(mg_app_t *a);
void mg_scene_draw(mg_app_t *a, float dt);

/* mg_input.c */
void mg_input_touch(mg_app_t *a);
void mg_input_pads(mg_app_t *a);
/* the hand of a team played by a person, from what LVGL's thread gathered (worker) */
void mg_input_hand(mg_app_t *a, int team, mg_hand_t *h, bool first_step);

/* mg_audio.c */
bool mg_audio_open(mg_pak_t *pak);
void mg_audio_close(void);
void mg_audio_set(bool sfx, bool music, bool crowd);
void mg_audio_music(bool on);
void mg_audio_crowd(float level);    /* 0..1, the murmur's loudness; < 0 off */
void mg_audio_pause(bool paused);
void mg_audio_play(int snd, int vol);
void mg_audio_pump(void);

/* metegol.c */
void mg_match_start(mg_app_t *a);
void mg_match_quit(mg_app_t *a);
