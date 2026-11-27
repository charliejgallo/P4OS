/*
 * P4OS - Marcador: what every file of the app shares.
 *
 * A game (mr_game_t) is the players, their scores and the history of every
 * change; it does not know how it is drawn. Each kind of scorekeeper is a
 * scene (mr_scene_ops_t): it draws its object (the felt with matches, the
 * beans on the table, the hand tally counter...), reads the fingers that land
 * on it and asks the model for points. The model answers by calling the
 * scene's sync(), and the scene moves its pieces to match, so undoing, a
 * keypad entry or a game loaded from the card all look the same to it.
 *
 *   marcador.c   life cycle, the list of games, the bar, the panels
 *   mr_model.c   rules, history, winner, saving to the card
 *   mr_px.c      pixels: ARGB sprites, RGB565 backdrops, noise, strokes
 *   mr_snd.c     every sound, synthesised when the app opens
 *   mr_<scene>.c one per scorekeeper
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

/* in the symbol table, declared only in LVGL's private headers */
void lv_image_cache_drop(const void *src);

#define MR_TAG      "marcador"
#define MR_MAXP     6
#define MR_NAME     24
#define MR_HIST     600
#define MR_FRAME_MS 16
#define MR_FINGERS  5

/* --- kinds of scorekeeper ------------------------------------------------- */

typedef enum {
    MR_T_PANO,      /* green felt with matches, squares of five            */
    MR_T_POROTOS,   /* beans dragged from a heap on a wooden table          */
    MR_T_CUENTA,    /* hand tally counters                                  */
    MR_T_ESTADIO,   /* the stadium board: lamps, clock, periods, horn       */
    MR_T_CARTEL,    /* the flip score card of table tennis and padel        */
    MR_T_ABACO,     /* a wooden abacus, one frame per player                */
    MR_T_TIZA,      /* chalk on a slate: tally marks or running totals      */
    MR_T_N
} mr_type_t;

/* opt[] of each kind */
enum { MR_PANO_SPLIT = 0 };                               /* malas / buenas  */
enum { MR_POR_KIND = 0, MR_POR_TOTAL = 1 };               /* bean, how many  */
enum { MR_EST_SPORT = 0, MR_EST_MIN = 1, MR_EST_PER = 2, MR_EST_DOWN = 3 };
enum { MR_CAR_MODE = 0, MR_CAR_BEST = 1 };                /* rules, sets     */
enum { MR_TIZA_MODE = 0, MR_TIZA_START = 1, MR_TIZA_SUB = 2 };

enum { MR_CAR_FREE, MR_CAR_PING, MR_CAR_PADEL };
enum { MR_TIZA_TALLY, MR_TIZA_TOTALS };

/* --- the game ------------------------------------------------------------- */

typedef struct {
    char     name[MR_NAME];
    uint32_t color;
    int32_t  score;
    int16_t  aux[2];        /* games and sets, where the kind has them      */
    int16_t  wins;          /* games won with these players (rematches)     */
} mr_player_t;

typedef struct {
    int8_t   who;           /* -1: the whole table (reset, clock)          */
    uint8_t  kind;          /* MR_H_*                                       */
    int16_t  delta;
    uint32_t t;             /* seconds since the game started               */
    int32_t  before[MR_MAXP];
    int16_t  aux_before[MR_MAXP][2];
    uint16_t group;         /* entries of one gesture undo together        */
} mr_hist_t;

enum { MR_H_POINTS, MR_H_RESET, MR_H_MOVE };

typedef struct {
    uint32_t    id;         /* its file: g<id>.txt                          */
    uint8_t     type;
    char        name[40];
    uint8_t     np;
    mr_player_t p[MR_MAXP];
    int32_t     target;     /* 0 = no target                                */
    int32_t     opt[8];
    uint32_t    created;    /* unix time, 0 if the clock was not set        */
    uint32_t    updated;
    uint32_t    played_s;   /* seconds of play, for the history's times     */

    /* the stadium's clock */
    int32_t     clock_ms;   /* elapsed in this period                       */
    uint8_t     period;
    bool        running;

    int8_t      winner;     /* -1 none                                      */
    bool        announced;

    mr_hist_t  *hist;       /* PSRAM, MR_HIST                               */
    int         nhist;
    uint16_t    group;
} mr_game_t;

/* --- pixels ---------------------------------------------------------------- */

typedef struct {
    int             w, h;
    uint32_t       *px;     /* ARGB8888, straight alpha, PSRAM              */
    lv_image_dsc_t  dsc;
} mr_img_t;

typedef struct {
    int       w, h;
    uint16_t *px;           /* RGB565, PSRAM                                */
} mr_cv_t;

/* --- a scene ----------------------------------------------------------------- */

typedef struct mr_scene mr_scene_t;

