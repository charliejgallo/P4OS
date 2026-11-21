/*
 * SOLITARIO - what the app's files share
 *
 *   solitario.c   life, the frame, touch and the gamepad, the flow of a hand
 *   so_view.c     where every pile goes, the cards as objects, the motion
 *   so_win.c      the endings: bouncing cards, fireworks, the spiral
 *   so_ui.c       the bars and the panels (menu, games, options, looks...)
 *   so_data.c     settings, statistics, achievements, the hand in progress
 *   so_es.c       the Spanish deck, Truco's drawing made to fan in columns
 *   so_game.c     the rules (no LVGL), so_solve.c the solver,
 *   so_art.c      the French cards, backs and felts (no LVGL)
 */
#pragma once

#include "lvgl.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"

#include "so_art.h"
#include "so_game.h"
#include "so_solve.h"
#include "so_seeds.h"

/* LVGL's public API, but not in the headers lvgl.h pulls in here */
void lv_image_cache_drop(const void *src);

#define SO_TAG       "solitario"
#define SO_FRAME_MS  20

/* the system's edge strips: notifications above, home below, back at the left */
#define SO_EDGE_TOP  52
#define SO_EDGE_BOT  40
#define SO_EDGE_LEFT 28

/* --- settings, statistics, achievements -------------------------------------- */

enum { SO_WIN_BOUNCE = 0, SO_WIN_FIREWORKS, SO_WIN_SPIRAL, SO_WIN_RANDOM, SO_NWINS };

typedef struct {
    uint8_t mode;
    uint8_t scoring;
    uint8_t limited;        /* passes through the stock limited        */
    uint8_t undo;           /* undo allowed                            */
    uint8_t hints;          /* hints allowed                           */
    uint8_t solvable;       /* deal only hands with a solution         */
    uint8_t win_anim;
    uint8_t sound;
    uint8_t autocomplete;
    uint8_t felt, back, deck;
} so_set_t;

typedef struct {
    uint32_t played, won, streak, best_streak;
    uint32_t best_time;     /* seconds, 0 = none yet                   */
    int32_t  best_score;
    uint32_t total_time;
    uint8_t  has_score;
} so_stat_t;

enum {
    SO_ACH_FIRST = 0, SO_ACH_KLON3, SO_ACH_SPIDER1, SO_ACH_SPIDER2, SO_ACH_SPIDER4,
    SO_ACH_FREECELL, SO_ACH_PYRAMID, SO_ACH_TRIPEAKS, SO_ACH_GOLF, SO_ACH_YUKON,
    SO_ACH_SPANISH, SO_ACH_DAILY, SO_ACH_STREAK5, SO_ACH_TEN, SO_ACH_CLEAN,
    SO_ACH_FAST, SO_ACH_VEGAS, SO_NACH
};

/* what an achievement unlocks */
enum { SO_UNL_NONE = 0, SO_UNL_FELT, SO_UNL_BACK, SO_UNL_DECK };

typedef struct {
    const char *name, *desc;    /* N_() keys */
    uint8_t     kind, item;
} so_ach_t;

extern const so_ach_t so_ach[SO_NACH];
extern const char *const so_felt_name[SO_NFELTS];
extern const char *const so_back_name[SO_NBACKS];
extern const char *const so_deck_name[SO_NDECKS];
extern const char *const so_scoring_name[SO_NSCORING];
extern const char *const so_win_name[SO_NWINS];

typedef struct {
    so_stat_t st[SO_NMODES];
    uint32_t  ach;              /* bits of SO_ACH_*                    */
    int32_t   vegas_bank;       /* the cumulative Vegas balance        */
    uint32_t  daily_day;        /* the last daily hand won, as yyyymmdd */
    uint32_t  daily_streak;
} so_data_t;

/* --- the view --------------------------------------------------------------------- */

typedef struct {
    int16_t x, y;               /* the first card's top-left            */
    int16_t dx, dy;             /* fan of face-up cards                 */
    int16_t ddx, ddy;           /* fan of face-down cards               */
    int16_t room;               /* how far a column may run down        */
    int8_t  fan_last;           /* only the top n cards fan (the waste) */
    uint8_t slot;               /* the empty mark, SO_SLOT_*            */
    bool    off;                /* not shown (Pyramid's discard pile)   */
    bool    pad_skip;           /* the gamepad does not stop here       */
} so_pl_t;

