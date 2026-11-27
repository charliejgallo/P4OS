/*
 * CIELO - what the app's files share.
 *
 *   cielo.c     life cycle, the frame timer, the touch, the time travel
 *   cl_view.c   a frame of the sky: the projection, the Milky Way, the
 *               stars and the figures, the planets, the Moon, the
 *               satellites, the ground, the labels, the buttons and the
 *               card; and what is under a finger
 *   cl_sat.c    the satellites: the TLE file and its download, the passes
 *               (in a thread of their own), the alerts, the files for the
 *               portal page
 *   cl_conf.c   the settings, in a file the portal page reads too
 *   cl_ui.c     the panels over the sky: settings, place, date, satellites
 *   cl_astro.c  cl_sgp4.c  cl_dmath.c  cl_cat.c  cl_draw.c: the engine
 *
 * Threads. LVGL's owns the view, the touch, the settings and the panels;
 * each frame it fills a scene (cl_scene_t) and hands it to the worker,
 * which draws it straight into the panel's free buffer and flips (in the
 * simulator, into one of two frames shown through an LVGL canvas). The
 * worker hands back where every object it drew landed, for the taps. The
 * passes thread owns the predictions; the satellites' elements are copied
 * under a mutex.
 */
#pragma once

#include "aos_app.h"
#include "aos_hal.h"
#include "lvgl.h"

#include "cl_astro.h"
#include "cl_cat.h"
#include "cl_draw.h"
#include "cl_sgp4.h"

#include <stdbool.h>
#include <stdint.h>

#define CL_TICK_MS      16
#define CL_MAX_SATS     12
#define CL_MAX_PASSES   64
#define CL_MAX_HITS     600
#define CL_TRACK_N      120

enum { SCR_SKY = 0, SCR_SETTINGS, SCR_PLACE, SCR_DATE, SCR_SATS };

/* what a hit or the selection is */
enum { OBJ_NONE = 0, OBJ_STAR, OBJ_BODY, OBJ_MESS, OBJ_SAT, OBJ_CONS };

/* fonts */
enum { F_S = 0, F_M, F_L, F_N };

/* ---------------------------------------------------------------------------
 * Settings (cl_conf.c): <data>/cielo.txt
 * ------------------------------------------------------------------------- */

typedef struct {
    char   place[48];
    float  lat, lon, height;        /* degrees, metres */
    bool   have_place;
    bool   night;                   /* red */
    bool   lines, cnames, snames, mw, messier, grid, ground, atmos, sats, planets;
    bool   alerts;
    int    alert_min;               /* minutes before a pass */
    float  mag_extra;               /* +- on the limiting magnitude */
    uint32_t sat_ids[CL_MAX_SATS];
    int    nsat_ids;
    char   tle_url[200];
    /* the view, kept between openings */
    float  az, alt, fov;
} cl_conf_t;

/* ---------------------------------------------------------------------------
 * The satellites (cl_sat.c)
 * ------------------------------------------------------------------------- */

typedef struct {
    cl_sat_t  el;
    float     std_mag;
} cl_satx_t;

typedef struct {
    int       sat;                  /* index into sats */
    cl_pass_t p;
    bool      alerted;
} cl_passx_t;

/* ---------------------------------------------------------------------------
 * The scene: everything a frame needs, by value
 * ------------------------------------------------------------------------- */

typedef struct {
    int    sw, sh;
    float  az, alt, fov;            /* the view, radians */
    double jd;
    float  lat, lon;
    bool   night, lines, cnames, snames, mw, messier, grid, ground, atmos, sats, planets;
    float  mag_extra;
    bool   moving;                  /* a finger or a fling: cheaper labels */
    int    sel_type, sel_idx;       /* the selection, marked with a ring */
    /* the top bar */
    char   top1[64], top2[64];
    char   rate[16];
    bool   live;                    /* the time is now's */
    /* the card */
    bool   card;
    char   card_title[64], card_sub[96];
    char   card_rows[6][2][48];
    int    card_nrows;
    char   card_foot[128];
    char   hint[96];                /* a line over the bottom bar */
    /* the satellites to draw: their elements and tracks */
    int    nsat;
    cl_satx_t sat[CL_MAX_SATS];
    int    track_sat;               /* the one whose pass is traced, -1 */
    double track_t0, track_t1;
    const char *labels[16];         /* translated words the worker writes */
    uint32_t gen;
} cl_scene_t;

typedef struct {
    int16_t x, y;
    uint8_t type, r;
    int16_t idx;
} cl_hit_t;

/* ---------------------------------------------------------------------------
 * The layout of what is drawn over the sky (cl_view.c)
 * ------------------------------------------------------------------------- */

typedef struct {
    int x, y, r;
} cl_disc_t;

typedef struct {
    int sw, sh;
    bool land;
    int bar_x, bar_y, bar_w, bar_h;             /* the top bar */
    cl_disc_t set, sats, mode, night;           /* buttons */
    cl_disc_t t_back2, t_back, t_now, t_fwd, t_fwd2;    /* the time bar */
    int time_y;
    int card_x, card_y, card_w, card_h;
    cl_disc_t card_close;
} cl_lay_t;

/* ---------------------------------------------------------------------------
 * The app
 * ------------------------------------------------------------------------- */

