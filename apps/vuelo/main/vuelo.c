/*
 * PLANEO - a flight over worlds drawn live in 3D (see vu_app.h for the
 * map of the files).
 *
 * Missions in five worlds made from seeds (rings against the clock, a race
 * against a rival, packages to carry) and a free flight; records and stars.
 * Lying down only.
 *
 * Threads. The worker (core 0) makes the worlds and flies: it steps the
 * plane and the mission, draws the frame small (426 x 240 at the normal
 * quality) with both cores (vr_frame splits every pass) and leaves it in
 * one of three slots. LVGL's timer, every 8 ms, reads the fingers, the
 * gamepad and the keyboard, plays the sound, and puts the newest finished
 * frame on the panel: the PPA scales it up and turns it into the panel's
 * free buffer, which is then flipped to (aos_hal_display_blit_into_fit +
 * flip), while the worker already draws the next. The menus, the pause and
 * the results are LVGL panels over a still frame scaled into a canvas;
 * where there is no panel to flip (the simulator) the flight goes through
 * the canvas too.
 *
 * The log says every 5 s what a frame costs (on the board: /api/log).
 */
#include "vu_app.h"
#include "aos_icon_ops.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS      8
#define FRAME_MIN_US 16000          /* 60 fps at most: the panel's refresh */
#define PERF_MS      5000
#define RESULT_AFTER 2.4f           /* seconds from the finish to the results */

/* The frame's sizes: each scales by a whole number of sixteenths into the
 * 1280 x 720 screen (the PPA's steps); 426 is 1278 wide, a pixel short
 * each side. Measured on the board (2026-11-23, before the map's tiles):
 * 320 x 180 20-36 fps, 426 x 240 17-37, 512 x 288 14-15, 640 x 360 11-12. */
static const int RES[VA_QUALITIES][2] = { { 320, 180 }, { 426, 240 }, { 512, 288 } };

uint32_t vr_now_us(void)
{
    return (uint32_t)aos_hal_uptime_us();
}

static const char *const WORLD_LOG[VW_BIOMES] = { "islas", "canones", "nieve", "volcan", "ciudad" };

/* --------------------------------------------------------------------------
 * The worker
 * -------------------------------------------------------------------------- */

