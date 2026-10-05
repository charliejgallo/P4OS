/*
 * P4OS (from AmoledOS) - Cameras: the pipeline between the socket and the
 * frame the UI shows. cam_view.h has the three frames and the lifetime.
 *
 * The lag is measured, not guessed (AmoledOS): the RTP clock says when the
 * camera took the picture, uptime says when it got here, and the smallest
 * difference ever seen is the pipe's own delay. Anything above that is
 * lateness. What to do about it depends on the codec:
 *
 *   H.264  every P frame is a reference of the next (cameras mark every
 *          slice nal_ref_idc=3), so a P frame cannot be skipped alone. Past
 *          LAG_H264_MS the view drops everything up to the next IDR and
 *          starts clean from there: with the camera at GOP = fps, at most a
 *          second of frozen picture.
 *   JPEG   every frame stands alone: the newest one wins. The sessions hand
 *          over every frame they complete, and the view decodes only the
 *          last one when the socket has been drained (cam_view_flush).
 */
#include "cam_view.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define LAG_H264_MS     700
#define RETRY_MS        3000
#define RETRY_SLOTS_MS  400     /* no free socket: one is being given back */
#define BAD_RUN_LIMIT   40      /* refused slices in a row with no picture yet */
#define JPEG_IN_CAP     (512 * 1024)
#define THUMB_JPEG_MS   180     /* the mosaic's MJPEG: ~4-5 fps at most */
#define THREAD_STACK    12288
/* Below LVGL's. AmoledOS measured what a busy camera thread above it does:
 * app_main holding the LVGL lock starved, and the picture froze for seconds
 * (APP-GUIDE section 14). */
#define THREAD_PRIO     3

static volatile int s_live;

int cam_view_live(void)
{
    return s_live;
}

bool cam_should_stop(const cam_view_t *v)
{
    return v->stop;
}

bool cam_view_stop_fn(void *view)
{
    return ((cam_view_t *)view)->stop;
}

int cam_tcp_connect(const char *host, int port, int timeout_ms, bool (*stop)(void *ctx), void *ctx)
{
    uint64_t t0 = aos_hal_uptime_ms();
    for (;;) {
        int r = aos_hal_tcp_connect(host, port, 1000);
        if (r > 0 || r == AOS_TCP_ERR_DNS || r == AOS_TCP_ERR_ARG || r == AOS_TCP_ERR_SLOTS) {
            return r;
        }
        /* refused (nothing listening) comes back at once: no point retrying it */
        if (aos_hal_uptime_ms() - t0 < 900 || (stop && stop(ctx)) ||
            aos_hal_uptime_ms() - t0 >= (uint64_t)timeout_ms) {
            return r;
        }
    }
}

/* ---- state for the UI ------------------------------------------------------- */

void cam_view_state(cam_view_t *v, cam_state_t st, const char *detail)
{
    /* "Not for this board" is final for the session: the SPS that says so
     * comes with the SDP, before PLAY, and the "waiting" that follows it
     * must not hide the message. */
    if (v->state == CAM_ST_UNSUPPORTED && st != CAM_ST_UNSUPPORTED) {
        return;
    }
    if (detail) {
        aos_hal_mutex_lock(v->mx);
        snprintf(v->detail, sizeof(v->detail), "%s", detail);
        v->detail_gen++;
        aos_hal_mutex_unlock(v->mx);
    }
    v->state = st;
}

void cam_view_bytes(cam_view_t *v, int n)
{
    v->bytes += (uint32_t)n;
}

void cam_view_codec(cam_view_t *v, cam_codec_t codec)
{
    v->codec = codec;
}

/* ---- the three frames ------------------------------------------------------- */

static bool same_target(const cam_target_t *a, const cam_target_t *b)
{
    return a->out_w == b->out_w && a->out_h == b->out_h && a->mode == b->mode && a->thumb == b->thumb;
}

static cam_target_t target(cam_view_t *v)
{
    aos_hal_mutex_lock(v->mx);
    cam_target_t t = v->want;
    aos_hal_mutex_unlock(v->mx);
    return t;
}

