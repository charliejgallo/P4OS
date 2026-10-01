/*
 * P4OS - blittest: a bench for the direct blit and the worker (aos_hal.h).
 *
 * Not an app for people: it checks, on the board, what the simulator cannot
 * (there the blit returns false). A worker paints a test card into a frame
 * - four colour quadrants, a yellow bar along the top edge and a white
 * square in the top-left corner, so orientation and mirroring are plain to
 * see in a screenshot - and an LVGL timer blits it scaled x3, in
 * little-endian and in big-endian on alternate frames (the colours must not
 * change), and logs what each blit took. The log (portal /api/log) says:
 *
 *   blittest: 240x400 x3 LE 1234 us, BE 1250 us, worker frames 57
 *
 * Portrait fills 720x1200 from the top; landscape, 320x200 x3 = 960x600
 * centred, which is what Doom will do. A tap exits.
 */
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_ui.h"

#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCALE 3

typedef struct {
    int         w, h;           /* the frame, before scaling */
    uint16_t   *le, *be;        /* the same card in both byte orders */
    volatile uint32_t frames;   /* the worker's count, to see it runs */
    lv_timer_t *timer;
    uint32_t    n;
    uint64_t    le_us, be_us;
} bt_t;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

static void paint(bt_t *t)
{
    for (int y = 0; y < t->h; y++) {
        for (int x = 0; x < t->w; x++) {
            bool right = x >= t->w / 2, low = y >= t->h / 2;
            uint16_t c = !right && !low ? rgb565(220, 40, 40)      /* top-left red   */
                       :  right && !low ? rgb565(40, 200, 60)      /* top-right green */
                       : !right &&  low ? rgb565(40, 80, 230)      /* bottom-left blue */
                       :                  rgb565(230, 230, 230);   /* bottom-right grey */
            if (y < 6) c = rgb565(250, 220, 0);                    /* the top edge   */
            if (x < 24 && y < 24) c = rgb565(255, 255, 255);       /* top-left corner */
            t->le[y * t->w + x] = c;
            t->be[y * t->w + x] = (uint16_t)(c << 8 | c >> 8);
        }
    }
}

/* The worker only counts: what is tested is that it starts on the other
 * core, keeps running while LVGL blits, and stops when told. */
static void worker(void *arg)
{
    bt_t *t = arg;
    while (!aos_hal_worker_should_stop()) {
        t->frames++;
        aos_hal_worker_sleep(16);
    }
}

/* Lying down: the PPA turning into a panel buffer (blit_into), from a
 * whole 1280x720 frame in PSRAM, and from bands of internal RAM of 8, 16
 * and 24 rows; logged once. */
static void bench_into(bt_t *t)
{
    static bool done;
    if (done) return;
    done = true;
    int W = aos_hal_screen_w(), H = aos_hal_screen_h();
    uint16_t *frame = malloc((size_t)W * H * 2);
    uint16_t *back = aos_hal_display_back();
    if (!frame || !back) { aos_hal_log("blittest", "into: no frame or back buffer"); free(frame); return; }
    for (int i = 0; i < W * H; i++) frame[i] = t->le[(i / W % t->h) * t->w + (i % W % t->w)];
    uint64_t t0 = aos_hal_uptime_us();
    bool ok = aos_hal_display_blit_into(back, 0, 0, W, H, frame);
    aos_hal_log("blittest", "into: whole %dx%d from PSRAM %u us (%s)", W, H, (unsigned)(aos_hal_uptime_us() - t0), ok ? "ok" : "FAILED");
    if (!aos_hal_landscape()) {
        /* upright: whole rows 1:1, the AXI DMA's case, and the flip */
        t0 = aos_hal_uptime_us();
        ok = aos_hal_display_blit_into(back, 0, 0, W, H, frame);
        uint64_t t1 = aos_hal_uptime_us();
        aos_hal_display_flip(back);
        aos_hal_log("blittest", "into: whole %dx%d upright %u us (%s), flip %u us", W, H, (unsigned)(t1 - t0),
                    ok ? "ok" : "FAILED", (unsigned)(aos_hal_uptime_us() - t1));
        free(frame);
        return;
    }
    static const int rows[] = { 24, 16, 8 };
    for (int k = 0; k < 3; k++) {
        int r = rows[k];
        uint16_t *band = heap_caps_malloc((size_t)W * r * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!band) { aos_hal_log("blittest", "into: no internal band of %d rows", r); continue; }
        memcpy(band, frame, (size_t)W * r * 2);
        t0 = aos_hal_uptime_us();
        int n = 0;
        for (int y = 0; y + r <= H; y += r, n++) aos_hal_display_blit_into(back, 0, y, W, r, band);
        aos_hal_log("blittest", "into: %d bands of %d rows from internal RAM %u us", n, r, (unsigned)(aos_hal_uptime_us() - t0));
        heap_caps_free(band);
    }
    free(frame);
}

static void tick(lv_timer_t *tm)
{
    bt_t *t = lv_timer_get_user_data(tm);
    bench_into(t);
    int x = (aos_hal_screen_w() - t->w * SCALE) / 2;
    int y = aos_hal_landscape() ? (aos_hal_screen_h() - t->h * SCALE) / 2 : 0;
    bool be = t->n & 1;
    uint64_t t0 = aos_hal_uptime_us();
    bool ok = aos_hal_display_blit_scaled(x, y, t->w, t->h, be ? t->be : t->le, SCALE, be);
    uint64_t us = aos_hal_uptime_us() - t0;
    if (be) t->be_us += us; else t->le_us += us;
    if (!ok) aos_hal_log("blittest", "blit refused");
    if (++t->n % 60 == 0) {
        aos_hal_log("blittest", "%dx%d x%d LE %u us, BE %u us, worker frames %u", t->w, t->h, SCALE,
                    (unsigned)(t->le_us / 30), (unsigned)(t->be_us / 30), (unsigned)t->frames);
        t->le_us = t->be_us = 0;
    }
}

static void tap_cb(lv_event_t *e)
{
    (void)e;
    aos_ui_request_nav(AOS_UI_NAV_BACK);
}

static void *bt_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    bt_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->w = aos_hal_landscape() ? 320 : 240;
    t->h = aos_hal_landscape() ? 200 : 400;
    t->le = malloc((size_t)t->w * t->h * 2);
    t->be = malloc((size_t)t->w * t->h * 2);
    if (!t->le || !t->be) {
        free(t->le);
        free(t->be);
        free(t);
        return NULL;
    }
    paint(t);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, tap_cb, LV_EVENT_CLICKED, t);
    if (!aos_hal_worker_start("blittest", worker, t, 4096)) aos_hal_log("blittest", "no worker");
    t->timer = lv_timer_create(tick, 33, t);
    return t;
}

static void bt_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    bt_t *t = inst;
    lv_timer_delete(t->timer);
    aos_hal_worker_stop();
    aos_hal_log("blittest", "worker stopped: %s", aos_hal_worker_running() ? "NO" : "yes");
    free(t->le);
    free(t->be);
    free(t);
}

static bool bt_init(aos_app_t *app)
{
    app->desc.id      = "dev.blittest";
    app->desc.name    = "Blit test";
    app->desc.icon    = LV_SYMBOL_IMAGE;
    app->desc.color_a = 0x636366;
    app->desc.color_b = 0x2C2C2E;
    app->desc.order   = 250;
    app->create  = bt_create;
    app->destroy = bt_destroy;
    return true;
}

AOS_APP_ENTRY(bt_init);
