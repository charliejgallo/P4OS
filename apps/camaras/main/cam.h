/*
 * P4OS (from AmoledOS) - Cameras: what the pieces of the app share.
 *
 *   cam_cfg.c     cameras.txt on the card, and URL parsing
 *   cam_rtsp.c    RTSP over TCP: Digest, SDP, interleaved RTP
 *   cam_http.c    MJPEG over HTTP (go2rtc, Frigate, ffmpeg), single JPEGs
 *                 polled (a snapshot URL), and a plain GET for Frigate's API
 *   cam_depay.c   RTP payloads back into NAL units (RFC 6184) and JPEG
 *                 files (RFC 2435)
 *   cam_view.c    one camera's thread: session, decode, drop when late,
 *                 scale to the size the UI asked for, hand the frame over
 *   cam_conv.c    I420 and RGB565 to RGB565 at another size
 *   cam_frigate.c Frigate's HTTP API: cameras and recent events
 *   camaras.c     the screens and the app's life cycle
 *
 * On AmoledOS one worker ran one camera. Here every camera on screen has a
 * thread of its own (aos_hal_thread_start), because the mosaic shows them
 * all at once; none of them touches LVGL. The two sides meet in cam_view_t:
 * three frame buffers that change hands under a lock, and a status the
 * thread writes and the UI reads.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAM_MAX          8
#define CAM_NAME_LEN     48
#define CAM_URL_LEN      240
#define CAM_CRED_LEN     64

/* One camera, as cameras.txt says it (or as the Frigate tab makes it). */
typedef struct {
    char name[CAM_NAME_LEN];
    char url[CAM_URL_LEN];          /* the mosaic's; credentials taken out */
    char url_full[CAM_URL_LEN];     /* the full screen's, "" = the same */
    char user[CAM_CRED_LEN];
    char pass[CAM_CRED_LEN];
    int  refresh_ms;                /* a single-JPEG URL is asked again this often; 0 = once */
} cam_t;

/* A URL taken apart. */
typedef struct {
    bool rtsp;                      /* rtsp:// ; otherwise http:// */
    char host[96];
    int  port;
    char path[CAM_URL_LEN];         /* from the first '/', query included */
    char user[CAM_CRED_LEN];        /* if the URL carried them */
    char pass[CAM_CRED_LEN];
} cam_url_t;

typedef struct {
    cam_t cams[CAM_MAX];
    int   count;
    /* [frigate] */
    char  frigate[CAM_URL_LEN];     /* base URL, "" = none */
    char  frigate_user[CAM_CRED_LEN];
    char  frigate_pass[CAM_CRED_LEN];
} cam_cfg_t;

/* Reads cameras.txt (CAM_CONFIG in the environment overrides the path, for
 * development). Writes a commented template when there is no file, so the
 * person finds it in the portal's Archivos. false if there is no card. */
bool        cam_cfg_load(cam_cfg_t *cfg);
const char *cam_cfg_path(void);
/* Size and date of the file: the app reloads when they move. */
uint32_t    cam_cfg_stamp(void);
bool        cam_url_parse(const char *url, cam_url_t *out);

/* ---- what the network side hands to the decoding side ------------------ */

typedef enum {
    CAM_CODEC_NONE = 0,
    CAM_CODEC_H264,
    CAM_CODEC_JPEG,
} cam_codec_t;

typedef struct cam_view cam_view_t;

/* true once the UI let the camera go: every loop polls it. */
bool cam_should_stop(const cam_view_t *v);

/* aos_hal_tcp_connect() in one-second tries up to timeout_ms, giving up as
 * soon as 'stop' says so: a connect cannot be interrupted, and the app's
 * destroy() waits for every thread (its code is unloaded right after). A
 * camera on the LAN answers in milliseconds; a dead one no longer holds the
 * exit for the whole timeout. */
int  cam_tcp_connect(const char *host, int port, int timeout_ms, bool (*stop)(void *ctx), void *ctx);
bool cam_view_stop_fn(void *view);

/* One H.264 NAL unit, start code included, and the RTP time of its picture
 * in milliseconds (monotonic, extended from the 90 kHz clock). */
void cam_view_nal(cam_view_t *v, const uint8_t *nal, int len, int64_t pts_ms);

/* One whole JPEG file. pts_ms < 0 when the source has no clock (HTTP). */
void cam_view_jpeg(cam_view_t *v, const uint8_t *jpeg, int len, int64_t pts_ms);

/* JPEG: decodes the newest frame handed over since the last flush, if any.
 * The sessions call it whenever they have drained what the socket had. */
void cam_view_flush(cam_view_t *v);

/* Network accounting and state, from the thread. */
void cam_view_bytes(cam_view_t *v, int n);
void cam_view_codec(cam_view_t *v, cam_codec_t codec);

typedef enum {
    CAM_ST_IDLE = 0,
    CAM_ST_CONNECTING,
    CAM_ST_NEGOTIATING,             /* RTSP handshake / HTTP headers */
    CAM_ST_WAITING,                 /* connected, no picture yet */
    CAM_ST_LIVE,
    CAM_ST_ERROR,                   /* detail says why; the thread retries */
    CAM_ST_UNSUPPORTED,             /* detail says why; retrying will not help */
} cam_state_t;

void cam_view_state(cam_view_t *v, cam_state_t st, const char *detail);

/* ---- sessions (run until stop or failure; return false with a reason) --- */

bool cam_rtsp_run(cam_view_t *v, const cam_t *cam, const cam_url_t *url,
                  char *why, size_t why_len);
/* MJPEG or a single JPEG. *polled comes back true when the server sent one
 * picture and closed: the caller asks again after cam->refresh_ms. */
bool cam_http_run(cam_view_t *v, const cam_t *cam, const cam_url_t *url,
                  char *why, size_t why_len, bool *polled);

/* A whole GET into buf (NUL-terminated), for Frigate's API and pictures.
 * Returns the HTTP status (200...) with *len set, or < 0 (AOS_TCP_ERR_*).
 * 'stop' is polled between reads; NULL = never. */
int  cam_http_get(const cam_url_t *url, const char *user, const char *pass,
                  uint8_t *buf, int cap, int *len, int timeout_ms,
                  bool (*stop)(void *ctx), void *ctx);

/* Base64 (RFC 4648) for sprop-parameter-sets and HTTP Basic. */
int  cam_b64_decode(const char *in, int in_len, uint8_t *out, int out_max);
int  cam_b64_encode(const uint8_t *in, int in_len, char *out, int out_max);
