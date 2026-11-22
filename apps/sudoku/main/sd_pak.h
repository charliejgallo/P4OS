/*
 * SUDOKU - the pack (tools/pack_p4.py): sudoku_p4.pak in /apps holds every
 * theme's art and the sounds.
 *
 *   "SDPK" u16 version u16 count
 *   count x entry (52 bytes):
 *       char name[24]  u8 type  u8 flags  u16 w  u16 h  i16 x  i16 y
 *       u32 off  u32 clen  u32 rawlen  u32 extra  u16 spare
 *   data: one LZ4 block per entry (clen == rawlen: stored as is)
 *
 * A JPEG is stored as is and never read through here: the HAL decodes it
 * straight from the file (aos_hal_image_decode with its offset and size),
 * which is why the pack is one file and not parts.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
    SDPK_TEXT = 0,
    SDPK_SPRITE = 2,    /* w*h RGB565 then w*h alpha (LVGL's RGB565A8); extra = frames, one after another */
    SDPK_PCM = 6,       /* mu-law 8-bit mono at 'extra' Hz */
    SDPK_JPEG = 8,      /* w x h, baseline */
};

#define SDPK_NAME_LEN 24

typedef struct {
    char     name[SDPK_NAME_LEN];
    uint8_t  type, flags;
    uint16_t w, h;
    int16_t  x, y;
    uint32_t off, clen, rawlen, extra;
} sd_pak_entry_t;

typedef struct {
    FILE           *fp;
    char            path[160];
    int             n;
    sd_pak_entry_t *e;
} sd_pak_t;

bool sd_pak_open(sd_pak_t *p, const char *path);
void sd_pak_close(sd_pak_t *p);
const sd_pak_entry_t *sd_pak_find(const sd_pak_t *p, const char *name);
/* the entry unpacked into a new PSRAM buffer (sd_free), NULL on error */
void *sd_pak_load(sd_pak_t *p, const sd_pak_entry_t *e);

void *sd_alloc(size_t n);       /* PSRAM, or nothing */
void  sd_free(void *p);