typedef struct {
    void (*build)(mr_scene_t *s);       /* objects under s->obj, s->w x s->h */
    void (*free)(mr_scene_t *s);        /* the memory; LVGL objects go alone */
    void (*sync)(mr_scene_t *s, int why, int who);
    void (*frame)(mr_scene_t *s, float dt);
    void (*down)(mr_scene_t *s, int f, float x, float y);
    void (*move)(mr_scene_t *s, int f, float x, float y);
    void (*up)(mr_scene_t *s, int f, float x, float y);
    void (*tick)(mr_scene_t *s);        /* 5 Hz                              */
} mr_scene_ops_t;

enum { MR_SYNC_LOAD, MR_SYNC_ADD, MR_SYNC_UNDO, MR_SYNC_RESET, MR_SYNC_CLOCK, MR_SYNC_NAMES };

struct mr_scene {
    const mr_scene_ops_t *ops;
    mr_game_t *g;
    lv_obj_t  *obj;         /* fills the app's root, not clickable          */
    int        w, h;
    int        top;         /* the bar's height: keep fingers' things below */
    bool       land;
    void      *st;          /* the scene's own state                        */
};

/* a finger, as the scenes see it */
typedef struct {
    uint8_t  id;
    float    x0, y0, x, y;
    uint32_t t0;
    bool     moved;
} mr_finger_t;

/* --- the app ------------------------------------------------------------------ */

typedef struct {
    lv_obj_t   *root;
    int         w, h;
    bool        land;
    lv_timer_t *timer;
    uint32_t    last_ms;
    uint32_t    tick_ms;

    int         screen;     /* MR_SCR_*                                     */
    mr_game_t  *g;          /* the open game (PSRAM)                        */
    mr_scene_t  scene;
    bool        scene_on;

    lv_obj_t   *bar, *bar_title, *bar_undo;
    lv_obj_t   *sheet;      /* a panel on top: fingers do not reach the scene */
    lv_obj_t   *kb;

    mr_finger_t fing[MR_FINGERS];
    uint8_t     stale[MR_FINGERS];
    bool        covered;

    bool        sound;
    uint32_t    save_due;   /* uptime ms; 0 = nothing to save              */

    /* the new-game form */
    mr_game_t  *draft;
    bool        draft_edit; /* editing the open game's players, not a new one */
} mr_app_t;

enum { MR_SCR_LIST, MR_SCR_NEW, MR_SCR_SETUP, MR_SCR_GAME };

extern mr_app_t *mr_A;

/* --- marcador.c ------------------------------------------------------------- */

void mr_scene_rebuild(void);
void mr_ui_winner(int who);
void mr_ui_bar_refresh(void);
void mr_ui_keypad(int who);
typedef void (*mr_num_cb_t)(int value, int who);
void mr_ui_number(const char *title, int value, int who, mr_num_cb_t cb);
void mr_thumb(mr_img_t *im, int type);     /* mr_thumb.c: the little picture of a kind */
void mr_toast(const char *msg);
lv_obj_t *mr_pill(lv_obj_t *parent, const char *txt, int w, int h, uint32_t bg, lv_event_cb_t cb, void *ud);
const char *mr_type_name(int type);
uint32_t mr_ms(void);

/* --- mr_model.c ------------------------------------------------------------- */

mr_game_t *mr_game_new(int type);
void       mr_game_free(mr_game_t *g);
void       mr_game_defaults(mr_game_t *g, int type, int np);
int        mr_type_minp(int type);
int        mr_type_maxp(int type);
void       mr_add(mr_game_t *g, int who, int delta);         /* + sync, save */
void       mr_point(mr_game_t *g, int who);                  /* the kind's rules */
void       mr_move(mr_game_t *g, int from, int to);          /* beans: one changes side */
bool       mr_undo(mr_game_t *g);
void       mr_reset(mr_game_t *g, bool rematch);
void       mr_hist_note(mr_game_t *g, int who, int kind, int delta);
void       mr_check_winner(mr_game_t *g);
void       mr_set_dirty(void);
bool       mr_save(mr_game_t *g);
bool       mr_load(mr_game_t *g, uint32_t id);
void       mr_delete(uint32_t id);
int        mr_list(uint32_t *ids, int max);                  /* newest first */
uint32_t   mr_new_id(void);
uint32_t   mr_now(void);                                     /* unix or 0 */
const char *mr_tennis_pts(const mr_game_t *g, int who);
extern const uint32_t mr_palette[10];

/* --- mr_px.c ------------------------------------------------------------------ */

bool  mr_img_new(mr_img_t *im, int w, int h);
void  mr_img_free(mr_img_t *im);
void  mr_img_clear(mr_img_t *im);
void  mr_img_over(mr_img_t *im, int x, int y, uint32_t rgb, float a);
bool  mr_cv_new(mr_cv_t *cv, int w, int h);
void  mr_cv_free(mr_cv_t *cv);
void  mr_cv_mix(mr_cv_t *cv, int x, int y, uint32_t rgb, float a);
void  mr_cv_put_img(mr_cv_t *cv, const mr_img_t *im, int x, int y, float a);
void  mr_cv_copy(mr_cv_t *dst, const mr_cv_t *src);
lv_obj_t *mr_cv_obj(lv_obj_t *parent, mr_cv_t *cv);

