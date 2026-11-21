/*
 * METEGOL - the hands: fingers and gamepads
 *
 * Each player owns the half of the screen with their goal (in one against
 * the board, only their own half counts). A finger there:
 *   - sliding up and down moves the rods (all of them, or only the one
 *     nearest the ball, as Ajustes says) as far as the finger goes;
 *   - a tap kicks with the rod nearest the ball: a whole quick turn, a
 *     quicker tap a harder one;
 *   - held still, the figures go up and let the ball through;
 *   - a short flick towards the other goal is a shot whose strength is the
 *     flick's speed and whose effect is its slant.
 * A gamepad per player (aos_hal_hid_gamepad_get(0) and (1)): the d-pad or
 * the stick moves the rods, A kicks, B raises, L/R choose the rod when
 * they go one at a time, START pauses.
 *
 * LVGL's thread gathers it into mg_in_t (under in_mx) and the worker takes
 * it once a frame (mg_input_hand).
 */
#include "mg_app.h"
#include "mg_view.h"

#include "aos_gesture.h"
#include "aos_hal.h"

#include <math.h>
#include <string.h>

#define HOLD_MS   230
#define TAP_PX    22.0f
#define PAUSE_X   1232
#define PAUSE_Y   38
#define PAUSE_R   34

extern void *mg_in_mx;

static float clampf(float v, float a, float b)
{
    return v < a ? a : v > b ? b : v;
}

/* the team a finger at x plays, or -1 */
static int team_at(const mg_app_t *a, float x)
{
    int side = x < MG_W / 2 ? 0 : 1;
    int team = mg_team_of_side(a, side);
    if (a->cfg.mode == MODE_CPU && team != 0) return -1;
    return team;
}

void mg_input_touch(mg_app_t *a)
{
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    int n = aos_touch_points_n(pts, AOS_TOUCH_MAX);
    (void)n;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    aos_hal_mutex_lock(mg_in_mx);
    bool raise[2] = { false, false };
    for (int i = 0; i < AOS_TOUCH_MAX && i < 5; i++) {
        mg_finger_t *f = &a->fing[i];
        const aos_touch_point_t *p = &pts[i];
        if (p->down) {
            if (f->id != p->id) {
                memset(f, 0, sizeof *f);
                f->id = p->id;
                f->x0 = f->x = p->x;
                f->y0 = f->y = f->ly = p->y;
                f->t0 = now;
                float dx = p->x - PAUSE_X, dy = p->y - PAUSE_Y;
                f->team = dx * dx + dy * dy < PAUSE_R * PAUSE_R ? -2 : team_at(a, p->x);
                if (a->game && a->game->state == MG_ST_REPLAY && f->team != -2) a->skip_replay = true;
            }
            f->x = p->x;
            f->y = p->y;
            float d = fabsf(f->x - f->x0) + fabsf(f->y - f->y0);
            if (d > f->moved) f->moved = d;
            if (f->team >= 0) {
                /* the rods follow the finger: a pixel down the screen is this many mm across the field */
                a->in[f->team].move += (f->y - f->ly) / (MG_S * MG_COS);
                if (!f->held && f->moved < TAP_PX && now - f->t0 > HOLD_MS) f->held = true;
                if (f->held) raise[f->team] = true;
            }
            f->ly = f->y;
        } else if (f->id) {
            if (f->team == -2) {
                float dx = f->x - PAUSE_X, dy = f->y - PAUSE_Y;
                if (dx * dx + dy * dy < PAUSE_R * PAUSE_R * 2) a->button_pause = true;
            } else if (f->team >= 0 && !f->held) {
                mg_in_t *in = &a->in[f->team];
                float dx = f->x - f->x0, dy = f->y - f->y0;
                float dt = (float)(now - f->t0);
                if (dt < 1) dt = 1;
                int side = mg_side_of_team(a, f->team);
                float dir = side == 0 ? 1.0f : -1.0f;
                if (f->moved < TAP_PX && dt < 320) {
                    in->kick = true;
                    in->power = dt < 110 ? 1.0f : 0.82f;
                    in->spin = 0;
                    in->slide = 0;
                } else if (dx * dir > 36.0f && dt < 400 && fabsf(dx) > fabsf(dy) * 0.6f) {
                    float sp = fabsf(dx) / dt;                    /* px a millisecond */
                    in->kick = true;
                    in->power = clampf(0.45f + sp * 0.35f, 0.45f, 1.0f);
                    in->spin = clampf(dy / fabsf(dx) * 1.3f, -1.0f, 1.0f);
                    in->slide = clampf(dy / (MG_S * MG_COS) / (dt / 1000.0f), -1600.0f, 1600.0f);
                }
            }
            memset(f, 0, sizeof *f);
        }
    }
    for (int t = 0; t < 2; t++) {
        /* a gamepad's B counts too */
        a->in[t].raise = raise[t] || (a->pad_prev[t] & AOS_PAD_B);
    }
    aos_hal_mutex_unlock(mg_in_mx);
    if (a->ui) {
        for (int i = 0; i < 5; i++) {
            a->ui->touch_on[i] = a->fing[i].id && a->fing[i].team >= 0;
            a->ui->touch_x[i] = (int16_t)a->fing[i].x;
            a->ui->touch_y[i] = (int16_t)a->fing[i].y;
        }
    }
}

