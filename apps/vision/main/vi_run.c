/*
 * P4OS - Vision: the worker. A thread of the app's own (the HAL's worker is
 * one for the whole system, and the camera may outlive a frame of LVGL):
 *
 *   take the newest JPEG -> decode it (the P4's JPEG engine) -> the mode:
 *   the engine's detector, or the code readers -> publish it.
 *
 * Publishing is a triple buffer: the screen shows one frame, another waits
 * finished, the worker fills the third; under vi.mx only to swap indexes.
 * A mode change is the worker's to carry out (load the pack, build the
 * models), between two frames. The portal's page gets the camera's own JPEG
 * and a JSON of what was found, only while it looks (aos_hal_live_*).
 */
#include "vision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void *vi_alloc(size_t n);
void vi_free(void *p);

vi_shared_t vi;

#define STACK_BYTES  (48 * 1024)
#define LIVE_IDLE_MS 2500
#define LIVE_MS      90          /* ~11 pictures a second to the page */
#define NO_FRAME_MS  5000

static const ve_api_t *s_api;
static ve_net_t *s_net;
static const uint8_t *s_blobs[2];
static int s_nblobs;
static aos_jpeg_t *s_jpeg;
static volatile bool s_done;

static void lock(void) { aos_hal_mutex_lock(vi.mx); }
static void unlock(void) { aos_hal_mutex_unlock(vi.mx); }

static void set_state(vi_state_t st, const char *err)
{
    lock();
    vi.state = st;
    if (err) snprintf(vi.err, sizeof vi.err, "%s", err);
    unlock();
    if (err && err[0]) aos_hal_log("vision", "%s", err);
}

static void net_free(void)
{
    if (s_net && s_api) s_api->close(s_net);
    s_net = NULL;
    vi_models_free(s_blobs, s_nblobs);
    s_nblobs = 0;
}

/* the mode asked for, ready to run: false keeps the state saying why not */
static bool switch_mode(vi_mode_t m)
{
    net_free();
    vi_codes_free();
    lock();
    vi.mode = m;
    vi.thr = 0;
    unlock();
    if (m == VI_M_CODES) {
        set_state(VI_ST_RUNNING, "");
        return true;
    }
    char err[96] = "";
    if (!vi_pack_present(m)) {
        set_state(VI_ST_NO_PACK, "");
        return false;
    }
    set_state(VI_ST_LOADING, "");
    if (!s_api && !(s_api = vi_engine(err, sizeof err))) {
        set_state(VI_ST_NO_ENGINE, err);
        return false;
    }
    uint32_t lens[2];
    s_nblobs = vi_models_load(m, s_blobs, lens, 2, err, sizeof err);
    if (s_nblobs <= 0) {
        s_nblobs = 0;
        set_state(VI_ST_NO_PACK, err);
        return false;
    }
    static const ve_kind_t KIND[] = {VE_K_FACE, VE_K_PERSON, VE_K_COCO, VE_K_HAND};
    uint64_t t0 = aos_hal_uptime_ms();
    s_net = s_api->open(KIND[m], s_blobs, lens, s_nblobs, err, sizeof err);
    if (!s_net) {
        vi_models_free(s_blobs, s_nblobs);
        s_nblobs = 0;
        set_state(VI_ST_MODEL_FAILED, err);
        return false;
    }
    if (vi.want_thr > 0) s_api->set_threshold(s_net, vi.want_thr);
    lock();
    vi.thr = s_api->threshold(s_net);
    unlock();
    aos_hal_log("vision", "%s ready in %u ms", vi_mode_pack(m), (unsigned)(aos_hal_uptime_ms() - t0));
    set_state(VI_ST_RUNNING, "");
    return true;
}

/* a frame of the buffer the screen is not showing and is not waiting */
static vi_frame_t *frame_to_fill(int *idx)
{
    lock();
    int i = 0;
    while (i == vi.shown || i == vi.ready) i++;
    unlock();
    *idx = i;
    return &vi.fr[i];
}

static void publish(int idx)
{
    lock();
    vi.ready = idx;
    unlock();
}

/* ---- the portal's page ---------------------------------------------------- */

