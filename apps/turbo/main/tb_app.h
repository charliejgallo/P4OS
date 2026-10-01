/*
 * TURBO - the app's shared state
 *
 * turbo.c     life cycle, preferences, the worker, the timer that pushes the
 *             frames, touch, and the LVGL panels (menu, stage select,
 *             garage, results, pause, settings), laid out for either
 *             orientation (tba_layout)
 * tb_link.c   racing another board over the radio link (hidden on the P4,
 *             which has no ESP-NOW yet: see tbl_available)
 *
 * While racing nothing of LVGL is on screen: the worker (core 0) steps the
 * race and renders frames into PSRAM buffers, in bands of internal RAM on
 * both cores (aos_hal_worker_split); the LVGL timer (core 1) pushes the
 * newest to the panel. Upright a frame is the screen as it is and goes out
 * with aos_hal_display_blit_scaled(); lying down it is drawn already turned
 * for the portrait panel and goes out with aos_hal_display_blit_native(),
 * since the PPA turning a whole frame took 62 ms. The panels are LVGL
 * objects over a canvas that shows an unturned frame (or a scene the worker
 * rendered for the menu), and only then does LVGL draw.
 */
#pragma once

#include "aos_app.h"

#include "tb_art.h"
#include "tb_game.h"
#include "tb_gfx.h"
#include "tb_hud.h"
#include "tb_render.h"
#include "tb_track.h"

#include <stdbool.h>
#include <stdint.h>

#define TB_NFB      3           /* at most; the 3rd only when PSRAM allows */
#define TB_FB_SPARE (2 * 1024 * 1024) /* PSRAM that must stay free after it */
#define TB_BAND_BYTES (24 * 1024)     /* one band of internal RAM per core  */

enum { FB_FREE = 0, FB_BUSY, FB_READY, FB_SHOWN };

enum {
    ST_BOOT = 0,
    ST_MENU,
    ST_SELECT,          /* the stage, for a time trial or the link    */
    ST_GARAGE,
    ST_SETTINGS,
    ST_LOADING,
    ST_RACE,
    ST_RESULT,
    ST_LOBBY,           /* waiting for the other board                */
};

enum { MODE_TOUR = 0, MODE_TRIAL, MODE_LINK };

enum { JOB_NONE = 0, JOB_BOOT, JOB_STAGE, JOB_SCENE, JOB_FIT };

/* what a finger on the race's screen does */
enum { TK_NONE = 0, TK_STEER, TK_PEDAL, TK_PAUSE, TK_ARROW };

typedef struct {
    bool    on;
    uint8_t what;           /* TK_*                                        */
    int16_t x0, y0, x, y;
    float   s0;             /* the steering when the finger came down      */
} tb_finger_t;

typedef struct app app_t;

struct app {
    aos_app_t  *self;
    lv_obj_t   *root, *canvas, *touch;

    /* frames */
    uint16_t   *fb[TB_NFB];
    volatile uint8_t  fb_state[TB_NFB];
    volatile uint32_t over;             /* aos_ui_overlay() but toasts, from the LVGL timer: nothing is drawn meanwhile */
    volatile uint16_t fb_rot[TB_NFB];   /* 0: as the screen; 90/270: turned for the panel */
    volatile uint32_t fb_seq[TB_NFB];
    uint32_t    seq;
    int         shown;              /* the buffer on the panel, -1 none      */
    volatile int nfb;               /* buffers in use, 2 or 3                */
    bool        spare_checked;      /* the 3rd was asked for (spare_frame)   */
    uint16_t   *band[2];            /* internal RAM, one per core            */
    int         band_bytes;
    int16_t     fw, fh;             /* the frames' shape as the screen's     */
    bool        turned_ok;          /* lying down, frames go out turned       */
    uint16_t   *cv;                 /* the simulator's unturned copy (TB_TURNED) */

    /* the worker's side */
    volatile int  job;              /* JOB_*, set by the UI, cleared when done */
    volatile bool job_done;
    volatile bool racing;           /* render and step                       */
    volatile bool paused;
    volatile bool want_still;       /* one unturned frame for the canvas      */
    volatile int  still;            /* the buffer it went to, -1 none         */
    volatile bool want_fit, fitting;/* the screen turned                      */
    int         job_stage;          /* JOB_STAGE / JOB_SCENE parameters      */
    int         scene_car, scene_paint;
    float       scene_yaw;
    int         loaded_stage;       /* -1 none                               */
    volatile uint32_t ev_ring[16];  /* the race's events, worker -> UI       */
    volatile uint32_t ev_w;
    uint32_t    ev_r;
    uint64_t    w_last_ms;
    uint32_t    w_frames;
    uint64_t    w_fps_t0;
    uint64_t    w_us_prep, w_us_bands, w_us_frame;

