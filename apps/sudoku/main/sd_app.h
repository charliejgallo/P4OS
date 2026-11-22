/*
 * SUDOKU - what the app's files share.
 *
 *   sudoku.c     the entry, the life cycle, the gamepad and the keyboard
 *   sd_core.c    the engine: solver, techniques, generator (no LVGL)
 *   sd_gen.c     the generator's thread and the stock of ready boards
 *   sd_game.c    a game: placing, notes, undo, hints, the end and its coins
 *   sd_board.c   the board, one canvas, and touches on it
 *   sd_ui.c      the screens: menu, game, shop, preview, statistics, settings
 *   sd_theme.c   the themes, their art from the pack (a thread) and the
 *                moving background
 *   sd_save.c    /data/sudoku: coins, themes, statistics, settings, the game
 *   sd_snd.c     the sounds, mixed into the streaming speaker
 *
 * Everything that must outlive a destroy/create (the screen turning) lives
 * in the three statics declared here: the game (sd_G), the saved data
 * (sd_D) and the art in memory (sd_A, sd_theme.c). The UI (sd_U) is rebuilt.
 */
#pragma once

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_pad.h"
#include "aos_pad_menu.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "sd_core.h"

#include <stdbool.h>
#include <stdint.h>

#define SD_TAG "sudoku"

/* LVGL's, exported to the apps but not in the headers they see (as Clima) */
extern void lv_image_cache_drop(const void *src);

/* ---- modes ---------------------------------------------------------------- */

enum { SD_M_CLASSIC = 0, SD_M_MINI, SD_M_KILLER, SD_M_DIAG, SD_M_DAILY, SD_MODES };

extern const uint8_t SD_MODE_LEVELS[SD_MODES];
extern const char *const SD_MODE_NAME[SD_MODES];
extern const char *const SD_LEVEL_NAME[SD_LEVELS];
sd_variant_t sd_mode_variant(int mode);

/* ---- the game --------------------------------------------------------------- */

#define SD_UNDO_MAX 3000

typedef struct {
    uint8_t  cell, oldv, newv, more;    /* more: the next entry is the same action */
    uint16_t oldn, newn;
} sd_undo_t;

/* A puzzle as it is kept: the shape is rebuilt from this. */
typedef struct {
    uint8_t n, diag, ncages, level;
    uint8_t cage_of[SD_MAXC];
    uint8_t cage_sum[SD_MAXCAGES];
    uint8_t given[SD_MAXC];
    uint8_t sol[SD_MAXC];
} sd_packed_t;

void sd_pack(const sd_puzzle_t *p, sd_packed_t *out);
void sd_unpack_shape(const sd_packed_t *in, sd_shape_t *s);

typedef struct {
    bool     active;            /* a game in progress (saved, resumable)     */
    bool     won;
    uint8_t  mode, level;
    uint32_t daily;             /* yyyymmdd of a daily challenge, else 0     */
    sd_packed_t puz;
    uint8_t  val[SD_MAXC];      /* given or entered                          */
    uint16_t notes[SD_MAXC];
    uint16_t hint_elim[SD_MAXC];/* candidates the hints already ruled out    */
    uint32_t elapsed_ms;
    uint16_t mistakes, hints;
    bool     assisted;          /* errors were shown, or automatic notes     */
    uint16_t nundo, ntop;       /* [0, nundo) done, [nundo, ntop) to redo    */
    sd_undo_t undo[SD_UNDO_MAX];
    /* the board as the player left it */
    int8_t   sel;
    uint8_t  digit;
    bool     notes_mode;
    /* the result, once won */
    uint16_t coins_won;
    uint16_t pay[6];            /* base, no hints, no mistakes, time, no help, streak */
    bool     record;
} sd_game_t;

extern sd_game_t sd_G;
extern sd_shape_t sd_shape;     /* sd_G.puz unpacked */

/* ---- saved data ------------------------------------------------------------- */

typedef struct {
    uint16_t played, won;
    uint32_t best_s, sum_s;
} sd_stat_t;