static void live_put(const vi_frame_t *f, const uint8_t *jpeg, int len)
{
    static uint32_t last;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    vi.live = aos_hal_live_idle_ms(VI_ID) < LIVE_IDLE_MS;
    if (!vi.live || now - last < LIVE_MS) return;
    last = now;
    if (len <= AOS_LIVE_BLOB_MAX) {
        aos_hal_live_put(VI_ID, "frame", "image/jpeg", jpeg, (size_t)len);
    } else {
        /* a JPEG over the live channel's 64 KB (a webcam's 640x480 often is):
         * the picture small instead, "V565" w h (u16) then RGB565 rows. There
         * is no JPEG encoder into memory for apps (aos_hal_jpeg_save writes
         * a file): asked of the firmware. */
        enum { SW = 192, SH = 144 };
        static uint8_t small[8 + SW * SH * 2];
        uint16_t sw = SW, sh = SH;
        memcpy(small, "V565", 4);
        memcpy(small + 4, &sw, 2);
        memcpy(small + 6, &sh, 2);
        uint16_t *o = (uint16_t *)(small + 8);
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++) o[y * SW + x] = f->px[(y * f->h / SH) * f->w + x * f->w / SW];
        aos_hal_live_put(VI_ID, "frame", "application/octet-stream", small, sizeof small);
    }
    /* what was found, in the frame's pixels; the page names the classes */
    static char js[4096];
    int n = snprintf(js, sizeof js,
                     "{\"seq\":%u,\"mode\":%d,\"state\":%d,\"w\":%d,\"h\":%d,\"fps\":%.1f,\"ms\":%.1f,"
                     "\"model_ms\":%.1f,\"thr\":%.2f,\"src\":%d,\"boxes\":[",
                     (unsigned)f->seq, (int)f->mode, (int)vi.state, f->w, f->h, vi.fps, f->run_us / 1000.f,
                     f->t.model_us / 1000.f, vi.thr, (int)vi.src);
    for (int i = 0; i < f->nbox && n < (int)sizeof js - 120; i++) {
        const ve_box_t *b = &f->box[i];
        n += snprintf(js + n, sizeof js - n, "%s[%d,%d,%d,%d,%d,%.2f,%.2f]", i ? "," : "", b->x0, b->y0, b->x1,
                      b->y1, b->cls, b->score, b->cls_score);
    }
    n += snprintf(js + n, sizeof js - n, "],\"codes\":[");
    for (int i = 0; i < f->ncode && n < (int)sizeof js - 40; i++) {
        const vi_code_t *c = &f->code[i];
        n += snprintf(js + n, sizeof js - n, "%s{\"k\":%d,\"b\":[%d,%d,%d,%d],\"t\":\"", i ? "," : "", (int)c->kind,
                      c->x0, c->y0, c->x1, c->y1);
        /* the text, escaped for JSON */
        for (const char *p = c->text; *p && n < (int)sizeof js - 12; p++) {
            unsigned char ch = (unsigned char)*p;
            if (ch == '"' || ch == '\\') n += snprintf(js + n, sizeof js - n, "\\%c", ch);
            else if (ch < 0x20) n += snprintf(js + n, sizeof js - n, "\\u%04x", ch);
            else js[n++] = (char)ch;
        }
        n += snprintf(js + n, sizeof js - n, "\"}");
    }
    n += snprintf(js + n, sizeof js - n, "]}");
    aos_hal_live_put(VI_ID, "state", "application/json", js, (size_t)n);
}

/* ---- a photo ------------------------------------------------------------- */

static void draw_rect(uint16_t *px, int w, int h, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;
    for (int t = 0; t < 3; t++) {
        for (int x = x0; x <= x1; x++) {
            if (y0 + t < h) px[(y0 + t) * w + x] = c;
            if (y1 - t >= 0) px[(y1 - t) * w + x] = c;
        }
        for (int y = y0; y <= y1; y++) {
            if (x0 + t < w) px[y * w + x0 + t] = c;
            if (x1 - t >= 0) px[y * w + x1 - t] = c;
        }
    }
}

