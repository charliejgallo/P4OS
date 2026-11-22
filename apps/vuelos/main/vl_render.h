/*
 * VUELOS - a view of the world into a buffer: which tiles, the style, the
 * labels.
 *
 * P4OS: the buffer may be at half the screen's resolution (res 0.5), the
 * quick render shown enlarged while the map moves. Everything that is a
 * size on the screen (the tiles, line widths, label spacing) is scaled by
 * res; everything that is a choice of the style (what shows at which zoom)
 * follows the view's zoom as it is, so the quick render is the same map,
 * coarser, and not the map of one level out.
 *
 * Positions are Web Mercator normalised to 32 bits: x = 0 at 180°W, 2^32 at
 * 180°E; y = 0 at the top (85°N), 2^32 at the bottom. A uint32 carries a
 * point to a centimetre, where a float would jitter by metres at street
 * zoom; the renderer only ever works with differences, which fit a float.
 *
 * Zoom follows the usual 256-pixel convention (z 0 = the world in 256 px);
 * the data comes from tiles one level below (dz = z - 1), because
 * OpenMapTiles tiles are designed to be seen at 512 px, and never deeper than
 * 14, the deepest the service has: past z 15 the same z14 tile is enlarged,
 * which vectors do cleanly.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "vl_draw.h"

#define VL_ZMIN     2.0f
#define VL_ZMAX     18.5f
#define VL_DZ_MAX   14

typedef struct {
    uint32_t cx, cy;
    float    z;
} vl_view_t;

typedef struct {
    uint8_t  z;
    uint32_t x, y;
} vl_key_t;

#define VL_MAX_MISSING 32

typedef struct {
    int      missing;               /* tiles not on the board */
    vl_key_t miss[VL_MAX_MISSING];  /* nearest to the centre first */
    int      shown;                 /* tiles drawn from their own data */
    int      stand_in;              /* tiles drawn from an enlarged ancestor */
    uint32_t us_tiles;              /* finding them: RAM, packs, card */
    uint32_t us_geom, us_labels;    /* for the log */
    uint32_t us_ldraw;              /* of us_labels, drawing the glyphs */
    uint32_t us_cls[32];            /* geometry time per class */
    uint32_t us_total;
    uint32_t pts_in, pts_out;       /* points before and after simplifying */
    int      labels;
} vl_render_stats_t;

enum { VL_FONT_S = 0, VL_FONT_M, VL_FONT_L, VL_FONTS };

#define VL_BG 0x0A0C10              /* the land: nearly black, a muted map for the aircraft */

/* Polled between classes and between labels; true stops the render (the
 * view went somewhere this buffer will not cover). */
typedef bool (*vl_abort_fn)(void *arg);

typedef struct {
    float           res;            /* buffer pixels per screen pixel: 1 or 0.5 */
    const vl_font_t *fonts;         /* VL_FONTS of them, NULL: no labels */
    const int16_t  (*reserve)[4];   /* kept free of labels: x0, y0, x1, y1 in buffer px */
    int             nreserve;
    vl_abort_fn     abort;
    void           *abort_arg;
} vl_render_opts_t;

/* Draws view v into fb (the whole buffer, centred on the view). false if
 * the abort callback stopped it; the buffer is then half drawn. */
bool vl_render(vl_fb_t *fb, const vl_view_t *v, const vl_render_opts_t *o, vl_render_stats_t *st);

/* Degrees <-> normalised Mercator. */
void  vl_lonlat_to_world(float lon, float lat, uint32_t *x, uint32_t *y);
void  vl_world_to_lonlat(uint32_t x, uint32_t y, float *lon, float *lat);
/* Metres per screen pixel at the view's latitude. */
float vl_metres_per_px(const vl_view_t *v);
/* Screen pixels per world unit (2^-32 of the world) at zoom z. */
float vl_px_per_unit(float z);