typedef struct {
    bool     loaded;
    uint32_t coins, coins_total;
    uint32_t owned;             /* bit per theme                             */
    uint8_t  theme;
    bool     mark_errors, hl_same, hl_peers, auto_clean, sound, anim;
    uint8_t  menu_variant;      /* the last one chosen in the menu           */
    sd_stat_t st[SD_MODES][SD_LEVELS];
    uint32_t last_day;          /* days since 1970 of the last game won      */
    uint16_t streak, best_streak;
    uint32_t daily_done;        /* yyyymmdd of the last daily won            */
    uint32_t daily_time_s;
    uint16_t daily_streak;
} sd_data_t;

extern sd_data_t sd_D;

void sd_save_dir(char *out, size_t n);
void sd_data_load(void);
void sd_data_save(void);
bool sd_game_load(void);
void sd_game_save(void);
void sd_game_forget(void);

/* ---- dates ------------------------------------------------------------------- */

bool     sd_today(uint32_t *ymd, int *wday);        /* false: no clock yet */
uint32_t sd_ymd_days(uint32_t ymd);                 /* days since 1970-01-01 */

/* ---- the generator's thread ----------------------------------------------------- */

void sd_gen_start(void);
void sd_gen_stop(void);         /* waits for the thread                    */
/* A board of that mode and level: true and 'out' at once when one is ready
 * in the stock; else false, and it is asked for (sd_gen_poll). */
bool sd_gen_take(int mode, int level, uint32_t daily, sd_packed_t *out);
bool sd_gen_poll(sd_packed_t *out);     /* the one asked for, once ready   */
bool sd_gen_busy(void);
void sd_daily_pick(uint32_t ymd, int wday, int *mode, int *level);

/* ---- themes --------------------------------------------------------------------- */

enum { SD_DECOR_NONE = 0, SD_DECOR_RISE, SD_DECOR_CROSS, SD_DECOR_FALL, SD_DECOR_TWINKLE,
       SD_DECOR_WANDER, SD_DECOR_WAVE };

typedef struct {
    const char *key;            /* the pack's entries: bg_<key>, tx_<key>...  */
    const char *name;           /* N_()                                      */
    uint16_t    price;
    uint32_t    bg, card, text, dim, accent;
    uint8_t     card_opa;
    uint32_t    board, line, box, given, user, note, err, sel, peer, same, cage, hint;
    uint8_t     line_opa, peer_opa, same_opa;
    uint8_t     decor[3];       /* the motion of each of its sprites          */
    uint8_t     decor_n[3];     /* how many of each                           */
    uint8_t     snd;            /* sound set                                  */
} sd_theme_t;

#define SD_THEMES 16
extern const sd_theme_t SD_THEME[SD_THEMES];

/* The art of the theme in use (or being previewed), in PSRAM. */
typedef struct {
    int       theme;            /* what is loaded, -1 none                  */
    uint16_t *bg;               /* SD_BG_W x SD_BG_H                         */
    uint16_t *tex;              /* SD_TEX x SD_TEX                           */
    uint8_t  *g9, *g6;          /* given digits, RGB565A8, 9 and 6 of them   */
    int       g9w, g6w;         /* their side                                */
    uint8_t  *spr[3];           /* decor sprites                             */
    int       spr_w[3], spr_h[3];
    uint16_t *thumb[SD_THEMES]; /* the shop's, SD_THUMB square               */
} sd_art_t;

#define SD_BG_W  1280
#define SD_BG_H  1280
#define SD_TEX   720
#define SD_THUMB 200

extern sd_art_t sd_A;

const sd_theme_t *sd_theme(void);       /* the one shown (preview or in use) */
void sd_theme_show(int idx);            /* loads it in the background         */
bool sd_theme_ready(void);              /* what was asked is in place         */
void sd_theme_thumbs(void);             /* load the shop's thumbnails         */
bool sd_theme_poll(void);               /* LVGL side: swaps in what arrived; true if anything did */
void sd_theme_stop(void);
bool sd_theme_pak_ok(void);

/* the moving background, over the picture and under everything else */
void sd_decor_build(lv_obj_t *parent);
void sd_decor_step(uint32_t dt_ms);
void sd_decor_clear(void);

/* ---- sound ---------------------------------------------------------------------- */

enum { SD_S_PLACE = 0, SD_S_NOTE, SD_S_ERASE, SD_S_ERROR, SD_S_UNIT, SD_S_WIN, SD_S_TAP, SD_S_HINT, SD_SOUNDS };
#define SD_SOUND_SETS 7

