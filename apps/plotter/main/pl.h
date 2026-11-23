/*
 * P4OS - Monitor serie (aos.plotter): what the app's files share.
 *
 * The app plots the numbers that arrive on a channel of the firmware's
 * serial capture service (aos_serial_*, aos_io.h), the same channels the
 * Terminal shows: a microcontroller printing readings, a multimeter's cable, a
 * scale, anything that sends lines with numbers in them.
 *
 *   plotter.c    the app: life cycle, the screen, reading the channel
 *   pl_parse.c   lines into numbers: automatic, or by a saved profile
 *   pl_data.c    the samples, in PSRAM: every row, and one-second buckets
 *   pl_chart.c   the chart, drawn by hand, and its fingers
 *   pl_cfg.c     profiles and alarms in a text file the portal edits too
 *   pl_sheets.c  the sheets: connection, format, series and alarms, view
 *   pl_csv.c     recording to a CSV on the card
 *   pl_live.c    the portal's page: the chart live
 *   pl_i18n.c    the app's own catalogue (it speaks while hidden)
 */
#pragma once

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_mono.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_io.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* _() goes to the app's own catalogue: the alarms and the CSV speak while
 * another app is in front (HANDOFF-APPS 8, Datalog's dl_i18n.c) */
#undef _
#define _(s) pl_tr(s)
const char *pl_tr(const char *es);
void pl_i18n_load(void);

#define PL_ID     "aos.plotter"
#define PL_MAXS   8           /* series */
#define PL_NAME   20
#define PL_UNIT   8
#define PL_MAXF   8           /* fields of a profile */
#define PL_MAXP   12          /* profiles */
#define PL_RAW_N  24          /* lines kept for the raw view */

#define PL_ISNAN(v) ((v) != (v))

void *pl_ps_alloc(size_t n);   /* PSRAM on the board */
void pl_scpy(char *dst, size_t cap, const char *src);
bool pl_has_nocase(const char *hay, const char *needle);
void pl_fmt(char *out, size_t cap, float v, int dec);     /* "26.5", "-1.2e+06" */

/* ---- lines into numbers (pl_parse.c) ---- */

typedef struct {
    int   field;              /* 1-based token */
    char  name[PL_NAME];
    char  unit[PL_UNIT];
    float scale, offset;
    int   dp_field;           /* 0, or the token whose value says the decimals */
    char  dp_map[40];         /* "0:0,1:3,2:2,4:1"; empty: the value IS the decimals */
} pl_pfield_t;

typedef struct {
    char name[24];
    char match[32];           /* only lines with this (case ignored); empty: all */
    bool comma;               /* the decimal mark is a comma */
    int  nf;
    pl_pfield_t f[PL_MAXF];
} pl_profile_t;

typedef struct {
    int    n;
    float  v[PL_MAXS];
    char   name[PL_MAXS][PL_NAME];
    char   unit[PL_MAXS][PL_UNIT];
    int8_t dec[PL_MAXS];      /* decimals as written */
    bool   header;            /* a CSV header: names only */
} pl_parsed_t;

typedef struct {              /* what the automatic parser keeps between lines */
    char hdr[PL_MAXS * 2][PL_NAME];
    int  nhdr;
} pl_auto_t;

/* Both return the values found (0: none), -1 for a line a profile skips */
int  pl_parse_auto(pl_auto_t *st, const char *line, pl_parsed_t *out);
int  pl_parse_profile(const pl_profile_t *p, const char *line, pl_parsed_t *out);
/* A profile's tokens, for numbering them in the raw view */
int  pl_tokens(const char *line, bool comma, char tok[][24], int max);
bool pl_num(const char *s, bool comma, float *v, int *dec, const char **unit);

/* ---- the samples (pl_data.c) ---- */

typedef struct {
    char     name[PL_NAME];
    char     unit[PL_UNIT];
    uint32_t col;             /* 0xRRGGBB */
    int8_t   dec;
    bool     hidden;
    bool     has;
    float    last;
    uint32_t last_t;
    float    dt;              /* the usual time between samples, ms */
    uint32_t count;
    /* alarms */
    bool     lo_on, hi_on, beep;
    float    lo, hi;
    uint8_t  state;           /* 0 inside, 1 under lo, 2 over hi */
    uint32_t alert_t;
} pl_series_t;

typedef struct {
    uint32_t t;
    float    v[PL_MAXS];
} pl_row_t;

typedef struct {
    uint32_t t;               /* the second's start, ms */
    float    mn[PL_MAXS], mx[PL_MAXS], sum[PL_MAXS];
    uint16_t n[PL_MAXS];
} pl_bkt_t;

typedef struct {
    pl_series_t s[PL_MAXS];
    int         ns;
    pl_row_t   *raw;
    uint32_t    raw_cap, raw_n, raw_head;
    pl_bkt_t   *bk;
    uint32_t    bk_cap, bk_n, bk_head;
    pl_bkt_t    cur;
    bool        cur_open;
    uint32_t    rows;          /* since the last clear */
    uint32_t    gen;           /* bumps when the series change or the data is cleared */
} pl_data_t;

extern pl_data_t PD;

bool pl_data_init(void);
void pl_data_free(void);
void pl_data_clear(void);
int  pl_series_get(const char *name, const char *unit);      /* -1 when full */
/* A parsed line at time t: its row, the buckets, the series' last values.
 * Fills row (NAN where a series has nothing) for the CSV. */
void pl_data_add(uint32_t t, const pl_parsed_t *p, pl_row_t *row);
uint32_t pl_data_first_t(void);
uint32_t pl_data_last_t(void);

/* The chart's columns over [t0, t1]: per series and column the low, the
 * high and the last value (NAN when empty), and the window's statistics */
typedef struct {
    int      ncols, cap;
    uint32_t t0, t1;
    float   *mn, *mx, *last;   /* [series * cap + column] */
    uint32_t *lt;              /* the last sample's time in the column */
    bool     buckets;
    float    smin[PL_MAXS], smax[PL_MAXS], ssum[PL_MAXS], slast[PL_MAXS];
    uint32_t sn[PL_MAXS], slast_t[PL_MAXS];
} pl_cols_t;

bool pl_cols_alloc(pl_cols_t *c, int cap);
void pl_cols_free(pl_cols_t *c);
void pl_data_columns(pl_cols_t *c, uint32_t t0, uint32_t t1, int ncols);
/* The rows in [t0, t1], at most max of them (evenly picked), oldest first;
 * the callback gets each. Rows only, not buckets: false when the window is
 * older than the rows kept. */
typedef void (*pl_row_cb_t)(const pl_row_t *r, void *ud);
int  pl_data_rows(uint32_t t0, uint32_t t1, int max, pl_row_cb_t cb, void *ud);

/* ---- the configuration (pl_cfg.c) ---- */

typedef struct {              /* an alarm, kept by the series' name */
    char  name[PL_NAME];
    bool  lo_on, hi_on, beep;
    float lo, hi;
} pl_alarm_t;

typedef struct {
    pl_profile_t p[PL_MAXP];
    int          np;
    pl_alarm_t   al[PL_MAXS * 2];
    int          na;
} pl_cfg_t;

extern pl_cfg_t PC;

void pl_cfg_load(void);
void pl_cfg_save(void);
bool pl_cfg_changed(void);    /* the file changed under us (the portal) */
void pl_cfg_alarm_apply(pl_series_t *s);
void pl_cfg_alarm_store(const pl_series_t *s);
void pl_profile_default(pl_profile_t *p, int tmpl);

/* ---- the app (plotter.c) ---- */

enum { PL_MODE_TIME, PL_MODE_XY };

typedef struct {               /* what is saved in the preferences */
    int      ch;               /* 0 A, 1 B */
    int      fmt;              /* -1 automatic, else a profile */
    int      win;              /* index into PL_WINDOWS */
    int      mode;
    int      xs, ys;           /* XY: the series */
    bool     yfix;
    float    ymin, ymax;
    int      eol;
    int      big;              /* the series shown big */
} pl_set_t;

extern pl_set_t PS;

typedef struct {
    char     text[AOS_SERIAL_LINE_MAX];
    char     got[96];          /* what came out of it */
} pl_rawline_t;

typedef struct {
    aos_app_t *self;
    lv_obj_t  *root, *bar, *seg[2], *port_dd, *conn_btn, *conn_lbl, *connect, *connect_lbl;
    lv_obj_t  *fmt_btn, *fmt_lbl, *win_btn, *win_lbl, *pause_btn, *pause_lbl, *rec_btn, *al_btn, *send_btn;
    lv_obj_t  *status, *bigbox, *big_name, *big_val, *big_unit, *big_stats, *chart, *live_btn;
    lv_obj_t  *overlay, *sheet_ta, *sheet_kb, *raw_box;
    int32_t    W, H;
    bool       land;
    lv_timer_t *timer;
    /* reading */
    uint32_t   next;           /* the channel's next line */
    uint32_t   lost;           /* lines that left the ring unread */
    uint32_t   lines, used;
    pl_auto_t  aut;
    pl_rawline_t raw[PL_RAW_N];
    uint32_t   nraw;
    /* the view */
    bool       paused;
    bool       hidden;         /* behind another app: no drawing */
    uint32_t   end_t;          /* paused: the right edge */
    uint32_t   win_ms;         /* the window, ms (pinch changes it) */
    uint32_t   gen_seen;
    uint32_t   last_draw;
    bool       dirty;
    /* sheets */
    void     (*text_done)(const char *);
    int        edit_p, edit_f, edit_s;
    /* connection, as chosen */
    uint32_t   baud;
    char       frame[4];
    uint8_t    ctl;
    int        usb_n;
} pl_app_t;

extern pl_app_t A;

extern const uint32_t PL_WINDOWS[];
extern const int PL_NWIN;

void pl_refresh(bool force);       /* the bar, the big value */
void pl_rebuild(void);             /* the whole screen, keeping the data */
void pl_restart_reading(void);     /* a new channel or format: from the ring again */
const char *pl_fmt_name(void);
void pl_set_save(void);
void pl_view_flags(bool chart);    /* the chart takes every drag while shown */
uint32_t pl_now(void);
void pl_fmt_dur(char *out, size_t cap, uint32_t ms);  /* "30 s", "5 min", "1.5 h" */

/* ---- the chart (pl_chart.c) ---- */

lv_obj_t *pl_chart_create(lv_obj_t *parent, int32_t w, int32_t h);
void pl_chart_poll(void);          /* from the app's timer */
void pl_chart_free(void);
void pl_chart_want(void);          /* columns again on the next poll */
bool pl_chart_stats(int s, float *mn, float *mx, float *avg, float *last);

/* ---- the sheets (pl_sheets.c) ---- */

void pl_sheet_close(void);
void pl_sheet_conn(void);
void pl_sheet_format(void);
void pl_sheet_series(void);
void pl_sheet_view(void);
void pl_sheet_send(void);
void pl_sheet_raw_update(void);    /* the raw view, when the format sheet is up */
void pl_text_entry(const char *title, const char *value, bool numeric, void (*done)(const char *));
lv_obj_t *pl_mini(lv_obj_t *parent, const char *text, bool on, lv_color_t on_color, lv_event_cb_t cb, void *ud);
void pl_conn_load(void);
void pl_conn_apply(void);
void pl_port_options(char *ports, size_t cap);

/* ---- recording (pl_csv.c) ---- */

bool pl_csv_start(void);
void pl_csv_stop(void);
bool pl_csv_on(void);
void pl_csv_row(const pl_row_t *r);
void pl_csv_poll(void);
const char *pl_csv_path(void);

/* ---- the portal (pl_live.c) ---- */

void pl_live_poll(void);
void pl_live_stop(void);