/* The back frame, at least w x h. */
static cam_frame_t *back_frame(cam_view_t *v, int w, int h)
{
    cam_frame_t *f = &v->fr[v->i_back];     /* only this thread moves i_back */
    size_t need = (size_t)w * h * 2;
    if (need > f->cap) {
        free(f->px);
        f->px = malloc(need);
        f->cap = f->px ? need : 0;
        if (!f->px) {
            return NULL;
        }
    }
    f->w = w;
    f->h = h;
    return f;
}

static void publish(cam_view_t *v)
{
    aos_hal_mutex_lock(v->mx);
    v->fr[v->i_back].seq = ++v->fseq;
    int t = v->i_ready;
    v->i_ready = v->i_back;
    v->i_back = t;
    v->fresh = true;
    aos_hal_mutex_unlock(v->mx);
}

const cam_frame_t *cam_view_take(cam_view_t *v)
{
    if (!v->fresh) {
        return NULL;
    }
    aos_hal_mutex_lock(v->mx);
    int t = v->i_front;
    v->i_front = v->i_ready;
    v->i_ready = t;
    v->fresh = false;
    const cam_frame_t *f = &v->fr[v->i_front];
    aos_hal_mutex_unlock(v->mx);
    return f->px ? f : NULL;
}

void cam_view_set_target(cam_view_t *v, int out_w, int out_h, cam_mode_t mode, bool thumb)
{
    aos_hal_mutex_lock(v->mx);
    if (v->want.out_w != out_w || v->want.out_h != out_h || v->want.mode != (int)mode ||
        v->want.thumb != thumb) {
        v->want.out_w = out_w;
        v->want.out_h = out_h;
        v->want.mode = mode;
        v->want.thumb = thumb;
        v->want_gen++;
    }
    aos_hal_mutex_unlock(v->mx);
}

void cam_view_snapshot(cam_view_t *v, const char *path)
{
    snprintf(v->snap_path, sizeof(v->snap_path), "%s", path);
    v->snap_result = 0;
    __sync_synchronize();
    v->snap_req = true;
}

static void picture_out(cam_view_t *v, int w, int h, uint32_t dec_ms, bool key)
{
    v->src_w = w;
    v->src_h = h;
    v->pictures++;
    v->dec_ms += dec_ms;
    if (key) {
        v->i_pics++;
        v->i_ms += dec_ms;
    }
    v->ever_picture = true;
    if (v->state != CAM_ST_LIVE) {
        cam_view_state(v, CAM_ST_LIVE, NULL);
    }
}

/* Lateness of a picture stamped pts_ms by the source's clock. */
static int32_t lateness(cam_view_t *v, int64_t pts_ms)
{
    int64_t off = (int64_t)aos_hal_uptime_ms() - pts_ms;
    if (!v->have_base || off < v->base_off) {
        v->have_base = true;
        v->base_off = off;
    }
    int32_t lag = (int32_t)(off - v->base_off);
    v->lag_ms = lag;
    return lag;
}

/* ---- H.264 ------------------------------------------------------------------ */

static void snapshot_i420(cam_view_t *v, const aos_h264_pic_t *pic)
{
    int w = pic->width & ~1, h = pic->height & ~1;
    uint16_t *px = malloc((size_t)w * h * 2);
    bool ok = false;
    if (px) {
        cam_geom_t g = { .sw = w, .sh = h, .ow = w, .oh = h };
        ok = cam_conv_i420(&v->conv, pic, &g, px) && aos_hal_jpeg_save(v->snap_path, px, w, h, w, 90);
        free(px);
    }
    v->snap_result = ok ? 1 : -1;
    v->snap_req = false;
}

