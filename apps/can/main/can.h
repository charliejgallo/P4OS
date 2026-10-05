/*
 * P4OS - CAN: what the app's files share.
 *
 * Two threads and the UI meet here:
 *
 *   the bus thread   lives while the bus is open. It is the only one that
 *                    touches the aos_io_can handle: it drains the receive
 *                    queue into the ring and the table of ids, sends what
 *                    the UI, the periodic list and a replay ask for, and
 *                    reads the controller's state.
 *   the file thread  lives with the app. It writes the recording from the
 *                    ring, feeds a replay from its CSV, and talks to the
 *                    portal page through two files in the data folder (the
 *                    page's commands, and a snapshot while the page looks).
 *   the UI           LVGL's thread; it copies what it shows under the lock
 *                    and formats it outside.
 *
 * One mutex (cn_lock) guards everything below marked "shared". Nobody holds
 * it across a call into the driver or the card.
 */
#pragma once

#include "lvgl.h"
#include "aos_io.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ---- a frame as the app keeps it ---- */

#define CN_EXT 0x01             /* 29-bit id */
#define CN_RTR 0x02             /* remote frame */
#define CN_TX  0x04             /* sent by us */

typedef struct {
    uint64_t t_us;              /* aos_hal_uptime_us() clock */
    uint32_t id;
    uint8_t  len;
    uint8_t  fl;                /* CN_EXT | CN_RTR | CN_TX */
    uint8_t  data[8];
} cn_frame_t;

#define CN_RING      4096       /* frames kept for the view and the recorder (PSRAM) */
#define CN_IDS       192        /* distinct ids followed */
#define CN_TXQ       16
#define CN_PERIODIC  8
#define CN_REPLAYQ   256

/* one id, as the "by id" view shows it */
typedef struct {
    uint32_t id;
    uint8_t  fl;                /* CN_EXT */
    uint8_t  len;
    uint8_t  data[8];
    uint8_t  changed[8];        /* every bit that ever changed */
    uint32_t chg_ms[8];         /* uptime when each byte last changed */
    uint32_t count, count_mark;
    float    hz;
    uint64_t last_us;
    uint32_t period_us;         /* the last gap between two of them */
} cn_id_t;

typedef enum { CN_MODE_LISTEN = 0, CN_MODE_NORMAL, CN_MODE_SELFTEST, CN_MODE_COUNT } cn_mode_t;
typedef enum { CN_FILT_NONE = 0, CN_FILT_STD, CN_FILT_EXT } cn_filt_t;

typedef struct {
    int      tx, rx;            /* GPIOs */
    uint32_t rate;              /* bit/s */
    cn_mode_t mode;
    cn_filt_t fkind;
    uint32_t fid, fmask;
} cn_cfg_t;

typedef struct {
    cn_frame_t f;
    char       name[24];
    uint32_t   period_ms;       /* 0: by hand */
    bool       running;         /* periodic, now */
} cn_saved_t;

#define CN_SAVED_MAX 32

/* ---- the shared state ---- */

typedef struct {
    /* fixed while the bus is open */
    cn_cfg_t cfg;
    bool     open;              /* the UI's view: a bus is open */
    uint64_t t0_us;             /* when it opened: times are shown from here */

    /* shared */
    cn_frame_t *ring;           /* CN_RING */
    uint32_t seq;               /* frames ever put in the ring */
    cn_id_t *ids;               /* CN_IDS */
    int      nids;
    uint32_t ids_gen;           /* bumped when an id is added: the views re-sort */
    char     state[12];         /* "active", ... from the driver */
    aos_can_status_t st;
    float    fps, load;         /* frames a second, bus load 0..1 */
    uint32_t tx_fail;           /* sends nobody acknowledged */
    bool     want_recover;

    cn_frame_t txq[CN_TXQ];
    int      ntxq;

    struct { cn_frame_t f; uint32_t period_ms; uint64_t next_us; bool on; int slot; } per[CN_PERIODIC];

    /* recording (the file thread writes it) */
    bool     rec;
    char     rec_name[48];
    uint32_t rec_seq;           /* next ring frame to write */
    uint32_t rec_frames, rec_lost;
    uint64_t rec_bytes;
    uint64_t rec_t0;

    /* replay: the file thread fills the queue, the bus thread sends */
    bool     replay;
    bool     replay_loop;
    char     replay_name[48];
    uint32_t replay_sent;
    cn_frame_t rq[CN_REPLAYQ];  /* t_us = when to send it */
    int      rq_head, rq_n;
    bool     rq_eof;

    /* the portal */
    uint32_t web_until_ms;
    bool     want_reload;       /* the page rewrote enviar.txt or senales.dbc */
} cn_bus_t;

extern cn_bus_t CB;

void cn_lock(void);
void cn_unlock(void);