typedef struct {
    float   x, y;               /* where it is drawn (the card's top-left) */
    float   x0, y0, x1, y1;     /* the tween                               */
    int16_t t, dur, delay;
    uint8_t ease;
    bool    moving;
    bool    up;                 /* the face it shows                       */
    bool    flip_to;            /* the face it is turning to               */
    int16_t ft, fdelay;         /* the turn: time, and when it starts      */
    bool    flipping;
    int16_t pop;                /* landing at home: ms left of the pop     */
    int16_t rot;                /* 0.1 degree                              */
    int8_t  pile;
    uint8_t idx;
    bool    hidden;
} so_cv_t;

enum { SO_EASE_OUT = 0, SO_EASE_SPRING, SO_EASE_INOUT, SO_EASE_DEAL };

#define SO_MAX_HL 16

typedef struct {
    so_art_t art;
    int      cw, ch;
    int      deck_kind;         /* 0 French, 1 Spanish                  */
    int      art_deck, art_cw, art_ch;
    so_img_t face[52];          /* French: suit * 13 + rank; Spanish 40 */
    so_img_t back;
    so_img_t slot[SO_NSLOTS];
    lv_image_dsc_t face_d[52], back_d, slot_d[SO_NSLOTS];
    int      back_id;

    int      W, H;
    bool     land;
    lv_area_t play;             /* where the piles may go               */
    so_pl_t  pl[SO_MAXPILES];

    lv_obj_t *felt;
    uint16_t *feltmem, *feltclean;
    int       felt_id;
    lv_obj_t *layer;            /* the cards' parent                    */
    lv_obj_t *slots[SO_MAXPILES];
    lv_obj_t *card[SO_MAXCARDS];
    so_cv_t   cv[SO_MAXCARDS];
    lv_obj_t *hl[SO_MAX_HL];    /* the drop targets, outlined           */
    lv_obj_t *shadow[2];        /* under what the finger lifts          */
    lv_obj_t *cursor;           /* the gamepad's frame                  */
    lv_obj_t *hint_a, *hint_b;
    lv_obj_t *ring;             /* the flash of a foundation filling    */
    int16_t   ring_t;
    bool      settling;         /* something moves: restack when done   */
    bool      busy;             /* some card still flies                */
    int       counts[SO_MAXPILES];
    int       stock_badge;      /* Spider: the deals left, as piles     */
    lv_obj_t *badge;
} so_view_t;

/* --- the win ------------------------------------------------------------------------ */

#define SO_WIN_MAXP 96

typedef struct {
    float   x, y, vx, vy, rot, vr, k;
    float   lx, ly;             /* where it was last stamped            */
    int16_t card, life, age;
    uint8_t type;               /* what it is: see so_win.c             */
    bool    live;
} so_part_t;

typedef struct {
    int       kind;
    uint32_t  t;
    int       next;             /* the next card of 'order' to launch   */
    int32_t   wait;             /* ms to the next launch                */
    so_part_t p[SO_WIN_MAXP];
    lv_obj_t *box;              /* the particles' layer                 */
    lv_obj_t *obj[SO_WIN_MAXP];
    lv_obj_t *banner;
    bool      stamped;          /* the felt was drawn on: restore it    */
    int16_t   order[SO_MAXCARDS];
    int       norder;
    uint32_t  rnd;
} so_win_t;

/* --- the app ---------------------------------------------------------------------- */

typedef enum {
    SO_PANEL_NONE = 0,
    SO_PANEL_MENU,
    SO_PANEL_GAMES,
    SO_PANEL_OPTIONS,
    SO_PANEL_LOOKS,
    SO_PANEL_STATS,
    SO_PANEL_ACH,
    SO_PANEL_WON,
    SO_PANEL_STUCK,
    SO_PANEL_HELP,
} so_panel_t;

