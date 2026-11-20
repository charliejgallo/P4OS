/*
 * BASTION - the packs (tools/pack_p4.py): bastion_p4.pak has what every
 * battle shares (towers, aliens, the fort, effects, fonts, sounds) and
 * bastion_<world>_p4.pak one world each (its picture, the depth of its
 * pixels, its definition). Both in /apps, the first maybe in parts
 * (.pak, .pak.1 ...) since it is over 8 MB.
 *
 *   "BSPK" u16 version u16 count
 *   count x entry (52 bytes):
 *       char name[24]  u8 type  u8 flags  u16 w  u16 h  i16 x  i16 y
 *       u32 off  u32 clen  u32 rawlen  u32 extra  u16 spare
 *   data: one LZ4 block per entry (clen == rawlen: stored as is)
 *
 * A sheet (PK_SHEET) is every frame of one model, each cropped to what is
 * not transparent and kept as runs, so the transparent pixels cost neither
 * memory nor time (bs_gfx.h has the layout).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
    PK_TEXT = 0,        /* bytes (a definition)                              */
    PK_RGB565 = 1,      /* w*h RGB565, opaque                                */
    PK_SPRITE = 2,      /* w*h RGB565 then w*h alpha (LVGL's RGB565A8)       */
    PK_DEPTH = 3,       /* w*h u16: (0.6 Y - 0.8 Z + 16) * 1024              */
    PK_SHEET = 4,       /* frames as runs; extra = frames                    */
    PK_PCM = 6,         /* mu-law 8-bit mono at 'extra' Hz                   */
    PK_FONT = 7,        /* glyph table + alpha atlas (bs_gfx.h)              */
};

#define BS_NAME_LEN 24
#define BS_PARTS    8

typedef struct {
    char     name[BS_NAME_LEN];
    uint8_t  type, flags;
    uint16_t w, h;
    int16_t  x, y;
    uint32_t off, clen, rawlen, extra;
} bs_entry_t;

typedef struct {
    FILE       *fp[BS_PARTS];
    uint32_t    part_len[BS_PARTS];
    int         parts;
    int         n;
    bs_entry_t *e;
    int         fails;      /* entries that could not be read (no memory)   */
} bs_pak_t;

bool bs_pak_open(bs_pak_t *p, const char *path);
void bs_pak_close(bs_pak_t *p);
const bs_entry_t *bs_pak_find(const bs_pak_t *p, const char *name);
/* the entry unpacked into a new PSRAM buffer (bs_free), NULL on error */
void *bs_pak_load(bs_pak_t *p, const bs_entry_t *e);
void *bs_pak_get(bs_pak_t *p, const char *name, const bs_entry_t **out);

void *bs_malloc(size_t n);          /* PSRAM, or nothing */
void *bs_calloc(size_t n);
void  bs_free(void *p);
size_t bs_mem_used(void);           /* bytes handed out and not freed yet   */