    /* the race */
    tb_track_t  trk;
    tb_game_t   game;
    tb_render_t *ren;
    tb_hud_t    hud;
    tb_hud_state_t hs;
    int         mode;
    int         stage;              /* being raced                           */
    float       tour_time;          /* the tour so far                        */
    int         tour_i;             /* which of the tour's stages             */
    int         tour_coins;
    bool        result_shown;
    bool        new_record;
    bool        res_rival_seen;     /* the result shows the rival's time      */
    bool        res_win_paid;
    int         coins_won;

    /* input: written by the UI, read by the worker */
    volatile float steer;
    volatile bool  gas, brake;
    bool        left, right;        /* the arrows held                        */
    tb_finger_t tk[2];
    uint32_t    touch_seq;
    bool        touch_lift;         /* ignore fingers until all have lifted   */
    bool        autoplay;           /* the bot drives (simulator switch)     */
    int8_t      dev_go;             /* turbo_dev.txt "go N": that stage first */
    bool        dev_noturn;         /* turbo_dev.txt "noturn": the PPA turns them */
    bool        dev_turn;           /* turbo_dev.txt "turn": lying down, the strips turned on the CPU */

    /* preferences */
    int32_t     coins;
    uint32_t    own_cars, own_paints;
    int         car;
    uint8_t     paint[CAR_N];
    int         diff, sens, ctl;
    bool        sfx, autogas;
    uint32_t    unlocked;           /* stages open in time trial              */
    int32_t     best[STAGE_N];      /* tenths of a second, 0 = none          */
    int32_t     best_tour;
    int32_t     rival_best[STAGE_N];
    char        rival_name[28];

    /* link (tb_link.c) */
    bool        link_on;
    int         link_state;
    uint32_t    link_nonce, link_peer_nonce;
    uint32_t    link_ms;
    char        partner[28];
    bool        is_host;
    int         rival_car, rival_paint;
    bool        rival_done;         /* its result arrived                    */
    bool        rival_finished;
    float       rival_time, rival_dist;
    uint32_t    pos_ms;

    /* LVGL */
    lv_obj_t   *p_boot, *boot_bar, *boot_lbl, *boot_title;
    lv_obj_t   *p_menu, *menu_band, *menu_title, *menu_sub, *lbl_coins, *menu_col, *btn_link, *lbl_link;
    lv_obj_t   *menu_row;
    lv_obj_t   *p_select, *sel_list, *sel_title;
    lv_obj_t   *p_garage, *g_top, *g_head, *g_name, *g_prev, *g_next, *g_statbox, *g_stats[4], *g_price;
    lv_obj_t   *g_btn, *g_btn_lbl, *g_sw[16], *g_swbox, *g_coins, *g_title, *g_bottom;
    int         g_car, g_paint;
    lv_obj_t   *p_settings, *set_col, *chip_diff[DIFF_N], *chip_sens[3], *chip_ctl[CTL_N], *chip_sfx, *chip_auto;
    lv_obj_t   *p_loading, *load_lbl;
    lv_obj_t   *p_result, *res_col, *res_title, *res_body, *res_btn_next, *res_btn_next_lbl;
    lv_obj_t   *p_pause, *pause_col;
    lv_obj_t   *p_lobby, *lobby_col, *lobby_lbl;

    int         state;
    uint32_t    st_ms;
    bool        want_exit, closing;
    uint32_t    last_gesture_ms;
    uint64_t    prev_ms;
    lv_timer_t *timer;
};

/* turbo.c */
void tba_set_state(app_t *a, int st);
void tba_race_start(app_t *a, int stage);
void tba_toast(app_t *a, const char *txt);
void tba_prefs_save(app_t *a);

/* tb_link.c */
bool tbl_available(app_t *a, char *name, int n);
void tbl_begin(app_t *a);
void tbl_end(app_t *a);
void tbl_tick(app_t *a);
void tbl_send_result(app_t *a);
void tbl_host_pick(app_t *a, int stage);     /* the host chose a stage in the lobby */
bool tbl_hold_loading(app_t *a);             /* both boards must have the stage */
