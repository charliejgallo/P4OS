/*
 * DERIVA - the hands: fingers on the deck and gamepads
 *
 * The deck has two halves, one per thumb. On the stick's side a thumb
 * that lands anywhere becomes a stick with its base where it landed: the
 * ship turns to where it points, and pushed past half way it also
 * pushes the ship, harder the further. In the buttons' way that side has
 * turn left, turn right and push, and a thumb between two presses both.
 * On the other side the jump button is the jump and everything else is
 * fire, so a thumb never has to find it. The pause is the small round
 * button in the corner. Fingers come from aos_touch_points_n, five at
 * once, so both thumbs work together.
 *
 * A gamepad: an analog stick is the touch stick, the d-pad turns and
 * pushes (up), A fires, B jumps, START pauses. With two players and two
 * gamepads each has one; with one gamepad it is player 2's, and player 1
 * plays on the screen.
 *
 * LVGL's thread gathers it into ast_in_t (under the in mutex) and the
 * worker takes it every step (ast_input_hand).
 */
#include "ast_app.h"

#include "aos_gesture.h"
#include "aos_hal.h"

#include <math.h>
#include <string.h>

extern void *ast_in_mx;

static bool inside(float x, float y, int cx, int cy, float r)
{
    float dx = x - (float)cx, dy = y - (float)cy;
    return dx * dx + dy * dy < r * r;
}

static bool stick_side(const ast_layout_t *L, float x, float y)
{
    return x >= (float)L->zone_x0 && x < (float)L->zone_x1 && y >= (float)L->zone_y0 && y < (float)L->zone_y1;
}

/* the other thumb's side of the deck */
static bool fire_side(const ast_layout_t *L, bool lefty, float x, float y)
{
    if (L->land) return lefty ? x < (float)L->fx : x >= (float)(L->fx + L->fw);
    if (y < (float)(L->fy + L->fh)) return false;
    return lefty ? x < (float)L->W / 2 : x >= (float)L->W / 2;
}

void ast_input_touch(ast_app_t *a)
{
    const ast_layout_t *L = &a->lay;
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    aos_touch_points_n(pts, AOS_TOUCH_MAX);
    aos_hal_mutex_lock(ast_in_mx);
    ast_in_t *in = &a->in[0];
    bool stick = false, fire = false, tl = false, tr = false, push = false, jump = false;
    for (int i = 0; i < AOS_TOUCH_MAX && i < 5; i++) {
        ast_finger_t *f = &a->fing[i];
        const aos_touch_point_t *p = &pts[i];
        if (p->down) {
            if (f->id != p->id || !f->id) {
                memset(f, 0, sizeof *f);
                f->id = p->id ? p->id : 0xFF;
                f->x0 = p->x;
                f->y0 = p->y;
                if (inside(p->x, p->y, L->pause_x, L->pause_y, (float)L->pause_r * 1.5f)) {
                    f->role = FG_PAUSE;
                } else if (stick_side(L, p->x, p->y)) {
                    f->role = a->cfg.controls == CTL_STICK ? FG_STICK : FG_BTNS;
                    if (f->role == FG_STICK) {
                        /* the base where the thumb landed, kept off the edges */
                        float m = (float)L->stick_r * 0.55f;
                        float cx = p->x, cy = p->y;
                        if (cx < L->zone_x0 + m) cx = L->zone_x0 + m;
                        if (cx > L->zone_x1 - m) cx = L->zone_x1 - m;
                        if (cy < L->zone_y0 + m) cy = L->zone_y0 + m;
                        if (cy > L->zone_y1 - m) cy = L->zone_y1 - m;
                        f->x0 = cx;
                        f->y0 = cy;
                    }
                } else if (inside(p->x, p->y, L->jump_x, L->jump_y, (float)L->jump_r * 1.3f)) {
                    f->role = FG_JUMP;
                    a->jump_latch[0] = true;
                } else if (fire_side(L, a->cfg.lefty != 0, p->x, p->y) ||
                           inside(p->x, p->y, L->fire_x, L->fire_y, (float)L->fire_r * 1.3f)) {
                    f->role = FG_FIRE;
                } else {
                    f->role = FG_NONE;
                }
            }
            f->x = p->x;
            f->y = p->y;
            switch (f->role) {
            case FG_STICK: {
                float dx = (f->x - f->x0) / (float)L->stick_r, dy = (f->y - f->y0) / (float)L->stick_r;
                float l = sqrtf(dx * dx + dy * dy);
                if (l > 1.0f) {
                    dx /= l;
                    dy /= l;
                }
                stick = true;
                in->sx = dx;
                in->sy = dy;
                in->cx = f->x0;
                in->cy = f->y0;
                break;
            }
            case FG_BTNS: {
                float r = (float)L->btn_r * 1.2f;
                bool l = inside(f->x, f->y, L->tl_x, L->tl_y, r), rr = inside(f->x, f->y, L->tr_x, L->tr_y, r);
                bool pu = inside(f->x, f->y, L->push_x, L->push_y, r);
                if (!l && !rr && !pu) {
                    /* nearest of the three */
                    float d[3], best = 1e9f;
                    int bi = 0;
                    int xs[3] = { L->tl_x, L->tr_x, L->push_x }, ys[3] = { L->tl_y, L->tr_y, L->push_y };
                    for (int k = 0; k < 3; k++) {
                        float ex = f->x - (float)xs[k], ey = f->y - (float)ys[k];
                        d[k] = ex * ex + ey * ey;
                        if (d[k] < best) {
                            best = d[k];
                            bi = k;
                        }
                    }
                    l = bi == 0;
                    rr = bi == 1;
                    pu = bi == 2;
                }
                tl |= l;
                tr |= rr;
                push |= pu;
                break;
            }
            case FG_FIRE: fire = true; break;
            case FG_JUMP: jump = true; break;
            default: break;
            }
        } else if (f->id) {
            if (f->role == FG_PAUSE && inside(f->x, f->y, L->pause_x, L->pause_y, (float)L->pause_r * 2.0f)) a->button_pause = true;
            memset(f, 0, sizeof *f);
        }
    }
    in->stick = stick;
    if (!stick) in->sx = in->sy = 0;
    in->fire = fire;
    in->jump = jump;
    in->turn_l = tl;
    in->turn_r = tr;
    in->push = push;
    aos_hal_mutex_unlock(ast_in_mx);
}

