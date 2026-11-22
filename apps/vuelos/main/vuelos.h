/*
 * VUELOS - what the app's files share: the state, the layout of what is
 * drawn over the map, and the calls from one file to the other.
 *
 *   vuelos.c    life cycle, the worker, the tiles' network, the view, the
 *               touch and the gamepad
 *   vl_frame.c  a frame: the screen cut out of the best map buffer, and on
 *               top of it the rings, the airports, the aircraft with their
 *               trails, the card of the one picked and the buttons
 *   vl_feed.c   the aircraft: asking the chosen source, reading its answer,
 *               the tracks, the trails, the movement between readings, the
 *               routes and the alerts
 *   vl_conf.c   the settings, in a file the portal page writes too
 *   vl_air.c    the airports of the pack
 *   vl_ui.c     the panels over the map: the list and the settings
 *
 * The map is Mapas' (apps/mapas), copied with the prefix vl_ because apps
 * share no code: the worker (core 0) renders the tiles into buffers bigger
 * than the screen, LVGL's side cuts every frame out of them and draws what
 * moves on top. Two threads: LVGL's owns the view, the touch, the network,
 * the aircraft and the frames; the worker owns the tile store and the
 * renders. They talk through the volatile fields below and nothing else.
 */
#pragma once

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_pad.h"
#include "lvgl.h"

#include "vl_draw.h"
#include "vl_render.h"
#include "vl_store.h"

#include <stdbool.h>
#include <stdint.h>

/* The two map buffers, by quality, as in Mapas: FULL at the screen's
 * resolution with FM pixels of margin, HALF at half of it with LM of its own
 * pixels of margin, shown enlarged while the map moves fast. Two of each. */
#define FM          96
#define LM          128
#define CAP_FULL    ((size_t)(AOS_PANEL_W + 2 * FM) * (AOS_PANEL_H + 2 * FM))
#define CAP_HALF    ((size_t)(AOS_PANEL_W / 2 + 2 * LM) * (AOS_PANEL_H / 2 + 2 * LM))
#define CAP_FRAME   ((size_t)AOS_PANEL_W * AOS_PANEL_H)

enum { Q_FULL = 0, Q_HALF, Q_N };

#define TICK_MS     16
#define SYNC_MS     120
#define STILL_MS    160

#define NQ          4
#define MAX_FAILED  16

enum { SCR_MAP = 0, SCR_LIST, SCR_SETTINGS };

enum { Q_FREE = 0, Q_FLIGHT, Q_READY, Q_DONE };

typedef struct {
    volatile int state;
    int          id;
    vl_key_t     k;
    bool         empty;
    int          max;           /* the ceiling it was asked with */
    uint32_t     t0;
} net_t;

typedef struct {
    uint16_t *px;
    int       w, h;
    float     res;
    vl_view_t v;
    uint32_t  geo;
    uint32_t  tgen;
    int       missing, stand_in;
} back_t;

typedef struct {
    back_t       b[2];
    volatile int front;
} pair_t;

/* ---------------------------------------------------------------------------
 * The settings (vl_conf.c): <data>/vuelos.txt, key=value lines, written by
 * the app and by its portal page.
 * ------------------------------------------------------------------------- */

enum { SRC_AIRPLANES = 0, SRC_ADSBLOL, SRC_OPENSKY, SRC_LOCAL, SRC_N };
enum { UNITS_AVIATION = 0, UNITS_METRIC };

typedef struct {
    char    name[40];           /* the place */
    float   lat, lon;
    bool    have_place;
    int     src;                /* SRC_* */
    int     radius_km;          /* what is asked for around the place */
    char    url[160];           /* the own receiver's aircraft.json */
    char    lol_key[80];        /* adsb.lol: an optional key */
    int     lol_day;            /* adsb.lol: requests a day the key (or none) allows */
    char    os_id[80];          /* OpenSky: an API client, optional */
    char    os_secret[80];
    bool    alert_over;         /* an aircraft passing overhead */
    float   over_km;            /* ...within this */
    int     over_ft;            /* ...and under this */
    bool    alert_emerg;        /* squawk 7500, 7600, 7700 */
    bool    alert_mil;          /* military, where the source says so */
    bool    alert_sound;
    int     trail_min;          /* 0: no trails */
    int     units;              /* UNITS_* */
    bool    routes;             /* ask for origin and destination */
    bool    labels;             /* the callsign under every aircraft */
} vl_conf_t;

/* ---------------------------------------------------------------------------
 * The aircraft (vl_feed.c)
 * ------------------------------------------------------------------------- */

#define VL_MAX_AC   200
#define VL_TRAIL    64

/* What an aircraft is drawn as. */
enum {
    K_JET = 0,      /* an airliner */
    K_HEAVY,        /* a wide-body */
    K_PROP,         /* a regional turboprop, a small jet */
    K_LIGHT,        /* a light aircraft */
    K_HELI,
    K_GLIDER,
    K_BALLOON,
    K_DRONE,
    K_GROUND,       /* a vehicle on the apron */
    K_N
};