uint32_t mr_hash(uint32_t x);
float    mr_rnd(uint32_t *seed);                 /* 0..1 */
float    mr_hashf(int x, int y, uint32_t salt);  /* 0..1 */
float    mr_vnoise(float x, float y, uint32_t salt);
float    mr_fbm(float x, float y, int oct, uint32_t salt);

/* a grid of low-resolution noise sampled with bilinear interpolation */
typedef struct {
    int    gw, gh, step;
    float *v;
} mr_field_t;
bool  mr_field_new(mr_field_t *f, int w, int h, int step, float scale, int oct, uint32_t salt);
float mr_field_at(const mr_field_t *f, int x, int y);
void  mr_field_free(mr_field_t *f);

uint32_t mr_rgb_mix(uint32_t a, uint32_t b, float t);
uint32_t mr_rgb_scale(uint32_t c, float k);
static inline uint16_t mr_565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}
static inline uint32_t mr_888(uint16_t c)
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}
static inline float mr_minf(float a, float b) { return a < b ? a : b; }
static inline float mr_maxf(float a, float b) { return a > b ? a : b; }
static inline float mr_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float mr_smooth(float e0, float e1, float x)
{
    float t = mr_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* Strokes: the digits and a few signs as centre lines in a unit box (x 0..1
 * left to right, y 0..1 top to bottom), so every scene draws them its way:
 * printed on a wheel, painted on a card, written in chalk. */
#define MR_STROKE_PTS 40
typedef struct {
    int   n;
    float x[MR_STROKE_PTS], y[MR_STROKE_PTS];
} mr_stroke_t;
int   mr_glyph(int ch, const mr_stroke_t **out);    /* strokes of '0'..'9', '-', '+', '/' */
float mr_seg_dist(float px, float py, float ax, float ay, float bx, float by);
/* draws a glyph into an image: box at (x, y) of w x h, line width lw */
void  mr_glyph_img(mr_img_t *im, int ch, float x, float y, float w, float h, float lw, uint32_t rgb);
void  mr_text_img(mr_img_t *im, const char *s, float x, float y, float h, float lw, uint32_t rgb);
float mr_text_w(const char *s, float h);

void  mr_img_disc(mr_img_t *im, float cx, float cy, float r, uint32_t rgb, float a);
void  mr_img_capsule(mr_img_t *im, float ax, float ay, float bx, float by, float r, uint32_t rgb, float a);
void  mr_img_rrect(mr_img_t *im, float x, float y, float w, float h, float rad, uint32_t rgb, float a);
float mr_rrect_sd(float px, float py, float x, float y, float w, float h, float rad);
void  mr_img_vgrad(mr_img_t *im, uint32_t top, uint32_t bot);

void  mr_img_blur(mr_img_t *im, int blur);    /* the alpha, colour kept */
/* soft drop shadow of an image's alpha, into the same image, under it */
void  mr_img_shadow(mr_img_t *im, int dx, int dy, int blur, float a);

/* --- mr_snd.c ---------------------------------------------------------------- */

enum {
    MR_S_MATCH_ON, MR_S_MATCH_OFF, MR_S_BEAN, MR_S_BEAN_SOFT, MR_S_BEAN_GRAB, MR_S_CLICK, MR_S_CLICK_UP,
    MR_S_RESET, MR_S_HORN, MR_S_BUZZ, MR_S_RELAY, MR_S_FLIP, MR_S_BEAD, MR_S_CHALK, MR_S_CHALK_TAP,
    MR_S_ERASE, MR_S_TAP, MR_S_WIN, MR_S_UNDO, MR_S_BOX, MR_S_N
};
void mr_snd_open(void);
void mr_snd_close(void);
void mr_snd_play(int snd, float gain, float pitch);
void mr_snd_pump(void);

/* --- the scenes ------------------------------------------------------------- */

extern const mr_scene_ops_t mr_pano_ops, mr_porotos_ops, mr_cuenta_ops, mr_estadio_ops, mr_cartel_ops,
    mr_abaco_ops, mr_tiza_ops;

/* shared little things the scenes use */
lv_obj_t *mr_img_obj(lv_obj_t *parent, mr_img_t *im);
lv_obj_t *mr_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *txt);
void      mr_obj_plain(lv_obj_t *o);   /* no style, no click, no scroll */

/* Buttons inside a scene are read from the raw fingers like the rest of it
 * (two players can press theirs at once): a styled object and a rectangle
 * in the scene's coordinates. Forgotten when the scene is rebuilt. */
typedef void (*mr_rbtn_cb_t)(mr_scene_t *s, int arg);
lv_obj_t *mr_rbtn(mr_scene_t *s, int x, int y, int w, int h, const char *txt, uint32_t bg, uint32_t fg,
                  mr_rbtn_cb_t cb, int arg);
void      mr_rbtn_move(lv_obj_t *o, int x, int y);
void      mr_rbtn_hide(lv_obj_t *o, bool hidden);
void     *mr_alloc(size_t n);           /* PSRAM, zeroed */
void      mr_free(void *p);
