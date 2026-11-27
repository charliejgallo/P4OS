/*
 * P4OS - Vision: a USB webcam with on-board vision. esp-dl's detectors
 * (faces, people, the 80 COCO objects, hands and their gestures) on the
 * P4's SIMD, and QR and bar codes read from the picture.
 *
 *   vision.c     the app: the screen, the modes, the portal's page
 *   vi_run.c     the worker: a frame, decoded, through the mode, published
 *   vi_src.c     where frames come from: the webcam, or test pictures
 *   vi_pack.c    the model packs and the engine module (main/ve_api.h)
 *   vi_codes.c   QR codes (quirc) and EAN/UPC bar codes
 *   vi_names.c   what each class and gesture is called
 *
 * The worker owns the camera, the decoder, the engine and the frame it
 * fills; the screen takes the newest finished frame under the lock. Text is
 * made on LVGL's side only: _() translates with the app in front.
 */
#pragma once

#include "aos_hal.h"
#include "lvgl.h"
#include "ve_api.h"

#include <stdbool.h>
#include <stdint.h>

#define VI_ID "aos.vision"

/* What the picture goes through. */
typedef enum {
    VI_M_FACE = 0,
    VI_M_PERSON,
    VI_M_COCO,
    VI_M_HAND,
    VI_M_CODES,
    VI_M_COUNT
} vi_mode_t;

#define VI_MAX_BOXES 24
#define VI_MAX_CODES 4
#define VI_CODE_LEN  256

/* The largest frame shown and run: the camera is asked for this or less. */
#define VI_W 640
#define VI_H 480

typedef enum {
    VI_CODE_QR = 0,
    VI_CODE_EAN13,
    VI_CODE_EAN8,
    VI_CODE_UPCA,
} vi_code_kind_t;

typedef struct {
    vi_code_kind_t kind;
    int16_t x0, y0, x1, y1;   /* where it is in the frame (a bar code: its row) */
    char text[VI_CODE_LEN];
} vi_code_t;

/* What the worker is doing, for the screen. */
typedef enum {
    VI_ST_IDLE = 0,
    VI_ST_NO_SOURCE,      /* no camera and no test pictures                  */
    VI_ST_WAITING,        /* the camera started, no frame yet                */
    VI_ST_LOADING,        /* reading a pack, building the models             */
    VI_ST_NO_PACK,        /* the mode's pack is not on the card              */
    VI_ST_NO_ENGINE,      /* engine.so missing or refused                    */
    VI_ST_MODEL_FAILED,   /* the models did not build                        */
    VI_ST_RUNNING,
} vi_state_t;

typedef enum {
    VI_SRC_NONE = 0,
    VI_SRC_CAMERA,
    VI_SRC_TEST,
} vi_src_kind_t;

/* One published frame: the picture and what was found in it. */
typedef struct {
    uint16_t *px;            /* RGB565, LVGL's order, w x h packed           */
    int w, h;
    uint32_t seq;
    vi_mode_t mode;
    int nbox;
    ve_box_t box[VI_MAX_BOXES];
    int ncode;
    vi_code_t code[VI_MAX_CODES];
    uint32_t run_us;         /* the mode's work on this frame               */
    ve_times_t t;            /* the engine's stages                          */
} vi_frame_t;

/* Shared between the worker and the screen (under vi.mx). */
typedef struct {
    void *mx;
    volatile bool stop;
    volatile bool running;          /* the worker is alive                    */
    volatile vi_mode_t want;        /* asked for by the screen or the portal  */
    volatile float want_thr;        /* 0: the model's own                     */
    volatile bool want_photo;       /* save the next frame                    */
    volatile bool want_test;        /* prefer the test pictures to the camera */

    vi_state_t state;
    vi_mode_t mode;                 /* what runs                              */
    vi_src_kind_t src;
    char src_name[48];              /* the camera's name, or the test file   */
    char err[96];                   /* the last failure, English, for the log */

    vi_frame_t fr[3];               /* triple buffer: shown, ready, filling   */
    int shown, ready;               /* indexes, -1 none                       */

    float fps;                      /* frames through the mode, per second    */
    float thr;                      /* the detector's threshold now           */
    uint32_t photos;                /* saved so far (the screen says so)      */
    char photo_path[160];
    bool photo_ok;
    uint32_t photo_seq;
    bool live;                      /* the portal's page is looking           */
} vi_shared_t;

extern vi_shared_t vi;

/* vi_run.c */
bool vi_run_start(void);
void vi_run_stop(void);

/* vi_src.c: a frame's JPEG, newest first; 1 got one, 0 none yet, -1 gone */
bool vi_src_open(bool prefer_test);
int  vi_src_next(const uint8_t **jpeg, int *len);
void vi_src_close(void);
vi_src_kind_t vi_src_kind(void);
const char *vi_src_name(void);
const char *vi_test_dir(void);

/* vi_pack.c; vi_alloc: 16-byte aligned PSRAM (malloc in the simulator) */
void *vi_alloc(size_t n);
void vi_free(void *p);
const ve_api_t *vi_engine(char *err, size_t errlen);
void vi_engine_close(void);
const char *vi_mode_pack(vi_mode_t m);          /* the pack file, NULL: none needed */
bool vi_pack_present(vi_mode_t m);
/* the mode's models, read whole into 16-byte aligned PSRAM; vi_models_free() */
int  vi_models_load(vi_mode_t m, const uint8_t **blobs, uint32_t *lens, int max, char *err, size_t errlen);
void vi_models_free(const uint8_t **blobs, int n);

/* vi_codes.c: the codes in a frame (RGB565), up to max */
bool vi_codes_init(int w, int h);
void vi_codes_free(void);
int  vi_codes_scan(const uint16_t *px, int w, int h, vi_code_t *out, int max);

/* vi_names.c: translated on LVGL's side */
const char *vi_coco_name(int cls);
const char *vi_gesture_name(int g);
const char *vi_mode_name(vi_mode_t m);
const char *vi_mode_noun(vi_mode_t m, int n);   /* "3 caras", for the count */
lv_color_t  vi_class_color(vi_mode_t m, int cls);
