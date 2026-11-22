/*
 * Juegos de mesa - the app's state, shared by its files:
 *
 *   mesa.c       the descriptor, create/destroy, the timer, the home screen
 *   jm_games.c   the games there are: the six of the app and the modules
 *   jm_setup.c   a new game's sheet, the settings, the standalone clock
 *   jm_play.c    a game being played: the list of moves, undo and redo, the
 *                board's thinking on a thread of its own, the clock
 *   jm_view.c    the table: the board's picture, where a cell is on the
 *                screen, the pieces and how they move, what a tap hits
 *   jm_hud.c     what LVGL draws over the table: the players, the clocks,
 *                the buttons, the score sheet, the panels
 *   jm_art.c     the packs: a game's board and pieces in the chosen set
 *   jm_audio.c   the sounds
 *   jm_save.c    the game left open, PGN files
 *   jm_gfx.c, jm_pak.c   Bastion's compositor and packs
 *   g_*.c        the games
 */
#pragma once

#include "mesa_game.h"
#include "jm_gfx.h"
#include "jm_pak.h"

#include "aos_app.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#define JM_ID          "demo.mesa"
#define JM_HIST_MAX    1500
#define JM_GAMES_MAX   24
#define JM_SPR_MAX     MESA_MAX_PIECES
#define JM_FRAME_MS    16

extern const mesa_host_t *mesa_host;

/* ---- the games ---------------------------------------------------------------------- */

typedef struct {
    const mesa_game_t *g;
    char  file[24];             /* a module's file without ".so"; "" built in */
    char  pack[40];             /* its art's pack, "" the app's            */
} jm_slot_t;

extern jm_slot_t jm_games[JM_GAMES_MAX];
extern int        jm_ngames;
void jm_games_scan(void);
void jm_games_free(void);
const jm_slot_t *jm_game_find(const char *word);

/* ---- the art ----------------------------------------------------------------------------- */

enum { SET_MADERA, SET_MARMOL, SET_VIDRIO, SET_METAL, SET_N };

typedef struct {
    float S;                    /* pixels a board unit                       */
    float E;                    /* the camera's height, degrees (90: from above) */
    int   rot;                  /* quarter turns of the board on the picture */
    float cx, cy;               /* the board's middle on the picture         */
    int   pw, ph;               /* the picture's size                        */
} jm_geo_t;

typedef struct {
    jm_sheet_t sheet;
    bool       ok;
} jm_kindart_t;

typedef struct {
    jm_pak_t     pak[2];        /* the set chosen, and the wooden one under it */
    bool         open[2];
    int          set;
    const mesa_game_t *game;
    char         board[16];
    bool         flat;
    uint16_t    *pic;           /* the board's picture, decoded (pw x ph)    */
    jm_geo_t     geo;
    bool         have_pic;
    jm_kindart_t kind[32];
    jm_sheet_t   over;          /* drawn over the pieces (four in a row's frame) */
    bool         have_over;
    int16_t      over_x, over_y;
    jm_sheet_t   dice;          /* six faces */
    bool         have_dice;
} jm_art_t;

bool jm_art_have_set(int set);
void jm_art_load(jm_art_t *a, const mesa_game_t *g, const char *board, int set, bool flat, bool portrait);
void jm_art_free(jm_art_t *a);
/* a game's picture for the menu as an LVGL image (freed with jm_free) */
bool jm_art_thumb(const mesa_game_t *g, lv_image_dsc_t *dsc);

/* ---- a game being played --------------------------------------------------------------- */

typedef struct {
    uint8_t     roll;           /* 1: a roll, of 'rnd'                       */
    uint8_t     pad[3];
    uint32_t    rnd;
    int32_t     clock[2];       /* both clocks once it was made              */
    mesa_move_t m;
} jm_hist_t;

enum { CLK_NONE, CLK_SUDDEN, CLK_FISCHER, CLK_BRONSTEIN, CLK_MOVE };

typedef struct {
    int      mode;
    int32_t  base_ms, inc_ms;
    int32_t  left[2];
    int32_t  spent;             /* this turn's, for Bronstein                */
    uint32_t last;              /* lv_tick of the last count                 */
    int      running;           /* whose clock runs, -1 none                 */
    int      flagged;           /* who ran out, -1 none                      */
} jm_clock_t;

enum { TWO_FACE, TWO_PASS };

typedef struct {
    const mesa_game_t *g;
    int      gi;                /* index into jm_games                       */
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
    jm_hist_t *hist;
    int      n, top;            /* moves made; and up to where redo goes     */
    jm_clock_t clock;
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
} jm_play_t;

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
} jm_piece_t;