static void make_world(app_t *a, int req)
{
    int biome = req >> 3, hour = req & 7;
    uint32_t t0 = vr_now_us();
    a->progress = 0;
    bool ok = vw_generate(&a->world, biome, 7u + (uint32_t)biome * 101u, hour, aos_hal_worker_split, &a->progress);
    uint32_t ms = (vr_now_us() - t0) / 1000;
    aos_hal_log("vuelo", "world %s hour %d made in %u ms (heights %u, trees+shadows %u, colours %u, levels %u), "
                "highest %d m | psram held %u KB",
                WORLD_LOG[biome % VW_BIOMES], hour, (unsigned)ms, (unsigned)a->world.gen_ms[0],
                (unsigned)a->world.gen_ms[1], (unsigned)a->world.gen_ms[2], (unsigned)a->world.gen_ms[3],
                (int)a->world.hmax, (unsigned)(vu_mem_used() / 1024));
    a->biome = biome;
    a->hour = hour;
    if (!a->flying) vf_reset(&a->pl, &a->world);
    /* only the request this was: another may have come while it was made */
    int expect = req;
    if (__atomic_compare_exchange_n(&a->gen_req, &expect, -1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) a->world_ok = ok;
}

static int free_slot(app_t *a)
{
    for (int i = 0; i < VA_NF; i++)
        if (__atomic_load_n(&a->fst[i], __ATOMIC_ACQUIRE) == F_FREE) return i;
    return -1;
}

static void over_fn(void *arg, uint16_t *out, int W, int H, int y0, int y1)
{
    app_t *a = (app_t *)arg;
    vm_draw(&a->marks, out, W, H, y0, y1);
    /* the two planes, the farther first */
    const vp_draw_t *first = a->rival_first ? &a->draw2 : &a->draw, *second = a->rival_first ? &a->draw : &a->draw2;
    vp_draw(first, out, W, H, y0, y1);
    vp_draw(second, out, W, H, y0, y1);
}

static const uint32_t PAINT[3] = { 0xF2E6C8, 0xD8352A, 0xF2E6C8 };
static const uint32_t RIVAL_PAINT[3] = { 0x2C3E66, 0xF2C230, 0x2C3E66 };

/* A frame of the flight: the camera, the planes, the marks, the instruments.
 * still: for the menus and the pause (no instruments; in the menus a view
 * from beside the plane). */
static void render(app_t *a, int slot, float dt, bool still)
{
    uint32_t t0 = vr_now_us();
    int W = a->W, H = a->H;
    vf_plane_t *pl = &a->pl;
    vr_cam_t cam;
    bool menu = still && !a->flying;
    if (menu) {
        /* beside and a little behind, looking along the plane */
        float eye[3], f[3] = { pl->f[0], pl->f[1], pl->f[2] };
        for (int k = 0; k < 3; k++) eye[k] = pl->p[k] - f[k] * 13.0f + pl->r[k] * 9.0f;
        eye[2] += 3.0f;
        float tgt[3] = { pl->p[0] + f[0] * 18.0f, pl->p[1] + f[1] * 18.0f, pl->p[2] + f[2] * 18.0f };
        float d[3] = { tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2] };
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        cam.x = vw_wrap(eye[0]);
        cam.y = vw_wrap(eye[1]);
        cam.z = eye[2];
        cam.yaw = atan2f(d[1], d[0]);
        cam.pitch = asinf(d[2] / l);
        cam.roll = 0.08f;
        cam.tanfov = 0.78f;
        cam.time = a->t;
    } else {
        vf_camera(pl, &a->world, still ? 0 : dt, a->t, &cam);
    }
    vr_proj_t pr;
    vr_setup(&cam, W, H, &pr);
    vp_pose_t pose;
    vf_pose(pl, &pose);
    memcpy(pose.paint, PAINT, sizeof pose.paint);
    a->draw.n = 0;
    a->draw.disc = false;
    a->draw2.n = 0;
    a->draw2.disc = false;
    if (pl->view != VF_COCKPIT || menu) vp_prepare(&a->draw, &pose, &pr, a->world.sun, a->world.sky_hor);
    vg_game_t *g = &a->game;
    if (a->flying && vg_rival(g)) {
        vp_pose_t rp;
        memcpy(rp.p, g->rival_p, sizeof rp.p);
        memcpy(rp.f, g->rival_f, sizeof rp.f);
        memcpy(rp.r, g->rival_r, sizeof rp.r);
        memcpy(rp.u, g->rival_u, sizeof rp.u);
        rp.prop = 0.9f;
        rp.ground = vw_height(&a->world, rp.p[0], rp.p[1]);
        memcpy(rp.paint, RIVAL_PAINT, sizeof rp.paint);
        vp_prepare(&a->draw2, &rp, &pr, a->world.sun, a->world.sky_hor);
        float d1 = 0, d2 = 0;
        for (int k = 0; k < 2; k++) {
            float e = k ? cam.y : cam.x;
            float q1 = pl->p[k] - e, q2 = rp.p[k] - e;
            if (q1 > VW_SIZE / 2) q1 -= VW_SIZE;
            if (q1 < -VW_SIZE / 2) q1 += VW_SIZE;
            if (q2 > VW_SIZE / 2) q2 -= VW_SIZE;
            if (q2 < -VW_SIZE / 2) q2 += VW_SIZE;
            d1 += q1 * q1;
            d2 += q2 * q2;
        }
        a->rival_first = d2 > d1;
    }
    if (a->flying) {
        vm_prepare(&a->marks, g, &a->world, &pr, a->t);
    } else {
        a->marks.n = 0;
        a->marks.target = false;
    }
    vr_prof_t prof;
    uint16_t *out = a->fr[slot];
    vr_frame(&a->world, &cam, &pr, out, over_fn, a, &prof);
    uint32_t t1 = vr_now_us();
    if (!still) {
        if (a->lay.W != W || a->lay.H != H) vh_layout(&a->lay, W, H);
        vh_state_t *s = &a->hs;
        s->speed_kmh = pl->v * 3.6f;
        s->alt_m = pl->p[2];
        s->agl_m = pl->agl;
        s->thr = a->in_thr;
        float kx = (float)W / (float)a->SW, ky = (float)H / (float)a->SH;
        s->stick_on = a->stick_on;
        s->stick_bx = a->stick_bx * kx;
        s->stick_by = a->stick_by * ky;
        s->stick_x = a->stick_x;
        s->stick_y = a->stick_y;
        s->rud = pl->in_rud;
        s->rud_on = a->rud_on;
        s->stall = pl->stall > 0.2f;
        s->crashed = pl->crashed;
        s->pressed = a->btn_held;
        s->kind = g->kind;
        s->next = g->next;
        s->npt = g->npt;
        s->done = g->state == VG_ST_DONE;
        s->clock = s->done ? g->final_t : g->t + g->penalty;
        if (g->state == VG_ST_COUNT) s->clock = g->t;
        s->carrying = g->carrying;
        s->place = 0;
        if (g->kind == VG_RACE && g->state != VG_ST_COUNT) {
            if (s->done) {
                s->place = g->won ? 1 : 2;
            } else {
                /* ahead of the rival: further along the course */
                float mine = 0;
                for (int i = 0; i < g->next && i < g->npt; i++) mine += g->seglen[i];
                if (g->next < g->npt) {
                    const vg_pt_t *c = &g->pt[g->next];
                    float dx = c->p[0] - pl->p[0], dy = c->p[1] - pl->p[1];
                    if (dx > VW_SIZE / 2) dx -= VW_SIZE;
                    if (dx < -VW_SIZE / 2) dx += VW_SIZE;
                    if (dy > VW_SIZE / 2) dy -= VW_SIZE;
                    if (dy < -VW_SIZE / 2) dy += VW_SIZE;
                    float left = sqrtf(dx * dx + dy * dy);
                    float seg = g->seglen[g->next];
                    mine += seg - (left < seg ? left : seg);
                }
                s->place = mine >= g->rival_v * g->t ? 1 : 2;
            }
        }
        s->banner = g->banner;
        s->banner_t = g->banner_t;
        s->target = a->marks.target;
        s->target_on = a->marks.on_screen;
        s->tx = a->marks.tx;
        s->ty = a->marks.ty;
        s->tdist = a->marks.dist;
        s->tdx = a->marks.dir_x;
        s->tdy = a->marks.dir_y;
        s->fps10 = a->cfg.fps ? a->fps10 : 0;
        s->res_w = W;
        s->res_h = H;
        vh_draw(&a->lay, s, &a->text, out);
    }
    uint32_t t2 = vr_now_us();
    a->fw[slot] = W;
    a->fh[slot] = H;
    a->fstill[slot] = still;
    a->fseq[slot] = ++a->seq;
    __atomic_store_n(&a->fst[slot], F_READY, __ATOMIC_RELEASE);
    if (!still) {
        a->perf_frames++;
        a->perf_frame_us += t2 - t0;
        a->perf_ground_us += prof.us_ground;
        a->perf_turn_us += prof.us_turn;
        a->perf_hud_us += t2 - t1;
        a->perf_samples += prof.samples;
        a->perf_lines += prof.lines;
        a->perf_upx += (uint64_t)prof.wu * (uint64_t)prof.hu;
    }
}

