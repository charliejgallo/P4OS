/*
 * P4OS - Bolsa: stocks, indices, currencies and crypto, with charts
 * (apps/bolsa/README.md).
 *
 * The pieces:
 *
 *   bs_sym.c     what a symbol is: its kind (stock, index, crypto, the
 *                Argentine dollar, a currency, the country risk), the
 *                built-in catalog of the ones that are not companies, and
 *                which provider serves it
 *   bs_cfg.c     <data>/bolsa/bolsa.txt: the lists, the provider keys, the
 *                alerts. The portal's page writes the same file.
 *   bs_series.c  a price series in memory and in the cache, slicing it to a
 *                range, the moving averages and the RSI, number and date text
 *   bs_prov.c    one layer per provider: the URL for what is wanted and the
 *                parser of what comes back, into a series or a quote
 *   bs_net.c     the queue of requests: one at a time per provider, each
 *                provider's own pace and its limits, retries
 *   bs_chart.c   the charts, drawn by hand on an object: line, area,
 *                candles, volume, averages, RSI, the cursor and the two-finger
 *                comparison, several symbols in %; and the sparkline
 *   bs_list.c    the lists: rows with price, change and sparkline; reordering,
 *                the search that adds a symbol
 *   bs_detail.c  one symbol: the big chart, the ranges, the day's data and
 *                the 52 weeks; lying down, the chart full screen
 *   bs_i18n.c    the app's own catalog: alerts speak while the app is hidden
 *   bolsa.c      the app: life cycle, navigation, the tick, the alerts
 *
 * Everything runs in the LVGL thread: requests go through aos_hal_http_*
 * (the HAL's own task) and are looked at from the tick.
 */
#pragma once

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The app's own catalog, not the front app's (bs_i18n.c): _() is bs_tr() */
const char *bs_tr(const char *es);
void        bs_i18n_load(void);
#undef _
#define _(s) bs_tr(s)

#define BS_APP_ID     "aos.bolsa"

#define BS_SYM_LEN    24      /* "cg:bitcoin", "USD.CCL", "^MERV", "YERBA.BA" */
#define BS_NAME_LEN   48
#define BS_MAX_LISTS  8
#define BS_MAX_SYMS   30      /* per list */
#define BS_MAX_ITEMS  96      /* every symbol in every list, plus a few */
#define BS_MAX_ALERTS 24
#define BS_KEY_LEN    72
#define BS_MAX_CMP    2       /* symbols compared against the one shown */

/* Colours */
#define BS_C_UP       lv_color_hex(0x30D158)
#define BS_C_DOWN     lv_color_hex(0xFF453A)
#define BS_C_LINE     lv_color_hex(0x0A84FF)
#define BS_C_MA1      lv_color_hex(0xFFD60A)
#define BS_C_MA2      lv_color_hex(0xBF5AF2)
#define BS_C_CMP1     lv_color_hex(0xFF9F0A)
#define BS_C_CMP2     lv_color_hex(0x40C8E0)
#define BS_C_GRID     lv_color_hex(0x3A3A3C)

/* ---- symbols (bs_sym.c) ---- */

typedef enum {
    BS_K_STOCK = 0,     /* a share, an ADR, a CEDEAR: needs a keyed provider */
    BS_K_INDEX,         /* an index: also a keyed provider */
    BS_K_CRYPTO,        /* CoinGecko, no key */
    BS_K_DOLAR,         /* the Argentine dollar's houses: ArgentinaDatos */
    BS_K_FX,            /* another currency in pesos: ArgentinaDatos */
    BS_K_RISK,          /* the country risk, in basis points: ArgentinaDatos */
} bs_kind_t;

typedef enum {
    BS_P_NONE = 0,
    BS_P_COINGECKO,
    BS_P_ARGDATOS,
    BS_P_STOOQ,
    BS_P_TWELVE,
    BS_P_ALPHA,
    BS_P_FINNHUB,
    BS_P_COUNT,
} bs_prov_t;

typedef struct {
    char sym[BS_SYM_LEN];       /* the canonical symbol, the key everywhere */
    char show[16];              /* what is drawn: "BTC", "Blue", "MERV" */
    char name[BS_NAME_LEN];
    uint8_t kind;               /* bs_kind_t */
    char ccy[4];                /* "USD", "ARS": the currency of the price */
    const char *remote;         /* provider-specific id (CoinGecko's, the casa) */
} bs_info_t;

/* Fills 'out' for any symbol: from the catalog, from the names the searches
 * learned (bs_cfg), or guessed from its shape. Always succeeds. */
void bs_sym_info(const char *sym, bs_info_t *out);
/* The catalog, for the search: entry i, false past the end. */
bool bs_sym_catalog(int i, bs_info_t *out);
/* The provider that serves this symbol's quotes / its history, given the
 * keys there are (BS_P_NONE: none can). */
