/*
 * P4OS - the HD icon pack (aos_icon_pack.c; plan in docs/plan/ICONOS.md).
 *
 * <icons>/hd.pak on the card (aos_hal_path_icons()) holds pictures of the
 * apps' icons drawn on a computer, each at the sizes the system shows: 120
 * px (the home screen upright), 108 (lying down) and 26 (a folder's
 * miniatures), stored as QOI. aos_icon_create() asks here first and draws
 * the app's AIC or glyph only when the pack has no picture for it, so a
 * card without the pack, or an app the pack does not know, looks as before.
 *
 * The file (tools/icons/mkpack.py writes it), little-endian:
 *
 *   "AICP"  u16 version (1)  u16 count  u32 0  u32 0         16 bytes
 *   count entries, sorted by id and then size:               48 bytes each
 *       char id[36] (NUL-padded)  u16 size  u16 0  u32 off  u32 len
 *   the pictures: QOI, RGBA, size x size, at off from the file's start
 *
 * The file is not kept open (the Store can replace it at any time): the
 * index is read into PSRAM and read again when the file's size or date
 * change. Each icon on screen owns its decoded picture, ARGB8888 in PSRAM,
 * freed with the object; nothing is cached, so the memory is what is shown.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AOS_ICON_PACK_ID_MAX 36

/* Reads the pack's index if the file is there and changed since the last
 * look; false when there is no pack (or HD icons are turned off). Called by
 * the functions below, at most every two seconds. */
bool aos_icon_pack_refresh(void);

/* Whether the pack has a picture for the app 'id'. */
bool aos_icon_pack_has(const char *id);

/* An image object of size x size with the app's picture (the pack's nearest
 * size at or above it, scaled down if it is not exact), or NULL when the pack
 * has none for it. The decoded picture is freed with the object. */
lv_obj_t *aos_icon_pack_create(lv_obj_t *parent, const char *id, int32_t size);

/* HD icons on or off (the preference "icon_hd", on by default). */
bool aos_icon_pack_enabled(void);
void aos_icon_pack_set_enabled(bool on);

#ifdef __cplusplus
}
#endif
