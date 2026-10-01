/*
 * P4OS - Retro canvas ("lienzo retro"), recipe C of docs/plan/APPS.md
 *
 * The games of AmoledOS draw a small RGB565 canvas (184x224) and upscaled it
 * x2 by hand. Here the OS does that part: the game gets a canvas, draws into
 * it, and says which part changed; the OS scales it by an integer factor
 * (x3 for 184x224 = 552x672), nearest neighbour, centred, with the PPA on the
 * board and in software in the simulator. Around the game the OS draws the
 * on-screen controls the game asked for and reads them with both fingers.
 *
 * The whole contract, with the measurements, is in docs/RETRO.md.
 *
 *     const aos_retro_t *r = aos_retro_begin(root, 184, 224, 0,
 *                                            AOS_RETRO_LR | AOS_RETRO_A | AOS_RETRO_PAUSE);
 *     ... draw into r->px (r->w x r->h, RGB565, stride r->w) ...
 *     aos_retro_present();                 // or aos_retro_present_rect() per dirty rect
 *     uint32_t held = aos_retro_buttons(); // AOS_RETRO_BTN_*
 *
 * One retro canvas at a time (it belongs to the app in front). Everything
 * here runs with the LVGL lock held: from create(), an event or an lv_timer.
 */
#pragma once

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- what the game asks for (flags of aos_retro_begin) ---- */
enum {
    AOS_RETRO_DPAD    = 1u << 0,  /* the four arrows, left thumb                   */
    AOS_RETRO_LR      = 1u << 1,  /* only left / right, big                        */
    AOS_RETRO_A       = 1u << 2,  /* action button A, right thumb                  */
    AOS_RETRO_B       = 1u << 3,  /* second action button                          */
    AOS_RETRO_PAUSE   = 1u << 4,  /* a pause button in the control area            */
    AOS_RETRO_TOUCH   = 1u << 5,  /* the game reads the finger on its canvas       */
    /* A horizontal band in the control area whose x maps to the canvas's
     * column straight above it: a paddle under the thumb that never covers
     * the ball. aos_retro_touch() reports it like a touch on the canvas, with
     * y = the canvas's last row. */
    AOS_RETRO_SLIDER  = 1u << 6,
    /* The controls float over the bottom of the game, translucent, and the
     * canvas takes the biggest factor that fits the whole screen (the wide
     * 240x426 x3 = 720x1278 variant). Without it the canvas takes the biggest
     * factor that leaves room for the controls below it. */
    AOS_RETRO_OVERLAY = 1u << 7,
    /* Canvas centred on the screen instead of at the top (games without
     * controls: the borders split evenly). */
    AOS_RETRO_CENTER  = 1u << 8,
    /* Left and right one per thumb (implies AOS_RETRO_LR): upright, LEFT
     * in the bottom-left corner and RIGHT in the bottom-right one, either
     * side of the pause pill; lying down, one low in each side column.
     * A and B, if asked for too, go above RIGHT. */
    AOS_RETRO_LR_SPLIT = 1u << 9,
};

/* ---- button state (aos_retro_buttons / aos_retro_pressed) ---- */
enum {
    AOS_RETRO_BTN_LEFT  = 1u << 0,
    AOS_RETRO_BTN_RIGHT = 1u << 1,
    AOS_RETRO_BTN_UP    = 1u << 2,
    AOS_RETRO_BTN_DOWN  = 1u << 3,
    AOS_RETRO_BTN_A     = 1u << 4,
    AOS_RETRO_BTN_B     = 1u << 5,
    AOS_RETRO_BTN_PAUSE = 1u << 6,
};

typedef struct {
    uint16_t *px;       /* w*h pixels, RGB565 as LVGL stores it, stride == w  */
    int16_t   w, h;     /* the canvas                                         */
    int16_t   scale;    /* the integer factor it is shown at                  */
    int16_t   x, y;     /* its top-left corner on the screen (root coords)    */
    lv_obj_t *view;     /* the LVGL object showing it: an app's panels go on
                           the root, above it, and LVGL composes them         */
    lv_obj_t *controls; /* the container of the on-screen controls, or NULL   */
} aos_retro_t;