typedef struct {
    uint32_t x, y;              /* world (normalised Mercator) */
    int32_t  alt;               /* feet, INT32_MIN unknown */
    uint32_t t;                 /* uptime ms */
} trail_pt_t;

typedef struct {
    uint32_t hex;               /* ICAO 24-bit address; bit 24: not ICAO ("~") */
    char     flight[10];        /* callsign, trimmed */
    char     reg[12];
    char     type[6];           /* ICAO type designator */
    char     cat[3];            /* emitter category, "A3", "" unknown */
    uint8_t  kind;              /* K_* */
    bool     ground, mil;
    bool     has_alt, has_gs, has_vr, has_trk, has_sq;
    int32_t  alt_ft;            /* barometric */
    int32_t  vr_fpm;
    float    gs_kt;
    float    trk;               /* degrees, clockwise from north */
    uint16_t sq;                /* the squawk as four octal digits, in decimal: 7700 */
    float    lat, lon;          /* the last fix */
    uint32_t fix_x, fix_y;      /* ...in world units */
    float    vx, vy;            /* world units per second, from gs and track */
    uint32_t fix_ms;            /* uptime when the fix was true */
    uint32_t seen_ms;           /* uptime of the last reading that had it */
    /* the error between where it was drawn and the new fix, fading away:
     * what keeps a new reading from making it jump */
    float    ex, ey;
    uint32_t e_ms;
    float    dist_km;           /* from the place, at the last fix */
    float    brg;               /* bearing from the place, degrees */
    trail_pt_t trail[VL_TRAIL];
    int      ntrail, trail_i;   /* a ring */
    uint8_t  alerted;           /* ALERT_* already given */
    bool     used;
} vl_ac_t;

enum { ALERT_OVER = 1, ALERT_EMERG = 2, ALERT_MIL = 4 };

/* A route, by callsign, from adsbdb. */
typedef struct {
    char     flight[10];
    char     from[8], to[8];    /* IATA or ICAO code */
    char     from_city[24], to_city[24];
    uint8_t  state;             /* 0 asking, 1 known, 2 unknown */
} vl_route_t;

#define VL_ROUTES 24

/* ---------------------------------------------------------------------------
 * The airports (vl_air.c)
 * ------------------------------------------------------------------------- */

enum { AP_LARGE = 0, AP_MEDIUM, AP_SMALL, AP_HELI };

typedef struct {
    int32_t lat, lon;           /* degrees x 1e5 */
    uint8_t kind;               /* AP_* */
    char    icao[5];
    char    iata[4];
    char    name[32];
} vl_airport_t;

/* ---------------------------------------------------------------------------
 * What is drawn over the map, in screen pixels, for the current shape
 * ------------------------------------------------------------------------- */

typedef struct {
    int x, y, r;
} disc_t;

typedef struct {
    int    sw, sh;
    bool   land;
    int    px0, py0, px1, py1;  /* the status pill */
    disc_t menu, zin, zout, home, list;
    int    scale_x, scale_y;
    int    attr_y;
    int    card_x, card_y, card_w, card_h;  /* the picked aircraft's card */
    disc_t card_close, card_follow;
} lay_t;

