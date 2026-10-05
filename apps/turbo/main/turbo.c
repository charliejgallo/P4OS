/*
 * TURBO - an arcade racer: the app (see tb_app.h for the map of the files)
 *
 * The watch's game (AmoledOS apps/turbo) on the P4's whole screen, in both
 * orientations. What changed for the P4 is said where it happens:
 *   - the frame is the screen, 720 x 1280 or 1280 x 720, rendered 1:1 by
 *     the worker in bands of internal RAM on both cores and pushed by the
 *     LVGL timer (push_frame); the art is the Blender pipeline's again, at
 *     the P4's size (tools/pack_p4.py, turbo_p4.pak);
 *   - lying down the frame is drawn turned for the portrait panel, a strip
 *     of columns at a time (band_part), and goes out by the DMA2D as it is:
 *     the PPA turning a whole frame took 62 ms, a cap of 15 fps;
 *   - the screen turns at any moment: a race pauses, the panels are laid
 *     out again and the worker takes the new shape between two frames;
 *   - touch only: the watch steered by tilting it. A wheel under the left
 *     thumb (drag it sideways) or two arrows, and the pedals under the
 *     right one, all drawn in the frame (touch_poll reads the panel's own
 *     samples, both fingers);
 *   - racing another board needs the radio link, which the P4 does not have
 *     yet: the menu only offers it when aos_hal_link_start() works;
 *   - a USB gamepad drives as well (aos_pad.h): the stick is the wheel, as
 *     far over as it is pushed, the d-pad the arrows, A the gas, B the brake
 *     and START the pause. On the panels the d-pad goes through the buttons
 *     and A presses them (aos_pad_menu.h); B is back, L and R change the car
 *     in the garage.
 *
 * Every word on screen is wrapped in _(): the LVGL panels directly, and the
 * words inside the frame (the clock's "TIEMPO", "¡YA!"...) are rendered
 * from _() into masks when the app opens, so they follow the language too.
 * Proper names (the stages, the cars) are not translated.
 */
#include "tb_app.h"
#include "tb_audio.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS         8           /* the timer: pushes frames as they come  */
#define ACCENT          0xFF8A1E
#define WORKER_STACK    (12 * 1024)
#define PAK_NAME        "turbo_p4.pak"

#define KEY_COINS   "tb_coins"
#define KEY_CARS    "tb_cars"
#define KEY_PAINTS  "tb_paints"
#define KEY_CAR     "tb_car"
#define KEY_PNT     "tb_pnt"
#define KEY_DIFF    "tb_diff"
#define KEY_SENS    "tb_sens"
#define KEY_SFX     "tb_sfx"
#define KEY_UNL     "tb_unl"
#define KEY_TOUR    "tb_tour"
#define KEY_CTL     "tb_ctl"
#define KEY_AUTO    "tb_auto"

/* one key per stage and per car, numbered */
static const char *key_n(char *buf, size_t n, const char *prefix, int i)
{
    snprintf(buf, n, "%s%d", prefix, i);
    return buf;
}

static bool s_sfx = true;

static void snd(int id)
{
    if (s_sfx) tb_snd(id);
}

static void fmt_time(char *b, size_t n, float t)
{
    if (t < 0) t = 0;
    int ds = (int)(t * 10.0f);         /* truncated, like the HUD's clock */
    snprintf(b, n, "%d:%02d.%d", ds / 600, (ds / 10) % 60, ds % 10);
}

/* --------------------------------------------------------------------------
 * Preferences
 * -------------------------------------------------------------------------- */

static void prefs_load(app_t *a)
{
    int32_t v;
    a->coins = aos_hal_pref_get_i32(KEY_COINS, &v) ? v : 0;
    a->own_cars = aos_hal_pref_get_i32(KEY_CARS, &v) ? (uint32_t)v | 1u : 1u;
    a->own_paints = aos_hal_pref_get_i32(KEY_PAINTS, &v) ? (uint32_t)v | 1u : 1u;
    a->car = aos_hal_pref_get_i32(KEY_CAR, &v) && v >= 0 && v < CAR_N ? v : 0;
    if (!(a->own_cars & (1u << a->car))) a->car = 0;
    char k[16];
    int32_t old = 0;
    bool have_old = aos_hal_pref_get_i32(KEY_PNT, &old);
    for (int c = 0; c < CAR_N; c++) {
        int p = 0;
        if (aos_hal_pref_get_i32(key_n(k, sizeof k, "tb_pc", c), &v)) p = v;
        else if (have_old && c < 8) p = (int)(((uint32_t)old >> (c * 4)) & 15);
        a->paint[c] = (uint8_t)(p >= 0 && p < tb_paint_n() && (a->own_paints & (1u << p)) ? p : 0);
    }
    a->diff = aos_hal_pref_get_i32(KEY_DIFF, &v) && v >= 0 && v < DIFF_N ? v : DIFF_NORMAL;
    a->sens = aos_hal_pref_get_i32(KEY_SENS, &v) && v >= 0 && v < 3 ? v : 1;
    a->ctl = aos_hal_pref_get_i32(KEY_CTL, &v) && v >= 0 && v < CTL_N ? v : CTL_WHEEL;
    a->autogas = aos_hal_pref_get_i32(KEY_AUTO, &v) && v != 0;
    s_sfx = !(aos_hal_pref_get_i32(KEY_SFX, &v) && v == 0);
    a->sfx = s_sfx;
    /* the stages open from the start are always open, whatever was saved */
    a->unlocked = (aos_hal_pref_get_i32(KEY_UNL, &v) ? (uint32_t)v : 0u) | tb_stage_open_mask();
    a->best_tour = aos_hal_pref_get_i32(KEY_TOUR, &v) ? v : 0;
    for (int i = 0; i < STAGE_N; i++) {
        a->best[i] = aos_hal_pref_get_i32(key_n(k, sizeof k, "tb_best", i), &v) ? v : 0;
        a->rival_best[i] = aos_hal_pref_get_i32(key_n(k, sizeof k, "tb_rb", i), &v) ? v : 0;
    }
    if (!aos_hal_pref_get_str("tb_rname", a->rival_name, sizeof a->rival_name)) a->rival_name[0] = 0;
}

void tba_prefs_save(app_t *a)
{
    aos_hal_pref_set_i32(KEY_COINS, a->coins);
    aos_hal_pref_set_i32(KEY_CARS, (int32_t)a->own_cars);
    aos_hal_pref_set_i32(KEY_PAINTS, (int32_t)a->own_paints);
    aos_hal_pref_set_i32(KEY_CAR, a->car);
    char k[16];
    for (int c = 0; c < CAR_N; c++) aos_hal_pref_set_i32(key_n(k, sizeof k, "tb_pc", c), a->paint[c]);
    aos_hal_pref_set_i32(KEY_DIFF, a->diff);
    aos_hal_pref_set_i32(KEY_SENS, a->sens);
    aos_hal_pref_set_i32(KEY_CTL, a->ctl);
    aos_hal_pref_set_i32(KEY_AUTO, a->autogas ? 1 : 0);
    aos_hal_pref_set_i32(KEY_SFX, s_sfx ? 1 : 0);
    aos_hal_pref_set_i32(KEY_UNL, (int32_t)a->unlocked);
    aos_hal_pref_set_i32(KEY_TOUR, a->best_tour);
    for (int i = 0; i < STAGE_N; i++) {
        aos_hal_pref_set_i32(key_n(k, sizeof k, "tb_best", i), a->best[i]);
        aos_hal_pref_set_i32(key_n(k, sizeof k, "tb_rb", i), a->rival_best[i]);
    }
    aos_hal_pref_set_str("tb_rname", a->rival_name);
}

/* --------------------------------------------------------------------------
 * The worker: loading, scenes, and the race itself
 * -------------------------------------------------------------------------- */

static uint64_t s_last_yield;

static void worker_yield(void)
{
    uint64_t now = aos_hal_uptime_ms();
    if ((uint32_t)(now - s_last_yield) > 1000) {
        /* once a second keeps the idle task fed (the watchdog waits 5 s) */
        aos_hal_worker_sleep(10);
        s_last_yield = aos_hal_uptime_ms();
    }
}