void sd_snd_open(void);
void sd_snd_close(void);
void sd_snd_load(int set, uint8_t **data, uint32_t *len);  /* thread: from the pack */
void sd_snd_play(int snd);
void sd_snd_pump(void);

/* ---- the game's rules (sd_game.c) ---------------------------------------------- */

void sd_game_new(int mode, int level, uint32_t daily, const sd_packed_t *p);
void sd_game_input(int digit);          /* a digit (or 0: erase) on the selection */
void sd_game_erase(void);
void sd_game_undo(void);
void sd_game_redo(void);
void sd_game_autonotes(void);
bool sd_game_hint(sd_step_t *st);       /* the next step, from the board as it is */
void sd_game_hint_apply(const sd_step_t *st);
int  sd_game_remaining(int digit);      /* how many of that digit are still to go */
bool sd_game_wrong(int cell);           /* the value there is not the solution's */
void sd_game_tick(uint32_t dt_ms);
uint32_t sd_game_seconds(void);
const char *sd_tech_name(int tech);
void sd_hint_text(const sd_step_t *st, char *out, size_t n);

/* ---- the board (sd_board.c) ------------------------------------------------------ */

typedef struct {
    lv_obj_t *canvas, *touch;
    uint16_t *buf;
    int       x, y, cell, px;       /* where, the cell, the side in pixels  */
    int       stride;               /* the buffer's row, in pixels          */
    bool      dirty;
    uint8_t   dmask[SD_MAXC];       /* cells to redraw                       */
    const sd_step_t *hint;          /* highlighted while the hint shows     */
    bool      cursor;               /* the pad's frame                       */
    bool      paused;               /* the board is covered                 */
    bool      demo;                 /* the shop's preview: not the game's   */
} sd_board_t;

extern sd_board_t sd_B;

void sd_board_create(lv_obj_t *parent, int x, int y, int max_px);
void sd_board_destroy(void);
void sd_board_all(void);
void sd_board_cell(int c);
void sd_board_present(void);
/* a demo board for the theme preview (a fixed position, not the game) */
void sd_board_demo(bool on);

/* ---- the screens (sd_ui.c) -------------------------------------------------------- */

enum { SD_SCR_MENU = 0, SD_SCR_GAME, SD_SCR_SHOP, SD_SCR_PREVIEW, SD_SCR_STATS, SD_SCR_SETTINGS };

typedef struct {
    aos_app_t *self;
    lv_obj_t  *root, *bgimg, *decor, *scr;
    int        W, H;
    bool       land;
    int        screen;
    int        preview;             /* the theme in the preview             */
    lv_timer_t *timer;
    uint32_t   last_ms;
    /* game screen */
    lv_obj_t  *lbl_title, *lbl_time, *lbl_coins;
    lv_obj_t  *pad_btn[SD_MAXN + 1], *pad_cnt[SD_MAXN + 1];
    lv_obj_t  *btn_notes, *btn_tools[6];
    lv_obj_t  *panel;               /* hint, pause, the end, waiting        */
    int        panel_kind;
    sd_step_t  hint;
    bool       waiting;             /* for a board from the generator       */
    int        want_mode, want_level;
    uint32_t   want_daily;
    lv_obj_t  *thumbs[SD_THEMES];
    lv_image_dsc_t thumb_dsc[SD_THEMES];
    lv_image_dsc_t bg_dsc;
    aos_pad_t  pad;
    aos_pad_menu_t menu;
    uint32_t   kb_bits;             /* keys turned into pad presses          */
    int        theme_shown;         /* to know when the art changed          */
} sd_ui_t;

extern sd_ui_t sd_U;

void sd_ui_build(void);             /* the current screen, from scratch      */
void sd_ui_go(int screen);
void sd_ui_frame(void);             /* every 30 ms                           */
void sd_ui_refresh_game(void);      /* labels and the number pad             */
void sd_ui_won(void);
void sd_ui_toast(const char *msg);
void sd_ui_start(int mode, int level, uint32_t daily);
bool sd_ui_back(void);
bool sd_ui_key(uint32_t key, uint8_t mods);
void sd_ui_pad(void);
void sd_ui_set_bg(void);            /* the theme's picture on the root       */

/* helpers shared by the screens */
lv_obj_t *sd_box(lv_obj_t *parent, uint32_t color, uint8_t opa, int radius);
lv_obj_t *sd_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color);
void      sd_fmt_time(char *out, size_t n, uint32_t s);
