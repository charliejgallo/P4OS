/*
 * DERIVA - the app's own state, shared by asteroides.c (life cycle,
 * the game's thread, its panels), ast_menu.c (the menus and the
 * workshop), ast_scene.c (a frame of the game), ast_input.c (fingers and
 * gamepads), ast_bg.c (the sectors' skies) and ast_audio.c (the sound).
 */
#pragma once

#include "ast_bot.h"
#include "ast_game.h"
#include "ast_gfx.h"

#include "aos_app.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"

#include "lvgl.h"

enum { ST_MENU, ST_PLAY };
enum { PN_NONE, PN_PAUSE, PN_OVER, PN_SHOP, PN_WON };
enum { SC_TITLE, SC_SETTINGS, SC_HELP };
enum { CTL_STICK, CTL_BUTTONS };

#define AST_RECORDS 5

/* the settings, the records and the campaign, in <data>/asteroides/ajustes.txt */
typedef struct {
    int diff;               /* AG_EASY .. AG_HARD                         */
    int sfx, music, shake;
    int controls;           /* CTL_STICK, CTL_BUTTONS                     */
    int lefty;              /* the stick on the right                     */
    int best[AST_RECORDS], best_wave[AST_RECORDS];
    /* the campaign goes on from here (0: none started) */
    int camp_sector, camp_lives, camp_score, camp_crystals, camp_diff;
    int camp_upg[UP_N];
    int camp_max;           /* the furthest sector reached                */
    int camp_won;
} ast_cfg_t;

/* where things are on the screen, as the player sees it */
typedef struct {
    int  W, H;
    bool land;
    int  fx, fy, fw, fh;            /* the field                           */
    int  stick_x, stick_y, stick_r; /* the stick's home and its travel     */
    int  zone_x0, zone_y0, zone_x1, zone_y1;   /* where a thumb takes the stick */
    int  fire_x, fire_y, fire_r;
    int  jump_x, jump_y, jump_r;
    int  pause_x, pause_y, pause_r;
    int  tl_x, tl_y, tr_x, tr_y, push_x, push_y, btn_r;  /* the buttons' way */
} ast_layout_t;

/* a finger, as ast_input.c follows it */
enum { FG_NONE, FG_STICK, FG_FIRE, FG_JUMP, FG_PAUSE, FG_BTNS };
typedef struct {
    uint8_t id;
    uint8_t role;
    float   x, y, x0, y0;
} ast_finger_t;

/* a player's hands, from LVGL's thread to the worker */
typedef struct {
    bool  stick;            /* a thumb on the stick                         */
    float sx, sy;           /* its vector, length 0..1                      */
    float cx, cy;           /* where it landed (the knob's base)            */
    bool  fire, jump, turn_l, turn_r, push;
    bool  pad_on;
    float pad_x, pad_y;     /* an analog stick                              */
    bool  pad_l, pad_r, pad_up, pad_fire, pad_jump;
} ast_in_t;

#define AST_PARTS  700
#define AST_TEXTS  6

typedef struct {
    uint8_t kind;           /* 0 streak, 1 puff, 2 ring, 3 shard          */
    float   x, y, vx, vy, life, max, a, spin, size;
    uint32_t color;
} ast_part_t;

typedef struct {
    float x, y, t;
    char  s[24];
    uint32_t color;
} ast_ftext_t;

/* what a frame shows besides the game (the worker's) */
typedef struct {
    float t, shake, flash;
    float sx, sy;           /* this frame's shake                         */
    ast_part_t part[AST_PARTS];
    int   npart;
    ast_ftext_t text[AST_TEXTS];
    float trail_acc[2];
    float jump_fx[2];
    bool  title;            /* the menus' still                           */
    int   seen_state;
    float banner_t;
    /* the texts, translated on LVGL's thread */
    char  s_sector[24], s_wave[24], s_clear[40], s_boss[32], s_over[32], s_won[40], s_ready[24];
    char  s_sector_name[AG_SECTORS][24];
    char  s_boss_name[4][24];
    char  s_power[PW_N][20];
    char  s_fire[16], s_jump[16], s_record[16], s_paused[16];
    char  s_title[24];
} ast_ui_t;

