/*
 * VUELO - a flight over worlds drawn live in 3D.
 *
 * The prototype of the engine: a world made from a seed, the plane, the
 * flight, and the numbers of what it costs. Lying down only.
 *
 * Threads. The worker (core 0) makes the worlds and flies: it steps the
 * plane, draws the frame small (426 x 240 unless asked otherwise) with
 * both cores (vr_frame splits every pass) and leaves it in one of three
 * slots. LVGL's timer, every 8 ms, reads the fingers, the gamepad and the
 * keyboard, and puts the newest finished frame on the panel: the PPA
 * scales it up and turns it into the panel's free buffer, which is then
 * flipped to (aos_hal_display_blit_into_fit + flip), while the worker
 * already draws the next. Where there is no panel to flip (the simulator)
 * the frame is scaled into a canvas instead.
 *
 * The log says every 5 s what a frame costs (on the board: /api/log).
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_gesture.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_pad.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include "vu_flight.h"
#include "vu_hud.h"
#include "vu_mem.h"
#include "vu_plane.h"
#include "vu_render.h"
#include "vu_world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS     8
#define NF          3
#define MAXW        640
#define MAXH        360
#define FRAME_MIN_US 16000          /* 60 fps at most: the panel's refresh */
#define PERF_MS     5000

/* The frame's sizes: each scales by a whole number of sixteenths into the
 * 1280 x 720 screen (the PPA's steps), so nothing of the panel is left
 * stale round it; 426 is 1278 wide, a pixel short each side. */
static const int RES[][2] = { { 320, 180 }, { 426, 240 }, { 512, 288 }, { 640, 360 } };
#define NRES 4
#define RES_DEFAULT 1

enum { F_FREE = 0, F_BUSY, F_READY, F_SHOWN };
enum { R_NONE = 0, R_STICK, R_THR, R_BTN };

static const char *const WORLD_NAME[VW_BIOMES] = { N_("Islas"), N_("Cañones"), N_("Nieve"), N_("Volcán"),
                                                    N_("Ciudad") };
/* the frame's font is ASCII: the names as it can draw them */
static const char *const WORLD_HUD[VW_BIOMES] = { "ISLAS", "CANONES", "NIEVE", "VOLCAN", "CIUDAD" };
static const uint8_t WORLD_HOUR[VW_BIOMES] = { VW_NOON, VW_EVENING, VW_MORNING, VW_EVENING, VW_NOON };

typedef struct {
    uint8_t id, role, btn;
    float   x0, y0;
} finger_t;

typedef struct {
    aos_app_t  *self;
    lv_obj_t   *root, *canvas, *load_box, *load_lbl, *load_bar;
    lv_timer_t *timer;
    int         SW, SH;
    bool        closing, worker_ok;
    /* frames */
    uint16_t   *fr_mem[NF], *fr[NF];
    int         fst[NF], fw[NF], fh[NF];
    uint32_t    fseq[NF], seq;
    int         shown;
    uint16_t   *cv_buf;
    int         ppa;                /* -1 not tried, 1 the PPA flips, 0 the canvas */
    volatile int res;               /* index into RES */
    /* the world */
    vw_world_t  world;
    volatile int gen_req;           /* biome * 8 + hour, -1 none */
    volatile int progress;
    volatile bool world_ok;
    uint32_t    seed;
    int         biome, hour;
    /* the flight: the worker's */
    vf_plane_t  pl;
    float       t;
    vp_draw_t   draw;
    vh_layout_t lay;
    vh_state_t  hs;
    /* what the hands ask: LVGL's thread writes, the worker reads */
    volatile float in_x, in_y, in_thr;
    volatile bool  in_auto, stick_on, paused;
    volatile float stick_bx, stick_by, stick_x, stick_y;   /* screen pixels; -1..1 */
    volatile int   in_view_n;                              /* camera presses, counted */
    volatile uint8_t btn_held;
    int            view_seen;
    finger_t       fing[AOS_TOUCH_MAX];
    aos_pad_t      pad;
    uint8_t        keys_prev[8];
    int            nkeys_prev;
    /* the measurement: the worker's sums, the UI's presents */
    uint32_t perf_t0, perf_frames, perf_presents;
    uint64_t perf_frame_us, perf_ground_us, perf_turn_us, perf_hud_us, perf_present_us, perf_samples, perf_lines;
    uint64_t perf_upx;
    volatile int fps10;
} app_t;