static uint32_t clock_ms(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

static void log_mem(app_t *a, const char *what)
{
    uint32_t hi = 0, hp = 0;
    aos_hal_heap_info(&hi, &hp);
    aos_hal_log("turbo", "%s | internal %u B, psram %u B, art %u KB, renderer %u KB", what, (unsigned)hi,
                (unsigned)hp, (unsigned)(tb_art_bytes() / 1024), (unsigned)(a->ren ? tb_render_bytes(a->ren) / 1024 : 0));
}

static void ensure_stage(app_t *a, int stage)
{
    if (a->loaded_stage == stage) return;
    uint64_t t0 = aos_hal_uptime_ms();
    /* the coloured car goes while the stage loads, and the previous race's
     * vehicles: the new stage loads its own after its props (JOB_STAGE),
     * never both sets at once */
    tb_render_car_drop(a->ren);
    tb_art_load_vehicles(0);
    tb_track_free(&a->trk);
    if (!tb_track_build(&a->trk, stage)) {
        aos_hal_log("turbo", "stage %d: out of memory", stage);
        a->loaded_stage = -1;
        return;
    }
    tb_art_load_stage(&a->trk);
    tb_render_stage(a->ren, &a->trk);
    tb_art_drop_backdrop();
    a->loaded_stage = stage;
    char b[96];
    snprintf(b, sizeof b, "stage %d: %d segments, %d props, %u ms", stage, a->trk.nseg, a->trk.nprop,
             (unsigned)(uint32_t)(aos_hal_uptime_ms() - t0));
    log_mem(a, b);
}

/* ---- a frame, in bands ---- */

typedef struct {
    app_t    *a;
    uint16_t *fb;
    int       rot;          /* 0: as the screen; 90 / 270: turned for the panel */
    bool      hud;
    int       parts;        /* 2: even bands here, odd ones on the other core */
} band_job_t;

/* A strip of columns [x0, x1) of the screen, all its rows, from the band
 * (sw pixels a row) into the panel's order: lying down the screen's column
 * x is one of the panel's rows, so the strip lands as sw whole rows of the
 * frame, one contiguous block (the cache writes it back line by line; a
 * band of the screen's rows would scatter over every row of the panel). */
static void turn_out(uint16_t *fb, const uint16_t *band, int sw, int x0, int x1, int rot)
{
    const int H = TB_H, W = TB_W;           /* the screen's: H is the panel's width */
    for (int x = x0; x < x1; x++) {
        const uint16_t *src = band + (x - x0);
        if (rot == 90) {
            /* screen (x, y) is the panel's (H - 1 - y, x) */
            uint16_t *dst = fb + (size_t)x * H;
            const uint16_t *s = src + (size_t)(H - 1) * sw;
            for (int c = 0; c < H; c++, s -= sw) dst[c] = *s;
        } else {
            /* 270: screen (x, y) is the panel's (y, W - 1 - x) */
            uint16_t *dst = fb + (size_t)(W - 1 - x) * H;
            const uint16_t *s = src;
            for (int c = 0; c < H; c++, s += sw) dst[c] = *s;
        }
    }
}

static void band_part(void *arg, int part)
{
    band_job_t *j = (band_job_t *)arg;
    app_t *a = j->a;
    const int W = TB_W, H = TB_H;
    int step = j->parts;
    uint16_t *own = a->band[j->parts == 2 ? part : 0];
    tb_img_t im;
    if (j->rot == 0) {
        int br = own ? a->band_bytes / (W * 2) : H;
        if (br < 1) br = 1;
        for (int y0 = part * br; y0 < H; y0 += step * br) {
            int y1 = y0 + br > H ? H : y0 + br;
            uint16_t *px = own ? own : j->fb + (size_t)y0 * W;
            im.px = px - (ptrdiff_t)y0 * W;
            im.stride = W;
            im.w = (int16_t)W;
            im.h = (int16_t)H;
            tb_img_clip(&im, 0, y0, W, y1);
            tb_render_band(a->ren, &im, &a->game);
            if (j->hud) tb_hud_draw(&a->hud, &im, &a->game, &a->hs);
            if (own) memcpy(j->fb + (size_t)y0 * W, own, (size_t)(y1 - y0) * W * 2);
        }
    } else {
        int sw = a->band_bytes / (H * 2);
        if (!own || sw < 1) return;
        for (int x0 = part * sw; x0 < W; x0 += step * sw) {
            int x1 = x0 + sw > W ? W : x0 + sw;
            im.px = own - x0;
            im.stride = sw;
            im.w = (int16_t)W;
            im.h = (int16_t)H;
            tb_img_clip(&im, x0, 0, x1, H);
            tb_render_band(a->ren, &im, &a->game);
            if (j->hud) tb_hud_draw(&a->hud, &im, &a->game, &a->hs);
            turn_out(j->fb, own, sw, x0, x1, j->rot);
        }
    }
}

/* the prepared frame into fb (one of ours, or the panel's); rot as band_job_t */
static void draw_into(app_t *a, uint16_t *fb, int rot, bool hud)
{
    band_job_t j = { .a = a, .fb = fb, .rot = rot, .hud = hud, .parts = 2 };
    if (!a->band[0] || !a->band[1] || !aos_hal_worker_split(band_part, &j)) {
        j.parts = 1;
        band_part(&j, 0);
    }
}

/* the prepared frame into buffer i; rot as band_job_t */
static void draw_frame(app_t *a, int i, int rot, bool hud)
{
    draw_into(a, a->fb[i], rot, hud);
    a->fb_rot[i] = (uint16_t)rot;
}

/* the menu's and the garage's picture: the car on the start line, in
 * buffer 0 (the canvas's), as the screen is */
static void render_scene(app_t *a)
{
    ensure_stage(a, a->job_stage);
    if (a->loaded_stage < 0) return;
    tb_paint_t p;
    tb_paint_get(a->scene_paint, &p);
    tb_render_paint(a->ren, &p, &p);
    tb_game_t *g = &a->game;
    g->rival_on = false;
    tb_game_start(g, &a->trk, a->scene_car, DIFF_EASY, 3);
    g->ntraffic = 0;
    g->yaw = a->scene_yaw;
    tb_render_prepare(a->ren, g, 0);
    draw_frame(a, 0, 0, false);
    a->fb_state[0] = FB_SHOWN;
    a->shown = 0;
}

/* The frames: two of 1.8 MB always (the screen's pixels, either way up),
 * a third when PSRAM allows (spare_frame). The bands: 24 KB of internal RAM
 * per core, or 12, or one; without any the frame is drawn in PSRAM. */
static bool frames_alloc(app_t *a)
{
    bool ok = true;
    size_t fbn = (size_t)AOS_PANEL_W * AOS_PANEL_H * 2;
    for (int i = 0; i < 2; i++) {
        if (!a->fb[i]) a->fb[i] = (uint16_t *)tb_malloc(fbn);
        if (a->fb[i]) memset(a->fb[i], 0, fbn);
        else ok = false;
    }
    a->nfb = 2;
    int bytes = TB_BAND_BYTES;
    for (int tries = 0; tries < 3 && !a->band[0]; tries++) {
        a->band[0] = (uint16_t *)tb_malloc_internal((size_t)bytes);
        a->band[1] = a->band[0] ? (uint16_t *)tb_malloc_internal((size_t)bytes) : NULL;
        if (a->band[1]) break;
        if (tries == 2) break;
        free(a->band[0]);
        a->band[0] = NULL;
        bytes /= 2;
    }
    if (!a->band[0]) {
        bytes = TB_BAND_BYTES / 2;
        a->band[0] = (uint16_t *)tb_malloc_internal((size_t)bytes);
    }
    a->band_bytes = bytes;
    aos_hal_log("turbo", "bands of %d KB, %s", bytes / 1024, a->band[1] ? "two cores" : a->band[0] ? "one core" : "none (PSRAM)");
    return ok;
}

/* the screen's shape into the renderer and the HUD (the worker, JOB_FIT,
 * or before it starts) */
static void fit(app_t *a)
{
    tb_view_set(AOS_SCREEN_W, AOS_SCREEN_H);
    a->fw = (int16_t)TB_W;
    a->fh = (int16_t)TB_H;
    tb_hud_layout(&a->hud, TB_W, TB_H, TB_CAR_Y);
    /* lying down the frames go out turned, drawn a strip at a time: that
     * needs a band to turn them in */
    /* Lying down, the strips turned on the CPU measured 8 fps on the board
     * against 15 for the whole frame turned by the PPA into the panel's
     * free buffer (2026-09-29): the PPA is the default, the strips only
     * with turbo_dev.txt "turn". */
    a->turned_ok = tb_view.land && a->band[0] != NULL && a->dev_turn;
#ifdef AOS_SIM_BUILTIN
    /* the simulator has no panel to blit to: the canvas shows every frame,
     * unturned, unless TB_TURNED=1 asks to try the turned path there */
    const char *e = getenv("TB_TURNED");
    if (!(e && e[0] == '1')) a->turned_ok = false;
#endif
    for (int i = 0; i < TB_NFB; i++) a->fb_state[i] = FB_FREE;
    a->shown = -1;
    a->still = -1;
}

static int free_fb(app_t *a)
{
    for (int i = 0; i < a->nfb; i++) {
        if (a->fb_state[i] == FB_FREE) return i;
    }
    return -1;
}

/* A third frame if PSRAM allows: the blit copies the shown one into the
 * panel's framebuffer, and with only two the worker waits for it. */
static void spare_frame(app_t *a)
{
    a->spare_checked = true;
    uint32_t hi = 0, hp = 0;
    aos_hal_heap_info(&hi, &hp);
    size_t fbn = (size_t)AOS_PANEL_W * AOS_PANEL_H * 2;
    if (!a->fb[2] && hp > (uint32_t)fbn + TB_FB_SPARE) {
        uint16_t *f = (uint16_t *)tb_malloc(fbn);
        if (f) {
            a->fb_state[2] = FB_FREE;
            a->fb[2] = f;
            a->nfb = 3;                 /* last: the UI loops up to nfb */
        }
    }
    char b[64];
    snprintf(b, sizeof b, "race with %d frame buffers", a->nfb);
    log_mem(a, b);
}

/* What a frame costs, logged every 2 s while racing (the portal's /api/log
 * on the board): the frames the worker finished, its time per frame
 * (the step and the geometry, the bands on both cores, the whole), the
 * push's time on the LVGL side and how long the worker waited for a free
 * buffer. */
static struct {
    uint64_t t0, prep_us, band_us, work_us, blit_us, wait_us;
    uint32_t frames, blits;
} s_perf;

static void perf_log(app_t *a)
{
    uint64_t now = aos_hal_uptime_us();
    if (!s_perf.t0) s_perf.t0 = now;
    if (now - s_perf.t0 < 2000000) return;
    uint32_t ms = (uint32_t)((now - s_perf.t0) / 1000);
    unsigned fps10 = ms ? (unsigned)((uint64_t)s_perf.frames * 10000u / ms) : 0;
    unsigned n = s_perf.frames ? s_perf.frames : 1;
    aos_hal_log("turbo", "%u.%u fps %dx%d %s, prep %u.%u ms, bands %u.%u ms, frame %u ms, push %u us, wait %u ms/s, stage %d",
                fps10 / 10, fps10 % 10, TB_W, TB_H,
                a->turned_ok ? "turned" : (tb_view.land ? "ppa" : "rows"),
                (unsigned)(s_perf.prep_us / n / 1000), (unsigned)(s_perf.prep_us / n / 100 % 10),
                (unsigned)(s_perf.band_us / n / 1000), (unsigned)(s_perf.band_us / n / 100 % 10),
                (unsigned)(s_perf.work_us / n / 1000),
                (unsigned)(s_perf.blits ? s_perf.blit_us / s_perf.blits : 0),
                (unsigned)(ms ? s_perf.wait_us / ms : 0), a->stage);
#ifdef AOS_SIM_BUILTIN
    if (getenv("TB_DBG")) aos_hal_log("turbo", "dbg v %.1f z %.1f x %.2f state %d rot %d steer %.2f gas %d brake %d", a->game.v, a->game.z, a->game.x, a->game.state, aos_hal_display_get_rotation(), a->steer, a->gas, a->brake);
#endif
    memset(&s_perf, 0, sizeof s_perf);
    s_perf.t0 = now;
}

static void race_frame(app_t *a)
{
    if (a->over) {
        aos_hal_worker_sleep(20);
        a->w_last_ms = 0;               /* no jump in the game's clock after */
        return;
    }
    int i = free_fb(a);
    if (i < 0) {
        uint64_t w0 = aos_hal_uptime_us();
        aos_hal_worker_sleep(10);
        s_last_yield = aos_hal_uptime_ms();
        s_perf.wait_us += aos_hal_uptime_us() - w0;
        return;
    }
    a->fb_state[i] = FB_BUSY;
    uint64_t t0 = aos_hal_uptime_us();
    uint64_t now = aos_hal_uptime_ms();
    float dt = a->w_last_ms ? (float)(uint32_t)(now - a->w_last_ms) / 1000.0f : 0.033f;
    if (dt > 0.1f) dt = 0.1f;
    a->w_last_ms = now;
    tb_game_t *g = &a->game;
    if (a->autoplay) {
        tb_game_bot(g);
    } else {
        g->in_steer = a->steer;
        g->in_gas = a->gas;
        g->in_brake = a->brake;
    }
    tb_game_step(g, dt);
    uint32_t ev = g->events;
    g->events = 0;
    tb_hud_events(&a->hs, g, ev, dt);
    if (ev) {
        a->ev_ring[a->ev_w & 15] = ev;
        a->ev_w++;
    }
    a->hs.gas = g->in_gas;
    a->hs.brake = g->in_brake;
    a->hs.steer = g->in_steer;
    a->hs.left = a->left;
    a->hs.right = a->right;
    a->hs.ctl = a->ctl;
    a->hs.controls = true;
    tb_hud_prepare(&a->hs, g);
    tb_render_prepare(a->ren, g, dt);
    uint64_t t1 = aos_hal_uptime_us();
    int rot = a->turned_ok ? aos_hal_display_get_rotation() == 270 ? 270 : 90 : 0;
    /* Upright, or lying down with the strips turned: the frame is in the
     * panel's own order either way, so it is drawn straight into the
     * panel's free buffer and flipped to (aos_hal_display_flip): no frame
     * of ours, no push, no tearing. */
    uint16_t *panel = NULL;
    if (a->band[0] && (rot || (a->fw == AOS_PANEL_W && a->fh == AOS_PANEL_H && !tb_view.land)))
        panel = aos_hal_display_back();
    if (panel) {
        draw_into(a, panel, rot, true);
        uint64_t t2 = aos_hal_uptime_us();
        aos_hal_display_flip(panel);
        s_perf.blit_us += aos_hal_uptime_us() - t2;
        s_perf.blits++;
        s_perf.prep_us += t1 - t0;
        s_perf.band_us += t2 - t1;
        s_perf.work_us += t2 - t0;
        s_perf.frames++;
        a->w_us_prep += t1 - t0;
        a->w_us_bands += t2 - t1;
        a->w_us_frame += t2 - t0;
        a->w_frames++;
        a->fb_state[i] = FB_FREE;
        /* nothing of ours is on the screen: the pause asks the worker for
         * a still frame (freeze_show) */
        if (a->shown >= 0) {
            a->fb_state[a->shown] = FB_FREE;
            a->shown = -1;
        }
        perf_log(a);
        worker_yield();
        return;
    }
    draw_frame(a, i, rot, true);
    uint64_t t2 = aos_hal_uptime_us();
    s_perf.prep_us += t1 - t0;
    s_perf.band_us += t2 - t1;
    s_perf.work_us += t2 - t0;
    s_perf.frames++;
    a->w_us_prep += t1 - t0;
    a->w_us_bands += t2 - t1;
    a->w_us_frame += t2 - t0;
    a->w_frames++;
    a->fb_seq[i] = ++a->seq;
    a->fb_state[i] = FB_READY;
    if (!a->spare_checked) spare_frame(a);
    worker_yield();
}

/* under the pause and the results the canvas needs a frame as the screen
 * is: lying down the race's are turned, so the worker draws one more, the
 * race as it stands, unturned (the pedals up) */
static void still_frame(app_t *a)
{
    a->want_still = false;
    int i = free_fb(a);
    if (i < 0) {
        /* every buffer busy: take one that is only waiting to be shown */
        for (int k = 0; k < a->nfb && i < 0; k++) {
            if (a->fb_state[k] == FB_READY) i = k;
        }
    }
    if (i < 0) return;
    a->fb_state[i] = FB_BUSY;
    a->hs.gas = a->hs.brake = a->hs.left = a->hs.right = false;
    a->hs.controls = a->state == ST_RACE;
    tb_hud_prepare(&a->hs, &a->game);
    tb_render_prepare(a->ren, &a->game, 0);
    draw_frame(a, i, 0, true);
    a->fb_seq[i] = ++a->seq;
    a->fb_state[i] = FB_SHOWN;
    a->still = i;
}

static void run_job(app_t *a, int j)
{
    switch (j) {
    case JOB_BOOT: {
        uint64_t t0 = aos_hal_uptime_ms();
        char path[160];
        snprintf(path, sizeof path, "%s/%s", aos_hal_path_apps(), PAK_NAME);
        bool art = tb_art_open(path);
        /* no vehicles yet: each race loads the ones its stage uses */
        if (art) tb_art_load_vehicles(0);
        aos_hal_log("turbo", "pack %s %s in %u ms", PAK_NAME, art ? "open" : "MISSING",
                    (unsigned)(uint32_t)(aos_hal_uptime_ms() - t0));
        render_scene(a);
        break;
    }
    case JOB_STAGE: {
        ensure_stage(a, a->job_stage);
        if (a->loaded_stage < 0) break;
        /* the vehicles of this race: the stage's traffic and the rival's car */
        uint64_t t0 = aos_hal_uptime_ms();
        uint32_t mask = tb_track_vehicles(&a->trk);
        if (a->mode == MODE_LINK) mask |= 1u << a->rival_car;
        tb_art_load_vehicles(mask);
        char b[64];
        snprintf(b, sizeof b, "vehicles %03x in %u ms", (unsigned)tb_art_vehicles_loaded(),
                 (unsigned)(uint32_t)(aos_hal_uptime_ms() - t0));
        log_mem(a, b);
        break;
    }
    case JOB_SCENE:
        render_scene(a);
        break;
    case JOB_FIT:
        fit(a);
        if (a->loaded_stage >= 0 && (a->state == ST_RACE || a->state == ST_RESULT)) {
            still_frame(a);
        } else if (a->loaded_stage >= 0) {
            /* the menus' picture in the new shape */
            tb_game_t *g = &a->game;
            if (!g->trk) render_scene(a);
            else {
                tb_render_prepare(a->ren, g, 0);
                draw_frame(a, 0, 0, false);
                a->fb_state[0] = FB_SHOWN;
                a->shown = 0;
            }
        }
        break;
    default:
        break;
    }
}

static void worker_fn(void *arg)
{
    app_t *a = (app_t *)arg;
    tb_set_clock(clock_ms);
    tb_set_yield(worker_yield);
    while (!aos_hal_worker_should_stop()) {
        if (a->job) {
            s_last_yield = aos_hal_uptime_ms();
            run_job(a, a->job);
            a->job = JOB_NONE;
            a->job_done = true;
            continue;
        }
        if (a->racing && !a->paused && !a->fitting) {
            race_frame(a);
            continue;
        }
        if (a->want_still && a->loaded_stage >= 0 && !a->fitting) {
            still_frame(a);
            continue;
        }
        a->w_last_ms = 0;
        aos_hal_worker_sleep(20);
    }
    tb_set_yield(NULL);
}

static bool job(app_t *a, int j)
{
    if (a->job != JOB_NONE) return false;
    a->job_done = false;
    a->job = j;
    return true;
}

/* --------------------------------------------------------------------------
 * Frames to the panel
 * -------------------------------------------------------------------------- */

/* the canvas shows buffer i under the panels (in the simulator every
 * frame: it has no panel to blit to). A turned buffer cannot be shown as it
 * is: in the simulator it is turned back into a copy (TB_TURNED); on the
 * board the panels ask the worker for an unturned one (want_still). */
static void canvas_show(app_t *a, int i)
{
    if (!a->canvas || i < 0 || !a->fb[i]) return;
    const uint16_t *src = a->fb[i];
    if (a->fb_rot[i]) {
#ifdef AOS_SIM_BUILTIN
        size_t n = (size_t)TB_W * TB_H;
        if (!a->cv) a->cv = (uint16_t *)tb_malloc(n * 2);
        if (!a->cv) return;
        const int W = TB_W, H = TB_H;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                size_t k = a->fb_rot[i] == 90 ? (size_t)x * H + (H - 1 - y) : (size_t)(W - 1 - x) * H + y;
                a->cv[(size_t)y * W + x] = src[k];
            }
        }
        src = a->cv;
#else
        return;
#endif
    }
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_buffer(a->canvas, (void *)src, a->fw, a->fh, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(a->canvas, a->fw, a->fh);
    lv_obj_invalidate(a->canvas);
}