void cam_view_nal(cam_view_t *v, const uint8_t *nal, int len, int64_t pts_ms)
{
    int sc = (len > 4 && nal[2] == 0) ? 4 : 3;
    if (len <= sc) {
        return;
    }
    int type = nal[sc] & 0x1F;
    bool slice = type == 1 || type == 5;

    if (v->state == CAM_ST_UNSUPPORTED) {
        return;                     /* said once; decoding would only fail again */
    }
    if (type == 7 && !v->have_sps && cam_sps_parse(nal + sc, len - sc, &v->sps)) {
        v->have_sps = true;
        aos_hal_log("camaras", "%s: sps profile %d level %d refs %d, %dx%d (coded %dx%d)", v->cam.name,
                    v->sps.profile_idc, v->sps.level_idc, v->sps.ref_frames,
                    v->sps.width, v->sps.height, v->sps.coded_w, v->sps.coded_h);
        v->src_w = v->sps.width;
        v->src_h = v->sps.height;
#ifndef AOS_SIM
        /* tinyh264 speaks constrained baseline only; libavcodec in the
         * simulator decodes anything, so there the stream just plays. */
        if (!cam_sps_baseline(&v->sps)) {
            char msg[112];
            const char *name = v->sps.profile_idc == 77 ? "Main" :
                               v->sps.profile_idc == 100 ? "High" : "?";
            snprintf(msg, sizeof(msg), _("H.264 %s: la placa sólo\ndecodifica el perfil Baseline."), name);
            cam_view_state(v, CAM_ST_UNSUPPORTED, msg);
            return;
        }
#endif
    }

    cam_target_t t = target(v);
    if (slice) {
        if (t.thumb && type != 5) {
            v->dropped++;           /* the mosaic: keyframes only */
            v->need_idr = true;
            return;
        }
        int32_t lag = pts_ms >= 0 ? lateness(v, pts_ms) : 0;
        if (type == 5) {
            v->skipping = false;
            v->need_idr = false;
        } else if (v->need_idr || v->skipping) {
            v->dropped++;
            return;
        } else if (lag > LAG_H264_MS) {
            aos_hal_log("camaras", "%s: late by %d ms, dropping to the next keyframe",
                        v->cam.name, (int)lag);
            v->skipping = true;
            v->skips++;
            v->dropped++;
            return;
        }
    }
    if (!v->h264) {
        v->h264 = aos_hal_h264_open();
        if (!v->h264) {
            cam_view_state(v, CAM_ST_UNSUPPORTED, _("Esta placa todavía no\ndecodifica H.264"));
            return;
        }
    }

    uint64_t t0 = aos_hal_uptime_ms();
    aos_h264_pic_t pic;
    int r = aos_hal_h264_decode(v->h264, nal, len, &pic);
    uint32_t dec_ms = (uint32_t)(aos_hal_uptime_ms() - t0);
    if (r == AOS_H264_ERR_MEM) {
        char msg[112];
        snprintf(msg, sizeof(msg), _("%dx%d no entra en\nla memoria de la placa."),
                 v->have_sps ? v->sps.width : 0, v->have_sps ? v->sps.height : 0);
        cam_view_state(v, CAM_ST_UNSUPPORTED, msg);
        return;
    }
    if (r < 0) {
        v->errors++;
        if (slice && !v->ever_picture && ++v->bad_run == BAD_RUN_LIMIT) {
            /* Every slice refused and not one picture: a stream this decoder
             * does not speak (Main/High, CABAC). Say so instead of "waiting". */
            v->undecodable = true;
            aos_hal_log("camaras", "%s: %d slices refused, no picture", v->cam.name, BAD_RUN_LIMIT);
        }
        return;
    }
    if (r == 0) {
        return;
    }
    v->bad_run = 0;
    /* tinyh264 ignores the SPS cropping: 640x360 comes out 640x368. */
    if (v->have_sps && v->sps.width <= pic.width && v->sps.height <= pic.height) {
        pic.width = v->sps.width;
        pic.height = v->sps.height;
    }
    picture_out(v, pic.width, pic.height, dec_ms, type == 5);

    if (v->snap_req) {
        snapshot_i420(v, &pic);
    }
    t0 = aos_hal_uptime_ms();
    cam_geom_t g;
    cam_geometry(pic.width, pic.height, t.out_w, t.out_h, (cam_mode_t)t.mode, &g);
    cam_frame_t *f = back_frame(v, g.ow, g.oh);
    if (f && cam_conv_i420(&v->conv, &pic, &g, f->px)) {
        v->conv_ms += (uint32_t)(aos_hal_uptime_ms() - t0);
        v->cur = t;
        publish(v);
    }
}