uint32_t vr_now_us(void)
{
    return (uint32_t)aos_hal_uptime_us();
}

/* --------------------------------------------------------------------------
 * The worker
 * -------------------------------------------------------------------------- */

static void make_world(app_t *a, int req)
{
    int biome = req >> 3, hour = req & 7;
    uint32_t t0 = vr_now_us();
    a->progress = 0;
    bool ok = vw_generate(&a->world, biome, a->seed + (uint32_t)biome * 101u, hour, aos_hal_worker_split, &a->progress);
    uint32_t ms = (vr_now_us() - t0) / 1000;
    aos_hal_log("vuelo", "world %d hour %d seed %u made in %u ms (heights %u, trees+shadows %u, colours %u, levels %u), "
                "highest %d m | psram held %u KB",
                biome, hour, (unsigned)a->seed, (unsigned)ms, (unsigned)a->world.gen_ms[0],
                (unsigned)a->world.gen_ms[1], (unsigned)a->world.gen_ms[2], (unsigned)a->world.gen_ms[3],
                (int)a->world.hmax, (unsigned)(vu_mem_used() / 1024));
    a->biome = biome;
    a->hour = hour;
    vf_reset(&a->pl, &a->world);
    a->t = 0;
    a->world_ok = ok;
    __atomic_store_n(&a->gen_req, -1, __ATOMIC_RELEASE);
}

static int free_slot(app_t *a)
{
    for (int i = 0; i < NF; i++)
        if (__atomic_load_n(&a->fst[i], __ATOMIC_ACQUIRE) == F_FREE) return i;
    return -1;
}

static void over_fn(void *arg, uint16_t *out, int W, int H, int y0, int y1)
{
    app_t *a = (app_t *)arg;
    vp_draw(&a->draw, out, W, H, y0, y1);
}

static void fly_frame(app_t *a, int slot, float dt)
{
    uint32_t t0 = vr_now_us();
    int W = RES[a->res][0], H = RES[a->res][1];
    vf_plane_t *pl = &a->pl;
    /* the hands */
    if (a->in_auto) {
        vf_autopilot(pl, &a->world, a->t);
    } else {
        pl->in_x = a->in_x;
        pl->in_y = a->in_y;
        pl->in_thr = a->in_thr;
    }
    int vn = a->in_view_n;
    if (vn != a->view_seen) {
        a->view_seen = vn;
        pl->view = (pl->view + 1) % VF_VIEWS;
        pl->cam_ok = false;
    }
    /* the step, in pieces no longer than 1/60 s */
    float left = dt;
    while (left > 0) {
        float h = left > 1.0f / 60.0f ? 1.0f / 60.0f : left;
        vf_step(pl, &a->world, h);
        left -= h;
    }
    a->t += dt;
    if (pl->crashed && pl->crash_t > 2.0f) vf_reset(pl, &a->world);
    vr_cam_t cam;
    vf_camera(pl, &a->world, dt, a->t, &cam);
    vr_proj_t pr;
    vr_setup(&cam, W, H, &pr);
    vp_pose_t pose;
    vf_pose(pl, &pose);
    pose.paint[0] = 0xF2E6C8;
    pose.paint[1] = 0xD8352A;
    pose.paint[2] = 0xF2E6C8;
    if (pl->view == VF_COCKPIT) a->draw.n = 0, a->draw.disc = false;
    else vp_prepare(&a->draw, &pose, &pr, a->world.sun, a->world.sky_hor);
    vr_prof_t prof;
    uint16_t *out = a->fr[slot];
    vr_frame(&a->world, &cam, &pr, out, pl->view == VF_COCKPIT ? NULL : over_fn, a, &prof);
    uint32_t t1 = vr_now_us();
    /* the instruments */
    if (a->lay.W != W || a->lay.H != H) vh_layout(&a->lay, W, H);
    vh_state_t *s = &a->hs;
    s->speed_kmh = pl->v * 3.6f;
    s->alt_m = pl->p[2];
    s->agl_m = pl->agl;
    s->thr = a->in_auto ? pl->in_thr : a->in_thr;
    float kx = (float)W / (float)a->SW, ky = (float)H / (float)a->SH;
    s->stick_on = a->stick_on;
    s->stick_bx = a->stick_bx * kx;
    s->stick_by = a->stick_by * ky;
    s->stick_x = a->in_auto ? pl->in_x : a->stick_x;
    s->stick_y = a->in_auto ? pl->in_y : a->stick_y;
    s->stall = pl->stall > 0.2f;
    s->crashed = pl->crashed;
    s->autopilot = a->in_auto;
    s->fps10 = a->fps10;
    s->res_w = W;
    s->res_h = H;
    s->pressed = a->btn_held;
    s->world = WORLD_HUD[a->biome % VW_BIOMES];
    vh_draw(&a->lay, s, out);
    uint32_t t2 = vr_now_us();
    a->fw[slot] = W;
    a->fh[slot] = H;
    a->fseq[slot] = ++a->seq;
    __atomic_store_n(&a->fst[slot], F_READY, __ATOMIC_RELEASE);
    /* the sums for the log line */
    a->perf_frames++;
    a->perf_frame_us += t2 - t0;
    a->perf_ground_us += prof.us_ground;
    a->perf_turn_us += prof.us_turn;
    a->perf_hud_us += t2 - t1;
    a->perf_samples += prof.samples;
    a->perf_lines += prof.lines;
    a->perf_upx += (uint64_t)prof.wu * (uint64_t)prof.hu;
}

