/*
 * Juegos de mesa - the app's state, shared by its files:
 *
 *   mesa.c       the descriptor, create/destroy, the timer, the home screen
 *   mg_games.c   the games there are: the six of the app and the modules
 *   mg_setup.c   a new game's sheet, the settings, the standalone clock
 *   mg_play.c    a game being played: the list of moves, undo and redo, the
 *                board's thinking on a thread of its own, the clock
 *   mg_view.c    the table: the board's picture, where a cell is on the
 *                screen, the pieces and how they move, what a tap hits
 *   mg_hud.c     what LVGL draws over the table: the players, the clocks,
 *                the buttons, the score sheet, the panels
 *   mg_art.c     the packs: a game's board and pieces in the chosen set
 *   mg_audio.c   the sounds
 *   mg_save.c    the game left open, PGN files
 *   mg_gfx.c, mg_pak.c   Bastion's compositor and packs
 *   g_*.c        the games
 */
#pragma once

#include "mesa_game.h"
#include "mg_gfx.h"
#include "mg_pak.h"

#include "aos_app.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#define MG_ID          "demo.mesa"
#define MG_HIST_MAX    1500
#define MG_GAMES_MAX   24
#define MG_SPR_MAX     MESA_MAX_PIECES
#define MG_FRAME_MS    16

extern const mesa_host_t *mesa_host;

/* ---- the games ---------------------------------------------------------------------- */

typedef struct {
    const mesa_game_t *g;
    char  file[24];             /* a module's file without ".so"; "" built in */
    char  pack[40];             /* its art's pack, "" the app's            */
} mg_slot_t;

extern mg_slot_t mg_games[MG_GAMES_MAX];
extern int        mg_ngames;
void mg_games_scan(void);
void mg_games_free(void);
const mg_slot_t *mg_game_find(const char *word);

/* ---- the art ----------------------------------------------------------------------------- */

enum { SET_MADERA, SET_MARMOL, SET_VIDRIO, SET_METAL, SET_N };

typedef struct {
    float S;                    /* pixels a board unit                       */
    float E;                    /* the camera's height, degrees (90: from above) */
    int   rot;                  /* quarter turns of the board on the picture */
    float cx, cy;               /* the board's middle on the picture         */
    int   pw, ph;               /* the picture's size                        */
} mg_geo_t;

typedef struct {
    mg_sheet_t sheet;
    bool       ok;
} mg_kindart_t;

typedef struct {
    mg_pak_t     pak[2];        /* the set chosen, and the wooden one under it */
    bool         open[2];
    int          set;
    const mesa_game_t *game;
    char         board[16];
    bool         flat;
    uint16_t    *pic;           /* the board's picture, decoded (pw x ph)    */
    mg_geo_t     geo;
    bool         have_pic;
    mg_kindart_t kind[32];
    mg_sheet_t   over;          /* drawn over the pieces (four in a row's frame) */
    bool         have_over;
    int16_t      over_x, over_y;
    mg_sheet_t   dice;          /* six faces */
    bool         have_dice;
} mg_art_t;

bool mg_art_have_set(int set);
void mg_art_load(mg_art_t *a, const mesa_game_t *g, const char *board, int set, bool flat, bool portrait);
void mg_art_free(mg_art_t *a);
/* a game's picture for the menu as an LVGL image (freed with mg_free) */
bool mg_art_thumb(const mesa_game_t *g, lv_image_dsc_t *dsc);

/* ---- a game being played --------------------------------------------------------------- */

typedef struct {
    uint8_t     roll;           /* 1: a roll, of 'rnd'                       */
    uint8_t     pad[3];
    uint32_t    rnd;
    int32_t     clock[2];       /* both clocks once it was made              */
    mesa_move_t m;
} mg_hist_t;

enum { CLK_NONE, CLK_SUDDEN, CLK_FISCHER, CLK_BRONSTEIN, CLK_MOVE };

typedef struct {
    int      mode;
    int32_t  base_ms, inc_ms;
    int32_t  left[2];
    int32_t  spent;             /* this turn's, for Bronstein                */
    uint32_t last;              /* lv_tick of the last count                 */
    int      running;           /* whose clock runs, -1 none                 */
    int      flagged;           /* who ran out, -1 none                      */
} mg_clock_t;

enum { TWO_FACE, TWO_PASS };