static void step(app_t *a, float dt)
{
    vf_plane_t *pl = &a->pl;
    vg_game_t *g = &a->game;
    pl->in_x = a->in_x;
    pl->in_y = a->in_y;
    pl->in_thr = a->in_thr;
    pl->in_rud = a->in_rud;
    /* development (planeo_dev.txt with "auto", or VU_AUTO=1 in the
     * simulator): the plane flies by itself, to measure without hands */
    if (a->dev_auto && g->state == VG_ST_FLY) {
        if (g->kind == VG_FREE || g->next >= g->npt) {
            vf_autopilot(pl, &a->world, a->t);
        } else {
            float tgt[3];
            memcpy(tgt, g->pt[g->next].p, sizeof tgt);
            if (g->kind == VG_DELIVER) {
                tgt[2] += 20.0f;
            } else {
                /* from afar, a point on the ring's axis before it; near, the ring */
                float dx = g->pt[g->next].p[0] - pl->p[0], dy = g->pt[g->next].p[1] - pl->p[1];
                if (dx > VW_SIZE / 2) dx -= VW_SIZE;
                if (dx < -VW_SIZE / 2) dx += VW_SIZE;
                if (dy > VW_SIZE / 2) dy -= VW_SIZE;
                if (dy < -VW_SIZE / 2) dy += VW_SIZE;
                float along = -(dx * g->pt[g->next].n[0] + dy * g->pt[g->next].n[1]);
                if (dx * dx + dy * dy > 150.0f * 150.0f || along < 30.0f)
                    for (int k = 0; k < 2; k++) tgt[k] -= g->pt[g->next].n[k] * 100.0f;
            }
            vf_autopilot_to(pl, &a->world, tgt);
        }
    }
    int vn = a->in_view_n;
    if (vn != a->view_seen) {
        a->view_seen = vn;
        pl->view = (pl->view + 1) % VF_VIEWS;
        pl->cam_ok = false;
    }
    float left = dt;
    while (left > 0) {
        float h = left > 1.0f / 60.0f ? 1.0f / 60.0f : left;
        if (g->state == VG_ST_COUNT) {
            /* the countdown: the plane hangs where it starts, the engine running */
            pl->thr = a->in_thr;
            vg_step(g, &a->world, pl, pl->p, h);
        } else {
            float p0[3] = { pl->p[0], pl->p[1], pl->p[2] };
            vf_step(pl, &a->world, h);
            vg_step(g, &a->world, pl, p0, h);
        }
        left -= h;
    }
    a->t += dt;
    if (g->events) {
        __atomic_fetch_or(&a->events, g->events, __ATOMIC_RELAXED);
        g->events = 0;
    }
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
        if (!a->world_ok) {
            aos_hal_worker_sleep(10);
            continue;
        }
        if (__atomic_load_n(&a->start_req, __ATOMIC_ACQUIRE)) {
            vg_start(&a->game, &a->world, a->mission, &a->pl);
            a->pl.view = a->cfg.view;
            a->view_seen = a->in_view_n;
            a->t = 0;
            a->flying = true;
            last = 0;
            __atomic_store_n(&a->start_req, 0, __ATOMIC_RELEASE);
        }
        int i = free_slot(a);
        if (__atomic_load_n(&a->still_req, __ATOMIC_ACQUIRE)) {
            if (i < 0) {
                aos_hal_worker_sleep(2);
                continue;
            }
            __atomic_store_n(&a->fst[i], F_BUSY, __ATOMIC_RELEASE);
            render(a, i, 0, true);
            __atomic_store_n(&a->still_req, 0, __ATOMIC_RELEASE);
            continue;
        }
        if (!a->flying || a->paused || a->hold) {
            aos_hal_worker_sleep(10);
            last = 0;
            continue;
        }
        uint32_t now = vr_now_us();
        if (i < 0 || (last && now - last < FRAME_MIN_US)) {
            aos_hal_worker_sleep(1);
            continue;
        }
        float dt = last ? (float)(now - last) / 1e6f : 1.0f / 60.0f;
        if (dt > 0.05f) dt = 0.05f;
        last = now;
        __atomic_store_n(&a->fst[i], F_BUSY, __ATOMIC_RELEASE);
        step(a, dt);
        render(a, i, dt, false);
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
        lv_canvas_set_buffer(a->canvas, a->cv_buf, a->SW, a->SH, LV_COLOR_FORMAT_RGB565);
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
    for (int i = 0; i < VA_NF; i++) {
        if (__atomic_load_n(&a->fst[i], __ATOMIC_ACQUIRE) == F_READY && (best < 0 || a->fseq[i] > bs)) {
            best = i;
            bs = a->fseq[i];
        }
    }
    if (best < 0) return;
    for (int i = 0; i < VA_NF; i++)
        if (i != best && __atomic_load_n(&a->fst[i], __ATOMIC_ACQUIRE) == F_READY)
            __atomic_store_n(&a->fst[i], F_FREE, __ATOMIC_RELEASE);
    /* a flight frame that came after the flight stopped: dropped */
    if (!a->fstill[best] && a->state != VA_FLY) {
        __atomic_store_n(&a->fst[best], F_FREE, __ATOMIC_RELEASE);
        return;
    }
    uint32_t t0 = vr_now_us();
    bool shown = false;
    /* a flight frame with nothing of LVGL's over it: through the PPA */
    if (!a->fstill[best] && a->ppa != 0) {
        shown = ppa_show(a, best);
        if (a->ppa < 0) {
            a->ppa = shown ? 1 : 0;
            aos_hal_log("vuelo", "frames through %s", shown ? "the PPA, flipped" : "the canvas");
        }
        if (shown) {
            /* the panel keeps its own copy: the frame is free at once */
            __atomic_store_n(&a->fst[best], F_FREE, __ATOMIC_RELEASE);
            if (!lv_obj_has_flag(a->canvas, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!shown) {
        canvas_show(a, best);
        if (a->shown >= 0 && a->shown != best) __atomic_store_n(&a->fst[a->shown], F_FREE, __ATOMIC_RELEASE);
        __atomic_store_n(&a->fst[best], F_SHOWN, __ATOMIC_RELEASE);
        a->shown = best;
    }
    if (!a->fstill[best]) {
        a->perf_present_us += vr_now_us() - t0;
        a->perf_presents++;
    }
}

/* LVGL marked an area to draw again: over a flipped frame that shows as a
 * patch of the root's colour until the next flip. Gathered for the log
 * (two such patches were seen to flicker once, 2026-11-23). */
static void inval_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    const lv_area_t *ar = (const lv_area_t *)lv_event_get_param(e);
    if (!a || !ar || a->closing || a->state != VA_FLY) return;
    a->inv_count++;
    for (int i = 0; i < a->ninv; i++)
        if (a->inv[i].x1 == ar->x1 && a->inv[i].y1 == ar->y1 && a->inv[i].x2 == ar->x2 && a->inv[i].y2 == ar->y2) return;
    if (a->ninv < 6) a->inv[a->ninv++] = *ar;
}

static void perf_reset(app_t *a, uint32_t now)
{
    a->perf_t0 = now;
    a->perf_frames = a->perf_presents = 0;
    a->perf_frame_us = a->perf_ground_us = a->perf_turn_us = a->perf_hud_us = a->perf_present_us = 0;
    a->perf_samples = a->perf_lines = a->perf_upx = 0;
}

static void perf_log(app_t *a)
{
    uint32_t now = aos_hal_uptime_ms();
    if (!a->perf_t0) {
        perf_reset(a, now);
        return;
    }
    uint32_t el = now - a->perf_t0;
    if (el < PERF_MS) return;
    unsigned n = a->perf_frames ? a->perf_frames : 1, np = a->perf_presents ? a->perf_presents : 1;
    unsigned fps10 = a->perf_presents * 10000u / el;
    a->fps10 = (int)fps10;
    unsigned upx = (unsigned)(a->perf_upx / n);
    aos_hal_log("vuelo", "%u.%u fps %dx%d (drawn %u, upright %u kpx), frame %u.%u ms (ground+sky %u.%u, turn+over %u.%u, "
                "hud %u.%u), present %u.%u ms, %u k samples, %u lines, %s, mission %d",
                fps10 / 10, fps10 % 10, a->W, a->H, a->perf_frames, upx / 1000,
                (unsigned)(a->perf_frame_us / n / 1000), (unsigned)(a->perf_frame_us / n / 100 % 10),
                (unsigned)(a->perf_ground_us / n / 1000), (unsigned)(a->perf_ground_us / n / 100 % 10),
                (unsigned)(a->perf_turn_us / n / 1000), (unsigned)(a->perf_turn_us / n / 100 % 10),
                (unsigned)(a->perf_hud_us / n / 1000), (unsigned)(a->perf_hud_us / n / 100 % 10),
                (unsigned)(a->perf_present_us / np / 1000), (unsigned)(a->perf_present_us / np / 100 % 10),
                (unsigned)(a->perf_samples / n / 1000), (unsigned)(a->perf_lines / n), WORLD_LOG[a->biome % VW_BIOMES],
                a->mission);
#ifdef AOS_SIM_BUILTIN
    if (getenv("VU_AUTO") && a->game.next < a->game.npt) {
        const vg_pt_t *c = &a->game.pt[a->game.next];
        float dx = c->p[0] - a->pl.p[0], dy = c->p[1] - a->pl.p[1];
        if (dx > VW_SIZE / 2) dx -= VW_SIZE;
        if (dx < -VW_SIZE / 2) dx += VW_SIZE;
        if (dy > VW_SIZE / 2) dy -= VW_SIZE;
        if (dy < -VW_SIZE / 2) dy += VW_SIZE;
        aos_hal_log("vuelo", "dbg next %d dist %d dz %d bearing %d heading %d bank %d v %d", a->game.next,
                    (int)sqrtf(dx * dx + dy * dy), (int)(c->p[2] - a->pl.p[2]), (int)(atan2f(dy, dx) * 57.3f),
                    (int)(a->pl.heading * 57.3f), (int)(a->pl.bank * 57.3f), (int)a->pl.v);
    }
#endif
    if (a->inv_count) {
        char b[200];
        int o = 0;
        for (int i = 0; i < a->ninv && o < (int)sizeof b - 32; i++)
            o += snprintf(b + o, sizeof b - (size_t)o, " %d,%d-%d,%d", (int)a->inv[i].x1, (int)a->inv[i].y1,
                          (int)a->inv[i].x2, (int)a->inv[i].y2);
        aos_hal_log("vuelo", "LVGL redrew %d areas over the flight:%s", a->inv_count, b);
        a->inv_count = a->ninv = 0;
    }
    perf_reset(a, now);
}

/* --------------------------------------------------------------------------
 * Loading
 * -------------------------------------------------------------------------- */

static void load_show(app_t *a, bool on)
{
    if (on && !a->load_box) {
        a->load_box = lv_obj_create(a->root);
        lv_obj_remove_style_all(a->load_box);
        lv_obj_set_size(a->load_box, 560, 150);
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
        lv_obj_set_size(a->load_bar, 460, 14);
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
    snprintf(b, sizeof b, _("Armando el mundo: %s"), _(VN_WORLD_NAME[biome % VW_BIOMES]));
    lv_label_set_text(a->load_lbl, b);
    lv_bar_set_value(a->load_bar, a->progress, LV_ANIM_OFF);
}

static void ask_world(app_t *a, int biome, int hour)
{
    biome %= VW_BIOMES;
    hour %= VW_HOURS;
    if (a->world_ok && a->gen_req < 0 && a->biome == biome && a->hour == hour) return;
    a->world_ok = false;
    __atomic_store_n(&a->gen_req, biome * 8 + hour, __ATOMIC_RELEASE);
}

/* --------------------------------------------------------------------------
 * The flights, for the screens
 * -------------------------------------------------------------------------- */

/* planeo_dev.txt in the data folder: "auto" flies by itself */
static void dev_read(app_t *a)
{
    char path[160];
    snprintf(path, sizeof path, "%s/planeo_dev.txt", aos_hal_path_data());
    FILE *f = fopen(path, "r");
    char b[64] = "";
    if (f) {
        size_t n = fread(b, 1, sizeof b - 1, f);
        b[n] = 0;
        fclose(f);
    }
    a->dev_auto = strstr(b, "auto") != NULL;
#ifdef AOS_SIM_BUILTIN
    if (getenv("VU_AUTO")) a->dev_auto = true;
#endif
    if (a->dev_auto) aos_hal_log("vuelo", "development: the plane flies by itself");
}

static void flight_begin(app_t *a)
{
    dev_read(a);
    vn_close(a);
    a->state = VA_FLY;
    a->paused = false;
    a->result_up = false;
    a->done_t = 0;
    a->was_stall = false;
    a->perf_t0 = 0;
    a->events = 0;
    __atomic_store_n(&a->start_req, 1, __ATOMIC_RELEASE);
    aos_hal_display_refresh_request(60);
}

void va_start_mission(app_t *a, int mission)
{
    int b, h;
    vg_mission_world(mission, &b, &h);
    a->flying = false;
    a->mission = mission;
    ask_world(a, b, h);
    flight_begin(a);
}

void va_start_free(app_t *a, int biome, int hour)
{
    a->flying = false;
    a->mission = -1;
    ask_world(a, biome, hour);
    flight_begin(a);
}

void va_restart(app_t *a)
{
    a->flying = false;
    flight_begin(a);
}

void va_pause(app_t *a)
{
    if (a->state != VA_FLY || !a->flying) return;
    a->state = VA_PAUSE;
    a->paused = true;
    __atomic_store_n(&a->still_req, 1, __ATOMIC_RELEASE);
    vn_show(a, VN_PAUSE);
}

void va_resume(app_t *a)
{
    vn_close(a);
    a->state = VA_FLY;
    a->paused = false;
    a->perf_t0 = 0;
}

void va_to_menu(app_t *a)
{
    a->flying = false;
    a->paused = false;
    a->state = VA_MENU;
    if (a->world_ok) vf_reset(&a->pl, &a->world);
    __atomic_store_n(&a->still_req, 1, __ATOMIC_RELEASE);
    vn_show(a, VN_MAIN);
}

bool va_world_open(app_t *a, int biome)
{
    return vg_total_stars(&a->save) >= vg_world_needs(biome);
}

void va_cfg_store(app_t *a)
{
    aos_hal_pref_set_i32("pl_snd", a->cfg.sound);
    aos_hal_pref_set_i32("pl_inv", a->cfg.invert);
    aos_hal_pref_set_i32("pl_q", a->cfg.quality);
    aos_hal_pref_set_i32("pl_cam", a->cfg.view);
    aos_hal_pref_set_i32("pl_fps", a->cfg.fps);
    vs_enable(a->cfg.sound);
}

void va_quality(app_t *a, int q)
{
    q = q < 0 ? 0 : q >= VA_QUALITIES ? VA_QUALITIES - 1 : q;
    a->cfg.quality = q;
    a->W = RES[q][0];
    a->H = RES[q][1];
    /* the words at the frame's new size (the worker is not flying here) */
    vt_build(&a->text, a->H);
    vh_layout(&a->lay, a->W, a->H);
}

static void cfg_load(app_t *a)
{
    int32_t v;
    a->cfg.sound = !aos_hal_pref_get_i32("pl_snd", &v) || v;
    a->cfg.invert = aos_hal_pref_get_i32("pl_inv", &v) && v;
    a->cfg.quality = aos_hal_pref_get_i32("pl_q", &v) && v >= 0 && v < VA_QUALITIES ? v : 1;
    a->cfg.view = aos_hal_pref_get_i32("pl_cam", &v) && v >= 0 && v < VF_VIEWS ? v : VF_CHASE;
    a->cfg.fps = aos_hal_pref_get_i32("pl_fps", &v) && v;
}

/* --------------------------------------------------------------------------
 * The hands: fingers, the gamepad, the keyboard (in flight)
 * -------------------------------------------------------------------------- */

static void button(app_t *a, int b)
{
    vs_cue(VS_CLICK);
    if (b == VH_BTN_PAUSE) va_pause(a);
    else a->in_view_n++;
}

static void touch(app_t *a)
{
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    aos_touch_points_n(pts, AOS_TOUCH_MAX);
    int W = a->W, H = a->H;
    vh_layout_t L;
    vh_layout(&L, W, H);
    float kx = (float)W / (float)a->SW, ky = (float)H / (float)a->SH;
    bool stick = false, rud = false;
    uint8_t held = 0;
    for (int i = 0; i < AOS_TOUCH_MAX; i++) {
        va_finger_t *f = &a->fing[i];
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
            int pad = (int)(8 * L.u);
            for (int b = 0; b < VH_BTNS && !f->role; b++) {
                if (fx >= L.btn_x[b] - pad && fx < L.btn_x[b] + L.btn_s + pad && fy < L.btn_y + L.btn_s + pad) {
                    f->role = R_BTN;
                    f->btn = (uint8_t)b;
                    /* LVGL may have taken this same press already */
                    if (!(a->lv_btn_ms && lv_tick_get() - a->lv_btn_ms < 400)) {
                        a->raw_btn_ms = lv_tick_get();
                        button(a, b);
                    }
                }
            }
            if (!f->role && fx >= L.rud_x0 - 12 * L.u && fx < L.rud_x1 + 8 * L.u && fy >= L.rud_y - 20 * L.u)
                f->role = R_RUD;
            if (!f->role && fx >= L.thr_x0 - 24 * L.u && fy >= L.thr_y0 - 20 * L.u) f->role = R_THR;
            if (!f->role && fx < (float)W * 0.5f && fy > (float)H * 0.2f) {
                f->role = R_STICK;
                /* the base where the thumb landed, kept off the edges */
                float m = L.stick_r * 1.1f / kx;
                f->x0 = p->x < m ? m : p->x;
                f->y0 = p->y > (float)a->SH - m ? (float)a->SH - m : p->y;
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
        case R_RUD: {
            /* from where the thumb landed, like a pedal: half the track is full over */
            float half = (float)(L.rud_x1 - L.rud_x0) * 0.5f / kx;
            float r = (p->x - f->x0) / half * 1.6f;
            a->rud_touch = r < -1 ? -1 : r > 1 ? 1 : r;
            rud = true;
            break;
        }
        case R_BTN: held |= (uint8_t)(1u << f->btn); break;
        default: break;
        }
    }
    a->stick_on = stick;
    if (!stick) a->stick_x = a->stick_y = 0;
    a->rud_on = rud;
    if (!rud) a->rud_touch = 0;
    a->btn_held = held;
}

/* The buttons through LVGL as well: a touch the portal injects (POST
 * /api/touch) never reaches the panel's raw samples. A finger on the panel
 * reaches both: whichever of the two sees the press first takes it. */
static void tap_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->closing || a->state != VA_FLY) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    uint32_t now = lv_tick_get();
    if (a->raw_btn_ms && now - a->raw_btn_ms < 400) return;
    vh_layout_t L;
    vh_layout(&L, a->W, a->H);
    float fx = (float)pt.x * (float)a->W / (float)a->SW, fy = (float)pt.y * (float)a->H / (float)a->SH;
    int pad = (int)(8 * L.u);
    for (int b = 0; b < VH_BTNS; b++) {
        if (fx >= L.btn_x[b] - pad && fx < L.btn_x[b] + L.btn_s + pad && fy < L.btn_y + L.btn_s + pad) {
            a->lv_btn_ms = now;
            button(a, b);
            return;
        }
    }
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
    float x = a->stick_x, y = a->stick_y, r = a->rud_touch;
    /* the gamepad: the stick as the touch stick, the d-pad full over; A and
     * B the throttle, L and R the pedals (both at once: the camera), START
     * the pause */
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
        bool pl_ = aos_pad_held(&a->pad, AOS_PAD_L), pr_ = aos_pad_held(&a->pad, AOS_PAD_R);
        if (pl_ && pr_) {
            if (aos_pad_pressed(&a->pad, AOS_PAD_L | AOS_PAD_R)) button(a, VH_BTN_VIEW);
        } else if (pl_) {
            r = -1;
        } else if (pr_) {
            r = 1;
        }
        if (aos_pad_pressed(&a->pad, AOS_PAD_START)) button(a, VH_BTN_PAUSE);
    }
    /* a keyboard (the Mac's in the simulator): the arrows, W and S the
     * throttle, A and D the pedals, C the camera, P the pause */
    uint8_t k[8], mods = 0;
    int n = aos_hal_usb_kbd_held(k, 8, &mods);
    if (n > 0 || a->nkeys_prev > 0) {
        if (key_in(k, n, 0x50)) x = -1;
        if (key_in(k, n, 0x4F)) x = 1;
        if (key_in(k, n, 0x52)) y = 1;
        if (key_in(k, n, 0x51)) y = -1;
        if (key_in(k, n, 0x04)) r = -1;
        if (key_in(k, n, 0x07)) r = 1;
        float t = a->in_thr;
        if (key_in(k, n, 0x1A)) t += dt * 0.7f;
        if (key_in(k, n, 0x16)) t -= dt * 0.7f;
        a->in_thr = t < 0 ? 0 : t > 1 ? 1 : t;
        if (key_in(k, n, 0x06) && !key_in(a->keys_prev, a->nkeys_prev, 0x06)) button(a, VH_BTN_VIEW);
        if (key_in(k, n, 0x13) && !key_in(a->keys_prev, a->nkeys_prev, 0x13)) button(a, VH_BTN_PAUSE);
        memcpy(a->keys_prev, k, sizeof k);
        a->nkeys_prev = n;
    }
    a->in_x = x;
    a->in_y = a->cfg.invert ? -y : y;
    a->in_rud = r;
}