typedef struct {
    uint16_t *buf;
    uint32_t  flip;
    uint32_t *hash;
} ast_pbuf_t;

typedef struct {
    aos_app_t  *self;
    lv_obj_t   *root;
    int         state, screen;
    ast_cfg_t   cfg;
    ast_layout_t lay;
    lv_timer_t *timer;
    aos_pad_t   pad;
    aos_pad_menu_t menu;
    uint32_t    pad_prev[2];
    int         mode, players;  /* the game being played                    */

    /* the menus */
    lv_obj_t   *scr;

    /* the game */
    ag_game_t  *game;
    ast_ui_t   *ui;
    ast_gfx_t   gfx;
    ast_bot_t   bot[2];
    bool        demo;           /* the board plays (the title, AST_DEMO)    */
    uint16_t   *bg;             /* the sector's sky and the deck            */
    int         bg_sector;      /* what bg holds (-1 nothing)               */
    uint16_t   *fb;             /* the frame LVGL shows under its panels    */
    uint32_t   *fb_hash;
    bool        fb_full;
    lv_obj_t   *canvas, *panel;
    int         panel_kind;
    bool        loaded, button_pause, over_done;
    float       over_t;

    /* the worker */
    void       *mx;
    volatile bool run;
    volatile int job;           /* 1: a still for the menus                 */
    volatile int job_done;
    bool        worker_on;
    bool        clock_reset;
    uint32_t    last_us;
    float       acc;
    bool        flip_ok;
    int         out;
    uint32_t    flips;
    ast_pbuf_t  pb[3];
    ast_in_t    in[2];
    ast_finger_t fing[5];
    bool        jump_latch[2];
    volatile uint32_t snd_w, snd_r;
    uint8_t     snd[64], snd_vol[64];
    volatile uint32_t frame_gen;
    uint32_t    frame_seen;
    float       thrust_level;

    uint32_t    perf_frames, perf_t0, perf_tiles, perf_items;
    uint32_t    perf_sim_us, perf_list_us, perf_draw_us;
} ast_app_t;

/* asteroides.c */
void ast_cfg_load(ast_cfg_t *c);
void ast_cfg_save(const ast_cfg_t *c);
void ast_layout(ast_app_t *a, int w, int h);
void ast_game_start(ast_app_t *a, int mode, int players, bool resume);
void ast_game_quit(ast_app_t *a);

/* ast_menu.c */
void ast_menu_show(ast_app_t *a, int screen);
void ast_menu_free(ast_app_t *a);
void ast_menu_pad(ast_app_t *a);
bool ast_menu_back(ast_app_t *a);
lv_obj_t *ast_button(lv_obj_t *parent, const char *text, bool primary, int w, int h, lv_event_cb_t cb, void *ud);
lv_obj_t *ast_label(lv_obj_t *parent, const char *text, const lv_font_t *f, uint32_t col);
lv_obj_t *ast_panel(ast_app_t *a, int w);
void ast_shop_fill(ast_app_t *a, lv_obj_t *p);

/* ast_scene.c */
void ast_scene_strings(ast_app_t *a);
void ast_scene_reset(ast_app_t *a);
void ast_scene_events(ast_app_t *a);
void ast_scene_draw(ast_app_t *a, float dt);
uint32_t ast_sector_color(int sector, int which);
int  ast_palette(const ag_game_t *g);   /* the sector whose colours are on */

/* ast_bg.c */
void ast_bg_make(ast_app_t *a, int sector);

/* ast_input.c */
void ast_input_touch(ast_app_t *a);
void ast_input_pads(ast_app_t *a);
/* the hand of player s, from what LVGL's thread gathered (worker) */
void ast_input_hand(ast_app_t *a, int s, ag_hand_t *h);

/* ast_audio.c */
bool ast_audio_open(void);
void ast_audio_make_music(void);     /* the worker's: a second or so */
void ast_audio_close(void);
void ast_audio_set(bool sfx, bool music);
void ast_audio_music(int which);     /* 0 off, 1 the menus, 2 play, 3 a boss */
void ast_audio_thrust(float level);
void ast_audio_pause(bool paused);
void ast_audio_play(int snd, int vol);
void ast_audio_pump(void);
