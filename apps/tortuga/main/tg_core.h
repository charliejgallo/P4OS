/*
 * Tortuga - the game's core, without LVGL: the levels (the .tl files), the
 * world the turtle walks (a board of tiles, or a sheet of paper), the trace
 * of what it did, and the game's own Logo. tools/tg_ref.py is the reference
 * this follows; tools/tg_harness.c runs every level against it on the Mac.
 *
 * Nothing here draws or allocates on its own behind the caller's back:
 * buffers come from the caller (tg_malloc for what must be on the heap),
 * and messages go through _() so the app can translate them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* the messages are the app's catalog's (aos_i18n.h); on the Mac (the
 * harness, -DTG_NO_I18N) they stay in Spanish */
#ifdef TG_NO_I18N
#define _(s) (s)
#else
#include "aos_i18n.h"
#endif

void *tg_malloc(size_t n);      /* PSRAM on the board; zeroed */
void  tg_free(void *p);

#define TG_MAXW         16
#define TG_MAXH         16
#define TG_MAXVAR       4       /* maps of one level */
#define TG_MAX_ACTIONS  5000    /* what the turtle does in one run */
#define TG_MAX_STEPS    200000  /* the interpreter's steps in one run */
#define TG_PAPER        200     /* the sheet goes from -200 to 200 */
#define TG_EV_CAP       (TG_MAX_ACTIONS + 64)

enum { TG_LANG_BLOCKS, TG_LANG_LOGO, TG_LANG_LUA, TG_LANG_PY, TG_LANG_N };
enum { TG_ES, TG_EN, TG_DE, TG_NL };     /* the languages of the texts */

/* ------------------------------------------------------------------ orders */

enum {
    TG_O_FORWARD, TG_O_BACK, TG_O_RIGHT, TG_O_LEFT, TG_O_EAT, TG_O_SAY,
    TG_O_PENDOWN, TG_O_PENUP, TG_O_COLOR,
    TG_S_PATH_AHEAD, TG_S_PATH_RIGHT, TG_S_PATH_LEFT, TG_S_WATER_AHEAD,
    TG_S_LETTUCE, TG_S_AT_GOAL, TG_S_HAS_KEY,
    TG_O_N
};

typedef struct {
    const char *es, *en;        /* the names Lua and Python use */
    const char *logo_es, *logo_en;  /* how Logo writes them */
    const char *alias[4];       /* LOGO's classic names */
    uint8_t sense, amin, amax;
} tg_order_t;

extern const tg_order_t tg_orders[TG_O_N];
int tg_order_find(const char *name);        /* any of its names, or -1 */

/* ------------------------------------------------------------------ levels */

enum { TG_G_REACH = 1, TG_G_LETTUCE = 2, TG_G_SAY_LETTUCE = 4, TG_G_DRAW = 8 };
enum { TG_DIR_N, TG_DIR_E, TG_DIR_S, TG_DIR_W };
enum { TG_SOL_LOGO, TG_SOL_LUA, TG_SOL_PY, TG_SOL_N };

#define TG_HINTS 3
#define TG_CARDS 4          /* the cards of a story, before a level */
#define TG_MAX_WORLDS 24

typedef struct {
    const char *id;                     /* "4-1" */
    int world, num;
    const char *title[TG_NL];           /* NULL: the Spanish one */
    const char *hint[TG_HINTS][TG_NL];
    int nhint;
    const char *card[TG_CARDS][TG_NL];  /* shown before the level, first time */
    int ncard;
    uint32_t orders;                    /* bit per order; 0: all */
    uint8_t goals;
    uint8_t star3, star2;
    int nmap;
    uint8_t w[TG_MAXVAR], h[TG_MAXVAR];
    const char *row[TG_MAXVAR][TG_MAXH];
    bool has_start;
    float sx, sy;
    uint8_t sdir;
    const char *figure;                 /* Logo that draws the figure */
    const char *sol[TG_SOL_N];
} tg_level_t;

typedef struct {
    char *buf;                          /* the text, cut in place */
    tg_level_t *lv;
    int n;
    /* worlds a file names (@mundo N, then @titulo and @idea): the game's
     * own have their names in the app; a pack's come from here */
    struct {
        int world;
        const char *title[TG_NL], *idea[TG_NL];
    } wh[TG_MAX_WORLDS];
    int nwh;
} tg_levels_t;

/* parses a .tl text (copied); false only without memory */
bool tg_levels_parse(tg_levels_t *s, const char *text, size_t len);
void tg_levels_free(tg_levels_t *s);
const char *tg_level_text(const char *const *by_lang, int lang);

/* ------------------------------------------------------------------- world */

enum {
    TG_EV_MOVE,         /* x0,y0 -> x1,y1 (tiles, or steps on the paper) */
    TG_EV_TURN,         /* h0 -> h1 (degrees, 0 = north, clockwise) */
    TG_EV_EAT,          /* at x0,y0 */
    TG_EV_CHEW,         /* eat with no lettuce: it chews grass */
    TG_EV_SAY,          /* val, or str */
    TG_EV_PEN,          /* pen = up or down */
    TG_EV_COLOR,        /* color */
    TG_EV_SENSE,        /* a = the sensor, val = the answer */
    TG_EV_KEY,          /* took the key at x1,y1 */
    TG_EV_DOOR,         /* opened the door at x1,y1 */
    TG_EV_BUMP,         /* tried x1,y1; a = the reason */
    TG_EV_SPLASH,       /* walked into the water at x1,y1 */
    TG_EV_WIN,
    TG_EV_FAIL,         /* a = the reason */
};