static void worker_fn(void *arg)
{
    app_t *a = (app_t *)arg;
    vr_set_split(aos_hal_worker_split);
    uint32_t last = 0;
    while (!aos_hal_worker_should_stop()) {
        int req = __atomic_load_n(&a->gen_req, __ATOMIC_ACQUIRE);
        if (req >= 0) {
            make_world(a, req);
            last = 0;
            continue;
        }
        if (!a->world_ok || a->paused) {
            aos_hal_worker_sleep(10);
            last = 0;
            continue;
        }
        int i = free_slot(a);
        uint32_t now = vr_now_us();
        if (i < 0 || (last && now - last < FRAME_MIN_US)) {
            aos_hal_worker_sleep(1);
            continue;
        }
        float dt = last ? (float)(now - last) / 1e6f : 1.0f / 60.0f;
        if (dt > 0.05f) dt = 0.05f;
        last = now;
        __atomic_store_n(&a->fst[i], F_BUSY, __ATOMIC_RELEASE);
        fly_frame(a, i, dt);
    }
    vr_set_split(NULL);
}

/* --------------------------------------------------------------------------
 * Putting frames on the screen (LVGL's thread)
 * -------------------------------------------------------------------------- */

static void canvas_show(app_t *a, int i)
{
    if (!a->cv_buf) {
        a->cv_buf = (uint16_t *)vu_malloc((size_t)a->SW * a->SH * 2);
        if (!a->cv_buf) return;
        a->canvas = lv_canvas_create(a->root);
        lv_canvas_set_buffer(a->canvas, a->cv_buf, a->SW, a->SH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(a->canvas, 0, 0);
        lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_to_index(a->canvas, 0);
    }
    /* nearest, 16.16 */
    int W = a->fw[i], H = a->fh[i];
    const uint16_t *src = a->fr[i];
    uint32_t sx = ((uint32_t)W << 16) / (uint32_t)a->SW, sy = ((uint32_t)H << 16) / (uint32_t)a->SH;
    for (int y = 0; y < a->SH; y++) {
        const uint16_t *row = src + (size_t)((y * sy) >> 16) * W;
        uint16_t *d = a->cv_buf + (size_t)y * a->SW;
        uint32_t acc = 0;
        for (int x = 0; x < a->SW; x++, acc += sx) d[x] = row[acc >> 16];
    }
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(a->canvas);
}

static bool ppa_show(app_t *a, int i)
{
    uint16_t *back = aos_hal_display_back();
    if (!back) return false;
    int W = a->fw[i], H = a->fh[i];
    /* the largest whole sixteenth, centred */
    int k = (a->SW * 16) / W;
    if ((a->SH * 16) / H < k) k = (a->SH * 16) / H;
    int dw = W * k / 16, dh = H * k / 16;
    int x = (a->SW - dw) / 2, y = (a->SH - dh) / 2;
    if (!aos_hal_display_blit_into_fit(back, x, y, dw, dh, a->fr[i], W, H, W, false)) return false;
    return aos_hal_display_flip(back);
}

static void present(app_t *a)
{
    int best = -1;
    uint32_t bs = 0;
    for (int i = 0; i < NF; i++) {
        if (__atomic_load_n(&a->fst[i], __ATOMIC_ACQUIRE) == F_READY && (best < 0 || a->fseq[i] > bs)) {
            best = i;
            bs = a->fseq[i];
        }
    }
    if (best < 0) return;
    for (int i = 0; i < NF; i++)
        if (i != best && __atomic_load_n(&a->fst[i], __ATOMIC_ACQUIRE) == F_READY)
            __atomic_store_n(&a->fst[i], F_FREE, __ATOMIC_RELEASE);
    uint32_t t0 = vr_now_us();
    bool shown = false;
    if (a->ppa != 0) {
        shown = ppa_show(a, best);
        if (a->ppa < 0) {
            a->ppa = shown ? 1 : 0;
            aos_hal_log("vuelo", "frames through %s", shown ? "the PPA, flipped" : "the canvas");
        }
        if (shown) {
            /* the panel keeps its own copy: the frame is free at once */
            __atomic_store_n(&a->fst[best], F_FREE, __ATOMIC_RELEASE);
            if (a->canvas) lv_obj_add_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!shown) {
        canvas_show(a, best);
        if (a->shown >= 0 && a->shown != best) __atomic_store_n(&a->fst[a->shown], F_FREE, __ATOMIC_RELEASE);
        __atomic_store_n(&a->fst[best], F_SHOWN, __ATOMIC_RELEASE);
        a->shown = best;
    }
    a->perf_present_us += vr_now_us() - t0;
    a->perf_presents++;
}

static void perf_log(app_t *a)
{
    uint32_t now = aos_hal_uptime_ms();
    if (!a->perf_t0) {
        a->perf_t0 = now;
        return;
    }
    uint32_t el = now - a->perf_t0;
    if (el < PERF_MS) return;
    unsigned n = a->perf_frames ? a->perf_frames : 1, np = a->perf_presents ? a->perf_presents : 1;
    unsigned fps10 = a->perf_presents * 10000u / el;
    a->fps10 = (int)fps10;
    int W = RES[a->res][0], H = RES[a->res][1];
    unsigned upx = (unsigned)(a->perf_upx / n);
    aos_hal_log("vuelo", "%u.%u fps %dx%d (drawn %u, upright %u kpx), frame %u.%u ms (ground+sky %u.%u, turn+plane %u.%u, "
                "hud %u.%u), present %u.%u ms, %u k samples, %u lines, %s, %s",
                fps10 / 10, fps10 % 10, W, H, a->perf_frames, upx / 1000,
                (unsigned)(a->perf_frame_us / n / 1000), (unsigned)(a->perf_frame_us / n / 100 % 10),
                (unsigned)(a->perf_ground_us / n / 1000), (unsigned)(a->perf_ground_us / n / 100 % 10),
                (unsigned)(a->perf_turn_us / n / 1000), (unsigned)(a->perf_turn_us / n / 100 % 10),
                (unsigned)(a->perf_hud_us / n / 1000), (unsigned)(a->perf_hud_us / n / 100 % 10),
                (unsigned)(a->perf_present_us / np / 1000), (unsigned)(a->perf_present_us / np / 100 % 10),
                (unsigned)(a->perf_samples / n / 1000), (unsigned)(a->perf_lines / n), WORLD_HUD[a->biome % VW_BIOMES],
                a->pl.view == VF_COCKPIT ? "cockpit" : "chase");
    a->perf_t0 = now;
    a->perf_frames = a->perf_presents = 0;
    a->perf_frame_us = a->perf_ground_us = a->perf_turn_us = a->perf_hud_us = a->perf_present_us = 0;
    a->perf_samples = a->perf_lines = a->perf_upx = 0;
}

/* --------------------------------------------------------------------------
 * Loading
 * -------------------------------------------------------------------------- */

static void load_show(app_t *a, bool on)
{
    if (on && !a->load_box) {
        a->load_box = lv_obj_create(a->root);
        lv_obj_remove_style_all(a->load_box);
        lv_obj_set_size(a->load_box, 520, 150);
        lv_obj_center(a->load_box);
        lv_obj_set_style_bg_color(a->load_box, lv_color_hex(0x102030), 0);
        lv_obj_set_style_bg_opa(a->load_box, LV_OPA_90, 0);
        lv_obj_set_style_radius(a->load_box, 26, 0);
        lv_obj_remove_flag(a->load_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        a->load_lbl = lv_label_create(a->load_box);
        lv_obj_set_style_text_font(a->load_lbl, aos_font_body, 0);
        lv_obj_set_style_text_color(a->load_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(a->load_lbl, LV_ALIGN_TOP_MID, 0, 30);
        a->load_bar = lv_bar_create(a->load_box);
        lv_obj_set_size(a->load_bar, 420, 14);
        lv_obj_align(a->load_bar, LV_ALIGN_BOTTOM_MID, 0, -34);
        lv_obj_set_style_bg_color(a->load_bar, lv_color_hex(0x2A4258), 0);
        lv_obj_set_style_bg_color(a->load_bar, lv_color_hex(0xFF8A1E), LV_PART_INDICATOR);
        lv_bar_set_range(a->load_bar, 0, 100);
        /* the flipped frames hid LVGL's screen: draw it all again */
        lv_obj_invalidate(a->root);
    }
    if (!on && a->load_box) {
        lv_obj_delete(a->load_box);
        a->load_box = a->load_lbl = a->load_bar = NULL;
    }
}

static void load_update(app_t *a)
{
    if (!a->load_box) return;
    int req = a->gen_req;
    int biome = req >= 0 ? req >> 3 : a->biome;
    char b[96];
    snprintf(b, sizeof b, _("Armando el mundo: %s"), _(WORLD_NAME[biome % VW_BIOMES]));
    lv_label_set_text(a->load_lbl, b);
    lv_bar_set_value(a->load_bar, a->progress, LV_ANIM_OFF);
}

static void ask_world(app_t *a, int biome, int hour)
{
    /* the frames in flight are dropped: the worker makes the world first */
    a->world_ok = false;
    __atomic_store_n(&a->gen_req, (biome % VW_BIOMES) * 8 + (hour % VW_HOURS), __ATOMIC_RELEASE);
    a->ppa = a->ppa == 1 ? 1 : -1;
    load_show(a, true);
    load_update(a);
}

/* --------------------------------------------------------------------------
 * The hands: fingers, the gamepad, the keyboard
 * -------------------------------------------------------------------------- */

static void button(app_t *a, int b)
{
    switch (b) {
    case VH_BTN_WORLD: {
        int nb = (a->biome + 1) % VW_BIOMES;
        ask_world(a, nb, WORLD_HOUR[nb]);
        break;
    }
    case VH_BTN_VIEW: a->in_view_n++; break;
    case VH_BTN_AUTO: a->in_auto = !a->in_auto; break;
    default:
        a->res = (a->res + 1) % NRES;
        a->perf_t0 = 0;
        aos_hal_log("vuelo", "frame size %dx%d", RES[a->res][0], RES[a->res][1]);
        break;
    }
}

static void touch(app_t *a)
{
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    aos_touch_points_n(pts, AOS_TOUCH_MAX);
    int W = RES[a->res][0], H = RES[a->res][1];
    vh_layout_t L;
    vh_layout(&L, W, H);
    float kx = (float)W / (float)a->SW, ky = (float)H / (float)a->SH;
    bool stick = false;
    uint8_t held = 0;
    for (int i = 0; i < AOS_TOUCH_MAX; i++) {
        finger_t *f = &a->fing[i];
        const aos_touch_point_t *p = &pts[i];
        if (!p->down) {
            f->id = 0;
            f->role = R_NONE;
            continue;
        }
        float fx = p->x * kx, fy = p->y * ky;
        if (f->id != p->id || !f->id) {
            memset(f, 0, sizeof *f);
            f->id = p->id ? p->id : 0xFF;
            f->x0 = p->x;
            f->y0 = p->y;
            int pad = (int)(6 * L.u);
            for (int b = 0; b < VH_BTNS && !f->role; b++) {
                if (fx >= L.btn_x[b] - pad && fx < L.btn_x[b] + L.btn_s + pad && fy < L.btn_y + L.btn_s + pad) {
                    f->role = R_BTN;
                    f->btn = (uint8_t)b;
                    button(a, b);
                }
            }
            if (!f->role && fx >= L.thr_x0 - 24 * L.u && fy >= L.thr_y0 - 20 * L.u) f->role = R_THR;
            if (!f->role && fx < (float)W * 0.5f && fy > (float)H * 0.2f) {
                f->role = R_STICK;
                /* the base where the thumb landed, kept off the edges */
                float m = L.stick_r * 1.1f / kx;
                float bx = p->x < m ? m : p->x, by = p->y > (float)a->SH - m ? (float)a->SH - m : p->y;
                f->x0 = bx;
                f->y0 = by;
            }
        }
        switch (f->role) {
        case R_STICK: {
            float r = L.stick_r / kx;
            float dx = (p->x - f->x0) / r, dy = -(p->y - f->y0) / r;
            float d = sqrtf(dx * dx + dy * dy);
            if (d > 1) {
                dx /= d;
                dy /= d;
            }
            a->stick_bx = f->x0;
            a->stick_by = f->y0;
            a->stick_x = dx;
            a->stick_y = dy;
            stick = true;
            break;
        }
        case R_THR: {
            float t = 1.0f - (fy - (float)L.thr_y0) / (float)(L.thr_y1 - L.thr_y0);
            a->in_thr = t < 0 ? 0 : t > 1 ? 1 : t;
            break;
        }
        case R_BTN: held |= (uint8_t)(1u << f->btn); break;
        default: break;
        }
    }
    a->stick_on = stick;
    if (!stick) a->stick_x = a->stick_y = 0;
    a->btn_held = held;
}

static bool key_in(const uint8_t *k, int n, uint8_t u)
{
    for (int i = 0; i < n; i++)
        if (k[i] == u) return true;
    return false;
}

static void hands(app_t *a, float dt)
{
    touch(a);
    float x = a->stick_x, y = a->stick_y;
    /* the gamepad: the stick as the touch stick, the d-pad full over;
     * A and B the throttle, L the camera, R the autopilot, START the world */
    aos_pad_update(&a->pad, lv_tick_get());
    if (a->pad.connected) {
        if (a->pad.x || a->pad.y) {
            x = (float)a->pad.x / 32767.0f;
            y = -(float)a->pad.y / 32767.0f;
        }
        if (aos_pad_held(&a->pad, AOS_PAD_LEFT)) x = -1;
        if (aos_pad_held(&a->pad, AOS_PAD_RIGHT)) x = 1;
        if (aos_pad_held(&a->pad, AOS_PAD_UP)) y = 1;
        if (aos_pad_held(&a->pad, AOS_PAD_DOWN)) y = -1;
        float t = a->in_thr;
        if (aos_pad_held(&a->pad, AOS_PAD_A)) t += dt * 0.7f;
        if (aos_pad_held(&a->pad, AOS_PAD_B)) t -= dt * 0.7f;
        a->in_thr = t < 0 ? 0 : t > 1 ? 1 : t;
        if (aos_pad_pressed(&a->pad, AOS_PAD_L)) button(a, VH_BTN_VIEW);
        if (aos_pad_pressed(&a->pad, AOS_PAD_R)) button(a, VH_BTN_AUTO);
        if (aos_pad_pressed(&a->pad, AOS_PAD_START)) button(a, VH_BTN_WORLD);
    }
    /* a keyboard (the Mac's in the simulator): the arrows, W and S the
     * throttle, C the camera, P the autopilot, N the world, H the hour,
     * R the frame's size */
    uint8_t k[8], mods = 0;
    int n = aos_hal_usb_kbd_held(k, 8, &mods);
    if (n > 0 || a->nkeys_prev > 0) {
        if (key_in(k, n, 0x50)) x = -1;
        if (key_in(k, n, 0x4F)) x = 1;
        if (key_in(k, n, 0x52)) y = 1;
        if (key_in(k, n, 0x51)) y = -1;
        float t = a->in_thr;
        if (key_in(k, n, 0x1A)) t += dt * 0.7f;
        if (key_in(k, n, 0x16)) t -= dt * 0.7f;
        a->in_thr = t < 0 ? 0 : t > 1 ? 1 : t;
        static const uint8_t EDGE[] = { 0x06, 0x13, 0x11, 0x0B, 0x15 };
        for (size_t e = 0; e < sizeof EDGE; e++) {
            if (!key_in(k, n, EDGE[e]) || key_in(a->keys_prev, a->nkeys_prev, EDGE[e])) continue;
            switch (EDGE[e]) {
            case 0x06: button(a, VH_BTN_VIEW); break;
            case 0x13: button(a, VH_BTN_AUTO); break;
            case 0x11: button(a, VH_BTN_WORLD); break;
            case 0x0B: ask_world(a, a->biome, a->hour + 1); break;
            default: button(a, VH_BTN_RES); break;
            }
        }
        memcpy(a->keys_prev, k, sizeof k);
        a->nkeys_prev = n;
    }
    a->in_x = x;
    a->in_y = y;
}

/* --------------------------------------------------------------------------
 * The clock
 * -------------------------------------------------------------------------- */

static void frame_cb(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    if (a->closing) return;
    static uint32_t last;
    uint32_t now = lv_tick_get();
    float dt = last ? (float)(now - last) / 1000.0f : 0.008f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    bool over = (aos_ui_overlay() & ~(uint32_t)AOS_UI_OVER_TOAST) != 0;
    if (over != a->paused) {
        a->paused = over;
        if (!over) a->perf_t0 = 0;
    }
    if (over) return;
    hands(a, dt);
    if (a->gen_req >= 0 || !a->world_ok) {
        load_show(a, true);
        load_update(a);
        return;
    }
    if (a->load_box) {
        load_show(a, false);
        a->perf_t0 = 0;
    }
    present(a);
    perf_log(a);
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static void free_all(app_t *a)
{
    for (int i = 0; i < NF; i++) {
        vu_free(a->fr_mem[i]);
        a->fr_mem[i] = a->fr[i] = NULL;
    }
    vu_free(a->cv_buf);
    a->cv_buf = NULL;
    vw_free(&a->world);
    vr_free();
}

static void *vuelo_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)vu_calloc(sizeof(app_t));
    if (!a) return NULL;
    a->self = self;
    a->root = root;
    a->SW = lv_obj_get_width(root);
    a->SH = lv_obj_get_height(root);
    if (a->SW < a->SH) {
        int t = a->SW;
        a->SW = a->SH;
        a->SH = t;
    }
    uint32_t hi = 0, hp = 0;
    aos_hal_heap_info(&hi, &hp);
    bool ok = vw_alloc(&a->world) && vr_init(MAXW, MAXH);
    for (int i = 0; i < NF && ok; i++) {
        /* 64-byte aligned: the PPA reads them by DMA */
        a->fr_mem[i] = (uint16_t *)vu_malloc((size_t)MAXW * MAXH * 2 + 64);
        if (!a->fr_mem[i]) ok = false;
        else a->fr[i] = (uint16_t *)(((uintptr_t)a->fr_mem[i] + 63) & ~(uintptr_t)63);
    }
    if (!ok) {
        aos_hal_log("vuelo", "out of memory (psram free %u KB)", (unsigned)(hp / 1024));
        free_all(a);
        vu_free(a);
        return NULL;
    }
    a->shown = -1;
    a->ppa = -1;
    a->res = RES_DEFAULT;
    a->in_thr = 0.6f;
    a->gen_req = -1;
    a->seed = 7;
    int biome = VW_ISLAS, hour = -1;
#ifdef AOS_SIM_BUILTIN
    /* development: VU_WORLD=0..4, VU_HOUR=0..3, VU_SEED, VU_RES=0..3,
     * VU_AUTO=1 flies by itself, VU_VIEW=1 from the cockpit */
    if (getenv("VU_WORLD")) biome = atoi(getenv("VU_WORLD")) % VW_BIOMES;
    if (getenv("VU_HOUR")) hour = atoi(getenv("VU_HOUR")) % VW_HOURS;
    if (getenv("VU_SEED")) a->seed = (uint32_t)atoi(getenv("VU_SEED"));
    if (getenv("VU_RES")) a->res = atoi(getenv("VU_RES")) % NRES;
    if (getenv("VU_AUTO")) a->in_auto = true;
    if (getenv("VU_VIEW")) a->pl.view = atoi(getenv("VU_VIEW")) % VF_VIEWS;
#endif
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0A1420), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    vp_build();
    a->worker_ok = aos_hal_worker_start_on("vuelo", worker_fn, a, 12 * 1024, 0, 3);
    if (!a->worker_ok) {
        aos_hal_log("vuelo", "no worker");
        free_all(a);
        vu_free(a);
        return NULL;
    }
    ask_world(a, biome, hour >= 0 ? hour : WORLD_HOUR[biome]);
    a->timer = lv_timer_create(frame_cb, TICK_MS, a);
    aos_hal_display_refresh_request(60);
    aos_hal_log("vuelo", "ready %dx%d | psram free before %u KB, held %u KB", a->SW, a->SH, (unsigned)(hp / 1024),
                (unsigned)(vu_mem_used() / 1024));
    return a;
}

static void vuelo_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    a->closing = true;
    if (a->worker_ok) aos_hal_worker_stop();
    if (a->timer) lv_timer_delete(a->timer);
    aos_hal_display_refresh_request(0);
    if (a->root) lv_obj_clean(a->root);
    aos_hal_log("vuelo", "closing | peak %u KB", (unsigned)(vu_mem_peak() / 1024));
    free_all(a);
    vu_free(a);
}

static void vuelo_show(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a) a->perf_t0 = 0;
    aos_hal_display_refresh_request(60);
}