/* ---- JPEG --------------------------------------------------------------------- */

void cam_view_jpeg(cam_view_t *v, const uint8_t *jpeg, int len, int64_t pts_ms)
{
    if (!v->jpeg_in) {
        v->jpeg_in = malloc(JPEG_IN_CAP);
        v->jpeg_in_cap = v->jpeg_in ? JPEG_IN_CAP : 0;
    }
    if (len > v->jpeg_in_cap) {
        v->errors++;
        return;
    }
    if (v->jpeg_in_len) {
        v->dropped++;               /* a newer one came before this was decoded */
    }
    memcpy(v->jpeg_in, jpeg, (size_t)len);
    v->jpeg_in_len = len;
    if (pts_ms >= 0) {
        lateness(v, pts_ms);        /* measured and shown, not acted on */
    }
}

static void snapshot_jpeg(cam_view_t *v, const uint8_t *data, int len)
{
    /* The camera's own bytes: full size, no second compression. */
    bool ok = false;
    char dir[200];
    snprintf(dir, sizeof dir, "%s", v->snap_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        char *p = strrchr(dir, '/');
        if (p) {                    /* photos/ exists; camaras/ may not */
            *p = '\0';
            mkdir(dir, 0755);
            *p = '/';
        }
        mkdir(dir, 0755);
    }
    FILE *f = fopen(v->snap_path, "wb");
    if (f) {
        ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
        ok = (fclose(f) == 0) && ok;
    }
    v->snap_result = ok ? 1 : -1;
    v->snap_req = false;
}

void cam_view_flush(cam_view_t *v)
{
    if (!v->jpeg_in_len) {
        return;
    }
    int len = v->jpeg_in_len;
    cam_target_t t = target(v);
    uint64_t now = aos_hal_uptime_ms();
    if (v->snap_req) {
        snapshot_jpeg(v, v->jpeg_in, len);
    }
    v->jpeg_in_len = 0;
    if (t.thumb && same_target(&t, &v->cur) && v->ever_picture &&
        now - v->last_jpeg_ms < THUMB_JPEG_MS) {
        v->dropped++;               /* the mosaic's rate */
        return;
    }
    if (!v->jpeg) {
        v->jpeg = aos_hal_jpeg_open();
        if (!v->jpeg) {
            cam_view_state(v, CAM_ST_ERROR, _("El decodificador JPEG no abrió"));
            return;
        }
    }
    v->last_jpeg_ms = now;
    int w = 0, h = 0, stride = 0;
    const uint16_t *px = aos_hal_jpeg_decode(v->jpeg, v->jpeg_in, (size_t)len, &w, &h, &stride);
    uint32_t dec_ms = (uint32_t)(aos_hal_uptime_ms() - now);
    if (!px) {
        v->errors++;
        return;
    }
    picture_out(v, w, h, dec_ms, false);
    uint64_t t0 = aos_hal_uptime_ms();
    cam_geom_t g;
    cam_geometry(w, h, t.out_w, t.out_h, (cam_mode_t)t.mode, &g);
    cam_frame_t *f = back_frame(v, g.ow, g.oh);
    if (f && cam_conv_rgb565(&v->conv, px, stride, &g, f->px)) {
        v->conv_ms += (uint32_t)(aos_hal_uptime_ms() - t0);
        v->cur = t;
        publish(v);
    }
}

/* ---- the thread ------------------------------------------------------------- */

static void close_decoders(cam_view_t *v)
{
    if (v->h264) {
        aos_hal_h264_close(v->h264);
        v->h264 = NULL;
    }
    if (v->jpeg) {
        aos_hal_jpeg_close(v->jpeg);
        v->jpeg = NULL;
    }
}

static void view_free(cam_view_t *v)
{
    for (int i = 0; i < 3; i++) {
        free(v->fr[i].px);
    }
    free(v->jpeg_in);
    cam_conv_release(&v->conv);
    free(v);
    /* the mutex is not freed: the HAL has no call for it (a few bytes) */
}

static void sleep_unless_stopped(cam_view_t *v, int ms)
{
    for (int t = 0; t < ms && !v->stop; t += 50) {
        aos_hal_sleep_ms(50);
    }
}

