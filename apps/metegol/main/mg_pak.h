/*
 * METEGOL - the packs (tools/pack_p4.py), Bastion's format: metegol_p4.pak
 * has what every match shares (the figures, the rods, the ball, fonts,
 * sounds) and two stadiums; metegol_<stadium>_p4.pak one stadium more each.
 * A stadium's entries are named "<id>.<what>" (its picture, its two bands
 * over the rods, its crowd, its definition), so a pack may hold several.
 * All in /apps.
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
    PK_SHEET = 4,       /* frames as runs (mg_gfx.h); extra = frames         */
    PK_JPEG = 5,        /* a JPEG file, w x h                                */
    PK_FIGS = 8,        /* the figures' masks (mg_kit.h); extra = frames     */
    PK_PCM = 6,         /* mu-law 8-bit mono at 'extra' Hz                   */
    PK_FONT = 7,        /* glyph table + alpha atlas (mg_gfx.h)              */
};

#define MG_NAME_LEN 24
#define MG_PARTS    8

typedef struct {
    char     name[MG_NAME_LEN];
    uint8_t  type, flags;
    uint16_t w, h;
    int16_t  x, y;
    uint32_t off, clen, rawlen, extra;
} mg_entry_t;

typedef struct {
    FILE       *fp[MG_PARTS];
    uint32_t    part_len[MG_PARTS];
    int         parts;
    int         n;
    mg_entry_t *e;
    int         fails;      /* entries that could not be read (no memory)   */
} mg_pak_t;

bool mg_pak_open(mg_pak_t *p, const char *path);
void mg_pak_close(mg_pak_t *p);
const mg_entry_t *mg_pak_find(const mg_pak_t *p, const char *name);
/* the entry unpacked into a new PSRAM buffer (mg_free), NULL on error */
void *mg_pak_load(mg_pak_t *p, const mg_entry_t *e);
void *mg_pak_get(mg_pak_t *p, const char *name, const mg_entry_t **out);

void *mg_malloc(size_t n);          /* PSRAM, or nothing */
void *mg_calloc(size_t n);
void  mg_free(void *p);
size_t mg_mem_used(void);           /* bytes handed out and not freed yet   */