typedef struct {
    float x0, y0, x1, y1;
    float h0, h1;
    float val;
    const char *str;
    int16_t line;
    uint8_t kind, a, pen, color;
} tg_ev_t;

enum {
    TG_ST_RUN, TG_ST_WIN, TG_ST_FAIL,
};

enum {                  /* why a run failed */
    TG_F_NONE,
    TG_F_ROCK, TG_F_EDGE, TG_F_WATER, TG_F_DOOR,
    TG_F_NOT_YET,       /* an order the level does not have yet (msg: its name) */
    TG_F_ARGS,          /* wrong number of things to an order */
    TG_F_TURN90,        /* the board only turns by 90 */
    TG_F_WHOLE,         /* the board only walks whole tiles */
    TG_F_OFF_PAPER,
    TG_F_NOT_HERE,      /* an order that makes no sense in this world */
    TG_F_SHORT,         /* ended away from the goal (arg: the tiles left) */
    TG_F_LETTUCE_LEFT,  /* arg: how many */
    TG_F_SAID_WRONG,    /* said nothing or something else */
    TG_F_PASSED,        /* hard mode: went over the goal and on */
    TG_F_DRAWING,       /* the drawing is not the figure */
    TG_F_LOOP,          /* too many actions or steps */
    TG_F_SYNTAX,        /* msg */
    TG_F_RUNTIME,       /* msg */
};

typedef struct tg_world {
    const tg_level_t *lv;
    int variant;
    bool hard, paper;
    int w, h;
    char t[TG_MAXH][TG_MAXW];
    int tx, ty, dir, keys, lettuce_total;
    float x, y, hd;
    bool pen;
    uint8_t color;
    bool said;
    float said_num;
    bool passed_goal;
    int actions, line;
    int state, reason, fail_line, arg;
    char msg[120];
    tg_ev_t *ev;
    int nev, cap;
} tg_world_t;

void tg_world_init(tg_world_t *w, const tg_level_t *lv, int variant, bool hard, bool paper,
                   tg_ev_t *ev, int cap);
/* one order from any language. Returns false when the run has to stop
 * (won, failed); a sensor's answer in *out. */
bool tg_world_call(tg_world_t *w, int order, const float *num, const char *const *str, int nargs,
                   int line, bool *out);
void tg_world_finish(tg_world_t *w);        /* the program ended */
void tg_world_fail(tg_world_t *w, int reason, int line, const char *msg);
int  tg_world_steps_to_goal(const tg_world_t *w);
/* the drawing of a finished paper run against the level's figure */
bool tg_world_same_drawing(const tg_world_t *drawn, const tg_world_t *figure);

/* -------------------------------------------------------------------- Logo */

typedef struct tg_node tg_node_t;

enum {                  /* statements */
    TG_N_CALL, TG_N_PCALL, TG_N_REPEAT, TG_N_IF, TG_N_WHILE, TG_N_SET, TG_N_TO,
    TG_N_STOP, TG_N_OUTPUT,
    /* expressions */
    TG_E_NUM, TG_E_STR, TG_E_BOOL, TG_E_VAR, TG_E_SENSE, TG_E_FCALL, TG_E_NEG, TG_E_NOT,
    TG_E_AND, TG_E_OR, TG_E_BIN, TG_E_CMP,
    TG_N_PARAM,
};

enum { TG_OP_ADD, TG_OP_SUB, TG_OP_MUL, TG_OP_DIV, TG_OP_LT, TG_OP_GT, TG_OP_EQ, TG_OP_LE, TG_OP_GE,
       TG_OP_NE };

struct tg_node {
    uint8_t kind, op;       /* op: the order, the operator */
    int16_t line;
    int16_t name;           /* a name's index (variables, procedures, strings) */
    float num;
    tg_node_t *a, *b, *c;   /* arguments or operands, the body, the else */
    tg_node_t *next;        /* the next statement, argument or parameter */
};

#define TG_MAX_NAMES 96
#define TG_NAME_LEN  32

typedef struct {
    tg_node_t *node;        /* the arena */
    int nnode, cap;
    char name[TG_MAX_NAMES][TG_NAME_LEN];
    int nname;
    tg_node_t *prog;        /* the statements */
    int err_line;
    char err[120];
} tg_prog_t;

bool tg_prog_init(tg_prog_t *p, int cap);
void tg_prog_free(tg_prog_t *p);
/* parses Logo; false with err and err_line set */
bool tg_logo_parse(tg_prog_t *p, const char *src);
/* runs a parsed program in the world; the world says how it ended */
void tg_logo_run(tg_prog_t *p, tg_world_t *w);
int  tg_logo_pieces(const tg_prog_t *p);

/* Lua and Python counted on their tokens, the same rule (DESIGN.md, 4) */
int tg_text_pieces(const char *src, bool python);

/* the program again as text: Logo (lang TG_ES/TG_EN), Lua or Python */
enum { TG_GEN_LOGO, TG_GEN_LUA, TG_GEN_PY };
int tg_logo_gen(const tg_prog_t *p, int kind, int lang, char *out, int len);
