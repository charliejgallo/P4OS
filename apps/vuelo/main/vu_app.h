/*
 * PLANEO - the app's state, shared by its core (vuelo.c: life cycle, the
 * worker, frames, hands, sound) and its screens (vu_menu.c).
 *
 * Files:
 *   vu_world   the worlds: heights and lit colours from a seed
 *   vu_render  the 3D: the ground in columns, the sky, the bank's turn
 *   vu_plane   the plane of triangles, its shadow, the propeller
 *   vu_flight  the flight model and the camera
 *   vu_game    the missions: courses, the rival, packages, stars, records
 *   vu_marks   the rings and the columns of light in the frame
 *   vu_hud     the instruments in the frame; vu_text its words
 *   vu_snd     the engine, the wind and the cues
 *   vu_menu    the menus, the pause, the results (LVGL)
 */
#pragma once

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "vu_flight.h"
#include "vu_game.h"
#include "vu_hud.h"
#include "vu_marks.h"
#include "vu_mem.h"
#include "vu_plane.h"
#include "vu_render.h"
#include "vu_snd.h"
#include "vu_text.h"
#include "vu_world.h"

#define VA_NF       3
#define VA_MAXW     512
#define VA_MAXH     288
#define VA_QUALITIES 3

enum { VA_MENU = 0, VA_FLY, VA_PAUSE, VA_RESULT };
enum { F_FREE = 0, F_BUSY, F_READY, F_SHOWN };
enum { R_NONE = 0, R_STICK, R_THR, R_BTN, R_RUD };
/* the menus' screens */
enum { VN_MAIN = 0, VN_MISSIONS, VN_FREE, VN_SETTINGS, VN_PAUSE, VN_RESULT, VN_NONE };

typedef struct {
    uint8_t id, role, btn;
    float   x0, y0;
} va_finger_t;

typedef struct {
    bool sound;
    bool invert;                /* the stick as in a plane: pull down to climb */
    int  quality;               /* 0 fast, 1 normal, 2 fine */
    int  view;                  /* the camera a flight starts with */
    bool fps;                   /* the measurement on the screen */
} va_cfg_t;

typedef struct app_s {
    aos_app_t  *self;
    lv_obj_t   *root, *canvas, *tap, *ui, *load_box, *load_lbl, *load_bar;
    lv_timer_t *timer;
    int         SW, SH;
    bool        closing, worker_ok;
    int         state;              /* VA_* */
    int         screen;             /* VN_* up now */
    /* frames */
    uint16_t   *fr_mem[VA_NF], *fr[VA_NF];
    int         fst[VA_NF], fw[VA_NF], fh[VA_NF];
    bool        fstill[VA_NF];
    uint32_t    fseq[VA_NF], seq;
    int         shown;
    uint16_t   *cv_buf;
    int         ppa;                /* -1 not tried, 1 the PPA flips, 0 the canvas */
    int         W, H;               /* the frame's size now */
    /* the world */
    vw_world_t  world;
    volatile int gen_req;           /* biome * 8 + hour, -1 none */
    volatile int progress;
    volatile bool world_ok;
    int         biome, hour;        /* of the world made */
    /* what the worker is asked */
    volatile int start_req;         /* 1: start the flight in the world made */
    volatile int still_req;         /* 1: one still frame for the menus */
    volatile bool flying, paused;
    volatile bool hold;             /* something of the system's over the app a moment */
    int         mission;            /* -1 free */
    /* the flight: the worker's */
    vf_plane_t  pl;
    vg_game_t   game;
    float       t;
    vp_draw_t   draw, draw2;
    bool        rival_first;        /* the rival is farther: drawn first */
    vm_draw_t   marks;
    vh_layout_t lay;
    vh_state_t  hs;
    vt_set_t    text;
    volatile uint32_t events;       /* VG_EV_* for the UI: the worker ORs, the UI takes */
    /* the hands: LVGL's thread writes, the worker reads */
    volatile float in_x, in_y, in_thr, in_rud;
    volatile float rud_touch;       /* the pedals under a thumb, -1..1 */
    volatile bool  rud_on;
    volatile bool  stick_on;
    volatile float stick_bx, stick_by, stick_x, stick_y;
    volatile int   in_view_n;
    volatile uint8_t btn_held;
    int            view_seen;
    va_finger_t    fing[AOS_TOUCH_MAX];
    uint32_t       raw_btn_ms, lv_btn_ms;
    aos_pad_t      pad;
    aos_pad_menu_t pad_menu;
    uint8_t        keys_prev[8];
    int            nkeys_prev;
    bool           boot_press;
    /* the game's keeping */
    vg_save_t   save;
    va_cfg_t    cfg;
    float       done_t;             /* how long since the finish */
    bool        result_up;
    int         result_stars;
    bool        result_record;
    float       result_time;
    bool        was_stall;
    /* the menus' state */
    int         menu_world;
    int         free_world, free_hour;
    /* the measurement */
    uint32_t perf_t0, perf_frames, perf_presents;
    uint64_t perf_frame_us, perf_ground_us, perf_turn_us, perf_hud_us, perf_present_us, perf_samples, perf_lines;
    uint64_t perf_upx;
    volatile int fps10;
    lv_area_t inv[6];
    int       ninv, inv_count;
} app_t;

/* vuelo.c, for the screens */
void va_start_mission(app_t *a, int mission);
void va_start_free(app_t *a, int biome, int hour);
void va_pause(app_t *a);
void va_resume(app_t *a);
void va_restart(app_t *a);
void va_to_menu(app_t *a);
void va_cfg_store(app_t *a);
void va_quality(app_t *a, int q);
bool va_world_open(app_t *a, int biome);

/* vu_menu.c */
void vn_show(app_t *a, int screen);
void vn_close(app_t *a);
void vn_pad(app_t *a);
extern const char *const VN_WORLD_NAME[VW_BIOMES];
extern const char *const VN_HOUR_NAME[VW_HOURS];