/* can_bus.c */
void        cn_bus_init(void);          /* buffers, the file thread */
void        cn_bus_deinit(void);
const char *cn_bus_open(const cn_cfg_t *cfg);   /* NULL ok, else the reason */
void        cn_bus_close(void);
bool        cn_bus_send(const cn_frame_t *f);   /* queued; false if it cannot go */
void        cn_bus_clear(void);
bool        cn_rec_start(void);
void        cn_rec_stop(void);
bool        cn_replay_start(const char *name, bool loop);
void        cn_replay_stop(void);
void        cn_periodic_set(int slot, const cn_frame_t *f, uint32_t period_ms, bool on);
void        cn_periodic_stop_all(void);
const char *cn_dir(void);               /* <card>/can, made if missing */
uint64_t    cn_us(void);
uint32_t    cn_ms(void);                /* the frames' clock in ms (the simulator's uptime_ms has another origin) */

/* can_frame.c: text forms */
int  cn_fmt_id(const cn_frame_t *f, char *out, int cap);          /* "0C0", "18FEF100" */
int  cn_fmt_data(const cn_frame_t *f, char *out, int cap);        /* "00 11 22" */
int  cn_fmt_candump(const cn_frame_t *f, char *out, int cap);     /* "0C0#0011", "123#R" */
bool cn_parse_candump(const char *s, cn_frame_t *f);
bool cn_parse_hex(const char *s, uint32_t *out);
const char *cn_state_name(const char *state);                     /* translated */
lv_color_t  cn_state_color(const char *state);
const char *cn_mode_name(cn_mode_t m);

/* the signals: a DBC subset (BO_ and SG_) */
#define CN_SIG_MAX 64
typedef struct {
    char     name[24];
    char     unit[12];
    uint32_t id;
    bool     ext;
    uint16_t start, len;
    bool     motorola, is_signed;
    float    scale, offset;
} cn_sig_t;
int  cn_sig_load(cn_sig_t *out, int max, char *err, int err_cap);   /* how many */
bool cn_sig_value(const cn_sig_t *s, const uint8_t *data, int len, float *out);
void cn_sig_write_example(void);
void cn_saved_load(void);
void cn_saved_save(void);
extern cn_saved_t CN_SAVED[CN_SAVED_MAX];
extern int        CN_NSAVED;

/* ---- the UI ---- */

enum { CN_TAB_LIVE = 0, CN_TAB_IDS, CN_TAB_SEND, CN_TAB_SIG, CN_TAB_COUNT };

typedef struct {
    lv_obj_t *root, *content;
    int32_t   W, H, cx, cy, cw, ch;     /* the screen; the content area */
    bool      land;
    int       tab;
    lv_obj_t *tabs[CN_TAB_COUNT];
    lv_obj_t *conn_lbl, *conn_dot, *connect_btn, *connect_lbl, *rec_btn, *pause_btn, *pause_lbl;
    lv_obj_t *status, *status2, *recover_btn;
    lv_obj_t *sheet;            /* a full-screen sheet over everything */
    lv_timer_t *timer;
    bool      paused;
    bool      boot_pause;       /* the BOOT button asked for it */
    /* view filters (software): one id only, or ids hidden */
    bool      only_on;
    uint32_t  only_id;
    uint8_t   only_fl;
    uint32_t  hidden[16];
    uint8_t   hidden_fl[16];
    int       nhidden;
} cn_ui_t;

extern cn_ui_t U;
extern cn_cfg_t CN_CFG;         /* what the connection sheet edits */

/* can.c */
void cn_ui_rebuild(void);
void cn_ui_status(void);
void cn_ui_set_tab(int tab);
bool cn_view_hidden(uint32_t id, uint8_t fl);
void cn_toggle_pause(void);
lv_obj_t *cn_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_event_cb_t cb, void *ud);
lv_obj_t *cn_pill(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud);
lv_obj_t *cn_sheet_open(const char *title);
void      cn_sheet_close(void);
void      cn_text_entry(const char *title, const char *value, lv_keyboard_mode_t mode,
                        void (*done)(const char *));
void      cn_cfg_save(void);

/* can_spy.c: "Tramas" and "Por id" */
void cn_live_build(lv_obj_t *parent);
void cn_live_refresh(bool force);
void cn_live_pause(bool on);    /* takes or drops the snapshot */
void cn_ids_build(lv_obj_t *parent);
void cn_ids_refresh(bool force);
void cn_spy_free(void);

/* can_send.c */
void cn_send_build(lv_obj_t *parent);
void cn_send_refresh(void);
void cn_send_load(const cn_frame_t *f);

/* can_sheets.c */
void cn_conn_sheet(void);
void cn_wiring_sheet(void);
void cn_files_sheet(void);

/* can_sig.c */
void cn_sig_build(lv_obj_t *parent);
void cn_sig_refresh(void);
void cn_sig_sample(void);       /* every 100 ms, also off the tab */
void cn_sig_adhoc(uint32_t id, uint8_t fl, int byte, int bits, bool motorola);
void cn_sig_reload(void);
