/*
 * P4OS - Música's covers (aos_app_music_cover.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "aos_app_image.h"

#define MUSIC_THUMB_PX   88             /* the mini player's and the rows' art */
#define MUSIC_TAG_COVER  0x40000000u    /* loader tags of the cover: this bit + a generation */

/* Asks for the cover of 'track' as a px x px square: the picture inside the
 * file when cover_size is not 0, else a cover/folder/front.jpg beside it
 * (or in its first subfolder). Returns at once; asking again for the same
 * picture at the same size decodes nothing. */
void music_cover_request(const char *track, uint32_t cover_offset, uint32_t cover_size, int px);

/* Hands a result of the loader to the cover. true if it was the cover's
 * (and so taken), false if it belongs to someone else. */
bool music_cover_offer(img_result_t *r);

/* true once the cover changed since the last call: *big (px square),
 * *thumb (MUSIC_THUMB_PX square) and *avg (0xRRGGBB) describe the new one,
 * NULL / 0 when there is none (draw the note). The previous pixels are freed
 * on the NEXT call, once the screen has the new ones. */
bool music_cover_take(const lv_image_dsc_t **big, const lv_image_dsc_t **thumb, uint32_t *avg);

/* What is on show right now, for a screen that is being built. */
void music_cover_current(const lv_image_dsc_t **big, const lv_image_dsc_t **thumb, uint32_t *avg);

/* The app is closing: whatever was asked for is dropped with its jobs, so
 * the next request asks again. The cover on show is kept for next time. */
void music_cover_abandon(void);

/* The picture file of a folder: cover.jpg, folder.jpg or front.jpg (any
 * case, .jpg/.jpeg/.png), in 'dir' or else in its first subfolder. */
bool music_folder_cover(const char *dir, char *out, size_t len);
