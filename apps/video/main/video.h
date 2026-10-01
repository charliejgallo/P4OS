/*
 * P4OS (from AmoledOS) - Video: plays MJPEG AVIs from the card, with sound.
 *
 * The shape of it, and what changed from the watch:
 *
 *   - The file is an AVI of JPEG frames (vd_avi.c), at any size up to the
 *     screen's; apps/video/convert.sh writes them. The sound is a WAV (or an
 *     MP3) beside it with the same name, played by the firmware's player
 *     (aos_hal_player_play). The app never touches audio hardware: it asks
 *     the player where it is and follows.
 *
 *   - The AUDIO IS THE CLOCK. The P4's player reports its position to the
 *     millisecond (aos_hal_player_info), so the app keeps a clock of its own
 *     and pulls it to the sound whenever the two drift apart by more than a
 *     frame's worth. A frame is shown when its time comes; when the app
 *     falls behind, the frames in between are never read nor decoded. A
 *     dropped frame is invisible, a click in the sound is not.
 *
 *   - Three stages, each on its own: a READER thread (vd_play.c) reads the
 *     frames' JPEGs off the card into a ring of packets, the app's WORKER
 *     decodes them into a ring of frames, and LVGL's timer blits each one
 *     when its time comes. The watch read and decoded one after the other
 *     in the worker; here the card and the JPEG engine work at the same
 *     time, so a frame costs the slower of the two and not their sum.
 *     Plain flags, one writer each: a packet goes FREE -> FULL (reader) ->
 *     FREE (worker); a frame FREE -> BUSY -> READY (worker) -> SHOWN -> FREE
 *     (UI).
 *
 *   - The decoder is the HAL's (aos_hal_jpeg_*): on the board the P4's JPEG
 *     engine, esp_new_jpeg in software for what it refuses; libavcodec in
 *     the simulator. One handle per frame slot, because the pixels are the
 *     handle's until its next decode: that is exactly a slot's life.
 *
 *   - To the screen by aos_hal_display_blit_fit(): the PPA scales to the
 *     biggest size that fits (in its steps of 1/16, up or down), reads the
 *     engine's padded rows as they are, turns for the orientation and
 *     copies into the framebuffer in one pass; a whole-number scale of
 *     packed rows takes aos_hal_display_blit_scaled(). 1280x720 upright is
 *     720x405, the watch's 368x448 is 713x868. With the controls up only the
 *     rows between the two bars are blitted, so LVGL's bars are never
 *     painted over. In the simulator the blit says no, and the frame goes
 *     through an lv_image instead: same geometry, same screens.
 *
 *   - Page flipping (vd_play.c, flip_frame): with the bars down and nothing
 *     else of LVGL's over the picture, the frame goes into the panel's free
 *     buffer (aos_hal_display_blit_into_fit) and that buffer is flipped to,
 *     so a frame is never shown half old, half new. The letterbox around
 *     the picture is cleared the first time a buffer is used, and again
 *     when LVGL drew into it or the picture's place changed
 *     (aos_hal_display_back_age). With the bars up, the message, or a
 *     toast, the frame goes into the buffer on screen as before: a flip
 *     would hide what LVGL drew there. A 1:1 frame upright the screen's
 *     width is flipped too: its rows go into the free buffer by the AXI
 *     DMA (19 ms for 720x1280), the few rows off a cache line by the CPU
 *     (rows_into). Narrower 1:1 upright stays the DMA2D copy into the
 *     buffer on screen, since the PPA would be slower.
 *
 *   - Under the system's UI (aos_ui_overlay: a panel, the switcher, a
 *     banner, the zoom, a gesture) the playback pauses, and plays on when
 *     it goes if it was playing. Under a toast it plays on.
 *
 * Every GEN (a number the UI bumps on each seek) tags packets and frames:
 * whatever belongs to an older one is dropped on sight, so a seek never
 * waits for the pipeline to drain.
 */
#pragma once

#include "aos_hal.h"
#include "lvgl.h"

#include "vd_avi.h"