bs_prov_t bs_sym_prov_quote(const bs_info_t *in);
bs_prov_t bs_sym_prov_hist(const bs_info_t *in);
const char *bs_prov_name(bs_prov_t p);
/* Upper-case, trimmed, only [A-Z0-9.^:-]; false if nothing is left. */
bool bs_sym_clean(const char *in, char *out, size_t n);

/* ---- configuration (bs_cfg.c) ---- */

typedef struct {
    char name[24];
    char sym[BS_MAX_SYMS][BS_SYM_LEN];
    int  n;
} bs_list_t;

enum { BS_AL_ABOVE = 0, BS_AL_BELOW };

typedef struct {
    char  sym[BS_SYM_LEN];
    uint8_t dir;                /* BS_AL_ABOVE / BS_AL_BELOW */
    bool  fired;
    float value;
} bs_alert_t;

typedef struct {
    bs_list_t  list[BS_MAX_LISTS];
    int        nlists;
    char       key[BS_P_COUNT][BS_KEY_LEN];
    uint8_t    prov;            /* the preferred provider for stocks, 0 = automatic */
    int        refresh_min;     /* quotes, minutes (1-60) */
    bs_alert_t alert[BS_MAX_ALERTS];
    int        nalerts;
    /* display choices, kept between sessions */
    uint8_t    chart_type, range;
    bool       show_vol, show_ma, show_rsi;
    int        cur_list;
} bs_cfg_t;

bs_cfg_t *bs_cfg(void);
void bs_cfg_load(void);             /* the file, or the factory lists */
bool bs_cfg_save(void);
bool bs_cfg_changed(void);          /* the file's date moved (the page saved) */
void bs_cfg_defaults(bs_cfg_t *c);
const char *bs_data_dir(void);      /* <data>/bolsa, created on first use */
/* Lists */
bool bs_cfg_has(int list, const char *sym);
bool bs_cfg_add(int list, const char *sym);
void bs_cfg_remove(int list, int idx);
void bs_cfg_move(int list, int idx, int to);
/* Names learned from searches ("YERBA.BA" -> "Yerba Norte S.A.") */
void bs_names_put(const char *sym, const char *name);
const char *bs_names_get(const char *sym);
void bs_names_load(void);
/* Alerts */
int  bs_alert_find(const char *sym, int from);

/* ---- series (bs_series.c) ---- */

typedef struct {
    int32_t t;                  /* UTC epoch, seconds */
    float   o, h, l, c, v;
} bs_pt_t;

/* Resolutions kept per symbol */
typedef enum {
    BS_R_DAY = 0,               /* today, every 5 minutes */
    BS_R_WEEK,                  /* five days, every 30-60 minutes */
    BS_R_YEAR,                  /* daily, about a year */
    BS_R_FULL,                  /* daily (or weekly), as far back as there is */
    BS_R_COUNT,
} bs_res_t;

typedef struct {
    bs_pt_t *p;
    int      n, cap;
    bool     ohlc;              /* open/high/low are real, not the close */
    bool     vol;               /* there is volume */
    uint8_t  prov;
    int32_t  fetched;           /* epoch when it came */
    uint32_t fetched_ms;        /* uptime when it came, 0 = from the cache */
} bs_series_t;

typedef struct {
    bool    ok;
    float   price, prev, open, high, low, vol, lo52, hi52;
    uint8_t has;                /* BS_Q_* bits */
    uint8_t prov;
    int32_t t;                  /* the price's own time, 0 if not given */
    int32_t fetched;
    uint32_t fetched_ms;
} bs_quote_t;

#define BS_Q_PREV  0x01
#define BS_Q_OHL   0x02
#define BS_Q_VOL   0x04
#define BS_Q_52W   0x08

bool  bs_series_reserve(bs_series_t *s, int n);
void  bs_series_free(bs_series_t *s);
bool  bs_series_push(bs_series_t *s, const bs_pt_t *pt);
void  bs_series_sort(bs_series_t *s);       /* by time, drops duplicates */
void  bs_series_copy(bs_series_t *dst, const bs_series_t *src);
/* Cache files: <data>/bolsa/c/<sym>.<res> */
bool  bs_cache_save(const char *sym, int res, const bs_series_t *s);
bool  bs_cache_load(const char *sym, int res, bs_series_t *s);
bool  bs_quotes_save(void);
void  bs_quotes_load(void);
/* The first index at or after time t */
int   bs_series_find(const bs_series_t *s, int32_t t);
/* Simple moving average of the close, out[i] = NAN until i >= n-1 */
void  bs_sma(const bs_pt_t *p, int cnt, int n, float *out);
/* Wilder's RSI */
void  bs_rsi(const bs_pt_t *p, int cnt, int n, float *out);
/* A quote out of a daily series: the last close, the one before, the day */
void  bs_quote_from_series(const bs_series_t *s, bs_quote_t *q, bool intraday_today);

