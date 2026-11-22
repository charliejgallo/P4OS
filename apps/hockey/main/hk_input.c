/*
 * HOCKEY - the hands: fingers and gamepads
 *
 * A finger holds a mallet by its knob: the mallet goes where the finger
 * is, as fast as a mallet may (it does not jump) and no further than its
 * half. Against the board any finger plays player 1's mallet (the last
 * one down); with two players a finger belongs to the half it lands in, so
 * each player has one finger of their own at the same time
 * (aos_touch_points_n). The pause is the round button on the right rail.
 *
 * A gamepad per player (aos_hal_hid_gamepad_get(0) and (1)): the d-pad or
 * the stick pushes the mallet, A held pushes it fast (a shot), B takes it
 * back to the goal, START pauses. Player 2 sits at the other end, so their
 * directions are turned. Against the board both gamepads play player 1.
 *
 * LVGL's thread gathers it into hk_in_t (under in_mx) and the worker takes
 * it every step (hk_input_hand).
 */
#include "hk_app.h"
#include "hk_view.h"

#include "aos_gesture.h"
#include "aos_hal.h"

#include <math.h>
#include <string.h>

#define FINGER_SPEED 4200.0f    /* mm/s: a mallet behind a finger */
#define PAD_SPEED    1500.0f
#define PAD_FAST     2800.0f

extern void *hk_in_mx;

static int side_at(const hk_app_t *a, float y)
{
    if (a->cfg.mode == MODE_CPU) return 0;
    return y >= (float)HK_CY ? 0 : 1;
}

void hk_input_touch(hk_app_t *a)
{
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    aos_touch_points_n(pts, AOS_TOUCH_MAX);
    aos_hal_mutex_lock(hk_in_mx);
    int last[2] = { -1, -1 };
    for (int i = 0; i < AOS_TOUCH_MAX && i < 5; i++) {
        hk_finger_t *f = &a->fing[i];
        const aos_touch_point_t *p = &pts[i];
        if (p->down) {
            if (f->id != p->id || !f->id) {
                memset(f, 0, sizeof *f);
                f->id = p->id ? p->id : 0xFF;
                float dx = p->x - HK_PAUSE_X, dy = p->y - HK_PAUSE_Y;
                f->side = dx * dx + dy * dy < HK_PAUSE_R * HK_PAUSE_R ? -2 : side_at(a, p->y);
                if (a->game && a->game->state == HK_ST_REPLAY && f->side != -2) a->skip_replay = true;
                /* the newest finger of a side is the one that plays */
                if (f->side >= 0) {
                    for (int j = 0; j < 5; j++) {
                        if (j != i && a->fing[j].side == f->side) a->fing[j].side = -1;
                    }
                }
            }
            f->x = p->x;
            f->y = p->y;
            if (f->side >= 0) last[f->side] = i;
        } else if (f->id) {
            if (f->side == -2) {
                float dx = f->x - HK_PAUSE_X, dy = f->y - HK_PAUSE_Y;
                if (dx * dx + dy * dy < HK_PAUSE_R * HK_PAUSE_R * 2) a->button_pause = true;
            }
            memset(f, 0, sizeof *f);
        }
    }
    for (int s = 0; s < 2; s++) {
        hk_in_t *in = &a->in[s];
        in->finger = last[s] >= 0;
        if (in->finger) {
            in->fx = hk_tx(a->fing[last[s]].x);
            in->fy = hk_ty(a->fing[last[s]].y, HK_GRIP_Z);
        }
    }
    aos_hal_mutex_unlock(hk_in_mx);
    if (a->ui) {
        for (int i = 0; i < 5; i++) {
            a->ui->touch_on[i] = a->fing[i].id && a->fing[i].side >= 0;
            a->ui->touch_x[i] = (int16_t)a->fing[i].x;
            a->ui->touch_y[i] = (int16_t)a->fing[i].y;
        }
    }
}

/* one gamepad's roles, as aos_pad.h maps them, and its stick */
static uint32_t pad_bits(int i, float *sx, float *sy)
{
    aos_gamepad_t p;
    *sx = *sy = 0;
    if (!aos_hal_hid_gamepad_get(i, &p)) return 0;
    uint32_t b = 0;
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
    if ((p.axes & 3) == 3) {
        int x = p.axis[0], y = p.axis[1];
        int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
        if (ax > 6000 && ax < 32800) *sx = (float)x / 32767.0f;
        if (ay > 6000 && ay < 32800) *sy = (float)y / 32767.0f;
    }
    return b;
}

void hk_input_pads(hk_app_t *a)
{
    aos_hal_mutex_lock(hk_in_mx);
    for (int s = 0; s < 2; s++) {
        a->in[s].pad_x = a->in[s].pad_y = 0;
        a->in[s].pad_fast = a->in[s].pad_home = false;
    }
    for (int p = 0; p < 2; p++) {
        float sx, sy;
        uint32_t b = pad_bits(p, &sx, &sy);
        int side = a->cfg.mode == MODE_CPU ? 0 : p;
        uint32_t pressed = b & ~a->pad_prev[p];
        a->pad_prev[p] = b;
        float x = sx, y = sy;
        if (b & AOS_PAD_LEFT) x = -1;
        if (b & AOS_PAD_RIGHT) x = 1;
        if (b & AOS_PAD_UP) y = -1;
        if (b & AOS_PAD_DOWN) y = 1;
        /* the far player looks at the table from the other end */
        if (side == 1) {
            x = -x;
            y = -y;
        }
        hk_in_t *in = &a->in[side];
        if (x != 0 || y != 0) {
            in->pad_x = x;
            in->pad_y = y;
        }
        if (b & AOS_PAD_A) in->pad_fast = true;
        if (b & AOS_PAD_B) in->pad_home = true;
        if (pressed & AOS_PAD_START) a->button_pause = true;
        if (pressed && a->game && a->game->state == HK_ST_REPLAY && !(pressed & AOS_PAD_START)) a->skip_replay = true;
    }
    aos_hal_mutex_unlock(hk_in_mx);
}

void hk_input_hand(hk_app_t *a, int side, hk_hand_t *h)
{
    const hk_game_t *g = a->game;
    const hk_mallet_t *m = &g->m[side];
    const hk_in_t *in = &a->in[side];
    memset(h, 0, sizeof *h);
    if (in->finger) {
        h->on = true;
        h->x = in->fx;
        h->y = in->fy;
        h->speed = FINGER_SPEED;
    } else if (in->pad_home) {
        h->on = true;
        h->x = 0;
        h->y = hk_dir(side) * HK_HOME_Y;
        h->speed = PAD_SPEED;
    } else if (in->pad_x != 0 || in->pad_y != 0) {
        float l = sqrtf(in->pad_x * in->pad_x + in->pad_y * in->pad_y);
        float k = l > 1.0f ? 1.0f / l : 1.0f;
        float sp = in->pad_fast ? PAD_FAST : PAD_SPEED;
        /* a point ahead of the mallet: it moves at the stick's share of the speed */
        h->on = true;
        h->speed = sp * (l > 1.0f ? 1.0f : l);
        h->x = m->x + in->pad_x * k * 200.0f;
        h->y = m->y + in->pad_y * k * 200.0f;
    }
}
