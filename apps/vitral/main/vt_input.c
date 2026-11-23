/*
 * VITRAL - the hands
 *
 * Fingers come from aos_touch_points_n() (every finger, with an id that
 * stays with it), read every frame, not as LVGL clicks: two thumbs at once
 * work, and in a match each player plays in their half.
 *
 * Gestures (the default), anywhere in the player's area:
 *   - drag sideways: the piece follows the finger, a column per cell
 *   - drag down: it comes down with the finger; a flick down drops it
 *   - flick up: puts it away (hold)
 *   - tap: turns it, clockwise on the right half, the other way on the left
 *   - tap on the held piece's box: hold
 * On-screen keys (Options): left, soft drop, right, drop, turn both ways,
 * with the board's own repeat.
 *
 * A USB gamepad: left/right move (repeating), down soft drop, up drops,
 * A turns clockwise, B the other way, L or R hold, START pause. In a match
 * the first gamepad is player 1 and the second player 2. The keyboard:
 * arrows, Z/X or up to turn, space drops, C holds, P or Esc pauses; in a
 * match player 2 has A/D, S, W (drop), Q/E and F.
 */
#include "vt_app.h"
#include "vt_snd.h"

#include "aos_gesture.h"
#include "aos_hal.h"

#include <math.h>
#include <string.h>

#define DAS_MS      150
#define ARR_MS      42
#define TAP_MS      260
#define TAP_PX      18

enum { K_LEFT, K_DOWN, K_RIGHT, K_DROP, K_CCW, K_CW };

/* keyboard presses, gathered until the next frame */
static uint32_t s_kb[2];
enum { KB_LEFT = 1, KB_RIGHT = 2, KB_DOWN = 4, KB_CW = 8, KB_CCW = 16, KB_DROP = 32, KB_HOLD = 64, KB_PAUSE = 128 };

void vt_input_reset(vt_app_t *a)
{
    memset(a->f, 0, sizeof a->f);
    for (int i = 0; i < 2; i++) {
        vt_player_t *p = &a->p[i];
        p->das_dir = 0;
        p->soft_btn = p->soft_pad = false;
        p->keys_down = 0;
        aos_pad_reset(&p->pad, lv_tick_get());
        vt_soft(&p->g, false);
    }
    s_kb[0] = s_kb[1] = 0;
}

bool vt_input_key(uint32_t key, uint8_t mods)
{
    (void)mods;
    vt_app_t *a = vt_A;
    if (!a || a->hidden) return false;
    if (a->screen != SCR_GAME || a->panel) {
        if (key == AOS_KEY_ESC && a->panel) return vt_menu_back(a);
        return false;
    }
    uint32_t b = 0;
    int pl = 0;
    switch (key) {
    case AOS_KEY_LEFT: b = KB_LEFT; break;
    case AOS_KEY_RIGHT: b = KB_RIGHT; break;
    case AOS_KEY_DOWN: b = KB_DOWN; break;
    case AOS_KEY_UP: case 'x': case 'X': b = KB_CW; break;
    case 'z': case 'Z': b = KB_CCW; break;
    case ' ': case AOS_KEY_ENTER: b = KB_DROP; break;
    case 'c': case 'C': b = KB_HOLD; break;
    case 'p': case 'P': case AOS_KEY_ESC: b = KB_PAUSE; break;
    default: break;
    }
    if (!b && a->nplayers == 2) {
        pl = 1;
        switch (key) {
        case 'a': case 'A': b = KB_LEFT; break;
        case 'd': case 'D': b = KB_RIGHT; break;
        case 's': case 'S': b = KB_DOWN; break;
        case 'w': case 'W': b = KB_DROP; break;
        case 'q': case 'Q': b = KB_CCW; break;
        case 'e': case 'E': b = KB_CW; break;
        case 'f': case 'F': b = KB_HOLD; break;
        default: break;
        }
    }
    if (!b) return false;
    s_kb[pl] |= b;
    return true;
}

