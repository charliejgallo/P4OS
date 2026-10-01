/*
 * MILA - a frame: the cache copied in, the moving things on top
 *
 * The worker builds a draw list per frame (ml_draw_t: a sprite at an LP
 * anchor with its depth), sorts it far to near, and renders the screen in
 * bands of rows in internal RAM: each band starts as a copy of the cache and
 * then every sprite that crosses it is drawn, depth-tested per pixel
 * against the cache. Sprites do not write depth: between moving things the
 * sort decides.
 */
#pragma once

#include "ml_art.h"
#include "ml_gfx.h"
#include "ml_world.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    DR_XRAY  = 1 << 0,      /* what is hidden shows as a faint silhouette   */
    DR_NOZ   = 1 << 1,      /* not depth-tested (effects above everything)  */
    DR_ADD   = 1 << 2,      /* COL added instead of blended (sparkles)       */
};

typedef struct {
    const ml_spr_t *s;
    const ml_lut_t *lut;        /* LID sprites                              */
    int16_t  x, y;              /* LP of the anchor                          */
    int16_t  d;                 /* depth of the anchor                       */
    uint8_t  fmt;               /* ML_PX_*                                   */
    uint8_t  alpha;             /* 0..255                                    */
    uint8_t  flags;             /* DR_*                                      */
    int8_t   prio;              /* order between equal depths (higher later) */
    uint16_t xray;              /* the silhouette's colour                   */
} ml_draw_t;

#define ML_MAX_DRAW 192

typedef struct {
    ml_draw_t d[ML_MAX_DRAW];
    int n;
} ml_dlist_t;

void ml_dlist_clear(ml_dlist_t *l);
ml_draw_t *ml_dlist_add(ml_dlist_t *l);
void ml_dlist_sort(ml_dlist_t *l);

/* rows y0..y1 of the screen, over the image's clip columns (the band's
 * image clips to them) */
void ml_render_band(const ml_world_t *w, ml_img_t *im, int cam_x, int cam_y, int y0, int y1,
                    const ml_dlist_t *l);

/* ---- what changed on the screen (P4OS) ----
 *
 * A puzzle stands still most of the time: a frame only redraws, and the
 * timer only pushes, the tiles of ML_TILE x ML_TILE screen pixels that
 * changed. A scene says what changed: the draw list against the last one
 * (ml_dlist_damage), the cache's blocks drawn again, its HUD's boxes; a
 * camera that moved, a mode that changed, or a turned screen is "full". */
#define ML_TILE     32
#define ML_TMAX     ((ML_WMAX + ML_TILE - 1) / ML_TILE)

typedef struct ml_dmg {
    uint64_t row[ML_TMAX];          /* a bit per tile column, per tile row  */
    bool     full;
    bool     any;
} ml_dmg_t;

/* the lowest and the highest bit set (m != 0), and how many: loops, not
 * __builtin_ctzll & co, which on the 32-bit RISC-V are libgcc calls the
 * firmware does not export */
static inline int ml_bit_lo(uint64_t m) { int i = 0; while (!(m & 1)) { m >>= 1; i++; } return i; }
static inline int ml_bit_hi(uint64_t m) { int i = -1; while (m) { m >>= 1; i++; } return i; }
static inline int ml_bit_count(uint64_t m) { int n = 0; while (m) { m &= m - 1; n++; } return n; }

void ml_dmg_clear(ml_dmg_t *d);
void ml_dmg_full(ml_dmg_t *d);
void ml_dmg_rect(ml_dmg_t *d, int x, int y, int w, int h);
void ml_dmg_or(ml_dmg_t *d, const ml_dmg_t *o);
static inline bool ml_dmg_empty(const ml_dmg_t *d) { return !d->full && !d->any; }
/* the columns that changed in the rows y0..y1, false if none */
bool ml_dmg_span(const ml_dmg_t *d, int y0, int y1, int *x0, int *x1);
/* tiles marked, out of all the screen's (for the logs) */
int  ml_dmg_count(const ml_dmg_t *d, int *of);
/* what moved between two draw lists seen from the same camera: the boxes
 * of every item that is in one and not the same in the other */
void ml_dlist_damage(const ml_dlist_t *prev, const ml_dlist_t *cur, int cam_x, int cam_y, ml_dmg_t *d);
