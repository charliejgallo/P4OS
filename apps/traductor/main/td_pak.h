/*
 * TRADUCTOR - the packs: one a language pair (traductor_es_en_p4.pak) with
 * what the app can do offline for it: the neural model of each direction,
 * the dictionaries and the phrasebook (tools/mkpack.py writes them).
 *
 *   "TRPK" u16 version u16 count
 *   count x entry (48 bytes):
 *       char name[24]  u32 type  u32 off  u32 len  u16 rows  u16 cols
 *       f32 scale  f32 scale_a
 *   data: each entry 16-byte aligned, stored as is
 *
 * One logical file in parts of 7.5 MB (.pak, .pak.1 ...), like Bastion's.
 * Plain C and stdio only: the same code runs in the app and in the bench on
 * the Mac (tools/bench_nmt.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
    TD_T_BYTES = 0,
    TD_T_MAT8 = 1,      /* rows x cols int8, value = q * scale; scale_a: Marian's QuantMultA */
    TD_T_VECF = 2,      /* cols floats                                       */
    TD_T_SPM = 3,       /* SentencePiece vocabulary (td_spm.c)               */
    TD_T_LEX = 4,       /* the shortlist of each source word (td_nmt.c)      */
    TD_T_DICT = 5,      /* a dictionary, one direction (td_dict.c)           */
    TD_T_PHRASE = 6,    /* the phrasebook (td_dict.c)                        */
    TD_T_INFO = 7,      /* text: key=value lines                             */
};

#define TD_NAME_LEN 24
#define TD_PARTS    16

typedef struct {
    char     name[TD_NAME_LEN];
    uint32_t type, off, len;
    uint16_t rows, cols;
    float    scale, scale_a;
} td_entry_t;

typedef struct {
    FILE       *fp[TD_PARTS];
    uint32_t    part_len[TD_PARTS];
    int         parts;
    int         n;
    td_entry_t *e;
} td_pak_t;

bool td_pak_open(td_pak_t *p, const char *path);
void td_pak_close(td_pak_t *p);
const td_entry_t *td_pak_find(const td_pak_t *p, const char *name);
/* len bytes at the logical offset off, across parts */
bool td_pak_read(td_pak_t *p, uint32_t off, void *dst, uint32_t len);
/* the entry into a new buffer (td_free), NULL on error */
void *td_pak_load(td_pak_t *p, const td_entry_t *e);

/* big buffers: PSRAM on the board, malloc elsewhere; counted */
void *td_malloc(size_t n);
void  td_free(void *p);
size_t td_mem_used(void);