/* one gamepad: its roles (aos_pad.h's mapping) and its stick */
static uint32_t pad_bits(int i, float *sx, float *sy, bool *analog)
{
    aos_gamepad_t p;
    *sx = *sy = 0;
    if (!aos_hal_hid_gamepad_get(i, &p)) return 0;
    uint32_t b = 0x80000000u;
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
        /* a d-pad sent as axes is all or nothing; a stick is seen part way */
        if ((ax > 8000 && ax < 28000) || (ay > 8000 && ay < 28000)) *analog = true;
        if (*analog) {
            if (ax > 5000) *sx = (float)x / 32767.0f;
            if (ay > 5000) *sy = (float)y / 32767.0f;
        }
    }
    return b;
}

void ast_input_pads(ast_app_t *a)
{
    static bool analog[2];
    aos_hal_mutex_lock(ast_in_mx);
    for (int s = 0; s < 2; s++) {
        ast_in_t *in = &a->in[s];
        in->pad_on = false;
        in->pad_x = in->pad_y = 0;
        in->pad_l = in->pad_r = in->pad_up = in->pad_fire = in->pad_jump = false;
    }
    float sx[2], sy[2];
    uint32_t bits[2];
    int npads = 0;
    for (int p = 0; p < 2; p++) {
        bits[p] = pad_bits(p, &sx[p], &sy[p], &analog[p]);
        if (bits[p]) npads++;
        else analog[p] = false;
    }
    for (int p = 0; p < 2; p++) {
        uint32_t b = bits[p];
        if (!b) continue;
        int s = 0;
        if (a->players == 2) s = npads >= 2 ? p : 1;
        uint32_t pressed = b & ~a->pad_prev[p];
        a->pad_prev[p] = b;
        ast_in_t *in = &a->in[s];
        in->pad_on = true;
        if (sx[p] != 0 || sy[p] != 0) {
            in->pad_x = sx[p];
            in->pad_y = sy[p];
        }
        in->pad_l |= (b & AOS_PAD_LEFT) != 0;
        in->pad_r |= (b & AOS_PAD_RIGHT) != 0;
        in->pad_up |= (b & AOS_PAD_UP) != 0;
        in->pad_fire |= (b & (AOS_PAD_A | AOS_PAD_L)) != 0;
        if (pressed & (AOS_PAD_B | AOS_PAD_R)) a->jump_latch[s] = true;
        if (pressed & AOS_PAD_START) a->button_pause = true;
    }
    for (int p = 0; p < 2; p++) {
        if (!bits[p]) a->pad_prev[p] = 0;
    }
    aos_hal_mutex_unlock(ast_in_mx);
}

static float push_of(float m)
{
    /* part way only turns; past half way it pushes, harder the further */
    if (m < 0.55f) return 0;
    float t = (m - 0.45f) / 0.5f;
    return t > 1 ? 1 : t;
}

void ast_input_hand(ast_app_t *a, int s, ag_hand_t *h)
{
    const ast_in_t *in = &a->in[s];
    memset(h, 0, sizeof *h);
    if (in->stick) {
        h->aim = true;
        h->ax = in->sx;
        h->ay = in->sy;
        h->thrust = push_of(sqrtf(in->sx * in->sx + in->sy * in->sy));
    } else if (in->turn_l || in->turn_r || in->push) {
        h->turn = (in->turn_r ? 1.0f : 0) - (in->turn_l ? 1.0f : 0);
        h->thrust = in->push ? 1.0f : 0;
    } else if (in->pad_x != 0 || in->pad_y != 0) {
        h->aim = true;
        h->ax = in->pad_x;
        h->ay = in->pad_y;
        float m = sqrtf(in->pad_x * in->pad_x + in->pad_y * in->pad_y);
        h->thrust = push_of(m > 1 ? 1 : m);
    } else {
        h->turn = (in->pad_r ? 1.0f : 0) - (in->pad_l ? 1.0f : 0);
        h->thrust = in->pad_up ? 1.0f : 0;
    }
    h->fire = in->fire || in->pad_fire;
    h->jump = a->jump_latch[s];
    a->jump_latch[s] = false;
}
