/*
 * P4OS - Grabador: the camera's thread.
 *
 * Each frame, in order:
 *   1. the newest picture: the webcam's MJPEG decoded (aos_hal_uvc_* and
 *      aos_hal_jpeg_*), or in the simulator, without a camera, a test
 *      picture where somebody crosses every half minute;
 *   2. motion against the background (gr_motion.c), in the zones on;
 *   3. a copy for the screen, a few times a second, and the JPEG for the
 *      portal when its page asks;
 *   4. H.264 by the board's encoder (aos_hal_h264_enc_*), while armed, while
 *      recording or while someone watches by RTSP: one key frame a second,
 *      so a clip can start up to that much before the motion;
 *   5. the coded frame kept in a ring (the seconds before), handed to the
 *      RTSP viewers, and written to the clip when there is one.
 *
 * A clip opens on motion (armed, and inside the hours if there are hours)
 * or by hand, starting at the last key frame at least pre_s seconds back;
 * it closes post_s seconds after the last motion, and at clip_max_s it is
 * cut and goes on in a new one. Each clip has its first moving picture as a
 * .jpg beside it, a note to the Telegram bot when it is on, and the oldest
 * clips go when the folder passes its space.
 */
#include "gr.h"

#include <stdlib.h>
#include <sys/stat.h>

#define FRAME_CAP   (768 * 1024)    /* a webcam's JPEG */
#define RING_FRAMES 256
#define IDLE_ENC_MS 5000            /* the encoder closes this long after nobody needs it */
#define PREVIEW_MS  100

bool aos_bot_send(const char *text);    /* the firmware's Telegram bot (aos_bot.h) */

typedef struct { uint32_t off, len, ms; bool key; } ring_e_t;

static struct {
    void       *mx;
    volatile bool stop, running;
    gr_cfg_t    cfg;
    uint32_t    cfg_gen;
    gr_status_t st;
    bool        manual;
    /* the preview, for the screen */
    uint16_t   *prev;
    int         pw, ph;
    uint32_t    pseq;
    bool        want_jpeg;
} E;

/* the thread's own */
static uint8_t *s_jpg;
static uint16_t *s_test;
static uint8_t *s_ring;
static size_t s_ring_cap;
static ring_e_t s_re[RING_FRAMES];
static int s_re_first, s_re_n;
static uint32_t s_ring_head;

static void lock(void) { aos_hal_mutex_lock(E.mx); }
static void unlock(void) { aos_hal_mutex_unlock(E.mx); }
static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

static void set_state(int st, const char *why)
{
    lock();
    E.st.state = st;
    snprintf(E.st.why, sizeof E.st.why, "%s", why ? why : "");
    unlock();
}

/* ---------------------------------------------------------------- the ring */

static void ring_reset(void) { s_re_first = s_re_n = 0; s_ring_head = 0; }

static void ring_drop_oldest(void)
{
    s_re_first = (s_re_first + 1) % RING_FRAMES;
    s_re_n--;
}

/* bytes [off, off+len) overlap a kept frame's? */
static bool overlaps(uint32_t off, uint32_t len, const ring_e_t *e)
{
    return off < e->off + e->len && e->off < off + len;
}

static void ring_push(const uint8_t *d, int len, bool key, uint32_t ms)
{
    if (!s_ring || len <= 0 || (size_t)len > s_ring_cap / 2) return;
    uint32_t off = s_ring_head;
    if (off + (uint32_t)len > s_ring_cap) off = 0;          /* wrap: the tail is left */
    while (s_re_n && overlaps(off, (uint32_t)len, &s_re[s_re_first])) ring_drop_oldest();
    if (s_re_n == RING_FRAMES) ring_drop_oldest();
    memcpy(s_ring + off, d, (size_t)len);
    int at = (s_re_first + s_re_n) % RING_FRAMES;
    s_re[at] = (ring_e_t){ off, (uint32_t)len, ms, key };
    s_re_n++;
    s_ring_head = off + (uint32_t)len;
}

/* the first frame to write for a clip: the last key frame at least pre_ms
 * back, else the oldest key frame kept; -1 none */
static int ring_start(uint32_t now, uint32_t pre_ms)
{
    int best = -1;
    for (int k = 0; k < s_re_n; k++) {
        const ring_e_t *e = &s_re[(s_re_first + k) % RING_FRAMES];
        if (!e->key) continue;
        if (best < 0 || now - e->ms >= pre_ms) best = k;
        if (now - e->ms < pre_ms) break;
    }
    return best;
}