/* --------------------------------------------------------------------------
 * The clock
 * -------------------------------------------------------------------------- */

static void sounds(app_t *a)
{
    uint32_t ev = __atomic_exchange_n(&a->events, 0, __ATOMIC_ACQ_REL);
    const vg_game_t *g = &a->game;
    if (ev & VG_EV_COUNT) vs_cue(VS_COUNT);
    if (ev & VG_EV_GO) vs_cue(VS_GO);
    if (ev & VG_EV_RING) vs_cue(VS_RING);
    if (ev & VG_EV_PICK) vs_cue(VS_PICK);
    if (ev & VG_EV_DROP) vs_cue(VS_DROP);
    if (ev & VG_EV_CRASH) vs_cue(VS_CRASH);
    if (ev & VG_EV_FINISH) vs_cue(g->kind == VG_RACE && !g->won ? VS_LOST : VS_FINISH);
    bool stall = a->pl.stall > 0.2f && !a->pl.crashed && a->state == VA_FLY;
    if (stall && !a->was_stall) vs_cue(VS_STALL);
    a->was_stall = stall;
    bool fly = a->state == VA_FLY && a->flying && !a->pl.crashed;
    vs_set(a->pl.thr, a->pl.v, fly);
}

static void results(app_t *a, float dt)
{
    vg_game_t *g = &a->game;
    if (a->state != VA_FLY || !a->flying || g->state != VG_ST_DONE || a->result_up) return;
    a->done_t += dt;
    if (a->done_t < RESULT_AFTER) return;
    a->result_up = true;
    int m = a->mission;
    a->result_stars = g->stars;
    a->result_time = g->final_t;
    a->result_record = false;
    if (m >= 0 && m < VG_MISSIONS) {
        uint32_t ms = (uint32_t)(g->final_t * 1000.0f);
        if (!a->save.best_ms[m] || ms < a->save.best_ms[m]) {
            a->result_record = a->save.best_ms[m] != 0;
            a->save.best_ms[m] = ms;
        }
        if (g->stars > a->save.stars[m]) a->save.stars[m] = (uint8_t)g->stars;
        vg_store(&a->save, m);
        aos_hal_log("vuelo", "mission %d done in %d.%d s, %d stars%s", m, (int)g->final_t,
                    (int)(g->final_t * 10.0f) % 10, g->stars, a->result_record ? ", a record" : "");
    }
    a->state = VA_RESULT;
    a->paused = true;
    __atomic_store_n(&a->still_req, 1, __ATOMIC_RELEASE);
    vn_show(a, VN_RESULT);
}

