/*
 * TURBO - what is written over the road: the clock, the speed, the wheel,
 * the pedals, the progress bar and the banners
 *
 * The frame goes straight to the panel, so LVGL cannot draw on top of it:
 * everything here is IN the frame. Text is rendered once by LVGL at start
 * (turbo.c, in the LVGL task) into alpha masks, then baked here into sprites
 * with their colour, a vertical gradient and a dark outline, the look of an
 * arcade clock. Drawing a sprite per glyph is then one pass.
 *
 * P4OS: touch only. The watch steered by tilting and had two pedals; here
 * the left thumb drives a wheel (drag it sideways) or two arrows, the right
 * one the pedals. Where everything goes depends on the screen's shape
 * (tb_hud_layout): upright the controls fill the bottom under the car and
 * the clock the sky; lying down the wheel and the pedals take the two
 * lower corners.
 */
#pragma once

#include "tb_game.h"
#include "tb_gfx.h"

#include <stdbool.h>
#include <stdint.h>

/* the words, rendered from _() by the app */
enum {
    TX_TIME = 0,        /* "TIEMPO" over the clock               */
    TX_KMH,
    TX_CHECKPOINT,
    TX_EXTRA,           /* "TIEMPO EXTRA"                        */
    TX_GO,
    TX_FINISH,
    TX_TIMEUP,
    TX_HURRY,           /* the last seconds                      */
    TX_GEAR,            /* "MARCHA"                               */
    TX_N
};

/* glyphs: 0-9 then ':' '.' '+' '-' */
#define TB_GLYPHS   14

/* how the car is steered */
enum { CTL_WHEEL = 0, CTL_ARROWS, CTL_N };

typedef struct {
    uint8_t *a;
    int16_t  w, h;
} tb_mask_t;

typedef struct { int16_t x0, y0, x1, y1; } tb_rect_t;

static inline bool tb_in(const tb_rect_t *r, int x, int y)
{
    return x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1;
}

/* where everything goes, for one screen shape */
typedef struct {
    int w, h;
    int clock_y, word_y, elapsed_y, bar_y, bar_x0, bar_x1;
    int banner_y;
    int speed_x, speed_y;           /* the speed's centre and top            */
    tb_rect_t pause;                /* the button and its touch zone          */
    int pause_x, pause_y;
    int wheel_cx, wheel_cy;         /* the wheel                              */
    tb_rect_t steer;                /* where a finger steers (wheel mode)     */
    tb_rect_t left, right;          /* the arrows (arrow mode), touch = drawn */
    tb_rect_t brake, gas;           /* touch zones                            */
    int brake_x, brake_y, gas_x, gas_y;   /* where the pedals are drawn       */
} tb_hud_lay_t;

typedef struct {
    tb_sprite_t big[TB_GLYPHS];     /* the clock and the countdown (96 px, gradient) */
    tb_sprite_t mid[TB_GLYPHS];     /* the speed (64 px, white)                      */
    tb_sprite_t sml[TB_GLYPHS];     /* the elapsed time, the gear (36 px)            */
    tb_sprite_t word[TX_N];
    tb_sprite_t pedal[2][2];        /* [brake, gas][up, pressed]                     */
    tb_sprite_t arrow[2][2];        /* [left, right][up, pressed]                    */
    tb_sprite_t wheel;              /* the rim and the hub; the spokes turn          */
    tb_sprite_t pause;
    tb_hud_lay_t lay;
    bool        ok;
} tb_hud_t;

/* what the HUD shows beyond the game's own state */
typedef struct {
    bool   gas, brake;              /* the pedals as pressed                */
    bool   left, right;             /* the arrows as pressed                */
    float  steer;                   /* the wheel's turn, -1..1              */
    int    ctl;                     /* CTL_*                                */
    bool   controls;                /* draw the wheel and the pedals        */
    int    banner;                  /* TX_* or -1                           */
    float  banner_t;                /* seconds left for it                  */
    float  added;                   /* seconds a checkpoint gave            */
    float  split_diff;              /* against the rival's split, s         */
    bool   split_show;
    bool   rival;                   /* show the rival on the bar            */
    float  rival_prog;              /* 0..1                                 */
    bool   low_time;
    /* the texts of this frame (tb_hud_prepare), so the bands do not format them */
    char   t_clock[12], t_elapsed[16], t_speed[12], t_gear[12], t_extra[16], t_final[16];
    int    count;                   /* the countdown's number, 0 none        */
    bool   blink;
    float  progress;
} tb_hud_state_t;

/* bakes a mask into a sprite: gradient from top to bottom colour, outline px */
bool tb_hud_bake(tb_sprite_t *out, const tb_mask_t *m, uint32_t top, uint32_t bottom, int outline);
/* the pedals, the arrows, the wheel and the pause button */
void tb_hud_make_controls(tb_hud_t *h);
void tb_hud_free(tb_hud_t *h);
uint32_t tb_hud_bytes(const tb_hud_t *h);       /* PSRAM the baked pieces take */
/* where everything goes on a w x h screen (tb_view) */
void tb_hud_layout(tb_hud_t *h, int w, int hgt, int car_y);

/* once a frame, before the bands: the texts */
void tb_hud_prepare(tb_hud_state_t *st, const tb_game_t *g);
/* the HUD over a frame of the race (or a band of it: im's clip) */
void tb_hud_draw(const tb_hud_t *h, tb_img_t *im, const tb_game_t *g, const tb_hud_state_t *st);
/* the worker calls this with each step's events: banners and their clocks */
void tb_hud_events(tb_hud_state_t *st, const tb_game_t *g, uint32_t ev, float dt);