static void push_frame(app_t *a)
{
    if (a->over) return;
    if (a->fitting || a->want_fit) return;
    int best = -1;
    uint32_t bs = 0;
    for (int i = 0; i < a->nfb; i++) {
        if (a->fb_state[i] == FB_READY && (best < 0 || a->fb_seq[i] > bs)) {
            best = i;
            bs = a->fb_seq[i];
        }
    }
    if (best < 0) return;
    /* older ready frames are dropped */
    for (int i = 0; i < a->nfb; i++) {
        if (i != best && a->fb_state[i] == FB_READY) a->fb_state[i] = FB_FREE;
    }
    uint64_t t0 = aos_hal_uptime_us();
    bool ok;
    if (a->fb_rot[best]) {
        /* already the panel's order: the DMA2D copies it as it is */
        ok = aos_hal_display_blit_native(0, 0, AOS_PANEL_W, AOS_PANEL_H, a->fb[best]);
    } else {
        /* lying down: the PPA turns it into the panel's free buffer, which
         * is then flipped to (no tearing, and 62 -> ~43 ms when the cores
         * are not drawing beside it); the frame shown before is free at
         * once, since the panel keeps its own copy, so the worker draws the
         * next one meanwhile */
        uint16_t *back = a->fw > a->fh ? aos_hal_display_back() : NULL;
        if (back && a->shown >= 0 && a->shown != best) {
            a->fb_state[a->shown] = FB_FREE;
            a->shown = -1;
        }
        if (back && aos_hal_display_blit_into(back, 0, 0, a->fw, a->fh, a->fb[best]))
            ok = aos_hal_display_flip(back);
        else
            /* upright the same DMA2D copy; lying down the PPA turns it */
            ok = aos_hal_display_blit_scaled(0, 0, a->fw, a->fh, a->fb[best], 1, false);
    }
    s_perf.blit_us += aos_hal_uptime_us() - t0;
    s_perf.blits++;
    if (!ok) canvas_show(a, best);
    if (a->shown >= 0 && a->shown != best) a->fb_state[a->shown] = FB_FREE;
    a->fb_state[best] = FB_SHOWN;
    a->shown = best;
    if (a->state == ST_RACE) perf_log(a);
}

/* the panels over the race's last frame: straight from the buffer upright,
 * a still one from the worker lying down */
static void freeze_show(app_t *a)
{
    if (a->shown >= 0 && !a->fb_rot[a->shown]) {
        canvas_show(a, a->shown);
        return;
    }
    if (a->canvas) lv_obj_add_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
    a->still = -1;
    a->want_still = true;
}

/* a still frame arrived: to the canvas */
static void still_take(app_t *a)
{
    int i = a->still;
    if (i < 0 || a->want_still || a->fitting) return;
    a->still = -1;
    for (int k = 0; k < a->nfb; k++) {
        if (k != i && a->fb_state[k] != FB_BUSY) a->fb_state[k] = FB_FREE;
    }
    a->shown = i;
    canvas_show(a, i);
}

/* --------------------------------------------------------------------------
 * Interface pieces
 * -------------------------------------------------------------------------- */

static lv_obj_t *panel(lv_obj_t *parent, int dim)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    if (dim) {
        lv_obj_set_style_bg_color(p, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(p, (lv_opa_t)dim, 0);
    }
    return p;
}

/* a transparent flex column: the panels' contents, placed by tba_layout */
static lv_obj_t *column(lv_obj_t *parent, int gap)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, gap, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *row(lv_obj_t *parent, int gap)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int h, uint32_t accent, lv_event_cb_t cb, void *data,
                        lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, lv_pct(100), h);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x14161C), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(accent), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, h >= 88 ? aos_font_title : aos_font_body, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, lv_pct(94));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    if (lbl_out) *lbl_out = l;
    return b;
}

void tba_toast(app_t *a, const char *txt)
{
    (void)a;
    aos_ui_toast(txt, 2000);
}

static lv_obj_t *panel_of(app_t *a, int i)
{
    lv_obj_t *const p[] = { a->p_boot, a->p_menu, a->p_select, a->p_garage, a->p_settings,
                            a->p_loading, a->p_result, a->p_pause, a->p_lobby };
    return i < (int)(sizeof(p) / sizeof(p[0])) ? p[i] : NULL;
}

/* Every button inside o, in the order they were made. */
static int pad_collect(lv_obj_t *o, lv_obj_t **out, int n, int max)
{
    uint32_t k = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < k && n < max; i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE)) out[n++] = c;
        else n = pad_collect(c, out, n, max);
    }
    return n;
}

/* The pad goes through the buttons of the panel showing. The one it had
 * picked before loses its outline first: that panel is hidden by now, and
 * aos_pad_menu leaves hidden ones alone, so it would still wear it the
 * next time the panel comes up. */
static void pad_menu_take(app_t *a, lv_obj_t *show)
{
    aos_pad_menu_t *m = &a->pmenu;
    if (m->sel < m->n && lv_obj_is_valid(m->item[m->sel])) {
        lv_obj_set_style_outline_width(m->item[m->sel], 0, 0);
    }
    if (!show) {
        aos_pad_menu_clear(m);
        return;
    }
    lv_obj_t *items[AOS_PAD_MENU_MAX];
    int n = pad_collect(show, items, 0, AOS_PAD_MENU_MAX);
    int sel = 0;
    for (int i = 0; i < n; i++) {
        if (items[i] == a->g_btn) sel = i;      /* the garage: buy / choose */
    }
    aos_pad_menu_set(m, items, n, sel);
}

/* Room for the pad's outline, which is drawn around a button and so out
 * of the flex column or row holding it, which would clip it: those that
 * stay put let their buttons draw outside them - and say they draw that
 * much further themselves, or LVGL would not redraw them, nor the outline,
 * where they end - and the lists that scroll (they have to clip) keep a
 * margin inside as wide as the outline. */
#define PAD_ROOM 12

static void pad_room_cb(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, PAD_ROOM);
}

static void pad_room(lv_obj_t *o)
{
    uint32_t k = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < k; i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE) || !lv_obj_get_child_count(c)) continue;
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_SCROLLABLE)) {
            lv_obj_set_style_pad_hor(c, PAD_ROOM, 0);
            lv_obj_set_style_pad_top(c, PAD_ROOM, 0);
        } else {
            lv_obj_add_flag(c, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
            lv_obj_add_event_cb(c, pad_room_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
            lv_obj_refresh_ext_draw_size(c);
        }
        pad_room(c);
    }
}