static void frame_cb(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    if (a->closing) return;
    static uint32_t last;
    uint32_t now = lv_tick_get();
    float dt = last ? (float)(now - last) / 1000.0f : 0.008f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    vs_tick();
    uint32_t over = aos_ui_overlay() & ~(uint32_t)AOS_UI_OVER_TOAST;
    /* a panel, the switcher, the way home or the lock: the flight waits in
     * the pause; the zoom from the icon or a banner only hold it a moment */
    const uint32_t PAUSING = AOS_UI_OVER_PANEL | AOS_UI_OVER_SWITCHER | AOS_UI_OVER_GESTURE | AOS_UI_OVER_LOCK |
                             AOS_UI_OVER_NOT_FRONT;
    a->hold = over != 0;
    if (over) {
        if ((over & PAUSING) && a->state == VA_FLY && a->flying) va_pause(a);
        vs_set(0, 0, false);
        return;
    }
    aos_pad_update(&a->pad, now);
    if (a->boot_press) {
        a->boot_press = false;
        if (a->state == VA_FLY) va_pause(a);
        else if (a->state == VA_PAUSE) va_resume(a);
    }
    bool loading = a->gen_req >= 0 || !a->world_ok;
    if (loading) {
        load_show(a, true);
        load_update(a);
    } else if (a->load_box) {
        load_show(a, false);
        a->perf_t0 = 0;
    }
    if (a->state == VA_FLY) {
        hands(a, dt);
        results(a, dt);
    } else {
        vn_pad(a);
    }
    sounds(a);
    if (!loading) present(a);
    if (a->state == VA_FLY && a->flying && !loading) perf_log(a);
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static void free_all(app_t *a)
{
    for (int i = 0; i < VA_NF; i++) {
        vu_free(a->fr_mem[i]);
        a->fr_mem[i] = a->fr[i] = NULL;
    }
    vu_free(a->cv_buf);
    a->cv_buf = NULL;
    vt_free(&a->text);
    vw_free(&a->world);
    vr_free();
}

static bool app_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    switch (a->state) {
    case VA_FLY: va_pause(a); return true;
    case VA_PAUSE: va_resume(a); return true;
    case VA_RESULT: va_to_menu(a); return true;
    default:
        if (a->screen != VN_MAIN) {
            vn_show(a, VN_MAIN);
            return true;
        }
        return false;
    }
}

