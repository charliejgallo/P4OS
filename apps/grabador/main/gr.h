/*
 * P4OS - Grabador: a USB webcam as a security camera.
 *
 * The pieces:
 *   gr_engine.c  the camera's thread: each frame decoded, looked at for
 *                motion, coded to H.264 (the board's encoder), kept a few
 *                seconds back, written to an MP4 while something moves, and
 *                handed to the RTSP viewers
 *   gr_motion.c  motion: a small grid of brightness against a slowly
 *                moving background, in the zones chosen
 *   gr_mp4.c     the MP4 writer (as the screen recording's); the Video app
 *                plays them (aos_ui_open_app_with)
 *   gr_rtsp.c    an RTSP server (RTP over the RTSP connection), for Frigate,
 *                VLC or the Cámaras app
 *   gr_cfg.c     the settings (<card>/grabador/grabador.txt)
 *   gr_ui.c      the screens: live, recordings, settings
 *   gr_live.c    the portal's page (web/grabador.js) through aos_hal_live_*
 *
 * Recordings: <card>/grabador/AAAAMMDD-HHMMSS.mp4, each with a .jpg of its
 * first moving frame beside it.
 */
#pragma once

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_sys_glyphs.h"
#include "aos_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define GR_APP_ID   "aos.grabador"
#define GR_DIR      "grabador"
#define GR_ZONES_W  4
#define GR_ZONES_H  3
#define GR_PREV_W   720             /* the live picture, at most */
#define GR_PREV_H   540

/* ---------------------------------------------------------------- settings */

typedef struct {
    bool     armed;                 /* records when something moves */
    int      res;                   /* 0: 640x480, 1: 1280x720 */
    int      fps;                   /* 5, 10, 15 */
    int      kbps;                  /* the encoder's bit rate */
    int      sens;                  /* 0 low, 1 medium, 2 high */
    int      pre_s, post_s;         /* seconds kept before, recorded after */
    int      clip_max_s;            /* a clip longer than this is cut in two */
    int      space_mb;              /* the oldest go beyond this */
    bool     sched;                 /* only between from_h and to_h */
    int      from_h, to_h;
    uint16_t zones;                 /* a bit per cell of GR_ZONES_W x GR_ZONES_H, row by row */
    bool     rtsp;                  /* the RTSP server */
    int      rtsp_port;
    bool     notify;                /* a message by the Telegram bot */
} gr_cfg_t;

void gr_cfg_defaults(gr_cfg_t *c);
void gr_cfg_load(gr_cfg_t *c);
void gr_cfg_save(const gr_cfg_t *c);
void gr_res_size(int res, int *w, int *h);
const char *gr_dir(char *out, size_t n);        /* <card>/grabador, created; NULL without card */

/* ---------------------------------------------------------------- motion */

typedef struct {
    float    bg[32 * 24];
    bool     ready;
    uint32_t frames;
    float    level;                 /* 0..1: the share of the zones' cells that moved */
    uint8_t  moved[GR_ZONES_W * GR_ZONES_H];    /* cells that moved in each zone, last frame */
} gr_motion_t;

void gr_motion_reset(gr_motion_t *m);
/* true when it counts as motion at this sensitivity in these zones */
bool gr_motion_feed(gr_motion_t *m, const uint16_t *px, int w, int h, int stride, int sens, uint16_t zones);

/* ---------------------------------------------------------------- MP4 */

typedef struct gr_mp4w gr_mp4w_t;
gr_mp4w_t *gr_mp4_create(const char *path, int w, int h);
bool gr_mp4_frame(gr_mp4w_t *m, const uint8_t *annexb, uint32_t len, bool key, uint32_t ms);
bool gr_mp4_close(gr_mp4w_t *m, uint32_t *duration_ms);

/* ---------------------------------------------------------------- engine */

enum { GR_ST_OFF = 0, GR_ST_NO_CAMERA, GR_ST_STARTING, GR_ST_WATCHING, GR_ST_RECORDING, GR_ST_ERROR };

typedef struct {
    int      state;                 /* GR_ST_* */
    char     why[96];
    char     camera[48];            /* its name, or the test picture's */
    int      w, h;                  /* what is coded */
    float    fps;                   /* frames a second, measured */
    float    level;                 /* motion, 0..1 */
    bool     moving;
    bool     manual;                /* recording because someone asked */
    uint32_t rec_ms;                /* how long the clip being written is */
    char     rec_file[48];          /* its name */
    uint32_t clips_today;
    uint32_t last_motion;           /* time(NULL) of the last motion, 0 none */
    int      viewers;               /* RTSP */
    uint32_t kbps;                  /* what the encoder gives, measured */
} gr_status_t;

bool gr_engine_start(void);
void gr_engine_stop(int wait_ms);   /* the loader unloads the .so: the threads must be gone */
void gr_engine_status(gr_status_t *out);
void gr_engine_cfg(const gr_cfg_t *c);          /* new settings, taken at the next frame */
void gr_engine_record(bool on);                 /* by hand */
/* The newest picture for the screen, at most GR_PREV_W x GR_PREV_H: copied
 * into px (RGB565), its size in *w, *h; false when there is none newer than
 * *seq. */
bool gr_engine_preview(uint16_t *px, int max_w, int max_h, int *w, int *h, uint32_t *seq);
/* The newest frame as a JPEG file for the portal (<data>/grabador.jpg), when
 * asked: true once written. */
void gr_engine_want_jpeg(void);

/* ---------------------------------------------------------------- RTSP */

bool gr_rtsp_start(int port);
void gr_rtsp_stop(void);
int  gr_rtsp_viewers(void);
bool gr_rtsp_wants_key(void);       /* a viewer is waiting for a key frame */
void gr_rtsp_push(const uint8_t *annexb, int len, bool key, uint32_t ms, int w, int h);

/* ---------------------------------------------------------------- recordings */

#define GR_CLIPS_MAX 200
typedef struct {
    char     name[32];              /* AAAAMMDD-HHMMSS.mp4 */
    uint32_t bytes;
    time_t   when;
} gr_clip_t;

int  gr_clips(gr_clip_t *out, int max);         /* newest first */
bool gr_clip_delete(const char *name);
void gr_clips_trim(int space_mb);               /* the oldest go until they fit */

/* ---------------------------------------------------------------- app */

typedef struct {
    aos_app_t *self;
    lv_obj_t  *root;
    bool       hidden;
    gr_cfg_t   cfg;
} gr_app_t;
extern gr_app_t gr_app;

void gr_ui_build(void);
void gr_ui_tick(void);
bool gr_ui_back(void);
void gr_ui_free(void);

void gr_live_tick(void);
