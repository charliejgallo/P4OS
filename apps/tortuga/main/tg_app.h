/*
 * Tortuga - what the app's parts share: the preferences, the progress, the
 * art, the screens. DESIGN.md is the game; tg_core.h its rules.
 *
 *   tortuga.c    the life cycle, the preferences and the progress
 *   tg_menu.c    the start, "how do you want to program", worlds, levels
 *   tg_play.c    a level: the board, running, stepping, the result
 *   tg_view.c    the board drawn (tg_gfx), the turtle walking the trace
 *   tg_blocks.c  the blocks editor
 *   tg_edit.c    the text editor (Logo, Lua, Python) and its palette
 *   tg_lua.c     Lua 5.4 inside the app, all static
 *   tg_audio.c   the sounds of the pack
 */
#pragma once

#include "lvgl.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_pad.h"
#include "aos_sys_glyphs.h"
#include "aos_mono.h"
#include "aos_theme.h"

#include "tg_core.h"
#include "tg_gfx.h"
#include "tg_pak.h"

#define TG_NWORLD   9
#define TG_PACK     "tortuga_p4.pak"

enum { SH_GROUND, SH_ROCK, SH_LETTUCE, SH_KEY, SH_DOOR, SH_GOAL, SH_BRIDGE, SH_WALK, SH_BUMP, SH_EAT,
       SH_PARTY, SH_N };

enum { SND_STEP, SND_TURN, SND_BUMP, SND_SPLASH, SND_EAT, SND_KEY, SND_DOOR, SND_WIN, SND_FAIL, SND_TAP,
       SND_N };

enum { SCR_HOME, SCR_PICK, SCR_WORLDS, SCR_LEVELS, SCR_PLAY };

typedef struct {
    uint8_t lang;           /* TG_LANG_*: how the player programs */
    uint8_t hard;           /* the hard mode: end exactly on the goal */
    uint8_t speed;          /* 0..4 */
    uint8_t sound;
    uint8_t chosen;         /* "how do you want to program" was answered */
} tg_prefs_t;

typedef struct {
    uint8_t stars[TG_LANG_N];   /* the best, per language */
    uint8_t medal;              /* a bit per language: won in the hard mode */
} tg_score_t;

typedef struct tg_play tg_play_t;

typedef struct {
    aos_app_t *self;
    lv_obj_t *root;
    int W, H;
    bool land;
    int tlang;                  /* TG_ES, TG_EN, TG_DE: the language of the texts */

    tg_prefs_t prefs;
    tg_levels_t levels;
    tg_score_t *score;          /* one per level */

    int screen;
    int world;                  /* the world open in the list */
    int level;                  /* the level played */
    bool free_mode;             /* the sheet with no goal */

    tg_pak_t pak;
    bool have_pak;
    uint8_t *sheet_data[SH_N];
    tg_sheet_t sheet[SH_N];

    lv_obj_t *scr;              /* the current screen */
    lv_timer_t *timer;
    aos_pad_t pad;
    tg_play_t *play;
    bool closing;
} tg_app_t;

extern tg_app_t *tg;

/* tortuga.c */
void tg_go(int screen);
void tg_save_prefs(void);
void tg_save_scores(void);
int  tg_world_stars(int world, int lang);   /* lang < 0: the best of any */
int  tg_total_stars(void);
bool tg_world_open(int world);
int  tg_world_first(int world);             /* index of its first level, -1 */
int  tg_world_count(int world);
int  tg_world_max(void);                     /* the last world, the card's included */
const char *tg_world_name(int world);
const char *tg_world_idea(int world);
const char *tg_lang_name(int lang);
bool tg_lang_available(int lang);
/* where a level's program is kept, per language */
void tg_program_path(char *out, size_t n, const tg_level_t *lv, int lang);
char *tg_program_load(const tg_level_t *lv, int lang);      /* tg_free; NULL: none */
void tg_program_save(const tg_level_t *lv, int lang, const char *text);
lv_obj_t *tg_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud);
lv_obj_t *tg_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);
lv_obj_t *tg_stars_draw(lv_obj_t *parent, int n, int size);
lv_color_t tg_lang_color(int lang);

/* tg_menu.c */
void tg_menu_home(lv_obj_t *scr);
void tg_menu_pick(lv_obj_t *scr);
void tg_menu_worlds(lv_obj_t *scr);
void tg_menu_levels(lv_obj_t *scr);
bool tg_menu_back(void);
void tg_menu_tick(void);
void tg_menu_pad(void);

/* tg_play.c */
void tg_play_open(lv_obj_t *scr);
void tg_play_close(void);
void tg_play_frame(void);
bool tg_play_back(void);
void tg_play_pad(void);

/* tg_audio.c */
void tg_audio_open(void);
void tg_audio_close(void);
void tg_audio_play(int snd);
void tg_audio_pump(void);

/* the built-in levels (tg_levels_data.c, made by tools/gen_levels.py) */
extern const char tg_levels_text[];
extern const unsigned tg_levels_len;
