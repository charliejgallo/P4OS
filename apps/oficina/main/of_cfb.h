/*
 * P4OS - Office: Compound File Binary (OLE2) reader (of_cfb.c), and the
 * readers of the old binaries on top of it: Word 97-2003 (of_doc97.c),
 * Excel 97-2003 (of_xls.c), PowerPoint 97-2003 (of_ppt.c).
 */
#pragma once

#include "of.h"

typedef struct {
    char     name[64];
    uint8_t  type;              /* 1 storage, 2 stream, 5 root */
    uint32_t left, right, child, start;
    uint64_t size;
} of_cfb_ent_t;

typedef struct {
    const uint8_t *buf;
    size_t         len;
    uint32_t       ssz, mssz, cutoff;
    uint32_t      *fat, nfat;
    uint32_t      *mfat, nmfat;
    uint8_t       *mini;
    size_t         mini_len;
    of_cfb_ent_t  *e;
    uint32_t       ne;
} of_cfb_t;

bool     of_cfb_is(const uint8_t *buf, size_t len);
bool     of_cfb_open(of_cfb_t *c, const uint8_t *buf, size_t len);
void     of_cfb_close(of_cfb_t *c);
int      of_cfb_find(const of_cfb_t *c, const char *name);     /* a stream under the root, -1 */
uint8_t *of_cfb_read(const of_cfb_t *c, int idx, size_t *len);  /* NUL-terminated */

struct of_doc_s;
struct of_deck_s;
/* err: "encrypted", "old" (Word 6/95, Excel 5/95), "format" */
struct of_doc_s  *of_doc97_load(const char *path, char *err, size_t errlen);
of_book_t        *of_xls_load(const char *path, char *err, size_t errlen);
struct of_deck_s *of_ppt_load(const char *path, char *err, size_t errlen);