static void show_panel(app_t *a, lv_obj_t *show)
{
    for (int i = 0; i < 9; i++) {
        lv_obj_t *p = panel_of(a, i);
        if (p && p != show) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    }
    if (show) {
        lv_obj_remove_flag(show, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(show);
    }
    pad_menu_take(a, show);
}

/* --------------------------------------------------------------------------
 * Text in the frame: LVGL renders it once into masks, the HUD bakes them
 * -------------------------------------------------------------------------- */

static bool text_mask(tb_mask_t *m, const char *txt, const lv_font_t *font)
{
    lv_point_t sz;
    lv_text_get_size(&sz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = sz.x + 2, h = sz.y;
    memset(m, 0, sizeof(*m));
    if (w <= 2 || h <= 0) return false;
    lv_draw_buf_t *db = lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_L8, 0);
    if (!db) return false;
    lv_obj_t *c = lv_canvas_create(lv_layer_top());
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(c, db);
    lv_canvas_fill_bg(c, lv_color_hex(0x000000), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(c, &layer);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.color = lv_color_hex(0xFFFFFF);
    d.font = font;
    d.text = txt;
    lv_area_t area = { 1, 0, w - 1, h - 1 };
    lv_draw_label(&layer, &d, &area);
    lv_canvas_finish_layer(c, &layer);
    m->a = (uint8_t *)tb_malloc((size_t)w * h);
    if (m->a) {
        m->w = (int16_t)w;
        m->h = (int16_t)h;
        for (int y = 0; y < h; y++) memcpy(m->a + (size_t)y * w, db->data + (size_t)y * db->header.stride, (size_t)w);
    }
    lv_obj_delete(c);
    lv_draw_buf_destroy(db);
    return m->a != NULL;
}

static void bake_text(tb_sprite_t *out, const char *txt, const lv_font_t *font, uint32_t top, uint32_t bot, int ol)
{
    tb_mask_t m;
    if (text_mask(&m, txt, font)) {
        tb_hud_bake(out, &m, top, bot, ol);
        free(m.a);
    }
}

/* the watch's sizes doubled where the P4 has them: the clock in the 96 px
 * digits, the speed in 64, the banners in 64 */
static void hud_build(app_t *a)
{
    static const char glyphs[TB_GLYPHS + 1] = "0123456789:.+-";
    tb_hud_t *h = &a->hud;
    for (int i = 0; i < TB_GLYPHS; i++) {
        char s[2] = { glyphs[i], 0 };
        bake_text(&h->big[i], s, &aos_inter_num_96, 0xFFF45A, 0xFF8A1E, 5);
        bake_text(&h->mid[i], s, &aos_montserrat_64, 0xFFFFFF, 0xC8D4E0, 4);
        bake_text(&h->sml[i], s, &aos_montserrat_36, 0xFFFFFF, 0xD8E0E8, 3);
    }
    bake_text(&h->word[TX_TIME], _("TIEMPO"), &aos_montserrat_36, 0xFFE040, 0xFFB020, 3);
    bake_text(&h->word[TX_KMH], _("km/h"), &aos_montserrat_24, 0xD8E0E8, 0xB8C0C8, 2);
    bake_text(&h->word[TX_EXTRA], _("¡TIEMPO EXTRA!"), &aos_montserrat_48, 0xFFF45A, 0xFF8A1E, 4);
    bake_text(&h->word[TX_GO], _("¡YA!"), &aos_montserrat_64, 0x9CFF6A, 0x30C040, 5);
    bake_text(&h->word[TX_FINISH], _("¡LLEGADA!"), &aos_montserrat_64, 0xFFFFFF, 0xFFD040, 5);
    bake_text(&h->word[TX_TIMEUP], _("SIN TIEMPO"), &aos_montserrat_64, 0xFF8A70, 0xE02020, 5);
    bake_text(&h->word[TX_HURRY], _("¡APURATE!"), &aos_montserrat_48, 0xFF8A70, 0xE02020, 4);
    tb_hud_make_controls(h);
    h->ok = true;
}

/* --------------------------------------------------------------------------
 * States
 * -------------------------------------------------------------------------- */

static void menu_refresh(app_t *a);
static void select_refresh(app_t *a);
static void garage_refresh(app_t *a);
static void settings_refresh(app_t *a);
static void tba_layout(app_t *a);

/* the 3rd frame is only for racing: loading a stage or recolouring the car
 * in the garage want the room */
static void spare_free(app_t *a)
{
    if (!a->fb[2] || a->racing || a->job != JOB_NONE) return;
    if (a->shown == 2) a->shown = -1;
    if (a->still == 2) a->still = -1;
    a->nfb = 2;
    free(a->fb[2]);
    a->fb[2] = NULL;
}

static bool s_scene_pending;

static void scene(app_t *a, int stage, int car, int paint, float yaw)
{
    a->job_stage = stage;
    a->scene_car = car;
    a->scene_paint = paint;
    a->scene_yaw = yaw;
    /* one running (a paint tapped while the last one draws): this one
     * after it, frame() starts it */
    s_scene_pending = !job(a, JOB_SCENE);
}

void tba_set_state(app_t *a, int st)
{
    int prev = a->state;
    a->state = st;
    a->st_ms = 0;
    switch (st) {
    case ST_MENU:
        spare_free(a);
        menu_refresh(a);
        show_panel(a, a->p_menu);
        if (prev != ST_BOOT) scene(a, a->loaded_stage >= 0 ? a->loaded_stage : 0, a->car, a->paint[a->car], 0);
        break;
    case ST_SELECT:
        select_refresh(a);
        show_panel(a, a->p_select);
        break;
    case ST_GARAGE:
        a->g_car = a->car;
        a->g_paint = a->paint[a->car];
        garage_refresh(a);
        show_panel(a, a->p_garage);
        scene(a, a->loaded_stage >= 0 ? a->loaded_stage : 0, a->g_car, a->g_paint, 0);
        break;
    case ST_SETTINGS:
        settings_refresh(a);
        show_panel(a, a->p_settings);
        break;
    case ST_LOADING:
        show_panel(a, a->p_loading);
        break;
    case ST_RACE:
        show_panel(a, NULL);
        break;
    case ST_RESULT:
        show_panel(a, a->p_result);
        break;
    case ST_LOBBY:
        show_panel(a, a->p_lobby);
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------------------
 * The race
 * -------------------------------------------------------------------------- */

static void tour_begin(app_t *a)
{
    a->mode = MODE_TOUR;
    a->tour_time = 0;
    a->tour_i = 0;
    tba_race_start(a, tb_tour_stage(0));
}

static bool s_stage_pending;

void tba_race_start(app_t *a, int stage)
{
    spare_free(a);
    a->stage = stage;
    char b[64];
    snprintf(b, sizeof b, "%s\n%s", _("Cargando"), tb_stage_name(stage));
    lv_label_set_text(a->load_lbl, b);
    tba_set_state(a, ST_LOADING);
    a->job_stage = stage;
    s_scene_pending = false;
    /* a scene being drawn finishes first */
    s_stage_pending = !job(a, JOB_STAGE);
}

static void race_go(app_t *a)
{
    tb_game_t *g = &a->game;
    tb_paint_t p, rp;
    tb_paint_get(a->paint[a->car], &p);
    tb_paint_get(a->rival_paint, &rp);
    tb_render_paint(a->ren, &p, &rp);
    uint32_t seed = a->mode == MODE_LINK ? a->link_nonce ^ a->link_peer_nonce : (uint32_t)aos_hal_uptime_ms() | 1u;
    g->rival_on = a->mode == MODE_LINK;
    tb_game_start(g, &a->trk, a->car, a->diff, seed);
    g->rival_car = a->rival_car;
    g->rival_paint = a->rival_paint;
    g->rival_z = g->z;
    g->rival_x = 0;
    memset(&a->hs, 0, sizeof a->hs);
    a->hs.banner = -1;
    a->hs.rival = a->mode == MODE_LINK;
    a->hs.ctl = a->ctl;
    a->hs.controls = true;
    a->rival_done = false;
    a->result_shown = false;
    a->gas = a->brake = a->left = a->right = false;
    a->steer = 0;
    memset(a->tk, 0, sizeof a->tk);
    a->ev_r = a->ev_w;
    a->nfb = a->fb[2] ? 3 : 2;
    a->spare_checked = false;
    for (int i = 0; i < TB_NFB; i++) a->fb_state[i] = FB_FREE;
    a->shown = -1;
    a->still = -1;
    a->paused = false;
    a->w_frames = 0;
    a->w_us_prep = a->w_us_bands = a->w_us_frame = 0;
    memset(&s_perf, 0, sizeof s_perf);
    a->w_fps_t0 = aos_hal_uptime_ms();
    tba_set_state(a, ST_RACE);
    if (a->canvas) lv_obj_add_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
#ifdef AOS_SIM_BUILTIN
    if (a->canvas) lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
#endif
    a->racing = true;
}

static void pause_show(app_t *a)
{
    if (a->state != ST_RACE || a->paused) return;
    a->paused = true;
    a->gas = a->brake = a->left = a->right = false;
    memset(a->tk, 0, sizeof a->tk);
    tb_audio_engine(false, 0, 0, 0, 0, 0);
    freeze_show(a);
    show_panel(a, a->p_pause);
}

static void resume(app_t *a)
{
    a->paused = false;
    a->w_last_ms = 0;
    show_panel(a, NULL);
#ifndef AOS_SIM_BUILTIN
    if (a->canvas) lv_obj_add_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
#endif
}

static int stage_coins(app_t *a, const tb_game_t *g, bool finished)
{
    int c = finished ? 40 + (int)g->time_left * 2 : (int)(tb_game_progress(g) * 30.0f);
    c += g->passes / 3;
    if (a->diff == DIFF_EASY) c = c * 7 / 10;
    if (a->diff == DIFF_HARD) c = c * 3 / 2;
    return c;
}

/* the results' text; the rival's line changes when its result arrives */
static void result_text(app_t *a)
{
    tb_game_t *g = &a->game;
    bool fin = g->state == RS_FINISHED;
    char body[480], t[24], bt[24];
    int s = a->stage;
    fmt_time(t, sizeof t, g->elapsed);
    fmt_time(bt, sizeof bt, (float)a->best[s] / 10.0f);
    int n = 0;
    n += snprintf(body + n, sizeof body - (size_t)n, "%s\n", tb_stage_name(s));
    if (fin) n += snprintf(body + n, sizeof body - (size_t)n, "%s  %s\n", _("Tiempo"), t);
    else n += snprintf(body + n, sizeof body - (size_t)n, "%s %d %%\n", _("Recorrido"), (int)(tb_game_progress(g) * 100.0f));
    if (a->best[s]) n += snprintf(body + n, sizeof body - (size_t)n, "%s  %s%s\n", _("Récord"), bt,
                                  a->new_record ? _("  ¡nuevo!") : "");
    n += snprintf(body + n, sizeof body - (size_t)n, "%s %d km/h · %s %d\n", _("Punta"), (int)g->top_speed,
                  _("choques"), g->crashes);
    if (a->mode == MODE_TOUR) {
        char tt[24];
        fmt_time(tt, sizeof tt, a->tour_time);
        n += snprintf(body + n, sizeof body - (size_t)n, "%s %d/%d  %s\n", _("Gira"), a->tour_i + 1, tb_tour_len(), tt);
        if (fin && a->tour_i == tb_tour_len() - 1) n += snprintf(body + n, sizeof body - (size_t)n, "%s\n", _("¡Gira completa!"));
    }
    if (a->mode == MODE_LINK) {
        char rt[24];
        if (!a->rival_done) {
            n += snprintf(body + n, sizeof body - (size_t)n, "%s: %s\n", a->partner, _("todavía corre..."));
        } else {
            if (a->rival_finished) fmt_time(rt, sizeof rt, a->rival_time);
            else snprintf(rt, sizeof rt, "%d %%", (int)(a->rival_dist * 100.0f));
            n += snprintf(body + n, sizeof body - (size_t)n, "%s: %s\n", a->partner, rt);
            bool win = fin && (!a->rival_finished || g->elapsed < a->rival_time);
            if (!fin && !a->rival_finished) win = tb_game_progress(g) > a->rival_dist;
            n += snprintf(body + n, sizeof body - (size_t)n, "%s\n", win ? _("¡Ganaste!") : _("Ganó el otro reloj"));
            if (win && !a->res_win_paid) {
                a->res_win_paid = true;
                a->coins_won += 60;
                a->coins += 60;
                tba_prefs_save(a);
            }
        }
        a->res_rival_seen = a->rival_done;
    }
    snprintf(body + n, sizeof body - (size_t)n, "+%d %s", a->coins_won, _("monedas"));
    lv_label_set_text(a->res_body, body);
}

static void result_show(app_t *a)
{
    tb_game_t *g = &a->game;
    bool fin = g->state == RS_FINISHED;
    int s = a->stage;
    a->coins_won = stage_coins(a, g, fin);
    a->res_win_paid = false;
    int32_t ds = (int32_t)(g->elapsed * 10.0f);
    a->new_record = fin && (a->best[s] == 0 || ds < a->best[s]);
    if (a->new_record) a->best[s] = ds;
    bool tour_done = a->mode == MODE_TOUR && fin && a->tour_i == tb_tour_len() - 1;
    for (int k = 0; k < STAGE_N; k++) {
        int after = -1;
        int rule = tb_stage_unlock(k, &after);
        if ((rule == UNL_AFTER && fin && after == s) || (rule == UNL_TOUR && tour_done)) a->unlocked |= 1u << k;
    }
    if (a->mode == MODE_TOUR) {
        a->tour_time += g->elapsed;
        if (tour_done) {
            a->coins_won += 200;
            int32_t tds = (int32_t)(a->tour_time * 10.0f);
            if (a->best_tour == 0 || tds < a->best_tour) a->best_tour = tds;
        }
    }
    a->coins += a->coins_won;
    lv_label_set_text(a->res_title, fin ? _("¡LLEGADA!") : _("SIN TIEMPO"));
    result_text(a);
    bool next = a->mode == MODE_TOUR && fin && a->tour_i + 1 < tb_tour_len();
    lv_label_set_text(a->res_btn_next_lbl, next ? _("Siguiente") : _("Menú"));
    lv_obj_set_user_data(a->res_btn_next, (void *)(intptr_t)(next ? 1 : 0));
    tba_prefs_save(a);
    tba_set_state(a, ST_RESULT);
    freeze_show(a);
}

/* the race's line for the log and the card: frames, frame rate, where the
 * worker's time went */
static void race_stats(app_t *a)
{
    uint32_t ms = (uint32_t)(aos_hal_uptime_ms() - a->w_fps_t0);
    if (!ms || !a->w_frames) return;
    unsigned fps10 = (unsigned)(a->w_frames * 10000u / ms);
    unsigned n = a->w_frames;
    char line[256];
    snprintf(line, sizeof line, "race: stage %d, %dx%d %s, %u frames in %u ms, %u.%u fps, prep %u.%u ms, bands %u.%u ms, frame %u.%u ms",
             a->stage, TB_W, TB_H, a->turned_ok ? "turned" : (tb_view.land ? "ppa" : "rows"), n, (unsigned)ms,
             fps10 / 10, fps10 % 10, (unsigned)(a->w_us_prep / n / 1000), (unsigned)(a->w_us_prep / n / 100 % 10),
             (unsigned)(a->w_us_bands / n / 1000), (unsigned)(a->w_us_bands / n / 100 % 10),
             (unsigned)(a->w_us_frame / n / 1000), (unsigned)(a->w_us_frame / n / 100 % 10));
    aos_hal_log("turbo", "%s", line);
    char path[160];
    snprintf(path, sizeof path, "%s/turbo_stats.txt", aos_hal_path_apps());
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

/* after the finish or the time running out: a few seconds of the car
 * rolling to a stop, then the results */
static void race_tick(app_t *a)
{
    tb_game_t *g = &a->game;
    while (a->ev_r != a->ev_w) {
        uint32_t ev = a->ev_ring[a->ev_r & 15];
        a->ev_r++;
        if (ev & EV_COUNT) snd(SND_COUNT);
        if (ev & EV_GO) snd(SND_GO);
        if (ev & EV_CHECKPOINT) snd(SND_CHECKPOINT);
        if (ev & EV_CRASH) snd(SND_CRASH);
        else if (ev & EV_BUMP) snd(SND_BUMP);
        if (ev & EV_PASS) snd(SND_PASS);
        if (ev & EV_GEAR) snd(SND_GEAR);
        if (ev & EV_LOW_TIME) snd(SND_LOW);
        if (ev & EV_FINISH) {
            snd(SND_FINISH);
            if (a->mode == MODE_LINK) tbl_send_result(a);
        }
        if (ev & EV_TIMEUP) {
            snd(SND_TIMEUP);
            if (a->mode == MODE_LINK) tbl_send_result(a);
        }
    }
    if (s_sfx && !a->paused) {
        float sq = fabsf(g->slide) > 0.6f ? (fabsf(g->slide) - 0.6f) * 2.0f : 0.0f;
        if (g->in_brake && g->v > 20.0f) sq += 0.5f;
        tb_audio_engine(true, g->rpm, g->in_gas ? 1.0f : 0.0f, g->v / 86.0f, sq > 1 ? 1 : sq,
                        g->offroad && g->v > 5.0f ? 0.8f : 0.0f);
    }
    if ((g->state == RS_FINISHED || g->state == RS_TIMEUP) && g->t_state > 3.0f && !a->result_shown) {
        a->result_shown = true;
        a->racing = false;
        tb_audio_engine(false, 0, 0, 0, 0, 0);
        race_stats(a);
        result_show(a);
    }
}

/* --------------------------------------------------------------------------
 * Touch: the panel's own samples, both fingers
 * -------------------------------------------------------------------------- */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void finger_press(app_t *a, tb_finger_t *k, int x, int y)
{
    const tb_hud_lay_t *l = &a->hud.lay;
    k->on = true;
    k->x0 = k->x = (int16_t)x;
    k->y0 = k->y = (int16_t)y;
    k->s0 = a->steer;
    if (tb_in(&l->pause, x, y)) k->what = TK_PAUSE;
    else if (a->ctl == CTL_ARROWS && (tb_in(&l->left, x, y) || tb_in(&l->right, x, y))) k->what = TK_ARROW;
    else if (tb_in(&l->brake, x, y) || tb_in(&l->gas, x, y)) k->what = TK_PEDAL;
    else if (a->ctl == CTL_WHEEL && x < l->w / 2) k->what = TK_STEER;
    else if (a->ctl == CTL_ARROWS && x < l->w / 2 && y > l->bar_y + 40) k->what = TK_ARROW;
    else if (x >= l->w / 2 && y > l->bar_y + 40) k->what = TK_PEDAL;
    else k->what = TK_NONE;
}

static void finger_release(app_t *a, tb_finger_t *k)
{
    if (!k->on) return;
    k->on = false;
    int dx = k->x - k->x0, dy = k->y - k->y0;
    if (k->what == TK_PAUSE && dx * dx + dy * dy < 40 * 40) pause_show(a);
}

/* The stick this far from the middle (of 32767) is still the middle: cheap
 * sticks rest a few thousand off. */
#define PAD_DEAD 4000

static void touch_poll(app_t *a, int dt_ms)
{
    aos_touch_frame_t fr[16];
    uint32_t n = aos_hal_touch_frames(a->touch_seq, fr, 16);
    bool live = a->state == ST_RACE && !a->paused && !a->closing && !a->fitting;
    lv_area_t rc;
    lv_obj_get_coords(a->root, &rc);    /* the runtime slides the root */
    for (uint32_t s = 0; s < n; s++) {
        const aos_touch_frame_t *f = &fr[s];
        a->touch_seq = f->seq;
        if (!live) {
            a->tk[0].on = a->tk[1].on = false;
            a->touch_lift = f->count > 0;
            continue;
        }
        if (a->touch_lift) {
            /* a finger from a menu (the pause's Continue) is not a pedal:
             * nothing counts until every finger has lifted once */
            if (f->count == 0) a->touch_lift = false;
            continue;
        }
        int np = f->count > 2 ? 2 : f->count;
        int px[2], py[2];
        bool used[2] = { false, false };
        for (int i = 0; i < np; i++) {
            px[i] = f->x[i] - rc.x1;
            py[i] = f->y[i] - rc.y1;
        }
        /* each finger that was down follows the nearest sample */
        for (int t = 0; t < 2; t++) {
            tb_finger_t *k = &a->tk[t];
            if (!k->on) continue;
            int best = -1, best_d = 300 * 300;
            for (int i = 0; i < np; i++) {
                if (used[i]) continue;
                int dx = px[i] - k->x, dy = py[i] - k->y, d = dx * dx + dy * dy;
                if (d < best_d) {
                    best_d = d;
                    best = i;
                }
            }
            if (best >= 0) {
                used[best] = true;
                k->x = (int16_t)px[best];
                k->y = (int16_t)py[best];
            } else {
                finger_release(a, k);
            }
        }
        for (int i = 0; i < np; i++) {
            if (used[i]) continue;
            for (int t = 0; t < 2; t++) {
                if (!a->tk[t].on) {
                    finger_press(a, &a->tk[t], px[i], py[i]);
                    break;
                }
            }
        }
    }
    /* what the fingers down mean now */
    const tb_hud_lay_t *l = &a->hud.lay;
    bool gas = false, brake = false, left = false, right = false, steering = false;
    float steer = a->steer;
    static const float lock_px[3] = { 210.0f, 150.0f, 105.0f };     /* a drag this long is full lock */
    for (int t = 0; t < 2 && live; t++) {
        const tb_finger_t *k = &a->tk[t];
        if (!k->on) continue;
        switch (k->what) {
        case TK_PEDAL:
            /* sliding between the pedals works: the side the finger is on */
            if (k->x >= l->gas.x0) gas = true;
            else brake = true;
            break;
        case TK_STEER:
            steering = true;
            steer = clampf(k->s0 + (float)(k->x - k->x0) / lock_px[a->sens], -1.0f, 1.0f);
            break;
        case TK_ARROW: {
            int mid = (l->left.x1 + l->right.x0) / 2;
            if (k->x < mid) left = true;
            else right = true;
            break;
        }
        default:
            break;
        }
    }
    /* the pad: A the gas and B the brake; the stick past its dead zone is
     * the wheel turned as far as the stick is pushed, a little gentler near
     * the middle and by the sensitivity; the d-pad (or a stick inside the
     * dead zone) turns it like the arrows, whatever the setting */
    bool pad_left = false, pad_right = false;
    if (live && a->pad.connected) {
        if (aos_pad_held(&a->pad, AOS_PAD_A)) gas = true;
        if (aos_pad_held(&a->pad, AOS_PAD_B)) brake = true;
        int sx = a->pad.x;
        if (sx > PAD_DEAD || sx < -PAD_DEAD) {
            static const float gain[3] = { 0.8f, 1.0f, 1.25f };
            float v = (float)(sx > 0 ? sx - PAD_DEAD : sx + PAD_DEAD) / (float)(32767 - PAD_DEAD);
            steering = true;
            steer = clampf(v * (0.4f + 0.6f * fabsf(v)) * gain[a->sens], -1.0f, 1.0f);
        } else {
            pad_left = aos_pad_held(&a->pad, AOS_PAD_LEFT);
            pad_right = aos_pad_held(&a->pad, AOS_PAD_RIGHT);
        }
    }
    float dt = (float)dt_ms / 1000.0f;
    if ((a->ctl == CTL_ARROWS || pad_left || pad_right) && !steering) {
        /* the arrows turn the wheel at a steady rate, faster back to centre */
        static const float rate[3] = { 3.0f, 4.5f, 6.5f };
        bool tl = left || pad_left, tr = right || pad_right;
        float target = tl == tr ? 0.0f : (tl ? -1.0f : 1.0f);
        float r = rate[a->sens] * (target == 0.0f ? 1.6f : 1.0f);
        float d = target - steer, stp = r * dt;
        steer += d > stp ? stp : (d < -stp ? -stp : d);
    } else if (!steering) {
        /* let go, the wheel comes back to the centre */
        float k = dt * 10.0f;
        steer -= steer * (k > 1 ? 1 : k);
        if (fabsf(steer) < 0.01f) steer = 0;
    }
    if (a->autogas && live) gas = !brake;
    a->steer = steer;
    a->gas = gas;
    a->brake = brake;
    a->left = left;
    a->right = right;
}

/* --------------------------------------------------------------------------
 * Panels
 * -------------------------------------------------------------------------- */

static app_t *app_of(lv_event_t *e)
{
    return (app_t *)lv_event_get_user_data(e);
}

static void menu_tour_cb(lv_event_t *e) { tour_begin(app_of(e)); }

static void menu_trial_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    a->mode = MODE_TRIAL;
    tba_set_state(a, ST_SELECT);
}

static void menu_link_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    a->mode = MODE_LINK;
    tba_set_state(a, ST_SELECT);
}

static void menu_garage_cb(lv_event_t *e) { tba_set_state(app_of(e), ST_GARAGE); }
static void menu_settings_cb(lv_event_t *e) { tba_set_state(app_of(e), ST_SETTINGS); }

static void menu_refresh(app_t *a)
{
    char b[48];
    snprintf(b, sizeof b, "%d " LV_SYMBOL_BULLET, (int)a->coins);
    lv_label_set_text(a->lbl_coins, b);
    char name[28];
    if (tbl_available(a, name, sizeof name)) {
        snprintf(b, sizeof b, "%s %s", _("Contra"), name);
        lv_label_set_text(a->lbl_link, b);
        lv_obj_remove_flag(a->btn_link, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a->btn_link, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_menu(app_t *a, lv_obj_t *root)
{
    a->p_menu = panel(root, 0);
    a->menu_band = lv_obj_create(a->p_menu);
    lv_obj_remove_style_all(a->menu_band);
    lv_obj_set_style_bg_color(a->menu_band, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(a->menu_band, LV_OPA_50, 0);
    lv_obj_remove_flag(a->menu_band, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->menu_band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(a->menu_band, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->menu_band, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    a->menu_title = label(a->menu_band, "TURBO", &aos_montserrat_64, ACCENT);
    a->menu_sub = label(a->menu_band, _("carreras contra el reloj"), aos_font_body, 0xE0E4EA);
    a->lbl_coins = lv_label_create(a->p_menu);
    lv_obj_set_style_text_font(a->lbl_coins, aos_font_title, 0);
    lv_obj_set_style_text_color(a->lbl_coins, lv_color_hex(0xFFD040), 0);
    lv_obj_align(a->lbl_coins, LV_ALIGN_TOP_RIGHT, -24, 20);
    a->menu_col = column(a->p_menu, 18);
    button(a->menu_col, _("Gira completa"), 96, ACCENT, menu_tour_cb, a, NULL);
    button(a->menu_col, _("Contrarreloj"), 96, ACCENT, menu_trial_cb, a, NULL);
    a->btn_link = button(a->menu_col, "", 96, 0x2AD8E8, menu_link_cb, a, &a->lbl_link);
    a->menu_row = row(a->menu_col, 16);
    lv_obj_t *g = button(a->menu_row, _("Garage"), 88, 0xFFD040, menu_garage_cb, a, NULL);
    lv_obj_t *s = button(a->menu_row, _("Ajustes"), 88, 0x9098A8, menu_settings_cb, a, NULL);
    lv_obj_set_flex_grow(g, 1);
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_width(g, 10);
    lv_obj_set_width(s, 10);
}

/* ---- stage select ---- */

static void lock_hint(char *b, size_t n, int s)
{
    int after = -1;
    int rule = tb_stage_unlock(s, &after);
    if (rule == UNL_TOUR) snprintf(b, n, "%s", _("Se abre con una gira"));
    else if (rule == UNL_AFTER && after >= 0) snprintf(b, n, "%s %s", _("Terminá"), tb_stage_name(after));
    else b[0] = 0;
}

static void stage_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    int s = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (!(a->unlocked & (1u << s))) {
        char h[64];
        lock_hint(h, sizeof h, s);
        snd(SND_NO);
        tba_toast(a, h);
        return;
    }
    if (a->mode == MODE_LINK) {
        a->stage = s;
        tbl_begin(a);
        return;
    }
    tba_race_start(a, s);
}

static void select_refresh(app_t *a)
{
    lv_label_set_text(a->sel_title, a->mode == MODE_LINK ? _("Elegí el tramo") : _("Contrarreloj"));
    for (int r = 0; r < STAGE_N; r++) {
        int i = tb_stage_order(r);
        lv_obj_t *b = lv_obj_get_child(a->sel_list, r);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        char t[24], rv[24], txt[112];
        bool open = (a->unlocked & (1u << i)) != 0;
        if (!open) {
            char h[64];
            lock_hint(h, sizeof h, i);
            snprintf(txt, sizeof txt, LV_SYMBOL_CLOSE " %s\n%s", tb_stage_name(i), h);
        } else {
            if (a->best[i]) fmt_time(t, sizeof t, (float)a->best[i] / 10.0f);
            else snprintf(t, sizeof t, "--:--");
            int n = snprintf(txt, sizeof txt, "%s\n%s %s", tb_stage_name(i), _("récord"), t);
            if (a->rival_best[i] && a->rival_name[0]) {
                fmt_time(rv, sizeof rv, (float)a->rival_best[i] / 10.0f);
                snprintf(txt + n, sizeof txt - (size_t)n, " · %s %s", a->rival_name, rv);
            }
        }
        lv_label_set_text(l, txt);
        lv_obj_set_style_border_color(b, lv_color_hex(open ? ACCENT : 0x505560), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(open ? 0xFFFFFF : 0x9098A8), 0);
    }
}

static void build_select(app_t *a, lv_obj_t *root)
{
    a->p_select = panel(root, 170);
    a->sel_title = label(a->p_select, "", aos_font_title, 0xFFFFFF);
    a->sel_list = column(a->p_select, 14);
    lv_obj_add_flag(a->sel_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->sel_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(a->sel_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_pad_bottom(a->sel_list, 24, 0);
    for (int r = 0; r < STAGE_N; r++) {
        lv_obj_t *l;
        lv_obj_t *b = button(a->sel_list, "", 112, ACCENT, stage_cb, a, &l);
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_user_data(b, (void *)(intptr_t)tb_stage_order(r));
        lv_obj_add_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    }
}

/* ---- garage ---- */

static void garage_paint_scene(app_t *a)
{
    scene(a, a->loaded_stage >= 0 ? a->loaded_stage : 0, a->g_car, a->g_paint, 0);
}

static void garage_refresh(app_t *a)
{
    const tb_car_spec_t *sp = tb_car_spec(a->g_car);
    lv_label_set_text(a->g_name, sp->name);
    const float v[4] = { (sp->vmax - 66.0f) / 22.0f, (sp->accel - 8.0f) / 5.0f, (sp->grip - 0.7f) / 0.7f,
                         (sp->offroad + sp->tough) / 1.5f };
    for (int i = 0; i < 4; i++) lv_bar_set_value(a->g_stats[i], (int32_t)(v[i] * 100.0f), LV_ANIM_OFF);
    char b[48];
    snprintf(b, sizeof b, "%d " LV_SYMBOL_BULLET, (int)a->coins);
    lv_label_set_text(a->g_coins, b);
    bool car_owned = (a->own_cars & (1u << a->g_car)) != 0;
    bool paint_owned = (a->own_paints & (1u << a->g_paint)) != 0;
    for (int i = 0; i < tb_paint_n() && i < 16; i++) {
        lv_obj_set_style_border_width(a->g_sw[i], i == a->g_paint ? 5 : 2, 0);
        lv_obj_set_style_border_color(a->g_sw[i], lv_color_hex(i == a->g_paint ? 0xFFFFFF : 0x606060), 0);
        lv_obj_set_style_bg_opa(a->g_sw[i], (a->own_paints & (1u << i)) ? LV_OPA_COVER : LV_OPA_40, 0);
    }
    int price = 0;
    if (!car_owned) price += sp->price;
    if (!paint_owned) price += tb_paint_price(a->g_paint);
    if (price) snprintf(b, sizeof b, "%s  %d " LV_SYMBOL_BULLET, _("Comprar"), price);
    else snprintf(b, sizeof b, "%s", (a->g_car == a->car && a->g_paint == a->paint[a->car]) ? _("Tu auto") : _("Elegir"));
    lv_label_set_text(a->g_btn_lbl, b);
}

static void garage_step(app_t *a, int d)
{
    a->g_car = (a->g_car + d + CAR_N) % CAR_N;
    a->g_paint = a->paint[a->g_car];
    snd(SND_TICK);
    garage_refresh(a);
    garage_paint_scene(a);
}

static void garage_prev_cb(lv_event_t *e) { garage_step(app_of(e), -1); }
static void garage_next_cb(lv_event_t *e) { garage_step(app_of(e), 1); }

static void swatch_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    a->g_paint = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    snd(SND_TICK);
    garage_refresh(a);
    garage_paint_scene(a);
}

static void garage_buy_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    const tb_car_spec_t *sp = tb_car_spec(a->g_car);
    bool car_owned = (a->own_cars & (1u << a->g_car)) != 0;
    bool paint_owned = (a->own_paints & (1u << a->g_paint)) != 0;
    int price = (car_owned ? 0 : sp->price) + (paint_owned ? 0 : tb_paint_price(a->g_paint));
    if (price > a->coins) {
        snd(SND_NO);
        tba_toast(a, _("No alcanzan las monedas"));
        return;
    }
    if (price) {
        a->coins -= price;
        a->own_cars |= 1u << a->g_car;
        a->own_paints |= 1u << a->g_paint;
        snd(SND_BUY);
    } else {
        snd(SND_TICK);
    }
    a->car = a->g_car;
    a->paint[a->car] = (uint8_t)a->g_paint;
    tba_prefs_save(a);
    garage_refresh(a);
}

static lv_obj_t *card(lv_obj_t *parent)
{
    lv_obj_t *c = column(parent, 14);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_radius(c, 0, 0);
    return c;
}

static void build_garage(app_t *a, lv_obj_t *root)
{
    a->p_garage = panel(root, 0);
    /* the car and its numbers */
    a->g_top = card(a->p_garage);
    lv_obj_t *hd = row(a->g_top, 12);
    lv_obj_set_flex_align(hd, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    a->g_title = lv_label_create(hd);
    lv_label_set_text(a->g_title, _("Garage"));
    lv_obj_set_style_text_font(a->g_title, aos_font_title, 0);
    lv_obj_set_style_text_color(a->g_title, lv_color_hex(0xFFD040), 0);
    a->g_coins = lv_label_create(hd);
    lv_obj_set_style_text_font(a->g_coins, aos_font_title, 0);
    lv_obj_set_style_text_color(a->g_coins, lv_color_hex(0xFFD040), 0);
    a->g_head = row(a->g_top, 12);
    a->g_prev = button(a->g_head, LV_SYMBOL_LEFT, 88, ACCENT, garage_prev_cb, a, NULL);
    lv_obj_set_width(a->g_prev, 96);
    a->g_name = lv_label_create(a->g_head);
    lv_obj_set_style_text_font(a->g_name, &aos_montserrat_48, 0);
    lv_obj_set_style_text_color(a->g_name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(a->g_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_flex_grow(a->g_name, 1);
    a->g_next = button(a->g_head, LV_SYMBOL_RIGHT, 88, ACCENT, garage_next_cb, a, NULL);
    lv_obj_set_width(a->g_next, 96);
    const char *names[4] = { _("Velocidad"), _("Aceleración"), _("Agarre"), _("Todo terreno") };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *r = row(a->g_top, 16);
        lv_obj_t *l = lv_label_create(r);
        lv_label_set_text(l, names[i]);
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xC8D0D8), 0);
        lv_obj_set_width(l, lv_pct(42));
        lv_obj_t *bar = lv_bar_create(r);
        lv_obj_set_height(bar, 18);
        lv_obj_set_flex_grow(bar, 1);
        lv_bar_set_range(bar, 0, 100);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x30343C), 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(ACCENT), LV_PART_INDICATOR);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        a->g_stats[i] = bar;
    }
    /* the paints and the button */
    a->g_bottom = card(a->p_garage);
    a->g_swbox = lv_obj_create(a->g_bottom);
    lv_obj_remove_style_all(a->g_swbox);
    lv_obj_set_size(a->g_swbox, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(a->g_swbox, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(a->g_swbox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(a->g_swbox, 18, 0);
    lv_obj_set_style_pad_row(a->g_swbox, 18, 0);
    lv_obj_remove_flag(a->g_swbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->g_swbox, LV_OBJ_FLAG_SCROLLABLE);
    int n = tb_paint_n();
    for (int i = 0; i < n && i < 16; i++) {
        tb_paint_t p;
        tb_paint_get(i, &p);
        lv_obj_t *s = lv_obj_create(a->g_swbox);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, 72, 72);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(p.c[RG_PAINT_A]), 0);
        lv_obj_set_style_bg_grad_color(s, lv_color_hex(p.c[RG_PAINT_B]), 0);
        lv_obj_set_style_bg_grad_dir(s, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s, 2, 0);
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(s, 8);
        lv_obj_set_user_data(s, (void *)(intptr_t)i);
        lv_obj_add_event_cb(s, swatch_cb, LV_EVENT_CLICKED, a);
        a->g_sw[i] = s;
    }
    a->g_btn = button(a->g_bottom, "", 96, 0xFFD040, garage_buy_cb, a, &a->g_btn_lbl);
    a->g_price = NULL;
}

/* ---- settings ---- */

static void chip_style(lv_obj_t *c, bool on)
{
    lv_obj_set_style_bg_color(c, lv_color_hex(on ? ACCENT : 0x14161C), 0);
}

static void settings_refresh(app_t *a)
{
    for (int i = 0; i < DIFF_N; i++) chip_style(a->chip_diff[i], i == a->diff);
    for (int i = 0; i < 3; i++) chip_style(a->chip_sens[i], i == a->sens);
    for (int i = 0; i < CTL_N; i++) chip_style(a->chip_ctl[i], i == a->ctl);
    chip_style(a->chip_sfx, s_sfx);
    chip_style(a->chip_auto, a->autogas);
}

static void diff_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    a->diff = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    settings_refresh(a);
    tba_prefs_save(a);
}

static void sens_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    a->sens = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    settings_refresh(a);
    tba_prefs_save(a);
}

static void ctl_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    a->ctl = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    settings_refresh(a);
    tba_prefs_save(a);
}

static void sfx_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    s_sfx = !s_sfx;
    a->sfx = s_sfx;
    if (s_sfx && !tb_audio_is_open()) tb_audio_open();
    if (!s_sfx) tb_audio_close();
    settings_refresh(a);
    tba_prefs_save(a);
}

static void auto_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    a->autogas = !a->autogas;
    settings_refresh(a);
    tba_prefs_save(a);
}

static void chips(app_t *a, lv_obj_t *col, const char *title, const char *const *names, int n, lv_obj_t **out,
                  lv_event_cb_t cb)
{
    lv_obj_t *l = label(col, title, aos_font_body, 0xC8D0D8);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_t *r = row(col, 14);
    for (int i = 0; i < n; i++) {
        out[i] = button(r, names[i], 84, ACCENT, cb, a, NULL);
        lv_obj_set_width(out[i], 10);
        lv_obj_set_flex_grow(out[i], 1);
        lv_obj_set_user_data(out[i], (void *)(intptr_t)i);
    }
}

static void build_settings(app_t *a, lv_obj_t *root)
{
    a->p_settings = panel(root, 200);
    a->set_col = column(a->p_settings, 14);
    lv_obj_add_flag(a->set_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(a->set_col, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(a->set_col, 24, 0);
    label(a->set_col, _("Ajustes"), aos_font_title, 0xFFFFFF);
    const char *dn[DIFF_N] = { _("Fácil"), _("Normal"), _("Difícil") };
    chips(a, a->set_col, _("Dificultad"), dn, DIFF_N, a->chip_diff, diff_cb);
    const char *cn[CTL_N] = { _("Volante"), _("Flechas") };
    chips(a, a->set_col, _("Dirección"), cn, CTL_N, a->chip_ctl, ctl_cb);
    const char *sn[3] = { _("Suave"), _("Medio"), _("Rápido") };
    chips(a, a->set_col, _("Sensibilidad"), sn, 3, a->chip_sens, sens_cb);
    lv_obj_t *r = row(a->set_col, 14);
    a->chip_auto = button(r, _("Acelerar solo"), 84, ACCENT, auto_cb, a, NULL);
    a->chip_sfx = button(r, _("Sonido"), 84, ACCENT, sfx_cb, a, NULL);
    lv_obj_set_width(a->chip_auto, 10);
    lv_obj_set_width(a->chip_sfx, 10);
    lv_obj_set_flex_grow(a->chip_auto, 1);
    lv_obj_set_flex_grow(a->chip_sfx, 1);
    label(a->set_col, _("Volante: arrastrá el pulgar izquierdo de costado. Pedales a la derecha: "
                        "freno y acelerador."), aos_font_small, 0x98A0A8);
}

/* ---- loading, results, pause, lobby ---- */

static void again_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    if (a->mode == MODE_LINK) {
        tbl_begin(a);
        return;
    }
    if (a->mode == MODE_TOUR) {
        tour_begin(a);
        return;
    }
    tba_race_start(a, a->stage);
}

static void next_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    if ((intptr_t)lv_obj_get_user_data(a->res_btn_next)) {
        a->tour_i++;
        tba_race_start(a, tb_tour_stage(a->tour_i));
        return;
    }
    if (a->mode == MODE_LINK) tbl_end(a);
    tba_set_state(a, ST_MENU);
}

static void resume_cb(lv_event_t *e) { resume(app_of(e)); }

static void restart_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    a->racing = false;
    if (a->mode == MODE_LINK) {
        resume(a);
        return;
    }
    race_go(a);
}

static void quit_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    a->racing = false;
    a->paused = false;
    if (a->mode == MODE_LINK) tbl_end(a);
    tba_set_state(a, ST_MENU);
}