typedef struct app {
    aos_app_t *self;
    lv_obj_t  *root, *sky, *canvas, *touch, *panel;
    lv_timer_t *timer;
    int        screen;
    bool       hidden;
    int        sw, sh;
    cl_lay_t   L;

    cl_conf_t  conf;
    long       conf_mtime;
    cl_cat_t   cat;
    bool       have_cat;
    cl_font_t  font[F_N];

    /* the view: what the finger moves */
    float      az, alt, fov;
    float      vaz, valt;           /* fling, radians per second */
    bool       touching, pinching;
    float      fov_target;          /* an animated zoom, 0 none */
    float      az_target, alt_target;
    bool       anim;
    uint32_t   moved_ms;

    /* time */
    double     offset_s;            /* the shown time minus now */
    double     rate;                /* seconds of sky per second */
    int        rate_i;
    double     last_real;

    /* selection */
    int        sel_type, sel_idx;
    double     sel_rts_jd;          /* when the card's times were worked out */
    uint32_t   sel_card_ms;

    /* frames */
    void      *mutex;
    cl_scene_t scene;               /* LVGL's, being filled */
    cl_scene_t wscene;              /* handed to the worker */
    cl_scene_t rscene;              /* the worker's own, while it draws */
    volatile uint32_t req_seq, done_seq;
    volatile bool     worker_busy;
    uint16_t  *frame[2];            /* without page flipping (the simulator) */
    volatile int ready, shown;
    bool       use_flip;
    volatile uint32_t over_bits;
    volatile bool     viewing;      /* the sky is up (no panel) */
    cl_hit_t  *hits;                /* what the last frame drew, for taps */
    int        nhits;
    cl_hit_t  *whits;
    volatile int wnhits;
    volatile bool hits_new;
    uint32_t   last_push_ms;
    uint32_t   gen;

    /* the worker's per-frame scratch */
    float     *proj;                /* x, y per star */
    uint8_t   *mwgrid;              /* the Milky Way sampled on the grid */

    /* measuring */
    uint32_t   fps_frames, fps_t0, draw_us_sum, draw_us_max;
    uint32_t   us_sky, us_mw, us_lab, us_draw;     /* where a frame's time goes */
    uint32_t   n_move, n_sat, n_idle;              /* frames asked, by why */
    bool       no_split;

    /* satellites */
    cl_satx_t  sats[CL_MAX_SATS];
    int        nsats;
    uint32_t   sats_gen;
    char       tle_err[64];
    long       tle_time;            /* when the TLE file was fetched, unix */
    int        tle_req;             /* HTTP id, 0 none */
    uint32_t   tle_req_ms;
    uint32_t   tle_last_try_ms;
    bool       tle_manual;
    char       (*tle_names)[25];    /* every satellite of the file, for the list */
    uint32_t  *tle_ids;
    int        tle_n;
    cl_passx_t passes[CL_MAX_PASSES];
    volatile int npasses;
    volatile bool pass_busy, pass_stop, pass_redo;
    volatile bool pass_thread;
    double     pass_jd;             /* computed from */
    uint32_t   replan_ms;
    uint32_t   json_ms;
    const char *alert_fmt;          /* copied translations for alerts made hidden */
    char       alert_txt[96];

    /* panels */
    lv_obj_t  *date_rollers[5];
    lv_obj_t  *place_ta[3];
    lv_obj_t  *list_box;
} app_t;

/* cl_conf.c */
void cl_conf_defaults(cl_conf_t *c);
void cl_conf_load(app_t *a);
void cl_conf_save(app_t *a);
void cl_conf_poll(app_t *a);
const char *cl_conf_path(char *out, size_t n);

/* cl_view.c */
void cl_layout(app_t *a);
bool cl_frame_init(app_t *a);
void cl_frame_free(void);
void cl_frame_prepare(app_t *a, const cl_scene_t *s, cl_hit_t *hits, int *nhits);
void cl_render(app_t *a, const cl_scene_t *s, cl_fb_t *fb, int part, int parts, cl_hit_t *hits, int *nhits);
bool cl_hit_disc(const cl_disc_t *d, float x, float y);
bool cl_hit_card(const app_t *a, float x, float y);
/* the screen point of an alt/az direction in a view; false: behind */
bool cl_project(const cl_scene_t *s, const float enu[3], float *x, float *y);
/* the alt/az direction under a screen point */
void cl_unproject(const cl_scene_t *s, float x, float y, float enu[3]);
/* cl_names.c: the worker's names, translated once from create() */
void cl_names_prepare(void);
const char *cl_body_name(int b);
const char *cl_star_name(const char *es);
const char *cl_cons_name(const char *abbr);
const char *cl_mess_name(int m);
const char *cl_mess_type(int t);
float cl_mess_dist_ly(int m);

/* cl_sat.c */
void cl_sat_init(app_t *a);
void cl_sat_free(app_t *a);
void cl_sat_tick(app_t *a, double jd_now, uint32_t now_ms);
void cl_sat_reload(app_t *a);
void cl_sat_fetch(app_t *a, bool manual);
void cl_sat_replan(app_t *a);
void cl_sat_write_now(app_t *a, double jd);
int  cl_sat_find(const app_t *a, uint32_t norad);
float cl_sat_std_mag(uint32_t norad);

/* cl_ui.c */
void cl_show_sky(app_t *a);
void cl_show_settings(app_t *a);
void cl_show_place(app_t *a);
void cl_show_date(app_t *a);
void cl_show_sats(app_t *a);
void cl_ui_relayout(app_t *a);
void cl_ui_tick(app_t *a, uint32_t now);

/* cielo.c */
double cl_now_jd(const app_t *a);
double cl_real_jd(void);
void   cl_set_time(app_t *a, double jd);
void   cl_go_live(app_t *a);
float  cl_dome_az(const app_t *a);
void   cl_select(app_t *a, int type, int idx);
void   cl_dirty(app_t *a);
/* style 0 "21:04", 1 "vie 27/11/2026 21:04", 2 "vie 27/11 21:04" */
void   cl_fmt_time(double jd, char *out, size_t n, int style);