/* ---------------------------------------------------------------- the test picture */

#if defined(AOS_SIM)
/* a room: a gradient wall, a floor, a window; somebody crosses from 0 to 6 s
 * of every 30 */
static void test_picture(uint16_t *px, int w, int h, uint32_t ms)
{
    uint32_t t = ms % 30000;
    int person_x = t < 6000 ? (int)(t * (uint32_t)(w + 120) / 6000) - 120 : -1000;
    uint32_t noise = ms * 2654435761u;
    for (int y = 0; y < h; y++) {
        uint16_t *row = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int r, g, b;
            if (y > h * 7 / 10) { r = 90; g = 70; b = 50; }                          /* the floor */
            else if (x > w * 6 / 10 && x < w * 9 / 10 && y > h / 8 && y < h / 2) {   /* the window */
                r = 150; g = 190; b = 230;
            } else { r = 200 - y * 80 / h; g = 190 - y * 70 / h; b = 170 - y * 60 / h; }
            if (x >= person_x && x < person_x + 90 && y > h / 3 && y < h * 9 / 10) { r = 40; g = 50; b = 90; }
            noise = noise * 1103515245u + 12345u;
            int n = (int)((noise >> 16) & 7) - 3;
            r += n; g += n; b += n;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            row[x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        }
    }
}
#endif

/* ---------------------------------------------------------------- the clip */

typedef struct {
    gr_mp4w_t *mp4;
    char       name[32];
    uint32_t   t0, last_ms;
    bool       moving;              /* its first moving picture saved */
} clip_t;

static bool in_hours(const gr_cfg_t *c)
{
    if (!c->sched) return true;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year < 120) return true;      /* no clock yet: always */
    int hr = tm.tm_hour;
    return c->from_h <= c->to_h ? hr >= c->from_h && hr < c->to_h : hr >= c->from_h || hr < c->to_h;
}

static bool clip_open(clip_t *cl, int w, int h, uint32_t now, int pre_s)
{
    char dir[96], path[160];
    if (!gr_dir(dir, sizeof dir)) return false;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(cl->name, sizeof cl->name, "%04u%02u%02u-%02u%02u%02u.mp4", (unsigned)(tm.tm_year + 1900) % 10000u,
             (unsigned)(tm.tm_mon + 1) % 100u, (unsigned)tm.tm_mday % 100u, (unsigned)tm.tm_hour % 100u,
             (unsigned)tm.tm_min % 100u, (unsigned)tm.tm_sec % 100u);
    snprintf(path, sizeof path, "%s/%s", dir, cl->name);
    cl->mp4 = gr_mp4_create(path, w, h);
    if (!cl->mp4) return false;
    cl->t0 = now;
    cl->last_ms = 0;
    cl->moving = false;
    /* the seconds before, from the ring */
    int k0 = ring_start(now, (uint32_t)pre_s * 1000);
    if (k0 >= 0) {
        cl->t0 = s_re[(s_re_first + k0) % RING_FRAMES].ms;
        for (int k = k0; k < s_re_n; k++) {
            const ring_e_t *e = &s_re[(s_re_first + k) % RING_FRAMES];
            gr_mp4_frame(cl->mp4, s_ring + e->off, e->len, e->key, e->ms - cl->t0);
        }
    }
    aos_hal_log("grabador", "recording %s", cl->name);
    return true;
}

static void clip_close(clip_t *cl, int space_mb)
{
    if (!cl->mp4) return;
    uint32_t dur = 0;
    bool ok = gr_mp4_close(cl->mp4, &dur);
    cl->mp4 = NULL;
    aos_hal_log("grabador", "%s closed: %u s%s", cl->name, (unsigned)(dur / 1000), ok ? "" : " (it could not be written)");
    gr_clips_trim(space_mb);
}

static void clip_thumb(const clip_t *cl, const uint16_t *px, int w, int h, int stride)
{
    char dir[96], path[160];
    if (!gr_dir(dir, sizeof dir)) return;
    snprintf(path, sizeof path, "%s/%.15s.jpg", dir, cl->name);
    aos_hal_jpeg_save(path, px, w, h, stride, 70);
}

/* ---------------------------------------------------------------- the preview */

