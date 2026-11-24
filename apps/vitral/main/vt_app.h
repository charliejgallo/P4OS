/*
 * VITRAL - what the app's files share
 *
 *   vt_game.c   the rules (no LVGL)
 *   vt_gfx.c    glass, lead and stone, drawn by code; the dirty tiles
 *   vt_view.c   where things go on the canvas, the wells, the effects
 *   vt_input.c  fingers, the on-screen keys, gamepads and the keyboard
 *   vt_snd.c    the sounds, synthesised when the app opens
 *   vt_menu.c   the panels: title, levels, options, pause, the end
 *   vitral.c    the life cycle and the frame
 */
#pragma once

#include "lvgl.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"
#include "aos_retro.h"

#include "vt_game.h"
#include "vt_gfx.h"

#define VT_FPS          60
#define VT_MAX_PARTS    420
#define VT_FINGERS      5

typedef enum {
    PANEL_NONE, PANEL_TITLE, PANEL_LEVELS, PANEL_OPTIONS, PANEL_HELP, PANEL_PAUSE,
    PANEL_OVER, PANEL_WON, PANEL_STUCK, PANEL_VS_OVER,
} vt_panel_t;

/* what the canvas is showing */
typedef enum {
    SCR_TITLE,
    SCR_GAME,
} vt_screen_t;

/* where a player's things go, in the coordinates of its surface */
typedef struct {
    int16_t ax, ay, aw, ah;     /* the player's area on the canvas */
    bool    flip;
    int16_t wx, wy;             /* the glass of the visible rows */
    int16_t cell;
    int16_t arch;
    int16_t nx, ny, nbox, nstep, nn, ncell;   /* the previews: a column of boxes */
    int16_t hx, hy;             /* the held piece's box (-1: none) */
    int16_t mx;                 /* versus: the meter of rubble coming (-1: none) */
    int16_t keys_y, keys_h;     /* the on-screen keys (keys_h 0: none) */
    int16_t key_x[6], key_w[6];
    int16_t kys[6];             /* each key's y (lying down they are in two columns) */
} vt_layout_t;

typedef struct {
    float    x, y, vx, vy, g;
    uint16_t color;
    uint8_t  life, max, size, kind;
} vt_part_t;

/* a finger, from when it lands to when it lifts */
typedef struct {
    uint8_t  id;
    bool     on;
    int8_t   player;            /* whose area it landed in, -1 none (a UI button) */
    int8_t   key;               /* the on-screen key under it, -1 none */
    bool     acted;             /* moved, dropped or held: no tap on lifting */
    bool     spent;             /* a drop or a hold: nothing more until it lifts */
    float    x0, y0, ax, ay, x, y;      /* landed, anchors, now (screen px) */
    uint32_t t0;
    float    hx[8], hy[8];
    uint32_t ht[8];
    uint8_t  hn;
} vt_finger_t;

typedef struct {
    vt_game_t  g;
    vt_layout_t L;
    vt_surf_t  sbg, sfb;        /* the area on the background and on the frame */
    uint32_t   key[VT_ROWS][VT_COLS_MAX];   /* what the background holds per cell */
    bool       board_all;       /* redraw every cell */
    uint8_t    shown_next[VT_QUEUE * 2 + 2];
    bool       previews_dirty;

    /* effects (frames left) */
    uint8_t    lock_t;
    vt_piece_t lock_p;
    uint8_t    trail_t;
    vt_piece_t trail_p;
    int8_t     trail_from;
    uint8_t    ray_t;
    uint16_t   ray_col;
    uint8_t    reject_t;
    vt_piece_t reject_p;
    uint8_t    level_t;
    int8_t     prev_frac_y;     /* the falling piece's last pixel row, for smoothness */

    /* input */
    int8_t     das_dir;
    uint32_t   das_next;
    bool       soft_btn, soft_pad;
    uint8_t    keys_down;       /* on-screen keys held, a bit each */
    aos_pad_t  pad;

    /* the labels of this player */
    lv_obj_t  *lb_score, *lb_level, *lb_lines, *lb_cap_level, *lb_cap_lines;
    lv_obj_t  *lb_cap_next, *lb_cap_hold, *lb_pop, *lb_info;
    uint32_t   shown_score;
    int        shown_level, shown_lines, shown_info;
    uint32_t   pop_until;
} vt_player_t;

