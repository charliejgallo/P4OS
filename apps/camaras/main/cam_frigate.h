/*
 * P4OS - Cameras: Frigate's HTTP API, from a thread of its own.
 *
 *   GET /api/config                       the cameras (the keys of "cameras")
 *   GET /api/events?limit=N               the latest events
 *   GET /api/events/<id>/thumbnail.jpg    each event's thumbnail
 *
 * and three URLs the camera views open on their own (cam_frigate_cam(),
 * cam_frigate_event()):
 *
 *   /api/<camera>/latest.jpg?h=360        the mosaic's tile, asked every 2 s
 *   /api/<camera>?fps=10&h=720            the full screen: Frigate's MJPEG
 *   /api/events/<id>/snapshot.jpg         an event, full screen
 *
 * All JPEG: the board decodes them in hardware, whatever the cameras send
 * Frigate. Clips (MP4) are not played: the event only says it has one.
 *
 * The UI reads the results under the lock when 'gen' moves; thumbnails come
 * already scaled to thumb_w x thumb_h (RGB565, LVGL's order).
 */
#pragma once

#include "cam.h"

#define FG_MAX_CAMS     CAM_MAX
#define FG_MAX_EVENTS   24

typedef struct {
    char      id[48];
    char      camera[32];
    char      label[24];
    int64_t   start;            /* epoch seconds */
    bool      has_snapshot, has_clip;
    int       score;            /* percent, -1 unknown */
    uint16_t *thumb;            /* thumb_w x thumb_h, NULL until fetched */
} fg_event_t;

typedef enum { FG_IDLE = 0, FG_LOADING, FG_OK, FG_ERROR } fg_state_t;

typedef struct {
    cam_url_t     base;
    char          base_url[CAM_URL_LEN];
    char          user[CAM_CRED_LEN], pass[CAM_CRED_LEN];
    int           thumb_w, thumb_h;
    void         *mx;
    volatile bool stop;
    volatile int  owner;
    volatile bool refresh_req;

    /* under mx */
    volatile int  state;
    char          error[96];
    char          cams[FG_MAX_CAMS][32];
    int           ncams;
    fg_event_t    ev[FG_MAX_EVENTS];
    int           nev;
    volatile uint32_t gen;
} cam_frigate_t;

cam_frigate_t *cam_frigate_start(const char *url, const char *user, const char *pass,
                                 int thumb_w, int thumb_h);
void           cam_frigate_refresh(cam_frigate_t *f);
void           cam_frigate_release(cam_frigate_t *f);

/* A camera of Frigate's as a cam_t: the tile's snapshot URL and the full
 * screen's MJPEG. An event as a cam_t: its snapshot, fetched once. */
void cam_frigate_cam(const cam_frigate_t *f, const char *camera, cam_t *out);
void cam_frigate_event(const cam_frigate_t *f, const fg_event_t *e, const char *title, cam_t *out);