/* -------------------------------------------------------------------------- */

static int area_of(const vt_app_t *a, float sx, float sy, float *lx, float *ly)
{
    int cx = (int)((sx - (float)a->r->x) / (float)a->r->scale);
    int cy = (int)((sy - (float)a->r->y) / (float)a->r->scale);
    for (int i = 0; i < a->nplayers; i++) {
        const vt_layout_t *L = &a->p[i].L;
        if (cx >= L->ax && cx < L->ax + L->aw && cy >= L->ay && cy < L->ay + L->ah) {
            int x = cx - L->ax, y = cy - L->ay;
            if (L->flip) {
                x = L->aw - 1 - x;
                y = L->ah - 1 - y;
            }
            *lx = (float)x;
            *ly = (float)y;
            return i;
        }
    }
    return -1;
}

static bool over_obj(lv_obj_t *o, float x, float y)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t r;
    lv_obj_get_coords(o, &r);
    return x >= r.x1 - 10 && x <= r.x2 + 10 && y >= r.y1 - 10 && y <= r.y2 + 10;
}

static int key_at(const vt_layout_t *L, float lx, float ly)
{
    if (!L->keys_h) return -1;
    for (int i = 0; i < 6; i++) {
        if (lx >= L->key_x[i] - 3 && lx < L->key_x[i] + L->key_w[i] + 3 && ly >= L->kys[i] - 8 &&
            ly < L->kys[i] + L->keys_h + 8)
            return i;
    }
    return -1;
}

static bool in_hold_box(const vt_layout_t *L, float lx, float ly)
{
    return L->hx >= 0 && lx >= L->hx - 8 && lx < L->hx + L->nbox + 8 && ly >= L->hy - 16 && ly < L->hy + L->nbox + 8;
}

/* screen pixels to the player's own directions */
static void to_local_d(const vt_player_t *p, float dx, float dy, float *ldx, float *ldy)
{
    *ldx = p->L.flip ? -dx : dx;
    *ldy = p->L.flip ? -dy : dy;
}

static void finger_gesture(vt_app_t *a, vt_finger_t *f, uint32_t now)
{
    vt_player_t *p = &a->p[f->player];
    vt_game_t *g = &p->g;
    float step = (float)(p->L.cell * a->r->scale) * 0.82f;
    if (step < 22.0f) step = 22.0f;
    if (f->spent) return;

    float dx, dy;
    to_local_d(p, f->x - f->ax, f->y - f->ay, &dx, &dy);
    int guard = 0;
    while (dx >= step && guard++ < 12) {
        vt_move(g, 1);
        f->ax += p->L.flip ? -step : step;
        dx -= step;
        f->acted = true;
    }
    while (dx <= -step && guard++ < 24) {
        vt_move(g, -1);
        f->ax -= p->L.flip ? -step : step;
        dx += step;
        f->acted = true;
    }

    /* the speed over the last ~70 ms, for the flicks */
    float vy = 0, vx = 0;
    if (f->hn >= 2) {
        int last = (f->hn - 1) & 7;
        for (int k = 1; k < 8 && k < f->hn; k++) {
            int i = (f->hn - 1 - k) & 7;
            uint32_t dt = f->ht[last] - f->ht[i];
            if (dt >= 50 || k == f->hn - 1 || k == 7) {
                if (dt > 0) {
                    to_local_d(p, (f->hx[last] - f->hx[i]) / (float)dt, (f->hy[last] - f->hy[i]) / (float)dt, &vx, &vy);
                }
                break;
            }
        }
    }
    float tdx, tdy;
    to_local_d(p, f->x - f->x0, f->y - f->y0, &tdx, &tdy);
    if (vy > 1.25f && tdy > step * 1.6f && fabsf(vy) > 2.0f * fabsf(vx)) {
        vt_hard(g);
        f->spent = f->acted = true;
        return;
    }
    if (vy < -1.1f && tdy < -step * 1.6f && fabsf(vy) > 2.0f * fabsf(vx)) {
        if (g->mode != VT_MODE_PUZZLE) vt_hold(g);
        f->spent = f->acted = true;
        return;
    }
    while (dy >= step * 1.15f && guard++ < 40) {
        vt_down(g);
        f->ay += p->L.flip ? -step * 1.15f : step * 1.15f;
        dy -= step * 1.15f;
        f->acted = true;
    }
    if (dy < -step) {
        /* moving back up re-anchors, so coming down again works at once */
        f->ay = f->y + (p->L.flip ? step : -step) * 0.0f;
    }
    (void)now;
}