/* Text */
void  bs_fmt_price(float v, const bs_info_t *in, char *out, size_t n);
void  bs_fmt_num(float v, int dec, char *out, size_t n);
void  bs_fmt_big(float v, char *out, size_t n);         /* 1,2 M  3,4 mil M */
void  bs_fmt_pct(float pct, char *out, size_t n);       /* +1,23 % */
void  bs_fmt_chg(float chg, const bs_info_t *in, char *out, size_t n);
void  bs_fmt_date(int32_t t, bool time_too, char *out, size_t n);
void  bs_fmt_hm(int32_t t, char *out, size_t n);
void  bs_fmt_ago(int32_t t, char *out, size_t n);       /* "hace 5 min" */
/* Days since 1970-01-01 of a civil date, and the inverse */
int32_t bs_days(int y, int m, int d);
void    bs_civil(int32_t days, int *y, int *m, int *d);
int32_t bs_now(void);                                   /* 0 without a clock */
char    bs_dec_sep(void);                               /* ',' or '.' */

/* ---- providers (bs_prov.c) ---- */

typedef enum {
    BS_J_QUOTES = 0,            /* the quotes of several symbols at once */
    BS_J_SERIES,                /* one symbol's series at one resolution */
    BS_J_SEARCH,                /* a search by name */
} bs_job_kind_t;

typedef struct {
    uint8_t kind;               /* bs_job_kind_t */
    uint8_t prov;
    uint8_t res;                /* BS_J_SERIES */
    char    sym[BS_SYM_LEN];    /* BS_J_SERIES, or the first of BS_J_QUOTES */
    char    syms[256];          /* BS_J_QUOTES: comma-separated canonical symbols */
    char    query[40];          /* BS_J_SEARCH */
} bs_job_t;

/* What came back from a search */
typedef struct {
    char sym[BS_SYM_LEN];
    char name[BS_NAME_LEN];
    char where[24];             /* exchange or type */
} bs_hit_t;

/* The URL for a job ("" if this provider cannot do it), and how big the
 * answer may be. */
bool bs_prov_url(const bs_job_t *j, char *url, size_t n, int *max_bytes);
/* What the provider can do */
bool bs_prov_can(bs_prov_t p, int res);
bool bs_prov_has_ohlc(bs_prov_t p, int res);
/* Parsers. They return 0 on success, or a BS_E_* code. */
#define BS_E_PARSE   1          /* not what the provider sends */
#define BS_E_EMPTY   2          /* an answer with no data */
#define BS_E_KEY     3          /* the key is wrong or missing */
#define BS_E_LIMIT   4          /* the provider's rate limit */
#define BS_E_NOSYM   5          /* the provider does not know the symbol */
#define BS_E_PLAN    6          /* the plan does not include it */
#define BS_E_HTTP    7
int  bs_prov_parse_series(const bs_job_t *j, const char *body, int len, bs_series_t *out);
/* Fills the items' quotes itself (bs_item_quote_set) */
int  bs_prov_parse_quotes(const bs_job_t *j, const char *body, int len);
int  bs_prov_parse_search(const bs_job_t *j, const char *body, int len, bs_hit_t *out, int max, int *n);

/* Small JSON readers shared by the parsers */
const char *bs_j_find(const char *from, const char *end, const char *key);
bool  bs_j_str(const char *v, const char *end, char *out, int max);
bool  bs_j_num(const char *v, const char *end, float *out);    /* number or "number" */
const char *bs_j_skip(const char *v, const char *end);         /* past one value */
const char *bs_j_obj_end(const char *p, const char *end);      /* p on '{' or '[' */

/* ---- items: every symbol shown, with its data (bolsa.c) ---- */

typedef struct {
    char        sym[BS_SYM_LEN];
    bs_info_t   info;
    bs_quote_t  q;
    bs_series_t s[BS_R_COUNT];
    bool        loaded[BS_R_COUNT];    /* the cache was looked at */
    uint32_t    asked_ms[BS_R_COUNT];  /* last time it was queued */
    uint32_t    q_asked_ms;
    uint32_t    used_ms;               /* last time something showed it */
    int         err;                   /* last BS_E_* or HTTP code, 0 fine */
    uint8_t     err_prov;
    uint32_t    err_ms;
    float       last_alert_price;      /* for crossing checks */
} bs_item_t;

bs_item_t *bs_item(const char *sym);            /* finds or makes */
bs_item_t *bs_item_find(const char *sym);
int        bs_items_count(void);
bs_item_t *bs_item_at(int i);
/* Asks for what is missing or old: the quote, a resolution */
void bs_item_want_quote(bs_item_t *it);
bs_series_t *bs_item_series(bs_item_t *it, int res, bool ask);
void bs_item_quote_set(const char *sym, const bs_quote_t *q);
/* The sparkline's points: the day if there is a fresh one, else the last
 * month. *base gets the previous close (NAN if none). */