#include <stdbool.h>
#include <stdint.h>

#define VD_MAX_FILES    64
#define VD_NAME_LEN     96
#define VD_SLOTS        3           /* decoded frames: one on screen, two ahead */
#define VD_PKTS         6           /* compressed ones read ahead of the decoder */
#define VD_MAX_SCALE    4
#define VD_THUMB_W      256
#define VD_THUMB_H      144

enum { SLOT_FREE = 0, SLOT_BUSY, SLOT_READY, SLOT_SHOWN };
enum { PKT_FREE = 0, PKT_FULL };
enum { OPEN_NONE = 0, OPEN_BUSY, OPEN_OK, OPEN_BAD, OPEN_NOMEM, OPEN_NOTASK };

typedef struct {
    volatile int    state;
    aos_jpeg_t     *jpeg;       /* this slot's decoder: its pixels are ours until FREE */
    const uint16_t *px;         /* the decoder's pixels, LVGL's byte order */
    int             w, h;
    int             stride;     /* pixels per row: the engine pads to 16 */
    int32_t         index;      /* the frame's number in the file */
    uint32_t        gen;
    uint32_t        read_ms, decode_ms;
    lv_image_dsc_t  dsc;        /* the LVGL path (simulator) */
} vd_slot_t;

typedef struct {
    volatile int    state;
    uint8_t        *buf;        /* aos_hal_io_alloc: the card reads straight into it */
    const uint8_t  *jpeg;
    int             len;
    int32_t         index;
    uint32_t        gen;
    uint32_t        read_ms;
} vd_pkt_t;

typedef struct {
    char            name[VD_NAME_LEN];
    uint32_t        bytes;
    volatile int    info;       /* 0 not looked at yet, 1 read, -1 not a video it plays */
    uint16_t        w, h;
    uint32_t        ms;
    uint32_t        fps_x100;
    bool            audio;
    uint16_t       *thumb;      /* aos_hal_image_decode's, freed with aos_hal_image_free */
    int             tw, th;
    lv_image_dsc_t  tdsc;
    bool            on_row;     /* the list shows what the worker found */
    lv_obj_t       *img, *meta, *note;
} vd_file_t;