static void preview(const uint16_t *px, int w, int h, int stride)
{
    int ow = GR_PREV_W, oh = GR_PREV_W * h / w;
    if (oh > GR_PREV_H) { oh = GR_PREV_H; ow = GR_PREV_H * w / h; }
    lock();
    if (E.prev) {
        for (int y = 0; y < oh; y++) {
            const uint16_t *src = px + (size_t)(y * h / oh) * stride;
            uint16_t *dst = E.prev + (size_t)y * ow;
            for (int x = 0; x < ow; x++) dst[x] = src[x * w / ow];
        }
        E.pw = ow;
        E.ph = oh;
        E.pseq++;
    }
    unlock();
}

static void portal_jpeg(const uint8_t *jpg, int jn, const uint16_t *px, int w, int h, int stride)
{
    char path[128];
    snprintf(path, sizeof path, "%s/grabador.jpg", aos_hal_path_data());
    mkdir(aos_hal_path_data(), 0777);
    if (jpg && jn > 0) {
        char tmp[136];
        snprintf(tmp, sizeof tmp, "%s.part", path);
        FILE *f = fopen(tmp, "wb");
        if (!f) return;
        bool ok = fwrite(jpg, 1, (size_t)jn, f) == (size_t)jn;
        if (fclose(f) == 0 && ok) { remove(path); rename(tmp, path); }
        else remove(tmp);
    } else {
        aos_hal_jpeg_save(path, px, w, h, stride, 70);
    }
}

/* ---------------------------------------------------------------- the thread */

