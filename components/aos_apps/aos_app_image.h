/*
 * P4OS - Pictures for the built-in apps (Música's covers, Fotos).
 *
 * Two layers:
 *
 *   aos_hal_image_decode()  the HAL's decoder: a JPEG (or PNG) from a file,
 *                           or from a range of one (the picture inside an
 *                           MP3), to RGB565 at the size asked for. Slow by
 *                           LVGL's measure: never call it from LVGL's task.
 *
 *   img_request/img_take    a loader that runs it on a thread of its own,
 *                           with a queue, cancelling, and thumbnails kept on
 *                           the card so a folder decoded once opens at once.
 *
 * The decoder is the HAL's (aos_hal.h): libavcodec in the simulator, the
 * P4's JPEG engine and software paths on the board (aos_image_p4.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aos_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The decoder itself is the HAL's: aos_hal_image_decode() in aos_hal.h. */

/* ---- the loader ------------------------------------------------------------ */

/* Who asked: each app drains and cancels only its own. */
enum { IMG_OWNER_MUSIC = 1, IMG_OWNER_PHOTOS = 2 };

typedef struct {
    const char *path;
    uint32_t    offset, size;   /* 0, 0: the whole file */
    int         w, h;
    bool        fill;
    bool        urgent;         /* before every non-urgent one; else LIFO */
    const char *cache_dir;      /* keep the result there too (NULL: don't) */
    bool        id3;            /* 'path' is an MP3: its ID3 tag's picture */
} img_job_t;

typedef struct {
    uintptr_t tag;              /* the caller's, as given to img_request() */
    uint16_t *px;               /* NULL: nothing could be decoded */
    int       w, h;             /* of px */
    int       src_w, src_h;     /* of the picture */
    uint8_t   kind_;            /* how to free it: img_free() knows */
} img_result_t;

/* Queues a decode. false if the queue is full (ask again later). */
bool img_request(int owner, uintptr_t tag, const img_job_t *job);

/* One finished job of 'owner', if there is one. The pixels are the
 * caller's from here: img_free() them once off the screen. */
bool img_take(int owner, img_result_t *out);

/* Forgets the queued (not yet started) jobs with this tag. */
void img_cancel(int owner, uintptr_t tag);

/* Forgets everything of 'owner': queued, running and finished. Does not
 * wait: whatever is running is freed by the loader when it ends. */
void img_cancel_all(int owner);

void img_free(img_result_t *r);

/* Names the way a person sorts them: case ignored, digit runs as numbers
 * ("IMG_2" before "IMG_10"). */
int  img_name_cmp(const char *a, const char *b);

/* A file name made fit to draw: what a Mac writes decomposed (a vowel and a
 * combining accent, "i\xCC\x81") composed back ("\xC3\xAD", which Inter
 * has), then through aos_text_safe(). */
size_t img_text(char *out, size_t len, const char *in);

/* A thumbnail downscaled from a bigger picture, averaging (box filter), into
 * a fresh buffer of dw x dh that the caller free()s. */
uint16_t *img_downscale(const uint16_t *src, int sw, int sh, int dw, int dh);

/* The average colour of a picture, 0xRRGGBB, for tinting what is around it. */
uint32_t img_average(const uint16_t *px, int w, int h);

#ifdef __cplusplus
}
#endif