typedef struct {
    const mesa_game_t *g;
    int      gi;                /* index into mg_games                       */
    uint8_t  opts[MESA_MAX_OPTS];
    uint32_t seed;
    char     fen[100];          /* a position set up, "" the usual start     */
    int      np;
    uint8_t  human[MESA_MAX_PLAYERS];
    uint8_t  level[MESA_MAX_PLAYERS];
    int      two;               /* TWO_FACE or TWO_PASS                      */
    int      bottom;            /* the player whose side is at the bottom    */
    void    *st;                /* the position now                          */
    void    *tmp;
    mg_hist_t *hist;
    int      n, top;            /* moves made; and up to where redo goes     */
    mg_clock_t clock;
    int      resigned;          /* who gave up, -1 none                      */
    bool     over;
    bool     saved_result;

    /* the moves of the player to move, while it is a person */
    mesa_move_t *moves;
    int      nmoves;
    int      sel;               /* steps chosen so far of the move being made */
    uint8_t  chosen[MESA_MAX_STEPS];

    /* the board thinking */
    volatile bool ai_busy, ai_done, ai_ok;
    mesa_ai_t ai;
    mesa_move_t ai_move;
    void    *ai_st;
    uint32_t ai_t0;
    bool     ai_thread;         /* a thread of ours is out there             */
    uint32_t wait_until;        /* a pause before the board's move shows     */
    bool     hint_busy;

    /* dice in the air */
    bool     rolling;
    uint32_t roll_t0;
    int      roll_shown[4];
} mg_play_t;

/* ---- the table on the screen --------------------------------------------------------------- */

typedef struct {
    bool     used;
    uint8_t  id, kind, show_kind, flags, cell, slot, tray;
    float    x, y;              /* the foot on the screen                    */
    float    z;                 /* how high it is, pixels                    */
    float    fx, fy, tx, ty;
    uint32_t t0, dur;
    float    arc;
    uint8_t  path[24];
    int      npath;
    bool     moving, landed;
    int16_t  order;
} mg_piece_t;

typedef struct {
    int       W, H;
    mg_geo_t  geo;
    int       rot;              /* the board's quarter turns now             */
    float     bw, bh;
    float     ox, oy;           /* the picture's corner on the screen        */
    uint16_t *bg;               /* W x H                                     */
    uint16_t *fb;               /* the canvas's buffer                       */
    uint32_t *shown;
    bool      full;
    lv_obj_t *canvas;
    mg_gfx_t  gfx;
    mg_piece_t pc[MG_SPR_MAX];
    int       cursor;           /* the gamepad's cell, -1 hidden             */
    int       hover_choice;
    bool      dirty_layout;
    uint32_t  anim_until;
    float     tray[2][3];       /* each tray's first slot (x, y) and step    */
} mg_view_t;

/* ---- the app ---------------------------------------------------------------------------- */

enum { SCR_HOME, SCR_PLAY, SCR_CLOCK };
enum { PANEL_NONE, PANEL_PAUSE, PANEL_END, PANEL_CHOICE, PANEL_FILES, PANEL_SHEET };

typedef struct {
    int  set;                   /* SET_*                                     */
    bool flat;
    bool hints;
    bool coords;
    bool sound;
    int  speed;                 /* 0 slow, 1 normal, 2 fast                  */
    bool dm_back;               /* draughts: men take backwards too          */
    bool dm_most;               /* draughts: the capture that takes the most */
} mg_prefs_t;

typedef struct mg_app {
    aos_app_t  *self;
    lv_obj_t   *root;
    int         W, H;
    bool        portrait;
    int         screen;
    lv_timer_t *timer;
    aos_pad_t   pad;
    aos_pad_menu_t menu;
    mg_prefs_t  prefs;
    mg_art_t    art;
    mg_play_t   play;
    mg_view_t   view;
    bool        playing;

    /* the home screen */
    lv_obj_t   *home;
    lv_image_dsc_t thumb[MG_GAMES_MAX];
    bool        thumb_ok[MG_GAMES_MAX];

    /* the game's screen */
    lv_obj_t   *hud;
    lv_obj_t   *card[2], *card_name[2], *card_clock[2], *card_info[2], *card_dot[2];
    lv_obj_t   *status;
    lv_obj_t   *bar;
    lv_obj_t   *btn_undo, *btn_redo, *btn_menu, *btn_sheet, *btn_roll, *btn_hint;
    lv_obj_t   *btn_menu2, *btn_undo2;     /* the far player's, face to face */
    lv_obj_t   *sheet;                     /* the score sheet               */
    lv_obj_t   *sheet_list;
    lv_obj_t   *panel;                     /* a panel over the table        */
    int         panel_kind;
    lv_obj_t   *coord[40];
    int         ncoord;
    bool        hud_dirty;
    uint32_t    status_until;
    char        status_text[96];

    /* the standalone clock */
    lv_obj_t   *clk_half[2], *clk_time[2], *clk_moves[2];
    mg_clock_t  clk;
    int         clk_moves_n[2];

    bool        ai_stop_all;
    bool        button_pause;
    uint32_t    perf_t0, perf_frames, perf_us, perf_tiles;
} mg_app_t;

