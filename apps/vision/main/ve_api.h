/*
 * P4OS - Vision: what the app asks of its engine (engine/ve_engine.cpp).
 *
 * On the board the engine is a module of its own, apps/vision/engine.so on
 * the card, opened with aos_hal_module_open() while a detector is chosen:
 * esp-dl (C++, ~1 MB of code) costs nothing while the app only reads codes.
 * In the simulator it is linked into the app, with esp-dl's C kernels.
 *
 * Plain C on this side: the module exports ve_module_api(), which returns
 * this table. The models are the .espdl files of the packs, read whole by
 * the app into PSRAM (16-byte aligned) and kept by it until close(): the
 * engine runs on them in place.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define VE_ABI 1

/* What a network finds. */
typedef enum {
    VE_K_FACE = 0,   /* faces, two models: MSR proposes, MNP confirms (5 landmarks)   */
    VE_K_PERSON,     /* people, Pico                                                  */
    VE_K_COCO,       /* the 80 COCO objects, YOLO11n at 320 x 320                     */
    VE_K_HAND,       /* hands (ESPDet); with a second model, each hand's gesture      */
    VE_K_COUNT
} ve_kind_t;

/* The gestures of the hand model, in its order (esp-dl's names in comments). */
typedef enum {
    VE_G_ONE = 0,     /* one        */
    VE_G_TWO,         /* two        */
    VE_G_THREE,       /* three      */
    VE_G_FOUR,        /* four       */
    VE_G_FIVE,        /* five       */
    VE_G_LIKE,        /* like       */
    VE_G_OK,          /* ok         */
    VE_G_NONE,        /* no_gesture */
    VE_G_CALL,        /* call       */
    VE_G_DISLIKE,     /* dislike    */
    VE_G_NO_HAND,     /* no_hand    */
    VE_G_COUNT
} ve_gesture_t;

typedef struct {
    int16_t x0, y0, x1, y1;   /* in the image's pixels, inclusive              */
    int16_t cls;              /* COCO class; the gesture for a hand; else 0   */
    int16_t nkp;              /* landmarks in kp (faces: 5, eyes nose mouth)   */
    float score;              /* 0..1                                          */
    float cls_score;          /* the gesture's score, 0..1                     */
    int16_t kp[10];           /* x, y of each landmark                         */
} ve_box_t;

typedef struct {
    uint32_t pre_us, model_us, post_us;   /* the last run, each stage summed over its models */
} ve_times_t;

typedef struct ve_net ve_net_t;

typedef struct {
    int abi;   /* VE_ABI */
    /* blobs: the model files ("EDL2..."); VE_K_FACE takes two (MSR, MNP),
     * VE_K_HAND one or two (the detector, the gesture classifier). NULL on
     * failure, with the reason in err (English: for the log). */
    ve_net_t *(*open)(ve_kind_t kind, const uint8_t *const *blobs, const uint32_t *lens, int nblobs, char *err,
                      size_t errlen);
    void (*close)(ve_net_t *net);
    /* One image, RGB565 in LVGL's byte order, rows stride_px apart. Up to max
     * boxes, best first; the count, or -1 on error. */
    int (*run)(ve_net_t *net, const uint16_t *px, int w, int h, int stride_px, ve_box_t *out, int max,
               ve_times_t *times);
    /* The detector's threshold, 0..1 (each kind starts at esp-dl's own). */
    void (*set_threshold)(ve_net_t *net, float score);
    float (*threshold)(ve_net_t *net);
} ve_api_t;

#ifdef __cplusplus
extern "C" {
#endif
const ve_api_t *ve_module_api(void);
#ifdef __cplusplus
}
#endif
