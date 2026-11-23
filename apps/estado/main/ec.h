/*
 * P4OS - Estado de la casa (aos.estado): what the pieces share.
 *
 *   estado.c     the app: life cycle, notices, the status bar, the portal's
 *                live channel
 *   ec_cfg.c     estado.txt (the services and the options) and the templates
 *   ec_check.c   the checkers: two threads that take the service that is due
 *                and try it (HTTP and HTTPS over aos_tls, or a TCP port)
 *   ec_cert.c    how many days a server's certificate has left: a TLS 1.2
 *                hello of our own, since the HAL's TLS does not hand the
 *                certificate to apps
 *   ec_hist.c    24 hours in five-minute buckets, 7 days in hours, the last
 *                outages; estado_hist.bin, and estado_eventos.txt for people
 *   ec_ui.c      helpers for the screens
 *   ec_list.c    the board: groups of tiles with their traffic light
 *   ec_detail.c  one service: latency chart, availability, outages
 *   ec_edit.c    the editor (the portal's page is the comfortable one)
 *   ec_desk.c    desk mode: big tiles over the whole screen
 *   ec_i18n.c    the app's own catalog: it speaks while hidden
 *
 * Threads: the checkers write the runtime state (ec_rt_t) and the history
 * under ec_lock(); LVGL's task reads them under the same lock and copies
 * what it draws. The configuration is the UI's: the checkers get a copy of
 * each service when they take it.
 */
#pragma once

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_text_safe.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EC_APP_ID "aos.estado"
#define EC_TAG    "estado"

/* The app's own catalog, not the front app's (ec_i18n.c): _() is ec_tr() */
const char *ec_tr(const char *es);
void ec_i18n_load(void);
#undef _
#define _(s) ec_tr(s)

#define EC_MAX        32
#define EC_NAME       40
#define EC_GROUP      24
#define EC_URL        200
#define EC_HOST       96
#define EC_EXPECT     24
#define EC_WORD       64

#define EC_EVERY_MIN  10
#define EC_EVERY_MAX  3600
#define EC_TO_MIN     1000
#define EC_TO_MAX     8000

enum { EC_HTTP = 0, EC_TCP = 1 };

/* ---- the configuration (ec_cfg.c) ---- */

typedef struct {
    char    name[EC_NAME];
    char    group[EC_GROUP];
    uint8_t kind;               /* EC_HTTP or EC_TCP */
    uint8_t tmpl;               /* index in ec_tmpl(): the glyph */
    char    url[EC_URL];        /* http(s)://host[:port]/path */
    char    host[EC_HOST];      /* EC_TCP */
    int     port;
    char    expect[EC_EXPECT];  /* "200", "200-399", "200,401" */
    char    word[EC_WORD];      /* must be in the response, "" none */
    int     every_s;
    int     slow_ms;            /* yellow above this */
    int     timeout_ms;
    int     cert_days;          /* warn when fewer are left; 0 off */
    bool    paused;
    bool    trust;              /* its own certificate is trusted (estado_<key>.pem) */
} ec_svc_t;

typedef struct {
    int      n;
    ec_svc_t s[EC_MAX];
    int      fails;             /* failures in a row before it is "down" */
    bool     sound;
    bool     boot;              /* start with the board */
} ec_cfg_t;

typedef struct {
    const char *word;           /* written in estado.txt */
    const char *label;          /* N_() */
    uint8_t     kind;
    bool        tls;
    int         port;
    const char *path;
    const char *expect;
    const char *match;
    const char *group;          /* N_() */
    const char *glyph;
    int         every_s, slow_ms;
} ec_tmpl_t;

int              ec_tmpl_n(void);
const ec_tmpl_t *ec_tmpl(int i);
int              ec_tmpl_find(const char *word);
void             ec_svc_from_tmpl(ec_svc_t *s, int t, const char *host);

void     ec_cfg_defaults(ec_cfg_t *c);
bool     ec_cfg_load(ec_cfg_t *c);
bool     ec_cfg_save(const ec_cfg_t *c);
uint32_t ec_cfg_stamp(void);            /* changes when the file does */
void     ec_cfg_path(char *out, size_t n);
const char *ec_svc_addr(const ec_svc_t *s, char *out, size_t n);   /* for people */
bool     ec_url_split(const char *url, char *host, size_t hn, int *port, char *path, size_t pn, bool *tls);
bool     ec_expect_ok(const char *expect, int code);
uint32_t ec_svc_key(const ec_svc_t *s);  /* the history's key: the name */

