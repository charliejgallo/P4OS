/*
 * PLANEO - what is drawn over the flight, inside the frame: the speed and
 * the height, the mission's state and its clock, the banners, the arrow to
 * the next target, the stick under the thumb, the throttle and the two
 * buttons (pause, camera). The words come from vu_text (in the language of
 * the moment); a 5 x 7 pixel font is left for the measurement's line.
 * Sizes are in units of the frame's height / 240.
 */
#pragma once

#include "vu_text.h"

#include <stdbool.h>
#include <stdint.h>

enum { VH_BTN_PAUSE = 0, VH_BTN_VIEW, VH_BTNS };

typedef struct {
    float speed_kmh, alt_m, agl_m;
    float thr;                  /* 0..1 */
    bool  stick_on;             /* a thumb on the stick */
    float stick_bx, stick_by;   /* its base, frame pixels */
    float stick_x, stick_y;     /* -1..1 */
    float rud;                  /* the pedals, -1..1 */
    bool  rud_on;               /* a thumb on them */
    bool  stall, crashed;
    uint8_t pressed;            /* buttons held, a bit each */
    /* the mission */
    int   kind;                 /* VG_*, VG_FREE */
    int   next, npt;
    float clock;                /* seconds (negative: the countdown) */
    bool  done, carrying;
    int   place;                /* the race: 1 or 2, 0 none */
    int   banner;               /* a VT_W_ word, -1 none */
    float banner_t;
    bool  target, target_on;
    float tx, ty, tdist, tdx, tdy;
    /* the measurement */
    int   fps10;                /* 0: not shown */
    int   res_w, res_h;
} vh_state_t;

typedef struct {
    int W, H;
    float u;                    /* pixels a unit */
    /* the touch zones, frame pixels */
    int btn_x[VH_BTNS], btn_y, btn_s;
    int thr_x0, thr_x1, thr_y0, thr_y1;
    int rud_x0, rud_x1, rud_y;  /* the pedals' track, its middle row */
    float stick_r;              /* how far the knob goes */
    float stick_home_x, stick_home_y;
} vh_layout_t;

void vh_layout(vh_layout_t *L, int W, int H);
void vh_draw(const vh_layout_t *L, const vh_state_t *s, const vt_set_t *t, uint16_t *out);
/* A line of the pixel font (upper case ASCII), scale s, with an edge */
int  vh_text(uint16_t *out, int W, int H, int x, int y, int s, const char *txt, uint16_t col);
int  vh_text_w(const char *txt, int s);