typedef struct {
    int       W, H;
    jm_geo_t  geo;
    int       rot;              /* the board's quarter turns now             */
    float     bw, bh;
    float     ox, oy;           /* the picture's corner on the screen        */
    uint16_t *bg;               /* W x H                                     */
    uint16_t *fb;               /* the canvas's buffer                       */
    uint32_t *shown;
    bool      full;
    lv_obj_t *canvas;
    jm_gfx_t  gfx;
    jm_piece_t pc[JM_SPR_MAX];
    int       cursor;           /* the gamepad's cell, -1 hidden             */
    int       hover_choice;
    bool      dirty_layout;
    uint32_t  anim_until;
    float     tray[2][3];       /* each tray's first slot (x, y) and step    */
} jm_view_t;

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
} jm_prefs_t;

typedef struct jm_app {
    aos_app_t  *self;
    lv_obj_t   *root;
    int         W, H;
    bool        portrait;
    int         screen;
    lv_timer_t *timer;
    aos_pad_t   pad;
    aos_pad_menu_t menu;
    jm_prefs_t  prefs;
    jm_art_t    art;
    jm_play_t   play;
    jm_view_t   view;
    bool        playing;

    /* the home screen */
    lv_obj_t   *home;
    lv_image_dsc_t thumb[JM_GAMES_MAX];
    bool        thumb_ok[JM_GAMES_MAX];

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
    jm_clock_t  clk;
    int         clk_moves_n[2];

    bool        ai_stop_all;
    bool        button_pause;
    uint32_t    perf_t0, perf_frames, perf_us, perf_tiles;
} jm_app_t;

extern jm_app_t *jm;

/* mesa.c */
void jm_home_show(void);
void jm_home_build(void);
lv_obj_t *jm_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud);
lv_obj_t *jm_label(lv_obj_t *parent, const char *text, const lv_font_t *f, lv_color_t c);
void jm_prefs_save(void);
uint32_t jm_now(void);
uint32_t jm_rand(void);

/* jm_setup.c */
void jm_setup_open(int gi);
void jm_settings_open(void);
void jm_clock_screen(void);
void jm_clock_tick(void);
void jm_clock_free(void);
const char *jm_clock_text(int32_t ms, char *buf, size_t n);
extern const int jm_clock_presets[][2];
extern const int jm_nclock_presets;

/* jm_play.c */
typedef struct {
    int      gi;
    uint8_t  opts[MESA_MAX_OPTS];
    uint8_t  human[MESA_MAX_PLAYERS];
    uint8_t  level[MESA_MAX_PLAYERS];
    int      two;
    int      clock_mode;
    int32_t  clock_base, clock_inc;
    const char *fen;
} jm_new_t;

bool jm_play_start(const jm_new_t *n);
void jm_play_stop(bool keep);           /* keep: saved to go on later */
void jm_play_tick(void);
void jm_play_tap(int x, int y);
void jm_play_pad(void);
void jm_play_undo(void);
void jm_play_redo(void);
void jm_play_resign(void);
void jm_play_hint(void);
void jm_play_roll(void);
void jm_play_choose(const mesa_move_t *m);
bool jm_play_person_to_move(void);
int  jm_play_turn(void);
void jm_play_relayout(void);
void jm_play_ai_cancel(void);
void jm_play_replay(int n);
bool jm_play_can_undo(void);
bool jm_play_can_redo(void);

/* jm_view.c */
bool jm_view_open(void);
void jm_view_close(void);
void jm_view_layout(void);
void jm_view_sync(bool animate);        /* the pieces to the game's position */
void jm_view_frame(void);
int  jm_view_hit(int x, int y);         /* a cell, or MESA_NO_CELL */
int  jm_view_hit_piece(int x, int y);   /* the cell of the piece drawn there */
void jm_view_cell_xy(int cell, int slot, float *x, float *y);
void jm_view_board_xy(float u, float v, float z, float *x, float *y);
bool jm_view_busy(void);
void jm_view_redraw(void);

/* jm_hud.c */
void jm_hud_build(void);
void jm_hud_free(void);
void jm_hud_refresh(void);
void jm_hud_status(const char *text, uint32_t ms);
void jm_hud_clock(void);
void jm_hud_sheet_toggle(void);
void jm_hud_sheet_refresh(void);
void jm_hud_pause(void);
void jm_hud_end(void);
void jm_hud_choice(const mesa_move_t *moves, int n);
void jm_hud_panel_close(void);
bool jm_hud_back(void);
void jm_hud_pad(void);
void jm_hud_coords(void);

/* jm_audio.c */
enum { SND_MOVE, SND_CAPTURE, SND_CHECK, SND_DICE, SND_FLIP, SND_DROP, SND_WIN, SND_LOSE, SND_TAP, SND_DENY,
       SND_N };
void jm_audio_open(void);
void jm_audio_close(void);
void jm_audio_play(int snd);
void jm_audio_pump(void);

/* jm_save.c */
bool jm_save_game(void);
bool jm_save_load(jm_new_t *n, bool apply);     /* the game left open */
void jm_save_forget(void);
bool jm_pgn_write(char *name_out, size_t n);
int  jm_pgn_list(char names[][40], int max);
bool jm_pgn_read(const char *name);
