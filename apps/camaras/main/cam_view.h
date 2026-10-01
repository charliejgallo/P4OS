/*
 * P4OS - Cameras: one camera's thread, and where it meets the UI.
 *
 * AmoledOS split each picture across the watch's two cores: the worker
 * decoded, and LVGL's timer converted the picture into strips pushed to the
 * panel. That handshake (the decoder's own picture handed over, the decoder
 * held back before N+2) existed because one S3 core could not do both at
 * 12 fps. Here every camera of the mosaic has its own thread, and the thread
 * does the whole job: session, decode, and the scale to the size the UI
 * asked for, into a frame of its own. The UI only swaps pointers:
 *
 *   back    the thread writes it
 *   ready   the newest whole frame, not yet taken
 *   front   the UI's: the lv_canvas points at it
 *
 * The thread finishes back, then swaps back <-> ready under the lock and
 * raises 'fresh'. The UI, from its timer, swaps ready <-> front under the
 * same lock when 'fresh' is up, and points its canvas at the new front. So
 * the newest frame always wins, neither side ever waits on the other, and
 * the UI never reads a frame being written. Each frame has its own size: the
 * thread (re)allocates the back one when the UI asks for another size (a
 * tile, the full screen, the screen turned), and the UI shows whatever size
 * it gets until the next one catches up.
 *
 * What the thread decodes depends on where the camera is shown:
 *
 *   mosaic       H.264: keyframes only (P frames skipped, never decoded):
 *                one refresh per GOP, a second with the camera at GOP =
 *                fps, and almost no CPU. JPEG: at most THUMB_JPEG_FPS.
 *   full screen  everything, with AmoledOS's lag policy (cam_view.c).
 *
 * Going from the mosaic to the full screen keeps the connection: the view
 * is re-targeted, and an H.264 view waits for the next keyframe (its P
 * frames need references it skipped) while the tile's frame stays up.
 *
 * Lifetime. cam_view_release() tells the thread to stop and hands it the
 * view: whichever of the two lets go last frees it (an atomic 'owner'), so
 * the UI never waits for a thread stuck in a 3 s connect. The app's
 * destroy() waits for all of them (cam_view_live() == 0): the code they run
 * belongs to the app.
 */
#pragma once

#include "cam.h"
#include "cam_conv.h"
#include "cam_sps.h"

#include "aos_hal.h"

typedef struct {
    uint16_t *px;
    int       w, h;
    size_t    cap;              /* bytes */
    uint32_t  seq;
} cam_frame_t;

typedef struct {
    int out_w, out_h;           /* the box the picture goes in */
    int mode;                   /* cam_mode_t */
    bool thumb;                 /* the mosaic's policy */
} cam_target_t;

struct cam_view {
    /* set before the thread starts */
    cam_t        cam;
    cam_url_t    url;
    bool         use_full;      /* cam.url_full rather than cam.url */

    /* what the UI asks for: under 'mx', read by the thread per picture */
    cam_target_t want;
    uint32_t     want_gen;

    /* snapshot: the UI sets the path and raises req; the thread answers */
    char              snap_path[200];
    volatile bool     snap_req;
    volatile int      snap_result;  /* 0 none yet, 1 saved, -1 failed */

    /* the three frames */
    void        *mx;
    cam_frame_t  fr[3];
    int          i_back, i_ready, i_front;
    volatile bool fresh;
    uint32_t     fseq;

    /* the thread */
    volatile bool stop;
    volatile int  owner;        /* 0 both, 1 thread gone, 2 UI gone */

    /* written by the thread, read by the UI */
    volatile int      state;    /* cam_state_t */
    char              detail[112];
    volatile uint32_t detail_gen;
    volatile int      codec;    /* cam_codec_t */
    volatile int      src_w, src_h;
    volatile bool     undecodable;
    volatile bool     have_audio;   /* the SDP offered an audio track (not played) */
    volatile bool     polled;       /* a snapshot URL: one picture per request */

    /* counters: the thread adds, the UI takes differences */
    volatile uint32_t pictures;         /* decoded */
    volatile uint32_t dropped;          /* not decoded, by policy */
    volatile uint32_t errors;           /* refused by a decoder */
    volatile uint32_t bytes;
    volatile uint32_t dec_ms;           /* summed over 'pictures' */
    volatile uint32_t conv_ms;          /* the scale, summed */
    volatile uint32_t i_pics, i_ms;     /* keyframes alone */
    volatile uint32_t skips;            /* times it gave up to the next IDR */
    volatile int32_t  lag_ms;

    /* the thread's own */
    cam_target_t cur;           /* the target the last frame was made for */
    aos_h264_t  *h264;
    aos_jpeg_t  *jpeg;
    cam_conv_t   conv;
    uint8_t     *jpeg_in;       /* the newest whole JPEG, waiting for flush */
    int          jpeg_in_len, jpeg_in_cap;
    uint64_t     last_jpeg_ms;
    bool         have_base;
    int64_t      base_off;
    bool         skipping;
    bool         need_idr;      /* coming out of the mosaic's keyframes-only */
    uint32_t     bad_run;
    bool         ever_picture;
    cam_sps_t    sps;
    bool         have_sps;
};

/* A new view for 'cam' (the full-screen URL when full is true). NULL if
 * there is no memory. */
cam_view_t *cam_view_new(const cam_t *cam, bool full);
bool        cam_view_start(cam_view_t *v);
/* Stop and let go; the view must not be touched after this. */
void        cam_view_release(cam_view_t *v);
/* Threads still running, released or not. */
int         cam_view_live(void);

/* ---- the UI's side ---------------------------------------------------------- */

void cam_view_set_target(cam_view_t *v, int out_w, int out_h, cam_mode_t mode, bool thumb);
/* The newest frame, if one came since the last call: the UI's until the
 * next call. NULL if nothing new. */
const cam_frame_t *cam_view_take(cam_view_t *v);
/* Asks for a snapshot to 'path' (a JPEG): the stream's own bytes for MJPEG,
 * the next picture encoded for H.264. */
void cam_view_snapshot(cam_view_t *v, const char *path);
