/*
 * HOCKEY - the packs (tools/pack_p4.py), Bastion's format: hockey_p4.pak
 * has what every match shares (fonts, sounds) and the bar's table;
 * hockey_<table>_p4.pak one table more each. A table's entries are named
 * "<id>.<what>" (its picture, its two ends drawn again over the puck, its
 * mallets, its puck, its definition), so a pack may hold several. All in
 * /apps.
 *
 *   "BSPK" u16 version u16 count
 *   count x entry (52 bytes):
 *       char name[24]  u8 type  u8 flags  u16 w  u16 h  i16 x  i16 y
 *       u32 off  u32 clen  u32 rawlen  u32 extra  u16 spare
 *   data: one LZ4 block per entry (clen == rawlen: stored as is)
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
    PK_SHEET = 4,       /* frames as runs (hk_gfx.h); extra = frames         */
    PK_JPEG = 5,        /* a JPEG file, w x h                                */
    PK_PCM = 6,         /* mu-law 8-bit mono at 'extra' Hz                   */
    PK_FONT = 7,        /* glyph table + alpha atlas (hk_gfx.h)              */
};

#define HK_NAME_LEN 24
#define HK_PARTS    8

typedef struct {
    char     name[HK_NAME_LEN];
    uint8_t  type, flags;
    uint16_t w, h;
    int16_t  x, y;
    uint32_t off, clen, rawlen, extra;
} hk_entry_t;

typedef struct {
    FILE       *fp[HK_PARTS];
    uint32_t    part_len[HK_PARTS];
    int         parts;
    int         n;
    hk_entry_t *e;
    int         fails;      /* entries that could not be read (no memory)   */
} hk_pak_t;

bool hk_pak_open(hk_pak_t *p, const char *path);
void hk_pak_close(hk_pak_t *p);
const hk_entry_t *hk_pak_find(const hk_pak_t *p, const char *name);
/* the entry unpacked into a new PSRAM buffer (hk_free), NULL on error */
void *hk_pak_load(hk_pak_t *p, const hk_entry_t *e);
void *hk_pak_get(hk_pak_t *p, const char *name, const hk_entry_t **out);

void *hk_malloc(size_t n);          /* PSRAM, or nothing */
void *hk_calloc(size_t n);
void  hk_free(void *p);
size_t hk_mem_used(void);           /* bytes handed out and not freed yet   */