/* Creates the canvas and the controls inside 'root' (the app's root: the
 * service lays it out, black, and owns what it adds). scale 0 = the biggest
 * integer factor that fits (see AOS_RETRO_OVERLAY). The pixels start black.
 * NULL when there is no memory. A second begin() ends the first. */
const aos_retro_t *aos_retro_begin(lv_obj_t *root, int w, int h, int scale, uint32_t flags);

/* Frees everything; px is gone after it. Safe to call twice or with nothing
 * begun; deleting the root also ends it. Call it from the app's destroy(). */
void aos_retro_end(void);

/* The current canvas, or NULL. */
const aos_retro_t *aos_retro_get(void);

/* The canvas changed: all of it, or only this rectangle (canvas pixels). The
 * scale happens when LVGL next refreshes, only over what was marked. */
void aos_retro_present(void);
void aos_retro_present_rect(int x, int y, int w, int h);

/* Buttons held right now, and the ones that went down since the previous
 * call of aos_retro_pressed() (a tap shorter than a frame still counts). */
uint32_t aos_retro_buttons(void);
uint32_t aos_retro_pressed(void);
/* The ones that were let go since the previous call of this function: with
 * pressed() it tells a press-and-release inside one tick apart from a hold. */
uint32_t aos_retro_released(void);

/* A finger on the canvas (AOS_RETRO_TOUCH) or on the slider
 * (AOS_RETRO_SLIDER), in canvas pixels. false = no finger there now; x/y
 * then keep where the last one was (to act on release). A finger on a
 * control (a button's touch target, the d-pad) is that control only, never
 * the canvas finger, even where the controls float over the canvas. */
bool aos_retro_touch(int *x, int *y);
/* A finger that LANDED on the canvas since the previous call (one sliding
 * in from outside, like an edge swipe, is not a tap), with where it came
 * down. A finger landing on a control is no tap, and a thumb held on a
 * control does not stop another finger's tap. Consumes it. */
bool aos_retro_tap(int *x, int *y);
/* Screen (root) coordinates to canvas coordinates, for games that read the
 * touch with their own LVGL objects. false = outside the canvas (x/y are
 * still written, clamped). */
bool aos_retro_to_canvas(int sx, int sy, int *x, int *y);

/* The caption under a button (A, B), e.g. _("Saltar"). NULL = the letter. */
void aos_retro_set_label(uint32_t btn, const char *text);
/* Colour of the screen around the canvas and under the controls, 0xRRGGBB. */
void aos_retro_set_border(uint32_t rgb);
/* Hides the controls (a menu over the game), or shows them again. While
 * hidden, the finger on the canvas is not reported either (touch() false,
 * no taps): it belongs to the menu's buttons. */
void aos_retro_show_controls(bool show);

/* ---- frame pacing ----
 *
 * A fixed tick: step() runs 'fps' times per second of real time (so the game
 * keeps its speed when a frame comes late, up to 4 steps to catch up), then
 * draw() once. Either may be NULL. The timer is an lv_timer: both run with
 * the LVGL lock. A new run() replaces the previous one; end() stops it. */
typedef void (*aos_retro_fn_t)(void *user);
void aos_retro_run(int fps, aos_retro_fn_t step, aos_retro_fn_t draw, void *user);
void aos_retro_stop(void);
/* Pauses the tick (true) without forgetting it; on resuming it does not try
 * to catch up the time it was paused. */
void aos_retro_pause(bool paused);

typedef struct {
    uint32_t frames;        /* presents since begin                           */
    uint16_t fps10;         /* draws per second x10, smoothed                 */
    uint32_t scale_us;      /* cost of scaling the last refresh, microseconds */
    uint32_t scale_us_avg;  /* the same, smoothed over the last ~16 frames    */
    uint32_t scale_us_max;
    uint32_t px_last;       /* screen pixels the last refresh scaled          */
    uint32_t refresh_us_avg;/* LVGL's whole refresh (render + flush), smoothed */
    uint8_t  hw;            /* 1 = the last refresh went through the HAL's
                               scaler (the PPA on the board)                  */
} aos_retro_stats_t;
void aos_retro_stats(aos_retro_stats_t *out);

#ifdef __cplusplus
}
#endif
