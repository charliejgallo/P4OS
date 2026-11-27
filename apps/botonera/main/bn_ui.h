/*
 * Botonera - what the grid (bn_ui.c) and the sheets (bn_edit.c) share.
 */
#pragma once

#include "bn.h"

#include "aos_gesture.h"
#include "aos_hal.h"

#define BN_CACHE_N  64                  /* decoded files kept */
#define BN_REQ_N    32                  /* the loader's ring */

typedef enum { CS_NONE = 0, CS_QUEUED, CS_READY, CS_FAILED } cache_state_t;

typedef struct {
    char       file[BN_FILE_MAX];
    bn_clip_t *clip;
    uint32_t   size, mtime;             /* of the file when it was asked for */
    uint32_t   used;                    /* when last played or shown */
    uint8_t    state;
    char       why[16];
} bn_cache_t;

typedef struct {
    int16_t    slot;                    /* the cache's; -1 = make the examples */
    char       path[200];
} bn_req_t;

typedef struct {
    int16_t    slot;
    bn_clip_t *clip;
    char       why[16];
} bn_res_t;

typedef struct {
    uint8_t id;                         /* the finger's, 0 = none */
    int     pad;                        /* the global index it landed on, -1 */
} bn_finger_t;

typedef void (*bn_text_cb)(bn_ui_t *u, const char *text);

struct bn_ui {
    lv_obj_t   *root;
    bn_grid_t  *g;
    int         page;
    bool        edit;
    int         move_from;              /* a pad waiting for where to go, -1 */
    bool        front;
    bool        ready;                  /* the examples are on the card */
    bool        land;
    bool        del_armed;
    uint32_t    del_t;
    bool        cut_told;

    /* the main screen */
    lv_obj_t   *bar, *title, *sub, *b_prev, *b_next, *b_edit, *b_stop, *b_add, *b_del;
    lv_obj_t   *box, *dots, *hint;
    lv_obj_t   *pad[BN_PER_PAGE];
    lv_obj_t   *pad_icon[BN_PER_PAGE], *pad_name[BN_PER_PAGE], *pad_bar[BN_PER_PAGE], *pad_tag[BN_PER_PAGE];
    uint8_t     pad_look[BN_PER_PAGE];  /* what is drawn: 0 idle, 1 playing, 2 loading */
    int16_t     pad_prog[BN_PER_PAGE];

    /* the sheets: one over the grid, one over that */
    lv_obj_t   *sheet, *sheet2;
    int         ed_pad;                 /* the global index being edited */
    lv_obj_t   *ed_preview, *ed_file, *ed_name, *ed_mode[BN_MODE_COUNT], *ed_loop, *ed_vol, *ed_vol_v;
    lv_obj_t   *ed_icons, *ed_colors, *ed_clear, *ed_try;
    bool        ed_clear_armed;
    bn_files_t  files;
    lv_obj_t   *pk_list;
    lv_obj_t   *kb, *ta;
    bn_text_cb  text_cb;
    /* the recorder */
    lv_obj_t   *rc_time, *rc_meter, *rc_btn, *rc_btn_l, *rc_msg, *rc_use, *rc_again;
    char        rc_path[200], rc_file[BN_FILE_MAX];
    bool        rc_on;
    uint32_t    rc_t0;
    int         preview_slot;           /* the cache slot the preview plays, -1 */

    /* fingers on the grid */
    bn_finger_t f[AOS_TOUCH_MAX];
    uint8_t     stale[AOS_TOUCH_MAX];   /* fingers down before the grid could take them */
    bool        covered;

    /* the decoded files and the loader */
    bn_cache_t  cache[BN_CACHE_N];
    uint32_t    clock;
    bool        want[BN_PADS];          /* a touch waiting for its sound */
    bool        held[BN_PADS];          /* a finger (or the phone) on a hold pad */
    bn_req_t    req[BN_REQ_N];
    volatile uint32_t req_w, req_r;
    bn_res_t    res[BN_REQ_N];
    volatile uint32_t res_w, res_r;
    volatile bool ld_quit, ld_done;
    bool        ld_running;

    uint32_t    grid_mt;                /* the file's date when last read or written */
    uint32_t    t_check;
    lv_timer_t *t_touch, *t_ui;
    char        live_last[600];
    uint32_t    live_t;
};

/* bn_ui.c */
void bn_ui_build(bn_ui_t *u);
void bn_ui_refresh(bn_ui_t *u);         /* the page's buttons, as the grid says */
void bn_ui_go_page(bn_ui_t *u, int page);
void bn_ui_save(bn_ui_t *u);
bn_pad_t *bn_ui_pad(bn_ui_t *u, int gi);
int  bn_cache_get(bn_ui_t *u, const char *file);       /* asks for it if needed; -1 */
void bn_ui_press(bn_ui_t *u, int gi);
void bn_ui_release(bn_ui_t *u, int gi);
float bn_gain(int vol);
void bn_ui_preview(bn_ui_t *u, const char *file);      /* plays a file once; NULL stops */
bool bn_ui_engine(bn_ui_t *u, bool on);
void bn_draw_pad(lv_obj_t *o, lv_obj_t *icon, lv_obj_t *name, const bn_pad_t *p, bool lit, bool big);

/* bn_edit.c */
void bn_edit_open(bn_ui_t *u, int gi);
void bn_edit_close(bn_ui_t *u);
void bn_text_open(bn_ui_t *u, const char *title, const char *text, bn_text_cb cb);
void bn_sheets_close(bn_ui_t *u);
void bn_rec_tick(bn_ui_t *u);
void bn_edit_file_changed(bn_ui_t *u);
lv_obj_t *bn_sheet_new(bn_ui_t *u, const char *title, bool top);
lv_obj_t *bn_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud);
