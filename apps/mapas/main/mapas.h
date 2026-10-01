/*
 * MAPAS - what the app's files share: the state, the layout of what is drawn
 * over the map, and the calls from one file to the other.
 *
 *   mapas.c     life cycle, the worker, the network, the view and the touch
 *   mp_frame.c  a frame: the screen cut out of the best buffer, what goes on
 *               top, and how it reaches the panel
 *   mp_ui.c     the panels over the map: the menu (zones, offline, settings)
 *               and the search
 *
 * Two threads: LVGL's (core 1) owns the view, the touch, the network and the
 * frames; the worker (core 0) owns the tile store and the renders. They talk
 * through the volatile fields below and nothing else.
 */
#pragma once

#include "aos_app.h"
#include "aos_hal.h"
#include "lvgl.h"

#include "mp_draw.h"
#include "mp_render.h"
#include "mp_search.h"
#include "mp_store.h"

#include <stdbool.h>
#include <stdint.h>

/* The two buffers the worker renders, by quality:
 *
 *   FULL  the screen's resolution, FM screen pixels of margin all round:
 *         912 x 1472 upright. What is shown while the map is still.
 *   HALF  half the resolution, LM of its own pixels of margin (2 x LM on
 *         the screen): 616 x 896, 0.55 Mpx for 1232 x 1792 screen pixels.
 *         A quarter of the pixels to draw for a bigger piece of the world:
 *         what is shown, enlarged x2, while the map moves faster than a
 *         FULL render keeps up with.
 *
 * Their pixel count does not depend on the orientation (w x h and h x w),
 * so they are allocated once and only their shape changes when the screen
 * turns. Two of each, in turns: LVGL cuts a frame out of one while the
 * worker draws the next into the other. */
#define FM          96
#define LM          128
#define CAP_FULL    ((size_t)(AOS_PANEL_W + 2 * FM) * (AOS_PANEL_H + 2 * FM))
#define CAP_HALF    ((size_t)(AOS_PANEL_W / 2 + 2 * LM) * (AOS_PANEL_H / 2 + 2 * LM))
#define CAP_FRAME   ((size_t)AOS_PANEL_W * AOS_PANEL_H)

enum { Q_FULL = 0, Q_HALF, Q_N };

#define TICK_MS     16
#define SYNC_MS     120                 /* still this long: a->frame cut again (mp_frame_sync) */
#define STILL_MS    160                 /* no change for this long: the map is still */

#define NQ          4
#define MAX_FAILED  16
#define MAX_ZONES   32
#define MAX_HITS    40
#define MAX_OFFLINE 30                  /* of MAX_HITS: room left for Photon's */

enum { SCR_MAP = 0, SCR_LIST, SCR_SEARCH };

enum { Q_FREE = 0, Q_FLIGHT, Q_READY, Q_DONE };

typedef struct {
    volatile int state;
    int          id;
    mp_key_t     k;
    bool         empty;         /* 204/404: a tile with nothing in it */
    uint32_t     t0;            /* when it was asked for */
} net_t;

typedef struct {
    uint16_t *px;
    int       w, h;             /* this render's shape */
    float     res;              /* buffer pixels per screen pixel */
    mp_view_t v;                /* the view at its centre */
    uint32_t  geo;              /* the screen's shape it was made for */
    uint32_t  tgen;             /* the tiles it was drawn with */
    int       missing, stand_in;
} back_t;

typedef struct {
    back_t       b[2];
    volatile int front;         /* the newest finished one, -1 none */
} pair_t;

typedef struct {
    char     name[32];
    uint32_t cx, cy;
    float    z;
} zone_t;

/* What is drawn over the map, in screen pixels, for the current shape.
 * The touch reads the same numbers the drawing uses. */
typedef struct {
    int x, y, r;
} disc_t;

typedef struct {
    int    sw, sh;
    int    px0, py0, px1, py1;  /* the search pill */
    disc_t menu, zin, zout;
    int    scale_x, scale_y;    /* the scale bar's left end, its baseline */
    int    attr_y;              /* the attribution's top */
    int    status_y;
} lay_t;