static void finger_lift(vt_app_t *a, vt_finger_t *f, uint32_t now)
{
    if (f->player < 0 || f->key >= 0) return;
    vt_player_t *p = &a->p[f->player];
    if (f->acted || f->spent) return;
    float d = fabsf(f->x - f->x0) + fabsf(f->y - f->y0);
    if (d > TAP_PX || now - f->t0 > TAP_MS) return;
    float lx, ly;
    area_of(a, f->x0, f->y0, &lx, &ly);
    if (in_hold_box(&p->L, lx, ly) && p->g.mode != VT_MODE_PUZZLE) {
        vt_hold(&p->g);
        return;
    }
    if (a->use_keys && a->nplayers == 1) return;
    vt_rotate(&p->g, lx >= p->L.aw / 2 ? 1 : -1);
}

static void read_fingers(vt_app_t *a, uint32_t now)
{
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    int n = aos_touch_points_n(pts, AOS_TOUCH_MAX);
    (void)n;
    bool seen[VT_FINGERS] = { false };
    uint8_t keys[2] = { 0, 0 };
    for (int i = 0; i < AOS_TOUCH_MAX; i++) {
        if (!pts[i].down) continue;
        int k = -1, free_k = -1;
        for (int j = 0; j < VT_FINGERS; j++) {
            if (a->f[j].on && a->f[j].id == pts[i].id) k = j;
            if (!a->f[j].on && free_k < 0) free_k = j;
        }
        if (k < 0) {
            if (free_k < 0) continue;
            k = free_k;
            vt_finger_t *f = &a->f[k];
            memset(f, 0, sizeof *f);
            f->on = true;
            f->id = pts[i].id;
            f->x0 = f->ax = f->x = pts[i].x;
            f->y0 = f->ay = f->y = pts[i].y;
            f->t0 = now;
            f->key = -1;
            float lx, ly;
            f->player = (int8_t)area_of(a, f->x, f->y, &lx, &ly);
            if (over_obj(a->btn_pause, f->x, f->y) || over_obj(a->btn_undo, f->x, f->y) ||
                over_obj(a->btn_hint, f->x, f->y))
                f->player = -1;
            if (f->player >= 0 && a->use_keys && a->nplayers == 1) f->key = (int8_t)key_at(&a->p[0].L, lx, ly);
        }
        vt_finger_t *f = &a->f[k];
        seen[k] = true;
        f->x = pts[i].x;
        f->y = pts[i].y;
        int h = f->hn & 7;
        f->hx[h] = f->x;
        f->hy[h] = f->y;
        f->ht[h] = now;
        f->hn++;
        if (f->player < 0) continue;
        if (f->key >= 0) {
            /* a key stays down while the finger is on it */
            float lx, ly;
            int pl = area_of(a, f->x, f->y, &lx, &ly);
            if (pl == f->player && key_at(&a->p[pl].L, lx, ly) == f->key) keys[f->player] |= (uint8_t)(1u << f->key);
            continue;
        }
        if (!a->use_keys || a->nplayers == 2) finger_gesture(a, f, now);
    }
    for (int j = 0; j < VT_FINGERS; j++) {
        if (a->f[j].on && !seen[j]) {
            finger_lift(a, &a->f[j], now);
            a->f[j].on = false;
        }
    }
    for (int i = 0; i < a->nplayers; i++) {
        vt_player_t *p = &a->p[i];
        uint8_t pressed = keys[i] & (uint8_t)~p->keys_down;
        p->keys_down = keys[i];
        vt_game_t *g = &p->g;
        if (pressed & (1u << K_DROP)) vt_hard(g);
        if (pressed & (1u << K_CW)) vt_rotate(g, 1);
        if (pressed & (1u << K_CCW)) vt_rotate(g, -1);
        p->soft_btn = (keys[i] >> K_DOWN) & 1;
        if (pressed & (1u << K_DOWN)) vt_down(g);
    }
}