static void save_photo(const vi_frame_t *f)
{
    uint16_t *copy = (uint16_t *)vi_alloc((size_t)f->w * f->h * 2);
    bool ok = false;
    char path[160];
    if (copy) {
        memcpy(copy, f->px, (size_t)f->w * f->h * 2);
        for (int i = 0; i < f->nbox; i++)
            draw_rect(copy, f->w, f->h, f->box[i].x0, f->box[i].y0, f->box[i].x1, f->box[i].y1, 0x07E0);
        for (int i = 0; i < f->ncode; i++)
            draw_rect(copy, f->w, f->h, f->code[i].x0, f->code[i].y0, f->code[i].x1, f->code[i].y1, 0xFFE0);
        /* a name of its own: the time when the clock is set, a counter else */
        char name[48];
        if (aos_hal_time_is_valid()) {
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            snprintf(name, sizeof name, "vision_%04d%02d%02d_%02d%02d%02d.jpg", tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        } else {
            snprintf(name, sizeof name, "vision_%u.jpg", (unsigned)(aos_hal_uptime_ms() / 1000));
        }
        snprintf(path, sizeof path, "%.110s/%s", aos_hal_path_photos(), name);
        ok = aos_hal_jpeg_save(path, copy, f->w, f->h, f->w, 90);
        vi_free(copy);
    }
    lock();
    vi.photo_ok = ok;
    snprintf(vi.photo_path, sizeof vi.photo_path, "%s", ok ? path : "");
    if (ok) vi.photos++;
    vi.photo_seq++;
    unlock();
}

/* ---- the loop ----------------------------------------------------------------- */

static void worker(void *arg)
{
    (void)arg;
    s_jpeg = aos_hal_jpeg_open();
    bool test = vi.want_test;
    bool have_src = vi_src_open(test);
    lock();
    vi.src = vi_src_kind();
    snprintf(vi.src_name, sizeof vi.src_name, "%s", vi_src_name());
    unlock();
    if (!have_src) set_state(VI_ST_NO_SOURCE, "");

    vi_mode_t mode = (vi_mode_t)-1;
    bool mode_ok = false;
    uint64_t last_frame = aos_hal_uptime_ms(), fps_t0 = last_frame;
    int fps_n = 0;
    float last_thr = 0;

    while (!vi.stop) {
        /* the source: asked to change, gone, or (none yet) appeared */
        if (vi.want_test != test || (!have_src && aos_hal_uvc_count() > 0)) {
            vi_src_close();
            test = vi.want_test;
            have_src = vi_src_open(test);
            lock();
            vi.src = vi_src_kind();
            snprintf(vi.src_name, sizeof vi.src_name, "%s", vi_src_name());
            unlock();
            last_frame = aos_hal_uptime_ms();
            if (!have_src) set_state(VI_ST_NO_SOURCE, "");
            else if (mode_ok) set_state(VI_ST_RUNNING, "");
        }
        if (vi.want != mode) {
            mode = vi.want;
            mode_ok = switch_mode(mode);
            last_thr = vi.want_thr;
            if (!have_src && mode_ok) set_state(VI_ST_NO_SOURCE, "");
        }
        if (mode_ok && s_net && vi.want_thr != last_thr) {
            last_thr = vi.want_thr;
            if (last_thr > 0) s_api->set_threshold(s_net, last_thr);
            lock();
            vi.thr = s_api->threshold(s_net);
            unlock();
        }
        if (!have_src) {
            aos_hal_sleep_ms(200);
            if (aos_hal_uptime_ms() - last_frame > 2000) {
                /* test pictures copied while the app is open */
                last_frame = aos_hal_uptime_ms();
                have_src = vi_src_open(test);
                if (have_src) {
                    lock();
                    vi.src = vi_src_kind();
                    snprintf(vi.src_name, sizeof vi.src_name, "%s", vi_src_name());
                    unlock();
                    if (mode_ok) set_state(VI_ST_RUNNING, "");
                }
            }
            continue;
        }

        const uint8_t *jpeg;
        int len;
        int got = vi_src_next(&jpeg, &len);
        if (got < 0) {
            vi_src_close();
            have_src = false;
            lock();
            vi.src = VI_SRC_NONE;
            unlock();
            set_state(VI_ST_NO_SOURCE, "");
            continue;
        }
        if (got == 0) {
            if (aos_hal_uptime_ms() - last_frame > NO_FRAME_MS && vi.state == VI_ST_RUNNING)
                set_state(VI_ST_WAITING, "");
            aos_hal_sleep_ms(4);
            continue;
        }
        last_frame = aos_hal_uptime_ms();

        int w, h, stride;
        const uint16_t *px = aos_hal_jpeg_decode(s_jpeg, jpeg, (size_t)len, &w, &h, &stride);
        if (!px || w > VI_W || h > VI_H) {
            if (px) aos_hal_log("vision", "a %dx%d frame: larger than %dx%d", w, h, VI_W, VI_H);
            aos_hal_sleep_ms(10);
            continue;
        }
        int idx;
        vi_frame_t *f = frame_to_fill(&idx);
        for (int y = 0; y < h; y++) memcpy(f->px + (size_t)y * w, px + (size_t)y * stride, (size_t)w * 2);
        f->w = w;
        f->h = h;
        f->mode = mode;
        f->nbox = f->ncode = 0;
        memset(&f->t, 0, sizeof f->t);
        uint64_t t0 = aos_hal_uptime_us();
        if (mode_ok && mode == VI_M_CODES) {
            f->ncode = vi_codes_scan(f->px, w, h, f->code, VI_MAX_CODES);
        } else if (mode_ok && s_net) {
            int n = s_api->run(s_net, f->px, w, h, w, f->box, VI_MAX_BOXES, &f->t);
            f->nbox = n < 0 ? 0 : n;
        }
        f->run_us = (uint32_t)(aos_hal_uptime_us() - t0);
        static uint32_t seq;
        f->seq = ++seq;
        if (vi.state != VI_ST_RUNNING && mode_ok) set_state(VI_ST_RUNNING, "");
        publish(idx);
        live_put(f, jpeg, len);
        if (vi.want_photo) {
            vi.want_photo = false;
            save_photo(f);
        }

        fps_n++;
        uint64_t now = aos_hal_uptime_ms();
        if (now - fps_t0 >= 1000) {
            float fps = fps_n * 1000.f / (float)(now - fps_t0);
            lock();
            vi.fps = fps;
            unlock();
            static uint64_t logged;
            if (now - logged >= 5000) {
                logged = now;
                aos_hal_log("vision", "%s %dx%d: %.1f fps, mode %.1f ms (pre %.1f, model %.1f, post %.1f)",
                            vi_mode_pack(mode) ? vi_mode_pack(mode) : "codes", w, h, fps, f->run_us / 1000.f,
                            f->t.pre_us / 1000.f, f->t.model_us / 1000.f, f->t.post_us / 1000.f);
            }
            fps_n = 0;
            fps_t0 = now;
        }
    }

    net_free();
    vi_codes_free();
    vi_src_close();
    if (s_jpeg) aos_hal_jpeg_close(s_jpeg);
    s_jpeg = NULL;
    aos_hal_live_clear(VI_ID);
    s_done = true;
}

bool vi_run_start(void)
{
    if (!vi.mx) vi.mx = aos_hal_mutex_create();
    for (int i = 0; i < 3; i++) {
        if (!vi.fr[i].px) vi.fr[i].px = (uint16_t *)vi_alloc((size_t)VI_W * VI_H * 2);
        if (!vi.fr[i].px) return false;
    }
    vi.shown = vi.ready = -1;
    vi.stop = false;
    s_done = false;
    vi.state = VI_ST_IDLE;
    if (!aos_hal_thread_start("vision", worker, NULL, STACK_BYTES, 3)) return false;
    vi.running = true;
    return true;
}

/* waits for the worker: the module and this code go away after destroy() */
void vi_run_stop(void)
{
    if (vi.running) {
        vi.stop = true;
        for (int i = 0; i < 400 && !s_done; i++) aos_hal_sleep_ms(10);
        if (!s_done) {
            /* it still holds the frames and the engine: leave them to it */
            aos_hal_log("vision", "the worker did not stop");
            vi.running = false;
            return;
        }
    }
    vi.running = false;
    s_api = NULL;
    vi_engine_close();
    for (int i = 0; i < 3; i++) {
        vi_free(vi.fr[i].px);
        vi.fr[i].px = NULL;
    }
}