/* one gamepad's roles, as aos_pad.h maps them */
static uint32_t pad_bits(int i, float *sy)
{
    aos_gamepad_t p;
    *sy = 0;
    if (!aos_hal_hid_gamepad_get(i, &p)) return 0;
    uint32_t b = 0;
    uint8_t d = aos_hal_hid_gamepad_dpad(&p);
    if (d & AOS_DPAD_UP) b |= AOS_PAD_UP;
    if (d & AOS_DPAD_DOWN) b |= AOS_PAD_DOWN;
    if (p.buttons & 0x003) b |= AOS_PAD_A;
    if (p.buttons & 0x00C) b |= AOS_PAD_B;
    if (p.buttons & 0x050) b |= AOS_PAD_L;
    if (p.buttons & 0x0A0) b |= AOS_PAD_R;
    if (p.buttons & 0x300) b |= AOS_PAD_START;
    if ((p.axes & 3) == 3) {
        int y = p.axis[1];
        int ay = y < 0 ? -y : y;
        if (ay > 6000 && ay < 30000) *sy = (float)y / 32767.0f;
    }
    return b;
}

void mg_input_pads(mg_app_t *a)
{
    aos_hal_mutex_lock(mg_in_mx);
    for (int p = 0; p < 2; p++) {
        float sy;
        uint32_t b = pad_bits(p, &sy);
        /* in one against the board both gamepads play the person's team */
        int team = a->cfg.mode == MODE_CPU ? 0 : p;
        uint32_t pressed = b & ~a->pad_prev[p];
        a->pad_prev[p] = b;
        mg_in_t *in = &a->in[team];
        float y = sy;
        if (b & AOS_PAD_UP) y = -1;
        if (b & AOS_PAD_DOWN) y = 1;
        if (p == 0 || a->cfg.mode == MODE_TWO || y != 0) in->pad_y = y;
        if (pressed & AOS_PAD_A) {
            in->kick = true;
            in->power = 0.9f;
            in->spin = 0;
            in->slide = y * 900.0f;
        }
        if (pressed & (AOS_PAD_L | AOS_PAD_R)) {
            int s = in->sel < 0 ? 2 : in->sel;
            s += (pressed & AOS_PAD_R) ? 1 : -1;
            in->sel = (s + MG_SIDE_RODS) % MG_SIDE_RODS;
        }
        if (pressed & AOS_PAD_START) a->button_pause = true;
        if (b & AOS_PAD_B) in->raise = true;
    }
    aos_hal_mutex_unlock(mg_in_mx);
}

/* the rod a kick should come from: the side's nearest the ball, preferring
 * one with the ball where a turn reaches it */
static int kick_rod(const mg_game_t *g, int side)
{
    int best = 0;
    float bd = 1e9f;
    for (int k = 0; k < MG_SIDE_RODS; k++) {
        float d = fabsf(g->rod[mg_rod_ix(side, k)].x - g->bx);
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

void mg_input_hand(mg_app_t *a, int team, mg_hand_t *h, bool first)
{
    mg_game_t *g = a->game;
    int side = mg_side_of_team(a, team);
    memset(h, 0, sizeof *h);
    h->kick_rod = -1;
    h->speed = 3200.0f;
    mg_in_t *in = &a->in[team];
    h->all = !a->cfg.one_rod;
    h->rod = in->sel >= 0 && a->cfg.one_rod ? in->sel : mg_nearest_rod(g, side);
    h->raise = in->raise;
    if (!first) return;
    float move = in->move + in->pad_y * 1100.0f / 60.0f;
    in->move = 0;
    /* the help: with the hand still, the rod nearest the ball puts a figure on it */
    if (a->cfg.assist && move == 0.0f) {
        int k = mg_nearest_rod(g, side);
        mg_rod_t *r = &g->rod[mg_rod_ix(side, k)];
        int f = mg_nearest_fig(r, g->by);
        float err = g->by - (r->base[f] + r->off);
        h->all = false;
        h->rod = k;
        move = clampf(err, -8.0f, 8.0f);
    }
    h->move = move;
    if (in->kick) {
        in->kick = false;
        h->kick_rod = a->cfg.one_rod && in->sel >= 0 ? in->sel : kick_rod(g, side);
        h->kick_power = in->power;
        h->kick_spin = in->spin;
        h->kick_slide = in->slide;
    }
}