/* the gamepads: all of them for one player; the first and second apart in a match */
static uint32_t pad_bits_of(int idx)
{
    aos_gamepad_t p;
    if (!aos_hal_hid_gamepad_get(idx, &p)) return 0;
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
    return b;
}

static void pad_player(vt_app_t *a, int i, uint32_t now, bool *pause)
{
    vt_player_t *p = &a->p[i];
    vt_game_t *g = &p->g;
    uint32_t held;
    if (a->nplayers == 2) {
        held = pad_bits_of(i);
        uint32_t prev = p->pad.held;
        p->pad.held = held;
        p->pad.pressed = held & ~prev;
    } else {
        aos_pad_update(&p->pad, now);
        held = p->pad.held;
    }
    uint32_t pr = p->pad.pressed;
    uint32_t kb = s_kb[i];
    s_kb[i] = 0;

    if ((pr & AOS_PAD_START) || (kb & KB_PAUSE)) *pause = true;
    if ((pr & AOS_PAD_UP) || (kb & KB_DROP)) vt_hard(g);
    if ((pr & AOS_PAD_A) || (kb & KB_CW)) vt_rotate(g, 1);
    if ((pr & AOS_PAD_B) || (kb & KB_CCW)) vt_rotate(g, -1);
    if ((pr & (AOS_PAD_L | AOS_PAD_R)) || (kb & KB_HOLD)) vt_hold(g);
    if (kb & KB_LEFT) vt_move(g, -1);
    if (kb & KB_RIGHT) vt_move(g, 1);
    if (kb & KB_DOWN) vt_down(g);
    p->soft_pad = (held & AOS_PAD_DOWN) != 0;
    if (pr & AOS_PAD_DOWN) vt_down(g);

    /* left and right repeat, from the pad or the on-screen keys */
    int dir = 0;
    bool l = (held & AOS_PAD_LEFT) || (p->keys_down & (1u << K_LEFT));
    bool r = (held & AOS_PAD_RIGHT) || (p->keys_down & (1u << K_RIGHT));
    if (l && !r) dir = -1;
    if (r && !l) dir = 1;
    if (dir != p->das_dir) {
        p->das_dir = (int8_t)dir;
        if (dir) {
            vt_move(g, dir);
            p->das_next = now + DAS_MS;
        }
    } else if (dir && (int32_t)(now - p->das_next) >= 0) {
        vt_move(g, dir);
        p->das_next = now + ARR_MS;
    }
    vt_soft(g, p->soft_btn || p->soft_pad);
}

void vt_input_frame(vt_app_t *a)
{
    uint32_t now = lv_tick_get();
    if (a->screen != SCR_GAME || a->panel || a->paused) {
        /* fingers that were down when a panel came up are forgotten */
        memset(a->f, 0, sizeof a->f);
        for (int i = 0; i < 2; i++) {
            a->p[i].keys_down = 0;
            vt_soft(&a->p[i].g, false);
        }
        s_kb[0] = s_kb[1] = 0;
        return;
    }
    read_fingers(a, now);
    bool pause = false;
    for (int i = 0; i < a->nplayers; i++) pad_player(a, i, now, &pause);
    if (pause) vt_set_paused(a, true);
}
