/*
 * LX_API - everything a Lua script can reach
 *
 * The sandbox is this file. There is no 'io', no 'os' and no 'package' -their
 * sources are not even compiled- so the only door out of the interpreter is
 * the table this opens, and every function behind that door has to assume its
 * arguments come from somebody who does not know what a buffer is.
 *
 * Three rules it keeps:
 *   - Numbers are taken with luaL_checkinteger, which RAISES a Lua error on
 *     anything else. A script that passes a table where a colour goes gets a
 *     message with a line number, which is the whole point of scripting.
 *   - Coordinates are clamped before they reach lx_pixel. The primitives clip
 *     already, but they clip in int arithmetic: a width of two thousand
 *     million is not a clipped rectangle, it is an overflow.
 *   - P4OS: nothing behind the door touches LVGL or the hardware. The script
 *     runs on a thread of its own (lua_app.c says why), so what it reads from
 *     outside -the finger, the gamepad, the time- is a snapshot the app wrote before
 *     handing it the frame, and what it asks of the outside -a beep- is
 *     queued for the app to do from the LVGL task afterwards.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lua.h"
#include "lx_pixel.h"

/* A tone asked for by the script. Queued, not played: the script runs on the
 * app's own thread, and the app plays them from the LVGL task once the frame
 * is handed back (lua_app.c). */
#define LX_MAX_BEEPS    4

typedef struct {
    uint16_t hz, ms;
} lx_beep_t;

typedef struct {
    lx_buf_t *buf;              /* where the script draws        */
    int16_t   w, h;             /* its size: aos.W, aos.H        */

    /* What the script touched this frame, in its own coordinates. Every
     * primitive marks its own bounding box, so a script gets this for
     * nothing: it does not call anything and does not know it exists, and
     * the app presents only those rectangles. A script that clears every
     * frame marks everything, which is what used to happen always. */
    lx_dirty_t *dirty;

    /* The frozen background, and the app's hook to freeze it. NULL until the
     * script asks for one; see aos.background() in lx_api.c. */
    bool      (*freeze)(void *app);
    void       *app;

    /* The finger, as the app last saw it. Written by the app before it hands
     * a frame to the script's thread, so the script reads a snapshot that
     * does not move under it. */
    int16_t   touch_x;
    int16_t   touch_y;
    bool      touch_down;
    uint8_t   fingers;          /* 0..2, for aos.fingers()       */
    uint8_t   finger_id[2];
    int16_t   finger_x[2], finger_y[2];
    uint32_t  t0;               /* ms when the script started    */

    /* The USB gamepad, the same way: aos_pad.h's roles held now, the ones
     * that went down since the frame before (gathered by the app between
     * frames, so a short press during a slow one is not lost), the left
     * stick, and whether one is plugged in. For aos.pad(). */
    uint16_t  pad_held;
    uint16_t  pad_pressed;
    int16_t   pad_x, pad_y;
    bool      pad_on;

    lx_beep_t beeps[LX_MAX_BEEPS];
    uint8_t   n_beeps;

    /* What the last frame was spent on, filled in by the app and handed back
     * to the script by aos.stats(). A script that is slow needs to know
     * WHICH of the two is slow: its own maths, or the screen. Without this
     * the author blames the interpreter, which is almost never the one at
     * fault. */
    uint16_t  ms_script;
    uint16_t  ms_screen;
    uint16_t  ms_frame;
    uint16_t  rows;             /* of aos.H, how many changed      */
    uint32_t  px;               /* canvas pixels presented         */
} lx_ctx_t;

/* Creates the global table 'aos' bound to this context. The context lives in
 * the app and must outlive the state. */
void lx_api_open(lua_State *L, lx_ctx_t *ctx);

/* The canvas changed size (the screen turned): aos.W and aos.H again. */
void lx_api_resize(lua_State *L, lx_ctx_t *ctx);
