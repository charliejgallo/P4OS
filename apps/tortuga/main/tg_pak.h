/*
 * Tortuga - the pack (tools/pack_p4.py), Bastion's format and reader, as
 * Juegos de mesa has it: tortuga_p4.pak holds the turtle's sheets and the
 * board's tiles, in /apps.
 *
 *   "BSPK" u16 version u16 count
 *   count x entry (52 bytes):
 *       char name[24]  u8 type  u8 flags  u16 w  u16 h  i16 x  i16 y
 *       u32 off  u32 clen  u32 rawlen  u32 extra  u16 spare
 *   data: one LZ4 block per entry (clen == rawlen: stored as is)
 *
 * A sheet (PK_SHEET) is every frame of one model, each cropped to what is
 * not transparent and kept as runs, so the transparent pixels cost neither
 * memory nor time (tg_gfx.h has the layout).
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
    PK_FONT = 7,        /* glyph table + alpha atlas (unused here)           */
    PK_JPEG = 8,        /* a JPEG file as is                                 */
};

#define TGX_NAME_LEN 24
#define TGX_PARTS    8

typedef struct {
    char     name[TGX_NAME_LEN];
    uint8_t  type, flags;
    uint16_t w, h;
    int16_t  x, y;
    uint32_t off, clen, rawlen, extra;
} tg_entry_t;

typedef struct {
    FILE       *fp[TGX_PARTS];
    uint32_t    part_len[TGX_PARTS];
    int         parts;
    int         n;
    tg_entry_t *e;
    int         fails;      /* entries that could not be read (no memory)   */
} tg_pak_t;

bool tg_pak_open(tg_pak_t *p, const char *path);
void tg_pak_close(tg_pak_t *p);
const tg_entry_t *tg_pak_find(const tg_pak_t *p, const char *name);
/* the entry unpacked into a new PSRAM buffer (tg_free), NULL on error */
void *tg_pak_load(tg_pak_t *p, const tg_entry_t *e);
void *tg_pak_get(tg_pak_t *p, const char *name, const tg_entry_t **out);

void *tg_alloc(size_t n);          /* PSRAM, or nothing */
void *tg_malloc(size_t n);         /* the same, zeroed */
void  tg_free(void *p);
size_t tg_mem_used(void);           /* bytes handed out and not freed yet   */