typedef struct {
    /* ---- the card ---- */
    vd_file_t   files[VD_MAX_FILES];
    int         count;
    char        dir[160];

    /* ---- the screen: rebuilt on every resize ---- */
    lv_obj_t   *root;
    int         scr_w, scr_h;
    bool        land;
    lv_obj_t   *list_view;
    lv_obj_t   *play_view;
    lv_obj_t   *frame_img;      /* the LVGL path */
    lv_obj_t   *top_bar, *bottom_bar;
    int         top_h, bottom_h;
    lv_obj_t   *title, *stats;
    lv_obj_t   *slider, *pos_label, *len_label;
    lv_obj_t   *play_icon;
    lv_obj_t   *message;
    lv_timer_t *timer;

    /* ---- what the UI is doing ---- */
    bool        in_player;
    bool        controls;       /* the bars are up */
    uint64_t    controls_until; /* when they go down by themselves */
    bool        hidden;         /* the app is not in front */
    uint32_t    over;           /* aos_ui_overlay(), read in LVGL's timer */
    bool        overlay;        /* ...anything in it but a toast: paused, nothing blitted */
    bool        ov_paused;      /* the overlay paused it: it plays on when that goes */
    bool        reblit;         /* LVGL just drew over the picture: put it back */
    struct {
        uint16_t *fb;           /* one of the panel's buffers, from back() */
        int       x, y, w, h;   /* where its picture is; its letterbox is clear */
    }           fbr[3];
    bool        lvgl_mode;      /* the blit is not there: frames through frame_img */
    int         blit_fails;
    bool        bar_again;      /* re-assert the status bar on the next tick */
    bool        thumbs_done;

    /* ---- playback, UI side ---- */
    int         cur;            /* the file */
    char        path[256];
    char        audio_path[256];
    bool        has_audio;
    bool        playing;        /* a file is open (paused or not) */
    bool        paused;
    bool        ended;
    bool        scrub;          /* the finger is on the seek bar */
    bool        scrub_paused;   /* ...and it was paused before */
    uint64_t    scrub_last;
    bool        show_next;      /* paused: show the next frame of this gen anyway */
    uint64_t    t_anchor;       /* uptime at which the video's t = 0 */
    uint32_t    t_hold;         /* the video's t while paused or waiting */
    bool        audio_wait;     /* held until the sound really moves */
    uint32_t    audio_target;
    uint32_t    audio_p0;
    uint64_t    audio_wait_t0;
    uint64_t    sync_last;
    int         cur_slot;       /* the one on screen, -1 before the first */
    int32_t     shown;          /* its frame number */
    uint32_t    gen;            /* the UI's; the stages see req_gen */

    /* ---- the file, as the worker opened it (read-only for the UI once OPEN_OK) ---- */
    volatile int open_state;
    bool        open_seen;      /* the UI has acted on it */
    uint32_t    rate, scale, frames;
    uint16_t    vw, vh;

    /* ---- UI -> stages ---- */
    volatile uint32_t req_gen;
    volatile uint32_t req_frame;
    volatile uint32_t skip_gen;
    volatile uint32_t skip_to;

    /* ---- the stages ---- */
    vd_avi_t    avi;            /* the worker's and the reader's */
    vd_pkt_t    pkts[VD_PKTS];
    vd_slot_t   slots[VD_SLOTS];
    volatile bool r_stop;
    volatile bool r_alive;
    volatile uint32_t eof_gen;  /* the reader got to the end of this gen */
    int           dst_w, dst_h; /* the size the last frame went up at, for the stats */

    /* ---- measurements: each counter has one writer ---- */
    volatile uint32_t r_skipped, w_skipped, r_errors, w_errors;
    volatile uint32_t r_wait_ms;    /* reader: the ring was full (ahead: fine) */
    volatile uint32_t w_starve_ms;  /* worker: no packet (the card is behind) */
    volatile uint32_t w_wait_ms;    /* worker: no free slot (the screen is behind) */
    uint64_t    stats_t0;
    uint32_t    st_frames, st_read_ms, st_dec_ms, st_blit_us, st_late_ms;
    uint32_t    st_flips, st_direct, st_clears;
    uint32_t    st_starve0, st_wait0;
    uint32_t    log_every;
} vd_t;

/* ---- vd_play.c ---- */
bool     vd_play_start(vd_t *v);            /* the worker opens v->path */
void     vd_play_stop(vd_t *v);             /* joins the stages, frees their memory */
void     vd_play_tick(vd_t *v);             /* LVGL's timer: clock, frames, stats */
void     vd_play_seek(vd_t *v, uint32_t frame, bool audio);
void     vd_play_pause(vd_t *v, bool paused);
bool     vd_play_blit_current(vd_t *v);     /* the frame on screen, again */
uint32_t vd_play_now_ms(vd_t *v);           /* where the video is */
uint32_t vd_frame_ms(const vd_t *v, uint32_t frame);
uint32_t vd_frame_at(const vd_t *v, uint64_t ms);
uint32_t vd_duration_ms(const vd_t *v);

/* ---- vd_list.c ---- */
void vd_list_scan(vd_t *v);
void vd_list_build(vd_t *v);
void vd_list_tick(vd_t *v);                 /* puts what the worker found on the rows */
bool vd_list_thumbs_start(vd_t *v);
void vd_list_free(vd_t *v);
bool vd_audio_path(const vd_t *v, int file, char *out, size_t len);

/* ---- video.c: the player's controls ---- */
void vd_ui_opened(vd_t *v);                 /* the worker has the file */
void vd_ui_message(vd_t *v, const char *text);
void vd_ui_update(vd_t *v);                 /* position, bar, icon */
void vd_ui_controls(vd_t *v, bool up);
void vd_ui_stats(vd_t *v, const char *text);
void vd_ui_open_file(vd_t *v, int index);
void vd_fmt_time(char *out, size_t len, uint32_t ms);