typedef struct so_app_s {
    lv_obj_t   *root;
    lv_timer_t *timer;
    so_game_t   g;
    so_set_t    set;
    so_data_t   data;
    so_view_t   v;
    so_win_t    win;
    so_solver_t *solver;

    /* the hand */
    bool        daily;          /* this is the day's hand               */
    uint32_t    daily_day;
    uint32_t    elapsed_ms;
    bool        started;        /* a move was made (counts as played)   */
    bool        finished;       /* won, or given up and counted         */
    bool        used_help;      /* an undo or a hint was used           */
    uint32_t    last_ops;       /* where the view's last sync stopped   */
    int         autoplay;       /* > 0: completing by itself            */
    int32_t     auto_wait;

    /* looking for a deal with a solution */
    bool        dealing;
    int         deal_mode;
    uint32_t    deal_seed;
    uint32_t    deal_t;
    int         deal_tries;

    /* the hint, searched over frames */
    bool        hinting;
    int16_t     hint_ms;
    so_move_t   hint_mv;

    /* the finger */
    bool        down, dragging;
    int         press_x, press_y, last_x, last_y;
    float       vx;
    int         pick_pile, pick_idx;
    int         grab_dx, grab_dy;
    uint32_t    press_ms;
    int         tap_card;       /* the card of the last tap that moved  */
    uint32_t    tap_ms;
    bool        legal_to[SO_MAXPILES];

    /* the gamepad */
    aos_pad_t      pad;
    aos_pad_menu_t pmenu;
    int            pad_pile, pad_idx;
    bool           pad_shown;
    int            pad_hold_pile, pad_hold_idx;

    /* the chrome */
    lv_obj_t   *bar;
    lv_obj_t   *lbl_title, *lbl_time, *lbl_score, *lbl_moves, *lbl_status;
    lv_obj_t   *btn_menu, *btn_new, *btn_undo, *btn_hint, *btn_auto;
    lv_obj_t   *panel;
    so_panel_t  panel_kind;
    lv_obj_t   *pad_items[AOS_PAD_MENU_MAX];
    int         npad_items;
    int         looks_tab;
    char        status_txt[96];
    int16_t     status_ms;

    bool        closing, want_exit, leaving;
    uint64_t    prev_ms;
} so_app_t;

/* so_view.c */
void so_view_layout(so_app_t *a);           /* the geometry, from the game and the size */
bool so_view_build(so_app_t *a);            /* objects and images                       */
void so_view_free(so_app_t *a);
void so_view_reset(so_app_t *a, bool deal); /* a new hand: everything to its place      */
void so_view_sync(so_app_t *a, uint32_t op_from, int stagger);
void so_view_frame(so_app_t *a, int dt);
void so_view_card_pos(so_app_t *a, int pile, int idx, float *x, float *y);
int  so_view_hit(so_app_t *a, int x, int y, int *idx);     /* pile under a point */
int  so_view_drop_pile(so_app_t *a, int x, int y);
void so_view_drag(so_app_t *a, int pile, int idx, int x, int y, float tilt);
void so_view_lift(so_app_t *a, int pile, int idx, bool on);
void so_view_highlight(so_app_t *a, const bool *legal, int hover);
void so_view_hint(so_app_t *a, const so_move_t *m, bool on);
void so_view_cursor(so_app_t *a, int pile, int idx, bool held);
void so_view_set_looks(so_app_t *a);        /* felt, back or deck changed               */
void so_view_hide_cards(so_app_t *a, bool hide);
lv_image_dsc_t *so_view_face(so_app_t *a, int card);
so_img_t *so_view_face_img(so_app_t *a, int card);

/* so_win.c */
void so_win_start(so_app_t *a);
bool so_win_frame(so_app_t *a, int dt);     /* false when it is over */
void so_win_stop(so_app_t *a);

/* so_ui.c */
void so_ui_build(so_app_t *a);
void so_ui_refresh(so_app_t *a);            /* time, score, buttons     */
void so_ui_panel(so_app_t *a, so_panel_t p);
void so_ui_status(so_app_t *a, const char *text, int ms);
void so_ui_frame(so_app_t *a, int dt);
void so_ui_pad(so_app_t *a);

/* so_data.c */
void so_data_load(so_app_t *a);
void so_data_save(so_app_t *a);
void so_set_load(so_set_t *s);
void so_set_save(const so_set_t *s);
bool so_game_save(so_app_t *a);
bool so_game_load(so_app_t *a);
void so_game_forget(void);
/* the end of a hand: statistics and achievements; returns new unlock bits */
uint32_t so_data_won(so_app_t *a);
void so_data_lost(so_app_t *a);
bool so_unlocked(const so_app_t *a, int kind, int item);
uint32_t so_today(void);                    /* yyyymmdd, 0 without a clock */
int  so_daily_mode(uint32_t day);
uint32_t so_daily_seed(uint32_t day, int mode);

/* solitario.c: actions the panels call */
void so_new_hand(so_app_t *a, int mode, bool daily);
void so_restart_hand(so_app_t *a);
void so_do_undo(so_app_t *a);
void so_do_hint(so_app_t *a);
void so_play(so_app_t *a, const so_move_t *m, int stagger);
void so_beep(so_app_t *a, int kind);
void so_rebuild(so_app_t *a);               /* the size or the game changed */
void so_exit(so_app_t *a);

/* so_es.c: the Spanish deck into ARGB images, through an LVGL canvas */
int  so_es_height(int cw);
bool so_es_render(lv_obj_t *parent, so_img_t *img, int cw, int ch, int pad, int suit, int rank);