static void lobby_cancel_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    tbl_end(a);
    tba_set_state(a, ST_MENU);
}

static void build_misc(app_t *a, lv_obj_t *root)
{
    a->p_loading = panel(root, 150);
    a->load_lbl = label(a->p_loading, "", aos_font_title, 0xFFFFFF);
    lv_obj_center(a->load_lbl);

    a->p_result = panel(root, 225);
    a->res_col = column(a->p_result, 20);
    a->res_title = label(a->res_col, "", &aos_montserrat_64, ACCENT);
    a->res_body = label(a->res_col, "", aos_font_body, 0xFFFFFF);
    lv_obj_set_style_text_line_space(a->res_body, 10, 0);
    lv_obj_t *r = row(a->res_col, 18);
    lv_obj_t *b1 = button(r, _("Otra vez"), 96, ACCENT, again_cb, a, NULL);
    a->res_btn_next = button(r, "", 96, 0x2AD8E8, next_cb, a, &a->res_btn_next_lbl);
    lv_obj_set_width(b1, 10);
    lv_obj_set_width(a->res_btn_next, 10);
    lv_obj_set_flex_grow(b1, 1);
    lv_obj_set_flex_grow(a->res_btn_next, 1);

    a->p_pause = panel(root, 170);
    a->pause_col = column(a->p_pause, 20);
    label(a->pause_col, _("Pausa"), &aos_montserrat_48, 0xFFFFFF);
    button(a->pause_col, _("Seguir"), 100, ACCENT, resume_cb, a, NULL);
    button(a->pause_col, _("Reiniciar"), 100, 0xFFD040, restart_cb, a, NULL);
    button(a->pause_col, _("Salir al menú"), 100, 0x9098A8, quit_cb, a, NULL);

    a->p_lobby = panel(root, 170);
    a->lobby_col = column(a->p_lobby, 24);
    label(a->lobby_col, _("Contra el otro reloj"), aos_font_title, 0x2AD8E8);
    a->lobby_lbl = label(a->lobby_col, "", aos_font_body, 0xFFFFFF);
    button(a->lobby_col, _("Cancelar"), 96, 0x9098A8, lobby_cancel_cb, a, NULL);

    a->p_boot = panel(root, 0);
    lv_obj_set_style_bg_color(a->p_boot, lv_color_hex(0x05070C), 0);
    lv_obj_set_style_bg_opa(a->p_boot, LV_OPA_COVER, 0);
    a->boot_title = label(a->p_boot, "TURBO", &aos_montserrat_64, ACCENT);
    a->boot_lbl = label(a->p_boot, _("Cargando..."), aos_font_body, 0xC8D0D8);
    a->boot_bar = lv_bar_create(a->p_boot);
    lv_obj_set_size(a->boot_bar, 440, 14);
    lv_bar_set_range(a->boot_bar, 0, 100);
    lv_obj_set_style_bg_color(a->boot_bar, lv_color_hex(0x30343C), 0);
    lv_obj_set_style_bg_color(a->boot_bar, lv_color_hex(ACCENT), LV_PART_INDICATOR);
}