extern mg_app_t *mg;

/* mesa.c */
void mg_home_show(void);
void mg_home_build(void);
lv_obj_t *mg_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud);
lv_obj_t *mg_label(lv_obj_t *parent, const char *text, const lv_font_t *f, lv_color_t c);
void mg_prefs_save(void);
uint32_t mg_now(void);
uint32_t mg_rand(void);

/* mg_setup.c */
void mg_setup_open(int gi);
void mg_settings_open(void);
void mg_clock_screen(void);
void mg_clock_tick(void);
void mg_clock_free(void);
const char *mg_clock_text(int32_t ms, char *buf, size_t n);
extern const int mg_clock_presets[][2];
extern const int mg_nclock_presets;

/* mg_play.c */
typedef struct {
    int      gi;
    uint8_t  opts[MESA_MAX_OPTS];
    uint8_t  human[MESA_MAX_PLAYERS];
    uint8_t  level[MESA_MAX_PLAYERS];
    int      two;
    int      clock_mode;
    int32_t  clock_base, clock_inc;
    const char *fen;
} mg_new_t;

bool mg_play_start(const mg_new_t *n);
void mg_play_stop(bool keep);           /* keep: saved to go on later */
void mg_play_tick(void);
void mg_play_tap(int x, int y);
void mg_play_pad(void);
void mg_play_undo(void);
void mg_play_redo(void);
void mg_play_resign(void);
void mg_play_hint(void);
void mg_play_roll(void);
void mg_play_choose(const mesa_move_t *m);
bool mg_play_person_to_move(void);
int  mg_play_turn(void);
void mg_play_relayout(void);
void mg_play_ai_cancel(void);
void mg_play_replay(int n);
bool mg_play_can_undo(void);
bool mg_play_can_redo(void);

/* mg_view.c */
bool mg_view_open(void);
void mg_view_close(void);
void mg_view_layout(void);
void mg_view_sync(bool animate);        /* the pieces to the game's position */
void mg_view_frame(void);
int  mg_view_hit(int x, int y);         /* a cell, or MESA_NO_CELL */
int  mg_view_hit_piece(int x, int y);   /* the cell of the piece drawn there */
void mg_view_cell_xy(int cell, int slot, float *x, float *y);
void mg_view_board_xy(float u, float v, float z, float *x, float *y);
bool mg_view_busy(void);
void mg_view_redraw(void);

/* mg_hud.c */
void mg_hud_build(void);
void mg_hud_free(void);
void mg_hud_refresh(void);
void mg_hud_status(const char *text, uint32_t ms);
void mg_hud_clock(void);
void mg_hud_sheet_toggle(void);
void mg_hud_sheet_refresh(void);
void mg_hud_pause(void);
void mg_hud_end(void);
void mg_hud_choice(const mesa_move_t *moves, int n);
void mg_hud_panel_close(void);
bool mg_hud_back(void);
void mg_hud_pad(void);
void mg_hud_coords(void);

/* mg_audio.c */
enum { SND_MOVE, SND_CAPTURE, SND_CHECK, SND_DICE, SND_FLIP, SND_DROP, SND_WIN, SND_LOSE, SND_TAP, SND_DENY,
       SND_N };
void mg_audio_open(void);
void mg_audio_close(void);
void mg_audio_play(int snd);
void mg_audio_pump(void);

/* mg_save.c */
bool mg_save_game(void);
bool mg_save_load(mg_new_t *n, bool apply);     /* the game left open */
void mg_save_forget(void);
bool mg_pgn_write(char *name_out, size_t n);
int  mg_pgn_list(char names[][40], int max);
bool mg_pgn_read(const char *name);