int  bs_item_spark(bs_item_t *it, const bs_pt_t **pts, float *base);
/* The best quote there is: the provider's, or out of the series */
bool bs_item_quote(bs_item_t *it, bs_quote_t *out);

/* ---- network (bs_net.c) ---- */

void bs_net_init(void);
void bs_net_stop(void);
bool bs_net_queue(const bs_job_t *j, bool front);
void bs_net_poll(void);
bool bs_net_busy(void);
int  bs_net_pending(void);
/* Status of a provider for the footer */
bool bs_net_limited(bs_prov_t p, char *why, size_t n);
const char *bs_net_last_error(void);
/* The search results, when they come */
typedef void (*bs_search_cb_t)(const bs_hit_t *hits, int n, int err);
void bs_net_search(const char *q, bs_prov_t p, bs_search_cb_t cb);
/* Called when new data arrived (to redraw) */
extern void (*bs_on_data)(const char *sym, int what);
#define BS_D_QUOTE   (-1)

/* ---- charts (bs_chart.c) ---- */

enum { BS_CT_LINE = 0, BS_CT_AREA, BS_CT_CANDLE, BS_CT_COUNT };

typedef struct bs_chart_s bs_chart_t;
bs_chart_t *bs_chart_create(lv_obj_t *parent, int32_t w, int32_t h);
void bs_chart_set(bs_chart_t *c, const bs_info_t *in, const bs_pt_t *p, int n,
                  bool ohlc, bool vol, float ref, bool intraday);
/* The averages and RSI come from the whole series so the start is right:
 * 'full' is the series, [off, off + n) the part drawn */
void bs_chart_set_full(bs_chart_t *c, const bs_pt_t *full, int full_n, int off);
void bs_chart_cmp_clear(bs_chart_t *c);
void bs_chart_cmp_add(bs_chart_t *c, const char *label, const bs_pt_t *p, int n);
void bs_chart_options(bs_chart_t *c, int type, bool vol, bool ma, bool rsi);
void bs_chart_message(bs_chart_t *c, const char *msg);
lv_obj_t *bs_chart_obj(bs_chart_t *c);
bool bs_chart_candles_ok(bs_chart_t *c);    /* real OHLC, or enough samples per candle */
void bs_chart_delete(bs_chart_t *c);
/* What the finger reads, for the header: text or "" when lifted */
typedef void (*bs_chart_read_cb_t)(const char *text, lv_color_t col, bool active);
void bs_chart_on_read(bs_chart_t *c, bs_chart_read_cb_t cb);
void bs_chart_poll(bs_chart_t *c);     /* the two fingers, from a timer */

/* The sparkline: a plain object that draws pts against base */
lv_obj_t *bs_spark_create(lv_obj_t *parent, int32_t w, int32_t h);
void bs_spark_set(lv_obj_t *o, const bs_pt_t *p, int n, float base, bool up);

/* ---- screens ---- */

typedef struct {
    lv_obj_t *root;
    int32_t W, H;
    bool land;
    int view;                   /* BS_V_* */
    char sym[BS_SYM_LEN];       /* the symbol in the detail */
} bs_ui_t;

enum { BS_V_LIST = 0, BS_V_DETAIL };

extern bs_ui_t bs_ui;
void bs_go(int view, const char *sym);
void bs_list_build(void);
void bs_list_refresh(const char *sym);
bool bs_list_back(void);
void bs_list_leave(void);
void bs_detail_build(const char *sym);
void bs_detail_refresh(const char *sym, int what);
bool bs_detail_back(void);
void bs_detail_leave(void);
void bs_detail_tick(void);

/* The line that is always there: delayed data, the source, no advice */
void bs_footer_text(char *out, size_t n, const bs_info_t *only);

/* Small widgets shared by the screens */
lv_obj_t *bs_box(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *bo_label(lv_obj_t *parent, const char *txt, const lv_font_t *f, lv_color_t col);
lv_obj_t *bs_round_btn(lv_obj_t *parent, const char *glyph, int32_t size, lv_color_t bg,
                       lv_event_cb_t cb, void *ud);
lv_obj_t *bs_chip(lv_obj_t *parent, const char *txt, bool on, lv_event_cb_t cb, void *ud);
lv_obj_t *bs_sheet(const char *title, lv_event_cb_t on_close);   /* a full overlay */
void      bs_sheet_close(void);
lv_obj_t *bs_sheet_obj(void);
void      bs_scpy(char *dst, size_t n, const char *src);
void      bs_alert_sheet(const char *sym);
void      bs_check_alerts(bs_item_t *it);