/* ---- what a check found, and the state (ec_check.c) ---- */

enum { EC_ST_UNKNOWN = 0, EC_ST_UP, EC_ST_SLOW, EC_ST_DOWN, EC_ST_PAUSED };

enum {
    EC_E_NONE = 0,
    EC_E_DNS,           /* the name does not resolve */
    EC_E_REFUSED,       /* nobody on that port */
    EC_E_TIMEOUT,       /* no answer in time */
    EC_E_TLS,           /* the certificate was refused */
    EC_E_CLOCK,         /* no time yet to check a certificate */
    EC_E_CODE,          /* answered, with another code */
    EC_E_WORD,          /* answered without the word */
    EC_E_PROTO,         /* not HTTP */
    EC_E_CLOSED,        /* cut the connection */
    EC_E_MEM,
    EC_E_URL,           /* the address is no good */
    EC_E_N
};

/* cert_state */
enum { EC_CERT_NONE = 0, EC_CERT_OK, EC_CERT_SOON, EC_CERT_EXPIRED, EC_CERT_UNKNOWN };

typedef struct {
    int      state;
    int      fails_row;
    uint32_t since_ms;          /* uptime when the state last changed */
    uint32_t since_epoch;       /* the same, on the clock (0 not known) */
    uint32_t checked_ms;        /* uptime of the last check, 0 never */
    int      ms;                /* the last latency, -1 none */
    int      conn_ms;           /* connecting (TCP and TLS), -1 none */
    int      code;              /* the last HTTP code, 0 none */
    int      err;               /* EC_E_* of the last check */
    char     why[96];           /* what the TLS layer said, English, "" none */
    bool     busy;              /* being checked now */
    bool     now;               /* asked for now */
    uint32_t due_ms;            /* uptime of the next check */
    /* the certificate, HTTPS only */
    int      cert_state;
    int      cert_days;         /* left; < 0 expired */
    bool     cert_self;         /* issued by itself */
    char     cert_name[64];     /* what it is issued to */
    uint32_t cert_due_ms;       /* next look */
    char     cert_check[96];    /* the name to check it against, once trusted */
    uint32_t seq;
} ec_rt_t;

/* A change worth a notice */
enum { EC_EV_DOWN = 0, EC_EV_UP, EC_EV_CERT };

typedef struct {
    int      i;                 /* the service */
    char     name[EC_NAME];
    int      kind;              /* EC_EV_* */
    int      err;
    int      code;
    int32_t  n;                 /* UP: seconds it was down; CERT: days left */
} ec_event_t;

void ec_lock(void);
void ec_unlock(void);
bool ec_core_init(void);
void ec_core_sync(const ec_cfg_t *c);   /* the list changed: keeps the state of the ones that stay */
void ec_core_stop(uint32_t wait_ms);
void ec_core_check_now(int i);          /* -1 all */
const ec_rt_t *ec_rt(int i);            /* under ec_lock() */
const uint8_t *ec_rt_der(int i, int *len);  /* the certificate it showed, kept after a refusal; under ec_lock() */
uint32_t ec_seq(void);                  /* bumps on any change */
bool ec_event_pop(ec_event_t *e);       /* from the UI */
bool ec_net_ok(void);
int  ec_count(int state);               /* under ec_lock() */

const char *ec_err_text(int err);       /* Spanish, translated */
const char *ec_err_word(int err);       /* for files: "timeout" */
lv_color_t  ec_state_color(int state);
const char *ec_state_name(int state);

/* ---- the certificate (ec_cert.c) ---- */

typedef struct {
    bool     ok;
    int64_t  not_after;         /* epoch */
    bool     self;
    char     name[64];          /* CN */
    char     dns[96];           /* first DNS name of the SAN, "" none */
    bool     san_ip;            /* the SAN has IP addresses */
    uint8_t *der;               /* the server's certificate, malloc'd; NULL if not kept */
    int      der_len;
} ec_cert_t;

/* Reads the certificate the server at host:port shows. Blocking: a checker
 * thread. keep_der: hand the DER back (free() it). */
bool ec_cert_fetch(const char *host, int port, int timeout_ms, bool keep_der, ec_cert_t *out);
/* The PEM of a DER certificate, malloc'd */
char *ec_cert_pem(const uint8_t *der, int len);
void  ec_cert_pin_path(uint32_t key, char *out, size_t n);
/* Saves the certificate a service showed last as the one to trust; false if
 * there is none. From LVGL's task. */
bool  ec_cert_trust(int i);

/* ---- the history (ec_hist.c) ---- */