static void engine(void *arg)
{
    (void)arg;
    gr_cfg_t c;
    uint32_t gen = UINT32_MAX;
    gr_motion_t mo;
    gr_motion_reset(&mo);
    aos_jpeg_t *jd = NULL;
    aos_h264_enc_t *enc = NULL;
    int enc_w = 0, enc_h = 0, enc_fps = 0, enc_kbps = 0;
    uint32_t enc_idle = 0;
    bool cam = false;
    int cam_res = -1;
    uint32_t seq = 0, last_frame = 0, last_prev = 0, last_cam_try = 0, last_ok = 0;
    clip_t cl = { 0 };
    uint32_t last_motion = 0;
    uint32_t fps_t0 = now_ms(), fps_n = 0, bytes = 0;
    bool continuing = false;

    while (!E.stop) {
        uint32_t now = now_ms();
        lock();
        if (E.cfg_gen != gen) { c = E.cfg; gen = E.cfg_gen; }
        bool manual = E.manual;
        bool want_jpeg = E.want_jpeg;
        unlock();
        int cw, ch;
        gr_res_size(c.res, &cw, &ch);

        /* 1. the picture */
        const uint16_t *px = NULL;
        int w = 0, h = 0, stride = 0, jn = 0;
        bool test = false;
        if (cam && cam_res != c.res) { aos_hal_uvc_stop(); cam = false; }
        if (!cam && aos_hal_uvc_count() > 0 && now - last_cam_try > 2000) {
            last_cam_try = now;
            char name[48] = "";
            uint16_t sizes[1][2];
            int ns = 0;
            aos_hal_uvc_info(0, name, sizeof name, sizes, 0, &ns);
            if (aos_hal_uvc_start(0, cw, ch)) {
                cam = true;
                cam_res = c.res;
                lock();
                snprintf(E.st.camera, sizeof E.st.camera, "%s", name[0] ? name : _("Webcam USB"));
                unlock();
                set_state(GR_ST_STARTING, NULL);
                last_ok = now;
                gr_motion_reset(&mo);
            }
        }
        if (cam) {
            if (!jd) jd = aos_hal_jpeg_open();
            int n = aos_hal_uvc_frame(s_jpg, FRAME_CAP, &seq);
            if (n < 0) {
                aos_hal_uvc_stop();
                cam = false;
                set_state(GR_ST_NO_CAMERA, _("Se desenchufó la cámara"));
                continue;
            }
            if (n == 0) {
                if (now - last_ok > 5000) set_state(GR_ST_ERROR, _("La cámara no manda imagen"));
                aos_hal_sleep_ms(5);
                continue;
            }
            /* at most the frames a second asked for */
            if (now - last_frame < 1000u / (unsigned)c.fps - 5) continue;
            px = jd ? aos_hal_jpeg_decode(jd, s_jpg, (size_t)n, &w, &h, &stride) : NULL;
            if (!px) continue;
            jn = n;
            last_ok = now;
        } else {
#if defined(AOS_SIM)
            /* no camera in the simulator: the test picture */
            if (now - last_frame < 1000u / (unsigned)c.fps) { aos_hal_sleep_ms(5); continue; }
            if (!s_test) s_test = malloc((size_t)1280 * 720 * 2);
            if (!s_test) { aos_hal_sleep_ms(500); continue; }
            test_picture(s_test, cw, ch, now);
            px = s_test;
            w = cw;
            h = ch;
            stride = cw;
            test = true;
            lock();
            snprintf(E.st.camera, sizeof E.st.camera, "%s", _("Imagen de prueba"));
            unlock();
#else
            set_state(GR_ST_NO_CAMERA, _("Enchufá una webcam USB en el puerto de arriba (USB 2.0)"));
            if (cl.mp4) clip_close(&cl, c.space_mb);
            aos_hal_sleep_ms(200);
            continue;
#endif
        }
        last_frame = now;
        fps_n++;

        /* 2. motion */
        bool moving = gr_motion_feed(&mo, px, w, h, stride, c.sens, c.zones);
        bool trig = (moving && c.armed && in_hours(&c)) || manual;
        if (trig) last_motion = now;

        /* 3. the screen and the portal */
        if (now - last_prev >= PREVIEW_MS) { last_prev = now; preview(px, w, h, stride); }
        if (want_jpeg) {
            portal_jpeg(test ? NULL : s_jpg, jn, px, w, h, stride);
            lock();
            E.want_jpeg = false;
            unlock();
        }

        /* 4. H.264 while anybody needs it */
        int viewers = gr_rtsp_viewers();
        bool need = c.armed || manual || cl.mp4 || viewers > 0;
        if (need) enc_idle = now;
        if (enc && (enc_w != cw || enc_h != ch || enc_fps != c.fps || enc_kbps != c.kbps || now - enc_idle > IDLE_ENC_MS)) {
            if (cl.mp4) clip_close(&cl, c.space_mb);
            aos_hal_h264_enc_close(enc);
            enc = NULL;
            ring_reset();
        }
        if (!enc && need) {
            enc = aos_hal_h264_enc_open(cw, ch, c.fps, c.kbps * 1000, c.fps);
            if (!enc) { set_state(GR_ST_ERROR, _("El codificador H.264 está ocupado (¿se está grabando la pantalla?)")); aos_hal_sleep_ms(1000); continue; }
            enc_w = cw; enc_h = ch; enc_fps = c.fps; enc_kbps = c.kbps;
            ring_reset();
            if (!s_ring) {
                s_ring_cap = (size_t)(5 + 2) * 8000 * 1000 / 8;     /* 7 s at 8 Mbit/s: the most asked */
                s_ring = malloc(s_ring_cap);
                if (!s_ring) { s_ring_cap = (size_t)3 * 1024 * 1024; s_ring = malloc(s_ring_cap); }
                if (!s_ring) s_ring_cap = 0;
            }
        }
        if (enc) {
            const uint8_t *out = NULL;
            bool key = false;
            bool want_key = gr_rtsp_wants_key() || continuing;
            int n = aos_hal_h264_enc_frame(enc, px, w, h, stride, want_key, &out, &key);
            if (n > 0) {
                bytes += (uint32_t)n;
                ring_push(out, n, key, now);
                gr_rtsp_push(out, n, key, now, cw, ch);
                if (continuing && key && !cl.mp4) {
                    continuing = false;
                    if (clip_open(&cl, cw, ch, now, 0)) cl.moving = true;
                } else if (cl.mp4) {
                    gr_mp4_frame(cl.mp4, out, (uint32_t)n, key, now - cl.t0);
                }
            }
        }

        /* 5. the clip */
        if (enc && !cl.mp4 && !continuing && trig) {
            if (clip_open(&cl, cw, ch, now, c.pre_s)) {
                lock();
                E.st.clips_today++;
                E.st.last_motion = (uint32_t)time(NULL);
                unlock();
                aos_ui_status_icon_set(GR_APP_ID, AOS_SYM_RECORD, 0xFF453A);
                if (c.notify && !manual) {
                    char msg[96];
                    time_t t = time(NULL);
                    struct tm tm;
                    localtime_r(&t, &tm);
                    snprintf(msg, sizeof msg, _("🎥 Movimiento en la cámara: grabando (%02d:%02d)"), tm.tm_hour, tm.tm_min);
                    aos_bot_send(msg);
                }
            } else {
                set_state(GR_ST_ERROR, _("No se pudo escribir en la tarjeta"));
            }
        }
        if (cl.mp4 && trig && !cl.moving) { cl.moving = true; clip_thumb(&cl, px, w, h, stride); }
        if (cl.mp4) {
            bool quiet = !manual && now - last_motion > (uint32_t)c.post_s * 1000;
            bool too_long = now - cl.t0 > (uint32_t)c.clip_max_s * 1000;
            if (quiet || too_long) {
                clip_close(&cl, c.space_mb);
                aos_ui_status_icon_set(GR_APP_ID, NULL, 0);
                /* cut in two: the next one starts at the next key frame */
                if (too_long && !quiet) continuing = true;
            }
        }

        /* the status */
        uint32_t el = now - fps_t0;
        lock();
        if (el >= 2000) {
            E.st.fps = fps_n * 1000.0f / (float)el;
            E.st.kbps = bytes * 8 / el;
            fps_t0 = now;
            fps_n = 0;
            bytes = 0;
        }
        E.st.state = cl.mp4 ? GR_ST_RECORDING : GR_ST_WATCHING;
        E.st.why[0] = 0;
        E.st.w = cw;
        E.st.h = ch;
        E.st.level = mo.level;
        E.st.moving = moving;
        E.st.manual = manual;
        E.st.rec_ms = cl.mp4 ? now - cl.t0 : 0;
        snprintf(E.st.rec_file, sizeof E.st.rec_file, "%s", cl.mp4 ? cl.name : "");
        E.st.viewers = viewers;
        unlock();
    }
    if (cl.mp4) clip_close(&cl, c.space_mb);
    aos_ui_status_icon_set(GR_APP_ID, NULL, 0);
    if (enc) aos_hal_h264_enc_close(enc);
    if (cam) aos_hal_uvc_stop();
    if (jd) aos_hal_jpeg_close(jd);
    E.running = false;
}