typedef struct {
    const aos_retro_t *r;
    uint16_t  *bg;              /* what is behind what moves */
    int        cw, ch;
    bool       land;
    vt_screen_t screen;
    vt_mode_t  mode;
    int        nplayers;
    vt_player_t p[2];
    vt_dirty_t d_prev, d_cur, d_static;
    vt_part_t  parts[VT_MAX_PARTS];
    int        nparts;
    vt_finger_t f[VT_FINGERS];

    bool       paused, over, hidden;
    uint32_t   now;             /* ms of game time, for the effects */
    uint32_t   seed;

    /* settings */
    bool       use_keys;        /* on-screen keys instead of gestures */
    bool       ghost;
    bool       sound;
    int        start_level;

    /* records */
    uint32_t   hi_marathon, hi_time;
    uint8_t    pz_open;          /* levels unlocked */
    uint8_t    pz_stars[VT_PZ_LEVELS];
    uint8_t    wins[2];
    int        pz_level;

    /* LVGL */
    lv_obj_t  *root;
    lv_obj_t  *hud;             /* labels over the canvas */
    lv_obj_t  *btn_pause, *btn_undo, *btn_hint;
    lv_obj_t  *panel;           /* the panel up, if any */
    lv_obj_t  *lb_title, *lb_time;
    aos_pad_menu_t menu;
    aos_pad_t  mpad;            /* the pad for the panels */
    int        hint_t;          /* puzzle: frames the hint shows */
    vt_panel_t panel_kind, panel_from;
    int32_t    end_t;           /* ms until the end panel shows (-1: none) */
    uint32_t   step_acc;
    bool       new_record;
    int        stars_now;
    int        winner;
    uint8_t    keys_shown[2];

    /* measuring */
    uint32_t   fps_frames, fps_t0, draw_us, draw_max;
} vt_app_t;

extern vt_app_t *vt_A;

/* vt_view.c */
void vt_view_layout(vt_app_t *a);           /* the canvas for the root's shape */
void vt_view_title(vt_app_t *a);            /* the title's background */
void vt_view_game(vt_app_t *a);             /* a game's background, all of it */
void vt_view_frame(vt_app_t *a);            /* one frame: what changed */
void vt_view_events(vt_app_t *a, int pi);   /* effects and sounds of the game's events */
void vt_view_hud(vt_app_t *a);              /* the labels, when their numbers change */
void vt_view_hud_build(vt_app_t *a);
void vt_view_burst(vt_app_t *a, int pi);    /* a puzzle won: the window lights up */

/* vt_input.c */
void vt_input_frame(vt_app_t *a);           /* every frame, before the step */
bool vt_input_key(uint32_t key, uint8_t mods);
void vt_input_reset(vt_app_t *a);

/* vt_menu.c */
void vt_menu_title(vt_app_t *a);
void vt_menu_pause(vt_app_t *a);
void vt_menu_over(vt_app_t *a);
void vt_menu_close(vt_app_t *a);
void vt_menu_frame(vt_app_t *a);            /* the pad on the panel */
bool vt_menu_back(vt_app_t *a);

/* vitral.c */
void vt_start(vt_app_t *a, vt_mode_t mode, int level);
void vt_quit_to_title(vt_app_t *a);
void vt_prefs_save(vt_app_t *a);
void vt_set_paused(vt_app_t *a, bool p);
void vt_game_ended(vt_app_t *a);

/* the canvas to the screen and back */
static inline int vt_sx(const vt_app_t *a, int cx) { return a->r->x + cx * a->r->scale; }
static inline int vt_sy(const vt_app_t *a, int cy) { return a->r->y + cy * a->r->scale; }