typedef struct {
    aos_app_t  *self;
    lv_obj_t   *root;
    lv_obj_t   *map;            /* the map's container: the canvas and the touch */
    lv_obj_t   *touch;
    lv_obj_t   *canvas;         /* the frame through LVGL: the simulator, and any redraw */
    uint16_t   *frame;          /* sw x sh, what is on the panel */
    lv_obj_t   *panel;          /* the menu or the search, over the map */
    lv_timer_t *timer;
    lay_t       L;

    mp_font_t   fonts[Q_N][MP_FONTS];

    /* the worker's buffers */
    pair_t          pair[Q_N];
    back_t * volatile composing;    /* the one a frame is being cut from */
    volatile uint32_t geo;          /* bumped when the screen turns */
    volatile int    sw, sh;
    volatile uint32_t tgen;         /* bumped when tiles arrive or packs change */
    volatile uint32_t front_gen;    /* bumped at every finished render */
    volatile bool   want_rescan, want_clear;
    volatile int    cleared;        /* files the last clear removed, -1 none */
    volatile bool   packs_ready;
    mp_pack_info_t  packs[MP_MAX_PACKS];
    volatile int    npacks;
    volatile uint32_t t_full_us, t_half_us;   /* what a render costs, smoothed */
    uint32_t        t_log;
    float           z_log;
    uint32_t        ram_budget;

    /* what the last render lacked, for the network (the worker writes) */
    mp_key_t        want[MP_MAX_MISSING];
    volatile int    nwant;
    volatile int    missing;

    /* the network (LVGL's side) */
    net_t       q[NQ];
    struct { mp_key_t k; uint32_t until; } failed[MAX_FAILED];
    int         nfailed;
    struct { mp_key_t k; uint32_t t; } recent[8];
    int         recent_i;
    char        tpl[192];
    int         tpl_id;
    uint32_t    tpl_retry;
    bool        online;
    int         max_inflight;
    int         last_err;
    uint32_t    downloaded;
    uint32_t    busy_ms;
    bool        low_latency;

    /* the view (LVGL's side writes, the worker reads) */
    mp_view_t   view;
    volatile uint32_t view_seq;
    volatile uint32_t moved_ms;
    volatile bool touching;
    volatile bool zooming;          /* an animation, or a pinch under way */
    volatile float speed;           /* screen px/s, smoothed */
    float       zt, ax, ay;         /* zoom animation: target and anchor */
    bool        animating;
    bool        pinching;
    float       vx, vy;             /* fling, px/s */
    uint32_t    last_tick;
    uint32_t    sp_ms;
    uint32_t    sp_cx, sp_cy;
    uint32_t    shown_seq, shown_gen;
    bool        overlay_dirty;
    bool        viewing;
    bool        hidden;
    bool        use_blit;           /* the preference map_blit, default on */
    int         blit_fails;
    bool        blit_ok;            /* a blit has worked once */
    uint32_t    over_bits;          /* aos_ui_overlay(), read by the tick (LVGL's task) */
    bool        frame_synced;       /* a->frame is what the screen shows */
    uint16_t   *flipped;            /* the panel buffer flipped to last, NULL otherwise */
    uint32_t    pushed_ms;
    int         present;            /* how the last frame went up, for the log */
    uint32_t    open_ms;

    zone_t      zones[MAX_ZONES];
    int         nzones;

    /* search: the typed text, the worker's offline pass, Photon online */
    char        query[64];
    lv_obj_t   *ta, *kb, *res_list, *res_note;
    uint32_t    typed_ms;           /* debounce of search-as-you-type */
    bool        typed;
    char        skey[64];
    volatile bool want_search, search_cancel, searching;
    volatile uint32_t search_seq;
    uint32_t    search_shown;
    mp_hit_t    hits[MAX_HITS];         /* LVGL's: what the rows show */
    int         nhits;
    mp_hit_t    whits[MAX_OFFLINE];     /* the worker's, copied over when it is done */
    volatile int nwhits;
    int         photon_id;
    bool        photon_done;
    bool        photon_asked;
    int         screen;             /* SCR_* */

    /* the pin of the last place found (or pressed) */
    bool        pin_on;
    uint32_t    pin_cx, pin_cy;
    char        pin_name[MP_HIT_NAME];

    /* goto.txt, "show it on the board" from a portal (worker reads, LVGL goes) */
    volatile uint32_t goto_seq;
    uint32_t    goto_seen;
    uint32_t    goto_cx, goto_cy;
    float       goto_z;

    /* <card>/maps/bench.txt: a scripted pan and zoom, timed */
    bool        bench;
    int         bench_phase;
    bool        bench_zoomed;
    char        bench_q[40];
    bool        bench_static;       /* "r": the same view rendered again and again */
    int         bs_zoom, bs_count;
    uint32_t    bs_t;
    uint32_t    bench_t;
    uint32_t    b_frames, b_compose, b_present;
    uint32_t    b_blits, b_lvgl, b_flips, b_retouch;
    volatile uint32_t b_renders[Q_N], b_render_us[Q_N];
} app_t;

/* mapas.c */
void mp_view_pan(app_t *a, float dx, float dy);
void mp_go_to(app_t *a, uint32_t cx, uint32_t cy, float z);
int  mp_inflight(const app_t *a);
void mp_zones_load(app_t *a);
bool mp_zone_append(app_t *a, const char *name);

/* mp_frame.c */
void mp_layout(app_t *a);
/* Pushes a frame: cut, overlay, to the panel. LVGL's task. */
void mp_frame_push(app_t *a);
/* From the tick, when no frame was pushed: keeps a->frame (LVGL's canvas)
 * up with a frame drawn straight into the panel. true: LVGL has painted
 * over the frame on screen, push another. LVGL's task. */
bool mp_frame_sync(app_t *a, uint32_t now);
/* The areas the overlay covers, in the buffer b's pixels, for the labels. */
int  mp_frame_reserve(const app_t *a, const back_t *b, int16_t (*out)[4], int max);
bool mp_hit_disc(const disc_t *d, float x, float y);
bool mp_hit_pill(const app_t *a, float x, float y);

/* mp_ui.c */
void mp_show_map(app_t *a);
void mp_show_list(app_t *a);
void mp_show_search(app_t *a);
void mp_ui_relayout(app_t *a);      /* the screen turned with a panel up */
void mp_search_pump(app_t *a);      /* from the tick */
