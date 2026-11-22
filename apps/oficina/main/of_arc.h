/*
 * P4OS - Office: archives (of_archive.c), without LVGL.
 */
#pragma once

#include "of.h"

enum { OF_ARC_ZIP = 1, OF_ARC_TAR, OF_ARC_TGZ, OF_ARC_GZ };

typedef struct {
    char    *name;              /* path inside, UTF-8, '/' separated, no trailing '/' */
    uint64_t size;
    uint64_t off;               /* TAR: where the data starts (in the file or the inflated buffer) */
    int      idx;               /* ZIP: entry index */
    bool     dir;
    bool     locked;            /* encrypted */
    uint32_t mtime;
} of_aent_t;

typedef struct {
    int        kind;            /* OF_ARC_* */
    char       path[300];
    of_aent_t *e;
    int        n;
    uint8_t   *buf;             /* ZIP: the file; TGZ, GZ: inflated */
    size_t     len;
    of_zip_t   zip;
} of_arc_t;

of_arc_t *of_arc_open(const char *path, char *err, size_t errlen);   /* err: "big", "damaged", "format" */
bool      of_arc_extract(of_arc_t *a, int i, const char *out, char *err, size_t errlen);
void      of_arc_free(of_arc_t *a);
bool      of_is_arc_ext(const char *ext);