static void vuelo_hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    aos_hal_display_refresh_request(0);
}

/* The launcher icon: a small red plane climbing across a blue sky */
static const uint8_t VU_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER,   0,   4, 60, 12, AIC_CIRCLE, AIC_C_LIT(0xF2E6C8), 255),
    AIC_RECT(AIC_CENTER,  -2,   6, 16, 62, 6,          AIC_C_LIT(0xE0402E), 255),
    AIC_RECT(AIC_CENTER, -26,   2, 6,  24, 3,          AIC_C_LIT(0xE0402E), 255),
    AIC_RECT(AIC_CENTER,  30,   4, 6,  14, AIC_CIRCLE, AIC_C_LIT(0x404048), 255),
    AIC_END
};

static bool vuelo_init(aos_app_t *app)
{
    app->desc.id       = "demo.vuelo";
    app->desc.name     = "Vuelo";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, VU_ICON, sizeof VU_ICON);
    app->desc.color_a  = 0x3C8CE0;
    app->desc.color_b  = 0x9AD8F4;
    app->desc.order    = 166;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN | AOS_APP_FLAG_LANDSCAPE |
                         AOS_APP_FLAG_LONG_DRAG;

    app->create  = vuelo_create;
    app->destroy = vuelo_destroy;
    app->show    = vuelo_show;
    app->hide    = vuelo_hide;
    return true;
}

AOS_APP_ENTRY(vuelo_init);