#define EC_B5_N   288           /* five minutes, 24 hours */
#define EC_H_N    168           /* an hour, 7 days */
#define EC_OUT_N  12

typedef struct {
    uint16_t n, ok;
    uint16_t avg_ms, max_ms;
} ec_bk_t;

typedef struct {
    uint32_t start, end;        /* epoch; end 0: still down */
    uint8_t  err;
    uint8_t  pad[3];
} ec_out_t;

typedef struct {
    uint32_t key;
    uint32_t b5_at;             /* epoch / 300 of the newest five-minute bucket */
    uint32_t h_at;              /* epoch / 3600 of the newest hour */
    uint16_t out_n;
    uint16_t pad;
    ec_bk_t  b5[EC_B5_N];       /* ring, by slot % EC_B5_N */
    ec_bk_t  h[EC_H_N];
    ec_out_t out[EC_OUT_N];     /* newest last */
} ec_hist_t;

void ec_hist_init(void);
void ec_hist_free(void);
void ec_hist_sync(const ec_cfg_t *c);   /* one per service, by key; under ec_lock() */
ec_hist_t *ec_hist(int i);              /* under ec_lock() */
void ec_hist_add(int i, bool ok, int ms);               /* under ec_lock() */
/* down: opens one (0) unless one is open (-1); up: closes the open one and
 * returns its seconds, -1 if none was open. Under ec_lock(). */
int32_t ec_hist_outage(int i, bool down, int err);
/* availability over the last 'buckets' five-minute buckets (<= EC_B5_N) or
 * hours; -1 no data */
float ec_hist_avail5(const ec_hist_t *h, int buckets, uint32_t now_slot);
float ec_hist_availh(const ec_hist_t *h, int hours, uint32_t now_slot);
/* the bucket of a slot, NULL if outside or empty */
const ec_bk_t *ec_hist_b5(const ec_hist_t *h, uint32_t slot);
const ec_bk_t *ec_hist_h(const ec_hist_t *h, uint32_t slot);
uint32_t ec_now_epoch(void);            /* 0 while the clock is not set */
void ec_hist_save_if(bool force);       /* from a checker thread or destroy */
void ec_hist_log(const char *name, const char *what, const char *detail, int ms);

/* ---- the app's screens (LVGL's task) ---- */

enum { EC_SCR_LIST = 0, EC_SCR_DETAIL, EC_SCR_EDIT };

typedef struct {
    aos_app_t *self;
    lv_obj_t  *root;
    int32_t    W, H;
    bool       land;
    bool       hidden;
    int        screen;
    int        detail;
    ec_cfg_t   cfg;
    uint32_t   cfg_stamp;
    bool       desk;
} ec_app_t;

extern ec_app_t ec_app;

void ec_build(void);
void ec_go_list(void);
void ec_go_detail(int i);
void ec_go_edit(int i);                 /* -1 a new one */
bool ec_cfg_commit(void);               /* saves ec_app.cfg and restarts the checks */

void ec_list_build(void);
void ec_list_tick(void);
void ec_settings_open(void);
bool ec_settings_back(void);

void ec_detail_build(void);
void ec_detail_tick(void);
bool ec_detail_back(void);

void ec_edit_open(int i);
void ec_edit_build(void);
void ec_edit_keep(void);
bool ec_edit_back(void);
void ec_edit_close(void);

void ec_desk_open(void);
void ec_desk_close(void);
void ec_desk_tick(void);
void ec_desk_relayout(void);
bool ec_desk_on(void);

/* ---- ec_ui.c ---- */

lv_obj_t *ec_box(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *ec_card(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *ec_text(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c, int32_t w);
lv_obj_t *ec_round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud);
lv_obj_t *ec_pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud);
void      ec_set_text(lv_obj_t *l, const char *t);
void      ec_copy(char *dst, size_t n, const char *src);
void      ec_fmt_ms(char *out, size_t n, int ms);
void      ec_fmt_ago(char *out, size_t n, uint32_t secs);   /* "hace 5 min" */
void      ec_fmt_dur(char *out, size_t n, uint32_t secs);   /* "12 min" */
void      ec_fmt_pct(char *out, size_t n, float pct);       /* "99,95 %" */
void      ec_fmt_when(char *out, size_t n, uint32_t epoch); /* "hoy 14:02", "23 nov 14:02" */
const char *ec_glyph(const ec_svc_t *s);
void      ec_beep(bool down);
void      ec_beep_tick(void);
void     *ec_psram(size_t n);