/* ---------------------------------------------------------------- the API */

bool gr_engine_start(void)
{
    if (E.running) return true;
    if (!E.mx) E.mx = aos_hal_mutex_create();
    if (!s_jpg) s_jpg = aos_hal_io_alloc(FRAME_CAP);
    if (!E.prev) E.prev = malloc((size_t)GR_PREV_W * GR_PREV_H * 2);
    if (!E.mx || !s_jpg || !E.prev) return false;
    E.stop = false;
    E.running = true;
    E.st.state = GR_ST_STARTING;
    if (!aos_hal_thread_start("grabador", engine, NULL, 16384, 3)) { E.running = false; return false; }
    return true;
}

void gr_engine_stop(int wait_ms)
{
    E.stop = true;
    for (int t = 0; E.running && t < wait_ms; t += 20) aos_hal_sleep_ms(20);
    if (E.running) return;          /* it did not end: its buffers stay */
    aos_hal_io_free(s_jpg);
    s_jpg = NULL;
    free(s_test);
    s_test = NULL;
    free(s_ring);
    s_ring = NULL;
    s_ring_cap = 0;
    free(E.prev);
    E.prev = NULL;
}

void gr_engine_status(gr_status_t *out)
{
    if (!E.mx) { memset(out, 0, sizeof *out); return; }
    lock();
    *out = E.st;
    unlock();
}

void gr_engine_cfg(const gr_cfg_t *c)
{
    if (!E.mx) E.mx = aos_hal_mutex_create();
    lock();
    E.cfg = *c;
    E.cfg_gen++;
    unlock();
}

void gr_engine_record(bool on)
{
    if (!E.mx) return;
    lock();
    E.manual = on;
    unlock();
}

bool gr_engine_preview(uint16_t *px, int max_w, int max_h, int *w, int *h, uint32_t *seq)
{
    if (!E.mx) return false;
    lock();
    bool fresh = E.prev && E.pseq != *seq && E.pw <= max_w && E.ph <= max_h && E.pw > 0;
    if (fresh) {
        memcpy(px, E.prev, (size_t)E.pw * E.ph * 2);
        *w = E.pw;
        *h = E.ph;
        *seq = E.pseq;
    }
    unlock();
    return fresh;
}

void gr_engine_want_jpeg(void)
{
    if (!E.mx) return;
    lock();
    E.want_jpeg = true;
    unlock();
}