/* Everything placed for the screen as it is now: upright the menus sit in
 * the sky over the car, lying down beside it; the lists are a centred
 * column; the garage's numbers above the car or to its left, its paints
 * below it or to its right. */
static void tba_layout(app_t *a)
{
    int W = AOS_SCREEN_W, H = AOS_SCREEN_H;
    bool land = W > H;
    int colw = W - 2 * 48 < 640 ? W - 2 * 48 : 640;

    lv_obj_set_size(a->touch, W, H);
    /* boot */
    lv_obj_set_width(a->boot_title, W);
    lv_obj_align(a->boot_title, LV_ALIGN_CENTER, 0, -90);
    lv_obj_set_width(a->boot_lbl, W);
    lv_obj_align(a->boot_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(a->boot_bar, LV_ALIGN_CENTER, 0, 60);
    /* the menu */
    lv_obj_set_size(a->menu_band, W, land ? 150 : 210);
    lv_obj_set_pos(a->menu_band, 0, 0);
    if (land) {
        lv_obj_set_size(a->menu_col, 400, LV_SIZE_CONTENT);
        lv_obj_align(a->menu_col, LV_ALIGN_RIGHT_MID, -28, 70);
    } else {
        lv_obj_set_size(a->menu_col, W - 2 * 72, LV_SIZE_CONTENT);
        lv_obj_align(a->menu_col, LV_ALIGN_TOP_MID, 0, 250);
    }
    /* the stage list */
    lv_obj_set_width(a->sel_title, W);
    lv_obj_align(a->sel_title, LV_ALIGN_TOP_MID, 0, land ? 16 : 40);
    lv_obj_set_size(a->sel_list, land ? 820 : W - 2 * 40, H - (land ? 80 : 120));
    lv_obj_align(a->sel_list, LV_ALIGN_TOP_MID, 0, land ? 72 : 110);
    /* the garage */
    if (land) {
        int side = W / 2 - 180 > 470 ? 470 : W / 2 - 180;
        lv_obj_set_size(a->g_top, side, H);
        lv_obj_set_pos(a->g_top, 0, 0);
        lv_obj_set_size(a->g_bottom, side, H);
        lv_obj_set_pos(a->g_bottom, W - side, 0);
        lv_obj_set_flex_align(a->g_bottom, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_flex_align(a->g_top, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    } else {
        lv_obj_set_size(a->g_top, W, LV_SIZE_CONTENT);
        lv_obj_set_pos(a->g_top, 0, 0);
        lv_obj_set_size(a->g_bottom, W, LV_SIZE_CONTENT);
        lv_obj_align(a->g_bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_flex_align(a->g_bottom, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_flex_align(a->g_top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
    /* settings */
    lv_obj_set_size(a->set_col, colw, H - (land ? 24 : 80));
    lv_obj_align(a->set_col, LV_ALIGN_TOP_MID, 0, land ? 12 : 60);
    /* results, pause, lobby */
    lv_obj_set_size(a->res_col, colw, LV_SIZE_CONTENT);
    lv_obj_center(a->res_col);
    lv_obj_set_size(a->pause_col, land ? 520 : colw, LV_SIZE_CONTENT);
    lv_obj_center(a->pause_col);
    lv_obj_set_size(a->lobby_col, colw, LV_SIZE_CONTENT);
    lv_obj_center(a->lobby_col);
    lv_obj_set_width(a->load_lbl, W);
    lv_obj_center(a->load_lbl);
}

/* --------------------------------------------------------------------------
 * Gestures, the timer
 * -------------------------------------------------------------------------- */

static void handle_gesture(app_t *a, int dir)
{
    uint32_t now = lv_tick_get();
    if ((uint32_t)(now - a->last_gesture_ms) < 400) return;
    a->last_gesture_ms = now;
    if (dir != LV_DIR_RIGHT) return;
    switch (a->state) {
    case ST_MENU: a->want_exit = true; break;
    case ST_SELECT: case ST_GARAGE: case ST_SETTINGS: tba_set_state(a, ST_MENU); break;
    default: break;
    }
}

static void gesture_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    lv_indev_t *indev = lv_indev_active();
    if (a->closing || !indev || a->state == ST_RACE) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT) handle_gesture(a, (int)dir);
}

static void boot_tick(app_t *a)
{
    int v = (int)lv_bar_get_value(a->boot_bar);
    if (v < 90) lv_bar_set_value(a->boot_bar, v + 2, LV_ANIM_OFF);
    if (a->job_done && a->job == JOB_NONE) {
        a->job_done = false;
        lv_bar_set_value(a->boot_bar, 100, LV_ANIM_OFF);
        canvas_show(a, 0);
        tba_set_state(a, ST_MENU);
        if (a->dev_go >= 0 && a->dev_go < STAGE_N) {
            a->mode = MODE_TRIAL;
            tba_race_start(a, a->dev_go);
        }
#ifdef AOS_SIM_BUILTIN
        /* Development switches (getenv() is NULL on the board):
         *   TB_STAGE=0..6 straight into a time trial of that stage
         *   TB_TOUR=1     straight into the tour
         *   TB_AUTO=1     the bot drives
         *   TB_COINS=n    coins for the garage
         *   TB_SCREEN=garage|settings|select
         *   TB_TURNED=1   lying down, draw the frames turned (as the board)
         */
        const char *e;
        if ((e = getenv("TB_COINS")) && e[0]) a->coins = atoi(e);
        if ((e = getenv("TB_AUTO")) && e[0]) a->autoplay = true;
        if ((e = getenv("TB_SCREEN")) && e[0]) {
            if (e[0] == 'g') tba_set_state(a, ST_GARAGE);
            else if (e[0] == 's' && e[1] == 'e' && e[2] == 't') tba_set_state(a, ST_SETTINGS);
            else if (e[0] == 's') { a->mode = MODE_TRIAL; tba_set_state(a, ST_SELECT); }
        }
        if ((e = getenv("TB_LINKGO")) && e[0]) {
            a->mode = MODE_LINK;
            a->unlocked = (1u << STAGE_N) - 1;
            a->stage = atoi(e) % STAGE_N;
            tbl_begin(a);
        } else if ((e = getenv("TB_TOUR")) && e[0]) {
            tour_begin(a);
        } else if ((e = getenv("TB_STAGE")) && e[0]) {
            a->mode = MODE_TRIAL;
            a->unlocked = (1u << STAGE_N) - 1;
            tba_race_start(a, atoi(e) % STAGE_N);
        }
#endif
    }
}

static bool app_back(aos_app_t *self, void *inst);

/* The pad outside the driving (touch_poll has that): START pauses a race
 * and resumes it, and on the title it presses the button picked; the d-pad
 * and A go through the panel's buttons; B is what the system's back is,
 * which never leaves the app from here (app_back). */
static void pad_ui(app_t *a)
{
    const aos_pad_t *p = &a->pad;
    if (!p->pressed && !p->repeat) return;
    if (a->closing || a->fitting || a->state == ST_BOOT) return;
    if (a->state == ST_RACE && !a->paused) {
        if (aos_pad_pressed(p, AOS_PAD_START)) pause_show(a);
        return;
    }
    if (aos_pad_menu_step(&a->pmenu, p)) return;
    if (aos_pad_pressed(p, AOS_PAD_START)) {
        lv_obj_t *o = aos_pad_menu_selected(&a->pmenu);
        if (a->state == ST_RACE) resume(a);
        else if (a->state == ST_MENU && o) lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
    } else if (aos_pad_pressed(p, AOS_PAD_B)) {
        app_back(a->self, a);
    } else if (a->state == ST_GARAGE && aos_pad_pressed(p, AOS_PAD_L | AOS_PAD_R)) {
        garage_step(a, aos_pad_pressed(p, AOS_PAD_L) ? -1 : 1);
    }
}

static void frame(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    /* A panel, the switcher or the gesture home over the game: the worker
     * waits (the game pauses) and nothing is pushed over them. Read here,
     * in LVGL's thread; the worker only looks at the number. */
    a->over = aos_ui_overlay() & ~(uint32_t)AOS_UI_OVER_TOAST;
    if (a->want_exit) {
        a->want_exit = false;
        aos_ui_back();
        return;
    }
    if ((aos_touch_gesture_t)aos_ui_take_gesture() == AOS_TOUCH_GESTURE_RIGHT && a->state != ST_RACE) {
        handle_gesture(a, LV_DIR_RIGHT);
    }
    uint64_t now = aos_hal_uptime_ms();
    int dt = a->prev_ms ? (int)(uint32_t)(now - a->prev_ms) : TICK_MS;
    if (dt > 100) dt = 100;
    a->prev_ms = now;
    a->st_ms += (uint32_t)dt;

    aos_pad_update(&a->pad, lv_tick_get());
    touch_poll(a, dt);
    pad_ui(a);
    tb_audio_tick();
    if (a->link_on) tbl_tick(a);

    /* the screen turned: the worker takes the new shape between frames */
    if (a->want_fit && a->job == JOB_NONE && a->state != ST_BOOT) {
        a->want_fit = false;
        a->fitting = true;
        job(a, JOB_FIT);
    }
    if (a->fitting && a->job_done && a->job == JOB_NONE) {
        a->job_done = false;
        a->fitting = false;
        if (a->state == ST_RACE || a->state == ST_RESULT) still_take(a);
        else canvas_show(a, 0);
    }
    if (a->fitting) return;
    if (s_stage_pending && a->job == JOB_NONE) {
        a->job_done = false;
        s_stage_pending = !job(a, JOB_STAGE);
        return;
    }
    if (a->still >= 0 && !a->want_still) still_take(a);
    if (s_scene_pending && a->job == JOB_NONE && !s_stage_pending && a->state != ST_LOADING &&
        a->state != ST_RACE && a->state != ST_RESULT) {
        a->job_done = false;
        s_scene_pending = !job(a, JOB_SCENE);
    }

    switch (a->state) {
    case ST_BOOT:
        boot_tick(a);
        break;
    case ST_MENU:
    case ST_GARAGE:
    case ST_SELECT:
    case ST_SETTINGS:
        if (a->job_done && a->job == JOB_NONE) {
            a->job_done = false;
            canvas_show(a, 0);
        }
        break;
    case ST_LOADING:
        if (a->job_done && a->job == JOB_NONE && !s_stage_pending && !tbl_hold_loading(a)) {
            a->job_done = false;
            if (a->loaded_stage == a->stage) race_go(a);
            else {
                tba_toast(a, _("No hay memoria para el tramo"));
                tba_set_state(a, ST_MENU);
            }
        }
        break;
    case ST_RACE:
        if (!a->paused) push_frame(a);
        race_tick(a);
        break;
    case ST_RESULT:
        if (a->mode == MODE_LINK && a->rival_done && !a->res_rival_seen) result_text(a);
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static bool app_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    switch (a->state) {
    case ST_MENU:
    case ST_BOOT:
        return false;
    case ST_RACE:
        if (a->paused) resume(a);
        else pause_show(a);
        return true;
    case ST_LOBBY:
        tbl_end(a);
        tba_set_state(a, ST_MENU);
        return true;
    case ST_LOADING:
        return true;
    case ST_RESULT:
        if (a->mode == MODE_LINK) tbl_end(a);
        tba_set_state(a, ST_MENU);
        return true;
    default:
        tba_set_state(a, ST_MENU);
        return true;
    }
}

/* The screen turned. A race pauses (the link's cannot: it keeps going), the
 * panels are laid out again at once, and the worker takes the new shape
 * when it can (JOB_FIT, frame()). Until then the canvas is hidden: its
 * buffer is about to be drawn again in the other shape. */
static bool app_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)root;
    app_t *a = (app_t *)inst;
    if (!a || a->closing) return false;
    if (a->state == ST_RACE && a->mode != MODE_LINK) pause_show(a);
    a->want_fit = true;
    a->want_still = false;
    a->still = -1;
    lv_obj_add_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
    tba_layout(a);
    return true;
}

static void turbo_hide(aos_app_t *self, void *inst)
{
    (void)self;
    if (inst) pause_show((app_t *)inst);
}

static void free_all(app_t *a)
{
    for (int i = 0; i < TB_NFB; i++) {
        free(a->fb[i]);
        a->fb[i] = NULL;
    }
    free(a->cv);
    a->cv = NULL;
    for (int i = 0; i < 2; i++) {
        free(a->band[i]);
        a->band[i] = NULL;
    }
    tb_track_free(&a->trk);
    tb_render_free(a->ren);
    a->ren = NULL;
    tb_hud_free(&a->hud);
    tb_art_close();
}

static void *turbo_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)tb_calloc(1, sizeof(app_t));
    if (!a) return NULL;
    a->self = self;
    uint32_t hi = 0, hp = 0;
    aos_hal_heap_info(&hi, &hp);
    aos_hal_log("turbo", "opening | internal %u B, psram %u B", (unsigned)hi, (unsigned)hp);
    bool ok = frames_alloc(a);
    a->ren = tb_render_new();
    if (!ok || !a->ren) {
        aos_hal_log("turbo", "out of memory");
        free_all(a);
        free(a);
        return NULL;
    }
    a->loaded_stage = -1;
    a->shown = -1;
    a->still = -1;
    a->root = root;
    prefs_load(a);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    a->canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(a->canvas, a->fb[0], AOS_SCREEN_W, AOS_SCREEN_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(a->canvas, 0, 0);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);

    /* over the frame while racing: it keeps LVGL's pointer off the panels
     * underneath; the fingers are read from the panel's samples */
    a->touch = lv_obj_create(root);
    lv_obj_remove_style_all(a->touch);
    lv_obj_set_size(a->touch, AOS_SCREEN_W, AOS_SCREEN_H);
    lv_obj_add_flag(a->touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->touch, LV_OBJ_FLAG_SCROLLABLE);

    hud_build(a);
    fit(a);
    build_menu(a, root);
    build_select(a, root);
    build_garage(a, root);
    build_settings(a, root);
    build_misc(a, root);
    for (int i = 0; i < 9; i++) pad_room(panel_of(a, i));
    tba_layout(a);

    lv_obj_remove_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(root, gesture_cb, LV_EVENT_GESTURE, a);

    /* a development switch on the card, for measuring on the board:
     * apps/turbo_dev.txt with "auto" (the bot drives), "unlock", "reset",
     * and "go N" (a time trial of stage N as soon as the app has loaded) */
    a->dev_go = -1;
    {
        char path[160], buf[64] = "";
        snprintf(path, sizeof path, "%s/turbo_dev.txt", aos_hal_path_apps());
        FILE *f = fopen(path, "r");
        if (f) {
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
            if (strstr(buf, "auto")) a->autoplay = true;
            if (strstr(buf, "unlock")) a->unlocked = (1u << STAGE_N) - 1;
            /* "noturn": lying down, frames go out unturned and the PPA turns
             * them (the way to compare the two on the board) */
            if (strstr(buf, "turn") && !strstr(buf, "noturn")) {
                a->dev_turn = true;
                fit(a);
            }
            if (strstr(buf, "noturn")) {
                a->dev_noturn = true;
                fit(a);
            }
            const char *go = strstr(buf, "go ");
            if (go) a->dev_go = (int8_t)atoi(go + 3);
            if (strstr(buf, "reset")) {
                a->coins = 0;
                a->own_cars = a->own_paints = 1;
                a->car = 0;
                memset(a->paint, 0, sizeof a->paint);
                a->unlocked = tb_stage_open_mask();
                a->best_tour = 0;
                memset(a->best, 0, sizeof a->best);
                memset(a->rival_best, 0, sizeof a->rival_best);
                a->rival_name[0] = 0;
                tba_prefs_save(a);
            }
            aos_hal_log("turbo", "dev switches: %s", buf);
        }
    }
    if (s_sfx) tb_audio_open();
    a->state = ST_BOOT;
    show_panel(a, a->p_boot);
    a->job_stage = STAGE_CITY;
    a->scene_car = a->car;
    a->scene_paint = a->paint[a->car];
    a->job = JOB_BOOT;
    /* core 0 beside the radio, LVGL on core 1: the render overlaps the push.
     * Priority 3, below LVGL's 4 (aos_hal.h: a busy worker at 5 there froze
     * the Cameras app's interface); the bands' other half runs on core 1 at
     * the same priority, taking what LVGL leaves */
    if (!aos_hal_worker_start_on("turbo", worker_fn, a, WORKER_STACK, 0, 3)) {
        aos_hal_log("turbo", "no worker");
    }
    a->timer = lv_timer_create(frame, TICK_MS, a);
    aos_hal_heap_info(&hi, &hp);
    aos_hal_log("turbo", "ready | internal %u B, psram %u B, hud %u KB, app %u B", (unsigned)hi, (unsigned)hp,
                (unsigned)(tb_hud_bytes(&a->hud) / 1024), (unsigned)sizeof(app_t));
    return a;
}

static void turbo_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    a->closing = true;
    a->racing = false;
    if (a->timer) lv_timer_delete(a->timer);
    aos_hal_worker_stop();
    tb_audio_close();
    tbl_end(a);
    if (a->root) lv_obj_clean(a->root);
    tba_prefs_save(a);
    free_all(a);
    free(a);
}

/* The launcher icon: a red wedge car from behind on a road to the horizon. */
static const uint8_t TURBO_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER,   0,  22, 84, 30, 4,          AIC_C_LIT(0x50545C), 255),
    AIC_RECT(AIC_CENTER,   0,  22,  4, 26, 1,          AIC_C_TEXT,          255),
    AIC_RECT(AIC_CENTER,   0,   2, 60, 22, 8,          AIC_C_LIT(0xE02020), 255),
    AIC_RECT(AIC_CENTER,   0,  -8, 36, 10, 4,          AIC_C_LIT(0x1A2230), 255),
    AIC_RECT(AIC_CENTER, -20,   6, 14,  5, 2,          AIC_C_LIT(0xFFB020), 255),
    AIC_RECT(AIC_CENTER,  20,   6, 14,  5, 2,          AIC_C_LIT(0xFFB020), 255),
    AIC_RECT(AIC_CENTER, -24,  16, 12,  8, 2,          AIC_C_LIT(0x101010), 255),
    AIC_RECT(AIC_CENTER,  24,  16, 12,  8, 2,          AIC_C_LIT(0x101010), 255),
    AIC_END
};

static bool turbo_init(aos_app_t *app)
{
    app->desc.id       = "demo.turbo";
    app->desc.name     = "Turbo";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, TURBO_ICON, sizeof TURBO_ICON);
    app->desc.color_a  = 0xE0501E;
    app->desc.color_b  = 0x3A1060;
    app->desc.order    = 159;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN |
                         AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    /* both orientations: no PORTRAIT / LANDSCAPE flag */
    app->create  = turbo_create;
    app->destroy = turbo_destroy;
    app->hide    = turbo_hide;
    app->back    = app_back;
    app->resize  = app_resize;
    return true;
}

AOS_APP_ENTRY(turbo_init);
