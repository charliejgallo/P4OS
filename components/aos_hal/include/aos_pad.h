/*
 * P4OS - A USB gamepad for the games, the same mapping in all of them.
 *
 * Header only: it reads aos_hal_hid_gamepad_get(), which the firmware
 * exports since 0.9.0, so an app that uses it needs no new firmware.
 *
 * Cheap pads number their buttons differently, so each role takes two:
 *     A      buttons 1 or 2        B      buttons 3 or 4
 *     L      buttons 5 or 7        R      buttons 6 or 8
 *     START  buttons 9 or 10 (pause, and "go" on a title screen)
 * and the directions come from the hat and from the left stick past half
 * way (aos_hal_hid_gamepad_dpad). Every pad plugged in counts, OR'ed
 * together. The retro canvas (aos_retro.c) maps its buttons the same way.
 *
 *     static aos_pad_t pad;
 *     ...every frame or tick:
 *     aos_pad_update(&pad, lv_tick_get());
 *     if (aos_pad_held(&pad, AOS_PAD_LEFT)) x--;
 *     if (aos_pad_pressed(&pad, AOS_PAD_A)) jump();
 *     if (aos_pad_repeat(&pad, AOS_PAD_DOWN)) cursor++;   (menus, grids)
 *
 * pad.x/pad.y are the left stick for analog use (aiming, steering), but
 * only once it has moved part way: a d-pad that reports itself as axes
 * keeps them at 0 and is read as directions.
 *
 * aos_pad_update() costs a mutex and a copy per pad: call it once a frame,
 * not once per test. Nothing to free.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "aos_hal.h"

enum {
    AOS_PAD_UP    = 1u << 0,
    AOS_PAD_DOWN  = 1u << 1,
    AOS_PAD_LEFT  = 1u << 2,
    AOS_PAD_RIGHT = 1u << 3,
    AOS_PAD_A     = 1u << 4,
    AOS_PAD_B     = 1u << 5,
    AOS_PAD_L     = 1u << 6,
    AOS_PAD_R     = 1u << 7,
    AOS_PAD_START = 1u << 8,
    AOS_PAD_DIRS  = AOS_PAD_UP | AOS_PAD_DOWN | AOS_PAD_LEFT | AOS_PAD_RIGHT,
};

/* how long a held direction waits before repeating, and then how often */
#define AOS_PAD_REPEAT_DELAY_MS 320
#define AOS_PAD_REPEAT_RATE_MS  110

typedef struct {
    uint32_t held;          /* AOS_PAD_* down now */
    uint32_t pressed;       /* went down in the last update */
    uint32_t released;      /* came up in the last update */
    uint32_t repeat;        /* pressed, plus held directions repeating */
    int16_t  x, y;          /* the left stick of the first pad, -32767..32767 (0 without one,
                             * and 0 until it is known to be analog: see aos_pad_update) */
    bool     connected;     /* some pad is plugged in */
    bool     analog;        /* its stick has been seen part way */
    uint32_t rep_next;      /* when the held directions repeat next */
} aos_pad_t;

/* The roles held right now on every pad, without edges: for code that only
 * polls (and so cannot keep an aos_pad_t). */
static inline uint32_t aos_pad_bits(int16_t *sx, int16_t *sy, bool *connected)
{
    uint32_t b = 0;
    bool any = false, stick = false;
    if (sx) *sx = 0;
    if (sy) *sy = 0;
    for (int i = 0; i < AOS_GAMEPAD_MAX; i++) {
        aos_gamepad_t p;
        if (!aos_hal_hid_gamepad_get(i, &p)) continue;
        any = true;
        uint8_t d = aos_hal_hid_gamepad_dpad(&p);
        if (d & AOS_DPAD_UP) b |= AOS_PAD_UP;
        if (d & AOS_DPAD_DOWN) b |= AOS_PAD_DOWN;
        if (d & AOS_DPAD_LEFT) b |= AOS_PAD_LEFT;
        if (d & AOS_DPAD_RIGHT) b |= AOS_PAD_RIGHT;
        if (p.buttons & 0x003) b |= AOS_PAD_A;
        if (p.buttons & 0x00C) b |= AOS_PAD_B;
        if (p.buttons & 0x050) b |= AOS_PAD_L;
        if (p.buttons & 0x0A0) b |= AOS_PAD_R;
        if (p.buttons & 0x300) b |= AOS_PAD_START;
        if (!stick && (p.axes & 3) == 3) {
            stick = true;
            if (sx) *sx = p.axis[0];
            if (sy) *sy = p.axis[1];
        }
    }
    if (connected) *connected = any;
    return b;
}

static inline void aos_pad_update(aos_pad_t *p, uint32_t now_ms)
{
    uint32_t prev = p->held;
    p->held = aos_pad_bits(&p->x, &p->y, &p->connected);
    /* Many cheap pads report their d-pad as the X/Y axes, all or nothing,
     * with no hat. Read as a stick, that is full deflection at a touch (a
     * steering wheel at full lock). So x/y stay 0 until an axis has been
     * seen part way, which only a real stick does; the directions come
     * from those axes either way. */
    if (!p->connected) p->analog = false;
    if (!p->analog) {
        int ax = p->x < 0 ? -p->x : p->x, ay = p->y < 0 ? -p->y : p->y;
        if ((ax > 8000 && ax < 28000) || (ay > 8000 && ay < 28000)) p->analog = true;
        else p->x = p->y = 0;
    }
    p->pressed = p->held & ~prev;
    p->released = prev & ~p->held;
    p->repeat = p->pressed;
    uint32_t dirs = p->held & AOS_PAD_DIRS;
    if (p->pressed & AOS_PAD_DIRS) {
        p->rep_next = now_ms + AOS_PAD_REPEAT_DELAY_MS;
    } else if (dirs && (int32_t)(now_ms - p->rep_next) >= 0) {
        p->repeat |= dirs;
        p->rep_next = now_ms + AOS_PAD_REPEAT_RATE_MS;
    }
}

static inline bool aos_pad_held(const aos_pad_t *p, uint32_t bits)    { return (p->held & bits) != 0; }
static inline bool aos_pad_pressed(const aos_pad_t *p, uint32_t bits) { return (p->pressed & bits) != 0; }
static inline bool aos_pad_repeat(const aos_pad_t *p, uint32_t bits)  { return (p->repeat & bits) != 0; }

/* Forget what is held, so a button that was down when a screen opened does
 * not count as pressed on it (call after switching screens). */
static inline void aos_pad_reset(aos_pad_t *p, uint32_t now_ms)
{
    p->held = aos_pad_bits(&p->x, &p->y, &p->connected);
    if (!p->analog) p->x = p->y = 0;
    p->pressed = p->released = p->repeat = 0;
    p->rep_next = now_ms + AOS_PAD_REPEAT_DELAY_MS;
}
