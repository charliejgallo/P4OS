/*
 * CIELO - the catalogues: the pack <card>/apps/cielo_p4.pak that
 * tools/gen_pack.py builds, and the Milky Way the app makes itself.
 *
 * The pack, little-endian:
 *
 *   "CIELOPK1", u32 sections, then per section { char tag[4]; u32 offset;
 *   u32 length; }. Sections:
 *
 *   STAR  u32 n, then n of 24 bytes, brightest first:
 *         i16 x, y, z     J2000 unit vector * 32767
 *         i16 mag * 100, i16 B-V * 1000 (32767 unknown), u16 HR,
 *         u16 parallax in 0.1 mas (0 unknown), u16 proper name (index into
 *         SNAM, 0xFFFF none), u32 designation (STRS offset),
 *         u8 spectral class letter, u8 0, u16 0
 *   SNAM  u32 n, n x u32 STRS offsets (the names, in Spanish)
 *   CONS  u32 88, then 20 bytes each: char abbr[4]; i16 x, y, z (where the
 *         name goes); u16 first segment; u16 segments; u16 0; u32 Latin
 *         name (STRS offset)
 *   SEGS  u32 n, n x { u16 star, u16 star }
 *   MESS  u32 n, then 16 bytes each: u8 number, u8 type (CL_M_*), i16
 *         mag * 100 (32767 unknown), i16 x, y, z, u16 size in 0.1 arcmin,
 *         u32 NGC/IC (STRS offset)
 *   STRS  NUL-terminated UTF-8 strings
 *
 * Loaded whole (340 KB) into PSRAM and unpacked into floats for the
 * renderer: about 400 KB in all.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { CL_M_OC = 1, CL_M_GB, CL_M_DI, CL_M_PL, CL_M_SP, CL_M_EL, CL_M_IR, CL_M_SNR, CL_M_OTHER };

typedef struct {
    float    *xyz;          /* 3 per star */
    float    *mag;
    uint32_t *rgb;          /* the colour by B-V */
    int16_t  *bv;
    uint16_t *hr, *plx, *name;
    uint32_t *desig;
    uint8_t  *spec;
    int       n;

    uint32_t *names;        /* STRS offsets */
    int       nnames;

    struct cl_cons {
        char     abbr[4];
        float    xyz[3];
        uint16_t first, nseg;
        uint32_t latin;
    } *cons;
    int ncons;

    uint16_t *segs;         /* pairs */
    int       nsegs;

    struct cl_mess {
        uint8_t  m, type;
        float    mag;       /* 99 unknown */
        float    xyz[3];
        float    size;      /* arcmin */
        uint32_t ngc;
    } *mess;
    int nmess;

    char *strs;
    int   nstrs;

    uint8_t *mw;            /* the Milky Way: MW_W x MW_H, galactic l, b */
    bool     ok;
} cl_cat_t;

#define CL_MW_W 512
#define CL_MW_H 256

/* false: no pack (the reason is logged) */
bool cl_cat_load(cl_cat_t *c, const char *path);
void cl_cat_free(cl_cat_t *c);
const char *cl_cat_str(const cl_cat_t *c, uint32_t off);
/* the star's proper name, or NULL */
const char *cl_cat_star_name(const cl_cat_t *c, int i);
/* the constellation a star's designation names ("Ori"), or "" */
void cl_cat_star_cons(const cl_cat_t *c, int i, char out[4]);

/* The Milky Way's brightness map, made from a model (no data): a band
 * along the galactic equator, brighter and wider towards the centre, the
 * star clouds of Sagittarius, Scutum, Cygnus and Carina, the Great Rift and
 * the Coalsack dark, the two Magellanic Clouds, and a fractal mottle. */
bool cl_cat_make_milky_way(cl_cat_t *c);
/* the map at galactic (l, b) radians, 0..255 */
int cl_cat_mw(const cl_cat_t *c, float l, float b);

/* B-V to a star's colour (a black body of the temperature it means) */
uint32_t cl_bv_rgb(float bv);