static void cam_thread(void *arg)
{
    cam_view_t *v = arg;
    while (!v->stop) {
        char why[112] = "";
        /* A new session starts clean: its clock is another camera's, or the
         * same camera's after a reconnection. */
        v->have_base = false;
        v->skipping = false;
        v->need_idr = false;
        v->bad_run = 0;
        v->ever_picture = v->ever_picture && !v->url.rtsp;
        v->jpeg_in_len = 0;
        v->have_sps = false;
        bool polled = false;
        bool ok = v->url.usb  ? cam_usb_run(v, &v->cam, &v->url, why, sizeof(why))
                : v->url.rtsp ? cam_rtsp_run(v, &v->cam, &v->url, why, sizeof(why))
                              : cam_http_run(v, &v->cam, &v->url, why, sizeof(why), &polled);
        if (v->url.rtsp) {
            close_decoders(v);      /* the next session starts at a keyframe anyway */
        }
        if (v->stop) {
            break;
        }
        if (polled) {
            /* one picture and the server closed: ask again later */
            v->polled = true;
            if (v->cam.refresh_ms <= 0) {
                while (!v->stop) aos_hal_sleep_ms(100);
                break;
            }
            sleep_unless_stopped(v, v->cam.refresh_ms);
            continue;
        }
        if (ok) {
            break;
        }
        if (v->state == CAM_ST_UNSUPPORTED) {
            aos_hal_log("camaras", "%s: %s", v->cam.name, v->detail);
            while (!v->stop) aos_hal_sleep_ms(100);
            break;
        }
        aos_hal_log("camaras", "%s: %s", v->cam.name, why);
        cam_view_state(v, CAM_ST_ERROR, why);
        bool slots = strstr(why, _("Sin conexiones libres")) != NULL;
        sleep_unless_stopped(v, slots ? RETRY_SLOTS_MS : RETRY_MS);
    }
    close_decoders(v);
    __sync_fetch_and_sub(&s_live, 1);
    /* whichever lets go last frees it */
    if (!__sync_bool_compare_and_swap(&v->owner, 0, 1)) {
        view_free(v);
    }
}

cam_view_t *cam_view_new(const cam_t *cam, bool full)
{
    cam_view_t *v = calloc(1, sizeof(*v));
    if (!v) {
        return NULL;
    }
    v->mx = aos_hal_mutex_create();
    if (!v->mx) {
        free(v);
        return NULL;
    }
    v->cam = *cam;
    v->use_full = full && cam->url_full[0];
    v->i_back = 0;
    v->i_ready = 1;
    v->i_front = 2;
    v->want.out_w = 320;
    v->want.out_h = 180;
    v->want.mode = CAM_FILL;
    v->want.thumb = true;
    return v;
}

bool cam_view_start(cam_view_t *v)
{
    const char *url = v->use_full ? v->cam.url_full : v->cam.url;
    if (!cam_url_parse(url, &v->url)) {
        cam_view_state(v, CAM_ST_UNSUPPORTED, _("La dirección no es\nrtsp:// ni http://"));
        return false;
    }
    if (!v->cam.user[0] && v->url.user[0]) {
        snprintf(v->cam.user, sizeof v->cam.user, "%s", v->url.user);
        snprintf(v->cam.pass, sizeof v->cam.pass, "%s", v->url.pass);
    }
    v->state = CAM_ST_CONNECTING;
    __sync_fetch_and_add(&s_live, 1);
    if (!aos_hal_thread_start("cam", cam_thread, v, THREAD_STACK, THREAD_PRIO)) {
        __sync_fetch_and_sub(&s_live, 1);
        cam_view_state(v, CAM_ST_ERROR, _("No hay lugar para otra tarea"));
        v->owner = 1;               /* no thread: the UI frees it */
        return false;
    }
    return true;
}

void cam_view_release(cam_view_t *v)
{
    if (!v) {
        return;
    }
    v->stop = true;
    __sync_synchronize();
    if (!__sync_bool_compare_and_swap(&v->owner, 0, 2)) {
        view_free(v);               /* the thread is gone (or never ran) */
    }
}