typedef struct {
    aos_app_t  *self;
    lv_obj_t   *root;
    lv_obj_t   *map;
    lv_obj_t   *touch;
    lv_obj_t   *canvas;
    uint16_t   *frame;
    lv_obj_t   *panel;
    lv_timer_t *timer;
    lay_t       L;

    vl_font_t   fonts[Q_N][VL_FONTS];
    vl_font_t   fbig;               /* the card's callsign */

    /* the worker's buffers */
    pair_t          pair[Q_N];
    back_t * volatile composing;
    volatile uint32_t geo;
    volatile int    sw, sh;
    volatile uint32_t tgen;
    volatile uint32_t front_gen;
    volatile uint32_t t_full_us, t_half_us;
    uint32_t        t_log;
    float           z_log;
    uint32_t        ram_budget;
    vl_key_t        want[VL_MAX_MISSING];
    volatile int    nwant;
    volatile int    missing;

    /* the tiles' network */
    net_t       q[NQ];
    struct { vl_key_t k; uint32_t until; } failed[MAX_FAILED];
    int         nfailed;
    struct { vl_key_t k; uint32_t t; } recent[8];
    int         recent_i;
    char        tpl[192];
    int         tpl_id;
    uint32_t    tpl_retry;
    bool        online;
    int         last_err;
    uint32_t    busy_ms;
    bool        low_latency;

    /* the view */
    vl_view_t   view;
    volatile uint32_t view_seq;
    volatile uint32_t moved_ms;
    volatile bool touching;
    volatile bool zooming;
    volatile float speed;
    float       zt, ax, ay;
    bool        animating;
    bool        pinching;
    float       vx, vy;
    uint32_t    last_tick;
    uint32_t    sp_ms;
    uint32_t    sp_cx, sp_cy;
    uint32_t    shown_seq, shown_gen;
    bool        overlay_dirty;
    bool        viewing;
    bool        hidden;
    bool        use_blit;
    int         blit_fails;
    bool        blit_ok;
    uint32_t    over_bits;
    bool        frame_synced;
    uint16_t   *flipped;
    uint32_t    pushed_ms;
    int         present;
    uint32_t    open_ms;
    int         screen;
    aos_pad_t   pad;

    /* the settings */
    vl_conf_t   conf;
    long        conf_mtime;
    uint32_t    conf_check_ms;
    uint32_t    place_x, place_y;   /* the place, in world units */

    /* the aircraft */
    vl_ac_t    *ac;                 /* VL_MAX_AC of them, PSRAM */
    int         nac;                /* slots in use (some may be !used) */
    int         feed_id;            /* the request in flight */
    int         feed_src;           /* ...to which source */
    uint32_t    feed_t0;
    uint32_t    feed_next;          /* uptime of the next request */
    uint32_t    feed_ok_ms;         /* the last good reading */
    int         feed_err;           /* 0, an HTTP code or AOS_HTTP_ERR_* */
    bool        tile_nomem;         /* the last tile could not be asked for: said once */
    bool        src_alt;            /* airplanes.live refused: adsb.lol in its place, until the source is chosen again */
    int         feed_count;         /* aircraft in the last reading */
    uint32_t    feed_bytes;
    bool        feed_parse_err;
    int         sel;                /* the picked one, -1 none */
    uint32_t    sel_hex;
    bool        follow;
    uint32_t    ac_push_ms;         /* the last frame pushed for the aircraft */
    float       ac_speed_px;        /* the fastest on screen, px/s */
    /* OpenSky's token, when an API client is set */
    char        os_token[1400];
    uint32_t    os_token_until;
    int         os_token_id;
    /* adsb.lol and OpenSky count their requests a day */
    int32_t     day_used;
    int32_t     day_no;
    /* routes */
    vl_route_t  routes[VL_ROUTES];
    int         route_i;
    int         route_id;
    int         route_slot;

    /* the airports of the pack */
    vl_airport_t *ap;
    int         nap;
    bool        ap_missing;

    /* the list */
    lv_obj_t   *list_box;
    bool        list_by_alt;
    uint32_t    list_ms;
} app_t;

/* vuelos.c */
void vl_view_pan(app_t *a, float dx, float dy);
void vl_go_to(app_t *a, uint32_t cx, uint32_t cy, float z);
void vl_zoom_to(app_t *a, float z, float ax, float ay);
int  vl_inflight(const app_t *a);
void vl_place_apply(app_t *a);          /* conf's place moved: the rings and the next reading */
/* world <-> screen for the current view */
void vl_to_screen(const app_t *a, uint32_t x, uint32_t y, float *sx, float *sy);

/* vl_frame.c */
void vl_layout(app_t *a);
void vl_frame_push(app_t *a);
/* the bar's line: how many aircraft and from where, or why there are none */
void vl_status_text(app_t *a, char *out, size_t n, uint32_t *rgb);
/* the source being asked: the chosen one, or the one standing in for it */
int  vl_src_now(const app_t *a);
bool vl_frame_sync(app_t *a, uint32_t now);
int  vl_frame_reserve(const app_t *a, const back_t *b, int16_t (*out)[4], int max);
bool vl_hit_disc(const disc_t *d, float x, float y);
bool vl_hit_card(const app_t *a, float x, float y);
uint32_t vl_alt_color(const vl_ac_t *ac);

/* vl_conf.c */
void vl_conf_defaults(vl_conf_t *c);
bool vl_conf_load(app_t *a);            /* true if the file changed */
bool vl_conf_save(app_t *a);
void vl_conf_poll(app_t *a, uint32_t now);
const char *vl_src_name(int src);

/* vl_feed.c */
bool vl_feed_init(app_t *a);
void vl_feed_free(app_t *a);
void vl_feed_pump(app_t *a, uint32_t now);
void vl_feed_restart(app_t *a);         /* the source or the place changed */
/* where an aircraft is drawn now (interpolated), world units */
void vl_ac_pos(const app_t *a, const vl_ac_t *ac, uint32_t now, uint32_t *x, uint32_t *y);
bool vl_ac_stale(const vl_ac_t *ac, uint32_t now);
int  vl_feed_interval_s(const app_t *a);
const vl_route_t *vl_route_of(app_t *a, const vl_ac_t *ac);
void vl_select(app_t *a, int i);
int  vl_ac_find(const app_t *a, uint32_t hex);
/* text helpers that follow the chosen units; out holds at least 24 */
void vl_fmt_alt(const app_t *a, const vl_ac_t *ac, char *out, size_t n);
void vl_fmt_speed(const app_t *a, const vl_ac_t *ac, char *out, size_t n);
void vl_fmt_vrate(const app_t *a, const vl_ac_t *ac, char *out, size_t n);
void vl_fmt_dist(const app_t *a, float km, char *out, size_t n);

/* vl_air.c */
void vl_air_load(app_t *a);
void vl_air_free(app_t *a);

/* vl_ui.c */
void vl_show_map(app_t *a);
void vl_show_list(app_t *a);
void vl_show_settings(app_t *a);
void vl_ui_relayout(app_t *a);
void vl_ui_tick(app_t *a, uint32_t now);