static bool app_button(aos_app_t *self, void *inst, int action)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a || (a->state != VA_FLY && a->state != VA_PAUSE)) return false;
    if (action == AOS_BUTTON_CLICK) a->boot_press = true;
    return action == AOS_BUTTON_CLICK || action == AOS_BUTTON_PRESS;
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
    bool ok = vw_alloc(&a->world) && vr_init(VA_MAXW, VA_MAXH);
    for (int i = 0; i < VA_NF && ok; i++) {
        /* 64-byte aligned: the PPA reads them by DMA */
        a->fr_mem[i] = (uint16_t *)vu_malloc((size_t)VA_MAXW * VA_MAXH * 2 + 64);
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
    a->in_thr = 0.6f;
    a->gen_req = -1;
    a->mission = -1;
    a->state = VA_MENU;
    a->screen = VN_NONE;
    cfg_load(a);
    vg_load(&a->save);
    vs_enable(a->cfg.sound);
#ifdef AOS_SIM_BUILTIN
    /* development: VU_STARS=n the first n missions with three stars (not kept) */
    if (getenv("VU_STARS")) {
        for (int m = 0; m < VG_MISSIONS && m < atoi(getenv("VU_STARS")); m++) a->save.stars[m] = 3;
    }
#endif
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0A1420), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    a->canvas = lv_canvas_create(root);
    lv_obj_set_pos(a->canvas, 0, 0);
    lv_obj_set_size(a->canvas, a->SW, a->SH);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
    a->tap = lv_obj_create(root);
    lv_obj_remove_style_all(a->tap);
    lv_obj_set_size(a->tap, a->SW, a->SH);
    lv_obj_add_flag(a->tap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->tap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(a->tap, tap_cb, LV_EVENT_PRESSED, a);
    va_quality(a, a->cfg.quality);
    vp_build();
    a->worker_ok = aos_hal_worker_start_on("vuelo", worker_fn, a, 12 * 1024, 0, 3);
    if (!a->worker_ok) {
        aos_hal_log("vuelo", "no worker");
        lv_obj_clean(root);
        free_all(a);
        vu_free(a);
        return NULL;
    }
    a->menu_world = 0;
    a->free_world = 0;
    a->free_hour = VW_NOON;
    ask_world(a, VW_ISLAS, VW_NOON);
    __atomic_store_n(&a->still_req, 1, __ATOMIC_RELEASE);
    vn_show(a, VN_MAIN);
    a->timer = lv_timer_create(frame_cb, TICK_MS, a);
    lv_display_add_event_cb(lv_display_get_default(), inval_cb, LV_EVENT_INVALIDATE_AREA, a);
    vs_open();
    aos_hal_log("vuelo", "ready %dx%d, frames %dx%d | psram free before %u KB, held %u KB, %d stars", a->SW, a->SH,
                a->W, a->H, (unsigned)(hp / 1024), (unsigned)(vu_mem_used() / 1024), vg_total_stars(&a->save));
#ifdef AOS_SIM_BUILTIN
    /* development: VU_MISSION=0..14 straight into it, VU_FREE=world*10+hour */
    if (getenv("VU_MISSION")) va_start_mission(a, atoi(getenv("VU_MISSION")) % VG_MISSIONS);
    else if (getenv("VU_FREE")) va_start_free(a, atoi(getenv("VU_FREE")) / 10, atoi(getenv("VU_FREE")) % 10);
#endif
    return a;
}

static void vuelo_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    a->closing = true;
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), inval_cb, a);
    if (a->worker_ok) aos_hal_worker_stop();
    if (a->timer) lv_timer_delete(a->timer);
    vs_close();
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
    vs_open();
    aos_hal_display_refresh_request(60);
}

static void vuelo_hide(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a && a->state == VA_FLY && a->flying) va_pause(a);
    vs_close();
    aos_hal_display_refresh_request(0);
}

/* The launcher icon: a small red plane seen from above, on a blue sky */
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
    app->desc.name     = "Planeo";
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
    app->back    = app_back;
    app->button  = app_button;
    return true;
}

AOS_APP_ENTRY(vuelo_init);
