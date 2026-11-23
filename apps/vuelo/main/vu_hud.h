/*
 * VUELO - what is drawn over the flight, inside the frame: the speed and
 * the height, the stick under the thumb, the throttle, the buttons, and the
 * numbers of the measurement. A 5 x 7 pixel font with a dark edge so it
 * reads over any ground. Sizes are in units of the frame's height / 240.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { VH_BTN_WORLD = 0, VH_BTN_VIEW, VH_BTN_AUTO, VH_BTN_RES, VH_BTNS };

typedef struct {
    float speed_kmh, alt_m, agl_m;
    float thr;                  /* 0..1 */
    bool  stick_on;             /* a thumb on the stick */
    float stick_bx, stick_by;   /* its base, frame pixels */
    float stick_x, stick_y;     /* -1..1 */
    bool  stall, crashed, autopilot;
    int   fps10;                /* 0: not shown */
    int   res_w, res_h;
    uint8_t pressed;            /* buttons held, a bit each */
    const char *world;          /* its name, upper case, ASCII */
} vh_state_t;

typedef struct {
    int W, H;
    float u;                    /* pixels a unit */
    /* the touch zones, frame pixels */
    int btn_x[VH_BTNS], btn_y, btn_s;
    int thr_x0, thr_x1, thr_y0, thr_y1;
    float stick_r;              /* how far the knob goes */
    float stick_home_x, stick_home_y;
} vh_layout_t;

void vh_layout(vh_layout_t *L, int W, int H);
void vh_draw(const vh_layout_t *L, const vh_state_t *s, uint16_t *out);
/* A line of text, scale s (1, 2...), colour 565, with an edge; returns its width */
int  vh_text(uint16_t *out, int W, int H, int x, int y, int s, const char *txt, uint16_t col);
int  vh_text_w(const char *txt, int s);
